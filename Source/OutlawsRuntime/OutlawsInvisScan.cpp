// (BUG-INVISWALLS, -OLInvisScan) the player's capsule swept over every floor, in eight directions from points on a grid: each
// sweep that stops where the original's sector walk would have gone on (the next sector's floor within a step, room for the
// head, no NOPASS) is an invisible wall, listed by what stopped it and the wall it stands on.
#include "OutlawsLevelActor.h"
#include "OutlawsFlags.h"
#include "OutlawsLevel.h"
#include "OutlawsGeometry.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsRuntime.h"
#include "ProceduralMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/Pawn.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#if WITH_OUTLAWS_TESTS

namespace
{
	struct FInvisHit
	{
		int32 N = 0;
		TArray<FString> Examples;
	};

	/** The original's walk from (P, feet Y) in sector From to Q: whether it gets there, and why not. */
	bool JediWalk(const FOutlawsLevel& L, int32 From, const FVector2D& P0, const FVector2D& Q, double Feet, double Step, double Tall, FString& Why)
	{
		int32 Cur = From;
		FVector2D P = P0;
		for (int32 Hop = 0; Hop < 8; ++Hop)
		{
			const FOutlawsSector& S = L.Sectors[Cur];
			const FVector2D Seg = Q - P;
			if (Seg.SizeSquared() < 1e-12) return true;
			double BestT = 2.0;
			int32 BestW = -1;
			for (int32 w = 0; w < S.Walls.Num(); ++w)
			{
				const FOutlawsWall& W = S.Walls[w];
				if (!S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) continue;
				const FVector2D Q1 = S.Verts[W.V1], E = S.Verts[W.V2] - Q1;
				const double Den = FVector2D::CrossProduct(Seg, E);
				if (FMath::Abs(Den) < 1e-12) continue;
				const FVector2D QP = Q1 - P;
				const double T = FVector2D::CrossProduct(QP, E) / Den, U = FVector2D::CrossProduct(QP, Seg) / Den;
				if (T > 1e-9 && T <= 1.0 && U >= -1e-9 && U <= 1.0 + 1e-9 && T < BestT) { BestT = T; BestW = w; }
			}
			if (BestW < 0) return true;
			const FOutlawsWall& W = S.Walls[BestW];
			const FVector2D Hit = P + Seg * BestT;
			int32 Next = W.Adjoin;
			if (L.Sectors.IsValidIndex(W.DAdjoin))
			{
				const FOutlawsSector& DS = L.Sectors[W.DAdjoin];
				if (Feet + Step >= L.SurfaceY(DS, Hit, true) - 0.01 && Feet < L.SurfaceY(DS, Hit, false)) Next = W.DAdjoin;
			}
			if (!L.Sectors.IsValidIndex(Next)) { Why = TEXT("solid"); return false; }
			if (W.Flags1 & OutlawsFlags::WallNoPass) { Why = TEXT("nopass"); return false; }
			const FOutlawsSector& N = L.Sectors[Next];
			const double NF = L.SurfaceY(N, Hit, true), NC = L.SurfaceY(N, Hit, false);
			if (NF > Feet + Step) { Why = TEXT("step"); return false; }
			if (NC - FMath::Max(NF, Feet) < Tall) { Why = TEXT("head"); return false; }
			Feet = NF;                // (up the step, or down to the floor beyond)
			P = Hit + Seg.GetSafeNormal() * 1e-5;
			Cur = Next;
		}
		return true;
	}

