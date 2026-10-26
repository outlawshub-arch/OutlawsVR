// REQ-HMCAREER (user 14 Oct): the five original Historical Missions (The Fort, The Granary, Dry Gulch, Simm's Fort, The
// Wilderness) only through Marshal Training; the Handful of Missions keep their rows on the Historical Missions page.
//
// -OLHmCareerTest[=fit] (scratchpad gate_lane3\hmcareer_checks.ps1): from the title menu, New Game > Historical Missions:
//   the page holds Marshal Training and every Handful mission the LABs have, none of the five (by level file); co-op's
//   Historical Missions list the same; every row drawn above the help line, apart, and each row's middle picks its own row
//   for each pointing hand and for the desktop's view ray.  fit stops there.  Otherwise: Marshal Training > Good, the crawl
//   skipped, the office as a Historical Missions session, its posters naming the five; the Fort's poster ridden; a save in
//   the Fort (slot 8); an old-style save in the Granary (slot 9: the single-mission start the page used to make, the same
//   save format); slot 9 then slot 8 loaded back.

#include "OutlawsLevelActor.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsPlayer.h"
#include "OutlawsCoop.h"
#include "OutlawsLab.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsMovieScreen.h"
#include "Camera/CameraComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

namespace OutlawsHmCareerLocal
{
	struct FTest
	{
		int32 Phase = 0;
		double At = 0.0;
		double Since = 0.0;
		int32 Passes = 0, Fails = 0;
		bool bDone = false;
	};
	FTest GTest;
}

