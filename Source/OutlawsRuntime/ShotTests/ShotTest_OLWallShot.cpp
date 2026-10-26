// -OLShots -OLWallShot: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLWallShot)
{
	FString WallShotArg;
	return OLFlags::Value(TEXT("OLWallShot"), WallShotArg, false);
}

OL_SHOT_TEST(OLWallShot, 1060)
{
	FString WallShotArg;
	OLFlags::Value(TEXT("OLWallShot"), WallShotArg, false);
	// (BUG-ADJOINDEFAULT) -OLWallShot=<sector>,<wall>,<units back>,<name>: stand that far in front of one wall, looking
	// at it (its sign when it has one): Saved/OLShots/ol_wall_<name>.png
	TArray<FString> Parts;
	WallShotArg.ParseIntoArray(Parts, TEXT(","));
	const FString ShotName = Parts.Num() > 3 ? Parts[3] : TEXT("shot");
	if (ShotStep == 0 && ShotTime > 4.f && L && Parts.Num() >= 3)
	{
		FVector Feet, At;
		if (L->DebugWallView(FCString::Atoi(*Parts[0]), FCString::Atoi(*Parts[1]), FCString::Atod(*Parts[2]), Feet, At))
		{
			const double Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			TeleportTo(Feet + FVector(0.0, 0.0, Half + 2.0), (At - Feet).GetSafeNormal2D().Rotation(), false, true);
			GetCharacterMovement()->DisableMovement();
		}
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 6.f && L && Parts.Num() >= 3)
	{
		// aim from the camera as it is now (its height is the body's), then the picture
		FVector Feet, At;
		if (L->DebugWallView(FCString::Atoi(*Parts[0]), FCString::Atoi(*Parts[1]), FCString::Atod(*Parts[2]), Feet, At) && Controller)
		{
			APlayerController* PC = Cast<APlayerController>(Controller);
			const FVector Eye = PC && PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : GetActorLocation();
			Controller->SetControlRotation((At - Eye).Rotation());
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wall shot %s: eye %s, at %s"), *ShotName, *Eye.ToCompactString(), *At.ToCompactString());
		}
		++ShotStep;
	}
	else if (ShotStep == 2 && ShotTime > 8.f) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / (TEXT("ol_wall_") + ShotName + TEXT(".png")), false, false); ++ShotStep; }
	else if (ShotStep == 3 && ShotTime > 9.5f) { FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; }
}

#endif // WITH_OUTLAWS_TESTS
