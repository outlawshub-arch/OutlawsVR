// -OLShots -OLTrainBodyTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLTrainBodyTest) { return OLFlags::Param(TEXT("OLTrainBodyTest")); }

OL_SHOT_TEST(OLTrainBodyTest, 920)
{
	// (BUG-TRAINBODY) -OLLevel=train -OLShots -OLTrainBodyTest: a living bad guy (held to his place) and a body stood on the open
	// moving ground beside the train, 3 m apart; after a second to settle, how far each goes in the next second against the
	// ground's own speed. The deadly floor is held off for the measure (else the living one dies on it), then let back on.
	static TWeakObjectPtr<AOutlawsEnemy> Man, Body;
	static FVector ManStart = FVector::ZeroVector, BodyStart = FVector::ZeroVector;
	static float NextAt = 0.f;
	if (ShotStep == 0 && ShotTime > 5.f && L)
	{
		FVector Ground;
		if (L->DebugMovingGround(GetActorLocation(), Ground, 2))
		{
			// off the train side of the strip, onto the open ground (away from the player, who is on the train)
			Ground.Y += (Ground.Y > GetActorLocation().Y ? 1.0 : -1.0) * 300.0;
			TArray<AOutlawsEnemy*, TInlineAllocator<64>> Near;
			for (TActorIterator<AOutlawsEnemy> It(GetWorld()); It; ++It)
			{
				if (!It->IsDead() && !It->bBoss && !It->bVoiceOnly && !It->IsCritter()) Near.Add(*It);
			}
			const FVector Me = GetActorLocation();
			Near.Sort([&Me](const AOutlawsEnemy& A, const AOutlawsEnemy& B) { return FVector::DistSquared(A.GetActorLocation(), Me) < FVector::DistSquared(B.GetActorLocation(), Me); });
			if (Near.Num() >= 2)
			{
				L->bFloorKillsOff = true;
				Man = Near[0];
				Body = Near[1];
				Body->SetDormant(false);
				const FVector BAt = Body->GetActorLocation();
				// (killed without a push: a shot throws the body, toward a drop near it too; the floor's own kill does not)
				Body->FloorKill(BAt);
				Man->SetDormant(false);
				Man->bStationary = true;
				for (int32 i = 0; i < 2; ++i)
				{
					AOutlawsEnemy* E = i == 0 ? Man.Get() : Body.Get();
					E->GetCharacterMovement()->StopMovementImmediately();
					E->SetActorLocation(Ground + FVector(i == 0 ? 150.0 : -150.0, 0.0, 2.0 + E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()), false, nullptr,
						ETeleportType::TeleportPhysics);
				}
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT train body test: %s (living) and %s (dead %d) stood on the moving ground at %s (it carries at %.0f cm/s)"),
					*Man->GetOutlawsName(), *Body->GetOutlawsName(), Body->IsDead() ? 1 : 0, *Ground.ToCompactString(), L->FloorVelocityAt(Ground + FVector(0.0, 0.0, 5.0)).Size());
			}
		}
		if (!Man.IsValid() || !Body.IsValid())
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT train body test: no open moving ground, or not two bad guys"));
			FGenericPlatformMisc::RequestExit(false);
			ShotTime = -1.f;
			return;
		}
		++ShotStep;
		NextAt = ShotTime + 1.f;
	}
	else if (ShotStep == 1 && ShotTime >= NextAt)
	{
		ManStart = Man.IsValid() ? Man->GetActorLocation() : FVector::ZeroVector;
		BodyStart = Body.IsValid() ? Body->GetActorLocation() : FVector::ZeroVector;
		++ShotStep;
		NextAt = ShotTime + 1.f;
	}
	else if (ShotStep == 2 && ShotTime >= NextAt)
	{
		const float Span = 1.f + Dt;
		for (int32 i = 0; i < 2; ++i)
		{
			AOutlawsEnemy* E = i == 0 ? Man.Get() : Body.Get();
			if (!E || !L) continue;
			const FVector Vel = L->CarryVelocityOf(E);
			const FVector Moved = E->GetActorLocation() - (i == 0 ? ManStart : BodyStart);
			const double Along = Vel.IsNearlyZero() ? 0.0 : FVector::DotProduct(Moved, Vel.GetSafeNormal());
			const double Want = Vel.Size() * Span;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT train body test: the %s %s (dead %d, mode %d) moved %.0f cm along the ground (%.0f cm in all) in %.2f s; the ground carries at %.0f cm/s, %.0f cm wanted (%s)"),
				i == 0 ? TEXT("living") : TEXT("body"), *E->GetOutlawsName(), E->IsDead() ? 1 : 0, int32(E->GetCharacterMovement()->MovementMode), Along, Moved.Size(), Span,
				Vel.Size(), Want, Want > 1.0 && FMath::Abs(Along - Want) < 0.1 * Want ? TEXT("full speed") : TEXT("NOT the ground's speed"));
		}
		if (L) L->bFloorKillsOff = false;
		++ShotStep;
		NextAt = ShotTime + 0.6f;
	}
	else if (ShotStep == 3 && ShotTime >= NextAt)
	{
		if (AOutlawsEnemy* E = Man.Get())
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT train body test: the deadly floor back on, the living one (%s) dead %d"), *E->GetOutlawsName(), E->IsDead() ? 1 : 0);
		}
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
