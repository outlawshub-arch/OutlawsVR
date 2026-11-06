// REQ-SWIMUPDOWN's probe (user 3 Nov: under water jump and crouch moved the body up and down in pulses and the stance note
// came up each time; held, they should swim straight up or down at a steady speed with a short ease in and out; and arm
// swimming on top of the stick's nearly doubled the speed; REQ-SWIMUPDOWN2, the user's answers: both together at most
// max(arms, 1.3 x stick), and a held jump climbs out at an edge by itself):
// -OLShots -OLNoFrontEnd -OLLevel=hideout -OLSwimUpDownProbe[=vr] [-OLSwimUpDownSector=N] [-OLOldSwimUpDown] [-OLOldSwimStack]
// [-OLSwimStackMaxMin] [-OLOldSwimClimb] [-OLOldSwimWeak].
// In the level's deepest water (DebugWaterSector; hideout's shaft 64, 11 m deep), at the Movement Speed dial's 100 %, each
// case put in place and settled:
//   up        from near the floor, jump held (the desktop's Space; =vr the turn stick pushed up): the speed up reaches the
//             stick's swim speed (MaxFlySpeed) within the ease and stays there (the per-frame spread under the tolerance);
//             let go, it eases out;
//   down      from under the surface's hold, crouch held (C; =vr the stick down): the same, down;
//   tosurface from 1.5 m under the float depth, jump held on: the surface takes the body at its float depth, no faster than
//             the swim on the way, never out of the water;
//   surfup    held at the surface facing a way with no edge to climb out on, jump pressed and held: still held, no bob out
//             (today's climb-out is the press's, over an edge only, as before);
//   surfdown  held at the surface, crouch held: it lets go and goes under;
//   bottom    the feet just over the floor, crouch held: on the floor, still swimming;
//   edgeup    facing the shaft's nearest side, the stick pushed a little at it, from 1.5 m under the float depth: jump held
//             (-OLOldSwimUpDown: pressed every half second, the user's pulses) up to the surface at the side's lip: no
//             faster than the swim on the way, then out over the side with no second press (-OLOldSwimClimb: still in);
// each of them: no "Crouch" / "Stand" note, the crouch's state as it was, no crouch to fit (REQ-CROUCHFIT's squeeze) except
// the old's.  -OLOldSwimUpDown: the held up and down must show the old pulse (a kick at the press, then the sink).  The
// desktop run then measures the stacking (GSwimStrokeTest: the strokes without a headset; the body held where it is across,
// so the shaft's sides never stop it, and the swim's own speed read): across, the stick alone, the arms alone (a hard swing;
// a breaststroke's rhythm), both; up, the stick looking up, the arms looking up, both, the held swim up, the held swim up
// and the arms; then the table's one line.  -OLSwimStackMaxMin: the first one's max + 0.3 x min.  -OLOldSwimStack: as before.
// BUG-SWIMWEAK: a weak stroke (2.0 m/s) with the stick held to the stick's limit whatever the arms alone read, and a sweep of
// stroke strengths: the arms alone on time, then both with a frame late in each stroke (about a dropped frame at 72 a second),
// never past the limit at any of them.  -OLOldSwimWeak: the late frame must take both past it at one strength at least.
// Lines "OLSHOT swimupdown <case>: ...", "OLSHOT swimupdown check PASS|FAIL: ..." and "OLSHOT swimupdown RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/IConsoleManager.h"

#if WITH_OUTLAWS_TESTS

extern bool GSwimStrokeTest;                // (OutlawsPlayer.cpp: the strokes run without a headset)

