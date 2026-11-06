// REQ-RUNHEAL (user 5 Nov 2026: health comes back running too, at half the walking rate): -OLRunHealProbe [-OLOldRunHeal], in any
// build (through OLFlags, as -OLStaminaProbe).  Part one in a single frame, the real UpdateStamina stepped at 72 Hz on the body
// set as each case wants it: from next to nothing up to the ceiling running at the speed dial's 40, 100 and 150 %, 5 s past the
// ceiling; then the rate from 2 of 10 standing, walking, crouched and crouched walking, running spent, crouched running, in the
// air, swimming.  A co-op client or match client: the session's rate, then that copy doubled by hand, to show it is the
// session's that is used.  Part two on real frames: the body run down the longest way from the spawn for 10 s at each dial,
// taken back to the start of it as it nears the wall, and 5 s standing and walking.  A hit in the middle: that measure again.
// -OLRunHealLead=<s> the seconds to wait, -OLRunHealStay: no quit, the script ends it.
// Lines "OLSHOT runheal ...", "OLSHOT runheal check PASS|FAIL: ..." and "OLSHOT runheal RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsCoop.h"
#include "OutlawsFeel.h"
#include "OutlawsFlags.h"
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
	constexpr float HealStepDt = 1.f / 72.f;
	struct FLive { const TCHAR* Name; float Dial; bool bRun, bMove; float Seconds; };
	const FLive GLive[] = {
		{ TEXT("running, the dial at 40 %"), 0.2f, true, true, 10.f }, { TEXT("running, the dial at 100 %"), 0.5f, true, true, 10.f },
		{ TEXT("running, the dial at 150 %"), 0.75f, true, true, 10.f }, { TEXT("walking"), 0.5f, false, true, 5.f }, { TEXT("standing"), 0.5f, false, false, 5.f },
	};
	struct FHealProbe
	{
		int32 Stage = 0, Case = 0, Step = 0, Pass = 0, Fail = 0, Tries = 0;
		float T = 0.f, At = 0.f, Run = 0.f, Sum = 0.f, PrevDt = 0.f, H0 = 0.f, Prev = 0.f, DialWas = 0.5f;
		bool bGood = false;         // (the last frame) the body doing what the case says
		FVector Home = FVector::ZeroVector, Dir = FVector::ForwardVector;
		double Room = 0.0;
		bool bWasInvincible = false;
	};
	FHealProbe GHeal;
}

bool AOutlawsPlayer::RunHealProbeOn()
{
	static const bool bOn = OLFlags::Param(TEXT("OLRunHealProbe"));
	return bOn;
}

