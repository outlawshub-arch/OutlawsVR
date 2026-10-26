// -OLShots -OLLevel=hideout -OLNoFrontEnd -OLArcStuckTest (BUG-ARCSTUCK, single player): the throw arc must go with the thing held
// to throw, whatever takes it from the hand.  A man held in either hand with the arc out is punched dead by the other hand, then
// shot dead; a lit stick of dynamite in either hand has the menu opened, the wheel opened, is dropped, and in the right hand the
// level is restarted with it held.  Before each the arc must show; two frames after, no dot of it may stand (one drawn afresh
// that frame by a stick still in hand is allowed after the restart).  Prints "OLSHOT arcstuck check PASS|FAIL: ..." and
// "OLSHOT arcstuck RESULT".  Before the restart (BUG-TNTARCTRIGGER), in each hand an unlit stick shows no arc, the trigger lights it and
// the arc shows, and letting go throws it and the arc goes (TntArcTestStep, run on the co-op host and client too).  The co-op halves (a partner's kill, on the host and on the client) are in -OLCoopTest=grab.

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsEnemy.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Components/StaticMeshComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/AudioComponent.h"
#include "MotionControllerComponent.h"
#include "HAL/PlatformTime.h"
#include "OutlawsShotTests.h"

#if WITH_OUTLAWS_TESTS

namespace
{
	enum class EArcCase : uint8 { Punch, Shot, Menu, Wheel, Drop, Restart, Trigger };
	struct FArcCase { EArcCase What; int32 Hand; const TCHAR* Name; };
	const FArcCase GArcCases[] = {
		{ EArcCase::Punch, 1, TEXT("right hand's man punched dead by the left") },
		{ EArcCase::Shot, 1, TEXT("right hand's man shot dead") },
		{ EArcCase::Punch, 0, TEXT("left hand's man punched dead by the right") },
		{ EArcCase::Shot, 0, TEXT("left hand's man shot dead") },
		{ EArcCase::Menu, 1, TEXT("lit dynamite in the right hand, the menu opened") },
		{ EArcCase::Wheel, 1, TEXT("lit dynamite in the right hand, the wheel opened") },
		{ EArcCase::Drop, 1, TEXT("lit dynamite dropped from the right hand") },
		{ EArcCase::Menu, 0, TEXT("lit dynamite in the left hand, the menu opened") },
		{ EArcCase::Wheel, 0, TEXT("lit dynamite in the left hand, the wheel opened") },
		{ EArcCase::Drop, 0, TEXT("lit dynamite dropped from the left hand") },
		{ EArcCase::Trigger, 1, TEXT("right hand's stick: no arc held, the arc with the trigger, gone on the throw") },
		{ EArcCase::Trigger, 0, TEXT("left hand's stick: no arc held, the arc with the trigger, gone on the throw") },
		{ EArcCase::Restart, 1, TEXT("lit dynamite in the right hand, the level restarted") },
	};
	constexpr int32 GNumArcCases = UE_ARRAY_COUNT(GArcCases);
}

