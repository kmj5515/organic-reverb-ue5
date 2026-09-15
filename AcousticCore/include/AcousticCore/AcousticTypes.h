// AcousticTypes.h
// ---------------------------------------------------------
// AcousticCore (엔진 독립 레이어) — 기본 타입과 물리 상수.
//
// 규칙: AcousticCore의 모든 파일은 C++17 표준 라이브러리 외에는 아무것도 include 하지 않는다.
//       (UObject, FVector, TArray 금지) → Unreal/Unity 어디서든 수정 없이 재사용.
// 단위: 길이 m, 면적 m^2, 부피 m^3, 시간 s. 에너지는 임의 단위(상대값).
// ---------------------------------------------------------
#pragma once

#include <cmath>
#include <cstdint>
#include <string>

namespace Acoustic
{
	// 음속 (m/s, 20°C 공기)
	constexpr float SpeedOfSound = 343.f;

	// -60dB = 에너지(세기) 기준 10^-6 배 → ln(10^6).
	// (음압 기준으로는 1/1000 배지만, 시뮬레이터가 다루는 건 에너지이므로 10^-6 이 맞다)
	constexpr float Ln1e6 = 13.815511f;

	constexpr int NumBands = 3;

	// 엔진 벡터 타입에 의존하지 않기 위한 최소 3D 벡터 (디버그 표시/위치 조회용)
	struct FVec3
	{
		float X = 0.f;
		float Y = 0.f;
		float Z = 0.f;

		FVec3() = default;
		FVec3(float InX, float InY, float InZ) : X(InX), Y(InY), Z(InZ) {}

		FVec3 operator-(const FVec3& Other) const { return FVec3(X - Other.X, Y - Other.Y, Z - Other.Z); }
		FVec3 operator+(const FVec3& Other) const { return FVec3(X + Other.X, Y + Other.Y, Z + Other.Z); }
		FVec3 operator*(float Scale) const { return FVec3(X * Scale, Y * Scale, Z * Scale); }
		float Dot(const FVec3& Other) const { return X * Other.X + Y * Other.Y + Z * Other.Z; }
		float LengthSquared() const { return X * X + Y * Y + Z * Z; }
		float Length() const { return std::sqrt(LengthSquared()); }
	};

	// 3개 주파수 대역 값 묶음 (디버그 시각화에서 R=Low, G=Mid, B=High 로 매핑)
	struct FBandValues
	{
		float Low = 0.f;   // ~250Hz
		float Mid = 0.f;   // ~1kHz
		float High = 0.f;  // ~4kHz 이상

		FBandValues() = default;
		FBandValues(float InLow, float InMid, float InHigh) : Low(InLow), Mid(InMid), High(InHigh) {}

		static FBandValues Uniform(float Value) { return FBandValues(Value, Value, Value); }

		float& operator[](int Band) { return Band == 0 ? Low : (Band == 1 ? Mid : High); }
		float operator[](int Band) const { return Band == 0 ? Low : (Band == 1 ? Mid : High); }

		float Sum() const { return Low + Mid + High; }
	};

	// 재질별 흡음계수 (0~1, 대역별). 값이 클수록 해당 대역을 더 많이 흡수한다.
	struct FAcousticMaterial
	{
		std::string Name;
		FBandValues Absorption = FBandValues::Uniform(0.1f);
	};

	// 자주 쓰는 재질 프리셋 (문헌 참고값, 실측 데이터로 대체 가능)
	namespace MaterialPresets
	{
		inline FAcousticMaterial Concrete() { return { "Concrete", { 0.02f, 0.03f, 0.04f } }; }
		inline FAcousticMaterial Wood()     { return { "Wood",     { 0.10f, 0.08f, 0.06f } }; }
		inline FAcousticMaterial Carpet()   { return { "Carpet",   { 0.05f, 0.20f, 0.35f } }; }
		inline FAcousticMaterial Glass()    { return { "Glass",    { 0.03f, 0.03f, 0.02f } }; }
		inline FAcousticMaterial OpenAir()  { return { "OpenAir",  { 0.99f, 0.99f, 0.99f } }; } // 사실상 반사 없음
	}

	// 공기 흡음 (atmospheric absorption)
	// 소리는 벽에 닿지 않아도 공기 자체의 점성·열전도·분자 이완으로 에너지를 잃는다.
	// 고음일수록 훨씬 크고, 큰 공간처럼 소리가 오래 돌아다닐수록 많이 쌓인다.
	// (실내 수십 m 직접음에서는 작지만, 잔향은 수백 m를 도는 소리라 무시할 수 없다)
	namespace AirAbsorption
	{
		// 거리당 감쇠 (dB/m). ISO 9613-1 기준 20 °C · 습도 50 % 대략값 (250 Hz / 1 kHz / 4 kHz).
		inline FBandValues DbPerMeter() { return FBandValues(0.001f, 0.005f, 0.025f); }

		// dB/m → 에너지 감쇠계수 m (1/m). E(x) = E0 · exp(-m·x), dB = -10·log10(e)·m·x 이므로 m = (dB/m) · ln(10)/10
		constexpr float DbPerMeterToCoefficient = 0.23025851f;

		// Sabine 식 분모의 4mV 항에 쓰는 m (1/m), 대역별.
		// 여기 하나만 바꾸면 잔향·전파·총성 계산이 모두 같은 값을 쓴다.
		inline FBandValues Coefficient()
		{
			FBandValues Out = DbPerMeter();
			for (int B = 0; B < NumBands; ++B) Out[B] *= DbPerMeterToCoefficient;
			return Out;
		}
	}

	using RoomId = uint32_t;
	using PortalId = uint32_t;
	constexpr RoomId InvalidRoomId = 0xFFFFFFFF;
	constexpr PortalId InvalidPortalId = 0xFFFFFFFF;
}
