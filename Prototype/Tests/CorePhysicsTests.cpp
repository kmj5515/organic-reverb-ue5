// CorePhysicsTests.cpp
// ---------------------------------------------------------
// AcousticCore 시뮬레이터의 물리적 정합성 검증:
// Sabine 일치, 에너지 보존, 밀도 평형, 무조건 안정성, 대역별 감쇠, 문 개폐, 입력 검증.
// ---------------------------------------------------------
#include "../TestFramework.h"
#include "AcousticCore/AcousticDiffusionSimulator.h"

using namespace Acoustic;

namespace
{
	FAcousticMaterial NoAbsorption() { return { "None", FBandValues::Uniform(0.f) }; }

	// 정육면체 방
	FRoom CubeRoom(float Volume, const FAcousticMaterial& Material = NoAbsorption())
	{
		const float Side = std::cbrt(Volume);
		return MakeBoxRoom("Cube", FVec3(), Side, Side, Side, Material);
	}

	void TickFor(FAcousticDiffusionSimulator& Sim, float Seconds, float Dt)
	{
		const int Steps = static_cast<int>(std::lround(Seconds / Dt));
		for (int I = 0; I < Steps; ++I) Sim.Tick(Dt);
	}

	// 공기 흡음을 끈 설정. 포탈 교환만 따로 떼어 검증할 때 쓴다 (공기가 있으면 총 에너지가 보존되지 않는다)
	FDiffusionSettings NoAir()
	{
		FDiffusionSettings S;
		S.AirAbsorptionCoefficient = FBandValues();
		return S;
	}
}

TEST_CASE(Core_SabineRT60_MatchesTextbookFormula)
{
	const FRoom Room = MakeBoxRoom("Box", FVec3(), 10.f, 8.f, 3.f, MaterialPresets::Concrete());
	CHECK_NEAR(Room.Volume, 240.0, 1e-3);
	CHECK_NEAR(Room.SurfaceArea, 268.0, 1e-3);

	// 교과서 식 0.161·V/A 와 비교 (0.161은 c=343 기준 0.16111의 반올림값)
	const FBandValues RT60 = SabineRT60(Room);
	CHECK_NEAR(RT60.Mid, 0.161 * 240.0 / (268.0 * 0.03), 0.01);
	CHECK(std::isinf(SabineRT60(10.f, 30.f, 0.f)));
}

// 흡음은 지수 해석해를 쓰므로 스텝 크기와 무관하게 RT60 시점에 정확히 -60dB 여야 한다.
TEST_CASE(Core_SingleRoom_Reaches60dBExactlyAtRT60)
{
	for (int StepsPerRT : { 600, 7 })
	{
		FAcousticRoomGraph Graph;
		const RoomId Id = Graph.AddRoom(MakeBoxRoom("Box", FVec3(), 10.f, 8.f, 3.f, MaterialPresets::Concrete()));
		FAcousticDiffusionSimulator Sim(Graph);

		const float RT60 = SabineRT60(*Graph.GetRoom(Id), SpeedOfSound, Sim.GetSettings().AirAbsorptionCoefficient).Mid;
		Sim.InjectEnergy(Id, { 0.f, 1.f, 0.f });
		for (int I = 0; I < StepsPerRT; ++I) Sim.Tick(RT60 / StepsPerRT);

		const float Db = 10.f * std::log10(Graph.GetRoom(Id)->Energy.Mid);
		LOG("RT60을 %d 스텝(dt=%.3fs)으로 진행 → %.3f dB", StepsPerRT, RT60 / StepsPerRT, Db);
		CHECK_NEAR(Db, -60.0, 0.1);
	}
}

