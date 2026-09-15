// SegmenterTests.cpp
// ---------------------------------------------------------
// AcousticSpaceSegmenter 검증: 점유 격자 → 방/포탈 자동 분할.
// 모든 장면은 콘크리트로 꽉 찬 격자에서 공간을 "파내서" 만든다. 칸 = 0.5 m.
// ---------------------------------------------------------
#include "../TestFramework.h"
#include "AcousticCore/AcousticDiffusionSimulator.h"
#include "AcousticCore/AcousticSpaceSegmenter.h"
#include "AcousticCore/ReverbParameterMapper.h"

#include <chrono>

using namespace Acoustic;

namespace
{
	constexpr float Cell = 0.5f;

	struct FSceneGrid
	{
		FOccupancyGrid Grid;
		uint8_t Concrete = 0;
		uint8_t Carpet = 0;

		FSceneGrid(int X, int Y, int Z)
		{
			Grid.Init(X, Y, Z, Cell);
			Concrete = Grid.AddMaterial(MaterialPresets::Concrete());
			Carpet = Grid.AddMaterial(MaterialPresets::Carpet());
			Grid.FillBox(0, 0, 0, X, Y, Z, Concrete);
		}

		void Carve(int X0, int Y0, int Z0, int X1, int Y1, int Z1) { Grid.FillBox(X0, Y0, Z0, X1, Y1, Z1, FOccupancyGrid::Air); }
		FVec3 At(int X, int Y, int Z) const { return Grid.CellCenter(X, Y, Z); }
	};

	void LogGraph(const FAcousticRoomGraph& Graph)
	{
		for (const FRoom& R : Graph.GetRooms())
		{
			LOG("%s: V=%.1f m3, S=%.1f m2, alpha(Mid)=%.3f", R.Name.c_str(), R.Volume, R.SurfaceArea, R.Material.Absorption.Mid);
		}
		for (const FPortal& P : Graph.GetPortals())
		{
			LOG("Portal %u: Room_%u <-> Room_%u, %.2f m2", P.Id, P.RoomA, P.RoomB, P.OpeningArea);
		}
	}

	float TotalVolume(const FAcousticRoomGraph& Graph)
	{
		float Sum = 0.f;
		for (const FRoom& R : Graph.GetRooms()) Sum += R.Volume;
		return Sum;
	}

	int CountPortalsWithArea(const FAcousticRoomGraph& Graph, float Area)
	{
		int Count = 0;
		for (const FPortal& P : Graph.GetPortals()) Count += std::fabs(P.OpeningArea - Area) < 1e-3f ? 1 : 0;
		return Count;
	}

	// 복도 24 x 2.5 m + 옆방 4 x 4 m 3개 (문 1 x 2 m), 천장 3 m
	FSceneGrid MakeCorridorScene()
	{
		FSceneGrid S(50, 16, 8);
		S.Carve(1, 1, 1, 49, 6, 7);
		for (int X0 : { 4, 20, 36 })
		{
			S.Carve(X0, 7, 1, X0 + 8, 15, 7);
			S.Carve(X0 + 3, 6, 1, X0 + 5, 7, 5);
		}
		return S;
	}
}

TEST_CASE(Seg_SingleBoxRoom_MeasuresVolumeAndSurface)
{
	FSceneGrid S(12, 10, 8);
	S.Carve(1, 1, 1, 11, 9, 7); // 5 x 4 x 3 m

	FAcousticRoomGraph Graph;
	const FSpaceSegmentation Seg = SegmentSpace(S.Grid, Graph);
	LogGraph(Graph);

	CHECK(Graph.NumRooms() == 1);
	CHECK(Graph.NumPortals() == 0);
	if (Graph.NumRooms() != 1) return;

	const FRoom& Room = Graph.GetRooms()[0];
	CHECK_NEAR(Room.Volume, 60.0, 1e-3);
	CHECK_NEAR(Room.SurfaceArea, 94.0, 1e-3);
	CHECK_NEAR(Room.Material.Absorption.Mid, 0.03, 1e-6);
	CHECK(Seg.FindRoomAt(S.At(5, 5, 3)) == 0);
	CHECK(Seg.FindRoomAt(FVec3(100.f, 100.f, 100.f)) == InvalidRoomId);
}

