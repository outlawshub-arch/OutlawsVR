// -OLShots -OLScopeTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLScopeTest) { return OLFlags::Param(TEXT("OLScopeTest")); }

OL_SHOT_TEST(OLScopeTest, 570)
{
	// the scope's view, forced on (desktop has no hands to raise): a shot of it, then quit
	if (ShotStep == 0 && ShotTime > 3.f)
	{
		if (L) L->GiveAllWeapons();
		Equip(1, 1);
		bScopeTestForce = true;
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 10.f)      // (a first run compiles the scope's shader)
	{
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_scope.png"), false, false);
		const APlayerCameraManager* Cm = UGameplayStatics::GetPlayerCameraManager(this, 0);
		UE_LOG(LogOutlaws, Display, TEXT("OLRT scope test: sheet %s visible %d material %s at %s; view camera at %s looking %s; capture at %s every frame %d"),
			ScopeView ? TEXT("made") : TEXT("missing"), ScopeView ? ScopeView->IsVisible() : -1,
			ScopeView && ScopeView->GetMaterial(0) ? *ScopeView->GetMaterial(0)->GetName() : TEXT("none"),
			ScopeView ? *ScopeView->GetComponentLocation().ToString() : TEXT("-"),
			Cm ? *Cm->GetCameraLocation().ToString() : TEXT("-"), Cm ? *Cm->GetCameraRotation().Vector().ToString() : TEXT("-"),
			ScopeCapture ? *ScopeCapture->GetComponentLocation().ToString() : TEXT("-"), ScopeCapture ? ScopeCapture->bCaptureEveryFrame : -1);
		++ShotStep;
	}
	else if (ShotStep == 2 && ShotTime > 11.f)
	{
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
