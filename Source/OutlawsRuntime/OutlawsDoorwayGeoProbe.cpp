// BUG-DOORWAYGEO (user 28 Oct, snapshots 3-5): standing inside two of the hideout's doorways at certain spots, geometry went
// missing (X 383, Z 426, sectors 376/325; X 383, Z 395, sector 190).  -OLShots -OLLevel=hideout -OLNoFrontEnd
// -OLDoorwayGeoProbe [-OLOldDoorwayGeo] [-featureleveles31]: the player at each snapshot's spot, the eye where the snapshot
// had it; then a sweep round each spot (every 15 cm over 60 cm, and turned about); at each eye rays all round to the level's
// faces against the sectors the culling left drawn (DebugEyeScan); shots doorway_<n>[_old][_es31].png in Saved\OLShots.
// Lines "OLSHOT doorway check PASS|FAIL: ..." and "OLSHOT doorway RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLDoorwayGeoProbe) { return OLFlags::Param(TEXT("OLDoorwayGeoProbe")); }

OL_SHOT_TEST(OLDoorwayGeoProbe, 1262)
{
	static int32 Passes = 0, Fails = 0, Spot = 0, Sweep = 0, SweepMissed = 0, SweepEyes = 0;
	static float Wait = 0.f;
	static FVector Fix = FVector::ZeroVector;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT doorway check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	const bool bOld = OLFlags::Param(TEXT("OLOldDoorwayGeo"));
	const bool bEs31 = GetWorld() && GetWorld()->FeatureLevel == ERHIFeatureLevel::ES3_1;
	// the snapshots (info.txt): the player's UE location, the eye's, the look
	struct FSpot { int32 Shot; FVector At, Eye; FRotator Look; };
	static const FSpot Spots[] = {
		{ 3, FVector(12787.391, 11502.769, 479.150), FVector(12788.387, 11504.360, 554.165), FRotator(-1.42, 136.34, 0.0) },
		{ 4, FVector(12799.656, 11500.419, 479.150), FVector(12800.919, 11501.778, 554.147), FRotator(-1.49, 134.77, 0.0) },
		{ 5, FVector(11869.756, 11501.127, 479.150), FVector(11868.770, 11500.377, 554.153), FRotator(1.37, 91.13, 0.0) },
	};
	APlayerController* PC = Cast<APlayerController>(Controller);
	auto EyeNow = [PC]() { return PC && PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : FVector::ZeroVector; };
	// the sweep: 5 x 5 spots 15 cm apart round the snapshot's, each looked from 4 ways
	auto SweepAt = [](const FSpot& S, int32 k, FVector& OutAt, FRotator& OutLook)
	{
		const int32 i = k % 25, Turn = k / 25;
		OutAt = S.At + FVector(double(i % 5 - 2) * 15.0, double(i / 5 - 2) * 15.0, 0.0);
		OutLook = FRotator(0.0, S.Look.Yaw + 90.0 * Turn, 0.0);
	};
	auto Place = [this](const FVector& At, const FRotator& Look)
	{
		TeleportTo(At + Fix, FRotator(0.0, Look.Yaw, 0.0), false, true);
		if (Controller) Controller->SetControlRotation(Look);
	};
	switch (ShotStep)
	{
	case 0:
		if (ShotTime < 4.f || !bUp) return;
		L->bInvincible = true;
		Spot = 0;
		Fix = FVector::ZeroVector;
		Place(Spots[0].At, Spots[0].Look);
		Wait = ShotTime + 1.f;
		ShotStep = 1;
		return;
	case 1:
	{
		// the eye put where the snapshot's was (this pawn's eye stands a little apart from the headset's)
		if (ShotTime < Wait) return;
		Fix += Spots[Spot].Eye - EyeNow();
		Place(Spots[Spot].At, Spots[Spot].Look);
		Wait = ShotTime + 1.f;
		ShotStep = 2;
		return;
	}
	case 2:
	{
		if (ShotTime < Wait) return;
		const FVector E = EyeNow();
		int32 Rays = 0, Missed = 0;
		const FString Scan = L->DebugEyeScan(this, E, Rays, Missed);
		const FString What = FString::Printf(TEXT("snapshot %d, eye %s (%.0f cm from the snapshot's): %s"), Spots[Spot].Shot, *E.ToCompactString(), FVector::Dist(E, Spots[Spot].Eye), *Scan);
		Check(bOld ? true : Missed == 0, What);
		const FString Name = FString::Printf(TEXT("doorway_%d%s%s"), Spots[Spot].Shot, bOld ? TEXT("_old") : TEXT(""), bEs31 ? TEXT("_es31") : TEXT(""));
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT doorway shot %s.png"), *Name);
		Sweep = SweepMissed = SweepEyes = 0;
		Wait = ShotTime + 1.f;
		ShotStep = 3;
		return;
	}
	case 3:
	{
		// the sweep round the spot: placed, then scanned on the next tick but one (the flood runs on the level's tick)
		if (ShotTime < Wait) return;
		if (Sweep < 100)
		{
			FVector At;
			FRotator Look;
			SweepAt(Spots[Spot], Sweep, At, Look);
			Place(At, Look);
			Wait = ShotTime + 0.1f;
			ShotStep = 4;
			return;
		}
		Check(bOld ? true : SweepMissed == 0, FString::Printf(TEXT("snapshot %d's sweep: %d of %d eyes with geometry missing in clear view"), Spots[Spot].Shot, SweepMissed, SweepEyes));
		if (++Spot < UE_ARRAY_COUNT(Spots))
		{
			Fix = FVector::ZeroVector;
			Place(Spots[Spot].At, Spots[Spot].Look);
			Wait = ShotTime + 1.f;
			ShotStep = 1;
			return;
		}
		// the Quest cost: 11 s standing at snapshot 5's spot (under the slab: the new start every tick), then the perf
		// file's visibility buckets (its last 5 s window)
		Place(Spots[UE_ARRAY_COUNT(Spots) - 1].At, Spots[UE_ARRAY_COUNT(Spots) - 1].Look);
		Wait = ShotTime + 11.f;
		ShotStep = 6;
		return;
	}
	case 6:
	{
		if (ShotTime < Wait) return;
		TArray<FString> Lines;
		FFileHelper::LoadFileToStringArray(Lines, *(FPaths::ProjectSavedDir() / TEXT("Perf") / TEXT("perf_live.txt")));
		for (const FString& Ln : Lines) if (Ln.Contains(TEXT("visibility")) || Ln.StartsWith(TEXT("frame"))) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT doorway perf%s: %s"), bOld ? TEXT(" (old)") : TEXT(""), *Ln.TrimStartAndEnd());
		ShotStep = 5;
		return;
	}
	case 4:
	{
		if (ShotTime < Wait) return;
		int32 Rays = 0, Missed = 0;
		const FString Scan = L->DebugEyeScan(this, EyeNow(), Rays, Missed);
		++SweepEyes;
		if (Missed > 0 && SweepMissed++ < 6) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT doorway sweep %d.%d at %s: %s"), Spots[Spot].Shot, Sweep, *EyeNow().ToCompactString(), *Scan);
		++Sweep;
		ShotStep = 3;
		return;
	}
	case 5:
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT doorway RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
