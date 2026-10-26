// Story co-op (tools/olue/COOP_DESIGN.md), 2026-10-02, the user's request: "for each additional player (incase we do 4
// player co op at some point) there should be more enemies, maybe an extra relative 50 percent per person ... make a clone of
// 50% of them, then place them 3 units away from its original clone in a random direction in a safe place in a sector (not
// near a wall).  maybe this data could be saved somewhere so it doesn't need to calculate at runtime every time".
// - Which: the level's ordinary bad guys placed at the difficulty the level is built for (FUNC BGY_New with a BGY* sprite
//   and a man's height: not a named outlaw, an outlaw's voice alone, a chicken, a townsman, a tarantula or the poodle, and
//   nothing a generator would make).  Extra = round(0.5 x (players - 1) x those).  A shuffle seeded by the level's name, the
//   difficulty and a fixed salt says whose copy comes first; a second copy of one only when more are wanted than there are.
// - Where: about 3 units (90 cm) from his original, the first of 16 ways round by the same seed, at a safe place worked out
//   from the level's sectors alone (no physics): the straight way there crosses only open, walkable openings; the floor there
//   is at the original's height and safe (no water, pit, sky, damage, current, ice or steep slope; not a door's or a lift's);
//   the body's room round it is clear of every wall, ledge, drop and door's sweep; clear of the other bad guys, the props and
//   the players' start.  All 16 ways at 3 units, then at 2, then at 4; no spot: the next original instead.  (The rules of the
//   placement prototype, checked against every wall of the nine story levels: scratchpad/clones/proto.py and verify.py.)
// - Who decides: the host, from its cache (Saved/OutlawsCache/Extras/<level>_<difficulty>_<n>p.json, kept only while the
//   level's own files are the same) or by working it out and caching it.  The list goes on the session with the start serial;
//   both machines make the copies in its order, after all the level's own bad guys, so every index (the snapshots, hits,
//   deaths, grabs, saves) is the same on both.  The client never works out a list of its own.
// - What each is (user 2026-10-02, later: "I'd like the new clones to be a bit more random.  And I don't want them to be
//   actual clones.  Replace them with all non-boss or non-special characters ... Make sure they are generally enemies that
//   are across all levels, just the standard fleet.  And just randomize between each clone instance"): a kind drawn for each
//   extra on his own from the ordinary bad guys every story level has (KindPool: BGY1 to BGY6, BGY8, BGY9 by their plain
//   ITMs), seeded by the level, the difficulty, the players and his own place in the list; never a named outlaw, a
//   townsman, a chicken, a tarantula or anything special.  He has that kind's own ITM (health, weapon, sounds, drop) and
//   sprite at the spawn difficulty's stats, the light of the bad guy he stands by, and starts as that one starts (his
//   BASE_MODE and WATCH_CHOR: a sniper's neighbour keeps his post, an ambusher's waits); his kind's drop unless that is a
//   key, a puzzle piece or anything the level's locks ask for (then no drop).  The list names the kind (ITM and sprite), so
//   the client makes exactly what the host drew.
// - The late boss (Sanchez, Two Feathers) comes at 80 % of the bad guys there are: the copies count, in all and killed.
//
// None of this runs in single player: the session's list is empty there and Build asks UOutlawsCoop::IsActive first.
#include "OutlawsCoop.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsUnlocks.h"
#include "OutlawsPlayer.h"
#include "OutlawsEnemy.h"
#include "OutlawsLab.h"
#include "OutlawsLevel.h"
#include "OutlawsFormats.h"
#include "OutlawsGeometry.h"
#include "OutlawsInf.h"
#include "OutlawsInfRuntime.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "Engine/OverlapResult.h"
#include "EngineUtils.h"
#include "CollisionQueryParams.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Crc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "TimerManager.h"

namespace OLXtra
{
	constexpr int32 AlgoVersion = 6;                 // (bump with any change to the rules below: every cache file is worked out again)
	                                                 // (6: a spread point no nearer the players' start than his original)
	                                                 // (5: REQ-SPREADSPAWN, spread over the original's own sector)
	                                                 // (2: each extra a kind of his own from KindPool, user 2026-10-02)
	constexpr double ManTall = 6.0;                  // Outlaws units (30 cm): the room a man needs over the floor
	constexpr double StepUp = 2.0, StepDown = 2.0;   // the most the floor may rise or fall on the way (the bad guys' own: 3 and 15)
	constexpr double WallRoom = 1.0;                 // every face this far beyond the body's radius
	constexpr double BodyRoom = 0.5;                 // between two bodies, beyond their radii
	constexpr double StartRoom = 10.0;               // from a player start (the second player stands 110 cm beside it)
	constexpr double SteepDeg = 30.0;
	constexpr double TryDist[3] = { 3.0, 2.0, 4.0 };
	// REQ-SPREADSPAWN (user 2026-10-06): an extra at a seeded point inside his original's own sector, this far apart from every
	// other bad guy (5 units, 1.5 m), relaxed a step at a time in a small sector (4, 3, then only clear of their bodies); no
	// point at all: the ring beside him (FindSpot)
	constexpr double SpreadGap[3] = { 5.0, 4.0, 3.0 };
	constexpr double SpreadEdge = 0.5;               // beyond WallRoom: from every line of the sector, an opening's too
	constexpr int32 SpreadTries = 96;
	constexpr uint32 SpreadSalt = 0x53505244u;        // ("SPRD")
	TArray<int32> LastSpread;                        // (-OLSpreadCheck: the last list's steps, in its order, for the check)
	// the 16 ways round, (x, z) for k x 22.5 degrees, written out (no sin or cos: the same numbers on every machine)
	constexpr double WayX[16] = { 1.0, 0.92387953251128674, 0.70710678118654757, 0.38268343236508984, 0.0, -0.38268343236508984, -0.70710678118654757, -0.92387953251128674,
		-1.0, -0.92387953251128674, -0.70710678118654757, -0.38268343236508984, 0.0, 0.38268343236508984, 0.70710678118654757, 0.92387953251128674 };
	constexpr double WayZ[16] = { 0.0, 0.38268343236508984, 0.70710678118654757, 0.92387953251128674, 1.0, 0.92387953251128674, 0.70710678118654757, 0.38268343236508984,
		0.0, -0.38268343236508984, -0.70710678118654757, -0.92387953251128674, -1.0, -0.92387953251128674, -0.70710678118654757, -0.38268343236508984 };
	// tried in this order from the first: it, the opposite, the quarters, the eighths, then the rest
	constexpr int32 WayOrder[16] = { 0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15 };
	constexpr uint32 SeedSalt = 0x4F4C5845u;          // ("OLXE")
	constexpr uint32 KindSalt = 0x4B494E44u;          // ("KIND")
	// the kinds an extra may be (user 2026-10-02, "just the standard fleet"): the ordinary bad guys' sprite families that all
	// nine story levels place (scratchpad/extras/kinds/kinds_survey.py over the LABs: BGY1 to BGY6, BGY8 and BGY9 in all
	// nine, BGY7 the rifleman in eight, not the town), each by its plain ITM (not the a, s, d, cs, wa or f variants, not the
	// key carriers).  One missing from an install is left out there (the host's log says so); the client never draws
	const TCHAR* const KindPool[] = { TEXT("bgy1"), TEXT("bgy2"), TEXT("bgy3"), TEXT("bgy4"), TEXT("bgy5"), TEXT("bgy6"), TEXT("bgy8"), TEXT("bgy9") };
	constexpr uint32 HurtFloors = OutlawsFlags::SecDamageSmall | OutlawsFlags::SecDamageLarge | OutlawsFlags::SecDamageDie | OutlawsFlags::FloorDamageSmall |
		OutlawsFlags::FloorDamageLarge | OutlawsFlags::FloorDamageDie;
	constexpr uint32 NoFooting = OutlawsFlags::Water | OutlawsFlags::ExtFloor | OutlawsFlags::NoWalls | OutlawsFlags::SecTerminate;
	constexpr uint32 ClosedOpening = OutlawsFlags::WallNoPass | OutlawsFlags::WallFence | OutlawsFlags::WallShatter;

	uint32 Fnv1a(const FString& Text)
	{
		uint32 H = 2166136261u;
		for (int32 i = 0; i < Text.Len(); ++i)
		{
			H ^= uint32(Text[i]) & 0xFFu;
			H *= 16777619u;
		}
		return H;
	}

	uint32 Mix(uint32 H)
	{
		H ^= H >> 16;
		H *= 0x85EBCA6Bu;
		H ^= H >> 13;
		H *= 0xC2B2AE35u;
		H ^= H >> 16;
		return H;
	}

	double SegDistSq(double Px, double Pz, const FVector2D& A, const FVector2D& B)
	{
		const double Ex = B.X - A.X, Ez = B.Y - A.Y;
		const double Len2 = Ex * Ex + Ez * Ez;
		double Along = 0.0;
		if (Len2 > 1e-12) Along = FMath::Clamp(((Px - A.X) * Ex + (Pz - A.Y) * Ez) / Len2, 0.0, 1.0);
		const double Dx = Px - (A.X + Ex * Along), Dz = Pz - (A.Y + Ez * Along);
		return Dx * Dx + Dz * Dz;
	}

	struct FBody { double X = 0.0, Z = 0.0, Floor = 0.0, R = 0.0; int32 Row = -1; };
	struct FPropRing { double X = 0.0, Z = 0.0, Y = 0.0, R = 0.0; };
	struct FSweep { bool bDisc = false; double A = 0.0, B = 0.0, C = 0.0, D = 0.0; };    // a disc: (A, B) and radius C; a box: A..C by B..D
	struct FOriginal { int32 Row = -1; FString Name; FString Anim; const FOutlawsSector* Sector = nullptr; double X = 0.0, Z = 0.0, Floor = 0.0, R = 1.0; };
	struct FSpot { int32 Row = -1; FString Name; FString Kind; FString Sprite; double X = 0.0, Y = 0.0, Z = 0.0; int32 YawDeg = 0; double Dist = 0.0; int32 Way = 0; int32 Round = 0;
		int32 Spread = 0; };                          // (REQ-SPREADSPAWN: 1..4 the spacing step it was placed at, 0 the ring beside him)
	struct FKind { FString Itm; FString Sprite; double R = 1.0; };

	/** One level at one difficulty, as Build will make it (the INF's live copy), and the search for safe spots in it. */
	class FPlanner
	{
	public:
		FOutlawsInfRuntime Inf;
		TSet<int32> Moving;
		TArray<FSweep> Sweeps;
		TArray<FBody> Bodies;
		TArray<FPropRing> Props;
		TArray<FVector2D> Starts;
		TArray<FOriginal> Originals;
		TArray<FBox2D> Boxes;
		TArray<FKind> Pool;                           // (KindPool as this install has it)
		TArray<FString> PoolLeftOut;                  // (... and the kinds it lacks, with why)
		TArray<FPropRing> Pickups;                    // (REQ-SPREADSPAWN: every other object on the map, a spread point keeps off it)
		TMap<FString, int32> Rejects;
		uint32 Seed = 0;
		uint32 SpreadSeed = 0;                        // (the randomizer's seed when it is on, else 0: the level's own Seed)
		bool bSpreadOff = false;                      // (-OLSpreadOff: the ring beside the original only, as before)
		FString Sources;                              // every file the answer depends on, as one fingerprint
		FString SourceFiles;                          // ... and each of the level's own: its archive, its size, its own CRC

		bool Prepare(const FOutlawsAssets& Assets, const FString& Level, EOutlawsDifficulty Diff, FString& OutWhy);
		void Search(int32 Wanted, int32 Players, TArray<FSpot>& Out);
		FString PoolText() const;

	private:
		const FOutlawsLevel& Lv() const { return Inf.Live; }
		void Reject(const TCHAR* Why) { ++Rejects.FindOrAdd(Why); }
		const TCHAR* SectorBad(const FOutlawsSector& S) const;
		const FOutlawsSector* Beyond(const FOutlawsWall& W, double YFeet) const;
		const FOutlawsSector* Walk(const FOutlawsSector& From, double X0, double Z0, double X1, double Z1, const TCHAR*& Why) const;
		const TCHAR* Clearance(const FOutlawsSector& Here, double X, double Z, double Y, double Need) const;
		const TCHAR* OtherFloorHere(const FOutlawsSector& Here, double X, double Z, double Y) const;
		const TCHAR* Crowded(double R0, double X, double Z, double Y) const;
		bool InBox(int32 Sector, double X, double Z, double Pad) const
		{
			const FBox2D& Bx = Boxes[Sector];
			return Bx.bIsValid && X >= Bx.Min.X - Pad && X <= Bx.Max.X + Pad && Z >= Bx.Min.Y - Pad && Z <= Bx.Max.Y + Pad;
		}
		bool FindSpot(const FOriginal& Of, int32 Round, double R, FSpot& Out);
		bool SpreadSpot(const FOriginal& Of, int32 Index, double R, FSpot& Out);
	};

	const TCHAR* FPlanner::SectorBad(const FOutlawsSector& S) const
	{
		if (Moving.Contains(S.Index)) return TEXT("moving");
		if (S.Flags & NoFooting) return TEXT("water, pit or sky");
		if (S.Flags & HurtFloors) return TEXT("damage");
		if (!S.Velocity.IsNearlyZero(1e-6)) return TEXT("a current");
		if (S.Friction < 0.99f) return TEXT("slippery");
		if (S.SlopeFloor.bSet && (S.Flags & OutlawsFlags::SlopedFloor) && FMath::Abs(S.SlopeFloor.Angle * (90.0 / 4096.0)) > SteepDeg) return TEXT("steep");
		return nullptr;
	}

