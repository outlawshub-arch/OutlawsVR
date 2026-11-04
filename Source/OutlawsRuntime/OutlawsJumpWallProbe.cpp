// BUG-JUMPWALL (user 4 Nov, after the Quest test: "I seem to bounce off walls I'm jumping into instead of being able to grab
// them"; an angle limit, "maybe there should be 45 degree leeway or maybe a 30 degree leeway", and a jump along a wall can still
// glance off it): -OLShots -OLNoFrontEnd -OLLevel=<level> -OLJumpWallProbe[=vr] [-OLOldJumpWall] [-OLJumpWallLeeway=<degrees>]
// [-OLJumpWallAngles=a+b] [-OLJumpWallDials=a+b].  Angles are degrees from straight at the wall (0: head-on, 90: along it).
// On -OLLevel=town, the level's longest solid wall with a flat free floor in front of it.  At each dial, each angle:
// a run's speed, the stick held the way it goes, the jump, the hit; logged the speed sideways along the wall 0.05 and 0.25 s
// after it and the way slid.  New: within the leeway the boost is over and the sideways speed at the hit is what the engine left
// (after it the stick held along the wall adds its own, the run's top speed at most), past it the boost holds (the speed scaled
// back up, as before).  Then a wall leaning back and one leaning over the body, a hit on the way down, on a real wall and on one as
// high as a ledge, a low lip and a step put in the way at the landing (both keep the boost and the body gets over them), a step put in
// the way on the rise (a wall: the boost ends and the stick carries it over), a ceiling (keeps the boost), and at 40 %, where the
// jump is stretched, a hit early in the rise reaching the open-air jump's height.  VR only: a block as high as the mill's walkway
// lip (that walkway has no wall under it), a jump at it with the hand up and the squeeze held, ending in the grab (REQ-AIRCATCH).
// -OLOldJumpWall: the same cases, the scale-up logged.  Invincible.  Lines "OLSHOT jumpwall ..." and "OLSHOT jumpwall RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsLevel.h"
#include "OutlawsGeometry.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Camera/CameraComponent.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "MotionControllerComponent.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsJumpWallLocal
{
	enum class EWallKind : uint8 { Wall, Slope, Late, LowWall, Lip, StepDown, StepUp, Ceiling, OpenApex, WallApex, Ledge };
	struct FCase
	{
		EWallKind Kind = EWallKind::Wall;
		double Angle = 0.0;     // degrees from straight at the wall
		double Lean = 0.0;      // Slope: the wall's pitch, degrees (+: its top over the body)
		double High = 0.0;      // LowWall, Lip, StepDown, StepUp: its top over the floor, cm
		float Dial = 0.5f;
		double Far = 0.0;       // Ledge: the take-off to the hit, cm (0: RunUp)
	};
	constexpr double MinLen = 40.0, Depth = 19.0, Room = 12.0;     // units: the wall 12 m long, 5.7 m free in front, 3.6 m high
	constexpr double RunUp = 220.0;      // cm from the take-off to the hit (0.3 s or so: on the rise)
	constexpr double LateRun = 520.0;  // the same on the way down
	constexpr double BandOff = 150.0;  // cm off the wall: the line of the jumps along it
	constexpr double LedgeHigh = 240.0;   // cm over the floor (the mill's walkway lip)
	TArray<FCase> WallCases;
	int32 CaseI = 0, Step = 0, Frames = 0, Passes = 0, Fails = 0;
	bool bArmed = false, bVRMode = false, bOld = false, bHaveWall = false, bFlew = false, bHit = false, bPlaced = false, bTook = false, bSqueezed = false;
	float LevelT = 0.f, CaseT = 0.f, HitT = -1.f, Leeway = 45.f, AirFrames = 0.f;
	bool bBoostAfter = false, bSampled = false;
	float PlaceT = -1.f;
	FVector HoldAt = FVector::ZeroVector;
	double TopAfter = -1e9;      // the feet's highest since the thing was put in the way
	double FloorZ = 0.0, WallLen = 0.0, IntoPrev = 0.0, SpeedPrev = 0.0, V0 = 0.0, S05 = -1.0, S25 = -1.0, Peak = 0.0, Slid = 0.0, Apex = 0.0, OpenApexZ = -1.0, TakeOffZ = 0.0, VzPrev = 0.0;
	double Speed05 = -1.0, S0 = -1.0, RunTop = 0.0;     // the sideways speed the frame of the hit, the run's top speed
	FVector WallMid = FVector::ZeroVector, WallIn = FVector::ZeroVector, WallT = FVector::ZeroVector;     // UE: the wall's middle, into it, along it
	FVector Way = FVector::ZeroVector, In = FVector::ZeroVector, Tan = FVector::ZeroVector, HitFeet = FVector::ZeroVector, BoxFar = FVector::ZeroVector, Off[2];
	TWeakObjectPtr<AActor> Box;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT jumpwall check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}

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

	bool FlatAt(const FOutlawsLevel& Lv, double Y, const FVector2D& P)
	{
		const FOutlawsSector* S = Lv.SectorAt(FVector(P.X, Y + 0.5, P.Y));
		return S && FMath::Abs(Lv.SurfaceY(*S, P, true) - Y) < 0.05 && Lv.SurfaceY(*S, P, false) - Y >= Room;
	}

	/** The level's longest solid wall (of MinLen or more) with a flat floor Depth deep before it, Room high. */
	bool FindWall(const FOutlawsLevel& Lv)
	{
		double Best = 0.0, Longest = 0.0;
		for (const FOutlawsSector& S : Lv.Sectors)
		{
			if (S.Ceil.Y - S.Floor.Y < Room) continue;
			for (const FOutlawsWall& W : S.Walls)
			{
				if (W.Adjoin >= 0 || !S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) continue;
				const FVector2D A = S.Verts[W.V1], B = S.Verts[W.V2];
				const double Len = FVector2D::Distance(A, B);
				Longest = FMath::Max(Longest, Len);
				if (Len < MinLen || Len <= Best) continue;
				const FVector2D T = (B - A) / Len;
				FVector2D N(-T.Y, T.X);
				if (!FlatAt(Lv, S.Floor.Y, (A + B) * 0.5 + N)) N = -N;
				bool bOk = true;
				for (int32 u = 0; u <= 10 && bOk; ++u)
				{
					for (double v = 1.0; v <= Depth && bOk; v += 2.0)
					{
						bOk = FlatAt(Lv, S.Floor.Y, A + T * (Len * (0.05 + 0.09 * u)) + N * v);
					}
				}
				if (!bOk) continue;
				Best = Len;
				const FVector2D Mid = (A + B) * 0.5;
				WallMid = FOutlawsGeometry::ToUE(Mid.X, S.Floor.Y, Mid.Y);
				FloorZ = WallMid.Z;
				WallIn = -(FOutlawsGeometry::ToUE(N.X, 0.0, N.Y) - FOutlawsGeometry::ToUE(0.0, 0.0, 0.0)).GetSafeNormal2D();
				WallT = (FOutlawsGeometry::ToUE(T.X, 0.0, T.Y) - FOutlawsGeometry::ToUE(0.0, 0.0, 0.0)).GetSafeNormal2D();
				WallLen = Len * 30.0;
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT jumpwall wall: sector %d, %.1f units from (%.1f, %.1f) to (%.1f, %.1f), floor %.1f"), S.Index, Len, A.X, A.Y, B.X, B.Y, S.Floor.Y);
			}
		}
		if (Best <= 0.0) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT jumpwall wall: none of %.0f units with the floor free (the longest solid wall %.1f)"), MinLen, Longest);
		return Best > 0.0;
	}

	const TCHAR* KindName(EWallKind K)
	{
		switch (K)
		{
		case EWallKind::Wall: return TEXT("wall");
		case EWallKind::Slope: return TEXT("sloped wall");
		case EWallKind::Late: return TEXT("wall, on the way down");
		case EWallKind::LowWall: return TEXT("ledge-high wall, on the way down");
		case EWallKind::Lip: return TEXT("low lip at the landing");
		case EWallKind::StepDown: return TEXT("step at the landing");
		case EWallKind::StepUp: return TEXT("step on the rise");
		case EWallKind::Ceiling: return TEXT("ceiling");
		case EWallKind::OpenApex: return TEXT("open air");
		case EWallKind::WallApex: return TEXT("wall early in the rise");
		default: return TEXT("ledge");
		}
	}
}

