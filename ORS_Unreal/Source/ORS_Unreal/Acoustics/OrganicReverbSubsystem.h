// OrganicReverbSubsystem.h
// ---------------------------------------------------------
// Unreal 어댑터: Core(방 그래프 + 확산 시뮬레이터)를 소유하고 매 틱 갱신하며,
// 리스너 위치의 결과를 Submix Reverb 프리셋에 흘려보내고 디버그 히트맵을 그린다.
//
// 게임플레이 코드(총기 발사 등)는 EmitAcousticEvent 같은 BlueprintCallable 함수만 호출하면 된다.
// 콘솔: ors.Debug 0/1, ors.Emit [세기], ors.Shot [세기], ors.Door <0~1>, ors.Rebuild
// ---------------------------------------------------------
#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Subsystems/WorldSubsystem.h"
#include "SubmixEffects/AudioMixerSubmixEffectReverb.h"
#include "Acoustics/AcousticGridScanner.h"
#include "AcousticCore/AcousticDecayMeter.h"
#include "AcousticCore/AcousticDiffusionSimulator.h"
#include "AcousticCore/AcousticPropagation.h"
#include "AcousticCore/AcousticShotRender.h"
#include "AcousticCore/AcousticSpaceSegmenter.h"
#include "AcousticCore/ReverbParameterMapper.h"
#include "OrganicReverbSubsystem.generated.h"

class AOrganicReverbVolume;
class APlayerController;
class UAcousticHeatmapVolume;
class UCanvas;
class UOrganicSoundSourceComponent;
class USoundAttenuation;
class USoundBase;
class USoundSubmixBase;

/** 특정 위치 기준 리버브 파라미터 */
USTRUCT(BlueprintType)
struct FOrganicReverbParams
{
	GENERATED_BODY()

	/** 위치가 속한 방 번호 (-1 = 방 밖) */
	UPROPERTY(BlueprintReadOnly, Category = "Acoustics")
	int32 RoomIndex = INDEX_NONE;

	/** Mid 대역 RT60 (s) */
	UPROPERTY(BlueprintReadOnly, Category = "Acoustics")
	float DecayTime = 1.f;

	/** RT60(High) / RT60(Mid) */
	UPROPERTY(BlueprintReadOnly, Category = "Acoustics")
	float DecayHFRatio = 1.f;

	/** 평균 자유 행로 / 음속 (s) */
	UPROPERTY(BlueprintReadOnly, Category = "Acoustics")
	float ReflectionsDelay = 0.f;

	/** 이벤트(총성) 에너지에 의한 Wet (0~1) */
	UPROPERTY(BlueprintReadOnly, Category = "Acoustics")
	float EventWetLevel = 0.f;
};

