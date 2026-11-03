// BUG-BLASTWALLS (CWWALL2 lane, 2026-11-01: a dynamite blast in the original tells the walls in reach as well as the sectors,
// ours told the sectors alone, so the nine line items that list the blast (EVENT_MASK 368: civlwar1's three CANNONT walls,
// the mill's SEWERGATE1T..6T) never took a stick; they still took a shot or the hand): -OLShots -OLNoFrontEnd
// -OLLevel=<civlwar1|mill> -OLBlastWallsProbe [-OLOldBlastWalls].  (0) every level in the install: the line items whose
// EVENT_MASK lists a blast's wall events (64 and 0x8000, a mirror's 128 and 0x10000), and the line triggers with no mask
// (left out: a blast is no one, as a shot is); each listed one run on its own copy of the level's INF with a blast as
// Explode sends it, from a stick a unit in front of the wall and from the first spot along its normal just out of
// WALL_RADIUS, the client elevators watched 15 s; (1) on the level loaded, the same two spots each with a lit stick set
// down, the real blast, the clients watched 3 s.  Lines "OLSHOT blastwalls check PASS|FAIL: ..." and "OLSHOT blastwalls
// RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsInf.h"
#include "OutlawsEnemy.h"
#include "OutlawsLab.h"
#include "OutlawsFormats.h"
#include "OutlawsGeometry.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLBlastWallsProbe) { return OLFlags::Param(TEXT("OLBlastWallsProbe")); }

namespace
{
	constexpr uint32 BlastWallBits = OutlawsInf::EvExplosion | 0x80u | 0x8000u | 0x10000u;

	/** The exe's flat distance [EXE 0x1402de1a2]: |dx| + |dz| less half the smaller */
	double Oct(const FVector2D& A, const FVector2D& B)
	{
		const double Dx = FMath::Abs(A.X - B.X), Dz = FMath::Abs(A.Y - B.Y);
		return Dx + Dz - 0.5 * FMath::Min(Dx, Dz);
	}

	/** One blast wall: its trigger, the wall, and the two spots (Outlaws x, y, z) a stick goes off at */
	struct FBlastWall
	{
		int32 Trigger = -1, Sector = -1, Wall = -1;
		FString Name, Id;
		FVector In = FVector::ZeroVector, Out = FVector::ZeroVector;
		bool bOut = false;
		double Gate = 0.0, InD = 0.0, OutD = 0.0;     // the sector's corners on average, the wall's ends from each spot
	};

	/** The wall's spots: a unit in front of it on its sector's floor, and the first spot along its normal (either way)
	 *  that is in a sector on about the same floor with the wall's two ends just out of WallR on average */
	void Spots(const FOutlawsLevel& Lv, FBlastWall& B, double WallR)
	{
		const FOutlawsSector& S = Lv.Sectors[B.Sector];
		const FOutlawsWall& W = S.Walls[B.Wall];
		const FVector2D A = S.Verts[W.V1], C = S.Verts[W.V2], M = (A + C) * 0.5;
		FVector2D N = FVector2D(C.Y - A.Y, A.X - C.X).GetSafeNormal();
		if (!Lv.ContainsXZ(S, M.X + N.X * 0.3, M.Y + N.Y * 0.3)) N = -N;
		const FVector2D P = M + N * 1.0;
		B.In = FVector(P.X, Lv.SurfaceY(S, P, true) + 0.3, P.Y);
		B.InD = (Oct(A, P) + Oct(C, P)) * 0.5;
		double Sum = 0.0;
		for (const FVector2D& V : S.Verts) Sum += Oct(V, P);
		B.Gate = Sum / FMath::Max(1, S.Verts.Num());
		const double F0 = Lv.SurfaceY(S, P, true);
		for (double D = 2.0; D <= 45.0 && !B.bOut; D += 0.5)
		{
			for (const double Sg : { 1.0, -1.0 })
			{
				const FVector2D Q = M + N * (D * Sg);
				const double Dq = (Oct(A, Q) + Oct(C, Q)) * 0.5;
				if (Dq < WallR + 0.5 || B.bOut) continue;
				for (const FOutlawsSector& T : Lv.Sectors)
				{
					if (!Lv.ContainsXZ(T, Q.X, Q.Y)) continue;
					const double Fq = Lv.SurfaceY(T, Q, true);
					if (FMath::Abs(Fq - F0) > 4.0 || Lv.SurfaceY(T, Q, false) - Fq < 2.0) continue;
					B.Out = FVector(Q.X, Fq + 0.3, Q.Y);
					B.OutD = Dq;
					B.bOut = true;
					break;
				}
			}
		}
	}

