// REQ-MUZZLEFLASH3D, the desktop test: -OLShots -OLLevel=town -OLNoFrontEnd -OLFlash3DTest [-OLNoFlash3D] [-OLFlash3DSave].
// Each gun (revolver, rifle, shotgun, double barrel, sawed-off, gatling) is held side on before the eyes, fired once with
// the flash held on, and photographed from the side, at 45 degrees and from the front (the eye moved round the muzzle, the
// gun kept where it was): ol_flash3d_<gun>_<side|45|front>.png, or ol_flashfb_... with -OLNoFlash3D (today's flash).  Then
// the frame time: 2 s held still, 3 s of the rifle fired every 0.1 s, 3 s of the shotgun likewise ("OLFLASH3D perf" lines).
// "OLFLASH3D check PASS|FAIL" per check, "OLFLASH3D test RESULT ALL PASS" at the end.
#include "OutlawsPlayer.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsBoxHand.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/Controller.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "RHI.h"
#include "DynamicRHI.h"
#include "UnrealClient.h"
#include "OutlawsShotTests.h"

#if WITH_OUTLAWS_TESTS

namespace
{
	constexpr int32 NumFlashGuns = 6;
	const int32 GFlashGunKind[NumFlashGuns] = { 0, 1, 2, 2, 2, 1 };       // revolver, rifle, shotgun, double, sawed-off, gatling
	const TCHAR* GFlashGunName[NumFlashGuns] = { TEXT("revolver"), TEXT("rifle"), TEXT("shotgun"), TEXT("double"), TEXT("sawedoff"), TEXT("gatling") };
	const TCHAR* GFlashView[3] = { TEXT("side"), TEXT("45"), TEXT("front") };
	constexpr float GunPeriod = 4.f;

	struct FFlashTestState
	{
		bool bStarted = false;
		FVector BaseLoc = FVector::ZeroVector;
		float BaseYaw = 0.f;
		FTransform GunPose;
		FVector Muzzle = FVector::ZeroVector, Bore = FVector::ForwardVector, Side = FVector::RightVector;
		int32 Fails = 0;
		int32 ShownBefore[AOutlawsLevelActor::NumFlashKinds] = { 0, 0, 0 };
		// the frame time windows: 0 still, 1 rifle, 2 shotgun
		TArray<float> Dts[3];
		TArray<float> Gpu[3];
		int32 Shots[3] = { 0, 0, 0 };
		float NextShot = 0.f;
	};
	FFlashTestState GFt;

	void FtCheck(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLFLASH3D check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (!bOk) ++GFt.Fails;
	}

	FString FtStats(const TArray<float>& V)
	{
		if (V.Num() == 0) return TEXT("no frames");
		TArray<float> S = V;
		S.Sort();
		double Sum = 0.0;
		for (const float X : S) Sum += X;
		return FString::Printf(TEXT("mean %.2f ms, median %.2f, p95 %.2f, max %.2f over %d frames"), Sum / S.Num(), S[S.Num() / 2], S[FMath::Min(S.Num() - 1, int32(S.Num() * 0.95))], S.Last(), S.Num());
	}
}

