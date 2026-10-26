// REQ-UNLOCKS' checks (scratchpad unlocks_checks.ps1, hidden; the store file is moved aside after each run by the save restore),
// made over for UNLOCKFREE (user decision for 0.95): the Randomizer and the Bad guys multiplier are open from the first start.
//
// Single player: -OLLevel=ranch -OLNoFrontEnd -OLUnlockTest=<mode> [-OLMpName=<profile>] [-OLUnlockSkipAt=<s>] [-OLUnlockShots]
//   fresh    a fresh profile (no achievements.json): both modes open, both rows on Options > Gameplay to use (none greyed), both
//            off by default, the saved settings used; the Cheats page has no Unlock all modes; then both turned on from Options
//            (the Bad guys row, the Randomizer page's Enemies row) and taken by the next run; no store written
//   good     as fresh, then the story's last level won on Good: nothing earned before its cutscene ends; after it the main
//            menu opens as ever, with no unlock page (nothing unlocks now); the achievement earned all the same
//   ugly     the same on Ugly: the Ugly achievements earned too, still no unlock page
//   toggle   an old store (the script writes it: unlockAllModes on, the story earned, both old unlock pages still queued): read as
//            before, not damaged, both open, the old pages never shown, nothing written
//   damaged  (the script writes a broken store first) noticed, no crash, both open all the same; then as good (the broken file
//            kept aside)
// Co-op: -OLCoopAuto -OLCoopTest=unlock -OLCoopLevel=ranch, each instance with -OLMpName (UOutlawsCoop::TickUnlockTest below).
#include "OutlawsUnlocks.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsCoop.h"
#include "OutlawsAchievements.h"
#include "OutlawsMenuDoc.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

