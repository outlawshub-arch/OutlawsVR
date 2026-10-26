// -OLShots -OLGlowTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLGlowTest) { return OLFlags::Param(TEXT("OLGlowTest")); }

OL_SHOT_TEST(OLGlowTest, 410)
{
	// the ledge and door glow (user 2026-10-01): for each of 12 headings, up to the nearest wall within 6 m, the right
	// hand against it at heights from the waist to over the head; the first strip shot (ol_glow.png)
	static float GlowYaw = -1000.f;
	static bool bShot = false, bThere = false;
	static FVector Dir = FVector::ForwardVector;
	if (GlowYaw < -999.f) GlowYaw = static_cast<float>(GetActorRotation().Yaw);
	if (ShotTime > 3.f && !bShot && ShotStep < 12)
	{
		if (!bThere)
		{
			const float Yaw = GlowYaw + 30.f * ShotStep;
			Dir = FRotator(0.0, Yaw, 0.0).Vector();
			const FVector Eye = Camera->GetComponentLocation();
			FHitResult Hw;
			FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsGlowTest), true, this);
			if (GetWorld()->LineTraceSingleByChannel(Hw, Eye, Eye + Dir * 600.0, ECC_Visibility, Q))
			{
				TeleportTo(GetActorLocation() + Dir * (Hw.Distance - 55.0), FRotator(0.0, Yaw, 0.0), false, true);
				if (Controller) Controller->SetControlRotation(FRotator(-15.0, Yaw, 0.0));
				bThere = true;
			}
			else ++ShotStep;
		}
		else
		{
			const FVector Eye = Camera->GetComponentLocation();
			for (double Dz = -80.0; Dz <= 30.0 && SurfaceGlowKind[1] == 0; Dz += 10.0)
			{
				GripOf(1)->SetWorldLocation(Eye + Dir * 48.0 + FVector(0.0, 0.0, Dz));
				UpdateSurfaceGlow(1, true);
			}
			if (SurfaceGlowKind[1] != 0)
			{
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT glow: kind %d at heading step %d, hand %s"), SurfaceGlowKind[1], ShotStep, *GripOf(1)->GetComponentLocation().ToString());
				// back off and look down at it (the strip stays where it was found: the hand's update is the test's)
				const FVector HandAt = GripOf(1)->GetComponentLocation();
				TeleportTo(GetActorLocation() - Dir * 120.0, GetActorRotation(), false, true);
				const FVector LookFrom = Camera->GetComponentLocation() - Dir * 120.0;
				if (Controller) Controller->SetControlRotation((HandAt - LookFrom).Rotation());
				bShot = true;
				ShotTime = 19.f;
			}
			else
			{
				bThere = false;
				++ShotStep;
			}
		}
	}
	static bool bTaken = false;
	if (bShot && !bTaken && ShotTime > 20.f) { bTaken = true; FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_glow.png"), false, false); }
	if ((bShot && ShotTime > 21.5f) || ShotStep >= 12) { UE_LOG(LogOutlaws, Display, TEXT("OLSHOT glow test done: shot %d"), bShot ? 1 : 0); FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; }
}

#endif // WITH_OUTLAWS_TESTS
