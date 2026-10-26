#pragma once

#include "CoreMinimal.h"

class FOutlawsAssets;

/**
 * Decoders for the Outlaws data formats the level needs.  Each is a port of the Python
 * reference in tools/olue (olue_export.py) and tools/wax_lib.py; keep them in step.
 */
namespace OutlawsFormats
{
	// ---- PCX (8-bit paletted, RLE) ---------------------------------------------------------
	struct FPcxImage
	{
		int32 W = 0;
		int32 H = 0;
		TArray<uint8> Indices;       // W*H, top-left origin
		TArray<FColor> Palette;      // 256 entries when the file has a VGA palette
	};
	OUTLAWSRUNTIME_API bool DecodePcx(const TArray<uint8>& Data, FPcxImage& Out);

	// ---- LAF (the game's bitmap fonts, "NFNT": mf3s.laf, text.laf...) ----------------------------
	struct FLafGlyph
	{
		int32 Advance = 0;           // pen step to the next letter
		int32 OffX = 0;              // the bitmap's left from the pen
		int32 OffY = 0;              // the bitmap's top from the baseline (negative: above it)
		int32 W = 0;
		int32 H = 0;
		int32 DataOffset = 0;        // into FLafFont::Data, W*H bytes, nonzero = ink
	};
	struct FLafFont
	{
		int32 Height = 0;            // the line height
		int32 Ascent = 0;            // top of the line to the baseline
		int32 FirstChar = 0;
		TArray<uint16> Index;        // char - FirstChar -> glyph
		TArray<FLafGlyph> Glyphs;
		TArray<uint8> Data;
		/** The glyph for a latin-1 letter, or null when the font has none. */
		const FLafGlyph* Find(TCHAR C) const
		{
			const int32 i = int32(C) - FirstChar;
			const int32 g = Index.IsValidIndex(i) ? int32(Index[i]) : -1;
			return Glyphs.IsValidIndex(g) ? &Glyphs[g] : nullptr;
		}
	};
	OUTLAWSRUNTIME_API bool DecodeLaf(const TArray<uint8>& Data, FLafFont& Out);

	/** ATX (animated texture script) -> the PCX named by its first TEXTURE line; PCX names pass through. */
	OUTLAWSRUNTIME_API FString ResolveTextureName(const FOutlawsAssets& Assets, const FString& Name);

	/** The 256-colour palette sprites and 3DO colours use (read from PCX files known to carry it). */
	OUTLAWSRUNTIME_API bool FindSpritePalette(const FOutlawsAssets& Assets, TArray<FColor>& Out);

	// ---- WAX / NWX sprites -------------------------------------------------------------------
	constexpr uint32 NodeFlipX = 0x100;
	constexpr uint32 NodeFlipY = 0x200;

	struct FWaxCell { int32 W = 0; int32 H = 0; uint32 Flags = 0; int32 DataOffset = 0; int32 DataSize = 0; };
	struct FWaxNode { int32 Cell = 0; int32 OX = 0; int32 OY = 0; uint32 Flags = 0; };
	struct FWaxFrame { TArray<FWaxNode> Nodes; };
	struct FWaxChor { int32 Dirs = 1; int32 Frames = 1; float FPS = 0.f; TArray<int16> Grid; };
	struct FWax
	{
		TArray<uint8> Data;
		TArray<FWaxCell> Cells;
		TArray<FWaxFrame> Frames;
		TArray<FWaxChor> Chors;
	};
	OUTLAWSRUNTIME_API bool ParseWax(TArray<uint8>&& Data, FWax& Out);
	/** Palette indices W*H, top-left origin. */
	OUTLAWSRUNTIME_API bool DecodeWaxCell(const FWax& Wax, int32 CellIndex, TArray<uint8>& Out);
	/** Frames shown by one choreography row: >= 0 frame, negative opcode + 2 args, "-1 0" ends. */
	OUTLAWSRUNTIME_API void ChorFrames(const int16* Row, int32 N, TArray<int32>& Out);

