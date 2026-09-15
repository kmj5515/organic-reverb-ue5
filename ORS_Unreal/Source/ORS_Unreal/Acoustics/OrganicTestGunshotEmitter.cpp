// OrganicTestGunshotEmitter.cpp
#include "Acoustics/OrganicTestGunshotEmitter.h"
#include "Acoustics/OrganicReverbSubsystem.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Sound/SoundBase.h"
#include "TimerManager.h"
#include "UObject/ConstructorHelpers.h"

AOrganicTestGunshotEmitter::AOrganicTestGunshotEmitter()
{
	PrimaryActorTick.bCanEverTick = false;

	static ConstructorHelpers::FObjectFinder<USoundBase> Ping(TEXT("/Engine/EngineSounds/1kSineTonePing.1kSineTonePing"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cone(TEXT("/Engine/BasicShapes/Cone.Cone"));
	NearSounds.Add(Ping.Object);

	Marker = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Marker"));
	RootComponent = Marker;
	Marker->SetStaticMesh(Cone.Object);
	Marker->SetRelativeScale3D(FVector(0.3f));
	Marker->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Marker->SetCanEverAffectNavigation(false);
}

void AOrganicTestGunshotEmitter::BeginPlay()
{
	Super::BeginPlay();
	GetWorldTimerManager().SetTimer(FireTimer, this, &AOrganicTestGunshotEmitter::Fire, Interval, true, 1.f);
}

void AOrganicTestGunshotEmitter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetWorldTimerManager().ClearTimer(FireTimer);
	Super::EndPlay(EndPlayReason);
}

void AOrganicTestGunshotEmitter::Fire()
{
	auto PickRandom = [](const TArray<TObjectPtr<USoundBase>>& Sounds) -> USoundBase*
	{
		return Sounds.Num() > 0 ? Sounds[FMath::RandRange(0, Sounds.Num() - 1)].Get() : nullptr;
	};

	if (UOrganicReverbSubsystem* Subsystem = GetWorld()->GetSubsystem<UOrganicReverbSubsystem>())
	{
		Subsystem->PlayGunshotAtLocation(PickRandom(NearSounds), PickRandom(FarSounds), GetActorLocation(), Intensity);
	}
}
