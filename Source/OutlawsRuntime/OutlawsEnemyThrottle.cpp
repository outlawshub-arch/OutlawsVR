// CPU2 (2026-10-05): the enemy throttle.  A town of 347 bad guys (bad guys x4), ~260 of them awake once it is alerted, cost
// the Quest 3's game thread ~10 ms in their own ticks and their movement components alone (target 13.9 ms at 72 Hz).
// Each frame the host (or single player) ranks the awake bad guys and gives each a tier (AOutlawsEnemy::SetThrottleTier):
//   0 FULL       thinks and moves every frame
//   1 MOVE-ONLY  moves every frame on his last plan, thinks every ol.throttle.ThinkInterval s
//   2 WANDER     no movement component: a slow walk inside his own sector (AOutlawsEnemy::TickWander)
//   3 FROZEN     no tick at all, nor his sprite's
// Rank: fighting (just hit, or chasing a player he can see, or firing) first, then in a player's view (his own sight, the
// host's visible sectors, a co-op client within 20 m) or just woken (an alert, a shot heard), then the nearest to any player.
// Caps by device (Quest 3 / Quest 2; the PC has none until it runs over budget), each a console variable:
//   ol.throttle.Think (FULL), ol.throttle.Move (FULL + MOVE-ONLY), ol.throttle.Wander, ol.throttle.Attack (firing at once,
//   per targeted player); -1 the device's (Quest 2 by OutlawsDeviceProfile::IsQuest2).  ol.throttle.Profile -1 by device, 0 PC, 2 Quest 2, 3 Quest 3 (also
//   -OLDeviceProfile=PC|Quest2|Quest3 on the command line).  ol.throttle.Enable 0 puts everyone in FULL.
// While the game thread (smoothed, less the XR runtime's frame wait: ol.throttle.SubtractXRWait) runs over 1000 / ol.perf.TargetFps, the caps tighten a step a second (x0.8, x0.65,
// x0.5 on a Quest; the PC from none to x2, x1.5, x1 the Quest 3's) and relax a step after 3 s back under 85 %.
// Promotions out of WANDER or FROZEN go through a wake queue, ol.throttle.WakePerFrame a frame (a shot in town does not wake
// 200 at once); one just hit goes FULL at once (AOutlawsEnemy::TakeHit).  Demotions wait 1 s in a tier (no flicker), and
// one fighting in a player's view is never throttled.

#include "OutlawsLevelActor.h"
#include "OutlawsFlags.h"
#include "OutlawsEnemy.h"
#include "OutlawsCoop.h"
#include "OutlawsPerf.h"
#include "OutlawsRuntime.h"
#include "OutlawsDeviceProfile.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "RenderCore.h"
#include "ProfilingDebugging/CsvProfiler.h"

CSV_DECLARE_CATEGORY_EXTERN(Outlaws);

