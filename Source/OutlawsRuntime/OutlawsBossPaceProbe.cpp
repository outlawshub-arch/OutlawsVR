// REQ-BOSSPACE (user 5 Nov): -OLShots -OLLevel=<first> -OLNoFrontEnd -OLBossPaceProbe [-OLBossPaceLevels=townex+ranch+train]
// [-OLOldBossPace].  Level by level (all the boss levels by default): who the level's bosses are and whether each carries the
// original's BOSS mark; the curtain woken, the late ones summoned, each one touched by a shot that does no harm so he comes,
// then FightSec seconds of the fight as it plays with the player standing at the level's start, invincible, at the default
// dial.  One line a boss: his pace, his top ground speed, his path, how near he came, his shots, his walk and shoot frame
// rates.  Pass: a marked boss, and the three by the user's rule (Tim Bloodworth, Bill Morgan, Mary, not marked), at pace 1 with
// his moves scaled 0.6 and his walk and run frames with them, shoot frames full; Bob and a plain bad guy at EnemyPace, nothing
// scaled.  On the first level: ol.BossPace and ol.BossMovePace changed live; the difficulties' speeds through Rescale (Good 0.48, Bad 0.6,
// Ugly 0.84 of the ITM's); co-op's x1.5 on the bad guys and not on him; then the player out of his sight far off, so the walking
// boss takes a long leg and his top speed meets his cap.  After each fight the player stands CloseSec seconds 7 m from each boss
// (line "OLSHOT bosspace close"): data for the fight's report, no check.  -OLOldBossPace: all at EnemyPace, nothing scaled.
// A walk frame count over clean spans (no restart of the chor) is the true rate; a boss's restarts of his stride are counted, and
// one who walks on the spot gets a "trace" line with his move state.  -OLBossPaceFightSec=<s>: a shorter fight, for the close data.
// Lines "OLSHOT bosspace check PASS|FAIL" and "OLSHOT bosspace RESULT".
#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsEnemy.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsRuntime.h"
#include "OutlawsGeometry.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsBossPaceProbeLocal
{
	float FightSec = 22.f;
	constexpr float CloseSec = 10.f, LegSec = 14.f;
	int32 Passes = 0, Fails = 0, Case = 0, CloseIdx = 0, LegIdx = -1;
	float Wait = 0.f, Began = 0.f, SampleAt = 0.f, CloseAt = 0.f;
	bool bLiveDone = false;
	TArray<FString> Levels;

	struct FWatch
	{
		TWeakObjectPtr<AOutlawsEnemy> E;
		FString Name;
		float PaceMin = 100.f, PaceMax = -1.f, Top = 0.f, Path = 0.f, Near = 1e9f, NearAt = -1.f, FirstShotAt = -1.f;
		float WalkFrames = 0.f, WalkTime = 0.f, FireFrames = 0.f, FireTime = 0.f, WalkFps = 0.f, FireFps = 0.f;
		float WalkRate = 1.f, FireRate = 1.f, SpeedShare = -1.f;
		int32 Shots0 = 0, LastChor = -1, LastFrame = 0;
		// clean spans of a walk or run chor: from its first frame change to its last, a restart or a new chor ends one
		float Clock = 0.f, SpanStart = -1.f, SpanEnd = 0.f, SpanFrames = 0.f, CleanFrames = 0.f, CleanTime = 0.f, CapSeen = 0.f, TraceAt = 0.f;
		float SpanWant = 0.f, SpanWantEnd = 0.f, CleanWant = 0.f;       // (the frames his chor's FPS, its rate and his pace should have shown: walk and run differ)
		int32 Restarts = 0, WalkLen = 0, CloseShots0 = 0;
		FVector TraceMark = FVector::ZeroVector;
		void EndSpan()
		{
			if (SpanStart >= 0.f && SpanFrames >= 2.f && SpanEnd > SpanStart) { CleanFrames += SpanFrames; CleanTime += SpanEnd - SpanStart; CleanWant += SpanWantEnd; }
			SpanStart = -1.f;
			SpanFrames = SpanWant = SpanWantEnd = 0.f;
		}
		FVector Last = FVector::ZeroVector, Mark = FVector::ZeroVector;
		bool bPlain = false, bNoRoute = false;
	};
	TArray<FWatch> Watch;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT bosspace check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}

	void SetCVar(const TCHAR* Name, float V)
	{
		if (IConsoleVariable* C = IConsoleManager::Get().FindConsoleVariable(Name)) C->Set(V, ECVF_SetByCode);
	}

	/** The named outlaws who run the plain bad-guy handler and carry no key: bosses in our code alone, boss speed by the user's rule. */
	bool OursAlone(const FString& N)
	{
		static const TSet<FString> Names = { TEXT("TIMBLOOD"), TEXT("ETIMBLOO"), TEXT("BILLMORG"), TEXT("EBILLMOR"), TEXT("MARY"), TEXT("EMARY"), TEXT("MARYBOTL") };
		return Names.Contains(N.ToUpper());
	}

	/** One frame of his fight. */
	void Sample(FWatch& W, float Dt, float Now, const FVector& PlayerFeet, bool bHalfSec)
	{
		AOutlawsEnemy* E = W.E.Get();
		if (!E || E->IsDead()) return;
		const float Pace = E->CustomTimeDilation;
		W.PaceMin = FMath::Min(W.PaceMin, Pace);
		W.PaceMax = FMath::Max(W.PaceMax, Pace);
		const FVector Here = E->GetActorLocation();
		W.Path += float(FVector::Dist2D(Here, W.Last));
		W.Last = Here;
		const float D = float(FVector::Dist(Here, PlayerFeet));
		if (D < W.Near) { W.Near = D; W.NearAt = Now - Began; }
		if (W.FirstShotAt < 0.f && E->ShotsTotal > W.Shots0) W.FirstShotAt = Now - Began;
		if (bHalfSec)
		{
			// on his feet, not flying from the touch of the shot
			const UCharacterMovementComponent* M = E->GetCharacterMovement();
			if (M->IsMovingOnGround() && Pace < 1.5f) W.Top = FMath::Max(W.Top, float(FVector::Dist2D(Here, W.Mark)) / 0.5f);
			W.Mark = Here;
		}
		const AOutlawsSpriteActor* S = E->GetSprite();
		if (!S) return;
		const int32 Chor = S->GetPlayingChor(), Frame = S->GetChorFrame();
		const bool bWalkChor = Chor >= 0 && (Chor == E->Def.WalkChor || Chor == E->Def.RunChor);
		W.Clock += Dt;
		if (Chor != W.LastChor || !bWalkChor) W.EndSpan();
		else if (Frame < W.LastFrame) { ++W.Restarts; W.EndSpan(); }
		// walking on the spot: where his 3 s went
		if (bWalkChor && !W.bPlain && Now - W.TraceAt >= 3.f)
		{
			if (W.TraceAt > 0.f && FVector::Dist2D(Here, W.TraceMark) < 100.f) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT bosspace trace %s at %.1f s, moved %.0f cm in 3 s, at %s: %s"), *W.Name, Now - Began, FVector::Dist2D(Here, W.TraceMark), *Here.ToCompactString(), *E->DebugMoveState());
			W.TraceAt = Now;
			W.TraceMark = Here;
		}
		if (Chor == W.LastChor && Frame >= W.LastFrame && S->Chors.IsValidIndex(Chor))
		{
			const float Fps = S->Chors[Chor].FPS;
			if (Chor == E->Def.WalkChor || Chor == E->Def.RunChor)
			{
				W.WalkFrames += float(Frame - W.LastFrame); W.WalkTime += Dt; W.WalkFps = Fps; W.WalkRate = S->GetChorRate();
				W.WalkLen = S->Chors[Chor].Length;
				W.CapSeen = FMath::Max(W.CapSeen, E->GetCharacterMovement()->MaxWalkSpeed * Pace);
				if (W.SpanStart >= 0.f) W.SpanWant += Fps * S->GetChorRate() * Pace * Dt;
				if (Frame > W.LastFrame)
				{
					if (W.SpanStart < 0.f) W.SpanStart = W.Clock;
					else W.SpanFrames += float(Frame - W.LastFrame);
					W.SpanEnd = W.Clock;
					W.SpanWantEnd = W.SpanWant;
				}
				const float Base = (Chor == E->Def.RunChor ? E->Def.RunSpeed : E->Def.WalkSpeed) * float(FOutlawsGeometry::UnitsToCm);
				if (Base > 0.f) W.SpeedShare = E->GetCharacterMovement()->MaxWalkSpeed / Base;
			}
			else if (Chor == E->Def.ShootChor)
			{
				W.FireFrames += float(Frame - W.LastFrame); W.FireTime += Dt; W.FireFps = Fps; W.FireRate = S->GetChorRate();
			}
		}
		W.LastChor = Chor;
		W.LastFrame = Frame;
	}

	/** The fight's line and its checks. */
	void Judge(FWatch& W, const TCHAR* Level, float EnemyPace, float PlayerTop)
	{
		AOutlawsEnemy* E = W.E.Get();
		if (!E) { Check(false, FString::Printf(TEXT("%s: %s gone"), Level, *W.Name)); return; }
		const bool bOld = AOutlawsEnemy::OldBossPace();
		const float Base = E->Def.RunSpeed * float(FOutlawsGeometry::UnitsToCm);
		const float WalkSeen = W.WalkTime > 0.3f ? W.WalkFrames / W.WalkTime : -1.f, FireSeen = W.FireTime > 0.3f ? W.FireFrames / W.FireTime : -1.f;
		const bool bPace = E->IsPaceBoss();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT bosspace fight %s %s: boss %d mark %d paced %d stationary %d hunter %d peek %d | pace %.2f..%.2f (bad guys %.2f) | run %.0f cm/s, top seen %.0f (player %.0f), path %.1f m | nearest %.1f m at %.1f s | shots %d, the first at %.1f s | move share %.2f | walk frames %.1f/s (chor %.1f, rate %.2f), shoot frames %.1f/s (chor %.1f, rate %.2f)"),
			Level, *W.Name, E->bBoss ? 1 : 0, E->bBossMark ? 1 : 0, bPace ? 1 : 0, E->bStationary ? 1 : 0, E->bHunter ? 1 : 0, E->bPeekShooter ? 1 : 0, W.PaceMin, W.PaceMax, EnemyPace,
			Base, W.Top, PlayerTop, W.Path / 100.f, W.Near / 100.f, W.NearAt, E->ShotsTotal - W.Shots0, W.FirstShotAt,
			W.SpeedShare, WalkSeen, W.WalkFps, W.WalkRate, FireSeen, W.FireFps, W.FireRate);
		W.EndSpan();
		const float CleanSeen = W.CleanTime > 0.5f ? W.CleanFrames / W.CleanTime : -1.f;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT bosspace stride %s %s: %.1f frames/s over %.1f s of clean spans, %d restarts of the chor in %.1f s of walking, cycle %d frames, cap seen %.0f, top %.0f"),
			Level, *W.Name, CleanSeen, W.CleanTime, W.Restarts, W.WalkTime, W.WalkLen, W.CapSeen, W.Top);
		if (E->IsDead()) return;
		const float PaceWant = bPace ? 1.f : EnemyPace, ScaleWant = bPace ? 0.6f : 1.f;
		// (no fight, no proof: a hunter on his feet has to have gone somewhere, and Bob to have fired)
		// (shgranry's SH_MARY, 185 m from the level's start: no way through the openings, the hunt goes straight at the player and
		// no floor ahead holds her, with -OLOldBossPace too: she is judged in the close part)
		W.bNoRoute = E->bHunter && W.Path <= 200.f && E->DebugMoveState().Contains(TEXT("via -1/-1"));
		if (W.bNoRoute)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT bosspace noroute %s %s: %.1f m in %.1f s of walk frames: %s"), Level, *W.Name, W.Path / 100.f, W.WalkTime, *E->DebugMoveState());
		}
		else if (E->bHunter) Check(W.Path > 200.f && W.WalkTime > 0.3f, FString::Printf(TEXT("%s %s: a hunter walked (%.1f m, %.1f s of walk or run frames)"), Level, *W.Name, W.Path / 100.f, W.WalkTime));
		// (the frames over clean spans: a count over all the walking loses a part of a frame at each restart of the chor, the
		// hunt's step every 1.5 s of his own time)
		if (W.CleanTime > 2.f) Check(FMath::Abs(W.CleanFrames / W.CleanWant - 1.f) <= 0.08f, FString::Printf(TEXT("%s %s: %.1f clean walk or run frames in %.1f s, %.1f wanted"), Level, *W.Name, W.CleanFrames, W.CleanTime, W.CleanWant));
		if (E->bPeekShooter) Check(E->ShotsTotal > W.Shots0, FString::Printf(TEXT("%s %s: fires from his curtain (%d shots)"), Level, *W.Name, E->ShotsTotal - W.Shots0));
		// (a plain one's hit reaction runs at full pace: his lowest is the pace he goes at)
		Check(FMath::IsNearlyEqual(W.PaceMin, PaceWant, 0.02f), FString::Printf(TEXT("%s %s: pace %.2f, wanted %.2f"), Level, *W.Name, W.PaceMin, PaceWant));
		if (W.SpeedShare >= 0.f) Check(FMath::IsNearlyEqual(W.SpeedShare, ScaleWant, 0.01f), FString::Printf(TEXT("%s %s: his move's speed cap %.2f of his ITM's at this difficulty, wanted %.2f"), Level, *W.Name, W.SpeedShare, ScaleWant));
		if (W.WalkTime > 0.3f) Check(FMath::IsNearlyEqual(W.WalkRate, ScaleWant, 0.01f), FString::Printf(TEXT("%s %s: walk and run frames at %.2f, wanted %.2f"), Level, *W.Name, W.WalkRate, ScaleWant));
		if (WalkSeen > 0.f && W.WalkTime > 2.f) Check(FMath::Abs(WalkSeen - W.WalkFps * ScaleWant * PaceWant) <= W.WalkFps * 0.12f + 0.3f, FString::Printf(TEXT("%s %s: walk frames %.1f/s seen, %.1f wanted"), Level, *W.Name, WalkSeen, W.WalkFps * ScaleWant * PaceWant));
		if (W.FireTime > 0.3f) Check(FMath::IsNearlyEqual(W.FireRate, 1.f, 0.01f), FString::Printf(TEXT("%s %s: shoot frames at their full rate (%.2f)"), Level, *W.Name, W.FireRate));
		// (the cap he had on, walk or run; Base's run cap let a walking boss off)
		if (W.Top > 0.f) Check(W.Top <= (W.CapSeen > 0.f ? W.CapSeen : Base * ScaleWant * PaceWant) * 1.15f + 20.f, FString::Printf(TEXT("%s %s: top ground speed %.0f cm/s, cap %.0f"), Level, *W.Name, W.Top, W.CapSeen));
		if (!bOld) Check(E->bBossMark == (E->bBoss && !OursAlone(W.Name)) || W.bPlain, FString::Printf(TEXT("%s %s: BOSS mark %d as the original's (ours alone %d)"), Level, *W.Name, E->bBossMark ? 1 : 0, OursAlone(W.Name) ? 1 : 0));
		// (the rule's three by name, the mark kept apart: every boss on his feet paced but Bob)
		if (!bOld) Check((E->bBossSpeedRule == OursAlone(W.Name) && bPace == (E->bBoss && !E->bPeekShooter)) || W.bPlain, FString::Printf(TEXT("%s %s: by the rule %d, paced %d"), Level, *W.Name, E->bBossSpeedRule ? 1 : 0, bPace ? 1 : 0));
	}
}

