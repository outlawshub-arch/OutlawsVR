#include "OutlawsItem3d.h"
#include "OutlawsLevelActor.h"
#include "OutlawsTextures.h"
#include "OutlawsLab.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"

#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformTime.h"
#include "ImageUtils.h"
#include "Engine/Texture2D.h"

// (BUG-DEVDIR2) The runtime half of tools/olue/item3d/item_blocks.py: its op list (OPS) over the player's HD sprites, the
// image ops as Pillow 12 computes them (the bilinear resize's fixed-point taps, the QUAD warp's bicubic, the box-blur
// Gaussian, RGBA premultiplied for the first two), the maths as numpy 2 does it (float32 kept where numpy keeps it, python
// numbers weak), scipy's distance-transform fill, morphology and labels.  The -OLItem3dShipProbe measures every model
// against the dev PC's texture.

namespace
{
	using FJsonArr = TArray<TSharedPtr<FJsonValue>>;

	// ------------------------------------------------------------------ arrays as numpy has them
	enum class EDt : uint8 { Bool, U8, I64, F32, F64 };

	struct FArr
	{
		EDt Dt = EDt::F64;
		TArray<int32> Shape;          // rows, columns (, channels); a colour is {3}
		TArray<double> V;             // row-major, each value as its dtype holds it
		int32 Dim(int32 i) const { return Shape.IsValidIndex(i) ? Shape[i] : 1; }
		int32 H() const { return Dim(0); }
		int32 W() const { return Dim(1); }
		int32 C() const { return Shape.Num() > 2 ? Shape[2] : 1; }
	};

	bool IsFloat(EDt D) { return D == EDt::F32 || D == EDt::F64; }

	/** A value stored as dtype D: float32 rounded, integers truncated (uint8 wraps, as a C cast does). */
	double Cast(EDt D, double X)
	{
		switch (D)
		{
		case EDt::Bool: return X != 0.0 ? 1.0 : 0.0;
		case EDt::U8: return double(uint8(int64(X) & 255));
		case EDt::I64: return double(int64(X));
		case EDt::F32: return double(float(X));
		default: return X;
		}
	}

	/** numpy's result type of two arrays. */
	EDt Promote(EDt A, EDt B)
	{
		if (A == B) return A;
		if (A == EDt::Bool) return B;
		if (B == EDt::Bool) return A;
		if (A == EDt::F64 || B == EDt::F64) return EDt::F64;
		if (A == EDt::F32 || B == EDt::F32) return (A == EDt::I64 || B == EDt::I64) ? EDt::F64 : EDt::F32;
		return EDt::I64;
	}

	/** A python float with an array of D (NEP 50: weak, so a float array keeps its type). */
	EDt WithPyFloat(EDt D) { return IsFloat(D) ? D : EDt::F64; }

	FArr Make(EDt Dt, const TArray<int32>& Shape, double Fill = 0.0)
	{
		FArr A;
		A.Dt = Dt;
		A.Shape = Shape;
		int32 N = 1;
		for (int32 S : Shape) N *= S;
		A.V.Init(Fill, N);
		return A;
	}

	FArr MakeImage(int32 H, int32 W, int32 C) { return Make(EDt::U8, C == 1 ? TArray<int32>{ H, W } : TArray<int32>{ H, W, C }); }
	int32 Chans(const FArr& A) { return A.Shape.Num() > 2 ? A.Shape[2] : 1; }

	/** X's shape padded at the front to three dimensions. */
	void Dims3(const TArray<int32>& S, int32 D[3])
	{
		for (int32 i = 0; i < 3; ++i)
		{
			const int32 j = i - (3 - S.Num());
			D[i] = j >= 0 ? S[j] : 1;
		}
	}

	/** X's strides when broadcast to the shape To (0 along a dimension it has as 1). */
	void BStrides(const TArray<int32>& S, const TArray<int32>& To, int32 St[3])
	{
		int32 D[3];
		TArray<int32> Pad;
		for (int32 i = 0; i < To.Num() - S.Num(); ++i) Pad.Add(1);
		Pad.Append(S);
		Dims3(Pad, D);
		int32 Stride = 1;
		for (int32 i = 2; i >= 0; --i)
		{
			St[i] = D[i] == 1 ? 0 : Stride;
			Stride *= D[i];
		}
	}

	bool Broadcast(const TArray<int32>& A, const TArray<int32>& B, TArray<int32>& Out)
	{
		const int32 N = FMath::Max(A.Num(), B.Num());
		Out.SetNum(N);
		for (int32 i = 0; i < N; ++i)
		{
			const int32 Ai = i - (N - A.Num()), Bi = i - (N - B.Num());
			const int32 Sa = Ai >= 0 ? A[Ai] : 1, Sb = Bi >= 0 ? B[Bi] : 1;
			if (Sa != Sb && Sa != 1 && Sb != 1) return false;
			Out[i] = Sa == 1 ? Sb : Sa;
		}
		return true;
	}

	/** out = Fn(a, b) elementwise, numpy broadcasting, stored as Dt. */
	template<typename F>
	bool Binary(const FArr& A, const FArr& B, EDt Dt, F Fn, FArr& Out)
	{
		TArray<int32> S;
		if (!Broadcast(A.Shape, B.Shape, S)) return false;
		int32 D[3], Sa[3], Sb[3];
		Dims3(S, D);
		BStrides(A.Shape, S, Sa);
		BStrides(B.Shape, S, Sb);
		FArr R = Make(Dt, S);
		int32 k = 0;
		for (int32 i = 0; i < D[0]; ++i)
			for (int32 j = 0; j < D[1]; ++j)
				for (int32 c = 0; c < D[2]; ++c)
					R.V[k++] = Cast(Dt, Fn(A.V[i * Sa[0] + j * Sa[1] + c * Sa[2]], B.V[i * Sb[0] + j * Sb[1] + c * Sb[2]]));
		Out = MoveTemp(R);
		return true;
	}

	/** X = Y (Y broadcast to X's shape, cast to X's type). */
	bool AssignInto(FArr& X, const FArr& Y)
	{
		TArray<int32> S;
		if (!Broadcast(X.Shape, Y.Shape, S) || S != X.Shape) return false;
		int32 D[3], Sy[3];
		Dims3(S, D);
		BStrides(Y.Shape, S, Sy);
		int32 k = 0;
		for (int32 i = 0; i < D[0]; ++i)
			for (int32 j = 0; j < D[1]; ++j)
				for (int32 c = 0; c < D[2]; ++c)
					X.V[k++] = Cast(X.Dt, Y.V[i * Sy[0] + j * Sy[1] + c * Sy[2]]);
		return true;
	}

	FArr AsType(const FArr& X, EDt Dt)
	{
		FArr R = X;
		R.Dt = Dt;
		for (double& v : R.V) v = Cast(Dt, v);
		return R;
	}

	// ------------------------------------------------------------------ json
	double Num(const TSharedPtr<FJsonObject>& O, const TCHAR* K, double Def = 0.0)
	{
		double V = Def;
		O->TryGetNumberField(K, V);
		return V;
	}

	TArray<double> JNums(const TSharedPtr<FJsonValue>& V)
	{
		TArray<double> Out;
		if (V.IsValid() && V->Type == EJson::Array) for (const TSharedPtr<FJsonValue>& E : V->AsArray()) Out.Add(E->Type == EJson::Number ? E->AsNumber() : 0.0);
		return Out;
	}

	TArray<double> JNums(const TSharedPtr<FJsonObject>& O, const TCHAR* K)
	{
		return O->HasField(K) ? JNums(O->TryGetField(K)) : TArray<double>();
	}

	FString Str(const TSharedPtr<FJsonObject>& O, const TCHAR* K)
	{
		FString S;
		O->TryGetStringField(K, S);
		return S;
	}

	/** A python slice [a:b] of a dimension of size N from the op's [a, b] (null: open), clamped as python does. */
	void Slice(const TSharedPtr<FJsonObject>& O, const TCHAR* K, int32 N, int32& A, int32& B)
	{
		A = 0;
		B = N;
		const FJsonArr* Arr = nullptr;
		if (!O->TryGetArrayField(K, Arr) || Arr->Num() != 2) return;
		auto Fix = [N](const TSharedPtr<FJsonValue>& V, int32 Def)
		{
			if (!V.IsValid() || V->Type != EJson::Number) return Def;
			int32 I = int32(V->AsNumber());
			if (I < 0) I += N;
			return FMath::Clamp(I, 0, N);
		};
		A = Fix((*Arr)[0], 0);
		B = FMath::Max(A, Fix((*Arr)[1], N));
	}

