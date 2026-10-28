#include "OutlawsTextures.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsLab.h"
#include "OutlawsFormats.h"
#include "OutlawsPerf.h"
#include "Async/ParallelFor.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "PixelFormat.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Canvas.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "RenderingThread.h"
#include "Hash/CityHash.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include <atomic>

using namespace OutlawsFormats;

namespace
{
	/** 2x2 box filter; colours weighted by alpha so transparent texels do not bleed their colour. */
	void Downsample(const TArray<FColor>& Src, int32 W, int32 H, TArray<FColor>& Dst, int32& OW, int32& OH)
	{
		OW = FMath::Max(1, W / 2);
		OH = FMath::Max(1, H / 2);
		Dst.SetNumUninitialized(OW * OH);
		const int32 DW = OW, DH = OH;
		// rows across the cores for the big ones: a level start builds a hundred 1024 px mip chains (Quest load time)
		ParallelFor(DH, [&Src, &Dst, W, H, DW](int32 y)
		{
			for (int32 x = 0; x < DW; ++x)
			{
				uint32 R = 0, G = 0, B = 0, Aw = 0, ASum = 0, N = 0;
				for (int32 dy = 0; dy < 2; ++dy)
				{
					for (int32 dx = 0; dx < 2; ++dx)
					{
						const int32 sx = FMath::Min(W - 1, x * 2 + dx);
						const int32 sy = FMath::Min(H - 1, y * 2 + dy);
						const FColor& C = Src[sy * W + sx];
						R += C.R * C.A; G += C.G * C.A; B += C.B * C.A; Aw += C.A;
						ASum += C.A;
						++N;
					}
				}
				FColor& O = Dst[y * DW + x];
				if (Aw > 0)
				{
					O.R = uint8(R / Aw); O.G = uint8(G / Aw); O.B = uint8(B / Aw);
				}
				else
				{
					O.R = O.G = O.B = 0;
				}
				O.A = uint8(ASum / N);
			}
		}, DW * DH < 16384);
	}

	void SetMipData(FTexture2DMipMap& Mip, const void* Data, int64 Bytes)
	{
		Mip.BulkData.Lock(LOCK_READ_WRITE);
		void* Dst = Mip.BulkData.Realloc(Bytes);
		FMemory::Memcpy(Dst, Data, Bytes);
		Mip.BulkData.Unlock();
	}

	TArray<FColor> PcxToBGRA(const FPcxImage& Img, const TArray<FColor>& Fallback, const TArray<FColor>* Shades = nullptr)
	{
		const TArray<FColor>& Pal = Img.Palette.Num() == 256 ? Img.Palette : Fallback;
		TArray<FColor> Px;
		Px.SetNumUninitialized(Img.W * Img.H);
		for (int32 i = 0; i < Px.Num(); ++i)
		{
			const uint8 Ix = Img.Indices[i];
			FColor C = Pal.IsValidIndex(Ix) ? Pal[Ix] : FColor::Black;
			C.A = Ix == 0 ? 0 : 255;       // index 0 transparent (masked materials use it; opaque ones ignore alpha)
			// (BUG-GLASS8BIT) a see-through texture's shade entries (the level's SHADES, 1 to 15): their colour at their strength,
			// as the original draws a broken window's shards over the room behind
			if (Shades && Shades->IsValidIndex(Ix) && (*Shades)[Ix].A > 0) C = (*Shades)[Ix];
			Px[i] = C;
		}
		return Px;
	}

	// BC7 on the CPU, for GPUs without it (the Quest's Adreno): the remaster's HDX, SPRX and menu DDS art is all BC7.
	// Tables from the D3D11 BC7 spec; checked block for block against Pillow over every mode and partition.
	namespace Bc7
	{
		const uint16 P2[64] = {
			0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000,
			0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C,
			0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660,
			0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6, 0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22};
		const char* const P3[64] = {
			"0011001102212222", "0001001122112221", "0000200122112211", "0222002200110111", "0000000011221122", "0011001100220022",
			"0022002211111111", "0011001122112211", "0000000011112222", "0000111111112222", "0000111122222222", "0012001200120012",
			"0112011201120112", "0122012201220122", "0011011211221222", "0011200122002220", "0001001101121122", "0111001120012200",
			"0000112211221122", "0022002200221111", "0111011102220222", "0001000122212221", "0000001101220122", "0000110022102210",
			"0122012200110000", "0012001211222222", "0110122112210110", "0000011012211221", "0022110211020022", "0110011020022222",
			"0011012201220011", "0000200022112221", "0000000211221222", "0222002200120011", "0011001200220222", "0120012001200120",
			"0000111122220000", "0120120120120120", "0120201212010120", "0011220011220011", "0011112222000011", "0101010122222222",
			"0000000021212121", "0022112200221122", "0022001100220011", "0220122102201221", "0101222222220101", "0000212121212121",
			"0101010101012222", "0222011102220111", "0002111200021112", "0000211221122112", "0222011101110222", "0002111211120002",
			"0110011001102222", "0000000021122112", "0110011022222222", "0022001100110022", "0022112211220022", "0000000000002112",
			"0002000100020001", "0222122202221222", "0101222222222222", "0111201122012220"};
		const uint8 A2[64] = {
			15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
			15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6, 6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15};
		const uint8 A3a[64] = {
			3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3, 3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15,
			8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15, 3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3};
		const uint8 A3b[64] = {
			15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8, 15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8,
			15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8, 15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8};
		const uint8 W2[4] = {0, 21, 43, 64};
		const uint8 W3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
		const uint8 W4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
		// subsets, partition bits, rotation bits, index-select bits, colour bits, alpha bits, endpoint p-bits,
		// shared p-bits, index bits, second index bits
		struct FMode { uint8 NS, PB, RB, ISB, CB, AB, EPB, SPB, IB, IB2; };
		const FMode Modes[8] = {
			{3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0}, {2, 6, 0, 0, 7, 0, 1, 0, 2, 0},
			{1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2}, {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0}};

		const uint8* Weights(int32 Bits) { return Bits == 2 ? W2 : Bits == 3 ? W3 : W4; }

		/** The block's 128 bits as two little-endian words; fields read LSB first, never wider than 8 bits. */
		struct FBits
		{
			uint64 Lo = 0, Hi = 0;
			int32 Pos = 0;
			FBits(const uint8* B, int32 Start) : Pos(Start)
			{
				FMemory::Memcpy(&Lo, B, 8);
				FMemory::Memcpy(&Hi, B + 8, 8);
			}
			uint32 Read(int32 N)
			{
				if (N <= 0) return 0;
				const uint64 V = Pos >= 64 ? Hi >> (Pos - 64) : Pos == 0 ? Lo : (Lo >> Pos) | (Hi << (64 - Pos));
				Pos += N;
				return uint32(V & ((uint64(1) << N) - 1));
			}
		};

		int32 SubsetOf(int32 NS, int32 Part, int32 i)
		{
			return NS == 1 ? 0 : NS == 2 ? (P2[Part] >> i) & 1 : P3[Part][i] - '0';
		}

		bool IsAnchor(int32 NS, int32 Part, int32 S, int32 i)
		{
			if (S == 0) return i == 0;
			return NS == 2 ? i == A2[Part] : i == (S == 1 ? A3a[Part] : A3b[Part]);
		}

