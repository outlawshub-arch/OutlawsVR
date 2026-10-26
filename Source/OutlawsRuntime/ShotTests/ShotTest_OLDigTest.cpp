// -OLShots -OLDigTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLDigTest) { return OLFlags::Param(TEXT("OLDigTest")); }

OL_SHOT_TEST(OLDigTest, 530)
{
	// the shovel's dig on townex's DIG_ME without hands: not held digs nothing, then each stab a step down
	const float Due[] = { 3.f, 4.5f, 6.f, 7.f };
	if (ShotStep >= UE_ARRAY_COUNT(Due) || ShotTime < Due[ShotStep]) return;
	if (ShotStep < 3) { if (L) L->DebugDig(ShotStep); }
	else { FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; }
	++ShotStep;
}

#endif // WITH_OUTLAWS_TESTS
