// REQ-CLIMBWHEEL (user 2026-10-14: "I can't use the weapon wheel while I'm holding or grabbing onto a ledge and I wish I
// could"): the wheel opens while one hand holds a ledge.  The change itself is two lines (OpenWheel no longer refuses a
// climb, OutlawsPlayerWeapons.cpp; ReleaseGrab puts an open wheel away, OutlawsPlayer.cpp).  This file is its test:
// -OLShots -OLLevel=townex -OLNoFrontEnd -OLClimbWheelTest, stood where the user's snapshot hung (2026-10-14_21-02-13,
// townex sector 602): a hand takes the ledge there, the other hand's wheel opens, a gun is picked into that hand, it
// fires, the body still hangs; the wheel opened again and the ledge let go: the wheel closes, the fall goes on.  Then the
// same with the hands the other way round.  With -OLOneController=Left|Right the one hand both holds and picks (its gun
// stays put away while it holds, as any gun in a holding hand does) and only that hand is tried.
// Co-op: -OLCoopAuto -OLCoopTest=climbwheel -OLCoopLevel=townex on the host and its client.  The host, then the client
// (once it has seen the host's turn), hangs by the left hand at the same spot and picks a gun into the right from the
// wheel (the host the rifle, the client the shotgun); the other machine checks that gun in the right hand and a bare left
// in the pose it hears, and the body drawn there held still while it hangs.
#include "OutlawsPlayer.h"
#include "OutlawsCoop.h"
#include "OutlawsRemotePlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/WorldSettings.h"
#include "ProceduralMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "MotionControllerComponent.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "OutlawsShotTests.h"

#if WITH_OUTLAWS_TESTS
bool AOutlawsPlayer::ClimbWheelTestHang(int32 HoldHand, const TCHAR* Tag)
{
	// the nearest ledge round the eyes: a wall within 1.2 m at eye height or up to 60 cm over it, with a walkable top just
	// past its face; the holding hand put on its lip and the grab taken as a squeeze takes it
	const TCHAR* Hand = HoldHand == 0 ? TEXT("left") : TEXT("right");
	const FVector Eye = Camera->GetComponentLocation();
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsClimbWheelTest), true, this);
	bool bGrabbed = false;
	for (int32 Lift = 0; Lift <= 160 && !bGrabbed; Lift += 20)
	{
		for (int32 k = 0; k < 24 && !bGrabbed; ++k)
		{
			const FVector Dir = FRotator(0.0, k * 15.0, 0.0).Vector();
			const FVector From = Eye + FVector(0.0, 0.0, Lift);
			FHitResult Wall;
			if (!GetWorld()->LineTraceSingleByChannel(Wall, From, From + Dir * 160.0, ECC_Visibility, Q) || FMath::Abs(Wall.ImpactNormal.Z) > 0.3) continue;
			const FVector In = -FVector(Wall.ImpactNormal.X, Wall.ImpactNormal.Y, 0.0).GetSafeNormal();
			FHitResult Top;
			const FVector Probe = FVector(Wall.ImpactPoint) + In * 10.0;
			if (!GetWorld()->LineTraceSingleByChannel(Top, Probe + FVector(0.0, 0.0, 90.0), Probe - FVector(0.0, 0.0, 40.0), ECC_Visibility, Q) || Top.bStartPenetrating
				|| Top.ImpactNormal.Z < 0.5) continue;
			GripOf(HoldHand)->SetWorldLocation(FVector(Wall.ImpactPoint) - In * 3.0 + FVector(0.0, 0.0, Top.ImpactPoint.Z - Wall.ImpactPoint.Z - 10.0));
			if (TryGrab(HoldHand))
			{
				bGrabbed = true;
				// the body up under the hand, as a hang has it (the eyes 25 cm under the grip; a desktop's body does not
				// follow the hand by itself)
				const double Up = (GrabPoint.Z - 25.0) - Camera->GetComponentLocation().Z;
				if (Up > 0.0) SetActorLocation(GetActorLocation() + FVector(0.0, 0.0, Up), true, nullptr, ETeleportType::TeleportPhysics);
				GripOf(HoldHand)->SetWorldLocation(GrabPoint);
				UE_LOG(LogOutlaws, Display, TEXT("%s: the %s hand holds a ledge (top z %.0f, %d degrees, %d cm over the eyes; the body lifted %.0f cm, feet z %.0f)"), Tag,
					Hand, LedgeTopZ, k * 15, Lift, FMath::Max(0.0, Up), GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
			}
		}
	}
	if (!bGrabbed)
	{
		// (no ledge found round the spot: the hang made by hand, as TryGrab makes it)
		GrabHand = HoldHand;
		GrabPoint = GripOf(HoldHand)->GetComponentLocation();
		LedgeTopZ = float(GrabPoint.Z + 4.0);
		LedgeIn = GetActorForwardVector();
		GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		GetCharacterMovement()->Velocity = FVector::ZeroVector;
		UE_LOG(LogOutlaws, Display, TEXT("%s: no ledge found round the spot, the hang set by hand"), Tag);
	}
	return IsClimbing() && GrabHand == HoldHand;
}

