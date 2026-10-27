// -OLShots -OLHurtTintTest[=bars|water]: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "ShaderCompiler.h"

namespace
{
	// BUG-HURTTINTALPHA: the view held still, photographed with the hit's red off and then on (held all round at
	// HurtTintAmount); each part of the picture that kept its colour under the red is counted
	constexpr float HurtTintAmount = 0.5f;

	struct FHurtTintBox { FString Name; FBox2D Box = FBox2D(ForceInit); };

	struct FHurtTintState
	{
		FString View, What;
		TWeakObjectPtr<AOutlawsEnemy> Held;
		FVector HeldAt = FVector::ZeroVector;
		TArray<FHurtTintBox> Boxes;
		TArray<float> Gpu[2];
		int32 Fails = 0;
	};
	FHurtTintState GHt;

	bool LoadPng(const FString& Path, TArray<uint8>& Bgra, int32& W, int32& H)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path)) return false;
		IImageWrapperModule& Mod = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> Img = Mod.CreateImageWrapper(EImageFormat::PNG);
		if (!Img.IsValid() || !Img->SetCompressed(Bytes.GetData(), Bytes.Num()) || !Img->GetRaw(ERGBFormat::BGRA, 8, Bgra)) return false;
		W = int32(Img->GetWidth());
		H = int32(Img->GetHeight());
		return Bgra.Num() >= W * H * 4;
	}

	FString HtStats(const TArray<float>& V)
	{
		if (V.Num() == 0) return TEXT("no frames");
		TArray<float> S = V;
		S.Sort();
		return FString::Printf(TEXT("median %.3f ms, p95 %.3f ms over %d frames"), S[S.Num() / 2], S[FMath::Min(S.Num() - 1, int32(S.Num() * 0.95))], S.Num());
	}
}

OL_SHOT_MATCH(OLHurtTintTest)
{
	FString V;
	return OLFlags::Value(TEXT("OLHurtTintTest"), V, false) || OLFlags::Param(TEXT("OLHurtTintTest"));
}

