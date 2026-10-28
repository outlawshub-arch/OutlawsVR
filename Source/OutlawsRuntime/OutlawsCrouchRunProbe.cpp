// REQ-CROUCHRUN (user 2026-10-28, snapshot 19's note: no crouch run): -OLShots -OLLevel=townex -OLNoFrontEnd -OLCrouchRunProbe
// [-OLOldCrouchRun].  First the rule itself (CrouchSpeedNow) rested, half spent and spent, and the other players' crouch walk
// legs for it (AOutlawsRemotePlayer::CrouchWalkLegs).  Then the game's own tick on the town's street, on the desktop and with
// the body as the headset has it (VR, this run only): a walk, a run, a crouch walk and a crouch run each from the same spot
// along the clearest line, the stamina kept full; the speed the movement allows (GetMaxSpeed) and the speed across the ground
// at 0.6 s.  The crouch run's share over the crouch walk must be the run's over the walk (-OLOldCrouchRun: none, and the
// desktop's crouch the fixed 480).  Last a crouch run with the stamina let run down: it drains as a run does.  Lines
// "OLSHOT crouchrun check PASS|FAIL: ..." and "OLSHOT crouchrun RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsRemotePlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

namespace
{
	struct FCrouchRunCase { bool bVR = false, bCrouch = false, bRun = false; float MaxSpeed = 0.f, Ground = 0.f, Half = 0.f; };
	const TCHAR* GaitName(const FCrouchRunCase& C) { return C.bCrouch ? (C.bRun ? TEXT("crouch run") : TEXT("crouch walk")) : (C.bRun ? TEXT("run") : TEXT("walk")); }
}

OL_SHOT_MATCH(OLCrouchRunProbe) { return OLFlags::Param(TEXT("OLCrouchRunProbe")); }