namespace
{
	TAutoConsoleVariable<int32> CVarThrottleEnable(TEXT("ol.throttle.Enable"), 1, TEXT("CPU2 enemy throttle: 1 on, 0 off (every bad guy thinks and moves every frame)."));
	TAutoConsoleVariable<int32> CVarThrottleProfile(TEXT("ol.throttle.Profile"), -1, TEXT("Caps of: -1 the device, 0 PC (none until over budget), 2 Quest 2, 3 Quest 3."));
	TAutoConsoleVariable<int32> CVarThrottleThink(TEXT("ol.throttle.Think"), -1, TEXT("FULL tier cap (thinking); -1 the profile's (Quest 3 32, Quest 2 16)."));
	TAutoConsoleVariable<int32> CVarThrottleMove(TEXT("ol.throttle.Move"), -1, TEXT("FULL + MOVE-ONLY cap (moving); -1 the profile's (Quest 3 40, Quest 2 20)."));
	TAutoConsoleVariable<int32> CVarThrottleWander(TEXT("ol.throttle.Wander"), -1, TEXT("WANDER cap; -1 the profile's (Quest 3 80, Quest 2 40); past it FROZEN."));
	TAutoConsoleVariable<int32> CVarThrottleAttack(TEXT("ol.throttle.Attack"), -1, TEXT("Bad guys firing at once per targeted player; -1 the profile's (Quest 3 6, Quest 2 4)."));
	TAutoConsoleVariable<float> CVarThrottleThinkInterval(TEXT("ol.throttle.ThinkInterval"), 0.5f, TEXT("MOVE-ONLY: seconds between thinks."));
	TAutoConsoleVariable<int32> CVarThrottleWakePerFrame(TEXT("ol.throttle.WakePerFrame"), 4, TEXT("Promotions out of WANDER / FROZEN a frame (the wake queue)."));
	TAutoConsoleVariable<int32> CVarThrottleDynamic(TEXT("ol.throttle.Dynamic"), 1, TEXT("1: tighten the caps while the game thread is over budget; 0: the profile's caps as they are."));
	TAutoConsoleVariable<int32> CVarThrottleSubtractXRWait(TEXT("ol.throttle.SubtractXRWait"), 1, TEXT("1: the budget held against the game thread less the XR runtime's frame wait (xrWaitFrame, the world's start); 0: the whole game thread."));
	TAutoConsoleVariable<int32> CVarThrottleForceLevel(TEXT("ol.throttle.ForceLevel"), -1, TEXT("Tests: hold the caps at this step (0 to 3; 3 the tightest, x0.5 on a Quest); -1 as the budget gives."));

	int32 ProfileNow()
	{
		static int32 FromCmd = [] {
			FString P;
			if (!OLFlags::Value(TEXT("OLDeviceProfile"), P)) return -1;
			return P.Equals(TEXT("Quest3"), ESearchCase::IgnoreCase) ? 3 : P.Equals(TEXT("Quest2"), ESearchCase::IgnoreCase) ? 2 : 0;
		}();
		const int32 V = CVarThrottleProfile.GetValueOnGameThread();
		if (V >= 0) return V;
		if (FromCmd >= 0) return FromCmd;
		if (OutlawsDeviceProfile::IsQuest2()) return 2;
		return PLATFORM_ANDROID ? 3 : 0;
	}

	const TCHAR* ProfileName(int32 P) { return P == 3 ? TEXT("Quest3") : P == 2 ? TEXT("Quest2") : TEXT("PC"); }
}

bool AOutlawsLevelActor::IsThrottleExempt(const AOutlawsEnemy* E) const
{
	// never throttled, always FULL and never counted against a cap: the named outlaws (every boss, the Historical
	// Missions' outlaw among them: the level's end waits on them, and Bob's door, Slim's and Doc's ground are theirs),
	// a voice, one held, thrown, knocked, taken alive, flinching or dying (a death or a capture plays through on every
	// frame), and one an INF lift or moving floor carries (the script moves him); a match bot's brain (REQ-MPBOTS: it plays
	// as a player, and is not in the level's list of bad guys either)
	return E->CoopSelf >= 0 || E->bBoss || E->bHunter || E->bStationary || E->bPeekShooter || E->bBehindDoor || E->bVoiceOnly || E->bTakenAlive
		|| E->bHeldByPlayer || E->bThrown || E->bKnockFlight || !E->PendingKnock.IsNearlyZero()
		|| E->State == EOutlawsEnemyState::Hit || E->IsDead() || IsCarriedBySector(E);
}

float AOutlawsLevelActor::ThrottleThinkInterval() const
{
	return FMath::Max(0.05f, CVarThrottleThinkInterval.GetValueOnGameThread());
}

bool AOutlawsLevelActor::TakeAttackToken(const AOutlawsEnemy* E)
{
	if (!E || ThrottleCaps[3] <= 0 || IsThrottleExempt(E)) return true;     // (a named outlaw always fires)
	const int32 T = FMath::Clamp(E->CoopTarget, 0, 7);
	if (ThrottleAttackCount[T] >= ThrottleCaps[3])
	{
		++ThrottleBinds[3];
		return false;
	}
	++ThrottleAttackCount[T];
	return true;
}

