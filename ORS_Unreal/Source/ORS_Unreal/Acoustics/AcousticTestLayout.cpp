// AcousticTestLayout.cpp
#include "Acoustics/AcousticTestLayout.h"
#include "Acoustics/OrganicReverbSubsystem.h"
#include "Acoustics/OrganicReverbVolume.h"
#include "Engine/CollisionProfile.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"

namespace AcousticTestLayoutPresets
{
	using EMat = EAcousticMaterialPreset;
	using ESide = EAcousticWallSide;

	static constexpr EMat AllMaterials[] = { EMat::Concrete, EMat::Wood, EMat::Carpet, EMat::Glass };
	static const TCHAR* const ComponentNames[] = { TEXT("Walls_Concrete"), TEXT("Walls_Wood"), TEXT("Walls_Carpet"), TEXT("Walls_Glass") };
	static const FLinearColor DisplayColors[] = { FLinearColor(0.5f, 0.5f, 0.5f), FLinearColor(0.45f, 0.28f, 0.12f), FLinearColor(0.55f, 0.1f, 0.1f), FLinearColor(0.6f, 0.8f, 0.9f) };

	static FAcousticTestDoor Door(ESide Side, float Offset, float Width = 100.f, float Height = 200.f)
	{
		FAcousticTestDoor D;
		D.Side = Side;
		D.Offset = Offset;
		D.Width = Width;
		D.Height = Height;
		return D;
	}

	static FAcousticTestRoom Room(const TCHAR* Name, const FVector& Origin, const FVector& Size, EMat Material,
		TArray<FAcousticTestDoor> Doors = {}, bool bCeiling = true)
	{
		FAcousticTestRoom R;
		R.Name = Name;
		R.Origin = Origin;
		R.Size = Size;
		R.Material = Material;
		R.Doors = MoveTemp(Doors);
		R.bCeiling = bCeiling;
		return R;
	}

	static TArray<FAcousticTestRoom> ModularRoom()
	{
		return { Room(TEXT("ModularRoom"), FVector::ZeroVector, FVector(600, 500, 300), EMat::Concrete, { Door(ESide::NegY, 250) }) };
	}
}

AAcousticTestLayout::AAcousticTestLayout()
{
	using namespace AcousticTestLayoutPresets;
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	CubeMesh = Cube.Object;

	for (int32 I = 0; I < UE_ARRAY_COUNT(AllMaterials); ++I)
	{
		UAcousticWallComponent* Walls = CreateDefaultSubobject<UAcousticWallComponent>(ComponentNames[I]);
		Walls->SetupAttachment(RootComponent);
		Walls->SetStaticMesh(CubeMesh);
		Walls->SetMobility(EComponentMobility::Movable);
		Walls->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName); // WorldStatic → 스캐너가 벽으로 인식
		Walls->Absorption = FAcousticAbsorption::FromPreset(AllMaterials[I]);
		WallComponents.Add(Walls);
	}

	Rooms = ModularRoom();
}

void AAcousticTestLayout::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	BuildGeometry();
}

void AAcousticTestLayout::BeginPlay()
{
	Super::BeginPlay();
	if (bAutoFitReverbVolume) FitReverbVolume();
}

void AAcousticTestLayout::RebuildLayout()
{
	BuildGeometry();
	if (bAutoFitReverbVolume) FitReverbVolume();

	UWorld* World = GetWorld();
	if (World && World->IsGameWorld())
	{
		if (UOrganicReverbSubsystem* Subsystem = World->GetSubsystem<UOrganicReverbSubsystem>()) Subsystem->RequestRebuild();
	}
}

void AAcousticTestLayout::SetRoomSize(int32 RoomIndex, FVector NewSize)
{
	if (!Rooms.IsValidIndex(RoomIndex)) return;
	Rooms[RoomIndex].Size = NewSize.ComponentMax(FVector(50.f));
	RebuildLayout();
}