		/** One 4x4 block to 16 texels, row-major.  An invalid block (no mode bit) is transparent black. */
		void DecodeBlock(const uint8* Block, FColor* Out)
		{
			int32 Mode = 0;
			while (Mode < 8 && !(Block[0] & (1 << Mode))) ++Mode;
			if (Mode == 8)
			{
				for (int32 i = 0; i < 16; ++i) Out[i] = FColor(0, 0, 0, 0);
				return;
			}
			const FMode& M = Modes[Mode];
			FBits R(Block, Mode + 1);
			const int32 Part = R.Read(M.PB), Rot = R.Read(M.RB), ISel = R.Read(M.ISB);
			uint8 Ep[3][2][4] = {};
			for (int32 c = 0; c < 3; ++c)
				for (int32 s = 0; s < M.NS; ++s)
					for (int32 e = 0; e < 2; ++e) Ep[s][e][c] = uint8(R.Read(M.CB));
			if (M.AB)
				for (int32 s = 0; s < M.NS; ++s)
					for (int32 e = 0; e < 2; ++e) Ep[s][e][3] = uint8(R.Read(M.AB));
			int32 Cb = M.CB, Ab = M.AB;
			if (M.EPB)
			{
				for (int32 s = 0; s < M.NS; ++s)
					for (int32 e = 0; e < 2; ++e)
					{
						const uint32 P = R.Read(1);
						for (int32 c = 0; c < (M.AB ? 4 : 3); ++c) Ep[s][e][c] = uint8((Ep[s][e][c] << 1) | P);
					}
				++Cb;
				if (M.AB) ++Ab;
			}
			if (M.SPB)
			{
				for (int32 s = 0; s < M.NS; ++s)
				{
					const uint32 P = R.Read(1);
					for (int32 e = 0; e < 2; ++e)
						for (int32 c = 0; c < 3; ++c) Ep[s][e][c] = uint8((Ep[s][e][c] << 1) | P);
				}
				++Cb;
			}
			for (int32 s = 0; s < M.NS; ++s)
				for (int32 e = 0; e < 2; ++e)
				{
					for (int32 c = 0; c < 3; ++c)
					{
						const uint32 V = uint32(Ep[s][e][c]) << (8 - Cb);
						Ep[s][e][c] = uint8(V | (V >> Cb));
					}
					if (Ab)
					{
						const uint32 V = uint32(Ep[s][e][3]) << (8 - Ab);
						Ep[s][e][3] = uint8(V | (V >> Ab));
					}
					else
					{
						Ep[s][e][3] = 255;
					}
				}
			uint8 Idx[16], Idx2[16] = {};
			for (int32 i = 0; i < 16; ++i)
			{
				const int32 S = SubsetOf(M.NS, Part, i);
				Idx[i] = uint8(R.Read(IsAnchor(M.NS, Part, S, i) ? M.IB - 1 : M.IB));
			}
			if (M.IB2)
				for (int32 i = 0; i < 16; ++i) Idx2[i] = uint8(R.Read(i == 0 ? M.IB2 - 1 : M.IB2));
			for (int32 i = 0; i < 16; ++i)
			{
				const int32 S = SubsetOf(M.NS, Part, i);
				int32 Ci = Idx[i], Ai = Idx[i];
				const uint8* Cw = Weights(M.IB);
				const uint8* Aw = Cw;
				if (M.IB2)
				{
					if (ISel) { Ci = Idx2[i]; Cw = Weights(M.IB2); }
					else { Ai = Idx2[i]; Aw = Weights(M.IB2); }
				}
				uint8 Px[4];
				for (int32 c = 0; c < 3; ++c) Px[c] = uint8(((64 - Cw[Ci]) * Ep[S][0][c] + Cw[Ci] * Ep[S][1][c] + 32) >> 6);
				Px[3] = uint8(((64 - Aw[Ai]) * Ep[S][0][3] + Aw[Ai] * Ep[S][1][3] + 32) >> 6);
				if (Rot) Swap(Px[Rot - 1], Px[3]);
				Out[i] = FColor(Px[0], Px[1], Px[2], Px[3]);
			}
		}

		/** A whole BC7 mip level to texels (W x H, edge blocks clipped). */
		void DecodeImage(const uint8* Data, int32 W, int32 H, TArray<FColor>& Out)
		{
			Out.SetNumUninitialized(W * H);
			const int32 Bw = (W + 3) / 4, Bh = (H + 3) / 4;
			FColor* Px = Out.GetData();
			auto Row = [Data, W, H, Bw, Px](int32 by)
			{
				FColor Texels[16];
				for (int32 bx = 0; bx < Bw; ++bx)
				{
					DecodeBlock(Data + (by * Bw + bx) * 16, Texels);
					for (int32 i = 0; i < 16; ++i)
					{
						const int32 x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
						if (x < W && y < H) Px[y * W + x] = Texels[i];
					}
				}
			};
			// a level start decodes a thousand of these at once (the Quest's 7 s stall): big ones across the cores
			ParallelFor(Bh, Row, Bw * Bh < 1024);
		}
	}

	TArray<FColor> SpriteLut(const TArray<FColor>& Palette)
	{
		TArray<FColor> Lut;
		Lut.SetNum(256);
		for (int32 i = 0; i < 256; ++i)
		{
			if (i == 0) Lut[i] = FColor(0, 0, 0, 0);
			else if (i <= 13) Lut[i] = FColor(0, 0, 0, 90);   // shade indices
			else { Lut[i] = Palette.IsValidIndex(i) ? Palette[i] : FColor::Black; Lut[i].A = 255; }
		}
		return Lut;
	}
}

bool OutlawsTextures::SkyLook(const FOutlawsAssets& A, const FString& Name, const TArray<FColor>& Palette, FOutlawsSkyLook& Out)
{
	const FString Resolved = ResolveTextureName(A, Name);
	TArray<uint8> Bytes;
	FPcxImage Img;
	if (Resolved.IsEmpty() || !A.Get(Resolved, Bytes) || !DecodePcx(Bytes, Img) || Img.W <= 0 || Img.H < 2)
	{
		return false;
	}
	const TArray<FColor> Px = PcxToBGRA(Img, Palette);
	const int32 W = Img.W, H = Img.H;
	auto RowMean = [&](int32 Y)
	{
		FVector M(0.0);
		for (int32 x = 0; x < W; ++x) { const FColor& C = Px[Y * W + x]; M += FVector(C.R, C.G, C.B); }
		return M / W;
	};
	auto RowDiff = [&](int32 Y0, int32 Y1)
	{
		double D = 0.0;
		for (int32 x = 0; x < W; ++x)
		{
			const FColor& a = Px[Y0 * W + x];
			const FColor& b = Px[Y1 * W + x];
			D += FMath::Abs(int32(a.R) - b.R) + FMath::Abs(int32(a.G) - b.G) + FMath::Abs(int32(a.B) - b.B);
		}
		return D / (3.0 * W);
	};
	const int32 Band = FMath::Max(1, H / 16);
	FVector Top(0.0), Bot(0.0);
	for (int32 y = 0; y < Band; ++y) { Top += RowMean(y); Bot += RowMean(H - 1 - y); }
	Top /= Band;
	Bot /= Band;
	Out.Band = float((FMath::Abs(Top.X - Bot.X) + FMath::Abs(Top.Y - Bot.Y) + FMath::Abs(Top.Z - Bot.Z)) / 3.0);
	Out.Seam = float(RowDiff(0, H - 1));
	double Adj = 0.0;
	for (int32 y = 0; y + 1 < H; ++y) Adj += RowDiff(y, y + 1);
	Out.Adj = float(Adj / (H - 1));
	// a horizon sky: its top rows unlike its bottom rows, or a top row that meets its bottom row with a step
	// far beyond the picture's own row-to-row change (the story skies: hideout, cliff, gulch, night Simms;
	// the tiling ones: town, canyon, mill, mine)
	Out.bTiles = !(Out.Band > 20.f || Out.Seam > 20.f || (Out.Seam > 8.f && Out.Seam > 8.f * Out.Adj));
	Out.Top = FLinearColor(FColor(uint8(Top.X), uint8(Top.Y), uint8(Top.Z)));    // sRGB to linear
	const FVector Row0 = RowMean(0);
	Out.TopRow = FColor(uint8(FMath::RoundToInt(Row0.X)), uint8(FMath::RoundToInt(Row0.Y)), uint8(FMath::RoundToInt(Row0.Z)), 255);
	return true;
}

namespace
{
	// (TEXLOAD) what a level load spent on runtime textures, summed over the threads that did it
	std::atomic<int32> StatBuilt{0}, StatFromCache{0};
	std::atomic<uint64> StatDecode{0}, StatMips{0}, StatUpload{0}, StatRead{0}, StatCacheIo{0};

	/** Time into a counter; on the game thread into the perf bucket too (workers' time is not the frame's). */
	struct FTexTimer
	{
		std::atomic<uint64>& Acc;
		int32 Bucket;
		uint64 T0;
		FTexTimer(std::atomic<uint64>& InAcc, int32 InBucket) : Acc(InAcc), Bucket(InBucket), T0(FPlatformTime::Cycles64()) {}
		~FTexTimer()
		{
			const uint64 D = FPlatformTime::Cycles64() - T0;
			Acc.fetch_add(D);
			if (Bucket >= 0) OutlawsPerf::Add(Bucket, D);
		}
	};

	struct FTexBuckets
	{
		int32 Decode, Mips, Upload;
		FTexBuckets()
			: Decode(OutlawsPerf::Bucket(TEXT("Textures: BC7 CPU decode")))
			, Mips(OutlawsPerf::Bucket(TEXT("Textures: mips")))
			, Upload(OutlawsPerf::Bucket(TEXT("Textures: create + upload")))
		{
		}
	};
	const FTexBuckets& TexBuckets()
	{
		static const FTexBuckets B;
		return B;
	}

