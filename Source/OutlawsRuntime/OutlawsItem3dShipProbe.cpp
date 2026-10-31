// BUG-DEVDIR2 (user 2026-11-01, option 2: the item3d meshes ship, the sprite's pixels never do): -OLItem3dShipProbe, on the
// dev PC (it needs the placeholders to measure against).  Every shipped recipe in Content/Item3d (and held/) painted from
// the player's HD sprites by OutlawsItem3d, against the dev texture make_item_ship.py made it from (a resampled swatch,
// the ruling's GAMBOXS red and brass, against its new colour, its move logged); every shipped OBJ against the dev one
// (sections, corners, bounds); TABLE02 and the stool from the Remaster's TABLE01 at their sizes.  Lines "OLSHOT item3dship
// <model> PASS|FAIL: ..." and "OLSHOT item3dship RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsItem3d.h"
#include "OutlawsLab.h"
#include "OutlawsTextures.h"
#include "OutlawsRuntime.h"
#include "Misc/CommandLine.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_OUTLAWS_TESTS

namespace
{
	TSharedPtr<FJsonObject> ProbeJson(const FString& Path)
	{
		FString Text;
		TSharedPtr<FJsonObject> J;
		if (!FFileHelper::LoadFileToString(Text, *Path) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), J)) return nullptr;
		return J;
	}
}

OL_SHOT_MATCH(OLItem3dShipProbe) { return OLFlags::Param(TEXT("OLItem3dShipProbe")); }

