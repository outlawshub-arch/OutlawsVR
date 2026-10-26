// -OLShots -OLBodyTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLBodyTest) { return OLFlags::Param(TEXT("OLBodyTest")); }

OL_SHOT_TEST(OLBodyTest, 310)
{
	// a body taken up and thrown (user 2026-10-01): the nearest man killed, his pool begun, taken up (the pool stops,
	// drops fall), thrown (he lands dead, a smaller pool), then a chicken or tarantula killed (its remains on the floor)
	static TWeakObjectPtr<AOutlawsEnemy> Body, Critter;
	// the right hand held out before the eyes (no tracked hand on the desktop)
	{
		const FVector Fl = FVector(Camera->GetForwardVector().X, Camera->GetForwardVector().Y, 0.0).GetSafeNormal();
		FrameOf(1)->SetWorldLocationAndRotation(Camera->GetComponentLocation() + Fl * 50.0 + Camera->GetRightVector() * 15.0 - FVector(0.0, 0.0, 15.0), Fl.Rotation());
	}
	auto Kill = [](AOutlawsEnemy* B) { for (int32 i = 0; B && i < 60 && !B->IsDead(); ++i) B->TakeHit(50.f, B->GetActorLocation(), FVector(1.0, 0.0, 0.0), B->GetActorLocation() - FVector(100.0, 0.0, 0.0)); };
	auto Nearest = [&](bool bCritter) -> AOutlawsEnemy*
	{
		AOutlawsEnemy* Best = nullptr;
		double BestD = TNumericLimits<double>::Max();
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
		{
			AOutlawsEnemy* E = Wk.Get();
			if (!E || E->IsDead() || E->bBoss || E->IsCritter() != bCritter) continue;
			const double D = FVector::Dist(E->GetActorLocation(), GetActorLocation());
			if (D < BestD) { BestD = D; Best = E; }
		}
		return Best;
	};
	if (ShotStep == 0 && ShotTime > 3.f && L)
	{
		Body = Nearest(false);
		Critter = Nearest(true);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT body test: man %s, critter %s"), Body.IsValid() ? *Body->GetName() : TEXT("none"), Critter.IsValid() ? *Critter->GetName() : TEXT("none"));
		Kill(Body.Get());
		Kill(Critter.Get());
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 7.f)
	{
		if (Body.IsValid()) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT body at rest: %s (expect dead 1 pool 1)"), *Body->DebugBodyState());
		if (Critter.IsValid())
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT critter: %s (expect remains 1)"), *Critter->DebugBodyState());
			// a look down at the remains
			const FVector At = Critter->GetActorLocation();
			TeleportTo(At + FVector(-120.0, 0.0, 60.0), FRotator(0.0, 0.0, 0.0), false, true);
			if (Controller) Controller->SetControlRotation(FRotator(-45.0, 0.0, 0.0));
		}
		const bool bTaken = Body.IsValid() && TryGrabEnemy(1, Body.Get());
		TestAxes.Add(TEXT("OLVR_RightGrip"), 1.f);     // (held: no hand on the desktop squeezes)
		bGripArmed[1] = false;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT body taken up: %d, %s (expect held 1 pool 0)"), bTaken ? 1 : 0, Body.IsValid() ? *Body->DebugBodyState() : TEXT("-"));
		++ShotStep;
	}
	else if (ShotStep == 2 && ShotTime > 8.f) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_remains.png"), false, false); ++ShotStep; }
	else if (ShotStep == 3 && ShotTime > 11.f)
	{
		// thrown ahead and a little up
		const FVector Vel = GetActorForwardVector() * 700.0 + FVector(0.0, 0.0, 300.0);
		LetGoEnemy(1, true, &Vel);
		TestAxes.Remove(TEXT("OLVR_RightGrip"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT body thrown: %s"), Body.IsValid() ? *Body->DebugBodyState() : TEXT("-"));
		++ShotStep;
	}
	else if (ShotStep == 4 && ShotTime > 16.f)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT body landed: %s (expect thrown 0 pool 1, bleed under 1)"), Body.IsValid() ? *Body->DebugBodyState() : TEXT("-"));
		++ShotStep;
	}
	else if (ShotStep == 5 && ShotTime > 17.f) { FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; }
}

#endif // WITH_OUTLAWS_TESTS