	// (TEXLOAD) the disk cache of CPU-decoded BC7 (the Quest has no BC7): Saved/TexCache/<hash>.oltc, keyed by the
	// compressed source's bytes, its size, mip count, the size cap and this format's version
	constexpr uint32 CacheMagic = 0x43544C4F;     // "OLTC"
	constexpr uint32 CacheVersion = 1;

	bool CacheOn()
	{
		// opt-in (-OLTexCache): measured 2026-10-06 on the desktop it wrote 681 MB for three levels and read back slower
		// than the decode it saved (the decode across the cores is the cheaper road)
		static const bool bOn = OLFlags::Param(TEXT("OLTexCache")) && !OLFlags::Param(TEXT("OLNoTexCache"));
		return bOn;
	}

	FString CachePath(uint64 Key)
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("TexCache"), FString::Printf(TEXT("%016llx.oltc"), Key));
	}

	bool CacheLoad(uint64 Key, OutlawsTextures::FPreparedTex& Out)
	{
		TArray<uint8> F;
		{
			FTexTimer T(StatCacheIo, -1);
			if (!FFileHelper::LoadFileToArray(F, *CachePath(Key), FILEREAD_Silent)) return false;
		}
		if (F.Num() < 20) return false;
		const uint32* Hd = reinterpret_cast<const uint32*>(F.GetData());
		if (Hd[0] != CacheMagic || Hd[1] != CacheVersion) return false;
		const int32 W = int32(Hd[2]), H = int32(Hd[3]), N = int32(Hd[4]);
		if (W <= 0 || H <= 0 || N <= 0 || N > 16) return false;
		int64 Off = 20;
		Out.Mips.SetNum(N);
		for (int32 m = 0; m < N; ++m)
		{
			if (Off + 4 > F.Num()) return false;
			const uint32 Bytes = *reinterpret_cast<const uint32*>(F.GetData() + Off);
			Off += 4;
			if (Off + int64(Bytes) > int64(F.Num())) return false;
			Out.Mips[m].SetNumUninitialized(Bytes);
			FMemory::Memcpy(Out.Mips[m].GetData(), F.GetData() + Off, Bytes);
			Off += Bytes;
		}
		Out.W = W;
		Out.H = H;
		Out.Format = PF_B8G8R8A8;
		return true;
	}

	void CacheSave(uint64 Key, const OutlawsTextures::FPreparedTex& P)
	{
		FTexTimer T(StatCacheIo, -1);
		TArray<uint8> F;
		int64 Total = 20;
		for (const TArray<uint8>& M : P.Mips) Total += 4 + M.Num();
		F.Reserve(Total);
		const uint32 Hd[5] = { CacheMagic, CacheVersion, uint32(P.W), uint32(P.H), uint32(P.Mips.Num()) };
		F.Append(reinterpret_cast<const uint8*>(Hd), sizeof(Hd));
		for (const TArray<uint8>& M : P.Mips)
		{
			const uint32 Bytes = M.Num();
			F.Append(reinterpret_cast<const uint8*>(&Bytes), 4);
			F.Append(M);
		}
		// written under a name of its own and moved into place, so a reader never meets half a file
		const FString Final = CachePath(Key);
		const FString Tmp = Final + FString::Printf(TEXT(".%u.tmp"), FPlatformTLS::GetCurrentThreadId());
		if (FFileHelper::SaveArrayToFile(F, *Tmp)) IFileManager::Get().Move(*Final, *Tmp, true, true, false, true);
	}

	/** Copies a BGRA picture into a prepared texture's first level, with its alpha-weighted mip chain under it. */
	void FillBGRA(int32 W, int32 H, const TArray<FColor>& Pixels, bool bMips, OutlawsTextures::FPreparedTex& Out)
	{
		Out.W = W;
		Out.H = H;
		Out.Format = PF_B8G8R8A8;
		Out.Mips.Reset();
		Out.Mips.AddDefaulted();
		Out.Mips[0].SetNumUninitialized(int64(W) * H * 4);
		FMemory::Memcpy(Out.Mips[0].GetData(), Pixels.GetData(), int64(W) * H * 4);
		if (!bMips) return;
		FTexTimer T(StatMips, IsInGameThread() ? TexBuckets().Mips : -1);
		TArray<FColor> Prev, Next;
		const TArray<FColor>* Src = &Pixels;
		int32 PW = W, PH = H;
		while (PW > 1 || PH > 1)
		{
			int32 NW, NH;
			Downsample(*Src, PW, PH, Next, NW, NH);
			TArray<uint8>& Mip = Out.Mips.AddDefaulted_GetRef();
			Mip.SetNumUninitialized(int64(NW) * NH * 4);
			FMemory::Memcpy(Mip.GetData(), Next.GetData(), int64(NW) * NH * 4);
			Swap(Prev, Next);
			Src = &Prev;
			PW = NW;
			PH = NH;
		}
	}
}

namespace
{
	TArray<TStrongObjectPtr<UTexture2D>> PendingCells;
}

void OutlawsTextures::ReleaseAtlasCells()
{
	if (PendingCells.Num() == 0) return;
	FlushRenderingCommands();
	PendingCells.Empty();
}

bool OutlawsTextures::CpuBC7()
{
	// (TEXLOAD) -OLCpuBC7: the Quest's path on the desktop (decode, mips, cache), to measure it
	static const bool bForce = OLFlags::Param(TEXT("OLCpuBC7"));
	return bForce || !GPixelFormats[PF_BC7].Supported;
}

void OutlawsTextures::ResetLoadStats()
{
	TexBuckets();
	StatBuilt = 0; StatFromCache = 0;
	StatDecode = 0; StatMips = 0; StatUpload = 0; StatRead = 0; StatCacheIo = 0;
}

void OutlawsTextures::AddReadCycles(uint64 Cycles)
{
	StatRead.fetch_add(Cycles);
}

OutlawsTextures::FLoadStats OutlawsTextures::GetLoadStats()
{
	FLoadStats S;
	S.Built = StatBuilt.load();
	S.FromCache = StatFromCache.load();
	S.DecodeMs = FPlatformTime::ToMilliseconds64(StatDecode.load());
	S.MipsMs = FPlatformTime::ToMilliseconds64(StatMips.load());
	S.UploadMs = FPlatformTime::ToMilliseconds64(StatUpload.load());
	S.ReadMs = FPlatformTime::ToMilliseconds64(StatRead.load());
	S.CacheIoMs = FPlatformTime::ToMilliseconds64(StatCacheIo.load());
	return S;
}

bool OutlawsTextures::PrepareBGRA(int32 W, int32 H, const TArray<FColor>& Pixels, bool bMips, TextureFilter Filter, TextureAddress Address, FPreparedTex& Out)
{
	Out = FPreparedTex();
	if (W <= 0 || H <= 0 || Pixels.Num() < W * H)
	{
		return false;
	}
	FillBGRA(W, H, Pixels, bMips, Out);
	Out.Filter = Filter;
	Out.Address = Address;
	return true;
}

