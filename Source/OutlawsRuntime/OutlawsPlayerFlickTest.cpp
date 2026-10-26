// -OLShots -OLLevel=hideout -OLNoFrontEnd -OLFlickTest (REQ-FLICKRELOAD, single player): the double guns' break, driven through the hand's
// own frame turning at 90 Hz as a controller would (OutlawsPlayerWeapons.cpp: FlickStep, TickBreak):
//   a forward flick opens the gun and throws out its fired shells (2, or 1 with a live one left in), the trigger does nothing open;
//   a flick back shuts it loaded from the reserve (one carried: one goes in; none: nothing, no free shells);
//   the sawed-off in the left hand the same; a flick back inside the cooldown is not taken;
//   the reload button (B) does the same break, a shell at a time, then shuts;
//   aim sweeps (side to side, fast up and down, diagonal, a fast mostly-sideways snap, tracking jitter) and a melee swing never break it;
//   the revolver and rifle never break;
//   (BUG-FLICKFEEL) a lighter flick (about 25 degrees at 550 deg/s), a flick carrying the hand along the barrel at 300 cm/s, a flick
//   with the forearm's roll in it (left hand) and a fast open-shut pair under 0.4 s all count; one shell loaded on the sawed-off
//   opens and shuts topped up; a chop across the barrel and a throw-fast hand still never break it;
//   (REQ-FLICKRULES) a flick open on a fully loaded gun and any flick held two-handed are refused (each logged with why); the reload
//   button held two-handed: the front hand lets go for the cycle and takes the gun again when it shuts (grip held, hand at the forend),
//   else it stays free (moved away, grip let go); the single-barrel Shotgun breaks too: one shell out, one in, the same rules;
//   (BUG-RELOADCLINK, REQ-RELOADSLICES) each shotgun's flick and button reloads (both barrels, one, an early shut) play the reload
//   wave's open slice, a shell slice per shell going in and the shut slice, an early shut cutting the shells off, never the
//   empty-chamber click; a dry fire empty still clicks (the waves heard: "OLSHOT flick sounds"); (BUG-RELOADPACE) the button
//   reload's slices back to back, the barrels shut as the shut slice starts, one shell 1.0 s at most, two 1.15 ("OLSHOT flick pace");
//   (BUG-FLICKUP) each break gun flicked open and shut with the wrist rolled 0, 30 and 60 degrees each way ("OLSHOT flick rolls");
//   the shells fly, land and lie on the floor, and are gone after two seconds.
// -OLFlickTest -OLFlickShot: the Shotgun side on, photographed shut, a shell leaving the open breech, open and shut again
// (Saved/OLShots/ol_flick_shotgun_<shut|eject|open|shut2>.png).
// Prints "OLSHOT flick check PASS|FAIL: ..." and "OLSHOT flick RESULT".  The two-machine half: OutlawsCoopFlickTest.cpp (-OLFlickNetTest).

#include "OutlawsPlayer.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsBoxHand.h"
#include "ProceduralMeshComponent.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/Controller.h"
#include "UnrealClient.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "OutlawsShotTests.h"

#if WITH_OUTLAWS_TESTS

