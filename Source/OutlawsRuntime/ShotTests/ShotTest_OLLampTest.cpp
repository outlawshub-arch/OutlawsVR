// -OLShots -OLLampTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLLampTest) { return OLFlags::Param(TEXT("OLLampTest")); }

OL_SHOT_TEST(OLLampTest, 400)
{
	// the lamps' light (backlog 8): stood 3 m from the nearest lit lamp, its fire ahead, a shot with the lamps' light and
	// one without (ol.LampLights 0), ol_lamp_on.png / ol_lamp_off.png
	static bool bPlaced = false;
	AOutlawsLevelActor* LampL = Level.Get();
	auto Shot = [](const TCHAR* Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(Name) + TEXT(".png"), false, false); };
	FVector Fire;
	if (!bPlaced && ShotTime > 2.f && L)
	{
		if (!LampL->NearestLampFire(GetActorLocation(), Fire))
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lamp test: no lit lamp in this level"));
			FGenericPlatformMisc::RequestExit(false);
			ShotTime = -1.f;
			return;
		}
		const FVector Away = FVector(GetActorLocation().X - Fire.X, GetActorLocation().Y - Fire.Y, 0.0).GetSafeNormal();
		const FVector Stand = Fire + (Away.IsNearlyZero() ? FVector::ForwardVector : Away) * 300.0;
		SetActorLocation(FVector(Stand.X, Stand.Y, Fire.Z - 60.0), false, nullptr, ETeleportType::TeleportPhysics);
		if (Controller) Controller->SetControlRotation((Fire - (GetActorLocation() + FVector(0.0, 0.0, 60.0))).Rotation());
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lamp test: fire at %s"), *Fire.ToString());
		bPlaced = true;
		ShotStep = 0;
		ShotTime = 3.f;
	}
	if (bPlaced && ShotStep == 0 && ShotTime > 5.f) { Shot(TEXT("ol_lamp_on")); IConsoleManager::Get().FindConsoleVariable(TEXT("ol.LampLights"))->Set(0); ++ShotStep; }
	else if (bPlaced && ShotStep == 1 && ShotTime > 6.5f) { Shot(TEXT("ol_lamp_off")); ++ShotStep; }
	else if (bPlaced && ShotStep == 2 && ShotTime > 7.5f) { UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lamp test done")); FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; }
}

#endif // WITH_OUTLAWS_TESTS
