// AcousticDiffusionSimulator.h
// ---------------------------------------------------------
// AcousticCore: 방 그래프 위에서 음향 에너지가 시간에 따라 확산/감쇠되는 것을 계산한다.
// "오가닉 리버브"의 알고리즘 본체.
//
// 물리 모델 (결합 공간 통계 음향, Coupled-room diffuse field):
//   각 방은 확산 음장(diffuse field)이라 가정하고 에너지 밀도 w = E / V 로 표현한다.
//   확산 음장에서 면적 S인 면을 통과하는 에너지 흐름 = c · w · S / 4
//
//   1) 흡음:  dE/dt = -(c · (A + 4mV) / 4V) · E,   A = S_surface · α,  m = 공기 흡음계수(1/m)
//             → 해석해 E(t) = E0 · exp(-k t),  k = c·A/4V + c·m
//             → k·RT60 = ln(10^6) 을 풀면 RT60 = 0.161 · V / (A + 4mV)  (Sabine 식과 정확히 일치)
//             공기 항은 벽에 닿지 않고도 공기 중에서 잃는 에너지. 고역일수록 크고 큰 방일수록 지배적이다.
//   2) 포탈:  두 방 사이 순 흐름 = G · (w_A - w_B),   G = c · S_portal · τ / 4
//             → 열린 개구부 = 흡음계수 1인 면. 흡음과 전파가 같은 식 하나에서 나온다.
//
// 적분 방식 (무조건 안정):
//   - 흡음: 지수 해석해를 그대로 곱함 (DeltaTime 크기와 무관하게 정확)
//   - 포탈: 포탈마다 두 방의 밀도차가 exp(-λh)로 줄어드는 2-방 해석해를 적용
//           → 에너지 정확히 보존, 음수 에너지/진동 불가능
//   - 포탈을 순서대로 적용하는 분할 오차는 정방향 h/2 + 역방향 h/2 대칭 스윕으로 줄인다.
//   - 전달 지연(6-5): 포탈을 빠져나간 에너지는 "포탈 → 받는 방 중심" 거리를 음속으로 지나는 동안
//     큐에 머물다가 도착한다. 흐름을 계산할 때 가는 중인 에너지를 받는 쪽 밀도에 미리 더해 두므로
//     (이미 보낸 걸 또 보내지 않으므로) 지연이 있어도 평형을 넘어가지 않는다.
// ---------------------------------------------------------
#pragma once

