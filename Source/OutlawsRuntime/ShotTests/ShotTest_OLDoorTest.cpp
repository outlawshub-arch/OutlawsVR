// -OLShots -OLDoorTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLDoorTest) { return OLFlags::Param(TEXT("OLDoorTest")); }

OL_SHOT_TEST(OLDoorTest, 720)
{
	// ranch: BOBLOCKED opens when its leaf is pushed from one side and says "locked from the other side" from the
	// other (user 2026-09-29: the doors locked from inside would not open)
	auto Report = [L](const TCHAR* When) { if (L) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT door %s: %s"), When, *L->DebugElevatorReport(TEXT("BOBLOCKED"))); };
	// the room (599) has its floor at 14 and its ceiling at 30; the leaf sector (46) is 29..30, so the leaf is the
	// room's bottom band, 14 to 29, with a slot over it
	auto Stand = [this](double OlX, double OlZ)
	{
		TeleportTo(FOutlawsGeometry::ToUE(OlX, 14.0, OlZ) + FVector(0.0, 0.0, 92.0), GetActorRotation(), false, true);
	};
	auto Push = [this, L](double OlX, double LeafX) -> bool
	{
		const FVector Eye = FOutlawsGeometry::ToUE(OlX, 14.0, 480.0) + FVector(0.0, 0.0, 160.0);
		const FVector Hand = FOutlawsGeometry::ToUE(LeafX, 18.0, 480.0);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT door: player sector %d"), L ? L->PlayerSectorIndex() : -1);
		return L && L->UseByHand(Eye, Hand, true);
	};
	// the locked side first (the door still shut), then the side that opens it
	if (ShotStep == 0 && ShotTime > 3.f) { Stand(325.5, 480.0); Report(TEXT("before")); ++ShotStep; }
	else if (ShotStep == 1 && ShotTime > 4.f) { UE_LOG(LogOutlaws, Display, TEXT("OLSHOT door pushed from the east: taken %d (expect the 'locked' message 807, no move)"), Push(325.5, 322.95) ? 1 : 0); ++ShotStep; }
	else if (ShotStep == 2 && ShotTime > 5.5f) { Report(TEXT("1.5 s after the east push")); Stand(320.0, 480.0); ++ShotStep; }
	else if (ShotStep == 3 && ShotTime > 6.5f) { UE_LOG(LogOutlaws, Display, TEXT("OLSHOT door pushed from the west: taken %d (expect it to open)"), Push(320.0, 322.3) ? 1 : 0); ++ShotStep; }
	else if (ShotStep == 4 && ShotTime > 8.f)
	{
		Report(TEXT("end"));
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