namespace
{
	struct FUnlockTest
	{
		FString Mode;
		int32 Step = 0;
		double At = 0.0, MovieSince = 0.0;
		int32 Passes = 0, Fails = 0;
		bool bSkipped = false, bNotedEarly = false;
		int32 Pages = 0;
	};
	FUnlockTest GTest;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLUNLOCK test check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++GTest.Passes; else ++GTest.Fails;
	}

	// the page File (built as the menu builds it) has a row matching Pred
	bool PageHas(AOutlawsPlayer* P, const FString& File, TFunctionRef<bool(const OutlawsMenu::FItem&)> Pred)
	{
		if (!P->IsMenuOpen()) P->CoopOpenMenu();
		if (!P->TestShowPage(File)) { UE_LOG(LogOutlaws, Display, TEXT("OLUNLOCK test: page %s could not be shown"), *File); return false; }
		const OutlawsMenu::FPage* Pg = P->TestMenuPage();
		return Pg && Pg->Items.ContainsByPredicate(Pred);
	}
	// (REQ-RANDOMOVE) the Randomizer row is on Options > Gameplay, at its bottom; the Options page never has it
	bool OptionsHasRando(AOutlawsPlayer* P) { return PageHas(P, TEXT("options.rml"), [](const OutlawsMenu::FItem& I) { return I.Target == TEXT("#rando"); }); }
	bool GameplayHasRando(AOutlawsPlayer* P)
	{
		// (REQ-EXTRAS: a locked one is there greyed, with what unlocks it: not a row to use)
		if (!PageHas(P, TEXT("options_gameplay.rml"), [](const OutlawsMenu::FItem& I) { return I.Target == TEXT("#rando") && !I.Classes.Contains(TEXT("disabled")); })) return false;
		const OutlawsMenu::FPage* Pg = P->TestMenuPage();
		int32 Last = Pg->Items.Num() - 1;
		while (Last > 0 && Pg->Items[Last].bGoBack) --Last;
		return Pg->Items[Last].Target == TEXT("#rando");
	}
	// UNLOCKFREE: nothing greyed on Options > Gameplay (the "finish the story" rows gone)
	bool GameplayShowsNoLocked(AOutlawsPlayer* P)
	{
		return !PageHas(P, TEXT("options_gameplay.rml"), [](const OutlawsMenu::FItem& I) { return I.Classes.Contains(TEXT("disabled")) || I.Name.StartsWith(TEXT("locked_")) || I.Label.Contains(TEXT("finish the story")); });
	}
	// both off by default, as a first start's settings are (the level's own defaults)
	bool DefaultsOff()
	{
		const AOutlawsLevelActor* D = GetDefault<AOutlawsLevelActor>();
		return D->RandoEnemies == 0 && D->RandoItems == 0 && D->RandoOrder == 0 && D->RandoBosses == 0 && D->RandoCritters == 0 && FMath::IsNearlyEqual(D->SoloBadGuys, 1.f) && D->CoopBadGuys <= 0.f;
	}
	// the walk a player takes: the pause page, Options, Gameplay, Randomizer (the same page as before), Back to Gameplay
	void RandoWalk(AOutlawsPlayer* P, bool bUnlocked)
	{
		P->CloseGameMenu();
		P->CoopOpenMenu();
		const bool bOpt = P->TestPressLink(TEXT("options.rml")) && P->CoopMenuPage() == TEXT("options.rml");
		Check(bOpt, FString::Printf(TEXT("walk: the pause page's Options opens Options (%s)"), *P->CoopMenuPage()));
		const OutlawsMenu::FPage* Op = P->TestMenuPage();
		Check(Op && !Op->Items.ContainsByPredicate([](const OutlawsMenu::FItem& I) { return I.Target == TEXT("#rando"); }), TEXT("walk: Options has no Randomizer row"));
		const bool bGp = P->TestPressLink(TEXT("options_gameplay.rml")) && P->CoopMenuPage() == TEXT("options_gameplay.rml");
		Check(bGp, FString::Printf(TEXT("walk: Options > Gameplay opens (%s)"), *P->CoopMenuPage()));
		const bool bPressed = P->TestPressLink(TEXT("#rando"));
		if (!bUnlocked)
		{
			Check(P->CoopMenuPage() == TEXT("options_gameplay.rml"), FString::Printf(TEXT("walk: locked, Gameplay's greyed Randomizer row does nothing (%s)"), bPressed ? TEXT("pressed") : TEXT("not there")));
		}
		else
		{
			Check(bPressed && P->CoopMenuPage() == TEXT("#rando"), FString::Printf(TEXT("walk: Gameplay > Randomizer opens the Randomizer page (%s)"), *P->CoopMenuPage()));
			const OutlawsMenu::FPage* Pg = P->TestMenuPage();
			Check(Pg && Pg->Title == TEXT("RANDOMIZER"), FString::Printf(TEXT("walk: the same page as before (title %s)"), Pg ? *Pg->Title : TEXT("none")));
			P->CoopMenuBack();
			Check(P->CoopMenuPage() == TEXT("options_gameplay.rml"), FString::Printf(TEXT("walk: Back from the Randomizer returns to Gameplay (%s)"), *P->CoopMenuPage()));
		}
		P->CoopMenuBack();
		Check(P->CoopMenuPage() == TEXT("options.rml"), FString::Printf(TEXT("walk: Back from Gameplay returns to Options (%s)"), *P->CoopMenuPage()));
		P->CloseGameMenu();
	}
	bool GameplayHasBadGuys(AOutlawsPlayer* P) { return PageHas(P, TEXT("options_gameplay.rml"), [](const OutlawsMenu::FItem& I) { return I.Name == TEXT("solo_badguys"); }); }

	// what a New Game and the next build would take from the settings (the page's choices on: 2 / x2), the level's own put back
	void Effective(AOutlawsLevelActor* L, int32& OutEnemies, float& OutSolo)
	{
		const AOutlawsLevelActor::FRandoRun Was = L->RandoRun;
		const int32 WasEnemies = L->RandoEnemies;
		const float WasSolo = L->SoloBadGuys;
		L->RandoEnemies = 2;
		L->SoloBadGuys = 2.f;
		L->BeginRandoRun(TEXT("unlock test"));
		OutEnemies = L->RandoRun.Enemies;
		OutSolo = L->SoloBadGuysWanted();
		L->RandoRun = Was;
		L->RandoEnemies = WasEnemies;
		L->SoloBadGuys = WasSolo;
	}

	FString FirstHeading(const AOutlawsPlayer* P)
	{
		const OutlawsMenu::FPage* Pg = P->TestMenuPage();
		if (!Pg) return FString();
		for (const OutlawsMenu::FItem& I : Pg->Items) if (I.Kind == OutlawsMenu::EItemKind::Heading) return I.Label;
		return FString();
	}

	void Finish()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLUNLOCK test %s RESULT %s (%d passed, %d failed)"), *GTest.Mode, GTest.Fails == 0 && GTest.Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), GTest.Passes, GTest.Fails);
		GTest.Step = 99;
		FGenericPlatformMisc::RequestExit(false);
	}
}