// 부피가 100배 다른 두 방: 총 에너지는 보존되고, 평형에서 "밀도"가 같아져야 한다.
TEST_CASE(Core_Conservation_EqualizesDensityNotEnergy)
{
	for (float Dt : { 1.f / 60.f, 0.5f })
	{
		FAcousticRoomGraph Graph;
		const RoomId Small = Graph.AddRoom(CubeRoom(10.f));
		const RoomId Large = Graph.AddRoom(CubeRoom(1000.f));
		Graph.AddPortal(Small, Large, 1.f);
		FAcousticDiffusionSimulator Sim(Graph, NoAir()); // 벽 흡음도 공기 흡음도 없어야 "보존"을 잴 수 있다

		Sim.InjectEnergy(Large, { 0.f, 1.f, 0.f });
		TickFor(Sim, 10.f, Dt);

		const float Ratio = Sim.GetEnergyDensity(Small).Mid / Sim.GetEnergyDensity(Large).Mid;
		LOG("dt=%.3f: 총 에너지=%.6f, 밀도비(작은/큰)=%.5f", Dt, Sim.GetTotalEnergy().Mid, Ratio);
		CHECK_NEAR(Sim.GetTotalEnergy().Mid, 1.0, 1e-4);
		CHECK_NEAR(Ratio, 1.0, 1e-3);
	}
}

// v0 결함 3의 반대: 거대한 개구부 + 극단적 DeltaTime에서도 음수/발산/에너지 생성/역전(overshoot)이 없어야 한다.
TEST_CASE(Core_Stability_HugePortalAndHitch)
{
	FAcousticRoomGraph Graph;
	const RoomId A = Graph.AddRoom(CubeRoom(5.f, MaterialPresets::Carpet()));
	const RoomId B = Graph.AddRoom(CubeRoom(5.f, MaterialPresets::Carpet()));
	const RoomId C = Graph.AddRoom(CubeRoom(5.f, MaterialPresets::Carpet()));
	Graph.AddPortal(A, B, 20.f);
	Graph.AddPortal(B, C, 20.f);
	FAcousticDiffusionSimulator Sim(Graph);
	Sim.InjectEnergy(A, FBandValues::Uniform(1.f));

	const float DtPattern[] = { 0.2f, 1.f / 60.f, 0.5f, 0.001f, 2.f };
	float PrevTotal = Sim.GetTotalEnergy().Sum();
	bool bNegative = false, bEnergyCreated = false, bOrderBroken = false;

	for (int I = 0; I < 50; ++I)
	{
		Sim.Tick(DtPattern[I % 5]);
		for (const FRoom& Room : Graph.GetRooms())
		{
			for (int Band = 0; Band < NumBands; ++Band) bNegative |= Room.Energy[Band] < 0.f;
		}

		const float Total = Sim.GetTotalEnergy().Sum();
		bEnergyCreated |= Total > PrevTotal * (1.f + 1e-5f);
		PrevTotal = Total;

		// 에너지는 A → B → C 로만 흘러가므로 밀도 순서 A >= B >= C 가 절대 뒤집히면 안 된다
		const float WA = Sim.GetEnergyDensity(A).Mid, WB = Sim.GetEnergyDensity(B).Mid, WC = Sim.GetEnergyDensity(C).Mid;
		bOrderBroken |= WA < WB * (1.f - 1e-4f) || WB < WC * (1.f - 1e-4f);
	}

	CHECK(!bNegative);
	CHECK(!bEnergyCreated);
	CHECK(!bOrderBroken);
}

