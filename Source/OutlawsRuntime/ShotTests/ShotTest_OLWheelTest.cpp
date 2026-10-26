// -OLShots -OLWheelTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLWheelTest) { return OLFlags::Param(TEXT("OLWheelTest")); }

OL_SHOT_TEST(OLWheelTest, 760)
{
	// the weapon wheel in the remaster's art, open in front of the camera, with a pick shown
	if (ShotStep == 0 && ShotTime > 3.f)
	{
		if (L) L->GiveAllWeapons();
		// the shotguns and the knife with nothing to fire: blacked out (user 2026-09-27)
		if (L)
		{
			L->TakeCount(TEXT("ishells"), L->GetCount(TEXT("ishells")));
			L->TakeCount(TEXT("iknife"), L->GetCount(TEXT("iknife")));
			for (const int32 Wi : { 2, 3, 4 }) for (int32 Hd = 0; Hd < 2; ++Hd) SavedMag.Add(Hd * 16 + Wi, 0);
		}
		// and items of several types for the row under the horseshoe (BUG-WHEELINV: past seven a second row; the stone and
		// steel keys by their cards' own names; the scope, a weapon, never listed)
		if (L)
		{
			L->AddCount(TEXT("idocbag"), 2);
			for (const TCHAR* K : { TEXT("isqrkeya"), TEXT("isqrkeyb"), TEXT("isqrkeyc"), TEXT("igear1"), TEXT("igear2"), TEXT("ibrsskey"), TEXT("isteekey"),
				TEXT("iironkey"), TEXT("icrowbar"), TEXT("idynable"), TEXT("ishovel"), TEXT("iscope") }) L->DebugGiveListed(K, K);
		}
		Equip(1, 0);
		OpenWheel(1);
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 4.f)
	{
		WheelPick[1] = 3;          // as if the right laser pointed at the shotgun
		bWheelHeld[1] = true;
		++ShotStep;
	}
	else if (ShotStep == 2 && ShotTime > 5.f)
	{
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_wheel.png"), false, false);
		// (a count plate's own canvas: its letters red, their outline green, M_OLR_TextPlate's channels)
		for (int32 Pi = 0; Pi < PlateRTs.Num(); ++Pi)
		{
			if (PlateShown[Pi].IsEmpty()) continue;
			FArchive* Ar = IFileManager::Get().CreateFileWriter(*(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_plate_rt.png")));
			if (Ar) { FImageUtils::ExportRenderTarget2DAsPNG(PlateRTs[Pi], *Ar); delete Ar; }
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT plate canvas %d: '%s'"), Pi, *PlateShown[Pi]);
			break;
		}
		++ShotStep;
	}
	else if (ShotStep == 3 && ShotTime > 6.f)
	{
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