	/** The trigger's client elevators (by name) */
	TArray<int32> ClientElevators(const FOutlawsInfRuntime& R, int32 Trigger)
	{
		TArray<int32> Out;
		const FOutlawsTrigger& T = R.Triggers[Trigger];
		const FOutlawsInfClass& C = R.Inf.Items[T.Item].Classes[T.ClassIndex];
		for (const FString& Client : C.Clients)
		{
			FString Nm, Id;
			FOutlawsInfFile::SplitNameId(Client, Nm, Id);
			for (int32 e = 0; e < R.Elevators.Num(); ++e) if (R.Elevators[e].Name.Equals(Nm, ESearchCase::IgnoreCase)) Out.AddUnique(e);
		}
		return Out;
	}

	/** Explode's INF events for a blast at At (Outlaws x, y, z), sectors as OutlawsLevelActorShoot.cpp sends them and, but
	 *  for -OLOldBlastWalls, the walls; the walls told (BlastWallEvents' list) */
	TArray<FIntVector> BlastAt(FOutlawsInfRuntime& R, const FOutlawsLevel& Authored, const FVector& At, double WallR, double Radius, bool bOld)
	{
		TArray<FIntVector> Told;
		const FOutlawsSector* Own = Authored.SectorAt(At.X, At.Y + 0.5, At.Z);
		if (!Own) return Told;
		R.SectorEvent(Own->Index, OutlawsInf::EvExplosion | 0x8000);
		TSet<int32> Minded;
		for (const TPair<int32, TArray<int32>>& Kv : R.TriggersBySector) Minded.Add(Kv.Key);
		for (const TPair<int32, TArray<int32>>& Kv : R.ElevatorsBySector) Minded.Add(Kv.Key);
		for (const int32 s : Minded)
		{
			if (s == Own->Index || !R.Live.Sectors.IsValidIndex(s)) continue;
			const FOutlawsSector& S = R.Live.Sectors[s];
			FBox B(ForceInit);
			for (const FVector2D& V : S.Verts) { B += FVector(V.X, S.Floor.Y, V.Y); B += FVector(V.X, S.Ceil.Y, V.Y); }
			if (B.IsValid && B.ComputeSquaredDistanceToPoint(At) <= WallR * WallR) R.SectorEvent(s, OutlawsInf::EvExplosion);
		}
		if (!bOld)
		{
			R.BlastWallEvents(Own->Index, At.X, At.Z, WallR, OutlawsInf::EvExplosion, &Told);
			R.BlastWallEvents(Own->Index, At.X, At.Z, Radius, 0x8000, &Told);
		}
		return Told;
	}
}

