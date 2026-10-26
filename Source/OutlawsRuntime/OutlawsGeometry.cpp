#include "OutlawsGeometry.h"
#include "OutlawsFlags.h"
#include "OutlawsLevel.h"
#include "OutlawsTriangulate.h"
#include "OutlawsRuntime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace
{
	using namespace OutlawsFlags;

	// (BUG-INVISWALLS) how far up a walker gets over a wall: the Dark Forces jump (6.3 units) and a step (3.5); a body's height
	constexpr double NoPassReachUnits = 10.0;
	constexpr double BodyUnits = 5.8;

	struct FOpening
	{
		double B1, B2, T1, T2;
		double Low() const { return FMath::Min(FMath::Min(B1, B2), FMath::Min(T1, T2)); }
		double High() const { return FMath::Max(FMath::Max(B1, B2), FMath::Max(T1, T2)); }
	};

	// Python's clamp: max(lo, min(hi, v)) (differs from FMath::Clamp when lo > hi)
	inline double PyClamp(double V, double Lo, double Hi) { return FMath::Max(Lo, FMath::Min(Hi, V)); }

	void AddOpening(const FOutlawsLevel& L, const FOutlawsSector* N, const FVector2D& A, const FVector2D& B,
		double FA, double FB, double CA, double CB, TArray<FOpening>& Out)
	{
		if (!N)
		{
			return;
		}
		// a No Walls neighbour (a sky backdrop round the level) leaves the whole height above its floor open, its sky
		// showing over it; the step up to its floor is still this side's wall, as the original draws a wall's bottom part
		// whatever the sector beyond (BUG-WILDWALLS, build 22: the Marshal training canyon's walls up to its rim backdrops
		// were missing all round the start)
		const bool bNoWalls = N->NoWalls();
		const double B1 = PyClamp(L.SurfaceY(*N, A, true), FA, CA);
		const double B2 = PyClamp(L.SurfaceY(*N, B, true), FB, CB);
		const double T1 = bNoWalls ? CA : PyClamp(L.SurfaceY(*N, A, false), FA, CA);
		const double T2 = bNoWalls ? CB : PyClamp(L.SurfaceY(*N, B, false), FB, CB);
		// a hairline opening still counts (townex's iron door stands 0.01 u short of its frame: dropping it
		// turned the wall solid and drew its DEFAULT mid); only a neighbour with no overlap at all is no
		// opening (keeping those drew bands that never existed: phantom walls and ceilings, 2026-09-27)
		if ((T1 - B1) <= 1e-6 && (T2 - B2) <= 1e-6)
		{
			return;
		}
		Out.Add({ B1, B2, T1, T2 });
	}

	bool InsideAnyOpening(const TArray<FOpening>& Os, double M1, double M2)
	{
		const double E = FOutlawsGeometry::MinSegment;
		for (const FOpening& O : Os)
		{
			const bool In1 = FMath::Min(O.B1, O.T1) - E <= M1 && M1 <= FMath::Max(O.B1, O.T1) + E;
			const bool In2 = FMath::Min(O.B2, O.T2) - E <= M2 && M2 <= FMath::Max(O.B2, O.T2) + E;
			if (In1 && In2)
			{
				return true;
			}
		}
		return false;
	}

	enum class EBand { Bot, Top, Mid, MidTex };

	EBand ClassifyBand(const TArray<FOpening>& Os, double M1, double M2)
	{
		const double Eps = FOutlawsGeometry::MinSegment * 0.5;
		bool bBelow = false, bAbove = false, bCross = false;
		for (const FOpening& O : Os)
		{
			const double Lo1 = FMath::Min(O.B1, O.T1), Lo2 = FMath::Min(O.B2, O.T2);
			const double Hi1 = FMath::Max(O.B1, O.T1), Hi2 = FMath::Max(O.B2, O.T2);
			if (M1 <= Lo1 + Eps && M2 <= Lo2 + Eps) bBelow = true;
			else if (M1 >= Hi1 - Eps && M2 >= Hi2 - Eps) bAbove = true;
			else bCross = true;
		}
		if (!bCross && bBelow && !bAbove) return EBand::Bot;
		if (!bCross && bAbove && !bBelow) return EBand::Top;
		const double Avg = (M1 + M2) * 0.5;
		double Lowest = TNumericLimits<double>::Max(), Highest = -TNumericLimits<double>::Max();
		for (const FOpening& O : Os)
		{
			Lowest = FMath::Min(Lowest, O.Low());
			Highest = FMath::Max(Highest, O.High());
		}
		if (Avg < Lowest - FOutlawsGeometry::MinSegment) return EBand::Bot;
		if (Avg > Highest + FOutlawsGeometry::MinSegment) return EBand::Top;
		return EBand::Mid;
	}

	/** Vertical portal (VADJOIN at the same height): 0 = none, 1 = open air, 2 = water surface. */
	int32 VerticalPortal(const FOutlawsLevel& L, const FOutlawsSector& S, bool bFloor)
	{
		const FOutlawsSector* T = L.Sector(S.VAdjoin);
		if (!T)
		{
			return 0;
		}
		if (bFloor && FMath::Abs(S.Floor.Y - T->Ceil.Y) < 1e-3) {}
		else if (!bFloor && FMath::Abs(S.Ceil.Y - T->Floor.Y) < 1e-3) {}
		else return 0;
		return ((S.Flags | T->Flags) & Water) ? 2 : 1;
	}

	/** Wall UVs for the quad (a,bottom),(b,bottom),(b,top),(a,top); OBJ convention (v up). */
	void WallUVs(const FVector2D& A, const FVector2D& B, double BA, double BB, double TA, double TB, double OX, double OY,
		double TW, double TH, double AnchorY, bool bHFlip, bool bTopLeft, FVector2D Out[4])
	{
		const double Len = FMath::Sqrt(FMath::Square(double(B.X - A.X)) + FMath::Square(double(B.Y - A.Y)));
		const double K = FOutlawsGeometry::TexelsPerUnit;
		auto UV = [&](double LocalX, double Y, double TopY)
		{
			// WALLFLAG1_HFLIP mirrors each column inside its tile after the offset is added (TFE rwallFloat and the
			// exe's wall column loops: texel = width - 1 - (u & mask)), so the flipped U runs backwards from the
			// wall's first vertex, offset and all.  Mirroring about the far end instead put a seam at every wall
			// of a run tiled by offsets (the hideout stairwell's planks, 6, 12, 14, 16 ... a wall; user 2026-10-02)
			const double Along = (LocalX * K + OX * K) / TW;
			const double U = bHFlip ? -Along : Along;
			const double V = bTopLeft ? 1.0 - ((TopY - Y) * K + OY * K) / TH : ((Y - AnchorY) * K + OY * K) / TH;
			return FVector2D(U, V);
		};
		Out[0] = UV(0.0, BA, TA);
		Out[1] = UV(Len, BB, TB);
		Out[2] = UV(Len, TB, TB);
		Out[3] = UV(0.0, TA, TA);
	}

	/** Reorder a wall quad so its front faces the given inward direction (UE space). */
	void OrientQuad(FVector Q[4], FVector2D UV[4], const FVector& Inward)
	{
		// the facing from the diagonals: a band beside a sloped floor tapers to nothing at one end (Q3 on Q0), where
		// (Q1 - Q0) x (Q3 - Q0) is zero and the band kept whichever way it was built (half of them facing out of
		// their sector: missing once the world went one-sided; user 2026-09-27)
		const FVector N = FVector::CrossProduct(Q[2] - Q[0], Q[3] - Q[1]);
		if (FVector::DotProduct(N, Inward) < 0.0)
		{
			Swap(Q[0], Q[1]); Swap(Q[2], Q[3]);
			Swap(UV[0], UV[1]); Swap(UV[2], UV[3]);
		}
	}
}

