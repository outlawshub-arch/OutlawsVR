#include "OutlawsObjMesh.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsLab.h"
#include "Algo/Sort.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "ImageUtils.h"
#include "Engine/Texture2D.h"
#include "ImageCore.h"
#include <cstdlib>

namespace
{
	int32 ResolveIndex(long I, int32 Count)
	{
		return I > 0 ? int32(I) - 1 : Count + int32(I);          // OBJ indices are 1-based, negatives count back
	}

	bool IsSpace(ANSICHAR C) { return C == ' ' || C == '\t'; }

	/** The line's first word is Word (and a space follows it). */
	bool Starts(const ANSICHAR* L, const ANSICHAR* Word)
	{
		const int32 N = FCStringAnsi::Strlen(Word);
		return FCStringAnsi::Strncmp(L, Word, N) == 0 && IsSpace(L[N]);
	}

	/** The word after the line's keyword, up to the next space. */
	FString SecondWord(const ANSICHAR* L)
	{
		while (*L && !IsSpace(*L)) ++L;
		while (IsSpace(*L)) ++L;
		const ANSICHAR* B = L;
		while (*L && !IsSpace(*L)) ++L;
		return FString(int32(L - B), B);
	}
}

// Read as bytes and parsed a line at a time in place: the old way (a string per line, per word and per face corner)
// took up to a second a model on the Quest, and a level start loads dozens (townex: 11.6 s).
bool OutlawsObj::Load(const FString& Path, double Scale, bool bRightHanded, FObjMesh& Out)
{
	Out = FObjMesh();
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path))
	{
		UE_LOG(LogOutlaws, Warning, TEXT("OLRT OBJ: cannot read %s"), *Path);
		return false;
	}
	FOutlawsAssets::NoteDevPlaceholder(Path);      // (BUG-DEVDIR: named in the log when testers never get it)
	const FString Dir = FPaths::GetPath(Path);
	TMap<FString, FString> MaterialTexture;
	TArray<FVector> V;
	TArray<FVector2D> VT;
	FString Material = TEXT("default");
	TArray<ANSICHAR, TInlineAllocator<512>> Line;
	const ANSICHAR* P = reinterpret_cast<const ANSICHAR*>(Bytes.GetData());
	const ANSICHAR* End = P + Bytes.Num();
	while (P < End)
	{
		const ANSICHAR* E = P;
		while (E < End && *E != '\n') ++E;
		const ANSICHAR* B = P;
		P = E + 1;
		while (B < E && IsSpace(*B)) ++B;
		const ANSICHAR* LEnd = E;
		while (LEnd > B && (LEnd[-1] == '\r' || IsSpace(LEnd[-1]))) --LEnd;
		if (LEnd <= B) continue;
		// a terminated copy of the line, so the number parsing cannot run on into the next one
		Line.SetNumUninitialized(int32(LEnd - B) + 1);
		FMemory::Memcpy(Line.GetData(), B, LEnd - B);
		Line[Line.Num() - 1] = 0;
		const ANSICHAR* L = Line.GetData();
		if (Starts(L, "v"))
		{
			ANSICHAR* Q = const_cast<ANSICHAR*>(L + 2);
			double C[3];
			int32 Got = 0;
			for (; Got < 3; ++Got)
			{
				ANSICHAR* Next = nullptr;
				C[Got] = strtod(Q, &Next);
				if (Next == Q) break;
				Q = Next;
			}
			// right-handed Z-up (RenderWare) to Unreal: mirror Y
			if (Got == 3) V.Add(FVector(C[0], bRightHanded ? -C[1] : C[1], C[2]) * Scale);
		}
		else if (Starts(L, "vt"))
		{
			ANSICHAR* Q = const_cast<ANSICHAR*>(L + 3);
			ANSICHAR* N1 = nullptr;
			ANSICHAR* N2 = nullptr;
			const double U = strtod(Q, &N1);
			const double W = strtod(N1, &N2);
			if (N1 != Q && N2 != N1) VT.Add(FVector2D(U, W));
		}
		else if (Starts(L, "usemtl"))
		{
			const FString Name = SecondWord(L);
			if (!Name.IsEmpty()) Material = Name;
		}
		else if (Starts(L, "mtllib"))
		{
			FString Mtl;
			const FString Name = SecondWord(L);
			if (!Name.IsEmpty() && FFileHelper::LoadFileToString(Mtl, *FPaths::Combine(Dir, Name)))
			{
				TArray<FString> MLines, MT;
				Mtl.ParseIntoArrayLines(MLines);
				FString Cur;
				for (const FString& ML : MLines)
				{
					MT.Reset();
					ML.ParseIntoArrayWS(MT);
					if (MT.Num() >= 2 && MT[0] == TEXT("newmtl")) Cur = MT[1];
					else if (MT.Num() >= 2 && MT[0] == TEXT("map_Kd") && !Cur.IsEmpty()) MaterialTexture.Add(Cur, FPaths::Combine(Dir, MT.Last()));
				}
			}
		}
		else if (Starts(L, "f"))
		{
			TArray<FVector, TInlineAllocator<8>> Pts;
			TArray<FVector2D, TInlineAllocator<8>> UV;
			const ANSICHAR* Q = L + 2;
			bool bBad = false;
			while (*Q)
			{
				while (IsSpace(*Q)) ++Q;
				if (!*Q) break;
				// a corner: v, v/t, v//n or v/t/n
				ANSICHAR* Next = nullptr;
				const long Vl = strtol(Q, &Next, 10);
				if (Next == Q) { bBad = true; break; }
				int32 Ti = -1;
				Q = Next;
				if (*Q == '/')
				{
					++Q;
					if (*Q != '/' && *Q && !IsSpace(*Q))
					{
						const long Tl = strtol(Q, &Next, 10);
						if (Next != Q) Ti = ResolveIndex(Tl, VT.Num());
						Q = Next;
					}
				}
				while (*Q && !IsSpace(*Q)) ++Q;            // the normal index, unused
				const int32 Vi = ResolveIndex(Vl, V.Num());
				if (!V.IsValidIndex(Vi)) { bBad = true; break; }
				Pts.Add(V[Vi]);
				UV.Add(VT.IsValidIndex(Ti) ? VT[Ti] : FVector2D::ZeroVector);
			}
			if (bBad || Pts.Num() < 3) continue;
			FOutlawsMeshSection* S = Out.Sections.Find(Material);
			if (!S)
			{
				S = &Out.Sections.Add(Material);
				S->Texture = MaterialTexture.FindRef(Material);
			}
			// the Y mirror flips handedness, so the source order is already Unreal's front-face
			// order; the materials are two-sided either way
			S->AddFace(Pts.GetData(), UV.GetData(), Pts.Num(), 1.f, !bRightHanded);
			for (const FVector& Q2 : Pts) Out.Bounds += Q2;
		}
	}
	// materials declared after their first use
	for (TPair<FString, FOutlawsMeshSection>& Kv : Out.Sections)
	{
		if (Kv.Value.Texture.IsEmpty()) Kv.Value.Texture = MaterialTexture.FindRef(Kv.Key);
	}
	return Out.Sections.Num() > 0;
}

