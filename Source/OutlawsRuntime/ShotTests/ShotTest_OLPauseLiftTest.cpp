// -OLShots -OLPauseLiftTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLPauseLiftTest) { return OLFlags::Param(TEXT("OLPauseLiftTest")); }

OL_SHOT_TEST(OLPauseLiftTest, 700)
{
	// -OLLevel=hideout -OLNoFrontEnd -OLShots -OLPauseLiftTest [-OLPauseLiftVR] (BUG-PAUSELIFT): stood at the user's snapshot
	// spot (X 497.34 Y -0.03 Z 184.52, sector 396), the pause menu opened for 3 s and closed: the body, the feet and the eye
	// must not move.  -OLPauseLiftVR runs the headset's body path (the camera held where a still head would be)
	static FVector Feet0, Eye0;
	static double MenuMove = 0.0;      // the most the feet strayed while the menu was up
	auto Report = [this](const TCHAR* When) -> FVector
	{
		const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
		if (bMenuOpen) MenuMove = FMath::Max(MenuMove, FVector::Dist(Feet, Feet0));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT pauselift %s: feet %.2f %.2f Z %.2f, eye Z %.2f, half %.2f, mode %d, menu %d, origin Z %.2f, menu Z %.2f"), When, Feet.X, Feet.Y, Feet.Z,
			Camera->GetComponentLocation().Z, GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight(), (int32)GetCharacterMovement()->MovementMode.GetValue(),
			bMenuOpen ? 1 : 0, VROrigin->GetRelativeLocation().Z, MenuRoot ? MenuRoot->GetComponentLocation().Z : 0.0);
		return Feet;
	};
	if (ShotStep == 0 && ShotTime > 3.f)
	{
		if (OLFlags::Param(TEXT("OLPauseLiftVR"))) bVR = true;
		TeleportTo(FVector(5535.600, 14920.119, -0.850 + GetCapsuleComponent()->GetScaledCapsuleHalfHeight()), FRotator(0.0, -93.43, 0.0), false, true);
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 5.f) { Feet0 = Report(TEXT("before")); Eye0 = Camera->GetComponentLocation(); OpenMenu(); ++ShotStep; }
	else if (ShotStep == 2 && ShotTime > 5.6f)
	{
		// the head leans 25 cm forward and ducks 30 cm while the menu is up (a player reaching for a row)
		Report(TEXT("menu +0.5 s"));
		if (bVR) Camera->SetRelativeLocation(Camera->GetRelativeLocation() + FVector(25.0, 0.0, -30.0));
		++ShotStep;
	}
	else if (ShotStep == 3 && ShotTime > 8.f)
	{
		Report(TEXT("menu +3 s, leaning"));
		if (bVR) Camera->SetRelativeLocation(Camera->GetRelativeLocation() - FVector(25.0, 0.0, -30.0));
		++ShotStep;
	}
	else if (ShotStep == 4 && ShotTime > 8.5f) { Report(TEXT("menu, head back")); CloseMenu(); ShotStep = 40; }
	else if (ShotStep == 40 && ShotTime > 8.6f) { Report(TEXT("closed +0.1 s")); ShotStep = 5; }
	else if (ShotStep == 5 && ShotTime > 10.5f)
	{
		const FVector Feet = Report(TEXT("closed +2 s"));
		const double DFeet = FVector::Dist(Feet, Feet0), DEye = FVector::Dist(Camera->GetComponentLocation(), Eye0);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT pauselift RESULT %s (feet moved %.2f cm behind the menu, %.2f after it; eye %.2f cm; expect each under 0.5)"),
			MenuMove < 0.5 && DFeet < 0.5 && DEye < 0.5 ? TEXT("PASS") : TEXT("FAIL"), MenuMove, DFeet, DEye);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
