// -OLShots -OLWallStuckTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLWallStuckTest) { return OLFlags::Param(TEXT("OLWallStuckTest")); }

OL_SHOT_TEST(OLWallStuckTest, 690)
{
	// -OLLevel=hideout -OLNoFrontEnd -OLShots -OLWallStuckTest (BUG-WALLSTUCK): put where the user's two snapshots were (in a
	// wall, freed by the unstuck): does the body overlap the world there, what walls are near, and where it ends up
	// the snapshots' own logs: a hand push on door sector 33 (spot 1, 395 <-> 33) and 31 (spot 2), then "the feet between
	// sectors" at about X 12650..12785 Y 11490 (feet Z 392) by the 31 door; spot 1 here is that, spot 2 door 33's middle
	static FVector Spots[2] = { FVector(12700.0, 11493.0, 479.15), FVector::ZeroVector };
	if (ShotStep == 0 && ShotTime > 1.5f && ShotTime < 2.5f && L)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wallstuck shut: %s | %s"), *L->DebugSectorBrief(31), *L->DebugSectorBrief(33));
		for (int32 Sx : { 81, 376, 386, 325, 401, 395 }) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wallstuck near: %s"), *L->DebugSectorBrief(Sx));
		FVector P33;
		if (L->DebugSectorSpot(TEXT("33"), P33)) Spots[1] = P33 + FVector(0.0, 0.0, 90.0);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wallstuck pushes: 31 %d, 33 %d"), L->PushDoorSector(31) ? 1 : 0, L->PushDoorSector(33) ? 1 : 0);
		ShotStep = 100;
		return;
	}
	if (ShotStep == 100)
	{
		if (ShotTime < 2.9f) return;
		if (L) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wallstuck open: %s | %s"), *L->DebugSectorBrief(31), *L->DebugSectorBrief(33));
		ShotStep = 0;
	}
	const int32 Si = ShotStep / 2;
	auto Overlap = [this]() -> bool
	{
		const UCapsuleComponent* Cap = GetCapsuleComponent();
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsWallStuck), false, this);
		FCollisionResponseParams RP;
		Cap->InitSweepCollisionParams(Q, RP);
		return GetWorld()->OverlapBlockingTestByChannel(GetActorLocation(), FQuat::Identity, Cap->GetCollisionObjectType(),
			FCollisionShape::MakeCapsule(Cap->GetScaledCapsuleRadius(), Cap->GetScaledCapsuleHalfHeight()), Q, RP);
	};
	auto FeetAt = [this]() { return GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight()); };
	if (Si < 2 && ShotStep % 2 == 0 && ShotTime > 3.f + Si * 5.f)
	{
		SetActorLocation(Spots[Si], false, nullptr, ETeleportType::TeleportPhysics);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wallstuck spot %d placed at %s: overlap %d, feet holder %d, head holder %d; %s"), Si + 1, *Spots[Si].ToCompactString(), Overlap() ? 1 : 0,
			L ? L->SectorHoldingPoint(FeetAt() + FVector(0.0, 0.0, 5.0), 15.0) : -2, L ? L->SectorHoldingPoint(Camera->GetComponentLocation(), 2.0) : -2,
			L ? *L->DebugWallProbe(FeetAt(), 80.0) : TEXT("no level"));
		++ShotStep;
	}
	else if (Si < 2 && ShotStep % 2 == 1 && ShotTime > 7.5f + Si * 5.f)
	{
		// 4.5 s on: past the unstuck's grace for the head (0.75 s) and the body (3 s)
		const double Moved = FVector::Dist(GetActorLocation(), Spots[Si]);
		const int32 FeetHolder = L ? L->SectorHoldingPoint(FeetAt() + FVector(0.0, 0.0, 5.0), 15.0) : -2, HeadHolder = L ? L->SectorHoldingPoint(Camera->GetComponentLocation(), 2.0) : -2;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wallstuck spot %d after 4.5 s: moved %.1f cm to %s, overlap %d, sector %d, feet holder %d, head holder %d"), Si + 1, Moved,
			*GetActorLocation().ToString(), Overlap() ? 1 : 0, L ? L->PlayerSectorIndex() : -1, FeetHolder, HeadHolder);
		if (Si == 0)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wallstuck RESULT %s (in door 31's open doorway: held by a sector, not sent back)"),
				FeetHolder >= 0 && HeadHolder >= 0 && Moved < 1.0 ? TEXT("PASS") : TEXT("FAIL"));
		}
		++ShotStep;
	}
	else if (Si >= 2)
	{
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
