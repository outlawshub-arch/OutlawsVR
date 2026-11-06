// REQ-NETVERSION (the user 6 Nov, choice A), its probe, from the main menu:
//   -OLShots -OLNetVersionProbe[=<tag>]    alone: the check's cases and the message on the join page, a shot of it
//   ... -OLNetVersionProbe=<tag> -OLNetVersionJoin=<host:port> -OLNetVersionExpect=refused|joined
//                                          a real join from the join page
//   -OLShots -OLNetVersionHost -OLMpHost=drygulch -OLMpClass=dm -OLMpBots=0 -OLMpStartWhen=0 -OLNetVersionExpect=refused|joined
//                                          the host of that join: it counts its refusal, or its second player
// Either end takes -OLNetVersionFake=<n>: that machine on another number.  One line a case "OLSHOT netversion case PASS|FAIL:
// ...", then "OLSHOT netversion RESULT".  -OLOldNetVersion: the refusal cases FAIL (the old behaviour: anyone let in).

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsCoop.h"
#include "OutlawsBeacon.h"
#include "OutlawsBuildInfo.h"
#include "OutlawsFlags.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsShotTests.h"
#include "GameFramework/GameModeBase.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsNetVersionProbeLocal
{
	int32 Stage = 0, Passes = 0, Fails = 0;
	double StageAt = 0.0;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT netversion case %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}
}

OL_SHOT_MATCH(OLNetVersionProbe) { return OLFlags::Has(TEXT("OLNetVersionProbe")) || OLFlags::Param(TEXT("OLNetVersionHost")); }

