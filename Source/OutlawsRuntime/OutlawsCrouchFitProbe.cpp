// REQ-CROUCHFIT (user 2026-10-28, snapshot 19: a hideout secret behind an underwater gap he could not get low enough for):
//   -OLShots -OLLevel=hideout -OLNoFrontEnd -OLCrouchFitProbe[=level+level] [-OLOldCrouchFit]
// Each level in turn (the ones the gap scan lists by default, out\CROUCHFIT\py\gapscan2.py), started as a historical
// mission.  (1) The scan again, from the level as built: every adjoined opening (a pair's adjoins joined end to end, the
// NOPASS ones left out) with its clear height (the lowest ceiling less the highest floor of the two, the roomiest of its
// quarter points), width and floor.  Listed: the original's crouched body (2.0 units) goes through it and ours before did
// not (land under 2.85 units, water under 5.85: the swimmer was the standing body).  Blocked: lower than our crouched
// body, at least 3 wide (the original's is shut too).  Narrow: under 2.55 wide between solid walls, room enough over it
// (shut to the original's 3-unit body; ours must not be let through by the crouch's narrowing).
// (2) Each, by the body itself: listed, the crouched body (the swim's crouched one in water) swept through it from 25 cm
// on the near side to 25 cm on the far, on its floor, and not too narrow there (NarrowAt): PASS when through; under
// -OLOldCrouchFit the crouch before (and the standing swimmer) must be stopped (the scan's premise).  Blocked: the crouched
// body stopped.  Narrow: NarrowAt says so for the crouched body there, and the 80 cm body is stopped.  A sector the INF
// moves (doors, lifts, the train) is not where the level file has it: a failure there is a SKIP.
// (3) The game's own tick: in the hideout the snapshot's tunnel (pool 258, through 308 to 378) swum along the bottom on the
// desktop and with the headset's body; a listed land crawl space walked through crouched, both ways too (the crouched eye's
// height logged); in townex BUG-BOXGAP's gap beside the SHOVE_ME box (room 493 into 73 at the door's north end, 2.5 units)
// walked at standing, crouched at three lines across it, and crouched with the headset's body: never through.
// Lines "OLSHOT crouchfit ..." a case and "OLSHOT crouchfit RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsLevel.h"
#include "OutlawsGeometry.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

namespace
{
	enum class ECrouchFitKind : uint8 { Listed, Blocked, Narrow };
	struct FCrouchFitGap
	{
		ECrouchFitKind Kind = ECrouchFitKind::Listed;
		int32 A = -1, B = -1;
		double H = 0.0, Floor = 0.0, Width = 0.0;
		FVector2D At = FVector2D::ZeroVector, In = FVector2D::ZeroVector;     // the roomiest point, the way from A into B (plan)
		bool bWet = false, bMoving = false;
	};
	struct FCrouchFitWalk
	{
		FString Name;
		FVector Start = FVector::ZeroVector;      // the body's centre
		double Yaw = 0.0;
		bool bVR = false, bCrouch = false;
		float Time = 5.f;
		int32 WantSector = -1;                    // reached (by the feet): through; -1: a shut case
		int32 ShutSector = -1;                    // never reached: shut
		double ShutX = 1e9;                       // (shut cases) the plan x the body must stay short of
		bool bSwim = false;
		TArray<FVector2D> Via;                    // (plan) headed for each in turn, the next once within a unit of it
	};
	const TCHAR* const CrouchFitLevels = TEXT("hideout+townex+canyon+civlwar1+civlwar2+cliff+granary+icecaves+mill+office+ranch+shgranry+shgulch+shwildns+town+train+villa+wharf");

	FString CrouchFitWhere(const FCrouchFitGap& G)
	{
		return FString::Printf(TEXT("%s %d-%d h %.2f u (%.0f cm) w %.1f at (%.1f,%.1f) floor %.2f%s"), G.bWet ? TEXT("water") : TEXT("land"), G.A, G.B, G.H, G.H * 30.0, G.Width,
			G.At.X, G.At.Y, G.Floor, G.bMoving ? TEXT(" (an INF sector)") : TEXT(""));
	}
}

