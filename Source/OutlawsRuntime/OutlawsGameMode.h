#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "GameFramework/GameModeBase.h"
#include "OutlawsGameMode.generated.h"

/** The pawn a single player's controller starts with until the level is built: AOutlawsLevelActor::TryPlacePlayer swaps it
 *  for AOutlawsPlayer and destroys it.  The first-person template's character had this job (the same capsule); it has no
 *  camera, mesh or input of its own. */
UCLASS(NotPlaceable, Transient)
class OUTLAWSRUNTIME_API AOutlawsStandInPawn : public ACharacter
{
	GENERATED_BODY()

public:
	AOutlawsStandInPawn();
};

/** The project's default game mode (Config/DefaultEngine.ini GlobalDefaultGameMode, REQ-TIDY tidy-6): the template's
 *  BP_FirstPersonGameMode with its pawn swapped for the stand-in above.  Co-op sessions open AOutlawsCoopGameMode instead. */
UCLASS(NotPlaceable)
class OUTLAWSRUNTIME_API AOutlawsGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AOutlawsGameMode();
};
