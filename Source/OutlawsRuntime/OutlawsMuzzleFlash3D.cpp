// REQ-MUZZLEFLASH3D: 3D muzzle flashes built at load from the enemies' own shooting sprites (the remaster's SPX in the
// player's files; nothing of them ships).  For each gun kind a shooter's SHOOT_CHOR (10) is searched: the front view
// (dir 0) frame with the most warm, bright pixels gives the starburst, the side view (dir 8 of 32) the flame jet,
// mirrored so the muzzle is at its left.  The cut (tools\olue\out\muzzleflash\scripts\flash3d.py): the largest warm
// cluster plus the white core near it, the dark hole in the starburst's middle (the muzzle in the art) filled with the
// core's colour, a soft edge, the body and the gun dropped.  Both go into one picture per kind, drawn by one procedural
// mesh: the jet on 3 planes crossed round the barrel at 60 degrees, the starburst on 1 plane facing forward at the muzzle;
// M_OLR_FxFlash is unlit and additive (ue_olue_fxflash_material.py).  Sanchez serves the pistols, a rifleman (Henry) the
// rifles and the gatling, a shotgunner (Bill Morgan, his round double burst) the shotguns.
#include "OutlawsLevelActor.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsLab.h"
#include "OutlawsFormats.h"
#include "OutlawsTextures.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Engine/Texture2D.h"
#include "ImageUtils.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

namespace
{
	struct FFlashSource { const TCHAR* Nwx; int32 Chor; float LengthCm; };
	// the jet's length along the barrel for each kind (the starburst is as wide); a shot scales it 0.85 to 1.15
	const FFlashSource GFlashSources[AOutlawsLevelActor::NumFlashKinds] = {
		{ TEXT("sanchez.nwx"), 10, 16.f },
		{ TEXT("henry.nwx"), 10, 24.f },
		{ TEXT("billmorg.nwx"), 10, 26.f } };
	constexpr int32 SlotsPerKind = 3;
	static_assert(SlotsPerKind * AOutlawsLevelActor::NumFlashKinds == sizeof(AOutlawsLevelActor::Flash3DSlots) / sizeof(AOutlawsLevelActor::FFlash3DSlot), "a slot per kind and shot");

	/** The shootframes.py count: drawn, red over 220, green over 150, blue under 160. */
	int32 FlashPixels(const TArray<FColor>& Px)
	{
		int32 N = 0;
		for (const FColor& C : Px) N += (C.A > 0 && C.R > 220 && C.G > 150 && C.B < 160) ? 1 : 0;
		return N;
	}

	/** Sets what the outside (4-neighbour, from the edges) cannot reach through the clear pixels. */
	void FillHoles(TArray<uint8>& M, int32 W, int32 H)
	{
		TArray<uint8> Out;
		Out.SetNumZeroed(W * H);
		TArray<int32> Stack;
		auto Seed = [&](int32 i) { if (!M[i] && !Out[i]) { Out[i] = 1; Stack.Add(i); } };
		for (int32 x = 0; x < W; ++x) { Seed(x); Seed((H - 1) * W + x); }
		for (int32 y = 0; y < H; ++y) { Seed(y * W); Seed(y * W + W - 1); }
		while (Stack.Num())
		{
			const int32 i = Stack.Pop(false);
			const int32 x = i % W, y = i / W;
			if (x > 0) Seed(i - 1);
			if (x + 1 < W) Seed(i + 1);
			if (y > 0) Seed(i - W);
			if (y + 1 < H) Seed(i + W);
		}
		for (int32 i = 0; i < W * H; ++i) if (!Out[i]) M[i] = 1;
	}

	/** 4-neighbour dilation, Iter times (scipy's binary_dilation with its default cross). */
	void Dilate(TArray<uint8>& M, int32 W, int32 H, int32 Iter)
	{
		TArray<uint8> T;
		for (int32 k = 0; k < Iter; ++k)
		{
			T = M;
			for (int32 y = 0; y < H; ++y)
			{
				for (int32 x = 0; x < W; ++x)
				{
					const int32 i = y * W + x;
					if (T[i]) continue;
					if ((x > 0 && T[i - 1]) || (x + 1 < W && T[i + 1]) || (y > 0 && T[i - W]) || (y + 1 < H && T[i + W])) M[i] = 1;
				}
			}
		}
	}

