// -OLShots -OLFaceTurnTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"
#include "OutlawsObjMesh.h"
#include "OutlawsInk.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLFaceTurnTest) { return OLFlags::Param(TEXT("OLFaceTurnTest")); }

namespace
{
	/** How much of a live mesh's own area (not its ink shell's) faces out.  bPairs: a world item's model, each triangle
	 *  followed by its reverse (UseItemModel draws both): the first of each. */
	OutlawsObj::FFaceTurn FaceTurnLiveOut(const UPrimitiveComponent* C, bool bPairs)
	{
		OutlawsObj::FFaceTurn All;
		UProceduralMeshComponent* M = const_cast<UProceduralMeshComponent*>(Cast<UProceduralMeshComponent>(C));
		const int32 Own = OutlawsInk::OwnSections(M);
		for (int32 s = 0; M && s < Own; ++s)
		{
			const FProcMeshSection* Sec = M->GetProcMeshSection(s);
			if (!Sec) continue;
			TArray<FVector> P;
			TArray<int32> T;
			for (const FProcMeshVertex& V : Sec->ProcVertexBuffer) P.Add(V.Position);
			for (int32 i = 0; i + 2 < Sec->ProcIndexBuffer.Num(); i += bPairs ? 6 : 3)
			{
				for (int32 k = 0; k < 3; ++k) T.Add(int32(Sec->ProcIndexBuffer[i + k]));
			}
			All.Add(OutlawsObj::MeasureFacesOutward(P, T));
		}
		return All;
	}
}

