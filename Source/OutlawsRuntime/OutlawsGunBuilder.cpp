#include "OutlawsGunBuilder.h"
#include "OutlawsPolygon.h"

#include "CompGeom/Delaunay2.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformTime.h"
#include "Hash/CityHash.h"

namespace
{
	using UE::Geometry::FIndex3i;

	constexpr int32 BuilderVersion = 1;          // bump when the build's output changes (the cache keys on it)
	constexpr uint32 CacheMagic = 0x4E55474F;     // "OGUN"

	// ---- fields (row-major W x H floats) ------------------------------------------------------------------------
	struct FGunField
	{
		int32 W = 0, H = 0;
		TArray<float> V;
		void Init(int32 InW, int32 InH, float Value) { W = InW; H = InH; V.Init(Value, W * H); }
		float& operator()(int32 x, int32 y) { return V[y * W + x]; }
		float operator()(int32 x, int32 y) const { return V[y * W + x]; }
	};

	int32 Reflect(int32 i, int32 n)
	{
		// scipy's 'reflect' (d c b a | a b c d | d c b a)
		while (i < 0 || i >= n) i = i < 0 ? -i - 1 : 2 * n - i - 1;
		return i;
	}

	/** scipy.ndimage.gaussian_filter (truncate 4, mode reflect): axis 0 (rows) first, then axis 1. */
	void Gaussian(FGunField& F, double Sigma)
	{
		const int32 R = int32(4.0 * Sigma + 0.5);
		TArray<double> K;
		K.SetNum(2 * R + 1);
		double Sum = 0.0;
		for (int32 i = -R; i <= R; ++i) { K[i + R] = FMath::Exp(-0.5 * double(i) * i / (Sigma * Sigma)); Sum += K[i + R]; }
		for (double& k : K) k /= Sum;
		FGunField T = F;
		for (int32 y = 0; y < F.H; ++y)
			for (int32 x = 0; x < F.W; ++x)
			{
				double A = 0.0;
				for (int32 i = -R; i <= R; ++i) A += K[i + R] * F(x, Reflect(y + i, F.H));
				T(x, y) = float(A);
			}
		for (int32 y = 0; y < F.H; ++y)
			for (int32 x = 0; x < F.W; ++x)
			{
				double A = 0.0;
				for (int32 i = -R; i <= R; ++i) A += K[i + R] * T(Reflect(x + i, F.W), y);
				F(x, y) = float(A);
			}
	}

	/** One line of the squared distance transform (Felzenszwalb and Huttenlocher). */
	void Edt1(const double* F, double* D, int32 N, TArray<int32>& Vv, TArray<double>& Z)
	{
		// the lower envelope of the parabolas at the finite samples only (1e20 stands for none)
		Vv.SetNum(N);
		Z.SetNum(N + 1);
		int32 K = -1;
		for (int32 q = 0; q < N; ++q)
		{
			if (F[q] >= 1e19) continue;
			if (K < 0)
			{
				K = 0;
				Vv[0] = q;
				Z[0] = -1e30;
				Z[1] = 1e30;
				continue;
			}
			double S = ((F[q] + double(q) * q) - (F[Vv[K]] + double(Vv[K]) * Vv[K])) / (2.0 * (q - Vv[K]));
			while (S <= Z[K])
			{
				--K;
				S = ((F[q] + double(q) * q) - (F[Vv[K]] + double(Vv[K]) * Vv[K])) / (2.0 * (q - Vv[K]));
			}
			++K;
			Vv[K] = q;
			Z[K] = S;
			Z[K + 1] = 1e30;
		}
		if (K < 0)
		{
			for (int32 q = 0; q < N; ++q) D[q] = 1e20;
			return;
		}
		K = 0;
		for (int32 q = 0; q < N; ++q)
		{
			while (Z[K + 1] < q) ++K;
			const int32 P = Vv[K];
			D[q] = double(q - P) * (q - P) + F[P];
		}
	}

	/** scipy.ndimage.distance_transform_edt(Mask): each mask pixel's distance to the nearest pixel off it. */
	FGunField Edt(const TArray<bool>& Mask, int32 W, int32 H)
	{
		const double Inf = 1e20;
		TArray<double> G;
		G.SetNumUninitialized(W * H);
		TArray<double> Col, Out;
		TArray<int32> Vv;
		TArray<double> Z;
		Col.SetNum(FMath::Max(W, H));
		Out.SetNum(FMath::Max(W, H));
		for (int32 x = 0; x < W; ++x)
		{
			for (int32 y = 0; y < H; ++y) Col[y] = Mask[y * W + x] ? Inf : 0.0;
			Edt1(Col.GetData(), Out.GetData(), H, Vv, Z);
			for (int32 y = 0; y < H; ++y) G[y * W + x] = Out[y];
		}
		FGunField F;
		F.Init(W, H, 0.f);
		for (int32 y = 0; y < H; ++y)
		{
			Edt1(G.GetData() + y * W, Out.GetData(), W, Vv, Z);
			for (int32 x = 0; x < W; ++x) F(x, y) = Mask[y * W + x] ? float(FMath::Sqrt(FMath::Min(Out[x], Inf))) : 0.f;
		}
		return F;
	}

	/** grey_dilation with a Win x Win square (Win odd): the max over the window, clamped to the image. */
	FGunField MaxFilter(const FGunField& F, int32 Win)
	{
		const int32 R = Win / 2;
		FGunField T = F, O = F;
		for (int32 y = 0; y < F.H; ++y)
			for (int32 x = 0; x < F.W; ++x)
			{
				float M = -FLT_MAX;
				for (int32 i = FMath::Max(0, x - R); i <= FMath::Min(F.W - 1, x + R); ++i) M = FMath::Max(M, F(i, y));
				T(x, y) = M;
			}
		for (int32 y = 0; y < F.H; ++y)
			for (int32 x = 0; x < F.W; ++x)
			{
				float M = -FLT_MAX;
				for (int32 i = FMath::Max(0, y - R); i <= FMath::Min(F.H - 1, y + R); ++i) M = FMath::Max(M, T(x, i));
				O(x, y) = M;
			}
		return O;
	}

