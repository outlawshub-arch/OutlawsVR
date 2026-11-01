// BUG-TNTDAM (user 2026-11-01, Quest snapshot 12, a soft lock: on civlwar2 the big TNT picked up earlier could not be used at
// the cracked stone wall by the rail track over the water): -OLShots -OLLevel=civlwar2 -OLNoFrontEnd -OLTntDamProbe
// [-OLOldTntDam].  (0) every level's INF locks that ask for an item (OBJECT: n), each made live, a tool's (the hand) or the
// wheel's; (1) at the snapshot's spot on the track (sector 623 DAMBOOM) what a grip on the crack meets; the lock items given
// (CheatJackpot, as their pickups list them), the wheel offering the TNT; (2) the TNT used as the wheel uses it (UseItem):
// the DAMBOOM line (#1E44F) takes it, the TNT goes, its bundle (NDYNABLX) shows, the blasts, the dam (DAMDROP) drops from
// -13 to -26.5, its sides (DAMLEFT, DAMRIGHT: SLOPE_FLOOR, BUG-CWBLASTFLOOR) tilt down, the water (DAMWATER floor, DAMUWATER ceiling) rises from -40 to -27.5; (3) the tunnels off the lake (floor
// -27) open: from 13 units over the water to half a unit, and a scripted swim to the nearest mouth, the jump out over its
// edge and a walk in reaches it; (4) what the rest of the level rests on: civlwar2 has no level end of its own (no
// END_LEVEL or COMPLETE in its INF, no named outlaw); the tunnel past the mouth is rubble to its ceiling (TUNNELPOW, EVENT_MASK
// 64) until a stick of dynamite goes off in the mouth, then the walkable region past it is counted; (5) the cannon
// (CANBOOM, #17D78) takes its ball from the wheel the same way.  Shots tntdam_<new|old>_before.png and _after.png in
// Saved\OLShots.  Lines "OLSHOT tntdam check PASS|FAIL: ..." and "OLSHOT tntdam RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsEnemy.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsLab.h"
#include "OutlawsGeometry.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

extern bool GSwimStrokeTest;                // (OutlawsPlayer.cpp: the strokes run without a headset)

OL_SHOT_MATCH(OLTntDamProbe) { return OLFlags::Param(TEXT("OLTntDamProbe")); }