// 공기 흡음 (6단계): 벽에 닿지 않아도 공기 중에서 잃는 에너지 = Sabine 분모의 4mV 항.
// 감쇠율에 c·m 이 더해지는 것과 같으므로, 흡음과 마찬가지로 지수 해석해가 그대로 성립해야 한다.
TEST_CASE(Core_AirAbsorption_MatchesSabineFourMVTerm)
{
	const FBandValues AirM = AirAbsorption::Coefficient();
	CHECK_NEAR(AirM.Mid, 0.005 * 0.23025851, 1e-9); // 0.005 dB/m → m (1/m)

	// 30 x 30 x 10 m 콘크리트 홀: V = 9000 m^3, S = 3000 m^2, A_mid = 90 m^2, 4mV_mid ≈ 41.4 m^2
	const FRoom Hall = MakeBoxRoom("Hall", FVec3(), 30.f, 30.f, 10.f, MaterialPresets::Concrete());
	const float WallOnly = 0.161f * Hall.Volume / (Hall.SurfaceArea * 0.03f);
	const float WithAir = 0.161f * Hall.Volume / (Hall.SurfaceArea * 0.03f + 4.f * AirM.Mid * Hall.Volume);
	CHECK_NEAR(SabineRT60(Hall).Mid, WallOnly, 0.01 * WallOnly);                            // 인자를 안 주면 교과서 Sabine 그대로
	CHECK_NEAR(SabineRT60(Hall, SpeedOfSound, AirM).Mid, WithAir, 0.01 * WithAir);
	LOG("30x30x10 콘크리트 홀 RT60(Mid): 벽만 %.2f s → 공기 흡음 포함 %.2f s", WallOnly, WithAir);

	// 시뮬레이터(기본 설정 = 공기 흡음 켜짐)도 그 RT60 시점에 정확히 -60 dB
	FAcousticRoomGraph Graph;
	const RoomId Id = Graph.AddRoom(Hall);
	FAcousticDiffusionSimulator Sim(Graph);
	const float RT60 = SabineRT60(Hall, SpeedOfSound, Sim.GetSettings().AirAbsorptionCoefficient).Mid;
	Sim.InjectEnergy(Id, { 0.f, 1.f, 0.f });
	for (int I = 0; I < 300; ++I) Sim.Tick(RT60 / 300.f);
	CHECK_NEAR(10.f * std::log10(Graph.GetRoom(Id)->Energy.Mid), -60.0, 0.1);

	// 끄면 예전(순수 Sabine) 거동으로 정확히 되돌아간다
	FAcousticRoomGraph OffGraph;
	const RoomId OffId = OffGraph.AddRoom(Hall);
	FAcousticDiffusionSimulator OffSim(OffGraph, NoAir());
	OffSim.InjectEnergy(OffId, { 0.f, 1.f, 0.f });
	for (int I = 0; I < 300; ++I) OffSim.Tick(WallOnly / 300.f);
	CHECK_NEAR(10.f * std::log10(OffGraph.GetRoom(OffId)->Energy.Mid), -60.0, 0.1);
}

// 공기 흡음은 방 크기와 무관한 상수 감쇠(c·m)라, 벽 항(c·A/4V)이 작아지는 큰 공간일수록 지배적이다.
// → 작은 방은 거의 그대로, 큰 홀은 크게 짧아지고, 특히 고역이 먼저 죽는다.
TEST_CASE(Core_AirAbsorption_HitsLargeRoomsAndHighsHardest)
{
	const FBandValues AirM = AirAbsorption::Coefficient();
	auto Shortening = [&](const FRoom& Room, int Band)
	{
		const float Wall = SabineRT60(Room.Volume, Room.SurfaceArea, Room.Material.Absorption[Band]);
		const float Air = SabineRT60(Room.Volume, Room.SurfaceArea, Room.Material.Absorption[Band], SpeedOfSound, AirM[Band]);
		return 1.f - Air / Wall; // 줄어든 비율
	};

	const FRoom Small = MakeBoxRoom("Small", FVec3(), 5.f, 4.f, 3.f, MaterialPresets::Concrete());
	const FRoom Hall = MakeBoxRoom("Hall", FVec3(), 30.f, 30.f, 10.f, MaterialPresets::Concrete());

	LOG("RT60 단축률  작은 방: Mid %.0f%% High %.0f%%  |  큰 홀: Mid %.0f%% High %.0f%%",
		Shortening(Small, 1) * 100.f, Shortening(Small, 2) * 100.f, Shortening(Hall, 1) * 100.f, Shortening(Hall, 2) * 100.f);

	CHECK(Shortening(Small, 1) < 0.15f);           // 5x4x3 방은 10% 안팎
	CHECK(Shortening(Hall, 1) > 0.25f);            // 30 m 홀은 30% 이상
	CHECK(Shortening(Hall, 1) > Shortening(Small, 1));
	CHECK(Shortening(Hall, 2) > Shortening(Hall, 1)); // 고역이 더 많이 깎인다 (m_high ≈ 5 × m_mid)
}