void AAcousticTestLayout::SetRoomMaterial(int32 RoomIndex, EAcousticMaterialPreset NewMaterial)
{
	if (!Rooms.IsValidIndex(RoomIndex)) return;
	Rooms[RoomIndex].Material = NewMaterial;
	RebuildLayout();
}

FBox AAcousticTestLayout::GetLayoutBounds(float MinPadding) const
{
	const float Pad = FMath::Max(WallThickness, MinPadding);
	FBox Local(ForceInit);
	for (const FAcousticTestRoom& Room : Rooms)
	{
		Local += Room.Origin - FVector(Pad);
		Local += FVector(Room.Origin.X + Room.Size.X + Pad, Room.Origin.Y + Room.Size.Y + Pad,
			Room.Origin.Z + Room.Size.Z + (Room.bCeiling ? Pad : 0.f));
	}
	return Local.IsValid ? Local.TransformBy(GetActorTransform()) : Local;
}

void AAcousticTestLayout::FitReverbVolume()
{
	UWorld* World = GetWorld();
	if (!World || Rooms.IsEmpty()) return;

	AOrganicReverbVolume* ReverbVolume = nullptr;
	for (TActorIterator<AOrganicReverbVolume> It(World); It; ++It)
	{
		ReverbVolume = *It;
		break;
	}
	if (!ReverbVolume)
	{
		if (World->IsGameWorld()) return; // 런타임에는 새로 만들지 않는다
		ReverbVolume = World->SpawnActor<AOrganicReverbVolume>();
		if (!ReverbVolume) return;
	}

	ReverbVolume->Modify();
	ReverbVolume->SetScanBounds(GetLayoutBounds(ReverbVolume->CellSize));
}

void AAcousticTestLayout::BuildGeometry()
{
	using namespace AcousticTestLayoutPresets;

	UMaterialInterface* BaseMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
	for (int32 I = 0; I < WallComponents.Num(); ++I)
	{
		UAcousticWallComponent* Walls = WallComponents[I];
		if (!Walls) continue;
		Walls->ClearInstances();
		if (CubeMesh && Walls->GetStaticMesh() != CubeMesh) Walls->SetStaticMesh(CubeMesh);

		// 재질별 색 구분 (콘크리트 회색 / 나무 갈색 / 카펫 빨강 / 유리 하늘색)
		if (BaseMaterial && !Cast<UMaterialInstanceDynamic>(Walls->GetMaterial(0)) && I < UE_ARRAY_COUNT(DisplayColors))
		{
			UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(BaseMaterial, this);
			MID->SetVectorParameterValue(TEXT("Color"), DisplayColors[I]);
			Walls->SetMaterial(0, MID);
		}
	}

	const float T = WallThickness;
	for (const FAcousticTestRoom& Room : Rooms)
	{
		const FVector O = Room.Origin;
		const FVector S = Room.Size.ComponentMax(FVector(10.f));

		AddBox(Room.Material, FVector(O.X - T, O.Y - T, O.Z - T), FVector(O.X + S.X + T, O.Y + S.Y + T, O.Z)); // 바닥
		if (Room.bCeiling)
		{
			AddBox(Room.Material, FVector(O.X - T, O.Y - T, O.Z + S.Z), FVector(O.X + S.X + T, O.Y + S.Y + T, O.Z + S.Z + T));
		}
		for (ESide Side : { ESide::NegX, ESide::PosX, ESide::NegY, ESide::PosY })
		{
			AddWall(Room, Side);
		}
	}
}

void AAcousticTestLayout::AddBox(EAcousticMaterialPreset Material, const FVector& Min, const FVector& Max)
{
	const int32 Index = static_cast<int32>(Material);
	UAcousticWallComponent* Walls = WallComponents.IsValidIndex(Index) ? WallComponents[Index].Get() : nullptr;
	const FVector Size = Max - Min;
	if (!Walls || Size.GetMin() <= KINDA_SMALL_NUMBER) return;

	// BasicShapes/Cube = 100cm, 피벗 중앙
	Walls->AddInstance(FTransform(FRotator::ZeroRotator, (Min + Max) * 0.5, Size / 100.0));
}