	const FOutlawsSector* FPlanner::Beyond(const FOutlawsWall& W, double YFeet) const
	{
		// through an opening: the sector beyond it whose floor is nearest the feet (an opening may lead to two, one over the other)
		const FOutlawsSector* Best = nullptr;
		for (const int32 Ai : { W.Adjoin, W.DAdjoin })
		{
			const FOutlawsSector* Cand = Lv().Sector(Ai);
			if (Cand && (!Best || FMath::Abs(Cand->Floor.Y - YFeet) < FMath::Abs(Best->Floor.Y - YFeet))) Best = Cand;
		}
		return Best;
	}

	const FOutlawsSector* FPlanner::Walk(const FOutlawsSector& From, double X0, double Z0, double X1, double Z1, const TCHAR*& Why) const
	{
		// the straight line from the original to the spot, sector by sector: only open, walkable openings on the way
		const FOutlawsSector* S = &From;
		const double Dx = X1 - X0, Dz = Z1 - Z0;
		double TNow = 0.0;
		for (int32 Crossings = 0; Crossings < 20; ++Crossings)
		{
			double BestT = 2.0;
			const FOutlawsWall* BestW = nullptr;
			for (const FOutlawsWall& W : S->Walls)
			{
				if (!S->Verts.IsValidIndex(W.V1) || !S->Verts.IsValidIndex(W.V2)) continue;
				const FVector2D& Va = S->Verts[W.V1];
				const FVector2D& Vb = S->Verts[W.V2];
				const double Ex = Vb.X - Va.X, Ez = Vb.Y - Va.Y;
				const double Den = Dx * Ez - Dz * Ex;
				if (FMath::Abs(Den) < 1e-12) continue;
				const double TCross = ((Va.X - X0) * Ez - (Va.Y - Z0) * Ex) / Den;
				const double UCross = ((Va.X - X0) * Dz - (Va.Y - Z0) * Dx) / Den;
				if (TCross <= TNow + 1e-9 || TCross > 1.0 || UCross < 0.0 || UCross > 1.0) continue;
				if (UCross < 1e-3 || UCross > 1.0 - 1e-3)
				{
					Why = TEXT("through a corner");
					return nullptr;
				}
				if (TCross < BestT)
				{
					BestT = TCross;
					BestW = &W;
				}
			}
			if (!BestW) return S;
			if (BestW->Adjoin < 0 && BestW->DAdjoin < 0)
			{
				Why = TEXT("a wall between");
				return nullptr;
			}
			if (BestW->Flags1 & ClosedOpening)
			{
				Why = TEXT("a closed opening between");
				return nullptr;
			}
			const FVector2D Cross(X0 + Dx * BestT, Z0 + Dz * BestT);
			const double YHere = Lv().SurfaceY(*S, Cross, true);
			const FOutlawsSector* Next = Beyond(*BestW, YHere);
			if (!Next)
			{
				Why = TEXT("nothing beyond");
				return nullptr;
			}
			if (const TCHAR* Bad = SectorBad(*Next))
			{
				Why = Bad;
				return nullptr;
			}
			const double YNext = Lv().SurfaceY(*Next, Cross, true);
			if (YNext - YHere > StepUp)
			{
				Why = TEXT("a step up on the way");
				return nullptr;
			}
			if (YHere - YNext > StepDown)
			{
				Why = TEXT("a drop on the way");
				return nullptr;
			}
			if (FMath::Min(Lv().SurfaceY(*S, Cross, false), Lv().SurfaceY(*Next, Cross, false)) - FMath::Max(YHere, YNext) < ManTall * 0.95)
			{
				Why = TEXT("a low opening on the way");
				return nullptr;
			}
			S = Next;
			TNow = BestT;
		}
		Why = TEXT("too many crossings");
		return nullptr;
	}

	const TCHAR* FPlanner::Clearance(const FOutlawsSector& Here, double X, double Z, double Y, double Need) const
	{
		// every face within the body's room: an opening into a safe sector at the same height, with headroom (followed on,
		// for the faces beyond it); never a wall, a closed opening, a ledge, a drop or a low lintel
		const double Need2 = Need * Need;
		TArray<const FOutlawsSector*, TInlineAllocator<16>> Todo;
		TSet<int32> Done;
		Todo.Add(&Here);
		Done.Add(Here.Index);
		while (Todo.Num() > 0)
		{
			const FOutlawsSector* Q = Todo.Pop(false);
			for (const FOutlawsWall& W : Q->Walls)
			{
				if (!Q->Verts.IsValidIndex(W.V1) || !Q->Verts.IsValidIndex(W.V2)) continue;
				if (SegDistSq(X, Z, Q->Verts[W.V1], Q->Verts[W.V2]) >= Need2) continue;
				if (W.Adjoin < 0 && W.DAdjoin < 0) return TEXT("a wall too near");
				if (W.Flags1 & ClosedOpening) return TEXT("a closed opening too near");
				const FOutlawsSector* Next = Beyond(W, Y);
				if (!Next || SectorBad(*Next)) return TEXT("an unsafe sector too near");
				const double Ft = Lv().ContainsXZ(*Next, X, Z) ? Lv().SurfaceY(*Next, FVector2D(X, Z), true) : Next->Floor.Y;
				if (Ft - Y > StepUp) return TEXT("a ledge too near");
				if (Y - Ft > StepDown) return TEXT("a drop too near");
				if (FMath::Min(Next->Ceil.Y, Q->Ceil.Y) - FMath::Max(Ft, Y) < ManTall) return TEXT("a low lintel too near");
				if (!Done.Contains(Next->Index))
				{
					Done.Add(Next->Index);
					Todo.Add(Next);
				}
			}
		}
		// rooms over, under or beside this one that it does not open into, in the body's height: their walls too
		const double Lo = Y + 0.1, Hi = Y + ManTall;
		for (const FOutlawsSector& Q : Lv().Sectors)
		{
			if (Q.NoWalls() || !InBox(Q.Index, X, Z, Need) || !(Q.Floor.Y < Hi && Q.Ceil.Y > Lo)) continue;
			for (const FOutlawsWall& W : Q.Walls)
			{
				const bool bSolid = (W.Adjoin < 0 && W.DAdjoin < 0) || (W.Flags1 & ClosedOpening) != 0;
				if (!bSolid || !Q.Verts.IsValidIndex(W.V1) || !Q.Verts.IsValidIndex(W.V2)) continue;
				if (SegDistSq(X, Z, Q.Verts[W.V1], Q.Verts[W.V2]) < Need2) return TEXT("a wall of another room too near");
			}
		}
		// a door's or a lift's sweep (a slide's box over its stops, a swing's disc)
		for (const FSweep& Sw : Sweeps)
		{
			if (Sw.bDisc)
			{
				const double R = Sw.C + Need;
				if (FMath::Square(X - Sw.A) + FMath::Square(Z - Sw.B) < R * R) return TEXT("a door's swing");
			}
			else if (X > Sw.A - Need && X < Sw.C + Need && Z > Sw.B - Need && Z < Sw.D + Need)
			{
				return TEXT("a door's slide");
			}
		}
		return nullptr;
	}

	const TCHAR* FPlanner::OtherFloorHere(const FOutlawsSector& Here, double X, double Z, double Y) const
	{
		// another sector holding this spot within the body's height (a room inside a room), or water at the feet
		for (const FOutlawsSector& T : Lv().Sectors)
		{
			if (T.Index == Here.Index || !InBox(T.Index, X, Z, 0.0) || !Lv().ContainsXZ(T, X, Z)) continue;
			const double F = Lv().SurfaceY(T, FVector2D(X, Z), true), Ce = Lv().SurfaceY(T, FVector2D(X, Z), false);
			if ((T.Flags & OutlawsFlags::Water) && FMath::Abs(Ce - Y) < 1.0) return TEXT("water at the feet");
			if (F < Y + ManTall && Ce > Y + 0.25 && F > Y + 0.25) return TEXT("a floor inside the body");
		}
		return nullptr;
	}

	const TCHAR* FPlanner::Crowded(double R0, double X, double Z, double Y) const
	{
		// room from every body (his own original's too, and the copies already placed), every prop, the players' start
		for (const FBody& B : Bodies)
		{
			if (FMath::Abs(B.Floor - Y) >= ManTall) continue;
			const double R = B.R + R0 + BodyRoom;
			if (FMath::Square(B.X - X) + FMath::Square(B.Z - Z) < R * R) return TEXT("another bad guy");
		}
		for (const FPropRing& P : Props)
		{
			if (FMath::Abs(P.Y - Y) >= ManTall) continue;
			const double R = P.R + R0 + BodyRoom;
			if (FMath::Square(P.X - X) + FMath::Square(P.Z - Z) < R * R) return TEXT("a prop");
		}
		for (const FVector2D& S : Starts)
		{
			if (FMath::Square(S.X - X) + FMath::Square(S.Y - Z) < StartRoom * StartRoom) return TEXT("the players' start");
		}
		return nullptr;
	}

	bool FPlanner::FindSpot(const FOriginal& Of, int32 Round, double R, FSpot& Out)
	{
		// (R: the body's radius, the larger of his own kind's and the bad guy's he stands by)
		const uint32 H = Mix(Seed ^ (uint32(Of.Row) * 0x85EBCA6Bu) ^ (uint32(Round + 1) * 0xC2B2AE35u));
		const int32 First = int32(H % 16u);
		const double Need = R + WallRoom;
		for (const double Dist : TryDist)
		{
			for (int32 j = 0; j < 16; ++j)
			{
				const int32 K = (First + WayOrder[j]) % 16;
				// (to a ten-thousandth of a unit: the cache's numbers are the search's own)
				const double X1 = FMath::RoundToDouble((Of.X + WayX[K] * Dist) * 10000.0) / 10000.0;
				const double Z1 = FMath::RoundToDouble((Of.Z + WayZ[K] * Dist) * 10000.0) / 10000.0;
				const TCHAR* Why = nullptr;
				const FOutlawsSector* End = Walk(*Of.Sector, Of.X, Of.Z, X1, Z1, Why);
				if (!End) { Reject(Why); continue; }
				if ((Why = SectorBad(*End)) != nullptr) { Reject(Why); continue; }
				const double Y1 = Lv().SurfaceY(*End, FVector2D(X1, Z1), true);
				if (FMath::Abs(Y1 - Of.Floor) > StepUp) { Reject(TEXT("not the original's floor")); continue; }
				if (Lv().SurfaceY(*End, FVector2D(X1, Z1), false) - Y1 < ManTall + 0.2) { Reject(TEXT("no headroom")); continue; }
				if ((Why = OtherFloorHere(*End, X1, Z1, Y1)) != nullptr) { Reject(Why); continue; }
				if ((Why = Clearance(*End, X1, Z1, Y1, Need)) != nullptr) { Reject(Why); continue; }
				if ((Why = Crowded(R, X1, Z1, Y1)) != nullptr) { Reject(Why); continue; }
				Out.Row = Of.Row;
				Out.Name = Of.Name;
				Out.X = X1;
				Out.Y = Y1;
				Out.Z = Z1;
				Out.Dist = Dist;
				Out.Way = K;
				Out.Round = Round;
				Out.YawDeg = int32(Mix(H ^ 0x68E31DA4u) % 360u);           // (facing a way of his own)
				return true;
			}
		}
		return false;
	}

