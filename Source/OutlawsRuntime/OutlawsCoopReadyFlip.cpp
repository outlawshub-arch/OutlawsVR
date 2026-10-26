// BUG-READYFLIP (RC2 93a444c, the user and a tester, 2026-10-10) was a lobby of three whose Ready row read the opposite of what
// the host counted.  REQ-MPLOBBY (landing a, 12 Oct) took Ready out: the lobby's Enter Game is one-way and means ready, so the
// check is now the lobby's Enter and the host's Start, still against what the host counts.
//
// Its check: -OLCoopAuto -OLCoopTest=readyflip -OLCoopPlayers=3 on the host and both clients (run_readyflip.ps1), co-op on
// hideout.  Nobody enters by themselves.
// 1. The host's own page before its start: no Ready row, a Start row.
// 2. Client 1 presses Enter Game as a hand's trigger presses it: the host counts it entered, its row says it is waiting for the
//    host; then twice in one frame: still entered (one-way).
// 3. The host presses Start with client 2 still in the lobby: client 1 goes in with the start, client 2 is left to join late.
// 4. The host lands in the lobby over its level, presses Enter Game and is in the game.
// 5. Client 1 has built the host's level from the start; client 2 presses Enter and joins the running level (the late join).
// Each check logs "OLCOOP readyflip check PASS|FAIL: ..." on the host; then "OLCOOP readyflip RESULT: <n> PASS, <m> FAIL".
//
// None of this runs outside that test.
#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"

#if WITH_OUTLAWS_TESTS

namespace
{
	const TCHAR* const EnterRow = TEXT("coop:enter");
	const TCHAR* const EnteredRow = TEXT("coop:entered");
}

void UOutlawsCoop::ReadyFlipCheck(bool bOk, const FString& What)
{
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP readyflip check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
	if (bOk) ++RFPass;
	else ++RFFail;
}

void UOutlawsCoop::TickReadyFlipHost(double Now, AOutlawsPlayer* P)
{
	AOutlawsCoopSession* S = GetSession();
	if (!S || !P || bExitRequested) return;
	if (RFStartAt == 0.0) RFStartAt = Now;
	if (RFStep < 90 && Now - RFStartAt > 330.0)
	{
		ReadyFlipCheck(false, FString::Printf(TEXT("the check ran out of time at step %d"), RFStep));
		RFStep = 90;
	}
	const int32 Want = FMath::Max(PartyPlayers, 3);
	AOutlawsCoopLink* C1 = GetLink(1);
	AOutlawsCoopLink* C2 = GetLink(2);
	// (a client's note: "readyflip <who> <k> <entered>|<the row it shows>")
	auto NoteFor = [this](int32 K, bool& bEntered, FString& Label)
	{
		FString Head;
		if (!RFNote.Split(TEXT("|"), &Head, &Label)) return false;
		TArray<FString> W;
		Head.ParseIntoArrayWS(W);
		if (W.Num() < 4 || FCString::Atoi(*W[2]) != K) return false;
		bEntered = FCString::Atoi(*W[3]) != 0;
		return true;
	};
	if (RFStep == 0)
	{
		// everyone here, the host's own page up, then three seconds for the clients' lobby pages
		if (P->CoopMenuPage() == TEXT("#coophost") && S->Connected >= Want && C1 && C2)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP readyflip: %d players in the lobby before the start"), S->Connected);
			RFStep = 10;
			RFAt = Now + 3.0;
		}
	}
	else if (RFStep == 10 && Now >= RFAt)
	{
		ReadyFlipCheck(P->CoopRowLabel(TEXT("coop:ready")).IsEmpty() && P->CoopRowLabel(EnterRow).IsEmpty(), TEXT("the host's page before its start has no Ready row and no Enter row"));
		ReadyFlipCheck(P->CoopRowLabel(TEXT("coop:start")) == TEXT("Start"), FString::Printf(TEXT("the host's page has its Start row ('%s')"), *P->CoopRowLabel(TEXT("coop:start"))));
		RFNote.Reset();
		TestSayTo(1, TEXT("readyflip"), 1);
		RFAt = Now + 15.0;
		RFStep = 20;
	}
	else if (RFStep == 20 || RFStep == 30)
	{
		const int32 K = RFStep == 20 ? 1 : 2;
		bool bEntered = false;
		FString Label;
		const bool bNote = NoteFor(K, bEntered, Label);
		if (!bNote && Now < RFAt) return;
		const bool bCounted = C1 && C1->bLobbyReady;
		ReadyFlipCheck(bNote && bCounted && bEntered && Label == TEXT("Entered: waiting for the host to start"),
			FString::Printf(TEXT("client 1 %s: the host counts it entered %d, its row shows '%s'"), K == 1 ? TEXT("presses Enter Game") : TEXT("presses it twice more in one frame"), bCounted ? 1 : 0,
				bNote ? *Label : TEXT("(no note)")));
		if (K == 1)
		{
			RFNote.Reset();
			TestSayTo(1, TEXT("readyflip"), 2);
			RFAt = Now + 15.0;
			RFStep = 30;
		}
		else
		{
			ReadyFlipCheck(C2 && !C2->bLobbyReady, TEXT("client 2 has not entered"));
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP readyflip: the host presses Start with client 2 still in the lobby"));
			if (!P->CoopPressRow(TEXT("coop:start"))) ReadyFlipCheck(false, TEXT("no Start row to press"));
			RFAt = Now + 2.0;
			RFStep = 40;
		}
	}
	else if (RFStep == 40 && Now >= RFAt)
	{
		ReadyFlipCheck(S->StartSerial > 0 && C1 && !C1->bLateJoin, FString::Printf(TEXT("Start: the level starts (serial %d) and client 1 goes in with it (late %d)"), S->StartSerial, C1 && C1->bLateJoin ? 1 : 0));
		ReadyFlipCheck(C2 && C2->bLateJoin && !C2->bLobbyReady, TEXT("client 2, still in the lobby, is left to join when it enters"));
		RFAt = Now + 120.0;
		RFStep = 50;
	}
	else if (RFStep == 50)
	{
		// the host lands in the lobby over its level
		const bool bThere = bLevelBuilt && P->CoopMenuPage() == TEXT("#cooplobby") && P->CoopRowLabel(EnterRow) == TEXT("Enter Game");
		if (!bThere && Now < RFAt) return;
		ReadyFlipCheck(bThere, FString::Printf(TEXT("the host's level is built behind the lobby, and the host lands in it (page %s, Enter row '%s')"), *P->CoopMenuPage(), *P->CoopRowLabel(EnterRow)));
		P->CoopPressRow(EnterRow);
		RFAt = Now + 1.5;
		RFStep = 60;
	}
	else if (RFStep == 60 && Now >= RFAt)
	{
		ReadyFlipCheck(S->bHostReady && !P->IsMenuOpen(), FString::Printf(TEXT("the host's Enter Game: in the game (entered %d, menu open %d)"), S->bHostReady ? 1 : 0, P->IsMenuOpen() ? 1 : 0));
		RFAt = Now + 120.0;
		RFStep = 70;
	}
	else if (RFStep == 70)
	{
		const bool bBuilt = C1 && C1->BuiltSerial == S->StartSerial;
		if (!bBuilt && Now < RFAt) return;
		ReadyFlipCheck(bBuilt, FString::Printf(TEXT("client 1 built the host's level from the start (serial %d)"), C1 ? C1->BuiltSerial : -1));
		TestSayTo(2, TEXT("readyflip"), 3);
		RFAt = Now + 150.0;
		RFStep = 80;
	}
	else if (RFStep == 80)
	{
		const bool bJoined = C2 && C2->BuiltSerial > 0;
		if (!bJoined && Now < RFAt) return;
		ReadyFlipCheck(bJoined && C2->bLobbyReady, FString::Printf(TEXT("client 2 pressed Enter Game and joined the running level (built serial %d)"), C2 ? C2->BuiltSerial : -1));
		RFStep = 90;
	}
	else if (RFStep == 90)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP readyflip RESULT: %d PASS, %d FAIL"), RFPass, RFFail);
		TestSay(TEXT("quit"));
		RFAt = Now + 3.0;
		RFStep = 91;
	}
	else if (RFStep == 91 && Now >= RFAt)
	{
		RFStep = 92;
		Summary(TEXT("done"));
		QuitForTest();
	}
}

