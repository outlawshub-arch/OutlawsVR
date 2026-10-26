// -OLShots -OLTrainFallTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLTrainFallTest) { return OLFlags::Param(TEXT("OLTrainFallTest")); }

OL_SHOT_TEST(OLTrainFallTest, 930)
{
	// (BUG-TRAINFALL / BUG-TRAINBODY) -OLLevel=train -OLShots -OLTrainFallTest: a living bad guy beside a drop off the train
	// pushed off it; where he is, how fast, his health, four times a second for six seconds (and the landing's own line);
	// then shot dead where he lies, how far the body goes in a second against the ground's own speed
	static TWeakObjectPtr<AOutlawsEnemy> Man;
	static float NextLog = 0.f;
	static FVector BodyStart = FVector::ZeroVector;
	if (ShotStep == 0 && ShotTime > 5.f && L)
	{
		// the nearest living bad guy, put 4 m over the middle of the open moving ground nearest the player (the train's roofs stand
		// 4 to 7 m over it; the strips beside the cars are 6.75 m high) going sideways at 3 m/s, as one knocked off a car is
		FVector Ground;
		double Near = 1e30;
		if (L->DebugMovingGround(GetActorLocation(), Ground, 2))
		{
			for (TActorIterator<AOutlawsEnemy> It(GetWorld()); It; ++It)
			{
				if (It->IsDead() || It->bBoss || It->bVoiceOnly || It->IsCritter()) continue;
				const double D = FVector::Dist(It->GetActorLocation(), GetActorLocation());
				if (D < Near) { Near = D; Man = *It; }
			}
		}
		if (AOutlawsEnemy* E = Man.Get())
		{
			E->SetDormant(false);
			E->SetActorLocation(Ground + FVector(0.0, 0.0, 400.0 + E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()), false, nullptr, ETeleportType::TeleportPhysics);
			E->LaunchCharacter(FVector(0.0, 300.0, 0.0), true, true);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT train fall test: %s dropped from 4 m onto the moving ground at %s (it carries at %.0f cm/s), health %.1f"), *E->GetOutlawsName(),
				*Ground.ToCompactString(), L->FloorVelocityAt(Ground + FVector(0.0, 0.0, 5.0)).Size(), E->GetHealth());
		}			if (!Man.IsValid()) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT train fall test: no moving ground beside the player, or no bad guy"));
		++ShotStep;
		NextLog = ShotTime;
	}
	else if (ShotStep == 1 && ShotTime >= NextLog)
	{
		NextLog = ShotTime + 0.25f;
		if (AOutlawsEnemy* E = Man.Get())
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT train fall test %.2f s: at %s, velocity %s, mode %d, health %.1f, dead %d, carried %d, dilation %.2f"), ShotTime - 5.f,
				*E->GetActorLocation().ToCompactString(), *E->GetVelocity().ToCompactString(), int32(E->GetCharacterMovement()->MovementMode), E->GetHealth(), E->IsDead() ? 1 : 0,
				L && L->IsCarriedBySector(E) ? 1 : 0, E->CustomTimeDilation);
		}
		if (ShotTime > 11.f)
		{
			++ShotStep;
			if (AOutlawsEnemy* E = Man.Get())
			{
				const FVector At = E->GetActorLocation();
				for (int32 k = 0; k < 60 && !E->IsDead(); ++k) E->TakeHit(50.f, At, FVector(0.0, 0.0, -1.0), At + FVector(0.0, 0.0, 300.0));
			}
			NextLog = ShotTime + 2.f;
		}
	}
	else if (ShotStep == 2 && ShotTime >= NextLog)
	{
		BodyStart = Man.IsValid() ? Man->GetActorLocation() : FVector::ZeroVector;
		++ShotStep;
		NextLog = ShotTime + 1.f;
	}
	else if (ShotStep == 3 && ShotTime >= NextLog)
	{
		if (AOutlawsEnemy* E = Man.Get())
		{
			const FVector Vel = L ? L->CarryVelocityOf(E) : FVector::ZeroVector;
			const double Moved = FVector::Dist2D(E->GetActorLocation(), BodyStart);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT train body test: the body (dead %d) moved %.0f cm in %.2f s; the ground carries at %.0f cm/s, pace %.2f (%s)"), E->IsDead() ? 1 : 0, Moved, 1.f + Dt,
				Vel.Size(), L ? L->Pace() : 0.f, Vel.Size() > 1.0 && FMath::Abs(Moved - Vel.Size()) < 0.1 * Vel.Size() ? TEXT("full speed") : TEXT("NOT the ground's speed"));
		}
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
