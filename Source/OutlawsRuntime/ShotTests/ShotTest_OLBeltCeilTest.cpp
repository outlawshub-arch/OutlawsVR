// -OLShots -OLBeltCeilTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLBeltCeilTest) { return OLFlags::Param(TEXT("OLBeltCeilTest")); }

OL_SHOT_TEST(OLBeltCeilTest, 680)
{
	// -OLLevel=townex -OLNoFrontEnd -OLShots -OLBeltCeilTest (BUG-BELTCEIL): drops made in mid-air where the user's snapshot
	// had the belt (X 15809 Y 1351 Z 1019, over two bodies) and over a street: each must rest on the floor below
	if (ShotStep == 0 && ShotTime > 3.f && L)
	{
		bool bAll = true;
		int32 n = 0;
		for (const FString& Item : { FString(TEXT("gambelt")), FString(TEXT("gdrop1")), FString(TEXT("gcanteen")) })
		{
			for (const FVector& At : { FVector(15809.222, 1350.584, 1019.467), FVector(16128.0, 2500.5, 600.0) })
			{
				AActor* A = L->SpawnGroundItem(Item, At);
				if (!A) { UE_LOG(LogOutlaws, Display, TEXT("OLSHOT beltceil %s: not made"), *Item); continue; }
				FHitResult Hit;
				FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsBeltCeil), true, A);
				FCollisionResponseParams RP;
				RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
				const bool bFloor = GetWorld()->LineTraceSingleByChannel(Hit, At, At - FVector(0.0, 0.0, 5000.0), ECC_Visibility, Q, RP);
				const double Above = A->GetActorLocation().Z - (bFloor ? Hit.ImpactPoint.Z : 0.0);
				const bool bOk = bFloor && Above > -5.0 && Above < 30.0;
				bAll &= bOk;
				++n;
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT beltceil %s made at Z %.0f: rests at Z %.1f, %.1f cm over the floor (Z %.1f): %s"), *Item, At.Z, A->GetActorLocation().Z, Above,
					bFloor ? Hit.ImpactPoint.Z : -1.0, bOk ? TEXT("ok") : TEXT("FLOATING"));
			}
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT beltceil RESULT %s (%d drops)"), bAll && n > 0 ? TEXT("PASS") : TEXT("FAIL"), n);
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 4.f)
	{
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
