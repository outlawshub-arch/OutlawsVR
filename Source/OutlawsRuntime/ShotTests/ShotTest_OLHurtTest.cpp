// -OLShots -OLHurtTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLHurtTest) { return OLFlags::Param(TEXT("OLHurtTest")); }

OL_SHOT_TEST(OLHurtTest, 490)
{
	// a hit from the right: the right rim should redden
	if (ShotStep == 0 && ShotTime > 3.f)
	{
		if (L) { L->bInvincible = false; L->SetHurtSource(Camera->GetComponentLocation() + Camera->GetRightVector() * 500.0); L->DamagePlayer(1.f); L->bInvincible = true; }
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 3.15f) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_hurt.png"), false, false); ++ShotStep; }
	else if (ShotStep == 2 && ShotTime > 4.f) { FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; }
}

#endif // WITH_OUTLAWS_TESTS
