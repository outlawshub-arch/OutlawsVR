// BUG-SKYWALL (user 1 Nov, snapshots 9, 10 and 11: log fence and blank grey-green strips standing up into the sky over the
// civil war yards, where the original shows open sky): -OLShots -OLLevel=civlwar1 -OLNoFrontEnd -OLSkyWallProbe
// [-OLOldSkyWall] [-featureleveles31].  Every level built both ways, the rule and -OLOldSkyWall's: the top and bottom parts
// turned to sky or drawn again counted, the rest of each build checked unchanged (floors, ceilings, the openings' collision,
// see-through mids, signs; every count where nothing turned).  Then shots from the snapshots' spots in civlwar1 and civlwar2
// and from wharf sector 777 at the warehouse front over its passage, skywall_<9|10|11|wharf>[_old][_es31].png in
// Saved\OLShots.  Lines "OLSHOT skywall check PASS|FAIL: ...", "OLSHOT skywall level ..." and "OLSHOT skywall RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsLevel.h"
#include "OutlawsLab.h"
#include "OutlawsGeometry.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLSkyWallProbe) { return OLFlags::Param(TEXT("OLSkyWallProbe")); }

OL_SHOT_TEST(OLSkyWallProbe, 1370)
{
	static int32 Passes = 0, Fails = 0, Spot = 0;
	static float Wait = 0.f;
	static FVector Fix = FVector::ZeroVector;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skywall check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	const bool bOld = OLFlags::Param(TEXT("OLOldSkyWall"));
	const bool bEs31 = GetWorld() && GetWorld()->FeatureLevel == ERHIFeatureLevel::ES3_1;
	// the snapshots (info.txt): the player's UE location, the eye's, the look; the wharf's spot is ours, in sector 777 12
	// units off its wall 4 (the passage into 761 under the warehouse front), looking up at the front over the passage
	struct FSpot { const TCHAR* Level; const TCHAR* Shot; FVector At, Eye; FRotator Look; };
	static const FSpot Spots[] = {
		{ TEXT("civlwar1"), TEXT("9"), FVector(-27482.587, -18589.966, 976.131), FVector(-27483.234, -18590.488, 1044.986), FRotator(7.79, -16.51, 0.0) },
		{ TEXT("civlwar2"), TEXT("10"), FVector(-26789.772, -18725.471, 80.308), FVector(-26789.504, -18724.875, 152.692), FRotator(17.02, -19.14, 0.0) },
		{ TEXT("civlwar2"), TEXT("11"), FVector(-23285.181, -21067.765, 89.150), FVector(-23285.945, -21068.578, 164.143), FRotator(42.23, 88.93, 0.0) },
		{ TEXT("wharf"), TEXT("wharf"), FVector(23775.0, -4779.0, 90.0), FVector(23775.0, -4779.0, 170.0), FRotator(48.0, -19.9, 0.0) },
	};
	APlayerController* PC = Cast<APlayerController>(Controller);
	auto EyeNow = [PC]() { return PC && PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : FVector::ZeroVector; };
	auto Place = [this](const FSpot& S)
	{
		TeleportTo(S.At + Fix, FRotator(0.0, S.Look.Yaw, 0.0), false, true);
		if (Controller) Controller->SetControlRotation(S.Look);
	};
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 4.f || !bUp) return;
		L->bInvincible = true;
		// every level built both ways from its LVT (no textures: the counts only)
		const FOutlawsAssets* A = L->GetAssets();
		TArray<FString> Lvts;
		if (A) A->ListByExtension(TEXT(".lvt"), Lvts);
		Lvts.Sort();
		// the counts the rule may not touch: floors and ceilings, sky surfaces, the openings' collision (open, NOPASS, shots,
		// FENCE), see-through mids and their glass, signs, water
		static const TCHAR* Kept[] = { TEXT("floor"), TEXT("ceil"), TEXT("sky_floor"), TEXT("sky_ceiling"), TEXT("open"), TEXT("ALL"), TEXT("PAWN"),
			TEXT("SHOT"), TEXT("fence"), TEXT("nopass_one_sided_dropped"), TEXT("wall_midtex"), TEXT("tinted_glass"), TEXT("wall_overlay"),
			TEXT("water_surface"), TEXT("default_wall_kept_solid"), TEXT("default_sign_skipped") };
		const bool bWas = FOutlawsGeometry::bOldSkyWall;
		const TMap<FString, FOutlawsTexInfo> NoTex;
		int32 Levels = 0, Changed = 0, KeptBad = 0, QuietBad = 0, TopSky = 0, TopDrawn = 0, BotSky = 0, BotDrawn = 0;
		TMap<FString, int32> TopSkyOf, TopDrawnOf;
		for (const FString& File : Lvts)
		{
			const FString Name = FPaths::GetBaseFilename(File).ToLower();
			FString Lvt;
			if (!A->Has(Name + TEXT(".obt")) || !A->GetText(Name + TEXT(".lvt"), Lvt)) continue;
			FOutlawsLevel Lv;
			if (!Lv.ParseLvt(Lvt)) continue;
			FOutlawsGeometry GNew, GOld;
			FOutlawsGeometry::bOldSkyWall = false;
			GNew.Build(Lv, NoTex, true);
			FOutlawsGeometry::bOldSkyWall = true;
			GOld.Build(Lv, NoTex, true);
			FOutlawsGeometry::bOldSkyWall = bWas;
			++Levels;
			const int32 Ts = GNew.Stats.FindRef(TEXT("skywall_top_to_sky")), Td = GNew.Stats.FindRef(TEXT("skywall_top_to_drawn"));
			const int32 Bs = GNew.Stats.FindRef(TEXT("skywall_bot_to_sky")), Bdn = GNew.Stats.FindRef(TEXT("skywall_bot_to_drawn"));
			TopSky += Ts; TopDrawn += Td; BotSky += Bs; BotDrawn += Bdn;
			TopSkyOf.Add(Name, Ts);
			TopDrawnOf.Add(Name, Td);
			TArray<FString> Bad;
			for (const TCHAR* K : Kept)
			{
				const int32 O = GOld.Stats.FindRef(K), N = GNew.Stats.FindRef(K);
				if (O != N) Bad.Add(FString::Printf(TEXT("%s %d->%d"), K, O, N));
			}
			KeptBad += Bad.Num() > 0 ? 1 : 0;
			// a level where nothing turned: every count the same, bar the sky parts' new collision
			const bool bTurned = Ts + Td + Bs + Bdn > 0;
			Changed += bTurned ? 1 : 0;
			TSet<FString> Keys;
			for (const TPair<FString, int32>& Kv : GOld.Stats) Keys.Add(Kv.Key);
			for (const TPair<FString, int32>& Kv : GNew.Stats) Keys.Add(Kv.Key);
			TArray<FString> Diffs;
			for (const FString& K : Keys)
			{
				const int32 O = GOld.Stats.FindRef(K), N = GNew.Stats.FindRef(K);
				if (O != N) Diffs.Add(FString::Printf(TEXT("%s %d->%d"), *K, O, N));
			}
			Diffs.Sort();
			if (!bTurned && Diffs.ContainsByPredicate([](const FString& D) { return !D.StartsWith(TEXT("sky_band_solid")); })) QuietBad += 1;
			int32 TrisOld = 0, TrisNew = 0;
			for (const TPair<FString, FOutlawsMeshSection>& Kv : GOld.World) TrisOld += Kv.Value.NumTriangles();
			for (const TPair<FString, FOutlawsMeshSection>& Kv : GNew.World) TrisNew += Kv.Value.NumTriangles();
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skywall level %-9s top parts to sky %d, drawn again %d; bottom parts to sky %d, drawn again %d | sky parts %d->%d | world tris %d->%d | %s%s"),
				*Name, Ts, Td, Bs, Bdn, GOld.Stats.FindRef(TEXT("sky_band")), GNew.Stats.FindRef(TEXT("sky_band")), TrisOld, TrisNew,
				Diffs.Num() ? *FString::Join(Diffs, TEXT(", ")) : TEXT("no count changed"), Bad.Num() ? *(TEXT(" | KEPT COUNTS CHANGED: ") + FString::Join(Bad, TEXT(", "))) : TEXT(""));
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skywall total over %d levels (%d with a part turned): top parts to sky %d, drawn again %d; bottom parts to sky %d, drawn again %d"),
			Levels, Changed, TopSky, TopDrawn, BotSky, BotDrawn);
		Check(Levels >= 30, FString::Printf(TEXT("%d levels built both ways"), Levels));
		Check(TopSkyOf.FindRef(TEXT("civlwar1")) > 0 && TopSkyOf.FindRef(TEXT("civlwar2")) > 0,
			FString::Printf(TEXT("the yards' tops over the flagged strips turn to sky: civlwar1 %d, civlwar2 %d"), TopSkyOf.FindRef(TEXT("civlwar1")), TopSkyOf.FindRef(TEXT("civlwar2"))));
		Check(TopDrawnOf.FindRef(TEXT("wharf")) > 0, FString::Printf(TEXT("the wharf's warehouse front over the passage from flagged 777 drawn again: %d"), TopDrawnOf.FindRef(TEXT("wharf"))));
		int32 Others = 0;
		for (const TCHAR* N : { TEXT("town"), TEXT("hideout"), TEXT("canyon") }) Others += TopSkyOf.FindRef(N) + TopDrawnOf.FindRef(N);
		Check(Others == 0, FString::Printf(TEXT("town, hideout and canyon: %d parts turned"), Others));
		Check(KeptBad == 0, FString::Printf(TEXT("floors, ceilings, openings' collision, see-through mids and signs the same both ways on every level: %d levels differ"), KeptBad));
		Check(QuietBad == 0, FString::Printf(TEXT("every level with no part turned built the same both ways: %d differ"), QuietBad));
		Spot = 0;
		Fix = FVector::ZeroVector;
		Place(Spots[0]);
		Wait = ShotTime + 1.f;
		ShotStep = 1;
		return;
	}
	case 1:
	{
		// the eye put where the snapshot's was (this pawn's eye stands a little apart from the headset's)
		if (ShotTime < Wait || !bUp) return;
		Fix += Spots[Spot].Eye - EyeNow();
		Place(Spots[Spot]);
		Wait = ShotTime + 1.5f;
		ShotStep = 2;
		return;
	}
	case 2:
	{
		if (ShotTime < Wait) return;
		const FString Name = FString::Printf(TEXT("skywall_%s%s%s"), Spots[Spot].Shot, bOld ? TEXT("_old") : TEXT(""), bEs31 ? TEXT("_es31") : TEXT(""));
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skywall shot %s.png (%s, eye %s, %.0f cm from the spot's, sector %d)"), *Name, *L->LevelName, *EyeNow().ToCompactString(),
			FVector::Dist(EyeNow(), Spots[Spot].Eye), L->PlayerSectorIndex());
		Wait = ShotTime + 1.f;
		ShotStep = 3;
		return;
	}
	case 3:
		if (ShotTime < Wait) return;
		if (++Spot >= UE_ARRAY_COUNT(Spots))
		{
			ShotStep = 5;
			return;
		}
		Fix = FVector::ZeroVector;
		if (!L->LevelName.Equals(Spots[Spot].Level, ESearchCase::IgnoreCase))
		{
			L->JumpToLevel(Spots[Spot].Level);
			Wait = ShotTime + 3.f;
			ShotStep = 4;
			return;
		}
		Place(Spots[Spot]);
		Wait = ShotTime + 1.f;
		ShotStep = 1;
		return;
	case 4:
		if (ShotTime < Wait || !bUp || !L->LevelName.Equals(Spots[Spot].Level, ESearchCase::IgnoreCase)) return;
		L->bInvincible = true;
		Place(Spots[Spot]);
		Wait = ShotTime + 2.f;
		ShotStep = 1;
		return;
	case 5:
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skywall RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
