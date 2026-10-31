#pragma once

#include "CoreMinimal.h"
#include "Engine/Texture.h"
#include "OutlawsGeometry.h"

class FOutlawsAssets;
class UTexture2D;
namespace OutlawsFormats { struct F3doModel; struct FSprxFrame; }

/** Runtime textures, sprite atlases and 3DO mesh sections built from the player's LABs. */
namespace OutlawsTextures
{
	/** (TEXLOAD) A runtime texture's levels made ready on any thread (decode, mips, the disk cache), made a texture on the game thread. */
	struct FPreparedTex
	{
		int32 W = 0;
		int32 H = 0;
		EPixelFormat Format = PF_B8G8R8A8;
		TArray<TArray<uint8>> Mips;        // level 0 first
		TextureFilter Filter = TF_Default;
		TextureAddress Address = TA_Wrap;
		/** (BUG-MENUBLACK) keep the CPU copy after the upload: menu art, and anything that may be uploaded again */
		bool bKeepCpu = false;
		bool IsValid() const { return W > 0 && H > 0 && Mips.Num() > 0; }
	};
	OUTLAWSRUNTIME_API bool PrepareBGRA(int32 W, int32 H, const TArray<FColor>& Pixels, bool bMips, TextureFilter Filter, TextureAddress Address, FPreparedTex& Out);
	/** BC7 kept as is where the GPU has it; elsewhere (the Quest, or -OLCpuBC7) decoded to BGRA8 with mips, through Saved/TexCache. */
	OUTLAWSRUNTIME_API bool PrepareBC7(int32 W, int32 H, int32 Mips, const uint8* Data, int32 DataSize, TextureFilter Filter, FPreparedTex& Out);
	/** Game thread: the texture from prepared levels (which it empties). */
	OUTLAWSRUNTIME_API UTexture2D* CreatePrepared(FPreparedTex& P);
	/** Any thread: a level texture's PCX look (bWantLo) and HD look (bWantHd), with Info, from one read of its files.
	 *  (BUG-GLASS8BIT) Shades: the level's SHADES (FOutlawsLevel::Shades) for a see-through texture's PCX look, its entries
	 *  1 to 15 drawn as their colour at their strength; null: every entry but 0 solid. */
	OUTLAWSRUNTIME_API void PrepareLevelTexture(const FOutlawsAssets& A, const FString& Name, bool bWantLo, bool bWantHd, const TArray<FColor>& Palette,
		bool bLinear, FOutlawsTexInfo& Info, FPreparedTex& Lo, FPreparedTex& Hd, const TArray<FColor>* Shades = nullptr);
	/** (tests, BUG-GLASS8BIT) a PCX level texture's pixels as its PCX look is made (Shades as PrepareLevelTexture's). */
	OUTLAWSRUNTIME_API bool LevelPcxPixels(const FOutlawsAssets& A, const FString& Name, const TArray<FColor>& Palette, const TArray<FColor>* Shades, int32& OutW,
		int32& OutH, TArray<FColor>& OutPx);
	OUTLAWSRUNTIME_API bool CpuBC7();
	/** After a level build: one flush for the HD sprite atlases' draws, then their cell textures let go. */
	OUTLAWSRUNTIME_API void ReleaseAtlasCells();

	/** What runtime textures cost since the last reset (ms summed over the threads that did the work). */
	struct FLoadStats
	{
		int32 Built = 0;
		int32 FromCache = 0;
		double DecodeMs = 0.0, MipsMs = 0.0, UploadMs = 0.0, ReadMs = 0.0, CacheIoMs = 0.0;
	};
	OUTLAWSRUNTIME_API void ResetLoadStats();
	OUTLAWSRUNTIME_API FLoadStats GetLoadStats();
	OUTLAWSRUNTIME_API void AddReadCycles(uint64 Cycles);

	/** BGRA8 sRGB transient texture; optional alpha-weighted mip chain. */
	OUTLAWSRUNTIME_API UTexture2D* MakeTextureBGRA(int32 W, int32 H, const TArray<FColor>& Pixels, bool bMips, TextureFilter Filter, TextureAddress Address,
		bool bKeepCpu = false);

	/** BC7 transient texture from a remaster SPRX frame's colour layer (mip chain copied as is). */
	OUTLAWSRUNTIME_API UTexture2D* MakeTextureBC7(int32 W, int32 H, int32 Mips, const uint8* Data, int32 DataSize, TextureFilter Filter,
		TextureAddress Address = TA_Wrap, bool bKeepCpu = false);

