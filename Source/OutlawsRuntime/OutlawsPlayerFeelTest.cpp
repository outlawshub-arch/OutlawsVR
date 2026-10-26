// -OLShots -OLFeelTest (user 2026-10-02): three VR feel fixes checked on the desktop, each logged.
//   A: a pickup put at the feet is walked over: its flight to the chest starts and ends ("OLRT collect fly start / end").
//   B: the longest flight of stairs down found near the start (or near a bad guy) is walked down: per frame the feet's
//      height and the eye's, and the most either moved in one frame (the eye eases, the feet snap).
//   C: a man held 12 cm off the chest for a second and a half, the hand swaying a little: per frame his view direction
//      and the quad's yaw, and how often the direction changed.

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsEnemy.h"
#include "OutlawsSpriteActor.h"
#include "Camera/CameraComponent.h"
#include "MotionControllerComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Misc/CommandLine.h"
#include "OutlawsShotTests.h"

#if WITH_OUTLAWS_TESTS

void AOutlawsPlayer::FeelTestTick(float Dt)
{
	AOutlawsLevelActor* L = Level.Get();
	UWorld* World = GetWorld();
	if (!L || !World) return;
	const double Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	auto FeetNow = [&]() { return GetActorLocation() - FVector(0.0, 0.0, Half); };

	// ---- A: walked over ----
	static TArray<TWeakObjectPtr<AActor>> Cands;
	static int32 Tried = 0;
	static TWeakObjectPtr<AActor> Cur;
	static FVector Home = FVector::ZeroVector;
	static float CurAt = 0.f;
	static FString CurName;
	if (ShotStep == 0 && ShotTime > 3.f)
	{
		const FVector Me = GetActorLocation();
		for (TActorIterator<AOutlawsSpriteActor> It(World); It; ++It)
		{
			AOutlawsSpriteActor* S = *It;
			const FVector B = S->GetActorLocation();
			const FVector Mid = B + FVector(0.0, 0.0, FMath::Max(1.0, S->GetActorScale3D().Z) * 0.5);
			if (L->FindPickupNear(Mid + FVector(0.0, 0.0, 40.0), Mid, 5.f) == S) Cands.Add(S);
		}
		Cands.Sort([Me](const TWeakObjectPtr<AActor>& X, const TWeakObjectPtr<AActor>& Y)
			{ return FVector::DistSquared(X->GetActorLocation(), Me) < FVector::DistSquared(Y->GetActorLocation(), Me); });
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT feel A: %d pickups to try"), Cands.Num());
		++ShotStep;
	}
	else if (ShotStep == 1)
	{
		if (!Cur.IsValid() && !CurName.IsEmpty())
		{
			// gone: taken, flown and destroyed
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT feel A: %s walked over, taken and gone after its flight"), *CurName);
			CurName.Reset();
			++ShotStep;
		}
		else if (!Cur.IsValid())
		{
			while (Tried < Cands.Num() && !Cands[Tried].IsValid()) ++Tried;
			if (Tried >= Cands.Num() || Tried >= 10)
			{
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT feel A: no pickup could be taken (%d tried)"), Tried);
				++ShotStep;
			}
			else
			{
				Cur = Cands[Tried++];
				CurName = Cur->GetName();
				Home = Cur->GetActorLocation();
				const FVector Fl = FVector(GetActorForwardVector().X, GetActorForwardVector().Y, 0.0).GetSafeNormal();
				Cur->SetActorLocation(FeetNow() + Fl * 30.0);
				CurAt = ShotTime;
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT feel A: %s put at the feet (from %s)"), *CurName, *Home.ToString());
			}
		}
		else if (ShotTime - CurAt > 1.2f)
		{
			const FVector B = Cur->GetActorLocation();
			const FVector Mid = B + FVector(0.0, 0.0, FMath::Max(1.0, Cur->GetActorScale3D().Z) * 0.5);
			if (L->FindPickupNear(Mid + FVector(0.0, 0.0, 40.0), Mid, 5.f) == Cur.Get())
			{
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT feel A: %s not taken (nothing of it fitted): put back"), *CurName);
				Cur->SetActorLocation(Home);
				Cur = nullptr;
				CurName.Reset();
			}
			else if (ShotTime - CurAt > 3.f)
			{
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT feel A: %s still there 3 s on, not a pickup: FAILED"), *CurName);
				Cur = nullptr;
				CurName.Reset();
				++ShotStep;
			}
		}
		return;
	}

	// ---- B: down a flight of stairs ----
	static FVector StairFrom = FVector::ZeroVector, StairDir = FVector::ForwardVector;
	static double StairLen = 0.0, Walked = 0.0, MaxCamStep = 0.0, MaxFeetStep = 0.0, MaxEase = 0.0;
	static FVector PrevFeet = FVector::ZeroVector, PrevCam = FVector::ZeroVector;
	static int32 StairDowns = 0, Frames = 0, Snaps = 0;
	static float WalkTime = 0.f;
	if (ShotStep == 2)
	{
		FCollisionObjectQueryParams Obj;
		Obj.AddObjectTypesToQuery(ECC_WorldStatic);
		Obj.AddObjectTypesToQuery(ECC_WorldDynamic);
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsFeelTest), true, this);
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies()) if (Wk.IsValid()) Q.AddIgnoredActor(Wk.Get());     // (seeds stand in them)
		// from a spot, along a heading: the floor sampled every 8 cm; a step down (10 cm up to the body's step height)
		// counted, a rise, a wall at the step's height or a drop past it ends the run
		const double Radius = GetCapsuleComponent()->GetScaledCapsuleRadius() - 2.0;
		auto Scan = [&](const FVector& Feet, const FVector& Dir, double& OutLen) -> int32
		{
			FVector Prev = Feet;
			int32 Downs = 0;
			OutLen = 0.0;
			for (double D = 8.0; D <= 900.0; D += 8.0)
			{
				const FVector P(Feet.X + Dir.X * D, Feet.Y + Dir.Y * D, Prev.Z);
				FHitResult Hit;
				const FVector Up(0.0, 0.0, StepHeight + 10.0);
				const FVector Waist(0.0, 0.0, Radius + 50.0);
				if (World->SweepTestByChannel(Prev + Waist, P + Waist, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeSphere(Radius), Q)) break;
				if (!World->LineTraceSingleByObjectType(Hit, P + Up, P - Up, Obj, Q)) break;
				const double Dz = Hit.ImpactPoint.Z - Prev.Z;
				if (Dz > 2.0) break;
				if (Dz < -10.0) { ++Downs; OutLen = D; }
				Prev = FVector(P.X, P.Y, Hit.ImpactPoint.Z);
			}
			return Downs;
		};
		TArray<FVector> Seeds;
		{
			FHitResult Hit;
			const FVector A = GetActorLocation();
			if (World->LineTraceSingleByObjectType(Hit, A, A - FVector(0.0, 0.0, 400.0), Obj, Q)) Seeds.Add(Hit.ImpactPoint);
		}
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
		{
			if (!Wk.IsValid() || Wk->bBoss || Wk->IsCritter() || Seeds.Num() > 40) continue;
			FHitResult Hit;
			const FVector A = Wk->GetActorLocation();
			if (World->LineTraceSingleByObjectType(Hit, A, A - FVector(0.0, 0.0, 400.0), Obj, Q)) Seeds.Add(Hit.ImpactPoint);
		}
		int32 Best = 0;
		for (const FVector& S : Seeds)
		{
			for (int32 k = 0; k < 24; ++k)
			{
				const FVector Dir = FRotator(0.0, k * 15.0, 0.0).Vector();
				double Len = 0.0;
				const int32 N = Scan(S, Dir, Len);
				if (N > Best) { Best = N; StairFrom = S; StairDir = Dir; StairLen = Len; }
			}
		}
		StairDowns = Best;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT feel B: %d seeds; the best run down has %d steps over %.0f cm from %s heading %.0f"),
			Seeds.Num(), Best, StairLen, *StairFrom.ToString(), StairDir.Rotation().Yaw);
		if (Best > 0)
		{
			// (the bad guys along the way stand aside: no body in the walk)
			for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
			{
				if (Wk.IsValid() && FVector::Dist(Wk->GetActorLocation(), StairFrom) < StairLen + 300.0) Wk->SetActorEnableCollision(false);
			}
			TeleportTo(StairFrom + FVector(0.0, 0.0, Half + 3.0), StairDir.Rotation(), false, true);
			if (Controller) Controller->SetControlRotation(StairDir.Rotation());
			GetCharacterMovement()->StopMovementImmediately();
		}
		WalkTime = 0.f;
		Walked = MaxCamStep = MaxFeetStep = MaxEase = 0.0;
		Frames = Snaps = 0;
		ShotStep = Best > 0 ? 3 : 4;
		return;
	}
	if (ShotStep == 3)
	{
		WalkTime += Dt;
		const FVector Feet = FeetNow();
		const FVector Cam = Camera->GetComponentLocation();
		if (WalkTime > 0.6f)          // (settled after the teleport)
		{
			if (Frames > 0)
			{
				const double DFeet = Feet.Z - PrevFeet.Z, DCam = Cam.Z - PrevCam.Z;
				Walked += FVector2D(Feet.X - PrevFeet.X, Feet.Y - PrevFeet.Y).Size();
				MaxFeetStep = FMath::Max(MaxFeetStep, FMath::Abs(DFeet));
				MaxCamStep = FMath::Max(MaxCamStep, FMath::Abs(DCam));
				MaxEase = FMath::Max(MaxEase, double(FMath::Abs(StepEase)));
				if (FMath::Abs(DFeet) > 5.0) ++Snaps;
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT feel B frame %d: dt %.3f walked %.0f feet %.1f (%+.1f) eye %.1f (%+.1f) eye over feet %.1f ease %.1f"),
					Frames, Dt, Walked, Feet.Z, DFeet, Cam.Z, DCam, Cam.Z - Feet.Z, StepEase);
			}
			++Frames;
			if (Controller) Controller->SetControlRotation(StairDir.Rotation());
			AddMovementInput(StairDir, 1.f);
		}
		PrevFeet = Feet;
		PrevCam = Cam;
		if (Walked > StairLen + 120.0 || WalkTime > 10.f)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT feel B: walked %.0f cm in %d frames down %d steps: feet snapped %d times, most in a frame feet %.1f cm, eye %.1f cm; most eased %.1f cm"),
				Walked, Frames, StairDowns, Snaps, MaxFeetStep, MaxCamStep, MaxEase);
			ShotStep = 4;
		}
		return;
	}

	// ---- C: a man held at the chest ----
	static TWeakObjectPtr<AOutlawsEnemy> Held;
	static float HoldTime = 0.f;
	static int32 LastDir = -1, DirChanges = 0, HoldFrames = 0, LastOldBin = 0, OldChanges = 0;
	static float LastYaw = 0.f, MaxYawStep = 0.f;
	if (ShotStep == 4)
	{
		AOutlawsEnemy* Best = nullptr;
		double BestD = TNumericLimits<double>::Max();
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
		{
			AOutlawsEnemy* E = Wk.Get();
			if (!E || E->IsDead() || E->bBoss || E->IsCritter() || !E->GetSprite()) continue;
			const double D = FVector::Dist(E->GetActorLocation(), GetActorLocation());
			if (D < BestD) { BestD = D; Best = E; }
		}
		if (!Best)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT feel C: no man to hold"));
			ShotStep = 6;
			return;
		}
		// next to him, the hand held out at the chest
		const FVector At = Best->GetActorLocation();
		TeleportTo(At - FVector(120.0, 0.0, 0.0), FRotator::ZeroRotator, false, true);
		if (Controller) Controller->SetControlRotation(FRotator::ZeroRotator);
		Held = Best;
		HoldTime = 0.f;
		LastDir = -1;
		DirChanges = HoldFrames = OldChanges = 0;
		MaxYawStep = 0.f;
		++ShotStep;
		return;
	}
	if (ShotStep == 5)
	{
		HoldTime += Dt;
		// the right hand 12 cm in front of the chest, swaying a centimetre and a half as a real one does
		const FVector Fl = FVector(Camera->GetForwardVector().X, Camera->GetForwardVector().Y, 0.0).GetSafeNormal();
		const FVector Side = FVector::CrossProduct(FVector::UpVector, Fl);
		const double Sway = 1.5 * FMath::Sin(HoldTime * 9.0), Bob = 1.0 * FMath::Sin(HoldTime * 6.3 + 1.0);
		const FVector Fist = ChestPoint() + Fl * 12.0 + Side * Sway + FVector(0.0, 0.0, Bob);
		GripOf(1)->SetWorldLocationAndRotation(Fist, Fl.Rotation());
		FrameOf(1)->SetWorldLocationAndRotation(Fist, Fl.Rotation());
		if (HoldTime > 0.3f && HoldTime - Dt <= 0.3f)
		{
			const bool bTaken = Held.IsValid() && TryGrabEnemy(1, Held.Get());
			TestAxes.Add(TEXT("OLVR_RightGrip"), 1.f);
			bGripArmed[1] = false;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT feel C: %s taken in the right hand: %d"), Held.IsValid() ? *Held->GetName() : TEXT("-"), bTaken ? 1 : 0);
		}
		AOutlawsSpriteActor* Spr = Held.IsValid() ? Held->GetSprite() : nullptr;
		if (Spr && HoldTime > 0.8f && HoldTime <= 2.3f && GrabbedEnemy[1].Get() == Held.Get())
		{
			const float Yaw = float(Spr->GetActorRotation().Yaw);
			const FVector To = Camera->GetComponentLocation() - Spr->GetActorLocation();
			// what the old rule picked: the angle from him to the eye, in the 32 views' 11.25 degree bins
			const int32 OldBin = FMath::RoundToInt(FMath::RadiansToDegrees(FMath::Atan2(To.Y, To.X)) / 11.25);
			if (HoldFrames > 0)
			{
				if (OldBin != LastOldBin) ++OldChanges;
				if (Spr->CurrentDirection != LastDir) ++DirChanges;
				MaxYawStep = FMath::Max(MaxYawStep, FMath::Abs(FMath::FindDeltaAngleDegrees(LastYaw, Yaw)));
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT feel C frame %d: fist %.1f cm from the chest, eye %.1f cm across from him; view dir %d cell %d quad yaw %.1f"),
				HoldFrames, FVector::Dist(GripOf(1)->GetComponentLocation(), ChestPoint()), FVector2D(To.X, To.Y).Size(), Spr->CurrentDirection, Spr->CurrentCell, Yaw);
			LastDir = Spr->CurrentDirection;
			LastOldBin = OldBin;
			LastYaw = Yaw;
			++HoldFrames;
		}
		if (HoldTime > 2.4f)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT feel C: held %d frames at the chest: view direction changed %d times (the old rule's angle to the eye crossed a view bin %d times), the quad turned at most %.1f deg in a frame"),
				HoldFrames, DirChanges, OldChanges, MaxYawStep);
			LetGoEnemy(1, false);
			TestAxes.Remove(TEXT("OLVR_RightGrip"));
			++ShotStep;
		}
		return;
	}
	if (ShotStep == 6 && ShotTime > 1.f)
	{
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

OL_SHOT_MATCH(OLFeelTest) { return OLFlags::Param(TEXT("OLFeelTest")); }
OL_SHOT_TEST(OLFeelTest, 240)
{
	FeelTestTick(Dt);                // (OutlawsPlayerFeelTest.cpp)
}
#endif // WITH_OUTLAWS_TESTS
