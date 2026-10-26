// -OLShots -OLLevelSkipTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLLevelSkipTest) { return OLFlags::Param(TEXT("OLLevelSkipTest")); }

OL_SHOT_TEST(OLLevelSkipTest, 430)
{
	// the level skip cheat: its list, then a jump to the second story level after this one
	if (ShotStep == 0 && ShotTime > 3.f)
	{
		TArray<FString> Names, Labels;
		L->GetLevelChoices(Names, Labels);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT %d levels: %s"), Names.Num(), *FString::Join(Labels, TEXT(", ")));
		L->JumpToLevel(Names.IsValidIndex(3) ? Names[3] : TEXT("train"));
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 9.f)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT now in %s, player at %s"), *L->LevelName, *GetActorLocation().ToString());
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
