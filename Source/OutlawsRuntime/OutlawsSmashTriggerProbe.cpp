// REQ-SMASHTRIGGER (user 2026-11-02: a held bad guy killed in the hand, by the smash of two held, dropped out of it and the
// gun given back fired at once, the trigger still held): -OLShots -OLLevel=hideout -OLNoFrontEnd -OLSmashTriggerProbe
// [-OLOldSmashTrigger].  The headset's rules (bVR) for the run.  A revolver in each hand, the two nearest men hurt by 70% and
// held one in each hand before the eyes, the right hand's trigger held (the throw's arc out), clapped together at 200 cm/s a
// hand: the smash fells both, they drop out, the revolvers come back.  Shots are the trigger's calls into FireGun
// (GunFireCalls), only the right trigger ever pulled.  Runs: (held) the trigger kept held 0.6 s after the drop: no shot (old:
// a shot within 0.2 s); let go and pulled again: one shot.  (floor) let go the frame after the drop and pulled again at 0.15
// s: no shot before 0.3 s nor from that pull; let go and pulled again later: one shot.  (release) one man held in the right
// hand with the trigger held and the grip let go (thrown along the arc, a normal release): a shot within 0.3 s, as before,
// old and new alike.  Lines "OLSHOT smashtrigger check PASS|FAIL: ..." and "OLSHOT smashtrigger RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsEnemy.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Camera/CameraComponent.h"
#include "MotionControllerComponent.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLSmashTriggerProbe) { return OLFlags::Param(TEXT("OLSmashTriggerProbe")); }

