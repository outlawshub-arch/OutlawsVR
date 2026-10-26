// -OLShots -OLTrainDoorTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLTrainDoorTest) { return OLFlags::Param(TEXT("OLTrainDoorTest")); }

OL_SHOT_TEST(OLTrainDoorTest, 710)
{
	// -OLLevel=train -OLNoFrontEnd -OLTrainDoorTest (BUG-TRAINDOOR2): stood where the user's snapshot was (X 234.98
	// Y 2.96 Z 863.96, sector 622, heading 98.9), a palm pushed into the door ahead opens it (LAST_DOOR: the level ends)
	if (ShotStep == 0 && ShotTime > 3.f)
	{
		TeleportTo(FVector(25918.873, 7049.423, 88.776), FRotator(0.0, -45.0, 0.0), false, true);
		if (Controller) Controller->SetControlRotation(FRotator(0.0, -8.93, 0.0));
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 4.f)
	{
		const FVector Eye = Camera->GetComponentLocation();
		const FVector Fwd = FVector(Camera->GetForwardVector().X, Camera->GetForwardVector().Y, 0.0).GetSafeNormal();
		const FVector At = Eye - FVector(0.0, 0.0, 40.0);
		FHitResult Wall;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsTrainDoorTest), true, this);
		const bool bWall = GetWorld()->LineTraceSingleByChannel(Wall, At, At + Fwd * 300.0, ECC_Visibility, Q);
		const FVector Hand = bWall ? Wall.ImpactPoint - Fwd * 5.0 : At + Fwd * 60.0;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT traindoor: wall ahead %d at %.1f cm; %s"), bWall ? 1 : 0, bWall ? Wall.Distance : -1.0,
			L ? *L->DebugDoorProbe(Eye, Hand) : TEXT("no level"));
		bool bClosed = false;
		const int32 Door = L ? L->DoorAtHand(Eye, Hand, bClosed) : -1;
		const bool bTaken = Door >= 0 && bClosed && L->PushDoorSector(Door);
		// the level's end starts at once (its fade is 0.375 s, then the cutscene takes over)
		const bool bEnding = L && L->DebugLevelEnding();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT traindoor pushed: door %d taken %d, level ending %d"), Door, bTaken ? 1 : 0, bEnding ? 1 : 0);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT traindoor RESULT %s"), bTaken && bEnding ? TEXT("PASS") : TEXT("FAIL"));
		++ShotStep;
	}
	else if (ShotStep == 2 && ShotTime > 6.f)
	{
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
