// OrganicReverbSubsystem.cpp
#include "Acoustics/OrganicReverbSubsystem.h"
#include "Acoustics/AcousticGridScanner.h"
#include "Acoustics/OrganicReverbVolume.h"
#include "Acoustics/OrganicSoundSourceComponent.h"
#include "AcousticCore/ReverbParameterMapper.h"
#include "Acoustics/AcousticHeatmapVolume.h"
#include "Debug/DebugDrawService.h"
#include "Engine/Canvas.h"
#include "Sound/AudioSettings.h"
#include "Sound/SoundSubmix.h"
#include "DrawDebugHelpers.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Components/AudioComponent.h"
#include "GameFramework/WorldSettings.h"
#include "Kismet/GameplayStatics.h"
#include "Sound/SoundAttenuation.h"
#include "TimerManager.h"
#include "Sound/SoundBase.h"

DEFINE_LOG_CATEGORY(LogOrganicReverb);

namespace OrganicReverbConsole
{
	// 히트맵 밝기와 짙기. 레벨 밝기에 따라 눈에 맞춰 조절한다 (0 = 히트맵 끔)
	static TAutoConsoleVariable<float> CVarHeatmapIntensity(
		TEXT("ors.Heatmap.Intensity"), 1.f,
		TEXT("Brightness of the acoustic energy heatmap overlay (0 = off)"));

	static TAutoConsoleVariable<int32> CVarDebug(
		TEXT("ors.Debug"), -1,
		TEXT("Organic Reverb debug heatmap. -1 = volume setting, 0 = off, 1 = on"));

	static UOrganicReverbSubsystem* FindSubsystem(UWorld* World)
	{
		return World ? World->GetSubsystem<UOrganicReverbSubsystem>() : nullptr;
	}

	static FAutoConsoleCommandWithWorldAndArgs EmitCommand(
		TEXT("ors.Emit"), TEXT("Emit an acoustic event at the listener. Args: [Intensity]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UOrganicReverbSubsystem* Subsystem = FindSubsystem(World);
			FVector Location;
			if (Subsystem && Subsystem->GetListenerLocation(Location))
			{
				Subsystem->EmitAcousticEvent(Location, Args.Num() > 0 ? FCString::Atof(*Args[0]) : -1.f);
			}
		}));

	static FAutoConsoleCommandWithWorldAndArgs ShotCommand(
		TEXT("ors.Shot"), TEXT("Play a test ping at the listener, emit an acoustic event there and measure the decay of the listener room. Args: [Intensity]"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			if (UOrganicReverbSubsystem* Subsystem = FindSubsystem(World))
			{
				Subsystem->StartDecayMeasurement(Args.Num() > 0 ? FCString::Atof(*Args[0]) : -1.f);
			}
		}));

	static FAutoConsoleCommandWithWorldAndArgs DoorCommand(
		TEXT("ors.Door"), TEXT("Set transmission of the portal nearest to the listener. Args: <0..1>"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>& Args, UWorld* World)
		{
			UOrganicReverbSubsystem* Subsystem = FindSubsystem(World);
			FVector Location;
			if (Subsystem && Subsystem->GetListenerLocation(Location))
			{
				Subsystem->SetPortalTransmissionAtLocation(Location, Args.Num() > 0 ? FCString::Atof(*Args[0]) : 0.f, 400.f);
			}
		}));

	static FAutoConsoleCommandWithWorldAndArgs RebuildCommand(
		TEXT("ors.Rebuild"), TEXT("Rescan the level and rebuild the acoustic room graph"),
		FConsoleCommandWithWorldAndArgsDelegate::CreateLambda([](const TArray<FString>&, UWorld* World)
		{
			if (UOrganicReverbSubsystem* Subsystem = FindSubsystem(World)) Subsystem->RebuildAcousticGraph();
		}));

	// 기준 밀도 대비 dB (-60 ~ 0) → 파랑 ~ 빨강
	static FColor HeatColor(float Db)
	{
		const float T = FMath::Clamp((Db + 60.f) / 60.f, 0.f, 1.f);
		FLinearColor Color = FLinearColor::LerpUsingHSV(FLinearColor(0.f, 0.15f, 1.f), FLinearColor(1.f, 0.f, 0.f), T);
		Color.A = 0.35f + 0.5f * T;
		return Color.ToFColor(true);
	}

	// 히트맵 볼륨용. 같은 파랑~빨강 램프지만 밝기를 에너지에 비례시켜서, 조용한 방은 빛나지 않게 한다
	// (Additive로 쌓이므로 0에 가까워야 "아무것도 없음"이 된다). sRGB 변환 없이 선형 값 그대로 올린다.
	static FColor HeatVolumeColor(float Db)
	{
		const float T = FMath::Clamp((Db + 60.f) / 60.f, 0.f, 1.f);
		const FLinearColor Ramp = FLinearColor::LerpUsingHSV(FLinearColor(0.f, 0.15f, 1.f), FLinearColor(1.f, 0.f, 0.f), T);
		const FLinearColor Scaled = Ramp * FMath::Pow(T, 1.5f);
		return FColor(
			static_cast<uint8>(FMath::Clamp(Scaled.R, 0.f, 1.f) * 255.f),
			static_cast<uint8>(FMath::Clamp(Scaled.G, 0.f, 1.f) * 255.f),
			static_cast<uint8>(FMath::Clamp(Scaled.B, 0.f, 1.f) * 255.f),
			255);
	}
}

void UOrganicReverbSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	// 화면 2D 그래프(감쇠 곡선)는 캔버스 디버그 드로우로 그린다
	DebugDrawHandle = UDebugDrawService::Register(TEXT("Game"), FDebugDrawDelegate::CreateUObject(this, &UOrganicReverbSubsystem::DrawDecayGraph));
}

void UOrganicReverbSubsystem::Deinitialize()
{
	UDebugDrawService::Unregister(DebugDrawHandle);
	RestorePreset();
	if (Heatmap) Heatmap->Release();
	Simulator.Reset();
	Super::Deinitialize();
}

TStatId UOrganicReverbSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UOrganicReverbSubsystem, STATGROUP_Tickables);
}