namespace
{
	/** Moller-Trumbore: the ray O + t D crosses the triangle A B C at some t over 1e-5. */
	bool RayCrosses(const FVector& O, const FVector& D, const FVector& A, const FVector& B, const FVector& C)
	{
		const FVector E1 = B - A, E2 = C - A;
		const FVector H = FVector::CrossProduct(D, E2);
		const double Det = FVector::DotProduct(E1, H);
		if (FMath::Abs(Det) < 1e-12) return false;
		const double F = 1.0 / Det;
		const FVector S = O - A;
		const double U = F * FVector::DotProduct(S, H);
		if (U < 0.0 || U > 1.0) return false;
		const FVector Q = FVector::CrossProduct(S, E1);
		const double V = F * FVector::DotProduct(D, Q);
		if (V < 0.0 || U + V > 1.0) return false;
		return F * FVector::DotProduct(E2, Q) > 1e-5;
	}

	uint64 EdgeKey(int32 A, int32 B) { return (uint64(uint32(FMath::Min(A, B))) << 32) | uint32(FMath::Max(A, B)); }

	/** (FACETURN) A piece's triangles in a box tree: a ray is tried only against the triangles in the boxes it passes through.
	 *  The boxes are padded far past rounding, so it counts the crossings trying every triangle would. */
	struct FPieceTree
	{
		struct FNode { FVector Min, Max; int32 First = 0, Count = 0; };   // a leaf (Count over 0): Order[First, First + Count); else the children First, First + 1
		TArray<FNode> Nodes;
		TArray<int32> Order;      // the leaves' triangles, as places in the piece's member list