	/** The cut of one view: premultiplied colour and the mask, cropped; bMirror flips it left to right.  False: no flash. */
	bool CutFlash(const TArray<FColor>& Src, int32 W, int32 H, bool bMirror, TArray<FVector3f>& OutRgb, TArray<float>& OutM, int32& OW, int32& OH)
	{
		const int32 N = W * H;
		TArray<FVector3f> Rgb;
		TArray<float> Al;
		Rgb.SetNumUninitialized(N);
		Al.SetNumUninitialized(N);
		TArray<uint8> Warm;
		Warm.SetNumZeroed(N);
		for (int32 i = 0; i < N; ++i)
		{
			const FColor& C = Src[i];
			Rgb[i] = FVector3f(C.R / 255.f, C.G / 255.f, C.B / 255.f);
			Al[i] = C.A / 255.f;
			const FVector3f& P = Rgb[i];
			Warm[i] = (C.A > 0 && P.X > 0.85f && P.Y > 0.35f && P.Z < 0.5f && P.X - P.Z > 0.4f) ? 1 : 0;
		}
		// the clusters of the warm pixels grown by 4, each sized by its warm pixels: the biggest is the flash
		TArray<uint8> Grown = Warm;
		Dilate(Grown, W, H, 4);
		TArray<int32> Label;
		Label.Init(0, N);
		TArray<int32> Sizes;
		Sizes.Add(0);
		TArray<int32> Stack;
		for (int32 s = 0; s < N; ++s)
		{
			if (!Grown[s] || Label[s]) continue;
			const int32 Id = Sizes.Num();
			Sizes.Add(0);
			Label[s] = Id;
			Stack.Reset();
			Stack.Add(s);
			while (Stack.Num())
			{
				const int32 i = Stack.Pop(false);
				Sizes[Id] += Warm[i];
				const int32 x = i % W, y = i / W;
				auto Visit = [&](int32 j) { if (Grown[j] && !Label[j]) { Label[j] = Id; Stack.Add(j); } };
				if (x > 0) Visit(i - 1);
				if (x + 1 < W) Visit(i + 1);
				if (y > 0) Visit(i - W);
				if (y + 1 < H) Visit(i + W);
			}
		}
		int32 Best = 0;
		for (int32 k = 1; k < Sizes.Num(); ++k) if (Sizes[k] > Sizes[Best]) Best = k;
		if (Best == 0 || Sizes[Best] < 150) return false;
		TArray<uint8> Big, Near, Wb, White;
		Big.SetNumZeroed(N);
		for (int32 i = 0; i < N; ++i) Big[i] = Label[i] == Best ? 1 : 0;
		Near = Big;
		Dilate(Near, W, H, 3);
		FillHoles(Near, W, H);            // (the white core and the bores inside the flame are near it however wide they are)
		Wb.SetNumZeroed(N);
		White.SetNumZeroed(N);
		for (int32 i = 0; i < N; ++i)
		{
			Wb[i] = Warm[i] && Big[i];
			White[i] = Al[i] > 0.f && Rgb[i].X > 0.9f && Rgb[i].Y > 0.85f && Near[i];
		}
		// the holes in the flash (the muzzle drawn dark in its middle, inside the white core): what the outside cannot reach
		// round the flame and its core, their 1 px gaps closed (the outside grown back by 1 after, so no rim is taken)
		TArray<uint8> Wall, Out;
		Wall.SetNumZeroed(N);
		for (int32 i = 0; i < N; ++i) Wall[i] = Wb[i] || White[i];
		Dilate(Wall, W, H, 1);
		Out.SetNumZeroed(N);
		Stack.Reset();
		auto Seed = [&](int32 i) { if (!Wall[i] && !Out[i]) { Out[i] = 1; Stack.Add(i); } };
		for (int32 x = 0; x < W; ++x) { Seed(x); Seed((H - 1) * W + x); }
		for (int32 y = 0; y < H; ++y) { Seed(y * W); Seed(y * W + W - 1); }
		while (Stack.Num())
		{
			const int32 i = Stack.Pop(false);
			const int32 x = i % W, y = i / W;
			if (x > 0) Seed(i - 1);
			if (x + 1 < W) Seed(i + 1);
			if (y > 0) Seed(i - W);
			if (y + 1 < H) Seed(i + W);
		}
		Dilate(Out, W, H, 1);
		// the bores of a gun drawn pointing at the eye are see-through or dark, and the grey rims round them leave gaps in the
		// white: such a pixel is a hole too when flame or core closes it in on all 4 sides within 14 px
		auto Enclosed = [&](int32 x, int32 y)
		{
			int32 Sides = 0;
			const int32 Dx[4] = { 1, -1, 0, 0 }, Dy[4] = { 0, 0, 1, -1 };
			for (int32 d = 0; d < 4; ++d)
			{
				for (int32 s = 1; s <= 14; ++s)
				{
					const int32 X = x + Dx[d] * s, Y = y + Dy[d] * s;
					if (X < 0 || Y < 0 || X >= W || Y >= H) break;
					if (Wb[Y * W + X] || White[Y * W + X]) { ++Sides; break; }
				}
			}
			return Sides;
		};
		TArray<uint8> Bore;
		Bore.SetNumZeroed(N);
		for (int32 y = 0; y < H; ++y)
			for (int32 x = 0; x < W; ++x)
			{
				const int32 i = y * W + x;
				if (Wb[i] || White[i] || !Near[i]) continue;
				const bool bDark = Al[i] <= 0.f || FMath::Max3(Rgb[i].X, Rgb[i].Y, Rgb[i].Z) < 0.7f;      // (dark or the barrels' grey)
				Bore[i] = bDark && Enclosed(x, y) == 4 ? 1 : 0;
			}
		TArray<float> M0;
		M0.SetNumZeroed(N);
		for (int32 i = 0; i < N; ++i)
		{
			const bool bCore = (!Out[i] && !Wb[i] && !White[i]) || Bore[i];
			if (bCore) Rgb[i] = FVector3f(1.f, 0.95f, 0.8f);
			M0[i] = (Wb[i] || White[i] || bCore) ? 1.f : 0.f;
		}
		// a soft edge: a gaussian of 0.8 px (radius 3), kept near the flash, dimmer where the colour is dim
		float Kern[7];
		float Sum = 0.f;
		for (int32 k = -3; k <= 3; ++k) { Kern[k + 3] = FMath::Exp(-0.5f * k * k / (0.8f * 0.8f)); Sum += Kern[k + 3]; }
		for (float& K : Kern) K /= Sum;
		TArray<float> Tmp, M;
		Tmp.SetNumZeroed(N);
		M.SetNumZeroed(N);
		for (int32 y = 0; y < H; ++y)
			for (int32 x = 0; x < W; ++x)
			{
				float A = 0.f;
				for (int32 k = -3; k <= 3; ++k) A += Kern[k + 3] * M0[y * W + FMath::Clamp(x + k, 0, W - 1)];
				Tmp[y * W + x] = A;
			}
		int32 X0 = W, Y0 = H, X1 = -1, Y1 = -1;
		for (int32 y = 0; y < H; ++y)
			for (int32 x = 0; x < W; ++x)
			{
				const int32 i = y * W + x;
				float A = 0.f;
				for (int32 k = -3; k <= 3; ++k) A += Kern[k + 3] * Tmp[FMath::Clamp(y + k, 0, H - 1) * W + x];
				A *= Near[i] ? 1.f : 0.f;
				const float Glow = FMath::Clamp((Rgb[i].X + Rgb[i].Y) * 0.5f, 0.f, 1.f);
				M[i] = FMath::Clamp(A * (0.4f + 0.6f * Glow), 0.f, 1.f);
				if (M[i] > 0.05f) { X0 = FMath::Min(X0, x); X1 = FMath::Max(X1, x); Y0 = FMath::Min(Y0, y); Y1 = FMath::Max(Y1, y); }
			}
		if (X1 < 0) return false;
		X0 = FMath::Max(0, X0 - 4); Y0 = FMath::Max(0, Y0 - 4);
		X1 = FMath::Min(W, X1 + 5); Y1 = FMath::Min(H, Y1 + 5);
		OW = X1 - X0;
		OH = Y1 - Y0;
		OutRgb.SetNumUninitialized(OW * OH);
		OutM.SetNumUninitialized(OW * OH);
		for (int32 y = 0; y < OH; ++y)
			for (int32 x = 0; x < OW; ++x)
			{
				const int32 Sx = X0 + (bMirror ? OW - 1 - x : x), i = (Y0 + y) * W + Sx;
				OutRgb[y * OW + x] = Rgb[i] * M[i];
				OutM[y * OW + x] = M[i];
			}
		return true;
	}

