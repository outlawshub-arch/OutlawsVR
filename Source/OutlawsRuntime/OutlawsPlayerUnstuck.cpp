// Stuck rescue (BUG-STUCKGAP, user 2026-10-05: the first internet co-op session on the hideout left the tester in a
// ceiling-to-floor gap after a climb, then the host in a like space where he died).  Single player and co-op alike:
// - The head: the eyes' point outside every sector (outside every sector's walls, or under its floor, or over its ceiling
//   at that spot, slopes included) for HeadGrace: back to safe ground.  Only outside every sector counts, so a lean or a
//   climb through an opening into the next sector never sets it off, and a quick lean through a wall is forgiven.
// - The body: the capsule inside the level's walls, or the feet outside every sector, for BodyGrace (a climb's own moves
//   are left alone).
// - The pause menu's Unstuck row, any time.
// Safe ground is the last spot the feet stood on a walkable floor with the head inside a sector and room for a standing
// body, checked again when it is wanted (a lift may have gone); with none, the middle of the last sector the head was in,
// at its floor and clear of walls and objects; else the level's start.  Each rescue goes through a blink of black and is
// logged with the level, both places and what set it off (to find the bad spots).
// BUG-UNSTUCKLOOP (the user's Quest log, 10 Oct, townex): a room-scale step had the head 60 cm off the body through a
// wall; the rescue put the body back on its own spot with the head still out, 22 times.  Now a rescue pulls the play space
// back under the body (the head over the feet), never puts the body on its own spot or one tried already, waits BackOff s
// before the next unless the body moves, and from the third try at one spot goes further back (else the level's start).
#include "OutlawsPlayer.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsMenuDoc.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "OutlawsShotTests.h"

namespace
{
	constexpr double HeadGrace = 0.75;           // s the head may stay outside every sector (a lean through a wall: less)
	constexpr double BodyGrace = 3.0;            // s the body may stay in a wall or between sectors
	constexpr double LookEvery = 0.1;            // s between looks
	constexpr double FadeOut = 0.2, FadeIn = 0.35;
	constexpr int32 SpotsKept = 6;
	constexpr double SpotApart = 50.0;           // cm between kept spots
	constexpr float StandHalf = 87.f;            // a standing body (OutlawsPlayer.cpp's StandHalfHeight)
	// (BUG-UNSTUCKLOOP)
	constexpr double BackOff = 3.0;              // s the checks wait after a rescue while the body stays on its spot
	constexpr double StillThere = 40.0;          // cm off the last rescue's spot that still counts as on it
	constexpr double RepeatWindow = 10.0;        // s within which a rescue at the last one's spot is a further try
	constexpr double NotHere = 25.0;             // cm: a target nearer the body than this is its own spot

	/** Whether a capsule there overlaps anything blocking but a body (the level's walls, its objects) that Me's own body
	 *  collides with: a wall only some kinds of body keep to (an enemy fence: BUG-UNSTUCKNARROW, the hideout's fence top
	 *  walked along with its adjoins' enemy blockers through the capsule) is no wall to be stuck in.  Nor is a room the
	 *  body passes through (Passed: BUG-MILLCLIMB, the mill's hall over the yard's ground, a zone gate's far side). */
	bool Blocked(const UWorld* W, const FVector& At, float Radius, float Half, const AActor* Me, const AActor* OnlyOwner, FString* OutWhat = nullptr, const AOutlawsLevelActor* Passed = nullptr)
	{
		TArray<FOverlapResult> Hits;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsUnstuck), false, Me);
		if (Passed) Passed->IgnorePassedOverlaps(Q);
		const FCollisionShape Shape = FCollisionShape::MakeCapsule(Radius, Half);
		W->OverlapMultiByChannel(Hits, At, FQuat::Identity, ECC_Pawn, Shape, Q);
		const UPrimitiveComponent* Mine = Me ? Cast<UPrimitiveComponent>(Me->GetRootComponent()) : nullptr;
		for (const FOverlapResult& H : Hits)
		{
			const AActor* A = H.GetActor();
			if (!H.bBlockingHit || !A || A->IsA<APawn>()) continue;
			if (OnlyOwner && A != OnlyOwner) continue;
			const UPrimitiveComponent* C = H.GetComponent();
			if (Mine && C && (Mine->GetCollisionResponseToChannel(C->GetCollisionObjectType()) != ECR_Block || C->GetCollisionResponseToChannel(Mine->GetCollisionObjectType()) != ECR_Block)) continue;
			if (OutWhat)
			{
				FMTDResult Mtd;
				UPrimitiveComponent* Hit = H.GetComponent();
				const bool bMtd = Hit && Hit->ComputePenetration(Mtd, Shape, At, FQuat::Identity);
				*OutWhat = FString::Printf(TEXT("%s, %.1f cm deep along %s"), Hit ? *Hit->GetName() : TEXT("?"), bMtd ? Mtd.Distance : -1.f, bMtd ? *Mtd.Direction.ToCompactString() : TEXT("?"));
			}
			return true;
		}
		return false;
	}

	void Fade(AOutlawsPlayer* P, float From, float To, float Time, bool bHold)
	{
		const APlayerController* PC = Cast<APlayerController>(P->GetController());
		if (PC && PC->PlayerCameraManager) PC->PlayerCameraManager->StartCameraFade(From, To, Time, FLinearColor::Black, false, bHold);
	}
}