	/** map_coordinates(F, [[y], [x]], order=1) with the outside 0 (array coordinates). */
	double Bilinear(const FGunField& F, double x, double y)
	{
		const int32 x0 = FMath::FloorToInt(x), y0 = FMath::FloorToInt(y);
		const double fx = x - x0, fy = y - y0;
		auto At = [&F](int32 i, int32 j) { return (i >= 0 && j >= 0 && i < F.W && j < F.H) ? double(F(i, j)) : 0.0; };
		return (At(x0, y0) * (1 - fx) + At(x0 + 1, y0) * fx) * (1 - fy) + (At(x0, y0 + 1) * (1 - fx) + At(x0 + 1, y0 + 1) * fx) * fy;
	}

	/** Marching squares: the closed loops where F crosses Level, in array coordinates (x = column, y = row); the
	 *  saddles by the cell's mean.  F must be under Level all round its border. */
	TArray<TArray<FVector2d>> IsoLoops(const FGunField& F, double Level)
	{
		TMap<int64, FVector2d> Pt;
		TMap<int64, TArray<int64, TInlineAllocator<2>>> Adj;
		const int64 W = F.W;
		auto HId = [W](int32 x, int32 y) { return (int64(y) * W + x) * 2; };          // the edge (x,y)-(x+1,y)
		auto VId = [W](int32 x, int32 y) { return (int64(y) * W + x) * 2 + 1; };      // the edge (x,y)-(x,y+1)
		auto Cross = [&](int64 Id, double ax, double ay, double av, double bx, double by, double bv)
		{
			if (!Pt.Contains(Id))
			{
				const double t = (Level - av) / (bv - av);
				Pt.Add(Id, FVector2d(ax + (bx - ax) * t, ay + (by - ay) * t));
			}
		};
		auto Link = [&](int64 A, int64 B) { Adj.FindOrAdd(A).Add(B); Adj.FindOrAdd(B).Add(A); };
		for (int32 y = 0; y + 1 < F.H; ++y)
			for (int32 x = 0; x + 1 < F.W; ++x)
			{
				const double tl = F(x, y), tr = F(x + 1, y), br = F(x + 1, y + 1), bl = F(x, y + 1);
				const int32 C = (tl > Level ? 1 : 0) | (tr > Level ? 2 : 0) | (br > Level ? 4 : 0) | (bl > Level ? 8 : 0);
				if (C == 0 || C == 15) continue;
				const int64 Top = HId(x, y), Bottom = HId(x, y + 1), Left = VId(x, y), Right = VId(x + 1, y);
				const bool bT = (C & 1) != ((C >> 1) & 1), bR = ((C >> 1) & 1) != ((C >> 2) & 1);
				const bool bB = ((C >> 2) & 1) != ((C >> 3) & 1), bL = ((C >> 3) & 1) != (C & 1);
				if (bT) Cross(Top, x, y, tl, x + 1, y, tr);
				if (bR) Cross(Right, x + 1, y, tr, x + 1, y + 1, br);
				if (bB) Cross(Bottom, x, y + 1, bl, x + 1, y + 1, br);
				if (bL) Cross(Left, x, y, tl, x, y + 1, bl);
				if (C == 5 || C == 10)
				{
					const bool bCentreUp = (tl + tr + br + bl) * 0.25 > Level;
					const bool bCutTlBr = (C == 5) != bCentreUp;     // the corners cut off: tl and br, else tr and bl
					if (bCutTlBr) { Link(Top, Left); Link(Right, Bottom); }
					else { Link(Top, Right); Link(Left, Bottom); }
				}
				else
				{
					int64 E[2];
					int32 n = 0;
					if (bT) E[n++] = Top;
					if (bR && n < 2) E[n++] = Right;
					if (bB && n < 2) E[n++] = Bottom;
					if (bL && n < 2) E[n++] = Left;
					if (n == 2) Link(E[0], E[1]);
				}
			}
		TArray<TArray<FVector2d>> Loops;
		TSet<int64> Done;
		for (const TPair<int64, TArray<int64, TInlineAllocator<2>>>& Kv : Adj)
		{
			if (Done.Contains(Kv.Key)) continue;
			TArray<FVector2d> L;
			int64 Prev = -1, Cur = Kv.Key;
			while (Cur >= 0 && !Done.Contains(Cur))
			{
				Done.Add(Cur);
				L.Add(Pt[Cur]);
				const TArray<int64, TInlineAllocator<2>>& N = Adj[Cur];
				int64 Next = -1;
				for (int64 c : N) if (c != Prev && !Done.Contains(c)) { Next = c; break; }
				Prev = Cur;
				Cur = Next;
			}
			if (L.Num() >= 3) Loops.Add(MoveTemp(L));
		}
		return Loops;
	}

	double LoopArea(const TArray<FVector2d>& L)
	{
		double A = 0.0;
		for (int32 i = 0; i < L.Num(); ++i)
		{
			const FVector2d& P = L[i];
			const FVector2d& Q = L[(i + 1) % L.Num()];
			A += P.X * Q.Y - Q.X * P.Y;
		}
		return 0.5 * A;
	}

	double SegDist(const FVector2d& P, const FVector2d& A, const FVector2d& B)
	{
		const FVector2d D = B - A;
		const double L2 = D.SquaredLength();
		const double t = L2 > 0.0 ? FMath::Clamp((P - A).Dot(D) / L2, 0.0, 1.0) : 0.0;
		return (P - (A + D * t)).Length();
	}

	/** shapely LineString(ring + ring[0]).simplify(Tol) without its last point: Douglas-Peucker from the first
	 *  point round to itself. */
	TArray<FVector2d> SimplifyRing(const TArray<FVector2d>& Ring, double Tol)
	{
		TArray<FVector2d> P = Ring;
		P.Add(Ring[0]);
		TArray<bool> Keep;
		Keep.Init(false, P.Num());
		Keep[0] = Keep.Last() = true;
		TArray<TPair<int32, int32>> Stack;
		Stack.Add({ 0, P.Num() - 1 });
		while (Stack.Num())
		{
			const TPair<int32, int32> S = Stack.Pop();
			double Best = -1.0;
			int32 Bi = -1;
			for (int32 i = S.Key + 1; i < S.Value; ++i)
			{
				const double d = SegDist(P[i], P[S.Key], P[S.Value]);
				if (d > Best) { Best = d; Bi = i; }
			}
			if (Bi >= 0 && Best > Tol)
			{
				Keep[Bi] = true;
				Stack.Add({ S.Key, Bi });
				Stack.Add({ Bi, S.Value });
			}
		}
		TArray<FVector2d> Out;
		for (int32 i = 0; i + 1 < P.Num(); ++i) if (Keep[i]) Out.Add(P[i]);
		return Out;
	}

