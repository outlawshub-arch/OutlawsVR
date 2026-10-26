// -OLShots -OLDrownTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLDrownTest) { return OLFlags::Param(TEXT("OLDrownTest")); }

OL_SHOT_TEST(OLDrownTest, 340)
{
	// under water with little air left (user 2026-10-01): the dark closing in, then choking every 5 s
	static int32 Sec = -1;
	if (L) L->bInvincible = false;           // (the shots tests keep the player invincible: drowning must hurt)
	if (ShotStep == 0 && ShotTime > 3.f && L)
	{
		Sec = L->DebugWaterSector();
		FVector Feet;
		float SlopeYaw = 0.f;
		if (Sec >= 0 && L->SlopeFoot(Sec, Feet, SlopeYaw)) TeleportTo(Feet + FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0), GetActorRotation(), false, true);
		L->bInvincible = false;
		Air = 0.2f;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT drown test: water sector %d"), Sec);
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 4.5f)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT under %d, air %.2f, health %.1f"), bHeadUnder ? 1 : 0, Air, L ? L->GetHealth() : -1.f);
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_breath.png"), false, false);
		++ShotStep;
	}
	else if (ShotStep == 2 && ShotTime > 19.f)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT drowning: air %.2f, health %.1f (expect 7 to 8 after 2 to 3 chokes)"), Air, L ? L->GetHealth() : -1.f);
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_drown.png"), false, false);
		++ShotStep;
	}
	else if (ShotStep == 3 && ShotTime > 20.f && L)
	{
		// (GULP) up into the air with none left: the gasp is logged ("OLRT surfaced with ... a gasp")
		TeleportTo(L->GetPlayerStartLocation(), GetActorRotation(), false, true);
		++ShotStep;
	}
	else if (ShotStep == 4 && ShotTime > 22.f) { FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; }
}

#endif // WITH_OUTLAWS_TESTS
