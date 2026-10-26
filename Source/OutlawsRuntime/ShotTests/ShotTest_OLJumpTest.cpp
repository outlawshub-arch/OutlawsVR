// -OLShots -OLJumpTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLJumpTest) { return OLFlags::Param(TEXT("OLJumpTest")); }

OL_SHOT_TEST(OLJumpTest, 390)
{
	// a running jump's reach at the 20 % and the 100 % movement speeds (user 2026-10-02: the same gaps at every speed):
	// from the level's start, a second and a half of walking, a jump, the ground covered from the take-off to the landing
	static int32 Phase = 0;
	static float T = 0.f;
	static FVector Start, Off;
	static const float Speeds[4] = { 0.2f, 0.4f, 0.6f, 0.75f };
	AOutlawsLevelActor* JL = Level.Get();
	UCharacterMovementComponent* JM = GetCharacterMovement();
	if (!JL || ShotTime < 3.f) return;
	if (Phase == 0) { Start = GetActorLocation(); Phase = 1; }
	const int32 Run = (Phase - 1) / 3, Step = (Phase - 1) % 3;
	if (Run >= 4) { FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; return; }
	T += GetWorld()->GetDeltaSeconds();
	if (Step == 0)
	{
		JL->GameSpeed = Speeds[Run];
		SetActorLocation(Start, false, nullptr, ETeleportType::TeleportPhysics);
		JM->Velocity = FVector::ZeroVector;
		T = 0.f;
		++Phase;
	}
	else if (Step == 1)
	{
		AddMovementInput(FRotator(0.0, GetControlRotation().Yaw, 0.0).Vector(), 1.f);
		if (T > 1.5f) { Off = GetActorLocation(); DoJump(); T = 0.f; ++Phase; }
	}
	else
	{
		AddMovementInput(FRotator(0.0, GetControlRotation().Yaw, 0.0).Vector(), 1.f);
		if (T > 0.15f && !JM->IsFalling())
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT jump test: speed %.0f %%, walked %.0f cm in 1.5 s, the jump covered %.0f cm in %.2f s"), Speeds[Run] * 100.f,
				FVector::Dist2D(Start, Off), FVector::Dist2D(Off, GetActorLocation()), T);
			++Phase;
		}
		else if (T > 5.f) { UE_LOG(LogOutlaws, Display, TEXT("OLSHOT jump test: never landed")); ++Phase; }
	}
}

#endif // WITH_OUTLAWS_TESTS