	/** The shape the loops bound (even-odd), for point tests and distances to its outline. */
	struct FShape
	{
		TArray<FVector2d> A, B;                 // the segments
		double Y0 = 0.0;
		TArray<TArray<int32>> Rows;             // segments by the unit rows they span
		double X0 = 0.0, Cell = 4.0;
		int32 GW = 0, GH = 0;
		TArray<TArray<int32>> Grid;             // segments by 4 px cells

		void Build(const TArray<TArray<FVector2d>>& Loops)
		{
			double MinX = 1e30, MinY = 1e30, MaxX = -1e30, MaxY = -1e30;
			for (const TArray<FVector2d>& L : Loops)
				for (int32 i = 0; i < L.Num(); ++i)
				{
					A.Add(L[i]); B.Add(L[(i + 1) % L.Num()]);
					MinX = FMath::Min(MinX, L[i].X); MaxX = FMath::Max(MaxX, L[i].X);
					MinY = FMath::Min(MinY, L[i].Y); MaxY = FMath::Max(MaxY, L[i].Y);
				}
			Y0 = FMath::FloorToDouble(MinY) - 1.0;
			Rows.SetNum(int32(MaxY - Y0) + 3);
			X0 = FMath::FloorToDouble(MinX) - 8.0;
			const double GY0 = Y0 - 8.0;
			GW = int32((MaxX - X0 + 16.0) / Cell) + 1;
			GH = int32((MaxY - GY0 + 16.0) / Cell) + 1;
			Grid.SetNum(GW * GH);
			for (int32 s = 0; s < A.Num(); ++s)
			{
				const int32 r0 = FMath::Clamp(int32(FMath::Min(A[s].Y, B[s].Y) - Y0), 0, Rows.Num() - 1);
				const int32 r1 = FMath::Clamp(int32(FMath::Max(A[s].Y, B[s].Y) - Y0), 0, Rows.Num() - 1);
				for (int32 r = r0; r <= r1; ++r) Rows[r].Add(s);
				const int32 c0 = int32((FMath::Min(A[s].X, B[s].X) - X0) / Cell), c1 = int32((FMath::Max(A[s].X, B[s].X) - X0) / Cell);
				const int32 g0 = int32((FMath::Min(A[s].Y, B[s].Y) - GY0) / Cell), g1 = int32((FMath::Max(A[s].Y, B[s].Y) - GY0) / Cell);
				for (int32 gy = g0; gy <= g1; ++gy) for (int32 gx = c0; gx <= c1; ++gx) Grid[FMath::Clamp(gy, 0, GH - 1) * GW + FMath::Clamp(gx, 0, GW - 1)].Add(s);
			}
		}
		bool Inside(double x, double y) const
		{
			const int32 r = int32(y - Y0);
			if (r < 0 || r >= Rows.Num()) return false;
			bool bIn = false;
			for (int32 s : Rows[r])
			{
				const FVector2d& P = A[s];
				const FVector2d& Q = B[s];
				if (OutlawsPolygon::Crosses(P, Q, x, y)) bIn = !bIn;
			}
			return bIn;
		}
		/** the distance to the outline, capped at Cell */
		double Dist(double x, double y) const
		{
			const int32 gx = int32((x - X0) / Cell), gy = int32((y - (Y0 - 8.0)) / Cell);
			double Best = Cell;
			for (int32 j = gy - 1; j <= gy + 1; ++j)
				for (int32 i = gx - 1; i <= gx + 1; ++i)
				{
					if (i < 0 || j < 0 || i >= GW || j >= GH) continue;
					for (int32 s : Grid[j * GW + i]) Best = FMath::Min(Best, SegDist(FVector2d(x, y), A[s], B[s]));
				}
			return Best;
		}
	};

	// ---- the recipe ---------------------------------------------------------------------------------------------
	struct FSurface { int32 Side = 1; double So[6] = { 1, 0, 1, 0, 1, 0 }; };
	struct FInflateDef
	{
		FString Group;
		bool bRound = false;
		double ThickCm = 0, Tol = 0.9, SpacingCm = 0, MinSpacingPx = 2.5, UvPush = 2.2;
		TArray<double> Insets;
		TArray<TSharedPtr<FJsonValue>> Ops;
		TArray<FSurface> Surfaces;
	};

	TArray<double> Nums(const TSharedPtr<FJsonObject>& J, const TCHAR* K)
	{
		TArray<double> Out;
		const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
		if (J.IsValid() && J->TryGetArrayField(K, A)) for (const TSharedPtr<FJsonValue>& V : *A) Out.Add(V->AsNumber());
		return Out;
	}

	bool ReadInflate(const TSharedPtr<FJsonObject>& J, FInflateDef& D)
	{
		if (!J.IsValid()) return false;
		D.Group = J->GetStringField(TEXT("group"));
		D.bRound = J->GetStringField(TEXT("kind")) == TEXT("round");
		D.ThickCm = J->GetNumberField(TEXT("thick_cm"));
		D.Tol = J->GetNumberField(TEXT("tol"));
		D.SpacingCm = J->GetNumberField(TEXT("spacing_cm"));
		D.MinSpacingPx = J->GetNumberField(TEXT("min_spacing_px"));
		J->TryGetNumberField(TEXT("uv_push"), D.UvPush);
		D.Insets = Nums(J, TEXT("insets"));
		D.Ops = J->GetArrayField(TEXT("ops"));
		for (const TSharedPtr<FJsonValue>& V : J->GetArrayField(TEXT("surfaces")))
		{
			FSurface S;
			S.Side = int32(V->AsObject()->GetNumberField(TEXT("side")));
			const TArray<double> So = Nums(V->AsObject(), TEXT("so"));
			if (So.Num() != 6) return false;
			for (int32 i = 0; i < 6; ++i) S.So[i] = So[i];
			D.Surfaces.Add(S);
		}
		return D.ThickCm > 0 && D.Surfaces.Num() > 0;
	}

