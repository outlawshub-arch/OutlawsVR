// -OLShots -OLFireTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLFireTest) { return OLFlags::Param(TEXT("OLFireTest")); }

OL_SHOT_TEST(OLFireTest, 510)
{
	// the revolver fired as fast as it lets for 3 s, then emptied and reloaded, at the movement speed
	// slider's top and bottom: the counts must match (the slider slows only walking and the bad guys)
	static int32 Shots = 0;
	static double Start = 0.0;
	const float Speeds[2] = { 0.75f, 0.1f };
	const int32 Phase = ShotStep / 2;
	if (Phase >= 2)
	{
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
		return;
	}
	if (ShotStep % 2 == 0 && ShotTime > 3.f + Phase * 5.f)
	{
		if (L) { L->GameSpeed = Speeds[Phase]; L->GiveAllWeapons(); }
		Equip(1, 0);
		Shots = 0;
		Start = FPlatformTime::Seconds();
		++ShotStep;
	}
	else if (ShotStep % 2 == 1)
	{
		// the gun timers (fire cooldown, reload) run on this actor's tick time: what it gets per real second
		static double PlayerTime = 0.0;
		if (Shots == 0) PlayerTime = 0.0;
		const float TickDt = GetWorld()->GetDeltaSeconds() * GetActorTimeDilation();
		PlayerTime += TickDt;
		FireCooldown[1] = FMath::Max(0.f, FireCooldown[1] - TickDt);     // (as UpdateGuns does in VR)
		if (FPlatformTime::Seconds() - Start < 3.0)
		{
			if (FireCooldown[1] <= 0.f)
			{
				if (Mag[1] <= 0) Mag[1] = 6;
				FireGun(1, false);
			}
			++Shots;
		}
		else
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLRT fire test: movement speed %.0f%%: the player's clock ran %.2f s in 3.00 real s (world dilation %.2f, player %.2f)"),
				Speeds[Phase] * 100.f, PlayerTime, GetWorld()->GetWorldSettings()->TimeDilation, CustomTimeDilation);
			// the bad guys' own floor: their dilation must read max(slider, EnemyPaceFloor) (user 2026-09-29: 30 %)
			int32 Idle = 0, Seen = 0;
			float EnemyDilation = -1.f;
			for (TActorIterator<AOutlawsEnemy> It(GetWorld()); It; ++It)
			{
				++Seen;
				if (!It->IsDead() && It->CustomTimeDilation < 0.999f) { ++Idle; EnemyDilation = It->CustomTimeDilation; }
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLRT fire test: movement speed %.0f%%: player walk %.0f cm/s, enemy pace %.2f (%d of %d enemies dilated, sample %.2f; expect %.2f)"),
				Speeds[Phase] * 100.f, GetCharacterMovement()->MaxWalkSpeed, L ? L->EnemyPace() : -1.f, Idle, Seen, EnemyDilation,
				L ? FMath::Max(Speeds[Phase], L->EnemyPaceFloor) : -1.f);
			++ShotStep;
		}
	}
}

#endif // WITH_OUTLAWS_TESTS
