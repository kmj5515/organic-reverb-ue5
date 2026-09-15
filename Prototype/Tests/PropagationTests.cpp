// PropagationTests.cpp
// ---------------------------------------------------------
// AcousticPropagation 검증: 음원별 직접음 경로(차폐/회절)와 잔향 결합(Exclusion).
// 모든 장면은 격자 → SegmentSpace로 만든 실제 그래프를 쓴다. 칸 = 0.5 m.
// ---------------------------------------------------------
#include "../TestFramework.h"
#include "AcousticCore/AcousticPropagation.h"

using namespace Acoustic;

namespace
{
	struct FScene
	{
		FOccupancyGrid Grid;
		FAcousticRoomGraph Graph;
		FSpaceSegmentation Seg;

		FPropagationPath Find(const FVec3& Source, const FVec3& Listener, const FPropagationSettings& Settings = FPropagationSettings()) const
		{
			return FindPropagationPath(Graph, Seg.FindRoomAt(Source), Source, Seg.FindRoomAt(Listener), Listener, Settings, &Seg);
		}
	};

	FScene Build(int X, int Y, int Z, const std::function<void(FOccupancyGrid&)>& CarveRooms)
	{
		FScene S;
		S.Grid.Init(X, Y, Z, 0.5f);
		const uint8_t Concrete = S.Grid.AddMaterial(MaterialPresets::Concrete());
		S.Grid.FillBox(0, 0, 0, X, Y, Z, Concrete);
		CarveRooms(S.Grid);
		S.Seg = SegmentSpace(S.Grid, S.Graph);
		return S;
	}

	// 방 A (x 0.5~5.5 m) | 벽 (5.5~6.0) | 방 B (6.0~11.0), y 0.5~4.5, 높이 3 m. 문 1 x 2 m, y 2.0~3.0
	FScene MakeTwoRooms()
	{
		return Build(23, 10, 8, [](FOccupancyGrid& G)
		{
			G.FillBox(1, 1, 1, 11, 9, 7, FOccupancyGrid::Air);
			G.FillBox(12, 1, 1, 22, 9, 7, FOccupancyGrid::Air);
			G.FillBox(11, 4, 1, 12, 6, 5, FOccupancyGrid::Air);
		});
	}

	// A | B | C 일렬. A-B 문은 y 0.5~1.5, B-C 문은 y 3.0~4.0 → 직선으로 보이지 않는 지그재그
	FScene MakeThreeRooms()
	{
		return Build(34, 10, 8, [](FOccupancyGrid& G)
		{
			G.FillBox(1, 1, 1, 11, 9, 7, FOccupancyGrid::Air);
			G.FillBox(12, 1, 1, 22, 9, 7, FOccupancyGrid::Air);
			G.FillBox(23, 1, 1, 33, 9, 7, FOccupancyGrid::Air);
			G.FillBox(11, 1, 1, 12, 3, 5, FOccupancyGrid::Air);
			G.FillBox(22, 6, 1, 23, 8, 5, FOccupancyGrid::Air);
		});
	}

	// ㄱ자 긴 복도: x축 36 m + y축 16 m, 폭 2 m. 분할 기준(MaxRoomLength 12 m)보다 길어서
	// 곧은 x축 팔이 3조각으로 잘리고 그 사이에 "가상 경계" 포탈 2개가 축 위에 생긴다.
	// 모퉁이 때문에 직선 가시는 안 되므로 문 경로를 타야 한다.
	FScene MakeLongLCorridor()
	{
		return Build(74, 38, 8, [](FOccupancyGrid& G)
		{
			G.FillBox(1, 1, 1, 73, 5, 7, FOccupancyGrid::Air);   // x 0.5~36.5 m, y 0.5~2.5 m
			G.FillBox(69, 5, 1, 73, 37, 7, FOccupancyGrid::Air); // x 34.5~36.5 m, y 2.5~18.5 m
		});
	}

	void LogPath(const char* Label, const FPropagationPath& P)
	{
		LOG("%s: 문 %zu개, 벽 %d, 경로 %.2f m (직선 %.2f m), Gain Low/Mid/High = %.1f / %.1f / %.1f dB%s%s",
			Label, P.Portals.size(), P.WallsBetween, P.PathLength, P.DirectDistance, P.GainDb.Low, P.GainDb.Mid, P.GainDb.High,
			P.bOccluded ? " [차폐]" : "", P.bLineOfSight ? " [직선 가시]" : "");
	}
}

