// OrganicReverbVolume.cpp
#include "Acoustics/OrganicReverbVolume.h"
#include "Acoustics/OrganicReverbSubsystem.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"

AOrganicReverbVolume::AOrganicReverbVolume()
{
	PrimaryActorTick.bCanEverTick = false;

	ScanBox = CreateDefaultSubobject<UBoxComponent>(TEXT("ScanBox"));
	ScanBox->SetBoxExtent(FVector(1000.f, 1000.f, 300.f));
	ScanBox->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ScanBox->SetCanEverAffectNavigation(false);
	ScanBox->ShapeColor = FColor(80, 200, 255);
	RootComponent = ScanBox;

	OutsideAbsorption = FAcousticAbsorption::FromCore(Acoustic::MaterialPresets::OpenAir());
}

FBox AOrganicReverbVolume::GetScanBounds() const
{
	return ScanBox ? ScanBox->Bounds.GetBox() : FBox(ForceInit);
}

void AOrganicReverbVolume::SetScanBounds(const FBox& WorldBox)
{
	SetActorScale3D(FVector::OneVector);
	SetActorRotation(FRotator::ZeroRotator);
	SetActorLocation(WorldBox.GetCenter());
	ScanBox->SetBoxExtent(WorldBox.GetExtent());
}

void AOrganicReverbVolume::BeginPlay()
{
	Super::BeginPlay();
	if (UOrganicReverbSubsystem* Subsystem = GetWorld()->GetSubsystem<UOrganicReverbSubsystem>())
	{
		Subsystem->RegisterVolume(this);
	}
}

void AOrganicReverbVolume::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (UOrganicReverbSubsystem* Subsystem = GetWorld()->GetSubsystem<UOrganicReverbSubsystem>())
	{
		Subsystem->UnregisterVolume(this);
	}
	Super::EndPlay(EndPlayReason);
}
