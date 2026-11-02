// The DYNTHROW follow-up (user rulings 2 Nov): -OLShots -OLNoFrontEnd -OLLevel=<lv> -OLDynThrowProbe2 [-OLOldDynThrow2], flat.
// shgulch (SH_DICK, THROW_MIN 10 units): (a) the player put 5 units from him in his sight: he goes out past THROW_MIN, as the
// original's advance does within 20 units [EXE 0x14008c343..0x14008c3a0], and throws from there (before: he stood at the
// player's feet and never threw).  cliff (Two Feathers, pknife): (b) his knife that comes down on the level leaves one gknife
// there (pknife's DEAD_ITEM), picked up it is a knife; one that strikes the player leaves none.  -OLOldDynThrow2: the before,
// FAIL expected.  Lines "OLSHOT dynthrow2 check PASS|FAIL: ..." and "OLSHOT dynthrow2 RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsEnemy.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLDynThrowProbe2) { return OLFlags::Param(TEXT("OLDynThrowProbe2")); }

OL_SHOT_TEST(OLDynThrowProbe2, 1620)
{
	static int32 Passes = 0, Fails = 0, ThrowsWas = 0, LeftWas = 0, HitsWas = 0, KnivesWas = 0;
	static float Until = 0.f, OutAt = -1.f, StartT = 0.f;
	static double FarthestCm = 0.0;
	static FVector Spot = FVector::ZeroVector;
	static TWeakObjectPtr<AOutlawsEnemy> Thrower;
	static TWeakObjectPtr<AActor> Drop;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dynthrow2 check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	const bool bOld = AOutlawsEnemy::OldDynThrow2();
	const FString Lv = L->LevelName.ToLower();
	UWorld* World = GetWorld();
	constexpr double UnitCm = 30.0;          // (FOutlawsGeometry::UnitsToCm)
	auto FeetOf = [](const AOutlawsEnemy* E) { return E->GetActorLocation() - FVector(0.0, 0.0, E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()); };
	auto MyFeet = [this]() { return GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight()); };
	AOutlawsEnemy* T = Thrower.Get();
	if (ShotStep > 0 && ShotStep < 20) L->bEnemiesActive = true;      // (ShotTest stills them each tick: (a) is a fight)
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 4.f || !bUp) return;
		const bool bCliff = Lv == TEXT("cliff");
		for (const TWeakObjectPtr<AOutlawsEnemy>& W : L->GetEnemies())
		{
			AOutlawsEnemy* E = W.Get();
			if (!E || E->IsDead() || !E->IsThrower()) continue;
			if (bCliff ? E->Def.ThrowItem == TEXT("pknife") : E->GetOutlawsName().StartsWith(TEXT("SH_DICK"), ESearchCase::IgnoreCase)) { Thrower = E; break; }
		}
		T = Thrower.Get();
		Check(T != nullptr, FString::Printf(TEXT("%s: the thrower is in the level"), *Lv));
		if (!T) { ShotStep = 90; return; }
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dynthrow2 %s: %s throws %s, THROW_MIN %.0f, THROW_MAX %.0f, DEAD_ITEM %s, boss %d (%s)"), *Lv, *T->GetOutlawsName(), *T->Def.ThrowItem,
			T->Def.ThrowMin, T->Def.ThrowMax, T->Def.ThrowDead.IsEmpty() ? TEXT("none") : *T->Def.ThrowDead, T->bBoss ? 1 : 0, bOld ? TEXT("-OLOldDynThrow2: the before") : TEXT("the original's rules"));
		if (T->IsDormant()) T->Appear(FeetOf(T));
		ShotStep = bCliff ? 20 : 10;
		return;
	}
	// ---- (a) shgulch: inside THROW_MIN ----
	case 10:
	{
		if (!T) { ShotStep = 90; return; }
		// a floor 5 units off him, in his sight
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsDynThrowProbe2), false, this);
		Q.AddIgnoredActor(T);
		const FVector Home = FeetOf(T), Eye = T->GetActorLocation() + FVector(0.0, 0.0, 60.0);
		Spot = FVector::ZeroVector;
		for (int32 k = 0; k < 16 && Spot.IsZero(); ++k)
		{
			const double A = 2.0 * PI * k / 16.0;
			const FVector P = Home + FVector(FMath::Cos(A), FMath::Sin(A), 0.0) * (5.0 * UnitCm) + FVector(0.0, 0.0, 150.0);
			FHitResult H;
			if (!World->LineTraceSingleByChannel(H, P, P - FVector(0.0, 0.0, 400.0), ECC_Visibility, Q) || H.ImpactNormal.Z < 0.7 || FMath::Abs(H.ImpactPoint.Z - Home.Z) > 100.0) continue;
			const FVector Floor = H.ImpactPoint;
			FHitResult Sight;
			if (World->LineTraceSingleByChannel(Sight, Eye, Floor + FVector(0.0, 0.0, 120.0), ECC_Visibility, Q)) continue;
			Spot = Floor + FVector(0.0, 0.0, 100.0);
		}
		Check(!Spot.IsZero(), FString::Printf(TEXT("shgulch: a clear spot 5 units (1.5 m) from SH_DICK (%s)"), *Spot.ToCompactString()));
		if (Spot.IsZero()) { ShotStep = 90; return; }
		TeleportTo(Spot, FRotator(0.0, (T->GetActorLocation() - Spot).Rotation().Yaw, 0.0), false, true);
		T->Wake(false);
		ThrowsWas = T->ThrowsTotal;
		FarthestCm = 0.0;
		OutAt = -1.f;
		StartT = ShotTime;
		Until = ShotTime + 6.f * T->Def.ShootTimer + 10.f;      // (his timer is four times as long in some modes)
		ShotStep = 11;
		return;
	}
	case 11:
	{
		if (!T) { ShotStep = 90; return; }
		// the player stays put; how far he gets from them, and whether he throws
		if (FVector::Dist2D(GetActorLocation(), Spot) > 60.0) TeleportTo(Spot, GetActorRotation(), false, true);
		const double D = FVector::Dist2D(FeetOf(T), MyFeet());
		FarthestCm = FMath::Max(FarthestCm, D);
		float Oct = 0.f;
		const bool bIn = T->TargetInThrowRange(&Oct);      // (past THROW_MIN by the throw's own measure)
		if (OutAt < 0.f && bIn) OutAt = ShotTime - StartT;
		const int32 Made = T->ThrowsTotal - ThrowsWas;
		if (ShotTime < Until && !(Made >= 1 && OutAt >= 0.f)) return;
		Check(OutAt >= 0.f, FString::Printf(TEXT("shgulch: SH_DICK goes out past THROW_MIN (%.0f cm) from the player put 1.5 m off: farthest %.0f cm flat, out after %.1f s"),
			T->Def.ThrowMin * UnitCm, FarthestCm, OutAt));
		Check(Made >= 1, FString::Printf(TEXT("shgulch: and throws from there: %d throws (the last %.0f cm off) in %.1f s; %s"), Made, T->LastThrowDist, ShotTime - StartT, *T->DebugBossState()));
		ShotStep = 90;
		return;
	}
	// ---- (b) cliff: a knife come down on the level ----
	case 20:
	{
		if (!T) { ShotStep = 90; return; }
		Check(bOld || T->Def.ThrowDead == TEXT("gknife"), FString::Printf(TEXT("cliff: pknife's DEAD_ITEM read: %s"), T->Def.ThrowDead.IsEmpty() ? TEXT("none") : *T->Def.ThrowDead));
		LeftWas = L->EnemyKnivesLeft;
		KnivesWas = L->GetCount(TEXT("iknife"));
		// one dropped straight down onto the floor 3 m beside the player (in no one's way)
		FVector Side = FVector::CrossProduct(GetActorForwardVector(), FVector::UpVector).GetSafeNormal2D();
		const FVector From = MyFeet() + Side * 300.0 + FVector(0.0, 0.0, 150.0);
		L->AddEnemyThrown(T, 2, From, FVector(0.0, 0.0, -600.0), 3.f, T->Def.Damage);
		Until = ShotTime + 2.f;
		ShotStep = 21;
		return;
	}
	case 21:
	{
		if (!T) { ShotStep = 90; return; }
		if (L->EnemyKnivesLeft == LeftWas && L->EnemyThrownCount() > 0 && ShotTime < Until) return;
		Drop = L->LastEnemyKnifeDrop;
		const int32 Left = L->EnemyKnivesLeft - LeftWas;
		Check(Left == 1 && Drop.IsValid(), FString::Printf(TEXT("cliff: the knife that came down on the level left %d gknife (%s)"), Left, Drop.IsValid() ? *L->DescribePickup(Drop.Get()) : TEXT("none")));
		if (Drop.IsValid())
		{
			const bool bGot = L->CollectPickupActor(Drop.Get(), true);
			Check(bGot && L->GetCount(TEXT("iknife")) == KnivesWas + 1, FString::Printf(TEXT("cliff: picked up, a knife: taken %d, knives %d -> %d"), bGot ? 1 : 0, KnivesWas, L->GetCount(TEXT("iknife"))));
		}
		// one into the player's middle from 80 cm in front: it strikes, and leaves none
		LeftWas = L->EnemyKnivesLeft;
		HitsWas = L->EnemyKnifeHits;
		const FVector Fwd = GetActorForwardVector().GetSafeNormal2D();
		L->AddEnemyThrown(T, 2, GetActorLocation() + Fwd * 80.0, -Fwd * 1500.0, 3.f, T->Def.Damage);
		Until = ShotTime + 1.5f;
		ShotStep = 22;
		return;
	}
	case 22:
	{
		if (L->EnemyKnifeHits == HitsWas && L->EnemyThrownCount() > 0 && ShotTime < Until) return;
		Check(L->EnemyKnifeHits == HitsWas + 1 && L->EnemyKnivesLeft == LeftWas, FString::Printf(TEXT("cliff: a knife that strikes the player leaves none: %d strikes, %d left"),
			L->EnemyKnifeHits - HitsWas, L->EnemyKnivesLeft - LeftWas));
		ShotStep = 90;
		return;
	}
	case 90:
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dynthrow2 RESULT %s (%d passed, %d failed)%s"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			bOld ? TEXT(" (-OLOldDynThrow2: the before, FAIL expected)") : TEXT(""));
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
