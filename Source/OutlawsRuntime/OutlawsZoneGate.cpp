// BUG-MILLCLIMB (user 2026-10-19, Quest 3): in the mill's hall, which is built inside the yard's open sectors, a climb onto a
// box threw the player out into the yard (a soft lock), and the yard flashed into view from a narrow wall indoors.  The
// tracking's pick by the feet (sector_which3D: the smallest sector holding them) took yard sector 602 (1960.5 square units)
// over the hall's own 576 (1961) whenever a move did not explain itself (a mantle's rise past the box's edge); the hall was
// then out of the body's reach, so it passed through the floor, and the flood ran from the yard.
//
// The user's design: a zone gate.  A sector the player must pass through between the two parts, from which neither part's
// overlapping sectors are seen; which side the player is on follows the order of the sectors walked through (out of the
// gate into one side or the other: backtracking any number of times works the same way), and the other side's
// overlapping sectors are off for this machine's player (not drawn, passed through, never returned by the sector lookups
// made for the player).  A jump (a teleport, a load, a restart, the level's start) decides the side from where the body
// stands, through the sector graph.  Co-op: each machine keeps its own player's side.  Per level (GZoneGateDefs): the
// gate and the sectors sharing space across it; the sides come from the level itself (the parts of the adjoin graph the
// gate leads to), so another level needs only its line there.  -OLOldMillclimb: none of it (before-and-after runs).

#include "OutlawsLevelActor.h"
#include "OutlawsFlags.h"
#include "OutlawsLevel.h"
#include "OutlawsGeometry.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsRuntime.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"

namespace
{
	struct FZoneGateDef
	{
		const TCHAR* Level;
		int32 NumSectors;       // the level as shipped (a changed level keeps no gate)
		int32 Gate;
		const TCHAR* Sectors;   // the sectors sharing space across the gate, both sides: each is off while the player is on the other
	};
	const FZoneGateDef GZoneGateDefs[] =
	{
		// the mill: its hall (576, its walkway 572 and the flumes 394, 461, 541, 550 over them) stands inside the yard's open
		// sectors (552, 602, 355, 473, 429, and 607 the sky round the level).  Every way between the yard and the hall runs
		// through the doorway 87, the rooms 502, 353 (BUST1), 532 (BUST2) and 364 (BUST3), the ACCESS1 doorway 132 and room
		// 404 ("A"), then 417 and on.  The yard is seen from the rooms as far in as 502, and the hall from nowhere on that
		// side, so the gate is room 404, from which neither is seen (-OLMillClimbTest=scan and check)
		{ TEXT("mill"), 611, 404, TEXT("552 602 355 473 429 607 576 572 550 394 461 541") },
	};

	bool OldMillclimb()
	{
		static const bool bOld = OLFlags::Param(TEXT("OLOldMillclimb"));
		return bOld;
	}
}

