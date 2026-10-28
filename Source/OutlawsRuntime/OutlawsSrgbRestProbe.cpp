// BUG-SRGBREST (28 Oct, after BUG-SCROLLSRGB): the waving cloths, the bending plants, the flickering flames and the bottle
// shards drew brighter than they were made after their first move (the update's sRGB conversion, the create's none).
// -OLShots -OLLevel=<map> -OLNoFrontEnd -OLSrgbRestProbe [-OLOldSRGBRest] [-OLHD] [-featureleveles31]: for each kind the
// level has, the player 3 m from its first model a while (the updates run near the player; a shelf bottle is burst for the
// shards), then every model of it an update reached: its vertex colours as the GPU has them against the colours it was made
// with; a shot of it, srgbrest_<map>_<kind>[_hd][_old][_es31].png in Saved\OLShots.
// Lines "OLSHOT srgbrest check PASS|FAIL: ..." and "OLSHOT srgbrest RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "ProceduralMeshComponent.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLSrgbRestProbe) { return OLFlags::Param(TEXT("OLSrgbRestProbe")); }

OL_SHOT_TEST(OLSrgbRestProbe, 1260)
{
	static int32 Passes = 0, Fails = 0, Kind = 0;
	static float Wait = 0.f;
	static FVector At = FVector::ZeroVector;
	static const TCHAR* Names[] = { TEXT("cloth"), TEXT("plant"), TEXT("flame"), TEXT("shard") };
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT srgbrest check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	const bool bOld = OLFlags::Param(TEXT("OLOldSRGBRest"));
	const bool bEs31 = GetWorld() && GetWorld()->FeatureLevel == ERHIFeatureLevel::ES3_1;
	// 3 m off the model, a little above it, looking at it
	auto Place = [this]()
	{
		const FVector From = At + FVector(-300.0, 0.0, 60.0);
		const FRotator Look = (At - From).Rotation();
		TeleportTo(From - FVector(0.0, 0.0, 60.0), FRotator(0.0, Look.Yaw, 0.0), false, true);
		if (Controller) Controller->SetControlRotation(Look);
	};
	int32 Models = 0, Moved = 0, Verts = 0, AsBuild = 0, AsOld = 0, Lit = 0;
	switch (ShotStep)
	{
	case 0:
		if (ShotTime < 4.f || !bUp) return;
		L->bInvincible = true;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT srgbrest %s, Remastered %s%s%s"), *L->LevelName, L->bHDTextures ? TEXT("on") : TEXT("off"), bOld ? TEXT(", -OLOldSRGBRest") : TEXT(""),
			bEs31 ? TEXT(", ES3_1") : TEXT(""));
		Kind = 0;
		ShotStep = 1;
		return;
	case 1:
	{
		// the next kind the level has: the player beside its first model
		if (Kind > 3)
		{
			ShotStep = 4;
			return;
		}
		if (Kind == 3)
		{
			// a shelf bottle burst where it stands: its glass is the shards
			TArray<AOutlawsSpriteActor*> Props;
			L->GrabbableProps(Props);
			AOutlawsSpriteActor* Bottle = nullptr;
			for (AOutlawsSpriteActor* S : Props) if (S && S->ModelStem.StartsWith(TEXT("BOTTLE")) && Cast<UProceduralMeshComponent>(S->Model)) { Bottle = S; break; }
			if (!Bottle)
			{
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT srgbrest %s: no shelf bottle in %s"), Names[Kind], *L->LevelName);
				++Kind;
				return;
			}
			At = Bottle->Model->Bounds.Origin;
			Place();
			L->ShatterGlass(Cast<UProceduralMeshComponent>(Bottle->Model), FVector(0.0, 0.0, 1.0), FString());
			Bottle->SetActorHiddenInGame(true);
			Wait = ShotTime + 0.5f;       // (the shards shrink away: checked while they fly)
			ShotStep = 2;
			return;
		}
		L->DebugRestColours(Kind, Models, Moved, Verts, AsBuild, AsOld, Lit, At);
		if (Models == 0)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT srgbrest %s: none in %s"), Names[Kind], *L->LevelName);
			++Kind;
			return;
		}
		Place();
		Wait = ShotTime + 2.5f;           // (each moves several times)
		ShotStep = 2;
		return;
	}
	case 2:
	{
		if (ShotTime < Wait) return;
		L->DebugRestColours(Kind, Models, Moved, Verts, AsBuild, AsOld, Lit, At);
		if (bOld)
			Check(Moved > 0 && Verts > 0 && AsOld == Verts && (Lit == 0 || AsBuild < Verts), FString::Printf(TEXT("%s %s (-OLOldSRGBRest): %d models, %d moved; of %d vertices %d brighter than made (sRGB-converted), %d as made, %d not black or white"),
				*L->LevelName, Names[Kind], Models, Moved, Verts, AsOld, AsBuild, Lit));
		else
			Check(Moved > 0 && Verts > 0 && AsBuild == Verts, FString::Printf(TEXT("%s %s: %d models, %d moved; of %d vertices %d as made, %d as the old sRGB conversion, %d not black or white"),
				*L->LevelName, Names[Kind], Models, Moved, Verts, AsBuild, AsOld, Lit));
		const FString Name = FString::Printf(TEXT("srgbrest_%s_%s%s%s%s"), *L->LevelName, Names[Kind], L->bHDTextures ? TEXT("_hd") : TEXT(""), bOld ? TEXT("_old") : TEXT(""),
			bEs31 ? TEXT("_es31") : TEXT(""));
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT srgbrest shot %s.png"), *Name);
		Wait = ShotTime + 1.0f;
		ShotStep = 3;
		return;
	}
	case 3:
		if (ShotTime < Wait) return;
		++Kind;
		ShotStep = 1;
		return;
	case 4:
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT srgbrest RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
