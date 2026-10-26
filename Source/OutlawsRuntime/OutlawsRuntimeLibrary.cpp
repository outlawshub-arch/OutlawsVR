#include "OutlawsRuntimeLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpression.h"
#include "Materials/MaterialExpressionCustom.h"
#include "OutlawsLab.h"
#include "OutlawsLevel.h"
#include "OutlawsFormats.h"
#include "OutlawsGeometry.h"
#include "OutlawsTextures.h"
#include "OutlawsRuntime.h"
#include "Misc/Paths.h"

using namespace OutlawsFormats;

FString UOutlawsRuntimeLibrary::FindInstallRoot(const FString& InstallPath)
{
	return FOutlawsAssets::FindInstallRoot(InstallPath);
}

TArray<FString> UOutlawsRuntimeLibrary::ListLevels(const FString& InstallPath)
{
	TArray<FString> Out;
	const FString Root = FOutlawsAssets::FindInstallRoot(InstallPath);
	TSharedPtr<FOutlawsAssets> A = Root.IsEmpty() ? nullptr : FOutlawsAssets::GetShared(Root);
	if (!A)
	{
		return Out;
	}
	TArray<FString> Lvts;
	A->ListByExtension(TEXT(".lvt"), Lvts);
	for (const FString& L : Lvts)
	{
		const FString Stem = FPaths::GetBaseFilename(L);
		if (A->Has(Stem + TEXT(".obt")))
		{
			Out.Add(Stem);
		}
	}
	return Out;
}

FString UOutlawsRuntimeLibrary::ValidateLevel(const FString& LevelName, const FString& InstallPath, EOutlawsDifficulty Difficulty)
{
	const double T0 = FPlatformTime::Seconds();
	const FString Root = FOutlawsAssets::FindInstallRoot(InstallPath);
	TSharedPtr<FOutlawsAssets> AP = Root.IsEmpty() ? nullptr : FOutlawsAssets::GetShared(Root);
	if (!AP)
	{
		return TEXT("ERROR no install");
	}
	const FOutlawsAssets& A = *AP;
	FString Lvt, Obt;
	if (!A.GetText(LevelName + TEXT(".lvt"), Lvt))
	{
		return FString::Printf(TEXT("ERROR %s.lvt missing"), *LevelName);
	}
	A.GetText(LevelName + TEXT(".obt"), Obt);
	FOutlawsLevel L;
	L.ParseLvt(Lvt);
	L.ParseObt(Obt);

	// texture sizes only (PCX headers), as the exporter uses them for UVs
	TMap<FString, FOutlawsTexInfo> Tex;
	int32 TexOk = 0;
	for (const FString& Name : L.Textures)
	{
		const FString Stem = FPaths::GetBaseFilename(Name).ToUpper();
		const FString Resolved = ResolveTextureName(A, Name);
		TArray<uint8> Bytes;
		FPcxImage Img;
		if (!Resolved.IsEmpty() && A.Get(Resolved, Bytes) && DecodePcx(Bytes, Img))
		{
			FOutlawsTexInfo I;
			I.W = Img.W;
			I.H = Img.H;
			I.bTopLeftMid = Name.EndsWith(TEXT(".atx"), ESearchCase::IgnoreCase) || (Img.W == 640 && Img.H == 480);
			Tex.Add(Stem, I);
			++TexOk;
		}
	}
	FOutlawsGeometry G;
	G.Build(L, Tex, true);
	int32 WorldTris = 0, WaterTris = 0;
	for (const TPair<FString, FOutlawsMeshSection>& Kv : G.World) WorldTris += Kv.Value.NumTriangles();
	for (const TPair<FString, FOutlawsMeshSection>& Kv : G.Water) WaterTris += Kv.Value.NumTriangles();

	// objects: same resolution rules as the exporter; decode sprites without GPU textures
	TArray<FColor> Palette;
	FindSpritePalette(A, Palette);
	TMap<FString, bool> SpriteOk;
	int32 Sprites = 0, Meshes = 0, None = 0, Hanging = 0, Outside = 0, OtherDifficulty = 0;
	const uint32 DiffBit = OutlawsDifficulty::ObjectBit(Difficulty);
	for (const FOutlawsObject& O : L.Objects)
	{
		if (!O.OnDifficulty(DiffBit))
		{
			++OtherDifficulty;
			continue;
		}
		if (!L.SectorById(O.SectorId) && !L.SectorAt(O.Pos.X, O.Pos.Y, O.Pos.Z))
		{
			++Outside;
			continue;
		}
		FString Itm;
		const FString Anim = ResolveAnim(A, O.Name, &Itm);
		if (Anim.EndsWith(TEXT(".nwx"), ESearchCase::IgnoreCase))
		{
			const FString Key = Anim.ToLower();
			if (!SpriteOk.Contains(Key))
			{
				SpriteOk.Add(Key, OutlawsTextures::BuildSpriteAtlas(A, Anim, Palette, false).IsValid());
			}
			if (SpriteOk[Key]) { ++Sprites; if (IsHanging(O.Name, Itm)) ++Hanging; }
			else ++None;
		}
		else if (Anim.EndsWith(TEXT(".3do"), ESearchCase::IgnoreCase))
		{
			++Meshes;
		}
		else
		{
			++None;
		}
	}
	int32 Floors = G.Stats.FindRef(TEXT("floor")), Ceils = G.Stats.FindRef(TEXT("ceil"));
	return FString::Printf(TEXT("%s sectors=%d textures=%d/%d floor=%d ceil=%d wall_mid=%d wall_bot=%d wall_top=%d wall_midtex=%d overlay=%d water=%d tris=%d water_tris=%d fallbacks=%d area_err=%.4f suspect=%d difficulty=%s other_difficulty=%d objects=%d sprite=%d mesh=%d none=%d outside=%d hanging=%d atlases=%d ms=%.0f"),
		*LevelName, L.Sectors.Num(), TexOk, L.Textures.Num(), Floors, Ceils,
		G.Stats.FindRef(TEXT("wall_mid")), G.Stats.FindRef(TEXT("wall_bot")), G.Stats.FindRef(TEXT("wall_top")), G.Stats.FindRef(TEXT("wall_midtex")),
		G.Stats.FindRef(TEXT("wall_overlay")), G.Stats.FindRef(TEXT("water_surface")), WorldTris, WaterTris,
		G.TriangulationFallbacks, G.AreaError, G.SuspectSectors.Num(), OutlawsDifficulty::Name(Difficulty), OtherDifficulty, L.Objects.Num() - OtherDifficulty, Sprites, Meshes, None, Outside, Hanging, SpriteOk.Num(),
		(FPlatformTime::Seconds() - T0) * 1000.0);
}

