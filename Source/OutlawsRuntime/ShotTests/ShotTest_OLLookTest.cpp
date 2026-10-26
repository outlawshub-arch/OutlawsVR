// -OLShots -OLLookTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLLookTest) { return OLFlags::Param(TEXT("OLLookTest")); }

OL_SHOT_TEST(OLLookTest, 750)
{
	// the 3D items standing in for sprites, seen in the level: the camera walks up to each kind in turn (up to 10;
	// -OLLookItem=<part of a name> picks) and shoots ol_look_<name>.png; then bullet holes, blood and lamp fire
	// on the nearest wall and floor (ol_decals.png)
	static TArray<TWeakObjectPtr<AOutlawsSpriteActor>> Tour;
	static float Next = 3.f;
	if (ShotStep == 0 && ShotTime > 3.f)
	{
		if (Controller) GEngine->Exec(GetWorld(), TEXT("r.MotionBlurQuality 0"));
		FString Want;
		OLFlags::Value(TEXT("OLLookItem"), Want);
		TSet<FString> Seen;
		for (TActorIterator<AOutlawsSpriteActor> It(GetWorld()); It; ++It)
		{
			if (!It->Model || Tour.Num() >= 14) continue;
			const FString Kind = It->OutlawsName.ToUpper();
			if ((!Want.IsEmpty() && !Kind.Contains(Want.ToUpper())) || Seen.Contains(Kind)) continue;
			Seen.Add(Kind);
			Tour.Add(*It);
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLRT look test: %d kinds with models"), Tour.Num());
		Next = ShotTime;
		++ShotStep;
	}
	else if (ShotStep >= 1 && ShotStep < 1 + 2 * Tour.Num() && ShotTime > Next)
	{
		const int32 k = (ShotStep - 1) / 2;
		AOutlawsSpriteActor* S = Tour[k].Get();
		if (!S)
		{
			ShotStep += 2;
		}
		else if ((ShotStep - 1) % 2 == 0)
		{
			// stand 1.7 m off, on the first side with a clear view, eye at 1.6 m, looking down at it
			const FVector At = S->GetActorLocation();
			const FBox B = S->Model->Bounds.GetBox();
			const double Off = FMath::Max(170.0, B.GetSize().Size() * 1.4);
			FVector Spot = At + FVector(Off, 0.0, 0.0);
			FCollisionQueryParams Q(TEXT("OLLook"), false, this);
			for (int32 d = 0; d < 8; ++d)
			{
				const FVector Try = At + FRotator(0.0, 45.0 * d + 20.0, 0.0).Vector() * Off;
				if (!GetWorld()->LineTraceTestByChannel(At + FVector(0.0, 0.0, 60.0), Try + FVector(0.0, 0.0, 60.0), ECC_Visibility, Q)) { Spot = Try; break; }
			}
			SetActorLocation(Spot + FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0), false, nullptr, ETeleportType::TeleportPhysics);
			if (Controller) Controller->SetControlRotation((B.GetCenter() - Camera->GetComponentLocation()).Rotation());
			Next = ShotTime + 1.5f;
			++ShotStep;
		}
		else
		{
			if (Controller) Controller->SetControlRotation((S->Model->Bounds.GetBox().GetCenter() - Camera->GetComponentLocation()).Rotation());
			FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / (TEXT("ol_look_") + S->OutlawsName.ToLower() + TEXT(".png")), false, false);
			UE_LOG(LogOutlaws, Display, TEXT("OLRT look test: %s at %s, model %s"), *S->OutlawsName, *S->GetActorLocation().ToString(), *S->Model->Bounds.GetBox().GetSize().ToString());
			Next = ShotTime + 0.5f;
			++ShotStep;
		}
	}
	else if (ShotStep == 1 + 2 * Tour.Num() && ShotTime > Next && L)
	{
		// the nearest wall ahead of the last spot: holes in it, blood on it and the floor, lamp fire dropping
		const FVector Eye = Camera->GetComponentLocation();
		FVector Best = FVector::ZeroVector;
		double BestD = 1e9;
		FRotator BestR;
		FCollisionQueryParams Q(TEXT("OLLook"), false, this);
		for (int32 d = 0; d < 16; ++d)
		{
			const FRotator R(-8.0, 22.5 * d, 0.0);
			FHitResult H;
			if (GetWorld()->LineTraceSingleByChannel(H, Eye, Eye + R.Vector() * 900.0, ECC_Visibility, Q) && H.ImpactNormal.Z < 0.3 && H.Distance < BestD && H.Distance > 150.0)
			{
				BestD = H.Distance;
				Best = H.ImpactPoint;
				BestR = R;
			}
		}
		if (BestD < 1e8)
		{
			if (Controller) Controller->SetControlRotation(BestR);
			for (int32 i = 0; i < 9; ++i)
			{
				FHitResult H;
				const FRotator R = BestR + FRotator(FMath::FRandRange(-4.f, 4.f), FMath::FRandRange(-5.f, 5.f), 0.0);
				if (GetWorld()->LineTraceSingleByChannel(H, Eye, Eye + R.Vector() * 1200.0, ECC_Visibility, Q)) L->AddBulletHole(H);
			}
			const FVector Dir = (Best - Eye).GetSafeNormal();
			L->SpawnBlood(Eye + Dir * (BestD * 0.6), Dir, 4);
			L->SpawnBlood(Eye + Dir * (BestD * 0.4), FVector(Dir.X, Dir.Y, -0.6), 4);
			L->DebugSpawnSparks(Eye + Dir * (BestD * 0.5) + FVector(0.0, 0.0, 60.0), 6);
			// a blast on the floor 60 cm short of the wall
			FHitResult Fl;
			const FVector Above = Best - FVector(Dir.X, Dir.Y, 0.0).GetSafeNormal() * 60.0;
			if (GetWorld()->LineTraceSingleByChannel(Fl, Above, Above - FVector(0.0, 0.0, 400.0), ECC_Visibility, Q)) L->DebugScorch(FVector(Fl.ImpactPoint) + FVector(0.0, 0.0, 25.0));
			UE_LOG(LogOutlaws, Display, TEXT("OLRT look test: wall %.0f cm ahead at %s"), BestD, *Best.ToString());
		}
		Next = ShotTime + 2.5f;
		++ShotStep;
	}
	else if (ShotStep == 2 + 2 * Tour.Num() && ShotTime > Next)
	{
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_decals.png"), false, false);
		Next = ShotTime + 1.f;
		++ShotStep;
	}
	else if (ShotStep == 3 + 2 * Tour.Num() && ShotTime > Next)
	{
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