void OutlawsUnlocks::TickTest(AOutlawsLevelActor* L, float Dt)
{
	FUnlockTest& T = GTest;
	if (T.Step >= 99 || !L) return;
	if (T.Mode.IsEmpty()) OLFlags::Value(TEXT("OLUnlockTest"), T.Mode);
	UWorld* W = L->GetWorld();
	AOutlawsPlayer* P = W && W->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(W->GetFirstPlayerController()->GetPawn()) : nullptr;
	if (!P) return;
	const double Now = FPlatformTime::Seconds();
	const bool bUgly = T.Mode == TEXT("ugly");
	const bool bFinish = T.Mode == TEXT("good") || bUgly || T.Mode == TEXT("damaged");
	switch (T.Step)
	{
	case 0:
		if (L->IsFrontEnd() || !L->LevelName.Equals(TEXT("ranch"), ESearchCase::IgnoreCase)) return;
		if (T.At == 0.0) { T.At = Now + 5.0; return; }
		if (Now < T.At) return;
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLUNLOCK test %s: profile %s, store %s (%s)"), *T.Mode, *ProfileOf(L), *StorePath(), IFileManager::Get().FileExists(*StorePath()) ? TEXT("there") : TEXT("none"));
			if (T.Mode == TEXT("damaged")) Check(StoreDamaged(), TEXT("the damaged store is noticed"));
			Check(IsUnlocked(L, EMode::Randomizer) && IsUnlocked(L, EMode::BadGuys), TEXT("both modes open at the start"));
			Check(GameplayHasRando(P) && !OptionsHasRando(P), TEXT("Options > Gameplay has the Randomizer row to use, at its bottom (Options has none)"));
			Check(GameplayHasBadGuys(P), TEXT("Options > Gameplay has the Bad guys row to use"));
			Check(GameplayShowsNoLocked(P), TEXT("Options > Gameplay shows nothing greyed and no 'finish the story'"));
			RandoWalk(P, true);
			Check(!PageHas(P, TEXT("#cheats"), [](const OutlawsMenu::FItem& I) { return I.Name == TEXT("cheat_unlockmodes"); }), TEXT("the Cheats page has no Unlock all modes row"));
			Check(DefaultsOff(), TEXT("both off by default (Randomizer all Off, Bad guys x1, co-op Auto)"));
			int32 En = -1; float Solo = -1.f;
			Effective(L, En, Solo);
			Check(En == 2 && FMath::IsNearlyEqual(Solo, 2.f), FString::Printf(TEXT("the saved settings are used (Randomizer enemies %d, bad guys x%.2f)"), En, Solo));
			P->CloseGameMenu();
			if (T.Mode == TEXT("fresh"))
			{
				// both turned on from Options the way a player does: Gameplay's Bad guys row, then the Randomizer page's Enemies row
				const int32 WasEn = L->RandoEnemies;
				const float WasSolo = L->SoloBadGuys;
				const AOutlawsLevelActor::FRandoRun WasRun = L->RandoRun;
				L->RandoEnemies = 0;
				L->SoloBadGuys = 1.f;
				P->CoopOpenMenu();
				const bool bGp = P->TestPressLink(TEXT("options.rml")) && P->TestPressLink(TEXT("options_gameplay.rml")) && P->CoopMenuPage() == TEXT("options_gameplay.rml");
				const bool bBg = bGp && P->CoopPressRow(TEXT("solo_badguys"));
				Check(bBg && L->SoloBadGuys > 1.001f && L->SoloBadGuysWanted() > 1.001f, FString::Printf(TEXT("fresh profile: Bad guys turned on from Options > Gameplay (x%.2f, the next level takes x%.2f)"), L->SoloBadGuys, L->SoloBadGuysWanted()));
				const bool bRd = P->TestPressLink(TEXT("#rando")) && P->CoopMenuPage() == TEXT("#rando") && P->CoopPressRow(TEXT("rando_enemies"));
				L->BeginRandoRun(TEXT("unlock test"));
				Check(bRd && L->RandoEnemies != 0 && L->RandoRun.Enemies == L->RandoEnemies, FString::Printf(TEXT("fresh profile: the Randomizer turned on from Options > Gameplay > Randomizer (enemies %d, the run takes %d)"), L->RandoEnemies, L->RandoRun.Enemies));
				P->CloseGameMenu();
				L->RandoEnemies = WasEn;
				L->SoloBadGuys = WasSolo;
				L->RandoRun = WasRun;
				Check(!IFileManager::Get().FileExists(*StorePath()), TEXT("nothing was written to the store for a fresh profile"));
				Finish();
				return;
			}
			if (T.Mode == TEXT("toggle"))
			{
				// the script's old store: read as before, its queued pages never shown
				FString Id, Title, Where;
				Check(!StoreDamaged() && IsEarned(L, StoryAny), TEXT("old store: read as before (not damaged, the story earned)"));
				Check(UnlockAll(), TEXT("old store: its unlockAllModes is read as before (it changes nothing now)"));
				Check(!NextNotice(L, Id, Title, Where), TEXT("old store: its queued unlock pages are not shown"));
				P->CoopOpenMenu();
				Check(P->TestShowPage(TEXT("#front")) && P->CoopMenuPage() == TEXT("#front"), FString::Printf(TEXT("old store: the main menu opens as itself, no unlock page (%s)"), *P->CoopMenuPage()));
				P->CloseGameMenu();
				Finish();
				return;
			}
			if (!bFinish) { Check(false, TEXT("an unknown mode")); Finish(); return; }
			L->Difficulty = bUgly ? EOutlawsDifficulty::Ugly : EOutlawsDifficulty::Good;
			L->CompleteLevel(TEXT("unlock test"));
			Check(L->UnlockStoryPending == int32(L->Difficulty), FString::Printf(TEXT("the story's last level won on %s is noted (%d)"), OutlawsDifficulty::Name(L->Difficulty), L->UnlockStoryPending));
			Check(!IsEarned(L, StoryAny), TEXT("nothing earned before the cutscene ends"));
			T.Step = 1;
			T.At = Now + 180.0;
			return;
		}
	case 1:
	{
		// the final movie (and the credits, where the build has them): skipped at -OLUnlockSkipAt (2.5 s), then the main menu
		static float SkipAt = -1.f;
		if (SkipAt < 0.f) { SkipAt = 2.5f; OLFlags::Value(TEXT("OLUnlockSkipAt"), SkipAt); }
		if (L->IsMoviePlaying())
		{
			if (T.MovieSince == 0.0) { T.MovieSince = Now; UE_LOG(LogOutlaws, Display, TEXT("OLUNLOCK test: the final movie is playing")); }
			if (!T.bNotedEarly && Now - T.MovieSince > 1.0) { T.bNotedEarly = true; Check(!IsEarned(L, StoryAny), TEXT("not earned while the final movie plays")); }
			if (!T.bSkipped && Now - T.MovieSince >= SkipAt && L->CanSkipMovie())
			{
				T.bSkipped = true;
				UE_LOG(LogOutlaws, Display, TEXT("OLUNLOCK test: the final movie skipped at %.1f s"), Now - T.MovieSince);
				L->SkipMovie();
			}
		}
		if (P->IsFrontMenuUp())
		{
			// (-OLUnlockShots: the page photographed first, Saved/OLShots/ol_unlock_<mode>.png)
			static bool bShot = false;
			if (!bShot && OLFlags::Param(TEXT("OLUnlockShots")))
			{
				bShot = true;
				T.At = Now + 2.0;
				FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("ol_unlock_%s.png"), *T.Mode), false, false);
				return;
			}
			if (bShot && Now < T.At) return;
			Check(T.MovieSince > 0.0, TEXT("the final movie played before the main menu"));
			Check(IsEarned(L, StoryAny), TEXT("the Randomizer's achievement is earned"));
			Check(IsEarned(L, StoryUgly) == bUgly, FString::Printf(TEXT("the Ugly achievement %s"), bUgly ? TEXT("is earned") : TEXT("is not earned")));
			Check(OutlawsAchievements::IsEarned(L, TEXT("story_bad")) == bUgly, FString::Printf(TEXT("Bad to the end %s"), bUgly ? TEXT("is earned") : TEXT("is not earned")));
			Check(P->CoopMenuPage() == TEXT("#front"), FString::Printf(TEXT("the main menu opens with no unlock page after the ending (%s: %s)"), *P->CoopMenuPage(), *FirstHeading(P)));
			T.Step = 2;
			T.At = Now + 2.0;
			return;
		}
		if (Now > T.At) { Check(false, TEXT("the main menu came up after the ending")); Finish(); }
		return;
	}
	case 2:
	{
		if (Now < T.At) return;
		FString Id, Title, Where;
		Check(!NextNotice(L, Id, Title, Where), TEXT("no unlock page to show"));
		Check(P->TestShowPage(TEXT("#front")) && P->CoopMenuPage() == TEXT("#front"), FString::Printf(TEXT("the main menu again: still no page (%s)"), *P->CoopMenuPage()));
		Check(GameplayHasRando(P) && !OptionsHasRando(P) && GameplayHasBadGuys(P), TEXT("Options > Gameplay still has both rows, the Randomizer at its bottom"));
		int32 En = -1; float Solo = -1.f;
		Effective(L, En, Solo);
		Check(En == 2 && FMath::IsNearlyEqual(Solo, 2.f), FString::Printf(TEXT("the saved settings are used as before (Randomizer enemies %d, bad guys x%.2f)"), En, Solo));
		P->TestShowPage(TEXT("#front"));
		Finish();
		return;
	}
	default:
		return;
	}
}