bool AOutlawsPlayer::SafeFeetAt(const FVector& Feet, FVector& OutFeet) const
{
	const AOutlawsLevelActor* L = Level.Get();
	const UWorld* W = GetWorld();
	if (!L || !W) return false;
	// a floor under the feet (the sector's, a box's, a lift's where it is now)
	FHitResult G;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsUnstuckFloor), false, this);
	L->IgnorePassedOverlaps(Q);             // (BUG-MILLCLIMB: not a floor the body passes through)
	if (!W->LineTraceSingleByChannel(G, Feet + FVector(0.0, 0.0, 25.0), Feet - FVector(0.0, 0.0, 45.0), ECC_Visibility, Q) || G.bStartPenetrating || G.ImpactNormal.Z < 0.7)
	{
		return false;
	}
	const FVector On = G.ImpactPoint;
	// inside a sector there, the head's height too, and a standing body's room clear of walls and objects
	if (L->SectorHoldingPoint(On + FVector(0.0, 0.0, 5.0), 15.0) < 0 || L->SectorHoldingPoint(On + FVector(0.0, 0.0, 2.0 * StandHalf - 10.0), 2.0) < 0) return false;
	const float R = GetCapsuleComponent()->GetScaledCapsuleRadius();
	if (Blocked(W, On + FVector(0.0, 0.0, StandHalf + 3.0), R, StandHalf, this, nullptr, nullptr, L)) return false;
	OutFeet = On;
	return true;
}

bool AOutlawsPlayer::LastSafeFeet(FVector& OutFeet) const
{
	for (int32 i = SafeSpots.Num() - 1; i >= 0; --i)
	{
		if (SafeFeetAt(SafeSpots[i].Feet, OutFeet)) return true;
	}
	return false;
}

void AOutlawsPlayer::RequestUnstuck(const TCHAR* Trigger)
{
	const AOutlawsLevelActor* L = Level.Get();
	UWorld* W = GetWorld();
	if (!L || !W || L->IsFrontEnd() || bDeadView || L->IsPlayerDead() || UnstuckWarpAt >= 0.0) return;
	if (bMenuOpen) CloseMenu();
	UnstuckTrigger = Trigger;
	UnstuckAskedAt = W->GetTimeSeconds();
	UnstuckWarpAt = UnstuckAskedAt + FadeOut;
	Fade(this, 0.f, 1.f, float(FadeOut), true);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT unstuck asked (%s) on %s at %s: black, then the warp"), Trigger, *L->LevelName, *GetActorLocation().ToCompactString());
}

bool AOutlawsPlayer::UnstuckHeldBack(double Now, const TCHAR* Why)
{
	// (BUG-UNSTUCKLOOP) after a rescue the checks wait BackOff s while the body stays on the spot it was put on
	if (UnstuckLastAt < 0.0 || Now - UnstuckLastAt >= BackOff || FVector::Dist(GetActorLocation(), UnstuckLastTo) > StillThere) return false;
	if (UnstuckHeldLogAt < UnstuckLastAt)
	{
		UnstuckHeldLogAt = Now;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT unstuck held back (%s): the last rescue %.1f s ago and the body not moved off its spot; again after %.0f s or a move"), Why, Now - UnstuckLastAt, BackOff);
	}
	return true;
}

