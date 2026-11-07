// BUG-ICEJUMP's slide probe (user 2 Nov, icecaves: the ice slope 215 never made the jump over the pit 232 to 228; "if it's wrong
// here, it's off everywhere there's a slide"): -OLShots -OLNoFrontEnd -OLLevel=icecaves -OLSlideProbe[=vr] [-OLOldSlide].
// The levels in turn (started as historical missions): icecaves: (1) from the high end of 215 (331, 829), no input:
// the speed at the lip (the first fall past x 540, or x 575 crossed) at least 1700 cm/s and the landing on 228, alive and still
// up at its height 2 s on (its rim holds the body's middle over the pit: counted as 228; fall damage taken, no invincibility); (2) the same with crouch held: the lip speed within 5 % of (1).  cliff: MUDSLIDE4's middle, 1.5 s each, no
// input, the stick across the flow, the stick up against it: across moves a little (30 cm or more off the no-input line),
// up holds against the flow or gains (no more than 50 cm carried down, and 100 cm or more further up than no input).  Then
// every slide sector (sloped, friction under 1, not NOSLIP) of cliff, canyon, civlwar1, fort, mill, shfort, shoot, wharf and
// shgranry, from its high end with no input for 2.5 s, invincible: never moved more than 3 m in a tick (a rescue or an
// unstuck), never 1 m over the start, ending inside a sector at or over its floor.  -OLOldSlide: icecaves (1) only, the lip
// speed no more than 960 cm/s, as before.  Lines "OLSHOT slide check PASS|FAIL: ..." and "OLSHOT slide RESULT".

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
#include "HAL/PlatformProcess.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsSlideProbeLocal
{
	enum class EKind : uint8 { Jump, Mud, Pass };

	struct FCase
	{
		EKind Kind = EKind::Pass;
		FString Id;
		int32 Sector = -1;
		FVector2D Start = FVector2D::ZeroVector;   // (x, z), Outlaws units
		FVector Stick = FVector::ZeroVector;      // the wanted move, world (zero: no input)
		bool bCrouch = false;
		float Secs = 2.5f;
		float HitchMs = 0.f, HitchX = 0.f;       // BUG-SLIDELOAD: a sleep of this many ms in the game thread once the feet pass this x (units)
	};

	TArray<FString> Levels;
	TArray<FCase> SlideCases;
	int32 LevelI = 0, CaseI = -1, Step = 0, Passes = 0, Fails = 0;
	bool bArmed = false, bAsked = false, bVRMode = false, bOld = false, bWasFalling = false, bJumped = false, bHitchDone = false, bHitchLog = false;
	int32 HitchLeft = -1, HitchFrames = 1;
	float CaseT = 0.f, LevelT = 0.f, LandedT = -1.f, LipSpeed = -1.f, PlainLip = -1.f, MaxRise = 0.f, MaxStepCm = 0.f, HealthAtLip = 0.f;
	FVector StartAt = FVector::ZeroVector, PrevAt = FVector::ZeroVector, NoInputMove = FVector::ZeroVector, FlowDir = FVector::ZeroVector;
	FString LevelName;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT slide check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}

	/** The point a little in from the sector's highest corner (toward its middle) that lies in it, or false. */
	bool HighEnd(const FOutlawsLevel& Lv, const FOutlawsSector& S, FVector2D& Out)
	{
		if (S.Verts.Num() < 3) return false;
		FVector2D C = FVector2D::ZeroVector;
		int32 Hi = 0;
		for (int32 i = 0; i < S.Verts.Num(); ++i)
		{
			C += S.Verts[i];
			if (Lv.SurfaceY(S, S.Verts[i], true) > Lv.SurfaceY(S, S.Verts[Hi], true)) Hi = i;
		}
		C /= S.Verts.Num();
		for (const double A : { 0.2, 0.35, 0.5, 0.7, 1.0 })
		{
			const FVector2D P = FMath::Lerp(S.Verts[Hi], C, A);
			if (Lv.ContainsXZ(S, P.X, P.Y)) { Out = P; return true; }
		}
		return false;
	}

	const FOutlawsSector* Named(const FOutlawsLevel& Lv, const TCHAR* Name)
	{
		for (const FOutlawsSector& S : Lv.Sectors) if (S.Name.Equals(Name, ESearchCase::IgnoreCase)) return &S;
		return nullptr;
	}

	void BuildCases(const FString& Name, const FOutlawsLevel& Lv)
	{
		SlideCases.Reset();
		if (Name.Equals(TEXT("icecaves"), ESearchCase::IgnoreCase))
		{
			FCase C;
			C.Kind = EKind::Jump;
			C.Sector = 215;
			C.Start = FVector2D(331.0, 829.0);     // (the strip's high end, 215 runs east x 326 -> 555 at z 783..833)
			C.Secs = 20.f;
			// BUG-SLIDELOAD: -OLSlideHitch=ms@x+ms@x: each pair is a case pair (no input, crouch held) with a sleep of ms on the
			// frame(s) the feet pass x (OLSlideHitchFrames, default 1), the stall a busy machine or the Quest gives
			FString Hitches, FramesStr;
			TArray<FString> Pairs;
			if (OLFlags::Value(TEXT("OLSlideHitch"), Hitches)) Hitches.ParseIntoArray(Pairs, TEXT("+"));
			if (Pairs.Num() == 0) Pairs.Add(TEXT("0@0"));
			if (OLFlags::Value(TEXT("OLSlideHitchFrames"), FramesStr)) HitchFrames = FMath::Max(1, FCString::Atoi(*FramesStr));
			for (const FString& Pair : Pairs)
			{
				FString Ms, X;
				if (!Pair.Split(TEXT("@"), &Ms, &X)) { Ms = Pair; X = TEXT("0"); }
				C.HitchMs = FCString::Atof(*Ms);
				C.HitchX = FCString::Atof(*X);
				const FString Tag = C.HitchMs > 0.f ? FString::Printf(TEXT(" hitch %.0f ms at x %.0f"), C.HitchMs, C.HitchX) : FString();
				C.bCrouch = false;
				C.Id = TEXT("ice 215 no input") + Tag;
				SlideCases.Add(C);
				if (!bOld)
				{
					C.bCrouch = true;
					C.Id = TEXT("ice 215 crouch held") + Tag;
					SlideCases.Add(C);
				}
			}
			return;
		}
		if (Name.Equals(TEXT("cliff"), ESearchCase::IgnoreCase))
		{
			if (const FOutlawsSector* M = Named(Lv, TEXT("MUDSLIDE4")))
			{
				FVector2D Mid = FVector2D::ZeroVector;
				for (const FVector2D& V : M->Verts) Mid += V;
				Mid /= FMath::Max(1, M->Verts.Num());
				FlowDir = FOutlawsGeometry::ToUE(M->Velocity.X, 0.0, M->Velocity.Z).GetSafeNormal2D();
				const FVector Across(-FlowDir.Y, FlowDir.X, 0.0);
				for (int32 k = 0; k < 3; ++k)
				{
					FCase C;
					C.Kind = EKind::Mud;
					C.Sector = M->Index;
					C.Start = Mid;
					C.Secs = 1.5f;
					C.Stick = k == 0 ? FVector::ZeroVector : k == 1 ? Across : -FlowDir;
					C.Id = k == 0 ? TEXT("MUDSLIDE4 no input") : k == 1 ? TEXT("MUDSLIDE4 across the flow") : TEXT("MUDSLIDE4 up against the flow");
					SlideCases.Add(C);
				}
			}
			else
			{
				Check(false, TEXT("cliff: no sector MUDSLIDE4"));
			}
		}
		for (const FOutlawsSector& S : Lv.Sectors)
		{
			if (!(S.Flags & OutlawsFlags::SlopedFloor) || (S.Flags & OutlawsFlags::NoSlip) || S.Friction >= 0.999f) continue;
			FCase C;
			C.Kind = EKind::Pass;
			C.Sector = S.Index;
			if (!HighEnd(Lv, S, C.Start))
			{
				Check(false, FString::Printf(TEXT("%s slide sector %d '%s': no spot in it"), *Name, S.Index, *S.Name));
				continue;
			}
			C.Id = FString::Printf(TEXT("%s slide sector %d '%s' (friction %.2f)"), *Name, S.Index, *S.Name, S.Friction);
			SlideCases.Add(C);
		}
	}
}