	/** The front or side frame of a choreography's row with the most flash pixels: its cell's HD pixels (flipped as the
	 *  node says).  False: none with a flash. */
	bool FindFlashFrame(const TArray<uint8>& Spx, const TArray<OutlawsFormats::FSprxFrame>& SpxFrames, const OutlawsFormats::FWax& Wax, int32 Chor, int32 Dir,
		TArray<FColor>& OutPx, int32& OutW, int32& OutH, FString& OutWhat)
	{
		if (!Wax.Chors.IsValidIndex(Chor)) return false;
		const OutlawsFormats::FWaxChor& Ch = Wax.Chors[Chor];
		if (Ch.Dirs <= Dir || Ch.Grid.Num() < (Dir + 1) * Ch.Frames) return false;
		TArray<int32> Row;
		OutlawsFormats::ChorFrames(Ch.Grid.GetData() + Dir * Ch.Frames, Ch.Frames, Row);
		int32 BestN = 200;
		TSet<int32> Tried;
		for (const int32 F : Row)
		{
			if (!Wax.Frames.IsValidIndex(F) || Tried.Contains(F)) continue;
			Tried.Add(F);
			for (const OutlawsFormats::FWaxNode& Nd : Wax.Frames[F].Nodes)
			{
				TArray<FColor> Px;
				int32 W = 0, H = 0;
				if (!OutlawsTextures::DecodeSpxCellPixels(Spx, SpxFrames, Nd.Cell, Px, W, H)) continue;
				const int32 N = FlashPixels(Px);
				if (N <= BestN) continue;
				BestN = N;
				if (Nd.Flags & (OutlawsFormats::NodeFlipX | OutlawsFormats::NodeFlipY))
				{
					TArray<FColor> F2;
					F2.SetNumUninitialized(W * H);
					for (int32 y = 0; y < H; ++y)
						for (int32 x = 0; x < W; ++x)
						{
							const int32 Sx = (Nd.Flags & OutlawsFormats::NodeFlipX) ? W - 1 - x : x;
							const int32 Sy = (Nd.Flags & OutlawsFormats::NodeFlipY) ? H - 1 - y : y;
							F2[y * W + x] = Px[Sy * W + Sx];
						}
					Px = MoveTemp(F2);
				}
				OutPx = MoveTemp(Px);
				OutW = W;
				OutH = H;
				OutWhat = FString::Printf(TEXT("f%d (cell %d, %dx%d, %d flash px)"), F, Nd.Cell, W, H, N);
			}
		}
		return OutPx.Num() > 0;
	}

