// BUG-DRIPABOVE (user 2 Nov, snapshots #36/#37, shgranry: a DRIP's drop hung at knee height over the walkway 327, the room
// above its own 328, between falls): -OLShots -OLLevel=shgranry -OLNoFrontEnd -OLDripWaitProbe [-OLOldEmptyFrame].  The
// player at snapshot #37's spot; every DRIP watched each tick over two loops of its script (82 frames at 18 fps, 4.6 s):
// (a) while its empty frame 0 is on (the wait, 43 of the 82) the quad is hidden; (b) on the frames it draws, the drop falls
// in its own sector as before (cells shown, the quad's lowest and highest place over its floor); (c) a shot from the
// snapshot's spot while the nearest drip waits (dripwait_37[_old].png in Saved\OLShots).  Lines "OLSHOT dripwait check
// PASS|FAIL: ..." and "OLSHOT dripwait RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsLevel.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLDripWaitProbe) { return OLFlags::Param(TEXT("OLDripWaitProbe")); }

OL_SHOT_TEST(OLDripWaitProbe, 1470)
{
	static int32 Passes = 0, Fails = 0, Samples = 0, Empty = 0, EmptyShown = 0, Shown = 0;
	static float Wait = 0.f, Until = 0.f;
	static double LowZ = 1e9, HighZ = -1e9;
	static TSet<int32> Cells;
	static TArray<TWeakObjectPtr<AOutlawsSpriteActor>> Drips;
	static TWeakObjectPtr<AOutlawsSpriteActor> Nearest;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dripwait check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	const bool bOld = OLFlags::Param(TEXT("OLOldEmptyFrame"));
	// snapshot #37 (info.txt): the player's UE location and look
	const FVector At(-3532.027, 2031.270, -64.850);
	const FRotator Look(3.70, -173.85, 0.0);
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 4.f || !bUp) return;
		L->bInvincible = true;
		Check(L->LevelName.Equals(TEXT("shgranry"), ESearchCase::IgnoreCase), FString::Printf(TEXT("the level is shgranry (%s)"), *L->LevelName));
		TeleportTo(At, FRotator(0.0, Look.Yaw, 0.0), false, true);
		if (Controller) Controller->SetControlRotation(Look);
		Drips.Reset();
		double Best = 1e18;
		for (TActorIterator<AOutlawsSpriteActor> It(GetWorld()); It; ++It)
		{
			if (!It->OutlawsName.Equals(TEXT("DRIP"), ESearchCase::IgnoreCase)) continue;
			Drips.Add(*It);
			const double D = FVector::Dist2D(It->GetActorLocation(), At);
			if (D < Best) { Best = D; Nearest = *It; }
		}
		Check(Drips.Num() == 13, FString::Printf(TEXT("13 DRIPs in the level (%d), the nearest %.0f cm from the snapshot's spot"), Drips.Num(), Best));
		Wait = ShotTime + 1.f;
		Until = Wait + 9.2f;
		ShotStep = 1;
		return;
	}
	case 1:
	{
		if (ShotTime < Wait) return;
		const FOutlawsLevel* Live = L->DebugLiveLevel();
		for (const TWeakObjectPtr<AOutlawsSpriteActor>& W : Drips)
		{
			const AOutlawsSpriteActor* S = W.Get();
			if (!S || !S->Quad || S->DebugCulled()) continue;
			++Samples;
			const bool bQuadHidden = S->Quad->bHiddenInGame;
			if (S->DebugEmptyCell())
			{
				++Empty;
				if (!bQuadHidden) ++EmptyShown;
				continue;
			}
			if (bQuadHidden) continue;
			++Shown;
			Cells.Add(S->DebugCell());
			const FOutlawsSector* Home = Live ? Live->Sector(S->HomeSector) : nullptr;
			if (!Home) continue;
			// the quad's box over its sector's floor, in game units (UE Z / 30)
			const FBoxSphereBounds B = S->Quad->Bounds;
			const double Foot = (B.Origin.Z - B.BoxExtent.Z) / 30.0 - Home->Floor.Y;
			LowZ = FMath::Min(LowZ, Foot);
			HighZ = FMath::Max(HighZ, (B.Origin.Z + B.BoxExtent.Z) / 30.0 - Home->Floor.Y);
		}
		if (ShotTime < Until) return;
		const double Frac = Samples ? double(Empty) / Samples : 0.0;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dripwait %d samples of %d drips: %d on the empty frame (%.0f%%, the script's 43 of 82 = 52%%), %d of those with the quad shown; %d drawn, %d cells, the quad's box %.2f .. %.2f units over its floor"),
			Samples, Drips.Num(), Empty, Frac * 100.0, EmptyShown, Shown, Cells.Num(), LowZ, HighZ);
		if (bOld)
		{
			Check(Empty == 0, FString::Printf(TEXT("old rule: no empty frame, the wait shows the first drawn cell (%d empty of %d)"), Empty, Samples));
		}
		else
		{
			Check(Samples > 0 && Frac > 0.4 && Frac < 0.65, FString::Printf(TEXT("the wait is shown as nothing about half the time (%.0f%% of %d samples)"), Frac * 100.0, Samples));
			Check(EmptyShown == 0, FString::Printf(TEXT("during the wait the quad is hidden (%d of %d shown)"), EmptyShown, Empty));
		}
		// (the quad's box is the whole cell, the drawing at its foot: the range is the one to compare with -OLOldEmptyFrame's)
		Check(Cells.Num() >= 12, FString::Printf(TEXT("the fall still draws: %d cells, the quad's box from %.2f to %.2f units over its floor"), Cells.Num(), LowZ, HighZ));
		Wait = ShotTime + 6.f;
		ShotStep = 2;
		return;
	}
	case 2:
	{
		// (c) the shot while the nearest drip waits (the old rule: while it shows the cell the wait used to)
		const AOutlawsSpriteActor* S = Nearest.Get();
		const bool bWaiting = S && (bOld ? S->DebugCell() == 0 : S->DebugEmptyCell());
		if (!bWaiting && ShotTime < Wait) return;
		const FString Name = FString::Printf(TEXT("dripwait_37%s"), bOld ? TEXT("_old") : TEXT(""));
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dripwait shot %s.png (nearest drip %s, cell %d, quad %s)"), *Name, bWaiting ? TEXT("waiting") : TEXT("not caught waiting"),
			S ? S->DebugCell() : -1, S && S->Quad && S->Quad->bHiddenInGame ? TEXT("hidden") : TEXT("shown"));
		Wait = ShotTime + 1.f;
		ShotStep = 3;
		return;
	}
	case 3:
		if (ShotTime < Wait) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dripwait RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
