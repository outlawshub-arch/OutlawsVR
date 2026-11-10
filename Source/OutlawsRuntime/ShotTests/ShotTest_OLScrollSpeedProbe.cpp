// -OLShots -OLScrollSpeedProbe (REQ-SCROLLSPEED, user 3 Nov: the right stick speeds up or slows scrolling text).  From the main
// menu: Extras > Credits rolls (PlayExtrasCredits), the stick held as a player holds it (TestAxes, OLVR_RightY), the roll's own
// clock measured over a second each: let go x1, full up x4 (the stick pushed right too: nothing turns), full down x0 (stopped,
// not backwards), let go again x1, half up in proportion; the first one's hint shows.  Then the Marshal Training crawl
// (StartHom): let go x1, full up x4.  The credits' canvas saved a second apart at x1 and at x4 (Saved\OLShots\
// ol_scroll_<normal|fast>_<0|1>.png).  Lines "OLSHOT scrollspeed check <what>: PASS|FAIL", "OLSHOT scrollspeed RESULT <n> PASS,
// <m> FAIL".  -OLOldScrollSpeed (always x1) must FAIL.

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS
#include "OutlawsMovieScreen.h"

OL_SHOT_MATCH(OLScrollSpeedProbe) { return OLFlags::Param(TEXT("OLScrollSpeedProbe")); }

