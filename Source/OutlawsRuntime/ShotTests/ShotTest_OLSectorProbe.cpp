// -OLShots -OLSectorProbe: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLSectorProbe)
{
	FString ProbeList;
	return OLFlags::Value(TEXT("OLSectorProbe"), ProbeList);
}

OL_SHOT_TEST(OLSectorProbe, 590)
{
	FString ProbeList;
	OLFlags::Value(TEXT("OLSectorProbe"), ProbeList);
	// each listed sector in turn: stand at its middle, a picture, which overlapping rooms are hidden, and what a
	// body sweeping out 150 cm in eight directions meets (user 2026-09-29: the mill's 396 and 592 clip into others)
	TArray<FString> Secs;
	ProbeList.ParseIntoArray(Secs, TEXT("+"));          // (a comma ends the command-line value)
	const int32 k = ShotStep / 2;
	if (!Secs.IsValidIndex(k))
	{
		if (ShotTime > 3.f) { FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; }
		return;
	}
	static FVector Spot = FVector::ZeroVector;
	if (ShotStep % 2 == 0 && ShotTime > 3.f + k * 3.f && L)
	{
		if (L->DebugSectorSpot(Secs[k], Spot))
		{
			SetCrouchHeld(true);
			GetCharacterMovement()->Velocity = FVector::ZeroVector;
			TeleportTo(Spot + FVector(0.0, 0.0, 50.0), GetActorRotation(), false, true);
		}
		++ShotStep;
	}
	else if (ShotStep % 2 == 1 && ShotTime > 4.5f + k * 3.f && L)
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsProbe), false, this);
		L->IgnoreHiddenOverlaps(Q);
		const FCollisionResponseParams RP(GetCapsuleComponent()->GetCollisionResponseToChannels());
		const FCollisionShape Body = FCollisionShape::MakeCapsule(GetCapsuleComponent()->GetScaledCapsuleRadius(), 30.f);     // (a crouched body, off the floor)
		FString Report;
		for (int32 d = 0; d < 8; ++d)
		{
			const FVector Way = FRotator(0.0, d * 45.0, 0.0).Vector();
			const FVector A = Spot + FVector(0.0, 0.0, 45.0);
			FHitResult Hit;
			if (GetWorld()->SweepSingleByChannel(Hit, A, A + Way * 150.0, FQuat::Identity, GetCapsuleComponent()->GetCollisionObjectType(), Body, Q, RP))
				Report += FString::Printf(TEXT(" [%d deg: %s at %.0f cm]"), d * 45, Hit.GetComponent() ? *Hit.GetComponent()->GetName() : TEXT("?"), Hit.Distance);
			else
				Report += FString::Printf(TEXT(" [%d deg: clear]"), d * 45);
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT probe sector %s: standing at %s, in sector %d |%s"), *Secs[k], *GetActorLocation().ToString(), L->PlayerSectorIndex(), *Report);
		if (Secs[k].IsNumeric()) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT shares space with %s:%s"), *Secs[k], *L->DebugOverlapReport(FCString::Atoi(*Secs[k])));
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / (TEXT("ol_probe_") + Secs[k] + TEXT(".png")), false, false);
		++ShotStep;
	}
}

#endif // WITH_OUTLAWS_TESTS
