// BUG-SAMENEMY (user 2 Nov, snapshot #40, shgulch: "Sam", the rabbit Max in the secret room, stood frozen as a 3D figure):
// -OLShots -OLLevel=<shgulch|civlwar2|gallery|miner> -OLNoFrontEnd -OLMaxProbe [-OLOldMaxStatue].  (a) MAX is a bad guy
// (an AOutlawsEnemy, HEALTH 200, counted), no sprite of him is left standing and none is made the MAX.obj figure; (b) the
// player set down near him, in his sight: he turns or moves, and (shgulch, SHOOT_TIMER_TIME 30) fires; (c) shot to death:
// dead, gone (his art has no death), nothing dropped, the kill counted.  Lines "OLSHOT max check PASS|FAIL: ..." and
// "OLSHOT max RESULT"; shots max_<level>_<near|dead>[_old].png in Saved\OLShots.

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsEnemy.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLMaxProbe) { return OLFlags::Param(TEXT("OLMaxProbe")); }

OL_SHOT_TEST(OLMaxProbe, 1460)
{
	static int32 Passes = 0, Fails = 0, KillsWas = 0, ShotsWas = 0;
	static float Wait = 0.f, Until = 0.f;
	static TWeakObjectPtr<AOutlawsEnemy> Max;
	static FVector Home = FVector::ZeroVector;
	static float HomeYaw = 0.f, MostMoved = 0.f, MostTurned = 0.f;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT max check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	const bool bOld = OLFlags::Param(TEXT("OLOldMaxStatue"));
	const FString Lv = L->LevelName.ToLower();
	auto Shot = [&](const TCHAR* What)
	{
		const FString Name = FString::Printf(TEXT("max_%s_%s%s"), *Lv, What, bOld ? TEXT("_old") : TEXT(""));
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT max shot %s.png"), *Name);
	};
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 4.f || !bUp) return;
		L->bInvincible = true;
		// (a) what MAX is
		TSet<const AOutlawsSpriteActor*> Bodies;
		int32 NumMax = 0;
		for (const TWeakObjectPtr<AOutlawsEnemy>& W : L->GetEnemies())
		{
			AOutlawsEnemy* E = W.Get();
			if (!E) continue;
			if (E->GetSprite()) Bodies.Add(E->GetSprite());
			if (!E->GetOutlawsName().Equals(TEXT("MAX"), ESearchCase::IgnoreCase)) continue;
			++NumMax;
			Max = E;
		}
		int32 Statues = 0, Models = 0;
		for (TActorIterator<AOutlawsSpriteActor> It(GetWorld()); It; ++It)
		{
			if (Bodies.Contains(*It) || !It->OutlawsName.Equals(TEXT("MAX"), ESearchCase::IgnoreCase)) continue;
			++Statues;
			if (It->Model) ++Models;
		}
		AOutlawsEnemy* E = Max.Get();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT max %s: %d MAX bad guys (health %.0f of %.0f, art %d chors, model %s), %d MAX sprites not a bad guy's (%d of them a 3D figure); %d bad guys counted"),
			*Lv, NumMax, E ? E->GetHealth() : 0.f, E ? E->GetMaxHealth() : 0.f, E && E->GetSprite() ? E->GetSprite()->Chors.Num() : -1,
			E && E->GetSprite() && E->GetSprite()->Model ? TEXT("yes") : TEXT("no"), Statues, Models, L->DebugTotalBadGuys());
		if (Lv == TEXT("miner"))
		{
			// miner's MAX row (ID 21770) names sector 0, which the level has not: with the STUMP, CYCLE and SIGNODD3 rows by
			// it (X 900-960, far off the map), it is never placed, under either rule
			Check(NumMax == 0 && Statues == 0, FString::Printf(TEXT("miner: MAX's row (sector 0, off the map) is not placed (%d bad guys, %d sprites)"), NumMax, Statues));
			ShotStep = 9;
			return;
		}
		if (bOld)
		{
			Check(NumMax == 0 && Statues == 1 && Models == 1, FString::Printf(TEXT("%s (old rule): MAX a static figure, as the snapshot (%d bad guys, %d sprites, %d figures)"), *Lv, NumMax, Statues, Models));
			ShotStep = 9;
			return;
		}
		Check(NumMax == 1 && E && FMath::IsNearlyEqual(E->GetMaxHealth(), 200.f) && !E->IsDead(), FString::Printf(TEXT("%s: MAX is one live bad guy with HEALTH 200 (%d, %.0f)"), *Lv, NumMax, E ? E->GetMaxHealth() : 0.f));
		Check(Statues == 0 && Models == 0 && E && E->GetSprite() && !E->GetSprite()->Model, FString::Printf(TEXT("%s: no MAX sprite or 3D figure besides the bad guy's own art (%d, %d)"), *Lv, Statues, Models));
		if (!E) { ShotStep = 9; return; }
		// (b) the player set down near him where the line to him is clear, facing him
		Home = E->GetActorLocation();
		HomeYaw = E->GetSprite() ? E->GetSprite()->OutlawsYaw : 0.f;
		const UWorld* World = GetWorld();
		FVector At = FVector::ZeroVector;
		for (const double R : { 350.0, 250.0, 180.0 })
		{
			for (int32 k = 0; k < 16 && At.IsZero(); ++k)
			{
				// in front of him first (the way his sprite faces), then round him
				const double A = FMath::DegreesToRadians(double(HomeYaw)) + (k % 2 ? 1.0 : -1.0) * ((k + 1) / 2) * PI / 8.0;
				const FVector P = Home + FVector(FMath::Cos(A) * R, FMath::Sin(A) * R, 20.0);
				FHitResult H;
				FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsMaxProbe), false, this);
				Q.AddIgnoredActor(E);
				if (World->LineTraceSingleByChannel(H, Home + FVector(0.0, 0.0, 20.0), P, ECC_Visibility, Q)) continue;
				if (!World->LineTraceSingleByChannel(H, P, P - FVector(0.0, 0.0, 300.0), ECC_Visibility, Q)) continue;
				At = H.ImpactPoint + FVector(0.0, 0.0, 100.0);
			}
			if (!At.IsZero()) break;
		}
		Check(!At.IsZero(), FString::Printf(TEXT("%s: a clear spot near MAX at %s (he is at %s)"), *Lv, *At.ToCompactString(), *Home.ToCompactString()));
		if (At.IsZero()) { ShotStep = 2; return; }
		const FRotator Look = (Home - At).Rotation();
		TeleportTo(At, FRotator(0.0, Look.Yaw, 0.0), false, true);
		if (Controller) Controller->SetControlRotation(FRotator(0.0, Look.Yaw, 0.0));
		ShotsWas = E->ShotsTotal;
		MostMoved = MostTurned = 0.f;
		Wait = ShotTime + 3.f;
		Until = ShotTime + (Lv == TEXT("shgulch") ? 75.f : 12.f);
		ShotStep = 1;
		return;
	}
	case 1:
	{
		AOutlawsEnemy* E = Max.Get();
		if (!E) { ShotStep = 9; return; }
		L->bEnemiesActive = true;          // (ShotTest stills them each tick: a fight here)
		MostMoved = FMath::Max(MostMoved, float(FVector::Dist2D(E->GetActorLocation(), Home)));
		if (E->GetSprite()) MostTurned = FMath::Max(MostTurned, FMath::Abs(FMath::FindDeltaAngleDegrees(HomeYaw, E->GetSprite()->OutlawsYaw)));
		if (ShotTime >= Wait && Wait > 0.f) { Shot(TEXT("near")); Wait = -1.f; }
		const bool bShot = E->ShotsTotal > ShotsWas;
		if (ShotTime < Until && !(bShot && MostMoved + MostTurned > 1.f)) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT max %s alerted: state %d, moved up to %.0f cm, turned up to %.0f deg, %d shots fired, chor %d on show (%s; sight: %s)"),
			*Lv, int32(E->GetState()), MostMoved, MostTurned, E->ShotsTotal - ShotsWas, E->GetSprite() ? E->GetSprite()->GetPlayingChor() : -1, *E->DebugMoveState(), *E->DebugSight());
		const float Facing = E->GetSprite() ? FMath::Abs(FMath::FindDeltaAngleDegrees(E->GetSprite()->OutlawsYaw, float((GetActorLocation() - E->GetActorLocation()).Rotation().Yaw))) : 180.f;
		Check(E->GetState() != EOutlawsEnemyState::Watch && Facing < 30.f, FString::Printf(TEXT("%s: MAX alerted (state %d) and facing the player (%.0f deg off; moved %.0f cm, turned %.0f deg)"),
			*Lv, int32(E->GetState()), Facing, MostMoved, MostTurned));
		if (Lv == TEXT("shgulch")) Check(bShot, FString::Printf(TEXT("shgulch: MAX fires at the player (%d shots in %.0f s)"), E->ShotsTotal - ShotsWas, ShotTime - (Until - 75.f)));
		ShotStep = 2;
		return;
	}
	case 2:
	{
		// (c) shot to death
		AOutlawsEnemy* E = Max.Get();
		if (!E) { ShotStep = 9; return; }
		KillsWas = L->GetKills();
		const FVector From = GetActorLocation();
		for (int32 i = 0; i < 60 && !E->IsDead(); ++i) E->TakeHit(50.f, E->GetActorLocation(), (E->GetActorLocation() - From).GetSafeNormal(), From);
		Wait = ShotTime + 2.5f;
		ShotStep = 3;
		return;
	}
	case 3:
	{
		if (ShotTime < Wait) return;
		AOutlawsEnemy* E = Max.Get();
		if (!E) { ShotStep = 9; return; }
		const AOutlawsSpriteActor* S = E->GetSprite();
		Shot(TEXT("dead"));
		Check(E->IsDead() && S && S->IsHidden(), FString::Printf(TEXT("%s: MAX dead (%s) and gone, his art has no death (sprite hidden %s, chor %d on show)"),
			*Lv, E->IsDead() ? TEXT("yes") : TEXT("no"), S && S->IsHidden() ? TEXT("yes") : TEXT("no"), S ? S->GetPlayingChor() : -1));
		Check(L->GetKills() == KillsWas + 1, FString::Printf(TEXT("%s: the kill counted (%d -> %d)"), *Lv, KillsWas, L->GetKills()));
		ShotStep = 9;
		return;
	}
	case 9:
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT max RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
