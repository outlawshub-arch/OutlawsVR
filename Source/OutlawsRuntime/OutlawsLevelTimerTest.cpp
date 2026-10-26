// REQ-LEVELTIMER's checks (scratchpad gate_popin\leveltimer_checks.ps1, hidden; the store, the saves and the settings are put back
// after each run).  Single player: -OLLevel=hideout -OLNoFrontEnd -OLMpName=<profile> [-OLDifficulty=good|bad|ugly]
//   -OLTimerTest=finish     3 s of play, the clock set to -OLTimerSet=<s> (when given), the level won; at the end card the "Time", "Best
//                           time", "New best!" and "No best kept" rows are logged (OLTIMER test RESULT finish ...) and photographed
//                           (-OLTimerShot=<name>, Saved/OLShots/ol_timer_<name>.png)
//   -OLTimerTest=cheat      as finish, a cheat row (the invincibility toggle) run first: no best kept
//   -OLTimerTest=jump       as finish, the level skipped to (from itself) first: no best kept
//   -OLTimerTest=pause      the pause menu opened at 3 s of play: its time line; 10 s with it open add nothing (single player); closed,
//                           the clock goes on; opened again, the line has moved
//   -OLTimerTest=saveload   the clock set to 40 s and saved; set to 100 s; the save loaded: the clock goes on from 40 s; then the level won
// The pair (-OLCoopAuto -OLCoopTest... with -OLStatsCoopTest -OLTimerSet=<s>) is TickCoopTest's: the host's clock is set before the
// win, the client's own clock is set to something else, and both cards are logged.
#include "OutlawsStats.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsCoop.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "HAL/PlatformMisc.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