void AOutlawsLevelActor::BuildZoneGates(const FOutlawsLevel& L)
{
	ZoneGates.Reset();
	ZoneNow.Reset();
	ZoneSkip.Empty();
	bZoneSkipOn = false;
	bZoneHavePrev = false;
	if (OldMillclimb()) return;
	const int32 N = L.Sectors.Num();
	TArray<FZoneGateDef> Defs;
	for (const FZoneGateDef& D : GZoneGateDefs)
	{
		if (!LevelName.Equals(D.Level, ESearchCase::IgnoreCase)) continue;
		if (N != D.NumSectors)
		{
			UE_LOG(LogOutlaws, Warning, TEXT("OLRT zone gate %s: the level has %d sectors, not %d: no gate"), *LevelName, N, D.NumSectors);
			continue;
		}
		Defs.Add(D);
	}
#if WITH_OUTLAWS_TESTS
	int32 Forced = -1;
	if (OLFlags::Value(TEXT("OLZoneGate"), Forced)) for (FZoneGateDef& D : Defs) D.Gate = Forced;      // (tests: another gate tried)
#endif
	if (Defs.Num() == 0) return;
	// the adjoin graph, both ways (double and vertical adjoins too)
	TArray<TArray<int32>> Adj;
	Adj.SetNum(N);
	auto Link = [&](int32 A, int32 B) { if (A < 0 || B < 0 || A >= N || B >= N || A == B) return; Adj[A].AddUnique(B); Adj[B].AddUnique(A); };
	for (const FOutlawsSector& S : L.Sectors)
	{
		Link(S.Index, S.VAdjoin);
		for (const FOutlawsWall& W : S.Walls) { Link(S.Index, W.Adjoin); Link(S.Index, W.DAdjoin); }
	}
	for (const FZoneGateDef& D : Defs)
	{
		const int32 Gate = D.Gate;
		if (!L.Sectors.IsValidIndex(Gate)) continue;
		TArray<FString> Words;
		FString(D.Sectors).ParseIntoArrayWS(Words);
		TArray<int32> Set;
		for (const FString& Wd : Words) { const int32 Si = FCString::Atoi(*Wd); if (L.Sectors.IsValidIndex(Si) && Si != Gate) Set.AddUnique(Si); }
		FZoneGate G;
		G.Gate = Gate;
		G.Zone.Init(-1, N);
		// the parts the gate leads to: the two holding the set's sectors are its sides; any other (a door leaf standing in
		// the gate, a dead end off it) counts as the gate itself
		TArray<TArray<int32>> Parts;
		TArray<int32> PartOf;
		PartOf.Init(-1, N);
		for (const int32 Nb : Adj[Gate])
		{
			if (PartOf[Nb] >= 0) continue;
			const int32 Pi = Parts.Num();
			TArray<int32>& Queue = Parts.AddDefaulted_GetRef();
			Queue.Add(Nb);
			PartOf[Nb] = Pi;
			for (int32 q = 0; q < Queue.Num(); ++q)
			{
				for (const int32 Nx : Adj[Queue[q]])
				{
					if (Nx == Gate || PartOf[Nx] >= 0) continue;
					PartOf[Nx] = Pi;
					Queue.Add(Nx);
				}
			}
		}
		TArray<int32> SidePart;
		for (const int32 Si : Set) if (PartOf[Si] >= 0) SidePart.AddUnique(PartOf[Si]);
		if (SidePart.Num() != 2)
		{
			UE_LOG(LogOutlaws, Warning, TEXT("OLRT zone gate %s: sector %d leads to %d parts, %d of them holding its sectors, not two: no gate"), *LevelName, Gate, Parts.Num(), SidePart.Num());
			continue;
		}
		SidePart.Sort();
		int32 Count[2] = { 0, 0 };
		for (int32 Side = 0; Side < 2; ++Side)
		{
			for (const int32 Si : Parts[SidePart[Side]]) G.Zone[Si] = int8(Side);
			Count[Side] = Parts[SidePart[Side]].Num();
			for (const int32 Nb : Adj[Gate]) if (PartOf[Nb] == SidePart[Side]) { G.Into[Side] = Nb; break; }
		}
		// each of the set's sectors is off while the player is on the other side
		for (const int32 Si : Set)
		{
			if (G.Zone[Si] >= 0) G.Off[1 - G.Zone[Si]].Add(Si);
			else UE_LOG(LogOutlaws, Warning, TEXT("OLRT zone gate %s: sector %d is on neither side of sector %d: left on"), *LevelName, Si, Gate);
		}
		auto List = [](const TSet<int32>& S2) { TArray<int32> A = S2.Array(); A.Sort(); FString O; for (const int32 X : A) O += FString::Printf(TEXT(" %d"), X); return O; };
		UE_LOG(LogOutlaws, Log, TEXT("OLRT zone gate %s: sector %d between side 0 (through sector %d, %d sectors) and side 1 (through sector %d, %d sectors); off on side 0:%s; off on side 1:%s"),
			*LevelName, Gate, G.Into[0], Count[0], G.Into[1], Count[1], *List(G.Off[0]), *List(G.Off[1]));
		ZoneGates.Add(MoveTemp(G));
	}
	ZoneNow.Init(-1, ZoneGates.Num());
	ZoneSkip.Init(false, N);
}

void AOutlawsLevelActor::RefreshZoneSkip()
{
	ZoneSkip.Init(false, ZoneSkip.Num());
	bZoneSkipOn = false;
	for (int32 g = 0; g < ZoneGates.Num(); ++g)
	{
		if (!ZoneNow.IsValidIndex(g) || ZoneNow[g] < 0) continue;
		for (const int32 S : ZoneGates[g].Off[ZoneNow[g]])
		{
			if (S >= 0 && S < ZoneSkip.Num()) { ZoneSkip[S] = true; bZoneSkipOn = true; }
		}
	}
}

