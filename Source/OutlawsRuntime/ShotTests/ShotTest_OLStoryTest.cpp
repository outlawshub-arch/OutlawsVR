// -OLShots -OLStoryTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLStoryTest) { return OLFlags::Param(TEXT("OLStoryTest")); }

OL_SHOT_TEST(OLStoryTest, 770)
{
	// the title menu up for a while (its backdrop tone full), then Main Game the way the menu starts it (menu
	// closed, the story begun), and the story's first movie a few seconds in: ol_story_movie.png, no death tone
	// (real seconds and frames: the first frames after loading carry huge delta times)
	auto Shot = [](const TCHAR* Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(Name) + TEXT(".png"), false, false); };
	static double Mark = 0.0;
	static uint64 MarkFrame = 0;
	const double Now = FPlatformTime::Seconds();
	auto Waited = [&](double Sec, uint64 Frames) { return Now - Mark > Sec && GFrameCounter - MarkFrame > Frames; };
	auto Next = [&]() { Mark = Now; MarkFrame = GFrameCounter; ++ShotStep; };
	if (ShotStep == 0 && ShotTime > 4.f && bMenuOpen) Next();
	else if (ShotStep == 1 && Waited(2.0, 30)) { Shot(TEXT("ol_story_menu")); Next(); }
	else if (ShotStep == 2 && Waited(1.0, 10) && L)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT menu tone before start %.2f"), BackdropTone);
		CloseMenu();
		L->StartStory(EOutlawsDifficulty::Bad);
		Next();
	}
	else if (ShotStep == 3 && Waited(8.0, 120))
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT movie playing %d, tone %.2f, vignette %.2f"), L && L->IsMoviePlaying() ? 1 : 0, BackdropTone, Camera->PostProcessSettings.VignetteIntensity);
		Shot(TEXT("ol_story_movie"));
		Next();
	}
	else if (ShotStep == 4 && Waited(1.0, 10)) { FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; }
}

#endif // WITH_OUTLAWS_TESTS