bool OutlawsTextures::PrepareBC7(int32 W, int32 H, int32 Mips, const uint8* Data, int32 DataSize, TextureFilter Filter, FPreparedTex& Out)
{
	Out = FPreparedTex();
	if (W <= 0 || H <= 0)
	{
		return false;
	}
	if (CpuBC7())
	{
		// decode on the CPU to BGRA8, which costs 4x BC7's memory, so start from a smaller mip when the chain has
		// one and halve what is still too big (1024 for world and sprite chains, 2048 for single-level menu art)
		const int32 MaxDim = Mips > 1 ? 1024 : 2048;
		int32 Off = 0, MW = W, MH = H, m = 0;
		while (m + 1 < Mips && FMath::Max(MW, MH) > MaxDim)
		{
			Off += Bc7MipBytes(MW, MH);
			MW = FMath::Max(1, MW / 2);
			MH = FMath::Max(1, MH / 2);
			++m;
		}
		if (Off + Bc7MipBytes(MW, MH) > DataSize)
		{
			return false;
		}
		uint64 Key = 0;
		if (CacheOn())
		{
			Key = CityHash64(reinterpret_cast<const char*>(Data + Off), Bc7MipBytes(MW, MH));
			Key = CityHash128to64(Uint128_64(Key, (uint64(MW) << 40) ^ (uint64(MH) << 20) ^ (uint64(Mips > 1 ? 1 : 0) << 8) ^ (uint64(MaxDim) << 1) ^ CacheVersion));
			if (CacheLoad(Key, Out))
			{
				Out.Filter = Filter;
				Out.Address = TA_Wrap;
				StatFromCache.fetch_add(1);
				return true;
			}
		}
		TArray<FColor> Px, Half;
		{
			FTexTimer T(StatDecode, IsInGameThread() ? TexBuckets().Decode : -1);
			Bc7::DecodeImage(Data + Off, MW, MH, Px);
			while (FMath::Max(MW, MH) > MaxDim)
			{
				Downsample(Px, MW, MH, Half, MW, MH);
				Swap(Px, Half);
			}
		}
		FillBGRA(MW, MH, Px, Mips > 1, Out);
		Out.Filter = Filter;
		Out.Address = TA_Wrap;
		if (CacheOn()) CacheSave(Key, Out);
		return true;
	}
	Out.W = W;
	Out.H = H;
	Out.Format = PF_BC7;
	Out.Filter = Filter;
	Out.Address = TA_Wrap;
	int32 Off = 0, MW = W, MH = H;
	for (int32 m = 0; m < Mips; ++m)
	{
		const int32 Bytes = Bc7MipBytes(MW, MH);
		if (Off + Bytes > DataSize)
		{
			break;
		}
		Out.Mips.AddDefaulted_GetRef().Append(Data + Off, Bytes);
		Off += Bytes;
		MW = FMath::Max(1, MW / 2);
		MH = FMath::Max(1, MH / 2);
	}
	if (Out.Mips.Num() == 0)
	{
		Out.W = 0;
		return false;
	}
	return true;
}

UTexture2D* OutlawsTextures::CreatePrepared(FPreparedTex& P)
{
	if (!P.IsValid())
	{
		return nullptr;
	}
	FTexTimer T(StatUpload, TexBuckets().Upload);
	OutlawsPerf::CountTexture(P.W, P.H);
	UTexture2D* Tex = UTexture2D::CreateTransient(P.W, P.H, P.Format);
	if (!Tex)
	{
		return nullptr;
	}
	FTexturePlatformData* PD = Tex->GetPlatformData();
	int32 MW = P.W, MH = P.H;
	for (int32 m = 0; m < P.Mips.Num(); ++m)
	{
		FTexture2DMipMap* Mip = nullptr;
		if (m == 0)
		{
			Mip = &PD->Mips[0];
		}
		else
		{
			Mip = new FTexture2DMipMap();
			Mip->SizeX = MW;
			Mip->SizeY = MH;
			Mip->SizeZ = 1;
			PD->Mips.Add(Mip);
		}
		SetMipData(*Mip, P.Mips[m].GetData(), P.Mips[m].Num());
		MW = FMath::Max(1, MW / 2);
		MH = FMath::Max(1, MH / 2);
	}
	P.Mips.Empty();
	// (BUG-RESPAWNCRASH) the mips are copied to the GPU's texture once, when the resource is made, then let go: a level's
	// CPU copies were 130 to 160 MB on the PC (more on the Quest, whose BC7 is decoded to RGBA), kept for the level's life
	// beside the GPU's (the Quest's memory is the same memory).  Nothing reads them back.
	// (BUG-MENUBLACK) a second UpdateResource after this has nothing to upload: the texture comes out black or stale
	// memory.  Set the sampler state before this upload, and pass bKeepCpu for a texture that may be uploaded again
	if (!P.bKeepCpu)
	{
		for (FTexture2DMipMap& Mip : PD->Mips) Mip.BulkData.SetBulkDataFlags(BULKDATA_SingleUse);
	}
	Tex->SRGB = true;
	Tex->Filter = P.Filter;
	Tex->AddressX = P.Address;
	Tex->AddressY = P.Address;
	Tex->NeverStream = true;
	Tex->UpdateResource();
	StatBuilt.fetch_add(1);
	return Tex;
}

UTexture2D* OutlawsTextures::MakeTextureBGRA(int32 W, int32 H, const TArray<FColor>& Pixels, bool bMips, TextureFilter Filter, TextureAddress Address,
	bool bKeepCpu)
{
	FPreparedTex P;
	if (!PrepareBGRA(W, H, Pixels, bMips, Filter, Address, P)) return nullptr;
	P.bKeepCpu = bKeepCpu;
	return CreatePrepared(P);
}

UTexture2D* OutlawsTextures::MakeTextureBC7(int32 W, int32 H, int32 Mips, const uint8* Data, int32 DataSize, TextureFilter Filter,
	TextureAddress Address, bool bKeepCpu)
{
	FPreparedTex P;
	if (!PrepareBC7(W, H, Mips, Data, DataSize, Filter, P)) return nullptr;
	P.Address = Address;
	P.bKeepCpu = bKeepCpu;
	return CreatePrepared(P);
}

void OutlawsTextures::PrepareLevelTexture(const FOutlawsAssets& A, const FString& Name, bool bWantLo, bool bWantHd, const TArray<FColor>& Palette,
	bool bLinear, FOutlawsTexInfo& Info, FPreparedTex& Lo, FPreparedTex& Hd, const TArray<FColor>* Shades)
{
	Lo = FPreparedTex();
	Hd = FPreparedTex();
	const FString Resolved = ResolveTextureName(A, Name);
	if (Resolved.IsEmpty())
	{
		return;
	}
	Info.bTopLeftMid = Name.EndsWith(TEXT(".atx"), ESearchCase::IgnoreCase);
	if (Info.bTopLeftMid)
	{
		TArray<OutlawsFormats::FOutlawsAtxInstr> Prog;
		bool bStops = false;
		if (OutlawsFormats::ParseAtx(A, Name, Prog, bStops))
		{
			Info.bAtxStops = bStops;
			Info.bAtxLoop = !bStops && Prog.ContainsByPredicate([](const OutlawsFormats::FOutlawsAtxInstr& I) { return I.Kind == OutlawsFormats::FOutlawsAtxInstr::EKind::Goto; });
		}
	}

	TArray<uint8> Bytes;
	FPcxImage Img;
	TArray<uint8> Hdx;
	TArray<FSprxFrame> Frames;
	bool bPcx = false, bHdx = false;
	{
		const uint64 R0 = FPlatformTime::Cycles64();
		bPcx = A.Get(Resolved, Bytes) && DecodePcx(Bytes, Img);
		// remaster HD texture: named after the PCX (for an ATX, its first frame)
		const FString HdName = FPaths::GetBaseFilename(Resolved) + TEXT(".hdx");
		bHdx = A.Get(HdName, Hdx) && ParseSprx(Hdx, Frames) && Frames.Num() > 0 && Frames[0].W > 0 && Frames[0].H > 0;
		AddReadCycles(FPlatformTime::Cycles64() - R0);
	}
	if (!bPcx && !bHdx)
	{
		return;
	}

	if (bPcx)
	{
		Info.W = Img.W;
		Info.H = Img.H;
		if (Img.W == 640 && Img.H == 480)
		{
			Info.bTopLeftMid = true;
			if (bHdx)
			{
				// window panes: the image sits in the top-left of a 640x480 canvas; the HDX (4x) says how big
				const int32 CW = FMath::Clamp(Frames[0].W / 4, 1, Img.W);
				const int32 CH = FMath::Clamp(Frames[0].H / 4, 1, Img.H);
				TArray<uint8> Crop;
				Crop.SetNumUninitialized(CW * CH);
				for (int32 y = 0; y < CH; ++y)
				{
					FMemory::Memcpy(Crop.GetData() + y * CW, Img.Indices.GetData() + y * Img.W, CW);
				}
				Img.Indices = MoveTemp(Crop);
				Img.W = Info.W = CW;
				Img.H = Info.H = CH;
			}
		}
	}
	else
	{
		Info.W = FMath::Max(1, Frames[0].W / 4);
		Info.H = FMath::Max(1, Frames[0].H / 4);
	}

	if (bWantHd && bHdx && Frames[0].Format == SprxBC7)
	{
		const FSprxFrame& F = Frames[0];
		const int32 Layers = 1 + ((F.Flags & 1) ? 1 : 0) + ((F.Flags & 2) ? 1 : 0);
		const int32 PerLayer = (F.End - F.Offset) / Layers;
		if (F.Offset >= 0 && F.Offset + PerLayer <= Hdx.Num())
		{
			PrepareBC7(F.W, F.H, F.Mips, Hdx.GetData() + F.Offset, PerLayer, TF_Trilinear, Hd);
		}
	}
	if (bWantLo && bPcx)
	{
		PrepareBGRA(Img.W, Img.H, PcxToBGRA(Img, Palette, Shades), true, bLinear ? TF_Trilinear : TF_Nearest, TA_Wrap, Lo);
	}
}

