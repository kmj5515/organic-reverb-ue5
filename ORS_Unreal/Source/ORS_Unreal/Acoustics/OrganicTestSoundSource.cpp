// OrganicTestSoundSource.cpp
#include "Acoustics/OrganicTestSoundSource.h"
#include "Acoustics/OrganicSoundSourceComponent.h"
#include "Components/AudioComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Sound/SoundBase.h"
#include "UObject/ConstructorHelpers.h"

AOrganicTestSoundSource::AOrganicTestSoundSource()
{
	PrimaryActorTick.bCanEverTick = false;

	static ConstructorHelpers::FObjectFinder<USoundBase> Ping(TEXT("/Engine/EngineSounds/1kSineTonePing.1kSineTonePing"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));

	Audio = CreateDefaultSubobject<UAudioComponent>(TEXT("Audio"));
	RootComponent = Audio;
	Audio->Sound = Ping.Object;
	Audio->bAutoActivate = false;
	Audio->bOverrideAttenuation = true;
	Audio->AttenuationOverrides.bSpatialize = true;
	Audio->AttenuationOverrides.bEnableReverbSend = false; // Reverb Send는 OrganicSource가 잔향 결합만큼 직접 넣는다

	Marker = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Marker"));
	Marker->SetupAttachment(Audio);
	Marker->SetStaticMesh(Sphere.Object);
	Marker->SetRelativeScale3D(FVector(0.3f));
	Marker->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Marker->SetCanEverAffectNavigation(false);

	OrganicSource = CreateDefaultSubobject<UOrganicSoundSourceComponent>(TEXT("OrganicSource"));
	OrganicSource->RepeatInterval = 1.5f;
	OrganicSource->bEmitAcousticEventOnPlay = true;
}
