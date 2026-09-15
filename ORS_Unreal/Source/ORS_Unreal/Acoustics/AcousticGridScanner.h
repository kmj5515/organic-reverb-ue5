// AcousticGridScanner.h
// ---------------------------------------------------------
// Unreal 어댑터: 월드 지오메트리를 Core 점유 격자(m 단위)로 변환한다. 알고리즘은 여기 없다.
//   1) 칸마다 박스 오버랩 테스트 → 공기 / 고체
//   2) 공기와 맞닿은 고체 칸만 오버랩 결과에서 재질을 읽는다
//      (UAcousticWallComponent → UAcousticPhysicalMaterial → 기본값 순)
//   3) 그 칸의 공기 쪽 면마다 얇은 슬랩으로 다시 조회해 면 재질을 기록한다 (벽 양쪽 마감재가 다른 경우)
// ---------------------------------------------------------
#pragma once

#include "CoreMinimal.h"
#include "CollisionQueryParams.h"
#include "CollisionShape.h"
#include "Engine/EngineTypes.h"
#include "Acoustics/AcousticSurfaceTypes.h"
#include "AcousticCore/AcousticOccupancyGrid.h"

class AActor;
class UPrimitiveComponent;
class UWorld;

struct FAcousticScanSettings
{
	FBox Bounds = FBox(ForceInit);  // 월드, cm
	float CellSize = 50.f;          // cm
	ECollisionChannel ObjectType = ECC_WorldStatic;
	FAcousticAbsorption DefaultAbsorption;
	TArray<const AActor*> IgnoredActors;
	int64 MaxCells = 8000000;       // 이보다 크면 스캔 거부 (메모리/시간 보호)
};

struct FAcousticScanStats
{
	int64 NumCells = 0;
	int64 SolidCells = 0;
	int64 SurfaceCells = 0;
	int64 FaceOverrides = 0; // 칸 재질과 다른 면 재질이 기록된 면 수 (양쪽 마감재가 다른 벽)
	int32 NumMaterials = 0;
	int32 Steps = 0;         // 몇 번에 나눠 진행했는가 (= 몇 프레임에 걸쳤는가, 한 번에 하면 1)
	double Seconds = 0.0;    // 시작부터 끝까지 걸린 시간. 나눠 하면 대기 시간도 포함된다
	double WorkSeconds = 0.0; // 실제로 스캔에 쓴 시간의 합
};

/**
 * 여러 프레임에 걸쳐 진행하는 스캔 (6-4).
 * 오버랩 쿼리는 물리 씬을 읽으므로 게임 스레드에서만 할 수 있다. 대신 한 프레임에 정해진 시간만 일하고
 * 멈췄다가 다음 프레임에 이어서 한다 → 레벨 로드 / ors.Rebuild 때 프레임이 통째로 멈추지 않는다.
 *
 *   Begin()으로 격자를 잡고, 끝날 때까지 매 프레임 Step()을 부른다.
 */
class FAcousticGridScanJob
{
public:
	enum class EPhase : uint8
	{
		Occupancy, // 칸마다 공기 / 고체 판정
		Surface,   // 공기와 맞닿은 고체 칸의 재질 + 면 재질
		Done
	};

	/** 격자를 할당하고 스캔을 시작한다. 칸 수가 MaxCells를 넘으면 false */
	bool Begin(const UWorld& World, const FAcousticScanSettings& Settings);

	/** TimeBudgetSeconds 만큼만 진행한다. 스캔이 끝나면 true (0 이하면 끝까지) */
	bool Step(const UWorld& World, double TimeBudgetSeconds);

	bool IsRunning() const { return Phase != EPhase::Done; }
	float GetProgress() const;

	Acoustic::FOccupancyGrid& GetGrid() { return Grid; }
	const FAcousticScanStats& GetStats() const { return Stats; }

private:
	uint8 ResolveValue(const UWorld& World, const FVector& Center, const FCollisionShape& Shape);
	FVector CellCenter(int32 X, int32 Y, int32 Z) const;

	FAcousticScanSettings Settings;
	Acoustic::FOccupancyGrid Grid;
	FAcousticScanStats Stats;

	// 표면 칸마다 최대 7번씩 불리므로 미리 만들어 둔다 (무시 액터 목록 복사 비용)
	FCollisionObjectQueryParams ObjectParams;
	FCollisionQueryParams QueryParams;

	EPhase Phase = EPhase::Done;
	int64 Cursor = 0; // 다음에 처리할 칸 번호 (단계마다 0부터)
	uint8 DefaultValue = 0;
	TMap<const UObject*, uint8> MaterialValues;
	double StartTime = 0.0;
};

class FAcousticGridScanner
{
public:
	/** 한 번에 끝까지 스캔한다 (자동화 테스트 · 즉시 재생성용) */
	static bool Scan(const UWorld& World, const FAcousticScanSettings& Settings, Acoustic::FOccupancyGrid& OutGrid, FAcousticScanStats* OutStats = nullptr);

	/** 컴포넌트의 흡음계수 조회. 찾으면 true + 캐시 키(재질을 공유하는 오브젝트)와 이름 반환 */
	static bool ResolveAbsorption(const UPrimitiveComponent* Component, FAcousticAbsorption& OutAbsorption, const UObject*& OutKey, FString& OutName);
};
