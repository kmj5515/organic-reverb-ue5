// AcousticGridScanner.cpp
#include "Acoustics/AcousticGridScanner.h"
#include "Components/PrimitiveComponent.h"
#include "Engine/OverlapResult.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeExit.h"
#include "Materials/MaterialInterface.h"
#include "PhysicsEngine/BodyInstance.h"

bool FAcousticGridScanner::ResolveAbsorption(const UPrimitiveComponent* Component, FAcousticAbsorption& OutAbsorption, const UObject*& OutKey, FString& OutName)
{
	if (!Component) return false;

	if (const UAcousticWallComponent* Wall = Cast<UAcousticWallComponent>(Component))
	{
		OutAbsorption = Wall->Absorption;
		OutKey = Wall;
		OutName = Wall->GetName();
		return true;
	}

	// Phys Material Override / 바디 셋업 → 메시 머티리얼의 Phys Material 순
	const UAcousticPhysicalMaterial* PhysMat = nullptr;
	if (const FBodyInstance* Body = Component->GetBodyInstance())
	{
		PhysMat = Cast<UAcousticPhysicalMaterial>(Body->GetSimplePhysicalMaterial());
	}
	if (!PhysMat)
	{
		if (const UMaterialInterface* Material = Component->GetMaterial(0))
		{
			PhysMat = Cast<UAcousticPhysicalMaterial>(Material->GetPhysicalMaterial());
		}
	}
	if (!PhysMat) return false;

	OutAbsorption = PhysMat->Absorption;
	OutKey = PhysMat;
	OutName = PhysMat->GetName();
	return true;
}

FVector FAcousticGridScanJob::CellCenter(int32 X, int32 Y, int32 Z) const
{
	return Settings.Bounds.Min + (FVector(X, Y, Z) + 0.5) * Settings.CellSize;
}

// 오버랩 결과 중 흡음이 가장 큰 재질의 격자 값 (없으면 Air).
// 흡음이 큰 쪽 우선: 결과가 오버랩 순서에 좌우되지 않게 하고, 흡음재는 보통 구조체 위에 덧대기 때문
uint8 FAcousticGridScanJob::ResolveValue(const UWorld& World, const FVector& Center, const FCollisionShape& Shape)
{
	using Acoustic::FOccupancyGrid;

	TArray<FOverlapResult> Overlaps;
	World.OverlapMultiByObjectType(Overlaps, Center, FQuat::Identity, ObjectParams, Shape, QueryParams);

	bool bFound = false;
	FAcousticAbsorption BestAbsorption;
	const UObject* BestKey = nullptr;
	FString BestName;
	for (const FOverlapResult& Overlap : Overlaps)
	{
		FAcousticAbsorption Absorption;
		const UObject* Key = nullptr;
		FString Name;
		if (!FAcousticGridScanner::ResolveAbsorption(Overlap.GetComponent(), Absorption, Key, Name)) continue;
		if (!bFound || Absorption.Mid > BestAbsorption.Mid || (Absorption.Mid == BestAbsorption.Mid && Name < BestName))
		{
			bFound = true;
			BestAbsorption = Absorption;
			BestKey = Key;
			BestName = Name;
		}
	}
	if (!bFound) return FOccupancyGrid::Air;

	if (const uint8* Found = MaterialValues.Find(BestKey)) return *Found;
	const uint8 Added = Grid.AddMaterial(BestAbsorption.ToCore(BestName));
	const uint8 Value = Added != FOccupancyGrid::Air ? Added : DefaultValue; // 255개 초과 시 기본 재질
	MaterialValues.Add(BestKey, Value);
	return Value;
}

bool FAcousticGridScanJob::Begin(const UWorld& World, const FAcousticScanSettings& InSettings)
{
	Phase = EPhase::Done;
	Settings = InSettings;
	Stats = FAcousticScanStats();
	MaterialValues.Reset();
	Cursor = 0;
	StartTime = FPlatformTime::Seconds();

	if (!Settings.Bounds.IsValid || Settings.CellSize < 1.f) return false;

	const FVector Size = Settings.Bounds.GetSize();
	const int32 SX = FMath::Max(1, FMath::CeilToInt32(Size.X / Settings.CellSize));
	const int32 SY = FMath::Max(1, FMath::CeilToInt32(Size.Y / Settings.CellSize));
	const int32 SZ = FMath::Max(1, FMath::CeilToInt32(Size.Z / Settings.CellSize));
	const int64 NumCells = static_cast<int64>(SX) * SY * SZ;
	if (NumCells > Settings.MaxCells)
	{
		UE_LOG(LogOrganicReverb, Warning, TEXT("Scan refused: %lld cells > MaxCells %lld. Increase CellSize or shrink the volume."), NumCells, Settings.MaxCells);
		return false;
	}

	Grid.Init(SX, SY, SZ, static_cast<float>(Settings.CellSize * OrganicReverb::CmToM), OrganicReverb::ToCore(Settings.Bounds.Min));
	Grid.Materials.clear();
	DefaultValue = Grid.AddMaterial(Settings.DefaultAbsorption.ToCore(TEXT("Default")));

	ObjectParams = FCollisionObjectQueryParams(Settings.ObjectType);
	QueryParams = FCollisionQueryParams(SCENE_QUERY_STAT(OrganicReverbScan), false);
	for (const AActor* Ignored : Settings.IgnoredActors) QueryParams.AddIgnoredActor(Ignored);

	Stats.NumCells = NumCells;
	Phase = EPhase::Occupancy;
	return true;
}

