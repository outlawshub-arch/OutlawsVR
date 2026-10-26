// -OLShots -OLShatterTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLShatterTest) { return OLFlags::Param(TEXT("OLShatterTest")); }

OL_SHOT_TEST(OLShatterTest, 1190)
{
	// the first window broken from where it can be seen, then a wait for trouble
	const float Due[] = { 3.f, 4.f, 12.f, 13.f };
	if (ShotStep >= UE_ARRAY_COUNT(Due) || ShotTime < Due[ShotStep]) return;
	FVector Floor;
	float Yaw = 0.f;
	FString What;
	switch (ShotStep)
	{
	case 0:
		if (L->DebugMidView(0, true, Floor, Yaw, What))
		{
			TeleportTo(Floor + FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0), FRotator(0.0, Yaw, 0.0), false, true);
			if (AController* C = GetController()) C->SetControlRotation(FRotator(0.0, Yaw, 0.0));
		}
		break;
	case 1:
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);      // what a long session does sooner or later
		L->DebugShatter(0);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT shattered pane 0 after a garbage collection"));
		break;
	case 2: UE_LOG(LogOutlaws, Display, TEXT("OLSHOT still alive 8 s after the shatter")); FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_shatter.png"), false, false); break;
	default: FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; break;
	}
	++ShotStep;
}

#endif // WITH_OUTLAWS_TESTS
