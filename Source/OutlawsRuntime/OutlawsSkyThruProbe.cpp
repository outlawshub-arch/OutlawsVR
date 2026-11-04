// REQ-SKYTHRU (user 3 Nov, snapshot 2026-11-03_19-19-59, zekes: "walls around the outer regions of the level that have a teal color
// that's supposed to be transparent ... there isn't anything behind those walls, so the teal should be showing as the sky texture
// behind it"): -OLShots -OLLevel=<zekes|nuketown|...> -OLNoFrontEnd [-OLNativeMaps=<dir>] -OLSkyThruProbe [-OLOldSkyWalls]
// [-OLDeviceProfile=Quest2] [-OLSkyThruWall=<n>] [-OLSkyThruPitch=<degrees>].  In zekes from the snapshot's spot and look, in another level before the biggest wall of the class (a
// level without one: where it starts), three shots of the same view: the walls whole (their mask off: the old teal), the walls
// hidden (the dome behind them), the walls as built (skythru_<level>_<whole|bare|new>.png in Saved\OLShots).  The texels the mask
// opens are the pixels whole and new disagree on: their teal has to be gone from the new shot, and they have to show what the bare
// shot shows there (the sky's own colours and place), the rest of the wall unchanged.  Draw calls and GPU ms of the two looks are
// printed.  -OLOldSkyWalls: no wall in the class, one shot (skythru_<level>_old.png), the teal counted.  "OLSHOT skythru RESULT".
// The clouds drift and the dome turns between shots, a level's INF can end it under the probe (civlwar1's biggest wall stands in its
// END_LEVEL sector): before the first shot both stop, the sky on one fixed turn every run.  A fourth shot (skythru_<level>_again.png)
// repeats the as-built look: what it and "new" disagree on is the run's own noise, printed as "OLSHOT skythru noise".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "OutlawsTextures.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
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
#include "RHI.h"
#include "DynamicRHI.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

namespace
{
	/** A shot's pixels, read back from its PNG (the offscreen viewport reads back black).  bLinear: the ES3_1 preview's shots hold
	 *  linear values (zekes's clear texel reads 0 32 43, 0 156 178 on the PC): they are put on the PC's sRGB scale, so one cyan test
	 *  and one set of limits read both. */
	bool SkyThruPixels(const FString& File, TArray<FColor>& Px, int32& W, int32& H, bool bLinear)
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
		if (bLinear)
		{
			uint8 Srgb[256];
			for (int32 i = 0; i < 256; ++i) Srgb[i] = FLinearColor(i / 255.f, 0.f, 0.f).ToFColor(true).R;
			for (FColor& C : Px) { C.R = Srgb[C.R]; C.G = Srgb[C.G]; C.B = Srgb[C.B]; }
		}
		return true;
	}

	int32 ColourGap(const FColor& A, const FColor& B)
	{
		return FMath::Abs(int32(A.R) - B.R) + FMath::Abs(int32(A.G) - B.G) + FMath::Abs(int32(A.B) - B.B);
	}

	/** The cyan of a clear texel, in any light: little red, green and blue both well over it and near each other. */
	bool IsTeal(const FColor& C)
	{
		return C.G > 40 && C.B > 40 && C.R * 2 < C.G && C.R * 2 < C.B && FMath::Abs(int32(C.G) - C.B) * 5 < FMath::Max<int32>(C.G, C.B);
	}

	struct FPerfWindow
	{
		double GpuMs = 0.0, Draws = 0.0;
		int32 N = 0;
		void Add() { GpuMs += FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0)); Draws += GNumDrawCallsRHI[0]; ++N; }
		float Gpu() const { return N > 0 ? float(GpuMs / N) : 0.f; }
		float Calls() const { return N > 0 ? float(Draws / N) : 0.f; }
	};
}

