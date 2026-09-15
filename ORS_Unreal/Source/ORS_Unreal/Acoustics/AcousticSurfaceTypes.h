// AcousticSurfaceTypes.h
// ---------------------------------------------------------
// Unreal 어댑터: 표면 흡음 데이터 타입 + Unreal(cm) ↔ Core(m) 단위 변환.
// ---------------------------------------------------------
#pragma once

#include "CoreMinimal.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "PhysicalMaterials/PhysicalMaterial.h"
#include "AcousticCore/AcousticTypes.h"
#include "AcousticSurfaceTypes.generated.h"

DECLARE_LOG_CATEGORY_EXTERN(LogOrganicReverb, Log, All);

UENUM(BlueprintType)
enum class EAcousticMaterialPreset : uint8
{
	Concrete,
	Wood,
	Carpet,
	Glass
};

/** 대역별 흡음계수 (0~1). 클수록 해당 대역을 더 많이 흡수한다. */
USTRUCT(BlueprintType)
struct FAcousticAbsorption
{
	GENERATED_BODY()

	/** ~250Hz */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Acoustics", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Low = 0.02f;

	/** ~1kHz */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Acoustics", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float Mid = 0.03f;

	/** ~4kHz 이상 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Acoustics", meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float High = 0.04f;

	static FAcousticAbsorption FromCore(const Acoustic::FAcousticMaterial& Material)
	{
		FAcousticAbsorption Result;
		Result.Low = Material.Absorption.Low;
		Result.Mid = Material.Absorption.Mid;
		Result.High = Material.Absorption.High;
		return Result;
	}

	static FAcousticAbsorption FromPreset(EAcousticMaterialPreset Preset)
	{
		switch (Preset)
		{
		case EAcousticMaterialPreset::Wood:   return FromCore(Acoustic::MaterialPresets::Wood());
		case EAcousticMaterialPreset::Carpet: return FromCore(Acoustic::MaterialPresets::Carpet());
		case EAcousticMaterialPreset::Glass:  return FromCore(Acoustic::MaterialPresets::Glass());
		default:                              return FromCore(Acoustic::MaterialPresets::Concrete());
		}
	}

	Acoustic::FAcousticMaterial ToCore(const FString& Name) const
	{
		return { std::string(TCHAR_TO_UTF8(*Name)), { Low, Mid, High } };
	}
};

/**
 * 레벨 지오메트리용: Physical Material 에셋에 흡음계수를 붙인다.
 * 메시 머티리얼의 Phys Material 또는 컴포넌트의 Phys Material Override로 지정하면 스캐너가 읽는다.
 */
UCLASS(BlueprintType, meta = (DisplayName = "Acoustic Physical Material"))
class UAcousticPhysicalMaterial : public UPhysicalMaterial
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Acoustics")
	FAcousticAbsorption Absorption;
};

/** 코드로 생성하는 벽(테스트 레이아웃)용: 컴포넌트가 흡음계수를 직접 들고 있다. */
UCLASS(ClassGroup = (Audio), meta = (BlueprintSpawnableComponent))
class UAcousticWallComponent : public UInstancedStaticMeshComponent
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Acoustics")
	FAcousticAbsorption Absorption;
};

namespace OrganicReverb
{
	constexpr double CmToM = 0.01;

	inline Acoustic::FVec3 ToCore(const FVector& V)
	{
		return Acoustic::FVec3(static_cast<float>(V.X * CmToM), static_cast<float>(V.Y * CmToM), static_cast<float>(V.Z * CmToM));
	}

	inline FVector ToUnreal(const Acoustic::FVec3& V)
	{
		return FVector(V.X, V.Y, V.Z) / CmToM;
	}
}
