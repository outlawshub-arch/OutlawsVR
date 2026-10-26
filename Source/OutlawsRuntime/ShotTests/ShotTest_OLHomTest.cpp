// -OLShots -OLHomTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLHomTest) { return OLFlags::Param(TEXT("OLHomTest")) || OLFlags::Param(TEXT("OLHomDoorTest")); }

OL_SHOT_TEST(OLHomTest, 320)
{
	if (L) L->TickHomTest(ShotTime);
}

#endif // WITH_OUTLAWS_TESTS
