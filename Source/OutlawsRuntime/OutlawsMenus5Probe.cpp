// The probe of the four menu jobs from the user's notes on the snapshots (5 Nov): REQ-RECENTIPS (#17), REQ-BUTTONLABEL (#18),
// REQ-BOSSDOWNLABEL (#19) and BUG-QUITGAP (#23):
//   -OLShots -OLLevel=town -OLNoFrontEnd -OLMenus5Probe=<tag> [-OLOldRecentIps] [-ResX=1832 -ResY=1920 -ForceRes -OLDeviceProfile=Quest2]
// Each page is opened, checked and photographed (Saved\OLShots\menus5_<tag>_<page>.png), at the panel size the launch gives:
// the join page with no hosts, with five (test addresses, the longest there can be and a 16-letter name), its drop-down open, a
// host picked, the drop-down shut by a key and by Back; the VR page's reload row both ways; the Gameplay page; every confirm page
// with a message, its pointer rows through MenuFitWalk.  One line a case "OLSHOT menus5 PASS|FAIL: ...", then "OLSHOT menus5
// RESULT".  -OLOldRecentIps: the Recent column as before (shots only on the join page; nothing is pressed: a pick would join).

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsCoop.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Engine/Font.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLMenus5Probe) { return OLFlags::Has(TEXT("OLMenus5Probe")); }