UCLASS()
class UOrganicReverbSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	void RegisterVolume(AOrganicReverbVolume* InVolume);
	void UnregisterVolume(AOrganicReverbVolume* InVolume);

	/** 음원별 전파(차폐/잔향 Send) 대상 등록 — UOrganicSoundSourceComponent가 BeginPlay/EndPlay에서 호출 */
	void RegisterSource(UOrganicSoundSourceComponent* Source);
	void UnregisterSource(UOrganicSoundSourceComponent* Source);

	/** 총성/폭발 등. 위치가 속한 방에 에너지를 주입한다. Intensity < 0 이면 볼륨의 기본값 */
	UFUNCTION(BlueprintCallable, Category = "Acoustics")
	void EmitAcousticEvent(const FVector& WorldLocation, float Intensity = -1.f);

	UFUNCTION(BlueprintCallable, Category = "Acoustics")
	FOrganicReverbParams GetReverbParamsAtLocation(const FVector& WorldLocation) const;

	/** 이벤트 에너지에 의한 Wet (0~1). MetaSounds 등 다른 출력에 연결할 때 사용 */
	UFUNCTION(BlueprintCallable, Category = "Acoustics")
	float GetWetLevelAtLocation(const FVector& WorldLocation) const;

	/**
	 * 원거리 총성: WorldLocation에서 쏜 총소리를 리스너 기준 거리감으로 재생한다.
	 * - 도착 지연 = 소리가 지나온 거리 / 음속 (문을 돌아오면 경로 길이)
	 * - 들리는 위치 = 문을 돌아오면 리스너 쪽 마지막 문, 아니면 음원
	 * - 직접음 = 거리 감쇠 + 공기 흡수 + 회절/차폐, 로우패스
	 * - NearSound / FarSound를 거리로 크로스페이드 (FarSound가 없으면 NearSound만)
	 * - 잔향 Send는 직접음이 작아져도 잔향 크기가 유지되도록 보정 → 멀수록 "탕"이 "쿠웅"으로
	 * 발사 순간 음원 위치에 음향 이벤트도 주입한다 (잔향 / 히트맵).
	 */
	UFUNCTION(BlueprintCallable, Category = "Acoustics")
	void PlayGunshotAtLocation(USoundBase* NearSound, USoundBase* FarSound, FVector WorldLocation, float Intensity = -1.f);

	/** 청취 테스트용 총성: 위치에서 핑 사운드(리버브 Send 켜짐)를 재생하고 같은 위치에 음향 이벤트를 주입 */
	UFUNCTION(BlueprintCallable, Category = "Acoustics|Debug")
	void PlayTestShot(const FVector& WorldLocation, float Intensity = -1.f);

	/** 문 개폐: 위치에서 가장 가까운 포탈의 투과율(0 닫힘 ~ 1 열림). 반경 안에 포탈이 없으면 false */
	UFUNCTION(BlueprintCallable, Category = "Acoustics")
	bool SetPortalTransmissionAtLocation(const FVector& WorldLocation, float Transmission, float SearchRadius = 300.f);

	/** 지오메트리가 바뀐 뒤 호출. 물리 갱신을 기다렸다가 백그라운드로 재스캔한다 (게임은 계속 진행) */
	UFUNCTION(BlueprintCallable, Category = "Acoustics")
	void RequestRebuild();

	/** 즉시 재스캔 + 방 그래프 재생성. 끝날 때까지 프레임이 멈춘다 (에너지는 초기화됨) */
	UFUNCTION(BlueprintCallable, Category = "Acoustics")
	void RebuildAcousticGraph();

	/** 백그라운드 재생성이 진행 중인가 */
	UFUNCTION(BlueprintPure, Category = "Acoustics")
	bool IsRebuilding() const;

	/** 리스너 위치에서 테스트 총성을 쏘고 리스너 방의 잔향 감쇠(EDT / T20 / 후기 RT)를 측정한다.
	 *  그래프가 아직 없으면 준비되는 즉시 실행. 결과는 로그 + 디버그 화면 그래프 (콘솔: ors.Shot) */
	UFUNCTION(BlueprintCallable, Category = "Acoustics|Debug")
	void StartDecayMeasurement(float Intensity = -1.f);

	UFUNCTION(BlueprintCallable, Category = "Acoustics|Debug")
	void SetDebugVisualizationEnabled(bool bEnabled) { bDebugDraw = bEnabled; }

	UFUNCTION(BlueprintPure, Category = "Acoustics|Debug")
	bool IsDebugVisualizationEnabled() const { return bDebugDraw; }

	UFUNCTION(BlueprintPure, Category = "Acoustics")
	int32 GetRoomCount() const { return static_cast<int32>(RoomGraph.NumRooms()); }

	/** 첫 번째 로컬 플레이어의 오디오 리스너 위치 */
	bool GetListenerLocation(FVector& OutLocation) const;

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	Acoustic::RoomId FindRoom(const FVector& WorldLocation) const;
	/** 백그라운드 재생성 진행 (스캔은 프레임마다 조금씩, 방 분할은 워커 스레드) */
	void UpdateRebuild();
	/** 완성된 그래프를 실제로 갈아 끼운다 (여기서만 시뮬레이터가 교체된다) */
	void ApplyNewGraph(Acoustic::FAcousticRoomGraph&& NewGraph, Acoustic::FSpaceSegmentation&& NewSegmentation, const FAcousticScanStats& ScanStats, double SegmentMs);
	Acoustic::FSegmentationSettings MakeSegmentationSettings() const;
	/** 볼륨의 튜닝 값(기준 밀도, 공기 흡음)을 반영한 매핑 설정. 로그·디버그 표시도 실제 출력과 같은 값을 쓰도록 */
	Acoustic::FReverbMappingSettings MakeMappingSettings() const;
	/** 볼륨의 공기 흡음 배율을 적용한 대역별 계수 (1 = 실측값, 0 = 끔) */
	Acoustic::FBandValues GetAirAbsorption() const;
	void UpdateOutput(float DeltaTime);
	void UpdateSources(float DeltaTime, const FVector& ListenerLocation);
	USoundSubmixBase* GetReverbSubmix();
	void SpawnShotLayer(USoundBase* Sound, const FVector& Location, float VolumeMultiplier, float LowpassHz, float ReverbSend);
	void ApplyToSubmix(const FOrganicReverbParams& Params);
	void RestorePreset();
	void BuildDebugCache(const Acoustic::FOccupancyGrid& Grid);
	/** 히트맵 볼륨에 방별 에너지 색을 올린다 (표시가 켜져 있을 때만) */
	void UpdateHeatmap(float DeltaTime);
	void DrawDebug() const;
	bool IsDebugDrawEnabled() const;
	void UpdateDecayMeasurement(float DeltaTime);
	void DrawDecayGraph(UCanvas* Canvas, APlayerController* PlayerController);

	// 감쇠 측정 (5-4)
	Acoustic::FDecayMeter DecayMeter;
	Acoustic::RoomId DecayRoom = Acoustic::InvalidRoomId;
	bool bDecayMeasurementPending = false;
	float PendingShotIntensity = -1.f;
	FDelegateHandle DebugDrawHandle;

	TWeakObjectPtr<AOrganicReverbVolume> Volume;

	/** 플레이 중 수정한 프리셋. 종료 시 원래 값으로 되돌린다 (에셋이 PIE 값으로 저장되는 것 방지) */
	UPROPERTY()
	TObjectPtr<USubmixEffectReverbPreset> ModifiedPreset;
	FSubmixEffectReverbSettings OriginalPresetSettings;

	UPROPERTY()
	TObjectPtr<USoundBase> TestShotSound;

	UPROPERTY()
	TObjectPtr<USoundAttenuation> TestShotAttenuation;

	/** 원거리 총성용: 엔진 거리 감쇠와 리버브 Send는 끄고(거리감은 RenderShot이 계산) 공간화만 켠다 */
	UPROPERTY()
	TObjectPtr<USoundAttenuation> GunshotAttenuation;

	Acoustic::FAcousticRoomGraph RoomGraph;
	Acoustic::FSpaceSegmentation Segmentation;
	TUniquePtr<Acoustic::FAcousticDiffusionSimulator> Simulator;

	// 음원별 전파
	TArray<TWeakObjectPtr<UOrganicSoundSourceComponent>> Sources;
	Acoustic::FPropagationSettings PropagationSettings;
	Acoustic::FReverbCoupling Coupling; // 문 상태나 그래프가 바뀌면 Invalidate

	/** Project Settings의 Reverb Submix (음원별 Send 대상) */
	UPROPERTY()
	TObjectPtr<USoundSubmixBase> ReverbSubmix;
	bool bReverbSubmixResolved = false;

	/** 백그라운드 재생성 (6-4). 끝날 때까지 기존 그래프로 계속 시뮬레이션한다 */
	struct FSegmentationResult
	{
		Acoustic::FAcousticRoomGraph Graph;
		Acoustic::FSpaceSegmentation Segmentation;
		double SegmentMs = 0.0;
	};
	TUniquePtr<FAcousticGridScanJob> ScanJob;
	TFuture<TSharedPtr<FSegmentationResult, ESPMode::ThreadSafe>> SegmentationFuture;
	FAcousticScanStats PendingScanStats;

	int32 RebuildCountdown = -1;
	bool bDebugDraw = false;
	bool bHasSmoothed = false;
	FOrganicReverbParams Smoothed;

	/** 에너지 히트맵 (레이마칭 볼륨 오버레이) */
	UPROPERTY()
	TObjectPtr<UAcousticHeatmapVolume> Heatmap;

	float HeatmapUpdateTimer = 0.f;
	TArray<FColor> HeatmapRoomColors;

	/** 스캔이 끝나기 전에 들어온 음향 이벤트 (레벨 시작 직후의 ors.Emit 등) */
	struct FPendingAcousticEvent
	{
		FVector Location = FVector::ZeroVector;
		float Intensity = -1.f;
	};
	TArray<FPendingAcousticEvent> PendingEvents;
};
