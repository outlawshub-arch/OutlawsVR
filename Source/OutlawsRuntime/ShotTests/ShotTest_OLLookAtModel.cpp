// -OLShots -OLLookAtModel: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLLookAtModel)
{
	FString LookModel;
	return OLFlags::Value(TEXT("OLLookAtModel"), LookModel);
}

OL_SHOT_TEST(OLLookAtModel, 1040)
{
	FString LookModel;
	OLFlags::Value(TEXT("OLLookAtModel"), LookModel);
	// (BUG-ALPHA) 6.5 m from the first of a 3D item model (TREE01, SHRUB01 ...), looking at it: ol_model.png
	if (ShotStep == 0 && ShotTime > 4.f && L)
	{
		const FVector* At = L->DebugModelAt.Find(LookModel.ToUpper());
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT look at %s: %s"), *LookModel, At ? *At->ToString() : TEXT("none in this level"));
		if (At)
		{
			const FVector Stand = *At + FVector(650.0, 0.0, 0.0);
			TeleportTo(Stand + FVector(0.0, 0.0, 120.0), FRotator(0.0, 180.0, 0.0), false, true);
			if (Controller) Controller->SetControlRotation((*At + FVector(0.0, 0.0, 380.0) - (Stand + FVector(0.0, 0.0, 160.0))).Rotation());
		}
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 7.f) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_model.png"), false, false); ++ShotStep; }
	else if (ShotStep == 2 && ShotTime > 8.f) { FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; }
}

#endif // WITH_OUTLAWS_TESTS
