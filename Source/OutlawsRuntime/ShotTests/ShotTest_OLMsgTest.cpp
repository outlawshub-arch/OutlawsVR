// -OLShots -OLMsgTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLMsgTest) { return OLFlags::Param(TEXT("OLMsgTest")); }

OL_SHOT_TEST(OLMsgTest, 640)
{
	// BUG-MSGCLIP: face the cliff's stone key wall from 30 cm with a notice up; ol_msgclip.png shows the line in
	// front of the wall
	if (ShotStep == 0 && ShotTime > 3.f && L)
	{
		FVector Spot, Wall;
		if (L->DebugSectorSpot(TEXT("KEYCODE"), Spot) && L->DebugWallSpot(TEXT("KEYCODE"), TEXT("8E66"), Wall))
		{
			const FVector In = FVector(Spot.X - Wall.X, Spot.Y - Wall.Y, 0.0).GetSafeNormal();
			TeleportTo(FVector(Wall.X, Wall.Y, Spot.Z + 92.0) + In * 30.0, (-In).Rotation(), false, true);
			if (Controller) Controller->SetControlRotation((-In).Rotation());
		}
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 4.f)
	{
		ShowMessage(TEXT("You need a key"), 10.f);
		++ShotStep;
	}
	else if (ShotStep == 2 && ShotTime > 6.f)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT msgclip: notice at %.0f cm, eye %s"), HudDist, Camera ? *Camera->GetComponentLocation().ToCompactString() : TEXT("-"));
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_msgclip.png"), false, false);
		++ShotStep;
	}
	else if (ShotStep == 3 && ShotTime > 8.f)
	{
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
