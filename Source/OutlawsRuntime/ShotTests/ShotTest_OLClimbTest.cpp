// -OLShots -OLClimbTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLClimbTest)
{
	FString ClimbKind;
	return OLFlags::Value(TEXT("OLClimbTest"), ClimbKind) && !ClimbKind.IsEmpty();
}

OL_SHOT_TEST(OLClimbTest, 980)
{
	FString ClimbKind;
	OLFlags::Value(TEXT("OLClimbTest"), ClimbKind);
	// (BUG-CLIMBCEIL / BUG-CLIMBBARS) -OLLevel=simms -OLNoFrontEnd -OLShots -OLClimbTest=box|bars: stood before the level's
	// box under a low ceiling with a room over it, or its barred window; the climb started at the lip; where the body is
	// two seconds on (on the box, in the room above, through the bars or still outside)
	static FVector Stand, Lip, In;
	static double TopZ = 0.0, CeilZ = 0.0, AboveZ = 0.0;
	static bool bStarted = false;
	const bool bBox = ClimbKind.Equals(TEXT("box"), ESearchCase::IgnoreCase);
	if (ShotStep == 0 && ShotTime > 3.f && L)
	{
		FString What;
		const bool bSpot = bBox ? L->DebugCeilBoxSpot(Stand, Lip, In, What, TopZ, CeilZ, AboveZ) : L->DebugBarsSpot(Stand, Lip, In, What);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT climb test %s: %s"), *ClimbKind, bSpot ? *What : TEXT("no such spot in this level"));
		if (!bSpot) { FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; return; }
		TeleportTo(Stand + FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0), In.Rotation(), false, true);
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 4.f)
	{
		GrabPoint = Lip - In * 3.0 - FVector(0.0, 0.0, 4.0);
		LedgeTopZ = float(Lip.Z);
		LedgeIn = In;
		bStarted = StartMantle();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT climb test %s: the lip at z %.0f, the climb %s%s"), *ClimbKind, Lip.Z, bStarted ? TEXT("started to ") : TEXT("refused"),
			bStarted ? *MantleTo.ToCompactString() : TEXT(""));
		++ShotStep;
	}
	else if (ShotStep == 2 && ShotTime > 6.f)
	{
		const double Feet = GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		const double Across = FVector::DotProduct(GetActorLocation() - Lip, In);
		FString Verdict;
		if (bBox) Verdict = FMath::Abs(Feet - TopZ) < 20.0 ? TEXT("on the box, under its ceiling") : Feet > CeilZ ? TEXT("IN THE ROOM ABOVE") : TEXT("not on the box");
		else Verdict = Across > 5.0 ? TEXT("THROUGH THE BARS") : TEXT("still this side of the bars");
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT climb test %s: two seconds on, the feet at z %.0f (box top %.0f, its ceiling %.0f, the room above %.0f), %.0f cm past the lip, half height %.0f: %s"),
			*ClimbKind, Feet, TopZ, CeilZ, AboveZ, Across, GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight(), *Verdict);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