bool UOrganicReverbSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UOrganicReverbSubsystem::RegisterVolume(AOrganicReverbVolume* InVolume)
{
	if (!InVolume) return;
	if (Volume.IsValid() && Volume.Get() != InVolume)
	{
		UE_LOG(LogOrganicReverb, Warning, TEXT("Multiple OrganicReverbVolumes found. Using %s."), *InVolume->GetName());
	}

	RestorePreset();
	Volume = InVolume;
	bDebugDraw = InVolume->bDrawDebugOnBeginPlay;
	bHasSmoothed = false;
	if (InVolume->ReverbPreset)
	{
		ModifiedPreset = InVolume->ReverbPreset;
		OriginalPresetSettings = ModifiedPreset->Settings;
	}
	else
	{
		UE_LOG(LogOrganicReverb, Warning, TEXT("OrganicReverbVolume has no ReverbPreset. Simulation runs, but no audio output."));
	}
	RequestRebuild();
}

void UOrganicReverbSubsystem::UnregisterVolume(AOrganicReverbVolume* InVolume)
{
	if (Volume.Get() != InVolume) return;
	RestorePreset();
	Volume.Reset();
	Simulator.Reset();
	RoomGraph.Clear();
	Segmentation = Acoustic::FSpaceSegmentation();
	ScanJob.Reset();
	RebuildCountdown = -1;
	// 진행 중인 워커 작업은 결과를 버린다 (그래프를 참조하지 않으므로 기다릴 필요는 없다)
	SegmentationFuture = TFuture<TSharedPtr<FSegmentationResult, ESPMode::ThreadSafe>>();
	if (Heatmap) Heatmap->Release();
}

void UOrganicReverbSubsystem::RegisterSource(UOrganicSoundSourceComponent* Source)
{
	if (Source) Sources.AddUnique(Source);
}

void UOrganicReverbSubsystem::UnregisterSource(UOrganicSoundSourceComponent* Source)
{
	Sources.Remove(Source);
}

void UOrganicReverbSubsystem::UpdateSources(float DeltaTime, const FVector& ListenerLocation)
{
	Sources.RemoveAll([](const TWeakObjectPtr<UOrganicSoundSourceComponent>& Source) { return !Source.IsValid(); });
	if (Sources.IsEmpty()) return;

	GetReverbSubmix();

	const Acoustic::RoomId ListenerRoom = FindRoom(ListenerLocation);
	const Acoustic::FVec3 Listener = OrganicReverb::ToCore(ListenerLocation);
	for (const TWeakObjectPtr<UOrganicSoundSourceComponent>& Weak : Sources)
	{
		UOrganicSoundSourceComponent* Source = Weak.Get();
		const FVector SourceLocation = Source->GetSourceLocation();
		const Acoustic::RoomId SourceRoom = FindRoom(SourceLocation);

		const Acoustic::FPropagationPath Path = Acoustic::FindPropagationPath(RoomGraph, SourceRoom, OrganicReverb::ToCore(SourceLocation),
			ListenerRoom, Listener, PropagationSettings, &Segmentation);
		const float CouplingDb = Path.bValid ? Coupling.GetCouplingDb(RoomGraph, SourceRoom, ListenerRoom) : 0.f;
		Source->ApplyPropagation(DeltaTime, Path, CouplingDb, Acoustic::MapToSourceOutput(Path, CouplingDb), ReverbSubmix);
	}
}

USoundSubmixBase* UOrganicReverbSubsystem::GetReverbSubmix()
{
	if (!bReverbSubmixResolved)
	{
		bReverbSubmixResolved = true;
		ReverbSubmix = Cast<USoundSubmixBase>(GetDefault<UAudioSettings>()->ReverbSubmix.TryLoad());
		if (!ReverbSubmix) UE_LOG(LogOrganicReverb, Warning, TEXT("Project Settings > Audio > Reverb Submix is not set. Per-source reverb send disabled."));
	}
	return ReverbSubmix;
}

