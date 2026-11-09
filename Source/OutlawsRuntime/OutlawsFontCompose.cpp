// (REQ-LANGUAGE) See OutlawsFontCompose.h.  A small TrueType reader and writer: the table directory, cmap (formats 4 and 12 read,
// format 4 written), loca, glyf (simple glyphs read and written), hmtx, hhea, maxp, head and post.  The new font keeps every glyph
// of the theme as it was and appends the composed ones; the character map gains them, post becomes format 3 (no glyph names).
#include "OutlawsFontCompose.h"
#include "OutlawsFlags.h"

namespace
{
	uint16 U16(const TArray<uint8>& B, int64 O) { return O + 2 <= B.Num() ? uint16((B[O] << 8) | B[O + 1]) : 0; }
	int16 S16(const TArray<uint8>& B, int64 O) { return int16(U16(B, O)); }
	uint32 U32(const TArray<uint8>& B, int64 O) { return O + 4 <= B.Num() ? (uint32(B[O]) << 24) | (uint32(B[O + 1]) << 16) | (uint32(B[O + 2]) << 8) | B[O + 3] : 0; }
	void P16(TArray<uint8>& B, uint16 V) { B.Add(uint8(V >> 8)); B.Add(uint8(V)); }
	void P32(TArray<uint8>& B, uint32 V) { P16(B, uint16(V >> 16)); P16(B, uint16(V)); }
	void W16(TArray<uint8>& B, int64 O, uint16 V) { if (O + 2 <= B.Num()) { B[O] = uint8(V >> 8); B[O + 1] = uint8(V); } }
	void W32(TArray<uint8>& B, int64 O, uint32 V) { W16(B, O, uint16(V >> 16)); W16(B, O + 2, uint16(V)); }

	struct FTable { uint32 Tag = 0; TArray<uint8> Data; };

	uint32 Tag(const char* T) { return (uint32(uint8(T[0])) << 24) | (uint32(uint8(T[1])) << 16) | (uint32(uint8(T[2])) << 8) | uint8(T[3]); }

	struct FFont
	{
		uint32 Version = 0;
		TArray<FTable> Tables;
		const TArray<uint8>* Find(const char* T) const
		{
			for (const FTable& X : Tables) if (X.Tag == Tag(T)) return &X.Data;
			return nullptr;
		}
		TArray<uint8>* Find(const char* T)
		{
			for (FTable& X : Tables) if (X.Tag == Tag(T)) return &X.Data;
			return nullptr;
		}
		bool Read(const TArray<uint8>& B)
		{
			Version = U32(B, 0);
			if (Version != 0x00010000 && Version != Tag("true")) return false;     // TrueType outlines only (no CFF, no collection)
			const int32 N = U16(B, 4);
			for (int32 i = 0; i < N; ++i)
			{
				const int64 R = 12 + 16 * i;
				const uint32 Off = U32(B, R + 8), Len = U32(B, R + 12);
				if (R + 16 > B.Num() || int64(Off) + Len > B.Num()) return false;
				FTable& T = Tables.AddDefaulted_GetRef();
				T.Tag = U32(B, R);
				T.Data.Append(B.GetData() + Off, Len);
			}
			return Find("head") && Find("maxp") && Find("hhea") && Find("hmtx") && Find("loca") && Find("glyf") && Find("cmap");
		}
		int32 NumGlyphs() const { return U16(*Find("maxp"), 4); }
		bool LongLoca() const { return S16(*Find("head"), 50) != 0; }
		// a glyph's bytes in glyf (empty for a blank one)
		bool GlyphBytes(int32 G, int64& Start, int64& End) const
		{
			const TArray<uint8>& L = *Find("loca");
			if (G < 0 || G >= NumGlyphs()) return false;
			if (LongLoca()) { Start = U32(L, 4 * G); End = U32(L, 4 * G + 4); }
			else { Start = int64(U16(L, 2 * G)) * 2; End = int64(U16(L, 2 * G + 2)) * 2; }
			return End >= Start && End <= Find("glyf")->Num();
		}
		void Metric(int32 G, uint16& Adv, int16& Lsb) const
		{
			const TArray<uint8>& H = *Find("hmtx");
			const int32 NH = FMath::Max<int32>(1, U16(*Find("hhea"), 34));
			if (G < NH) { Adv = U16(H, 4 * G); Lsb = S16(H, 4 * G + 2); }
			else { Adv = U16(H, 4 * (NH - 1)); Lsb = S16(H, 4 * NH + 2 * (G - NH)); }
		}
	};

