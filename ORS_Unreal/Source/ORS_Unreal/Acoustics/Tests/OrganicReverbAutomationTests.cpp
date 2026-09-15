// OrganicReverbAutomationTests.cpp
// ---------------------------------------------------------
// 엔진 안 통합 테스트: 테스트 레이아웃 스폰 → 실제 콜리전 쿼리로 격자 스캔 → 방 분할 → 시뮬레이션.
// 콘솔 프로토타입(Prototype/)이 알고리즘을 검증했다면, 여기서는 Unreal 어댑터(스캐너, 단위 변환, 재질 조회)를 검증한다.
//
// 실행 (헤드리스):
//   UnrealEditor-Cmd.exe ORS_Unreal.uproject -ExecCmds="Automation RunTests OrganicReverb;Quit" -unattended -nullrhi -nosound
// 에디터: Tools → Session Frontend → Automation → OrganicReverb
// ---------------------------------------------------------
#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Acoustics/AcousticGridScanner.h"
#include "Acoustics/AcousticTestLayout.h"
#include "AcousticCore/AcousticDiffusionSimulator.h"
#include "AcousticCore/AcousticSpaceSegmenter.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

namespace OrganicReverbTests
{
	struct FScanResult
	{
		Acoustic::FAcousticRoomGraph Graph;
		Acoustic::FSpaceSegmentation Segmentation;
		FAcousticScanStats Stats;
		bool bScanned = false;
	};

	// 임시 게임 월드에 레이아웃 스폰 → 프리셋 적용 → 스캔 → 분할. MaxRoomLength 0 = 분할 안 함
	static FScanResult ScanLayout(TFunctionRef<void(AAcousticTestLayout&)> SetupLayout, float MaxRoomLengthMeters)
	{
		FScanResult Result;
		UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("OrganicReverbTestWorld"));
		FWorldContext& Context = GEngine->CreateNewWorldContext(EWorldType::Game);
		Context.SetCurrentWorld(World);

		if (AAcousticTestLayout* Layout = World->SpawnActor<AAcousticTestLayout>())
		{
			Layout->bAutoFitReverbVolume = false;
			SetupLayout(*Layout);

			FAcousticScanSettings ScanSettings;
			ScanSettings.Bounds = Layout->GetLayoutBounds();
			ScanSettings.CellSize = 50.f;

			Acoustic::FOccupancyGrid Grid;
			Result.bScanned = FAcousticGridScanner::Scan(*World, ScanSettings, Grid, &Result.Stats);
			if (Result.bScanned)
			{
				Acoustic::FSegmentationSettings SegSettings;
				SegSettings.MaxRoomLength = MaxRoomLengthMeters;
				Result.Segmentation = Acoustic::SegmentSpace(Grid, Result.Graph, SegSettings);
			}
		}

