// -OLShots -OLTrainPlayerTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLTrainPlayerTest) { return OLFlags::Param(TEXT("OLTrainPlayerTest")); }

OL_SHOT_TEST(OLTrainPlayerTest, 910)
{
	// (BUG-TRAINPLAYER) -OLLevel=train -OLShots -OLTrainPlayerTest: the player (invincible in tests) first stood in a gap between
	// two cars (a moving floor that does not kill): where the carry takes him in 1.2 s (never onto the deadly ground); then on
	// the open moving ground beside the train: the carry's speed every 0.1 s as it eases in, then how far he goes in a second
	// against the ground's own speed
	static FVector Start = FVector::ZeroVector, TestFrom = FVector::ZeroVector;
	static float NextAt = 0.f, PlacedAt = 0.f;
	auto Place = [this](const FVector& Ground)
	{
		GetCharacterMovement()->StopMovementImmediately();
		SetActorLocation(Ground + FVector(0.0, 0.0, 2.0 + GetCapsuleComponent()->GetScaledCapsuleHalfHeight()), false, nullptr, ETeleportType::TeleportPhysics);
	};
	if (ShotStep == 0 && ShotTime > 5.f)
	{
		FVector Gap;
		TestFrom = GetActorLocation();
		if (!L->DebugMovingGround(TestFrom, Gap, 1))
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT train player test: no harmless moving floor"));
			FGenericPlatformMisc::RequestExit(false);
			ShotTime = -1.f;
			return;
		}
		Place(Gap);
		Start = GetActorLocation();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT train player test: stood in a gap between cars at %s (it carries at %.0f cm/s)"), *Gap.ToCompactString(),
			L->FloorVelocityAt(Gap + FVector(0.0, 0.0, 5.0)).Size());
		PlacedAt = ShotTime;
		NextAt = ShotTime + 0.1f;
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime >= NextAt)
	{
		NextAt = ShotTime + 0.1f;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT train player test gap %.2f s: at %s, moved %.0f cm, carry %.0f cm/s"), ShotTime - PlacedAt, *GetActorLocation().ToCompactString(),
			FVector::Dist2D(GetActorLocation(), Start), L->DebugPlayerCarry().Size());
		if (ShotTime - PlacedAt >= 1.2f)
		{
			FVector Ground;
			// (the ground nearest where the player began, not the gap: that one is in the middle of the track)
			if (L->DebugMovingGround(TestFrom, Ground, 2))
			{
				Ground.Y += (Ground.Y > TestFrom.Y ? 1.0 : -1.0) * 300.0;
				Place(Ground);
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT train player test: stood on the open moving ground at %s (it carries at %.0f cm/s)"), *Ground.ToCompactString(),
					L->FloorVelocityAt(Ground + FVector(0.0, 0.0, 5.0)).Size());
			}
			PlacedAt = ShotTime;
			NextAt = ShotTime + 0.1f;
			++ShotStep;
		}
	}
	else if (ShotStep == 2 && ShotTime >= NextAt)
	{
		NextAt = ShotTime + 0.1f;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT train player test ground %.2f s: carry %.0f cm/s"), ShotTime - PlacedAt, L->DebugPlayerCarry().Size());
		if (ShotTime - PlacedAt >= 1.f)
		{
			Start = GetActorLocation();
			NextAt = ShotTime + 1.f;
			++ShotStep;
		}
	}
	else if (ShotStep == 3 && ShotTime >= NextAt)
	{
		const float Span = 1.f + Dt;
		const FVector Vel = L->FloorVelocityAt(GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() - 5.0));
		const FVector Moved = GetActorLocation() - Start;
		const double Along = Vel.IsNearlyZero() ? 0.0 : FVector::DotProduct(Moved, Vel.GetSafeNormal());
		const double Want = Vel.Size() * Span;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT train player test: the player moved %.0f cm along the ground (%.0f cm in all) in %.2f s; the ground carries at %.0f cm/s, %.0f cm wanted, pace %.2f (%s)"),
			Along, Moved.Size(), Span, Vel.Size(), Want, L->Pace(), Want > 1.0 && FMath::Abs(Along - Want) < 0.1 * Want ? TEXT("full speed") : TEXT("NOT the ground's speed"));
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