	bool FPlanner::SpreadSpot(const FOriginal& Of, int32 Index, double R, FSpot& Out)
	{
		// REQ-SPREADSPAWN: a seeded point inside the original's own sector, on its own flat floor, clear of every line of it
		// (its openings too: doors, steps, ledges), of other rooms, doors' sweeps, props, pickups and the players' start; of
		// the points that pass, the one farthest from every bad guy, at 5, 4 or 3 units at the least.  False: the ring.
		if (bSpreadOff || !Of.Sector) return false;
		const FOutlawsSector& S = *Of.Sector;
		if (SectorBad(S) || (S.Flags & OutlawsFlags::SlopedFloor) || S.SlopeFloor.bSet)
		{
			Reject(TEXT("spread: a moving, sloped or unsafe sector"));
			return false;
		}
		const double Y = S.Floor.Y;
		if (FMath::Abs(Of.Floor - Y) > 0.05 || !Boxes.IsValidIndex(S.Index) || !Boxes[S.Index].bIsValid)
		{
			Reject(TEXT("spread: not on the sector's own floor"));
			return false;
		}
		const double Need = R + WallRoom + SpreadEdge;
		const double Need2 = Need * Need;
		const FBox2D& Bx = Boxes[S.Index];
		const double W = Bx.Max.X - Bx.Min.X - 2.0 * Need, D = Bx.Max.Y - Bx.Min.Y - 2.0 * Need;
		if (W <= 0.0 || D <= 0.0)
		{
			Reject(TEXT("spread: a sector too small"));
			return false;
		}
		// no nearer the players' start than his original stands (2026-10-06: spread toward it in a start room, an extra was
		// on the players within 3 s, shooting before the start autosave could be made)
		double OfStart = 1e30;
		for (const FVector2D& St : Starts) OfStart = FMath::Min(OfStart, FMath::Sqrt(FMath::Square(St.X - Of.X) + FMath::Square(St.Y - Of.Z)));
		const double StartNeed = FMath::Max(StartRoom, OfStart < 1e29 ? OfStart : 0.0);
		const uint32 Base = Mix((SpreadSeed ? Mix(SpreadSeed ^ SpreadSalt) ^ Seed : Seed ^ SpreadSalt) + uint32(Index) * 0x9E3779B1u);
		double BestGap = -1.0, BestX = 0.0, BestZ = 0.0;
		uint32 BestH = 0;
		for (int32 t = 0; t < SpreadTries; ++t)
		{
			const uint32 H = Mix(Base ^ (uint32(t + 1) * 0x85EBCA6Bu));
			const uint32 H2 = Mix(H ^ 0x27D4EB2Fu);
			// (to a ten-thousandth of a unit, from integers only: the same numbers on every machine and in the cache)
			const double X = FMath::RoundToDouble((Bx.Min.X + Need + W * double(H & 0xFFFFu) / 65535.0) * 10000.0) / 10000.0;
			const double Z = FMath::RoundToDouble((Bx.Min.Y + Need + D * double(H2 & 0xFFFFu) / 65535.0) * 10000.0) / 10000.0;
			if (!Lv().ContainsXZ(S, X, Z)) { Reject(TEXT("spread: outside the sector")); continue; }
			bool bNear = false;
			for (const FOutlawsWall& Wl : S.Walls)
			{
				if (!S.Verts.IsValidIndex(Wl.V1) || !S.Verts.IsValidIndex(Wl.V2)) continue;
				if (SegDistSq(X, Z, S.Verts[Wl.V1], S.Verts[Wl.V2]) < Need2) { bNear = true; break; }
			}
			if (bNear) { Reject(TEXT("spread: a line of the sector too near")); continue; }
			if (Lv().SurfaceY(S, FVector2D(X, Z), false) - Y < ManTall + 0.2) { Reject(TEXT("spread: no headroom")); continue; }
			const TCHAR* Why = nullptr;
			if ((Why = OtherFloorHere(S, X, Z, Y)) != nullptr || (Why = Clearance(S, X, Z, Y, Need)) != nullptr) { Reject(Why); continue; }
			// no other sector's floor, ceiling or body in his height at his middle or round his body's edge (a beam, a
			// platform or a room-in-a-room whose walls are not lines of this one)
			bool bSolid = false;
			for (int32 k = -1; k < 8 && !bSolid; ++k)
			{
				const double Px = k < 0 ? X : X + WayX[k * 2] * Need, Pz = k < 0 ? Z : Z + WayZ[k * 2] * Need;
				for (const FOutlawsSector& T : Lv().Sectors)
				{
					if (T.Index == S.Index || T.NoWalls() || !InBox(T.Index, Px, Pz, 0.0) || !Lv().ContainsXZ(T, Px, Pz)) continue;
					const double F = Lv().SurfaceY(T, FVector2D(Px, Pz), true), Ce = Lv().SurfaceY(T, FVector2D(Px, Pz), false);
					if (F < Y + ManTall && Ce > Y + 0.25) { bSolid = true; break; }
				}
			}
			if (bSolid) { Reject(TEXT("spread: another sector in his height")); continue; }
			bool bBlocked = false;
			for (const TArray<FPropRing>* List : { &Props, &Pickups })
			{
				for (const FPropRing& P : *List)
				{
					if (FMath::Abs(P.Y - Y) >= ManTall) continue;
					const double Rr = P.R + R + BodyRoom;
					if (FMath::Square(P.X - X) + FMath::Square(P.Z - Z) < Rr * Rr) { bBlocked = true; break; }
				}
				if (bBlocked) break;
			}
			if (bBlocked) { Reject(TEXT("spread: a prop or pickup")); continue; }
			bool bStart = false;
			for (const FVector2D& St : Starts) bStart |= FMath::Square(St.X - X) + FMath::Square(St.Y - Z) < StartNeed * StartNeed;
			if (bStart) { Reject(TEXT("spread: the players' start")); continue; }
			double Gap = 1e30;
			bool bTouch = false;
			for (const FBody& B : Bodies)
			{
				if (FMath::Abs(B.Floor - Y) >= ManTall) continue;
				const double Dd = FMath::Sqrt(FMath::Square(B.X - X) + FMath::Square(B.Z - Z));
				bTouch |= Dd < B.R + R + BodyRoom;
				Gap = FMath::Min(Gap, Dd);
			}
			if (bTouch) { Reject(TEXT("spread: another bad guy")); continue; }
			if (Gap > BestGap)
			{
				BestGap = Gap;
				BestX = X;
				BestZ = Z;
				BestH = H;
			}
		}
		int32 Step = 0;
		for (int32 k = 0; k < 3 && Step == 0; ++k) Step = BestGap >= SpreadGap[k] ? k + 1 : 0;
		if (Step == 0 && BestGap >= 0.0) Step = 4;       // (a small sector: any point that passed, his body clear of theirs)
		if (Step == 0)
		{
			Reject(TEXT("spread: no point passed"));
			return false;
		}
		Out.Row = Of.Row;
		Out.Name = Of.Name;
		Out.X = BestX;
		Out.Y = Y;
		Out.Z = BestZ;
		Out.Dist = FMath::Sqrt(FMath::Square(BestX - Of.X) + FMath::Square(BestZ - Of.Z));
		Out.Way = 0;
		Out.Round = 0;
		Out.Spread = Step;
		Out.YawDeg = int32(Mix(BestH ^ 0x68E31DA4u) % 360u);
		return true;
	}

	bool FPlanner::Prepare(const FOutlawsAssets& Assets, const FString& Level, EOutlawsDifficulty Diff, FString& OutWhy)
	{
		FString Lvt, Obt, InfText;
		if (!Assets.GetText(Level + TEXT(".lvt"), Lvt))
		{
			OutWhy = Level + TEXT(".lvt not found");
			return false;
		}
		Assets.GetText(Level + TEXT(".obt"), Obt);
		Assets.GetText(Level + TEXT(".inf"), InfText);
		{
			FOutlawsLevel Parsed;
			Parsed.ParseLvt(Lvt);
			Parsed.ParseObt(Obt);
			Inf.Init(Parsed, InfText);              // (as Build does: the level with its doors and lifts at their first stops)
		}
		const FOutlawsLevel& L = Lv();
		// the fingerprint: the rules' version, the level's own files, then every item and model file its objects use
		uint32 Crc = FCrc::StrCrc32(*FString::Printf(TEXT("algo %d"), AlgoVersion));
		SourceFiles.Reset();
		for (const TPair<const TCHAR*, const FString*>& F : { TPair<const TCHAR*, const FString*>(TEXT(".lvt"), &Lvt), TPair<const TCHAR*, const FString*>(TEXT(".obt"), &Obt),
			TPair<const TCHAR*, const FString*>(TEXT(".inf"), &InfText) })
		{
			const uint32 Own = FCrc::StrCrc32(**F.Value);
			Crc = FCrc::StrCrc32(**F.Value, Crc);
			SourceFiles += FString::Printf(TEXT("%s%s %s %d %08x; "), *Level, F.Key, *Assets.Where(Level + F.Key), F.Value->Len(), Own);
		}
		Boxes.SetNum(L.Sectors.Num());
		for (int32 s = 0; s < L.Sectors.Num(); ++s)
		{
			FBox2D Bx(ForceInit);
			for (const FVector2D& V : L.Sectors[s].Verts) Bx += V;
			Boxes[s] = Bx;
		}
		// what moves (a door's or a lift's sectors, a door-flagged sector), and where a door's slide or swing reaches
		for (const FOutlawsElevator& E : Inf.Elevators)
		{
			if (E.IsScroll()) continue;
			for (const int32 s : E.Sectors) Moving.Add(s);
			if (!E.IsMorph() || !Inf.Inf.Items.IsValidIndex(E.Item) || !Inf.Inf.Items[E.Item].Classes.IsValidIndex(E.ClassIndex)) continue;
			const FOutlawsInfClass& C = Inf.Inf.Items[E.Item].Classes[E.ClassIndex];
			double Reach = 0.0;
			for (int32 i = 0; i < C.Stops.Num(); ++i) Reach = FMath::Max(Reach, FMath::Abs(Inf.StopValue(E, i)));
			for (const int32 s : E.Sectors)
			{
				const FOutlawsSector* S = L.Sector(s);
				if (!S || S->Verts.Num() == 0) continue;
				FSweep Sw;
				if (E.Class == EOutlawsInfClass::MorphSpin)
				{
					Sw.bDisc = true;
					Sw.A = C.Center.X;
					Sw.B = C.Center.Y;
					for (const FVector2D& V : S->Verts) Sw.C = FMath::Max(Sw.C, FVector2D::Distance(V, C.Center));
				}
				else
				{
					const double Rad = FMath::DegreesToRadians(C.Angle);
					const double Ox = FMath::Sin(Rad) * Reach, Oz = FMath::Cos(Rad) * Reach;
					Sw.A = Sw.B = 1e30;
					Sw.C = Sw.D = -1e30;
					for (const FVector2D& V : S->Verts)
					{
						Sw.A = FMath::Min3(Sw.A, V.X - FMath::Abs(Ox), V.X + FMath::Abs(Ox));
						Sw.C = FMath::Max3(Sw.C, V.X - FMath::Abs(Ox), V.X + FMath::Abs(Ox));
						Sw.B = FMath::Min3(Sw.B, V.Y - FMath::Abs(Oz), V.Y + FMath::Abs(Oz));
						Sw.D = FMath::Max3(Sw.D, V.Y - FMath::Abs(Oz), V.Y + FMath::Abs(Oz));
					}
				}
				Sweeps.Add(Sw);
			}
		}
		for (const FOutlawsSector& S : L.Sectors)
		{
			if (S.Flags & OutlawsFlags::SectorDoor) Moving.Add(S.Index);
		}
		// the objects placed for this difficulty on the map: every man (the bad guys, the named outlaws in each of their
		// places, the chickens and the townsfolk) is a body to keep clear of; the ordinary bad guys are the ones to copy
		// (the boss functions as Build has them, OutlawsLevelActor.cpp)
		static const TSet<FString> OutlawFuncs = { TEXT("sanchez_new"), TEXT("twofeathers_new"), TEXT("bloodeye_new"), TEXT("bob_new"), TEXT("buckshot_new"),
			TEXT("drdeath_new"), TEXT("dynamite_new"), TEXT("farmer_new"), TEXT("mary_new"), TEXT("simms_new"), TEXT("slim_new"),
			TEXT("shbill_new"), TEXT("shblood_new"), TEXT("shchez_new"), TEXT("shdick_new"), TEXT("shmary_new") };
		struct FItmInfo { FString Anim, Func; TMap<FString, FString> Data; };
		TMap<FString, FItmInfo> ItmOf;
		TMap<FString, double> ModelRadius;
		TMap<FString, uint32> UsedFiles;              // (name -> its text's CRC: into the fingerprint in name order)
		const uint32 DiffBit = OutlawsDifficulty::ObjectBit(Diff);
		for (int32 Row = 0; Row < L.Objects.Num(); ++Row)
		{
			const FOutlawsObject& O = L.Objects[Row];
			if (!O.OnDifficulty(DiffBit)) continue;
			const FOutlawsSector* Sec = L.SectorById(O.SectorId);
			if (!Sec) Sec = L.SectorAt(O.Pos.X, O.Pos.Y, O.Pos.Z);
			if (!Sec) continue;                       // (off the map: a scripted scene's staging, never built)
			if (O.Name == TEXT("PLAYER"))
			{
				Starts.Add(FVector2D(O.Pos.X, O.Pos.Z));
				continue;
			}
			FItmInfo* I = ItmOf.Find(O.Name);
			if (!I)
			{
				I = &ItmOf.Add(O.Name);
				FString Itm;
				I->Anim = OutlawsFormats::ResolveAnim(Assets, O.Name, &Itm);
				OutlawsFormats::ParseItm(Itm, I->Func, I->Data);
				I->Func = I->Func.ToLower();
				UsedFiles.Add(O.Name.ToLower() + TEXT(".itm"), FCrc::StrCrc32(*Itm));
			}
			const double Height = I->Data.Contains(TEXT("HEIGHT")) ? FCString::Atod(*I->Data[TEXT("HEIGHT")]) : 6.0;
			const double Radius = FMath::Clamp(FMath::Abs(I->Data.Contains(TEXT("RADIUS")) ? FCString::Atod(*I->Data[TEXT("RADIUS")]) : 1.0), 22.0 / 30.0, 2.0);
			const bool bMan = I->Func == TEXT("bgy_new") || I->Func == TEXT("chicken_new") || I->Func == TEXT("civilian_new") || OutlawFuncs.Contains(I->Func);
			if (bMan)
			{
				FBody B;
				B.X = O.Pos.X;
				B.Z = O.Pos.Z;
				B.Floor = L.SurfaceY(*Sec, FVector2D(O.Pos.X, O.Pos.Z), true);
				B.R = Radius;
				B.Row = Row;
				Bodies.Add(B);
				const FString Stem = FPaths::GetBaseFilename(I->Anim).ToLower();
				if (I->Func == TEXT("bgy_new") && Height >= 4.5 && I->Anim.EndsWith(TEXT(".nwx"), ESearchCase::IgnoreCase) && Stem.StartsWith(TEXT("bgy")))
				{
					FOriginal Of;
					Of.Row = Row;
					Of.Name = O.Name;
					Of.Anim = I->Anim;
					Of.Sector = Sec;
					Of.X = O.Pos.X;
					Of.Z = O.Pos.Z;
					Of.Floor = B.Floor;
					Of.R = Radius;
					Originals.Add(Of);
				}
				continue;
			}
			if (I->Func == TEXT("gen_new")) continue;
			{
				FPropRing P;
				P.X = O.Pos.X;
				P.Z = O.Pos.Z;
				P.Y = L.SurfaceY(*Sec, FVector2D(O.Pos.X, O.Pos.Z), true);
				P.R = FMath::Max(Radius, 1.0);
				Pickups.Add(P);
			}
			if (I->Anim.EndsWith(TEXT(".3do"), ESearchCase::IgnoreCase))
			{
				if (I->Func == TEXT("shot_projectiledispatch")) continue;     // (a stick of dynamite on the ground)
				const FString Key = I->Anim.ToLower();
				double* Have = ModelRadius.Find(Key);
				if (!Have)
				{
					// its reach round its middle, from its own vertices (Outlaws units)
					FString Text;
					OutlawsFormats::F3doModel Model;
					double Reach = 0.0;
					if (Assets.GetText(I->Anim, Text) && OutlawsFormats::Parse3do(Text, Model))
					{
						for (const OutlawsFormats::F3doObject& Ob : Model.Objects)
						{
							for (const FVector3f& V : Ob.Verts) Reach = FMath::Max(Reach, FMath::Sqrt(double(V.X) * double(V.X) + double(V.Z) * double(V.Z)));
						}
					}
					UsedFiles.Add(Key, FCrc::StrCrc32(*Text));
					Have = &ModelRadius.Add(Key, Reach);
				}
				FPropRing P;
				P.X = O.Pos.X;
				P.Z = O.Pos.Z;
				P.Y = O.Pos.Y;
				P.R = *Have;
				Props.Add(P);
			}
			else if (I->Anim.EndsWith(TEXT(".nwx"), ESearchCase::IgnoreCase) && I->Data.Contains(TEXT("RADIUS")) && Height > 0.0 && I->Data.Contains(TEXT("HEIGHT"))
				&& (I->Func == TEXT("inv_object") || I->Func == TEXT("null")))
			{
				FPropRing P;
				P.X = O.Pos.X;
				P.Z = O.Pos.Z;
				P.Y = O.Pos.Y;
				P.R = FMath::Abs(FCString::Atod(*I->Data[TEXT("RADIUS")]));
				Props.Add(P);
			}
		}
		// the kinds an extra may be, as this install has them (each one's ITM into the fingerprint too)
		for (const TCHAR* K : KindPool)
		{
			FString Itm, Func;
			TMap<FString, FString> Data;
			const FString Anim = OutlawsFormats::ResolveAnim(Assets, K, &Itm);
			OutlawsFormats::ParseItm(Itm, Func, Data);
			const TCHAR* Lack = Itm.IsEmpty() ? TEXT("no ITM") : !Func.Equals(TEXT("BGY_New"), ESearchCase::IgnoreCase) ? TEXT("not an ordinary bad guy's ITM")
				: (!Anim.EndsWith(TEXT(".nwx"), ESearchCase::IgnoreCase) || !Assets.Has(Anim)) ? TEXT("no sprite") : nullptr;
			if (Lack)
			{
				PoolLeftOut.Add(FString::Printf(TEXT("%s (%s)"), K, Lack));
				continue;
			}
			UsedFiles.Add(FString(K) + TEXT(".itm"), FCrc::StrCrc32(*Itm));
			FKind Kd;
			Kd.Itm = K;
			Kd.Sprite = Anim.ToUpper();
			Kd.R = FMath::Clamp(FMath::Abs(Data.Contains(TEXT("RADIUS")) ? FCString::Atod(*Data[TEXT("RADIUS")]) : 1.0), 22.0 / 30.0, 2.0);
			Pool.Add(Kd);
		}
		Crc = FCrc::StrCrc32(*PoolText(), Crc);
		UsedFiles.KeySort([](const FString& A, const FString& B) { return A < B; });
		for (const TPair<FString, uint32>& Kv : UsedFiles)
		{
			Crc = FCrc::StrCrc32(*Kv.Key, Crc);
			Crc = FCrc::MemCrc32(&Kv.Value, sizeof(Kv.Value), Crc);
		}
		Sources = FString::Printf(TEXT("%08x-%d"), Crc, UsedFiles.Num());
		Seed = Fnv1a(Level.ToLower()) ^ (uint32(int32(Diff) + 1) * 0x9E3779B1u) ^ SeedSalt;
		return true;
	}