namespace OutlawsSwimUpDownLocal
{
	constexpr float StandHalf = 87.f;           // (OutlawsPlayer.cpp's: a standing body's half height, the swim's "middle")
	constexpr float Ease = 0.25f;               // (OutlawsPlayer.cpp's SwimUpDownEase)
	enum EKind { Up, Down, ToSurface, SurfUp, SurfDown, Bottom, EdgeUp, EdgeOut, Stack };
	enum EArms { NoArms, Hard, Stroke, Short, Weak, Light };
	struct FCase
	{
		const TCHAR* Name;
		EKind Kind;
		float Hold;              // seconds held (the up / down cases: at most, the room decides)
		// (the stacking's)
		bool bStick = false;     // the move stick full forward
		EArms Arms = NoArms;
		float Pitch = 0.f;       // the view's pitch
		bool bUpHeld = false;    // the jump held
		bool bAcross = true;     // measured across (else up)
		// (BUG-SWIMWEAK's sweep)
		float Hand = 0.f;        // the hands' speed in a Weak rhythm (cm/s; else the weak stroke's 200)
		bool bLate = false;      // a frame late in each stroke
	};
	const FCase SwimCases[] = {
		{ TEXT("up"), Up, 2.0f }, { TEXT("down"), Down, 2.0f }, { TEXT("tosurface"), ToSurface, 4.0f },
		{ TEXT("surfup"), SurfUp, 1.5f }, { TEXT("surfdown"), SurfDown, 1.2f }, { TEXT("bottom"), Bottom, 1.2f },
		{ TEXT("edgeup"), EdgeUp, 4.0f }, { TEXT("edgeout"), EdgeOut, 1.0f },
		{ TEXT("across stick"), Stack, 2.6f, true, NoArms, 0.f }, { TEXT("across arms hard"), Stack, 2.6f, false, Hard, 0.f },
		{ TEXT("across both hard"), Stack, 2.6f, true, Hard, 0.f }, { TEXT("across arms strokes"), Stack, 2.6f, false, Stroke, 0.f },
		{ TEXT("across both strokes"), Stack, 2.6f, true, Stroke, 0.f }, { TEXT("across arms short"), Stack, 2.6f, false, Short, 0.f },
		{ TEXT("across both short"), Stack, 2.6f, true, Short, 0.f }, { TEXT("across arms weak"), Stack, 2.6f, false, Weak, 0.f },
		{ TEXT("across both weak"), Stack, 2.6f, true, Weak, 0.f }, { TEXT("across arms light"), Stack, 3.6f, false, Light, 0.f },
		{ TEXT("across both light"), Stack, 3.6f, true, Light, 0.f },
		{ TEXT("sweep arms 130"), Stack, 2.6f, false, Weak, 0.f, false, true, 130.f }, { TEXT("sweep both 130"), Stack, 2.6f, true, Weak, 0.f, false, true, 130.f, true },
		{ TEXT("sweep arms 150"), Stack, 2.6f, false, Weak, 0.f, false, true, 150.f }, { TEXT("sweep both 150"), Stack, 2.6f, true, Weak, 0.f, false, true, 150.f, true },
		{ TEXT("sweep arms 170"), Stack, 2.6f, false, Weak, 0.f, false, true, 170.f }, { TEXT("sweep both 170"), Stack, 2.6f, true, Weak, 0.f, false, true, 170.f, true },
		{ TEXT("sweep arms 190"), Stack, 2.6f, false, Weak, 0.f, false, true, 190.f }, { TEXT("sweep both 190"), Stack, 2.6f, true, Weak, 0.f, false, true, 190.f, true },
		{ TEXT("sweep arms 200"), Stack, 2.6f, false, Weak, 0.f, false, true, 200.f }, { TEXT("sweep both 200"), Stack, 2.6f, true, Weak, 0.f, false, true, 200.f, true },
		{ TEXT("sweep arms 205"), Stack, 2.6f, false, Weak, 0.f, false, true, 205.f }, { TEXT("sweep both 205"), Stack, 2.6f, true, Weak, 0.f, false, true, 205.f, true },
		{ TEXT("sweep arms 210"), Stack, 2.6f, false, Weak, 0.f, false, true, 210.f }, { TEXT("sweep both 210"), Stack, 2.6f, true, Weak, 0.f, false, true, 210.f, true },
		{ TEXT("sweep arms 215"), Stack, 2.6f, false, Weak, 0.f, false, true, 215.f }, { TEXT("sweep both 215"), Stack, 2.6f, true, Weak, 0.f, false, true, 215.f, true },
		{ TEXT("sweep arms 220"), Stack, 2.6f, false, Weak, 0.f, false, true, 220.f }, { TEXT("sweep both 220"), Stack, 2.6f, true, Weak, 0.f, false, true, 220.f, true },
		{ TEXT("sweep arms 230"), Stack, 2.6f, false, Weak, 0.f, false, true, 230.f }, { TEXT("sweep both 230"), Stack, 2.6f, true, Weak, 0.f, false, true, 230.f, true },
		{ TEXT("up stick looking up"), Stack, 2.6f, true, NoArms, 80.f, false, false },
		{ TEXT("up arms looking up"), Stack, 2.6f, false, Stroke, 80.f, false, false },
		{ TEXT("up both looking up"), Stack, 2.6f, true, Stroke, 80.f, false, false },
		{ TEXT("up held"), Stack, 2.6f, false, NoArms, 0.f, true, false },
		{ TEXT("up held and arms looking up"), Stack, 2.6f, false, Stroke, 80.f, true, false } };
	int32 CaseI = 0, Step = 0, Passes = 0, Fails = 0, Toasts = 0, LetGo = 0, Exits = 0, Squeezes = 0;
	int32 Jumps0 = 0;               // (REQ-JUMPSTAMINA) the jumps taken and the stamina as the out-of-the-water cases start
	float ExitStamina = -1.f;
	bool bArmed = false, bVRMode = false, bOldUD = false, bOldStack = false, bMaxMin = false, bOldClimb = false, bWasHeld = false, bWasSwimming = false, bHeldSeen = false;
	float ExitAt = -1.f, ExitMid = 0.f;         // (the first time out of the water: when, and the middle then)
	bool bCrouch0 = false, bAuto0 = false, bIsCrouched0 = false, bWasSqueezed = false, bSurfEdge = false;
	float T = 0.f, SurfaceZ = 0.f, FloorZ = 0.f, Speed = 0.f, HoldFor = 0.f, PrevMid = 0.f, StartMid = 0.f, NextPress = 0.f;
	float Across = 0.f, SurfYaw = 0.f, EdgeYaw = 0.f;    // the longest free way across; a way with no edge at the surface; the nearest side
	int32 Sector = -1;
	FVector Plan = FVector::ZeroVector;
	FString ToastWhere;
	struct FSample { float T, Vz, Mid, VelZ, VelH, Arm; bool bHeld, bSwim; };
	const TCHAR* StackMode() { return bOldStack ? TEXT("-OLOldSwimStack") : bMaxMin ? TEXT("-OLSwimStackMaxMin") : TEXT("new stacking"); }
	TArray<FSample> Samples;
	TMap<FString, float> Means;                 // (the stacking's means, for the rule's check)
	float GameSpeedWas = 0.5f, FpsWas = -1.f;
	int32 LateStroke = -1, OverLimit = 0;       // (BUG-SWIMWEAK's sweep: the stroke with its late frame done; strengths past the limit)
	float LongestDt = 0.f, SweepLimit = 0.f, SweepArms = 0.f;
	bool bOldWeak = false;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT swimupdown check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}
}

OL_SHOT_MATCH(OLSwimUpDownProbe) { return OLFlags::Has(TEXT("OLSwimUpDownProbe")); }

