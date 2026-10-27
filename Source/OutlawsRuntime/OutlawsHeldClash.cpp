// REQ-HELDCLASH: the opaque masks of two held men and the test of one against the other (see OutlawsHeldClash.h).
#include "OutlawsHeldClash.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsTextures.h"
#include "Components/StaticMeshComponent.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"

static TAutoConsoleVariable<float> CVarHeldClashThick(TEXT("ol.heldclash.ThickCm"), 35.f,
	TEXT("REQ-HELDCLASH: a body's thickness along the view: two held men further apart than this, one in front of the other, pass without touching."), ECVF_Default);
static TAutoConsoleVariable<float> CVarHeldClashEase(TEXT("ol.heldclash.EaseSec"), 0.15f,
	TEXT("REQ-HELDCLASH: once apart, how long a held man takes to come back to his hand from where the other pushed him."), ECVF_Default);
static TAutoConsoleVariable<float> CVarHeldClashBounce(TEXT("ol.heldclash.BounceCm"), 4.f,
	TEXT("REQ-HELDCLASH: how far each bounces off the other on a smash at the hard-hit speed (twice that speed and over: twice this)."), ECVF_Default);
static TAutoConsoleVariable<float> CVarHeldClashMaxOff(TEXT("ol.heldclash.MaxOffsetCm"), 100.f,
	TEXT("REQ-HELDCLASH: the farthest a held man is pushed from his hand."), ECVF_Default);

double OutlawsHeldClash::ThickCm() { return FMath::Max(1.0, double(CVarHeldClashThick.GetValueOnGameThread())); }
double OutlawsHeldClash::EaseSec() { return FMath::Max(0.01, double(CVarHeldClashEase.GetValueOnGameThread())); }
double OutlawsHeldClash::BounceCm() { return FMath::Max(0.0, double(CVarHeldClashBounce.GetValueOnGameThread())); }
double OutlawsHeldClash::MaxOffsetCm() { return FMath::Max(0.0, double(CVarHeldClashMaxOff.GetValueOnGameThread())); }

OutlawsHeldClash::FCost& OutlawsHeldClash::Cost()
{
	static FCost C;
	return C;
}

namespace
{
	double UsSince(uint64 C0) { return FPlatformTime::ToMilliseconds64(FPlatformTime::Cycles64() - C0) * 1000.0; }

	/** A mask's place in the plane: the pixel corner (x, y) at O + X * x + Y * y, and back. */
	struct FMap2
	{
		FVector2D O, X, Y;
		double Det = 0.0;
		FVector2D Inv(const FVector2D& V) const { return FVector2D((Y.Y * V.X - Y.X * V.Y) / Det, (X.X * V.Y - X.Y * V.X) / Det); }
		FVector2D At(double Px, double Py) const { return O + X * Px + Y * Py; }
	};
}

bool OutlawsHeldClash::BuildMask(const OutlawsTextures::FSpriteAtlas& A, int32 Cell, FMask& Out)
{
	const uint64 C0 = FPlatformTime::Cycles64();
	Out = FMask();
	const int32 Cols = FMath::Max(1, A.Cols);
	const int32 Cx0 = (FMath::Max(0, Cell) % Cols) * A.CellW, Cy0 = (FMath::Max(0, Cell) / Cols) * A.CellH;
	if (Cell < 0 || A.CellW <= 0 || A.CellH <= 0 || A.Alpha.Num() < A.AtlasW * A.AtlasH || Cx0 + A.CellW > A.AtlasW || Cy0 + A.CellH > A.AtlasH)
	{
		return false;
	}
	Out.W = A.CellW;
	Out.H = A.CellH;
	Out.Words = (Out.W + 63) / 64;
	Out.Bits.SetNumZeroed(Out.Words * Out.H);
	Out.X0 = Out.W;
	Out.Y0 = Out.H;
	double Sx = 0.0, Sy = 0.0;
	for (int32 y = 0; y < Out.H; ++y)
	{
		const uint8* Row = A.Alpha.GetData() + (Cy0 + y) * A.AtlasW + Cx0;
		uint64* Bits = Out.Bits.GetData() + y * Out.Words;
		for (int32 x = 0; x < Out.W; ++x)
		{
			if (!Row[x]) continue;
			Bits[x >> 6] |= 1ull << (x & 63);
			++Out.Count;
			Sx += x + 0.5;
			Sy += y + 0.5;
			Out.X0 = FMath::Min(Out.X0, x);
			Out.X1 = FMath::Max(Out.X1, x);
			Out.Y0 = FMath::Min(Out.Y0, y);
			Out.Y1 = FMath::Max(Out.Y1, y);
		}
	}
	if (Out.Count > 0) Out.Mid = FVector2D(Sx / Out.Count, Sy / Out.Count);
	else { Out.X0 = Out.Y0 = 0; Out.X1 = Out.Y1 = -1; }
	FCost& C = Cost();
	const double Us = UsSince(C0);
	++C.Builds;
	C.BuildUs += Us;
	C.BuildMaxUs = FMath::Max(C.BuildMaxUs, Us);
	return true;
}

