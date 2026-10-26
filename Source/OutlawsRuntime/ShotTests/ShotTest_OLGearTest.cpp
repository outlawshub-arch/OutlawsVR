// -OLShots -OLGearTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLGearTest) { return OLFlags::Param(TEXT("OLGearTest")); }

OL_SHOT_TEST(OLGearTest, 1100)
{
	// the held gear on the desktop (user 2026-10-01): the plate in the left hand and the map in the right (ol_gear_a),
	// six shots into the plate (ol_gear_b), the lantern lit in the right hand (ol_gear_c)
	if (ShotStep == 0) GEngine->Exec(GetWorld(), TEXT("r.MotionBlurQuality 0"));
	static float GearYaw = -1000.f;
	if (GearYaw < -999.f) GearYaw = static_cast<float>(GetActorRotation().Yaw);
	if (Controller) Controller->SetControlRotation(FRotator(-12.0, GearYaw, 0.0));
	const FVector Eye = Camera->GetComponentLocation();
	const FVector F = Camera->GetForwardVector(), R = Camera->GetRightVector();
	const FVector Fl = FVector(F.X, F.Y, 0.0).GetSafeNormal();
	for (int32 H = 0; H < 2; ++H)
	{
		UOutlawsBoxHand* Box = H == 0 ? LeftHand.Get() : RightHand.Get();
		if (!Box) continue;
		const double Side = H == 0 ? -1.0 : 1.0;
		const FVector Pos = Eye + Fl * 48.0 + R * Side * 16.0 - FVector::UpVector * 18.0;
		FrameOf(H)->SetWorldLocationAndRotation(Pos, FRotationMatrix::MakeFromXZ(Fl, FVector::UpVector).ToQuat());
		Box->SetVisibility(true, true);
		Box->SetCurl(1.f, 1.f);
	}
	auto Shot = [](const TCHAR* Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(Name) + TEXT(".png"), false, false); };
	if (ShotStep == 0 && ShotTime > 3.f && L)
	{
		L->AddCount(TEXT("ibplate"), 150);
		L->AddCount(TEXT("ioil"), 100);
		// (-OLGearTools: the shovel and the crowbar, then the pen, for a look at the modelled tools in hand)
		const bool bTools = OLFlags::Param(TEXT("OLGearTools"));
		EquipTool(0, bTools ? TEXT("ishovel") : TEXT("ibplate"));
		EquipTool(1, bTools ? TEXT("icrowbar") : TEXT("imap"));
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 6.f) { Shot(TEXT("ol_gear_a")); ++ShotStep; }
	else if (ShotStep == 2 && ShotTime > 7.f && L)
	{
		// six shots from ahead at the plate's face, about it
		const FVector C = GearMesh[0] ? GearMesh[0]->GetComponentLocation() : Eye + Fl * 56.0;
		for (int32 k = 0; k < 6; ++k)
		{
			const FVector Aim = C + R * FMath::FRandRange(-12.f, 12.f) + FVector::UpVector * FMath::FRandRange(-10.f, 10.f);
			FVector To = Aim - Fl * 30.0;
			float Dmg = 1.5f;
			const bool bStop = PlateStops(Aim + Fl * 600.0, To, Dmg);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT gear shot %d: %s, armour %d"), k, bStop ? TEXT("stopped") : TEXT("passed"), L->GetArmour());
		}
		// a hit from the right (ol_gear_b, with -OLHurtVeil: the headset's red on the right of the view)
		HurtDir = FVector2D(1.0, 0.0);
		HurtDirAmount = 3.f;
		++ShotStep;
	}
	else if (ShotStep == 3 && ShotTime > 8.5f) { Shot(TEXT("ol_gear_b")); ++ShotStep; }
	else if (ShotStep == 4 && ShotTime > 9.5f && L)
	{
		EquipTool(1, OLFlags::Param(TEXT("OLGearTools")) ? TEXT("iink") : TEXT("ilantern"));
		if (!L->bLampOn) L->ToggleLamp();
		L->BadgeLeft = 25.f;                  // (the badge's gold round the view, ol_gear_c)
		++ShotStep;
	}
	else if (ShotStep == 5 && ShotTime > 12.f) { Shot(TEXT("ol_gear_c")); ++ShotStep; }
	else if (ShotStep == 6 && ShotTime > 13.f) { FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; }
	for (int32 H = 0; H < 2; ++H) if (IsGear(HeldTool[H])) UpdateGear(H, FMath::Max(0.001f, GetWorld()->GetDeltaSeconds()));   // (the hand update does not run on the desktop)
	UpdateBadgeVignette(FMath::Max(0.001f, GetWorld()->GetDeltaSeconds()));
}

#endif // WITH_OUTLAWS_TESTS
