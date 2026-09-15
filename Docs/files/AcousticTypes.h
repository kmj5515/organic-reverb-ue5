// AcousticTypes.h
// ---------------------------------------------------------
// Core 레이어: 엔진(Unreal/Unity 등)에 전혀 의존하지 않는 순수 타입 정의.
// UObject, FVector, TArray 같은 엔진 타입은 여기서 절대 쓰지 않는다.
// 나중에 다른 엔진으로 포팅할 때 이 파일은 수정 없이 그대로 재사용 가능해야 한다.
// ---------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace Acoustic
{
	// 엔진 벡터 타입에 의존하지 않기 위한 최소 3D 벡터
	struct FVec3
	{
		float X = 0.f;
		float Y = 0.f;
		float Z = 0.f;

		FVec3() = default;
		FVec3(float InX, float InY, float InZ) : X(InX), Y(InY), Z(InZ) {}

		FVec3 operator-(const FVec3& Other) const
		{
			return FVec3(X - Other.X, Y - Other.Y, Z - Other.Z);
		}

		float LengthSquared() const
		{
			return X * X + Y * Y + Z * Z;
		}
	};

	// 재질별 음향 특성 (0~1 사이 흡음계수, 주파수 대역별로 분리)
	// 값이 클수록 해당 대역을 더 많이 흡수(=반사가 적음)한다.
	struct FAcousticMaterial
	{
		std::string Name;
		float AbsorptionLow = 0.1f;   // ~250Hz 대역
		float AbsorptionMid = 0.1f;   // ~1kHz 대역
		float AbsorptionHigh = 0.1f;  // ~4kHz 이상 대역
	};

	// 자주 쓰는 재질 프리셋 (Sabine 계수 참고값, 실측 대체 가능)
	namespace MaterialPresets
	{
		inline FAcousticMaterial Concrete()  { return { "Concrete",  0.02f, 0.03f, 0.04f }; }
		inline FAcousticMaterial Wood()      { return { "Wood",      0.10f, 0.08f, 0.06f }; }
		inline FAcousticMaterial Carpet()    { return { "Carpet",    0.05f, 0.20f, 0.35f }; }
		inline FAcousticMaterial Glass()     { return { "Glass",     0.03f, 0.03f, 0.02f }; }
		inline FAcousticMaterial OpenAir()   { return { "OpenAir",   0.99f, 0.99f, 0.99f }; } // 사실상 반사 없음
	}

	using RoomId = uint32_t;
	using PortalId = uint32_t;
	constexpr RoomId InvalidRoomId = 0xFFFFFFFF;

	// 3개 주파수 대역의 에너지/RT60 값을 함께 들고 다니기 위한 구조체
	// (디버그 시각화에서 R=Low, G=Mid, B=High 로 매핑하기 좋음)
	struct FBandValues
	{
		float Low = 0.f;
		float Mid = 0.f;
		float High = 0.f;
	};
}
