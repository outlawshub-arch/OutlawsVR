// REQ-STAMINA15 (user 2026-10-27, "out of breath in VR"): -OLStaminaProbe times the stamina rates, in any build (Shipping
// too, as -OLShipSmoke and -OLToolWhackProbe: through OLFlags, not under WITH_OUTLAWS_TESTS; nothing runs without the flag).
// First StaminaRate stepped as UpdateStamina steps it (a 72 Hz frame, kept in 0..100) under both rules, the new one and
// -OLOldStamina's: a run from full to spent at the speed dial's 50 %, 100 % (the default) and 150 % (its top), at the
// teleport pace (0.5), at pace 1.0 (past the dial's top today) and carrying a man; the rests from spent to full standing,
// walking, crouched and crouched walking; the jumps from full to spent.  Then, once the level has been up 3 s, the player
// standing still (invincible meanwhile) is spent and the game's own tick (UpdateStamina) timed until full, against the
// stepped time of the rule in use.  Lines "OLSTAMINA ..." and "OLSTAMINA RESULT PASS|FAIL".  With -OLShipSmoke the level
// is played until it is done (OutlawsShipSmoke::HoldLevel); without it the game quits once done.  See OutlawsPlayer.h.

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsLevelActor.h"
#include "OutlawsShipSmoke.h"
#include "Components/CapsuleComponent.h"
#include "HAL/PlatformMisc.h"

namespace
{
	constexpr float StaminaStepDt = 1.f / 72.f;     // (the Quest 2's frame)
	constexpr float StaminaLead = 3.f;              // the level's seconds before the timed rest

	struct FStaminaProbe
	{
		int32 Stage = 0;            // 0: the level awaited; 1: the rest timed on the tick; 2: done
		float T = 0.f;              // the level's seconds since the probe first saw it
		float RestT = 0.f;          // the timed rest so far
		float Expect = 0.f;         // its stepped time
		bool bCrouchedAtStart = false;
		bool bWasInvincible = false;
		FString Disturbed;          // what moved the player or stopped the tick during the timed rest
		int32 Pass = 0, Fail = 0;
		TWeakObjectPtr<AOutlawsLevelActor> Level;
	};
	FStaminaProbe GStamina;
}

bool AOutlawsPlayer::StaminaProbeOn()
{
	static const bool bOn = OLFlags::Param(TEXT("OLStaminaProbe"));
	return bOn;
}

