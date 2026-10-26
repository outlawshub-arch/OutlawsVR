// -OLShots -OLFloorShot: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLFloorShot)
{
	FString FloorSector;
	return OLFlags::Value(TEXT("OLFloorShot"), FloorSector);
}

OL_SHOT_TEST(OLFloorShot, 660)
{
	FString FloorSector;
	OLFlags::Value(TEXT("OLFloorShot"), FloorSector);
	// a look down at a sector's floor from its middle (ol_floor.png): the texture's turn and mirror
	if (ShotStep == 0 && ShotTime > 3.f && L)
	{
		FVector Spot;
		if (L->DebugSectorSpot(FloorSector, Spot)) TeleportTo(Spot + FVector(0.0, 0.0, 92.0), GetActorRotation(), false, true);
		if (Controller) Controller->SetControlRotation(FRotator(-70.0, GetActorRotation().Yaw, 0.0));
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 4.5f) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_floor.png"), false, false); ++ShotStep; }
	else if (ShotStep == 2 && ShotTime > 5.5f) { FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; }
}

#endif // WITH_OUTLAWS_TESTS