		void Build(const TArray<int32>& Members, const TArray<FVector>& P, const TArray<int32>& Tri)
		{
			const int32 N = Members.Num();
			TArray<FVector> Lo, Hi, Mid;
			Lo.SetNumUninitialized(N);
			Hi.SetNumUninitialized(N);
			Mid.SetNumUninitialized(N);
			Order.SetNumUninitialized(N);
			for (int32 i = 0; i < N; ++i)
			{
				const int32 t = Members[i];
				const FVector& A = P[Tri[3 * t]];
				const FVector& B = P[Tri[3 * t + 1]];
				const FVector& C = P[Tri[3 * t + 2]];
				Lo[i] = A.ComponentMin(B).ComponentMin(C);
				Hi[i] = A.ComponentMax(B).ComponentMax(C);
				Mid[i] = (A + B + C) / 3.0;
				Order[i] = i;
			}
			// each box split at the middle triangle along its triangles' longest spread, down to 8 a leaf
			struct FJob { int32 Node, Lo, Hi; };
			TArray<FJob, TInlineAllocator<64>> Jobs;
			Nodes.Reset();
			Nodes.AddDefaulted();
			Jobs.Add({ 0, 0, N });
			while (Jobs.Num())
			{
				const FJob J = Jobs.Pop(false);
				FVector BMin(UE_BIG_NUMBER), BMax(-UE_BIG_NUMBER), CMin(UE_BIG_NUMBER), CMax(-UE_BIG_NUMBER);
				for (int32 k = J.Lo; k < J.Hi; ++k)
				{
					const int32 i = Order[k];
					BMin = BMin.ComponentMin(Lo[i]);
					BMax = BMax.ComponentMax(Hi[i]);
					CMin = CMin.ComponentMin(Mid[i]);
					CMax = CMax.ComponentMax(Mid[i]);
				}
				const FVector Pad(1e-3 + 1e-6 * (BMax - BMin).GetAbsMax());
				Nodes[J.Node].Min = BMin - Pad;
				Nodes[J.Node].Max = BMax + Pad;
				if (J.Hi - J.Lo <= 8)
				{
					Nodes[J.Node].First = J.Lo;
					Nodes[J.Node].Count = J.Hi - J.Lo;
					continue;
				}
				const FVector Spread = CMax - CMin;
				const int32 Ax = Spread.X >= Spread.Y && Spread.X >= Spread.Z ? 0 : Spread.Y >= Spread.Z ? 1 : 2;
				Algo::Sort(MakeArrayView(Order.GetData() + J.Lo, J.Hi - J.Lo), [&Mid, Ax](int32 a, int32 b) { return Mid[a][Ax] < Mid[b][Ax]; });
				const int32 Half = (J.Lo + J.Hi) / 2;
				const int32 Kids = Nodes.Num();
				Nodes.AddDefaulted(2);
				Nodes[J.Node].First = Kids;
				Nodes[J.Node].Count = 0;
				Jobs.Add({ Kids, J.Lo, Half });
				Jobs.Add({ Kids + 1, Half, J.Hi });
			}
		}

		/** How many of the piece's triangles but its Skip'th the ray O + t D crosses (RayCrosses). */
		int32 Crossings(const FVector& O, const FVector& D, int32 Skip, const TArray<int32>& Members, const TArray<FVector>& P, const TArray<int32>& Tri) const
		{
			auto Inv = [](double V) { return FMath::Abs(V) > 1e-30 ? 1.0 / V : (V < 0.0 ? -1e30 : 1e30); };
			const FVector Id(Inv(D.X), Inv(D.Y), Inv(D.Z));
			int32 Hits = 0;
			TArray<int32, TInlineAllocator<64>> Stack;
			Stack.Add(0);
			while (Stack.Num())
			{
				const FNode& Nd = Nodes[Stack.Pop(false)];
				// the ray's stretch (t from 0 on) inside each pair of the box's sides
				double T0 = 0.0, T1 = UE_BIG_NUMBER;
				for (int32 a = 0; a < 3 && T0 <= T1; ++a)
				{
					double Ta = (Nd.Min[a] - O[a]) * Id[a], Tb = (Nd.Max[a] - O[a]) * Id[a];
					if (Ta > Tb) Swap(Ta, Tb);
					T0 = FMath::Max(T0, Ta);
					T1 = FMath::Min(T1, Tb);
				}
				if (T0 > T1) continue;
				if (Nd.Count == 0)
				{
					Stack.Add(Nd.First);
					Stack.Add(Nd.First + 1);
					continue;
				}
				for (int32 k = Nd.First; k < Nd.First + Nd.Count; ++k)
				{
					const int32 i = Order[k];
					if (i == Skip) continue;
					const int32 u = Members[i];
					if (RayCrosses(O, D, P[Tri[3 * u]], P[Tri[3 * u + 1]], P[Tri[3 * u + 2]])) ++Hits;
				}
			}
			return Hits;
		}
	};

