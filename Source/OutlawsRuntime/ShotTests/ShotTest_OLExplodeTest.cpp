// -OLShots -OLExplodeTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLExplodeTest) { return OLFlags::Param(TEXT("OLExplodeTest")); }

OL_SHOT_TEST(OLExplodeTest, 600)
{
	// cliff: the cracked wall (EXPLODE1) stands, blocks, and comes down to a blast in front of it (user 2026-09-29: the
	// crack floated with no wall, an invisible wall stood there, and dynamite did nothing)
	auto Shot = [](const TCHAR* Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(Name) + TEXT(".png"), false, false); };
	static FVector Plug = FVector::ZeroVector, Front = FVector::ZeroVector;
	auto Blocked = [this, L]() -> int32
	{
		// a crouched body swept from in front of the plug through it and on into the tunnel behind (as the player's own
		// moves: hidden overlapping rooms ignored)
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsExplodeTest), false, this);
		if (L) L->IgnoreHiddenOverlaps(Q);
		FHitResult Hit;
		const FVector A = Front + FVector(0.0, 0.0, 75.0), B = Plug + (Plug - Front) * 1.5 + FVector(0.0, 0.0, 75.0);
		const FCollisionResponseParams RP(GetCapsuleComponent()->GetCollisionResponseToChannels());   // (the player's own: fences for bad guys pass)
		const bool bHit = GetWorld()->SweepSingleByChannel(Hit, A, B, FQuat::Identity, GetCapsuleComponent()->GetCollisionObjectType(), FCollisionShape::MakeCapsule(20.f, 40.f), Q, RP);
		if (bHit) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT explode sweep stopped by %s (%s) at %s, %.0f cm along"), Hit.GetComponent() ? *Hit.GetComponent()->GetName() : TEXT("?"),
			Hit.GetActor() ? *Hit.GetActor()->GetName() : TEXT("?"), *Hit.ImpactPoint.ToString(), Hit.Distance);
		return bHit ? 1 : 0;
	};
	if (ShotStep == 0 && ShotTime > 3.f && L)
	{
		FVector Room;
		if (L->DebugSectorSpot(TEXT("EXPLODE1"), Plug) && L->DebugSectorSpot(TEXT("58"), Room))
		{
			FVector Way = Plug - Room;
			Way.Z = 0.0;
			Way = Way.GetSafeNormal();
			Front = Plug - Way * 110.0;
			SetCrouchHeld(true);
			TeleportTo(Front - Way * 60.0 + FVector(0.0, 0.0, 50.0), Way.Rotation(), false, true);
			if (Controller) Controller->SetControlRotation(Way.Rotation());
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT explode before: a sweep through the wall blocked %d (expect 1)"), Blocked());
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 4.5f) { Shot(TEXT("ol_explode_before")); ++ShotStep; }
	else if (ShotStep == 2 && ShotTime > 5.5f && L)
	{
		L->Explode(Front + FVector(0.0, 0.0, 20.0));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT explode: dynamite went off 110 cm in front of the wall"));
		++ShotStep;
	}
	else if (ShotStep == 3 && ShotTime > 8.f && L)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT explode after: a sweep through the wall blocked %d (expect 0, or the tunnel's own rising floor well past the wall)"), Blocked());
		// back where the blast threw the body from, the same look, for the picture
		FVector Way = Plug - Front;
		Way.Z = 0.0;
		Way = Way.GetSafeNormal();
		GetCharacterMovement()->Velocity = FVector::ZeroVector;
		TeleportTo(Plug - Way * 25.0 + FVector(0.0, 0.0, 50.0), Way.Rotation(), false, true);     // (in the gap: past the smoke)
		if (Controller) Controller->SetControlRotation(Way.Rotation());
		++ShotStep;
	}
	else if (ShotStep == 4 && ShotTime > 14.f) { Shot(TEXT("ol_explode_after")); ++ShotStep; }     // (the smoke gone)
	else if (ShotStep == 5 && ShotTime > 15.f)
	{
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
