// REQ-ENDONBOSS follow-up (user 3 Nov: with the host's "End mission on boss down" on, a co-op joiner flashed "Mission Complete!"
// just before the host ended the level): -OLCoopAuto -OLCoopTest=handfulend -OLCoopLevel=villa -OLHandfulEndOn=1|0
// [-OLOld899Flash] on the host and its client (run_coop.ps1).  The host sets its option as asked and the client its own the
// other way (the host's decides), then the host shoots CROWE (villa's boss, ITM BOSS TRUE) down; each machine watches its HUD
// for message 899 and its level's end.  On: the host's option reached the joiner (the session's bHostEndOnBoss), the host ends
// the level at once with no 899 (REQ-ENDONBOSS), the joiner says no 899 either and ends on the host's word; off: both say 899
// and the level ends about 5 s on, as before.  -OLOld899Flash: the joiner's 899 as before (on: its check FAILs, the control).
// Lines "OLSHOT handfulend check PASS|FAIL (host|client): ..." and "OLSHOT handfulend host|client RESULT".

#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsEnemy.h"
#include "OutlawsFlags.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsHandfulEndCoopProbeLocal
{
	int32 Step = 0, Passes = 0, Fails = 0;
	double At = 0.0, KillAt = -1.0, LineAt = -1.0, WonAt = -1.0;
	TWeakObjectPtr<AOutlawsEnemy> Boss;
}

