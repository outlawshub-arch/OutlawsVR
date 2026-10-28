// BUG-CHEATNOTICE (user 2026-10-28, snapshot 1: the hideout's pause > Cheats page, "Cheats unlocked" written over Back and its
// controllers): -OLShots -OLLevel=hideout -OLNoFrontEnd -OLCheatNoticeProbe.  On the flat screen, then laid out for VR (its
// controllers drawn under the prompts), each page a notice shows on: the Cheats page with "Cheats unlocked" and with the host's
// cheats off line, the Dev checks page with a check's long note, the pause page with "Cheats unlocked" (the turn on the spot
// shows it there) and New Game's page with "Performance test unlocked".  Each notice line clear of the foot line (Back, "more
// below" and their drawings), of every row and the title, and on the sheet; a shot of each, cheatnotice_<flat|vr>_<page>.png
// in Saved\OLShots.  Lines "OLSHOT cheatnotice check PASS|FAIL: ..." and "OLSHOT cheatnotice RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "OutlawsDevChecks.h"
#include "OutlawsMenuDoc.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLCheatNoticeProbe) { return OLFlags::Param(TEXT("OLCheatNoticeProbe")); }

OL_SHOT_TEST(OLCheatNoticeProbe, 1241)
{
	static int32 Passes = 0, Fails = 0, Scene = 0;
	static float Wait = 0.f;
	static bool bWasVR = false;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cheatnotice check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	// the scenes, each on the flat screen (0..4) then in VR (5..9)
	static const TCHAR* const Names[] = { TEXT("cheats"), TEXT("hostoff"), TEXT("devchecks"), TEXT("pause"), TEXT("newgame") };
	constexpr int32 NumScenes = UE_ARRAY_COUNT(Names);
	auto Over = [](const FBox2D& A, const FBox2D& B) { return A.bIsValid && B.bIsValid && A.Min.X < B.Max.X && B.Min.X < A.Max.X && A.Min.Y < B.Max.Y && B.Min.Y < A.Max.Y; };
	auto Box = [](const FBox2D& B) { return B.bIsValid ? FString::Printf(TEXT("x %.0f..%.0f y %.0f..%.0f"), B.Min.X, B.Max.X, B.Min.Y, B.Max.Y) : FString(TEXT("none")); };
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	switch (ShotStep)
	{
	case 0:
		if (ShotTime < 4.f || !bUp) return;
		L->bInvincible = true;
		bWasVR = bVR;
		Scene = 0;
		ShotStep = 1;
		return;
	case 1:
	{
		// ---- a scene set up ----
		if (Scene >= NumScenes * 2)
		{
			bVR = bWasVR;
			MenuToastText(FString());
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cheatnotice RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
			ShotStep = -1;
			FGenericPlatformMisc::RequestExit(false);
			return;
		}
		bVR = Scene >= NumScenes;
		const FString Name = Names[Scene % NumScenes];
		if (!bMenuOpen) OpenMenu();
		while (MenuStack.Num() > 0) MenuBack();
		bCheatsShown = true;
		ShowMenuPage(TEXT("main_game.rml"), false);
		FString Note = TEXT("Cheats unlocked");
		if (Name == TEXT("cheats") || Name == TEXT("hostoff")) ShowMenuPage(TEXT("#cheats"), true);
		if (Name == TEXT("hostoff")) Note = TEXT("The host has cheats off");
		if (Name == TEXT("devchecks"))
		{
			ShowMenuPage(TEXT("#devchecks"), true);
			Note.Reset();
			for (const OutlawsDevChecks::FCheck& C : OutlawsDevChecks::Checks()) if (C.Detail.Len() > Note.Len()) Note = C.Detail.Replace(TEXT("\n"), TEXT(" "));
			if (Note.IsEmpty()) Note = TEXT("Walk into the corner by the door and turn on the spot: the wall stays solid and the view does not jump through it");
		}
		if (Name == TEXT("newgame"))
		{
			bPerfTestShown = true;
			ShowMenuPage(TEXT("#newgame"), true);
			Note = TEXT("Performance test unlocked");
		}
		MenuToastText(Note);
		MenuToastLeft = 1.0e6f;          // (held while it is measured and shot)
		RefreshMenuRows();
		RedrawMenu();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cheatnotice scene %s %s: page %s (%d rows), notice \"%s\""), bVR ? TEXT("vr") : TEXT("flat"), *Name, MenuPage ? *MenuPage->File : TEXT("none"),
			MenuPage ? MenuPage->Items.Num() : 0, *Note);
		Wait = ShotTime + 1.2f;
		ShotStep = 2;
		return;
	}
	case 2:
	{
		// ---- measured and shot ----
		if (ShotTime < Wait) return;
		const FString Name = Names[Scene % NumScenes];
		const FString Tag = FString::Printf(TEXT("%s %s"), bVR ? TEXT("vr") : TEXT("flat"), *Name);
		FString Lines;
		for (const FBox2D& T : SheetToastInk) Lines += FString::Printf(TEXT(" [%s]"), *Box(T));
		Check(SheetToastInk.Num() > 0, FString::Printf(TEXT("%s: the notice is drawn:%s"), *Tag, *Lines));
		if (IsFrontPage())
		{
			// the wide page: clear of its rows and of the flat screen's help line (VR's is on its plate)
			FString Hit;
			for (const FBox2D& T : SheetToastInk)
			{
				for (int32 i = 0; i < FrontItemInk.Num(); ++i) if (Over(T, FrontItemInk[i])) Hit += FString::Printf(TEXT(" row %d [%s];"), i, *Box(FrontItemInk[i]));
				if (Over(T, WideHelpInk)) Hit += FString::Printf(TEXT(" the help line [%s];"), *Box(WideHelpInk));
				if (T.Min.Y < 0.f || T.Max.Y > 1080.f) Hit += TEXT(" off the view;");
			}
			FBox2D Last = FrontItemInk.Num() > 0 ? FrontItemInk.Last() : FBox2D(ForceInit);
			Check(Hit.IsEmpty(), FString::Printf(TEXT("%s: the notice clear of the rows (last %s) and the help line (%s)%s"), *Tag, *Box(Last), *Box(WideHelpInk), Hit.IsEmpty() ? TEXT("") : *(TEXT(":") + Hit)));
		}
		else
		{
			FString Hit;
			const float W = float(SheetCanvasSize.X), H = float(SheetCanvasSize.Y);
			FBox2D Foot(ForceInit);
			for (const FBox2D& F : SheetFootInk) Foot += F;
			for (const FBox2D& T : SheetToastInk)
			{
				for (const FBox2D& F : SheetFootInk) if (Over(T, F)) Hit += FString::Printf(TEXT(" the foot line [%s];"), *Box(F));
				for (int32 r = 0; r < SheetRowInk.Num(); ++r) if (Over(T, SheetRowInk[r])) Hit += FString::Printf(TEXT(" row %d [%s];"), r, *Box(SheetRowInk[r]));
				if (Over(T, SheetTitleInk)) Hit += TEXT(" the title;");
				if (Over(T, SheetMoreUpInk)) Hit += TEXT(" more above;");
				if (T.Min.X < 4.f || T.Max.X > W - 4.f || T.Min.Y < 4.f || T.Max.Y > H - 4.f) Hit += FString::Printf(TEXT(" off the %.0f x %.0f sheet;"), W, H);
			}
			const FBox2D Rows = SheetRowInk.Num() > 0 ? SheetRowInk.Last() : FBox2D(ForceInit);
			Check(SheetToastInk.Num() > 0 && Hit.IsEmpty(), FString::Printf(TEXT("%s: the notice clear of the foot line (%s, %d parts), the rows (last %s) and on the sheet%s"), *Tag, *Box(Foot), SheetFootInk.Num(),
				*Box(Rows), Hit.IsEmpty() ? TEXT("") : *(TEXT(":") + Hit)));
		}
		const FString File = FString::Printf(TEXT("cheatnotice_%s_%s"), bVR ? TEXT("vr") : TEXT("flat"), *Name);
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / File + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cheatnotice shot %s.png"), *File);
		Wait = ShotTime + 0.8f;
		ShotStep = 3;
		return;
	}
	case 3:
		if (ShotTime < Wait) return;
		++Scene;
		ShotStep = 1;
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
