// -OLShots -OLLedgeTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLLedgeTest) { return OLFlags::Param(TEXT("OLLedgeTest")); }

OL_SHOT_TEST(OLLedgeTest, 870)
{
	// (REQ-LEDGEKNOCK) the first bad guy with a drop beside him, shot dead from the side away from it (the push into
	// the shooter's own way): does he go over (more than 2.5 m down two seconds on)
	static TWeakObjectPtr<AOutlawsEnemy> Man;
	static double StartZ = 0.0;
	if (ShotStep == 0 && ShotTime > 4.f && L)
	{
		for (TActorIterator<AOutlawsEnemy> It(GetWorld()); It && !Man.IsValid(); ++It)
		{
			if (It->IsDead() || It->bBoss) continue;
			FVector Edge;
			float D = 0.f;
			if (!It->FindDropEdge(FVector(1.0, 0.0, 0.0), Edge, D)) continue;
			Man = *It;
			StartZ = It->GetActorLocation().Z;
			It->SetDormant(false);
			const FVector Dir = -Edge;                           // (the shot comes from beyond the drop, pushing away from it)
			// (a revolver's shots, 1 each: the kill's push is the drop's own, not a huge hit's)
			for (int32 k = 0; k < 60 && !It->IsDead(); ++k) It->TakeHit(1.f, It->GetActorLocation(), Dir, It->GetActorLocation() - Dir * 300.0);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT ledge test: %s, a drop %.0f cm off, shot from beyond it (dead %d)"), *It->GetOutlawsName(), D, It->IsDead() ? 1 : 0);
		}
		if (!Man.IsValid()) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT ledge test: no bad guy beside a drop here"));
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 7.f)
	{
		const double Fell = Man.IsValid() ? StartZ - Man->GetActorLocation().Z : 0.0;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT ledge test: fell %.0f cm (%s)"), Fell, Fell > 250.0 ? TEXT("over the edge") : TEXT("stayed up"));
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
