#include "OutlawsFormats.h"
#include "OutlawsLab.h"
#include "OutlawsRuntime.h"
#include "Misc/Paths.h"

namespace
{
	inline uint16 U16(const uint8* P) { return uint16(P[0]) | (uint16(P[1]) << 8); }
	inline uint32 U32(const uint8* P) { uint32 V; FMemory::Memcpy(&V, P, 4); return V; }
	inline int32 I32(const uint8* P) { int32 V; FMemory::Memcpy(&V, P, 4); return V; }
	inline int16 I16(const uint8* P) { int16 V; FMemory::Memcpy(&V, P, 2); return V; }

	void Tokens(const FString& Line, TArray<FString>& Out)
	{
		Out.Reset();
		Line.ParseIntoArrayWS(Out);
	}
}

// ------------------------------------------------------------------------------------------------
// LAF: eight 32-bit fields (glyph count, data size, line height, ascent, two unknowns, first and last char), a
// uint16 glyph index per char from first to last, 16-byte glyph headers (data offset, advance, x and y offset as
// signed bytes, a pad byte, bitmap width and height), then the bitmaps, one byte a pixel.  The header count is
// what lies between the index and the data: the western display fonts (mf3s, sf3...) carry more headers than their
// first field says
bool OutlawsFormats::DecodeLaf(const TArray<uint8>& Data, FLafFont& Out)
{
	Out = FLafFont();
	if (Data.Num() < 32) return false;
	const uint8* P = Data.GetData();
	const int32 DataSize = I32(P + 4);
	const int32 First = I32(P + 24), Last = I32(P + 28);
	if (DataSize < 0 || DataSize > Data.Num() || First < 0 || Last < First || Last > 0xFFFF) return false;
	const int32 NumIndex = Last - First + 1;
	const int32 HeadAt = 32 + NumIndex * 2;
	const int32 DataAt = Data.Num() - DataSize;
	if (DataAt < HeadAt) return false;
	const int32 NumGlyphs = (DataAt - HeadAt) / 16;
	Out.Height = I32(P + 8);
	Out.Ascent = I32(P + 12);
	Out.FirstChar = First;
	Out.Index.SetNumUninitialized(NumIndex);
	for (int32 i = 0; i < NumIndex; ++i) Out.Index[i] = U16(P + 32 + i * 2);
	Out.Glyphs.SetNum(NumGlyphs);
	for (int32 i = 0; i < NumGlyphs; ++i)
	{
		const uint8* H = P + HeadAt + i * 16;
		FLafGlyph& G = Out.Glyphs[i];
		G.DataOffset = I32(H);
		G.Advance = int8(H[4]);
		G.OffX = int8(H[5]);
		G.OffY = int8(H[6]);
		G.W = I32(H + 8);
		G.H = I32(H + 12);
		if (G.W < 0 || G.H < 0 || G.DataOffset < 0 || int64(G.DataOffset) + int64(G.W) * G.H > DataSize) G.W = G.H = 0;
	}
	Out.Data.Append(P + DataAt, DataSize);
	return Out.Height > 0 && NumGlyphs > 0;
}

// ------------------------------------------------------------------------------------------------
bool OutlawsFormats::DecodePcx(const TArray<uint8>& Data, FPcxImage& Out)
{
	if (Data.Num() < 128 || Data[0] != 0x0A)
	{
		return false;
	}
	const uint8* D = Data.GetData();
	const uint8 Encoding = D[2];
	const uint8 Bpp = D[3];
	const int32 W = int32(U16(D + 8)) - int32(U16(D + 4)) + 1;
	const int32 H = int32(U16(D + 10)) - int32(U16(D + 6)) + 1;
	const int32 Planes = D[65];
	const int32 BytesPerLine = U16(D + 66);
	if (Bpp != 8 || Planes != 1 || W <= 0 || H <= 0 || BytesPerLine < W)
	{
		return false;
	}
	// decode the whole RLE stream (runs may cross scanlines in some encoders)
	const int64 Total = int64(BytesPerLine) * H;
	TArray<uint8> Flat;
	Flat.SetNumUninitialized(Total);
	int64 O = 0;
	int32 Pos = 128;
	const int32 DataEnd = Data.Num();
	while (O < Total && Pos < DataEnd)
	{
		uint8 B = D[Pos++];
		int32 Count = 1;
		if (Encoding == 1 && (B & 0xC0) == 0xC0)
		{
			Count = B & 0x3F;
			B = Pos < DataEnd ? D[Pos++] : 0;
		}
		while (Count-- > 0 && O < Total)
		{
			Flat[O++] = B;
		}
	}
	while (O < Total) { Flat[O++] = 0; }
	Out.W = W;
	Out.H = H;
	Out.Indices.SetNumUninitialized(W * H);
	for (int32 y = 0; y < H; ++y)
	{
		FMemory::Memcpy(Out.Indices.GetData() + y * W, Flat.GetData() + int64(y) * BytesPerLine, W);
	}
	Out.Palette.Reset();
	if (Data.Num() >= 769 && D[Data.Num() - 769] == 0x0C)
	{
		const uint8* Pal = D + Data.Num() - 768;
		Out.Palette.SetNumUninitialized(256);
		for (int32 i = 0; i < 256; ++i)
		{
			Out.Palette[i] = FColor(Pal[i * 3], Pal[i * 3 + 1], Pal[i * 3 + 2], 255);
		}
	}
	return true;
}

