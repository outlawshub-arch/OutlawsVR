// M1, the host's side in the menu (OutlawsMpHost.h): the Host Game page drawn from the remaster's multiplayer_host_start.rml
// (its rows in its order: Start, Max Players, Locations, Games, Minutes, Kills / Captures, Friendly Fire, Allow Cheats; REQ-LOBBYTIDY:
// Start, Map, Game, Minutes, Kills, Bots, Bot skill, More options), the map list, and -OLMpHostShot, the desktop test of the page,
// the maps and the match flow.
#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsMpHost.h"
#include "OutlawsLab.h"
#include "OutlawsLevelActor.h"
#include "OutlawsCoop.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
#include "OutlawsShotTests.h"

namespace
{
	struct FMpHostMenu
	{
		bool bLoaded = false;
		FOutlawsMpHostOptions Options;
		TArray<FOutlawsMpMap> Maps;
	};
	FMpHostMenu& HostMenu()
	{
		static FMpHostMenu M;
		return M;
	}
	const FOutlawsMpMap* FindMap(const FString& File)
	{
		const FMpHostMenu& M = HostMenu();
		return M.Maps.FindByPredicate([&File](const FOutlawsMpMap& X) { return X.File == File; });
	}
	void EnsureHostMenu()
	{
		FMpHostMenu& M = HostMenu();
		if (M.bLoaded) return;
		M.bLoaded = true;
		M.Options.Load();
		const FString Root = FOutlawsAssets::FindInstallRoot(FString());
		const TSharedPtr<FOutlawsAssets> A = Root.IsEmpty() ? nullptr : FOutlawsAssets::GetShared(Root);
		OutlawsMpHost::ListMaps(A.Get(), M.Maps);
		// (no map chosen yet, or one no longer there: the first that plays Death Match)
		if (!FindMap(M.Options.MapFile) && M.Maps.Num() > 0)
		{
			const FOutlawsMpMap* Dm = M.Maps.FindByPredicate([](const FOutlawsMpMap& X) { return X.Allows(EOutlawsGameClass::DeathMatch); });
			M.Options.MapFile = (Dm ? *Dm : M.Maps[0]).File;
		}
		M.Options.Clamp(FindMap(M.Options.MapFile));
	}
	template <typename T>
	T NextOf(const TArray<T>& Steps, const T& Now)
	{
		return OutlawsMenu::StepAmong(Steps, Now);       // (REQ-TRIGGERSTEP: a number row, up or down)
	}
}

void AOutlawsPlayer::MpHostPrefill(const FOutlawsMpHostOptions& In)
{
	// (REQ-MPLOBBY b: the next match's page starts from the last match's settings; "Random each match" stays Random)
	EnsureHostMenu();
	FOutlawsMpHostOptions& O = HostMenu().Options;
	O = In;
	O.Clamp(FindMap(O.MapFile));
	UE_LOG(LogOutlaws, Log, TEXT("OLMPHOST page: filled with the last match's settings (%s)"), *O.Describe());
}

bool AOutlawsPlayer::MpHostKeyboardDone(const FString& Purpose, const FString& Text)
{
	if (Purpose != TEXT("mphost_password")) return false;
	EnsureHostMenu();
	FOutlawsMpHostOptions& O = HostMenu().Options;
	O.Password = UOutlawsCoop::CleanPassword(Text);
	O.Save();
	UE_LOG(LogOutlaws, Log, TEXT("OLMPHOST page: password %s"), O.Password.IsEmpty() ? TEXT("cleared (anyone may join)") : TEXT("set"));
	MenuBack();
	MenuToastText(O.Password.IsEmpty() ? FString(TEXT("No password: anyone may join")) : FString(TEXT("Password set: friends type it on the Join page")));
	return true;
}

