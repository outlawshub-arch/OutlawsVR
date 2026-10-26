// -OLShots -OLLookRound: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLLookRound) { return OLFlags::Param(TEXT("OLLookRound")); }

OL_SHOT_TEST(OLLookRound, 1020)
{
	// (BUG-WILDWALLS) at the start, looking round in four quarters: ol_round_<tag>_<n>.png (-OLShotTag=), the doors and
	// lifts hidden logged with each
	static FString Tag;
	if (ShotStep == 0) { if (!OLFlags::Value(TEXT("OLShotTag"), Tag)) Tag = TEXT("x"); }
	if (ShotStep < 8 && ShotTime > 5.f + ShotStep * 1.f)
	{
		const int32 k = ShotStep / 2;
		if (Controller) Controller->SetControlRotation(FRotator(0.0, k * 90.0, 0.0));
		if (ShotStep % 2 == 1)
		{
			FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("ol_round_%s_%d.png"), *Tag, k), false, false);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT look round %d (yaw %d): player sector %d, %d sectors drawn, %s"), k, k * 90, L ? L->PlayerSectorIndex() : -1, L ? L->DebugSectorsDrawn() : 0, *L->DebugGroupsHidden());
		}
		++ShotStep;
	}
	else if (ShotStep == 8 && ShotTime > 14.f)
	{
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