	/** One kind's picture from its shooter's frames; the session keeps it (the same files on every level). */
	void CutKind(const FOutlawsAssets& A, int32 Kind, AOutlawsLevelActor::FFlash3DKind& K)
	{
		const FFlashSource& S = GFlashSources[Kind];
		K = AOutlawsLevelActor::FFlash3DKind();
		const FString Spx = FPaths::GetBaseFilename(S.Nwx) + TEXT(".spx");
		TArray<uint8> Bytes;
		OutlawsFormats::FWax Wax;
		if (!A.Get(S.Nwx, Bytes) || !OutlawsFormats::ParseWax(MoveTemp(Bytes), Wax))
		{
			K.Source = FString::Printf(TEXT("%s not found"), S.Nwx);
			return;
		}
		TArray<uint8> SpxBytes;                  // (read once: each cell is a slice of it)
		TArray<OutlawsFormats::FSprxFrame> SpxFrames;
		if (!A.Get(Spx, SpxBytes) || !OutlawsFormats::ParseSprx(SpxBytes, SpxFrames))
		{
			K.Source = FString::Printf(TEXT("%s: no remaster SPX"), S.Nwx);
			return;
		}
		TArray<FColor> FrontPx, SidePx;
		int32 FW = 0, FH = 0, SW = 0, SH = 0;
		FString FWhat, SWhat;
		if (!FindFlashFrame(SpxBytes, SpxFrames, Wax, S.Chor, 0, FrontPx, FW, FH, FWhat) || !FindFlashFrame(SpxBytes, SpxFrames, Wax, S.Chor, 8, SidePx, SW, SH, SWhat))
		{
			K.Source = FString::Printf(TEXT("%s chor %d: no frame with a flash (front %s, side %s)"), S.Nwx, S.Chor, FrontPx.Num() ? TEXT("found") : TEXT("none"),
				SidePx.Num() ? TEXT("found") : TEXT("none"));
			return;
		}
		TArray<FVector3f> StarRgb, JetRgb;
		TArray<float> StarM, JetM;
		int32 Sw = 0, Sh = 0, Jw = 0, Jh = 0;
		if (!CutFlash(FrontPx, FW, FH, false, StarRgb, StarM, Sw, Sh) || !CutFlash(SidePx, SW, SH, true, JetRgb, JetM, Jw, Jh))
		{
			K.Source = FString::Printf(TEXT("%s chor %d: the cut found no flash"), S.Nwx, S.Chor);
			return;
		}
		// one picture: the jet at the left (its muzzle at x 0), 2 px apart, the starburst at the right
		K.W = Jw + 2 + Sw;
		K.H = FMath::Max(Jh, Sh);
		K.Px.Init(FColor(0, 0, 0, 255), K.W * K.H);
		auto Put = [&K](const TArray<FVector3f>& Rgb, int32 W, int32 H, int32 X0)
		{
			for (int32 y = 0; y < H; ++y)
				for (int32 x = 0; x < W; ++x)
				{
					const FVector3f& C = Rgb[y * W + x];
					K.Px[y * K.W + X0 + x] = FColor(uint8(FMath::Clamp(C.X, 0.f, 1.f) * 255.f + 0.5f), uint8(FMath::Clamp(C.Y, 0.f, 1.f) * 255.f + 0.5f),
						uint8(FMath::Clamp(C.Z, 0.f, 1.f) * 255.f + 0.5f), 255);
				}
		};
		Put(JetRgb, Jw, Jh, 0);
		Put(StarRgb, Sw, Sh, Jw + 2);
		K.JetU1 = float(Jw) / K.W;
		K.JetV1 = float(Jh) / K.H;
		K.StarV1 = float(Sh) / K.H;
		K.JetAspect = float(Jh) / float(Jw);
		K.StarAspect = float(Sh) / float(Sw);
		// the barrel's line through the jet: its brightness-weighted middle row in the third nearest the muzzle; the
		// starburst's hot middle likewise over all of it
		double Wy = 0.0, Ws = 0.0;
		for (int32 y = 0; y < Jh; ++y)
			for (int32 x = 0; x < FMath::Max(1, Jw / 3); ++x)
			{
				const FVector3f& C = JetRgb[y * Jw + x];
				const double B = FMath::Square(double(C.X + C.Y + C.Z));
				Wy += B * (y + 0.5);
				Ws += B;
			}
		K.JetAxisV = Ws > 0.0 ? float(Wy / Ws / Jh) : 0.5f;
		double Cx = 0.0, Cy = 0.0;
		Ws = 0.0;
		for (int32 y = 0; y < Sh; ++y)
			for (int32 x = 0; x < Sw; ++x)
			{
				const FVector3f& C = StarRgb[y * Sw + x];
				const double B = FMath::Pow(double(C.X + C.Y + C.Z), 4.0);
				Cx += B * (x + 0.5);
				Cy += B * (y + 0.5);
				Ws += B;
			}
		K.StarCentre = Ws > 0.0 ? FVector2f(float(Cx / Ws / Sw), float(Cy / Ws / Sh)) : FVector2f(0.5f);
		K.Source = FString::Printf(TEXT("%s chor %d: front %s, side %s -> %dx%d"), S.Nwx, S.Chor, *FWhat, *SWhat, K.W, K.H);
		K.bOk = true;
	}