void AOutlawsPlayer::BuildMpHostPage(const FString& File, OutlawsMenu::FPage& Page)
{
	using namespace OutlawsMenu;
	EnsureHostMenu();
	FMpHostMenu& M = HostMenu();
	const FOutlawsMpHostOptions& O = M.Options;
	auto Loc = [this](const TCHAR* Key, const TCHAR* Fallback) { const FString S = MenuSource ? MenuSource->Loc(Key) : FString(); return S.IsEmpty() || S.StartsWith(TEXT("$")) ? FString(Fallback) : S; };
	// the stock maps' names are the remaster's strings ("NAME: ^level_fort_rcm" is "The Fort"): put in once
	bool bRenamed = false;
	for (FOutlawsMpMap& Map : M.Maps)
	{
		if (!Map.Name.StartsWith(TEXT("^")) || !MenuSource) continue;
		const FString S = MenuSource->Loc(TEXT("$") + Map.Name.Mid(1));
		if (!S.IsEmpty() && !S.StartsWith(TEXT("$"))) { Map.Name = S; bRenamed = true; }
	}
	if (bRenamed) M.Maps.Sort([](const FOutlawsMpMap& A, const FOutlawsMpMap& B) { return A.bCustom != B.bCustom ? !A.bCustom : A.Name.ToLower() < B.Name.ToLower(); });
	Page.File = File;
	if (File == TEXT("#mphost:maps"))
	{
		// the map list (the remaster's Locations): the stock maps, then the custom ones in the game folder; each with the game
		// classes it allows (REQ-LOBBYTIDY: titled MAP, as the Host Game row that opens it reads "Map:")
		Page.Title = TEXT("MAP");
		{
			// (REQ-MPLOBBY b, decision 4: a stock map rolled for every match, one that plays the chosen game)
			FItem R;
			R.Kind = EItemKind::Action;
			R.Label = FString::Printf(TEXT("%sRandom each match"), O.bRandomMap ? TEXT("> ") : TEXT(""));
			R.Name = TEXT("mphost_map:*");
			Page.Items.Add(R);
		}
		for (const FOutlawsMpMap& Map : M.Maps)
		{
			FItem I;
			I.Kind = EItemKind::Action;
			I.Label = FString::Printf(TEXT("%s%s%s  [%s]"), !O.bRandomMap && Map.File == O.MapFile ? TEXT("> ") : TEXT(""), *Map.Name, Map.bCustom ? TEXT("  (custom)") : TEXT(""), *Map.ModesText());
			I.Name = TEXT("mphost_map:") + Map.File;
			Page.Items.Add(I);
		}
		if (M.Maps.Num() == 0)
		{
			FItem I;
			I.Kind = EItemKind::Heading;
			I.Label = TEXT("No multiplayer maps found");
			Page.Items.Add(I);
		}
		return;
	}
	Page.Title = Loc(TEXT("$m_local_7007_UC"), TEXT("HOST GAME"));
	auto Row = [&Page](EItemKind Kind, const FString& Label, const TCHAR* Name, const FString& Value)
	{
		FItem I;
		I.Kind = Kind;
		I.Label = Label;
		I.Name = Name;
		if (Kind == EItemKind::Select) I.Options = { Value };       // (one option: the row shows "Label:  Value"; a press steps it)
		Page.Items.Add(I);
	};
	const FOutlawsMpMap* Map = FindMap(O.MapFile);
	if (File == TEXT("#mphost:more"))
	{
		// REQ-LOBBYTIDY (user 12 Oct): the rest of the options, a page of their own
		Page.Title = TEXT("MORE OPTIONS");
		Row(EItemKind::Select, Loc(TEXT("$m_multi_lobby_max_players"), TEXT("Max Players")), TEXT("mphost_max"), FString::FromInt(O.MaxPlayers));
		Row(EItemKind::Select, Loc(TEXT("$m_local_7101"), TEXT("Friendly Fire")), TEXT("mphost_ff"), O.bFriendlyFire ? TEXT("On") : TEXT("Off"));
		if (O.GameClass == EOutlawsGameClass::SecretDocument || O.bRandomClass) Row(EItemKind::Select, TEXT("Teams"), TEXT("mphost_teams"), O.bTeams ? TEXT("Red against blue") : TEXT("Every man for himself"));     // (M2)
		Row(EItemKind::Select, Loc(TEXT("$m_local_7105"), TEXT("Allow Cheats")), TEXT("mphost_cheats"), O.bAllowCheats ? TEXT("On") : TEXT("Off"));
		Row(EItemKind::Action, FString::Printf(TEXT("Password:  %s"), O.Password.IsEmpty() ? TEXT("(none)") : *O.Password), TEXT("mphost_password"), FString());     // (M1: optional)
		return;
	}
	// REQ-LOBBYTIDY / REQ-MPLOBBY (user 12 Oct): 14 rows to 8; "Start when" gone (the lobby's countdown starts the match)
	Row(EItemKind::Action, Loc(TEXT("$m_prompt_start"), TEXT("Start")), TEXT("mphost_start"), FString());
	// (REQ-MPLOBBY b, decision 4: "Random each match" for the map and for the game)
	Row(EItemKind::Action, FString::Printf(TEXT("Map:  %s  >"), O.bRandomMap ? TEXT("Random each match") : Map ? *Map->Name : TEXT("(none)")), TEXT("mphost_maps"), FString());
	Row(EItemKind::Select, TEXT("Game"), TEXT("mphost_class"), O.bRandomClass ? FString(TEXT("Random each match")) : OutlawsMpHost::ClassLabel(O.GameClass));
	Row(EItemKind::Select, Loc(TEXT("$m_local_7113"), TEXT("Minutes")), TEXT("mphost_time"), O.TimeLimitMinutes > 0 ? FString::FromInt(O.TimeLimitMinutes) : Loc(TEXT("$m_unlimited"), TEXT("Unlimited")));
	const bool bCaptures = O.GameClass == EOutlawsGameClass::CaptureTheFlag;
	Row(EItemKind::Select, bCaptures ? Loc(TEXT("$m_local_7115"), TEXT("Captures")) : Loc(TEXT("$m_local_7111"), TEXT("Kills")), TEXT("mphost_score"),
		O.ScoreLimit > 0 ? FString::FromInt(O.ScoreLimit) : Loc(TEXT("$m_unlimited"), TEXT("Unlimited")));
	if (OutlawsMpHost::BotsAllowed(O.GameClass) || O.bRandomClass)
	{
		// (REQ-MPBOTS: Death Match and Team Play; Bots and Bot count are one row now: Off, 1..N)
		Row(EItemKind::Select, TEXT("Bots"), TEXT("mphost_bots"), O.bBots ? FString::FromInt(O.BotCount) : FString(TEXT("Off")));
		if (O.bBots) Row(EItemKind::Select, TEXT("Bot skill"), TEXT("mphost_botskill"), OutlawsMpHost::BotSkillLabel(O.BotSkill));
	}
	{
		FItem More;
		More.Kind = EItemKind::Link;
		More.Label = TEXT("More options");
		More.Target = TEXT("#mphost:more");
		Page.Items.Add(More);
	}
	if (UOutlawsCoop::IsHostBetweenMatches(this))
	{
		// (REQ-MPLOBBY b: the next match's page: Start sets it up for everyone, End game sends everyone to the main menu)
		Row(EItemKind::Action, TEXT("End game"), TEXT("mphost_endgame"), FString());
		return;
	}
	if (OutlawsMpHost::MaxPlayersCap() < 16) Row(EItemKind::Heading, FString::Printf(TEXT("Up to %d players on a Quest host"), OutlawsMpHost::MaxPlayersCap()), TEXT(""), FString());
}

