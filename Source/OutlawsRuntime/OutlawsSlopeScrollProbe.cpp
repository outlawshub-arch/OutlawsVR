// BUG-SLOPESCROLL's probe (the user's snapshot #2, icecaves: WATERFALL01's water ran sideways): -OLShots -OLNoFrontEnd
// -OLLevel=icecaves -OLSlopeScrollProbe [-OLOldSlopeScroll], flat.  icecaves, then cliff (started as a historical mission):
// every floor scroll, read back through its surface's own UVs (DebugScrollDirs): the sloped ones named WATERFALL01..03
// (icecaves), FALL* and MUDSLIDE* (cliff) run downhill (the texture's way dotted with the floor's downhill over 0.9); every
// flat floor's scroll runs as the old rule ran it.  A shot of WATERFALL01 from above its hinge (slopescroll_wf01[_old].png
// in Saved\OLShots).  -OLOldSlopeScroll: the waterfalls' dots as they were (logged, WATERFALL01 under 0.9).  Lines
// "OLSHOT slopescroll check PASS|FAIL: ..." and "OLSHOT slopescroll RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsLevel.h"
#include "OutlawsGeometry.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsSlopeScrollProbeLocal
{
	int32 LevelI = 0, Step = 0, Passes = 0, Fails = 0;
	bool bAsked = false;
	float LevelT = 0.f;
	FString LevelName;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT slopescroll check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}
}

OL_SHOT_MATCH(OLSlopeScrollProbe) { return OLFlags::Has(TEXT("OLSlopeScrollProbe")); }

OL_SHOT_TEST(OLSlopeScrollProbe, 1560)
{
	using namespace OutlawsSlopeScrollProbeLocal;
	if (!L || ShotTime < 0.f) return;
	const bool bOld = OLFlags::Param(TEXT("OLOldSlopeScroll"));
	static const TCHAR* const Levels[] = { TEXT("icecaves"), TEXT("cliff") };
	if (LevelI >= 2)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT slopescroll RESULT %s (%d passed, %d failed, %s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			bOld ? TEXT("-OLOldSlopeScroll") : TEXT("new"));
		ShotTime = -1.f;
		FGenericPlatformMisc::RequestExit(false);
		return;
	}
	if (L->IsRebuildPending() || L->IsMoviePlaying() || bMenuOpen) return;
	if (L->IsFrontEnd() || !L->LevelName.Equals(Levels[LevelI], ESearchCase::IgnoreCase))
	{
		if (!bAsked) { bAsked = true; L->StartMission(Levels[LevelI], EOutlawsDifficulty::Bad); }
		return;
	}
	L->StartSaveDelay = -1.f;           // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	const FOutlawsLevel* Lv = L->LiveLevel();
	if (LevelName != L->LevelName) { LevelName = L->LevelName; LevelT = 0.f; Step = 0; }
	LevelT += Dt;
	if (LevelT < 3.f || !Lv) return;
	const bool bIce = LevelI == 0;
	if (Step == 0)
	{
		TArray<AOutlawsLevelActor::FDebugScrollDir> Dirs;
		L->DebugScrollDirs(Dirs);
		int32 Slopes = 0, Flats = 0, FlatSame = 0;
		for (const AOutlawsLevelActor::FDebugScrollDir& R : Dirs)
		{
			const FOutlawsSector& S = Lv->Sectors[R.Sector];
			const FString N = S.Name.ToUpper();
			if (!R.bSloped)
			{
				++Flats;
				FlatSame += FVector2D::DotProduct(R.Dir, R.OldDir) > 0.9999;
				continue;
			}
			const double Dot = FVector2D::DotProduct(R.Dir, R.Downhill), OldDot = FVector2D::DotProduct(R.OldDir, R.Downhill);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT slopescroll %s sector %d '%s': runs (%.2f, %.2f), downhill (%.2f, %.2f): dot %.3f (the old frame's %.3f)"), *LevelName, R.Sector, *S.Name,
				R.Dir.X, R.Dir.Y, R.Downhill.X, R.Downhill.Y, Dot, OldDot);
			const bool bNamed = bIce ? (N == TEXT("WATERFALL01") || N == TEXT("WATERFALL02") || N == TEXT("WATERFALL03")) : (N.StartsWith(TEXT("FALL")) || N.StartsWith(TEXT("MUDSLIDE")));
			if (!bNamed) continue;
			++Slopes;
			if (bOld)
			{
				if (N == TEXT("WATERFALL01")) Check(Dot < 0.9, FString::Printf(TEXT("(old rule) %s WATERFALL01 runs off its slope as before: dot %.3f"), *LevelName, Dot));
				continue;
			}
			Check(Dot > 0.9, FString::Printf(TEXT("%s sector %d '%s' runs downhill: dot %.3f (old frame %.3f)"), *LevelName, R.Sector, *S.Name, Dot, OldDot));
		}
		Check(Slopes > 0, FString::Printf(TEXT("%s: %d named slope scrolls found"), *LevelName, Slopes));
		Check(FlatSame == Flats, FString::Printf(TEXT("%s: %d of %d flat floor scrolls as before"), *LevelName, FlatSame, Flats));
		if (!bIce) { ++LevelI; bAsked = false; return; }
		// the shot: from two units out past WATERFALL01's hinge, up its slope, looking down it
		const FOutlawsSector* W = nullptr;
		for (const FOutlawsSector& S : Lv->Sectors) if (S.Name.Equals(TEXT("WATERFALL01"), ESearchCase::IgnoreCase)) { W = &S; break; }
		FVector2D A, Nrm, H;
		if (!W || !Lv->SlopeFrame(*W, true, A, Nrm, &H))
		{
			Check(false, TEXT("WATERFALL01 and its slope"));
			++LevelI;
			bAsked = false;
			return;
		}
		FVector2D C = FVector2D::ZeroVector;
		for (const FVector2D& V : W->Verts) C += V;
		C /= FMath::Max(1, W->Verts.Num());
		const FVector2D Mid = H + A * FVector2D::DotProduct(C - H, A);
		const FVector2D From = Mid - Nrm * 4.0;
		const FOutlawsSector* Stand = Lv->SectorAt(From.X, 1e6, From.Y);
		const double FloorY = Stand ? Lv->SurfaceY(*Stand, From, true) : W->Floor.Y;
		const FVector Eye = FOutlawsGeometry::ToUE(From.X, FloorY + 0.3, From.Y) + FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
		TeleportTo(Eye, FRotator::ZeroRotator, false, true);
		GetCharacterMovement()->StopMovementImmediately();
		const FVector Look = FOutlawsGeometry::ToUE(C.X, Lv->SurfaceY(*W, C, true), C.Y);
		if (AController* Ctl = GetController()) Ctl->SetControlRotation((Look - (Eye + FVector(0.0, 0.0, 60.0))).Rotation());
		LevelT = 3.f;
		Step = 1;
		return;
	}
	if (Step == 1)
	{
		if (LevelT < 5.f) return;
		const FString Name = FString::Printf(TEXT("slopescroll_wf01%s"), bOld ? TEXT("_old") : TEXT(""));
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT slopescroll shot %s.png from %s"), *Name, *GetActorLocation().ToCompactString());
		Step = 2;
		return;
	}
	if (LevelT < 6.f) return;
	++LevelI;
	bAsked = false;
}

#endif // WITH_OUTLAWS_TESTS
