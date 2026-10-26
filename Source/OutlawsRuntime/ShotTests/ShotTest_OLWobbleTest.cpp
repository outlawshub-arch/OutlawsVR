// -OLShots -OLWobbleTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLWobbleTest) { return OLFlags::Param(TEXT("OLWobbleTest")); }

OL_SHOT_TEST(OLWobbleTest, 1200)
{
	// a 3D barrel shot: it leans away along the shot and rocks back to stand where it stood (user 2026-09-29)
	static const float Times[] = { 3.f, 3.05f, 3.1f, 3.2f, 3.35f, 3.5f, 3.8f, 4.5f };
	if (L && ShotStep < 8 && ShotTime > Times[ShotStep])
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wobble t+%.2f: %s"), Times[ShotStep] - 3.f, *L->DebugKnockModelProp(ShotStep == 0));
		++ShotStep;
	}
	else if (ShotStep == 8)
	{
		FGenericPlatformMisc::RequestExit(false);
		ShotStep = 9;
	}
}

#endif // WITH_OUTLAWS_TESTS
