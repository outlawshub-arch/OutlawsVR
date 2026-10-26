// -OLShots -OLHayTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLHayTest) { return OLFlags::Param(TEXT("OLHayTest")); }

OL_SHOT_TEST(OLHayTest, 630)
{
	// ranch: the rope shot drops the hay bale through the floor, floor, sides and underside together (user
	// 2026-09-29: only its top came down); the INF's own state, and the room seen from beside it
	auto Shot = [](const TCHAR* Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(Name) + TEXT(".png"), false, false); };
	auto Report = [L](const TCHAR* When) { if (L) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hay %s: %s"), When, *L->DebugElevatorReport(TEXT("HAY"))); };
	static FVector Bale = FVector::ZeroVector;
	if (ShotStep == 0 && ShotTime > 3.f && L)
	{
		FVector Room;
		L->DebugSectorSpot(TEXT("HAY2"), Bale);
		if (L->DebugSectorSpot(TEXT("557"), Room))
		{
			TeleportTo(Room + FVector(0.0, 0.0, 92.0), GetActorRotation(), false, true);
			if (Controller) Controller->SetControlRotation((Bale + FVector(0.0, 0.0, 250.0) - (Room + FVector(0.0, 0.0, 250.0))).Rotation());
		}
		Report(TEXT("before"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hay: rope props shot: %d"), L->DebugShootProp(TEXT("ROPEBRK")));
		++ShotStep;
	}
	else if (ShotStep >= 1 && ShotStep <= 2 && ShotTime > 3.f + ShotStep * 0.4f && L)
	{
		// three hits fray the rope through: each plays its next chor, the fourth is the break with the object event
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hay: rope shot again: %d"), L->DebugShootProp(TEXT("ROPEBRK")));
		++ShotStep;
	}
	else if (ShotStep == 3 && ShotTime > 5.5f)
	{
		Report(TEXT("1.5 s after the third shot"));
		Shot(TEXT("ol_hay_falling"));
		++ShotStep;
	}
	else if (ShotStep == 4 && ShotTime > 9.f) { Report(TEXT("6 s after")); Shot(TEXT("ol_hay_after")); ++ShotStep; }
	else if (ShotStep == 5 && ShotTime > 10.f)
	{
		// (user 2026-10-03: can't get down the hole since the bale's sides were fixed) a standing and a crouched body
		// walked off each side of the landed bale, under the cellar's ceiling: what stops them
		FVector Top;
		if (L && L->DebugSectorSpot(TEXT("HAY2"), Top))
		{
			FCollisionQueryParams Q(TEXT("OLHayProbe"), false, this);
			FCollisionResponseParams R;
			GetCapsuleComponent()->InitSweepCollisionParams(Q, R);
			for (float Hh : { 87.f, 42.f })
			{
				const FCollisionShape Body = FCollisionShape::MakeCapsule(40.f, Hh);
				const FVector From = Top + FVector(0.0, 0.0, Hh + 2.0);
				for (const FVector& D : { FVector(1, 0, 0), FVector(-1, 0, 0), FVector(0, 1, 0), FVector(0, -1, 0) })
				{
					FHitResult H;
					const bool bHit = GetWorld()->SweepSingleByChannel(H, From, From + D * 300.0, FQuat::Identity, GetCapsuleComponent()->GetCollisionObjectType(), Body, Q, R);
					UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hay probe hh %.0f dir %s: %s"), Hh, *D.ToCompactString(),
						bHit ? *FString::Printf(TEXT("blocked at %.0f cm by %s (%s) impact z %.0f normal %s, start penetrating %d"), H.Distance,
							H.GetComponent() ? *H.GetComponent()->GetName() : TEXT("?"), H.GetActor() ? *H.GetActor()->GetName() : TEXT("?"),
							H.ImpactPoint.Z, *H.ImpactNormal.ToCompactString(), H.bStartPenetrating ? 1 : 0) : TEXT("clear"));
				}
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hay probe: bale top z %.0f"), Top.Z);
		}
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