	struct FSprite
	{
		const TArray<FColor>* Px = nullptr;
		int32 W = 0, H = 0;
		const FColor& At(int32 x, int32 y) const { return (*Px)[y * W + x]; }
	};

	bool InRect(const TArray<double>& R, int32 x, int32 y)
	{
		return R.Num() == 4 && x >= R[0] && x <= R[2] && y >= R[1] && y <= R[3];
	}

	/** proto.lever_mask: opaque and dark inside the rect, less the "less" rects */
	bool LeverPx(const FSprite& S, const TSharedPtr<FJsonObject>& O, int32 x, int32 y)
	{
		const FColor& C = S.At(x, y);
		if (!InRect(Nums(O, TEXT("rect")), x, y) || C.A <= 127 || int32(C.R) + C.G + C.B >= O->GetNumberField(TEXT("dark_sum"))) return false;
		for (const TSharedPtr<FJsonValue>& L : O->GetArrayField(TEXT("less")))
		{
			TArray<double> R;
			for (const TSharedPtr<FJsonValue>& V : L->AsArray()) R.Add(V->AsNumber());
			if (InRect(R, x, y)) return false;
		}
		return true;
	}

	/** the alpha an inflate sees: the sprite's, through the recipe's ops */
	FGunField RunOps(const FSprite& S, const TArray<TSharedPtr<FJsonValue>>& Ops)
	{
		FGunField A;
		A.Init(S.W, S.H, 0.f);
		for (int32 y = 0; y < S.H; ++y) for (int32 x = 0; x < S.W; ++x) A(x, y) = S.At(x, y).A;
		for (const TSharedPtr<FJsonValue>& V : Ops)
		{
			const TSharedPtr<FJsonObject> O = V->AsObject();
			const FString Op = O->GetStringField(TEXT("op"));
			if (Op == TEXT("smooth")) { Gaussian(A, O->GetNumberField(TEXT("sigma"))); continue; }
			// rects as numpy slices: y1 null = to the bottom
			TArray<double> R;
			const TArray<TSharedPtr<FJsonValue>>* RA = nullptr;
			if (O->TryGetArrayField(TEXT("rect"), RA)) for (const TSharedPtr<FJsonValue>& E : *RA) R.Add(E->IsNull() ? double(S.H - 1) : E->AsNumber());
			for (int32 y = 0; y < S.H; ++y)
				for (int32 x = 0; x < S.W; ++x)
				{
					const bool bIn = Op == TEXT("clear_lever") || Op == TEXT("keep_lever") ? LeverPx(S, O, x, y) : InRect(R, x, y);
					const bool bClear = Op.StartsWith(TEXT("clear")) ? bIn : !bIn;
					if (bClear) A(x, y) = 0.f;
				}
		}
		return A;
	}

	struct FMap { double S = 1, Cx = 0, Cy = 0; };