// 전달 지연 (6-5): 확산 모델 자체에는 전파 속도가 없다. 포탈을 지난 에너지가 받는 방 가운데까지
// 가는 시간만큼 늦게 도착해야 한다 (= 방 중심 사이 거리 / 음속).
TEST_CASE(Core_PortalDelay_EnergyArrivesAfterTravelTime)
{
	auto FirstArrival = [](bool bDelay)
	{
		FAcousticRoomGraph Graph;
		// 34.3 m 떨어진 두 방 → 소리가 지나는 데 정확히 0.1초
		const RoomId A = Graph.AddRoom(MakeBoxRoom("A", FVec3(0.f, 0.f, 0.f), 6.f, 5.f, 3.f, MaterialPresets::Concrete()));
		const RoomId B = Graph.AddRoom(MakeBoxRoom("B", FVec3(34.3f, 0.f, 0.f), 6.f, 5.f, 3.f, MaterialPresets::Concrete()));
		Graph.AddPortal(A, B, 2.f);

		FDiffusionSettings S;
		S.bPortalDelay = bDelay;
		FAcousticDiffusionSimulator Sim(Graph, S);
		Sim.InjectEnergy(A, { 0.f, 1.f, 0.f });

		const float Dt = 1.f / 240.f;
		for (int I = 0; I < 240; ++I) // 1초
		{
			Sim.Tick(Dt);
			if (Graph.GetRoom(B)->Energy.Mid > 0.f) return (I + 1) * Dt;
		}
		return -1.f;
	};

	const float Instant = FirstArrival(false);
	const float Delayed = FirstArrival(true);
	LOG("34.3 m 떨어진 옆방 첫 도착: 지연 없음 %.3f s → 지연 적용 %.3f s (음속 기준 0.100 s)", Instant, Delayed);

	CHECK(Instant >= 0.f && Instant < 0.01f); // 예전 모델은 한 틱 만에 도달
	CHECK_NEAR(Delayed, 0.1, 0.01);
}

// 지연을 넣어도 v0 결함(에너지 생성·음수·발산)이 되살아나면 안 된다.
// 가는 중인 에너지까지 세면 총량이 그대로여야 하고, 어떤 방도 음수가 되면 안 된다.
TEST_CASE(Core_PortalDelay_StaysStableAndConserves)
{
	FAcousticRoomGraph Graph;
	const RoomId A = Graph.AddRoom(MakeBoxRoom("A", FVec3(0.f, 0.f, 0.f), 5.f, 5.f, 5.f, NoAbsorption()));
	const RoomId B = Graph.AddRoom(MakeBoxRoom("B", FVec3(20.f, 0.f, 0.f), 5.f, 5.f, 5.f, NoAbsorption()));
	const RoomId C = Graph.AddRoom(MakeBoxRoom("C", FVec3(40.f, 0.f, 0.f), 50.f, 5.f, 5.f, NoAbsorption()));
	Graph.AddPortal(A, B, 20.f); // 거대한 개구부 = 가장 세게 흔드는 조건
	Graph.AddPortal(B, C, 20.f);

	FDiffusionSettings S;
	S.AirAbsorptionCoefficient = FBandValues(); // 보존을 재려면 흡음이 전혀 없어야 한다
	FAcousticDiffusionSimulator Sim(Graph, S);
	Sim.InjectEnergy(A, FBandValues::Uniform(1.f));

	const float DtPattern[] = { 0.2f, 1.f / 60.f, 0.5f, 0.001f, 2.f };
	bool bNegative = false, bEnergyCreated = false;
	float MaxTotal = 0.f;
	for (int I = 0; I < 50; ++I)
	{
		Sim.Tick(DtPattern[I % 5]);
		for (const FRoom& Room : Graph.GetRooms())
		{
			for (int Band = 0; Band < NumBands; ++Band) bNegative |= Room.Energy[Band] < 0.f;
		}
		const float Total = Sim.GetTotalEnergy().Sum();
		MaxTotal = std::max(MaxTotal, Total);
		bEnergyCreated |= Total > 3.f * (1.f + 1e-4f); // 3대역 × 1.0
	}

	LOG("지연 적용: 총 에너지 최대 %.6f (주입 3.000), 마지막 %.6f, 가는 중 %.6f",
		MaxTotal, Sim.GetTotalEnergy().Sum(), Sim.GetInFlightEnergy().Sum());
	CHECK(!bNegative);
	CHECK(!bEnergyCreated);
	CHECK_NEAR(Sim.GetTotalEnergy().Sum(), 3.0, 1e-3);

	// 충분히 지나면 다 도착하고 밀도가 같아진다 (지연은 늦출 뿐 평형을 바꾸지 않는다)
	TickFor(Sim, 10.f, 1.f / 60.f);
	CHECK_NEAR(Sim.GetInFlightEnergy().Sum(), 0.0, 1e-4);
	CHECK_NEAR(Sim.GetEnergyDensity(A).Mid / Sim.GetEnergyDensity(C).Mid, 1.0, 1e-3);
}
TEST_CASE(Core_FrequencyBands_CarpetDampsHighsFaster)
{
	FAcousticRoomGraph Graph;
	const RoomId Id = Graph.AddRoom(MakeBoxRoom("CarpetRoom", FVec3(), 5.f, 4.f, 3.f, MaterialPresets::Carpet()));
	FAcousticDiffusionSimulator Sim(Graph);

	Sim.InjectEnergy(Id, FBandValues::Uniform(1.f));
	TickFor(Sim, 0.3f, 1.f / 60.f);

	const FBandValues E = Graph.GetRoom(Id)->Energy;
	LOG("0.3초 후 대역별 잔여 에너지: Low=%.3g Mid=%.3g High=%.3g", E.Low, E.Mid, E.High);
	CHECK(E.High < E.Mid);
	CHECK(E.Mid < E.Low);
}