OL_SHOT_TEST(OLSmashTriggerProbe, 1440)
{
	enum ECase : int32 { Held, Floor, Release };
	static TArray<int32> Runs;
	static TWeakObjectPtr<AOutlawsEnemy> Man[2];
	static FVector Eye0, Fl, Rt;
	static int32 Ci = 0, Stage = 0, Passes = 0, Fails = 0, Shots0 = 0, Step = 0;
	static double Sep = 170.0, StageAt = 0.0, DropAt = -1.0;
	if (!L || ShotTime < 0.f) return;
	const double T = ShotTime;
	const bool bOld = OLFlags::Param(TEXT("OLOldSmashTrigger"));
	bVR = true;                          // (the guns' update runs only in VR)
	const TCHAR* const RTrig = TEXT("OLVR_RightTrigger");
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT smashtrigger check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT smashtrigger RESULT %s (%d passed, %d failed, %s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			bOld ? TEXT("-OLOldSmashTrigger") : TEXT("new"));
		TestAxes.Reset();
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	};
	// both hands Sep apart across the view, 1.8 m ahead; Speed: each hand's toward the other
	auto Place = [&](double Speed)
	{
		const FVector Base = Eye0 + Fl * 180.0 + FVector(0.0, 0.0, 35.0);
		GripOf(0)->SetWorldLocationAndRotation(Base - Rt * (Sep * 0.5), Fl.Rotation());
		GripOf(1)->SetWorldLocationAndRotation(Base + Rt * (Sep * 0.5), Fl.Rotation());
		HandVelocity[0] = HandWorldVel[0] = Rt * Speed;
		HandVelocity[1] = HandWorldVel[1] = -Rt * Speed;
	};
	auto Trig = [&](float V) { TestAxes.Add(RTrig, V); };
	const int32 Shots = GunFireCalls - Shots0;
	if (Stage == 0)
	{
		if (T < 3.0) return;
		if (Runs.Num() == 0)
		{
			L->bInvincible = true;
			L->GiveAllWeapons();
			Eye0 = Camera->GetComponentLocation();
			Fl = FVector(Camera->GetForwardVector().X, Camera->GetForwardVector().Y, 0.0).GetSafeNormal();
			Rt = FVector(Camera->GetRightVector().X, Camera->GetRightVector().Y, 0.0).GetSafeNormal();
			Runs = bOld ? TArray<int32>{ Held, Release } : TArray<int32>{ Held, Floor, Release };
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT smashtrigger start: %s, %s, %d cases"), *L->LevelName, bOld ? TEXT("old") : TEXT("new"), Runs.Num());
		}
		if (Ci >= Runs.Num())
		{
			Finish();
			return;
		}
		const int32 C = Runs[Ci];
		const int32 Need = C == Release ? 1 : 2;
		TArray<AOutlawsEnemy*> Near;
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
		{
			AOutlawsEnemy* E = Wk.Get();
			if (!E || E->IsDead() || E->bBoss || E->IsCritter() || E->IsDormant() || E->bHeldByPlayer || E->bThrown || !E->CanBeGrabbed() || !E->GetSprite()) continue;
			Near.Add(E);
		}
		Near.Sort([this](const AOutlawsEnemy& X, const AOutlawsEnemy& Y) { return FVector::DistSquared(X.GetActorLocation(), GetActorLocation()) < FVector::DistSquared(Y.GetActorLocation(), GetActorLocation()); });
		if (Near.Num() < Need)
		{
			Check(false, FString::Printf(TEXT("case %d: %d men to hold (%d about)"), C, Need, Near.Num()));
			Finish();
			return;
		}
		// a revolver in each hand, loaded, its trigger let go before (armed); then the men taken (the revolvers set aside)
		TestAxes.Reset();
		for (int32 h = 0; h < 2; ++h)
		{
			Equip(h, 0);
			Mag[h] = 6;
			bTriggerArmed[h] = true;
		}
		Sep = 170.0;
		Place(0.0);
		bool bOk = true;
		const int32 First = C == Release ? 1 : 0;
		for (int32 h = First; h < 2 && bOk; ++h)
		{
			AOutlawsEnemy* E = Near[h - First];
			Man[h] = E;
			E->SetActorLocation(GripOf(h)->GetComponentLocation() + Fl * 40.0, false, nullptr, ETeleportType::TeleportPhysics);
			if (C != Release) E->TakeHit(E->GetMaxHealth() * 0.7f, E->GrabChest(), Fl, Eye0, true, 0.f);    // (one smash fells him)
			bOk = !E->IsDead() && TryGrabEnemy(h, E);
			TestAxes.Add(h == 0 ? TEXT("OLVR_LeftGrip") : TEXT("OLVR_RightGrip"), 1.f);
			bGripArmed[h] = false;
		}
		// the right hand's throw lined up, its trigger held
		bLaunchArmed[1] = false;
		bGrabAiming[1] = true;
		Trig(1.f);
		Check(bOk, FString::Printf(TEXT("case %s: %d held, the revolvers set aside (left %d, right %d)"), C == Held ? TEXT("held") : C == Floor ? TEXT("floor") : TEXT("release"),
			Need, Equipped[0], Equipped[1]));
		if (!bOk) { Finish(); return; }
		Shots0 = GunFireCalls;
		DropAt = -1.0;
		Step = 0;
		Stage = 1;
		StageAt = T;
		return;
	}
	const int32 C = Runs[Ci];
	auto NextCase = [&]()
	{
		TestAxes.Reset();
		for (int32 h = 0; h < 2; ++h) if (GrabbedEnemy[h].IsValid()) LetGoEnemy(h, false);
		++Ci;
		Stage = 0;
		StageAt = T;
	};
	if (Stage == 1)
	{
		// held still, past the smash's cooldown; nothing fires from a hand that holds a man
		Place(0.0);
		if (T - StageAt < 0.5) return;
		Check(Shots == 0, FString::Printf(TEXT("held: no shot (%d)"), Shots));
		if (C == Release)
		{
			TestAxes.Add(TEXT("OLVR_RightGrip"), 0.f);        // (the grip let go, the trigger held: thrown along the arc)
			Stage = 3;
			StageAt = T;
			return;
		}
		Stage = 2;
		StageAt = T;
		return;
	}
	if (Stage == 2)
	{
		// the clap, till both have dropped out
		Sep = FMath::Max(0.0, Sep - 400.0 * Dt);
		Place(200.0);
		if (!GrabbedEnemy[0].IsValid() && !GrabbedEnemy[1].IsValid())
		{
			AOutlawsEnemy* A = Man[0].Get();
			AOutlawsEnemy* B = Man[1].Get();
			Check(A && B && A->IsDead() && B->IsDead() && Equipped[1] == 0, FString::Printf(TEXT("the smash felled both (%d %d) and the right revolver is back (%d, %d rounds), %d shots"),
				A && A->IsDead() ? 1 : 0, B && B->IsDead() ? 1 : 0, Equipped[1], Mag[1], Shots));
			Shots0 = GunFireCalls;
			Stage = 3;
			StageAt = T;
			Sep = 170.0;
			return;
		}
		if (Sep <= 0.0 && T - StageAt > 1.5)
		{
			Check(false, TEXT("the smash felled them (they stayed in the hands)"));
			NextCase();
		}
		return;
	}
	// after the drop (or the let-go): the hands still before the eyes
	Place(0.0);
	const double Since = T - StageAt;
	if (C == Release)
	{
		if (Since < 0.3) return;
		Check(Shots >= 1, FString::Printf(TEXT("a normal let-go with the trigger held: a shot within 0.3 s, as before (%d; right revolver %d)"), Shots, Equipped[1]));
		NextCase();
		return;
	}
	if (C == Held)
	{
		if (bOld)
		{
			if (Since < 0.2) return;
			Check(Shots >= 1, FString::Printf(TEXT("old: the trigger held, a shot within 0.2 s of the drop (%d)"), Shots));
			NextCase();
			return;
		}
		if (Step == 0)
		{
			if (Since < 0.6) return;
			Check(Shots == 0, FString::Printf(TEXT("the trigger kept held 0.6 s after the drop: no shot (%d)"), Shots));
			Trig(0.f);
			Step = 1;
			return;
		}
		if (Step == 1)
		{
			if (Since < 0.75) return;
			Trig(1.f);
			Step = 2;
			return;
		}
		if (Since < 0.95) return;
		Check(Shots == 1, FString::Printf(TEXT("let go and pulled again: one shot (%d)"), Shots));
		NextCase();
		return;
	}
	// Floor: let go at once, pulled again at 0.15 s
	if (Step == 0)
	{
		Trig(0.f);
		Step = 1;
		return;
	}
	if (Step == 1)
	{
		if (Since < 0.15) return;
		Trig(1.f);
		Step = 2;
		return;
	}
	if (Step == 2)
	{
		if (Since < 0.28) return;
		Check(Shots == 0, FString::Printf(TEXT("let go at once and pulled at 0.15 s: no shot before 0.3 s (%d)"), Shots));
		Step = 3;
		return;
	}
	if (Step == 3)
	{
		if (Since < 0.5) return;
		Check(Shots == 0, FString::Printf(TEXT("that pull, still held at 0.5 s: no shot from it (%d)"), Shots));
		Trig(0.f);
		Step = 4;
		return;
	}
	if (Step == 4)
	{
		if (Since < 0.6) return;
		Trig(1.f);
		Step = 5;
		return;
	}
	if (Since < 0.8) return;
	Check(Shots == 1, FString::Printf(TEXT("let go and pulled again after the floor: one shot (%d)"), Shots));
	NextCase();
}

#endif // WITH_OUTLAWS_TESTS
