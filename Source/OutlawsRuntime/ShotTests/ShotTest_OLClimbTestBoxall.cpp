// -OLShots -OLClimbTestBoxall: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLClimbTestBoxall)
{
	FString ClimbKind;
	return OLFlags::Value(TEXT("OLClimbTest"), ClimbKind) && ClimbKind.Equals(TEXT("boxall"), ESearchCase::IgnoreCase);
}

OL_SHOT_TEST(OLClimbTestBoxall, 970)
{
	FString ClimbKind;
	OLFlags::Value(TEXT("OLClimbTest"), ClimbKind);
	// (BUG-STUCKGAP) every box under a low ceiling with a room over it (the ceiling-to-floor gaps up to 2.4 m included),
	// one after another: stood before it, the climb started at its lip, two seconds on where the body is (on the box
	// under its ceiling, in the gap or the room above) and whether the stuck rescue fired (it must not)
	static FVector Stand, Lip, In;
	static double TopZ = 0.0, CeilZ = 0.0, AboveZ = 0.0, T0 = 3.0;
	static int32 Nth = 0, OnBox = 0, Rescues0 = 0;
	const int32 Phase = ShotStep % 3;
	if (Phase == 0 && ShotTime > T0 && L)
	{
		FString What;
		if (!L->DebugCeilBoxSpotN(Nth, Stand, Lip, In, What, TopZ, CeilZ, AboveZ))
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT climb test boxall on %s: %d boxes climbed, %d on top under their ceiling; stuck rescues during the climbs %d"), *L->LevelName, Nth, OnBox,
				UnstuckCount);
			FGenericPlatformMisc::RequestExit(false);
			ShotTime = -1.f;
			return;
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT climb test boxall %d: %s"), Nth, *What);
		TeleportTo(Stand + FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0), In.Rotation(), false, true);
		Rescues0 = UnstuckCount;
		++ShotStep;
	}
	else if (Phase == 1 && ShotTime > T0 + 1.f)
	{
		GrabPoint = Lip - In * 3.0 - FVector(0.0, 0.0, 4.0);
		LedgeTopZ = float(Lip.Z);
		LedgeIn = In;
		const bool bStarted = StartMantle();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT climb test boxall %d: the lip at z %.0f, the climb %s%s"), Nth, Lip.Z, bStarted ? TEXT("started to ") : TEXT("refused"),
			bStarted ? *MantleTo.ToCompactString() : TEXT(""));
		++ShotStep;
	}
	else if (Phase == 2 && ShotTime > T0 + 3.f)
	{
		const double Feet = GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		const FVector Head = Camera ? Camera->GetComponentLocation() : GetActorLocation();
		const bool bTop = FMath::Abs(Feet - TopZ) < 20.0;
		OnBox += bTop ? 1 : 0;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT climb test boxall %d: two seconds on, the feet at z %.0f (box top %.0f, its ceiling %.0f, the room above %.0f), half height %.0f, head in sector %d, rescues %d: %s"),
			Nth, Feet, TopZ, CeilZ, AboveZ, GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight(), L ? L->SectorHoldingPoint(Head, 2.0) : -2, UnstuckCount - Rescues0,
			bTop ? TEXT("on the box, under its ceiling") : Feet > CeilZ - 5.0 ? TEXT("IN THE GAP OR THE ROOM ABOVE") : TEXT("not on the box (the climb refused or fell back)"));
		++Nth;
		T0 = ShotTime + 0.5;
		++ShotStep;
	}
}

#endif // WITH_OUTLAWS_TESTS
