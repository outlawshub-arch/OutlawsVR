// -OLShots -OLSoundTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLSoundTest) { return OLFlags::Param(TEXT("OLSoundTest")); }

OL_SHOT_TEST(OLSoundTest, 500)
{
	// a taunt through the Remastered sounds (oltaunthd.lab, .ogg), then quit
	if (ShotStep == 0 && ShotTime > 3.f) { if (L) L->PlayMenuSound(TEXT("bm01")); ++ShotStep; }
	else if (ShotStep == 1 && ShotTime > 5.f) { FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; }
}

#endif // WITH_OUTLAWS_TESTS
