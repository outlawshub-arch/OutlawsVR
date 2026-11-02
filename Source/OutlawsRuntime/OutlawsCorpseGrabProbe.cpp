// REQ-CORPSEGRAB (user 2026-11-02: reaching for a bad guy in front often took a body at the feet that was a little nearer):
// -OLShots -OLLevel=hideout -OLNoFrontEnd -OLCorpseGrabProbe[=vr] [-OLOldCorpseGrab].  Two men nearest the player: one killed
// and laid at the player's feet (35 cm ahead), one alive stood 110 cm ahead; the right hand placed, its open palm facing ahead,
// and asked what its grip would take, as the grip's chain asks (the touch first, FindGrabEnemy, then the reach), taking
// nothing.  Cases: (1) the hand at chest height, both there: the live one (old: the body, nearer); (2) the hand at chest
// height, the live one sent away: not the body (old: the body); (3) the hand low over the body (20 cm off its floor), the
// live one 110 cm ahead: the body (old: the body); (4) the hand low over the body, the live one stood beside it (80 cm
// ahead, both in the touch's 60 cm): the live one (old: the body).  Lines "OLSHOT corpsegrab check PASS|FAIL: ..." and
// "OLSHOT corpsegrab RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsEnemy.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "MotionControllerComponent.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLCorpseGrabProbe) { return OLFlags::Has(TEXT("OLCorpseGrabProbe")); }