UTexture2D* OutlawsTextures::LoadLevelTexture(const FOutlawsAssets& A, const FString& Name, bool bHD, const TArray<FColor>& Palette,
	bool bLinear, FOutlawsTexInfo& Info, bool& bOutWasHd, const TArray<FColor>* Shades)
{
	bOutWasHd = false;
	FPreparedTex Lo, Hd;
	PrepareLevelTexture(A, Name, false, bHD, Palette, bLinear, Info, Lo, Hd);
	if (Hd.IsValid())
	{
		if (UTexture2D* T = CreatePrepared(Hd))
		{
			bOutWasHd = true;
			return T;
		}
	}
	// no HD look (or not asked for): the PCX
	PrepareLevelTexture(A, Name, true, false, Palette, bLinear, Info, Lo, Hd, Shades);
	return CreatePrepared(Lo);
}

bool OutlawsTextures::LevelPcxPixels(const FOutlawsAssets& A, const FString& Name, const TArray<FColor>& Palette, const TArray<FColor>* Shades, int32& OutW, int32& OutH,
	TArray<FColor>& OutPx)
{
	const FString Resolved = ResolveTextureName(A, Name);
	TArray<uint8> Bytes;
	FPcxImage Img;
	if (Resolved.IsEmpty() || !A.Get(Resolved, Bytes) || !DecodePcx(Bytes, Img)) return false;
	OutW = Img.W;
	OutH = Img.H;
	OutPx = PcxToBGRA(Img, Palette, Shades);
	return true;
}

