// -OLShots -OLMovieTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLMovieTest) { return OLFlags::Param(TEXT("OLMovieTest")); }

OL_SHOT_TEST(OLMovieTest, 1220)
{
	// the level's end: the boss summoned and shot, the movie on its screen, skipped, the next level
	auto Shot = [](const TCHAR* Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(Name) + TEXT(".png"), false, false); };
	const float Due[] = { 3.f, 6.f, 16.f, 22.f, 23.f, 30.f, 31.f };
	if (ShotStep >= UE_ARRAY_COUNT(Due) || ShotTime < Due[ShotStep]) return;
	AOutlawsEnemy* B = L->GetBoss();
	switch (ShotStep)
	{
	case 0: L->DebugSummonBoss(); break;
	case 1:
		for (int32 i = 0; B && i < 20 && !B->IsDead(); ++i) B->TakeHit(4.f, B->GetActorLocation(), FVector(1.0, 0.0, 0.0), B->GetActorLocation() - FVector(100.0, 0.0, 0.0));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT boss dead %d"), B && B->IsDead() ? 1 : 0);
		break;
	case 2: UE_LOG(LogOutlaws, Display, TEXT("OLSHOT movie playing %d"), L->IsMoviePlaying() ? 1 : 0); Shot(TEXT("ol_movie1")); break;
	case 3:
		// -OLMoviePause: the pause menu over the movie (user 2026-10-01), shot behind it, then closed again
		if (OLFlags::Param(TEXT("OLMoviePause"))) { PressMenu(); UE_LOG(LogOutlaws, Display, TEXT("OLSHOT menu over the movie: open %d"), bMenuOpen ? 1 : 0); }
		else { L->SetMovieSkipHold(1.6f, 3.f); Shot(TEXT("ol_movie2")); }
		break;
	case 4:
		if (OLFlags::Param(TEXT("OLMoviePause"))) { Shot(TEXT("ol_movie_paused")); }
		else L->SkipMovie();
		break;
	case 5:
		if (OLFlags::Param(TEXT("OLMoviePause"))) { if (bMenuOpen) PressMenu(); UE_LOG(LogOutlaws, Display, TEXT("OLSHOT menu closed: movie %d"), L->IsMoviePlaying() ? 1 : 0); break; }
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT after skip: level %s, movie %d"), *L->LevelName, L->IsMoviePlaying() ? 1 : 0); Shot(TEXT("ol_next")); break;
	default: FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; break;
	}
	++ShotStep;
}

#endif // WITH_OUTLAWS_TESTS