#if WITH_OUTLAWS_TESTS
void AOutlawsLevelActor::TickHmCareerTest(float Dt)
{
	using namespace OutlawsHmCareerLocal;
	FTest& T = GTest;
	if (T.bDone) return;
	FString Mode = TEXT("all");
	OLFlags::Value(TEXT("OLHmCareerTest"), Mode);
	UWorld* World = GetWorld();
	AOutlawsPlayer* P = World && World->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(World->GetFirstPlayerController()->GetPawn()) : nullptr;
	if (!P) return;
	const double Now = FPlatformTime::Seconds();
	if (T.At == 0.0) { T.At = Now + 4.0; T.Since = Now; }
	if (Now < T.At) return;
	auto Check = [&T](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLHMC check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++T.Passes; else ++T.Fails;
	};
	auto Finish = [&T, &Mode]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLHMC test %s RESULT %s (%d passed, %d failed)"), *Mode, T.Fails == 0 && T.Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), T.Passes, T.Fails);
		T.bDone = true;
		FGenericPlatformMisc::RequestExit(false);
	};
	auto Shot = [](const FString& Name)
	{
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("ol_hmcareer_%s.png"), *Name), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLHMC screenshot ol_hmcareer_%s.png"), *Name);
	};
	auto Next = [&T, Now](int32 Phase, double Wait) { T.Phase = Phase; T.At = Now + Wait; T.Since = Now; };
	auto Built = [this]() { return !bFrontEnd && !bRebuildPending && !bNeedPlace && Inf != nullptr && !Movie; };
	auto TimedOut = [&](double Secs, const TCHAR* What)
	{
		if (Now - T.Since <= Secs) return false;
		Check(false, FString::Printf(TEXT("%s within %.0f s (level %s)"), What, Secs, *LevelName));
		Finish();
		return true;
	};
	const FString Page = P->bMenuOpen && P->MenuPage ? P->MenuPage->File : FString();
	static const TCHAR* const Five[] = { TEXT("shfort"), TEXT("shgranry"), TEXT("shgulch"), TEXT("shsimms"), TEXT("shwildns") };
	switch (T.Phase)
	{
	case 0:
		if (!(bFrontEnd && !bRebuildPending && !bNeedPlace && Page == TEXT("#front")))
		{
			TimedOut(150.0, TEXT("the title menu came up"));
			return;
		}
		Check(P->TutorialTestPressLabel(TEXT("New Game")), TEXT("New Game pressed on the title menu"));
		Next(1, 1.0);
		return;
	case 1:
	{
		const FString Rows = P->TutorialTestRows();
		Check(P->TutorialTestPressLabel(TEXT("Historical Missions")), FString::Printf(TEXT("Historical Missions pressed (%s)"), *Rows));
	}
		Next(2, 1.0);
		return;
	case 2:
		Check(Page == TEXT("#missions"), FString::Printf(TEXT("the Historical Missions page is up (%s)"), *Page));
		P->RedrawMenu();
		Next(3, 1.0);
		return;
	case 3:
	{
		const TArray<OutlawsMenu::FItem>& It = P->MenuPage->Items;
		UE_LOG(LogOutlaws, Display, TEXT("OLHMC the Historical Missions page: %s"), *P->TutorialTestRows());
		// the five gone, by level file (the rows' targets) and by name
		TArray<FString> FiveNames, FiveLabels;
		GetMissionChoices(FiveNames, FiveLabels);
		FString Found;
		for (const OutlawsMenu::FItem& I : It)
		{
			const FString Lv = I.Target.StartsWith(TEXT("#difficulty:")) ? I.Target.Mid(12) : I.Name;
			if (IsCareerMission(Lv) || FiveLabels.Contains(I.Label)) Found += I.Label + TEXT(" ");
		}
		Check(Found.IsEmpty(), FString::Printf(TEXT("none of the five original missions has a row (%s)"), Found.IsEmpty() ? TEXT("none") : *Found));
		// Marshal Training, then every Handful mission the LABs hold
		const int32 Mt = It.IndexOfByPredicate([](const OutlawsMenu::FItem& I) { return I.Label == TEXT("Marshal Training") && I.Target == TEXT("#difficulty:hom"); });
		const int32 Rs = It.IndexOfByPredicate([](const OutlawsMenu::FItem& I) { return I.Name == TEXT("hom_resume"); });
		Check(Mt != INDEX_NONE && Mt <= 1, FString::Printf(TEXT("Marshal Training is at the top (row %d)"), Mt));
		Check((Rs != INDEX_NONE) == HasHomSave() && (Rs == INDEX_NONE || Rs == 0), FString::Printf(TEXT("Resume Marshal Training %s (a career save %s)"), Rs != INDEX_NONE ? TEXT("on top") : TEXT("absent"), HasHomSave() ? TEXT("there") : TEXT("absent")));
		TArray<FString> HNames, HLabels;
		GetHandfulChoices(HNames, HLabels, AssetsRef.Get());
		Check(HNames.Num() == 4, FString::Printf(TEXT("the LABs hold all four Handful missions (%s)"), *FString::Join(HNames, TEXT(", "))));
		for (int32 h = 0; h < HNames.Num(); ++h)
		{
			const FString Want = TEXT("#difficulty:") + HNames[h];
			const bool bRow = It.ContainsByPredicate([&](const OutlawsMenu::FItem& I) { return I.Target == Want && I.Label == HLabels[h]; });
			Check(bRow, FString::Printf(TEXT("%s (%s) has its row"), *HLabels[h], *HNames[h]));
		}
		Check(It.Num() == HNames.Num() + 1 + (Rs != INDEX_NONE ? 1 : 0), FString::Printf(TEXT("nothing else on the page (%d rows)"), It.Num()));
		// co-op's Historical Missions list (the host's choice): the same rule
		TArray<FString> CoopNames;
		UOutlawsCoop::HistoricalLevels(this, CoopNames);
		bool bCoopFive = false;
		for (const FString& N : CoopNames) bCoopFive |= IsCareerMission(N);
		Check(CoopNames.Num() > 0 && CoopNames[0] == TEXT("office") && !bCoopFive && CoopNames.Num() == HNames.Num() + 1,
			FString::Printf(TEXT("co-op's Historical Missions list: Marshal Training (office) then the Handful, none of the five (%s)"), *FString::Join(CoopNames, TEXT(", "))));
		for (const TCHAR* F : Five) Check(UOutlawsCoop::IsMissionLevel(F), FString::Printf(TEXT("a co-op autosave in %s still continues as Historical Missions"), F));
		// Skip to level (the cheat's list) keeps every level, the five included
		TArray<FString> All, AllLabels;
		GetLevelChoices(All, AllLabels);
		int32 InSkip = 0;
		for (const TCHAR* F : Five) InSkip += All.Contains(F) ? 1 : 0;
		Check(InSkip == 5, FString::Printf(TEXT("Skip to level (OLSCORE) still lists the five (%d)"), InSkip));
		// the fit: every row drawn, above the help line (its letters from about y 1015), apart from the next
		FString Fit;
		const bool bFits = P->MenuPageFits(Fit);
		Check(bFits, FString::Printf(TEXT("the page fits the view (%s)"), *Fit));
		const TArray<FBox2D>& Ink = P->FrontItemInk;
		bool bInk = Ink.Num() == It.Num();
		FString Boxes;
		for (int32 i = 0; i < Ink.Num(); ++i)
		{
			bInk &= Ink[i].bIsValid && Ink[i].Min.X >= 0.f && Ink[i].Max.X <= 1920.f && Ink[i].Min.Y >= 300.f && Ink[i].Max.Y <= 1015.f;
			if (i > 0) bInk &= Ink[i].GetCenter().Y - Ink[i - 1].GetCenter().Y >= 100.f;      // (the letters about 60 high: apart)
			Boxes += FString::Printf(TEXT("%s%s y %.0f..%.0f"), i ? TEXT(", ") : TEXT(""), It.IsValidIndex(i) ? *It[i].Label : TEXT("?"), Ink[i].Min.Y, Ink[i].Max.Y);
		}
		Check(bInk, FString::Printf(TEXT("every row drawn under the title, above the help line (y 1015) and 100 or more apart (%s)"), *Boxes));
		if (Ink.Num() == It.Num())
		{
			const EOutlawsHand DomWas = P->DominantHand;
			const int32 OneWas = P->OneController;
			struct FCase { const TCHAR* Name; EOutlawsHand Dom; int32 One; };
			const FCase Cases[] = { { TEXT("right-handed"), EOutlawsHand::Right, 0 }, { TEXT("left-handed"), EOutlawsHand::Left, 0 },
				{ TEXT("one controller, left"), EOutlawsHand::Right, 1 }, { TEXT("one controller, right"), EOutlawsHand::Left, 2 } };
			for (const FCase& Cs : Cases)
			{
				P->DominantHand = Cs.Dom;
				P->OneController = Cs.One;
				bool bOk = true;
				FString Got;
				for (int32 i = 0; i < It.Num(); ++i)
				{
					const int32 Hit = P->FrontHitAt(Ink[i].GetCenter());
					bOk &= Hit == i;
					Got += FString::Printf(TEXT("%s%d"), i ? TEXT(",") : TEXT(""), Hit);
				}
				Check(bOk, FString::Printf(TEXT("%s: each row's middle picks its own row (%s)"), Cs.Name, *Got));
			}
			P->DominantHand = DomWas;
			P->OneController = OneWas;
			if (P->Camera && P->MenuRoot && P->bWideSheet)
			{
				const FTransform Panel = P->MenuRoot->GetComponentTransform();
				const FVector Eye = P->Camera->GetComponentLocation();
				bool bOk = true;
				FString Got;
				for (int32 i = 0; i < It.Num(); ++i)
				{
					const FVector2D C = Ink[i].GetCenter();
					const FVector At = Panel.TransformPosition(FVector(0.0, (0.5 - C.X / 1920.0) * P->SheetW, P->SheetH * 0.5 - C.Y / 1080.0 * P->SheetH));
					float Len = 0.f;
					const int32 Hit = P->MenuRowUnderRay(Eye, (At - Eye).GetSafeNormal(), Len);
					bOk &= Hit == i;
					Got += FString::Printf(TEXT("%s%d"), i ? TEXT(",") : TEXT(""), Hit);
				}
				Check(bOk, FString::Printf(TEXT("desktop: the view's ray to each row's middle finds it (%s)"), *Got));
			}
		}
		Shot(Mode == TEXT("fit") ? FString(TEXT("missions_fit")) : FString(TEXT("missions")));
		if (Mode == TEXT("fit")) { Next(99, 1.5); return; }
		Next(4, 1.5);
		return;
	}
	case 4:
		Check(P->TutorialTestPressLabel(TEXT("Marshal Training")), TEXT("Marshal Training pressed"));
		Next(5, 1.0);
		return;
	case 5:
		Check(Page == TEXT("#difficulty:hom"), FString::Printf(TEXT("its difficulty page (%s)"), *Page));
		Check(P->TutorialTestPressLabel(TEXT("Good")), TEXT("Good pressed: Marshal Training starts"));
		Next(6, 2.0);
		return;
	case 6:
		// the crawl (skipped), then the office
		if (Movie) { Movie->Skip(); T.At = Now + 1.0; return; }
		if (!Built() || !LevelName.Equals(TEXT("office"), ESearchCase::IgnoreCase))
		{
			TimedOut(120.0, TEXT("Marshal Training's office built"));
			return;
		}
		Check(bHom, FString::Printf(TEXT("Marshal Training starts at its first level, the office, as a Historical Missions session (hom %d)"), bHom ? 1 : 0));
		{
			FString OfficeInf;
			int32 Posters = 0;
			if (AssetsRef && AssetsRef->GetText(TEXT("office.inf"), OfficeInf))
			{
				for (const TCHAR* F : Five) Posters += OfficeInf.Contains(FString::Printf(TEXT("SPAWN_LEVEL %s"), F), ESearchCase::IgnoreCase) ? 1 : 0;
			}
			Check(Posters == 5, FString::Printf(TEXT("the office's posters ride to all five (%d of 5 SPAWN_LEVEL lines)"), Posters));
		}
		Shot(TEXT("office"));
		Next(7, 2.0);
		return;
	case 7:
		HomSpawnLevel(TEXT("SHFORT PORCH1D"));
		if (P->bMenuOpen) P->CloseMenu();          // (the "Ride out?" page: ridden at once below, as its first row does)
		if (!LevelName.Equals(TEXT("shfort"), ESearchCase::IgnoreCase)) HomRide();
		Next(8, 2.0);
		return;
	case 8:
		if (!Built() || !LevelName.Equals(TEXT("shfort"), ESearchCase::IgnoreCase))
		{
			TimedOut(120.0, TEXT("the Fort built from its poster"));
			return;
		}
		Check(bHom && HomMission == TEXT("shfort"), FString::Printf(TEXT("The Fort reached through Marshal Training (hom %d, mission %s, title %s)"), bHom ? 1 : 0, *HomMission, *HomMissionLabel()));
		Next(9, 3.0);
		return;
	case 9:
		Check(SaveGame(8), TEXT("a save in the Fort (slot 8, Marshal Training's)"));
		// the old way in: the Granary started on its own, as its row on the page did before
		StartMission(TEXT("shgranry"), EOutlawsDifficulty::Good);
		Next(10, 2.0);
		return;
	case 10:
		if (!Built() || !LevelName.Equals(TEXT("shgranry"), ESearchCase::IgnoreCase))
		{
			TimedOut(120.0, TEXT("the Granary built on its own"));
			return;
		}
		Next(11, 3.0);
		return;
	case 11:
		Check(!bHom && SaveGame(9), TEXT("an old-style save in the Granary (slot 9: the single mission, no career)"));
		ReturnToFrontEnd();
		Next(12, 4.0);
		return;
	case 12:
		if (!(bFrontEnd && !bRebuildPending && !bNeedPlace))
		{
			TimedOut(90.0, TEXT("back at the title"));
			return;
		}
		Check(LoadGame(9), TEXT("slot 9 (the Granary) loading from the title"));
		Next(13, 2.0);
		return;
	case 13:
		if (!Built() || !LevelName.Equals(TEXT("shgranry"), ESearchCase::IgnoreCase))
		{
			TimedOut(120.0, TEXT("the old Granary save loaded"));
			return;
		}
		Check(!bHom, FString::Printf(TEXT("the old save in the Granary loads (%s, hom %d)"), *LevelName, bHom ? 1 : 0));
		Shot(TEXT("old_save_granary"));
		Next(14, 3.0);
		return;
	case 14:
		Check(LoadGame(8), TEXT("slot 8 (the Fort, Marshal Training) loading"));
		Next(15, 2.0);
		return;
	case 15:
		if (!Built() || !LevelName.Equals(TEXT("shfort"), ESearchCase::IgnoreCase))
		{
			TimedOut(120.0, TEXT("the Fort's save loaded"));
			return;
		}
		Check(true, FString::Printf(TEXT("the Fort's save (made in Marshal Training) loads (%s; hom %d: a slot save holds the level, not the session, as before)"), *LevelName, bHom ? 1 : 0));
		Next(99, 2.0);
		return;
	case 99:
		Finish();
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
