// REQ-FIREBRIGHT (user 2026-10-28, snapshot 17: the shsimms campfire at night, its flame's top dark red):
// -OLShots -OLLevel=shsimms -OLNoFrontEnd -OLFireBrightProbe [-OLOldFireBright].  The level's colour map's full-bright entries;
// the share of full-bright colours in the art of the fires, the explosions and some others (lamps, a cow, characters, a barrel)
// and which are drawn full bright; every sprite of the level: the fires full bright at Brightness 1 whatever their sector's
// light, every other by its light as before; then a shot of the campfire from 3 m, firebright_after.png (-OLOldFireBright:
// firebright_before.png) in Saved\OLShots.  Lines "OLSHOT firebright check PASS|FAIL: ..." and "OLSHOT firebright RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsTextures.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Controller.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLFireBrightProbe) { return OLFlags::Param(TEXT("OLFireBrightProbe")); }

OL_SHOT_TEST(OLFireBrightProbe, 17)
{
	static int32 Passes = 0, Fails = 0;
	static float Wait = 0.f;
	static TWeakObjectPtr<AOutlawsSpriteActor> Fire;
	if (!L || ShotStep < 0) return;
	const bool bOld = AOutlawsLevelActor::OldFireBright();
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT firebright check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	auto Bright = [](const AOutlawsSpriteActor* S)
	{
		float B = -1.f;
		const UMaterialInstanceDynamic* M = S && S->Quad ? Cast<UMaterialInstanceDynamic>(S->Quad->GetMaterial(0)) : nullptr;
		if (M) M->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("Brightness")), B);
		return B;
	};
	auto IsFireName = [](const FString& N) { return N.StartsWith(TEXT("CAMFIRE")) || N.StartsWith(TEXT("FIRE0")) || N.StartsWith(TEXT("FIREX")); };
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 4.f || !bUp) return;
		L->bInvincible = true;
		// (1) the colour map's full-bright entries
		FString Ix;
		int32 N = 0;
		for (int32 i = 0; i < L->FullBrightIx.Num(); ++i) if (L->FullBrightIx[i]) Ix += FString::Printf(TEXT("%s%d"), N++ ? TEXT(" ") : TEXT(""), i);
		const bool bRamp = L->FullBrightIx.Num() == 256 && L->FullBrightIx[16] && L->FullBrightIx[19] && L->FullBrightIx[250] && L->FullBrightIx[254] && !L->FullBrightIx[20] && !L->FullBrightIx[249];
		Check(bRamp, FString::Printf(TEXT("(1) %s's colour map keeps %d entries from the light: %s (wanted the fire ramp 16..19, 250..254 and white)"), *L->LevelName, N, *Ix));
		// (2) the art: the fires and explosions full bright, the rest not
		struct FArt { const TCHAR* Nwx; bool bFire; };
		static const FArt Arts[] = { { TEXT("camfire1.nwx"), true }, { TEXT("camfire2.nwx"), true }, { TEXT("fire01.nwx"), true }, { TEXT("fire02.nwx"), true },
			{ TEXT("tntboom.nwx"), true }, { TEXT("a51cow.nwx"), false }, { TEXT("lantern.nwx"), false }, { TEXT("simlamp.nwx"), false },
			{ TEXT("explode3.nwx"), false }, { TEXT("glantern.nwx"), false }, { TEXT("barrel01.nwx"), false }, { TEXT("sanchez.nwx"), false }, { TEXT("bgy1.nwx"), false } };
		for (const FArt& Ar : Arts)
		{
			const TSharedPtr<OutlawsTextures::FSpriteAtlas> At = L->LoadSpriteSet(Ar.Nwx);
			if (!At)
			{
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT firebright (2) %s: not in this install"), Ar.Nwx);
				continue;
			}
			float Share = 0.f;
			const bool bFull = L->IsFullBrightArt(*At, &Share);
			Check(bFull == (Ar.bFire && !bOld), FString::Printf(TEXT("(2) %s: %.1f %% of its art in full-bright colours: %s (wanted %s)"), Ar.Nwx, Share * 100.f,
				bFull ? TEXT("full bright") : TEXT("lit"), Ar.bFire && !bOld ? TEXT("full bright") : TEXT("lit")));
		}
		// (3) every sprite in the level: the fires full bright whatever their light, the others by their light as before
		int32 Fires = 0, Others = 0, BadFires = 0, BadOthers = 0;
		FString FireList, BadList;
		for (TActorIterator<AOutlawsSpriteActor> It(GetWorld()); It; ++It)
		{
			AOutlawsSpriteActor* S = *It;
			if (!S->Quad || S->OutlawsName.IsEmpty()) continue;
			const float B = Bright(S), Lit = FMath::Pow(FMath::Clamp(S->LightLevel, 0, 31) / 31.f, 2.2f);
			if (B < 0.f) continue;           // (no instance of its own yet: not drawn)
			const FString Nm = S->OutlawsName.ToUpper();
			if (IsFireName(Nm))
			{
				++Fires;
				const bool bOk = bOld ? (!S->bFullBright && FMath::IsNearlyEqual(B, Lit, 0.001f)) : (S->bFullBright && FMath::IsNearlyEqual(B, 1.f, 0.001f));
				if (!bOk) { ++BadFires; BadList += FString::Printf(TEXT(" %s(light %d, %.3f)"), *Nm, S->LightLevel, B); }
				if (FireList.Len() < 300) FireList += FString::Printf(TEXT(" %s(light %d, brightness %.3f)"), *Nm, S->LightLevel, B);
				if (!Fire.IsValid() && Nm.StartsWith(TEXT("CAMFIRE"))) Fire = S;
			}
			else
			{
				++Others;
				const bool bOk = !S->bFullBright && FMath::IsNearlyEqual(B, Lit, 0.001f);
				if (!bOk) { ++BadOthers; if (BadList.Len() < 400) BadList += FString::Printf(TEXT(" %s(full %d, light %d, %.3f)"), *Nm, S->bFullBright ? 1 : 0, S->LightLevel, B); }
			}
		}
		Check(Fires > 0 && BadFires == 0, FString::Printf(TEXT("(3) %d fires in %s, each %s:%s"), Fires, *L->LevelName, bOld ? TEXT("by its light (-OLOldFireBright)") : TEXT("full bright"), *FireList));
		Check(Others > 0 && BadOthers == 0, FString::Printf(TEXT("(3) %d other sprites, each by its sector's light; %d not:%s"), Others, BadOthers, BadList.IsEmpty() ? TEXT(" none") : *BadList));
		// (4) the campfire from 3 m, the eyes level with its middle
		if (AOutlawsSpriteActor* F = Fire.Get())
		{
			const FVector At = F->GetActorLocation();
			const FVector Dir = FRotator(0.0, 72.0, 0.0).Vector();       // (the snapshot's look, yaw 72)
			const FVector From = At - Dir * 300.0 + FVector(0.0, 0.0, 90.0);
			TeleportTo(From, FRotator(0.0, 72.0, 0.0), false, true);
			if (Controller) Controller->SetControlRotation(FRotator(-12.0, 72.0, 0.0));
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT firebright (4) the campfire %s at %s (light %d): the player at %s"), *F->OutlawsName, *At.ToString(), F->LightLevel, *From.ToString());
		}
		else
		{
			Check(false, TEXT("(4) no campfire in this level for the shot"));
		}
		Wait = ShotTime + 2.f;
		ShotStep = 1;
		return;
	}
	case 1:
	{
		if (ShotTime < Wait) return;
		if (Controller) Controller->SetControlRotation(FRotator(-12.0, 72.0, 0.0));
		const FString Name = bOld ? TEXT("firebright_before") : TEXT("firebright_after");
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT firebright (4) shot %s.png (HD look %d), the campfire's brightness %.3f"), *Name, L->bHDTextures ? 1 : 0, Bright(Fire.Get()));
		Wait = ShotTime + 1.5f;
		ShotStep = 2;
		return;
	}
	case 2:
		if (ShotTime < Wait) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT firebright RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