	// the character map: (3,10) format 12 first, then (3,1) / (0,x) format 4 or 12
	bool ReadCmap(const TArray<uint8>& C, TMap<uint32, uint16>& Map)
	{
		const int32 N = U16(C, 2);
		int64 Best = -1;
		int32 BestRank = 0;
		for (int32 i = 0; i < N; ++i)
		{
			const uint16 Pid = U16(C, 4 + 8 * i), Eid = U16(C, 6 + 8 * i);
			const int64 Off = U32(C, 8 + 8 * i);
			const uint16 Fmt = U16(C, Off);
			if (Fmt != 4 && Fmt != 12) continue;
			const int32 Rank = (Pid == 3 && Eid == 10) ? 4 : (Pid == 3 && Eid == 1) ? 3 : Pid == 0 ? 2 : 0;
			if (Rank > BestRank) { BestRank = Rank; Best = Off; }
		}
		if (Best < 0) return false;
		if (U16(C, Best) == 4)
		{
			const int32 Seg = U16(C, Best + 6) / 2;
			const int64 Ends = Best + 14, Starts = Ends + 2 * Seg + 2, Deltas = Starts + 2 * Seg, Ranges = Deltas + 2 * Seg;
			for (int32 s = 0; s < Seg; ++s)
			{
				const uint32 E = U16(C, Ends + 2 * s), S = U16(C, Starts + 2 * s);
				const uint16 D = U16(C, Deltas + 2 * s), R = U16(C, Ranges + 2 * s);
				for (uint32 c = S; c <= E && c != 0xFFFF; ++c)
				{
					uint16 G = 0;
					if (R == 0) G = uint16(c + D);
					else
					{
						const uint16 Raw = U16(C, Ranges + 2 * s + R + 2 * (c - S));
						G = Raw ? uint16(Raw + D) : 0;
					}
					if (G) Map.Add(c, G);
				}
			}
		}
		else
		{
			const uint32 Groups = U32(C, Best + 12);
			for (uint32 g = 0; g < Groups && g < 100000; ++g)
			{
				const int64 O = Best + 16 + 12 * g;
				const uint32 S = U32(C, O), E = U32(C, O + 4), G0 = U32(C, O + 8);
				for (uint32 c = S; c <= E && c - S < 0x10000; ++c) if (G0 + (c - S)) Map.Add(c, uint16(G0 + (c - S)));
			}
		}
		return true;
	}

	struct FPt { int32 X = 0, Y = 0; bool bOn = true; };
	struct FGlyph
	{
		TArray<FPt> Pts;
		TArray<int32> Ends;           // the last point of each contour
		bool IsEmpty() const { return Pts.Num() == 0; }
		void Bounds(int32& X0, int32& Y0, int32& X1, int32& Y1) const
		{
			X0 = Y0 = MAX_int32; X1 = Y1 = MIN_int32;
			for (const FPt& P : Pts) { X0 = FMath::Min(X0, P.X); Y0 = FMath::Min(Y0, P.Y); X1 = FMath::Max(X1, P.X); Y1 = FMath::Max(Y1, P.Y); }
			if (Pts.Num() == 0) X0 = Y0 = X1 = Y1 = 0;
		}
		void Append(const FGlyph& O, double Sx, double Sy, double Dx, double Dy)
		{
			const int32 Base = Pts.Num();
			for (const FPt& P : O.Pts) Pts.Add({ int32(FMath::RoundToDouble(P.X * Sx + Dx)), int32(FMath::RoundToDouble(P.Y * Sy + Dy)), P.bOn });
			for (int32 E : O.Ends) Ends.Add(E + Base);
		}
	};

