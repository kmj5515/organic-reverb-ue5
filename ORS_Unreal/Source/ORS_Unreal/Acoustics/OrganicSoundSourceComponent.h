// OrganicSoundSourceComponent.h
// ---------------------------------------------------------
// Unreal 어댑터: 음원별 전파 처리 (5단계). 같은 액터의 AudioComponent에 리스너까지의 경로를 반영한다.
//   - 직접음: 문을 돌아오는 경로의 감쇠 + 로우패스 (벽 너머면 크게 막힘) → Occlusion / Obstruction
//   - 잔향: 음원 방 → 리스너 방 잔향 결합만큼 Reverb Submix Send 조절 → Exclusion
// 계산은 UOrganicReverbSubsystem이 매 틱 해서 ApplyPropagation으로 넘겨준다.
// 주의: AudioComponent의 Attenuation에서 Reverb Send는 꺼 둘 것 (이 컴포넌트가 Send를 직접 넣는다).
// ---------------------------------------------------------
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "AcousticCore/AcousticPropagation.h"
#include "OrganicSoundSourceComponent.generated.h"

class UAudioComponent;
class USoundSubmixBase;

/** 블루프린트/디버그용 전파 결과 요약 */
USTRUCT(BlueprintType)
struct FOrganicPropagationInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Acoustics")
	bool bValid = false;

	/** 열린 문 경로가 없음 (벽 투과음만) */
	UPROPERTY(BlueprintReadOnly, Category = "Acoustics")
	bool bOccluded = false;

	/** 같은 방이거나 열린 문 너머로 직접 보임 */
	UPROPERTY(BlueprintReadOnly, Category = "Acoustics")
	bool bClearPath = false;

	UPROPERTY(BlueprintReadOnly, Category = "Acoustics")
	int32 PortalCount = 0;

	/** 직접음 추가 감쇠 (Mid, 스무딩 적용) */
	UPROPERTY(BlueprintReadOnly, Category = "Acoustics")
	float DirectGainDb = 0.f;

	UPROPERTY(BlueprintReadOnly, Category = "Acoustics")
	float LowpassHz = 20000.f;

	/** 음원 방 → 리스너 방 잔향 결합 */
	UPROPERTY(BlueprintReadOnly, Category = "Acoustics")
	float ReverbSendDb = 0.f;
};

UCLASS(ClassGroup = (Audio), meta = (BlueprintSpawnableComponent))
class UOrganicSoundSourceComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UOrganicSoundSourceComponent();

	/** 직접음에 경로 감쇠 + 로우패스 적용 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Acoustics")
	bool bApplyOcclusion = true;

	/** 잔향 결합만큼 Reverb Submix Send 적용 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Acoustics")
	bool bApplyReverbSend = true;

	/** 리스너와 같은 방일 때의 Reverb Submix Send 양 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Acoustics", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float BaseReverbSend = 0.5f;

	/** 파라미터 전환 시간 (리스너가 움직일 때 찌직거림 방지) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Acoustics", meta = (ClampMin = "0.0", Units = "s"))
	float SmoothingTime = 0.1f;

	/** 0보다 크면 이 간격마다 사운드를 다시 재생 (청취 테스트용) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Acoustics|Test", meta = (ClampMin = "0.0", Units = "s"))
	float RepeatInterval = 0.f;

	/** 재생할 때마다 음원 위치에 음향 이벤트(에너지)도 주입 → 히트맵에 파동이 보임 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Acoustics|Test")
	bool bEmitAcousticEventOnPlay = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Acoustics|Test", meta = (ClampMin = "0.0"))
	float EventIntensity = 30.f;

	UFUNCTION(BlueprintCallable, Category = "Acoustics")
	void PlaySound();

	UFUNCTION(BlueprintPure, Category = "Acoustics")
	FOrganicPropagationInfo GetPropagationInfo() const { return Info; }

	/** 서브시스템이 매 틱 호출 */
	void ApplyPropagation(float DeltaTime, const Acoustic::FPropagationPath& Path, float ReverbCouplingDb,
		const Acoustic::FSourceOutput& Output, USoundSubmixBase* ReverbSubmix);

	FVector GetSourceLocation() const;
	const Acoustic::FPropagationPath& GetLastPath() const { return LastPath; }

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	UPROPERTY()
	TObjectPtr<UAudioComponent> Audio;

	float BaseVolume = 1.f;
	bool bHasSmoothed = false;
	float SmoothedGainDb = 0.f;
	float SmoothedLowpassLog2 = 0.f;
	float SmoothedSendDb = 0.f;
	int32 LastLoggedState = -1;

	FOrganicPropagationInfo Info;
	Acoustic::FPropagationPath LastPath;
	FTimerHandle RepeatTimer;
};
