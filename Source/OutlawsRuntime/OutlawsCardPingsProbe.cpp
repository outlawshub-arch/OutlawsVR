// REQ-CARDPINGS's probe (the user, 4 Nov, via the planner: "yes fill in other players' pings on the card"), three machines,
// -OLCardPingsProbe on each, the bot left in for its row:
//   host:    -OLMpHost=town_2p -OLMpClass=dm -OLMpBots=1 -OLMpHostRespawn=2 -OLMpMinutes=30 -OLMpFrags=0 [-OLOldCardPings]
//   clients: -OLCoopJoin=<host>:<port> [-OLOldCardPings]
// The host starts the match once both clients are in its rules, and 16 s into it each machine reads its own wheel card
// (FillMatchTable, the card's and Tab's table): on a client the host's row has the ping it always had, the other client's row
// has one too (-OLOldCardPings: blank), its own row and the bot's none; on the host each client's as ever.  Each machine stays
// past the reads: a client that quits takes its row off the scores, so a client's quit at its own read took that row off the card
// of the one that read a tenth of a second after it.  The host counts the times a new ping went out with the scores from the
// match's start (its count runs from the warm-up): no more than one each 5 s.  To force that quit's race: -OLCardPingsQuitAtRead
// on each client and -OLCardPingsReadLate=<ms> on one.  Lines "OLSHOT cardpings check PASS|FAIL: ...",
// "OLSHOT cardpings (who) card row: ..." and "OLSHOT cardpings RESULT".

#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsDeathmatch.h"
#include "OutlawsFlags.h"
#include "OutlawsFragTable.h"
#include "OutlawsLevelActor.h"
#include "OutlawsMpHost.h"
#include "OutlawsPlayer.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsCardPingsProbeLocal
{
	constexpr double ReadAfter = 16.0, ClientStays = 5.0, HostStays = 10.0;
	int32 Passes = 0, Fails = 0, SendsAtStart = 0, SendsSeen = 0;
	double StartAt = -1.0, Deadline = -1.0, LastSendAt = -1.0, LeastGap = -1.0;
	bool bRead = false, bDone = false;

	void Check(bool bOk, const TCHAR* Who, const FString& What)
	{
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT cardpings check %s: (%s) %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), Who, *What);
		if (bOk) ++Passes; else ++Fails;
	}

	FString CardCell(const FFragTableRow& R, int32 Column)
	{
		return R.Values.IsValidIndex(Column) ? R.Values[Column] : FString();
	}
}