// ------------------------------------------------------------------------------------------------
void FOutlawsMeshSection::AddFace(const FVector* Pts, const FVector2D* ObjUVs, int32 N, float Brightness, bool bReverse, bool bSwirl)
{
	if (N < 3)
	{
		return;
	}
	const int32 Base = Positions.Num();
	// Newell's normal: sound when a corner repeats (a band tapering to nothing at one end)
	FVector Normal = FVector::ZeroVector;
	for (int32 i = 1; i + 1 < N; ++i) Normal += FVector::CrossProduct(Pts[i] - Pts[0], Pts[i + 1] - Pts[0]);
	Normal = Normal.GetSafeNormal();
	// the cross product above uses the exporter's (right-handed) convention; Unreal's front-face
	// normal for the reversed order is the same vector
	const FLinearColor C(Brightness, Brightness, Brightness, bSwirl ? 0.5f : 1.f);
	for (int32 i = 0; i < N; ++i)
	{
		Positions.Add(Pts[i]);
		Normals.Add(Normal);
		UVs.Add(FVector2D(ObjUVs[i].X, 1.0 - ObjUVs[i].Y));
		Colors.Add(C);
	}
	for (int32 i = 1; i + 1 < N; ++i)
	{
		if (bReverse)
		{
			Triangles.Add(Base); Triangles.Add(Base + i + 1); Triangles.Add(Base + i);
		}
		else
		{
			Triangles.Add(Base); Triangles.Add(Base + i); Triangles.Add(Base + i + 1);
		}
	}
}

TMap<int32, int32> FOutlawsGeometry::SectorWaterGroup;
bool FOutlawsGeometry::bOldSignRule = false;
bool FOutlawsGeometry::bOldSkygap = false;

namespace
{
	/** (BUG-SKYGAP) A door drawn as a notch in a ring rather than as a hole: a run of walls onto a door sector, reached
	 *  and left through walls onto the sector itself that meet at the same points (hideout's RHSECRET in 314: v6-v7 in,
	 *  round the door, v11-v12 out, v6 = v12 and v7 = v11).  The whole excursion is cut out, so the room's floor and
	 *  ceiling cover the door's footprint as they do a door's hole ring.  Returns the number of notches cut. */
	int32 CutDoorNotches(const FOutlawsSector& S, TArray<int32>& Ring, TArray<int32>& RingW, const TSet<int32>& Doors)
	{
		auto Same = [&S](int32 A, int32 B) { return FVector2D::DistSquared(S.Verts[A], S.Verts[B]) < 1e-8; };
		int32 Cuts = 0;
		for (bool bCut = true; bCut;)
		{
			bCut = false;
			const int32 N = Ring.Num();
			auto P = [&](int32 k) { return Ring[((k % N) + N) % N]; };
			auto Door = [&](int32 k) { return Doors.Contains(S.Walls[RingW[((k % N) + N) % N]].Adjoin); };
			auto Self = [&](int32 k) { return S.Walls[RingW[((k % N) + N) % N]].Adjoin == S.Index; };
			for (int32 i = 0; i < N && !bCut; ++i)
			{
				if (!Door(i) || Door(i - 1)) continue;
				int32 Len = 1;           // walls in the excursion so far, from vertex a to vertex b
				while (Len < N && Door(i + Len)) ++Len;
				int32 a = i, b = i + Len;
				while (!Same(P(a), P(b)) && Self(a - 1) && Self(b) && Len + 2 < N - 2) { --a; ++b; Len += 2; }
				if (!Same(P(a), P(b))) continue;
				// the way in and out (two-sided walls over the same points) goes with it
				while (Self(a - 1) && Self(b) && Same(P(a - 1), P(b + 1)) && Len + 2 < N - 2) { --a; ++b; Len += 2; }
				if (Len >= N - 2) continue;
				// keep vertex a, drop a+1..b; the wall leaving a is now the one that left b
				TArray<int32> NR, NW;
				for (int32 k = b; k < b + N - Len; ++k)
				{
					NR.Add(P(k == b ? a : k));
					NW.Add(RingW[((k % N) + N) % N]);
				}
				Ring = MoveTemp(NR);
				RingW = MoveTemp(NW);
				++Cuts;
				bCut = true;
			}
		}
		return Cuts;
	}
}

namespace
{
	/** Clip a convex polygon in (along, height) to the side of the line h = H0 + (H1 - H0) * along / Len that bKeepAbove
	 *  names (Sutherland-Hodgman, one edge). */
	void ClipToLine(TArray<FVector2D>& Poly, double H0, double H1, double Len, bool bKeepAbove)
	{
		auto Side = [&](const FVector2D& P) { const double D = P.Y - (H0 + (H1 - H0) * P.X / Len); return bKeepAbove ? D : -D; };
		TArray<FVector2D> Out;
		for (int32 i = 0; i < Poly.Num(); ++i)
		{
			const FVector2D& P = Poly[i];
			const FVector2D& Q = Poly[(i + 1) % Poly.Num()];
			const double SP = Side(P), SQ = Side(Q);
			if (SP >= 0.0) Out.Add(P);
			if ((SP >= 0.0) != (SQ >= 0.0))
			{
				const double T = SP / (SP - SQ);
				Out.Add(P + (Q - P) * T);
			}
		}
		Poly = MoveTemp(Out);
	}

	double PolyArea(const TArray<FVector2D>& Poly)
	{
		double A = 0.0;
		for (int32 i = 0; i < Poly.Num(); ++i) A += FVector2D::CrossProduct(Poly[i], Poly[(i + 1) % Poly.Num()]);
		return FMath::Abs(A) * 0.5;
	}
}

namespace
{
	int32 WaterGroupOf(int32 Sector)
	{
		const int32* G = FOutlawsGeometry::SectorWaterGroup.Find(Sector);
		return G ? *G : -1;
	}
}

