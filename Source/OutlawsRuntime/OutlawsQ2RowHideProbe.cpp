// REQ-Q2ROWHIDE's probe (user 2 Nov: the Graphics page's Quest 2 Mode row hidden in the release build only; it stays in the
// user's own builds):  -OLShots -OLLevel=town -OLNoFrontEnd -OLQ2RowHideProbe [-OLQ2RowHideOff] [-ResX=1832 -ResY=1920 -ForceRes]
// -OLQ2RowHideProbe makes OutlawsDeviceProfile::ShowQuest2ModeRow answer as a release build (OL_RELEASE_BUILD=1) does; with
// -OLQ2RowHideOff as this build does.  The rule (ShowQuest2ModeRowFor) both ways, then the Graphics page found under Options
// (the page with the Render Resolution row): the Quest 2 Mode row gone (there with -OLQ2RowHideOff), the rows around it kept,
// and the page and every page under it fit their panel at the top and scrolled to the bottom (MenuFitWalk).  Lines
// "OLSHOT q2rowhide check PASS|FAIL: ..." and "OLSHOT q2rowhide RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsDeviceProfile.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLQ2RowHideProbe) { return OLFlags::Param(TEXT("OLQ2RowHideProbe")); }

OL_SHOT_TEST(OLQ2RowHideProbe, 1730)
{
	static int32 Passes = 0, Fails = 0;
	static float Wait = 0.f;
	static FString Graphics;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT q2rowhide check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	const bool bOff = OLFlags::Param(TEXT("OLQ2RowHideOff"));
	auto Has = [this](const TCHAR* Name) { return MenuPage && MenuPage->Items.ContainsByPredicate([Name](const OutlawsMenu::FItem& I) { return I.Name == Name; }); };
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 3.f || !bUp) return;
		L->bInvincible = true;
		Check(!OutlawsDeviceProfile::IsReleaseBuild(), TEXT("this build is not a release build (OL_RELEASE_BUILD 0: the user's own builds)"));
		Check(!OutlawsDeviceProfile::ShowQuest2ModeRowFor(true), TEXT("a release build never shows the row"));
		Check(OutlawsDeviceProfile::ShowQuest2ModeRowFor(false), TEXT("a build that is not one shows it here (not a Quest 2)"));
		Check(OutlawsDeviceProfile::ShowQuest2ModeRow() == bOff, FString::Printf(TEXT("ShowQuest2ModeRow answers %s (%s)"), OutlawsDeviceProfile::ShowQuest2ModeRow() ? TEXT("shown") : TEXT("hidden"),
			bOff ? TEXT("-OLQ2RowHideOff: as this build") : TEXT("standing in for a release build")));
		// the Graphics page: the first page under Options with the Render Resolution row
		if (!bMenuOpen) OpenMenu();
		TArray<FString> Queue = { TEXT("options.rml") };
		TSet<FString> Seen = { TEXT("options.rml") };
		Graphics.Reset();
		while (Queue.Num() > 0 && Graphics.IsEmpty())
		{
			const FString File = Queue[0];
			Queue.RemoveAt(0);
			if (!ShowMenuPage(File, false) || !MenuPage) continue;
			if (Has(TEXT("r_resolution"))) { Graphics = File; break; }
			for (const OutlawsMenu::FItem& I : MenuPage->Items)
				if ((I.Kind == OutlawsMenu::EItemKind::Link || I.Kind == OutlawsMenu::EItemKind::Dialog) && !I.Target.IsEmpty() && !Seen.Contains(I.Target)) { Seen.Add(I.Target); Queue.Add(I.Target); }
		}
		Check(!Graphics.IsEmpty(), FString::Printf(TEXT("the Graphics page found under Options: %s"), Graphics.IsEmpty() ? TEXT("none") : *Graphics));
		if (Graphics.IsEmpty()) { ShotStep = 3; return; }
		ShowMenuPage(TEXT("options.rml"), false);
		ShowMenuPage(Graphics, true);
		RefreshMenuRows();
		TArray<FString> Rows;
		for (const OutlawsMenu::FItem& I : MenuPage->Items) Rows.Add(I.Label);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT q2rowhide %s rows (%d): %s"), *Graphics, Rows.Num(), *FString::Join(Rows, TEXT(" | ")));
		Check(Has(TEXT("r_quest2mode")) == bOff, FString::Printf(TEXT("the Quest 2 Mode row is %s (want %s)"), Has(TEXT("r_quest2mode")) ? TEXT("there") : TEXT("gone"), bOff ? TEXT("there") : TEXT("gone")));
		Check(Has(TEXT("r_objdetail")) && Has(TEXT("r_perfhud")) && Has(TEXT("r_grabglow")) && Has(TEXT("r_lamps")), TEXT("Lamp Lights, Object Detail, Performance HUD and Grab Glow kept"));
		Wait = ShotTime + 0.5f;
		ShotStep = 1;
		return;
	}
	case 1:
	{
		if (ShotTime < Wait) return;
		const FString Name = FString::Printf(TEXT("q2rowhide_%s"), bOff ? TEXT("off") : TEXT("release"));
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		Wait = ShotTime + 1.f;
		ShotStep = 2;
		return;
	}
	case 2:
	{
		if (ShotTime < Wait) return;
		const int32 N = MenuFitWalk(Graphics, Check);
		Check(N >= 1, FString::Printf(TEXT("%d pages walked from %s"), N, *Graphics));
		ShotStep = 3;
		return;
	}
	case 3:
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT q2rowhide RESULT %s (%d passed, %d failed%s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails, bOff ? TEXT(", -OLQ2RowHideOff") : TEXT(""));
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
