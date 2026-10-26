// -OLShots -OLOpeningTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLOpeningTest) { return OLFlags::Param(TEXT("OLOpeningTest")); }

OL_SHOT_TEST(OLOpeningTest, 370)
{
	// a level won (user 2026-10-02): the autosave becomes the next level's opening; loaded, that level starts from its
	// beginning with what was carried
	static int32 Step = 0;
	AOutlawsLevelActor* OL = Level.Get();
	if (!OL || ShotTime < 3.f) return;
	if (Step == 0)
	{
		OL->AddCount(TEXT("ishells"), 17);
		OL->DebugDumpCarry(TEXT("before the win"));
		OL->CompleteLevel(TEXT("opening test"));
		const AOutlawsLevelActor::FSaveInfo I = AOutlawsLevelActor::ReadSaveInfo(-1);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT opening: the autosave now names %s (between levels %d)"), *I.Level, OL->IsBetweenLevels() ? 1 : 0);
		Step = 1;
		ShotTime = 3.f;
	}
	else if (Step == 1 && ShotTime > 3.5f)
	{
		const bool bOk = OL->LoadGame(-1);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT opening: load %s"), bOk ? TEXT("started") : TEXT("FAILED"));
		Step = 2;
		ShotTime = 3.f;
	}
	else if (Step == 2 && ShotTime > 12.f)
	{
		OL->DebugDumpCarry(TEXT("after the load"));
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
