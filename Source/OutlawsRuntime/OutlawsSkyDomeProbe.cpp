// REQ-SKYDOME (user 3 Nov: "the sky for armadillo, frostbite acres, and others likely need their UV map adjusted ... it needs to
// wrap around the level, a bit like the hideout one.  And the top dome needs to just be a solid color as well"):
// -OLShots -OLLevel=<armad|frostbi|isla|hideout|...> -OLNoFrontEnd [-OLNativeMaps=<dir>] -OLSkyDomeProbe [-OLOldSkyDome]
// [-OLSkyCensus].  The level's sky as the dome maps it (the old test's judgement and the one used, the land's foot row, the
// material's values); out under the open sky with the widest view, a shot at the horizon and one looking up (skydome_<level>_
// <new|old>_<horizon|up>.png in Saved\OLShots); a land sky's foot within 3 degrees under the horizon, its zenith one colour (the
// up shot's middle, low spread), no cloud layer; any other sky mapped as before.  -OLSkyCensus: every level's sky (the install's
// and the native maps'): picture, size, the old and new judgements, and (REQ-SKYTHRU) the walls whose index-0 texels would show
// the sky.  Lines "OLSHOT skydome check PASS|FAIL: ...", "OLSHOT skydome census ..." and "OLSHOT skydome RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsLevel.h"
#include "OutlawsLab.h"
#include "OutlawsFormats.h"
#include "OutlawsGeometry.h"
#include "OutlawsTextures.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/Controller.h"
#include "CollisionQueryParams.h"
#include "Engine/World.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

