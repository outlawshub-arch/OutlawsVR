// -OLShots -OLDualLaserProbe (BUG-DUALLASER, 9 Nov; the user: a small pop-up over the menu, the laser already out, brought a
// SECOND red laser out of the same hand at another angle).  The title menu opens; then, each over it, the updater's bubble and
// the network's "is playing" bubble, and each hand pointed at the bubble's middle.  Every visible laser in the world is counted
// (any mesh in the laser's material), each put to the hand it starts nearest.  A line per scene and hand:
//   "OLSHOT duallaser <scene> <hand>: <n> beam(s) [<owner>/<component> <len> cm ...], <angle> deg, <cm> cm apart PASS|FAIL"
// (PASS: at most one beam from the hand), then "OLSHOT duallaser RESULT <n> PASS, <m> FAIL".  With -OLDualLaserShot it shoots
// Saved\OLShots\ol_duallaser_<scene>.png too.

#include "ShotTestCommon.h"
#include "OutlawsBeacon.h"
#include "UObject/UObjectIterator.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLDualLaserProbe) { return OLFlags::Param(TEXT("OLDualLaserProbe")); }

OL_SHOT_TEST(OLDualLaserProbe, 2577)
{
	static int32 Scene = 0, Step = 0, Pass = 0, Fail = 0;
	static float Wait = 0.f;
	static TWeakObjectPtr<AOutlawsJoinBubble> Bub;
	if (Scene < 0) return;
	Wait += Dt;
	const TCHAR* Names[] = { TEXT("update"), TEXT("offer") };
	const int32 NumScenes = UE_ARRAY_COUNT(Names);
	if (Step == 0)
	{
		if (Wait < 4.f || !bMenuOpen) return;
		bVR = true;                      // (the headset's menu and bubble paths, this run only)
		FActorSpawnParameters SP;
		SP.ObjectFlags |= RF_Transient;
		if (!Bub.IsValid()) Bub = GetWorld()->SpawnActor<AOutlawsJoinBubble>(AOutlawsJoinBubble::StaticClass(), FTransform::Identity, SP);
		if (Scene == 0) Bub->Show(EOutlawsBubbleKind::Update, TEXT("Outlaws VR v0.97 is out. Update now?"), TEXT("Update"), TEXT("Not now"), TEXT("Skip"));
		else Bub->Show(EOutlawsBubbleKind::Offer, TEXT("Someone is playing Hideout nearby"), TEXT("Join"), TEXT("X"));
		Step = 1; Wait = 0.f;
		return;
	}
	// both hands pointed at the bubble's middle, from a little below and to each side of the eyes
	const FVector Eye = Camera->GetComponentLocation();
	const FVector Target = Bub.IsValid() ? Bub->GetActorLocation() : Eye + Camera->GetForwardVector() * 60.0;
	const FVector Rt = Camera->GetRightVector();
	for (int32 H = 0; H < 2; ++H)
	{
		const FVector At = Eye + (Target - Eye) * 0.45 + Rt * (H == 0 ? -12.0 : 12.0) - FVector::UpVector * 22.0;
		const FQuat Q = FRotationMatrix::MakeFromX((Target - At).GetSafeNormal()).ToQuat();
		GripOf(H)->SetWorldLocationAndRotation(At, Q);
		AimOf(H)->SetWorldLocationAndRotation(At, Q);
	}
	if (Step == 1)
	{
		if (Wait < 1.5f) return;
		if (OLFlags::Param(TEXT("OLDualLaserShot")))
			FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(TEXT("ol_duallaser_")) + Names[Scene] + TEXT(".png"), false, false);
		UMaterialInterface* LM = LaserMaterial();
		struct FBeam { FString Who; FVector From, Dir; double Len; };
		TArray<FBeam> Beams[2];
		for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
		{
			UStaticMeshComponent* C = *It;
			if (!IsValid(C) || C->GetWorld() != GetWorld() || !C->IsRegistered() || !C->IsVisible() || !LM || C->GetMaterial(0) != LM) continue;
			if (C->GetOwner() && C->GetOwner()->IsHidden()) continue;
			const FVector Dir = C->GetForwardVector();
			const double Len = C->GetComponentScale().X * 100.0;
			const FVector From = C->GetComponentLocation() - Dir * (Len * 0.5);
			double Best = 1e9; int32 Hb = 0;
			for (int32 H = 0; H < 2; ++H)
			{
				const double Dd = FVector::Dist(From, AimOf(H)->GetComponentLocation());
				if (Dd < Best) { Best = Dd; Hb = H; }
			}
			Beams[Hb].Add({ (C->GetOwner() ? C->GetOwner()->GetClass()->GetName() : FString(TEXT("none"))) + TEXT("/") + C->GetName(), From, Dir, Len });
		}
		for (int32 H = 0; H < 2; ++H)
		{
			FString List;
			for (const FBeam& B : Beams[H]) List += FString::Printf(TEXT("%s%s %.0f cm"), List.IsEmpty() ? TEXT("") : TEXT(", "), *B.Who, B.Len);
			double Ang = 0.0, Apart = 0.0;
			if (Beams[H].Num() >= 2)
			{
				Ang = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(Beams[H][0].Dir, Beams[H][1].Dir), -1.0, 1.0)));
				Apart = FVector::Dist(Beams[H][0].From, Beams[H][1].From);
			}
			const bool bOk = Beams[H].Num() <= 1;
			(bOk ? Pass : Fail)++;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT duallaser %s %s: %d beam(s) [%s], %.1f deg, %.1f cm apart %s (pointer hand %s)"), Names[Scene],
				H == 0 ? TEXT("left") : TEXT("right"), Beams[H].Num(), *List, Ang, Apart, bOk ? TEXT("PASS") : TEXT("FAIL"),
				MenuPointerHand() == 0 ? TEXT("left") : TEXT("right"));
		}
		Step = 2; Wait = 0.f;
		return;
	}
	if (Wait < 1.f) return;
	if (Bub.IsValid()) Bub->Show(EOutlawsBubbleKind::None, FString(), FString(), FString());
	if (++Scene < NumScenes) { Step = 0; Wait = 3.5f; return; }
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT duallaser RESULT %d PASS, %d FAIL"), Pass, Fail);
	Scene = -1;
	FGenericPlatformMisc::RequestExit(false);
}

#endif // WITH_OUTLAWS_TESTS