OL_SHOT_TEST(OLFaceTurnTest, 1117)
{
	// FACETURN (-OLLevel=hideout -OLNoFrontEnd): the held dynamite and knife in the right hand, side on and tipped toward the
	// eye (their tops), then a cactus, TREE01, the cow skull, a flag and the gatling pickup on the floor.  Each mesh's area
	// facing out is measured and logged: OLFACETURN ...  With -OLDetailedThrowables the hands hold item3d's GDYNAM3D and
	// GKNIFE3D (turned at load), without it the game's own 3DOs from the player's install (measured only: not ours to turn).
	// -OLOldScopeFaces: the before pictures (every item3d model as its file has it), no checks
	static const TCHAR* Poses[] = { TEXT("dyn_side"), TEXT("dyn_tip"), TEXT("knife_side"), TEXT("knife_tip") };
	static const TCHAR* Floor[] = { TEXT("CACTUS01"), TEXT("TREE01"), TEXT("COWSKULL"), TEXT("CONFLAG"), TEXT("GGATGUN") };
	constexpr int32 NumPoses = UE_ARRAY_COUNT(Poses), NumFloor = UE_ARRAY_COUNT(Floor);
	static const bool bOld = OLFlags::Param(TEXT("OLOldScopeFaces"));
	static const bool bDetailed = OLFlags::Param(TEXT("OLDetailedThrowables"));
	static float Yaw0 = -1000.f;
	static int32 Fails = 0;
	static TWeakObjectPtr<AActor> Item;
	auto Check = [](bool bOk, const FString& What) { UE_LOG(LogOutlaws, Display, TEXT("OLFACETURN check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What); return bOk ? 0 : 1; };
	auto Shot = [](const FString& Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / (TEXT("ol_faceturn_") + Name + TEXT(".png")), false, false); };
	const FString Tail = FString(bDetailed ? TEXT("") : TEXT("_3do")) + (bOld ? TEXT("_old") : TEXT(""));
	if (ShotStep == 0) GEngine->Exec(GetWorld(), TEXT("r.MotionBlurQuality 0"));
	if (Yaw0 < -999.f) Yaw0 = static_cast<float>(GetActorRotation().Yaw);
	const int32 Pose = ShotStep >= 1 && ShotStep <= 2 * NumPoses ? (ShotStep - 1) / 2 : -1;
	// the pose held every frame (the desktop has no hand tracking): the right hand's frame turned and moved until the held
	// thing stands up its long side across the view (or its top tipped toward the eye), its middle 45 cm ahead
	if (Pose >= 0 && Controller)
	{
		Controller->SetControlRotation(FRotator(0.0, Yaw0, 0.0));
		UProceduralMeshComponent* Gun = WeaponMesh[1];
		USceneComponent* Fr = FrameOf(1);
		if (Gun && Fr && GunBounds[1].IsValid)
		{
			const FVector Eye = Camera->GetComponentLocation();
			const FVector F = Camera->GetForwardVector(), R = Camera->GetRightVector(), U = Camera->GetUpVector();
			const bool bTip = Pose == 1 || Pose == 3;
			const FQuat Want = FRotationMatrix::MakeFromXZ(R, bTip ? (U - F * 0.9).GetSafeNormal() : U).ToQuat();
			Fr->SetWorldRotation(Want);
			Fr->SetWorldRotation(Want * (Gun->GetComponentQuat().Inverse() * Want));
			const FVector Target = Eye + F * 45.0;
			Fr->SetWorldLocation(Fr->GetComponentLocation() + (Target - Gun->GetComponentTransform().TransformPosition(GunBounds[1].GetCenter())));
		}
		if (UOutlawsBoxHand* Box = RightHand.Get()) { Box->SetVisibility(true, true); Box->SetCurl(1.f, 1.f); }
		if (UOutlawsBoxHand* Other = LeftHand.Get()) Other->SetVisibility(false, true);
	}
	const float PoseDue = 4.f + 2.5f * Pose;
	// the floor: a step to drop each model, one to photograph it
	const int32 F0 = 1 + 2 * NumPoses;
	const int32 Fi = ShotStep >= F0 ? (ShotStep - F0) / 2 : -1, Fp = ShotStep >= F0 ? (ShotStep - F0) % 2 : -1;
	const float FloorDue = 4.f + 2.5f * NumPoses + 3.f * Fi;
	if (ShotStep == 0 && ShotTime > 3.f && L)
	{
		L->GiveAllWeapons();
		UE_LOG(LogOutlaws, Display, TEXT("OLFACETURN start%s: held throwables %s"), bOld ? TEXT(" (old faces)") : TEXT(""),
			bDetailed ? TEXT("item3d's GDYNAM3D and GKNIFE3D (-OLDetailedThrowables)") : TEXT("the game's own gdynam.3do and gknife.3do"));
		ShotStep = 1;
	}
	else if (Pose >= 0 && (ShotStep - 1) % 2 == 0 && ShotTime > PoseDue)
	{
		const int32 Wi = Pose < 2 ? 6 : 7;
		if (Equipped[0] >= 0) Equip(0, -1);
		if (Equipped[1] != Wi) Equip(1, Wi);
		++ShotStep;
	}
	else if (Pose >= 0 && (ShotStep - 1) % 2 == 1 && ShotTime > PoseDue + 2.f)
	{
		const FString Name = FString(Poses[Pose]) + Tail;
		Shot(Name);
		UProceduralMeshComponent* Part = WeaponMesh[1].Get();
		const OutlawsObj::FFaceTurn Ft = FaceTurnLiveOut(Part, false);
		const bool bShown = Part && Part->IsVisible() && Part->GetNumSections() > 0;
		UE_LOG(LogOutlaws, Display, TEXT("OLFACETURN %s: %s, %s, %d triangles in %d pieces, facing out %.1f%% (would turn %d)"), *Name, Pose < 2 ? TEXT("dynamite") : TEXT("knife"),
			bShown ? TEXT("shown") : TEXT("NOT SHOWN"), Ft.Faces, Ft.Pieces, Ft.ShareBefore() * 100.0, Ft.Turned);
		Fails += Check(bShown, FString::Printf(TEXT("%s: the %s is shown"), Poses[Pose], Pose < 2 ? TEXT("dynamite") : TEXT("knife")));
		if (!bOld && bDetailed) Fails += Check(Ft.ShareBefore() > 0.95, FString::Printf(TEXT("%s: %.1f%% of it faces out (over 95)"), Poses[Pose], Ft.ShareBefore() * 100.0));
		++ShotStep;
	}
	else if (Fi >= 0 && Fi < NumFloor && Fp == 0 && ShotTime > FloorDue && L)
	{
		// the model on the floor 2.5 m off on the first clear side, the eye stood back from it (a wall between: in front
		// of the wall) looking at its middle
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
		FCollisionQueryParams Q(TEXT("OLFaceTurn"), false, this);
		FVector Dir = FRotator(0.0, Yaw0, 0.0).Vector();
		for (int32 d = 0; d < 8; ++d)
		{
			const FVector Try = FRotator(0.0, Yaw0 + 45.0 * d, 0.0).Vector();
			if (!GetWorld()->LineTraceTestByChannel(Feet + FVector(0.0, 0.0, 60.0), Feet + FVector(0.0, 0.0, 60.0) + Try * 450.0, ECC_Visibility, Q)) { Dir = Try; break; }
		}
		FMath::RandInit(2730 + Fi);          // (a drop lies at a random turn: the same one in the before and after runs)
		Item = L->SpawnGroundItem(Floor[Fi], Feet + Dir * 250.0);
		AOutlawsSpriteActor* S = Cast<AOutlawsSpriteActor>(Item.Get());
		if (S && S->Model)
		{
			const FBox B = S->Model->Bounds.GetBox();
			const double Off = FMath::Max(140.0, B.GetSize().GetMax() * 0.9);
			FVector Stand = B.GetCenter() - Dir * Off;
			Stand.Z = Feet.Z + Half + 2.0;
			FHitResult Hit;
			const FVector From(B.GetCenter().X, B.GetCenter().Y, Stand.Z);
			if (GetWorld()->LineTraceSingleByChannel(Hit, From, Stand, ECC_Visibility, Q)) Stand = Hit.ImpactPoint + Dir * 40.0;
			SetActorLocation(Stand, false, nullptr, ETeleportType::TeleportPhysics);
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLFACETURN floor %s: %s"), Floor[Fi], S ? (S->Model ? TEXT("model made") : TEXT("SPRITE ONLY")) : TEXT("NOT MADE"));
		++ShotStep;
	}
	else if (Fi >= 0 && Fi < NumFloor && Fp == 1 && ShotTime > FloorDue + 2.5f)
	{
		AOutlawsSpriteActor* S = Cast<AOutlawsSpriteActor>(Item.Get());
		if (S && S->Model && Controller) Controller->SetControlRotation((S->Model->Bounds.GetBox().GetCenter() - Camera->GetComponentLocation()).Rotation());
		const FString Name = FString(TEXT("floor_")) + FString(Floor[Fi]).ToLower() + (bOld ? TEXT("_old") : TEXT(""));
		Shot(Name);
		const OutlawsObj::FFaceTurn Ft = FaceTurnLiveOut(S ? S->Model.Get() : nullptr, true);
		UE_LOG(LogOutlaws, Display, TEXT("OLFACETURN %s: %d triangles in %d pieces, facing out %.1f%% (would turn %d)"), *Name, Ft.Faces, Ft.Pieces, Ft.ShareBefore() * 100.0, Ft.Turned);
		Fails += Check(S && S->Model, FString::Printf(TEXT("floor %s: its 3D model is made"), Floor[Fi]));
		if (!bOld) Fails += Check(Ft.ShareBefore() > 0.95, FString::Printf(TEXT("floor %s: %.1f%% of it faces out (over 95)"), Floor[Fi], Ft.ShareBefore() * 100.0));
		++ShotStep;
	}
	else if (ShotStep == F0 + 2 * NumFloor && ShotTime > 4.f + 2.5f * NumPoses + 3.f * NumFloor + 0.5f)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLFACETURN RESULT %s%s"), Fails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), bOld ? TEXT(" (old faces: pictures, no faces checks)") : TEXT(""));
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