TEST_CASE(Seg_TwoRoomsWithDoor_DetectsPortal)
{
	FSceneGrid S(23, 10, 8);
	S.Carve(1, 1, 1, 11, 9, 7);
	S.Carve(12, 1, 1, 22, 9, 7);
	S.Carve(11, 4, 1, 12, 6, 5); // 벽(두께 1칸)에 문 1 x 2 m

	FAcousticRoomGraph Graph;
	const FSpaceSegmentation Seg = SegmentSpace(S.Grid, Graph);
	LogGraph(Graph);

	CHECK(Graph.NumRooms() == 2);
	CHECK(Graph.NumPortals() == 1);
	CHECK(CountPortalsWithArea(Graph, 2.f) == 1);
	CHECK_NEAR(TotalVolume(Graph), 121.0, 1e-3); // 60 + 60 + 문 칸 1
	CHECK(Seg.FindRoomAt(S.At(5, 5, 3)) != Seg.FindRoomAt(S.At(17, 5, 3)));
}

// 6방향 레이 프로브 방식이 깨지는 대표 사례. 문이 없으니 하나의 방이어야 한다.
TEST_CASE(Seg_LShapedRoom_StaysOneRoom)
{
	FSceneGrid S(18, 18, 8);
	S.Carve(1, 1, 1, 17, 7, 7); // 가로 팔 8 x 3 m
	S.Carve(1, 1, 1, 7, 17, 7); // 세로 팔 3 x 8 m

	FAcousticRoomGraph Graph;
	SegmentSpace(S.Grid, Graph);
	LogGraph(Graph);

	CHECK(Graph.NumRooms() == 1);
	CHECK(Graph.NumPortals() == 0);
	CHECK_NEAR(TotalVolume(Graph), 117.0, 1e-3);
}

// 옆방 3개는 문으로, 24m 복도는 12m 두 구간으로 나뉘어야 한다.
TEST_CASE(Seg_CorridorWithSideRooms_SplitsLongCorridor)
{
	const FSceneGrid S = MakeCorridorScene();
	FAcousticRoomGraph Graph;
	SegmentSpace(S.Grid, Graph);
	LogGraph(Graph);

	CHECK(Graph.NumRooms() == 5);
	CHECK(Graph.NumPortals() == 4);
	CHECK(CountPortalsWithArea(Graph, 2.f) == 3);  // 문
	CHECK(CountPortalsWithArea(Graph, 7.5f) == 1); // 복도 분할면 = 단면 2.5 x 3 m
}

// 폭 2m 복도는 코어가 없지만, 양쪽 방에 흡수되지 않고 독립된 방이 되어야 한다.
TEST_CASE(Seg_NarrowCorridor_BecomesOwnRoom)
{
	FSceneGrid S(38, 12, 8);
	S.Carve(1, 1, 1, 11, 11, 7);  // 방 A 5 x 5 m
	S.Carve(11, 4, 1, 27, 8, 7);  // 폭 2 m, 길이 8 m 복도 (천장까지 트임)
	S.Carve(27, 1, 1, 37, 11, 7); // 방 B 5 x 5 m

	FAcousticRoomGraph Graph;
	const FSpaceSegmentation Seg = SegmentSpace(S.Grid, Graph);
	LogGraph(Graph);

	CHECK(Graph.NumRooms() == 3);
	CHECK(Graph.NumPortals() == 2);
	CHECK(CountPortalsWithArea(Graph, 6.f) == 2); // 복도 단면 2 x 3 m

	const FRoom* Corridor = Graph.GetRoom(Seg.FindRoomAt(S.At(19, 5, 3)));
	CHECK(Corridor != nullptr);
	if (Corridor) CHECK_NEAR(Corridor->Volume, 48.0, 1e-3);
}

