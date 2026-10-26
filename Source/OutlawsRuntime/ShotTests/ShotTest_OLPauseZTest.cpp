// -OLShots -OLPauseZTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLPauseZTest) { return OLFlags::Param(TEXT("OLPauseZTest")); }

OL_SHOT_TEST(OLPauseZTest, 1216)
{
	// -OLLevel=<level> -OLNoFrontEnd -OLShots -OLPauseZTest [-OLOldPauseZ] (BUG-PAUSEZ): the headset's body (bVR, the head
	// 170 cm over the play-space floor) on a high floor: the user's simms roof spot (sector 322, feet X 4686.83 Y 352.59
	// Z 722, simms only) and the level's highest floor with room to stand.  At each, the menu opens and closes as the Menu
	// button does (from input, before this tick, PressMenu), twice: standing still, and with the stick crouch's view ride in
	// motion.  The feet must not move; the eye never more than 5 cm in a frame, nor off a still head's height by more than
	// 1 cm (still), and inside the body's height range (riding).
	constexpr float CrouchEyeZ = 70.f;    // (OutlawsPlayer.cpp's CrouchEye; a standing body's half height there is 87)
	struct FSpot { FVector Feet; FString What; bool bCandidate; };
	static TArray<FSpot> Spots;
	static int32 SpotI = 0, Case = 0, Fails = 0, Runs = 0;
	static float CaseT = 0.f;
	static FVector Feet0, Eye0, PrevEye;
	static double MaxFeet = 0.0, MaxStep = 0.0, MaxOff = 0.0, MinUp = 1e9, MaxUp = -1e9;
	static bool bArmed = false, bStarted = false;
	static int32 PreTickAsk = 0;          // 1: open the menu from the next pre-tick, 2: close it
	if (ShotStep == 0 && ShotTime > 3.f)
	{
		FWorldDelegates::OnWorldPreActorTick.AddLambda([Self = TWeakObjectPtr<AOutlawsPlayer>(this)](UWorld* W, ELevelTick, float)
		{
			AOutlawsPlayer* P = Self.Get();
			if (PreTickAsk == 0 || !P || W != P->GetWorld()) return;
			const bool bOpen = PreTickAsk == 1;
			PreTickAsk = 0;
			if (bOpen != P->bMenuOpen) P->PressMenu();      // (the Menu button's own path, before the actors tick)
		});
		const float Half = 87.f, R = GetCapsuleComponent()->GetUnscaledCapsuleRadius();
		if (L->LevelName.Equals(TEXT("simms"), ESearchCase::IgnoreCase)) Spots.Add({ FVector(4686.83, 352.59, 722.0), TEXT("the user's roof spot (sector 322)"), false });
		// the highest floors with a standing spot whose floor a trace finds there, highest first: the first one the body
		// stays on is the one tested (a sector out of reach of the play, a decoration, can drop it)
		TArray<TPair<double, int32>> Floors;
		TMap<int32, FVector> At;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsPauseZ), false, this);
		for (int32 Si = 0; Si < L->NumSectors(); ++Si)
		{
			FVector C;
			FHitResult Hit;
			int32 Drops = 0;
			if (L->SectorHeadroomCm(Si, Drops) < 220.0 || !L->SectorMiddleSpot(Si, R, Half, this, C)) continue;
			if (!GetWorld()->LineTraceSingleByChannel(Hit, C, C - FVector(0.0, 0.0, Half + 30.0), ECC_Pawn, Q) || FMath::Abs(Hit.ImpactPoint.Z - (C.Z - Half - 3.0)) > 5.0) continue;
			Floors.Add(TPair<double, int32>(Hit.ImpactPoint.Z, Si));
			At.Add(Si, FVector(C.X, C.Y, Hit.ImpactPoint.Z));
		}
		Floors.Sort([](const TPair<double, int32>& A, const TPair<double, int32>& B) { return A.Key > B.Key; });
		for (int32 i = 0; i < FMath::Min(8, Floors.Num()); ++i)
			Spots.Add({ At[Floors[i].Value], FString::Printf(TEXT("the highest floor (sector %d, Z %.0f; %s)"), Floors[i].Value, Floors[i].Key, *L->DescribeSector(Floors[i].Value).Left(60)), true });
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT pausez %s: %d spots"), *L->LevelName, Spots.Num());
		SpotI = Case = 0;
		CaseT = 0.f;
		ShotStep = Spots.Num() > 0 ? 1 : 9;
		return;
	}
	if (ShotStep == 1)
	{
		const FSpot& S = Spots[SpotI];
		const float Half = GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
		auto Feet = [this]() { return GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight()); };
		CaseT += FMath::Min(Dt, float(FApp::GetDeltaTime()));     // (real time: -OLOldPauseZ's frame comes 10000 times too long)
		if (!bStarted)
		{
			bStarted = true;
			bVR = true;
			bCrouchHeld = bStandPending = false;
			CrouchAmount = 1.f;
			CrouchDrop = 0.f;
			ShownDrop = ShownDropVel = 0.f;
			HeightOffset = 0.f;
			Camera->SetRelativeLocation(FVector(0.0, 0.0, 170.0));
			TeleportTo(S.Feet + FVector(0.0, 0.0, Half + 2.0), GetActorRotation(), false, true);
			bArmed = false;
			MaxFeet = MaxStep = MaxOff = 0.0;
			MinUp = 1e9;
			MaxUp = -1e9;
			return;
		}
		const FVector Eye = Camera->GetComponentLocation();
		if (!bArmed && CaseT > 2.f)
		{
			if (S.bCandidate && (FVector::Dist(Feet(), S.Feet) > 5.0 || GetCharacterMovement()->MovementMode != MOVE_Walking))
			{
				// not a floor the body stays on: the next highest
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT pausez skipped %s: the feet at %.0f %.0f %.0f after 2 s"), *S.What, Feet().X, Feet().Y, Feet().Z);
				bStarted = false;
				CaseT = 0.f;
				Case = 0;
				++SpotI;
				if (SpotI >= Spots.Num()) ShotStep = 9;
				return;
			}
			bArmed = true;
			Feet0 = Feet();
			Eye0 = PrevEye = Eye;
			if (Case == 1)
			{
				// the stick crouch, all the way: the view rides down at its pace (200 cm/s at the 55 % speed)
				bCrouchHeld = true;
				bStandPending = false;
				CrouchFull = CrouchDrop = FMath::Max(0.f, HeadHeight() - CrouchEyeZ);
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT pausez %s case %s: feet %.2f %.2f %.2f, eye Z %.2f (%.2f over the feet)"), *S.What, Case == 0 ? TEXT("still") : TEXT("riding"),
				Feet0.X, Feet0.Y, Feet0.Z, Eye.Z, Eye.Z - Feet0.Z);
		}
		if (!bArmed) return;
		const float Real = FMath::Min(Dt, float(FApp::GetDeltaTime()));
		if (CaseT > 2.1f && CaseT - Real <= 2.1f) PreTickAsk = 1;          // (riding: 0.1 s into the ride)
		if (CaseT > 5.1f && CaseT - Real <= 5.1f) PreTickAsk = 2;
		const FVector F = Feet();
		MaxFeet = FMath::Max(MaxFeet, FVector::Dist(F, Feet0));
		MaxStep = FMath::Max(MaxStep, FMath::Abs(Eye.Z - PrevEye.Z));
		MaxOff = FMath::Max(MaxOff, FMath::Abs(Eye.Z - Eye0.Z));
		MinUp = FMath::Min(MinUp, Eye.Z - F.Z);
		MaxUp = FMath::Max(MaxUp, Eye.Z - F.Z);
		PrevEye = Eye;
		if (CaseT > 7.1f)
		{
			// the riding case's range: from the crouch eye to the standing head, a couple of cm either way
			const bool bOk = MaxFeet < 1.0 && MaxStep < 5.0 && (Case == 0 ? MaxOff < 1.0 : MinUp > CrouchEyeZ - 3.0 && MaxUp < 173.0) && !bMenuOpen;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT pausez %s %s %s: feet moved %.2f cm at most; the eye's biggest step in a frame %.2f cm, %.2f cm off its start at most, %.2f to %.2f cm over the feet (menu open %d)"),
				bOk ? TEXT("PASS") : TEXT("FAIL"), *S.What, Case == 0 ? TEXT("still") : TEXT("riding"), MaxFeet, MaxStep, MaxOff, MinUp, MaxUp, bMenuOpen ? 1 : 0);
			++Runs;
			if (!bOk) ++Fails;
			bCrouchHeld = false;
			bStandPending = true;
			CaseT = 0.f;
			bStarted = false;
			if (++Case > 1)
			{
				Case = 0;
				if (S.bCandidate) while (SpotI + 1 < Spots.Num() && Spots[SpotI + 1].bCandidate) Spots.RemoveAt(SpotI + 1);   // (the one tested)
				++SpotI;
			}
			if (SpotI >= Spots.Num()) ShotStep = 9;
		}
		return;
	}
	if (ShotStep == 9)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT pausez RESULT %s on %s: %d of %d cases passed%s"), Fails == 0 && Runs > 0 ? TEXT("PASS") : TEXT("FAIL"), *L->LevelName, Runs - Fails, Runs,
			OLFlags::Param(TEXT("OLOldPauseZ")) ? TEXT(" (-OLOldPauseZ)") : TEXT(""));
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