	// a simple glyph's outline (false for a composite or broken one)
	bool ReadGlyph(const FFont& F, int32 G, FGlyph& Out)
	{
		int64 S = 0, E = 0;
		if (!F.GlyphBytes(G, S, E)) return false;
		if (E == S) return true;      // blank
		const TArray<uint8>& B = *F.Find("glyf");
		const int16 NC = S16(B, S);
		if (NC < 0) return false;
		int64 O = S + 10;
		for (int32 c = 0; c < NC; ++c) Out.Ends.Add(U16(B, O + 2 * c));
		O += 2 * NC;
		const int32 NP = NC ? Out.Ends.Last() + 1 : 0;
		O += 2 + U16(B, O);           // the instructions, skipped
		TArray<uint8> Flags;
		while (Flags.Num() < NP && O < E)
		{
			const uint8 Fl = B[O++];
			Flags.Add(Fl);
			if (Fl & 8) { int32 R = B[O++]; while (R-- > 0 && Flags.Num() < NP) Flags.Add(Fl); }
		}
		if (Flags.Num() != NP) return false;
		Out.Pts.SetNum(NP);
		int32 V = 0;
		for (int32 p = 0; p < NP; ++p)
		{
			const uint8 Fl = Flags[p];
			if (Fl & 2) { const int32 D = B[O++]; V += (Fl & 16) ? D : -D; }
			else if (!(Fl & 16)) { V += S16(B, O); O += 2; }
			Out.Pts[p].X = V;
			Out.Pts[p].bOn = (Fl & 1) != 0;
		}
		V = 0;
		for (int32 p = 0; p < NP; ++p)
		{
			const uint8 Fl = Flags[p];
			if (Fl & 4) { const int32 D = B[O++]; V += (Fl & 32) ? D : -D; }
			else if (!(Fl & 32)) { V += S16(B, O); O += 2; }
			Out.Pts[p].Y = V;
		}
		return O <= E;
	}

	// a simple glyph, every coordinate a full word (no instructions)
	void WriteGlyph(const FGlyph& G, TArray<uint8>& B)
	{
		if (G.IsEmpty()) return;
		int32 X0, Y0, X1, Y1;
		G.Bounds(X0, Y0, X1, Y1);
		P16(B, uint16(G.Ends.Num())); P16(B, uint16(X0)); P16(B, uint16(Y0)); P16(B, uint16(X1)); P16(B, uint16(Y1));
		for (int32 E : G.Ends) P16(B, uint16(E));
		P16(B, 0);
		for (const FPt& P : G.Pts) B.Add(P.bOn ? 1 : 0);
		int32 V = 0;
		for (const FPt& P : G.Pts) { P16(B, uint16(int16(P.X - V))); V = P.X; }
		V = 0;
		for (const FPt& P : G.Pts) { P16(B, uint16(int16(P.Y - V))); V = P.Y; }
		while (B.Num() % 4) B.Add(0);
	}

	enum EMark : uint8 { Grave, Acute, Circ, Tilde, Dier, Ring, Cedilla, Pair, NumMarks };
	const uint32 MarkCp[] = { 0x60, 0xB4, 0x2C6, 0x2DC, 0xA8, 0x2DA, 0xB8 };

	struct FCompose { uint32 Cp; uint32 Base; EMark Mark; uint32 Second; };
	const TArray<FCompose>& Table()
	{
		static TArray<FCompose> T = []()
		{
			TArray<FCompose> R;
			auto Add = [&R](uint32 Cp, TCHAR Base, EMark M) { R.Add({ Cp, uint32(Base), M, 0 }); };
			// Latin-1 0xC0-0xFF, upper case then lower case (lower = upper + 0x20)
			const struct { uint32 Up; TCHAR Base; EMark M; } L1[] = {
				{ 0xC0, 'A', Grave }, { 0xC1, 'A', Acute }, { 0xC2, 'A', Circ }, { 0xC3, 'A', Tilde }, { 0xC4, 'A', Dier }, { 0xC5, 'A', Ring },
				{ 0xC7, 'C', Cedilla }, { 0xC8, 'E', Grave }, { 0xC9, 'E', Acute }, { 0xCA, 'E', Circ }, { 0xCB, 'E', Dier },
				{ 0xCC, 'I', Grave }, { 0xCD, 'I', Acute }, { 0xCE, 'I', Circ }, { 0xCF, 'I', Dier }, { 0xD1, 'N', Tilde },
				{ 0xD2, 'O', Grave }, { 0xD3, 'O', Acute }, { 0xD4, 'O', Circ }, { 0xD5, 'O', Tilde }, { 0xD6, 'O', Dier },
				{ 0xD9, 'U', Grave }, { 0xDA, 'U', Acute }, { 0xDB, 'U', Circ }, { 0xDC, 'U', Dier }, { 0xDD, 'Y', Acute } };
			for (const auto& E : L1) Add(E.Up, E.Base, E.M);
			for (const auto& E : L1) Add(E.Up + 0x20, TCHAR(E.Base + 0x20), E.M);
			Add(0xFF, 'y', Dier);
			Add(0x178, 'Y', Dier);
			R.Add({ 0xC6, 'A', Pair, 'E' });
			R.Add({ 0xE6, 'a', Pair, 'e' });
			R.Add({ 0x152, 'O', Pair, 'E' });
			R.Add({ 0x153, 'o', Pair, 'e' });
			R.Add({ 0xDF, 's', Pair, 's' });     // the German sharp s as the font's own "ss" (the Swiss spelling, and what capitals use)
			return R;
		}();
		return T;
	}