void AOutlawsPlayer::FlickTestTick(float Dt)
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L || !GetWorld()) return;
	static int32 Fails = 0;
	static float At = 0.f;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT flick check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (!bOk) ++Fails;
	};
	auto Finish = [this]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT flick RESULT %s (%d failed)"), Fails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Fails);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	};
	const FString Ammo = TEXT("ishells");
	// a wrist flick: about 46 degrees in 90 ms, at up to 800 deg/s, a little off its axis, the hand itself barely moving
	auto Fwd = [this](int32 H) { TestHandMotion(H, -800.f, 40.f, 30.f, 0.09f, 60.f); };
	auto Back = [this](int32 H) { TestHandMotion(H, 800.f, -40.f, -30.f, 0.09f, 60.f); };
	auto Hand = [](int32 H) { return H == 0 ? TEXT("left") : TEXT("right"); };

	if (OLFlags::Param(TEXT("OLFlickShot")))
	{
		// the Shotgun side on before the eyes, its barrel to the right (as -OLHeldGunTest frames a gun), held each frame
		static float Yaw = -1000.f;
		static float StepAt = 0.f;
		if (Yaw < -999.f) Yaw = static_cast<float>(GetActorRotation().Yaw);
		if (Controller) Controller->SetControlRotation(FRotator(0.0, Yaw, 0.0));
		const FVector Eye = Camera->GetComponentLocation();
		const FVector F = Camera->GetForwardVector(), R = Camera->GetRightVector();
		if (LeftHand) LeftHand->SetVisibility(false, true);
		if (UOutlawsBoxHand* Box = RightHand.Get())
		{
			Box->SetVisibility(true, true);
			Box->SetCurl(1.f, 1.f);
		}
		if (USceneComponent* Fr = FrameOf(1))
		{
			const FBox B = GunBounds[1];
			const double Span = B.IsValid ? B.GetSize().X : 30.0;
			const double Middle = B.IsValid ? B.GetCenter().X : 10.0;
			Fr->SetWorldLocationAndRotation(Eye + F * FMath::Max(40.0, Span * 0.75) - R * Middle - FVector::UpVector * 4.0, FRotationMatrix::MakeFromXZ(R, FVector::UpVector).ToQuat());
		}
		auto Shot = [this](const TCHAR* Name)
		{
			FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(Name) + TEXT(".png"), false, false);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT flick shot %s: %s, barrels %.2f open, hinge %s"), Name, bBreakOpen[1] ? TEXT("open") : TEXT("shut"), BreakShown[1], *BreakHinge[1].ToCompactString());
		};
		// the barrels' swing each frame (the desktop runs no gun update of its own)
		if (ShotStep > 0) TickBreak(1, Dt, false);
		const float T = ShotTime - StepAt;
		switch (ShotStep)
		{
		case 0: if (ShotTime < 4.f) return; TestBreakArm(1, 2, 0, 10); break;
		case 1: if (T < 1.0f) return; Shot(TEXT("ol_flick_shotgun_shut")); break;
		case 2: if (T < 0.5f) return; OpenBreak(1, TEXT("the screenshot")); break;
		case 3: if (T < 0.07f) return; Shot(TEXT("ol_flick_shotgun_eject")); break;
		case 4: if (T < 1.0f) return; Shot(TEXT("ol_flick_shotgun_open")); break;
		case 5: if (T < 0.5f) return; CloseBreak(1, TEXT("the screenshot")); break;
		case 6: if (T < 0.5f) return; Shot(TEXT("ol_flick_shotgun_shut2")); break;
		default:
			if (T < 1.0f) return;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT flick shot RESULT done"));
			FGenericPlatformMisc::RequestExit(false);
			ShotTime = -1.f;
			return;
		}
		StepAt = ShotTime;
		++ShotStep;
		return;
	}

	if (ShotStep == 0)
	{
		if (ShotTime < 4.f) return;
		const bool bFace = bFaceReload;
		bFaceReload = true;                      // (the trial layout's B reloads the right hand, as shipped)

		// 1. the double barrel, both barrels fired, ten shells carried
		TestBreakArm(1, 3, 0, 10);
		Check(Equipped[1] == 3 && WeaponMesh[1], FString::Printf(TEXT("the %s in the right hand"), *WeaponLabel(3)));
		Check(BreakPivot[1] && BarrelMesh[1] && BarrelMesh[1]->GetNumSections() > 0 && WeaponMesh[1]->GetNumSections() > 0,
			FString::Printf(TEXT("its barrels are their own part on a hinge at %s"), *BreakHinge[1].ToCompactString()));
		int32 S0 = ShellsOut;
		Fwd(1);
		Check(bBreakOpen[1] && ShellsOut - S0 == 2 && Mag[1] == 0, FString::Printf(TEXT("a forward flick opens it: %s, %d shells out (want 2), %d loaded"),
			bBreakOpen[1] ? TEXT("open") : TEXT("shut"), ShellsOut - S0, Mag[1]));
		TestHandStill(1, 0.3f);
		const double Swung = BreakPivot[1] ? BreakPivot[1]->GetRelativeRotation().Pitch : 0.0;
		Check(BreakShown[1] > 0.99f && FMath::Abs(Swung + 40.0) < 0.5, FString::Printf(TEXT("the barrels swung down %.1f degrees (want 40)"), -Swung));
		FireGun(1, false);
		Check(bBreakOpen[1] && Mag[1] == 0, TEXT("the trigger fires nothing while it is open"));
		FireCooldown[1] = 0.f;
		Back(1);
		Check(!bBreakOpen[1] && Mag[1] == 2 && L->GetCount(Ammo) == 8, FString::Printf(TEXT("a flick back shuts it loaded: %s, %d loaded (want 2), %d carried (want 8)"),
			bBreakOpen[1] ? TEXT("open") : TEXT("shut"), Mag[1], L->GetCount(Ammo)));
		TestHandStill(1, 0.3f);
		Check(BreakShown[1] < 0.01f, TEXT("the barrels swung home"));

		// 2. one barrel fired: one shell out, the live one stays
		TestBreakArm(1, 3, 1, 10);
		S0 = ShellsOut;
		Fwd(1);
		Check(bBreakOpen[1] && ShellsOut - S0 == 1 && Mag[1] == 1, FString::Printf(TEXT("one fired: %d shell out (want 1), %d still loaded"), ShellsOut - S0, Mag[1]));
		TestHandStill(1, 0.5f);
		Back(1);
		Check(!bBreakOpen[1] && Mag[1] == 2 && L->GetCount(Ammo) == 9, FString::Printf(TEXT("shut: %d loaded, %d carried (want 2, 9)"), Mag[1], L->GetCount(Ammo)));

		// 3. one shell carried: a partial reload
		TestBreakArm(1, 3, 0, 1);
		Fwd(1);
		TestHandStill(1, 0.5f);
		Back(1);
		Check(!bBreakOpen[1] && Mag[1] == 1 && L->GetCount(Ammo) == 0, FString::Printf(TEXT("one shell carried: %d loaded (want 1), %d carried (want 0)"), Mag[1], L->GetCount(Ammo)));

		// 4. none carried: shut empty, no free shells
		TestBreakArm(1, 3, 0, 0);
		Fwd(1);
		const bool bOpenedEmpty = bBreakOpen[1];
		TestHandStill(1, 0.5f);
		Back(1);
		Check(bOpenedEmpty && !bBreakOpen[1] && Mag[1] == 0 && L->GetCount(Ammo) == 0, FString::Printf(TEXT("none carried: opened, shut with %d loaded (want 0)"), Mag[1]));

		// 5. the sawed-off in the left hand
		TestBreakArm(0, 4, 0, 6);
		S0 = ShellsOut;
		Fwd(0);
		Check(Equipped[0] == 4 && bBreakOpen[0] && ShellsOut - S0 == 2, FString::Printf(TEXT("the %s in the %s hand: open, %d shells out"), *WeaponLabel(4), Hand(0), ShellsOut - S0));
		TestHandStill(0, 0.5f);
		Back(0);
		Check(!bBreakOpen[0] && Mag[0] == 2 && L->GetCount(Ammo) == 4, FString::Printf(TEXT("shut: %d loaded, %d carried (want 2, 4)"), Mag[0], L->GetCount(Ammo)));

		// 6. a flick back inside the cooldown is the same flick's bounce: not taken; a later one is
		TestBreakArm(1, 3, 0, 10);
		Fwd(1);
		TestHandStill(1, 0.05f);
		Back(1);
		Check(bBreakOpen[1], TEXT("a flick back 0.05 s after the flick forward leaves it open (the cooldown)"));
		TestHandStill(1, 0.5f);
		Back(1);
		Check(!bBreakOpen[1] && Mag[1] == 2, TEXT("and one 0.5 s later shuts it, loaded"));

		// 6b. (BUG-FLICKFEEL) a fast open-shut pair: open, 0.2 s still, shut (0.38 s in all)
		TestBreakArm(1, 3, 0, 10);
		Fwd(1);
		TestHandStill(1, 0.2f);
		Back(1);
		Check(!bBreakOpen[1] && Mag[1] == 2, FString::Printf(TEXT("a fast open-shut pair under 0.4 s: %s, %d loaded (want shut, 2)"), bBreakOpen[1] ? TEXT("open") : TEXT("shut"), Mag[1]));
		// a lighter flick: about 25 degrees in 80 ms at up to 550 deg/s (under the old 25 degrees at 600)
		TestBreakArm(1, 3, 0, 10);
		TestHandMotion(1, -550.f, 15.f, 10.f, 0.08f, 60.f);
		const bool bLightOpen = bBreakOpen[1];
		TestHandStill(1, 0.3f);
		TestHandMotion(1, 550.f, -15.f, -10.f, 0.08f, 60.f);
		Check(bLightOpen && !bBreakOpen[1], FString::Printf(TEXT("a lighter flick (25 deg at 550 deg/s) opens (%d) and shuts (%d)"), bLightOpen ? 1 : 0, bBreakOpen[1] ? 0 : 1));
		// the hand carried along the barrel at 300 cm/s with the flick (past the old 200 cm/s void)
		TestBreakArm(1, 3, 0, 10);
		TestHandMotion(1, -800.f, 40.f, 30.f, 0.09f, 300.f);
		const bool bCarryOpen = bBreakOpen[1];
		TestHandStill(1, 0.3f);
		TestHandMotion(1, 800.f, -40.f, -30.f, 0.09f, 300.f);
		Check(bCarryOpen && !bBreakOpen[1], FString::Printf(TEXT("a flick carrying the hand along the barrel at 300 cm/s opens (%d) and shuts (%d)"), bCarryOpen ? 1 : 0, bBreakOpen[1] ? 0 : 1));
		// the forearm's roll riding along (the left hand, the sawed-off): 46 degrees down with 34 of roll
		TestBreakArm(0, 4, 0, 6);
		TestHandMotion(0, -800.f, 60.f, 600.f, 0.09f, 60.f);
		const bool bRollOpen = bBreakOpen[0];
		TestHandStill(0, 0.3f);
		TestHandMotion(0, 800.f, -60.f, -600.f, 0.09f, 60.f);
		Check(bRollOpen && !bBreakOpen[0], FString::Printf(TEXT("a left-hand flick with the forearm's roll in it opens (%d) and shuts (%d)"), bRollOpen ? 1 : 0, bBreakOpen[0] ? 0 : 1));
		// one shell loaded on the sawed-off: one out, the live one stays, the shut tops it up
		TestBreakArm(0, 4, 1, 6);
		S0 = ShellsOut;
		Fwd(0);
		Check(bBreakOpen[0] && ShellsOut - S0 == 1 && Mag[0] == 1, FString::Printf(TEXT("the sawed-off with one live: open, %d shell out (want 1), %d still loaded (want 1)"), ShellsOut - S0, Mag[0]));
		TestHandStill(0, 0.3f);
		Back(0);
		Check(!bBreakOpen[0] && Mag[0] == 2 && L->GetCount(Ammo) == 5, FString::Printf(TEXT("and shut: %d loaded, %d carried (want 2, 5)"), Mag[0], L->GetCount(Ammo)));

		// 7. the reload button: the same break, a shell at a time at the reload's pace, then shut
		TestBreakArm(1, 3, 0, 10);
		S0 = ShellsOut;
		PressB();
		TestHandStill(1, 0.05f);
		Check(bBreakOpen[1] && ShellsOut - S0 == 2 && Mag[1] == 0, FString::Printf(TEXT("B opens it: %d shells out"), ShellsOut - S0));
		TestHandStill(1, 0.25f);
		Check(bBreakOpen[1] && Mag[1] == 1, FString::Printf(TEXT("B: 0.3 s on, a shell in (%d loaded), still open"), Mag[1]));
		TestHandStill(1, 1.0f);
		Check(!bBreakOpen[1] && Mag[1] == 2 && L->GetCount(Ammo) == 8, FString::Printf(TEXT("B: shut, %d loaded, %d carried (want 2, 8)"), Mag[1], L->GetCount(Ammo)));
		TestBreakArm(1, 3, 0, 1);
		PressB();
		TestHandStill(1, 1.5f);
		Check(!bBreakOpen[1] && Mag[1] == 1 && L->GetCount(Ammo) == 0, FString::Printf(TEXT("B with one shell carried: shut, %d loaded (want 1)"), Mag[1]));
		TestBreakArm(1, 3, 2, 10);
		const int32 Opens0 = FlickOpens;
		PressB();
		TestHandStill(1, 0.3f);
		Check(FlickOpens == Opens0 && !bBreakOpen[1], TEXT("B with both barrels loaded: nothing to do, it stays shut"));

		// 7b. (REQ-FLICKRULES) a fully loaded gun: a flick forward is refused (one spent opens: case 2)
		TestBreakArm(1, 3, 2, 10);
		int32 Full0 = FlickRefusedFull;
		Fwd(1);
		Check(!bBreakOpen[1] && Mag[1] == 2 && FlickRefusedFull == Full0 + 1, FString::Printf(TEXT("a flick forward on a full double barrel is refused (%s, %d refused as full)"),
			bBreakOpen[1] ? TEXT("open") : TEXT("shut"), FlickRefusedFull - Full0));
		// two hands on the gun: a flick forward refused, and a flick back on a gun opened one-handed refused; B still shuts it
		TestBreakArm(1, 3, 0, 10);
		TestSetTwoHand(1, true);
		int32 Two0 = FlickRefusedTwoHands;
		Fwd(1);
		Check(!bBreakOpen[1] && Mag[1] == 0 && FlickRefusedTwoHands == Two0 + 1, FString::Printf(TEXT("held two-handed: a flick forward is refused (%s, %d refused)"),
			bBreakOpen[1] ? TEXT("open") : TEXT("shut"), FlickRefusedTwoHands - Two0));
		TestSetTwoHand(1, false);
		TestHandStill(1, 0.3f);
		Fwd(1);
		TestHandStill(1, 0.3f);
		TestSetTwoHand(1, true);
		Two0 = FlickRefusedTwoHands;
		Back(1);
		Check(bBreakOpen[1] && FlickRefusedTwoHands == Two0 + 1, FString::Printf(TEXT("opened one-handed, then held two-handed: a flick back is refused (%s)"), bBreakOpen[1] ? TEXT("open") : TEXT("shut")));
		PressB();
		TestHandStill(1, 0.1f);
		Check(!bBreakOpen[1] && Mag[1] == 2, FString::Printf(TEXT("and B shuts it two-handed, %d loaded"), Mag[1]));

		// 7c. B held two-handed: the front hand lets go for the cycle and takes the gun again when it shuts (grip held, at the forend)
		TestAxes.Add(TEXT("OLVR_LeftGrip"), 1.f);
		TestBreakArm(1, 3, 0, 10);
		TestSetTwoHand(1, true);
		TestFrontHandAt(1, 0.f);
		Check(NearForegrip(0), TEXT("the left hand is at the double barrel's forend"));
		PressB();
		TestHandStill(1, 0.05f);
		Check(bBreakOpen[1] && !TestTwoHand(1) && bRegrip[0], FString::Printf(TEXT("B two-handed: open (%d), the front hand let go (%d)"), bBreakOpen[1] ? 1 : 0, TestTwoHand(1) ? 0 : 1));
		TestHandStill(1, 1.5f);
		Check(!bBreakOpen[1] && Mag[1] == 2 && TestTwoHand(1) && !bRegrip[0], FString::Printf(TEXT("B two-handed: shut, %d loaded, the front hand holds the gun again (%d)"), Mag[1], TestTwoHand(1) ? 1 : 0));
		// the hand moved away during the cycle: it stays free
		TestBreakArm(1, 3, 0, 10);
		TestSetTwoHand(1, true);
		TestFrontHandAt(1, 0.f);
		PressB();
		TestHandStill(1, 0.05f);
		TestFrontHandAt(1, 40.f);
		TestHandStill(1, 1.5f);
		Check(!bBreakOpen[1] && !TestTwoHand(1) && !bRegrip[0], FString::Printf(TEXT("B two-handed, the front hand moved 40 cm away: shut, the hand stays free (%d)"), TestTwoHand(1) ? 0 : 1));
		// the grip let go during the cycle: it stays free
		TestBreakArm(1, 3, 0, 10);
		TestSetTwoHand(1, true);
		TestFrontHandAt(1, 0.f);
		PressB();
		TestHandStill(1, 0.05f);
		TestAxes.Add(TEXT("OLVR_LeftGrip"), 0.f);
		TestHandStill(1, 1.5f);
		Check(!bBreakOpen[1] && !TestTwoHand(1), FString::Printf(TEXT("B two-handed, the grip let go: shut, the hand stays free (%d)"), TestTwoHand(1) ? 0 : 1));

		// 7d. the single-barrel Shotgun breaks too: one shell out, one in, the same rules
		TestBreakArm(1, 2, 0, 10);
		Check(Equipped[1] == 2 && BarrelMesh[1] && BarrelMesh[1]->GetNumSections() > 0 && WeaponMesh[1]->GetNumSections() > 0,
			FString::Printf(TEXT("the %s's barrel is its own part on a hinge at %s"), *WeaponLabel(2), *BreakHinge[1].ToCompactString()));
		S0 = ShellsOut;
		Fwd(1);
		Check(bBreakOpen[1] && ShellsOut - S0 == 1 && Mag[1] == 0, FString::Printf(TEXT("the %s fired: a forward flick opens it, %d shell out (want 1)"), *WeaponLabel(2), ShellsOut - S0));
		TestHandStill(1, 0.3f);
		const double SgSwung = BreakPivot[1] ? BreakPivot[1]->GetRelativeRotation().Pitch : 0.0;
		Check(FMath::Abs(SgSwung + 40.0) < 0.5, FString::Printf(TEXT("its barrel swung down %.1f degrees (want 40)"), -SgSwung));
		Back(1);
		Check(!bBreakOpen[1] && Mag[1] == 1 && L->GetCount(Ammo) == 9, FString::Printf(TEXT("a flick back shuts it: %d loaded, %d carried (want 1, 9)"), Mag[1], L->GetCount(Ammo)));
		TestHandStill(1, 0.3f);
		Full0 = FlickRefusedFull;
		Fwd(1);
		Check(!bBreakOpen[1] && FlickRefusedFull == Full0 + 1, TEXT("loaded (its one shell): a flick forward is refused"));
		TestBreakArm(1, 2, 0, 10);
		TestSetTwoHand(1, true);
		Two0 = FlickRefusedTwoHands;
		Fwd(1);
		Check(!bBreakOpen[1] && FlickRefusedTwoHands == Two0 + 1, TEXT("held two-handed: a flick forward is refused"));
		TestSetTwoHand(1, false);
		S0 = ShellsOut;
		PressB();
		TestHandStill(1, 0.05f);
		Check(bBreakOpen[1] && ShellsOut - S0 == 1, FString::Printf(TEXT("B opens the %s, %d shell out"), *WeaponLabel(2), ShellsOut - S0));
		TestHandStill(1, 1.5f);
		Check(!bBreakOpen[1] && Mag[1] == 1 && L->GetCount(Ammo) == 9, FString::Printf(TEXT("B: shut, %d loaded, %d carried (want 1, 9)"), Mag[1], L->GetCount(Ammo)));
		TestAxes.Add(TEXT("OLVR_LeftGrip"), 1.f);
		TestBreakArm(1, 2, 0, 10);
		TestSetTwoHand(1, true);
		TestFrontHandAt(1, 0.f);
		PressB();
		TestHandStill(1, 0.05f);
		const bool bSgLetGo = bBreakOpen[1] && !TestTwoHand(1);
		TestHandStill(1, 1.5f);
		Check(bSgLetGo && !bBreakOpen[1] && Mag[1] == 1 && TestTwoHand(1), FString::Printf(TEXT("B two-handed on the %s: the front hand let go (%d) and holds it again after the shut (%d)"),
			*WeaponLabel(2), bSgLetGo ? 1 : 0, TestTwoHand(1) ? 1 : 0));
		TestAxes.Remove(TEXT("OLVR_LeftGrip"));
		TestSetTwoHand(1, false);
		// the Shotgun in the left hand
		TestBreakArm(0, 2, 0, 5);
		S0 = ShellsOut;
		Fwd(0);
		const bool bSgLeftOpen = bBreakOpen[0] && ShellsOut - S0 == 1;
		TestHandStill(0, 0.3f);
		Back(0);
		Check(bSgLeftOpen && !bBreakOpen[0] && Mag[0] == 1 && L->GetCount(Ammo) == 4, FString::Printf(TEXT("the %s in the left hand opens (%d) and shuts, %d loaded, %d carried (want 1, 4)"),
			*WeaponLabel(2), bSgLeftOpen ? 1 : 0, Mag[0], L->GetCount(Ammo)));

		// 7e. (BUG-RELOADCLINK, REQ-RELOADSLICES) the sounds of each shotgun's reload, flick and button: the open slice and a shell
		// slice per shell going in, the shut slice on the shut; an early shut cuts the shells off; never the empty-chamber click or the
		// whole reload wave; a dry fire on an empty gun still clicks.  Letters: O open, S shell, C shut, X click, R the whole wave
		{
			static TArray<FString> Spy;
			static TArray<AOutlawsLevelActor::FRunHeard> Runs;
			AOutlawsLevelActor::WavSpy = &Spy;
			AOutlawsLevelActor::RunSpy = &Runs;
			auto Case = [&](int32 Wi, int32 Loaded, bool bButton, bool bEarly, const TCHAR* Want)
			{
				TestBreakArm(1, Wi, Loaded, 10);
				TestHandStill(1, 1.0f);              // (the last case's slices done)
				Spy.Reset();
				Runs.Reset();
				const int32 Cuts0 = L->SoundRunCuts;
				if (bButton)
				{
					PressB();
					TestHandStill(1, bEarly ? 0.3f : 2.0f);
					if (bEarly) { PressB(); TestHandStill(1, 0.1f); }
				}
				else
				{
					Fwd(1);
					TestHandStill(1, bEarly ? 0.3f : 1.3f);
					Back(1);
					TestHandStill(1, 0.1f);
				}
				const FString Got = AOutlawsLevelActor::ReloadCodes(Spy);
				const int32 Cuts = L->SoundRunCuts - Cuts0;
				const FString What = FString::Printf(TEXT("%s, %s, %s%s"), *WeaponLabel(Wi), bButton ? TEXT("button") : TEXT("flick"), Loaded ? TEXT("one barrel fired") : Wi == 2 ? TEXT("fired") : TEXT("both barrels fired"),
					bEarly ? TEXT(", an early shut") : TEXT(""));
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT flick sounds %s: heard %s (%s), %d cut"), *What, Spy.Num() ? *FString::Join(Spy, TEXT(", ")) : TEXT("nothing"), *Got, Cuts);
				Check(!bBreakOpen[1] && Got == Want && Cuts == (bEarly ? 1 : 0), FString::Printf(TEXT("the reload sounds, %s: %s (want %s), %d cut off (want %d)"), *What, *Got, Want, Cuts, bEarly ? 1 : 0));
				// (BUG-RELOADPACE) when each slice started, from the open's start; the button's run back to back, the barrels shut as the
				// shut slice starts, one shell about the whole wave's 0.97 s and two about 1.1
				if (Runs.Num() == 0) return;
				const double T0 = Runs[0].At;
				FString Times;
				double Gap = 0.0;
				for (int32 i = 0; i < Runs.Num(); ++i)
				{
					Times += FString::Printf(TEXT("%s%s %.3f"), i ? TEXT(", ") : TEXT(""), *Runs[i].Wav.Mid(Runs[i].Wav.Find(TEXT("@")) + 1), Runs[i].At - T0);
					if (i > 0) Gap = FMath::Max(Gap, FMath::Abs(Runs[i].At - (Runs[i - 1].At + Runs[i - 1].Len)));
				}
				const AOutlawsLevelActor::FRunHeard& Last = Runs.Last();
				const double Total = Last.At + Last.Len - T0;
				const double ShutOff = BreakShutClock[1] - Last.At;
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT flick pace %s: %s; the barrels shut at %.3f; %.3f s in all, %.0f ms the worst gap"), *What, *Times, BreakShutClock[1] - T0, Total, Gap * 1000.0);
				if (bButton && !bEarly)
				{
					const int32 In = Wi == 2 || Loaded ? 1 : 2;
					const double Most = In == 1 ? 1.0 : 1.15;
					Check(Gap < 0.03 && FMath::Abs(ShutOff) < 0.03 && Total <= Most, FString::Printf(TEXT("the button reload's pace, %s: %.3f s in all (want %.2f at most), the slices %.0f ms apart at worst, the barrels shut %.0f ms from the shut slice (want under 30)"),
						*What, Total, Most, Gap * 1000.0, ShutOff * 1000.0));
				}
			};
			for (const bool bButton : { false, true })
			{
				Case(2, 0, bButton, false, TEXT("OSC"));
				Case(2, 0, bButton, true, TEXT("OSC"));
				for (const int32 Wi : { 3, 4 })
				{
					Case(Wi, 0, bButton, false, TEXT("OSSC"));
					Case(Wi, 1, bButton, false, TEXT("OSC"));
					Case(Wi, 0, bButton, true, TEXT("OSC"));
				}
			}
			for (const int32 Wi : { 2, 3, 4 })
			{
				TestBreakArm(1, Wi, 0, 0);
				FireCooldown[1] = 0.f;
				Spy.Reset();
				FireGun(1, false);
				const FString Got = AOutlawsLevelActor::ReloadCodes(Spy);
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT flick sounds %s, dry fire: heard %s"), *WeaponLabel(Wi), Spy.Num() ? *FString::Join(Spy, TEXT(", ")) : TEXT("nothing"));
				Check(Got == TEXT("X"), FString::Printf(TEXT("the %s dry fired empty: %s (want X, the click)"), *WeaponLabel(Wi), *Got));
				FireCooldown[1] = 0.f;
			}
			AOutlawsLevelActor::WavSpy = nullptr;
			AOutlawsLevelActor::RunSpy = nullptr;
			Spy.Reset();
			Runs.Reset();
		}

		// 7f. (BUG-FLICKUP) the wrist rolled 0, 30 and 60 degrees each way about the barrel, the muzzle dropped and raised about the
		// level axis across the barrel (a canted wrist's flick): each break gun opens and shuts at every roll
		{
			int32 Taken = 0, Tried = 0;
			for (const int32 Wi : { 2, 3, 4 })
			{
				for (const float Roll : { 0.f, 30.f, -30.f, 60.f, -60.f })
				{
					TestBreakArm(1, Wi, 0, 10);
					TestRollWrist(1, Roll);
					TestHandStill(1, 0.05f);
					Fwd(1);
					const bool bOpened = bBreakOpen[1];
					if (!bOpened) OpenBreak(1, TEXT("the test (its flick was not taken)"));
					TestHandStill(1, 0.3f);
					Back(1);
					const bool bShut = !bBreakOpen[1];
					Tried += 2;
					Taken += (bOpened ? 1 : 0) + (bShut ? 1 : 0);
					Check(bOpened && bShut, FString::Printf(TEXT("the %s, the wrist rolled %+.0f degrees: the flick open %s, the flick shut %s"), *WeaponLabel(Wi), Roll,
						bOpened ? TEXT("taken") : TEXT("NOT taken"), bShut ? TEXT("taken") : TEXT("NOT taken")));
				}
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT flick rolls: %d of %d flicks taken"), Taken, Tried);
			// (the user's Quest log) the shut flicks it turned away: a wrist turn of 24 degrees peaking at 420 deg/s (a run of about 17
			// over the hold rate, as the log counts it) and 26 at 400 (about 20) now shut an open gun; the same forward on a shut gun
			// (an aim down) still opens nothing
			for (const int32 Wi : { 2, 3, 4 })
			{
				TestBreakArm(1, Wi, 0, 10);
				OpenBreak(1, TEXT("the test"));
				TestHandStill(1, 0.3f);
				TestHandMotion(1, 420.f, 0.f, 0.f, 24.f * PI / (2.f * 420.f), 60.f);
				const bool bShut24 = !bBreakOpen[1];
				TestBreakArm(1, Wi, 0, 10);
				OpenBreak(1, TEXT("the test"));
				TestHandStill(1, 0.3f);
				TestHandMotion(1, 400.f, 0.f, 0.f, 26.f * PI / (2.f * 400.f), 60.f);
				const bool bShut17 = !bBreakOpen[1];
				TestBreakArm(1, Wi, 0, 10);
				TestHandMotion(1, -420.f, 0.f, 0.f, 24.f * PI / (2.f * 420.f), 60.f);
				const bool bStayed = !bBreakOpen[1];
				Check(bShut24 && bShut17 && bStayed, FString::Printf(TEXT("the %s: a slow shut flick (24 deg at 420 deg/s, a 17 deg run) %s, another (26 at 400, a 20 deg run) %s, the first forward on the shut gun %s"),
					*WeaponLabel(Wi), bShut24 ? TEXT("shuts") : TEXT("NOT taken"), bShut17 ? TEXT("shuts") : TEXT("NOT taken"), bStayed ? TEXT("opens nothing") : TEXT("OPENED it")));
			}
		}

		// 7g. (REQ-RELOADHIDEHAND) the button reload held two-handed: the front hand hidden from the open to the shut and back on the
		// forend after, the grip as before; the flick reloads (one-handed, and a refused two-handed flick) never hide it
		for (const int32 Wi : { 2, 3, 4 })
		{
			FString Seen;
			const bool bOk = TestHiddenHandReload(Wi, Seen);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT flick hidehand %s, button two-handed: %s"), *WeaponLabel(Wi), *Seen);
			Check(bOk, FString::Printf(TEXT("the %s's button reload held two-handed hides the front hand open to shut, back on the forend after: %s"), *WeaponLabel(Wi), *Seen));
			TestBreakArm(1, Wi, 0, 10);
			bool bHidFlick = false;
			Fwd(1);
			bHidFlick |= IsFrontHidden(0);
			TestHandStill(1, 0.3f);
			bHidFlick |= IsFrontHidden(0);
			Back(1);
			bHidFlick |= IsFrontHidden(0);
			const bool bFlicked = FlickCloses > 0 && !bBreakOpen[1];
			TestSetTwoHand(1, true);
			Fwd(1);
			bHidFlick |= IsFrontHidden(0);
			const bool bHeld = TestTwoHand(1);
			TestSetTwoHand(1, false);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT flick hidehand %s, flicks: the left hand %s through a flick open and shut and a refused two-handed flick"), *WeaponLabel(Wi),
				bHidFlick ? TEXT("HIDDEN") : TEXT("drawn"));
			Check(!bHidFlick && bFlicked && bHeld && !bBreakOpen[1], FString::Printf(TEXT("the %s's flick reloads never hide the other hand (%s)"), *WeaponLabel(Wi), bHidFlick ? TEXT("hidden") : TEXT("drawn")));
		}

		// 8. aiming never breaks it (one shell spent: a flick would open it)
		TestBreakArm(1, 3, 1, 10);
		const int32 Moves0 = FlickOpens + FlickCloses;
		TestHandMotion(1, 0.f, 400.f, 0.f, 0.6f, 60.f);            // a quick look-round sweep, side to side
		TestHandMotion(1, 0.f, -400.f, 0.f, 0.6f, 60.f);
		TestHandMotion(1, -450.f, 0.f, 0.f, 0.3f, 60.f);           // a fast aim down (86 degrees at up to 450 deg/s)
		TestHandMotion(1, 450.f, 0.f, 0.f, 0.3f, 60.f);            // and up
		TestHandMotion(1, -500.f, 450.f, 0.f, 0.25f, 80.f);        // a diagonal swing
		TestHandMotion(1, -900.f, 900.f, 200.f, 0.12f, 80.f);      // a fast snap to a target low and to the side (as fast as a flick, mostly sideways)
		TestHandMotion(1, 900.f, -900.f, -200.f, 0.12f, 80.f);
		for (int32 i = 0; i < 180; ++i)                             // two seconds of tracking jitter
		{
			TestHandMotion(1, FMath::FRandRange(-60.f, 60.f), FMath::FRandRange(-60.f, 60.f), FMath::FRandRange(-60.f, 60.f), 1.f / 90.f, 20.f);
		}
		Check(FlickOpens + FlickCloses == Moves0 && !bBreakOpen[1] && Mag[1] == 1, FString::Printf(TEXT("aim sweeps (side, up and down, diagonal, a fast sideways snap, jitter): no break (%d)"),
			FlickOpens + FlickCloses - Moves0));

		// 9. a melee swing: the wrist turns as fast as a flick but the hand chops across the barrel at 450 cm/s; and a throw-fast
		// hand (900 cm/s along the barrel)
		TestHandAcross = true;
		TestHandMotion(1, -900.f, 200.f, 100.f, 0.15f, 450.f);
		TestHandStill(1, 0.5f);
		TestHandMotion(1, 900.f, -200.f, -100.f, 0.15f, 450.f);
		TestHandAcross = false;
		TestHandStill(1, 0.5f);
		TestHandMotion(1, -900.f, 40.f, 30.f, 0.12f, 900.f);
		TestHandStill(1, 0.5f);
		Check(FlickOpens + FlickCloses == Moves0 && !bBreakOpen[1] && Mag[1] == 1, TEXT("a melee chop down and back across the barrel, and a throw-fast hand: no break"));

		// 10. the other guns: nothing
		for (const int32 Wi : { 0, 1 })
		{
			TestBreakArm(1, Wi, 0, 10);
			const int32 Before = ShellsOut, M0 = Mag[1], N0 = FlickOpens;
			Fwd(1);
			TestHandStill(1, 0.5f);
			const bool bOpenSeen = bBreakOpen[1];
			Back(1);
			Check(Equipped[1] == Wi && !bOpenSeen && FlickOpens == N0 && ShellsOut == Before && Mag[1] == M0 && !(BarrelMesh[1] && BarrelMesh[1]->IsVisible()),
				FString::Printf(TEXT("the %s: flicks do nothing (%d loaded before and after %d)"), *WeaponLabel(Wi), M0, Mag[1]));
		}
		bFaceReload = bFace;

		// the shells' flight, watched over the next frames: two more thrown out now
		TestBreakArm(1, 3, 0, 10);
		Fwd(1);
		At = ShotTime;
		ShotStep = 1;
		return;
	}
	if (ShotStep == 1)
	{
		if (ShotTime - At < 1.6f) return;
		int32 Resting = 0;
		const int32 Flying = TestShellsFlying(Resting);
		Check(Flying >= 2 && Resting >= 1, FString::Printf(TEXT("1.6 s on the shells are still there (%d) and lie on the floor (%d)"), Flying, Resting));
		ShotStep = 2;
		return;
	}
	if (ShotStep == 2)
	{
		if (ShotTime - At < 2.4f) return;
		int32 Resting = 0;
		const int32 Flying = TestShellsFlying(Resting);
		Check(Flying == 0, FString::Printf(TEXT("2.4 s on they are gone (%d left)"), Flying));
		Finish();
	}
}

OL_SHOT_MATCH(OLFlickTest) { return OLFlags::Param(TEXT("OLFlickTest")); }
OL_SHOT_TEST(OLFlickTest, 120)
{
	FlickTestTick(Dt);               // (REQ-FLICKRELOAD: OutlawsPlayerFlickTest.cpp)
}
#endif // WITH_OUTLAWS_TESTS