#include "AcousticRoomGraph.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace Acoustic
{
	// 흡음에 의한 에너지 감쇠율 k (1/s):  dE/dt = -k · E
	//   벽 흡음 c·S·α/(4V) + 공기 흡음 c·m  (AirM = 공기 흡음계수 1/m, 0이면 벽만)
	//   공기 항은 Sabine 식 분모의 4mV 항과 같은 것이다: c·(A + 4mV)/(4V) = c·A/(4V) + c·m
	//   → 방 크기와 무관한 상수 감쇠. 큰 방일수록 벽 항이 작아져서 상대적으로 지배적이 된다.
	inline float AbsorptionDecayRate(float Volume, float SurfaceArea, float Alpha, float C = SpeedOfSound, float AirM = 0.f)
	{
		if (!(Volume > 0.f)) return 0.f;
		return C * SurfaceArea * Alpha / (4.f * Volume) + C * std::max(0.f, AirM);
	}

	// Sabine RT60 (s). 벽 흡음도 공기 흡음도 0이면 무한대.
	inline float SabineRT60(float Volume, float SurfaceArea, float Alpha, float C = SpeedOfSound, float AirM = 0.f)
	{
		const float K = AbsorptionDecayRate(Volume, SurfaceArea, Alpha, C, AirM);
		return K > 0.f ? Ln1e6 / K : std::numeric_limits<float>::infinity();
	}

	// 방 하나의 대역별 RT60. AirM을 비우면(기본) 교과서 Sabine 식 그대로,
	// AirAbsorption::Coefficient()를 넘기면 공기 흡음까지 포함한 값.
	inline FBandValues SabineRT60(const FRoom& Room, float C = SpeedOfSound, const FBandValues& AirM = FBandValues())
	{
		FBandValues Result;
		for (int B = 0; B < NumBands; ++B)
		{
			Result[B] = SabineRT60(Room.Volume, Room.SurfaceArea, Room.Material.Absorption[B], C, AirM[B]);
		}
		return Result;
	}

	struct FDiffusionSettings
	{
		float SpeedOfSound = Acoustic::SpeedOfSound;

		// 큰 DeltaTime(프레임 드랍)을 이 크기 이하로 쪼갠다. 안정성과는 무관하고(무조건 안정) 분할 오차만 줄인다.
		float MaxSubStep = 1.f / 60.f;
		int MaxSubStepCount = 16;

		// 이 값 미만의 에너지는 0으로 컷 (부동소수점 잔여값 제거)
		float EnergyEpsilon = 1e-9f;

		// 공기 흡음계수 m (1/m, 대역별). 큰 공간에서 고역 잔향이 빨리 죽는 이유.
		// FBandValues()(전부 0)로 두면 벽 흡음만 쓰는 순수 Sabine 거동이 된다.
		FBandValues AirAbsorptionCoefficient = AirAbsorption::Coefficient();

		// 방 사이 에너지 전달 지연 (6-5). 확산 모델 자체에는 전파 속도가 없어서, 끄면 총성이 난 순간
		// 40 m 밖 방에도 (아주 작게나마) 에너지가 도달한다. 켜면 포탈을 통과한 에너지가
		// "포탈 → 받는 방 중심" 거리를 음속으로 지나는 시간만큼 늦게 도착한다.
		bool bPortalDelay = true;
		float MaxPortalDelay = 0.5f; // s, 안전 상한 (지연 큐 길이를 묶어 둔다)
	};

	class FAcousticDiffusionSimulator
	{
	public:
		explicit FAcousticDiffusionSimulator(FAcousticRoomGraph& InGraph, const FDiffusionSettings& InSettings = FDiffusionSettings())
			: Graph(InGraph)
			, Settings(InSettings)
		{
		}

		// 발사/폭발 등 이벤트 발생 시 해당 방에 에너지 주입
		void InjectEnergy(RoomId Room, const FBandValues& Energy)
		{
			if (FRoom* R = Graph.GetRoom(Room))
			{
				for (int B = 0; B < NumBands; ++B)
				{
					R->Energy[B] += std::max(0.f, Energy[B]);
				}
			}
		}

		// 매 틱(또는 매 N프레임) 호출. DeltaTime은 초 단위.
		void Tick(float DeltaTime)
		{
			if (!(DeltaTime > 0.f)) return;

			int Steps = 1;
			if (Settings.MaxSubStep > 0.f)
			{
				Steps = static_cast<int>(std::ceil(DeltaTime / Settings.MaxSubStep));
				Steps = std::clamp(Steps, 1, std::max(1, Settings.MaxSubStepCount));
			}
			const float H = DeltaTime / static_cast<float>(Steps);
			if (Settings.bPortalDelay) EnsureTransits();

			for (int Step = 0; Step < Steps; ++Step)
			{
				ApplyAbsorption(0.5f * H);
				ExchangeThroughPortals(0.5f * H, false);
				ExchangeThroughPortals(0.5f * H, true);
				// 이번 스텝에 포탈을 빠져나간 에너지가 받는 방으로 흘러 들어오는 만큼 진행
				if (Settings.bPortalDelay) AdvanceTransits(H);
				ApplyAbsorption(0.5f * H);
			}

			for (FRoom& Room : Graph.GetRooms())
			{
				for (int B = 0; B < NumBands; ++B)
				{
					if (Room.Energy[B] < Settings.EnergyEpsilon) Room.Energy[B] = 0.f;
				}
			}
		}

		// 모든 방의 에너지를 0으로 (가는 중인 에너지도 버린다)
		void Reset()
		{
			for (FRoom& Room : Graph.GetRooms()) Room.Energy = FBandValues();
			for (FPortalTransit& Transit : Transits)
			{
				Transit.ToA.Clear();
				Transit.ToB.Clear();
			}
		}

		FBandValues GetEnergyDensity(RoomId Room) const
		{
			const FRoom* R = Graph.GetRoom(Room);
			if (!R) return FBandValues();
			const float InvV = 1.f / R->Volume;
			return FBandValues(R->Energy.Low * InvV, R->Energy.Mid * InvV, R->Energy.High * InvV);
		}

		// 방에 있는 에너지 + 포탈을 지나 아직 도착하지 않은 에너지. 보존을 볼 때는 둘 다 세야 한다
		FBandValues GetTotalEnergy() const
		{
			FBandValues Total;
			for (const FRoom& Room : Graph.GetRooms())
			{
				for (int B = 0; B < NumBands; ++B) Total[B] += Room.Energy[B];
			}
			const FBandValues Transit = GetInFlightEnergy();
			for (int B = 0; B < NumBands; ++B) Total[B] += Transit[B];
			return Total;
		}

		// 포탈을 지나 받는 방으로 가는 중인 에너지
		FBandValues GetInFlightEnergy() const
		{
			FBandValues Total;
			for (const FPortalTransit& Transit : Transits)
			{
				for (int B = 0; B < NumBands; ++B) Total[B] += Transit.ToA.InFlight[B] + Transit.ToB.InFlight[B];
			}
			return Total;
		}

		const FDiffusionSettings& GetSettings() const { return Settings; }
		void SetSettings(const FDiffusionSettings& InSettings) { Settings = InSettings; }

	private:
		void ApplyAbsorption(float H)
		{
			for (FRoom& Room : Graph.GetRooms())
			{
				for (int B = 0; B < NumBands; ++B)
				{
					const float K = AbsorptionDecayRate(Room.Volume, Room.SurfaceArea, Room.Material.Absorption[B],
						Settings.SpeedOfSound, Settings.AirAbsorptionCoefficient[B]);
					Room.Energy[B] *= std::exp(-K * H);
				}
			}
		}

		void ExchangeThroughPortals(float H, bool bReverse)
		{
			const std::vector<FPortal>& Portals = Graph.GetPortals();
			const size_t Count = Portals.size();
			for (size_t I = 0; I < Count; ++I)
			{
				ExchangePair(Portals[bReverse ? Count - 1 - I : I], H);
			}
		}

		// 두 방만 따로 떼어 보면 밀도차 (w_A - w_B)가 exp(-λh)로 줄어든다. λ = G · (1/V_A + 1/V_B)
		// 이 해석해대로 에너지를 옮기면 한 스텝에 평형을 넘어설 수 없다(overshoot 없음).
		void ExchangePair(const FPortal& Portal, float H)
		{
			const float G = 0.25f * Settings.SpeedOfSound * Portal.OpeningArea * std::clamp(Portal.Transmission, 0.f, 1.f);
			if (!(G > 0.f)) return;

			FRoom* A = Graph.GetRoom(Portal.RoomA);
			FRoom* B = Graph.GetRoom(Portal.RoomB);
			if (!A || !B) return;

			const float InvVA = 1.f / A->Volume;
			const float InvVB = 1.f / B->Volume;
			const float InvVSum = InvVA + InvVB;
			const float Relax = -std::expm1(-G * InvVSum * H); // 이번 스텝에 밀도차가 해소되는 비율 (0~1)

			FPortalTransit* Transit = Settings.bPortalDelay && Portal.Id < Transits.size() ? &Transits[Portal.Id] : nullptr;

			for (int Band = 0; Band < NumBands; ++Band)
			{
				if (!Transit)
				{
					const float DensityDiff = A->Energy[Band] * InvVA - B->Energy[Band] * InvVB;
					const float Transfer = DensityDiff * Relax / InvVSum;
					A->Energy[Band] -= Transfer;
					B->Energy[Band] += Transfer;
					continue;
				}

				// 이미 가는 중인 에너지도 받는 쪽 밀도에 포함시킨다. 그러지 않으면 도착하기 전까지
				// "아직 안 왔네" 하고 같은 에너지를 계속 다시 보내서 평형을 넘어가고 진동한다.
				const float DensityDiff =
					(A->Energy[Band] + Transit->ToA.InFlight[Band]) * InvVA -
					(B->Energy[Band] + Transit->ToB.InFlight[Band]) * InvVB;
				float Transfer = DensityDiff * Relax / InvVSum;

				// 실제로 방 안에 있는 에너지만 보낼 수 있다 (음수 에너지 방지)
				Transfer = std::clamp(Transfer, -B->Energy[Band], A->Energy[Band]);
				if (Transfer > 0.f)
				{
					A->Energy[Band] -= Transfer;
					Transit->ToB.Push(Band, Transfer);
				}
				else if (Transfer < 0.f)
				{
					B->Energy[Band] += Transfer;
					Transit->ToA.Push(Band, -Transfer);
				}
			}
		}

		// 포탈을 통과한 뒤 아직 받는 방에 닿지 않은 에너지 (한 방향)
		struct FTransitLine
		{
			struct FPacket
			{
				float Remaining = 0.f;
				FBandValues Energy;
			};

			std::vector<FPacket> Packets;
			size_t Head = 0;      // Packets[Head] 앞쪽은 이미 전달됨
			FBandValues InFlight; // 아직 전달되지 않은 에너지 합
			float Delay = 0.f;    // s, 포탈 → 이 방 중심

			void Push(int Band, float Energy)
			{
				InFlight[Band] += Energy;
				// 같은 스텝에 들어온 것끼리는 한 패킷으로 묶는다 (대역마다 패킷이 생기지 않게)
				if (Head < Packets.size() && Packets.back().Remaining == Delay)
				{
					Packets.back().Energy[Band] += Energy;
					return;
				}
				FPacket Packet;
				Packet.Remaining = Delay;
				Packet.Energy[Band] = Energy;
				Packets.push_back(Packet);
			}

			// H초 진행. 도착한 에너지를 Room에 넣는다
			void Advance(float H, FRoom& Room)
			{
				if (Head >= Packets.size()) return;
				for (size_t I = Head; I < Packets.size(); ++I)
				{
					FPacket& Packet = Packets[I];
					Packet.Remaining -= H;
					if (Packet.Remaining > 0.f) continue;

					for (int B = 0; B < NumBands; ++B)
					{
						Room.Energy[B] += Packet.Energy[B];
						InFlight[B] = std::max(0.f, InFlight[B] - Packet.Energy[B]);
					}
					Packet.Energy = FBandValues();
					if (I == Head) ++Head; // 앞에서부터 도착하므로 보통 여기서 정리된다
				}
				if (Head == Packets.size())
				{
					Packets.clear();
					Head = 0;
				}
			}

			void Clear()
			{
				Packets.clear();
				Head = 0;
				InFlight = FBandValues();
			}
		};

		struct FPortalTransit
		{
			FTransitLine ToA;
			FTransitLine ToB;
		};

		// 포탈 수가 바뀌면(그래프 재생성) 지연 큐를 다시 만든다. 지연은 기하학에서 한 번만 구한다.
		void EnsureTransits()
		{
			const std::vector<FPortal>& Portals = Graph.GetPortals();
			if (Transits.size() == Portals.size()) return;

			Transits.assign(Portals.size(), FPortalTransit());
			for (const FPortal& Portal : Portals)
			{
				const FRoom* A = Graph.GetRoom(Portal.RoomA);
				const FRoom* B = Graph.GetRoom(Portal.RoomB);
				if (!A || !B || Portal.Id >= Transits.size()) continue;

				// 한 방의 가운데에서 문을 거쳐 옆방 가운데까지 소리가 지나는 시간.
				// 직선 거리가 아니라 문을 경유한 거리라, ㄱ자로 꺾인 통로에서도 벽을 뚫지 않는다.
				const float PathLength = (A->Center - Portal.Center).Length() + (B->Center - Portal.Center).Length();
				const float Delay = std::clamp(PathLength / Settings.SpeedOfSound, 0.f, Settings.MaxPortalDelay);
				Transits[Portal.Id].ToA.Delay = Delay;
				Transits[Portal.Id].ToB.Delay = Delay;
			}
		}

		void AdvanceTransits(float H)
		{
			for (const FPortal& Portal : Graph.GetPortals())
			{
				if (Portal.Id >= Transits.size()) continue;
				FPortalTransit& Transit = Transits[Portal.Id];
				// 대부분의 포탈은 아무것도 지나가지 않는다 (소리는 몇 개 방에만 있다)
				if (Transit.ToA.Packets.empty() && Transit.ToB.Packets.empty()) continue;
				if (FRoom* A = Graph.GetRoom(Portal.RoomA)) Transit.ToA.Advance(H, *A);
				if (FRoom* B = Graph.GetRoom(Portal.RoomB)) Transit.ToB.Advance(H, *B);
			}
		}

		FAcousticRoomGraph& Graph;
		FDiffusionSettings Settings;
		std::vector<FPortalTransit> Transits;
	};
}
