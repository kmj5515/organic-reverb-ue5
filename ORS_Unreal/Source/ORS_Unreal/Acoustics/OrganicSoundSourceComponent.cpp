// OrganicSoundSourceComponent.cpp
#include "Acoustics/OrganicSoundSourceComponent.h"
#include "Acoustics/AcousticSurfaceTypes.h"
#include "Acoustics/OrganicReverbSubsystem.h"
#include "Components/AudioComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "TimerManager.h"

UOrganicSoundSourceComponent::UOrganicSoundSourceComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void UOrganicSoundSourceComponent::BeginPlay()
{
	Super::BeginPlay();

	Audio = GetOwner() ? GetOwner()->FindComponentByClass<UAudioComponent>() : nullptr;
	if (Audio)
	{
		BaseVolume = Audio->VolumeMultiplier;
	}
	else
	{
		UE_LOG(LogOrganicReverb, Warning, TEXT("%s: owner has no AudioComponent."), *GetPathName());
	}

	if (UOrganicReverbSubsystem* Subsystem = GetWorld()->GetSubsystem<UOrganicReverbSubsystem>())
	{
		Subsystem->RegisterSource(this);
	}
	if (RepeatInterval > 0.f)
	{
		GetWorld()->GetTimerManager().SetTimer(RepeatTimer, this, &UOrganicSoundSourceComponent::PlaySound, RepeatInterval, true, 0.5f);
	}
}

void UOrganicSoundSourceComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorld()->GetTimerManager().ClearTimer(RepeatTimer);
	if (UOrganicReverbSubsystem* Subsystem = GetWorld()->GetSubsystem<UOrganicReverbSubsystem>())
	{
		Subsystem->UnregisterSource(this);
	}
	Super::EndPlay(EndPlayReason);
}

void UOrganicSoundSourceComponent::PlaySound()
{
	if (Audio) Audio->Play();

	if (bEmitAcousticEventOnPlay)
	{
		if (UOrganicReverbSubsystem* Subsystem = GetWorld()->GetSubsystem<UOrganicReverbSubsystem>())
		{
			Subsystem->EmitAcousticEvent(GetSourceLocation(), EventIntensity);
		}
	}
}

FVector UOrganicSoundSourceComponent::GetSourceLocation() const
{
	if (Audio) return Audio->GetComponentLocation();
	return GetOwner() ? GetOwner()->GetActorLocation() : FVector::ZeroVector;
}

void UOrganicSoundSourceComponent::ApplyPropagation(float DeltaTime, const Acoustic::FPropagationPath& Path, float ReverbCouplingDb,
	const Acoustic::FSourceOutput& Output, USoundSubmixBase* ReverbSubmix)
{
	LastPath = Path;

	// dB와 로그 주파수 공간에서 스무딩 (선형으로 섞으면 작은 값 쪽 전환이 부자연스러움)
	const float TargetLowpassLog2 = FMath::Log2(Output.LowpassHz);
	if (!bHasSmoothed)
	{
		SmoothedGainDb = Path.GainDb.Mid;
		SmoothedLowpassLog2 = TargetLowpassLog2;
		SmoothedSendDb = ReverbCouplingDb;
		bHasSmoothed = true;
	}
	else
	{
		const float Alpha = SmoothingTime > 0.f ? 1.f - FMath::Exp(-DeltaTime / SmoothingTime) : 1.f;
		SmoothedGainDb = FMath::Lerp(SmoothedGainDb, Path.GainDb.Mid, Alpha);
		SmoothedLowpassLog2 = FMath::Lerp(SmoothedLowpassLog2, TargetLowpassLog2, Alpha);
		SmoothedSendDb = FMath::Lerp(SmoothedSendDb, ReverbCouplingDb, Alpha);
	}

	Info.bValid = Path.bValid;
	Info.bOccluded = Path.bOccluded;
	Info.bClearPath = Path.bSameRoom || Path.bLineOfSight;
	Info.PortalCount = static_cast<int32>(Path.Portals.size());
	Info.DirectGainDb = SmoothedGainDb;
	Info.LowpassHz = FMath::Exp2(SmoothedLowpassLog2);
	Info.ReverbSendDb = SmoothedSendDb;

	if (Audio && Path.bValid)
	{
		if (bApplyOcclusion)
		{
			Audio->SetVolumeMultiplier(BaseVolume * FMath::Pow(10.f, SmoothedGainDb / 20.f));
			Audio->SetLowPassFilterEnabled(true);
			Audio->SetLowPassFilterFrequency(Info.LowpassHz);
		}
		if (bApplyReverbSend && ReverbSubmix)
		{
			Audio->SetSubmixSend(ReverbSubmix, BaseReverbSend * FMath::Pow(10.f, SmoothedSendDb / 20.f));
		}
	}

	// 상태가 바뀔 때만 로그 (디버그 / 헤드리스 검증용)
	const int32 State = !Path.bValid ? 0 : Path.bSameRoom ? 1 : Path.bLineOfSight ? 2 : Path.bOccluded ? 4 : 3;
	if (State != LastLoggedState)
	{
		static const TCHAR* StateNames[] = { TEXT("outside rooms"), TEXT("same room"), TEXT("line of sight"), TEXT("around portals"), TEXT("OCCLUDED") };
		LastLoggedState = State;
		UE_LOG(LogOrganicReverb, Log, TEXT("Source %s: %s, portals %d, walls %d, direct %.1f dB (High %.1f), LPF %.0f Hz, reverb send %.1f dB"),
			GetOwner() ? *GetOwner()->GetActorNameOrLabel() : TEXT("?"), StateNames[State], Info.PortalCount, Path.WallsBetween,
			Path.GainDb.Mid, Path.GainDb.High, Output.LowpassHz, ReverbCouplingDb);
	}
}