OL_SHOT_TEST(OLCrouchRunProbe, 16)
{
	static int32 Passes = 0, Fails = 0, CaseI = -1;
	static float CaseT = 0.f, DrainFrom = 0.f, DrainT = 0.f;
	static FVector Start = FVector::ZeroVector;
	static double Yaw = 0.0;
	static TArray<FCrouchRunCase> RunCases;
	if (!L || ShotStep < 0) return;
	const bool bOld = OldCrouchRun();
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT crouchrun check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	UCharacterMovementComponent* M = GetCharacterMovement();
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	// one case's body: VR or not, crouched (the desktop's key, the headset's turn stick held down) or not, running or not
	auto Hold = [&](const FCrouchRunCase& C)
	{
		bRunning = C.bRun;
		if (C.bVR) TestAxes.Add(TEXT("OLVR_RightY"), C.bCrouch ? -1.f : 0.f);
		else bCrouchHeld = C.bCrouch;
	};
	auto PutAtStart = [&]()
	{
		TeleportTo(Start, FRotator(0.0, Yaw, 0.0), false, true);
		if (Controller) Controller->SetControlRotation(FRotator(0.0, Yaw, 0.0));
		M->StopMovementImmediately();
	};
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 3.f || !bUp) return;
		L->bInvincible = true;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT crouchrun in %s, %s"), *L->LevelName, bOld ? TEXT("-OLOldCrouchRun (the rule before)") : TEXT("the new rule"));
		// (1) the rule: crouch walk 480, walk 960, run 1920, at full, half and no stamina
		for (const float Frac : { 1.f, 0.5f, 0.f })
		{
			const float RunNow = FMath::Lerp(VRWalkSpeed, VRRunSpeed, Frac);
			const float Walk = CrouchSpeedNow(VRCrouchSpeed, VRWalkSpeed, RunNow, false, bOld), Run = CrouchSpeedNow(VRCrouchSpeed, VRWalkSpeed, RunNow, true, bOld);
			const float Want = bOld ? 1.f : RunNow / VRWalkSpeed;
			Check(FMath::IsNearlyEqual(Run / Walk, Want, 0.001f), FString::Printf(TEXT("(1) stamina %.0f %%: crouch walk %.0f, crouch run %.0f (%.2f times; the run %.0f over the walk %.0f is %.2f)"),
				Frac * 100.f, Walk, Run, Run / Walk, RunNow, VRWalkSpeed, RunNow / VRWalkSpeed));
		}
		// the other players' legs: a crouch run at the default dial (pace 0.5: 480 cm/s) and a crouch walk (240)
		const float LegsRun = AOutlawsRemotePlayer::CrouchWalkLegs(480.f, 0.5f), LegsWalk = AOutlawsRemotePlayer::CrouchWalkLegs(240.f, 0.5f);
		Check(FMath::IsNearlyEqual(LegsWalk, 1.f, 0.001f) && FMath::IsNearlyEqual(LegsRun, bOld ? 1.5f : 2.f, 0.001f),
			FString::Printf(TEXT("(1) the crouch walk's legs seen by the others: %.2f crouch walking, %.2f crouch running (wanted 1.00 and %.2f)"), LegsWalk, LegsRun, bOld ? 1.5f : 2.f));
		// the clearest line from here, at the eyes and the knees, over 16 headings
		Start = GetActorLocation();
		const double KneeZ = Start.Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 45.0;
		double Best = -1.0;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsCrouchRunProbe), false, this);
		for (int32 i = 0; i < 16; ++i)
		{
			const FVector D = FRotator(0.0, i * 22.5, 0.0).Vector();
			double Free = 1e9;
			for (const double Z : { Start.Z + 60.0, KneeZ })
			{
				const FVector S(Start.X, Start.Y, Z);
				FHitResult H;
				Free = FMath::Min(Free, GetWorld()->LineTraceSingleByChannel(H, S, S + D * 1500.0, ECC_Visibility, Q) ? double(H.Distance) : 1500.0);
			}
			if (Free > Best) { Best = Free; Yaw = i * 22.5; }
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT crouchrun start %s, yaw %.1f (%.0f cm clear); pace %.2f"), *Start.ToString(), Yaw, Best, L->Pace());
		// (2) the game's own tick: the desktop's four, then the headset's four
		RunCases.Reset();
		for (const bool bV : { false, true })
		{
			for (const bool bC : { false, true })
			{
				for (const bool bR : { false, true })
				{
					FCrouchRunCase C;
					C.bVR = bV;
					C.bCrouch = bC;
					C.bRun = bR;
					RunCases.Add(C);
				}
			}
		}
		CaseI = -1;
		ShotStep = 1;
		return;
	}
	case 1:
	{
		// the next case: stand up and stop first (a tick of each), then from the start
		if (CaseI >= 0 && CaseT < 1.2f)
		{
			CaseT += Dt;
			FCrouchRunCase& C = RunCases[CaseI];
			Hold(C);
			Stamina = 100.f;
			if (CaseT >= 0.6f && C.MaxSpeed <= 0.f)
			{
				C.MaxSpeed = M->GetMaxSpeed();
				C.Ground = float(GetVelocity().Size2D());
				C.Half = GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT crouchrun %s %s: the movement allows %.0f cm/s, %.0f across the ground, body half height %.1f, crouched %d, running %d"),
					C.bVR ? TEXT("VR") : TEXT("desktop"), GaitName(C), C.MaxSpeed, C.Ground, C.Half, C.Half < 60.f ? 1 : 0, bRunning ? 1 : 0);
			}
			return;
		}
		++CaseI;
		WalkTestLeft = 0.f;
		bRunning = false;
		bCrouchHeld = false;
		bBarrelLatched = false;              // (the headset's stick let go too: the next case's push crouches afresh)
		TestAxes.Remove(TEXT("OLVR_RightY"));
		if (!RunCases.IsValidIndex(CaseI))
		{
			ShotStep = 2;
			return;
		}
		FCrouchRunCase& C = RunCases[CaseI];
		if (C.bVR && !bVR)
		{
			// the headset's body, as -OLClimbProbe stands it: the head 170 cm over the play-space floor (this run only); the
			// desktop's engine crouch let go first (the headset sizes the body itself and never calls UnCrouch)
			if (bIsCrouched) UnCrouch();
			bVR = true;
			bAutoCrouched = bStandPending = false;
			CrouchAmount = 1.f;
			CrouchDrop = CrouchFull = 0.f;
			HeightOffset = 0.f;
			Camera->SetRelativeLocation(FVector(0.0, 0.0, 170.0));
			ApplyModeSettings();
		}
		PutAtStart();
		Hold(C);
		WalkTestLeft = 1.2f;
		CaseT = 0.f;
		return;
	}
	case 2:
	{
		// (2) the shares: the crouch run over the crouch walk against the run over the walk, by the speed the movement allows
		for (const bool bV : { false, true })
		{
			const FCrouchRunCase* Ca[4] = {};
			for (const FCrouchRunCase& C : RunCases) if (C.bVR == bV) Ca[(C.bCrouch ? 2 : 0) + (C.bRun ? 1 : 0)] = &C;
			if (!Ca[0] || !Ca[1] || !Ca[2] || !Ca[3]) continue;
			const TCHAR* Who = bV ? TEXT("VR") : TEXT("desktop");
			const float RunShare = Ca[1]->MaxSpeed / FMath::Max(1.f, Ca[0]->MaxSpeed), CrouchShare = Ca[3]->MaxSpeed / FMath::Max(1.f, Ca[2]->MaxSpeed);
			Check(Ca[2]->Half < 60.f && Ca[3]->Half < 60.f && Ca[0]->Half >= 60.f && Ca[1]->Half >= 60.f,
				FString::Printf(TEXT("(2) %s: crouched for the crouched two, standing for the others (half heights %.0f %.0f %.0f %.0f)"), Who, Ca[0]->Half, Ca[1]->Half, Ca[2]->Half, Ca[3]->Half));
			Check(bOld ? FMath::IsNearlyEqual(CrouchShare, 1.f, 0.01f) : FMath::IsNearlyEqual(CrouchShare, RunShare, 0.01f * RunShare),
				FString::Printf(TEXT("(2) %s: the crouch run allows %.0f, the crouch walk %.0f (%.2f times); the run %.0f, the walk %.0f (%.2f times)%s"), Who, Ca[3]->MaxSpeed, Ca[2]->MaxSpeed,
				CrouchShare, Ca[1]->MaxSpeed, Ca[0]->MaxSpeed, RunShare, bOld ? TEXT("; wanted no faster (-OLOldCrouchRun)") : TEXT("")));
			// the crouch walk against the walk: half on both (the desktop's now with the speed dial; -OLOldCrouchRun: its fixed 480)
			const float WalkShare = Ca[2]->MaxSpeed / FMath::Max(1.f, Ca[0]->MaxSpeed);
			const float WantWalk = bOld && !bV ? DesktopCrouchSpeed / FMath::Max(1.f, Ca[0]->MaxSpeed) : 0.5f;
			Check(FMath::IsNearlyEqual(WalkShare, WantWalk, 0.01f), FString::Printf(TEXT("(2) %s: the crouch walk is %.2f of the walk (wanted %.2f)"), Who, WalkShare, WantWalk));
			// across the ground, where the line was clear: each at what the movement allows (within a tenth)
			FString Ground;
			bool bGroundOk = true;
			for (int32 k = 0; k < 4; ++k)
			{
				Ground += FString::Printf(TEXT("%s%s %.0f of %.0f"), k ? TEXT(", ") : TEXT(""), GaitName(*Ca[k]), Ca[k]->Ground, Ca[k]->MaxSpeed);
				bGroundOk &= Ca[k]->Ground >= 0.9f * Ca[k]->MaxSpeed && Ca[k]->Ground <= 1.02f * Ca[k]->MaxSpeed;
			}
			Check(bGroundOk, FString::Printf(TEXT("(2) %s across the ground at 0.6 s: %s"), Who, *Ground));
			Check(FMath::IsNearlyEqual(AOutlawsRemotePlayer::CrouchWalkLegs(Ca[3]->Ground, L->Pace()), FMath::Clamp(Ca[3]->Ground / (480.f * L->Pace()), 0.5f, bOld ? 1.5f : 2.f), 0.01f)
				&& (bOld || AOutlawsRemotePlayer::CrouchWalkLegs(Ca[3]->Ground, L->Pace()) > 1.8f || CrouchShare < 1.8f),
				FString::Printf(TEXT("(2) %s: the others see the crouch run's legs at %.2f of the crouch walk's pace"), Who, AOutlawsRemotePlayer::CrouchWalkLegs(Ca[3]->Ground, L->Pace())));
		}
		// (3) a crouch run with the stamina let run down (the headset's body): it drains as a run does
		PutAtStart();
		Hold(RunCases.Last());
		WalkTestLeft = 1.2f;
		Stamina = 100.f;
		DrainFrom = Stamina;
		DrainT = 0.f;
		ShotStep = 3;
		return;
	}
	case 3:
	{
		Hold(RunCases.Last());
		DrainT += Dt;
		if (DrainT < 1.0f) return;
		const float RunRate = StaminaRate(true, true, true, L->Pace(), false, OldStamina());
		const float Lost = DrainFrom - Stamina;
		Check(RunRate < 0.f && Lost > 0.6f * -RunRate * DrainT && Lost < 1.4f * -RunRate * DrainT,
			FString::Printf(TEXT("(3) crouch running %.2f s spent %.2f stamina (a run's rate %.2f a second: %.2f)"), DrainT, Lost, RunRate, -RunRate * DrainT));
		WalkTestLeft = 0.f;
		bRunning = false;
		TestAxes.Remove(TEXT("OLVR_RightY"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT crouchrun RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	}
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
