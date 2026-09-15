// ReverbParameterMapper.h
// ---------------------------------------------------------
// AcousticCore: 시뮬레이션 결과(방 정보 + 리스너 방의 에너지 밀도)를
// 리버브 DSP 파라미터로 변환한다. 엔진 독립적인 "중간 표현"만 만들고,
// 실제 엔진 API(UE FReverbSettings, MetaSounds 입력 등)로 옮기는 건 어댑터가 한다.
// ---------------------------------------------------------
#pragma once

#include "AcousticDiffusionSimulator.h"

namespace Acoustic
{
	struct FReverbParams
	{
		float DecayTime = 1.f;         // s, Mid 대역 Sabine RT60           → UE FReverbSettings::DecayTime
		float DecayHFRatio = 1.f;      // RT60(High) / RT60(Mid)             → UE FReverbSettings::DecayHFRatio
		float ReflectionsDelay = 0.f;  // s, 평균 자유 행로(4V/S) / c       → UE FReverbSettings::ReflectionsDelay
		float WetLevel = 0.f;          // 0~1, 리스너 방 에너지 밀도의 dB 스케일 매핑
	};

	struct FReverbMappingSettings
	{
		// 출력 범위 (기본값 = UE FReverbSettings 허용 범위)
		float MinDecayTime = 0.1f;
		float MaxDecayTime = 20.f;
		float MinDecayHFRatio = 0.1f;
		float MaxDecayHFRatio = 2.f;
		float MaxReflectionsDelay = 0.3f;

		// 에너지 밀도(3대역 합)가 ReferenceDensity일 때 WetLevel = 1 (0 dB),
		// 거기서 DynamicRangeDb 만큼 내려가면 WetLevel = 0. 사이는 dB 기준 선형.
		float ReferenceDensity = 1.f;
		float DynamicRangeDb = 60.f;

		// 공기 흡음계수 m (1/m, 대역별). 시뮬레이터(FDiffusionSettings)와 같은 값을 써야
		// 화면에 표시되는 Decay Time과 실제로 들리는 감쇠가 일치한다.
		FBandValues AirAbsorptionCoefficient = AirAbsorption::Coefficient();
	};

	// 리버브 길이를 계산할 "공간"의 음향 특성. 방 하나일 수도, 같은 그룹의 방 여러 개를 합친 것일 수도 있다.
	struct FSpaceAcoustics
	{
		float Volume = 0.f;          // m^3
		float SurfaceArea = 0.f;     // m^2 (방 사이 분할면은 포탈이므로 포함되지 않음)
		FBandValues AbsorptionArea;  // Σ S·α (대역별, m^2)
	};

	inline FSpaceAcoustics ToSpaceAcoustics(const FRoom& Room)
	{
		FSpaceAcoustics Out;
		Out.Volume = Room.Volume;
		Out.SurfaceArea = Room.SurfaceArea;
		for (int B = 0; B < NumBands; ++B) Out.AbsorptionArea[B] = Room.SurfaceArea * Room.Material.Absorption[B];
		return Out;
	}

	// 같은 GroupId를 가진 방들(분할된 복도/홀 조각)을 하나의 공간으로 합친다.
	// 조각 하나만 보면 바깥 벽이 거의 없어 RT60이 비정상적으로 길게 나오기 때문.
	inline FSpaceAcoustics ComputeGroupAcoustics(const FAcousticRoomGraph& Graph, uint32_t GroupId)
	{
		FSpaceAcoustics Out;
		for (const FRoom& Room : Graph.GetRooms())
		{
			if (Room.GroupId != GroupId) continue;
			const FSpaceAcoustics Part = ToSpaceAcoustics(Room);
			Out.Volume += Part.Volume;
			Out.SurfaceArea += Part.SurfaceArea;
			for (int B = 0; B < NumBands; ++B) Out.AbsorptionArea[B] += Part.AbsorptionArea[B];
		}
		return Out;
	}

	inline FReverbParams MapToReverbParams(const FSpaceAcoustics& Space, const FBandValues& EnergyDensity,
		const FReverbMappingSettings& S = FReverbMappingSettings(), float C = SpeedOfSound)
	{
		FReverbParams Out;

		// Sabine + 공기 흡음: RT60 = ln(10^6) · 4V / (c · (A + 4mV)). 흡음 0이면 무한대 → 클램프한 값끼리 비율을 구해 NaN 방지
		// 4mV 항은 부피에 비례하므로 큰 공간일수록 세지고, m이 큰 High 대역을 먼저 깎는다 → DecayHFRatio가 자연히 내려간다
		auto RT60Of = [&](float AbsorptionArea, float AirM)
		{
			const float Total = AbsorptionArea + 4.f * std::max(0.f, AirM) * Space.Volume;
			return Total > 0.f ? Ln1e6 * 4.f * Space.Volume / (C * Total) : std::numeric_limits<float>::infinity();
		};
		const float RTMid = std::clamp(RT60Of(Space.AbsorptionArea.Mid, S.AirAbsorptionCoefficient.Mid), S.MinDecayTime, S.MaxDecayTime);
		const float RTHigh = std::clamp(RT60Of(Space.AbsorptionArea.High, S.AirAbsorptionCoefficient.High), S.MinDecayTime, S.MaxDecayTime);
		Out.DecayTime = RTMid;
		Out.DecayHFRatio = std::clamp(RTHigh / RTMid, S.MinDecayHFRatio, S.MaxDecayHFRatio);

		if (Space.SurfaceArea > 0.f)
		{
			const float MeanFreePath = 4.f * Space.Volume / Space.SurfaceArea;
			Out.ReflectionsDelay = std::clamp(MeanFreePath / C, 0.f, S.MaxReflectionsDelay);
		}

		const float Density = EnergyDensity.Sum();
		if (Density > 0.f && S.ReferenceDensity > 0.f && S.DynamicRangeDb > 0.f)
		{
			const float LevelDb = 10.f * std::log10(Density / S.ReferenceDensity);
			Out.WetLevel = std::clamp(1.f + LevelDb / S.DynamicRangeDb, 0.f, 1.f);
		}

		return Out;
	}

	// 방 하나를 독립된 공간으로 볼 때
	inline FReverbParams MapToReverbParams(const FRoom& ListenerRoom, const FBandValues& EnergyDensity,
		const FReverbMappingSettings& S = FReverbMappingSettings(), float C = SpeedOfSound)
	{
		return MapToReverbParams(ToSpaceAcoustics(ListenerRoom), EnergyDensity, S, C);
	}

	// 그래프 안의 방: 잔향 길이/반사 지연은 같은 그룹 전체로, Wet은 이 방의 에너지 밀도로 계산
	inline FReverbParams MapToReverbParams(const FAcousticRoomGraph& Graph, RoomId ListenerRoom, const FBandValues& EnergyDensity,
		const FReverbMappingSettings& S = FReverbMappingSettings(), float C = SpeedOfSound)
	{
		const FRoom* Room = Graph.GetRoom(ListenerRoom);
		if (!Room) return FReverbParams();
		return MapToReverbParams(ComputeGroupAcoustics(Graph, Room->GroupId), EnergyDensity, S, C);
	}
}
