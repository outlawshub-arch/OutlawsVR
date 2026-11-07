// BUG-DYNTHROW (user 2 Nov, the user's snapshot #41, shgulch: Dick Farmer never threw): -OLShots -OLNoFrontEnd -OLLevel=<lv>
// -OLDynThrowProbe [-OLOldDynThrow], flat.  shgulch (SH_DICK): his ITM's throw read; (a) the player under THROW_MIN (2 m): no
// throw; (b) behind a wall: no lob reaches, no throw; (c) 9 m off in sight: a throw within his shoot timer, the stick photographed
// in flight and lying lit, going off LIFE (4.5 s) after it left the hand by its mark, the player hurt by the blast's falloff;
// (d) the rate: no two throws closer than SHOOT_TIMER_TIME (the difficulty's) allows; (e) a blast flagged a bad guy's kills a bad
// guy that is no one's (not the player's dynamite kill), the same blast unflagged is the player's.  hideout (BGY2BOOM): a throw at
// 20 m (inside THROW_MAX 150 units), none at 50 m.  cliff (Two Feathers): knives thrown and one strikes.  -OLOldDynThrow: no
// throw, as before.  Lines "OLSHOT dynthrow check PASS|FAIL: ..." and "OLSHOT dynthrow RESULT"; shots
// dynthrow_<level>_<flight|lit>[_old].png in Saved\OLShots.
// (f) BUG-DICKSTICK (shgulch): his own sticks in (c) leave his health as it was, each one in his reach and sight sparing him;
// in (e) a bad guy's stick at a bad guy's own feet, his own, spares him, and the next there, SH_DICK's, kills him.
// -OLOldOwnStick: his own sticks hurt him, as before ((f) FAILs).

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsEnemy.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsStats.h"
#include "OutlawsShotTests.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLDynThrowProbe) { return OLFlags::Param(TEXT("OLDynThrowProbe")); }