OL_SHOT_TEST(OLNetVersionProbe, 1781)
{
	using namespace OutlawsNetVersionProbeLocal;
	if (Stage < 0) return;
	const double Now = FPlatformTime::Seconds();
	UOutlawsCoop* C = UOutlawsCoop::Get(this);
	FString Want, Addr, Tag;
	OLFlags::Value(TEXT("OLNetVersionExpect"), Want);
	OLFlags::Value(TEXT("OLNetVersionJoin"), Addr);
	OLFlags::Value(TEXT("OLNetVersionProbe"), Tag);
	if (Tag.IsEmpty()) Tag = TEXT("run");
	const bool bHost = OLFlags::Param(TEXT("OLNetVersionHost"));
	const FString Words = UOutlawsCoop::NetVersionRefusal();
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT netversion RESULT %s (%d passed, %d failed; this machine on %d, the baked number %d, build %s%s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			UOutlawsCoop::NetVersionHere(), OutlawsBuildInfo::NetVersion, OutlawsBuildInfo::Commit(), UOutlawsCoop::OldNetVersion() ? TEXT(", -OLOldNetVersion") : TEXT(""));
		Stage = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	auto Shot = [&Tag](const TCHAR* Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("netversion_%s_%s.png"), *Tag, Name), false, false); };
	if (Stage == 0)
	{
		StageAt = Now;
		Stage = bHost ? 10 : 1;
		return;
	}
	if (Stage >= 10)
	{
		// the host of a real join: the joiner's run decides the rest, this one says what it did with it
		if (L) { L->StartSaveDelay = -1.f; L->AutosaveWait = 0.f; }
		const int32 Refused = UOutlawsCoop::NetVersionTurnedAway;
		const UWorld* W = GetWorld();
		const int32 Players = W && W->GetAuthGameMode() ? W->GetAuthGameMode()->GetNumPlayers() : 0;
		const bool bDone = Want == TEXT("refused") ? Refused > 0 : Players >= 2;
		if (Stage == 10 && (bDone || Now - StageAt > 150.0))
		{
			Check(bDone, FString::Printf(TEXT("the host: %d refused, %d players (want %s)"), Refused, Players, *Want));
			StageAt = Now;
			Stage = 11;
		}
		else if (Stage == 11 && Now - StageAt > 8.0)
		{
			Check(Want == TEXT("refused") ? Players <= 1 : Refused == 0, FString::Printf(TEXT("the host 8 s on: %d refused, %d players"), Refused, Players));
			Finish();
		}
		return;
	}
	if (!C || !L || !bMenuOpen || !L->HasActorBegunPlay())
	{
		if (Stage == 1 && Now - StageAt > 150.0) { Check(false, TEXT("the main menu came up")); Finish(); }
		return;
	}
	switch (Stage)
	{
	case 1:
	{
		if (Now - StageAt < 3.0) return;
		LoadMenuSource();
		const bool bOverlay = MenuSource && MenuSource->HasOverlay();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT netversion font: %s"), bOverlay ? TEXT("the MenuOverlay.kpf font (as shipped)") : TEXT("the remaster's stock font (no overlay found)"));
		if (OLFlags::Param(TEXT("OLMenuFontExpect"))) Check(bOverlay, TEXT("the shots are in the shipped menu font (overlay open)"));
		// the check, on what joiners send
		const int32 Here = UOutlawsCoop::NetVersionHere();
		const FString Own = UOutlawsCoop::JoinGuardOptions();
		auto Ask = [](const FString& Options) { return UOutlawsCoop::CheckJoinNetVersion(Options); };
		const FString Key = FString::Printf(TEXT("?OLNetVersion=%d"), Here);
		Check(Own.Contains(Key), FString::Printf(TEXT("the joiner's URL says its number: \"%s\""), *Own.Left(40)));
		Check(Ask(Own).IsEmpty(), TEXT("the same number joins"));
		Check(Ask(Key + TEXT("?OLBuild=0000000")).IsEmpty(), TEXT("the same number on another build joins, whatever the nightly"));
		Check(Ask(FString::Printf(TEXT("?OLNetVersion=%d"), Here + 1)) == Words, TEXT("a higher number is refused, in the one line"));
		Check(Ask(FString::Printf(TEXT("?OLNetVersion=%d"), Here - 1)) == Words, TEXT("a lower number is refused, in the one line"));
		Check(Ask(TEXT("?OLBuild=0000000?OLMaps=")) == Words, TEXT("no number on its URL (0.95, the nightlies since) is refused"));
		Check(Ask(TEXT("?OLNetVersion=junk")) == Words, TEXT("a number that is no number is refused"));
		Check(Words.Len() <= 60 && !Words.Contains(TEXT("Checksum")), FString::Printf(TEXT("the words read plainly: \"%s\""), *Words));
		// the beacon
		Check(UOutlawsBeacon::OffersNetVersion(Here), TEXT("the beacon: a game on this number is offered"));
		Check(!UOutlawsBeacon::OffersNetVersion(Here + 1), TEXT("the beacon: a game on another number is not"));
		Check(!UOutlawsBeacon::OffersNetVersion(0), TEXT("the beacon: a game from before the number is not"));
		Stage = Addr.IsEmpty() ? 2 : 4;
		StageAt = Now;
		return;
	}
	case 2:      // alone: the words on the page, as a refusal leaves them
		C->JoinError = Words;
		ShowMenuPage(TEXT("#coopjoin"), true);
		RedrawMenu();
		Stage = 3;
		StageAt = Now;
		return;
	case 3:
		if (Now - StageAt < 1.0) return;
		Stage = 7;
		StageAt = Now;
		return;
	case 4:      // a real join, from the join page
		ShowMenuPage(TEXT("#coopjoin"), true);
		C->Join(Addr, true);
		Stage = 5;
		StageAt = Now;
		return;
	case 5:
	{
		const bool bIn = C->Mode == EOutlawsCoopMode::Client && C->GetSession();
		if (bIn && Want == TEXT("joined") && Now - StageAt > 3.0)
		{
			static double InSince = 0.0;
			if (InSince == 0.0) InSince = Now;
			if (Now - InSince < 6.0) return;
			Check(C->JoinError.IsEmpty(), FString::Printf(TEXT("joined, the session up for 6 s (\"%s\")"), *C->JoinError));
			Finish();
			return;
		}
		if (!C->JoinError.IsEmpty())
		{
			if (Want == TEXT("joined")) { Check(false, FString::Printf(TEXT("refused: \"%s\""), *C->JoinError)); Finish(); return; }
			Check(Want == TEXT("refused") && C->JoinError == Words, FString::Printf(TEXT("the joiner reads: \"%s\""), *C->JoinError));
			Stage = 6;
			StageAt = Now;
			return;
		}
		if (Now - StageAt > 60.0) { Check(false, TEXT("no answer from the host in 60 s")); Finish(); }
		return;
	}
	case 6:      // back on the join page, the words under the address
		if (Now - StageAt < 1.5) return;
		Check(C->Mode == EOutlawsCoopMode::None, TEXT("refused before the lobby: no session here"));
		ShowMenuPage(TEXT("#coopjoin"), false);
		RedrawMenu();
		Stage = 7;
		StageAt = Now;
		return;
	case 7:
	{
		if (Now - StageAt < 1.0) return;
		FString Said;
		const bool bFits = MenuPageFits(Said);
		Check(CoopMenuPage() == TEXT("#coopjoin") && bFits, FString::Printf(TEXT("the join page holds it on %s (%s)"), *CoopMenuPage(), *Said));
		Shot(TEXT("joinpage"));
		Stage = 8;
		StageAt = Now;
		return;
	}
	case 8:
		if (Now - StageAt > 1.5) Finish();
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
