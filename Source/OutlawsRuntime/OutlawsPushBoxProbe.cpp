// BUG-PUSHBOX (user 2026-11-01, Quest snapshots 7 and 8: civlwar1's ASH ENGINE COMPANY crate, SHOVE_ME in sector 305, moved
// on the first push and never again): -OLShots -OLLevel=civlwar1 -OLNoFrontEnd -OLPushBoxProbe [-OLOldPushBox].
// Every pushable crate on the level (a MORPH_MOVE a nudge works whose last stop is TERMINATE: SHOVE_ME, civlwar2's CRATEs)
// is pushed as a player does it: the player walks up to the face behind it (the side ANGLE points away from), stands there
// a second with the unstuck's checks on (they sent the user back), and a hand at the face pushes it as PushDoors finds it
// (DoorAtHand, PushDoorSector); once it rests, again from behind it, to its last stop.  After each move: its stop, its mesh
// and its outline in the live level (the crate's sector and its hole in the room round it).  The first crate also, from its
// first stop and before that: a side face (pushed from beside it, it still slides along ANGLE) and the face it slides to (it
// comes at the player and waits while the player stands in its way, as Jedi's sector_canMoveWalls holds it, and goes on
// once the player steps back); a crate with no room behind it is pushed from beside it.  A side with no room to stand is a
// SKIP.  Shots pushbox_<new|old>_<crate>_<push1|walk2|push2|end>.png in Saved\OLShots.
// Lines "OLSHOT pushbox <crate> ..." a case, "OLSHOT pushbox check PASS|FAIL: ..." and "OLSHOT pushbox RESULT".
// -OLOldPushBox: the checks expect the old behaviour (the crate moves once; the walk in behind it is outside every sector).

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/Controller.h"
#include "ProceduralMeshComponent.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

bool AOutlawsLevelActor::DebugMoverFace(int32 Elevator, const FVector& Side, FVector& OutFace, FBox& OutBox) const
{
	const FMoverGroup* G = Groups.FindByPredicate([Elevator](const FMoverGroup& M) { return M.Elevator == Elevator && M.Main; });
	if (!G) return false;
	OutBox = G->Main->Bounds.GetBox();
	const FVector C = OutBox.GetCenter();
	const FVector D = FVector(Side.X, Side.Y, 0.0).GetSafeNormal();
	FHitResult Face;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsPushBoxProbe), true);
	if (!G->Main->LineTraceComponent(Face, C + D * (OutBox.GetExtent().Size() + 100.0), C, Q)) return false;
	OutFace = Face.ImpactPoint;
	return true;
}

void AOutlawsLevelActor::DebugResetMover(int32 Elevator)
{
	if (!Inf || !Inf->Elevators.IsValidIndex(Elevator)) return;
	Inf->SetFromHost(Elevator, Inf->StopValue(Inf->Elevators[Elevator], 0), 0, -1, false, false, true);
}

namespace
{
	enum EPushSide { PushBehind, PushBeside, PushAhead };
	const TCHAR* const PushSideName[] = { TEXT("behind it"), TEXT("beside it"), TEXT("the side it slides to") };

	struct FPushBoxRun
	{
		TArray<int32> Boxes;          // elevator indices
		int32 Box = 0;
		EPushSide Side = PushBehind;
		int32 Step = 0;
		int32 Push = 0;               // pushes taken on this side
		int32 Moves = 0;              // moves seen on this side
		double At = 0.0, WalkStart = 0.0, HeldFrom = 0.0;
		FVector Stand = FVector::ZeroVector, Face = FVector::ZeroVector, Out = FVector::ZeroVector;
		FBox Mesh = FBox(ForceInit), Mesh0 = FBox(ForceInit);
		FVector2D Outline0 = FVector2D::ZeroVector;     // the crate's sector's middle in the live level, at its first stop
		double HalfLen = 0.0;                           // half its length along ANGLE (units)
		int32 StopBefore = 0;
		double HeadOut = -1.0, HeadOutMost = 0.0;
		int32 InBox = 0, Ticks = 0;
		FString UnstuckWas;
		bool bSentBack = false;
		bool bSkipped = false;        // no room to stand on this side
		int32 Passes = 0, Fails = 0;
	};
	FPushBoxRun GPushBox;

	FVector2D PushBoxMiddle(const FOutlawsSector& S)
	{
		FVector2D C = FVector2D::ZeroVector;
		for (const FVector2D& V : S.Verts) C += V;
		return S.Verts.Num() ? C / S.Verts.Num() : C;
	}
}

