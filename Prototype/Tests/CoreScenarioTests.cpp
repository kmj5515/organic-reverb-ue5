// CoreScenarioTests.cpp
// ---------------------------------------------------------
// 실제 게임 공간을 흉내 낸 시나리오 검증 + 리버브 파라미터 매핑 + 성능.
// ---------------------------------------------------------
#include "../TestFramework.h"
#include "AcousticCore/ReverbParameterMapper.h"

#include <chrono>
#include <cstdint>
#include <string>

using namespace Acoustic;

namespace
{
	float ToDb(float Ratio) { return Ratio > 0.f ? 10.f * std::log10(Ratio) : -999.f; }

	// Series[i] (dB)가 처음으로 Threshold 이하가 되는 시간. 못 찾으면 -1.
	float FirstCrossingTime(const std::vector<float>& SeriesDb, float Dt, float ThresholdDb)
	{
		for (size_t I = 0; I < SeriesDb.size(); ++I)
		{
			if (SeriesDb[I] <= ThresholdDb) return static_cast<float>(I) * Dt;
		}
		return -1.f;
	}
}

// BODYCAM 히트맵 재현: 복도 6구간(C0~C5) + 홀수 구간에 문으로 연결된 옆방(R1, R3, R5).
// C0에서 총성 → 복도를 따라 흐르며 식고, 옆방으로는 문을 통해 "새어 들어가야" 한다.
TEST_CASE(Scenario_CorridorPropagation_Heatmap)
{
	FAcousticRoomGraph Graph;
	std::vector<RoomId> Corridor;
	std::vector<RoomId> Side;
	std::vector<size_t> SideParent;

	for (int I = 0; I < 6; ++I)
	{
		Corridor.push_back(Graph.AddRoom(MakeBoxRoom("C" + std::to_string(I), FVec3(I * 8.f, 0.f, 0.f), 8.f, 2.5f, 3.f, MaterialPresets::Concrete())));
		if (I > 0) Graph.AddPortal(Corridor[I - 1], Corridor[I], 2.5f * 3.f); // 복도 단면 전체가 개구부
	}
	for (int I : { 1, 3, 5 })
	{
		Side.push_back(Graph.AddRoom(MakeBoxRoom("R" + std::to_string(I), FVec3(I * 8.f, 5.f, 0.f), 6.f, 5.f, 3.f, MaterialPresets::Wood())));
		SideParent.push_back(static_cast<size_t>(I));
		Graph.AddPortal(Corridor[I], Side.back(), 0.9f * 2.1f); // 문
	}

	FAcousticDiffusionSimulator Sim(Graph);
	Sim.InjectEnergy(Corridor[0], { 0.f, 1.f, 0.f });
	const float ReferenceDensity = Sim.GetEnergyDensity(Corridor[0]).Mid;

	const size_t NumRooms = Graph.NumRooms();
	std::vector<float> PeakDensity(NumRooms, 0.f), PeakTime(NumRooms, 0.f);
	PeakDensity[Corridor[0]] = ReferenceDensity;

	const float SampleTimes[] = { 0.005f, 0.02f, 0.05f, 0.1f, 0.2f, 0.5f, 1.f, 2.f };
	size_t NextSample = 0;

	std::string Header = "      t |";
	for (const FRoom& Room : Graph.GetRooms()) { char Buf[16]; std::snprintf(Buf, sizeof(Buf), "%6s", Room.Name.c_str()); Header += Buf; }
	LOG("에너지 밀도 히트맵 (C0 주입 직후 대비 dB, Mid 대역)");
	LOG("%s", Header.c_str());

	const float Dt = 0.001f;
	for (int Step = 1; Step <= 2000; ++Step)
	{
		Sim.Tick(Dt);
		const float T = Step * Dt;
		for (RoomId Id = 0; Id < NumRooms; ++Id)
		{
			const float W = Sim.GetEnergyDensity(Id).Mid;
			if (W > PeakDensity[Id]) { PeakDensity[Id] = W; PeakTime[Id] = T; }
		}

		if (NextSample < std::size(SampleTimes) && T >= SampleTimes[NextSample] - 1e-6f)
		{
			std::string Row;
			char Buf[32];
			std::snprintf(Buf, sizeof(Buf), "%6.3fs |", T);
			Row += Buf;
			for (RoomId Id = 0; Id < NumRooms; ++Id)
			{
				std::snprintf(Buf, sizeof(Buf), "%6.0f", ToDb(Sim.GetEnergyDensity(Id).Mid / ReferenceDensity));
				Row += Buf;
			}
			LOG("%s", Row.c_str());
			++NextSample;
		}
	}

	// 복도를 따라 갈수록 피크가 늦게, 약하게 도착해야 한다
	for (size_t I = 1; I < Corridor.size(); ++I)
	{
		CHECK(PeakTime[Corridor[I]] > PeakTime[Corridor[I - 1]]);
		CHECK(PeakDensity[Corridor[I]] < PeakDensity[Corridor[I - 1]]);
	}
	// 옆방은 연결된 복도 구간보다 늦게, 약하게
	for (size_t K = 0; K < Side.size(); ++K)
	{
		const RoomId Parent = Corridor[SideParent[K]];
		LOG("%s: 피크 %.0fms / %.1f dB  (연결 복도 %s: %.0fms / %.1f dB)",
			Graph.GetRoom(Side[K])->Name.c_str(), PeakTime[Side[K]] * 1000.f, ToDb(PeakDensity[Side[K]] / ReferenceDensity),
			Graph.GetRoom(Parent)->Name.c_str(), PeakTime[Parent] * 1000.f, ToDb(PeakDensity[Parent] / ReferenceDensity));
		CHECK(PeakTime[Side[K]] > PeakTime[Parent]);
		CHECK(PeakDensity[Side[K]] < PeakDensity[Parent]);
	}
}

