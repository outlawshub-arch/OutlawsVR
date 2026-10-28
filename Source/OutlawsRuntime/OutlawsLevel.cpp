#include "OutlawsLevel.h"
#include "OutlawsFlags.h"
#include "OutlawsPolygon.h"
#include "OutlawsRuntime.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace
{
	// Tokenizer helpers over one line
	void SplitTokens(const FString& Line, TArray<FString>& Out)
	{
		Out.Reset();
		Line.ParseIntoArrayWS(Out);
	}

	int32 TokInt(const TArray<FString>& T, int32 i, int32 Default = 0)
	{
		return T.IsValidIndex(i) ? FCString::Atoi(*T[i]) : Default;
	}

	int64 TokInt64(const TArray<FString>& T, int32 i, int64 Default = 0)
	{
		return T.IsValidIndex(i) ? FCString::Atoi64(*T[i]) : Default;
	}

	double TokFloat(const TArray<FString>& T, int32 i, double Default = 0.0)
	{
		return T.IsValidIndex(i) ? FCString::Atod(*T[i]) : Default;
	}

	// "KEY:" token helpers for WALL lines: find the index of a token equal to Key
	int32 FindTok(const TArray<FString>& T, const TCHAR* Key)
	{
		for (int32 i = 0; i < T.Num(); ++i)
		{
			if (T[i].Equals(Key, ESearchCase::IgnoreCase))
			{
				return i;
			}
		}
		return -1;
	}

	void StripComment(FString& Line)
	{
		int32 Hash;
		if (Line.FindChar(TEXT('#'), Hash))
		{
			Line.LeftInline(Hash);
		}
		Line.TrimStartAndEndInline();
	}
}

bool FOutlawsSector::IsClockwise() const
{
	// (BUG-WALLSFLIPPED) the shoelace over the walls, each V1 -> V2: the vertex list's own order is not the walls' order.
	// A sector with holes (a table's legs, a post in a room) or with its vertices listed out of ring order closed the old
	// sum over the list across rings, and when that came out with the wrong sign every wall of the sector faced out of
	// it.  Over the walls, an outer ring and its holes run opposite ways and add up to the sector's own area
	// (-OLOldWinding: the rule as it was, for before-and-after shots from one build)
	static const bool bOldRule = OLFlags::Param(TEXT("OLOldWinding"));
	if (bOldRule) return IsClockwiseByVerts();
	double Sum = 0.0;
	int32 Counted = 0;
	for (const FOutlawsWall& W : Walls)
	{
		if (!Verts.IsValidIndex(W.V1) || !Verts.IsValidIndex(W.V2)) continue;
		const FVector2D& A = Verts[W.V1];
		const FVector2D& B = Verts[W.V2];
		Sum += (B.X - A.X) * (B.Y + A.Y);
		++Counted;
	}
	if (Counted >= 3 && Sum != 0.0)
	{
		return Sum > 0.0;
	}
	return IsClockwiseByVerts();
}

bool FOutlawsSector::IsClockwiseByVerts() const
{
	double Sum = 0.0;
	const int32 N = Verts.Num();
	for (int32 i = 0; i < N; ++i)
	{
		const FVector2D& A = Verts[i];
		const FVector2D& B = Verts[(i + 1) % N];
		Sum += (B.X - A.X) * (B.Y + A.Y);
	}
	return Sum > 0.0;
}

int32 FOutlawsLevel::LightLevel(int32 Ambient, int32 WallLight)
{
	const int32 A = FMath::Clamp(Ambient, 0, 31);
	return FMath::Clamp(A + FMath::Max(0, WallLight), 0, 31);
}

FString FOutlawsLevel::TexStem(int32 Index) const
{
	if (Textures.IsValidIndex(Index))
	{
		return FPaths::GetBaseFilename(Textures[Index]).ToUpper();
	}
	return TEXT("DEFAULT");
}