OL_SHOT_TEST(OLBlastWallsProbe, 1800)
{
	static int32 Passes = 0, Fails = 0, Case = 0;
	static float Wait = 0.f, WatchTo = 0.f;
	static double WallR = 15.0, Radius = 50.0;
	static bool bStick = false, bMoved = false;
	static TArray<FBlastWall> Here;
	static TArray<double> Start;
	static FVector StickLast = FVector::ZeroVector;
	if (!L || ShotStep < 0) return;
	const bool bOld = OLFlags::Param(TEXT("OLOldBlastWalls"));
	const FOutlawsInfRuntime* Inf = L->DebugInf();
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT blastwalls check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT blastwalls RESULT %s (%d passed, %d failed, %s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			bOld ? TEXT("-OLOldBlastWalls") : TEXT("new"));
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	auto LastStick = [this]() -> FThrown* { for (int32 i = Thrown.Num() - 1; i >= 0; --i) if (Thrown[i].Type == 1 && Thrown[i].Fuse >= 0.f) return &Thrown[i]; return nullptr; };
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
			if (A && A->GetText(TEXT("pdynam.itm"), Pd))
			{
				OutlawsFormats::ParseItm(Pd, Func, D);
				if (D.Contains(TEXT("WALL_RADIUS"))) WallR = FCString::Atod(*D[TEXT("WALL_RADIUS")]);
				if (D.Contains(TEXT("RADIUS"))) Radius = FCString::Atod(*D[TEXT("RADIUS")]);
			}
		}
		// (0) every level in the install
		TArray<FString> Lvts;
		if (A) A->ListByExtension(TEXT(".lvt"), Lvts);
		Lvts.Sort();
		int32 Levels = 0, Listed = 0, Unmasked = 0, InOk = 0, OutOk = 0, NoOut = 0, Reach = 0;
		FString List, Bad;
		for (const FString& Lvt : Lvts)
		{
			const FString Lvl = FPaths::GetBaseFilename(Lvt);
			FString LvtText, InfText;
			if (!A->GetText(Lvt, LvtText) || !A->GetText(Lvl + TEXT(".inf"), InfText)) continue;
			++Levels;
			FOutlawsLevel Lv;
			Lv.ParseLvt(LvtText);
			FOutlawsInfRuntime R0;
			R0.Init(Lv, InfText);
			const bool bThis = Lvl.Equals(L->LevelName, ESearchCase::IgnoreCase);
			for (int32 t = 0; t < R0.Triggers.Num(); ++t)
			{
				const FOutlawsTrigger& T = R0.Triggers[t];
				if (!T.bLine || !R0.Live.Sectors.IsValidIndex(T.Sector) || !R0.Live.Sectors[T.Sector].Walls.IsValidIndex(T.Wall)) continue;
				const FOutlawsInfClass& C = R0.Inf.Items[T.Item].Classes[T.ClassIndex];
				if (!C.bHasEventMask) { ++Unmasked; continue; }
				if (!(T.Mask & BlastWallBits)) continue;
				++Listed;
				FBlastWall B;
				B.Trigger = t;
				B.Sector = T.Sector;
				B.Wall = T.Wall;
				B.Name = R0.Inf.Items[T.Item].Name;
				B.Id = R0.Live.Sectors[T.Sector].Walls[T.Wall].Id;
				Spots(R0.Live, B, WallR);
				List += FString::Printf(TEXT(" %s:%s#%s(mask %u)"), *Lvl, *B.Name, *B.Id, T.Mask);
				FString Each;
				bool bRow = true;
				for (const bool bIn : { true, false })
				{
					if (!bIn && !B.bOut) { ++NoOut; Each += TEXT(" [no spot out of reach]"); continue; }
					FOutlawsInfRuntime R;
					R.Init(Lv, InfText);
					const TArray<int32> Es = ClientElevators(R, t);
					TArray<double> E0;
					for (const int32 e : Es) E0.Add(R.Elevators[e].Current);
					const FVector At = bIn ? B.In : B.Out;
					const TArray<FIntVector> Told = BlastAt(R, Lv, At, WallR, Radius, bOld);
					const bool bTaken = Told.ContainsByPredicate([&](const FIntVector& V) { return V.X == B.Sector && V.Y == B.Wall && V.Z; });
					bool bMv = false;
					for (int32 k = 0; k < 300; ++k)
					{
						R.Tick(0.05);
						for (int32 j = 0; j < Es.Num(); ++j) bMv |= R.Elevators[Es[j]].Target >= 0 || FMath::Abs(R.Elevators[Es[j]].Current - E0[j]) > 0.01;
					}
					// (the exe tells a wall only from a sector whose corners lie in reach on average: civlwar1's CANNONT
					// sector is the whole long gun line, its corners some 95 units off, so the original's cannons never
					// take a blast; the mill's gate sectors are a few units across)
					const bool bWant = bIn && !bOld && B.Gate < WallR;
					const bool bOk = bWant ? bTaken && bMv : !bTaken && !bMv;
					if (bIn && bWant && bOk) ++Reach;
					if (bIn) InOk += bOk ? 1 : 0; else OutOk += bOk ? 1 : 0;
					bRow &= bOk;
					const FOutlawsSector* Own = Lv.SectorAt(At.X, At.Y + 0.5, At.Z);
					Each += FString::Printf(TEXT(" [%s: x %.2f z %.2f in %d, the wall's ends %.2f units on average, %d walls told, taken %d, its %d client(s) %s]"), bIn ? TEXT("in front") : TEXT("out of reach"),
						At.X, At.Z, Own ? Own->Index : -1, bIn ? B.InD : B.OutD, Told.Num(), bTaken ? 1 : 0, Es.Num(), bMv ? TEXT("moved") : TEXT("stayed"));
				}
				if (!bRow) Bad += FString::Printf(TEXT(" %s:%s#%s"), *Lvl, *B.Name, *B.Id);
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT blastwalls (0) %s %s #%s (sector %d wall %d, its corners %.2f units from the stick on average):%s"), *Lvl, *B.Name, *B.Id, B.Sector, B.Wall, B.Gate, *Each);
				if (bThis) Here.Add(B);
			}
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT blastwalls (0) %d levels; %d line items list a blast's wall events:%s; %d line triggers with no EVENT_MASK (left out)"), Levels, Listed, *List, Unmasked);
		Check(Levels > 0 && Listed == 9, FString::Printf(TEXT("(0) the install's %d levels: the line items that list the blast are the nine (civlwar1's CANNONT x3, the mill's SEWERGATE1T..6T): %d"), Levels, Listed));
		Check(Listed > 0 && InOk == Listed && OutOk == Listed - NoOut && Reach == (bOld ? 0 : 6), FString::Printf(TEXT("(0) %s, out of reach none does: right in front %d of %d (%d fired), out of reach %d of %d%s"),
			bOld ? TEXT("a stick in front of each fires none (-OLOldBlastWalls: the sectors alone)") : TEXT("a stick in front of each of the six sewer gates fires it, of the three cannons none (their sector's corners lie out of reach, as in the original)"),
			InOk, Listed, Reach, OutOk, Listed - NoOut, Bad.IsEmpty() ? TEXT("") : *(TEXT("; wrong:") + Bad)));
		if (!Here.Num())
		{
			Check(false, FString::Printf(TEXT("(1) a blast wall on the level loaded (%s)"), *L->LevelName));
			Finish();
			return;
		}
		Case = 0;
		Wait = ShotTime + 0.5f;
		ShotStep = 1;
		return;
	}
	case 1:
	{
		// (1) each wall of this level: the stick out of reach first (a SINGLE trigger fires once), then in front
		if (ShotTime < Wait) return;
		if (Case >= Here.Num() * 2) { Finish(); return; }
		const FBlastWall& B = Here[Case / 2];
		const bool bIn = Case % 2 == 1;
		if (!bIn && !B.bOut) { ++Case; return; }
		const TArray<int32> Es = ClientElevators(*Inf, B.Trigger);
		for (const int32 e : Es) if (Inf->Elevators[e].Target >= 0) return;     // (the last one's clients settle first)
		Equip(1, 6);
		Mag[1] = 1;
		bFuseLit[1] = true;
		Fuse[1] = 2.5f;
		Wait = ShotTime + 0.5f;
		ShotStep = 2;
		return;
	}
	case 2:
	{
		if (ShotTime < Wait) return;
		const FBlastWall& B = Here[Case / 2];
		const bool bIn = Case % 2 == 1;
		const int32 Before = Thrown.Num();
		ThrowHeld(1, false, true);
		FThrown* T = Thrown.Num() > Before ? LastStick() : nullptr;
		if (T)
		{
			const FVector At = bIn ? B.In : B.Out;
			T->Pos = FOutlawsGeometry::ToUE(FVector2D(At.X, At.Z), At.Y + 0.2);
			T->Vel = FVector::ZeroVector;
			T->Fuse = 1.0f;
			if (AActor* Ac = T->Actor.Get()) Ac->SetActorLocation(T->Pos);
		}
		Start.Reset();
		for (const int32 e : ClientElevators(*Inf, B.Trigger)) Start.Add(Inf->Elevators[e].Current);
		bStick = T != nullptr;
		bMoved = false;
		if (!bStick)
		{
			Check(false, FString::Printf(TEXT("(1) %s #%s: a lit stick set down"), *B.Name, *B.Id));
			++Case;
			ShotStep = 1;
			return;
		}
		WatchTo = 0.f;
		ShotStep = 3;
		return;
	}
	case 3:
	{
		const FBlastWall& B = Here[Case / 2];
		const bool bIn = Case % 2 == 1;
		if (const FThrown* T = LastStick()) { StickLast = T->Pos; return; }
		if (WatchTo <= 0.f) WatchTo = ShotTime + 3.f;
		const TArray<int32> Es = ClientElevators(*Inf, B.Trigger);
		for (int32 j = 0; j < Es.Num() && j < Start.Num(); ++j) bMoved |= Inf->Elevators[Es[j]].Target >= 0 || FMath::Abs(Inf->Elevators[Es[j]].Current - Start[j]) > 0.01;
		if (ShotTime < WatchTo) return;
		const bool bWant = bIn && !bOld && B.Gate < WallR;     // (the cannons: not, as (0))
		// (where it went off, the sector there, the wall's sector's corners from it as the level stands now, the trigger armed)
		const FVector Bl = FOutlawsGeometry::ToOutlaws(StickLast);
		const FOutlawsSector* At = Inf->Live.SectorAt(Bl.X, Bl.Y + 0.5, Bl.Z);
		double Sum = 0.0;
		const FOutlawsSector& Ws = Inf->Live.Sectors[B.Sector];
		for (const FVector2D& V : Ws.Verts) Sum += Oct(V, FVector2D(Bl.X, Bl.Z));
		Check(bMoved == bWant && Es.Num() > 0, FString::Printf(TEXT("(1) %s %s #%s: a lit stick set down %s (%.2f units from the wall's ends on average; it went off at x %.2f y %.2f z %.2f in sector %d, the wall's sector %d's corners %.2f off on average, the trigger %s): its %d client(s) %s"),
			*L->LevelName, *B.Name, *B.Id, bIn ? TEXT("in front of it") : TEXT("out of reach"), bIn ? B.InD : B.OutD, Bl.X, Bl.Y, Bl.Z, At ? At->Index : -1, B.Sector, Sum / FMath::Max(1, Ws.Verts.Num()),
			Inf->Triggers[B.Trigger].bMaster ? TEXT("armed") : TEXT("not armed"), Es.Num(), bMoved ? TEXT("moved (it fired)") : TEXT("stayed")));
		++Case;
		Wait = ShotTime + 0.5f;
		ShotStep = 1;
		return;
	}
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
