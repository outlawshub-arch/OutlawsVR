// -OLShots -OLValveTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLValveTest) { return OLFlags::Param(TEXT("OLValveTest")); }

OL_SHOT_TEST(OLValveTest, 300)
{
	// the mill's valve wheels worked by hand: their events open the gates and start the water
	if (ShotStep == 0 && ShotTime > 3.f && L)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT valves: %d nudge props worked"), L->DebugNudgeAllProps());
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 9.f)
	{
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