	/** guns.inflate_round (bRound) or proto.inflate: points (sprite px), half-thickness (cm), triangles as the
	 *  generator keeps them, UVs (OBJ convention) */
	bool Inflate(const FGunField& Alpha, const FInflateDef& D, const FMap& M, int32 TexW, int32 TexH,
		TArray<FVector2d>& Pts, TArray<double>& Zs, TArray<FIndex3i>& Keep, TArray<FVector2d>& UVs)
	{
		const int32 W = Alpha.W, H = Alpha.H;
		TArray<bool> Mask;
		Mask.SetNum(W * H);
		int32 MinX = W, MinY = H, MaxX = -1, MaxY = -1;
		for (int32 y = 0; y < H; ++y)
			for (int32 x = 0; x < W; ++x)
			{
				const bool b = Alpha(x, y) > 127.f;
				Mask[y * W + x] = b;
				if (b) { MinX = FMath::Min(MinX, x); MaxX = FMath::Max(MaxX, x); MinY = FMath::Min(MinY, y); MaxY = FMath::Max(MaxY, y); }
			}
		if (MaxX < 0) return false;
		const double s = M.S;
		FGunField Dt = Edt(Mask, W, H);
		float DtMax = 0.f;
		for (float& v : Dt.V) { v = FMath::Max(0.f, v - 0.5f); DtMax = FMath::Max(DtMax, v); }
		const double Cap = D.ThickCm / 2.0 / s;
		const int32 Win = 2 * int32(FMath::CeilToDouble(FMath::Min(Cap, double(DtMax)))) + 1;
		const FGunField Ridge = MaxFilter(Dt, Win);
		FGunField Zpx;
		Zpx.Init(W, H, 0.f);
		for (int32 i = 0; i < W * H; ++i)
		{
			if (!Mask[i]) continue;
			const double rr = FMath::Max(FMath::Min(double(Ridge.V[i]), Cap), 0.5);
			const double dd = FMath::Min(double(Dt.V[i]), rr);
			const double q = 1.0 - dd / rr;
			Zpx.V[i] = float(rr * FMath::Sqrt(FMath::Clamp(1.0 - q * q, 0.0, 1.0)));
		}
		// the silhouette: contours of the alpha padded by one, at 127.5
		FGunField Pad;
		Pad.Init(W + 2, H + 2, 0.f);
		for (int32 y = 0; y < H; ++y) for (int32 x = 0; x < W; ++x) Pad(x + 1, y + 1) = Alpha(x, y);
		TArray<TArray<FVector2d>> Raw = IsoLoops(Pad, 127.5);
		TArray<TArray<FVector2d>> Loops, Rings;
		for (TArray<FVector2d>& L : Raw)
		{
			for (FVector2d& P : L) P -= FVector2d(0.5, 0.5);
			if (FMath::Abs(LoopArea(L)) < 6.0) continue;
			Rings.Add(SimplifyRing(L, D.Tol));
			Loops.Add(MoveTemp(L));
		}
		if (!Loops.Num()) return false;
		FShape Shape;
		Shape.Build(Loops);
		const double Sp = FMath::Max(D.MinSpacingPx, D.SpacingCm / s);
		auto Zat = [&Zpx](double x, double y) { return Bilinear(Zpx, x - 0.5, y - 0.5); };
		for (const TArray<FVector2d>& R : Rings)
			for (int32 i = 0; i < R.Num(); ++i)
			{
				const FVector2d& A = R[i];
				const FVector2d& B = R[(i + 1) % R.Num()];
				const int32 K = FMath::Max(1, int32(FMath::CeilToDouble((B - A).Length() / Sp)));
				for (int32 j = 0; j < K; ++j) { Pts.Add(A + (B - A) * (double(j) / K)); Zs.Add(0.0); }
			}
		if (D.bRound && D.Insets.Num())
		{
			// the inset rings: iso-lines of the distance inside the outline (shapely's negative buffers), on a half
			// pixel grid one and a half pixels past the sprite
			const double h = 0.5, Gx0 = -1.5, Gy0 = -1.5;
			FGunField Sd;
			Sd.Init(int32((W + 3) / h) + 1, int32((H + 3) / h) + 1, 0.f);
			for (int32 j = 0; j < Sd.H; ++j)
				for (int32 i = 0; i < Sd.W; ++i)
				{
					const double x = Gx0 + i * h, y = Gy0 + j * h;
					const double d = Shape.Dist(x, y);
					Sd(i, j) = float(Shape.Inside(x, y) ? d : -d);
				}
			for (double Inset : D.Insets)
			{
				for (const TArray<FVector2d>& L : IsoLoops(Sd, Inset))
				{
					TArray<FVector2d> P;
					for (const FVector2d& Q : L) P.Add(FVector2d(Gx0 + Q.X * h, Gy0 + Q.Y * h));
					TArray<double> Cum;
					Cum.Add(0.0);
					for (int32 i = 0; i < P.Num(); ++i) Cum.Add(Cum.Last() + (P[(i + 1) % P.Num()] - P[i]).Length());
					const double Len = Cum.Last();
					const int32 K = FMath::Max(3, int32(FMath::CeilToDouble(Len / (Sp * 0.8))));
					int32 Seg = 0;
					for (int32 j = 0; j < K; ++j)
					{
						const double t = Len * j / K;
						while (Seg + 1 < P.Num() && Cum[Seg + 1] < t) ++Seg;
						const double SegLen = Cum[Seg + 1] - Cum[Seg];
						const double f = SegLen > 0.0 ? (t - Cum[Seg]) / SegLen : 0.0;
						const FVector2d Q = P[Seg] + (P[(Seg + 1) % P.Num()] - P[Seg]) * f;
						Pts.Add(Q);
						Zs.Add(Zat(Q.X, Q.Y));
					}
				}
			}
		}
		const double Thr = D.bRound ? FMath::Max(0.4 * Sp, (D.Insets.Num() ? D.Insets.Last() : 0.0) + 0.5) : 0.4 * Sp;
		for (double gy = MinY + Sp / 2; gy < MaxY + 1; gy += Sp)
			for (double gx = MinX + Sp / 2; gx < MaxX + 1; gx += Sp)
			{
				const int32 ix = int32(gx), iy = int32(gy);
				if (ix < W && iy < H && Mask[iy * W + ix] && Dt(ix, iy) >= Thr) { Pts.Add(FVector2d(gx, gy)); Zs.Add(Zat(gx, gy)); }
			}
		// one of each point (the triangulation's), then Delaunay, kept where the centroid is inside
		{
			TMap<FVector2d, int32> Seen;
			TArray<FVector2d> P2;
			TArray<double> Z2;
			for (int32 i = 0; i < Pts.Num(); ++i)
			{
				if (Seen.Contains(Pts[i])) continue;
				Seen.Add(Pts[i], P2.Num());
				P2.Add(Pts[i]);
				Z2.Add(Zs[i]);
			}
			Pts = MoveTemp(P2);
			Zs = MoveTemp(Z2);
		}
		UE::Geometry::FDelaunay2 Del;
		if (!Del.Triangulate(TArrayView<const FVector2d>(Pts))) return false;
		for (const FIndex3i& T : Del.GetTriangles())
		{
			const FVector2d& a = Pts[T.A];
			const FVector2d& b = Pts[T.B];
			const FVector2d& c = Pts[T.C];
			const FVector2d Cen = (a + b + c) / 3.0;
			if (!Shape.Inside(Cen.X, Cen.Y)) continue;
			const double Area = (b.X - a.X) * (c.Y - a.Y) - (c.X - a.X) * (b.Y - a.Y);
			Keep.Add(Area > 0 ? FIndex3i(T.A, T.C, T.B) : FIndex3i(T.A, T.B, T.C));
		}
		for (double& z : Zs) z *= s;
		// UVs: within UvPush of the edge read further in, down the (smoothed) distance's slope (round only)
		FGunField Dts = Dt;
		if (D.bRound) Gaussian(Dts, 1.0);
		for (const FVector2d& P : Pts)
		{
			double x = P.X, y = P.Y;
			if (D.bRound)
			{
				const double d = Bilinear(Dt, x - 0.5, y - 0.5);
				const double Push = FMath::Max(0.0, D.UvPush - d);
				if (Push > 0)
				{
					const int32 ix = FMath::Clamp(int32(x), 0, W - 1), iy = FMath::Clamp(int32(y), 0, H - 1);
					const double gx = ix == 0 ? Dts(1, iy) - Dts(0, iy) : ix == W - 1 ? Dts(W - 1, iy) - Dts(W - 2, iy) : (Dts(ix + 1, iy) - Dts(ix - 1, iy)) * 0.5;
					const double gy = iy == 0 ? Dts(ix, 1) - Dts(ix, 0) : iy == H - 1 ? Dts(ix, H - 1) - Dts(ix, H - 2) : (Dts(ix, iy + 1) - Dts(ix, iy - 1)) * 0.5;
					const double Ln = FMath::Sqrt(gx * gx + gy * gy);
					if (Ln > 1e-6) { x += gx / Ln * Push; y += gy / Ln * Push; }
				}
			}
			UVs.Add(FVector2d(x / TexW, 1.0 - y / TexH));
		}
		return Keep.Num() > 0;
	}