bool OutlawsFormats::ParseAtx(const FOutlawsAssets& Assets, const FString& Name, TArray<FOutlawsAtxInstr>& Out, bool& bOutHasStop)
{
	Out.Reset();
	bOutHasStop = false;
	FString Text;
	if (!Name.EndsWith(TEXT(".atx"), ESearchCase::IgnoreCase) || !Assets.GetText(Name, Text))
	{
		return false;
	}
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	TArray<FString> T;
	for (const FString& L : Lines)
	{
		FString Line = L;
		int32 Hash;
		if (Line.FindChar(TEXT('#'), Hash)) Line.LeftInline(Hash);      // "# 1": the authors' frame numbers
		Tokens(Line, T);
		if (T.Num() == 0) continue;
		const FString Op = T[0].ToUpper();
		FOutlawsAtxInstr I;
		if (Op == TEXT("ATX") || Op == TEXT("INST")) continue;
		if (Op == TEXT("RATE") && T.Num() > 1) { I.Kind = FOutlawsAtxInstr::EKind::Rate; I.Value = FMath::Max(1.f, FCString::Atof(*T[1])); }
		else if (Op == TEXT("TEXTURE") && T.Num() > 1) { I.Kind = FOutlawsAtxInstr::EKind::Texture; I.Name = T[1].Contains(TEXT(".")) ? T[1] : T[1] + TEXT(".pcx"); }
		else if (Op == TEXT("STOP")) { I.Kind = FOutlawsAtxInstr::EKind::Stop; bOutHasStop = true; }
		else if (Op.StartsWith(TEXT("GOTO"))) { I.Kind = FOutlawsAtxInstr::EKind::Goto; I.Value = T.Num() > 1 ? FCString::Atof(*T[1]) : FCString::Atof(*Op.Mid(4)); }   // "GOTO1" (trwhl.atx)
		else if (Op == TEXT("START_SOUND") && T.Num() > 1) { I.Kind = FOutlawsAtxInstr::EKind::Sound; I.Name = T.Last(); I.Value = T.Num() > 2 ? FCString::Atof(*T[1]) : 64.f; }
		else continue;
		Out.Add(I);
	}
	return Out.Num() > 0;
}

FString OutlawsFormats::ResolveTextureName(const FOutlawsAssets& Assets, const FString& Name)
{
	if (!Name.EndsWith(TEXT(".atx"), ESearchCase::IgnoreCase))
	{
		return Name;
	}
	FString Text;
	if (!Assets.GetText(Name, Text))
	{
		return FString();
	}
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	TArray<FString> T;
	for (const FString& L : Lines)
	{
		Tokens(L, T);
		if (T.Num() >= 2 && T[0].Equals(TEXT("TEXTURE"), ESearchCase::IgnoreCase))
		{
			return T[1];
		}
	}
	return FString();
}

bool OutlawsFormats::FindSpritePalette(const FOutlawsAssets& Assets, TArray<FColor>& Out)
{
	// PCX files whose embedded palette is exactly the game palette (verified against the
	// NwxSpxEditor palette, 256 of 256 entries, 2026-09-26).
	static const TCHAR* Candidates[] = { TEXT("snctuary.pcx"), TEXT("swchap3.pcx"), TEXT("osanchz.pcx"), TEXT("omorgan.pcx"),
		TEXT("opatrick.pcx"), TEXT("omary3.pcx"), nullptr };
	for (int32 i = 0; Candidates[i]; ++i)
	{
		TArray<uint8> Bytes;
		FPcxImage Img;
		if (Assets.Get(Candidates[i], Bytes) && DecodePcx(Bytes, Img) && Img.Palette.Num() == 256)
		{
			Out = Img.Palette;
			return true;
		}
	}
	return false;
}