bool AOutlawsPlayer::TntArcTestStep(int32 Hand, int32 Stage, const TCHAR* Who)
{
	const int32 H = Hand & 1;
	const TCHAR* Trig = H == 0 ? TEXT("OLVR_LeftTrigger") : TEXT("OLVR_RightTrigger");
	const TCHAR* Side = H == 0 ? TEXT("left") : TEXT("right");
	bool bOk = true;
	FString What;
	switch (Stage)
	{
	case 0:
		// (the guns' and throwables' update runs only in VR)
		bVR = true;
		if (AOutlawsLevelActor* L = Level.Get()) L->GiveAllWeapons();
		Equip(1 - H, -1);
		Equip(H, 6);
		Mag[H] = 1;
		bFuseLit[H] = false;
		bTntGrip[H] = false;
		bTriggerArmed[H] = true;
		TestAxes.Remove(Trig);
		What = FString::Printf(TEXT("%s hand: a stick taken (weapon %d, mesh %d)"), Side, Equipped[H], WeaponMesh[H] ? 1 : 0);
		bOk = Equipped[H] == 6 && WeaponMesh[H] != nullptr;
		break;
	case 1:
		bOk = !bFuseLit[H] && !ArcDotsVisible(H) && !ArcDotsVisible(1 - H);
		What = FString::Printf(TEXT("%s hand: the stick just held, no arc (lit %d, arc %d %d)"), Side, bFuseLit[H] ? 1 : 0, ArcDotsVisible(0) ? 1 : 0, ArcDotsVisible(1) ? 1 : 0);
		bTriggerArmed[H] = true;
		TestAxes.Add(Trig, 1.f);
		break;
	case 2:
		Fuse[H] = FMath::Max(Fuse[H], 3.f);           // (kept from going off in the hand)
		bOk = bFuseLit[H] && ArcDotsVisible(H) && !ArcDotsVisible(1 - H);
		What = FString::Printf(TEXT("%s hand: the trigger held, the fuse lit and the arc out (lit %d, arc %d %d)"), Side, bFuseLit[H] ? 1 : 0, ArcDotsVisible(0) ? 1 : 0, ArcDotsVisible(1) ? 1 : 0);
		TestAxes.Remove(Trig);
		break;
	default:
		bOk = !bFuseLit[H] && !ArcDotsVisible(H) && !ArcDotsVisible(1 - H);
		What = FString::Printf(TEXT("%s hand: the trigger let go, thrown and the arc gone (lit %d, rounds %d, arc %d %d)"), Side, bFuseLit[H] ? 1 : 0, Mag[H], ArcDotsVisible(0) ? 1 : 0, ArcDotsVisible(1) ? 1 : 0);
		Equip(H, -1);
		break;
	}
	UE_LOG(LogOutlaws, Display, TEXT("%s tntarc check %s: %s"), Who, bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
	return bOk;
}

void AOutlawsPlayer::ArcStuckTestTick()
{
	AOutlawsLevelActor* L = Level.Get();
	UWorld* W = GetWorld();
	if (!L || !W) return;
	// (statics, not ShotStep: the restart may make the player anew)
	static int32 Case = 0, Phase = 0, Fails = 0, Clear0 = 0;
	static double At = 0.0, Start = 0.0;
	static uint64 EventFrame = 0;
	static TWeakObjectPtr<AOutlawsEnemy> Man;
	const double Now = FPlatformTime::Seconds();
	if (Start == 0.0) Start = Now;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT arcstuck check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (!bOk) ++Fails;
	};
	auto Finish = [this]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT arcstuck RESULT %s (%d failed)"), Fails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Fails);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	};
	if (Now - Start < 6.0) return;
	if (Now - Start > 150.0)
	{
		Check(false, FString::Printf(TEXT("timed out in case %d phase %d"), Case, Phase));
		Finish();
		return;
	}
	if (Case >= GNumArcCases)
	{
		Finish();
		return;
	}
	const FArcCase& C = GArcCases[Case];
	const int32 H = C.Hand;
	// both hands held out before the eyes, a hand's width apart
	const FVector Fl = FVector(Camera->GetForwardVector().X, Camera->GetForwardVector().Y, 0.0).GetSafeNormal();
	for (int32 h = 0; h < 2; ++h)
	{
		GripOf(h)->SetWorldLocationAndRotation(Camera->GetComponentLocation() + Fl * 50.0 + Camera->GetRightVector() * (h == 0 ? -15.0 : 15.0) - FVector(0.0, 0.0, 15.0), Fl.Rotation());
		HandVelocity[h] = HandWorldVel[h] = FVector::ZeroVector;
	}
	const TCHAR* Trig = H == 0 ? TEXT("OLVR_LeftTrigger") : TEXT("OLVR_RightTrigger");
	const TCHAR* Grip = H == 0 ? TEXT("OLVR_LeftGrip") : TEXT("OLVR_RightGrip");
	const bool bEnemyCase = C.What == EArcCase::Punch || C.What == EArcCase::Shot;
	if (C.What == EArcCase::Trigger)
	{
		// (BUG-TNTARCTRIGGER) a step every 0.6 s: taken, held with no arc, the trigger lighting it with the arc, let go
		if (Phase > 0 && Now - At < 0.6) return;
		if (Phase == 0) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT arcstuck case %d: %s"), Case + 1, C.Name);
		if (!TntArcTestStep(H, Phase, TEXT("OLSHOT arcstuck"))) ++Fails;
		At = Now;
		if (++Phase > 3)
		{
			++Case;
			Phase = 0;
		}
		return;
	}
	if (Phase == 0)
	{
		// set up: the thing in the hand, the throw lined up
		if (bEnemyCase)
		{
			AOutlawsEnemy* Best = nullptr;
			for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
			{
				AOutlawsEnemy* E = Wk.Get();
				if (!E || E->IsDead() || E->IsDormant() || E->IsCritter() || E->bBoss || E->bVoiceOnly || E->bNotBadGuy || E->bHeldByPlayer || E->bThrown) continue;
				if (!Best || FVector::DistSquared(E->GetActorLocation(), GetActorLocation()) < FVector::DistSquared(Best->GetActorLocation(), GetActorLocation())) Best = E;
			}
			const bool bOk = Best && TryGrabEnemy(H, Best);
			if (!bOk)
			{
				Check(false, FString::Printf(TEXT("%s: a man to take"), C.Name));
				Finish();
				return;
			}
			Man = Best;
			TestAxes.Add(Grip, 1.f);
			bGripArmed[H] = false;
			bLaunchArmed[H] = false;
			bGrabAiming[H] = true;
			TestAxes.Add(Trig, 1.f);
		}
		else
		{
			// (the guns' and throwables' update runs only in VR)
			bVR = true;
			L->GiveAllWeapons();
			Equip(1 - H, -1);
			Equip(H, 6);
			Mag[H] = 1;
			bFuseLit[H] = true;
			Fuse[H] = 4.f;
			bTntGrip[H] = false;
			TestAxes.Add(Trig, 1.f);               // (a lit stick is thrown when the trigger is let go)
			bTriggerArmed[H] = false;
			Clear0 = L->GetClearCount();
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT arcstuck case %d: %s"), Case + 1, C.Name);
		Phase = 1;
		At = Now;
		return;
	}
	if (Phase == 1)
	{
		if (Now - At < 0.6) return;
		if (!bEnemyCase) Fuse[H] = 4.f;                  // (kept from going off in the hand)
		Check(ArcDotsVisible(H) && ArcDrawnFrame[H] == GFrameCounter, FString::Printf(TEXT("%s: the arc shows before (shown %d, drawn %llu frames ago; weapon %d, mesh %d, rounds %d, vr %d)"),
			C.Name, ArcDotsVisible(H) ? 1 : 0, GFrameCounter - ArcDrawnFrame[H], Equipped[H], WeaponMesh[H] ? 1 : 0, Mag[H], bVR ? 1 : 0));
		switch (C.What)
		{
		case EArcCase::Punch:
		case EArcCase::Shot:
			if (AOutlawsEnemy* E = Man.Get())
			{
				const FVector From = C.What == EArcCase::Punch ? GripOf(1 - H)->GetComponentLocation() : Camera->GetComponentLocation();
				for (int32 i = 0; i < 60 && !E->IsDead(); ++i)
				{
					AOutlawsEnemy::GNextHitMelee = C.What == EArcCase::Punch;
					E->TakeHit(50.f, E->GrabChest(), (E->GrabChest() - From).GetSafeNormal(), From);
				}
				AOutlawsEnemy::GNextHitMelee = false;
				Check(E->IsDead(), FString::Printf(TEXT("%s: he died"), C.Name));
			}
			break;
		case EArcCase::Menu:
			TestAxes.Remove(Trig);                 // (no pull on a menu row; nothing throws behind the menu)
			OpenMenu(false);
			Check(bMenuOpen, FString::Printf(TEXT("%s: the menu opened"), C.Name));
			break;
		case EArcCase::Wheel:
			OpenWheel(H);
			Check(bWheelOpen, FString::Printf(TEXT("%s: the wheel opened"), C.Name));
			break;
		case EArcCase::Drop:
			ThrowHeld(H, false, true);
			break;
		case EArcCase::Restart:
			L->RestartLevel();
			break;
		}
		EventFrame = GFrameCounter;
		Phase = 2;
		At = Now;
		return;
	}
	if (Phase == 2)
	{
		if (C.What == EArcCase::Wheel && (!bWheelOpen || !bWheelHeld[H])) OpenWheel(H);     // (held open, as its button)
		if (!bEnemyCase) Fuse[H] = 4.f;
		if (C.What == EArcCase::Restart)
		{
			// the new level built and a second on it
			if (L->GetClearCount() == Clear0 && Now - At < 30.0) return;
			if (Now - At < 3.0) return;
			const bool bStale = ArcDotsVisible(H) && ArcDrawnFrame[H] != GFrameCounter;
			Check(!bStale && !ArcDotsVisible(1 - H), FString::Printf(TEXT("%s: no arc left standing (shown %d, drawn this frame %d)"), C.Name,
				ArcDotsVisible(H) ? 1 : 0, ArcDrawnFrame[H] == GFrameCounter ? 1 : 0));
		}
		else
		{
			if (GFrameCounter < EventFrame + 2) return;
			Check(!ArcDotsVisible(H) && !ArcDotsVisible(1 - H), FString::Printf(TEXT("%s: the arc is gone %llu frames on"), C.Name, GFrameCounter - EventFrame));
			if (bEnemyCase)
			{
				Check(!GrabbedEnemy[H].IsValid() && !bGrabAiming[H], FString::Printf(TEXT("%s: the hand let go, no throw lined up"), C.Name));
			}
		}
		// put back for the next case
		TestAxes.Remove(Trig);
		TestAxes.Remove(Grip);
		if (bMenuOpen) CloseMenu();
		if (bWheelOpen) CloseWheel();
		if (!bEnemyCase)
		{
			bFuseLit[H] = false;
			if (FuseSound[H].IsValid()) FuseSound[H]->Stop();
			Equip(H, -1);
		}
		++Case;
		Phase = 0;
		At = Now;
	}
}

OL_SHOT_MATCH(OLArcStuckTest) { return OLFlags::Param(TEXT("OLArcStuckTest")); }
OL_SHOT_TEST(OLArcStuckTest, 220)
{
	ArcStuckTestTick();              // (BUG-ARCSTUCK: OutlawsPlayerArcStuckTest.cpp)
}
#endif // WITH_OUTLAWS_TESTS