bool OutlawsHeldClash::DrawnOf(const AOutlawsSpriteActor* S, const FMask& M, FDrawn& Out)
{
	if (!S || !S->Quad || M.W <= 0 || M.H <= 0) return false;
	// SM_SpriteQuad: 1 wide along Y (-0.5..0.5), 1 tall (0..1) from its bottom middle, the texture's right toward -Y and V
	// down (PlaceQuad's and GrabChest's rule); the actor's scale (mirrored for the left hand) sizes it
	const FTransform T = S->Quad->GetComponentTransform();
	Out.O = T.TransformPosition(FVector(0.0, 0.5, 1.0));
	Out.Ex = T.TransformVector(FVector(0.0, -1.0 / M.W, 0.0));
	Out.Ey = T.TransformVector(FVector(0.0, 0.0, -1.0 / M.H));
	// (REQ-HELDSWAY) swung, the bottom edge trails: each row is moved by its share of the swing offset, down from the top
	FVector BL, BR, RL, RR;
	if (S->SwingBottom(BL, BR, RL, RR)) Out.Ey += S->SwingOffset() / double(M.H);
	return true;
}

void OutlawsHeldClash::PlaneOf(const FMask& MA, const FDrawn& DA, const FMask& MB, const FDrawn& DB, const FVector& Eye, FVector& OutAt, FVector& OutView, FVector& OutR, FVector& OutU)
{
	const FVector CA = DA.At(MA.Mid.X, MA.Mid.Y), CB = DB.At(MB.Mid.X, MB.Mid.Y);
	OutAt = (CA + CB) * 0.5;
	OutView = (OutAt - Eye).GetSafeNormal();
	if (OutView.IsNearlyZero()) OutView = FVector::ForwardVector;
	OutR = (FVector::UpVector ^ OutView).GetSafeNormal();
	if (OutR.IsNearlyZero())
	{
		// (looked at from straight above or below: across is the line between them)
		const FVector D = CB - CA;
		OutR = (D - OutView * FVector::DotProduct(D, OutView)).GetSafeNormal();
		if (OutR.IsNearlyZero()) OutR = FVector::RightVector;
	}
	OutU = OutView ^ OutR;
}

