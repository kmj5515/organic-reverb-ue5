// DecayMeterTests.cpp
// ---------------------------------------------------------
// FDecayMeter 검증: 시뮬레이터 곡선에서 EDT / T20 / 후기 RT를 제대로 재는가.
// ---------------------------------------------------------
#include "../TestFramework.h"
#include "AcousticCore/AcousticDecayMeter.h"
#include "AcousticCore/AcousticDiffusionSimulator.h"

using namespace Acoustic;

namespace
{
	// 측정이 끝날 때까지 시뮬레이터를 돌리며 Room의 Mid 밀도를 기록
	FDecayMeasurement Measure(FAcousticDiffusionSimulator& Sim, RoomId Room, float MaxDuration = 30.f, float Dt = 1.f / 120.f)
	{
		FDecayMeter Meter;
		Meter.Start(MaxDuration);
		while (Meter.IsRunning())
		{
			Sim.Tick(Dt);
			Meter.AddSample(Dt, Sim.GetEnergyDensity(Room).Mid);
		}
		return Meter.GetResult();
	}

	void LogResult(const char* Label, const FDecayMeasurement& R)
	{
		LOG("%s: 피크 %.3f s, EDT %.2f s, T20 %.2f s, 후기(-40~-60) %.2f s, 측정 %.1f s%s",
			Label, R.PeakTime, R.EDT, R.T20, R.LateRT, R.Duration, R.IsDoubleSlope() ? " [이중 기울기]" : "");
	}
}

// 방 하나는 순수 지수 감쇠 → 세 지표가 모두 Sabine RT60과 같아야 한다
TEST_CASE(Decay_SingleRoom_AllMetricsMatchSabine)
{
	FAcousticRoomGraph Graph;
	const RoomId Room = Graph.AddRoom(MakeBoxRoom("Box", FVec3(), 10.f, 8.f, 3.f, MaterialPresets::Concrete()));
	FAcousticDiffusionSimulator Sim(Graph);
	Sim.InjectEnergy(Room, { 0.f, 1.f, 0.f });

	// 기준값은 시뮬레이터와 같은 물리로 (공기 흡음 포함)
	const float RT60 = SabineRT60(*Graph.GetRoom(Room), SpeedOfSound, Sim.GetSettings().AirAbsorptionCoefficient).Mid;
	const FDecayMeasurement R = Measure(Sim, Room);
	LogResult("콘크리트 방", R);

	CHECK(R.bComplete);
	CHECK_NEAR(R.EDT, RT60, 0.01 * RT60);
	CHECK_NEAR(R.T20, RT60, 0.01 * RT60);
	CHECK_NEAR(R.LateRT, RT60, 0.01 * RT60);
	CHECK(!R.IsDoubleSlope());
}

// 카펫 옷장 + 콘크리트 홀: 초기는 옷장, 후기는 홀을 따라야 한다
TEST_CASE(Decay_ClosetAndHall_DetectsDoubleSlope)
{
	FAcousticRoomGraph Graph;
	const RoomId Closet = Graph.AddRoom(MakeBoxRoom("Closet", FVec3(), 3.f, 3.f, 2.5f, MaterialPresets::Carpet()));
	const RoomId Hall = Graph.AddRoom(MakeBoxRoom("Hall", FVec3(), 20.f, 20.f, 10.f, MaterialPresets::Concrete()));
	Graph.AddPortal(Closet, Hall, 0.9f * 2.1f);
	FAcousticDiffusionSimulator Sim(Graph);
	Sim.InjectEnergy(Closet, { 0.f, 1.f, 0.f });

	const FBandValues AirM = Sim.GetSettings().AirAbsorptionCoefficient;
	const float ClosetRT60 = SabineRT60(*Graph.GetRoom(Closet), SpeedOfSound, AirM).Mid;
	const float HallRT60 = SabineRT60(*Graph.GetRoom(Hall), SpeedOfSound, AirM).Mid;
	const FDecayMeasurement R = Measure(Sim, Closet);
	LogResult("옷장 (홀과 연결)", R);

	CHECK(R.bComplete);
	CHECK(R.EDT < 1.5f * ClosetRT60);
	CHECK_NEAR(R.LateRT, HallRT60, 0.2 * HallRT60);
	CHECK(R.IsDoubleSlope());
}

// 옆방에서 난 소리는 에너지가 넘어오는 동안 커지다가 줄어든다 → 피크가 늦게 온다
TEST_CASE(Decay_EventInNeighborRoom_PeakArrivesLater)
{
	FAcousticRoomGraph Graph;
	const RoomId A = Graph.AddRoom(MakeBoxRoom("A", FVec3(), 5.f, 4.f, 3.f, MaterialPresets::Concrete()));
	const RoomId B = Graph.AddRoom(MakeBoxRoom("B", FVec3(), 5.f, 4.f, 3.f, MaterialPresets::Concrete()));
	Graph.AddPortal(A, B, 2.f);
	FAcousticDiffusionSimulator Sim(Graph);
	Sim.InjectEnergy(A, { 0.f, 1.f, 0.f });

	const FDecayMeasurement R = Measure(Sim, B);
	LogResult("옆방에서 측정", R);
	CHECK(R.bComplete);
	CHECK(R.PeakTime > 0.05f);
	CHECK(R.EDT > 0.f);
}

// 흡음이 없으면 영원히 줄지 않는다 → 최대 시간에서 멈추고 지표는 "측정 불가"
TEST_CASE(Decay_NoAbsorption_StopsAtMaxDuration)
{
	FAcousticRoomGraph Graph;
	const RoomId Room = Graph.AddRoom(MakeBoxRoom("NoAbs", FVec3(), 5.f, 5.f, 5.f, { "None", FBandValues::Uniform(0.f) }));
	FAcousticDiffusionSimulator Sim(Graph);
	Sim.InjectEnergy(Room, { 0.f, 1.f, 0.f });

	const FDecayMeasurement R = Measure(Sim, Room, 2.f);
	CHECK(R.bComplete);
	CHECK_NEAR(R.Duration, 2.0, 0.02);
	CHECK(R.EDT < 0.f);
	CHECK(R.LateRT < 0.f);
}