// 결합 공간의 이중 기울기 감쇠(double-slope decay):
// 흡음이 강한 작은 방(카펫)이 잔향이 긴 큰 홀(콘크리트)과 문으로 연결되면,
// 초기엔 작은 방 자체 RT60로 빠르게 떨어지다가, 후반엔 홀에서 되돌아오는 에너지 때문에 홀의 RT60로 길게 꼬리를 끈다.
// → 프리셋 스위칭(Reverb Volume)으로는 표현 불가능한 "오가닉" 특성.
TEST_CASE(Scenario_CoupledRooms_DoubleSlopeDecay)
{
	FAcousticRoomGraph Graph;
	const RoomId Closet = Graph.AddRoom(MakeBoxRoom("Closet", FVec3(), 3.f, 3.f, 2.5f, MaterialPresets::Carpet()));
	const RoomId Hall = Graph.AddRoom(MakeBoxRoom("Hall", FVec3(), 20.f, 20.f, 10.f, MaterialPresets::Concrete()));
	Graph.AddPortal(Closet, Hall, 0.9f * 2.1f);
	FAcousticDiffusionSimulator Sim(Graph);

	Sim.InjectEnergy(Closet, { 0.f, 1.f, 0.f });
	const float W0 = Sim.GetEnergyDensity(Closet).Mid;

	const float Dt = 1.f / 120.f;
	std::vector<float> SeriesDb;
	for (int I = 0; I < 8 * 120; ++I)
	{
		Sim.Tick(Dt);
		SeriesDb.push_back(ToDb(Sim.GetEnergyDensity(Closet).Mid / W0));
	}

	const FBandValues AirM = Sim.GetSettings().AirAbsorptionCoefficient; // 기준값도 시뮬레이터와 같은 물리로
	const float OwnRT60 = SabineRT60(*Graph.GetRoom(Closet), SpeedOfSound, AirM).Mid;
	const float HallRT60 = SabineRT60(*Graph.GetRoom(Hall), SpeedOfSound, AirM).Mid;
	const float EDT = 6.f * FirstCrossingTime(SeriesDb, Dt, -10.f);                                              // 0 → -10 dB 구간 ×6
	const float LateRT = 6.f * (FirstCrossingTime(SeriesDb, Dt, -55.f) - FirstCrossingTime(SeriesDb, Dt, -45.f)); // -45 → -55 dB 구간 ×6

	LOG("작은 방 자체 RT60=%.2fs, 홀 RT60=%.2fs", OwnRT60, HallRT60);
	LOG("측정: 초기 감쇠 EDT=%.2fs, 후기 감쇠 RT=%.2fs", EDT, LateRT);
	CHECK(FirstCrossingTime(SeriesDb, Dt, -55.f) > 0.f);
	CHECK(EDT < 1.5f * OwnRT60);
	CHECK(LateRT > 5.f * OwnRT60);
	CHECK_NEAR(LateRT, HallRT60, 0.2 * HallRT60);
}

