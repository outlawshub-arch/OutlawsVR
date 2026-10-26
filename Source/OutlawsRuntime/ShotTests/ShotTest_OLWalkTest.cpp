// -OLShots -OLWalkTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLWalkTest)
{
	int32 WalkSector = -1;
	return OLFlags::Value(TEXT("OLWalkTest"), WalkSector);
}

OL_SHOT_TEST(OLWalkTest, 520)
{
	int32 WalkSector = -1;
	OLFlags::Value(TEXT("OLWalkTest"), WalkSector);
	// up a slope from its foot: height gained each half second, and what stops it
	if (ShotStep == 0 && ShotTime > 3.f)
	{
		FVector Feet;
		float Yaw = 0.f;
		if (L && L->SlopeFoot(WalkSector, Feet, Yaw))
		{
			TeleportTo(Feet + FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0), FRotator(0.0, Yaw, 0.0), false, true);
			if (AController* C = GetController()) C->SetControlRotation(FRotator(0.0, Yaw, 0.0));
			WalkTestLeft = 5.f;
		}
		++ShotStep;
	}
	else if (ShotStep >= 1 && ShotStep < 12 && ShotTime > 3.f + 0.5f * ShotStep)
	{
		const FHitResult& Block = GetCharacterMovement()->CurrentFloor.HitResult;
		UE_LOG(LogOutlaws, Display, TEXT("OLRT walk test t%.1f: feet z %.0f, moving %.0f cm/s, mode %d, floor %s walkable %d, %s"), 0.5f * ShotStep,
			GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), GetVelocity().Size2D(), int32(GetCharacterMovement()->MovementMode.GetValue()),
			Block.GetComponent() ? *Block.GetComponent()->GetName() : TEXT("-"), GetCharacterMovement()->CurrentFloor.bWalkableFloor ? 1 : 0,
			L ? *L->DescribeSectorsAt(GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() - 5.0)) : TEXT(""));
		if (ShotStep == 5) FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_walk.png"), false, false);
		++ShotStep;
	}
	else if (ShotStep >= 12)
	{
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
