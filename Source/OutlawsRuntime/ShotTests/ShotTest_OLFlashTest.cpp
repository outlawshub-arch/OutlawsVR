// -OLShots -OLFlashTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLFlashTest)
{
	int32 FlashSector = -2;
	return OLFlags::Value(TEXT("OLFlashTest"), FlashSector);
}

OL_SHOT_TEST(OLFlashTest, 480)
{
	int32 FlashSector = -2;
	OLFlags::Value(TEXT("OLFlashTest"), FlashSector);
	// in the darkest room (or the sector given): dark, a muzzle flash, then the lantern
	if (ShotStep == 0 && ShotTime > 3.f)
	{
		const int32 Sec = FlashSector >= 0 ? FlashSector : (L ? L->DarkestSector() : -1);
		FVector Feet;
		float Yaw = 0.f;
		if (L && L->SlopeFoot(Sec, Feet, Yaw))
		{
			TeleportTo(Feet + FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0), FRotator(0.0, Yaw, 0.0), false, true);
			if (AController* C = GetController()) C->SetControlRotation(FRotator(0.0, Yaw, 0.0));
		}
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 4.f) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_dark.png"), false, false); ++ShotStep; }
	else if (ShotStep == 2 || ShotStep == 3)
	{
		if (ShotTime > 4.5f && L) L->MuzzleFlash(Camera->GetComponentLocation() + Camera->GetForwardVector() * 40.0);
		if (ShotStep == 2 && ShotTime > 4.6f) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_flash.png"), false, false); ++ShotStep; }
		else if (ShotStep == 3 && ShotTime > 4.8f) ++ShotStep;
	}
	else if (ShotStep == 4 && ShotTime > 5.5f)
	{
		if (L) { L->AddCount(TEXT("ilantern"), 1); L->AddCount(TEXT("ioil"), 150); L->ToggleLamp(); }
		++ShotStep;
	}
	else if (ShotStep == 5 && ShotTime > 6.5f) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_lamp.png"), false, false); ++ShotStep; }
	else if (ShotStep == 6 && ShotTime > 7.5f) { FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; }
}

#endif // WITH_OUTLAWS_TESTS
