// BUG-STAIRWALLS (user 3 Nov, the user's snapshots #10 and #11: the villa's spiral stair round the bath kept a riser band
// missing): -OLShots -OLLevel=villa -OLNoFrontEnd -OLLiftNeighbourProbe [-OLOldLiftNeighbours].  The links from a lift to the
// other lifts' groups holding walls on it counted; the stair photographed from snapshot #11's spot; THE_BATH_ITSELF's GOTO_STOP
// 1 sent to STEP01..STEP07 as the bath sends it; once they are all at rest each step's walls as drawn are checked against a
// fresh build from the live heights (no stale band), and the stair photographed again from snapshots #11's and #10's spots
// (liftneighbour_11_before / _11_after / _10_after[_old].png in Saved\OLShots).  Lines "OLSHOT liftneighbour check PASS|FAIL:
// ..." and "OLSHOT liftneighbour RESULT".

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

OL_SHOT_MATCH(OLLiftNeighbourProbe) { return OLFlags::Param(TEXT("OLLiftNeighbourProbe")); }

OL_SHOT_TEST(OLLiftNeighbourProbe, 1700)
{
	static int32 Passes = 0, Fails = 0;
	static float Wait = 0.f, Until = 0.f;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT liftneighbour check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !bDeadView;
	const bool bOld = OLFlags::Param(TEXT("OLOldLiftNeighbours"));
	// the snapshots' spots (info.txt): #11 on the stair (sector 577) looking back down round it, #10 at its foot (574)
	const FVector At11(4681.162, 1955.537, -36.563), At10(4208.941, 2148.739, -300.985);
	const FRotator Look11(-7.25, -173.43, 0.0), Look10(-2.89, 33.93, 0.0);
	auto StandAt = [&](const FVector& At, const FRotator& Look)
	{
		TeleportTo(At, FRotator(0.0, Look.Yaw, 0.0), false, true);
		if (Controller) Controller->SetControlRotation(Look);
	};
	auto Shot = [&](const TCHAR* Name)
	{
		const FString File = FString::Printf(TEXT("liftneighbour_%s%s"), Name, bOld ? TEXT("_old") : TEXT(""));
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / File + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT liftneighbour shot %s.png"), *File);
	};
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 4.f || !bUp) return;
		L->bInvincible = true;
		Check(L->LevelName.Equals(TEXT("villa"), ESearchCase::IgnoreCase), FString::Printf(TEXT("the level is the villa (%s)"), *L->LevelName));
		const int32 Links = L->DebugLiftNeighbourLinks();
		if (bOld) Check(Links == 0, FString::Printf(TEXT("old rule: no links between lifts (%d)"), Links));
		else Check(Links > 0, FString::Printf(TEXT("%d links from a lift to another lift's group holding walls on it"), Links));
		StandAt(At11 + FVector(0.0, 0.0, 190.0), Look11);      // (the steps all at their first stop, floor 1: 6 units over 577's -5)
		Wait = ShotTime + 2.f;
		ShotStep = 1;
		return;
	}
	case 1:
		if (ShotTime < Wait) return;
		Shot(TEXT("11_before"));
		Wait = ShotTime + 1.f;
		ShotStep = 2;
		return;
	case 2:
	{
		if (ShotTime < Wait) return;
		for (int32 i = 1; i <= 7; ++i) L->DebugInfGotoStop(FString::Printf(TEXT("STEP%02d"), i), 1);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT liftneighbour: GOTO_STOP 1 to STEP01..STEP07 (as THE_BATH_ITSELF)"));
		Wait = ShotTime + 0.5f;
		Until = ShotTime + 40.f;
		ShotStep = 3;
		return;
	}
	case 3:
	{
		if (ShotTime < Wait) return;
		if (!L->DebugLiftsAtRest(TEXT("STEP0")))      // (STEP01..07, not the STEPPING_STONEs)
		{
			if (ShotTime < Until) return;
			Check(false, TEXT("the steps came to rest within 40 s"));
		}
		int32 Groups = 0;
		FString Report;
		const int32 Stale = L->DebugLiftWallsCheck(TEXT("STEP0"), Groups, Report);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT liftneighbour steps at rest:%s"), *Report);
		if (bOld) Check(Groups == 7 && Stale > 0, FString::Printf(TEXT("old rule: %d of the %d steps' walls stale (the riser bands the step above stopped before)"), Stale, Groups));
		else Check(Groups == 7 && Stale == 0, FString::Printf(TEXT("every step's walls as drawn match a fresh build from the live heights (%d of %d stale)"), Stale, Groups));
		StandAt(At11, Look11);          // (the snapshot's spot again: before the bath the steps stood at its first stop, above it)
		Wait = ShotTime + 2.f;
		ShotStep = 4;
		return;
	}
	case 4:
		if (ShotTime < Wait) return;
		Shot(TEXT("11_after"));
		Wait = ShotTime + 1.f;
		ShotStep = 5;
		return;
	case 5:
		if (ShotTime < Wait) return;
		StandAt(At10, Look10);
		Wait = ShotTime + 2.f;
		ShotStep = 6;
		return;
	case 6:
		if (ShotTime < Wait) return;
		Shot(TEXT("10_after"));
		Wait = ShotTime + 1.f;
		ShotStep = 7;
		return;
	case 7:
		if (ShotTime < Wait) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT liftneighbour RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
