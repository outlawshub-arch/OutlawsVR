// -OLShots -OLToneTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLToneTest) { return OLFlags::Param(TEXT("OLToneTest")); }

OL_SHOT_TEST(OLToneTest, 1180)
{
	// the sector palette grade under water, then the death tone, as screenshots
	auto Shot = [](const TCHAR* Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(Name) + TEXT(".png"), false, false); };
	const float Due[] = { 3.f, 6.f, 7.f, 9.f, 10.f, 11.5f, 12.5f };
	if (ShotStep >= UE_ARRAY_COUNT(Due) || ShotTime < Due[ShotStep]) return;
	FVector W;
	switch (ShotStep)
	{
	case 0:
		if (L->FindWaterPoint(W)) TeleportTo(W - FVector(0.0, 0.0, 150.0), GetActorRotation(), false, true);
		if (AController* C = GetController()) C->SetControlRotation(FRotator(0.0, GetActorRotation().Yaw, 0.0));
		break;
	case 1: Shot(TEXT("ol_water")); break;
	case 2:
		TeleportTo(L->GetPlayerStartLocation(), FRotator(0.0, L->GetPlayerStartYaw(), 0.0), false, true);
		if (AController* C = GetController()) C->SetControlRotation(FRotator(10.0, L->GetPlayerStartYaw() + 180.0, 0.0));
		break;
	case 3: Shot(TEXT("ol_alive")); break;
	case 4: L->bInvincible = false; L->DamagePlayer(50.f); break;
	case 5: Shot(TEXT("ol_dead")); break;
	default: FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; break;
	}
	++ShotStep;
}

#endif // WITH_OUTLAWS_TESTS
