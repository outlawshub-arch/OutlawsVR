// REQ-MPLOBBY, landing (b) (the user 12 Oct, decisions 3 to 5): a match's end and the next match.  The end fades every machine
// to black, then the score screen (#coopscore) with Continue at once.  A joiner's Continue takes it to the lobby for the next
// game, Enter Game (Join Red / Join Blue) greyed out with "Waiting for the host to start the next match" until the host starts
// it; Character, the players, mute, Options and Quit work meanwhile.  The host's Continue is the Host Game page with the last
// match's settings, Start or End game.  End game (or 3 minutes with no next match) sends the joiners to the main menu with
// "The host ended the game".  A launched host (-OLMpHost, -OLMpRotate) sets the next match up itself 10 s after the end.
// Every next match runs the lobby's start rule again; "Random each match" rolls its map and game again (OutlawsMpHost::RollMatch).
#include "OutlawsCoop.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsMpHost.h"
#include "OutlawsFragTable.h"
#include "OutlawsLab.h"
#include "Camera/CameraComponent.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

namespace
{
	constexpr double FadeSeconds = 1.0;              // the fade to black at the match's end, then the score screen
	constexpr double LaunchedNextSeconds = 10.0;     // a launched host's own next match, this long after the end
}

void AOutlawsCoopLink::ClientHostEnded_Implementation()
{
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->LeaveForHostEnd(TEXT("the host ended the game"));
}

bool UOutlawsCoop::IsHostBetweenMatches(const UObject* Context)
{
	const UOutlawsCoop* C = Get(Context);
	const AOutlawsCoopSession* S = C ? C->GetSession() : nullptr;
	return C && C->IsHost() && C->IsMatch() && S && S->bBetweenMatches;
}

void UOutlawsCoop::HostEndGameFor(const UObject* Context)
{
	if (UOutlawsCoop* C = Get(Context)) C->HostEndGame(TEXT("End game on the Host Game page"));
}

FString UOutlawsCoop::MatchLineFor(uint8 Class, const FString& MapFile, uint8 RandomBits)
{
	FString Line = FString::Printf(TEXT("%s on %s"), *OutlawsMpHost::ClassLabel(EOutlawsGameClass(FMath::Clamp<int32>(Class, 1, 6))), *OutlawsMpHost::MapTitle(MapFile));
	if ((RandomBits & 3) == 3) Line += TEXT("  (random map and game)");
	else if (RandomBits & 1) Line += TEXT("  (random map)");
	else if (RandomBits & 2) Line += TEXT("  (random game)");
	return Line;
}

FString UOutlawsCoop::MatchLine() const
{
	const AOutlawsCoopSession* S = GetSession();
	return S ? MatchLineFor(S->MatchClass, S->MapName, S->RandomBits) : FString();
}

// ---- the host ------------------------------------------------------------------------------------------------------------------

void UOutlawsCoop::TickNextMatchHost(double Now, AOutlawsCoopSession* S)
{
	if (!S || !IsHost() || !NextMatchCycleOn()) return;
	FOutlawsMatchFlow& F = OutlawsMpHost::Flow();
	if (F.GetState() == FOutlawsMatchFlow::EState::Ended && !S->bBetweenMatches && EndedCycle != S->MatchCycle)
	{
		EndedCycle = S->MatchCycle;
		BetweenSince = Now;
		S->bBetweenMatches = true;
		S->ForceNetUpdate();
		++Version;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH next: match %d is over (%s): the score screen everywhere, the joiners' lobby waits for the host's next match (%s)"),
			S->MatchCycle + 1, *MatchLine(), HostSetsNextItself() ? TEXT("this launched host sets it up itself in 10 s") : TEXT("the Host Game page"));
	}
	if (S->bBetweenMatches && HostSetsNextItself() && HostEndAt == 0.0 && Now - BetweenSince >= LaunchedNextSeconds)
	{
		FOutlawsMpHostOptions O = MatchOptions;
		if (bLaunchRotate) O.bRandomMap = true;      // (-OLMpRotate: another stock map each match)
		if (!StartNextMatch(O, bLaunchRotate ? TEXT("the launched host's rotation") : TEXT("the launched host"))) BetweenSince = Now;       // (refused: tried again in 10 s)
	}
}

