// REQ-NATIVEMAPS part 1's third probe (the MAPQUICK shot of Zeke's Cabins showed a large black and grey slab standing in the
// sky behind the spawn):
//   -OLShots -OLNativeMaps=<dir> -OLMpHost=drygulch -OLMpClass=dm -OLMpBots=0 -OLMpStartWhen=0 -OLZekesSlabProbe
// The slab: zekes' tree walls (zk_tree.pcx, solid walls) are trees under a cyan top, palette index 0, which an opaque wall
// draws in its palette colour; the mips had every all-clear block black, so a far tree wall's top stood black against the sky.
// The launched host hosts zekes in Capture The Flag (as MAPQUICK did).  Checks: zk_tree's PCX look keeps index 0's colour on
// every mip; at the spawn, looking round in eight directions (a shot each, Saved/OLShots/ol_zekes_<n>.png), the top 45% of
// each view (the sky over the trees) has under 1% near-black pixels.  Rays up into each view's sky part list the surfaces
// hit higher than the trees (8 m over the eye) by their texture.  -OLOldClearMips fails both.  "OLSHOT zekes RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsLevel.h"
#include "OutlawsGeometry.h"
#include "OutlawsCoop.h"
#include "OutlawsMpHost.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "OutlawsTextures.h"
#include "ProceduralMeshComponent.h"
#include "ImageUtils.h"
#include "ImageCore.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsZekesSlabProbeLocal
{
	int32 Stage = 0, Dir = 0, Passes = 0, Fails = 0;
	double StageAt = 0.0;
	float Yaw0 = 0.f;
	TMap<FString, int32> HighHits;          // "<material key>" -> rays
	int32 DarkViews = 0;
	double WorstDark = 0.0;
	FDateTime ShotAsked;

	FString ShotPath() { return FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("ol_zekes_%d.png"), Dir); }

	void ZekesCheck(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT zekes check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}
}

OL_SHOT_MATCH(OLZekesSlabProbe) { return OLFlags::Param(TEXT("OLZekesSlabProbe")); }

