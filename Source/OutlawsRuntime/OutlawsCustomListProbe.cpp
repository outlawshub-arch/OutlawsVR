// BUG-CUSTOMLIST's probe (the probe sweep 8 Nov: a custom map in the game folder, frostbi.rcm beside frostbi.lab with
// -OLNoNativeMaps, was on the Host Game page as "1 custom"; hosting it built an empty level, "ERROR: frostbi.lvt not found",
// no enemies, no pickups):
//   -OLShots -OLNoFrontEnd -OLLevel=drygulch -OLCustomListProbe [-OLNativeMaps=<dir> | -OLNoNativeMaps]
// Two seconds into the level, the Host Game page's list (OutlawsMpHost::ListMaps, RollMatch's pool takes the same) against the
// files: (a) every map listed has its level's .lvt in a LAB the game reads (FOutlawsAssets::HasLevel: the stock order or a native
// map's), so none hosts an empty level; (b) each game-folder map (<game folder>/<name>.rcm beside <name>.lab, not a stock or
// native name) is listed exactly when its level is found; (c) every native map whose level is found is listed (none with
// -OLNoNativeMaps); (d) every stock map whose level is found is listed (BUG-BUNKERS leaves out the rest).  One line a check
// "OLSHOT customlist check PASS|FAIL: ...", the counts "OLSHOT customlist maps: ...", the game folder's maps "OLSHOT customlist
// game folder: listed <files>; left out <files>", then "OLSHOT customlist RESULT ALL PASS|FAIL" and the game quits.

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsMpHost.h"
#include "OutlawsLab.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsCustomListProbeLocal
{
	int32 Passes = 0, Fails = 0;
	bool bDone = false;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT customlist check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}

	FString Joined(TArray<FString> Names)
	{
		Names.Sort();
		return Names.Num() ? FString::Join(Names, TEXT(" ")) : FString(TEXT("none"));
	}
}

OL_SHOT_MATCH(OLCustomListProbe) { return OLFlags::Param(TEXT("OLCustomListProbe")); }