void AOutlawsLevelActor::TestSkyThruQuads(TArray<FVector>& OutCorners, TArray<FVector>& OutNormals) const
{
	TInlineComponentArray<UProceduralMeshComponent*> Meshes;
	GetComponents(Meshes);
	for (UProceduralMeshComponent* M : Meshes)
	{
		for (int32 s = 0; s < M->GetNumSections(); ++s)
		{
			FProcMeshSection* Sec = M->GetProcMeshSection(s);
			if (!Sec || !Sec->bSectionVisible || !SkyThruMids.Contains(Cast<UMaterialInstanceDynamic>(M->GetMaterial(s)))) continue;
			for (int32 v = 0; v + 3 < Sec->ProcVertexBuffer.Num(); v += 4)
			{
				for (int32 k = 0; k < 4; ++k) OutCorners.Add(Sec->ProcVertexBuffer[v + k].Position);
				OutNormals.Add(Sec->ProcVertexBuffer[v].Normal);
			}
		}
	}
}

void AOutlawsLevelActor::TestSkyThruLook(int32 Mode)
{
	if (Mode == 3)
	{
		// the probe's still world: the speeds off and every run on one turn, so two shots (of one run or of two) see the same sky;
		// no INF level event from here on
		SkySpinSpeed = 0.f;
		CloudSpinSpeed = 0.f;
		CloudWindRate = FVector2D::ZeroVector;
		CloudSpin = 1.f;
		CloudDrift = FVector2D(0.25, 0.25);
		if (SkyMesh)
		{
			FRotator Turn = SkyMesh->GetComponentRotation();
			Turn.Yaw = 0.0;
			SkyMesh->SetWorldRotation(Turn);
		}
		if (Inf) Inf->OnLevelEvent = nullptr;
		for (const TPair<FString, TObjectPtr<UMaterialInstanceDynamic>>& Kv : Materials)
		{
			if (Kv.Value && SkyThruMids.Contains(Kv.Value)) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skythru material %s"), *Kv.Key);
		}
		return;
	}
	static UTexture2D* Solid = nullptr;
	if (!Solid)
	{
		const TArray<FColor> White = { FColor::White };
		Solid = OutlawsTextures::MakeTextureBGRA(1, 1, White, false, TF_Nearest, TA_Clamp);
		if (Solid) Solid->AddToRoot();
	}
	for (const TPair<FString, TObjectPtr<UMaterialInstanceDynamic>>& Kv : Materials)
	{
		int32 Bar;
		if (!Kv.Value || !SkyThruMids.Contains(Kv.Value) || !Kv.Key.FindChar(TEXT('|'), Bar)) continue;
		const TObjectPtr<UTexture2D>* Lo = TexturesLo.Find(Kv.Key.Mid(Bar + 1));
		if (Mode == 1 && Solid) Kv.Value->SetTextureParameterValue(TEXT("MaskTex"), Solid);
		else if (Lo) Kv.Value->SetTextureParameterValue(TEXT("MaskTex"), *Lo);
	}
	TInlineComponentArray<UProceduralMeshComponent*> Meshes;
	GetComponents(Meshes);
	for (UProceduralMeshComponent* M : Meshes)
	{
		for (int32 s = 0; s < M->GetNumSections(); ++s)
		{
			if (SkyThruMids.Contains(Cast<UMaterialInstanceDynamic>(M->GetMaterial(s)))) M->SetVisibility(Mode != 2 || M == WorldMesh);
		}
	}
}

OL_SHOT_MATCH(OLSkyThruProbe) { return OLFlags::Param(TEXT("OLSkyThruProbe")); }