	/**
	 * A level texture by its LVT name (PCX or ATX).  HD: the remaster's HDX for the resolved PCX
	 * stem when present.  Info gets the size used for UV maths (the PCX size; a 640x480 window
	 * canvas is cropped to the HDX content size) and the top-left anchoring rule.
	 */
	OUTLAWSRUNTIME_API UTexture2D* LoadLevelTexture(const FOutlawsAssets& A, const FString& Name, bool bHD, const TArray<FColor>& Palette,
		bool bLinear, FOutlawsTexInfo& Info, bool& bOutWasHd, const TArray<FColor>* Shades = nullptr);

	/**
	 * What kind of picture a sky is, from its PCX: a tiling sky (clouds all over, its top rows like its
	 * bottom rows, top row meeting bottom row) or a horizon sky (a horizon along its bottom, plain above),
	 * with the mean colour of its top rows.  Band: top rows against bottom rows (0..255); Seam: the top
	 * row against the bottom row; Adj: the mean row-to-row change.
	 */
	struct FOutlawsSkyLook
	{
		bool bTiles = true;
		FLinearColor Top = FLinearColor(0.07f, 0.12f, 0.31f);
		FColor TopRow = FColor(0, 0, 0, 0);      // (BUG-TRAINSKY8BIT) the mean of its top row of pixels alone, as drawn (sRGB); A 0 when not read
		float Band = 0.f, Seam = 0.f, Adj = 0.f;
	};
	OUTLAWSRUNTIME_API bool SkyLook(const FOutlawsAssets& A, const FString& Name, const TArray<FColor>& Palette, FOutlawsSkyLook& Out);

	struct FSpriteAtlas
	{
		UTexture2D* Texture = nullptr;
		int32 CellW = 0;
		int32 CellH = 0;
		int32 Cols = 1;
		int32 Rows = 1;
		int32 Cells = 0;
		int32 Dirs = 1;
		int32 Frames = 1;
		float FPS = 0.f;
		TArray<int32> Grid;   // Dirs x Frames cell indices
		/**
		 * Where each cell stands (user 2026-10-02: sprites floated).  A WAX frame is drawn with its insertion point
		 * (node offsets 0, 0) on the object's position, as the Jedi engine draws it (sprite_drawFrame: bottom = pos +
		 * offsetY), and most frames reach a few rows below it: the near feet of a figure drawn from a little above
		 * (the horse 1..9 px by view, Sanchez 8..13, the tarantula 17).  Per cell (1x px): BelowPx, rows of the cell
		 * under the insertion point (the cell sits bottom aligned); FootPx, rows of drawing under it (0 when the
		 * drawing ends above it: such a frame is lowered onto its feet, BGY1..3 draw theirs up to 2 px above).
		 */
		TArray<int16> BelowPx;
		TArray<int16> FootPx;
		/** R4 (the sprite-foot audit): twice the insertion point's offset right of the cell's centre, in 1x px (the
		 *  frame sits in its cell centred by its drawing's bounds, not by the insertion point, so a wide frame stood
		 *  beside its place: the horse's side view, a shooter's arm out). */
		TArray<int16> SidePx2;
		/** BUG-SCONCELOW: rows of the cell under the insertion point, signed and unclamped (negative when the frame ends
		 *  above it), for hanging sprites, which are not lowered onto their drawing's foot as BelowPx is. */
		TArray<int16> InsBottomPx;
		/** With bAllChors: every choreography (stand, walk, shoot, hit, die... for characters). */
		struct FChor
		{
			int32 Dirs = 1;
			int32 Frames = 1;          // longest row; shorter rows hold their last frame
			float FPS = 0.f;
			int32 GridStart = 0;       // into ChorGrid: Dirs x Frames cell indices
			int32 Length = 1;          // frames the first row really shows (a one-shot ends there)
			int32 HeightPx = 0;        // tallest frame it shows (a crouch is short): the body height it implies
			/** Opcodes in the first row: (frames shown before it, opcode, arg1, arg2). */
			TArray<FIntVector4> Events;
		};
		TArray<FChor> Chors;
		TArray<int32> ChorGrid;

