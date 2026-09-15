// OrganicReverbSubsystem.h
// ---------------------------------------------------------
// Unreal 어댑터 레이어: Core(FAcousticRoomGraph + FAcousticDiffusionSimulator)를
// 소유하고, 매 틱 갱신하며, 결과를 MetaSounds/Submix 파라미터로 흘려보내고
// 디버그 히트맵을 그려주는 진입점.
//
// 게임플레이 코드(총기 발사 등)는 이 서브시스템의 BlueprintCallable 함수만
// 호출하면 되고, Core 내부 구현은 알 필요가 없다.
// ---------------------------------------------------------
#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Core/AcousticRoomGraph.h"
#include "Core/AcousticDiffusionSimulator.h"
#include "OrganicReverbSubsystem.generated.h"

class UAcousticGeometryScanner;

UCLASS()
class UOrganicReverbSubsystem : public UTickableWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;

	// 총성/폭발 등 이벤트 발생 시 호출. 월드 좌표 -> RoomId 변환은 내부에서 처리.
	UFUNCTION(BlueprintCallable, Category = "Acoustics")
	void EmitAcousticEvent(const FVector& WorldLocation, float Intensity);

	// 특정 리스너(플레이어) 위치 기준으로 현재 적용해야 할 Wet Level을 반환.
	// MetaSounds 그래프의 입력 파라미터로 바인딩해서 사용.
	UFUNCTION(BlueprintCallable, Category = "Acoustics")
	float GetWetLevelAtLocation(const FVector& ListenerLocation) const;

	// 레벨 로드 완료 후 (또는 파괴 가능한 지형 변경 후) 그래프 재빌드.
	UFUNCTION(BlueprintCallable, Category = "Acoustics")
	void RebuildAcousticGraph();

	// 디버그 히트맵 표시 On/Off (에디터/개발 빌드 전용)
	UFUNCTION(BlueprintCallable, Category = "Acoustics|Debug")
	void SetDebugVisualizationEnabled(bool bEnabled);

protected:
	// 매 틱 각 방의 에너지 값을 색으로 변환해서 DrawDebugBox/DrawDebugString으로 표시.
	// R=Low, G=Mid, B=High 매핑 (2번 문서 참고).
	void DrawDebugHeatmap() const;

private:
	Acoustic::FAcousticRoomGraph RoomGraph;
	TUniquePtr<Acoustic::FAcousticDiffusionSimulator> Simulator;

	UPROPERTY()
	TObjectPtr<UAcousticGeometryScanner> GeometryScanner = nullptr;

	bool bDebugVisualizationEnabled = false;
};