OL_SHOT_TEST(OLCorpseGrabProbe, 1430)
{
	static TWeakObjectPtr<AOutlawsEnemy> Live, Body;
	static FVector Eye0, Fl;
	static double FloorZ = 0.0, StageAt = 0.0;
	static int32 Stage = 0, Passes = 0, Fails = 0;
	static bool bVRMode = false;
	if (!L || ShotTime < 0.f) return;
	const double T = ShotTime;
	const bool bOld = OLFlags::Param(TEXT("OLOldCorpseGrab"));
	if (bVRMode) bVR = true;            // (the headset's rules, this run only)
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT corpsegrab check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT corpsegrab RESULT %s (%d passed, %d failed, %s, %s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			bOld ? TEXT("-OLOldCorpseGrab") : TEXT("new"), bVRMode ? TEXT("vr") : TEXT("flat"));
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	};
	// a man stood (or laid) with his feet on the player's floor, Ahead cm out along the view
	auto Stand = [&](AOutlawsEnemy* E, double Ahead)
	{
		const double Half = E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		const FVector At(Eye0.X + Fl.X * Ahead, Eye0.Y + Fl.Y * Ahead, FloorZ + Half + 2.0);
		E->SetActorLocation(At, false, nullptr, ETeleportType::TeleportPhysics);
	};
	// the right hand Ahead cm out and Up cm off the floor, its open palm facing ahead
	auto Hand = [&](double Ahead, double Up)
	{
		USceneComponent* G = GripOf(1);
		G->SetWorldLocationAndRotation(FVector(Eye0.X + Fl.X * Ahead, Eye0.Y + Fl.Y * Ahead, FloorZ + Up), Fl.Rotation());
		const FQuat Turn = FQuat::FindBetweenNormals(PalmDir(1).GetSafeNormal(), Fl);
		G->SetWorldRotation(Turn * G->GetComponentQuat());
	};
	// what the grip would take: the touch first, then the reach (as the grip's chain: TryGrabEnemy, then TryReachGrab)
	auto Pick = [&]() -> AOutlawsEnemy*
	{
		double S = 0.0;
		AOutlawsEnemy* E = FindGrabEnemy(1, false, S);
		return E ? E : FindGrabEnemy(1, true, S);
	};
	auto Name = [&](const AOutlawsEnemy* E) -> FString
	{
		return !E ? FString(TEXT("nothing")) : E == Live.Get() ? FString(TEXT("the live one")) : E == Body.Get() ? FString(TEXT("the body"))
			: FString::Printf(TEXT("another (%s, %s)"), *E->GetName(), E->IsDead() ? TEXT("dead") : TEXT("alive"));
	};
	if (Stage == 0)
	{
		if (T < 3.0) return;
		FString Mode;
		OLFlags::Value(TEXT("OLCorpseGrabProbe"), Mode);
		bVRMode = Mode.Equals(TEXT("vr"), ESearchCase::IgnoreCase);
		if (bVRMode) bVR = true;
		L->bInvincible = true;
		Eye0 = Camera->GetComponentLocation();
		Fl = FVector(Camera->GetForwardVector().X, Camera->GetForwardVector().Y, 0.0).GetSafeNormal();
		FloorZ = GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		TArray<AOutlawsEnemy*> Near;
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
		{
			AOutlawsEnemy* E = Wk.Get();
			if (!E || E->IsDead() || E->bBoss || E->IsCritter() || E->IsDormant() || !E->CanBeGrabbed() || !E->GetSprite()) continue;
			Near.Add(E);
		}
		Near.Sort([this](const AOutlawsEnemy& X, const AOutlawsEnemy& Y) { return FVector::DistSquared(X.GetActorLocation(), GetActorLocation()) < FVector::DistSquared(Y.GetActorLocation(), GetActorLocation()); });
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT corpsegrab start: %s, %s, %s, %d men about, floor %.1f, eye %.1f above it"), *L->LevelName, bOld ? TEXT("old") : TEXT("new"),
			bVRMode ? TEXT("vr") : TEXT("flat"), Near.Num(), FloorZ, Eye0.Z - FloorZ);
		if (Near.Num() < 2)
		{
			Check(false, FString::Printf(TEXT("two men about (%d)"), Near.Num()));
			Finish();
			return;
		}
		Live = Near[0];
		Body = Near[1];
		Stand(Near[1], 35.0);
		for (int32 i = 0; i < 60 && !Near[1]->IsDead(); ++i) Near[1]->TakeHit(50.f, Near[1]->GetActorLocation(), Fl, Near[1]->GetActorLocation() - Fl * 100.0, false, 0.f);
		Stand(Near[0], 110.0);
		Stage = 1;
		StageAt = T;
		return;
	}
	AOutlawsEnemy* A = Live.Get();
	AOutlawsEnemy* B = Body.Get();
	if (!A || !B || A->IsDead() || !B->IsDead())
	{
		Check(false, FString::Printf(TEXT("the two men as set (live %d, body %d)"), A && !A->IsDead() ? 1 : 0, B && B->IsDead() ? 1 : 0));
		Finish();
		return;
	}
	// (each case laid out afresh this tick and asked at once: nothing moves between)
	Stand(B, 35.0);
	if (Stage == 1)
	{
		Stand(A, 110.0);
		if (T - StageAt < 1.5) return;        // (the body's fall played out)
		Hand(45.0, Eye0.Z - FloorZ - 40.0);
		const AOutlawsEnemy* P = Pick();
		Check(bOld ? P == B : P == A, FString::Printf(TEXT("chest-high hand, the live one ahead and the body at the feet: %s (want %s)"), *Name(P), bOld ? TEXT("the body") : TEXT("the live one")));
		Stage = 2;
		return;
	}
	if (Stage == 2)
	{
		Stand(A, -900.0);                     // (sent away behind the player)
		Hand(45.0, Eye0.Z - FloorZ - 40.0);
		const AOutlawsEnemy* P = Pick();
		Check(bOld ? P == B : P != B, FString::Printf(TEXT("chest-high hand, the body only: %s (want %s)"), *Name(P), bOld ? TEXT("the body") : TEXT("not the body")));
		Stage = 3;
		return;
	}
	if (Stage == 3)
	{
		Stand(A, 110.0);
		Hand(35.0, 20.0);
		const AOutlawsEnemy* P = Pick();
		Check(P == B, FString::Printf(TEXT("hand 20 cm off the floor over the body, the live one 110 cm ahead: %s (want the body)"), *Name(P)));
		Stage = 4;
		return;
	}
	if (Stage == 4)
	{
		Stand(A, 80.0);
		Hand(35.0, 20.0);
		const AOutlawsEnemy* P = Pick();
		Check(bOld ? P == B : P == A, FString::Printf(TEXT("hand low over the body, the live one beside it (both in touch): %s (want %s)"), *Name(P), bOld ? TEXT("the body") : TEXT("the live one")));
		Stage = 5;
		return;
	}
	Finish();
}

#endif // WITH_OUTLAWS_TESTS