TEST_CASE(Seg_MixedMaterials_AreaWeightedAbsorption)
{
	FSceneGrid S(12, 10, 8);
	S.Grid.FillBox(0, 0, 0, 12, 10, 1, S.Carpet); // 바닥만 카펫
	S.Carve(1, 1, 1, 11, 9, 7);

	FAcousticRoomGraph Graph;
	SegmentSpace(S.Grid, Graph);
	CHECK(Graph.NumRooms() == 1);
	if (Graph.NumRooms() != 1) return;

	// 바닥 20 m2 카펫 + 나머지 74 m2 콘크리트
	const double Expected = (20.0 * 0.20 + 74.0 * 0.03) / 94.0;
	LOG("합성 흡음계수(Mid) = %.4f (기대 %.4f)", Graph.GetRooms()[0].Material.Absorption.Mid, Expected);
	CHECK_NEAR(Graph.GetRooms()[0].Material.Absorption.Mid, Expected, 1e-5);
}

// 공유 벽 양쪽 마감재가 다르면 방마다 자기 쪽 면 재질을 써야 한다 (칸 하나로는 구분 불가 → 면 재질)
TEST_CASE(Seg_SharedWall_PerFaceMaterials)
{
	FSceneGrid S(23, 10, 8);
	S.Carve(1, 1, 1, 11, 9, 7);  // 방 A (콘크리트)
	S.Carve(12, 1, 1, 22, 9, 7); // 방 B, 사이 벽 x = 11
	for (int Z = 1; Z < 7; ++Z)
		for (int Y = 1; Y < 9; ++Y)
			S.Grid.SetFaceMaterial(S.Grid.Index(11, Y, Z), 0 /* +X = B 쪽 */, S.Carpet);

	FAcousticRoomGraph Graph;
	const FSpaceSegmentation Seg = SegmentSpace(S.Grid, Graph);
	const FRoom* A = Graph.GetRoom(Seg.FindRoomAt(S.At(5, 5, 3)));
	const FRoom* B = Graph.GetRoom(Seg.FindRoomAt(S.At(17, 5, 3)));
	CHECK(A && B && A != B);
	if (!A || !B) return;

	// B: 공유 벽 4 x 3 m = 12 m2만 카펫, 나머지 82 m2 콘크리트
	const double ExpectedB = (12.0 * 0.20 + 82.0 * 0.03) / 94.0;
	LOG("공유 벽 한쪽만 카펫: A alpha(Mid) = %.4f, B = %.4f (기대 %.4f)", A->Material.Absorption.Mid, B->Material.Absorption.Mid, ExpectedB);
	CHECK_NEAR(A->Material.Absorption.Mid, 0.03, 1e-6);
	CHECK_NEAR(B->Material.Absorption.Mid, ExpectedB, 1e-5);
}

// 천장이 없는 안뜰: 위쪽 면은 격자 밖(열린 하늘) → BoundaryMaterial(OpenAir)
TEST_CASE(Seg_OpenRoof_UsesBoundaryMaterial)
{
	FSceneGrid Closed(12, 10, 8);
	Closed.Carve(1, 1, 1, 11, 9, 7);
	FSceneGrid Open(12, 10, 7);
	Open.Carve(1, 1, 1, 11, 9, 7);

	FAcousticRoomGraph ClosedGraph, OpenGraph;
	SegmentSpace(Closed.Grid, ClosedGraph);
	SegmentSpace(Open.Grid, OpenGraph);
	CHECK(OpenGraph.NumRooms() == 1);
	if (OpenGraph.NumRooms() != 1 || ClosedGraph.NumRooms() != 1) return;

	const double Expected = (20.0 * 0.99 + 74.0 * 0.03) / 94.0;
	CHECK_NEAR(OpenGraph.GetRooms()[0].Material.Absorption.Mid, Expected, 1e-5);

	const float RTClosed = SabineRT60(ClosedGraph.GetRooms()[0]).Mid;
	const float RTOpen = SabineRT60(OpenGraph.GetRooms()[0]).Mid;
	LOG("RT60: 밀폐 %.2fs → 천장 개방 %.2fs", RTClosed, RTOpen);
	CHECK(RTOpen < 0.2f * RTClosed);
}

