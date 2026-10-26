// BUG-ALLOWCHEATS (v1.0): the Host Game page's "Allow Cheats" row (FOutlawsMpHostOptions::bAllowCheats) was stored and never read.
// Each machine keeps its own pockets (health, ammunition, keys, weapons: a cheat changes this machine's player), so the host's
// answer is asked BEFORE a client's cheat row runs: the client sends ServerCheatAsk(row) and runs the row only on the host's
// ClientCheatReply(yes).  The host answers from its own MatchOptions (never from anything the client says), logs each refusal
// ("OLCHEAT refused ..."), and nothing is changed on either machine.  The client's Cheats page reads the replicated
// AOutlawsCoopSession::bCheatsOff and shows one line, "The host has cheats off", in place of the rows.
// Story co-op has no such row on the host's page: its cheats are as they were.  Single player and the host's own are unchanged.
// A row that only changes what this player's own menus show is exempt: AOutlawsPlayer::CheatIsLocalOnly (OutlawsPlayerMenu.cpp).
// The check is -OLCoopAuto -OLCoopTest=allowcheats -OLAcExpect=0|1 (allowcheats_checks.ps1): the client asks for the all-weapons
// cheat and its Cheats page is read; the host's own cheat.

#include "OutlawsCoop.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsMpHost.h"
#include "OutlawsUnlocks.h"
#include "Engine/World.h"

void AOutlawsCoopLink::ServerCheatAsk_Implementation(int32 Id, const FString& Name)
{
	if (!Admit(EOLGate::Cheat, Name.Len() <= 40 && Name.StartsWith(TEXT("cheat_")))) return;
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnCheatAsk(PlayerIndex, Id, Name);
}

void AOutlawsCoopLink::ClientCheatReply_Implementation(int32 Id, bool bAllowed)
{
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnCheatReply(Id, bAllowed);
}

bool UOutlawsCoop::CheatsOffHere(const UObject* Context)
{
	const UOutlawsCoop* C = Get(Context);
	if (!C || C->IsHost() || C->Mode == EOutlawsCoopMode::None) return false;
	const AOutlawsCoopSession* S = C->GetSession();
	return S && S->bCheatsOff;
}

bool UOutlawsCoop::AskHostCheat(const UObject* Context, const FString& Name, TFunction<void(bool)> Done)
{
	UOutlawsCoop* C = Get(Context);
	if (!C || C->IsHost() || C->Mode == EOutlawsCoopMode::None) return false;
	AOutlawsCoopLink* Mine = C->GetLink(C->MyIndex());
	if (!Mine) return false;
	const int32 Id = ++C->CheatAskId;
	C->CheatWaiting.Add(Id, MoveTemp(Done));
	Mine->ServerCheatAsk(Id, Name);
	return true;
}

void UOutlawsCoop::OnCheatAsk(int32 Who, int32 Id, const FString& Name)
{
	// (the host) the authority is the host's own Allow Cheats row for this match; story co-op has none; a local-only row is not governed
	if (!IsHost()) return;
	const bool bGoverned = IsMatch() && !AOutlawsPlayer::CheatIsLocalOnly(Name);
	const bool bOk = !bGoverned || MatchOptions.bAllowCheats;
	if (bOk) { UE_LOG(LogOutlawsCoop, Log, TEXT("OLCHEAT allowed: the %s used %s (%s)"), WhoWord(FMath::Clamp(Who, 0, MaxPlayers - 1)), *Name, bGoverned ? TEXT("Allow Cheats is on") : TEXT("not governed here")); }
	else { UE_LOG(LogOutlawsCoop, Log, TEXT("OLCHEAT refused: the %s asked for %s, the host has Allow Cheats off; nothing changed"), WhoWord(FMath::Clamp(Who, 0, MaxPlayers - 1)), *Name); }
	if (AOutlawsCoopLink* K = Who > 0 ? GetLink(Who) : nullptr) K->ClientCheatReply(Id, bOk);
}

void UOutlawsCoop::OnCheatReply(int32 Id, bool bAllowed)
{
	TFunction<void(bool)> Done;
	if (!CheatWaiting.RemoveAndCopyValue(Id, Done) || !Done) return;
	Done(bAllowed);
}