OL_SHOT_TEST(OLMenus5Probe, 2493)
{
	struct FStep { FString Shot; TFunction<void()> Do; TFunction<void()> After; };
	static int32 Passes = 0, Fails = 0, Case = -1, Phase = 0;
	static float Wait = 0.f;
	static TArray<FStep> Steps;
	static FString Tag;
	if (!L || ShotStep < 0) return;
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	static auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT menus5 %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 3.f || !bUp) return;
		L->bInvincible = true;
		OLFlags::Value(TEXT("OLMenus5Probe"), Tag);
		if (Tag.IsEmpty()) Tag = TEXT("run");
		const bool bOld = OLFlags::Param(TEXT("OLOldRecentIps"));
		UOutlawsCoop* C = UOutlawsCoop::Get(this);
		if (!bMenuOpen) OpenMenu();
		// (REQ-MENUFONT) which font the shots are in: the user's MenuOverlay.kpf (what ships) or the remaster's stock one. The run
		// script passes -OLMenuOverlay=<file> and -OLMenuFontExpect when it found the overlay, so a shot in the wrong font fails here
		LoadMenuSource();
		const bool bOverlay = MenuSource && MenuSource->HasOverlay();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT menus5 font: %s"), bOverlay ? TEXT("the MenuOverlay.kpf font (as shipped)") : TEXT("the remaster's stock font (no overlay found)"));
		if (OLFlags::Param(TEXT("OLMenuFontExpect"))) Check(bOverlay, TEXT("the shots are in the shipped menu font (overlay open)"));
		auto Row = [this](const FString& Name) { return MenuPage ? MenuPage->Items.IndexOfByPredicate([&Name](const OutlawsMenu::FItem& It) { return It.Name == Name; }) : INDEX_NONE; };
		auto ListRows = [this]()
		{
			int32 N = 0;
			if (MenuPage) for (const OutlawsMenu::FItem& It : MenuPage->Items) N += It.Name.StartsWith(TEXT("coop:recent:")) ? 1 : 0;
			return N;
		};
		auto Hosts = [C](int32 N)
		{
			// (test addresses, not read from or written to coop_recent.txt: bRecentRead keeps ReadRecent off the file)
			static const TCHAR* const A[] = { TEXT("host-a.example"), TEXT("255 255 255 255:65535"), TEXT("host-b.example"), TEXT("host-c.example:7883"), TEXT("host-d.example") };
			static const TCHAR* const Nm[] = { TEXT(""), TEXT("Sixteen Letters!"), TEXT("Dotard"), TEXT(""), TEXT("Marshal") };
			C->bRecentRead = true;
			C->RecentAddrs.Reset();
			C->RecentNames.Reset();
			for (int32 i = 0; i < N; ++i) { C->RecentAddrs.Add(A[i]); C->RecentNames.Add(Nm[i]); }
		};
		// (REQ-MENUFONT) each host name under its own address: inside the slip, clear of its address and of the next row's address
		auto NamesFit = [this](const TCHAR* What)
		{
			int32 Named = 0, Bad = 0;
			FString Said;
			if (MenuPage) for (int32 k = 0; k < MenuPage->Items.Num(); ++k)
			{
				if (!MenuPage->Items[k].Name.StartsWith(TEXT("coop:recent:")) || !FrontNameInk.IsValidIndex(k) || !FrontNameInk[k].bIsValid || !FrontItemInk.IsValidIndex(k)) continue;
				++Named;
				const FBox2D& N = FrontNameInk[k];
				const FBox2D& A = FrontItemInk[k];
				bool bOk = N.Min.X >= 1140.f && N.Max.X <= 1428.f && N.Min.Y >= A.Max.Y;
				for (int32 j = 0; j < FrontItemInk.Num(); ++j) if (j != k && FrontItemInk[j].bIsValid && FrontItemInk[j].Intersect(N)) bOk = false;
				if (!bOk) { ++Bad; Said += FString::Printf(TEXT(" [%s: name y %.0f..%.0f x %.0f..%.0f under its address y %.0f..%.0f]"), *MenuPage->Items[k].Label, N.Min.Y, N.Max.Y, N.Min.X, N.Max.X, A.Min.Y, A.Max.Y); }
			}
			Check(Named > 0 && Bad == 0, FString::Printf(TEXT("%s: %d host names under their addresses, %d running into one%s"), What, Named, Bad, *Said));
		};
		auto Fits = [this](const TCHAR* What)
		{
			FString Said;
			const bool bFits = MenuPageFits(Said);
			Check(bFits, FString::Printf(TEXT("%s fits (%s)"), What, *Said));
		};
		Steps.Add({ TEXT("join_empty"), [this, Hosts]() { Hosts(0); ShowMenuPage(TEXT("#coopjoin"), true); }, [=]()
		{
			Fits(TEXT("the join page with no hosts"));
			if (!bOld) Check(Row(TEXT("coop:recentlist")) == INDEX_NONE && ListRows() == 0, TEXT("no hosts: no Recent button, no list"));
		} });
		Steps.Add({ TEXT("join_closed"), [this, Hosts]() { Hosts(5); ShowMenuPage(TEXT("#coopjoin"), false); }, [=]()
		{
			Fits(TEXT("the join page with five hosts"));
			if (!bOld) Check(Row(TEXT("coop:recentlist")) != INDEX_NONE && ListRows() == 0, FString::Printf(TEXT("five hosts: the Recent button, the list shut (%d rows)"), ListRows()));
			else UE_LOG(LogOutlaws, Display, TEXT("OLSHOT menus5 old: %d hosts in the column"), ListRows());
		} });
		if (!bOld)
		{
			Steps.Add({ TEXT("join_open"), [this]() { CoopPressRow(TEXT("coop:recentlist")); }, [=]()
			{
				Fits(TEXT("the drop-down open"));
				Check(ListRows() == 5, FString::Printf(TEXT("the button opens the list: %d rows"), ListRows()));
				const int32 Btn = Row(TEXT("coop:recentlist"));
				// each row: on the parchment, clear of the pad's and the password's ink and of the next row, the pointer finds it; the
				// addresses all one size; the slip clear of everything else on the page
				const FBox2D Slip(FVector2D(1140.f, 528.f), FVector2D(1428.f, 528.f + 5.f * 92.f + 12.f));
				bool bSlipClear = true;
				for (int32 k = 0; k < MenuPage->Items.Num() && k < FrontItemInk.Num(); ++k)
				{
					if (!MenuPage->Items[k].Name.StartsWith(TEXT("coop:recent")) && FrontItemInk[k].bIsValid && FrontItemInk[k].Intersect(Slip))
					{
						bSlipClear = false;
						UE_LOG(LogOutlaws, Display, TEXT("OLSHOT menus5 slip over '%s', x %.0f..%.0f y %.0f..%.0f"), *MenuPage->Items[k].Label, FrontItemInk[k].Min.X, FrontItemInk[k].Max.X, FrontItemInk[k].Min.Y, FrontItemInk[k].Max.Y);
					}
				}
				Check(bSlipClear, TEXT("the slip clear of the pad and the rows under it"));
				double Tallest = 0.0, Lowest = 1e6;
				for (int32 r = 0; r < ListRows(); ++r)
				{
					const int32 i = Row(FString::Printf(TEXT("coop:recent:%d"), r));
					if (!FrontItemInk.IsValidIndex(i)) { Check(false, FString::Printf(TEXT("row %d drawn"), r)); continue; }
					const FBox2D Ink = FrontItemInk[i];
					bool bClear = true;
					for (int32 k = 0; k < FrontItemInk.Num(); ++k) if (k != i && FrontItemInk[k].bIsValid && FrontItemInk[k].Intersect(Ink)) bClear = false;
					const int32 Hit = FrontHitAt(Ink.GetCenter());
					const int32 Edge = FrontHitAt(FVector2D(1145.f, Ink.GetCenter().Y));
					Tallest = FMath::Max(Tallest, Ink.GetSize().Y);
					Lowest = FMath::Min(Lowest, Ink.GetSize().Y);
					Check(bClear && Ink.Min.X >= 1140.f && Ink.Max.X <= 1428.f && Hit == i && Edge == i, FString::Printf(TEXT("row %d '%s' ink x %.0f..%.0f y %.0f..%.0f, clear of the rest %d, the pointer finds %d, at the list's edge %d (row %d)"),
						r, *MenuPage->Items[i].Label, Ink.Min.X, Ink.Max.X, Ink.Min.Y, Ink.Max.Y, bClear ? 1 : 0, Hit, Edge, i));
				}
				Check(Tallest - Lowest <= 1.0 && Lowest >= 28.0, FString::Printf(TEXT("the addresses one size: %.0f..%.0f tall (the hint line about 30)"), Lowest, Tallest));
				NamesFit(TEXT("the open list"));
				if (FrontItemInk.IsValidIndex(Btn)) Check(FrontHitAt(FrontItemInk[Btn].GetCenter()) == Btn, FString::Printf(TEXT("the pointer finds the button, ink x %.0f..%.0f"), FrontItemInk[Btn].Min.X, FrontItemInk[Btn].Max.X));
			} });
			Steps.Add({ TEXT("join_filled"), [this]() { CoopPressRow(TEXT("coop:recent:1")); }, [=]()
			{
				Fits(TEXT("the page after a pick"));
				Check(C->JoinAddress == TEXT("255 255 255 255:65535"), FString::Printf(TEXT("the pick fills the address: %s"), *C->JoinAddress));
				Check(C->Mode == EOutlawsCoopMode::None, TEXT("the pick does not join"));
				Check(ListRows() == 0 && MenuPage && MenuPage->File == TEXT("#coopjoin"), TEXT("the pick shuts the list, on the join page"));
			} });
			Steps.Add({ TEXT(""), [this]() { CoopPressRow(TEXT("coop:recentlist")); }, [=]() { Check(ListRows() == 5, TEXT("opened again")); } });
			Steps.Add({ TEXT(""), [this]() { CoopPressRow(TEXT("coop:key:del")); }, [=]()
			{
				Check(ListRows() == 0 && C->JoinAddress == TEXT("255 255 255 255:6553"), FString::Printf(TEXT("a key shuts the list and still types (%s)"), *C->JoinAddress));
			} });
			Steps.Add({ TEXT(""), [this]() { CoopPressRow(TEXT("coop:recentlist")); }, [=]() { Check(ListRows() == 5, TEXT("opened again")); } });
			Steps.Add({ TEXT(""), [this]() { CoopPressRow(TEXT("coop:recentlist")); }, [=]() { Check(ListRows() == 0, TEXT("the button shuts the list")); } });
			Steps.Add({ TEXT(""), [this]() { CoopPressRow(TEXT("coop:recentlist")); }, [=]() { Check(ListRows() == 5, TEXT("opened again")); } });
			Steps.Add({ TEXT(""), [this]() { MenuBack(); }, [=]()
			{
				Check(ListRows() == 0 && MenuPage && MenuPage->File == TEXT("#coopjoin"), TEXT("B / Y back shuts the list and stays on the join page"));
			} });
			// the list as it usually is: no ports (the pad types none), a name on most (the five above: the longest there can be)
			Steps.Add({ TEXT("join_typical"), [=]()
			{
				C->RecentAddrs = { TEXT("host-a.example"), TEXT("host-b.example"), TEXT("host-d.example") };
				C->RecentNames = { TEXT("Dotard"), TEXT("Marshal"), TEXT("") };
				CoopPressRow(TEXT("coop:recentlist"));
			}, [=]() { Fits(TEXT("the drop-down open, three hosts")); Check(ListRows() == 3, TEXT("three hosts: three rows")); NamesFit(TEXT("the open list, three hosts")); } });
			// (REQ-MENUOVERLAYPROBES) -OLMenus5Addr=<address>: the join page and its Recent list with a real address, dots and all, in
			// whatever font this run uses. The value comes from the run script's command line only; the default run shoots nothing here
			FString Dotted;
			if (OLFlags::Value(TEXT("OLMenus5Addr"), Dotted, /*bShouldStopOnSeparator*/ false) && !Dotted.IsEmpty())
			{
				Steps.Add({ TEXT("join_dotted"), [=]()
				{
					C->bRecentRead = true;
					C->RecentAddrs = { Dotted, Dotted + TEXT(":7883") };
					C->RecentNames = { TEXT("Dotard"), TEXT("") };
					C->JoinAddress = Dotted;
					if (ListRows() > 0) CoopPressRow(TEXT("coop:recentlist"));     // (shut, so the press below opens it on the new hosts)
					CoopPressRow(TEXT("coop:recentlist"));
				}, [=]() { Fits(TEXT("the join page, a dotted address, the list open")); Check(ListRows() == 2, TEXT("two hosts: two rows")); NamesFit(TEXT("the open list, dotted addresses")); } });
			}
		}
		// the VR page's reload row, both ways (the row at the page's foot: scrolled there)
		const bool bFace0 = bFaceReload;
		auto Reload = [=](bool bOn)
		{
			bFaceReload = bOn;
			if (!MenuPage || MenuPage->File != TEXT("#vr")) ShowMenuPage(TEXT("#vr"), true);
			MenuScroll = SheetMaxScroll();
			RefreshMenuRows();
		};
		auto RowFit = [=](const TCHAR* Name, const FString& Want)
		{
			const int32 i = Row(Name);
			const FString Got = i != INDEX_NONE ? MenuRowText(i) : TEXT("(no row)");
			const float Natural = MenuRowFont ? float(MenuRowFont->GetStringSize(*Got)) : 0.f;
			const float ColW = 0.9f * float(SheetCanvasSize.X);
			const float Sc = Natural > ColW ? ColW / Natural : 1.f;
			FBox2D Widest(ForceInit);
			for (const FBox2D& B : SheetRowInk) if (!Widest.bIsValid || B.GetSize().X > Widest.GetSize().X) Widest = B;
			Check(Got == Want, FString::Printf(TEXT("%s reads '%s'"), Name, *Got));
			Check(Sc >= 0.8f && Widest.bIsValid && Widest.Min.X >= 0.f && Widest.Max.X <= SheetCanvasSize.X, FString::Printf(TEXT("%s drawn at %.2f of its size (%.0f px wide, the column %.0f); widest row x %.0f..%.0f of %d"),
				Name, Sc, Natural, ColW, Widest.Min.X, Widest.Max.X, SheetCanvasSize.X));
		};
		Steps.Add({ TEXT("vrrow_on"), [=]() { Reload(true); }, [=]() { Fits(TEXT("the VR page")); RowFit(TEXT("vr_facereload"), TEXT("Reload button:  Y / B")); } });
		Steps.Add({ TEXT("vrrow_off"), [=]() { Reload(false); }, [=]() { Fits(TEXT("the VR page")); RowFit(TEXT("vr_facereload"), TEXT("Reload button:  Tap X / A")); } });
		Steps.Add({ TEXT(""), [=]() { Reload(bFace0); MenuScroll = 0; RefreshMenuRows(); }, [=]() {} });
		Steps.Add({ TEXT("gameplay"), [this]() { ShowMenuPage(TEXT("options_gameplay.rml"), true); }, [=]()
		{
			Fits(TEXT("the Gameplay page"));
			RowFit(TEXT("endonboss"), FString::Printf(TEXT("End historical mission on boss down:  %s"), L->bEndOnBoss ? TEXT("On") : TEXT("Off")));
		} });
		// every confirm page with a message: the buttons close under it (title to first button: the old pages 37 cm)
		for (const TCHAR* D : { TEXT("dialog_quit.rml"), TEXT("dialog_exit_game.rml"), TEXT("dialog_restart_level.rml"), TEXT("dialog_save_overwrite.rml"), TEXT("dialog_load_game.rml"),
			TEXT("dialog_delete_game.rml"), TEXT("dialog_reset_bindings.rml"), TEXT("dialog_video_restart.rml") })
		{
			const FString File = D;
			Steps.Add({ FString(File).Replace(TEXT(".rml"), TEXT("")), [=]() { ShowMenuPage(File, true); }, [=]()
			{
				Fits(*File);
				const float PxCm = float(SheetCanvasSize.X) / 100.f;
				const float Gap = SheetRowInk.Num() > 0 && SheetTitleInk.bIsValid ? (SheetRowInk[0].GetCenter().Y - SheetTitleInk.GetCenter().Y) / PxCm : -1.f;
				const FString Message = MenuPage ? MenuPage->Message : FString();
				const int32 Buttons = MenuPage ? MenuPage->Items.Num() : 0;
				const bool bThere = MenuPage && MenuPage->File == File;
				Check(Gap > 0.f && Gap < 32.f && bThere, FString::Printf(TEXT("%s: '%s', %d buttons, title to first button %.1f cm"), *File, *Message, Buttons, Gap));
			} });
			Steps.Add({ TEXT(""), [=]()
			{
				int32 Bad = 0;
				const int32 N = MenuFitWalk(File, [&Bad](bool bOk, const FString& Line) { if (!bOk) { ++Bad; UE_LOG(LogOutlaws, Display, TEXT("OLSHOT menus5 walk: %s"), *Line); } });
				Check(Bad == 0 && N >= 1, FString::Printf(TEXT("%s: the fit walk's %d pages, %d bad (the pointer finds the first and last button)"), *File, N, Bad));
			}, [=]() {} });
		}
		Case = 0;
		Phase = 0;
		Wait = ShotTime + 0.5f;
		ShotStep = 1;
		return;
	}
	case 1:
	{
		if (ShotTime < Wait) return;
		if (Case >= Steps.Num())
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT menus5 RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
			ShotStep = -1;
			FGenericPlatformMisc::RequestExit(false);
			return;
		}
		const FStep& S = Steps[Case];
		if (Phase == 0)
		{
			S.Do();
			RedrawMenu();
			Phase = 1;
		}
		else
		{
			S.After();
			if (!S.Shot.IsEmpty()) FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("menus5_%s_%s.png"), *Tag, *S.Shot), false, false);
			Phase = 0;
			++Case;
		}
		Wait = ShotTime + 0.6f;
		return;
	}
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
