// -OLShots -OLFreezeTest (user 2026-10-03): the far-and-unseen bad guys hold still (AOutlawsEnemy::UpdateFarFreeze) and a 3D
// sound from beyond hearing is not started (AOutlawsLevelActor::PlayWav), checked on a big open level (-OLLevel=canyon or
// ranch), each step logged:
//   1: the counts (frozen, active, carried by a lift and frozen: expect none), a frozen man picked; he holds still two seconds;
//   2: the player put 30 m from him: he thaws (what he was when he froze against what he is now: "OLSHOT freeze cycle");
//   3: the player back home: frozen again; shot from afar (TakeHit): thawed at once;
//   4: another frozen man: an alert reaches him (ShareTargetNear, the neighbours' word): thawed at once;
//   5: a sound at 50 m is skipped (the level's count), one at 10 m is not.
// -OLShots -OLFreezeSoak: stand at the start, enemies thinking, for 60 s (the counts every 10 s), then quit: OutlawsPerf's
// Saved/Perf/perf_timeline.csv has the game thread for an A/B with -OLFarFreeze=0 against 1.
// -OLFarFreeze=0|1 sets ol.FarFreezeEnable for the run (both modes).
// -OLShots -OLCpuBench [-OLBenchSec=150] (CPU2, 2026-10-05): the desktop benchmark. The player stands at the start, every
// living bad guy is alerted at 3 s (they all come for him; the player cannot be hurt), the counts and the throttle's tiers
// every 10 s, quit at 3 + BenchSec s. Pin it with -OLLevel=townex -OLNoFrontEnd -OLRando=2,1,0 -OLRandoSeed=1
// -OLSoloBadGuys=4; Saved/Perf has the numbers (tools/olue/questperf/cpu2_buckets.py).
// -OLShots -OLSwarmTest: the throttle's wake. The town left unalerted 20 s (the far crowd throttled), then a shot at the
// player's feet heard by all: the tiers and the number chasing every second for 15 s, and the time until half of the
// alerted are in the full tier.

#include "OutlawsPlayer.h"
#include "OutlawsFlags.h"
#include "OutlawsPerf.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsEnemy.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "OutlawsShotTests.h"

#if WITH_OUTLAWS_TESTS