OL_SHOT_TEST(OLHurtTintTest, 1225)
{
	// -OLHurtTintTest=bars (default): a see-through mid (-OLHurtTintMid=<n>, the n-th as -OLShots counts them, or
	// -OLHurtTintPose=x,y,z,pitch,yaw), the right hand and its pistol low in the view with the 3D muzzle flash held, a bad guy
	// held on the left.  =water: over the level's deepest water, looking down at its surface.  =start: the level's start
	// (the town's street, for the frame time).  Shots
	// Saved/OLShots/ol_hurttint_<view>_off|on.png; -OLHurtTintPerf adds the GPU time held still, the red off and on
	if (!L || !Camera) return;
	static const bool bPerf = OLFlags::Param(TEXT("OLHurtTintPerf"));
	if (ShotStep == 0)
	{
		if (ShotTime < 4.f) return;
#if WITH_EDITOR
		// the shaders first: a change to MPC_OLR_Lights recompiles every material that reads it, and until then the world draws
		// with the stand-in material (black); up to 10 minutes, the schedule below starting once they are done
		static const double WaitFrom = FPlatformTime::Seconds();
		static double LastSaid = 0.0;
		const double Now = FPlatformTime::Seconds();
		if (GShaderCompilingManager && GShaderCompilingManager->IsCompiling() && Now - WaitFrom < 600.0)
		{
			if (Now - LastSaid > 15.0)
			{
				LastSaid = Now;
				UE_LOG(LogOutlaws, Display, TEXT("OLHURTTINT waiting for the shaders: %d jobs left after %.0f s"), GShaderCompilingManager->GetNumRemainingJobs(), Now - WaitFrom);
			}
			ShotTime = 3.9f;
			return;
		}
		if (Now - WaitFrom > 1.0)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLHURTTINT shaders ready after %.0f s (%d jobs left)"), Now - WaitFrom, GShaderCompilingManager ? GShaderCompilingManager->GetNumRemainingJobs() : 0);
		}
#endif
		FString V;
		OLFlags::Value(TEXT("OLHurtTintTest"), V, false);
		GHt.View = V.IsEmpty() ? TEXT("bars") : V.ToLower();
		L->bInvincible = true;
		L->GiveAllWeapons();
		FVector At = FVector::ZeroVector;
		FRotator Look = FRotator::ZeroRotator;
		bool bPose = false;
		FString PoseArg;
		TArray<FString> Parts;
		if (OLFlags::Value(TEXT("OLHurtTintPose"), PoseArg, false) && PoseArg.ParseIntoArray(Parts, TEXT(",")) >= 5)
		{
			At = FVector(FCString::Atod(*Parts[0]), FCString::Atod(*Parts[1]), FCString::Atod(*Parts[2]));
			Look = FRotator(FCString::Atod(*Parts[3]), FCString::Atod(*Parts[4]), 0.0);
			GHt.What = TEXT("the pose given");
			bPose = true;
		}
		else if (GHt.View == TEXT("start"))
		{
			At = L->GetPlayerStartLocation();
			Look = FRotator(0.0, L->GetPlayerStartYaw(), 0.0);
			GHt.What = TEXT("the level's start");
			bPose = true;
		}
		else if (GHt.View == TEXT("water"))
		{
			FVector W;
			if (L->FindWaterPoint(W))
			{
				At = W + FVector(0.0, 0.0, 200.0);
				Look = FRotator(-55.0, L->GetPlayerStartYaw(), 0.0);
				GHt.What = FString::Printf(TEXT("the deepest water's surface at %s"), *W.ToCompactString());
				bPose = true;
			}
		}
		else
		{
			FString N;
			const int32 Nth = OLFlags::Value(TEXT("OLHurtTintMid"), N, false) ? FCString::Atoi(*N) : 0;
			FVector Floor;
			float Yaw = 0.f;
			if (L->DebugMidView(Nth, false, Floor, Yaw, GHt.What))
			{
				At = Floor + FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0);
				Look = FRotator(0.0, Yaw, 0.0);
				bPose = true;
			}
		}
		if (!bPose)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLHURTTINT test FAIL: no %s view in this level"), *GHt.View);
			UE_LOG(LogOutlaws, Display, TEXT("OLHURTTINT test RESULT FAIL (1 failed)"));
			FGenericPlatformMisc::RequestExit(false);
			ShotTime = -1.f;
			return;
		}
		TeleportTo(At, FRotator(0.0, Look.Yaw, 0.0), false, true);
		GetCharacterMovement()->DisableMovement();
		if (Controller) Controller->SetControlRotation(Look);
		Equip(1, 0);
		if (Mag[1] <= 0) Mag[1] = 2;
		UE_LOG(LogOutlaws, Display, TEXT("OLHURTTINT view %s: %s, at %s, look %s, feature level %s"), *GHt.View, *GHt.What, *At.ToCompactString(), *Look.ToCompactString(),
			GetWorld()->FeatureLevel <= ERHIFeatureLevel::ES3_1 ? TEXT("ES3_1") : TEXT("SM5"));
		++ShotStep;
		return;
	}
	// every frame: the HUD lines out of the picture, the pistol held low on the right side on (barrel to the right, the
	// hand round it), the bad guy held where the eye sees him
	if (HudText) HudText->SetHiddenInGame(true);
	if (PerfHudText) PerfHudText->SetHiddenInGame(true);
	if (LeftHand) LeftHand->SetVisibility(false, true);
	const FVector Eye = Camera->GetComponentLocation(), F = Camera->GetForwardVector(), R = Camera->GetRightVector(), U = Camera->GetUpVector();
	{
		const FVector Muzzle = Eye + F * 55.0 + R * 16.0 - U * 13.0;
		const FBox B = GunBounds[1];
		const double Front = B.IsValid ? B.Max.X : 20.0;
		const FQuat Q = FRotationMatrix::MakeFromXZ(R, U).ToQuat();
		FrameOf(1)->SetWorldLocationAndRotation(Muzzle - Q.RotateVector(FVector(Front, 0.0, 0.0)), Q);
		if (UOutlawsBoxHand* Box = RightHand.Get())
		{
			Box->SetVisibility(true, true);
			Box->SetCurl(1.f, 1.f);
		}
	}
	if (ShotStep == 1)
	{
		if (GHt.View != TEXT("water"))
		{
			double Best = 1e30;
			for (TActorIterator<AOutlawsEnemy> It(GetWorld()); It; ++It)
			{
				const double D = FVector::DistSquared(It->GetActorLocation(), Eye);
				if (D < Best && It->GetSprite()) { Best = D; GHt.Held = *It; }
			}
			FHitResult Hit;
			FCollisionQueryParams Qp(SCENE_QUERY_STAT(OutlawsHurtTint), true, this);
			const FVector Flat = FVector(F.X, F.Y, 0.0).GetSafeNormal();
			const FVector Spot = Eye + Flat * 140.0 - R * 62.0;
			GHt.HeldAt = GetWorld()->LineTraceSingleByChannel(Hit, Spot, Spot - FVector(0.0, 0.0, 600.0), ECC_Visibility, Qp) ? Hit.ImpactPoint + FVector(0.0, 0.0, 90.0) : Spot - FVector(0.0, 0.0, 70.0);
		}
		++ShotStep;
		return;
	}
	if (AOutlawsEnemy* E = GHt.Held.Get())
	{
		E->SetActorLocation(GHt.HeldAt, false, nullptr, ETeleportType::TeleportPhysics);
		if (ShotStep == 3)
		{
			// still from here on: the same frame of him in both pictures
			E->GetCharacterMovement()->DisableMovement();
			E->SetActorTickEnabled(false);
			if (AOutlawsSpriteActor* S = E->GetSprite()) S->SetActorTickEnabled(false);
		}
	}
	if (ShotStep == 2 && ShotTime > 6.f)
	{
		L->bFlash3DHold = true;
		FireCooldown[1] = 0.f;
		if (Mag[1] <= 0) Mag[1] = 2;
		const int32 Before = Mag[1];
		FireGun(1, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLHURTTINT pistol fired (rounds %d -> %d), 3D flashes on %d, held %s"), Before, Mag[1], L->Flash3DVisible(),
			GHt.Held.IsValid() ? *GHt.Held->GetName() : TEXT("nobody"));
		++ShotStep;
		return;
	}
	const FString Prefix = FPaths::ProjectSavedDir() / TEXT("OLShots") / (TEXT("ol_hurttint_") + GHt.View);
	if (ShotStep == 3 && ShotTime > 7.5f)
	{
		FScreenshotRequest::RequestScreenshot(Prefix + TEXT("_off.png"), false, false);
		// where the parts are on the screen (the picture's own size is read back with it)
		GHt.Boxes.Reset();
		APlayerController* PC = Cast<APlayerController>(Controller);
		FVector2D Vs(1.0, 1.0);
		if (GEngine && GEngine->GameViewport) GEngine->GameViewport->GetViewportSize(Vs);
		// a part's box on the screen from its points (a mesh's own vertices: the held parts' bounds are widened for the
		// culling), a few pixels wider for its ink line
		auto AddPoints = [&](const FString& Name, const TArray<FVector>& Pts)
		{
			FHurtTintBox Hb;
			Hb.Name = Name;
			for (const FVector& P : Pts)
			{
				FVector2D S;
				if (PC && PC->ProjectWorldLocationToScreen(P, S, true)) Hb.Box += FVector2D(S.X / Vs.X, S.Y / Vs.Y);
			}
			if (Hb.Box.bIsValid)
			{
				Hb.Box = Hb.Box.ExpandBy(FVector2D(4.0 / Vs.X, 4.0 / Vs.Y));
				Hb.Box.Min = Hb.Box.Min.ClampAxes(0.0, 1.0);
				Hb.Box.Max = Hb.Box.Max.ClampAxes(0.0, 1.0);
			}
			GHt.Boxes.Add(Hb);
		};
		auto MeshPoints = [](const UProceduralMeshComponent* Pm, TArray<FVector>& Out)
		{
			if (!Pm || !Pm->IsVisible()) return;
			const FTransform T = Pm->GetComponentTransform();
			for (int32 s = 0; s < Pm->GetNumSections(); ++s)
			{
				const FProcMeshSection* Sec = const_cast<UProceduralMeshComponent*>(Pm)->GetProcMeshSection(s);
				if (!Sec || !Sec->bSectionVisible) continue;
				for (const FProcMeshVertex& V : Sec->ProcVertexBuffer) Out.Add(T.TransformPosition(V.Position));
			}
		};
		FHurtTintBox Whole;
		Whole.Name = TEXT("whole picture");
		Whole.Box = FBox2D(FVector2D(0.0, 0.0), FVector2D(1.0, 1.0));
		GHt.Boxes.Add(Whole);
		FHurtTintBox Mid;
		Mid.Name = GHt.View == TEXT("water") ? TEXT("water (the picture's middle)") : TEXT("the mid (the picture's middle)");
		Mid.Box = FBox2D(FVector2D(0.3, 0.25), FVector2D(0.7, 0.6));
		GHt.Boxes.Add(Mid);
		TArray<FVector> Pts;
		MeshPoints(WeaponMesh[1], Pts);
		AddPoints(TEXT("pistol and its ink line"), Pts);
		Pts.Reset();
		if (RightHand)
		{
			TArray<USceneComponent*> Kids;
			RightHand->GetChildrenComponents(true, Kids);
			for (USceneComponent* K : Kids) MeshPoints(Cast<UProceduralMeshComponent>(K), Pts);
		}
		AddPoints(TEXT("hand and its ink shell"), Pts);
		Pts.Reset();
		for (const AOutlawsLevelActor::FFlash3DSlot& S : L->Flash3DSlots) MeshPoints(S.Mesh.Get(), Pts);
		AddPoints(TEXT("muzzle flash"), Pts);
		Pts.Reset();
		if (AOutlawsEnemy* E = GHt.Held.Get())
		{
			// (the quad's own mesh box: its bounds are scaled up 3x for the culling)
			AOutlawsSpriteActor* S = E->GetSprite();
			if (S && S->Quad && S->Quad->GetStaticMesh())
			{
				const FBox Lb = S->Quad->GetStaticMesh()->GetBoundingBox();
				const FTransform T = S->Quad->GetComponentTransform();
				for (int32 c = 0; c < 8 && Lb.IsValid; ++c) Pts.Add(T.TransformPosition(FVector(c & 1 ? Lb.Max.X : Lb.Min.X, c & 2 ? Lb.Max.Y : Lb.Min.Y, c & 4 ? Lb.Max.Z : Lb.Min.Z)));
			}
			AddPoints(TEXT("bad guy's sprite"), Pts);
		}
		++ShotStep;
		return;
	}
	if (ShotStep == 4 && ShotTime > 8.f) { HurtPin = HurtTintAmount; ++ShotStep; return; }
	if (ShotStep == 5 && ShotTime > 9.5f) { FScreenshotRequest::RequestScreenshot(Prefix + TEXT("_on.png"), false, false); ++ShotStep; return; }
	if (ShotStep == 6 && ShotTime > 10.5f)
	{
		TArray<uint8> Off, On;
		int32 W0 = 0, H0 = 0, W1 = 0, H1 = 0;
		const bool bRead = LoadPng(Prefix + TEXT("_off.png"), Off, W0, H0) && LoadPng(Prefix + TEXT("_on.png"), On, W1, H1) && W0 == W1 && H0 == H1;
		if (!bRead)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLHURTTINT test FAIL: the two pictures could not be read back"));
			++GHt.Fails;
		}
		for (const FHurtTintBox& Hb : GHt.Boxes)
		{
			if (!bRead) break;
			if (!Hb.Box.bIsValid || Hb.Box.GetArea() <= 0.0)
			{
				UE_LOG(LogOutlaws, Display, TEXT("OLHURTTINT %s: not in the picture"), *Hb.Name);
				if (GHt.View != TEXT("water") || Hb.Name.StartsWith(TEXT("pistol")) || Hb.Name.StartsWith(TEXT("hand")))
				{
					UE_LOG(LogOutlaws, Display, TEXT("OLHURTTINT test FAIL: %s missing from the view"), *Hb.Name);
					++GHt.Fails;
				}
				continue;
			}
			const int32 X0 = FMath::Clamp(int32(Hb.Box.Min.X * W0), 0, W0 - 1), X1 = FMath::Clamp(int32(Hb.Box.Max.X * W0), 0, W0 - 1);
			const int32 Y0 = FMath::Clamp(int32(Hb.Box.Min.Y * H0), 0, H0 - 1), Y1 = FMath::Clamp(int32(Hb.Box.Max.Y * H0), 0, H0 - 1);
			int64 N = 0, Kept = 0;
			double Roff = 0.0, Goff = 0.0, Ron = 0.0, Gon = 0.0;
			for (int32 y = Y0; y <= Y1; ++y)
			{
				for (int32 x = X0; x <= X1; ++x)
				{
					const int32 i = (y * W0 + x) * 4;
					const int32 db = FMath::Abs(int32(On[i]) - int32(Off[i])), dg = FMath::Abs(int32(On[i + 1]) - int32(Off[i + 1])), dr = FMath::Abs(int32(On[i + 2]) - int32(Off[i + 2]));
					// a pixel the red left as it was: the same colour, or (a moving one: water, a flame) no redder against its green
					// (a pure red one, or one whose green is clipped white in both, has nothing to show it with: not counted)
					const bool bNoShow = (Off[i + 2] > 200 && Off[i + 1] < 40 && Off[i] < 40) || (Off[i + 1] >= 250 && On[i + 1] >= 250);
					const double Gain = ((On[i + 2] + 4.0) / (On[i + 1] + 4.0)) / ((Off[i + 2] + 4.0) / (Off[i + 1] + 4.0));
					if (!bNoShow && (FMath::Max3(db, dg, dr) <= 6 || Gain < 1.06)) ++Kept;
					++N;
					Roff += Off[i + 2]; Goff += Off[i + 1]; Ron += On[i + 2]; Gon += On[i + 1];
				}
			}
			const double Frac = N > 0 ? double(Kept) / double(N) : 1.0;
			const double Red = (Roff > 0.0 && Gon > 0.0) ? (Ron / FMath::Max(Gon, 1.0)) / (Roff / FMath::Max(Goff, 1.0)) : 0.0;
			const double Limit = Hb.Name == TEXT("whole picture") ? 0.003 : 0.01;
			const bool bOk = Frac <= Limit;
			UE_LOG(LogOutlaws, Display, TEXT("OLHURTTINT %s %s: %lld of %lld pixels kept their colour (%.2f%%, at most %.1f%%), red over green %.2fx the untinted picture's, box %s"),
				bOk ? TEXT("PASS") : TEXT("FAIL"), *Hb.Name, Kept, N, Frac * 100.0, Limit * 100.0, Red, *Hb.Box.ToString());
			if (!bOk) ++GHt.Fails;
		}
		++ShotStep;
		if (!bPerf)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLHURTTINT test RESULT %s (%d failed)"), GHt.Fails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), GHt.Fails);
			HurtPin = -1.f;
			L->FlashHoldRelease();
			FGenericPlatformMisc::RequestExit(false);
			ShotTime = -1.f;
		}
		return;
	}
	// -OLHurtTintPerf: the GPU's frame time held still, 4 s with the red off, then 4 s with it on
	if (ShotStep == 7)
	{
		const float T = ShotTime - 10.5f;
		HurtPin = T < 5.f ? 0.f : HurtTintAmount;
		const int32 Win = T >= 1.f && T < 5.f ? 0 : T >= 6.f && T < 10.f ? 1 : -1;
		if (Win >= 0) GHt.Gpu[Win].Add(FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0)));
		if (T >= 10.f)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLHURTTINT perf %s, red off: gpu %s"), *GHt.View, *HtStats(GHt.Gpu[0]));
			UE_LOG(LogOutlaws, Display, TEXT("OLHURTTINT perf %s, red on: gpu %s"), *GHt.View, *HtStats(GHt.Gpu[1]));
			UE_LOG(LogOutlaws, Display, TEXT("OLHURTTINT test RESULT %s (%d failed)"), GHt.Fails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), GHt.Fails);
			HurtPin = -1.f;
			L->FlashHoldRelease();
			FGenericPlatformMisc::RequestExit(false);
			ShotTime = -1.f;
		}
	}
}

#endif // WITH_OUTLAWS_TESTS
