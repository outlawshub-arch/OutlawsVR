// -OLShots -OLCylSideTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLCylSideTest) { return OLFlags::Param(TEXT("OLCylSideTest")); }

OL_SHOT_TEST(OLCylSideTest, 1140)
{
	// a revolver in each hand, both cylinders out: which side of its gun each swings to (right hand: its left)
	if (ShotStep == 0 && ShotTime > 3.f && L)
	{
		L->GiveAllWeapons();
		L->AddCount(TEXT("ipistol"), 1);
		Equip(0, 0);
		Equip(1, 0);
		bDebugCylOut = true;
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 5.f)
	{
		for (int32 H = 0; H < 2; ++H) UpdateCylinder(H, 1.f);      // (the hand update does not run headless)
		for (int32 H = 0; H < 2; ++H)
		{
			if (!CylMesh[H] || !WeaponMesh[H]) { UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cylside %s: no cylinder"), H == 0 ? TEXT("left") : TEXT("right")); continue; }
			const FVector Off = CylMesh[H]->GetComponentLocation() - WeaponMesh[H]->GetComponentLocation();
			const double Side = FVector::DotProduct(Off, FrameOf(H)->GetRightVector());
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cylside %s hand: cylinder %.2f cm to the gun's %s (out %.2f)"), H == 0 ? TEXT("left") : TEXT("right"),
				FMath::Abs(Side), Side > 0.0 ? TEXT("RIGHT") : TEXT("LEFT"), CylOut[H]);
		}
		// the rounds in the right-hand revolver's chambers: full, after two shots, after one round loaded
		auto Shown = [this]()
		{
			int32 N = 0;
			for (int32 k = 0; k < 6; ++k) if (RoundParts.Num() >= 24 && RoundParts[12 + k * 2] && RoundParts[12 + k * 2]->IsVisible()) ++N;
			return N;
		};
		bDebugCylOut = false;
		UpdateCylinder(1, 1.f);
		const int32 Full = Shown();
		FireCooldown[1] = 0.f; FireGun(1, false);
		FireCooldown[1] = 0.f; FireGun(1, false);
		UpdateCylinder(1, 1.f);
		const int32 AfterTwo = Shown();
		++Mag[1];
		CylSpinTarget[1] -= 60.f;
		UpdateCylinder(1, 1.f);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT rounds: %d shown full, %d after two shots (Mag %d), %d after one loaded (Mag %d)"),
			Full, AfterTwo, Mag[1] - 1, Shown(), Mag[1]);
		// a long gun in the right hand empties the left: no round of either revolver may stay in the air
		Equip(1, 1);
		for (int32 H = 0; H < 2; ++H) UpdateCylinder(H, 0.1f);
		int32 Lingering = 0;
		for (int32 i = 0; i < RoundParts.Num(); ++i) if (RoundParts[i] && RoundParts[i]->IsVisible()) ++Lingering;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT rounds after a rifle in the right hand: left %s, right %s, %d round parts still shown (expect 0)"),
			Equipped[0] < 0 ? TEXT("bare") : TEXT("armed"), Equipped[1] == 1 ? TEXT("rifle") : TEXT("?"), Lingering);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