	/** a sample spec read off the sprite (make_held_recipe.sample) */
	FColor SampleSpec(const FSprite& S, const TSharedPtr<FJsonObject>& Spec)
	{
		const TArray<double> At = Nums(Spec, TEXT("at"));
		const int32 x = int32(At[0]), y = int32(At[1]), r = int32(At[2]);
		const bool bOpaque = Spec->GetStringField(TEXT("mode")) == TEXT("opaque");
		double Sum[3] = { 0, 0, 0 };
		int32 N = 0;
		for (int32 j = FMath::Max(0, y - r); j <= FMath::Min(S.H - 1, y + r); ++j)
			for (int32 i = FMath::Max(0, x - r); i <= FMath::Min(S.W - 1, x + r); ++i)
			{
				const FColor& C = S.At(i, j);
				if (bOpaque && C.A <= 127) continue;
				Sum[0] += C.R; Sum[1] += C.G; Sum[2] += C.B;
				++N;
			}
		double Mean[3] = { 60, 60, 60 };
		if (N) for (int32 c = 0; c < 3; ++c) Mean[c] = Sum[c] / N;
		const TArray<TSharedPtr<FJsonValue>> Ks = Spec->GetArrayField(TEXT("k"));
		int32 Out[3];
		int32 Ki = 0;
		if (bOpaque)
		{
			const double k0 = Ks.Num() ? Ks[0]->AsNumber() : 1.0;
			for (int32 c = 0; c < 3; ++c) Out[c] = int32(FMath::Clamp(Mean[c] * k0, 0.0, 255.0));
			Ki = 1;
		}
		else
		{
			for (int32 c = 0; c < 3; ++c) Out[c] = int32(Mean[c]);
		}
		for (; Ki < Ks.Num(); ++Ki)
		{
			double K3[3];
			if (Ks[Ki]->Type == EJson::Array)
			{
				const TArray<TSharedPtr<FJsonValue>>& A = Ks[Ki]->AsArray();
				for (int32 c = 0; c < 3; ++c) K3[c] = A[c]->AsNumber();
			}
			else
			{
				K3[0] = K3[1] = K3[2] = Ks[Ki]->AsNumber();
			}
			for (int32 c = 0; c < 3; ++c) Out[c] = int32(Out[c] * K3[c]);
		}
		return FColor(uint8(FMath::Clamp(Out[0], 0, 255)), uint8(FMath::Clamp(Out[1], 0, 255)), uint8(FMath::Clamp(Out[2], 0, 255)), 255);
	}

	/** the OBJ's two faces per triangle, as OutlawsObj::Load reads them (its Y mirror, its flat normals) */
	void AddTwoSided(FOutlawsMeshSection& Sec, FBox& Bounds, const FVector& A, const FVector& B, const FVector& C,
		const FVector2D& Ua, const FVector2D& Ub, const FVector2D& Uc)
	{
		const FVector P1[3] = { A, B, C };
		const FVector2D U1[3] = { Ua, Ub, Uc };
		const FVector P2[3] = { A, C, B };
		const FVector2D U2[3] = { Ua, Uc, Ub };
		Sec.AddFace(P1, U1, 3, 1.f, false);
		Sec.AddFace(P2, U2, 3, 1.f, false);
		Bounds += A; Bounds += B; Bounds += C;
	}

	FOutlawsMeshSection& SectionOf(TMap<FString, OutlawsObj::FObjMesh>& Groups, const FString& Group, const FString& TexturePath)
	{
		OutlawsObj::FObjMesh& M = Groups.FindOrAdd(Group);
		FOutlawsMeshSection* S = M.Sections.Find(TEXT("tex"));
		if (!S)
		{
			S = &M.Sections.Add(TEXT("tex"));
			S->Texture = TexturePath;
		}
		return *S;
	}

