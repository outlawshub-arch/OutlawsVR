// BUG-HANDFULDONE / BUG-ICEMARY (user 2 Nov: the Handful's civlwar2, villa and wharf never ended; icecaves ended on Mary):
//   -OLShots -OLLevel=civlwar1 -OLNoFrontEnd -OLMissionEndProbe [-OLOldHandfulEnd]
// civlwar1: the player put in its SPAWN sector, its INF END_LEVEL ends it.  Then civlwar2, icecaves, villa and wharf in turn:
// every bad guy but the mission's boss (ITM BOSS TRUE: KENNY, CONFED2, CROWE, DORSEY) shot dead, and nothing ends in 3 s;
// icecaves first MARYBOTL alone, and nothing ends in 10 s; then the boss shot: "Mission Complete!" (message 899) shown and the
// level's end 4 to 6.5 s on.  -OLOldHandfulEnd: the old outcomes (civlwar2, villa, wharf: no line, no end; icecaves ends on
// Mary at once).  Lines "OLSHOT missionend check PASS|FAIL: ..." and "OLSHOT missionend RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsEnemy.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsMissionEndProbeLocal
{
	int32 Passes = 0, Fails = 0, Case = 0;
	float Wait = 0.f, KillAt = -1.f, WonAt = -1.f, LineAt = -1.f;
	struct FCase { const TCHAR* Level; const TCHAR* Boss; };
	const FCase EndCases[] = { { TEXT("civlwar1"), nullptr }, { TEXT("civlwar2"), TEXT("KENNY") }, { TEXT("icecaves"), TEXT("CONFED2") },
		{ TEXT("villa"), TEXT("CROWE") }, { TEXT("wharf"), TEXT("DORSEY") } };

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT missionend check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}

	void Shoot(AOutlawsEnemy* E)
	{
		for (int32 k = 0; k < 20 && E && !E->IsDead(); ++k)
		{
			AOutlawsEnemy::GNextHitMelee = false;
			E->TakeHit(5.f, E->GetActorLocation(), FVector(1.0, 0.0, 0.0), E->GetActorLocation() - FVector(60.0, 0.0, 0.0));
		}
	}
}

OL_SHOT_MATCH(OLMissionEndProbe) { return OLFlags::Param(TEXT("OLMissionEndProbe")); }

