// BUG-GLASS8BIT (user 2026-10-28, snapshot 2: townex, Remastered Graphics off, the broken windows grey instead of see-through):
// -OLShots -OLLevel=townex -OLNoFrontEnd -OLGlass8BitProbe [-OLHD] [-OLOldGlass8Bit] [-featureleveles31].  The level's SHADES as
// parsed; a broken window's frame (GWWN642) as its PCX look is made: its shards (entries 2 to 4) at the SHADES' strengths, the
// old rule's solid; the whole window (GWWN641) clear in its pane; the snapshot's two windows (sectors 43 and 45, wall 3) and their
// panes in the glass material, six break frames each; then shots from the snapshot's place, whole and broken:
// glass8bit_<8bit|hd>[_old][_es31]_<whole|broken>.png in Saved\OLShots (the look picked first: Remastered off unless -OLHD, the
// level built again in it).  Lines "OLSHOT glass8bit check PASS|FAIL: ..." and "OLSHOT glass8bit RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsTextures.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLGlass8BitProbe) { return OLFlags::Param(TEXT("OLGlass8BitProbe")); }

OL_SHOT_TEST(OLGlass8BitProbe, 1242)
{
	static int32 Passes = 0, Fails = 0;
	static float Wait = 0.f;
	static bool bRebuilt = false;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT glass8bit check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	const bool bWantHd = OLFlags::Param(TEXT("OLHD"));
	const bool bOld = OLFlags::Param(TEXT("OLOldGlass8Bit"));
	const bool bEs31 = GetWorld() && GetWorld()->FeatureLevel == ERHIFeatureLevel::ES3_1;
	const FString Tag = FString::Printf(TEXT("%s%s%s"), bWantHd ? TEXT("hd") : TEXT("8bit"), bOld ? TEXT("_old") : TEXT(""), bEs31 ? TEXT("_es31") : TEXT(""));
	// the snapshot's place: the player at (14888, 3893, 96), looking at yaw 117, pitch -1
	auto Place = [this]()
	{
		TeleportTo(FVector(14887.8, 3893.1, 96.4), FRotator(0.0, 117.3, 0.0), false, true);
		if (Controller) Controller->SetControlRotation(FRotator(-1.3, 117.3, 0.0));
	};
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 4.f || !bUp) return;
		if (!bRebuilt && L->bHDTextures != bWantHd)
		{
			// the panes' break frames are loaded in the look of the level's build: the look set, the level built again
			bRebuilt = true;
			L->SetHDTextures(bWantHd);
			L->RestartLevel();
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT glass8bit Remastered Graphics %s: the level built again"), bWantHd ? TEXT("on") : TEXT("off"));
			ShotTime = 0.f;
			return;
		}
		L->bInvincible = true;
		L->bEnemiesActive = false;
		// (1) the level's SHADES
		const TArray<FColor> Sh = L->DebugShades();
		FString Said;
		for (int32 i = 1; i < Sh.Num(); ++i) if (Sh[i].A > 0) Said += FString::Printf(TEXT(" %d=(%d %d %d, %d)"), i, Sh[i].R, Sh[i].G, Sh[i].B, Sh[i].A);
		Check(Sh.Num() == 16 && Sh[2] == FColor(200, 200, 200, 64) && Sh[3] == FColor(200, 200, 200, 128) && Sh[4] == FColor(200, 200, 200, 191),
			FString::Printf(TEXT("(1) %s's SHADES:%s (wanted 2 to 4 white at 25, 50 and 75 %%: alpha 64, 128, 191)"), *L->LevelName, *Said));
		// (2) a broken frame's PCX look: its shards as the SHADES, the old rule's solid; (3) the whole window clear in its pane
		if (const FOutlawsAssets* A = L->GetAssets())
		{
			const TArray<FColor> NoPal;
			int32 W = 0, H = 0;
			TArray<FColor> New, Was, Whole;
			const bool bRead = OutlawsTextures::LevelPcxPixels(*A, TEXT("GWWN642.PCX"), NoPal, &Sh, W, H, New) && OutlawsTextures::LevelPcxPixels(*A, TEXT("GWWN642.PCX"), NoPal, nullptr, W, H, Was);
			int32 Shards = 0, ShardsRight = 0, WasSolid = 0, Clear = 0, Solid = 0;
			for (int32 i = 0; bRead && i < New.Num(); ++i)
			{
				if (New[i].A > 0 && New[i].A < 255)
				{
					++Shards;
					ShardsRight += New[i].R == 200 && New[i].G == 200 && New[i].B == 200 ? 1 : 0;
					WasSolid += Was[i].A == 255 ? 1 : 0;
				}
				Clear += New[i].A == 0 ? 1 : 0;
				Solid += New[i].A == 255 ? 1 : 0;
			}
			Check(bRead && Shards > 2000 && ShardsRight == Shards && WasSolid == Shards, FString::Printf(TEXT("(2) GWWN642 (%dx%d): %d shard pixels see-through in the SHADES' white (%d), the old rule's solid grey %d; %d clear, %d solid (the frame)"),
				W, H, Shards, ShardsRight, WasSolid, Clear, Solid));
			const bool bWhole = OutlawsTextures::LevelPcxPixels(*A, TEXT("GWWN641.PCX"), NoPal, &Sh, W, H, Whole);
			int32 Part = 0, WholeClear = 0;
			for (const FColor& C : Whole) { Part += C.A > 0 && C.A < 255 ? 1 : 0; WholeClear += C.A == 0 ? 1 : 0; }
			Check(bWhole && Part == 0 && WholeClear > W * H / 2, FString::Printf(TEXT("(3) GWWN641, the whole window: %d clear pixels of %d, %d half see-through (wanted none: its pane is clear as before)"), WholeClear, W * H, Part));
		}
		// (4) the snapshot's windows
		for (const int32 S : { 43, 45 })
		{
			const int32 P = L->DebugPaneAt(S, 3);
			const FString State = L->DebugPaneState(P);
			Check(P >= 0 && State.StartsWith(TEXT("6 frames")) && State.Contains(TEXT("WorldGlass")), FString::Printf(TEXT("(4) sector %d wall 3: pane %d, %s (Remastered %s)"), S, P, *State, L->bHDTextures ? TEXT("on") : TEXT("off")));
		}
		Place();
		Wait = ShotTime + 2.f;
		ShotStep = 1;
		return;
	}
	case 1:
	{
		if (ShotTime < Wait) return;
		Place();
		const FString Name = FString::Printf(TEXT("glass8bit_%s_whole"), *Tag);
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT glass8bit shot %s.png"), *Name);
		for (const int32 S : { 43, 45 }) L->DebugShatter(L->DebugPaneAt(S, 3));
		Wait = ShotTime + 2.f;
		ShotStep = 2;
		return;
	}
	case 2:
	{
		if (ShotTime < Wait) return;
		Place();
		for (const int32 S : { 43, 45 })
		{
			const FString State = L->DebugPaneState(L->DebugPaneAt(S, 3));
			Check(State.Contains(TEXT("broken 1")) && State.Contains(TEXT("frame 5")), FString::Printf(TEXT("(5) sector %d wall 3 shot: %s (its last break frame held)"), S, *State));
		}
		const FString Name = FString::Printf(TEXT("glass8bit_%s_broken"), *Tag);
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT glass8bit shot %s.png"), *Name);
		Wait = ShotTime + 1.5f;
		ShotStep = 3;
		return;
	}
	case 3:
		if (ShotTime < Wait) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT glass8bit RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
