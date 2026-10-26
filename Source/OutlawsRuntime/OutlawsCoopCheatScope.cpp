// REQ-CHEATSCOPE (user 2026-10-13): the all-weapons cheat also gives the rifle's scope, as if it had been picked up, so the scope
// works on the rifle at once.  In co-op each machine keeps its own pockets (a pickup is arbitrated by the host, then given by
// the player's own machine through GiveEntry), so a client's cheat is its own machine's GiveAllWeapons, the same GiveEntry.
// This is the check, -OLCoopAuto -OLCoopTest=cheatscope (cheatscope_checks.ps1): the host and its client each, on their own
// machine: no scope and no zoom with the rifle alone; after the cheat the scope is owned and the rifle zooms.

#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "Engine/World.h"

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TickCheatScopeTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	const bool bHost = IsHost();
	const TCHAR* Me = bHost ? TEXT("host") : TEXT("client");
	auto Check = [this, Me](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP cheatscope check %s (%s): %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), Me, *What);
		if (bOk) ++CsPasses; else ++CsFails;
	};
	switch (CsStep)
	{
	case 0:
		if (bHost)
		{
			const AOutlawsCoopLink* CL = GetLink(PartnerIndex());
			if (!CL || CL->BuiltSerial != StartedSerial || !PartnerPuppet()) return;
		}
		if (CsAt == 0.0) { CsAt = Now + 4.0; return; }
		if (Now < CsAt) return;
		Check(L->GetCount(TEXT("iscope")) == 0, FString::Printf(TEXT("no scope at the start (%d)"), L->GetCount(TEXT("iscope"))));
		P->CheatScopeTestRaise();
		CsAt = Now + 4.0;
		CsStep = 1;
		return;
	case 1:
		if (Now < CsAt) return;
		Check(L->GetCount(TEXT("iscope")) == 0 && !P->CheatScopeTestUp(), FString::Printf(TEXT("no cheat: scope owned %d, zoom up %d"), L->GetCount(TEXT("iscope")), P->CheatScopeTestUp() ? 1 : 0));
		L->GiveAllWeapons();                 // (the cheat page's All weapons row calls this)
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP cheatscope: the %s uses the all-weapons cheat"), Me);
		CsAt = Now + 4.0;
		CsStep = 2;
		return;
	case 2:
		if (Now < CsAt) return;
		Check(L->GetCount(TEXT("iscope")) == 1, FString::Printf(TEXT("cheat: scope owned (%d)"), L->GetCount(TEXT("iscope"))));
		Check(P->CheatScopeTestUp(), TEXT("cheat: the rifle zooms (scope picture up)"));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP cheatscope %s RESULT %s (%d passed, %d failed)"), Me, CsFails == 0 && CsPasses > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), CsPasses, CsFails);
		CsAt = Now + (bHost ? 25.0 : 120.0);          // (the host waits for the client's turn, the client for the host's quit)
		CsStep = bHost ? 3 : 20;
		return;
	case 3:
		if (Now < CsAt) return;
		TestSay(TEXT("quit"), 0);
		CsAt = Now + 2.0;
		CsStep = 4;
		return;
	case 4:
		if (Now < CsAt) return;
		Summary(TEXT("done"));
		QuitForTest();
		CsStep = 5;
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