void UOrganicReverbSubsystem::PlayGunshotAtLocation(USoundBase* NearSound, USoundBase* FarSound, FVector WorldLocation, float Intensity)
{
	UWorld* World = GetWorld();
	if (!World || (!NearSound && !FarSound)) return;

	// 에너지는 발사 순간 음원 방에 들어가 퍼진다 (잔향 / 히트맵)
	EmitAcousticEvent(WorldLocation, Intensity);

	FVector ListenerLocation;
	if (!GetListenerLocation(ListenerLocation)) return;

	Acoustic::FShotRenderSettings ShotSettings;
	float BaseReverbSend = 0.15f;
	if (const AOrganicReverbVolume* V = Volume.Get())
	{
		ShotSettings.InnerRadius = static_cast<float>(V->GunshotInnerRadius * OrganicReverb::CmToM);
		ShotSettings.NearLayerDistance = static_cast<float>(V->GunshotNearLayerDistance * OrganicReverb::CmToM);
		ShotSettings.FarLayerDistance = static_cast<float>(V->GunshotFarLayerDistance * OrganicReverb::CmToM);
		BaseReverbSend = V->GunshotBaseReverbSend;
		// 직접음의 공기 흡수도 잔향 쪽과 같은 배율로 (같은 공기다)
		const float AirScale = FMath::Max(0.f, V->AirAbsorptionScale);
		for (int32 B = 0; B < Acoustic::NumBands; ++B) ShotSettings.AirAbsorptionDbPerMeter[B] *= AirScale;
	}

	const Acoustic::RoomId SourceRoom = FindRoom(WorldLocation);
	const Acoustic::RoomId ListenerRoom = FindRoom(ListenerLocation);
	const Acoustic::FPropagationPath Path = Acoustic::FindPropagationPath(RoomGraph, SourceRoom, OrganicReverb::ToCore(WorldLocation),
		ListenerRoom, OrganicReverb::ToCore(ListenerLocation), PropagationSettings, &Segmentation);
	const float CouplingDb = Path.bValid ? Coupling.GetCouplingDb(RoomGraph, SourceRoom, ListenerRoom) : 0.f;
	const Acoustic::FShotRender Shot = Acoustic::RenderShot(Path, CouplingDb, ShotSettings);

	// 잔향 Send는 볼륨이 곱해진 뒤에 갈라지므로(PostDistanceAttenuation), 직접음 볼륨으로 나눠 잔향 절대 크기를 유지한다.
	// 0~1 클램프 때문에 직접음이 BaseReverbSend보다 작아지면(대략 −20 dB 이하) 잔향도 같이 줄기 시작한다.
	const float TargetReverb = BaseReverbSend * Shot.ReverbSendMultiplier;
	const float Send = FMath::Clamp(TargetReverb / FMath::Max(Shot.VolumeMultiplier, 1e-4f), 0.f, 1.f);
	const float NearGain = FarSound ? Shot.NearLayerGain : 1.f; // 원거리 녹음이 없으면 근거리 녹음이 전부 담당
	const float FarGain = FarSound ? Shot.FarLayerGain : 0.f;
	const FVector PlayLocation = OrganicReverb::ToUnreal(Shot.ApparentPosition);

	UE_LOG(LogOrganicReverb, Log, TEXT("Gunshot: path %.1f m (%d portals, walls %d%s), delay %.0f ms, direct %.1f dB (High %.1f), LPF %.0f Hz, layers near %.2f / far %.2f, reverb coupling %.1f dB -> send %.2f"),
		Shot.Distance, static_cast<int32>(Path.Portals.size()), Path.WallsBetween, Path.bOccluded ? TEXT(", occluded") : TEXT(""),
		Shot.DelaySeconds * 1000.f, Shot.GainDb.Mid, Shot.GainDb.High, Shot.LowpassHz, NearGain, FarGain, CouplingDb, Send);

	if (IsDebugDrawEnabled())
	{
		const FColor Color = Path.bOccluded ? FColor::Red : (!Path.bValid || Path.bSameRoom || Path.bLineOfSight) ? FColor::Green : FColor::Orange;
		// 당기고 남은 꼭짓점을 그대로 잇는다 (= 거리·회절을 계산한 그 경로)
		FVector Prev = WorldLocation;
		for (int32 I = 1; I < static_cast<int32>(Path.PathPoints.size()) - 1; ++I)
		{
			const FVector Point = OrganicReverb::ToUnreal(Path.PathPoints[I]);
			DrawDebugLine(World, Prev, Point, Color, false, 2.f, 0, 4.f);
			Prev = Point;
		}
		DrawDebugLine(World, Prev, ListenerLocation - FVector(0.f, 0.f, 30.f), Color, false, 2.f, 0, 4.f);
		DrawDebugString(World, PlayLocation + FVector(0.f, 0.f, 80.f),
			FString::Printf(TEXT("SHOT %.0fm, +%.0fms\n%.1f dB, LPF %.0f Hz\nfar layer %.2f"), Shot.Distance, Shot.DelaySeconds * 1000.f, Shot.GainDb.Mid, Shot.LowpassHz, FarGain),
			nullptr, Color, 2.f, true);
	}

	const TWeakObjectPtr<USoundBase> WeakNear = NearSound;
	const TWeakObjectPtr<USoundBase> WeakFar = FarSound;
	const float DirectVolume = Shot.VolumeMultiplier;
	const float LowpassHz = Shot.LowpassHz;
	auto PlayLayers = [this, WeakNear, WeakFar, PlayLocation, DirectVolume, LowpassHz, Send, NearGain, FarGain]()
	{
		if (USoundBase* Near = WeakNear.Get(); Near && NearGain > 0.001f) SpawnShotLayer(Near, PlayLocation, DirectVolume * NearGain, LowpassHz, Send);
		if (USoundBase* Far = WeakFar.Get(); Far && FarGain > 0.001f) SpawnShotLayer(Far, PlayLocation, DirectVolume * FarGain, LowpassHz, Send);
	};

	// 소리가 도착하는 시각에 재생 (34 m ≈ 0.1 s)
	if (Shot.DelaySeconds > 0.001f)
	{
		FTimerHandle Handle;
		World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateWeakLambda(this, PlayLayers), Shot.DelaySeconds, false);
	}
	else
	{
		PlayLayers();
	}
}

void UOrganicReverbSubsystem::SpawnShotLayer(USoundBase* Sound, const FVector& Location, float VolumeMultiplier, float LowpassHz, float ReverbSend)
{
	UWorld* World = GetWorld();
	if (!World || !Sound) return;

	if (!GunshotAttenuation)
	{
		GunshotAttenuation = NewObject<USoundAttenuation>(this);
		GunshotAttenuation->Attenuation.bAttenuate = false;        // 거리 감쇠는 RenderShot이 볼륨으로 계산
		GunshotAttenuation->Attenuation.bSpatialize = true;        // 방향(패닝)은 엔진이 들리는 위치 기준으로
		GunshotAttenuation->Attenuation.bEnableReverbSend = false; // 잔향 Send는 아래에서 직접
	}

	// 재생 전에 로우패스/Send를 설정해야 첫 트랜지언트("탕")부터 적용된다 → 컴포넌트를 직접 만들어 설정 후 Play
	UAudioComponent* Audio = NewObject<UAudioComponent>(World->GetWorldSettings());
	Audio->bAutoActivate = false;
	Audio->bAutoDestroy = true;
	Audio->SetSound(Sound);
	Audio->AttenuationSettings = GunshotAttenuation;
	Audio->SetVolumeMultiplier(VolumeMultiplier);
	Audio->SetWorldLocation(Location);
	Audio->RegisterComponentWithWorld(World);
	Audio->SetLowPassFilterEnabled(true);
	Audio->SetLowPassFilterFrequency(LowpassHz);
	if (USoundSubmixBase* Submix = GetReverbSubmix())
	{
		Audio->SetSubmixSend(Submix, ReverbSend);
	}
	Audio->Play();
}

void UOrganicReverbSubsystem::RestorePreset()
{
	if (ModifiedPreset)
	{
		ModifiedPreset->SetSettings(OriginalPresetSettings);
		ModifiedPreset = nullptr;
	}
}

void UOrganicReverbSubsystem::RequestRebuild()
{
	RebuildCountdown = 2;
}

bool UOrganicReverbSubsystem::IsRebuilding() const
{
	return RebuildCountdown >= 0 || ScanJob.IsValid() || SegmentationFuture.IsValid();
}