const FOutlawsSector* FOutlawsLevel::SectorById(const FString& Id) const
{
	for (const FOutlawsSector& S : Sectors)
	{
		if (S.Id.Equals(Id, ESearchCase::IgnoreCase))
		{
			return &S;
		}
	}
	return nullptr;
}

namespace
{
	bool PointInRing(const FOutlawsSector& S, const TArray<int32>& Ring, double X, double Z)
	{
		return OutlawsPolygon::InRing(S.Verts, Ring, X, Z);
	}
}

bool FOutlawsLevel::InsideWalls(const FOutlawsSector& S, double X, double Z)
{
	bool bIn = false;
	for (const FOutlawsWall& W : S.Walls)
	{
		if (!S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) continue;
		if (OutlawsPolygon::Crosses(S.Verts[W.V1], S.Verts[W.V2], X, Z)) bIn = !bIn;
	}
	return bIn;
}

const TArray<TArray<int32>>* FOutlawsLevel::CachedRings(const FOutlawsSector& S) const
{
	if (RingCache.Num() != Sectors.Num())
	{
		RingCache.SetNum(Sectors.Num());
		for (int32 i = 0; i < Sectors.Num(); ++i)
		{
			const FOutlawsSector& Sec = Sectors[i];
			TArray<TArray<int32>>& Rings = RingCache[i];
			Rings.Reset();
			LoopsFromWalls(Sec, Rings);
			Rings.RemoveAll([&Sec](const TArray<int32>& R)
			{
				for (int32 v : R) if (!Sec.Verts.IsValidIndex(v)) return true;
				return false;
			});
			Rings.StableSort([&Sec](const TArray<int32>& A, const TArray<int32>& B) { return FMath::Abs(RingArea(Sec, A)) > FMath::Abs(RingArea(Sec, B)); });
		}
	}
	// by index: the INF's live copy of the level shares the rings (same walls, moved vertices)
	return RingCache.IsValidIndex(S.Index) ? &RingCache[S.Index] : nullptr;
}

bool FOutlawsLevel::ContainsXZ(const FOutlawsSector& S, double X, double Z) const
{
	const TArray<TArray<int32>>* Cached = CachedRings(S);
	if (!Cached || Cached->Num() == 0) return false;
	const TArray<TArray<int32>>& Rings = *Cached;
	if (!PointInRing(S, Rings[0], X, Z)) return false;
	for (int32 r = 1; r < Rings.Num(); ++r)
	{
		if (PointInRing(S, Rings[r], X, Z)) return false;
	}
	return true;
}

const FOutlawsSector* FOutlawsLevel::SectorAt(double X, double Y, double Z, const TBitArray<>* Skip) const
{
	const FOutlawsSector* BestBelow = nullptr;
	double BestBelowY = -TNumericLimits<double>::Max();
	const FOutlawsSector* Lowest = nullptr;
	double LowestY = TNumericLimits<double>::Max();
	for (const FOutlawsSector& S : Sectors)
	{
		if (Skipped(Skip, S.Index)) continue;
		const TArray<TArray<int32>>* Cached = CachedRings(S);
		if (!Cached || Cached->Num() == 0)
		{
			continue;
		}
		const TArray<TArray<int32>>& Rings = *Cached;
		if (!PointInRing(S, Rings[0], X, Z))
		{
			continue;
		}
		bool bInHole = false;
		for (int32 r = 1; r < Rings.Num() && !bInHole; ++r)
		{
			bInHole = PointInRing(S, Rings[r], X, Z);
		}
		if (bInHole)
		{
			continue;
		}
		const double F = SurfaceY(S, FVector2D(X, Z), true);
		if (F <= Y + 0.01 && F > BestBelowY) { BestBelow = &S; BestBelowY = F; }
		if (F < LowestY) { Lowest = &S; LowestY = F; }
	}
	return BestBelow ? BestBelow : Lowest;
}