TSharedPtr<OutlawsTextures::FSpriteAtlas> OutlawsTextures::BuildSpriteAtlas(const FOutlawsAssets& A, const FString& NwxName, const TArray<FColor>& Palette, bool bCreateTexture, bool bAllChors)
{
	TArray<uint8> Bytes;
	FWax Wax;
	if (!A.Get(NwxName, Bytes) || !ParseWax(MoveTemp(Bytes), Wax) || Wax.Frames.Num() == 0)
	{
		return nullptr;
	}
	const TArray<FColor> Lut = SpriteLut(Palette);

	TSharedPtr<FSpriteAtlas> Out = MakeShared<FSpriteAtlas>();
	// the frames each direction row shows, per choreography (only the first unless bAllChors)
	TArray<TArray<TArray<int32>>> ChorRows;
	const int32 NumChors = Wax.Chors.Num() == 0 ? 0 : (bAllChors ? Wax.Chors.Num() : 1);
	for (int32 c = 0; c < NumChors; ++c)
	{
		const FWaxChor& C = Wax.Chors[c];
		TArray<TArray<int32>>& Rows = ChorRows.AddDefaulted_GetRef();
		for (int32 d = 0; d < C.Dirs; ++d)
		{
			TArray<int32> R;
			ChorFrames(C.Grid.GetData() + d * C.Frames, C.Frames, R);
			R.RemoveAll([&Wax](int32 f) { return !Wax.Frames.IsValidIndex(f); });
			if (R.Num() == 0) R.Add(0);
			Rows.Add(R);
		}
	}
	if (ChorRows.Num() == 0)
	{
		ChorRows.AddDefaulted_GetRef().Add({ 0 });
	}
	const TArray<TArray<int32>>& Rows = ChorRows[0];
	if (Wax.Chors.Num() > 0)
	{
		Out->Dirs = Wax.Chors[0].Dirs;
		Out->FPS = Wax.Chors[0].FPS;
	}
	Out->Frames = 1;
	for (const TArray<int32>& R : Rows) Out->Frames = FMath::Max(Out->Frames, R.Num());

	// render each frame used
	struct FPlacedNode { int32 Cell, X, Y, W, H; uint32 Flags; };
	struct FRendered { TArray<FColor> Px; TArray<uint8> Ix8; int32 W = 0; int32 H = 0; int32 Bottom = 0; int32 Left = 0; bool bValid = false; TArray<FPlacedNode> Nodes; };
	TMap<int32, FRendered> Rendered;
	TArray<uint8> Ix;
	Out->IndexUse.SetNumZeroed(256);
	TArray<int32> AllFrames;
	for (const TArray<TArray<int32>>& CR : ChorRows)
	{
		for (const TArray<int32>& R : CR) AllFrames.Append(R);
	}
	{
		const TArray<int32>& R = AllFrames;
		for (int32 f : R)
		{
			if (Rendered.Contains(f)) continue;
			FRendered& Rd = Rendered.Add(f);
			const FWaxFrame& Fr = Wax.Frames[f];
			int32 MinX = MAX_int32, MinY = MAX_int32, MaxX = MIN_int32, MaxY = MIN_int32;
			struct FPlaced { int32 Cell, X0, Y0; uint32 Flags; };
			TArray<FPlaced> Placed;
			for (const FWaxNode& Nd : Fr.Nodes)
			{
				if (!Wax.Cells.IsValidIndex(Nd.Cell)) continue;
				const FWaxCell& C = Wax.Cells[Nd.Cell];
				if (C.W <= 0 || C.H <= 0) continue;
				const int32 X0 = (Nd.Flags & NodeFlipX) ? 2 - Nd.OX - C.W : Nd.OX;
				const int32 Y0 = (Nd.Flags & NodeFlipY) ? 2 - Nd.OY - C.H : Nd.OY;
				Placed.Add({ Nd.Cell, X0, Y0, Nd.Flags });
				MinX = FMath::Min(MinX, X0); MinY = FMath::Min(MinY, Y0);
				MaxX = FMath::Max(MaxX, X0 + C.W); MaxY = FMath::Max(MaxY, Y0 + C.H);
			}
			if (Placed.Num() == 0) continue;
			Rd.W = MaxX - MinX;
			Rd.H = MaxY - MinY;
			Rd.Bottom = MaxY;                // the frame's bottom edge, rows under its insertion point
			Rd.Left = MinX;                  // its left edge, columns left of its insertion point (negative)
			Rd.Px.SetNumZeroed(Rd.W * Rd.H);
			Rd.Ix8.SetNumZeroed(Rd.W * Rd.H);
			for (const FPlaced& Pl : Placed)
			{
				const FWaxCell& C = Wax.Cells[Pl.Cell];
				Rd.Nodes.Add({ Pl.Cell, Pl.X0 - MinX, Pl.Y0 - MinY, C.W, C.H, Pl.Flags });
				if (!DecodeWaxCell(Wax, Pl.Cell, Ix)) continue;
				const bool bFX = (Pl.Flags & NodeFlipX) != 0, bFY = (Pl.Flags & NodeFlipY) != 0;
				for (int32 y = 0; y < C.H; ++y)
				{
					const int32 sy = bFY ? C.H - 1 - y : y;
					for (int32 x = 0; x < C.W; ++x)
					{
						const int32 sx = bFX ? C.W - 1 - x : x;
						const uint8 V = Ix[sy * C.W + sx];
						const FColor Col = Lut[V];
						if (Col.A == 0) continue;
						++Out->IndexUse[V];
						Rd.Px[(Pl.Y0 - MinY + y) * Rd.W + (Pl.X0 - MinX + x)] = Col;
						Rd.Ix8[(Pl.Y0 - MinY + y) * Rd.W + (Pl.X0 - MinX + x)] = V;
					}
				}
			}
			Rd.bValid = true;
		}
	}
	TArray<int32> Used;
	for (const TPair<int32, FRendered>& Kv : Rendered)
	{
		if (Kv.Value.bValid) Used.Add(Kv.Key);
	}
	Used.Sort();
	if (Used.Num() == 0)
	{
		return nullptr;
	}
	for (int32 f : Used)
	{
		Out->CellW = FMath::Max(Out->CellW, Rendered[f].W);
		Out->CellH = FMath::Max(Out->CellH, Rendered[f].H);
	}
	Out->Cells = Used.Num();
	Out->Cols = FMath::CeilToInt(FMath::Sqrt(float(Used.Num())));
	Out->Rows = FMath::CeilToInt(float(Used.Num()) / Out->Cols);
	const int32 AW = Out->Cols * Out->CellW, AH = Out->Rows * Out->CellH;
	TMap<int32, int32> CellOf;
	TArray<FColor> Atlas;
	if (bCreateTexture)
	{
		Atlas.SetNumZeroed(AW * AH);
		Out->Indices.SetNumZeroed(AW * AH);
	}
	Out->BelowPx.SetNumZeroed(Used.Num());
	Out->FootPx.SetNumZeroed(Used.Num());
	Out->SidePx2.SetNumZeroed(Used.Num());
	Out->InsBottomPx.SetNumZeroed(Used.Num());
	constexpr int32 MaxLowerPx = 4;           // (BUG-DRIPFLOOR) a gap under the drawing this small is closed
	int32 RaisedCells = 0, MaxGap = 0;
	FString RaisedList;
	for (int32 k = 0; k < Used.Num(); ++k)
	{
		const int32 f = Used[k];
		CellOf.Add(f, k);
		{
			// where the cell stands: its insertion point on the floor (see FSpriteAtlas::BelowPx)
			const FRendered& Rd = Rendered[f];
			int32 Pad = Rd.H;                // transparent rows under the drawing
			for (int32 y = Rd.H - 1; y >= 0 && Pad == Rd.H; --y)
			{
				for (int32 x = 0; x < Rd.W; ++x)
				{
					if (Rd.Px[y * Rd.W + x].A == 255) { Pad = Rd.H - 1 - y; break; }
				}
			}
			const int32 Foot = FMath::Max(0, Rd.Bottom - Pad);
			Out->FootPx[k] = int16(FMath::Clamp(Foot, 0, 4096));
			// a drawing that ends a few rows over its point (BGY1..3, up to 2 px) is let down onto it; one that ends far
			// over it is drawn there, as the Jedi engine draws every frame (BUG-DRIPFLOOR, user 2026-10-15: DRIP's falling
			// drop, its frames drawn up to 208 px over the insertion point, lay on the floor the whole fall, the drop
			// looping in place; the cell's bottom then sits over the point: a negative BelowPx).  Not the figures with
			// every choreography (bad guys, props that react): their few airborne frames (a death's throw, up to 35 px)
			// stay let down, the body's own flight carries them.  -OLOldSpriteLower: every such frame let down, as before
			static const bool bOldLower = OLFlags::Param(TEXT("OLOldSpriteLower"));
			const int32 Gap = Pad - Rd.Bottom;          // rows from the drawing's lowest row down to the point
			if (Gap > MaxLowerPx && !bOldLower && !bAllChors)
			{
				Out->BelowPx[k] = int16(FMath::Clamp(Rd.Bottom, -4096, 4096));
				++RaisedCells;
				MaxGap = FMath::Max(MaxGap, Gap);
				if (RaisedCells <= 16) RaisedList += FString::Printf(TEXT(" %d"), Gap);
			}
			else Out->BelowPx[k] = int16(FMath::Clamp(Foot + FMath::Min(Pad, Rd.H), 0, 4096));
			Out->InsBottomPx[k] = int16(FMath::Clamp(Rd.Bottom, -4096, 4096));
			// (R4) and across: the insertion point's column in the cell (the frame placed (CellW - W) / 2 in, below),
			// as twice its offset from the cell's centre
			const int32 InsX = (Out->CellW - Rd.W) / 2 - Rd.Left;
			Out->SidePx2[k] = int16(FMath::Clamp(2 * InsX - Out->CellW, -4096, 4096));
		}
		const FRendered& Rd = Rendered[f];
		const int32 CX = (k % Out->Cols) * Out->CellW, CY = (k / Out->Cols) * Out->CellH;
		const int32 X0 = CX + (Out->CellW - Rd.W) / 2;
		const int32 Y0 = CY + (Out->CellH - Rd.H);       // bottom aligned
		// (REQ-GRABUPSCALE) the nodes and each cell's WAX frame without a texture too: the grab upscaler builds atlases on a worker
		Out->CellFrame.Add(f);
		for (const FPlacedNode& N : Rd.Nodes)
		{
			Out->Nodes.Add({ N.Cell, X0 + N.X, Y0 + N.Y, N.W, N.H, (N.Flags & NodeFlipX) != 0, (N.Flags & NodeFlipY) != 0 });
		}
		if (!bCreateTexture) continue;
		for (int32 y = 0; y < Rd.H; ++y)
		{
			FMemory::Memcpy(Atlas.GetData() + (Y0 + y) * AW + X0, Rd.Px.GetData() + y * Rd.W, Rd.W * sizeof(FColor));
			FMemory::Memcpy(Out->Indices.GetData() + (Y0 + y) * AW + X0, Rd.Ix8.GetData() + y * Rd.W, Rd.W);
		}
	}
	auto AddGrid = [&CellOf](const TArray<TArray<int32>>& ChorR, int32 Frames, TArray<int32>& Grid)
	{
		for (const TArray<int32>& R : ChorR)
		{
			int32 Last = 0;
			for (int32 i = 0; i < Frames; ++i)
			{
				if (i < R.Num())
				{
					const int32* C = CellOf.Find(R[i]);
					Last = C ? *C : 0;
				}
				Grid.Add(Last);   // short rows hold their last frame
			}
		}
	};
	AddGrid(Rows, Out->Frames, Out->Grid);
	if (bAllChors)
	{
		for (int32 c = 0; c < Wax.Chors.Num() && c < ChorRows.Num(); ++c)
		{
			const FWaxChor& W = Wax.Chors[c];
			FSpriteAtlas::FChor& C = Out->Chors.AddDefaulted_GetRef();
			C.Dirs = W.Dirs;
			C.FPS = W.FPS;
			C.Frames = 1;
			for (const TArray<int32>& R : ChorRows[c]) C.Frames = FMath::Max(C.Frames, R.Num());
			C.Length = ChorRows[c].Num() > 0 ? ChorRows[c][0].Num() : 1;
			for (const TArray<int32>& R : ChorRows[c])
			{
				for (int32 f : R)
				{
					if (const FRendered* Rd = Rendered.Find(f)) C.HeightPx = FMath::Max(C.HeightPx, Rd->bValid ? Rd->H : 0);
				}
			}
			C.GridStart = Out->ChorGrid.Num();
			AddGrid(ChorRows[c], C.Frames, Out->ChorGrid);
			// the first row's opcodes, with the number of frames shown before each
			int32 Shown = 0;
			for (int32 i = 0; i < W.Frames; )
			{
				const int16 V = W.Grid[i];
				if (V >= 0)
				{
					++Shown;
					++i;
					continue;
				}
				if (W.Frames - i <= 2) break;
				C.Events.Add(FIntVector4(Shown, V, W.Grid[i + 1], W.Grid[i + 2]));
				i += 3;
			}
		}
	}
	if (bCreateTexture)
	{
		Out->Texture = MakeTextureBGRA(AW, AH, Atlas, false, TF_Nearest, TA_Clamp);
	}
	Out->AtlasW = AW;
	Out->AtlasH = AH;
	Out->Alpha.SetNumUninitialized(Atlas.Num());
	for (int32 i = 0; i < Atlas.Num(); ++i) Out->Alpha[i] = Atlas[i].A > 127 ? 1 : 0;
	Out->Nwx = NwxName;
	if (RaisedCells > 0)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT sprite %s: %d of %d cells drawn over their insertion points (up to %d px), not let down; px:%s"), *NwxName, RaisedCells, Used.Num(), MaxGap, *RaisedList);
	}
	return Out;
}

