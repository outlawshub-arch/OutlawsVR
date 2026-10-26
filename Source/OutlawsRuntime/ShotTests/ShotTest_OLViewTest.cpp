// -OLShots -OLViewTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLViewTest)
{
	FString ViewArg;
	return OLFlags::Value(TEXT("OLViewTest"), ViewArg, false);
}

OL_SHOT_TEST(OLViewTest, 1070)
{
	FString ViewArg;
	OLFlags::Value(TEXT("OLViewTest"), ViewArg, false);
	// a look in a direction from where the level put the player: ol_view.png
	if (ShotStep == 0 && ShotTime > 3.f)
	{
		TArray<FString> Parts;
		ViewArg.ParseIntoArray(Parts, TEXT(","));
		const float Pitch = Parts.Num() > 0 ? FCString::Atof(*Parts[0]) : 0.f;
		const float Yaw = Parts.Num() > 1 ? FCString::Atof(*Parts[1]) : float(GetActorRotation().Yaw);
		// a third number: the sector to stand in first (its lowest corner, as the walk test)
		if (Parts.Num() > 2 && L)
		{
			FVector Feet;
			float SlopeYaw = 0.f;
			if (L->SlopeFoot(FCString::Atoi(*Parts[2]), Feet, SlopeYaw)) TeleportTo(Feet + FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0), FRotator(0.0, Yaw, 0.0), false, true);
		}
		float LookYaw = Yaw, LookPitch = Pitch;
		if (OLFlags::Param(TEXT("OLSettleLook")) && L && !L->SettleLookAt.IsZero())
		{
			// the item the settling moved most, from 2 m off where nothing is in the way, looking down at it
			const FVector It = L->SettleLookAt;
			for (int32 k = 0; k < 8; ++k)
			{
				const FVector Dir = FRotator(0.0, 45.0 * k, 0.0).Vector();
				const FVector Eye = It + Dir * 200.0 + FVector(0.0, 0.0, 120.0);
				FHitResult Hb;
				if (GetWorld()->LineTraceSingleByChannel(Hb, It + FVector(0.0, 0.0, 60.0), Eye, ECC_Visibility)) continue;
				TeleportTo(Eye - FVector(0.0, 0.0, 60.0), FRotator(0.0, 180.0 + 45.0 * k, 0.0), false, true);
				LookYaw = 180.f + 45.f * k;
				LookPitch = -28.f;
				break;
			}
		}
		if (Controller) Controller->SetControlRotation(FRotator(LookPitch, LookYaw, 0.0));
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 4.5f) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_view.png"), false, false); ++ShotStep; }
	else if (ShotStep == 2 && ShotTime > 5.5f) { FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; }
}

#endif // WITH_OUTLAWS_TESTS