void AAcousticTestLayout::AddWall(const FAcousticTestRoom& Room, EAcousticWallSide Side)
{
	using ESide = EAcousticWallSide;
	const FVector O = Room.Origin;
	const FVector S = Room.Size.ComponentMax(FVector(10.f));
	const float T = WallThickness;
	const bool bXWall = Side == ESide::NegX || Side == ESide::PosX;

	// U = 벽을 따라가는 축, N = 벽 두께 축. X벽은 모서리까지 덮고 Y벽은 방 내부 폭만 덮는다
	const double U0 = bXWall ? O.Y - T : O.X;
	const double U1 = bXWall ? O.Y + S.Y + T : O.X + S.X;
	const double UOrigin = bXWall ? O.Y : O.X;
	double N0 = 0.0;
	switch (Side)
	{
	case ESide::NegX: N0 = O.X - T; break;
	case ESide::PosX: N0 = O.X + S.X; break;
	case ESide::NegY: N0 = O.Y - T; break;
	case ESide::PosY: N0 = O.Y + S.Y; break;
	}
	const double N1 = N0 + T;
	const double Z0 = O.Z;
	const double Z1 = O.Z + S.Z;

	auto Emit = [&](double A0, double A1, double B0, double B1) // U 범위, Z 범위
	{
		if (A1 - A0 <= KINDA_SMALL_NUMBER || B1 - B0 <= KINDA_SMALL_NUMBER) return;
		const FVector Min = bXWall ? FVector(N0, A0, B0) : FVector(A0, N0, B0);
		const FVector Max = bXWall ? FVector(N1, A1, B1) : FVector(A1, N1, B1);
		AddBox(Room.Material, Min, Max);
	};

	TArray<FAcousticTestDoor> Doors = Room.Doors.FilterByPredicate([Side](const FAcousticTestDoor& D) { return D.Side == Side; });
	Doors.Sort([](const FAcousticTestDoor& A, const FAcousticTestDoor& B) { return A.Offset < B.Offset; });

	double Cursor = U0;
	for (const FAcousticTestDoor& Door : Doors)
	{
		const double D0 = FMath::Clamp(UOrigin + Door.Offset, U0, U1);
		const double D1 = FMath::Clamp(D0 + Door.Width, U0, U1);
		const double Top = FMath::Clamp(Z0 + Door.Height, Z0, Z1);
		Emit(Cursor, D0, Z0, Z1);                  // 문 앞 벽
		Emit(FMath::Max(Cursor, D0), D1, Top, Z1); // 문 위 인방
		Cursor = FMath::Max(Cursor, D1);
	}
	Emit(Cursor, U1, Z0, Z1);
}

// ---------------------------------------------------------
// 프리셋 (방 간격 50 cm, 벽 두께 25 cm → 공유 벽이 겹치지 않음. 모든 방 좌표 50 cm 배수 → 격자와 정렬)
// ---------------------------------------------------------

void AAcousticTestLayout::PresetModularRoom()
{
	WallThickness = 25.f;
	Rooms = AcousticTestLayoutPresets::ModularRoom();
	RebuildLayout();
}

void AAcousticTestLayout::PresetMaterialComparison()
{
	using namespace AcousticTestLayoutPresets;
	WallThickness = 25.f;
	Rooms = {
		Room(TEXT("Concrete"), FVector(0, 0, 0), FVector(500, 400, 300), EMat::Concrete, { Door(ESide::NegY, 200) }),
		Room(TEXT("Carpet"), FVector(550, 0, 0), FVector(500, 400, 300), EMat::Carpet, { Door(ESide::NegY, 200) }),
	};
	RebuildLayout();
}