void AOutlawsPlayer::MpHostPress(const FString& Name)
{
	EnsureHostMenu();
	FMpHostMenu& M = HostMenu();
	FOutlawsMpHostOptions& O = M.Options;
	if (Name == TEXT("mphost_matchstart"))
	{
		// the pause menu's "Start the game" (the host, in warm-up)
		if (OutlawsMpHost::Flow().HostStart()) CloseMenu();
		else MenuToastText(TEXT("Nobody has joined yet"));
		return;
	}
	if (Name == TEXT("mphost_password"))
	{
		OpenKeyboard(TEXT("mphost_password"), TEXT("GAME PASSWORD"), O.Password, 16);
		return;
	}
	if (Name == TEXT("mphost_maps"))
	{
		ShowMenuPage(TEXT("#mphost:maps"), true);
		return;
	}
	if (Name == TEXT("mphost_start"))
	{
		O.StartWhen = 0;                     // (REQ-MPLOBBY: no "Start when" row: the lobby's countdown starts the match)
		O.Save();
		if (OutlawsMpHost::Host(O))
		{
			CloseMenu();
			return;
		}
		MenuToastText(TEXT("Hosting is not wired up yet: the options are saved"));
		return;
	}
	if (Name == TEXT("mphost_endgame"))
	{
		UOutlawsCoop::HostEndGameFor(this);      // (REQ-MPLOBBY b: between matches, the game ends for everyone)
		return;
	}
	if (Name.StartsWith(TEXT("mphost_map:")))
	{
		// (REQ-MPLOBBY b: "mphost_map:*" is Random each match)
		O.bRandomMap = Name.Mid(11) == TEXT("*");
		if (!O.bRandomMap) O.MapFile = Name.Mid(11);
		O.Clamp(FindMap(O.MapFile));
		O.Save();
		UE_LOG(LogOutlaws, Log, TEXT("OLMPHOST page: map %s (%s)"), O.bRandomMap ? TEXT("random each match") : *O.MapFile, *O.Describe());
		MenuBack();
		return;
	}
	const FOutlawsMpMap* Map = FindMap(O.MapFile);
	if (Name == TEXT("mphost_max"))
	{
		// 2 .. the cap (REQ-TRIGGERSTEP: a number row)
		const int32 Count = FMath::Max(1, OutlawsMpHost::MaxPlayersCap() - 1);
		O.MaxPlayers = 2 + OutlawsMenu::StepIndex(FMath::Clamp(O.MaxPlayers - 2, 0, Count - 1), Count, true);
	}
	else if (Name == TEXT("mphost_class"))
	{
		// the next class the map allows (a random map: any class a stock map plays), then "Random each match" (REQ-MPLOBBY b,
		// decision 4: a class rolled from those the map allows, every match), then round again
		const FMpHostMenu& Menu = HostMenu();
		auto Offered = [&O, Map, &Menu](EOutlawsGameClass C)
		{
			if (!OutlawsMpHost::IsClassOn(C)) return false;       // (M2: a class switched off is not offered)
			if (!O.bRandomMap) return !Map || Map->Allows(C);
			return Menu.Maps.ContainsByPredicate([C](const FOutlawsMpMap& X) { return !X.bCustom && X.Allows(C); });
		};
		// (REQ-TRIGGERSTEP: a named list, round both ways; 0 stands for "Random each match", the last choice)
		TArray<int32> Choices;
		for (int32 c = 1; c <= 6; ++c) if (Offered(EOutlawsGameClass(c))) Choices.Add(c);
		Choices.Add(0);
		int32 Cur = O.bRandomClass ? Choices.Num() - 1 : Choices.IndexOfByKey(int32(O.GameClass));
		if (Cur == INDEX_NONE)
		{
			// (a class the map does not offer: as if on the last offered one below it, so a step up is the next above it)
			for (int32 i = 0; i < Choices.Num() - 1; ++i) if (Choices[i] < int32(O.GameClass)) Cur = i;
		}
		const int32 Pick = Choices[OutlawsMenu::StepIndex(Cur, Choices.Num(), false)];
		O.bRandomClass = Pick == 0;
		if (Pick != 0) O.GameClass = EOutlawsGameClass(Pick);
	}
	else if (Name == TEXT("mphost_time")) O.TimeLimitMinutes = NextOf(OutlawsMpHost::TimeSteps(), O.TimeLimitMinutes);
	else if (Name == TEXT("mphost_score")) O.ScoreLimit = NextOf(OutlawsMpHost::ScoreSteps(), O.ScoreLimit);
	else if (Name == TEXT("mphost_ff")) O.bFriendlyFire = !O.bFriendlyFire;
	else if (Name == TEXT("mphost_teams")) O.bTeams = !O.bTeams;
	else if (Name == TEXT("mphost_cheats")) O.bAllowCheats = !O.bAllowCheats;
	else if (Name == TEXT("mphost_bots"))
	{
		// Off, 1 .. Max Players - 1 (REQ-TRIGGERSTEP: a number row)
		const int32 Count = FMath::Max(1, O.MaxPlayers);
		const int32 N = OutlawsMenu::StepIndex(O.bBots ? FMath::Clamp(O.BotCount, 0, Count - 1) : 0, Count, true);
		O.bBots = N > 0;
		if (N > 0) O.BotCount = N;
	}
	else if (Name == TEXT("mphost_botskill")) O.BotSkill = OutlawsMenu::StepIndex(O.BotSkill, 3, false);
	else return;
	O.Clamp(Map);
	O.Save();
	UE_LOG(LogOutlaws, Log, TEXT("OLMPHOST page: %s -> %s"), *Name, *O.Describe());
	ShowMenuPage(MenuPage ? MenuPage->File : FString(TEXT("#mphost")), false);
}