TEST_CASE(Mapper_ProducesPhysicallyOrderedParams)
{
	const FRoom Concrete = MakeBoxRoom("Concrete", FVec3(), 10.f, 8.f, 3.f, MaterialPresets::Concrete());
	const FRoom Carpet = MakeBoxRoom("Carpet", FVec3(), 10.f, 8.f, 3.f, MaterialPresets::Carpet());
	const FRoom BigHall = MakeBoxRoom("Hall", FVec3(), 20.f, 20.f, 10.f, MaterialPresets::Concrete());
	const FRoom Anechoic = MakeBoxRoom("NoAbs", FVec3(), 5.f, 5.f, 5.f, { "None", FBandValues::Uniform(0.f) });

	// WetLevel: dB 스케일 (기준 밀도 1 = 1.0, -30dB = 0.5, 에너지 0 = 0)
	CHECK(MapToReverbParams(Concrete, FBandValues()).WetLevel == 0.f);
	CHECK_NEAR(MapToReverbParams(Concrete, { 0.f, 1.f, 0.f }).WetLevel, 1.0, 1e-6);
	CHECK_NEAR(MapToReverbParams(Concrete, { 0.f, 1e-3f, 0.f }).WetLevel, 0.5, 1e-4);

	// 벽 흡음만 두고 교과서 식과 대조 (공기 흡음은 아래에서 따로 본다)
	FReverbMappingSettings NoAir;
	NoAir.AirAbsorptionCoefficient = FBandValues();

	const FReverbParams PConcrete = MapToReverbParams(Concrete, FBandValues(), NoAir);
	const FReverbParams PCarpet = MapToReverbParams(Carpet, FBandValues(), NoAir);
	LOG("콘크리트 10x8x3: Decay=%.2fs HFRatio=%.2f ReflDelay=%.1fms", PConcrete.DecayTime, PConcrete.DecayHFRatio, PConcrete.ReflectionsDelay * 1000.f);
	LOG("카펫     10x8x3: Decay=%.2fs HFRatio=%.2f ReflDelay=%.1fms", PCarpet.DecayTime, PCarpet.DecayHFRatio, PCarpet.ReflectionsDelay * 1000.f);

	CHECK_NEAR(PConcrete.DecayTime, SabineRT60(Concrete).Mid, 1e-4);
	CHECK(PCarpet.DecayTime < PConcrete.DecayTime);
	CHECK_NEAR(PConcrete.DecayHFRatio, 0.03 / 0.04, 1e-3);
	CHECK(PCarpet.DecayHFRatio < PConcrete.DecayHFRatio);
	CHECK_NEAR(PConcrete.ReflectionsDelay, 4.0 * 240.0 / 268.0 / 343.0, 1e-4);
	CHECK(MapToReverbParams(BigHall, FBandValues(), NoAir).ReflectionsDelay > PConcrete.ReflectionsDelay);

	// 흡음 0 → RT60 무한대여도 클램프된 유한값이 나와야 함
	const FReverbParams PAnechoic = MapToReverbParams(Anechoic, FBandValues(), NoAir);
	CHECK(PAnechoic.DecayTime == 20.f);
	CHECK_NEAR(PAnechoic.DecayHFRatio, 1.0, 1e-6);
}