UTexture* OutlawsTextures::BuildSpriteAtlasHD(UObject* Outer, const FOutlawsAssets& A, FSpriteAtlas& Atlas)
{
	Atlas.bHdTried = true;
	if (!FApp::CanEverRender() || !Outer || Atlas.Nodes.Num() == 0)
	{
		return nullptr;
	}
	TArray<uint8> Spx;
	TArray<FSprxFrame> Frames;
	if (!A.Get(FPaths::GetBaseFilename(Atlas.Nwx) + TEXT(".spx"), Spx) || !ParseSprx(Spx, Frames))
	{
		return nullptr;
	}
	for (const FSpriteAtlas::FNode& N : Atlas.Nodes)
	{
		// all or nothing: a half-HD character would flicker between looks
		if (!Frames.IsValidIndex(N.Cell) || Frames[N.Cell].W <= 0 || Frames[N.Cell].H <= 0 || Frames[N.Cell].Format != SprxBC7) return nullptr;
	}
	const int32 AW = Atlas.Cols * Atlas.CellW, AH = Atlas.Rows * Atlas.CellH;
	int32 S = 4;
	while (S > 1 && (FMath::Max(AW, AH) * S > 4096 || int64(AW) * AH * S * S * 4 > (48ll << 20)))
	{
		--S;
	}
	if (S < 2)
	{
		return nullptr;
	}
	UTextureRenderTarget2D* RT = NewObject<UTextureRenderTarget2D>(Outer);
	RT->RenderTargetFormat = RTF_RGBA8_SRGB;
	RT->ClearColor = FLinearColor::Transparent;
	RT->bAutoGenerateMips = false;
	RT->TargetGamma = 1.f;          // the sRGB target encodes on write: no gamma in the canvas shader
	RT->Filter = TF_Bilinear;
	RT->AddressX = TA_Clamp;
	RT->AddressY = TA_Clamp;
	RT->InitAutoFormat(AW * S, AH * S);
	RT->UpdateResourceImmediate(true);
	UCanvas* Canvas = nullptr;
	FVector2D Size;
	FDrawToRenderTargetContext Ctx;
	UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(Outer, RT, Canvas, Size, Ctx);
	if (!Canvas)
	{
		return nullptr;
	}
	// (TEXLOAD) the cells' pictures made ready across the cores (on the Quest each is a BC7 decode and a mip chain, most
	// too small to split by rows), then made textures here in the nodes' order
	TArray<int32> Cells;
	{
		TSet<int32> SeenCells;
		for (const FSpriteAtlas::FNode& N : Atlas.Nodes) if (!SeenCells.Contains(N.Cell)) { SeenCells.Add(N.Cell); Cells.Add(N.Cell); }
	}
	TArray<FPreparedTex> Prepared;
	Prepared.SetNum(Cells.Num());
	OL_PERF_SCOPE("Sprites: HD cells + draws");
	ParallelFor(Cells.Num(), [&](int32 i)
	{
		const FSprxFrame& F = Frames[Cells[i]];
		const int32 Layers = 1 + ((F.Flags & 1) ? 1 : 0) + ((F.Flags & 2) ? 1 : 0);
		const int32 PerLayer = (F.End - F.Offset) / Layers;
		if (F.Offset >= 0 && F.Offset + PerLayer <= Spx.Num())
		{
			PrepareBC7(F.W, F.H, F.Mips, Spx.GetData() + F.Offset, PerLayer, TF_Bilinear, Prepared[i]);
		}
	}, OLFlags::Param(TEXT("OLTexSerial")));
	TMap<int32, UTexture2D*> CellTex;
	TArray<TStrongObjectPtr<UTexture2D>> Keep;      // alive until the draws have run
	for (int32 i = 0; i < Cells.Num(); ++i)
	{
		UTexture2D* T = CreatePrepared(Prepared[i]);
		CellTex.Add(Cells[i], T);
		if (T) Keep.Emplace(T);
	}
	for (const FSpriteAtlas::FNode& N : Atlas.Nodes)
	{
		UTexture2D* T = CellTex.FindRef(N.Cell);
		if (!T) continue;
		Canvas->K2_DrawTexture(T, FVector2D(N.X * S, N.Y * S), FVector2D(N.W * S, N.H * S),
			FVector2D(N.bFlipX ? 1.0 : 0.0, N.bFlipY ? 1.0 : 0.0), FVector2D(N.bFlipX ? -1.0 : 1.0, N.bFlipY ? -1.0 : 1.0),
			FLinearColor::White, BLEND_AlphaComposite);   // translucent keeps the target's alpha (0): composite writes it
	}
	UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(Outer, Ctx);
	static int32 Checked = 0;
	if (OutlawsPerf::IsLoading() && Checked >= 3)
	{
		// (TEXLOAD) in a level build the cells stay referenced till its end, where one flush serves every atlas
		// (ReleaseAtlasCells), not one wait for the render thread per atlas
		for (TStrongObjectPtr<UTexture2D>& K : Keep) PendingCells.Add(MoveTemp(K));
	}
	else
	{
		OL_PERF_SCOPE("Sprites: HD atlas flush");
		FlushRenderingCommands();
	}
	if (Checked++ < 3)
	{
		// diagnosis: how much of the atlas the draws covered
		TArray<FColor> Px;
		if (FTextureRenderTargetResource* Res = RT->GameThread_GetRenderTargetResource()) Res->ReadPixels(Px);
		int32 Solid = 0;
		FColor Sample = FColor::Transparent;
		for (const FColor& C : Px) if (C.A > 0) { ++Solid; Sample = C; }
		UE_LOG(LogOutlaws, Log, TEXT("OLRT HD atlas %s: %dx%d at %dx, %d nodes, %d cells, %d of %d pixels drawn, sample %s"), *Atlas.Nwx, AW * S, AH * S, S,
			Atlas.Nodes.Num(), CellTex.Num(), Solid, Px.Num(), *Sample.ToString());
	}
	Atlas.TextureHd = RT;
	Atlas.HdScale = S;
	return RT;
}

bool OutlawsTextures::DecodeSpriteAtlasHdPixels(const FOutlawsAssets& A, const FSpriteAtlas& Atlas, TArray<FColor>& Out, int32& OutW, int32& OutH)
{
	constexpr int32 HdCellScale = 4;          // the remaster's SPX cells are 4x the NWX cell
	OutW = OutH = 0;
	TArray<uint8> Spx;
	TArray<FSprxFrame> Frames;
	if (Atlas.Nodes.Num() == 0 || !A.Get(FPaths::GetBaseFilename(Atlas.Nwx) + TEXT(".spx"), Spx) || !ParseSprx(Spx, Frames))
	{
		return false;
	}
	const int32 AW = Atlas.Cols * Atlas.CellW * HdCellScale, AH = Atlas.Rows * Atlas.CellH * HdCellScale;
	TArray<FColor> Canvas;
	Canvas.SetNumZeroed(AW * AH);
	TArray<FColor> Cell;
	for (const FSpriteAtlas::FNode& N : Atlas.Nodes)
	{
		if (!Frames.IsValidIndex(N.Cell)) return false;
		const FSprxFrame& F = Frames[N.Cell];
		const int32 Layers = 1 + ((F.Flags & 1) ? 1 : 0) + ((F.Flags & 2) ? 1 : 0);
		const int32 PerLayer = (F.End - F.Offset) / Layers;
		if (F.Format != SprxBC7 || F.W != N.W * HdCellScale || F.H != N.H * HdCellScale || F.Offset < 0
			|| F.Offset + Bc7MipBytes(F.W, F.H) > Spx.Num() || Bc7MipBytes(F.W, F.H) > PerLayer)
		{
			return false;
		}
		Bc7::DecodeImage(Spx.GetData() + F.Offset, F.W, F.H, Cell);
		for (int32 y = 0; y < F.H; ++y)
		{
			const int32 Sy = N.bFlipY ? F.H - 1 - y : y;
			const int32 Dy = N.Y * HdCellScale + y;
			if (Dy < 0 || Dy >= AH) continue;
			for (int32 x = 0; x < F.W; ++x)
			{
				const int32 Dx = N.X * HdCellScale + x;
				const FColor& C = Cell[Sy * F.W + (N.bFlipX ? F.W - 1 - x : x)];
				if (C.A == 0 || Dx < 0 || Dx >= AW) continue;
				Canvas[Dy * AW + Dx] = C;
			}
		}
	}
	Out = MoveTemp(Canvas);
	OutW = AW;
	OutH = AH;
	return true;
}