const FOutlawsSector* FOutlawsLevel::SectorAtBoxed(double X, double Y, double Z, const TArray<FBox2D>& Boxes) const
{
	if (Boxes.Num() != Sectors.Num()) return SectorAt(X, Y, Z);
	const FOutlawsSector* BestBelow = nullptr;
	double BestBelowY = -TNumericLimits<double>::Max();
	const FOutlawsSector* Lowest = nullptr;
	double LowestY = TNumericLimits<double>::Max();
	for (int32 i = 0; i < Sectors.Num(); ++i)
	{
		const FBox2D& B = Boxes[i];
		// (a point outside the box of every vertex is outside the outer ring: the box check alone, no ring walked)
		if (!B.bIsValid || X < B.Min.X || X > B.Max.X || Z < B.Min.Y || Z > B.Max.Y) continue;
		const FOutlawsSector& S = Sectors[i];
		if (!ContainsXZ(S, X, Z)) continue;
		const double F = SurfaceY(S, FVector2D(X, Z), true);
		if (F <= Y + 0.01 && F > BestBelowY) { BestBelow = &S; BestBelowY = F; }
		if (F < LowestY) { Lowest = &S; LowestY = F; }
	}
	return BestBelow ? BestBelow : Lowest;
}

bool FOutlawsLevel::ParseLvt(const FString& Text)
{
	Textures.Empty();
	Sectors.Empty();
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, false);

	FOutlawsSector* Cur = nullptr;
	enum class EMode { None, Vertices, Walls } Mode = EMode::None;
	TArray<FString> T;
	int32 SectorCounter = 0;

	for (FString Line : Lines)
	{
		StripComment(Line);   // "SECTOR 9CB1F456 # 0" keeps the id; comments are dropped
		if (Line.IsEmpty())
		{
			continue;
		}
		SplitTokens(Line, T);
		if (T.Num() == 0)
		{
			continue;
		}
		const FString& K = T[0];

		if (K.Equals(TEXT("LEVELNAME"), ESearchCase::IgnoreCase))
		{
			LevelName = T.IsValidIndex(1) ? T[1] : TEXT("");
			continue;
		}
		if (K.Equals(TEXT("MUSIC"), ESearchCase::IgnoreCase) && T.IsValidIndex(1) && Sectors.Num() == 0)
		{
			Music = T[1];
			continue;
		}
		if (K.Equals(TEXT("PALETTE:"), ESearchCase::IgnoreCase) && T.IsValidIndex(1))
		{
			Palettes.Add(T[1]);
			continue;
		}
		if (K.Equals(TEXT("CMAP:"), ESearchCase::IgnoreCase) && T.IsValidIndex(1))
		{
			Cmaps.Add(T[1]);
			continue;
		}
		if (K.Equals(TEXT("PARALLAX"), ESearchCase::IgnoreCase))
		{
			Parallax = FVector2D(TokFloat(T, 1, 1024.0), TokFloat(T, 2, 1024.0));
			continue;
		}
		if (K.Equals(TEXT("TEXTURE:"), ESearchCase::IgnoreCase) && Cur == nullptr)
		{
			if (T.IsValidIndex(1))
			{
				Textures.Add(T[1]);
			}
			continue;
		}
		if (K.Equals(TEXT("SECTOR"), ESearchCase::IgnoreCase))
		{
			FOutlawsSector S;
			S.Index = SectorCounter++;
			S.Id = T.IsValidIndex(1) ? T[1] : FString::Printf(TEXT("%d"), S.Index);
			Sectors.Add(MoveTemp(S));
			Cur = &Sectors.Last();
			Mode = EMode::None;
			continue;
		}
		if (!Cur)
		{
			continue;
		}
		if (K.Equals(TEXT("NAME"), ESearchCase::IgnoreCase))
		{
			Cur->Name = T.IsValidIndex(1) ? T[1] : TEXT("");
			continue;
		}
		if (K.Equals(TEXT("AMBIENT"), ESearchCase::IgnoreCase)) { Cur->Ambient = TokInt(T, 1, 31); continue; }
		if (K.Equals(TEXT("GRAVITY"), ESearchCase::IgnoreCase)) { Cur->Gravity = static_cast<float>(TokFloat(T, 1, -60.0)); continue; }
		if (K.Equals(TEXT("PALETTE"), ESearchCase::IgnoreCase)) { Cur->Palette = TokInt(T, 1, 0); continue; }
		if (K.Equals(TEXT("VADJOIN"), ESearchCase::IgnoreCase)) { Cur->VAdjoin = TokInt(T, 1, -1); continue; }
		if (K.Equals(TEXT("LAYER"), ESearchCase::IgnoreCase)) { Cur->Layer = TokInt(T, 1, 0); continue; }
		if (K.Equals(TEXT("FRICTION"), ESearchCase::IgnoreCase)) { Cur->Friction = static_cast<float>(FMath::Clamp(TokFloat(T, 1, 1.0), 0.0, 1.0)); continue; }
		if (K.Equals(TEXT("VELOCITY"), ESearchCase::IgnoreCase)) { Cur->Velocity = FVector(TokFloat(T, 1, 0.0), TokFloat(T, 2, 0.0), TokFloat(T, 3, 0.0)); continue; }
		if (K.Equals(TEXT("FLAGS"), ESearchCase::IgnoreCase))
		{
			Cur->Flags = uint32(TokInt64(T, 1, 0) & 0xFFFFFFFFll);
			continue;
		}
		if (K.Equals(TEXT("FLOOR"), ESearchCase::IgnoreCase) || K.Equals(TEXT("CEILING"), ESearchCase::IgnoreCase))
		{
			// "FLOOR Y 0.25 0 0.00 0.00 0.00" / "FLOOR SOUND NULL" / "FLOOR OFFSETS 0"
			if (T.IsValidIndex(2) && T[1].Equals(TEXT("SOUND"), ESearchCase::IgnoreCase) && K.Equals(TEXT("FLOOR"), ESearchCase::IgnoreCase))
			{
				Cur->FloorSound = T[2].Equals(TEXT("NULL"), ESearchCase::IgnoreCase) ? FString() : T[2].ToLower();
			}
			if (T.IsValidIndex(1) && T[1].Equals(TEXT("Y"), ESearchCase::IgnoreCase))
			{
				FOutlawsPlane& P = K.Equals(TEXT("FLOOR"), ESearchCase::IgnoreCase) ? Cur->Floor : Cur->Ceil;
				P.Y = TokFloat(T, 2);
				P.Tex = TokInt(T, 3, -1);
				P.OffX = TokFloat(T, 4);
				P.OffY = TokFloat(T, 5);
				P.Angle = TokFloat(T, 6);
			}
			continue;
		}
		if (K.Equals(TEXT("SLOPEDFLOOR"), ESearchCase::IgnoreCase) || K.Equals(TEXT("SLOPEDCEILING"), ESearchCase::IgnoreCase))
		{
			FOutlawsSlope& Sl = K.Equals(TEXT("SLOPEDFLOOR"), ESearchCase::IgnoreCase) ? Cur->SlopeFloor : Cur->SlopeCeil;
			Sl.Sector = TokInt(T, 1, -1);
			Sl.Wall = TokInt(T, 2, -1);
			Sl.Angle = TokFloat(T, 3);
			Sl.bSet = true;
			continue;
		}
		if (K.Equals(TEXT("VERTICES"), ESearchCase::IgnoreCase)) { Mode = EMode::Vertices; continue; }
		if (K.Equals(TEXT("WALLS"), ESearchCase::IgnoreCase)) { Mode = EMode::Walls; continue; }
		if (Mode == EMode::Vertices && K.Equals(TEXT("X:"), ESearchCase::IgnoreCase))
		{
			// "X: 128.50 Z: 105.75"
			Cur->Verts.Add(FVector2D(TokFloat(T, 1), TokFloat(T, 3)));
			continue;
		}
		if (Mode == EMode::Walls && K.Equals(TEXT("WALL:"), ESearchCase::IgnoreCase))
		{
			// WALL: id V1: v1 V2: v2 MID: t x y TOP: t x y BOT: t x y OVERLAY: t x y ADJOIN: a MIRROR: m DADJOIN: d DMIRROR: dm FLAGS: f1 f2 LIGHT: l
			FOutlawsWall W;
			W.Id = T.IsValidIndex(1) ? T[1] : FString();
			int32 i;
			if ((i = FindTok(T, TEXT("V1:"))) >= 0) W.V1 = TokInt(T, i + 1);
			if ((i = FindTok(T, TEXT("V2:"))) >= 0) W.V2 = TokInt(T, i + 1);
			if ((i = FindTok(T, TEXT("MID:"))) >= 0) { W.Mid = TokInt(T, i + 1, -1); W.MidX = TokFloat(T, i + 2); W.MidY = TokFloat(T, i + 3); }
			if ((i = FindTok(T, TEXT("TOP:"))) >= 0) { W.Top = TokInt(T, i + 1, -1); W.TopX = TokFloat(T, i + 2); W.TopY = TokFloat(T, i + 3); }
			if ((i = FindTok(T, TEXT("BOT:"))) >= 0) { W.Bot = TokInt(T, i + 1, -1); W.BotX = TokFloat(T, i + 2); W.BotY = TokFloat(T, i + 3); }
			if ((i = FindTok(T, TEXT("OVERLAY:"))) >= 0) { W.Overlay = TokInt(T, i + 1, -1); W.OvX = TokFloat(T, i + 2); W.OvY = TokFloat(T, i + 3); }
			if ((i = FindTok(T, TEXT("ADJOIN:"))) >= 0) W.Adjoin = TokInt(T, i + 1, -1);
			if ((i = FindTok(T, TEXT("MIRROR:"))) >= 0) W.Mirror = TokInt(T, i + 1, -1);
			if ((i = FindTok(T, TEXT("DADJOIN:"))) >= 0) W.DAdjoin = TokInt(T, i + 1, -1);
			if ((i = FindTok(T, TEXT("DMIRROR:"))) >= 0) W.DMirror = TokInt(T, i + 1, -1);
			if ((i = FindTok(T, TEXT("FLAGS:"))) >= 0) { W.Flags1 = uint32(TokInt64(T, i + 1) & 0xFFFFFFFFll); W.Flags2 = uint32(TokInt64(T, i + 2) & 0xFFFFFFFFll); }
			if ((i = FindTok(T, TEXT("LIGHT:"))) >= 0) W.Light = TokInt(T, i + 1);
			Cur->Walls.Add(W);
			continue;
		}
	}
	UE_LOG(LogOutlaws, Log, TEXT("LVT %s: %d sectors, %d textures"), *LevelName, Sectors.Num(), Textures.Num());
	return Sectors.Num() > 0;
}

