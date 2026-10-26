// -OLShots -OLHandTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLHandTest) { return OLFlags::Param(TEXT("OLHandTest")); }

OL_SHOT_TEST(OLHandTest, 1130)
{
	// the hands held up before the eyes on the desktop, backs toward the eyes, fingers up and away: open, then
	// closed (ol_hands_open, ol_hands_fist)
	if (ShotStep == 0) GEngine->Exec(GetWorld(), TEXT("r.MotionBlurQuality 0"));
	static float HoldYaw = -1000.f;
	if (HoldYaw < -999.f) HoldYaw = static_cast<float>(GetActorRotation().Yaw);
	if (Controller) Controller->SetControlRotation(FRotator(ShotStep >= 7 ? -14.0 : -38.0, HoldYaw, 0.0));     // a still camera, looking down at the hands
	const FVector Eye = Camera->GetComponentLocation();
	const FVector F = Camera->GetForwardVector(), R = Camera->GetRightVector(), U = Camera->GetUpVector();
	const bool bFist = ShotStep >= 3;
	for (int32 H = 0; H < 2; ++H)
	{
		UOutlawsBoxHand* Box = H == 0 ? LeftHand.Get() : RightHand.Get();
		if (!Box) continue;
		const double Side = H == 0 ? -1.0 : 1.0;
		// as held in VR: the controllers forward at the waist, seen from the eyes above and behind
		const FVector Fl = FVector(F.X, F.Y, 0.0).GetSafeNormal();
		const FVector Pos = Eye + Fl * 34.0 + R * Side * 13.0 - FVector::UpVector * 30.0;
		const FQuat Rot = FRotationMatrix::MakeFromXZ((Fl - R * Side * 0.25).GetSafeNormal(), FVector::UpVector).ToQuat();
		Box->SetVisibility(true, true);
		FrameOf(H)->SetWorldLocationAndRotation(Pos, Rot);        // the hand frame: the hand and any gun follow
		if (ShotStep < 5) Box->SetCurl(bFist ? 1.f : 0.f, bFist ? 1.f : 0.f);
	}
	auto Shot = [](const TCHAR* Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(Name) + TEXT(".png"), false, false); };
	if (ShotStep == 0 && ShotTime > 3.f) ++ShotStep;
	else if (ShotStep == 1 && ShotTime > 7.f) { Shot(TEXT("ol_hands_open")); ++ShotStep; }
	else if (ShotStep == 2 && ShotTime > 7.f) ++ShotStep;
	else if (ShotStep == 3 && ShotTime > 8.5f) { Shot(TEXT("ol_hands_fist")); ++ShotStep; }
	else if (ShotStep == 4 && ShotTime > 9.5f)
	{
		// a revolver in each hand: seated in the fist, index on the trigger (ol_hands_gun)
		if (L) { L->GiveAllWeapons(); L->AddCount(TEXT("ipistol"), 1); }
		Equip(0, 0);
		Equip(1, 0);
		++ShotStep;
	}
	else if (ShotStep == 5 && ShotTime > 12.f) { Shot(TEXT("ol_hands_gun")); ++ShotStep; }
	else if (ShotStep == 6 && ShotTime > 13.f)
	{
		// a lit stick of dynamite in the right hand: the wick at two thirds, then near its end (ol_fuse_a, ol_fuse_b)
		Equip(1, 6);
		Mag[1] = 1;
		bFuseLit[1] = true;
		Fuse[1] = 3.0f;
		++ShotStep;
	}
	else if (ShotStep == 7 && ShotTime > 15.f) { Shot(TEXT("ol_fuse_a")); Fuse[1] = 0.9f; ++ShotStep; }
	else if (ShotStep == 8 && ShotTime > 16.5f) { Shot(TEXT("ol_fuse_b")); Fuse[1] = 2.5f; ++ShotStep; }
	else if (ShotStep == 9 && ShotTime > 17.5f) { ThrowHeld(1); ++ShotStep; }
	else if (ShotStep == 10 && ShotTime > 17.62f) { Shot(TEXT("ol_fuse_thrown")); ++ShotStep; }
	else if (ShotStep == 11 && ShotTime > 18.5f) { FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; }
	if (ShotStep >= 7 && ShotStep <= 9) UpdateFuseLook(1);       // (the hand update does not run on the desktop)
}

#endif // WITH_OUTLAWS_TESTS