	// a dotted base (a lower-case i in a font with lower case): the dot is a contour wholly above the rest; dropped under a mark
	FGlyph Undotted(const FGlyph& G)
	{
		if (G.Ends.Num() < 2) return G;
		int32 Top = INDEX_NONE, TopY0 = MIN_int32, RestY1 = MIN_int32;
		TArray<int32> Y0s, Y1s;
		for (int32 c = 0, First = 0; c < G.Ends.Num(); First = G.Ends[c] + 1, ++c)
		{
			int32 A = MAX_int32, Z = MIN_int32;
			for (int32 p = First; p <= G.Ends[c]; ++p) { A = FMath::Min(A, G.Pts[p].Y); Z = FMath::Max(Z, G.Pts[p].Y); }
			Y0s.Add(A); Y1s.Add(Z);
			if (A > TopY0) { TopY0 = A; Top = c; }
		}
		for (int32 c = 0; c < Y1s.Num(); ++c) if (c != Top) RestY1 = FMath::Max(RestY1, Y1s[c]);
		if (Top == INDEX_NONE || TopY0 <= RestY1) return G;
		FGlyph R;
		for (int32 c = 0, First = 0; c < G.Ends.Num(); First = G.Ends[c] + 1, ++c)
		{
			if (c == Top) continue;
			for (int32 p = First; p <= G.Ends[c]; ++p) R.Pts.Add(G.Pts[p]);
			R.Ends.Add(R.Pts.Num() - 1);
		}
		return R;
	}

	uint32 Sum(const TArray<uint8>& D)
	{
		uint32 S = 0;
		for (int32 i = 0; i < D.Num(); i += 4)
		{
			uint32 W = 0;
			for (int32 k = 0; k < 4; ++k) W = (W << 8) | (i + k < D.Num() ? D[i + k] : 0);
			S += W;
		}
		return S;
	}

	TArray<uint8> BuildCmap4(const TMap<uint32, uint16>& Map)
	{
		TArray<uint32> Keys;
		Map.GetKeys(Keys);
		Keys.Sort();
		struct FSeg { uint16 S, E, D; };
		TArray<FSeg> Segs;
		for (uint32 K : Keys)
		{
			if (K >= 0xFFFF) continue;
			const uint16 G = Map[K];
			if (Segs.Num() && Segs.Last().E + 1 == K && uint16(K + Segs.Last().D) == G) { Segs.Last().E = uint16(K); continue; }
			Segs.Add({ uint16(K), uint16(K), uint16(G - K) });
		}
		Segs.Add({ 0xFFFF, 0xFFFF, 1 });
		const int32 N = Segs.Num();
		int32 Pow = 1, Log = 0;
		while (Pow * 2 <= N) { Pow *= 2; ++Log; }
		TArray<uint8> Sub;
		P16(Sub, 4); P16(Sub, uint16(16 + 8 * N)); P16(Sub, 0);
		P16(Sub, uint16(2 * N)); P16(Sub, uint16(2 * Pow)); P16(Sub, uint16(Log)); P16(Sub, uint16(2 * N - 2 * Pow));
		for (const FSeg& S : Segs) P16(Sub, S.E);
		P16(Sub, 0);
		for (const FSeg& S : Segs) P16(Sub, S.S);
		for (const FSeg& S : Segs) P16(Sub, S.D);
		for (int32 i = 0; i < N; ++i) P16(Sub, 0);
		TArray<uint8> C;
		P16(C, 0); P16(C, 2);
		P16(C, 0); P16(C, 3); P32(C, 20);
		P16(C, 3); P16(C, 1); P32(C, 20);
		C.Append(Sub);
		return C;
	}
}

