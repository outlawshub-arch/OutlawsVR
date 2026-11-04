// REQ-SKYTHRU (user 3 Nov, snapshot 2026-11-03_19-19-59, zekes: "walls around the outer regions of the level that have a teal color
// that's supposed to be transparent ... there isn't anything behind those walls, so the teal should be showing as the sky texture
// behind it"): -OLShots -OLLevel=<zekes|nuketown|...> -OLNoFrontEnd [-OLNativeMaps=<dir>] -OLSkyThruProbe [-OLOldSkyWalls]
// [-OLDeviceProfile=Quest2].  In zekes from the snapshot's spot and look, in another level before the biggest wall of the class (a
// level without one: where it starts), three shots of the same view: the walls whole (their mask off: the old teal), the walls
// hidden (the dome behind them), the walls as built (skythru_<level>_<whole|bare|new>.png in Saved\OLShots).  The texels the mask
// opens are the pixels whole and new disagree on: their teal has to be gone from the new shot, and they have to show what the bare
// shot shows there (the sky's own colours and place), the rest of the wall unchanged.  Draw calls and GPU ms of the two looks are
// printed.  -OLOldSkyWalls: no wall in the class, one shot (skythru_<level>_old.png), the teal counted.  "OLSHOT skythru RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
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
	/** A shot's pixels, read back from its PNG (the offscreen viewport reads back black). */
	bool SkyThruPixels(const FString& File, TArray<FColor>& Px, int32& W, int32& H)
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
			// before the biggest wall that a place 8 m in front of it can see
			Look = GetControlRotation();
			double Best = 0.0;
			FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsSkyThru), false, this);
			for (int32 i = 0; i < Normals.Num(); ++i)
			{
				const FVector* C = &Corners[i * 4];
				const double Area = FVector::CrossProduct(C[2] - C[0], C[3] - C[1]).Size() * 0.5;
				if (Area <= Best) continue;
				const FVector Mid = (C[0] + C[1] + C[2] + C[3]) / 4.0;
				const FVector Flat = FVector(Normals[i].X, Normals[i].Y, 0.0).GetSafeNormal();
				FHitResult Hit;
				const FVector Pt = Mid + Flat * 800.0;
				if (!W->LineTraceSingleByChannel(Hit, Pt + FVector(0.0, 0.0, 200.0), Pt - FVector(0.0, 0.0, 6000.0), ECC_Visibility, Q)) continue;
				const FVector Feet = Hit.ImpactPoint + FVector(0.0, 0.0, Half + 5.0);
				const FVector Eye = Feet + FVector(0.0, 0.0, 80.0);
				if (!L->DebugCanSee(Eye, Mid + Flat * 5.0, nullptr)) continue;
				Best = Area;
				At = Feet;
				const double TopZ = FMath::Max3(C[0].Z, C[1].Z, FMath::Max(C[2].Z, C[3].Z));
				Look = (FVector(Mid.X, Mid.Y, FMath::Lerp(Mid.Z, TopZ, 0.6)) - Eye).Rotation();
			}
		}
		TeleportTo(At, FRotator(0.0, Look.Yaw, 0.0), false, true);
		Hold();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skythru place: at %s, look %s"), *At.ToString(), *Look.ToString());
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
		Wait = ShotTime + 2.f;
		ShotStep = 5;
		return;
	case 5:
	{
		Hold();
		if (ShotTime < Wait) return;
		TArray<FColor> Whole, Bare, New, Was;
		int32 Vw = 0, Vh = 0;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skythru perf: draws %.0f, GPU %.2f ms (the walls whole: %d frames); draws %.0f, GPU %.2f ms (as built: %d frames)"), PerfWhole.Calls(), PerfWhole.Gpu(), PerfWhole.N, PerfNew.Calls(), PerfNew.Gpu(), PerfNew.N);
		if (bOld)
		{
			int32 Teal = 0;
			if (SkyThruPixels(FileOf(TEXT("old")), Was, Vw, Vh)) for (const FColor& C : Was) Teal += IsTeal(C) ? 1 : 0;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skythru teal: %d pixels of %d (-OLOldSkyWalls)"), Teal, Was.Num());
			Check(Was.Num() > 0, TEXT("the old look's shot read"));
			if (Lv == TEXT("zekes")) Check(Teal > 1000, FString::Printf(TEXT("the old look shows its teal (%d pixels, want over 1000)"), Teal));
		}
		else if (SkyThruPixels(FileOf(TEXT("whole")), Whole, Vw, Vh) && SkyThruPixels(FileOf(TEXT("bare")), Bare, Vw, Vh) && SkyThruPixels(FileOf(TEXT("new")), New, Vw, Vh)
			&& Whole.Num() == New.Num() && Bare.Num() == New.Num())
		{
			// the sky's slow turn moves the picture a hair between shots: a pixel opened is one that was teal and changed
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
				// (another map's index 0 need not be cyan, and its view is the probe's own: what changed has to be the sky)
				int32 Changed = 0;
				double Gap = 0.0;
				for (int32 i = 0; i < New.Num(); ++i)
				{
					if (ColourGap(Whole[i], New[i]) > 30) { ++Changed; Gap += ColourGap(New[i], Bare[i]) / 3.0; }
				}
				const double Mean = Changed > 0 ? Gap / Changed : 0.0;
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skythru changed: %d pixels, %.1f a channel from the bare shot"), Changed, Mean);
				Check(Mean < 12.0, FString::Printf(TEXT("what changed shows the sky the bare shot shows there (%d pixels, %.1f a channel, want under 12)"), Changed, Mean));
			}
			else if (L->TestSkyThruMids() > 0)
			{
				Check(TealWhole > 500, FString::Printf(TEXT("the view holds the class's clear texels (%d teal pixels with the mask off, want over 500)"), TealWhole));
				Check(TealNew * 50 <= FMath::Max(TealWhole, 50), FString::Printf(TEXT("the teal is gone (%d pixels left of %d, want under 2%%)"), TealNew, TealWhole));
				Check(MeanGap < 12.0, FString::Printf(TEXT("the opened texels show the sky the bare shot shows there (%.1f a channel, want under 12)"), MeanGap));
				Check(Moved * 100 <= New.Num(), FString::Printf(TEXT("the rest of the picture unchanged (%d of %d pixels moved, the hand's bob among them, want under 1%%)"), Moved, New.Num()));
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