	FString FPlanner::PoolText() const
	{
		TArray<FString> Names;
		for (const FKind& K : Pool) Names.Add(K.Itm);
		return FString::Join(Names, TEXT(","));
	}

	void FPlanner::Search(int32 Wanted, int32 Players, TArray<FSpot>& Out)
	{
		Out.Reset();
		if (Wanted <= 0 || Originals.Num() == 0) return;
		// whose copy comes first: a shuffle seeded by the level's name and the difficulty
		TArray<int32> Order;
		for (int32 i = 0; i < Originals.Num(); ++i) Order.Add(i);
		uint32 State = Seed ? Seed : 1u;
		for (int32 i = Order.Num() - 1; i > 0; --i)
		{
			State ^= State << 13;
			State ^= State >> 17;
			State ^= State << 5;
			Order.Swap(i, int32(State % uint32(i + 1)));
		}
		// a second extra beside one only when more are wanted than there are (four players: 150 %); R9 (user 2026-10-03,
		// option B): and when the first round came short (a bad guy with no safe spot beside him: three players wanted one
		// each), more rounds in the same seeded order put the rest beside others as second extras, until the count is met
		// or a round finds no room at all
		const int32 Rounds = FMath::DivideAndRoundUp(Wanted, Originals.Num());
		for (int32 Round = 0; Out.Num() < Wanted && Round < Rounds + 3; ++Round)
		{
			const int32 Before = Out.Num();
			for (const int32 j : Order)
			{
				if (Out.Num() >= Wanted) break;
				// his kind first (it sets the room he needs): drawn for his place in the list, so the same place draws the
				// same kind whichever bad guy he ends up beside (user 2026-10-02: "just randomize between each clone instance")
				const FOriginal& Of = Originals[j];
				const FKind* Kind = nullptr;
				if (Pool.Num() > 0)
				{
					const uint32 H = Mix(Mix(Mix(Seed ^ KindSalt) + uint32(Players)) + uint32(Out.Num()));
					Kind = &Pool[int32(H % uint32(Pool.Num()))];
				}
				const double R = Kind ? FMath::Max(Of.R, Kind->R) : Of.R;
				FSpot Spot;
				if (!SpreadSpot(Of, Out.Num(), R, Spot) && !FindSpot(Of, Round, R, Spot)) continue;
				Spot.Round = Round;
				// (no kind in this install at all: the kind of the bad guy he stands by, as before)
				Spot.Kind = Kind ? Kind->Itm : Of.Name.ToLower();
				Spot.Sprite = Kind ? Kind->Sprite : Of.Anim.ToUpper();
				Out.Add(Spot);
				FBody B;
				B.X = Spot.X;
				B.Z = Spot.Z;
				B.Floor = Spot.Y;
				B.R = R;
				B.Row = Spot.Row;
				Bodies.Add(B);                         // (the next ones keep clear of him too)
			}
			if (Round >= Rounds - 1 && Out.Num() == Before) break;
		}
	}

	// (R14: the lists are by the bad guys' multiplier, as hundredths: 150 = x1.50, the old two players)
	FString CachePath(const FString& Level, EOutlawsDifficulty Diff, int32 MultX100, uint32 RandoSeed = 0)
	{
		// (REQ-SPREADSPAWN: the randomizer's seed, when it is on, picks the spread points: a list of its own)
		return FPaths::ProjectSavedDir() / TEXT("OutlawsCache") / TEXT("Extras") / FString::Printf(TEXT("%s_%s_x%d.%02d%s.json"), *Level.ToLower(), *FString(OutlawsDifficulty::Name(Diff)).ToLower(),
			MultX100 / 100, MultX100 % 100, RandoSeed ? *FString::Printf(TEXT("_r%u"), RandoSeed) : TEXT(""));
	}

	bool ReadCache(const FString& Path, const FString& Sources, int32 Players, int32 Eligible, const FString& Pool, TArray<FSpot>& Out, FString& Why)
	{
		Out.Reset();
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			Why = TEXT("no cache file yet");
			return false;
		}
		TSharedPtr<FJsonObject> O;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, O) || !O.IsValid())
		{
			Why = TEXT("the cache file is not readable");
			return false;
		}
		if (O->GetStringField(TEXT("kind")) != TEXT("olue-coop-extras") || int32(O->GetNumberField(TEXT("algo"))) != AlgoVersion)
		{
			Why = TEXT("the cache file is from other rules");
			return false;
		}
		if (O->GetStringField(TEXT("sources")) != Sources || int32(O->GetNumberField(TEXT("players"))) != Players || int32(O->GetNumberField(TEXT("eligible"))) != Eligible
			|| O->GetStringField(TEXT("pool")) != Pool)
		{
			Why = TEXT("the level's files are not the ones the cache was made from");
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
		if (!O->TryGetArrayField(TEXT("extras"), List))
		{
			Why = TEXT("the cache file has no list");
			return false;
		}
		for (const TSharedPtr<FJsonValue>& V : *List)
		{
			const TSharedPtr<FJsonObject> J = V.IsValid() ? V->AsObject() : nullptr;
			if (!J.IsValid())
			{
				Out.Reset();
				Why = TEXT("the cache file's list is not readable");
				return false;
			}
			FSpot S;
			S.Row = int32(J->GetNumberField(TEXT("row")));
			S.Name = J->GetStringField(TEXT("name"));
			S.Kind = J->GetStringField(TEXT("kind"));
			S.Sprite = J->GetStringField(TEXT("sprite"));
			if (S.Kind.IsEmpty() || S.Sprite.IsEmpty())
			{
				Out.Reset();
				Why = TEXT("the cache file's list has no kinds");
				return false;
			}
			S.X = J->GetNumberField(TEXT("x"));
			S.Y = J->GetNumberField(TEXT("y"));
			S.Z = J->GetNumberField(TEXT("z"));
			S.YawDeg = int32(J->GetNumberField(TEXT("yaw")));
			S.Dist = J->GetNumberField(TEXT("dist"));
			S.Way = int32(J->GetNumberField(TEXT("way")));
			S.Round = int32(J->GetNumberField(TEXT("round")));
			double Sp = 0.0;
			S.Spread = J->TryGetNumberField(TEXT("spread"), Sp) ? int32(Sp) : 0;
			Out.Add(S);
		}
		return true;
	}

	bool WriteCache(const FString& Path, const FString& Level, EOutlawsDifficulty Diff, int32 Players, const FPlanner& Plan, int32 Wanted, const TArray<FSpot>& Spots)
	{
		const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("kind"), TEXT("olue-coop-extras"));
		O->SetNumberField(TEXT("algo"), AlgoVersion);
		O->SetStringField(TEXT("about"), TEXT("OutlawsUE co-op: the extra bad guys for the players beyond the first, worked out from this machine's own game files; "
			"remade whenever those files change (delete it to have it worked out again). Each stands beside the level's bad guy at OBT row 'row' ('name') and is of "
			"the kind 'kind' (its ITM; 'sprite' its NWX), drawn from 'pool'. x, y (the floor), z in Outlaws units of 30 cm, yaw in degrees."));
		O->SetStringField(TEXT("level"), Level);
		O->SetStringField(TEXT("difficulty"), FString(OutlawsDifficulty::Name(Diff)).ToLower());
		O->SetNumberField(TEXT("players"), Players);              // (R14: the multiplier in hundredths)
		O->SetNumberField(TEXT("unit_cm"), 30);
		O->SetStringField(TEXT("sources"), Plan.Sources);
		O->SetStringField(TEXT("source_files"), Plan.SourceFiles);
		O->SetNumberField(TEXT("eligible"), Plan.Originals.Num());
		O->SetStringField(TEXT("pool"), Plan.PoolText());
		O->SetNumberField(TEXT("wanted"), Wanted);
		TArray<TSharedPtr<FJsonValue>> List;
		for (const FSpot& S : Spots)
		{
			const TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
			J->SetNumberField(TEXT("row"), S.Row);
			J->SetStringField(TEXT("name"), S.Name);
			J->SetStringField(TEXT("kind"), S.Kind);
			J->SetStringField(TEXT("sprite"), S.Sprite);
			J->SetNumberField(TEXT("x"), S.X);
			J->SetNumberField(TEXT("y"), S.Y);
			J->SetNumberField(TEXT("z"), S.Z);
			J->SetNumberField(TEXT("yaw"), S.YawDeg);
			J->SetNumberField(TEXT("dist"), S.Dist);
			J->SetNumberField(TEXT("way"), S.Way);
			J->SetNumberField(TEXT("round"), S.Round);
			J->SetNumberField(TEXT("spread"), S.Spread);
			List.Add(MakeShared<FJsonValueObject>(J));
		}
		O->SetArrayField(TEXT("extras"), List);
		FString Text;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
		if (!FJsonSerializer::Serialize(O, Writer)) return false;
		// (written beside it first and moved over it: a game closed half way never leaves half a file)
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		const FString Tmp = Path + TEXT(".tmp");
		if (!FFileHelper::SaveStringToFile(Text, *Tmp)) return false;
		return IFileManager::Get().Move(*Path, *Tmp, true, true);
	}
}