bool UOutlawsCoop::StartNextMatch(const FOutlawsMpHostOptions& Options, const TCHAR* Who)
{
	AOutlawsCoopSession* S = GetSession();
	AOutlawsLevelActor* L = FindLevel();
	if (!IsHost() || !S || !L || !IsMatch() || HostEndAt > 0.0) return false;
	TArray<FOutlawsMpMap> Maps;
	OutlawsMpHost::ListMaps(L->AssetsRef.Get(), Maps);
	FOutlawsMpHostOptions O = Options;
	const FString WasMap = MatchOptions.MapFile;
	if (O.bRandomMap || O.bRandomClass)
	{
		FString Said;
		if (OutlawsMpHost::RollMatch(Maps, O, MatchOptions.MapFile, MatchOptions.GameClass, Said))
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH random each match: the next match is %s"), *Said);
		}
		else
		{
			UE_LOG(LogOutlawsCoop, Warning, TEXT("OLMATCH random each match: nothing to roll (%s): the chosen map and game"), *Said);
		}
	}
	const FOutlawsMpMap* Map = Maps.FindByPredicate([&O](const FOutlawsMpMap& M) { return M.File.Equals(O.MapFile, ESearchCase::IgnoreCase); });
	if (!Map) Map = Maps.FindByPredicate([&O](const FOutlawsMpMap& M) { return M.Level.Equals(O.MapFile, ESearchCase::IgnoreCase); });
	if (!Map)
	{
		UE_LOG(LogOutlawsCoop, Warning, TEXT("OLMATCH next: no map %s among %d: the next match is not set up"), *O.MapFile, Maps.Num());
		return false;
	}
	if (!Map->Allows(O.GameClass) || !OutlawsMpHost::IsClassOn(O.GameClass))
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH next: %s does not play %s here: Death Match"), *Map->Name, *OutlawsMpHost::ClassLabel(O.GameClass));
		O.GameClass = EOutlawsGameClass::DeathMatch;
	}
	if (!LaunchMatchMap.IsEmpty()) O.StartWhen = MatchOptions.StartWhen;     // (a launched host's -OLMpStartWhen stays the start rule's minimum)
	O.MapFile = Map->File;
	O.Clamp(Map);
	if (!O.Password.IsEmpty()) HostPassword = CleanPassword(O.Password);
	MatchOptions = O;
	LevelToStart = Map->Level.ToLower();
	S->MatchClass = uint8(O.GameClass);
	S->bCheatsOff = !O.bAllowCheats;                  // (BUG-ALLOWCHEATS: each match has the host's row of that time)
	S->FragLimit = O.ScoreLimit;
	S->TimeLimitMinutes = O.TimeLimitMinutes;
	S->MapName = Map->File;
	S->RandomBits = (O.bRandomMap ? 1 : 0) | (O.bRandomClass ? 2 : 0);
	S->Scores.Reset();
	// everyone back in the lobby (bots never are): each enters the next match from there
	S->bHostReady = false;
	bEnterPressed = false;
	TArray<AOutlawsCoopLink*> Links;
	GetLinks(Links);
	for (AOutlawsCoopLink* K : Links)
	{
		if (K->bBot || K->PlayerIndex <= 0) continue;
		K->bLobbyReady = false;
		K->ForceNetUpdate();
	}
	FMemory::Memzero(LobbyTeam);
	S->bBetweenMatches = false;
	++S->MatchCycle;
	S->ForceNetUpdate();
	RefreshLobby();
	OutlawsMpHost::Flow().Stop();                // (the new level's SetUpMatchLevel begins it again with these options)
	bMatchSetUp = false;
	HideMatchPanel(TEXT("the next match"));
	const FString Title = OutlawsMpHost::MapTitle(Map->File);
	if (bLaunchRotate && !LaunchMatchMap.IsEmpty())
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH rotate: %s -> %s (%s, level %s)"), *WasMap, *Title, *Map->File, *LevelToStart);
	}
	SayToAll(FString::Printf(TEXT("Next match: %s"), *MatchLine()), 5.f);
	RequestStart(EOutlawsCoopStart::Lobby, LevelToStart);
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH next: match %d set up by %s: %s (level %s, serial %d; %s); everyone back in the lobby, the start rule runs again"), S->MatchCycle + 1, Who,
		*MatchLine(), *LevelToStart, S->StartSerial, *MatchOptions.Describe());
	++Version;
	return true;
}

void UOutlawsCoop::HostEndGame(const TCHAR* Why)
{
	if (!IsHost() || HostEndAt > 0.0) return;
	int32 Told = 0;
	ForClients([&Told](AOutlawsCoopLink& CL) { if (!CL.bBot) { CL.ClientHostEnded(); ++Told; } });
	HostEndAt = FPlatformTime::Seconds() + 1.5;      // (the joiners hear it before this host's session closes)
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH the host ended the game (%s): %d joiners told; this host's session ends in 1.5 s"), Why, Told);
	if (AOutlawsPlayer* P = LocalPlayer()) P->CoopToast(TEXT("Ending the game ..."));
}

// ---- every machine ---------------------------------------------------------------------------------------------------------

void UOutlawsCoop::LeaveForHostEnd(const TCHAR* Why)
{
	if (!IsClient()) return;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH next: %s: this joiner goes to its main menu"), Why);
	EndSession(Why, true);
	PendingToast = TEXT("The host ended the game");
}

void UOutlawsCoop::GoToNextLobby(AOutlawsPlayer* P, const TCHAR* Why)
{
	const AOutlawsCoopSession* S = GetSession();
	bEnterPressed = false;
	bNextLobbyArmed = true;
	ScorePhase = 0;
	if (AOutlawsCoopLink* Mine = GetLink(MyIndex()); Mine && Mine->bLobbyReady) SetReady(false);       // (out of the level: its body drawn nowhere)
	if (P) P->CoopOpenPage(TEXT("#cooplobby"));
	++Version;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby: the %s is in the lobby for the next match (%s; %s)"), WhoWord(MyIndex()), Why,
		S && S->bBetweenMatches ? TEXT("Enter Game greyed out: waiting for the host to start the next match") : TEXT("the next match is set up: Enter Game"));
}

