// REQ-NATIVEMAPS (user 3 Nov), the full probe of the shipped community maps:
//   -OLShots -OLNativeMaps=<dir> -OLMpHost=drygulch -OLMpClass=dm -OLMpBots=0 -OLMpStartWhen=0 -OLNativeMapsProbe
// The launched host, alone, hosts every native map in EACH game its RCM's MODES line allows (StartNextMatch, as the Host Game
// page's Start between matches; about 85 cases for the 19), each case: the level builds under its own overlay (no "ERROR",
// sectors > 0), the class is the one asked, the rules have the host in with spawn points read, the mode's objects are there
// (both flags for Capture The Flag, the chicken for Kill The Fool With The Chicken, the document for Secret Document), and in
// the games bots play (Death Match, Team Play) one bot in the rules.  One line a case "OLSHOT nativemaps case PASS|FAIL: ...",
// one a map "OLSHOT nativemaps map <name> <passed>/<cases> ...", the table lines, then "OLSHOT nativemaps RESULT ... in N s".
// (REQ-MAPQUICK's -OLNativeMapsQuickProbe, 1630, stays the quick one: each map in its first mode, with a shot.)

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsLevel.h"
#include "OutlawsCoop.h"
#include "OutlawsMpHost.h"
#include "OutlawsMatchModes.h"
#include "OutlawsLab.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsNativeMapsProbeLocal
{
	struct FCase { int32 Map = 0; EOutlawsGameClass Class = EOutlawsGameClass::DeathMatch; };
	int32 Stage = 0, Passes = 0, Fails = 0, Next = -1;
	double StageAt = 0.0, ReqAt = 0.0, BeganAt = 0.0;
	FString Target;
	TArray<FOutlawsMpMap> Natives;
	TArray<FCase> AllCases;
	TArray<int32> MapPass, MapCases;
	TArray<FString> MapModes, Table;

	void Line(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT nativemaps case %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}

	bool HasItem(const AOutlawsCoopSession* S, EOutlawsModeItemKind K)
	{
		return S && S->ModeItems.ContainsByPredicate([K](const FOutlawsModeItemRep& R) { return R.Kind == uint8(K); });
	}
}

OL_SHOT_MATCH(OLNativeMapsProbe) { return OLFlags::Param(TEXT("OLNativeMapsProbe")); }