	/** item_blocks.dec: a field as the recipe encodes it (a run-length mask, uint8 or float values, plain or in runs). */
	bool Decode(const TSharedPtr<FJsonObject>& D, FArr& Out)
	{
		TArray<int32> Shape;
		for (double S : JNums(D, TEXT("shape"))) Shape.Add(int32(S));
		int32 N = 1;
		for (int32 S : Shape) N *= S;
		const FJsonArr* Arr = nullptr;
		if (D->TryGetArrayField(TEXT("runs"), Arr))
		{
			Out = Make(EDt::Bool, Shape);
			int32 P = 0;
			bool bOn = false;
			for (const TSharedPtr<FJsonValue>& R : *Arr)
			{
				const int32 Len = int32(R->AsNumber());
				for (int32 i = 0; i < Len && P < N; ++i) Out.V[P++] = bOn ? 1.0 : 0.0;
				bOn = !bOn;
			}
			return P == N;
		}
		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			const EDt Dt = Pass == 0 ? EDt::U8 : EDt::F64;
			const FString Key = Pass == 0 ? TEXT("u8") : TEXT("f");
			if (D->TryGetArrayField(*Key, Arr))
			{
				Out = Make(Dt, Shape);
				if (Arr->Num() != N) return false;
				for (int32 i = 0; i < N; ++i) Out.V[i] = (*Arr)[i]->AsNumber();
				return true;
			}
			if (D->TryGetArrayField(*(Key + TEXT("_runs")), Arr))
			{
				Out = Make(Dt, Shape);
				int32 P = 0;
				for (int32 i = 0; i + 1 < Arr->Num(); i += 2)
				{
					const double V = (*Arr)[i]->AsNumber();
					const int32 Len = int32((*Arr)[i + 1]->AsNumber());
					for (int32 j = 0; j < Len && P < N; ++j) Out.V[P++] = V;
				}
				return P == N;
			}
		}
		return false;
	}

	// ------------------------------------------------------------------ scipy: the nearest feature, morphology, labels
	/** For every texel, the index of its nearest texel in Feat (Euclidean; -1 when there is none): scipy's
	 *  distance_transform_edt(~Feat, return_indices=True), per column the nearest such row, then per row the lower envelope
	 *  of those distances' parabolas (Felzenszwalb and Huttenlocher), ties to the upper and the left (the held guns' fill,
	 *  OutlawsPlayerWeapons FillFromNearestOpaque). */
	void NearestFeature(const TArray<uint8>& Feat, int32 W, int32 H, TArray<int32>& Out)
	{
		TArray<int32> NearRow;
		NearRow.Init(-1, W * H);
		Out.Init(-1, W * H);
		for (int32 x = 0; x < W; ++x)
		{
			int32 Last = -1;
			for (int32 y = 0; y < H; ++y)
			{
				if (Feat[y * W + x]) Last = y;
				NearRow[y * W + x] = Last;
			}
			Last = -1;
			for (int32 y = H - 1; y >= 0; --y)
			{
				if (Feat[y * W + x]) Last = y;
				const int32 Up = NearRow[y * W + x];
				const int64 DUp = Up >= 0 ? y - Up : MAX_int32, DDown = Last >= 0 ? Last - y : MAX_int32;
				NearRow[y * W + x] = DUp <= DDown ? Up : Last;
			}
		}
		TArray<int32> Env;
		TArray<double> EnvFrom, EnvF;
		Env.SetNumUninitialized(W);
		EnvFrom.SetNumUninitialized(W);
		EnvF.SetNumUninitialized(W);
		for (int32 y = 0; y < H; ++y)
		{
			int32 K = -1;
			for (int32 q = 0; q < W; ++q)
			{
				const int32 R = NearRow[y * W + q];
				if (R < 0) continue;
				const double Fq = double(y - R) * double(y - R);
				double Cross = 0.0;
				while (K >= 0)
				{
					const int32 P = Env[K];
					Cross = ((Fq + double(q) * q) - (EnvF[K] + double(P) * P)) / (2.0 * (q - P));
					if (Cross <= EnvFrom[K]) --K;
					else break;
				}
				++K;
				Env[K] = q;
				EnvF[K] = Fq;
				EnvFrom[K] = K == 0 ? -1e30 : Cross;
			}
			if (K < 0) continue;
			int32 J = 0;
			for (int32 x = 0; x < W; ++x)
			{
				while (J + 1 <= K && EnvFrom[J + 1] < x) ++J;
				const int32 Q = Env[J];
				Out[y * W + x] = NearRow[y * W + Q] * W + Q;
			}
		}
	}

	TArray<uint8> MaskOf(const FArr& A)
	{
		TArray<uint8> M;
		M.SetNumUninitialized(A.V.Num());
		for (int32 i = 0; i < A.V.Num(); ++i) M[i] = A.V[i] != 0.0 ? 1 : 0;
		return M;
	}

	FArr FromMask(const TArray<uint8>& M, int32 H, int32 W)
	{
		FArr A = Make(EDt::Bool, { H, W });
		for (int32 i = 0; i < M.Num(); ++i) A.V[i] = M[i];
		return A;
	}

	/** scipy binary_dilation (Erode: binary_erosion), the cross, border 0, Iter times. */
	TArray<uint8> Morph(TArray<uint8> M, int32 W, int32 H, int32 Iter, bool bErode)
	{
		TArray<uint8> N;
		for (int32 it = 0; it < Iter; ++it)
		{
			N = M;
			for (int32 y = 0; y < H; ++y)
				for (int32 x = 0; x < W; ++x)
				{
					auto At = [&](int32 xx, int32 yy) -> uint8 { return xx >= 0 && yy >= 0 && xx < W && yy < H ? M[yy * W + xx] : 0; };
					const uint8 C = M[y * W + x];
					if (bErode) N[y * W + x] = C && At(x - 1, y) && At(x + 1, y) && At(x, y - 1) && At(x, y + 1);
					else N[y * W + x] = C || At(x - 1, y) || At(x + 1, y) || At(x, y - 1) || At(x, y + 1);
				}
			M = MoveTemp(N);
		}
		return M;
	}

	/** scipy binary_fill_holes: what the background reaches from the border (cross steps) stays out, the rest is in. */
	TArray<uint8> FillHoles(const TArray<uint8>& M, int32 W, int32 H)
	{
		TArray<uint8> Out;
		Out.Init(1, W * H);
		TArray<int32> Stack;
		auto Seed = [&](int32 x, int32 y) { const int32 i = y * W + x; if (!M[i] && Out[i]) { Out[i] = 0; Stack.Add(i); } };
		for (int32 x = 0; x < W; ++x) { Seed(x, 0); Seed(x, H - 1); }
		for (int32 y = 0; y < H; ++y) { Seed(0, y); Seed(W - 1, y); }
		while (Stack.Num())
		{
			const int32 i = Stack.Pop(false);
			const int32 x = i % W, y = i / W;
			if (x > 0) Seed(x - 1, y);
			if (x + 1 < W) Seed(x + 1, y);
			if (y > 0) Seed(x, y - 1);
			if (y + 1 < H) Seed(x, y + 1);
		}
		return Out;
	}

	/** The biggest cross-connected piece (labels in raster order, the first on a tie: scipy label then argmax). */
	TArray<uint8> Largest(const TArray<uint8>& M, int32 W, int32 H)
	{
		TArray<int32> Lab;
		Lab.Init(0, W * H);
		int32 Best = 0, BestN = -1, Next = 0;
		TArray<int32> Stack;
		for (int32 s = 0; s < W * H; ++s)
		{
			if (!M[s] || Lab[s]) continue;
			++Next;
			int32 Count = 0;
			Lab[s] = Next;
			Stack.Add(s);
			while (Stack.Num())
			{
				const int32 i = Stack.Pop(false);
				++Count;
				const int32 x = i % W, y = i / W;
				const int32 Nb[4] = { x > 0 ? i - 1 : -1, x + 1 < W ? i + 1 : -1, y > 0 ? i - W : -1, y + 1 < H ? i + W : -1 };
				for (int32 n : Nb) if (n >= 0 && M[n] && !Lab[n]) { Lab[n] = Next; Stack.Add(n); }
			}
			if (Count > BestN) { BestN = Count; Best = Next; }
		}
		if (!Next) return M;
		TArray<uint8> Out;
		Out.SetNumUninitialized(W * H);
		for (int32 i = 0; i < W * H; ++i) Out[i] = Lab[i] == Best ? 1 : 0;
		return Out;
	}

	/** scipy uniform_filter1d along Axis (mode reflect), the line in double, the result stored as X's type. */
	void Uniform1d(FArr& X, int32 Axis, int32 Size)
	{
		int32 D[3];
		Dims3(X.Shape, D);
		const int32 Len = D[Axis];
		const int32 S1 = Size / 2, S2 = Size - S1 - 1;
		int32 St[3];
		BStrides(X.Shape, X.Shape, St);
		TArray<double> Line, Res;
		Line.SetNumUninitialized(Len + Size - 1);
		Res.SetNumUninitialized(Len);
		const int32 A1 = Axis == 0 ? 1 : 0, A2 = Axis == 2 ? 1 : 2;
		auto Refl = [Len](int32 i)
		{
			// "reflect": d c b a | a b c d | d c b a
			const int32 P = 2 * Len;
			i = ((i % P) + P) % P;
			return i < Len ? i : P - 1 - i;
		};
		for (int32 u = 0; u < D[A1]; ++u)
			for (int32 v = 0; v < D[A2]; ++v)
			{
				const int32 Base = u * St[A1] + v * St[A2];
				for (int32 i = 0; i < Len + Size - 1; ++i) Line[i] = X.V[Base + Refl(i - S1) * St[Axis]];
				double Tmp = 0.0;
				for (int32 i = 0; i < Size; ++i) Tmp += Line[i];
				Tmp /= double(Size);
				Res[0] = Tmp;
				for (int32 i = 1; i < Len; ++i)
				{
					Tmp += (Line[i + Size - 1] - Line[i - 1]) / double(Size);
					Res[i] = Tmp;
				}
				for (int32 i = 0; i < Len; ++i) X.V[Base + i * St[Axis]] = Cast(X.Dt, Res[i]);
			}
		(void)S2;
	}

	// ------------------------------------------------------------------ Pillow
	void Premultiply(FArr& X)          // RGBA -> RGBa (MULDIV255)
	{
		for (int32 i = 0; i + 3 < X.V.Num(); i += 4)
		{
			const uint32 A = uint32(X.V[i + 3]);
			for (int32 c = 0; c < 3; ++c)
			{
				const uint32 T = uint32(X.V[i + c]) * A + 128;
				X.V[i + c] = double(((T >> 8) + T) >> 8);
			}
		}
	}

	void Unpremultiply(FArr& X)        // RGBa -> RGBA
	{
		for (int32 i = 0; i + 3 < X.V.Num(); i += 4)
		{
			const uint32 A = uint32(X.V[i + 3]);
			if (A == 0 || A == 255) continue;
			for (int32 c = 0; c < 3; ++c) X.V[i + c] = double(FMath::Min<uint32>(255, (255 * uint32(X.V[i + c])) / A));
		}
	}

	/** Resample.c's filters: bilinear (support 1) and lanczos (support 3, the truncated sinc). */
	double FilterAt(double T, bool bLanczos)
	{
		if (bLanczos)
		{
			auto Sinc = [](double X) { if (X == 0.0) return 1.0; X = X * PI; return FMath::Sin(X) / X; };
			return (-3.0 <= T && T < 3.0) ? Sinc(T) * Sinc(T / 3) : 0.0;
		}
		if (T < 0.0) T = -T;
		return T < 1.0 ? 1.0 - T : 0.0;
	}

	/** Resample.c precompute_coeffs and normalize_coeffs_8bpc. */
	void Coeffs(int32 InSize, double In0, double In1, int32 OutSize, int32& KSize, TArray<int32>& Bounds, TArray<int64>& K, bool bLanczos = false)
	{
		constexpr int32 Prec = 32 - 8 - 2;
		const double Scale = (In1 - In0) / OutSize;
		const double FilterScale = FMath::Max(Scale, 1.0);
		const double Support = (bLanczos ? 3.0 : 1.0) * FilterScale;
		KSize = int32(FMath::CeilToDouble(Support)) * 2 + 1;
		TArray<double> Kd;
		Kd.Init(0.0, OutSize * KSize);
		Bounds.SetNumUninitialized(OutSize * 2);
		for (int32 xx = 0; xx < OutSize; ++xx)
		{
			const double Center = In0 + (xx + 0.5) * Scale;
			double Ww = 0.0;
			const double Ss = 1.0 / FilterScale;
			int32 XMin = int32(Center - Support + 0.5);
			if (XMin < 0) XMin = 0;
			int32 XMax = int32(Center + Support + 0.5);
			if (XMax > InSize) XMax = InSize;
			XMax -= XMin;
			double* Kk = &Kd[xx * KSize];
			for (int32 x = 0; x < XMax; ++x)
			{
				const double Wt = FilterAt((x + XMin - Center + 0.5) * Ss, bLanczos);
				Kk[x] = Wt;
				Ww += Wt;
			}
			for (int32 x = 0; x < XMax; ++x) if (Ww != 0.0) Kk[x] /= Ww;
			Bounds[xx * 2] = XMin;
			Bounds[xx * 2 + 1] = XMax;
		}
		K.SetNumUninitialized(Kd.Num());
		for (int32 i = 0; i < Kd.Num(); ++i) K[i] = Kd[i] < 0 ? int64(-0.5 + Kd[i] * (1 << Prec)) : int64(0.5 + Kd[i] * (1 << Prec));
	}

	double Clip8(int64 In)
	{
		constexpr int32 Prec = 32 - 8 - 2;
		if (In >= (int64(1) << Prec << 8)) return 255.0;
		if (In <= 0) return 0.0;
		return double(In >> Prec);
	}

	/** Image.resize(size, BILINEAR or LANCZOS) of a uint8 image (RGBA premultiplied first, as Pillow does). */
	FArr ResizeBilinear(const FArr& In, int32 OW, int32 OH, bool bLanczos = false)
	{
		const int32 W = In.W(), H = In.H(), C = Chans(In);
		if (OW == W && OH == H) return In;
		FArr X = In;
		if (C == 4) Premultiply(X);
		constexpr int32 Half = 1 << (32 - 8 - 2 - 1);
		if (OW != W)
		{
			int32 Ks = 0;
			TArray<int32> B;
			TArray<int64> K;
			Coeffs(W, 0.0, W, OW, Ks, B, K, bLanczos);
			FArr T = MakeImage(H, OW, C);
			for (int32 y = 0; y < H; ++y)
				for (int32 xx = 0; xx < OW; ++xx)
					for (int32 c = 0; c < C; ++c)
					{
						int64 Ss = Half;
						for (int32 x = 0; x < B[xx * 2 + 1]; ++x) Ss += int64(X.V[(y * W + x + B[xx * 2]) * C + c]) * K[xx * Ks + x];
						T.V[(y * OW + xx) * C + c] = Clip8(Ss);
					}
			X = MoveTemp(T);
		}
		if (OH != H)
		{
			int32 Ks = 0;
			TArray<int32> B;
			TArray<int64> K;
			Coeffs(H, 0.0, H, OH, Ks, B, K, bLanczos);
			FArr T = MakeImage(OH, OW, C);
			for (int32 yy = 0; yy < OH; ++yy)
				for (int32 x = 0; x < OW; ++x)
					for (int32 c = 0; c < C; ++c)
					{
						int64 Ss = Half;
						for (int32 y = 0; y < B[yy * 2 + 1]; ++y) Ss += int64(X.V[((y + B[yy * 2]) * OW + x) * C + c]) * K[yy * Ks + y];
						T.V[(yy * OW + x) * C + c] = Clip8(Ss);
					}
			X = MoveTemp(T);
		}
		if (C == 4) Unpremultiply(X);
		return X;
	}

	/** Image.transform(size, QUAD, data, BICUBIC) of a uint8 image (RGBA premultiplied as Pillow does): Geometry.c's
	 *  quad_transform and bicubic filter, 0 outside. */
	FArr QuadBicubic(const FArr& In, int32 OW, int32 OH, const TArray<double>& Data)
	{
		const int32 W = In.W(), H = In.H(), C = Chans(In);
		FArr X = In;
		if (C == 4) Premultiply(X);
		const double X0 = Data[0], Y0 = Data[1];
		const double As = 1.0 / OW, At = 1.0 / OH;
		// NW, SW, SE, NE
		const double A[8] = { X0, (Data[6] - X0) * As, (Data[2] - X0) * At, (Data[4] - Data[2] - Data[6] + X0) * As * At,
			Y0, (Data[7] - Y0) * As, (Data[3] - Y0) * At, (Data[5] - Data[3] - Data[7] + Y0) * As * At };
		FArr Out = MakeImage(OH, OW, C);
		auto Cubic = [](double V1, double V2, double V3, double V4, double D)
		{
			const double P1 = V2, P2 = -V1 + V3, P3 = 2 * (V1 - V2) + V3 - V4, P4 = -V1 + V2 - V3 + V4;
			return P1 + D * (P2 + D * (P3 + D * P4));
		};
		auto XClip = [W](int32 x) { return x < 0 ? 0 : x < W ? x : W - 1; };
		for (int32 oy = 0; oy < OH; ++oy)
			for (int32 ox = 0; ox < OW; ++ox)
			{
				const double Xi = ox + 0.5, Yi = oy + 0.5;
				double Xin = A[0] + A[1] * Xi + A[2] * Yi + A[3] * Xi * Yi;
				double Yin = A[4] + A[5] * Xi + A[6] * Yi + A[7] * Xi * Yi;
				if (Xin < 0.0 || Xin >= W || Yin < 0.0 || Yin >= H) continue;
				Xin -= 0.5;
				Yin -= 0.5;
				int32 x = Xin < 0.0 ? int32(FMath::FloorToDouble(Xin)) : int32(Xin);
				int32 y = Yin < 0.0 ? int32(FMath::FloorToDouble(Yin)) : int32(Yin);
				const double Dx = Xin - x, Dy = Yin - y;
				--x;
				--y;
				const int32 Xs[4] = { XClip(x), XClip(x + 1), XClip(x + 2), XClip(x + 3) };
				for (int32 c = 0; c < C; ++c)
				{
					auto Row = [&](int32 yy) { return Cubic(X.V[(yy * W + Xs[0]) * C + c], X.V[(yy * W + Xs[1]) * C + c], X.V[(yy * W + Xs[2]) * C + c], X.V[(yy * W + Xs[3]) * C + c], Dx); };
					const double V1 = Row(y < 0 ? 0 : y < H ? y : H - 1);
					const double V2 = (y + 1 >= 0 && y + 1 < H) ? Row(y + 1) : V1;
					const double V3 = (y + 2 >= 0 && y + 2 < H) ? Row(y + 2) : V2;
					const double V4 = (y + 3 >= 0 && y + 3 < H) ? Row(y + 3) : V3;
					const double V = Cubic(V1, V2, V3, V4, Dy);
					Out.V[(oy * OW + ox) * C + c] = V <= 0.0 ? 0.0 : V >= 255.0 ? 255.0 : double(uint8(V));
				}
			}
		if (C == 4) Unpremultiply(Out);
		return Out;
	}

	/** BoxBlur.c ImagingLineBoxBlur over one line of one channel (radius, edges, weights as ImagingHorizontalBoxBlur). */
	void BoxLine(const uint32* In, uint32* Out, int32 LastX, int32 Radius, int32 EdgeA, int32 EdgeB, uint32 Ww, uint32 Fw)
	{
		uint32 Acc = In[0] * uint32(Radius + 1);
		for (int32 x = 0; x < EdgeA - 1; ++x) Acc += In[x];
		Acc += In[LastX] * uint32(Radius - EdgeA + 1);
		auto Step = [&](int32 x, int32 Sub, int32 Add, int32 Left, int32 Right)
		{
			Acc += In[Add] - In[Sub];
			const uint32 Bulk = Acc * Ww + (In[Left] + In[Right]) * Fw;
			Out[x] = uint8((Bulk + (1u << 23)) >> 24);
		};
		if (EdgeA <= EdgeB)
		{
			for (int32 x = 0; x < EdgeA; ++x) Step(x, 0, x + Radius, 0, x + Radius + 1);
			for (int32 x = EdgeA; x < EdgeB; ++x) Step(x, x - Radius - 1, x + Radius, x - Radius - 1, x + Radius + 1);
			for (int32 x = EdgeB; x <= LastX; ++x) Step(x, x - Radius - 1, LastX, x - Radius - 1, LastX);
		}
		else
		{
			for (int32 x = 0; x < EdgeB; ++x) Step(x, 0, x + Radius, 0, x + Radius + 1);
			for (int32 x = EdgeB; x < EdgeA; ++x) Step(x, 0, LastX, 0, LastX);
			for (int32 x = EdgeA; x <= LastX; ++x) Step(x, x - Radius - 1, LastX, x - Radius - 1, LastX);
		}
	}

	/** ImageFilter.GaussianBlur(radius) of a uint8 image in its own mode: three box blurs a way, rows then columns. */
	FArr GaussianBlur(const FArr& In, double RadiusIn)
	{
		const int32 W = In.W(), H = In.H(), C = Chans(In);
		const float Radius = float(RadiusIn);
		auto BoxRadius = [](float R)
		{
			const float Sigma2 = R * R / 3;
			const float L = float(FMath::Sqrt(12.0 * Sigma2 + 1.0));
			const float Lf = float(FMath::FloorToDouble((L - 1.0) / 2.0));
			float A = (2 * Lf + 1) * (Lf * (Lf + 1) - 3 * Sigma2);
			A /= 6 * (Sigma2 - (Lf + 1) * (Lf + 1));
			return Lf + A;
		};
		const float Br = BoxRadius(Radius);
		FArr X = In;
		if (Br == 0.f) return X;
		auto Pass = [&](bool bRows)
		{
			const int32 Len = bRows ? W : H, Lines = bRows ? H : W;
			const int32 R = int32(Br);
			const uint32 Ww = uint32(float(1 << 24) / (Br * 2 + 1));
			const uint32 Fw = ((1 << 24) - (R * 2 + 1) * Ww) / 2;
			const int32 EdgeA = FMath::Min(R + 1, Len), EdgeB = FMath::Max(Len - R - 1, 0);
			TArray<uint32> Li, Lo;
			Li.SetNumUninitialized(Len);
			Lo.SetNumUninitialized(Len);
			for (int32 l = 0; l < Lines; ++l)
				for (int32 c = 0; c < C; ++c)
				{
					for (int32 n = 0; n < 3; ++n)
					{
						for (int32 i = 0; i < Len; ++i) Li[i] = uint32(X.V[((bRows ? l * W + i : i * W + l)) * C + c]);
						BoxLine(Li.GetData(), Lo.GetData(), Len - 1, R, EdgeA, EdgeB, Ww, Fw);
						for (int32 i = 0; i < Len; ++i) X.V[((bRows ? l * W + i : i * W + l)) * C + c] = double(Lo[i]);
					}
				}
		};
		Pass(true);
		Pass(false);
		return X;
	}

	/** Image.crop(rect): texels outside are 0. */
	FArr Crop(const FArr& In, int32 X0, int32 Y0, int32 X1, int32 Y1)
	{
		const int32 C = Chans(In);
		FArr Out = MakeImage(Y1 - Y0, X1 - X0, C);
		for (int32 y = Y0; y < Y1; ++y)
			for (int32 x = X0; x < X1; ++x)
			{
				if (x < 0 || y < 0 || x >= In.W() || y >= In.H()) continue;
				for (int32 c = 0; c < C; ++c) Out.V[((y - Y0) * (X1 - X0) + x - X0) * C + c] = In.V[(y * In.W() + x) * C + c];
			}
		return Out;
	}

	/** Image.paste(src, at) with no mask: src in dst's mode (RGB gets alpha 255, RGBA drops it), clipped to dst. */
	void Paste(FArr& Dst, const FArr& Src, int32 AtX, int32 AtY)
	{
		const int32 Cd = Chans(Dst), Cs = Chans(Src);
		for (int32 y = 0; y < Src.H(); ++y)
			for (int32 x = 0; x < Src.W(); ++x)
			{
				const int32 Dx = AtX + x, Dy = AtY + y;
				if (Dx < 0 || Dy < 0 || Dx >= Dst.W() || Dy >= Dst.H()) continue;
				for (int32 c = 0; c < Cd; ++c)
				{
					double V;
					if (c < 3) V = Src.V[(y * Src.W() + x) * Cs + (Cs == 1 ? 0 : c)];
					else V = Cs == 4 ? Src.V[(y * Src.W() + x) * Cs + 3] : 255.0;
					Dst.V[(Dy * Dst.W() + Dx) * Cd + c] = V;
				}
			}
	}

	/** item_blocks._opaque / make_item_ship.bleed: the RGB of every texel at or under alpha 127 from the nearest over it.
	 *  bAlpha255: then alpha 255 everywhere (the "opaque" op). */
	void FillNearest(FArr& X, bool bAlpha255)
	{
		const int32 W = X.W(), H = X.H();
		TArray<uint8> Solid;
		Solid.SetNumUninitialized(W * H);
		bool bAny = false, bAll = true;
		for (int32 i = 0; i < W * H; ++i)
		{
			Solid[i] = X.V[i * 4 + 3] > 127 ? 1 : 0;
			bAny |= Solid[i] != 0;
			bAll &= Solid[i] != 0;
		}
		if (bAny && !bAll)
		{
			TArray<int32> Near;
			NearestFeature(Solid, W, H, Near);
			const TArray<double> Src = X.V;
			for (int32 i = 0; i < W * H; ++i) for (int32 c = 0; c < 3; ++c) X.V[i * 4 + c] = Src[Near[i] * 4 + c];
		}
		if (bAlpha255) for (int32 i = 0; i < W * H; ++i) X.V[i * 4 + 3] = 255.0;
	}

	/** scenery.strip_outline / rework._strip_ink: the drawn ink outline (Ink texels wide) painted with the nearest colour
	 *  deeper inside. */
	void StripOutline(FArr& X, int32 Ink)
	{
		if (Ink <= 0) return;
		const int32 W = X.W(), H = X.H();
		TArray<uint8> Bg, Deep;
		Bg.SetNumUninitialized(W * H);
		Deep.Init(0, W * H);
		for (int32 i = 0; i < W * H; ++i) Bg[i] = X.V[i * 4 + 3] > 127 ? 0 : 1;
		TArray<int32> NearBg;
		NearestFeature(Bg, W, H, NearBg);
		bool bAny = false;
		for (int32 i = 0; i < W * H; ++i)
		{
			if (Bg[i] || NearBg[i] < 0) { Deep[i] = !Bg[i] && NearBg[i] < 0; bAny |= Deep[i] != 0; continue; }
			const int64 Dx = i % W - NearBg[i] % W, Dy = i / W - NearBg[i] / W;
			Deep[i] = Dx * Dx + Dy * Dy > int64(Ink) * Ink ? 1 : 0;
			bAny |= Deep[i] != 0;
		}
		if (!bAny) return;
		TArray<int32> Near;
		NearestFeature(Deep, W, H, Near);
		const TArray<double> Src = X.V;
		for (int32 i = 0; i < W * H; ++i)
		{
			if (Bg[i] || Deep[i]) continue;
			for (int32 c = 0; c < 3; ++c) X.V[i * 4 + c] = Src[Near[i] * 4 + c];
		}
	}

	// ------------------------------------------------------------------ sample specs (item_blocks.colour)
	/** A number as python holds it: a python int or float, or a numpy scalar of a type (the type decides the maths). */
	enum class EKind : uint8 { PyInt, PyFloat, I64, F32, F64 };
	struct FNum { double V = 0.0; EKind K = EKind::PyFloat; };

	FNum NumOf(double V, EDt Dt)
	{
		return { V, Dt == EDt::F32 ? EKind::F32 : Dt == EDt::F64 ? EKind::F64 : EKind::I64 };
	}

	/** q * k, k a python number. */
	FNum MulK(FNum Q, double K)
	{
		if (Q.K == EKind::F32) return { double(float(Q.V) * float(K)), EKind::F32 };
		if (Q.K == EKind::F64 || Q.K == EKind::I64) return { Q.V * K, EKind::F64 };
		return { Q.V * K, EKind::PyFloat };
	}

	/** q + k, k a python number. */
	FNum AddK(FNum Q, double K)
	{
		if (Q.K == EKind::F32) return { double(float(Q.V) + float(K)), EKind::F32 };
		if (Q.K == EKind::F64 || Q.K == EKind::I64) return { Q.V + K, EKind::F64 };
		return { Q.V + K, EKind::PyFloat };
	}

	FNum Lit(double V) { return { V, FMath::FloorToDouble(V) == V ? EKind::PyInt : EKind::PyFloat }; }

	/** np.array(list of these): its type. */
	EDt ArrayType(const TArray<FNum>& Ns)
	{
		bool bAllInt = true, bAllF32 = true;
		for (const FNum& N : Ns)
		{
			bAllInt &= N.K == EKind::PyInt || N.K == EKind::I64;
			bAllF32 &= N.K == EKind::F32;
		}
		return bAllInt ? EDt::I64 : bAllF32 && Ns.Num() ? EDt::F32 : EDt::F64;
	}

	/** One reduction's float accumulator as numpy's (float32 sequential for float32 data). */
	struct FAcc
	{
		bool b32 = false;
		float F = 0.f;
		double D = 0.0;
		void Add(double V) { if (b32) F = float(F + float(V)); else D += V; }
		double Mean(int32 N) const { return b32 ? double(F / float(N)) : D / double(N); }
	};

	/** np.percentile(v, q) (method linear, q a python number) of sorted values in Dt. */
	double Percentile(const TArray<double>& Sorted, double Q, EDt Dt)
	{
		const int32 N = Sorted.Num();
		const double Virt = double(N - 1) * (Q / 100.0);
		int32 Prev = int32(FMath::FloorToDouble(Virt));
		int32 Next = Prev + 1;
		if (Virt >= N - 1) { Prev = Next = N - 1; }
		else if (Virt < 0) { Prev = Next = 0; }
		const double Gamma = Virt - FMath::FloorToDouble(Virt);
		const double A = Sorted[Prev], B = Sorted[FMath::Min(Next, N - 1)];
		if (Dt == EDt::F32)
		{
			const float Diff = float(B) - float(A);
			if (Gamma >= 0.5) return double(float(B) - Diff * float(1.0 - Gamma));
			return double(float(A) + Diff * float(Gamma));
		}
		const double Diff = B - A;
		if (Gamma >= 0.5) return B - Diff * (1.0 - Gamma);
		return A + Diff * Gamma;
	}

	/** item_blocks.predicate's operand over one pixel (px: r, g, b, a in Dt). */
	double PredValue(const TSharedPtr<FJsonValue>& E, const double* Px, EDt Dt)
	{
		auto Ch = [&](const FString& N) { const int32 i = FString(TEXT("rgba")).Find(N); return Px[FMath::Max(0, i)]; };
		auto Rd = [Dt](double V) { return Dt == EDt::F32 ? double(float(V)) : V; };
		if (E->Type == EJson::Number) return E->AsNumber();
		if (E->Type == EJson::String)
		{
			const FString S = E->AsString();
			if (S == TEXT("sum")) return Rd(Rd(Px[0] + Px[1]) + Px[2]);
			return Ch(S);
		}
		const FJsonArr& A = E->AsArray();
		if (A.Num() == 3 && A[0]->Type == EJson::String && A[0]->AsString() == TEXT("absdiff"))
		{
			const double D = FMath::Abs(Ch(A[1]->AsString()) - Ch(A[2]->AsString()));
			return Dt == EDt::U8 ? double((int32(Ch(A[1]->AsString())) - int32(Ch(A[2]->AsString()))) & 255) : D;
		}
		const double V = PredValue(A[0], Px, Dt);
		const double K = A[2]->AsNumber();
		if (A[1]->AsString() == TEXT("*")) return Dt == EDt::F32 ? double(float(V) * float(K)) : V * K;
		return Dt == EDt::F32 ? double(float(V) + float(K)) : V + K;
	}

	bool Predicate(const FJsonArr& Terms, const double* Px, EDt Dt)
	{
		for (const TSharedPtr<FJsonValue>& T : Terms)
		{
			if (T->Type == EJson::String)
			{
				if (T->AsString() == TEXT("opaque") && !(Px[3] > 127)) return false;
				continue;
			}
			if (T->Type == EJson::Object)
			{
				const FJsonArr* Not = nullptr;
				if (T->AsObject()->TryGetArrayField(TEXT("not"), Not) && Predicate(*Not, Px, Dt)) return false;
				continue;
			}
			const FJsonArr& A = T->AsArray();
			const double L = PredValue(A[0], Px, Dt), R = PredValue(A[2], Px, Dt);
			const FString Op = A[1]->AsString();
			const bool bOk = Op == TEXT("<") ? L < R : Op == TEXT(">") ? L > R : L >= R;
			if (!bOk) return false;
		}
		return true;
	}

	/** item_blocks.colour: a sample spec over the model's image (Img; bImgF32: as float32, else its uint8) or the registers. */
	bool Colour(const TSharedPtr<FJsonObject>& Spec, const TMap<FString, FArr>& Regs, const FArr* Img, bool bImgF32, TArray<FNum>& V)
	{
		V.Reset();
		const FJsonArr* AtArr = nullptr;
		if (Spec->TryGetArrayField(TEXT("at"), AtArr))
		{
			if (!Img) return false;
			const TArray<double> At = JNums(Spec, TEXT("at"));
			const int32 X = int32(At[0]), Y = int32(At[1]), R = int32(At[2]);
			const int32 Y0 = FMath::Max(0, Y - R), Y1 = FMath::Min(Img->H(), Y + R + 1), X0 = FMath::Max(0, X - R), X1 = FMath::Min(Img->W(), X + R + 1);
			const bool bOpaque = Str(Spec, TEXT("mode")) == TEXT("opaque");
			FAcc Acc[3];
			int32 N = 0;
			for (int32 c = 0; c < 3; ++c) Acc[c].b32 = bImgF32;
			for (int32 y = Y0; y < Y1; ++y)
				for (int32 x = X0; x < X1; ++x)
				{
					const double* P = &Img->V[(y * Img->W() + x) * 4];
					if (bOpaque && !(P[3] > 127)) continue;
					for (int32 c = 0; c < 3; ++c) Acc[c].Add(P[c]);
					++N;
				}
			const FJsonArr& Ks = Spec->GetArrayField(TEXT("k"));
			TArray<FNum> C;
			for (int32 c = 0; c < 3; ++c) C.Add(N ? FNum{ Acc[c].Mean(N), bImgF32 ? EKind::F32 : EKind::F64 } : FNum{ 60.0, EKind::F64 });
			int32 Ki = 0;
			if (bOpaque)
			{
				const double K0 = Ks.Num() ? Ks[0]->AsNumber() : 1.0;
				Ki = 1;
				for (FNum& Q : C)
				{
					FNum M = MulK(Q, K0);
					const double Mx = M.V > 0.0 ? M.V : 0.0;              // max(0, v): v when over 0
					const double Mn = 255.0 < Mx ? 255.0 : Mx;              // min(255, .)
					Q = { double(int64(Mn)), EKind::PyInt };
				}
			}
			else
			{
				for (FNum& Q : C) Q = { double(int64(Q.V)), EKind::PyInt };
			}
			for (; Ki < Ks.Num(); ++Ki)
			{
				TArray<double> Kk = JNums(Ks[Ki]);
				if (Kk.Num() != 3) Kk.Init(Ks[Ki]->AsNumber(), 3);
				for (int32 c = 0; c < 3; ++c) C[c] = { double(int64(C[c].V * Kk[c])), EKind::PyInt };
			}
			V = C;
			return true;
		}
		FString Ref;
		if (Spec->TryGetStringField(TEXT("ref"), Ref))
		{
			const FArr* A = Regs.Find(Ref);
			if (!A) return false;
			for (double X : A->V) V.Add(NumOf(X, A->Dt));
		}
		else
		{
			const TSharedPtr<FJsonObject>* PxP = nullptr;
			if (!Spec->TryGetObjectField(TEXT("px"), PxP)) return false;
			const TSharedPtr<FJsonObject>& P = *PxP;
			FString SrcName;
			const FArr* Src = Img;
			if (P->TryGetStringField(TEXT("src"), SrcName)) Src = Regs.Find(SrcName);
			if (!Src) return false;
			const EDt Dt = Str(P, TEXT("dtype")) == TEXT("f32") ? EDt::F32 : EDt::F64;
			int32 X0 = 0, Y0 = 0, X1 = Src->W(), Y1 = Src->H();
			const TArray<double> Rect = JNums(P, TEXT("rect"));
			if (Rect.Num() == 4)
			{
				auto Fix = [](double V, int32 N) { int32 I = int32(V); if (I < 0) I += N; return FMath::Clamp(I, 0, N); };
				X0 = Fix(Rect[0], Src->W()); Y0 = Fix(Rect[1], Src->H()); X1 = FMath::Max(X0, Fix(Rect[2], Src->W())); Y1 = FMath::Max(Y0, Fix(Rect[3], Src->H()));
			}
			const FJsonArr* Where = nullptr;
			static const FJsonArr NoTerms;
			if (!P->TryGetArrayField(TEXT("where"), Where)) Where = &NoTerms;
			const int32 Cs = Chans(*Src);
			TArray<double> Px;          // N x 3, in Dt
			TArray<FIntPoint> At;
			for (int32 y = Y0; y < Y1; ++y)
				for (int32 x = X0; x < X1; ++x)
				{
					double Q[4];
					for (int32 c = 0; c < 4; ++c) Q[c] = c < Cs ? Cast(Dt, Src->V[(y * Src->W() + x) * Cs + c]) : 255.0;
					if (!Predicate(*Where, Q, Dt)) continue;
					Px.Append(Q, 3);
					At.Add(FIntPoint(x, y));
				}
			const int32 N = At.Num();
			const TSharedPtr<FJsonObject>& Red = Spec->GetObjectField(TEXT("reduce"));
			auto MeanOf = [&](const TArray<int32>& Rows)
			{
				for (int32 c = 0; c < 3; ++c)
				{
					FAcc A;
					A.b32 = Dt == EDt::F32;
					for (int32 i : Rows) A.Add(Px[i * 3 + c]);
					V.Add(NumOf(A.Mean(Rows.Num()), Dt));
				}
			};
			auto RowMean = [&](int32 i)      // px.mean(1): r + g + b, then / 3, in Dt
			{
				if (Dt == EDt::F32) return double((float(Px[i * 3]) + float(Px[i * 3 + 1]) + float(Px[i * 3 + 2])) / 3.f);
				return (Px[i * 3] + Px[i * 3 + 1] + Px[i * 3 + 2]) / 3.0;
			};
			TArray<int32> All;
			for (int32 i = 0; i < N; ++i) All.Add(i);
			if (!N)
			{
				for (double E : JNums(Spec, TEXT("else"))) V.Add(Lit(E));
			}
			else if (Red->HasField(TEXT("mean")))
			{
				MeanOf(All);
			}
			else if (Red->HasField(TEXT("median")) || Red->HasField(TEXT("percentile")))
			{
				const bool bMedian = Red->HasField(TEXT("median"));
				for (int32 c = 0; c < 3; ++c)
				{
					TArray<double> S;
					for (int32 i = 0; i < N; ++i) S.Add(Px[i * 3 + c]);
					S.Sort();
					double M;
					if (bMedian)
					{
						if (N % 2) M = S[N / 2];
						else M = Dt == EDt::F32 ? double((float(S[N / 2 - 1]) + float(S[N / 2])) / 2.f) : (S[N / 2 - 1] + S[N / 2]) / 2.0;
					}
					else M = Percentile(S, Num(Red, TEXT("percentile")), Dt);
					V.Add(NumOf(M, Dt));
				}
			}
			else if (Red->HasField(TEXT("bright")))
			{
				const TSharedPtr<FJsonObject>& B = Red->GetObjectField(TEXT("bright"));
				const TArray<double> Wt = JNums(B, TEXT("w"));
				TArray<double> Lum, S;
				for (int32 i = 0; i < N; ++i) Lum.Add(Px[i * 3] * Wt[0] + Px[i * 3 + 1] * Wt[1] + Px[i * 3 + 2] * Wt[2]);
				S = Lum;
				S.Sort();
				const double Q = Percentile(S, Num(B, TEXT("q")), EDt::F64);
				TArray<int32> Rows;
				for (int32 i = 0; i < N; ++i) if (Lum[i] > Q) Rows.Add(i);
				MeanOf(Rows);
			}
			else if (Red->HasField(TEXT("band")))
			{
				const TSharedPtr<FJsonObject>& B = Red->GetObjectField(TEXT("band"));
				TArray<double> Lum, S;
				for (int32 i = 0; i < N; ++i) Lum.Add(RowMean(i));
				S = Lum;
				S.Sort();
				const double Lo = Percentile(S, Num(B, TEXT("lo")), Dt), Hi = Percentile(S, Num(B, TEXT("hi")), Dt);
				TArray<int32> Rows;
				for (int32 i = 0; i < N; ++i) if (Lum[i] > Lo && Lum[i] < Hi) Rows.Add(i);
				MeanOf(Rows);
			}
			else if (Red->HasField(TEXT("tone")))
			{
				const TSharedPtr<FJsonObject>& T = Red->GetObjectField(TEXT("tone"));
				const int32 Want = int32(Num(T, TEXT("n")));
				TArray<double> Lum, S, Dd;
				for (int32 i = 0; i < N; ++i) Lum.Add(RowMean(i));
				S = Lum;
				S.Sort();
				const double Q = Percentile(S, Num(T, TEXT("q")), Dt);
				for (int32 i = 0; i < N; ++i) Dd.Add(Dt == EDt::F32 ? double(FMath::Abs(float(Lum[i]) - float(Q))) : FMath::Abs(Lum[i] - Q));
				TArray<double> Ds = Dd;
				Ds.Sort();
				const double Cut = Ds[FMath::Clamp(Want - 1, 0, N - 1)];
				TArray<int32> Rows, Tied;
				for (int32 i = 0; i < N; ++i)
				{
					if (Dd[i] < Cut) Rows.Add(i);
					else if (Dd[i] == Cut) Tied.Add(i);
				}
				const int32 Need = Want - Rows.Num();
				TSet<FIntPoint> Ties;
				const FJsonArr* TieArr = nullptr;
				if (T->TryGetArrayField(TEXT("ties"), TieArr)) for (const TSharedPtr<FJsonValue>& Tv : *TieArr) { const TArray<double> P2 = JNums(Tv); if (P2.Num() == 2) Ties.Add(FIntPoint(int32(P2[0]), int32(P2[1]))); }
				TArray<int32> Pick;
				for (int32 i : Tied) if (Ties.Contains(At[i])) Pick.Add(i);
				if (Pick.Num() != Need) { Pick.Reset(); for (int32 j = 0; j < Need && j < Tied.Num(); ++j) Pick.Add(Tied[j]); }
				Rows.Append(Pick);
				Rows.Sort();
				MeanOf(Rows);
			}
			else return false;
		}
		const FJsonArr* Then = nullptr;
		if (Spec->TryGetArrayField(TEXT("then"), Then))
		{
			for (const TSharedPtr<FJsonValue>& St : *Then)
			{
				const FJsonArr& S = St->AsArray();
				const FString Op = S[0]->AsString();
				if (Op == TEXT("int")) for (FNum& Q : V) Q = { double(int64(Q.V)), EKind::PyInt };
				else if (Op == TEXT("mul"))
				{
					TArray<double> Kk = JNums(S[1]);
					if (Kk.Num() != 3) Kk.Init(S[1]->AsNumber(), 3);
					for (int32 c = 0; c < V.Num(); ++c) V[c] = MulK(V[c], Kk[FMath::Min(c, 2)]);
				}
				else if (Op == TEXT("min")) { const FNum L = Lit(S[1]->AsNumber()); for (FNum& Q : V) if (!(Q.V < L.V)) Q = L; }
				else if (Op == TEXT("max")) { const FNum L = Lit(S[1]->AsNumber()); for (FNum& Q : V) if (!(Q.V > L.V)) Q = L; }
				else if (Op == TEXT("mix"))
				{
					const double A = S[1]->AsNumber(), B = S[3]->AsNumber();
					const TArray<double> G = JNums(S[2]);
					for (int32 c = 0; c < V.Num(); ++c) V[c] = AddK(MulK(V[c], A), B * G[c]);
				}
			}
		}
		return V.Num() > 0;
	}

	// ------------------------------------------------------------------ the evaluator (item_blocks.run)
	struct FPaint
	{
		AOutlawsLevelActor* L = nullptr;
		TMap<FString, FArr> Sprites;         // by stem: the player's HD sprite, RGBA
		TMap<FString, FIntPoint> Want;       // the base's sprite as the generator had it: its size (x -1: the first cell, y rows high)
		TMap<FString, int32> AtlasCell;      // the recipe's atlas_cell: the cell traced when it is not the first view's (the cannon)
		FString Err;

		const FArr* Sprite(const FString& Stem)
		{
			if (const FArr* Hit = Sprites.Find(Stem)) return Hit;
			const FOutlawsAssets* Labs = L ? L->GetLabAssets() : nullptr;
			const TSharedPtr<OutlawsTextures::FSpriteAtlas> Atlas = Labs ? L->GetPickupAtlas(Stem + TEXT(".NWX")) : nullptr;
			TArray<FColor> Hd;
			int32 W = 0, H = 0;
			if (!Atlas || !OutlawsTextures::DecodeSpriteAtlasHdPixels(*Labs, *Atlas, Hd, W, H, true) || Hd.Num() != W * H || !W)
			{
				Err = FString::Printf(TEXT("no HD sprite %s.NWX"), *Stem);
				return nullptr;
			}
			// the atlas as far as the generator's sprite went: sprites_hd holds the whole atlas for some (the badge's and the
			// Gatling's eight views) and only the first cell for others (the cannon, the plants, the flags, the lantern)
			// (the first cell: the one the first view's first frame is drawn from, as the generator's sprite was)
			int32 Cw = W, Ch = H, X0 = 0, Y0 = 0;
			if (const FIntPoint* Wt = Want.Find(Stem))
			{
				const int32 CellW = Atlas->CellW * 4, CellH = Atlas->CellH * 4;
				const bool bOneCell = (Wt->X < 0 && Wt->Y == CellH) || (Wt->X == CellW && Wt->Y == CellH);
				if (bOneCell && (W > CellW || H > CellH))
				{
					const int32* Asked = AtlasCell.Find(Stem);
					const int32 First = Asked && *Asked < Atlas->Cols * Atlas->Rows ? *Asked : Atlas->Grid.Num() ? Atlas->Grid[0] : 0;
					X0 = (First % FMath::Max(1, Atlas->Cols)) * CellW;
					Y0 = (First / FMath::Max(1, Atlas->Cols)) * CellH;
					Cw = CellW;
					Ch = CellH;
				}
				else if (Wt->X > 0) { Cw = FMath::Min(W, Wt->X); Ch = FMath::Min(H, Wt->Y); }
			}
			Cw = FMath::Min(Cw, W - X0);
			Ch = FMath::Min(Ch, H - Y0);
			FArr A = MakeImage(Ch, Cw, 4);
			for (int32 y = 0; y < Ch; ++y)
				for (int32 x = 0; x < Cw; ++x)
				{
					const FColor& C = Hd[(Y0 + y) * W + X0 + x];
					double* D = &A.V[(y * Cw + x) * 4];
					D[0] = C.R;
					D[1] = C.G;
					D[2] = C.B;
					D[3] = C.A;
				}
			return &Sprites.Add(Stem, MoveTemp(A));
		}
	};

	bool MaskArg(const TSharedPtr<FJsonObject>& O, const TCHAR* K, const TMap<FString, FArr>& R, FArr& Out)
	{
		const TSharedPtr<FJsonValue> V = O->TryGetField(K);
		if (!V.IsValid()) return false;
		if (V->Type == EJson::Object) return Decode(V->AsObject(), Out);
		const FArr* A = R.Find(V->AsString());
		if (!A) return false;
		Out = *A;
		return true;
	}

	/** item_blocks.run: Img is the model's image so far (uint8 RGBA; null for a base program).  The result is R["out"]. */
	bool Run(const FJsonArr& Prog, const FArr* Img, FPaint& P, TMap<FString, FArr>& R, TMap<FString, FArr>& Exports)
	{
		for (const TSharedPtr<FJsonValue>& Ov : Prog)
		{
			const TSharedPtr<FJsonObject> O = Ov->AsObject();
			const FString K = Str(O, TEXT("op")), OutName = Str(O, TEXT("out"));
			auto Reg = [&](const TCHAR* Field) -> FArr* { return R.Find(Str(O, Field)); };
			auto Fail = [&](const TCHAR* Why) { P.Err = FString::Printf(TEXT("op %s: %s"), *K, Why); return false; };
			FArr* X = O->HasField(TEXT("r")) ? Reg(TEXT("r")) : nullptr;
			if (O->HasField(TEXT("r")) && !X && K != TEXT("export")) return Fail(TEXT("no register"));
			if (K == TEXT("src"))
			{
				FString Stem;
				const FArr* Base = Img;
				if (O->TryGetStringField(TEXT("stem"), Stem) && !Stem.IsEmpty()) Base = P.Sprite(Stem);
				if (!Base) return Fail(TEXT("no image"));
				const TArray<double> Rc = JNums(O, TEXT("rect"));
				R.Add(OutName, Rc.Num() == 4 ? Crop(*Base, int32(Rc[0]), int32(Rc[1]), int32(Rc[2]), int32(Rc[3])) : *Base);
			}
			else if (K == TEXT("new"))
			{
				const TArray<double> S = JNums(O, TEXT("size")), Fill = JNums(O, TEXT("fill"));
				FArr A = MakeImage(int32(S[1]), int32(S[0]), Fill.Num());
				for (int32 i = 0; i < A.V.Num(); ++i) A.V[i] = Fill[i % Fill.Num()];
				R.Add(OutName, MoveTemp(A));
			}
			else if (K == TEXT("opaque")) FillNearest(*X, true);
			else if (K == TEXT("resize"))
			{
				const TArray<double> S = JNums(O, TEXT("size"));
				if (Str(O, TEXT("filter")) != TEXT("bilinear")) return Fail(TEXT("only bilinear"));
				R.Add(OutName, ResizeBilinear(*X, int32(S[0]), int32(S[1])));
			}
			else if (K == TEXT("flip"))
			{
				const bool bX = Str(O, TEXT("axis")) == TEXT("x");
				const int32 W = X->W(), H = X->H(), C = Chans(*X);
				FArr A = *X;
				for (int32 y = 0; y < H; ++y)
					for (int32 x = 0; x < W; ++x)
						for (int32 c = 0; c < C; ++c) A.V[(y * W + x) * C + c] = X->V[((bX ? y : H - 1 - y) * W + (bX ? W - 1 - x : x)) * C + c];
				R.Add(OutName, MoveTemp(A));
			}
			else if (K == TEXT("paste"))
			{
				FArr* D = Reg(TEXT("dst"));
				const TArray<double> At = JNums(O, TEXT("at"));
				if (!D || At.Num() != 2) return Fail(TEXT("bad paste"));
				Paste(*D, *X, int32(At[0]), int32(At[1]));
			}
			else if (K == TEXT("alpha_mask"))
			{
				FArr M;
				if (!MaskArg(O, TEXT("mask"), R, M) || M.V.Num() != X->W() * X->H()) return Fail(TEXT("bad mask"));
				for (int32 i = 0; i < M.V.Num(); ++i) X->V[i * 4 + 3] = M.V[i] != 0.0 && X->V[i * 4 + 3] > 127 ? 255.0 : 0.0;
			}
			else if (K == TEXT("quad"))
			{
				const TArray<double> S = JNums(O, TEXT("size")), D = JNums(O, TEXT("data"));
				if (Str(O, TEXT("filter")) != TEXT("bicubic") || D.Num() != 8) return Fail(TEXT("only bicubic"));
				R.Add(OutName, QuadBicubic(*X, int32(S[0]), int32(S[1]), D));
			}
			else if (K == TEXT("gauss")) R.Add(OutName, GaussianBlur(*X, Num(O, TEXT("radius"))));
			else if (K == TEXT("rgb") || K == TEXT("rgba"))
			{
				const int32 W = X->W(), H = X->H(), C = Chans(*X);
				FArr A = MakeImage(H, W, K == TEXT("rgb") ? 3 : 4);
				for (int32 i = 0; i < W * H; ++i)
				{
					for (int32 c = 0; c < 3; ++c) A.V[i * Chans(A) + c] = X->V[i * C + c];
					if (Chans(A) == 4) A.V[i * 4 + 3] = 255.0;
				}
				R.Add(OutName, MoveTemp(A));
			}
			else if (K == TEXT("field"))
			{
				FArr F;
				const TSharedPtr<FJsonObject>* D = nullptr;
				if (!O->TryGetObjectField(TEXT("data"), D) || !Decode(*D, F)) return Fail(TEXT("bad field"));
				const double Div = Num(O, TEXT("div"));
				if (Div != 0.0) { F.Dt = EDt::F64; for (double& v : F.V) v = v / Div; }
				R.Add(OutName, MoveTemp(F));
			}
			else if (K == TEXT("const"))
			{
				FArr A = Make(EDt::F64, { 3 });
				const TArray<double> C = JNums(O, TEXT("own"));
				for (int32 c = 0; c < 3 && c < C.Num(); ++c) A.V[c] = C[c];
				R.Add(OutName, MoveTemp(A));
			}
			else if (K == TEXT("sample"))
			{
				TArray<FNum> C;
				const TSharedPtr<FJsonObject>* Spec = nullptr;
				if (!O->TryGetObjectField(TEXT("spec"), Spec) || !Colour(*Spec, R, Img, false, C)) return Fail(TEXT("bad sample"));
				const EDt Dt = ArrayType(C);
				FArr A = Make(Dt, { C.Num() });
				for (int32 c = 0; c < C.Num(); ++c) A.V[c] = Cast(Dt, C[c].V);
				R.Add(OutName, MoveTemp(A));
			}
			else if (K == TEXT("lin"))
			{
				const double A = Num(O, TEXT("a")), B = Num(O, TEXT("b"));
				const EDt Dt = WithPyFloat(X->Dt);
				FArr Y = *X;
				Y.Dt = Dt;
				for (double& v : Y.V) v = Dt == EDt::F32 ? double(float(A) + float(float(B) * float(v))) : A + B * v;
				R.Add(OutName, MoveTemp(Y));
			}
			else if (K == TEXT("mul") || K == TEXT("add") || K == TEXT("sub"))
			{
				const FArr* A = Reg(TEXT("a"));
				const FArr* B = Reg(TEXT("b"));
				if (!A || !B) return Fail(TEXT("no register"));
				FArr Y;
				const int32 Which = K == TEXT("mul") ? 0 : K == TEXT("add") ? 1 : 2;
				if (!Binary(*A, *B, Promote(A->Dt, B->Dt), [Which](double a, double b) { return Which == 0 ? a * b : Which == 1 ? a + b : a - b; }, Y)) return Fail(TEXT("shapes"));
				R.Add(OutName, MoveTemp(Y));
			}
			else if (K == TEXT("mul3"))
			{
				const FArr* F = Reg(TEXT("f"));
				if (!F) return Fail(TEXT("no field"));
				FArr Y = AsType(*X, EDt::F64);
				const int32 C = Chans(Y);
				FArr Rgb = Make(EDt::F64, { Y.H(), Y.W(), 3 });
				for (int32 i = 0; i < Y.H() * Y.W(); ++i) for (int32 c = 0; c < 3; ++c) Rgb.V[i * 3 + c] = Y.V[i * C + c];
				FArr Prod;
				if (!Binary(Rgb, *F, EDt::F64, [](double a, double b) { return a * b; }, Prod) || Prod.Shape != Rgb.Shape) return Fail(TEXT("shapes"));
				for (int32 i = 0; i < Y.H() * Y.W(); ++i) for (int32 c = 0; c < 3; ++c) Y.V[i * C + c] = Prod.V[i * 3 + c];
				R.Add(OutName, MoveTemp(Y));
			}
			else if (K == TEXT("chan"))
			{
				const int32 C = Chans(*X), Ci = int32(Num(O, TEXT("c")));
				const double Kk = Num(O, TEXT("k"));
				for (int32 i = 0; i < X->H() * X->W(); ++i) X->V[i * C + Ci] = Cast(X->Dt, X->Dt == EDt::F32 ? double(float(X->V[i * C + Ci]) * float(Kk)) : X->V[i * C + Ci] * Kk);
			}
			else if (K == TEXT("where"))
			{
				FArr M;
				const FArr* A = Reg(TEXT("a"));
				const FArr* B = Reg(TEXT("b"));
				if (!A || !B || !MaskArg(O, TEXT("mask"), R, M)) return Fail(TEXT("no register"));
				if (A->Shape.Num() == 3 && M.Shape.Num() == 2) M.Shape.Add(1);
				TArray<int32> S1, S;
				if (!Broadcast(M.Shape, A->Shape, S1) || !Broadcast(S1, B->Shape, S)) return Fail(TEXT("shapes"));
				int32 D[3], Sm[3], Sa[3], Sb[3];
				Dims3(S, D);
				BStrides(M.Shape, S, Sm);
				BStrides(A->Shape, S, Sa);
				BStrides(B->Shape, S, Sb);
				FArr Y = Make(Promote(A->Dt, B->Dt), S);
				int32 k = 0;
				for (int32 i = 0; i < D[0]; ++i)
					for (int32 j = 0; j < D[1]; ++j)
						for (int32 c = 0; c < D[2]; ++c, ++k)
							Y.V[k] = Cast(Y.Dt, M.V[i * Sm[0] + j * Sm[1] + c * Sm[2]] != 0.0 ? A->V[i * Sa[0] + j * Sa[1] + c * Sa[2]] : B->V[i * Sb[0] + j * Sb[1] + c * Sb[2]]);
				R.Add(OutName, MoveTemp(Y));
			}
			else if (K == TEXT("clip"))
			{
				const double Lo = Num(O, TEXT("lo")), Hi = Num(O, TEXT("hi"));
				FArr Y = *X;
				for (double& v : Y.V) v = Cast(Y.Dt, FMath::Min(FMath::Max(v, Lo), Hi));
				R.Add(OutName, MoveTemp(Y));
			}
			else if (K == TEXT("u8")) R.Add(OutName, AsType(*X, EDt::U8));
			else if (K == TEXT("astype"))
			{
				const FString D = Str(O, TEXT("dtype"));
				R.Add(OutName, AsType(*X, D == TEXT("f32") ? EDt::F32 : D == TEXT("u8") ? EDt::U8 : EDt::F64));
			}
			else if (K == TEXT("rep"))
			{
				const int32 N = int32(Num(O, TEXT("n"))), Axis = int32(Num(O, TEXT("axis")));
				FArr Y = *X;
				int32 D[3];
				Dims3(X->Shape, D);
				const int32 Ax = Axis + (3 - X->Shape.Num());
				Y.Shape[Axis] *= N;
				int32 Dy[3];
				Dims3(Y.Shape, Dy);
				Y.V.SetNumUninitialized(Dy[0] * Dy[1] * Dy[2]);
				int32 k = 0;
				for (int32 i = 0; i < Dy[0]; ++i)
					for (int32 j = 0; j < Dy[1]; ++j)
						for (int32 c = 0; c < Dy[2]; ++c)
						{
							int32 Src[3] = { i, j, c };
							Src[Ax] /= N;
							Y.V[k++] = X->V[(Src[0] * D[1] + Src[1]) * D[2] + Src[2]];
						}
				R.Add(OutName, MoveTemp(Y));
			}
			else if (K == TEXT("get") || K == TEXT("put"))
			{
				const int32 C = Chans(*X);
				int32 Y0, Y1, X0, X1, C0, C1;
				Slice(O, TEXT("rows"), X->H(), Y0, Y1);
				Slice(O, TEXT("cols"), X->W(), X0, X1);
				Slice(O, TEXT("ch"), C, C0, C1);
				const bool bCh = X->Shape.Num() == 3;
				FArr Part = Make(X->Dt, bCh ? TArray<int32>{ Y1 - Y0, X1 - X0, C1 - C0 } : TArray<int32>{ Y1 - Y0, X1 - X0 });
				if (K == TEXT("get"))
				{
					int32 k = 0;
					for (int32 y = Y0; y < Y1; ++y) for (int32 x = X0; x < X1; ++x) for (int32 c = C0; c < C1; ++c) Part.V[k++] = X->V[(y * X->W() + x) * C + c];
					R.Add(OutName, MoveTemp(Part));
				}
				else
				{
					const FArr* Vv = Reg(TEXT("v"));
					if (!Vv || !AssignInto(Part, *Vv)) return Fail(TEXT("shapes"));
					int32 k = 0;
					for (int32 y = Y0; y < Y1; ++y) for (int32 x = X0; x < X1; ++x) for (int32 c = C0; c < C1; ++c) X->V[(y * X->W() + x) * C + c] = Part.V[k++];
				}
			}
			else if (K == TEXT("set"))
			{
				FArr M;
				if (!MaskArg(O, TEXT("mask"), R, M) || M.V.Num() != X->H() * X->W()) return Fail(TEXT("bad mask"));
				TArray<double> Vv = JNums(O, TEXT("own"));
				if (!O->HasField(TEXT("own")))
				{
					const FArr* A = Reg(TEXT("v"));
					if (!A) return Fail(TEXT("no colour"));
					Vv = A->V;
				}
				const int32 C = Chans(*X);
				for (int32 i = 0; i < M.V.Num(); ++i) if (M.V[i] != 0.0) for (int32 c = 0; c < Vv.Num() && c < C; ++c) X->V[i * C + c] = Cast(X->Dt, Vv[c]);
			}
			else if (K == TEXT("ink"))
			{
				FArr M;
				const TSharedPtr<FJsonObject>* D = nullptr;
				if (!O->TryGetObjectField(TEXT("mask"), D) || !Decode(*D, M) || M.V.Num() != X->H() * X->W()) return Fail(TEXT("bad mask"));
				const TArray<double> Own = JNums(O, TEXT("own"));
				const int32 C = Chans(*X);
				for (int32 i = 0; i < M.V.Num(); ++i)
				{
					const int64 m = int64(M.V[i]);
					for (int32 c = 0; c < 3; ++c)
					{
						const int64 T = int64(X->V[i * C + c]) * (255 - m) + int64(Own[c]) * m + 128;
						X->V[i * C + c] = double(uint8(((T >> 8) + T) >> 8));
					}
				}
			}
			else if (K == TEXT("recolour"))
			{
				TArray<const FArr*> Pal;
				for (const TSharedPtr<FJsonValue>& Pv : O->GetArrayField(TEXT("pal"))) { const FArr* A = R.Find(Pv->AsString()); if (!A) return Fail(TEXT("no palette")); Pal.Add(A); }
				const int32 N = Pal.Num(), C = Chans(*X);
				FArr Y = MakeImage(X->H(), X->W(), 3);
				for (int32 i = 0; i < X->H() * X->W(); ++i)
				{
					const double Lm = ((X->V[i * C] + X->V[i * C + 1]) + X->V[i * C + 2]) / 3.0 / 255.0;
					const double Xx = FMath::Clamp(Lm * (N - 1), 0.0, N - 1 - 1e-6);
					const int32 Ii = int32(FMath::FloorToDouble(Xx));
					const double F = Xx - Ii;
					for (int32 c = 0; c < 3; ++c) Y.V[i * 3 + c] = Cast(EDt::U8, FMath::Clamp(Pal[Ii]->V[c] * (1 - F) + Pal[Ii + 1]->V[c] * F, 0.0, 255.0));
				}
				R.Add(OutName, MoveTemp(Y));
			}
			else if (K == TEXT("pred"))
			{
				const FJsonArr& Terms = O->GetArrayField(TEXT("where"));
				const int32 C = Chans(*X);
				FArr M = Make(EDt::Bool, { X->H(), X->W() });
				for (int32 i = 0; i < X->H() * X->W(); ++i)
				{
					double Q[4] = { 0, 0, 0, 255 };
					for (int32 c = 0; c < C && c < 4; ++c) Q[c] = X->V[i * C + c];
					M.V[i] = Predicate(Terms, Q, X->Dt) ? 1.0 : 0.0;
				}
				R.Add(OutName, MoveTemp(M));
			}
			else if (K == TEXT("box"))
			{
				const TArray<double> Rc = JNums(O, TEXT("rect"));
				FArr M = Make(EDt::Bool, { X->H(), X->W() });
				for (int32 y = FMath::Max(0, int32(Rc[1])); y < FMath::Min(X->H(), int32(Rc[3])); ++y)
					for (int32 x = FMath::Max(0, int32(Rc[0])); x < FMath::Min(X->W(), int32(Rc[2])); ++x) M.V[y * X->W() + x] = 1.0;
				R.Add(OutName, MoveTemp(M));
			}
			else if (K == TEXT("and") || K == TEXT("andnot") || K == TEXT("or"))
			{
				const FArr* A = Reg(TEXT("a"));
				const FArr* B = Reg(TEXT("b"));
				if (!A || !B || A->V.Num() != B->V.Num()) return Fail(TEXT("no register"));
				FArr M = *A;
				for (int32 i = 0; i < M.V.Num(); ++i)
				{
					const bool a = A->V[i] != 0.0, b = B->V[i] != 0.0;
					M.V[i] = (K == TEXT("and") ? a && b : K == TEXT("andnot") ? a && !b : a || b) ? 1.0 : 0.0;
				}
				R.Add(OutName, MoveTemp(M));
			}
			else if (K == TEXT("close") || K == TEXT("dilate"))
			{
				const int32 It = int32(Num(O, TEXT("iterations"), 1.0));
				TArray<uint8> M = Morph(MaskOf(*X), X->W(), X->H(), It, false);
				if (K == TEXT("close")) M = Morph(MoveTemp(M), X->W(), X->H(), It, true);
				R.Add(OutName, FromMask(M, X->H(), X->W()));
			}
			else if (K == TEXT("fill_holes")) R.Add(OutName, FromMask(FillHoles(MaskOf(*X), X->W(), X->H()), X->H(), X->W()));
			else if (K == TEXT("largest")) R.Add(OutName, FromMask(Largest(MaskOf(*X), X->W(), X->H()), X->H(), X->W()));
			else if (K == TEXT("median_at"))
			{
				const FArr* M = Reg(TEXT("mask"));
				if (!M) return Fail(TEXT("no mask"));
				const int32 C = Chans(*X);
				FArr Y = Make(X->Dt, { 3 });
				TArray<double> S[3];
				for (int32 i = 0; i < M->V.Num(); ++i) if (M->V[i] != 0.0) for (int32 c = 0; c < 3; ++c) S[c].Add(X->V[i * C + c]);
				const TArray<double> Else = JNums(O, TEXT("else"));
				for (int32 c = 0; c < 3; ++c)
				{
					const int32 N = S[c].Num();
					if (!N) { Y.V[c] = Cast(X->Dt, Else[c]); continue; }
					S[c].Sort();
					Y.V[c] = N % 2 ? S[c][N / 2] : X->Dt == EDt::F32 ? double((float(S[c][N / 2 - 1]) + float(S[c][N / 2])) / 2.f) : (S[c][N / 2 - 1] + S[c][N / 2]) / 2.0;
				}
				R.Add(OutName, MoveTemp(Y));
			}
			else if (K == TEXT("inpaint"))
			{
				const FArr* M = Reg(TEXT("mask"));
				const FArr* Vv = Reg(TEXT("v"));
				if (!M || !Vv) return Fail(TEXT("no register"));
				const int32 C = Chans(*X), N = X->H() * X->W();
				for (int32 i = 0; i < N; ++i) if (M->V[i] != 0.0) for (int32 c = 0; c < C; ++c) X->V[i * C + c] = Cast(X->Dt, Vv->V[FMath::Min(c, Vv->V.Num() - 1)]);
				const TArray<double> Size = JNums(O, TEXT("size"));
				for (int32 it = 0; it < int32(Num(O, TEXT("iters"))); ++it)
				{
					FArr B = *X;
					for (int32 a = 0; a < Size.Num() && a < X->Shape.Num(); ++a) if (Size[a] > 1) Uniform1d(B, a + (3 - X->Shape.Num()), int32(Size[a]));
					for (int32 i = 0; i < N; ++i) if (M->V[i] != 0.0) for (int32 c = 0; c < C; ++c) X->V[i * C + c] = B.V[i * C + c];
				}
			}
			else if (K == TEXT("sel"))
			{
				FArr M;
				if (!MaskArg(O, TEXT("mask"), R, M)) return Fail(TEXT("no mask"));
				const double Hi = Num(O, TEXT("hi")), Lo = Num(O, TEXT("lo"));
				for (double& v : M.V) v = Cast(EDt::U8, v != 0.0 ? Hi : Lo);
				M.Dt = EDt::U8;
				bool bAxis = false;
				if (O->TryGetBoolField(TEXT("axis"), bAxis) && bAxis) M.Shape.Add(1);
				R.Add(OutName, MoveTemp(M));
			}
			else if (K == TEXT("zeros"))
			{
				TArray<int32> S;
				for (double D : JNums(O, TEXT("shape"))) S.Add(int32(D));
				const FString D = Str(O, TEXT("dtype"));
				R.Add(OutName, Make(D == TEXT("u8") ? EDt::U8 : D == TEXT("f32") ? EDt::F32 : EDt::F64, S));
			}
			else if (K == TEXT("export"))
			{
				const FArr* A = Reg(TEXT("r"));
				if (!A) return Fail(TEXT("no register"));
				Exports.Add(Str(O, TEXT("key")), *A);
			}
			else return Fail(TEXT("unknown"));
		}
		return true;
	}

	// ------------------------------------------------------------------ the recipe
	TSharedPtr<FJsonObject> LoadJson(const FString& Path)
	{
		FString Text;
		TSharedPtr<FJsonObject> J;
		if (!FFileHelper::LoadFileToString(Text, *Path) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), J) || !J.IsValid()) return nullptr;
		return J;
	}

	/** make_item_ship.grounded_base: the sprite mirrored Mirror x its height under its bottom row, 2 rows of padding, the
	 *  ink outline stripped. */
	FArr GroundedBase(const FArr& Raw, double Mirror, int32 Ink)
	{
		const int32 W = Raw.W(), H = Raw.H();
		int32 Top = -1, Bot = -1;
		for (int32 y = 0; y < H; ++y)
		{
			bool bAny = false;
			for (int32 x = 0; x < W && !bAny; ++x) bAny = Raw.V[(y * W + x) * 4 + 3] > 127;
			if (bAny) { if (Top < 0) Top = y; Bot = y + 1; }
		}
		if (Top < 0) return Raw;
		const int32 Body = Bot - Top;
		const int32 N = FMath::Max(2, int32(Body * Mirror));
		const int32 Ext = Body + FMath::Min(N, Body);
		FArr Pad = MakeImage(Ext + 4, W, 4);
		for (int32 r = 0; r < Ext; ++r)
		{
			const int32 SrcY = r < Body ? Top + r : Bot - 1 - (r - Body);
			for (int32 i = 0; i < W * 4; ++i) Pad.V[(r + 2) * W * 4 + i] = Raw.V[SrcY * W * 4 + i];
		}
		StripOutline(Pad, Ink);
		return Pad;
	}

	/** The whole texture of a recipe (make_item_ship's: base, blocks, the fill, the swatches), RGBA uint8. */
	bool PaintRecipe(const TSharedPtr<FJsonObject>& J, FPaint& P, FArr& Tex)
	{
		const TArray<double> Size = JNums(J, TEXT("size"));
		if (Size.Num() != 2) { P.Err = TEXT("no size"); return false; }
		const int32 W = int32(Size[0]), H = int32(Size[1]);
		Tex = MakeImage(H, W, 4);
		const TSharedPtr<FJsonObject> B = J->GetObjectField(TEXT("base"));
		const FString Op = Str(B, TEXT("op"));
		{
			const TArray<double> Sz = JNums(B, TEXT("size")), Rows = JNums(B, TEXT("rows"));
			if (Op != TEXT("program") && Sz.Num() == 2) P.Want.Add(Str(B, TEXT("stem")), FIntPoint(int32(Sz[0]), int32(Sz[1])));
			else if (Op == TEXT("program") && Rows.Num() == 2) P.Want.Add(Str(B, TEXT("stem")), FIntPoint(-1, int32(Rows[1])));
			double Cell = 0.0;
			if (B->TryGetNumberField(TEXT("atlas_cell"), Cell)) P.AtlasCell.Add(Str(B, TEXT("stem")), int32(Cell));
		}
		FArr Base;
		bool bBase = true;
		if (Op == TEXT("sprite") || Op == TEXT("strip_ink") || Op == TEXT("grounded"))
		{
			const FArr* S = P.Sprite(Str(B, TEXT("stem")));
			if (!S) return false;
			Base = *S;
			const TArray<double> Cell = JNums(B, TEXT("cell"));
			if (Op == TEXT("sprite") && Cell.Num() == 4) Base = Crop(Base, int32(Cell[0]), int32(Cell[1]), int32(Cell[2]), int32(Cell[3]));
			if (Op == TEXT("strip_ink")) StripOutline(Base, int32(Num(B, TEXT("ink"), 4.0)));
			if (Op == TEXT("grounded")) Base = GroundedBase(Base, Num(B, TEXT("mirror"), 1.0), int32(Num(B, TEXT("ink"), 4.0)));
		}
		else if (Op == TEXT("program"))
		{
			TMap<FString, FArr> R, Ex;
			if (!Run(B->GetArrayField(TEXT("prog")), nullptr, P, R, Ex) || !R.Contains(TEXT("out"))) return false;
			Base = R[TEXT("out")];
		}
		else if (Op == TEXT("none")) bBase = false;
		else { P.Err = TEXT("base ") + Op; return false; }
		if (bBase)
		{
			for (int32 y = 0; y < FMath::Min(H, Base.H()); ++y)
				for (int32 x = 0; x < FMath::Min(W, Base.W()); ++x)
					for (int32 c = 0; c < 4; ++c) Tex.V[(y * W + x) * 4 + c] = Base.V[(y * Base.W() + x) * Chans(Base) + FMath::Min(c, Chans(Base) - 1)];
		}
		TMap<FString, FArr> Exports;
		for (const TSharedPtr<FJsonValue>& Bv : J->GetArrayField(TEXT("blocks")))
		{
			const TSharedPtr<FJsonObject> Blk = Bv->AsObject();
			const TArray<double> Rc = JNums(Blk, TEXT("rect"));
			const FJsonArr* Prog = nullptr;
			if (Rc.Num() != 4 || !Blk->TryGetArrayField(TEXT("prog"), Prog)) { P.Err = TEXT("a block with no program"); return false; }
			TMap<FString, FArr> R;
			const FArr Copy = Tex;
			if (!Run(*Prog, &Copy, P, R, Exports)) return false;
			const FArr* Out = R.Find(TEXT("out"));
			const int32 Bx = int32(Rc[0]), By = int32(Rc[1]), Bw = int32(Rc[2]), Bh = int32(Rc[3]);
			if (!Out || Out->W() != Bw || Out->H() != Bh) { P.Err = TEXT("a block's size"); return false; }
			const int32 Co = Chans(*Out);
			for (int32 y = 0; y < Bh; ++y)
				for (int32 x = 0; x < Bw; ++x)
					for (int32 c = 0; c < 4; ++c)
						if (By + y < H && Bx + x < W) Tex.V[((By + y) * W + Bx + x) * 4 + c] = Cast(EDt::U8, c < Co ? Out->V[(y * Bw + x) * Co + c] : 255.0);
		}
		// the swatches read the image as rebuilt (the rows the fill covers, as float32), before the fill
		const TSharedPtr<FJsonObject>* Fill = nullptr;
		int32 F0 = 0, F1 = H;
		const bool bFill = J->TryGetObjectField(TEXT("fill"), Fill);
		if (bFill)
		{
			const TArray<double> Rows = JNums(*Fill, TEXT("rows"));
			F0 = int32(Rows[0]);
			F1 = FMath::Min(H, int32(Rows[1]));
		}
		FArr Img = MakeImage(F1, W, 4);
		for (int32 i = 0; i < F1 * W * 4; ++i) Img.V[i] = Tex.V[i];
		Img.Dt = EDt::F32;
		if (bFill)
		{
			FArr Part = MakeImage(F1 - F0, W, 4);
			for (int32 i = 0; i < Part.V.Num(); ++i) Part.V[i] = Tex.V[F0 * W * 4 + i];
			FillNearest(Part, false);
			for (int32 i = 0; i < Part.V.Num(); ++i) Tex.V[F0 * W * 4 + i] = Part.V[i];
		}
		for (const TSharedPtr<FJsonValue>& Sv : J->GetArrayField(TEXT("swatches")))
		{
			const TSharedPtr<FJsonObject> S = Sv->AsObject();
			const TArray<double> Rc = JNums(S, TEXT("rect"));
			TArray<double> C = JNums(S, TEXT("own"));
			if (!S->HasField(TEXT("own")))
			{
				const TSharedPtr<FJsonObject>* Spec = nullptr;
				TArray<FNum> Cn;
				if (!S->TryGetObjectField(TEXT("sample"), Spec) || !Colour(*Spec, Exports, &Img, true, Cn)) { P.Err = TEXT("a swatch with no sample"); return false; }
				C.Reset();
				for (const FNum& N : Cn) C.Add(double(int64(N.V)));
			}
			const int32 X0 = int32(Rc[0]), Y0 = int32(Rc[1]), Sw = int32(Rc[2]);
			for (int32 y = Y0; y < FMath::Min(H, Y0 + Sw); ++y)
				for (int32 x = X0; x < FMath::Min(W, X0 + Sw); ++x)
				{
					for (int32 c = 0; c < 3; ++c) Tex.V[(y * W + x) * 4 + c] = Cast(EDt::U8, C[c]);
					Tex.V[(y * W + x) * 4 + 3] = 255.0;
				}
		}
		return true;
	}

	// ------------------------------------------------------------------ textures and models
	TMap<FString, TWeakObjectPtr<UTexture2D>> GTextures;      // by section key
	TWeakObjectPtr<AOutlawsLevelActor> GLevel;
	constexpr const TCHAR* Of3do = TEXT(".3do:");             // a from_3do model's section key: <dir>/<NAME>.3do:<the 3DO's texture>

	FString RecipeOf(const FString& Key)
	{
		return FPaths::Combine(FPaths::GetPath(Key), FPaths::GetBaseFilename(Key) + TEXT("_tex.json"));
	}

	bool InShipped(const FString& Path)
	{
		const FString Dir = FPaths::GetPath(Path);
		return FPaths::IsSamePath(Dir, OutlawsItem3d::ShippedDir()) || FPaths::IsSamePath(Dir, FPaths::Combine(OutlawsItem3d::ShippedDir(), TEXT("held")));
	}

	/** The dev PC's texture of a model, for the parity line (read straight, not a placeholder use). */
	bool DevTexture(const FString& RecipePath, TArray<FColor>& Out, int32& W, int32& H)
	{
		const FString Dev = FOutlawsAssets::DevPlaceholderDir();
		if (Dev.IsEmpty()) return false;
		const bool bHeld = FPaths::GetCleanFilename(FPaths::GetPath(RecipePath)) == TEXT("held");
		const FString Name = FPaths::GetBaseFilename(RecipePath).LeftChop(4);
		const FString Png = FPaths::Combine(Dev, bHeld ? TEXT("items3d/held") : TEXT("items3d"), Name + TEXT(".png"));
		FImage Img;
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Png, FILEREAD_Silent) || !FImageUtils::DecompressImage(Bytes.GetData(), Bytes.Num(), Img)) return false;
		Img.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		const TArrayView64<const FColor> Px = static_cast<const FImage&>(Img).AsBGRA8();
		Out = TArray<FColor>(Px.GetData(), int32(Px.Num()));
		W = Img.SizeX;
		H = Img.SizeY;
		return true;
	}

	/** TABLE02, the stool (props.from_table01): the Remaster's TABLE01 3DO, across to the sprite's opaque width and up
	 *  to its height (the sidecar's real_size_cm), stood on its base, its own textures. */
	bool LoadFrom3do(const FString& Path, const TSharedPtr<FJsonObject>& Recipe, OutlawsObj::FObjMesh& Out, AOutlawsLevelActor* L)
	{
		const TSharedPtr<FJsonObject>* From = nullptr;
		if (!L || !Recipe->TryGetObjectField(TEXT("from_3do"), From)) return false;
		const TSharedPtr<FJsonObject> Side = LoadJson(FPaths::ChangeExtension(Path, TEXT("json")));
		const TArray<double> Real = Side.IsValid() ? JNums(Side, TEXT("real_size_cm")) : TArray<double>();
		TMap<FString, FOutlawsMeshSection> Src;
		TMap<FString, UTexture2D*> Tex;
		if (Real.Num() != 3 || !L->BuildItemModel(Str(*From, TEXT("mesh")) + TEXT(".3DO"), Src, Tex)) return false;
		FBox B(ForceInit);
		for (const TPair<FString, FOutlawsMeshSection>& Kv : Src) for (const FVector& V : Kv.Value.Positions) B += V;
		if (!B.IsValid) return false;
		const FVector E = B.GetSize();
		const double Kxy = Real[0] / FMath::Max3(E.X, E.Y, 1e-6), Kz = Real[2] / FMath::Max(E.Z, 1e-6);
		const FVector C = B.GetCenter();
		Out = OutlawsObj::FObjMesh();
		const FString Stem = FPaths::Combine(FPaths::GetPath(Path), FPaths::GetBaseFilename(Path)) + Of3do;
		for (TPair<FString, FOutlawsMeshSection>& Kv : Src)
		{
			FOutlawsMeshSection S = Kv.Value;
			for (FVector& V : S.Positions)
			{
				V = FVector((V.X - C.X) * Kxy, (V.Y - C.Y) * Kxy, (V.Z - B.Min.Z) * Kz);
				Out.Bounds += V;
			}
			for (FVector& N : S.Normals) N = FVector(N.X / Kxy, N.Y / Kxy, N.Z / Kz).GetSafeNormal();
			if (UTexture2D* const* T = Tex.Find(S.Texture)) GTextures.Add(Stem + S.Texture, *T);
			S.Texture = Stem + S.Texture;
			Out.Sections.Add(Kv.Key, MoveTemp(S));
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLRT item3d %s: the Remaster's %s scaled to %.1f x %.1f cm, %d sections"), *FPaths::GetBaseFilename(Path), *Str(*From, TEXT("mesh")), Real[0], Real[2], Out.Sections.Num());
		return Out.Sections.Num() > 0;
	}
}

