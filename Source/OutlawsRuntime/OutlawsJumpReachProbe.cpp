// BUG-JUMPSPEED (user 3 Nov: icecaves' jump from 182 over the death pit 211 to 203 fell short at a low movement speed; "ease
// it"): -OLShots -OLNoFrontEnd -OLLevel=icecaves -OLJumpReachProbe[=vr] [-OLOldJumpSpeed].  At the movement speed dial's 40,
// 100 and 150 % in turn, running and then walking: a run-up along the narrowest line across the gap (182's lip to 203's, the
// same height both sides), the jump at 182's lip, the stick held on; the reach is the feet's way across from the take-off to
// where they come back down to its height (on 203, or over the pit).  Invincible; a walk over the pit is taken back the
// moment it is measured.  -OLOldJumpSpeed: the reaches logged, the 40 % run short of the original's.  Lines "OLSHOT jumpreach
// check PASS|FAIL: ..." and "OLSHOT jumpreach RESULT".
// BUG-JUMPHYBRID (user 3 Nov: the big gap a little further on, the ice slide 215's jump over the pit 232 to 228, no longer
// cleared at 40 %): at each dial also that jump off the slide, from 215's high end (331, 829) running with the stick held to
// the lip (78, 104 and 109's walls to 232), the jump at the lip and SlideLeads seconds before it (-OLJumpReachLeads=a+b: those);
// the far rim is 228's and 207's walls to 232 along the take-off's line, the margin the way across where the feet come down to
// the rim's height less the rim's distance, and the end on the far side (228, 207, or 232's edge at their height) or into the
// pit.  New (the stretch and a share of the boost): a jump no further than the original's, running it lands on 203 MinMargin
// over its lip, the slide jump at the lip across past the far rim at every setting and the early one at 40 % (the early one over
// it logged); -OLOldJumpSpeed logs the slide jumps.  -OLJumpReachDials=0.2+0.5:
// those dials only; -OLJumpReachShares=0+0.5: the boost share's sweep, the run over 211 at each (logged), nothing else.

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
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsJumpReachLocal
{
	using FSegs = TArray<TPair<FVector2D, FVector2D>>;
	struct FCase { float Dial = 0.5f; bool bRun = true; bool bSlide = false; float Lead = 0.f; float Share = -1.f; };
	TArray<FCase> JumpCases;
	int32 CaseI = 0, Step = 0, Passes = 0, Fails = 0;
	bool bArmed = false, bVRMode = false, bOld = false, bGeo = false, bWasFalling = false;
	float CaseT = 0.f, LevelT = 0.f, AirSpeed = 0.f, OffSpeed = 0.f, DownT = -1.f, ReachT = -1.f;
	FVector2D Lip182 = FVector2D::ZeroVector, Lip203 = FVector2D::ZeroVector, Dir = FVector2D::ZeroVector;   // Outlaws units (x, z)
	double Gap = 0.0, MeasuredReach = -1.0, OffAlong = 0.0;
	FVector TakeOff = FVector::ZeroVector, PrevFeet = FVector::ZeroVector;
	// (BUG-JUMPHYBRID) the big gap: the slide's lip and the far rim (Outlaws units), the take-off's line across, the rim on it
	const FVector2D SlideStart(331.0, 829.0);     // (215's high end, as the slide probe's)
	FSegs SlideLip, FarRim;
	FVector2D LipMid = FVector2D::ZeroVector, OffDir = FVector2D::ZeroVector, RimAt = FVector2D::ZeroVector;
	double RimDist = -1.0, RimY = 0.0, AcrossAtRim = -1.0, OffEarly = 0.0;
	int32 DownIn = -1;
	float OwnShare = -1.f;        // the player's boost share before the sweep's
	constexpr double MinMargin = 90.0;     // cm (3 units) over 203's lip a running jump lands with (the original's: 228)

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

	/** The walls of sector s that open onto sector Into. */
	void WallsTo(const FOutlawsLevel& Lv, int32 s, int32 Into, FSegs& Out)
	{
		if (!Lv.Sectors.IsValidIndex(s)) return;
		const FOutlawsSector& S = Lv.Sectors[s];
		for (const FOutlawsWall& W : S.Walls) if (W.Adjoin == Into && S.Verts.IsValidIndex(W.V1) && S.Verts.IsValidIndex(W.V2)) Out.Emplace(S.Verts[W.V1], S.Verts[W.V2]);
	}

	/** (BUG-JUMPHYBRID) the slide 215's lip into the pit 232 (78, 104 and 109's walls to it) and the far rim (228's and 207's). */
	bool FindSlide(const FOutlawsLevel& Lv)
	{
		SlideLip.Reset();
		FarRim.Reset();
		for (const int32 s : { 78, 104, 109 }) WallsTo(Lv, s, 232, SlideLip);
		for (const int32 s : { 228, 207 }) WallsTo(Lv, s, 232, FarRim);
		// (steered to the middle of 78's edge, where the slide comes off with no input: (574, 803))
		LipMid = SlideLip.Num() ? (SlideLip[0].Key + SlideLip[0].Value) * 0.5 : FVector2D::ZeroVector;
		return SlideLip.Num() > 0 && FarRim.Num() > 0 && Lv.Sectors.IsValidIndex(215) && Lv.ContainsXZ(Lv.Sectors[215], SlideStart.X, SlideStart.Y);
	}

	/** How far along the ray from P (unit direction D) it first meets one of the walls (units; -1: none), and where. */
	double RayHit(const FVector2D& P, const FVector2D& D, const FSegs& Segs, FVector2D* At = nullptr)
	{
		double Best = -1.0;
		for (const auto& S : Segs)
		{
			const FVector2D E = S.Value - S.Key, W = S.Key - P;
			const double Den = D.X * E.Y - D.Y * E.X;
			if (FMath::Abs(Den) < 1e-9) continue;
			const double T = (W.X * E.Y - W.Y * E.X) / Den;      // along the ray
			const double U = (W.X * D.Y - W.Y * D.X) / Den;      // along the wall
			if (T >= 0.0 && U >= 0.0 && U <= 1.0 && (Best < 0.0 || T < Best)) { Best = T; if (At) *At = P + D * T; }
		}
		return Best;
	}

	/** How far P is from the nearest of the walls (units). */
	double NearestTo(const FVector2D& P, const FSegs& Segs)
	{
		double Best = 1e9;
		for (const auto& S : Segs) Best = FMath::Min(Best, FVector2D::Distance(P, Nearest(P, S.Key, S.Value)));
		return Best;
	}

	/** A list "a+b+c" of numbers from the flag, or Def without it. */
	TArray<float> Numbers(const TCHAR* Flag, const TArray<float>& Def)
	{
		FString V;
		if (!OLFlags::Value(Flag, V) || V.IsEmpty()) return Def;
		TArray<FString> Parts;
		V.ParseIntoArray(Parts, TEXT("+"));
		TArray<float> Out;
		for (const FString& P : Parts) Out.Add(FCString::Atof(*P));
		return Out.Num() ? Out : Def;
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
		const TArray<float> SlideLeads = Numbers(TEXT("OLJumpReachLeads"), { 0.f, 0.5f });
		const TArray<float> Shares = Numbers(TEXT("OLJumpReachShares"), {});
		for (const float D : Numbers(TEXT("OLJumpReachDials"), { 0.2f, 0.5f, 0.75f }))
		{
			// (BUG-JUMPHYBRID) -OLJumpReachShares=a+b: the boost share's sweep, the run over 211 at each share, nothing else
			for (const float Sh : Shares) JumpCases.Add({ D, true, false, 0.f, Sh });
			if (Shares.Num()) continue;
			for (const bool bRun : { true, false }) JumpCases.Add({ D, bRun });
			for (const float Lead : SlideLeads) JumpCases.Add({ D, true, true, Lead });      // (BUG-JUMPHYBRID) off the slide
		}
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
		if (!FindSlide(*Lv))
		{
			Check(false, TEXT("icecaves' slide 215, its lip into 232 and the far rim (228, 207) found"));
			Finish();
			return;
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT jumpreach slide: 215 from (%.1f, %.1f), its lip into 232 %d walls about (%.1f, %.1f), the far rim %d walls"), SlideStart.X, SlideStart.Y,
			SlideLip.Num(), LipMid.X, LipMid.Y, FarRim.Num());
	}
	if (!JumpCases.IsValidIndex(CaseI)) { Finish(); return; }
	const FCase& C = JumpCases[CaseI];
	const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, Half);
	const FOutlawsSector& S182 = Lv->Sectors[182];
	const FString Id = FString::Printf(TEXT("%s at %.0f %%%s"), C.bRun ? TEXT("run") : TEXT("walk"), C.Dial * 200.f, C.Share >= 0.f ? *FString::Printf(TEXT(", share %.2f"), C.Share) : TEXT(""));
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
		// (REQ-MPSPEED: -OLJumpReachHeld: each dial the host's, held over a player's own 150 %: the same reaches)
		if (OLFlags::Param(TEXT("OLJumpReachHeld"))) { L->GameSpeed = AOutlawsLevelActor::MaxGameSpeed; L->HeldGameSpeed = C.Dial; }
		else L->GameSpeed = C.Dial;
		if (OwnShare < 0.f) OwnShare = JumpBoostShareNow();       // (the player's own: the default, or -OLJumpBoostShare)
		JumpBoostShare = C.Share >= 0.f ? C.Share : OwnShare;
		if (C.bSlide)
		{
			// (BUG-JUMPHYBRID) the slide from 215's high end, facing the lip, running
			const FVector At = FOutlawsGeometry::ToUE(SlideStart.X, Lv->SurfaceY(Lv->Sectors[215], SlideStart, true) + 0.3, SlideStart.Y) + FVector(0.0, 0.0, Half);
			const FRotator Face = (FOutlawsGeometry::ToUE(LipMid.X, 0.0, LipMid.Y) - FOutlawsGeometry::ToUE(SlideStart.X, 0.0, SlideStart.Y)).GetSafeNormal2D().Rotation();
			TeleportTo(At, Face, false, true);
			if (Controller) Controller->SetControlRotation(Face);
			M->StopMovementImmediately();
			M->SetMovementMode(MOVE_Walking);
			L->HealPlayer(100.f);
			Stamina = 100.f;
			bRunning = C.bRun;
			CaseT = 0.f;
			bWasFalling = false;
			AirSpeed = OffSpeed = 0.f;
			Step = 1;
			return;
		}
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
		ReachT = -1.f;
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
	if (C.bSlide && Step < 9)
	{
		// (BUG-JUMPHYBRID) the big gap off the slide: the stick held to the lip's middle, the jump at the lip (Lead 0) or Lead
		// seconds before it at the speed then; in the air the stick held on along the take-off's line
		const FVector Ol = FOutlawsGeometry::ToOutlaws(Feet);
		const FVector2D P(Ol.X, Ol.Z);
		const FString Sid = FString::Printf(TEXT("slide, jump %s, at %.0f %%"), C.Lead > 0.f ? *FString::Printf(TEXT("%.2f s early"), C.Lead) : TEXT("at the lip"), C.Dial * 200.f);
		const bool bFalling = M->IsFalling();
		auto UeOf = [](const FVector2D& D) { return (FOutlawsGeometry::ToUE(D.X, 0.0, D.Y) - FOutlawsGeometry::ToUE(0.0, 0.0, 0.0)).GetSafeNormal2D(); };
		if (Step == 2)
		{
			HeldStick = StickFor(UeOf(LipMid - P));
			HeldStickLeft = 0.2f;
			const FVector Vu(M->Velocity.X, M->Velocity.Y, 0.0);
			const double Speed = Vu.Size();
			const FVector VOl = FOutlawsGeometry::ToOutlaws(GetActorLocation() + Vu) - FOutlawsGeometry::ToOutlaws(GetActorLocation());
			const FVector2D VDir = FVector2D(VOl.X, VOl.Z).GetSafeNormal();
			const double Ray = VDir.IsNearlyZero() ? -1.0 : RayHit(P, VDir, SlideLip);
			const double ToLip = (Ray >= 0.0 ? Ray : NearestTo(P, SlideLip)) * 30.0;      // cm on to the lip (or to its nearest point)
			if (bFalling || CaseT >= 20.f)
			{
				Check(false, FString::Printf(TEXT("%s: on the slide at the jump (%s after %.1f s at (%.1f, %.1f, %.1f))"), *Sid, bFalling ? TEXT("off it first") : TEXT("not at the lip"), CaseT, Ol.X, Ol.Y, Ol.Z));
				Step = 9;
				return;
			}
			// (two frames ahead at the lip: the jump itself is the next movement tick's)
			if (Speed < 100.0 || ToLip > Speed * (C.Lead + 2.f * Dt) + 5.0) return;
			OffSpeed = float(Speed);
			OffEarly = ToLip;
			TakeOff = PrevFeet = Feet;
			OffDir = VDir;
			RimDist = RayHit(P, OffDir, FarRim, &RimAt);
			if (RimDist < 0.0)
			{
				// (the line misses the rim: its nearest point, the way along the line to it)
				double Best = 1e9;
				for (const auto& W : FarRim)
				{
					const FVector2D Q = Nearest(P, W.Key, W.Value);
					if (FVector2D::Distance(P, Q) < Best) { Best = FVector2D::Distance(P, Q); RimAt = Q; }
				}
				RimDist = FVector2D::DotProduct(RimAt - P, OffDir);
			}
			RimY = Lv->SurfaceY(Lv->Sectors[228], RimAt, true);
			AcrossAtRim = -1.0;
			DownT = -1.f;
			DownIn = -1;
			AirSpeed = 0.f;
			bWasFalling = false;
			DoJump();
			Step = 3;
			CaseT = 0.f;
			return;
		}
		HeldStick = StickFor(UeOf(OffDir));
		HeldStickLeft = 0.2f;
		const FVector OffOl = FOutlawsGeometry::ToOutlaws(TakeOff), PrevOl = FOutlawsGeometry::ToOutlaws(PrevFeet);
		const FVector2D Off2(OffOl.X, OffOl.Z);
		if (bFalling) AirSpeed = FMath::Max(AirSpeed, float(M->Velocity.Size2D()));
		// the way across, along the take-off's line, where the feet come down through the rim's height (or touch down above it)
		if (AcrossAtRim < 0.0 && M->Velocity.Z < 0.0 && PrevOl.Y > RimY && Ol.Y <= RimY)
		{
			const double T = (PrevOl.Y - RimY) / FMath::Max(1e-6, PrevOl.Y - Ol.Y);
			AcrossAtRim = FVector2D::DotProduct(FMath::Lerp(FVector2D(PrevOl.X, PrevOl.Z), P, T) - Off2, OffDir);
		}
		PrevFeet = Feet;
		if (bWasFalling && !bFalling && DownT < 0.f)
		{
			DownT = CaseT;
			const FOutlawsSector* Sd = Lv->SectorAt(FVector(Ol.X, Ol.Y + 0.5, Ol.Z));
			DownIn = Sd ? Sd->Index : -1;
			if (AcrossAtRim < 0.0) AcrossAtRim = FVector2D::DotProduct(P - Off2, OffDir);
		}
		bWasFalling = bWasFalling || bFalling;
		// the end: into the pit (the far side is at -84 .. -96 units, 232's floor -500), or a second after the touchdown
		const bool bFellIn = Ol.Y < -130.0;
		if (!bFellIn && !(DownT >= 0.f && CaseT - DownT > 1.f) && CaseT < 8.f) return;
		const FOutlawsSector* Se = Lv->SectorAt(FVector(Ol.X, Ol.Y + 0.5, Ol.Z));
		const int32 EndIn = Se ? Se->Index : -1;
		// (the far side's rim holds the body with its middle over the pit: 232 at the rim's height counts as across)
		const bool bAcross = !bFellIn && DownT >= 0.f && (EndIn == 228 || EndIn == 207 || (EndIn == 232 && Ol.Y > -120.0));
		const double Margin = AcrossAtRim >= 0.0 && RimDist >= 0.0 ? (AcrossAtRim - RimDist) * 30.0 : -9999.0;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT jumpreach %s: off at %.0f cm/s %.0f cm before the lip, %.0f cm/s across in the air, the far rim %.0f cm on and %.0f cm down, %.0f cm across at its height: margin %.0f cm, %s (sector %d, down in %d after %.2f s; off at %.1f, %.1f, %.1f; now %.1f, %.1f, %.1f)"),
			*Sid, OffSpeed, OffEarly, AirSpeed, RimDist * 30.0, (OffOl.Y - RimY) * 30.0, AcrossAtRim * 30.0, Margin, bAcross ? TEXT("ACROSS") : TEXT("INTO THE PIT"), EndIn, DownIn, DownT,
			OffOl.X, OffOl.Y, OffOl.Z, Ol.X, Ol.Y, Ol.Z);
		if (bOld) Check(true, FString::Printf(TEXT("%s (old rule): %s, margin %.0f cm (logged)"), *Sid, bAcross ? TEXT("across") : TEXT("into the pit"), Margin));
		else if (C.Lead <= 0.f || C.Dial < 0.3f) Check(bAcross && Margin > 0.0, FString::Printf(TEXT("%s: across onto 228 (or 207) past its rim (margin %.0f cm, sector %d)"), *Sid, Margin, EndIn));
		else Check(true, FString::Printf(TEXT("%s: %s, margin %.0f cm (an early jump over 100 %%: logged)"), *Sid, bAcross ? TEXT("across") : TEXT("into the pit"), Margin));
		Step = 9;
		return;
	}
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
		OffAlong = Along;
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
				ReachT = CaseT;
			}
			else if (bFalling && M->Velocity.Z < 0.0 && Feet.Z <= TakeOff.Z && PrevFeet.Z > TakeOff.Z)
			{
				const double T = (PrevFeet.Z - TakeOff.Z) / FMath::Max(1e-3, PrevFeet.Z - Feet.Z);
				MeasuredReach = FVector::Dist2D(FMath::Lerp(PrevFeet, Feet, T), TakeOff);
				ReachT = CaseT - float(1.0 - T) * Dt;
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
		// (BUG-JUMPHYBRID) the margin: the reach less the way from the take-off to 203's lip
		const double Margin = Reach - (Gap - OffAlong) * 30.0;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT jumpreach %s: off at %.0f cm/s, %.0f cm/s across in the air (%.2f s up and back down to its height), reach %.0f cm (the original's %.0f), margin %.0f cm over 203's lip, %s sector %d (off at %.1f, %.1f; now %.1f, %.1f)"),
			*Id, OffSpeed, AirSpeed, ReachT, Reach, Want, Margin, bFell ? TEXT("falling in") : TEXT("down in"), In, OffOl.X, OffOl.Z, Ol.X, Ol.Z);
		if (bOld)
		{
			if (C.bRun && C.Dial < 0.3f) Check(Reach < Want * 0.95, FString::Printf(TEXT("%s (old rule): short of the original's %.0f cm (%.0f)"), *Id, Want, Reach));
			else Check(true, FString::Printf(TEXT("%s (old rule): reach %.0f cm logged"), *Id, Reach));
		}
		else if (C.Share >= 0.f)
		{
			Check(true, FString::Printf(TEXT("%s: reach %.0f cm, margin %.0f cm, %s (the sweep, logged)"), *Id, Reach, Margin, bDown && In == 203 ? TEXT("on 203") : TEXT("not on 203")));
		}
		else
		{
			// (BUG-JUMPHYBRID) the stretch and a share of the boost: never further than the original's, and running onto 203
			// with MinMargin to spare
			Check(Reach <= Want * 1.02, FString::Printf(TEXT("%s: no further than the original's %.0f cm (2 %% for the frame steps; %.0f)"), *Id, Want, Reach));
			if (C.bRun) Check(bDown && In == 203 && Margin >= MinMargin, FString::Printf(TEXT("%s: lands on 203 across the pit, %.0f cm or more over its lip (sector %d, %.0f)"), *Id, MinMargin, In, Margin));
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

// BUG-JUMPHYBRID's shot pair at the big gap: -OLShots -OLNoFrontEnd -OLLevel=icecaves -OLJumpGapShot[=vr] [-OLOldJumpSpeed].
// At the dial's 40 %, the slide from 215's high end, running with the stick held to the lip, the jump GapShotLead seconds
// before it (the probe's early case), the view along the take-off's line a little down; shots 0.8 s after the take-off
// (jumpgap_air[_vr][_old].png) and the end, a second after the touchdown or once the feet are 10 units under the far rim's
// height (jumpgap_end[_vr][_old].png), in Saved\OLShots.  Invincible.  Lines "OLSHOT jumpgap ..." and "OLSHOT jumpgap RESULT".
namespace OutlawsJumpGapLocal
{
	constexpr float GapShotLead = 0.5f;
	int32 Step = 0;
	bool bArmed = false, bVRMode = false, bOld = false, bShotAir = false, bWasFalling = false, bAcross = false;
	float LevelT = 0.f, CaseT = 0.f, DownT = -1.f, EndT = -1.f;
	double RimY = 0.0;
	FVector2D GapDir = FVector2D::ZeroVector;
	FString Tail;
}

OL_SHOT_MATCH(OLJumpGapShot) { return OLFlags::Has(TEXT("OLJumpGapShot")); }

OL_SHOT_TEST(OLJumpGapShot, 1980)
{
	using namespace OutlawsJumpGapLocal;
	namespace R = OutlawsJumpReachLocal;       // (the slide's lip and far rim)
	if (!L || ShotTime < 0.f) return;
	if (!bArmed)
	{
		bArmed = true;
		FString Mode;
		OLFlags::Value(TEXT("OLJumpGapShot"), Mode);
		bVRMode = Mode.Equals(TEXT("vr"), ESearchCase::IgnoreCase);
		bOld = OLFlags::Param(TEXT("OLOldJumpSpeed"));
		Tail = FString(bVRMode ? TEXT("_vr") : TEXT("")) + (bOld ? TEXT("_old") : TEXT(""));
	}
	if (bVRMode) { bVR = true; BaseGravityScale = 1.5f; }
	auto Done = [&](bool bOk, const FString& Why)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT jumpgap RESULT %s (%s; %s, %s)"), bOk ? TEXT("ALL PASS") : TEXT("FAIL"), *Why, bOld ? TEXT("-OLOldJumpSpeed") : TEXT("new"),
			bVRMode ? TEXT("vr") : TEXT("flat"));
		ShotTime = -1.f;
		FGenericPlatformMisc::RequestExit(false);
	};
	auto Shot = [&](const TCHAR* What)
	{
		const FString Name = FString::Printf(TEXT("jumpgap_%s%s"), What, *Tail);
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		const FVector O = FOutlawsGeometry::ToOutlaws(GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight()));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT jumpgap shot %s.png at %.2f s, feet (%.1f, %.1f, %.1f) units, %.0f cm/s across"), *Name, CaseT, O.X, O.Y, O.Z, GetCharacterMovement()->Velocity.Size2D());
	};
	if (L->IsFrontEnd() || L->IsRebuildPending() || L->IsMoviePlaying() || bMenuOpen) return;
	L->StartSaveDelay = -1.f;           // (no autosaves: the test writes nothing but its shots)
	L->AutosaveWait = 0.f;
	L->bInvincible = true;
	const FOutlawsLevel* Lv = L->LiveLevel();
	UCharacterMovementComponent* M = GetCharacterMovement();
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	LevelT += Dt;
	if (LevelT < 3.f || !Lv) return;
	const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, Half);
	const FVector Ol = FOutlawsGeometry::ToOutlaws(Feet);
	const FVector2D P(Ol.X, Ol.Z);
	auto UeOf = [](const FVector2D& D) { return (FOutlawsGeometry::ToUE(D.X, 0.0, D.Y) - FOutlawsGeometry::ToUE(0.0, 0.0, 0.0)).GetSafeNormal2D(); };
	auto StickFor = [&](const FVector& WantDir) -> FVector2D
	{
		FRotator From = GetControlRotation();
		From.Pitch = From.Roll = 0.0;
		const FVector Fwd = From.Vector(), Right = FRotator(0.0, From.Yaw + 90.0, 0.0).Vector();
		return FVector2D(FVector::DotProduct(WantDir, Right), FVector::DotProduct(WantDir, Fwd));
	};
	if (Step == 0)
	{
		if (!L->LevelName.Equals(TEXT("icecaves"), ESearchCase::IgnoreCase) || !R::FindSlide(*Lv)) { Done(false, TEXT("icecaves' slide 215 and the pit 232 not found")); return; }
		L->GameSpeed = 0.2f;
		const FVector At = FOutlawsGeometry::ToUE(R::SlideStart.X, Lv->SurfaceY(Lv->Sectors[215], R::SlideStart, true) + 0.3, R::SlideStart.Y) + FVector(0.0, 0.0, Half);
		const FRotator Face = UeOf(R::LipMid - R::SlideStart).Rotation();
		TeleportTo(At, Face, false, true);
		if (Controller) Controller->SetControlRotation(Face);
		M->StopMovementImmediately();
		M->SetMovementMode(MOVE_Walking);
		L->HealPlayer(100.f);
		CaseT = 0.f;
		Step = 1;
		return;
	}
	CaseT += Dt;
	Stamina = 100.f;
	bRunning = true;
	if (Step == 1)
	{
		if (CaseT < 0.3f) { M->Velocity = FVector::ZeroVector; return; }
		Step = 2;
		CaseT = 0.f;
	}
	if (Step == 2)
	{
		HeldStick = StickFor(UeOf(R::LipMid - P));
		HeldStickLeft = 0.2f;
		const FVector Vu(M->Velocity.X, M->Velocity.Y, 0.0);
		const FVector VOl = FOutlawsGeometry::ToOutlaws(GetActorLocation() + Vu) - FOutlawsGeometry::ToOutlaws(GetActorLocation());
		const FVector2D VDir = FVector2D(VOl.X, VOl.Z).GetSafeNormal();
		const double Ray = VDir.IsNearlyZero() ? -1.0 : R::RayHit(P, VDir, R::SlideLip);
		const double ToLip = (Ray >= 0.0 ? Ray : R::NearestTo(P, R::SlideLip)) * 30.0;
		if (M->IsFalling() || CaseT >= 20.f) { Done(false, TEXT("off the slide before the jump")); return; }
		if (Vu.Size() < 100.0 || ToLip > Vu.Size() * (GapShotLead + 2.f * Dt) + 5.0) return;
		GapDir = VDir;
		FVector2D Rim;
		RimY = R::RayHit(P, GapDir, R::FarRim, &Rim) >= 0.0 ? Lv->SurfaceY(Lv->Sectors[228], Rim, true) : -96.0;
		if (Controller) Controller->SetControlRotation(FRotator(-12.0, UeOf(GapDir).Rotation().Yaw, 0.0));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT jumpgap jump at 40 %%: off at %.0f cm/s, %.0f cm before the lip, feet (%.1f, %.1f, %.1f) units"), Vu.Size(), ToLip, Ol.X, Ol.Y, Ol.Z);
		DoJump();
		Step = 3;
		CaseT = 0.f;
		return;
	}
	HeldStick = StickFor(UeOf(GapDir));
	HeldStickLeft = 0.2f;
	if (Step == 3)
	{
		const bool bFalling = M->IsFalling();
		if (!bShotAir && CaseT >= 0.8f) { bShotAir = true; Shot(TEXT("air")); }
		if (bWasFalling && !bFalling && DownT < 0.f) DownT = CaseT;
		bWasFalling = bWasFalling || bFalling;
		const bool bUnder = Ol.Y < RimY - 10.0;
		if (!bShotAir || (!bUnder && !(DownT >= 0.f && CaseT - DownT > 1.f) && CaseT < 8.f)) return;
		const FOutlawsSector* Se = Lv->SectorAt(FVector(Ol.X, Ol.Y + 0.5, Ol.Z));
		const int32 In = Se ? Se->Index : -1;
		bAcross = !bUnder && DownT >= 0.f && (In == 228 || In == 207 || (In == 232 && Ol.Y > -120.0));
		Shot(TEXT("end"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT jumpgap end: %s (sector %d)"), bAcross ? TEXT("ACROSS") : TEXT("INTO THE PIT"), In);
		EndT = ShotTime + 1.f;
		Step = 4;
		return;
	}
	if (Step == 4)
	{
		M->Velocity = FVector::ZeroVector;
		if (ShotTime < EndT) return;
		Done(true, FString::Printf(TEXT("2 shots, the jump %s"), bAcross ? TEXT("across") : TEXT("into the pit")));
	}
}

#endif // WITH_OUTLAWS_TESTS