	/** TurnFacesOutward's look at the triangles: OutTurn[t] for each one it would turn over. */
	OutlawsObj::FFaceTurn FacesOutward(const TArray<FVector>& P, const TArray<int32>& Tri, TArray<bool>& OutTurn)
	{
		OutlawsObj::FFaceTurn R;
		const int32 NT = Tri.Num() / 3;
		OutTurn.Init(false, NT);
		// the corners welded (a tube's seam and its caps' rims are separate corners at the same place)
		TMap<FIntVector, int32> Weld;
		TArray<int32> W;
		W.SetNumUninitialized(P.Num());
		for (int32 i = 0; i < P.Num(); ++i)
		{
			const FIntVector K(FMath::RoundToInt(P[i].X * 1000.0), FMath::RoundToInt(P[i].Y * 1000.0), FMath::RoundToInt(P[i].Z * 1000.0));
			if (const int32* Got = Weld.Find(K)) W[i] = *Got;
			else { const int32 Id = Weld.Num(); Weld.Add(K, Id); W[i] = Id; }
		}
		// the triangles with an area, each edge with the triangles on it and which way each runs it (+1 low to high)
		TArray<int32> Live;
		TArray<FVector> Front;            // (C - A) x (B - A): the side the triangle shows (FOutlawsMeshSection::AddFace)
		Front.SetNum(NT);
		TMap<uint64, TArray<TPair<int32, int32>, TInlineAllocator<2>>> Edges;
		for (int32 t = 0; t < NT; ++t)
		{
			const int32 a = Tri[3 * t], b = Tri[3 * t + 1], c = Tri[3 * t + 2];
			if (!P.IsValidIndex(a) || !P.IsValidIndex(b) || !P.IsValidIndex(c)) continue;
			const int32 w[3] = { W[a], W[b], W[c] };
			Front[t] = FVector::CrossProduct(P[c] - P[a], P[b] - P[a]);
			if (w[0] == w[1] || w[1] == w[2] || w[0] == w[2] || Front[t].SizeSquared() < 1e-16) continue;
			Live.Add(t);
			for (int32 k = 0; k < 3; ++k) Edges.FindOrAdd(EdgeKey(w[k], w[(k + 1) % 3])).Add(TPair<int32, int32>(t, w[k] < w[(k + 1) % 3] ? 1 : -1));
		}
		// the pieces, each wound as its first triangle: across an edge with two triangles on it, the neighbour runs it the
		// other way (an edge with one triangle is a border, one with more is not crossed)
		TArray<int32> Piece;
		Piece.Init(-1, NT);
		TArray<bool> Rev;
		Rev.Init(false, NT);
		TArray<TArray<int32>> Pieces;
		for (int32 s : Live)
		{
			if (Piece[s] >= 0) continue;
			const int32 Pi = Pieces.Num();
			TArray<int32> Members;
			TArray<int32> Stack;
			Stack.Add(s);
			Piece[s] = Pi;
			while (Stack.Num())
			{
				const int32 t = Stack.Pop(false);
				Members.Add(t);
				const int32 w[3] = { W[Tri[3 * t]], W[Tri[3 * t + 1]], W[Tri[3 * t + 2]] };
				for (int32 k = 0; k < 3; ++k)
				{
					const int32 e0 = w[k], e1 = w[(k + 1) % 3];
					const TArray<TPair<int32, int32>, TInlineAllocator<2>>* On = Edges.Find(EdgeKey(e0, e1));
					if (!On || On->Num() != 2) continue;
					const int32 Runs = (e0 < e1 ? 1 : -1) * (Rev[t] ? -1 : 1);
					for (const TPair<int32, int32>& Kv : *On)
					{
						if (Kv.Key == t || Piece[Kv.Key] >= 0) continue;
						Piece[Kv.Key] = Pi;
						Rev[Kv.Key] = Kv.Value == Runs;
						Stack.Add(Kv.Key);
					}
				}
			}
			Pieces.Add(MoveTemp(Members));
		}
		// each piece's faces vote (all of them, or 1500 spread over a big piece): a ray off the front, and one off the back,
		// out through the piece an even number of times starts outside it.  Kept as it is, wound as one, or that turned over:
		// whichever leaves the most area facing out, changed only when clearly better
		const FVector Jit(1.3e-4, 2.1e-4, 3.7e-4);       // (off the exact normal: a ray along an edge counts it twice)
		FPieceTree Tree;
		for (const TArray<int32>& Members : Pieces)
		{
			const bool bTree = Members.Num() > 32;
			if (bTree) Tree.Build(Members, P, Tri);
			const int32 Step = FMath::Max(1, Members.Num() / 1500);
			double Area = 0.0, OutAsIs = 0.0, OutWound = 0.0, OutTurned = 0.0;
			for (int32 m = 0; m < Members.Num(); m += Step)
			{
				const int32 t = Members[m];
				const FVector& A = P[Tri[3 * t]];
				const FVector& B = P[Tri[3 * t + 1]];
				const FVector& C = P[Tri[3 * t + 2]];
				const double Ar = Front[t].Size() * 0.5;
				const FVector N = Front[t].GetSafeNormal();
				const FVector Cen = (A + B + C) / 3.0;
				bool bOut[2];
				for (int32 Side = 0; Side < 2; ++Side)
				{
					const FVector Nd = Side ? -N : N;
					const FVector D = (Nd + Jit).GetSafeNormal();
					const FVector O = Cen + Nd * 0.002;
					int32 Hits = 0;
					if (bTree)
					{
						Hits = Tree.Crossings(O, D, m, Members, P, Tri);
					}
					else
					{
						for (int32 u : Members)
						{
							if (u != t && RayCrosses(O, D, P[Tri[3 * u]], P[Tri[3 * u + 1]], P[Tri[3 * u + 2]])) ++Hits;
						}
					}
					bOut[Side] = (Hits & 1) == 0;
				}
				Area += Ar;
				if (bOut[0]) OutAsIs += Ar;
				if (bOut[Rev[t] ? 1 : 0]) OutWound += Ar;
				if (bOut[Rev[t] ? 0 : 1]) OutTurned += Ar;
			}
			const bool bTurnAll = OutTurned > OutWound;
			const double Best = FMath::Max(OutWound, OutTurned);
			const bool bChange = Best > OutAsIs + 0.1 * Area && Best > 0.75 * Area;
			R.Faces += Members.Num();
			R.Pieces += 1;
			R.Area += Area;
			R.OutBefore += OutAsIs;
			R.OutAfter += bChange ? Best : OutAsIs;
			if (!bChange) continue;
			for (int32 t : Members)
			{
				OutTurn[t] = Rev[t] != bTurnAll;
				R.Turned += OutTurn[t] ? 1 : 0;
			}
		}
		return R;
	}
}