void UOutlawsRuntimeLibrary::RunAdjoinScan(const FString& InstallPath)
{
	const FString Root = FOutlawsAssets::FindInstallRoot(InstallPath);
	TSharedPtr<FOutlawsAssets> AP = Root.IsEmpty() ? nullptr : FOutlawsAssets::GetShared(Root);
	if (!AP)
	{
		UE_LOG(LogOutlaws, Error, TEXT("OLSCAN no install"));
		return;
	}
	const FOutlawsAssets& A = *AP;
	const TArray<FString> Levels = ListLevels(InstallPath);
	UE_LOG(LogOutlaws, Display, TEXT("OLSCAN %d levels under %s"), Levels.Num(), *Root);
	int32 TotalOld = 0, TotalNew = 0, TotalSigns = 0, LevelsChanged = 0;
	for (const FString& Name : Levels)
	{
		FString Lvt;
		if (!A.GetText(Name + TEXT(".lvt"), Lvt)) continue;
		FOutlawsLevel L;
		L.ParseLvt(Lvt);
		TMap<FString, FOutlawsTexInfo> Tex;
		for (const FString& T : L.Textures)
		{
			const FString Resolved = ResolveTextureName(A, T);
			TArray<uint8> Bytes;
			FPcxImage Img;
			if (!Resolved.IsEmpty() && A.Get(Resolved, Bytes) && DecodePcx(Bytes, Img))
			{
				FOutlawsTexInfo I;
				I.W = Img.W;
				I.H = Img.H;
				I.bTopLeftMid = T.EndsWith(TEXT(".atx"), ESearchCase::IgnoreCase) || (Img.W == 640 && Img.H == 480);
				Tex.Add(FPaths::GetBaseFilename(T).ToUpper(), I);
			}
		}
		TArray<FString> AuditOld, AuditNew;
		FOutlawsGeometry GOld, GNew;
		FOutlawsGeometry::bOldSignRule = true;
		GOld.Audit = &AuditOld;
		GOld.Build(L, Tex, true);
		FOutlawsGeometry::bOldSignRule = false;
		GNew.Audit = &AuditNew;
		GNew.Build(L, Tex, true);
		// DEFAULT lines: the placeholder drawn on an adjoined wall (DEFAULT bands were already left out by BUG-DEFAULTBAND
		// in both builds, so these are its signs).  SIGN lines: a real sign drawn partly over an opening, listed in the
		// new build to look at (the rule leaves them as they were)
		auto Count = [](const TArray<FString>& Au, const TCHAR* Kind) { int32 N = 0; for (const FString& S : Au) N += S.StartsWith(Kind) ? 1 : 0; return N; };
		const int32 OldDefault = Count(AuditOld, TEXT("DEFAULT")), NewLeft = Count(AuditNew, TEXT("DEFAULT")), Signs = Count(AuditNew, TEXT("SIGN"));
		for (const FString& S : AuditOld) if (S.StartsWith(TEXT("DEFAULT"))) UE_LOG(LogOutlaws, Display, TEXT("OLSCAN old %s %s"), *Name, *S);
		for (const FString& S : AuditNew) UE_LOG(LogOutlaws, Display, TEXT("OLSCAN new %s %s"), *Name, *S);
		TotalOld += OldDefault;
		TotalNew += NewLeft;
		TotalSigns += Signs;
		int32 TrisOld = 0, TrisNew = 0;
		for (const TPair<FString, FOutlawsMeshSection>& Kv : GOld.World) TrisOld += Kv.Value.NumTriangles();
		for (const TPair<FString, FOutlawsMeshSection>& Kv : GNew.World) TrisNew += Kv.Value.NumTriangles();
		TSet<FString> Keys;
		for (const TPair<FString, int32>& Kv : GOld.Stats) Keys.Add(Kv.Key);
		for (const TPair<FString, int32>& Kv : GNew.Stats) Keys.Add(Kv.Key);
		TArray<FString> Diffs;
		for (const FString& K : Keys)
		{
			const int32 O = GOld.Stats.FindRef(K), N = GNew.Stats.FindRef(K);
			if (O != N) Diffs.Add(FString::Printf(TEXT("%s %d->%d"), *K, O, N));
		}
		Diffs.Sort();
		if (Diffs.Num() > 0) ++LevelsChanged;
		UE_LOG(LogOutlaws, Display, TEXT("OLSCAN level %-9s DEFAULT on an adjoin: old %d new %d, signs over an opening (unchanged) %d | world tris %d->%d | sections %d->%d | %s"),
			*Name, OldDefault, NewLeft, Signs, TrisOld, TrisNew, GOld.World.Num(), GNew.World.Num(),
			Diffs.Num() ? *FString::Join(Diffs, TEXT(", ")) : TEXT("no count changed"));
	}
	UE_LOG(LogOutlaws, Display, TEXT("OLSCAN total DEFAULT on an adjoin: old %d new %d; signs over an opening (unchanged) %d; over %d levels (%d with any count changed)"),
		TotalOld, TotalNew, TotalSigns, Levels.Num(), LevelsChanged);
}

