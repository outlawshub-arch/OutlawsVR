// -OLShots -OLKeyTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLKeyTest) { return OLFlags::Param(TEXT("OLKeyTest")); }

OL_SHOT_TEST(OLKeyTest, 650)
{
	// cliff: the three stone keys pushed into their slots: the slots' ATX frames, the DUMKEY chain, the stone door
	// (user 2026-09-29: the keys could not be used)
	auto Shot = [](const TCHAR* Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(Name) + TEXT(".png"), false, false); };
	auto Report = [L](const TCHAR* When)
	{
		if (!L) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT keys %s: %s"), When, *L->DebugElevatorReport(TEXT("DUMKEY")));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT keydoor: %s | md: %s"), *L->DebugElevatorReport(TEXT("KEYDOOR")), *L->DebugElevatorReport(TEXT("MD")));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT atx: %s"), *L->DebugAtxReport());
	};
	static const TCHAR* Walls[3] = { TEXT("8E65"), TEXT("8E66"), TEXT("8E67") };
	if (ShotStep == 0 && ShotTime > 3.f && L)
	{
		for (const TCHAR* K : { TEXT("isqrkeya"), TEXT("isqrkeyc"), TEXT("isqrkeye") }) L->DebugGiveListed(K, K);
		FVector Spot, Wall;
		if (L->DebugSectorSpot(TEXT("KEYCODE"), Spot))
		{
			TeleportTo(Spot + FVector(0.0, 0.0, 92.0), GetActorRotation(), false, true);
			if (L->DebugWallSpot(TEXT("KEYCODE"), Walls[1], Wall) && Controller) Controller->SetControlRotation((Wall - (Spot + FVector(0.0, 0.0, 160.0))).Rotation());
		}
		Report(TEXT("before"));
		Shot(TEXT("ol_keys_before"));
		++ShotStep;
	}
	else if (ShotStep >= 1 && ShotStep <= 3 && ShotTime > 3.f + ShotStep * 1.5f)
	{
		// the first slot by a hand as the player would push it (from inside the sector, at the wall's middle); the
		// others by the nudge itself
		bool bTaken = false;
		if (ShotStep == 1 && L)
		{
			FVector Wall, Spot;
			if (L->DebugWallSpot(TEXT("KEYCODE"), Walls[0], Wall) && L->DebugSectorSpot(TEXT("KEYCODE"), Spot))
			{
				const FVector In = FVector(Spot.X - Wall.X, Spot.Y - Wall.Y, 0.0).GetSafeNormal();
				bTaken = L->UseByHand(Wall + In * 100.0 + FVector(0.0, 0.0, 30.0), Wall + In * 5.0, true);
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT key wall %s pushed by hand (player sector %d): taken %d"), Walls[0], L->PlayerSectorIndex(), bTaken ? 1 : 0);
			}
		}
		if (!bTaken) bTaken = L && L->DebugNudgeWall(TEXT("KEYCODE"), Walls[ShotStep - 1]);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT key wall %s pushed: taken %d"), Walls[ShotStep - 1], bTaken ? 1 : 0);
		Report(TEXT("after the push"));
		++ShotStep;
	}
	else if (ShotStep == 4 && ShotTime > 10.f) { Report(TEXT("10 s")); Shot(TEXT("ol_keys_after")); ++ShotStep; }
	else if (ShotStep == 5 && ShotTime > 22.f)
	{
		Report(TEXT("22 s"));
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