void FOutlawsGeometry::AddTo(TMap<FString, FOutlawsMeshSection>& Map, const FString& Tex, bool bMasked, const FVector* Pts, const FVector2D* UVs, int32 N, float Brightness, bool bReverse, bool bNoCollision, bool bSwirl, int32 WaterGroup, int64 AtxWall)
{
	const FString Key = FString(bNoCollision ? TEXT("~") : TEXT("")) + (bMasked ? Tex + TEXT("|m") : Tex) + (WaterGroup >= 0 ? FString::Printf(TEXT("|w%d"), WaterGroup) : FString())
		+ (AtxWall >= 0 ? FString::Printf(TEXT("|x%lld"), AtxWall) : FString());
	FOutlawsMeshSection* S = Map.Find(Key);
	if (!S)
	{
		S = &Map.Add(Key);
		S->Texture = Tex;
		S->bMasked = bMasked;
		S->WaterGroup = WaterGroup;
		S->AtxWall = AtxWall;
	}
	S->AddFace(Pts, UVs, N, Brightness, bReverse, bSwirl);
	for (int32 i = 0; i < N; ++i)
	{
		Bounds += Pts[i];
	}
}

bool FOutlawsGeometry::TriangulateSector(const FOutlawsSector& S, TArray<int32>& OutTris, int32* OutFallbacks, double* OutAreaError,
	const TSet<int32>* FillHolesOf, int32* OutCutNotches)
{
	OutTris.Reset();
	TArray<TArray<int32>> RawRings, RawWalls;
	FOutlawsLevel::LoopsFromWallsEx(S, RawRings, RawWalls);
	// valid vertex indices only
	TArray<int32> Idx;
	for (int32 r = 0; r < RawRings.Num(); ++r)
	{
		bool bOk = RawRings[r].Num() >= 3;
		for (int32 v : RawRings[r]) bOk &= S.Verts.IsValidIndex(v);
		if (bOk) Idx.Add(r);
	}
	if (Idx.Num() == 0)
	{
		return false;
	}
	// largest ring first (outer), the rest are holes; stable like the exporter's sort
	Idx.StableSort([&S, &RawRings](int32 A, int32 B)
	{
		return FMath::Abs(FOutlawsLevel::RingArea(S, RawRings[A])) > FMath::Abs(FOutlawsLevel::RingArea(S, RawRings[B]));
	});
	TArray<TArray<int32>> Rings;
	for (int32 k = 0; k < Idx.Num(); ++k)
	{
		const int32 r = Idx[k];
		if (k > 0 && FillHolesOf && FillHolesOf->Num() > 0)
		{
			// a hole cut for a swinging / sliding door: fill it, the door moves away
			bool bAll = RawWalls[r].Num() > 0;
			for (int32 w : RawWalls[r]) bAll &= FillHolesOf->Contains(S.Walls[w].Adjoin);
			if (bAll) continue;
		}
		if (FillHolesOf && FillHolesOf->Num() > 0 && !bOldSkygap)
		{
			// a door drawn as a notch in the ring is filled the same way (BUG-SKYGAP)
			const int32 Cuts = CutDoorNotches(S, RawRings[r], RawWalls[r], *FillHolesOf);
			if (OutCutNotches) *OutCutNotches += Cuts;
		}
		Rings.Add(RawRings[r]);
	}
	TArray<FVector2D> Pts;
	TArray<int32> Order;
	TArray<int32> Ends;
	double NetArea = 0.0;
	for (int32 r = 0; r < Rings.Num(); ++r)
	{
		for (int32 v : Rings[r])
		{
			Pts.Add(FVector2D(S.Verts[v].X, S.Verts[v].Y));
			Order.Add(v);
		}
		Ends.Add(Pts.Num());
		const double A = FMath::Abs(FOutlawsLevel::RingArea(S, Rings[r]));
		NetArea += r == 0 ? A : -A;
	}
	TArray<int32> Flat;
	const int32 Fallbacks = OutlawsTriangulate::Earcut(Pts, Ends, Flat);
	if (OutFallbacks) *OutFallbacks += Fallbacks;
	if (OutAreaError)
	{
		*OutAreaError += FMath::Abs(NetArea - OutlawsTriangulate::TrianglesArea(Pts, Flat));
	}
	OutTris.Reserve(Flat.Num());
	for (int32 i : Flat)
	{
		OutTris.Add(Order[i]);
	}
	return OutTris.Num() >= 3;
}