void AOutlawsPlayer::UnstuckWarp()
{
	AOutlawsLevelActor* L = Level.Get();
	UWorld* W = GetWorld();
	UnstuckWarpAt = -1.0;
	if (!L || !W) return;
	const double Now = W->GetTimeSeconds();
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), R = GetCapsuleComponent()->GetScaledCapsuleRadius();
	const FVector From = GetActorLocation();
	const FVector Head = Camera ? Camera->GetComponentLocation() + UnstuckTestHeadOffset : From;
	// (BUG-UNSTUCKLOOP) the head's room-scale step off the body: the warp moves the body, so the play space is pulled back
	// under it too (else the head lands as far off the new spot as it stood off the old, through the same kind of wall)
	FVector HeadOff = Camera ? Camera->GetComponentLocation() - From : FVector::ZeroVector;
	HeadOff.Z = 0.0;
	// one rescue on another at the same spot (the player not moved off the last one's): the spots tried in the run are not
	// tried again, and from the third on the body goes further back (the oldest safe spot clear of them, else the start)
	const bool bRepeat = UnstuckLastAt >= 0.0 && Now - UnstuckLastAt < RepeatWindow && FVector::Dist(From, UnstuckLastTo) <= StillThere;
	UnstuckStreak = bRepeat ? UnstuckStreak + 1 : 0;
	if (!bRepeat) UnstuckTried.Reset();
	const bool bFurther = UnstuckStreak >= 2;
	TArray<FString> Passed;
	// a target: the body there with the head over it both inside a sector, a standing body's room (SafeFeetAt), not the
	// body's own spot, not a spot this run already tried (from the third try on: not within a metre of one)
	auto Fits = [&](const FVector& Body, const FString& What) -> bool
	{
		if (FVector::Dist(Body, From) < NotHere)
		{
			Passed.Add(What + TEXT(": the body's own spot"));
			return false;
		}
		for (const FVector& T : UnstuckTried)
		{
			if (FVector::Dist(Body, T) < (bFurther ? 100.0 : NotHere))
			{
				Passed.Add(What + TEXT(": tried already this run"));
				return false;
			}
		}
		return true;
	};
	FVector To;
	FString Where;
	FVector Feet;
	bool bFound = false;
	for (int32 n = 0; n < SafeSpots.Num() && !bFound; ++n)
	{
		const int32 i = bFurther ? n : SafeSpots.Num() - 1 - n;
		const FString What = FString::Printf(TEXT("safe spot %d"), i);
		if (!SafeFeetAt(SafeSpots[i].Feet, Feet))
		{
			Passed.Add(What + TEXT(": no longer safe"));
			continue;
		}
		if (!Fits(Feet + FVector(0.0, 0.0, Half + 2.0), What)) continue;
		To = Feet + FVector(0.0, 0.0, Half + 2.0);
		bFound = true;
		Where = FString::Printf(TEXT("%s safe spot (%.1f s old, sector %d)"), bFurther ? TEXT("the oldest clear") : TEXT("the last"), Now - SafeSpots[i].At, SafeSpots[i].Sector);
	}
	// then (a third try on: the level's start first) the middle of the last sector the head was in, then the start
	auto TryMiddle = [&]()
	{
		FVector Mid;
		if (bFound || !L->SectorMiddleSpot(LastHeadSector, R, Half, this, Mid)) return;
		Mid.Z += 2.0;
		const FString What = FString::Printf(TEXT("the middle of sector %d"), LastHeadSector);
		if (!SafeFeetAt(Mid - FVector(0.0, 0.0, Half + 2.0), Feet)) Passed.Add(What + TEXT(": not safe"));
		else if (Fits(Mid, What))
		{
			To = Mid;
			bFound = true;
			Where = FString::Printf(TEXT("the middle of sector %d, the last the head was in (no safe spot fits)"), LastHeadSector);
		}
	};
	const FVector Start = L->GetPlayerStartLocation();
	if (!bFurther) TryMiddle();
	if (!bFound && Fits(Start, TEXT("the level's start")))
	{
		To = Start;
		bFound = true;
		Where = bFurther ? TEXT("the level's start (a third try at the spot, no safe spot further back)") : TEXT("the level's start (no safe spot, no sector)");
	}
	if (bFurther) TryMiddle();
	if (!bFound)
	{
		To = Start;
		Where = TEXT("the level's start (nothing else left)");
	}
	TeleportTo(To, GetActorRotation(), false, true);
	if (VROrigin && !HeadOff.IsNearlyZero(1.0)) VROrigin->AddWorldOffset(-HeadOff);
	if (UCharacterMovementComponent* M = GetCharacterMovement())
	{
		M->StopMovementImmediately();
		M->SetMovementMode(MOVE_Walking);
	}
	GrantFallGrace(Now + 3.0);
	FallSince = -1.0;
	HeadOutSince = BodyStuckSince = -1.0;
	UnstuckTestHeadOffset = FVector::ZeroVector;
	Fade(this, 1.f, 0.f, float(FadeIn), false);
	++UnstuckCount;
	UnstuckLastAt = Now;
	UnstuckLastTo = GetActorLocation();
	UnstuckTried.Add(UnstuckLastTo);
	const FVector HeadTo = Camera ? Camera->GetComponentLocation() : UnstuckLastTo;
	const FVector FeetTo = UnstuckLastTo - FVector(0.0, 0.0, Half);
	LastUnstuck = FString::Printf(TEXT("OLRT unstuck (%s): level %s, from %s (head %s) to %s, %s; try %d at the spot, the head pulled %.0f cm back over the body, now head sector %d, feet sector %d%s%s"),
		*UnstuckTrigger, *L->LevelName, *From.ToCompactString(), *Head.ToCompactString(), *UnstuckLastTo.ToCompactString(), *Where, UnstuckStreak + 1, HeadOff.Size(),
		L->SectorHoldingPoint(HeadTo, 2.0), L->SectorHoldingPoint(FeetTo + FVector(0.0, 0.0, 5.0), 15.0), Passed.Num() ? TEXT("; passed over: ") : TEXT(""), *FString::Join(Passed, TEXT(", ")));
	UE_LOG(LogOutlaws, Log, TEXT("%s"), *LastUnstuck);
}

