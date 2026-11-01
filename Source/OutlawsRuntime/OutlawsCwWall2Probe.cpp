// BUG-CWWALL2 (user 2026-11-01, round 9 Quest snapshots 3-6, a soft lock: on civlwar2, the dam blown, a lit stick lay by the
// cracked wall of the tunnel off the lake (sector 871) and the wall stayed): -OLShots -OLLevel=civlwar2 -OLNoFrontEnd
// -OLCwWall2Probe [-OLOldCwWall2].  The wall is the face of TUNNELPOW (227), rubble to its ceiling (MOVE_FLOOR -21 HOLD,
// -27 TERMINATE, EVENT_MASK 64); TUNNELPOW2 (242) is the same rubble's other face, off mouth 893, and each one's TRIGGER
// STANDARD (EVENT_MASK 64) sends the other NEXT_STOP.  (0) every stock level's sector items that a blast works (EVENT_MASK
// 64 on an elevator or a trigger), each run on its own copy of the level's INF with a blast as Explode sends it (the sector
// it goes off in 0x40 | 0x8000, every other blast-minded sector whose box is within pdynam's WALL_RADIUS 0x40) from a stick
// set down 1 unit and one come to rest 2.8 units out in front of it, on the floor of its roomiest neighbour; the line
// items (EVENT_MASK 64 on a LINE) and the ELEVATOR EXPLODE sectors are counted beside them.  (1) on civlwar2 the dam blown
// with the big TNT (as -OLTntDamProbe) and the water up; (2) the player at the snapshot's spot in 871 facing the crack
// (cwwall2_<new|old>_before.png), saved to slot 7; (3) a lit stick thrown at the crack at 635 cm/s (the snapshot's throw):
// 227 and 242 after it (cwwall2_<tag>_thrown.png); (4) slot 7 loaded, a lit stick set down at the foot of the crack, the
// same (cwwall2_<tag>_placed.png); (5) (the fix) a crouched walk from the snapshot's spot through the opened rubble to 893
// and on to the pit 643 (cwwall2_new_way.png), and what can be walked to, and climbed to, from there.  Lines "OLSHOT cwwall2
// check PASS|FAIL: ..." and "OLSHOT cwwall2 RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsEnemy.h"
#include "OutlawsLab.h"
#include "OutlawsFormats.h"
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

OL_SHOT_MATCH(OLCwWall2Probe) { return OLFlags::Param(TEXT("OLCwWall2Probe")); }

namespace
{
	/** The blast-minded sectors (a trigger or an elevator of theirs listens) */
	void BlastMinded(const FOutlawsInfRuntime& R, TSet<int32>& Out)
	{
		for (const TPair<int32, TArray<int32>>& Kv : R.TriggersBySector) Out.Add(Kv.Key);
		for (const TPair<int32, TArray<int32>>& Kv : R.ElevatorsBySector) Out.Add(Kv.Key);
	}

	/** Explode's INF events (OutlawsLevelActorShoot.cpp) for a blast at At (Outlaws x, y, z): the sector it goes off in
	 *  0x40 | 0x8000, every other blast-minded sector whose box (its corners from its floor to its ceiling now) lies within
	 *  WallR units 0x40.  The sectors told, in order. */
	TArray<int32> BlastSectors(const FOutlawsInfRuntime& R, const FOutlawsLevel& Authored, const FVector& At, double WallR, int32& OutOwn)
	{
		TArray<int32> Got;
		const FOutlawsSector* Own = Authored.SectorAt(At.X, At.Y + 0.5, At.Z);
		OutOwn = Own ? Own->Index : -1;
		TSet<int32> Minded;
		BlastMinded(R, Minded);
		for (const int32 s : Minded)
		{
			if (s == OutOwn || !R.Live.Sectors.IsValidIndex(s)) continue;
			const FOutlawsSector& S = R.Live.Sectors[s];
			FBox B(ForceInit);
			for (const FVector2D& V : S.Verts)
			{
				B += FVector(V.X, S.Floor.Y, V.Y);
				B += FVector(V.X, S.Ceil.Y, V.Y);
			}
			if (B.IsValid && B.ComputeSquaredDistanceToPoint(At) <= WallR * WallR) Got.Add(s);
		}
		return Got;
	}
}

