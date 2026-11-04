// REQ-MAPAUTHOR's probe (the user 4 Nov: "to reduce fluff in the menu, I think 'by Molycoat' or by author shouldn't show. just
// show the map name here, and show who it's from in the white text that currently appears when the map/game is loading", then
// "it should be for all of those custom maps, and I dont think it needs (CUSTOM) in brackets ... sort everything alphabetically"):
//   -OLShots -OLNativeMaps=<dir> -OLMpHost=drygulch -OLMpClass=dm -OLMpBots=0 -OLMpStartWhen=0 -OLMapAuthorProbe [-OLOldMapAuthor]
// The names (the name before the last " by ", the whole NAME kept, a map with no author as it was), the Host Game page's MAP page
// (no author, no tag, one list by name, stock and community mixed, photographed), the filters that need the difference (the random
// pool, bCustom), the match lines (the name alone; the white "Next match" line with the author, hosted for real and photographed),
// the lobby's line after it, the line again and its shot once the map has faded in.  -OLMpHost=zekes: the first match's white line
// too.  -OLOldMapAuthor: the same run wants the tag, the "by" and the stock maps first.  One line a check
// "OLSHOT mapauthor check PASS|FAIL: ...", then "OLSHOT mapauthor RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsCoop.h"
#include "OutlawsMpHost.h"
#include "OutlawsLab.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsMapAuthorProbeLocal
{
	int32 Stage = 0, Passes = 0, Fails = 0;
	double StageAt = 0.0;
	TArray<FOutlawsMpMap> Maps;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mapauthor check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}

	void Picture(const TCHAR* Name)
	{
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("ol_mapauthor_%s.png"), Name), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mapauthor shot ol_mapauthor_%s.png"), Name);
	}
}

OL_SHOT_MATCH(OLMapAuthorProbe) { return OLFlags::Param(TEXT("OLMapAuthorProbe")); }