void AOutlawsPlayer::UpdateUnstuck(float Dt)
{
	AOutlawsLevelActor* L = Level.Get();
	UWorld* W = GetWorld();
	if (!L || !W || L->IsFrontEnd()) return;
	const double Now = W->GetTimeSeconds();
	if (SafeClear != L->GetClearCount())
	{
		// a new level: nothing safe known yet
		SafeClear = L->GetClearCount();
		SafeSpots.Reset();
		LastHeadSector = -1;
		HeadOutSince = BodyStuckSince = UnstuckWarpAt = -1.0;
		UnstuckLastAt = -1.0;
		UnstuckStreak = 0;
		UnstuckTried.Reset();
	}
	if (UnstuckWarpAt >= 0.0)
	{
		if (Now >= UnstuckWarpAt) UnstuckWarp();
		return;
	}
	if (Now < NextUnstuckLook || L->IsPlayerDead()) return;
	NextUnstuckLook = Now + LookEvery;
	// (other automated tests place the body where they like: the checks only look on with their own test or the climb's)
	static const bool bOwnTest = OLFlags::Param(TEXT("OLUnstuckTest")) || OLFlags::Param(TEXT("OLUnstuckLoopTest")) || OLFlags::HasValue(TEXT("OLClimbTest"))
		|| OLFlags::HasValue(TEXT("OLNarrowWalkTest"));
	const bool bOtherTest = (ShotTime >= 0.f || SmokeTime >= 0.f) && !bOwnTest;
	const FVector Body = GetActorLocation();
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), R = GetCapsuleComponent()->GetScaledCapsuleRadius();
	const FVector FeetNow = Body - FVector(0.0, 0.0, Half);
	const FVector Head = (Camera ? Camera->GetComponentLocation() : Body) + UnstuckTestHeadOffset;
	const int32 HeadSector = L->SectorHoldingPoint(Head, 2.0);
	if (HeadSector == -2) return;                                     // (no level yet)
	if (HeadSector >= 0) LastHeadSector = HeadSector;
	// the safe spot: the feet on a walkable floor, the head inside a sector
	const UCharacterMovementComponent* M = GetCharacterMovement();
	FVector Feet;
	if (HeadSector >= 0 && M && M->IsMovingOnGround() && M->CurrentFloor.IsWalkableFloor() && !IsClimbing() && MantleLeft <= 0.f && SafeFeetAt(FeetNow, Feet))
	{
		if (SafeSpots.Num() == 0 || FVector::Dist(SafeSpots.Last().Feet, Feet) > SpotApart)
		{
			if (SafeSpots.Num() >= SpotsKept) SafeSpots.RemoveAt(0);
			SafeSpots.Add({ Feet, Now, L->SectorHoldingPoint(Feet + FVector(0.0, 0.0, 5.0), 15.0) });
		}
		else
		{
			SafeSpots.Last().Feet = Feet;
			SafeSpots.Last().At = Now;
		}
	}
	if (bOtherTest) return;
	// the head outside every sector
	if (HeadSector >= 0) HeadOutSince = -1.0;
	else if (HeadOutSince < 0.0)
	{
		HeadOutSince = Now;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT unstuck: the head is outside every sector at %s on %s (back to safe ground after %.2f s of it); the feet at %s in sector %d"), *Head.ToCompactString(),
			*L->LevelName, HeadGrace, *FeetNow.ToCompactString(), L->SectorHoldingPoint(FeetNow + FVector(0.0, 0.0, 5.0), 15.0));
	}
	else if (Now - HeadOutSince >= HeadGrace && !UnstuckHeldBack(Now, TEXT("head")))
	{
		RequestUnstuck(*FString::Printf(TEXT("head, %.2f s outside every sector"), Now - HeadOutSince));
		return;
	}
	// the body in the level's walls, or its feet between sectors (a climb's moves left alone)
	if (IsClimbing() || MantleLeft > 0.f || bSwimming)
	{
		BodyStuckSince = -1.0;
		return;
	}
	FString WallWhat;
	const bool bInWall = Blocked(W, Body, FMath::Max(1.f, R - 2.f), FMath::Max(R, Half - 5.f), this, L, &WallWhat, L);
	const bool bFeetOut = L->SectorHoldingPoint(FeetNow + FVector(0.0, 0.0, 5.0), 15.0) == -1;
	// a body that walks on over a floor is not stuck, whatever it brushes: the wall test counts only while it makes no
	// headway (under 20 cm in half a second), or is off its feet, or its head or feet are outside every sector
	if (Now - StuckProgressAt >= 0.5)
	{
		bStuckProgress = StuckProgressAt >= 0.0 && FVector::Dist(Body, StuckProgressFrom) > 20.0;
		StuckProgressFrom = Body;
		StuckProgressAt = Now;
	}
	const bool bWalkingOn = bStuckProgress && HeadSector >= 0 && !bFeetOut && M && M->IsMovingOnGround() && M->CurrentFloor.IsWalkableFloor();
	if ((!bInWall || bWalkingOn) && !bFeetOut) BodyStuckSince = -1.0;
	else if (BodyStuckSince < 0.0)
	{
		BodyStuckSince = Now;
		BodyStuckWhy = bInWall ? TEXT("in the level's walls") : TEXT("the feet between sectors");
		UE_LOG(LogOutlaws, Log, TEXT("OLRT unstuck: the body is %s at %s on %s (back to safe ground after %.0f s of it); the wall: %s; feet sector %d, head sector %d, %s, floor %s %.1f cm off, speed %.0f"),
			*BodyStuckWhy, *Body.ToCompactString(), *L->LevelName, BodyGrace, bInWall ? *WallWhat : TEXT("none"), L->SectorHoldingPoint(FeetNow + FVector(0.0, 0.0, 5.0), 15.0), HeadSector,
			M && M->IsMovingOnGround() ? TEXT("on the ground") : TEXT("off the ground"), M && M->CurrentFloor.IsWalkableFloor() ? TEXT("walkable") : TEXT("none"),
			M ? M->CurrentFloor.FloorDist : -1.f, GetVelocity().Size2D());
	}
	else if (Now - BodyStuckSince >= BodyGrace && !UnstuckHeldBack(Now, TEXT("body")))
	{
		RequestUnstuck(*FString::Printf(TEXT("body, %.1f s %s"), Now - BodyStuckSince, *BodyStuckWhy));
	}
}