bool OLFontCompose::OldAccents()
{
	static const bool bOld = OLFlags::Param(TEXT("OLOldMenuAccents"));
	return bOld;
}

const TArray<uint32>& OLFontCompose::Letters()
{
	static TArray<uint32> L = []() { TArray<uint32> R; for (const FCompose& C : Table()) R.Add(C.Cp); return R; }();
	return L;
}

uint32 OLFontCompose::BaseOf(uint32 Cp)
{
	for (const FCompose& C : Table()) if (C.Cp == Cp) return C.Base;
	return 0;
}

bool OLFontCompose::HasChar(const TArray<uint8>& Font, uint32 Cp)
{
	FFont F;
	TMap<uint32, uint16> Map;
	return F.Read(Font) && ReadCmap(*F.Find("cmap"), Map) && Map.Contains(Cp);
}

bool OLFontCompose::StartsWithBase(const TArray<uint8>& Font, uint32 Cp, uint32 BaseCp)
{
	FFont F;
	TMap<uint32, uint16> Map;
	if (!F.Read(Font) || !ReadCmap(*F.Find("cmap"), Map) || !Map.Contains(Cp) || !Map.Contains(BaseCp)) return false;
	FGlyph G, B;
	if (!ReadGlyph(F, Map[Cp], G) || !ReadGlyph(F, Map[BaseCp], B) || B.IsEmpty()) return false;
	B = Undotted(B);
	if (G.Pts.Num() <= B.Pts.Num() || G.Ends.Num() <= B.Ends.Num()) return false;     // the base and at least one more contour
	for (int32 i = 0; i < B.Pts.Num(); ++i) if (G.Pts[i].X != B.Pts[i].X || G.Pts[i].Y != B.Pts[i].Y || G.Pts[i].bOn != B.Pts[i].bOn) return false;
	for (int32 c = 0; c < B.Ends.Num(); ++c) if (G.Ends[c] != B.Ends[c]) return false;
	return true;
}

