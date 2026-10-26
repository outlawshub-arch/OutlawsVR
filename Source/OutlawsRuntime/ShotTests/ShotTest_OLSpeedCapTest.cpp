// -OLShots -OLSpeedCapTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLSpeedCapTest) { return OLFlags::Param(TEXT("OLSpeedCapTest")); }

OL_SHOT_TEST(OLSpeedCapTest, 560)
{
	// BUG-SPEEDCAP (-OLLevel=hideout -OLNoFrontEnd -OLShots): the Movement speed row pressed twelve times from 40 % shows 50 ... 150 %, then 40 %
	// again; a saved 200 % (1.0) loads as 150 % (0.75); a co-op host's -OLCoopSpeed=1 is 0.75 (the client's check is the pair run)
	static int32 Fails = 0, Passes = 0;
	auto Check = [](bool bOk, const FString& What) { UE_LOG(LogOutlaws, Display, TEXT("OLSPEEDCAP check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What); if (bOk) ++Passes; else ++Fails; };
	if (!L) return;
	if (ShotStep == 0 && ShotTime > 3.f)
	{
		L->GameSpeed = 0.2f;
		FString Seq;
		bool bRows = true;
		for (int32 i = 0; i < 12; ++i)
		{
			bRows = VrSpeedMenuTestPress() && bRows;
			Seq += FString::Printf(TEXT("%s%d"), i ? TEXT(",") : TEXT(""), FMath::RoundToInt(L->GameSpeed * 200.f));
		}
		Check(bRows, TEXT("the Options > VR page has the Movement speed row"));
		Check(Seq == TEXT("50,60,70,80,90,100,110,120,130,140,150,40"), FString::Printf(TEXT("the row cycles %s"), *Seq));
		// the row already at the top (150 %) goes round to 40 %, and one over the top (an old value) too
		L->GameSpeed = 0.75f;
		VrSpeedMenuTestPress();
		Check(FMath::IsNearlyEqual(L->GameSpeed, 0.2f, 0.001f), FString::Printf(TEXT("150 %% then the row: %.0f %%"), L->GameSpeed * 200.f));
		Check(AOutlawsLevelActor::ClampGameSpeed(1.f) == 0.75f && AOutlawsLevelActor::ClampGameSpeed(0.f) == 0.2f, TEXT("the clamp: 1.0 is 0.75, 0 is 0.2"));
		// a saved 200 %: the settings file written with it, then read as at a start
		L->GameSpeed = 1.f;
		SaveSettings();
		L->GameSpeed = 0.5f;
		FString Text;
		const FString Path = FPaths::ProjectSavedDir() / TEXT("OutlawsSettings.ini");
		FFileHelper::LoadFileToString(Text, *Path);
		Text += TEXT("\nMovementSpeed=1.0\n");
		FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		L->LoadSettings();
		Check(FMath::IsNearlyEqual(L->GameSpeed, 0.75f, 0.001f), FString::Printf(TEXT("a saved 200 %% (1.0) loads as %.0f %%"), L->GameSpeed * 200.f));
		Check(L->Pace() <= AOutlawsLevelActor::MaxPace() + 1e-4f, FString::Printf(TEXT("its pace %.4f is within the top pace %.4f"), L->Pace(), AOutlawsLevelActor::MaxPace()));
		// the in-memory dial set past the top (a test's setter) still gives no more than the top pace
		L->GameSpeed = 1.f;
		Check(L->Pace() <= AOutlawsLevelActor::MaxPace() + 1e-4f, FString::Printf(TEXT("a dial set to 1.0 gives pace %.4f, no more than %.4f"), L->Pace(), AOutlawsLevelActor::MaxPace()));
		L->GameSpeed = 0.5f;
		UE_LOG(LogOutlaws, Display, TEXT("OLSPEEDCAP RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 4.f)
	{
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