// 백그라운드 재생성 (6-4).
//   1) 스캔: 오버랩 쿼리는 물리 씬을 읽으므로 게임 스레드에서만 가능 → 프레임마다 예산만큼만 진행
//   2) 방 분할: 순수 C++ 계산 → 워커 스레드로 넘기고 매 프레임 완료만 확인
//   3) 다 되면 그래프를 갈아 끼운다. 그 전까지는 이전 그래프로 계속 소리가 난다
void UOrganicReverbSubsystem::UpdateRebuild()
{
	UWorld* World = GetWorld();
	const AOrganicReverbVolume* V = Volume.Get();
	if (!World || !V) return;

	// 3) 워커 스레드가 끝났는가
	if (SegmentationFuture.IsValid())
	{
		if (!SegmentationFuture.IsReady()) return;

		TSharedPtr<FSegmentationResult, ESPMode::ThreadSafe> Result = SegmentationFuture.Get();
		SegmentationFuture = TFuture<TSharedPtr<FSegmentationResult, ESPMode::ThreadSafe>>();
		if (Result.IsValid())
		{
			ApplyNewGraph(MoveTemp(Result->Graph), MoveTemp(Result->Segmentation), PendingScanStats, Result->SegmentMs);
		}
		return;
	}

	// 1) 스캔 진행
	if (!ScanJob.IsValid()) return;

	const double BudgetSeconds = FMath::Max(0.1f, V->ScanTimeBudgetMs) / 1000.0;
	if (!ScanJob->Step(*World, BudgetSeconds)) return;

	// 2) 스캔이 끝났으면 방 분할을 워커 스레드로
	PendingScanStats = ScanJob->GetStats();
	TSharedPtr<Acoustic::FOccupancyGrid, ESPMode::ThreadSafe> Grid =
		MakeShared<Acoustic::FOccupancyGrid, ESPMode::ThreadSafe>(MoveTemp(ScanJob->GetGrid()));
	ScanJob.Reset();

	const Acoustic::FSegmentationSettings SegSettings = MakeSegmentationSettings();
	SegmentationFuture = Async(EAsyncExecution::ThreadPool, [Grid, SegSettings]()
	{
		TSharedPtr<FSegmentationResult, ESPMode::ThreadSafe> Result = MakeShared<FSegmentationResult, ESPMode::ThreadSafe>();
		const double Start = FPlatformTime::Seconds();
		Result->Segmentation = Acoustic::SegmentSpace(*Grid, Result->Graph, SegSettings);
		Result->SegmentMs = (FPlatformTime::Seconds() - Start) * 1000.0;
		return Result;
	});
}

void UOrganicReverbSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (RebuildCountdown > 0)
	{
		--RebuildCountdown;
	}
	else if (RebuildCountdown == 0)
	{
		// 물리가 자리를 잡았으니 백그라운드 재생성을 시작한다
		RebuildCountdown = -1;
		if (const AOrganicReverbVolume* V = Volume.Get())
		{
			FAcousticScanSettings ScanSettings;
			ScanSettings.Bounds = V->GetScanBounds();
			ScanSettings.CellSize = V->CellSize;
			ScanSettings.ObjectType = V->ScanObjectType;
			ScanSettings.DefaultAbsorption = V->DefaultAbsorption;
			ScanSettings.IgnoredActors.Add(V);

			ScanJob = MakeUnique<FAcousticGridScanJob>();
			if (!ScanJob->Begin(*GetWorld(), ScanSettings))
			{
				UE_LOG(LogOrganicReverb, Warning, TEXT("Grid scan failed."));
				ScanJob.Reset();
			}
		}
		else
		{
			UE_LOG(LogOrganicReverb, Warning, TEXT("RequestRebuild: no OrganicReverbVolume registered."));
		}
	}
	UpdateRebuild();

	if (!Simulator) return;

	Simulator->Tick(DeltaTime);
	UpdateOutput(DeltaTime);
	UpdateDecayMeasurement(DeltaTime);

	FVector ListenerLocation;
	if (GetListenerLocation(ListenerLocation))
	{
		UpdateSources(DeltaTime, ListenerLocation);
	}

	UpdateHeatmap(DeltaTime);
	if (IsDebugDrawEnabled())
	{
		DrawDebug();
	}
}

bool UOrganicReverbSubsystem::IsDebugDrawEnabled() const
{
	const int32 DebugCVar = OrganicReverbConsole::CVarDebug.GetValueOnGameThread();
	return DebugCVar < 0 ? bDebugDraw : DebugCVar > 0;
}

void UOrganicReverbSubsystem::StartDecayMeasurement(float Intensity)
{
	bDecayMeasurementPending = true; // 다음 틱에 (그래프와 리스너가 준비되어 있으면) 실행
	PendingShotIntensity = Intensity;
}

void UOrganicReverbSubsystem::UpdateDecayMeasurement(float DeltaTime)
{
	if (bDecayMeasurementPending)
	{
		FVector ListenerLocation;
		const Acoustic::RoomId Room = GetListenerLocation(ListenerLocation) ? FindRoom(ListenerLocation) : Acoustic::InvalidRoomId;
		if (Room != Acoustic::InvalidRoomId)
		{
			bDecayMeasurementPending = false;
			DecayRoom = Room;
			PlayTestShot(ListenerLocation, PendingShotIntensity);
			DecayMeter.Start(30.f);
			return; // 첫 샘플은 시뮬레이션이 한 번 돈 다음 틱부터
		}
	}

	if (!DecayMeter.IsRunning()) return;

	DecayMeter.AddSample(DeltaTime, Simulator->GetEnergyDensity(DecayRoom).Mid);
	if (!DecayMeter.IsRunning())
	{
		const Acoustic::FDecayMeasurement R = DecayMeter.GetResult();
		const Acoustic::FRoom* Room = RoomGraph.GetRoom(DecayRoom);
		UE_LOG(LogOrganicReverb, Log, TEXT("Decay measurement in %s: EDT %.2f s, T20 %.2f s, late(-40~-60 dB) %.2f s, room decay %.2f s, measured %.1f s%s"),
			Room ? UTF8_TO_TCHAR(Room->Name.c_str()) : TEXT("?"), R.EDT, R.T20, R.LateRT,
			Acoustic::MapToReverbParams(RoomGraph, DecayRoom, Acoustic::FBandValues(), MakeMappingSettings()).DecayTime, R.Duration,
			R.IsDoubleSlope() ? TEXT(" [double slope]") : TEXT(""));
	}
}

