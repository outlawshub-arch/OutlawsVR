// -OLShots -OLHeldGunTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLHeldGunTest) { return OLFlags::Param(TEXT("OLHeldGunTest")); }

OL_SHOT_TEST(OLHeldGunTest, 1110)
{
	// the five held guns in turn in the right fist on the desktop (user 2026-10-02: the shipped guns), side on before
	// the eyes with the barrel to the right, each fired once and photographed: ol_heldgun_<name>_<dev|shipped|card>; then
	// the dynamite, not lit (BUG-TNT: its cord)
	if (ShotStep == 0) GEngine->Exec(GetWorld(), TEXT("r.MotionBlurQuality 0"));
	static float HeldGunYaw = -1000.f;
	if (HeldGunYaw < -999.f) HeldGunYaw = static_cast<float>(GetActorRotation().Yaw);
	if (Controller) Controller->SetControlRotation(FRotator(0.0, HeldGunYaw, 0.0));
	const FVector Eye = Camera->GetComponentLocation();
	const FVector F = Camera->GetForwardVector(), R = Camera->GetRightVector();
	if (LeftHand) LeftHand->SetVisibility(false, true);
	if (UOutlawsBoxHand* Box = RightHand.Get())
	{
		// far enough off for the whole gun to fit (90 degrees across), its middle in the middle of the view
		const FBox B = GunBounds[1];
		const double Span = B.IsValid ? B.GetSize().X : 30.0;
		const double Middle = B.IsValid ? B.GetCenter().X : 10.0;
		FrameOf(1)->SetWorldLocationAndRotation(Eye + F * FMath::Max(40.0, Span * 0.75) - R * Middle - FVector::UpVector * 4.0,
			FRotationMatrix::MakeFromXZ(R, FVector::UpVector).ToQuat());
		Box->SetVisibility(true, true);
		Box->SetCurl(1.f, 1.f);
	}
	static int32 RoundsBefore = 0;
	const int32 Gun = (ShotStep - 1) / 3, Phase = (ShotStep - 1) % 3;
	const int32 Wi = Gun < 5 ? Gun : 6;          // (6: the dynamite)
	const float Due = 3.f + 4.f * Gun + (Phase == 0 ? 0.f : Phase == 1 ? 1.5f : 2.5f);
	if (ShotStep == 0 && ShotTime > 3.f && L)
	{
		L->GiveAllWeapons();
		++ShotStep;
	}
	else if (ShotStep > 0 && Gun >= 6 && ShotTime > Due)
	{
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
	else if (ShotStep > 0 && ShotTime > Due)
	{
		if (Phase == 0)
		{
			Equip(1, Wi);
			if (Mag[1] <= 0) Mag[1] = 1;
			if (Wi == 6 && WeaponMesh[1])
			{
				// (BUG-TNT on the Quest) every section's state: the cord drew on the desktop, not on the mobile renderer
				for (int32 s = 0; s < WeaponMesh[1]->GetNumSections(); ++s)
				{
					const FProcMeshSection* Sec = WeaponMesh[1]->GetProcMeshSection(s);
					UMaterialInterface* M = WeaponMesh[1]->GetMaterial(s);
					UE_LOG(LogOutlaws, Display, TEXT("OLSHOT heldgun dynamite section %d%s: %d verts, %d indices, visible %d, box %s, material %s"), s, s == FuseSection[1] ? TEXT(" (fuse)") : TEXT(""),
						Sec ? Sec->ProcVertexBuffer.Num() : -1, Sec ? Sec->ProcIndexBuffer.Num() : -1, Sec ? int32(Sec->bSectionVisible) : -1, Sec ? *Sec->SectionLocalBox.ToString() : TEXT("-"), M ? *M->GetName() : TEXT("none"));
				}
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT heldgun %s: %s, %d sections, bounds %s"), *HeldGunTestName(Wi),
				Equipped[1] == Wi ? TEXT("held") : TEXT("NOT HELD"), WeaponMesh[1] ? WeaponMesh[1]->GetNumSections() : 0, *GunBounds[1].ToString());
		}
		else if (Phase == 1 && Wi != 6)
		{
			RoundsBefore = Mag[1];
			FireCooldown[1] = 0.f;
			FireGun(1, false);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT heldgun %s: fired, rounds %d -> %d (%s)"), *HeldGunTestName(Wi), RoundsBefore, Mag[1],
				Mag[1] < RoundsBefore ? TEXT("a shot") : TEXT("NO SHOT"));
		}
		else if (Phase == 2)
		{
			FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / (TEXT("ol_heldgun_") + HeldGunTestName(Wi) + TEXT(".png")), false, false);
		}
		++ShotStep;
	}
}

#endif // WITH_OUTLAWS_TESTS
