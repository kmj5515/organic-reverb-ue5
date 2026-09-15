// AcousticGeometryScanner.h
// ---------------------------------------------------------
// Unreal 어댑터 레이어: 여기서만 Unreal 타입(UObject, FVector, FHitResult 등)을 쓴다.
// 하는 일은 딱 하나 — 레벨 지오메트리를 레이캐스트로 스캔해서
// Core의 FAcousticRoomGraph를 채워 넣는 것.
//
// 알고리즘 로직은 여기 없다. 여기 있으면 안 된다 (Core로 이관).
// ---------------------------------------------------------
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Core/AcousticRoomGraph.h"
#include "AcousticGeometryScanner.generated.h"

UCLASS(ClassGroup = (Audio), meta = (BlueprintSpawnableComponent))
class UAcousticGeometryScanner : public UActorComponent
{
	GENERATED_BODY()

public:
	// 씬 안의 방들을 스캔해서 그래프를 새로 빌드한다.
	// 보통 레벨 로드 시 1회, 혹은 파괴 가능한 벽이 있다면 이벤트 발생 시 재호출.
	UFUNCTION(BlueprintCallable, Category = "Acoustics")
	void ScanLevelAndBuildGraph(Acoustic::FAcousticRoomGraph& OutGraph);

	// 특정 위치가 어느 방(RoomId)에 속하는지 조회.
	// 발사 지점, 플레이어 위치 등을 에너지 확산 시뮬레이터에 매핑할 때 사용.
	UFUNCTION(BlueprintCallable, Category = "Acoustics")
	int32 FindRoomIdAtLocation(const FVector& WorldLocation) const;

protected:
	// 방 하나의 부피/표면적을 추정하기 위해 6방향(위/아래/좌/우/앞/뒤)으로
	// 레이를 쏴서 벽까지의 거리를 재는 헬퍼.
	// 정밀한 방식은 아니지만, 실시간 근사치로는 충분함 (Sabine 공식 자체가 근사식).
	bool ProbeRoomBounds(const FVector& SeedLocation, FVector& OutExtent, float& OutVolume, float& OutSurfaceArea) const;

	// 히트된 피직컬 머티리얼을 Acoustic::FAcousticMaterial로 변환.
	// 프로젝트의 Physical Material에 커스텀 데이터(흡음계수)를 등록해두고 여기서 읽어온다.
	Acoustic::FAcousticMaterial ResolveAcousticMaterial(const FHitResult& Hit) const;

	// 두 방 사이의 개구부(문/복도)를 감지 — 예: 두 방의 경계에 콜리전이 없는 영역을 스캔
	bool DetectPortalBetween(const FVector& RoomACenter, const FVector& RoomBCenter, float& OutOpeningArea) const;

	UPROPERTY(EditAnywhere, Category = "Acoustics")
	float ProbeRayMaxDistance = 3000.f; // cm 단위, 레이가 뻗어나갈 최대 거리

	UPROPERTY(EditAnywhere, Category = "Acoustics")
	TEnumAsByte<ECollisionChannel> AcousticTraceChannel = ECC_Visibility;
};
