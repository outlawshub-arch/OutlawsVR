// -OLShots -OLNoFrontEnd -OLLevel=hideout -OLBoxArtProbe=art|today [-OLBoxArtFile=<png>] [-OLBoxArtSynth] [-OLBoxArtShots=<tag>]:
// REQ-BOXART's guard, one of the ShotTest tests (OutlawsShotTests.h).  The hideout's hidden box (a fresh profile, so it is placed):
//   art    the 1997 big box: its mesh 3.5 deep, 18.4 wide, 23.5 high (cm) within 2%, 24 corners, six faces, each its own rect of
//          the atlas (T_BoxArt_Atlas_uv.json) the right way round, the material's Tex the 2048x1024 art; with -OLBoxArtSynth (the
//          weekgate's made-up atlas, each face's rect one flat colour) each face's middle reads its own colour in the file.
//   today  today's box: 8 x 26 x 32 cm within 2%, its 64x64 cardboard picture, no art.
// Both: no collision, the ink outline's stencil 4, the glint there, and walking into it still takes it (the profile's count goes up).
// -OLBoxArtShots=<tag>: before the walk, pictures in Saved\OLShots (ol_boxart_<tag>_close, _spine, _pickup, _prop: the box 38 cm away
// face on, at a front corner, from a standing eye at the walk-in reach, beside the nearest pickup for scale; each from a place
// clear of the walls).  These show the art: local only.
// Lines "OLSHOT boxart check <what>: PASS|FAIL" and "OLSHOT boxart RESULT <n> PASS, <m> FAIL".

#include "ShotTestCommon.h"
#include "OutlawsAchievements.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Camera/CameraActor.h"

#if WITH_OUTLAWS_TESTS

namespace
{
	struct FBoxArtProbeFace
	{
		const TCHAR* Name;
		FVector N, AU, AV;
		double U0, V0, U1, V1;
		FColor Synth;           // the weekgate's made-up atlas: this face's colour
	};
	// From T_BoxArt_Atlas_uv.json (the front along +X, width along Y, height along Z; top and bottom stored turned 90 cw)
	const FBoxArtProbeFace GProbeFaces[6] =
	{
		{ TEXT("front"), FVector(1, 0, 0), FVector(0, -1, 0), FVector(0, 0, -1), 0.001953125, 0.00390625, 0.388671875, 0.99609375, FColor(220, 40, 40) },
		{ TEXT("back"), FVector(-1, 0, 0), FVector(0, 1, 0), FVector(0, 0, -1), 0.392578125, 0.00390625, 0.779296875, 0.99609375, FColor(40, 200, 40) },
		{ TEXT("left_side"), FVector(0, 1, 0), FVector(1, 0, 0), FVector(0, 0, -1), 0.783203125, 0.00390625, 0.857421875, 0.99609375, FColor(40, 60, 220) },
		{ TEXT("right_side"), FVector(0, -1, 0), FVector(-1, 0, 0), FVector(0, 0, -1), 0.861328125, 0.00390625, 0.935546875, 0.99609375, FColor(230, 210, 40) },
		{ TEXT("top"), FVector(0, 0, 1), FVector(-1, 0, 0), FVector(0, -1, 0), 0.939453125, 0.00390625, 0.998046875, 0.49609375, FColor(40, 210, 210) },
		{ TEXT("bottom"), FVector(0, 0, -1), FVector(1, 0, 0), FVector(0, -1, 0), 0.939453125, 0.50390625, 0.998046875, 0.99609375, FColor(210, 40, 210) },
	};

	struct FBoxArtProbe
	{
		int32 Step = 0;
		double At = 0.0;
		int32 Pass = 0, Fail = 0;
		FString Mode, Tag;
		int32 Found0 = 0;
		FVector Prop = FVector::ZeroVector;
		FString PropName;
		bool bDone = false;
		TWeakObjectPtr<ACameraActor> Cam;
		double WaitSince = 0.0;
	};
	FBoxArtProbe BA;

	bool BoxArtDecode(const FString& Path, int32& W, int32& H, TArray<FColor>& Px)
	{
		TArray<uint8> Bytes, Raw;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path)) return false;
		IImageWrapperModule& Mod = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> Png = Mod.CreateImageWrapper(EImageFormat::PNG);
		if (!Png.IsValid() || !Png->SetCompressed(Bytes.GetData(), Bytes.Num()) || !Png->GetRaw(ERGBFormat::BGRA, 8, Raw)) return false;
		W = int32(Png->GetWidth());
		H = int32(Png->GetHeight());
		if (Raw.Num() != W * H * 4) return false;
		Px.SetNumUninitialized(W * H);
		FMemory::Memcpy(Px.GetData(), Raw.GetData(), Raw.Num());
		return true;
	}
}