#if WITH_OUTLAWS_TESTS
void AOutlawsPlayer::UnstuckShotTick()
{
	AOutlawsLevelActor* L = Level.Get();
	UWorld* W = GetWorld();
	if (!L || !W) return;
	auto Say = [this](const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT unstuck test: %s; rescues %d; last: %s"), *What, UnstuckCount, LastUnstuck.IsEmpty() ? TEXT("none") : *LastUnstuck);
	};
	if (const APlayerController* PC = Cast<APlayerController>(GetController()); PC && PC->PlayerCameraManager)
	{
		UnstuckTestMaxFade = FMath::Max(UnstuckTestMaxFade, PC->PlayerCameraManager->FadeAmount);
	}
	const FVector Head = Camera ? Camera->GetComponentLocation() : GetActorLocation();
	if (ShotStep == 0 && ShotTime > 4.f)
	{
		// the nearest wall of the level whose far side is outside every sector
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsUnstuckTest), false, this);
		double Best = 1e9;
		for (int32 k = 0; k < 16; ++k)
		{
			const FVector D = FRotator(0.0, 22.5 * k, 0.0).Vector();
			FHitResult H;
			if (!W->LineTraceSingleByChannel(H, Head, Head + D * 1500.0, ECC_Visibility, Q) || H.GetActor() != L || H.Distance < 40.0) continue;
			if (L->SectorHoldingPoint(Head + D * (H.Distance + 40.0), 2.0) != -1 || H.Distance >= Best) continue;
			Best = H.Distance;
			UnstuckTestDir = D * (H.Distance + 40.0);
		}
		FVector Safe;
		const bool bSafe = LastSafeFeet(Safe);
		Say(FString::Printf(TEXT("level %s, standing at %s, safe spot %s, head %s in sector %d; the wall %.0f cm off, the head put 40 cm past it (outside every sector)"), *L->LevelName,
			*GetActorLocation().ToCompactString(), bSafe ? *Safe.ToCompactString() : TEXT("NONE"), *Head.ToCompactString(), L->SectorHoldingPoint(Head, 2.0), Best));
		if (Best > 1e8) { FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; return; }
		UnstuckTestHeadOffset = UnstuckTestDir;
		UnstuckTestT0 = ShotTime;
		UnstuckTestCount0 = UnstuckCount;
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > UnstuckTestT0 + 0.3f)
	{
		UnstuckTestHeadOffset = FVector::ZeroVector;
		++ShotStep;
	}
	else if (ShotStep == 2 && ShotTime > UnstuckTestT0 + 2.f)
	{
		Say(FString::Printf(TEXT("A: the head through the wall for 0.3 s: %s"), UnstuckCount == UnstuckTestCount0 ? TEXT("no rescue (a lean is forgiven)") : TEXT("RESCUED (WRONG)")));
		UnstuckTestHeadOffset = UnstuckTestDir;
		UnstuckTestT0 = ShotTime;
		UnstuckTestCount0 = UnstuckCount;
		UnstuckTestMaxFade = 0.f;
		++ShotStep;
	}
	else if (ShotStep == 3 && ShotTime > UnstuckTestT0 + 1.5f)
	{
		UnstuckTestHeadOffset = FVector::ZeroVector;
		Say(FString::Printf(TEXT("B: the head through the wall for 1.5 s: %s, the screen's fade reached %.2f"), UnstuckCount > UnstuckTestCount0 ? TEXT("rescued") : TEXT("NOT RESCUED"), UnstuckTestMaxFade));
		// C: the body held fast with its feet 50 cm under the floor, the head inside the room
		FVector Safe;
		if (LastSafeFeet(Safe))
		{
			const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			TeleportTo(Safe + FVector(0.0, 0.0, Half - 50.0), GetActorRotation(), false, true);
			GetCharacterMovement()->SetMovementMode(MOVE_None);
		}
		UnstuckTestT0 = ShotTime;
		UnstuckTestCount0 = UnstuckCount;
		++ShotStep;
	}
	else if (ShotStep == 4 && ShotTime > UnstuckTestT0 + 4.f)
	{
		Say(FString::Printf(TEXT("C: the body held in the floor (the head inside): %s"), UnstuckCount > UnstuckTestCount0 ? TEXT("rescued") : TEXT("NOT RESCUED")));
		// D: the pause menu's Unstuck row (REQ-PAUSETIDY: on the Tools page)
		OpenMenu();
		ShowMenuPage(TEXT("#tools"), true);
		const int32 i = MenuPage ? MenuPage->Items.IndexOfByPredicate([](const OutlawsMenu::FItem& It) { return It.Name == TEXT("unstuck"); }) : INDEX_NONE;
		Say(FString::Printf(TEXT("D: the pause menu (%s) has the Unstuck row at %d"), MenuPage ? *MenuPage->File : TEXT("none"), i));
		UnstuckTestCount0 = UnstuckCount;
		if (i != INDEX_NONE) MenuActivate(1, i);
		UnstuckTestT0 = ShotTime;
		++ShotStep;
	}
	else if (ShotStep == 5 && ShotTime > UnstuckTestT0 + 1.5f)
	{
		Say(FString::Printf(TEXT("D: the Unstuck row: %s, the menu %s"), UnstuckCount > UnstuckTestCount0 ? TEXT("rescued") : TEXT("NOT RESCUED"), bMenuOpen ? TEXT("STILL OPEN") : TEXT("closed")));
		// E: no safe spot known: the middle of the last sector the head was in
		ForgetSafeSpots();
		UnstuckTestCount0 = UnstuckCount;
		RequestUnstuck(TEXT("test, no safe spot"));
		UnstuckTestT0 = ShotTime;
		++ShotStep;
	}
	else if (ShotStep == 6 && ShotTime > UnstuckTestT0 + 1.5f)
	{
		Say(FString::Printf(TEXT("E: the fallback: %s"), UnstuckCount > UnstuckTestCount0 ? TEXT("moved") : TEXT("NOT MOVED")));
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

OL_SHOT_MATCH(OLUnstuckTest) { return OLFlags::Param(TEXT("OLUnstuckTest")); }
OL_SHOT_TEST(OLUnstuckTest, 960)
{
	UnstuckShotTick();               // (BUG-STUCKGAP: OutlawsPlayerUnstuck.cpp)
}

void AOutlawsPlayer::NarrowWalkShotTick()
{
	// (BUG-UNSTUCKNARROW, user 2026-10-08: walking the hideout's fence top set the rescue off) -OLShots
	// -OLNarrowWalkTest=406 (sector indexes, commas between) or =narrow (the level's thin walled floors, a few): each one
	// stood on at its line's first point and walked down its middle to the last; no rescue may fire on the way
	AOutlawsLevelActor* L = Level.Get();
	UWorld* W = GetWorld();
	if (!L || !W) return;
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	auto Start = [&](int32 Point)
	{
		const FVector At = NarrowTestLine[Point];
		TeleportTo(At + FVector(0.0, 0.0, Half + 2.0), GetActorRotation(), false, true);
		GetCharacterMovement()->StopMovementImmediately();
		GetCharacterMovement()->SetMovementMode(MOVE_Falling);
		NarrowTestPoint = Point + 1;
	};
	auto Next = [&]()
	{
		while (NarrowTestAt < NarrowTestSectors.Num())
		{
			const int32 Si = NarrowTestSectors[NarrowTestAt];
			if (L->SectorWalkLine(Si, 45.0, NarrowTestLine, NarrowTestWidth, NarrowTestLength) && NarrowTestLine.Num() >= 2)
			{
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT narrow walk: %s; its walk line %d points, %.0f cm long, %.0f cm across at the narrowest"), *L->DescribeSector(Si), NarrowTestLine.Num(),
					NarrowTestLength, NarrowTestWidth);
				NarrowTestFalls = 0;
				NarrowTestChecks = 0;
				NarrowTestCount0 = UnstuckCount;
				NarrowTestT0 = ShotTime;
				Start(0);
				return true;
			}
			++NarrowTestAt;
		}
		return false;
	};
	if (ShotStep == 0 && ShotTime > 4.f)
	{
		FString Arg;
		OLFlags::Value(TEXT("OLNarrowWalkTest"), Arg, false);
		TArray<FString> Parts;
		Arg.ParseIntoArray(Parts, TEXT(","));
		for (const FString& A : Parts)
		{
			if (A.IsNumeric()) NarrowTestSectors.Add(FCString::Atoi(*A));
		}
		if (Arg.Contains(TEXT("narrow")))
		{
			// the raised floors (two walls or more drop off them) at most 75 cm across, 3 m long down their line and with a
			// standing body's room (fence tops, beams, ledges): a few walked
			TArray<FVector> Line;
			double Wd = 0.0, Ln = 0.0;
			int32 Found = 0;
			for (int32 Si = 0; Si < L->NumSectors(); ++Si)
			{
				int32 Drops = 0;
				if (!L->SectorWalkLine(Si, 45.0, Line, Wd, Ln) || Wd > 75.0 || Ln < 300.0 || NarrowTestSectors.Contains(Si) || L->SectorHeadroomCm(Si, Drops) < 200.0 || Drops < 2) continue;
				++Found;
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT narrow walk: narrow sector %d, %.0f cm across, %.0f cm long"), Si, Wd, Ln);
				if (Found <= 6) NarrowTestSectors.Add(Si);
			}
		}
		NarrowTestAt = 0;
		ShotStep = 1;
		if (!Next())
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT narrow walk: no sector to walk"));
			FGenericPlatformMisc::RequestExit(false);
			ShotTime = -1.f;
		}
		return;
	}
	if (ShotStep != 1) return;
	if (BodyStuckSince >= 0.0 || HeadOutSince >= 0.0) ++NarrowTestChecks;
	const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, Half);
	const bool bDone = NarrowTestPoint >= NarrowTestLine.Num() || ShotTime > NarrowTestT0 + 90.f;
	if (!bDone)
	{
		const FVector Goal = NarrowTestLine[NarrowTestPoint];
		if (Feet.Z < Goal.Z - 60.0 && GetCharacterMovement()->IsMovingOnGround())
		{
			// off the side: back on at the point it was walking to
			++NarrowTestFalls;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT narrow walk: off the side at %s (feet %.0f cm under the line), back on at point %d"), *Feet.ToCompactString(), Goal.Z - Feet.Z, NarrowTestPoint);
			Start(NarrowTestPoint);
			return;
		}
		if (FVector::Dist2D(Feet, Goal) < 15.0)
		{
			++NarrowTestPoint;
			return;
		}
		const FVector D = Goal - Feet;
		if (APlayerController* PC = Cast<APlayerController>(GetController())) PC->SetControlRotation(FRotator(0.0, D.Rotation().Yaw, 0.0));
		HoldMoveStick(FVector2D(0.0, FMath::Clamp(FVector::Dist2D(Feet, Goal) / 80.0, 0.6, 1.0)), 0.2f);
		return;
	}
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT narrow walk: sector %d walked to point %d of %d (now at %s) in %.1f s, falls off %d, stuck checks running %d looks, rescues %d (%s); last: %s"),
		NarrowTestSectors[NarrowTestAt], FMath::Min(NarrowTestPoint, NarrowTestLine.Num()), NarrowTestLine.Num(), *Feet.ToCompactString(), ShotTime - NarrowTestT0, NarrowTestFalls, NarrowTestChecks,
		UnstuckCount - NarrowTestCount0, UnstuckCount == NarrowTestCount0 ? TEXT("ok") : TEXT("RESCUED (WRONG)"), LastUnstuck.IsEmpty() ? TEXT("none") : *LastUnstuck);
	HoldMoveStick(FVector2D::ZeroVector, 0.f);
	++NarrowTestAt;
	if (!Next())
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT narrow walk: done, rescues in all %d"), UnstuckCount);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