void AAcousticTestLayout::PresetCorridorWithSideRooms()
{
	using namespace AcousticTestLayoutPresets;
	WallThickness = 25.f;
	TArray<FAcousticTestDoor> CorridorDoors = { Door(ESide::NegX, 100) }; // 복도 입구
	TArray<FAcousticTestRoom> NewRooms;
	for (const float X : { 150.f, 950.f, 1750.f })
	{
		CorridorDoors.Add(Door(ESide::PosY, X + 150.f));
		NewRooms.Add(Room(*FString::Printf(TEXT("Side_%d"), NewRooms.Num() + 1), FVector(X, 300, 0), FVector(400, 400, 300), EMat::Wood, { Door(ESide::NegY, 150) }));
	}
	NewRooms.Insert(Room(TEXT("Corridor"), FVector(0, 0, 0), FVector(2400, 250, 300), EMat::Concrete, CorridorDoors), 0);
	Rooms = MoveTemp(NewRooms);
	RebuildLayout();
}

void AAcousticTestLayout::PresetClosetAndHall()
{
	using namespace AcousticTestLayoutPresets;
	WallThickness = 25.f;
	Rooms = {
		Room(TEXT("Hall"), FVector(0, 0, 0), FVector(2000, 2000, 1000), EMat::Concrete, { Door(ESide::PosX, 100), Door(ESide::NegX, 900) }),
		Room(TEXT("Closet"), FVector(2050, 0, 0), FVector(300, 300, 250), EMat::Carpet, { Door(ESide::NegX, 100) }),
	};
	RebuildLayout();
}

void AAcousticTestLayout::PresetWallTest()
{
	using namespace AcousticTestLayoutPresets;
	WallThickness = 25.f;
	Rooms = {
		Room(TEXT("Listener"), FVector(0, 0, 0), FVector(800, 600, 300), EMat::Concrete, { Door(ESide::PosX, 50) }),
		Room(TEXT("Source"), FVector(850, 0, 0), FVector(800, 600, 300), EMat::Concrete, { Door(ESide::NegX, 50) }),
	};
	RebuildLayout();
}

void AAcousticTestLayout::PresetLongCorridor()
{
	using namespace AcousticTestLayoutPresets;
	WallThickness = 25.f;
	Rooms = {
		// 30 m 복도 (x 방향). 끝(+X)은 모퉁이 복도로 완전히 트여 있다
		Room(TEXT("CorridorA"), FVector(0, 0, 0), FVector(3000, 300, 350), EMat::Concrete,
			{ Door(ESide::PosY, 150, 100, 210), Door(ESide::PosX, 0, 300, 350) }),
		// 모퉁이를 돈 20 m 복도 (y 방향). 총기는 이 끝에 둔다
		Room(TEXT("CorridorB"), FVector(3050, 0, 0), FVector(300, 2000, 350), EMat::Concrete,
			{ Door(ESide::NegX, 0, 300, 350) }),
		// 복도 시작점 옆 대기실 (리스너 시작 위치)
		Room(TEXT("WaitingRoom"), FVector(0, 350, 0), FVector(600, 600, 300), EMat::Wood,
			{ Door(ESide::NegY, 150, 100, 210) }),
	};
	RebuildLayout();
}

void AAcousticTestLayout::PresetCourtyard()
{
	using namespace AcousticTestLayoutPresets;
	WallThickness = 25.f;
	Rooms = {
		Room(TEXT("Courtyard"), FVector(0, 0, 0), FVector(1000, 1000, 600), EMat::Concrete, { Door(ESide::PosX, 200), Door(ESide::NegX, 450) }, false),
		Room(TEXT("Indoor"), FVector(1050, 0, 0), FVector(500, 500, 300), EMat::Wood, { Door(ESide::NegX, 200) }),
	};
	RebuildLayout();
}