FString AOutlawsLevelActor::ThrottleSummary() const
{
	return FString::Printf(TEXT("throttle %s level %d game %.1f ms xr wait %.1f effective %.1f | caps think %d move %d wander %d attack %d | full %d move %d wander %d frozen %d of %d | queue %d | binds/s %.0f %.0f %.0f %.0f"),
		ProfileName(ProfileNow()), ThrottleLevel, ThrottleSmoothMs, ThrottleSmoothWaitMs, ThrottleEffMs, ThrottleCaps[0], ThrottleCaps[1], ThrottleCaps[2], ThrottleCaps[3],
		ThrottleCounts[0], ThrottleCounts[1], ThrottleCounts[2], ThrottleCounts[3], ThrottleEligible, ThrottleWakeQueue,
		ThrottleBindRate[0], ThrottleBindRate[1], ThrottleBindRate[2], ThrottleBindRate[3]);
}

float AOutlawsLevelActor::ThrottleFullShare() const
{
	const int32 Room = ThrottleCaps[0] > 0 ? FMath::Min(ThrottleCaps[0], ThrottleEligible) : ThrottleEligible;
	return Room > 0 ? float(ThrottleCounts[0]) / float(Room) : 1.f;
}

void AOutlawsLevelActor::TickEnemyThrottle(float Dt)
{
	OL_PERF_SCOPE("Enemies: throttle");
	UWorld* World = GetWorld();
	if (!World) return;
	const double Now = FPlatformTime::Seconds();
	const bool bOn = CVarThrottleEnable.GetValueOnGameThread() != 0;
	// (Q2PROFILE: after an app resume the level starts again at 0; the suspend's long frame says nothing of the load)
	if (ThrottleResumeSeen != OutlawsDeviceProfile::ResumeCount())
	{
		UE_LOG(LogOutlawsAI, Log, TEXT("OLRT throttle: app resumed, level %d (game %.1f ms) back to 0"), ThrottleLevel, ThrottleSmoothMs);
		ThrottleResumeSeen = OutlawsDeviceProfile::ResumeCount();
		ThrottleLevel = 0;
		ThrottleSmoothMs = 0.0;
		ThrottleSmoothWaitMs = 0.0;
		ThrottleOverFor = ThrottleUnderFor = 0.f;
	}

	// the budget: the game thread smoothed over ~half a second, against the refresh rate's frame (ol.perf.TargetFps follows
	// the Refresh Rate row, REQ-REFRESH).  On the headset GGameThreadTime holds the XR runtime's xrWaitFrame (its pacing,
	// ~3.4 ms at the refresh: build 31's Sanctuary sat at step 3 on it), so the frame wait the world's start measured is
	// taken off it first: our own work against the budget
	const double GameMs = FPlatformTime::ToMilliseconds(GGameThreadTime);
	const double WaitMs = FMath::Min(OutlawsPerf::XRWaitMs(), GameMs);
	const double Blend = FMath::Clamp(double(Dt) * 2.0, 0.0, 1.0);
	ThrottleSmoothMs = ThrottleSmoothMs <= 0.0 ? GameMs : FMath::Lerp(ThrottleSmoothMs, GameMs, Blend);
	ThrottleSmoothWaitMs = FMath::Lerp(ThrottleSmoothWaitMs, WaitMs, Blend);
	ThrottleEffMs = CVarThrottleSubtractXRWait.GetValueOnGameThread() != 0 ? FMath::Max(0.0, ThrottleSmoothMs - ThrottleSmoothWaitMs) : ThrottleSmoothMs;
	static IConsoleVariable* TargetFps = IConsoleManager::Get().FindConsoleVariable(TEXT("ol.perf.TargetFps"));
	const double Budget = 1000.0 / FMath::Max(1.f, TargetFps ? TargetFps->GetFloat() : 72.f);
	const int32 ForceLevel = CVarThrottleForceLevel.GetValueOnGameThread();
	if (bOn && ForceLevel >= 0)
	{
		ThrottleLevel = FMath::Clamp(ForceLevel, 0, 3);
	}
	else if (bOn && CVarThrottleDynamic.GetValueOnGameThread() != 0)
	{
		if (ThrottleEffMs > Budget * 1.05) { ThrottleOverFor += Dt; ThrottleUnderFor = 0.f; }
		else if (ThrottleEffMs < Budget * 0.85) { ThrottleUnderFor += Dt; ThrottleOverFor = 0.f; }
		else { ThrottleOverFor = 0.f; ThrottleUnderFor = 0.f; }
		if (ThrottleOverFor > 1.f && ThrottleLevel < 3) { ++ThrottleLevel; ThrottleOverFor = 0.f; }
		if (ThrottleUnderFor > 3.f && ThrottleLevel > 0) { --ThrottleLevel; ThrottleUnderFor = 0.f; }
	}
	else
	{
		ThrottleLevel = 0;
	}

	// the caps in use
	const int32 Profile = ProfileNow();
	const bool bQuest = Profile >= 2;
	// (BUG-STUTTER2: Quest 3 raised from 24 / 32 / 60, build 32 ran with 2 to 3 ms of game thread to spare)
	int32 Base[4] = { Profile == 2 ? 16 : 32, Profile == 2 ? 20 : 40, Profile == 2 ? 40 : 80, Profile == 2 ? 4 : 6 };
	const int32 Over[4] = { CVarThrottleThink.GetValueOnGameThread(), CVarThrottleMove.GetValueOnGameThread(), CVarThrottleWander.GetValueOnGameThread(), CVarThrottleAttack.GetValueOnGameThread() };
	for (int32 i = 0; i < 4; ++i) if (Over[i] >= 0) Base[i] = Over[i];
	static const float QuestScale[4] = { 1.f, 0.8f, 0.65f, 0.5f };
	static const float PcScale[4] = { 0.f, 2.f, 1.5f, 1.f };
	const float Scale = !bOn ? 0.f : bQuest ? QuestScale[ThrottleLevel] : PcScale[ThrottleLevel];
	for (int32 i = 0; i < 4; ++i) ThrottleCaps[i] = Scale > 0.f && Base[i] > 0 ? FMath::Max(i == 3 ? 1 : 4, FMath::RoundToInt(Base[i] * Scale)) : 0;
	ThrottleCaps[1] = ThrottleCaps[1] > 0 ? FMath::Max(ThrottleCaps[1], ThrottleCaps[0]) : 0;
	if (!bThrottleLogged)
	{
		bThrottleLogged = true;
		UE_LOG(LogOutlawsAI, Display, TEXT("OLRT throttle: profile %s, enabled %d, caps think %d move %d wander %d attack %d (level 0; past them FROZEN), think interval %.2f s, wake %d a frame, budget %.1f ms, dynamic %d"),
			ProfileName(Profile), bOn ? 1 : 0, ThrottleCaps[0], ThrottleCaps[1], ThrottleCaps[2], ThrottleCaps[3], ThrottleThinkInterval(),
			CVarThrottleWakePerFrame.GetValueOnGameThread(), Budget, CVarThrottleDynamic.GetValueOnGameThread());
	}

	// the players: the host's own and (co-op) every client's
	TArray<FVector, TInlineAllocator<8>> Players;
	const APlayerController* PC = World->GetFirstPlayerController();
	if (const APawn* P = PC ? PC->GetPawn() : nullptr) Players.Add(P->GetActorLocation());
	const UOutlawsCoop* Coop = nullptr;
	for (const TWeakObjectPtr<AOutlawsEnemy>& W : Enemies) if (const AOutlawsEnemy* E = W.Get()) { Coop = E->Coop.Get(); break; }
	if (Coop)
	{
		for (const FOutlawsCoopRemoteView& V : Coop->RemoteViews) if (V.bValid && !V.bDead) Players.Add(V.Feet);
	}

	// this frame's shooters at each player (the attack tokens) and the ranking
	for (int32& C : ThrottleAttackCount) C = 0;
	struct FRank { AOutlawsEnemy* E; double Key; bool bPinned; bool bHit; int32 Class; };
	TArray<FRank, TInlineAllocator<512>> Ranked;
	for (const TWeakObjectPtr<AOutlawsEnemy>& W : Enemies)
	{
		AOutlawsEnemy* E = W.Get();
		if (!E) continue;
		const bool bShooting = E->Sub == AOutlawsEnemy::SubShoot || E->Sub == AOutlawsEnemy::SubShootMove;
		const bool bExempt = IsThrottleExempt(E);
		if (bShooting && !bExempt) ++ThrottleAttackCount[FMath::Clamp(E->CoopTarget, 0, 7)];
		const bool bEligible = bOn && ThrottleCaps[0] > 0 && E->CoopRole != 2 && !bExempt && !E->bDormant && !E->bFarFrozen;
		if (!bEligible)
		{
			E->SetThrottleTier(0);
			continue;
		}
		const FVector Here = E->GetActorLocation();
		double D2 = 1e12;
		for (const FVector& P : Players) D2 = FMath::Min(D2, FVector::DistSquared(P, Here));
		// his sector, looked up again once he has moved 30 cm
		if (E->ThrottleSector == -2 || FVector::DistSquared(Here, E->ThrottleSectorAt) > 900.0)
		{
			E->ThrottleSector = CullSectorOf(Here, &E->CullHint);
			E->ThrottleSectorAt = Here;
		}
		const bool bHit = Now - E->ThrottleHitAt < 5.0;
		const bool bSees = E->bSeen && E->ThrottleTier <= 1;           // (his own sight is fresh only while he thinks)
		const bool bInView = bSees || IsSectorSeen(E->ThrottleSector) || (Players.Num() > 1 && E->CoopRemoteNear(2000.0));
		const bool bFighting = bHit || bShooting || (E->State == EOutlawsEnemyState::Chase && bSees);
		const bool bWoken = Now - E->ThrottleWakeAt < 3.0;
		const int32 Class = bFighting ? 0 : (bInView || bWoken) ? 1 : 2;
		Ranked.Add({ E, Class * 1e13 + D2, bFighting && bInView, bHit && Now - E->ThrottleHitAt < 1.0, Class });
	}
	ThrottleEligible = Ranked.Num();
	Ranked.Sort([](const FRank& A, const FRank& B) { return A.Key < B.Key; });

	int32 WakeLeft = FMath::Max(1, CVarThrottleWakePerFrame.GetValueOnGameThread());
	int32 Want[4] = { 0, 0, 0, 0 };
	ThrottleWakeQueue = 0;
	for (int32& C : ThrottleCounts) C = 0;
	bool bBound[3] = { false, false, false };
	for (const FRank& R : Ranked)
	{
		uint8 Tier;
		if (R.bPinned || Want[0] < ThrottleCaps[0]) Tier = 0;
		else if (Want[0] + Want[1] < ThrottleCaps[1]) Tier = 1;
		else if (Want[2] < ThrottleCaps[2]) Tier = 2;
		else Tier = 3;
		++Want[Tier];
		// what each cap turned away: one fighting or in view kept from thinking (think), from moving (move); anyone FROZEN (wander)
		if (R.Class <= 1 && Tier >= 1) bBound[0] = true;
		if (R.Class <= 1 && Tier >= 2) bBound[1] = true;
		if (Tier == 3) bBound[2] = true;
		AOutlawsEnemy* E = R.E;
		const uint8 Cur = E->ThrottleTier;
		if (Tier < Cur)
		{
			// up: from MOVE-ONLY at once; out of WANDER or FROZEN through the wake queue (one just hit at once)
			if (Cur <= 1 || R.bHit) E->SetThrottleTier(Tier);
			else if (WakeLeft > 0) { --WakeLeft; E->SetThrottleTier(Tier); }
			else ++ThrottleWakeQueue;
		}
		else if (Tier > Cur && !R.bPinned && Now - E->ThrottleTierSince > 1.0)
		{
			// down, after a second in his tier (one the INF carries is exempt above)
			E->SetThrottleTier(Tier);
		}
		++ThrottleCounts[E->ThrottleTier];
	}

	// how often each cap binds, per second
	// (BUG-STUTTER2: each cap counted on its own; the three had counted every frame with more eligible than the cap,
	// the same frames for all three on a crowded level)
	for (int32 i = 0; i < 3; ++i) if (ThrottleCaps[i] > 0 && bBound[i]) ++ThrottleBinds[i];
	if (Now - ThrottleBindSince >= 1.0)
	{
		const float Secs = float(Now - ThrottleBindSince);
		for (int32 i = 0; i < 4; ++i) { ThrottleBindRate[i] = ThrottleBindSince > 0.0 ? ThrottleBinds[i] / Secs : 0.f; ThrottleBinds[i] = 0; }
		ThrottleBindSince = Now;
	}

	// co-op: every 10 s the tiers of those after each player, and any after a client within 15 m of him left in WANDER or
	// FROZEN (expect none: his attackers act in front of him as the host's do)
	static double NextByTarget = 0.0;
	if (Players.Num() > 1 && Now >= NextByTarget)
	{
		NextByTarget = Now + 10.0;
		int32 By[8][4] = {};
		int32 NearThrottled = 0;
		for (const FRank& R : Ranked)
		{
			const AOutlawsEnemy* E = R.E;
			if (E->State != EOutlawsEnemyState::Chase) continue;
			const int32 T = FMath::Clamp(E->CoopTarget, 0, 7);
			++By[T][E->ThrottleTier];
			if (T > 0 && E->ThrottleTier >= 2 && Coop)
			{
				const FOutlawsCoopRemoteView* V = Coop->RemoteViewOf(T);
				if (V && V->bValid && FVector::DistSquared(V->Feet, E->GetActorLocation()) < FMath::Square(1500.0)) ++NearThrottled;
			}
		}
		FString S;
		for (int32 T = 0; T < 8; ++T) if (By[T][0] + By[T][1] + By[T][2] + By[T][3] > 0) S += FString::Printf(TEXT(" | p%d full %d move %d wander %d frozen %d"), T, By[T][0], By[T][1], By[T][2], By[T][3]);
		UE_LOG(LogOutlawsAI, Display, TEXT("OLCOOP throttle by target:%s | a client's within 15 m throttled: %d (expect 0)"), *S, NearThrottled);
	}

	CSV_CUSTOM_STAT(Outlaws, ThrottleLevel, ThrottleLevel, ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(Outlaws, ThrottleGameMs, float(ThrottleSmoothMs), ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(Outlaws, ThrottleXRWaitMs, float(ThrottleSmoothWaitMs), ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(Outlaws, ThrottleEffMs, float(ThrottleEffMs), ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(Outlaws, ThrottleFull, ThrottleCounts[0], ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(Outlaws, ThrottleMoveOnly, ThrottleCounts[1], ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(Outlaws, ThrottleWander, ThrottleCounts[2], ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(Outlaws, ThrottleFrozen, ThrottleCounts[3], ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(Outlaws, ThrottleCapThink, ThrottleCaps[0], ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(Outlaws, ThrottleCapMove, ThrottleCaps[1], ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(Outlaws, ThrottleCapWander, ThrottleCaps[2], ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(Outlaws, ThrottleCapAttack, ThrottleCaps[3], ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(Outlaws, ThrottleBindThinkPerSec, ThrottleBindRate[0], ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(Outlaws, ThrottleBindMovePerSec, ThrottleBindRate[1], ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(Outlaws, ThrottleBindWanderPerSec, ThrottleBindRate[2], ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(Outlaws, ThrottleAttackRefusedPerSec, ThrottleBindRate[3], ECsvCustomStatOp::Set);
	CSV_CUSTOM_STAT(Outlaws, ThrottleWakeQueue, ThrottleWakeQueue, ECsvCustomStatOp::Set);
}