void AOutlawsPlayer::TickStaminaProbe(float Dt)
{
	FStaminaProbe& P = GStamina;
	if (P.Stage >= 2) return;
	AOutlawsLevelActor* L = Level.Get();
	const bool bCrouched = GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() < 60.f;
	auto Check = [&P](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSTAMINA %s: %s"), *What, bOk ? TEXT("PASS") : TEXT("FAIL"));
		if (bOk) ++P.Pass; else ++P.Fail;
	};
	// seconds from From until the stamina reaches To at the case's rate, stepped and kept in 0..100 as UpdateStamina does
	auto StepTime = [](float From, float To, bool bRun, bool bMoving, bool bCrouch, float Pace, bool bCarry, bool bOld)
	{
		const float Rate = StaminaRate(bRun, bMoving, bCrouch, Pace, bCarry, bOld);
		if ((To < From && Rate >= 0.f) || (To > From && Rate <= 0.f)) return -1.f;
		float S = From;
		int32 N = 0;
		while (N < 10000000 && (To < From ? S > To : S < To))
		{
			S = FMath::Clamp(S + Rate * StaminaStepDt, 0.f, 100.f);
			++N;
		}
		return float(N) * StaminaStepDt;
	};
	if (P.Stage == 0)
	{
		if (!L || L->IsFrontEnd() || L->IsRebuildPending() || L->IsMoviePlaying() || L->IsPlayerDead() || bMenuOpen || bDeadView) return;
		if (P.Level.Get() != L)
		{
			P.Level = L;
			P.T = 0.f;
			UE_LOG(LogOutlaws, Display, TEXT("OLSTAMINA %s: probe armed, the rule in use %s; the dial %.0f %%, pace %.3f"), *L->LevelName,
				OldStamina() ? TEXT("old (-OLOldStamina)") : TEXT("new (REQ-STAMINA15)"), L->GameSpeed * 200.f, L->Pace());
		}
		OutlawsShipSmoke::HoldLevel(3.0);      // (-OLShipSmoke: this level is played on until the probe is done)
		P.T += Dt;
		if (P.T < StaminaLead) return;
		// the rates stepped, both rules: a run from full to spent (1.5 times as long now), the rests from spent to full (twice as fast)
		struct FPaceCase { const TCHAR* Name; float Pace; bool bCarry; };
		const FPaceCase Runs[] = {
			{ TEXT("the dial at 50 %"), AOutlawsLevelActor::SpeedFromDial(0.25f) * 0.5f, false },
			{ TEXT("the dial at 100 % (the default)"), AOutlawsLevelActor::SpeedFromDial(0.5f) * 0.5f, false },
			{ TEXT("the dial at 150 % (its top)"), AOutlawsLevelActor::SpeedFromDial(AOutlawsLevelActor::MaxGameSpeed) * 0.5f, false },
			{ TEXT("the teleport pace"), 0.5f, false },
			{ TEXT("pace 1.0 (past the dial's top)"), 1.f, false },
			{ TEXT("the dial at 100 %, carrying a man"), AOutlawsLevelActor::SpeedFromDial(0.5f) * 0.5f, true },
		};
		for (const FPaceCase& C : Runs)
		{
			const float Old = StepTime(100.f, 0.f, true, true, false, C.Pace, C.bCarry, true);
			const float New = StepTime(100.f, 0.f, true, true, false, C.Pace, C.bCarry, false);
			const float K = Old > 0.f ? New / Old : 0.f;
			Check(FMath::IsNearlyEqual(K, 1.5f, 0.01f), FString::Printf(TEXT("run, %s (pace %.3f): full to spent %.1f s before, %.1f s now, %.2f times as long"),
				C.Name, C.Pace, Old, New, K));
		}
		struct FRestCase { const TCHAR* Name; bool bMoving; bool bCrouch; };
		const FRestCase Rests[] = {
			{ TEXT("standing still"), false, false }, { TEXT("walking"), true, false },
			{ TEXT("crouched, still"), false, true }, { TEXT("crouched, walking"), true, true },
		};
		for (const FRestCase& C : Rests)
		{
			const float Old = StepTime(0.f, 100.f, false, C.bMoving, C.bCrouch, 0.5f, false, true);
			const float New = StepTime(0.f, 100.f, false, C.bMoving, C.bCrouch, 0.5f, false, false);
			const float K = New > 0.f ? Old / New : 0.f;
			Check(FMath::IsNearlyEqual(K, 2.f, 0.02f), FString::Printf(TEXT("rest, %s: spent to full %.1f s before (%.2f a second), %.1f s now (%.2f a second), %.2f times as fast"),
				C.Name, Old, StaminaRate(false, C.bMoving, C.bCrouch, 0.5f, false, true), New, StaminaRate(false, C.bMoving, C.bCrouch, 0.5f, false, false), K));
		}
		for (const bool bCarry : { false, true })
		{
			const float OldCost = StaminaJumpCost(bCarry, true), NewCost = StaminaJumpCost(bCarry, false);
			const int32 OldN = FMath::CeilToInt(100.f / OldCost - 0.0001f), NewN = FMath::CeilToInt(100.f / NewCost - 0.0001f);
			Check(FMath::IsNearlyEqual(OldCost / NewCost, 1.5f, 0.001f), FString::Printf(TEXT("jumps%s: %.3f each before, %.3f now; full to spent in %d jumps before, %d now"),
				bCarry ? TEXT(", carrying a man") : TEXT(""), OldCost, NewCost, OldN, NewN));
		}
		// the game's own tick: spent, standing still, timed until full (the rule in use)
		P.bCrouchedAtStart = bCrouched;
		P.Expect = StepTime(0.f, 100.f, false, false, bCrouched, L->Pace(), false, OldStamina());
		P.bWasInvincible = L->bInvincible;
		L->bInvincible = true;
		Stamina = 0.f;
		P.RestT = Dt;          // (this frame's UpdateStamina, after this, is the first of the rest)
		P.Stage = 1;
		UE_LOG(LogOutlaws, Display, TEXT("OLSTAMINA %s: spent, standing still%s; the tick's rest timed (%.1f s stepped)"), *L->LevelName,
			bCrouched ? TEXT(", crouched") : TEXT(""), P.Expect);
		return;
	}
	// Stage 1: the timed rest (the stamina here is as the frames before left it)
	OutlawsShipSmoke::HoldLevel(3.0);
	if (!L || L != P.Level.Get())
	{
		Check(false, TEXT("the timed rest: the level went"));
		P.Stage = 2;
	}
	else
	{
		if (P.Disturbed.IsEmpty())
		{
			if (GetVelocity().Size2D() > 60.f) P.Disturbed = TEXT("the player moved");
			else if (bCrouched != P.bCrouchedAtStart) P.Disturbed = TEXT("the crouch changed");
			else if (bMenuOpen || bDeadView || L->IsMoviePlaying() || L->IsRebuildPending()) P.Disturbed = TEXT("the tick was held (a menu, a death, a movie or a load)");
		}
		if (Stamina >= 100.f || P.RestT > P.Expect * 3.f + 5.f)
		{
			const bool bOk = P.Disturbed.IsEmpty() && FMath::Abs(P.RestT - P.Expect) <= P.Expect * 0.05f + 0.1f;
			Check(bOk, FString::Printf(TEXT("the tick's rest, %s, standing still%s: spent to full in %.2f s (stepped %.2f s)%s%s"), *L->LevelName,
				P.bCrouchedAtStart ? TEXT(", crouched") : TEXT(""), P.RestT, P.Expect, Stamina >= 100.f ? TEXT("") : TEXT(", never full"),
				P.Disturbed.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", %s"), *P.Disturbed)));
			L->bInvincible = P.bWasInvincible;
			P.Stage = 2;
		}
		else
		{
			P.RestT += Dt;
			return;
		}
	}
	UE_LOG(LogOutlaws, Display, TEXT("OLSTAMINA RESULT %s: %d passed, %d failed (the rule in use %s)"), P.Fail == 0 && P.Pass > 0 ? TEXT("PASS") : TEXT("FAIL"),
		P.Pass, P.Fail, OldStamina() ? TEXT("old, -OLOldStamina") : TEXT("new"));
	if (!OutlawsShipSmoke::Active()) FPlatformMisc::RequestExit(false);
}