void UOrganicReverbSubsystem::DrawDecayGraph(UCanvas* Canvas, APlayerController* PlayerController)
{
	if (!Canvas || !DecayMeter.HasData() || !IsDebugDrawEnabled()) return;
	if (PlayerController && PlayerController->GetWorld() != GetWorld()) return; // 다른 PIE 월드의 뷰포트

	const float W = 420.f, H = 180.f;
	const float X0 = 60.f, Y0 = Canvas->ClipY - H - 60.f;
	const Acoustic::FDecayMeasurement R = DecayMeter.GetResult();
	const float Duration = FMath::Max(1.f, R.Duration);
	auto ToScreen = [&](float Time, float Db) { return FVector2D(X0 + W * Time / Duration, Y0 + H * (-FMath::Clamp(Db, -60.f, 0.f) / 60.f)); };

	// 축과 −20 / −40 dB 눈금
	Canvas->K2_DrawLine(FVector2D(X0, Y0), FVector2D(X0, Y0 + H), 1.f, FLinearColor(0.7f, 0.7f, 0.7f));
	Canvas->K2_DrawLine(FVector2D(X0, Y0 + H), FVector2D(X0 + W, Y0 + H), 1.f, FLinearColor(0.7f, 0.7f, 0.7f));
	for (const float Db : { -20.f, -40.f })
	{
		Canvas->K2_DrawLine(ToScreen(0.f, Db), ToScreen(Duration, Db), 1.f, FLinearColor(0.3f, 0.3f, 0.3f));
	}

	// 감쇠 곡선 (최대 약 400개 선분)
	const size_t Num = DecayMeter.NumSamples();
	const size_t Step = FMath::Max<size_t>(1, Num / 400);
	FVector2D Prev = ToScreen(DecayMeter.TimeAt(0), DecayMeter.DbAt(0));
	for (size_t I = Step; I < Num; I += Step)
	{
		const FVector2D Point = ToScreen(DecayMeter.TimeAt(I), DecayMeter.DbAt(I));
		Canvas->K2_DrawLine(Prev, Point, 2.f, FLinearColor(1.f, 0.8f, 0.2f));
		Prev = Point;
	}

	auto Seconds = [](float Value) { return Value > 0.f ? FString::Printf(TEXT("%.2fs"), Value) : FString(TEXT("-")); };
	const Acoustic::FRoom* Room = RoomGraph.GetRoom(DecayRoom);
	const float RoomDecay = Room ? Acoustic::MapToReverbParams(RoomGraph, DecayRoom, Acoustic::FBandValues(), MakeMappingSettings()).DecayTime : 0.f;
	UFont* Font = GEngine->GetSmallFont();
	Canvas->SetDrawColor(FColor::White);
	Canvas->DrawText(Font, FString::Printf(TEXT("Decay @ %s  (%.1fs)%s"), Room ? UTF8_TO_TCHAR(Room->Name.c_str()) : TEXT("?"), R.Duration,
		DecayMeter.IsRunning() ? TEXT("  measuring...") : TEXT("")), X0, Y0 - 36.f);
	Canvas->DrawText(Font, FString::Printf(TEXT("EDT %s   T20 %s   Late(-40~-60) %s   Room decay %.2fs%s"),
		*Seconds(R.EDT), *Seconds(R.T20), *Seconds(R.LateRT), RoomDecay, R.IsDoubleSlope() ? TEXT("   DOUBLE SLOPE") : TEXT("")), X0, Y0 - 20.f);
	Canvas->DrawText(Font, TEXT("0 dB"), X0 - 44.f, Y0 - 6.f);
	Canvas->DrawText(Font, TEXT("-60"), X0 - 44.f, Y0 + H - 6.f);
}

void UOrganicReverbSubsystem::RebuildAcousticGraph()
{
	const AOrganicReverbVolume* V = Volume.Get();
	if (!V)
	{
		UE_LOG(LogOrganicReverb, Warning, TEXT("RebuildAcousticGraph: no OrganicReverbVolume registered."));
		return;
	}

	FAcousticScanSettings ScanSettings;
	ScanSettings.Bounds = V->GetScanBounds();
	ScanSettings.CellSize = V->CellSize;
	ScanSettings.ObjectType = V->ScanObjectType;
	ScanSettings.DefaultAbsorption = V->DefaultAbsorption;
	ScanSettings.IgnoredActors.Add(V);

	Acoustic::FOccupancyGrid Grid;
	FAcousticScanStats Stats;
	if (!FAcousticGridScanner::Scan(*GetWorld(), ScanSettings, Grid, &Stats))
	{
		UE_LOG(LogOrganicReverb, Warning, TEXT("Grid scan failed."));
		return;
	}

	const double SegStart = FPlatformTime::Seconds();
	Acoustic::FAcousticRoomGraph NewGraph;
	Acoustic::FSpaceSegmentation NewSegmentation = Acoustic::SegmentSpace(Grid, NewGraph, MakeSegmentationSettings());
	ApplyNewGraph(MoveTemp(NewGraph), MoveTemp(NewSegmentation), Stats, (FPlatformTime::Seconds() - SegStart) * 1000.0);
}

Acoustic::FSegmentationSettings UOrganicReverbSubsystem::MakeSegmentationSettings() const
{
	Acoustic::FSegmentationSettings SegSettings;
	if (const AOrganicReverbVolume* V = Volume.Get())
	{
		SegSettings.MaxPortalWidth = static_cast<float>(V->MaxPortalWidth * OrganicReverb::CmToM);
		SegSettings.MaxRoomLength = static_cast<float>(V->MaxRoomLength * OrganicReverb::CmToM);
		SegSettings.MinRoomVolume = V->MinRoomVolume;
		SegSettings.BoundaryMaterial = V->OutsideAbsorption.ToCore(TEXT("Outside"));
	}
	return SegSettings;
}