// 긴 공간을 여러 방으로 나눠도 리버브 길이는 공간 전체 기준이어야 한다.
// (게임 모드 검증에서 발견: 20 m 홀을 8 m씩 나누자 조각마다 RT60이 10~27초로 제각각 — 조각은 바깥 벽이 거의 없기 때문)
TEST_CASE(Seg_SplitSpace_SharesGroupDecayTime)
{
	FSceneGrid Hall(62, 62, 22);
	Hall.Carve(1, 1, 1, 61, 61, 21); // 30 x 30 x 10 m 콘크리트 홀 → 12 m 기준 3 x 3 분할

	FAcousticRoomGraph Graph;
	SegmentSpace(Hall.Grid, Graph);
	CHECK(Graph.NumRooms() == 9);
	if (Graph.NumRooms() == 0) return;

	// 기준값은 매퍼와 같은 물리로 (공기 흡음 포함). 4mV 항도 부피 합 = 홀 전체라 조각을 합치면 그대로 맞아야 한다
	const float Expected = SabineRT60(MakeBoxRoom("Hall", FVec3(), 30.f, 30.f, 10.f, MaterialPresets::Concrete()),
		SpeedOfSound, AirAbsorption::Coefficient()).Mid;
	float MinOwn = 1e9f, MaxOwn = 0.f;
	bool bSameGroup = true;
	for (const FRoom& Room : Graph.GetRooms())
	{
		bSameGroup &= Room.GroupId == Graph.GetRooms()[0].GroupId;
		const float Own = MapToReverbParams(Room, FBandValues()).DecayTime;
		MinOwn = std::min(MinOwn, Own);
		MaxOwn = std::max(MaxOwn, Own);
		CHECK_NEAR(MapToReverbParams(Graph, Room.Id, FBandValues()).DecayTime, Expected, 0.01 * Expected);
	}
	LOG("조각 단독 RT60 %.1f ~ %.1f s → 그룹 기준 %.2f s (홀 전체 Sabine %.2f s)", MinOwn, MaxOwn,
		MapToReverbParams(Graph, 0, FBandValues()).DecayTime, Expected);
	CHECK(bSameGroup);

	// 복도 두 조각은 같은 그룹, 옆방은 각자 다른 그룹
	const FSceneGrid Corridor = MakeCorridorScene();
	FAcousticRoomGraph CorridorGraph;
	const FSpaceSegmentation Seg = SegmentSpace(Corridor.Grid, CorridorGraph);
	const FRoom* Near = CorridorGraph.GetRoom(Seg.FindRoomAt(Corridor.At(2, 3, 3)));
	const FRoom* Far = CorridorGraph.GetRoom(Seg.FindRoomAt(Corridor.At(46, 3, 3)));
	const FRoom* Side = CorridorGraph.GetRoom(Seg.FindRoomAt(Corridor.At(8, 11, 3)));
	CHECK(Near && Far && Side);
	if (Near && Far && Side)
	{
		CHECK(Near->Id != Far->Id);
		CHECK(Near->GroupId == Far->GroupId);
		CHECK(Side->GroupId != Near->GroupId);
	}
}