OL_SHOT_TEST(OLScrollSpeedProbe, 2671)
{
	static int32 Pass = 0, Fail = 0;
	static float Mark = 0.f, AgeMark = 0.f, YawMark = 0.f;
	if (!L || !L->GetLabAssets()) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT scrollspeed check %s: %s"), *What, bOk ? TEXT("PASS") : TEXT("FAIL"));
		++(bOk ? Pass : Fail);
	};
	auto Finish = [&]()
	{
		TestAxes.Remove(TEXT("OLVR_RightY"));
		TestAxes.Remove(TEXT("OLVR_RightX"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT scrollspeed RESULT %d PASS, %d FAIL"), Pass, Fail);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	};
	AOutlawsMovieScreen* M = L->GetMovieScreen();
	const bool bCrawlPart = ShotStep >= 20;
	auto ClockNow = [&]() { return M ? (bCrawlPart ? M->CrawlAgeNow() : M->CreditsAgeNow()) : 0.f; };
	auto Start = [&]() { Mark = ShotTime; AgeMark = ClockNow(); };
	auto Rate = [&]() { return (ClockNow() - AgeMark) / FMath::Max(0.01f, ShotTime - Mark); };
	auto Stick = [&](float Y, float X = 0.f)
	{
		if (Y == 0.f) TestAxes.Remove(TEXT("OLVR_RightY")); else TestAxes.Add(TEXT("OLVR_RightY"), Y);
		if (X == 0.f) TestAxes.Remove(TEXT("OLVR_RightX")); else TestAxes.Add(TEXT("OLVR_RightX"), X);
	};
	auto Canvas = [&](const TCHAR* Name)
	{
		const bool bOk = M && M->SaveCrawlPng(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("ol_scroll_%s.png"), Name));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT scrollspeed canvas %s (%d), roll at %.2f s"), Name, bOk ? 1 : 0, ClockNow());
	};
	const float Wait = 1.f;
	switch (ShotStep)
	{
	case 0:
		if (ShotTime < 4.f || !bMenuOpen) return;
		Check(L->PlayExtrasCredits(), TEXT("Extras > Credits rolls"));
		Mark = ShotTime;
		++ShotStep;
		return;
	case 1:          // (the roll under way, its canvas drawn)
		if (ShotTime - Mark < 2.5f) return;
		Check(M && M->IsScrollingText() && M->IsCreditsRolling(), TEXT("the credits are scrolling text"));
		if (!M) { Finish(); return; }
		Check(M->ScrollHintShown() || AOutlawsMovieScreen::OldScrollSpeed(), TEXT("the first scrolling text shows the speed hint"));
		Stick(0.f);
		Start();
		Canvas(TEXT("normal_0"));
		++ShotStep;
		return;
	case 2:
		if (ShotTime - Mark < Wait) return;
		Canvas(TEXT("normal_1"));
		Check(FMath::IsNearlyEqual(Rate(), 1.f, 0.1f), FString::Printf(TEXT("the stick let go: the credits at x%.2f (x1 wanted)"), Rate()));
		Stick(1.f, 1.f);
		YawMark = float(GetControlRotation().Yaw);
		Mark = ShotTime;
		++ShotStep;
		return;
	case 3:
		if (ShotTime - Mark < 0.2f) return;
		Start();
		Canvas(TEXT("fast_0"));
		++ShotStep;
		return;
	case 4:
		if (ShotTime - Mark < Wait) return;
		Canvas(TEXT("fast_1"));
		Check(FMath::IsNearlyEqual(Rate(), 4.f, 0.3f), FString::Printf(TEXT("the stick full up: the credits at x%.2f (x4 wanted)"), Rate()));
		Check(FMath::Abs(FRotator::NormalizeAxis(float(GetControlRotation().Yaw) - YawMark)) < 0.5f,
			FString::Printf(TEXT("the stick pushed up and right turns nothing over the text (yaw moved %.2f deg)"), FRotator::NormalizeAxis(float(GetControlRotation().Yaw) - YawMark)));
		Stick(-1.f);
		Mark = ShotTime;
		++ShotStep;
		return;
	case 5:
		if (ShotTime - Mark < 0.2f) return;
		Start();
		++ShotStep;
		return;
	case 6:
		if (ShotTime - Mark < Wait) return;
		Check(Rate() >= 0.f && Rate() < 0.05f, FString::Printf(TEXT("the stick full down: the credits at x%.2f (stopped, not backwards)"), Rate()));
		Stick(0.f);
		Mark = ShotTime;
		++ShotStep;
		return;
	case 7:
		if (ShotTime - Mark < 0.2f) return;
		Start();
		++ShotStep;
		return;
	case 8:
		if (ShotTime - Mark < Wait) return;
		Check(FMath::IsNearlyEqual(Rate(), 1.f, 0.1f), FString::Printf(TEXT("let go again: the credits at x%.2f (x1 wanted)"), Rate()));
		Stick(0.5f);
		Mark = ShotTime;
		++ShotStep;
		return;
	case 9:
		if (ShotTime - Mark < 0.2f) return;
		Start();
		++ShotStep;
		return;
	case 10:
	{
		if (ShotTime - Mark < Wait) return;
		const float Want = 1.f + 3.f * (0.5f - 0.15f) / 0.85f;
		Check(FMath::IsNearlyEqual(Rate(), Want, 0.25f), FString::Printf(TEXT("the stick half up: the credits at x%.2f (x%.2f wanted)"), Rate(), Want));
		Stick(0.f);
		L->SkipMovie();
		Mark = ShotTime;
		ShotStep = 19;
		return;
	}
	case 19:         // (back on the Extras page) the Marshal Training crawl, as the menu starts it
		if (ShotTime - Mark < 2.f) return;
		if (bMenuOpen) CloseGameMenu();
		L->StartHom(EOutlawsDifficulty::Bad);
		Mark = ShotTime;
		ShotStep = 20;
		return;
	case 20:
		if (ShotTime - Mark < 3.f) return;
		Check(M && M->IsCrawl() && M->IsScrollingText(), TEXT("the Marshal Training crawl is scrolling text"));
		if (!M || !M->IsCrawl()) { Finish(); return; }
		Start();
		++ShotStep;
		return;
	case 21:
		if (ShotTime - Mark < Wait) return;
		Check(FMath::IsNearlyEqual(Rate(), 1.f, 0.1f), FString::Printf(TEXT("the crawl, the stick let go: x%.2f (x1 wanted)"), Rate()));
		Stick(1.f);
		Mark = ShotTime;
		++ShotStep;
		return;
	case 22:
		if (ShotTime - Mark < 0.2f) return;
		Start();
		++ShotStep;
		return;
	case 23:
		if (ShotTime - Mark < Wait) return;
		Check(FMath::IsNearlyEqual(Rate(), 4.f, 0.3f), FString::Printf(TEXT("the crawl, the stick full up: x%.2f (x4 wanted)"), Rate()));
		Finish();
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