	// ---- SPRX (remaster HDX / SPX containers) ------------------------------------------------
	constexpr uint32 SprxRGBA = 1;
	constexpr uint32 SprxBC7 = 33;
	struct FSprxFrame { uint32 Format = 0; uint32 Flags = 0; int32 W = 0; int32 H = 0; int32 Mips = 1; int32 Offset = 0; int32 End = 0; };
	OUTLAWSRUNTIME_API bool ParseSprx(const TArray<uint8>& Data, TArray<FSprxFrame>& Out);
	/** Byte size of one BC7 mip level. */
	inline int32 Bc7MipBytes(int32 W, int32 H) { return FMath::Max(1, (W + 3) / 4) * FMath::Max(1, (H + 3) / 4) * 16; }

	// ---- 3DO meshes -----------------------------------------------------------------------------
	struct F3doFace { TArray<int32, TInlineAllocator<4>> V; TArray<int32, TInlineAllocator<4>> T; int32 Color = 0; bool bTextured = false; };
	struct F3doObject { int32 Texture = -1; TArray<FVector3f> Verts; TArray<FVector2f> TVerts; TArray<F3doFace> Faces; };
	struct F3doModel { TArray<FString> Textures; TArray<F3doObject> Objects; };
	OUTLAWSRUNTIME_API bool Parse3do(const FString& Text, F3doModel& Out);

	// ---- ITM resolution --------------------------------------------------------------------------
	/**
	 * The animation (NWX or 3DO) an OBT object shows: the ITM's ANIM line, else an ITM it refers
	 * to, else (no ITM at all) <name>.nwx / <name>.3do.  Empty when there is nothing to show.
	 */
	OUTLAWSRUNTIME_API FString ResolveAnim(const FOutlawsAssets& Assets, const FString& Name, FString* OutItm = nullptr, int32 Depth = 0);
	/** Generator item (FUNC Gen_*): true, with the GENERATE target in OutGenerate (may be empty). */
	OUTLAWSRUNTIME_API bool GeneratorTarget(const FString& ItmText, FString& OutGenerate);
	/** An ITM's FUNC and DATA values ("STR PICKUP ibrsskey" -> Data["PICKUP"] = "ibrsskey"); keys upper case. */
	OUTLAWSRUNTIME_API void ParseItm(const FString& ItmText, FString& OutFunc, TMap<FString, FString>& OutData);
	/** Objects hung from the ceiling (lamps, chandeliers, banners) anchor at the sprite top. */
	OUTLAWSRUNTIME_API bool IsHanging(const FString& Name, const FString& ItmText);

	/** One ATX (animated texture script) instruction. */
	struct FOutlawsAtxInstr
	{
		enum class EKind : uint8 { Rate, Texture, Stop, Goto, Sound };
		EKind Kind = EKind::Stop;
		float Value = 0.f;      // RATE: frames a second; GOTO: the instruction index (0-based, RATE counts, the INST line not)
		FString Name;           // TEXTURE: its file; START_SOUND: the wav
	};
	/** ATX 1.0: "INST n" then instructions (RATE r | TEXTURE x.pcx | STOP | GOTO n | START_SOUND vol name).  The program
	 *  runs from 0; a TEXTURE shows for 1/RATE s; a STOP holds until the wall gets an INF NEXT_STOP (on past it) or
	 *  GOTO_STOP n (from instruction n).  cliff.inf's KEYCODE walls: GOTO_STOP 4 / 10 / 16 land on dasqrkey.atx's
	 *  key A / C / E triples (START_SOUND, TEXTURE, STOP), which fixes the indexing.  bOutHasStop: none means it runs
	 *  on its own (water, the train's wheels).  User 2026-09-29. */
	OUTLAWSRUNTIME_API bool ParseAtx(const FOutlawsAssets& Assets, const FString& Name, TArray<FOutlawsAtxInstr>& Out, bool& bOutHasStop);
}