OL_SHOT_MATCH(OLPushBoxProbe) { return OLFlags::Param(TEXT("OLPushBoxProbe")); }

OL_SHOT_TEST(OLPushBoxProbe, 1360)
{
	if (!L || ShotStep < 0) return;
	const FOutlawsInfRuntime* Inf = L->DebugInf();
	UWorld* World = GetWorld();
	if (!Inf || !World) return;
	const bool bOld = FOutlawsInfRuntime::OldPushBox();
	const TCHAR* const Tag = bOld ? TEXT("old") : TEXT("new");
	const double Now = World->GetTimeSeconds();
	const float R = GetCapsuleComponent()->GetScaledCapsuleRadius(), Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT pushbox check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++GPushBox.Passes; else ++GPushBox.Fails;
	};
	auto Shot = [&](const FString& What)
	{
		const FString Name = FString::Printf(TEXT("pushbox_%s_%s"), Tag, *What);
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT pushbox shot %s.png"), *Name);
	};
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT pushbox RESULT %s (%d passed, %d failed)"), GPushBox.Fails == 0 && GPushBox.Passes > 1 ? TEXT("ALL PASS") : TEXT("FAIL"), GPushBox.Passes, GPushBox.Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	if (!bUp) return;
	const int32 e = GPushBox.Boxes.IsValidIndex(GPushBox.Box) ? GPushBox.Boxes[GPushBox.Box] : -1;
	const FOutlawsElevator* E = Inf->Elevators.IsValidIndex(e) ? &Inf->Elevators[e] : nullptr;
	const FOutlawsInfClass* C = E ? &Inf->Inf.Items[E->Item].Classes[E->ClassIndex] : nullptr;
	const int32 Master = E ? E->Sectors[0] : -1;
	// the slide's direction on the map (UE, a unit): ANGLE's (x, z) as the level actor turns it
	const FVector Dir = E ? FVector(E->MorphDir.Y, E->MorphDir.X, 0.0).GetSafeNormal() : FVector::ZeroVector;
	auto SideDir = [&](EPushSide S, int32 Which) -> FVector
	{
		return S == PushBehind ? -Dir : S == PushAhead ? Dir : (Which == 0 ? FVector(-Dir.Y, Dir.X, 0.0) : FVector(Dir.Y, -Dir.X, 0.0));
	};
	auto Ground = [&](const FVector& P, double& OutZ) -> bool
	{
		FHitResult H;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsPushBoxProbe), false, this);
		if (!World->LineTraceSingleByChannel(H, P + FVector(0.0, 0.0, 40.0), P - FVector(0.0, 0.0, 600.0), ECC_Visibility, Q)) return false;
		OutZ = H.ImpactPoint.Z;
		return true;
	};
	auto Free = [&](const FVector& Body) -> bool
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsPushBoxProbe), false, this);
		return !World->OverlapBlockingTestByChannel(Body, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(R - 2.f, Half - 4.f), Q);
	};
	// where to stand to push from side S (the face's middle, R + 12 cm out), on the floor there; false when no room
	auto StandAt = [&](EPushSide S, int32 Which, FVector& OutStand, FVector& OutFace, FVector& OutOut) -> bool
	{
		OutOut = SideDir(S, Which);
		FBox B(ForceInit);
		if (!L->DebugMoverFace(e, OutOut, OutFace, B)) return false;
		GPushBox.Mesh = B;
		double Gz = 0.0;
		const FVector Spot = OutFace + OutOut * (R + 12.0);
		if (!Ground(FVector(Spot.X, Spot.Y, B.Min.Z + 20.0), Gz)) return false;
		OutStand = FVector(Spot.X, Spot.Y, Gz + Half + 2.0);
		// and nothing between there and the face (civlwar1's crate runs between walls 4 cm off its sides)
		FHitResult H;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsPushBoxProbe), true, this);
		const FVector Eye(Spot.X, Spot.Y, OutFace.Z);
		if (World->LineTraceSingleByChannel(H, Eye, OutFace - OutOut * 2.0, ECC_Visibility, Q) && FVector::Dist(H.ImpactPoint, OutFace) > 1.5) return false;
		return Free(OutStand);
	};
	auto Sample = [&](double Dt2)
	{
		// the player's sector (the tracking's), and the head's (the unstuck's check): outside every sector for 0.75 s sends
		// the player back
		++GPushBox.Ticks;
		if (L->PlayerSectorIndex() == Master) ++GPushBox.InBox;
		if (L->SectorHoldingPoint(Camera->GetComponentLocation(), 2.0) == -1) GPushBox.HeadOut = FMath::Max(0.0, GPushBox.HeadOut) + Dt2;
		else GPushBox.HeadOut = -1.0;
		GPushBox.HeadOutMost = FMath::Max(GPushBox.HeadOutMost, GPushBox.HeadOut);
		if (LastUnstuck != GPushBox.UnstuckWas) { GPushBox.bSentBack = true; GPushBox.UnstuckWas = LastUnstuck; }
	};
	auto Look = [&](const FVector& At)
	{
		if (Controller) Controller->SetControlRotation(FRotator(-8.0, (At - GetActorLocation()).Rotation().Yaw, 0.0));
	};

	switch (ShotStep)
	{
	case 0:
	{
		// the level's crates: a MORPH_MOVE a nudge works, its last stop TERMINATE
		if (ShotTime < 4.f) return;
		L->bInvincible = true;
		for (int32 i = 0; i < Inf->Elevators.Num(); ++i)
		{
			const FOutlawsElevator& X = Inf->Elevators[i];
			const FOutlawsInfClass& Xc = Inf->Inf.Items[X.Item].Classes[X.ClassIndex];
			if (X.Class != EOutlawsInfClass::MorphMove || !(Xc.EventMask & (OutlawsInf::EvNudgeFront | OutlawsInf::EvNudgeBack)) || Xc.Stops.Num() < 2
				|| Xc.Stops.Last().Action != EOutlawsStopAction::Terminate) continue;
			GPushBox.Boxes.Add(i);
			FString Stops;
			for (const FOutlawsInfStop& St : Xc.Stops) Stops += FString::Printf(TEXT(" %.2f"), St.Value);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT pushbox crate %s: elevator %d, sector %d, ANGLE %.0f, EVENT_MASK %u, stops%s, speed %.1f; %d outline vertices slide (%s)"),
				*X.Name, i, X.Sectors[0], Xc.Angle, Xc.EventMask, *Stops, X.Speed, X.MorphVerts.Num(), *L->DebugSectorBrief(X.Sectors[0]));
		}
		Check(GPushBox.Boxes.Num() > 0, FString::Printf(TEXT("%d pushable crates on %s"), GPushBox.Boxes.Num(), *L->LevelName));
		Check(Inf->MorphOutlinesOn() == !bOld, FString::Printf(TEXT("the slides' outlines move in the live level: %d (wanted %d)"), Inf->MorphOutlinesOn() ? 1 : 0, bOld ? 0 : 1));
		if (GPushBox.Boxes.Num() == 0) { Finish(); return; }
		GPushBox.Box = 0;
		GPushBox.Side = PushBeside;
		GPushBox.UnstuckWas = LastUnstuck;
		ShotStep = 1;
		return;
	}
	case 1:
	{
		// a side's first push: the player put down 1.2 m out from where they stand to push, facing the crate
		GPushBox.Push = GPushBox.Moves = 0;
		GPushBox.Outline0 = PushBoxMiddle(Inf->Live.Sectors[Master]) - (Inf->MorphOutlinesOn() ? E->MorphDir * E->Current : FVector2D::ZeroVector);
		GPushBox.HalfLen = 0.0;
		for (const FVector2D& V : Inf->Live.Sectors[Master].Verts) GPushBox.HalfLen = FMath::Max(GPushBox.HalfLen, FMath::Abs(FVector2D::DotProduct(V - PushBoxMiddle(Inf->Live.Sectors[Master]), E->MorphDir)));
		bool bRoom = false;
		int32 Which = 0;
		for (; Which < (GPushBox.Side == PushBeside ? 2 : 1) && !bRoom; ++Which) bRoom = StandAt(GPushBox.Side, Which, GPushBox.Stand, GPushBox.Face, GPushBox.Out);
		FVector From = GPushBox.Stand + GPushBox.Out * 120.0;
		double Gz = 0.0;
		if (bRoom && Ground(From, Gz)) From.Z = Gz + Half + 2.0;
		if (bRoom && !Free(From)) From = GPushBox.Stand + GPushBox.Out * 30.0;
		if (!bRoom)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT pushbox %s from %s: SKIP, no room to stand there (the level's crates and walls)"), *E->Name, PushSideName[GPushBox.Side]);
			GPushBox.bSkipped = true;
			ShotStep = 9;
			return;
		}
		GPushBox.Mesh0 = GPushBox.Mesh;
		TeleportTo(From, FRotator(0.0, (-GPushBox.Out).Rotation().Yaw, 0.0), false, true);
		Look(GPushBox.Face);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT pushbox %s from %s: stands at %s to push the face at %s (stop %d, value %.2f)"), *E->Name, PushSideName[GPushBox.Side], *GPushBox.Stand.ToCompactString(),
			*GPushBox.Face.ToCompactString(), E->Stop, E->Current);
		GPushBox.At = Now + 0.5;
		ShotStep = 2;
		return;
	}
	case 2:
	{
		// walk up to the face (every frame, the level's tracking and the unstuck looking on)
		if (Now < GPushBox.At) return;
		GPushBox.HeadOut = -1.0;
		GPushBox.HeadOutMost = 0.0;
		GPushBox.InBox = GPushBox.Ticks = 0;
		GPushBox.bSentBack = false;
		GPushBox.UnstuckWas = LastUnstuck;
		GPushBox.WalkStart = Now;
		GPushBox.StopBefore = E->Stop;
		// (the crate where the last push left it, the floor it left between: the walk up to it again)
		if (GPushBox.Side == PushBehind && GPushBox.Box == 0 && GPushBox.Push == 1) Shot(FString::Printf(TEXT("%s_walk2"), *E->Name));
		ShotStep = 3;
		return;
	}
	case 3:
	{
		Sample(Dt);
		const FVector Me = GetActorLocation();
		const double Left = FVector::Dist2D(Me, GPushBox.Stand);
		if (Left > 6.0 && Now - GPushBox.WalkStart < 6.0)
		{
			const FVector To = (GPushBox.Stand - Me).GetSafeNormal2D();
			Look(GPushBox.Face);
			AddMovementInput(To, float(FMath::Clamp(Left / 80.0, 0.2, 1.0)));
			return;
		}
		GPushBox.At = Now + 1.0;           // (a second there: the unstuck acts after 0.75 s of the head outside every sector)
		ShotStep = 4;
		return;
	}
	case 4:
	{
		Sample(Dt);
		Look(GPushBox.Face);
		if (Now < GPushBox.At) return;
		const FVector Eye = Camera->GetComponentLocation();
		const FVector Target(GPushBox.Face.X, GPushBox.Face.Y, FMath::Clamp(Eye.Z, GPushBox.Mesh.Min.Z + 25.0, GPushBox.Mesh.Max.Z - 25.0));
		const FVector Hand = Target - (Target - Eye).GetSafeNormal() * 5.0;
		const double Reach = FVector::Dist(Eye, Hand);
		bool bClosed = false;
		const int32 Door = Reach < 100.0 ? L->DoorAtHand(Eye, Hand, bClosed) : -1;
		const bool bTaken = Door == Master && bClosed && L->PushDoorSector(Master);
		const bool bStood = GPushBox.HeadOutMost < 0.75 && !GPushBox.bSentBack;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT pushbox %s push %d from %s at stop %d: walked %.1f s, stood in sector %d (in the crate's own %d of %d looks), the head outside every sector %.2f s at most, %s; the hand %.0f cm from the eyes, DoorAtHand %d closed %d, taken %d"),
			*E->Name, GPushBox.Push + 1, PushSideName[GPushBox.Side], E->Stop, GPushBox.At - 1.0 - GPushBox.WalkStart, L->PlayerSectorIndex(), GPushBox.InBox, GPushBox.Ticks, FMath::Max(0.0, GPushBox.HeadOutMost),
			GPushBox.bSentBack ? *FString::Printf(TEXT("SENT BACK by the unstuck (%s)"), *LastUnstuck.Left(160)) : TEXT("not sent back"), Reach, Door, bClosed ? 1 : 0, bTaken ? 1 : 0);
		if (GPushBox.Side == PushBehind && GPushBox.Box == 0 && GPushBox.Push < 2) Shot(FString::Printf(TEXT("%s_push%d"), *E->Name, GPushBox.Push + 1));
		if (E->bTerminated)
		{
			Check(!bTaken, FString::Printf(TEXT("%s at its last stop (%d) takes no more pushes"), *E->Name, E->Stop));
			ShotStep = 9;
			return;
		}
		if (bOld && GPushBox.Side == PushBehind && GPushBox.Push > 0)
		{
			// as before: once it has moved, the place behind it is still its own sector (the air over its top)
			Check(!(bStood && bTaken), FString::Printf(TEXT("as before (-OLOldPushBox): push %d of %s from behind it fails (stood %d, taken %d; the walk in behind it was in the crate's sector %d of %d looks)"),
				GPushBox.Push + 1, *E->Name, bStood ? 1 : 0, bTaken ? 1 : 0, GPushBox.InBox, GPushBox.Ticks));
			ShotStep = 9;
			return;
		}
		Check(bStood && GPushBox.InBox == 0, FString::Printf(TEXT("%s push %d from %s: the player stood at the crate (the head in a sector, %.2f s outside at most; in the crate's sector %d of %d looks; %s)"),
			*E->Name, GPushBox.Push + 1, PushSideName[GPushBox.Side], FMath::Max(0.0, GPushBox.HeadOutMost), GPushBox.InBox, GPushBox.Ticks, GPushBox.bSentBack ? TEXT("sent back") : TEXT("not sent back")));
		Check(bTaken, FString::Printf(TEXT("%s push %d from %s is taken (DoorAtHand %d, closed %d)"), *E->Name, GPushBox.Push + 1, PushSideName[GPushBox.Side], Door, bClosed ? 1 : 0));
		if (!bTaken) { ShotStep = 9; return; }
		++GPushBox.Push;
		GPushBox.HeldFrom = Now;
		GPushBox.At = Now + 0.25;
		ShotStep = GPushBox.Side == PushAhead ? 6 : 5;
		return;
	}
	case 5:
	{
		// the move: once it rests, its stop, its mesh and its outline
		if (Now < GPushBox.At || (E->Target >= 0 && Now - GPushBox.HeldFrom < 8.0)) return;
		FVector F;
		FBox B(ForceInit);
		L->DebugMoverFace(e, -Dir, F, B);
		const double MeshMoved = FVector::DotProduct(B.GetCenter() - GPushBox.Mesh0.GetCenter(), Dir) / FOutlawsGeometry::UnitsToCm;
		const FVector2D Mid = PushBoxMiddle(Inf->Live.Sectors[Master]);
		const double OutlineMoved = FVector2D::DotProduct(Mid - GPushBox.Outline0, E->MorphDir);
		++GPushBox.Moves;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT pushbox %s move %d: stop %d -> %d, value %.2f%s; the mesh %.2f units along ANGLE from its first stop, the outline in the live level %.2f (%s)"),
			*E->Name, GPushBox.Moves, GPushBox.StopBefore, E->Stop, E->Current, E->bTerminated ? TEXT(" (ended)") : TEXT(""), MeshMoved, OutlineMoved, *L->DebugSectorBrief(Master));
		Check(E->Target < 0 && E->Stop == GPushBox.StopBefore + 1, FString::Printf(TEXT("%s push %d steps one stop (%d -> %d)"), *E->Name, GPushBox.Push, GPushBox.StopBefore, E->Stop));
		Check(FMath::Abs(MeshMoved - E->Current) < 0.05, FString::Printf(TEXT("%s's mesh is where its value says (%.2f units, value %.2f)"), *E->Name, MeshMoved, E->Current));
		const double Want = bOld ? 0.0 : E->Current;
		Check(FMath::Abs(OutlineMoved - Want) < 0.01, FString::Printf(TEXT("%s's outline in the live level %s (%.2f units moved, wanted %.2f)"), *E->Name,
			bOld ? TEXT("stays where the level put it, as before") : TEXT("moved with it"), OutlineMoved, Want));
		if (!bOld)
		{
			// the place it left is the room's again: halfway along the strip between its first back face and its back face now
			const FVector2D Left = GPushBox.Outline0 + E->MorphDir * (-GPushBox.HalfLen + 0.5 * FMath::Min(E->Current, 2.0 * GPushBox.HalfLen));
			const FOutlawsSector* There = Inf->Live.SectorAt(Left.X, Inf->Live.Sectors[Master].Floor.Y - 1.0, Left.Y);
			const int32 Room = There ? There->Index : -1;
			Check(Room >= 0 && Room != Master, FString::Printf(TEXT("%s: the floor it left (%.1f, %.1f) is in sector %d, not the crate's %d"), *E->Name, Left.X, Left.Y, Room, Master));
		}
		if (GPushBox.Side == PushBehind && GPushBox.Box == 0 && E->bTerminated) Shot(FString::Printf(TEXT("%s_end"), *E->Name));
		// the next push from the same side, walking on from here
		GPushBox.At = Now + 0.3;
		ShotStep = 7;
		return;
	}
	case 6:
	{
		// pushed from the side it slides to: it comes at the player.  Jedi holds it while the player is in its way (new); as
		// before it went through the player (old)
		if (Now < GPushBox.HeldFrom + 1.5) return;
		const double Value = E->Current;
		const double Goal = Inf->StopValue(*E, GPushBox.StopBefore + 1);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT pushbox %s pushed from the side it slides to: 1.5 s on, value %.2f of %.2f, stop %d, moving %d; the player %.0f cm from where they pushed"),
			*E->Name, Value, Goal, E->Stop, E->Target >= 0 ? 1 : 0, FVector::Dist2D(GetActorLocation(), GPushBox.Stand));
		if (bOld) Check(E->Target < 0 && FMath::Abs(Value - Goal) < 1e-3, FString::Printf(TEXT("as before (-OLOldPushBox): %s slid on through the player to its next stop (%.2f)"), *E->Name, Value));
		else Check(E->Target >= 0 && FMath::Abs(Value - Goal) > 1.0, FString::Printf(TEXT("%s waits while the player stands in its way (value %.2f of %.2f, still on the move)"), *E->Name, Value, Goal));
		if (bOld) { ShotStep = 9; return; }
		// the player steps back out of its way: it goes on to its stop
		FVector Back = GPushBox.Stand + GPushBox.Out * (Goal * FOutlawsGeometry::UnitsToCm + 150.0);
		double Gz = 0.0;
		if (Ground(Back, Gz)) Back.Z = Gz + Half + 2.0;
		TeleportTo(Back, GetActorRotation(), false, true);
		GPushBox.At = Now + 2.5;
		ShotStep = 8;
		return;
	}
	case 7:
	{
		if (Now < GPushBox.At) return;
		if (!StandAt(GPushBox.Side, 0, GPushBox.Stand, GPushBox.Face, GPushBox.Out) && GPushBox.Side == PushBeside) StandAt(GPushBox.Side, 1, GPushBox.Stand, GPushBox.Face, GPushBox.Out);
		ShotStep = 2;
		return;
	}
	case 8:
	{
		if (Now < GPushBox.At) return;
		const double Goal = Inf->StopValue(*E, GPushBox.StopBefore + 1);
		Check(E->Target < 0 && E->Stop == GPushBox.StopBefore + 1 && FMath::Abs(E->Current - Goal) < 1e-3, FString::Printf(TEXT("%s goes on to its stop once the player is out of its way (value %.2f, stop %d, at rest %d)"),
			*E->Name, E->Current, E->Stop, E->Target < 0 ? 1 : 0));
		ShotStep = 9;
		return;
	}
	case 9:
	{
		// this side done.  The first crate: beside it, the side it slides to, then behind it, each from its first stop (so it
		// ends at its last stop, out of the next crate's way: shgulch's SHOVE_ME before SHOVE_ME2); the others behind it,
		// beside it when there is no room behind (townex's crate stands against the wall it is pushed off)
		const int32 Moved = GPushBox.Moves;
		const int32 N = C->Stops.Num();
		if (GPushBox.Side == PushBehind && !GPushBox.bSkipped)
		{
			if (bOld) Check(Moved <= 1, FString::Printf(TEXT("as before (-OLOldPushBox): %s moved %d time(s) from behind it, then no more"), *E->Name, Moved));
			else Check(Moved == N - 1, FString::Printf(TEXT("%s moved %d times from behind it, every stop to its last (%d stops)"), *E->Name, Moved, N));
		}
		else if (GPushBox.Side == PushBeside && GPushBox.Push > 0 && !bOld)
		{
			Check(Moved == GPushBox.Push, FString::Printf(TEXT("%s pushed from beside it: %d pushes, %d moves along ANGLE"), *E->Name, GPushBox.Push, Moved));
		}
		const bool bWasSkipped = GPushBox.bSkipped;
		GPushBox.bSkipped = false;
		if (GPushBox.Box == 0 && GPushBox.Side != PushBehind)
		{
			GPushBox.Side = GPushBox.Side == PushBeside ? PushAhead : PushBehind;
			L->DebugResetMover(e);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT pushbox %s put back at its first stop (value %.2f): %s"), *E->Name, E->Current, *L->DebugSectorBrief(Master));
			ShotStep = 1;
			return;
		}
		if (GPushBox.Box > 0 && GPushBox.Side == PushBehind && bWasSkipped)
		{
			GPushBox.Side = PushBeside;
			ShotStep = 1;
			return;
		}
		if (++GPushBox.Box >= GPushBox.Boxes.Num()) { Finish(); return; }
		GPushBox.Side = PushBehind;
		ShotStep = 1;
		return;
	}
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
