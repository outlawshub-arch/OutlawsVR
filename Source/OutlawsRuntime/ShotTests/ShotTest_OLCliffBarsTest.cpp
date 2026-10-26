// -OLShots -OLCliffBarsTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLCliffBarsTest) { return OLFlags::Param(TEXT("OLCliffBarsTest")); }

OL_SHOT_TEST(OLCliffBarsTest, 620)
{
	// cliff: the round key in its slot (BTRIG wall 16) slides the bars B1..B6 away; the three bar switches of the first
	// stone door (CSDS1 walls 22, 21, 20 -> counters CSD11, CSD12, CSD13) set to 1, 1, 3 open CSD1 (user 2026-09-29:
	// the sliding-bar puzzles could not be done)
	auto Bars = [L]() { return L ? L->DebugElevatorReport(TEXT("B")) : FString(); };
	auto Doors = [L]() { return L ? L->DebugElevatorReport(TEXT("CSD1")) : FString(); };
	if (ShotStep == 0 && ShotTime > 3.f && L)
	{
		L->DebugGiveListed(TEXT("irkey"), TEXT("irkey"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT bars before: %s"), *Bars());
		const bool bT = L->DebugNudgeWall(TEXT("BTRIG"), TEXT("16"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT round key slot pushed: taken %d, key left %d (expect 0)"), bT ? 1 : 0, L->GetCount(TEXT("irkey")));
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 15.f && L)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT bars 12 s after (expect B1 at 0, B2..B6 at 1): %s"), *Bars());
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT atx: %s"), *L->DebugAtxReport());
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT door before: %s"), *Doors());
		// the switches worked by hand (the grip): 22 once (CSD11 1, up), 21 once (CSD12 1, up), 20 three times (CSD13 3, down)
		bool bT = L->DebugNudgeWall(TEXT("CSDS1"), TEXT("22"));
		bT &= L->DebugNudgeWall(TEXT("CSDS1"), TEXT("21"));
		for (int32 k = 0; k < 3; ++k) bT &= L->DebugNudgeWall(TEXT("CSDS1"), TEXT("20"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT switches worked (all taken %d); bar switches ignore a push: %d (expect 1)"), bT ? 1 : 0, L->DebugWallSlides(195, 22) ? 1 : 0);
		ShotStep = 4;
	}
	else if (ShotStep == 4 && ShotTime > 20.f && L)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT door 3 s after (expect CSD1 on its way to 18; CSD11 1, CSD12 1, CSD13 3): %s"), *Doors());
		++ShotStep;
	}
	else if (ShotStep == 5 && ShotTime > 21.f)
	{
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