OL_SHOT_TEST(OLMapAuthorProbe, 2424)
{
	using namespace OutlawsMapAuthorProbeLocal;
	if (!L || Stage < 0) return;
	const double Now = FPlatformTime::Seconds();
	L->StartSaveDelay = -1.f;           // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	const bool bOld = OutlawsMpHost::OldMapAuthor();
	UOutlawsCoop* C = UOutlawsCoop::Get(this);
	AOutlawsCoopSession* S = C ? C->GetSession() : nullptr;
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mapauthor RESULT %s (%d passed, %d failed%s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails, bOld ? TEXT(", -OLOldMapAuthor") : TEXT(""));
		ShotTime = -1.f;
		Stage = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	if (Stage == 0)
	{
		const FOutlawsAssets* A = L->GetLabAssets();
		if (!A || !C || !C->IsHost() || !C->IsMatch() || !C->bLevelBuilt || ShotTime < 3.f)
		{
			if (ShotTime < 90.f) return;
			Check(false, TEXT("needs a launched host: -OLMpHost=drygulch -OLMpClass=dm -OLMpBots=0 -OLMpStartWhen=0"));
			Finish();
			return;
		}
		OutlawsMpHost::ListMaps(A, Maps);
		auto Find = [](const TCHAR* Lv) { return Maps.FindByPredicate([Lv](const FOutlawsMpMap& M) { return M.Level.Equals(Lv, ESearchCase::IgnoreCase); }); };
		// (0) hosted straight on a map with an author (-OLMpHost=zekes): the first match has no "Next match" line, so EnterGame says it
		if (S && !bOld && OutlawsMpHost::MapTitleBy(S->MapName) != OutlawsMpHost::MapTitle(S->MapName))
		{
			const FString Line = FString::Join(HudLines, TEXT(" | "));
			Check(Line.Contains(OutlawsMpHost::MapTitleBy(S->MapName)), FString::Printf(TEXT("the first match's white line: \"%s\""), *Line));
			Picture(TEXT("first"));
		}
		// (1) the names
		int32 Natives = 0, WithBy = 0, Short = 0;
		for (const FOutlawsMpMap& M : Maps)
		{
			if (!M.bNative) continue;
			++Natives;
			const bool bBy = M.FullName.Contains(TEXT(" by "), ESearchCase::IgnoreCase);
			WithBy += bBy ? 1 : 0;
			Short += bBy && !bOld && M.FullName.StartsWith(M.Name + TEXT(" ")) && M.Name.Len() < M.FullName.Len() ? 1 : 0;
		}
		Check(Natives > 0 && WithBy > 0 && Short == (bOld ? 0 : WithBy), FString::Printf(TEXT("%d of %d native maps have an author in their RCM's NAME, %d show the name alone"), WithBy, Natives, Short));
		const FOutlawsMpMap* Dawn = Find(TEXT("deaddawn")), *Zekes = Find(TEXT("zekes")), *Az = Find(TEXT("arizonas")), *Bud = Find(TEXT("budville"));
		if (!Dawn || !Zekes || !Az || !Bud) { Check(false, TEXT("deaddawn, zekes, arizonas and budville listed (-OLNativeMaps=<dir>)")); Finish(); return; }
		if (bOld)
		{
			Check(Zekes->Name == TEXT("Zeke's Cabins by Molycoat"), FString::Printf(TEXT("old: the name with its author (%s)"), *Zekes->Name));
		}
		else
		{
			Check(Dawn->Name == TEXT("Dead By Dawn") && Dawn->FullName == TEXT("Dead By Dawn By Sleepy"), FString::Printf(TEXT("the last \" by \" splits it, any case (%s; %s)"), *Dawn->Name, *Dawn->FullName));
			Check(Zekes->Name == TEXT("Zeke's Cabins") && Bud->Name == TEXT("Budville"), FString::Printf(TEXT("%s, %s"), *Zekes->Name, *Bud->Name));
		}
		Check(Az->Name == Az->FullName && Az->Name == TEXT("Arizona Jim's Desert Oasis"), FString::Printf(TEXT("a map with no author shows as it did (%s)"), *Az->Name));
		// (2) the filters that need the difference
		int32 Pool = 0, Custom = 0;
		for (const FOutlawsMpMap& M : Maps) { Pool += M.InRandomPool() ? 1 : 0; Custom += M.bCustom ? 1 : 0; }
		TSet<FString> Rolled;
		FOutlawsMpHostOptions O = C->MatchOptions;
		O.bRandomMap = true;
		for (int32 i = 0; i < 400; ++i)
		{
			FOutlawsMpHostOptions Roll = O;
			FString Said;
			if (OutlawsMpHost::RollMatch(Maps, Roll, FString(), EOutlawsGameClass(0), Said)) Rolled.Add(Roll.MapFile);
		}
		const bool bRolledOk = Maps.ContainsByPredicate([&Rolled](const FOutlawsMpMap& M) { return M.bNative && Rolled.Contains(M.File); })
			&& Maps.ContainsByPredicate([&Rolled](const FOutlawsMpMap& M) { return !M.bCustom && Rolled.Contains(M.File); });
		Check(Custom == Natives && Pool == Maps.Num() && bRolledOk, FString::Printf(TEXT("the random pool: %d of %d maps, %d custom kept apart in the data, 400 rolls gave %d maps, stock and native"), Pool, Maps.Num(), Custom, Rolled.Num()));
		// (3) the lines
		const FString Short2 = UOutlawsCoop::MatchLineFor(6, Zekes->File, 0), By2 = UOutlawsCoop::MatchLineFor(6, Zekes->File, 0, true);
		Check(By2 == TEXT("Death Match on Zeke's Cabins by Molycoat") && (bOld ? Short2 == By2 : Short2 == TEXT("Death Match on Zeke's Cabins")), FString::Printf(TEXT("the lobby's line \"%s\", the white line's \"%s\""), *Short2, *By2));
		Check(UOutlawsCoop::MatchLineFor(6, TEXT("drygulch.rcm"), 0, true) == TEXT("Death Match on Dry Gulch") && OutlawsMpHost::MapTitleBy(TEXT("fort.rcm")) == OutlawsMpHost::MapTitle(TEXT("fort.rcm")), TEXT("a stock map's lines do not change"));
		if (!bMenuOpen) OpenMenu();
		ShowMenuPage(TEXT("#mphost:maps"), true);
		StageAt = Now;
		Stage = 1;
		return;
	}
	if (!C) return;
	if (Stage == 1)
	{
		if (Now - StageAt < 1.5) return;
		// (4) the page: the rows as they read, Random each match first
		TArray<FString> Rows;
		if (MenuPage) for (const OutlawsMenu::FItem& I : MenuPage->Items) if (I.Name.StartsWith(TEXT("mphost_map:")) && I.Name != TEXT("mphost_map:*")) Rows.Add(I.Label);
		int32 CustomTags = 0, Bys = 0, OutOfOrder = 0, Mixes = 0;
		FString Last;
		bool bLastCustom = false;
		for (int32 i = 0; i < Rows.Num(); ++i)
		{
			FString Name = Rows[i];
			const int32 Modes = Name.Find(TEXT("  ["));
			if (Modes != INDEX_NONE) Name.LeftInline(Modes);
			Name.RemoveFromStart(TEXT("> "));
			CustomTags += Rows[i].Contains(TEXT("(custom)")) ? 1 : 0;
			// (a row is a map's short name and nothing else: Dead By Dawn keeps its own "By", the author is what has to go)
			Bys += Maps.ContainsByPredicate([&Name](const FOutlawsMpMap& X) { return X.Name == Name; }) ? 0 : 1;
			OutOfOrder += i > 0 && Name.ToLower() < Last ? 1 : 0;
			const FOutlawsMpMap* M = Maps.FindByPredicate([&Name](const FOutlawsMpMap& X) { return Name.StartsWith(X.Name); });
			const bool bCustomRow = M && M->bCustom;
			Mixes += i > 0 && bCustomRow != bLastCustom ? 1 : 0;
			bLastCustom = bCustomRow;
			Last = Name.ToLower();
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mapauthor rows: %s"), *FString::Join(Rows, TEXT(" | ")).Left(1500));
		if (bOld) Check(Rows.Num() == Maps.Num() && CustomTags > 0 && Mixes == 1, FString::Printf(TEXT("old: %d rows, %d (custom) tags, the stock maps then the rest"), Rows.Num(), CustomTags));
		else Check(Rows.Num() == Maps.Num() && CustomTags == 0 && Bys == 0 && OutOfOrder == 0 && Mixes > 1, FString::Printf(TEXT("the page: %d rows, %d tags, %d authors, %d out of order, stock and community mixed (%d changes)"), Rows.Num(), CustomTags, Bys, OutOfOrder, Mixes));
		Picture(TEXT("menu"));
		StageAt = Now;
		Stage = 2;
		return;
	}
	if (Stage == 2)
	{
		if (Now - StageAt < 1.0) return;
		CloseMenu();
		// (5) hosted for real: the white line, then the lobby's
		const FOutlawsMpMap* Zekes = Maps.FindByPredicate([](const FOutlawsMpMap& M) { return M.Level.Equals(TEXT("zekes"), ESearchCase::IgnoreCase); });
		FOutlawsMpHostOptions O = C->MatchOptions;
		O.bRandomMap = O.bRandomClass = false;
		O.bBots = false;
		O.MapFile = Zekes ? Zekes->File : FString();
		O.GameClass = EOutlawsGameClass::DeathMatch;
		if (!C->StartNextMatch(O, TEXT("the map author probe"))) { Check(false, TEXT("StartNextMatch refused zekes")); Finish(); return; }
		StageAt = Now;
		Stage = 3;
		return;
	}
	if (Stage == 3)
	{
		if (Now - StageAt < 0.6) return;
		const FString Line = FString::Join(HudLines, TEXT(" | "));
		Check(Line.Contains(TEXT("Next match: Death Match on Zeke's Cabins by Molycoat")), FString::Printf(TEXT("the white line as the map loads: \"%s\""), *Line));
		StageAt = Now;
		Stage = 4;
		return;
	}
	if (Stage == 4)
	{
		const bool bBuilt = C->bLevelBuilt && C->BuiltLevel.Equals(TEXT("zekes"), ESearchCase::IgnoreCase) && L->LevelName.Equals(TEXT("zekes"), ESearchCase::IgnoreCase) && !L->IsRebuildPending() && C->bMatchSetUp && S && S->bHostReady;
		if (!bBuilt)
		{
			if (Now - StageAt < 120.0) return;
			Check(false, FString::Printf(TEXT("zekes not ready in 120 s (report: %s)"), *L->GetBuildReport().Left(200)));
			Finish();
			return;
		}
		Check(bOld ? C->MatchLine().Contains(TEXT(" by ")) : !C->MatchLine().Contains(TEXT(" by ")), FString::Printf(TEXT("the lobby and the score page say \"%s\""), *C->MatchLine()));
		StageAt = Now;
		Stage = 5;
		return;
	}
	if (Stage == 5)
	{
		// the screen is black while the map builds: the line is read and photographed once the new map has faded in
		if (Now - StageAt < 1.5) return;
		const FString Line = FString::Join(HudLines, TEXT(" | "));
		Check(Line.Contains(TEXT("Next match: Death Match on Zeke's Cabins by Molycoat")), FString::Printf(TEXT("the line is still up as the map fades in: \"%s\""), *Line));
		Picture(TEXT("load"));
		StageAt = Now;
		Stage = 6;
		return;
	}
	if (Stage == 6 && Now - StageAt > 1.0) Finish();
}
#endif // WITH_OUTLAWS_TESTS
