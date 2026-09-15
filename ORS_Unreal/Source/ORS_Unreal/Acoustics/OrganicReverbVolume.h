// OrganicReverbVolume.h
// ---------------------------------------------------------
// Unreal 어댑터: 오가닉 리버브 적용 영역. 레벨에 하나 배치하고 박스로 스캔 범위를 덮는다.
// BeginPlay 시 UOrganicReverbSubsystem에 등록 → 격자 스캔 → 방 분할 → 시뮬레이션 시작.
// ---------------------------------------------------------
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Engine/EngineTypes.h"
#include "Acoustics/AcousticSurfaceTypes.h"
#include "OrganicReverbVolume.generated.h"

class UBoxComponent;
class USoundBase;
class USubmixEffectReverbPreset;

UCLASS(meta = (DisplayName = "Organic Reverb Volume"))
class AOrganicReverbVolume : public AActor
{
	GENERATED_BODY()

public:
	AOrganicReverbVolume();

	/** 스캔 범위 (월드 AABB, cm) */
	FBox GetScanBounds() const;

	/** 스캔 범위를 월드 박스에 정확히 맞춘다 (테스트 레이아웃 자동 맞춤용) */
	void SetScanBounds(const FBox& WorldBox);

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Acoustics")
	TObjectPtr<UBoxComponent> ScanBox;

	/** 격자 칸 크기. 작을수록 정밀하지만 스캔 비용은 세제곱으로 늘어난다 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Acoustics|Scan", meta = (ClampMin = "10.0", Units = "cm"))
	float CellSize = 50.f;

	/** 벽으로 인식할 오브젝트 타입. 기본 WorldStatic → 움직이는 문/소품은 개구부로 취급된다 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Acoustics|Scan")
	TEnumAsByte<ECollisionChannel> ScanObjectType = ECC_WorldStatic;

	/**
	 * 백그라운드 스캔이 한 프레임에 쓸 시간 (ms). 스캔은 물리 씬을 읽어야 해서 게임 스레드에서만 할 수 있으므로,
	 * 이 예산만큼만 진행하고 다음 프레임에 이어서 한다. 크게 잡으면 빨리 끝나지만 프레임이 튄다.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Acoustics|Scan", meta = (ClampMin = "0.1", ClampMax = "50.0", Units = "ms"))
	float ScanTimeBudgetMs = 2.f;

	/** Acoustic 재질 정보가 없는 표면의 흡음계수 (기본: 콘크리트) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Acoustics|Scan")
	FAcousticAbsorption DefaultAbsorption;

	/** 스캔 범위 바깥(열린 하늘 등)과 맞닿은 면의 흡음계수 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Acoustics|Scan")
	FAcousticAbsorption OutsideAbsorption;

	/** 이 폭 이하의 좁은 개구부는 방 경계(문)로 본다 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Acoustics|Segmentation", meta = (ClampMin = "50.0", Units = "cm"))
	float MaxPortalWidth = 200.f;

	/** 이보다 긴 공간은 여러 방으로 나눈다 (0 = 분할 안 함). 작을수록 전파가 세밀해진다 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Acoustics|Segmentation", meta = (ClampMin = "0.0", Units = "cm"))
	float MaxRoomLength = 800.f;

	/** 이보다 작은 조각(m^3)은 이웃 방에 병합 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Acoustics|Segmentation", meta = (ClampMin = "0.0"))
	float MinRoomVolume = 2.f;

	/**
	 * 공기 흡음의 세기 배율. 1 = 실측값(ISO 9613-1, 20 °C · 습도 50 %), 0 = 끔(벽 흡음만 쓰는 순수 Sabine).
	 * 벽에 닿지 않아도 공기 중에서 잃는 에너지라 큰 공간일수록, 고음일수록 크게 작용한다.
	 * (30 m 홀 기준 Mid 잔향 약 30 %, High 약 60 % 짧아짐) 건조/습한 공기를 흉내내려면 이 값을 조정한다.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Acoustics|Simulation", meta = (ClampMin = "0.0", ClampMax = "4.0"))
	float AirAbsorptionScale = 1.f;

	/**
	 * 방 사이 에너지 전달 지연. 켜면 포탈을 지난 에너지가 "방 중심 → 문 → 옆방 중심" 거리를
	 * 음속으로 지나는 시간만큼 늦게 도착한다 (복도를 따라 소리가 번져 가는 게 보인다).
	 * 끄면 예전처럼 즉시 전달된다 (거리에 따라 작아지기만 하고 늦어지지는 않음).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Acoustics|Simulation")
	bool bPortalDelay = true;

	/** 파라미터를 적용할 Submix Reverb 프리셋 (Reverb Submix 이펙트 체인에 들어있는 프리셋) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Acoustics|Output")
	TObjectPtr<USubmixEffectReverbPreset> ReverbPreset;

	/** 이벤트가 없을 때의 기본 Wet Level */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Acoustics|Output", meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float BaseWetLevel = 0.3f;

	/** 이벤트(총성) 에너지가 리스너 방에 도달했을 때 더해지는 최대 Wet Level */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Acoustics|Output", meta = (ClampMin = "0.0", ClampMax = "10.0"))
	float EventWetBoost = 0.7f;

	/** 리스너가 방을 옮길 때 파라미터 전환 시간 (s). 이벤트 Wet 상승은 즉시, 하강에만 적용 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Acoustics|Output", meta = (ClampMin = "0.0", Units = "s"))
	float ParameterSmoothingTime = 0.25f;

	/** 이 에너지 밀도(3대역 합)에서 이벤트 Wet = 1. 여기서 -60dB 아래면 0 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Acoustics|Output", meta = (ClampMin = "0.0001"))
	float EventReferenceDensity = 1.f;

	/** EmitAcousticEvent 세기 기본값 (대역별 주입 에너지) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Acoustics|Output", meta = (ClampMin = "0.0"))
	float DefaultEventIntensity = 100.f;

	/** ors.Shot 테스트 총성에 쓸 사운드 (여러 개면 무작위). 원거리 총성 경로로 재생된다. 비우면 엔진 핑 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Acoustics|Gunshot")
	TArray<TObjectPtr<USoundBase>> ShotSounds;

	/** 원거리 총성: 이 반경 안에서는 거리 감쇠 없음. 밖에서는 거리 2배당 −6 dB */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Acoustics|Gunshot", meta = (ClampMin = "10.0", Units = "cm"))
	float GunshotInnerRadius = 500.f;

	/** 이 거리까지는 근거리 녹음만 (문을 돌아오면 경로 길이 기준) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Acoustics|Gunshot", meta = (ClampMin = "0.0", Units = "cm"))
	float GunshotNearLayerDistance = 1000.f;

	/** 이 거리부터는 원거리 녹음만. 사이는 동일 파워 크로스페이드 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Acoustics|Gunshot", meta = (ClampMin = "0.0", Units = "cm"))
	float GunshotFarLayerDistance = 5000.f;

	/** 같은 방 가까이서 쐈을 때의 리버브 Send. 멀어져 직접음이 작아져도 잔향 크기는 유지되도록 보정한다 (직접음/잔향 비율) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Acoustics|Gunshot", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float GunshotBaseReverbSend = 0.15f;

	/** 시작 시 디버그 히트맵 표시 (콘솔: ors.Debug 1) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Acoustics|Debug")
	bool bDrawDebugOnBeginPlay = true;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
};