void AOutlawsPlayer::FreezeTestTick(float Dt)
{
	AOutlawsLevelActor* L = Level.Get();
	UWorld* World = GetWorld();
	if (!L || !World) return;
	L->bEnemiesActive = true;                   // (ShotTest stands them down: this test is about them thinking, or not)
	static bool bSetUp = false;
	static bool bSoak = false;
	static bool bBench = false, bSwarm = false, bAlerted = false;
	static float BenchSec = 150.f, SwarmAt = -1.f, HalfFullAt = -1.f;
	static float NextCount = 0.f;
	static TWeakObjectPtr<AOutlawsEnemy> First, Second;
	static FVector Home = FVector::ZeroVector;
	static FString SnapA;
	if (!bSetUp)
	{
		bSetUp = true;
		bSoak = OLFlags::Param(TEXT("OLFreezeSoak"));
		bBench = OLFlags::Param(TEXT("OLCpuBench"));
		bSwarm = OLFlags::Param(TEXT("OLSwarmTest"));
		OLFlags::Value(TEXT("OLBenchSec"), BenchSec);
		int32 On = 1;
		if (OLFlags::Value(TEXT("OLFarFreeze"), On))
		{
			if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(TEXT("ol.FarFreezeEnable"))) V->Set(On, ECVF_SetByCode);
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT freeze %s: ol.FarFreezeEnable %d, level %s"), bSoak ? TEXT("soak") : TEXT("test"), On, *L->LevelName);
	}
	// (QSPEED-1) -OLEnemyDriverAB=<s>: from 3 s on, the engine's ticks (ol.enemy.Driver 0) and the enemy driver's (1) in turn, s
	// seconds each, the game thread's ms summed for each (the first second of a turn left out): an A/B in one run and one scene
	static float DriverAB = -1.f;
	static double AbSum[2] = { 0.0, 0.0 }, AbPhaseSum[2] = { 0.0, 0.0 };
	static int32 AbFrames[2] = { 0, 0 }, AbPhaseFrames[2] = { 0, 0 }, AbPhase = -1, AbPairs = 0, AbDriverWins = 0;
	static double AbDeltaSum = 0.0;
	static const TCHAR* AbBuckets[] = { TEXT("Eng: actor ticks (total)"), TEXT("Enemies (all)"), TEXT("Enemies: movement (CMC)"), TEXT("Enemies: driver (all)"),
		TEXT("Level tick (total)"), TEXT("Player tick (total)"), TEXT("Sprites (all)"), TEXT("Enemies: throttle") };
	static double AbBucketSum[2][8] = {};
	if (DriverAB < 0.f)
	{
		DriverAB = 0.f;
		OLFlags::Value(TEXT("OLEnemyDriverAB"), DriverAB);
	}
	auto AbLog = [&]()
	{
		if (DriverAB <= 0.f || AbFrames[0] == 0 || AbFrames[1] == 0) return;
		const double A = AbSum[0] / AbFrames[0], B = AbSum[1] / AbFrames[1];
		FString Bk;
		for (int32 k = 0; k < 8; ++k) Bk += FString::Printf(TEXT("%s%s %.3f -> %.3f"), k ? TEXT(", ") : TEXT(""), AbBuckets[k], AbBucketSum[0][k] / AbFrames[0], AbBucketSum[1][k] / AbFrames[1]);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT driver A/B buckets %s x%.2f (engine -> driver, ms a frame): %s"), *L->LevelName, L->SoloBadGuysWanted(), *Bk);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT driver A/B %s x%.2f: engine ticks %.3f ms game (%d frames), enemy driver %.3f ms (%d frames): %+.3f ms; %d turn pairs, mean pair delta %+.3f ms, driver faster in %d of them"),
			*L->LevelName, L->SoloBadGuysWanted(), A, AbFrames[0], B, AbFrames[1], B - A, AbPairs, AbPairs > 0 ? AbDeltaSum / AbPairs : 0.0, AbDriverWins);
	};
	if (DriverAB > 0.f && ShotTime > 3.f)
	{
		const int32 Phase = int32((ShotTime - 3.f) / DriverAB);
		const int32 Side = Phase % 2;                       // 0 the engine's ticks, 1 the driver's
		if (Phase != AbPhase)
		{
			// a pair (engine turn, driver turn) done: its delta
			if (AbPhase >= 0 && AbPhase % 2 == 1 && AbPhaseFrames[0] > 0 && AbPhaseFrames[1] > 0)
			{
				const double D = AbPhaseSum[1] / AbPhaseFrames[1] - AbPhaseSum[0] / AbPhaseFrames[0];
				AbDeltaSum += D;
				++AbPairs;
				if (D < 0.0) ++AbDriverWins;
				AbPhaseSum[0] = AbPhaseSum[1] = 0.0;
				AbPhaseFrames[0] = AbPhaseFrames[1] = 0;
			}
			AbPhase = Phase;
			if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(TEXT("ol.enemy.Driver"))) V->Set(Side, ECVF_SetByCode);
		}
		else if (ShotTime - 3.f - Phase * DriverAB > 1.f)
		{
			const double Ms = FPlatformTime::ToMilliseconds(GGameThreadTime);
			AbSum[Side] += Ms; ++AbFrames[Side];
			for (int32 k = 0; k < 8; ++k) AbBucketSum[Side][k] += OutlawsPerf::LastFrameMs(AbBuckets[k]);
			AbPhaseSum[Side] += Ms; ++AbPhaseFrames[Side];
		}
	}
	auto Counts = [&](const TCHAR* When)
	{
		int32 Frozen = 0, Active = 0, Carried = 0, CarriedFrozen = 0, Living = 0;
		for (const TWeakObjectPtr<AOutlawsEnemy>& W : L->GetEnemies())
		{
			const AOutlawsEnemy* E = W.Get();
			if (!E || E->IsDead() || E->IsDormant()) continue;
			++Living;
			const bool bCarried = L->IsCarriedBySector(E);
			if (bCarried) ++Carried;
			if (E->IsFarFrozen()) { ++Frozen; if (bCarried) ++CarriedFrozen; }
			else ++Active;
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT freeze counts %s: at %s: %d frozen, %d active of %d living; %d carried by a moving sector, %d of those frozen (expect 0)"),
			When, *GetActorLocation().ToCompactString(), Frozen, Active, Living, Carried, CarriedFrozen);
	};
	auto Tiers = [&](const TCHAR* When)
	{
		int32 Chasing = 0;
		for (const TWeakObjectPtr<AOutlawsEnemy>& W : L->GetEnemies())
		{
			const AOutlawsEnemy* E = W.Get();
			if (E && !E->IsDead() && !E->IsDormant() && E->GetState() != EOutlawsEnemyState::Watch) ++Chasing;
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT bench %s t=%.0f: %d not watching; %s | %s"), When, ShotTime, Chasing, *L->ThrottleSummary(), *AOutlawsEnemy::DriverStats());
		return Chasing;
	};
	if (bBench)
	{
		if (!bAlerted && ShotTime > 3.f)
		{
			bAlerted = true;
			int32 N = 0;
			for (const TWeakObjectPtr<AOutlawsEnemy>& W : L->GetEnemies())
			{
				AOutlawsEnemy* E = W.Get();
				if (E && !E->IsDead() && !E->IsDormant() && !E->IsVoiceOnly()) { E->Wake(false); ++N; }
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT bench alerted %d bad guys at %s"), N, *GetActorLocation().ToCompactString());
			NextCount = ShotTime + 10.f;
		}
		if (bAlerted && ShotTime >= NextCount) { NextCount = ShotTime + 10.f; Counts(TEXT("bench")); Tiers(TEXT("tiers")); }
		if (ShotTime > 3.f + BenchSec)
		{
			AbLog();
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT bench done"));
			FGenericPlatformMisc::RequestExit(false);
			ShotTime = -1000.f;
		}
		return;
	}
	if (bSwarm)
	{
		if (SwarmAt < 0.f && ShotTime > 20.f)
		{
			SwarmAt = ShotTime;
			Tiers(TEXT("swarm before"));
			// a shot heard by every bad guy in the level (the throttled too)
			for (const TWeakObjectPtr<AOutlawsEnemy>& W : L->GetEnemies())
			{
				if (AOutlawsEnemy* E = W.Get()) if (!E->IsDead() && !E->IsDormant() && !E->IsVoiceOnly()) E->Wake(false);
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT swarm shot at %s"), *GetActorLocation().ToCompactString());
			NextCount = ShotTime + 1.f;
		}
		else if (SwarmAt >= 0.f && ShotTime >= NextCount)
		{
			NextCount = ShotTime + 1.f;
			Tiers(TEXT("swarm"));
			if (HalfFullAt < 0.f && L->ThrottleFullShare() >= 0.5f)
			{
				HalfFullAt = ShotTime;
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT swarm half the alerted in the full tier %.1f s after the shot"), HalfFullAt - SwarmAt);
			}
		}
		if (SwarmAt >= 0.f && ShotTime > SwarmAt + 15.f)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT swarm done (%s)"), HalfFullAt < 0.f ? TEXT("FAIL: never half full") : TEXT("PASS"));
			FGenericPlatformMisc::RequestExit(false);
			ShotTime = -1000.f;
		}
		return;
	}
	auto PickFrozen = [&](const AOutlawsEnemy* Not, bool bModeZero) -> AOutlawsEnemy*
	{
		// the frozen watcher farthest off (the clearest case); bModeZero: a normal one (a sniper, an ambusher takes no
		// neighbour's word: the game's own rule)
		AOutlawsEnemy* Best = nullptr;
		double BestD = -1.0;
		for (const TWeakObjectPtr<AOutlawsEnemy>& W : L->GetEnemies())
		{
			AOutlawsEnemy* E = W.Get();
			if (!E || E == Not || !E->IsFarFrozen() || E->GetState() != EOutlawsEnemyState::Watch || E->IsCritter() || (bModeZero && E->GetMode() != 0)) continue;
			const double D = FVector::Dist(E->GetActorLocation(), GetActorLocation());
			if (D > BestD) { BestD = D; Best = E; }
		}
		return Best;
	};
	auto Describe = [](const AOutlawsEnemy* E) { return E ? FString::Printf(TEXT("%s frozen %d [%s]"), *E->GetOutlawsName(), E->IsFarFrozen() ? 1 : 0, *E->DebugFreezeState()) : FString(TEXT("none")); };

	if (bSoak)
	{
		if (ShotTime > 3.f && ShotTime >= NextCount)
		{
			NextCount = ShotTime + 10.f;
			Counts(TEXT("soak"));
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT freeze soak t=%.0f: %s"), ShotTime, *AOutlawsEnemy::DriverStats());
		}
		static const bool bSoakLength = OLFlags::HasValue(TEXT("OLBenchSec"));        // (-OLBenchSec sets the soak's length too)
		if (ShotTime > (bSoakLength ? 3.f + BenchSec : 64.f))
		{
			Counts(TEXT("soak end"));
			AbLog();
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT freeze soak done"));
			FGenericPlatformMisc::RequestExit(false);
			ShotTime = -1000.f;
		}
		return;
	}

	if (ShotStep == 0 && ShotTime > 4.f)
	{
		Home = GetActorLocation();
		Counts(TEXT("1 start"));
		First = PickFrozen(nullptr, false);
		SnapA = First.IsValid() ? First->DebugFreezeState() : FString();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT freeze 1: picked %s%s"), *Describe(First.Get()), First.IsValid() ? TEXT("") : TEXT(" FAIL: nobody frozen"));
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 6.f)
	{
		if (AOutlawsEnemy* E = First.Get())
		{
			const FString Now = E->DebugFreezeState();
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT freeze 1: two seconds on, still frozen %d (expect 1), unchanged %s"), E->IsFarFrozen() ? 1 : 0, Now == SnapA ? TEXT("yes") : TEXT("NO"));
			// the player put 30 m from him, the eye at his height: inside the thaw distance (nine tenths of 100 m)
			FVector Away = Home - E->GetActorLocation();
			Away.Z = 0.0;
			const FVector Spot = E->GetActorLocation() + Away.GetSafeNormal() * 3000.0 + FVector(0.0, 0.0, 50.0);
			TeleportTo(Spot, GetActorRotation(), false, true);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT freeze 2: player moved to %s, %.0f cm from him"), *Spot.ToCompactString(), FVector::Dist(Spot, E->GetActorLocation()));
		}
		++ShotStep;
	}
	else if (ShotStep == 2 && ShotTime > 8.f)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT freeze 2: near him two seconds: %s (expect frozen 0)"), *Describe(First.Get()));
		Counts(TEXT("2 near"));
		TeleportTo(Home, GetActorRotation(), false, true);
		++ShotStep;
	}
	else if (ShotStep == 3 && ShotTime > 11.f)
	{
		if (AOutlawsEnemy* E = First.Get())
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT freeze 3: home again three seconds: %s (expect frozen 1 unless he was alerted: state 1)"), *Describe(E));
			// shot from afar: a light hit from the player's side
			const FVector From = GetActorLocation();
			const FVector Dir = (E->GetActorLocation() - From).GetSafeNormal();
			E->TakeHit(0.25f, E->GetActorLocation(), Dir, From);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT freeze 3: hit from %.0f cm: %s (expect frozen 0)"), FVector::Dist(From, E->GetActorLocation()), *Describe(E));
		}
		++ShotStep;
	}
	else if (ShotStep == 4 && ShotTime > 12.f)
	{
		Second = PickFrozen(First.Get(), true);
		if (AOutlawsEnemy* E = Second.Get())
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT freeze 4: picked %s (mode %d)"), *Describe(E), E->GetMode());
			// a neighbour's alert reaches him (the share-target word, as a bad guy who spots the player passes it on)
			L->ShareTargetNear(nullptr, E->GetActorLocation(), 100.f);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT freeze 4: alerted by a neighbour: %s (expect frozen 0, state 1)"), *Describe(E));
		}
		else
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT freeze 4: FAIL nobody else frozen"));
		}
		++ShotStep;
	}
	else if (ShotStep == 5 && ShotTime > 14.f)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT freeze 4: two seconds on: %s (expect frozen 0: he acts on it a while)"), *Describe(Second.Get()));
		Counts(TEXT("5 end"));
		// the sounds: one 50 m off, one 10 m off
		const FVector Eye = Camera->GetComponentLocation();
		const FVector Fwd = Camera->GetForwardVector();
		const int32 N0 = L->FarSoundsSkipped;
		L->PlaySoundAt(TEXT("jajump"), Eye + Fwd * 5000.0);
		const int32 N1 = L->FarSoundsSkipped;
		L->PlaySoundAt(TEXT("jajump"), Eye + Fwd * 1000.0);
		const int32 N2 = L->FarSoundsSkipped;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT freeze 5: sound at 50 m skipped %d (expect 1), at 10 m skipped %d (expect 0); %d far sounds skipped so far this level"), N1 - N0, N2 - N1, N2);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT freeze test done"));
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1000.f;
	}
}

OL_SHOT_MATCH(OLFreezeTest) { return OLFlags::Param(TEXT("OLFreezeTest")) || OLFlags::Param(TEXT("OLFreezeSoak")) || OLFlags::Param(TEXT("OLCpuBench")) || OLFlags::Param(TEXT("OLSwarmTest")); }
OL_SHOT_TEST(OLFreezeTest, 250)
{
	FreezeTestTick(Dt);              // (OutlawsPlayerFreezeTest.cpp)
}
#endif // WITH_OUTLAWS_TESTS