void UOutlawsCoop::ScoreContinue(AOutlawsPlayer* P)
{
	if (!P || !IsMatch()) return;
	ScorePhase = 0;
	if (IsHost())
	{
		if (HostSetsNextItself())
		{
			P->CoopToast(TEXT("The next match starts by itself"));
			return;
		}
		// the Host Game page with the last match's settings ("Random each match" stays Random): Start or End game
		AOutlawsPlayer::MpHostPrefill(MatchOptions);
		P->CoopShowPage(TEXT("#mphost"), true);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH next: the host's Continue: the Host Game page with the last match's settings (%s)"), *MatchOptions.Describe());
		return;
	}
	GoToNextLobby(P, TEXT("its Continue on the score screen"));
}

void UOutlawsCoop::BuildScorePage(AOutlawsPlayer* P, OutlawsMenu::FPage& Page)
{
	using namespace OutlawsMenu;
	Page.Title = TEXT("MATCH OVER");
	FItem I;
	I.Kind = EItemKind::Action;
	I.Label = TEXT("Continue");
	I.Name = TEXT("coop:continue");
	Page.Items.Add(I);
}

void UOutlawsCoop::ScoreLines(TArray<FString>& Out) const
{
	const AOutlawsCoopSession* S = GetSession();
	if (!S) return;
	Out.Add(MatchLine());
	FFragTableData T;
	const bool bTable = FillMatchTable(T);
	if (S->bTeams) Out.Add(FString::Printf(TEXT("Red %d  -  Blue %d%s"), S->RedScore, S->BlueScore, S->RedScore > S->BlueScore ? TEXT(":  red wins") : S->BlueScore > S->RedScore ? TEXT(":  blue wins") : TEXT(":  a draw")));
	else if (bTable && T.Rows.Num() > 0) Out.Add(T.Rows.Num() > 1 && T.Rows[0].SortA == T.Rows[1].SortA && T.Rows[0].SortB == T.Rows[1].SortB ? FString(TEXT("A draw"))
		: FString::Printf(TEXT("%s wins"), *T.Rows[0].Name));
	// (REQ-SCORESIDE, user 14 Oct: the match's end uses the pause menu's frag table, the panel to the left of this page with every
	// player and column, instead of a list of its own here; only the result and what Continue does are on the page)
	Out.Add(IsHost() ? (HostSetsNextItself() ? FString(TEXT("The next match is set up by itself shortly")) : FString(TEXT("Continue: set up the next match")))
		: FString(TEXT("Continue: the lobby for the next match")));
}

void UOutlawsCoop::TickNextMatch(double Now, AOutlawsCoopSession* S, AOutlawsPlayer* P)
{
	if (!S || !IsMatch()) return;
	if (IsHost() && HostEndAt > 0.0 && Now >= HostEndAt)
	{
		EndSession(TEXT("the host ended the game"), true);
		return;
	}
	if (!NextMatchCycleOn()) return;
	AOutlawsLevelActor* L = FindLevel();
	// the match's end seen here: the fade to black, then the score screen (a joiner never in a level waits in its lobby)
	if (S->bBetweenMatches && SeenCycle != S->MatchCycle)
	{
		SeenCycle = S->MatchCycle;
		const bool bInLevel = IsHost() || StartedSerial > 0;
		if (IsClient())
		{
			bEnterPressed = false;
			bNextLobbyArmed = true;
			WaitSince = Now;
		}
		if (bInLevel)
		{
			ScorePhase = 1;
			ScoreAt = Now + FadeSeconds + 0.2;
			if (P && P->IsMenuOpen()) P->CoopCloseMenu();
			if (L) L->FadeCamera(0.f, 1.f, float(FadeSeconds), true);
			HideMatchPanel(TEXT("the score screen"));
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH next: the match is over here (%s, %s): %s"), IsHost() ? TEXT("host") : WhoWord(MyIndex()), *MatchLine(),
			bInLevel ? TEXT("fade to black, then the score screen") : TEXT("never in the level: its lobby waits for the next match"));
		++Version;
	}
	if (ScorePhase == 1 && Now >= ScoreAt)
	{
		ScorePhase = 2;
		if (L && P && (L->bDead || P->IsDeadView()))
		{
			// (a player dead at the end: on its feet where it lies, behind the black, so the score screen can come up)
			const AOutlawsCoopLink* Mine = GetLink(MyIndex());
			const FString Char = IsHost() ? CoopCharChoice() : Mine && !Mine->Character.IsEmpty() ? Mine->Character : FString(TEXT("Jam"));
			OnMatchRespawnHere(LevelEpoch(), P->GetActorLocation(), float(P->GetActorRotation().Yaw), Char);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH next: the %s was dead at the end: back on its feet for the score screen"), IsHost() ? TEXT("host") : WhoWord(MyIndex()));
		}
		if (L) L->FadeCamera(1.f, 0.f, 0.5f, false);
		if (P) P->CoopOpenPage(TEXT("#coopscore"));
		TArray<FString> Lines;
		ScoreLines(Lines);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH next: the score screen is up (%s): %s"), IsHost() ? TEXT("host") : WhoWord(MyIndex()), *FString::Join(Lines, TEXT(" | ")));
	}
	if (ScorePhase == 2 && P && P->CoopMenuPage() != TEXT("#coopscore")) ScorePhase = 0;      // (Continue, or the next match's build)
	// the joiner: 3 minutes (decision 3) with no next match from the host, and it goes to its main menu
	if (IsClient())
	{
		if (!S->bBetweenMatches) WaitSince = 0.0;
		else if (WaitSince == 0.0) WaitSince = Now;
		else if (Now - WaitSince >= NextTimeout)
		{
			LeaveForHostEnd(*FString::Printf(TEXT("no next match from the host in %.0f s"), Now - WaitSince));
			return;
		}
	}
	// the lobby again whenever its menu is shut (the next match's build closes it): until this player enters
	if (P && !P->IsMenuOpen() && ScorePhase != 1 && Now >= NextMenuTryAt)
	{
		if (IsClient() && bNextLobbyArmed && !bEnterPressed && (S->bBetweenMatches || (bLevelBuilt && StartedSerial == S->StartSerial && Now - LevelBuiltAt > 1.0)))
		{
			NextMenuTryAt = Now + 1.0;
			GoToNextLobby(P, S->bBetweenMatches ? TEXT("its menu came back") : TEXT("the next match's level is built behind it"));
		}
		else if (IsHost() && S->bBetweenMatches && !HostSetsNextItself() && HostEndAt == 0.0)
		{
			NextMenuTryAt = Now + 1.0;
			P->CoopOpenPage(TEXT("#coopscore"));       // (the host sets up the next match, or ends the game, from here)
		}
	}
#if WITH_OUTLAWS_TESTS
	TickNextTest(Now, S, P);
#endif
}

