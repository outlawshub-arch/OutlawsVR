// -OLShots -OLRoofTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLRoofTest)
{
	int32 RoofN = -1;
	return OLFlags::Value(TEXT("OLRoofTest"), RoofN);
}

OL_SHOT_TEST(OLRoofTest, 990)
{
	int32 RoofN = -1;
	OLFlags::Value(TEXT("OLRoofTest"), RoofN);
	// (BUG-SLOPES) climbing onto a sloped roof: stood at its wall, the climb started at the lip, then where the body is
	// for two seconds (its feet against the roof under it, the movement mode, the speed)
	static FVector RoofLip, RoofIn;
	static int32 RoofSector = -1;
	if (ShotStep == 0 && ShotTime > 3.f && L)
	{
		FVector Ground;
		if (!L->DebugRoofLip(RoofN, Ground, RoofLip, RoofIn, RoofSector))
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT roof test: no sloped roof %d here"), RoofN);
			FGenericPlatformMisc::RequestExit(false);
			ShotTime = -1.f;
			return;
		}
		TeleportTo(Ground + FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0), RoofIn.Rotation(), false, true);
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 4.5f)
	{
		GrabPoint = RoofLip - RoofIn * 3.0 - FVector(0.0, 0.0, 4.0);
		LedgeTopZ = float(RoofLip.Z);
		LedgeIn = RoofIn;
		const bool bOk = StartMantle();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT roof test: sector %d, lip z %.0f, the body at z %.0f: climb %s"), RoofSector, RoofLip.Z,
			GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), bOk ? TEXT("started") : TEXT("refused (no room)"));
		++ShotStep;
	}
	else if (ShotStep >= 2 && ShotStep < 10 && ShotTime > 4.5f + 0.25f * (ShotStep - 1))
	{
		const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
		FHitResult Fl;
		FCollisionQueryParams Qf(SCENE_QUERY_STAT(OutlawsRoofProbe), false, this);
		const bool bFl = GetWorld()->LineTraceSingleByChannel(Fl, Feet + FVector(0.0, 0.0, 50.0), Feet - FVector(0.0, 0.0, 300.0), ECC_Visibility, Qf);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT roof test t+%.2f: feet z %.0f (lip %.0f), surface under %.0f (normal z %.2f), mode %d, speed %.0f, floor walkable %d, sector %d"),
			0.25f * (ShotStep - 1), Feet.Z, RoofLip.Z, bFl ? Fl.ImpactPoint.Z : -1.0, bFl ? Fl.ImpactNormal.Z : 0.0, int32(GetCharacterMovement()->MovementMode),
			GetVelocity().Size(), GetCharacterMovement()->CurrentFloor.bWalkableFloor ? 1 : 0, L ? L->PlayerSectorIndex() : -1);
		++ShotStep;
	}
	else if (ShotStep == 10) { FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; }
}

#endif // WITH_OUTLAWS_TESTS
