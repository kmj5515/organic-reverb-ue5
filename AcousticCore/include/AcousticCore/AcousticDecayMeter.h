// AcousticDecayMeter.h
// ---------------------------------------------------------
// AcousticCore: 이벤트(총성) 후 한 방의 에너지 감쇠 곡선을 기록하고 잔향 지표를 측정한다 (5-4 디버그 도구).
//   EDT  : 피크 → −10 dB 구간 × 6   — 사람이 느끼는 잔향 길이
//   T20  : −5 → −25 dB 구간 × 3    — 실측에서 쓰는 표준 값
//   Late : −40 → −60 dB 구간 × 3   — 후기 꼬리. 결합 공간이면 EDT보다 훨씬 길다 (이중 기울기)
// 시뮬레이션 곡선은 잡음이 없으므로 구간 교차 시각(샘플 사이 선형 보간)으로 충분하다.
// ---------------------------------------------------------
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace Acoustic
{
	struct FDecayMeasurement
	{
		bool bComplete = false;  // −60 dB까지 내려갔거나 최대 시간에 도달
		float PeakTime = 0.f;    // s, 측정 시작 기준 (옆방 이벤트면 0보다 큼)
		float Duration = 0.f;    // s
		float EDT = -1.f;        // s, 음수 = 그 구간까지 내려가지 않음
		float T20 = -1.f;
		float LateRT = -1.f;

		bool IsDoubleSlope(float Ratio = 1.5f) const { return EDT > 0.f && LateRT > Ratio * EDT; }
	};

	class FDecayMeter
	{
	public:
		void Start(float InMaxDuration = 30.f)
		{
			Times.clear();
			Densities.clear();
			Peak = 0.f;
			PeakIndex = 0;
			Elapsed = 0.f;
			MaxDuration = InMaxDuration;
			bRunning = true;
		}

		void Stop() { bRunning = false; }
		bool IsRunning() const { return bRunning; }
		bool HasData() const { return !Densities.empty(); }

		// 매 틱 측정 대상 방의 에너지 밀도를 넣는다. −60 dB 또는 최대 시간에 도달하면 자동으로 멈춘다.
		void AddSample(float DeltaTime, float EnergyDensity)
		{
			if (!bRunning) return;

			Elapsed += std::max(0.f, DeltaTime);
			Times.push_back(Elapsed);
			Densities.push_back(std::max(0.f, EnergyDensity));
			if (Densities.back() > Peak)
			{
				Peak = Densities.back();
				PeakIndex = Densities.size() - 1;
			}

			const bool bAfterPeak = Densities.size() - 1 > PeakIndex;
			const bool bDecayed = Peak > 0.f && bAfterPeak && Densities.back() <= Peak * 1e-6f;
			if (bDecayed || Elapsed >= MaxDuration) bRunning = false;
		}

		size_t NumSamples() const { return Densities.size(); }
		float TimeAt(size_t I) const { return Times[I]; }

		// 피크 대비 dB. 에너지 0은 −120 dB
		float DbAt(size_t I) const
		{
			return (Peak > 0.f && Densities[I] > 0.f) ? 10.f * std::log10(Densities[I] / Peak) : -120.f;
		}

		FDecayMeasurement GetResult() const
		{
			FDecayMeasurement Result;
			Result.bComplete = !bRunning && HasData();
			Result.Duration = Elapsed;
			if (!HasData() || !(Peak > 0.f)) return Result;

			Result.PeakTime = Times[PeakIndex];
			const float T5 = Crossing(-5.f), T10 = Crossing(-10.f), T25 = Crossing(-25.f), T40 = Crossing(-40.f), T60 = Crossing(-60.f);
			if (T10 >= 0.f) Result.EDT = 6.f * (T10 - Result.PeakTime);
			if (T5 >= 0.f && T25 >= 0.f) Result.T20 = 3.f * (T25 - T5);
			if (T40 >= 0.f && T60 >= 0.f) Result.LateRT = 3.f * (T60 - T40);
			return Result;
		}

	private:
		// 피크 이후 처음으로 Threshold dB 이하가 되는 시각 (샘플 사이 선형 보간). 없으면 −1
		float Crossing(float ThresholdDb) const
		{
			for (size_t I = PeakIndex + 1; I < Densities.size(); ++I)
			{
				const float Db = DbAt(I);
				if (Db > ThresholdDb) continue;
				const float PrevDb = DbAt(I - 1);
				const float T = (PrevDb - Db) > 1e-6f ? (PrevDb - ThresholdDb) / (PrevDb - Db) : 1.f;
				return Times[I - 1] + T * (Times[I] - Times[I - 1]);
			}
			return -1.f;
		}

		std::vector<float> Times;
		std::vector<float> Densities;
		float Peak = 0.f;
		size_t PeakIndex = 0;
		float Elapsed = 0.f;
		float MaxDuration = 30.f;
		bool bRunning = false;
	};
}