void AOutlawsPlayer::ClimbWheelTestPrepare()
{
	// bare hands, every weapon, stood at the snapshot's spot (the body at X 10487.8 Y 1359.9 Z 280.6, the eyes 83 cm over it)
	if (AOutlawsLevelActor* L = Level.Get()) L->GiveAllWeapons();
	CloseWheel();
	if (IsClimbing()) ReleaseGrab(false);
	Equip(0, -1);
	Equip(1, -1);
	TeleportTo(FVector(10487.840, 1359.898, 280.610), FRotator(0.0, 171.07, 0.0), false, true);
}

int32 AOutlawsPlayer::ClimbWheelTestPick(int32 PickHand, int32 Weapon, bool& bOutOpened)
{
	// the wheel opened on PickHand, Weapon pointed at as the laser would, the button let go of: what the hand holds after
	OpenWheel(PickHand);
	bOutOpened = bWheelOpen && bWheelHeld[PickHand & 1];
	WheelPick[PickHand & 1] = WheelWeapon.IndexOfByKey(Weapon);
	ReleaseWheel(PickHand);
	return Equipped[PickHand & 1];
}

void AOutlawsPlayer::ClimbWheelTestLetGo(int32 HoldHand)
{
	// let go with the hand high (a fall, not the climb onto the top)
	GripOf(HoldHand)->SetWorldLocation(Camera->GetComponentLocation() + FVector(0.0, 0.0, 20.0));
	ReleaseGrab(false);
}