// 완성된 그래프를 갈아 끼운다. 여기 오기 전까지는 이전 그래프로 계속 소리가 난다
void UOrganicReverbSubsystem::ApplyNewGraph(Acoustic::FAcousticRoomGraph&& NewGraph, Acoustic::FSpaceSegmentation&& NewSegmentation,
	const FAcousticScanStats& ScanStats, double SegmentMs)
{
	Simulator.Reset(); // 시뮬레이터가 그래프를 참조하므로 먼저 해제
	RoomGraph = MoveTemp(NewGraph);
	Segmentation = MoveTemp(NewSegmentation);

	// 시뮬레이터 · 잔향 결합 · 파라미터 매핑이 모두 같은 공기 흡음 값을 써야 들리는 것과 표시가 맞는다
	Acoustic::FDiffusionSettings DiffusionSettings;
	DiffusionSettings.AirAbsorptionCoefficient = GetAirAbsorption();
	if (const AOrganicReverbVolume* V = Volume.Get()) DiffusionSettings.bPortalDelay = V->bPortalDelay;
	Simulator = MakeUnique<Acoustic::FAcousticDiffusionSimulator>(RoomGraph, DiffusionSettings);
	Coupling.AirAbsorptionCoefficient = DiffusionSettings.AirAbsorptionCoefficient.Mid;
	Coupling.Invalidate();
	DecayMeter.Stop(); // 방 번호가 바뀌므로 진행 중인 측정은 중단

	if (!Heatmap) Heatmap = NewObject<UAcousticHeatmapVolume>(this);
	Heatmap->Rebuild(GetWorld(), Segmentation);
	HeatmapUpdateTimer = 0.f;
	bHasSmoothed = false;

	// 스캔 전에 들어와 있던 이벤트를 이제 쏜다
	const TArray<FPendingAcousticEvent> Pending = MoveTemp(PendingEvents);
	PendingEvents.Reset();
	for (const FPendingAcousticEvent& Event : Pending)
	{
		EmitAcousticEvent(Event.Location, Event.Intensity);
	}

	UE_LOG(LogOrganicReverb, Log, TEXT("Scan %dx%dx%d = %lld cells (solid %lld, surface %lld, materials %d) in %.1f ms over %d step(s), %.1f s wall -> %d rooms, %d portals in %.1f ms"),
		Segmentation.SizeX, Segmentation.SizeY, Segmentation.SizeZ, ScanStats.NumCells, ScanStats.SolidCells, ScanStats.SurfaceCells,
		ScanStats.NumMaterials, ScanStats.WorkSeconds * 1000.0, ScanStats.Steps, ScanStats.Seconds,
		static_cast<int32>(RoomGraph.NumRooms()), static_cast<int32>(RoomGraph.NumPortals()), SegmentMs);
	for (const Acoustic::FRoom& Room : RoomGraph.GetRooms())
	{
		UE_LOG(LogOrganicReverb, Log, TEXT("  %s (group %u): V=%.1f m3, S=%.1f m2, alpha(Mid)=%.3f, Decay(group)=%.2f s"),
			UTF8_TO_TCHAR(Room.Name.c_str()), Room.GroupId, Room.Volume, Room.SurfaceArea, Room.Material.Absorption.Mid,
			Acoustic::MapToReverbParams(RoomGraph, Room.Id, Acoustic::FBandValues(), MakeMappingSettings()).DecayTime);
	}
}

Acoustic::RoomId UOrganicReverbSubsystem::FindRoom(const FVector& WorldLocation) const
{
	if (!Simulator) return Acoustic::InvalidRoomId;
	return Segmentation.FindRoomAt(OrganicReverb::ToCore(WorldLocation));
}

Acoustic::FBandValues UOrganicReverbSubsystem::GetAirAbsorption() const
{
	const AOrganicReverbVolume* V = Volume.Get();
	const float Scale = V ? FMath::Max(0.f, V->AirAbsorptionScale) : 1.f;
	Acoustic::FBandValues Out = Acoustic::AirAbsorption::Coefficient();
	for (int32 B = 0; B < Acoustic::NumBands; ++B) Out[B] *= Scale;
	return Out;
}

Acoustic::FReverbMappingSettings UOrganicReverbSubsystem::MakeMappingSettings() const
{
	Acoustic::FReverbMappingSettings Mapping;
	Mapping.AirAbsorptionCoefficient = GetAirAbsorption();
	if (const AOrganicReverbVolume* V = Volume.Get()) Mapping.ReferenceDensity = V->EventReferenceDensity;
	return Mapping;
}

void UOrganicReverbSubsystem::EmitAcousticEvent(const FVector& WorldLocation, float Intensity)
{
	// 레벨 시작 직후(스캔 전)에 들어온 이벤트는 그래프가 준비되면 쏘도록 미뤄 둔다.
	// 시작 인자 -ExecCmds="ors.Emit ..." 이 조용히 무시되던 문제
	if (!Simulator)
	{
		PendingEvents.Add({ WorldLocation, Intensity });
		return;
	}

	const Acoustic::RoomId Room = FindRoom(WorldLocation);
	if (Room == Acoustic::InvalidRoomId)
	{
		UE_LOG(LogOrganicReverb, Verbose, TEXT("EmitAcousticEvent: %s is not inside any room."), *WorldLocation.ToString());
		return;
	}

	const AOrganicReverbVolume* V = Volume.Get();
	const float Energy = Intensity >= 0.f ? Intensity : (V ? V->DefaultEventIntensity : 100.f);
	Simulator->InjectEnergy(Room, Acoustic::FBandValues::Uniform(Energy));
	UE_LOG(LogOrganicReverb, Log, TEXT("Acoustic event %.0f in %s"), Energy, UTF8_TO_TCHAR(RoomGraph.GetRoom(Room)->Name.c_str()));
}

FOrganicReverbParams UOrganicReverbSubsystem::GetReverbParamsAtLocation(const FVector& WorldLocation) const
{
	FOrganicReverbParams Out;
	const Acoustic::RoomId Room = FindRoom(WorldLocation);
	const Acoustic::FRoom* R = RoomGraph.GetRoom(Room);
	if (!R) return Out;

	// 잔향 길이는 같은 그룹(분할된 복도/홀) 전체로, 이벤트 Wet은 이 방의 에너지로
	const Acoustic::FReverbParams P = Acoustic::MapToReverbParams(RoomGraph, Room, Simulator->GetEnergyDensity(Room), MakeMappingSettings());
	Out.RoomIndex = static_cast<int32>(Room);
	Out.DecayTime = P.DecayTime;
	Out.DecayHFRatio = P.DecayHFRatio;
	Out.ReflectionsDelay = P.ReflectionsDelay;
	Out.EventWetLevel = P.WetLevel;
	return Out;
}

