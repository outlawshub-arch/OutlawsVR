// REQ-SHIPLOG: -OLShipSmoke (see OutlawsShipSmoke.h).  The state lives here, not on the level actor: a level start may
// load L_Runtime again (BUG-RESPAWNCRASH) and bring a new actor.
#include "OutlawsShipSmoke.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "CoreGlobals.h"

namespace
{
	// the story's levels in their order (OutlawsSaveGame.cpp's list)
	const TCHAR* const SmokeLevels[] = { TEXT("hideout"), TEXT("townex"), TEXT("train"), TEXT("canyon"), TEXT("mill"), TEXT("simms"), TEXT("miner"), TEXT("cliff"), TEXT("ranch") };
	constexpr int32 NumSmokeLevels = UE_ARRAY_COUNT(SmokeLevels);
	constexpr double SmokePlaySecondsPlain = 2.0;  // each level (and the menu) ticked this long before the next
	constexpr double SmokePlaySecondsProbe = 40.0; // (FIRSTHITCH: with -OLFirstUseProbe, its two rounds of steps)
	double SmokePlaySecondsFor(bool bMenu)
	{
		static const bool bProbe = OLFlags::Param(TEXT("OLFirstUseProbe"));
		return bProbe && !bMenu ? SmokePlaySecondsProbe : SmokePlaySecondsPlain;
	}
	constexpr double SmokeMenuWait = 180.0;        // the menu up by then from the start
	constexpr double SmokeLevelWait = 180.0;       // a level built by then from its start

	enum class ESmokeStage { Menu, Level, Settle, Done };
	ESmokeStage GSmokeStage = ESmokeStage::Menu;
	int32 GSmokeIndex = -1;                  // the level being loaded or played (-1: the menu)
	double GSmokeStageAt = 0.0;              // when the menu wait (the module's start) or the level's start began
	double GSmokeSettleUntil = 0.0;
	int32 GSmokeLoaded = 0, GSmokeFailed = 0;
	double GSmokeTotalMs = 0.0;
	TWeakObjectPtr<AOutlawsLevelActor> GSmokeLevel;
	bool bSmokeTicking = false;

	const TCHAR* SmokeName(int32 I) { return I < 0 ? TEXT("main menu") : SmokeLevels[I]; }

	void SmokeFinish()
	{
		GSmokeStage = ESmokeStage::Done;
		UE_LOG(LogOutlaws, Display, TEXT("OLShipSmoke: done, %d of %d loaded, %d failed, %.0f ms loading in all"), GSmokeLoaded, NumSmokeLevels + 1, GSmokeFailed, GSmokeTotalMs);
		const bool bOk = GSmokeFailed == 0;
		// (a second on, from the core ticker: BUG-EXITCRASH)
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([bOk](float)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLShipSmoke: quitting (exit code %d)"), bOk ? 0 : 1);
			FPlatformMisc::RequestExitWithStatus(false, bOk ? 0 : 1);
			return false;
		}), 1.f);
	}

	void SmokeFail(const FString& Why)
	{
		++GSmokeFailed;
		UE_LOG(LogOutlaws, Error, TEXT("OLShipSmoke: %s FAILED: %s"), SmokeName(GSmokeIndex), *Why);
	}

	void SmokeStartNext()
	{
		++GSmokeIndex;
		// (FIRSTHITCH: -OLShipSmokeLevels=N, the first N levels only)
		static const int32 NumToRun = []() { int32 N = NumSmokeLevels; OLFlags::Value(TEXT("OLShipSmokeLevels"), N); return FMath::Clamp(N, 1, NumSmokeLevels); }();
		if (GSmokeIndex >= NumToRun)
		{
			SmokeFinish();
			return;
		}
		AOutlawsLevelActor* L = GSmokeLevel.Get();
		if (!L)
		{
			SmokeFail(TEXT("no level actor to start it"));
			SmokeFinish();
			return;
		}
		UWorld* W = L->GetWorld();
		if (AOutlawsPlayer* P = W && W->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(W->GetFirstPlayerController()->GetPawn()) : nullptr)
		{
			if (P->IsMenuOpen()) P->CloseGameMenu();
		}
		GSmokeStage = ESmokeStage::Level;
		GSmokeStageAt = FPlatformTime::Seconds();
		L->StartMission(SmokeLevels[GSmokeIndex], EOutlawsDifficulty::Bad);
	}

	bool SmokeTick(float)
	{
		const double Now = FPlatformTime::Seconds();
		switch (GSmokeStage)
		{
		case ESmokeStage::Menu:
			if (Now - GSmokeStageAt > SmokeMenuWait)
			{
				SmokeFail(FString::Printf(TEXT("not up in %.0f s"), SmokeMenuWait));
				SmokeFinish();
			}
			break;
		case ESmokeStage::Level:
			if (Now - GSmokeStageAt > SmokeLevelWait)
			{
				SmokeFail(FString::Printf(TEXT("not built in %.0f s"), SmokeLevelWait));
				SmokeStartNext();
			}
			break;
		case ESmokeStage::Settle:
			if (AOutlawsLevelActor* L = GSmokeLevel.Get()) L->StartSaveDelay = -1.f;      // (no level-start autosave)
			if (Now >= GSmokeSettleUntil) SmokeStartNext();
			break;
		case ESmokeStage::Done:
			bSmokeTicking = false;
			return false;
		}
		return true;
	}

	void SmokeEnsureTicker()
	{
		if (bSmokeTicking) return;
		bSmokeTicking = true;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&SmokeTick));
	}
}

