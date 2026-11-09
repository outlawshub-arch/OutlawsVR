// -OLShots -OLDualLaserProbe (BUG-DUALLASER, 9 Nov; the user: a small pop-up over the menu, the laser already out, brought a
// SECOND red laser out of the same hand at another angle).  The title menu opens; then, each over it, the updater's bubble and
// the network's "is playing" bubble, and each hand pointed at the bubble's middle.  Every visible laser in the world is counted
// (the engine cube a few millimetres thick), each put to the hand it starts nearest.  A line per scene and hand:
//   "OLSHOT duallaser <scene> <hand>: <n> beam(s) [<owner>/<component> <len> cm ...], <angle> deg, <cm> cm apart PASS|FAIL"
// (PASS: the pointer hand one beam, ending on the bubble; the other hand none), then "OLSHOT duallaser RESULT <n> PASS, <m> FAIL".  With -OLDualLaserShot it shoots
// Saved\OLShots\ol_duallaser_<scene>.png too.

#include "ShotTestCommon.h"
#include "OutlawsBeacon.h"
#include "OutlawsCoop.h"
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
		struct FBeam { FString Who; FVector From, Dir; double Len; };
		TArray<FBeam> Beams[2];
		for (TObjectIterator<UStaticMeshComponent> It; It; ++It)
		{
			UStaticMeshComponent* C = *It;
			// (a beam: the engine cube drawn a few millimetres thick; each owner has its own instance of the laser's colour)
			if (!IsValid(C) || C->GetWorld() != GetWorld() || !C->IsRegistered() || !C->IsVisible() || !C->GetStaticMesh()
				|| C->GetStaticMesh()->GetName() != TEXT("Cube")) continue;
			const FVector Sc = C->GetComponentScale();
			if (Sc.Y > 0.006 || Sc.Z > 0.006 || Sc.X < 0.05) continue;
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
			// the pointer hand: one beam, stopping on the bubble (not out past it to the menu); the other hand: none, as the
			// title menu points with one finger
			const bool bPointer = H == MenuPointerHand();
			const double ToBub = Bub.IsValid() ? FVector::Dist(AimOf(H)->GetComponentLocation(), Bub->GetActorLocation()) : 0.0;
			const bool bOk = bPointer ? (Beams[H].Num() == 1 && Beams[H][0].Len < ToBub + 10.0) : Beams[H].Num() == 0;
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

// -OLShots -OLBubbleOrderProbe (BUG-DUALLASER, the user 9 Nov: the pop-up "a little further and a little higher", at about the
// kill feed's height, and still in front of every other thing on screen).  In a match: the kill feed's lines, the HUD, then the
// scoreboard too, with the network's bubble up.  Every visible thing that draws as screen furniture (the foreground group, or a
// see-through thing sorted 5 or more) is listed with its group and sort; one drawn after the bubble is a FAIL:
//   "OLSHOT bubbleorder <scene>: bubble <group>/<sort> at <cm> cm, <deg> deg over the view line; after it: <list> PASS|FAIL"
// then "OLSHOT bubbleorder RESULT ...".  Shots: Saved\OLShots\ol_bubbleorder_<scene>.png.
OL_SHOT_MATCH(OLBubbleOrderProbe) { return OLFlags::Param(TEXT("OLBubbleOrderProbe")); }

OL_SHOT_TEST(OLBubbleOrderProbe, 2578)
{
	static int32 Step = 0, Pass = 0, Fail = 0;
	static float Wait = 0.f, InLevel = 0.f;
	static TWeakObjectPtr<AOutlawsJoinBubble> Bub;
	if (Step < 0 || !L) return;
	if (L->IsFrontEnd() || bMenuOpen) { InLevel = 0.f; return; }
	InLevel += Dt;
	Wait += Dt;
	if (Step == 0)
	{
		if (InLevel < 6.f) return;
		FActorSpawnParameters SP;
		SP.ObjectFlags |= RF_Transient;
		Bub = GetWorld()->SpawnActor<AOutlawsJoinBubble>(AOutlawsJoinBubble::StaticClass(), FTransform::Identity, SP);
		if (Bub.IsValid()) Bub->Show(EOutlawsBubbleKind::Offer, TEXT("Someone is playing Hideout nearby"), TEXT("Join"), TEXT("X"));
		Step = 1; Wait = 0.f;
	}
	// the feed kept full
	while (KillLines.Num() < 3) KillLines.Add({ KillLines.Num() == 0 ? TEXT("James killed Doc") : KillLines.Num() == 1 ? TEXT("Doc killed Bob") : TEXT("You killed James"), 30.f, KillLines.Num() == 2 });
	auto Judge = [&](const TCHAR* Scene)
	{
		UPrimitiveComponent* Plane = Bub.IsValid() ? Bub->FindComponentByClass<UStaticMeshComponent>() : nullptr;
		for (UActorComponent* AC : Bub.IsValid() ? Bub->GetComponents().Array() : TArray<UActorComponent*>())
			if (UStaticMeshComponent* M = Cast<UStaticMeshComponent>(AC)) if (M->GetName() == TEXT("Plane")) Plane = M;
		if (!Plane) { UE_LOG(LogOutlaws, Display, TEXT("OLSHOT bubbleorder %s: no bubble FAIL"), Scene); ++Fail; return; }
		const int32 Bg = Plane->DepthPriorityGroup, Bs = Plane->TranslucencySortPriority;
		const FVector Eye = Camera->GetComponentLocation(), C = Plane->GetComponentLocation();
		const double Cm = FVector::Dist(Eye, C);
		const double Deg = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp((C.Z - Eye.Z) / FMath::Max(Cm, 1.0), -1.0, 1.0)));
		FString After, Seen;
		int32 NSeen = 0;
		for (TObjectIterator<UPrimitiveComponent> It; It; ++It)
		{
			UPrimitiveComponent* Pc = *It;
			if (!IsValid(Pc) || Pc == Plane || Pc->GetWorld() != GetWorld() || !Pc->IsRegistered() || !Pc->IsVisible()) continue;
			if (Pc->GetOwner() && Pc->GetOwner()->IsHidden()) continue;
			if (Pc->GetOwner() == Bub.Get()) continue;
			const int32 G = Pc->DepthPriorityGroup, S = Pc->TranslucencySortPriority;
			if (G != SDPG_Foreground && S < 5) continue;
			++NSeen;
			const FString Who = FString::Printf(TEXT("%s/%s %d/%d"), Pc->GetOwner() ? *Pc->GetOwner()->GetClass()->GetName() : TEXT("none"), *Pc->GetName(), G, S);
			if (Seen.Len() < 900) Seen += (Seen.IsEmpty() ? TEXT("") : TEXT(", ")) + Who;
			if (G > Bg || (G == Bg && S >= Bs)) After += (After.IsEmpty() ? TEXT("") : TEXT(", ")) + Who;
		}
		const bool bOk = After.IsEmpty();
		(bOk ? Pass : Fail)++;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT bubbleorder %s: bubble %d/%d at %.0f cm, %.1f deg over the view line; %d UI things; after it: [%s] %s"), Scene, Bg, Bs, Cm, Deg, NSeen,
			*After, bOk ? TEXT("PASS") : TEXT("FAIL"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT bubbleorder %s seen: %s"), Scene, *Seen);
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(TEXT("ol_bubbleorder_")) + Scene + TEXT(".png"), false, false);
	};
	if (Step == 1 && Wait > 2.f) { Judge(TEXT("feed")); Step = 2; Wait = 0.f; }
	else if (Step == 2 && Wait > 1.f) { UOutlawsCoop::ToggleScoreboard(this, TEXT("bubble order probe")); Step = 3; Wait = 0.f; }
	else if (Step == 3 && Wait > 2.f) { Judge(TEXT("scoreboard")); Step = 4; Wait = 0.f; }
	else if (Step == 4 && Wait > 1.f)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT bubbleorder RESULT %d PASS, %d FAIL"), Pass, Fail);
		Step = -1;
		FGenericPlatformMisc::RequestExit(false);
	}
}

#endif // WITH_OUTLAWS_TESTS
