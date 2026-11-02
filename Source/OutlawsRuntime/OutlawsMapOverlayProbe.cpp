// REQ-NATIVEMAPS part 1's probe (user 2 Nov: a map's own files apply ONLY on their own map), the per-map overlay:
//   -OLShots -OLNativeMaps=<dir> -OLMpHost=drygulch -OLMpClass=dm -OLMpBots=0 -OLMpStartWhen=0 -OLMapOverlayProbe
// The launched host, alone, hosts ColdWar, Frostbite, a stock map, Bankrupt, Frostbite, ColdWar, the stock map in Death Match
// (StartNextMatch, as the Host Game page's Start between matches).  On each level: the overlay is that map's LAB (none on the
// stock map); every name in the three maps' LABs resolves as the original would (the map's own bytes where its LAB has the
// name, else the stock file or nothing: CRC of the bytes each way), so ColdWar's and Frostbite's shared names are each map's
// own, Bankrupt's stock overrides apply on Bankrupt only, and the stock map sees stock files only; the level's sound cache
// forgot Bankrupt's dog1 when the map changed.  The leak check: ColdWar after one switch and after six, the LAB files open,
// the textures alive (after a collection) and the memory used, no higher.  One line a host "OLSHOT mapoverlay host PASS|FAIL",
// then "OLSHOT mapoverlay RESULT".  With -OLOldNativeMount (MAPQUICK's mount) the same run shows the old behaviour (no overlay:
// the first map mounted wins a shared name, Bankrupt's overrides never apply).

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsLevel.h"
#include "OutlawsCoop.h"
#include "OutlawsMpHost.h"
#include "OutlawsLab.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Engine/Texture2D.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/Crc.h"
#include "Misc/Paths.h"
#include "UObject/UObjectIterator.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsMapOverlayProbeLocal
{
	int32 Stage = 0, Passes = 0, Fails = 0, Next = -1;
	double StageAt = 0.0;
	FString Target;
	struct FHost { FOutlawsMpMap Map; TSharedPtr<FOutlawsLabFile> Lab; };     // (Lab null: the stock map)
	TArray<FHost> Plan;
	TArray<TSharedPtr<FOutlawsLabFile>> NativeLabs;                         // ColdWar, Frostbite, Bankrupt, read here directly
	TUniquePtr<FOutlawsAssets> StockView;                                    // the install alone (no overlay ever opened on it)
	TArray<FString> Names;                                                   // every name the three LABs hold
	TSet<FString> Shared;                                                    // ColdWar's and Frostbite's names with different bytes
	TSet<FString> Overrides;                                                 // Bankrupt's stock names with different bytes
	uint32 BankruptDog = 0;
	int32 Builds = 0, Switches = 0;
	uint32 SerialWas = 0;
	struct FMem { int32 Labs = 0, Textures = 0; double MB = 0.0; bool bSet = false; } MemOne, MemSix;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mapoverlay check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}

	uint32 Crc(const TArray<uint8>& B) { return B.Num() > 0 ? FCrc::MemCrc32(B.GetData(), B.Num()) : 1u; }

	/** A name's bytes from one LAB (0 when it has none). */
	uint32 LabCrc(const FOutlawsLabFile& Lab, const FString& Key)
	{
		const int32* E = Lab.GetIndex().Find(Key);
		TArray<uint8> B;
		return E && Lab.Read(*E, B) ? Crc(B) : 0u;
	}

	/** The stock install's bytes of a name (0 when it is in none of its LABs: -OLOldNativeMount's view also mounts the maps). */
	uint32 StockCrc(const FString& Key)
	{
		const FString W = StockView->Where(Key).ToLower();
		if (W.IsEmpty()) return 0u;
		for (const TSharedPtr<FOutlawsLabFile>& N : NativeLabs) if (FPaths::GetCleanFilename(N->GetPath()).ToLower() == W) return 0u;
		TArray<uint8> B;
		return StockView->Get(Key, B) ? Crc(B) : 0u;
	}

	FMem Measure()
	{
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
		FMem M;
		M.Labs = FOutlawsLabFile::NumLive();
		for (TObjectIterator<UTexture2D> It; It; ++It) ++M.Textures;
		M.MB = double(FPlatformMemory::GetStats().UsedPhysical) / (1024.0 * 1024.0);
		M.bSet = true;
		return M;
	}
}

