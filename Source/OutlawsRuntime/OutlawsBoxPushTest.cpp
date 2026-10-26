// BUG-BOXPUSHONCE: the pushable boxes' check (shgulch's SHOVE_ME and SHOVE_ME2, shgranry's SHOVE_ME).
//   -OLShots -OLLevel=shgulch -OLNoFrontEnd -OLBoxPushTest: every SHOVE* box is pushed as a hand push finds it (DoorAtHand on the
//   box's face, then PushDoorSector), again each time it comes to rest, until it reaches its TERMINATE stop; every move is logged.
//   A box must take one push per stop after its first and step one stop each time; once it is ended a push is refused.
//   Prints "OLSHOT boxpush check PASS|FAIL: ..." and "OLSHOT boxpush RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsInfRuntime.h"
#include "Engine/World.h"
#include "ProceduralMeshComponent.h"
#include "OutlawsShotTests.h"

#if WITH_OUTLAWS_TESTS

FString AOutlawsLevelActor::DebugHandPushMover(int32 Elevator, bool& bOutFound, bool& bOutClosed, bool& bOutTaken)
{
	// a hand 5 cm off the box's face, the eyes 60 cm behind it: tried on each side, the first the hand push finds the box from
	bOutFound = bOutClosed = bOutTaken = false;
	if (!Inf || !Inf->Elevators.IsValidIndex(Elevator)) return TEXT("no elevator");
	const FMoverGroup* G = Groups.FindByPredicate([Elevator](const FMoverGroup& M) { return M.Elevator == Elevator && M.Main; });
	if (!G) return TEXT("no mover mesh");
	const int32 Master = Inf->Elevators[Elevator].Sectors[0];
	const FBox B = G->Main->Bounds.GetBox();
	const FVector C = B.GetCenter();
	for (const FVector& D : { FVector(1, 0, 0), FVector(-1, 0, 0), FVector(0, 1, 0), FVector(0, -1, 0) })
	{
		FHitResult Face;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsBoxPushTest), true);
		if (!G->Main->LineTraceComponent(Face, C + D * (B.GetExtent().Size() + 100.0), C, Q)) continue;
		const FVector Hand = Face.ImpactPoint + D * 5.0;
		const FVector Eye = Face.ImpactPoint + D * 60.0;
		bool bClosed = false;
		if (DoorAtHand(Eye, Hand, bClosed) != Master) continue;
		bOutFound = true;
		bOutClosed = bClosed;
		bOutTaken = bClosed && PushDoorSector(Master);
		return FString::Printf(TEXT("pushed from %s at %s"), *D.ToCompactString(), *Face.ImpactPoint.ToCompactString());
	}
	return TEXT("no side of the box the hand push finds");
}

void AOutlawsPlayer::BoxPushTestTick(float Dt)
{
	AOutlawsLevelActor* L = Level.Get();
	const FOutlawsInfRuntime* Inf = L ? L->DebugInf() : nullptr;
	if (!Inf || ShotTime < 4.f) return;
	static int32 Fails = 0, Passes = 0, Box = -1, Pushes = 0, LastStop = 0;
	static TArray<int32> Boxes;
	static bool bListed = false;
	static double WaitUntil = 0.0;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT boxpush check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	if (!bListed)
	{
		bListed = true;
		for (int32 i = 0; i < Inf->Elevators.Num(); ++i)
		{
			const FOutlawsElevator& E = Inf->Elevators[i];
			if (E.Name.StartsWith(TEXT("SHOVE")) && E.Class == EOutlawsInfClass::MorphMove) Boxes.Add(i);
		}
		Check(Boxes.Num() > 0, FString::Printf(TEXT("%d pushable boxes in %s"), Boxes.Num(), *L->LevelName));
		Box = 0;
		Pushes = 0;
		LastStop = Boxes.Num() > 0 ? Inf->Elevators[Boxes[0]].Stop : 0;
	}
	const double Now = GetWorld()->GetTimeSeconds();
	if (Now < WaitUntil) return;
	if (!Boxes.IsValidIndex(Box))
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT boxpush RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 1 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
		Box = 1 << 20;
		return;
	}
	const int32 e = Boxes[Box];
	const FOutlawsElevator& E = Inf->Elevators[e];
	if (E.Target >= 0) return;                       // (on the move: the next push once it rests)
	const int32 N = Inf->Inf.Items[E.Item].Classes[E.ClassIndex].Stops.Num();
	if (Pushes > 0)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT boxpush %s move %d: stop %d -> %d at %.2f%s"), *E.Name, Pushes, LastStop, E.Stop, E.Current, E.bTerminated ? TEXT(" (ended)") : TEXT(""));
		Check(E.Stop == LastStop + 1, FString::Printf(TEXT("%s push %d steps one stop (%d -> %d)"), *E.Name, Pushes, LastStop, E.Stop));
	}
	LastStop = E.Stop;
	bool bFound = false, bClosed = false, bTaken = false;
	const FString How = L->DebugHandPushMover(e, bFound, bClosed, bTaken);
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT boxpush %s at stop %d of %d: %s; found %d pushable %d taken %d"), *E.Name, E.Stop, N, *How, bFound ? 1 : 0, bClosed ? 1 : 0, bTaken ? 1 : 0);
	if (E.bTerminated)
	{
		Check(!bTaken, FString::Printf(TEXT("%s at its last stop takes no more pushes"), *E.Name));
		Check(Pushes == N - 1, FString::Printf(TEXT("%s moved %d times, every stop to its last (%d stops)"), *E.Name, Pushes, N));
		++Box;
		Pushes = 0;
		if (Boxes.IsValidIndex(Box)) LastStop = Inf->Elevators[Boxes[Box]].Stop;
		return;
	}
	if (!bTaken)
	{
		Check(false, FString::Printf(TEXT("%s push %d is refused at stop %d (%s)"), *E.Name, Pushes + 1, E.Stop, *How));
		++Box;
		Pushes = 0;
		if (Boxes.IsValidIndex(Box)) LastStop = Inf->Elevators[Boxes[Box]].Stop;
		return;
	}
	++Pushes;
	WaitUntil = Now + 0.3;                           // (the move starts)
}

OL_SHOT_MATCH(OLBoxPushTest) { return OLFlags::Param(TEXT("OLBoxPushTest")); }
OL_SHOT_TEST(OLBoxPushTest, 140)
{
	BoxPushTestTick(Dt);             // (BUG-BOXPUSHONCE: OutlawsBoxPushTest.cpp)
}
#endif // WITH_OUTLAWS_TESTS
