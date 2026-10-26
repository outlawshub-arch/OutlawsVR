// -OLShots -OLPromptCentreTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLPromptCentreTest) { return OLFlags::Param(TEXT("OLPromptCentreTest")); }

OL_SHOT_TEST(OLPromptCentreTest, 440)
{
	// REQ-PROMPTCENTRE: every page under pause as the player finds it (and at its scroll's end): the foot line (back / time /
	// "more below" as one) and "more above" across from the sheet's middle, "more above" clear of the title and the first row;
	// then the notices (a tutorial card and its hint) and the death screen's prompt across from the line of sight. Each
	// photographed (pc_<n>_<page>.png, pc_hud.png, pc_death.png); every line within 1 cm of the middle
	static TArray<FString> PcPages;
	static int32 PcIdx = -1, PcPhase = 0, PcFail = 0, PcLines = 0;
	static float PcNext = 0.f;
	auto Shot = [](const FString& Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false); };
	auto Say = [](bool bOk, const FString& Screen, const FString& Line, float Off)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT promptcentre %s %s: \"%s\" %+.1f cm from the middle"), bOk ? TEXT("ok") : TEXT("FAIL"), *Screen, *Line, Off);
	};
	auto Across = [this](const USceneComponent* T)
	{
		// the component's middle across the view from the line of sight (cm)
		const FVector Eye = Camera->GetComponentLocation();
		return float(FVector::DotProduct(T->GetComponentLocation() - Eye, Camera->GetRightVector()));
	};
	if (PcIdx < 0)
	{
		if (ShotTime < 3.f) return;
		if (OLFlags::Param(TEXT("OLPromptCentreVR"))) bVR = true;     // (the headset's pages, rows and prompt words)
		OpenMenu();
		if (!bMenuOpen) return;
		TArray<FString> Queue = { TEXT("main_game.rml") };
		TSet<FString> Seen = { TEXT("main_game.rml") };
		while (Queue.Num() > 0)
		{
			const FString File = Queue[0];
			Queue.RemoveAt(0);
			if (!ShowMenuPage(File, false) || !MenuPage) continue;
			PcPages.Add(File);
			for (const OutlawsMenu::FItem& I : MenuPage->Items)
			{
				if ((I.Kind == OutlawsMenu::EItemKind::Link || I.Kind == OutlawsMenu::EItemKind::Dialog) && !I.Target.IsEmpty() && !Seen.Contains(I.Target))
				{
					Seen.Add(I.Target);
					Queue.Add(I.Target);
				}
			}
		}
		// (and two made to scroll, six rows at most: "more below" beside the back line, then "more above" at the end)
		PcPages.Add(TEXT("cap:options_display.rml"));
		PcPages.Add(TEXT("cap:main_game.rml"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT promptcentre: %d pages under pause"), PcPages.Num() - 2);
		PcIdx = 0;
		PcPhase = 0;
		PcNext = ShotTime + 0.5f;
		return;
	}
	if (ShotTime < PcNext) return;
	if (Controller) Controller->SetControlRotation(FRotator(0.0, MenuYaw, 0.0));
	if (PcIdx < PcPages.Num())
	{
		const bool bCap = PcPages[PcIdx].StartsWith(TEXT("cap:"));
		const FString File = bCap ? PcPages[PcIdx].Mid(4) : PcPages[PcIdx];
		const FString Tag = FString::Printf(TEXT("pc_%02d_%s%s"), PcIdx, *File.Replace(TEXT("#"), TEXT("")).Replace(TEXT(".rml"), TEXT("")), bCap ? TEXT("_scrolls") : TEXT(""));
		if (PcPhase == 0)
		{
			TestSheetRowCap = bCap ? 6 : 0;
			ShowMenuPage(File, false);
			MenuStack.Reset();
			if (File != TEXT("main_game.rml")) MenuStack.Add(TEXT("main_game.rml"));
			MenuScroll = 0;
			MenuPitch = 0.f;
			MenuPitchVel = 0.f;
			RefreshMenuRows();
			PlaceMenuRoot();
			PcPhase = 1;
		}
		else if (PcPhase == 3)
		{
			// (scrolled a tick after the shot: the shot is taken at the end of its frame, so a scroll in that frame showed in it)
			MenuScroll = IsSheetPage() ? SheetMaxScroll() : 0;
			RefreshMenuRows();
			PcPhase = 2;
		}
		else
		{
			const bool bEnd = PcPhase == 2;
			const FString Name = Tag + (bEnd ? TEXT("_end") : TEXT(""));
			float Off = 0.f;
			FString Line;
			if (PromptFootOffset(Off, Line))
			{
				const bool bOk = FMath::Abs(Off) <= 1.f;
				PcFail += bOk ? 0 : 1;
				++PcLines;
				Say(bOk, Name, Line, Off);
			}
			else UE_LOG(LogOutlaws, Display, TEXT("OLSHOT promptcentre -- %s: no foot line%s"), *Name, IsSheetPage() ? TEXT("") : TEXT(" (not a sheet page)"));
			if (PromptMoreUpOffset(Off))
			{
				const bool bClear = !(SheetTitleInk.bIsValid && SheetTitleInk.Intersect(SheetMoreUpInk)) && !(SheetRowInk.Num() > 0 && SheetRowInk[0].Intersect(SheetMoreUpInk));
				const bool bOk = FMath::Abs(Off) <= 1.f && bClear;
				PcFail += bOk ? 0 : 1;
				++PcLines;
				Say(bOk, Name, MenuMoreUp ? MenuMoreUp->Text.ToString() : FString(), Off);
				if (!bClear) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT promptcentre FAIL %s: \"more above\" touches the title or the first row"), *Name);
			}
			FString Fit;
			const bool bFits = !IsSheetPage() || MenuPageFits(Fit);
			PcFail += bFits ? 0 : 1;
			if (IsSheetPage()) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT promptcentre %s %s fits (%s)"), bFits ? TEXT("ok") : TEXT("FAIL"), *Name, *Fit);
			Shot(Name);
			const int32 MaxScroll = IsSheetPage() ? SheetMaxScroll() : 0;
			if (!bEnd && MaxScroll > 0) PcPhase = 3;
			else
			{
				++PcIdx;
				PcPhase = 0;
			}
		}
		PcNext = ShotTime + 0.6f;
		return;
	}
	// the notices: a tutorial card's line and its hint (the same lines ShowMessage gets from OutlawsTutorial.cpp)
	if (PcIdx == PcPages.Num())
	{
		if (PcPhase == 0)
		{
			TestSheetRowCap = 0;
			CloseMenu();
			ShowMessage(TEXT("Point the revolver down the street and pull the trigger to fire."), 30.f);
			ShowMessage(TEXT("Raise your hand to throw further."), 30.f);
			PcPhase = 1;
			PcNext = ShotTime + 0.8f;
			return;
		}
		if (HudText)
		{
			const float Off = Across(HudText);
			TArray<FString> Lines;
			HudText->Text.ToString().ParseIntoArrayLines(Lines);
			const bool bCentred = HudText->HorizontalAlignment == EHTA_Center;
			for (const FString& Ln : Lines)
			{
				const bool bOk = bCentred && FMath::Abs(Off) <= 1.f;
				PcFail += bOk ? 0 : 1;
				++PcLines;
				Say(bOk, TEXT("notices (tutorial card)"), Ln, Off);
			}
		}
		Shot(TEXT("pc_hud"));
		DropMessage(TEXT("Point the revolver down the street and pull the trigger to fire."));
		DropMessage(TEXT("Raise your hand to throw further."));
		++PcIdx;
		PcPhase = 0;
		PcNext = ShotTime + 0.6f;
		return;
	}
	// the death screen
	if (PcIdx == PcPages.Num() + 1)
	{
		if (PcPhase == 0)
		{
			OnDied();
			PcPhase = 1;
			PcNext = ShotTime + 2.5f;
			return;
		}
		if (DeathPlate && DeathPlate->IsVisible())
		{
			// (REQ-BUTTONGLYPHS: VR shows the prompt on the glyph plate, every row centred on it)
			const float Off = Across(DeathPlate);
			for (const FOutlawsGlyphRow& R : DeathPlate->GetRows())
			{
				const bool bOk = FMath::Abs(Off) <= 1.f;
				PcFail += bOk ? 0 : 1;
				++PcLines;
				Say(bOk, TEXT("death screen"), R.Text, Off);
			}
		}
		else if (RespawnText && RespawnText->IsVisible())
		{
			const float Off = Across(RespawnText);
			TArray<FString> Lines;
			RespawnText->Text.ToString().ParseIntoArrayLines(Lines);
			const bool bCentred = RespawnText->HorizontalAlignment == EHTA_Center;
			for (const FString& Ln : Lines)
			{
				const bool bOk = bCentred && FMath::Abs(Off) <= 1.f;
				PcFail += bOk ? 0 : 1;
				++PcLines;
				Say(bOk, TEXT("death screen"), Ln, Off);
			}
		}
		else
		{
			++PcFail;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT promptcentre FAIL death screen: its prompt not shown"));
		}
		Shot(TEXT("pc_death"));
		++PcIdx;
		PcNext = ShotTime + 0.6f;
		return;
	}
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT promptcentre: %s (%d lines measured, %d failed)"), PcFail == 0 && PcLines > 0 ? TEXT("PASS") : TEXT("FAIL"), PcLines, PcFail);
	FGenericPlatformMisc::RequestExit(false);
	ShotTime = -1.f;
	PcIdx = PcPages.Num() + 3;
}

#endif // WITH_OUTLAWS_TESTS