bool OutlawsShipSmoke::Active()
{
	static const bool bOn = OLFlags::Param(TEXT("OLShipSmoke"));
	return bOn;
}

void OutlawsShipSmoke::Startup()
{
	if (!Active()) return;
	UE_LOG(LogOutlaws, Display, TEXT("OLShipSmoke: on: the main menu, then %d story levels, then quit"), NumSmokeLevels);
	GSmokeStage = ESmokeStage::Menu;
	GSmokeStageAt = FPlatformTime::Seconds();
	SmokeEnsureTicker();
}

void OutlawsShipSmoke::NoteMenu(AOutlawsLevelActor* L)
{
	if (!Active() || GSmokeStage != ESmokeStage::Menu || !L) return;
	GSmokeLevel = L;
	const double Ms = (FPlatformTime::Seconds() - GStartTime) * 1000.0;
	++GSmokeLoaded;
	GSmokeTotalMs += Ms;
	UE_LOG(LogOutlaws, Display, TEXT("OLShipSmoke: main menu loaded in %.0f ms (from the start)"), Ms);
	GSmokeStage = ESmokeStage::Settle;
	GSmokeSettleUntil = FPlatformTime::Seconds() + SmokePlaySecondsFor(true);
	SmokeEnsureTicker();
}

void OutlawsShipSmoke::NoteBuilt(AOutlawsLevelActor* L)
{
	if (!Active() || !L) return;
	if (GSmokeStage != ESmokeStage::Level || !L->LevelName.Equals(SmokeLevels[GSmokeIndex], ESearchCase::IgnoreCase)) return;
	GSmokeLevel = L;
	const double Ms = (FPlatformTime::Seconds() - GSmokeStageAt) * 1000.0;
	const FString Report = L->GetBuildReport();
	if (Report.StartsWith(TEXT("ERROR")))
	{
		SmokeFail(Report);
	}
	else
	{
		++GSmokeLoaded;
		GSmokeTotalMs += Ms;
		UE_LOG(LogOutlaws, Display, TEXT("OLShipSmoke: %s loaded in %.0f ms"), SmokeLevels[GSmokeIndex], Ms);
	}
	GSmokeStage = ESmokeStage::Settle;
	GSmokeSettleUntil = FPlatformTime::Seconds() + SmokePlaySecondsFor(false);
}