		/** Where each NWX cell lands in the atlas (1x pixels), for the remaster's HD cells. */
		struct FNode { int32 Cell = 0; int32 X = 0; int32 Y = 0; int32 W = 0; int32 H = 0; bool bFlipX = false; bool bFlipY = false; };
		TArray<FNode> Nodes;
		FString Nwx;
		/** REQ-FIREBRIGHT: the drawn pixels of each palette entry over every frame shown (256), for the level's colour map to
		 *  say how much of the art is full bright (AOutlawsLevelActor::IsFullBrightArt). */
		TArray<uint32> IndexUse;
		/** REQ-FIREBRIGHT2: the atlas at 1x as palette entries (AtlasW x AtlasH, 0 where not drawn; with a texture only), for
		 *  the never-dim mask of the art drawn full bright (AOutlawsLevelActor::FireMaterialFor), and that mask once made:
		 *  nearest for the 8-bit look, filtered for the HD one (the same 1x mask scaled up). */
		TArray<uint8> Indices;
		UTexture* FireMask = nullptr;
		UTexture* FireMaskHd = nullptr;
		/** REQ-FIREBRIGHT3: FireMaskHd is the remaster's own full-bright layer (DecodeSpriteAtlasFullBrightMask), once tried. */
		bool bFireMaskLayer = false;
		bool bFireMaskLayerTried = false;
		/** The atlas's opacity at 1x (AtlasW x AtlasH, 1 where drawn): where a held figure's neck is (user 2026-09-29). */
		TArray<uint8> Alpha;
		int32 AtlasW = 0, AtlasH = 0;
		/** The same atlas from the remaster's SPX cells (a render target at HdScale x), once built. */
		UTexture* TextureHd = nullptr;
		int32 HdScale = 0;
		bool bHdTried = false;
	};

	/** Atlas of every frame choreography 0 shows, over all directions (reference: SpriteCache._render);
	 *  bAllChors packs every choreography's frames into the same atlas (characters). */
	OUTLAWSRUNTIME_API TSharedPtr<FSpriteAtlas> BuildSpriteAtlas(const FOutlawsAssets& A, const FString& NwxName, const TArray<FColor>& Palette,
		bool bCreateTexture = true, bool bAllChors = false);

	/**
	 * The HD look of an atlas: every cell's remaster SPX frame (BC7, 4x) drawn where the NWX cell
	 * sits, into a render target of the same layout (so sprite UVs do not change) at up to 4x, less
	 * for big atlases.  Null when the SPX is missing or incomplete, or nothing renders (-nullrhi).
	 */
	OUTLAWSRUNTIME_API UTexture* BuildSpriteAtlasHD(UObject* Outer, const FOutlawsAssets& A, FSpriteAtlas& Atlas);

	/**
	 * The same HD look on the CPU (the held guns' textures, user 2026-10-02): every cell's SPX frame (BC7 mip 0)
	 * at 4x where its NWX cell sits, flipped as the node says, its texels with alpha drawn over a transparent black
	 * atlas (as olue_export.py --hd writes sprites_hd).  False when the SPX is missing or incomplete, or a frame
	 * is not 4x its cell.
	 */
	OUTLAWSRUNTIME_API bool DecodeSpriteAtlasHdPixels(const FOutlawsAssets& A, const FSpriteAtlas& Atlas, TArray<FColor>& Out, int32& OutW, int32& OutH);

	/** REQ-FIREBRIGHT3: the HD look's fire mask from the remaster's own full-bright layer (each SPX frame's second layer, Flags
	 *  bit 0), laid out as DecodeSpriteAtlasHdPixels lays the colour: 0 where the layer keeps a texel bright, 255 where it
	 *  dims with the light.  False when a node's frame has no such layer (the 8-bit colours' mask stands); OutLitPx: the
	 *  texels the layer keeps bright. */
	OUTLAWSRUNTIME_API bool DecodeSpriteAtlasFullBrightMask(const FOutlawsAssets& A, const FSpriteAtlas& Atlas, TArray<FColor>& Out, int32& OutW, int32& OutH,
		int32* OutLitPx = nullptr);

	/** (REQ-MUZZLEFLASH3D) One remaster SPX cell (BC7 mip 0) on the CPU, as it is stored (no flip), from the SPX file's
	 *  bytes and ParseSprx's frames.  False when the cell is missing or not BC7. */
	OUTLAWSRUNTIME_API bool DecodeSpxCellPixels(const TArray<uint8>& Spx, const TArray<OutlawsFormats::FSprxFrame>& Frames, int32 Cell, TArray<FColor>& Out, int32& OutW, int32& OutH);

	/** 3DO model -> mesh sections (textures loaded into Textures by stem; flat colours use PALETTE). */
	OUTLAWSRUNTIME_API void Build3doSections(const FOutlawsAssets& A, const OutlawsFormats::F3doModel& M, int32 Light, bool bHD,
		const TArray<FColor>& Palette, bool bReverse, TMap<FString, TObjectPtr<UTexture2D>>& Textures, TMap<FString, FOutlawsMeshSection>& Out);
}
