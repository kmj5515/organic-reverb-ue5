// AcousticShotRender.h
// ---------------------------------------------------------
// AcousticCore: 원거리 총성(한 번 울리는 큰 소리)의 거리감 계산 (5단계).
// 이미 계산한 전파 경로(FPropagationPath)와 잔향 결합을 받아, 리스너에게 "어떻게 들려야 하는지"를 만든다.
//
//   도착 지연   : 소리가 실제로 지나온 거리 / 음속 (문을 돌아오면 경로 길이, 벽을 뚫고 오면 직선 거리)
//   들리는 위치 : 문을 돌아오면 리스너 쪽 마지막 문, 아니면 음원 위치
//   직접음 크기 : 거리 감쇠 (InnerRadius 밖에서 거리 2배당 −6 dB) + 공기 흡수 (대역별, 고음일수록 큼) + 경로 손실(회절/차폐)
//   로우패스    : 고음이 중음보다 더 깎인 만큼
//   레이어      : 근거리 / 원거리 녹음을 거리로 섞는 비율 (동일 파워 크로스페이드)
//   리버브 Send : 음원 방 → 리스너 방 잔향 결합. 거리와 무관 (확산 음장 잔향은 방 안 어디서나 같음)
//                → 멀어질수록 직접음만 줄어 "탕"에서 "쿠웅"으로 바뀐다 (직접음/잔향 비율)
// ---------------------------------------------------------
#pragma once

#include "AcousticPropagation.h"

namespace Acoustic
{
	struct FShotRenderSettings
	{
		float InnerRadius = 5.f; // m, 이 안에서는 거리 감쇠 없음

		// 공기 흡수 (dB/m). 실내 수십 m에서는 작고, 야외 수백 m에서 커진다.
		// 잔향 쪽 공기 흡음(Sabine 4mV 항)과 같은 상수를 쓴다 (AcousticTypes.h).
		FBandValues AirAbsorptionDbPerMeter = AirAbsorption::DbPerMeter();

		float NearLayerDistance = 10.f; // m, 이하 = 근거리 레이어만
		float FarLayerDistance = 50.f;  // m, 이상 = 원거리 레이어만
		float MinGainDb = -80.f;
		float SpeedOfSound = Acoustic::SpeedOfSound;
		FSourceOutputSettings Output;   // 로우패스 매핑
	};

	struct FShotRender
	{
		bool bValid = false;          // 음원/리스너가 방 안에 있어 경로를 계산했는가 (아니면 직선 거리로만 계산)
		float Distance = 0.f;         // m, 소리가 지나온 거리
		float DelaySeconds = 0.f;
		FVec3 ApparentPosition;
		FBandValues GainDb;           // 직접음 총 감쇠 (0 dB = InnerRadius 안, 막힘 없음)
		float VolumeMultiplier = 1.f; // Mid 기준 진폭 배율
		float LowpassHz = 20000.f;
		float NearLayerGain = 1.f;    // 진폭 (NearLayerGain² + FarLayerGain² = 1)
		float FarLayerGain = 0.f;
		float ReverbSendMultiplier = 1.f;
	};

	inline FShotRender RenderShot(const FPropagationPath& Path, float ReverbCouplingDb, const FShotRenderSettings& S = FShotRenderSettings())
	{
		FShotRender R;
		R.bValid = Path.bValid;
		R.ApparentPosition = Path.ApparentPosition;

		// 벽을 뚫고 오거나 직선으로 보이면 직선 거리, 문을 돌아오면 경로 길이
		const bool bAroundPortals = Path.bValid && !Path.bSameRoom && !Path.bLineOfSight && !Path.bOccluded;
		R.Distance = bAroundPortals ? Path.PathLength : Path.DirectDistance;
		R.DelaySeconds = R.Distance / S.SpeedOfSound;

		// 거리 감쇠는 직선 거리로 계산한다. Path.GainDb에 이미 "돌아가는 만큼 멀어진 손실"(20·log(d/d_path))이
		// 들어 있어서, 둘을 더하면 경로 길이 기준 감쇠가 된다.
		const float Radius = std::max(0.01f, S.InnerRadius);
		const float DistanceDb = 20.f * std::log10(Radius / std::max(Radius, Path.DirectDistance));
		for (int B = 0; B < NumBands; ++B)
		{
			const float Db = DistanceDb - S.AirAbsorptionDbPerMeter[B] * R.Distance + Path.GainDb[B];
			R.GainDb[B] = std::max(S.MinGainDb, Db);
		}

		R.VolumeMultiplier = std::pow(10.f, R.GainDb.Mid / 20.f);
		const float HighExtraLossDb = std::max(0.f, R.GainDb.Mid - R.GainDb.High);
		const float T = std::clamp(HighExtraLossDb / S.Output.FullMuffleDb, 0.f, 1.f);
		R.LowpassHz = S.Output.MaxLowpassHz * std::pow(S.Output.MinLowpassHz / S.Output.MaxLowpassHz, T);

		// 동일 파워 크로스페이드: 전체 에너지는 거리와 무관하게 유지 (크기는 GainDb가 담당)
		const float Span = std::max(0.01f, S.FarLayerDistance - S.NearLayerDistance);
		const float Mix = std::clamp((R.Distance - S.NearLayerDistance) / Span, 0.f, 1.f);
		R.NearLayerGain = std::cos(Mix * 1.5707963f);
		R.FarLayerGain = std::sin(Mix * 1.5707963f);

		R.ReverbSendMultiplier = std::pow(10.f, ReverbCouplingDb / 20.f);
		return R;
	}
}