float FAcousticGridScanJob::GetProgress() const
{
	if (Phase == EPhase::Done) return 1.f;
	if (Stats.NumCells <= 0) return 0.f;
	// 두 단계를 절반씩으로 친다 (표면 단계는 고체 칸만 훑지만 칸당 비용이 커서 대략 비슷하다)
	const float Within = static_cast<float>(Cursor) / static_cast<float>(Stats.NumCells);
	return Phase == EPhase::Occupancy ? 0.5f * Within : 0.5f + 0.5f * Within;
}

bool FAcousticGridScanJob::Step(const UWorld& World, double TimeBudgetSeconds)
{
	using Acoustic::FOccupancyGrid;
	if (Phase == EPhase::Done) return true;

	const double StepStart = FPlatformTime::Seconds();
	++Stats.Steps;
	ON_SCOPE_EXIT{ Stats.WorkSeconds += FPlatformTime::Seconds() - StepStart; };

	const double Deadline = StepStart + TimeBudgetSeconds;
	// 시간 확인 자체도 비용이라 일정 칸마다만 본다
	constexpr int64 CheckInterval = 256;

	// 칸보다 살짝 작은 박스: 칸 경계에 딱 붙은 벽이 양쪽 칸을 모두 막지 않도록
	const FCollisionShape CellShape = FCollisionShape::MakeBox(FVector(Settings.CellSize * 0.49f));

	const int64 Layer = static_cast<int64>(Grid.SizeX) * Grid.SizeY;
	auto ToCoord = [&](int64 I, int32& X, int32& Y, int32& Z)
	{
		X = static_cast<int32>(I % Grid.SizeX);
		Y = static_cast<int32>((I % Layer) / Grid.SizeX);
		Z = static_cast<int32>(I / Layer);
	};

	// 1) 점유: 공기 / 고체
	if (Phase == EPhase::Occupancy)
	{
		while (Cursor < Stats.NumCells)
		{
			int32 X, Y, Z;
			ToCoord(Cursor, X, Y, Z);
			if (World.OverlapAnyTestByObjectType(CellCenter(X, Y, Z), FQuat::Identity, ObjectParams, CellShape, QueryParams))
			{
				Grid.Cells[Grid.Index(X, Y, Z)] = DefaultValue;
				++Stats.SolidCells;
			}
			++Cursor;
			if (TimeBudgetSeconds > 0.0 && (Cursor % CheckInterval) == 0 && FPlatformTime::Seconds() >= Deadline) return false;
		}
		Phase = EPhase::Surface;
		Cursor = 0;
	}

	// 2) 재질: 공기와 맞닿은 고체 칸(= 소리가 부딪히는 표면)만 조회
	// 면 재질은 공기 쪽 면에 붙은 얇은 슬랩만 검사 → 한 칸에 벽 두 개가 들어가도 각 면의 마감재를 구분
	const float Cell = Settings.CellSize;
	const float SlabHalf = Cell * 0.05f;
	const float SlabOffset = Cell * 0.5f - SlabHalf - Cell * 0.01f;

	while (Cursor < Stats.NumCells)
	{
		int32 X, Y, Z;
		ToCoord(Cursor, X, Y, Z);
		++Cursor;

		const size_t I = Grid.Index(X, Y, Z);
		if (!Grid.IsAir(I))
		{
			bool bAirFace[6] = {};
			bool bSurface = false;
			for (int32 Face = 0; Face < 6; ++Face)
			{
				const int* D = Acoustic::FaceOffsets[Face];
				bAirFace[Face] = Grid.IsInside(X + D[0], Y + D[1], Z + D[2]) && Grid.IsAir(Grid.Index(X + D[0], Y + D[1], Z + D[2]));
				bSurface |= bAirFace[Face];
			}
			if (bSurface)
			{
				++Stats.SurfaceCells;

				const FVector Center = CellCenter(X, Y, Z);
				const uint8 CellValue = ResolveValue(World, Center, CellShape);
				if (CellValue != FOccupancyGrid::Air) Grid.Cells[I] = CellValue;

				for (int32 Face = 0; Face < 6; ++Face)
				{
					if (!bAirFace[Face]) continue;
					const int* D = Acoustic::FaceOffsets[Face];
					const FVector Dir(D[0], D[1], D[2]);
					const FVector Axis = Dir.GetAbs();
					const FVector HalfExtent = Axis * SlabHalf + (FVector::OneVector - Axis) * (Cell * 0.45f);
					const uint8 FaceValue = ResolveValue(World, Center + Dir * SlabOffset, FCollisionShape::MakeBox(HalfExtent));
					if (FaceValue != FOccupancyGrid::Air && FaceValue != Grid.Cells[I])
					{
						Grid.SetFaceMaterial(I, Face, FaceValue);
						++Stats.FaceOverrides;
					}
				}
			}
		}

		if (TimeBudgetSeconds > 0.0 && (Cursor % CheckInterval) == 0 && FPlatformTime::Seconds() >= Deadline) return false;
	}

	Stats.NumMaterials = static_cast<int32>(Grid.Materials.size());
	Stats.Seconds = FPlatformTime::Seconds() - StartTime;
	Phase = EPhase::Done;
	return true;
}

bool FAcousticGridScanner::Scan(const UWorld& World, const FAcousticScanSettings& Settings, Acoustic::FOccupancyGrid& OutGrid, FAcousticScanStats* OutStats)
{
	FAcousticGridScanJob Job;
	if (!Job.Begin(World, Settings)) return false;
	Job.Step(World, 0.0); // 예산 0 = 끝까지
	OutGrid = MoveTemp(Job.GetGrid());
	if (OutStats) *OutStats = Job.GetStats();
	return true;
}