bool FOutlawsLevel::ParseObt(const FString& Text)
{
	Objects.Empty();
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, false);
	TArray<FString> T;
	for (const FString& Raw : Lines)
	{
		FString Line = Raw;
		StripComment(Line);
		if (!Line.StartsWith(TEXT("NAME:"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		SplitTokens(Line, T);
		FOutlawsObject O;
		int32 i;
		if ((i = FindTok(T, TEXT("NAME:"))) >= 0) O.Name = T.IsValidIndex(i + 1) ? T[i + 1].ToUpper() : TEXT("");
		if ((i = FindTok(T, TEXT("ID:"))) >= 0) O.Id = T.IsValidIndex(i + 1) ? T[i + 1] : TEXT("");
		if ((i = FindTok(T, TEXT("SECTOR:"))) >= 0) O.SectorId = T.IsValidIndex(i + 1) ? T[i + 1] : TEXT("");
		if ((i = FindTok(T, TEXT("X:"))) >= 0) O.Pos.X = TokFloat(T, i + 1);
		if ((i = FindTok(T, TEXT("Y:"))) >= 0) O.Pos.Y = TokFloat(T, i + 1);
		if ((i = FindTok(T, TEXT("Z:"))) >= 0) O.Pos.Z = TokFloat(T, i + 1);
		if ((i = FindTok(T, TEXT("PCH:"))) >= 0) O.Pitch = float(TokFloat(T, i + 1));
		if ((i = FindTok(T, TEXT("YAW:"))) >= 0) O.Yaw = float(TokFloat(T, i + 1));
		if ((i = FindTok(T, TEXT("ROL:"))) >= 0) O.Roll = float(TokFloat(T, i + 1));
		if ((i = FindTok(T, TEXT("FLAGS:"))) >= 0) { O.Flags1 = uint32(TokInt64(T, i + 1) & 0xFFFFFFFFll); O.Flags2 = uint32(TokInt64(T, i + 2) & 0xFFFFFFFFll); }
		if (!O.Name.IsEmpty())
		{
			Objects.Add(MoveTemp(O));
		}
	}
	UE_LOG(LogOutlaws, Log, TEXT("OBT: %d objects"), Objects.Num());
	return true;
}

double FOutlawsLevel::SurfaceY(const FOutlawsSector& S, const FVector2D& Pt, bool bFloor) const
{
	const FOutlawsPlane& Plane = bFloor ? S.Floor : S.Ceil;
	const FOutlawsSlope& Slope = bFloor ? S.SlopeFloor : S.SlopeCeil;
	const uint32 Flag = bFloor ? OutlawsFlags::SlopedFloor : OutlawsFlags::SlopedCeiling;
	const double Base = Plane.Y;
	if (!Slope.bSet || !(S.Flags & Flag) || Slope.Wall < 0 || FMath::Abs(Slope.Angle) < 1e-4)
	{
		return Base;
	}
	const FOutlawsSector* Ref = Sector(Slope.Sector);
	if (!Ref)
	{
		Ref = &S;
	}
	if (!Ref->Walls.IsValidIndex(Slope.Wall))
	{
		return Base;
	}
	const FOutlawsWall& W = Ref->Walls[Slope.Wall];
	if (!Ref->Verts.IsValidIndex(W.V1) || !Ref->Verts.IsValidIndex(W.V2))
	{
		return Base;
	}
	const FVector2D A = Ref->Verts[W.V1];
	const FVector2D B = Ref->Verts[W.V2];
	FVector2D E = B - A;
	const double Len = E.Size();
	if (Len < 1e-4)
	{
		return Base;
	}
	E /= Len;
	const FVector2D Inward = Ref->IsClockwise() ? FVector2D(-E.Y, E.X) : FVector2D(E.Y, -E.X);
	const double Dist = FVector2D::DotProduct(Pt - A, Inward);
	const double Degrees = Slope.Angle * (90.0 / 4096.0);
	return Base - FMath::Tan(FMath::DegreesToRadians(Degrees)) * Dist;
}

void FOutlawsLevel::SurfaceTexFrame(const FOutlawsSector& S, bool bFloor, FVector2D& OutOrigin, FVector2D& OutAlong, FVector2D& OutNormal, double& OutStretch) const
{
	OutOrigin = FVector2D::ZeroVector;
	OutAlong = FVector2D(1.0, 0.0);
	OutNormal = FVector2D(0.0, 1.0);
	OutStretch = 1.0;
	double Degrees = 0.0;
	if (SlopeFrame(S, bFloor, OutAlong, OutNormal, &OutOrigin, &Degrees))
	{
		OutStretch = 1.0 / FMath::Max(0.05, FMath::Cos(FMath::DegreesToRadians(Degrees)));
	}
}

bool FOutlawsLevel::SlopeFrame(const FOutlawsSector& S, bool bFloor, FVector2D& OutAlong, FVector2D& OutNormal, FVector2D* OutOrigin, double* OutDegrees) const
{
	const FOutlawsSlope& Slope = bFloor ? S.SlopeFloor : S.SlopeCeil;
	const uint32 Flag = bFloor ? OutlawsFlags::SlopedFloor : OutlawsFlags::SlopedCeiling;
	if (!Slope.bSet || !(S.Flags & Flag) || Slope.Wall < 0) return false;
	const FOutlawsSector* Ref = Sector(Slope.Sector);
	if (!Ref) Ref = &S;
	if (!Ref->Walls.IsValidIndex(Slope.Wall)) return false;
	const FOutlawsWall& W = Ref->Walls[Slope.Wall];
	if (!Ref->Verts.IsValidIndex(W.V1) || !Ref->Verts.IsValidIndex(W.V2)) return false;
	FVector2D E = Ref->Verts[W.V2] - Ref->Verts[W.V1];
	if (E.Size() < 1e-4) return false;
	E.Normalize();
	OutAlong = E;
	OutNormal = Ref->IsClockwise() ? FVector2D(-E.Y, E.X) : FVector2D(E.Y, -E.X);
	if (OutOrigin) *OutOrigin = Ref->Verts[W.V1];
	if (OutDegrees) *OutDegrees = Slope.Angle * (90.0 / 4096.0);
	return true;
}

void FOutlawsLevel::LoopsFromWalls(const FOutlawsSector& S, TArray<TArray<int32>>& OutRings)
{
	OutRings.Reset();
	TArray<int32> Cur;
	int32 PrevV2 = -1;
	for (const FOutlawsWall& W : S.Walls)
	{
		if (Cur.Num() > 0 && W.V1 != PrevV2)
		{
			OutRings.Add(Cur);
			Cur.Reset();
		}
		Cur.Add(W.V1);
		PrevV2 = W.V2;
	}
	if (Cur.Num() > 0)
	{
		OutRings.Add(Cur);
	}
	OutRings.RemoveAll([](const TArray<int32>& R) { return R.Num() < 3; });
}

double FOutlawsLevel::RingArea(const FOutlawsSector& S, const TArray<int32>& Ring)
{
	double A = 0.0;
	for (int32 i = 0; i < Ring.Num(); ++i)
	{
		const FVector2D& P = S.Verts[Ring[i]];
		const FVector2D& Q = S.Verts[Ring[(i + 1) % Ring.Num()]];
		A += P.X * Q.Y - Q.X * P.Y;
	}
	return A * 0.5;
}

void FOutlawsLevel::LoopsFromWallsEx(const FOutlawsSector& S, TArray<TArray<int32>>& OutRings, TArray<TArray<int32>>& OutRingWalls)
{
	OutRings.Reset();
	OutRingWalls.Reset();
	TArray<int32> Cur, CurW;
	int32 PrevV2 = -1;
	for (int32 w = 0; w < S.Walls.Num(); ++w)
	{
		const FOutlawsWall& W = S.Walls[w];
		if (Cur.Num() > 0 && W.V1 != PrevV2)
		{
			OutRings.Add(Cur);
			OutRingWalls.Add(CurW);
			Cur.Reset();
			CurW.Reset();
		}
		Cur.Add(W.V1);
		CurW.Add(w);
		PrevV2 = W.V2;
	}
	if (Cur.Num() > 0)
	{
		OutRings.Add(Cur);
		OutRingWalls.Add(CurW);
	}
	for (int32 r = OutRings.Num() - 1; r >= 0; --r)
	{
		if (OutRings[r].Num() < 3)
		{
			OutRings.RemoveAt(r);
			OutRingWalls.RemoveAt(r);
		}
	}
}

int32 FOutlawsLevel::Which3D(double X, double Y, double Z, const TBitArray<>* Skip) const
{
	int32 Best = -1;
	double BestArea = TNumericLimits<double>::Max();
	const FVector2D Pt(X, Z);
	for (const FOutlawsSector& S : Sectors)
	{
		if (S.Verts.Num() < 3 || Skipped(Skip, S.Index) || !ContainsXZ(S, X, Z)) continue;
		const double F = SurfaceY(S, Pt, true), C = SurfaceY(S, Pt, false);
		if (Y < F - 0.5 || Y > C + 0.5) continue;
		double A2 = 0.0;
		for (int32 i = 0; i < S.Verts.Num(); ++i) { const FVector2D& P = S.Verts[i]; const FVector2D& Q = S.Verts[(i + 1) % S.Verts.Num()]; A2 += P.X * Q.Y - Q.X * P.Y; }
		const double Area = FMath::Abs(A2) * 0.5;
		if (Area < BestArea) { BestArea = Area; Best = S.Index; }
	}
	if (Best >= 0) return Best;
	const FOutlawsSector* S = SectorAt(X, Y, Z, Skip);
	return S ? S->Index : -1;
}

int32 FOutlawsLevel::Floor3D(double X, double Y, double Z, const TBitArray<>* Skip) const
{
	int32 Best = -1;
	double BestScore = TNumericLimits<double>::Max(), BestArea = TNumericLimits<double>::Max();
	const FVector2D Pt(X, Z);
	for (const FOutlawsSector& S : Sectors)
	{
		if (S.Verts.Num() < 3 || Skipped(Skip, S.Index) || !ContainsXZ(S, X, Z)) continue;
		const double F = SurfaceY(S, Pt, true), C = SurfaceY(S, Pt, false);
		if (Y < F - 0.5 || Y > C + 0.5) continue;
		const double Score = (S.NoWalls() ? 1e6 : 0.0) + FMath::Abs(Y - F);
		double A2 = 0.0;
		for (int32 i = 0; i < S.Verts.Num(); ++i) { const FVector2D& P = S.Verts[i]; const FVector2D& Q = S.Verts[(i + 1) % S.Verts.Num()]; A2 += P.X * Q.Y - Q.X * P.Y; }
		const double Area = FMath::Abs(A2);
		if (Score < BestScore - 0.01 || (Score <= BestScore + 0.01 && Area < BestArea)) { Best = S.Index; BestScore = Score; BestArea = Area; }
	}
	if (Best >= 0) return Best;
	const FOutlawsSector* S = SectorAt(X, Y, Z, Skip);
	return S ? S->Index : -1;
}

int32 FOutlawsLevel::TrackMove(int32 From, const FVector2D& A, const FVector2D& B, double Y, const TBitArray<>* Skip) const
{
	if (!Sectors.IsValidIndex(From)) return -2;
	int32 Cur = From;
	FVector2D P = A;
	const FVector2D D = B - P;
	for (int32 Hop = 0; Hop < 8 && D.SizeSquared() > 1e-12; ++Hop)
	{
		const FOutlawsSector& S = Sectors[Cur];
		double BestT = 2.0;
		int32 BestW = -1;
		const FVector2D Seg = B - P;
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
		if (BestW < 0) break;
		const FOutlawsWall& W = S.Walls[BestW];
		int32 Next = W.Adjoin;
		if (Sectors.IsValidIndex(W.DAdjoin))
		{
			// (a double adjoin: the one of the two whose height holds the feet)
			const FVector2D Hit = P + Seg * BestT;
			const FOutlawsSector& DS = Sectors[W.DAdjoin];
			if (Y >= SurfaceY(DS, Hit, true) - 0.5 && Y <= SurfaceY(DS, Hit, false) + 0.5) Next = W.DAdjoin;
		}
		if (!Sectors.IsValidIndex(Next) || Skipped(Skip, Next)) return -2;          // (through a solid wall: not a walk)
		P = P + Seg * BestT + Seg.GetSafeNormal() * 1e-5;
		Cur = Next;
	}
	const FOutlawsSector& S = Sectors[Cur];
	auto Holds = [&](const FOutlawsSector& X) { return !Skipped(Skip, X.Index) && ContainsXZ(X, B.X, B.Y) && Y >= SurfaceY(X, B, true) - 0.5 && Y <= SurfaceY(X, B, false) + 0.5; };
	if (Holds(S)) return Cur;
	if (Sectors.IsValidIndex(S.VAdjoin) && Holds(Sectors[S.VAdjoin])) return S.VAdjoin;      // (through the water's surface)
	return -2;
}
