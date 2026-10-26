// -OLShots -OLWristTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLWristTest) { return OLFlags::Param(TEXT("OLWristTest")); }

OL_SHOT_TEST(OLWristTest, 1090)
{
	// the wrist panel with something on it: health and armour (the bag, gold and keys given are not shown)
	if (ShotStep == 0 && ShotTime > 3.f && L)
	{
		L->AddCount(TEXT("ibplate"), 50);
		L->AddCount(TEXT("idocbag"), 3);
		L->AddCount(TEXT("igbar"), 2);
		L->GiveItem(101);
		L->DebugGiveListed(TEXT("ibrsskey"), TEXT("Brass Key"));
		L->DebugGiveListed(TEXT("iironkey"), TEXT("Iron Key"));
		L->bInvincible = false; L->DamagePlayer(3.f, 1.f, true); L->bInvincible = true;
		WristLast.Reset();
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 4.5f) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_wrist.png"), false, false); ++ShotStep; }
	else if (ShotStep == 2 && ShotTime > 5.5f) { FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; }
}

#endif // WITH_OUTLAWS_TESTS