OutlawsObj::FFaceTurn OutlawsObj::TurnFacesOutward(FOutlawsMeshSection& S, bool bApply)
{
	TArray<bool> Turn;
	const FFaceTurn R = FacesOutward(S.Positions, S.Triangles, Turn);
	if (!bApply || R.Turned == 0) return R;
	// each turned triangle the other way round, the normal at its corners its new front (AddFace gives each polygon its
	// own corners; the loader's normal for an OBJ face was already the opposite of its old front)
	for (int32 t = 0; t < Turn.Num(); ++t)
	{
		if (!Turn[t]) continue;
		Swap(S.Triangles[3 * t + 1], S.Triangles[3 * t + 2]);
		const int32 a = S.Triangles[3 * t], b = S.Triangles[3 * t + 1], c = S.Triangles[3 * t + 2];
		const FVector N = FVector::CrossProduct(S.Positions[c] - S.Positions[a], S.Positions[b] - S.Positions[a]).GetSafeNormal();
		for (const int32 v : { a, b, c }) if (S.Normals.IsValidIndex(v)) S.Normals[v] = N;
	}
	return R;
}

OutlawsObj::FFaceTurn OutlawsObj::TurnFacesOutward(FObjMesh& M, bool bApply)
{
	FFaceTurn R;
	for (TPair<FString, FOutlawsMeshSection>& Kv : M.Sections) R.Add(TurnFacesOutward(Kv.Value, bApply));
	return R;
}

