// AcousticTestLayout.h
// ---------------------------------------------------------
// 테스트 씬 화이트박싱용: 방 목록으로 벽/바닥/천장을 생성한다 (에셋 불필요).
// - 인접한 방은 50 cm(격자 1칸) 띄워 배치한다. 벽 두께 25 cm면 두 방의 벽이 겹치지 않고 나란히 서서
//   스캐너가 양쪽 면 재질을 구분한다. 문은 양쪽 방에 같은 위치로 지정.
// - 방 좌표를 격자 칸 크기(기본 50 cm)의 배수로 두면 스캔 결과가 정확해진다.
// - 디테일 패널의 Preset 버튼으로 테스트 씬 5종을 한 번에 만들 수 있다.
// ---------------------------------------------------------
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Acoustics/AcousticSurfaceTypes.h"
#include "AcousticTestLayout.generated.h"

class UStaticMesh;

UENUM(BlueprintType)
enum class EAcousticWallSide : uint8
{
	NegX,
	PosX,
	NegY,
	PosY
};

USTRUCT(BlueprintType)
struct FAcousticTestDoor
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Door")
	EAcousticWallSide Side = EAcousticWallSide::PosX;

	/** 방 내부 최소 코너 기준, 벽을 따라 문 시작점까지 거리 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Door", meta = (Units = "cm"))
	float Offset = 150.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Door", meta = (Units = "cm"))
	float Width = 100.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Door", meta = (Units = "cm"))
	float Height = 200.f;
};

USTRUCT(BlueprintType)
struct FAcousticTestRoom
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Room")
	FString Name;

	/** 방 내부 최소 코너 (액터 로컬, cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Room")
	FVector Origin = FVector::ZeroVector;

	/** 방 내부 크기 (cm) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Room")
	FVector Size = FVector(600.f, 500.f, 300.f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Room")
	EAcousticMaterialPreset Material = EAcousticMaterialPreset::Concrete;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Room")
	bool bCeiling = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Room")
	TArray<FAcousticTestDoor> Doors;
};

UCLASS(meta = (DisplayName = "Acoustic Test Layout"))
class AAcousticTestLayout : public AActor
{
	GENERATED_BODY()

public:
	AAcousticTestLayout();
	virtual void OnConstruction(const FTransform& Transform) override;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	TArray<FAcousticTestRoom> Rooms;

	/** 방마다 자기 벽 두께. 인접한 방 간격(50 cm)의 절반이면 벽이 겹치지 않는다 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout", meta = (Units = "cm", ClampMin = "5.0"))
	float WallThickness = 25.f;

	/** 레이아웃이 바뀌면 Organic Reverb Volume 스캔 범위를 레이아웃에 맞춘다 (없으면 에디터에서 생성) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Layout")
	bool bAutoFitReverbVolume = true;

	/** 씬 1: 가변 룸 — 런타임에 SetRoomSize로 크기를 바꿔 리버브 변화 시연 */
	UFUNCTION(CallInEditor, Category = "Layout|Presets")
	void PresetModularRoom();

	/** 씬 2: 재질 비교 — 같은 크기 방 두 개 (콘크리트 / 카펫), 바깥으로 문 */
	UFUNCTION(CallInEditor, Category = "Layout|Presets")
	void PresetMaterialComparison();

	/** 씬 5: BODYCAM 히트맵 — 24m 복도 + 옆방 3개 */
	UFUNCTION(CallInEditor, Category = "Layout|Presets")
	void PresetCorridorWithSideRooms();

	/** 결합 공간: 카펫 옷장 + 콘크리트 대형 홀 (이중 기울기 감쇠) */
	UFUNCTION(CallInEditor, Category = "Layout|Presets")
	void PresetClosetAndHall();

	/** 씬 3: 개방-밀폐 전이 — 천장 없는 안뜰 + 실내 방 */
	UFUNCTION(CallInEditor, Category = "Layout|Presets")
	void PresetCourtyard();

	/** 씬 4: 벽 너머 소리 — 같은 크기 방 두 개, 벽 한쪽 끝에만 문 (문 쪽으로 걸어가면 직선 가시) */
	UFUNCTION(CallInEditor, Category = "Layout|Presets")
	void PresetWallTest();

	/** 원거리 총성: 30 m 복도 + 모퉁이 + 20 m 복도, 복도 시작점 옆에 대기실 (먼 총성이 복도를 타고 들려옴) */
	UFUNCTION(CallInEditor, Category = "Layout|Presets")
	void PresetLongCorridor();

	/** 지오메트리 재생성 + 볼륨 맞춤 + (플레이 중이면) 음향 그래프 재빌드 */
	UFUNCTION(BlueprintCallable, CallInEditor, Category = "Layout")
	void RebuildLayout();

	UFUNCTION(BlueprintCallable, Category = "Layout")
	void SetRoomSize(int32 RoomIndex, FVector NewSize);

	UFUNCTION(BlueprintCallable, Category = "Layout")
	void SetRoomMaterial(int32 RoomIndex, EAcousticMaterialPreset NewMaterial);

	/** 레이아웃 전체의 월드 AABB. 방 내부에서 max(벽 두께, MinPadding)만큼 넓힌다 —
	 *  MinPadding을 격자 칸 크기로 주면 벽이 얇아도 격자가 방 좌표에 정렬된다 (천장 없는 방의 위쪽은 넓히지 않음) */
	FBox GetLayoutBounds(float MinPadding = 50.f) const;

protected:
	virtual void BeginPlay() override;

private:
	void BuildGeometry();
	void FitReverbVolume();
	void AddBox(EAcousticMaterialPreset Material, const FVector& Min, const FVector& Max);
	void AddWall(const FAcousticTestRoom& Room, EAcousticWallSide Side);

	/** EAcousticMaterialPreset 순서 (재질마다 ISM 하나) */
	UPROPERTY(VisibleAnywhere, Category = "Layout")
	TArray<TObjectPtr<UAcousticWallComponent>> WallComponents;

	UPROPERTY()
	TObjectPtr<UStaticMesh> CubeMesh;
};