OL_SHOT_TEST(OLNativeMapsProbe, 1790)
{
	using namespace OutlawsNativeMapsProbeLocal;
	if (!L || ShotTime < 0.f || Stage < 0) return;
	const double Now = FPlatformTime::Seconds();
	L->StartSaveDelay = -1.f;           // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	UOutlawsCoop* C = UOutlawsCoop::Get(this);
	auto Finish = [&]()
	{
		for (int32 m = 0; m < Natives.Num(); ++m)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT nativemaps map %s %d/%d (%s)"), *Natives[m].Name, MapPass.IsValidIndex(m) ? MapPass[m] : 0, MapCases.IsValidIndex(m) ? MapCases[m] : 0,
				MapModes.IsValidIndex(m) ? *MapModes[m] : TEXT(""));
		}
		for (const FString& T : Table) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT nativemaps table %s"), *T);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT nativemaps RESULT %s (%d passed, %d failed; %d cases on %d maps) in %.0f s"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"),
			Passes, Fails, AllCases.Num(), Natives.Num(), BeganAt > 0.0 ? Now - BeganAt : 0.0);
		ShotTime = -1.f;
		Stage = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	if (Stage == 0)
	{
		// the list and the cases: each native map in each game its RCM allows
		const FOutlawsAssets* A = L->GetLabAssets();
		if (!A || ShotTime < 2.f) return;
		TArray<FOutlawsMpMap> Maps;
		OutlawsMpHost::ListMaps(A, Maps);
		Natives = Maps.FilterByPredicate([](const FOutlawsMpMap& M) { return M.bNative; });
		MapPass.Init(0, Natives.Num());
		MapCases.Init(0, Natives.Num());
		MapModes.Init(FString(), Natives.Num());
		for (int32 m = 0; m < Natives.Num(); ++m)
		{
			for (int32 c = 1; c <= 6; ++c)
			{
				if (!(Natives[m].Modes & (1u << (c - 1)))) continue;
				AllCases.Add({ m, EOutlawsGameClass(c) });
				++MapCases[m];
			}
			MapModes[m] = Natives[m].ModesText();
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT nativemaps: %d native maps, %d cases (each map in each game its RCM allows)"), Natives.Num(), AllCases.Num());
		if (AllCases.Num() == 0)
		{
			Line(false, TEXT("no native maps listed (-OLNativeMaps=<the folder>)"));
			Finish();
			return;
		}
		StageAt = Now;
		Stage = 4;
		return;
	}
	if (Stage == 4)
	{
		// the launched host's match up first
		if (!C || !C->IsHost() || !C->IsMatch() || !C->bLevelBuilt)
		{
			if (Now - StageAt < 90.0) return;
			Line(false, TEXT("needs a launched host: -OLMpHost=drygulch -OLMpClass=dm -OLMpBots=0 -OLMpStartWhen=0"));
			Finish();
			return;
		}
		BeganAt = Now;
		Next = -1;
		Stage = 3;
		StageAt = Now - 10.0;
		return;
	}
	if (!C) return;
	AOutlawsCoopSession* S = C->GetSession();
	const FCase* K = AllCases.IsValidIndex(Next) ? &AllCases[Next] : nullptr;
	const FOutlawsMpMap* M = K ? &Natives[K->Map] : nullptr;
	const FString Label = M ? FString::Printf(TEXT("%s %s"), *M->Name, *OutlawsMpHost::ClassLabel(K->Class)) : FString();
	if (Stage == 3)
	{
		if (Now - StageAt < 0.5) return;
		++Next;
		if (Next >= AllCases.Num()) { Finish(); return; }
		const FCase& NK = AllCases[Next];
		const FOutlawsMpMap& NM = Natives[NK.Map];
		FOutlawsMpHostOptions O = C->MatchOptions;
		O.bRandomMap = O.bRandomClass = false;
		O.MapFile = NM.File;
		O.GameClass = NK.Class;
		O.bTeams = false;
		O.bBots = OutlawsMpHost::BotsAllowed(NK.Class);       // (one bot where bots play)
		O.BotCount = 1;
		Target = NM.Level;
		ReqAt = Now;
		StageAt = Now;
		Stage = 1;
		if (!C->StartNextMatch(O, TEXT("the native maps probe")))
		{
			Line(false, FString::Printf(TEXT("%s %s: StartNextMatch refused"), *NM.Name, *OutlawsMpHost::ClassLabel(NK.Class)));
			Table.Add(FString::Printf(TEXT("| %s | %s | - | - | refused |"), *NM.Name, *OutlawsMpHost::ClassLabel(NK.Class)));
			Stage = 3;
		}
		return;
	}
	if (!M) return;
	if (Stage == 1)
	{
		// the level of this start: built after the request, the class asked, the host in the rules, nothing pending
		const bool bBuilt = C->bLevelBuilt && C->LevelBuiltAt > ReqAt && C->BuiltLevel.Equals(Target, ESearchCase::IgnoreCase) && L->LevelName.Equals(Target, ESearchCase::IgnoreCase)
			&& !L->IsRebuildPending() && C->bMatchSetUp && S && S->bHostReady && C->MatchInRules[0] && !bMenuOpen;
		if (!bBuilt)
		{
			if (Now - StageAt < 120.0) return;
			const FString Report = L->GetBuildReport().Left(200);
			Line(false, FString::Printf(TEXT("%s: not ready in 120 s (built %d as %s, set up %d; report: %s)"), *Label, C->bLevelBuilt ? 1 : 0, *C->BuiltLevel, C->bMatchSetUp ? 1 : 0, *Report));
			Table.Add(FString::Printf(TEXT("| %s | %s | - | - | not ready |"), *M->Name, *OutlawsMpHost::ClassLabel(K->Class)));
			Stage = 3;
			StageAt = Now;
			return;
		}
		Stage = 2;
		StageAt = Now;
		return;
	}
	if (Stage == 2)
	{
		if (Now - StageAt < 2.0) return;     // (the modes' objects placed, a bot in)
		const FOutlawsLevel* Lv = L->LiveLevel();
		const int32 Sectors = Lv ? Lv->Sectors.Num() : 0;
		const int32 Spawns = C->Dm.GetSpawns().Num();
		const FString Report = L->GetBuildReport();
		const bool bLevel = Sectors > 0 && !Report.StartsWith(TEXT("ERROR"));
		const bool bClass = S && S->MatchClass == uint8(K->Class);
		const bool bIn = Spawns > 0 && C->MatchInRules[0];
		bool bItems = true;
		FString Items = TEXT("none needed");
		switch (K->Class)
		{
		case EOutlawsGameClass::CaptureTheFlag:
			bItems = HasItem(S, EOutlawsModeItemKind::RedFlag) && HasItem(S, EOutlawsModeItemKind::BlueFlag);
			Items = bItems ? TEXT("both flags") : TEXT("a flag MISSING");
			break;
		case EOutlawsGameClass::Chicken:
			bItems = HasItem(S, EOutlawsModeItemKind::Chicken);
			Items = bItems ? TEXT("the chicken") : TEXT("the chicken MISSING");
			break;
		case EOutlawsGameClass::SecretDocument:
			bItems = HasItem(S, EOutlawsModeItemKind::Document);
			Items = bItems ? TEXT("the document") : TEXT("the document MISSING");
			break;
		case EOutlawsGameClass::Tag:
			Items = FString::Printf(TEXT("it: player %d"), S ? S->ItPlayer : -1);
			break;
		default:
			break;
		}
		const bool bWantBot = OutlawsMpHost::BotsAllowed(K->Class);
		const int32 NumBots = C->Bots.Num();
		const bool bBotIn = NumBots > 0 && C->Bots[0].Who >= 0 && C->Bots[0].Who < UOutlawsCoop::MaxPlayers && C->MatchInRules[C->Bots[0].Who];
		const bool bBot = !bWantBot || bBotIn;
		const bool bOk = bLevel && bClass && bIn && bItems && bBot;
		const double LoadMs = (C->LevelBuiltAt - ReqAt) * 1000.0;
		Line(bOk, FString::Printf(TEXT("%s (%s, level %s): sectors %d, load %.0f ms, class %d (want %d), spawns %d, host in %d, items: %s, bots %d%s%s"), *Label, *M->File, *M->Level,
			Sectors, LoadMs, S ? int32(S->MatchClass) : -1, int32(K->Class), Spawns, C->MatchInRules[0] ? 1 : 0, *Items, NumBots,
			bWantBot ? (bBotIn ? TEXT(" (one in the rules)") : TEXT(" (NONE in the rules)")) : TEXT(""), bLevel ? TEXT("") : *(TEXT("; ") + Report.Left(160))));
		if (bOk) ++MapPass[K->Map];
		Table.Add(FString::Printf(TEXT("| %s | %s | %d | %.0f | %s | %s |"), *M->Name, *OutlawsMpHost::ClassLabel(K->Class), Sectors, LoadMs, *Items, bOk ? TEXT("PASS") : TEXT("FAIL")));
		Stage = 3;
		StageAt = Now;
	}
}

#endif // WITH_OUTLAWS_TESTS
