// BUG-CWBLASTFLOOR (user 2026-11-01, Quest snapshots 1-4: at the start of civlwar1 the blasts went off near the start and
// the fence, the roof and the trenches stayed as they were): -OLShots -OLLevel=civlwar1 -OLNoFrontEnd -OLCwBlastProbe
// [-OLOldCwBlast].  The start area's triggers (ATRIG, BTRIG, CTRIG, DTRIG through DTRIGDUMMY) and FTRIG send GOTO_STOP /
// NEXT_STOP to SLOPE_FLOOR / SLOPE_CEILING elevators (WALLA1-2 the fence, BCEIL1-3 the roof, DCRAT1-3 the craters, FCRASH)
// beside the MOVE_FLOOR ones (CHOLE1-2 the wall holes, the xBOOM spots).  The probe logs each sector's floor (BCEIL1: its
// ceiling) at its corners at the start, walks into the five triggers (DTRIG last, at snapshot 1's place), logs them again,
// traces down (up) onto each tilted surface where it moved most, and takes the snapshots' views: cwblast_<after|before>_
// <trench|fence|roof>.png in Saved\OLShots (trench: snapshot 1's pose; fence: snapshot 3's place, turned to WALLA1-2;
// roof: 15 m from BCEIL1 towards snapshot 4's place).  Then (the fix only) a save to slot 7 is loaded (the slopes as they
// were saved) and the same save cut at the first slope elevator as slot 9 (a save from before them: every other elevator
// put back, the sector lights, which never rest, not compared, the slopes at their start).  On any other level (civlwar2's dam sides, shgulch's BOOMCEILING1 and BOOMFLOOR)
// every slope elevator is sent to its last stop (GOTO_STOP, as its trigger does), logged and traced the same way, and one
// shot (cwblast_<tag>_<level>.png) looks at the first.  Lines "OLSHOT cwblast check PASS|FAIL: ..." and
// "OLSHOT cwblast RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsGeometry.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLCwBlastProbe) { return OLFlags::Param(TEXT("OLCwBlastProbe")); }

namespace
{
	struct FCwWatch
	{
		FString Name;
		bool bFloor = true;          // the surface the INF moves (BCEIL1: its ceiling)
		bool bSlope = true;          // a SLOPE_* elevator's (else CHOLE's MOVE_FLOOR)
		int32 Sector = -1;
		FVector2D At = FVector2D::ZeroVector;   // where the surface moves most (traced)
		double StartAt = 0.0;
	};
}