// (REQ-SCORESIDE) the score screen's frag table: up, the live table's panel, to the left of the page as the eyes see it
bool UOutlawsCoop::ScoreSideOk(const AOutlawsPlayer* P) const
{
	const AOutlawsFragTablePanel* Panel = PausePanel.Get();
	if (!P || !Panel || !P->MenuRoot || !P->Camera || Panel->IsHidden() || Panel->GetData().Rows.Num() < 2) return false;
	const FVector Eye = P->Camera->GetComponentLocation();
	const FVector A = Panel->GetActorLocation() - Eye, B = P->MenuRoot->GetComponentLocation() - Eye;
	const double Apart = FRotator::NormalizeAxis(FMath::RadiansToDegrees(FMath::Atan2(A.Y, A.X) - FMath::Atan2(B.Y, B.X)));
	return Apart < -35.0 && Apart > -75.0;
}

FString UOutlawsCoop::ScoreSideSays(const AOutlawsPlayer* P) const
{
	const AOutlawsFragTablePanel* Panel = PausePanel.Get();
	if (!Panel) return TEXT("no table");
	FString Where;
	if (P && P->MenuRoot && P->Camera)
	{
		const FVector Eye = P->Camera->GetComponentLocation();
		const FVector A = Panel->GetActorLocation() - Eye, B = P->MenuRoot->GetComponentLocation() - Eye;
		Where = FString::Printf(TEXT("%.1f deg from the page (left is less than 0)"), FRotator::NormalizeAxis(FMath::RadiansToDegrees(FMath::Atan2(A.Y, A.X) - FMath::Atan2(B.Y, B.X))));
	}
	return FString::Printf(TEXT("%s; %s; %s"), *Where, *Panel->DescribeMaterial(), *Panel->GetData().Describe());
}

namespace
{
	// (the score screen's table) its Time Limit row is the match's last clock, the live table's own reading frozen at the end:
	// 0:00 when time ran out, what was left when the kill limit or the host ended it
	bool EndClockOk(const AOutlawsCoopSession* S, const AOutlawsFragTablePanel* Panel, FString& Said)
	{
		if (!S || !Panel) { Said = TEXT("no session or table"); return false; }
		FString Shown = TEXT("(no row)");
		for (const FFragTableHeaderRow& R : Panel->GetData().HeaderRows) if (R.Label == TEXT("Time Limit")) Shown = R.Value;
		const int32 Left = FMath::Max(0, FMath::CeilToInt(S->TimeLeft));
		const FString Want = S->TimeLimitMinutes <= 0 ? FString(TEXT("None")) : FString::Printf(TEXT("%d:%02d"), Left / 60, Left % 60);
		const bool bEnded = S->MatchState == uint8(FOutlawsMatchFlow::EState::Ended);
		Said = FString::Printf(TEXT("Time Limit %s, the match's last clock %s, the match %s"), *Shown, *Want, bEnded ? TEXT("ended") : TEXT("NOT ended"));
		return bEnded && Shown == Want;
	}
}

// ---- the tests: -OLMpNextTest=start|end|idle (the host), client (a joiner); -OLMpNextShot=<tag> its pictures --------------------

