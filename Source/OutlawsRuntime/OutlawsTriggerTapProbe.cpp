// BUG-TRIGGERTAP (user 2026-11-04, after a Quest test: guns slow to re-fire, mostly pistols; "I want to have each trigger push be
// a new shot, but I just want it to feel snappier"; holding must never auto-fire): -OLShots -OLLevel=hideout -OLNoFrontEnd
// -OLTriggerTapProbe [-OLOldTriggerTap].  The headset's rules (bVR) for the run, the trigger driven through TestAxes, a shot counted
// where its round goes (TutNotes.Shots).  Each of the five guns: (tap) a press, let go, a press inside the cycle, let go: two shots,
// the kept one at the cycle's end (old: the second press lost); (held) one pull held past three cycles: one shot; (held2) the press
// inside the cycle kept held: two shots, no more; (spam) a press every other frame: the cycle measured, "OLSHOT triggertap cycle".
// The revolver also: no stacking; 8 presses a second for 2 s; a dip to 0.45 re-arms, inside the cycle and after it (old: neither); a
// dip to 0.55 does not; a kept press goes with a swapped gun and with the REQ-SMASHTRIGGER guard; the left hand; the fan's 0.2, and
// the revolver's tunable never under it; the rifle's tunable.  The gatling and the knife as they were.  One line a check,
// "OLSHOT triggertap check PASS|FAIL: ...", and "OLSHOT triggertap RESULT".  BUG-TRIGGERTAP2: the revolver's cycle is the trial's
// 0.25 s, the other four the table's 0.5 s, each gun's checks against its own.

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Camera/CameraComponent.h"
#include "MotionControllerComponent.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLTriggerTapProbe) { return OLFlags::Param(TEXT("OLTriggerTapProbe")); }