// ------------------------------------------------------------------------------------------------
bool OutlawsFormats::ParseWax(TArray<uint8>&& InData, FWax& Out)
{
	Out.Data = MoveTemp(InData);
	const TArray<uint8>& Data = Out.Data;
	const int32 N = Data.Num();
	if (N < 32 || FMemory::Memcmp(Data.GetData(), "WAXF", 4) != 0)
	{
		return false;
	}
	const uint8* D = Data.GetData();
	const int32 CellOff = int32(U32(D + 20));
	const int32 FrameOff = int32(U32(D + 24));
	const int32 ChorOff = int32(U32(D + 28));
	auto Need = [N](int64 At, int64 Len) { return At >= 0 && At + Len <= N; };

	if (!Need(CellOff, 12) || FMemory::Memcmp(D + CellOff, "CELT", 4) != 0) return false;
	int32 Count = int32(U32(D + CellOff + 4));
	int32 Pos = CellOff + 12;
	for (int32 i = 0; i < Count; ++i)
	{
		if (!Need(Pos, 20)) return false;
		FWaxCell C;
		const int32 Size = I32(D + Pos + 4);
		C.W = I32(D + Pos + 8);
		C.H = I32(D + Pos + 12);
		C.Flags = U32(D + Pos + 16);
		C.DataOffset = Pos + 20;
		C.DataSize = Size;
		if (Size < 0 || !Need(C.DataOffset, Size)) return false;
		Out.Cells.Add(C);
		Pos = C.DataOffset + Size;
	}

	if (!Need(FrameOff, 12) || FMemory::Memcmp(D + FrameOff, "FRMT", 4) != 0) return false;
	Count = int32(U32(D + FrameOff + 4));
	Pos = FrameOff + 12;
	for (int32 i = 0; i < Count; ++i)
	{
		if (!Need(Pos, 24)) return false;
		FWaxFrame F;
		const int32 NodeCount = I32(D + Pos + 20);
		Pos += 24;
		if (NodeCount < 0 || !Need(Pos, int64(NodeCount) * 16)) return false;
		for (int32 k = 0; k < NodeCount; ++k)
		{
			FWaxNode Nd;
			Nd.Cell = I32(D + Pos);
			Nd.OX = I32(D + Pos + 4);
			Nd.OY = I32(D + Pos + 8);
			Nd.Flags = U32(D + Pos + 12);
			F.Nodes.Add(Nd);
			Pos += 16;
		}
		Out.Frames.Add(MoveTemp(F));
	}

	if (!Need(ChorOff, 12) || FMemory::Memcmp(D + ChorOff, "CHOT", 4) != 0) return false;
	Count = int32(U32(D + ChorOff + 4));
	Pos = ChorOff + 12;
	for (int32 i = 0; i < Count; ++i)
	{
		if (!Need(Pos, 32)) return false;
		FWaxChor C;
		C.FPS = float(U32(D + Pos + 16));
		C.Dirs = int32(U32(D + Pos + 24));
		C.Frames = int32(U32(D + Pos + 28));
		Pos += 32;
		const int64 Cells = int64(C.Dirs) * C.Frames;
		if (C.Dirs <= 0 || C.Frames <= 0 || !Need(Pos, Cells * 2)) return false;
		C.Grid.SetNumUninitialized(Cells);
		for (int64 k = 0; k < Cells; ++k)
		{
			C.Grid[k] = I16(D + Pos + k * 2);
		}
		Pos += int32(Cells * 2);
		Out.Chors.Add(MoveTemp(C));
	}
	return true;
}

