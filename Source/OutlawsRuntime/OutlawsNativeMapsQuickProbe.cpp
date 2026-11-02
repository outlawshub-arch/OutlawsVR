// REQ-MAPQUICK's probe (user 2 Nov: the 19 community maps playable now, ahead of REQ-NATIVEMAPS):
//   -OLShots -OLNativeMaps=<dir> -OLMpHost=drygulch -OLMpClass=dm -OLMpBots=0 -OLMpStartWhen=0 -OLNativeMapsQuickProbe
// (1) the list: ListMaps (the Host Game page's list) has a native map for every <dir>/<map>/<name>.rcm, each with its level's
// .lvt found; RollMatch's random map and game, 400 rolls, gives every native map (the pool's count logged).  (2) the launched
// host, alone, then hosts each native map in turn in its first RCM mode (StartNextMatch, as the Host Game page's Start between
// matches): the level builds (no "ERROR: ... not found", sectors > 0), the class is the one asked, the rules have the host in
// with spawn points read, the host stands still on a floor; one shot each at the spawn, looking level
// (Saved/OLShots/ol_mapquick_<map>.png).  One line a map "OLSHOT mapquick map PASS|FAIL: <name> <mode> sectors N load N ms",
// the table line "OLSHOT mapquick table ...", then "OLSHOT mapquick RESULT".  With -OLNoNativeMaps: (1) only, expecting no
// native LAB mounted and no native map listed.

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsLevel.h"
#include "OutlawsCoop.h"
#include "OutlawsMpHost.h"
#include "OutlawsLab.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsNativeMapsQuickProbeLocal
{
	int32 Stage = 0, Passes = 0, Fails = 0, MapPasses = 0, Next = -1;
	double StageAt = 0.0, ReqAt = 0.0, BuiltSeenAt = 0.0;
	float ZAtSettle = 0.f;
	FString Target;                      // the level being waited for
	TArray<FOutlawsMpMap> Natives;
	TArray<FString> Table;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mapquick check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}

	EOutlawsGameClass FirstMode(const FOutlawsMpMap& M)
	{
		// its RCM's first mode in the class order (every one of the 19 has C, T or K)
		for (int32 c = 1; c <= 6; ++c) if (M.Modes & (1u << (c - 1))) return EOutlawsGameClass(c);
		return EOutlawsGameClass::DeathMatch;
	}
}

OL_SHOT_MATCH(OLNativeMapsQuickProbe) { return OLFlags::Param(TEXT("OLNativeMapsQuickProbe")); }