OL_SHOT_MATCH(OLMapOverlayProbe) { return OLFlags::Param(TEXT("OLMapOverlayProbe")); }

OL_SHOT_TEST(OLMapOverlayProbe, 1750)
{
	using namespace OutlawsMapOverlayProbeLocal;
	if (!L || ShotTime < 0.f || Stage < 0) return;
	const double Now = FPlatformTime::Seconds();
	L->StartSaveDelay = -1.f;           // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	const bool bOld = FOutlawsAssets::OldNativeMount();
	UOutlawsCoop* C = UOutlawsCoop::Get(this);
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mapoverlay RESULT %s (%d passed, %d failed; %d builds, %d overlay switches%s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"),
			Passes, Fails, Builds, Switches, bOld ? TEXT(", -OLOldNativeMount") : TEXT(""));
		NativeLabs.Reset();
		StockView.Reset();
		ShotTime = -1.f;
		Stage = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	if (Stage == 0)
	{
		// the launched stock match up first, then the plan from the list
		const FOutlawsAssets* A = L->GetLabAssets();
		if (!A || !C || !C->IsHost() || !C->IsMatch() || !C->bLevelBuilt || ShotTime < 3.f)
		{
			if (ShotTime < 90.f) return;
			Check(false, TEXT("needs a launched host: -OLMpHost=drygulch -OLMpClass=dm -OLMpBots=0 -OLMpStartWhen=0"));
			Finish();
			return;
		}
		TArray<FOutlawsMpMap> Maps;
		OutlawsMpHost::ListMaps(A, Maps);
		auto Native = [&Maps](const TCHAR* Lv) { return Maps.FindByPredicate([Lv](const FOutlawsMpMap& M) { return M.bNative && M.Level == Lv; }); };
		const FOutlawsMpMap* Cw = Native(TEXT("coldwar"));
		const FOutlawsMpMap* Fb = Native(TEXT("frostbi"));
		const FOutlawsMpMap* Bk = Native(TEXT("bankrupt"));
		const FString Launched = C->MatchOptions.MapFile;
		const FOutlawsMpMap* St = Maps.FindByPredicate([&Launched](const FOutlawsMpMap& M) { return !M.bCustom && M.File != Launched && M.Allows(EOutlawsGameClass::DeathMatch); });
		if (!Cw || !Fb || !Bk || !St)
		{
			Check(false, FString::Printf(TEXT("the maps: ColdWar %d, Frostbite %d, Bankrupt %d, a stock map %d (want all listed: -OLNativeMaps=<dir>)"), Cw ? 1 : 0, Fb ? 1 : 0, Bk ? 1 : 0, St ? 1 : 0));
			Finish();
			return;
		}
		for (const FOutlawsMpMap* M : { Cw, Fb, Bk })
		{
			TSharedPtr<FOutlawsLabFile> Lab = MakeShared<FOutlawsLabFile>();
			if (!Lab->Open(M->Lab)) { Check(false, FString::Printf(TEXT("%s: its LAB %s does not open"), *M->Name, *M->Lab)); Finish(); return; }
			NativeLabs.Add(Lab);
		}
		StockView = MakeUnique<FOutlawsAssets>();
		StockView->Open(A->GetRoot());
		TSet<FString> All;
		for (const TSharedPtr<FOutlawsLabFile>& N : NativeLabs) for (const TPair<FString, int32>& Kv : N->GetIndex()) All.Add(Kv.Key);
		Names = All.Array();
		Names.Sort();
		for (const TPair<FString, int32>& Kv : NativeLabs[0]->GetIndex())
		{
			const uint32 Other = LabCrc(*NativeLabs[1], Kv.Key);
			if (Other && Other != LabCrc(*NativeLabs[0], Kv.Key) && !StockCrc(Kv.Key)) Shared.Add(Kv.Key);
		}
		for (const TPair<FString, int32>& Kv : NativeLabs[2]->GetIndex())
		{
			const uint32 S = StockCrc(Kv.Key);
			if (S && S != LabCrc(*NativeLabs[2], Kv.Key)) Overrides.Add(Kv.Key);
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mapoverlay plan: %d names in the three LABs; ColdWar and Frostbite share %d with different bytes (%s); Bankrupt overrides %d stock files (%s); the stock map %s"),
			Names.Num(), Shared.Num(), *FString::Join(Shared.Array(), TEXT(" ")), Overrides.Num(), *FString::Join(Overrides.Array(), TEXT(" ")), *St->File);
		Check(Shared.Num() >= 15 && Overrides.Num() >= 3, FString::Printf(TEXT("the cases are there: %d shared names, %d Bankrupt overrides (want 15+, 3+)"), Shared.Num(), Overrides.Num()));
		const TSharedPtr<FOutlawsLabFile> NoLab;
		Plan = { { *Cw, NativeLabs[0] }, { *Fb, NativeLabs[1] }, { *St, NoLab }, { *Bk, NativeLabs[2] }, { *Fb, NativeLabs[1] }, { *Cw, NativeLabs[0] }, { *St, NoLab } };
		SerialWas = FOutlawsAssets::OverlaySerial();
		Next = -1;
		Stage = 3;
		StageAt = Now;
		return;
	}
	if (!C) return;
	AOutlawsCoopSession* S = C->GetSession();
	auto Advance = [&]()
	{
		++Next;
		if (Next >= Plan.Num()) { Finish(); return; }
		const FOutlawsMpMap& M = Plan[Next].Map;
		FOutlawsMpHostOptions O = C->MatchOptions;
		O.bRandomMap = O.bRandomClass = false;
		O.bBots = false;
		O.MapFile = M.File;
		O.GameClass = EOutlawsGameClass::DeathMatch;
		Target = M.Level;
		StageAt = Now;
		Stage = 1;
		if (!C->StartNextMatch(O, TEXT("the map overlay probe")))
		{
			Check(false, FString::Printf(TEXT("%s: StartNextMatch refused"), *M.Name));
			Stage = 3;
		}
	};
	if (Stage == 1)
	{
		const bool bBuilt = C->bLevelBuilt && C->BuiltLevel.Equals(Target, ESearchCase::IgnoreCase) && L->LevelName.Equals(Target, ESearchCase::IgnoreCase)
			&& !L->IsRebuildPending() && C->bMatchSetUp && S && S->bHostReady && !bMenuOpen;
		if (!bBuilt)
		{
			if (Now - StageAt < 120.0) return;
			Check(false, FString::Printf(TEXT("%s: the level %s not ready in 120 s (report: %s)"), *Plan[Next].Map.Name, *Target, *L->GetBuildReport().Left(200)));
			Advance();
			return;
		}
		Stage = 2;
		StageAt = Now;
		return;
	}
	if (Stage == 2)
	{
		if (Now - StageAt < 2.0) return;
		++Builds;
		const FHost& H = Plan[Next];
		const FOutlawsAssets* A = L->GetLabAssets();
		const uint32 Serial = FOutlawsAssets::OverlaySerial();
		const bool bSwitched = Serial != SerialWas;
		if (bSwitched) ++Switches;
		SerialWas = Serial;
		// (1) the overlay is this map's LAB, none on the stock map
		const FString Want = H.Lab ? H.Lab->GetPath() : FString();
		const FString Got = A ? A->GetOverlay() : FString();
		// (2) every name of the three LABs as the original resolves it: this map's own bytes, else the stock file or none
		int32 Wrong = 0, SharedOwn = 0, OverridesRight = 0;
		TArray<FString> WrongNames;
		for (const FString& N : Names)
		{
			const uint32 Own = H.Lab ? LabCrc(*H.Lab, N) : 0u;
			const uint32 Expect = Own ? Own : StockCrc(N);
			TArray<uint8> B;
			const uint32 Actual = A && A->Get(N, B) ? Crc(B) : 0u;
			const bool bRight = Actual == Expect;
			if (!bRight) { ++Wrong; if (WrongNames.Num() < 6) WrongNames.Add(FString::Printf(TEXT("%s (from %s)"), *N, A ? *A->Where(N) : TEXT("-"))); }
			if (Shared.Contains(N) && Own && bRight) ++SharedOwn;
			if (Overrides.Contains(N) && bRight) ++OverridesRight;
		}
		const bool bCw = H.Map.Level == TEXT("coldwar"), bFb = H.Map.Level == TEXT("frostbi"), bBk = H.Map.Level == TEXT("bankrupt");
		const bool bOverlayOk = Got == Want;
		const bool bFilesOk = Wrong == 0 && (!(bCw || bFb) || SharedOwn == Shared.Num()) && OverridesRight == Overrides.Num();
		// (3) the level's sound cache: Bankrupt's own dog1 there, never on the next map
		bool bSoundOk = true;
		FString SoundSaid;
		if (bBk)
		{
			L->WarmWav(TEXT("dog1"));
			BankruptDog = L->DebugCachedWavCrc(TEXT("dog1"));
			bSoundOk = BankruptDog != 0;
			SoundSaid = FString::Printf(TEXT("; dog1 cached %08x"), BankruptDog);
		}
		else if (BankruptDog)
		{
			const uint32 Before = L->DebugCachedWavCrc(TEXT("dog1"));
			L->WarmWav(TEXT("dog1"));
			const uint32 After = L->DebugCachedWavCrc(TEXT("dog1"));
			bSoundOk = Before != BankruptDog && After != BankruptDog;
			SoundSaid = FString::Printf(TEXT("; dog1 cached %08x before its warm, %08x after (Bankrupt's %08x)"), Before, After, BankruptDog);
			BankruptDog = 0;
		}
		const bool bOk = bOverlayOk && bFilesOk && bSoundOk && !L->GetBuildReport().StartsWith(TEXT("ERROR"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mapoverlay host %s: %d %s (%s, level %s): overlay %s (want %s)%s; %d of %d names resolve as the original (%d wrong%s%s); shared own %d of %d; Bankrupt overrides right %d of %d%s; forgets %d"),
			bOk ? TEXT("PASS") : TEXT("FAIL"), Builds, *H.Map.Name, *H.Map.File, *H.Map.Level, Got.IsEmpty() ? TEXT("none") : *FPaths::GetCleanFilename(Got), Want.IsEmpty() ? TEXT("none") : *FPaths::GetCleanFilename(Want),
			bSwitched ? TEXT(" (switched)") : TEXT(""), Names.Num() - Wrong, Names.Num(), Wrong, WrongNames.Num() ? TEXT(": ") : TEXT(""), *FString::Join(WrongNames, TEXT(", ")),
			(bCw || bFb) ? SharedOwn : 0, (bCw || bFb) ? Shared.Num() : 0, OverridesRight, Overrides.Num(), *SoundSaid, L->LabForgets);
		if (bOk) ++Passes; else ++Fails;
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("ol_mapoverlay_%d_%s.png"), Builds, *H.Map.Level), false, false);
		// (4) the leak check: ColdWar after one switch (build 1) and after six (build 6)
		if (bCw && Builds == 1) MemOne = Measure();
		if (bCw && Builds == 6)
		{
			MemSix = Measure();
			Check(MemOne.bSet && MemSix.Labs <= MemOne.Labs && MemSix.Textures <= MemOne.Textures + 8 && MemSix.MB <= MemOne.MB + 128.0,
				FString::Printf(TEXT("no leak: ColdWar after 1 switch: %d LAB files open, %d textures, %.0f MB; after %d switches: %d, %d, %.0f MB (want no more LABs, textures within 8, memory within 128 MB)"),
					MemOne.Labs, MemOne.Textures, MemOne.MB, Switches, MemSix.Labs, MemSix.Textures, MemSix.MB));
		}
		Stage = 3;
		StageAt = Now;
		return;
	}
	if (Stage == 3)
	{
		if (Now - StageAt < 1.5) return;     // (the shot taken)
		Advance();
	}
}

#endif // WITH_OUTLAWS_TESTS