TEST_CASE(Prop_SameRoom_NoLoss)
{
	const FScene S = MakeTwoRooms();
	const FPropagationPath P = S.Find(FVec3(1.5f, 1.5f, 1.5f), FVec3(4.f, 3.5f, 1.5f));
	CHECK(P.bValid);
	CHECK(P.bSameRoom);
	CHECK(P.GainDb.Low == 0.f && P.GainDb.Mid == 0.f && P.GainDb.High == 0.f);

	FReverbCoupling Coupling;
	CHECK(Coupling.GetCouplingDb(S.Graph, S.Seg.FindRoomAt(FVec3(1.5f, 1.5f, 1.5f)), S.Seg.FindRoomAt(FVec3(4.f, 3.5f, 1.5f))) == 0.f);
}

// 문을 돌아서 오는 소리: 약간 작아지고, 고음이 더 많이 깎여 먹먹해지고, 문 쪽에서 들려야 한다
TEST_CASE(Prop_AroundDoor_DiffractionMuffles)
{
	const FScene S = MakeTwoRooms();
	const FPropagationPath P = S.Find(FVec3(1.f, 4.f, 1.5f), FVec3(10.5f, 4.f, 1.5f));
	LogPath("문 돌아가기", P);

	CHECK(P.bValid && !P.bSameRoom && !P.bOccluded && !P.bLineOfSight);
	CHECK(P.WallsBetween == 1);
	CHECK(P.Portals.size() == 1);
	CHECK(P.PathLength > P.DirectDistance);
	CHECK(P.GainDb.Mid < 0.f);
	CHECK(P.GainDb.High < P.GainDb.Mid);
	CHECK(P.GainDb.Low > P.GainDb.Mid);
	CHECK_NEAR(P.ApparentPosition.X, 6.0, 0.3); // 문 위치에서 들림
}

// 열린 문 너머로 직선으로 보이면 막힘이 없어야 한다
TEST_CASE(Prop_StraightThroughDoor_LineOfSight)
{
	const FScene S = MakeTwoRooms();
	const FPropagationPath P = S.Find(FVec3(2.f, 2.5f, 1.5f), FVec3(10.f, 2.5f, 1.5f));
	LogPath("문 너머 직선", P);
	CHECK(P.bLineOfSight);
	CHECK(P.WallsBetween == 0);
	CHECK(P.GainDb.Mid == 0.f && P.GainDb.High == 0.f);
}

TEST_CASE(Prop_DoorState_OcclusionAndHalfOpen)
{
	FScene S = MakeTwoRooms();
	const FVec3 Source(1.f, 4.f, 1.5f), Listener(10.5f, 4.f, 1.5f);
	const FPropagationPath Open = S.Find(Source, Listener);

	FPortal* Door = S.Graph.GetPortal(0);
	CHECK(Door != nullptr);
	if (!Door) return;

	Door->Transmission = 0.5f;
	const FPropagationPath Half = S.Find(Source, Listener);
	Door->Transmission = 0.f;
	const FPropagationPath Closed = S.Find(Source, Listener);
	LogPath("반개방", Half);
	LogPath("닫힘", Closed);

	CHECK_NEAR(Half.GainDb.Mid, Open.GainDb.Mid - 3.01, 0.05); // τ = 0.5 → -3 dB
	CHECK(Closed.bOccluded);
	CHECK_NEAR(Closed.GainDb.Mid, -35.0, 0.01); // 벽 1개 투과 손실
	CHECK(Closed.GainDb.High < Closed.GainDb.Mid);

	const FSourceOutput OpenOut = MapToSourceOutput(Open, 0.f);
	const FSourceOutput ClosedOut = MapToSourceOutput(Closed, 0.f);
	LOG("출력: 열림 볼륨 %.2f / LPF %.0f Hz, 닫힘 볼륨 %.3f / LPF %.0f Hz",
		OpenOut.VolumeMultiplier, OpenOut.LowpassHz, ClosedOut.VolumeMultiplier, ClosedOut.LowpassHz);
	CHECK(ClosedOut.VolumeMultiplier < 0.05f);
	CHECK(ClosedOut.LowpassHz < 3000.f);
	CHECK(OpenOut.LowpassHz > ClosedOut.LowpassHz);
}