	bool Build(const TSharedPtr<FJsonObject>& J, const FSprite& S, const FString& TexturePath, OutlawsGunBuilder::FBuiltGun& Out, FString& Err)
	{
		const TArray<double> SpriteSize = Nums(J, TEXT("sprite_size")), TexSize = Nums(J, TEXT("tex_size"));
		if (SpriteSize.Num() != 2 || int32(SpriteSize[0]) != S.W || int32(SpriteSize[1]) != S.H)
		{
			Err = FString::Printf(TEXT("the sprite is %dx%d, the recipe's %s"), S.W, S.H, SpriteSize.Num() == 2 ? *FString::Printf(TEXT("%dx%d"), int32(SpriteSize[0]), int32(SpriteSize[1])) : TEXT("?"));
			return false;
		}
		Out.TexW = int32(TexSize[0]);
		Out.TexH = int32(TexSize[1]);
		Out.ImageRows = int32(J->GetNumberField(TEXT("image_rows")));
		if (Out.TexW < S.W || Out.TexH < Out.ImageRows || Out.ImageRows < S.H) { Err = TEXT("a bad texture size"); return false; }
		// the scale and centre: the whole sprite's opaque box across the frame's width
		const TSharedPtr<FJsonObject> Frame = J->GetObjectField(TEXT("frame"));
		int32 MinX = S.W, MinY = S.H, MaxX = -1, MaxY = -1;
		for (int32 y = 0; y < S.H; ++y)
			for (int32 x = 0; x < S.W; ++x)
				if (S.At(x, y).A > 127) { MinX = FMath::Min(MinX, x); MaxX = FMath::Max(MaxX, x); MinY = FMath::Min(MinY, y); MaxY = FMath::Max(MaxY, y); }
		if (MaxX < 0) { Err = TEXT("an empty sprite"); return false; }
		FMap M;
		M.S = Frame->GetNumberField(TEXT("width_cm")) / (MaxX - MinX + 1);
		M.Cx = (MinX + MaxX + 1) / 2.0;
		M.Cy = (MinY + MaxY + 1) / 2.0;
		const double Sg = Frame->GetNumberField(TEXT("dir"));
		const TArray<double> Grip = Nums(Frame, TEXT("grip"));
		const double Gx = (Grip[0] - M.Cx) * M.S, Gy = (M.Cy - Grip[1]) * M.S;
		// model (three.js axes, cm) -> the hand frame as the loader gives it (the OBJ's -Y mirrored back)
		auto ToUe = [Sg, Gx, Gy](double X, double Y, double Z) { return FVector(Sg * (X - Gx), Sg * Z, Y - Gy); };
		// the body and the action parts
		TArray<FInflateDef> Defs;
		{
			FInflateDef B;
			if (!ReadInflate(J->GetObjectField(TEXT("body")), B)) { Err = TEXT("a bad body"); return false; }
			Defs.Add(B);
			for (const TSharedPtr<FJsonValue>& V : J->GetArrayField(TEXT("parts")))
			{
				FInflateDef P;
				if (!ReadInflate(V->AsObject(), P)) { Err = TEXT("a bad part"); return false; }
				Defs.Add(P);
			}
		}
		for (const FInflateDef& D : Defs)
		{
			const FGunField Alpha = RunOps(S, D.Ops);
			TArray<FVector2d> Pts, UVs;
			TArray<double> Zs;
			TArray<FIndex3i> Keep;
			if (!Inflate(Alpha, D, M, Out.TexW, Out.TexH, Pts, Zs, Keep, UVs))
			{
				Err = FString::Printf(TEXT("the %s did not inflate"), D.Group.IsEmpty() ? TEXT("body") : *D.Group);
				return false;
			}
			OutlawsObj::FObjMesh& Mesh = Out.Groups.FindOrAdd(D.Group);
			FOutlawsMeshSection& Sec = SectionOf(Out.Groups, D.Group, TexturePath);
			for (const FSurface& Su : D.Surfaces)
			{
				TArray<FVector> P;
				P.SetNum(Pts.Num());
				for (int32 i = 0; i < Pts.Num(); ++i)
				{
					const double X = (Pts[i].X - M.Cx) * M.S, Y = (M.Cy - Pts[i].Y) * M.S, Z = Su.Side * Zs[i];
					P[i] = ToUe(Su.So[0] * X + Su.So[1], Su.So[2] * Y + Su.So[3], Su.So[4] * Z + Su.So[5]);
				}
				for (const FIndex3i& T : Keep)
				{
					const FIndex3i Tt = Su.Side > 0 ? T : FIndex3i(T.C, T.B, T.A);
					AddTwoSided(Sec, Mesh.Bounds, P[Tt.A], P[Tt.B], P[Tt.C], FVector2D(UVs[Tt.A]), FVector2D(UVs[Tt.B]), FVector2D(UVs[Tt.C]));
				}
			}
		}
		// the texture: the sprite, the blocks, the swatches (the fill is the caller's)
		Out.Tex.Init(FColor(0, 0, 0, 0), Out.TexW * Out.TexH);
		for (int32 y = 0; y < S.H; ++y) for (int32 x = 0; x < S.W; ++x) Out.Tex[y * Out.TexW + x] = S.At(x, y);
		for (const TSharedPtr<FJsonValue>& V : J->GetArrayField(TEXT("blocks")))
		{
			const TSharedPtr<FJsonObject> B = V->AsObject();
			if (B->GetStringField(TEXT("kind")) != TEXT("cylface")) continue;
			// guns.build_pistol_held's cylinder face: the metal, a darker rim, six chambers, the hub
			const FColor Metal = SampleSpec(S, B->GetObjectField(TEXT("metal")));
			const int32 Top = int32(B->GetNumberField(TEXT("top"))), Dn = int32(B->GetNumberField(TEXT("size")));
			const double Face = B->GetNumberField(TEXT("face")), Rim = B->GetNumberField(TEXT("rim")), Hub = B->GetNumberField(TEXT("hub"));
			const TArray<double> HoleK = Nums(B, TEXT("hole"));
			if (HoleK.Num() != 3 || Top + Dn > Out.ImageRows || Dn > Out.TexW) { Err = TEXT("a bad block"); return false; }
			const float Mf[3] = { float(Metal.R), float(Metal.G), float(Metal.B) };
			const int32 Hole[3] = { int32(Metal.R * HoleK[0]), int32(Metal.G * HoleK[1]), int32(Metal.B * HoleK[2]) };
			for (int32 y = 0; y < Dn; ++y)
				for (int32 x = 0; x < Dn; ++x)
				{
					const double xx = x + 0.5, yy = y + 0.5, h = Dn / 2.0;
					const double rr = FMath::Sqrt((xx - h) * (xx - h) + (yy - h) * (yy - h)) / h;
					float C[3];
					for (int32 c = 0; c < 3; ++c) C[c] = Mf[c] * float(Face);
					if (rr > 0.92) for (int32 c = 0; c < 3; ++c) C[c] = Mf[c] * float(Rim);
					for (int32 k = 0; k < 6; ++k)
					{
						const double a = FMath::DegreesToRadians(90.0 + 60.0 * k);
						const double px = h + 0.55 * h * FMath::Cos(a), py = h - 0.55 * h * FMath::Sin(a);
						if (FMath::Sqrt((xx - px) * (xx - px) + (yy - py) * (yy - py)) / h < 0.24) for (int32 c = 0; c < 3; ++c) C[c] = float(Hole[c]);
					}
					if (rr < 0.10) for (int32 c = 0; c < 3; ++c) C[c] = Mf[c] * float(Hub);
					Out.Tex[(Top + y) * Out.TexW + x] = FColor(uint8(FMath::Clamp(C[0], 0.f, 255.f)), uint8(FMath::Clamp(C[1], 0.f, 255.f)), uint8(FMath::Clamp(C[2], 0.f, 255.f)), 255);
				}
		}
		// the details: our geometry; a solid colour read off the sprite into its swatch
		for (const TSharedPtr<FJsonValue>& V : J->GetArrayField(TEXT("details")))
		{
			const TSharedPtr<FJsonObject> Dt = V->AsObject();
			const FString Group = Dt->GetStringField(TEXT("group"));
			const TArray<double> Pos = Nums(Dt, TEXT("pos")), Idx = Nums(Dt, TEXT("idx"));
			TArray<FVector2D> UV;
			UV.SetNum(Pos.Num() / 3);
			const TSharedPtr<FJsonObject>* Sample = nullptr;
			if (Dt->TryGetObjectField(TEXT("sample"), Sample))
			{
				const TArray<double> Sw = Nums(Dt, TEXT("swatch"));
				const int32 X0 = int32(Sw[0]), Y0 = int32(Sw[1]), N = int32(Sw[2]);
				if (X0 + N > Out.TexW || Y0 + N > Out.TexH) { Err = TEXT("a swatch off the texture"); return false; }
				const FColor C = SampleSpec(S, *Sample);
				for (int32 y = Y0; y < Y0 + N; ++y) for (int32 x = X0; x < X0 + N; ++x) Out.Tex[y * Out.TexW + x] = C;
				const FVector2D U((X0 + N / 2.0) / Out.TexW, 1.0 - (Y0 + N / 2.0) / Out.TexH);
				for (FVector2D& u : UV) u = U;
			}
			else
			{
				const TArray<double> U = Nums(Dt, TEXT("uv"));
				if (U.Num() != UV.Num() * 2) { Err = TEXT("a detail's UVs"); return false; }
				for (int32 i = 0; i < UV.Num(); ++i) UV[i] = FVector2D(U[2 * i], U[2 * i + 1]);
			}
			TArray<FVector> P;
			P.SetNum(Pos.Num() / 3);
			for (int32 i = 0; i < P.Num(); ++i) P[i] = FVector(Pos[3 * i], -Pos[3 * i + 1], Pos[3 * i + 2]);
			OutlawsObj::FObjMesh& Mesh = Out.Groups.FindOrAdd(Group);
			FOutlawsMeshSection& Sec = SectionOf(Out.Groups, Group, TexturePath);
			for (int32 t = 0; t + 2 < Idx.Num(); t += 3)
			{
				const int32 a = int32(Idx[t]), b = int32(Idx[t + 1]), c = int32(Idx[t + 2]);
				if (!P.IsValidIndex(a) || !P.IsValidIndex(b) || !P.IsValidIndex(c)) { Err = TEXT("a detail's index"); return false; }
				AddTwoSided(Sec, Mesh.Bounds, P[a], P[b], P[c], UV[a], UV[b], UV[c]);
			}
		}
		return true;
	}