// (REQ-NATIVEMAPS) the native map overlay changed: the textures painted from the LABs' sprites let go (painted again from the
// next map's files at their next use)
int32 OlForgetItem3dTextures()
{
	const int32 N = GTextures.Num();
	GTextures.Empty();
	return N;
}

bool OutlawsItem3d::Paint(AOutlawsLevelActor* L, const FString& RecipePath, TArray<FColor>& Out, int32& OutW, int32& OutH, FString& OutError)
{
	const TSharedPtr<FJsonObject> J = LoadJson(RecipePath);
	if (!J.IsValid()) { OutError = TEXT("no recipe"); return false; }
	FPaint P;
	P.L = L;
	FArr Tex;
	if (!PaintRecipe(J, P, Tex)) { OutError = P.Err; return false; }
	OutW = Tex.W();
	OutH = Tex.H();
	Out.SetNumUninitialized(OutW * OutH);
	for (int32 i = 0; i < OutW * OutH; ++i) Out[i] = FColor(uint8(Tex.V[i * 4]), uint8(Tex.V[i * 4 + 1]), uint8(Tex.V[i * 4 + 2]), uint8(Tex.V[i * 4 + 3]));
	return true;
}

void OutlawsItem3d::ResizeLanczos(const TArray<FColor>& In, int32 W, int32 H, int32 OW, int32 OH, TArray<FColor>& Out)
{
	FArr A = MakeImage(H, W, 4);
	for (int32 i = 0; i < W * H; ++i) { A.V[i * 4] = In[i].R; A.V[i * 4 + 1] = In[i].G; A.V[i * 4 + 2] = In[i].B; A.V[i * 4 + 3] = In[i].A; }
	const FArr R = ResizeBilinear(A, OW, OH, true);
	Out.SetNumUninitialized(OW * OH);
	for (int32 i = 0; i < OW * OH; ++i) Out[i] = FColor(uint8(R.V[i * 4]), uint8(R.V[i * 4 + 1]), uint8(R.V[i * 4 + 2]), uint8(R.V[i * 4 + 3]));
}