// 방을 더 많이 거칠수록 더 작고 먹먹해야 하며, 문을 다 닫으면 벽 수만큼 막혀야 한다
TEST_CASE(Prop_MultipleRooms_MoreHopsMoreLoss)
{
	FScene S = MakeThreeRooms();
	const FVec3 Source(1.f, 4.f, 1.5f);
	const FVec3 InB(10.f, 2.5f, 1.5f), InC(15.5f, 1.f, 1.5f);
	const FPropagationPath ToB = S.Find(Source, InB);
	const FPropagationPath ToC = S.Find(Source, InC);
	LogPath("A → B", ToB);
	LogPath("A → C", ToC);

	CHECK(ToB.Portals.size() == 1);
	CHECK(ToC.Portals.size() == 2);
	CHECK(ToC.GainDb.Mid < ToB.GainDb.Mid);
	CHECK(ToC.GainDb.High < ToB.GainDb.High);

	for (const FPortal& Portal : S.Graph.GetPortals()) S.Graph.GetPortal(Portal.Id)->Transmission = 0.f;
	const FPropagationPath ClosedB = S.Find(Source, InB);
	const FPropagationPath ClosedC = S.Find(Source, InC);
	LogPath("A → B (문 모두 닫힘)", ClosedB);
	LogPath("A → C (문 모두 닫힘)", ClosedC);
	CHECK(ClosedB.WallsBetween == 1);
	CHECK(ClosedC.WallsBetween == 2);
	CHECK_NEAR(ClosedB.GainDb.Mid, -35.0, 0.01);
	CHECK_NEAR(ClosedC.GainDb.Mid, -60.0, 0.01); // 벽 2개 = -70 dB → 최소값 -60 dB로 클램프
}

// 경로 당기기 (6-2): 긴 복도를 조각낸 "가상 경계"는 실제 벽이 아니므로 그 중심을 지날 이유가 없다.
// 당기지 않으면 곧은 구간에서도 꼭짓점마다 회절 손실이 붙고 경로가 길어진다.
TEST_CASE(Prop_StringPulling_RemovesFakeCornersInSplitCorridor)
{
	const FScene S = MakeLongLCorridor();
	const FVec3 Source(2.f, 1.5f, 1.5f);     // x축 팔의 끝
	const FVec3 Listener(35.5f, 17.f, 1.5f); // y축 팔의 끝

	FPropagationSettings Off;
	Off.bStringPulling = false;
	const FPropagationPath Loose = S.Find(Source, Listener, Off);
	const FPropagationPath Tight = S.Find(Source, Listener);

	LogPath("당기기 없음", Loose);
	LogPath("당기기 적용", Tight);
	LOG("꼭짓점 %zu개 → %zu개 (지나온 문 %zu개는 그대로)", Loose.PathPoints.size(), Tight.PathPoints.size(), Tight.Portals.size());
	for (const FVec3& P : Tight.PathPoints) LOG("  남은 꼭짓점 (%.2f, %.2f, %.2f)", P.X, P.Y, P.Z);

	CHECK(Loose.bValid && !Loose.bLineOfSight && !Loose.bOccluded);
	CHECK(Tight.Portals.size() == Loose.Portals.size()); // 지나온 문 목록은 건드리지 않는다

	// 곧은 x축 팔 안의 가상 경계(x ≈ 12.5)는 펴지고, 모퉁이 쪽 꺾임은 남는다
	CHECK(Tight.PathPoints.size() == Loose.PathPoints.size() - 1);
	bool bMidArmVertexGone = true;
	for (size_t I = 1; I + 1 < Tight.PathPoints.size(); ++I)
	{
		bMidArmVertexGone &= !(Tight.PathPoints[I].X > 10.f && Tight.PathPoints[I].X < 20.f);
	}
	CHECK(bMidArmVertexGone);

	// 가짜 꼭짓점이 사라지면 경로가 짧아지고 회절 손실이 줄어든다 (고역에서 가장 크게)
	CHECK(Tight.PathLength < Loose.PathLength);
	CHECK(Tight.GainDb.High > Loose.GainDb.High);
	CHECK(Tight.GainDb.Mid > Loose.GainDb.Mid);

	// 한계: 당기기는 꼭짓점을 "빼는" 것이라 모퉁이의 진짜 꼭짓점을 새로 만들어내지는 못한다.
	// 모퉁이를 질러가는 문 중심이 그대로 남아 꺾임 일부는 여전히 실제보다 완만하다.
	CHECK(Tight.PathPoints.size() > 3);
}