// ---- -OLMpHostShot ----------------------------------------------------------------------------------------------------------------
// The title menu, New Game > Multiplayer > Competitive > Host Game (photographed: ol_mphost_page); every row stepped (Max Players round its cap and back),
// the settings file read back; Locations (photographed: ol_mphost_maps) and a custom map picked; then the match flow run on
// simulated time (start when N joined, the host's start, a player leaving in the countdown, the kill and time limits, warm-up
// kills not counting, the next round) and once in real time on the message line (photographed: ol_mphost_countdown).
// Each check "OLMPHOST test PASS|FAIL", then a count and the exit.

#if WITH_OUTLAWS_TESTS
void AOutlawsPlayer::MpHostShotTick()
{
	static int32 Pass = 0, Fail = 0;
	static double Mark = 0.0;
	static TArray<FString> Said;
	static int32 Starts = 0, Ends = 0;
	static FOutlawsMatchFlow Live;
	auto Check = [](bool bOk, const FString& What)
	{
		(bOk ? Pass : Fail)++;
		UE_LOG(LogOutlaws, Display, TEXT("OLMPHOST test %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
	};
	auto Shot = [](const TCHAR* Name)
	{
		const FString Path = FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(Name) + TEXT(".png");
		FScreenshotRequest::RequestScreenshot(Path, false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLMPHOST screenshot %s"), *Path);
	};
	auto Index = [this](const FString& Name) { return MenuPage ? MenuPage->Items.IndexOfByPredicate([&Name](const OutlawsMenu::FItem& It) { return It.Name == Name || It.Target == Name; }) : INDEX_NONE; };
	auto Press = [this, &Index](const FString& Name)
	{
		const int32 i = Index(Name);
		if (i != INDEX_NONE) MenuActivate(1, i);
		return i != INDEX_NONE;
	};
	auto Rows = [this]()
	{
		TArray<FString> Out;
		if (MenuPage) for (int32 i = 0; i < MenuPage->Items.Num(); ++i) Out.Add(MenuRowText(i));
		return FString::Join(Out, TEXT(" | "));
	};
	const double Now = FPlatformTime::Seconds();
	auto Waited = [&](double S) { return Now - Mark > S; };
	auto Next = [&]() { Mark = Now; ++ShotStep; };
	// (REQ-MPLOBBY c: the page as drawn fits the panel, at the top and scrolled to the bottom)
	auto Fits = [this, &Check](const TCHAR* What)
	{
		const int32 Max = SheetMaxScroll();
		for (int32 Sc : { 0, Max })
		{
			MenuScroll = Sc;
			RefreshMenuRows();
			FString Measured;
			const bool bFits = MenuPageFits(Measured);
			Check(bFits, FString::Printf(TEXT("%s fits the panel (%s)"), What, *Measured));
			if (Max == 0) break;
		}
		MenuScroll = 0;
		RefreshMenuRows();
	};
	if (ShotStep == 0 && ShotTime > 4.f && bMenuOpen && MenuPage)
	{
		{
			// (BUG-MPRESUME, REQ-TITLEMENU) single player's title menu: Load Game pressable when there is a save, no Resume Game
			const OutlawsMenu::FItem* Ld = MenuPage->Items.FindByPredicate([](const OutlawsMenu::FItem& It) { return It.Name == TEXT("title_load"); });
			const bool bLoad = Ld && !Ld->Classes.Contains(TEXT("disabled"));
			const bool bResume = MenuPage->Items.ContainsByPredicate([](const OutlawsMenu::FItem& It) { return It.Action == TEXT("ev_resume_last_save"); });
			Check(MenuPage->File == TEXT("#front") && Ld && !bResume && bLoad == AnySaveToLoad() && TitleLoadOffered(this) == AnySaveToLoad(),
				FString::Printf(TEXT("single player's title menu (%s): Load Game %s, no Resume Game %d (a save %s)"), *MenuPage->File, !Ld ? TEXT("missing") : bLoad ? TEXT("pressable") : TEXT("greyed out"),
				bResume ? 0 : 1, AnySaveToLoad() ? TEXT("there") : TEXT("absent")));
		}
		// (REQ-MPMENU) New Game > Multiplayer > Competitive > Host Game
		ShowMenuPage(TEXT("#newgame"), true);
		const bool bMulti = Press(TEXT("#coopmulti"));
		const bool bComp = bMulti && Press(TEXT("#coopcomp"));
		const bool bLink = bComp && Press(TEXT("#mphost"));
		Check(bLink && MenuPage && MenuPage->File == TEXT("#mphost"), FString::Printf(TEXT("New Game > Multiplayer > Competitive has Host Game, and it opens the page (%s; multi %d comp %d)"),
			MenuPage ? *MenuPage->File : TEXT("?"), bMulti ? 1 : 0, bComp ? 1 : 0));
		UE_LOG(LogOutlaws, Display, TEXT("OLMPHOST page rows: %s"), *Rows());
		bool bAll = true;
		for (const TCHAR* R : { TEXT("mphost_start"), TEXT("mphost_maps"), TEXT("mphost_class"), TEXT("mphost_time"), TEXT("mphost_score"), TEXT("#mphost:more") })
		{
			bAll &= Index(R) != INDEX_NONE;
		}
		Check(bAll && Index(TEXT("mphost_startwhen")) == INDEX_NONE && Index(TEXT("mphost_max")) == INDEX_NONE && MenuPage->Title == TEXT("HOST GAME"),
			FString::Printf(TEXT("the page has the approved rows (Start, Map, Game, Minutes, Kills, Bots, More options; no Start when), titled %s"), MenuPage ? *MenuPage->Title : TEXT("?")));
		Check(MenuPage->Items.Num() <= 9, FString::Printf(TEXT("the page has %d rows (8 at most, and the Quest note)"), MenuPage->Items.Num()));
		Next();
	}
	else if (ShotStep == 1 && Waited(1.5)) { Fits(TEXT("the Host Game page")); Shot(TEXT("ol_mphost_page")); Next(); }
	else if (ShotStep == 2 && Waited(1.0))
	{
		FOutlawsMpHostOptions& O = HostMenu().Options;
		// More options: Max Players up to the cap, then round to 2 (REQ-LOBBYTIDY: on its own page now)
		const bool bMore = Press(TEXT("#mphost:more"));
		Check(bMore && MenuPage && MenuPage->File == TEXT("#mphost:more") && Index(TEXT("mphost_max")) != INDEX_NONE && Index(TEXT("mphost_ff")) != INDEX_NONE
			&& Index(TEXT("mphost_cheats")) != INDEX_NONE && Index(TEXT("mphost_password")) != INDEX_NONE, FString::Printf(TEXT("More options opens its page: %s"), *Rows()));
		O.MaxPlayers = 2;
		TArray<FString> Seen;
		for (int32 k = 0; k < 20; ++k)
		{
			Seen.Add(FString::FromInt(O.MaxPlayers));
			Press(TEXT("mphost_max"));
		}
		const int32 Cap = OutlawsMpHost::MaxPlayersCap();
		Check(Seen.Contains(FString::FromInt(Cap)) && !Seen.Contains(FString::FromInt(Cap + 1)) && Seen.Contains(TEXT("2")),
			FString::Printf(TEXT("Max Players steps 2..%d and round (%s)"), Cap, *FString::Join(Seen, TEXT(","))));
		while (O.MaxPlayers != FMath::Min(4, Cap)) Press(TEXT("mphost_max"));
		Press(TEXT("mphost_ff"));
		Next();
	}
	else if (ShotStep == 3 && Waited(1.5)) { Fits(TEXT("More options")); Shot(TEXT("ol_mphost_more")); Next(); }
	else if (ShotStep == 4 && Waited(1.0))
	{
		FOutlawsMpHostOptions& O = HostMenu().Options;
		const int32 Cap = OutlawsMpHost::MaxPlayersCap();
		MenuBack();
		const EOutlawsGameClass C0 = O.GameClass;
		const FOutlawsMpMap* Map = FindMap(O.MapFile);
		// (REQ-MPLOBBY b) Game steps through the classes the map allows, then "Random each match", then round
		TArray<FString> Cycle;
		bool bAllowed = true;
		for (int32 k = 0; k < 7; ++k)
		{
			Press(TEXT("mphost_class"));
			Cycle.Add(O.bRandomClass ? FString(TEXT("Random")) : OutlawsMpHost::ClassLabel(O.GameClass));
			bAllowed &= O.bRandomClass || (Map && Map->Allows(O.GameClass));
		}
		const int32 Randoms = Cycle.FilterByPredicate([](const FString& X) { return X == TEXT("Random"); }).Num();
		Check(bAllowed && Randoms >= 1 && Randoms <= 2, FString::Printf(TEXT("Games steps to classes the map allows and Random each match (%s on %s [%s], from %s)"), *FString::Join(Cycle, TEXT(",")),
			Map ? *Map->Name : TEXT("?"), Map ? *Map->ModesText() : TEXT(""), *OutlawsMpHost::ClassLabel(C0)));
		while (O.bRandomClass) Press(TEXT("mphost_class"));
		Check(MenuRowText(Index(TEXT("mphost_class"))).Contains(OutlawsMpHost::ClassLabel(O.GameClass)), FString::Printf(TEXT("the Game row shows the class again (%s)"), *MenuRowText(Index(TEXT("mphost_class")))));
		while (O.GameClass != EOutlawsGameClass::DeathMatch && Map && Map->Allows(EOutlawsGameClass::DeathMatch)) Press(TEXT("mphost_class"));
		Press(TEXT("mphost_time"));
		Press(TEXT("mphost_score"));
		// Bots: Off, 1 .. Max Players - 1, Off (one row now)
		if (Index(TEXT("mphost_bots")) != INDEX_NONE)
		{
			const bool bWas = O.bBots;
			const int32 WasCount = O.BotCount;
			O.bBots = false;
			TArray<FString> Seen;
			for (int32 k = 0; k < O.MaxPlayers + 1; ++k)
			{
				Seen.Add(O.bBots ? FString::FromInt(O.BotCount) : FString(TEXT("Off")));
				if (k < O.MaxPlayers) Press(TEXT("mphost_bots"));          // (the last one seen is where it is left: Off again)
			}
			Check(Seen.Num() == O.MaxPlayers + 1 && Seen[0] == TEXT("Off") && Seen[1] == TEXT("1") && Seen[O.MaxPlayers - 1] == FString::FromInt(O.MaxPlayers - 1) && Seen.Last() == TEXT("Off") && !O.bBots,
				FString::Printf(TEXT("Bots steps Off, 1 .. %d, Off (%s)"), O.MaxPlayers - 1, *FString::Join(Seen, TEXT(","))));
			while (O.bBots != bWas || (bWas && O.BotCount != WasCount)) Press(TEXT("mphost_bots"));
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLMPHOST page rows after: %s"), *Rows());
		const TMap<FString, FString> S = AOutlawsLevelActor::ReadSettings();
		FOutlawsMpHostOptions Back;
		Back.Load();
		Check(Back.Describe() == O.Describe(), FString::Printf(TEXT("the options saved and read back alike (%s)"), *Back.Describe()));
		Check(O.MaxPlayers == FMath::Min(4, Cap) && O.bFriendlyFire, TEXT("max players 4, friendly fire on (More options)"));
		Press(TEXT("mphost_maps"));
		const int32 NumMaps = HostMenu().Maps.Num();
		int32 NumCustom = 0;
		for (const FOutlawsMpMap& Mp : HostMenu().Maps) NumCustom += Mp.bCustom ? 1 : 0;
		UE_LOG(LogOutlaws, Display, TEXT("OLMPHOST maps page (%d rows): %s"), MenuPage ? MenuPage->Items.Num() : 0, *Rows().Left(1500));
		const bool bNamed = !HostMenu().Maps.ContainsByPredicate([](const FOutlawsMpMap& Mp) { return Mp.Name.StartsWith(TEXT("^")); });
		Check(bNamed, TEXT("every stock map shows its name from the remaster's strings (no ^level_..._rcm keys)"));
		Check(MenuPage && MenuPage->File == TEXT("#mphost:maps") && NumMaps - NumCustom >= 6 && NumCustom > 0 && MenuPage->Items.Num() == NumMaps + 1,
			FString::Printf(TEXT("Locations lists %d stock and %d custom maps"), NumMaps - NumCustom, NumCustom));
		Check(MenuPage && MenuPage->Items.Num() > 0 && MenuPage->Items[0].Name == TEXT("mphost_map:*") && MenuPage->Items[0].Label.Contains(TEXT("Random each match")),
			TEXT("Locations starts with Random each match (REQ-MPLOBBY b)"));
		Next();
	}
	else if (ShotStep == 5 && Waited(1.5)) { Shot(TEXT("ol_mphost_maps")); Fits(TEXT("Locations")); Next(); }
	else if (ShotStep == 6 && Waited(1.0))
	{
		const FOutlawsMpMap* Custom = HostMenu().Maps.FindByPredicate([](const FOutlawsMpMap& Mp) { return Mp.bCustom && Mp.Allows(EOutlawsGameClass::DeathMatch); });
		const bool bPicked = Custom && Press(TEXT("mphost_map:") + Custom->File);
		Check(bPicked && MenuPage && MenuPage->File == TEXT("#mphost") && HostMenu().Options.MapFile == Custom->File,
			FString::Printf(TEXT("a custom map picked (%s, %s, its LAB %s): back on the page"), Custom ? *Custom->Name : TEXT("?"), Custom ? *Custom->Level : TEXT("?"), Custom ? *Custom->Lab : TEXT("?")));
		UE_LOG(LogOutlaws, Display, TEXT("OLMPHOST page rows with it: %s"), *Rows());
		// ---- the match flow on simulated time ----
		auto Run = [](FOutlawsMatchFlow& F, float Seconds) { for (float t = 0.f; t < Seconds; t += 0.1f) F.Tick(0.1f); };
		auto Has = [](const TCHAR* Text) { return Said.ContainsByPredicate([Text](const FString& S) { return S.Contains(Text); }); };
		FOutlawsMatchFlow F;
		F.OnMessage = [](const FString& T, float) { Said.Add(T); };
		F.OnMatchStart = []() { ++Starts; };
		F.OnMatchEnd = [](const FString&) { ++Ends; };
		FOutlawsMpHostOptions O;
		O.MaxPlayers = 4;
		O.StartWhen = 3;
		O.ScoreLimit = 10;
		O.TimeLimitMinutes = 5;
		Said.Reset();
		F.Begin(O);
		Run(F, 6.f);
		Check(F.GetState() == FOutlawsMatchFlow::EState::WarmUp && Has(TEXT("Waiting for players (1/4)")), TEXT("hosting: warm-up, \"Waiting for players (1/4)\""));
		F.SetPlayers(2);
		Run(F, 1.f);
		Check(F.GetState() == FOutlawsMatchFlow::EState::WarmUp && Has(TEXT("Waiting for players (2/4)")), TEXT("one joins: still warm-up, (2/4)"));
		F.ReportScore(50, TEXT("A"));
		Check(F.GetState() == FOutlawsMatchFlow::EState::WarmUp && !F.CountsKills(), TEXT("warm-up kills do not count (a score past the limit ends nothing)"));
		F.SetPlayers(3);
		Run(F, 0.2f);
		Check(F.GetState() == FOutlawsMatchFlow::EState::Countdown && Has(TEXT("Starting in 10")), TEXT("the third joins: the countdown starts by itself, \"Starting in 10\""));
		F.SetPlayers(2);
		Check(F.GetState() == FOutlawsMatchFlow::EState::WarmUp && Has(TEXT("countdown stopped")), TEXT("one leaves in the countdown: back to warm-up"));
		F.SetPlayers(3);
		Said.Reset();
		Run(F, 10.5f);
		bool bAllSecs = true;
		for (int32 s = 10; s >= 1; --s) bAllSecs &= Has(*FString::Printf(TEXT("Starting in %d"), s));
		Check(bAllSecs && F.GetState() == FOutlawsMatchFlow::EState::Playing && Starts == 1 && Has(TEXT("Fight!")), FString::Printf(TEXT("10 to 1, then the match (%s)"), *FString::Join(Said, TEXT(" / "))));
		F.ReportScore(9, TEXT("B"));
		Check(F.GetState() == FOutlawsMatchFlow::EState::Playing, TEXT("9 kills of 10: the match goes on"));
		F.ReportScore(10, TEXT("B"));
		Check(F.GetState() == FOutlawsMatchFlow::EState::Ended && Ends == 1 && Has(TEXT("B wins!")), TEXT("10 kills: the match ends, \"B wins!\""));
		Run(F, FOutlawsMatchFlow::EndedSeconds + 0.2f);
		Check(F.GetState() == FOutlawsMatchFlow::EState::Countdown || F.GetState() == FOutlawsMatchFlow::EState::WarmUp,
			FString::Printf(TEXT("after the scoreboard, the next round (%s: 3 still here, so its countdown follows)"), FOutlawsMatchFlow::StateName(F.GetState())));
		// the host's own start, and the time limit
		FOutlawsMatchFlow G;
		G.OnMessage = [](const FString& T, float) { Said.Add(T); };
		G.OnMatchStart = []() { ++Starts; };
		G.OnMatchEnd = [](const FString&) { ++Ends; };
		O.StartWhen = 0;
		O.ScoreLimit = 0;
		Said.Reset();
		G.Begin(O);
		Check(!G.HostStart() && Has(TEXT("Nobody has joined yet")), TEXT("Start the game alone: refused"));
		G.SetPlayers(2);
		Run(G, 30.f);
		Check(G.GetState() == FOutlawsMatchFlow::EState::WarmUp, TEXT("start by the host: warm-up waits, however long"));
		Check(G.HostStart() && G.GetState() == FOutlawsMatchFlow::EState::Countdown, TEXT("Start the game with two: the countdown"));
		Run(G, 10.5f);
		Run(G, 5 * 60.f - 61.f);
		Check(G.GetState() == FOutlawsMatchFlow::EState::Playing && !Has(TEXT("One minute left")), FString::Printf(TEXT("4 minutes in: playing (%.0f s left)"), G.TimeLeft()));
		Run(G, 2.f);
		Check(Has(TEXT("One minute left")), TEXT("a minute to go: \"One minute left\""));
		Run(G, 60.f);
		Check(G.GetState() == FOutlawsMatchFlow::EState::Ended && Has(TEXT("Time is up!")) && Ends == 2, TEXT("5 minutes: \"Time is up!\", the match ends"));
		Run(G, FOutlawsMatchFlow::EndedSeconds + 0.2f);
		Check(G.GetState() == FOutlawsMatchFlow::EState::WarmUp, TEXT("then warm-up again (the host starts the next)"));
		{
			// (REQ-MPLOBBY b) the session's next-match cycle: the end holds (the score screen) until the host's next match
			FOutlawsMatchFlow H;
			H.bHoldEnd = true;
			H.Begin(O);
			H.SetPlayers(2);
			H.HostStart();
			Run(H, 10.5f);
			Run(H, 5 * 60.f + 1.f);
			const bool bEnded = H.GetState() == FOutlawsMatchFlow::EState::Ended;
			Run(H, FOutlawsMatchFlow::EndedSeconds * 4.f);
			Check(bEnded && H.GetState() == FOutlawsMatchFlow::EState::Ended, FString::Printf(TEXT("with the next-match cycle the end holds (%s after %.0f s)"),
				FOutlawsMatchFlow::StateName(H.GetState()), FOutlawsMatchFlow::EndedSeconds * 4.f));
		}
		// ---- the flow in real time, on this player's message line ----
		CloseMenu();
		Live.OnMessage = [this](const FString& T, float Sec) { ShowMessage(T, Sec); };
		Live.OnMatchStart = []() { ++Starts; };
		O.StartWhen = 2;
		Live.Begin(O);
		Live.SetPlayers(2);
		Next();
	}
	else if (ShotStep == 7)
	{
		static double Last = Now;
		Live.Tick(float(FMath::Min(Now - Last, 0.1)));
		Last = Now;
		if (Live.GetState() == FOutlawsMatchFlow::EState::Countdown && Live.TimeLeft() != 0.f && Waited(3.5))
		{
			Shot(TEXT("ol_mphost_countdown"));
			Next();
		}
	}
	else if (ShotStep == 8)
	{
		static double Last = Now;
		Live.Tick(float(FMath::Min(Now - Last, 0.1)));
		Last = Now;
		if (Live.GetState() == FOutlawsMatchFlow::EState::Playing)
		{
			Check(Starts == 3, TEXT("the real-time countdown ran to the match"));
			UE_LOG(LogOutlaws, Display, TEXT("OLMPHOST test done: %d passed, %d failed"), Pass, Fail);
			FGenericPlatformMisc::RequestExit(false);
			ShotTime = -1.f;
			ShotStep = 99;
		}
		else if (Waited(30.0))
		{
			Check(false, TEXT("the real-time countdown never reached the match"));
			UE_LOG(LogOutlaws, Display, TEXT("OLMPHOST test done: %d passed, %d failed"), Pass, Fail);
			FGenericPlatformMisc::RequestExit(false);
			ShotStep = 99;
		}
	}
}

OL_SHOT_MATCH(OLMpHostShot) { return OLFlags::Param(TEXT("OLMpHostShot")); }
OL_SHOT_TEST(OLMpHostShot, 800)
{
	MpHostShotTick();                // (M1 host: the Host Game page, its maps, the match flow: OutlawsPlayerMpHost.cpp)
}
#endif // WITH_OUTLAWS_TESTS
