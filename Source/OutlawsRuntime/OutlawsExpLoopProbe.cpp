// BUG-CWEXPLOOP (user 2026-10-31 via the planner: on the Civil War level the explosions near the player start kept looping
// their animation after they had gone off): -OLShots -OLLevel=civlwar1 -OLNoFrontEnd -OLExpLoopProbe [-OLOldExpLoop].
// The blasts are NBOOM generators (FUNC Gen_New, GENERATE explode1.NWX, MASTER OFF), turned on by the start area's trigger
// sectors (CTRIG: #541A, #5416..#5419; ATRIG: #5111..#5114).  explode1's script is 0 1 2 3 4 5 TERMINATE: the original
// plays it once and frees the object.  The probe walks into ATRIG and CTRIG, goes back to the start and watches the blasts
// for 20 s: shots cwexploop_<after|before>_blast.png (just after they go off) and _20s.png in Saved\OLShots, a state line a
// sample (shown, gone, the cell on show).  Lines "OLSHOT cwexploop check PASS|FAIL: ..." and "OLSHOT cwexploop RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLExpLoopProbe) { return OLFlags::Param(TEXT("OLExpLoopProbe")); }

OL_SHOT_TEST(OLExpLoopProbe, 1330)
{
	static int32 Passes = 0, Fails = 0, Sample = 0;
	static float Wait = 0.f, OnAt = 0.f;
	static FVector Start = FVector::ZeroVector;
	static FRotator StartRot = FRotator::ZeroRotator;
	static const TCHAR* const Watched[] = { TEXT("541A"), TEXT("5416"), TEXT("5417"), TEXT("5418"), TEXT("5419"), TEXT("5111"), TEXT("5112"), TEXT("5113"), TEXT("5114") };
	static const float SampleAt[] = { 0.15f, 1.f, 2.f, 5.f, 10.f, 20.f };
	if (!L || ShotStep < 0) return;
	const bool bOld = OLFlags::Param(TEXT("OLOldExpLoop"));
	const TCHAR* const Tag = bOld ? TEXT("before") : TEXT("after");
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cwexploop check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	auto Gen = [L](const TCHAR* Id) -> AOutlawsSpriteActor* { const TWeakObjectPtr<AOutlawsSpriteActor>* G = L->CellGens.Find(Id); return G ? G->Get() : nullptr; };
	auto Count = [&](int32& OutShown, int32& OutGone, FString& OutCells, FVector& OutMid)
	{
		OutShown = OutGone = 0;
		OutCells.Reset();
		OutMid = FVector::ZeroVector;
		int32 N = 0;
		for (const TCHAR* Id : Watched)
		{
			AOutlawsSpriteActor* S = Gen(Id);
			if (!S) continue;
			if (!S->IsHidden()) ++OutShown;
			if (S->bTerminated) ++OutGone;
			OutCells += FString::Printf(TEXT(" %s:%s"), Id, S->IsHidden() ? (S->bTerminated ? TEXT("gone") : TEXT("off")) : *FString::FromInt(S->CurrentCell));
			OutMid += S->GetActorLocation();
			++N;
		}
		if (N) OutMid /= N;
	};
	auto LookAtBlasts = [&]()
	{
		int32 Shown, Gone;
		FString Cells;
		FVector Mid;
		Count(Shown, Gone, Cells, Mid);
		const FRotator R = (Mid - Start).Rotation();
		TeleportTo(Start, FRotator(0.0, R.Yaw, 0.0), false, true);
		if (Controller) Controller->SetControlRotation(FRotator(-4.0, R.Yaw, 0.0));
	};
	auto Shot = [&](const TCHAR* What)
	{
		const FString Name = FString::Printf(TEXT("cwexploop_%s_%s"), Tag, What);
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cwexploop shot %s.png"), *Name);
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 4.f || !bUp) return;
		L->bInvincible = true;
		Start = GetActorLocation();
		StartRot = GetActorRotation();
		int32 Have = 0, OneShot = 0;
		for (const TCHAR* Id : Watched)
		{
			AOutlawsSpriteActor* S = Gen(Id);
			if (!S) continue;
			++Have;
			if (S->EndOp == 2) ++OneShot;
		}
		Check(L->LevelName.Equals(TEXT("civlwar1"), ESearchCase::IgnoreCase), FString::Printf(TEXT("the level is %s (wanted civlwar1)"), *L->LevelName));
		Check(Have == UE_ARRAY_COUNT(Watched), FString::Printf(TEXT("%d of the %d start-area NBOOM generators built (%d figure generators in all)"), Have, int32(UE_ARRAY_COUNT(Watched)), L->CellGens.Num()));
		Check(bOld ? OneShot == 0 : OneShot == Have, FString::Printf(TEXT("%d of them a one-shot (explode1: TERMINATE ends its script; wanted %s)"), OneShot, bOld ? TEXT("none, as before") : TEXT("all")));
		FVector Spot;
		if (L->DebugSectorSpot(TEXT("ATRIG"), Spot)) TeleportTo(Spot + FVector(0.0, 0.0, 100.0), StartRot, false, true);
		else Check(false, TEXT("no ATRIG sector"));
		Wait = ShotTime + 0.5f;
		ShotStep = 1;
		return;
	}
	case 1:
	{
		if (ShotTime < Wait) return;
		FVector Spot;
		if (L->DebugSectorSpot(TEXT("CTRIG"), Spot)) TeleportTo(Spot + FVector(0.0, 0.0, 100.0), StartRot, false, true);
		else Check(false, TEXT("no CTRIG sector"));
		Wait = ShotTime + 0.05f;
		ShotStep = 2;
		return;
	}
	case 2:
	{
		if (ShotTime < Wait) return;
		int32 Shown, Gone;
		FString Cells;
		FVector Mid;
		Count(Shown, Gone, Cells, Mid);
		if (Shown + Gone == 0 && ShotTime < Wait + 5.f) return;
		Check(Shown + Gone == UE_ARRAY_COUNT(Watched), FString::Printf(TEXT("the triggers set off %d of the %d blasts"), Shown + Gone, int32(UE_ARRAY_COUNT(Watched))));
		OnAt = ShotTime;
		LookAtBlasts();
		Sample = 0;
		ShotStep = 3;
		return;
	}
	case 3:
	{
		if (ShotTime < OnAt + SampleAt[Sample]) return;
		int32 Shown, Gone;
		FString Cells;
		FVector Mid;
		Count(Shown, Gone, Cells, Mid);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cwexploop state at %.1f s: %d shown, %d gone, cells%s"), ShotTime - OnAt, Shown, Gone, *Cells);
		if (Sample == 0) Shot(TEXT("blast"));
		if (++Sample < UE_ARRAY_COUNT(SampleAt)) return;
		Shot(TEXT("20s"));
		if (bOld) Check(Shown == UE_ARRAY_COUNT(Watched), FString::Printf(TEXT("as before, 20 s on: %d of the blasts still showing (looping)"), Shown));
		else Check(Shown == 0 && Gone == UE_ARRAY_COUNT(Watched), FString::Printf(TEXT("20 s on: %d of the blasts still showing, %d played once and gone (wanted 0 and all)"), Shown, Gone));
		Wait = ShotTime + 1.5f;
		ShotStep = 4;
		return;
	}
	case 4:
		if (ShotTime < Wait) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cwexploop RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