OL_SHOT_MATCH(OLBoxArtProbe) { return OLFlags::Has(TEXT("OLBoxArtProbe")); }

OL_SHOT_TEST(OLBoxArtProbe, 2650)
{
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (!L || !PC || !Camera || BA.bDone) return;
	const double Now = FPlatformTime::Seconds();
	auto Check = [](bool bOk, const FString& What)
	{
		(bOk ? BA.Pass : BA.Fail)++;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT boxart check %s: %s"), *What, bOk ? TEXT("PASS") : TEXT("FAIL"));
	};
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT boxart RESULT %d PASS, %d FAIL"), BA.Pass, BA.Fail);
		BA.bDone = true;
		FGenericPlatformMisc::RequestExit(false);
	};
	auto FoundCount = [&]()
	{
		TArray<FString> Levels, Labels;
		OutlawsAchievements::BoxLevels(Levels, Labels);
		int32 N = 0;
		for (const FString& Lv : Levels) N += OutlawsAchievements::BoxFound(L, Lv) ? 1 : 0;
		return N;
	};
	if (Now < BA.At) return;
	AActor* Box = L->GetCollectBox();
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const bool bArt = L->CollectBoxHasArt();
	const double DH = bArt ? 23.5 : 32.0;
	// the pictures' own camera put at From looking at To (the player's view and body stay where they are)
	auto Aim = [&](const FVector& From, const FVector& To)
	{
		if (ACameraActor* C = BA.Cam.Get()) C->SetActorLocationAndRotation(From, (To - From).Rotation());
	};
	auto BoxMid = [&]() { return Box->GetActorLocation() + FVector(0.0, 0.0, 6.0 + DH * 0.5); };
	auto Front = [&]() { return Box->GetActorRotation().RotateVector(FVector(1, 0, 0)); };
	auto Shot = [&](const TCHAR* Name)
	{
		const FString Path = FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("ol_boxart_%s_%s.png"), *BA.Tag, Name);
		FScreenshotRequest::RequestScreenshot(Path, false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT boxart shot %s"), *Path);
	};
	// the walk-in reach (UpdateCollectBox: the body's radius or 1.5 units, and today's half width)
	const double Reach = FMath::Max(double(GetCapsuleComponent()->GetScaledCapsuleRadius()), 1.5 * FOutlawsGeometry::UnitsToCm) + 13.0;
	switch (BA.Step)
	{
	case 0:
	{
		if (BA.At == 0.0) { BA.At = Now + 4.0; return; }
		if (L->IsFrontEnd() || L->PendingLoad.IsValid() || L->IsRebuildPending()) return;
		OLFlags::Value(TEXT("OLBoxArtProbe"), BA.Mode);
		if (!OLFlags::Value(TEXT("OLBoxArtShots"), BA.Tag) || BA.Tag.IsEmpty()) BA.Tag.Reset();
		L->bInvincible = true;
		BA.Step = 1;
		BA.At = Now + 2.0;
		return;
	}
	case 1:
	{
		const bool bWantArt = BA.Mode == TEXT("art");
		Check(BA.Mode == TEXT("art") || BA.Mode == TEXT("today"), FString::Printf(TEXT("-OLBoxArtProbe=art|today given (%s)"), *BA.Mode));
		Check(Box != nullptr, FString::Printf(TEXT("the %s box placed (%s)"), *L->LevelName, *L->CollectBoxLook()));
		if (!Box) { Finish(); return; }
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT boxart look: %s"), *L->CollectBoxLook());
		Check(bArt == bWantArt, FString::Printf(TEXT("drawn %s, wanted %s"), bArt ? TEXT("from the art") : TEXT("as today"), bWantArt ? TEXT("the art") : TEXT("today's")));
		UProceduralMeshComponent* M = Box->FindComponentByClass<UProceduralMeshComponent>();
		FProcMeshSection* Sec = M ? M->GetProcMeshSection(0) : nullptr;
		Check(Sec != nullptr && Sec->ProcVertexBuffer.Num() == 24 && Sec->ProcIndexBuffer.Num() == 36,
			FString::Printf(TEXT("one section, 24 corners and 12 triangles (%d, %d)"), Sec ? Sec->ProcVertexBuffer.Num() : -1, Sec ? Sec->ProcIndexBuffer.Num() : -1));
		if (!Sec || Sec->ProcVertexBuffer.Num() != 24) { Finish(); return; }
		FBox B(ForceInit);
		for (const FProcMeshVertex& V : Sec->ProcVertexBuffer) B += V.Position;
		const FVector Size = B.GetSize();
		const FVector Want = bWantArt ? FVector(3.5, 18.4, 23.5) : FVector(8.0, 26.0, 32.0);
		const bool bDims = FMath::Abs(Size.X - Want.X) <= 0.02 * Want.X && FMath::Abs(Size.Y - Want.Y) <= 0.02 * Want.Y && FMath::Abs(Size.Z - Want.Z) <= 0.02 * Want.Z;
		Check(bDims, FString::Printf(TEXT("mesh %.2f deep x %.2f wide x %.2f high cm, wanted %.1f x %.1f x %.1f within 2%%"), Size.X, Size.Y, Size.Z, Want.X, Want.Y, Want.Z));
		UMaterialInstanceDynamic* Mid = Cast<UMaterialInstanceDynamic>(M->GetMaterial(0));
		UTexture* Tex = nullptr;
		if (Mid) Mid->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Tex")), Tex);
		UTexture2D* T2 = Cast<UTexture2D>(Tex);
		const int32 TW = T2 ? T2->GetSizeX() : -1, TH = T2 ? T2->GetSizeY() : -1;
		if (bWantArt) Check(TW == 2048 && TH == 1024, FString::Printf(TEXT("the material's Tex is the 2048x1024 art (%dx%d)"), TW, TH));
		else Check(TW == 64 && TH == 64, FString::Printf(TEXT("the material's Tex is today's 64x64 cardboard (%dx%d)"), TW, TH));
		// the faces: each four corners with one normal, matched to the atlas's faces
		const FVector C = B.GetCenter();
		int32 Matched = 0;
		TArray<FColor> FilePx;
		int32 FW = 0, FH = 0;
		FString ArtPath;
		const bool bSynth = bWantArt && OLFlags::Param(TEXT("OLBoxArtSynth")) && OLFlags::Value(TEXT("OLBoxArtFile"), ArtPath, false) && BoxArtDecode(ArtPath.TrimQuotes(), FW, FH, FilePx);
		if (bWantArt && OLFlags::Param(TEXT("OLBoxArtSynth"))) Check(bSynth, TEXT("the made-up atlas read back"));
		for (int32 f = 0; f < 6; ++f)
		{
			const FProcMeshVertex* Q = &Sec->ProcVertexBuffer[f * 4];
			const FVector N = Q[0].Normal;
			bool bSame = true;
			for (int32 k = 1; k < 4; ++k) bSame &= Q[k].Normal.Equals(N, 1e-3);
			if (!bWantArt)
			{
				Matched += bSame ? 1 : 0;
				continue;
			}
			const FBoxArtProbeFace* F = nullptr;
			for (const FBoxArtProbeFace& G : GProbeFaces) if (G.N.Equals(N, 1e-3)) F = &G;
			if (!F || !bSame) { Check(false, FString::Printf(TEXT("face %d: a known normal (%s)"), f, *N.ToCompactString())); continue; }
			double u0 = 9, v0 = 9, u1 = -9, v1 = -9;
			bool bWay = true;
			for (int32 k = 0; k < 4; ++k)
			{
				const FVector2D UV = Q[k].UV0;
				u0 = FMath::Min(u0, UV.X); u1 = FMath::Max(u1, UV.X); v0 = FMath::Min(v0, UV.Y); v1 = FMath::Max(v1, UV.Y);
				// the right way round: a corner toward AU has U1, toward AV has V1
				const FVector P = Q[k].Position - C;
				bWay &= FMath::IsNearlyEqual(UV.X, FVector::DotProduct(P, F->AU) > 0.0 ? F->U1 : F->U0, 1e-4);
				bWay &= FMath::IsNearlyEqual(UV.Y, FVector::DotProduct(P, F->AV) > 0.0 ? F->V1 : F->V0, 1e-4);
				bWay &= FMath::IsNearlyEqual(FVector::DotProduct(P, F->N), FMath::Abs(FVector::DotProduct(Size * 0.5, F->N)), 0.01);
			}
			const bool bRect = FMath::IsNearlyEqual(u0, F->U0, 1e-4) && FMath::IsNearlyEqual(u1, F->U1, 1e-4) && FMath::IsNearlyEqual(v0, F->V0, 1e-4) && FMath::IsNearlyEqual(v1, F->V1, 1e-4);
			FString Col;
			bool bCol = true;
			if (bSynth)
			{
				const int32 X = FMath::Clamp(int32((u0 + u1) * 0.5 * FW), 0, FW - 1), Y = FMath::Clamp(int32((v0 + v1) * 0.5 * FH), 0, FH - 1);
				const FColor P = FilePx[Y * FW + X];
				bCol = FMath::Abs(P.R - F->Synth.R) < 8 && FMath::Abs(P.G - F->Synth.G) < 8 && FMath::Abs(P.B - F->Synth.B) < 8;
				Col = FString::Printf(TEXT(", its middle %d,%d,%d"), P.R, P.G, P.B);
			}
			Check(bRect && bWay && bCol, FString::Printf(TEXT("face %s: atlas rect %.4f,%.4f..%.4f,%.4f, the right way round %d%s"), F->Name, u0, v0, u1, v1, bWay ? 1 : 0, *Col));
			Matched += bRect && bWay && bCol ? 1 : 0;
		}
		Check(Matched == 6, FString::Printf(TEXT("all six faces %s (%d)"), bWantArt ? TEXT("textured from the atlas") : TEXT("drawn"), Matched));
		// kept as before: no collision, the outline's stencil, the glint
		Check(M->GetCollisionEnabled() == ECollisionEnabled::NoCollision, TEXT("no collision (as before)"));
		Check(M->bRenderCustomDepth && M->CustomDepthStencilValue == 4, TEXT("the ink outline's world-item stencil 4 (as before)"));
		UStaticMeshComponent* Glint = nullptr;
		TArray<UStaticMeshComponent*> Ms;
		Box->GetComponents(Ms);
		for (UStaticMeshComponent* S : Ms) if (S->GetName() == TEXT("Glint")) Glint = S;
		Check(Glint != nullptr && Glint->GetAttachParent() == M, TEXT("the glint there (as before)"));
		if (Glint)
		{
			const FVector G = Glint->GetRelativeLocation();
			Check(G.Z > B.Max.Z - 4.0 && G.Z < B.Max.Z && G.X > B.Max.X, FString::Printf(TEXT("the glint at a top front corner (%s)"), *G.ToCompactString()));
		}
		// the nearest pickup, for the scale picture
		TArray<AOutlawsLevelActor::FPickupDebug> Ps;
		L->DebugPickups(Ps);
		double Best = 400.0;
		for (const AOutlawsLevelActor::FPickupDebug& P : Ps)
		{
			const double D = FVector::Dist(P.Where, Box->GetActorLocation());
			if (D < Best && D > 30.0) { Best = D; BA.Prop = P.Where; BA.PropName = P.Ground; }
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT boxart the nearest pickup: %s"), BA.PropName.IsEmpty() ? TEXT("none within 4 m") : *FString::Printf(TEXT("%s, %.0f cm away"), *BA.PropName, Best));
		BA.Found0 = FoundCount();
		// the body out of reach while the camera is placed
		SetActorLocation(Box->GetActorLocation() - Front() * (Reach + 60.0) + FVector(0.0, 0.0, Half + 2.0), false, nullptr, ETeleportType::TeleportPhysics);
		GetCharacterMovement()->Velocity = FVector::ZeroVector;
		L->bTestKeepCollectBox = !BA.Tag.IsEmpty();       // (belt and braces while the pictures are taken)
		if (!BA.Tag.IsEmpty())
		{
			// a camera of its own, no motion blur or depth of field (it jumps from place to place), the player's field of view
			FActorSpawnParameters Sp;
			Sp.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			ACameraActor* NewCam = GetWorld()->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), FTransform(BoxMid() + Front() * 100.0), Sp);
			if (NewCam)
			{
				UCameraComponent* CC = NewCam->GetCameraComponent();
				CC->SetFieldOfView(Camera->FieldOfView);
				CC->SetConstraintAspectRatio(false);
				CC->PostProcessSettings.bOverride_MotionBlurAmount = true;
				CC->PostProcessSettings.MotionBlurAmount = 0.f;
				CC->PostProcessSettings.bOverride_DepthOfFieldFstop = true;
				CC->PostProcessSettings.DepthOfFieldFstop = 32.f;
				CC->PostProcessBlendWeight = 1.f;
				PC->SetViewTarget(NewCam);
				BA.Cam = NewCam;
			}
			Check(NewCam != nullptr, TEXT("the pictures' camera"));
		}
		BA.Step = BA.Tag.IsEmpty() ? 20 : 10;
		BA.At = Now + 0.5;
		return;
	}
	// the pictures: each placed for a few frames, then taken
	case 10: case 11: case 12: case 13: case 14: case 15: case 16: case 17:
	{
		if (!Box) { Check(false, TEXT("the box still there for the pictures")); Finish(); return; }
		const int32 Which = (BA.Step - 10) / 2;
		const bool bTake = (BA.Step - 10) % 2 == 1;
		const FVector Mid = BoxMid(), Fw = Front();
		const FVector Side = FVector::CrossProduct(FVector::UpVector, Fw).GetSafeNormal();
		const TCHAR* Name = Which == 0 ? TEXT("close") : Which == 1 ? TEXT("spine") : Which == 2 ? TEXT("pickup") : TEXT("prop");
		if (Which <= 2)
		{
			// face on 38 cm away; a front corner 40 cm away (the front and a spine); a standing eye at the walk-in reach.  The box
			// turns: wait (up to 20 s) until that place is clear of the room's walls
			const FVector From = Which == 0 ? Mid + Fw * 38.0 + Side * 10.0 + FVector(0.0, 0.0, 8.0)
				: Which == 1 ? Mid + (Fw * 0.75 - Side * 0.66).GetSafeNormal() * 40.0 + FVector(0.0, 0.0, 14.0)
				: Box->GetActorLocation() + Fw * Reach + FVector(0.0, 0.0, 160.0);
			FCollisionQueryParams Q(SCENE_QUERY_STAT(OLBoxArtProbe), false, Box);
			Q.AddIgnoredActor(this);
			FHitResult Hit;
			const bool bClear = !GetWorld()->LineTraceSingleByChannel(Hit, Mid, From + (From - Mid).GetSafeNormal() * 15.0, ECC_Visibility, Q);
			if (BA.WaitSince == 0.0) BA.WaitSince = Now;
			if (!bClear && Now - BA.WaitSince < 20.0)
			{
				if (bTake) BA.Step--;
				BA.At = Now + 0.03;
				return;
			}
			if (!bClear) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT boxart the %s picture's place is not clear after 20 s; taken anyway"), Name);
			Aim(From, Mid);
			if (bTake) BA.WaitSince = 0.0;
		}
		else
		{
			if (BA.PropName.IsEmpty()) { BA.Step = 20; return; }
			const FVector Between = (Mid + BA.Prop) * 0.5;
			FVector Across = FVector::CrossProduct(FVector::UpVector, (BA.Prop - Mid).GetSafeNormal2D());
			if (FVector::DotProduct(Across, Fw) < 0.0) Across = -Across;      // (from the box's front side)
			const double Back = FMath::Max(110.0, 0.9 * FVector::Dist2D(Mid, BA.Prop));
			Aim(Between + Across * Back + FVector(0.0, 0.0, 70.0), Between);
		}
		if (bTake) Shot(Name);
		BA.Step++;
		BA.At = Now + (bTake ? 0.6 : 0.25);
		return;
	}
	case 20:
	{
		// still taken by walking into it, as before
		if (!Box) { Check(false, TEXT("the box there for the walk")); Finish(); return; }
		if (ACameraActor* C = BA.Cam.Get()) { PC->SetViewTarget(this); C->Destroy(); }
		L->bTestKeepCollectBox = false;
		SetActorLocation(Box->GetActorLocation() + FVector(0.0, 0.0, Half + 2.0), false, nullptr, ETeleportType::TeleportPhysics);
		GetCharacterMovement()->Velocity = FVector::ZeroVector;
		BA.Step = 21;
		BA.At = Now + 1.0;
		return;
	}
	case 21:
	{
		const int32 N = FoundCount();
		Check(L->GetCollectBox() == nullptr && N == BA.Found0 + 1, FString::Printf(TEXT("walked into, taken: the profile's count %d to %d"), BA.Found0, N));
		Finish();
		return;
	}
	default:
		Finish();
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