		GEngine->DestroyWorldContext(World);
		World->DestroyWorld(false);
		return Result;
	}

	static Acoustic::RoomId RoomAt(const FScanResult& Result, const FVector& LocationCm)
	{
		return Result.Segmentation.FindRoomAt(OrganicReverb::ToCore(LocationCm));
	}

	static int32 CountPortalsWithArea(const Acoustic::FAcousticRoomGraph& Graph, float Area)
	{
		int32 Count = 0;
		for (const Acoustic::FPortal& Portal : Graph.GetPortals())
		{
			Count += FMath::IsNearlyEqual(Portal.OpeningArea, Area, 0.01f) ? 1 : 0;
		}
		return Count;
	}

	static const Acoustic::FPortal* FindPortalBetween(const Acoustic::FAcousticRoomGraph& Graph, Acoustic::RoomId A, Acoustic::RoomId B)
	{
		for (const Acoustic::FPortal& Portal : Graph.GetPortals())
		{
			if ((Portal.RoomA == A && Portal.RoomB == B) || (Portal.RoomA == B && Portal.RoomB == A)) return &Portal;
		}
		return nullptr;
	}

	static void LogResult(FAutomationTestBase& Test, const FScanResult& Result)
	{
		Test.AddInfo(FString::Printf(TEXT("Scan %lld cells (solid %lld, surface %lld, materials %d) in %.1f ms"),
			Result.Stats.NumCells, Result.Stats.SolidCells, Result.Stats.SurfaceCells, Result.Stats.NumMaterials, Result.Stats.Seconds * 1000.0));
		for (const Acoustic::FRoom& Room : Result.Graph.GetRooms())
		{
			Test.AddInfo(FString::Printf(TEXT("  %s: V=%.1f m3, S=%.1f m2, alpha(Mid)=%.3f, RT60(Mid)=%.2f s"),
				UTF8_TO_TCHAR(Room.Name.c_str()), Room.Volume, Room.SurfaceArea, Room.Material.Absorption.Mid,
				Acoustic::SabineRT60(Room, Acoustic::SpeedOfSound, Acoustic::AirAbsorption::Coefficient()).Mid));
		}
		for (const Acoustic::FPortal& Portal : Result.Graph.GetPortals())
		{
			Test.AddInfo(FString::Printf(TEXT("  Portal %u: Room_%u <-> Room_%u, %.2f m2"), Portal.Id, Portal.RoomA, Portal.RoomB, Portal.OpeningArea));
		}
	}

	// dB 시계열이 처음으로 Threshold 이하가 되는 시간 (못 찾으면 -1)
	static float FirstCrossingTime(const TArray<float>& SeriesDb, float Dt, float ThresholdDb)
	{
		for (int32 I = 0; I < SeriesDb.Num(); ++I)
		{
			if (SeriesDb[I] <= ThresholdDb) return I * Dt;
		}
		return -1.f;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrganicReverbCorridorScanTest, "OrganicReverb.Scan.CorridorWithSideRooms",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FOrganicReverbCorridorScanTest::RunTest(const FString& Parameters)
{
	using namespace OrganicReverbTests;
	const FScanResult Result = ScanLayout([](AAcousticTestLayout& Layout) { Layout.PresetCorridorWithSideRooms(); }, 12.f);
	LogResult(*this, Result);
	if (!TestTrue(TEXT("Scan succeeded"), Result.bScanned)) return false;

	// 복도(24 m)는 12 m씩 두 방, 옆방 3개는 각각 별도 방
	const Acoustic::RoomId CorridorNear = RoomAt(Result, FVector(200, 125, 150));
	const Acoustic::RoomId CorridorFar = RoomAt(Result, FVector(2200, 125, 150));
	const Acoustic::RoomId Side1 = RoomAt(Result, FVector(350, 500, 150));
	const Acoustic::RoomId Side2 = RoomAt(Result, FVector(1150, 500, 150));
	const Acoustic::RoomId Side3 = RoomAt(Result, FVector(1950, 500, 150));
	const TSet<Acoustic::RoomId> Unique = { CorridorNear, CorridorFar, Side1, Side2, Side3 };

	TestFalse(TEXT("All sample points are inside rooms"), Unique.Contains(Acoustic::InvalidRoomId));
	TestEqual(TEXT("Corridor halves + 3 side rooms are distinct"), Unique.Num(), 5);
	TestEqual(TEXT("Door portals (1 x 2 m)"), CountPortalsWithArea(Result.Graph, 2.f), 3);
	TestEqual(TEXT("Corridor split portal (2.5 x 3 m)"), CountPortalsWithArea(Result.Graph, 7.5f), 1);

	if (const Acoustic::FRoom* Side = Result.Graph.GetRoom(Side1))
	{
		TestTrue(TEXT("Side room volume ~48 m3"), FMath::IsNearlyEqual(Side->Volume, 48.f, 1.5f));
		// 벽을 공유하는 칸은 콘크리트/나무 중 하나로 읽힐 수 있으므로 범위로 확인
		TestTrue(TEXT("Side room absorption is wood-dominant"), Side->Material.Absorption.Mid > 0.065f && Side->Material.Absorption.Mid < 0.081f);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrganicReverbClosetHallTest, "OrganicReverb.Scan.ClosetAndHallDoubleSlope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FOrganicReverbClosetHallTest::RunTest(const FString& Parameters)
{
	using namespace OrganicReverbTests;
	const FScanResult Result = ScanLayout([](AAcousticTestLayout& Layout) { Layout.PresetClosetAndHall(); }, 0.f);
	LogResult(*this, Result);
	if (!TestTrue(TEXT("Scan succeeded"), Result.bScanned)) return false;

	const Acoustic::RoomId Hall = RoomAt(Result, FVector(1000, 1000, 500));
	const Acoustic::RoomId Closet = RoomAt(Result, FVector(2200, 150, 125));
	if (!TestTrue(TEXT("Hall and closet are distinct rooms"), Hall != Acoustic::InvalidRoomId && Closet != Acoustic::InvalidRoomId && Hall != Closet)) return false;

	const Acoustic::FPortal* Door = FindPortalBetween(Result.Graph, Hall, Closet);
	TestTrue(TEXT("Door portal between hall and closet (2 m2)"), Door && FMath::IsNearlyEqual(Door->OpeningArea, 2.f, 0.01f));
	TestTrue(TEXT("Hall volume ~4000 m3"), FMath::IsNearlyEqual(Result.Graph.GetRoom(Hall)->Volume, 4001.5f, 2.f));
	TestTrue(TEXT("Closet volume ~22.5 m3"), FMath::IsNearlyEqual(Result.Graph.GetRoom(Closet)->Volume, 23.f, 1.f));

	// 스캔으로 만든 그래프에서도 이중 기울기 감쇠가 나와야 한다
	Acoustic::FAcousticRoomGraph Graph = Result.Graph;
	Acoustic::FAcousticDiffusionSimulator Sim(Graph);
	Sim.InjectEnergy(Closet, { 0.f, 1.f, 0.f });
	const float W0 = Sim.GetEnergyDensity(Closet).Mid;

	const float Dt = 1.f / 120.f;
	TArray<float> SeriesDb;
	for (int32 I = 0; I < 8 * 120; ++I)
	{
		Sim.Tick(Dt);
		const float W = Sim.GetEnergyDensity(Closet).Mid;
		SeriesDb.Add(W > 0.f ? 10.f * FMath::LogX(10.f, W / W0) : -999.f);
	}

	// 기준 RT60도 시뮬레이터와 같은 물리로 (공기 흡음 포함)
	const Acoustic::FBandValues AirM = Sim.GetSettings().AirAbsorptionCoefficient;
	const float OwnRT60 = Acoustic::SabineRT60(*Graph.GetRoom(Closet), Acoustic::SpeedOfSound, AirM).Mid;
	const float EDT = 6.f * FirstCrossingTime(SeriesDb, Dt, -10.f);
	const float LateRT = 6.f * (FirstCrossingTime(SeriesDb, Dt, -55.f) - FirstCrossingTime(SeriesDb, Dt, -45.f));
	AddInfo(FString::Printf(TEXT("Closet RT60 %.2f s, hall RT60 %.2f s -> measured EDT %.2f s, late RT %.2f s"),
		OwnRT60, Acoustic::SabineRT60(*Graph.GetRoom(Hall), Acoustic::SpeedOfSound, AirM).Mid, EDT, LateRT));
	TestTrue(TEXT("Reached -55 dB"), FirstCrossingTime(SeriesDb, Dt, -55.f) > 0.f);
	TestTrue(TEXT("Early decay follows the closet"), EDT < 1.5f * OwnRT60);
	TestTrue(TEXT("Late decay follows the hall"), LateRT > 5.f * OwnRT60);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOrganicReverbModularRoomTest, "OrganicReverb.Scan.ModularRoomResize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FOrganicReverbModularRoomTest::RunTest(const FString& Parameters)
{
	using namespace OrganicReverbTests;
	const FVector Inside(300, 250, 150);

	const FScanResult Small = ScanLayout([](AAcousticTestLayout& Layout) { Layout.PresetModularRoom(); }, 0.f);
	const FScanResult Large = ScanLayout([](AAcousticTestLayout& Layout)
	{
		Layout.PresetModularRoom();
		Layout.SetRoomSize(0, FVector(1000, 500, 600)); // 런타임 크기 변경 (씬 1)
	}, 0.f);
	LogResult(*this, Small);
	LogResult(*this, Large);

	const Acoustic::FRoom* SmallRoom = Small.Graph.GetRoom(RoomAt(Small, Inside));
	const Acoustic::FRoom* LargeRoom = Large.Graph.GetRoom(RoomAt(Large, Inside));
	if (!TestTrue(TEXT("Room found in both scans"), SmallRoom && LargeRoom)) return false;

	TestTrue(TEXT("6 x 5 x 3 m room ~90 m3"), FMath::IsNearlyEqual(SmallRoom->Volume, 90.5f, 1.f));
	TestTrue(TEXT("10 x 5 x 6 m room ~300 m3"), FMath::IsNearlyEqual(LargeRoom->Volume, 300.5f, 1.f));
	TestTrue(TEXT("Larger room reverberates longer"), Acoustic::SabineRT60(*LargeRoom).Mid > Acoustic::SabineRT60(*SmallRoom).Mid);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
