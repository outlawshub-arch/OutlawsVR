// BUG-WHEELHITCH (the Quest 2 Nov: the first wheel open of a run 215 ms, the later ones a normal frame): -OLShots -OLNoFrontEnd
// -OLLevel=hideout -OLWheelOpenProbe [-OLOldWheelOpen], flat.  Once the level start's prebuild has run, the wheel opened and shut
// four times: each open's ms and the objects it made (the engine's object count, the wheel's slots, plates and pictures).  PASS
// when the first open makes no object and costs under twice a later one; -OLOldWheelOpen prints the before (the first open makes
// the wheel).  Lines "OLSHOT wheelopen check PASS|FAIL: ..." and "OLSHOT wheelopen RESULT".
// BUG-WHEELLATE (the Quest 7 Nov: the slow opens made 2 pictures after a key or the crowbar was picked up, or 5 objects at a
// match's first open, the scorecard's art and canvas): then a listed item this level's ground items give (the first one not
// carried) is given as its pickup gives it, and a fifth open must make no object either (a level with none says so and skips
// that case).  In a match (-OLMpHost=<map> -OLMpBots=<n> -OLMpStartWhen=0 -OLLobbyEnterAfter=4) the first open waits for the
// scores to have a row, so the scorecard is drawn with players in it.  -OLOldWheelLate: the before (the card made at that
// open, the scorecard drawn at the first; FAIL expected).

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "OutlawsCoop.h"
#include "Engine/World.h"
#include "Engine/Texture2D.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLWheelOpenProbe) { return OLFlags::Param(TEXT("OLWheelOpenProbe")); }