OL_SHOT_TEST(OLZekesSlabProbe, 1770)
{
	using namespace OutlawsZekesSlabProbeLocal;
	if (!L || ShotTime < 0.f || Stage < 0) return;
	const double Now = FPlatformTime::Seconds();
	L->StartSaveDelay = -1.f;
	L->AutosaveWait = 0.f;
	UOutlawsCoop* C = UOutlawsCoop::Get(this);
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT zekes RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotTime = -1.f;
		Stage = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	if (Stage == 0)
	{
		const FOutlawsAssets* A = L->GetLabAssets();
		if (!A || !C || !C->IsHost() || !C->IsMatch() || !C->bLevelBuilt || ShotTime < 3.f)
		{
			if (ShotTime < 90.f) return;
			ZekesCheck(false, TEXT("needs a launched host"));
			Finish();
			return;
		}
		TArray<FOutlawsMpMap> Maps;
		OutlawsMpHost::ListMaps(A, Maps);
		const FOutlawsMpMap* Z = Maps.FindByPredicate([](const FOutlawsMpMap& M) { return M.bNative && M.Level == TEXT("zekes"); });
		if (!Z) { ZekesCheck(false, TEXT("zekes is not listed")); Finish(); return; }
		FOutlawsMpHostOptions O = C->MatchOptions;
		O.bRandomMap = O.bRandomClass = false;
		O.bBots = false;
		O.MapFile = Z->File;
		O.GameClass = EOutlawsGameClass::CaptureTheFlag;
		if (!C->StartNextMatch(O, TEXT("the zekes slab probe"))) { ZekesCheck(false, TEXT("StartNextMatch refused")); Finish(); return; }
		Stage = 1;
		StageAt = Now;
		return;
	}
	if (!C) return;
	AOutlawsCoopSession* S = C->GetSession();
	if (Stage == 1)
	{
		const bool bBuilt = C->bLevelBuilt && C->BuiltLevel.Equals(TEXT("zekes"), ESearchCase::IgnoreCase) && L->LevelName.Equals(TEXT("zekes"), ESearchCase::IgnoreCase)
			&& !L->IsRebuildPending() && C->bMatchSetUp && S && S->bHostReady && !bMenuOpen;
		if (!bBuilt)
		{
			if (Now - StageAt < 120.0) return;
			ZekesCheck(false, FString::Printf(TEXT("zekes not ready in 120 s (report: %s)"), *L->GetBuildReport().Left(200)));
			Finish();
			return;
		}
		// zk_tree's PCX look as the level makes it (the PCX has its own palette; the grey one is only the fallback): every clear
		// texel of every mip keeps the colour the first level's clear texels have (index 0's, cyan)
		{
			const FOutlawsAssets* A = L->GetLabAssets();
			TArray<FColor> Grey;
			for (int32 i = 0; i < 256; ++i) Grey.Add(FColor(i, i, i, 255));
			FOutlawsTexInfo Info;
			OutlawsTextures::FPreparedTex Lo, Hd;
			if (A) OutlawsTextures::PrepareLevelTexture(*A, TEXT("zk_tree.pcx"), true, false, Grey, false, Info, Lo, Hd);
			FColor Want = FColor::Black;
			if (Lo.Mips.Num() > 0)
			{
				const FColor* Px0 = reinterpret_cast<const FColor*>(Lo.Mips[0].GetData());
				for (int32 i = 0; i < Lo.Mips[0].Num() / 4; ++i) if (Px0[i].A == 0) { Want = Px0[i]; break; }
			}
			int32 Clear = 0, Off = 0;
			for (int32 m = 1; m < Lo.Mips.Num(); ++m)
			{
				const FColor* Px = reinterpret_cast<const FColor*>(Lo.Mips[m].GetData());
				for (int32 i = 0; i < Lo.Mips[m].Num() / 4; ++i)
				{
					if (Px[i].A > 0) continue;
					++Clear;
					if (FMath::Abs(Px[i].R - Want.R) + FMath::Abs(Px[i].G - Want.G) + FMath::Abs(Px[i].B - Want.B) > 6) ++Off;
				}
			}
			// (a deep mip's texel can round to alpha 0 from a little tree: it keeps the tree's colour, so a 0.1% allowance)
			ZekesCheck(Lo.Mips.Num() > 1 && Clear > 0 && Off * 1000 <= Clear, FString::Printf(TEXT("zk_tree's %d mips under the first: %d clear texels, %d not index 0's colour %s (want under 0.1%%)"),
				FMath::Max(0, Lo.Mips.Num() - 1), Clear, Off, *Want.ToString()));
		}
		Yaw0 = GetController() ? float(GetController()->GetControlRotation().Yaw) : 0.f;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT zekes the host at %s (units x %.1f z %.1f), yaw %.0f"), *GetActorLocation().ToCompactString(), GetActorLocation().X / 30.0, -GetActorLocation().Y / 30.0, Yaw0);
		Dir = 0;
		Stage = 2;
		StageAt = Now;
		return;
	}
	if (Stage == 2)
	{
		AController* Ctl = GetController();
		const float Yaw = Yaw0 + 45.f * Dir;
		if (Ctl) Ctl->SetControlRotation(FRotator(0.0, Yaw, 0.0));
		if (Now - StageAt < 0.8) return;
		// rays up into this view's sky part
		UWorld* W = GetWorld();
		const FVector Eye = GetPawnViewLocation();
		for (int32 p = 0; p < 4 && W; ++p)
		{
			for (int32 y = -4; y <= 4; ++y)
			{
				const FRotator R(5.0 + 8.0 * p, Yaw + 8.0 * y, 0.0);
				FHitResult Hit;
				FCollisionQueryParams Q(TEXT("OlZekesSlab"), true, this);
				Q.bReturnFaceIndex = true;
				if (!W->LineTraceSingleByChannel(Hit, Eye, Eye + R.Vector() * 20000.0, ECC_Visibility, Q)) continue;
				if (Hit.ImpactPoint.Z < Eye.Z + 800.0) continue;
				FString What = Hit.GetComponent() ? Hit.GetComponent()->GetName() : TEXT("?");
				if (UProceduralMeshComponent* M = Cast<UProceduralMeshComponent>(Hit.GetComponent()))
				{
					int32 Sec = -1;
					UMaterialInterface* Mat = Hit.FaceIndex >= 0 ? M->GetMaterialFromCollisionFaceIndex(Hit.FaceIndex, Sec) : nullptr;
					What += TEXT("/") + (Mat ? L->DebugMaterialKey(Mat) : FString(TEXT("?")));
				}
				else if (Hit.GetActor())
				{
					What += TEXT(" of ") + Hit.GetActor()->GetName();
				}
				HighHits.FindOrAdd(What) += 1;
			}
		}
		ShotAsked = FDateTime::UtcNow() - FTimespan::FromSeconds(1.0);
		FScreenshotRequest::RequestScreenshot(ShotPath(), false, false);
		Stage = 3;
		StageAt = Now;
		return;
	}
	if (Stage == 3)
	{
		if (Now - StageAt < 1.2) return;
		// the shot's top 45% (the sky over the trees): its near-black pixels (the slab was black, the sky and the cyan tree tops are not)
		const FDateTime At = IFileManager::Get().GetTimeStamp(*ShotPath());
		if (At < ShotAsked && Now - StageAt < 6.0) return;
		TArray<uint8> Bytes;
		FImage Img;
		double Dark = -1.0;
		if (At >= ShotAsked && FFileHelper::LoadFileToArray(Bytes, *ShotPath()) && FImageUtils::DecompressImage(Bytes.GetData(), Bytes.Num(), Img))
		{
			Img.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
			const TArrayView64<FColor> Px = Img.AsBGRA8();
			const int32 Rows = Img.SizeY * 45 / 100;
			int64 N = 0;
			for (int32 y = 0; y < Rows; ++y)
			{
				for (int32 x = 0; x < Img.SizeX; ++x)
				{
					const FColor& Pc = Px[int64(y) * Img.SizeX + x];
					if (FMath::Max3(Pc.R, Pc.G, Pc.B) < 20) ++N;
				}
			}
			Dark = Rows > 0 ? 100.0 * double(N) / (double(Rows) * Img.SizeX) : -1.0;
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT zekes view %d (yaw %.0f): %.2f%% of the sky part near black"), Dir, Yaw0 + 45.f * Dir, Dark);
		if (Dark < 0.0 || Dark >= 1.0) ++DarkViews;
		WorstDark = FMath::Max(WorstDark, Dark < 0.0 ? 100.0 : Dark);
		if (++Dir < 8) { Stage = 2; StageAt = Now; return; }
		for (const TPair<FString, int32>& Kv : HighHits) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT zekes high surface %s: %d rays"), *Kv.Key, Kv.Value);
		ZekesCheck(DarkViews == 0, FString::Printf(TEXT("no black slab: %d of 8 views with 1%% or more of the sky part near black (the worst %.2f%%)"), DarkViews, WorstDark));
		Finish();
	}
}

#endif // WITH_OUTLAWS_TESTS
