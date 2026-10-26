// -OLShots -OLLevel=hideout -OLNoFrontEnd -OLBackClickTest (REQ-BACKCLICK): the Back prompt under a menu page points and pulls as
// a row does.  On each page (Options under the pause page, an Options page, one paged down, the save and load screens, a co-op
// title-style page) the right hand's laser is first laid on the sheet away from Back (Back must stay unlit), then on Back's middle
// (it must light, and the shot bc_<page>_hover.png is taken), then the trigger is pulled: the page one level up must show, as the
// B / Y button gives.  The menus' drawing height is logged against the death screen's (three quarters).  Prints
// "OLSHOT backclick check PASS|FAIL: ..." and "OLSHOT backclick RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsGlyphs.h"
#include "OutlawsLevelActor.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsRuntime.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/Controller.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "MotionControllerComponent.h"
#include "UnrealClient.h"
#include "OutlawsShotTests.h"

#if WITH_OUTLAWS_TESTS

namespace
{
	struct FBackCase { const TCHAR* Name; TArray<FString> Push; int32 Scroll; const TCHAR* Want; };
	const TArray<FBackCase>& BackCases()
	{
		static const TArray<FBackCase> Cases = {
			{ TEXT("options"), { TEXT("options.rml") }, 0, TEXT("main_game.rml") },
			{ TEXT("vr"), { TEXT("options.rml"), TEXT("#vr") }, 0, TEXT("options.rml") },
			{ TEXT("display_paged"), { TEXT("options.rml"), TEXT("options_display.rml") }, 1, TEXT("options.rml") },
			{ TEXT("save"), { TEXT("save_game.rml") }, 0, TEXT("main_game.rml") },
			{ TEXT("load"), { TEXT("load_game.rml") }, 0, TEXT("main_game.rml") },
			{ TEXT("coop"), { TEXT("#coop") }, 0, TEXT("main_game.rml") },
		};
		return Cases;
	}
}