void AOutlawsPlayer::Flash3DTestTick(float Dt)
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L || !Camera) return;
	static const bool bFallback = OLFlags::Param(TEXT("OLNoFlash3D"));
	const TCHAR* Prefix = bFallback ? TEXT("ol_flashfb_") : TEXT("ol_flash3d_");
	if (!GFt.bStarted)
	{
		if (ShotTime < 3.f) return;
		GFt.bStarted = true;
		GEngine->Exec(GetWorld(), TEXT("r.MotionBlurQuality 0"));
		GFt.BaseLoc = GetActorLocation();
		GFt.BaseYaw = static_cast<float>(GetActorRotation().Yaw);
		L->GiveAllWeapons();
		UE_LOG(LogOutlaws, Display, TEXT("OLFLASH3D test: %s; %s"), bFallback ? TEXT("the fallback (-OLNoFlash3D)") : TEXT("3D flashes"), *L->Flash3DReport);
		for (int32 k = 0; k < AOutlawsLevelActor::NumFlashKinds; ++k)
		{
			FtCheck(bFallback ? !L->HasFlash3D(k) : L->HasFlash3D(k), FString::Printf(TEXT("the %s flash %s"), AOutlawsLevelActor::FlashKindName(k),
				bFallback ? TEXT("is off: today's flash") : TEXT("was cut from the sprites")));
		}
		ShotStep = 0;
		ShotTime = 0.f;
	}
	if (LeftHand) LeftHand->SetVisibility(false, true);
	auto PlaceEye = [this](const FVector& Eye, const FVector& LookAt)
	{
		SetActorLocation(GetActorLocation() + (Eye - Camera->GetComponentLocation()), false, nullptr, ETeleportType::TeleportPhysics);
		if (Controller) Controller->SetControlRotation((LookAt - Eye).Rotation());
	};
	auto PoseGun = [this](int32 Hand, const FVector& Muzzle, const FVector& Bore)
	{
		// the gun's frame put so its front (its bounds' far end along the barrel) is at Muzzle, the barrel along Bore
		const FBox B = GunBounds[Hand];
		const double Front = B.IsValid ? B.Max.X : 20.0;
		const FQuat Q = FRotationMatrix::MakeFromXZ(Bore, FVector::UpVector).ToQuat();
		FrameOf(Hand)->SetWorldLocationAndRotation(Muzzle - Q.RotateVector(FVector(Front, 0.0, 0.0)), Q);
		if (UOutlawsBoxHand* Box = (Hand == 1 ? RightHand.Get() : LeftHand.Get()))
		{
			Box->SetVisibility(true, true);
			Box->SetCurl(1.f, 1.f);
		}
	};
	const int32 Gun = FMath::FloorToInt(ShotTime / GunPeriod);
	if (Gun < NumFlashGuns)
	{
		const float T = ShotTime - Gun * GunPeriod;
		const int32 Wi = Gun;
		const int32 Kind = GFlashGunKind[Gun];
		const FString Name = GFlashGunName[Gun];
		if (ShotStep == Gun * 8 + 0)
		{
			if (Controller) Controller->SetControlRotation(FRotator(0.0, GFt.BaseYaw, 0.0));
			SetActorLocation(GFt.BaseLoc, false, nullptr, ETeleportType::TeleportPhysics);
			Equip(1, Wi);
			if (Mag[1] <= 0) Mag[1] = 2;
			++ShotStep;
		}
		if (ShotStep == Gun * 8 + 1)
		{
			// side on before the eyes, the barrel to the right, the muzzle a little right of the middle
			const FVector Eye = Camera->GetComponentLocation(), F = Camera->GetForwardVector(), R = Camera->GetRightVector();
			GFt.Muzzle = Eye + F * 60.0 - R * 6.0;
			GFt.Bore = R;
			GFt.Side = F;
			PoseGun(1, GFt.Muzzle, GFt.Bore);
			if (T < 1.f) return;
			for (int32 k = 0; k < AOutlawsLevelActor::NumFlashKinds; ++k) GFt.ShownBefore[k] = L->Flash3DShown[k];
			L->bFlash3DHold = true;
			FireCooldown[1] = 0.f;
			if (Mag[1] <= 0) Mag[1] = 2;
			const int32 Before = Mag[1];
			FireGun(1, false);
			GFt.GunPose = FrameOf(1)->GetComponentTransform();
			const bool bShot = Mag[1] < Before;
			if (Gun == 5 && Equipped[1] != Wi)
			{
				// (the gatling may only be mounted, not held: nothing to see of it here)
				UE_LOG(LogOutlaws, Display, TEXT("OLFLASH3D gatling: not held in a hand on the desktop, left out"));
				L->bFlash3DHold = false;
				ShotStep = Gun * 8 + 8;
				ShotTime = (Gun + 1) * GunPeriod;
				return;
			}
			FtCheck(Equipped[1] == Wi && bShot, FString::Printf(TEXT("%s held and fired (rounds %d -> %d)"), *Name, Before, Mag[1]));
			if (bFallback)
			{
				FtCheck(L->Flash3DVisible() == 0 && MuzzleFx[1] && MuzzleFx[1]->IsVisible(), FString::Printf(TEXT("%s: today's flash shown, no 3D flash"), *Name));
			}
			else
			{
				const bool bKind = L->Flash3DShown[Kind] == GFt.ShownBefore[Kind] + 1;
				FtCheck(bKind && L->Flash3DVisible() == 1 && !(MuzzleFx[1] && MuzzleFx[1]->IsVisible()), FString::Printf(TEXT("%s: the %s's 3D flash shown, today's glow not (shown %d)"), *Name,
					AOutlawsLevelActor::FlashKindName(Kind), L->Flash3DVisible()));
			}
			++ShotStep;
			return;
		}
		// the gun kept where it fired from while the eye goes round the muzzle
		FrameOf(1)->SetWorldTransform(GFt.GunPose);
		const int32 Sub = ShotStep - Gun * 8;          // 2 side shot, 3 move 45, 4 45 shot, 5 move front, 6 front shot, 7 release
		const float Due[8] = { 0.f, 1.f, 1.4f, 1.8f, 2.2f, 2.6f, 3.0f, 3.4f };
		if (Sub >= 2 && Sub <= 7 && T >= Due[Sub])
		{
			if (Sub == 2 || Sub == 4 || Sub == 6)
			{
				const FString File = FString::Printf(TEXT("%s%s_%s.png"), Prefix, *Name, GFlashView[(Sub - 2) / 2]);
				FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / File, false, false);
				UE_LOG(LogOutlaws, Display, TEXT("OLFLASH3D shot %s: 3D slots on %d, today's glow %s"), *File, L->Flash3DVisible(), MuzzleFx[1] && MuzzleFx[1]->IsVisible() ? TEXT("on") : TEXT("off"));
			}
			else if (Sub == 3)
			{
				// 45 degrees between the side and the front, as far off
				const FVector Dir = (GFt.Bore - GFt.Side).GetSafeNormal();
				PlaceEye(GFt.Muzzle + Dir * 60.0 + GFt.Bore * 4.0, GFt.Muzzle + GFt.Bore * 6.0);
			}
			else if (Sub == 5)
			{
				// in front of the muzzle, a hair off the barrel's line, looking down it
				PlaceEye(GFt.Muzzle + GFt.Bore * 70.0 + FVector(0.0, 0.0, 3.0), GFt.Muzzle);
			}
			else if (Sub == 7)
			{
				L->FlashHoldRelease();
			}
			++ShotStep;
		}
		return;
	}
	// ---- the frame time while firing fast ----
	const float Tp = ShotTime - NumFlashGuns * GunPeriod;
	if (ShotStep == NumFlashGuns * 8)
	{
		L->FlashHoldRelease();
		SetActorLocation(GFt.BaseLoc, false, nullptr, ETeleportType::TeleportPhysics);
		if (Controller) Controller->SetControlRotation(FRotator(0.0, GFt.BaseYaw, 0.0));
		Equip(1, 1);
		++ShotStep;
		return;
	}
	{
		const FVector Eye = Camera->GetComponentLocation(), F = Camera->GetForwardVector(), R = Camera->GetRightVector();
		PoseGun(1, Eye + F * 60.0 - R * 6.0, R);
	}
	const int32 Win = Tp >= 1.f && Tp < 3.f ? 0 : Tp >= 3.f && Tp < 6.f ? 1 : Tp >= 6.5f && Tp < 9.5f ? 2 : -1;
	if (ShotStep == NumFlashGuns * 8 + 1 && Tp >= 6.f)
	{
		Equip(1, 2);
		++ShotStep;
	}
	if (Win >= 0)
	{
		GFt.Dts[Win].Add(Dt * 1000.f);
		GFt.Gpu[Win].Add(FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0)));
		if (Win > 0)
		{
			GFt.NextShot -= Dt;
			if (GFt.NextShot <= 0.f)
			{
				GFt.NextShot = 0.1f;
				Mag[1] = FMath::Max(Mag[1], 2);
				FireCooldown[1] = 0.f;
				FireGun(1, false);
				++GFt.Shots[Win];
			}
		}
	}
	if (Tp >= 10.f && ShotStep == NumFlashGuns * 8 + 2)
	{
		const TCHAR* WinName[3] = { TEXT("still"), TEXT("rifle every 0.1 s"), TEXT("shotgun every 0.1 s") };
		for (int32 w = 0; w < 3; ++w)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLFLASH3D perf %s %s: frame %s; gpu %s; shots %d"), bFallback ? TEXT("fallback") : TEXT("3d"), WinName[w], *FtStats(GFt.Dts[w]), *FtStats(GFt.Gpu[w]), GFt.Shots[w]);
		}
		FtCheck(GFt.Shots[1] >= 20 && GFt.Shots[2] >= 20, FString::Printf(TEXT("fired fast: %d rifle and %d shotgun shots"), GFt.Shots[1], GFt.Shots[2]));
		UE_LOG(LogOutlaws, Display, TEXT("OLFLASH3D shown by kind: pistol %d, rifle %d, shotgun %d"), L->Flash3DShown[0], L->Flash3DShown[1], L->Flash3DShown[2]);
		UE_LOG(LogOutlaws, Display, TEXT("OLFLASH3D test RESULT %s (%d failed)"), GFt.Fails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), GFt.Fails);
		++ShotStep;
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

OL_SHOT_MATCH(OLFlash3DTest) { return OLFlags::Param(TEXT("OLFlash3DTest")); }
OL_SHOT_TEST(OLFlash3DTest, 80)
{
	Flash3DTestTick(Dt);             // (REQ-MUZZLEFLASH3D: OutlawsPlayerFlashTest.cpp)
}
#endif // WITH_OUTLAWS_TESTS