OL_SHOT_TEST(OLTriggerTapProbe, 2380)
{
	typedef TFunction<bool()> FStep;     // true when done: the next step starts at once
	struct FCase
	{
		FString Name;
		int32 Gun = 0;
		int32 Hand = 1;
		TArray<FStep> Steps;
		TFunction<void()> Verify;
	};
	static const TCHAR* const GunName[] = { TEXT("Revolver"), TEXT("Rifle"), TEXT("Shotgun"), TEXT("Double Barrel"), TEXT("Sawed-Off"), TEXT("Gatling"), TEXT("Dynamite"), TEXT("Knife") };
	static TArray<FCase> TapCases;
	static TArray<double> ShotAt;
	static FVector Eye0, Fl, Rt;
	static int32 Ci = -1, Si = 0, Passes = 0, Fails = 0, Shots0 = 0, Frames = 0, Presses = 0;
	static double Now = 0.0, StepAt = 0.0;
	static float MaxDt = 0.f, MarkCool = 0.f, Seen = 0.f;
	static bool bLastDown = false;
	if (!L || ShotTime < 3.f) return;
	const bool bOld = OLFlags::Param(TEXT("OLOldTriggerTap"));
	const float Cycle = bOld ? 0.5f : 0.25f;
	bVR = true;                          // (the guns' update runs only in VR)
	Now = GetWorld()->GetTimeSeconds();
	MaxDt = FMath::Max(MaxDt, Dt);
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT triggertap check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	auto SetCVar = [](const TCHAR* Name, float V)
	{
		if (IConsoleVariable* C = IConsoleManager::Get().FindConsoleVariable(Name)) C->Set(V, ECVF_SetByCode);
	};
	auto Begin = [this]()
	{
		const FCase& K = TapCases[Ci];
		TestAxes.Reset();
		Equip(1 - K.Hand, -1);
		Equip(K.Hand, K.Gun);
		Mag[K.Hand] = FMath::Max(Mag[K.Hand], 1);
		for (int32 h = 0; h < 2; ++h)
		{
			bTriggerArmed[h] = true;
			bDeathDropTrigger[h] = false;
			FireCooldown[h] = TapKept[h] = 0.f;
		}
		Si = Frames = Presses = 0;
		StepAt = Now;
		MaxDt = MarkCool = Seen = 0.f;
		bLastDown = false;
		ShotAt.Reset();
		Shots0 = TutNotes.Shots;
	};
	if (TapCases.Num() == 0)
	{
		L->bInvincible = true;
		L->GiveAllWeapons();
		bUnlimitedAmmo = true;           // (a shot still needs a round in: Begin gives one; the single-shell gun fires on)
		Eye0 = Camera->GetComponentLocation();
		Fl = FVector(Camera->GetForwardVector().X, Camera->GetForwardVector().Y, 0.0).GetSafeNormal();
		Rt = FVector(Camera->GetRightVector().X, Camera->GetRightVector().Y, 0.0).GetSafeNormal();
		// the steps run from here on, long after this frame: statics only, no locals of this call
		auto Set = [this](float V) -> FStep { return [this, V]()
		{
			TestAxes.Add(TapCases[Ci].Hand == 0 ? TEXT("OLVR_LeftTrigger") : TEXT("OLVR_RightTrigger"), V);
			const bool bDown = V > 0.7f;
			if (bDown && !bLastDown) ++Presses;
			bLastDown = bDown;
			return true;
		}; };
		auto WaitFrames = [](int32 N) -> FStep { return [N]() { return Frames >= N; }; };
		auto Wait = [](double S) -> FStep { return [S]() { return Now - StepAt >= S; }; };
		auto Shots = [](int32 N) -> FStep { return [N]() { return ShotAt.Num() >= N || Now - StepAt >= 1.5; }; };
		// the press whose moment counts: was the gun inside its cycle?
		auto MarkPress = [this]() -> FStep { return [this]()
		{
			MarkCool = FireCooldown[TapCases[Ci].Hand];
			TestAxes.Add(TapCases[Ci].Hand == 0 ? TEXT("OLVR_LeftTrigger") : TEXT("OLVR_RightTrigger"), 1.f);
			return true;
		}; };
		auto Do = [](TFunction<void()> F) -> FStep { return [F]() { F(); return true; }; };
		// a press every other frame for Sec; or Hz presses a second
		auto Spam = [this](double Sec, double Hz) -> FStep { return [this, Sec, Hz]()
		{
			const double Dur = Now - StepAt;
			const bool bDown = Dur < Sec && (Hz > 0.0 ? FMath::FloorToInt(Dur * Hz * 2.0) % 2 == 0 : Frames % 2 == 0);
			TestAxes.Add(TapCases[Ci].Hand == 0 ? TEXT("OLVR_LeftTrigger") : TEXT("OLVR_RightTrigger"), bDown ? 1.f : 0.f);
			if (bDown && !bLastDown) ++Presses;
			bLastDown = bDown;
			return Dur >= Sec;
		}; };
		auto First = [&](TArray<FStep> More)
		{
			// settled, let go; one shot, let go
			TArray<FStep> A = { Wait(0.3), Set(0.f), WaitFrames(2), Set(1.f), Shots(1), WaitFrames(1) };
			A.Append(MoveTemp(More));
			return A;
		};
		auto Add = [&](const FString& Name, int32 Gun, int32 Hand, TArray<FStep> Steps, TFunction<void()> Verify)
		{
			FCase K;
			K.Name = Name;
			K.Gun = Gun;
			K.Hand = Hand;
			K.Steps = MoveTemp(Steps);
			K.Verify = MoveTemp(Verify);
			TapCases.Add(MoveTemp(K));
		};
		const bool bO = bOld;
		// the kept press's shot: the cycle after the first, whenever inside it the press came
		auto TapVerify = [bO, Check](const TCHAR* Gun, float Cy)
		{
			const FString Name = Gun;
			return [Cy, bO, Check, Name]()
			{
				const double Gap = ShotAt.Num() >= 2 ? ShotAt[1] - ShotAt[0] : -1.0;
				if (bO) Check(ShotAt.Num() == 1, FString::Printf(TEXT("%s tap: old: the press inside the cycle lost (cooldown left %.2f, %d shots)"), *Name, MarkCool, ShotAt.Num()));
				else Check(ShotAt.Num() == 2 && MarkCool > 0.f && Gap >= Cy - 0.002 && Gap <= Cy + 2.0 * MaxDt + 0.005,
					FString::Printf(TEXT("%s tap: two presses, the second %.2f s before the cycle's end: %d shots, %.3f s apart (cycle %.2f)"), *Name, MarkCool, ShotAt.Num(), Gap, Cy));
			};
		};
		for (int32 g = 0; g < 5; ++g)
		{
			const FString Gn = GunName[g];
			const float Cy = g == 0 ? Cycle : 0.5f;        // (BUG-TRIGGERTAP2: the revolver's alone is halved)
			Add(TEXT("tap"), g, 1, First({ Set(0.f), WaitFrames(1), MarkPress(), WaitFrames(1), Set(0.f), Wait(Cy * 2.f + 0.2f) }), TapVerify(GunName[g], Cy));
			Add(TEXT("held"), g, 1, First({ Wait(Cy * 3.f + 0.1f), Set(0.f) }), [Check, Gn]()
			{
				Check(ShotAt.Num() == 1, FString::Printf(TEXT("%s held past three cycles: %d shot"), *Gn, ShotAt.Num()));
			});
			Add(TEXT("held2"), g, 1, First({ Set(0.f), WaitFrames(1), MarkPress(), Wait(Cy * 4.f), Set(0.f) }), [Check, Gn, bO]()
			{
				Check(ShotAt.Num() == (bO ? 1 : 2), FString::Printf(TEXT("%s press inside the cycle and held: %d shots (no repeat)"), *Gn, ShotAt.Num()));
			});
			Add(TEXT("spam"), g, 1, { Wait(0.3), Set(0.f), WaitFrames(2), Spam(Cy * 6.f + 0.1f, 0.0), Set(0.f) }, [Check, Gn, Cy]()
			{
				double Sum = 0.0, Lo = 100.0;
				for (int32 i = 1; i < ShotAt.Num(); ++i) { const double G = ShotAt[i] - ShotAt[i - 1]; Sum += G; Lo = FMath::Min(Lo, G); }
				const int32 Gaps = FMath::Max(1, ShotAt.Num() - 1);
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT triggertap cycle %s: %.3f s mean, %.3f s least, %d gaps, %d presses, a frame up to %.3f s"), *Gn, Sum / Gaps, Lo, ShotAt.Num() - 1, Presses, MaxDt);
				Check(ShotAt.Num() >= 5 && Lo >= Cy - 0.002 && Sum / Gaps <= Cy + 3.0 * MaxDt + 0.01, FString::Printf(TEXT("%s measured cycle %.3f s (want %.2f), a shot every %.1f presses"), *Gn, Sum / Gaps, Cy, float(Presses) / FMath::Max(1, ShotAt.Num())));
			});
		}
		// the revolver: no stacking, however many presses inside the one cycle
		Add(TEXT("nostack"), 0, 1, First({ Set(0.f), WaitFrames(1), MarkPress(), WaitFrames(1), Set(0.f), WaitFrames(1), Set(1.f), WaitFrames(1), Set(0.f), Wait(Cycle * 3.f) }), [Check, bO]()
		{
			Check(ShotAt.Num() == (bO ? 1 : 2), FString::Printf(TEXT("two presses inside one cycle keep one: %d shots"), ShotAt.Num()));
		});
		// fast tapping, 8 presses a second for 2 s
		Add(TEXT("rapid"), 0, 1, { Wait(0.3), Set(0.f), WaitFrames(2), Spam(2.0, 8.0), Set(0.f), Wait(0.4) }, [Check, bO]()
		{
			Check(bO ? ShotAt.Num() <= 5 : ShotAt.Num() >= 7 && ShotAt.Num() <= 10, FString::Printf(TEXT("8 presses a second for 2 s: %d presses, %d shots"), Presses, ShotAt.Num()));
		});
		// the quick partial release: down only to 0.45, inside the cycle and after it; to 0.55 no re-arm
		Add(TEXT("dip in"), 0, 1, First({ Set(0.45f), WaitFrames(1), MarkPress(), Wait(Cycle * 2.f), Set(0.f) }), [Check, bO]()
		{
			Check(ShotAt.Num() == (bO ? 1 : 2), FString::Printf(TEXT("a dip to 0.45 inside the cycle and back: %d shots"), ShotAt.Num()));
		});
		Add(TEXT("dip after"), 0, 1, First({ Wait(Cycle + 0.1f), Set(0.45f), WaitFrames(2), Set(1.f), WaitFrames(4), Set(0.f), Wait(0.1) }), [Check, bO]()
		{
			Check(ShotAt.Num() == (bO ? 1 : 2), FString::Printf(TEXT("a dip to 0.45 after the cycle and back: %d shots"), ShotAt.Num()));
		});
		Add(TEXT("dip short"), 0, 1, First({ Wait(Cycle + 0.1f), Set(0.55f), WaitFrames(2), Set(1.f), WaitFrames(4), Set(0.f), Wait(0.1) }), [Check]()
		{
			Check(ShotAt.Num() == 1, FString::Printf(TEXT("a dip only to 0.55: no re-arm, %d shot"), ShotAt.Num()));
		});
		if (!bOld)
		{
			Add(TEXT("swap"), 0, 1, First({ Set(0.f), WaitFrames(1), MarkPress(), WaitFrames(1), Do([this]() { Seen = TapKept[1]; Equip(1, 1); }), Set(0.f), Wait(0.6) }), [Check]()
			{
				Check(Seen > 0.f && ShotAt.Num() == 1, FString::Printf(TEXT("a press kept (%.2f s good) goes with the gun swapped: %d shot"), Seen, ShotAt.Num()));
			});
			Add(TEXT("guard"), 0, 1, First({ Set(0.f), WaitFrames(1), MarkPress(), WaitFrames(1), Do([this]() { Seen = TapKept[1]; bDeathDropTrigger[1] = true; DeathDropAt[1] = Now; }), Wait(0.6), Set(0.f), Wait(0.1) }), [this, Check]()
			{
				Check(Seen > 0.f && ShotAt.Num() == 1 && !bDeathDropTrigger[1], FString::Printf(TEXT("a press kept goes with REQ-SMASHTRIGGER's guard: %d shot, the guard off once let go"), ShotAt.Num()));
			});
			Add(TEXT("left"), 0, 0, First({ Set(0.f), WaitFrames(1), MarkPress(), WaitFrames(1), Set(0.f), Wait(Cycle * 3.f) }), [Check]()
			{
				Check(ShotAt.Num() == 2, FString::Printf(TEXT("the left hand's press kept: %d shots"), ShotAt.Num()));
			});
			const TCHAR* const RevCvar = TEXT("ol.vr.CycleRevolver");
			const TCHAR* const RifleCvar = TEXT("ol.vr.CycleRifle");
			Add(TEXT("clamp"), 0, 1, { Wait(0.3), Do([this, SetCVar, RevCvar]() { SetCVar(RevCvar, 0.1f); FireCooldown[1] = 0.f; FireGun(1, false); Seen = FireCooldown[1]; SetCVar(RevCvar, 0.25f); FireCooldown[1] = 0.f; }) }, [Check]()
			{
				Check(FMath::IsNearlyEqual(Seen, 0.2f, 0.001f), FString::Printf(TEXT("ol.vr.CycleRevolver at 0.1: the aimed shot stays at the fan's %.3f"), Seen));
			});
			Add(TEXT("tunable"), 1, 1, { Wait(0.3), Do([this, SetCVar, RifleCvar]() { SetCVar(RifleCvar, 0.4f); FireCooldown[1] = 0.f; FireGun(1, false); Seen = FireCooldown[1]; SetCVar(RifleCvar, 0.5f); FireCooldown[1] = 0.f; }) }, [Check]()
			{
				Check(FMath::IsNearlyEqual(Seen, 0.4f, 0.001f), FString::Printf(TEXT("ol.vr.CycleRifle at 0.4: %.3f"), Seen));
			});
		}
		Add(TEXT("fan"), 0, 1, { Wait(0.3), Do([this]() { FireCooldown[1] = 0.f; FireGun(1, true); Seen = FireCooldown[1]; FireCooldown[1] = 0.f; }) }, [Check]()
		{
			Check(FMath::IsNearlyEqual(Seen, 0.2f, 0.001f), FString::Printf(TEXT("the fan's cycle as it was: %.3f"), Seen));
		});
		Add(TEXT("gatling"), 5, 1, { Wait(0.3), Set(0.f), WaitFrames(2), Set(1.f), WaitFrames(1), Wait(1.0), Do([this]() { Seen = TapKept[1]; }), Set(0.f), Wait(0.15) }, [Check]()
		{
			double Lo = 100.0;
			for (int32 i = 1; i < ShotAt.Num(); ++i) Lo = FMath::Min(Lo, ShotAt[i] - ShotAt[i - 1]);
			Check(ShotAt.Num() >= 5 && Lo >= 0.098 && Seen == 0.f, FString::Printf(TEXT("the gatling held keeps firing, nothing kept: %d shots in 1 s, least gap %.3f s"), ShotAt.Num(), Lo));
		});
		Add(TEXT("knife"), 7, 1, { Wait(0.3), Do([this]() { bTriggerArmed[1] = false; }), Set(0.45f), WaitFrames(2), Do([this]() { Seen = bTriggerArmed[1] ? 1.f : 0.f; }), Set(0.2f), WaitFrames(2) }, [this, Check]()
		{
			Check(Seen == 0.f && bTriggerArmed[1], FString::Printf(TEXT("a throwable re-arms under 0.3 as ever: at 0.45 %s, at 0.2 %s"), Seen > 0.f ? TEXT("armed") : TEXT("not"), bTriggerArmed[1] ? TEXT("armed") : TEXT("not")));
		});
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT triggertap start: %s, %s, %d cases"), *L->LevelName, bOld ? TEXT("-OLOldTriggerTap") : TEXT("new"), TapCases.Num());
		Ci = 0;
		Begin();
		return;
	}
	if (!TapCases.IsValidIndex(Ci)) return;
	// both hands out ahead, apart, still, along the view
	{
		const FVector Base = Eye0 + Fl * 70.0 - FVector(0.0, 0.0, 20.0);
		GripOf(0)->SetWorldLocationAndRotation(Base - Rt * 35.0, Fl.Rotation());
		GripOf(1)->SetWorldLocationAndRotation(Base + Rt * 35.0, Fl.Rotation());
		for (int32 h = 0; h < 2; ++h) HandVelocity[h] = HandWorldVel[h] = FVector::ZeroVector;
	}
	for (const int32 N = TutNotes.Shots - Shots0; ShotAt.Num() < N;) ShotAt.Add(Now);
	FCase& K = TapCases[Ci];
	while (Si < K.Steps.Num() && K.Steps[Si]())
	{
		++Si;
		Frames = 0;
		StepAt = Now;
	}
	++Frames;
	if (Si < K.Steps.Num()) return;
	K.Verify();
	++Ci;
	if (Ci < TapCases.Num())
	{
		Begin();
		return;
	}
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT triggertap RESULT %s (%d passed, %d failed, %s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
		bOld ? TEXT("-OLOldTriggerTap") : TEXT("new"));
	TestAxes.Reset();
	FGenericPlatformMisc::RequestExit(false);
	ShotTime = -1.f;
}

#endif // WITH_OUTLAWS_TESTS
