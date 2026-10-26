// -OLShots -OLHeldSwayTest (REQ-HELDSWAY): a bad guy held out before the eyes, the hand moved sideways at 150 cm/s for a
// second and stopped, then toward the eyes at 150 cm/s for 0.6 s and stopped, then sideways again and let go on the move.
// Each frame the legs' lag (cm, across the floor), the bottom corners' height change (cm) and the bottom edge's turn (deg)
// are logged; the verdict: lag while moving, no height change ever, the turn within ol.heldsway.MaxYawDeg, back under
// 1 cm and 1 degree within 1.2 s of each stop, and none the frame after the let-go.  Screenshots at the two peaks
// (Saved/OLShots/ol_heldsway_*.png).
#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsEnemy.h"
#include "OutlawsSpriteActor.h"
#include "Camera/CameraComponent.h"
#include "HAL/IConsoleManager.h"
#include "MotionControllerComponent.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
#include "OutlawsShotTests.h"

#if WITH_OUTLAWS_TESTS
void AOutlawsPlayer::HeldSwayTestTick(float Dt)
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L || ShotTime < 0.f) return;
	static TWeakObjectPtr<AOutlawsEnemy> Man;
	static float T0 = 0.f, StopAt = 0.f, Peak = 0.f, PeakYaw = 0.f, MaxDz = 0.f, MaxYawAll = 0.f, SettledAt = -1.f;
	static int32 Phase = 0, Frame = 0, MovingFrames = 0, MovingZero = 0, Fails = 0;
	static FVector Base, Hand;
	static bool bShot = false;
	const FVector Fl = FVector(Camera->GetForwardVector().X, Camera->GetForwardVector().Y, 0.0).GetSafeNormal();
	const FVector Rt = FVector(Camera->GetRightVector().X, Camera->GetRightVector().Y, 0.0).GetSafeNormal();
	auto Finish = [&](const TCHAR* Why)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT heldsway: %s (%d failed checks)"), Why, Fails);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	};
	if (Phase == 0)
	{
		if (ShotTime < 3.f) return;
		AOutlawsEnemy* Pick = nullptr;
		double BestD = TNumericLimits<double>::Max();
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
		{
			AOutlawsEnemy* E = Wk.Get();
			if (!E || E->IsDead() || E->bBoss || E->IsCritter() || E->IsDormant()) continue;
			const double D = FVector::Dist(E->GetActorLocation(), GetActorLocation());
			if (D < BestD) { BestD = D; Pick = E; }
		}
		Man = Pick;
		Base = Camera->GetComponentLocation() + Fl * 170.0 + FVector(0.0, 0.0, 20.0);
		Hand = Base;
		GripOf(1)->SetWorldLocationAndRotation(Hand, Fl.Rotation());
		bool bOk = false;
		if (Pick)
		{
			Pick->SetActorLocation(Base + Fl * 40.0, false, nullptr, ETeleportType::TeleportPhysics);
			bOk = TryGrabEnemy(1, Pick);
			TestAxes.Add(TEXT("OLVR_RightGrip"), 1.f);     // (held: no hand on the desktop squeezes)
			bGripArmed[1] = false;
		}
		const IConsoleVariable* Cv[] = { IConsoleManager::Get().FindConsoleVariable(TEXT("ol.heldsway.Stiffness")), IConsoleManager::Get().FindConsoleVariable(TEXT("ol.heldsway.Damping")),
			IConsoleManager::Get().FindConsoleVariable(TEXT("ol.heldsway.MaxLagCm")), IConsoleManager::Get().FindConsoleVariable(TEXT("ol.heldsway.MaxYawDeg")) };
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT heldsway: man %s, held %d; stiffness %.1f damping %.2f max lag %.0f cm max turn %.0f deg"), Pick ? *Pick->GetName() : TEXT("none"), bOk ? 1 : 0,
			Cv[0] ? Cv[0]->GetFloat() : -1.f, Cv[1] ? Cv[1]->GetFloat() : -1.f, Cv[2] ? Cv[2]->GetFloat() : -1.f, Cv[3] ? Cv[3]->GetFloat() : -1.f);
		if (!bOk) { ++Fails; Finish(TEXT("FAIL no man held")); return; }
		T0 = ShotTime;
		Phase = 1;
		return;
	}
	AOutlawsEnemy* E = Man.Get();
	AOutlawsSpriteActor* S = E ? E->GetSprite() : nullptr;
	if (!S) { ++Fails; Finish(TEXT("FAIL the man is gone")); return; }
	const float T = ShotTime - T0;
	const float MaxYawCap = []() { const IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(TEXT("ol.heldsway.MaxYawDeg")); return V ? V->GetFloat() : 30.f; }();
	// the phases: 1 held still (the grab's glide settles), 2 sideways, 3 stopped, 4 toward the eyes, 5 stopped, 6 sideways
	// again, 7 let go, 8 the frame after
	const TCHAR* PhaseName[] = { TEXT(""), TEXT("hold"), TEXT("side"), TEXT("stop1"), TEXT("toward"), TEXT("stop2"), TEXT("side2"), TEXT("letgo"), TEXT("after") };
	constexpr double Speed = 150.0;
	bool bMoving = false;
	if (Phase == 1 && T > 2.f) { Phase = 2; StopAt = T; }
	if (Phase == 2) { Hand += Rt * Speed * Dt; bMoving = true; if (T - StopAt >= 1.f) { Phase = 3; StopAt = T; SettledAt = -1.f; } }
	else if (Phase == 4) { Hand -= Fl * Speed * Dt; bMoving = true; if (T - StopAt >= 0.6f) { Phase = 5; StopAt = T; SettledAt = -1.f; } }
	else if (Phase == 6) { Hand -= Rt * Speed * Dt; bMoving = true; if (T - StopAt >= 0.5f) Phase = 7; }
	HandVelocity[1] = HandWorldVel[1] = FVector::ZeroVector;       // (the let-go a drop, not a throw)
	GripOf(1)->SetWorldLocationAndRotation(Hand, Fl.Rotation());
	FVector BL, BR, RL, RR;
	const bool bShown = S->SwingBottom(BL, BR, RL, RR);
	const float Lag = float(S->SwingOffset().Size2D());
	const float Yaw = S->SwingYaw();
	const float Dz = bShown ? float(FMath::Max(FMath::Abs(BL.Z - RL.Z), FMath::Abs(BR.Z - RR.Z))) : 0.f;
	++Frame;
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT heldsway f %d t %.3f %s: lag %.2f cm (%s) dz %.4f yaw %.2f shown %d held %d"), Frame, T, PhaseName[Phase], Lag,
		*S->SwingOffset().ToCompactString(), Dz, Yaw, bShown ? 1 : 0, E->bHeldByPlayer ? 1 : 0);
	MaxDz = FMath::Max(MaxDz, Dz);
	MaxYawAll = FMath::Max(MaxYawAll, FMath::Abs(Yaw));
	if (bMoving && T - StopAt > 0.1f)
	{
		++MovingFrames;
		if (Lag < 0.5f) ++MovingZero;
	}
	if (Phase == 2 || Phase == 4)
	{
		Peak = FMath::Max(Peak, Lag);
		PeakYaw = FMath::Max(PeakYaw, FMath::Abs(Yaw));
		if (!bShot && Phase == 2 && T - StopAt > 0.8f)
		{
			bShot = true;
			FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_heldsway_side.png"), false, false);
		}
		if (bShot && Phase == 4 && T - StopAt > 0.5f)
		{
			bShot = false;
			FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_heldsway_toward.png"), false, false);
		}
	}
	if (Phase == 3 || Phase == 5)
	{
		const bool bIn = Lag < 1.f && FMath::Abs(Yaw) < 1.f;
		if (!bIn) SettledAt = -1.f;
		else if (SettledAt < 0.f) SettledAt = T - StopAt;
		if (T - StopAt >= 1.5f)
		{
			const bool bOk = SettledAt >= 0.f && SettledAt <= 1.2f;
			Fails += bOk ? 0 : 1;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT heldsway %s: peak lag %.1f cm, peak turn %.1f deg; under 1 cm and 1 deg %.2f s after the stop: %s"), Phase == 3 ? TEXT("sideways") : TEXT("toward"),
				Peak, PeakYaw, SettledAt, bOk ? TEXT("PASS") : TEXT("FAIL"));
			if (Phase == 3 && Peak < 5.f) { ++Fails; UE_LOG(LogOutlaws, Display, TEXT("OLSHOT heldsway sideways: FAIL the legs hardly trailed")); }
			if (Phase == 5 && PeakYaw < 3.f) { ++Fails; UE_LOG(LogOutlaws, Display, TEXT("OLSHOT heldsway toward: FAIL the bottom edge hardly turned")); }
			Peak = PeakYaw = 0.f;
			Phase = Phase == 3 ? 4 : 6;
			StopAt = T;
		}
	}
	else if (Phase == 7)
	{
		const float Before = Lag;
		TestAxes.Remove(TEXT("OLVR_RightGrip"));
		LetGoEnemy(1, false, nullptr);
		FVector A, B2, C, D;
		const bool bAfter = S->SwingBottom(A, B2, C, D);
		const float LagAfter = float(S->SwingOffset().Size2D());
		const bool bOk = LagAfter == 0.f && S->SwingYaw() == 0.f && !bAfter;
		Fails += bOk ? 0 : 1;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT heldsway let go on the move: lag %.2f cm before, %.2f cm and %.2f deg after (swung quad %d): %s"), Before, LagAfter, S->SwingYaw(), bAfter ? 1 : 0, bOk ? TEXT("PASS") : TEXT("FAIL"));
		Phase = 8;
		StopAt = T;
	}
	else if (Phase == 8 && T - StopAt > 0.5f)
	{
		// (dropped and falling: still none)
		FVector A, B2, C, D;
		const bool bLater = S->SwingBottom(A, B2, C, D);
		const bool bLaterOk = !bLater && S->SwingOffset().IsZero();
		Fails += bLaterOk ? 0 : 1;
		const bool bMoveOk = MovingFrames > 0 && MovingZero == 0;
		const bool bDzOk = MaxDz < 0.001f;
		const bool bYawOk = MaxYawAll <= MaxYawCap + 0.01f;
		Fails += (bMoveOk ? 0 : 1) + (bDzOk ? 0 : 1) + (bYawOk ? 0 : 1);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT heldsway: lag on %d of %d moving frames (%s); bottom height change at most %.4f cm (%s); turn at most %.2f deg of %.0f (%s); half a second after the let-go lag %.2f (%s)"),
			MovingFrames - MovingZero, MovingFrames, bMoveOk ? TEXT("PASS") : TEXT("FAIL"), MaxDz, bDzOk ? TEXT("PASS") : TEXT("FAIL"), MaxYawAll, MaxYawCap, bYawOk ? TEXT("PASS") : TEXT("FAIL"),
			S->SwingOffset().Size(), bLaterOk ? TEXT("PASS") : TEXT("FAIL"));
		Finish(Fails == 0 ? TEXT("PASS") : TEXT("FAIL"));
	}
	if (T > 20.f) { ++Fails; Finish(TEXT("FAIL timed out")); }
}

OL_SHOT_MATCH(OLHeldSwayTest) { return OLFlags::Param(TEXT("OLHeldSwayTest")); }
OL_SHOT_TEST(OLHeldSwayTest, 30)
{
	HeldSwayTestTick(Dt);            // (REQ-HELDSWAY: OutlawsPlayerHeldSway.cpp)
}
#endif // WITH_OUTLAWS_TESTS