void UOutlawsRuntimeLibrary::RunWallFacingScan(const FString& InstallPath)
{
	const FString Root = FOutlawsAssets::FindInstallRoot(InstallPath);
	TSharedPtr<FOutlawsAssets> AP = Root.IsEmpty() ? nullptr : FOutlawsAssets::GetShared(Root);
	if (!AP)
	{
		UE_LOG(LogOutlaws, Error, TEXT("OLFACING no install"));
		return;
	}
	const FOutlawsAssets& A = *AP;
	const TArray<FString> Levels = ListLevels(InstallPath);
	UE_LOG(LogOutlaws, Display, TEXT("OLFACING %d levels under %s"), Levels.Num(), *Root);
	// inside the sector by the even-odd rule over all its walls (outer ring and holes alike, whatever their order)
	auto Inside = [](const FOutlawsSector& S, double X, double Z) { return FOutlawsLevel::InsideWalls(S, X, Z); };
	// a wall faces out when a point a hair off its middle along the inward normal is outside its sector and the point as
	// far the other way is inside (walls with both or neither side inside are counted apart, as unclear)
	auto CountOut = [&Inside](const FOutlawsSector& S, bool bCW, int32& OutUnclear)
	{
		int32 Out = 0;
		for (const FOutlawsWall& W : S.Walls)
		{
			if (!S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) continue;
			const FVector2D Pa = S.Verts[W.V1], Pb = S.Verts[W.V2];
			const double EX = Pb.X - Pa.X, EZ = Pb.Y - Pa.Y;
			const double Len = FMath::Sqrt(EX * EX + EZ * EZ);
			if (Len < 1e-6) continue;
			const FVector2D N = FVector2D(bCW ? EZ : -EZ, bCW ? -EX : EX) / Len;
			const FVector2D M = (Pa + Pb) * 0.5;
			const double Eps = FMath::Min(0.01, Len * 0.01);
			const bool bFront = Inside(S, M.X + N.X * Eps, M.Y + N.Y * Eps);
			const bool bBack = Inside(S, M.X - N.X * Eps, M.Y - N.Y * Eps);
			if (bFront == bBack) ++OutUnclear;
			else if (!bFront) ++Out;
		}
		return Out;
	};
	int32 TotWalls = 0, TotOld = 0, TotNew = 0, TotUnclear = 0, TotSecOld = 0, TotSecNew = 0, TotSlopes = 0;
	for (const FString& Name : Levels)
	{
		FString Lvt;
		if (!A.GetText(Name + TEXT(".lvt"), Lvt)) continue;
		FOutlawsLevel L;
		L.ParseLvt(Lvt);
		int32 Walls = 0, Old = 0, New = 0, Unclear = 0, SecOld = 0, SecNew = 0, SlopesTurned = 0;
		TArray<FString> Listed;
		for (const FOutlawsSector& S : L.Sectors)
		{
			if (S.NoWalls()) continue;
			Walls += S.Walls.Num();
			int32 U0 = 0, U1 = 0;
			const int32 O = CountOut(S, S.IsClockwiseByVerts(), U0);
			const int32 N = CountOut(S, S.IsClockwise(), U1);
			Old += O;
			New += N;
			Unclear += U1;
			SecOld += O > 0 ? 1 : 0;
			SecNew += N > 0 ? 1 : 0;
			if (O > 0 && Listed.Num() < 40) Listed.Add(FString::Printf(TEXT("%d(%d/%d%s)"), S.Index, O, S.Walls.Num(), N > 0 ? TEXT(" STILL") : TEXT("")));
		}
		// sloped floors and ceilings whose reference sector's winding changed: their slope tilts the other way now
		for (const FOutlawsSector& S : L.Sectors)
		{
			for (int32 k = 0; k < 2; ++k)
			{
				const FOutlawsSlope& Sl = k == 0 ? S.SlopeFloor : S.SlopeCeil;
				const uint32 Flag = k == 0 ? OutlawsFlags::SlopedFloor : OutlawsFlags::SlopedCeiling;
				if (!Sl.bSet || !(S.Flags & Flag)) continue;
				const FOutlawsSector* Ref = L.Sector(Sl.Sector);
				if (!Ref) Ref = &S;
				if (Ref->IsClockwise() != Ref->IsClockwiseByVerts())
				{
					++SlopesTurned;
					UE_LOG(LogOutlaws, Display, TEXT("OLFACING %s slope turned: sector %d %s (reference sector %d wall %d, angle %.0f)"),
						*Name, S.Index, k == 0 ? TEXT("floor") : TEXT("ceiling"), Ref->Index, Sl.Wall, Sl.Angle);
				}
			}
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLFACING level %-9s walls %5d facing out of their sector: before %4d (%3d sectors), after %4d (%3d sectors); unclear %d; slopes turned %d | %s"),
			*Name, Walls, Old, SecOld, New, SecNew, Unclear, SlopesTurned, *FString::Join(Listed, TEXT(" ")));
		TotWalls += Walls; TotOld += Old; TotNew += New; TotUnclear += Unclear; TotSecOld += SecOld; TotSecNew += SecNew; TotSlopes += SlopesTurned;
	}
	UE_LOG(LogOutlaws, Display, TEXT("OLFACING total over %d levels: %d walls; facing out of their sector before %d (%d sectors), after %d (%d sectors); unclear %d; slopes turned %d"),
		Levels.Num(), TotWalls, TotOld, TotSecOld, TotNew, TotSecNew, TotUnclear, TotSlopes);
}

