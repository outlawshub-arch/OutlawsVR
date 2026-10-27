// -OLShots -OLScopeFacesTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"
#include "OutlawsObjMesh.h"
#include "OutlawsInk.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLScopeFacesTest) { return OLFlags::Param(TEXT("OLScopeFacesTest")); }

namespace
{
	/** How much of a live mesh's area faces out (OutlawsObj::MeasureFacesOutward over its own sections, not its ink shell's).
	 *  bPairs: a world item's model, each triangle followed by its reverse (UseItemModel draws both): the first of each. */
	OutlawsObj::FFaceTurn LiveFacesOut(const UPrimitiveComponent* C, bool bPairs = false)
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

OL_SHOT_TEST(OLScopeFacesTest, 1115)
{
	// SCOPEFACES (-OLLevel=hideout -OLNoFrontEnd, the placeholder guns): the rifle's telescope (item3d GSCOPE on the receiver)
	// side on and from behind it at the eye, in the right hand and in the mirrored left; the double's barrels from in
	// front (a tube part that was always double-faced); then the GSCOPE, GLEVER, GCROWBAR and GCRANK pickups on the floor.
	// Each mesh's area facing out is measured and logged: OLSCOPEFACES ...; with -OLOldScopeFaces the before pictures.
	// -OLScopeFacesTake: the floor pickups alone (nothing given first), each walked onto after its picture and checked taken
	static const TCHAR* Poses[] = { TEXT("rifle_side_r"), TEXT("rifle_eye_r"), TEXT("rifle_side_l"), TEXT("rifle_eye_l"), TEXT("dbs_front_r") };
	static const TCHAR* Floor[] = { TEXT("GSCOPE"), TEXT("GLEVER"), TEXT("GCROWBAR"), TEXT("GCRANK") };
	constexpr int32 NumPoses = UE_ARRAY_COUNT(Poses), NumFloor = UE_ARRAY_COUNT(Floor);
	static const bool bOld = OLFlags::Param(TEXT("OLOldScopeFaces"));
	static float Yaw0 = -1000.f;
	static int32 Fails = 0;
	static TWeakObjectPtr<AActor> Item;
	static const bool bTake = OLFlags::Param(TEXT("OLScopeFacesTake"));
	static const TCHAR* Counter[] = { TEXT("iscope"), TEXT("ilever"), TEXT("icrowbar"), TEXT("icrank") };
	static int32 TakeBefore = 0;
	auto Check = [](bool bOk, const FString& What) { UE_LOG(LogOutlaws, Display, TEXT("OLSCOPEFACES check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What); return bOk ? 0 : 1; };
	auto Shot = [](const FString& Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / (TEXT("ol_scopefaces_") + Name + TEXT(".png")), false, false); };
	if (ShotStep == 0) GEngine->Exec(GetWorld(), TEXT("r.MotionBlurQuality 0"));
	if (Yaw0 < -999.f) Yaw0 = static_cast<float>(GetActorRotation().Yaw);
	const int32 Pose = ShotStep >= 1 && ShotStep <= 2 * NumPoses ? (ShotStep - 1) / 2 : -1;
	// the pose held every frame (the desktop has no hand tracking): the gun turned and moved by its hand's frame until
	// its own axes and the point named land where the picture wants them
	if (Pose >= 0 && Controller)
	{
		Controller->SetControlRotation(FRotator(0.0, Yaw0, 0.0));
		const int32 Hand = Pose == 2 || Pose == 3 ? 0 : 1;
		const bool bEye = Pose == 1 || Pose == 3;
		UProceduralMeshComponent* Gun = WeaponMesh[Hand];
		USceneComponent* Fr = FrameOf(Hand);
		if (Gun && Fr)
		{
			const FVector Eye = Camera->GetComponentLocation();
			const FVector F = Camera->GetForwardVector(), R = Camera->GetRightVector(), U = Camera->GetUpVector();
			const double A = FMath::DegreesToRadians(40.0);
			const FVector GunX = bEye ? F : Pose == 4 ? (R * FMath::Cos(A) - F * FMath::Sin(A)) : R;
			const FQuat Want = FRotationMatrix::MakeFromXZ(GunX, U).ToQuat();
			Fr->SetWorldRotation(Want);
			Fr->SetWorldRotation(Want * (Gun->GetComponentQuat().Inverse() * Want));
			// the scope's axis in the gun's own frame: X 4 to 44, Z 7.35 (GSCOPE fitted 24 cm ahead, 4.9 cm up)
			// (the eye: 20 cm behind the eyepiece, 5 cm over the tube and 3 cm to its side, so the tube shows running away)
			const FVector Local = bEye ? FVector(4.0 - 20.0, 3.0, 7.35 + 5.0) : Pose == 4 ? GunBounds[Hand].GetCenter() : FVector(24.0, 0.0, 7.35);
			const FVector Target = bEye ? Eye : Pose == 4 ? Eye + F * 55.0 - U * 3.0 : Eye + F * 45.0 - U * 6.0;
			Fr->SetWorldLocation(Fr->GetComponentLocation() + (Target - Gun->GetComponentTransform().TransformPosition(Local)));
		}
		if (ScopeMesh[Hand] && ScopeMesh[Hand]->GetNumSections() > 0) ScopeMesh[Hand]->SetVisibility(Equipped[Hand] == 1);
		if (UOutlawsBoxHand* Box = Hand == 0 ? LeftHand.Get() : RightHand.Get()) { Box->SetVisibility(true, true); Box->SetCurl(1.f, 1.f); }
		if (UOutlawsBoxHand* Other = Hand == 0 ? RightHand.Get() : LeftHand.Get()) Other->SetVisibility(false, true);
	}
	const float PoseDue = 4.f + 2.5f * Pose;
	// the floor: a step to drop each pickup, one to photograph it (and, taking, one to see it taken)
	const int32 K = bTake ? 3 : 2, F0 = 1 + 2 * NumPoses;
	const int32 Fi = ShotStep >= F0 ? (ShotStep - F0) / K : -1, Fp = ShotStep >= F0 ? (ShotStep - F0) % K : -1;
	const float FloorStep = bTake ? 4.5f : 3.f;
	const float FloorDue = 4.f + 2.5f * NumPoses + FloorStep * Fi;
	if (ShotStep == 0 && ShotTime > 3.f && L)
	{
		if (!bTake) L->GiveAllWeapons();
		UE_LOG(LogOutlaws, Display, TEXT("OLSCOPEFACES start%s%s: rifle %s, scope carried %d"), bOld ? TEXT(" (old faces)") : TEXT(""), bTake ? TEXT(" (floor pickups taken)") : TEXT(""),
			*HeldGunTestName(1), L->GetCount(TEXT("iscope")));
		ShotStep = bTake ? F0 : 1;
	}
	else if (Pose >= 0 && (ShotStep - 1) % 2 == 0 && ShotTime > PoseDue)
	{
		// the gun into the pose's hand, the other hand empty
		const int32 Hand = Pose == 2 || Pose == 3 ? 0 : 1;
		const int32 Wi = Pose == 4 ? 3 : 1;
		if (Equipped[1 - Hand] >= 0) Equip(1 - Hand, -1);
		if (Equipped[Hand] != Wi) Equip(Hand, Wi);
		++ShotStep;
	}
	else if (Pose >= 0 && (ShotStep - 1) % 2 == 1 && ShotTime > PoseDue + 2.f)
	{
		const int32 Hand = Pose == 2 || Pose == 3 ? 0 : 1;
		const FString Name = FString(Poses[Pose]) + (bOld ? TEXT("_old") : TEXT(""));
		Shot(Name);
		UProceduralMeshComponent* Part = Pose == 4 ? WeaponMesh[Hand].Get() : ScopeMesh[Hand].Get();
		const OutlawsObj::FFaceTurn Ft = LiveFacesOut(Part);
		const bool bShown = Part && Part->IsVisible() && Part->GetNumSections() > 0;
		UE_LOG(LogOutlaws, Display, TEXT("OLSCOPEFACES %s: %s %s, %s, %d triangles in %d pieces, facing out %.1f%% (would turn %d)"), *Name, *HeldGunTestName(Pose == 4 ? 3 : 1),
			Pose == 4 ? TEXT("gun") : TEXT("scope"), bShown ? TEXT("shown") : TEXT("NOT SHOWN"), Ft.Faces, Ft.Pieces, Ft.ShareBefore() * 100.0, Ft.Turned);
		if (!bOld)
		{
			Fails += Check(bShown, FString::Printf(TEXT("%s: the %s is shown"), Poses[Pose], Pose == 4 ? TEXT("gun") : TEXT("scope")));
			Fails += Check(Ft.ShareBefore() > 0.95, FString::Printf(TEXT("%s: %.1f%% of it faces out (over 95)"), Poses[Pose], Ft.ShareBefore() * 100.0));
		}
		++ShotStep;
	}
	else if (Fi >= 0 && Fi < NumFloor && Fp == 0 && ShotTime > FloorDue && L)
	{
		// the pickup on the floor 2.5 m off on the first clear side, the eye stood back from it looking down at it
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
		FCollisionQueryParams Q(TEXT("OLScopeFaces"), false, this);
		FVector Dir = FRotator(0.0, Yaw0, 0.0).Vector();
		for (int32 d = 0; d < 8; ++d)
		{
			const FVector Try = FRotator(0.0, Yaw0 + 45.0 * d, 0.0).Vector();
			if (!GetWorld()->LineTraceTestByChannel(Feet + FVector(0.0, 0.0, 60.0), Feet + FVector(0.0, 0.0, 60.0) + Try * 450.0, ECC_Visibility, Q)) { Dir = Try; break; }
		}
		FMath::RandInit(2710 + Fi);          // (a drop lies at a random turn: the same one in the before and after runs)
		Item = L->SpawnGroundItem(Floor[Fi], Feet + Dir * 250.0);
		AOutlawsSpriteActor* S = Cast<AOutlawsSpriteActor>(Item.Get());
		if (S && S->Model)
		{
			const FBox B = S->Model->Bounds.GetBox();
			const double Off = FMath::Max(140.0, B.GetSize().GetMax() * 0.75);     // (nearer, the body walks over it and takes it)
			const FVector Stand = B.GetCenter() - Dir * Off;
			SetActorLocation(FVector(Stand.X, Stand.Y, Feet.Z + Half + 2.0), false, nullptr, ETeleportType::TeleportPhysics);
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSCOPEFACES floor %s: %s"), Floor[Fi], S ? (S->Model ? TEXT("model made") : TEXT("SPRITE ONLY")) : TEXT("NOT MADE"));
		++ShotStep;
	}
	else if (Fi >= 0 && Fi < NumFloor && Fp == 1 && ShotTime > FloorDue + 2.5f)
	{
		AOutlawsSpriteActor* S = Cast<AOutlawsSpriteActor>(Item.Get());
		if (S && S->Model && Controller) Controller->SetControlRotation((S->Model->Bounds.GetBox().GetCenter() - Camera->GetComponentLocation()).Rotation());
		const FString Name = FString(TEXT("floor_")) + FString(Floor[Fi]).ToLower() + (bOld ? TEXT("_old") : TEXT(""));
		Shot(Name);
		const OutlawsObj::FFaceTurn Ft = LiveFacesOut(S ? S->Model.Get() : nullptr, true);
		UE_LOG(LogOutlaws, Display, TEXT("OLSCOPEFACES %s: %d triangles in %d pieces, facing out %.1f%% (would turn %d)"), *Name, Ft.Faces, Ft.Pieces, Ft.ShareBefore() * 100.0, Ft.Turned);
		if (!bOld)
		{
			Fails += Check(S && S->Model, FString::Printf(TEXT("floor %s: its 3D model is made"), Floor[Fi]));
			Fails += Check(Ft.ShareBefore() > 0.95, FString::Printf(TEXT("floor %s: %.1f%% of it faces out (over 95)"), Floor[Fi], Ft.ShareBefore() * 100.0));
		}
		if (bTake && S && L)
		{
			// walked onto: the body takes it as any pickup
			TakeBefore = L->GetCount(Counter[Fi]);
			const double Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			SetActorLocation(S->GetActorLocation() + FVector(0.0, 0.0, Half + 2.0), false, nullptr, ETeleportType::TeleportPhysics);
		}
		++ShotStep;
	}
	else if (bTake && Fi >= 0 && Fi < NumFloor && Fp == 2 && ShotTime > FloorDue + 3.8f && L)
	{
		const int32 Now = L->GetCount(Counter[Fi]);
		const bool bTaken = Now > TakeBefore || !Item.IsValid() || Item->IsHidden();
		Fails += Check(bTaken, FString::Printf(TEXT("floor %s: walked onto, taken (%s %d -> %d)"), Floor[Fi], Counter[Fi], TakeBefore, Now));
		++ShotStep;
	}
	else if (ShotStep == F0 + K * NumFloor && ShotTime > 4.f + 2.5f * NumPoses + FloorStep * NumFloor + 0.5f)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSCOPEFACES RESULT %s"), bOld ? TEXT("(old faces: pictures only)") : Fails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"));
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

OL_SHOT_MATCH(OLFaceTurnSurvey) { return OLFlags::Param(TEXT("OLFaceTurnSurvey")); }

OL_SHOT_TEST(OLFaceTurnSurvey, 1116)
{
	// SCOPEFACES: every placeholder model (items3d\*.obj, the floor's, and items3d\held\*.obj, the hands') loaded as the game
	// loads it and put through OutlawsObj::TurnFacesOutward once, applied (to the copy read here; the game turns only the
	// telescope and the four tube pickups): what it would turn in each, logged OLFACETURN.  A model whose share facing out
	// went down, or that read no triangles, fails
	if (ShotStep == 0 && ShotTime > 1.f)
	{
		int32 Models = 0, Turning = 0, Fails = 0;
		double Ms = 0.0;
		for (const TCHAR* Sub : { TEXT("items3d"), TEXT("items3d/held") })
		{
			const FString Dir = FPaths::Combine(FOutlawsAssets::PlaceholderDir(), Sub);
			TArray<FString> Files;
			IFileManager::Get().FindFiles(Files, *FPaths::Combine(Dir, TEXT("*.obj")), true, false);
			Files.Sort();
			for (const FString& F : Files)
			{
				OutlawsObj::FObjMesh Obj;
				const double T0 = FPlatformTime::Seconds();
				const bool bRead = OutlawsObj::Load(FPaths::Combine(Dir, F), 1.0, true, Obj);
				const OutlawsObj::FFaceTurn Ft = bRead ? OutlawsObj::TurnFacesOutward(Obj, true) : OutlawsObj::FFaceTurn();
				const double Took = (FPlatformTime::Seconds() - T0) * 1000.0;
				Ms += Took;
				const bool bBad = !bRead || Ft.Faces == 0 || Ft.ShareAfter() + 1e-9 < Ft.ShareBefore();
				Fails += bBad ? 1 : 0;
				Turning += Ft.Turned > 0 ? 1 : 0;
				++Models;
				UE_LOG(LogOutlaws, Display, TEXT("OLFACETURN %s/%s: %d triangles in %d pieces, %d turned, facing out %.1f%% -> %.1f%%, %.1f ms%s"), Sub, *F, Ft.Faces, Ft.Pieces, Ft.Turned,
					Ft.ShareBefore() * 100.0, Ft.ShareAfter() * 100.0, Took, bBad ? TEXT(" FAIL") : TEXT(""));
			}
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLFACETURN RESULT %s: %d models, %d with faces to turn, %.0f ms in all"), Fails == 0 && Models > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Models, Turning, Ms);
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 2.f)
	{
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