void UOutlawsCoop::OnReadyFlipNote(const FString& Note)
{
	RFNote = Note;
}

bool UOutlawsCoop::OnReadyFlipStep(const FString& Step, int32 Arg)
{
	if (TestOnly != TEXT("readyflip")) return false;
	if (Step == TEXT("readyflip"))
	{
		RFClientK = Arg;
		RFClientStep = 1;
		return true;
	}
	if (Step == TEXT("quit"))
	{
		Summary(TEXT("done"));
		QuitForTest();
		return true;
	}
	return false;
}

void UOutlawsCoop::TickReadyFlipClient(double Now, AOutlawsPlayer* P)
{
	if (bExitRequested) return;
	if (RFStartAt == 0.0) RFStartAt = Now;
	if (Now - RFStartAt > 360.0)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP readyflip: the host's steps stopped coming: the client ends"));
		RFStartAt = Now;
		Summary(TEXT("the host's steps stopped"));
		QuitForTest();
		return;
	}
	if (RFClientStep == 0) return;
	AOutlawsCoopLink* Mine = GetLink(MyIndex());
	if (!P || !Mine || P->CoopMenuPage() != TEXT("#cooplobby")) return;       // (its lobby page first)
	if (RFClientStep == 1)
	{
		// Enter Game pressed as a hand's trigger presses it (the same MenuActivate as the desktop's click); step 2 twice in a frame
		const int32 Presses = RFClientK == 2 ? 2 : 1;
		for (int32 i = 0; i < Presses; ++i)
		{
			if (!P->CoopPressRow(EnterRow)) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP readyflip client: no Enter row on %s (%s)"), *P->CoopMenuPage(), *P->CoopRowLabel(EnteredRow));
		}
		RFClientDeadline = Now + 4.0;
		RFClientStep = 3;
	}
	else if (RFClientStep == 3)
	{
		if (Mine->bLobbyReady || Now >= RFClientDeadline)
		{
			RFClientAt = Now + 0.6;
			RFClientStep = 4;
		}
	}
	else if (RFClientStep == 4 && Now >= RFClientAt)
	{
		const FString Label = P->CoopRowLabel(EnteredRow);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP readyflip client: step %d: the row shows '%s', the host counts this player entered %d"), RFClientK, *Label, Mine->bLobbyReady ? 1 : 0);
		Mine->ServerTestNote(FString::Printf(TEXT("readyflip %d %d %d|%s"), MyIndex(), RFClientK, Mine->bLobbyReady ? 1 : 0, *Label));
		RFClientStep = 0;
	}
}
#endif // WITH_OUTLAWS_TESTS
