// REQ-JUMPSTAMINA (user 5 Nov 2026: "every jump should take about 5 to 10 %", then 5 %, the original's E_JUMP; no refill in a
// jump's air time): -OLJumpStaminaProbe[=vr] [-OLOldJumpStamina], best with -OLShots -OLNoFrontEnd -OLLevel=mill -OLWristTest, in
// any build (through OLFlags, as -OLStaminaProbe).  The real take-off, real frames: =vr the headset's rules and fall.  From the
// body's spawn: a standing jump, a walking one, a sprinting one at the speed dial's 40, 100 and 150 %, a jump held through the
// landing, three hops, a jump at empty and at 3 %, a body thrown up with no jump (a fall), and on the mill a real walk off walkway
// 572; the wrist tube across a jump.  A case read frame by frame from the press to the first frame on the ground: the drops (a
// frame's fall of over 0.3 points), the net landing to landing, the air time, the rise, the jumps taken (TutNotes).  A case a
// hit got into (health down, a death) is played again, up to 3 times, and says so.  In co-op and matches the same cases run
// on the local player of each side, -OLJumpStaminaLead=<s> the seconds to wait, -OLJumpStaminaStay: no quit, the script ends it.
// Lines "OLSHOT jumpstamina <case>: ...", "OLSHOT jumpstamina check PASS|FAIL: ..." and "OLSHOT jumpstamina RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsGeometry.h"
#include "OutlawsLevelActor.h"
#include "OutlawsShipSmoke.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "HAL/PlatformMisc.h"
#include "MotionControllerComponent.h"
#include "Engine/World.h"

namespace
{
	enum class EJSAct : uint8 { Jump, Held, Hops, Thrown, WalkOff };
	struct FJSCase
	{
		const TCHAR* Name = TEXT("");
		EJSAct Act = EJSAct::Jump;
		float Start = 60.f;         // the stamina at the press
		bool bMove = false, bRun = false;
		float Dial = 0.5f;          // the speed setting (0.5 shows as 100 %)
		bool bGauge = false;
	};
	struct FJSProbe
	{
		TArray<FJSCase> Cases;
		int32 CaseI = -1, Step = 0, Stage = 0, Hop = 0, Tries = 0, Drops = 0, Jumps0 = 0, Pass = 0, Fail = 0;
		float T = 0.f, At = 0.f, Air = 0.f, S0 = 0.f, Prev = 0.f, DropSum = 0.f, DropMax = 0.f, Health0 = 0.f, Gauge0 = -1.f, DialWas = 0.5f;
		double Z0 = 0.0, Top = 0.0, FullRise = 0.0, Reach = 0.0;
		float PressFrame = 0.f, OnGround = 0.f;      // the press's own frame on the ground (its rate, before the take-off), and all of those so far
		bool bInAir = false, bHeldDown = false, bWasInvincible = false, bWasVR = false;
		FVector Home = FVector::ZeroVector, Dir = FVector::ForwardVector;
		FRotator Face = FRotator::ZeroRotator;
	};
	FJSProbe GJS;
}

bool AOutlawsPlayer::JumpStaminaProbeOn()
{
	static const bool bOn = OLFlags::Has(TEXT("OLJumpStaminaProbe"));
	return bOn;
}

