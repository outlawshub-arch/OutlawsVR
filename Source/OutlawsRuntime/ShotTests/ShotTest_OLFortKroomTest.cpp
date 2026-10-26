// -OLShots -OLFortKroomTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLFortKroomTest) { return OLFlags::Param(TEXT("OLFortKroomTest")); }

OL_SHOT_TEST(OLFortKroomTest, 610)
{
	// (BUG-SIMMSINF) fort.inf's "ITEM: LINE NAME:KROOM NUM: #7D64" (no space after NAME:): the switch raises BAR1's bars
	if (ShotStep == 0 && ShotTime > 3.f && L)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT kroom before: %s"), *L->DebugElevatorReport(TEXT("BAR1")));
		const bool bT = L->DebugNudgeWall(TEXT("KROOM"), TEXT("7D64")) || L->DebugNudgeWall(TEXT("KROOM"), TEXT("#7D64"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT kroom switch pushed: taken %d (expect 1)"), bT ? 1 : 0);
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 8.f && L)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT kroom 5 s after (expect BAR1 on to stop 1): %s"), *L->DebugElevatorReport(TEXT("BAR1")));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT kroom test done"));
		FGenericPlatformMisc::RequestExit(false);
		++ShotStep;
	}
}

#endif // WITH_OUTLAWS_TESTS