	/** The mesh of one kind (cm at scale 1, the muzzle at the origin, the barrel along +X): the jet's 3 planes, then the
	 *  starburst's.  Two-sided material: one face each. */
	void FlashMesh(const AOutlawsLevelActor::FFlash3DKind& K, float L, TArray<FVector>& P, TArray<int32>& T, TArray<FVector>& Nrm, TArray<FVector2D>& Uv)
	{
		auto Quad = [&](const FVector& A, const FVector& B, const FVector& C, const FVector& D, const FVector2D& Ua, const FVector2D& Ub, const FVector2D& Uc, const FVector2D& Ud)
		{
			const int32 i = P.Num();
			P.Append({ A, B, C, D });
			Uv.Append({ Ua, Ub, Uc, Ud });
			const FVector N = FVector::CrossProduct(B - A, D - A).GetSafeNormal();
			Nrm.Append({ N, N, N, N });
			T.Append({ i, i + 1, i + 2, i, i + 2, i + 3 });
		};
		const double Hh = L * K.JetAspect, Up = Hh * K.JetAxisV, Dn = Hh - Up;
		for (int32 a = 0; a < 3; ++a)
		{
			const double R = FMath::DegreesToRadians(60.0 * a);
			const FVector D(0.0, FMath::Cos(R), FMath::Sin(R));
			// top-left (the muzzle, above the line), top-right, bottom-right, bottom-left
			Quad(D * Up, FVector(L, 0.0, 0.0) + D * Up, FVector(L, 0.0, 0.0) - D * Dn, -D * Dn,
				FVector2D(0.0, 0.0), FVector2D(K.JetU1, 0.0), FVector2D(K.JetU1, K.JetV1), FVector2D(0.0, K.JetV1));
		}
		// the starburst: as wide as the jet is long (its longer side), a little ahead of the muzzle, its hot middle on the line
		const double Sw = L / FMath::Max(1.f, K.StarAspect), Sh = Sw * K.StarAspect;
		const double Left = -Sw * K.StarCentre.X, Top = Sh * K.StarCentre.Y;
		const double X = L * 0.04;
		const double U0 = K.JetU1 + 2.0 / K.W;
		Quad(FVector(X, Left, Top), FVector(X, Left + Sw, Top), FVector(X, Left + Sw, Top - Sh), FVector(X, Left, Top - Sh),
			FVector2D(U0, 0.0), FVector2D(1.0, 0.0), FVector2D(1.0, K.StarV1), FVector2D(U0, K.StarV1));
	}

