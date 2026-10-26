// -OLShots -OLKnifeTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLKnifeTest) { return OLFlags::Param(TEXT("OLKnifeTest")); }

OL_SHOT_TEST(OLKnifeTest, 900)
{
	// (REQ-KNIFESTAB) -OLShots -OLKnifeTest: the knife in the right hand, the grip let go; a bad guy put before the player, the
	// hand reaching at him; a slow push (no stab), then a fast swing (a stab, the knife's damage)
	static TWeakObjectPtr<AOutlawsEnemy> Man;
	if (ShotStep == 0 && ShotTime > 4.f && L)
	{
		L->GiveAllWeapons();
		Equip(0, -1);
		Equip(1, 7);
		Mag[1] = 1;
		const FVector Eye = Camera->GetComponentLocation();
		const FVector F = FVector(Camera->GetForwardVector().X, Camera->GetForwardVector().Y, 0.0).GetSafeNormal();
		for (TActorIterator<AOutlawsEnemy> It(GetWorld()); It && !Man.IsValid(); ++It)
		{
			if (It->IsDead() || It->bBoss || It->bVoiceOnly || It->IsCritter()) continue;
			Man = *It;
			It->SetDormant(false);
			It->SetActorLocation(FVector(Eye.X, Eye.Y, GetActorLocation().Z) + F * 75.0 + FVector(0.0, 0.0, It->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() - GetCapsuleComponent()->GetScaledCapsuleHalfHeight()),
				false, nullptr, ETeleportType::TeleportPhysics);
			It->GetCharacterMovement()->DisableMovement();
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT knife test: holding %s, %s put 75 cm ahead"), Equipped[1] == 7 ? *WeaponLabel(7) : TEXT("NOT the knife"), Man.IsValid() ? *Man->GetOutlawsName() : TEXT("nobody"));
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 5.f)
	{
		AOutlawsEnemy* E = Man.Get();
		const FVector Eye = Camera->GetComponentLocation();
		const FVector F = FVector(Camera->GetForwardVector().X, Camera->GetForwardVector().Y, 0.0).GetSafeNormal();
		if (E && L)
		{
			const FVector Body = E->GetActorLocation();
			// the hand at his chest, 10 cm short of his middle, the blade pointing at him
			FrameOf(1)->SetWorldLocationAndRotation(FVector(Body.X, Body.Y, Body.Z + 20.0) - F * 25.0, F.Rotation());
			for (int32 Pass = 0; Pass < 2; ++Pass)
			{
				const float Speed = Pass == 0 ? OutlawsFeel::HitSpeedHard * 0.5f : OutlawsFeel::HitSpeedHard + 150.f;
				HandVelocity[1] = F * Speed;
				HandWorldVel[1] = F * Speed;
				KnifeCool[1] = 0.f;
				const float Before = E->GetHealth();
				UpdateThrowable(1, 0.f, 0.016f);
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT knife test: a %s swing at %.0f cm/s, grip %.2f: health %.2f -> %.2f (%s)"), Pass == 0 ? TEXT("slow") : TEXT("fast"), Speed,
					Axis(TEXT("OLVR_RightGrip")), Before, E->GetHealth(), E->GetHealth() < Before ? TEXT("stabbed") : TEXT("no stab"));
			}
		}
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
