// -OLShots -OLVisCacheWalk[=seconds]: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLVisCacheWalk)
{
	return OLFlags::Has(TEXT("OLVisCacheWalk"));
}

OL_SHOT_TEST(OLVisCacheWalk, 1217)
{
	// (QSPEED-3) a walk over the level for the kept flood: every 6 s the body is put on another sector's floor (sectors
	// spread over the whole level), then for 1.5 s it stands while the head leans and turns round (12 cm each way), for
	// 3 s it walks and turns, and for 1.5 s it stands with the head all but still, as a player's in a headset (a drift of
	// 1.5 mm and a tremble of a tenth of one).  With -OLVisCacheCheck every tick that keeps the flood also floods afresh
	// and compares; the OLVISCACHE lines and the last one here give the counts (ticks, floods, kept, checked, differ),
	// and the floods per phase
	static float Seconds = -1.f, NextSpot = 3.f;
	static int32 Spot = 0, Spots = 0, LastPhase = -1;
	static FVector EyeBase = FVector::ZeroVector;
	static int64 PhaseTicks[3] = { 0, 0, 0 }, PhaseFloods[3] = { 0, 0, 0 }, AtTicks = 0, AtFloods = 0;
	if (Seconds < 0.f)
	{
		Seconds = 120.f;
		OLFlags::Value(TEXT("OLVisCacheWalk"), Seconds);
		EyeBase = Camera->GetRelativeLocation();
	}
	if (!L || ShotTime < 3.f) return;
	const int32 NumS = L->DebugNumSectors();
	if (ShotTime >= NextSpot && NumS > 0)
	{
		// the next sector, a stride that visits the level from end to end and wraps round
		const int32 Stride = FMath::Max(1, NumS / 19);
		for (int32 Tries = 0; Tries < 64; ++Tries)
		{
			const int32 Si = (Spot * Stride + Spot / 19) % NumS;
			++Spot;
			FVector Feet;
			float Yaw = 0.f;
			if (!L->SlopeFoot(Si, Feet, Yaw)) continue;
			TeleportTo(Feet + FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0), FRotator(0.0, Yaw, 0.0), false, true);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT viscache walk: spot %d, sector %d (player in %d)"), Spots, Si, L->PlayerSectorIndex());
			++Spots;
			break;
		}
		NextSpot = ShotTime + 6.f;
	}
	const double T = ShotTime;
	const double In = 6.0 - (NextSpot - T);           // seconds into this spot
	const int32 Phase = In < 1.5 ? 0 : In < 4.5 ? 1 : 2;
	{
		// the counts by phase (each phase's ticks and floods since it began)
		int64 Ticks = 0, Floods = 0;
		L->DebugVisCounts(Ticks, Floods);
		if (LastPhase >= 0)
		{
			PhaseTicks[LastPhase] += Ticks - AtTicks;
			PhaseFloods[LastPhase] += Floods - AtFloods;
		}
		AtTicks = Ticks;
		AtFloods = Floods;
		LastPhase = Phase;
	}
	if (Phase == 0)
	{
		Camera->SetRelativeLocation(EyeBase + FVector(12.0 * FMath::Sin(T * 0.9) + 0.3 * FMath::Sin(T * 23.0), 9.0 * FMath::Sin(T * 0.7) + 0.3 * FMath::Sin(T * 19.0), 3.0 * FMath::Sin(T * 1.9)));
		if (Controller) Controller->SetControlRotation(FRotator(0.0, 80.0 * FMath::Sin(T * 0.5) + 20.0 * T, 0.0));
	}
	else if (Phase == 1)
	{
		Camera->SetRelativeLocation(EyeBase + FVector(2.0 * FMath::Sin(T * 7.0), 2.0 * FMath::Sin(T * 5.0), 1.5 * FMath::Sin(T * 9.0)));      // (the walk's bob)
		if (Controller) Controller->SetControlRotation(FRotator(0.0, 80.0 * FMath::Sin(T * 0.5) + 20.0 * T, 0.0));
		AddMovementInput(FRotator(0.0, 70.0 * FMath::Sin(T * 0.6) + 25.0 * T, 0.0).Vector(), 0.7f);
	}
	else
	{
		Camera->SetRelativeLocation(EyeBase + FVector(0.15 * FMath::Sin(T * 3.1) + 0.01 * FMath::Sin(T * 131.0), 0.12 * FMath::Sin(T * 2.3) + 0.01 * FMath::Sin(T * 97.0), 0.05 * FMath::Sin(T * 1.7)));
	}
	if (T < 3.0 + Seconds) return;
	Camera->SetRelativeLocation(EyeBase);
	auto Pc = [](int64 Tk, int64 Fl) { return Tk > 0 ? 100.0 * double(Tk - Fl) / double(Tk) : 0.0; };
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT viscache walk done: %d spots; kept: looking round %.1f%% of %lld ticks, walking %.1f%% of %lld, head all but still %.1f%% of %lld; OLVISCACHE %s"), Spots,
		Pc(PhaseTicks[0], PhaseFloods[0]), PhaseTicks[0], Pc(PhaseTicks[1], PhaseFloods[1]), PhaseTicks[1], Pc(PhaseTicks[2], PhaseFloods[2]), PhaseTicks[2], *L->DebugVisCacheStats());
	FGenericPlatformMisc::RequestExit(false);
	ShotTime = -1.f;
	Seconds = 1e9f;
}

#endif // WITH_OUTLAWS_TESTS
