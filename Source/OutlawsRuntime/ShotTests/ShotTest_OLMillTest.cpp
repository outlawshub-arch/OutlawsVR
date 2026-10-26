// -OLShots -OLMillTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLMillTest) { return OLFlags::Param(TEXT("OLMillTest")); }

OL_SHOT_TEST(OLMillTest, 580)
{
	// mill: the valve wheel in SWITCH67 opens GATE7, whose first stop opens the grate under the water (user 2026-09-29:
	// the hatch stayed shut); and one gear fits one slot only (GRDUMMY steps once, the gear is gone)
	auto Report = [L](const TCHAR* When) { if (L) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mill %s: %s || %s || grate verts %s"), When, *L->DebugElevatorReport(TEXT("GATE7")), *L->DebugElevatorReport(TEXT("GRATE")), *L->DebugSectorVerts(TEXT("GRATE"), 2)); };
	if (ShotStep == 0 && ShotTime > 3.f && L)
	{
		FVector Spot;
		if (L->DebugSectorSpot(TEXT("SWITCH67"), Spot)) TeleportTo(Spot + FVector(0.0, 0.0, 92.0), GetActorRotation(), false, true);
		Report(TEXT("before"));
		// the wheel stands at Outlaws (311, 23, 239.5): the hand at its middle
		const bool bTurned = L->TurnPropByHand(FOutlawsGeometry::ToUE(311.0, 26.0, 239.5));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mill wheel turned by hand: %d (expect 1)"), bTurned ? 1 : 0);
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 5.f) { Report(TEXT("2 s after the wheel")); ++ShotStep; }
	else if (ShotStep == 2 && ShotTime > 9.f) { Report(TEXT("6 s after the wheel (expect GATE7 past stop 1, GRATE at 90)")); ++ShotStep; }
	else if (ShotStep == 3 && ShotTime > 9.5f && L)
	{
		L->DebugGiveListed(TEXT("igear2"), TEXT("igear2"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mill gears carried before: %d"), L->GetCount(TEXT("igear2")));
		const bool bT1 = L->DebugNudgeWall(TEXT("GEAR"), TEXT("1AED8"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mill gear slot pushed: taken %d | %s"), bT1 ? 1 : 0, *L->DebugElevatorReport(TEXT("GRDUMMY")));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mill gears carried after: %d (expect 0)"), L->GetCount(TEXT("igear2")));
		const bool bT2 = L->DebugNudgeWall(TEXT("GEAR"), TEXT("1AED8"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mill gear slot pushed again: taken %d | %s (expect GRDUMMY still headed for stop 1)"), bT2 ? 1 : 0, *L->DebugElevatorReport(TEXT("GRDUMMY")));
		// the sewer gate button (MASWITCH): both halves of the hatch swing, the current turns
		const bool bS = L->DebugNudgeWall(TEXT("SEWERGATE1T"), TEXT("8658"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mill sewer button pushed: taken %d"), bS ? 1 : 0);
		++ShotStep;
	}
	else if (ShotStep == 4 && ShotTime > 10.5f && L)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mill sewer gate 1 s after (expect both halves at -52 or on the way, VEL51 on): %s || %s"), *L->DebugElevatorReport(TEXT("SEWERGATE1")), *L->DebugElevatorReport(TEXT("VEL51")));
		// a stick of dynamite on the ground: a pickup now
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mill dynamite pickups: %s"), *L->PickupLocation(TEXT("gdynam")).ToString());
		++ShotStep;
	}
	else if (ShotStep == 5 && ShotTime > 13.5f && L)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mill sewer gate 4 s after (expect closed again after its 2 s wait): %s"), *L->DebugElevatorReport(TEXT("SEWERGATE1")));
		++ShotStep;
	}
	else if (ShotStep == 6 && ShotTime > 14.f)
	{
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