void UOutlawsStats::TimerCheck(bool bOk, const FString& What)
{
	UE_LOG(LogOutlaws, Display, TEXT("OLTIMER test check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
	if (bOk) ++TimerPasses; else ++TimerFails;
}

void UOutlawsStats::TickTimerTest(double Now)
{
	FString Mode;
	OLFlags::Value(TEXT("OLTimerTest"), Mode);
	if (Mode.IsEmpty() || TimerStep >= 99) return;
	AOutlawsLevelActor* L = FindLevel();
	AOutlawsPlayer* P = LocalPlayer();
	if (!L || !P) return;
	const bool bReady = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && Level.Equals(L->LevelName, ESearchCase::IgnoreCase) && !L->IsPlayerDead() && !L->IsMoviePlaying();
	auto Done = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLTIMER test %s RESULT %s (%d passed, %d failed)"), *Mode, TimerFails == 0 && TimerPasses > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), TimerPasses, TimerFails);
		TimerStep = 99;
		FGenericPlatformMisc::RequestExit(false);
	};
	auto Shot = [&](const TCHAR* Default)
	{
		FString Name = Default;
		OLFlags::Value(TEXT("OLTimerShot"), Name);
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("ol_timer_%s.png"), *Name), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLTIMER test: screenshot ol_timer_%s.png"), *Name);
	};
	switch (TimerStep)
	{
	case 0:
		if (!bReady) return;
		TimerAt = Now + 3.0;
		TimerStep = 1;
		UE_LOG(LogOutlaws, Display, TEXT("OLTIMER test %s: %s is up (difficulty %s)"), *Mode, *L->LevelName, OutlawsDifficulty::Name(L->Difficulty));
		return;
	case 1:
	{
		if (Now < TimerAt) return;
		if (Mode == TEXT("pause"))
		{
			TimerT0 = SecondsF;
			P->CoopOpenMenu();
			TimerStep = 30;
			TimerAt = Now + 1.5;
			return;
		}
		if (Mode == TEXT("saveload"))
		{
			SecondsF = 40.f;
			Mine.Seconds = 40;
			const bool bSaved = L->SaveGame(2);
			TimerCheck(bSaved, FString::Printf(TEXT("saved at %.1f s into slot 2"), SecondsF));
			TimerAt = Now + 2.0;
			TimerStep = 40;
			return;
		}
		if (Mode == TEXT("cheat"))
		{
			NoteCheat(L, TEXT("cheat_health"), L->bInvincible, false, L->bEnemiesActive);       // (a row that is no toggle)
		}
		if (Mode == TEXT("jump"))
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLTIMER test: skipping to %s (from itself)"), *L->LevelName);
			const FString Same = L->LevelName;
			L->JumpToLevel(Same);
			TimerMark = 0.0;
			TimerStep = 2;
			return;
		}
		TimerStep = 20;
		return;
	}
	case 2:
		// (jump: the rebuilt level up, then 3 s more)
		if (!bReady) return;
		if (TimerMark == 0.0) { TimerMark = Now + 3.0; return; }
		if (Now >= TimerMark) TimerStep = 20;
		return;
	case 20:
	{
		float Set = -1.f;
		if (OLFlags::Value(TEXT("OLTimerSet"), Set))
		{
			SecondsF = Set;
			Mine.Seconds = FMath::FloorToInt(Set);
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLTIMER test: the level is won with the clock at %d s (cheated %d, skipped %d)"), Mine.Seconds, bCheated ? 1 : 0, bSkipped ? 1 : 0);
		L->CompleteLevel(TEXT("the timer test"));
		TimerStep = 21;
		return;
	}
	case 21:
		if (!L->IsMoviePlaying()) return;
		TimerMark = Now;
		TimerStep = 22;
		return;
	case 22:
	{
		if (Now < TimerMark + 4.0) return;
		Shot(TEXT("card"));
		TArray<FOutlawsStatsPanel> Ps;
		bool bTwo = false;
		Panels(L, Ps, bTwo);
		FString Time = TEXT("?"), Best = TEXT("none"), New = TEXT("no"), Why = TEXT("none");
		for (const FOutlawsStatsPanel& Pn : Ps)
		{
			FString Rows;
			for (const TPair<FString, FString>& R : Pn.Rows)
			{
				Rows += FString::Printf(TEXT("%s%s %s"), Rows.IsEmpty() ? TEXT("") : TEXT(", "), *R.Key, *R.Value);
				if (&Pn != &Ps[0]) continue;
				if (R.Key == TEXT("Time")) Time = R.Value;
				else if (R.Key == TEXT("Best time")) Best = R.Value;
				else if (R.Key == TEXT("New best!")) New = R.Value;
				else if (R.Key == TEXT("No best kept")) Why = R.Value;
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLTIMER test panel: [%s] %s"), *Pn.Name, *Rows);
		}
		FOutlawsStatsCard Card;
		if (HandCard(L, Card)) UE_LOG(LogOutlaws, Display, TEXT("OLTIMER test hand card: %s"), *Card.Lines());
		UE_LOG(LogOutlaws, Display, TEXT("OLTIMER test RESULT %s time=%s best=%s new=%s why=%s"), *Mode, *Time, *Best, *New, *Why);
		TimerStep = 23;
		return;
	}
	case 23:
		if (Now < TimerMark + 6.0) return;
		TimerPasses = 1;
		Done();
		return;
	case 30:
	{
		// pause: the menu is up
		if (Now < TimerAt) return;
		const FString Line = PauseLine(L);
		TimerCheck(P->IsMenuOpen() && Line.StartsWith(TEXT("Time ")), FString::Printf(TEXT("the pause menu is open with a time line (%s)"), *Line));
		Shot(TEXT("pause"));
		TimerMark = Now;
		TimerStep = 31;
		return;
	}
	case 31:
	{
		if (Now < TimerMark + 10.0) return;
		const float Delta = SecondsF - TimerT0;
		TimerCheck(Delta < 0.5f, FString::Printf(TEXT("11.5 s with the menu open added %.3f s to the clock"), Delta));
		P->CloseGameMenu();
		TimerT0 = SecondsF;
		TimerMark = Now;
		TimerStep = 32;
		return;
	}
	case 32:
	{
		if (Now < TimerMark + 4.0) return;
		const float Delta = SecondsF - TimerT0;
		TimerCheck(Delta > 3.0f && Delta < 5.0f, FString::Printf(TEXT("4 s of play after closing it added %.2f s"), Delta));
		P->CoopOpenMenu();
		TimerStep = 33;
		TimerMark = Now;
		return;
	}
	case 33:
	{
		if (Now < TimerMark + 1.0) return;
		const FString Line = PauseLine(L);
		const int32 Secs = FMath::FloorToInt(SecondsF);
		TimerCheck(Line == TEXT("Time ") + Clock(Secs) && Secs >= FMath::FloorToInt(TimerT0 + 3.0f), FString::Printf(TEXT("the line moved on and reads the clock (%s, clock %.1f s)"), *Line, SecondsF));
		P->CloseGameMenu();
		Done();
		return;
	}
	case 40:
	{
		// saveload: the clock set well past the save, then the save loaded
		if (Now < TimerAt) return;
		SecondsF = 100.f;
		Mine.Seconds = 100;
		LoadsAtTest = LoadsApplied;
		L->LoadGame(2);
		TimerMark = Now;
		TimerStep = 41;
		return;
	}
	case 41:
	{
		// (the rebuild runs, then the load is applied: the clock is the save's, from 40)
		if (LoadsApplied == LoadsAtTest)
		{
			if (Now > TimerMark + 90.0) { TimerCheck(false, TEXT("the load was applied within 90 s")); Done(); }
			return;
		}
		if (Now < TimerMark + 1.0 || !bReady) return;
		TimerCheck(SecondsF >= 40.f && SecondsF < 46.f, FString::Printf(TEXT("after the load the clock goes on from the save's 40 s (now %.1f s)"), SecondsF));
		TimerT0 = SecondsF;
		TimerMark = Now;
		TimerStep = 42;
		return;
	}
	case 42:
	{
		if (Now < TimerMark + 4.0) return;
		const float Delta = SecondsF - TimerT0;
		TimerCheck(Delta > 3.0f && Delta < 5.0f, FString::Printf(TEXT("4 s later the clock has added %.2f s (now %.1f s)"), Delta, SecondsF));
		TimerStep = 20;                // (the level won with no clock set: the finish is the save's 40 s and the play since)
		return;
	}
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