	// ---- the cache ----------------------------------------------------------------------------------------------
	void Serialize(FArchive& Ar, OutlawsGunBuilder::FBuiltGun& G)
	{
		Ar << G.TexW << G.TexH << G.ImageRows << G.Tex;
		int32 N = G.Groups.Num();
		Ar << N;
		if (Ar.IsLoading()) G.Groups.Reset();
		TArray<FString> Keys;
		G.Groups.GetKeys(Keys);
		for (int32 i = 0; i < N; ++i)
		{
			FString K = Ar.IsLoading() ? FString() : Keys[i];
			Ar << K;
			OutlawsObj::FObjMesh& M = G.Groups.FindOrAdd(K);
			FOutlawsMeshSection& S = M.Sections.FindOrAdd(TEXT("tex"));
			Ar << S.Texture << S.Positions << S.Normals << S.UVs << S.Triangles << M.Bounds;
			if (Ar.IsLoading()) S.Colors.Init(FLinearColor::White, S.Positions.Num());
		}
	}
}

bool OutlawsGunBuilder::BuildOrLoad(const FString& RecipePath, const TArray<FColor>& Sprite, int32 SW, int32 SH,
	const FString& TexturePath, FBuiltGun& Out, FString& OutError)
{
	const double T0 = FPlatformTime::Seconds();
	Out = FBuiltGun();
	Out.Name = FPaths::GetBaseFilename(RecipePath).LeftChop(6);     // (less "_build")
	FString Text;
	if (Sprite.Num() != SW * SH || !FFileHelper::LoadFileToString(Text, *RecipePath))
	{
		OutError = FString::Printf(TEXT("no recipe at %s"), *RecipePath);
		return false;
	}
	// the key: the sprite's pixels, the recipe's text, the builder's version, where the texture is said to be
	uint64 Key = CityHash64(reinterpret_cast<const char*>(Sprite.GetData()), Sprite.Num() * sizeof(FColor));
	const FTCHARToUTF8 Utf(*Text);
	Key = CityHash128to64(Uint128_64(Key, CityHash64(Utf.Get(), Utf.Length())));
	const FTCHARToUTF8 TexUtf(*TexturePath);
	Key = CityHash128to64(Uint128_64(Key, CityHash64(TexUtf.Get(), TexUtf.Length()) + BuilderVersion));
	const FString CachePath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("HeldGuns"), Out.Name + TEXT(".bin"));
	TArray<uint8> Bytes;
	if (FFileHelper::LoadFileToArray(Bytes, *CachePath, FILEREAD_Silent) && Bytes.Num() > 16)
	{
		FMemoryReader R(Bytes);
		uint32 Magic = 0;
		int32 Ver = 0;
		uint64 Got = 0;
		R << Magic << Ver << Got;
		if (Magic == CacheMagic && Ver == BuilderVersion && Got == Key)
		{
			Serialize(R, Out);
			if (!R.IsError() && Out.Tex.Num() == Out.TexW * Out.TexH && Out.Groups.Contains(FString()))
			{
				Out.bCacheHit = true;
				Out.BuildMs = (FPlatformTime::Seconds() - T0) * 1000.0;
				return true;
			}
			Out = FBuiltGun();
			Out.Name = FPaths::GetBaseFilename(RecipePath).LeftChop(6);
		}
	}
	TSharedPtr<FJsonObject> J;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), J) || !J.IsValid() || int32(J->GetNumberField(TEXT("version"))) != 1)
	{
		OutError = TEXT("a bad recipe");
		return false;
	}
	FSprite S;
	S.Px = &Sprite;
	S.W = SW;
	S.H = SH;
	if (!Build(J, S, TexturePath, Out, OutError)) return false;
	TArray<uint8> Saved;
	FMemoryWriter Wr(Saved);
	uint32 Magic = CacheMagic;
	int32 Ver = BuilderVersion;
	Wr << Magic << Ver << Key;
	Serialize(Wr, Out);
	FFileHelper::SaveArrayToFile(Saved, *CachePath);
	Out.BuildMs = (FPlatformTime::Seconds() - T0) * 1000.0;
	return true;
}