OL_SHOT_TEST(OLCwWall2Probe, 1400)
{
	static int32 Passes = 0, Fails = 0, Boom = -1, Dam = -1, Water = -1, Mouth871 = -1, Pow = -1, Pow2 = -1, CrackWall = -1, Way = 0;
	static float Wait = 0.f, UseAt = 0.f, LastLog = 0.f, WalkAt = 0.f;
	static double Water0 = 0.0, WallR = 15.0;
	static bool bSeenPending = false, bStick = false;
	static FVector StickLast = FVector::ZeroVector;
	static FVector2D MovedFrom = FVector2D::ZeroVector;
	static float MovedAt = 0.f;
	static TArray<FVector2D> Waypoints;
	static FString WalkPath;
	if (!L || ShotStep < 0) return;
	const bool bOld = OLFlags::Param(TEXT("OLOldCwWall2"));
	const TCHAR* const Tag = bOld ? TEXT("old") : TEXT("new");
	const FOutlawsInfRuntime* Inf = L->DebugInf();
	const double U = FOutlawsGeometry::UnitsToCm;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cwwall2 check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	auto Finish = [&]()
	{
		SetCrouchHeld(false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cwwall2 RESULT %s (%d passed, %d failed, %s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			bOld ? TEXT("-OLOldCwWall2") : TEXT("new"));
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	auto SectorNamed = [Inf](const TCHAR* Name) { TArray<int32> S; Inf->FindSectors(Name, FString(), S); return S.Num() ? S[0] : -1; };
	auto Floor = [Inf](int32 s) { return Inf->Live.Sectors.IsValidIndex(s) ? Inf->Live.Sectors[s].Floor.Y : 0.0; };
	auto Ceil = [Inf](int32 s) { return Inf->Live.Sectors.IsValidIndex(s) ? Inf->Live.Sectors[s].Ceil.Y : 0.0; };
	auto PutAt = [&](const FVector& Feet, double Yaw, double Pitch)
	{
		TeleportTo(Feet + FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0), FRotator(0.0, Yaw, 0.0), false, true);
		GetCharacterMovement()->Velocity = FVector::ZeroVector;
		if (Controller) Controller->SetControlRotation(FRotator(Pitch, Yaw, 0.0));
	};
	auto Shot = [&](const TCHAR* When)
	{
		const FString Name = FString::Printf(TEXT("cwwall2_%s_%s"), Tag, When);
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cwwall2 shot %s.png"), *Name);
	};
	auto Loaded = [&]()
	{
		bSeenPending |= L->PendingLoad.IsValid() || L->IsRebuildPending();
		return bSeenPending && !L->PendingLoad.IsValid() && !L->IsRebuildPending() && L->HasActorBegunPlay() && ShotTime > Wait;
	};
	// the crack: 871's wall onto TUNNELPOW, its middle and the yaw to it from a plan point
	auto CrackMid = [&]() -> FVector2D
	{
		const FOutlawsSector& S = Inf->Live.Sectors[Mouth871];
		return (S.Verts[S.Walls[CrackWall].V1] + S.Verts[S.Walls[CrackWall].V2]) * 0.5;
	};
	auto CrackIn = [&]() -> FVector2D       // (the wall's normal into 871)
	{
		const FOutlawsSector& S = Inf->Live.Sectors[Mouth871];
		const FVector2D A = S.Verts[S.Walls[CrackWall].V1], B = S.Verts[S.Walls[CrackWall].V2], M = (A + B) * 0.5;
		FVector2D N = FVector2D(B.Y - A.Y, A.X - B.X).GetSafeNormal();
		return Inf->Live.ContainsXZ(S, M.X + N.X * 0.3, M.Y + N.Y * 0.3) ? N : -N;
	};
	auto YawTo = [](const FVector2D& From, const FVector2D& To) { return FMath::RadiansToDegrees(FMath::Atan2(To.X - From.X, To.Y - From.Y)); };     // (UE X is Outlaws z, UE Y is Outlaws x)
	// the snapshot's place: UE (-4016.2, 5357.0), game x 178.57 z -133.87 in 871, looking at the crack
	auto ToSnapshot = [&]()
	{
		const FVector2D At(178.57, -133.87);
		PutAt(FOutlawsGeometry::ToUE(At, Floor(Mouth871)), YawTo(At, CrackMid()), -8.0);
	};
	auto Plugs = [&]() { return FString::Printf(TEXT("TUNNELPOW %d floor %.2f ceiling %.2f, TUNNELPOW2 %d floor %.2f ceiling %.2f"), Pow, Floor(Pow), Ceil(Pow), Pow2, Floor(Pow2), Ceil(Pow2)); };
	auto Elev = [&](int32 s) -> FString
	{
		const TArray<int32>* Es = Inf->ElevatorsBySector.Find(s);
		if (!Es || !Es->Num()) return TEXT("none");
		const FOutlawsElevator& E = Inf->Elevators[(*Es)[0]];
		return FString::Printf(TEXT("stop %d target %d%s"), E.Stop, E.Target, E.bTerminated ? TEXT(" terminated") : TEXT(""));
	};
	auto StickLit = [&]()
	{
		Equip(1, 6);
		Mag[1] = 1;
		bFuseLit[1] = true;
		Fuse[1] = 2.5f;
	};
	auto LastStick = [this]() -> FThrown* { for (int32 i = Thrown.Num() - 1; i >= 0; --i) if (Thrown[i].Type == 1 && Thrown[i].Fuse >= 0.f) return &Thrown[i]; return nullptr; };
	// after the stick: where it went off, the sectors its blast told, the plugs
	auto AfterStick = [&](const TCHAR* How)
	{
		const FVector At = FOutlawsGeometry::ToOutlaws(StickLast);
		int32 Own = -1;
		const TArray<int32> Got = BlastSectors(*Inf, Inf->Live, At, WallR, Own);
		const FVector2D M = CrackMid();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cwwall2 (%s) the stick went off at game x %.2f y %.2f z %.2f (sector %d), %.2f units from the crack's middle, %.2f out from its wall; blast-minded sectors within WALL_RADIUS %.0f: %s"),
			How, At.X, At.Y, At.Z, Own, FVector2D::Distance(FVector2D(At.X, At.Z), M), FVector2D::DotProduct(FVector2D(At.X, At.Z) - M, CrackIn()), WallR,
			*FString::JoinBy(Got, TEXT(","), [&](int32 s) { return FString::Printf(TEXT("%d %s"), s, *Inf->Live.Sectors[s].Name); }));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cwwall2 (%s) after: %s; TUNNELPOW elevator %s, TUNNELPOW2 elevator %s"), How, *Plugs(), *Elev(Pow), *Elev(Pow2));
		if (bOld)
		{
			Check(FMath::Abs(Floor(Pow) + 21.0) < 0.01 && FMath::Abs(Floor(Pow2) + 27.0) < 0.01, FString::Printf(TEXT("(%s) -OLOldCwWall2: as reported, the crack's rubble stayed (TUNNELPOW floor %.2f) while the far face's opened (TUNNELPOW2 %.2f)"),
				How, Floor(Pow), Floor(Pow2)));
		}
		else
		{
			Check(Got.Contains(Pow) && FMath::Abs(Floor(Pow) + 27.0) < 0.01 && FMath::Abs(Floor(Pow2) + 27.0) < 0.01, FString::Printf(TEXT("(%s) the blast opened the crack: TUNNELPOW floor %.2f, TUNNELPOW2 %.2f (from -21 to -27, the floor of 871)"),
				How, Floor(Pow), Floor(Pow2)));
		}
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 4.f || !bUp || !Inf) return;
		L->bInvincible = true;
		for (const TWeakObjectPtr<AOutlawsEnemy>& E : L->GetEnemies()) if (E.IsValid() && !E->IsDead() && !E->IsDormant()) E->SetDormant(true);
		const FOutlawsAssets* A = L->GetAssets();
		{
			FString Pd, Func;
			TMap<FString, FString> D;
			if (A && A->GetText(TEXT("pdynam.itm"), Pd)) { OutlawsFormats::ParseItm(Pd, Func, D); if (D.Contains(TEXT("WALL_RADIUS"))) WallR = FCString::Atod(*D[TEXT("WALL_RADIUS")]); }
		}
		// (0) every level's sector items a blast works: a stick set down 1 unit and one come to rest 2.8 units out in front of
		// each, on the floor of its roomiest neighbour (a crouch's 2.6 units at least), the INF run 15 s on
		static const TCHAR* const Levels[] = { TEXT("canyon"), TEXT("civlwar1"), TEXT("civlwar2"), TEXT("civpow2"), TEXT("cliff"), TEXT("drygulch"), TEXT("fort"),
			TEXT("gallery"), TEXT("granary"), TEXT("hideout"), TEXT("htrain"), TEXT("icecaves"), TEXT("indy"), TEXT("mill"), TEXT("miner"), TEXT("office"), TEXT("ranch"),
			TEXT("shfort"), TEXT("shgranry"), TEXT("shgulch"), TEXT("shoot"), TEXT("shsimms"), TEXT("shwildns"), TEXT("simms"), TEXT("town"), TEXT("town_2p"),
			TEXT("townex"), TEXT("train"), TEXT("villa"), TEXT("wharf") };
		int32 Items = 0, Opened = 0, Lines = 0, Explodes = 0, PowOk = 0;
		FString Shut, LineList, ExplodeList;
		for (const TCHAR* Lvl : Levels)
		{
			FString Lvt, InfText;
			if (!A || !A->GetText(FString(Lvl) + TEXT(".lvt"), Lvt) || !A->GetText(FString(Lvl) + TEXT(".inf"), InfText)) continue;
			FOutlawsLevel Lv;
			Lv.ParseLvt(Lvt);
			FOutlawsInfRuntime R0;
			R0.Init(Lv, InfText);
			for (int32 i = 0; i < R0.Inf.Items.Num(); ++i)
			{
				const FOutlawsInfItem& It = R0.Inf.Items[i];
				bool bBlast = false, bExplode = false;
				for (const FOutlawsInfClass& C : It.Classes)
				{
					bBlast |= C.bHasEventMask && (C.EventMask & OutlawsInf::EvExplosion) && (C.IsElevator() || C.IsTrigger());
					bExplode |= C.Class == EOutlawsInfClass::Explode;
				}
				if (bExplode) { ++Explodes; ExplodeList += FString::Printf(TEXT(" %s:%s"), Lvl, *It.Name); }
				if (!bBlast) continue;
				if (It.bLine) { ++Lines; LineList += FString::Printf(TEXT(" %s:%s#%s"), Lvl, *It.Name, *It.WallId); continue; }
				TArray<int32> Secs;
				R0.FindSectors(It.Name, It.NameId, Secs);
				if (!Secs.Num()) { ++Items; Shut += FString::Printf(TEXT(" %s:%s(no sector)"), Lvl, *It.Name); continue; }
				const int32 s = Secs[0];
				const FOutlawsSector& S = R0.Live.Sectors[s];
				TSet<int32> Minded;
				BlastMinded(R0, Minded);
				// the roomiest neighbour that is not blast-minded itself
				int32 BestW = -1;
				double BestRoom = 2.6;
				for (int32 w = 0; w < S.Walls.Num(); ++w)
				{
					const int32 n = S.Walls[w].Adjoin;
					if (n < 0 || Minded.Contains(n)) continue;
					const FOutlawsSector& N = R0.Live.Sectors[n];
					const FVector2D Mid = (S.Verts[S.Walls[w].V1] + S.Verts[S.Walls[w].V2]) * 0.5;
					const double Room = R0.Live.SurfaceY(N, Mid, false) - R0.Live.SurfaceY(N, Mid, true);
					if (Room > BestRoom) { BestRoom = Room; BestW = w; }
				}
				++Items;
				if (BestW < 0) { Shut += FString::Printf(TEXT(" %s:%s(no neighbour with room)"), Lvl, *It.Name); continue; }
				const int32 n = S.Walls[BestW].Adjoin;
				const FVector2D A2 = S.Verts[S.Walls[BestW].V1], B2 = S.Verts[S.Walls[BestW].V2], Mid = (A2 + B2) * 0.5;
				FVector2D Nrm = FVector2D(B2.Y - A2.Y, A2.X - B2.X).GetSafeNormal();
				if (!R0.Live.ContainsXZ(R0.Live.Sectors[n], Mid.X + Nrm.X * 0.3, Mid.Y + Nrm.Y * 0.3)) Nrm = -Nrm;
				FString Each;
				bool bAll = true;
				for (const double Out : { 1.0, 2.8 })
				{
					FOutlawsInfRuntime R;
					R.Init(Lv, InfText);
					const FVector2D P = Mid + Nrm * Out;
					const FOutlawsSector* Under = R.Live.SectorAt(P.X, R.Live.SurfaceY(R.Live.Sectors[n], P, true) + 1.0, P.Y);
					const FOutlawsSector& On = Under ? *Under : R.Live.Sectors[n];
					const FVector At(P.X, R.Live.SurfaceY(On, P, true) + 0.3, P.Y);
					int32 Own = -1;
					const TArray<int32> Got = BlastSectors(R, Lv, At, WallR, Own);
					if (Own >= 0) R.SectorEvent(Own, OutlawsInf::EvExplosion | 0x8000);
					for (const int32 g : Got) R.SectorEvent(g, OutlawsInf::EvExplosion);
					for (int32 k = 0; k < 300; ++k) R.Tick(0.05);
					// opened: each of its blast elevators past its start, each blast trigger's client elevators moved on
					bool bOk = true;
					FString Moved;
					for (int32 e = 0; e < R.Elevators.Num(); ++e)
					{
						const FOutlawsElevator& E = R.Elevators[e];
						const FOutlawsInfClass& C = R.Inf.Items[E.Item].Classes[E.ClassIndex];
						bool bWatch = E.Item == i && C.bHasEventMask && (C.EventMask & OutlawsInf::EvExplosion);
						for (const FOutlawsInfClass& T : It.Classes)
						{
							if (!T.IsTrigger() || !T.bHasEventMask || !(T.EventMask & OutlawsInf::EvExplosion)) continue;
							for (const FString& Client : T.Clients) bWatch |= E.Name.Equals(Client, ESearchCase::IgnoreCase);
						}
						if (!bWatch) continue;
						const bool bOn = E.Target < 0 && E.Stop != FMath::Clamp(C.StartStop, 0, C.Stops.Num() - 1);
						bOk &= bOn;
						Moved += FString::Printf(TEXT(" %s %s %.2f"), *E.Name, bOn ? TEXT("moved to") : TEXT("STAYED at"), E.Current);
					}
					bAll &= bOk && !Moved.IsEmpty();
					Each += FString::Printf(TEXT(" [%s %.1f units out in %d (own sector %d, %d more told):%s]"), Out < 2.0 ? TEXT("set down") : TEXT("thrown"), Out, n, Own, Got.Num(), *Moved);
				}
				Opened += bAll ? 1 : 0;
				if (!bAll) Shut += FString::Printf(TEXT(" %s:%s"), Lvl, *It.Name);
				if (FCString::Stricmp(Lvl, TEXT("civlwar2")) == 0 && It.Name.StartsWith(TEXT("TUNNELPOW"))) PowOk += bAll ? 1 : 0;
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cwwall2 (0) %s %s (sector %d, floor %.1f ceiling %.1f): %s%s"), Lvl, *It.Name, s, S.Floor.Y, S.Ceil.Y, bAll ? TEXT("opens") : TEXT("DOES NOT OPEN"), *Each);
			}
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cwwall2 (0) beside them: %d line items a blast works (no blast event reaches a line here; each also opens to a shot or the hand):%s; %d ELEVATOR EXPLODE sectors (their walls blown by the level's own ExplodeWalls):%s"),
			Lines, *LineList, Explodes, *ExplodeList);
		if (bOld)
		{
			Check(PowOk == 0 && Opened == Items - 2, FString::Printf(TEXT("(0) -OLOldCwWall2: as reported, civlwar2's two rubble faces stay shut, every other of the %d blast sector items opens (%d do; shut:%s)"), Items, Opened, *Shut));
		}
		else
		{
			Check(Items > 0 && Opened == Items, FString::Printf(TEXT("(0) every level's sector items a blast works open to a stick set down or thrown before them: %d of %d%s"), Opened, Items,
				Shut.IsEmpty() ? TEXT("") : *(TEXT("; shut:") + Shut)));
		}
		Boom = SectorNamed(TEXT("DAMBOOM"));
		Dam = SectorNamed(TEXT("DAMDROP"));
		Water = SectorNamed(TEXT("DAMWATER"));
		Pow = SectorNamed(TEXT("TUNNELPOW"));
		Pow2 = SectorNamed(TEXT("TUNNELPOW2"));
		if (!L->LevelName.Equals(TEXT("civlwar2"), ESearchCase::IgnoreCase) || Boom < 0 || Dam < 0 || Water < 0 || Pow < 0 || Pow2 < 0)
		{
			Check(false, FString::Printf(TEXT("civlwar2's sectors (DAMBOOM %d, DAMDROP %d, DAMWATER %d, TUNNELPOW %d, TUNNELPOW2 %d) on %s"), Boom, Dam, Water, Pow, Pow2, *L->LevelName));
			Finish();
			return;
		}
		// the snapshot's sector: the one with the wall onto TUNNELPOW, next to the lake
		for (int32 s = 0; s < Inf->Live.Sectors.Num() && Mouth871 < 0; ++s)
		{
			const FOutlawsSector& S = Inf->Live.Sectors[s];
			if (s == Pow) continue;
			for (int32 w = 0; w < S.Walls.Num(); ++w) if (S.Walls[w].Adjoin == Pow && S.Walls.ContainsByPredicate([&](const FOutlawsWall& W) { return W.Adjoin == Water; })) { Mouth871 = s; CrackWall = w; }
		}
		// (1) the dam blown with the big TNT from its crack (as -OLTntDamProbe), the water up
		L->CheatJackpot();
		PutAt(FVector(-2040.685, 11255.092, Floor(Boom) * U), -64.03, -1.1);
		Water0 = Floor(Water);
		Wait = ShotTime + 1.f;
		ShotStep = 1;
		return;
	}
	case 1:
	{
		if (ShotTime < Wait) return;
		const bool bUsed = L->UseItem(TEXT("idynable"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cwwall2 (1) the big TNT used at the dam: %d; %s"), bUsed ? 1 : 0, *L->LastItemUse);
		UseAt = LastLog = ShotTime;
		ShotStep = 2;
		return;
	}
	case 2:
	{
		const float Since = ShotTime - UseAt;
		const bool bUp2 = Floor(Water) >= -27.5 - 0.01;
		if (ShotTime - LastLog >= 10.f)
		{
			LastLog = ShotTime;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cwwall2 (1) %.0f s: the dam's top %.2f, the water %.2f"), Since, Floor(Dam), Floor(Water));
		}
		if (!bUp2 && Since < 150.f) return;
		Check(bUp2 && Floor(Dam) < -26.0 && Water0 < -39.9, FString::Printf(TEXT("(1) the dam blown (its top %.2f) and the water up from %.2f to %.2f, %.0f s after the TNT"), Floor(Dam), Water0, Floor(Water), Since));
		if (Mouth871 < 0)
		{
			Check(false, TEXT("(2) the snapshot's sector: the one off the lake with the wall onto TUNNELPOW"));
			Finish();
			return;
		}
		ToSnapshot();
		Wait = ShotTime + 1.5f;
		ShotStep = 3;
		return;
	}
	case 3:
	{
		if (ShotTime < Wait) return;
		// (2) the snapshot's spot: the sector, the crack's wall (871's onto TUNNELPOW: its overlay the crack), the rubble
		const FOutlawsSector& S = Inf->Live.Sectors[Mouth871];
		const FOutlawsWall& W = S.Walls[CrackWall];
		int32 Back = -1;
		const FOutlawsSector& P = Inf->Live.Sectors[Pow];
		for (int32 w = 0; w < P.Walls.Num(); ++w) if (P.Walls[w].Adjoin == Mouth871) Back = w;
		const FVector2D Me = FOutlawsGeometry::ToOutlawsPlan(GetActorLocation());
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cwwall2 (2) the player at game x %.2f z %.2f in sector %d, %.2f units from the crack; the crack is %d's wall %d (#%s, overlay %d, flags %x %x) onto %d, whose wall %d (#%s, overlay %d, flags %x %x) faces back; before: %s; TUNNELPOW elevator %s, TUNNELPOW2 elevator %s"),
			Me.X, Me.Y, L->PlayerSectorIndex(), FVector2D::Distance(Me, CrackMid()), Mouth871, CrackWall, *W.Id, W.Overlay, W.Flags1, W.Flags2, Pow, Back,
			Back >= 0 ? *P.Walls[Back].Id : TEXT("-"), Back >= 0 ? P.Walls[Back].Overlay : -1, Back >= 0 ? P.Walls[Back].Flags1 : 0, Back >= 0 ? P.Walls[Back].Flags2 : 0, *Plugs(), *Elev(Pow), *Elev(Pow2));
		Check(L->PlayerSectorIndex() == Mouth871 && Mouth871 == 871 && FMath::Abs(Floor(Pow) + 21.0) < 0.01 && FMath::Abs(Floor(Pow2) + 21.0) < 0.01,
			FString::Printf(TEXT("(2) the snapshot's spot is in sector %d (871), the rubble shut: %s"), L->PlayerSectorIndex(), *Plugs()));
		Shot(TEXT("before"));
		Wait = ShotTime + 1.f;
		ShotStep = 4;
		return;
	}
	case 4:
	{
		if (ShotTime < Wait) return;
		const bool bSaved = L->SaveGame(7);
		Check(bSaved, TEXT("(2) saved to slot 7 at the snapshot's spot"));
		StickLit();
		Wait = ShotTime + 0.5f;
		ShotStep = 5;
		return;
	}
	case 5:
	{
		if (ShotTime < Wait) return;
		// (3) thrown at the crack as the snapshot's was (635 cm/s), from the right hand by the shoulder
		const FVector Eye = Camera->GetComponentLocation();
		const FVector2D M = CrackMid();
		const FVector Aim = FOutlawsGeometry::ToUE(M, Floor(Mouth871) + 2.0);
		const int32 Before = Thrown.Num();
		ThrowHeld(1, false, false);
		FThrown* T = Thrown.Num() > Before ? LastStick() : nullptr;
		if (T)
		{
			T->Pos = Eye + (Aim - Eye).GetSafeNormal2D() * 30.0 - FVector(0.0, 0.0, 30.0);
			T->Vel = (Aim - T->Pos).GetSafeNormal() * 635.0 + FVector(0.0, 0.0, 120.0);
			T->Fuse = 2.5f;
			if (AActor* Ac = T->Actor.Get()) Ac->SetActorLocation(T->Pos);
		}
		Check(T != nullptr, TEXT("(3) a lit stick thrown at the crack at 635 cm/s"));
		bStick = T != nullptr;
		ShotStep = 6;
		return;
	}
	case 6:
	case 9:
	{
		// the stick in flight or at rest: where it is, until it goes off
		if (FThrown* T = LastStick()) { StickLast = T->Pos; return; }
		if (bStick) { bStick = false; Wait = ShotTime + 1.5f; return; }
		if (ShotTime < Wait) return;
		AfterStick(ShotStep == 6 ? TEXT("3, thrown") : TEXT("4, set down"));
		ToSnapshot();
		Wait = ShotTime + 0.5f;
		ShotStep = ShotStep == 6 ? 7 : 10;
		return;
	}
	case 7:
	{
		if (ShotTime < Wait) return;
		Shot(TEXT("thrown"));
		// (4) back to before the blast (slot 7), then a stick set down at the foot of the crack
		Check(L->LoadGame(7), TEXT("(4) loading slot 7"));
		bSeenPending = false;
		Wait = ShotTime + 1.5f;
		ShotStep = 8;
		return;
	}
	case 8:
	{
		if (!Loaded()) return;
		Check(FMath::Abs(Floor(Pow) + 21.0) < 0.01 && FMath::Abs(Floor(Pow2) + 21.0) < 0.01, FString::Printf(TEXT("(4) slot 7 loaded: the rubble shut again: %s"), *Plugs()));
		StickLit();
		const int32 Before = Thrown.Num();
		ThrowHeld(1, false, true);
		FThrown* T = Thrown.Num() > Before ? LastStick() : nullptr;
		if (T)
		{
			T->Pos = FOutlawsGeometry::ToUE(CrackMid() + CrackIn() * 1.0, Floor(Mouth871) + 0.5);
			T->Vel = FVector::ZeroVector;
			T->Fuse = 2.5f;
			if (AActor* Ac = T->Actor.Get()) Ac->SetActorLocation(T->Pos);
		}
		Check(T != nullptr, TEXT("(4) a lit stick set down a unit in front of the crack"));
		bStick = T != nullptr;
		ShotStep = 9;
		return;
	}
	case 10:
	{
		if (ShotTime < Wait) return;
		Shot(TEXT("placed"));
		if (bOld) { Wait = ShotTime + 1.f; ShotStep = 13; return; }
		// (5) the way on: crouched from the snapshot's spot through the opened rubble (TUNNELPOW, the rubble's sloped middle
		// 115, TUNNELPOW2) into 893, then along it to the pit 643
		const FOutlawsSector& P = Inf->Live.Sectors[Pow];
		auto Centre = [&](int32 s) { FVector2D C = FVector2D::ZeroVector; for (const FVector2D& V : Inf->Live.Sectors[s].Verts) C += V; return C / FMath::Max(1, Inf->Live.Sectors[s].Verts.Num()); };
		// (the roomiest of the rubble's middle sectors, between the two faces)
		int32 Mid = -1;
		double MidRoom = 0.0;
		for (const FOutlawsWall& W : P.Walls)
		{
			if (W.Adjoin < 0 || W.Adjoin == Mouth871 || !Inf->Live.Sectors[W.Adjoin].Walls.ContainsByPredicate([&](const FOutlawsWall& W2) { return W2.Adjoin == Pow2; })) continue;
			const FOutlawsSector& Ms = Inf->Live.Sectors[W.Adjoin];
			const double Room = Inf->Live.SurfaceY(Ms, Centre(W.Adjoin), false) - Inf->Live.SurfaceY(Ms, Centre(W.Adjoin), true);
			if (Room > MidRoom) { MidRoom = Room; Mid = W.Adjoin; }
		}
		Waypoints = { CrackMid() + CrackIn() * 1.5, Centre(Pow), Mid >= 0 ? Centre(Mid) : Centre(Pow), Centre(Pow2), FVector2D(164.5, -136.0), FVector2D(160.0, -126.0),
			FVector2D(145.0, -119.5), FVector2D(137.0, -118.0), FVector2D(129.25, -118.5) };
		ToSnapshot();
		SetCrouchHeld(true);
		Way = 0;
		WalkPath = FString::FromInt(L->PlayerSectorIndex());
		WalkAt = MovedAt = ShotTime;
		ShotStep = 11;
		return;
	}
	case 11:
	{
		const FVector2D Me = FOutlawsGeometry::ToOutlawsPlan(GetActorLocation());
		while (Waypoints.IsValidIndex(Way) && FVector2D::Distance(Me, Waypoints[Way]) < 0.7) ++Way;
		const int32 Now = L->PlayerSectorIndex();
		if (!WalkPath.EndsWith(FString::Printf(TEXT(" %d"), Now)) && !WalkPath.Equals(FString::FromInt(Now))) WalkPath += FString::Printf(TEXT(" %d"), Now);
		const float Since = ShotTime - WalkAt;
		if (ShotTime - LastLog >= 2.f)
		{
			LastLog = ShotTime;
			const UCapsuleComponent* Cap = GetCapsuleComponent();
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cwwall2 (5) %.0f s: game x %.2f y %.2f z %.2f in sector %d, waypoint %d, crouched %d (body %.0f cm high, %.0f wide), %.0f cm/s"), Since, Me.X,
				GetActorLocation().Z / U - Cap->GetScaledCapsuleHalfHeight() / U, Me.Y, Now, Way, bIsCrouched ? 1 : 0, Cap->GetScaledCapsuleHalfHeight() * 2.f, Cap->GetScaledCapsuleRadius() * 2.f,
				GetVelocity().Size2D());
		}
		if (FVector2D::Distance(Me, MovedFrom) > 0.25) { MovedFrom = Me; MovedAt = ShotTime; }
		if (Waypoints.IsValidIndex(Way) && Since < 45.f && ShotTime - MovedAt < 5.f)     // (on till the last waypoint, or 5 s without getting on)
		{
			const double Yaw = YawTo(Me, Waypoints[Way]);
			if (Controller) Controller->SetControlRotation(FRotator(0.0, Yaw, 0.0));
			AddMovementInput(FRotator(0.0, Yaw, 0.0).Vector(), 1.f);
			return;
		}
		if (!WalkPath.Contains(TEXT(" 893")))
		{
			// stuck short of the far mouth: the world's floor and roof along the way through the rubble (traces down and up
			// every quarter unit from the crack to 893, on the line the walk took), and the INF's own heights there
			FString Prof;
			const FVector2D From = CrackMid() + CrackIn() * 1.5, To = FVector2D(166.5, CrackMid().Y);
			for (int32 k = 0; k <= 20; ++k)
			{
				const FVector2D Pt = From + (To - From) * (k / 20.0);
				const FVector Hi = FOutlawsGeometry::ToUE(Pt, -22.0);
				FHitResult Dn, Upw;
				FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsCwWall2), true, this);
				const bool bDn = GetWorld()->LineTraceSingleByChannel(Dn, Hi, FOutlawsGeometry::ToUE(Pt, -30.0), ECC_Pawn, Q);
				const FVector Lo = bDn ? Dn.ImpactPoint + FVector(0.0, 0.0, 15.0) : FOutlawsGeometry::ToUE(Pt, -26.0);
				const bool bRoof = GetWorld()->LineTraceSingleByChannel(Upw, Lo, FOutlawsGeometry::ToUE(Pt, -18.0), ECC_Pawn, Q);
				const FOutlawsSector* At = Inf->Live.SectorAt(Pt.X, -25.5, Pt.Y);
				Prof += FString::Printf(TEXT(" [x %.2f: floor %s roof %s; sector %d %s]"), Pt.X, bDn ? *FString::Printf(TEXT("%.2f (%s)"), Dn.ImpactPoint.Z / U, Dn.GetComponent() ? *Dn.GetComponent()->GetName() : TEXT("-")) : TEXT("none"),
					bRoof ? *FString::Printf(TEXT("%.2f (%s)"), Upw.ImpactPoint.Z / U, Upw.GetComponent() ? *Upw.GetComponent()->GetName() : TEXT("-")) : TEXT("none"), At ? At->Index : -1,
					At ? *FString::Printf(TEXT("%.2f..%.2f"), Inf->Live.SurfaceY(*At, Pt, true), Inf->Live.SurfaceY(*At, Pt, false)) : TEXT(""));
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cwwall2 (5) the way through, traced at z %.2f:%s"), CrackMid().Y, *Prof);
			// the body as it is, swept on toward the waypoint it could not reach: what stops it
			const UCapsuleComponent* Cap = GetCapsuleComponent();
			FHitResult Hit;
			FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsCwWall2), false, this);
			const FVector Ahead = FVector(FRotator(0.0, YawTo(Me, Waypoints.IsValidIndex(Way) ? Waypoints[Way] : Me), 0.0).Vector()) * 45.0;
			for (const double Lift : { 0.0, 20.0, 40.0 })
			{
				const FVector P0 = GetActorLocation() + FVector(0.0, 0.0, Lift);
				const bool bHit = GetWorld()->SweepSingleByChannel(Hit, P0, P0 + Ahead, FQuat::Identity, ECC_Pawn,
					FCollisionShape::MakeCapsule(Cap->GetScaledCapsuleRadius(), Cap->GetScaledCapsuleHalfHeight()), Q);
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cwwall2 (5) the body swept 45 cm on from %.0f cm up: %s"), Lift, bHit ? *FString::Printf(TEXT("hits %s at game x %.2f y %.2f z %.2f, normal %s, after %.0f cm%s"),
					Hit.GetComponent() ? *Hit.GetComponent()->GetName() : TEXT("-"), Hit.ImpactPoint.Y / U, Hit.ImpactPoint.Z / U, Hit.ImpactPoint.X / U, *Hit.ImpactNormal.ToString(), Hit.Distance,
					Hit.bStartPenetrating ? TEXT(" (starts in it)") : TEXT("")) : TEXT("free"));
			}
		}
		const bool bThrough = WalkPath.Contains(FString::Printf(TEXT(" %d"), Pow2)) && WalkPath.Contains(TEXT(" 893"));
		Check(bThrough && WalkPath.Contains(FString::Printf(TEXT(" %d"), Pow)), FString::Printf(TEXT("(5) the way through the opened rubble: a crouched walk from the snapshot's spot reached 893 by TUNNELPOW and TUNNELPOW2 (sectors on the way: %s; %d of %d waypoints in %.1f s)"),
			*WalkPath.Left(200), Way, Waypoints.Num(), Since));
		Check(Now == 643 || WalkPath.Contains(TEXT(" 643")), FString::Printf(TEXT("(5) on along 893 to the pit 643: the walk ended in sector %d at game x %.2f z %.2f (floor %.2f)"), Now, Me.X, Me.Y, Floor(Now)));
		SetCrouchHeld(false);
		if (Controller) Controller->SetControlRotation(FRotator(25.0, YawTo(Me, FVector2D(129.0, -112.0)), 0.0));
		Wait = ShotTime + 1.f;
		ShotStep = 12;
		return;
	}
	case 12:
	{
		if (ShotTime < Wait) return;
		Shot(TEXT("way"));
		// what can be walked to from there (a step of 3.5 units, a crouch's 2.6 of room at the shared wall), and climbed to
		// (a jump with the ledge grab: 7.5 units up); civlwar2 has no level end of its own (-OLTntDamProbe (4))
		const FOutlawsLevel& Lv = Inf->Live;
		auto Flood = [&](int32 From, double Step, int32& OutDeepest, int32& OutDepth)
		{
			TArray<int32> Open = { From };
			TMap<int32, int32> Depth = { { From, 0 } };
			OutDeepest = From;
			OutDepth = 0;
			for (int32 k = 0; k < Open.Num(); ++k)
			{
				const FOutlawsSector& S = Lv.Sectors[Open[k]];
				for (const FOutlawsWall& W : S.Walls)
				{
					const int32 Nx = W.Adjoin;
					if (Nx < 0 || Depth.Contains(Nx) || !Lv.Sectors.IsValidIndex(Nx)) continue;
					const FOutlawsSector& T = Lv.Sectors[Nx];
					bool bOk = false;
					for (const FVector2D& Pt : { S.Verts[W.V1], S.Verts[W.V2], (S.Verts[W.V1] + S.Verts[W.V2]) * 0.5 })
					{
						const double Fs = Lv.SurfaceY(S, Pt, true), Ft = Lv.SurfaceY(T, Pt, true);
						const double Room = FMath::Min(Lv.SurfaceY(S, Pt, false), Lv.SurfaceY(T, Pt, false)) - FMath::Max(Fs, Ft);
						bOk |= Ft - Fs <= Step && Room >= 2.6;
					}
					if (!bOk) continue;
					const int32 D = Depth[Open[k]] + 1;
					Depth.Add(Nx, D);
					Open.Add(Nx);
					if (D > OutDepth) { OutDepth = D; OutDeepest = Nx; }
				}
			}
			return Open.Num();
		};
		const int32 Here = L->PlayerSectorIndex();
		int32 WalkFar = -1, WalkDepth = 0, ClimbFar = -1, ClimbDepth = 0;
		const int32 Walk = Flood(Here, 3.5, WalkFar, WalkDepth);
		const int32 Climb = Flood(Here, 7.5, ClimbFar, ClimbDepth);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cwwall2 (5) the last place reached: sector %d (floor %.2f, ceiling %.2f); walked on from there: %d of the level's %d sectors (the farthest %d, %d sectors on); with jumps and the ledge grab: %d (the farthest %d, floor %.2f, %d on)"),
			Here, Floor(Here), Ceil(Here), Walk, Lv.Sectors.Num(), WalkFar, WalkDepth, Climb, ClimbFar, Floor(ClimbFar), ClimbDepth);
		Wait = ShotTime + 0.5f;
		ShotStep = 13;
		return;
	}
	case 13:
		if (ShotTime < Wait) return;
		Finish();
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