	/** The wall nearest a point in plan, among the sectors whose height holds Y: "s<sector> w<wall> ...". */
	FString WallNear(const FOutlawsLevel& L, const FVector2D& P, double Y)
	{
		double Best = 0.6;
		const FOutlawsSector* BS = nullptr;
		int32 BW = -1;
		for (const FOutlawsSector& S : L.Sectors)
		{
			if (S.Verts.Num() < 3) continue;
			for (int32 w = 0; w < S.Walls.Num(); ++w)
			{
				const FOutlawsWall& W = S.Walls[w];
				if (!S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) continue;
				const FVector2D A = S.Verts[W.V1], B = S.Verts[W.V2];
				const FVector2D AB = B - A;
				const double T = FMath::Clamp(FVector2D::DotProduct(P - A, AB) / FMath::Max(1e-12, AB.SizeSquared()), 0.0, 1.0);
				const double D = FVector2D::Distance(P, A + AB * T);
				const FVector2D At = A + AB * T;
				if (Y < L.SurfaceY(S, At, true) - 0.5 || Y > L.SurfaceY(S, At, false) + 0.5) continue;
				if (D < Best) { Best = D; BS = &S; BW = w; }
			}
		}
		if (!BS) return TEXT("no wall near");
		const FOutlawsWall& W = BS->Walls[BW];
		const FOutlawsSector* N = L.Sector(W.Adjoin);
		return FString::Printf(TEXT("s%d w%d %s adj %d dadj %d flags 0x%x/0x%x sflags 0x%x mid %s top %s bot %s sign %s fl/ce %.1f/%.1f beyond %.1f/%.1f%s"),
			BS->Index, BW, *BS->Name, W.Adjoin, W.DAdjoin, W.Flags1, W.Flags2, BS->Flags, *L.TexStem(W.Mid), *L.TexStem(W.Top), *L.TexStem(W.Bot),
			W.Overlay >= 0 ? *L.TexStem(W.Overlay) : TEXT("-"), BS->Floor.Y, BS->Ceil.Y, N ? N->Floor.Y : 0.0, N ? N->Ceil.Y : 0.0,
			N && N->NoWalls() ? TEXT(" (beyond: no walls)") : TEXT(""));
	}
}