OL_SHOT_TEST(OLMissionEndProbe, 1640)
{
	using namespace OutlawsMissionEndProbeLocal;
	if (!L || ShotStep < 0) return;
	const bool bOld = OLFlags::Param(TEXT("OLOldHandfulEnd"));
	const FCase& C = EndCases[FMath::Min(Case, int32(UE_ARRAY_COUNT(EndCases)) - 1)];
	const bool bHere = L->LevelName.Equals(C.Level, ESearchCase::IgnoreCase);
	const bool bUp = bHere && !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView && ShotTime > Wait;
	// the level's end and the line, as they come (once a case's kill is made)
	const FString Line = L->UserMessageOr(899, TEXT("Mission Complete!"));
	if (KillAt >= 0.f)
	{
		if (WonAt < 0.f && (L->DebugLevelWon() || L->IsMoviePlaying() || !bHere || L->IsFrontEnd())) WonAt = ShotTime;
		if (LineAt < 0.f) for (const FString& H : HudLines) if (H == Line) LineAt = ShotTime;
	}
	auto Mark = [&]() { KillAt = ShotTime; WonAt = -1.f; LineAt = -1.f; };
	auto Since = [&](float T) { return T < 0.f ? -1.f : T - KillAt; };
	auto Find = [&](const TCHAR* Name) -> AOutlawsEnemy*
	{
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies()) if (Wk.IsValid() && Wk->GetOutlawsName().Equals(Name, ESearchCase::IgnoreCase)) return Wk.Get();
		return nullptr;
	};
	auto Next = [&](float Secs) { Wait = ShotTime + Secs; ++ShotStep; };
	auto NextCase = [&]()
	{
		++Case;
		KillAt = -1.f;
		if (Case >= int32(UE_ARRAY_COUNT(EndCases)))
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT missionend RESULT %s (%d passed, %d failed)%s"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
				bOld ? TEXT(" -OLOldHandfulEnd") : TEXT(""));
			ShotStep = -1;
			FGenericPlatformMisc::RequestExit(false);
			return;
		}
		L->JumpToLevel(EndCases[Case].Level, false);
		Wait = ShotTime + 4.f;
		ShotStep = 1;
	};
	switch (ShotStep)
	{
	case 0:
	{
		// civlwar1: its SPAWN sector's END_LEVEL (enter, the player)
		if (ShotTime < 4.f || !bUp) return;
		L->bInvincible = true;
		FVector At;
		const bool bSpot = L->DebugSectorSpot(TEXT("SPAWN"), At);
		if (bSpot) TeleportTo(At + FVector(0.0, 0.0, 100.0), GetActorRotation(), false, true);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT missionend civlwar1: into SPAWN (%d) at %s"), bSpot ? 1 : 0, *At.ToString());
		Mark();
		Wait = ShotTime + 4.f;
		ShotStep = 10;
		return;
	}
	case 10:
		if (WonAt < 0.f && ShotTime < Wait) return;
		Check(WonAt >= 0.f, FString::Printf(TEXT("civlwar1: its INF END_LEVEL ends it (%.1f s after the player went in)"), Since(WonAt)));
		NextCase();
		return;
	case 1:
	{
		// every bad guy but the boss (and icecaves' Mary) shot dead
		if (!bUp) return;
		L->bInvincible = true;
		const bool bIce = FCString::Stricmp(C.Level, TEXT("icecaves")) == 0;
		AOutlawsEnemy* B = Find(C.Boss);
		int32 Shot = 0;
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
		{
			AOutlawsEnemy* E = Wk.Get();
			if (!E || E == B || E->IsDead() || E->bVoiceOnly || E->bNotBadGuy || (bIce && E->GetOutlawsName().Equals(TEXT("MARYBOTL"), ESearchCase::IgnoreCase))) continue;
			Shoot(E);
			Shot += E->IsDead() ? 1 : 0;
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT missionend %s: boss %s found %d (a boss %d), %d other bad guys shot dead"), C.Level, C.Boss, B ? 1 : 0, B && B->bBoss ? 1 : 0, Shot);
		Check(B && B->bBoss == !bOld, FString::Printf(TEXT("%s: %s %s"), C.Level, C.Boss, bOld ? TEXT("a plain bad guy (-OLOldHandfulEnd: the BOSS key unread)") : TEXT("is the mission's boss (ITM BOSS TRUE)")));
		Mark();
		Next(3.f);
		return;
	}
	case 2:
	{
		if (ShotTime < Wait) return;
		Check(WonAt < 0.f && LineAt < 0.f, FString::Printf(TEXT("%s: the other bad guys down end nothing (end %.1f s, line %.1f s)"), C.Level, Since(WonAt), Since(LineAt)));
		if (FCString::Stricmp(C.Level, TEXT("icecaves")) == 0)
		{
			AOutlawsEnemy* M = Find(TEXT("MARYBOTL"));
			Shoot(M);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT missionend icecaves: MARYBOTL found %d, dead %d"), M ? 1 : 0, M && M->IsDead() ? 1 : 0);
			Mark();
			Wait = ShotTime + (bOld ? 4.f : 10.f);
			ShotStep = 20;
			return;
		}
		Next(0.f);
		return;
	}
	case 20:
		if (ShotTime < Wait && !(bOld && WonAt >= 0.f)) return;
		if (bOld)
		{
			Check(WonAt >= 0.f && WonAt - KillAt < 2.f, FString::Printf(TEXT("icecaves (-OLOldHandfulEnd): MARYBOTL down ends it at once (%.1f s)"), Since(WonAt)));
			Wait = ShotTime + 1.f;
			ShotStep = 5;
			return;
		}
		Check(WonAt < 0.f && LineAt < 0.f, FString::Printf(TEXT("icecaves: MARYBOTL down, no end and no line in 10 s (end %.1f s, line %.1f s)"), Since(WonAt), Since(LineAt)));
		ShotStep = 3;
		return;
	case 3:
	{
		if (ShotTime < Wait) return;
		AOutlawsEnemy* B = Find(C.Boss);
		Shoot(B);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT missionend %s: %s shot, dead %d"), C.Level, C.Boss, B && B->IsDead() ? 1 : 0);
		Mark();
		Next(6.5f);
		return;
	}
	case 4:
		if (ShotTime < Wait && !(WonAt >= 0.f && LineAt >= 0.f)) return;
		if (bOld)
		{
			Check(WonAt < 0.f && LineAt < 0.f, FString::Printf(TEXT("%s (-OLOldHandfulEnd): %s down, no line and no end, as before (end %.1f s, line %.1f s)"), C.Level, C.Boss, Since(WonAt), Since(LineAt)));
		}
		else
		{
			Check(LineAt >= 0.f && LineAt - KillAt < 1.f, FString::Printf(TEXT("%s: %s down: \"%s\" shown (%.1f s)"), C.Level, C.Boss, *Line, Since(LineAt)));
			Check(WonAt >= 0.f && WonAt - KillAt >= 4.f && WonAt - KillAt <= 6.5f, FString::Printf(TEXT("%s: the level ends by itself about 5 s on (%.1f s)"), C.Level, Since(WonAt)));
		}
		Wait = ShotTime + 1.f;
		Next(1.f);
		return;
	case 5:
		if (ShotTime < Wait) return;
		NextCase();
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
