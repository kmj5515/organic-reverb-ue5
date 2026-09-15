#include "Acoustics/AcousticHeatmapVolume.h"

#include "Acoustics/AcousticSurfaceTypes.h"
#include "Components/DecalComponent.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/WorldSettings.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "RenderUtils.h"

DEFINE_LOG_CATEGORY_STATIC(LogAcousticHeatmap, Log, All);

namespace
{
	// 격자 해상도 상한. 시각화용이라 스캔 격자를 그대로 쓸 필요가 없다 (아틀라스 크기와 업로드 비용을 묶어 둔다)
	constexpr int32 MaxLatticeXY = 96;
	constexpr int32 MaxLatticeZ = 48;

	const TCHAR* HeatmapMaterialPath = TEXT("/Game/OrganicReverb/Debug/M_AcousticHeatmap.M_AcousticHeatmap");
}

void UAcousticHeatmapVolume::Release()
{
	if (Decal)
	{
		Decal->DestroyComponent();
		Decal = nullptr;
	}
	MaterialInstance = nullptr;
	LatticeTexture = nullptr;
	LatticeRooms.Reset();
	LatticeX = LatticeY = LatticeZ = 0;
}

bool UAcousticHeatmapVolume::EnsureMaterial()
{
	if (MaterialInstance) return true;

	UMaterialInterface* Material = LoadObject<UMaterialInterface>(nullptr, HeatmapMaterialPath);
	if (!Material)
	{
		UE_LOG(LogAcousticHeatmap, Warning,
			TEXT("Heatmap material %s not found. Run Scripts/create_heatmap_material.py to generate it."), HeatmapMaterialPath);
		return false;
	}

	MaterialInstance = UMaterialInstanceDynamic::Create(Material, this);
	return MaterialInstance != nullptr;
}

void UAcousticHeatmapVolume::Rebuild(UWorld* World, const Acoustic::FSpaceSegmentation& Segmentation)
{
	Release();
	if (!World || Segmentation.SizeX <= 0 || Segmentation.SizeY <= 0 || Segmentation.SizeZ <= 0) return;
	if (!EnsureMaterial()) return;

	LatticeX = FMath::Clamp(Segmentation.SizeX, 1, MaxLatticeXY);
	LatticeY = FMath::Clamp(Segmentation.SizeY, 1, MaxLatticeXY);
	LatticeZ = FMath::Clamp(Segmentation.SizeZ, 1, MaxLatticeZ);

	// Z 슬라이스를 가로 TilesX개씩 늘어놓아 2D 아틀라스로 만든다
	TilesX = FMath::CeilToInt(FMath::Sqrt(static_cast<float>(LatticeZ)));
	TilesY = FMath::DivideAndRoundUp(LatticeZ, TilesX);
	AtlasWidth = LatticeX * TilesX;
	AtlasHeight = LatticeY * TilesY;

	// 격자 칸마다 방 번호를 굽는다. 칸 하나가 스캔 격자 여러 칸을 덮으므로, 그 안에 방이 하나라도
	// 있으면 그 방으로 친다 (얇은 벽 때문에 히트맵에 구멍이 뚫리지 않도록)
	LatticeRooms.Init(INDEX_NONE, LatticeX * LatticeY * LatticeZ);
	for (int32 LZ = 0; LZ < LatticeZ; ++LZ)
	{
		const int32 Z0 = LZ * Segmentation.SizeZ / LatticeZ;
		const int32 Z1 = FMath::Max(Z0 + 1, (LZ + 1) * Segmentation.SizeZ / LatticeZ);
		for (int32 LY = 0; LY < LatticeY; ++LY)
		{
			const int32 Y0 = LY * Segmentation.SizeY / LatticeY;
			const int32 Y1 = FMath::Max(Y0 + 1, (LY + 1) * Segmentation.SizeY / LatticeY);
			for (int32 LX = 0; LX < LatticeX; ++LX)
			{
				const int32 X0 = LX * Segmentation.SizeX / LatticeX;
				const int32 X1 = FMath::Max(X0 + 1, (LX + 1) * Segmentation.SizeX / LatticeX);

				Acoustic::RoomId Found = Acoustic::InvalidRoomId;
				for (int32 Z = Z0; Z < Z1 && Found == Acoustic::InvalidRoomId; ++Z)
					for (int32 Y = Y0; Y < Y1 && Found == Acoustic::InvalidRoomId; ++Y)
						for (int32 X = X0; X < X1 && Found == Acoustic::InvalidRoomId; ++X)
						{
							const Acoustic::RoomId Room = Segmentation.GetCellRoom(X, Y, Z);
							if (Room != Acoustic::InvalidRoomId) Found = Room;
						}

				if (Found != Acoustic::InvalidRoomId)
				{
					LatticeRooms[(LZ * LatticeY + LY) * LatticeX + LX] = static_cast<int32>(Found);
				}
			}
		}
	}

	LatticeTexture = UTexture2D::CreateTransient(AtlasWidth, AtlasHeight, PF_B8G8R8A8);
	if (!LatticeTexture)
	{
		Release();
		return;
	}
	LatticeTexture->SRGB = false;
	LatticeTexture->Filter = TextureFilter::TF_Bilinear;
	LatticeTexture->AddressX = TextureAddress::TA_Clamp;
	LatticeTexture->AddressY = TextureAddress::TA_Clamp;
	LatticeTexture->CompressionSettings = TextureCompressionSettings::TC_VectorDisplacementmap;
	LatticeTexture->NeverStream = true;
	LatticeTexture->UpdateResource();

	// 스캔 범위를 덮는 박스 (음향 좌표 m → 언리얼 cm)
	const FVector BoundsMin = OrganicReverb::ToUnreal(Segmentation.Origin);
	const FVector BoundsSize(
		Segmentation.SizeX * Segmentation.CellSize / OrganicReverb::CmToM,
		Segmentation.SizeY * Segmentation.CellSize / OrganicReverb::CmToM,
		Segmentation.SizeZ * Segmentation.CellSize / OrganicReverb::CmToM);

	MaterialInstance->SetTextureParameterValue(TEXT("LatticeTex"), LatticeTexture);
	MaterialInstance->SetVectorParameterValue(TEXT("BoundsMin"), FLinearColor(BoundsMin));
	MaterialInstance->SetVectorParameterValue(TEXT("BoundsSize"), FLinearColor(BoundsSize));
	MaterialInstance->SetVectorParameterValue(TEXT("Lattice"), FLinearColor(LatticeX, LatticeY, LatticeZ, 0.f));
	MaterialInstance->SetVectorParameterValue(TEXT("Tiles"), FLinearColor(TilesX, TilesY, 0.f, 0.f));

	// 스캔 범위 전체를 덮는 데칼. 위에서 아래로 투영하지만, 색은 데칼 UV가 아니라 표면의 월드 좌표로
	// 고르므로 늘어나는 문제가 없다. 바닥이든 벽이든 "그 지점이 속한 방"의 색으로 칠해진다.
	Decal = NewObject<UDecalComponent>(World->GetWorldSettings(), NAME_None, RF_Transient);
	Decal->SetDecalMaterial(MaterialInstance);
	Decal->SetFadeScreenSize(0.f); // 멀어져도 사라지지 않게
	Decal->RegisterComponentWithWorld(World);
	Decal->SetWorldLocation(BoundsMin + BoundsSize * 0.5);
	Decal->SetWorldRotation(FRotator(-90.f, 0.f, 0.f)); // 로컬 X(투영 방향)를 아래로
	// 데칼 상자는 로컬 (투영 깊이, Y, Z)의 절반 크기. 위 회전에서 로컬 Y = 월드 Y, 로컬 Z = 월드 X
	Decal->DecalSize = FVector(BoundsSize.Z * 0.5, BoundsSize.Y * 0.5, BoundsSize.X * 0.5);
	Decal->SetHiddenInGame(true);
	Decal->MarkRenderStateDirty();

	UE_LOG(LogAcousticHeatmap, Log, TEXT("Heatmap lattice %dx%dx%d -> atlas %dx%d (%d tiles), bounds min %s size %s"),
		LatticeX, LatticeY, LatticeZ, AtlasWidth, AtlasHeight, TilesX * TilesY, *BoundsMin.ToString(), *BoundsSize.ToString());
}

