// BUG-TRAINSKY8BIT (user 2026-10-28, snapshot 3: the train with Remastered Graphics off, a pale gradient over a bright blue band):
// -OLShots -OLLevel=train -OLNoFrontEnd -OLTrainSky8BitProbe [-featureleveles31].  The level's gradient dome; with Remastered off
// one flat blue, the mean of the top row of the 8-bit picture on the outer walls (read here again from its PCX), with it on the
// gradient as before; the look switched live both ways; shots from the snapshot's place, trainsky_<8bit|hd>[_es31].png in
// Saved\OLShots.  Lines "OLSHOT trainsky check PASS|FAIL: ..." and "OLSHOT trainsky RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsLevel.h"
#include "OutlawsLab.h"
#include "OutlawsFormats.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLTrainSky8BitProbe) { return OLFlags::Param(TEXT("OLTrainSky8BitProbe")); }

OL_SHOT_TEST(OLTrainSky8BitProbe, 1243)
{
	static int32 Passes = 0, Fails = 0, Shot = 0;
	static float Wait = 0.f;
	static bool bStartHd = false;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT trainsky check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	const bool bEs31 = GetWorld() && GetWorld()->FeatureLevel == ERHIFeatureLevel::ES3_1;
	// the snapshot's place: the player at (3699, 7508, 170), looking at yaw 67, pitch 1
	auto Place = [this]()
	{
		TeleportTo(FVector(3698.7, 7507.7, 169.9), FRotator(0.0, 67.4, 0.0), false, true);
		if (Controller) Controller->SetControlRotation(FRotator(0.6, 67.4, 0.0));
	};
	// the outer walls' picture (the one the dome names, "flat r g b (<picture>)") read again here: its top row's mean
	auto TopRow = [L](FString& OutStem, FColor& OutTop)
	{
		const FOutlawsAssets* A = L->GetAssets();
		FString Look = L->DebugSkyLook(), Rest;
		if (!A || !Look.Split(TEXT("("), nullptr, &Rest)) return false;
		OutStem = Rest.LeftChop(1);
		TArray<uint8> Bytes;
		OutlawsFormats::FPcxImage Img;
		if (OutStem.IsEmpty() || !A->Get(OutStem, Bytes) || !OutlawsFormats::DecodePcx(Bytes, Img) || Img.Palette.Num() != 256 || Img.W <= 0) return false;
		double R = 0.0, G = 0.0, B = 0.0;
		for (int32 x = 0; x < Img.W; ++x) { const FColor& C = Img.Palette[Img.Indices[x]]; R += C.R; G += C.G; B += C.B; }
		OutTop = FColor(uint8(FMath::RoundToInt(R / Img.W)), uint8(FMath::RoundToInt(G / Img.W)), uint8(FMath::RoundToInt(B / Img.W)), 255);
		return true;
	};
	static FString Stem;
	static FColor Top;
	auto LookCheck = [&](const TCHAR* When)
	{
		if (Stem.IsEmpty() && !L->bHDTextures) TopRow(Stem, Top);       // (read in the 8-bit look, where the dome names its picture)
		const bool bRead = L->bHDTextures || !Stem.IsEmpty();
		const FString Look = L->DebugSkyLook();
		const FString Want = L->bHDTextures ? FString(TEXT("gradient")) : FString::Printf(TEXT("flat %d %d %d"), Top.R, Top.G, Top.B);
		Check(L->HasGradientSky() && bRead && Look.StartsWith(Want), FString::Printf(TEXT("%s, Remastered %s: the dome shows \"%s\" (wanted \"%s\"; the outer walls' %s, its top row's mean)"), When,
			L->bHDTextures ? TEXT("on") : TEXT("off"), *Look, *Want, *Stem));
	};
	switch (ShotStep)
	{
	case 0:
		if (ShotTime < 4.f || !bUp) return;
		L->bInvincible = true;
		bStartHd = L->bHDTextures;
		LookCheck(TEXT("(1) as built"));
		Place();
		Wait = ShotTime + 2.f;
		ShotStep = 1;
		return;
	case 1:
	{
		if (ShotTime < Wait) return;
		Place();
		const FString Name = FString::Printf(TEXT("trainsky_%s%s"), L->bHDTextures ? TEXT("hd") : TEXT("8bit"), bEs31 ? TEXT("_es31") : TEXT(""));
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT trainsky shot %s.png"), *Name);
		++Shot;
		Wait = ShotTime + 1.f;
		ShotStep = 2;
		return;
	}
	case 2:
		if (ShotTime < Wait) return;
		if (Shot < 3)
		{
			// the look switched live (the Graphics page's row), then back
			L->SetHDTextures(!L->bHDTextures);
			LookCheck(Shot == 1 ? TEXT("(2) switched") : TEXT("(3) switched back"));
			Wait = ShotTime + 2.f;
			ShotStep = Shot == 1 ? 1 : 3;
			if (Shot == 2) ++Shot;
			return;
		}
		ShotStep = 3;
		return;
	case 3:
		if (ShotTime < Wait) return;
		Check(L->bHDTextures == bStartHd, TEXT("(4) the look back as it started"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT trainsky RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
