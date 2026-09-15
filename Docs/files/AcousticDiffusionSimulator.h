// AcousticDiffusionSimulator.h
// ---------------------------------------------------------
// Core 레이어: 방 그래프 위에서 음향 에너지가 시간에 따라 확산/감쇠되는 것을
// 계산한다. 열 확산 방정식과 개념적으로 유사한 근사 모델.
//
// 이 클래스 하나가 사실상 "오가닉 리버브"의 알고리즘 본체다.
// 엔진 의존성이 전혀 없으므로 유닛 테스트도 엔진 없이 그냥 짤 수 있다.
// ---------------------------------------------------------
#pragma once

#include "AcousticRoomGraph.h"
#include <cmath>

namespace Acoustic
{
	class FAcousticDiffusionSimulator
	{
	public:
		explicit FAcousticDiffusionSimulator(FAcousticRoomGraph* InGraph)
			: Graph(InGraph)
		{
		}

		// 발사/폭발 등 이벤트 발생 시, 해당 방에 에너지를 주입
		void InjectEnergy(RoomId Room, const FBandValues& Energy)
		{
			if (FRoom* R = Graph->GetRoom(Room))
			{
				R->CurrentEnergy.Low += Energy.Low;
				R->CurrentEnergy.Mid += Energy.Mid;
				R->CurrentEnergy.High += Energy.High;
			}
		}

		// 매 틱(또는 매 N프레임) 호출. DeltaTime은 초 단위.
		void Tick(float DeltaTime)
		{
			// 1. 각 방의 RT60을 Sabine 공식으로 갱신
			//    RT60 = 0.161 * V / A_absorption
			for (FRoom& Room : Graph->GetAllRooms())
			{
				Room.RT60.Low  = ComputeRT60(Room, Room.DominantMaterial.AbsorptionLow);
				Room.RT60.Mid  = ComputeRT60(Room, Room.DominantMaterial.AbsorptionMid);
				Room.RT60.High = ComputeRT60(Room, Room.DominantMaterial.AbsorptionHigh);
			}

			// 2. 인접 방 사이 에너지 확산 (개구부 크기에 비례해서 유출입)
			std::vector<FBandValues> Delta(Graph->NumRooms());

			for (FRoom& Room : Graph->GetAllRooms())
			{
				for (PortalId PId : Graph->GetPortalsOfRoom(Room.Id))
				{
					const FPortal* Portal = Graph->GetPortal(PId);
					if (!Portal) continue;

					RoomId OtherId = (Portal->RoomA == Room.Id) ? Portal->RoomB : Portal->RoomA;
					FRoom* Other = Graph->GetRoom(OtherId);
					if (!Other) continue;

					// 개구부가 클수록, 거리가 가까울수록 더 빨리 흘러감
					float FlowRate = DiffusionRate * Portal->OpeningArea / (1.f + Portal->Distance);

					FBandValues Flow;
					Flow.Low  = (Room.CurrentEnergy.Low  - Other->CurrentEnergy.Low)  * FlowRate * DeltaTime;
					Flow.Mid  = (Room.CurrentEnergy.Mid  - Other->CurrentEnergy.Mid)  * FlowRate * DeltaTime;
					Flow.High = (Room.CurrentEnergy.High - Other->CurrentEnergy.High) * FlowRate * DeltaTime;

					Delta[Room.Id].Low  -= Flow.Low;
					Delta[Room.Id].Mid  -= Flow.Mid;
					Delta[Room.Id].High -= Flow.High;

					Delta[OtherId].Low  += Flow.Low;
					Delta[OtherId].Mid  += Flow.Mid;
					Delta[OtherId].High += Flow.High;
				}
			}

			// 3. 확산 결과 적용 + 자체 감쇠(RT60 기반 exponential decay)
			for (FRoom& Room : Graph->GetAllRooms())
			{
				Room.CurrentEnergy.Low  += Delta[Room.Id].Low;
				Room.CurrentEnergy.Mid  += Delta[Room.Id].Mid;
				Room.CurrentEnergy.High += Delta[Room.Id].High;

				Room.CurrentEnergy.Low  *= DecayFactor(Room.RT60.Low,  DeltaTime);
				Room.CurrentEnergy.Mid  *= DecayFactor(Room.RT60.Mid,  DeltaTime);
				Room.CurrentEnergy.High *= DecayFactor(Room.RT60.High, DeltaTime);

				// 부동소수점 잔여값이 무한히 남지 않도록 컷오프
				ClampNearZero(Room.CurrentEnergy);
			}
		}

		// 방 하나의 현재 상태를 리버브 파라미터로 변환 (Wet Level 0~1)
		float GetWetLevel(RoomId Room) const
		{
			const FRoom* R = const_cast<FAcousticRoomGraph*>(Graph)->GetRoom(Room);
			if (!R) return 0.f;

			float Total = R->CurrentEnergy.Low + R->CurrentEnergy.Mid + R->CurrentEnergy.High;
			return std::min(1.f, Total / EnergyToWetNormalizer);
		}

	private:
		float ComputeRT60(const FRoom& Room, float AbsorptionCoefficient) const
		{
			float TotalAbsorption = Room.SurfaceArea * AbsorptionCoefficient;
			if (TotalAbsorption <= 0.001f) TotalAbsorption = 0.001f; // 0 나눗셈 방지
			return 0.161f * Room.Volume / TotalAbsorption;
		}

		// RT60 초과 시간 동안 -60dB 감쇠 -> 프레임당 감쇠 배율로 환산
		float DecayFactor(float RT60, float DeltaTime) const
		{
			if (RT60 <= 0.001f) return 0.f;
			// -60dB = 1/1000 배 -> pow(1/1000, DeltaTime/RT60)
			return std::pow(0.001f, DeltaTime / RT60);
		}

		void ClampNearZero(FBandValues& Values) const
		{
			constexpr float Epsilon = 1e-5f;
			if (Values.Low  < Epsilon) Values.Low  = 0.f;
			if (Values.Mid  < Epsilon) Values.Mid  = 0.f;
			if (Values.High < Epsilon) Values.High = 0.f;
		}

		FAcousticRoomGraph* Graph = nullptr;

		// 튜닝 파라미터 (테스트 씬에서 슬라이더로 노출하면 좋음)
		float DiffusionRate = 0.5f;
		float EnergyToWetNormalizer = 100.f;
	};
}