float UOrganicReverbSubsystem::GetWetLevelAtLocation(const FVector& WorldLocation) const
{
	return GetReverbParamsAtLocation(WorldLocation).EventWetLevel;
}

void UOrganicReverbSubsystem::PlayTestShot(const FVector& WorldLocation, float Intensity)
{
	// 볼륨에 총성 사운드가 지정되어 있으면 원거리 총성과 같은 경로로 재생 (무작위 변주)
	if (const AOrganicReverbVolume* V = Volume.Get(); V && V->ShotSounds.Num() > 0)
	{
		if (USoundBase* Sound = V->ShotSounds[FMath::RandRange(0, V->ShotSounds.Num() - 1)].Get())
		{
			PlayGunshotAtLocation(Sound, nullptr, WorldLocation, Intensity);
			return;
		}
	}

	if (!TestShotSound)
	{
		// 짧은 1kHz 핑: 잔향 꼬리를 듣기 좋은 엔진 기본 사운드
		TestShotSound = LoadObject<USoundBase>(nullptr, TEXT("/Engine/EngineSounds/1kSineTonePing.1kSineTonePing"));
		TestShotAttenuation = NewObject<USoundAttenuation>(this);
		TestShotAttenuation->Attenuation.bEnableReverbSend = true; // Project Settings의 Reverb Submix로 보냄
	}

	if (TestShotSound)
	{
		UGameplayStatics::PlaySoundAtLocation(this, TestShotSound, WorldLocation, 1.f, 1.f, 0.f, TestShotAttenuation);
	}
	else
	{
		UE_LOG(LogOrganicReverb, Warning, TEXT("PlayTestShot: /Engine/EngineSounds/1kSineTonePing not found."));
	}
	EmitAcousticEvent(WorldLocation, Intensity);
}

bool UOrganicReverbSubsystem::SetPortalTransmissionAtLocation(const FVector& WorldLocation, float Transmission, float SearchRadius)
{
	if (!Simulator) return false;

	const Acoustic::FVec3 P = OrganicReverb::ToCore(WorldLocation);
	float BestDistSq = FMath::Square(static_cast<float>(SearchRadius * OrganicReverb::CmToM));
	Acoustic::PortalId Best = Acoustic::InvalidPortalId;
	for (const Acoustic::FPortal& Portal : RoomGraph.GetPortals())
	{
		const float DistSq = (Portal.Center - P).LengthSquared();
		if (DistSq <= BestDistSq)
		{
			BestDistSq = DistSq;
			Best = Portal.Id;
		}
	}

	Acoustic::FPortal* Portal = RoomGraph.GetPortal(Best);
	if (!Portal) return false;
	Portal->Transmission = FMath::Clamp(Transmission, 0.f, 1.f);
	Coupling.Invalidate(); // 잔향 결합은 문 상태에 따라 달라짐
	UE_LOG(LogOrganicReverb, Log, TEXT("Portal %u (Room_%u <-> Room_%u) transmission = %.2f"), Portal->Id, Portal->RoomA, Portal->RoomB, Portal->Transmission);
	return true;
}

bool UOrganicReverbSubsystem::GetListenerLocation(FVector& OutLocation) const
{
	const UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC) return false;

	FVector Front, Right;
	PC->GetAudioListenerPosition(OutLocation, Front, Right);
	return true;
}

void UOrganicReverbSubsystem::UpdateOutput(float DeltaTime)
{
	FVector ListenerLocation;
	if (!GetListenerLocation(ListenerLocation)) return;

	const FOrganicReverbParams Target = GetReverbParamsAtLocation(ListenerLocation);
	if (Target.RoomIndex == INDEX_NONE) return; // 방 밖(벽 속 등)이면 마지막 값 유지

	const AOrganicReverbVolume* V = Volume.Get();
	const float SmoothingTime = V ? V->ParameterSmoothingTime : 0.25f;
	const float Alpha = SmoothingTime > 0.f ? 1.f - FMath::Exp(-DeltaTime / SmoothingTime) : 1.f;

	if (!bHasSmoothed)
	{
		Smoothed = Target;
		bHasSmoothed = true;
	}
	else
	{
		Smoothed.RoomIndex = Target.RoomIndex;
		Smoothed.DecayTime = FMath::Lerp(Smoothed.DecayTime, Target.DecayTime, Alpha);
		Smoothed.DecayHFRatio = FMath::Lerp(Smoothed.DecayHFRatio, Target.DecayHFRatio, Alpha);
		Smoothed.ReflectionsDelay = FMath::Lerp(Smoothed.ReflectionsDelay, Target.ReflectionsDelay, Alpha);
		// 총성 도달은 즉시 반영, 사라질 때만 부드럽게
		Smoothed.EventWetLevel = Target.EventWetLevel > Smoothed.EventWetLevel
			? Target.EventWetLevel
			: FMath::Lerp(Smoothed.EventWetLevel, Target.EventWetLevel, Alpha);
	}

	ApplyToSubmix(Smoothed);
}

void UOrganicReverbSubsystem::ApplyToSubmix(const FOrganicReverbParams& Params)
{
	const AOrganicReverbVolume* V = Volume.Get();
	if (!V || !ModifiedPreset) return;

	FSubmixEffectReverbSettings Settings = ModifiedPreset->Settings; // 디자이너가 정한 나머지 값은 유지
	Settings.DecayTime = FMath::Clamp(Params.DecayTime, 0.1f, 20.f);
	Settings.DecayHFRatio = FMath::Clamp(Params.DecayHFRatio, 0.1f, 2.f);
	Settings.ReflectionsDelay = FMath::Clamp(Params.ReflectionsDelay, 0.f, 0.3f);
	Settings.WetLevel = FMath::Clamp(V->BaseWetLevel + V->EventWetBoost * Params.EventWetLevel, 0.f, 10.f);
	ModifiedPreset->SetSettings(Settings);
}