bool OutlawsItem3d::UseShipped()
{
	static const bool bShipped = []()
	{
		if (OLFlags::Param(TEXT("OLOldDevDir2"))) return false;
		if (OLFlags::Param(TEXT("OLShippedItems"))) return true;
		return !FPaths::DirectoryExists(FPaths::Combine(FOutlawsAssets::PlaceholderDir(), TEXT("items3d")));
	}();
	return bShipped;
}

FString OutlawsItem3d::ShippedDir() { return FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Item3d")); }

FString OutlawsItem3d::Dir() { return UseShipped() ? ShippedDir() : FPaths::Combine(FOutlawsAssets::PlaceholderDir(), TEXT("items3d")); }

FString OutlawsItem3d::HeldDir() { return FPaths::Combine(Dir(), TEXT("held")); }

void OutlawsItem3d::SetLevel(AOutlawsLevelActor* L) { if (L) GLevel = L; }

bool OutlawsItem3d::CanPaint() { return !UseShipped() || GLevel.IsValid(); }

bool OutlawsItem3d::Exists(const FString& Path)
{
	if (FPaths::FileExists(Path)) return true;
	if (!InShipped(Path)) return false;
	const TSharedPtr<FJsonObject> J = LoadJson(RecipeOf(Path));
	return J.IsValid() && J->HasField(TEXT("from_3do"));
}

bool OutlawsItem3d::Load(const FString& Path, double Scale, bool bRightHanded, OutlawsObj::FObjMesh& Out, AOutlawsLevelActor* L)
{
	SetLevel(L);
	if (!InShipped(Path)) return FPaths::FileExists(Path) && OutlawsObj::Load(Path, Scale, bRightHanded, Out);
	const TSharedPtr<FJsonObject> J = LoadJson(RecipeOf(Path));
	if (!J.IsValid()) return false;
	if (J->HasField(TEXT("from_3do"))) return LoadFrom3do(Path, J, Out, GLevel.Get());
	if (!FPaths::FileExists(Path) || !OutlawsObj::Load(Path, Scale, bRightHanded, Out)) return false;
	const FString Key = FPaths::ChangeExtension(Path, TEXT("png"));
	for (TPair<FString, FOutlawsMeshSection>& Kv : Out.Sections) Kv.Value.Texture = Key;
	// painted now, so a model whose sprite this install lacks is no model (the caller's fallback, the flat sprite)
	return Texture(Key, L) != nullptr;
}

UTexture2D* OutlawsItem3d::Texture(const FString& Key, AOutlawsLevelActor* L)
{
	SetLevel(L);
	if (const TWeakObjectPtr<UTexture2D>* Hit = GTextures.Find(Key))
	{
		if (Hit->IsValid()) return Hit->Get();
	}
	if (!InShipped(Key)) return OutlawsObj::LoadTextureFile(Key);
	AOutlawsLevelActor* Lv = GLevel.Get();
	const int32 At3do = Key.Find(Of3do);
	if (At3do != INDEX_NONE)
	{
		// a from_3do model's texture collected with its level: the model built again from this one
		OutlawsObj::FObjMesh M;
		LoadFrom3do(Key.Left(At3do) + TEXT(".obj"), LoadJson(Key.Left(At3do) + TEXT("_tex.json")), M, Lv);
		const TWeakObjectPtr<UTexture2D>* Again = GTextures.Find(Key);
		return Again && Again->IsValid() ? Again->Get() : nullptr;
	}
	const FString RecipePath = RecipeOf(Key);
	const double T0 = FPlatformTime::Seconds();
	TArray<FColor> Px;
	int32 W = 0, H = 0;
	FString Err;
	if (!Paint(Lv, RecipePath, Px, W, H, Err))
	{
		UE_LOG(LogOutlaws, Warning, TEXT("OLRT item3d %s: not painted (%s)"), *FPaths::GetBaseFilename(Key), *Err);
		return nullptr;
	}
	const double Ms = (FPlatformTime::Seconds() - T0) * 1000.0;
	UTexture2D* T = OutlawsTextures::MakeTextureBGRA(W, H, Px, false, TF_Nearest, TA_Wrap);      // as the loaded PNG was
	if (!T) return nullptr;
	OutlawsObj::NoteSolidTexels(T, W, H, Px);
	GTextures.Add(Key, T);
	// this PC: against the dev texture it was made from (texels more than 2 apart in a channel)
	TArray<FColor> Dev;
	int32 Dw = 0, Dh = 0;
	if (DevTexture(RecipePath, Dev, Dw, Dh))
	{
		int32 Differ = 0, MaxD = 0;
		if (Dw == W && Dh == H)
		{
			for (int32 i = 0; i < Px.Num(); ++i)
			{
				const int32 D = FMath::Max(FMath::Max(FMath::Abs(Px[i].R - Dev[i].R), FMath::Abs(Px[i].G - Dev[i].G)), FMath::Max(FMath::Abs(Px[i].B - Dev[i].B), FMath::Abs(Px[i].A - Dev[i].A)));
				Differ += D > 2 ? 1 : 0;
				MaxD = FMath::Max(MaxD, D);
			}
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLRT item3d %s: painted %dx%d in %.1f ms, against the dev texture %dx%d: %d of %d texels differ by more than 2 (max %d)"),
			*FPaths::GetBaseFilename(Key), W, H, Ms, Dw, Dh, Differ, Px.Num(), MaxD);
	}
	else
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT item3d %s: painted %dx%d from the player's sprite in %.1f ms"), *FPaths::GetBaseFilename(Key), W, H, Ms);
	}
	return T;
}