TEST_CASE(Core_Portal_DoorTransmissionControlsLeak)
{
	auto LeakAfterHalfSecond = [](float Transmission)
	{
		FAcousticRoomGraph Graph;
		const RoomId A = Graph.AddRoom(CubeRoom(50.f, MaterialPresets::Concrete()));
		const RoomId B = Graph.AddRoom(CubeRoom(50.f, MaterialPresets::Concrete()));
		Graph.AddPortal(A, B, 2.f, Transmission);
		FAcousticDiffusionSimulator Sim(Graph);
		Sim.InjectEnergy(A, { 0.f, 1.f, 0.f });
		TickFor(Sim, 0.5f, 1.f / 60.f);
		return Graph.GetRoom(B)->Energy.Mid;
	};

	const float Closed = LeakAfterHalfSecond(0.f), Half = LeakAfterHalfSecond(0.5f), Open = LeakAfterHalfSecond(1.f);
	LOG("옆방 유입 에너지: 닫힘=%.4f, 반개방=%.4f, 개방=%.4f", Closed, Half, Open);
	CHECK(Closed == 0.f);
	CHECK(Half > 0.f);
	CHECK(Half < Open);

	// 런타임에 문을 여는 경우
	FAcousticRoomGraph Graph;
	const RoomId A = Graph.AddRoom(CubeRoom(50.f, MaterialPresets::Concrete()));
	const RoomId B = Graph.AddRoom(CubeRoom(50.f, MaterialPresets::Concrete()));
	const PortalId Door = Graph.AddPortal(A, B, 2.f, 0.f);
	FAcousticDiffusionSimulator Sim(Graph);
	Sim.InjectEnergy(A, { 0.f, 1.f, 0.f });
	TickFor(Sim, 0.5f, 1.f / 60.f);
	CHECK(Graph.GetRoom(B)->Energy.Mid == 0.f);
	Graph.GetPortal(Door)->Transmission = 1.f;
	TickFor(Sim, 0.1f, 1.f / 60.f);
	CHECK(Graph.GetRoom(B)->Energy.Mid > 0.f);
}

TEST_CASE(Core_Graph_RejectsInvalidInput)
{
	FAcousticRoomGraph Graph;
	CHECK(Graph.AddRoom(CubeRoom(0.f)) == InvalidRoomId);

	const RoomId A = Graph.AddRoom(CubeRoom(10.f));
	const RoomId B = Graph.AddRoom(CubeRoom(10.f));
	CHECK(Graph.AddPortal(A, A, 1.f) == InvalidPortalId);
	CHECK(Graph.AddPortal(A, 99, 1.f) == InvalidPortalId);
	CHECK(Graph.AddPortal(A, B, 0.f) == InvalidPortalId);
	CHECK(Graph.NumPortals() == 0);

	FAcousticDiffusionSimulator Sim(Graph);
	Sim.InjectEnergy(A, { 0.f, 1.f, 0.f });
	Sim.InjectEnergy(99, { 0.f, 1.f, 0.f }); // 무시되어야 함
	Sim.Tick(0.f);
	Sim.Tick(-1.f);
	Sim.Tick(std::nanf(""));
	CHECK(Graph.GetRoom(A)->Energy.Mid == 1.f);
}