// ---- the host: the list, with each level start -----------------------------------------------------------------------------

int32 UOutlawsCoop::PlayerCount() const
{
	int32 N = 0;
	if (UWorld* W = CurrentWorld())
	{
		for (TActorIterator<AOutlawsCoopLink> It(W); It; ++It) ++N;
	}
	return FMath::Max(1, N);
}

bool UOutlawsCoop::IsBadGuysAuto() const
{
	const AOutlawsLevelActor* L = FindLevel();
	return TestBadGuys <= 0.f && LoadMultiplier <= 0.f && (!L || L->CoopBadGuys <= 0.f);
}

float UOutlawsCoop::BadGuysMultiplier() const
{
	if (IsMatch()) return 1.f;                       // (M1: a match has no extra bad guys)
	if (TestBadGuys > 0.f) return FMath::Clamp(TestBadGuys, 1.f, 4.f);
	if (LoadMultiplier > 0.f) return FMath::Clamp(LoadMultiplier, 1.f, 4.f);
	const AOutlawsLevelActor* L = FindLevel();
	// (the host's own choice; UNLOCKFREE: every host's from the first start, no Ugly finish needed)
	if (L && L->CoopBadGuys > 0.f) return FMath::Clamp(L->CoopBadGuys, 1.f, 4.f);
	// Auto (R7: a host's level is made for two at least, so the extra bad guys are there when a friend joins later; R14 and
	// the user 2026-10-03: never past four times)
	const int32 Players = FMath::Max(PlayersExpected, PlayerCount());
	return FMath::Min(4.f, 1.f + 0.5f * float(Players - 1));
}

namespace OLXtra
{
	/** The extra bad guys' list for one level at one difficulty and multiplier (the co-op host's, and single player's own,
	 *  REQ-SPMULT): the planner's spots (read from the cache when it holds them), logged.  Returns how many were eligible. */
	static int32 ListExtras(const AOutlawsLevelActor* L, const FString& Lv, EOutlawsDifficulty Diff, float Mult, int32 PlayersNow, int32 Players,
		const TCHAR* Whose, TArray<FOutlawsCoopExtra>& Out, FString& How)
	{
		const FString Root = FOutlawsAssets::FindInstallRoot(L ? L->InstallPath : FString());
		TSharedPtr<FOutlawsAssets> Assets;
		if (!Root.IsEmpty()) Assets = FOutlawsAssets::GetShared(Root);
		if (!Assets)
		{
			UE_LOG(LogOutlawsCoop, Warning, TEXT("OLCOOP extra bad guys: none for %s (the game's files were not found)"), *Lv);
			return 0;
		}
		const double T0 = FPlatformTime::Seconds();
		const TUniquePtr<OLXtra::FPlanner> Plan = MakeUnique<OLXtra::FPlanner>();
		FString Why;
		if (!Plan->Prepare(*Assets, Lv, Diff, Why))
		{
			UE_LOG(LogOutlawsCoop, Warning, TEXT("OLCOOP extra bad guys: none for %s (%s)"), *Lv, *Why);
			return 0;
		}
		const int32 Eligible = Plan->Originals.Num();
		const int32 Wanted = FMath::RoundToInt(double(Mult - 1.f) * double(Eligible));
		const double T1 = FPlatformTime::Seconds();
		// REQ-SPREADSPAWN: the randomizer's seed when it is on (the host's run in co-op), else the level's own seed
		Plan->SpreadSeed = L && L->RandoOn() ? (L->RandoRun.Seed ? L->RandoRun.Seed : 1u) : 0u;
		Plan->bSpreadOff = OLFlags::Param(TEXT("OLSpreadOff"));
		const bool bCheck = OLFlags::Param(TEXT("OLSpreadCheck"));      // (a test's: worked out afresh, never cached)
		const FString Path = OLXtra::CachePath(Lv, Diff, Players, Plan->SpreadSeed);
		TArray<OLXtra::FSpot> Spots;
		FString CacheWhy = bCheck ? TEXT("-OLSpreadCheck") : TEXT("");
		bool bWritten = false;
		if (!bCheck && OLXtra::ReadCache(Path, Plan->Sources, Players, Eligible, Plan->PoolText(), Spots, CacheWhy))
		{
			How = TEXT("cached");
		}
		else
		{
			Plan->Search(Wanted, Players, Spots);
			How = TEXT("computed");
			if (!bCheck) bWritten = OLXtra::WriteCache(Path, Lv, Diff, Players, *Plan, Wanted, Spots);
		}
		int32 Spread[5] = { 0, 0, 0, 0, 0 };
		TMap<int32, int32> PerSector;
		OLXtra::LastSpread.Reset();
		for (const OLXtra::FSpot& Sp : Spots)
		{
			OLXtra::LastSpread.Add(Sp.Spread);
			++Spread[FMath::Clamp(Sp.Spread, 0, 4)];
			for (const OLXtra::FOriginal& Of : Plan->Originals)
			{
				if (Of.Row == Sp.Row && Of.Sector) { ++PerSector.FindOrAdd(Of.Sector->Index); break; }
			}
		}
		int32 MostInOne = 0;
		for (const TPair<int32, int32>& Kv : PerSector) MostInOne = FMath::Max(MostInOne, Kv.Value);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLRT spread extras for %s: %d placed, %d spread (%d at 5 units, %d at 4, %d at 3, %d closer), %d fallbacks to the ring%s; seed %s; %d sectors, at most %d in one"),
			*Lv, Spots.Num(), Spread[1] + Spread[2] + Spread[3] + Spread[4], Spread[1], Spread[2], Spread[3], Spread[4], Spread[0], Plan->bSpreadOff ? TEXT(" (-OLSpreadOff)") : TEXT(""),
			Plan->SpreadSeed ? *FString::Printf(TEXT("the randomizer's %u"), Plan->SpreadSeed) : TEXT("the level's"), PerSector.Num(), MostInOne);
		const double T2 = FPlatformTime::Seconds();
		int32 At3 = 0, At2 = 0, At4 = 0, Second = 0;
		TMap<FString, int32> Kinds;
		for (const OLXtra::FSpot& Sp : Spots)
		{
			FOutlawsCoopExtra X;
			X.Row = Sp.Row;
			X.Kind = Sp.Kind;
			X.Sprite = Sp.Sprite;
			++Kinds.FindOrAdd(Sp.Kind);
			// (to a tenth of a centimetre, as the session sends it: the host's copies stand exactly where the client's do)
			const FVector Feet = FOutlawsGeometry::ToUE(Sp.X, Sp.Y, Sp.Z);
			X.Feet = FVector(FMath::RoundToDouble(Feet.X * 10.0) / 10.0, FMath::RoundToDouble(Feet.Y * 10.0) / 10.0, FMath::RoundToDouble(Feet.Z * 10.0) / 10.0);
			X.Yaw = uint16((FMath::Clamp(Sp.YawDeg, 0, 359) * 65536) / 360);
			Out.Add(X);
			(Sp.Dist < 2.5 ? At2 : Sp.Dist < 3.5 ? At3 : At4)++;
			Second += Sp.Round > 0 ? 1 : 0;
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP extra bad guys: %d of %d for x%.2f (%d players, %s) (%s)"), Out.Num(), Eligible, Mult, PlayersNow,
			Whose, *How);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP extra bad guys for %s at %s: %d wanted (half of %d for each player beyond the first), %d placed: %d at 3 units, %d at 2, %d at 4%s; "
			"prep %.0f ms, %s %.0f ms; %s %s"), *Lv, OutlawsDifficulty::Name(Diff), Wanted, Eligible, Spots.Num(), At3, At2, At4,
			Second > 0 ? *FString::Printf(TEXT(", %d second copies"), Second) : TEXT(""), (T1 - T0) * 1000.0, How == TEXT("cached") ? TEXT("read") : TEXT("search"), (T2 - T1) * 1000.0,
			How == TEXT("cached") ? TEXT("from") : bWritten ? TEXT("cached in") : TEXT("NOT CACHED in"), *Path);
		// their kinds (user 2026-10-02: drawn for each from the ordinary bad guys of the whole story)
		Kinds.KeySort([](const FString& A, const FString& B) { return A < B; });
		TArray<FString> KindParts;
		for (const TPair<FString, int32>& Kv : Kinds) KindParts.Add(FString::Printf(TEXT("%s %d"), *Kv.Key, Kv.Value));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP extra bad guys' kinds for %s: %s (%d kinds, drawn from the pool %s%s)"), *Lv, *FString::Join(KindParts, TEXT(", ")), Kinds.Num(), *Plan->PoolText(),
			Plan->PoolLeftOut.Num() > 0 ? *FString::Printf(TEXT("; left out of the pool in this install: %s"), *FString::Join(Plan->PoolLeftOut, TEXT(", "))) : TEXT(""));
		if (Plan->PoolLeftOut.Num() > 0)
		{
			UE_LOG(LogOutlawsCoop, Warning, TEXT("OLCOOP extra bad guys: kinds missing from this install, left out of the pool: %s"), *FString::Join(Plan->PoolLeftOut, TEXT(", ")));
		}
		if (How == TEXT("computed"))
		{
			// what turned the other ways and distances down (each try counts)
			TArray<TPair<FString, int32>> Tally;
			for (const TPair<FString, int32>& Kv : Plan->Rejects) Tally.Emplace(Kv.Key, Kv.Value);
			Tally.Sort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B) { return A.Value > B.Value; });
			TArray<FString> Parts;
			for (int32 i = 0; i < FMath::Min(10, Tally.Num()); ++i) Parts.Add(FString::Printf(TEXT("%s %d"), *Tally[i].Key, Tally[i].Value));
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP extra bad guys for %s, the tries turned down: %s (the cache was not used: %s; sources %s)"), *Lv, *FString::Join(Parts, TEXT(", ")), *CacheWhy, *Plan->SourceFiles);
		}
		return Eligible;
	}

	/** A listed extra as the level makes it. */
	static AOutlawsLevelActor::FCoopExtraSpot SpotOf(const FOutlawsCoopExtra& X)
	{
		AOutlawsLevelActor::FCoopExtraSpot Spot;
		Spot.Row = X.Row;
		Spot.Feet = X.Feet;
		Spot.Yaw = float(X.Yaw) * (360.f / 65536.f);
		Spot.Kind = X.Kind;
		Spot.Sprite = X.Sprite;
		return Spot;
	}
}

void UOutlawsCoop::PrepareExtras(AOutlawsCoopSession* S, const FString& Level, int32 SpawnDifficulty)
{
	if (!S || !IsHost()) return;
	const int32 PlayersNow = FMath::Max(PlayersExpected, PlayerCount());
	const float Mult = FMath::RoundToFloat(BadGuysMultiplier() * 4.f) / 4.f;
	const int32 Players = FMath::RoundToInt(Mult * 100.f);         // (the list's key: the multiplier in hundredths)
	LoadMultiplier = 0.f;                                              // (a save's own: for that start only)
	const FString Lv = Level.ToLower();
	const EOutlawsDifficulty Diff = static_cast<EOutlawsDifficulty>(FMath::Clamp(SpawnDifficulty, 0, 2));
	S->Players = PlayersNow;
	S->Multiplier = Mult;
	S->ExtrasLevel = Lv;
	S->ExtrasDifficulty = int32(Diff);
	S->Extras.Reset();
	S->ExtrasEligible = 0;
	S->ExtrasSerial = S->StartSerial + 1;           // (the start serial moves on to this in the same frame)
	ExtrasHow.Reset();
	ExtrasListed = 0;
	// REQ-COOPRANDO: the host's randomizer run and this level's attempt, for every machine's build of this start
	S->Rando.Reset();
	if (AOutlawsLevelActor* LR = FindLevel())
	{
		if (!LR->RandoRun.bSet) LR->BeginRandoRun(TEXT("co-op start"));
		const AOutlawsLevelActor::FRandoRun& R = LR->RandoRun;
		const int32 Attempt = LR->NextRandoAttempt(Lv);
		S->Rando = { R.Enemies, R.Items, R.Order, R.Bosses, int32(R.Seed), Attempt, R.Critters };      // (REQ-RANDOCRITTERS: the host's Critters last)
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP randomizer for %s: enemies %d, items %d, order %d, bosses %d, critters %d, seed %u, attempt %d (the host's)"), *Lv, R.Enemies, R.Items, R.Order, R.Bosses,
			R.Critters, R.Seed, Attempt);
	}
	if (Mult <= 1.001f)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP extra bad guys: none for %s (x1.00)"), *Lv);
		return;
	}
	const AOutlawsLevelActor* L = FindLevel();
	S->ExtrasEligible = OLXtra::ListExtras(L, Lv, Diff, Mult, PlayersNow, Players, IsBadGuysAuto() ? TEXT("auto") : TEXT("the host's choice"), S->Extras, ExtrasHow);
	ExtrasListed = S->Extras.Num();
}