OutlawsHeldClash::FResult OutlawsHeldClash::Test(const FMask& MA, const FDrawn& DA, const FMask& MB, const FDrawn& DB, const FVector& Eye, double Thick, const FVector2D& PrevN, bool bMayPassAlongView)
{
	const uint64 C0 = FPlatformTime::Cycles64();
	FCost& Co = Cost();
	FResult R;
	R.Gap = 1.0e9;
	auto Done = [&](bool bFull)
	{
		const double Us = UsSince(C0);
		++Co.Tests;
		Co.TestUs += Us;
		Co.TestMaxUs = FMath::Max(Co.TestMaxUs, Us);
		if (bFull)
		{
			++Co.FullTests;
			Co.FullUs += Us;
			Co.FullMaxUs = FMath::Max(Co.FullMaxUs, Us);
		}
		return R;
	};
	if (MA.IsEmpty() || MB.IsEmpty()) return Done(false);
	FVector M, F, Rt, Up;
	PlaneOf(MA, DA, MB, DB, Eye, M, F, Rt, Up);
	R.PlaneAt = M;
	R.View = F;
	R.PlaneR = Rt;
	R.PlaneU = Up;
	const FVector CA = DA.At(MA.Mid.X, MA.Mid.Y), CB = DB.At(MB.Mid.X, MB.Mid.Y);
	const double DepA = FVector::DotProduct(CA - Eye, F), DepB = FVector::DotProduct(CB - Eye, F);
	R.DepthGap = FMath::Abs(DepB - DepA);
	// each mask laid along the view onto the plane: an affine map of its pixels
	auto Map = [&](const FDrawn& D)
	{
		FMap2 Q;
		const FVector O = D.O - M;
		Q.O = FVector2D(FVector::DotProduct(O, Rt), FVector::DotProduct(O, Up));
		Q.X = FVector2D(FVector::DotProduct(D.Ex, Rt), FVector::DotProduct(D.Ex, Up));
		Q.Y = FVector2D(FVector::DotProduct(D.Ey, Rt), FVector::DotProduct(D.Ey, Up));
		Q.Det = Q.X.X * Q.Y.Y - Q.Y.X * Q.X.Y;
		return Q;
	};
	const FMap2 A2 = Map(DA), B2 = Map(DB);
	// (seen edge on: nothing to touch)
	if (FMath::Abs(A2.Det) < 1e-4 || FMath::Abs(B2.Det) < 1e-4) return Done(false);
	auto BoxOf = [](const FMap2& Q, const FMask& K)
	{
		FBox2D B(ForceInit);
		B += Q.At(K.X0, K.Y0);
		B += Q.At(K.X1 + 1, K.Y0);
		B += Q.At(K.X0, K.Y1 + 1);
		B += Q.At(K.X1 + 1, K.Y1 + 1);
		return B;
	};
	const FBox2D BA = BoxOf(A2, MA), BB = BoxOf(B2, MB);
	const FVector2D Ca = A2.At(MA.Mid.X, MA.Mid.Y), Cb = B2.At(MB.Mid.X, MB.Mid.Y);
	FVector2D Axis = Cb - Ca;
	if (!Axis.Normalize()) Axis = FVector2D(1.0, 0.0);
	const double G = FMath::Clamp(FMath::Min(FMath::Min(A2.X.Size(), A2.Y.Size()), FMath::Min(B2.X.Size(), B2.Y.Size())), 0.5, 10.0);
	R.CellCm = G;
	if (!BA.Intersect(BB))
	{
		const double Dx = FMath::Max(0.0, FMath::Max(BA.Min.X - BB.Max.X, BB.Min.X - BA.Max.X));
		const double Dy = FMath::Max(0.0, FMath::Max(BA.Min.Y - BB.Max.Y, BB.Min.Y - BA.Max.Y));
		R.Gap = R.BoxGap = FMath::Sqrt(Dx * Dx + Dy * Dy);
		return Done(false);
	}
	// (for the log: overlapping boxes, by the narrower side of where they meet: how much closer than boxes would let them)
	R.BoxGap = -FMath::Min(FMath::Min(BA.Max.X, BB.Max.X) - FMath::Max(BA.Min.X, BB.Min.X), FMath::Min(BA.Max.Y, BB.Max.Y) - FMath::Max(BA.Min.Y, BB.Min.Y));
	if (R.DepthGap >= Thick)
	{
		// (one in front of the other, their boxes meeting: apart along the view only)
		R.Gap = 0.0;
		return Done(false);
	}
	R.bNear = true;
	// Each way N tried: the plane sampled in strips along N, a sample about a pixel, over the stretch where B moved on along N
	// could still meet A (from the near edge of B's box to the far edge of A's, laid on N) and across where both boxes are.
	// A strip's samples are bits.  The overlap counts the samples opaque in both; the push is the least whole number of samples
	// d that B moved on along N takes it off A in every strip (B's bits moved d on never land on A's): the shortest parting
	// that way, the see-through gaps kept (an arm reaching past the other's body does not have to clear it).  The first way
	// (whether they touch at all) is across or up the plane, whichever is nearer the line between them, so its strips cover
	// no more than where their boxes meet, sampled at half a pixel: every pixel then holds a sample, and no sliver of
	// overlap thicker than half a pixel slips between them.  The other ways only compare, at a pixel.
	TArray<uint64, TInlineAllocator<4>> RowA, RowB, Blocked;
	auto Pass = [&](const FVector2D& N, double LimitCm, bool bFine, double& OutPush, double& OutGap) -> int32
	{
		const FVector2D S(-N.Y, N.X);
		auto Span = [](const FBox2D& Bx, const FVector2D& Ax, double& Lo, double& Hi)
		{
			const FVector2D K[4] = { Bx.Min, Bx.Max, FVector2D(Bx.Min.X, Bx.Max.Y), FVector2D(Bx.Max.X, Bx.Min.Y) };
			Lo = 1.0e9;
			Hi = -1.0e9;
			for (const FVector2D& P : K)
			{
				const double t = FVector2D::DotProduct(P, Ax);
				Lo = FMath::Min(Lo, t);
				Hi = FMath::Max(Hi, t);
			}
		};
		double TA0, TA1, TB0, TB1, SA0, SA1, SB0, SB1;
		Span(BA, N, TA0, TA1);
		Span(BB, N, TB0, TB1);
		Span(BA, S, SA0, SA1);
		Span(BB, S, SB0, SB1);
		const double T0 = TB0 - G, T1 = TA1 + G, S0 = FMath::Max(SA0, SB0) - G, S1 = FMath::Min(SA1, SB1) + G;
		OutPush = 0.0;
		OutGap = 1.0e9;
		if (T1 <= T0 || S1 <= S0) return 0;             // (B's box already wholly past A's that way)
		double Gs = bFine ? 0.5 * G : G;
		const int64 Cap = bFine ? 6000 : 2000;
		int32 Nt = FMath::Max(1, int32(FMath::CeilToInt((T1 - T0) / Gs))), Ns = FMath::Max(1, int32(FMath::CeilToInt((S1 - S0) / Gs)));
		while (int64(Nt) * Ns > Cap)
		{
			// (a deep overlap: coarser, never more than 6000 samples the first way, 2000 another)
			Gs *= 1.25;
			Nt = FMath::Max(1, int32(FMath::CeilToInt((T1 - T0) / Gs)));
			Ns = FMath::Max(1, int32(FMath::CeilToInt((S1 - S0) / Gs)));
		}
		const int32 Words = (Nt + 63) / 64;
		// (no further than LimitCm need be looked: a way that needs more loses anyway)
		const int32 Lim = FMath::Min(Nt, int32(FMath::CeilToInt(FMath::Min(LimitCm, 1.0e6) / Gs)) + 1);
		const int32 BWords = (Lim + 1 + 63) / 64;
		RowA.SetNumUninitialized(Words);
		RowB.SetNumUninitialized(Words);
		Blocked.SetNumZeroed(BWords);
		auto Bits64 = [&](int32 From) -> uint64
		{
			// A's 64 samples from From on, as bits
			const int32 W = From >> 6, O = From & 63;
			if (W >= Words) return 0;
			const uint64 Lo = RowA[W] >> O;
			return O && W + 1 < Words ? Lo | (RowA[W + 1] << (64 - O)) : Lo;
		};
		const FVector2D StepA = A2.Inv(N * Gs), StepB = B2.Inv(N * Gs);
		int32 Overlap = 0;
		for (int32 j = 0; j < Ns; ++j)
		{
			const double s = S0 + (j + 0.5) * Gs;
			const FVector2D P0 = N * (T0 + 0.5 * Gs) + S * s;
			FVector2D a = A2.Inv(P0 - A2.O), b = B2.Inv(P0 - B2.O);
			FMemory::Memzero(RowA.GetData(), Words * sizeof(uint64));
			FMemory::Memzero(RowB.GetData(), Words * sizeof(uint64));
			int32 LastA = -1, FirstB = -1;
			for (int32 i = 0; i < Nt; ++i)
			{
				if (MA.At(FMath::FloorToInt32(a.X), FMath::FloorToInt32(a.Y))) { RowA[i >> 6] |= 1ull << (i & 63); LastA = i; }
				if (MB.At(FMath::FloorToInt32(b.X), FMath::FloorToInt32(b.Y))) { RowB[i >> 6] |= 1ull << (i & 63); if (FirstB < 0) FirstB = i; }
				a += StepA;
				b += StepB;
			}
			if (LastA < 0 || FirstB < 0) continue;
			int32 Both = 0;
			for (int32 w = 0; w < Words; ++w) Both += FMath::CountBits(RowA[w] & RowB[w]);
			Overlap += Both;
			// (apart in this strip: by the stretch between, or none when they interleave without touching)
			OutGap = FMath::Min(OutGap, Both == 0 ? FMath::Max(0, FirstB - LastA - 1) * Gs : 0.0);
			// the moves B may not make: d = a - b for every A sample a at or past a B sample b
			for (int32 w = 0; w < Words; ++w)
			{
				for (uint64 Bw = RowB[w]; Bw; Bw &= Bw - 1)
				{
					const int32 Bi = (w << 6) + int32(FMath::CountTrailingZeros64(Bw));
					if (Bi > LastA) break;
					for (int32 k = 0; k < BWords; ++k) Blocked[k] |= Bits64(Bi + (k << 6));
				}
			}
		}
		// the least move not blocked (past the limit: the limit and a sample)
		int32 D = Lim + 1;
		for (int32 k = 0; k < BWords; ++k)
		{
			if (~Blocked[k] == 0) continue;
			D = FMath::Min(D, (k << 6) + int32(FMath::CountTrailingZeros64(~Blocked[k])));
			break;
		}
		OutPush = D * Gs;
		return Overlap;
	};
	double Push0 = 0.0, Gap0 = 0.0;
	const FVector2D Square = FMath::Abs(Axis.X) >= FMath::Abs(Axis.Y) ? FVector2D(Axis.X >= 0.0 ? 1.0 : -1.0, 0.0) : FVector2D(0.0, Axis.Y >= 0.0 ? 1.0 : -1.0);
	R.Overlap = Pass(Square, 1.0e9, true, Push0, Gap0);
	if (R.Overlap == 0)
	{
		// apart in the plane (their boxes overlap: the see-through gaps keep them off each other)
		R.Gap = FMath::Max(0.0, Gap0);
		return Done(true);
	}
	R.bContact = true;
	R.Gap = 0.0;
	// touching: the shortest way to part them, across (that first way, the line between them and 30 and 60 degrees either
	// side of it, and last frame's way if it is nearly as short) or, held one well in front of the other, along the view (to a body's
	// thickness apart: he slides past).  A way whose samples miss the overlap (a sliver under a sample) is not taken: it
	// would part them by nothing.  None is tried that cannot beat the best so far by a sample
	const double ViewPush = bMayPassAlongView ? Thick - R.DepthGap + G : 1.0e9;
	FVector2D Best = Square;
	double BestPush = Push0;
	const double Base = FMath::Atan2(Axis.Y, Axis.X);
	for (const double Deg : { 0.0, 30.0, -30.0, 60.0, -60.0 })
	{
		if (FMath::Min(BestPush, ViewPush) <= G) break;
		const double An = Base + FMath::DegreesToRadians(Deg);
		const FVector2D N(FMath::Cos(An), FMath::Sin(An));
		double P = 0.0, Gp = 0.0;
		if (Pass(N, FMath::Min(BestPush, ViewPush), false, P, Gp) > 0 && P < BestPush)
		{
			BestPush = P;
			Best = N;
		}
	}
	if (!PrevN.IsNearlyZero() && FVector2D::DotProduct(PrevN, Axis) > 0.0 && FVector2D::DotProduct(PrevN, Best) < 0.9999)
	{
		double P = 0.0, Gp = 0.0;
		if (Pass(PrevN, BestPush * 1.25 + G, false, P, Gp) > 0 && P <= BestPush * 1.25 + G)
		{
			BestPush = P;
			Best = PrevN;
		}
	}
	if (ViewPush < BestPush)
	{
		R.bAlongView = true;
		R.Push = ViewPush;
		R.Normal = F * (DepB >= DepA ? 1.0 : -1.0);
	}
	else
	{
		R.Push = BestPush;
		R.Normal2 = Best;
		R.Normal = (Rt * Best.X + Up * Best.Y).GetSafeNormal();
	}
	return Done(true);
}