OL_SHOT_MATCH(OLNarrowWalkTest) { return OLFlags::HasValue(TEXT("OLNarrowWalkTest")); }
OL_SHOT_TEST(OLNarrowWalkTest, 940)
{
	NarrowWalkShotTick();            // (BUG-UNSTUCKNARROW: OutlawsPlayerUnstuck.cpp)
}

void AOutlawsPlayer::UnstuckLoopShotTick()
{
	// (BUG-UNSTUCKLOOP, the user's Quest log of 10 Oct: townex by X=6065 Y=3931 the head stood 60 cm off the body through a
	// wall, the rescue put the body back on its own spot with the head still out, 22 rescues, 9 of them a second apart)
	AOutlawsLevelActor* L = Level.Get();
	UWorld* W = GetWorld();
	if (!L || !W) return;
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	auto HeadNow = [this]() { return Camera ? Camera->GetComponentLocation() : GetActorLocation(); };
	if (ShotStep == 0 && ShotTime > 4.f)
	{
		const FVector Feet(6064.55, 3931.32, 9.5);
		TeleportTo(Feet + FVector(0.0, 0.0, Half + 2.0), GetActorRotation(), false, true);
		GetCharacterMovement()->StopMovementImmediately();
		GetCharacterMovement()->SetMovementMode(MOVE_Falling);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT unstuckloop: level %s, the body put at %s (feet sector %d)"), *L->LevelName, *GetActorLocation().ToCompactString(),
			L->SectorHoldingPoint(Feet + FVector(0.0, 0.0, 5.0), 15.0));
		UnstuckTestT0 = ShotTime;
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > UnstuckTestT0 + 2.f)
	{
		// the head moved off the body as a room-scale step does (the play space, not the capsule): the log's offset first,
		// else the nearest 60..80 cm one that leaves the head outside every sector
		FVector Safe;
		const bool bSafe = LastSafeFeet(Safe);
		const FVector Body = GetActorLocation();
		FVector Off(-37.0, 49.0, 0.0);
		for (int32 k = 0; k < 48 && L->SectorHoldingPoint(HeadNow() + Off, 2.0) != -1; ++k)
		{
			Off = FRotator(0.0, 22.5 * (k % 16), 0.0).Vector() * (60.0 + 10.0 * (k / 16));
		}
		VROrigin->AddWorldOffset(Off);
		const FVector Head = HeadNow();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT unstuckloop: standing at %s, safe spot %s; the head moved %s off the body to %s, head sector %d (%s)"), *Body.ToCompactString(),
			bSafe ? *Safe.ToCompactString() : TEXT("NONE"), *Off.ToCompactString(), *Head.ToCompactString(), L->SectorHoldingPoint(Head, 2.0),
			L->SectorHoldingPoint(Head, 2.0) == -1 ? TEXT("outside every sector, as on the Quest") : TEXT("STILL INSIDE: no repro"));
		UnstuckTestCount0 = UnstuckCount;
		UnstuckTestT0 = ShotTime;
		++ShotStep;
	}
	else if (ShotStep == 2 && ShotTime > UnstuckTestT0 + 14.f)
	{
		const int32 Rescues = UnstuckCount - UnstuckTestCount0;
		const FVector Body = GetActorLocation(), Head = HeadNow();
		const FVector Feet = Body - FVector(0.0, 0.0, Half);
		const int32 HeadSector = L->SectorHoldingPoint(Head, 2.0), FeetSector = L->SectorHoldingPoint(Feet + FVector(0.0, 0.0, 5.0), 15.0);
		FVector On;
		const bool bStand = SafeFeetAt(Feet, On);
		const bool bPass = Rescues >= 1 && Rescues <= 2 && HeadSector >= 0 && FeetSector >= 0 && bStand;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT unstuckloop: %s: %d rescues in 14 s (at most 2 wanted); now the body %s (feet sector %d), the head %s (sector %d), %s; last: %s"),
			bPass ? TEXT("PASS") : TEXT("FAIL"), Rescues, *Body.ToCompactString(), FeetSector, *Head.ToCompactString(), HeadSector, bStand ? TEXT("standing safe") : TEXT("NOT A SAFE SPOT"),
			LastUnstuck.IsEmpty() ? TEXT("none") : *LastUnstuck);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

OL_SHOT_MATCH(OLUnstuckLoopTest) { return OLFlags::Param(TEXT("OLUnstuckLoopTest")); }
OL_SHOT_TEST(OLUnstuckLoopTest, 950)
{
	UnstuckLoopShotTick();           // (BUG-UNSTUCKLOOP: OutlawsPlayerUnstuck.cpp)
}
#endif // WITH_OUTLAWS_TESTS