// 방별 에너지를 색으로 바꿔 히트맵 볼륨에 올린다. 매 프레임 텍스처를 올릴 필요는 없으므로 20 Hz로 제한
void UOrganicReverbSubsystem::UpdateHeatmap(float DeltaTime)
{
	if (!Heatmap || !Heatmap->IsBuilt()) return;

	const float Intensity = OrganicReverbConsole::CVarHeatmapIntensity.GetValueOnGameThread();
	const bool bEnabled = IsDebugDrawEnabled() && Intensity > 0.f;
	Heatmap->SetVisible(bEnabled);
	if (!bEnabled || !Simulator) return;
	Heatmap->SetLook(Intensity);

	HeatmapUpdateTimer -= DeltaTime;
	if (HeatmapUpdateTimer > 0.f) return;
	HeatmapUpdateTimer = 0.05f;

	const AOrganicReverbVolume* V = Volume.Get();
	const float Reference = V ? V->EventReferenceDensity : 1.f;

	HeatmapRoomColors.SetNum(static_cast<int32>(RoomGraph.NumRooms()), EAllowShrinking::No);
	for (const Acoustic::FRoom& Room : RoomGraph.GetRooms())
	{
		// 문으로 이어진 곳이 없는 영역(레이아웃 사이 바깥 공기 등)은 히트맵에서 뺀다
		const bool bIsolated = RoomGraph.GetPortalsOfRoom(Room.Id).empty();
		const float Density = Simulator->GetEnergyDensity(Room.Id).Sum();
		const float Db = Density > 0.f ? 10.f * FMath::LogX(10.f, Density / Reference) : -120.f;
		HeatmapRoomColors[static_cast<int32>(Room.Id)] = bIsolated ? FColor(0, 0, 0, 0) : OrganicReverbConsole::HeatVolumeColor(Db);
	}

	Heatmap->Update(HeatmapRoomColors);
}

void UOrganicReverbSubsystem::DrawDebug() const
{
	UWorld* World = GetWorld();
	const AOrganicReverbVolume* V = Volume.Get();
	if (!World || !V || !Simulator) return;

	// 에너지 자체는 히트맵 볼륨(UpdateHeatmap)이 그린다. 여기서는 방 이름 / 잔향 길이 같은 글자만
	for (const Acoustic::FRoom& Room : RoomGraph.GetRooms())
	{
		// 문으로 이어진 곳이 없는 영역(레이아웃 사이 바깥 공기 등)은 리스너가 그 안에 있을 때만 표시
		if (RoomGraph.GetPortalsOfRoom(Room.Id).empty() && static_cast<int32>(Room.Id) != Smoothed.RoomIndex) continue;

		const float Density = Simulator->GetEnergyDensity(Room.Id).Sum();
		const float Db = Density > 0.f ? 10.f * FMath::LogX(10.f, Density / V->EventReferenceDensity) : -120.f;

		const bool bListenerRoom = static_cast<int32>(Room.Id) == Smoothed.RoomIndex;
		DrawDebugString(World, OrganicReverb::ToUnreal(Room.Center),
			FString::Printf(TEXT("%s\nDecay %.2fs\n%.0f dB"), UTF8_TO_TCHAR(Room.Name.c_str()),
				Acoustic::MapToReverbParams(RoomGraph, Room.Id, Acoustic::FBandValues(), MakeMappingSettings()).DecayTime, Db),
			nullptr, bListenerRoom ? FColor::Yellow : FColor::White, 0.f, true);
	}

	for (const Acoustic::FPortal& Portal : RoomGraph.GetPortals())
	{
		const FVector Center = OrganicReverb::ToUnreal(Portal.Center);
		const FColor Color = Portal.Transmission > 0.5f ? FColor::Green : FColor::Red;
		DrawDebugSphere(World, Center, 20.f, 8, Color, false, -1.f);
		DrawDebugLine(World, OrganicReverb::ToUnreal(RoomGraph.GetRoom(Portal.RoomA)->Center), Center, Color, false, -1.f, 0, 1.5f);
		DrawDebugLine(World, Center, OrganicReverb::ToUnreal(RoomGraph.GetRoom(Portal.RoomB)->Center), Color, false, -1.f, 0, 1.5f);
	}

	// 음원별 전파 경로: 초록 = 같은 방/직선으로 보임, 노랑 = 문을 돌아옴, 빨강 = 막힘(벽 투과만)
	FVector ListenerLocation;
	const bool bHasListener = GetListenerLocation(ListenerLocation);
	for (const TWeakObjectPtr<UOrganicSoundSourceComponent>& Weak : Sources)
	{
		const UOrganicSoundSourceComponent* Source = Weak.Get();
		if (!Source || !Source->GetLastPath().bValid) continue;

		const Acoustic::FPropagationPath& Path = Source->GetLastPath();
		const FColor Color = Path.bOccluded ? FColor::Red : (Path.bSameRoom || Path.bLineOfSight) ? FColor::Green : FColor::Yellow;
		FVector Prev = Source->GetSourceLocation();
		for (int32 I = 1; I < static_cast<int32>(Path.PathPoints.size()) - 1; ++I)
		{
			const FVector Point = OrganicReverb::ToUnreal(Path.PathPoints[I]);
			DrawDebugLine(World, Prev, Point, Color, false, -1.f, 0, 3.f);
			Prev = Point;
		}
		if (bHasListener) DrawDebugLine(World, Prev, ListenerLocation - FVector(0.f, 0.f, 30.f), Color, false, -1.f, 0, 3.f);

		const FOrganicPropagationInfo Info = Source->GetPropagationInfo();
		DrawDebugString(World, Source->GetSourceLocation() + FVector(0.f, 0.f, 60.f),
			FString::Printf(TEXT("%s\nDirect %.1f dB, LPF %.0f Hz\nReverb send %.1f dB"),
				Path.bOccluded ? TEXT("OCCLUDED") : Info.bClearPath ? TEXT("CLEAR") : TEXT("AROUND PORTAL"),
				Info.DirectGainDb, Info.LowpassHz, Info.ReverbSendDb),
			nullptr, Color, 0.f, true);
	}

	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(static_cast<uint64>(0x0B5C0DE), 0.f, FColor::Cyan, FString::Printf(
			TEXT("[OrganicReverb] Room %d | Decay %.2fs | HF %.2f | Refl %.0fms | EventWet %.2f | %d rooms, %d portals"),
			Smoothed.RoomIndex, Smoothed.DecayTime, Smoothed.DecayHFRatio, Smoothed.ReflectionsDelay * 1000.f, Smoothed.EventWetLevel,
			static_cast<int32>(RoomGraph.NumRooms()), static_cast<int32>(RoomGraph.NumPortals())));
	}
}
