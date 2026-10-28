// REQ-FIREBRIGHT2 (user 2026-10-28: the campfire's logs as bright as its flame, the HD flame's soft halo a flat light red):
// -OLShots -OLLevel=shsimms -OLNoFrontEnd -OLFireBright2Probe [-OLOldFireBright2].  Every sprite of the level: the art drawn
// full bright on M_OLR_SpriteFire with its atlas's mask and Brightness by its sector's light (the mask keeps the flame's
// pixels bright), every other on M_OL_Sprite as before (-OLOldFireBright2: the fires on M_OL_Sprite at Brightness 1); the
// campfire's mask (its share of pixels that dim); the cost (the sprites on each material, each one draw as before, and the
// two materials' shaders at this feature level); then the campfire from 3 m in the 8-bit look and the HD one,
// firebright2_after_8bit.png and firebright2_after_hd.png (-OLOldFireBright2: firebright2_before_*) in Saved\OLShots.
// Lines "OLSHOT firebright2 check PASS|FAIL: ..." and "OLSHOT firebright2 RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsTextures.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Controller.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MaterialShared.h"
#include "LocalVertexFactory.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLFireBright2Probe) { return OLFlags::Param(TEXT("OLFireBright2Probe")); }

OL_SHOT_TEST(OLFireBright2Probe, 1250)
{
	static int32 Passes = 0, Fails = 0;
	static float Wait = 0.f;
	static TWeakObjectPtr<AOutlawsSpriteActor> Fire;
	if (!L || ShotStep < 0) return;
	const bool bOld = AOutlawsLevelActor::OldFireBright2();
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT firebright2 check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	auto Mid = [](const AOutlawsSpriteActor* S) { return S && S->Quad ? Cast<UMaterialInstanceDynamic>(S->Quad->GetMaterial(0)) : nullptr; };
	auto Param = [](const UMaterialInstanceDynamic* M, const TCHAR* Name)
	{
		float B = -1.f;
		if (M) M->GetScalarParameterValue(FHashedMaterialParameterInfo(Name), B);
		return B;
	};
	auto MaskOf = [](const UMaterialInstanceDynamic* M)
	{
		UTexture* T = nullptr;
		if (M) M->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Mask")), T);
		return T;
	};
	auto BaseName = [](const UMaterialInstanceDynamic* M) { return M && M->GetMaterial() ? M->GetMaterial()->GetName() : FString(TEXT("none")); };
	auto Shot = [&](const TCHAR* Look)
	{
		if (Controller) Controller->SetControlRotation(FRotator(-12.0, 72.0, 0.0));
		const FString Name = FString::Printf(TEXT("firebright2_%s_%s"), bOld ? TEXT("before") : TEXT("after"), Look);
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		const UMaterialInstanceDynamic* M = Mid(Fire.Get());
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT firebright2 (4) shot %s.png (HD look %d): the campfire on %s, brightness %.3f, mask %s"), *Name, L->bHDTextures ? 1 : 0,
			*BaseName(M), Param(M, TEXT("Brightness")), MaskOf(M) ? *MaskOf(M)->GetName() : TEXT("none"));
	};
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 4.f || !bUp) return;
		L->bInvincible = true;
		if (L->bHDTextures) L->SetHDTextures(false);       // (the 8-bit look first)
		// (1) every sprite: the full-bright art per pixel, the rest as before
		int32 Fires = 0, Others = 0, BadFires = 0, BadOthers = 0;
		FString FireList, BadList;
		for (TActorIterator<AOutlawsSpriteActor> It(GetWorld()); It; ++It)
		{
			AOutlawsSpriteActor* S = *It;
			const UMaterialInstanceDynamic* M = Mid(S);
			if (!M || S->OutlawsName.IsEmpty()) continue;
			const float B = Param(M, TEXT("Brightness")), Lit = FMath::Pow(FMath::Clamp(S->LightLevel, 0, 31) / 31.f, 2.2f);
			const FString Base = BaseName(M), Nm = S->OutlawsName.ToUpper();
			if (S->bFullBright)
			{
				++Fires;
				const bool bOk = bOld ? (Base == TEXT("M_OL_Sprite") && FMath::IsNearlyEqual(B, 1.f, 0.001f))
					: (Base == TEXT("M_OLR_SpriteFire") && S->bMaskLook && MaskOf(M) == S->FireMask.Get() && S->FireMask && FMath::IsNearlyEqual(B, Lit, 0.001f));
				if (!bOk) { ++BadFires; BadList += FString::Printf(TEXT(" %s(%s, light %d, %.3f)"), *Nm, *Base, S->LightLevel, B); }
				if (FireList.Len() < 300) FireList += FString::Printf(TEXT(" %s(light %d, brightness %.3f)"), *Nm, S->LightLevel, B);
				if (!Fire.IsValid() && Nm.StartsWith(TEXT("CAMFIRE"))) Fire = S;
			}
			else if (Base == TEXT("M_OL_Sprite"))
			{
				++Others;
				if (S->bMaskLook || !FMath::IsNearlyEqual(B, Lit, 0.001f)) { ++BadOthers; if (BadList.Len() < 400) BadList += FString::Printf(TEXT(" %s(light %d, %.3f)"), *Nm, S->LightLevel, B); }
			}
		}
		Check(Fires > 0 && BadFires == 0, FString::Printf(TEXT("(1) %d full-bright sprites in %s, each %s:%s"), Fires, *L->LevelName,
			bOld ? TEXT("on M_OL_Sprite at 1 (-OLOldFireBright2)") : TEXT("on M_OLR_SpriteFire with its mask, Brightness by its light"), *FireList));
		Check(Others > 0 && BadOthers == 0, FString::Printf(TEXT("(1) %d other sprites on M_OL_Sprite by their light; %d not:%s"), Others, BadOthers, BadList.IsEmpty() ? TEXT(" none") : *BadList));
		// (2) the campfire's mask: its logs dim, its flame not
		if (const TSharedPtr<OutlawsTextures::FSpriteAtlas> At = L->LoadSpriteSet(TEXT("camfire1.nwx")))
		{
			int32 Drawn = 0, Dim = 0;
			for (int32 i = 0; i < At->Indices.Num() && i < At->Alpha.Num(); ++i)
			{
				if (!At->Alpha[i]) continue;
				++Drawn;
				const uint8 V = At->Indices[i];
				if (!L->FireKeepsBright(V)) ++Dim;
			}
			const float Share = Drawn ? float(Dim) / Drawn : 0.f;
			Check(Drawn > 0 && Share > 0.05f && Share < 0.6f, FString::Printf(TEXT("(2) camfire1's art: %d of %d drawn px (%.1f %%) in colours the light dims (the logs), the rest kept bright"), Dim, Drawn, Share * 100.f));
		}
		// (3) the cost: the sprites on each material (one draw each, as before) and the two materials' shaders
		{
			int32 OnFire = 0, OnSprite = 0;
			for (TActorIterator<AOutlawsSpriteActor> It(GetWorld()); It; ++It)
			{
				const FString Base = BaseName(Mid(*It));
				OnFire += Base == TEXT("M_OLR_SpriteFire");
				OnSprite += Base == TEXT("M_OL_Sprite");
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT firebright2 (3) cost: %d sprites on M_OLR_SpriteFire, %d on M_OL_Sprite (one draw each either way)"), OnFire, OnSprite);
			const ERHIFeatureLevel::Type Fl = GetWorld()->FeatureLevel;
			for (const TCHAR* P : { TEXT("/Game/Outlaws/Core/M_OL_Sprite.M_OL_Sprite"), TEXT("/Game/Outlaws/Runtime/M_OLR_SpriteFire.M_OLR_SpriteFire") })
			{
				const UMaterial* Mt = LoadObject<UMaterial>(nullptr, P);
				const FMaterialResource* R = Mt ? Mt->GetMaterialResource(Fl) : nullptr;
				const FMaterialShaderMap* Map = R ? R->GetGameThreadShaderMap() : nullptr;
				int32 NumShaders = 0, NumPipelines = 0;
				uint32 MaxPs = 0, Vs = 0, Ps = 0;
				if (Map)
				{
					Map->CountNumShaders(NumShaders, NumPipelines);
					if (const FMeshMaterialShaderMap* MeshMap = Map->GetMeshShaderMap(&FLocalVertexFactory::StaticType))
					{
						for (const TMemoryImagePtr<FShader>& Sh : MeshMap->GetShaders())
						{
							if (Sh.IsValid() && Sh->GetFrequency() == SF_Pixel) MaxPs = FMath::Max(MaxPs, Sh->GetNumInstructions());
						}
					}
#if WITH_EDITOR
					R->GetEstimatedNumTextureSamples(Vs, Ps);       // (an editor build's only)
#endif
				}
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT firebright2 (3) cost: %s at feature level %d: %d shaders, the static mesh's pixel shaders up to %u instructions, %u texture samples"),
					Mt ? *Mt->GetName() : P, int32(Fl), NumShaders, MaxPs, Ps);
			}
		}
		// (4) the campfire from 3 m, the eyes level with its middle
		if (AOutlawsSpriteActor* F = Fire.Get())
		{
			const FVector At = F->GetActorLocation();
			const FVector Dir = FRotator(0.0, 72.0, 0.0).Vector();       // (REQ-FIREBRIGHT's look, the snapshot's yaw 72)
			TeleportTo(At - Dir * 300.0 + FVector(0.0, 0.0, 90.0), FRotator(0.0, 72.0, 0.0), false, true);
			if (Controller) Controller->SetControlRotation(FRotator(-12.0, 72.0, 0.0));
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT firebright2 (4) the campfire %s at %s (light %d)"), *F->OutlawsName, *At.ToString(), F->LightLevel);
		}
		else
		{
			Check(false, TEXT("(4) no campfire in this level for the shots"));
		}
		Wait = ShotTime + 2.f;
		ShotStep = 1;
		return;
	}
	case 1:
		if (ShotTime < Wait) return;
		Shot(TEXT("8bit"));
		Wait = ShotTime + 1.f;
		ShotStep = 2;
		return;
	case 2:
		if (ShotTime < Wait) return;
		L->SetHDTextures(true);
		Wait = ShotTime + 2.f;
		ShotStep = 3;
		return;
	case 3:
	{
		if (ShotTime < Wait) return;
		const AOutlawsSpriteActor* F = Fire.Get();
		const UMaterialInstanceDynamic* M = Mid(F);
		if (F && !bOld)
		{
			const bool bHdArt = F->SpriteTextureLo && F->SpriteTexture != F->SpriteTextureLo;
			Check(bHdArt && MaskOf(M) == F->FireMaskHd.Get() && F->FireMaskHd, FString::Printf(TEXT("(4) the HD look: the campfire's HD art %d, its mask the filtered one (%s)"),
				bHdArt ? 1 : 0, MaskOf(M) ? *MaskOf(M)->GetName() : TEXT("none")));
		}
		Shot(TEXT("hd"));
		Wait = ShotTime + 1.5f;
		ShotStep = 4;
		return;
	}
	case 4:
		if (ShotTime < Wait) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT firebright2 RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	}
}

#endif