	AOutlawsLevelActor::FFlash3DKind GFlashCut[AOutlawsLevelActor::NumFlashKinds];
	const FOutlawsAssets* GFlashCutFrom = nullptr;
}

int32 AOutlawsLevelActor::FlashKindOfSound(const FString& Wav)
{
	FString Base;
	if (!Wav.Split(TEXT(","), &Base, nullptr)) Base = Wav;
	Base = FPaths::GetBaseFilename(Base.TrimStartAndEnd()).ToLower();
	if (Base == TEXT("rifle") || Base == TEXT("gatlshot")) return 1;
	if (Base == TEXT("single") || Base == TEXT("double") || Base == TEXT("sawed") || Base == TEXT("shotgun")) return 2;
	return 0;
}

const TCHAR* AOutlawsLevelActor::FlashKindName(int32 Kind)
{
	return Kind == 0 ? TEXT("pistol") : Kind == 1 ? TEXT("rifle") : Kind == 2 ? TEXT("shotgun") : TEXT("?");
}

void AOutlawsLevelActor::BuildFlash3D(const FOutlawsAssets& A)
{
	const double T0 = FPlatformTime::Seconds();
	static const bool bOff = OLFlags::Param(TEXT("OLNoFlash3D"));
	for (FFlash3DKind& K : Flash3D) K = FFlash3DKind();
	static const bool bSave = OLFlags::Param(TEXT("OLFlash3DSave"));
	// (tests under -nullrhi: -OLFlash3DSave cuts and saves the pictures; -OLFlash3DNull also makes the slots, untextured, so
	// the shots' paths can be checked without a GPU)
	static const bool bNullTest = OLFlags::Param(TEXT("OLFlash3DNull"));
	const bool bRender = FApp::CanEverRender();
	if (bOff || (!bRender && !bSave && !bNullTest))
	{
		Flash3DReport = bOff ? TEXT("3D muzzle flashes: off (-OLNoFlash3D), today's flash") : TEXT("3D muzzle flashes: off (nothing renders)");
		UE_LOG(LogOutlaws, Log, TEXT("OLFLASH3D %s"), *Flash3DReport);
		return;
	}
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_FxFlash.M_OLR_FxFlash"));
	if (!Base)
	{
		Flash3DReport = TEXT("3D muzzle flashes: off (M_OLR_FxFlash missing), today's flash");
		UE_LOG(LogOutlaws, Warning, TEXT("OLFLASH3D %s"), *Flash3DReport);
		return;
	}
	const bool bCut = GFlashCutFrom != &A;
	if (bCut)
	{
		for (int32 k = 0; k < NumFlashKinds; ++k) CutKind(A, k, GFlashCut[k]);
		GFlashCutFrom = &A;
	}
	if (Flash3DTex.Num() != NumFlashKinds) { Flash3DTex.Init(nullptr, NumFlashKinds); Flash3DMats.Init(nullptr, NumFlashKinds); }
	TArray<FString> Parts;
	for (int32 k = 0; k < NumFlashKinds; ++k)
	{
		FFlash3DKind& K = Flash3D[k];
		K = GFlashCut[k];
		UE_LOG(LogOutlaws, Log, TEXT("OLFLASH3D kind %s: %s%s"), FlashKindName(k), K.bOk ? TEXT("cut, ") : TEXT("OFF, "), *K.Source);
		if (!K.bOk) { Parts.Add(FString::Printf(TEXT("%s off"), FlashKindName(k))); continue; }
		if (bSave && bCut)
		{
			TArray64<uint8> Png;
			FImageUtils::PNGCompressImageArray(K.W, K.H, TArrayView64<const FColor>(K.Px.GetData(), K.Px.Num()), Png);
			FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("flash3d_%s.png"), FlashKindName(k))));
		}
		if (!bRender && !bNullTest) { K.bOk = false; Parts.Add(FString::Printf(TEXT("%s cut only (nothing renders)"), FlashKindName(k))); continue; }
		if (!Flash3DTex[k] && bRender) Flash3DTex[k] = OutlawsTextures::MakeTextureBGRA(K.W, K.H, K.Px, true, TF_Bilinear, TA_Clamp);
		if (!Flash3DTex[k] && bRender) { K.bOk = false; Parts.Add(FString::Printf(TEXT("%s off (no texture)"), FlashKindName(k))); continue; }
		if (!Flash3DMats[k])
		{
			UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(Base, this);
			if (Flash3DTex[k]) M->SetTextureParameterValue(TEXT("Tex"), Flash3DTex[k]);
			Flash3DMats[k] = M;
		}
		TArray<FVector> P, Nrm;
		TArray<int32> T;
		TArray<FVector2D> Uv;
		FlashMesh(K, GFlashSources[k].LengthCm, P, T, Nrm, Uv);
		for (int32 s = 0; s < SlotsPerKind; ++s)
		{
			FFlash3DSlot& S = Flash3DSlots[k * SlotsPerKind + s];
			S.Kind = k;
			S.Age = 99.f;
			UProceduralMeshComponent* Pm = S.Mesh.Get();
			if (!Pm)
			{
				Pm = NewObject<UProceduralMeshComponent>(this);
				Pm->bUseAsyncCooking = false;
				Pm->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				Pm->SetCastShadow(false);
				Pm->SetUsingAbsoluteLocation(true);
				Pm->SetUsingAbsoluteRotation(true);
				Pm->SetUsingAbsoluteScale(true);
				Pm->SetupAttachment(GetRootComponent());
				Pm->RegisterComponent();
				S.Mesh = Pm;
			}
			Pm->CreateMeshSection_LinearColor(0, P, T, Nrm, Uv, {}, {}, false);
			Pm->SetMaterial(0, Flash3DMats[k]);
			Pm->SetVisibility(false);
		}
		Parts.Add(FString::Printf(TEXT("%s %dx%d"), FlashKindName(k), K.W, K.H));
	}
	Flash3DReport = FString::Printf(TEXT("3D muzzle flashes: %s (%s, %.1f ms)"), *FString::Join(Parts, TEXT(", ")), bCut ? TEXT("cut now") : TEXT("kept from before"),
		(FPlatformTime::Seconds() - T0) * 1000.0);
	UE_LOG(LogOutlaws, Log, TEXT("OLFLASH3D %s"), *Flash3DReport);
}