void UAcousticHeatmapVolume::Update(const TArray<FColor>& RoomColors)
{
	if (!IsBuilt() || !LatticeTexture || !LatticeTexture->GetResource()) return;

	const int32 NumBytes = AtlasWidth * AtlasHeight * 4;
	uint8* Pixels = static_cast<uint8*>(FMemory::Malloc(NumBytes));
	FMemory::Memzero(Pixels, NumBytes);

	for (int32 LZ = 0; LZ < LatticeZ; ++LZ)
	{
		const int32 TileX = (LZ % TilesX) * LatticeX;
		const int32 TileY = (LZ / TilesX) * LatticeY;
		for (int32 LY = 0; LY < LatticeY; ++LY)
		{
			for (int32 LX = 0; LX < LatticeX; ++LX)
			{
				const int32 Room = LatticeRooms[(LZ * LatticeY + LY) * LatticeX + LX];
				if (!RoomColors.IsValidIndex(Room)) continue;

				const FColor Color = RoomColors[Room];
				uint8* Pixel = Pixels + ((TileY + LY) * AtlasWidth + (TileX + LX)) * 4;
				Pixel[0] = Color.B;
				Pixel[1] = Color.G;
				Pixel[2] = Color.R;
				Pixel[3] = Color.A;
			}
		}
	}

	static FUpdateTextureRegion2D Region(0, 0, 0, 0, 0, 0);
	Region.Width = AtlasWidth;
	Region.Height = AtlasHeight;

	LatticeTexture->UpdateTextureRegions(0, 1, &Region, AtlasWidth * 4, 4, Pixels,
		[](uint8* Data, const FUpdateTextureRegion2D*) { FMemory::Free(Data); });
}

void UAcousticHeatmapVolume::SetLook(float Intensity)
{
	if (MaterialInstance) MaterialInstance->SetScalarParameterValue(TEXT("Intensity"), Intensity);
}

void UAcousticHeatmapVolume::SetVisible(bool bVisible)
{
	if (Decal) Decal->SetHiddenInGame(!bVisible);
}
