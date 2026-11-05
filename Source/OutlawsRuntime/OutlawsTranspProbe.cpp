// BUG-TRANSPREGRESS (user 2026-11-05, snapshot 25: the mill's channel, sector 376; the same place as BUG-WATERMIDTINT's snapshot):
// -OLShots -OLLevel=mill -OLNoFrontEnd -OLHD -OLTranspProbe [-OLOldTranspOrder] [-featureleveles31].  The bars stood sharp over the
// water while the walls round them were seen through it: the see-through mids (the world mesh) and the water (a mesh of its own)
// had no order but how far each mesh's centre lies from the eye.  (1) over a grid of the water's plan, the spots that draw the
// glass after the water; (2) from the snapshot's place, a shot and the row across the bars under the water: a bar left over the
// water keeps its tint's green (green above blue), the water over it leaves no such pixel; (3) the row across the bars above the
// water: they stay lighter than the dark behind them (the water kept off them).  transp_snap[_old][_es31].png in Saved\OLShots.
// Lines "OLSHOT transp check PASS|FAIL: ..." and "OLSHOT transp RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "HAL/PlatformMisc.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

namespace
{
	/** A shot's pixels, read back from its PNG. */
	bool ShotPixels(const FString& File, TArray<FColor>& Px, int32& W, int32& H)
	{
		TArray<uint8> Bytes;
		TArray64<uint8> Raw;
		if (!FFileHelper::LoadFileToArray(Bytes, *File, FILEREAD_Silent)) return false;
		IImageWrapperModule& Mod = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> Png = Mod.CreateImageWrapper(EImageFormat::PNG);
		if (!Png.IsValid() || !Png->SetCompressed(Bytes.GetData(), Bytes.Num()) || !Png->GetRaw(ERGBFormat::BGRA, 8, Raw)) return false;
		W = int32(Png->GetWidth());
		H = int32(Png->GetHeight());
		if (W <= 0 || H <= 0 || Raw.Num() != int64(W) * H * 4) return false;
		Px.SetNumUninitialized(W * H);
		FMemory::Memcpy(Px.GetData(), Raw.GetData(), Raw.Num());
		return true;
	}
}

OL_SHOT_MATCH(OLTranspProbe) { return OLFlags::Param(TEXT("OLTranspProbe")); }

OL_SHOT_TEST(OLTranspProbe, 1270)
{
	static int32 Passes = 0, Fails = 0;
	static float Wait = 0.f;
	static FString ShotFile;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT transp check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	const bool bOld = OLFlags::Param(TEXT("OLOldTranspOrder"));
	const bool bEs31 = GetWorld() && GetWorld()->FeatureLevel == ERHIFeatureLevel::ES3_1;
	// the snapshot's place, its pitch, level and facing up the channel
	auto Place = [this]()
	{
		TeleportTo(FVector(893.77, 11323.158, 64.875), FRotator(0.0, 1.88, 0.0), false, true);
		GetCharacterMovement()->DisableMovement();
		if (Controller) Controller->SetControlRotation(FRotator(4.1, 1.88, 0.0));
	};
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 5.f || !bUp) return;
		L->bInvincible = true;
		L->bEnemiesActive = false;
		int32 Spots = 0, GlassLast = 0;
		const FString Order = L->DebugGlassWaterOrder(Spots, GlassLast);
		Check(Spots > 0 && GlassLast == 0, FString::Printf(TEXT("(1) the glass after the water at %d of %d spots (wanted none): %s"), GlassLast, Spots, *Order));
		Place();
		Wait = ShotTime + 3.f;
		ShotStep = 1;
		return;
	}
	case 1:
	{
		if (ShotTime < Wait) return;
		Place();
		const FString Name = FString::Printf(TEXT("transp_snap%s%s"), bOld ? TEXT("_old") : TEXT(""), bEs31 ? TEXT("_es31") : TEXT(""));
		ShotFile = FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png");
		FScreenshotRequest::RequestScreenshot(ShotFile, false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT transp shot %s.png"), *Name);
		Wait = ShotTime + 2.f;
		ShotStep = 2;
		return;
	}
	case 2:
	{
		if (ShotTime < Wait) return;
		// (read back from the shot's file: the offscreen viewport reads back black)
		TArray<FColor> Px;
		int32 W = 0, H = 0;
		if (!ShotPixels(ShotFile, Px, W, H)) { Check(false, FString::Printf(TEXT("(2) %s could not be read"), *ShotFile)); ShotStep = 3; return; }
		// the bars span 530 to 730 of 1280 and the water line lies at 420 of 720; the rows in the picture's own proportion
		const int32 X0 = W * 530 / 1280, X1 = W * 730 / 1280, Under = H * 490 / 720, Over = H * 390 / 720;
		int32 Green = 0, MostGreen = 0, Lightest = 0, Darkest = 255;
		for (int32 X = X0; X < X1; ++X)
		{
			const FColor& U = Px[Under * W + X];
			const int32 D = int32(U.G) - int32(U.B);
			MostGreen = FMath::Max(MostGreen, D);
			Green += D > 0 ? 1 : 0;
			const FColor& A = Px[Over * W + X];
			Lightest = FMath::Max<int32>(Lightest, A.G);
			Darkest = FMath::Min<int32>(Darkest, A.G);
		}
		Check(Green == 0, FString::Printf(TEXT("(2) under the water, row %d: %d of %d pixels green above blue, by up to %d (wanted none: a bar left over the water)"), Under, Green, X1 - X0, MostGreen));
		Check(Lightest >= Darkest + (bEs31 ? 3 : 20), FString::Printf(TEXT("(3) above the water, row %d: bars %d against the dark behind %d"), Over, Lightest, Darkest));
		Wait = ShotTime + 0.5f;
		ShotStep = 3;
		return;
	}
	case 3:
		if (ShotTime < Wait) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT transp RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