// 격자 → 그래프 → 총성 시뮬레이션 전체 파이프라인
TEST_CASE(Seg_EndToEnd_GunshotInCorridor)
{
	const FSceneGrid S = MakeCorridorScene();
	FAcousticRoomGraph Graph;
	const FSpaceSegmentation Seg = SegmentSpace(S.Grid, Graph);

	const RoomId Shot = Seg.FindRoomAt(S.At(2, 3, 3));    // 복도 시작점
	const RoomId Near = Seg.FindRoomAt(S.At(8, 11, 3));   // 첫 번째 옆방
	const RoomId Far = Seg.FindRoomAt(S.At(40, 11, 3));   // 세 번째 옆방
	CHECK(Shot != InvalidRoomId && Near != InvalidRoomId && Far != InvalidRoomId);
	CHECK(Shot != Near && Near != Far);
	CHECK(Seg.FindRoomAt(S.At(16, 11, 3)) == InvalidRoomId); // 옆방 사이 두꺼운 벽 속
	if (Shot == InvalidRoomId || Near == InvalidRoomId || Far == InvalidRoomId) return;

	FAcousticDiffusionSimulator Sim(Graph);
	Sim.InjectEnergy(Shot, { 0.f, 1.f, 0.f });
	const float W0 = Sim.GetEnergyDensity(Shot).Mid;

	// 초반(0.1초)엔 거리 차이가 뚜렷하고, 시간이 지나면 복도를 통해 균등해진다
	float NearDb[2], FarDb[2];
	const int Ticks[2] = { 6, 24 }; // 누적 0.1초, 0.5초
	for (int K = 0; K < 2; ++K)
	{
		for (int I = 0; I < Ticks[K]; ++I) Sim.Tick(1.f / 60.f);
		NearDb[K] = 10.f * std::log10(Sim.GetEnergyDensity(Near).Mid / W0);
		FarDb[K] = 10.f * std::log10(Sim.GetEnergyDensity(Far).Mid / W0);
	}
	LOG("0.1초: 가까운 옆방 %.1f dB, 먼 옆방 %.1f dB (차이 %.1f dB)", NearDb[0], FarDb[0], NearDb[0] - FarDb[0]);
	LOG("0.5초: 가까운 옆방 %.1f dB, 먼 옆방 %.1f dB (차이 %.1f dB)", NearDb[1], FarDb[1], NearDb[1] - FarDb[1]);
	CHECK(NearDb[0] - FarDb[0] > 3.f);
	CHECK(NearDb[1] - FarDb[1] < NearDb[0] - FarDb[0]);
}

// 목표: 레벨 로드 시 1회 실행. 100개 방 건물 (100 x 100 m) 분할이 수백 ms 이내
TEST_CASE(Seg_Perf_100RoomBuilding)
{
	const int Rooms = 10, Pitch = 20; // 방 9.5 x 9.5 m + 벽 0.5 m
	FSceneGrid S(Rooms * Pitch + 1, Rooms * Pitch + 1, 8);
	for (int RY = 0; RY < Rooms; ++RY)
	{
		for (int RX = 0; RX < Rooms; ++RX)
		{
			const int X0 = RX * Pitch + 1, Y0 = RY * Pitch + 1;
			S.Carve(X0, Y0, 1, X0 + Pitch - 1, Y0 + Pitch - 1, 7);
			if (RX + 1 < Rooms) S.Carve(X0 + Pitch - 1, Y0 + 9, 1, X0 + Pitch, Y0 + 11, 5); // 동쪽 문
			if (RY + 1 < Rooms) S.Carve(X0 + 9, Y0 + Pitch - 1, 1, X0 + 11, Y0 + Pitch, 5); // 북쪽 문
		}
	}

	FAcousticRoomGraph Graph;
	const auto Start = std::chrono::steady_clock::now();
	SegmentSpace(S.Grid, Graph);
	const double Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count();

	LOG("격자 %zu칸 → 방 %zu개, 포탈 %zu개, %.1f ms", S.Grid.NumCells(), Graph.NumRooms(), Graph.NumPortals(), Ms);
	CHECK(Graph.NumRooms() == 100);
	CHECK(Graph.NumPortals() == 180);
	CHECK(Ms < 2000.0);
}
