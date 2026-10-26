// -OLShots -OLFanTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLFanTest) { return OLFlags::Param(TEXT("OLFanTest")); }

OL_SHOT_TEST(OLFanTest, 890)
{
	// (REQ-FANPALM) -OLShots -OLFanTest: the revolver in the right hand, the left free and raking back over the hammer, palm
	// down (a fan shot) and palm up, cupped (no fan); each logged with the palm's facing and the rounds
	if (ShotStep == 0 && ShotTime > 4.f && L)
	{
		++ShotStep;
		L->GiveAllWeapons();
		Equip(0, -1);
		Equip(1, 0);
		const FWeaponDef_Public Def = WeaponFanZone(0);
		for (int32 Pass = 0; Pass < 3; ++Pass)
		{
			const bool bUp = Pass == 1;
			Mag[1] = 6;
			FireCooldown[1] = 0.f;
			const FTransform GunXf = WeaponMesh[1]->GetComponentTransform();
			const FVector Fwd = GunXf.GetUnitAxis(EAxis::X);
			const FVector Zone = GunXf.TransformPosition(Def.Hammer + FVector(-4.0, 0.0, -2.0));
			FrameOf(0)->SetWorldRotation(FRotationMatrix::MakeFromYX(bUp ? FVector::UpVector : -FVector::UpVector, Fwd).Rotator());
			FrameOf(0)->AddWorldOffset(Zone - GripOf(0)->GetComponentLocation());
			HandVelocity[0] = -Fwd * 220.0;
			HandVelocity[1] = FVector::ZeroVector;
			const int32 Before = Mag[1];
			const bool bFan = UpdateFan(1, false);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT fan test: palm %s (facing up %.2f, held up %d), raked back over the hammer at 220 cm/s: rounds %d -> %d (%s)"), bUp ? TEXT("up, cupped") : TEXT("down"),
				FVector::DotProduct(PalmDir(0), FVector::UpVector), bFanPalmUp[0] ? 1 : 0, Before, Mag[1], bFan ? TEXT("fanned") : TEXT("no fan"));
		}
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