void FOutlawsGeometry::Build(const FOutlawsLevel& L, const TMap<FString, FOutlawsTexInfo>& Tex, bool bRev, const FOutlawsBuildFilter* Filter)
{
	static const FOutlawsBuildFilter NoFilter;
	const FOutlawsBuildFilter& F = Filter ? *Filter : NoFilter;
	World.Reset();
	Water.Reset();
	Stats.Reset();
	Bounds = FBox(ForceInit);
	TriangulationFallbacks = 0;
	AreaError = 0.0;
	SuspectSectors.Reset();

	auto Size = [&Tex](const FString& Stem) -> FOutlawsTexInfo
	{
		const FOutlawsTexInfo* I = Tex.Find(Stem);
		return I ? *I : FOutlawsTexInfo();
	};
	auto Bright = [](int32 Lvl) { return float(Lvl) / 31.f; };
	auto Stat = [this](const TCHAR* K) { Stats.FindOrAdd(K)++; };

	// --- floors and ceilings ----------------------------------------------------------------
	TArray<int32> Tris;
	for (const FOutlawsSector& S : L.Sectors)
	{
		if (S.Verts.Num() < 3 || S.Walls.Num() == 0)
		{
			continue;
		}
		if (!F.WantFloor(S.Index) && !F.WantCeil(S.Index))
		{
			continue;
		}
		double SectorErr = 0.0;
		if (!TriangulateSector(S, Tris, &TriangulationFallbacks, &SectorErr, &F.FillHolesOf))
		{
			Stat(TEXT("skipped_sector"));
			continue;
		}
		AreaError += SectorErr;
		if (SectorErr > 0.01)
		{
			SuspectSectors.Add(S.Index);
		}
		const int32 Lvl = FOutlawsLevel::LightLevel(S.Ambient);
		const int32* WaterOf = SectorWaterGroup.Find(S.Index);
		const int32 WG = WaterOf ? *WaterOf : -1;
		for (int32 f = 0; f < 2; ++f)
		{
			const bool bFloor = f == 0;
			const FOutlawsPlane& Plane = bFloor ? S.Floor : S.Ceil;
			if (bFloor ? !F.WantFloor(S.Index) : !F.WantCeil(S.Index)) continue;
			if ((bFloor && (S.Flags & ExtFloor)) || (!bFloor && (S.Flags & ExtCeiling)))
			{
				// sky: drawn as the sky's own surface (one-sided, into the sector), which hides whatever lies beyond.  Not
				// a wall-less backdrop sector's (the sky wrapped round a level, seen in the original only through the
				// openings into it): the mill's 607 laid a sky plane at 20 across the whole level, through ACCESS1T's and
				// STORSWITCH's ceilings (flickering, the sky showing) and the ELLE lift's shaft (user 2026-09-29); the sky
				// dome is that backdrop here
				if (S.NoWalls())
				{
					Stat(TEXT("sky_backdrop_skipped"));
					continue;
				}
				Stat(bFloor ? TEXT("sky_floor") : TEXT("sky_ceiling"));
				for (int32 t = 0; t + 2 < Tris.Num(); t += 3)
				{
					FVector P[3];
					const FVector2D NoUV[3];
					for (int32 k = 0; k < 3; ++k)
					{
						const FVector2D V = S.Verts[Tris[t + k]];
						P[k] = ToUE(V.X, L.SurfaceY(S, V, bFloor), V.Y);
					}
					if ((FVector::CrossProduct(P[1] - P[0], P[2] - P[0]).Z > 0.0) != bFloor) Swap(P[0], P[2]);
					SkyFaces.AddFace(P, NoUV, 3, 1.f, bRev);
				}
				continue;
			}
			const int32 Portal = VerticalPortal(L, S, bFloor);
			if (Portal == 1 || (Portal == 2 && !bFloor))
			{
				Stat(TEXT("vertical_portal_skipped"));
				continue;
			}
			const FString TexStem = L.TexStem(Plane.Tex);
			const FOutlawsTexInfo TI = Size(TexStem);
			const bool bWater = Portal == 2;
			if (bWater) Stat(TEXT("water_surface"));
			// SECFLAG1_SWIRLFLOORTEX: the sector's floor swirls, on any floor. A water surface is also the ceiling of
			// the sector under it (drawn once, seen from both sides), so it swirls from below too; every water
			// surface ripples in its material anyway (user 2026-09-27). No other ceiling swirls
			const bool bSwirl = bFloor && (S.Flags & OutlawsFlags::SwirlFloor) != 0;
			if (bSwirl) Stat(TEXT("swirl_floor"));
			// the texture as the original lays it (the LawMaker viewer's BuildFlatSurfaceUv, matched to the game): the
			// world x/z (or on a slope its own frame) less the plane's offsets, turned by its angle, mirrored on x, and a
			// ceiling mirrored on v as well (user 2026-09-29: floor angles were off, the ceilings too)
			FVector2D FrameO, FrameA, FrameN;
			double Stretch = 1.0;
			L.SurfaceTexFrame(S, bFloor, FrameO, FrameA, FrameN, Stretch);
			const double Rad = FMath::DegreesToRadians(-Plane.Angle);
			const double CosA = FMath::Cos(Rad), SinA = FMath::Sin(Rad);
			for (int32 t = 0; t + 2 < Tris.Num(); t += 3)
			{
				FVector P[3];
				FVector2D UV[3];
				for (int32 k = 0; k < 3; ++k)
				{
					const FVector2D V = S.Verts[Tris[t + k]];
					const double Y = L.SurfaceY(S, V, bFloor);
					P[k] = ToUE(V.X, Y, V.Y);
					const FVector2D D = V - FrameO;
					const double Lx = (FVector2D::DotProduct(D, FrameA) - Plane.OffX) * TexelsPerUnit;
					const double Lz = (FVector2D::DotProduct(D, FrameN) * Stretch - Plane.OffY) * TexelsPerUnit;
					const double Rx = Lx * CosA - Lz * SinA, Rz = Lx * SinA + Lz * CosA;
					UV[k] = FVector2D(-Rx / TI.W, (bFloor ? Rz : -Rz) / TI.H);
				}
				const FVector N = FVector::CrossProduct(P[1] - P[0], P[2] - P[0]);
				if ((N.Z > 0.0) != bFloor)
				{
					Swap(P[0], P[2]);
					Swap(UV[0], UV[2]);
				}
				AddTo(bWater ? Water : World, TexStem, false, P, UV, 3, Bright(Lvl), bRev, false, bSwirl, bWater ? -1 : WG);
			}
			Stat(bFloor ? TEXT("floor") : TEXT("ceil"));
		}
	}

	// --- walls (LawMaker viewer rules) -------------------------------------------------------
	struct FBandDef { EBand Band; double BA, BB, TA, TB; int32 TexIdx; double OX, OY, Anchor; bool bMasked; bool bSky = false; };
	TArray<FOpening> Openings;
	TArray<FBandDef> Bands;
	TArray<TPair<double, double>> BoundsList;
	for (const FOutlawsSector& S : L.Sectors)
	{
		if (S.NoWalls())
		{
			Stat(TEXT("nowalls_sector"));
			continue;
		}
		const bool bCW = S.IsClockwise();
		for (int32 WallIndex = 0; WallIndex < S.Walls.Num(); ++WallIndex)
		{
			const FOutlawsWall& W = S.Walls[WallIndex];
			if (!S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2) || !F.WantWall(S.Index, WallIndex))
			{
				continue;
			}
			const FVector2D A = S.Verts[W.V1];
			const FVector2D B = S.Verts[W.V2];
			const double FA = L.SurfaceY(S, A, true), FB = L.SurfaceY(S, B, true);
			const double CA = L.SurfaceY(S, A, false), CB = L.SurfaceY(S, B, false);
			const int32 Lvl = FOutlawsLevel::LightLevel(S.Ambient, W.Light);
			const bool bHFlip = (W.Flags1 & WallHFlip) != 0;
			// V is measured from the band's bottom, as the Jedi engine does (TFE rwallFloat: v = offset.z +
			// 8 x height above the band bottom); WALLFLAG1_ANCHOR (0x8) only matters while an elevator moves
			// (TFE rsector sector_adjustTextureWallOffsets_*). Anchoring flagged walls to the ceiling put the
			// train's boxcar door art 2 units low (2026-09-27)
			// ... and while a lift floor has moved: a flagged wall of the lift itself keeps its texture where the
			// level put it, a flagged wall facing the lift (its side, the bottom band) carries it with the floor
			const bool bAnchored = (W.Flags1 & WallAnchor) != 0;
			const double* OwnShift = bAnchored ? F.FloorShift.Find(S.Index) : nullptr;
			const double AnchorY = S.Floor.Y - (OwnShift ? *OwnShift : 0.0);
			const double* NextShift = bAnchored ? F.FloorShift.Find(W.Adjoin) : nullptr;
			const double BotAnchorY = AnchorY + (NextShift ? *NextShift : 0.0);

			Openings.Reset();
			AddOpening(L, L.Sector(W.Adjoin), A, B, FA, FB, CA, CB, Openings);
			AddOpening(L, L.Sector(W.DAdjoin), A, B, FA, FB, CA, CB, Openings);

			Bands.Reset();
			static const bool bOldMillvis = OLFlags::Param(TEXT("OLOldMillvis"));     // (before-and-after shots)
			const FOutlawsSector* Shut = (Openings.Num() == 0 && W.DAdjoin < 0 && !bOldMillvis) ? L.Sector(W.Adjoin) : nullptr;
			const bool bDefaultMid = L.TexStem(W.Mid).Equals(TEXT("DEFAULT"), ESearchCase::IgnoreCase);
			if (Shut && bDefaultMid && !Shut->NoWalls() && L.SurfaceY(*Shut, A, false) <= FA + MinSegment && L.SurfaceY(*Shut, B, false) <= FB + MinSegment)
			{
				// (BUG-MILLVIS) an opening shut by the sector beyond, its ceiling at or under this floor (the mill's flume gates
				// let down): the original draws the wall's top part down to this floor (TFE wall_process: the top from the
				// ceiling to the higher of the next ceiling and this floor), not its mid.  The mid is the level's DEFAULT
				// placeholder there, which is not drawn (BUG-DEFAULTBAND), so the doorway stood open onto the sky
				if (S.Flags & ExtTopAdjoin) Bands.Add({ EBand::Top, FA, FB, CA, CB, -1, 0.0, 0.0, 0.0, false, true });   // sky
				else Bands.Add({ EBand::Top, FA, FB, CA, CB, W.Top, W.TopX, W.TopY, (FA + FB) * 0.5, false });
				Stat(TEXT("wall_top_shut"));
			}
			else if (Shut && bDefaultMid && !Shut->NoWalls() && L.SurfaceY(*Shut, A, true) >= CA - MinSegment && L.SurfaceY(*Shut, B, true) >= CB - MinSegment)
			{
				// (BUG-MILLVIS) ... and shut from below, its floor at or over this ceiling: the bottom part up to this ceiling
				if (S.Flags & ExtBtmAdjoin) Bands.Add({ EBand::Bot, FA, FB, CA, CB, -1, 0.0, 0.0, 0.0, false, true });   // sky
				else Bands.Add({ EBand::Bot, FA, FB, CA, CB, W.Bot, W.BotX, W.BotY, BotAnchorY, false });
				Stat(TEXT("wall_bot_shut"));
			}
			else if (Openings.Num() == 0)
			{
				Bands.Add({ EBand::Mid, FA, FB, CA, CB, W.Mid, W.MidX, W.MidY, AnchorY, false });
			}
			else
			{
				BoundsList.Reset();
				BoundsList.Add({ FA, FB });
				BoundsList.Add({ CA, CB });
				for (const FOpening& O : Openings)
				{
					BoundsList.Add({ O.B1, O.B2 });
					BoundsList.Add({ O.T1, O.T2 });
				}
				BoundsList.StableSort([](const TPair<double, double>& X, const TPair<double, double>& Y)
				{
					return (X.Key + X.Value) * 0.5 < (Y.Key + Y.Value) * 0.5;
				});
				for (int32 i = 0; i + 1 < BoundsList.Num(); ++i)
				{
					const TPair<double, double>& Lo = BoundsList[i];
					const TPair<double, double>& Hi = BoundsList[i + 1];
					if ((Hi.Key - Lo.Key) <= MinSegment && (Hi.Value - Lo.Value) <= MinSegment) continue;
					const double M1 = (Lo.Key + Hi.Key) * 0.5, M2 = (Lo.Value + Hi.Value) * 0.5;
					if (InsideAnyOpening(Openings, M1, M2)) continue;
					const EBand Band = ClassifyBand(Openings, M1, M2);
					// a double adjoin's solid middle stays its MID even while one of its two openings is shut
					// (the townex dig spot: the basement below the dirt opens only as it is dug)
					const FOutlawsSector* DN = L.Sector(W.DAdjoin);
					if (DN && ((Band == EBand::Bot && M1 >= L.SurfaceY(*DN, A, false) - MinSegment && M2 >= L.SurfaceY(*DN, B, false) - MinSegment)
						|| (Band == EBand::Top && M1 <= L.SurfaceY(*DN, A, true) + MinSegment && M2 <= L.SurfaceY(*DN, B, true) + MinSegment)))
					{
						Bands.Add({ EBand::Mid, Lo.Key, Lo.Value, Hi.Key, Hi.Value, W.Mid, W.MidX, W.MidY, (Lo.Key + Lo.Value) * 0.5, false });
						continue;
					}
					if (Band == EBand::Bot)
					{
						if (S.Flags & ExtBtmAdjoin)
						{
							Bands.Add({ Band, Lo.Key, Lo.Value, Hi.Key, Hi.Value, -1, 0.0, 0.0, 0.0, false, true });   // sky
							continue;
						}
						Bands.Add({ Band, Lo.Key, Lo.Value, Hi.Key, Hi.Value, W.Bot, W.BotX, W.BotY, BotAnchorY, false });
					}
					else if (Band == EBand::Top)
					{
						if (S.Flags & ExtTopAdjoin)
						{
							Bands.Add({ Band, Lo.Key, Lo.Value, Hi.Key, Hi.Value, -1, 0.0, 0.0, 0.0, false, true });   // sky
							continue;
						}
						// V from the top band's own bottom (the neighbour's ceiling), except over a sloped neighbour
						// ceiling, where the original keeps the room's floor (fits 1051 of 1545 such walls across
						// every level, the slope's height at the wall 36; flat neighbours: their ceiling, 3830 of 5656)
						const FOutlawsSector* Upper = L.Sector(W.Adjoin);
						const bool bSlopedUnder = Upper && (Upper->Flags & SlopedCeiling);
						Bands.Add({ Band, Lo.Key, Lo.Value, Hi.Key, Hi.Value, W.Top, W.TopX, W.TopY, bSlopedUnder ? AnchorY : (Lo.Key + Lo.Value) * 0.5, false });
					}
					else
					{
						const double Anc = W.DAdjoin >= 0 ? (Lo.Key + Lo.Value) * 0.5 : AnchorY;
						Bands.Add({ Band, Lo.Key, Lo.Value, Hi.Key, Hi.Value, W.Mid, W.MidX, W.MidY, Anc, false });
					}
				}
				if ((W.Flags1 & WallMidTex) && W.Mid >= 0 && F.WantMid(S.Index, WallIndex))
				{
					// a see-through mid fills its opening, the texture tiled up it with its offset, as the engine
					// draws it (TFE wall_drawTransparent wraps V with texHeightMask): the hideout's GWWIN2 windows
					// sit 7.9 units into an 8-unit texture, so a single floor-anchored copy left their openings
					// empty (user 2026-09-27).  Each side draws its own copy, facing its own sector (one-sided, as
					// the original draws only the near side's wall): the train's outer views show their 128-unit
					// gctbgrn2 painting once, never the ring's 64-unit GCTBGRND from behind
					for (const FOpening& O : Openings)
					{
						if ((O.T1 - O.B1) <= MinSegment && (O.T2 - O.B2) <= MinSegment) continue;
						Bands.Add({ EBand::MidTex, O.B1, O.B2, O.T1, O.T2, W.Mid, W.MidX, W.MidY, FMath::Min(O.B1, O.B2), true });
						// (a picture shorter than its opening repeats up it: say so, with the size it is drawn at)
						const FOutlawsTexInfo MI = Size(L.TexStem(W.Mid));
						const double Tall = FMath::Max(O.T1, O.T2) - FMath::Min(O.B1, O.B2);
						if (Tall > MI.H / TexelsPerUnit + 0.01)
						{
							UE_LOG(LogOutlaws, Log, TEXT("OLRT midtex repeats: sector %d wall %d -> %d %s %dx%d (%.0f u) over %.1f u, midy %.2f"),
								S.Index, WallIndex, W.Adjoin, *L.TexStem(W.Mid), MI.W, MI.H, MI.H / TexelsPerUnit, Tall, W.MidY);
						}
					}
				}
			}

			// inward normal: the wall faces into its own sector.  IsClockwise() is true for a negative shoelace area in
			// (x, z), whose inside lies to the RIGHT of each edge (EZ, -EX); the other way round, to the left.  (These
			// signs were once reversed: every wall faced out of its sector, which showed only when the world went
			// one-sided, walls drawn from the far side, overlays lifted behind their walls, and each see-through
			// mid's copy nudged toward the wrong sector, so the train's outer views showed the scenery ring's
			// 64-unit copy tiled twice; user 2026-09-27)
			const double EX = B.X - A.X, EZ = B.Y - A.Y;
			const double NX = bCW ? EZ : -EZ;
			const double NZ = bCW ? -EX : EX;
			const FVector Inward = ToUE(NX, 0.0, NZ);

			for (const FBandDef& Bd : Bands)
			{
				if (F.bMidOnly && Bd.Band != EBand::MidTex) continue;
				if (!F.WantBand(S.Index, WallIndex, Bd.Band == EBand::Top ? 1 : Bd.Band == EBand::Bot ? 4 : 2)) continue;
				if (Bd.bSky)
				{
					FVector Q[4] = { ToUE(A.X, Bd.BA, A.Y), ToUE(B.X, Bd.BB, B.Y), ToUE(B.X, Bd.TB, B.Y), ToUE(A.X, Bd.TA, A.Y) };
					FVector2D NoUV[4];
					OrientQuad(Q, NoUV, Inward);
					SkyFaces.AddFace(Q, NoUV, 4, 1.f, bRev);
					Stat(TEXT("sky_band"));
					continue;
				}
				const FString TexStem = L.TexStem(Bd.TexIdx);
				// the level's placeholder texture showing is a band the original never draws: report it
				if (TexStem.Equals(TEXT("DEFAULT"), ESearchCase::IgnoreCase) && (Bd.TA - Bd.BA) + (Bd.TB - Bd.BB) > 0.1)
				{
					UE_LOG(LogOutlaws, Log, TEXT("OLRT DEFAULT band: sector %d %s wall %d (adjoin %d dadjoin %d, flags 0x%x) %s %.2f..%.2f, sector %.2f..%.2f"),
						S.Index, *S.Name, WallIndex, W.Adjoin, W.DAdjoin, W.Flags1,
						Bd.Band == EBand::Bot ? TEXT("bot") : Bd.Band == EBand::Top ? TEXT("top") : Bd.Band == EBand::Mid ? TEXT("mid") : TEXT("midtex"),
						(Bd.BA + Bd.BB) * 0.5, (Bd.TA + Bd.TB) * 0.5, S.Floor.Y, S.Ceil.Y);
					// (user 2026-10-03, BUG-DEFAULTBAND: the hideout's grey "DEFAULT" panel over the bed, sector 373 wall 41:
					// "it should just be an invisible wall you can walk through") not drawn.  A band on an opening (a step's
					// riser, a lintel, a see-through mid) goes entirely, so it is walked through; a whole wall with nothing
					// behind it (the office's sector 154, the wharf's 287: the edge of the map) stays solid but unseen, in
					// the walkers' and shots' blockers, so no one walks or shoots out into the void
					if (W.Adjoin < 0 && Bd.Band == EBand::Mid && !F.bOnly)
					{
						const FVector QB[4] = { ToUE(A.X, Bd.BA, A.Y), ToUE(B.X, Bd.BB, B.Y), ToUE(B.X, Bd.TB, B.Y), ToUE(A.X, Bd.TA, A.Y) };
						const FVector2D NoUVB[4];
						FOutlawsMeshSection& Blk = Blockers.FindOrAdd(TEXT("ALL"));
						Blk.AddFace(QB, NoUVB, 4, 1.f, false);
						Blk.AddFace(QB, NoUVB, 4, 1.f, true);
						Stat(TEXT("default_wall_kept_solid"));
					}
					Stat(TEXT("default_band_skipped"));
					continue;
				}
				const FOutlawsTexInfo TI = Size(TexStem);
				FVector Q[4] = { ToUE(A.X, Bd.BA, A.Y), ToUE(B.X, Bd.BB, B.Y), ToUE(B.X, Bd.TB, B.Y), ToUE(A.X, Bd.TA, A.Y) };
				if (FMath::Abs(Bd.TA - Bd.BA) < 1e-6 || FMath::Abs(Bd.TB - Bd.BB) < 1e-6)
				{
					Stat(TEXT("wall_band_tapered"));
					UE_LOG(LogOutlaws, Verbose, TEXT("OLRT tapered band: sector %d wall %d -> %d, %.2f..%.2f / %.2f..%.2f"), S.Index, WallIndex, W.Adjoin, Bd.BA, Bd.TA, Bd.BB, Bd.TB);
					if (FMath::Abs(Bd.TA - Bd.BA) < 1e-6)
					{
						static int32 Shown = 0;
						if (Shown++ < 40) UE_LOG(LogOutlaws, Log, TEXT("OLRT tapered at its first end: sector %d wall %d -> %d, %.2f..%.2f / %.2f..%.2f"), S.Index, WallIndex, W.Adjoin, Bd.BA, Bd.TA, Bd.BB, Bd.TB);
					}
				}
				FVector2D UV[4];
				WallUVs(A, B, Bd.BA, Bd.BB, Bd.TA, Bd.TB, Bd.OX, Bd.OY, TI.W, TI.H, Bd.Anchor, bHFlip,
					Bd.Band == EBand::MidTex && TI.bTopLeftMid, UV);
				OrientQuad(Q, UV, Inward);
				if (Bd.Band == EBand::MidTex && (W.Flags1 & OutlawsFlags::WallTint))
				{
					// the glass itself, once per opening: from the lower-numbered sector, or from this one when
					// the far side is not tinted
					const FOutlawsSector* Far = L.Sector(W.Adjoin);
					const bool bFarTint = Far && Far->Walls.IsValidIndex(W.Mirror) && (Far->Walls[W.Mirror].Flags1 & OutlawsFlags::WallTint);
					if (!bFarTint || S.Index < W.Adjoin)
					{
						const FVector2D NoUV[4];
						Tint.AddFace(Q, NoUV, 4, Bright(Lvl), false);
						Tint.AddFace(Q, NoUV, 4, Bright(Lvl), true);
						Stat(TEXT("tinted_glass"));
					}
				}
				if (Bd.Band == EBand::MidTex)
				{
					// both sides of an opening draw its glass or fence: each copy sits a hair toward its own
					// sector, so each side sees its own light (not whichever copy won the depth tie)
					for (FVector& V : Q) V += Inward.GetSafeNormal() * 0.3;
				}
				// a wall whose ATX stops (a switch, a key slot) draws from a section of its own: its frames are its alone
				const int64 AtxKey = TI.bAtxStops ? int64(FOutlawsBuildFilter::Key(S.Index, WallIndex)) : -1;
				AddTo(World, TexStem, Bd.bMasked, Q, UV, 4, Bright(Lvl), bRev, Bd.Band == EBand::MidTex, false, WaterGroupOf(S.Index), AtxKey);   // blockers below collide instead
				Stat(Bd.Band == EBand::Bot ? TEXT("wall_bot") : Bd.Band == EBand::Top ? TEXT("wall_top") : Bd.Band == EBand::Mid ? TEXT("wall_mid") : TEXT("wall_midtex"));
			}

			// --- the opening's collision, by its flags (glass panes have their own mesh)
			if (!F.bOnly && Openings.Num() > 0 && !(W.Flags1 & OutlawsFlags::WallShatter))
			{
				const bool bMid = (W.Flags1 & WallMidTex) && W.Mid >= 0;
				bool bNoPass = (W.Flags1 & OutlawsFlags::WallNoPass) != 0;
				// (BUG-INVISWALLS) the Jedi engine tests NOPASS on the wall being crossed only (TFE collision.cpp: the walls of the
				// sector the walker is in), so a NOPASS on this side alone keeps walkers from leaving this sector and nothing
				// more; the mirror without it lets them come in.  The blocker here stops both ways, so where nobody could leave
				// by this wall anyway (the sector beyond 10 units up, past a jump and a step, or no room for a body in it) it only
				// stopped walkers from the far side: a canyon rim's edge in shwildns, shgulch, shsimms and townex, the drop off
				// it walled by nothing to be seen (158 such walls in shwildns, 240 in shgulch).  Those walls collide no more
				static const bool bOldInviswalls = OLFlags::Param(TEXT("OLOldInviswalls"));
				if (bNoPass && !bOldInviswalls && W.DAdjoin < 0)
				{
					const FOutlawsSector* Far = L.Sector(W.Adjoin);
					const bool bFarNoPass = Far && Far->Walls.IsValidIndex(W.Mirror) && (Far->Walls[W.Mirror].Flags1 & OutlawsFlags::WallNoPass);
					if (Far && !bFarNoPass && !Far->NoWalls())
					{
						bool bLeaves = false;
						for (const FVector2D& E : { A, B, (A + B) * 0.5 })
						{
							const double MyF = L.SurfaceY(S, E, true);
							const double NF = L.SurfaceY(*Far, E, true), NC = L.SurfaceY(*Far, E, false);
							if (NF <= MyF + NoPassReachUnits && NC - FMath::Max(NF, MyF) >= BodyUnits) bLeaves = true;
						}
						if (!bLeaves)
						{
							bNoPass = false;
							Stat(TEXT("nopass_one_sided_dropped"));
						}
					}
				}
				const bool bShotsPass = !bMid || (W.Flags1 & OutlawsFlags::WallProjectileOk) != 0;
				const TCHAR* Kind = bNoPass ? (bShotsPass ? TEXT("PAWN") : TEXT("ALL")) : (bShotsPass ? nullptr : TEXT("SHOT"));
				const bool bFence = (W.Flags1 & OutlawsFlags::WallFence) != 0;
				for (const FOpening& O : Openings)
				{
					if ((O.T1 - O.B1) <= MinSegment && (O.T2 - O.B2) <= MinSegment) continue;
					const FVector Q[4] = { ToUE(A.X, O.B1, A.Y), ToUE(B.X, O.B2, B.Y), ToUE(B.X, O.T2, B.Y), ToUE(A.X, O.T1, A.Y) };
					const FVector2D UV[4];
					for (const TCHAR* K : { Kind, bFence ? TEXT("ENEMY") : nullptr })
					{
						if (!K) continue;
						FOutlawsMeshSection& Blk = Blockers.FindOrAdd(K);
						Blk.AddFace(Q, UV, 4, 1.f, false);      // both faces: a sweep from either side stops
						Blk.AddFace(Q, UV, 4, 1.f, true);
					}
					Stat(Kind ? Kind : TEXT("open"));
					if (bFence) Stat(TEXT("fence"));
				}
			}

			// --- overlay decal (signs), per the viewer's BuildOverlayPanel
			if (!F.bMidOnly && W.Overlay >= 0 && W.Overlay < L.Textures.Num() && F.WantBand(S.Index, WallIndex, 8))
			{
				const FString OTex = L.TexStem(W.Overlay);
				const FOutlawsTexInfo TI = Size(OTex);
				const double TWU = TI.W / TexelsPerUnit, THU = TI.H / TexelsPerUnit;
				const double Len = FMath::Sqrt(EX * EX + EZ * EZ);
				if (Len < 1e-4) continue;
				double MidAnchor = AnchorY;
				const FOutlawsSector* AdjA = L.Sector(W.Adjoin);
				const FOutlawsSector* AdjD = L.Sector(W.DAdjoin);
				if (AdjA && AdjD)
				{
					MidAnchor = FMath::Min(AdjA->Ceil.Y, AdjD->Ceil.Y);
				}
				// a sign is never flipped, nor moved by its wall's flip: the engine reads its columns straight from u
				// less the sign's offset, the flip only for the wall's own texture (TFE setupSignTexture, the exe likewise)
				const double Along = -W.MidX + W.OvX;
				const double Up = MidAnchor - W.OvY;
				const double A0 = Along, A1 = Along + TWU;
				const double U0 = Up, U1 = Up + THU;
				const double CA0 = FMath::Max(A0, 0.0), CA1 = FMath::Min(A1, Len);
				const double CU0 = FMath::Max(U0, FMath::Min(FA, FB)), CU1 = FMath::Min(U1, FMath::Max(CA, CB));
				if (CA1 - CA0 < 1e-4 || CU1 - CU0 < 1e-4) continue;
				const double TX = EX / Len, TZ = EZ / Len;
				const double NL = FMath::Max(1e-9, FMath::Sqrt(NX * NX + NZ * NZ));
				const double Lift = 0.02;
				const double LX = NX / NL * Lift, LZ = NZ / NL * Lift;
				const bool bDefaultSign = OTex.Equals(TEXT("DEFAULT"), ESearchCase::IgnoreCase);
				// BUG-ADJOINDEFAULT (user 2026-10-08: frame-shaped DEFAULT panels in the mine over the rail track, sector 424
				// wall 3, and in the hideout's doorway by the bed, sector 73 walls 1 and 3): a sign that is the level's DEFAULT
				// placeholder is not drawn, as a DEFAULT band is not (BUG-DEFAULTBAND).  Those three walls are open their whole
				// height (the rooms beyond are as tall, their floors lower), so no part of them is drawn but the sign
				if (!bOldSignRule && bDefaultSign)
				{
					Stat(TEXT("default_sign_skipped"));
					continue;
				}
				const double UL = (CA0 - A0) / TWU, UR = (CA1 - A0) / TWU;
				const double VB = (CU0 - U0) / THU, VT = (CU1 - U0) / THU;
				const double PLX = A.X + TX * CA0 + LX, PLZ = A.Y + TZ * CA0 + LZ;
				const double PRX = A.X + TX * CA1 + LX, PRZ = A.Y + TZ * CA1 + LZ;
				FVector Q[4] = { ToUE(PLX, CU0, PLZ), ToUE(PRX, CU0, PRZ), ToUE(PRX, CU1, PRZ), ToUE(PLX, CU1, PLZ) };
				FVector2D UV[4] = { FVector2D(UL, VB), FVector2D(UR, VB), FVector2D(UR, VT), FVector2D(UL, VT) };
				OrientQuad(Q, UV, Inward);
				AddTo(World, OTex, true, Q, UV, 4, Bright(Lvl), bRev, false, false, WaterGroupOf(S.Index));
				Stat(TEXT("wall_overlay"));
				if (Audit && (W.Adjoin >= 0 || W.DAdjoin >= 0))
				{
					// (-OLAdjoinScan) a sign drawn on an adjoined wall: the DEFAULT placeholder, or any of it over the opening
					// (the Dark Forces renderer paints a sign only on the parts of its wall it draws solid: TFE rwallFloat
					// setupSignTexture in wall_drawSolid / _drawTop / _drawBottom; these are listed to look at, not changed)
					const TArray<FVector2D> Rect = { FVector2D(CA0, CU0), FVector2D(CA1, CU0), FVector2D(CA1, CU1), FVector2D(CA0, CU1) };
					double SolidArea = 0.0;
					for (const FBandDef& Bd : Bands)
					{
						if (Bd.bSky || Bd.Band == EBand::MidTex) continue;
						TArray<FVector2D> P = Rect;
						ClipToLine(P, Bd.BA, Bd.BB, Len, true);
						if (P.Num() >= 3) ClipToLine(P, Bd.TA, Bd.TB, Len, false);
						if (P.Num() >= 3) SolidArea += PolyArea(P);
					}
					const double RectArea = (CA1 - CA0) * (CU1 - CU0);
					if (bDefaultSign || RectArea - SolidArea > 0.01)
					{
						const FOutlawsSector* Far = L.Sector(W.Adjoin);
						Audit->Add(FString::Printf(TEXT("%s sector %d wall %d (id %s, adjoin %d mirror %d, flags 0x%x, floor/ceiling %.2f/%.2f, beyond %.2f/%.2f): sign %s %.1f x %.1f u, %.2f of its %.2f sq u over the opening"),
							bDefaultSign ? TEXT("DEFAULT") : TEXT("SIGN"), S.Index, WallIndex, *W.Id, W.Adjoin, W.Mirror, W.Flags1, S.Floor.Y, S.Ceil.Y,
							Far ? Far->Floor.Y : 0.0, Far ? Far->Ceil.Y : 0.0, *OTex, CA1 - CA0, CU1 - CU0, RectArea - SolidArea, RectArea));
					}
				}
			}
		}
	}
}