OL_SHOT_TEST(OLCustomListProbe, 2547)
{
	using namespace OutlawsCustomListProbeLocal;
	if (!L || ShotTime < 2.f || bDone) return;
	L->StartSaveDelay = -1.f;           // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	const FOutlawsAssets* A = L->GetLabAssets();
	if (!A) return;
	bDone = true;
	const bool bNoNatives = OLFlags::Param(TEXT("OLNoNativeMaps"));
	TArray<FOutlawsMpMap> Maps;
	OutlawsMpHost::ListMaps(A, Maps);
	auto Listed = [&Maps](const FString& File) { return Maps.FindByPredicate([&File](const FOutlawsMpMap& M) { return M.File == File.ToLower(); }); };

	// (a) every map listed loads its level
	TArray<FString> NoLevel;
	for (const FOutlawsMpMap& M : Maps) if (!A->HasLevel(M.Level)) NoLevel.Add(FString::Printf(TEXT("%s (level %s)"), *M.File, *M.Level));
	Check(NoLevel.Num() == 0, FString::Printf(TEXT("(a) every one of the %d maps listed has its level's .lvt in a LAB the game reads; without: %s"), Maps.Num(), *Joined(NoLevel)));

	// (d) the stock maps: every RCM the install's LABs hold whose level is found
	TArray<FString> StockRcms, StockWant, StockMissing;
	A->ListByExtension(TEXT(".rcm"), StockRcms);
	TSet<FString> Taken;                 // (the names ListMaps lists first: a later map of the same name is not listed again)
	for (const FString& F : StockRcms)
	{
		FString Text, Why;
		FOutlawsMpMap M;
		if (!A->GetText(F, Text) || !OutlawsMpHost::ParseRcm(Text, M, Why) || !A->Has(M.Level + TEXT(".lvt"))) continue;
		Taken.Add(F.ToLower());
		StockWant.Add(F.ToLower());
		const FOutlawsMpMap* Got = Listed(F);
		if (!Got || Got->bCustom) StockMissing.Add(F.ToLower());
	}
	const int32 NumStock = Maps.FilterByPredicate([](const FOutlawsMpMap& M) { return !M.bCustom; }).Num();
	Check(StockMissing.Num() == 0 && NumStock == StockWant.Num(), FString::Printf(TEXT("(d) the stock maps: %d listed, %d of the %d stock RCMs have their level (missing: %s)"),
		NumStock, StockWant.Num(), StockRcms.Num(), *Joined(StockMissing)));

	// (c) the native maps: each <native root>/<map>/<name>.rcm beside its <name>.lab, level found
	const FString NativeRoot = A->GetNativeRoot();
	int32 NativeWant = 0;
	TArray<FString> NativeMissing;
	if (!NativeRoot.IsEmpty())
	{
		for (const FOutlawsAssets::FNativeMap& Map : FOutlawsAssets::ListNativeMaps(NativeRoot))
		{
			for (const FString& F : Map.Rcms)
			{
				const FString Base = FPaths::GetBaseFilename(F);
				if (!Map.Labs.ContainsByPredicate([&Base](const FString& Lab) { return FPaths::GetBaseFilename(Lab).Equals(Base, ESearchCase::IgnoreCase); })) continue;
				if (Taken.Contains(F.ToLower())) continue;
				FString Text, Why;
				FOutlawsMpMap M;
				if (!FFileHelper::LoadFileToString(Text, *(NativeRoot / Map.Dir / F)) || !OutlawsMpHost::ParseRcm(Text, M, Why) || !A->HasLevel(M.Level)) continue;
				Taken.Add(F.ToLower());
				++NativeWant;
				const FOutlawsMpMap* Got = Listed(F);
				if (!Got || !Got->bNative) NativeMissing.Add(F.ToLower());
			}
		}
	}
	const int32 NumNative = Maps.FilterByPredicate([](const FOutlawsMpMap& M) { return M.bNative; }).Num();
	Check(NativeMissing.Num() == 0 && NumNative == NativeWant && (!bNoNatives || NumNative == 0), FString::Printf(TEXT("(c) the native maps (%s): %d listed, %d wanted (missing: %s)"),
		NativeRoot.IsEmpty() ? TEXT("no native maps' folder") : *NativeRoot, NumNative, NativeWant, *Joined(NativeMissing)));

	// (b) the game folder's maps: listed exactly when their level is found
	const FString Root = A->GetRoot();
	TArray<FString> Found, InList, LeftOut, Wrong;
	IFileManager::Get().FindFiles(Found, *(Root / TEXT("*.rcm")), true, false);
	for (const FString& F : Found)
	{
		const FString Base = FPaths::GetBaseFilename(F);
		bool bLab = false;
		for (const TCHAR* Ext : { TEXT(".lab"), TEXT(".LAB"), TEXT(".Lab") }) bLab |= FPaths::FileExists(Root / Base + Ext);
		if (!bLab || Taken.Contains(F.ToLower())) continue;         // (no LAB beside it, or a stock or native name: that one wins)
		FString Text, Why;
		FOutlawsMpMap M;
		if (!FFileHelper::LoadFileToString(Text, *(Root / F)) || !OutlawsMpHost::ParseRcm(Text, M, Why)) continue;
		const bool bLoads = A->HasLevel(M.Level);
		const FOutlawsMpMap* Got = Listed(F);
		const bool bIn = Got && Got->bCustom && !Got->bNative;
		(bIn ? InList : LeftOut).Add(F.ToLower());
		if (bIn != bLoads) Wrong.Add(FString::Printf(TEXT("%s (level %s %s, %s)"), *F.ToLower(), *M.Level, bLoads ? TEXT("found") : TEXT("in no LAB the game reads"), bIn ? TEXT("listed") : TEXT("left out")));
	}
	Check(Wrong.Num() == 0, FString::Printf(TEXT("(b) the game folder's %d custom maps: each listed exactly when its level is found; wrong: %s"), InList.Num() + LeftOut.Num(), *Joined(Wrong)));

	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT customlist maps: %d listed (%d stock, %d native, %d from the game folder)%s"), Maps.Num(), NumStock, NumNative, InList.Num(),
		bNoNatives ? TEXT(", -OLNoNativeMaps") : TEXT(""));
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT customlist game folder: listed %s; left out %s"), *Joined(InList), *Joined(LeftOut));
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT customlist RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
	ShotTime = -1.f;
	FGenericPlatformMisc::RequestExit(false);
}

#endif // WITH_OUTLAWS_TESTS