FString AOutlawsLevelActor::DebugInvisScan(const APawn* Body, int32& OutCandidates) const
{
	OutCandidates = 0;
	const FOutlawsLevel* LP = LiveLevel();
	UWorld* World = GetWorld();
	const UCapsuleComponent* Cap = Body ? Cast<UCapsuleComponent>(Body->GetRootComponent()) : nullptr;
	if (!LP || !World || !Cap) return TEXT("no level");
	const FOutlawsLevel& L = *LP;
	const double U = FOutlawsGeometry::UnitsToCm;
	const double R = Cap->GetScaledCapsuleRadius(), H = Cap->GetScaledCapsuleHalfHeight();
	const double Tall = 2.0 * H / U;          // (5.8 units)
	const double Step = 3.5;                  // the step a walk climbs, in units (the player's MaxStepHeight 105 cm)
	// the body from a step over the feet to the head: what is under it is climbed (the walk steps 105 cm), not walked into
	const double Lift = 100.0;
	const double Hs = FMath::Max(R, H - Lift * 0.5);
	const FCollisionShape Shape = FCollisionShape::MakeCapsule(float(R), float(Hs));
	FCollisionQueryParams QP(SCENE_QUERY_STAT(OLInvisScan), true, Body);
	const FCollisionResponseParams RP(Cap->GetCollisionResponseToChannels());
	const ECollisionChannel Ch = Cap->GetCollisionObjectType();
	const double Grid = 3.0, Reach = 3.0;     // units
	// the sectors a walk reaches from the start, by the original's rules at each wall's two ends (a step up of 3.5 units at
	// most, any drop, room for the head, no NOPASS on the side left; a lift's sector reaches all its neighbours): a hit in
	// a sector no walk reaches (a canyon's rim) is listed apart
	TSet<int32> Reached;
	double ReachStep = Step;                  // (-OLInvisReach=10: with a jump at every wall)
	OLFlags::Value(TEXT("OLInvisReach"), ReachStep);
	{
		const FVector BL = Body->GetActorLocation() - FVector(0.0, 0.0, H);
		const int32 Start = L.Which3D(FOutlawsGeometry::ToOutlaws(BL, 0.2));
		TArray<int32> Todo;
		if (Start >= 0) { Reached.Add(Start); Todo.Add(Start); }
		while (Todo.Num() > 0)
		{
			const FOutlawsSector& S = L.Sectors[Todo.Pop()];
			const bool bLift = Inf && Inf->ElevatorsBySector.Contains(S.Index);
			for (const FOutlawsWall& W : S.Walls)
			{
				if (!S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) continue;
				for (const int32 NIdx : { W.Adjoin, W.DAdjoin })
				{
					const FOutlawsSector* N = L.Sector(NIdx);
					if (!N || Reached.Contains(NIdx) || N->NoWalls()) continue;
					bool bOk = bLift || (Inf && Inf->ElevatorsBySector.Contains(NIdx));
					if (!bOk && !(W.Flags1 & OutlawsFlags::WallNoPass))
					{
						for (const FVector2D& E : { S.Verts[W.V1], S.Verts[W.V2], (S.Verts[W.V1] + S.Verts[W.V2]) * 0.5 })
						{
							const double SF = L.SurfaceY(S, E, true), NF = L.SurfaceY(*N, E, true), NC = L.SurfaceY(*N, E, false);
							if (NF <= SF + ReachStep && NC - FMath::Max(NF, SF) >= Tall) { bOk = true; break; }
						}
					}
					if (bOk) { Reached.Add(NIdx); Todo.Add(NIdx); }
				}
			}
		}
	}
	TMap<const UPrimitiveComponent*, int32> PartOf;      // an overlapping room's own meshes, by its sector
	for (const TPair<int32, TArray<FOverlapPart>>& Kv : OverlapParts)
	{
		for (const FOverlapPart& Part : Kv.Value) if (Part.Comp.IsValid()) PartOf.Add(Part.Comp.Get(), Kv.Key);
	}
	TMap<FString, FInvisHit> Hits;
	TMap<FString, int32> Seen;                // (sweeps by outcome)
	int32 ReachedHits = 0;
	int32 Points = 0, Sweeps = 0;
	const double T0 = FPlatformTime::Seconds();
	for (const FOutlawsSector& S : L.Sectors)
	{
		if (S.Verts.Num() < 3 || S.NoWalls() || (S.Flags & OutlawsFlags::Water)) continue;
		if (FPlatformTime::Seconds() - T0 > 400.0) { Seen.FindOrAdd(TEXT("time out"))++; break; }
		FBox2D Box(ForceInit);
		for (const FVector2D& V : S.Verts) Box += V;
		for (double X = FMath::FloorToDouble(Box.Min.X / Grid) * Grid + Grid * 0.5; X < Box.Max.X; X += Grid)
		{
			for (double Z = FMath::FloorToDouble(Box.Min.Y / Grid) * Grid + Grid * 0.5; Z < Box.Max.Y; Z += Grid)
			{
				if (!L.ContainsXZ(S, X, Z)) continue;
				const FVector2D P(X, Z);
				const double F = L.SurfaceY(S, P, true), C = L.SurfaceY(S, P, false);
				if (C - F < Tall + 0.2) continue;
				if (L.Which3D(X, F + 0.1, Z) != S.Index) continue;
				const FVector Floor = FOutlawsGeometry::ToUE(X, F, Z);
				const FVector Centre = Floor + FVector(0.0, 0.0, Hs + Lift);
				++Points;
				// the rooms sharing space the body would pass through from here (ApplyOverlapChoice: not the body's own, nor
				// one an opening within reach leads into)
				FCollisionQueryParams QPP = QP;
				TSet<int32> SeenHere;           // (what the eye floods from here: a room sharing space is drawn only then)
				if (OverlapParts.Num() > 0)
				{
					TSet<int32> Near;
					NearFlood(L, S.Index, P, Near);
					PortalFlood(L, S.Index, P, F + 5.3, SeenHere);
					for (const TPair<int32, TArray<FOverlapPart>>& Kv : OverlapParts)
					{
						if (Kv.Key == S.Index || Near.Contains(Kv.Key)) continue;
						for (const FOverlapPart& Part : Kv.Value) if (Part.Comp.IsValid()) QPP.AddIgnoredComponent(Part.Comp.Get());
					}
				}
				TArray<FOverlapResult> Ov;
				if (World->OverlapMultiByChannel(Ov, Centre, FQuat::Identity, Ch, Shape, QPP, RP))
				{
					const UPrimitiveComponent* OC = Ov.Num() ? Ov[0].GetComponent() : nullptr;
					FString ON = OC ? OC->GetName() : TEXT("?");
					int32 Cut = ON.Len();
					while (Cut > 0 && (FChar::IsDigit(ON[Cut - 1]) || ON[Cut - 1] == TEXT('_'))) --Cut;
					Seen.FindOrAdd(TEXT("start blocked by ") + ON.Left(Cut))++;
					if (OC && !ON.StartsWith(TEXT("OutlawsWorld")) && !(OC->GetOwner() && OC->GetOwner()->IsA<APawn>()))
					{
						// standing in something with room to stand by the original: listed with the hits
						const int32* Part = PartOf.Find(OC);
						const FString Key = FString::Printf(TEXT("%sINSIDE %s%s | in s%d"), Reached.Contains(S.Index) ? TEXT("") : TEXT("(unreached) "), *ON.Left(Cut),
							Part ? *FString::Printf(TEXT(" of s%d%s"), *Part, SeenHere.Contains(*Part) ? TEXT("") : TEXT(" (ROOM UNSEEN)")) : TEXT(""), S.Index);
						FInvisHit& E = Hits.FindOrAdd(Key);
						if (E.N++ == 0) E.Examples.Add(FString::Printf(TEXT("feet %.1f,%.1f,%.1f, floor/ceiling %.2f/%.2f"), Floor.X, Floor.Y, Floor.Z + 2.0, F, C));
						if (Reached.Contains(S.Index)) ++ReachedHits;
						++OutCandidates;
					}
					continue;
				}
				for (int32 d = 0; d < 8; ++d)
				{
					const double Ang = d * PI / 4.0;
					const FVector2D Dir(FMath::Cos(Ang), FMath::Sin(Ang));
					const FVector End = FOutlawsGeometry::ToUE(X + Dir.X * Reach, F, Z + Dir.Y * Reach) + FVector(0.0, 0.0, Hs + Lift);
					FHitResult Hit;
					++Sweeps;
					if (!World->SweepSingleByChannel(Hit, Centre, End, FQuat::Identity, Ch, Shape, QPP, RP))
					{
						Seen.FindOrAdd(TEXT("clear"))++;
						continue;
					}
					if (Hit.bStartPenetrating) { Seen.FindOrAdd(TEXT("start penetrating"))++; continue; }
					if (FMath::Abs(Hit.ImpactNormal.Z) > 0.6 && Hit.GetComponent() && Hit.GetComponent()->GetName().StartsWith(TEXT("OutlawsWorld"))) { Seen.FindOrAdd(TEXT("floor or ceiling"))++; continue; }
					const AActor* HitActor = Hit.GetActor();
					if (HitActor && HitActor->IsA<APawn>()) { Seen.FindOrAdd(TEXT("a pawn"))++; continue; }
					// the original's walk to just past the contact
					const FVector2D Imp = FOutlawsGeometry::ToOutlawsPlan(Hit.ImpactPoint);
					const FVector2D Past = Imp + Dir * 0.5;
					FString Why;
					if (!JediWalk(L, S.Index, P, Past, F, Step, Tall, Why))
					{
						Seen.FindOrAdd(TEXT("blocked as the original: ") + Why)++;
						const UPrimitiveComponent* BC = Hit.GetComponent();
						if (BC && !BC->IsVisible() && Why == TEXT("solid"))
						{
							// blocked where the original blocks, by a face that is not drawn (the original drew that wall)
							const FString BKey = FString::Printf(TEXT("%sUNDRAWN SOLID %s | %s"), Reached.Contains(S.Index) ? TEXT("") : TEXT("(unreached) "), *BC->GetName(),
								*WallNear(L, Imp, Hit.ImpactPoint.Z / U));
							FInvisHit& E = Hits.FindOrAdd(BKey);
							if (E.N++ == 0) E.Examples.Add(FString::Printf(TEXT("from s%d feet %.1f,%.1f,%.1f yaw %.1f"), S.Index, Floor.X, Floor.Y, Floor.Z + 2.0,
								FMath::RadiansToDegrees(FMath::Atan2(End.Y - Centre.Y, End.X - Centre.X))));
						}
						continue;
					}
					const UPrimitiveComponent* Comp = Hit.GetComponent();
					FString Name = Comp ? Comp->GetName() : TEXT("?");
					// (unique-name numbers off: one label per kind of component)
					int32 Cut = Name.Len();
					while (Cut > 0 && (FChar::IsDigit(Name[Cut - 1]) || Name[Cut - 1] == TEXT('_'))) --Cut;
					const int32* Part = Comp ? PartOf.Find(Comp) : nullptr;
					const FString Kind = FString::Printf(TEXT("%s%s%s [%s]"), *Name.Left(Cut), Comp && !Comp->IsVisible() ? TEXT(" (hidden)") : TEXT(""),
						Part ? (SeenHere.Contains(*Part) ? TEXT(" (room seen)") : TEXT(" (ROOM UNSEEN: solid, not drawn)")) : TEXT(""),
						HitActor ? *HitActor->GetClass()->GetName() : TEXT("-"));
					const double ImpY = Hit.ImpactPoint.Z / U;
					const FString Wall = WallNear(L, Imp, ImpY);
					const bool bReached = Reached.Contains(S.Index);
					if (bReached) ++ReachedHits;
					const FString Key = (bReached ? FString() : FString(TEXT("(unreached) "))) + Kind + TEXT(" | ") + Wall;
					FInvisHit& E = Hits.FindOrAdd(Key);
					if (E.N++ == 0)
					{
						const FVector Feet = Floor + FVector(0.0, 0.0, 2.0);
						const double Yaw = FMath::RadiansToDegrees(FMath::Atan2(End.Y - Centre.Y, End.X - Centre.X));
						E.Examples.Add(FString::Printf(TEXT("from s%d feet %.1f,%.1f,%.1f yaw %.1f, contact %.1f,%.1f,%.1f (height %.2f u, %.0f cm over the feet)"),
							S.Index, Feet.X, Feet.Y, Feet.Z, Yaw, Hit.ImpactPoint.X, Hit.ImpactPoint.Y, Hit.ImpactPoint.Z, ImpY, Hit.ImpactPoint.Z - Floor.Z));
					}
					++OutCandidates;
				}
			}
		}
	}
	TArray<FString> Keys;
	Hits.GetKeys(Keys);
	Keys.Sort([&Hits](const FString& A, const FString& B) { return Hits[A].N > Hits[B].N; });
	TMap<FString, int32> ByKind;
	for (const FString& K : Keys)
	{
		FString Kind, Wall;
		K.Split(TEXT(" | "), &Kind, &Wall);
		ByKind.FindOrAdd(Kind) += Hits[K].N;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT invis hit x%d: %s; %s"), Hits[K].N, *K, Hits[K].Examples.Num() ? *Hits[K].Examples[0] : TEXT(""));
	}
	FString Out = FString::Printf(TEXT("%d in sectors a walk reaches (%d of %d sectors); %d points, %d sweeps, %d walls, %.1f s;"),
		ReachedHits, Reached.Num(), L.Sectors.Num(), Points, Sweeps, Keys.Num(), FPlatformTime::Seconds() - T0);
	for (const TPair<FString, int32>& Kv : ByKind) Out += FString::Printf(TEXT(" [%s] %d;"), *Kv.Key, Kv.Value);
	for (const TPair<FString, int32>& Kv : Seen) Out += FString::Printf(TEXT(" (%s %d)"), *Kv.Key, Kv.Value);
	return Out;
}
#endif // WITH_OUTLAWS_TESTS
