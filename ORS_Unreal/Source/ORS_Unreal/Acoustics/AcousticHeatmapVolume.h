// AcousticHeatmapVolume.h
// ---------------------------------------------------------
// Unreal 어댑터: 방별 음향 에너지를 레벨 위에 겹쳐 그리는 히트맵 (6-3).
//
// 예전에는 방마다 바닥 칸에 DrawDebugSolidBox를 찍었다. 방 하나가 단색이라 경계가 뚝 끊기고,
// 비용 때문에 칸을 3만 개로 솎아내야 해서 복도를 타고 번지는 모습이 보이지 않았다.
//
// 대신 여기서는 스캔 범위를 격자(lattice)로 나눠 칸마다 "그 자리의 방 번호"를 미리 구워 두고,
// 매 틱 방별 에너지 색을 아틀라스 텍스처(Z 슬라이스를 타일로 펼친 2D 텍스처)에 채운다.
// 스캔 범위를 덮는 디퍼드 데칼(M_AcousticHeatmap)이 화면에 보이는 표면을 "그 지점의 에너지" 색으로
// 물들인다. 색을 데칼 UV가 아니라 표면의 월드 좌표로 고르기 때문에 늘어나지 않고,
// 바닥이든 벽이든 그 자리가 속한 방의 색이 칠해진다. 방 경계는 텍스처 보간으로 부드럽게 이어진다.
// ---------------------------------------------------------
#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "AcousticCore/AcousticSpaceSegmenter.h"
#include "AcousticHeatmapVolume.generated.h"

class UDecalComponent;
class UMaterialInstanceDynamic;
class UTexture2D;

UCLASS()
class UAcousticHeatmapVolume : public UObject
{
	GENERATED_BODY()

public:
	/** 방 그래프를 새로 만들 때 호출. 격자 칸마다 방 번호를 굽고 텍스처/박스를 준비한다 */
	void Rebuild(UWorld* World, const Acoustic::FSpaceSegmentation& Segmentation);

	/** 방별 색(에너지)을 텍스처에 반영. RoomColors[RoomId] */
	void Update(const TArray<FColor>& RoomColors);

	/** 화면에 보이는 밝기 (콘솔 ors.Heatmap.Intensity) */
	void SetLook(float Intensity);

	void SetVisible(bool bVisible);

	/** 박스와 텍스처를 해제 (레벨 종료 / 볼륨 등록 해제) */
	void Release();

	bool IsBuilt() const { return LatticeRooms.Num() > 0; }

private:
	bool EnsureMaterial();

	UPROPERTY(Transient)
	TObjectPtr<UDecalComponent> Decal;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> MaterialInstance;

	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> LatticeTexture;

	/** 격자 칸 → 방 번호 (INDEX_NONE = 방 없음). 크기 = LatticeX * LatticeY * LatticeZ */
	TArray<int32> LatticeRooms;

	int32 LatticeX = 0;
	int32 LatticeY = 0;
	int32 LatticeZ = 0;
	int32 TilesX = 0;
	int32 TilesY = 0;
	int32 AtlasWidth = 0;
	int32 AtlasHeight = 0;
};
