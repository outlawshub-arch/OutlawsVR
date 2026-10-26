// -OLShots -OLMenuCentreTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLMenuCentreTest) { return OLFlags::Param(TEXT("OLMenuCentreTest")); }

OL_SHOT_TEST(OLMenuCentreTest, 450)
{
	// REQ-MENUCENTRE: every page under the pause page opened in turn as the player would find it (the help line under a
	// sub-page), facing level: its middle row against eye height and the gap from its last row to the prompt line logged,
	// photographed (mc_<n>_<page>.png), a page that scrolls also at its end; all within 2 cm of eye height, one gap
	static TArray<FString> McPages;
	static TArray<float> McOff, McGap;
	static int32 McIdx = -1, McPhase = 0, McFail = 0;
	static float McNext = 0.f;
	auto Shot = [](const FString& Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false); };
	if (McIdx < 0)
	{
		if (ShotTime < 3.f) return;
		if (OLFlags::Param(TEXT("OLPromptCentreVR"))) bVR = true;     // (BUG-GFXTALL: the headset's prompts, drawings and all)
		OpenMenu();
		if (!bMenuOpen) return;
		TArray<FString> Queue = { TEXT("main_game.rml") };
		TSet<FString> Seen = { TEXT("main_game.rml") };
		while (Queue.Num() > 0)
		{
			const FString File = Queue[0];
			Queue.RemoveAt(0);
			if (!ShowMenuPage(File, false) || !MenuPage) continue;
			McPages.Add(File);
			for (const OutlawsMenu::FItem& I : MenuPage->Items)
			{
				if ((I.Kind == OutlawsMenu::EItemKind::Link || I.Kind == OutlawsMenu::EItemKind::Dialog) && !I.Target.IsEmpty() && !Seen.Contains(I.Target))
				{
					Seen.Add(I.Target);
					Queue.Add(I.Target);
				}
			}
		}
		// (BUG-SPEEDWARN) the VR page again with the speed over the warning's mark: the warning row under the speed
		if (McPages.Contains(TEXT("#vr"))) McPages.Add(TEXT("speedwarn:#vr"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT menucentre: %d pages under pause: %s"), McPages.Num(), *FString::Join(McPages, TEXT(", ")));
		McIdx = 0;
		McPhase = 0;
		McNext = ShotTime + 0.5f;
		return;
	}
	if (ShotTime < McNext) return;
	if (Controller) Controller->SetControlRotation(FRotator(0.0, MenuYaw, 0.0));
	if (McIdx < McPages.Num())
	{
		const bool bWarnView = McPages[McIdx].StartsWith(TEXT("speedwarn:"));
		const FString File = bWarnView ? McPages[McIdx].Mid(10) : McPages[McIdx];
		FString Tag = File.Replace(TEXT("#"), TEXT("")).Replace(TEXT(".rml"), TEXT(""));
		Tag = FString::Printf(TEXT("mc_%02d_%s%s"), McIdx, *Tag, bWarnView ? TEXT("_speedwarn") : TEXT(""));
		static float McSpeedWas = -1.f;
		static bool McAdvance = false;
		if (McPhase == 0)
		{
			if (bWarnView && L && McSpeedWas < 0.f) { McSpeedWas = L->GameSpeed; L->GameSpeed = AOutlawsLevelActor::MaxGameSpeed; }
			ShowMenuPage(File, false);
			if (MenuPage && File == TEXT("#vr"))
			{
				const int32 W = MenuPage->Items.IndexOfByPredicate([](const OutlawsMenu::FItem& It) { return It.Name == TEXT("vr_speedwarn"); });
				const int32 Sp = MenuPage->Items.IndexOfByPredicate([](const OutlawsMenu::FItem& It) { return It.Name == TEXT("vr_speed"); });
				const bool bHigh = L && L->GameSpeed > 0.605f;
				const bool bOk = (W != INDEX_NONE) == bHigh && (W == INDEX_NONE || W == Sp + 1) && (!bWarnView || bHigh);
				McFail += bOk ? 0 : 1;
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT menucentre %s %s: speed %.0f %%, warning row %s"), bOk ? TEXT("ok") : TEXT("FAIL"), *Tag,
					L ? L->GameSpeed * 200.f : -1.f, W == INDEX_NONE ? TEXT("not shown") : *FString::Printf(TEXT("row %d, under the speed's row %d"), W, Sp));
			}
			MenuStack.Reset();
			if (McIdx > 0) MenuStack.Add(TEXT("main_game.rml"));
			MenuScroll = 0;
			MenuPitch = 0.f;
			MenuPitchVel = 0.f;
			RefreshMenuRows();
			PlaceMenuRoot();
			McPhase = 1;
		}
		else if (!McAdvance)
		{
			float Off = 0.f, Gap = -1.f;
			const bool bSheet = MenuCentreMeasure(Off, Gap);
			const bool bEnd = McPhase == 2;
			const FString Name = Tag + (bEnd ? TEXT("_end") : McPhase == 3 ? TEXT("_scrolled") : TEXT(""));
			if (bSheet)
			{
				const bool bOk = FMath::Abs(Off) <= 2.f;
				McFail += bOk ? 0 : 1;
				McOff.Add(Off);
				if (Gap >= 0.f) McGap.Add(Gap);
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT menucentre %s %s: %d rows shown (scroll %d), middle row %+.1f cm from eye height, last row to prompt %s"), bOk ? TEXT("ok") : TEXT("FAIL"),
					*Name, SheetRowsShownNow(), MenuScroll, Off, Gap >= 0.f ? *FString::Printf(TEXT("%.1f cm"), Gap) : TEXT("(no prompt line)"));
			}
			else
			{
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT menucentre -- %s: not a sheet page (its own title-screen layout), not measured"), *Name);
			}
			FString Fit;
			const bool bFits = !bSheet || MenuPageFits(Fit);
			McFail += bFits ? 0 : 1;
			if (bSheet) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT menucentre %s %s fits (%s)"), bFits ? TEXT("ok") : TEXT("FAIL"), *Name, *Fit);
			// (BUG-GFXTALL) the block, "more above" or the title down to the prompts' foot, and its top over eye height
			float Block = 0.f, Top = 0.f, TopDeg = 0.f;
			if (SheetBlockMeasure(Block, Top, TopDeg))
			{
				const bool bIn = Block <= SheetBlockLimitCm() + 0.5f && Top <= SheetTopLimitCm() + 0.5f;
				McFail += bIn ? 0 : 1;
				const float MenuScale = MenuRoot ? float(MenuRoot->GetComponentScale().Z) : 1.f;      // (sheet cm to world cm)
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT menucentre %s %s block: %.1f cm tall (limit %.0f), top %.1f cm over eye height (limit %.0f), %.1f deg up; %d rows, scroll %d"),
					bIn ? TEXT("ok") : TEXT("FAIL"), *Name, Block * MenuScale, SheetBlockLimitCm() * MenuScale, Top * MenuScale, SheetTopLimitCm() * MenuScale, TopDeg, SheetRowsShownNow(), MenuScroll);
			}
			Shot(Name);
			// (the shot is taken at the frame's end: the next view waits for a later tick, or the shot shows it)
			McAdvance = true;
			McNext = ShotTime + 0.2f;
			return;
		}
		if (McAdvance)
		{
			McAdvance = false;
			const bool bEnd = McPhase == 2;
			const int32 MaxScroll = IsSheetPage() ? SheetMaxScroll() : 0;
			if (McPhase == 1 && MaxScroll > 1)
			{
				// (BUG-GFXTALL) a page that pages its rows also one step down: "more above" and "more below" both up
				MenuScroll = 1;
				RefreshMenuRows();
				McPhase = 3;
			}
			else if (!bEnd && MaxScroll > 0)
			{
				MenuScroll = MaxScroll;
				RefreshMenuRows();
				McPhase = 2;
			}
			else
			{
				if (bWarnView && L && McSpeedWas >= 0.f) { L->GameSpeed = McSpeedWas; McSpeedWas = -1.f; }
				++McIdx;
				McPhase = 0;
			}
		}
		McNext = ShotTime + 0.6f;
		return;
	}
	float GapLo = 1e9f, GapHi = -1e9f, OffLo = 1e9f, OffHi = -1e9f;
	for (const float G : McGap) { GapLo = FMath::Min(GapLo, G); GapHi = FMath::Max(GapHi, G); }
	for (const float O : McOff) { OffLo = FMath::Min(OffLo, O); OffHi = FMath::Max(OffHi, O); }
	const bool bGapsOk = McGap.Num() > 0 && GapHi - GapLo <= 1.f;
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT menucentre: %s (%d views measured, %d failed; middle row %.1f..%.1f cm from eye height; last row to prompt %.1f..%.1f cm over %d views)"),
		McFail == 0 && bGapsOk && McOff.Num() > 0 ? TEXT("PASS") : TEXT("FAIL"), McOff.Num(), McFail, OffLo, OffHi, GapLo, GapHi, McGap.Num());
	FGenericPlatformMisc::RequestExit(false);
	ShotTime = -1.f;
	McIdx = McPages.Num() + 1;
}

#endif // WITH_OUTLAWS_TESTS