bool OutlawsFormats::DecodeWaxCell(const FWax& Wax, int32 CellIndex, TArray<uint8>& Out)
{
	if (!Wax.Cells.IsValidIndex(CellIndex))
	{
		return false;
	}
	const FWaxCell& C = Wax.Cells[CellIndex];
	const int32 W = C.W, H = C.H;
	if (W <= 0 || H <= 0)
	{
		return false;
	}
	const uint8* D = Wax.Data.GetData() + C.DataOffset;
	const int32 Size = C.DataSize;
	Out.SetNumZeroed(W * H);

	auto Rle = [D, Size](int32 Off, int32 Count, uint8* Dst, int32 Stride)
	{
		int32 Ip = Off;
		int32 Op = 0;
		while (Op < Count && Ip < Size)
		{
			const uint8 Ctl = D[Ip++];
			const bool bRun = (Ctl & 1) != 0;
			int32 Cnt = FMath::Min(1 + (Ctl >> 1), Count - Op);
			if (bRun)
			{
				const uint8 V = Ip < Size ? D[Ip++] : 0;
				for (int32 k = 0; k < Cnt; ++k) Dst[(Op + k) * Stride] = V;
			}
			else
			{
				for (int32 k = 0; k < Cnt && Ip < Size; ++k) Dst[(Op + k) * Stride] = D[Ip++];
			}
			Op += Cnt;
		}
	};

	if (C.Flags & 0x2)          // row mode: i32 offset table [H], rows top-down
	{
		for (int32 y = 0; y < H && (y + 1) * 4 <= Size; ++y)
		{
			Rle(I32(D + y * 4), W, Out.GetData() + y * W, 1);
		}
	}
	else if (C.Flags & 0x1)     // column mode: i32 offset table [W], columns stored bottom-up
	{
		for (int32 x = 0; x < W && (x + 1) * 4 <= Size; ++x)
		{
			// bottom-up: element y of the column goes to row (H - 1 - y)
			Rle(I32(D + x * 4), H, Out.GetData() + (H - 1) * W + x, -W);
		}
	}
	else
	{
		return false;
	}
	return true;
}

void OutlawsFormats::ChorFrames(const int16* Row, int32 N, TArray<int32>& Out)
{
	Out.Reset();
	int32 i = 0;
	while (i < N)
	{
		const int16 V = Row[i];
		if (V >= 0)
		{
			Out.Add(V);
			++i;
		}
		else
		{
			if (N - i <= 2)
			{
				break;
			}
			i += 3;
		}
	}
}

// ------------------------------------------------------------------------------------------------
bool OutlawsFormats::ParseSprx(const TArray<uint8>& Data, TArray<FSprxFrame>& Out)
{
	Out.Reset();
	if (Data.Num() < 16 || FMemory::Memcmp(Data.GetData(), "SPRX", 4) != 0)
	{
		return false;
	}
	const uint8* D = Data.GetData();
	const int32 Count = I32(D + 12);
	if (Count <= 0 || 16 + int64(Count) * 24 > Data.Num())
	{
		return false;
	}
	for (int32 i = 0; i < Count; ++i)
	{
		const uint8* H = D + 16 + i * 24;
		FSprxFrame F;
		F.Format = U32(H);
		F.Flags = U32(H + 4);
		F.W = I32(H + 8);
		F.H = I32(H + 12);
		F.Mips = FMath::Max(1, I32(H + 16));
		F.Offset = I32(H + 20);
		Out.Add(F);
	}
	for (int32 i = 0; i < Count; ++i)
	{
		Out[i].End = (i + 1 < Count) ? Out[i + 1].Offset : Data.Num();
	}
	return true;
}

