// -OLShots -OLJackpotTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLJackpotTest) { return OLFlags::Param(TEXT("OLJackpotTest")); }

OL_SHOT_TEST(OLJackpotTest, 330)
{
	// OLJACKPOT (user 2026-10-03): the level's lock items given once (logged item by item), and a second time nothing
	if (ShotStep == 0 && ShotTime > 3.f && L)
	{
		const int32 A = L->CheatJackpot(), B = L->CheatJackpot();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT jackpot: first %d, again %d (expect again 0); wrist list: %s"), A, B, *FString::Join(L->GetInventoryLabels(), TEXT(", ")));
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 4.f) { FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; }
}

#endif // WITH_OUTLAWS_TESTS