void UOutlawsCoop::HandExtrasToLevel(AOutlawsLevelActor* L, int32 Serial)
{
	if (!L) return;
	L->CoopExtras.Reset();
	L->CoopExtrasLevel.Reset();
	L->CoopExtrasDifficulty = -1;
	L->CoopExtrasPlayers = 1;
	const AOutlawsCoopSession* S = GetSession();
	// REQ-COOPRANDO: every machine (the host too) builds with the session's run and attempt, never its own saved settings;
	// none for this start (a client that gave up waiting): the randomizer off for it
	L->RandoRun = AOutlawsLevelActor::FRandoRun();
	L->RandoRun.bSet = true;
	L->RandoPendingAttempt = -1;
	if (S && S->ExtrasSerial == Serial && S->Rando.Num() >= 6)
	{
		L->RandoRun.Enemies = FMath::Clamp(S->Rando[0], 0, 2);
		L->RandoRun.Items = FMath::Clamp(S->Rando[1], 0, 2);
		L->RandoRun.Order = FMath::Clamp(S->Rando[2], 0, 2);
		L->RandoRun.Bosses = FMath::Clamp(S->Rando[3], 0, 1);
		L->RandoRun.Seed = uint32(S->Rando[4]);
		L->RandoPendingAttempt = FMath::Max(0, S->Rando[5]);
		L->RandoRun.Critters = S->Rando.Num() >= 7 ? FMath::Clamp(S->Rando[6], 0, 1) : 1;      // (a host from before REQ-RANDOCRITTERS: critters in, as it drew)
		L->RandoAttemptLevel = S->ExtrasLevel;
		if (IsClient())
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP randomizer for %s: enemies %d, items %d, order %d, bosses %d, critters %d, seed %u, attempt %d (the host's)"), *S->ExtrasLevel,
				L->RandoRun.Enemies, L->RandoRun.Items, L->RandoRun.Order, L->RandoRun.Bosses, L->RandoRun.Critters, L->RandoRun.Seed, L->RandoPendingAttempt);
		}
	}
	if (!S || S->ExtrasSerial != Serial)
	{
		UE_LOG(LogOutlawsCoop, Warning, TEXT("OLCOOP extra bad guys: none for this level (the host's list is for start %d, this start is %d)"), S ? S->ExtrasSerial : -1, Serial);
		return;
	}
	L->CoopExtrasLevel = S->ExtrasLevel;
	L->CoopExtrasDifficulty = S->ExtrasDifficulty;
	L->CoopExtrasPlayers = S->Players;
	for (const FOutlawsCoopExtra& X : S->Extras) L->CoopExtras.Add(OLXtra::SpotOf(X));
	ExtrasListed = S->Extras.Num();
	if (IsClient())
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP extra bad guys: %d of %d for x%.2f (%d players) (the host's list)"), S->Extras.Num(), S->ExtrasEligible, S->Multiplier, S->Players);
	}
}

float AOutlawsLevelActor::SoloBadGuysWanted() const
{
	// the settings' multiplier (UNLOCKFREE, user decision for 0.95: no Ugly finish needed any more; x1 until the player sets it)
	float Want = SoloBadGuys;
	OLFlags::Value(TEXT("OLSoloBadGuys"), Want);       // (a test's: never written to the settings)
	return FMath::RoundToFloat(FMath::Clamp(Want, 1.f, 4.f) * 4.f) / 4.f;
}

bool AOutlawsLevelActor::PrepareSoloExtras()
{
	// REQ-SPMULT (user 2026-10-05): single player's multiplier, the co-op host's list made the same way (Main Game and
	// Historical Missions alike; the randomizer's roll changes kinds, not which rows are bad guys)
	CoopExtras.Reset();
	CoopExtrasLevel.Reset();
	CoopExtrasDifficulty = -1;
	CoopExtrasPlayers = 1;
	const float Mult = SoloBadGuysWanted();
	if (Mult <= 1.001f) return false;
	const FString Lv = LevelName.ToLower();
	// (REQ-SPREADSPAWN: the run's randomizer, set now if this is its first level, so its seed picks the spread points)
	if (!RandoRun.bSet && !bHom) BeginRandoRun(TEXT("the first level built"));
	TArray<FOutlawsCoopExtra> List;
	FString How;
	const int32 Eligible = OLXtra::ListExtras(this, Lv, SpawnDifficulty, Mult, 1, FMath::RoundToInt(Mult * 100.f), TEXT("single player"), List, How);
	for (const FOutlawsCoopExtra& X : List) CoopExtras.Add(OLXtra::SpotOf(X));
	CoopExtrasLevel = Lv;
	CoopExtrasDifficulty = int32(SpawnDifficulty);
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLRT single player bad guys x%.2f in %s: %d extra listed of %d eligible (%s)"), Mult, *Lv, CoopExtras.Num(), Eligible, *How);
	return CoopExtras.Num() > 0;
}

// ---- both machines: the copies made, after the level's own --------------------------------------------------------------------

bool AOutlawsLevelActor::CoopCopyMayDrop(const FString& Drop) const
{
	const FString Name = FPaths::GetBaseFilename(Drop).ToLower();
	if (Name.IsEmpty() || Name == TEXT("null")) return true;                 // (nothing to drop anyway)
	if (Name.Contains(TEXT("key")) || Name.Contains(TEXT("puzz"))) return false;
	FString Text;
	if (!AssetsRef || !AssetsRef->GetText(Name + TEXT(".itm"), Text)) return false;      // (not known: nothing rather than a guess)
	FString Func;
	TMap<FString, FString> Data;
	OutlawsFormats::ParseItm(Text, Func, Data);
	TSet<int32> Needed;
	if (Inf) Inf->RequiredObjects(Needed);
	// every item it gives (PICKUP, PICKUP1..): a key, a puzzle piece or anything the level's locks ask for is kept back
	for (const TPair<FString, FString>& Kv : Data)
	{
		if (!Kv.Key.StartsWith(TEXT("PICKUP")) || Kv.Key.StartsWith(TEXT("PICKUP_"))) continue;
		const FString Item = FPaths::GetBaseFilename(Kv.Value).ToLower();
		const FItemDef* D = ItemDefs.Find(Item);
		const FString Class = D ? D->Class : FString();
		if (Item.Contains(TEXT("key")) || Class.Contains(TEXT("key")) || Class.Contains(TEXT("puzz"))) return false;
		if (D && D->UseId >= 0 && Needed.Contains(D->UseId)) return false;
	}
	return true;
}

int32 AOutlawsLevelActor::SpawnCoopExtras(const FOutlawsLevel& Built, const TMap<int32, FCoopCopySource>& Sources)
{
	UOutlawsCoop* Co = UOutlawsCoop::Get(this);
	const TCHAR* Who = !UOutlawsCoop::IsActive(this) ? TEXT("single player") : Co && Co->IsHost() ? TEXT("host") : TEXT("client");
	if (CoopExtrasLevel != LevelName.ToLower() || CoopExtrasDifficulty != int32(SpawnDifficulty))
	{
		UE_LOG(LogOutlawsCoop, Warning, TEXT("OLCOOP extra bad guys: none made here (the host's list is for %s at %d, this build is %s at %d) (%s)"), *CoopExtrasLevel, CoopExtrasDifficulty,
			*LevelName, int32(SpawnDifficulty), Who);
		return 0;
	}
	// each one logged with -OLCoopTest=extras (or -OLCoopExtrasLog): the same lines on both machines
	FString TestName;
	const bool bEach = OLFlags::Param(TEXT("OLCoopExtrasLog")) || (OLFlags::Value(TEXT("OLCoopTest"), TestName) && TestName.Equals(TEXT("extras"), ESearchCase::IgnoreCase));
	const int32 OwnBadGuys = TotalBadGuys;
	int32 NoOriginal = 0, NotMade = 0, DropsTaken = 0, Moved = 0, KindMissing = 0;
	TMap<FString, int32> KindsMade;
	for (int32 n = 0; n < CoopExtras.Num(); ++n)
	{
		const FCoopExtraSpot& X = CoopExtras[n];
		const FCoopCopySource* Src = Sources.Find(X.Row);
		if (!Src)
		{
			++NoOriginal;                          // (the bad guy he stands by was not made here: nor is he, on both machines alike)
			continue;
		}
		// his kind, as the host's list names it (user 2026-10-02: not a copy of the bad guy he stands by): that kind's own ITM
		// (health, weapon, sounds, drop) and sprite; the stats follow the spawn difficulty as for any bad guy (Init scales them)
		TMap<FString, FString> Data;
		FString Anim, Name;
		if (!X.Kind.IsEmpty() && AssetsRef)
		{
			FString Itm, Func;
			const FString Resolved = OutlawsFormats::ResolveAnim(*AssetsRef, X.Kind, &Itm);
			OutlawsFormats::ParseItm(Itm, Func, Data);
			Anim = X.Sprite.IsEmpty() ? Resolved : X.Sprite;
			if (Itm.IsEmpty() || !Func.Equals(TEXT("BGY_New"), ESearchCase::IgnoreCase) || !AssetsRef->Has(Anim))
			{
				Data.Reset();
				Anim.Reset();
			}
			else
			{
				Name = X.Kind.ToUpper();
				// he starts as the bad guy he stands by starts (a sniper's neighbour keeps his post, an ambusher's waits
				// crouched), not as his kind's plain ITM would have him
				for (const TCHAR* Key : { TEXT("BASE_MODE"), TEXT("WATCH_CHOR") })
				{
					if (const FString* V = Src->Data.Find(Key)) Data.Add(Key, *V);
					else Data.Remove(Key);
				}
			}
		}
		if (Anim.IsEmpty())
		{
			// (his kind missing here: the kind of the bad guy he stands by instead, so every index still lines up)
			++KindMissing;
			UE_LOG(LogOutlawsCoop, Warning, TEXT("OLCOOP extra bad guy %d: his kind %s (%s) is not in this install, so he is a %s like the bad guy he stands by (%s)"), Enemies.Num(), *X.Kind,
				*X.Sprite, *Src->Name, Who);
			Data = Src->Data;
			Anim = Src->Anim;
			Name = Src->Name;
		}
		FString DropKept;
		if (const FString* Drop = Data.Find(TEXT("DROP_ITEM")); Drop && !CoopCopyMayDrop(*Drop))
		{
			DropKept = FPaths::GetBaseFilename(*Drop);
			Data.Add(TEXT("DROP_ITEM"), TEXT("null"));
			++DropsTaken;
		}
		// (an extra never stands on a door or a lift: the list's spots are kept off them; the ride is asked all the same)
		double RideDz = 0.0;
		if (const FOutlawsSector* Sec = Built.SectorAt(FOutlawsGeometry::ToOutlaws(X.Feet, 0.01)))
		{
			RideFor(Sec->Index, X.Feet.Z / FOutlawsGeometry::UnitsToCm, false, RideDz);
		}
		const int32 Index = Enemies.Num();
		bool bMade = SpawnEnemy(Name, Data, Anim, X.Feet + FVector(0.0, 0.0, RideDz), X.Yaw, Src->Light);
		if (!bMade && Anim != Src->Anim)
		{
			// (his kind would not make here: the bad guy he stands by's kind, so the indexes still line up)
			++KindMissing;
			UE_LOG(LogOutlawsCoop, Warning, TEXT("OLCOOP extra bad guy %d: his kind %s would not make here, so he is a %s like the bad guy he stands by (%s)"), Index, *Name, *Src->Name, Who);
			Data = Src->Data;
			if (const FString* Drop = Data.Find(TEXT("DROP_ITEM")); Drop && !CoopCopyMayDrop(*Drop)) Data.Add(TEXT("DROP_ITEM"), TEXT("null"));
			Anim = Src->Anim;
			Name = Src->Name;
			bMade = SpawnEnemy(Name, Data, Anim, X.Feet + FVector(0.0, 0.0, RideDz), X.Yaw, Src->Light);
		}
		if (!bMade)
		{
			++NotMade;
			continue;
		}
		AOutlawsEnemy* E = Enemies.Last().Get();
		if (E)
		{
			E->CoopCopyOf = Src->Enemy;
			E->CoopExtraIndex = n;
		}
		if (CoopExtrasFirst < 0) CoopExtrasFirst = Index;
		++CoopExtrasMade;
		++TotalBadGuys;                            // (the late boss comes at 80 % of all the bad guys there are, the extras too)
		++KindsMade.FindOrAdd(Name.ToLower());
		const double Off = E ? FVector::Dist2D(E->GetActorLocation(), X.Feet) : 0.0;
		Moved += Off > 5.0 ? 1 : 0;
		const int32 Rec[8] = { Index, X.Row, int32(FMath::RoundToInt(X.Feet.X * 10.0)), int32(FMath::RoundToInt(X.Feet.Y * 10.0)), int32(FMath::RoundToInt(X.Feet.Z * 10.0)),
			int32(FMath::RoundToInt(X.Yaw * 10.f)), int32(FCrc::StrCrc32(*Name.ToLower())), int32(FCrc::StrCrc32(*Anim.ToLower())) };
		CoopExtrasSum = FCrc::MemCrc32(Rec, sizeof(Rec), CoopExtrasSum);
		if (bEach)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP extra bad guy %d in %s: list %d, a %s (%s) beside enemy %d (%s, row %d) at %s facing %.1f, starts mode %s chor %s%s%s (%s)"), Index, *LevelName, n,
				*Name, *Anim.ToUpper(), Src->Enemy, *Src->Name, X.Row, *X.Feet.ToCompactString(), X.Yaw, *Data.FindRef(TEXT("BASE_MODE")), *Data.FindRef(TEXT("WATCH_CHOR")),
				Off > 5.0 ? *FString::Printf(TEXT(", MOVED %.0f cm by the spawn"), Off) : TEXT(""),
				DropKept.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", no drop (his kind's %s is kept back)"), *DropKept), Who);
		}
	}
	KindsMade.KeySort([](const FString& A, const FString& B) { return A < B; });
	TArray<FString> KindParts;
	for (const TPair<FString, int32>& Kv : KindsMade) KindParts.Add(FString::Printf(TEXT("%s %d"), *Kv.Key, Kv.Value));
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP extra bad guys made here: %d of %d listed (enemies %d..%d of %d), checksum %08x; %d without their neighbour here, %d not made, %d moved by the spawn, "
		"%d drops taken off (keys and the like), %d not of their listed kind (%s)"), CoopExtrasMade, CoopExtras.Num(), CoopExtrasFirst, CoopExtrasFirst + CoopExtrasMade - 1, Enemies.Num(), CoopExtrasSum,
		NoOriginal, NotMade, Moved, DropsTaken, KindMissing, Who);
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP extra bad guys' kinds made here: %s (%s)"), *FString::Join(KindParts, TEXT(", ")), Who);
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP extra bad guys count toward the late boss: %d bad guys in all (%d of the level's own, %d extra); he comes at 80 %% of them, killed (%s)"),
		TotalBadGuys, OwnBadGuys, CoopExtrasMade, Who);
	if (OLFlags::Param(TEXT("OLSpreadCheck"))) CheckSpread(Built);
	return CoopExtrasMade;
}