bool AOutlawsLevelActor::MuzzleFlash3D(const FVector& Muzzle, const FVector& Bore, int32 Kind, float Size)
{
	if (!HasFlash3D(Kind) || Bore.IsNearlyZero()) return false;
	// the kind's slot that is free or oldest
	int32 Pick = INDEX_NONE;
	for (int32 s = 0; s < SlotsPerKind; ++s)
	{
		const int32 i = Kind * SlotsPerKind + s;
		if (!Flash3DSlots[i].Mesh.IsValid()) continue;
		if (Pick == INDEX_NONE || Flash3DSlots[i].Age > Flash3DSlots[Pick].Age) Pick = i;
	}
	if (Pick == INDEX_NONE) return false;
	FFlash3DSlot& S = Flash3DSlots[Pick];
	UProceduralMeshComponent* Pm = S.Mesh.Get();
	FRotator R = Bore.GetSafeNormal().Rotation();
	R.Roll = FMath::FRandRange(0.f, 360.f);
	Pm->SetWorldLocationAndRotation(Muzzle, R);
	Pm->SetWorldScale3D(FVector(FMath::FRandRange(0.85f, 1.15f) * FMath::Clamp(Size, 0.25f, 4.f)));
	Pm->SetVisibility(true);
	S.Age = 0.f;
	S.Frames = 0;
	++Flash3DShown[Kind];
	SetActorTickEnabled(true);
	return true;
}

void AOutlawsLevelActor::TickFlash3D(float Dt)
{
	for (FFlash3DSlot& S : Flash3DSlots)
	{
		if (S.Age > 90.f) continue;
		S.Age += Dt;
		++S.Frames;
		// 1 to 2 frames: on for the frame it was fired in and the next, a frame at most at a low rate
		if (!bFlash3DHold && (S.Frames >= 2 || S.Age >= 0.034f))
		{
			if (UProceduralMeshComponent* Pm = S.Mesh.Get()) Pm->SetVisibility(false);
			S.Age = 99.f;
		}
	}
}

void AOutlawsLevelActor::FlashHoldRelease()
{
	bFlash3DHold = false;
	for (FFlash3DSlot& S : Flash3DSlots)
	{
		if (UProceduralMeshComponent* Pm = S.Mesh.Get()) Pm->SetVisibility(false);
		S.Age = 99.f;
	}
}

int32 AOutlawsLevelActor::Flash3DVisible() const
{
	int32 N = 0;
	for (const FFlash3DSlot& S : Flash3DSlots) N += S.Mesh.IsValid() && S.Mesh->IsVisible() ? 1 : 0;
	return N;
}