// 당기기는 "펴도 되는" 꼭짓점만 편다. 진짜 벽에 막힌 꺾임은 그대로 남아야 한다.
TEST_CASE(Prop_StringPulling_KeepsRealCorners)
{
	const FScene S = MakeThreeRooms(); // 지그재그 문 배치 → 어느 구간도 직선으로 관통되지 않는다
	const FVec3 Source(3.f, 2.5f, 1.5f);
	const FVec3 Listener(14.f, 2.5f, 1.5f);

	FPropagationSettings Off;
	Off.bStringPulling = false;
	const FPropagationPath Loose = S.Find(Source, Listener, Off);
	const FPropagationPath Tight = S.Find(Source, Listener);

	LogPath("지그재그 (당기기 없음)", Loose);
	LogPath("지그재그 (당기기 적용)", Tight);
	CHECK(Tight.PathPoints.size() == Loose.PathPoints.size());
	CHECK_NEAR(Tight.PathLength, Loose.PathLength, 1e-4);
	CHECK_NEAR(Tight.GainDb.High, Loose.GainDb.High, 1e-4);

	// 반쯤 닫힌 문의 투과 손실은 꼭짓점을 펴도 그대로 남는다
	FScene Half = MakeTwoRooms();
	for (PortalId P = 0; P < Half.Graph.NumPortals(); ++P) Half.Graph.GetPortal(P)->Transmission = 0.5f;
	const FPropagationPath HalfPath = FindPropagationPath(Half.Graph,
		Half.Seg.FindRoomAt(FVec3(2.f, 1.f, 1.5f)), FVec3(2.f, 1.f, 1.5f),
		Half.Seg.FindRoomAt(FVec3(9.f, 1.f, 1.5f)), FVec3(9.f, 1.f, 1.5f), FPropagationSettings(), &Half.Seg);
	LogPath("반쯤 닫힌 문", HalfPath);
	CHECK(HalfPath.GainDb.Mid < -3.f);
}

// 잔향 결합: 음원 방에서 멀어질수록 리스너 방에 쌓이는 잔향이 작아진다. 같은 공간의 분할 조각은 거의 그대로.
TEST_CASE(Prop_ReverbCoupling_ExclusionByDistance)
{
	FScene S = MakeThreeRooms();
	const RoomId A = S.Seg.FindRoomAt(FVec3(3.f, 2.5f, 1.5f));
	const RoomId B = S.Seg.FindRoomAt(FVec3(8.5f, 2.5f, 1.5f));
	const RoomId C = S.Seg.FindRoomAt(FVec3(14.f, 2.5f, 1.5f));

	FReverbCoupling Coupling;
	const float AB = Coupling.GetCouplingDb(S.Graph, A, B);
	const float AC = Coupling.GetCouplingDb(S.Graph, A, C);
	LOG("잔향 결합: A→B %.1f dB, A→C %.1f dB", AB, AC);
	CHECK(AB < -1.f);
	CHECK(AC < AB);

	// 문을 닫으면 잔향도 전달되지 않는다 (캐시 무효화 필요)
	S.Graph.GetPortal(0)->Transmission = 0.f;
	Coupling.Invalidate();
	CHECK_NEAR(Coupling.GetCouplingDb(S.Graph, A, B), -60.0, 0.01);

	// 30 m 홀을 3 x 3으로 나눈 조각끼리는 거의 한 공간 (분할 자체가 Exclusion을 만들면 안 된다)
	FScene Hall = Build(62, 62, 22, [](FOccupancyGrid& G) { G.FillBox(1, 1, 1, 61, 61, 21, FOccupancyGrid::Air); });
	const RoomId Corner = Hall.Seg.FindRoomAt(FVec3(2.f, 2.f, 2.f));
	const RoomId Opposite = Hall.Seg.FindRoomAt(FVec3(28.f, 28.f, 2.f));
	FReverbCoupling HallCoupling;
	HallCoupling.AirAbsorptionCoefficient = 0.f;
	const float PiecesNoAir = HallCoupling.GetCouplingDb(Hall.Graph, Corner, Opposite);
	HallCoupling.AirAbsorptionCoefficient = AirAbsorption::Coefficient().Mid;
	HallCoupling.Invalidate();
	const float Pieces = HallCoupling.GetCouplingDb(Hall.Graph, Corner, Opposite);
	LOG("30 m 홀 대각선 끝 조각끼리 잔향 결합: 벽 흡음만 %.2f dB → 공기 흡음 포함 %.2f dB", PiecesNoAir, Pieces);
	CHECK(Corner != Opposite);
	CHECK(PiecesNoAir > -3.f);
	// 공기 흡음이 붙으면 40 m 대각선에서 1 dB 남짓 더 떨어진다. 다만 조각 크기(MaxRoomLength)에
	// 의존하는 값이라 "거의 한 공간" 수준을 넘지 않는지만 본다 (알려진 한계: 방 안은 균일 밀도)
	CHECK(Pieces > -4.f);
	CHECK(Pieces < PiecesNoAir);
}

// 경로가 없는(방 밖) 위치는 처리하지 않는다
TEST_CASE(Prop_OutsideRooms_Ignored)
{
	const FScene S = MakeTwoRooms();
	const FPropagationPath P = S.Find(FVec3(-5.f, -5.f, -5.f), FVec3(2.f, 2.f, 1.5f));
	CHECK(!P.bValid);
	const FSourceOutput Out = MapToSourceOutput(P, 0.f);
	CHECK(Out.VolumeMultiplier == 1.f);
	CHECK(Out.LowpassHz == 20000.f);
}