bool UOutlawsCoop::PageFits(AOutlawsPlayer* P, FString& Said)
{
	// (REQ-MPLOBBY c: the open page as last drawn, a #coop page or a sheet page: AOutlawsPlayer::MenuPageFits)
	if (!P) return false;
	return P->MenuPageFits(Said);
}

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TickNextTest(double Now, AOutlawsCoopSession* S, AOutlawsPlayer* P)
{
	const bool bClientAuto = IsClient() && (NextTest == TEXT("client") || (bAutoReady && NextTest.IsEmpty()));
	if (!P || (!bClientAuto && !(IsHost() && !NextTest.IsEmpty() && !HostSetsNextItself()))) return;
	auto Check = [](bool bOk, const FString& What) { UE_LOG(LogOutlawsCoop, Log, TEXT("OLNEXT check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What); };
	auto Photo = [this, P, &Check](const FString& What)
	{
		OutlawsMenu::FPage Page;
		const FString File = P->CoopMenuPage();
		TArray<FString> Rows, Lines;
		if (File.StartsWith(TEXT("#coop")))
		{
			BuildMenuPage(P, File, Page);
			StatusLines(P, File, Lines);
		}
		else P->BuildMpHostPage(File, Page);
		for (const OutlawsMenu::FItem& I : Page.Items) Rows.Add(I.Label + (I.Classes.Contains(TEXT("disabled")) ? TEXT(" [greyed]") : TEXT("")));
		FString Fit;
		const bool bFits = PageFits(P, Fit);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLNEXT page %s (%s): %s; rows (%d): %s; lines: %s"), *What, *File, *Page.Title, Rows.Num(), *FString::Join(Rows, TEXT(" | ")), *FString::Join(Lines, TEXT(" | ")));
		Check(bFits, FString::Printf(TEXT("%s fits the panel (%s)"), *What, *Fit));
		if (!NextShotTag.IsEmpty()) Shot(FString::Printf(TEXT("ol_%s_%s"), *NextShotTag, *What));
	};
	const FString Page = P->CoopMenuPage();
	static bool bDiedForTest = false;
	if (IsClient() && NextTest == TEXT("client") && !bDiedForTest && OLFlags::Param(TEXT("OLMpNextDead"))
		&& S->MatchState == uint8(FOutlawsMatchFlow::EState::Playing) && S->TimeLeft >= 0.f && S->TimeLeft < 3.f)
	{
		// (-OLMpNextDead: this joiner dead when the match ends, so its score screen has to stand it up first)
		bDiedForTest = true;
		if (AOutlawsLevelActor* L = FindLevel()) L->ShotOwnHead();
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLNEXT the %s dies %.1f s before the end (test)"), WhoWord(MyIndex()), S->TimeLeft);
	}
	if (IsHost())
	{
		// the host: the score screen, Continue, the Host Game page with the last match's settings, then Start (once every joiner
		// is in its lobby), End game, or nothing (the joiners' 3-minute wait)
		switch (NextTestStep)
		{
		case 0:
			if (ScorePhase != 2 || Page != TEXT("#coopscore")) return;
			NextTestAt = Now;
			NextTestStep = 1;
			return;
		case 1:
			if (Now - NextTestAt < 1.5) return;
			Check(ScoreSideOk(P), FString::Printf(TEXT("the score screen has the pause menu's frag table to its left (REQ-SCORESIDE): %s"), *ScoreSideSays(P)));
			{ FString Said; const bool bClock = EndClockOk(S, PausePanel.Get(), Said); Check(bClock, FString::Printf(TEXT("the score screen's table shows the match's last clock: %s"), *Said)); }
			Photo(TEXT("score_host"));
			NextTestAt = Now;
			NextTestStep = 2;
			return;
		case 2:
			if (Now - NextTestAt < 1.5) return;
			Check(P->CoopPressRow(TEXT("coop:continue")) && P->CoopMenuPage() == TEXT("#mphost"), FString::Printf(TEXT("the host's Continue opens the Host Game page (%s)"), *P->CoopMenuPage()));
			NextTestAt = Now;
			NextTestStep = 3;
			return;
		case 3:
		{
			if (Now - NextTestAt < 1.5) return;
			OutlawsMenu::FPage Pg;
			P->BuildMpHostPage(TEXT("#mphost"), Pg);
			const bool bEnd = Pg.Items.ContainsByPredicate([](const OutlawsMenu::FItem& I) { return I.Name == TEXT("mphost_endgame"); });
			const bool bStart = Pg.Items.ContainsByPredicate([](const OutlawsMenu::FItem& I) { return I.Name == TEXT("mphost_start"); });
			auto RowOf = [&Pg](const TCHAR* Name)
			{
				const int32 i = Pg.Items.IndexOfByPredicate([Name](const OutlawsMenu::FItem& I) { return I.Name == Name; });
				return i == INDEX_NONE ? FString() : Pg.Items[i].Kind == OutlawsMenu::EItemKind::Select && Pg.Items[i].Options.Num() > 0 ? Pg.Items[i].Label + TEXT(": ") + Pg.Items[i].Options[0] : Pg.Items[i].Label;
			};
			const FString MapRow = RowOf(TEXT("mphost_maps")), GameRow = RowOf(TEXT("mphost_class")), TimeRow = RowOf(TEXT("mphost_time"));
			Check(bStart && bEnd, TEXT("the Host Game page between matches has Start and End game"));
			const bool bMapOk = MatchOptions.bRandomMap ? MapRow.Contains(TEXT("Random each match")) : !MapRow.Contains(TEXT("Random"));
			const bool bGameOk = MatchOptions.bRandomClass ? GameRow.Contains(TEXT("Random each match")) : GameRow.Contains(OutlawsMpHost::ClassLabel(MatchOptions.GameClass));
			const bool bTimeOk = MatchOptions.TimeLimitMinutes <= 0 || TimeRow.Contains(FString::FromInt(MatchOptions.TimeLimitMinutes));
			Check(bMapOk && bGameOk && bTimeOk, FString::Printf(TEXT("it is filled with the last match's settings (%s / %s / %s; the last match: %s)"), *MapRow, *GameRow, *TimeRow, *MatchOptions.Describe()));
			Photo(TEXT("mphost"));
			NextTestAt = Now;
			NextTestStep = 4;
			return;
		}
		case 4:
		{
			if (NextTest == TEXT("idle")) return;
			if (NextTest == TEXT("end"))
			{
				if (Now - NextTestAt < 6.0) return;
				Check(P->CoopPressRow(TEXT("mphost_endgame")), TEXT("the host presses End game"));
				NextTestStep = 5;
				return;
			}
			// (start) every joiner in its lobby (out of the level), and its locked lobby photographed, first
			TArray<AOutlawsCoopLink*> Links;
			GetLinks(Links);
			int32 Joiners = 0, InLobby = 0;
			for (const AOutlawsCoopLink* K : Links)
			{
				if (K->bBot || K->PlayerIndex <= 0) continue;
				++Joiners;
				InLobby += K->bLobbyReady ? 0 : 1;
			}
			if ((Joiners == 0 || InLobby < Joiners || Now - NextTestAt < 8.0) && Now - NextTestAt < 60.0) return;
			Check(Joiners > 0 && InLobby == Joiners, FString::Printf(TEXT("every joiner is in its lobby before the host's Start (%d of %d)"), InLobby, Joiners));
			const int32 Cycle = S->MatchCycle;
			const bool bPressed = P->CoopPressRow(TEXT("mphost_start"));
			Check(bPressed && S->MatchCycle == Cycle + 1 && !S->bBetweenMatches, FString::Printf(TEXT("the host's Start sets up the next match (cycle %d -> %d)"), Cycle, S->MatchCycle));
			NextTestStep = 5;
			return;
		}
		default:
			return;
		}
	}
	// the joiner: the score screen, Continue, the locked lobby (Enter refused), then the host's Start unlocks it, and Enter
	switch (NextTestStep)
	{
	case 0:
		if (S->bBetweenMatches && Page == TEXT("#cooplobby") && bNextLobbyArmed)
		{
			NextTestAt = Now;                        // (already in its lobby: the score screen was left another way)
			NextTestStep = 3;
			return;
		}
		if (ScorePhase != 2 || Page != TEXT("#coopscore")) return;
		NextTestAt = Now;
		NextTestStep = 1;
		return;
	case 1:
		if (Now - NextTestAt < 2.0) return;
		if (NextTest == TEXT("client"))
		{
			OutlawsMenu::FPage Pg;
			BuildMenuPage(P, TEXT("#coopscore"), Pg);
			Check(Pg.Items.Num() > 0 && Pg.Items[0].Name == TEXT("coop:continue"), TEXT("Continue is on the score screen at once"));
			Check(ScoreSideOk(P), FString::Printf(TEXT("the score screen has the pause menu's frag table to its left (REQ-SCORESIDE): %s"), *ScoreSideSays(P)));
			{ FString Said; const bool bClock = EndClockOk(S, PausePanel.Get(), Said); Check(bClock, FString::Printf(TEXT("the score screen's table shows the match's last clock: %s"), *Said)); }
			Photo(FString::Printf(TEXT("score_joiner%d"), MyIndex()));
		}
		NextTestAt = Now;
		NextTestStep = 2;
		return;
	case 2:
		if (Now - NextTestAt < 1.0) return;
		P->CoopPressRow(TEXT("coop:continue"));
		if (NextTest == TEXT("client")) Check(P->CoopMenuPage() == TEXT("#cooplobby"), FString::Printf(TEXT("the joiner's Continue opens the lobby (%s)"), *P->CoopMenuPage()));
		NextTestAt = Now;
		NextTestStep = 3;
		return;
	case 3:
	{
		if (Now - NextTestAt < 2.0 || !S->bBetweenMatches)
		{
			if (!S->bBetweenMatches) NextTestStep = 4;      // (the host was quicker: the lobby is open already)
			return;
		}
		if (NextTest == TEXT("client"))
		{
			OutlawsMenu::FPage Pg;
			BuildMenuPage(P, TEXT("#cooplobby"), Pg);
			TArray<FString> Lines;
			StatusLines(P, TEXT("#cooplobby"), Lines);
			const bool bGrey = Pg.Items.Num() > 0 && Pg.Items[0].Name.StartsWith(TEXT("coop:enter")) && Pg.Items[0].Classes.Contains(TEXT("disabled"));
			const bool bSays = Lines.ContainsByPredicate([](const FString& X) { return X == TEXT("Waiting for the host to start the next match"); });
			const bool bRest = Pg.Items.ContainsByPredicate([](const OutlawsMenu::FItem& I) { return I.Target == TEXT("#character"); })
				&& Pg.Items.ContainsByPredicate([](const OutlawsMenu::FItem& I) { return I.Target == TEXT("options.rml"); })
				&& Pg.Items.ContainsByPredicate([](const OutlawsMenu::FItem& I) { return I.Name == TEXT("coop:leave"); });
			Check(bGrey && bSays, TEXT("the lobby is locked: Enter Game greyed out, \"Waiting for the host to start the next match\""));
			Check(bRest, TEXT("Character, the players, Options and Quit Game are on the locked lobby"));
			const AOutlawsCoopLink* Mine = GetLink(MyIndex());
			Check(Mine && !Mine->bLobbyReady, TEXT("the host counts this joiner in the lobby (out of the level)"));
			Photo(FString::Printf(TEXT("locked_joiner%d"), MyIndex()));
			NextTestAt = Now;
			NextTestStep = 7;                        // (Enter tried once the picture is taken: its refusal's line is not in it)
			return;
		}
		NextTestStep = 4;
		return;
	}
	case 7:
	{
		if (Now - NextTestAt < 1.5) return;
		const bool bWas = P->CoopMenuPage() == TEXT("#cooplobby") && S->bBetweenMatches;
		EnterGame(P, 0);
		if (bWas) Check(!bEnterPressed && InLobbyHere(), TEXT("Enter Game is refused while the lobby is locked"));
		else UE_LOG(LogOutlawsCoop, Log, TEXT("OLNEXT the host started the next match before the locked lobby's Enter was tried"));
		NextTestStep = 4;
		return;
	}
	case 4:
		// the host's next match: its level built behind the lobby, the lobby open again with Enter Game, then Enter
		if (S->bBetweenMatches || !bLevelBuilt || StartedSerial != S->StartSerial || Page != TEXT("#cooplobby")) return;
		if (NextTest == TEXT("client"))
		{
			OutlawsMenu::FPage Pg;
			BuildMenuPage(P, TEXT("#cooplobby"), Pg);
			TArray<FString> Lines;
			StatusLines(P, TEXT("#cooplobby"), Lines);
			const bool bOpen = Pg.Items.Num() > 0 && Pg.Items[0].Name.StartsWith(TEXT("coop:enter")) && !Pg.Items[0].Classes.Contains(TEXT("disabled"));
			Check(bOpen, FString::Printf(TEXT("the host started the next match: the lobby unlocked (%s; %s)"), Pg.Items.Num() > 0 ? *Pg.Items[0].Label : TEXT("?"), *FString::Join(Lines, TEXT(" | "))));
		}
		NextTestAt = Now;
		NextTestStep = 5;
		return;
	case 5:
		if (Now - NextTestAt < 2.0) return;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLNEXT the %s enters the next match"), WhoWord(MyIndex()));
		EnterGame(P, 0);
		NextTestStep = 6;
		return;
	default:
		// (the next end: the same again)
		if (S->bBetweenMatches) NextTestStep = 0;
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS

// ---- -OLMpRandomTest ----------------------------------------------------------------------------------------------------------

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::RunRandomTest()
{
	int32 Pass = 0, Fail = 0;
	auto Check = [&](bool bOk, const FString& What)
	{
		(bOk ? Pass : Fail)++;
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLNEXT random test %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
	};
	const FString Root = FOutlawsAssets::FindInstallRoot(FString());
	const TSharedPtr<FOutlawsAssets> A = Root.IsEmpty() ? nullptr : FOutlawsAssets::GetShared(Root);
	TArray<FOutlawsMpMap> Maps;
	OutlawsMpHost::ListMaps(A.Get(), Maps);
	const int32 Stock = Maps.FilterByPredicate([](const FOutlawsMpMap& M) { return !M.bCustom; }).Num();
	Check(Stock >= 6, FString::Printf(TEXT("the map list: %d stock maps, %d custom"), Stock, Maps.Num() - Stock));
	auto MapOf = [&Maps](const FString& File) { return Maps.FindByPredicate([&File](const FOutlawsMpMap& M) { return M.File == File; }); };
	// rolls N matches in a row as the host does (each against the last), checking every roll; returns the maps and games seen
	auto Rolls = [&](FOutlawsMpHostOptions O, int32 N, TSet<FString>& SeenMaps, TSet<int32>& SeenClasses, bool& bAllValid, bool& bChanged, TArray<FString>& Said)
	{
		bAllValid = true;
		bChanged = true;
		const bool bWasMap = O.bRandomMap, bWasClass = O.bRandomClass;
		FString LastMap = O.MapFile;
		EOutlawsGameClass LastClass = O.GameClass;
		for (int32 k = 0; k < N; ++k)
		{
			FString Why;
			if (!OutlawsMpHost::RollMatch(Maps, O, LastMap, LastClass, Why)) { bAllValid = false; Said.Add(TEXT("no roll: ") + Why); continue; }
			const FOutlawsMpMap* M = MapOf(O.MapFile);
			const bool bOk = M && M->Allows(O.GameClass) && OutlawsMpHost::IsClassOn(O.GameClass) && (!O.bRandomMap || !M->bCustom);
			bAllValid &= bOk;
			if (O.bRandomMap) bChanged &= !O.MapFile.Equals(LastMap);
			if (O.bRandomClass && !O.bRandomMap) bChanged &= O.GameClass != LastClass;
			SeenMaps.Add(O.MapFile);
			SeenClasses.Add(int32(O.GameClass));
			if (Said.Num() < 6) Said.Add(MatchLineFor(uint8(O.GameClass), O.MapFile, (O.bRandomMap ? 1 : 0) | (O.bRandomClass ? 2 : 0)));
			LastMap = O.MapFile;
			LastClass = O.GameClass;
		}
		Check(O.bRandomMap == bWasMap && O.bRandomClass == bWasClass, TEXT("the Random flags stay as the page set them after the rolls (re-rolled each match)"));
	};
	{
		FOutlawsMpHostOptions O;
		O.MapFile = TEXT("drygulch.rcm");
		O.GameClass = EOutlawsGameClass::DeathMatch;
		O.bRandomMap = true;
		TSet<FString> Ms; TSet<int32> Cs; bool bValid, bChanged; TArray<FString> Said;
		Rolls(O, 30, Ms, Cs, bValid, bChanged, Said);
		Check(bValid && bChanged && Ms.Num() >= 3 && Cs.Num() == 1 && Cs.Contains(int32(EOutlawsGameClass::DeathMatch)),
			FString::Printf(TEXT("random map, Death Match: 30 rolls, every one a stock map that plays it, never the same map twice running, %d maps seen (%s ...)"), Ms.Num(), *FString::Join(Said, TEXT("; "))));
	}
	{
		FOutlawsMpHostOptions O;
		O.MapFile = TEXT("drygulch.rcm");
		O.GameClass = EOutlawsGameClass::CaptureTheFlag;
		O.bRandomMap = true;
		TSet<FString> Ms; TSet<int32> Cs; bool bValid, bChanged; TArray<FString> Said;
		Rolls(O, 20, Ms, Cs, bValid, bChanged, Said);
		Check(bValid && Cs.Num() == 1 && Cs.Contains(int32(EOutlawsGameClass::CaptureTheFlag)),
			FString::Printf(TEXT("random map, Capture The Flag: only maps that allow it (%d seen: %s)"), Ms.Num(), *FString::Join(Ms.Array(), TEXT(", "))));
	}
	{
		FOutlawsMpHostOptions O;
		O.MapFile = TEXT("civpow2.rcm");
		O.GameClass = EOutlawsGameClass::DeathMatch;
		O.bRandomClass = true;
		TSet<FString> Ms; TSet<int32> Cs; bool bValid, bChanged; TArray<FString> Said;
		Rolls(O, 30, Ms, Cs, bValid, bChanged, Said);
		const FOutlawsMpMap* M = MapOf(TEXT("civpow2.rcm"));
		Check(bValid && bChanged && Ms.Num() == 1 && Cs.Num() >= 2, FString::Printf(TEXT("random game on Buckshotville [%s]: 30 rolls, each a game it allows, never the same twice running, %d games seen (%s ...)"),
			M ? *M->ModesText() : TEXT("?"), Cs.Num(), *FString::Join(Said, TEXT("; "))));
	}
	{
		FOutlawsMpHostOptions O;
		O.MapFile = TEXT("drygulch.rcm");
		O.bRandomMap = O.bRandomClass = true;
		TSet<FString> Ms; TSet<int32> Cs; bool bValid, bChanged; TArray<FString> Said;
		Rolls(O, 40, Ms, Cs, bValid, bChanged, Said);
		Check(bValid && bChanged && Ms.Num() >= 3 && Cs.Num() >= 2, FString::Printf(TEXT("random map and game: 40 rolls, every pair valid, %d maps and %d games seen (%s ...)"), Ms.Num(), Cs.Num(), *FString::Join(Said, TEXT("; "))));
	}
	{
		FOutlawsMpHostOptions O;
		O.MapFile = TEXT("drygulch.rcm");
		O.GameClass = EOutlawsGameClass::TeamPlay;
		FString Why;
		const bool bSame = OutlawsMpHost::RollMatch(Maps, O, TEXT("fort.rcm"), EOutlawsGameClass::DeathMatch, Why) && O.MapFile == TEXT("drygulch.rcm") && O.GameClass == EOutlawsGameClass::TeamPlay;
		Check(bSame, TEXT("neither random: the chosen map and game stay"));
		// (a random map keeps a game the last rolled map does not play: the next roll finds a map for it)
		O.bRandomMap = true;
		O.GameClass = EOutlawsGameClass::Tag;
		O.Clamp(MapOf(TEXT("drygulch.rcm")));
		Check(O.GameClass == EOutlawsGameClass::Tag, TEXT("random map: the page keeps the chosen game whatever the last map played"));
	}
	Check(MatchLineFor(6, TEXT("drygulch.rcm"), 1) == TEXT("Death Match on Dry Gulch  (random map)") && MatchLineFor(5, TEXT("fort.rcm"), 3) == TEXT("Team Play on The Fort  (random map and game)")
		&& MatchLineFor(1, TEXT("civpow2.rcm"), 2) == TEXT("Capture The Flag on Buckshotville  (random game)") && MatchLineFor(6, TEXT("fort.rcm"), 0) == TEXT("Death Match on The Fort"),
		TEXT("the lobby's and the score screen's line says what was rolled (\"Death Match on Dry Gulch  (random map)\")"));
	UE_LOG(LogOutlawsCoop, Display, TEXT("OLNEXT random test done: %d passed, %d failed"), Pass, Fail);
	FGenericPlatformMisc::RequestExit(false);
}
#endif // WITH_OUTLAWS_TESTS
