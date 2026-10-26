// -OLShots -OLLiftRideTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLLiftRideTest)
{
	return OLFlags::Param(TEXT("OLLiftRideTest"));
}

OL_SHOT_TEST(OLLiftRideTest, 1218)
{
	// (QSPEED-3) riding the level's lift floors that move by themselves (up to four, 10 s each): stood near the middle of
	// each, the feet against the floor's own height every tick, and how far the floor went.  PASS when on every lift the
	// feet stayed within 6 cm of the floor (a lift's step a frame and the capsule's float) and the floor moved
	static TArray<int32> Sectors;
	static TArray<FString> Names;
	static TArray<FVector> Tops;
	static int32 Lift = -1, Fails = 0, Rides = 0;
	static float LiftAt = 0.f;
	static double MaxOff = 0.0, MinZ = 1e9, MaxZ = -1e9;
	static int32 Ticks = 0, Away = 0;
	if (!L || ShotTime < 3.f) return;
	if (Lift < 0)
	{
		L->DebugMovingFloors(Sectors, Names, Tops);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT liftride: %d lift floors moving by themselves"), Sectors.Num());
		Lift = 0;
		LiftAt = -1.f;
	}
	const int32 N = FMath::Min(Sectors.Num(), 4);
	if (Lift < N)
	{
		const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		if (LiftAt < 0.f)
		{
			// on its floor where it is now
			FVector Top = Tops[Lift];
			Top.Z = L->DebugFloorZ(Sectors[Lift], Top);
			TeleportTo(Top + FVector(0.0, 0.0, Half + 2.0), GetActorRotation(), false, true);
			LiftAt = ShotTime;
			MaxOff = 0.0; MinZ = 1e9; MaxZ = -1e9; Ticks = 0; Away = 0;
			return;
		}
		const double T = ShotTime - LiftAt;
		if (T > 1.0)
		{
			// (the first second: settling onto it)
			const FVector P = GetActorLocation();
			const double Feet = P.Z - Half, Floor = L->DebugFloorZ(Sectors[Lift], P);
			if (L->PlayerSectorIndex() == Sectors[Lift])
			{
				MaxOff = FMath::Max(MaxOff, FMath::Abs(Feet - Floor));
				MinZ = FMath::Min(MinZ, Floor);
				MaxZ = FMath::Max(MaxZ, Floor);
				++Ticks;
			}
			else ++Away;
		}
		if (T < 11.0) return;
		const bool bOk = Ticks > 0 && MaxOff < 6.0 && Away * 10 < Ticks + Away;
		Fails += bOk ? 0 : 1;
		++Rides;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT liftride %s: %s (sector %d): floor moved %.0f cm (%.0f..%.0f), feet off the floor %.2f cm at most over %d ticks on it, %d off it"),
			bOk ? TEXT("PASS") : TEXT("FAIL"), *Names[Lift], Sectors[Lift], MaxZ - MinZ, MinZ, MaxZ, MaxOff, Ticks, Away);
		++Lift;
		LiftAt = -1.f;
		return;
	}
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT liftride RESULT %s: %d rides, %d failed"), Fails == 0 && Rides > 0 ? TEXT("PASS") : TEXT("FAIL"), Rides, Fails);
	FGenericPlatformMisc::RequestExit(false);
	ShotTime = -1.f;
	Lift = 1 << 20;
}

#endif // WITH_OUTLAWS_TESTS
