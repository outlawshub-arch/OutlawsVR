// REQ-REFRESHHINT (user 2 Nov: the Quest's power saving held the display at 72 while every ask of 90 and 120 came back
// XR_SUCCESS, with no change event): -OLShots -OLLevel=hideout -OLNoFrontEnd -OLRefreshHintProbe [-OLOldRefreshHint], on
// the desktop with OutlawsXRRefresh's stand-in runtime (rates 72 80 90 120, every ask succeeds, the display stays at 72).
// (a) an ask of 72: no flag 10 s on; (b) an ask of 90: no flag at 6 s, the flag at about 9 s (Maintain's 3rd retry), the
// Graphics page's note right under the Refresh Rate row, the page's fit checked and photographed (OLShots/refreshhint_page.png);
// (c) the PC's gate never shows it; (d) a change event clears the flag and the page drops the note; (e) set again, an ask of
// 72 clears it.  -OLOldRefreshHint: the flag as before, never a note.  Lines "OLSHOT refreshhint check PASS|FAIL: ..." and
// "OLSHOT refreshhint RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "OutlawsDeviceProfile.h"
#include "OutlawsXRRefresh.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLRefreshHintProbe) { return OLFlags::Param(TEXT("OLRefreshHintProbe")); }

OL_SHOT_TEST(OLRefreshHintProbe, 1520)
{
	static int32 Passes = 0, Fails = 0;
	static double T0 = 0.0, Wait = 0.0;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT refreshhint check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	auto Find = [this](const TCHAR* Name) { return MenuPage.IsValid() ? MenuPage->Items.IndexOfByPredicate([Name](const OutlawsMenu::FItem& It) { return It.Name == Name; }) : int32(INDEX_NONE); };
	const bool bOld = OLFlags::Param(TEXT("OLOldRefreshHint"));
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	const double Now = FPlatformTime::Seconds();
	switch (ShotStep)
	{
	case 0:
		if (ShotTime < 4.f || !bUp) return;
		L->bInvincible = true;
		OutlawsXRRefresh::TestFakeRuntime(true);
		Check(OutlawsXRRefresh::IsAvailable() && !OutlawsXRRefresh::Declined(), FString::Printf(TEXT("the stand-in runtime up (offered %d rates, Refresh Rate row's %d), no flag"),
			OutlawsXRRefresh::SupportedRates().Num(), OutlawsDeviceProfile::RefreshRates().Num()));
		OutlawsXRRefresh::Request(72.f);
		T0 = Now;
		ShotStep = 1;
		return;
	case 1:
		if (Now - T0 < 10.5) return;
		Check(!OutlawsXRRefresh::Declined(), FString::Printf(TEXT("(a) an ask of 72, 10 s on: no flag (%s)"), *OutlawsXRRefresh::StatusText()));
		OutlawsXRRefresh::Request(90.f);
		T0 = Now;
		ShotStep = 2;
		return;
	case 2:
		if (Now - T0 < 6.0) return;
		Check(!OutlawsXRRefresh::Declined(), FString::Printf(TEXT("(b) an ask of 90, 6 s on: no flag yet (%s)"), *OutlawsXRRefresh::StatusText()));
		ShotStep = 3;
		return;
	case 3:
	{
		const bool bSet = OutlawsXRRefresh::Declined();
		if (!bSet && Now - T0 < 14.0) return;
		FString Why;
		OutlawsXRRefresh::Declined(&Why);
		Check(bSet && Now - T0 >= 8.5 && Now - T0 <= 11.0, FString::Printf(TEXT("(b) the flag set %.1f s after the ask (%s)"), Now - T0, *Why));
		Check(OutlawsDeviceProfile::RefreshHint().IsEmpty() == bOld, FString::Printf(TEXT("(b) the note's text %s"), bOld ? TEXT("empty (-OLOldRefreshHint)") : *(TEXT("'") + OutlawsDeviceProfile::RefreshHint().Replace(TEXT("\n"), TEXT(" / ")) + TEXT("'"))));
		Check(!OutlawsDeviceProfile::RefreshHintFor(false) && OutlawsDeviceProfile::RefreshHintFor(true) == !bOld,
			FString::Printf(TEXT("(c) the gate with the flag set: the PC %d (never), the Quest %d"), OutlawsDeviceProfile::RefreshHintFor(false) ? 1 : 0, OutlawsDeviceProfile::RefreshHintFor(true) ? 1 : 0));
		CoopOpenMenu();
		CoopShowPage(TEXT("options.rml"), true);
		CoopShowPage(TEXT("options_display.rml"), true);
		const int32 Rf = Find(TEXT("r_refresh")), N = Find(TEXT("r_refreshhint"));
		TArray<FString> Rows;
		if (MenuPage.IsValid()) for (int32 i = 0; i < MenuPage->Items.Num(); ++i) Rows.Add(MenuRowText(i).Replace(TEXT("\n"), TEXT(" / ")));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT refreshhint page %s: %s"), MenuPage.IsValid() ? *MenuPage->File : TEXT("?"), *FString::Join(Rows, TEXT(" | ")));
		Check(Rf != INDEX_NONE && (bOld ? N == INDEX_NONE : N == Rf + 1), FString::Printf(TEXT("(b) the Graphics page: Refresh Rate row %d, the note %s"), Rf,
			N == INDEX_NONE ? TEXT("not there") : *FString::Printf(TEXT("row %d"), N)));
		// (the note on the sheet: the page scrolled to put the Refresh Rate row near its top)
		if (Rf != INDEX_NONE)
		{
			MenuScroll = FMath::Clamp(Rf - 1, 0, SheetMaxScroll());
			RefreshMenuRows();
		}
		Wait = Now + 0.6;
		ShotStep = 4;
		return;
	}
	case 4:
	{
		if (Now < Wait) return;
		FString Fit;
		const bool bFits = MenuPageFits(Fit);
		Check(bFits, FString::Printf(TEXT("(b) the page fits with the note (scroll %d): %s"), MenuScroll, *Fit));
		const FString Path = FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(bOld ? TEXT("refreshhint_page_old") : TEXT("refreshhint_page")) + TEXT(".png");
		FScreenshotRequest::RequestScreenshot(Path, false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT refreshhint screenshot %s"), *Path);
		Wait = Now + 0.4;
		ShotStep = 5;
		return;
	}
	case 5:
		if (Now < Wait) return;
		OutlawsXRRefresh::TestFakeChange(72.f, 90.f);
		Check(!OutlawsXRRefresh::Declined(), TEXT("(d) a change event 72 -> 90 Hz clears the flag"));
		Wait = Now + 0.4;
		ShotStep = 6;
		return;
	case 6:
		if (Now < Wait) return;
		Check(Find(TEXT("r_refresh")) != INDEX_NONE && Find(TEXT("r_refreshhint")) == INDEX_NONE && OutlawsDeviceProfile::RefreshHint().IsEmpty(), TEXT("(d) the page built again without the note"));
		CloseMenu();
		// (e) the display back at 72 (its own event), then 90 asked again
		OutlawsXRRefresh::TestFakeChange(90.f, 72.f);
		OutlawsXRRefresh::Request(90.f);
		T0 = Now;
		ShotStep = 7;
		return;
	case 7:
	{
		const bool bSet = OutlawsXRRefresh::Declined();
		if (!bSet && Now - T0 < 14.0) return;
		Check(bSet, FString::Printf(TEXT("(e) 90 asked again: the flag set again %.1f s on"), Now - T0));
		OutlawsXRRefresh::Request(72.f);
		FString Why;
		const bool bStill = OutlawsXRRefresh::Declined(&Why);
		Check(!bStill, FString::Printf(TEXT("(e) an ask of 72 clears it (%s)"), *Why));
		OutlawsXRRefresh::TestFakeRuntime(false);
		Wait = Now + 0.5;
		ShotStep = 8;
		return;
	}
	case 8:
		if (Now < Wait) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT refreshhint RESULT %s (%d passed, %d failed)%s"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			bOld ? TEXT(" -OLOldRefreshHint") : TEXT(""));
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
