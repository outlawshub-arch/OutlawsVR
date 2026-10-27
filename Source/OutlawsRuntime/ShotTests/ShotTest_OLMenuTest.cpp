// -OLShots -OLMenuTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLMenuTest) { return OLFlags::Param(TEXT("OLMenuTest")); }

OL_SHOT_TEST(OLMenuTest, 460)
{
	// the game menu on its parchment: the pause page with a row pointed at, then the VR page
	auto Shot = [](const TCHAR* Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(Name) + TEXT(".png"), false, false); };
	const float Due[] = { 3.f, 4.f, 5.f, 6.f, 7.f, 8.f, 9.f, 10.f, 11.f, 12.f, 13.f, 14.f, 15.f, 16.f, 17.f, 18.f, 19.f };
	static float StartAt = -1.f;           // (-OLMenuTestAt=<s>: later, once a level given with -OLLevel has settled)
	if (StartAt < 0.f) { StartAt = 0.f; OLFlags::Value(TEXT("OLMenuTestAt"), StartAt); }
	// (REQ-SUBMENUSPACE: each sub-page shot says how it fits the taller sheet)
	auto Fits = [this](const TCHAR* Name)
	{
		FString Said;
		const bool bOk = MenuPageFits(Said);
		UE_LOG(LogOutlaws, Display, TEXT("OLRT menu shot %s: fits %s (%s)"), Name, bOk ? TEXT("PASS") : TEXT("FAIL"), *Said);
	};
	if (ShotStep >= UE_ARRAY_COUNT(Due) || ShotTime < Due[ShotStep] + StartAt) return;
	// look level at the menu (a stray mouse on the test window would tip the view off it)
	if (ShotStep > 0 && Controller) Controller->SetControlRotation(FRotator(0.0, MenuYaw, 0.0));
	switch (ShotStep)
	{
	case 0: OpenMenu(); break;
	case 1:
		MenuHover[1] = 1;
		RedrawMenu();
		// a lot of pointing about (the 2026-09-27 crash: hover sounds under the frozen world)
		if (OLFlags::Param(TEXT("OLHoverFlood")))
		{
			for (int32 i = 0; i < 40; ++i) if (L) L->PlayMenuSound(i % 5 == 4 ? TEXT("chicken1") : TEXT("riflout"));
		}
		break;
	case 2:
	{
		Shot(TEXT("ol_menu_main"));
		{
			// (REQ-NOUNSTUCK) the pause page: no Tools or Unstuck row; no Cheats row (or Dev checks) until the turn shows it
			FString Rows;
			bool bOk = MenuPage && MenuPage->File == TEXT("main_game.rml");
			if (MenuPage) for (const OutlawsMenu::FItem& It : MenuPage->Items)
			{
				Rows += (Rows.IsEmpty() ? TEXT("") : TEXT(" | ")) + It.Label;
				if (It.Target == TEXT("#tools") || It.Name == TEXT("unstuck") || It.Label == TEXT("Tools") || It.Label == TEXT("Unstuck")) bOk = false;
				if (!bCheatsShown && (It.Target == TEXT("#cheats") || It.Target == TEXT("#devchecks"))) bOk = false;
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLRT menu shot ol_menu_main rows %s: %s (no Tools, no Unstuck; cheats %s)"), bOk ? TEXT("PASS") : TEXT("FAIL"), *Rows,
				bCheatsShown ? TEXT("shown") : TEXT("hidden: no Cheats row"));
		}
		// what the sheet holds: its coverage (alpha) and where it sits against the eye
		TArray<FColor> Px;
		int32 Covered = 0, Lit = 0;
		if (MenuMask)
		{
			if (FTextureRenderTargetResource* Res = MenuMask->GameThread_GetRenderTargetResource()) Res->ReadPixels(Px);
			for (const FColor& P : Px) { Covered += P.R > 16; }
		}
		if (MenuCanvas)
		{
			TArray<FColor> Cx;
			if (FTextureRenderTargetResource* Res = MenuCanvas->GameThread_GetRenderTargetResource()) Res->ReadPixels(Cx);
			for (const FColor& P : Cx) { Lit += (P.R + P.G + P.B) > 48; }
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLRT menu sheet: %d px, %d covered, %d with colour; visible %d, material %s, %.0f cm from the eye, root scale %.2f"),
			Px.Num(), Covered, Lit, MenuSheet ? MenuSheet->IsVisible() : -1, MenuSheet && MenuSheet->GetMaterial(0) ? *MenuSheet->GetMaterial(0)->GetName() : TEXT("none"),
			MenuSheet ? FVector::Dist(MenuSheet->GetComponentLocation(), Camera->GetComponentLocation()) : -1.0, MenuRoot ? MenuRoot->GetComponentScale().X : -1.0);
		break;
	}
	case 3: MenuHover[1] = -1; ShowMenuPage(TEXT("options.rml"), true); ShowMenuPage(TEXT("#vr"), true); break;
	case 4: Shot(TEXT("ol_menu_vr")); Fits(TEXT("ol_menu_vr")); break;
	case 5: MenuScroll = SheetMaxScroll(); RefreshMenuRows(); RedrawMenu(); break;          // (the page scrolled to its last rows: the layout row)
	case 6: Shot(TEXT("ol_menu_vr_end")); Fits(TEXT("ol_menu_vr_end")); break;
	// (REQ-SUBMENUSPACE) the Display page, the longest, at the top and scrolled to its end
	case 7: MenuBack(); ShowMenuPage(TEXT("options_display.rml"), true); break;
	case 8: Shot(TEXT("ol_menu_display")); Fits(TEXT("ol_menu_display")); break;
	case 9: MenuScroll = SheetMaxScroll(); RefreshMenuRows(); RedrawMenu(); break;
	case 10: Shot(TEXT("ol_menu_display_end")); Fits(TEXT("ol_menu_display_end")); break;
	// (REQ-RANDOMOVE) the Gameplay page, its Randomizer row at the bottom once unlocked (-OLUnlockAll)
	case 11: MenuBack(); ShowMenuPage(TEXT("options_gameplay.rml"), true); MenuScroll = SheetMaxScroll(); RefreshMenuRows(); break;
	case 12:
	{
		Shot(TEXT("ol_menu_gameplay"));
		Fits(TEXT("ol_menu_gameplay"));
		FString Rows;
		if (MenuPage) for (const OutlawsMenu::FItem& It : MenuPage->Items) Rows += (Rows.IsEmpty() ? TEXT("") : TEXT(" | ")) + It.Label;
		UE_LOG(LogOutlaws, Display, TEXT("OLRT menu shot ol_menu_gameplay rows: %s"), *Rows);
		break;
	}
	// (REQ-EXTRAS) the Extras page; REQ-NOUNLOCKABLES: Achievements, Collectibles, Credits, no Unlockables page
	case 13: MenuStack.Reset(); ShowMenuPage(TEXT("#extras"), true); break;
	case 14:
	{
		Shot(TEXT("ol_menu_extras"));
		Fits(TEXT("ol_menu_extras"));
		FString Rows;
		if (MenuPage) for (const OutlawsMenu::FItem& It : MenuPage->Items) Rows += (Rows.IsEmpty() ? TEXT("") : TEXT(" | ")) + It.Label + (It.Classes.Contains(TEXT("disabled")) ? TEXT(" [greyed]") : TEXT(""));
		const bool bRows = Rows == TEXT("Achievements | Collectibles | Credits");
		UE_LOG(LogOutlaws, Display, TEXT("OLRT menu shot ol_menu_extras rows %s: %s"), bRows ? TEXT("PASS") : TEXT("FAIL"), *Rows);
		break;
	}
	default: FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; break;
	}
	++ShotStep;
}

#endif // WITH_OUTLAWS_TESTS