// ---- -OLCoopTest=allowcheats --------------------------------------------------------------------------------------------------

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TickAllowCheatsTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	const bool bHost = IsHost();
	const TCHAR* Me = bHost ? TEXT("host") : TEXT("client");
	int32 Expect = 1;
	OLFlags::Value(TEXT("OLAcExpect"), Expect);          // 1: the client's cheat is allowed, 0: refused
	auto Check = [this, Me](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP allowcheats check %s (%s): %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), Me, *What);
		if (bOk) ++AcPasses; else ++AcFails;
	};
	auto Give = [L]() { L->GiveAllWeapons(); };
	switch (AcStep)
	{
	case 0:
		if (bHost)
		{
			TArray<AOutlawsCoopLink*> Links;
			GetLinks(Links);
			bool bJoined = false;
			for (const AOutlawsCoopLink* K : Links) if (!K->bBot && K->PlayerIndex > 0 && K->BuiltSerial != 0) bJoined = true;
			if (!bJoined) return;
		}
		if (IsMatch())
		{
			// (a match spawns every player with the start kit when it goes live: the cheat is tried after that)
			const AOutlawsCoopSession* S = GetSession();
			if (!S || S->MatchState != uint8(FOutlawsMatchFlow::EState::Playing)) return;
		}
		if (AcAt == 0.0) { AcAt = Now + 6.0; return; }
		if (Now < AcAt) return;
		AcBaseRifle = L->GetCount(TEXT("irifle"));          // (a match's start kit may hold one)
		Check(L->GetCount(TEXT("iscope")) == 0, FString::Printf(TEXT("no scope at the start (rifles %d)"), AcBaseRifle));
		if (!bHost)
		{
			// the host's flag has reached this machine (a match with it off); story co-op has none
			Check(CheatsOffHere(P) == (Expect == 0), FString::Printf(TEXT("the replicated flag says cheats %s (expected %s)"), CheatsOffHere(P) ? TEXT("off") : TEXT("on"), Expect == 0 ? TEXT("off") : TEXT("on")));
			const FString Page = P->CheatsPageTestLabels();
			// (UNLOCKFREE: the local-only Unlock all modes row went with the locks, so cheats off is the one line)
			if (Expect == 0) Check(Page == TEXT("The host has cheats off"), FString::Printf(TEXT("the Cheats page is the one line (\"%s\")"), *Page));
			else Check(Page.Contains(TEXT("All weapons and ammo")) && !Page.Contains(TEXT("The host has cheats off")), FString::Printf(TEXT("the Cheats page has its rows (\"%s\")"), *Page.Left(60)));
			Check(!Page.Contains(TEXT("Unlock all modes")), FString::Printf(TEXT("no Unlock all modes row with the host's cheats %s (every mode open from the start)"), Expect == 0 ? TEXT("off") : TEXT("on")));
			P->CheatsMenuTestClose();
			// the client asks for the all-weapons cheat (forced past its own menu's gate: the host must refuse it itself)
			AcReply = -1;
			if (Expect == 0) P->RunCheat(TEXT("cheat_weapons"), Give, true, [this](bool bOk) { AcReply = bOk ? 1 : 0; });
			else
			{
				Check(P->CheatsMenuTestPress(TEXT("cheat_weapons")), TEXT("the All weapons row pressed on the Cheats page"));      // (the real menu path: asks, then runs)
				AcReply = 1;
			}
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP allowcheats: the client asks for the all-weapons cheat"));
		}
		AcAt = Now + 6.0;
		AcStep = 1;
		return;
	case 1:
		if (bHost)
		{
			if (!AcClientDone) { if (Now > AcAt + 60.0) { Check(false, TEXT("the client never reported")); AcStep = 3; } return; }
			// the client has tried: the host's pockets are as they were
			Check(L->GetCount(TEXT("iscope")) == 0 && L->GetCount(TEXT("irifle")) == AcBaseRifle, FString::Printf(TEXT("the host's pockets unchanged after the client's cheat (%d, %d)"), L->GetCount(TEXT("irifle")), L->GetCount(TEXT("iscope"))));
			Check(P->CheatsMenuTestPress(TEXT("cheat_weapons")), TEXT("the host presses the All weapons row on its Cheats page"));       // (the real menu path)
			AcAt = Now + 3.0;
			AcStep = 2;
			return;
		}
		if (Now < AcAt && (Expect == 1 || AcReply < 0)) return;          // (allowed: the menu's own ask runs the row when the host answers; wait it out)
		if (AcReply < 0) { Check(false, TEXT("no answer from the host")); }
		else
		{
			Check(AcReply == (Expect == 0 ? 0 : 1), FString::Printf(TEXT("the host's answer %d (expected %d)"), AcReply, Expect == 0 ? 0 : 1));
			if (Expect == 0) Check(L->GetCount(TEXT("iscope")) == 0 && L->GetCount(TEXT("irifle")) == AcBaseRifle, FString::Printf(TEXT("refused: the client's pockets unchanged (%d, %d)"), L->GetCount(TEXT("irifle")), L->GetCount(TEXT("iscope"))));
			else Check(L->GetCount(TEXT("iscope")) == 1, FString::Printf(TEXT("allowed: the rifle and scope given (%d, %d)"), L->GetCount(TEXT("irifle")), L->GetCount(TEXT("iscope"))));
		}
		P->CheatsMenuTestClose();
		if (AOutlawsCoopLink* Mine = GetLink(MyIndex())) Mine->ServerTestNote(TEXT("acdone"));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP allowcheats client RESULT %s (%d passed, %d failed)"), AcFails == 0 && AcPasses > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), AcPasses, AcFails);
		AcAt = Now + 120.0;
		AcStep = 20;                       // (the client waits for the host's quit)
		return;
	case 2:
		if (Now < AcAt) return;
		P->CheatsMenuTestClose();
		Check(L->GetCount(TEXT("iscope")) == 1, FString::Printf(TEXT("the host's own cheat worked with Allow Cheats %s (scope %d)"), Expect == 0 ? TEXT("off") : TEXT("on"), L->GetCount(TEXT("iscope"))));
		AcStep = 3;
		return;
	case 3:
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP allowcheats host RESULT %s (%d passed, %d failed)"), AcFails == 0 && AcPasses > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), AcPasses, AcFails);
		AcAt = Now + 2.0;
		AcStep = 4;
		return;
	case 4:
		if (Now < AcAt) return;
		TestSay(TEXT("quit"), 0);
		AcAt = Now + 2.0;
		AcStep = 5;
		return;
	case 5:
		if (Now < AcAt) return;
		Summary(TEXT("done"));
		QuitForTest();
		AcStep = 6;
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