void AOutlawsPlayer::ClimbWheelTestTick()
{
	AOutlawsLevelActor* L = Level.Get();
	static int32 Round = 0;           // 0: the first holding hand, 1: the other
	static int32 HoldHand = 0, PickHand = 1;
	static int32 Fails = 0, Passes = 0;
	static FVector HangAt = FVector::ZeroVector;
	static double T0 = 3.0;
	static int32 SlotPicked = -1, WeaponPicked = -1;
	auto Check = [](bool bOk, const FString& What)
	{
		(bOk ? Passes : Fails) += 1;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT climbwheel %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
	};
	auto HandName = [](int32 H) { return H == 0 ? TEXT("left") : TEXT("right"); };
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT climbwheel RESULT %s (%d pass, %d fail)"), Fails == 0 && Passes > 0 ? TEXT("PASS") : TEXT("FAIL"), Passes, Fails);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	};
	const int32 Rounds = OneHand() >= 0 ? 1 : 2;
	const int32 Step = ShotStep % 10;
	if (Step == 0 && ShotTime > T0 && L)
	{
		if (Round >= Rounds) { Finish(); return; }
		HoldHand = OneHand() >= 0 ? OneHand() : (Round == 0 ? 0 : 1);
		PickHand = OneHand() >= 0 ? OneHand() : 1 - HoldHand;
		if (Round == 0) L->GiveAllWeapons();
		CloseWheel();
		if (IsClimbing()) ReleaseGrab(false);
		Equip(0, -1);
		Equip(1, -1);
		// the snapshot's spot: the body at X 10487.8 Y 1359.9 Z 280.6, the eyes 83 cm over it
		TeleportTo(FVector(10487.840, 1359.898, 280.610), FRotator(0.0, 171.07, 0.0), false, true);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT climbwheel round %d: holding with the %s hand, the wheel on the %s (one controller %d), sector %d"), Round, HandName(HoldHand),
			HandName(PickHand), OneHand(), L->PlayerSectorIndex());
		++ShotStep;
	}
	else if (Step == 1 && ShotTime > T0 + 1.0)
	{
		ClimbWheelTestHang(HoldHand, *FString::Printf(TEXT("OLSHOT climbwheel round %d"), Round));
		HangAt = GetActorLocation();
		Check(IsClimbing() && GrabHand == HoldHand, FString::Printf(TEXT("round %d: hanging by the %s hand (mode %d)"), Round, HandName(HoldHand), int32(GetCharacterMovement()->MovementMode)));
		++ShotStep;
	}
	else if (Step == 2 && ShotTime > T0 + 1.5)
	{
		OpenWheel(PickHand);
		Check(bWheelOpen && bWheelHeld[PickHand], FString::Printf(TEXT("round %d: the wheel opens on the %s hand while the %s holds the ledge"), Round, HandName(PickHand), HandName(HoldHand)));
		Check(WheelMapPage && WheelMapPage->IsVisible() && WheelMapArrow && WheelMapArrow->IsVisible(), FString::Printf(TEXT("round %d: the map and its arrow in the wheel's middle (REQ-WHEELMAP)"), Round));
		// the rifle (round 0) or the revolver (round 1) pointed at, as the laser would
		WeaponPicked = Round == 0 ? 1 : 0;
		SlotPicked = WheelWeapon.IndexOfByKey(WeaponPicked);
		WheelPick[PickHand] = SlotPicked;
		++ShotStep;
	}
	else if (Step == 3 && ShotTime > T0 + 2.0)
	{
		ReleaseWheel(PickHand);
		Check(Equipped[PickHand] == WeaponPicked, FString::Printf(TEXT("round %d: the %s hand took weapon %d from slot %d (holds %d)"), Round, HandName(PickHand), WeaponPicked, SlotPicked, Equipped[PickHand]));
		Check(!bWheelOpen, FString::Printf(TEXT("round %d: the wheel put away by the button's release"), Round));
		Check(IsClimbing() && GrabHand == HoldHand, FString::Printf(TEXT("round %d: the %s hand still holds the ledge after the pick"), Round, HandName(HoldHand)));
		if (OneHand() < 0)
		{
			Check(Equipped[HoldHand] < 0, FString::Printf(TEXT("round %d: the holding hand holds no weapon (%d)"), Round, Equipped[HoldHand]));
		}
		++ShotStep;
	}
	else if (Step == 4 && ShotTime > T0 + 2.5)
	{
		if (OneHand() < 0)
		{
			const int32 Before = Mag[PickHand];
			FireCooldown[PickHand] = 0.f;
			FireGun(PickHand, false);
			Check(Mag[PickHand] < Before, FString::Printf(TEXT("round %d: the %s hand's gun fires while hanging (rounds %d to %d)"), Round, HandName(PickHand), Before, Mag[PickHand]));
		}
		else
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT climbwheel round %d: one controller: the gun waits in the holding hand, put away there in VR (UpdateGuns; a desktop run shows %d)"), Round,
				WeaponMesh[PickHand] && WeaponMesh[PickHand]->IsVisible() ? 1 : 0);
		}
		++ShotStep;
	}
	else if (Step == 5 && ShotTime > T0 + 3.5)
	{
		const double Moved = FVector::Dist(GetActorLocation(), HangAt);
		Check(IsClimbing() && GrabHand == HoldHand && Moved < 10.0, FString::Printf(TEXT("round %d: a second on still hanging (moved %.1f cm, mode %d)"), Round, Moved,
			int32(GetCharacterMovement()->MovementMode)));
		// the wheel up again, then the ledge let go of
		OpenWheel(PickHand);
		Check(bWheelOpen, FString::Printf(TEXT("round %d: the wheel opens again"), Round));
		++ShotStep;
	}
	else if (Step == 6 && ShotTime > T0 + 4.0)
	{
		// let go with the hand high (a fall, not the climb onto the top): the grip lifted over the eyes first
		GripOf(HoldHand)->SetWorldLocation(Camera->GetComponentLocation() + FVector(0.0, 0.0, 20.0));
		ReleaseGrab(false);
		Check(!bWheelOpen && !bWheelHeld[0] && !bWheelHeld[1], FString::Printf(TEXT("round %d: letting go of the ledge put the wheel away"), Round));
		Check(!IsClimbing() && GetCharacterMovement()->MovementMode == MOVE_Falling, FString::Printf(TEXT("round %d: the body falls (mode %d)"), Round, int32(GetCharacterMovement()->MovementMode)));
		// the button let go of after: nothing more happens
		ReleaseWheel(PickHand);
		HangAt = GetActorLocation();
		++ShotStep;
	}
	else if (Step == 7 && ShotTime > T0 + 5.5)
	{
		const double Fell = HangAt.Z - GetActorLocation().Z;
		Check(!bWheelOpen && GetCharacterMovement()->MovementMode != MOVE_Flying, FString::Printf(TEXT("round %d: 1.5 s on the wheel is away and the body is down (fell %.0f cm, mode %d, time dilation %.2f)"),
			Round, Fell, int32(GetCharacterMovement()->MovementMode), GetWorld()->GetWorldSettings()->TimeDilation));
		++Round;
		T0 = ShotTime + 0.5;
		ShotStep = Round * 10;
	}
}