void UOutlawsCoop::TickHandfulEndTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	using namespace OutlawsHandfulEndCoopProbeLocal;
	const bool bHost = IsHost();
	const TCHAR* Me = bHost ? TEXT("host") : TEXT("client");
	int32 OnArg = 1;
	FParse::Value(FCommandLine::Get(), TEXT("OLHandfulEndOn="), OnArg);
	const bool bOn = OnArg != 0;
	const bool bOld = OLFlags::Param(TEXT("OLOld899Flash"));
	auto Check = [Me](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT handfulend check %s (%s): %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), Me, *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const FString Line = L->UserMessageOr(899, TEXT("Mission Complete!"));
	// (each frame once the boss is down here) the line on this player's HUD, and this machine's level's end
	if (KillAt >= 0.0)
	{
		if (LineAt < 0.0) for (const FString& H : P->HudLines) if (H == Line) LineAt = Now;
		if (WonAt < 0.0 && (L->DebugLevelWon() || L->IsMoviePlaying() || L->IsFrontEnd() || !L->LevelName.Equals(TEXT("villa"), ESearchCase::IgnoreCase))) WonAt = Now;
	}
	auto Since = [](double T) { return T < 0.0 ? -1.0 : T - KillAt; };
	const AOutlawsCoopSession* S = GetSession();
	switch (Step)
	{
	case 0:
	{
		if (!L->LevelName.Equals(TEXT("villa"), ESearchCase::IgnoreCase) || !L->HasActorBegunPlay() || !L->AreEnemiesActive()) return;
		if (bHost)
		{
			const AOutlawsCoopLink* CL = GetLink(PartnerIndex());
			if (!CL || CL->BuiltSerial != StartedSerial || !PartnerPuppet()) return;
		}
		if (At == 0.0) { At = Now + 4.0; return; }
		if (Now < At) return;
		L->bInvincible = true;
		// the host's option as asked; the client's own the other way (the host's is the one that counts)
		L->bEndOnBoss = bHost ? bOn : !bOn;
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies()) if (Wk.IsValid() && Wk->GetOutlawsName().Equals(TEXT("CROWE"), ESearchCase::IgnoreCase)) Boss = Wk;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT handfulend %s: villa, CROWE found %d; End mission on boss down here %s (the host's %s)%s"), Me, Boss.IsValid() ? 1 : 0,
			L->bEndOnBoss ? TEXT("on") : TEXT("off"), bOn ? TEXT("on") : TEXT("off"), bOld ? TEXT(", -OLOld899Flash") : TEXT(""));
		At = Now + 2.0;
		Step = bHost ? 1 : 20;
		return;
	}
	// ---- the host ----
	case 1:
	{
		if (Now < At) return;
		Check(S && S->bHostEndOnBoss == bOn, FString::Printf(TEXT("the session carries the host's option to its joiners (%s)"), S && S->bHostEndOnBoss ? TEXT("on") : TEXT("off")));
		AOutlawsEnemy* B = Boss.Get();
		for (int32 k = 0; k < 20 && B && !B->IsDead(); ++k)
		{
			AOutlawsEnemy::GNextHitMelee = false;
			B->TakeHit(5.f, B->GetActorLocation(), FVector(1.0, 0.0, 0.0), B->GetActorLocation() - FVector(60.0, 0.0, 0.0));
		}
		Check(B && B->IsDead(), FString::Printf(TEXT("the host shoots CROWE down (%s)"), B ? (B->IsDead() ? TEXT("dead") : TEXT("standing")) : TEXT("not found")));
		KillAt = Now;
		LineAt = WonAt = -1.0;
		Step = 2;
		return;
	}
	case 2:
		if (WonAt < 0.0 && Now < KillAt + 8.0) return;
		if (WonAt >= 0.0 && Now < WonAt + 1.0) return;
		if (bOn) Check(WonAt >= 0.0 && Since(WonAt) < 1.0 && LineAt < 0.0, FString::Printf(TEXT("(on) the host ends the level at once (%.1f s), no \"%s\" (%.1f s), as before"), Since(WonAt), *Line, Since(LineAt)));
		else Check(LineAt >= 0.0 && Since(WonAt) >= 4.0 && Since(WonAt) <= 6.5, FString::Printf(TEXT("(off) \"%s\" here (%.1f s), the end about 5 s on (%.1f s), as before"), *Line, Since(LineAt), Since(WonAt)));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT handfulend host RESULT %s (%d passed, %d failed; the option %s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails, bOn ? TEXT("on") : TEXT("off"));
		At = Now + 4.0;
		Step = 3;
		return;
	case 3:
		if (Now < At) return;
		TestSay(TEXT("quit"), 0);
		At = Now + 2.0;
		Step = 4;
		return;
	case 4:
		if (Now < At) return;
		Summary(TEXT("done"));
		QuitForTest();
		Step = 5;
		return;
	// ---- the client: its copy of CROWE dies on the host's word ----
	case 20:
		if (Now < At) return;
		Check(S && S->bHostEndOnBoss == bOn, FString::Printf(TEXT("the host's option has come here (%s; this player's own %s)"), S && S->bHostEndOnBoss ? TEXT("on") : TEXT("off"),
			L->bEndOnBoss ? TEXT("on") : TEXT("off")));
		At = Now + 30.0;
		Step = 21;
		return;
	case 21:
	{
		const AOutlawsEnemy* B = Boss.Get();
		const bool bWon = L->DebugLevelWon() || L->IsMoviePlaying();
		if (!(B && B->IsDead()) && !bWon)
		{
			if (Now < At) return;
			Check(false, TEXT("CROWE seen down here, or the level's end, in 30 s"));
			Step = 23;
			return;
		}
		KillAt = Now;
		LineAt = WonAt = -1.0;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT handfulend client: %s first"), B && B->IsDead() ? TEXT("CROWE's death came") : TEXT("the host's end came"));
		Step = 22;
		return;
	}
	case 22:
		// (on: watched until 1.5 s past the end, a late boss death too; off: the line, then the end about 5 s on)
		if (WonAt < 0.0 && Now < KillAt + 9.0) return;
		if (WonAt >= 0.0 && Now < WonAt + 1.5) return;
		if (bOn) Check(WonAt >= 0.0 && LineAt < 0.0, FString::Printf(TEXT("(on) no \"%s\" here (%.1f s), the level ends on the host's word (%.1f s)%s"), *Line, Since(LineAt), Since(WonAt),
			bOld ? TEXT(" (-OLOld899Flash: the flash as before, FAIL expected)") : TEXT("")));
		else Check(LineAt >= 0.0 && WonAt >= 0.0, FString::Printf(TEXT("(off) \"%s\" here (%.1f s), then the level's end (%.1f s), as before"), *Line, Since(LineAt), Since(WonAt)));
		Step = 23;
		return;
	case 23:
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT handfulend client RESULT %s (%d passed, %d failed; the option %s)%s"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			bOn ? TEXT("on") : TEXT("off"), bOld ? TEXT(" -OLOld899Flash") : TEXT(""));
		Step = 24;
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
