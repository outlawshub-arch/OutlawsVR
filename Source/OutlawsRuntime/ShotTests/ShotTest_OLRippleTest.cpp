// -OLShots -OLRippleTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLRippleTest) { return OLFlags::Param(TEXT("OLRippleTest")); }

OL_SHOT_TEST(OLRippleTest, 1170)
{
	// a water surface from above and from below (its ripples and the rippled view through it)
	auto Shot = [](const TCHAR* Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(Name) + TEXT(".png"), false, false); };
	static FVector W = FVector::ZeroVector;
	const float Due[] = { 3.f, 3.3f, 4.f, 4.3f, 5.f, 5.6f, 6.2f };
	if (ShotStep >= UE_ARRAY_COUNT(Due) || ShotTime < Due[ShotStep]) return;
	switch (ShotStep)
	{
	case 4:
		// deeper, looking along the bottom: the depth tint and the water's colour baked into the walls and floor
		TeleportTo(W - FVector(0.0, 0.0, 150.0), GetActorRotation(), false, true);
		if (Controller) Controller->SetControlRotation(FRotator(-25.0, GetActorRotation().Yaw, 0.0));
		break;
	case 5: Shot(TEXT("ol_ripple_under")); break;
	case 0:
		if (L && L->FindWaterPoint(W)) TeleportTo(W + FVector(0.0, 0.0, 60.0 + 160.0), GetActorRotation(), false, true);
		GetCharacterMovement()->SetMovementMode(MOVE_Flying);
		// the original's splashes on the surface in view: a big one ahead, a small one to the side
		if (L)
		{
			FVector Top;
			const FVector Ahead = FRotator(0.0, GetActorRotation().Yaw, 0.0).Vector() * 150.0;
			if (L->WaterSurfaceHit(W + Ahead + FVector(0.0, 0.0, 300.0), W + Ahead - FVector(0.0, 0.0, 300.0), Top)) L->SpawnSplash(Top, true);
			if (L->WaterSurfaceHit(W + Ahead * 0.6 + FVector(0.0, 80.0, 300.0), W + Ahead * 0.6 + FVector(0.0, 80.0, -300.0), Top)) L->SpawnSplash(Top, false);
		}
		if (Controller) Controller->SetControlRotation(FRotator(-55.0, GetActorRotation().Yaw, 0.0));
		break;
	case 1: Shot(TEXT("ol_ripple_above")); break;
	case 2:
		TeleportTo(W - FVector(0.0, 0.0, 40.0), GetActorRotation(), false, true);
		if (Controller) Controller->SetControlRotation(FRotator(60.0, GetActorRotation().Yaw, 0.0));
		break;
	case 3: Shot(TEXT("ol_ripple_below")); break;
	default: FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; return;
	}
	++ShotStep;
}

#endif // WITH_OUTLAWS_TESTS
