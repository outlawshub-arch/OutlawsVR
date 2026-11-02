// BUG-JUMPSPEED (user 3 Nov: icecaves' jump from 182 over the death pit 211 to 203 fell short at a low movement speed; "ease
// it"): -OLShots -OLNoFrontEnd -OLLevel=icecaves -OLJumpReachProbe[=vr] [-OLOldJumpSpeed].  At the movement speed dial's 40,
// 100 and 150 % in turn, running and then walking: a run-up along the narrowest line across the gap (182's lip to 203's, the
// same height both sides), the jump at 182's lip, the stick held on; the reach is the feet's way across from the take-off to
// where they come back down to its height (on 203, or over the pit).  New: within 5 % of the original's 900 cm running and
// 600 walking, no more than 2 % over at 150 %; running it lands on 203.  Invincible; a walk over the pit is taken back the
// moment it is measured.  -OLOldJumpSpeed: the reaches logged, the 40 % run short of the original's.  Lines "OLSHOT jumpreach
// check PASS|FAIL: ..." and "OLSHOT jumpreach RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsLevel.h"
#include "OutlawsGeometry.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "MotionControllerComponent.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsJumpReachLocal
{
	struct FCase { float Dial = 0.5f; bool bRun = true; };
	TArray<FCase> JumpCases;
	int32 CaseI = 0, Step = 0, Passes = 0, Fails = 0;
	bool bArmed = false, bVRMode = false, bOld = false, bGeo = false, bWasFalling = false;
	float CaseT = 0.f, LevelT = 0.f, AirSpeed = 0.f, OffSpeed = 0.f;
	FVector2D Lip182 = FVector2D::ZeroVector, Lip203 = FVector2D::ZeroVector, Dir = FVector2D::ZeroVector;   // Outlaws units (x, z)
	double Gap = 0.0, MeasuredReach = -1.0;
	FVector TakeOff = FVector::ZeroVector, PrevFeet = FVector::ZeroVector;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT jumpreach check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}

	FVector2D Nearest(const FVector2D& P, const FVector2D& A, const FVector2D& B)
	{
		const FVector2D D = B - A;
		const double L2 = D.SizeSquared();
		const double T = L2 > 0.0 ? FMath::Clamp(FVector2D::DotProduct(P - A, D) / L2, 0.0, 1.0) : 0.0;
		return A + D * T;
	}

	/** 182's and 203's lips over the pit 211 where they are nearest each other (as the diagnosis' gap.py: 22.07 units). */
	bool FindGap(const FOutlawsLevel& Lv)
	{
		if (!Lv.Sectors.IsValidIndex(182) || !Lv.Sectors.IsValidIndex(203)) return false;
		auto Lips = [&](int32 s)
		{
			TArray<TPair<FVector2D, FVector2D>> Out;
			const FOutlawsSector& S = Lv.Sectors[s];
			for (const FOutlawsWall& W : S.Walls) if (W.Adjoin == 211 && S.Verts.IsValidIndex(W.V1) && S.Verts.IsValidIndex(W.V2)) Out.Emplace(S.Verts[W.V1], S.Verts[W.V2]);
			return Out;
		};
		const auto A = Lips(182), B = Lips(203);
		Gap = 1e9;
		for (const auto& Sa : A)
		{
			for (int32 k = 0; k <= 50; ++k)
			{
				const FVector2D P = FMath::Lerp(Sa.Key, Sa.Value, k / 50.0);
				for (const auto& Sb : B)
				{
					const FVector2D Q = Nearest(P, Sb.Key, Sb.Value);
					const double D = FVector2D::Distance(P, Q);
					if (D < Gap) { Gap = D; Lip182 = P; Lip203 = Q; }
				}
			}
		}
		Dir = (Lip203 - Lip182).GetSafeNormal();
		return Gap < 1e8 && !Dir.IsNearlyZero();
	}
}

OL_SHOT_MATCH(OLJumpReachProbe) { return OLFlags::Has(TEXT("OLJumpReachProbe")); }