OL_SHOT_TEST(OLItem3dShipProbe, 1340)
{
	if (!L || ShotStep != 0) return;
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying();
	if (ShotTime < 3.f || !bUp) return;
	ShotStep = -1;
	int32 Passes = 0, Fails = 0, Exact = 0, MaxAll = 0;
	double TotalMs = 0.0, WorstMs = 0.0;
	FString Worst;
	auto Check = [&](bool bOk, const FString& Name, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT item3dship %s %s: %s"), *Name, bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const FString Dev = FOutlawsAssets::DevPlaceholderDir();
	const FString Ship = OutlawsItem3d::ShippedDir();
	TArray<FString> Recipes;
	for (const TCHAR* Sub : { TEXT(""), TEXT("held") })
	{
		TArray<FString> Found;
		const FString D = FPaths::Combine(Ship, Sub);
		IFileManager::Get().FindFiles(Found, *FPaths::Combine(D, TEXT("*_tex.json")), true, false);
		Found.Sort();
		for (const FString& F : Found) Recipes.Add(FPaths::Combine(D, F));
	}
	for (const FString& R : Recipes)
	{
		const bool bHeld = FPaths::GetCleanFilename(FPaths::GetPath(R)) == TEXT("held");
		const FString Name = FPaths::GetBaseFilename(R).LeftChop(4);
		const FString Tag = bHeld ? TEXT("held/") + Name : Name;
		const TSharedPtr<FJsonObject> J = ProbeJson(R);
		if (!J.IsValid()) { Check(false, Tag, TEXT("the recipe does not read")); continue; }
		const FString ObjPath = FPaths::Combine(FPaths::GetPath(R), Name + TEXT(".obj"));
		if (J->HasField(TEXT("from_3do")))
		{
			// TABLE02, the stool: the Remaster's TABLE01 at the sidecar's size
			OutlawsObj::FObjMesh M;
			const bool bOk = OutlawsItem3d::Exists(ObjPath) && OutlawsItem3d::Load(ObjPath, 1.0, true, M, L);
			const TSharedPtr<FJsonObject> Side = ProbeJson(FPaths::ChangeExtension(ObjPath, TEXT("json")));
			const TArray<TSharedPtr<FJsonValue>>* Real = nullptr;
			const FVector E = M.Bounds.IsValid ? M.Bounds.GetSize() : FVector::ZeroVector;
			bool bSize = Side.IsValid() && Side->TryGetArrayField(TEXT("real_size_cm"), Real) && Real->Num() == 3
				&& FMath::Abs(FMath::Max(E.X, E.Y) - (*Real)[0]->AsNumber()) < 0.1 && FMath::Abs(E.Z - (*Real)[2]->AsNumber()) < 0.1;
			bool bTex = bOk;
			for (const TPair<FString, FOutlawsMeshSection>& Kv : M.Sections) bTex &= OutlawsItem3d::Texture(Kv.Value.Texture, L) != nullptr;
			Check(bOk && bSize && bTex, Tag, FString::Printf(TEXT("from the Remaster's TABLE01: %d sections, %.1f x %.1f x %.1f cm, textures %s"),
				M.Sections.Num(), E.X, E.Y, E.Z, bTex ? TEXT("all") : TEXT("MISSING")));
			continue;
		}
		// the base sprite as the game decodes it (its size against the recipe's; -OLItem3dShipDump=<dir>: written there, dev only)
		{
			const TSharedPtr<FJsonObject>* Base = nullptr;
			FString Stem;
			if (J->TryGetObjectField(TEXT("base"), Base) && (*Base)->TryGetStringField(TEXT("stem"), Stem) && L->GetLabAssets())
			{
				const TSharedPtr<OutlawsTextures::FSpriteAtlas> Atlas = L->GetPickupAtlas(Stem + TEXT(".NWX"));
				TArray<FColor> Hd;
				int32 Hw = 0, Hh = 0;
				const bool bHd = Atlas && OutlawsTextures::DecodeSpriteAtlasHdPixels(*L->GetLabAssets(), *Atlas, Hd, Hw, Hh);
				const TArray<TSharedPtr<FJsonValue>>* Sz = nullptr;
				const int32 Rw = (*Base)->TryGetArrayField(TEXT("size"), Sz) ? int32((*Sz)[0]->AsNumber()) : -1, Rh = Sz ? int32((*Sz)[1]->AsNumber()) : -1;
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT item3dship sprite %s: %s, %dx%d (the recipe's %dx%d), atlas %dx%d cells of %dx%d, %d nodes"), *Stem, bHd ? TEXT("decoded") : TEXT("NOT decoded"),
					Hw, Hh, Rw, Rh, Atlas ? Atlas->Cols : 0, Atlas ? Atlas->Rows : 0, Atlas ? Atlas->CellW : 0, Atlas ? Atlas->CellH : 0, Atlas ? Atlas->Nodes.Num() : 0);
				FString DumpDir;
				if (bHd && FParse::Value(FCommandLine::Get(), TEXT("OLItem3dShipDump="), DumpDir))
				{
					FImage Out(Hw, Hh, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
					FMemory::Memcpy(Out.RawData.GetData(), Hd.GetData(), Hd.Num() * sizeof(FColor));
					FImageUtils::SaveImageByExtension(*FPaths::Combine(DumpDir, Stem + TEXT(".png")), Out);
				}
			}
		}
		// the texture, painted from the player's sprites
		const double T0 = FPlatformTime::Seconds();
		TArray<FColor> Px;
		int32 W = 0, H = 0;
		FString Err;
		const bool bPainted = OutlawsItem3d::Paint(L, R, Px, W, H, Err);
		const double Ms = (FPlatformTime::Seconds() - T0) * 1000.0;
		TotalMs += Ms;
		FString DumpTo;
		if (bPainted && FParse::Value(FCommandLine::Get(), TEXT("OLItem3dShipDump="), DumpTo))
		{
			FImage Out(W, H, ERawImageFormat::BGRA8, EGammaSpace::sRGB);
			FMemory::Memcpy(Out.RawData.GetData(), Px.GetData(), Px.Num() * sizeof(FColor));
			FImageUtils::SaveImageByExtension(*FPaths::Combine(DumpTo, Name + TEXT("_rt.png")), Out);
		}
		if (Ms > WorstMs) { WorstMs = Ms; Worst = Tag; }
		FImage Img;
		const FString Png = FPaths::Combine(Dev, bHeld ? TEXT("items3d/held") : TEXT("items3d"), Name + TEXT(".png"));
		TArray<uint8> PngBytes;
		if (!bPainted || Dev.IsEmpty() || !FFileHelper::LoadFileToArray(PngBytes, *Png, FILEREAD_Silent) || !FImageUtils::DecompressImage(PngBytes.GetData(), PngBytes.Num(), Img))
		{
			Check(false, Tag, bPainted ? TEXT("no dev texture to measure against") : *FString::Printf(TEXT("not painted: %s"), *Err));
			continue;
		}
		Img.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		TArray<FColor> Ref(static_cast<const FImage&>(Img).AsBGRA8().GetData(), int32(Img.SizeX * Img.SizeY));
		if (Img.SizeX != W || Img.SizeY != H) { Check(false, Tag, FString::Printf(TEXT("painted %dx%d, the dev texture %dx%d"), W, H, Img.SizeX, Img.SizeY)); continue; }
		// a swatch the ruling resamples from the sprite: expected at its new colour, its move from the dev one logged
		FString Moved;
		for (const TSharedPtr<FJsonValue>& Sv : J->GetArrayField(TEXT("swatches")))
		{
			const TSharedPtr<FJsonObject> S = Sv->AsObject();
			const TArray<TSharedPtr<FJsonValue>>* Was = nullptr;
			if (!S->TryGetArrayField(TEXT("was"), Was)) continue;
			const TArray<TSharedPtr<FJsonValue>>& Rc = S->GetArrayField(TEXT("rect"));
			const int32 X0 = int32(Rc[0]->AsNumber()), Y0 = int32(Rc[1]->AsNumber()), Sw = int32(Rc[2]->AsNumber());
			const FColor& Now = Px[Y0 * W + X0];
			Moved += FString::Printf(TEXT(" resampled %d,%d,%d -> %d,%d,%d;"), Ref[Y0 * W + X0].R, Ref[Y0 * W + X0].G, Ref[Y0 * W + X0].B, Now.R, Now.G, Now.B);
			for (int32 y = Y0; y < Y0 + Sw; ++y) for (int32 x = X0; x < X0 + Sw; ++x) Ref[y * W + x] = Now;
		}
		int32 Differ = 0, Off2 = 0, MaxD = 0;
		for (int32 i = 0; i < Px.Num(); ++i)
		{
			const int32 D = FMath::Max(FMath::Max(FMath::Abs(Px[i].R - Ref[i].R), FMath::Abs(Px[i].G - Ref[i].G)), FMath::Max(FMath::Abs(Px[i].B - Ref[i].B), FMath::Abs(Px[i].A - Ref[i].A)));
			Differ += D > 0 ? 1 : 0;
			Off2 += D > 2 ? 1 : 0;
			MaxD = FMath::Max(MaxD, D);
		}
		Exact += Differ == 0 ? 1 : 0;
		MaxAll = FMath::Max(MaxAll, MaxD);
		// the geometry: the shipped OBJ against the dev one (corners and bounds)
		OutlawsObj::FObjMesh Sh, De;
		const FString DevObj = FPaths::Combine(Dev, bHeld ? TEXT("items3d/held") : TEXT("items3d"), Name + TEXT(".obj"));
		const bool bSh = OutlawsObj::Load(ObjPath, 1.0, true, Sh), bDe = OutlawsObj::Load(DevObj, 1.0, true, De);
		int32 VSh = 0, VDe = 0;
		for (const TPair<FString, FOutlawsMeshSection>& Kv : Sh.Sections) VSh += Kv.Value.Positions.Num();
		for (const TPair<FString, FOutlawsMeshSection>& Kv : De.Sections) VDe += Kv.Value.Positions.Num();
		const double BDiff = bSh && bDe && Sh.Bounds.IsValid && De.Bounds.IsValid ? FMath::Max((Sh.Bounds.Min - De.Bounds.Min).GetAbsMax(), (Sh.Bounds.Max - De.Bounds.Max).GetAbsMax()) : 1e9;
		const bool bGeo = bSh && bDe && VSh == VDe && Sh.Sections.Num() == De.Sections.Num() && BDiff < 0.01;
		// the dev texture painted from the 8-bit sprite blown up 4x (this PC's sprites_hd fell back to it for some props): its
		// sprite rows in 4x4 blocks of one colour.  The player's HD sprite paints those sharper; measured by the mean
		// colour difference where both are solid instead
		int32 Rows = H;
		const TSharedPtr<FJsonObject>* Fill = nullptr;
		if (J->TryGetObjectField(TEXT("fill"), Fill)) Rows = FMath::Min(H, int32((*Fill)->GetArrayField(TEXT("rows"))[1]->AsNumber()));
		bool bDev8 = false;
		for (int32 Oy = 0; Oy < 4 && !bDev8; ++Oy)
			for (int32 Ox = 0; Ox < 4 && !bDev8; ++Ox)
			{
				int32 Blocks = 0, Flat = 0;
				for (int32 by = Oy; by + 4 <= Rows; by += 4)
					for (int32 bx = Ox; bx + 4 <= W; bx += 4)
					{
						const FColor C0 = Ref[by * W + bx];
						if (C0.A <= 127) continue;
						bool bSame = true;
						for (int32 k = 0; k < 16 && bSame; ++k) bSame = Ref[(by + k / 4) * W + bx + k % 4] == C0;
						++Blocks;
						Flat += bSame ? 1 : 0;
					}
				bDev8 = Blocks > 0 && Flat * 10 >= Blocks * 7;
			}
		double SumD = 0.0, SumRt[3] = { 0, 0, 0 }, SumDev[3] = { 0, 0, 0 };
		int32 Both = 0, Either = 0, NRt = 0, NDev = 0;
		for (int32 i = 0; i < Rows * W; ++i)
		{
			const bool bR = Px[i].A > 127, bD = Ref[i].A > 127;
			Either += bR || bD ? 1 : 0;
			if (bR) { SumRt[0] += Px[i].R; SumRt[1] += Px[i].G; SumRt[2] += Px[i].B; ++NRt; }
			if (bD) { SumDev[0] += Ref[i].R; SumDev[1] += Ref[i].G; SumDev[2] += Ref[i].B; ++NDev; }
			if (!bR || !bD) continue;
			SumD += (FMath::Abs(Px[i].R - Ref[i].R) + FMath::Abs(Px[i].G - Ref[i].G) + FMath::Abs(Px[i].B - Ref[i].B)) / 3.0;
			++Both;
		}
		const double MeanD = Both ? SumD / Both : 0.0, Overlap = Either ? double(Both) / Either : 1.0;
		double Tint = 0.0;
		for (int32 c = 0; c < 3; ++c) Tint = FMath::Max(Tint, FMath::Abs((NRt ? SumRt[c] / NRt : 0.0) - (NDev ? SumDev[c] / NDev : 0.0)));
		// texel for texel, or (a port that can not match Pillow or numpy exactly) no more than 1 in 200 texels off by over 2;
		// against an 8-bit dev texture, the same view: 40 % of its solid shape shared at least (the HD frame's edges and the
		// plants' thin stems differ from the blown-up 8-bit one, RPLANT06 44 %; a wrong view, the cannon head on, shared 37 %)
		const bool bTexOk = bDev8 ? Overlap >= 0.4 : Off2 * 200 <= Px.Num();
		Check(bTexOk && bGeo, Tag, FString::Printf(TEXT("painted %dx%d in %.1f ms: %d texels differ (%d by more than 2, max %d, mean %.1f where solid)%s;%s geometry %d corners vs %d, %d sections vs %d, bounds diff %.4f cm"),
			W, H, Ms, Differ, Off2, MaxD, MeanD, bDev8 ? *FString::Printf(TEXT(", the dev texture from the 8-bit sprite (HD now): shape %.0f %% the same, average colour %.1f apart"), Overlap * 100.0, Tint) : TEXT(""),
			*Moved, VSh, VDe, Sh.Sections.Num(), De.Sections.Num(), BDiff));
	}
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT item3dship paint: %d recipes, %d texel for texel, max channel difference %d, %.0f ms in all, the slowest %s %.1f ms"),
		Recipes.Num(), Exact, MaxAll, TotalMs, *Worst, WorstMs);
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT item3dship RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
	FGenericPlatformMisc::RequestExit(false);
}

#endif