OL_SHOT_MATCH(OLClimbWheelTest) { return OLFlags::Param(TEXT("OLClimbWheelTest")); }
OL_SHOT_TEST(OLClimbWheelTest, 270)
{
	ClimbWheelTestTick();            // (REQ-CLIMBWHEEL: OutlawsClimbWheel.cpp)
}

void UOutlawsCoop::TickClimbWheelTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	const bool bHost = IsHost();
	const TCHAR* Me = bHost ? TEXT("host") : TEXT("client");
	const TCHAR* Them = bHost ? TEXT("client") : TEXT("host");
	const int32 MyGun = bHost ? 1 : 2;               // the host picks the rifle, the client the shotgun
	const int32 TheirGun = bHost ? 2 : 1;
	L->bInvincible = true;
	auto Check = [this, Me](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLCOOP climbwheel check %s (%s): %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), Me, *What);
		if (bOk) ++CwPasses; else ++CwFails;
	};
	// the partner's turn as heard here: its right hand bare first (its turn's start), then its gun there; that seen, 1.5 s on
	// the gun still in the pose, the left bare, and its body drawn here no more than 10 cm from where it was
	const AOutlawsCoopLink* PL = GetLink(PartnerIndex());
	const AOutlawsRemotePlayer* Them3D = PartnerPuppet();
	if (PL && PL->Pose.Seq > 0 && Them3D && !bCwWatchDone && CwStep > 0)
	{
		if (!bCwArmed && PL->Pose.RightWeapon == -1) bCwArmed = true;
		if (bCwArmed && CwSeenAt < 0.0 && PL->Pose.RightWeapon == TheirGun)
		{
			CwSeenAt = Now;
			CwSeenBody = Them3D->ShownBody();
			UE_LOG(LogOutlaws, Log, TEXT("OLCOOP climbwheel: the %s sees the %s's pick: L %d R %d, body drawn at %s"), Me, Them, PL->Pose.LeftWeapon, PL->Pose.RightWeapon, *CwSeenBody.ToCompactString());
		}
		if (CwSeenAt >= 0.0 && Now > CwSeenAt + 1.5)
		{
			const double Moved = FVector::Dist(Them3D->ShownBody(), CwSeenBody);
			Check(PL->Pose.RightWeapon == TheirGun && PL->Pose.LeftWeapon == -1, FString::Printf(TEXT("the %s's pick shows here: right hand weapon %d (want %d), left %d (want -1, it holds the ledge)"), Them,
				PL->Pose.RightWeapon, TheirGun, PL->Pose.LeftWeapon));
			Check(Moved < 10.0, FString::Printf(TEXT("the %s hangs still as drawn here (moved %.1f cm in 1.5 s, at %s)"), Them, Moved, *Them3D->ShownBody().ToCompactString()));
			bCwWatchDone = true;
		}
	}
	switch (CwStep)
	{
	case 0:
		if (!L->LevelName.Equals(TEXT("townex"), ESearchCase::IgnoreCase) || !L->HasActorBegunPlay()) return;
		if (bHost)
		{
			const AOutlawsCoopLink* CL = GetLink(PartnerIndex());
			if (!CL || CL->BuiltSerial != StartedSerial || !PartnerPuppet()) return;
		}
		CwHome = P->GetActorLocation();
		CwAt = Now + 4.0;
		CwStep = 1;
		return;
	case 1:
		// the client waits for the host's turn, as heard (or 60 s)
		if (Now < CwAt || (!bHost && !bCwWatchDone && Now < CwAt + 60.0)) return;
		if (!bHost && !bCwWatchDone) Check(false, TEXT("the host's pick heard before the client's turn"));
		P->ClimbWheelTestPrepare();
		UE_LOG(LogOutlaws, Log, TEXT("OLCOOP climbwheel: the %s's turn at the snapshot spot (sector %d)"), Me, L->PlayerSectorIndex());
		CwAt = Now + 1.0;
		CwStep = 2;
		return;
	case 2:
		if (Now < CwAt) return;
		Check(P->ClimbWheelTestHang(0, *FString::Printf(TEXT("OLCOOP climbwheel (%s)"), Me)), FString::Printf(TEXT("hanging by the left hand (mode %d)"), int32(P->GetCharacterMovement()->MovementMode)));
		CwHangAt = P->GetActorLocation();
		CwAt = Now + 1.0;
		CwStep = 3;
		return;
	case 3:
	{
		if (Now < CwAt) return;
		bool bOpened = false;
		const int32 Got = P->ClimbWheelTestPick(1, MyGun, bOpened);
		Check(bOpened, TEXT("the wheel opens on the right hand while the left holds the ledge"));
		Check(Got == MyGun, FString::Printf(TEXT("the right hand took weapon %d (holds %d)"), MyGun, Got));
		Check(P->IsClimbing(), TEXT("the left hand still holds the ledge after the pick"));
		CwAt = Now + 3.0;                         // (the other machine looks meanwhile)
		CwStep = 4;
		return;
	}
	case 4:
	{
		if (Now < CwAt) return;
		const double Moved = FVector::Dist(P->GetActorLocation(), CwHangAt);
		int32 W = -1;
		FString Tool;
		P->CoopHeld(1, W, Tool);
		Check(P->IsClimbing() && Moved < 10.0 && W == MyGun, FString::Printf(TEXT("3 s on still hanging with the gun (moved %.1f cm, mode %d, right hand %d)"), Moved,
			int32(P->GetCharacterMovement()->MovementMode), W));
		P->ClimbWheelTestLetGo(0);
		Check(!P->IsClimbing(), TEXT("the ledge let go of"));
		CwAt = Now + 2.0;
		CwStep = 5;
		return;
	}
	case 5:
		if (Now < CwAt) return;
		// out of the way for the other's turn at the same spot
		P->TeleportTo(CwHome, P->GetActorRotation(), false, true);
		if (!bHost)
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLCOOP climbwheel client RESULT %s (%d passed, %d failed)"), CwFails == 0 && CwPasses > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), CwPasses, CwFails);
			CwStep = 9;
			return;
		}
		// the host: the client's turn next; its pick heard here (or 60 s)
		bCwArmed = false;
		CwAt = Now + 60.0;
		CwStep = 6;
		return;
	case 6:
		if (!bCwWatchDone && Now < CwAt) return;
		if (!bCwWatchDone) Check(false, TEXT("the client's pick heard"));
		UE_LOG(LogOutlaws, Log, TEXT("OLCOOP climbwheel host RESULT %s (%d passed, %d failed)"), CwFails == 0 && CwPasses > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), CwPasses, CwFails);
		CwAt = Now + 8.0;                         // (the client's own turn ends and its result is logged first)
		CwStep = 7;
		return;
	case 7:
		if (Now < CwAt) return;
		TestSay(TEXT("quit"), 0);
		CwAt = Now + 2.0;
		CwStep = 8;
		return;
	case 8:
		if (Now < CwAt) return;
		Summary(TEXT("done"));
		QuitForTest();
		CwStep = 9;
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
