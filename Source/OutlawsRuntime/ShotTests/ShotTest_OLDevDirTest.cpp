// -OLShots -OLDevDirTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLDevDirTest) { return OLFlags::Param(TEXT("OLDevDirTest")); }

OL_SHOT_TEST(OLDevDirTest, 1300)
{
	// (BUG-DEVDIR, user 2026-10-31 via the planner: testers' Gatling is the wrong model and draws purple, their held lantern
	// differs) what a player sees of the things the dev placeholders folder used to supply, for the side-by-side of a tester's
	// install and this PC: the Gatling in the right fist (ol_devdir_gatling_held, each section's texture logged: none is the
	// purple), then on the floor one at a time 2.5 m off, looked at from in front: the Gatling pickup, a cactus, a tree, the cow
	// skull, a flag, the lantern and the armour plate (ol_devdir_<stem>).  The verdict: every held section textured, every
	// floor thing made, and as a tester (-OLNoDevDir, or no placeholders folder) no placeholder file read
	static const TCHAR* Floor[] = { TEXT("GGATGUN"), TEXT("CACTUS01"), TEXT("TREE01"), TEXT("COWSKULL"), TEXT("UNFLAG"), TEXT("GLANTERN"), TEXT("GBPLATE") };
	constexpr int32 NumFloor = UE_ARRAY_COUNT(Floor);
	static int32 Fails = 0;
	static float Yaw0 = -1000.f;
	static TWeakObjectPtr<AActor> Item;
	if (Yaw0 < -999.f) Yaw0 = static_cast<float>(GetActorRotation().Yaw);
	if (ShotStep == 0) GEngine->Exec(GetWorld(), TEXT("r.MotionBlurQuality 0"));
	auto Shot = [](const FString& Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / (TEXT("ol_devdir_") + Name + TEXT(".png")), false, false); };
	if (ShotStep <= 2)
	{
		// the Gatling held side on before the eyes, its barrels to the right (as -OLHeldGunTest holds the guns)
		if (Controller) Controller->SetControlRotation(FRotator(0.0, Yaw0, 0.0));
		const FVector Eye = Camera->GetComponentLocation();
		const FVector F = Camera->GetForwardVector(), R = Camera->GetRightVector();
		if (LeftHand) LeftHand->SetVisibility(false, true);
		if (UOutlawsBoxHand* Box = RightHand.Get())
		{
			const FBox B = GunBounds[1];
			const double Span = B.IsValid ? B.GetSize().X : 60.0;
			const double Middle = B.IsValid ? B.GetCenter().X : 20.0;
			FrameOf(1)->SetWorldLocationAndRotation(Eye + F * FMath::Max(40.0, Span * 0.75) - R * Middle - FVector::UpVector * 4.0,
				FRotationMatrix::MakeFromXZ(R, FVector::UpVector).ToQuat());
			Box->SetVisibility(true, true);
			Box->SetCurl(1.f, 1.f);
		}
	}
	if (ShotStep == 0 && ShotTime > 3.f && L)
	{
		L->GiveAllWeapons();
		Equip(1, 5);
		int32 Bare = 0;
		const int32 N = WeaponMesh[1] ? WeaponMesh[1]->GetNumSections() : 0;
		for (int32 s = 0; s < N; ++s)
		{
			UMaterialInterface* M = WeaponMesh[1]->GetMaterial(s);
			UTexture* T = nullptr;
			if (M) M->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Tex")), T);
			const FProcMeshSection* Sec = WeaponMesh[1]->GetProcMeshSection(s);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT devdir gatling held section %d: %d verts, material %s, texture %s"), s, Sec ? Sec->ProcVertexBuffer.Num() : -1,
				M ? *M->GetName() : TEXT("none"), T ? *T->GetName() : TEXT("NONE (purple)"));
			if (!T) ++Bare;
		}
		const bool bOk = Equipped[1] == 5 && N > 0 && Bare == 0;
		if (!bOk) ++Fails;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT devdir gatling held: %s, %d sections, %d untextured, bounds %s: %s"), Equipped[1] == 5 ? TEXT("held") : TEXT("NOT HELD"),
			N, Bare, *GunBounds[1].ToString(), bOk ? TEXT("PASS") : TEXT("FAIL"));
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 6.f) { Shot(TEXT("gatling_held")); ++ShotStep; }
	else if (ShotStep >= 2 && ShotTime > 7.f + 3.5f * float((ShotStep - 2) / 2) + ((ShotStep - 2) % 2 ? 2.5f : 0.f) && L)
	{
		const int32 Fi = (ShotStep - 2) / 2, Fp = (ShotStep - 2) % 2;
		if (Fi >= NumFloor)
		{
			// as a tester (-OLNoDevDir) nothing may come from the placeholders folder; on this PC each file read is named
			const int32 Used = FOutlawsAssets::DevPlaceholderUses();
			const bool bTester = !FPaths::DirectoryExists(FOutlawsAssets::PlaceholderDir());
			if (bTester && Used > 0) ++Fails;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT devdir placeholder files read: %d (%s)%s"), Used, bTester ? TEXT("tester view") : TEXT("this PC's view"),
				bTester ? (Used > 0 ? TEXT(": FAIL") : TEXT(": PASS")) : TEXT(""));
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT devdir %s (%d failed)"), Fails == 0 ? TEXT("PASS") : TEXT("FAIL"), Fails);
			FGenericPlatformMisc::RequestExit(false);
			ShotTime = -1.f;
			return;
		}
		if (Fp == 0)
		{
			// on the floor 2.5 m off on the first clear side, the eye stood back from it looking at its middle
			if (Fi == 0)
			{
				Equip(0, -1);
				Equip(1, -1);
				if (LeftHand) LeftHand->SetVisibility(false, true);
				if (RightHand) RightHand->SetVisibility(false, true);
			}
			if (Item.IsValid()) Item->Destroy();
			const double Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			static FVector Start = FVector::ZeroVector;
			if (Fi == 0) Start = GetActorLocation();
			const FVector Feet = Start - FVector(0.0, 0.0, Half);
			FCollisionQueryParams Q(TEXT("OLDevDir"), false, this);
			FVector Dir = FRotator(0.0, Yaw0, 0.0).Vector();
			for (int32 d = 0; d < 8; ++d)
			{
				const FVector Try = FRotator(0.0, Yaw0 + 45.0 * d, 0.0).Vector();
				if (!GetWorld()->LineTraceTestByChannel(Feet + FVector(0.0, 0.0, 60.0), Feet + FVector(0.0, 0.0, 60.0) + Try * 450.0, ECC_Visibility, Q)) { Dir = Try; break; }
			}
			FMath::RandInit(1300 + Fi);          // (a drop lies at a random turn: the same one in every run)
			Item = L->SpawnGroundItem(Floor[Fi], Feet + Dir * 250.0);
			AOutlawsSpriteActor* S = Cast<AOutlawsSpriteActor>(Item.Get());
			const FBox B = !Item.IsValid() ? FBox(ForceInit) : S && S->Model ? S->Model->Bounds.GetBox() : Item->GetComponentsBoundingBox(true);
			if (B.IsValid)
			{
				const double Off = FMath::Max(140.0, B.GetSize().GetMax() * 0.9);
				FVector Stand = B.GetCenter() - Dir * Off;
				Stand.Z = Feet.Z + Half + 2.0;
				FHitResult Hit;
				const FVector From(B.GetCenter().X, B.GetCenter().Y, Stand.Z);
				if (GetWorld()->LineTraceSingleByChannel(Hit, From, Stand, ECC_Visibility, Q)) Stand = Hit.ImpactPoint + Dir * 40.0;
				SetActorLocation(Stand, false, nullptr, ETeleportType::TeleportPhysics);
			}
			if (!Item.IsValid()) ++Fails;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT devdir floor %s: %s, box %s: %s"), Floor[Fi], !Item.IsValid() ? TEXT("NOT MADE") : S && S->Model ? TEXT("3D model")
				: S ? TEXT("sprite") : *Item->GetClass()->GetName(), *B.ToString(), Item.IsValid() ? TEXT("PASS") : TEXT("FAIL"));
		}
		else
		{
			AOutlawsSpriteActor* S = Cast<AOutlawsSpriteActor>(Item.Get());
			const FBox B = !Item.IsValid() ? FBox(ForceInit) : S && S->Model ? S->Model->Bounds.GetBox() : Item->GetComponentsBoundingBox(true);
			if (B.IsValid && Controller) Controller->SetControlRotation((B.GetCenter() - Camera->GetComponentLocation()).Rotation());
			Shot(FString(Floor[Fi]).ToLower());
		}
		++ShotStep;
	}
}

#endif // WITH_OUTLAWS_TESTS