// ------------------------------------------------------------------------------------------------
bool OutlawsFormats::Parse3do(const FString& Text, F3doModel& Out)
{
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	enum class EMode { None, Verts, Tris, Quads, TVerts, TTris, TQuads } Mode = EMode::None;
	F3doObject* Cur = nullptr;
	TArray<FString> T;
	TArray<TArray<int32>> TexTris, TexQuads;    // per object, UV index rows
	TArray<int32> TriFaceIdx, QuadFaceIdx;      // face index for each triangle / quad row in order

	auto Finish = [&]()
	{
		if (!Cur) return;
		// attach texture rows to faces in order: triangles first then quads, as listed
		for (int32 i = 0; i < TriFaceIdx.Num() && i < TexTris.Num(); ++i)
		{
			F3doFace& F = Cur->Faces[TriFaceIdx[i]];
			F.T.Reset();
			for (int32 v : TexTris[i]) F.T.Add(v);
		}
		for (int32 i = 0; i < QuadFaceIdx.Num() && i < TexQuads.Num(); ++i)
		{
			F3doFace& F = Cur->Faces[QuadFaceIdx[i]];
			F.T.Reset();
			for (int32 v : TexQuads[i]) F.T.Add(v);
		}
		TexTris.Reset(); TexQuads.Reset(); TriFaceIdx.Reset(); QuadFaceIdx.Reset();
	};

	for (FString Line : Lines)
	{
		int32 Hash;
		if (Line.FindChar(TEXT('#'), Hash)) Line.LeftInline(Hash);
		Line.TrimStartAndEndInline();
		if (Line.IsEmpty()) continue;
		Tokens(Line, T);
		if (T.Num() == 0) continue;
		const FString& K = T[0];
		if (K.Equals(TEXT("TEXTURE:"), ESearchCase::IgnoreCase))
		{
			if (T.Num() > 1) Out.Textures.Add(T[1]);
			continue;
		}
		if (K.Equals(TEXT("OBJECT"), ESearchCase::IgnoreCase))
		{
			Finish();
			Cur = &Out.Objects.AddDefaulted_GetRef();
			Mode = EMode::None;
			continue;
		}
		if (!Cur) continue;
		if (K.Equals(TEXT("TEXTURE"), ESearchCase::IgnoreCase) && T.Num() > 1)
		{
			if (T[1].Equals(TEXT("VERTICES"), ESearchCase::IgnoreCase)) { Mode = EMode::TVerts; continue; }
			if (T[1].Equals(TEXT("TRIANGLES"), ESearchCase::IgnoreCase)) { Mode = EMode::TTris; continue; }
			if (T[1].Equals(TEXT("QUADS"), ESearchCase::IgnoreCase)) { Mode = EMode::TQuads; continue; }
			Cur->Texture = FCString::Atoi(*T[1]);
			continue;
		}
		if (K.Equals(TEXT("VERTICES"), ESearchCase::IgnoreCase)) { Mode = EMode::Verts; continue; }
		if (K.Equals(TEXT("TRIANGLES"), ESearchCase::IgnoreCase)) { Mode = EMode::Tris; continue; }
		if (K.Equals(TEXT("QUADS"), ESearchCase::IgnoreCase)) { Mode = EMode::Quads; continue; }
		if (!K.EndsWith(TEXT(":"))) continue;       // data rows are "n: ..."
		auto Int = [&T](int32 i) { return T.IsValidIndex(i) ? FCString::Atoi(*T[i]) : 0; };
		auto Flt = [&T](int32 i) { return T.IsValidIndex(i) ? FCString::Atof(*T[i]) : 0.f; };
		switch (Mode)
		{
		case EMode::Verts: Cur->Verts.Add(FVector3f(Flt(1), Flt(2), Flt(3))); break;
		case EMode::TVerts: Cur->TVerts.Add(FVector2f(Flt(1), Flt(2))); break;
		case EMode::Tris:
		case EMode::Quads:
		{
			const int32 NV = Mode == EMode::Tris ? 3 : 4;
			F3doFace F;
			for (int32 i = 0; i < NV; ++i) F.V.Add(Int(1 + i));
			F.Color = Int(1 + NV);
			const FString Shading = T.IsValidIndex(2 + NV) ? T[2 + NV].ToUpper() : TEXT("FLAT");
			F.bTextured = Shading == TEXT("GOURTEX") || Shading == TEXT("TEXTURE") || Shading == TEXT("PLANTEX") || Shading == TEXT("TEXT");
			(Mode == EMode::Tris ? TriFaceIdx : QuadFaceIdx).Add(Cur->Faces.Num());
			Cur->Faces.Add(MoveTemp(F));
			break;
		}
		case EMode::TTris: TexTris.Add({ Int(1), Int(2), Int(3) }); break;
		case EMode::TQuads: TexQuads.Add({ Int(1), Int(2), Int(3), Int(4) }); break;
		default: break;
		}
	}
	Finish();
	return Out.Objects.Num() > 0;
}

// ------------------------------------------------------------------------------------------------
void OutlawsFormats::ParseItm(const FString& ItmText, FString& OutFunc, TMap<FString, FString>& OutData)
{
	OutFunc.Reset();
	OutData.Reset();
	TArray<FString> Lines;
	ItmText.ParseIntoArrayLines(Lines);
	TArray<FString> T;
	for (const FString& L : Lines)
	{
		T.Reset();
		L.ParseIntoArrayWS(T);
		if (T.Num() >= 2 && T[0].Equals(TEXT("FUNC"), ESearchCase::IgnoreCase))
		{
			OutFunc = T[1];
		}
		else if (T.Num() >= 3 && (T[0].Equals(TEXT("STR"), ESearchCase::IgnoreCase) || T[0].Equals(TEXT("INT"), ESearchCase::IgnoreCase) ||
			T[0].Equals(TEXT("FLOAT"), ESearchCase::IgnoreCase)))
		{
			OutData.Add(T[1].ToUpper(), T[2]);
		}
	}
}