OL_SHOT_MATCH(OLCrouchFitProbe) { return OLFlags::Has(TEXT("OLCrouchFitProbe")); }

OL_SHOT_TEST(OLCrouchFitProbe, 18)
{
	static TArray<FString> Levels;
	static int32 LevelI = 0, Passes = 0, Fails = 0, Skips = 0, WalkI = -1;
	static bool bArmed = false, bAsked = false;
	static float SettleT = 0.f, WalkT = 0.f;
	static TArray<FCrouchFitWalk> Walks;
	static int32 WalkFirstSector = -2, WalkReached = 0;
	static double WalkMaxX = -1e9;
	static float WalkMinHalf = 1e9f, WalkEyeUp = 0.f;
	static bool bWalkSqueezed = false;
	if (!L || ShotStep < 0) return;
	const bool bOld = OldCrouchFit();
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT crouchfit check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	if (!bArmed)
	{
		bArmed = true;
		FString List = CrouchFitLevels;
		FString Given;
		if (OLFlags::Value(TEXT("OLCrouchFitProbe"), Given) && !Given.IsEmpty()) List = Given;
		List.ParseIntoArray(Levels, TEXT("+"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT crouchfit on: %s (%s)"), *List, bOld ? TEXT("-OLOldCrouchFit: the crouch before") : TEXT("the crouch as built"));
	}
	UCharacterMovementComponent* M = GetCharacterMovement();
	UCapsuleComponent* Cap = GetCapsuleComponent();
	UWorld* World = GetWorld();
	if (!Levels.IsValidIndex(LevelI))
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT crouchfit RESULT %s (%d passed, %d failed, %d skipped)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails, Skips);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	}
	const FString Want = Levels[LevelI];
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	if (L->IsFrontEnd() || !L->LevelName.Equals(Want, ESearchCase::IgnoreCase))
	{
		if (!bAsked)
		{
			bAsked = true;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT crouchfit starting %s"), *Want);
			if (bMenuOpen) CloseGameMenu();
			L->StartMission(Want, EOutlawsDifficulty::Bad);
		}
		return;
	}
	if (!bUp) return;
	L->bInvincible = true;
	L->bEnemiesActive = false;
	L->StartSaveDelay = -1.f;             // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	Stamina = 100.f;
	Air = 1.f;
	// the desktop's body again (a walk case may have left the headset's on)
	auto DesktopBody = [&]()
	{
		TestAxes.Remove(TEXT("OLVR_RightY"));
		bBarrelLatched = false;
		bCrouchHeld = bAutoCrouched = bStandPending = bSwimSqueeze = false;
		CrouchAmount = 1.f;
		CrouchDrop = CrouchFull = 0.f;
		if (bVR)
		{
			bVR = false;
			Camera->SetRelativeLocation(FVector(0.0, 0.0, 160.0));
			ApplyModeSettings();
		}
		if (bIsCrouched) UnCrouch();
		Cap->SetCapsuleSize(RadiusFor(87.f), 87.f, false);
	};
	switch (ShotStep)
	{
	case 0:
	{
		SettleT += Dt;
		if (SettleT < 3.f) return;
		SettleT = 0.f;
		const FOutlawsLevel* Lv = L->LiveLevel();
		if (!Lv)
		{
			Check(false, FString::Printf(TEXT("%s: no level data"), *Want));
			++LevelI;
			bAsked = false;
			return;
		}
		const double U = FOutlawsGeometry::UnitsToCm;
		// (1) the scan, as gapscan2.py has it
		TSet<int32> Moving;
		if (const FOutlawsInfRuntime* Inf = L->DebugInf())
		{
			for (const FOutlawsElevator& E : Inf->Elevators) for (const int32 S : E.Sectors) Moving.Add(S);
		}
		// (and the automatic doors, and the whole train, whose cars the level carries along)
		const bool bTrain = L->LevelName.Equals(TEXT("train"), ESearchCase::IgnoreCase);
		for (const FOutlawsSector& S : Lv->Sectors)
		{
			if (bTrain || (S.Flags & (OutlawsFlags::SectorDoor | OutlawsFlags::SectorDoorReverse))) Moving.Add(S.Index);
		}
		TArray<FCrouchFitGap> Gaps;
		for (const FOutlawsSector& S : Lv->Sectors)
		{
			TMap<int32, TArray<int32>> By;
			for (int32 w = 0; w < S.Walls.Num(); ++w)
			{
				const FOutlawsWall& W = S.Walls[w];
				if (W.Adjoin <= S.Index || !Lv->Sectors.IsValidIndex(W.Adjoin) || (W.Flags1 & OutlawsFlags::WallNoPass)) continue;
				By.FindOrAdd(W.Adjoin).Add(w);
			}
			for (const TPair<int32, TArray<int32>>& P : By)
			{
				const FOutlawsSector& T = Lv->Sectors[P.Key];
				// joined end to end: a chain of the pair's walls (each wall starts where one ends)
				TArray<TArray<int32>> Chains;
				for (const int32 w : P.Value)
				{
					const FVector2D A = S.Verts[S.Walls[w].V1], B = S.Verts[S.Walls[w].V2];
					bool bPut = false;
					for (TArray<int32>& Ch : Chains)
					{
						if (S.Verts[S.Walls[Ch.Last()].V2].Equals(A, 1e-3)) { Ch.Add(w); bPut = true; break; }
						if (S.Verts[S.Walls[Ch[0]].V1].Equals(B, 1e-3)) { Ch.Insert(w, 0); bPut = true; break; }
					}
					if (!bPut) Chains.Add({ w });
				}
				for (const TArray<int32>& Ch : Chains)
				{
					FCrouchFitGap G;
					G.A = S.Index;
					G.B = T.Index;
					G.H = -1e9;
					for (const int32 w : Ch)
					{
						const FVector2D A = S.Verts[S.Walls[w].V1], B = S.Verts[S.Walls[w].V2];
						G.Width += FVector2D::Distance(A, B);
						for (const double t : { 0.25, 0.5, 0.75 })
						{
							const FVector2D Pt = A + (B - A) * t;
							const double F = FMath::Max(Lv->SurfaceY(S, Pt, true), Lv->SurfaceY(T, Pt, true));
							const double C = FMath::Min(Lv->SurfaceY(S, Pt, false), Lv->SurfaceY(T, Pt, false));
							if (C - F > G.H)
							{
								G.H = C - F;
								G.Floor = F;
								G.At = Pt;
								const FVector2D E = (B - A).GetSafeNormal();
								G.In = FVector2D(-E.Y, E.X);
								if (!Lv->ContainsXZ(T, Pt.X + G.In.X * 0.3, Pt.Y + G.In.Y * 0.3)) G.In = -G.In;
							}
						}
					}
					G.bWet = ((S.Flags | T.Flags) & OutlawsFlags::Water) != 0;
					G.bMoving = Moving.Contains(G.A) || Moving.Contains(G.B);
					const double Step = FMath::Abs(Lv->SurfaceY(S, G.At, true) - Lv->SurfaceY(T, G.At, true));
					if (G.H <= 0.05) continue;
					if (G.H >= 1.99 && G.Width >= 2.99 && (G.bWet ? G.H < 5.85 : (G.H < 2.85 && Step <= 3.0))) G.Kind = ECrouchFitKind::Listed;
					else if (G.H >= 1.0 && G.H < 1.92 && G.Width >= 2.99) G.Kind = ECrouchFitKind::Blocked;
					else if (G.H >= 2.0 && G.Width >= 1.0 && G.Width < 2.55)
					{
						// between solid walls: the near sector's walls at both ends of the chain have no adjoin
						const FVector2D Ca = S.Verts[S.Walls[Ch[0]].V1], Cb = S.Verts[S.Walls[Ch.Last()].V2];
						bool bSolidA = false, bSolidB = false;
						for (const FOutlawsWall& W : S.Walls)
						{
							if (W.Adjoin >= 0 && !(W.Flags1 & OutlawsFlags::WallNoPass)) continue;
							if (S.Verts[W.V2].Equals(Ca, 1e-3)) bSolidA = true;
							if (S.Verts[W.V1].Equals(Cb, 1e-3)) bSolidB = true;
						}
						if (!bSolidA || !bSolidB) continue;
						G.Kind = ECrouchFitKind::Narrow;
					}
					else continue;
					Gaps.Add(G);
				}
			}
		}
		int32 NListed = 0, NBlocked = 0, NNarrow = 0;
		for (const FCrouchFitGap& G : Gaps) { NListed += G.Kind == ECrouchFitKind::Listed; NBlocked += G.Kind == ECrouchFitKind::Blocked; NNarrow += G.Kind == ECrouchFitKind::Narrow; }
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT crouchfit %s scan: %d listed (the original through, ours before not), %d blocked (lower than the crouch), %d narrow (under 2.55 wide)"),
			*L->LevelName, NListed, NBlocked, NNarrow);
		// (2) by the body: sweeps of the crouched body (or the swim's), the 80 cm one for the narrow
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsCrouchFitProbe), false, this);
		FCollisionResponseParams RP;
		Cap->InitSweepCollisionParams(Q, RP);
		RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
		L->IgnorePassedOverlaps(Q);
		const ECollisionChannel Channel = Cap->GetCollisionObjectType();
		auto Through = [&](const FCrouchFitGap& G, float R, float Half, FString& Why)
		{
			const FOutlawsSector* Sa = Lv->Sector(G.A);
			const FOutlawsSector* Sb = Lv->Sector(G.B);
			// from either side, well in or just in (a thin sector beside the opening: a step, a strip); through from any one
			bool bThrough = false, bTried = false;
			for (const double Off0 : { R + 25.0, R + 4.0 })
			{
				for (const double Side : { 1.0, -1.0 })
				{
					const double Off = Off0 / U * Side;
					const FVector2D Pa = G.At - G.In * Off, Pb = G.At + G.In * Off;
					const FOutlawsSector* From = Side > 0.0 ? Sa : Sb;
					const FOutlawsSector* To = Side > 0.0 ? Sb : Sa;
					if (!Lv->ContainsXZ(*From, Pa.X, Pa.Y)) continue;
					const double F = FMath::Max3(G.Floor, Lv->SurfaceY(*From, Pa, true), Lv->SurfaceY(*To, Pb, true));
					// (at the floor, or a little over it where the opening has room: over a sill or a step's lip, as a walk steps)
					for (const double Lift : { 3.0, 12.0, 24.0 })
					{
						if (Lift > 3.0 && 2.0 * Half + Lift > G.H * U - 1.0) break;
						const FVector A = FOutlawsGeometry::ToUE(Pa, F) + FVector(0.0, 0.0, Half + Lift), B = FOutlawsGeometry::ToUE(Pb, F) + FVector(0.0, 0.0, Half + Lift);
						FHitResult H;
						bTried = true;
						if (World->SweepSingleByChannel(H, A, B, FQuat::Identity, Channel, FCollisionShape::MakeCapsule(R, Half), Q, RP))
						{
							Why = FString::Printf(TEXT("stopped %s by %s at %s"), H.bStartPenetrating ? TEXT("where it starts") : *FString::Printf(TEXT("%.0f cm on"), H.Distance),
								H.GetComponent() ? *H.GetComponent()->GetName() : TEXT("?"), *FVector(H.ImpactPoint).ToCompactString());
							continue;
						}
						bThrough = true;
						break;
					}
					if (bThrough) break;
				}
				if (bThrough) break;
			}
			if (!bTried) Why = TEXT("no spot beside it inside either sector");
			if (!bThrough) return false;
			const double F = G.Floor;
			if (R < 39.5f && R > 10.f && NarrowAt(FOutlawsGeometry::ToUE(G.At, F) + FVector(0.0, 0.0, Half + 3.0), Half, R, FVector(G.In.Y, G.In.X, 0.0)))
			{
				Why = TEXT("narrower there than the 80 cm body");
				return false;
			}
			Why = TEXT("through");
			return true;
		};
		const float Hc = CrouchFitHalf(), Rc = RadiusFor(Hc);
		int32 LP = 0, LF = 0, LS = 0;
		for (const FCrouchFitGap& G : Gaps)
		{
			FString Why;
			bool bOk = false;
			FString What;
			if (G.Kind == ECrouchFitKind::Listed)
			{
				// the crouch (the swim's crouch in water; before it, the standing swimmer)
				const float Half = G.bWet && bOld ? 87.f : Hc, R = RadiusFor(Half);
				const bool bThrough = Through(G, R, Half, Why);
				bOk = bOld ? !bThrough : bThrough;
				if (bOld)
				{
					// (the A/B: what the body before made of it, counted, not judged)
					UE_LOG(LogOutlaws, Display, TEXT("OLSHOT crouchfit before (%s): listed %s: the body before %s (%s)"), *L->LevelName, *CrouchFitWhere(G), bThrough ? TEXT("went through") : TEXT("was stopped"), *Why);
					continue;
				}
				// (stopped: whatever the body? A stick 10 cm thick, the opening's height less a few centimetres, through it the same
				// way: stopped too, it is shut by something else there in our level (a mid texture, a fence, an object, a door),
				// not by the body's size; no spot beside it to start from, the same)
				FString WhyStick;
				if (!bThrough && !bOld && (Why.Contains(TEXT("no spot")) || !Through(G, 5.f, FMath::Max(5.f, float(G.H * U) * 0.5f - 6.f), WhyStick)))
				{
					++Skips;
					++LS;
					UE_LOG(LogOutlaws, Display, TEXT("OLSHOT crouchfit SKIP SHUT (%s): listed %s: shut whatever the body (%s; a thin stick: %s)"), *L->LevelName, *CrouchFitWhere(G), *Why, *WhyStick);
					continue;
				}
				What = FString::Printf(TEXT("listed %s: %s %s (%s)"), *CrouchFitWhere(G), bOld ? TEXT("the body before") : G.bWet ? TEXT("the swim's crouch") : TEXT("the crouch"), bThrough ? TEXT("goes through") : TEXT("is stopped"), *Why);
			}
			else if (G.Kind == ECrouchFitKind::Blocked)
			{
				bOk = !Through(G, Rc, Hc, Why);
				What = FString::Printf(TEXT("blocked %s: the crouch %s (%s)"), *CrouchFitWhere(G), bOk ? TEXT("is stopped") : TEXT("goes through"), *Why);
			}
			else
			{
				FString Why80;
				const bool bWide = Through(G, 40.f, 87.f, Why80);
				const double F = G.Floor;
				FString WhyC;
				const bool bNarrow = bOld || NarrowAt(FOutlawsGeometry::ToUE(G.At, F) + FVector(0.0, 0.0, Hc + 3.0), Hc, Rc, FVector(G.In.Y, G.In.X, 0.0))
					|| !Through(G, Rc, Hc, WhyC);           // (or too narrow for the crouched body itself)
				bOk = !bWide && bNarrow;
				What = FString::Printf(TEXT("narrow %s: the 80 cm body %s (%s), the crouch %s"), *CrouchFitWhere(G), bWide ? TEXT("goes through") : TEXT("is stopped"), *Why80,
					bOld ? TEXT("is the 80 cm body") : bNarrow ? TEXT("held out (narrower than the 80 cm)") : TEXT("let in"));
			}
			if (!bOk && (G.bMoving || Why.Contains(TEXT("OutlawsDoor")) || Why.Contains(TEXT("OutlawsLift"))))     // (a mover in the way: where the INF has it now)
			{
				++Skips;
				++LS;
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT crouchfit SKIP (%s): %s"), *L->LevelName, *What);
				continue;
			}
			(bOk ? LP : LF) += 1;
			Check(bOk, FString::Printf(TEXT("%s %s"), *L->LevelName, *What));
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT crouchfit %s gaps: %d passed, %d failed, %d skipped"), *L->LevelName, LP, LF, LS);
		// (3) the game's own tick, where there are walk cases
		Walks.Reset();
		auto Centre = [&](const FVector2D& Plan, double FloorY, float Half) { return FOutlawsGeometry::ToUE(Plan, FloorY) + FVector(0.0, 0.0, Half + 2.0); };
		if (L->LevelName.Equals(TEXT("hideout"), ESearchCase::IgnoreCase))
		{
			// the snapshot's tunnel: from the pool's bottom 3 units west of its mouth, east along it, desktop then headset
			for (const bool bV : { false, true })
			{
				FCrouchFitWalk W;
				W.Name = FString::Printf(TEXT("the snapshot's tunnel 258 > 308 > 378 swum along the bottom (%s)"), bV ? TEXT("VR") : TEXT("desktop"));
				W.Start = Centre(FVector2D(466.0, 458.2), -40.0, 87.f);
				W.Yaw = 90.0;              // (Outlaws +x is the engine's +y)
				W.bVR = bV;
				W.bSwim = true;
				W.Time = 16.f;
				W.WantSector = 378;
				W.Via = { FVector2D(472.5, 459.5), FVector2D(476.5, 462.0), FVector2D(481.0, 457.5), FVector2D(487.0, 455.5) };     // (308 bends north, then south)
				Walks.Add(W);
			}
		}
		// a listed land crawl space here walked through crouched (the first through by the sweep, not an INF one, with
		// room on its near side), desktop and headset
		for (const FCrouchFitGap& G : Gaps)
		{
			if (G.Kind != ECrouchFitKind::Listed || G.bWet || G.bMoving || L->LevelName.Equals(TEXT("train"), ESearchCase::IgnoreCase)) continue;
			FString Why;
			if (!Through(G, Rc, Hc, Why)) continue;
			const FVector2D From = G.At - G.In * (90.0 / U);
			const FOutlawsSector* Sa = Lv->Sector(G.A);
			if (!Sa || !Lv->ContainsXZ(*Sa, From.X, From.Y)) continue;
			for (const bool bV : { false, true })
			{
				FCrouchFitWalk W;
				W.Name = FString::Printf(TEXT("the crawl space %d > %d (%.2f u) walked through crouched (%s)"), G.A, G.B, G.H, bV ? TEXT("VR") : TEXT("desktop"));
				W.Start = Centre(From, Lv->SurfaceY(*Sa, From, true), Hc);
				W.Yaw = FMath::RadiansToDegrees(FMath::Atan2(G.In.X, G.In.Y));      // (the engine's x is Outlaws z, y is x)
				W.bVR = bV;
				W.bCrouch = true;
				W.Time = 6.f;
				W.WantSector = G.B;
				Walks.Add(W);
			}
			break;
		}
		if (L->LevelName.Equals(TEXT("townex"), ESearchCase::IgnoreCase))
		{
			// BUG-BOXGAP: room 493 east into 73 by the door's north end, past the SHOVE_ME box (its north face z 149.5, the jamb
			// z 152): standing, crouched at three lines across, crouched with the headset's body
			struct FLine { double Z; bool bCrouch; bool bVR; double Yaw; };
			for (const FLine& Li : { FLine{ 150.75, false, false, 90.0 }, FLine{ 150.75, true, false, 90.0 }, FLine{ 150.2, true, false, 80.0 }, FLine{ 151.3, true, false, 100.0 }, FLine{ 150.75, true, true, 90.0 } })
			{
				FCrouchFitWalk W;
				W.Name = FString::Printf(TEXT("BUG-BOXGAP's gap by the SHOVE_ME box, z %.2f yaw %.0f, %s (%s)"), Li.Z, Li.Yaw, Li.bCrouch ? TEXT("crouched") : TEXT("standing"), Li.bVR ? TEXT("VR") : TEXT("desktop"));
				W.Start = Centre(FVector2D(130.5, Li.Z), 0.25, Li.bCrouch ? Hc : 87.f);
				W.Yaw = Li.Yaw;
				W.bVR = Li.bVR;
				W.bCrouch = Li.bCrouch;
				W.Time = 4.f;
				W.ShutSector = 73;
				W.ShutX = 133.5 + 0.4;
				Walks.Add(W);
			}
		}
		WalkI = -1;
		ShotStep = 1;
		return;
	}
	case 1:
	{
		// a walk case: settled for a second (crouched where it is one), then the move held
		if (WalkI >= 0 && Walks.IsValidIndex(WalkI))
		{
			FCrouchFitWalk& W = Walks[WalkI];
			WalkT += Dt;
			if (W.bCrouch)
			{
				if (W.bVR) TestAxes.Add(TEXT("OLVR_RightY"), -1.f);
				else bCrouchHeld = true;
			}
			if (W.Via.Num() > 0)
			{
				const FVector2D Here = FOutlawsGeometry::ToOutlawsPlan(GetActorLocation());
				if (W.Via.Num() > 1 && FVector2D::Distance(Here, W.Via[0]) < 1.0) W.Via.RemoveAt(0);
				const FVector2D To = W.Via[0] - Here;
				W.Yaw = FMath::RadiansToDegrees(FMath::Atan2(To.X, To.Y));
				if (bVR) SetActorRotation(FRotator(0.0, W.Yaw, 0.0));
			}
			if (Controller) Controller->SetControlRotation(FRotator(W.bSwim ? -4.0 : 0.0, W.Yaw, 0.0));
			if (WalkT >= 1.f && WalkT - Dt < 1.f) WalkTestLeft = W.Time;
			const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, Cap->GetUnscaledCapsuleHalfHeight());
			const int32 Sec = L->SectorHoldingPoint(Feet + FVector(0.0, 0.0, 5.0), 15.0);
			if (WalkFirstSector == -2) WalkFirstSector = Sec;
			if (W.WantSector >= 0 && Sec == W.WantSector) WalkReached = 1;
			if (W.ShutSector >= 0 && Sec == W.ShutSector) WalkReached = 1;
			if (W.bSwim && Sec == 308 && !bWalkSqueezed) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT crouchfit walk: into 308 (the secret) at %.1f s"), WalkT);
			bWalkSqueezed |= bSwimSqueeze;
			WalkMaxX = FMath::Max(WalkMaxX, FOutlawsGeometry::ToOutlawsPlan(GetActorLocation()).X);
			WalkMinHalf = FMath::Min(WalkMinHalf, Cap->GetUnscaledCapsuleHalfHeight());
			if (WalkT > 0.9f && WalkT - Dt <= 0.9f) WalkEyeUp = float(Camera->GetComponentLocation().Z - Feet.Z);
			const bool bDone = WalkT >= W.Time + 1.f || (W.WantSector >= 0 && WalkReached && WalkT > 1.5f);
			if (!bDone) return;
			WalkTestLeft = 0.f;
			const FString Seen = FString::Printf(TEXT("from sector %d, ended in %d at %s after %.1f s, the body's half height down to %.0f (radius %.0f now), the eye %.0f cm over the feet at the start%s"),
				WalkFirstSector, Sec, *GetActorLocation().ToCompactString(), WalkT, WalkMinHalf, Cap->GetUnscaledCapsuleRadius(), WalkEyeUp, W.bSwim ? (bWalkSqueezed ? TEXT(", crouched to fit by the swim") : TEXT(", never crouched to fit")) : TEXT(""));
			if (W.WantSector >= 0)
			{
				const bool bThrough = WalkReached != 0;
				if (bOld)
				{
					UE_LOG(LogOutlaws, Display, TEXT("OLSHOT crouchfit walk (before, for the A/B) %s: %s; %s"), *W.Name, bThrough ? TEXT("through") : TEXT("not through"), *Seen);
				}
				else
				{
					Check(bThrough, FString::Printf(TEXT("walk %s: %s; %s"), *W.Name, bThrough ? TEXT("through") : TEXT("not through"), *Seen));
				}
				// the crouched eye: the original's 1.5 units over the feet, inside the body
				if (W.bCrouch && !bOld)
				{
					Check(WalkEyeUp > CrouchEyeHeight() - 6.f && WalkEyeUp < CrouchEyeHeight() + 6.f && WalkEyeUp < 2.f * CrouchFitHalf() - 6.f,
						FString::Printf(TEXT("walk %s: the crouched eye %.0f cm over the feet (wanted %.0f, under the body's top %.0f)"), *W.Name, WalkEyeUp, CrouchEyeHeight(), 2.f * CrouchFitHalf()));
				}
			}
			else
			{
				const bool bShut = !WalkReached && WalkMaxX < W.ShutX;
				Check(bShut, FString::Printf(TEXT("walk %s: %s (furthest plan x %.2f, the door at 133.5); %s"), *W.Name, bShut ? TEXT("shut") : TEXT("GOT THROUGH"), WalkMaxX, *Seen));
			}
		}
		// the next case: the desktop's body stood up first, then put at the start
		++WalkI;
		WalkT = 0.f;
		WalkFirstSector = -2;
		WalkReached = 0;
		WalkMaxX = -1e9;
		WalkMinHalf = 1e9f;
		WalkEyeUp = 0.f;
		bWalkSqueezed = false;
		WalkTestLeft = 0.f;
		bRunning = false;
		DesktopBody();
		if (!Walks.IsValidIndex(WalkI))
		{
			++LevelI;
			bAsked = false;
			ShotStep = 0;
			return;
		}
		FCrouchFitWalk& W = Walks[WalkI];
		if (W.bVR)
		{
			// the headset's body, as -OLClimbProbe stands it: the head 170 cm over the play-space floor (this case only)
			bVR = true;
			HeightOffset = 0.f;
			Camera->SetRelativeLocation(FVector(0.0, 0.0, 170.0));
			ApplyModeSettings();
		}
		if (W.bCrouch)
		{
			// crouched before it is put there (the start is the crouched body's): the key held, or the stick held down
			Cap->SetCapsuleSize(RadiusFor(CrouchFitHalf()), CrouchFitHalf(), false);
			bCrouchHeld = true;
			if (bVR)
			{
				TestAxes.Add(TEXT("OLVR_RightY"), -1.f);
				bBarrelLatched = true;
				CrouchAmount = 1.f;
				CrouchFull = CrouchDrop = FMath::Max(0.f, HeadHeight() - CrouchEyeHeight());
				ShownDrop = CrouchFull;
				ShownDropVel = 0.f;
			}
		}
		TeleportTo(W.Start, FRotator(0.0, W.Yaw, 0.0), false, true);
		if (Controller) Controller->SetControlRotation(FRotator(0.0, W.Yaw, 0.0));
		M->StopMovementImmediately();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT crouchfit walk %d: %s, from %s"), WalkI, *W.Name, *W.Start.ToCompactString());
		return;
	}
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