void AOutlawsPlayer::TickRunHealProbe(float Dt)
{
	FHealProbe& P = GHeal;
	if (P.Stage >= 3 || !IsLocallyControlled()) return;
	AOutlawsLevelActor* L = Level.Get();
	if (!L || L->IsFrontEnd() || L->IsRebuildPending() || L->IsMoviePlaying() || bMenuOpen) return;
	L->StartSaveDelay = -1.f;           // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	UCharacterMovementComponent* M = GetCharacterMovement();
	static const bool bOldRule = OLFlags::Param(TEXT("OLOldRunHeal"));
	const float Rate = RegenRate();
	const float RunShare = bOldRule ? 0.f : 0.125f;
	auto Check = [&P](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT runheal check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++P.Pass; else ++P.Fail;
	};
	P.T += Dt;
	if (bDeadView)
	{
		if (P.T > P.At + 1.f) { P.At = P.T; RequestRespawn(); }
		P.Step = 0;
		return;
	}
	if (P.Stage == 0)
	{
		static const float Lead = [] { float V = 3.f; FParse::Value(FCommandLine::Get(), TEXT("OLRunHealLead="), V); return V; }();
		if (P.T < Lead || !M->IsMovingOnGround()) return;
		P.bWasInvincible = L->bInvincible;
		L->bInvincible = true;
		P.DialWas = L->GameSpeed;
		P.Home = GetActorLocation();
		for (int32 I = 0; I < 16; ++I)
		{
			const FVector D = FRotator(0.0, I * 22.5, 0.0).Vector();
			FHitResult Hit;
			const FVector A = P.Home + FVector(0.0, 0.0, 10.0);
			const bool bHit = GetWorld()->LineTraceSingleByChannel(Hit, A, A + D * 3000.0, ECC_Visibility);
			const double Far = bHit ? Hit.Distance : 3000.0;
			if (Far > P.Room) { P.Room = Far; P.Dir = D; }
		}
		UOutlawsCoop* Co = UOutlawsCoop::Get(this);
		const bool bClient = Co && Co->IsClient();
		AOutlawsCoopSession* Session = Co ? Co->GetSession() : nullptr;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT runheal start: %s, %s, %s, the rate in use %.3f a second (this build's %.3f, the session's %.3f), %.0f cm of room"), *L->LevelName,
			bClient ? TEXT("a client") : Session ? TEXT("the host") : TEXT("alone"), bOldRule ? TEXT("-OLOldRunHeal") : TEXT("new"), Rate, OutlawsFeel::RegenPerSecond, Session ? Session->RegenRate : -1.f, P.Room);

		// part one: UpdateStamina itself on the body as set, from health From until To or the seconds run out
		const FVector Fwd = P.Dir;
		const float CapHalf = GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
		const float CapR = GetCapsuleComponent()->GetUnscaledCapsuleRadius();
		const bool bRunWas = bRunning, bSwimWas = bSwimming;
		auto Stepped = [&](float From, float To, float MaxSeconds, bool bRun, float Speed, bool bCrouch, bool bSpent, EMovementMode Mode, float& EndHealth)
		{
			L->TestSetHealth(From);
			bRunning = bRun;
			M->SetMovementMode(Mode);
			if (bCrouch) GetCapsuleComponent()->SetCapsuleSize(CapR, 50.f, false);
			float T = 0.f;
			while (T < MaxSeconds && L->GetHealth() < To)
			{
				M->Velocity = Fwd * Speed;
				Stamina = bSpent ? 0.f : 100.f;
				TiredWait = 10.f;
				UpdateStamina(HealStepDt);
				T += HealStepDt;
			}
			EndHealth = L->GetHealth();
			if (bCrouch) GetCapsuleComponent()->SetCapsuleSize(CapR, CapHalf, false);
			return T;
		};
		float End = 0.f;
		auto Share = [&](bool bRun, float Speed, bool bCrouch, bool bSpent, EMovementMode Mode)
		{
			const float T = Stepped(2.f, 3.f, 10.f, bRun, Speed, bCrouch, bSpent, Mode, End);
			return T > 0.f ? (End - 2.f) / T / Rate : 0.f;
		};
		const float RunSpeed = 432.f;
		for (const float Dial : { 0.2f, 0.5f, 0.75f })
		{
			L->GameSpeed = Dial;
			const float T = Stepped(0.01f, OutlawsFeel::RegenCap, 260.f, true, RunSpeed * Dial / 0.5f, false, false, MOVE_Walking, End);
			const float Want = RunShare > 0.f ? (OutlawsFeel::RegenCap - 0.01f) / (Rate * RunShare) : 260.f;
			Check(FMath::Abs(T - Want) <= Want * 0.05f, FString::Printf(TEXT("running at the dial's %.0f %%, next to nothing to %.2f of 10: %.1f s (%.1f wanted)"), Dial * 200.f, End, T, Want));
			if (!bOldRule)
			{
				float Past = 0.f;
				Stepped(OutlawsFeel::RegenCap, 99.f, 5.f, true, RunSpeed * Dial / 0.5f, false, false, MOVE_Walking, Past);
				Check(FMath::IsNearlyEqual(Past, OutlawsFeel::RegenCap, 0.001f), FString::Printf(TEXT("5 s running past the ceiling: %.3f of 10"), Past));
			}
		}
		L->GameSpeed = 0.5f;
		struct FRow { const TCHAR* Name; bool bRun; float Speed; bool bCrouch, bSpent; EMovementMode Mode; float Want; bool bSwim; };
		const FRow Rows[] = {
			{ TEXT("standing"), false, 0.f, false, false, MOVE_Walking, 1.f, false },
			{ TEXT("walking"), false, 200.f, false, false, MOVE_Walking, 0.25f, false },
			{ TEXT("crouched, still"), false, 0.f, true, false, MOVE_Walking, 2.f, false },
			{ TEXT("crouched, walking"), false, 100.f, true, false, MOVE_Walking, 0.5f, false },
			{ TEXT("running"), true, 432.f, false, false, MOVE_Walking, RunShare, false },
			{ TEXT("running, spent"), true, 200.f, false, true, MOVE_Walking, RunShare, false },
			{ TEXT("crouched, running"), true, 200.f, true, false, MOVE_Walking, RunShare * 2.f, false },
			{ TEXT("in the air, moving"), true, 432.f, false, false, MOVE_Falling, 0.f, false },
			{ TEXT("swimming"), false, 200.f, false, false, MOVE_Flying, 0.f, true },
		};
		for (const FRow& Row : Rows)
		{
			bSwimming = Row.bSwim;
			const float Got = Share(Row.bRun, Row.Speed, Row.bCrouch, Row.bSpent, Row.Mode);
			Check(FMath::IsNearlyEqual(Got, Row.Want, 0.002f), FString::Printf(TEXT("%s: %.4f of the rate, %.3f %% a second (%.4f wanted)"), Row.Name, Got, Got * Rate * 10.f, Row.Want));
			bSwimming = false;
		}
		if (bClient && Session)
		{
			// the session's copy doubled here, as a host's other rate would come: running follows it
			const float Was = Session->RegenRate;
			Session->RegenRate = Was * 2.f;
			const float Got = Share(true, 432.f, false, false, MOVE_Walking);
			Check(FMath::IsNearlyEqual(Got, RunShare * 2.f, 0.002f) && FMath::IsNearlyEqual(RegenRate(), Was * 2.f, 0.0001f), FString::Printf(TEXT("a client runs at the host's rate: the session's copy at %.3f, %.4f of the rate it had"), RegenRate(), Got));
			Session->RegenRate = Was;
		}
		bSwimming = bSwimWas;
		bRunning = bRunWas;
		M->SetMovementMode(MOVE_Walking);
		M->Velocity = FVector::ZeroVector;
		L->GameSpeed = P.DialWas;
		P.Stage = 1;
		P.Step = 0;
		return;
	}
	if (P.Stage == 1)
	{
		// part two: real frames
		if (P.Case >= UE_ARRAY_COUNT(GLive))
		{
			P.Stage = 2;
			return;
		}
		const FLive& C = GLive[P.Case];
		if (P.Step == 0)
		{
			L->GameSpeed = C.Dial;
			bRunning = false;
			HeldStickLeft = 0.f;
			M->Velocity = FVector::ZeroVector;
			TeleportTo(P.Home, FRotator(0.0, GetActorRotation().Yaw, 0.0), false, true);
			L->TestSetHealth(1.f);
			P.At = P.T;
			P.Step = 1;
			return;
		}
		FRotator From = !bVR ? GetControlRotation() : Camera->GetComponentRotation();
		From.Pitch = From.Roll = 0.0;
		const FVector F = From.Vector(), Right = FRotator(0.0, From.Yaw + 90.0, 0.0).Vector();
		if (C.bMove)
		{
			bRunning = C.bRun;
			HeldStick = FVector2D(FVector::DotProduct(P.Dir, Right), FVector::DotProduct(P.Dir, F));
			HeldStickLeft = 0.2f;
			// nearly at the end of the way: back to its start at the same speed
			if (FVector::DotProduct(GetActorLocation() - P.Home, P.Dir) > P.Room - 120.0)
			{
				const FVector Vel = M->Velocity;
				SetActorLocation(P.Home, false, nullptr, ETeleportType::TeleportPhysics);
				M->Velocity = Vel;
			}
		}
		if (P.Step == 1)
		{
			// up to speed, then the measure starts
			if (P.T < P.At + 0.8f) return;
			P.H0 = P.Prev = L->GetHealth();
			P.Run = P.Sum = 0.f;
			P.bGood = false;
			P.At = P.T;
			P.Step = 2;
			return;
		}
		const float H = L->GetHealth();
		if (H < P.Prev - 0.001f)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT runheal %s: hit in the middle of it, %.2f to %.2f, again"), C.Name, P.Prev, H);
			P.Step = 0;
			if (++P.Tries < 8) return;
			Check(false, FString::Printf(TEXT("%s: eight goes, each hit"), C.Name));
			P.Tries = 0;
			++P.Case;
			return;
		}
		// only the frames the body did what the case says: co-op's hideout and the match's town stop a run short of the longest way
		// (the first reading: 0 cm/s at a corner, the still rate for those frames), and the match's start respawns everyone
		if (P.bGood && H - P.Prev <= 0.05f) { P.Sum += H - P.Prev; P.Run += P.PrevDt; }
		P.Prev = H;
		P.PrevDt = Dt;
		P.bGood = M->IsMovingOnGround() && (C.bMove ? GetVelocity().Size2D() > 60.f : GetVelocity().Size() < 20.f);
		if (Stamina < 50.f) Stamina = 100.f;      // (a run's breath: not what this measures)
		if (P.Run < C.Seconds && P.T < P.At + C.Seconds * 3.f) return;
		const float Got = P.Run > 0.f ? P.Sum / P.Run : 0.f;
		const float Want = Rate * (C.bRun ? RunShare : C.bMove ? 0.25f : 1.f);
		Check(P.Run >= C.Seconds * 0.6f && FMath::Abs(Got - Want) <= FMath::Max(Want * 0.05f, 0.002f), FString::Printf(TEXT("live, %s: %.2f to %.2f, %.2f gained in %.2f s of %.2f, %.4f a second (%.4f wanted), %.0f cm/s"),
			C.Name, P.H0, H, P.Sum, P.Run, P.T - P.At, Got, Want, GetVelocity().Size2D()));
		P.Step = 0;
		P.Tries = 0;
		++P.Case;
		return;
	}
	bRunning = false;
	HeldStickLeft = 0.f;
	L->TestSetHealth(10.f);
	L->GameSpeed = P.DialWas;
	L->bInvincible = P.bWasInvincible;
	P.Stage = 3;
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT runheal RESULT %s (%d passed, %d failed, %s)"), P.Fail == 0 && P.Pass > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), P.Pass, P.Fail, bOldRule ? TEXT("-OLOldRunHeal") : TEXT("new"));
	if (!OutlawsShipSmoke::Active() && !OLFlags::Param(TEXT("OLRunHealStay"))) FPlatformMisc::RequestExit(false);
}
