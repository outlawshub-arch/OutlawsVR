// -OLShots -OLSkyTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLSkyTest) { return OLFlags::Param(TEXT("OLSkyTest")); }

OL_SHOT_TEST(OLSkyTest, 740)
{
	// the sky and the cloud layer from the start: near the horizon, half way up and nearly overhead
	// (ol_sky_low / _mid / _up), then quit
	static const float Pitch[3] = { 8.f, 35.f, 75.f };
	static const TCHAR* Name[3] = { TEXT("ol_sky_low"), TEXT("ol_sky_mid"), TEXT("ol_sky_up") };
	if (ShotStep < 6 && ShotTime > 4.f + ShotStep * 1.2f)
	{
		const int32 k = ShotStep / 2;
		if (Controller) Controller->SetControlRotation(FRotator(Pitch[k], GetActorRotation().Yaw, 0.0));
		if (ShotStep % 2 == 1) FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / (FString(Name[k]) + TEXT(".png")), false, false);
		if (ShotStep == 0 && L) UE_LOG(LogOutlaws, Display, TEXT("OLRT sky test: %s; camera at %s"), *L->CloudReport(), *Camera->GetComponentLocation().ToString());
		++ShotStep;
	}
	else if (ShotStep == 6 && ShotTime > 12.f)
	{
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
