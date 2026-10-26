// -OLShots -OLBadgeHudTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLBadgeHudTest) { return OLFlags::Param(TEXT("OLBadgeHudTest")); }

OL_SHOT_TEST(OLBadgeHudTest, 1150)
{
	// the badge picked up: its message on the HUD, the spinning star above it (two shots, 0.3 s apart)
	auto Shot = [](const TCHAR* Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(Name) + TEXT(".png"), false, false); };
	static double Mark = 0.0;
	const double Now = FPlatformTime::Seconds();
	if (ShotStep == 0 && ShotTime > 3.f && L)
	{
		L->BadgeLeft = 30.f;
		L->Notify(TEXT("Sheriff's Badge"), 3.f);
		Mark = Now;
		++ShotStep;
	}
	else if (ShotStep == 1 && Now - Mark > 1.5) { Shot(TEXT("ol_badge_hud1")); Mark = Now; ++ShotStep; }
	else if (ShotStep == 2 && Now - Mark > 0.3) { Shot(TEXT("ol_badge_hud2")); Mark = Now; ++ShotStep; }
	else if (ShotStep == 3 && Now - Mark > 1.0) { FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; }
}

#endif // WITH_OUTLAWS_TESTS
