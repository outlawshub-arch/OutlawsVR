// -OLShots -OLSkipInvTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLSkipInvTest) { return OLFlags::Param(TEXT("OLSkipInvTest")); }

OL_SHOT_TEST(OLSkipInvTest, 550)
{
	// BUG-SKIPLEVELINV (-OLLevel=hideout -OLNoFrontEnd -OLShots): the start kit noted; every weapon and some armour added; the cheat's
	// skip to the town (as the menu's row calls it) keeps the pockets, a restart there too; then a New Game gives the start kit again
	static int32 Fails = 0, Passes = 0;
	static FString Base, Full;
	static float Wait = 0.f;
	auto Check = [](bool bOk, const FString& What) { UE_LOG(LogOutlaws, Display, TEXT("OLSKIPINV check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What); if (bOk) ++Passes; else ++Fails; };
	if (!L) return;
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && ShotTime > Wait;
	if (ShotStep == 0 && ShotTime > 3.f && bUp)
	{
		Base = L->CarrySignature();
		L->GiveAllWeapons();
		L->SetArmour(40);
		Full = L->CarrySignature();
		UE_LOG(LogOutlaws, Display, TEXT("OLSKIPINV start kit: %s"), *Base);
		UE_LOG(LogOutlaws, Display, TEXT("OLSKIPINV full kit:  %s"), *Full);
		Check(Full != Base, TEXT("the cheat gave more than the start kit"));
		L->JumpToLevel(TEXT("town"), true);
		Wait = ShotTime + 3.f;
		ShotStep = 1;
	}
	else if (ShotStep == 1 && ShotTime > 3.f && bUp && L->LevelName == TEXT("town"))
	{
		Check(L->CarrySignature() == Full, FString::Printf(TEXT("after the skip to the town: %s"), *L->CarrySignature()));
		Check(L->GetHealth() >= 10.f, TEXT("health full (as after a won level)"));
		L->RestartLevel();
		Wait = ShotTime + 3.f;
		ShotStep = 2;
	}
	else if (ShotStep == 2 && bUp)
	{
		Check(L->CarrySignature() == Full, FString::Printf(TEXT("after a restart of the town: %s"), *L->CarrySignature()));
		L->JumpToLevel(TEXT("hideout"));          // (the plain skip: the start kit, as before the fix)
		Wait = ShotTime + 3.f;
		ShotStep = 3;
	}
	else if (ShotStep == 3 && bUp && L->LevelName == TEXT("hideout"))
	{
		Check(L->CarrySignature() == Base, FString::Printf(TEXT("a skip without the carry gives the start kit: %s"), *L->CarrySignature()));
		L->GiveAllWeapons();
		Check(L->CarrySignature() != Base, TEXT("pockets full again before the New Game"));
		L->StartStory(L->Difficulty);
		Wait = ShotTime + 3.f;
		ShotStep = 4;
	}
	else if (ShotStep == 4)
	{
		if (L->IsMoviePlaying()) L->SkipMovie();
		if (bUp && L->LevelName == TEXT("hideout"))
		{
			Check(L->CarrySignature() == Base, FString::Printf(TEXT("a New Game gives the start kit: %s"), *L->CarrySignature()));
			UE_LOG(LogOutlaws, Display, TEXT("OLSKIPINV RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
			Wait = ShotTime + 1.f;
			ShotStep = 5;
		}
	}
	else if (ShotStep == 5 && ShotTime > Wait)
	{
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