OL_SHOT_TEST(OLJumpReachProbe, 1710)
{
	using namespace OutlawsJumpReachLocal;
	if (!L || ShotTime < 0.f) return;
	if (!bArmed)
	{
		bArmed = true;
		FString Mode;
		OLFlags::Value(TEXT("OLJumpReachProbe"), Mode);
		bVRMode = Mode.Equals(TEXT("vr"), ESearchCase::IgnoreCase);
		bOld = OLFlags::Param(TEXT("OLOldJumpSpeed"));
		for (const float D : { 0.2f, 0.5f, 0.75f }) for (const bool bRun : { true, false }) JumpCases.Add({ D, bRun });
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT jumpreach start: %s, %s"), bOld ? TEXT("-OLOldJumpSpeed") : TEXT("new"), bVRMode ? TEXT("vr") : TEXT("flat"));
	}
	if (bVRMode) { bVR = true; BaseGravityScale = 1.5f; }     // (the headset's rules and its fall, this run only)
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT jumpreach RESULT %s (%d passed, %d failed, %s, %s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			bOld ? TEXT("-OLOldJumpSpeed") : TEXT("new"), bVRMode ? TEXT("vr") : TEXT("flat"));
		ShotTime = -1.f;
		FGenericPlatformMisc::RequestExit(false);
	};
	if (L->IsFrontEnd() || L->IsRebuildPending() || L->IsMoviePlaying() || bMenuOpen) return;
	L->StartSaveDelay = -1.f;           // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	L->bInvincible = true;
	const FOutlawsLevel* Lv = L->LiveLevel();
	UCharacterMovementComponent* M = GetCharacterMovement();
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	LevelT += Dt;
	if (LevelT < 3.f || !Lv) return;
	if (!bGeo)
	{
		bGeo = true;
		if (!L->LevelName.Equals(TEXT("icecaves"), ESearchCase::IgnoreCase) || !FindGap(*Lv))
		{
			Check(false, FString::Printf(TEXT("icecaves' gap 182 -> 203 over 211 found (level %s)"), *L->LevelName));
			Finish();
			return;
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT jumpreach gap: 182's lip (%.1f, %.1f) to 203's (%.1f, %.1f): %.2f units (%.0f cm)"), Lip182.X, Lip182.Y, Lip203.X, Lip203.Y, Gap, Gap * 30.0);
	}
	if (!JumpCases.IsValidIndex(CaseI)) { Finish(); return; }
	const FCase& C = JumpCases[CaseI];
	const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, Half);
	const FOutlawsSector& S182 = Lv->Sectors[182];
	const FString Id = FString::Printf(TEXT("%s at %.0f %%"), C.bRun ? TEXT("run") : TEXT("walk"), C.Dial * 200.f);
	auto StickFor = [&](const FVector& WantDir) -> FVector2D
	{
		FRotator From = !bVR ? GetControlRotation()
			: MoveDirection == EOutlawsMoveDirection::OffHand ? OffAim()->GetComponentRotation()
			: MoveDirection == EOutlawsMoveDirection::DominantHand ? DominantAim()->GetComponentRotation()
			: Camera->GetComponentRotation();
		From.Pitch = From.Roll = 0.0;
		const FVector Fwd = From.Vector(), Right = FRotator(0.0, From.Yaw + 90.0, 0.0).Vector();
		return FVector2D(FVector::DotProduct(WantDir, Right), FVector::DotProduct(WantDir, Fwd));
	};
	const FVector UeDir = (FOutlawsGeometry::ToUE(Dir.X, 0.0, Dir.Y) - FOutlawsGeometry::ToUE(0.0, 0.0, 0.0)).GetSafeNormal2D();
	if (Step == 0)
	{
		L->GameSpeed = C.Dial;
		// the run-up: back from 182's lip along the line, as far as 182 (or the floor at its height) goes, up to 20 units
		double Back = 20.0;
		for (; Back > 4.0; Back -= 1.0)
		{
			const FVector2D P = Lip182 - Dir * Back;
			const FOutlawsSector* Sp = Lv->SectorAt(FVector(P.X, S182.Floor.Y + 0.5, P.Y));
			if (Sp && FMath::Abs(Lv->SurfaceY(*Sp, P, true) - S182.Floor.Y) < 0.3) break;
		}
		const FVector2D From = Lip182 - Dir * Back;
		const FVector At = FOutlawsGeometry::ToUE(From.X, S182.Floor.Y + 0.3, From.Y) + FVector(0.0, 0.0, Half);
		TeleportTo(At, UeDir.Rotation(), false, true);
		if (Controller) Controller->SetControlRotation(UeDir.Rotation());
		M->StopMovementImmediately();
		M->SetMovementMode(MOVE_Walking);
		L->HealPlayer(100.f);
		Stamina = 100.f;
		bRunning = C.bRun;
		CaseT = 0.f;
		bWasFalling = false;
		AirSpeed = OffSpeed = 0.f;
		MeasuredReach = -1.0;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT jumpreach %s: run-up %.0f units from 182's lip"), *Id, Back);
		Step = 1;
		return;
	}
	CaseT += Dt;
	if (Step == 1)
	{
		if (CaseT < 0.3f) { M->Velocity = FVector::ZeroVector; return; }
		Step = 2;
		CaseT = 0.f;
	}
	Stamina = 100.f;
	bRunning = C.bRun;
	HeldStick = StickFor(UeDir);
	HeldStickLeft = 0.2f;
	const FVector Ol = FOutlawsGeometry::ToOutlaws(Feet);
	const double Along = FVector2D::DotProduct(FVector2D(Ol.X, Ol.Z) - Lip182, Dir);       // units past 182's lip
	if (Step == 2)
	{
		// the jump at the lip (the capsule's middle 10 cm short of it: still on 182)
		if (Along * 30.0 < -10.0 && CaseT < 8.f) return;
		if (CaseT >= 8.f) { Check(false, FString::Printf(TEXT("%s: got to the lip within 8 s"), *Id)); Step = 9; return; }
		OffSpeed = float(M->Velocity.Size2D());
		TakeOff = Feet;
		PrevFeet = Feet;
		DoJump();
		Step = 3;
		CaseT = 0.f;
		return;
	}
	if (Step == 3)
	{
		const bool bFalling = M->IsFalling();
		if (bFalling) AirSpeed = FMath::Max(AirSpeed, float(M->Velocity.Size2D()));
		// the reach: where the feet first come back down to the take-off's height (on a floor of that height, the landing)
		const bool bDown = bWasFalling && !bFalling;
		if (MeasuredReach < 0.0)
		{
			if (bDown)
			{
				MeasuredReach = FVector::Dist2D(Feet, TakeOff);
			}
			else if (bFalling && M->Velocity.Z < 0.0 && Feet.Z <= TakeOff.Z && PrevFeet.Z > TakeOff.Z)
			{
				const double T = (PrevFeet.Z - TakeOff.Z) / FMath::Max(1e-3, PrevFeet.Z - Feet.Z);
				MeasuredReach = FVector::Dist2D(FMath::Lerp(PrevFeet, Feet, T), TakeOff);
			}
		}
		bWasFalling = bWasFalling || bFalling;
		PrevFeet = Feet;
		// where it ends: down on a floor, or a metre under the take-off's height still falling (into the pit)
		const bool bFell = bFalling && Feet.Z < TakeOff.Z - 100.0;
		if (!bDown && !bFell)
		{
			if (CaseT > 4.f) { Check(false, FString::Printf(TEXT("%s: the jump came down within 4 s"), *Id)); Step = 9; }
			return;
		}
		const double Reach = MeasuredReach >= 0.0 ? MeasuredReach : FVector::Dist2D(Feet, TakeOff);
		const FOutlawsSector* Sec = Lv->SectorAt(FVector(Ol.X, Ol.Y + 0.5, Ol.Z));
		const int32 In = Sec ? Sec->Index : -1;
		const FVector OffOl = FOutlawsGeometry::ToOutlaws(TakeOff);
		const float Want = C.bRun ? 900.f : 600.f;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT jumpreach %s: off at %.0f cm/s, %.0f cm/s across in the air (%.2f s air time on the flat), reach %.0f cm (the original's %.0f), %s sector %d (off at %.1f, %.1f; now %.1f, %.1f)"),
			*Id, OffSpeed, AirSpeed, DebugJumpAirTime(), Reach, Want, bFell ? TEXT("falling in") : TEXT("down in"), In, OffOl.X, OffOl.Z, Ol.X, Ol.Z);
		if (bOld)
		{
			if (C.bRun && C.Dial < 0.3f) Check(Reach < Want * 0.95, FString::Printf(TEXT("%s (old rule): short of the original's %.0f cm (%.0f)"), *Id, Want, Reach));
			else Check(true, FString::Printf(TEXT("%s (old rule): reach %.0f cm logged"), *Id, Reach));
		}
		else
		{
			Check(FMath::Abs(Reach - Want) <= Want * 0.05, FString::Printf(TEXT("%s: reach %.0f cm within 5 %% of the original's %.0f"), *Id, Reach, Want));
			if (C.Dial > 0.7f) Check(Reach <= Want * 1.02, FString::Printf(TEXT("%s: no further than the original's %.0f cm (2 %% for the frame steps; %.0f)"), *Id, Want, Reach));
			if (C.bRun) Check(bDown && In == 203,FString::Printf(TEXT("%s: lands on 203 across the pit (sector %d)"), *Id, In));
		}
		Step = 9;
		return;
	}
	if (Step == 9)
	{
		HeldStickLeft = 0.f;
		M->StopMovementImmediately();
		++CaseI;
		Step = 0;
	}
}

#endif // WITH_OUTLAWS_TESTS
