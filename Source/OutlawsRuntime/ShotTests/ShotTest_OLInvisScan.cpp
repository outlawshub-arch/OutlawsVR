// -OLShots -OLInvisScan: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLInvisScan) { return OLFlags::Param(TEXT("OLInvisScan")); }

OL_SHOT_TEST(OLInvisScan, 1010)
{
	// (BUG-INVISWALLS) where the body is stopped with nothing drawn, and the original walks on
	if (!L || ShotTime < 6.f) return;
	int32 Found = 0;
	const FString R = L->DebugInvisScan(this, Found);
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT invis scan: %d sweeps stopped where the original walks on: %s"), Found, *R);
	FGenericPlatformMisc::RequestExit(false);
	ShotTime = -1.f;
}

#endif // WITH_OUTLAWS_TESTS
