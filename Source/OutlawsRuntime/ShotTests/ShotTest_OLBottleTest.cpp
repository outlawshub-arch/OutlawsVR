// -OLShots -OLBottleTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLBottleTest) { return OLFlags::Param(TEXT("OLBottleTest")); }

OL_SHOT_TEST(OLBottleTest, 730)
{
	// the bottles (user 2026-09-28): the right hand put on the nearest one takes it (real size: about 30 cm tall, the
	// shelf's stand 52), a hard throw bursts it (ol_bottle_burst.png mid-burst), a second one let go where it
	// stood stands again; the numbers come back as OLSHOT lines
	auto Size = [this]() { return HeldBottle[1] ? HeldBottle[1]->Bounds.BoxExtent.Z * 2.0 : 0.0; };
	auto Bottles = [this](bool bResting) { int32 N = 0; for (const FThrown& T : Thrown) N += T.Type == 3 && T.bResting == bResting; return N; };
	auto TakeNearest = [this, L]() -> bool
	{
		FVector Spot;
		if (!L || !L->DebugBottleSpot(GetActorLocation(), Spot)) return false;
		Equip(1, -1);
		SetActorLocation(GetActorLocation() + (Spot - GripOf(1)->GetComponentLocation()), false, nullptr, ETeleportType::TeleportPhysics);
		if (Controller) Controller->SetControlRotation(FRotator(-20.0, GetActorRotation().Yaw, 0.0));
		return TryHoldBottle(1);
	};
	if (ShotStep == 0 && ShotTime > 3.f)
	{
		const bool bTook = TakeNearest();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT bottle taken: %d, %.1f cm tall as it leaves the shelf (expect about 52)"), bTook ? 1 : 0, Size());
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 4.f)
	{
		// thrown up and out across the room (away from the shelf's wall), the view turned onto it
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT in the hand: %.1f cm tall (expect about 30), %.1f cm from the fist (expect about 3)"), Size(),
			HeldBottle[1] ? FVector::Dist(HeldBottle[1]->Bounds.Origin, FrameOf(1)->GetComponentLocation()) : -1.0);
		BottleTestAt = HeldBottle[1] ? HeldBottle[1]->Bounds.Origin : GetActorLocation();
		const FVector Out = FVector(-Camera->GetForwardVector().X, -Camera->GetForwardVector().Y, 0.0).GetSafeNormal();
		HandWorldVel[1] = Out * 450.0 + FVector(0.0, 0.0, 250.0);
		ReleaseBottle(1);
		if (Controller) Controller->SetControlRotation((BottleTestAt + Out * 120.0 - Camera->GetComponentLocation()).Rotation());
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT thrown: %d in flight (expect 1)"), Bottles(false));
		++ShotStep;
	}
	else if (ShotStep == 2 && ShotTime <= 4.55f)
	{
		// the view follows the bottle while it flies, and stays on where it burst
		for (const FThrown& T : Thrown) if (T.Type == 3 && !T.bResting) BottleTestAt = T.Pos;
		if (Controller) Controller->SetControlRotation((BottleTestAt - Camera->GetComponentLocation()).Rotation());
	}
	else if (ShotStep == 2 && ShotTime > 4.55f)
	{
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_bottle_burst.png"), false, false);
		++ShotStep;
	}
	else if (ShotStep == 3 && ShotTime > 6.5f)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT after the throw: %d in flight, %d standing (expect 0, 0: it burst)"), Bottles(false), Bottles(true));
		const bool bTook = TakeNearest();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT second bottle taken %d"), bTook ? 1 : 0);
		++ShotStep;
	}
	else if (ShotStep == 4 && ShotTime > 7.f && ShotTime <= 8.5f)
	{
		// in the fist now: the hand lowered onto the shelf it came from and let go there, a set-down
		if (HeldBottle[1])
		{
			FVector Spot;
			const FVector Hold = HeldBottle[1]->Bounds.Origin;
			const double Bottom = HeldBottle[1]->Bounds.GetBox().Min.Z;
			FHitResult Shelf;
			FCollisionQueryParams SQ(SCENE_QUERY_STAT(OutlawsBottleTest), false, this);
			if (GetWorld()->LineTraceSingleByChannel(Shelf, Hold, Hold - FVector(0.0, 0.0, 400.0), ECC_Visibility, SQ))
			{
				AddActorWorldOffset(FVector(0.0, 0.0, Shelf.ImpactPoint.Z + 3.0 - Bottom), false, nullptr, ETeleportType::TeleportPhysics);
			}
			HandWorldVel[1] = FVector::ZeroVector;
			BottleTestAt = HeldBottle[1]->Bounds.Origin;
			ReleaseBottle(1);
			if (Controller) Controller->SetControlRotation((BottleTestAt - Camera->GetComponentLocation()).Rotation());
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT let go 3 cm over the shelf"));
		}
		ShotStep = 5;
	}
	else if (ShotStep == 5 && ShotTime > 8.5f)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT set down: %d standing, %d in flight (expect 1, 0)"), Bottles(true), Bottles(false));
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_bottle_setdown.png"), false, false);
		++ShotStep;
	}
	else if (ShotStep == 6 && ShotTime > 10.f)
	{
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
