// -OLShots -OLVisScan: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLVisScan) { return OLFlags::Param(TEXT("OLVisScan")); }

OL_SHOT_TEST(OLVisScan, 1000)
{
	// (BUG-POPIN) bad guys in clear view the flood leaves undrawn, from eyes over the whole level, and why
	if (!L || ShotTime < 6.f) return;
	int32 False = 0, Pairs = 0;
	const double T0 = FPlatformTime::Seconds();
	const FString R = L->DebugVisScan(this, False, Pairs);
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT vis scan: %d eye-to-bad-guy sightings culled in clear view, %d bad guys (%.1f s): %s"), False, Pairs, FPlatformTime::Seconds() - T0, *R);
	FGenericPlatformMisc::RequestExit(false);
	ShotTime = -1.f;
}

#endif // WITH_OUTLAWS_TESTS
