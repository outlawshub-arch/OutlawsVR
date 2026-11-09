// -OLShots -OLAimPickProbe (TEMP-AIMTUNE, 9 Nov; the user: the gun on someone with the head turned, the shot went to a further bad
// guy, the one nearest the middle of the view).  In a story level (bad guys are what the aim assist takes): two live bad guys put
// out in the open, NEAR 4 m along the gun's way and FAR 12 m along the head's look, the head turned 50 degrees from the gun.
// AimAssistPick (the shot's own) asked, no shot fired:
//   repro trigger: the bore on NEAR, FAR 5 degrees off the bore and in the middle of the view (old: FAR; new: NEAR)
//   repro fan:     the bore on NEAR, the head on FAR (old: FAR; new: NEAR)
//   table:         the bore swung off NEAR by 0..20 degrees (away from FAR), trigger and fan, at 0, 6 and 12 degrees
// Lines "OLSHOT aimpick <case>: <near|far|none> ...", checks "OLSHOT aimpick check PASS|FAIL: ...", "OLSHOT aimpick RESULT".

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLAimPickProbe) { return OLFlags::Param(TEXT("OLAimPickProbe")); }

OL_SHOT_TEST(OLAimPickProbe, 2580)
{
	static int32 Step = 0, Pass = 0, Fail = 0;
	static TWeakObjectPtr<AOutlawsEnemy> Near, Far;
	static FVector Muzzle = FVector::ZeroVector, GunWay = FVector::ZeroVector, HeadWay = FVector::ZeroVector;
	if (Step < 0 || !L) return;
	auto Check = [&](bool bOk, const FString& What)
	{
		(bOk ? Pass : Fail)++;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT aimpick check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
	};
	auto Look = [&](const FVector& Way)
	{
		const FRotator R = FRotator(0.f, float(Way.Rotation().Yaw), 0.f);
		if (Controller) Controller->SetControlRotation(Way.Rotation());
		SetActorRotation(R);
	};
	auto Name = [&](const AOutlawsEnemy* E) { return E == Near.Get() ? TEXT("near") : E == Far.Get() ? TEXT("far") : E ? TEXT("other") : TEXT("none"); };
	if (Step == 0)
	{
		if (ShotTime < 6.f) return;
		// an open spot: from the player, 8 ways tried, the first where both lines (4 m on one, 12 m 50 degrees round) are clear
		UWorld* W = GetWorld();
		const FVector Eye = Camera->GetComponentLocation();
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsAimProbe), true, this);
		bool bFound = false;
		for (int32 k = 0; k < 16 && !bFound; ++k)
		{
			const float Yaw = k * 22.5f;
			const FVector G = FRotator(0.f, Yaw, 0.f).Vector(), H = FRotator(0.f, Yaw + 50.f, 0.f).Vector();
			FHitResult Hit;
			if (W->LineTraceSingleByChannel(Hit, Eye, Eye + G * 500.0, ECC_Visibility, Q)) continue;
			if (W->LineTraceSingleByChannel(Hit, Eye, Eye + H * 1400.0, ECC_Visibility, Q)) continue;
			GunWay = G; HeadWay = H; bFound = true;
		}
		// the two bad guys: the first two living ones, moved there
		TArray<AOutlawsEnemy*> Live;
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies()) if (AOutlawsEnemy* E = Wk.Get()) if (!E->IsDead()) Live.Add(E);
		if (!bFound || Live.Num() < 2)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT aimpick: no open spot (%d) or too few bad guys (%d)"), bFound ? 1 : 0, Live.Num());
			Check(false, TEXT("an open spot and two bad guys"));
			Step = 9;
			return;
		}
		// every other bad guy out of the way (far under the level), so only these two can be picked
		for (int32 i = 2; i < Live.Num(); ++i) Live[i]->SetActorLocation(Live[i]->GetActorLocation() - FVector(0.0, 0.0, 100000.0), false, nullptr, ETeleportType::TeleportPhysics);
		Near = Live[0]; Far = Live[1];
		const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
		Muzzle = Eye + GunWay * 30.0 - FVector::UpVector * 20.0;
		auto Put = [&](AOutlawsEnemy* E, const FVector& Way, double Dist)
		{
			const FVector At = Feet + Way * Dist + FVector(0.0, 0.0, E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0);
			E->SetActorLocation(At, false, nullptr, ETeleportType::TeleportPhysics);
		};
		Put(Near.Get(), GunWay, 400.0);
		Put(Far.Get(), HeadWay, 1200.0);
		Step = 1;
		return;
	}
	if (Step == 1)
	{
		// the gun (the right hand, a revolver) on NEAR; the head first turned onto where FAR will stand for the trigger's repro
		// (5 degrees off the bore on NEAR, 12 m out), a frame given for the view to follow
		const FVector OnNear0 = (Near->GetActorLocation() + FVector(0.0, 0.0, 25.0) - Muzzle).GetSafeNormal();
		const FVector Side = FVector::CrossProduct(OnNear0, FVector::UpVector).GetSafeNormal();
		const FVector FarAt = Muzzle + (OnNear0 * FMath::Cos(FMath::DegreesToRadians(5.0)) + Side * FMath::Sin(FMath::DegreesToRadians(5.0))) * 1200.0;
		Far->SetActorLocation(FarAt - FVector(0.0, 0.0, 25.0), false, nullptr, ETeleportType::TeleportPhysics);
		Look(FarAt - Camera->GetComponentLocation());
		int32 Rev = -1;
		for (int32 i = 0; i < 32; ++i) if (WeaponLabel(i).Contains(TEXT("revolver"))) { Rev = i; break; }
		Equip(1, Rev);
		Step = 2;
		return;
	}
	if (Step == 2)
	{
		const FVector NearChest = Near->GetActorLocation() + FVector(0.0, 0.0, 25.0), FarChest = Far->GetActorLocation() + FVector(0.0, 0.0, 25.0);
		const FVector OnNear = (NearChest - Muzzle).GetSafeNormal();
		auto Pick = [&](const FVector& Bore, bool bFan) { FVector To; return AimAssistPick(1, Muzzle, Bore, bFan, To); };
		const bool bOld = OldAimPick();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT aimpick setup: %s pick, head %.0f deg from the gun, near %.0f cm, far %.0f cm, far %.1f deg off the view middle"),
			bOld ? TEXT("old (view)") : TEXT("new (bore)"), FMath::RadiansToDegrees(FMath::Acos(FVector::DotProduct(GunWay, HeadWay))),
			FVector::Dist(Muzzle, NearChest), FVector::Dist(Muzzle, FarChest),
			FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct((FarChest - Camera->GetComponentLocation()).GetSafeNormal(), Camera->GetForwardVector()), -1.0, 1.0))));
		// the repro, trigger: FAR 5 degrees off the bore on NEAR, 12 m out, the head on him (set a frame ago)
		{
			const AOutlawsEnemy* E = Pick(OnNear, false);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT aimpick repro trigger (bore on near, far 5 deg off it and mid view, %.1f deg off the view middle): %s"),
				FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct((FarChest - Camera->GetComponentLocation()).GetSafeNormal(), Camera->GetForwardVector()), -1.0, 1.0))), Name(E));
			Check(E == Near.Get(), FString::Printf(TEXT("repro trigger: the shot goes to the one the gun is on (got %s)"), Name(E)));
		}
		// then FAR back out 50 degrees round, 12 m, the head on him, a frame for the view
		Far->SetActorLocation(GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight()) + HeadWay * 1200.0
			+ FVector(0.0, 0.0, Far->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0), false, nullptr, ETeleportType::TeleportPhysics);
		Look(HeadWay);
		Step = 3;
		return;
	}
	if (Step == 3)
	{
		const FVector NearChest = Near->GetActorLocation() + FVector(0.0, 0.0, 25.0), FarChest = Far->GetActorLocation() + FVector(0.0, 0.0, 25.0);
		const FVector OnNear = (NearChest - Muzzle).GetSafeNormal();
		const float Was = AimTuneDeg;
		auto Pick = [&](const FVector& Bore, bool bFan) { FVector To; return AimAssistPick(1, Muzzle, Bore, bFan, To); };
		const bool bOld = OldAimPick();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT aimpick fan setup: far %.1f deg off the view middle, %.1f deg off the bore"),
			FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct((FarChest - Camera->GetComponentLocation()).GetSafeNormal(), Camera->GetForwardVector()), -1.0, 1.0))),
			FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct((FarChest - Muzzle).GetSafeNormal(), OnNear), -1.0, 1.0))));
		// the repro, fan: the bore on NEAR, the head 50 degrees round on FAR
		{
			const AOutlawsEnemy* E = Pick(OnNear, true);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT aimpick repro fan (bore on near, head on far): %s"), Name(E));
			Check(E == Near.Get(), FString::Printf(TEXT("repro fan: the fanned shot goes to the one the gun is on (got %s)"), Name(E)));
		}
		// the table (the new pick only: the old one has no slider)
		if (!bOld)
		{
			const FVector Away = -FVector::CrossProduct(OnNear, FVector::UpVector).GetSafeNormal() * (FVector::DotProduct(FVector::CrossProduct(OnNear, FVector::UpVector), HeadWay) > 0.0 ? 1.0 : -1.0);
			const float Settings[] = { 0.f, 6.f, 12.f };
			const int32 Offs[] = { 0, 2, 4, 6, 8, 10, 12, 14, 20, 26 };
			for (float S : Settings)
			{
				AimTuneDeg = S;
				for (int32 Fan = 0; Fan < 2; ++Fan)
				{
					FString Row;
					for (int32 O : Offs)
					{
						const FVector Bore = (OnNear * FMath::Cos(FMath::DegreesToRadians(double(O))) + Away * FMath::Sin(FMath::DegreesToRadians(double(O)))).GetSafeNormal();
						const AOutlawsEnemy* E = Pick(Bore, Fan == 1);
						Row += FString::Printf(TEXT(" %d:%s"), O, Name(E));
						const float Cone = AimBoreDeg() * (Fan ? 2.f : 1.f);
						const bool bWant = O <= Cone - 0.5f ? E == Near.Get() : (O >= Cone + 0.5f ? E == nullptr : true);
						if (!bWant) Check(false, FString::Printf(TEXT("table %s at %.0f deg, bore %d deg off: %s"), Fan ? TEXT("fan") : TEXT("trigger"), S, O, Name(E)));
					}
					UE_LOG(LogOutlaws, Display, TEXT("OLSHOT aimpick table %s, slider %.0f deg (cone %.0f):%s"), Fan ? TEXT("fan    ") : TEXT("trigger"), S,
						AimBoreDeg() * (Fan ? 2.f : 1.f), *Row);
				}
			}
			Check(true, TEXT("the table as wanted (any miss is a FAIL line above)"));
			AimTuneDeg = Was;
		}
		Step = 9;
		return;
	}
	if (Step == 9)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT aimpick RESULT %d PASS, %d FAIL"), Pass, Fail);
		Step = -1;
		FGenericPlatformMisc::RequestExit(false);
	}
}

#endif // WITH_OUTLAWS_TESTS
