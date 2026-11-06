// BUG-ENEMYCORNERS' probe (user 5 Nov, snapshots 13 and 14): -OLLevel=hideout -OLNoFrontEnd -OLSoloBadGuys=4 -OLDeviceProfile=Quest3
// -OLEnemyCornersProbe [-OLCornersPack=70] [-OLCornersAt=X=..,Y=..,Z=..] [-OLOldEnemyCorners].  A pack put in the room of snapshot
// 14's crowd, every other bad guy dormant, woken with the player in another room (snapshot 13's place), so all of them walk at
// a target past the walls; after 45 s of their time the player comes back to snapshot 14's place.  Every 5 s of their own time
// (EnemyPace) a line: how many stand bunched (two or more others within 1.8 m), the biggest bunch, how many moved under 1 m, the
// moves, the throttle's tiers, and the counters.  Checks at the end of the 45 s (the last three lines' mean): no slide off a body,
// most looks at the bunch find somewhere to go, biggest bunch and men standing below the old code's.  Lines "OLSHOT enemycorners
// check PASS|FAIL" and "OLSHOT enemycorners RESULT".
#include "OutlawsEnemy.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsFlags.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsEnemyCornersProbeLocal
{
	const FVector PlaceA(13446.0, 12145.0, 485.0);       // snapshot 14's place: another room
	const FVector PlaceB(11903.0, 10880.0, 485.0);       // snapshot 13's: he comes back
	FVector PackAt(12760.0, 12161.0, 482.0);             // the room of snapshot 14's crowd

	double T = 0.0, Own = 0.0, NextLog = 0.0;
	int32 Phase = -1;
	double PhaseEnd[2] = { 45.0, 65.0 };
	TArray<TWeakObjectPtr<AOutlawsEnemy>> Pack;
	TMap<AOutlawsEnemy*, FVector> Was;
	int32 Passes = 0, Fails = 0;
	TArray<FVector2D> Tail;         // (biggest, standing) of phase 0's samples

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT enemycorners check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}

	void Judge()
	{
		const int32* C = AOutlawsEnemy::GCornerStat;
		FVector2D Mean(0.0, 0.0);
		const int32 N = FMath::Min(3, Tail.Num());
		for (int32 i = Tail.Num() - N; i < Tail.Num(); ++i) Mean += Tail[i] / double(FMath::Max(N, 1));
		Check(C[6] == 0, FString::Printf(TEXT("%d slides off a body"), C[6]));
		Check(C[0] >= 50 && C[1] * 2 < C[0], FString::Printf(TEXT("%d looks at the bunch, %d found nowhere to go"), C[0], C[1]));
		Check(Mean.X < 18.0, FString::Printf(TEXT("biggest bunch %.1f men round one (old 20 to 22)"), Mean.X));
		Check(Mean.Y < 45.0, FString::Printf(TEXT("%.1f of them moved under 1 m in 5 s (old 55)"), Mean.Y));
	}

	void Sample(AOutlawsLevelActor* L, APawn* Pawn)
	{
		TArray<AOutlawsEnemy*> Men;
		int32 Still = 0;
		FVector Centre = FVector::ZeroVector;
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : Pack)
		{
			AOutlawsEnemy* E = Wk.Get();
			if (!E || E->IsDead()) continue;
			Men.Add(E);
			Centre += E->GetActorLocation();
			if (const FVector* V = Was.Find(E)) { if (FVector::Dist2D(*V, E->GetActorLocation()) < 100.0) ++Still; }
			Was.Add(E, E->GetActorLocation());
		}
		if (Men.Num() > 0) Centre /= Men.Num();
		int32 BunchTier[4] = {}, Bunched = 0, Biggest = 0, Seen = 0, Subs[12] = {}, Tiers[4] = {};
		AOutlawsEnemy* Worst = nullptr;
		for (AOutlawsEnemy* E : Men)
		{
			int32 N = 0;
			const FVector At = E->GetActorLocation();
			for (AOutlawsEnemy* O : Men)
			{
				if (O != E && FVector::DistSquared2D(O->GetActorLocation(), At) < 180.0 * 180.0 && FMath::Abs(O->GetActorLocation().Z - At.Z) < 200.0) ++N;
			}
			if (N >= 2) { ++Bunched; ++BunchTier[E->GetThrottleTier() & 3]; }
			if (N > Biggest) { Biggest = N; Worst = E; }
			if (E->DebugSees()) ++Seen;
			++Subs[FMath::Clamp(E->DebugSub(), 0, 11)];
			++Tiers[E->GetThrottleTier() & 3];
		}
		if (Phase == 0) Tail.Add(FVector2D(Biggest, Still));
		int32* C = AOutlawsEnemy::GCornerStat;
		UE_LOG(LogOutlaws, Display, TEXT("OLCORNERS t %.0f phase %d: men %d bunched %d biggest %d sees %d still %d centre %s | face %d adv %d retreat %d cover %d shoot %d | tiers %d %d %d %d bunched %d %d %d %d | crowded looks %d none %d off %d stuck %d slid %d reached %d | worst at %s sub %d target %s"),
			Own, Phase, Men.Num(), Bunched, Biggest, Seen, Still, *Centre.ToCompactString(), Subs[1], Subs[3], Subs[4], Subs[8], Subs[5] + Subs[6], Tiers[0], Tiers[1], Tiers[2], Tiers[3], BunchTier[0], BunchTier[1], BunchTier[2], BunchTier[3],
			C[0], C[1], C[2], C[3], C[4], C[5], Worst ? *Worst->GetActorLocation().ToCompactString() : TEXT("-"), Worst ? Worst->DebugSub() : -1,
			Worst ? *Worst->DebugTargetPos().ToCompactString() : TEXT("-"));
	}

	bool Tick(float Dt)
	{
		for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
		{
			UWorld* W = Ctx.World();
			if (!W || !W->IsGameWorld()) continue;
			AOutlawsLevelActor* L = nullptr;
			for (TActorIterator<AOutlawsLevelActor> It(W); It; ++It) { L = *It; break; }
			APlayerController* PC = W->GetFirstPlayerController();
			APawn* Pawn = PC ? PC->GetPawn() : nullptr;
			if (!L || !Pawn || L->IsFrontEnd() || L->GetEnemies().Num() == 0) return true;
			if (L->IsMoviePlaying()) { L->SkipMovie(); return true; }
			T += Dt;
			if (T < 5.0 || !L->AreEnemiesActive()) return true;
			L->bInvincible = true;
			if (Phase < 0)
			{
				Phase = 0;
				Pawn->TeleportTo(PlaceB, FRotator::ZeroRotator, false, true);
				FString At;
				int32 PackSize = 70;
				OLFlags::Value(TEXT("OLCornersPack"), PackSize);
				if (OLFlags::Value(TEXT("OLCornersAt"), At)) PackAt.InitFromString(At);
				TArray<FVector> Spots;
				for (int32 X = -6; X <= 6; ++X) for (int32 Y = -6; Y <= 6; ++Y)
				{
					const FVector S = PackAt + FVector(X * 75.0, Y * 75.0, 0.0);
					FHitResult H;
					FCollisionQueryParams Q(SCENE_QUERY_STAT(OLCorners), false);
					FCollisionResponseParams RP;
					RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
					if (W->LineTraceSingleByChannel(H, PackAt, S + (S - PackAt).GetSafeNormal() * 45.0, ECC_Visibility, Q, RP)) continue;
					if (!W->LineTraceSingleByChannel(H, S, S - FVector(0.0, 0.0, 160.0), ECC_Visibility, Q, RP)) continue;
					Spots.Add(S);
				}
				Spots.Sort([](const FVector& A, const FVector& B) { return FVector::DistSquared2D(A, PackAt) < FVector::DistSquared2D(B, PackAt); });
				for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
				{
					AOutlawsEnemy* E = Wk.Get();
					if (!E || E->IsDead() || E->IsDormant()) continue;
					if (Pack.Num() < PackSize && Pack.Num() < Spots.Num() && !E->bBoss && !E->bHunter && !E->bVoiceOnly && !E->bStationary && !E->IsCritter() && !E->bBehindDoor && !E->IsThrower())
					{
						E->SetActorLocation(Spots[Pack.Num()], false, nullptr, ETeleportType::TeleportPhysics);
						Pack.Add(E);
					}
					else E->SetDormant(true);
				}
				for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : Pack) if (Wk.IsValid()) Wk->Wake(false);
				UE_LOG(LogOutlaws, Display, TEXT("OLCORNERS %s: %d bad guys, a pack of %d, %s"), *L->LevelName, L->GetEnemies().Num(), Pack.Num(), AOutlawsEnemy::OldBossWall() ? TEXT("-OLOldBossWall") : TEXT("new"));
				return true;
			}
			double Pace = 0.3;
			for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : Pack) if (Wk.IsValid() && !Wk->IsDead()) { Pace = Wk->CustomTimeDilation; if (Pace > 0.01) break; }
			Own += FMath::Min(double(Dt), 0.5) * Pace;
			if (Own >= NextLog) { NextLog += 5.0; Sample(L, Pawn); }
			if (Own >= PhaseEnd[Phase])
			{
				++Phase;
				if (Phase == 1) Judge();
				if (Phase >= 2)
				{
					UE_LOG(LogOutlaws, Display, TEXT("OLSHOT enemycorners RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
					FGenericPlatformMisc::RequestExit(false);
					return false;
				}
				Pawn->TeleportTo(PlaceA, FRotator(0.0, 180.0, 0.0), false, true);
			}
			return true;
		}
		return true;
	}

	struct FReg
	{
		FReg()
		{
			if (OLFlags::Param(TEXT("OLEnemyCornersProbe"))) FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick));
		}
	} GReg;
}

#endif