void UOutlawsCoop::TickUnlockTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	const bool bHost = IsHost();
	const TCHAR* Me = bHost ? TEXT("host") : TEXT("client");
	auto Check = [this, Me](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLCOOP unlock check %s (%s): %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), Me, *What);
		if (bOk) ++UlPasses; else ++UlFails;
	};
	switch (UlStep)
	{
	case 0:
		if (bHost)
		{
			const AOutlawsCoopLink* CL = GetLink(PartnerIndex());
			if (!CL || CL->BuiltSerial != StartedSerial || !PartnerPuppet()) return;
		}
		if (UlAt == 0.0) { UlAt = Now + 4.0; return; }
		if (Now < UlAt) return;
		UE_LOG(LogOutlaws, Log, TEXT("OLCOOP unlock: the %s's profile is %s"), Me, *OutlawsUnlocks::ProfileOf(L));
		Check(OutlawsUnlocks::IsUnlocked(L, OutlawsUnlocks::EMode::Randomizer) && OutlawsUnlocks::IsUnlocked(L, OutlawsUnlocks::EMode::BadGuys), TEXT("both modes open at the start (UNLOCKFREE)"));
		if (bHost)
		{
			P->CoopOpenMenu();
			P->TestShowPage(TEXT("#coophostopts"));
			const OutlawsMenu::FPage* Pg = P->TestMenuPage();
			Check(Pg && Pg->File == TEXT("#coophostopts") && Pg->Items.Num() > 0, TEXT("the lobby's Options page built"));
			Check(Pg && Pg->Items.ContainsByPredicate([](const OutlawsMenu::FItem& I) { return I.Name == TEXT("coop:badguys"); })
				&& Pg->Items.ContainsByPredicate([](const OutlawsMenu::FItem& I) { return I.Target == TEXT("#rando") && !I.Classes.Contains(TEXT("disabled")); }),
				TEXT("the lobby's Options have the Bad guys row and the Randomizer to use"));
			Check(Pg && !Pg->Items.ContainsByPredicate([](const OutlawsMenu::FItem& I) { return I.Classes.Contains(TEXT("disabled")) || I.Name == TEXT("coop:locked_badguys") || I.Label.Contains(TEXT("finish the story")); }),
				TEXT("the lobby's Options show nothing greyed"));
			// a fresh host turns both on from the lobby's Options: Bad guys stepped off Auto, the Randomizer page's Enemies row
			const float Was = L->CoopBadGuys;
			const int32 WasEn = L->RandoEnemies;
			const AOutlawsLevelActor::FRandoRun WasRun = L->RandoRun;
			L->CoopBadGuys = 0.f;
			L->RandoEnemies = 0;
			Check(IsBadGuysAuto(), FString::Printf(TEXT("Bad guys start on Auto (x%.2f)"), BadGuysMultiplier()));
			const bool bB1 = P->CoopPressRow(TEXT("coop:badguys"));
			P->TestShowPage(TEXT("#coophostopts"));
			const bool bB2 = P->CoopPressRow(TEXT("coop:badguys"));
			Check(bB1 && bB2 && !IsBadGuysAuto() && BadGuysMultiplier() > 1.001f, FString::Printf(TEXT("the host turned Bad guys on from the lobby's Options (x%.2f)"), BadGuysMultiplier()));
			P->TestShowPage(TEXT("#coophostopts"));
			const bool bRd = P->TestPressLink(TEXT("#rando")) && P->CoopMenuPage() == TEXT("#rando") && P->CoopPressRow(TEXT("rando_enemies"));
			L->BeginRandoRun(TEXT("co-op unlock test"));
			Check(bRd && L->RandoEnemies != 0 && L->RandoRun.Enemies == L->RandoEnemies, FString::Printf(TEXT("the host turned the Randomizer on from the lobby's Options (enemies %d, the run takes %d)"), L->RandoEnemies, L->RandoRun.Enemies));
			L->CoopBadGuys = 3.f;
			Check(!IsBadGuysAuto() && FMath::IsNearlyEqual(BadGuysMultiplier(), 3.f), FString::Printf(TEXT("a saved x3 is used as it is (x%.2f)"), BadGuysMultiplier()));
			L->CoopBadGuys = Was;
			L->RandoEnemies = WasEn;
			L->RandoRun = WasRun;
			P->CloseGameMenu();
			L->CompleteLevel(TEXT("co-op unlock test"));
			UE_LOG(LogOutlaws, Log, TEXT("OLCOOP unlock: the host wins the story's last level"));
		}
		UlAt = Now + 60.0;
		UlStep = 1;
		return;
	case 1:
		// each machine: the win noted, the final movie skipped once it may be; the ending and the session's end do the rest
		if (L->IsMoviePlaying())
		{
			Check(L->UnlockStoryPending >= 0 && !OutlawsUnlocks::IsEarned(L, OutlawsUnlocks::StoryAny), FString::Printf(TEXT("the story's last level won is noted, nothing earned yet (%d)"), L->UnlockStoryPending));
			UlAt = Now + 2.5;
			UlStep = 2;
			return;
		}
		if (Now > UlAt) { Check(false, TEXT("the final movie started")); UlStep = 9; }
		return;
	case 2:
		// (RC5) the client does not skip: the host's skip ends the story's last movie for both, the session with it, so a
		// client's own skip 2.5 s into its movie raced the host's quit and lost when its movie began a few frames later
		// (EndUnlockTestAtSessionEnd gives the client's RESULT)
		if (!bHost) return;
		if (Now < UlAt || !L->CanSkipMovie()) return;
		UE_LOG(LogOutlaws, Log, TEXT("OLCOOP unlock: the %s skips the final movie"), Me);
		L->SkipMovie();
		UE_LOG(LogOutlaws, Log, TEXT("OLCOOP unlock %s RESULT %s (%d passed, %d failed)"), Me, UlFails == 0 && UlPasses > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), UlPasses, UlFails);
		UlStep = 3;
		return;
	case 9:
		UE_LOG(LogOutlaws, Log, TEXT("OLCOOP unlock %s RESULT FAIL (%d passed, %d failed)"), Me, UlPasses, UlFails);
		UlStep = 10;
		Summary(TEXT("unlock test failed"));
		QuitForTest();
		return;
	default:
		return;
	}
}

void UOutlawsCoop::EndUnlockTestAtSessionEnd(AOutlawsLevelActor* L)
{
	if (IsHost() || UlStep != 2 || !L) return;
	auto Check = [this](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLCOOP unlock check %s (client): %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++UlPasses; else ++UlFails;
	};
	// the host's quit (QuitForTest has earned what was pending): the final movie still playing here, the story earned
	Check(L->IsMoviePlaying(), TEXT("the final movie was still playing when the host's skip ended the session"));
	Check(OutlawsUnlocks::IsEarned(L, OutlawsUnlocks::StoryAny), TEXT("the story earned on the session's end"));
	UE_LOG(LogOutlaws, Log, TEXT("OLCOOP unlock client RESULT %s (%d passed, %d failed)"), UlFails == 0 && UlPasses > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), UlPasses, UlFails);
	UlStep = 3;
}
#endif // WITH_OUTLAWS_TESTS