OL_SHOT_TEST(OLWheelOpenProbe, 1600)
{
	static int32 Passes = 0, Fails = 0, Opens = 0;
	static float At = 0.f;
	static TArray<float> Ms;
	static TArray<int32> Made;
	static FString GroundItem;
	static int32 PickMade = -1;
	static float PickMs = -1.f;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wheelopen check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const bool bOld = OldWheelOpen();
	const bool bLate = OldWheelLate();
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	UOutlawsCoop* C = UOutlawsCoop::Get(this);
	const AOutlawsCoopSession* S = C ? C->GetSession() : nullptr;
	const bool bMatch = C && C->IsMatch();
	auto Open = [&](int32 Number)
	{
		const int32 SlotsWas = WheelSlots.Num(), PlatesWas = PlateRTs.Num(), PicsWas = KpfTextures.Num();
		TArray<FString> PicsBefore;
		KpfTextures.GetKeys(PicsBefore);
		OpenWheel(1);
		for (const TPair<FString, TObjectPtr<UTexture2D>>& Kv : KpfTextures)
		{
			if (!PicsBefore.Contains(Kv.Key)) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wheelopen open %d: a picture read at the open: %s (%s)"), Number, *Kv.Key, Kv.Value ? TEXT("found") : TEXT("missing"));
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wheelopen open %d: %.2f ms, %d objects made; slots %d (+%d), plates %d (+%d), pictures %d (+%d); open %d%s"), Number, WheelOpenMs, WheelOpenObjects,
			WheelSlots.Num(), WheelSlots.Num() - SlotsWas, PlateRTs.Num(), PlateRTs.Num() - PlatesWas, KpfTextures.Num(), KpfTextures.Num() - PicsWas, bWheelOpen ? 1 : 0,
			WheelScore.IsValid() ? TEXT(", the scorecard under it") : TEXT(""));
	};
	switch (ShotStep)
	{
	case 0:
		if (ShotTime < 4.f || !bUp || WarmFrames <= 3) return;
		// (BUG-WHEELLATE) a match: the scores with a row first (the bots or the host joined), as a player's first open has them
		if (bMatch && (!S || S->Scores.Num() == 0))
		{
			if (ShotTime < 150.f) return;
			Check(false, TEXT("the match's scores never had a row"));
			ShotStep = 3;
			return;
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wheelopen %s%s: prebuilt %d, %d slots, %d plates, %d pictures before the first open%s"), bOld ? TEXT("old (no prebuild)") : TEXT("prebuild"),
			bLate ? TEXT(", -OLOldWheelLate") : TEXT(""), bWheelPrebuilt ? 1 : 0, WheelSlots.Num(), PlateRTs.Num(), KpfTextures.Num(),
			bMatch ? *FString::Printf(TEXT("; a match, %d in the scores"), S ? S->Scores.Num() : 0) : TEXT(""));
		At = ShotTime;
		ShotStep = 1;
		return;
	case 1:
		if (ShotTime < At + 0.5f) return;
		Open(Opens + 1);
		Ms.Add(WheelOpenMs);
		Made.Add(WheelOpenObjects);
		At = ShotTime;
		ShotStep = 2;
		return;
	case 2:
		if (ShotTime < At + 0.5f) return;
		CloseWheel();
		At = ShotTime;
		ShotStep = ++Opens < 4 ? 1 : 4;
		return;
	case 4:
	{
		// (BUG-WHEELLATE) a listed item this level's ground items give, given as its pickup gives it, then the wheel opened
		if (ShotTime < At + 0.5f) return;
		TArray<FString> Ground, Carried, Unused;
		L->GetGroundWheelItems(Ground);
		L->GetWheelItems(Carried, Unused);
		for (const FString& It : Ground)
		{
			if (!Carried.Contains(It))
			{
				GroundItem = It;
				break;
			}
		}
		if (GroundItem.IsEmpty())
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wheelopen pickup case not run: %s has no listed ground item that is not carried (%d listed: %s)"), *L->LevelName, Ground.Num(), *FString::Join(Ground, TEXT(" ")));
			ShotStep = 3;
			return;
		}
		L->DebugGiveListed(GroundItem, GroundItem);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wheelopen pickup case: %s given (one of %d listed ground items here: %s)"), *GroundItem, Ground.Num(), *FString::Join(Ground, TEXT(" ")));
		Open(5);
		PickMade = WheelOpenObjects;
		PickMs = WheelOpenMs;
		At = ShotTime;
		ShotStep = 5;
		return;
	}
	case 5:
		if (ShotTime < At + 0.5f) return;
		CloseWheel();
		ShotStep = 3;
		return;
	case 3:
	{
		float Later = 0.f;
		for (int32 k = 1; k < Ms.Num(); ++k) Later = FMath::Max(Later, Ms[k]);
		const FString Before = FString::Printf(TEXT("the first open %.2f ms, %d objects; the later ones at most %.2f ms (%s objects)"), Ms.Num() ? Ms[0] : -1.f, Made.Num() ? Made[0] : -1, Later,
			*FString::JoinBy(Made, TEXT(", "), [](int32 N) { return FString::FromInt(N); }));
		if (bOld) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wheelopen BEFORE (-OLOldWheelOpen): %s"), *Before);
		if (bLate) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wheelopen BEFORE (-OLOldWheelLate): %s; the open after the pickup %.2f ms, %d objects"), *Before, PickMs, PickMade);
		Check(Ms.Num() == 4, FString::Printf(TEXT("four opens (%d)"), Ms.Num()));
		Check(Made.Num() > 0 && Made[0] == 0, FString::Printf(TEXT("the first open makes no object (%d)%s"), Made.Num() ? Made[0] : -1, bMatch ? TEXT(", the match's scorecard with it") : TEXT("")));
		// (a millisecond's floor: the later opens cost a few, the timer's own noise is a fraction of one)
		Check(Ms.Num() > 1 && Ms[0] < 2.f * FMath::Max(Later, 1.f), FString::Printf(TEXT("the first open costs under twice a later one: %s"), *Before));
		bool bLaterNone = true;
		for (int32 k = 1; k < Made.Num(); ++k) bLaterNone &= Made[k] == 0;
		Check(bLaterNone, TEXT("the later opens make no object"));
		if (!GroundItem.IsEmpty())
		{
			Check(PickMade == 0, FString::Printf(TEXT("the open after %s was picked up makes no object (%d, %.2f ms)"), *GroundItem, PickMade, PickMs));
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wheelopen RESULT %s (%d passed, %d failed)%s"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			bOld ? TEXT(" (-OLOldWheelOpen: the before, FAIL expected)") : bLate ? TEXT(" (-OLOldWheelLate: the before, FAIL expected)") : TEXT(""));
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	}
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