void AOutlawsLevelActor::CheckSpread(const FOutlawsLevel& Built)
{
	// REQ-SPREADSPAWN's test (-OLSpreadCheck, single player with -OLSoloBadGuys): each extra as made, against the level as
	// parsed (his original's sector, its lines) and the other bad guys; a capsule test against the built level a second on
	// (the geometry's collision in place), then the summary and the exit
	constexpr double U = FOutlawsGeometry::UnitsToCm;
	TArray<double> Gaps;
	int32 Outside = 0, TooNear = 0, Spread = 0, Fallback = 0;
	TMap<int32, int32> PerSector;
	for (int32 i = 0; i < Enemies.Num(); ++i)
	{
		const AOutlawsEnemy* E = Enemies[i].Get();
		if (!E || E->CoopCopyOf < 0 || !CoopExtras.IsValidIndex(E->CoopExtraIndex)) continue;
		const int32 n = E->CoopExtraIndex;
		const FCoopExtraSpot& X = CoopExtras[n];
		const int32 Step = OLXtra::LastSpread.IsValidIndex(n) ? OLXtra::LastSpread[n] : -1;
		(Step > 0 ? Spread : Fallback)++;
		const FVector OlFeet = FOutlawsGeometry::ToOutlaws(X.Feet);
		const double Ox = OlFeet.X, Oz = OlFeet.Z, Oy = OlFeet.Y;
		const FOutlawsSector* Home = nullptr;
		if (Built.Objects.IsValidIndex(X.Row))
		{
			const FOutlawsObject& O = Built.Objects[X.Row];
			Home = Built.SectorById(O.SectorId);
			if (!Home) Home = Built.SectorAt(O.Pos.X, O.Pos.Y, O.Pos.Z);
		}
		const bool bIn = Home && Built.ContainsXZ(*Home, Ox, Oz);
		double Wall = 1e30;
		if (Home)
		{
			for (const FOutlawsWall& W : Home->Walls)
			{
				if (Home->Verts.IsValidIndex(W.V1) && Home->Verts.IsValidIndex(W.V2)) Wall = FMath::Min(Wall, FMath::Sqrt(OLXtra::SegDistSq(Ox, Oz, Home->Verts[W.V1], Home->Verts[W.V2])));
			}
		}
		double Near = 1e30;
		for (int32 j = 0; j < Enemies.Num(); ++j)
		{
			const AOutlawsEnemy* F = Enemies[j].Get();
			if (!F || j == i) continue;
			const FVector P = F->CoopCopyOf >= 0 && CoopExtras.IsValidIndex(F->CoopExtraIndex) ? CoopExtras[F->CoopExtraIndex].Feet
				: F->GetActorLocation() - FVector(0.0, 0.0, F->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());      // (his feet)
			if (FMath::Abs(P.Z - X.Feet.Z) >= OLXtra::ManTall * U) continue;      // (the planner's own: a floor above or below is not near)
			Near = FMath::Min(Near, FVector::Dist2D(P, X.Feet) / U);
		}
		Gaps.Add(Near);
		const bool bBadIn = Step > 0 && !bIn;
		const bool bBadWall = Step > 0 && Wall < 1.0;
		Outside += bBadIn ? 1 : 0;
		TooNear += bBadWall ? 1 : 0;
		if (Home) ++PerSector.FindOrAdd(Home->Index);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLRT spread extra %d (row %d): sector %d %s, wall %.2f u, nearest bad guy %.2f u (%.2f m), %s%s"), n, X.Row, Home ? Home->Index : -1,
			bIn ? TEXT("inside") : TEXT("OUTSIDE"), Wall, Near, Near * U / 100.0, Step > 0 ? *FString::Printf(TEXT("spread step %d"), Step) : TEXT("fallback (the ring)"),
			bBadIn || bBadWall ? TEXT(" FAIL") : TEXT(""));
	}
	Gaps.Sort();
	const double Min = Gaps.Num() > 0 ? Gaps[0] : 0.0, Med = Gaps.Num() > 0 ? Gaps[Gaps.Num() / 2] : 0.0;
	int32 MostInOne = 0;
	for (const TPair<int32, int32>& Kv : PerSector) MostInOne = FMath::Max(MostInOne, Kv.Value);
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLRT spread check %s: %d extras placed, %d spread, %d fallbacks; nearest bad guy min %.2f u, median %.2f u (%.2f m); %d sectors, at most %d in one; %d outside their sector, %d within 1 u of a line; checksum %08x"),
		*LevelName, Gaps.Num(), Spread, Fallback, Min, Med, Med * U / 100.0, PerSector.Num(), MostInOne, Outside, TooNear, CoopExtrasSum);
	const int32 Bad = Outside + TooNear;
	FTimerHandle Th;
	GetWorldTimerManager().SetTimer(Th, FTimerDelegate::CreateWeakLambda(this, [this, Bad]()
	{
		FCollisionObjectQueryParams Objs;
		Objs.AddObjectTypesToQuery(ECC_WorldStatic);
		Objs.AddObjectTypesToQuery(ECC_WorldDynamic);
		Objs.AddObjectTypesToQuery(ECC_GameTraceChannel2);
		Objs.AddObjectTypesToQuery(ECC_GameTraceChannel3);
		int32 InWall = 0, Checked = 0;
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wp : Enemies)
		{
			AOutlawsEnemy* E = Wp.Get();
			if (!E || E->CoopCopyOf < 0 || !CoopExtras.IsValidIndex(E->CoopExtraIndex)) continue;
			const UCapsuleComponent* Cap = E->GetCapsuleComponent();
			const float Half = Cap->GetScaledCapsuleHalfHeight(), Rad = FMath::Max(5.f, Cap->GetScaledCapsuleRadius() - 2.f);
			const FVector& Feet = CoopExtras[E->CoopExtraIndex].Feet;
			const double Bottom = Feet.Z + 25.0, Top = Feet.Z + 2.0 * Half - 2.0;
			const FVector Centre(Feet.X, Feet.Y, (Bottom + Top) * 0.5);
			FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsSpreadCheck), false, E);
			TArray<FOverlapResult> Hits;
			GetWorld()->OverlapMultiByObjectType(Hits, Centre, FQuat::Identity, Objs, FCollisionShape::MakeCapsule(Rad, FMath::Max(Rad + 1.f, float((Top - Bottom) * 0.5))), Q);
			++Checked;
			for (const FOverlapResult& Hit : Hits)
			{
				if (Hit.GetActor() == this)
				{
					++InWall;
					UE_LOG(LogOutlawsCoop, Warning, TEXT("OLRT spread extra %d IN THE LEVEL'S GEOMETRY at %s (%s)"), E->CoopExtraIndex, *Feet.ToCompactString(), Hit.GetComponent() ? *Hit.GetComponent()->GetName() : TEXT("?"));
					break;
				}
			}
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLRT spread check %s: capsules %d checked, %d in the level's geometry: %s"), *LevelName, Checked, InWall, Bad + InWall == 0 && Checked > 0 ? TEXT("PASS") : TEXT("FAIL"));
		FGenericPlatformMisc::RequestExit(false);
	}), 1.5f, false);
}

void UOutlawsCoop::CheckExtras(AOutlawsLevelActor* L)
{
	// each copy's body against the level as built (its walls, blockers, doors and lifts), at the height a wall stands: the
	// floor itself is left out (a slope touches a capsule's foot); what else touches him is said, not counted
	ExtrasMadeHere = L ? L->CoopExtrasMade : 0;
	UWorld* W = L ? L->GetWorld() : nullptr;
	if (!W || ExtrasMadeHere == 0) return;
	FCollisionObjectQueryParams Objs;
	Objs.AddObjectTypesToQuery(ECC_WorldStatic);
	Objs.AddObjectTypesToQuery(ECC_WorldDynamic);
	Objs.AddObjectTypesToQuery(ECC_GameTraceChannel2);       // (EnemyBlocker: a fence)
	Objs.AddObjectTypesToQuery(ECC_GameTraceChannel3);       // (PawnBlocker: a no-pass opening)
	int32 Clear = 0, InWall = 0, Touching = 0;
	const TArray<TWeakObjectPtr<AOutlawsEnemy>>& List = L->GetEnemies();
	for (int32 i = 0; i < List.Num(); ++i)
	{
		AOutlawsEnemy* E = List[i].Get();
		if (!E || E->CoopCopyOf < 0) continue;
		const UCapsuleComponent* Cap = E->GetCapsuleComponent();
		const float Half = Cap->GetScaledCapsuleHalfHeight(), Rad = FMath::Max(5.f, Cap->GetScaledCapsuleRadius() - 2.f);
		const double FeetZ = E->GetActorLocation().Z - Half;
		const double Bottom = FeetZ + 25.0, Top = FeetZ + 2.0 * Half - 2.0;
		const float TestHalf = FMath::Max(Rad + 1.f, float((Top - Bottom) * 0.5));
		const FVector Centre(E->GetActorLocation().X, E->GetActorLocation().Y, (Bottom + Top) * 0.5);
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsCoopExtraCheck), false, E);
		TArray<FOverlapResult> Hits;
		W->OverlapMultiByObjectType(Hits, Centre, FQuat::Identity, Objs, FCollisionShape::MakeCapsule(Rad, TestHalf), Q);
		bool bWall = false;
		TArray<FString> Others;
		for (const FOverlapResult& Hit : Hits)
		{
			const AActor* A = Hit.GetActor();
			if (!A || A->IsA<APawn>()) continue;
			if (A == L)
			{
				bWall = true;
				Others.Add(Hit.GetComponent() ? Hit.GetComponent()->GetName() : FString(TEXT("the level")));
			}
			else
			{
				Others.Add(A->GetClass()->GetName());
			}
		}
		if (bWall)
		{
			++InWall;
			UE_LOG(LogOutlawsCoop, Warning, TEXT("OLCOOP extra bad guy %d IN A WALL at %s (%s) (%s)"), i, *E->GetActorLocation().ToCompactString(), *FString::Join(Others, TEXT(", ")), IsHost() ? TEXT("host") : TEXT("client"));
		}
		else
		{
			++Clear;
			if (Others.Num() > 0)
			{
				++Touching;
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP extra bad guy %d clear of the walls, touching %s (%s)"), i, *FString::Join(Others, TEXT(", ")), IsHost() ? TEXT("host") : TEXT("client"));
			}
		}
	}
	ExtrasInWalls = InWall;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP extra bad guys checked against the level here: %d clear of its walls, %d in a wall, %d touching something else (%s)"), Clear, InWall, Touching,
		IsHost() ? TEXT("host") : TEXT("client"));
}

FString UOutlawsCoop::ExtrasSummary() const
{
	const AOutlawsCoopSession* S = GetSession();
	return FString::Printf(TEXT("players %d, listed %d%s, made here %d, in a wall %d"), S ? S->Players : 0, ExtrasListed, ExtrasHow.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" (%s)"), *ExtrasHow),
		ExtrasMadeHere, ExtrasInWalls);
}

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::RunExtrasSurvey()
{
	// (a test: no session) every story level at each difficulty for two, three and four players, written to the folder given
	const AOutlawsLevelActor* L = FindLevel();
	const FString Root = FOutlawsAssets::FindInstallRoot(L ? L->InstallPath : FString());
	TSharedPtr<FOutlawsAssets> Assets;
	if (!Root.IsEmpty()) Assets = FOutlawsAssets::GetShared(Root);
	if (!Assets) return;
	static const TCHAR* const StoryLevels[] = { TEXT("hideout"), TEXT("townex"), TEXT("train"), TEXT("canyon"), TEXT("mill"), TEXT("simms"), TEXT("miner"), TEXT("cliff"), TEXT("ranch") };
	int32 Lists = 0, Short = 0;
	for (const TCHAR* Lv : StoryLevels)
	{
		for (int32 D = 0; D < 3; ++D)
		{
			for (const int32 Players : { 150, 200, 250, 300, 400 })      // (R14: multipliers in hundredths)
			{
				const double T0 = FPlatformTime::Seconds();
				const TUniquePtr<OLXtra::FPlanner> Plan = MakeUnique<OLXtra::FPlanner>();
				FString Why;
				const EOutlawsDifficulty Diff = static_cast<EOutlawsDifficulty>(D);
				if (!Plan->Prepare(*Assets, Lv, Diff, Why))
				{
					UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP extras survey: %s: %s"), Lv, *Why);
					continue;
				}
				const int32 Wanted = FMath::RoundToInt(double(Players - 100) / 100.0 * double(Plan->Originals.Num()));
				const double T1 = FPlatformTime::Seconds();
				TArray<OLXtra::FSpot> Spots;
				Plan->Search(Wanted, Players, Spots);
				const double T2 = FPlatformTime::Seconds();
				const FString Path = ExtrasSurveyDir / FPaths::GetCleanFilename(OLXtra::CachePath(Lv, Diff, Players));
				OLXtra::WriteCache(Path, Lv, Diff, Players, *Plan, Wanted, Spots);
				int32 Second = 0;
				for (const OLXtra::FSpot& Sp : Spots) Second += Sp.Round > 0 ? 1 : 0;
				++Lists;
				Short += Spots.Num() < Wanted ? 1 : 0;
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP extras survey: %s %s, %d players: %d of %d wanted (eligible %d, %d second copies), prep %.0f ms, search %.0f ms"), Lv,
					OutlawsDifficulty::Name(Diff), Players, Spots.Num(), Wanted, Plan->Originals.Num(), Second, (T1 - T0) * 1000.0, (T2 - T1) * 1000.0);
			}
		}
	}
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP extras survey: %d lists written to %s, %d short of what was wanted"), Lists, *ExtrasSurveyDir, Short);
}
#endif // WITH_OUTLAWS_TESTS

