// -OLShots -OLTntTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

namespace
{
	float FuseTestAt = 0.f;              // (BUG-FUSEENSURE: when the held stick's tree is checked)
}

OL_SHOT_MATCH(OLTntTest) { return OLFlags::Param(TEXT("OLTntTest")); }

OL_SHOT_TEST(OLTntTest, 1120)
{
	// (BUG-TNTWHITE) a stick of dynamite lit in the right hand after the fire look is loaded (a lantern or a thrown
	// stick does it in play): shots 0, 2, 5 and 10 frames after lighting (ol_tnt_f0..f10), then the stick close
	// before the eyes for the cord's colour (ol_tnt_fuse_close); one OLRT tnt line per shot with the flame's size
	static float HoldYaw = -1000.f;
	static uint64 LitFrame = 0;
	if (HoldYaw < -999.f) HoldYaw = static_cast<float>(GetActorRotation().Yaw);
	const bool bClose = ShotStep >= 7;
	if (Controller) Controller->SetControlRotation(FRotator(bClose ? -10.0 : -30.0, HoldYaw, 0.0));
	const FVector Eye = Camera->GetComponentLocation();
	const FVector F = Camera->GetForwardVector(), R = Camera->GetRightVector();
	const FVector Fl = FVector(F.X, F.Y, 0.0).GetSafeNormal();
	if (UOutlawsBoxHand* Box = RightHand.Get())
	{
		const FVector Pos = bClose ? Eye + F * 22.0 + R * 2.0 - FVector::UpVector * 8.0 : Eye + Fl * 58.0 + R * 14.0 - FVector::UpVector * 32.0;     // (at arm's length, as held in the headset)
		Box->SetVisibility(true, true);
		FrameOf(1)->SetWorldLocationAndRotation(Pos, FRotationMatrix::MakeFromXZ((Fl - R * 0.25).GetSafeNormal(), FVector::UpVector).ToQuat());
		Box->SetCurl(1.f, 1.f);
	}
	auto Shot = [this](const TCHAR* Name)
	{
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(Name) + TEXT(".png"), false, false);
		const UStaticMeshComponent* Fm = FuseFlame[1].Get();
		UE_LOG(LogOutlaws, Display, TEXT("OLRT tnt %s: frame +%llu, lit %d, flame %s visible %d scale %s, eye to flame %.1f cm"), Name,
			GFrameCounter - LitFrame, bFuseLit[1] ? 1 : 0, Fm && Fm->GetStaticMesh() ? *Fm->GetStaticMesh()->GetName() : TEXT("none"),
			Fm && Fm->IsVisible() ? 1 : 0, Fm ? *Fm->GetComponentScale().ToString() : TEXT("-"),
			Fm ? FVector::Dist(Fm->GetComponentLocation(), Camera->GetComponentLocation()) : -1.0);
	};
	AOutlawsLevelActor* Lt = Level.Get();
	if (ShotStep == 0 && ShotTime > 3.f)
	{
		GEngine->Exec(GetWorld(), TEXT("r.MotionBlurQuality 0"));
		if (Lt) { Lt->bInvincible = true; Lt->GiveAllWeapons(); }
		// the fire look loaded, as a lantern or a thrown stick leaves it
		if (Lt && FireCells.Num() == 0) { if (UMaterialInstanceDynamic* M = Lt->MakeFireLook(this, FireCells, FireFps, FireAspect)) KeepAlive.Add(M); }
		UE_LOG(LogOutlaws, Display, TEXT("OLRT tnt: fire cells %d"), FireCells.Num());
		Equip(1, 6);
		Mag[1] = 1;
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 5.f) { Shot(TEXT("ol_tnt_unlit")); ++ShotStep; }
	else if (ShotStep == 2 && ShotTime > 6.f)
	{
		bFuseLit[1] = true;
		Fuse[1] = 4.5f;
		LitFrame = GFrameCounter;
		UpdateFuseLook(1);
		Shot(TEXT("ol_tnt_f0"));
		++ShotStep;
	}
	else if (ShotStep == 3 && GFrameCounter >= LitFrame + 2) { Shot(TEXT("ol_tnt_f2")); ++ShotStep; }
	else if (ShotStep == 4 && GFrameCounter >= LitFrame + 5) { Shot(TEXT("ol_tnt_f5")); ++ShotStep; }
	else if (ShotStep == 5 && GFrameCounter >= LitFrame + 10) { Shot(TEXT("ol_tnt_f10")); ++ShotStep; }
	else if (ShotStep == 6 && ShotTime > 7.f) ++ShotStep;
	else if (ShotStep == 7 && ShotTime > 7.6f) { Shot(TEXT("ol_tnt_fuse_close")); ++ShotStep; }
	else if (ShotStep == 8 && ShotTime > 8.2f) ++ShotStep;
	// BUG-FUSEENSURE: a lit stick in each hand, then the hand's controller tree as the late update gathers it (the
	// engine's own FLateUpdateManager::Setup, and every part walked here): no part placed in world space (the gate
	// also wants no 'Ensure condition failed' in the log)
	else if (ShotStep == 9 || ShotStep == 11)
	{
		const int32 H = ShotStep == 9 ? 0 : 1;
		if (UOutlawsBoxHand* Box = H == 0 ? LeftHand.Get() : RightHand.Get())
		{
			const double Side = H == 0 ? -1.0 : 1.0;
			Box->SetVisibility(true, true);
			FrameOf(H)->SetWorldLocationAndRotation(Eye + Fl * 58.0 + R * Side * 14.0 - FVector::UpVector * 32.0, FRotationMatrix::MakeFromXZ((Fl - R * Side * 0.25).GetSafeNormal(), FVector::UpVector).ToQuat());
			Box->SetCurl(1.f, 1.f);
		}
		Equip(1 - H, -1);
		Equip(H, 6);
		Mag[H] = 1;
		bFuseLit[H] = true;
		Fuse[H] = 4.5f;
		UpdateFuseLook(H);
		FuseTestAt = ShotTime + 0.5f;
		++ShotStep;
	}
	else if ((ShotStep == 10 || ShotStep == 12) && ShotTime > FuseTestAt)
	{
		const int32 H = ShotStep == 10 ? 0 : 1;
		UMotionControllerComponent* Grip = H == 0 ? LeftGrip.Get() : RightGrip.Get();
		FString Bad;
		int32 Parts = 0;
		if (Grip)
		{
			TArray<USceneComponent*> Kids;
			Grip->GetChildrenComponents(true, Kids);
			Kids.Add(Grip);
			for (const USceneComponent* C : Kids)
			{
				++Parts;
				if (C && (C->IsUsingAbsoluteLocation() || C->IsUsingAbsoluteRotation())) Bad += FString::Printf(TEXT("%s%s"), Bad.IsEmpty() ? TEXT("") : TEXT(", "), *C->GetName());
			}
			static FLateUpdateManager GTestLateUpdate;
			const USceneComponent* Up = Grip->GetAttachParent();
			GTestLateUpdate.Setup(Up ? Up->GetComponentTransform() : FTransform::Identity, Grip, false);
		}
		const bool bFlame = FuseFlame[H] && FuseFlame[H]->IsVisible() && FuseFlame[H]->GetAttachParent() == WeaponMesh[H];
		UE_LOG(LogOutlaws, Display, TEXT("OLRT tnt fuseensure %s hand: lit %d, flame on the stick %d, %d parts under the grip, in world space: %s (the engine's ensure, if any, is logged as 'Ensure condition failed')"),
			H == 0 ? TEXT("left") : TEXT("right"), bFuseLit[H] ? 1 : 0, bFlame ? 1 : 0, Parts, Bad.IsEmpty() ? TEXT("none") : *Bad);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT tnt fuseensure check %s: a lit stick in the %s hand, no late-update part in world space"),
			Grip && bFlame && Bad.IsEmpty() ? TEXT("PASS") : TEXT("FAIL"), H == 0 ? TEXT("left") : TEXT("right"));
		bFuseLit[H] = false;
		UpdateFuseLook(H);
		++ShotStep;
	}
	else if (ShotStep == 13) { FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; }
	if (bFuseLit[1] && ShotStep >= 3 && ShotStep < 9) { Fuse[1] = FMath::Max(Fuse[1], 3.f); UpdateFuseLook(1); }
	if (ShotStep == 10 || ShotStep == 12) { const int32 H = ShotStep == 10 ? 0 : 1; Fuse[H] = FMath::Max(Fuse[H], 3.f); UpdateFuseLook(H); }
}

#endif // WITH_OUTLAWS_TESTS
