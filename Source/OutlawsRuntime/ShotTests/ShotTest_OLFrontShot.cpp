// -OLShots -OLFrontShot: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLFrontShot) { return OLFlags::Param(TEXT("OLFrontShot")); }

OL_SHOT_TEST(OLFrontShot, 860)
{
	// the title pages as they draw: the main menu (nothing pointed at, then an item pointed at), the difficulty page,
	// then (BUG-MENUBLACK) the options and load pages, each page's art drawn the first time it is seen
	auto Shot = [](const TCHAR* Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(Name) + TEXT(".png"), false, false); };
	// (BUG-MENUGLYPH) -OLFrontShotVR=right|left|one|oneleft: the headset's main menu in that layout (this run only, nothing
	// saved); -OLFrontShotQuick: out after the main menu's two shots
	FString FsLay;
	const bool bFsVR = OLFlags::Value(TEXT("OLFrontShotVR"), FsLay);
	if (ShotStep == 0 && ShotTime > 4.f && bMenuOpen)
	{
		if (bFsVR)
		{
			bVR = true;
			DominantHand = (FsLay == TEXT("left") || FsLay == TEXT("oneleft")) ? EOutlawsHand::Left : EOutlawsHand::Right;
			OneController = FsLay == TEXT("one") ? 2 : FsLay == TEXT("oneleft") ? 1 : 0;
			if (MenuPage) ShowMenuPage(MenuPage->File, false);
		}
		++ShotStep; ShotTime = 4.f;   // (the timeline from the menu's opening)
	}
	else if (ShotStep == 1 && ShotTime > 4.5f)
	{
		Shot(TEXT("ol_front_plain"));
		// the sheet's middle against the line of sight (0 = level with the eyes) and where the help plate sits on it
		const bool bPlate = MenuHelpPlate && MenuHelpPlate->IsVisible();
		const FVector Pl = bPlate ? MenuHelpPlate->GetRelativeLocation() : FVector::ZeroVector;
		// the drawn words and controller (the plate's ink), in sheet cm from its middle: right is +, up is +
		const FBox2D Ink = bPlate ? MenuHelpPlate->InkBox() : FBox2D(ForceInit);
		const float Cpp = bPlate ? MenuHelpPlate->WidthCmNow() / float(MenuHelpPlate->PixWide()) : 0.f;
		const float Hh = bPlate ? MenuHelpPlate->HeightCm() * 0.5f : 0.f, Wh = bPlate ? MenuHelpPlate->WidthCmNow() * 0.5f : 0.f;
		const float L0 = -Pl.Y - Wh + Ink.Min.X * Cpp, R0 = -Pl.Y - Wh + Ink.Max.X * Cpp, T0 = Pl.Z + Hh - Ink.Min.Y * Cpp, B0 = Pl.Z + Hh - Ink.Max.Y * Cpp;
		const bool bIn = Ink.bIsValid && L0 >= -SheetW * 0.5f && R0 <= SheetW * 0.5f && B0 >= -SheetH * 0.5f && T0 <= SheetH * 0.5f;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT front %s: sheet %.0f x %.0f cm, its middle %.1f cm above the line of sight; help %s"), bFsVR ? *FsLay : TEXT("desktop"),
			SheetW, SheetH, -MenuEyeZ(), bPlate ? *FString::Printf(TEXT("ink left %.1f right %.1f top %.1f foot %.1f (sheet right edge %.1f, foot %.1f): %s"),
				L0, R0, T0, B0, SheetW * 0.5f, -SheetH * 0.5f, bIn ? TEXT("inside the sheet") : TEXT("outside the sheet"))
				: TEXT("drawn on the sheet"));
		++ShotStep;
	}
	else if (ShotStep == 2 && ShotTime > 5.f) { MenuHover[1] = 0; RedrawMenu(); ++ShotStep; }
	else if (ShotStep == 3 && ShotTime > 5.5f)
	{
		Shot(TEXT("ol_front"));
		++ShotStep;
		if (OLFlags::Param(TEXT("OLFrontShotPages"))) { MenuHover[1] = -1; ShotStep = 100; ShotTime = 6.f; }
		else if (OLFlags::Param(TEXT("OLFrontShotQuick"))) { FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; }
	}
	else if (ShotStep >= 100)
	{
		// (BUG-MENUGLYPH2) -OLFrontShotPages: every wide page in turn (one level under the main menu, so its Back shows), a shot
		// of each (ol_page_<name>) and where its help line's ink sits: on the sheet, against the parchment's clean edge
		// (WideParchmentRight) and clear of the page's rows; with the sheet's middle against the line of sight
		static const TCHAR* const Pages[] = { TEXT("#front"), TEXT("#newgame"), TEXT("#missions"), TEXT("#difficulty:story"), TEXT("#coopmulti"), TEXT("#coop"),
			TEXT("#coopcomp"), TEXT("#coophost"), TEXT("#coophostopts"), TEXT("#cooplevels"), TEXT("#cooplobby"), TEXT("#coopclient"), TEXT("#coopscore"),
			TEXT("#cooppause"), TEXT("#coopjoin"), TEXT("#kbd"), TEXT("#character"), TEXT("load_game.rml"), TEXT("save_game.rml"),
			TEXT("#extras"), TEXT("#extras_achiev"), TEXT("#extras_shelf") };          // (REQ-EXTRAS)
		static int32 Bad = 0;
		const int32 Num = UE_ARRAY_COUNT(Pages);
		const int32 Pi = (ShotStep - 100) / 2;
		if (Pi >= Num)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT pages %s: %d of %d pages with the help line badly placed"), bFsVR ? *FsLay : TEXT("desktop"), Bad, Num);
			FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f;
			return;
		}
		const float At = 6.f + float(Pi) * 1.5f;
		if ((ShotStep - 100) % 2 == 0 && ShotTime > At)
		{
			MenuStack.Reset();
			if (Pi > 0) MenuStack.Add(TEXT("#front"));
			ShowMenuPage(Pages[Pi], false);
			++ShotStep;
		}
		else if ((ShotStep - 100) % 2 == 1 && ShotTime > At + 1.f)
		{
			const FString Nm = FString(Pages[Pi]).Replace(TEXT("#"), TEXT("")).Replace(TEXT(":"), TEXT("_")).Replace(TEXT(".rml"), TEXT(""));
			Shot(*(TEXT("ol_page_") + Nm));
			// the help line's ink in the 1920 x 1080 view: as drawn on the flat screen, or the VR plate's drawn words and controllers
			// (each of its rows, and all of them together)
			TArray<FBox2D> Parts;
			FBox2D Ink = WideHelpInk;
			if (WideHelpInk.bIsValid) Parts.Add(WideHelpInk);
			const bool bPlate = MenuHelpPlate && MenuHelpPlate->IsVisible();
			if (bPlate)
			{
				const FVector Pl = MenuHelpPlate->GetRelativeLocation();
				const float Cpp = MenuHelpPlate->WidthCmNow() / float(MenuHelpPlate->PixWide());
				const float Hh = MenuHelpPlate->HeightCm() * 0.5f, Wh = MenuHelpPlate->WidthCmNow() * 0.5f;
				// (sheet cm from its middle, right and up +, to the view: x right from the left edge, y down from the top)
				auto View = [this](float Xc, float Zc) { return FVector2D((Xc + SheetW * 0.5f) * 1920.f / SheetW, (SheetH * 0.5f - Zc) * 1080.f / SheetH); };
				auto ToView = [&](const FBox2D& B) { return B.bIsValid ? FBox2D(View(-Pl.Y - Wh + B.Min.X * Cpp, Pl.Z + Hh - B.Min.Y * Cpp), View(-Pl.Y - Wh + B.Max.X * Cpp, Pl.Z + Hh - B.Max.Y * Cpp)) : FBox2D(ForceInit); };
				Ink = ToView(MenuHelpPlate->InkBox());
				Parts.Reset();
				for (int32 r = 0; r < MenuHelpPlate->GetRows().Num(); ++r) if (MenuHelpPlate->RowBox(r).bIsValid) Parts.Add(ToView(MenuHelpPlate->RowBox(r)));
			}
			const bool bOnSheet = Ink.bIsValid && Ink.Min.X >= 0.f && Ink.Max.X <= 1920.f && Ink.Min.Y >= 0.f && Ink.Max.Y <= 1080.f;
			const float Edge = WideParchmentRight(), Torn = WideTornRight();
			// on the clean parchment (left of the torn edge), or clear of it to the right (the VR plate on the art past it)
			const bool bOffTorn = Ink.bIsValid && (Ink.Max.X <= Edge || Ink.Min.X >= Torn);
			// clear of the page's rows as drawn: their labels' letters (a text box's middle 62 %, the drawn boxes' rule, YL * 0.31
			// either side), the flat line's letters likewise; the pages that draw no labels by their pointing boxes (the character
			// page's tiles, whose frames and names end 30 above their boxes' feet; the difficulty rows; the keyboard's cells); the
			// save and load pages by their slot list's frame
			auto Letters = [](const FBox2D& B) { const float Cut = float(B.GetSize().Y) * 0.19f; return FBox2D(B.Min + FVector2D(0.f, Cut), B.Max - FVector2D(0.f, Cut)); };
			FString Rows;
			int32 Over = 0;
			auto Hits = [&](const FBox2D& B, const FString& What) { for (const FBox2D& Pt : Parts) if (B.Intersect(bPlate ? Pt : Letters(Pt))) { ++Over; Rows += TEXT(" ") + What; return; } };
			const bool bChar = MenuPage && MenuPage->File == TEXT("#character");
			if (IsAchievPage()) Hits(FBox2D(FVector2D(953.f, 194.f), FVector2D(1793.f, 990.f)), TEXT("the achievements frame"));      // (REQ-EXTRAS)
			else if (IsSavePage()) Hits(FBox2D(FVector2D(822.f, 145.f), FVector2D(1662.f, 941.f)), TEXT("the slot list"));
			else
			{
				bool bLabels = false;
				for (int32 i = 0; i < FrontItemInk.Num(); ++i) if (FrontItemInk[i].bIsValid) { bLabels = true; Hits(Letters(FrontItemInk[i]), FString::FromInt(i)); }
				if (!bLabels) for (const TPair<FBox2D, int32>& H : FrontHits) Hits(bChar ? FBox2D(H.Key.Min, H.Key.Max - FVector2D(0.f, 30.f)) : H.Key, FString::FromInt(H.Value));
				if (!bLabels && FrontInk.bIsValid) Hits(FrontInk, TEXT("lines"));
			}
			const bool bOk = bOnSheet && bOffTorn && Over == 0;
			if (!bOk) ++Bad;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT page %s %s: sheet middle %.1f cm above the line of sight; help %s ink x %.0f..%.0f y %.0f..%.0f (view 1920 x 1080; parchment clean to x %.0f, torn edge to %.0f; right end %.1f cm in from the clean edge, %.1f from the sheet's, foot %.1f cm up from the sheet's); %s, %s, %s: %s"),
				Pages[Pi], bFsVR ? *FsLay : TEXT("desktop"), -MenuEyeZ(), bPlate ? TEXT("plate") : TEXT("line"), Ink.Min.X, Ink.Max.X, Ink.Min.Y, Ink.Max.Y, Edge, Torn,
				(Edge - Ink.Max.X) * SheetW / 1920.f, (1920.f - Ink.Max.X) * SheetW / 1920.f, (1080.f - Ink.Max.Y) * SheetH / 1080.f,
				bOnSheet ? TEXT("inside the sheet") : TEXT("OUTSIDE the sheet"), bOffTorn ? TEXT("off the torn edge") : TEXT("ON the torn edge"),
				Over == 0 ? TEXT("clear of the rows") : *FString::Printf(TEXT("OVER rows%s"), *Rows), bOk ? TEXT("ok") : TEXT("BAD"));
			++ShotStep;
		}
	}
	else if (ShotStep == 4 && OLFlags::Param(TEXT("OLFrontShotQuick"))) {}
	else if (ShotStep == 4 && ShotTime > 6.f) { MenuHover[1] = -1; ShowMenuPage(TEXT("#newgame"), true); ++ShotStep; }
	else if (ShotStep == 5 && ShotTime > 7.f) { Shot(TEXT("ol_front_diff")); ++ShotStep; }
	else if (ShotStep == 6 && ShotTime > 7.5f) { MenuBack(); ShowMenuPage(TEXT("options.rml"), true); ++ShotStep; }
	else if (ShotStep == 7 && ShotTime > 8.5f) { Shot(TEXT("ol_front_options")); ++ShotStep; }
	else if (ShotStep == 8 && ShotTime > 9.f)
	{
		// (REQ-TITLEMENU) the title's Load Game pressed (the load page; with no save it stays greyed out on the title)
		MenuBack();
		const int32 Ld = MenuPage ? MenuPage->Items.IndexOfByPredicate([](const OutlawsMenu::FItem& I) { return I.Name == TEXT("title_load"); }) : INDEX_NONE;
		if (Ld != INDEX_NONE) MenuActivate(MenuPointerHand(), Ld);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT front: Load Game pressed, the page is %s"), MenuPage ? *MenuPage->File : TEXT("?"));
		++ShotStep;
	}
	else if (ShotStep == 9 && ShotTime > 10.f) { Shot(TEXT("ol_front_load")); ++ShotStep; }
	else if (ShotStep == 10 && ShotTime > 11.f)
	{
		// the resume point (user 2026-10-02; REQ-TITLEMENU: the load page's first row, from the title's Load Game) loaded,
		// then a look at play going on (ol_front_resumed)
		const bool bOnLoad = MenuPage && MenuPage->File == TEXT("load_game.rml");
		const int32 Resume = bOnLoad ? MenuPage->Items.IndexOfByPredicate([](const OutlawsMenu::FItem& I) { return I.Name == TEXT("load:-1"); }) : INDEX_NONE;
		if (Resume == 0)
		{
			MenuActivate(MenuPointerHand(), Resume);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT front: resume point picked on the load page, %s"), bMenuOpen ? TEXT("FAILED (the menu is still up)") : TEXT("loading"));
			++ShotStep;
		}
		else
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT front: no resume point to load (page %s, row %d)"), MenuPage ? *MenuPage->File : TEXT("?"), Resume);
			FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f;
		}
	}
	else if (ShotStep == 11 && ShotTime > 21.f)
	{
		const AOutlawsLevelActor* LR = Level.Get();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT front: resumed in %s, front end %d, menu %d"), LR ? *LR->LevelName : TEXT("?"), LR && LR->IsFrontEnd() ? 1 : 0, bMenuOpen ? 1 : 0);
		Shot(TEXT("ol_front_resumed"));
		++ShotStep;
	}
	else if (ShotStep == 12 && ShotTime > 22.f) { FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; }
}

#endif // WITH_OUTLAWS_TESTS