FString FOutlawsGeometry::PickSkyTexture(const FOutlawsLevel& L)
{
	TMap<FString, int32> Count;
	for (const FOutlawsSector& S : L.Sectors)
	{
		if (S.Flags & ExtCeiling)
		{
			Count.FindOrAdd(L.TexStem(S.Ceil.Tex))++;
		}
	}
	FString Best;
	int32 BestN = 0;
	for (const TPair<FString, int32>& Kv : Count)
	{
		if (Kv.Value > BestN) { Best = Kv.Key; BestN = Kv.Value; }
	}
	return Best;
}

void FOutlawsGeometry::BuildSky(const FOutlawsLevel& L, const FOutlawsTexInfo& SkyInfo, const FBox& LevelBounds, bool bRev, FOutlawsMeshSection& Out)
{
	// the texture twice round and twice up the dome (once stretched it too far and warped at the top)
	const double Repeats = 2.0 * FMath::Max(1.0, L.Parallax.X / FMath::Max(1, SkyInfo.W));
	const int32 Segs = 48, Rings = 12;
	const FVector C = LevelBounds.IsValid ? LevelBounds.GetCenter() : FVector::ZeroVector;
	const FVector Ext = LevelBounds.IsValid ? LevelBounds.GetExtent() : FVector::ZeroVector;
	const double Half = FMath::Max(Ext.X, Ext.Y);
	const double R = FMath::Max(SkyRadiusCm, 1.3 * Half);
	auto Lat = [](int32 i) { return FMath::DegreesToRadians(-30.0 + 120.0 * i / 12.0); };
	auto Vert = [&](int32 i, int32 j)
	{
		const double Phi = 2.0 * PI * j / Segs;
		const double La = Lat(i);
		return FVector(C.X + R * FMath::Cos(La) * FMath::Cos(Phi), C.Y + R * FMath::Cos(La) * FMath::Sin(Phi), R * FMath::Sin(La));
	};
	auto UV = [&](int32 i, int32 j)
	{
		const double Frac = FMath::Max(0.0, Lat(i)) / (PI / 2.0);
		return FVector2D(Repeats * j / Segs, 2.0 * Frac);
	};
	for (int32 i = 0; i < Rings; ++i)
	{
		for (int32 j = 0; j < Segs; ++j)
		{
			FVector Q[4] = { Vert(i, j), Vert(i, j + 1), Vert(i + 1, j + 1), Vert(i + 1, j) };
			FVector2D T[4] = { UV(i, j), UV(i, j + 1), UV(i + 1, j + 1), UV(i + 1, j) };
			Out.AddFace(Q, T, i == Rings - 1 ? 3 : 4, 1.f, bRev);
		}
	}
}
