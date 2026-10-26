#include "OutlawsGameMode.h"

#include "Components/CapsuleComponent.h"

AOutlawsStandInPawn::AOutlawsStandInPawn()
{
	GetCapsuleComponent()->InitCapsuleSize(55.f, 96.f);
}

AOutlawsGameMode::AOutlawsGameMode()
{
	DefaultPawnClass = AOutlawsStandInPawn::StaticClass();
}