OL_SHOT_MATCH(OLBossPaceProbe) { return OLFlags::Param(TEXT("OLBossPaceProbe")); }

OL_SHOT_TEST(OLBossPaceProbe, 1645)
{
	using namespace OutlawsBossPaceProbeLocal;
	if (!L || ShotStep < 0) return;
	L->bEnemiesActive = true;              // (ShotTest stills them each tick: a fight here)
	if (Levels.Num() == 0)
	{
		FString List;
		if (!OLFlags::Value(TEXT("OLBossPaceLevels"), List)) List = TEXT("townex+hideout+canyon+mill+simms+cliff+ranch+miner+train+civlwar2+icecaves+villa+wharf+shfort+shgranry+shgulch+shsimms+shwildns");
		List.ParseIntoArray(Levels, TEXT("+"));
		FString Sec;
		if (OLFlags::Value(TEXT("OLBossPaceFightSec"), Sec)) FightSec = FMath::Max(1.f, FCString::Atof(*Sec));
	}
	const bool bOld = AOutlawsEnemy::OldBossPace();
	const FString& Lv = Levels[FMath::Min(Case, Levels.Num() - 1)];
	const bool bHere = L->LevelName.Equals(Lv, ESearchCase::IgnoreCase);
	const bool bUp = bHere && !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView && ShotTime > Wait;
	auto Bosses = [&]()
	{
		TArray<AOutlawsEnemy*> Out;
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies()) if (Wk.IsValid() && Wk->bBoss && !Wk->bVoiceOnly) Out.Add(Wk.Get());
		return Out;
	};
	auto FeetOfPlayer = [&]() { return GetActorLocation() - FVector(0.0, 0.0, 87.0); };
	auto NextLevel = [&]()
	{
		++Case;
		Watch.Reset();
		if (Case >= Levels.Num())
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT bosspace RESULT %s (%d passed, %d failed)%s"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails, bOld ? TEXT(" -OLOldBossPace") : TEXT(""));
			ShotStep = -1;
			FGenericPlatformMisc::RequestExit(false);
			return;
		}
		L->JumpToLevel(Levels[Case], false);
		Wait = ShotTime + 4.f;
		ShotStep = 0;
	};
	switch (ShotStep)
	{
	case 0:
	{
		// jumped here, or the first level of the run to jump to
		if (Case == 0 && !bHere && ShotTime > 4.f && !L->IsRebuildPending() && Wait <= 0.f)
		{
			L->JumpToLevel(Lv, false);
			Wait = ShotTime + 4.f;
			return;
		}
		if (!bUp || ShotTime < 4.f) return;
		L->bInvincible = true;
		TArray<AOutlawsEnemy*> B = Bosses();
		FString Who;
		for (const AOutlawsEnemy* E : B) Who += FString::Printf(TEXT(" %s[mark %d paced %d dormant %d behind door %d]"), *E->GetOutlawsName(), E->bBossMark ? 1 : 0, E->IsPaceBoss() ? 1 : 0, E->IsDormant() ? 1 : 0, E->bBehindDoor ? 1 : 0);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT bosspace level %s: difficulty %d, enemy pace %.2f, %d bosses:%s"), *Lv, int32(L->Difficulty), L->EnemyPace(), B.Num(), *Who);
		Check(B.Num() > 0, FString::Printf(TEXT("%s: has a boss"), *Lv));
		L->WakeCurtainBoss(TEXT("bosspace probe"));
		L->DebugSummonBoss();
		Wait = ShotTime + 3.f;
		ShotStep = 1;
		return;
	}
	case 1:
	{
		if (!bUp) return;
		Watch.Reset();
		for (AOutlawsEnemy* E : Bosses())
		{
			FWatch& W = Watch.AddDefaulted_GetRef();
			W.E = E;
			W.Name = E->GetOutlawsName();
			W.Last = W.Mark = E->GetActorLocation();
			W.Shots0 = E->ShotsTotal;
			if (E->IsDormant()) E->SetDormant(false);
			AOutlawsEnemy::GNextHitMelee = false;
			E->TakeHit(0.001f, E->GetActorLocation(), FVector(1.0, 0.0, 0.0), FeetOfPlayer());
		}
		// a plain bad guy beside them: at EnemyPace, nothing scaled
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
		{
			AOutlawsEnemy* E = Wk.Get();
			if (!E || E->bBoss || E->IsDead() || E->IsDormant() || E->bVoiceOnly || E->bNotBadGuy || E->IsCritter() || E->CoopSelf >= 0) continue;
			FWatch& W = Watch.AddDefaulted_GetRef();
			W.E = E;
			W.Name = E->GetOutlawsName();
			W.bPlain = true;
			W.Last = W.Mark = E->GetActorLocation();
			W.Shots0 = E->ShotsTotal;
			break;
		}
		Began = SampleAt = ShotTime;
		ShotStep = 2;
		return;
	}
	case 2:
	{
		if (!bHere || L->IsMoviePlaying()) return;
		const bool bHalf = ShotTime - SampleAt >= 0.5f;
		if (bHalf) SampleAt = ShotTime;
		for (FWatch& W : Watch) Sample(W, Dt, ShotTime, FeetOfPlayer(), bHalf);
		// Bob fires only with the player inside his reach (20 m): kept by him, wherever the spot beside him drops the player
		if (bHalf) for (const FWatch& W : Watch) if (W.E.IsValid() && W.E->bPeekShooter && !W.E->IsDead()) TeleportTo(W.E->GetActorLocation() + FVector(300.0, 0.0, 0.0), GetActorRotation(), false, true);
		if (ShotTime - Began > FightSec)
		{
			const float PlayerTop = VRRunSpeed * L->Pace();
			for (FWatch& W : Watch) Judge(W, *Lv, L->EnemyPace(), PlayerTop);
			CloseIdx = 0;
			ShotStep = 3;
		}
		return;
	}
	// the player 7 m from each boss in turn, in his line of sight: his fire and how he comes on, close to
	case 3:
	{
		if (!bHere) { if (!L->IsRebuildPending()) NextLevel(); return; }
		while (Watch.IsValidIndex(CloseIdx) && (Watch[CloseIdx].bPlain || !Watch[CloseIdx].E.IsValid() || Watch[CloseIdx].E->IsDead() || Watch[CloseIdx].E->bPeekShooter)) ++CloseIdx;
		if (!Watch.IsValidIndex(CloseIdx))
		{
			if (Case == 0 && !bOld) { ShotStep = 10; return; }
			NextLevel();
			return;
		}
		FWatch& W = Watch[CloseIdx];
		AOutlawsEnemy* E = W.E.Get();
		const FVector From = E->GetActorLocation() + FVector(0.0, 0.0, 40.0);
		FCollisionQueryParams Q;
		Q.AddIgnoredActor(E);
		Q.AddIgnoredActor(this);
		bool bPlaced = false;
		for (int32 i = 0; i < 8 && !bPlaced; ++i)
		{
			const FVector Dir = FRotator(0.f, 45.f * i, 0.f).Vector(), To = From + Dir * 700.0;
			FHitResult Hit;
			if (GetWorld()->LineTraceSingleByChannel(Hit, From, To, ECC_Visibility, Q)) continue;
			if (!GetWorld()->LineTraceSingleByChannel(Hit, To, To - FVector(0.0, 0.0, 400.0), ECC_Visibility, Q)) continue;
			bPlaced = TeleportTo(Hit.Location + FVector(0.0, 0.0, 100.0), (-Dir).Rotation(), false, false);
		}
		W.CloseShots0 = E->ShotsTotal;
		W.Mark = E->GetActorLocation();
		CloseAt = ShotTime;
		if (!bPlaced) { UE_LOG(LogOutlaws, Display, TEXT("OLSHOT bosspace close %s %s: no spot 7 m off in his sight"), *Lv, *W.Name); ++CloseIdx; return; }
		ShotStep = 4;
		return;
	}
	case 4:
	{
		if (!bHere) { if (!L->IsRebuildPending()) NextLevel(); return; }
		if (ShotTime - CloseAt < CloseSec) return;
		const FWatch& W = Watch[CloseIdx];
		if (const AOutlawsEnemy* E = W.E.Get())
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT bosspace close %s %s: %d shots in %.0f s, %.1f m off at the end, sees %d, pace %.2f, %s"), *Lv, *W.Name, E->ShotsTotal - W.CloseShots0, CloseSec,
				FVector::Dist(E->GetActorLocation(), FeetOfPlayer()) / 100.f, E->DebugSees() ? 1 : 0, E->CustomTimeDilation, *E->DebugBossState());
			if (W.bNoRoute) Check(E->ShotsTotal > W.CloseShots0 || FVector::Dist2D(E->GetActorLocation(), W.Mark) > 100.f, FString::Printf(TEXT("%s %s, no way to the far player: close to, moved %.1f m and fired %d"), *Lv, *W.Name, FVector::Dist2D(E->GetActorLocation(), W.Mark) / 100.f, E->ShotsTotal - W.CloseShots0));
		}
		++CloseIdx;
		ShotStep = 3;
		return;
	}
	// the first level's live cases, on the first boss who walks
	case 10:
	{
		AOutlawsEnemy* B = nullptr;
		for (AOutlawsEnemy* E : Bosses()) if (E->IsPaceBoss() && !E->IsDead() && !E->bStationary) B = E;
		Check(B != nullptr, TEXT("a paced boss on his feet for the live cases"));
		if (!B) { NextLevel(); return; }
		SetCVar(TEXT("ol.BossPace"), 0.5f);
		SetCVar(TEXT("ol.BossMovePace"), 0.7f);
		Wait = ShotTime + 1.f;
		ShotStep = 11;
		return;
	}
	case 11:
	{
		if (ShotTime < Wait) return;
		for (AOutlawsEnemy* E : Bosses())
		{
			if (!E->IsPaceBoss() || E->IsDead() || E->bStationary) continue;
			Check(FMath::IsNearlyEqual(E->CustomTimeDilation, 0.5f, 0.01f) && FMath::IsNearlyEqual(E->BossMoveScale(), 0.7f, 0.001f), FString::Printf(TEXT("ol.BossPace 0.5 and ol.BossMovePace 0.7 live: pace %.2f, scale %.2f"), E->CustomTimeDilation, E->BossMoveScale()));
		}
		SetCVar(TEXT("ol.BossPace"), 1.f);
		SetCVar(TEXT("ol.BossMovePace"), 0.6f);
		Wait = ShotTime + 1.f;
		ShotStep = 12;
		return;
	}
	case 12:
	{
		if (ShotTime < Wait) return;
		// the difficulties through Rescale, the cap on his step scaled with them
		const EOutlawsDifficulty Was = L->Difficulty;
		for (AOutlawsEnemy* E : Bosses())
		{
			if (!E->IsPaceBoss() || E->IsDead() || E->bStationary) continue;
			UCharacterMovementComponent* M = E->GetCharacterMovement();
			const float Cap = E->Unscaled.RunSpeed * float(FOutlawsGeometry::UnitsToCm) * 0.6f;
			M->MaxWalkSpeed = E->Def.RunSpeed * float(FOutlawsGeometry::UnitsToCm) * E->BossMoveScale();
			struct FD { EOutlawsDifficulty D; float Want; const TCHAR* Name; };
			for (const FD& D : { FD{ EOutlawsDifficulty::Good, 0.48f, TEXT("Good") }, FD{ EOutlawsDifficulty::Bad, 0.6f, TEXT("Bad") }, FD{ EOutlawsDifficulty::Ugly, 0.84f, TEXT("Ugly") } })
			{
				E->Rescale(D.D);
				const float Share = M->MaxWalkSpeed / (E->Unscaled.RunSpeed * float(FOutlawsGeometry::UnitsToCm));
				Check(FMath::IsNearlyEqual(Share, D.Want, 0.005f), FString::Printf(TEXT("%s: %s runs at %.2f of his ITM's speed (%.0f cm/s of %.0f, wanted %.2f)"), D.Name, *E->GetOutlawsName(), Share, M->MaxWalkSpeed, Cap / 0.6f, D.Want));
			}
			E->Rescale(Was);
		}
		// co-op's x1.5 on the bad guys, not on him
		L->CoopPaceScale = 1.5f;
		Wait = ShotTime + 1.f;
		ShotStep = 13;
		return;
	}
	case 13:
	{
		if (ShotTime < Wait) return;
		for (AOutlawsEnemy* E : Bosses())
		{
			if (!E->IsPaceBoss() || E->IsDead()) continue;
			Check(FMath::IsNearlyEqual(E->CustomTimeDilation, 1.f, 0.01f) && FMath::IsNearlyEqual(E->BossMoveScale(), 0.6f, 0.001f), FString::Printf(TEXT("co-op's x1.5 (bad guys %.2f): %s stays at pace %.2f, scale %.2f"), L->EnemyPace(), *E->GetOutlawsName(), E->CustomTimeDilation, E->BossMoveScale()));
		}
		for (const FWatch& W : Watch)
		{
			if (W.bPlain && W.E.IsValid() && !W.E->IsDead()) Check(FMath::IsNearlyEqual(W.E->CustomTimeDilation, L->EnemyPace(), 0.02f), FString::Printf(TEXT("co-op's x1.5: the plain %s at %.2f, the bad guys' %.2f"), *W.Name, W.E->CustomTimeDilation, L->EnemyPace()));
		}
		L->CoopPaceScale = 1.f;
		ShotStep = 14;
		return;
	}
	// the long leg: the player out of the walking boss's sight far off, his top speed against the cap he has on
	case 14:
	{
		LegIdx = -1;
		for (int32 i = 0; i < Watch.Num(); ++i) if (Watch[i].E.IsValid() && Watch[i].E->IsPaceBoss() && !Watch[i].E->IsDead() && Watch[i].E->bHunter) LegIdx = i;
		Check(LegIdx >= 0, TEXT("a hunter for the long leg"));
		if (LegIdx < 0) { NextLevel(); return; }
		FWatch& W = Watch[LegIdx];
		AOutlawsEnemy* B = W.E.Get();
		const FVector From = B->GetActorLocation();
		FCollisionQueryParams Q;
		Q.AddIgnoredActor(B);
		Q.AddIgnoredActor(this);
		const AOutlawsEnemy* Spot = nullptr;
		double Best = 0.0;
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
		{
			const AOutlawsEnemy* E = Wk.Get();
			if (!E || E == B || E->IsDead()) continue;
			const double D = FVector::Dist2D(E->GetActorLocation(), From);
			FHitResult Hit;
			if (D > 2500.0 && D < 9000.0 && D > Best && GetWorld()->LineTraceSingleByChannel(Hit, From, E->GetActorLocation(), ECC_Visibility, Q)) { Best = D; Spot = E; }
		}
		Check(Spot != nullptr, TEXT("a spot out of his sight for the long leg"));
		if (!Spot) { NextLevel(); return; }
		TeleportTo(Spot->GetActorLocation() + FVector(150.0, 0.0, 40.0), GetActorRotation(), false, true);
		W.Top = W.CapSeen = W.Path = 0.f;
		W.Last = W.Mark = From;
		Began = SampleAt = ShotTime;
		ShotStep = 15;
		return;
	}
	case 15:
	{
		const bool bHalf = ShotTime - SampleAt >= 0.5f;
		if (bHalf) SampleAt = ShotTime;
		FWatch& W = Watch[LegIdx];
		Sample(W, Dt, ShotTime, FeetOfPlayer(), bHalf);
		if (ShotTime - Began > LegSec)
		{
			Check(W.CapSeen > 0.f && W.Top >= W.CapSeen * 0.85f && W.Top <= W.CapSeen * 1.15f + 20.f, FString::Printf(TEXT("%s's long leg: %.1f m in %.0f s, top %.0f cm/s against the cap on him %.0f"), *W.Name, W.Path / 100.f, LegSec, W.Top, W.CapSeen));
			NextLevel();
		}
		return;
	}
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