namespace
{
	/** Every level's sky (and the see-through outer walls) from its own files: the install's levels, then the native maps' (each
	 *  read through its own overlay, as a build does; called last, just before the run ends). */
	void SkyCensus(const FOutlawsAssets& Shown)
	{
		TSharedPtr<FOutlawsAssets> A = FOutlawsAssets::GetShared(Shown.GetRoot());
		if (!A) return;
		TArray<FString> Levels;
		A->ListByExtension(TEXT(".lvt"), Levels);
		for (FString& Lv : Levels) Lv = Lv.LeftChop(4);
		const int32 NumStock = Levels.Num();
		TArray<FString> Natives;
		A->NativeStampList().ParseIntoArray(Natives, TEXT("~"));
		for (const FString& N : Natives) { FString Lv, Stamp; if (N.Split(TEXT("."), &Lv, &Stamp, ESearchCase::IgnoreCase, ESearchDir::FromEnd)) Levels.Add(Lv); }
		static const int32 Owner = 0;
		int32 Land = 0, Changed = 0, WallLevels = 0, Walls = 0;
		for (int32 i = 0; i < Levels.Num(); ++i)
		{
			const FString& Lv = Levels[i];
			A->UseLevelOverlay(Lv, &Owner);
			FString Text;
			FOutlawsLevel L;
			if (!A->GetText(Lv + TEXT(".lvt"), Text) || !L.ParseLvt(Text))
			{
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skydome census %s (%s): not read"), *Lv, i < NumStock ? TEXT("stock") : TEXT("native"));
				continue;
			}
			// the textures' own 8-bit pictures, read once a level: index-0 texels (REQ-SKYTHRU)
			TMap<int32, bool> Clear;
			auto HasClear = [&](int32 Tex)
			{
				if (const bool* Hit = Clear.Find(Tex)) return *Hit;
				TArray<uint8> Bytes;
				OutlawsFormats::FPcxImage Img;
				const FString R = OutlawsFormats::ResolveTextureName(*A, L.Textures[Tex]);
				const bool b = !R.IsEmpty() && A->Get(R, Bytes) && OutlawsFormats::DecodePcx(Bytes, Img) && Img.Indices.Contains(0);
				return Clear.Add(Tex, b);
			};
			// REQ-SKYTHRU's class: walls with no neighbour whose mid has index-0 texels, owned by a sky sector that draws its walls
			int32 N = 0;
			TMap<FString, int32> ByTex;
			for (const FOutlawsSector& S : L.Sectors)
			{
				if (!(S.Flags & OutlawsFlags::ExtCeiling) || S.NoWalls()) continue;
				for (const FOutlawsWall& W : S.Walls)
				{
					if (W.Adjoin >= 0 || !L.Textures.IsValidIndex(W.Mid) || !HasClear(W.Mid)) continue;
					++N;
					ByTex.FindOrAdd(L.TexStem(W.Mid))++;
				}
			}
			FString WallList;
			for (const TPair<FString, int32>& Kv : ByTex) WallList += FString::Printf(TEXT(" %s x%d"), *Kv.Key, Kv.Value);
			if (N > 0) { ++WallLevels; Walls += N; }
			const FString Stem = FOutlawsGeometry::PickSkyTexture(L);
			FString SkyName;
			for (const FString& T : L.Textures) if (FPaths::GetBaseFilename(T).ToUpper() == Stem) { SkyName = T; break; }
			OutlawsTextures::FOutlawsSkyLook Look;
			TArray<uint8> Bytes;
			const FString R = SkyName.IsEmpty() ? FString() : OutlawsFormats::ResolveTextureName(*A, SkyName);
			const bool bRead = !R.IsEmpty() && A->Get(R, Bytes) && OutlawsTextures::SkyLookPcx(Bytes, TArray<FColor>(), Look);
			const bool bLand = bRead && Look.bTiles && Look.bLand;
			Land += bLand ? 1 : 0;
			Changed += bLand ? 1 : 0;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skydome census %s (%s): sky %s %dx%d, old %s, new %s%s; skythru walls %d%s"), *Lv, i < NumStock ? TEXT("stock") : TEXT("native"),
				Stem.IsEmpty() ? TEXT("none") : *Stem, Look.W, Look.H, !bRead ? TEXT("not read") : Look.bTiles ? TEXT("tiling") : TEXT("horizon"),
				!bRead ? TEXT("not read") : bLand ? TEXT("LAND") : Look.bTiles ? TEXT("tiling") : TEXT("horizon"),
				bLand ? *FString::Printf(TEXT(" (foot at row %d, %s, %.0f%% of its columns)"), Look.LandRow, Look.LandHow, 100.f * Look.LandCover) : TEXT(""), N, *WallList);
		}
		A->CloseOverlay(&Owner);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skydome census done: %d levels (%d stock, %d native); %d skies change (tiling to land); %d levels with %d skythru walls"),
			Levels.Num(), NumStock, Levels.Num() - NumStock, Changed, WallLevels, Walls);
	}

	/** A shot's pixels, read back from its PNG (the offscreen viewport reads back black). */
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

OL_SHOT_MATCH(OLSkyDomeProbe) { return OLFlags::Param(TEXT("OLSkyDomeProbe")); }

