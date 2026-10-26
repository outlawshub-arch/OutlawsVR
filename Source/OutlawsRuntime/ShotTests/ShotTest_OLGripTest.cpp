// -OLShots -OLGripTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLGripTest) { return OLFlags::Param(TEXT("OLGripTest")); }

OL_SHOT_TEST(OLGripTest, 880)
{
	// (BUG-GRIPSAVE) -OLGripTest -OLGripStep=bake|write|read: bake: each gun in each hand at its baked grip (the left the
	// mirror); write: a revolver placement made in the right hand and written; read (the next start): it is back in the right
	// hand and mirrored in the left, then Reset grips deletes the file and the baked grip returns
	if (ShotStep != 0 || ShotTime < 4.f || !L) return;
	++ShotStep;
	FString Mode = TEXT("bake");
	OLFlags::Value(TEXT("OLGripStep"), Mode);
	L->GiveAllWeapons();
	auto Near = [](const FVector& A, const FVector& B) { return A.Equals(B, 0.011); };
	int32 Bad = 0;
	auto Check = [&](bool bOk, const FString& What)
	{
		Bad += bOk ? 0 : 1;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT grip test %s: %s: %s"), *Mode, bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
	};
	const FVector Placed(5.0, 1.0, 2.0);
	if (Mode == TEXT("bake"))
	{
		for (int32 Wi = 0; Wi < 8; ++Wi)
		{
			for (int32 H = 1; H >= 0; --H)
			{
				Equip(1 - H, -1);
				Equip(H, Wi);
				if (Equipped[H] != Wi) { UE_LOG(LogOutlaws, Display, TEXT("OLSHOT grip test bake: weapon %d not held in the %s hand"), Wi, H ? TEXT("right") : TEXT("left")); continue; }
				const FString Label = WeaponLabel(Wi);
				FVector Want;
				const bool bBaked = BakedGrip(TEXT("weapon:") + Label, Want);
				if (H == 0) Want.Y = -Want.Y;
				Check(bBaked && Near(GunBaseLoc[H], Want), FString::Printf(TEXT("%s %s hand GunBaseLoc %s, baked %s"), *Label, H ? TEXT("right") : TEXT("left"), *GunBaseLoc[H].ToString(), *Want.ToString()));
				Equip(H, -1);
			}
		}
	}
	else if (Mode == TEXT("write"))
	{
		Equip(1, 0);
		const FString Key = TEXT("weapon:") + WeaponLabel(0) + TEXT("@R");
		GripAdjust.Add(Key, FTransform(Placed));
		PlaceGrip(Key);
		Check(FPaths::FileExists(GripSavePath()), FString::Printf(TEXT("%s placed at %s and written to %s"), *Key, *Placed.ToString(), *GripSavePath()));
	}
	else
	{
		Equip(0, -1);
		Equip(1, 0);
		Check(Near(GunBaseLoc[1], Placed), FString::Printf(TEXT("after the restart the right revolver at %s (placed %s)"), *GunBaseLoc[1].ToString(), *Placed.ToString()));
		Equip(1, -1);
		Equip(0, 0);
		Check(Near(GunBaseLoc[0], FVector(Placed.X, -Placed.Y, Placed.Z)), FString::Printf(TEXT("and the left revolver mirrored at %s"), *GunBaseLoc[0].ToString()));
		ResetGrips();
		Equip(0, -1);
		Equip(1, 0);
		FVector Want;
		BakedGrip(TEXT("weapon:") + WeaponLabel(0), Want);
		Check(!FPaths::FileExists(GripSavePath()) && Near(GunBaseLoc[1], Want), FString::Printf(TEXT("Reset grips: the file gone, the right revolver back at %s"), *GunBaseLoc[1].ToString()));
	}
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT grip test %s done: %d failed"), *Mode, Bad);
	FGenericPlatformMisc::RequestExit(false);
	ShotTime = -1.f;
}

#endif // WITH_OUTLAWS_TESTS