UMaterialExpression* UOutlawsRuntimeLibrary::GetWorldPositionOffsetExpression(UMaterial* Material)
{
#if WITH_EDITORONLY_DATA
	return Material && Material->GetEditorOnlyData() ? Material->GetEditorOnlyData()->WorldPositionOffset.Expression : nullptr;
#else
	return nullptr;
#endif
}

bool UOutlawsRuntimeLibrary::ConnectWorldPositionOffset(UMaterial* Material, UMaterialExpression* Expression)
{
#if WITH_EDITOR
	if (!Material || !Expression || !Material->GetEditorOnlyData())
	{
		return false;
	}
	Material->GetEditorOnlyData()->WorldPositionOffset.Connect(0, Expression);
	return true;
#else
	return false;
#endif
}

bool UOutlawsRuntimeLibrary::LinkCustomInput(UMaterialExpression* Custom, const FString& Name, UMaterialExpression* From)
{
#if WITH_EDITOR
	UMaterialExpressionCustom* C = Cast<UMaterialExpressionCustom>(Custom);
	if (!C || !From || Name.IsEmpty())
	{
		return false;
	}
	FCustomInput* In = C->Inputs.FindByPredicate([&Name](const FCustomInput& I) { return I.InputName.ToString() == Name; });
	if (!In)
	{
		In = &C->Inputs.AddDefaulted_GetRef();
		In->InputName = FName(*Name);
	}
	In->Input.Connect(0, From);
	C->PostEditChange();
	return true;
#else
	return false;
#endif
}