OL_SHOT_TEST(OLCwBlastProbe, 1390)
{
	static int32 Passes = 0, Fails = 0, Trig = 0, SlopeN = 0;
	static float Wait = 0.f;
	static bool bSeenPending = false, bCivil = false;
	static FVector Start = FVector::ZeroVector;
	static FRotator StartRot = FRotator::ZeroRotator;
	static TArray<FCwWatch> Watch;
	static TArray<double> SavedBefore;
	static TMap<FString, double> SavedAngle;
	if (!L || ShotStep < 0) return;
	const bool bOld = OLFlags::Param(TEXT("OLOldCwBlast"));
	const TCHAR* const Tag = bOld ? TEXT("before") : TEXT("after");
	const FOutlawsInfRuntime* Inf = L->DebugInf();
	const bool bUp = Inf && !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	const double U = FOutlawsGeometry::UnitsToCm;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cwblast check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	auto Loaded = [&]()
	{
		bSeenPending |= L->PendingLoad.IsValid() || L->IsRebuildPending();
		return bSeenPending && !L->PendingLoad.IsValid() && bUp && ShotTime > Wait;
	};
	auto Elev = [&](const FCwWatch& W) -> const FOutlawsElevator*
	{
		for (const FOutlawsElevator& E : Inf->Elevators) if (E.IsSlope() == W.bSlope && (W.bSlope || E.Class == EOutlawsInfClass::MoveFloor) && E.Sectors.Contains(W.Sector)) return &E;
		return nullptr;
	};
	auto Range = [&](const FCwWatch& W, double& Lo, double& Hi)
	{
		const FOutlawsSector& S = Inf->Live.Sectors[W.Sector];
		Lo = TNumericLimits<double>::Max();
		Hi = -Lo;
		for (const FVector2D& V : S.Verts)
		{
			const double Y = Inf->Live.SurfaceY(S, V, W.bFloor);
			Lo = FMath::Min(Lo, Y);
			Hi = FMath::Max(Hi, Y);
		}
	};
	auto AngleOf = [&](const FCwWatch& W) -> double
	{
		const FOutlawsSector& S = Inf->Live.Sectors[W.Sector];
		return (W.bFloor ? S.SlopeFloor : S.SlopeCeil).Angle;
	};
	// the level's own faces under (over) the spot: the first hit of this actor's meshes, in units (NoHit: none)
	static constexpr double NoHit = -1.0e9;
	auto Trace = [&](const FCwWatch& W) -> double
	{
		const FOutlawsSector& S = Inf->Live.Sectors[W.Sector];
		const double Want = Inf->Live.SurfaceY(S, W.At, W.bFloor);
		// (from inside the room: half its height there, at most 20 units; CHOLE's shut hole is a quarter unit high)
		const double Gap = FMath::Abs(Inf->Live.SurfaceY(S, W.At, !W.bFloor) - Want);
		const double From = Want + (W.bFloor ? 1.0 : -1.0) * FMath::Clamp(Gap * 0.5, 0.05, 20.0);
		const double To = W.bFloor ? Want - 20.0 : Want + 20.0;
		TArray<FHitResult> Hits;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OLCwBlast), true, this);
		GetWorld()->LineTraceMultiByObjectType(Hits, FOutlawsGeometry::ToUE(W.At, From), FOutlawsGeometry::ToUE(W.At, To), FCollisionObjectQueryParams(ECC_WorldStatic), Q);
		for (const FHitResult& H : Hits) if (H.GetActor() == L) return H.ImpactPoint.Z / U;
		return NoHit;
	};
	auto Report = [&](const TCHAR* When)
	{
		int32 Hit = 0;
		for (const FCwWatch& W : Watch)
		{
			double Lo, Hi;
			Range(W, Lo, Hi);
			const FOutlawsSector& S = Inf->Live.Sectors[W.Sector];
			const double Want = Inf->Live.SurfaceY(S, W.At, W.bFloor);
			const double Got = Trace(W);
			const FOutlawsElevator* E = Elev(W);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cwblast %s %s #%d %s %.2f..%.2f, angle %.0f (%.2f deg), at the far spot %.2f traced %s; elevator %s"), When, *W.Name, W.Sector,
				W.bFloor ? TEXT("floor") : TEXT("ceiling"), Lo, Hi, W.bSlope ? AngleOf(W) : 0.0, W.bSlope ? AngleOf(W) * 90.0 / 4096.0 : 0.0, Want,
				Got == NoHit ? TEXT("nothing") : *FString::Printf(TEXT("%.2f"), Got),
				E ? *FString::Printf(TEXT("%s stop %d target %d term %d value %.0f"), *E->Name, E->Stop, E->Target, E->bTerminated ? 1 : 0, E->Current) : TEXT("none"));
			if (FMath::Abs(Got - Want) < 0.3) ++Hit;
		}
		Check(Hit == Watch.Num() && Watch.Num() > 0, FString::Printf(TEXT("%s: %d of the %d surfaces traced where Live has them (within 0.3 units at the far spot)"), When, Hit, Watch.Num()));
	};
	auto Centroid = [](const FOutlawsSector& S)
	{
		FVector2D C = FVector2D::ZeroVector;
		for (const FVector2D& V : S.Verts) C += V;
		return C / double(FMath::Max(1, S.Verts.Num()));
	};
	auto InsidePoint = [&](const FOutlawsSector& S, FVector2D& Out) -> bool
	{
		const FVector2D C = Centroid(S);
		if (FOutlawsLevel::InsideWalls(S, C.X, C.Y)) { Out = C; return true; }
		for (const double F : { 0.1, 0.25, 0.5 })
		{
			for (const FVector2D& V : S.Verts)
			{
				const FVector2D P = V + (C - V) * F;
				if (FOutlawsLevel::InsideWalls(S, P.X, P.Y)) { Out = P; return true; }
			}
		}
		return false;
	};
	auto Add = [&](const FString& Name, int32 Sector, bool bFloor, bool bSlope)
	{
		FCwWatch& W = Watch.AddDefaulted_GetRef();
		W.Name = Name;
		W.Sector = Sector;
		W.bFloor = bFloor;
		W.bSlope = bSlope;
		const FOutlawsSector& S = Inf->Live.Sectors[Sector];
		const FVector2D C = Centroid(S);
		W.At = C;
		FVector2D Along, Normal, Origin;
		if (bSlope && Inf->Live.SlopeFrame(S, bFloor, Along, Normal, &Origin))
		{
			// the corner furthest from the hinge, drawn in towards the middle (a ceiling's less: the roof comes down to the
			// floor at its far corner)
			FVector2D Far = C;
			double Best = -1.0;
			for (const FVector2D& V : S.Verts)
			{
				const double D = FMath::Abs(FVector2D::DotProduct(V - Origin, Normal));
				if (D > Best) { Best = D; Far = V; }
			}
			for (const double F : { bFloor ? 0.8 : 0.5, 0.5, 0.25 })
			{
				const FVector2D P = C + (Far - C) * F;
				if (FOutlawsLevel::InsideWalls(S, P.X, P.Y)) { W.At = P; break; }
			}
		}
		W.StartAt = Inf->Live.SurfaceY(S, W.At, bFloor);
	};
	auto Walk = [&](const TCHAR* Name)
	{
		TArray<int32> Secs;
		Inf->FindSectors(Name, FString(), Secs);
		FVector2D P;
		if (Secs.Num() == 0 || !InsidePoint(Inf->Live.Sectors[Secs[0]], P)) { Check(false, FString::Printf(TEXT("no spot inside %s"), Name)); return; }
		const FOutlawsSector& S = Inf->Live.Sectors[Secs[0]];
		TeleportTo(FOutlawsGeometry::ToUE(P, Inf->Live.SurfaceY(S, P, true)) + FVector(0.0, 0.0, 100.0), StartRot, false, true);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cwblast into %s (#%d) at %.1f %.1f"), Name, Secs[0], P.X, P.Y);
	};
	auto Look = [&](const FVector& Feet, const FVector& At)
	{
		const FRotator R = (At - (Feet + FVector(0.0, 0.0, 78.0))).Rotation();
		TeleportTo(Feet, FRotator(0.0, R.Yaw, 0.0), false, true);
		if (Controller) Controller->SetControlRotation(FRotator(R.Pitch, R.Yaw, 0.0));
	};
	auto Mid = [&](const FString& Name, double UpUnits) -> FVector
	{
		TArray<int32> Secs;
		Inf->FindSectors(Name, FString(), Secs);
		if (Secs.Num() == 0) return FVector::ZeroVector;
		const FOutlawsSector& S = Inf->Live.Sectors[Secs[0]];
		return FOutlawsGeometry::ToUE(Centroid(S), S.Floor.Y + UpUnits);
	};
	// a spot Units away from At towards Towards (plan), on the floor there, looking at At
	auto LookFrom = [&](const FVector& At, const FVector& Towards, double Units)
	{
		FVector Feet = At + (FVector(Towards.X, Towards.Y, At.Z) - At).GetSafeNormal() * (Units * U);
		const FOutlawsSector* Under = Inf->Live.SectorAt(FOutlawsGeometry::ToOutlaws(FVector(Feet.X, Feet.Y, At.Z)));
		Feet.Z = (Under ? Under->Floor.Y * U : At.Z) + 92.0;
		Look(Feet, At);
	};
	auto Shot = [&](const FString& What)
	{
		const FString Name = FString::Printf(TEXT("cwblast_%s_%s"), Tag, *What);
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cwblast shot %s.png"), *Name);
	};
	auto SlopesSettled = [&](bool bLast, FString& Out) -> int32
	{
		// each slope elevator at its last stop (ended) or its start, its master's angle the stop's
		int32 Ok = 0;
		for (int32 e = FMath::Max(0, Inf->SlopesFrom); Inf->SlopesFrom >= 0 && e < Inf->Elevators.Num(); ++e)
		{
			const FOutlawsElevator& E = Inf->Elevators[e];
			const FOutlawsInfClass& C = Inf->Inf.Items[E.Item].Classes[E.ClassIndex];
			const int32 Want = bLast ? C.Stops.Num() - 1 : FMath::Clamp(C.StartStop, 0, C.Stops.Num() - 1);
			const FOutlawsSector& S = Inf->Live.Sectors[E.Sectors[0]];
			const double A = (E.Class == EOutlawsInfClass::SlopeFloor ? S.SlopeFloor : S.SlopeCeil).Angle;
			const bool bOk = E.Stop == Want && E.Target < 0 && FMath::Abs(A - Inf->StopValue(E, Want)) < 1e-6 && (!bLast || E.bTerminated);
			Ok += bOk ? 1 : 0;
			Out += FString::Printf(TEXT(" %s:%d/%.0f%s"), *E.Name, E.Stop, A, bOk ? TEXT("") : TEXT("(off)"));
		}
		return Ok;
	};
	auto Moved = [&](FString& Which) -> int32
	{
		int32 N = 0;
		for (const FCwWatch& W : Watch)
		{
			if (!W.bSlope) continue;
			const double Now = Inf->Live.SurfaceY(Inf->Live.Sectors[W.Sector], W.At, W.bFloor);
			N += FMath::Abs(Now - W.StartAt) > 1.0 ? 1 : 0;
			Which += FString::Printf(TEXT(" %s %.2f->%.2f"), *W.Name, W.StartAt, Now);
		}
		return N;
	};
	const FString Dir = FPaths::ProjectSavedDir() / TEXT("OutlawsSaves");
	static const FVector Snap1(-31691.188, -18702.372, 92.0);
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 4.f || !bUp) return;
		L->bInvincible = true;
		Start = GetActorLocation();
		StartRot = GetActorRotation();
		bCivil = L->LevelName.Equals(TEXT("civlwar1"), ESearchCase::IgnoreCase);
		SlopeN = Inf->SlopesFrom >= 0 ? Inf->Elevators.Num() - Inf->SlopesFrom : 0;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cwblast %s: %s; slope elevators from %d"), *L->LevelName, *Inf->Summary(), Inf->SlopesFrom);
		// the sectors watched: civlwar1's by name (with CHOLE's MOVE_FLOOR), else every slope elevator's master and slaves
		Watch.Reset();
		if (bCivil)
		{
			static const TCHAR* const Names[] = { TEXT("WALLA1"), TEXT("WALLA2"), TEXT("BCEIL1"), TEXT("BCEIL2"), TEXT("BCEIL3"), TEXT("DCRAT1"), TEXT("DCRAT2"), TEXT("DCRAT3"), TEXT("FCRASH"), TEXT("CHOLE1"), TEXT("CHOLE2") };
			for (const TCHAR* N : Names)
			{
				TArray<int32> Secs;
				Inf->FindSectors(N, FString(), Secs);
				const FString Nm(N);
				if (Secs.Num() > 0) Add(Nm, Secs[0], !Nm.Equals(TEXT("BCEIL1")), !Nm.StartsWith(TEXT("CHOLE")));
			}
			Check(Watch.Num() == UE_ARRAY_COUNT(Names), FString::Printf(TEXT("%d of the %d watched sectors found"), Watch.Num(), int32(UE_ARRAY_COUNT(Names))));
			if (bOld) Check(Inf->SlopesFrom < 0, FString::Printf(TEXT("-OLOldCwBlast: no slope elevator made (%d), as before"), SlopeN));
			else Check(SlopeN == 9, FString::Printf(TEXT("%d slope elevators made from %d of %d (wanted the 9: WALLA1-2, BCEIL1-3, DCRAT1-3, FCRASH)"), SlopeN, Inf->SlopesFrom, Inf->Elevators.Num()));
		}
		else
		{
			for (int32 e = FMath::Max(0, Inf->SlopesFrom); Inf->SlopesFrom >= 0 && e < Inf->Elevators.Num(); ++e)
			{
				const FOutlawsElevator& E = Inf->Elevators[e];
				for (int32 k = 0; k < E.Sectors.Num(); ++k)
				{
					const FOutlawsSector& S = Inf->Live.Sectors[E.Sectors[k]];
					Add(k == 0 ? E.Name : FString::Printf(TEXT("%s(slave %s)"), *E.Name, *S.Name), E.Sectors[k], E.Class == EOutlawsInfClass::SlopeFloor, true);
				}
			}
			if (bOld) Check(Inf->SlopesFrom < 0, FString::Printf(TEXT("-OLOldCwBlast: no slope elevator made on %s, as before"), *L->LevelName));
			else Check(SlopeN > 0, FString::Printf(TEXT("%s: %d slope elevators made (%d sectors)"), *L->LevelName, SlopeN, Watch.Num()));
		}
		if (Watch.Num() > 0) Report(TEXT("start"));
		if (!bOld)
		{
			FString S;
			const int32 Ok = SlopesSettled(false, S);
			Check(Ok == SlopeN, FString::Printf(TEXT("at the start every slope at its stop 0 (civlwar1's BCEIL3 laid flat from its authored -883): %d of %d:%s"), Ok, SlopeN, *S));
		}
		Trig = 0;
		Wait = ShotTime + 0.2f;
		ShotStep = bCivil ? 1 : 30;
		return;
	}
	case 1:
	{
		// the triggers, a moment in each: the blasts and their sector changes
		if (ShotTime < Wait) return;
		static const TCHAR* const Triggers[] = { TEXT("ATRIG"), TEXT("BTRIG"), TEXT("CTRIG"), TEXT("FTRIG") };
		if (Trig < UE_ARRAY_COUNT(Triggers))
		{
			Walk(Triggers[Trig++]);
			Wait = ShotTime + 0.5f;
			return;
		}
		// DTRIG last, at snapshot 1's place (sector 516): DTRIGDUMMY's three steps, 0.125 s and 0.5 s apart
		TeleportTo(Snap1, FRotator(0.0, 85.18, 0.0), false, true);
		if (Controller) Controller->SetControlRotation(FRotator(2.48, 85.18, 0.0));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cwblast into DTRIG at snapshot 1's place"));
		Wait = ShotTime + 3.f;
		ShotStep = 2;
		return;
	}
	case 2:
	{
		if (ShotTime < Wait || !bUp) return;
		Report(TEXT("after"));
		FString Which;
		const int32 N = Moved(Which);
		if (bOld) Check(N == 0, FString::Printf(TEXT("as before, the fence, roof, craters and FCRASH stay as they were: %d moved:%s"), N, *Which));
		else
		{
			Check(N == 9, FString::Printf(TEXT("the fence, roof, craters and FCRASH tilted, each more than a unit at its far spot: %d of 9:%s"), N, *Which));
			FString S;
			const int32 Ok = SlopesSettled(true, S);
			Check(Ok == SlopeN, FString::Printf(TEXT("every slope elevator ended at its last stop with its master's angle there: %d of %d:%s"), Ok, SlopeN, *S));
		}
		int32 Holes = 0;
		for (const FCwWatch& W : Watch) if (!W.bSlope) Holes += FMath::Abs(Inf->Live.Sectors[W.Sector].Floor.Y) < 1e-3 && W.StartAt > 11.0 ? 1 : 0;
		Check(Holes == 2, FString::Printf(TEXT("CHOLE1 and CHOLE2 (MOVE_FLOOR, worked before too) opened from 11.75 to 0: %d of 2"), Holes));
		// snapshot 1's pose again (a blast may have nudged the body)
		TeleportTo(Snap1, FRotator(0.0, 85.18, 0.0), false, true);
		if (Controller) Controller->SetControlRotation(FRotator(2.48, 85.18, 0.0));
		Wait = ShotTime + 0.4f;
		ShotStep = 3;
		return;
	}
	case 3:
		if (ShotTime < Wait) return;
		Shot(TEXT("trench"));
		Wait = ShotTime + 1.f;
		ShotStep = 4;
		return;
	case 4:
	{
		if (ShotTime < Wait) return;
		// snapshot 3's place, turned to the fence (WALLA1 and WALLA2)
		const FVector At = (Mid(TEXT("WALLA1"), 4.0) + Mid(TEXT("WALLA2"), 4.0)) * 0.5;
		Look(FVector(-32926.392, -16893.914, 92.0), FVector(At.X, At.Y, 4.0 * U));
		Wait = ShotTime + 0.6f;
		ShotStep = 5;
		return;
	}
	case 5:
		if (ShotTime < Wait) return;
		Shot(TEXT("fence"));
		Wait = ShotTime + 1.f;
		ShotStep = 6;
		return;
	case 6:
		// 15 m from the roof (BCEIL1) towards snapshot 4's place, looking at it
		if (ShotTime < Wait) return;
		LookFrom(Mid(TEXT("BCEIL1"), 8.0), FVector(-32944.971, -18241.558, 0.0), 50.0);
		Wait = ShotTime + 0.8f;
		ShotStep = 7;
		return;
	case 7:
		if (ShotTime < Wait) return;
		Shot(TEXT("roof"));
		Wait = ShotTime + 1.f;
		ShotStep = bOld ? 20 : 8;
		return;
	case 8:
		// back to the start (no trigger there), then saved: slot 7 as it is, slot 9 cut at the first slope elevator
		if (ShotTime < Wait) return;
		TeleportTo(Start, StartRot, false, true);
		Wait = ShotTime + 0.8f;
		ShotStep = 9;
		return;
	case 9:
	{
		if (ShotTime < Wait || !bUp) return;
		SavedBefore.Reset();
		// (BUG-CWBLASTFLOOR, the lane's reruns 1 Nov: about 1 run in 2 failed the slot 9 check on elevator 50, LIGHT_ME1)  The
		// sector lights between the elevators and the slopes are saved and put back too, but a torch's flicker runs on by
		// itself and never rests, so the check leaves them out, as TestWorldPrint does since BUG-LIGHTSAVE
		const int32 Upto = Inf->LightsFrom >= 0 ? Inf->LightsFrom : Inf->SlopesFrom;
		for (int32 e = 0; e < Upto; ++e) SavedBefore.Add(Inf->Elevators[e].Current);
		SavedAngle.Reset();
		for (const FCwWatch& W : Watch) if (W.bSlope) SavedAngle.Add(W.Name, AngleOf(W));
		const bool bSaved = L->SaveGame(7);
		FString Text;
		TSharedPtr<FJsonObject> O;
		const TArray<TSharedPtr<FJsonValue>>* Es = nullptr;
		const TSharedPtr<FJsonObject>* In = nullptr;
		bool bCut = false;
		int32 Held = -1;
		if (bSaved && FFileHelper::LoadFileToString(Text, *(Dir / TEXT("slot7.json"))) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), O) && O.IsValid() &&
			O->TryGetObjectField(TEXT("inf"), In) && (*In)->TryGetArrayField(TEXT("elevators"), Es))
		{
			Held = Es->Num();
			TArray<TSharedPtr<FJsonValue>> Cut = *Es;
			Cut.SetNum(FMath::Min(Cut.Num(), Inf->SlopesFrom));
			TSharedPtr<FJsonObject> InCut = MakeShared<FJsonObject>(**In);
			InCut->SetArrayField(TEXT("elevators"), Cut);
			O->SetObjectField(TEXT("inf"), InCut);
			FString Out;
			bCut = FJsonSerializer::Serialize(O.ToSharedRef(), TJsonWriterFactory<>::Create(&Out)) && FFileHelper::SaveStringToFile(Out, *(Dir / TEXT("slot9.json")));
		}
		Check(bSaved && Held == Inf->Elevators.Num() && bCut, FString::Printf(TEXT("saved to slot 7 with %d elevators (%d now); slot 9 written cut at %d"), Held, Inf->Elevators.Num(), Inf->SlopesFrom));
		Check(L->LoadGame(7), TEXT("loading slot 7"));
		bSeenPending = false;
		Wait = ShotTime + 1.5f;
		ShotStep = 10;
		return;
	}
	case 10:
	{
		if (!Loaded()) return;
		int32 Same = 0;
		FString Diff;
		for (const FCwWatch& W : Watch)
		{
			const double* A = SavedAngle.Find(W.Name);
			if (!A) continue;
			if (FMath::Abs(AngleOf(W) - *A) < 1e-6) ++Same; else Diff += FString::Printf(TEXT(" %s %.0f (saved %.0f)"), *W.Name, AngleOf(W), *A);
		}
		Check(Same == SavedAngle.Num(), FString::Printf(TEXT("slot 7 loaded: %d of %d slopes at their saved angle%s"), Same, SavedAngle.Num(), *Diff));
		Report(TEXT("slot7"));
		Check(L->LoadGame(9), FString::Printf(TEXT("loading slot 9 (a save from before the slope elevators: %d elevators)"), Inf->SlopesFrom));
		bSeenPending = false;
		Wait = ShotTime + 1.5f;
		ShotStep = 11;
		return;
	}
	case 11:
	{
		if (!Loaded()) return;
		int32 Back = 0;
		for (int32 e = 0; e < SavedBefore.Num() && e < Inf->Elevators.Num(); ++e) Back += FMath::Abs(Inf->Elevators[e].Current - SavedBefore[e]) < 1e-3 ? 1 : 0;
		FString S;
		const int32 Ok = SlopesSettled(false, S);
		Check(Back == SavedBefore.Num() && Ok == SlopeN, FString::Printf(TEXT("slot 9 loaded: %d of the %d elevators before the sector lights put back, %d of %d slopes at their start:%s"), Back, SavedBefore.Num(), Ok, SlopeN, *S));
		Report(TEXT("slot9"));
		ShotStep = 20;
		return;
	}
	case 30:
	{
		// another level: every slope elevator sent to its last stop, as its trigger does
		if (ShotTime < Wait) return;
		if (SlopeN == 0) { ShotStep = 20; return; }
		FOutlawsInfRuntime* Run = const_cast<FOutlawsInfRuntime*>(Inf);
		for (int32 e = Inf->SlopesFrom; e < Inf->Elevators.Num(); ++e)
		{
			const FOutlawsElevator& E = Inf->Elevators[e];
			FOutlawsInfMessage M;
			M.Type = EOutlawsInfMsg::GotoStop;
			M.Arg = Inf->Inf.Items[E.Item].Classes[E.ClassIndex].Stops.Num() - 1;
			Run->Send(E.Name, FString(), M);
		}
		Wait = ShotTime + 1.5f;
		ShotStep = 31;
		return;
	}
	case 31:
	{
		if (ShotTime < Wait || !bUp) return;
		Report(TEXT("after"));
		FString Which, S;
		const int32 N = Moved(Which);
		Check(N == Watch.Num(), FString::Printf(TEXT("%s: every slope surface moved more than a unit at its far spot: %d of %d:%s"), *L->LevelName, N, Watch.Num(), *Which));
		const int32 Ok = SlopesSettled(true, S);
		Check(Ok == SlopeN, FString::Printf(TEXT("every slope elevator ended at its last stop with its master's angle there: %d of %d:%s"), Ok, SlopeN, *S));
		LookFrom(Mid(Watch[0].Name, 4.0), Start, 40.0);
		Wait = ShotTime + 0.8f;
		ShotStep = 32;
		return;
	}
	case 32:
		if (ShotTime < Wait) return;
		Shot(L->LevelName.ToLower());
		Wait = ShotTime + 1.f;
		ShotStep = 20;
		return;
	case 20:
		if (ShotTime < Wait) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cwblast RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		Wait = ShotTime + 1.f;
		ShotStep = 21;
		return;
	case 21:
		if (ShotTime < Wait) return;
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