OL_SHOT_TEST(OLDynThrowProbe, 1510)
{
	static int32 Passes = 0, Fails = 0, ThrowsWas = 0, BlastsWas = 0, KnifeHitsWas = 0, SparedWas = 0;
	static float Until = 0.f, NextPlace = 0.f, ThrowerHealthWas = 0.f;
	static double StartAt = 0.0;
	static float HealthPrev = 0.f;
	static bool bShotFlight = false, bShotLit = false, bBlastChecked = false;
	static FVector FeetAtThrow = FVector::ZeroVector, ThrowerFrom = FVector::ZeroVector;
	static TArray<double> ThrowTimes;
	static TWeakObjectPtr<AOutlawsEnemy> Thrower;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dynthrow check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	const bool bOld = AOutlawsEnemy::OldDynThrow();
	const FString Lv = L->LevelName.ToLower();
	UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	L->bEnemiesActive = true;              // (ShotTest stills them each tick: a fight here)
	if (ShotStep > 0) L->bInvincible = false;      // (... and makes the player invincible each tick: the blasts' harm is measured here)
	auto Shot = [&](const TCHAR* What)
	{
		const FString Name = FString::Printf(TEXT("dynthrow_%s_%s%s"), *Lv, What, bOld ? TEXT("_old") : TEXT(""));
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dynthrow shot %s.png"), *Name);
	};
	auto FeetOf = [](const AOutlawsEnemy* E) { return E->GetActorLocation() - FVector(0.0, 0.0, E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()); };
	// a place for the player Radius cm (flat) from the thrower, on a floor within 3 m of his, with his eye's line to it clear
	// (bClear) or blocked by the level; the player's middle there (the floor + 100 cm), or zero
	auto FindSpot = [&](const AOutlawsEnemy* E, const TArray<double>& Radii, bool bClear, bool bNeedSameFloor) -> FVector
	{
		const FVector Home = FeetOf(E), Eye = E->GetActorLocation() + FVector(0.0, 0.0, E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() * 0.8);
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsDynThrowProbe), false, this);
		Q.AddIgnoredActor(E);
		for (const double R : Radii)
		{
			for (int32 k = 0; k < 32; ++k)
			{
				const double A = 2.0 * PI * k / 32.0;
				const FVector P = Home + FVector(FMath::Cos(A) * R, FMath::Sin(A) * R, 150.0);
				FHitResult H;
				if (!World->LineTraceSingleByChannel(H, P, P - FVector(0.0, 0.0, 450.0), ECC_Visibility, Q)) continue;
				if (H.ImpactNormal.Z < 0.7) continue;
				const FVector Floor = H.ImpactPoint;
				if (bNeedSameFloor && FMath::Abs(Floor.Z - Home.Z) > 300.0) continue;
				const FVector Mid = Floor + FVector(0.0, 0.0, 100.0);
				FHitResult Up;
				if (World->LineTraceSingleByChannel(Up, Floor + FVector(0.0, 0.0, 5.0), Floor + FVector(0.0, 0.0, 200.0), ECC_Visibility, Q)) continue;   // (room to stand)
				const bool bBlocked = World->LineTraceSingleByChannel(H, Eye, Mid + FVector(0.0, 0.0, 50.0), ECC_Visibility, Q)
					|| World->LineTraceSingleByChannel(H, Eye, Floor + FVector(0.0, 0.0, 20.0), ECC_Visibility, Q);
				if (bClear == !bBlocked) return Mid;
			}
		}
		return FVector::ZeroVector;
	};
	auto PutPlayer = [&](const AOutlawsEnemy* E, const FVector& Mid)
	{
		const FRotator Look = (E->GetActorLocation() - Mid).Rotation();
		TeleportTo(Mid, FRotator(0.0, Look.Yaw, 0.0), false, true);
		if (Controller) Controller->SetControlRotation(FRotator(0.0, Look.Yaw, 0.0));
	};
	auto Solves = [&](const AOutlawsEnemy* E, const FVector& Mid)
	{
		FVector Hand, Vel, Mark;
		double Tm = 0.0;
		return E->SolveThrow(Mid - FVector(0.0, 0.0, 100.0), 175.f, 10, false, Hand, Vel, Tm, Mark);
	};
	AOutlawsEnemy* T = Thrower.Get();
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 4.f || !bUp) return;
		L->bInvincible = false;
		L->TestSetHealth(1000.f);          // (the blasts and knives taken on a deep pool: each one's harm read off it)
		L->SetArmour(0);
		const TCHAR* Want = Lv == TEXT("shgulch") ? TEXT("SH_DICK") : Lv == TEXT("cliff") ? TEXT("Two Feathers (pknife)") : TEXT("BGY2BOOM");
		for (const TWeakObjectPtr<AOutlawsEnemy>& W : L->GetEnemies())
		{
			AOutlawsEnemy* E = W.Get();
			if (!E || E->IsDead() || !E->IsThrower()) continue;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dynthrow %s: thrower %s at %s: %s, THROW_MIN %.0f, THROW_MAX %.0f, THROW_VELOCITY %.0f, fuse %.2f s, shoot timer %.2f s, shoot chor %d, damage %.2f, boss %d, dormant %d"),
				*Lv, *E->GetOutlawsName(), *E->GetActorLocation().ToCompactString(), *E->Def.ThrowItem, E->Def.ThrowMin, E->Def.ThrowMax, E->Def.ThrowVelocity, E->Def.ThrowLife, E->Def.ShootTimer,
				E->Def.ShootChor, E->Def.Damage, E->bBoss ? 1 : 0, E->IsDormant() ? 1 : 0);
			if (!Thrower.IsValid() && (Lv == TEXT("cliff") ? E->Def.ThrowItem == TEXT("pknife") : E->GetOutlawsName().StartsWith(Want, ESearchCase::IgnoreCase))) Thrower = E;
		}
		T = Thrower.Get();
		Check(T != nullptr, FString::Printf(TEXT("%s: a %s thrower is in the level"), *Lv, Want));
		if (!T) { ShotStep = 90; return; }
		if (T->IsDormant()) T->Appear(FeetOf(T));
		if (Lv == TEXT("shgulch"))
		{
			Check(T->Def.ThrowItem == TEXT("pdynam") && FMath::IsNearlyEqual(T->Def.ThrowVelocity, 70.f) && FMath::IsNearlyEqual(T->Def.ThrowMin, 10.f) && FMath::IsNearlyEqual(T->Def.ThrowMax, 350.f)
				&& FMath::IsNearlyEqual(T->Def.ThrowLife, 4.5f, 0.01f), FString::Printf(TEXT("SH_DICK's throw read: %s, velocity %.0f, %.0f..%.0f units, fuse %.2f s (pdynam LIFE)"),
				*T->Def.ThrowItem, T->Def.ThrowVelocity, T->Def.ThrowMin, T->Def.ThrowMax, T->Def.ThrowLife));
			ShotStep = bOld ? 80 : 10;
		}
		else if (Lv == TEXT("cliff"))
		{
			Check(T->Def.ThrowItem == TEXT("pknife") && T->Def.ThrowVelocity >= 100.f, FString::Printf(TEXT("Two Feathers' throw read: %s, velocity %.0f"), *T->Def.ThrowItem, T->Def.ThrowVelocity));
			ShotStep = bOld ? 80 : 75;
		}
		else
		{
			Check(T->Def.ThrowItem == TEXT("pdynam") && FMath::IsNearlyEqual(T->Def.ThrowVelocity, 100.f) && FMath::IsNearlyEqual(T->Def.ThrowMin, 30.f) && FMath::IsNearlyEqual(T->Def.ThrowMax, 150.f),
				FString::Printf(TEXT("BGY2BOOM's throw read: %s, velocity %.0f, %.0f..%.0f units"), *T->Def.ThrowItem, T->Def.ThrowVelocity, T->Def.ThrowMin, T->Def.ThrowMax));
			ShotStep = bOld ? 80 : 60;
		}
		return;
	}
	// ---- (a) under THROW_MIN ----
	case 10:
	{
		if (!T) { ShotStep = 90; return; }
		const FVector Mid = FindSpot(T, { 200.0, 220.0, 180.0 }, true, true);
		Check(!Mid.IsZero(), FString::Printf(TEXT("shgulch: a clear spot 2 m from SH_DICK (%s)"), *Mid.ToCompactString()));
		if (Mid.IsZero()) { ShotStep = 20; return; }
		PutPlayer(T, Mid);
		T->Wake(false);
		ThrowsWas = T->ThrowsTotal;
		Until = ShotTime + T->Def.ShootTimer + 2.f;
		NextPlace = ShotTime + 0.5f;
		ShotStep = 11;
		return;
	}
	case 11:
	{
		if (!T) { ShotStep = 90; return; }
		if (ShotTime >= NextPlace)
		{
			// kept under his THROW_MIN as he moves
			NextPlace = ShotTime + 0.5f;
			if (FVector::Dist2D(GetActorLocation(), T->GetActorLocation()) > 260.0)
			{
				const FVector Mid = FindSpot(T, { 200.0, 180.0, 220.0 }, true, true);
				if (!Mid.IsZero()) PutPlayer(T, Mid);
			}
		}
		if (ShotTime < Until) return;
		float D = 0.f;
		const bool bIn = T->TargetInThrowRange(&D);
		const int32 Made = T->ThrowsTotal - ThrowsWas;
		Check(Made == 0 || T->LastThrowDist > T->Def.ThrowMin * 30.f, FString::Printf(TEXT("shgulch: under THROW_MIN (%.0f cm) no throw: %d made in %.1f s (the last at %.0f cm; now %.0f cm, in range %d)"),
			T->Def.ThrowMin * 30.f, Made, T->Def.ShootTimer + 2.f, T->LastThrowDist, D, bIn ? 1 : 0));
		ShotStep = 20;
		return;
	}
	// ---- (b) a wall between ----
	case 20:
	{
		if (!T) { ShotStep = 90; return; }
		FVector Mid = FVector::ZeroVector;
		for (const double R : { 700.0, 900.0, 600.0, 1100.0, 500.0, 1300.0 })
		{
			const FVector P = FindSpot(T, { R }, false, true);
			if (!P.IsZero() && !Solves(T, P)) { Mid = P; break; }
		}
		Check(!Mid.IsZero(), FString::Printf(TEXT("shgulch: a spot behind a wall from SH_DICK where no lob reaches (%s, %.0f cm off)"), *Mid.ToCompactString(), Mid.IsZero() ? 0.0 : FVector::Dist2D(Mid, T->GetActorLocation())));
		if (Mid.IsZero()) { ShotStep = 30; return; }
		PutPlayer(T, Mid);
		T->Wake(false);
		ThrowsWas = T->ThrowsTotal;
		ThrowerFrom = T->GetActorLocation();
		Until = ShotTime + T->Def.ShootTimer + 2.f;
		ShotStep = 21;
		return;
	}
	case 21:
	{
		if (!T) { ShotStep = 90; return; }
		if (ShotTime < Until) return;
		const int32 Made = T->ThrowsTotal - ThrowsWas;
		const double Moved = FVector::Dist2D(T->GetActorLocation(), ThrowerFrom);
		Check(Made == 0 || Moved > 150.0, FString::Printf(TEXT("shgulch: a wall between, no throw from where he stood: %d made in %.1f s (he moved %.0f cm)"), Made, T->Def.ShootTimer + 2.f, Moved));
		ShotStep = 30;
		return;
	}
	// ---- (c), (d): 9 m off in sight ----
	case 30:
	case 60:
	case 75:
	{
		if (!T) { ShotStep = 90; return; }
		const bool bDick = ShotStep == 30, bBoom = ShotStep == 60;
		const TArray<double> Radii = bDick ? TArray<double>{ 900.0, 850.0, 950.0, 800.0, 1000.0 }
			: bBoom ? TArray<double>{ 2000.0, 1800.0, 2200.0, 1600.0, 2500.0, 1400.0, 3000.0, 1200.0 } : TArray<double>{ 1000.0, 900.0, 1100.0, 800.0, 1300.0, 700.0 };
		FVector Mid = FVector::ZeroVector;
		for (const double R : Radii)
		{
			const FVector P = FindSpot(T, { R }, true, true);
			if (!P.IsZero() && Solves(T, P)) { Mid = P; break; }
		}
		const double D = Mid.IsZero() ? 0.0 : FVector::Dist2D(Mid, FeetOf(T));
		Check(!Mid.IsZero(), FString::Printf(TEXT("%s: a clear spot %.1f m from %s where a lob reaches (%s)"), *Lv, D / 100.0, *T->GetOutlawsName(), *Mid.ToCompactString()));
		if (Mid.IsZero()) { ShotStep = bDick ? 50 : 90; return; }
		PutPlayer(T, Mid);
		T->Wake(false);
		ThrowsWas = T->ThrowsTotal;
		BlastsWas = L->EnemyBlasts;
		KnifeHitsWas = L->EnemyKnifeHits;
		SparedWas = L->EnemyBlastsSpared;
		ThrowerHealthWas = T->GetHealth();
		HealthPrev = L->GetHealth();
		FeetAtThrow = Mid - FVector(0.0, 0.0, 100.0);
		StartAt = Now;
		ThrowTimes.Reset();
		bShotFlight = bShotLit = bBlastChecked = false;
		// Dick: three throws (the rate) in three of his timers; the thin yellow one: the first; Two Feathers: a knife home
		Until = ShotTime + (bDick ? 5.f * T->Def.ShootTimer + 6.f : bBoom ? T->Def.ShootTimer + 4.f : 4.f * T->Def.ShootTimer + 6.f);
		NextPlace = ShotTime + 1.f;
		ShotStep = ShotStep + 1;
		return;
	}
	case 31:
	case 61:
	case 76:
	{
		if (!T) { ShotStep = 90; return; }
		const bool bDick = ShotStep == 31, bBoom = ShotStep == 61;
		if (T->ThrowsTotal - ThrowsWas > ThrowTimes.Num()) ThrowTimes.Add(T->LastThrowAt);
		FVector Pos;
		float Age = 0.f;
		int32 Kind = 0;
		bool bRest = false, bDrawn = false;
		if (L->DebugEnemyThrown(Pos, Age, Kind, bRest, bDrawn))
		{
			// (a stick tumbles end over end toward the one it was thrown at: a little after a quarter turn it is side on to him)
			if (!bShotFlight && !bRest && Age >= (Kind == 1 ? 0.15f : 0.08f))
			{
				bShotFlight = true;
				Shot(TEXT("flight"));
				const FVector CamAt = Controller && Cast<APlayerController>(Controller) && Cast<APlayerController>(Controller)->PlayerCameraManager ? Cast<APlayerController>(Controller)->PlayerCameraManager->GetCameraLocation() : GetActorLocation();
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dynthrow %s in flight at %s, %.2f s out, %.0f cm from the eye (camera at %s), drawn %d: %s"), Kind == 1 ? TEXT("stick") : TEXT("knife"), *Pos.ToCompactString(), Age,
					FVector::Dist(Pos, GetActorLocation()), *CamAt.ToCompactString(), bDrawn ? 1 : 0, *L->DebugEnemyThrownLook());
				Check(bDrawn, FString::Printf(TEXT("%s: the %s is drawn as the player's own"), *Lv, Kind == 1 ? TEXT("stick") : TEXT("knife")));
			}
			if (!bShotLit && bRest && Kind == 1 && Age >= 1.2f)
			{
				// looked down at it lying lit
				bShotLit = true;
				if (Controller) Controller->SetControlRotation((Pos - (GetActorLocation() + FVector(0.0, 0.0, 70.0))).Rotation());
				Shot(TEXT("lit"));
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dynthrow lit look: %s"), *L->DebugEnemyThrownLook());
			}
		}
		// the first blast: its fuse from the hand, by its mark, the player's hurt by its falloff
		if (!bBlastChecked && L->EnemyBlasts > BlastsWas)
		{
			bBlastChecked = true;
			const float Dropped = HealthPrev - L->GetHealth();
			const FVector From = L->LastEnemyBlastAt + FVector(0.0, 0.0, 20.0);
			const double R = L->BlastRadiusCm(), Dd = FVector::Dist(From, GetActorLocation());
			const double Point = OLFlags::Param(TEXT("OLOldBlastPlayer")) ? 12.0 : 6.0;     // (BUG-BLASTMISS: half on a player in the story)
			const double Want = Dd >= R ? 0.0 : Point * (Dd <= R * 0.25 ? 1.0 : FMath::Square((R - Dd) / (0.75 * R)));
			Check(FMath::IsNearlyEqual(L->LastEnemyFuse, T->Def.ThrowLife, 0.1f), FString::Printf(TEXT("%s: the stick goes off %.2f s after it left the hand (fuse %.2f s)"), *Lv, L->LastEnemyFuse, T->Def.ThrowLife));
			Check(FVector::Dist2D(L->LastEnemyBlastAt, FeetAtThrow) < 250.0, FString::Printf(TEXT("%s: it went off by the player's feet: %.0f cm from them, %.0f cm from its mark"), *Lv,
				FVector::Dist2D(L->LastEnemyBlastAt, FeetAtThrow), FVector::Dist2D(L->LastEnemyBlastAt, T->LastThrowMark)));
			Check(Dropped > 0.f && FMath::Abs(Dropped - float(Want)) < 0.75f, FString::Printf(TEXT("%s: the player hurt by the blast's falloff: %.2f (%.0f cm from it: %.2f by the falloff)"), *Lv, Dropped, Dd, Want));
			if (Controller) Controller->SetControlRotation(FRotator(0.0, (T->GetActorLocation() - GetActorLocation()).Rotation().Yaw, 0.0));
		}
		HealthPrev = L->GetHealth();
		// the player kept in his reach for the rate (he presses on): once the first stick went off, or (Two Feathers) always, put
		// back 9 m from him when he came within 5 m; never with a stick in the air
		const bool bCanPlace = bDick ? bBlastChecked : !bBoom;
		if (bCanPlace && ShotTime >= NextPlace && L->EnemyThrownCount() == 0)
		{
			NextPlace = ShotTime + 1.f;
			const double Dd = FVector::Dist2D(GetActorLocation(), T->GetActorLocation());
			if (Dd < 500.0 || Dd > 1300.0)
			{
				const FVector Mid = FindSpot(T, { 900.0, 800.0, 1000.0, 700.0, 1100.0 }, true, true);
				if (!Mid.IsZero()) PutPlayer(T, Mid);
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dynthrow %s came within %.1f m: the player put back %s (%s)"), *T->GetOutlawsName(), Dd / 100.0,
					Mid.IsZero() ? TEXT("NOWHERE (no clear spot)") : *FString::Printf(TEXT("%.1f m off"), FVector::Dist2D(Mid, T->GetActorLocation()) / 100.0), *T->DebugBossState());
			}
		}
		const int32 Made = T->ThrowsTotal - ThrowsWas;
		const bool bDone = bDick ? Made >= 3 && (bBlastChecked || ShotTime >= Until)
			: bBoom ? Made >= 1 && bBlastChecked : L->EnemyKnifeHits > KnifeHitsWas && Made >= 2;
		if (!bDone && ShotTime < Until) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dynthrow %s at the end: %s; sight: %s"), *T->GetOutlawsName(), *T->DebugBossState(), *T->DebugSight());
		// (the first within two of his timers: at the first the player may be out of his sight a moment, by the light)
		Check(Made >= 1 && ThrowTimes.Num() > 0 && ThrowTimes[0] - StartAt <= 2.0 * T->Def.ShootTimer + 4.0, FString::Printf(TEXT("%s: %s throws at the player %.1f m off (%d throws; the first %.1f s after he was put there; shoot timer %.1f s)"),
			*Lv, *T->GetOutlawsName(), FVector::Dist2D(FeetAtThrow, FeetOf(T)) / 100.0, Made, ThrowTimes.Num() > 0 ? ThrowTimes[0] - StartAt : -1.0, T->Def.ShootTimer));
		if (bDick || bBoom) Check(bBlastChecked, FString::Printf(TEXT("%s: the stick went off (%d blasts)"), *Lv, L->EnemyBlasts - BlastsWas));
		if (bDick || bBoom)
		{
			// (f) BUG-DICKSTICK: his own sticks spare him; the player 9 m off (Dick) puts each blast well inside his reach.  BGY2BOOM's
			// land 20 m off, past it (told, not checked)
			const int32 Spared = L->EnemyBlastsSpared - SparedWas;
			const FString Own = FString::Printf(TEXT("%s: his own sticks spare %s: health %.2f before, %.2f after (dead %d) over %d blasts, %d of them in his reach and sight, the last %.1f m from him%s"),
				*Lv, *T->GetOutlawsName(), ThrowerHealthWas, T->GetHealth(), T->IsDead() ? 1 : 0, L->EnemyBlasts - BlastsWas, Spared,
				FVector::Dist(L->LastEnemyBlastAt, T->GetActorLocation()) / 100.0, AOutlawsEnemy::OldOwnStick() ? TEXT(" (-OLOldOwnStick)") : TEXT(""));
			if (bDick) Check(!T->IsDead() && FMath::IsNearlyEqual(T->GetHealth(), ThrowerHealthWas) && Spared >= 1, Own);
			else UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dynthrow %s"), *Own);
		}
		if (bDick)
		{
			// [EXE 0x14008b9c6]: the timer's next is SHOOT_TIMER_TIME +- 1.024 s from the last; a throw a timer at most
			FString Gaps;
			double Least = 1e9;
			for (int32 k = 1; k < ThrowTimes.Num(); ++k) { const double Gp = ThrowTimes[k] - ThrowTimes[k - 1]; Least = FMath::Min(Least, Gp); Gaps += FString::Printf(TEXT(" %.1f"), Gp); }
			Check(ThrowTimes.Num() >= 2 && Least >= T->Def.ShootTimer - 1.2, FString::Printf(TEXT("shgulch: the throws come no faster than his shoot timer %.1f s (gaps:%s)"), T->Def.ShootTimer, Gaps.IsEmpty() ? TEXT(" none") : *Gaps));
		}
		if (!bDick && !bBoom)
		{
			Check(L->EnemyKnifeHits > KnifeHitsWas, FString::Printf(TEXT("cliff: a knife strikes the player (%d of %d; %d spent on the level)"), L->EnemyKnifeHits - KnifeHitsWas, Made, L->EnemyKnivesSpent));
		}
		ShotStep = bDick ? 50 : bBoom ? 70 : 90;
		return;
	}
	// ---- (e) a bad guy's blast kills no one's ----
	case 50:
	{
		TArray<AOutlawsEnemy*> Others;
		for (const TWeakObjectPtr<AOutlawsEnemy>& W : L->GetEnemies())
		{
			AOutlawsEnemy* E = W.Get();
			if (E && E != T && !E->IsDead() && !E->IsDormant() && !E->bBoss && !E->bVoiceOnly && !E->bNotBadGuy && E->CoopSelf < 0
				&& FVector::Dist(E->GetActorLocation(), GetActorLocation()) > L->BlastRadiusCm() * 1.5f) Others.Add(E);
		}
		for (int32 a = 0; a < Others.Num() && Others.Num() >= 2; ++a)
		{
			for (int32 b = a + 1; b < Others.Num(); ++b)
			{
				if (FVector::Dist(Others[a]->GetActorLocation(), Others[b]->GetActorLocation()) < L->BlastRadiusCm() * 1.5f) { Others.RemoveAt(b); --b; }
			}
		}
		Check(Others.Num() >= 2, FString::Printf(TEXT("shgulch: two bad guys apart for the blast check (%d)"), Others.Num()));
		if (Others.Num() < 2) { ShotStep = 90; return; }
		for (int32 k = 0; k < 2; ++k)
		{
			AOutlawsEnemy* E = Others[k];
			const int32 DynWas = UOutlawsStats::MyDynamiteKills(L);
			const FVector At = FeetOf(E) + FVector(0.0, 0.0, 10.0);
			if (k == 0)
			{
				// (f) a bad guy's stick at his own feet, his own: it spares him
				const float HpWas = E->GetHealth();
				const int32 SpWas = L->EnemyBlastsSpared;
				{
					TGuardValue<bool> ByEnemy(L->bEnemyBlast, true);
					TGuardValue<TWeakObjectPtr<AOutlawsEnemy>> Spares(L->EnemyBlastBy, E);
					L->Explode(At);
				}
				Check(!E->IsDead() && FMath::IsNearlyEqual(E->GetHealth(), HpWas) && L->EnemyBlastsSpared == SpWas + 1, FString::Printf(TEXT("shgulch: (f) his own stick at %s's feet spares him (health %.2f -> %.2f, dead %d, spared %d)"),
					*E->GetOutlawsName(), HpWas, E->GetHealth(), E->IsDead() ? 1 : 0, L->EnemyBlastsSpared - SpWas));
				// ... and the next there, thrown by SH_DICK, is a bad guy's that kills him (the others in a blast's reach hurt as ever)
				TGuardValue<bool> ByEnemy(L->bEnemyBlast, true);
				TGuardValue<TWeakObjectPtr<AOutlawsEnemy>> Spares(L->EnemyBlastBy, Thrower);
				L->Explode(At);
			}
			else
			{
				L->Explode(At);
			}
			const int32 DynNow = UOutlawsStats::MyDynamiteKills(L);
			if (k == 0) Check(E->IsDead() && DynNow == DynWas && E->StatsBy == uint8(EOutlawsStatsBy::Nobody), FString::Printf(TEXT("shgulch: a bad guy's stick kills %s, no one's kill (dead %d, the player's dynamite kills %d -> %d, by %d)"),
				*E->GetOutlawsName(), E->IsDead() ? 1 : 0, DynWas, DynNow, int32(E->StatsBy)));
			else Check(E->IsDead() && DynNow > DynWas, FString::Printf(TEXT("shgulch: (the control) the player's own stick kills %s, the player's (dead %d, dynamite kills %d -> %d)"),
				*E->GetOutlawsName(), E->IsDead() ? 1 : 0, DynWas, DynNow));
		}
		ShotStep = 90;
		return;
	}
	// ---- BGY2BOOM: none at 50 m ----
	case 70:
	{
		if (!T) { ShotStep = 90; return; }
		const FVector Mid = FindSpot(T, { 5000.0, 4800.0, 5300.0, 5600.0 }, true, false);
		const FVector Any = Mid.IsZero() ? FindSpot(T, { 5000.0, 4800.0, 5300.0, 5600.0 }, false, false) : Mid;
		Check(!Any.IsZero(), FString::Printf(TEXT("%s: a spot 50 m from BGY2BOOM (%s, %s)"), *Lv, *Any.ToCompactString(), Mid.IsZero() ? TEXT("out of his sight") : TEXT("in his sight")));
		if (Any.IsZero()) { ShotStep = 90; return; }
		PutPlayer(T, Any);
		ThrowsWas = T->ThrowsTotal;
		Until = ShotTime + 10.f;
		ShotStep = 71;
		return;
	}
	case 71:
	{
		if (!T) { ShotStep = 90; return; }
		if (ShotTime < Until) return;
		float D = 0.f;
		const bool bIn = T->TargetInThrowRange(&D);
		Check(!bIn && T->ThrowsTotal == ThrowsWas, FString::Printf(TEXT("%s: none at %.1f m (past THROW_MAX %.0f units = %.0f m): in range %d, %d throws in 10 s"), *Lv, D / 100.0, T->Def.ThrowMax, T->Def.ThrowMax * 0.3, bIn ? 1 : 0, T->ThrowsTotal - ThrowsWas));
		ShotStep = 90;
		return;
	}
	// ---- -OLOldDynThrow: they hold fire ----
	case 80:
	{
		if (!T) { ShotStep = 90; return; }
		const FVector Mid = FindSpot(T, { 900.0, 1000.0, 800.0, 1200.0 }, true, true);
		Check(!Mid.IsZero(), FString::Printf(TEXT("%s (old rule): a clear spot near %s"), *Lv, *T->GetOutlawsName()));
		if (Mid.IsZero()) { ShotStep = 90; return; }
		PutPlayer(T, Mid);
		T->Wake(false);
		ThrowsWas = T->ThrowsTotal;
		BlastsWas = T->ShotsTotal;
		Until = ShotTime + T->Def.ShootTimer + 3.f;
		ShotStep = 81;
		return;
	}
	case 81:
	{
		if (!T) { ShotStep = 90; return; }
		if (ShotTime < Until) return;
		Shot(TEXT("flight"));
		Check(T->ThrowsTotal == ThrowsWas && T->ShotsTotal == BlastsWas && L->EnemyThrownCount() == 0, FString::Printf(TEXT("%s (old rule): %s holds fire: %d throws, %d shots, %d in the air in %.1f s"),
			*Lv, *T->GetOutlawsName(), T->ThrowsTotal - ThrowsWas, T->ShotsTotal - BlastsWas, L->EnemyThrownCount(), T->Def.ShootTimer + 3.f));
		ShotStep = 90;
		return;
	}
	case 90:
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dynthrow RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