int32 AOutlawsLevelActor::ZoneRedecide(double X, double Y, double Z, const TCHAR* Why)
{
	const FOutlawsLevel* Lv = Inf ? &Inf->Live : LevelData.Get();
	if (!Lv) return -1;
	// of the sectors holding the feet (half a unit either way), the walled one whose floor is nearest the feet (the one
	// stood on), the smaller on a tie; the sky round a level only when nothing else holds them (FOutlawsLevel::Floor3D,
	// BUG-JUMPPICK: every level's jumps take this pick now).  The far side is not left out here: this decides it
	const int32 Best = Lv->Floor3D(X, Y, Z);
	bool bChanged = false;
	for (int32 g = 0; g < ZoneGates.Num(); ++g)
	{
		const int32 Side = ZoneGates[g].Zone.IsValidIndex(Best) ? ZoneGates[g].Zone[Best] : -1;
		if (Side < 0 || Side == ZoneNow[g]) continue;          // (in the gate: either side's set is right there; kept)
		UE_LOG(LogOutlaws, Log, TEXT("OLRT zone gate %s %d: side %d from where the body stands after %s (sector %d, was side %d)"), *LevelName, ZoneGates[g].Gate, Side, Why, Best, ZoneNow[g]);
		ZoneNow[g] = int8(Side);
		bChanged = true;
	}
	if (bChanged) RefreshZoneSkip();
	return Best;
}

void AOutlawsLevelActor::ZoneAfterMove(int32 From, int32 Now, bool bByCrossing)
{
	if (Now == From) return;
	bool bChanged = false;
	for (int32 g = 0; g < ZoneGates.Num(); ++g)
	{
		const int32 Side = ZoneGates[g].Zone.IsValidIndex(Now) ? ZoneGates[g].Zone[Now] : -1;
		if (Side < 0 || Side == ZoneNow[g]) continue;          // (into the gate, or along this side)
		// out of the gate (or through it in one tick) onto the other side; a pick by the feet can only get there when the
		// body is truly there (the other side's sectors sharing space with this side are left out of it)
		UE_LOG(LogOutlaws, Log, TEXT("OLRT zone gate %s %d: side %d -> %d, sector %d -> %d (%s)"), *LevelName, ZoneGates[g].Gate, ZoneNow[g], Side, From, Now,
			From == ZoneGates[g].Gate ? TEXT("out of the gate") : bByCrossing ? TEXT("through the gate") : TEXT("by the feet"));
		ZoneNow[g] = int8(Side);
		bChanged = true;
	}
	if (bChanged) RefreshZoneSkip();
}

void AOutlawsLevelActor::ZoneForceOff(TSet<int32>& Hide, TSet<int32>& Unseen) const
{
	for (int32 g = 0; g < ZoneGates.Num(); ++g)
	{
		if (!ZoneNow.IsValidIndex(g) || ZoneNow[g] < 0) continue;
		for (const int32 S : ZoneGates[g].Off[ZoneNow[g]])
		{
			if (!OverlapParts.Contains(S)) continue;            // (the sky round the level has no meshes of its own)
			Hide.Add(S);
			Unseen.Add(S);
		}
	}
}