bool OLFontCompose::Compose(const TArray<uint8>& Theme, const TArray<uint8>& Marks, TArray<uint8>& Out, TArray<uint32>& Added)
{
	Out.Reset();
	Added.Reset();
	FFont T, M;
	TMap<uint32, uint16> TMapCp, MMapCp;
	if (!T.Read(Theme) || !M.Read(Marks) || !ReadCmap(*T.Find("cmap"), TMapCp) || !ReadCmap(*M.Find("cmap"), MMapCp)) return false;
	for (const auto& KV : TMapCp) if (KV.Key > 0xFFFF) return false;     // (a format 4 map is written back: nothing past the BMP to lose)
	if (TMapCp.Contains(0xE9) || TMapCp.Contains(0xC9)) return false;     // it has its own accented letters
	if (T.Find("gvar") || T.Find("fvar")) return false;                   // (a variable font's variations would not cover new glyphs)
	auto Glyph = [](const FFont& F, const TMap<uint32, uint16>& Map, uint32 Cp, FGlyph& G) { G = FGlyph(); const uint16* I = Map.Find(Cp); return I && ReadGlyph(F, *I, G) && !G.IsEmpty(); };
	// the size of a mark: Noto's capitals against the theme's (the H, else the O)
	FGlyph Probe;
	int32 X0, Y0, X1, Y1;
	double ThemeCap = 0.0, MarkCap = 0.0;
	if (Glyph(T, TMapCp, 'H', Probe) || Glyph(T, TMapCp, 'O', Probe)) { Probe.Bounds(X0, Y0, X1, Y1); ThemeCap = Y1; }
	if (Glyph(M, MMapCp, 'H', Probe)) { Probe.Bounds(X0, Y0, X1, Y1); MarkCap = Y1; }
	if (ThemeCap <= 0.0 || MarkCap <= 0.0) return false;
	const double Ratio = ThemeCap / MarkCap;
	// (the overlay's letters are heavy and wide: the mark a little wider than tall, so its strokes hold up beside them)
	const double Sy = Ratio * 0.92, SxWant = Ratio * 1.08;
	const double Gap = ThemeCap * 0.05;
	FGlyph MarkG[NumMarks];
	bool bMark[NumMarks] = {};
	for (int32 m = 0; m < Cedilla + 1; ++m) bMark[m] = Glyph(M, MMapCp, MarkCp[m], MarkG[m]);

	const int32 N0 = T.NumGlyphs();
	TArray<uint8> NewGlyf;
	TArray<uint32> Offsets;
	TArray<TPair<uint16, int16>> NewMetrics;
	int32 MaxPts = 0, MaxCont = 0;
	TMap<uint32, uint16> NewMap = TMapCp;
	for (const FCompose& C : Table())
	{
		if (TMapCp.Contains(C.Cp)) continue;
		FGlyph Base;
		if (!Glyph(T, TMapCp, C.Base, Base)) continue;
		uint16 Adv = 0; int16 Lsb = 0;
		T.Metric(TMapCp[C.Base], Adv, Lsb);
		FGlyph G;
		if (C.Mark == Pair)
		{
			FGlyph Second;
			uint16 Adv2 = 0; int16 Lsb2 = 0;
			if (!Glyph(T, TMapCp, C.Second, Second)) continue;
			T.Metric(TMapCp[C.Second], Adv2, Lsb2);
			Base.Bounds(X0, Y0, X1, Y1);
			int32 A0, B0, A1, B1;
			Second.Bounds(A0, B0, A1, B1);
			// the second letter set close against the first, a twentieth of the capital height overlapping
			const double Dx = X1 - A0 - ThemeCap * 0.05;
			G = Base;
			G.Append(Second, 1.0, 1.0, Dx, 0.0);
			Adv = uint16(FMath::Max<double>(Adv, Dx + Adv2));
		}
		else
		{
			if (!bMark[C.Mark]) continue;
			G = Undotted(Base);
			G.Bounds(X0, Y0, X1, Y1);
			const FGlyph& Mk = MarkG[C.Mark];
			int32 A0, B0, A1, B1;
			Mk.Bounds(A0, B0, A1, B1);
			const double Sx = FMath::Min(SxWant, (X1 - X0) * 0.9 / FMath::Max(1, A1 - A0));
			const double Dx = (X0 + X1) * 0.5 - (A0 + A1) * 0.5 * Sx;
			const double Dy = C.Mark == Cedilla ? Y0 + Gap * 0.6 - B1 * Sy : Y1 + Gap - B0 * Sy;
			G.Append(Mk, Sx, Sy, Dx, Dy);
		}
		const int32 Gid = N0 + NewMetrics.Num();
		if (Gid > 0xFFFE) break;
		Offsets.Add(NewGlyf.Num());
		WriteGlyph(G, NewGlyf);
		int32 GX0, GY0, GX1, GY1;
		G.Bounds(GX0, GY0, GX1, GY1);
		NewMetrics.Add(TPair<uint16, int16>(Adv, int16(GX0)));
		MaxPts = FMath::Max(MaxPts, G.Pts.Num());
		MaxCont = FMath::Max(MaxCont, G.Ends.Num());
		NewMap.Add(C.Cp, uint16(Gid));
		Added.Add(C.Cp);
	}
	if (Added.Num() == 0) return false;
	const int32 N1 = N0 + NewMetrics.Num();

	// glyf: the old bytes as they were (each glyph's own offsets kept), the new glyphs after, 4-aligned
	TArray<uint8> Glyf = *T.Find("glyf");
	while (Glyf.Num() % 4) Glyf.Add(0);
	const uint32 Base0 = Glyf.Num();
	Glyf.Append(NewGlyf);
	TArray<uint8> Loca;
	for (int32 g = 0; g < N0; ++g) { int64 S = 0, E = 0; T.GlyphBytes(g, S, E); P32(Loca, uint32(S)); }
	for (int32 i = 0; i < Offsets.Num(); ++i) P32(Loca, Base0 + Offsets[i]);
	P32(Loca, Glyf.Num());
	// hmtx: every glyph a full metric
	TArray<uint8> Hmtx;
	uint16 AdvMax = 0;
	for (int32 g = 0; g < N0; ++g) { uint16 A; int16 L; T.Metric(g, A, L); P16(Hmtx, A); P16(Hmtx, uint16(L)); AdvMax = FMath::Max(AdvMax, A); }
	for (const auto& Mt : NewMetrics) { P16(Hmtx, Mt.Key); P16(Hmtx, uint16(Mt.Value)); AdvMax = FMath::Max(AdvMax, Mt.Key); }
	TArray<uint8> Hhea = *T.Find("hhea");
	W16(Hhea, 34, uint16(N1));
	W16(Hhea, 10, FMath::Max<uint16>(U16(Hhea, 10), AdvMax));
	TArray<uint8> Maxp = *T.Find("maxp");
	W16(Maxp, 4, uint16(N1));
	if (U32(Maxp, 0) == 0x00010000)
	{
		W16(Maxp, 6, FMath::Max<uint16>(U16(Maxp, 6), uint16(MaxPts)));
		W16(Maxp, 8, FMath::Max<uint16>(U16(Maxp, 8), uint16(MaxCont)));
	}
	TArray<uint8> Head = *T.Find("head");
	W16(Head, 50, 1);
	{
		int16 HX0 = S16(Head, 36), HY0 = S16(Head, 38), HX1 = S16(Head, 40), HY1 = S16(Head, 42);
		for (int32 i = 0; i < Offsets.Num(); ++i)
		{
			const int64 O = Base0 + Offsets[i];
			HX0 = FMath::Min(HX0, S16(Glyf, O + 2)); HY0 = FMath::Min(HY0, S16(Glyf, O + 4));
			HX1 = FMath::Max(HX1, S16(Glyf, O + 6)); HY1 = FMath::Max(HY1, S16(Glyf, O + 8));
		}
		W16(Head, 36, uint16(HX0)); W16(Head, 38, uint16(HY0)); W16(Head, 40, uint16(HX1)); W16(Head, 42, uint16(HY1));
	}
	W32(Head, 8, 0);
	TArray<uint8> Post;
	if (const TArray<uint8>* P = T.Find("post")) { Post.Append(P->GetData(), FMath::Min(32, P->Num())); }
	Post.SetNumZeroed(32);
	W32(Post, 0, 0x00030000);

	// the new table set: the theme's, the changed ones replaced, those tied to the old glyph count dropped
	TArray<FTable> Tables;
	for (const FTable& X : T.Tables)
	{
		if (X.Tag == Tag("hdmx") || X.Tag == Tag("LTSH") || X.Tag == Tag("VDMX") || X.Tag == Tag("DSIG")) continue;
		FTable& Y = Tables.AddDefaulted_GetRef();
		Y.Tag = X.Tag;
		Y.Data = X.Tag == Tag("glyf") ? Glyf : X.Tag == Tag("loca") ? Loca : X.Tag == Tag("hmtx") ? Hmtx : X.Tag == Tag("hhea") ? Hhea
			: X.Tag == Tag("maxp") ? Maxp : X.Tag == Tag("head") ? Head : X.Tag == Tag("cmap") ? BuildCmap4(NewMap) : X.Tag == Tag("post") ? Post : X.Data;
	}
	Tables.Sort([](const FTable& A, const FTable& B) { return A.Tag < B.Tag; });
	const int32 NT = Tables.Num();
	int32 Pow = 1, Log = 0;
	while (Pow * 2 <= NT) { Pow *= 2; ++Log; }
	P32(Out, 0x00010000); P16(Out, uint16(NT)); P16(Out, uint16(16 * Pow)); P16(Out, uint16(Log)); P16(Out, uint16(16 * NT - 16 * Pow));
	uint32 Off = 12 + 16 * NT;
	int64 HeadAt = -1;
	for (const FTable& X : Tables)
	{
		P32(Out, X.Tag); P32(Out, Sum(X.Data)); P32(Out, Off); P32(Out, X.Data.Num());
		Off += (X.Data.Num() + 3) & ~3;
	}
	for (const FTable& X : Tables)
	{
		if (X.Tag == Tag("head")) HeadAt = Out.Num();
		Out.Append(X.Data);
		while (Out.Num() % 4) Out.Add(0);
	}
	if (HeadAt >= 0) W32(Out, HeadAt + 8, 0xB1B0AFBAu - Sum(Out));
	return true;
}
