// -OLShots -OLPoseShot: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLPoseShot)
{
	FString PoseShotArg;
	return OLFlags::Value(TEXT("OLPoseShot"), PoseShotArg, false);
}

OL_SHOT_TEST(OLPoseShot, 1050)
{
	FString PoseShotArg;
	OLFlags::Value(TEXT("OLPoseShot"), PoseShotArg, false);
	// (BUG-WALLSFLIPPED) -OLPoseShot=<x>,<y>,<z>,<pitch>,<yaw>,<name>: the body where a bug snapshot's info.txt put it (its
	// "UE location"), looking as its "look" did: Saved/OLShots/ol_pose_<name>.png
	TArray<FString> Parts;
	PoseShotArg.ParseIntoArray(Parts, TEXT(","));
	const FString ShotName = Parts.Num() > 5 ? Parts[5] : TEXT("shot");
	// (BUG-ENEMYWALLCLIP) ...,<ex>,<ey>,<ez>: the nearest bad guy held there (an OutlawsEnemy's place from the snapshot)
	// from the pose on, every frame, until the shot
	if (Parts.Num() >= 9 && ShotStep >= 1 && ShotStep <= 2)
	{
		const FVector EnemyAt(FCString::Atod(*Parts[6]), FCString::Atod(*Parts[7]), FCString::Atod(*Parts[8]));
		static TWeakObjectPtr<AOutlawsEnemy> Held;
		if (!Held.IsValid())
		{
			double Best = 1e30;
			for (TActorIterator<AOutlawsEnemy> It(GetWorld()); It; ++It)
			{
				const double D = FVector::DistSquared(It->GetActorLocation(), EnemyAt);
				if (D < Best) { Best = D; Held = *It; }
			}
			if (Held.IsValid()) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT pose shot %s: holding %s at %s"), *ShotName, *Held->GetName(), *EnemyAt.ToCompactString());
		}
		if (Held.IsValid()) Held->SetActorLocation(EnemyAt, false, nullptr, ETeleportType::TeleportPhysics);
	}
	if (ShotStep == 0 && ShotTime > 4.f && Parts.Num() >= 5)
	{
		const FVector At(FCString::Atod(*Parts[0]), FCString::Atod(*Parts[1]), FCString::Atod(*Parts[2]));
		const FRotator Look(FCString::Atod(*Parts[3]), FCString::Atod(*Parts[4]), 0.0);
		TeleportTo(At, FRotator(0.0, Look.Yaw, 0.0), false, true);
		GetCharacterMovement()->DisableMovement();
		if (Controller) Controller->SetControlRotation(Look);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT pose shot %s: at %s, look %s"), *ShotName, *At.ToCompactString(), *Look.ToCompactString());
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 7.f) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / (TEXT("ol_pose_") + ShotName + TEXT(".png")), false, false); ++ShotStep; }
	else if (ShotStep == 2 && ShotTime > 8.5f) { FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; }
}

#endif // WITH_OUTLAWS_TESTS