OL_SHOT_MATCH(OLSlideProbe) { return OLFlags::Has(TEXT("OLSlideProbe")); }

OL_SHOT_TEST(OLSlideProbe, 1540)
{
	using namespace OutlawsSlideProbeLocal;
	if (!L || ShotTime < 0.f) return;
	if (!bArmed)
	{
		bArmed = true;
		FString Mode;
		OLFlags::Value(TEXT("OLSlideProbe"), Mode);
		bVRMode = Mode.Equals(TEXT("vr"), ESearchCase::IgnoreCase);
		bOld = OLFlags::Param(TEXT("OLOldSlide"));
		FString List = bOld ? TEXT("icecaves") : TEXT("icecaves+cliff+canyon+civlwar1+fort+mill+shfort+shoot+wharf+shgranry");
		OLFlags::Value(TEXT("OLSlideProbeLevels"), List);      // (-OLSlideProbeLevels=a+b: those only)
		List.ParseIntoArray(Levels, TEXT("+"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT slide start: %s, %s, levels %s"), bOld ? TEXT("-OLOldSlide") : TEXT("new"), bVRMode ? TEXT("vr") : TEXT("flat"), *List);
	}
	if (bVRMode) { bVR = true; BaseGravityScale = 1.5f; }     // (the headset's rules and its fall, this run only)
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT slide RESULT %s (%d passed, %d failed, %s, %s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			bOld ? TEXT("-OLOldSlide") : TEXT("new"), bVRMode ? TEXT("vr") : TEXT("flat"));
		ShotTime = -1.f;
		FGenericPlatformMisc::RequestExit(false);
	};
	if (!Levels.IsValidIndex(LevelI)) { Finish(); return; }
	if (L->IsRebuildPending() || L->IsMoviePlaying() || bMenuOpen) return;
	const FString Want = Levels[LevelI];
	if (L->IsFrontEnd() || !L->LevelName.Equals(Want, ESearchCase::IgnoreCase))
	{
		if (!bAsked)
		{
			bAsked = true;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT slide starting %s"), *Want);
			L->StartMission(Want, EOutlawsDifficulty::Bad);
		}
		return;
	}
	L->StartSaveDelay = -1.f;           // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	const FOutlawsLevel* Lv = L->LiveLevel();
	UCharacterMovementComponent* M = GetCharacterMovement();
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	if (LevelName != L->LevelName)
	{
		LevelName = L->LevelName;
		LevelT = 0.f;
		CaseI = -1;
		Step = 0;
		if (Lv) BuildCases(LevelName.ToLower(), *Lv); else SlideCases.Reset();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT slide %s: %d cases"), *LevelName, SlideCases.Num());
	}
	LevelT += Dt;
	if (LevelT < 3.f || !Lv) return;     // (the level settled, the fade in done)
	auto NextCase = [&]() { ++CaseI; Step = 0; };
	if (CaseI < 0) CaseI = 0;
	if (!SlideCases.IsValidIndex(CaseI))
	{
		++LevelI;
		bAsked = false;
		HeldStickLeft = 0.f;
		bCrouchHeld = false;
		return;
	}
	const FCase& C = SlideCases[CaseI];
	const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, Half);
	auto SectorOfFeet = [&]() { return Lv->SectorAt(FOutlawsGeometry::ToOutlaws(Feet, 0.2)); };
	// the stick that moves the body along the wanted world direction, as Locomotion reads the stick
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
	if (Step == 0)
	{
		const FOutlawsSector* S = Lv->Sectors.IsValidIndex(C.Sector) ? &Lv->Sectors[C.Sector] : nullptr;
		if (!S || !Lv->ContainsXZ(*S, C.Start.X, C.Start.Y))
		{
			Check(false, FString::Printf(TEXT("%s: the start (%.1f, %.1f) is not in sector %d"), *C.Id, C.Start.X, C.Start.Y, C.Sector));
			NextCase();
			return;
		}
		const FVector At = FOutlawsGeometry::ToUE(C.Start.X, Lv->SurfaceY(*S, C.Start, true) + 0.3, C.Start.Y) + FVector(0.0, 0.0, Half);
		bCrouchHeld = false;
		TeleportTo(At, GetActorRotation(), false, true);
		M->StopMovementImmediately();
		M->SetMovementMode(MOVE_Walking);
		L->HealPlayer(100.f);
		StartAt = PrevAt = At;
		CaseT = 0.f;
		LipSpeed = LandedT = -1.f;
		MaxRise = MaxStepCm = 0.f;
		bWasFalling = bJumped = false;
		bHitchDone = bHitchLog = false;
		HitchLeft = -1;
		Step = 1;
		return;
	}
	CaseT += Dt;
	// the jump's landing and fall damage are real; the rest is invincible (a slide may run on into a deadly floor)
	L->bInvincible = C.Kind != EKind::Jump;
	if (Step == 1)
	{
		// (a moment stood still before the slide's input or crouch: the teleport settled)
		if (CaseT < 0.3f) { M->Velocity = FVector::ZeroVector; return; }
		Step = 2;
		CaseT = 0.f;
		StartAt = PrevAt = GetActorLocation();
		if (C.bCrouch) bCrouchHeld = true;
	}
	if (CaseT >= 0.5f && CaseT - Dt < 0.5f)
	{
		// (the floor as Locomotion reads it, half a second in)
		float Fr = 1.f;
		bool bSl = false;
		FVector Dn = FVector::ZeroVector;
		const bool bFloor = L->FloorUnder(Feet, Fr, bSl, Dn);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT slide %s at 0.5 s: mode %d, swimming %d, speed %.0f cm/s, floor %d friction %.2f slides %d gradient %.3f, braking %.0f, ground friction %.1f"), *C.Id,
			int32(M->MovementMode.GetValue()), bSwimming ? 1 : 0, M->Velocity.Size(), bFloor ? 1 : 0, Fr, bSl ? 1 : 0, Dn.Size(), M->BrakingDecelerationWalking, M->GroundFriction);
	}
	if (!C.Stick.IsNearlyZero())
	{
		HeldStick = StickFor(C.Stick);
		HeldStickLeft = 0.2f;
	}
	else
	{
		HeldStickLeft = 0.f;
	}
	const FVector Here = GetActorLocation();
	MaxStepCm = FMath::Max(MaxStepCm, float(FVector::Dist(Here, PrevAt)));
	MaxRise = FMath::Max(MaxRise, float(Here.Z - StartAt.Z));
	PrevAt = Here;
	if (C.Kind == EKind::Jump)
	{
		const bool bFalling = M->IsFalling();
		const FVector Ol = FOutlawsGeometry::ToOutlaws(Feet);
		if (bHitchLog)
		{
			bHitchLog = false;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT slide hitch: the frame after it ran Dt %.0f ms, now at (%.1f, %.1f, %.1f) units, %.0f cm/s"), Dt * 1000.f, Ol.X, Ol.Y, Ol.Z, M->Velocity.Size2D());
		}
		if (C.HitchMs > 0.f && !bHitchDone && Ol.X >= C.HitchX)
		{
			if (HitchLeft < 0) HitchLeft = HitchFrames;
			FPlatformProcess::Sleep(C.HitchMs / 1000.f);
			bHitchLog = true;
			if (--HitchLeft <= 0) bHitchDone = true;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT slide hitch: slept %.0f ms at (%.1f, %.1f, %.1f) units, %.0f cm/s, %.2f s"), C.HitchMs, Ol.X, Ol.Y, Ol.Z, M->Velocity.Size2D(), CaseT);
		}
		if (LipSpeed < 0.f && ((bFalling && !bWasFalling && Ol.X > 540.0) || Ol.X > 575.0))
		{
			LipSpeed = float(M->Velocity.Size2D());
			HealthAtLip = L->GetHealth();
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT slide %s: off the lip at (%.1f, %.1f, %.1f) units, %.0f cm/s, %.1f s, crouched %d (half %.0f)"), *C.Id, Ol.X, Ol.Y, Ol.Z, LipSpeed, CaseT,
				bIsCrouched || Half < 60.f ? 1 : 0, Half);
		}
		if (bFalling) bJumped = bJumped || LipSpeed >= 0.f;
		bWasFalling = bFalling;
		const bool bLanded = bJumped && !bFalling;
		// (judged 2 s after the landing: on 228 it stays up there, at its height; off it, it slides back into the pit)
		if (bLanded && LandedT < 0.f)
		{
			LandedT = CaseT;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT slide %s: down at (%.1f, %.1f, %.1f) units after %.1f s, health %.1f"), *C.Id, Ol.X, Ol.Y, Ol.Z, CaseT, L->GetHealth());
		}
		if (!(LandedT >= 0.f && CaseT - LandedT > 2.f) && !bDeadView && CaseT < C.Secs) return;
		const FOutlawsSector* S = SectorOfFeet();
		// (the far side's rim holds the body with its middle over the pit: the floor it stands on is 228's height, -84 to -96)
		const int32 Landed = S && S->Index == 232 && Ol.Y > -120.0 && Lv->Sectors.IsValidIndex(228) ? 228 : S ? S->Index : -1;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT slide %s: %s in sector %d after %.1f s at (%.1f, %.1f, %.1f) units, mode %d, health %.1f (%.1f at the lip), dead %d"), *C.Id, bLanded ? TEXT("landed") : TEXT("ended"),
			Landed, CaseT, Ol.X, Ol.Y, Ol.Z, int32(M->MovementMode.GetValue()), L->GetHealth(), HealthAtLip, bDeadView ? 1 : 0);
		if (bOld)
		{
			Check(LipSpeed <= 960.f * 1.02f, FString::Printf(TEXT("%s (old rule): off the lip at %.0f cm/s (-1: never got there; want 960 or less, as before)"), *C.Id, LipSpeed));
			Finish();
			return;
		}
		Check(LipSpeed >= 1700.f, FString::Printf(TEXT("%s: off the lip at %.0f cm/s (want 1700 or more)"), *C.Id, LipSpeed));
		// (the desktop's fall, at 4.59 g, comes off the lip at 3300 cm/s and slides on over the ice of 228 into 207 behind it)
		Check(bLanded && (Landed == 228 || Landed == 207) && !bDeadView, FString::Printf(TEXT("%s: lands on the far side alive, 228 (or 207 behind it) (sector %d, dead %d, health %.1f)"), *C.Id, Landed,
			bDeadView ? 1 : 0, L->GetHealth()));
		if (!C.bCrouch) PlainLip = LipSpeed;
		else Check(PlainLip > 0.f && FMath::Abs(LipSpeed - PlainLip) <= 0.05f * PlainLip, FString::Printf(TEXT("%s: the lip speed %.0f within 5 %% of no input's %.0f"), *C.Id, LipSpeed, PlainLip));
		bCrouchHeld = false;
		if (bDeadView) { Finish(); return; }     // (a death here ends the run: the rest would start from the respawn)
		NextCase();
		return;
	}
	if (CaseT < C.Secs) return;
	const FVector Move = Here - StartAt;
	if (C.Kind == EKind::Mud)
	{
		const FVector Across(-FlowDir.Y, FlowDir.X, 0.0);
		const double Up = FVector::DotProduct(Move, -FlowDir), Side = FMath::Abs(FVector::DotProduct(Move - NoInputMove, Across));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT slide %s: moved %s in %.1f s (up the flow %.0f cm, across it %.0f cm off no input's)"), *C.Id, *Move.ToCompactString(), CaseT, Up, Side);
		if (C.Stick.IsNearlyZero())
		{
			NoInputMove = Move;
		}
		else if (FVector::DotProduct(C.Stick, FlowDir) < -0.5)
		{
			const double NoUp = FVector::DotProduct(NoInputMove, -FlowDir);
			Check(Up >= -50.0 && Up - NoUp >= 100.0, FString::Printf(TEXT("%s: holds or gains (%.0f cm up the flow; no input %.0f)"), *C.Id, Up, NoUp));
		}
		else
		{
			Check(Side >= 30.0, FString::Printf(TEXT("%s: moves a little across (%.0f cm off no input's line)"), *C.Id, Side));
		}
		NextCase();
		return;
	}
	const FOutlawsSector* S = SectorOfFeet();
	const double FloorCm = S ? Lv->SurfaceY(*S, FOutlawsGeometry::ToOutlawsPlan(Feet), true) * FOutlawsGeometry::UnitsToCm : 0.0;
	const bool bOk = S && Feet.Z >= FloorCm - 30.0 && MaxStepCm <= 300.f && MaxRise <= 100.f;
	Check(bOk, FString::Printf(TEXT("%s: %.0f cm in %.1f s, now in sector %d %.0f cm over its floor, the biggest tick %.0f cm, rose %.0f cm"), *C.Id, Move.Size2D(), CaseT,
		S ? S->Index : -1, S ? Feet.Z - FloorCm : 0.0, MaxStepCm, MaxRise));
	NextCase();
}

#endif // WITH_OUTLAWS_TESTS