OL_SHOT_TEST(OLSkyThruProbe, 2160)
{
	static int32 Passes = 0, Fails = 0;
	static float Wait = 0.f, Quiet = -1.f;
	static FRotator Look = FRotator::ZeroRotator;
	static FPerfWindow PerfWhole, PerfNew;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skythru check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	UWorld* W = GetWorld();
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	const bool bOld = OLFlags::Param(TEXT("OLOldSkyWalls"));
	const FString Lv = L->LevelName.ToLower();
	auto FileOf = [&](const TCHAR* Pose) { return FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("skythru_%s_%s.png"), *Lv, Pose); };
	auto Shot = [&](const TCHAR* Pose)
	{
		FScreenshotRequest::RequestScreenshot(FileOf(Pose), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skythru shot skythru_%s_%s.png"), *Lv, Pose);
	};
	auto Hold = [&]() { if (Controller) Controller->SetControlRotation(Look); };
	switch (ShotStep)
	{
	case 0:
	{
		if (!bUp || !W || ShotTime < 6.f || !TestShadersReady(Quiet, 3.f)) return;
		L->bInvincible = true;
		TArray<FVector> Corners, Normals;
		L->TestSkyThruQuads(Corners, Normals);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skythru level %s: %d materials, %d wall quads%s"), *Lv, L->TestSkyThruMids(), Normals.Num(), bOld ? TEXT(" (-OLOldSkyWalls)") : TEXT(""));
		const double Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		FVector At = GetActorLocation();
		if (Lv == TEXT("zekes"))
		{
			// the snapshot's feet and look
			At = FVector(11982.417, 720.641, 89.150);
			Look = FRotator(24.5f, -79.9f, 0.f);
			Check(bOld || Normals.Num() > 0, FString::Printf(TEXT("zekes has walls in the class (%d quads)"), Normals.Num()));
		}
		else
		{
			// before the biggest wall that a place 8 m in front of it can see; -OLSkyThruWall=<n>: the n-th after it of the walls with
			// such a place (the last if there are fewer), for a level whose biggest wall opens nothing in view (civlwar2: 1);
			// -OLSkyThruPitch=<degrees>: the view tipped to that, as a walker sees the wall
			Look = GetControlRotation();
			struct FSpot { double Area; FVector Feet; FRotator Turn; };
			TArray<FSpot> Spots;
			FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsSkyThru), false, this);
			for (int32 i = 0; i < Normals.Num(); ++i)
			{
				const FVector* C = &Corners[i * 4];
				const double Area = FVector::CrossProduct(C[2] - C[0], C[3] - C[1]).Size() * 0.5;
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skythru quad %d: Z %.0f to %.0f, %.0f wide, at %s"), i, FMath::Min3(C[0].Z, C[1].Z, FMath::Min(C[2].Z, C[3].Z)), FMath::Max3(C[0].Z, C[1].Z, FMath::Max(C[2].Z, C[3].Z)),
					FVector::Dist2D(C[0], C[2]), *((C[0] + C[1] + C[2] + C[3]) / 4.0).ToString());
				const FVector Mid = (C[0] + C[1] + C[2] + C[3]) / 4.0;
				const FVector Flat = FVector(Normals[i].X, Normals[i].Y, 0.0).GetSafeNormal();
				FHitResult Hit;
				const FVector Pt = Mid + Flat * 800.0;
				if (!W->LineTraceSingleByChannel(Hit, Pt + FVector(0.0, 0.0, 200.0), Pt - FVector(0.0, 0.0, 6000.0), ECC_Visibility, Q)) continue;
				const FVector Feet = Hit.ImpactPoint + FVector(0.0, 0.0, Half + 5.0);
				const FVector Eye = Feet + FVector(0.0, 0.0, 80.0);
				if (!L->DebugCanSee(Eye, Mid + Flat * 5.0, nullptr)) continue;
				const double TopZ = FMath::Max3(C[0].Z, C[1].Z, FMath::Max(C[2].Z, C[3].Z));
				Spots.Add({ Area, Feet, (FVector(Mid.X, Mid.Y, FMath::Lerp(Mid.Z, TopZ, 0.6)) - Eye).Rotation() });
			}
			Spots.StableSort([](const FSpot& A, const FSpot& B) { return A.Area > B.Area; });
			int32 Nth = 0;
			OLFlags::Value(TEXT("OLSkyThruWall"), Nth);
			if (Spots.Num() > 0)
			{
				const FSpot& Spot = Spots[FMath::Clamp(Nth, 0, Spots.Num() - 1)];
				At = Spot.Feet;
				Look = Spot.Turn;
			}
			float Pitch = 0.f;
			if (OLFlags::Value(TEXT("OLSkyThruPitch"), Pitch)) Look.Pitch = Pitch;
		}
		TeleportTo(At, FRotator(0.0, Look.Yaw, 0.0), false, true);
		Hold();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skythru place: at %s, look %s"), *At.ToString(), *Look.ToString());
		L->TestSkyThruLook(3);
		L->TestSkyThruLook(1);
		Wait = ShotTime + 2.5f;
		ShotStep = 1;
		return;
	}
	case 1:
		Hold();
		if (ShotTime < Wait - 1.5f) PerfWhole.Add();
		if (ShotTime < Wait) return;
		Shot(bOld ? TEXT("old") : TEXT("whole"));
		Wait = ShotTime + 1.5f;
		ShotStep = bOld ? 5 : 2;
		return;
	case 2:
		Hold();
		if (ShotTime < Wait) return;
		L->TestSkyThruLook(2);
		Wait = ShotTime + 1.5f;
		ShotStep = 3;
		return;
	case 3:
		Hold();
		if (ShotTime < Wait) return;
		Shot(TEXT("bare"));
		Wait = ShotTime + 1.5f;
		ShotStep = 4;
		return;
	case 4:
		Hold();
		if (ShotTime < Wait) return;
		L->TestSkyThruLook(0);
		Wait = ShotTime + 2.5f;
		ShotStep = 40;
		return;
	case 40:
		Hold();
		if (ShotTime > Wait - 1.5f && ShotTime < Wait) PerfNew.Add();
		if (ShotTime < Wait) return;
		Shot(TEXT("new"));
		Wait = ShotTime + 1.5f;
		ShotStep = 41;
		return;
	case 41:
		Hold();
		if (ShotTime < Wait) return;
		Shot(TEXT("again"));
		Wait = ShotTime + 2.f;
		ShotStep = 5;
		return;
	case 5:
	{
		Hold();
		if (ShotTime < Wait) return;
		TArray<FColor> Whole, Bare, New, Again, Was;
		int32 Vw = 0, Vh = 0;
		const bool bLinear = W && W->FeatureLevel == ERHIFeatureLevel::ES3_1;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skythru perf: draws %.0f, GPU %.2f ms (the walls whole: %d frames); draws %.0f, GPU %.2f ms (as built: %d frames)"), PerfWhole.Calls(), PerfWhole.Gpu(), PerfWhole.N, PerfNew.Calls(), PerfNew.Gpu(), PerfNew.N);
		if (bOld)
		{
			int32 Teal = 0;
			if (SkyThruPixels(FileOf(TEXT("old")), Was, Vw, Vh, bLinear)) for (const FColor& C : Was) Teal += IsTeal(C) ? 1 : 0;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skythru teal: %d pixels of %d (-OLOldSkyWalls)"), Teal, Was.Num());
			Check(Was.Num() > 0, TEXT("the old look's shot read"));
			if (Lv == TEXT("zekes")) Check(Teal > 1000, FString::Printf(TEXT("the old look shows its teal (%d pixels, want over 1000)"), Teal));
		}
		else if (SkyThruPixels(FileOf(TEXT("whole")), Whole, Vw, Vh, bLinear) && SkyThruPixels(FileOf(TEXT("bare")), Bare, Vw, Vh, bLinear) && SkyThruPixels(FileOf(TEXT("new")), New, Vw, Vh, bLinear)
			&& SkyThruPixels(FileOf(TEXT("again")), Again, Vw, Vh, bLinear) && Whole.Num() == New.Num() && Bare.Num() == New.Num() && Again.Num() == New.Num())
		{
			// the run's noise: the as-built look shot twice, nothing changed between
			int32 Noise = 0;
			for (int32 i = 0; i < New.Num(); ++i) Noise += ColourGap(New[i], Again[i]) > 45 ? 1 : 0;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skythru noise: %d pixels of %d moved between two shots of the as-built look"), Noise, New.Num());
			// a pixel opened is one that was teal and changed
			int32 TealWhole = 0, TealNew = 0, Opened = 0, Same = 0, Moved = 0;
			double SkyGap = 0.0;
			for (int32 i = 0; i < New.Num(); ++i)
			{
				const bool bTealWas = IsTeal(Whole[i]);
				TealWhole += bTealWas ? 1 : 0;
				TealNew += IsTeal(New[i]) ? 1 : 0;
				if (bTealWas && ColourGap(Whole[i], New[i]) > 30) { ++Opened; SkyGap += ColourGap(New[i], Bare[i]) / 3.0; }
				else if (!bTealWas) { (ColourGap(Whole[i], New[i]) > 45 ? Moved : Same)++; }
			}
			const double MeanGap = Opened > 0 ? SkyGap / Opened : 0.0;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skythru pixels: teal %d whole, %d new; %d opened, the sky there %.1f a channel from the bare shot; %d others kept, %d changed (of %d)"), TealWhole, TealNew, Opened, MeanGap, Same, Moved, New.Num());
			if (L->TestSkyThruMids() > 0 && Lv != TEXT("zekes"))
			{
				// (another map's index 0 need not be cyan, and its view is the probe's own: what changed has to be the sky)  The mask is
				// filtered: a texel's edge is a ramp a texel wide, up to 6 pixels of these shots from 8 m, and a pixel on it is part wall
				// and part sky, so it can't match the bare shot, which has no wall (civlwar1 with the sky still: 18.7 a channel over all
				// that changed, 0.8 with 6 pixels peeled off every edge).  Clear of that rim the sky has to match, on the same limit; on
				// the rim the pixel has to have gone toward it (the worst of the levels' runs: civlwar1, 97.8%)
				TArray<uint8> Inner, Peel;
				Inner.SetNumZeroed(New.Num());
				int32 Changed = 0;
				for (int32 i = 0; i < New.Num(); ++i)
				{
					Inner[i] = ColourGap(Whole[i], New[i]) > 30 ? 1 : 0;
					Changed += Inner[i];
				}
				for (int32 Pass = 0; Pass < 6; ++Pass)
				{
					Peel = Inner;
					for (int32 y = 0; y < Vh; ++y)
					{
						for (int32 x = 0; x < Vw; ++x)
						{
							const int32 i = y * Vw + x;
							const bool bEdge = x == 0 || y == 0 || x == Vw - 1 || y == Vh - 1;
							Inner[i] = !bEdge && Peel[i] && Peel[i - 1] && Peel[i + 1] && Peel[i - Vw] && Peel[i + Vw] ? 1 : 0;
						}
					}
				}
				int32 Clear = 0, Toward = 0;
				double Gap = 0.0;
				for (int32 i = 0; i < New.Num(); ++i)
				{
					if (Inner[i]) { ++Clear; Gap += ColourGap(New[i], Bare[i]) / 3.0; }
					else if (ColourGap(Whole[i], New[i]) > 30) Toward += ColourGap(New[i], Bare[i]) < ColourGap(Whole[i], Bare[i]) ? 1 : 0;
				}
				const int32 Rim = Changed - Clear;
				const double Mean = Clear > 0 ? Gap / Clear : 0.0;
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skythru changed: %d pixels; %d clear of the rim, %.1f a channel from the bare shot; %d on it, %d of them gone toward it"), Changed, Clear, Mean, Rim, Toward);
				Check(Clear >= 100, FString::Printf(TEXT("the view holds opened pixels clear of the rim (%d, want 100)"), Clear));
				Check(Mean < 12.0, FString::Printf(TEXT("what opened shows the sky the bare shot shows there (%d pixels, %.1f a channel, want under 12)"), Clear, Mean));
				Check(Toward * 10 >= Rim * 9, FString::Printf(TEXT("the rim goes from the wall toward that sky (%d of %d, want 90%%)"), Toward, Rim));
			}
			else if (L->TestSkyThruMids() > 0)
			{
				Check(TealWhole > 500, FString::Printf(TEXT("the view holds the class's clear texels (%d teal pixels with the mask off, want over 500)"), TealWhole));
				Check(TealNew * 50 <= FMath::Max(TealWhole, 50), FString::Printf(TEXT("the teal is gone (%d pixels left of %d, want under 2%%)"), TealNew, TealWhole));
				Check(MeanGap < 12.0, FString::Printf(TEXT("the opened texels show the sky the bare shot shows there (%.1f a channel, want under 12)"), MeanGap));
				Check(Moved * 100 <= New.Num(), FString::Printf(TEXT("the rest of the picture unchanged (%d of %d pixels moved, the opened texels' rim among them, want under 1%%)"), Moved, New.Num()));
				Check(PerfNew.Calls() <= PerfWhole.Calls() + 6.f, FString::Printf(TEXT("draw calls within noise (%.0f as built, %.0f whole)"), PerfNew.Calls(), PerfWhole.Calls()));
			}
			else Check(Moved * 100 <= New.Num(), FString::Printf(TEXT("no walls in the class here: the picture unchanged (%d moved)"), Moved));
		}
		else Check(false, TEXT("a shot not read"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skythru RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	}
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
