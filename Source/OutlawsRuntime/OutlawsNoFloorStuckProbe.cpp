// BUG-NOFLOORSTUCK's probe (from lane BOSSWALL's hideout scan, 2026-10-28): -OLLevel=hideout -OLNoFrontEnd -OLNoFloorStuckProbe
// [-OLOldNoFloorStuck].  In the hideout a hunter whose next step was sector 333's opening 8 into 87 stood still about 25 s,
// speed 0, nothing blocking him: his look for floor ahead went through the wall beside the opening and found none.  Every
// other bad guy put dormant, the player invincible, the player past the opening out of his view.  (1) a bad guy made a hunter
// put on that spot; (2) an ordinary bad guy there sent straight at the step.  Each: the longest spell he stands still on a
// move (under 25 cm in a second, not shooting), when he is through the opening (80 cm from its step) and when the player is
// first in his sight (he sees them, or a clear line from his eye to theirs), and at 1.5 s what lies ahead of him (floor, a
// wall before it).  All in his own time (the bad guys run at the player's pace: 0.3 of real time while the player stands
// still).  Pass: no spell over 2 s, and the hunter through or in sight within 8 s.  Lines "OLSHOT nofloorstuck check
// PASS|FAIL" and "OLSHOT nofloorstuck RESULT".
#include "OutlawsEnemy.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsGeometry.h"
#include "OutlawsFlags.h"
#include "Components/CapsuleComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsNoFloorStuckProbeLocal
{
	// the spot lane BOSSWALL's frame scan found (bw_hideout.log): his feet, the hunt's step through 333/8, the player
	const FVector SpotFeet(6686.40, 19965.00, -90.00);
	const FVector StepFeet(6420.15, 19912.50, -60.00);
	const FVector PlayerFeet(6420.15, 19800.00, -60.00);

	double T = 0.0, NextLog = 0.0, CaseT = 0.0, StillSince = 0.0, Longest = 0.0, SeenAt = -1.0, ThroughAt = -1.0, Pace = 1.0;
	FVector Anchor = FVector::ZeroVector, Start = FVector::ZeroVector;
	int32 CaseI = -1, Passes = 0, Fails = 0;
	bool bDumped = false, bWasHunter = false;
	TWeakObjectPtr<AOutlawsEnemy> Man;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT nofloorstuck check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}

	bool IsMoving(const AOutlawsEnemy* E)
	{
		const FString St = E->DebugBossState();
		return E->GetState() != EOutlawsEnemyState::Shoot && (St.Contains(TEXT("sub 3 ")) || St.Contains(TEXT("sub 4 ")) || St.Contains(TEXT("sub 8 ")));
	}

	/** What lies ahead of him toward his move's target, as MoveAlong looks: floor under the spot, a wall before it. */
	FString Ahead(UWorld* W, const AOutlawsEnemy* E)
	{
		const FVector Here = E->GetActorLocation();
		const float Half = E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		const float Rad = E->GetCapsuleComponent()->GetScaledCapsuleRadius();
		const FVector Dir = (E->DebugMoveTarget() - Here).GetSafeNormal2D();
		const FVector A = Here + Dir * (Rad + 30.0);
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OLNoFloorProbe), true, E);
		FCollisionResponseParams RP;
		RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
		FHitResult F, Wl;
		const bool bF = W->LineTraceSingleByChannel(F, A, A - FVector(0.0, 0.0, Half + 15.0 * FOutlawsGeometry::UnitsToCm), ECC_Visibility, Q, RP);
		const bool bW = W->LineTraceSingleByChannel(Wl, Here, A, ECC_Visibility, Q, RP);
		return FString::Printf(TEXT("the spot ahead %s: floor %s; between %s"), *A.ToCompactString(),
			bF ? *FString::Printf(TEXT("at %.0f"), F.ImpactPoint.Z) : TEXT("none"),
			bW ? *FString::Printf(TEXT("%s at %s normal %s"), *GetNameSafe(Wl.GetComponent()), *Wl.ImpactPoint.ToCompactString(), *Wl.ImpactNormal.ToCompactString()) : TEXT("nothing"));
	}

	void Begin(APawn* Pawn)
	{
		AOutlawsEnemy* E = Man.Get();
		if (!E) return;
		Pawn->TeleportTo(PlayerFeet + FVector(0.0, 0.0, Pawn->GetSimpleCollisionHalfHeight() + 5.0), (SpotFeet - PlayerFeet).Rotation(), false, true);
		E->bHunter = CaseI == 0;
		const float Half = E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		if (E->IsDormant()) { E->SetDormant(false); E->Appear(SpotFeet); }
		else E->SetActorLocation(SpotFeet + FVector(0.0, 0.0, Half + 2.0), false, nullptr, ETeleportType::TeleportPhysics);
		E->Wake(false);
		if (CaseI == 1) E->DebugSwimTo(StepFeet + FVector(0.0, 0.0, Half));
		CaseT = StillSince = Longest = 0.0;
		SeenAt = ThroughAt = -1.0;
		bDumped = false;
		Anchor = Start = E->GetActorLocation();
		UE_LOG(LogOutlaws, Display, TEXT("OLNOFLOOR case %d (%s) %s at %s, %s"), CaseI + 1, CaseI == 0 ? TEXT("a hunter") : TEXT("an ordinary bad guy sent at the step"),
			*E->GetOutlawsName(), *Start.ToCompactString(), *E->DebugBossState());
	}

	bool Tick(float Dt)
	{
		UWorld* W = nullptr;
		if (GEngine) for (const FWorldContext& Ctx : GEngine->GetWorldContexts()) if (Ctx.World() && Ctx.World()->IsGameWorld()) W = Ctx.World();
		if (!W) return true;
		AOutlawsLevelActor* L = nullptr;
		for (TActorIterator<AOutlawsLevelActor> It(W); It; ++It) { L = *It; break; }
		APlayerController* PC = W->GetFirstPlayerController();
		APawn* Pawn = PC ? PC->GetPawn() : nullptr;
		if (!L || !Pawn || L->IsFrontEnd() || L->GetEnemies().Num() == 0) return true;
		if (L->IsMoviePlaying()) { L->SkipMovie(); return true; }
		Dt = FMath::Min(Dt, 0.1f);
		T += Dt;
		if (T < 5.0 || !L->AreEnemiesActive()) return true;
		L->bInvincible = true;
		if (CaseI < 0)
		{
			AOutlawsEnemy* M = nullptr;
			for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
			{
				AOutlawsEnemy* E = Wk.Get();
				if (!M && E && !E->bBoss && !E->IsDead() && !E->IsDormant() && !E->bVoiceOnly && !E->bStationary && !E->IsCritter() && !E->bBehindDoor) M = E;
			}
			for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies()) if (Wk.IsValid() && !Wk->IsDead() && !Wk->IsDormant()) Wk->SetDormant(true);
			UE_LOG(LogOutlaws, Display, TEXT("OLNOFLOOR %s, %s: the bad guy %s"), *L->LevelName,
				AOutlawsEnemy::OldNoFloorStuck() ? TEXT("-OLOldNoFloorStuck") : TEXT("a wall before no floor walked into"), M ? *M->GetOutlawsName() : TEXT("none"));
			Man = M;
			bWasHunter = M && M->bHunter;
			CaseI = 0;
			if (!M || !L->LevelName.Equals(TEXT("hideout"), ESearchCase::IgnoreCase)) { Check(false, TEXT("the hideout and an ordinary bad guy to try")); CaseI = 2; }
			else Begin(Pawn);
			return true;
		}
		AOutlawsEnemy* E = Man.Get();
		if (CaseI < 2 && (!E || E->IsDead())) { Check(false, TEXT("the bad guy is gone")); CaseI = 2; }
		if (CaseI >= 2)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT nofloorstuck RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
			FGenericPlatformMisc::RequestExit(false);
			return false;
		}
		// (his own time: the bad guys run at the player's pace, 0.3 of real time for a player standing still: EnemyPace)
		Pace = E->CustomTimeDilation;
		CaseT += Dt * Pace;
		const FVector Now = E->GetActorLocation();
		if (ThroughAt < 0.0 && FVector::Dist2D(Now, StepFeet) < 80.0) ThroughAt = CaseT;
		if (!IsMoving(E) || FVector::Dist2D(Now, Anchor) > 25.0) { Anchor = Now; StillSince = CaseT; }
		Longest = FMath::Max(Longest, CaseT - StillSince);
		if (SeenAt < 0.0)
		{
			// in sight: he sees the player, or nothing solid stands between his eye and theirs
			FCollisionQueryParams Q(SCENE_QUERY_STAT(OLNoFloorSight), true, E);
			Q.AddIgnoredActor(Pawn);
			FCollisionResponseParams RP;
			RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
			FHitResult H;
			const FVector MyEye = Now + FVector(0.0, 0.0, E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() * 0.8);
			if (E->DebugBossState().Contains(TEXT("sees 1")) || !W->LineTraceSingleByChannel(H, MyEye, Pawn->GetPawnViewLocation(), ECC_Visibility, Q, RP)) SeenAt = CaseT;
		}
		if (!bDumped && CaseT >= 1.5)
		{
			bDumped = true;
			UE_LOG(LogOutlaws, Display, TEXT("OLNOFLOOR %d at 1.5 s: %s; %s"), CaseI + 1, *Ahead(W, E), *E->DebugMoveState());
		}
		if (T >= NextLog)
		{
			NextLog = T + 2.0;
			UE_LOG(LogOutlaws, Display, TEXT("OLNOFLOOR %d t %.2f: at %s (%.0f cm from the start), %s; %s"), CaseI + 1, CaseT, *Now.ToCompactString(), FVector::Dist2D(Now, Start),
				*E->DebugBossState(), *E->DebugMoveState());
		}
		const double Sec = CaseI == 0 ? 12.0 : 8.0;
		if (CaseT >= Sec)
		{
			const double Got = ThroughAt < 0.0 ? SeenAt : (SeenAt < 0.0 ? ThroughAt : FMath::Min(ThroughAt, SeenAt));
			const bool bOk = Longest <= 2.0 && (CaseI != 0 || (Got >= 0.0 && Got <= 8.0));
			Check(bOk, FString::Printf(TEXT("(%d) %s, in his own time (pace %.2f): the longest still spell on a move %.1f s, through the opening (80 cm from its step) %s, the player in sight %s, walked off the spot %.0f cm; now %s, the player at %s"), CaseI + 1,
				CaseI == 0 ? TEXT("a hunter") : TEXT("an ordinary bad guy sent at the step"), Pace, Longest,
				ThroughAt < 0.0 ? TEXT("never") : *FString::Printf(TEXT("at %.1f s"), ThroughAt),
				SeenAt < 0.0 ? TEXT("never") : *FString::Printf(TEXT("at %.1f s"), SeenAt), FVector::Dist2D(Now, Start), *Now.ToCompactString(), *Pawn->GetActorLocation().ToCompactString()));
			++CaseI;
			if (CaseI < 2) Begin(Pawn);
			else { E->bHunter = bWasHunter; E->SetDormant(true); }
		}
		return true;
	}

	struct FReg
	{
		FReg()
		{
			if (OLFlags::Param(TEXT("OLNoFloorStuckProbe"))) FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick));
		}
	} GReg;
}

#endif