OL_SHOT_TEST(OLNativeMapsQuickProbe, 1630)
{
	using namespace OutlawsNativeMapsQuickProbeLocal;
	if (!L || ShotTime < 0.f || Stage < 0) return;
	const double Now = FPlatformTime::Seconds();
	L->StartSaveDelay = -1.f;           // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	const bool bNoNatives = OLFlags::Param(TEXT("OLNoNativeMaps"));
	UOutlawsCoop* C = UOutlawsCoop::Get(this);
	auto Finish = [&]()
	{
		for (const FString& T : Table) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mapquick table %s"), *T);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mapquick RESULT %s (%d passed, %d failed; %d of %d maps hosted%s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"),
			Passes, Fails, MapPasses, Natives.Num(), bNoNatives ? TEXT(", -OLNoNativeMaps") : TEXT(""));
		ShotTime = -1.f;
		Stage = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	// ---- (1) the list ----
	if (Stage == 0)
	{
		const FOutlawsAssets* A = L->GetLabAssets();
		if (!A || ShotTime < 2.f) return;
		TArray<FOutlawsMpMap> Maps;
		OutlawsMpHost::ListMaps(A, Maps);
		Natives = Maps.FilterByPredicate([](const FOutlawsMpMap& M) { return M.bNative; });
		const int32 Stock = Maps.FilterByPredicate([](const FOutlawsMpMap& M) { return !M.bCustom; }).Num();
		TArray<FString> Names;
		for (const FOutlawsMpMap& M : Natives) Names.Add(FString::Printf(TEXT("%s (%s, %s)"), *M.Name, *M.File, *M.ModesText()));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mapquick natives: %s"), *FString::Join(Names, TEXT("; ")));
		if (bNoNatives)
		{
			Check(Natives.Num() == 0 && A->NumNativeLabs() == 0 && A->GetNativeRoot().IsEmpty() && FOutlawsAssets::NativeMapsRoot().IsEmpty() && Stock >= 6,
				FString::Printf(TEXT("-OLNoNativeMaps: %d native LABs mounted, %d native maps listed, %d stock (want 0, 0, 6 or more)"), A->NumNativeLabs(), Natives.Num(), Stock));
			Finish();
			return;
		}
		// one native map a <root>/<map>/<name>.rcm
		const FString Root = A->GetNativeRoot();
		int32 Rcms = 0;
		TArray<FString> Dirs;
		if (!Root.IsEmpty()) IFileManager::Get().FindFiles(Dirs, *(Root / TEXT("*")), false, true);
		for (const FString& D : Dirs)
		{
			TArray<FString> F;
			IFileManager::Get().FindFiles(F, *(Root / D / TEXT("*.rcm")), true, false);
			Rcms += F.Num();
		}
		Check(!Root.IsEmpty() && A->NumNativeLabs() == Dirs.Num() && Natives.Num() == Rcms && Rcms > 0,
			FString::Printf(TEXT("the native maps' folder %s: %d folders, %d LABs found, %d RCMs, %d native maps listed (the Host Game page lists %d: %d stock + %d native + %d custom)"),
				*Root, Dirs.Num(), A->NumNativeLabs(), Rcms, Natives.Num(), Maps.Num(), Stock, Natives.Num(), Maps.Num() - Stock - Natives.Num()));
		bool bLevels = true;
		for (const FOutlawsMpMap& M : Natives) bLevels &= A->HasLevel(M.Level) && !M.Lab.IsEmpty() && FPaths::FileExists(M.Lab);     // (REQ-NATIVEMAPS: mounted or not)
		Check(bLevels, TEXT("every native map's level .lvt is found and its LAB is there"));
		// RollMatch's pool: random map and game
		const int32 Pool = Maps.FilterByPredicate([](const FOutlawsMpMap& M) { return M.InRandomPool(); }).Num();
		TSet<FString> Seen, SeenNative;
		FOutlawsMpHostOptions O;
		O.MapFile = TEXT("drygulch.rcm");
		O.bRandomMap = O.bRandomClass = true;
		FString Last = O.MapFile;
		EOutlawsGameClass LastC = EOutlawsGameClass::DeathMatch;
		bool bRolled = true;
		for (int32 k = 0; k < 400; ++k)
		{
			FString Said;
			bRolled &= OutlawsMpHost::RollMatch(Maps, O, Last, LastC, Said);
			Seen.Add(O.MapFile);
			if (Natives.ContainsByPredicate([&O](const FOutlawsMpMap& M) { return M.File == O.MapFile; })) SeenNative.Add(O.MapFile);
			Last = O.MapFile;
			LastC = O.GameClass;
		}
		Check(bRolled && Pool == Stock + Natives.Num() && SeenNative.Num() == Natives.Num(),
			FString::Printf(TEXT("RollMatch's pool (random map and game): %d maps (%d stock + %d native); 400 rolls saw %d maps, %d of the %d native"), Pool, Stock, Natives.Num(), Seen.Num(), SeenNative.Num(), Natives.Num()));
		StageAt = Now;
		Stage = 4;
		return;
	}
	if (Stage == 4)
	{
		// (2) the launched host's match up (its menu walk first)
		if (!C || !C->IsHost() || !C->IsMatch() || C->LevelToStart.IsEmpty())
		{
			if (Now - StageAt < 90.0) return;
			Check(false, TEXT("(2) needs a launched host: -OLMpHost=drygulch -OLMpClass=dm -OLMpBots=0 -OLMpStartWhen=0"));
			Finish();
			return;
		}
		Target = C->LevelToStart;
		ReqAt = Now;
		StageAt = Now;
		Next = -1;                       // (the launched stock match first, then the natives)
		Stage = 1;
		return;
	}
	if (!C) return;
	AOutlawsCoopSession* S = C->GetSession();
	const bool bLast = Next >= Natives.Num() - 1;
	auto Advance = [&]()
	{
		++Next;
		if (Next >= Natives.Num()) { Finish(); return; }
		const FOutlawsMpMap& M = Natives[Next];
		FOutlawsMpHostOptions O = C->MatchOptions;
		O.bRandomMap = O.bRandomClass = false;
		O.bBots = false;
		O.MapFile = M.File;
		O.GameClass = FirstMode(M);
		Target = M.Level;
		ReqAt = Now;
		StageAt = Now;
		BuiltSeenAt = 0.0;
		Stage = 1;
		if (!C->StartNextMatch(O, TEXT("the native maps probe")))
		{
			Check(false, FString::Printf(TEXT("%s: StartNextMatch refused"), *M.Name));
			Table.Add(FString::Printf(TEXT("| %s | %s | - | - | refused |"), *M.Name, *OutlawsMpHost::ClassLabel(O.GameClass)));
			Stage = 3;                   // (straight on to the next)
		}
	};
	const FOutlawsMpMap* M = Natives.IsValidIndex(Next) ? &Natives[Next] : nullptr;
	const FString Name = M ? M->Name : TEXT("the launched stock map");
	if (Stage == 1)
	{
		// waiting for the level: built, the host in the rules, entered, nothing pending
		const bool bBuilt = C->bLevelBuilt && C->BuiltLevel.Equals(Target, ESearchCase::IgnoreCase) && L->LevelName.Equals(Target, ESearchCase::IgnoreCase)
			&& !L->IsRebuildPending() && C->bMatchSetUp && S && S->bHostReady && C->MatchInRules[0] && !bMenuOpen;
		if (!bBuilt)
		{
			if (Now - StageAt < 120.0) return;
			const FString Report = L->GetBuildReport().Left(200);
			Check(false, FString::Printf(TEXT("%s: the level %s not ready in 120 s (built %d as %s, level actor %s, set up %d, entered %d; report: %s)"), *Name, *Target,
				C->bLevelBuilt ? 1 : 0, *C->BuiltLevel, *L->LevelName, C->bMatchSetUp ? 1 : 0, S && S->bHostReady ? 1 : 0, *Report));
			if (M) Table.Add(FString::Printf(TEXT("| %s | %s | - | - | not ready: %s |"), *M->Name, *OutlawsMpHost::ClassLabel(FirstMode(*M)), *Report.Replace(TEXT("\n"), TEXT(" "))));
			Advance();
			return;
		}
		BuiltSeenAt = Now;
		if (AController* Ctl = GetController()) Ctl->SetControlRotation(FRotator(0.0, Ctl->GetControlRotation().Yaw, 0.0));     // (looking level)
		Stage = 2;
		StageAt = Now;
		return;
	}
	if (Stage == 2)
	{
		if (Now - StageAt < 1.0) return;
		if (ZAtSettle == 0.f || Now - StageAt < 1.05) ZAtSettle = float(GetActorLocation().Z);
		if (AController* Ctl = GetController()) Ctl->SetControlRotation(FRotator(0.0, Ctl->GetControlRotation().Yaw, 0.0));
		if (Now - StageAt < 2.5) return;
		if (!M)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mapquick: the launched stock match (%s) is up: now the %d native maps"), *Target, Natives.Num());
			ZAtSettle = 0.f;
			Advance();
			return;
		}
		const FOutlawsLevel* Lv = L->LiveLevel();
		const int32 Sectors = Lv ? Lv->Sectors.Num() : 0;
		const int32 Spawns = C->Dm.GetSpawns().Num();
		const FString Report = L->GetBuildReport();
		const EOutlawsGameClass Want = FirstMode(*M);
		const double LoadMs = (C->LevelBuiltAt - ReqAt) * 1000.0;
		const FVector At = GetActorLocation();
		double Near = 1e9;
		for (const FOutlawsDmSpawn& Sp : C->Dm.GetSpawns()) Near = FMath::Min(Near, FVector::Dist2D(Sp.Location, At));
		const float Drop = ZAtSettle - float(At.Z);
		const bool bOk = Sectors > 0 && !Report.StartsWith(TEXT("ERROR")) && S && S->MatchClass == uint8(Want) && Spawns > 0 && C->MatchInRules[0] && FMath::Abs(Drop) < 30.f;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mapquick map %s: %s (%s, level %s) %s sectors %d load %.0f ms; spawns %d, the host %.0f cm from the nearest, dropped %.0f cm in 1.5 s; class %d (want %d)%s"),
			bOk ? TEXT("PASS") : TEXT("FAIL"), *M->Name, *M->File, *M->Level, *OutlawsMpHost::ClassLabel(Want), Sectors, LoadMs, Spawns, Near, Drop, S ? int32(S->MatchClass) : -1, int32(Want),
			Report.StartsWith(TEXT("ERROR")) ? *(TEXT("; ") + Report.Left(160)) : TEXT(""));
		if (bOk) { ++Passes; ++MapPasses; } else ++Fails;
		Table.Add(FString::Printf(TEXT("| %s | %s | %d | %.0f | %s |"), *M->Name, *OutlawsMpHost::ClassLabel(Want), Sectors, LoadMs, bOk ? TEXT("PASS") : TEXT("FAIL")));
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / (TEXT("ol_mapquick_") + FPaths::GetBaseFilename(M->File) + TEXT(".png")), false, false);
		ZAtSettle = 0.f;
		Stage = 3;
		StageAt = Now;
		return;
	}
	if (Stage == 3)
	{
		if (Now - StageAt < 1.5) return;      // (the shot taken)
		if (bLast) { Finish(); return; }
		Advance();
	}
}

#endif // WITH_OUTLAWS_TESTS