OL_SHOT_TEST(OLTntDamProbe, 1380)
{
	static int32 Passes = 0, Fails = 0, Mouth = -1, MouthWall = -1, Water = -1, UWater = -1, Dam = -1, Boom = -1;
	static float Wait = 0.f, UseAt = 0.f, LastLog = 0.f, JumpAt = -10.f, DamAt = -1.f, WaterAt = -1.f, In = -1.f;
	static double Dam0 = 0.0, Water0 = 0.0, UWater0 = 0.0, Left0 = 0.0, Right0 = 0.0;
	static bool bUsed = false, bBundle = false, bBlasts = false;
	static TArray<int32> Mouths;
	static TArray<double> MouthAbove0;
	static FVector2D MouthAt = FVector2D::ZeroVector, MouthIn = FVector2D::ZeroVector;
	static FString Path;
	if (!L || ShotStep < 0) return;
	const bool bOld = OLFlags::Param(TEXT("OLOldTntDam"));
	const TCHAR* const Tag = bOld ? TEXT("old") : TEXT("new");
	const FOutlawsInfRuntime* Inf = L->DebugInf();
	const double U = FOutlawsGeometry::UnitsToCm;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT tntdam check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	auto Finish = [&]()
	{
		GSwimStrokeTest = false;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT tntdam RESULT %s (%d passed, %d failed, %s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			bOld ? TEXT("-OLOldTntDam") : TEXT("new"));
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	auto SectorNamed = [Inf](const TCHAR* Name) { TArray<int32> S; Inf->FindSectors(Name, FString(), S); return S.Num() ? S[0] : -1; };
	auto Floor = [Inf](int32 s) { return Inf->Live.Sectors.IsValidIndex(s) ? Inf->Live.Sectors[s].Floor.Y : 0.0; };
	auto Ceil = [Inf](int32 s) { return Inf->Live.Sectors.IsValidIndex(s) ? Inf->Live.Sectors[s].Ceil.Y : 0.0; };
	// (BUG-CWBLASTFLOOR's slopes) a sloped floor's lowest corner: the dam's sides tilt down after the blast
	auto Lowest = [Inf](int32 s)
	{
		if (!Inf->Live.Sectors.IsValidIndex(s)) return 0.0;
		const FOutlawsSector& S = Inf->Live.Sectors[s];
		double Lo = TNumericLimits<double>::Max();
		for (const FVector2D& V : S.Verts) Lo = FMath::Min(Lo, Inf->Live.SurfaceY(S, V, true));
		return Lo;
	};
	auto Gen = [L](const TCHAR* Id) { const TWeakObjectPtr<AOutlawsSpriteActor>* G = L->CellGens.Find(Id); return G && G->IsValid() && !(*G)->IsHidden(); };
	auto PutAt = [&](const FVector& Feet, double Yaw, double Pitch)
	{
		TeleportTo(Feet + FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0), FRotator(0.0, Yaw, 0.0), false, true);
		GetCharacterMovement()->Velocity = FVector::ZeroVector;
		if (Controller) Controller->SetControlRotation(FRotator(Pitch, Yaw, 0.0));
	};
	auto Shot = [&](const TCHAR* When)
	{
		const FString Name = FString::Printf(TEXT("tntdam_%s_%s"), Tag, When);
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT tntdam shot %s.png"), *Name);
	};
	// the snapshot's place: UE (-2040.7, 11255.1) on the track, the crack to the right of the view (look yaw -64)
	auto ToSnapshot = [&]() { PutAt(FVector(-2040.685, 11255.092, Floor(Boom) * U), -64.03, -1.1); };
	// a wall's lock: its midpoint and a standing spot d units in front of it (inside its sector's side), the yaw to face it
	auto WallSpot = [Inf](int32 s, int32 w, FVector& OutFeet, double& OutYaw, FVector2D& OutMid) -> bool
	{
		const FOutlawsLevel& Lv = Inf->Live;
		if (!Lv.Sectors.IsValidIndex(s) || !Lv.Sectors[s].Walls.IsValidIndex(w)) return false;
		const FOutlawsSector& S = Lv.Sectors[s];
		const FVector2D A = S.Verts[S.Walls[w].V1], B = S.Verts[S.Walls[w].V2];
		OutMid = (A + B) * 0.5;
		FVector2D N = FVector2D(B.Y - A.Y, A.X - B.X).GetSafeNormal();
		if (!Lv.ContainsXZ(S, OutMid.X + N.X * 0.3, OutMid.Y + N.Y * 0.3)) N = -N;
		for (const double D : { 4.0, 3.0, 2.0, 1.2 })
		{
			const FVector2D P = OutMid + N * D;
			const FOutlawsSector* At = Lv.SectorAt(P.X, S.Floor.Y + 3.0, P.Y);
			if (!At || At->Ceil.Y - At->Floor.Y < 6.0) continue;
			OutFeet = FOutlawsGeometry::ToUE(P, At->Floor.Y);
			const FVector2D To = OutMid - P;
			OutYaw = FMath::RadiansToDegrees(FMath::Atan2(To.X, To.Y));      // (UE X is Outlaws z, UE Y is Outlaws x)
			return true;
		}
		return false;
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 4.f || !bUp || !Inf) return;
		L->bInvincible = true;
		for (const TWeakObjectPtr<AOutlawsEnemy>& E : L->GetEnemies()) if (E.IsValid() && !E->IsDead() && !E->IsDormant()) E->SetDormant(true);
		// (0) every level's item locks: an INF class asking for an item (OBJECT: n) made live as a trigger or an elevator
		static const TCHAR* const Levels[] = { TEXT("canyon"), TEXT("civlwar1"), TEXT("civlwar2"), TEXT("civpow2"), TEXT("cliff"), TEXT("drygulch"), TEXT("fort"),
			TEXT("gallery"), TEXT("granary"), TEXT("hideout"), TEXT("htrain"), TEXT("icecaves"), TEXT("indy"), TEXT("mill"), TEXT("miner"), TEXT("office"), TEXT("ranch"),
			TEXT("shfort"), TEXT("shgranry"), TEXT("shgulch"), TEXT("shoot"), TEXT("shsimms"), TEXT("shwildns"), TEXT("simms"), TEXT("town"), TEXT("town_2p"),
			TEXT("townex"), TEXT("train"), TEXT("villa"), TEXT("wharf") };
		const FOutlawsAssets* A = L->GetAssets();
		int32 AllLocks = 0, AllLive = 0, WithLocks = 0;
		FString Missing;
		for (const TCHAR* Lvl : Levels)
		{
			FString Lvt, InfText;
			if (!A || !A->GetText(FString(Lvl) + TEXT(".lvt"), Lvt) || !A->GetText(FString(Lvl) + TEXT(".inf"), InfText)) continue;
			FOutlawsLevel Lv;
			Lv.ParseLvt(Lvt);
			FOutlawsInfRuntime R;
			R.Init(Lv, InfText);
			TSet<FString> Ids;
			TSet<FString> Lifts;            // a line lock on a lift's face (a grip there meets the lift's mesh)
			TSet<int32> LiftSecs;
			for (const FOutlawsElevator& E : R.Elevators) if (E.Class == EOutlawsInfClass::MoveFloor || E.Class == EOutlawsInfClass::MoveFC) for (int32 s : E.Sectors) LiftSecs.Add(s);
			int32 Locks = 0, Live = 0, Wheel = 0, Tools = 0;
			for (int32 i = 0; i < R.Inf.Items.Num(); ++i)
			{
				for (int32 c = 0; c < R.Inf.Items[i].Classes.Num(); ++c)
				{
					const FOutlawsInfClass& C = R.Inf.Items[i].Classes[c];
					if (C.KeyObject < 0) continue;
					++Locks;
					const bool bTool = AOutlawsLevelActor::IsToolUseId(C.KeyObject);
					(bTool ? Tools : Wheel) += 1;
					Ids.Add(FString::Printf(TEXT("%d%s"), C.KeyObject, bTool ? TEXT("t") : TEXT("")));
					bool bLive = false;
					for (const FOutlawsTrigger& T : R.Triggers)
					{
						if (T.Item != i || T.ClassIndex != c) continue;
						bLive = true;
						if (T.bLine && Lv.Sectors.IsValidIndex(T.Sector) && Lv.Sectors[T.Sector].Walls.IsValidIndex(T.Wall))
						{
							const FOutlawsWall& W = Lv.Sectors[T.Sector].Walls[T.Wall];
							if (LiftSecs.Contains(T.Sector) || LiftSecs.Contains(W.Adjoin)) Lifts.Add(FString::Printf(TEXT("%s #%s"), *R.Inf.Items[i].Name, *W.Id));
						}
					}
					for (const FOutlawsElevator& E : R.Elevators) bLive |= E.Item == i && E.ClassIndex == c;
					if (bLive) ++Live;
					else Missing += FString::Printf(TEXT(" %s:%s(class %d)"), Lvl, *R.Inf.Items[i].Name, c);
				}
			}
			if (!Locks) continue;
			++WithLocks;
			AllLocks += Locks;
			AllLive += Live;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT tntdam (0) %s: %d item locks, %d live; %d for wheel items, %d for tools (t, used in the hand); use ids %s; line locks on a lift's face: %s"),
				Lvl, Locks, Live, Wheel, Tools, *FString::Join(Ids.Array(), TEXT(",")), Lifts.Num() ? *FString::Join(Lifts.Array(), TEXT(", ")) : TEXT("none"));
		}
		Check(AllLocks > 0 && AllLive == AllLocks, FString::Printf(TEXT("(0) %d levels with item locks: %d locks, all live as triggers or elevators (%d)%s"), WithLocks, AllLocks, AllLive,
			Missing.IsEmpty() ? TEXT("") : *(TEXT("; not live:") + Missing)));
		Boom = SectorNamed(TEXT("DAMBOOM"));
		Dam = SectorNamed(TEXT("DAMDROP"));
		Water = SectorNamed(TEXT("DAMWATER"));
		UWater = SectorNamed(TEXT("DAMUWATER"));
		if (!L->LevelName.Equals(TEXT("civlwar2"), ESearchCase::IgnoreCase) || Boom < 0 || Dam < 0 || Water < 0 || UWater < 0)
		{
			Check(false, FString::Printf(TEXT("civlwar2's dam sectors (DAMBOOM %d, DAMDROP %d, DAMWATER %d, DAMUWATER %d) on %s"), Boom, Dam, Water, UWater, *L->LevelName));
			Finish();
			return;
		}
		ToSnapshot();
		Wait = ShotTime + 1.f;
		ShotStep = 1;
		return;
	}
	case 1:
	{
		if (ShotTime < Wait) return;
		// (1) the cause: a grip on the crack (the line DAMBOOM, #1E44F, on the step face of the dam over the track)
		const FOutlawsSector& S = Inf->Live.Sectors[Boom];
		int32 W = -1;
		for (int32 w = 0; w < S.Walls.Num(); ++w) if (S.Walls[w].Id.Equals(TEXT("1E44F"), ESearchCase::IgnoreCase)) W = w;
		const FVector Eye = Camera->GetComponentLocation();
		if (W >= 0)
		{
			const FVector2D A = S.Verts[S.Walls[W].V1], B = S.Verts[S.Walls[W].V2], E = FOutlawsGeometry::ToOutlawsPlan(Eye);
			const double T = FMath::Clamp(FVector2D::DotProduct(E - A, B - A) / FMath::Max(1e-9, (B - A).SizeSquared()), 0.0, 1.0);
			const FVector At = FOutlawsGeometry::ToUE(A + (B - A) * T, -20.0);
			const FVector Hand = At - (At - Eye).GetSafeNormal() * 5.0;
			FVector Spot, N;
			const bool bSpot = L->HandUseSpot(Eye, Hand, Spot, N);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT tntdam (1) the player in sector %d %s, the eyes %.0f cm from the crack; a grip there meets: %s; the glow %d"),
				L->PlayerSectorIndex(), *S.Name, FVector::Dist(Eye, At), *L->DebugDoorProbe(Eye, Hand), bSpot ? 1 : 0);
		}
		Check(W >= 0 && L->PlayerSectorIndex() == Boom, FString::Printf(TEXT("(1) the snapshot's spot is in sector %d (DAMBOOM %d), the crack's wall #1E44F found (%d)"), L->PlayerSectorIndex(), Boom, W));
		// the lock items given as their pickups list them; the wheel offers the TNT
		const int32 Given = L->CheatJackpot();
		TArray<FString> Items, Labels;
		L->GetWheelItems(Items, Labels);
		const int32 At = Items.IndexOfByKey(TEXT("idynable"));
		Check(At != INDEX_NONE, FString::Printf(TEXT("(1) %d lock items given; the wheel offers the TNT: %s"), Given, At != INDEX_NONE ? *FString::Printf(TEXT("\"%s\""), *Labels[At]) : TEXT("no")));
		Dam0 = Floor(Dam);
		Water0 = Floor(Water);
		UWater0 = Ceil(UWater);
		Left0 = Lowest(SectorNamed(TEXT("DAMLEFT")));
		Right0 = Lowest(SectorNamed(TEXT("DAMRIGHT")));
		// the tunnels off the lake: the sectors past its walls whose floor is above the water now and not above its set height
		const FOutlawsSector& Ws = Inf->Live.Sectors[Water];
		Mouths.Reset();
		MouthAbove0.Reset();
		for (int32 w = 0; w < Ws.Walls.Num(); ++w)
		{
			const int32 Adj = Ws.Walls[w].Adjoin;
			if (Adj < 0 || Mouths.Contains(Adj) || Floor(Adj) < Water0 + 3.0 || Floor(Adj) > -27.5 + 0.75 || Ceil(Adj) - Floor(Adj) > 20.0) continue;   // (not the track)
			Mouths.Add(Adj);
			MouthAbove0.Add(Floor(Adj) - Water0);
			if (Mouth < 0) { Mouth = Adj; MouthWall = w; }
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT tntdam (1) before: the dam's top %.2f, the water %.2f (under it %.2f), the tunnel mouths %s"), Dam0, Water0, UWater0,
			*FString::JoinBy(Mouths, TEXT(","), [&](int32 s) { return FString::Printf(TEXT("%d floor %.1f ceiling %.1f"), s, Floor(s), Ceil(s)); }));
		Shot(TEXT("before"));
		Wait = ShotTime + 1.f;
		ShotStep = 2;
		return;
	}
	case 2:
	{
		if (ShotTime < Wait) return;
		// (2) the TNT used as the wheel's pick uses it
		bUsed = L->UseItem(TEXT("idynable"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT tntdam (2) the wheel's pick of the TNT: %d; %s"), bUsed ? 1 : 0, *L->LastItemUse);
		UseAt = LastLog = ShotTime;
		ShotStep = 3;
		return;
	}
	case 3:
	{
		const float Since = ShotTime - UseAt;
		bBundle |= Gen(TEXT("1AAEE"));
		bBlasts |= Gen(TEXT("1AAEB")) || Gen(TEXT("1AAEF")) || Gen(TEXT("1AAEC")) || Gen(TEXT("1AAED")) || Gen(TEXT("1AAF1"));
		if (DamAt < 0.f && FMath::Abs(Floor(Dam) - Dam0) > 0.1) DamAt = Since;
		if (WaterAt < 0.f && Floor(Water) >= -27.5 - 0.01) WaterAt = Since;
		if (ShotTime - LastLog >= 5.f)
		{
			LastLog = ShotTime;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT tntdam (2) %.0f s: the TNT's bundle %d, blasts %d, the dam's top %.2f, the water %.2f (under it %.2f)"),
				Since, bBundle ? 1 : 0, bBlasts ? 1 : 0, Floor(Dam), Floor(Water), Ceil(UWater));
		}
		if (Since < (bOld ? 8.f : 150.f) && WaterAt < 0.f) return;
		if (bOld)
		{
			Check(L->GetCount(TEXT("idynable")) == 1 && Floor(Dam) == Dam0 && Floor(Water) == Water0, FString::Printf(TEXT("(2) -OLOldTntDam: as reported, the pick only shows the name: the TNT kept (%d), the dam %.2f, the water %.2f"),
				L->GetCount(TEXT("idynable")), Floor(Dam), Floor(Water)));
			ToSnapshot();
			Wait = ShotTime + 1.f;
			ShotStep = 9;
			return;
		}
		TArray<FString> Items, Labels;
		L->GetWheelItems(Items, Labels);
		Check(bUsed && L->LastItemUse.Contains(TEXT("DAMBOOM")) && L->LastItemUse.EndsWith(TEXT("taken")), FString::Printf(TEXT("(2) the pick used it on the crack: %s"), *L->LastItemUse));
		Check(L->GetCount(TEXT("idynable")) == 0 && !Items.Contains(TEXT("idynable")), FString::Printf(TEXT("(2) the TNT went into the wall (OBJECT 120 589824): %d left, on the wheel %d"),
			L->GetCount(TEXT("idynable")), Items.Contains(TEXT("idynable")) ? 1 : 0));
		Check(bBundle && bBlasts, FString::Printf(TEXT("(2) the TNT's bundle shown at the crack (NDYNABLX) %d, the blasts (NBOOM..) %d"), bBundle ? 1 : 0, bBlasts ? 1 : 0));
		Check(FMath::Abs(Floor(Dam) + 26.5) < 0.01 && Dam0 > -13.5, FString::Printf(TEXT("(2) the wall blew: the dam's top from %.2f to %.2f (track level -26.5), %.1f s after the pick"), Dam0, Floor(Dam), DamAt));
		const double LeftLo = Lowest(SectorNamed(TEXT("DAMLEFT"))), RightLo = Lowest(SectorNamed(TEXT("DAMRIGHT")));
		// (the far corners drop most: -38.93 and -47.13 degrees about each side's hinge wall take them to about -26.5, the
		// sides' middles to about -25)
		Check(FMath::Abs(Left0 + 13.0) < 0.01 && FMath::Abs(Right0 + 13.0) < 0.01 && LeftLo < -24.0 && RightLo < -24.0,
			FString::Printf(TEXT("(2) the dam's sides tilted down (SLOPE_FLOOR, BUG-CWBLASTFLOOR): DAMLEFT's lowest corner %.2f to %.2f, DAMRIGHT's %.2f to %.2f"),
				Left0, LeftLo, Right0, RightLo));
		Check(FMath::Abs(Floor(Water) + 27.5) < 0.01 && FMath::Abs(Ceil(UWater) + 27.5) < 0.01 && Water0 < -39.9,
			FString::Printf(TEXT("(2) the water rose to its set height: from %.2f to %.2f (under it %.2f to %.2f), %.1f s after the pick"), Water0, Floor(Water), UWater0, Ceil(UWater), WaterAt));
		ToSnapshot();
		Wait = ShotTime + 1.f;
		ShotStep = 4;
		return;
	}
	case 4:
	{
		if (ShotTime < Wait) return;
		Shot(TEXT("after"));
		Wait = ShotTime + 1.f;           // (the shot is taken before the swim's teleport)
		ShotStep = 12;
		return;
	}
	case 12:
	{
		if (ShotTime < Wait) return;
		// (3) the tunnels: half a unit over the water now, thirteen before
		FString Each;
		bool bOk = Mouths.Num() > 0;
		for (int32 i = 0; i < Mouths.Num(); ++i)
		{
			const double Above = Floor(Mouths[i]) - Floor(Water);
			bOk &= Above >= 0.0 && Above <= 1.0 && MouthAbove0[i] >= 10.0;
			Each += FString::Printf(TEXT(" %d: %.1f units over the water before, %.1f now, %.1f high;"), Mouths[i], MouthAbove0[i], Above, Ceil(Mouths[i]) - Floor(Mouths[i]));
		}
		Check(bOk, FString::Printf(TEXT("(3) the tunnel mouths off the lake open:%s"), *Each));
		// a swim to the nearest mouth from 4 units out, the jump out over its edge, a walk in
		FVector Feet;
		double Yaw = 0.0;
		const FOutlawsSector& Ws = Inf->Live.Sectors[Water];
		const FVector2D A = Ws.Verts[Ws.Walls[MouthWall].V1], B = Ws.Verts[Ws.Walls[MouthWall].V2];
		MouthAt = (A + B) * 0.5;
		FVector2D N = FVector2D(B.Y - A.Y, A.X - B.X).GetSafeNormal();
		if (!Inf->Live.ContainsXZ(Ws, MouthAt.X + N.X * 0.3, MouthAt.Y + N.Y * 0.3)) N = -N;
		const FVector2D From = MouthAt + N * 4.0;
		MouthIn = -N;
		Yaw = FMath::RadiansToDegrees(FMath::Atan2(MouthAt.X - From.X, MouthAt.Y - From.Y));
		Feet = FOutlawsGeometry::ToUE(From, Floor(Water) - 4.0);
		PutAt(Feet, Yaw, -5.0);
		GSwimStrokeTest = true;
		Path = FString::Printf(TEXT("%d"), L->PlayerSectorIndex());
		In = -1.f;
		UseAt = ShotTime;
		ShotStep = 5;
		return;
	}
	case 5:
	{
		const float Since = ShotTime - UseAt;
		const FVector P = GetActorLocation();
		const FVector2D Pp = FOutlawsGeometry::ToOutlawsPlan(P);
		const FVector2D To = (MouthAt - Pp);
		const FVector2D Goal = MouthAt + MouthIn * 3.0 - Pp;              // (on into the tunnel, 3 units past its lip)
		const double Yaw = FMath::RadiansToDegrees(FMath::Atan2(Goal.X, Goal.Y));
		if (Controller) Controller->SetControlRotation(FRotator(bSwimming ? 5.0 : 0.0, Yaw, 0.0));
		const FVector Dir = FRotator(0.0, Yaw, 0.0).Vector();
		if (bSwimming) HandVelocity[0] = HandVelocity[1] = -Dir * 300.0;
		AddMovementInput(Dir, 1.f);
		if (bSwimming && To.Size() * U < 120.0 && ShotTime - JumpAt > 1.f) { JumpAt = ShotTime; DoJump(); }
		const int32 Now = L->PlayerSectorIndex();
		if (!Path.EndsWith(FString::Printf(TEXT(" %d"), Now)) && !Path.Equals(FString::FromInt(Now))) Path += FString::Printf(TEXT(" %d"), Now);
		if (In < 0.f && Mouths.Contains(Now)) In = Since;
		if (Since < 1.5f && In >= 0.f) return;           // (a moment on, to see it stays)
		if ((In < 0.f || Since < In + 1.5f) && Since < 20.f) return;
		GSwimStrokeTest = false;
		const double Past = FVector2D::DotProduct(Pp - MouthAt, MouthIn) * U;
		Check(In >= 0.f && Mouths.Contains(Now) && Past > 30.0, FString::Printf(TEXT("(3) the tunnel is reachable: a swim from 4 units out, the jump over the edge and a walk reached sector %d in %.1f s and stood %.0f cm past its lip (sectors on the way: %s; swimming %d)"),
			Mouth, In, Past, *Path.Left(200), bSwimming ? 1 : 0));
		ShotStep = 6;
		return;
	}
	case 6:
	{
		// (4) civlwar2's end: none of its own.  The exe ends a level on an INF END_LEVEL [0x1400d5df9], a named outlaw's
		// death [0x14008f300] or the end-level cheat; civlwar2's INF has neither END_LEVEL nor COMPLETE and its OBT no named
		// outlaw (every bad guy BGY_New).  So the run rests on what can be walked to past the tunnel
		FString InfText;
		const bool bInfEnd = L->GetAssets() && L->GetAssets()->GetText(TEXT("civlwar2.inf"), InfText) && (InfText.Contains(TEXT("END_LEVEL")) || InfText.Contains(TEXT("COMPLETE")));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT tntdam (4) civlwar2's own end: INF END_LEVEL or COMPLETE %d, a named outlaw %d (the boss slots' first %s); the level ending now %d"),
			bInfEnd ? 1 : 0, L->GetBoss() ? 1 : 0, L->GetBoss() ? *L->GetBoss()->GetName() : TEXT("none"), L->DebugLevelEnding() ? 1 : 0);
		Check(!bInfEnd && !L->GetBoss(), TEXT("(4) civlwar2 has no level end of its own (no INF END_LEVEL or COMPLETE, no named outlaw): the run rests on the way on being open"));
		// the tunnel past the mouth is filled to its ceiling (TUNNELPOW / TUNNELPOW2, MOVE_FLOOR -21 to -27 on EVENT_MASK 64): a
		// stick of dynamite thrown in opens it; one goes off in the mouth by that wall
		Dam = -1;
		const FOutlawsSector& Ms = Inf->Live.Sectors[Mouth];
		for (int32 w = 0; w < Ms.Walls.Num() && Dam < 0; ++w)
		{
			const int32 Adj = Ms.Walls[w].Adjoin;
			if (Adj < 0 || !Inf->Live.Sectors[Adj].Name.StartsWith(TEXT("TUNNELPOW"), ESearchCase::IgnoreCase)) continue;
			Dam = Adj;
			const FVector2D A = Ms.Verts[Ms.Walls[w].V1], B = Ms.Verts[Ms.Walls[w].V2], M = (A + B) * 0.5;
			FVector2D N = FVector2D(B.Y - A.Y, A.X - B.X).GetSafeNormal();
			if (!Inf->Live.ContainsXZ(Ms, M.X + N.X * 0.3, M.Y + N.Y * 0.3)) N = -N;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT tntdam (4) the tunnel past mouth %d: sector %d %s, floor %.1f ceiling %.1f; a stick goes off by it"), Mouth, Adj, *Inf->Live.Sectors[Adj].Name, Floor(Adj), Ceil(Adj));
			L->Explode(FOutlawsGeometry::ToUE(M + N * 1.0, Floor(Mouth) + 2.0));
		}
		Wait = ShotTime + 1.5f;
		ShotStep = 11;
		return;
	}
	case 11:
	{
		if (ShotTime < Wait) return;
		if (Dam >= 0)
		{
			TArray<int32> Pow;
			Inf->FindSectors(TEXT("TUNNELPOW"), FString(), Pow);
			{ TArray<int32> Pow2; Inf->FindSectors(TEXT("TUNNELPOW2"), FString(), Pow2); Pow.Append(Pow2); }
			// (the other tunnel's INF also asks to be opened by this one's blast, a STANDARD trigger sending it NEXT_STOP; logged)
			Check(FMath::Abs(Floor(Dam) + 27.0) < 0.01, FString::Printf(TEXT("(4) the blast opened the tunnel past the mouth: sector %d floor %.1f (every TUNNELPOW: %s)"), Dam, Floor(Dam),
				*FString::JoinBy(Pow, TEXT(", "), [&](int32 s) { return FString::Printf(TEXT("%d %s floor %.1f"), s, *Inf->Live.Sectors[s].Name, Floor(s)); })));
		}
		else Check(false, FString::Printf(TEXT("(4) a TUNNELPOW sector past mouth %d"), Mouth));
		// the region past the mouth: sectors joined by openings a body walks through (a step up of 2.5 units or less, 5 of room)
		const FOutlawsLevel& Lv = Inf->Live;
		TArray<int32> Open = { Mouth };
		TSet<int32> Seen = { Mouth };
		int32 WithItems = 0;
		for (int32 k = 0; k < Open.Num(); ++k)
		{
			const FOutlawsSector& S = Lv.Sectors[Open[k]];
			for (const FOutlawsWall& W : S.Walls)
			{
				const int32 Nx = W.Adjoin;
				if (Nx < 0 || Seen.Contains(Nx) || !Lv.Sectors.IsValidIndex(Nx)) continue;
				const FOutlawsSector& T = Lv.Sectors[Nx];
				if (T.Floor.Y > S.Floor.Y + 2.5 || FMath::Min(S.Ceil.Y, T.Ceil.Y) - FMath::Max(S.Floor.Y, T.Floor.Y) < 5.0) continue;
				Seen.Add(Nx);
				Open.Add(Nx);
			}
		}
		for (const FOutlawsObject& O : Lv.Objects) { const int32 Si = Lv.Sectors.IndexOfByPredicate([&O](const FOutlawsSector& S) { return S.Id.Equals(O.SectorId, ESearchCase::IgnoreCase); }); WithItems += Seen.Contains(Si) ? 1 : 0; }
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT tntdam (4) past the tunnel: %d of the level's %d sectors walkable from mouth %d, %d of its %d objects in them; sectors %s"),
			Seen.Num(), Lv.Sectors.Num(), Mouth, WithItems, Lv.Objects.Num(), *FString::JoinBy(Seen.Array(), TEXT(","), [](int32 s) { return FString::FromInt(s); }).Left(600));
		Check(Seen.Num() > Mouths.Num() + 2, FString::Printf(TEXT("(4) the way goes on past the opened tunnel: %d sectors walk on from mouth %d"), Seen.Num(), Mouth));
		// (5) the cannon: its ball from the wheel, at its line (CANBOOM, #17D78)
		int32 Cs = -1, Cw = -1;
		for (const FOutlawsTrigger& T : Inf->Triggers)
		{
			if (T.bLine && Inf->Inf.Items[T.Item].Classes[T.ClassIndex].KeyObject == 123) { Cs = T.Sector; Cw = T.Wall; }
		}
		FVector Feet;
		double Yaw = 0.0;
		FVector2D Mid;
		if (Cs < 0 || !WallSpot(Cs, Cw, Feet, Yaw, Mid))
		{
			Check(false, FString::Printf(TEXT("(5) the cannon's line (use id 123) and a spot before it: sector %d wall %d"), Cs, Cw));
			Wait = ShotTime;
			ShotStep = 9;
			return;
		}
		PutAt(Feet, Yaw, -10.0);
		Wait = ShotTime + 1.f;
		ShotStep = 7;
		return;
	}
	case 7:
	{
		if (ShotTime < Wait) return;
		bUsed = L->UseItem(TEXT("icanball"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT tntdam (5) the wheel's pick of the cannonball: %d; %s"), bUsed ? 1 : 0, *L->LastItemUse);
		Wait = ShotTime + 2.f;
		ShotStep = 8;
		return;
	}
	case 8:
	{
		if (ShotTime < Wait) return;
		int32 Fence = -1;
		for (int32 e = 0; e < Inf->Elevators.Num(); ++e) if (Inf->Elevators[e].Name.Equals(TEXT("BOOMFENCE"), ESearchCase::IgnoreCase)) Fence = e;
		const bool bMoved = Fence >= 0 && (Inf->Elevators[Fence].Stop > 0 || Inf->Elevators[Fence].Target > 0);
		Check(bUsed && L->LastItemUse.Contains(TEXT("CANBOOM")) && L->GetCount(TEXT("icanball")) == 0 && bMoved,
			FString::Printf(TEXT("(5) the cannon took its ball from the wheel: %s; balls left %d; BOOMFENCE at stop %d (target %d)"), *L->LastItemUse, L->GetCount(TEXT("icanball")),
				Fence >= 0 ? Inf->Elevators[Fence].Stop : -1, Fence >= 0 ? Inf->Elevators[Fence].Target : -1));
		Wait = ShotTime;
		ShotStep = 10;
		return;
	}
	case 9:
		if (ShotTime < Wait) return;
		Shot(TEXT("after"));
		Wait = ShotTime + 1.5f;
		ShotStep = 10;
		return;
	case 10:
		if (ShotTime < Wait) return;
		Finish();
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