void UOutlawsCoop::TickCardPingsProbe(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	using namespace OutlawsCardPingsProbeLocal;
	if (bDone) return;
	const bool bHost = IsHost(), bOld = OLFlags::Param(TEXT("OLOldCardPings"));
	const TCHAR* Who = bHost ? TEXT("host") : TEXT("client");
	const AOutlawsCoopSession* S = GetSession();
	if (Deadline < 0.0) Deadline = Now + 240.0;
	auto Finish = [&]()
	{
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT cardpings RESULT %s (%s: %d passed, %d failed, %s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Who, Passes, Fails, bOld ? TEXT("-OLOldCardPings") : TEXT("new"));
		bDone = true;
		FGenericPlatformMisc::RequestExit(false);
	};
	if (Now > Deadline)
	{
		Check(false, Who, FString::Printf(TEXT("a match with both clients in 240 s (read %d)"), bRead ? 1 : 0));
		Finish();
		return;
	}
	if (StartAt < 0.0)
	{
		if (bHost)
		{
			TArray<AOutlawsCoopLink*> Links;
			GetLinks(Links);
			int32 InRules = 0;
			for (const AOutlawsCoopLink* K : Links) InRules += K->PlayerIndex > 0 && K->PlayerIndex < MaxPlayers && !K->bBot && K->BuiltSerial == StartedSerial && MatchInRules[K->PlayerIndex] ? 1 : 0;
			FOutlawsMatchFlow& F = OutlawsMpHost::Flow();
			if (InRules < 2 || !bMatchSetUp || !MatchInRules[0]) return;
			if (F.GetState() == FOutlawsMatchFlow::EState::WarmUp) F.HostStart();
			if (F.GetState() != FOutlawsMatchFlow::EState::Playing) return;
		}
		else if (!S || S->Scores.Num() < 4 || !IsMatch() || S->MatchState != uint8(FOutlawsMatchFlow::EState::Playing)) return;
		StartAt = Now;
		SendsAtStart = SendsSeen = ScorePingSends;
		return;
	}
	if (bHost && ScorePingSends != SendsSeen)
	{
		// (as this tick sees them, a frame late at most: the number for the report, the check is the count)
		SendsSeen = ScorePingSends;
		if (LastSendAt >= 0.0) LeastGap = LeastGap < 0.0 ? Now - LastSendAt : FMath::Min(LeastGap, Now - LastSendAt);
		LastSendAt = Now;
	}
	if (!bRead)
	{
		int32 LateMs = 0;
		OLFlags::Value(TEXT("OLCardPingsReadLate"), LateMs);
		if (Now < StartAt + ReadAfter + LateMs / 1000.0) return;
		bRead = true;
		FFragTableData T;
		const bool bTable = FillMatchTable(T);
		Check(bTable && T.Rows.Num() >= 4, Who, FString::Printf(TEXT("the card has the three people and the bot: %d rows"), T.Rows.Num()));
		int32 Ping = -1;
		for (int32 c = 0; c < T.Columns.Num(); ++c) if (T.Columns[c].Header == TEXT("PING")) Ping = c - 1;      // (into a row's Values: the name is no value)
		TArray<FPlayerRow> People;
		PlayerRows(People);
		int32 Others = 0, Numbers = 0, BotRows = 0, BotBlank = 0;
		for (const FFragTableRow& R : T.Rows)
		{
			const FString Cell = CardCell(R, Ping);
			const bool bPerson = People.ContainsByPredicate([&R](const FPlayerRow& X) { return X.Who == R.Who; });
			UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT cardpings (%s) card row: player %d \"%s\"%s%s, ping \"%s\""), Who, R.Who, *R.Name, R.bLocal ? TEXT(", this machine's") : TEXT(""), bPerson ? TEXT("") : TEXT(", a bot"), *Cell);
			if (!bPerson) { ++BotRows; BotBlank += Cell.IsEmpty() ? 1 : 0; continue; }
			if (R.bLocal) { Check(Cell.IsEmpty(), Who, FString::Printf(TEXT("this player's own row has no ping, as REQ-PAUSECHAR2 left it: \"%s\""), *Cell)); continue; }
			if (bOld && !bHost && R.Who != 0) { Check(Cell.IsEmpty(), Who, FString::Printf(TEXT("(old) the other client's row is blank: \"%s\""), *Cell)); continue; }
			++Others;
			Numbers += Cell.IsNumeric() ? 1 : 0;
		}
		Check(Others == 2 - (bOld && !bHost ? 1 : 0) && Numbers == Others, Who, FString::Printf(TEXT("%d of the %d other people's rows have a number"), Numbers, Others));
		Check(BotRows >= 1 && BotBlank == BotRows, Who, FString::Printf(TEXT("the %d bot row(s) show nothing"), BotRows));
		if (bHost)
		{
			const int32 Sends = ScorePingSends - SendsAtStart;
			Check(Sends <= 1 + int32((Now - StartAt) / 5.0), Who, FString::Printf(TEXT("%d new pings sent in %.1f s of play, the least gap %.2f s (%d more before the match)"), Sends, Now - StartAt, LeastGap, SendsAtStart));
		}
		else if (OLFlags::Param(TEXT("OLCardPingsQuitAtRead"))) Finish();
		return;
	}
	if (Now >= StartAt + ReadAfter + (bHost ? HostStays : ClientStays)) Finish();
}

#endif // WITH_OUTLAWS_TESTS
