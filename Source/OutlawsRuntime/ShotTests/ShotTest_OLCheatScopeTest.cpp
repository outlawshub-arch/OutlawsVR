// -OLShots -OLCheatScopeTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLCheatScopeTest) { return OLFlags::Param(TEXT("OLCheatScopeTest")); }

OL_SHOT_TEST(OLCheatScopeTest, 540)
{
	// REQ-CHEATSCOPE (-OLLevel=hideout -OLNoFrontEnd): no cheat, no scope (the rifle alone does not zoom); then the all-weapons
	// cheat gives the scope as a pickup would, and the rifle zooms with it
	auto Check = [](bool bOk, const FString& What) { UE_LOG(LogOutlaws, Display, TEXT("OLCHEATSCOPE check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What); return bOk ? 0 : 1; };
	static int32 Fails = 0;
	if (ShotStep == 0 && ShotTime > 3.f && L)
	{
		Fails += Check(L->GetCount(TEXT("iscope")) == 0, FString::Printf(TEXT("no scope at the start (%d)"), L->GetCount(TEXT("iscope"))));
		CheatScopeTestRaise();
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 7.f && L)
	{
		Fails += Check(L->GetCount(TEXT("iscope")) == 0 && !CheatScopeTestUp(), FString::Printf(TEXT("no cheat: scope owned %d, zoom up %d"), L->GetCount(TEXT("iscope")), CheatScopeTestUp() ? 1 : 0));
		L->GiveAllWeapons();          // (the cheat page's All weapons row calls this)
		++ShotStep;
	}
	else if (ShotStep == 2 && ShotTime > 11.f && L)
	{
		Fails += Check(L->GetCount(TEXT("iscope")) == 1, FString::Printf(TEXT("cheat: scope owned (%d)"), L->GetCount(TEXT("iscope"))));
		Fails += Check(CheatScopeTestUp(), TEXT("cheat: the rifle zooms (scope picture up)"));
		UE_LOG(LogOutlaws, Display, TEXT("OLCHEATSCOPE RESULT %s"), Fails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"));
		++ShotStep;
	}
	else if (ShotStep == 3 && ShotTime > 12.f)
	{
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