void AOutlawsPlayer::TickJumpStaminaProbe(float Dt)
{
	FJSProbe& P = GJS;
	if (P.Stage >= 2 || !IsLocallyControlled()) return;
	AOutlawsLevelActor* L = Level.Get();
	if (!L || L->IsFrontEnd() || L->IsRebuildPending() || L->IsMoviePlaying() || bMenuOpen) return;
	L->StartSaveDelay = -1.f;           // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	UCharacterMovementComponent* M = GetCharacterMovement();
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const float R = GetCapsuleComponent()->GetScaledCapsuleRadius();
	const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, Half);
	static const bool bOldRule = OLFlags::Param(TEXT("OLOldJumpStamina"));
	const float Want = !bOldRule ? 5.f : OldStamina() ? 1.25f : 1.25f / 1.5f;
	auto Check = [&P](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT jumpstamina check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++P.Pass; else ++P.Fail;
	};
	auto StickFor = [&](const FVector& WantDir)
	{
		FRotator From = !bVR ? GetControlRotation()
			: MoveDirection == EOutlawsMoveDirection::OffHand ? OffAim()->GetComponentRotation()
			: MoveDirection == EOutlawsMoveDirection::DominantHand ? DominantAim()->GetComponentRotation()
			: Camera->GetComponentRotation();
		From.Pitch = From.Roll = 0.0;
		const FVector Fwd = From.Vector(), Right = FRotator(0.0, From.Yaw + 90.0, 0.0).Vector();
		return FVector2D(FVector::DotProduct(WantDir, Right), FVector::DotProduct(WantDir, Fwd));
	};
	// the frame of a press still ticks on the ground, its own rate, before the take-off's drain (the first reading: 4.91 to 5.04 a jump, three
	// hops 14.78): left out of the jump's numbers, on the new rule; the old one drains on the press itself and is read as it comes
	auto PressFrameRate = [&]()
	{
		const bool bMoves = GetVelocity().Size2D() > 60.f;
		P.PressFrame = bOldRule ? 0.f : StaminaRate(bRunning && bMoves, bMoves, false, L->Pace(), false, OldStamina()) * Dt;
		P.OnGround += P.PressFrame;
	};
	P.T += Dt;
	if (P.CaseI < 0)
	{
		static const float Lead = [] { float V = 3.f; FParse::Value(FCommandLine::Get(), TEXT("OLJumpStaminaLead="), V); return V; }();
		if (P.T < Lead || !M->IsMovingOnGround()) return;
		FString Mode;
		FParse::Value(FCommandLine::Get(), TEXT("OLJumpStaminaProbe="), Mode);
		P.bWasVR = bVR;
		if (Mode == TEXT("vr")) { bVR = true; BaseGravityScale = 1.5f; }
		P.bWasInvincible = L->bInvincible;
		L->bInvincible = true;
		P.DialWas = L->GameSpeed;
		P.Home = GetActorLocation();
		P.Face = FRotator(0.0, GetActorRotation().Yaw, 0.0);
		// the way with the most room, for the running cases
		double Best = -1.0;
		for (int32 I = 0; I < 16; ++I)
		{
			const FVector D = FRotator(0.0, I * 22.5, 0.0).Vector();
			FHitResult Hit;
			const FVector A = P.Home + FVector(0.0, 0.0, 10.0);
			const bool bHit = GetWorld()->LineTraceSingleByChannel(Hit, A, A + D * 3000.0, ECC_Visibility);
			const double Far = bHit ? Hit.Distance : 3000.0;
			if (Far > Best) { Best = Far; P.Dir = D; }
		}
		P.Cases.Add({ TEXT("a standing jump"), EJSAct::Jump });
		P.Cases.Add({ TEXT("a walking jump"), EJSAct::Jump, 60.f, true, false });
		P.Cases.Add({ TEXT("a sprinting jump, the dial at 100 %"), EJSAct::Jump, 60.f, true, true, 0.5f });
		P.Cases.Add({ TEXT("a sprinting jump, the dial at 40 %"), EJSAct::Jump, 60.f, true, true, 0.2f });
		P.Cases.Add({ TEXT("a sprinting jump, the dial at 150 %"), EJSAct::Jump, 60.f, true, true, 0.75f });
		P.Cases.Add({ TEXT("a jump held through the landing"), EJSAct::Held });
		P.Cases.Add({ TEXT("three hops"), EJSAct::Hops });
		P.Cases.Add({ TEXT("a jump at empty"), EJSAct::Jump, 0.f });
		P.Cases.Add({ TEXT("a jump at 3 %"), EJSAct::Jump, 3.f });
		P.Cases.Add({ TEXT("thrown up, no jump (a fall)"), EJSAct::Thrown });
		if (L->LevelName == TEXT("mill")) P.Cases.Add({ TEXT("a walk off the walkway"), EJSAct::WalkOff });
		P.Cases.Add({ TEXT("the wrist tube across a jump"), EJSAct::Jump, 80.f, false, false, 0.5f, true });
		P.CaseI = 0;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT jumpstamina start: %s, %s, %s, %d cases, %.0f cm of room, a jump to take %.3f"), *L->LevelName, bVR ? TEXT("vr") : TEXT("flat"),
			bOldRule ? TEXT("-OLOldJumpStamina") : TEXT("new"), P.Cases.Num(), Best, Want);
	}
	if (P.CaseI >= P.Cases.Num())
	{
		L->GameSpeed = P.DialWas;
		L->bInvincible = P.bWasInvincible;
		P.Stage = 2;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT jumpstamina RESULT %s (%d passed, %d failed, %s, %s)"), P.Fail == 0 && P.Pass > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), P.Pass, P.Fail,
			bVR ? TEXT("vr") : TEXT("flat"), bOldRule ? TEXT("-OLOldJumpStamina") : TEXT("new"));
		if (!OutlawsShipSmoke::Active() && !OLFlags::Param(TEXT("OLJumpStaminaStay"))) FPlatformMisc::RequestExit(false);
		return;
	}
	const FJSCase& C = P.Cases[P.CaseI];
	if (bDeadView)
	{
		if (P.T > P.At + 1.f) { P.At = P.T; RequestRespawn(); }
		P.Step = 0;
		return;
	}
	const bool bKnocked = P.Step >= 3 && L->GetHealth() < P.Health0 - 0.01f;
	if (bKnocked)
	{
		// a hit in the middle of it (a match's bots): that case again
		if (P.bHeldDown) { DesktopJumpReleased(); P.bHeldDown = false; }
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT jumpstamina %s: hit in the middle of it, health %.1f to %.1f, again"), C.Name, P.Health0, L->GetHealth());
		if (++P.Tries < 3) { P.Step = 0; return; }
		Check(false, FString::Printf(TEXT("%s: three goes, each hit"), C.Name));
		P.Tries = 0;
		P.Step = 0;
		++P.CaseI;
		return;
	}
	const FVector Lip = FOutlawsGeometry::ToUE(308.0, 29.0, 16.0 - 7.0 * 40.0 / 59.0);
	const FVector In = FOutlawsGeometry::ToUE(0.561, 0.0, 0.8276).GetSafeNormal();
	const FVector Way = C.Act == EJSAct::WalkOff ? -In : P.Dir;
	if (P.Step == 0)
	{
		// on the spot, still, the stamina where the case wants it
		L->GameSpeed = C.Dial;
		bRunning = false;
		HeldStickLeft = 0.f;
		M->SetMovementMode(MOVE_Walking);
		M->Velocity = FVector::ZeroVector;
		const FVector Spot = C.Act == EJSAct::WalkOff ? FVector(Lip.X + In.X * (R + 40.0), Lip.Y + In.Y * (R + 40.0), Lip.Z + Half + 2.0) : P.Home;
		TeleportTo(Spot, P.Face, false, true);
		if (Controller) Controller->SetControlRotation(P.Face);
		Stamina = C.Start;
		P.At = P.T;
		P.Step = 1;
		return;
	}
	Stamina = P.Step == 1 ? C.Start : Stamina;
	if (P.Step == 1)
	{
		// settled, then the stick held for the run-up (the walk-off: on to the edge)
		const bool bMoving = C.bMove || C.Act == EJSAct::WalkOff;
		if (bMoving) { bRunning = C.bRun; HeldStick = StickFor(Way); HeldStickLeft = 0.2f; }
		if (bMoving ? P.T < P.At + 0.7f && C.Act != EJSAct::WalkOff : P.T < P.At + 0.3f || !M->IsMovingOnGround() || GetVelocity().Size() >= 20.f)
		{
			if (P.T < P.At + 4.f) return;
		}
		if (C.Act == EJSAct::WalkOff && (P.T < P.At + 0.3f || !M->IsFalling()) && P.T < P.At + 4.f) return;      // (past the settling, then over the edge)
		P.Hop = 0;
		P.Drops = 0;
		P.DropSum = P.DropMax = 0.f;
		P.Air = 0.f;
		P.bInAir = C.Act == EJSAct::WalkOff;
		P.S0 = P.Prev = Stamina;
		P.Z0 = P.Top = Feet.Z;
		P.Reach = 0.0;
		P.Jumps0 = TutNotes.Jumps;
		P.Health0 = L->GetHealth();
		P.Gauge0 = WristGaugeLevel;
		P.OnGround = P.PressFrame = 0.f;
		if (C.Act != EJSAct::Thrown && C.Act != EJSAct::WalkOff) PressFrameRate();
		if (C.Act == EJSAct::Held) { DesktopJumpPressed(); P.bHeldDown = true; }
		else if (C.Act == EJSAct::Thrown)
		{
			M->SetMovementMode(MOVE_Falling);
			M->Velocity = FVector(0.0, 0.0, FMath::Sqrt(2.0 * FMath::Abs(M->GetGravityZ()) * 80.0));
		}
		else if (C.Act != EJSAct::WalkOff) DoJump();
		P.At = P.T;
		P.Step = 3;
		return;
	}
	// from the press on, a frame at a time
	if (C.bMove) { bRunning = C.bRun; HeldStick = StickFor(Way); HeldStickLeft = 0.2f; }
	// (the old rule refills in the air: a long frame's refill hid a drop from the first reading, so it is added back; the cases' 0.74 to 0.87 for a 0.83)
	const float Back = bOldRule && Stamina < 100.f ? FMath::Max(0.f, StaminaRate(false, GetVelocity().Size2D() > 60.f, false, L->Pace(), false, OldStamina())) * Dt : 0.f;
	const float Fall = P.Prev - Stamina + Back + P.PressFrame;
	P.PressFrame = 0.f;
	if (Fall > 0.3f) { ++P.Drops; P.DropSum += Fall; P.DropMax = FMath::Max(P.DropMax, Fall); }
	P.Prev = Stamina;
	const bool bFalling = M->IsFalling();
	if (bFalling) { P.bInAir = true; P.Air += Dt; P.Top = FMath::Max(P.Top, Feet.Z); }
	if (P.Step == 3 && P.bInAir && !bFalling)
	{
		if (C.Act == EJSAct::Hops && ++P.Hop < 3)
		{
			P.bInAir = false;
			PressFrameRate();
			DoJump();
			P.At = P.T;
			return;
		}
		const float Net = P.S0 + P.OnGround - Stamina;
		const int32 Jumps = TutNotes.Jumps - P.Jumps0;
		const float Rise = float(P.Top - P.Z0);
		if (C.Start >= 60.f && C.Act == EJSAct::Jump && !C.bMove) P.FullRise = Rise;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT jumpstamina %s: at %.2f, %d drops (%.3f in all, the most %.3f), on the ground at %.2f: net %.3f; %.2f s in the air, %.3f a second back while up; %d jumps taken, rise %.0f cm%s"),
			C.Name, P.S0, P.Drops, P.DropSum, P.DropMax, Stamina, Net, P.Air, P.Air > 0.f ? (P.DropSum - Net) / P.Air : 0.f, Jumps, Rise, C.bMove ? *FString::Printf(TEXT(", off at %.0f cm/s"), M->Velocity.Size2D()) : TEXT(""));
		if (C.Act == EJSAct::Thrown || C.Act == EJSAct::WalkOff)
		{
			Check(Jumps == 0 && P.Drops == 0 && Net < 0.f, FString::Printf(TEXT("%s: nothing taken, the refill on as it was (%d jumps, %d drops, net %.3f over %.2f s)"), C.Name, Jumps, P.Drops, Net, P.Air));
		}
		else if (C.Act == EJSAct::Hops)
		{
			Check(Jumps == 3 && P.Drops == 3, FString::Printf(TEXT("three hops: one drop each (%d jumps, %d drops, %.3f in all)"), Jumps, P.Drops, P.DropSum));
			Check(FMath::Abs(P.DropSum - 3.f * Want) <= (bOldRule ? 0.45f : 0.15f), FString::Printf(TEXT("three hops: %.3f each (%.3f in all, %.3f wanted)"), Want, P.DropSum, 3.f * Want));
			if (!bOldRule) Check(FMath::Abs(Net - 15.f) <= 0.4f, FString::Printf(TEXT("three hops: net %.3f, 15 less the frames on the ground"), Net));
		}
		else if (C.Start < 5.f)
		{
			Check(Jumps == 1 && P.Air > 0.1f && Rise > 20.f, FString::Printf(TEXT("%s: the jump still goes (%d jumps, %.2f s up, rise %.0f cm, %.2f of a full jump's)"), C.Name, Jumps, P.Air, Rise, P.FullRise > 0.0 ? Rise / P.FullRise : 0.f));
			Check(Stamina >= 0.f && (bOldRule || Stamina <= 0.01f), FString::Printf(TEXT("%s: down to %.3f, not under 0, nothing back while up"), C.Name, Stamina));
		}
		else
		{
			Check(Jumps == 1 && P.Drops == 1 && FMath::Abs(P.DropSum - Want) <= (bOldRule ? 0.15f : 0.1f), FString::Printf(TEXT("%s: one drop of %.3f (%d jumps, %d drops, %.3f)"), C.Name, Want, Jumps, P.Drops, P.DropSum));
			if (!bOldRule) Check(FMath::Abs(Net - 5.f) <= 0.1f, FString::Printf(TEXT("%s: net 5 landing to landing (%.3f, %.2f s up)"), C.Name, Net, P.Air));
		}
		if (C.Act == EJSAct::Held) { P.Step = 4; P.At = P.T; return; }
		if (C.bGauge) { P.Step = 5; P.At = P.T; Stamina = P.S0 - Want; return; }
		bRunning = false;
		P.Step = 0;
		P.Tries = 0;
		++P.CaseI;
		return;
	}
	if (P.Step == 3)
	{
		if (P.T > P.At + 6.f)
		{
			Check(false, FString::Printf(TEXT("%s: done within 6 s of the press (in the air %d, %.2f s)"), C.Name, P.bInAir ? 1 : 0, P.Air));
			P.Step = 0;
			++P.CaseI;
		}
		return;
	}
	if (P.Step == 4)
	{
		// still held 0.8 s on the ground: no second jump by itself
		if (P.T < P.At + 0.8f) return;
		const int32 Jumps = TutNotes.Jumps - P.Jumps0;
		Check(Jumps == 1 && !M->IsFalling(), FString::Printf(TEXT("%s: one jump for the one press (%d, %d drops)"), C.Name, Jumps, P.Drops));
		DesktopJumpReleased();
		P.bHeldDown = false;
		P.Step = 0;
		P.Tries = 0;
		++P.CaseI;
		return;
	}
	// Step 5: the tube laid out again after the drop, the stamina held there
	Stamina = P.S0 - Want;
	if (P.T < P.At + 0.2f) return;
	const float Gauge1 = WristGaugeLevel;
	Check(!(P.Gauge0 >= 0.f) || bOldRule || Gauge1 < P.Gauge0 - 0.02f, FString::Printf(TEXT("the tube shows the drop: %.3f of its length before, %.3f after (stamina %.2f to %.2f)"), P.Gauge0, Gauge1, P.S0, Stamina));
	P.Step = 0;
	P.Tries = 0;
	++P.CaseI;
}