void AOutlawsPlayer::BackClickTestTick(float Dt)
{
	static int32 Case = -1, Phase = 0, Pass = 0, Fail = 0, Depth = 0;
	static double Next = 0.0;
	AOutlawsLevelActor* L = Level.Get();
	const double Now = FPlatformTime::Seconds();
	if (Case == -3 || !L || L->IsFrontEnd() || ShotTime < 6.f || !Camera) return;
	auto Check = [](bool bOk, const FString& What)
	{
		(bOk ? Pass : Fail)++;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT backclick check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
	};
	if (bMenuOpen && Controller) Controller->SetControlRotation(FRotator(0.0, MenuYaw, 0.0));
	const TArray<FBackCase>& All = BackCases();

	// the point the right hand aims at this phase, in the menu root's space (X = 0 is the sheet's plane)
	auto BackLocal = [this](FVector& Out) -> bool
	{
		if (MenuHelpPlate && MenuHelpPlate->IsVisible() && (bWideSheet || IsFrontPage() || IsSavePage()))
		{
			// (BUG-MENUGLYPH2: Back on a row of its own under Select)
			const TArray<FOutlawsGlyphRow>& Rows = MenuHelpPlate->GetRows();
			int32 r = 0, k = INDEX_NONE;
			for (; r < Rows.Num() && k == INDEX_NONE; ++r) k = Rows[r].More.IndexOfByPredicate([](const OutlawsGlyphs::FPart& P) { return P.Label == TEXT("Back"); });
			FVector2D Uv;
			if (k == INDEX_NONE || !MenuHelpPlate->PartCentreUv(r - 1, k + 1, Uv)) return false;
			const FVector At = MenuHelpPlate->GetRelativeLocation();
			Out = FVector(0.0, At.Y + (0.5 - Uv.X) * MenuHelpPlate->WidthCmNow(), At.Z + (0.5 - Uv.Y) * MenuHelpPlate->HeightCm());
			return true;
		}
		if (!SheetBackInk.bIsValid || SheetCanvasSize.X <= 0) return false;
		const double S = SheetCanvasSize.X / SheetW;       // (a sheet page's SheetW is its PanelW)
		const FVector2D C = SheetBackInk.GetCenter();
		Out = FVector(0.0, (0.5 - C.X / SheetCanvasSize.X) * SheetW, PageSheetH() * 0.5 - C.Y / S);
		return true;
	};
	auto AimAt = [this](const FVector& Local)
	{
		const FVector Target = MenuRoot->GetComponentTransform().TransformPosition(Local);
		const FVector From = Camera->GetComponentLocation() + Camera->GetRightVector() * 15.0 - FVector(0.0, 0.0, 20.0);
		if (UMotionControllerComponent* R = AimOf(1)) R->SetWorldLocationAndRotation(From, (Target - From).Rotation());
		// the left hand points at the floor, at nothing
		if (UMotionControllerComponent* Lh = AimOf(0)) Lh->SetWorldLocationAndRotation(From - Camera->GetRightVector() * 30.0, FRotator(-80.0, Camera->GetComponentRotation().Yaw, 0.0));
	};

	if (Case == -1)
	{
		bVR = true;                      // (the lasers and the drawn controllers are VR's; this run only)
		if (!bMenuOpen) OpenMenu();
		const float Menu = MenuGlyphCm(), Seen = MenuGlyphSeenCm();
		Check(FMath::Abs(Menu / Seen - 0.75f) < 0.01f, FString::Printf(TEXT("the menus' drawings %.2f sheet cm, the death screen's seen size %.2f: %.0f %%; the death plate keeps %.2f cm at 140 cm"),
			Menu, Seen, Menu / Seen * 100.f, OutlawsGlyphs::ViewTan * 140.f));
		Case = 0;
		Phase = 0;
		Next = Now + 1.0;
		return;
	}
	if (Case >= All.Num())
	{
		TestAxes.Remove(TEXT("OLVR_RightTrigger"));
		if (bMenuOpen) { while (MenuStack.Num() > 0) MenuBack(); CloseMenu(); }
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT backclick RESULT %s (%d passed, %d failed)"), Fail == 0 && Pass > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Pass, Fail);
		Case = -3;
		FGenericPlatformMisc::RequestExit(false);
		return;
	}
	const FBackCase& C = All[Case];
	FVector Back;
	const bool bBack = BackLocal(Back);
	if (Phase == 1 && bMenuOpen && MenuRoot)
	{
		// away from Back: the sheet's upper left corner (no row there)
		const float W = SheetW, H = bWideSheet ? SheetH : PageSheetH();
		AimAt(FVector(0.0, W * 0.42, H * 0.44));
	}
	else if ((Phase == 2 || Phase == 3) && bBack && MenuRoot) AimAt(Back);
	if (Now < Next) return;

	if (Phase == 0)
	{
		TestAxes.Remove(TEXT("OLVR_RightTrigger"));
		if (!bMenuOpen) OpenMenu();
		while (MenuStack.Num() > 0) MenuBack();
		for (const FString& P : C.Push) ShowMenuPage(P, true);
		if (C.Scroll > 0) { MenuScroll = FMath::Min(C.Scroll, SheetMaxScroll()); RefreshMenuRows(); }
		RedrawMenu();
		Depth = MenuStack.Num();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT backclick case %d %s: on %s (scroll %d, %d deep)"), Case + 1, C.Name, MenuPage ? *MenuPage->File : TEXT("?"), MenuScroll, Depth);
		Phase = 1;
		Next = Now + 0.8;
		return;
	}
	if (Phase == 1)
	{
		Check(!IsMenuBackHot() && MenuPage && MenuPage->File == C.Push.Last(), FString::Printf(TEXT("%s: the laser off Back leaves it unlit (lit %d, page %s)"), C.Name, IsMenuBackHot() ? 1 : 0, MenuPage ? *MenuPage->File : TEXT("?")));
		Phase = 2;
		Next = Now + 0.5;
		return;
	}
	if (Phase == 2)
	{
		float Len = 0.f;
		const FVector O = AimOf(1)->GetComponentLocation(), D = AimOf(1)->GetForwardVector();
		const bool bRay = MenuBackUnderRay(O, D, Len);
		Check(bBack && bRay && MenuBackHover[1] && !MenuBackHover[0], FString::Printf(TEXT("%s: the laser on Back lights it (found %d, ray %d, %.0f cm out; right %d, left %d)%s"), C.Name, bBack ? 1 : 0, bRay ? 1 : 0, Len,
			MenuBackHover[1] ? 1 : 0, MenuBackHover[0] ? 1 : 0, MenuHelpPlate && MenuHelpPlate->IsVisible() ? TEXT(" on the help plate") : TEXT(" on the sheet")));
		if (MenuHelpPlate && MenuHelpPlate->IsVisible())
		{
			bool bLit = false;
			for (const FOutlawsGlyphRow& Rw : MenuHelpPlate->GetRows()) for (const OutlawsGlyphs::FPart& Pt : Rw.More) bLit |= Pt.Label == TEXT("Back") && Pt.bHot;
			Check(bLit, FString::Printf(TEXT("%s: the plate's Back drawn lit"), C.Name));
		}
		// (REQ-BACKRIGHT) Back's controllers to the right of the word, on its line: the hit box (the part as drawn) spans both,
		// no taller than a drawing; the line (Back, or Back with its neighbours) centred on the page
		{
			FBox2D Box(ForceInit), Line(ForceInit);
			OutlawsGlyphs::FGlyph G;
			float GH = 0.f, CmPerPx = 0.f, Mid = 0.f;
			const bool bPlate = MenuHelpPlate && MenuHelpPlate->IsVisible() && (bWideSheet || IsFrontPage() || IsSavePage());
			if (bPlate)
			{
				// (BUG-MENUGLYPH2) Back's row under Select's in the sheet's corner: centred on the prompts' block, as Select's is
				const TArray<FOutlawsGlyphRow>& Rows = MenuHelpPlate->GetRows();
				for (int32 r = 0; r < Rows.Num(); ++r)
				{
					const int32 k = Rows[r].More.IndexOfByPredicate([](const OutlawsGlyphs::FPart& P) { return P.Label == TEXT("Back"); });
					if (k == INDEX_NONE) continue;
					Box = MenuHelpPlate->PartBox(r, k + 1);
					G = Rows[r].More[k].Glyph;
					Line = MenuHelpPlate->RowBox(r);
					break;
				}
				GH = MenuHelpPlate->GlyphPx();
				CmPerPx = MenuHelpPlate->WidthCmNow() / MenuHelpPlate->PixWide();
				Mid = MenuHelpPlate->InkBox().bIsValid ? float(MenuHelpPlate->InkBox().GetCenter().X) : MenuHelpPlate->PixWide() * 0.5f;
			}
			else if (!bPlate && SheetCanvasSize.X > 0)
			{
				TArray<OutlawsGlyphs::FPart> Parts;
				if (GlyphLineParts(TEXT("B or Y: back"), Parts)) G = Parts[0].Glyph;
				Box = SheetBackInk;
				for (const FBox2D& F : SheetFootInk) Line += F;
				CmPerPx = SheetW / SheetCanvasSize.X;
				GH = MenuGlyphCm() / CmPerPx;
				Mid = SheetCanvasSize.X * 0.5f;
			}
			const float GW = OutlawsGlyphs::Width(G, GH);
			const bool bBeside = Box.bIsValid && GH > 0.f && Box.GetSize().Y < GH * (1.f + OutlawsGlyphs::StackGap) && Box.GetSize().X > GW + GH * 0.15f + 8.f;
			const float Off = Line.bIsValid ? (float(Line.GetCenter().X) - Mid) * CmPerPx : 99.f;
			const float BackOff = Box.bIsValid ? (float(Box.GetCenter().X) - Mid) * CmPerPx : 99.f;
			Check(bBeside, FString::Printf(TEXT("%s: Back's hit box %.0f x %.0f px holds the word (%.0f px) and its controllers beside it (%.0f x %.0f px)"), C.Name,
				Box.bIsValid ? Box.GetSize().X : 0.0, Box.bIsValid ? Box.GetSize().Y : 0.0, Box.bIsValid ? float(Box.GetSize().X) - GW - GH * 0.15f : 0.f, GW, GH));
			Check(FMath::Abs(Off) < 0.1f, FString::Printf(TEXT("%s: the Back line's centre %.2f cm off the page's (Back's own %.2f cm, %s)"), C.Name, Off, BackOff,
				Line == Box ?TEXT("Back alone on it") : TEXT("Back with its neighbours")));
		}
		const FString File = FString::Printf(TEXT("bc_%s_hover"), C.Name);
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / File + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT backclick shot %s.png (Back box %.0f x %.0f px at %.0f, %.0f)"), *File, SheetBackInk.bIsValid ? SheetBackInk.GetSize().X : 0.0,
			SheetBackInk.bIsValid ? SheetBackInk.GetSize().Y : 0.0, SheetBackInk.bIsValid ? SheetBackInk.GetCenter().X : 0.0, SheetBackInk.bIsValid ? SheetBackInk.GetCenter().Y : 0.0);
		Phase = 3;
		Next = Now + 0.8;
		return;
	}
	if (Phase == 3)
	{
		TestAxes.Add(TEXT("OLVR_RightTrigger"), 1.f);      // (pulled: the menu reads it next frame)
		Phase = 4;
		Next = Now + 0.4;
		return;
	}
	if (Phase == 4)
	{
		TestAxes.Remove(TEXT("OLVR_RightTrigger"));
		const FString Got = MenuPage ? MenuPage->File : FString();
		Check(bMenuOpen && Got == C.Want && MenuStack.Num() == Depth - 1, FString::Printf(TEXT("%s: the trigger on Back went one level up to %s (wanted %s; %d deep, was %d)"),
			C.Name, *Got, C.Want, MenuStack.Num(), Depth));
		++Case;
		Phase = 0;
		Next = Now + 0.4;
	}
}

OL_SHOT_MATCH(OLBackClickTest) { return OLFlags::Param(TEXT("OLBackClickTest")); }
OL_SHOT_TEST(OLBackClickTest, 20)
{
	BackClickTestTick(Dt);           // (REQ-BACKCLICK: OutlawsPlayerBackClickTest.cpp)
}
#endif // WITH_OUTLAWS_TESTS
