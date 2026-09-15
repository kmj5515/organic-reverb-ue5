// ShotRenderTests.cpp
// ---------------------------------------------------------
// RenderShot 검증: 원거리 총성의 거리감 (도착 지연, 들리는 위치, 거리 감쇠, 공기 흡수, 레이어, 잔향 Send).
// ---------------------------------------------------------
#include "../TestFramework.h"
#include "AcousticCore/AcousticShotRender.h"

using namespace Acoustic;

namespace
{
	// 방 A (x 0.5~5.5) | 벽 | 방 B (x 6.0~11.0), 문 y 2.0~3.0 (PropagationTests와 같은 장면)
	struct FTwoRooms
	{
		FOccupancyGrid Grid;
		FAcousticRoomGraph Graph;
		FSpaceSegmentation Seg;

		FTwoRooms()
		{
			Grid.Init(23, 10, 8, 0.5f);
			const uint8_t Concrete = Grid.AddMaterial(MaterialPresets::Concrete());
			Grid.FillBox(0, 0, 0, 23, 10, 8, Concrete);
			Grid.FillBox(1, 1, 1, 11, 9, 7, FOccupancyGrid::Air);
			Grid.FillBox(12, 1, 1, 22, 9, 7, FOccupancyGrid::Air);
			Grid.FillBox(11, 4, 1, 12, 6, 5, FOccupancyGrid::Air);
			Seg = SegmentSpace(Grid, Graph);
		}

		FShotRender Render(const FVec3& Source, const FVec3& Listener, float CouplingDb = 0.f) const
		{
			const FPropagationPath Path = FindPropagationPath(Graph, Seg.FindRoomAt(Source), Source, Seg.FindRoomAt(Listener), Listener, FPropagationSettings(), &Seg);
			return RenderShot(Path, CouplingDb);
		}
	};

	// 방 밖(열린 공간)에서의 직선 전파: 경로 계산 없이 직선 거리만 있는 경우
	FShotRender RenderOpen(float Distance)
	{
		FPropagationPath Path;
		Path.DirectDistance = Distance;
		Path.PathLength = Distance;
		return RenderShot(Path, 0.f);
	}

	void LogShot(const char* Label, const FShotRender& R)
	{
		LOG("%s: 거리 %.1f m, 지연 %.0f ms, Gain L/M/H %.1f / %.1f / %.1f dB, LPF %.0f Hz, 레이어 근 %.2f / 원 %.2f, 잔향 Send %.2f",
			Label, R.Distance, R.DelaySeconds * 1000.f, R.GainDb.Low, R.GainDb.Mid, R.GainDb.High, R.LowpassHz, R.NearLayerGain, R.FarLayerGain, R.ReverbSendMultiplier);
	}
}

// 열린 공간: 거리 2배당 −6 dB, 음속으로 늦게 도착, 멀수록 원거리 레이어
TEST_CASE(Shot_OpenSpace_DistanceCues)
{
	const FShotRender Near = RenderOpen(4.f);
	const FShotRender Mid = RenderOpen(20.f);
	const FShotRender Far = RenderOpen(40.f);
	LogShot("4 m", Near);
	LogShot("20 m", Mid);
	LogShot("40 m", Far);

	CHECK_NEAR(Near.GainDb.Mid, -0.02, 0.01);            // InnerRadius 안: 공기 흡수만
	CHECK_NEAR(Mid.GainDb.Mid, 20.0 * std::log10(5.0 / 20.0) - 0.1, 0.01);
	CHECK_NEAR(Far.GainDb.Mid - Mid.GainDb.Mid, -6.02 - 0.1, 0.02); // 거리 2배 → −6 dB (+ 공기 흡수)
	CHECK_NEAR(Mid.DelaySeconds, 20.0 / 343.0, 1e-4);
	CHECK(Near.NearLayerGain == 1.f && Near.FarLayerGain == 0.f);
	CHECK(Far.FarLayerGain > Far.NearLayerGain);
	CHECK_NEAR(Mid.NearLayerGain * Mid.NearLayerGain + Mid.FarLayerGain * Mid.FarLayerGain, 1.0, 1e-5); // 동일 파워
}

// 공기 흡수: 수백 m에서는 고음이 확실히 더 깎여 먹먹해진다
TEST_CASE(Shot_AirAbsorption_DullsFarShots)
{
	const FShotRender Indoor = RenderOpen(30.f);
	const FShotRender Outdoor = RenderOpen(300.f);
	LogShot("30 m", Indoor);
	LogShot("300 m", Outdoor);
	CHECK(Indoor.GainDb.Mid - Indoor.GainDb.High < 1.f); // 실내 거리에서는 영향 작음
	CHECK_NEAR(Outdoor.GainDb.Mid - Outdoor.GainDb.High, 300.0 * (0.025 - 0.005), 0.01);
	CHECK(Outdoor.LowpassHz < Indoor.LowpassHz);
}

// 모퉁이 너머: 경로 길이만큼 늦게, 문 쪽에서, 같은 직선 거리의 탁 트인 총성보다 작고 먹먹하게
TEST_CASE(Shot_AroundCorner_ArrivesLaterFromDoorway)
{
	const FTwoRooms S;
	const FVec3 Source(1.f, 4.f, 1.5f), Listener(10.5f, 4.f, 1.5f);
	const FShotRender Around = S.Render(Source, Listener, -5.f);
	const FShotRender Open = RenderOpen((Listener - Source).Length());
	LogShot("모퉁이 너머", Around);
	LogShot("같은 거리 탁 트임", Open);

	CHECK(Around.bValid);
	CHECK(Around.Distance > (Listener - Source).Length());          // 돌아온 경로
	CHECK_NEAR(Around.DelaySeconds, Around.Distance / 343.0, 1e-5);
	CHECK_NEAR(Around.ApparentPosition.X, 6.0, 0.3);                // 문 쪽에서 들림
	CHECK(Around.GainDb.Mid < Open.GainDb.Mid);
	CHECK(Around.LowpassHz < Open.LowpassHz);
	CHECK_NEAR(Around.ReverbSendMultiplier, std::pow(10.0, -5.0 / 20.0), 1e-5);
}

// 문이 닫혀 벽을 뚫고 오면: 직선 거리로 도착, 크게 막히고 먹먹함
TEST_CASE(Shot_ThroughWall_Muffled)
{
	FTwoRooms S;
	S.Graph.GetPortal(0)->Transmission = 0.f;
	const FVec3 Source(1.f, 4.f, 1.5f), Listener(10.5f, 4.f, 1.5f);
	const FShotRender Wall = S.Render(Source, Listener);
	LogShot("벽 너머", Wall);

	CHECK_NEAR(Wall.Distance, (Listener - Source).Length(), 1e-4);
	CHECK(Wall.GainDb.Mid < -40.f);
	CHECK(Wall.LowpassHz < 3000.f);
}
