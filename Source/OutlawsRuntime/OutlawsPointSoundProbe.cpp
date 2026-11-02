// BUG-VILLADRIP (user 3 Nov, the user's snapshots #7, #8 and #9: the underground's drips heard all over the villa): -OLShots
// -OLLevel=<level> -OLNoFrontEnd -OLPointSoundProbe [-OLOldPointSound].  Every POINT_SOUND generator of the level listed (wav,
// sector, place); on the villa first the player at the three snapshots' spots: no drip (drip2, drip3) may play there; then the
// player at each generator's own spot, in its own sector: it plays there (full, or half with no clear line), as the original's
// does.  Lines "OLSHOT pointsound check PASS|FAIL: ..." and "OLSHOT pointsound RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLPointSoundProbe) { return OLFlags::Param(TEXT("OLPointSoundProbe")); }

OL_SHOT_TEST(OLPointSoundProbe, 1690)
{
	static int32 Passes = 0, Fails = 0, Spot = 0;
	static float Wait = 0.f;
	struct FSpot { FString What; FVector At; float Yaw = 0.f; int32 Gen = -1; };
	static TArray<FSpot> Spots;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT pointsound check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	static const TCHAR* GateName[] = { TEXT("silent"), TEXT("half"), TEXT("full") };
	auto Gate = [](int32 G) { return G >= 0 && G <= 2 ? GateName[G] : TEXT("?"); };
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !bDeadView;
	const bool bOld = OLFlags::Param(TEXT("OLOldPointSound"));
	const bool bVilla = L->LevelName.Equals(TEXT("villa"), ESearchCase::IgnoreCase);
	APlayerController* PC = Cast<APlayerController>(Controller);
	auto Ear = [PC, this]() { return PC && PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : GetActorLocation(); };
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 4.f || !bUp) return;
		L->bInvincible = true;
		const int32 N = L->DebugPointSoundCount();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT pointsound %s: %d point sound generators%s"), *L->LevelName, N, bOld ? TEXT(" (old rule: -OLOldPointSound)") : TEXT(""));
		Spots.Reset();
		if (bVilla)
		{
			// the snapshots' spots (info.txt: the player's UE location and look)
			Spots.Add({ TEXT("snapshot #7"), FVector(8452.307, -1479.213, 96.500), 72.99f });
			Spots.Add({ TEXT("snapshot #8"), FVector(7077.132, -998.842, 88.939), 37.79f });
			Spots.Add({ TEXT("snapshot #9"), FVector(8790.085, 1314.088, 88.945), -170.69f });
		}
		for (int32 i = 0; i < N; ++i)
		{
			FString Wav;
			int32 Sector = -1, Plays = 0;
			FVector Pos;
			L->DebugPointSound(i, Ear(), Wav, Sector, Pos, Plays);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT pointsound gen %d: %s in sector %d at (%.0f, %.0f, %.0f)"), i, *Wav, Sector, Pos.X, Pos.Y, Pos.Z);
			Spots.Add({ FString::Printf(TEXT("gen %d (%s) at its own spot"), i, *Wav), Pos + FVector(0.0, 0.0, 100.0), 0.f, i });
		}
		if (bVilla) Check(N >= 2, FString::Printf(TEXT("the villa has its point sounds (%d)"), N));
		else Check(true, FString::Printf(TEXT("%s's point sounds listed (%d)"), *L->LevelName, N));
		Spot = 0;
		ShotStep = Spots.Num() > 0 ? 1 : 3;
		return;
	}
	case 1:
		TeleportTo(Spots[Spot].At, FRotator(0.0, Spots[Spot].Yaw, 0.0), false, true);
		if (Controller) Controller->SetControlRotation(FRotator(0.0, Spots[Spot].Yaw, 0.0));
		Wait = ShotTime + 1.5f;          // (the eye's portal flood settled where it now stands)
		ShotStep = 2;
		return;
	case 2:
	{
		if (ShotTime < Wait) return;
		const FSpot& S = Spots[Spot];
		const FVector At = Ear();
		const int32 Feet = L->CullSectorOf(GetActorLocation());
		if (S.Gen < 0)
		{
			// a snapshot's spot: every drip silent (the old rule: by distance, they play)
			FString Said;
			int32 Drips = 0, Heard = 0;
			for (int32 i = 0; i < L->DebugPointSoundCount(); ++i)
			{
				FString Wav;
				int32 Sector = -1, Plays = 0;
				FVector Pos;
				const int32 G = L->DebugPointSound(i, At, Wav, Sector, Pos, Plays);
				Said += FString::Printf(TEXT(" %s(s%d, %.0f m): %s;"), *Wav, Sector, FVector::Dist(Pos, At) / 100.0, Gate(G));
				if (!Wav.StartsWith(TEXT("drip"), ESearchCase::IgnoreCase)) continue;
				++Drips;
				Heard += G > 0 ? 1 : 0;
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT pointsound %s (feet sector %d):%s"), *S.What, Feet, *Said);
			if (bOld) Check(Heard > 0, FString::Printf(TEXT("old rule: at %s a drip plays (%d of %d)"), *S.What, Heard, Drips));
			else Check(Drips > 0 && Heard == 0, FString::Printf(TEXT("at %s no drip plays (%d of %d would)"), *S.What, Heard, Drips));
		}
		else
		{
			FString Wav;
			int32 Sector = -1, Plays = 0;
			FVector Pos;
			const int32 G = L->DebugPointSound(S.Gen, At, Wav, Sector, Pos, Plays);
			Check(G > 0, FString::Printf(TEXT("%s: %s (the ear in sector %d, its sector %d, %.1f m off, %d sounds started so far)"), *S.What, Gate(G), Feet, Sector, FVector::Dist(Pos, At) / 100.0, Plays));
		}
		if (++Spot < Spots.Num()) { ShotStep = 1; return; }
		ShotStep = 3;
		return;
	}
	case 3:
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT pointsound RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