// 공기 흡음 (6단계)이 리버브 파라미터에 어떻게 나타나는가:
// 큰 공간일수록 Decay가 짧아지고, 고역이 더 많이 깎여 HF Decay Ratio가 내려간다.
TEST_CASE(Mapper_AirAbsorption_ShortensLargeSpacesAndLowersHFRatio)
{
	FReverbMappingSettings NoAir;
	NoAir.AirAbsorptionCoefficient = FBandValues();

	const FRoom Small = MakeBoxRoom("Small", FVec3(), 5.f, 4.f, 3.f, MaterialPresets::Concrete());
	const FRoom Hall = MakeBoxRoom("Hall", FVec3(), 30.f, 30.f, 10.f, MaterialPresets::Concrete());

	const FReverbParams SmallOff = MapToReverbParams(Small, FBandValues(), NoAir);
	const FReverbParams SmallOn = MapToReverbParams(Small, FBandValues());
	const FReverbParams HallOff = MapToReverbParams(Hall, FBandValues(), NoAir);
	const FReverbParams HallOn = MapToReverbParams(Hall, FBandValues());

	LOG("작은 방: Decay %.2f → %.2f s, HFRatio %.2f → %.2f", SmallOff.DecayTime, SmallOn.DecayTime, SmallOff.DecayHFRatio, SmallOn.DecayHFRatio);
	LOG("30 m 홀: Decay %.2f → %.2f s, HFRatio %.2f → %.2f", HallOff.DecayTime, HallOn.DecayTime, HallOff.DecayHFRatio, HallOn.DecayHFRatio);

	CHECK(SmallOn.DecayTime < SmallOff.DecayTime);
	CHECK(HallOn.DecayTime < HallOff.DecayTime);
	CHECK(1.f - HallOn.DecayTime / HallOff.DecayTime > 1.f - SmallOn.DecayTime / SmallOff.DecayTime); // 큰 공간일수록 많이 짧아진다
	CHECK(HallOn.DecayHFRatio < SmallOn.DecayHFRatio);
	CHECK(SmallOn.DecayHFRatio < SmallOff.DecayHFRatio);
	CHECK_NEAR(SmallOff.DecayHFRatio, HallOff.DecayHFRatio, 1e-3); // 공기가 없으면 크기와 무관하게 α 비율(0.75)로 같다

	// 반사 지연(평균 자유 행로)은 기하학만의 값이라 공기 흡음과 무관해야 한다
	CHECK_NEAR(HallOn.ReflectionsDelay, HallOff.ReflectionsDelay, 1e-6);
}

// 목표: 1000개 방 / ~2000개 포탈에서 Tick 1회 < 1ms (게임 스레드 예산의 극히 일부)
TEST_CASE(Perf_1000Rooms_TickCost)
{
	FAcousticRoomGraph Graph;
	uint32_t Seed = 12345;
	auto Rand01 = [&Seed]() { Seed = Seed * 1664525u + 1013904223u; return static_cast<float>(Seed >> 8) / 16777216.f; };

	const int NumRooms = 1000;
	for (int I = 0; I < NumRooms; ++I)
	{
		Graph.AddRoom(MakeBoxRoom("Room", FVec3(), 3.f + 12.f * Rand01(), 3.f + 12.f * Rand01(), 2.5f + 3.f * Rand01(), MaterialPresets::Wood()));
		if (I > 0) Graph.AddPortal(I - 1, I, 1.f + 5.f * Rand01());
	}
	for (int I = 0; I < NumRooms; ++I)
	{
		const RoomId Other = static_cast<RoomId>(Rand01() * NumRooms) % NumRooms;
		Graph.AddPortal(I, Other, 1.f + 3.f * Rand01()); // 자기 자신이 뽑히면 거부됨
	}

	FAcousticDiffusionSimulator Sim(Graph);
	Sim.InjectEnergy(0, FBandValues::Uniform(100.f));
	for (int I = 0; I < 10; ++I) Sim.Tick(1.f / 60.f); // 워밍업

	const int Ticks = 600;
	const auto Start = std::chrono::steady_clock::now();
	for (int I = 0; I < Ticks; ++I) Sim.Tick(1.f / 60.f);
	const double TotalUs = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - Start).count();

	LOG("방 %zu개, 포탈 %zu개: Tick 평균 %.1f us", Graph.NumRooms(), Graph.NumPortals(), TotalUs / Ticks);
	CHECK(TotalUs / Ticks < 1000.0);
}
