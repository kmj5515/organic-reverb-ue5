// OrganicTestSoundSource.h
// ---------------------------------------------------------
// 청취 테스트용 음원 액터: 1 kHz 핑을 일정 간격으로 반복 재생하고, 음원별 전파(차폐/잔향 Send)를 적용한다.
// 씬 4(벽 너머 소리)에서 문을 열고 닫거나 리스너가 움직일 때 소리가 어떻게 바뀌는지 확인하는 용도.
// ---------------------------------------------------------
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "OrganicTestSoundSource.generated.h"

class UAudioComponent;
class UOrganicSoundSourceComponent;
class UStaticMeshComponent;

UCLASS(meta = (DisplayName = "Organic Test Sound Source"))
class AOrganicTestSoundSource : public AActor
{
	GENERATED_BODY()

public:
	AOrganicTestSoundSource();

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Acoustics")
	TObjectPtr<UAudioComponent> Audio;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Acoustics")
	TObjectPtr<UOrganicSoundSourceComponent> OrganicSource;

	/** 위치 표시용 구 (콜리전 없음) */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Acoustics")
	TObjectPtr<UStaticMeshComponent> Marker;
};