int32 AOutlawsLevelActor::ClimbLandingSector(const FVector& FeetUE, const FVector& FromUE, FString* OutWhy) const
{
	// (BUG-MILLCLIMB) a climb never lands in a sector the player's own does not lead to through openings near the climb:
	// from the player's sector, three openings deep at most, through openings within 3 m of the landing or the climb's start
	if (OldMillclimb()) return -2;
	const FOutlawsLevel* Lv = Inf ? &Inf->Live : LevelData.Get();
	if (!Lv || !Lv->Sectors.IsValidIndex(PlayerSector)) return -2;
	const TBitArray<>* Skip = PlayerSkip();
	const FVector Ol = FOutlawsGeometry::ToOutlaws(FeetUE, 0.2);
	const FVector2D Land(Ol.X, Ol.Z), Start = FOutlawsGeometry::ToOutlawsPlan(FromUE);
	TArray<int32> Holders;
	for (const FOutlawsSector& S : Lv->Sectors)
	{
		if (S.Verts.Num() < 3 || FOutlawsLevel::Skipped(Skip, S.Index) || !Lv->ContainsXZ(S, Land.X, Land.Y)) continue;
		const double F = Lv->SurfaceY(S, Land, true), C = Lv->SurfaceY(S, Land, false);
		if (Ol.Y < F - 0.5 || Ol.Y > C + 0.5) continue;
		Holders.Add(S.Index);
	}
	if (Holders.Num() == 0)
	{
		const int32 W = Lv->Which3D(Ol.X, Ol.Y, Ol.Z, Skip);
		if (W >= 0) Holders.Add(W);
	}
	const double Reach = 300.0 / FOutlawsGeometry::UnitsToCm;
	TMap<int32, int32> Depth;
	TArray<int32> Queue = { PlayerSector };
	Depth.Add(PlayerSector, 0);
	for (int32 q = 0; q < Queue.Num(); ++q)
	{
		const int32 Si = Queue[q];
		const int32 D = Depth[Si];
		if (D >= 3) continue;
		const FOutlawsSector& S = Lv->Sectors[Si];
		auto Visit = [&](int32 Nx) { if (Lv->Sectors.IsValidIndex(Nx) && !FOutlawsLevel::Skipped(Skip, Nx) && !Depth.Contains(Nx)) { Depth.Add(Nx, D + 1); Queue.Add(Nx); } };
		Visit(S.VAdjoin);
		for (const FOutlawsWall& W : S.Walls)
		{
			if ((W.Adjoin < 0 && W.DAdjoin < 0) || (W.Flags1 & OutlawsFlags::WallNoPass) || !S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) continue;
			const FVector2D A = S.Verts[W.V1], B = S.Verts[W.V2];
			if (FVector2D::Distance(Land, FMath::ClosestPointOnSegment2D(Land, A, B)) > Reach && FVector2D::Distance(Start, FMath::ClosestPointOnSegment2D(Start, A, B)) > Reach) continue;
			Visit(W.Adjoin);
			Visit(W.DAdjoin);
		}
	}
	int32 Best = -1;
	for (const int32 H : Holders) if (Depth.Contains(H) && (Best < 0 || Depth[H] < Depth[Best])) Best = H;
	if (Best < 0 && OutWhy)
	{
		FString Hs;
		for (const int32 H : Holders) Hs += FString::Printf(TEXT(" %d"), H);
		*OutWhy = FString::Printf(TEXT("the landing's sectors (%s ) are not reached from sector %d through openings near the climb"), Hs.IsEmpty() ? TEXT(" none") : *Hs, PlayerSector);
	}
	return Best;
}

FString AOutlawsLevelActor::DebugZoneReport() const
{
	if (ZoneGates.Num() == 0) return TEXT("no zone gates");
	FString Out;
	for (int32 g = 0; g < ZoneGates.Num(); ++g)
	{
		const FZoneGate& G = ZoneGates[g];
		const int32 Side = ZoneNow.IsValidIndex(g) ? ZoneNow[g] : -1;
		Out += FString::Printf(TEXT("%sgate %d: side %d (player sector %d, side %d), %d off"), g ? TEXT("; ") : TEXT(""), G.Gate, Side, PlayerSector,
			G.Zone.IsValidIndex(PlayerSector) ? G.Zone[PlayerSector] : -1, Side >= 0 ? G.Off[Side].Num() : 0);
	}
	return Out;
}

#if WITH_OUTLAWS_TESTS
namespace
{
	/** Eyes over a sector for the zone scans: a grid over its plan and points just inside each of its openings (the widest
	 *  views), crouched and standing (2 m at most, under its ceiling). */
	void ZoneEyes(const FOutlawsLevel& L, const FOutlawsSector& S, TArray<FVector>& Out)
	{
		Out.Reset();
		if (S.Verts.Num() < 3) return;
		const double U = FOutlawsGeometry::UnitsToCm;
		FBox2D Box(ForceInit);
		for (const FVector2D& V : S.Verts) Box += V;
		TArray<FVector2D> Pts;
		const FVector2D Ext = Box.GetSize();
		const double Step = FMath::Max(150.0 / U, FMath::Max(Ext.X, Ext.Y) / 6.0);
		for (double y = Box.Min.Y + Step * 0.5; y < Box.Max.Y; y += Step)
			for (double x = Box.Min.X + Step * 0.5; x < Box.Max.X; x += Step) Pts.Add(FVector2D(x, y));
		const FVector2D Mid = Box.GetCenter();
		for (const FOutlawsWall& W : S.Walls)
		{
			if ((W.Adjoin < 0 && W.DAdjoin < 0) || !S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) continue;
			const FVector2D A = S.Verts[W.V1], B = S.Verts[W.V2];
			const FVector2D E = B - A;
			FVector2D N = FVector2D(-E.Y, E.X).GetSafeNormal();
			for (const double T : { 0.1, 0.5, 0.9 })
			{
				const FVector2D On = A + E * T;
				const FVector2D In = L.ContainsXZ(S, On.X + N.X * 0.3, On.Y + N.Y * 0.3) ? On + N * 0.3 : On - N * 0.3;
				Pts.Add(In);
			}
		}
		Pts.Add(Mid);
		for (const FVector2D& P : Pts)
		{
			if (!L.ContainsXZ(S, P.X, P.Y)) continue;
			const double F = L.SurfaceY(S, P, true), C = L.SurfaceY(S, P, false);
			if (C - F < 60.0 / U) continue;
			for (const double H : { 100.0, 200.0 })
			{
				Out.Add(FVector(P.X, P.Y, FMath::Min(F + H / U, C - 0.1)));
			}
		}
	}
}

