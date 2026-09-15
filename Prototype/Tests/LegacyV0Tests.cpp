// LegacyV0Tests.cpp
// ---------------------------------------------------------
// v0 설계(Docs/files/AcousticDiffusionSimulator.h)의 결함 "재현" 테스트.
// 결함이 재현되면 PASS — 새 AcousticCore가 왜 지금 구조인지에 대한 근거 자료.
//
// v0 헤더는 새 Core와 클래스 이름이 같으므로 V0 네임스페이스로 감싸서 격리한다 (ODR 충돌 방지).
// 표준 헤더를 먼저 include 해두면 v0 헤더 안의 #include <...>는 no-op이 된다.
// ---------------------------------------------------------
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "../TestFramework.h"

namespace V0
{
#include "../../Docs/files/AcousticDiffusionSimulator.h"
}

using namespace V0::Acoustic;

namespace
{
	FRoom MakeRoomV0(float Volume, float SurfaceArea, const FAcousticMaterial& Material)
	{
		FRoom Room;
		Room.Volume = Volume;
		Room.SurfaceArea = SurfaceArea;
		Room.DominantMaterial = Material;
		return Room;
	}

	const FAcousticMaterial NoAbsorption{ "None", 0.f, 0.f, 0.f };
	constexpr float V0DiffusionRate = 0.5f; // v0 private 멤버 기본값
}

// 결함 1: 에너지에 pow(1/1000, t/RT60)를 곱함 → RT60 동안 에너지 기준 -30dB만 감쇠.
//         -60dB(음압 1/1000)는 에너지로는 1/10^6 이어야 한다. 결과적으로 잔향이 2배 길게 들림.
TEST_CASE(V0_Finding1_DecayIsOnly30dBPerRT60)
{
	FAcousticRoomGraph Graph;
	const RoomId Room = Graph.AddRoom(MakeRoomV0(240.f, 268.f, MaterialPresets::Concrete())); // 10x8x3 콘크리트
	FAcousticDiffusionSimulator Sim(&Graph);

	const float RT60 = 0.161f * 240.f / (268.f * 0.03f);
	Sim.InjectEnergy(Room, { 0.f, 1.f, 0.f });

	const int Steps = static_cast<int>(std::lround(RT60 * 60.f));
	for (int I = 0; I < Steps; ++I) Sim.Tick(RT60 / Steps);

	const float Remaining = Graph.GetRoom(Room)->CurrentEnergy.Mid;
	LOG("RT60=%.3fs 경과 후 잔여 에너지 = %.3g (%.1f dB), 물리적 기대값 = 1e-06 (-60 dB)", RT60, Remaining, 10.f * std::log10(Remaining));
	CHECK_NEAR(Remaining, 1e-3, 1e-4);
}

// 결함 2: 방마다 인접 포탈을 순회 → 포탈 하나가 양쪽 방에서 두 번 처리되어 확산 속도가 의도의 2배.
TEST_CASE(V0_Finding2_PortalProcessedTwice)
{
	FAcousticRoomGraph Graph;
	const RoomId A = Graph.AddRoom(MakeRoomV0(100.f, 1.f, NoAbsorption));
	const RoomId B = Graph.AddRoom(MakeRoomV0(100.f, 1.f, NoAbsorption));
	Graph.AddPortal(A, B, 1.f, 0.f); // FlowRate = 0.5 * 1 / (1 + 0)
	FAcousticDiffusionSimulator Sim(&Graph);

	const float Dt = 0.01f;
	const float FlowRate = V0DiffusionRate * 1.f / (1.f + 0.f);
	Sim.InjectEnergy(A, { 0.f, 1.f, 0.f });
	Sim.Tick(Dt);

	const float Moved = Graph.GetRoom(B)->CurrentEnergy.Mid;
	LOG("한 틱 동안 이동한 에너지 = %.4f (의도 %.4f, 이중 처리 시 %.4f)", Moved, FlowRate * Dt, 2.f * FlowRate * Dt);
	CHECK_NEAR(Moved, 2.f * FlowRate * Dt, 1e-4);
}

// 결함 3: 명시적 Euler → 큰 개구부 + 프레임 드랍(DeltaTime 0.2s)에서 발산.
//         음수 에너지를 ClampNearZero가 0으로 잘라내면서 에너지가 "생성"된다.
TEST_CASE(V0_Finding3_ExplicitEulerCreatesEnergyOnHitch)
{
	FAcousticRoomGraph Graph;
	const RoomId A = Graph.AddRoom(MakeRoomV0(100.f, 1.f, NoAbsorption));
	const RoomId B = Graph.AddRoom(MakeRoomV0(100.f, 1.f, NoAbsorption));
	Graph.AddPortal(A, B, 10.f, 0.f); // FlowRate = 5 /s
	FAcousticDiffusionSimulator Sim(&Graph);

	Sim.InjectEnergy(A, { 0.f, 1.f, 0.f });
	Sim.Tick(0.2f); // 5 FPS 순간 히치

	const float Total = Graph.GetRoom(A)->CurrentEnergy.Mid + Graph.GetRoom(B)->CurrentEnergy.Mid;
	LOG("주입 1.0 → 한 틱 후 총 에너지 = %.3f (A=%.3f, B=%.3f). 흡음이 있으니 1.0 이하여야 정상", Total,
		Graph.GetRoom(A)->CurrentEnergy.Mid, Graph.GetRoom(B)->CurrentEnergy.Mid);
	CHECK(Total > 1.5f);
}

// 결함 4: 에너지 "양"의 차이로 흐름을 계산 → 평형에서 에너지가 같아짐.
//         물리적으로는 에너지 "밀도"(E/V)가 같아져야 한다. 작은 방이 큰 방보다 100배 시끄러운 채로 멈춤.
TEST_CASE(V0_Finding4_EquilibriumOnEnergyNotDensity)
{
	FAcousticRoomGraph Graph;
	const RoomId Small = Graph.AddRoom(MakeRoomV0(10.f, 1.f, NoAbsorption));
	const RoomId Large = Graph.AddRoom(MakeRoomV0(1000.f, 1.f, NoAbsorption));
	Graph.AddPortal(Small, Large, 1.f, 0.f);
	FAcousticDiffusionSimulator Sim(&Graph);

	Sim.InjectEnergy(Large, { 0.f, 1.f, 0.f });
	for (int I = 0; I < 2000; ++I) Sim.Tick(0.01f);

	const float DensitySmall = Graph.GetRoom(Small)->CurrentEnergy.Mid / 10.f;
	const float DensityLarge = Graph.GetRoom(Large)->CurrentEnergy.Mid / 1000.f;
	LOG("20초 후 밀도비 (작은 방 / 큰 방) = %.1f  (물리적 평형 = 1.0)", DensitySmall / DensityLarge);
	CHECK(DensitySmall / DensityLarge > 50.f);
}
