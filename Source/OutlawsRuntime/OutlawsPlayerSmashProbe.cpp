// REVERT-HELDCLASH (user 2026-10-28: "we went a bit too overboard with it"): -OLShots -OLLevel=hideout -OLNoFrontEnd
// -OLSmashProbe.  The old smash of two held bad guys, as before REQ-HELDCLASH (UpdateEnemyGrab): the two nearest men held one
// in each hand before the eyes, clapped together twice at 200 cm/s a hand, held pressed 0.8 s, then parted slowly (100 cm/s
// a hand, under the hard hit speed).  Each clap: one smash, with their chests under 55 cm apart, each man hurt by 38 to 45%
// of his full health (the hit sound and both hands' buzz are in the same step); none while pressed or parting.  Lines
// "OLSHOT smash ..." and "OLSHOT smash check PASS|FAIL: ...", then "OLSHOT smash RESULT".
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

OL_SHOT_MATCH(OLSmashProbe) { return OLFlags::Param(TEXT("OLSmashProbe")); }

OL_SHOT_TEST(OLSmashProbe, 31)
{
	static TWeakObjectPtr<AOutlawsEnemy> Man[2];
	static FVector Eye0, Fl, Rt;
	static int32 Stage = 0, Passes = 0, Fails = 0, Clap = 0, Pressed = 0;
	static int32 Smashes[2] = { 0, 0 };
	static double Sep = 170.0, StageAt = 0.0;
	static float LastCool = 0.f;
	static double SmashCm[2] = { -1.0, -1.0 };
	static float HurtA[2] = { 0.f, 0.f }, HurtB[2] = { 0.f, 0.f };
	if (!L || ShotTime < 0.f) return;
	const double T = ShotTime;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT smash check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT smash RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	};
	// both hands Sep apart across the view, 1.8 m ahead; Speed: each hand's towards the other (less than 0: apart)
	auto Place = [&](double Speed)
	{
		const FVector Base = Eye0 + Fl * 180.0 + FVector(0.0, 0.0, 35.0);
		GripOf(0)->SetWorldLocationAndRotation(Base - Rt * (Sep * 0.5), Fl.Rotation());
		GripOf(1)->SetWorldLocationAndRotation(Base + Rt * (Sep * 0.5), Fl.Rotation());
		HandVelocity[0] = HandWorldVel[0] = Rt * Speed;
		HandVelocity[1] = HandWorldVel[1] = -Rt * Speed;
	};
	if (Stage == 0)
	{
		if (T < 3.0) return;
		L->bInvincible = true;
		Eye0 = Camera->GetComponentLocation();
		Fl = FVector(Camera->GetForwardVector().X, Camera->GetForwardVector().Y, 0.0).GetSafeNormal();
		Rt = FVector(Camera->GetRightVector().X, Camera->GetRightVector().Y, 0.0).GetSafeNormal();
		TArray<AOutlawsEnemy*> Near;
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
		{
			AOutlawsEnemy* E = Wk.Get();
			if (!E || E->IsDead() || E->bBoss || E->IsCritter() || E->IsDormant() || !E->GetSprite()) continue;
			Near.Add(E);
		}
		Near.Sort([this](const AOutlawsEnemy& X, const AOutlawsEnemy& Y) { return FVector::DistSquared(X.GetActorLocation(), GetActorLocation()) < FVector::DistSquared(Y.GetActorLocation(), GetActorLocation()); });
		Sep = 170.0;
		Place(0.0);
		bool bOk = Near.Num() >= 2;
		for (int32 h = 0; h < 2 && bOk; ++h)
		{
			Man[h] = Near[h];
			Near[h]->SetActorLocation(GripOf(h)->GetComponentLocation() + Fl * 40.0, false, nullptr, ETeleportType::TeleportPhysics);
			bOk = TryGrabEnemy(h, Near[h]);
			TestAxes.Add(h == 0 ? TEXT("OLVR_LeftGrip") : TEXT("OLVR_RightGrip"), 1.f);
			bGripArmed[h] = false;
		}
		Check(bOk, FString::Printf(TEXT("two men held (%d about)"), Near.Num()));
		if (!bOk) { Finish(); return; }
		LastCool = SmashCool;
		Stage = 1;
		StageAt = T;
		return;
	}
	AOutlawsEnemy* A = Man[0].Get();
	AOutlawsEnemy* B = Man[1].Get();
	if (!A || !B || GrabbedEnemy[0].Get() != A || GrabbedEnemy[1].Get() != B)
	{
		Check(false, TEXT("a man left the hand"));
		Finish();
		return;
	}
	// a smash this frame: SmashCool set to its 0.5 s again (UpdateEnemyGrab ran before this, this tick)
	if (SmashCool > LastCool + 0.01f)
	{
		const double Cm = FVector::Dist(A->GrabChest(), B->GrabChest());
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT smash at %.2f s, stage %d, clap %d: chests %.1f cm apart, health %.1f/%.0f and %.1f/%.0f"), T, Stage, Clap, Cm,
			A->GetHealth(), A->GetMaxHealth(), B->GetHealth(), B->GetMaxHealth());
		if (Stage == 2 && Clap < 2)
		{
			if (++Smashes[Clap] == 1) SmashCm[Clap] = Cm;
		}
		else ++Pressed;
	}
	LastCool = SmashCool;
	static float Before[2] = { 0.f, 0.f };
	switch (Stage)
	{
	case 1:                                // apart, still, past the cooldown
		Place(0.0);
		if (T - StageAt < 0.7) return;
		Before[0] = A->GetHealth();
		Before[1] = B->GetHealth();
		Stage = 2;
		StageAt = T;
		return;
	case 2:                                // the clap
		Sep = FMath::Max(0.0, Sep - 400.0 * Dt);
		Place(200.0);
		if (Sep > 0.0) return;
		HurtA[Clap] = (Before[0] - A->GetHealth()) / FMath::Max(1.f, A->GetMaxHealth());
		HurtB[Clap] = (Before[1] - B->GetHealth()) / FMath::Max(1.f, B->GetMaxHealth());
		Stage = 3;
		StageAt = T;
		return;
	case 3:                                // pressed together
		Place(0.0);
		if (T - StageAt < 0.8) return;
		Stage = 4;
		StageAt = T;
		return;
	case 4:                                // parted slowly
		Sep = FMath::Min(170.0, Sep + 200.0 * Dt);
		Place(-100.0);
		if (Sep < 170.0) return;
		++Clap;
		Stage = Clap < 2 ? 1 : 5;
		StageAt = T;
		return;
	default:
		break;
	}
	for (int32 c = 0; c < 2; ++c)
	{
		Check(Smashes[c] == 1 && SmashCm[c] >= 0.0 && SmashCm[c] < 55.0, FString::Printf(TEXT("clap %d: %d smash (want 1), the chests %.1f cm apart (under 55)"), c + 1, Smashes[c], SmashCm[c]));
		Check(HurtA[c] >= 0.37f && HurtA[c] <= 0.46f && HurtB[c] >= 0.37f && HurtB[c] <= 0.46f,
			FString::Printf(TEXT("clap %d: each hurt by 38 to 45%% of his full health (%.1f%% and %.1f%%)"), c + 1, HurtA[c] * 100.f, HurtB[c] * 100.f));
	}
	Check(Pressed == 0, FString::Printf(TEXT("no smash while pressed or parting (%d)"), Pressed));
	Finish();
}

#endif // WITH_OUTLAWS_TESTS