void AOutlawsLevelActor::DebugZoneScan(const TSet<int32>& Targets) const
{
	const FOutlawsLevel* LP = Inf ? &Inf->Live : nullptr;
	if (!LP) return;
	const FOutlawsLevel& L = *LP;
	const double T0 = FPlatformTime::Seconds();
	int32 Eyes = 0, Lines = 0;
	TArray<FVector> E;
	for (const FOutlawsSector& S : L.Sectors)
	{
		ZoneEyes(L, S, E);
		TMap<int32, int32> Hits;
		for (const FVector& Eye : E)
		{
			TSet<int32> Seen;
			PortalFlood(L, S.Index, FVector2D(Eye.X, Eye.Y), Eye.Z, Seen);
			++Eyes;
			for (const int32 T : Targets) if (T != S.Index && Seen.Contains(T)) ++Hits.FindOrAdd(T);
		}
		if (Hits.Num() == 0) continue;
		FString H;
		TArray<int32> Keys;
		Hits.GetKeys(Keys);
		Keys.Sort();
		for (const int32 K : Keys) H += FString::Printf(TEXT(" %d(%d)"), K, Hits[K]);
		UE_LOG(LogOutlaws, Display, TEXT("OLZONESCAN %d of %d eyes:%s"), S.Index, E.Num(), *H);
		++Lines;
	}
	UE_LOG(LogOutlaws, Display, TEXT("OLZONESCAN done: %d eyes over %d sectors, %d sectors see a target, %.1f s"), Eyes, L.Sectors.Num(), Lines, FPlatformTime::Seconds() - T0);
}

int32 AOutlawsLevelActor::DebugZoneGateCheck(FString& OutReport) const
{
	OutReport.Reset();
	const FOutlawsLevel* LP = Inf ? &Inf->Live : nullptr;
	if (!LP || ZoneGates.Num() == 0) { OutReport = TEXT("no zone gates"); return 0; }
	const FOutlawsLevel& L = *LP;
	int32 Bad = 0;
	TArray<FVector> E;
	for (const FZoneGate& G : ZoneGates)
	{
		int32 Eyes = 0, BadHere = 0;
		FString Ex;
		for (const FOutlawsSector& S : L.Sectors)
		{
			const int32 Z = G.Zone[S.Index];
			if (Z < 0 && S.Index != G.Gate) continue;
			ZoneEyes(L, S, E);
			for (const FVector& Eye : E)
			{
				TSet<int32> Seen;
				PortalFlood(L, S.Index, FVector2D(Eye.X, Eye.Y), Eye.Z, Seen);
				++Eyes;
				for (int32 Side = 0; Side < 2; ++Side)
				{
					if (Z >= 0 && Side != Z) continue;              // (the gate: both sides' sets)
					for (const int32 T : G.Off[Side])
					{
						if (!Seen.Contains(T)) continue;
						++BadHere;
						if (BadHere <= 12) Ex += FString::Printf(TEXT(" [eye in %d (side %d) at %.1f,%.1f,%.1f sees %d]"), S.Index, Z, Eye.X, Eye.Y, Eye.Z, T);
						break;
					}
				}
			}
		}
		OutReport += FString::Printf(TEXT("gate %d: %d eyes, %d see a sector their side switches off%s; "), G.Gate, Eyes, BadHere, *Ex);
		Bad += BadHere;
	}
	return Bad;
}

void AOutlawsLevelActor::DebugSetZone(int32 Side)
{
	if (ZoneNow.Num() == 0) return;
	ZoneNow[0] = int8(Side);
	RefreshZoneSkip();
}
#else
void AOutlawsLevelActor::DebugZoneScan(const TSet<int32>& Targets) const {}
int32 AOutlawsLevelActor::DebugZoneGateCheck(FString& OutReport) const { return 0; }
void AOutlawsLevelActor::DebugSetZone(int32 Side) {}
#endif
