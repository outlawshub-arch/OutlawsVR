// -OLShots -OLAtxTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLAtxTest) { return OLFlags::Param(TEXT("OLAtxTest")); }

OL_SHOT_TEST(OLAtxTest, 670)
{
	// the looping animated textures (the train's wheels): their frames stepping on their own
	if (ShotStep < 4 && ShotTime > 3.f + ShotStep * 0.3f && L)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT atx at %.1f s: %s"), ShotTime, *L->DebugAtxReport());
		++ShotStep;
	}
	else if (ShotStep == 4 && ShotTime > 5.f)
	{
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
