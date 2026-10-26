// -OLShots -OLSwimSpeedTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

extern bool GSwimStrokeTest;                // (OutlawsPlayer.cpp: the strokes run without a headset)

OL_SHOT_MATCH(OLSwimSpeedTest) { return OLFlags::Param(TEXT("OLSwimSpeedTest")); }

OL_SHOT_TEST(OLSwimSpeedTest, 350)
{
	// (BUG-SWIMSPEED) arm swimming at the Movement Speed dial's 40, 100 and 200 %: both hands swung hard (4 m/s) for
	// 2.5 s each; the peak speed should be the walk's cap at that dial, not past it
	static const float Dials[3] = { 0.2f, 0.5f, 0.75f };
	static int32 Dial = 0;
	static float Since = 0.f, Peak = 0.f;
	static FVector Start = FVector::ZeroVector;
	UCharacterMovementComponent* MS = GetCharacterMovement();
	if (ShotStep == 0 && ShotTime > 3.f)
	{
		const int32 Sec = L->DebugWaterSector();
		FVector Feet;
		float SlopeYaw = 0.f;
		if (Sec < 0 || !L->SlopeFoot(Sec, Feet, SlopeYaw))
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT swim speed: no water sector here"));
			FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; return;
		}
		Start = Feet + FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 40.0);
		GSwimStrokeTest = true;
		L->GameSpeed = Dials[Dial];
		TeleportTo(Start, GetActorRotation(), false, true);
		MS->Velocity = FVector::ZeroVector;
		if (GetController()) GetController()->SetControlRotation(FRotator(0.0, GetActorRotation().Yaw, 0.0));
		Since = 0.f; Peak = 0.f;
		++ShotStep;
	}
	else if (ShotStep == 1)
	{
		Since += Dt;
		if (Since > 0.5f)
		{
			HandVelocity[0] = HandVelocity[1] = GetActorForwardVector() * 400.0;
			Peak = FMath::Max(Peak, float(MS->Velocity.Size()));
		}
		if (Since > 3.f)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT swim speed: dial %.0f %%, swimming %d, arm swim peak %.0f cm/s, walk cap %.0f cm/s (expect equal or under), stick swim cap %.0f"),
				Dials[Dial] * 200.f, bSwimming ? 1 : 0, Peak, MS->MaxWalkSpeed, MS->MaxFlySpeed);
			HandVelocity[0] = HandVelocity[1] = FVector::ZeroVector;
			if (++Dial >= 3) { GSwimStrokeTest = false; L->GameSpeed = 0.5f; FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; return; }
			L->GameSpeed = Dials[Dial];
			TeleportTo(Start, GetActorRotation(), false, true);
			MS->Velocity = FVector::ZeroVector;
			Since = 0.f; Peak = 0.f;
		}
	}
}

#endif // WITH_OUTLAWS_TESTS
