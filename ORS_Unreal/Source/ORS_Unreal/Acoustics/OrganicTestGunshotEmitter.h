// OrganicTestGunshotEmitter.h
// ---------------------------------------------------------
// 청취 테스트용 원거리 총기: 일정 간격으로 UOrganicReverbSubsystem::PlayGunshotAtLocation을 호출한다.
// 멀리 떨어진 복도 끝에 두고, 리스너 위치에 따라 도착 지연 / 들리는 방향 / 거리감 / 잔향 비율이 어떻게 바뀌는지 확인.
// 총성 음원은 NearSounds(근거리 녹음), FarSounds(원거리 녹음, 선택)로 지정. 여러 개면 무작위. 기본값은 엔진 핑 사운드.
// ---------------------------------------------------------
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "OrganicTestGunshotEmitter.generated.h"

class USoundBase;
class UStaticMeshComponent;

UCLASS(meta = (DisplayName = "Organic Test Gunshot Emitter"))
class AOrganicTestGunshotEmitter : public AActor
{
	GENERATED_BODY()

public:
	AOrganicTestGunshotEmitter();

	/** 근거리에서 녹음한 총성. 여러 개면 발사할 때마다 무작위로 고른다 (같은 소리 반복 방지) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gunshot")
	TArray<TObjectPtr<USoundBase>> NearSounds;

	/** 원거리에서 녹음한 총성 (선택, 여러 개면 무작위). 거리에 따라 근거리 총성과 섞인다 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gunshot")
	TArray<TObjectPtr<USoundBase>> FarSounds;

	/** 발사 간격 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gunshot", meta = (ClampMin = "0.2", Units = "s"))
	float Interval = 3.f;

	/** 음향 이벤트 세기 (에너지 주입량, 음수 = 볼륨 기본값) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Gunshot")
	float Intensity = -1.f;

	UFUNCTION(BlueprintCallable, Category = "Gunshot")
	void Fire();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Gunshot")
	TObjectPtr<UStaticMeshComponent> Marker;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	FTimerHandle FireTimer;
};