OutlawsObj::FFaceTurn OutlawsObj::FaceItemModelOutward(FObjMesh& M, const FString& What)
{
	static const bool bOld = OLFlags::Param(TEXT("OLOldScopeFaces"));
	const double T0 = FPlatformTime::Seconds();
	const FFaceTurn Ft = TurnFacesOutward(M, !bOld);
	const double Ms = (FPlatformTime::Seconds() - T0) * 1000.0;
	// a model with faces to turn on the console too; one with none to the log file only
	if (Ft.Turned > 0)
	{
		UE_LOG(LogOutlaws, Display, TEXT("%s faces%s: %d triangles in %d pieces, %d %s, facing out %.0f%% -> %.0f%%, %.1f ms"), *What, bOld ? TEXT(" (old)") : TEXT(""),
			Ft.Faces, Ft.Pieces, Ft.Turned, bOld ? TEXT("would turn") : TEXT("turned"), Ft.ShareBefore() * 100.0, (bOld ? Ft.ShareBefore() : Ft.ShareAfter()) * 100.0, Ms);
	}
	else
	{
		UE_LOG(LogOutlaws, Log, TEXT("%s faces: %d triangles in %d pieces, none to turn, facing out %.0f%%, %.1f ms"), *What, Ft.Faces, Ft.Pieces, Ft.ShareBefore() * 100.0, Ms);
	}
	return Ft;
}

OutlawsObj::FFaceTurn OutlawsObj::MeasureFacesOutward(const TArray<FVector>& Positions, const TArray<int32>& Triangles)
{
	TArray<bool> Turn;
	return FacesOutward(Positions, Triangles, Turn);
}

namespace
{
	// the file each texture LoadTextureFile made came from (its texels on the CPU, for SolidMaskOf)
	TMap<TWeakObjectPtr<const UTexture>, FString> GTextureFile;
}

UTexture2D* OutlawsObj::LoadTextureFile(const FString& Path)
{
	// once per file while anything still uses it: every gun equip used to read, decode and upload its textures again
	// (twice over: the import makes the GPU texture, the filter change remade it), a new GPU texture mid-fight
	static TMap<FString, TWeakObjectPtr<UTexture2D>> Loaded;
	if (const TWeakObjectPtr<UTexture2D>* Hit = Loaded.Find(Path))
	{
		if (Hit->IsValid()) return Hit->Get();
	}
	if (Path.IsEmpty() || !FPaths::FileExists(Path))
	{
		return nullptr;
	}
	FOutlawsAssets::NoteDevPlaceholder(Path);
	UTexture2D* T = FImageUtils::ImportFileAsTexture2D(Path);
	if (T)
	{
		T->Filter = TF_Nearest;
		T->UpdateResource();
		Loaded.Add(Path, T);
		GTextureFile.Add(T, Path);
	}
	return T;
}

bool OutlawsObj::FSolidMask::At(double U, double V) const
{
	if (W <= 0 || H <= 0) return true;
	const int32 X = FMath::Clamp(int32((U - FMath::FloorToDouble(U)) * W), 0, W - 1);
	const int32 Y = FMath::Clamp(int32((V - FMath::FloorToDouble(V)) * H), 0, H - 1);
	return Solid[Y * W + X] != 0;
}

const OutlawsObj::FSolidMask* OutlawsObj::SolidMaskOf(const UTexture* T)
{
	static TMap<FString, FSolidMask> Masks;
	const FString* Path = T ? GTextureFile.Find(T) : nullptr;
	if (!Path)
	{
		return nullptr;
	}
	if (const FSolidMask* Hit = Masks.Find(*Path))
	{
		return Hit->W > 0 ? Hit : nullptr;
	}
	FSolidMask& M = Masks.Add(*Path);
	FImage Img;
	if (FImageUtils::LoadImage(**Path, Img))
	{
		Img.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		const TArrayView64<const FColor> Px = static_cast<const FImage&>(Img).AsBGRA8();
		M.W = Img.SizeX;
		M.H = Img.SizeY;
		M.Solid.SetNumUninitialized(int32(Px.Num()));
		for (int64 i = 0; i < Px.Num(); ++i) M.Solid[int32(i)] = Px[i].A >= 128 ? 1 : 0;
	}
	return M.W > 0 ? &M : nullptr;
}
