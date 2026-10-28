// BUG-OUTHOUSEWALL (user 28 Oct, snapshots 1, 2 and 6-9): the hideout outhouse's back wall drew its planks and GWCRACK
// from the floor right up to the ceiling before the blast; the original draws them only up to the beam under the opening
// into the room with the hole.  -OLShots -OLLevel=hideout -OLNoFrontEnd -OLOuthouseWallProbe [-OLOldOuthouseWall]
// [-featureleveles31]: every explodable wall's drawn plug against its sector (no more than the sector's height and the
// beam's half unit over it), shots from the snapshots' spots before and after the wall is blown,
// outhouse_<1|6>_<before|after>[_old][_es31].png in Saved\OLShots.
// Lines "OLSHOT outhouse check PASS|FAIL: ..." and "OLSHOT outhouse RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLOuthouseWallProbe) { return OLFlags::Param(TEXT("OLOuthouseWallProbe")); }

OL_SHOT_TEST(OLOuthouseWallProbe, 1261)
{
	static int32 Passes = 0, Fails = 0, Spot = 0;
	static float Wait = 0.f;
	static bool bAfter = false;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT outhouse check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	const bool bOld = OLFlags::Param(TEXT("OLOldOuthouseWall"));
	const bool bEs31 = GetWorld() && GetWorld()->FeatureLevel == ERHIFeatureLevel::ES3_1;
	// the snapshots' spots (their info.txt: the player's UE location and the eye's look): 1 just outside the door,
	// 6 a step back and to the side, as the original's shot stands (the door shut unless the player has opened it); 0
	// inside the outhouse at its door, facing the back wall, a little up
	struct FSpot { int32 Shot; FVector At; FRotator Look; };
	static const FSpot Spots[] = {
		{ 0, FVector(13530.0, 13965.0, -85.0), FRotator(18.0, 0.0, 0.0) },
		{ 1, FVector(13252.376, 14037.716, -0.850), FRotator(-0.09, -13.62, 0.0) },
		{ 6, FVector(13252.496, 14075.961, -0.850), FRotator(4.35, -22.24, 0.0) },
	};
	auto Place = [this](const FSpot& S)
	{
		TeleportTo(S.At, FRotator(0.0, S.Look.Yaw, 0.0), false, true);
		if (Controller) Controller->SetControlRotation(S.Look);
	};
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 4.f || !bUp) return;
		L->bInvincible = true;
		if (!L->LevelName.Equals(TEXT("hideout"), ESearchCase::IgnoreCase))
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT outhouse note: the outhouse is in hideout; %s's explodable walls are checked, the shots are of nothing"), *L->LevelName);
		// every explodable wall: its plug no taller than its sector, bar the beam's half unit (the sign runs up the solid
		// part to the opening above); the old fill ran on up the openings beside it
		int32 Over = 0;
		for (int32 i = 0; i < L->DebugExplodeWallCount(); ++i)
		{
			FBox Drawn, Sec;
			int32 SecIndex = -1;
			if (!L->DebugExplodeWallDrawn(i, Drawn, Sec, SecIndex)) continue;
			const double Above = Drawn.Max.Z - Sec.Max.Z;
			Over += Above > 16.0 ? 1 : 0;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT outhouse wall %d (sector %d): plug drawn %.0f..%.0f cm, its sector %.0f..%.0f cm, %.0f cm over"), i, SecIndex, Drawn.Min.Z, Drawn.Max.Z,
				Sec.Min.Z, Sec.Max.Z, Above);
		}
		if (bOld)
			Check(Over > 0, FString::Printf(TEXT("%s (-OLOldOuthouseWall): %d of %d explodable walls drawn over their sector's height (the fill up the opening beside)"), *L->LevelName, Over, L->DebugExplodeWallCount()));
		else
			Check(L->DebugExplodeWallCount() > 0 && Over == 0, FString::Printf(TEXT("%s: %d of %d explodable walls drawn over their sector's height"), *L->LevelName, Over, L->DebugExplodeWallCount()));
		Spot = 0;
		bAfter = false;
		Place(Spots[Spot]);
		Wait = ShotTime + 2.f;
		ShotStep = 1;
		return;
	}
	case 1:
	{
		if (ShotTime < Wait) return;
		const FString Name = FString::Printf(TEXT("outhouse_%d_%s%s%s"), Spots[Spot].Shot, bAfter ? TEXT("after") : TEXT("before"), bOld ? TEXT("_old") : TEXT(""), bEs31 ? TEXT("_es31") : TEXT(""));
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT outhouse shot %s.png (the player at %s, sector %d)"), *Name, *GetActorLocation().ToCompactString(), L->PlayerSectorIndex());
		Wait = ShotTime + 1.5f;
		ShotStep = 2;
		return;
	}
	case 2:
		if (ShotTime < Wait) return;
		if (++Spot < UE_ARRAY_COUNT(Spots))
		{
			Place(Spots[Spot]);
			Wait = ShotTime + 1.5f;
			ShotStep = 1;
			return;
		}
		if (!bAfter)
		{
			// the blast: every explodable wall's plug goes
			for (int32 i = 0; i < L->DebugExplodeWallCount(); ++i) L->DebugBlowExplodeWall(i);
			int32 Left = 0;
			for (int32 i = 0; i < L->DebugExplodeWallCount(); ++i)
			{
				FBox Drawn, Sec;
				int32 SecIndex = -1;
				Left += L->DebugExplodeWallDrawn(i, Drawn, Sec, SecIndex) ? 1 : 0;
			}
			Check(Left == 0, FString::Printf(TEXT("blown: %d of %d plugs still drawn"), Left, L->DebugExplodeWallCount()));
			bAfter = true;
			Spot = 0;
			Place(Spots[Spot]);
			Wait = ShotTime + 2.f;
			ShotStep = 1;
			return;
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT outhouse RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