OL_SHOT_TEST(OLSkyDomeProbe, 2155)
{
	static int32 Passes = 0, Fails = 0;
	static float Wait = 0.f, Quiet = -1.f;
	static float Yaw = 0.f;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skydome check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	UWorld* W = GetWorld();
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	const bool bOld = OLFlags::Param(TEXT("OLOldSkyDome"));
	const FString Tag = FString::Printf(TEXT("skydome_%s_%s"), *L->LevelName.ToLower(), bOld ? TEXT("old") : TEXT("new"));
	const AOutlawsLevelActor::FSkyDomeInfo& D = L->GetSkyDome();
	auto Shot = [&](const TCHAR* Pose)
	{
		const FString Name = Tag + TEXT("_") + Pose;
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skydome shot %s.png"), *Name);
	};
	switch (ShotStep)
	{
	case 0:
	{
		if (!bUp || !W || ShotTime < 6.f || !TestShadersReady(Quiet, 3.f)) return;
		L->bInvincible = true;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skydome sky %s %dx%d: old %s, now %s, land row %d, Repeats %.2f, RepV %.2f, LandV %.4f, LandDrop %.4f, wrap %d, cloud layer %d, fade %.3f %.3f %.3f%s"),
			*D.Stem, D.W, D.H, *D.Old, *D.Now, D.LandRow, D.Repeats, D.RepV, D.LandV, D.LandDrop, D.bWrap ? 1 : 0, D.bCloudLayer ? 1 : 0, D.Fade.R, D.Fade.G, D.Fade.B,
			bOld ? TEXT(" (-OLOldSkyDome)") : TEXT(""));
		// out under the open sky: the sky sector whose eye-high view runs farthest (the mean of 16 traces round), facing its longest
		int32 Pick = INDEX_NONE;
		double BestMean = -1.0;
		FVector BestAt = GetActorLocation();
		float BestYaw = 0.f;
		const double Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsSkyDome), false, this);
		// (no sky sector with a floor under its point, frostbi: any sector, the widest view)
		for (int32 i = 0, Tried = 0, Pass = 0; Pass < 2 && Tried < 600; i = (i + 1 < L->TestSectorCount()) ? i + 1 : (++Pass, 0))
		{
			if (Pass == 1 && Pick != INDEX_NONE) break;
			FVector Pt;
			if ((Pass == 0 && !(L->TestSectorFlags(i) & 1u)) || !L->DebugPointInSector(i, Pt)) continue;
			++Tried;
			FHitResult Hit;
			if (!W->LineTraceSingleByChannel(Hit, Pt - FVector(0.0, 0.0, 20.0), Pt - FVector(0.0, 0.0, 6000.0), ECC_Visibility, Q)) continue;
			const FVector Eye = Hit.ImpactPoint + FVector(0.0, 0.0, 170.0);
			double Sum = 0.0, Far = -1.0;
			float FarYaw = 0.f;
			for (int32 k = 0; k < 16; ++k)
			{
				const float Yw = 22.5f * k;
				const FVector Dir = FRotator(0.0, Yw, 0.0).Vector();
				FHitResult H2;
				const double Dist = W->LineTraceSingleByChannel(H2, Eye, Eye + Dir * 200000.0, ECC_Visibility, Q) ? H2.Distance : 200000.0;
				Sum += FMath::Min(Dist, 20000.0);
				if (Dist > Far) { Far = Dist; FarYaw = Yw; }
			}
			if (Sum / 16.0 > BestMean) { BestMean = Sum / 16.0; Pick = i; BestAt = Hit.ImpactPoint + FVector(0.0, 0.0, Half + 5.0); BestYaw = FarYaw; }
		}
		TeleportTo(BestAt, FRotator(0.0, BestYaw, 0.0), false, true);
		Yaw = BestYaw;
		if (Controller) Controller->SetControlRotation(FRotator(0.0, Yaw, 0.0));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skydome place: sky sector %d, mean view %.0f cm, at %s facing %.1f"), Pick, BestMean, *BestAt.ToString(), Yaw);
		Wait = ShotTime + 2.f;
		ShotStep = 1;
		return;
	}
	case 1:
		if (Controller) Controller->SetControlRotation(FRotator(0.0, Yaw, 0.0));
		if (ShotTime < Wait) return;
		Shot(TEXT("horizon"));
		Wait = ShotTime + 1.5f;
		ShotStep = 2;
		return;
	case 2:
		if (Controller) Controller->SetControlRotation(FRotator(80.0, Yaw, 0.0));
		if (ShotTime < Wait) return;
		Wait = ShotTime + 1.5f;
		ShotStep = 3;
		return;
	case 3:
	{
		if (Controller) Controller->SetControlRotation(FRotator(80.0, Yaw, 0.0));
		if (ShotTime < Wait) return;
		Shot(TEXT("up"));
		Wait = ShotTime + 2.f;
		ShotStep = 4;
		return;
	}
	case 4:
	{
		if (Controller) Controller->SetControlRotation(FRotator(80.0, Yaw, 0.0));
		if (ShotTime < Wait) return;
		// the up shot's middle (a fifth of it each way): its colours' spread
		TArray<FColor> Px;
		int32 Vw = 0, Vh = 0;
		double Spread = -1.0;
		FVector Mean(0.0);
		if (ShotPixels(FPaths::ProjectSavedDir() / TEXT("OLShots") / Tag + TEXT("_up.png"), Px, Vw, Vh))
		{
			int32 N = 0;
			FVector Sq(0.0);
			for (int32 y = Vh * 2 / 5; y < Vh * 3 / 5; ++y)
			{
				for (int32 x = Vw * 2 / 5; x < Vw * 3 / 5; ++x)
				{
					const FColor& C = Px[y * Vw + x];
					const FVector V(C.R, C.G, C.B);
					Mean += V;
					Sq += V * V;
					++N;
				}
			}
			Mean /= FMath::Max(1, N);
			const FVector Var = Sq / FMath::Max(1, N) - Mean * Mean;
			Spread = FMath::Sqrt(FMath::Max(0.0, (Var.X + Var.Y + Var.Z) / 3.0));
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skydome up: the middle's mean %.0f %.0f %.0f, spread %.2f"), Mean.X, Mean.Y, Mean.Z, Spread);
		// the material's mapping (tl = RepV x (elevation + LandDrop) picture heights over the foot, v = LandV - tl): the foot at
		// elevation -LandDrop, the picture's top row (its top colour alone from there up) at LandV / RepV - LandDrop
		const float FootDeg = -D.LandDrop * 90.f;
		const float TopDeg = D.RepV > 0.f ? (D.LandV / D.RepV - D.LandDrop) * 90.f : 0.f;
		if (D.Now == TEXT("land"))
		{
			Check(!bOld, FString::Printf(TEXT("%s %dx%d, called %s by the old test, is mapped as a land sky (its land's foot at row %d)"), *D.Stem, D.W, D.H, *D.Old, D.LandRow));
			Check(FootDeg <= 0.f && FootDeg >= -3.f, FString::Printf(TEXT("the land's foot %.2f degrees from the horizon (want 0 to 3 under it)"), FootDeg));
			Check(!D.bWrap && !D.bCloudLayer, FString::Printf(TEXT("no second copy up the dome (wrap %d) and no cloud layer (%d)"), D.bWrap ? 1 : 0, D.bCloudLayer ? 1 : 0));
			Check(TopDeg < 60.f && Spread >= 0.0 && Spread < 4.0, FString::Printf(TEXT("the zenith one colour: the up view's middle spread %.2f (want under 4; the picture's top colour alone from %.0f degrees up)"), Spread, TopDeg));
			// (user 3 Nov ~23:4x: armad's overhead colour bluer) no land sky near white overhead
			Check(Mean.Z - Mean.X > 30.0, FString::Printf(TEXT("the overhead colour reads blue, not near white: the up view's mean %.0f %.0f %.0f (want blue over red by more than 30)"), Mean.X, Mean.Y, Mean.Z));
		}
		else
		{
			Check(D.Now == D.Old && D.LandV == 0.f && D.LandDrop == 0.f, FString::Printf(TEXT("%s %dx%d mapped as before: %s (old test %s), LandV %.3f"), *D.Stem, D.W, D.H, *D.Now, *D.Old, D.LandV));
		}
		Wait = ShotTime + 1.5f;
		ShotStep = 5;
		return;
	}
	case 5:
		if (ShotTime < Wait) return;
		if (OLFlags::Param(TEXT("OLSkyCensus")) && L->GetAssets()) SkyCensus(*L->GetAssets());
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skydome RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