// ---- the test (-OLCoopAuto -OLCoopTest=extras -OLCoopLevel=hideout) -----------------------------------------------------------

#if WITH_OUTLAWS_TESTS
int32 UOutlawsCoop::TestFrameExtra(AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	// the extra bad guys whose neighbours (the bad guys they stand by) are up too, the nearest first: from a free spot 3.3 to 5.2 m off, square to the pair
	// where it can be (both side by side in the picture), both bodies in plain sight.  First only pairs still at their posts
	// and side by side (not yet woken, at most 1.5 m apart), then any
	UWorld* W = L ? L->GetWorld() : nullptr;
	if (!W || !P) return -1;
	const TArray<TWeakObjectPtr<AOutlawsEnemy>>& List = L->GetEnemies();
	TArray<TPair<double, int32>> Cands;
	for (int32 i = 0; i < List.Num(); ++i)
	{
		const AOutlawsEnemy* E = List[i].Get();
		const AOutlawsEnemy* O = E && List.IsValidIndex(E->CoopCopyOf) ? List[E->CoopCopyOf].Get() : nullptr;
		if (!E || !O || E->IsDead() || O->IsDead() || E->IsDormant() || O->IsDormant()) continue;
		// (user 2026-10-02: a pair of two kinds first, to show he is not the other's copy)
		const bool bSameKind = E->GetOutlawsName().Left(4).Equals(O->GetOutlawsName().Left(4), ESearchCase::IgnoreCase);
		Cands.Emplace(FVector::DistSquared(E->GetActorLocation(), P->GetActorLocation()) + (bSameKind ? 1e12 : 0.0), i);
	}
	Cands.Sort([](const TPair<double, int32>& A, const TPair<double, int32>& B) { return A.Key < B.Key; });
	const float Half = P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), Rad = P->GetCapsuleComponent()->GetScaledCapsuleRadius();
	static const float Turns[] = { 0.f, 180.f, 30.f, -30.f, 150.f, 210.f, 60.f, -60.f, 120.f, 240.f, 90.f, 270.f };
	for (int32 c = 0; c < 2 * FMath::Min(40, Cands.Num()); ++c)
	{
		const bool bStrict = c < FMath::Min(40, Cands.Num());
		AOutlawsEnemy* E = List[Cands[c % FMath::Min(40, Cands.Num())].Value].Get();
		AOutlawsEnemy* O = List[E->CoopCopyOf].Get();
		if (bStrict && (E->GetState() != EOutlawsEnemyState::Watch || O->GetState() != EOutlawsEnemyState::Watch || FVector::Dist2D(E->GetActorLocation(), O->GetActorLocation()) > 150.0)) continue;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsCoopExtraShot), false, P);
		Q.AddIgnoredActor(E);
		Q.AddIgnoredActor(O);
		const FVector A = E->GetActorLocation(), B = O->GetActorLocation();
		const FVector Mid = (A + B) * 0.5;
		const FVector Pair = (B - A).GetSafeNormal2D();
		const FVector Across = Pair.IsNearlyZero() ? FVector::ForwardVector : FVector(-Pair.Y, Pair.X, 0.0);
		const double FeetZ = Mid.Z - E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		for (const double Dist : { 420.0, 330.0, 520.0 })
		{
			for (const float Turn : Turns)
			{
				const FVector Way = FRotator(0.f, Turn, 0.f).RotateVector(Across);
				const FVector At = Mid + Way * Dist;
				FHitResult Floor;
				if (!W->LineTraceSingleByChannel(Floor, FVector(At.X, At.Y, FeetZ + 150.0), FVector(At.X, At.Y, FeetZ - 300.0), ECC_Visibility, Q)) continue;
				if (FMath::Abs(Floor.ImpactPoint.Z - FeetZ) > 150.0) continue;
				const FVector Stand = Floor.ImpactPoint + FVector(0.0, 0.0, Half + 3.0);
				if (W->OverlapBlockingTestByChannel(Stand, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(Rad, Half), Q)) continue;
				const FVector Eye = Stand + FVector(0.0, 0.0, 70.0);
				FHitResult Hit;
				if (W->LineTraceSingleByChannel(Hit, Eye, A + FVector(0.0, 0.0, 30.0), ECC_Visibility, Q) || W->LineTraceSingleByChannel(Hit, Eye, B + FVector(0.0, 0.0, 30.0), ECC_Visibility, Q)) continue;
				if (W->LineTraceSingleByChannel(Hit, Eye, A - FVector(0.0, 0.0, 60.0), ECC_Visibility, Q) || W->LineTraceSingleByChannel(Hit, Eye, B - FVector(0.0, 0.0, 60.0), ECC_Visibility, Q)) continue;
				P->TeleportTo(Stand, FRotator(0.0, (Mid - Stand).Rotation().Yaw, 0.0), false, true);
				if (APlayerController* PC = Cast<APlayerController>(P->GetController())) PC->SetControlRotation((Mid - FVector(0.0, 0.0, 10.0) - Eye).Rotation());
				const int32 Shown = Cands[c % FMath::Min(40, Cands.Num())].Value;
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: extras on %s: the host frames extra bad guy %d (a %s) beside enemy %d (%s), the bad guy he stands by: %.0f cm apart, seen from %.0f cm at %s%s"), *BuiltLevel,
					Shown, *E->GetOutlawsName(), E->CoopCopyOf, *O->GetOutlawsName(), FVector::Dist2D(A, B), FVector::Dist2D(Stand, Mid), *Stand.ToCompactString(), bStrict ? TEXT("") : TEXT(" (no pair still at its posts)"));
				return Shown;
			}
		}
	}
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: extras on %s: NO extra bad guy could be framed beside the bad guy he stands by"), *BuiltLevel);
	return -1;
}

void UOutlawsCoop::TickHostExtrasTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	// the hideout (the list worked out, or read if cached before), the same again (Restart Level: the list from the cache,
	// the same extras), the town, the canyon.  On each but the repeat: an extra photographed beside the bad guy he stands by
	// (one of another kind where there is one), one woken to fight, then shot dead by the client (dead on both); both
	// machines' logs list every extra (index, kind, neighbour, place)
	static const TCHAR* const ExtraLevels[] = { TEXT("hideout"), TEXT("hideout"), TEXT("townex"), TEXT("canyon") };
	if (Now < TestAt) return;
	const TArray<TWeakObjectPtr<AOutlawsEnemy>>& List = L->GetEnemies();
	switch (TestStep)
	{
	case 230:
	{
		L->bInvincible = true;                   // (both out of harm: the test is about the bad guys, not the players' deaths)
		TestSay(TEXT("godmode"), 1);
		int32 Living = 0;
		for (const TWeakObjectPtr<AOutlawsEnemy>& E : List) Living += E.IsValid() && E->CoopCopyOf >= 0 && !E->IsDead() ? 1 : 0;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: extras on %s (pass %d, %s): enemies %d, extra made here %d (first %d), living %d, checksum %08x"), *BuiltLevel, TestExtraLevel,
			*ExtrasHow, List.Num(), L->CoopExtrasMade, L->CoopExtrasFirst, Living, L->CoopExtrasSum);
		TestSay(TEXT("x:count"), TestExtraLevel);
		if (TestExtraLevel == 1)
		{
			TestStep = 235;                      // (the repeat from the cache: the lists compared, nothing more)
			TestAt = Now + 1.0;
			return;
		}
		L->bEnemiesActive = false;               // (held still for the photograph)
		TestExtraShown = TestFrameExtra(L, P);
		TestStep = 231;
		TestAt = Now + 1.5;
		return;
	}
	case 231:
		Shot(FString::Printf(TEXT("ol_extra_%s"), *BuiltLevel));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: extras on %s: photographed (extra bad guy %d)"), *BuiltLevel, TestExtraShown);
		TestStep = 232;
		TestAt = Now + 0.6;
		return;
	case 232:
	{
		L->bEnemiesActive = true;
		// the fight: the living extra bad guy nearest the client, woken (he goes after a player and shoots as any bad guy)
		const AOutlawsCoopLink* CL = GetLink(1);
		const FVector Near = CL ? FVector(CL->Pose.Body) : P->GetActorLocation();
		TestExtraFight = -1;
		double Best = 1e30;
		for (int32 i = 0; i < List.Num(); ++i)
		{
			const AOutlawsEnemy* E = List[i].Get();
			if (!E || E->CoopCopyOf < 0 || E->IsDead() || E->IsDormant() || E->Def.ShootChor < 0) continue;
			const double D = FVector::DistSquared(E->GetActorLocation(), Near);
			if (D < Best) { Best = D; TestExtraFight = i; }
		}
		if (AOutlawsEnemy* E = List.IsValidIndex(TestExtraFight) ? List[TestExtraFight].Get() : nullptr)
		{
			E->Wake(false);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: extras on %s: extra bad guy %d (a %s beside enemy %d) woken, %.0f cm from the client"), *BuiltLevel, TestExtraFight,
				*E->GetOutlawsName(), E->CoopCopyOf, FMath::Sqrt(Best));
		}
		TestStep = 233;
		TestAt = Now + 3.5;
		return;
	}
	case 233:
	{
		if (const AOutlawsEnemy* E = List.IsValidIndex(TestExtraFight) ? List[TestExtraFight].Get() : nullptr)
		{
			static const TCHAR* States[] = { TEXT("watch"), TEXT("chase"), TEXT("shoot"), TEXT("hit"), TEXT("dead") };
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: extras on %s: extra bad guy %d after waking: state %s, after the %s player, shots %d (host)"), *BuiltLevel, TestExtraFight,
				States[FMath::Clamp(int32(E->GetState()), 0, 4)], E->CoopTarget == 1 ? TEXT("client") : TEXT("host"), int32(E->CoopShots));
		}
		TestLogEnemy(L, TestExtraFight, TEXT("before the client's shot"));
		TestSay(TEXT("enemy"), TestExtraFight);
		TestSay(TEXT("kill"), TestExtraFight);
		TestStep = 234;
		TestAt = Now + 2.5;
		return;
	}
	case 234:
		TestLogEnemy(L, TestExtraFight, TEXT("after the client's shot"));
		TestSay(TEXT("enemy"), TestExtraFight);
		TestStep = 235;
		TestAt = Now + 1.0;
		return;
	case 235:
	{
		// the next: the same level again (Restart Level: from the cache), the town, the canyon; then done
		++TestExtraLevel;
		if (TestExtraLevel >= int32(UE_ARRAY_COUNT(ExtraLevels)))
		{
			TestSay(TEXT("quit"), 0);
			TestStep = 100;
			TestAt = Now + 3.0;
			return;
		}
		TestArg = StartedSerial;
		if (TestExtraLevel == 1) RequestStart(EOutlawsCoopStart::Restart, BuiltLevel);
		else RequestStart(EOutlawsCoopStart::Jump, ExtraLevels[TestExtraLevel]);
		TestStep = 236;
		TestAt = Now + 1.0;
		return;
	}
	case 236:
	{
		// (both machines' builds of it)
		const AOutlawsCoopLink* CL = GetLink(1);
		if (!(bLevelBuilt && StartedSerial > TestArg && CL && CL->BuiltSerial == StartedSerial)) return;
		TestStep = 230;
		TestAt = Now + 3.0;
		return;
	}
	default:
		return;
	}
}

void UOutlawsCoop::OnExtrasTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	if (Step == TEXT("count"))
	{
		const TArray<TWeakObjectPtr<AOutlawsEnemy>>& List = L->GetEnemies();
		int32 Living = 0;
		for (const TWeakObjectPtr<AOutlawsEnemy>& E : List) Living += E.IsValid() && E->CoopCopyOf >= 0 && !E->IsDead() ? 1 : 0;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: extras on %s (pass %d, the client): enemies %d, extra made here %d (first %d), living %d, checksum %08x"), *BuiltLevel, Arg, List.Num(),
			L->CoopExtrasMade, L->CoopExtrasFirst, Living, L->CoopExtrasSum);
	}
}
#endif // WITH_OUTLAWS_TESTS