OL_SHOT_MATCH(OLJumpWallProbe) { return OLFlags::Has(TEXT("OLJumpWallProbe")); }

OL_SHOT_TEST(OLJumpWallProbe, 2410)
{
	using namespace OutlawsJumpWallLocal;
	if (!L || ShotTime < 0.f) return;
	if (L->IsFrontEnd() || L->IsRebuildPending() || L->IsMoviePlaying() || bMenuOpen) return;
	L->StartSaveDelay = -1.f;           // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	L->bInvincible = true;
	const FOutlawsLevel* Lv = L->LiveLevel();
	UCharacterMovementComponent* M = GetCharacterMovement();
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const float R = GetCapsuleComponent()->GetScaledCapsuleRadius();
	LevelT += Dt;
	if (LevelT < 3.f || !Lv) return;
	if (bVRMode) { bVR = true; BaseGravityScale = 1.5f; }     // (the headset's rules and its fall, this run only)
	if (!bArmed)
	{
		bArmed = true;
		FString Mode;
		OLFlags::Value(TEXT("OLJumpWallProbe"), Mode);
		bVRMode = Mode.Equals(TEXT("vr"), ESearchCase::IgnoreCase);
		bOld = OLFlags::Param(TEXT("OLOldJumpWall"));
		OLFlags::Value(TEXT("OLJumpWallLeeway"), Leeway);
		if (bVRMode)
		{
			bCrouchHeld = bStandPending = bAutoCrouched = false;
			CrouchAmount = 1.f;
			CrouchDrop = CrouchFull = 0.f;
			ShownDrop = ShownDropVel = 0.f;
			HeightOffset = 0.f;
			Camera->SetRelativeLocation(FVector(0.0, 0.0, 170.0));
		}
		bHaveWall = FindWall(*Lv);
		if (bHaveWall)
		{
			const TArray<float> Angles = Numbers(TEXT("OLJumpWallAngles"), { 0.f, 15.f, 30.f, 44.f, 46.f, 60.f, 80.f });
			for (const float D : Numbers(TEXT("OLJumpWallDials"), { 0.5f, 0.2f, 0.75f }))
			{
				for (const float A : Angles) WallCases.Add({ EWallKind::Wall, A, 0.0, 0.0, D });
			}
			for (const double Lean : { -12.0, 12.0 })
			{
				for (const double A : { 20.0, 60.0 }) WallCases.Add({ EWallKind::Slope, A, Lean, 0.0, 0.5f });
			}
			WallCases.Add({ EWallKind::Late, 20.0, 0.0, 0.0, 0.5f });
			WallCases.Add({ EWallKind::LowWall, 20.0, 0.0, 240.0, 0.5f });
			WallCases.Add({ EWallKind::Lip, 0.0, 0.0, 20.0, 0.5f });
			WallCases.Add({ EWallKind::Lip, 20.0, 0.0, 20.0, 0.5f });
			WallCases.Add({ EWallKind::StepDown, 0.0, 0.0, 60.0, 0.5f });
			WallCases.Add({ EWallKind::StepUp, 20.0, 0.0, 100.0, 0.5f });
			WallCases.Add({ EWallKind::Ceiling, 0.0, 0.0, 0.0, 0.5f });
			WallCases.Add({ EWallKind::OpenApex, 0.0, 0.0, 0.0, 0.2f });
			WallCases.Add({ EWallKind::WallApex, 20.0, 0.0, 0.0, 0.2f });
		}
		if (bHaveWall && bVRMode)
		{
			for (const double A : { 0.0, 25.0, -25.0 }) WallCases.Add({ EWallKind::Ledge, A, 0.0, 0.0, 0.5f });
			WallCases.Add({ EWallKind::Ledge, 25.0, 0.0, 0.0, 0.75f });
			WallCases.Add({ EWallKind::Ledge, 25.0, 0.0, 0.0, 0.5f, 480.0 });      // (reaching the wall on the way down)
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT jumpwall start: %d cases, %s, %s, leeway %.0f deg, level %s, gravity %.0f"), WallCases.Num(), bOld ? TEXT("-OLOldJumpWall") : TEXT("new"), bVRMode ? TEXT("vr") : TEXT("flat"), Leeway, *L->LevelName, M->GetGravityZ());
		return;
	}
	auto ClearBox = [&]()
	{
		if (AActor* A = Box.Get()) A->Destroy();
		Box = nullptr;
	};
	if (!WallCases.IsValidIndex(CaseI))
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT jumpwall RESULT %s (%d passed, %d failed, %s, %s, leeway %.0f deg)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			bOld ? TEXT("-OLOldJumpWall") : TEXT("new"), bVRMode ? TEXT("vr") : TEXT("flat"), Leeway);
		ShotTime = -1.f;
		FGenericPlatformMisc::RequestExit(false);
		return;
	}
	const FCase& C = WallCases[CaseI];
	const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, Half);
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
	auto AddBox = [&](const FVector& At, const FRotator& Rot, const FVector& Ext)
	{
		ClearBox();
		if (AActor* S = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform(Rot, At)))
		{
			UBoxComponent* B = NewObject<UBoxComponent>(S);
			S->SetRootComponent(B);
			B->SetBoxExtent(Ext);
			B->SetCollisionProfileName(TEXT("BlockAll"));
			B->RegisterComponent();
			B->SetWorldLocationAndRotation(At, Rot);
			Box = S;
		}
	};
	auto PutHands = [&]() { for (int32 i = 0; i < 2; ++i) GripOf(i)->SetWorldLocation(GetActorLocation() + Off[i]); };
	const int32 H = 1, O = 0;       // (the ledge's hand)
	if (Step == 0)
	{
		TestAxes.Reset();
		if (IsClimbing()) ClimbWheelTestLetGo(GrabHand);      // (a fall, not the climb onto the top)
		ClearBox();
		L->GameSpeed = C.Dial;
		L->HealPlayer(100.f);
		Stamina = 100.f;
		bRunning = true;
		const double Rad = FMath::DegreesToRadians(C.Angle);
		FVector From;
		if (C.Kind == EWallKind::Ledge)
		{
			// a block as high as the mill's walkway lip, 1.5 m out from the real wall, its top deep enough for the hand
			In = WallIn;
			Tan = WallT;
			Way = In * FMath::Cos(Rad) + Tan * FMath::Sin(Rad);
			const FVector At = WallMid - In * 150.0 + FVector(0.0, 0.0, LedgeHigh * 0.5);
			AddBox(At, FRotator(0.0, In.Rotation().Yaw, 0.0), FVector(100.0, 1000.0, LedgeHigh * 0.5));     // (long: the stick stays held along it)
			From = FVector(At.X, At.Y, FloorZ) - In * (100.0 + R) - Way * (C.Far > 0.0 ? C.Far : RunUp);
			Equip(0, -1);
			Equip(1, -1);
			bSupport[0] = bSupport[1] = false;
			Off[H] = In * (R - 6.0) + FVector(0.0, 0.0, 190.0 - Half);     // (ahead, up, 6 cm off the face at the wall)
			Off[O] = -In * 30.0 + FVector(0.0, 0.0, 50.0 - Half);          // (low, behind the body, out of every reach)
		}
		else if (C.Kind == EWallKind::Lip || C.Kind == EWallKind::StepDown || C.Kind == EWallKind::StepUp || C.Kind == EWallKind::Ceiling || C.Kind == EWallKind::OpenApex)
		{
			// along the wall, off it: nothing in the way but what the case puts there
			Way = WallT;
			In = Way;
			Tan = FVector(-In.Y, In.X, 0.0);
			From = WallMid - WallIn * BandOff - WallT * (WallLen * 0.45);
			if (C.Angle != 0.0 && C.Kind != EWallKind::Ceiling)
			{
				// (the thing put in the way faces this much off the jump's line)
				In = Way * FMath::Cos(Rad) - FVector(-Way.Y, Way.X, 0.0) * FMath::Sin(Rad);
				Tan = FVector(-In.Y, In.X, 0.0);
				if (FVector::DotProduct(Tan, Way) < 0.0) Tan = -Tan;
			}
		}
		else
		{
			In = WallIn;
			Tan = WallT;
			Way = In * FMath::Cos(Rad) + Tan * FMath::Sin(Rad);
			FVector Mid = WallMid;
			if (C.Kind == EWallKind::LowWall)
			{
				// a wall as high as the mill's ledge, 2 m out from the real one
				const FVector At = WallMid - In * 200.0 + FVector(0.0, 0.0, C.High * 0.5);
				AddBox(At, FRotator(0.0, In.Rotation().Yaw, 0.0), FVector(20.0, 400.0, C.High * 0.5));
				Mid = FVector(At.X, At.Y, FloorZ) - In * 20.0;
			}
			if (C.Kind == EWallKind::Slope)
			{
				// a slab 2 m out from the real wall, its near face the wall, pitched about its long side
				const FVector At = WallMid - In * 200.0 + FVector(0.0, 0.0, 200.0);
				AddBox(At, FRotator(C.Lean, In.Rotation().Yaw, 0.0), FVector(20.0, 400.0, 300.0));
				Mid = FVector(At.X, At.Y, FloorZ) - In * 20.0;
			}
			From = Mid - In * R - Way * (C.Kind == EWallKind::Late || C.Kind == EWallKind::LowWall ? LateRun : C.Kind == EWallKind::WallApex ? 130.0 : RunUp);
		}
		From.Z = FloorZ + Half + 2.0;
		const FRotator Face(0.0, Way.Rotation().Yaw, 0.0);
		TeleportTo(From, Face, false, true);
		if (Controller) Controller->SetControlRotation(Face);
		M->StopMovementImmediately();
		M->SetMovementMode(MOVE_Walking);
		if (C.Kind == EWallKind::Ledge) PutHands();
		CaseT = 0.f;
		Frames = 0;
		AirFrames = 0.f;
		bFlew = bHit = bPlaced = bTook = bSqueezed = bBoostAfter = bSampled = false;
		HitT = PlaceT = -1.f;
		TopAfter = -1e9;
		IntoPrev = SpeedPrev = V0 = Peak = Slid = 0.0;
		S05 = S25 = Speed05 = S0 = -1.0;
		VzPrev = 0.0;
		Step = 1;
		return;
	}
	if (C.Kind == EWallKind::Ledge) PutHands();
	CaseT += Dt;
	Stamina = 100.f;
	bRunning = true;
	if (Step == 1)
	{
		if (CaseT < 0.3f) { M->Velocity = FVector::ZeroVector; return; }
		Step = 2;
	}
	HeldStick = StickFor(Way);
	HeldStickLeft = 0.2f;
	if (Step == 2)
	{
		// the run's speed and the stick's whole push (the headset's stick eases in) held on the spot: no room needed for a run-up
		if (Frames == 0) HoldAt = GetActorLocation();
		M->Velocity = Way * M->MaxWalkSpeed;
		if (++Frames < 4 || (M->GetAnalogInputModifier() < 0.98f && Frames < 120))
		{
			SetActorLocation(HoldAt, false, nullptr, ETeleportType::TeleportPhysics);
			return;
		}
		TakeOffZ = Feet.Z;
		Apex = Feet.Z;
		SpeedPrev = RunTop = M->MaxWalkSpeed;
		DoJump();
		CaseT = 0.f;
		Step = 3;
		return;
	}
	if (Step == 3)
	{
		const bool bFalling = M->IsFalling();
		const FVector Vh(M->Velocity.X, M->Velocity.Y, 0.0);
		const double Speed = Vh.Size(), Into = FVector::DotProduct(Vh, In), Side = FVector::DotProduct(Vh, Tan);
		bFlew = bFlew || bFalling;
		Apex = FMath::Max(Apex, double(Feet.Z));
		const double High = Feet.Z - FloorZ;
		if (C.Kind == EWallKind::Ledge && bFalling && ++AirFrames >= 2.f && !bSqueezed)
		{
			TestAxes.Add(H == 0 ? TEXT("OLVR_LeftGrip") : TEXT("OLVR_RightGrip"), 1.f);
			bSqueezed = true;
		}
		// what the case puts in the way, a few cm ahead of the body; the lip by the next frame's feet (a frame's fall is wider than
		// the height to meet it at), only as far ahead as the capsule's round reaches at its top, so it is met in the air
		const double HighNext = High + M->Velocity.Z * Dt;
		const bool bLand = M->Velocity.Z < 0.0 && (C.Kind == EWallKind::Lip ? HighNext <= C.High - 12.0 : C.Kind == EWallKind::StepDown && High <= C.High - 35.0);
		const bool bRise = C.Kind == EWallKind::StepUp && M->Velocity.Z > 0.0 && High >= 15.0;
		if (bFlew && !bPlaced && (bLand || bRise))
		{
			const double Under = FMath::Clamp(C.High - FMath::Max(0.0, HighNext), 0.0, double(R));      // (its top over the next feet)
			const double Near = C.Kind == EWallKind::Lip ? FMath::Sqrt(R * R - (R - Under) * (R - Under)) + Speed * Dt * 0.7 : R + 6.0 + Speed * Dt;
			const FVector At = FVector(Feet.X, Feet.Y, FloorZ + C.High * 0.5) + In * (Near + 30.0);
			AddBox(At, FRotator(0.0, In.Rotation().Yaw, 0.0), FVector(30.0, 200.0, C.High * 0.5));
			BoxFar = At + In * 30.0;
			bPlaced = true;
			PlaceT = CaseT;
			IntoPrev = 0.0;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT jumpwall   %s: put %.0f cm ahead, the feet %.0f over the floor, %.0f cm/s across, %.0f up"), KindName(C.Kind), Near, High, Speed, M->Velocity.Z);
		}
		if (bPlaced) TopAfter = FMath::Max(TopAfter, double(Feet.Z));
		if (C.Kind == EWallKind::Ceiling && bFlew && !bPlaced && M->Velocity.Z > 100.0 && CaseT > 0.05f)
		{
			AddBox(GetActorLocation() + FVector(0.0, 0.0, Half + 16.0), FRotator(0.0, In.Rotation().Yaw, 0.0), FVector(400.0, 400.0, 10.0));
			bPlaced = true;
		}
		if (bFlew && !bHit)
		{
			const bool bCeil = C.Kind == EWallKind::Ceiling;
			if (bCeil ? bPlaced && M->Velocity.Z < VzPrev * 0.5 && VzPrev > 30.0 : IntoPrev > 30.0 && Into < IntoPrev * 0.5)
			{
				bHit = true;
				HitT = CaseT;
				V0 = SpeedPrev;
				HitFeet = Feet;
				S0 = FMath::Abs(Side);
			}
			IntoPrev = Into;
			SpeedPrev = Speed;
		}
		VzPrev = M->Velocity.Z;
		if (bHit)
		{
			const float Since = CaseT - HitT;
			if (Since > 0.f && Since <= 0.3f) Peak = FMath::Max(Peak, FMath::Abs(Side));
			if (S05 < 0.0 && Since >= 0.05f) { S05 = FMath::Abs(Side); Speed05 = Speed; bBoostAfter = JumpAirSpeed > 0.f; bSampled = true; }
			if (S25 < 0.0 && Since >= 0.25f) { S25 = FMath::Abs(Side); Slid = FMath::Abs(FVector::DotProduct(Feet - HitFeet, Tan)); }
		}
		bTook = bTook || (IsClimbing() && GrabHand == H);
		const bool bDown = bFlew && !bFalling;
		bool bEnd = CaseT > 3.5f;
		const bool bTail = C.Kind == EWallKind::Lip || C.Kind == EWallKind::StepDown || C.Kind == EWallKind::StepUp;
		if (C.Kind == EWallKind::OpenApex || C.Kind == EWallKind::WallApex) bEnd = bEnd || bDown;
		else if (C.Kind == EWallKind::Ledge) bEnd = bEnd || bTook || (bDown && CaseT > 0.1f);
		else if (bTail) bEnd = bEnd || (bPlaced && CaseT > PlaceT + 1.2f);
		else bEnd = bEnd || (bHit && CaseT >= HitT + 0.3f) || (bDown && bHit);
		if (!bEnd) return;
		Step = 4;
	}
	if (Step == 4)
	{
		const double Rad = FMath::DegreesToRadians(FMath::Abs(C.Angle));
		const double Left = V0 * FMath::Sin(Rad);
		const bool bBoost = bSampled ? bBoostAfter : JumpAirSpeed > 0.f;
		const FString Id = FString::Printf(TEXT("%s at %.0f deg, %.0f %%%s"), KindName(C.Kind), C.Angle, C.Dial * 200.f, C.Lean != 0.0 ? *FString::Printf(TEXT(", pitched %.0f"), C.Lean) : TEXT(""));
		const float Held = JumpAirSpeed;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT jumpwall case %s: %.0f cm/s at the hit, sideways %.0f at it, %.0f at 0.05 s, %.0f at 0.25 s, peak %.0f, slid %.0f cm, %.0f cm/s across at 0.05 s, boost %s, apex %.0f cm"),
			*Id, V0, S0, S05, S25, Peak, Slid, Speed05, bBoost ? TEXT("held") : TEXT("over"), Apex - TakeOffZ);
		switch (C.Kind)
		{
		case EWallKind::Wall:
		case EWallKind::Slope:
		case EWallKind::Late:
		case EWallKind::LowWall:
		case EWallKind::WallApex:
		{
			Check(bHit, FString::Printf(TEXT("%s: hit the wall"), *Id));
			const bool bStop = !bOld && C.Angle <= Leeway;
			if (C.Kind == EWallKind::WallApex && !bOld)
			{
				Check(Apex - TakeOffZ >= OpenApexZ - 8.0, FString::Printf(TEXT("%s: as high as the open air's %.0f cm (%.0f)"), *Id, OpenApexZ, Apex - TakeOffZ));
			}
			if (bStop)
			{
				Check(!bBoost, FString::Printf(TEXT("%s: the boost is over"), *Id));
				// (the frame of the hit: what the engine left; after it the stick held along the wall adds its own, the walk's top speed at most)
				Check(S0 >= 0.0 && S0 <= Left * 1.05 + 40.0, FString::Printf(TEXT("%s: sideways at the hit what the engine left, %.0f (%.0f)"), *Id, Left, S0));
				Check(Peak <= FMath::Max(Left, RunTop) * 1.05, FString::Printf(TEXT("%s: after it no more than that or the run's top speed %.0f (%.0f)"), *Id, RunTop, Peak));
			}
			else if (C.Angle >= 15.0)
			{
				Check(bBoost, FString::Printf(TEXT("%s: the boost holds"), *Id));
				Check(S05 >= V0 * 0.85, FString::Printf(TEXT("%s: scaled back up along the wall, %.0f (%.0f)"), *Id, V0, S05));
			}
			break;
		}
		case EWallKind::Lip:
		case EWallKind::StepDown:
		{
			const bool bOver = FVector::DotProduct(Feet - BoxFar, In) > 0.0;
			Check(bPlaced && TopAfter >= FloorZ + C.High - 3.0, FString::Printf(TEXT("%s: rode up over its top (the feet to %.0f cm over the floor)"), *Id, TopAfter - FloorZ));
			Check(Held > 0.f, FString::Printf(TEXT("%s: the boost is kept"), *Id));
			Check(bOver, FString::Printf(TEXT("%s: over it (%.0f cm past its far side)"), *Id, FVector::DotProduct(Feet - BoxFar, In)));
			break;
		}
		case EWallKind::StepUp:
		{
			const bool bOver = FVector::DotProduct(Feet - BoxFar, In) > 0.0;
			Check(bPlaced && bHit, FString::Printf(TEXT("%s: met it on the rise"), *Id));
			if (!bOld)
			{
				Check(Held <= 0.f, FString::Printf(TEXT("%s: the boost is over"), *Id));
				Check(S0 >= 0.0 && S0 <= Left * 1.05 + 40.0, FString::Printf(TEXT("%s: sideways at the hit what the engine left, %.0f (%.0f)"), *Id, Left, S0));
				Check(Peak <= FMath::Max(Left, RunTop) * 1.05, FString::Printf(TEXT("%s: after it no more than that or the run's top speed %.0f (%.0f)"), *Id, RunTop, Peak));
			}
			Check(bOver, FString::Printf(TEXT("%s: over it (%.0f cm past its far side)"), *Id, FVector::DotProduct(Feet - BoxFar, In)));
			break;
		}
		case EWallKind::Ceiling:
			Check(bPlaced && bHit, FString::Printf(TEXT("%s: hit it"), *Id));
			Check(Held > 0.f, FString::Printf(TEXT("%s: the boost is kept"), *Id));
			Check(Speed05 >= V0 * 0.9, FString::Printf(TEXT("%s: the speed across kept, %.0f (%.0f)"), *Id, V0, Speed05));
			break;
		case EWallKind::OpenApex:
			OpenApexZ = Apex - TakeOffZ;
			Check(!bHit, FString::Printf(TEXT("%s: nothing in the way"), *Id));
			break;
		case EWallKind::Ledge:
			Check(bHit || bTook, FString::Printf(TEXT("%s: reached the wall"), *Id));
			if (!bOld) Check(bTook, FString::Printf(TEXT("%s: ended in the grab"), *Id));
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT jumpwall   %s: %s, %.0f cm along the wall from the hit, %.2f s after it"), *Id, bTook ? TEXT("grabbed") : TEXT("no grab"), bHit ? FMath::Abs(FVector::DotProduct(Feet - HitFeet, Tan)) : -1.0, HitT >= 0.f ? CaseT - HitT : -1.f);
			break;
		}
		HeldStickLeft = 0.f;
		HeldStick = FVector2D::ZeroVector;
		TestAxes.Reset();
		if (IsClimbing()) ClimbWheelTestLetGo(GrabHand);
		M->StopMovementImmediately();
		ClearBox();
		++CaseI;
		Step = 0;
	}
}

#endif // WITH_OUTLAWS_TESTS