bool OutlawsTextures::DecodeSpriteAtlasFullBrightMask(const FOutlawsAssets& A, const FSpriteAtlas& Atlas, TArray<FColor>& Out, int32& OutW, int32& OutH, int32* OutLitPx)
{
	// REQ-FIREBRIGHT3: the remaster's own full-bright layer (an SPX frame's second layer, Flags bit 0: camfire1's flame, not
	// its logs) where every node's frame has one, as the fire material's mask at 4x: 255 - the layer's alpha, so 0 (kept
	// bright) where the layer draws the colour and 255 (dims with the light) where it does not
	constexpr int32 HdCellScale = 4;
	OutW = OutH = 0;
	if (OutLitPx) *OutLitPx = 0;
	TArray<uint8> Spx;
	TArray<FSprxFrame> Frames;
	if (Atlas.Nodes.Num() == 0 || !A.Get(FPaths::GetBaseFilename(Atlas.Nwx) + TEXT(".spx"), Spx) || !ParseSprx(Spx, Frames))
	{
		return false;
	}
	const int32 AW = Atlas.Cols * Atlas.CellW * HdCellScale, AH = Atlas.Rows * Atlas.CellH * HdCellScale;
	TArray<FColor> Canvas;
	Canvas.Init(FColor(255, 255, 255, 255), AW * AH);
	TArray<FColor> Base, Lit;
	int32 NumLit = 0;
	for (const FSpriteAtlas::FNode& N : Atlas.Nodes)
	{
		if (!Frames.IsValidIndex(N.Cell)) return false;
		const FSprxFrame& F = Frames[N.Cell];
		if (!(F.Flags & 1)) return false;     // (no full-bright layer: the 8-bit colours' mask stands)
		const int32 Layers = 2 + ((F.Flags & 2) ? 1 : 0);
		const int32 PerLayer = (F.End - F.Offset) / Layers;
		if (F.Format != SprxBC7 || F.W != N.W * HdCellScale || F.H != N.H * HdCellScale || F.Offset < 0
			|| F.Offset + PerLayer + Bc7MipBytes(F.W, F.H) > Spx.Num() || Bc7MipBytes(F.W, F.H) > PerLayer)
		{
			return false;
		}
		Bc7::DecodeImage(Spx.GetData() + F.Offset, F.W, F.H, Base);
		Bc7::DecodeImage(Spx.GetData() + F.Offset + PerLayer, F.W, F.H, Lit);
		for (int32 y = 0; y < F.H; ++y)
		{
			const int32 Sy = N.bFlipY ? F.H - 1 - y : y;
			const int32 Dy = N.Y * HdCellScale + y;
			if (Dy < 0 || Dy >= AH) continue;
			for (int32 x = 0; x < F.W; ++x)
			{
				const int32 Dx = N.X * HdCellScale + x;
				const int32 j = Sy * F.W + (N.bFlipX ? F.W - 1 - x : x);
				if ((Base[j].A == 0 && Lit[j].A == 0) || Dx < 0 || Dx >= AW) continue;
				const float M = 1.f - Lit[j].A / 255.f;           // (the texture is sRGB: stored so it samples back as this)
				Canvas[Dy * AW + Dx] = FLinearColor(M, M, M, 1.f).ToFColor(true);
				NumLit += Lit[j].A >= 128;
			}
		}
	}
	Out = MoveTemp(Canvas);
	OutW = AW;
	OutH = AH;
	if (OutLitPx) *OutLitPx = NumLit;
	return true;
}

bool OutlawsTextures::DecodeSpxCellPixels(const TArray<uint8>& Data, const TArray<FSprxFrame>& Frames, int32 Cell, TArray<FColor>& Out, int32& OutW, int32& OutH)
{
	OutW = OutH = 0;
	if (!Frames.IsValidIndex(Cell)) return false;
	const FSprxFrame& F = Frames[Cell];
	if (F.Format != SprxBC7 || F.W <= 0 || F.H <= 0 || F.Offset < 0 || F.Offset + Bc7MipBytes(F.W, F.H) > Data.Num()) return false;
	Bc7::DecodeImage(Data.GetData() + F.Offset, F.W, F.H, Out);
	OutW = F.W;
	OutH = F.H;
	return true;
}

bool OutlawsTextures::DecodeSpriteCellHd(const TArray<uint8>& Spx, const TArray<FSprxFrame>& Frames, const FSpriteAtlas& Atlas, int32 Cell, TArray<FColor>& Out, int32& OutW, int32& OutH)
{
	constexpr int32 S = 4;                    // the remaster's SPX cells are 4x the NWX cell
	OutW = OutH = 0;
	const int32 Cols = FMath::Max(1, Atlas.Cols);
	if (Cell < 0 || Cell >= Atlas.Cells || Atlas.CellW <= 0 || Atlas.CellH <= 0) return false;
	const int32 CX = (Cell % Cols) * Atlas.CellW, CY = (Cell / Cols) * Atlas.CellH;
	const int32 W = Atlas.CellW * S, H = Atlas.CellH * S;
	TArray<FColor> Canvas;
	Canvas.SetNumZeroed(W * H);
	TArray<FColor> Px;
	int32 Drawn = 0;
	for (const FSpriteAtlas::FNode& N : Atlas.Nodes)
	{
		if (N.X < CX || N.X >= CX + Atlas.CellW || N.Y < CY || N.Y >= CY + Atlas.CellH) continue;    // (another cell's)
		if (!Frames.IsValidIndex(N.Cell)) return false;
		const FSprxFrame& F = Frames[N.Cell];
		const int32 Layers = 1 + ((F.Flags & 1) ? 1 : 0) + ((F.Flags & 2) ? 1 : 0);
		const int32 PerLayer = (F.End - F.Offset) / Layers;
		if (F.Format != SprxBC7 || F.W != N.W * S || F.H != N.H * S || F.Offset < 0
			|| F.Offset + Bc7MipBytes(F.W, F.H) > Spx.Num() || Bc7MipBytes(F.W, F.H) > PerLayer)
		{
			return false;                     // (all or nothing, as the HD atlas)
		}
		Bc7::DecodeImage(Spx.GetData() + F.Offset, F.W, F.H, Px);
		for (int32 y = 0; y < F.H; ++y)
		{
			const int32 Sy = N.bFlipY ? F.H - 1 - y : y;
			const int32 Dy = (N.Y - CY) * S + y;
			if (Dy < 0 || Dy >= H) continue;
			for (int32 x = 0; x < F.W; ++x)
			{
				const int32 Dx = (N.X - CX) * S + x;
				const FColor& C = Px[Sy * F.W + (N.bFlipX ? F.W - 1 - x : x)];
				if (C.A == 0 || Dx < 0 || Dx >= W) continue;
				Canvas[Dy * W + Dx] = C;
			}
		}
		++Drawn;
	}
	if (Drawn == 0) return false;
	Out = MoveTemp(Canvas);
	OutW = W;
	OutH = H;
	return true;
}

void OutlawsTextures::Build3doSections(const FOutlawsAssets& A, const F3doModel& M, int32 Light, bool bHD,
	const TArray<FColor>& Palette, bool bReverse, TMap<FString, TObjectPtr<UTexture2D>>& Textures, TMap<FString, FOutlawsMeshSection>& Out)
{
	const float Bright = FMath::Clamp(Light, 0, 31) / 31.f;
	if (!Textures.Contains(TEXT("PALETTE")))
	{
		TArray<FColor> Px = Palette;
		for (FColor& C : Px) C.A = 255;
		if (UTexture2D* T = MakeTextureBGRA(256, 1, Px, false, TF_Nearest, TA_Clamp))
		{
			Textures.Add(TEXT("PALETTE"), T);
		}
	}
	auto ToUE3do = [](const FVector3f& V) { return FVector(V.Z * 30.0, V.X * 30.0, -V.Y * 30.0); };
	for (const F3doObject& Ob : M.Objects)
	{
		FString TexStem;
		if (M.Textures.IsValidIndex(Ob.Texture))
		{
			TexStem = FPaths::GetBaseFilename(M.Textures[Ob.Texture]).ToUpper();
			if (!Textures.Contains(TexStem))
			{
				FOutlawsTexInfo Info;
				bool bWasHd = false;
				if (UTexture2D* T = LoadLevelTexture(A, M.Textures[Ob.Texture], bHD, Palette, false, Info, bWasHd))
				{
					Textures.Add(TexStem, T);
				}
				else
				{
					TexStem.Reset();
				}
			}
		}
		for (const F3doFace& F : Ob.Faces)
		{
			const int32 N = F.V.Num();
			bool bOk = N >= 3;
			for (int32 v : F.V) bOk &= Ob.Verts.IsValidIndex(v);
			if (!bOk) continue;
			FVector P[4];
			FVector2D UV[4];
			const bool bTex = !TexStem.IsEmpty() && F.bTextured && F.T.Num() == N;
			bool bUVOk = bTex;
			for (int32 i = 0; i < N && i < 4; ++i)
			{
				P[i] = ToUE3do(Ob.Verts[F.V[i]]);
				if (bTex && Ob.TVerts.IsValidIndex(F.T[i]))
				{
					UV[i] = FVector2D(Ob.TVerts[F.T[i]].X, Ob.TVerts[F.T[i]].Y);
				}
				else
				{
					bUVOk = false;
				}
			}
			FString Key = TexStem;
			if (!bUVOk)
			{
				const double U = (FMath::Clamp(F.Color, 0, 255) + 0.5) / 256.0;
				for (int32 i = 0; i < N && i < 4; ++i) UV[i] = FVector2D(U, 0.5);
				Key = TEXT("PALETTE");
			}
			FOutlawsMeshSection& S = Out.FindOrAdd(Key);
			S.Texture = Key;
			S.AddFace(P, UV, FMath::Min(N, 4), Bright, bReverse);
		}
	}
}
