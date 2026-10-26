#include "OutlawsObjMesh.h"
#include "OutlawsRuntime.h"
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
