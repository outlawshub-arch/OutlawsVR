#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "OutlawsMpPolish.generated.h"

class AOutlawsLevelActor;
class FOutlawsAssets;

/**
 * M4's standalone polish (PLANS.md "M4. Polish"): the original's multiplayer music, a hit confirmation for the shooter, and
 * the taunt keys.  Each works in single player too; co-op is only looked at (no link carries a taunt yet: M1's session will).
 */
namespace OutlawsMpPolish
{
	/** The music script a level plays: its own (LVT/LVB MUSIC), or for a multiplayer map (one with an .rcm) left at NULL, the
	 *  remaster's "<map>.msc" where it ships one unused (wilderns), else town.msc's rotation (TRK08A, TRK06A, TRK03A), the one
	 *  the remaster gives Sanctuary, Civil War and Little Battle. */
	OUTLAWSRUNTIME_API FString LevelMusic(const AOutlawsLevelActor* L, const FString& Msc);
	OUTLAWSRUNTIME_API FString MusicFor(const FOutlawsAssets* A, const FString& Level, const FString& Msc);
	OUTLAWSRUNTIME_API bool IsMultiplayerMap(const FOutlawsAssets* A, const FString& Level);

	/** This machine's player's shot struck a living bad guy (once a volley), or killed him: a short tick in the head. */
	OUTLAWSRUNTIME_API void HitConfirm(AOutlawsLevelActor* L, bool bKill);

	/** Taunt slot 1..10 (F1..F10, as the remaster's quick taunts): the slot's wav (OutlawsSettings.ini Taunt1..Taunt10, else
	 *  the original's LOCAL.MSG defaults) said at the player.  False while a taunt still plays. */
	OUTLAWSRUNTIME_API bool Taunt(AOutlawsLevelActor* L, int32 Slot);
	OUTLAWSRUNTIME_API FString TauntSlotWav(int32 Slot);
	/** VR (M4): the move stick held HeldSeconds and let go: past 0.6 s the next taunt slot is said (true: it was a taunt's
	 *  press, not the run toggle's).  ol.VrTaunt 0 turns it off. */
	OUTLAWSRUNTIME_API bool VrTauntPress(AOutlawsLevelActor* L, double HeldSeconds);
}

/** -OLM4Test: the music choice, a hit and a kill confirmed, the taunt slots; each proved by its log lines. */
UCLASS()
class OUTLAWSRUNTIME_API UOutlawsMpPolishTest : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Conditional; }
	virtual bool IsTickable() const override { return !IsTemplate(); }
	virtual bool IsTickableInEditor() const override { return false; }
	virtual UWorld* GetTickableGameObjectWorld() const override;

private:
	int32 Step = 0;
	double At = 0.0;
	int32 Bad = 0;
};