FString OutlawsFormats::ResolveAnim(const FOutlawsAssets& Assets, const FString& Name, FString* OutItm, int32 Depth)
{
	FString Itm;
	const bool bHasItm = Assets.GetText(Name + TEXT(".itm"), Itm);
	if (OutItm && Depth == 0)
	{
		*OutItm = bHasItm ? Itm : FString();
	}
	if (!bHasItm)
	{
		if (Depth == 0)
		{
			for (const TCHAR* Ext : { TEXT(".nwx"), TEXT(".3do") })
			{
				if (Assets.Has(Name + Ext))
				{
					return Name + Ext;
				}
			}
		}
		return FString();
	}
	if (Depth > 3)
	{
		return FString();
	}
	// generators (respawning pickups, effect and sound spawners) have no picture of their own:
	// show the item they spawn, nothing for effects and sounds (olue_export.py generator_target)
	FString Generate;
	if (GeneratorTarget(Itm, Generate))
	{
		if (Generate.EndsWith(TEXT(".itm"), ESearchCase::IgnoreCase))
		{
			return ResolveAnim(Assets, Generate.LeftChop(4), nullptr, Depth + 1);
		}
		return FString();
	}
	TArray<FString> Lines;
	Itm.ParseIntoArrayLines(Lines);
	TArray<FString> T;
	for (const FString& L : Lines)
	{
		Tokens(L, T);
		if (T.Num() >= 2 && T[0].Equals(TEXT("ANIM"), ESearchCase::IgnoreCase))
		{
			if (!T[1].Equals(TEXT("NULL"), ESearchCase::IgnoreCase))
			{
				return T[1];
			}
			break;   // first ANIM line only, as the exporter does
		}
	}
	// no usable ANIM: follow the first ITM the file names (GENERATE / proxy items)
	const FString Upper = Itm.ToUpper();
	const int32 At = Upper.Find(TEXT(".ITM"), ESearchCase::CaseSensitive);
	if (At != INDEX_NONE)
	{
		int32 S = At;
		while (S > 0 && (FChar::IsAlnum(Upper[S - 1]) || Upper[S - 1] == TEXT('_') || Upper[S - 1] == TEXT('-'))) --S;
		const FString Ref = Upper.Mid(S, At - S);
		if (!Ref.IsEmpty() && !Ref.Equals(Name, ESearchCase::IgnoreCase))
		{
			return ResolveAnim(Assets, Ref, nullptr, Depth + 1);
		}
	}
	return FString();
}

bool OutlawsFormats::GeneratorTarget(const FString& ItmText, FString& OutGenerate)
{
	OutGenerate.Reset();
	if (ItmText.IsEmpty())
	{
		return false;
	}
	TArray<FString> Lines;
	ItmText.ParseIntoArrayLines(Lines);
	TArray<FString> T;
	bool bGenerator = false;
	bool bSeenFunc = false;
	for (const FString& L : Lines)
	{
		Tokens(L, T);
		if (!bSeenFunc && T.Num() >= 2 && T[0].Equals(TEXT("FUNC"), ESearchCase::IgnoreCase))
		{
			bSeenFunc = true;   // the first FUNC line decides, as in the exporter
			bGenerator = T[1].StartsWith(TEXT("Gen_"), ESearchCase::IgnoreCase);
			if (!bGenerator)
			{
				return false;
			}
		}
		else if (T.Num() >= 3 && T[0].Equals(TEXT("STR"), ESearchCase::IgnoreCase) && T[1].Equals(TEXT("GENERATE"), ESearchCase::IgnoreCase) && OutGenerate.IsEmpty())
		{
			OutGenerate = T[2];
		}
	}
	return bGenerator;
}

bool OutlawsFormats::IsHanging(const FString& Name, const FString& ItmText)
{
	static const TCHAR* NameParts[] = { TEXT("CHAND"), TEXT("LAMP"), TEXT("LANTERN"), TEXT("BANNER"), TEXT("FAN"), TEXT("TRAPEZE"), TEXT("TORCH"), TEXT("HANG"), nullptr };
	const FString U = Name.ToUpper();
	for (int32 i = 0; NameParts[i]; ++i)
	{
		if (U.Contains(NameParts[i])) return true;
	}
	if (!ItmText.IsEmpty())
	{
		TArray<FString> T;
		Tokens(ItmText.ToUpper(), T);
		for (const FString& W : T)
		{
			if (W == TEXT("HANG") || W == TEXT("HANG_FROM_CEILING") || W == TEXT("HANG_OBJECT") || W == TEXT("DANGLE")) return true;
		}
	}
	return false;
}