OL_SHOT_TEST(OLSwimUpDownProbe, 1920)
{
	using namespace OutlawsSwimUpDownLocal;
	if (!L || ShotTime < 0.f) return;
	UCharacterMovementComponent* M = GetCharacterMovement();
	if (!bArmed)
	{
		bArmed = true;
		FString Mode;
		OLFlags::Value(TEXT("OLSwimUpDownProbe"), Mode);
		bVRMode = Mode.Equals(TEXT("vr"), ESearchCase::IgnoreCase);
		bOldUD = OLFlags::Param(TEXT("OLOldSwimUpDown"));
		bOldStack = OLFlags::Param(TEXT("OLOldSwimStack"));
		bMaxMin = SwimStackMaxMin();
		bOldClimb = OldSwimClimb();
		bOldWeak = OldSwimWeak();
		GameSpeedWas = L->GameSpeed;
		// at the headset's frame rate (the Quest's 72; -OLSwimUpDownFps=N, 0 as it comes): the fly mode's stop under 10 cm/s
		// makes a stroke's push a frame count, so the arms' numbers go with the rate
		float Fps = 72.f;
		OLFlags::Value(TEXT("OLSwimUpDownFps"), Fps);
		if (IConsoleVariable* MaxFps = IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS")); MaxFps && Fps > 0.f)
		{
			FpsWas = MaxFps->GetFloat();
			MaxFps->Set(Fps, ECVF_SetByCode);
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT swimupdown start: %s, %s, %s, %s, %s, %s, stack share %.2f, at %.0f frames a second"), *L->LevelName, bOldUD ? TEXT("-OLOldSwimUpDown") : TEXT("new up/down"),
			StackMode(), bOldWeak ? TEXT("-OLOldSwimWeak") : TEXT("arms in usual steps"), bOldClimb ? TEXT("-OLOldSwimClimb") : TEXT("held climb"), bVRMode ? TEXT("vr") : TEXT("flat"), SwimStackShare(), Fps);
	}
	if (bVRMode) { bVR = true; BaseGravityScale = 1.5f; }     // (the headset's rules, this run only)
	auto Release = [&]()
	{
		if (bJumpKeyDown) DesktopJumpReleased();
		if (bCrouchKeyDown) CrouchReleased();
		HeldTurnLeft = 0.f;
		HeldTurn = FVector2D::ZeroVector;
		HoldMoveStick(FVector2D::ZeroVector, 0.f);
		HandVelocity[0] = HandVelocity[1] = FVector::ZeroVector;
	};
	auto Finish = [&]()
	{
		Release();
		GSwimStrokeTest = false;
		L->GameSpeed = GameSpeedWas;
		if (IConsoleVariable* MaxFps = IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS")); MaxFps && FpsWas >= 0.f) MaxFps->Set(FpsWas, ECVF_SetByCode);
		// (the old's fault, shown: a late frame in a stroke takes both past the limit)
		if (bOldWeak && !bOldStack && !bMaxMin && Means.Num() > 0) Check(OverLimit > 0, FString::Printf(TEXT("-OLOldSwimWeak: a late frame took both past the limit at %d strengths"), OverLimit));
		if (Means.Num() > 0)
		{
			// the stacking's table in one line (cm/s across): the stick, then the arms alone and both, hard, short and light
			auto Get = [&](const TCHAR* K) { const float* V = Means.Find(K); return V ? *V : -1.f; };
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT swimupdown table (%s): stick %.0f; hard arms %.0f both %.0f; short arms %.0f both %.0f; light arms %.0f both %.0f"), StackMode(),
				Get(TEXT("across stick")), Get(TEXT("across arms hard")), Get(TEXT("across both hard")), Get(TEXT("across arms short")), Get(TEXT("across both short")),
				Get(TEXT("across arms light")), Get(TEXT("across both light")));
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT swimupdown RESULT %s (%d passed, %d failed, %s, %s, %s, %s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			bOldUD ? TEXT("-OLOldSwimUpDown") : TEXT("new up/down"), StackMode(), bOldClimb ? TEXT("-OLOldSwimClimb") : TEXT("held climb"), bVRMode ? TEXT("vr") : TEXT("flat"));
		ShotTime = -1.f;
		CaseI = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	if (CaseI < 0) return;
	if (L->IsFrontEnd() || L->IsRebuildPending() || L->IsMoviePlaying() || bMenuOpen || ShotTime < 3.f) return;
	L->StartSaveDelay = -1.f;           // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	L->bInvincible = true;              // (no drowning)
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const float Mid = float(GetActorLocation().Z) - Half + StandHalf;
	const float FloatZ = SurfaceZ - SwimFloatDepth;
	// the water: its deepest sector, the surface over a point well inside it and the floor under that point
	if (Sector < 0)
	{
		FString Arg;
		Sector = OLFlags::Value(TEXT("OLSwimUpDownSector"), Arg) && Arg.IsNumeric() ? FCString::Atoi(*Arg) : L->DebugWaterSector();
		int32 Top = -1;
		FHitResult Hit;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsSwimUpDownProbe), false, this);
		if (Sector < 0 || !L->DebugPointInSector(Sector, Plan) || !L->WaterSurfaceAt(Plan - FVector(0.0, 0.0, 10.0), SurfaceZ, Top)
			|| !GetWorld()->LineTraceSingleByChannel(Hit, FVector(Plan.X, Plan.Y, SurfaceZ - 10.0), FVector(Plan.X, Plan.Y, SurfaceZ - 5000.0), ECC_Pawn, Q))
		{
			Check(false, FString::Printf(TEXT("deep water in %s (sector %d)"), *L->LevelName, Sector));
			Finish();
			return;
		}
		FloorZ = float(Hit.ImpactPoint.Z);
		// the ways round: the longest free one at mid-depth; at the float depth, one with nothing within DoJump's 70 cm (no
		// edge to climb out on) and the nearest side
		float Best = 0.f, Nearest = 1e9f;
		bSurfEdge = true;
		for (int32 k = 0; k < 8; ++k)
		{
			const FVector Way = FRotator(0.0, k * 45.0, 0.0).Vector();
			const FVector From(Plan.X, Plan.Y, (SurfaceZ + FloorZ) * 0.5f);
			FHitResult W;
			const float Free = GetWorld()->LineTraceSingleByChannel(W, From, From + Way * 5000.0, ECC_Pawn, Q) ? float(W.Distance) : 5000.f;
			if (Free > Best) { Best = Free; Across = k * 45.f; }
			const FVector AtSurf(Plan.X, Plan.Y, SurfaceZ - SwimFloatDepth);
			FHitResult S;
			const bool bHit = GetWorld()->SweepSingleByChannel(S, AtSurf, AtSurf + Way * 100.0, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeSphere(20.f), Q);
			if (!bHit && bSurfEdge) { bSurfEdge = false; SurfYaw = k * 45.f; }
			const float D = bHit ? float(S.Distance) : 1e9f;
			if (D < Nearest) { Nearest = D; EdgeYaw = k * 45.f; }
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT swimupdown water: sector %d, surface z %.0f, floor z %.0f (%.0f cm deep); the longest way across %.0f cm at yaw %.0f; at the surface %s; the nearest side %.0f cm at yaw %.0f"),
			Sector, SurfaceZ, FloorZ, SurfaceZ - FloorZ, Best, Across, bSurfEdge ? TEXT("an edge every way") : *FString::Printf(TEXT("no edge at yaw %.0f"), SurfYaw), Nearest, EdgeYaw);
	}
	if (!(CaseI < int32(UE_ARRAY_COUNT(SwimCases)))) { Finish(); return; }
	const FCase& C = SwimCases[CaseI];
	if (C.Kind == Stack && bVRMode) { ++CaseI; return; }         // (the stacking: the desktop run's, the strokes driven by hand there)
	if (C.Hand > 0.f && (bOldStack || bMaxMin)) { ++CaseI; return; }     // (the sweep goes by the rule against the stick)
	const float CaseYaw = C.Kind == Stack ? Across : C.Kind == EdgeUp || C.Kind == EdgeOut ? EdgeYaw : SurfYaw;
	auto Put = [&](float Middle)
	{
		const FVector At(Plan.X, Plan.Y, Middle - StandHalf + Half);
		TeleportTo(At, FRotator(0.0, CaseYaw, 0.0), false, true);
		M->Velocity = FVector::ZeroVector;
		bSurfaceLocked = false;           // (put somewhere new: the surface's hold from the case before is not this one's)
		if (Controller) Controller->SetControlRotation(FRotator(C.Pitch, CaseYaw, 0.0));
		PrevMid = Middle;
	};
	auto Hold = [&](float Dir)
	{
		// as a player holds it: the desktop's key, the turn stick in the headset
		if (bVRMode) { HeldTurn = FVector2D(0.0, Dir); HeldTurnLeft = 30.f; }
		else if (Dir > 0.f) DesktopJumpPressed();
		else CrouchPressed();
	};
	// the stance note, as it comes up: counted and taken down; a crouch to fit (REQ-CROUCHFIT's squeeze), counted
	for (int32 i = HudLines.Num() - 1; i >= 0; --i)
	{
		if (HudLines[i] != TEXT("Crouch") && HudLines[i] != TEXT("Stand")) continue;
		++Toasts;
		ToastWhere += FString::Printf(TEXT(" %s at %.2f s (middle %.0f under the surface, %.0f over the floor, %.0f cm/s up)"), *HudLines[i], T,
			SurfaceZ - Mid, Mid - StandHalf - FloorZ, M->Velocity.Z);
		DropMessage(HudLines[i]);
	}
	if (bSwimSqueeze && !bWasSqueezed) ++Squeezes;
	bWasSqueezed = bSwimSqueeze;
	if (Step == 0)
	{
		// in place: low for up, under the hold for down, at the surface for the surface's, on the floor for the bottom's
		L->GameSpeed = 0.5f;                                          // (the dial's 100 %)
		Release();
		GSwimStrokeTest = C.Kind == Stack;
		const float Low = FloorZ + StandHalf + 10.f;
		const float Start = C.Kind == Up || (C.Kind == Stack && !C.bAcross) ? Low : C.Kind == Down ? FloatZ - 50.f
			: C.Kind == ToSurface || C.Kind == EdgeUp ? FMath::Max(Low, FloatZ - 150.f) : C.Kind == SurfUp || C.Kind == SurfDown || C.Kind == EdgeOut ? SurfaceZ - 50.f
			: C.Kind == Bottom ? FloorZ + StandHalf + 5.f : (SurfaceZ + FloorZ) * 0.5f;
		Put(Start);
		// (nothing left of the case before's strokes: the velocity is zeroed by Put, the carry, the arms' own swim and the glide
		// are not, and strokes that start over the fly mode's 10 cm/s stop go at their full speed, those under it hardly at all)
		SwimCarry = SwimArmVel = SwimArmCarry = FVector::ZeroVector;
		StrokeUntil = StrokeGlideUntil = 0.0;
		LateStroke = -1;
		LongestDt = 0.f;
		StartMid = Start;
		T = 0.f;
		Samples.Reset();
		Toasts = LetGo = Exits = Squeezes = 0;
		ExitAt = -1.f;
		ToastWhere.Reset();
		bHeldSeen = false;
		Step = 1;
		return;
	}
	T += Dt;
	if (Step == 1)
	{
		// settle (the surface's cases till the hold has the body); the stacking's from a standing start in the water
		const bool bSurf = C.Kind == SurfUp || C.Kind == SurfDown || C.Kind == EdgeOut;
		if (C.Kind != Stack && !bSurf) Put(StartMid);              // (held in place while it settles: no sink before the start)
		if (T < (bSurf ? 1.5f : 0.5f)) return;
		if (!bSwimming)
		{
			Check(false, FString::Printf(TEXT("%s: swimming at the start (middle %.0f under the surface)"), C.Name, SurfaceZ - Mid));
			Step = 0;
			++CaseI;
			return;
		}
		bCrouch0 = bCrouchHeld; bAuto0 = bAutoCrouched; bIsCrouched0 = bIsCrouched;
		bWasHeld = IsSurfaceHeld();
		bWasSwimming = true;
		PrevMid = Mid;
		Toasts = Squeezes = 0;
		ToastWhere.Reset();
		Speed = M->MaxFlySpeed;
		// how long the room lets it be held (up: till 40 cm under the float depth; down: till 30 cm over the floor)
		HoldFor = C.Hold;
		if (C.Kind == Up) HoldFor = FMath::Clamp((FloatZ - 40.f - Mid) / FMath::Max(Speed, 1.f), 0.6f, C.Hold);
		if (C.Kind == Down) HoldFor = FMath::Clamp((Mid - StandHalf - FloorZ - 30.f) / FMath::Max(Speed, 1.f), 0.6f, C.Hold);
		if (C.Kind == Stack)
		{
			if (C.bStick) HoldMoveStick(FVector2D(0.0, 1.0), C.Hold);
			if (C.bUpHeld) Hold(1.f);
		}
		else if (C.Kind == Up || C.Kind == ToSurface || C.Kind == SurfUp || C.Kind == EdgeUp || C.Kind == EdgeOut) Hold(1.f);
		else Hold(-1.f);
		if (C.Kind == SurfUp || C.Kind == EdgeOut) { Stamina = 60.f; ExitStamina = -1.f; Jumps0 = TutNotes.Jumps; }
		NextPress = 0.5f;
		T = 0.f;
		Step = 2;
		return;
	}
	// the measure, every frame
	LongestDt = FMath::Max(LongestDt, Dt);
	const float Vz = (Mid - PrevMid) / FMath::Max(Dt, 1e-4f);
	PrevMid = Mid;
	const bool bHeldNow = T <= HoldFor;
	if (C.Kind == Stack)
	{
		// the strokes: a hard swing (both hands at 4 m/s); a breaststroke's rhythm (2.5 m/s for 0.4 s of every 1.0 s); a short
		// one (2.6 m/s for 0.2 s of every 1.0 s); a weak one (2.0 m/s for 0.4 s of every 1.0 s: alone, under the fly mode's
		// 10 cm/s stop a frame at 72 a second); a light one (2.6 m/s for 0.1 s of every 1.5 s: alone, under the stick's speed)
		const float Part = C.Arms == Short ? 0.2f : C.Arms == Light ? 0.1f : 0.4f;
		const bool bSwing = C.Arms == Hard || (C.Arms != NoArms && FMath::Fmod(T, C.Arms == Light ? 1.5f : 1.0f) < Part);
		const float HandSpeed = C.Arms == Hard ? 400.f : C.Arms == Short || C.Arms == Light ? 260.f : C.Hand > 0.f ? C.Hand : C.Arms == Weak ? 200.f : 250.f;
		HandVelocity[0] = HandVelocity[1] = bSwing && bHeldNow ? FVector(0.0, 0.0, -HandSpeed) : FVector::ZeroVector;
		// (BUG-SWIMWEAK's sweep: a frame late at each stroke's middle, about a dropped frame at 72 a second: what flips a weak stroke)
		if (C.bLate && bSwing && bHeldNow && FMath::Fmod(T, 1.0f) >= 0.2f && int32(T) != LateStroke)
		{
			LateStroke = int32(T);
			FPlatformProcess::Sleep(0.022f);
		}
		if (C.bStick && bHeldNow) HoldMoveStick(FVector2D(0.0, 1.0), 0.2f);
		// the body held where it is across (the shaft's sides would stop it; the swim's own speed is what is read)
		SetActorLocation(FVector(Plan.X, Plan.Y, GetActorLocation().Z), false, nullptr, ETeleportType::TeleportPhysics);
		// up: ended once the body nears the surface's hold (the hold would stop it)
		if (!C.bAcross && Mid > FloatZ - 60.f && bHeldNow) HoldFor = T;
	}
	if (C.Kind == EdgeUp && bHeldNow)
	{
		HoldMoveStick(FVector2D(0.0, 0.4), 0.2f);                     // (a little at the side: the way the swimmer goes)
		if (bOldUD && T >= NextPress)
		{
			// the old one a press: pressed again every half second (the user's pulses)
			NextPress = T + 0.5f;
			if (bVRMode) { HeldTurnLeft = 0.f; HeldTurn = FVector2D::ZeroVector; bJumpLatched = false; Hold(1.f); }
			else { DesktopJumpReleased(); Hold(1.f); }
		}
	}
	Samples.Add({ T, Vz, Mid, float(M->Velocity.Z), float(M->Velocity.Size2D()), float(SwimArmVel.Size()), bHeldNow, bSwimming });
	if (bWasHeld && !IsSurfaceHeld() && bSwimming) ++LetGo;
	if (bWasSwimming && !bSwimming)
	{
		if (Exits == 0) { ExitAt = T; ExitMid = PrevMid; ExitStamina = Stamina; }
		++Exits;
	}
	bWasHeld = IsSurfaceHeld();
	bWasSwimming = bSwimming;
	if (IsSurfaceHeld()) bHeldSeen = true;
	if (bHeldNow) return;
	if (T < HoldFor + Dt * 1.5f) Release();                          // (let go, then the ease out watched)
	if (T < HoldFor + (C.Kind == Stack ? 0.1f : 0.8f)) return;
	// the case's numbers
	auto Window = [&](float From, float To, bool bVert, float& OutMean, float& OutMin, float& OutMax, float& OutPeak, bool bVel)
	{
		OutMean = 0.f; OutMin = 1e9f; OutMax = -1e9f; OutPeak = 0.f;
		int32 N = 0;
		for (const FSample& S : Samples)
		{
			const float V = bVel ? (bVert ? S.VelZ : S.VelH) : S.Vz;
			OutPeak = FMath::Max(OutPeak, FMath::Abs(V));
			if (S.T < From || S.T > To) continue;
			OutMean += V; OutMin = FMath::Min(OutMin, V); OutMax = FMath::Max(OutMax, V); ++N;
		}
		OutMean = N ? OutMean / N : 0.f;
		if (!N) { OutMin = OutMax = 0.f; }
	};
	const bool bCrouchSame = bCrouchHeld == bCrouch0 && bAutoCrouched == bAuto0 && bIsCrouched == bIsCrouched0;
	const FString Stance = FString::Printf(TEXT("no stance note (%d%s), no crouch to fit (%d), the crouch as it was (held %d auto %d crouched %d, were %d %d %d)"), Toasts, *ToastWhere,
		Squeezes, bCrouchHeld ? 1 : 0, bAutoCrouched ? 1 : 0, bIsCrouched ? 1 : 0, bCrouch0 ? 1 : 0, bAuto0 ? 1 : 0, bIsCrouched0 ? 1 : 0);
	auto StanceCheck = [&]()
	{
		if (!bOldUD) Check(Toasts == 0 && Squeezes == 0 && bCrouchSame, FString::Printf(TEXT("%s: %s"), C.Name, *Stance));
		else UE_LOG(LogOutlaws, Display, TEXT("OLSHOT swimupdown %s, -OLOldSwimUpDown: %s"), C.Name, *Stance);
	};
	if (C.Kind == Up || C.Kind == Down)
	{
		const float Sign = C.Kind == Up ? 1.f : -1.f;
		float Mean, Lo, Hi, Peak;
		Window(Ease + 0.15f, HoldFor, false, Mean, Lo, Hi, Peak, false);
		// reached: the first frame at 90 % of the set speed
		float Reached = -1.f;
		for (const FSample& S : Samples) if (S.Vz * Sign >= 0.9f * Speed) { Reached = S.T; break; }
		// the ease out: how long after letting go till within a tenth of the set speed of still, or of the water's sink
		const float Sink = SwimSinkSpeed * L->Pace();
		float Out = -1.f;
		for (const FSample& S : Samples) if (S.T > HoldFor && Out < 0.f && (FMath::Abs(S.Vz) < 0.1f * Speed || FMath::Abs(S.Vz + Sink) < 0.1f * Speed)) Out = S.T - HoldFor;
		// the pulse: the fastest frame against the held window's end (the last third)
		float EndMean = 0.f; int32 EndN = 0;
		for (const FSample& S : Samples) if (S.T > HoldFor * 0.67f && S.T <= HoldFor) { EndMean += S.Vz * Sign; ++EndN; }
		EndMean = EndN ? EndMean / EndN : 0.f;
		const float Tol = FMath::Max(0.12f * Speed, 6.f);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT swimupdown %s: held %.2f s from %.0f cm under the surface; set speed %.0f cm/s; reached 90 %% at %.2f s; steady window %.2f..%.2f s mean %.0f, min %.0f, max %.0f (spread %.0f, tolerance %.0f); peak %.0f, the last third %.0f; eased out to a tenth %.2f s after letting go; toasts %d"),
			C.Name, HoldFor, SurfaceZ - StartMid, Speed, Reached, Ease + 0.15f, HoldFor, Mean * Sign, Lo * Sign, Hi * Sign, Hi - Lo, Tol, Peak, EndMean, Out, Toasts);
		if (!bOldUD)
		{
			Check(Reached >= 0.f && Reached <= Ease + 0.15f, FString::Printf(TEXT("%s: up to speed within the ease (90 %% of %.0f cm/s at %.2f s, the ease %.2f)"), C.Name, Speed, Reached, Ease));
			Check(Hi - Lo <= Tol && FMath::Abs(Mean * Sign - Speed) <= Tol, FString::Printf(TEXT("%s: steady, no pulses (mean %.0f of %.0f cm/s, spread %.0f under %.0f)"), C.Name, Mean * Sign, Speed, Hi - Lo, Tol));
			Check(Out >= 0.f && Out <= 0.6f, FString::Printf(TEXT("%s: eased out after letting go (within a tenth of the speed %.2f s after)"), C.Name, Out));
		}
		else
		{
			Check(Peak > 1.4f * FMath::Max(EndMean, 1.f) || Hi - Lo > Tol, FString::Printf(TEXT("%s, -OLOldSwimUpDown: the old pulse (a kick to %.0f cm/s, %.0f by the hold's last third, spread %.0f)"), C.Name, Peak, EndMean, Hi - Lo));
		}
		StanceCheck();
	}
	else if (C.Kind == ToSurface || C.Kind == EdgeUp || C.Kind == EdgeOut)
	{
		float Top = -1e9f, Fastest = 0.f;
		for (const FSample& S : Samples) { Top = FMath::Max(Top, S.Mid); Fastest = FMath::Max(Fastest, S.Vz); }
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT swimupdown %s: from %.0f cm under the surface at yaw %.0f, held %d at the end (seen %d), out of the water %d times; the highest middle %.0f cm off its float depth; the fastest frame up %.0f cm/s (the swim %.0f); toasts %d, crouches to fit %d"),
			C.Name, SurfaceZ - StartMid, CaseYaw, IsSurfaceHeld() ? 1 : 0, bHeldSeen ? 1 : 0, Exits, Top - FloatZ, Fastest, Speed, Toasts, Squeezes);
		if (C.Kind == EdgeOut)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT swimupdown stamina, out of the water: 60 at the hold, %.2f at the climb out, %.2f at the end, %d jumps taken"), ExitStamina, Stamina, TutNotes.Jumps - Jumps0);
			Check(Exits > 0 && Top > FloatZ + 50.f, FString::Printf(TEXT("%s: held at the surface facing the side, the jump's press climbs out over it as before (out %d, the middle up to %.0f over its float depth)"),
				C.Name, Exits, Top - FloatZ));
		}
		if (C.Kind == EdgeUp)
		{
			// (REQ-SWIMUPDOWN2) the held jump: up at the swim speed while in the water, then out over the side by itself at the
			// surface (one press only, at the start, deep); -OLOldSwimClimb: still in the water, a fresh press needed
			float FastestIn = 0.f;
			for (const FSample& S : Samples) if (S.bSwim && (ExitAt < 0.f || S.T < ExitAt)) FastestIn = FMath::Max(FastestIn, S.Vz);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT swimupdown %s, %s: out at %.2f s (the middle %.0f cm off its float depth then); the fastest frame up in the water %.0f cm/s"),
				C.Name, bOldClimb ? TEXT("-OLOldSwimClimb") : TEXT("held climb"), ExitAt, ExitMid - FloatZ, FastestIn);
			if (!bOldUD && !bOldClimb)
			{
				Check(Exits > 0 && Top > FloatZ + 50.f && ExitMid > SurfaceZ - 80.f && FastestIn <= 1.15f * Speed + 3.f, FString::Printf(TEXT("%s: the held jump swims up at the swim speed (fastest %.0f of %.0f cm/s) and climbs out at the surface by itself, no second press (out %d at %.2f s, the middle %.0f off its float depth then, up to %.0f over it)"),
					C.Name, FastestIn, Speed, Exits, ExitAt, ExitMid - FloatZ, Top - FloatZ));
			}
			else if (!bOldUD)
			{
				Check(Exits == 0 && bHeldSeen, FString::Printf(TEXT("%s, -OLOldSwimClimb: the held jump stays in the water at the side (held %d, out %d): a fresh press needed"), C.Name, bHeldSeen ? 1 : 0, Exits));
			}
		}
		if (!bOldUD && C.Kind == ToSurface)
		{
			Check(bHeldSeen && Exits == 0 && Top <= FloatZ + 10.f && Fastest <= 1.15f * Speed + 3.f, FString::Printf(TEXT("%s: the surface takes the held swim up at its float depth (held %d, out %d, the middle at most %.0f over it, the fastest %.0f of %.0f cm/s)"),
				C.Name, bHeldSeen ? 1 : 0, Exits, Top - FloatZ, Fastest, Speed));
		}
		StanceCheck();
	}
	else if (C.Kind == SurfUp || C.Kind == SurfDown || C.Kind == Bottom)
	{
		float Top = -1e9f, Low = 1e9f;
		for (const FSample& S : Samples) if (S.bHeld) { Top = FMath::Max(Top, S.Mid); Low = FMath::Min(Low, S.Mid); }
		float Mean, Lo, Hi, Peak;
		Window(HoldFor - 0.5f, HoldFor, false, Mean, Lo, Hi, Peak, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT swimupdown %s: held %.1f s at yaw %.0f; the middle %.0f..%.0f cm off its float depth (feet %.0f..%.0f over the floor); the hold let go %d times, out of the water %d times, held at the end %d, swimming %d; the last half second %.0f cm/s; toasts %d"),
			C.Name, HoldFor, CaseYaw, Low - FloatZ, Top - FloatZ, Low - StandHalf - FloorZ, Top - StandHalf - FloorZ, LetGo, Exits, IsSurfaceHeld() ? 1 : 0, bSwimming ? 1 : 0, Mean, Toasts);
		if (C.Kind == SurfUp) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT swimupdown stamina, the jump held at the surface: 60 at the hold, %.2f at the end, out %d times, %d jumps taken"), Stamina, Exits, TutNotes.Jumps - Jumps0);
		if (C.Kind == SurfUp && !bSurfEdge) Check(LetGo == 0 && Exits == 0 && Top <= FloatZ + 12.f && Low >= FloatZ - 12.f, FString::Printf(TEXT("%s: held at the surface through the press and the hold, no edge ahead (let go %d, out %d, %.0f..%.0f cm)"), C.Name, LetGo, Exits, Low - FloatZ, Top - FloatZ));
		if (C.Kind == SurfUp && bSurfEdge) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT swimupdown %s: an edge every way here: the press climbs out over it (today's), out %d"), C.Name, Exits);
		if (C.Kind == SurfDown) Check(LetGo > 0 && Low < FloatZ - 25.f, FString::Printf(TEXT("%s: the hold lets go and the body goes under (%.0f cm under its float depth)"), C.Name, FloatZ - Low));
		if (C.Kind == Bottom) Check(bSwimming && Low - StandHalf >= FloorZ - 5.f && FMath::Abs(Mean) < 10.f, FString::Printf(TEXT("%s: on the floor, still swimming (feet %.0f over it, %.0f cm/s)"), C.Name, Low - StandHalf - FloorZ, Mean));
		StanceCheck();
	}
	else
	{
		// the stacking: the swim's own speed (its velocity: across, or up) over two of the strokes' cycles
		float Mean, Lo, Hi, Peak;
		const float From = FMath::Min(0.6f, HoldFor * 0.5f);
		Window(From, HoldFor, !C.bAcross, Mean, Lo, Hi, Peak, true);
		float MoveMean, MLo, MHi, MPeak;
		Window(From, HoldFor, true, MoveMean, MLo, MHi, MPeak, false);
		Means.Add(C.Name, Mean);
		// (the arms' own swim, the stacking's copy of them that the cap goes by, and the frames a second it ran at)
		float ArmMean = 0.f, ArmPeak = 0.f;
		int32 ArmN = 0;
		for (const FSample& S : Samples) if (S.T >= From && S.T <= HoldFor) { ArmMean += S.Arm; ArmPeak = FMath::Max(ArmPeak, S.Arm); ++ArmN; }
		ArmMean = ArmN ? ArmMean / ArmN : 0.f;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT swimupdown stack %s: %.2f..%.2f s, mean %.0f cm/s (min %.0f, max %.0f), peak %.0f; the stick's swim %.0f, the arms' cap %.0f; the body's own rise %.0f cm/s; the arms' own swim mean %.0f, peak %.0f; %.0f frames a second, the longest %.1f ms"),
			C.Name, From, HoldFor, Mean, Lo, Hi, Peak, Speed, Speed / 0.5f, MoveMean, ArmMean, ArmPeak, ArmN / FMath::Max(HoldFor - From, 0.01f), LongestDt * 1000.f);
		// the rule, once both and each alone are in: both no more than max + share x min (some slack), no less than the faster
		auto Rule = [&](const TCHAR* Both, const TCHAR* Stick, const TCHAR* Arms)
		{
			if (FCString::Strcmp(C.Name, Both) != 0) return;
			const float* B = Means.Find(Both); const float* S = Means.Find(Stick); const float* A = Means.Find(Arms);
			if (!B || !S || !A) return;
			const float MaxMin = FMath::Max(*S, *A) + SwimStackShare() * FMath::Min(*S, *A);
			const float Against = FMath::Max(*A, (1.f + SwimStackShare()) * *S);     // (REQ-SWIMUPDOWN2: against the stick)
			const float Want = bMaxMin ? MaxMin : Against;
			const float Sum = *S + *A;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT swimupdown stack rule %s (%s): both %.0f, stick %.0f, arms %.0f; max(arms, %.2f x stick) %.0f, max + %.2f x min %.0f, the sum %.0f; both / stick %.2f, both / the faster %.2f"),
				Both, StackMode(), *B, *S, *A, 1.f + SwimStackShare(), Against, SwimStackShare(), MaxMin, Sum, *B / FMath::Max(*S, 1.f), *B / FMath::Max(FMath::Max(*S, *A), 1.f));
			// (BUG-SWIMWEAK: a weak stroke sits on the fly mode's 10 cm/s stop. Alone, the same stroke still reads 13 or 159, a frame
			// running late takes it over; the cap's copy no longer goes by that, so both is held to the stick's limit whichever the
			// arms alone read. -OLOldSwimWeak: printed only, the old lands at 143 or 173 by the frames)
			if (FCString::Strcmp(Both, TEXT("across both weak")) == 0)
			{
				if (!bOldStack && !bOldWeak && !bMaxMin) Check(*B <= (1.f + SwimStackShare()) * *S * 1.08f + 5.f && *B >= *S * 0.92f - 5.f, FString::Printf(TEXT("%s: both %.0f cm/s, the stick's limit %.0f (arms alone %.0f, the stick %.0f)"), Both, *B, (1.f + SwimStackShare()) * *S, *A, *S));
				return;
			}
			if (!bOldStack) Check(*B <= Want * 1.08f + 5.f && *B >= FMath::Max(*S, *A) * 0.92f - 5.f, FString::Printf(TEXT("%s: both %.0f cm/s, the rule's %.0f (between the faster alone, %.0f, and it)"), Both, *B, Want, FMath::Max(*S, *A)));
		};
		Rule(TEXT("across both hard"), TEXT("across stick"), TEXT("across arms hard"));
		Rule(TEXT("across both strokes"), TEXT("across stick"), TEXT("across arms strokes"));
		Rule(TEXT("across both short"), TEXT("across stick"), TEXT("across arms short"));
		Rule(TEXT("across both weak"), TEXT("across stick"), TEXT("across arms weak"));
		Rule(TEXT("across both light"), TEXT("across stick"), TEXT("across arms light"));
		Rule(TEXT("up both looking up"), TEXT("up stick looking up"), TEXT("up arms looking up"));
		Rule(TEXT("up held and arms looking up"), TEXT("up held"), TEXT("up arms looking up"));
		// BUG-SWIMWEAK's sweep: at each strength both, with a frame late in each stroke, against the limit by the arms alone on
		// time: the two farthest apart a strength's flip can put them, the arms at their lowest and both at their highest.  The
		// limit a frame at a time, its mean over the arms' frames: max(arms then, the stick's limit); both rides that limit's
		// top between the strokes, so its mean passes the arms' mean and max(mean, the stick's limit) would read too low
		const float* S = Means.Find(TEXT("across stick"));
		if (C.Hand > 0.f && S)
		{
			const float StickLimit = (1.f + SwimStackShare()) * *S;
			if (!C.bStick)
			{
				float Sum = 0.f;
				int32 N = 0;
				for (const FSample& F : Samples) if (F.T >= From && F.T <= HoldFor) { Sum += FMath::Max(F.VelH, StickLimit); ++N; }
				SweepLimit = N ? Sum / N : StickLimit;
				SweepArms = Mean;
			}
			else
			{
				// (to the weak stroke's strength the arms on time stay under the stop, the limit the stick's whatever they read here)
				const float Limit = C.Hand <= 200.f ? StickLimit : SweepLimit;
				const bool bOver = Mean > Limit * 1.08f + 5.f;
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT swimupdown sweep %.0f: both %.0f cm/s with a late frame a stroke (the longest %.1f ms), the arms' own swim %.0f in it; arms alone %.0f, the limit %.0f"), C.Hand, Mean, LongestDt * 1000.f, ArmMean, SweepArms, Limit);
				if (bOver) ++OverLimit;
				if (!bOldWeak) Check(!bOver && Mean >= *S * 0.92f - 5.f, FString::Printf(TEXT("%s: both %.0f cm/s, the limit %.0f (arms alone %.0f)"), C.Name, Mean, Limit, SweepArms));
			}
		}
	}
	Release();
	Step = 0;
	++CaseI;
}

#endif // WITH_OUTLAWS_TESTS
