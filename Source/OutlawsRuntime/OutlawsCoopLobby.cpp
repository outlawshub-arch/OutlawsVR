// REQ-MPLOBBY, landing (a) (the user 12 Oct, after a remaster comparison: Common.kpf menu/lobby_main.rml): one lobby for host
// and joiners, in a match and in co-op.  The host's Start loads the level behind it; the host lands in it, and so does every
// joiner.  Its rows: Enter Game (Join Red / Join Blue with counts in a team match; Enter means ready and drops the player into
// the level, in warm-up in a match), Character, the players (In lobby / In game, a mute button by anyone with a microphone),
// Options and Quit Game.  No Ready, no chat, no room code.  A joiner who enters after the start joins from the host's snapshot
// (R7's late join, unchanged).  Level changes and restarts stay in the game: the lobby is only the session's way in.
//
// The match's start rule (FOutlawsLobbyStart, OutlawsMpHost.cpp): a 60 s countdown from the first Enter, time left = 60 x
// (players still in the lobby / all players), everyone in starts it now; a lone host gets the full 60 s, then bots if on, else
// it waits for a second real player.  Bots never count.  The host can still start early from pause ("Start the game").
#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsMpHost.h"
#include "Engine/World.h"
#include "Components/CapsuleComponent.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "OutlawsDeathmatch.h"
#include "OutlawsFlags.h"
#include "OutlawsLobbyPage.h"
#include "OutlawsFragTable.h"

namespace OutlawsLobbyPage
{
	FDrawn& Drawn()
	{
		static FDrawn D;
		return D;
	}

	bool Old()
	{
		static const bool bOld = OLFlags::Param(TEXT("OLOldLobby"));
		return bOld;
	}

	float SeenDeg(float ViewPx)
	{
		// (the wide sheet: 128 cm wide at MenuDistance / MenuScale = 96 cm, its 1920 view units across; at its middle)
		return FMath::RadiansToDegrees(FMath::Atan(ViewPx * 128.f / 1920.f / 96.f));
	}

	float FloorPx()
	{
		return FMath::Tan(FMath::DegreesToRadians(FloorDeg)) * 96.f * 1920.f / 128.f;
	}

#if WITH_OUTLAWS_TESTS
	FTest& Test()
	{
		static FTest T;
		return T;
	}
#endif
}

bool UOutlawsCoop::LobbyViewFor(const UObject* Context, const FString& File, OutlawsLobbyPage::FView& Out)
{
	const UOutlawsCoop* C = Get(Context);
	if (!C) return false;
	C->LobbyView(File, Out);
	return true;
}

void UOutlawsCoop::LobbyView(const FString& File, OutlawsLobbyPage::FView& V) const
{
	using namespace OutlawsLobbyPage;
	V = FView();
#if WITH_OUTLAWS_TESTS
	if (Test().bOn)
	{
		V = Test().View;
		return;
	}
#endif
	const AOutlawsCoopSession* S = GetSession();
	const UWorld* W = CurrentWorld();
	const bool bMatch = IsMatch();
	V.bMatch = bMatch;
	V.bHostPage = File == TEXT("#coophost");
	V.bTeams = bMatch && S && S->bTeams;
	V.bCharacter = bMatch || !IsHost();
	V.bInLobby = InLobbyHere();
	V.bLocked = bMatch && S && S->bBetweenMatches;
	V.Title = bMatch ? TEXT("OUTLAWS LOBBY") : TEXT("CO-OP LOBBY");
	// the panel: everyone in the session but the bots (they are a count at its foot, P5)
	TArray<FPlayerRow> Rows;
	PlayerRows(Rows);
	for (const FPlayerRow& R : Rows)
	{
		FPlayer& Pl = V.Players.AddDefaulted_GetRef();
		Pl.Name = R.Name;
		Pl.Who = R.Who;
		Pl.Team = V.bTeams ? R.Team : 0;
		Pl.bInGame = R.bInGame;
		Pl.bYou = R.Who == MyIndex();
		Pl.bHost = R.Who == 0;
		V.InGame += R.bInGame ? 1 : 0;
		V.Red += Pl.Team == 1 ? 1 : 0;
		V.Blue += Pl.Team == 2 ? 1 : 0;
	}
	V.Bots = BotsInSession();
	// the foot: the host's address, the host only (P3: joining is by address; the router's detail is on the Host Game page)
	if (IsHost()) V.Foot = FString::Printf(TEXT("Your address  %s    port %d"), LanAddress.IsEmpty() ? TEXT("(unknown)") : *LanAddress, ListenPort);
	// the state, at most two lines (P2): what is played and where, then what the page waits for
	if (V.bHostPage)
	{
		FString Saved;
		const bool bCont = CampaignAutosave(Saved) && LobbyLoadSlot == -2;
		V.Level = bCont ? FString::Printf(TEXT("Continue (%s)"), *Saved) : CampaignLevelTitle(LevelToStart);
		V.State.Add(!S || !S->bClientConnected ? FString(TEXT("Waiting for friends to join"))
			: FString(TEXT("Start Game takes in everyone who has entered")));
		V.State.Add(FString::Printf(TEXT("Bad guys x%.2f%s"), BadGuysMultiplier(), IsBadGuysAuto() ? TEXT(" (Auto)") : TEXT("")));
#if PLATFORM_ANDROID
		if (BadGuysMultiplier() > 2.5f) V.State[1] = TEXT("Many bad guys: a Quest host may slow down");       // (R14)
#endif
		return;
	}
	if (IsClient() && !(W && W->GetNetMode() == NM_Client))
	{
		V.State.Add(FString::Printf(TEXT("Connecting to %s ..."), *ConnectingTo));
		return;
	}
	if (!S) return;
	if (bMatch)
	{
		V.State.Add(MatchLine());
		if (V.bLocked) V.State.Add(TEXT("Waiting for the host to start the next match"));        // (REQ-MPLOBBY b: as the old page said it)
		else if (!V.bInLobby) V.State.Add(TEXT("Entering the game ..."));
		else switch (static_cast<FOutlawsMatchFlow::EState>(S->MatchState))
		{
		case FOutlawsMatchFlow::EState::WarmUp:
			V.State.Add(S->LobbyLeft >= 0.f ? FString::Printf(TEXT("Starts in %d s (sooner as players enter)"), FMath::CeilToInt(S->LobbyLeft))
				: S->LobbyLeft < -1.5f ? FString(TEXT("Waiting for a second player"))
				: FString(TEXT("The countdown starts when a player enters")));
			break;
		case FOutlawsMatchFlow::EState::Countdown: V.State.Add(TEXT("The match is starting")); break;
		case FOutlawsMatchFlow::EState::Playing: V.State.Add(TEXT("The match is under way: enter to join it")); break;
		case FOutlawsMatchFlow::EState::Ended: V.State.Add(TEXT("The match has ended")); break;
		default: break;
		}
	}
	else
	{
		if (S->StartSerial == 0) V.State.Add(bEnterPressed ? TEXT("Waiting for the host to start") : TEXT("Enter now: you go in with the host's Start"));
		else V.State.Add(FString::Printf(TEXT("The host is playing %s"), *CampaignLevelTitle(S->Level)));
		V.State.Add(FString::Printf(TEXT("Bad guys x%.2f (the host's choice)"), S->Multiplier));        // (R14; co-op only)
	}
	const AOutlawsCoopLink* Mine = IsClient() ? GetLink(MyIndex()) : nullptr;
	if (Mine && Mine->bLateJoin && bEnterPressed && S->StartSerial > 0)
	{
		// (R7: the host's game on its way, in the second line's place)
		const FString Join = Mine->JoinState == JoinHostBusy ? TEXT("Between levels: you join at the next one")
			: Mine->JoinState == JoinSent ? TEXT("Joining the host's game ...") : TEXT("Asking the host for its game ...");
		if (V.State.Num() >= 2) V.State[1] = Join;
		else V.State.Add(Join);
	}
}

void UOutlawsCoop::BuildCoopHostPage(OutlawsMenu::FPage& Page)
{
	// REQ-LOBBYREDO: the co-op host's own page before its start in the lobby's layout (OutlawsLobbyPage.h): the same four rows
	// as before (Start, the level on its own page, the co-op options, Leave) as the big items, the friends in the panel
	using namespace OutlawsMenu;
	OutlawsLobbyPage::FView V;
	LobbyView(TEXT("#coophost"), V);
	Page.Title = V.Title;
	auto Add = [&Page](EItemKind Kind, const FString& Label, const FString& Name, const FString& Target = FString())
	{
		FItem I;
		I.Kind = Kind;
		I.Label = Label;
		I.Name = Name;
		I.Target = Target;
		Page.Items.Add(I);
	};
	Add(EItemKind::Action, TEXT("START GAME"), TEXT("coop:start"));
	Add(EItemKind::Action, TEXT("LEVEL: ") + V.Level.ToUpper(), TEXT("coop:level"));
	Add(EItemKind::Link, TEXT("OPTIONS"), FString(), TEXT("#coophostopts"));
	Add(EItemKind::Action, TEXT("QUIT GAME"), TEXT("coop:leave"));
}

void AOutlawsCoopLink::ServerMic_Implementation(bool bOn)
{
	if (!Admit(EOLGate::Ready, true)) return;
	bMic = bOn;
	ForceNetUpdate();
}

bool UOutlawsCoop::IsPlayerEntered(const AOutlawsCoopLink* K) const
{
	const AOutlawsCoopSession* S = GetSession();
	if (!K) return false;
	return K->PlayerIndex == 0 ? S && S->bHostReady : K->bLobbyReady;
}

bool UOutlawsCoop::PlayerInLobby(int32 Who) const
{
	// (the level started and this player not entered: the host by its own flag, a joiner by its link's; a bot never)
	const AOutlawsCoopSession* S = GetSession();
	if (Mode == EOutlawsCoopMode::None || !S || S->StartSerial == 0 || Who < 0) return false;
	if (Who == 0) return !S->bHostReady;
	const AOutlawsCoopLink* K = GetLink(Who);
	return K && !K->bBot && !K->bLobbyReady;
}

bool UOutlawsCoop::InLobbyFor(const UObject* Context, int32 Who)
{
	const UOutlawsCoop* C = Get(Context);
	return C && C->PlayerInLobby(Who);
}

void UOutlawsCoop::TickLobbyBody(AOutlawsPlayer* P)
{
	// (the host) its pawn stands at the level's start while it is in the lobby: the bots walk through it, none hunts it, and
	// every other machine draws nothing there (AOutlawsRemotePlayer); back in the way once it enters
	if (!IsHost() || !P) return;
	const bool bAway = PlayerInLobby(0);
	if (bAway == bHostBodyAway && (!bAway || HostBodyPawn.Get() == P)) return;          // (a new pawn while away: set again)
	const bool bSay = bAway != bHostBodyAway;
	bHostBodyAway = bAway;
	HostBodyPawn = P;
	if (UCapsuleComponent* Cap = P->GetCapsuleComponent()) Cap->SetCollisionResponseToChannel(ECC_Pawn, bAway ? ECR_Ignore : ECR_Block);
	if (bSay) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby body: the host's own body %s"), bAway ? TEXT("is out of the way and out of harm (in the lobby)") : TEXT("is in the level again (entered)"));
}

bool UOutlawsCoop::InLobbyHere() const
{
	const AOutlawsCoopSession* S = GetSession();
	if (Mode == EOutlawsCoopMode::None || !S) return false;
	return IsHost() ? !S->bHostReady : !bEnterPressed;
}

void UOutlawsCoop::EnterGame(AOutlawsPlayer* P, int32 Team)
{
	AOutlawsCoopSession* S = GetSession();
	if (!S || !P || Mode == EOutlawsCoopMode::None || !InLobbyHere()) return;
	if (IsHost() && S->StartSerial == 0) return;              // (the co-op host's own page has Start, not Enter)
	if (S->bBetweenMatches && IsMatch())
	{
		// (REQ-MPLOBBY b: greyed out until the host starts the next match)
		P->CoopToast(TEXT("Waiting for the host to start the next match"));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby: the %s's Enter refused: the host has not started the next match"), WhoWord(MyIndex()));
		return;
	}
	bEnterPressed = true;
	if (Team == 1 || Team == 2)
	{
		if (IsHost()) LobbyTeam[0] = uint8(Team);
		else if (AOutlawsCoopLink* Mine = GetLink(MyIndex()); Mine && MyIndex() > 0) Mine->ServerMatchTeam(uint8(Team));
	}
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby: the %s enters the game%s (%s)"), WhoWord(MyIndex()), Team == 1 ? TEXT(" on the red team") : Team == 2 ? TEXT(" on the blue team") : TEXT(""),
		S->StartSerial == 0 ? TEXT("before the start: it goes in with the host's Start") : IsHost() ? TEXT("into the host's level") : TEXT("it asks the host for its game"));
	SetReady(true);                                           // (the host's own runs here at once)
	// (REQ-MAPAUTHOR) the first match has no "Next match" line: who made the map goes up in white as the player goes in
	if (IsMatch() && S->MatchCycle == 0 && OutlawsMpHost::MapTitleBy(S->MapName) != OutlawsMpHost::MapTitle(S->MapName))
	{
		if (AOutlawsLevelActor* L = FindLevel()) L->Notify(MatchLineFor(S->MatchClass, S->MapName, S->RandomBits, true), 5.f);
	}
	// (REQ-MPLOBBY b: a joiner whose next-match level is built behind its lobby goes straight in, as the host does)
	const bool bBuiltHere = IsClient() && bLevelBuilt && StartedSerial == S->StartSerial;
	if (IsHost() || bBuiltHere) P->CoopCloseMenu();
	bNextLobbyArmed = false;
	++Version;
}

void UOutlawsCoop::BuildLobbyPage(AOutlawsPlayer* P, OutlawsMenu::FPage& Page)
{
	using namespace OutlawsMenu;
	const AOutlawsCoopSession* S = GetSession();
	const bool bMatch = IsMatch();
	Page.Title = bMatch ? TEXT("LOBBY") : TEXT("CO-OP LOBBY");
	auto Add = [&Page](EItemKind Kind, const FString& Label, const FString& Name, const FString& Target = FString())
	{
		FItem I;
		I.Kind = Kind;
		I.Label = Label;
		I.Name = Name;
		I.Target = Target;
		Page.Items.Add(I);
	};
	if (!OutlawsLobbyPage::Old())
	{
		// REQ-LOBBYREDO (user 3 Nov): the items only, as the original's lobby_main.rml has them (ENTER GAME, or JOIN RED and JOIN
		// BLUE in a team game, CHARACTER, QUIT GAME) with OPTIONS as a fourth (P1: a joiner has no pause menu over the lobby, and
		// turning, height and voice are only reachable from here).  The players are the panel's (DrawFrontCanvas), not rows: no
		// More players, no mute here (P4: the pause Players page keeps it).  Entered, or between matches, ENTER is greyed out.
		OutlawsLobbyPage::FView V;
		LobbyView(TEXT("#cooplobby"), V);
		Page.Title = V.Title;
		const int32 First = Page.Items.Num();
		if (V.bTeams)
		{
			Add(EItemKind::Action, FString::Printf(TEXT("JOIN RED (%d)"), V.Red), V.bInLobby ? TEXT("coop:enter:1") : TEXT("coop:entered"));
			Add(EItemKind::Action, FString::Printf(TEXT("JOIN BLUE (%d)"), V.Blue), V.bInLobby ? TEXT("coop:enter:2") : TEXT("coop:entered:2"));
		}
		else Add(EItemKind::Action, FString::Printf(TEXT("ENTER GAME (%d)"), V.InGame), V.bInLobby ? TEXT("coop:enter") : TEXT("coop:entered"));
		if (!V.bInLobby || V.bLocked) for (int32 i = First; i < Page.Items.Num(); ++i) Page.Items[i].Classes = TEXT("disabled");
		if (V.bCharacter) Add(EItemKind::Link, TEXT("CHARACTER"), FString(), TEXT("#character"));
		Add(EItemKind::Link, TEXT("OPTIONS"), FString(), TEXT("options.rml"));
		Add(EItemKind::Action, TEXT("QUIT GAME"), TEXT("coop:leave"));
		return;
	}
	if (InLobbyHere())
	{
		const int32 First = Page.Items.Num();
		if (bMatch && S && S->bTeams)
		{
			int32 Red = 0, Blue = 0;
			for (const FOutlawsMatchScore& Sc : S->Scores) { Red += Sc.Team == 1 ? 1 : 0; Blue += Sc.Team == 2 ? 1 : 0; }
			Add(EItemKind::Action, FString::Printf(TEXT("Join Red  (%d)"), Red), TEXT("coop:enter:1"));
			Add(EItemKind::Action, FString::Printf(TEXT("Join Blue  (%d)"), Blue), TEXT("coop:enter:2"));
		}
		else Add(EItemKind::Action, TEXT("Enter Game"), TEXT("coop:enter"));
		// (REQ-MPLOBBY b: between matches the Enter rows are greyed out until the host starts the next match)
		if (bMatch && S && S->bBetweenMatches) for (int32 i = First; i < Page.Items.Num(); ++i) Page.Items[i].Classes = TEXT("disabled");
	}
	else Add(EItemKind::Heading, S && S->StartSerial == 0 ? TEXT("Entered: waiting for the host to start") : TEXT("Entering the game ..."), TEXT("coop:entered"));
	if (bMatch || !IsHost())
	{
		// (a match's character, or REQ-COOPCHAR's joiner's look and name; the co-op host stays James Anderson)
		const FOutlawsMpCharacter* Ch = FOutlawsMpCharacter::Find(CoopCharChoice());
		Add(EItemKind::Link, FString::Printf(TEXT("Character: %s"), Ch ? Ch->Name : TEXT("James Anderson")), FString(), TEXT("#character"));
	}
	// the players: In lobby / In game; a mute button by anyone else with a microphone on (REQ-MPLOBBY c: as many as fit under
	// the lines, then a "More players" row for the next page of them)
	TArray<FPlayerRow> Rows;
	PlayerRows(Rows);
	TArray<FItem> Players;
	for (const FPlayerRow& R : Rows)
	{
		FItem I;
		const FString Row = FString::Printf(TEXT("%s%s  -  %s"), *R.Name, R.Who == MyIndex() ? TEXT(" (you)") : R.Who == 0 ? TEXT(" (host)") : TEXT(""), R.bInGame ? TEXT("In game") : TEXT("In lobby"));
		if (R.Who != MyIndex() && R.bMic)
		{
			I.Kind = EItemKind::Action;
			I.Label = Row + (IsVoiceMuted(R.Who) ? TEXT("  -  Muted") : TEXT("  -  Mute"));
			I.Name = FString::Printf(TEXT("coop:mute:%d"), R.Who);
		}
		else
		{
			I.Kind = EItemKind::Heading;
			I.Label = Row;
			I.Name = FString::Printf(TEXT("coop:player:%d"), R.Who);
		}
		Players.Add(I);
	}
	const int32 PlayersAt = Page.Items.Num();
	Add(EItemKind::Link, TEXT("Options"), FString(), TEXT("options.rml"));
	Add(EItemKind::Action, TEXT("Quit Game"), TEXT("coop:leave"));
	TArray<FString> Lines;
	LobbyLines(Lines);
	AddPlayerRows(Page, PlayersAt, Players, Lines.Num());
}

void UOutlawsCoop::LobbyLines(TArray<FString>& Out) const
{
	const AOutlawsCoopSession* S = GetSession();
	const UWorld* W = CurrentWorld();
	const bool bLocked = S && S->bBetweenMatches && IsMatch();      // (REQ-MPLOBBY b: the waiting lines instead, room for the rows)
	if (IsClient())
	{
		if (!bLocked) Out.Add(W && W->GetNetMode() == NM_Client ? FString::Printf(TEXT("Joined the game at %s"), *ConnectingTo) : FString::Printf(TEXT("Connecting to %s ..."), *ConnectingTo));
	}
	else
	{
		Out.Add(FString::Printf(TEXT("Your address:  %s    port %d"), LanAddress.IsEmpty() ? TEXT("(unknown)") : *LanAddress, ListenPort));
		if (!bLocked) UpnpLines(Out);                // (REQ-UPNP: whether the router opened the port)
	}
	if (!S) return;
	if (IsMatch() && S->bBetweenMatches)
	{
		// (REQ-MPLOBBY b: the last match's, until the host starts the next one)
		Out.Add(TEXT("Waiting for the host to start the next match"));
		Out.Add(TEXT("Last match: ") + MatchLine());
	}
	else if (IsMatch())
	{
		Out.Add(MatchLine());
		switch (static_cast<FOutlawsMatchFlow::EState>(S->MatchState))
		{
		case FOutlawsMatchFlow::EState::WarmUp:
			Out.Add(S->LobbyLeft >= 0.f ? FString::Printf(TEXT("The match starts in %d s (sooner as players enter)"), FMath::CeilToInt(S->LobbyLeft))
				: S->LobbyLeft < -1.5f ? FString(TEXT("Warm-up: waiting for a second player"))
				: FString(TEXT("Warm-up: the countdown starts when the first player enters")));
			break;
		case FOutlawsMatchFlow::EState::Countdown: Out.Add(TEXT("The match is starting")); break;
		case FOutlawsMatchFlow::EState::Playing: Out.Add(TEXT("The match is under way: Enter Game to join it")); break;
		case FOutlawsMatchFlow::EState::Ended: Out.Add(TEXT("The match has ended")); break;
		default: break;
		}
	}
	else
	{
		if (S->StartSerial == 0) Out.Add(bEnterPressed ? TEXT("Waiting for the host to start") : TEXT("Enter Game when you are ready: the host's Start takes you in"));
		else Out.Add(FString::Printf(TEXT("The host is playing %s"), *CampaignLevelTitle(S->Level)));
		Out.Add(FString::Printf(TEXT("Bad guys: x%.2f (the host's choice)"), S->Multiplier));        // (R14; co-op only)
	}
	const AOutlawsCoopLink* Mine = IsClient() ? GetLink(MyIndex()) : nullptr;
	if (Mine && Mine->bLateJoin && bEnterPressed && S->StartSerial > 0)
	{
		// (R7: the host's game on its way)
		Out.Add(Mine->JoinState == JoinHostBusy ? TEXT("The host is between levels: you join at the next one")
			: Mine->JoinState == JoinSent ? TEXT("Joining the host's game ...") : TEXT("Asking the host for its game ..."));
	}
}

void UOutlawsCoop::TickLobby(double Now, AOutlawsCoopSession* S, AOutlawsPlayer* P)
{
	if (!S) return;
	TickLobbyBody(P);
	// (REQ-LOBBYREDO: the HUD's "Waiting for players" line off while the lobby page is up: its state line says the same)
	const bool bPageUp = !OutlawsLobbyPage::Old() && P && P->CoopMenuPage() == TEXT("#cooplobby");
	// (REQ-WHEELSCORE follow-up, user 3 Nov: and while the pause menu or the wheel's scorecard is up: the line, fixed 10 degrees
	// over the view's middle, crossed the menu's Resume and the card's title row; it comes back with the next one said, 5 s at most)
	const bool bMenuUp = P && !bPageUp && P->IsMenuOpen();
	const bool bCardUp = P && P->WheelScore.IsValid() && !P->WheelScore->IsHidden();
	const bool bOff = bPageUp || bMenuUp || bCardUp;
	if (bOff && !bWaitingOff && !WaitingSaid.IsEmpty())
	{
		TArray<FString> Up;
		for (const FString& H : P->HudLines) if (H.StartsWith(TEXT("Waiting for players"))) Up.AddUnique(H);
		for (const FString& H : Up) P->DropMessage(H);
		if (bPageUp && !bWaitingHidSaid) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby: the HUD's \"%s\" is taken off while the lobby page is up"), *WaitingSaid);
		if (!bPageUp && Up.Num() > 0) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the HUD's \"%s\" is taken off while the %s is up"), *WaitingSaid, bCardUp ? TEXT("wheel's scorecard") : TEXT("pause menu"));
		bWaitingHidSaid |= bPageUp;
	}
	bWaitingOff = bOff;
	// (every machine) this player's microphone setting, for the others' mute rows
	if (AOutlawsCoopLink* Mine = (IsHost() || MyIndex() > 0) ? GetLink(MyIndex()) : nullptr)
	{
		const int8 Mic = MicSettingOn() ? 1 : 0;
		if (Mic != SentMic)
		{
			SentMic = Mic;
			Mine->ServerMic(Mic != 0);
		}
	}
	// (a joiner's Enter that went before its link had come, or a reconnect's: said again until the host counts it)
	if (IsClient() && bEnterPressed && MyIndex() > 0 && Now >= LobbyOpenTryAt)
	{
		AOutlawsCoopLink* Mine = GetLink(MyIndex());
		if (Mine && !Mine->bLobbyReady)
		{
			LobbyOpenTryAt = Now + 2.0;
			SetReady(true);
		}
	}
	// (a test's late Enter: -OLLobbyEnterAfter=<s> after this player's lobby page is up)
	if (LobbyEnterAfter > 0.f && P && InLobbyHere() && P->CoopMenuPage() == TEXT("#cooplobby"))
	{
		if (LobbyPageAt == 0.0) LobbyPageAt = Now;
		else if (!LobbyShotName.IsEmpty() && !bLobbyShotTaken && Now - LobbyPageAt >= FMath::Max(1.5, LobbyEnterAfter - 2.0))
		{
			// (-OLLobbyShot: the page photographed, its rows and lines logged)
			bLobbyShotTaken = true;
			OutlawsMenu::FPage Page;
			BuildMenuPage(P, TEXT("#cooplobby"), Page);
			TArray<FString> Rows, Lines;
			for (const OutlawsMenu::FItem& I : Page.Items) Rows.Add(I.Label);
			StatusLines(P, TEXT("#cooplobby"), Lines);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby shot %s: %s; rows (%d): %s; lines: %s"), *LobbyShotName, *Page.Title, Rows.Num(), *FString::Join(Rows, TEXT(" | ")), *FString::Join(Lines, TEXT(" | ")));
			Shot(LobbyShotName);
			// (REQ-MPLOBBY c: the page as drawn fits its panel; with more players than it holds, "More players" shows the rest)
			FString Fit;
			const bool bFits = PageFits(P, Fit);
			TArray<FPlayerRow> Listed;
			PlayerRows(Listed);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLFIT check %s: the lobby (%s, %d players) fits the panel (%s)"), bFits ? TEXT("PASS") : TEXT("FAIL"), *LobbyShotName, Listed.Num(), *Fit);
			const bool bMore = Page.Items.ContainsByPredicate([](const OutlawsMenu::FItem& I) { return I.Name == TEXT("coop:moreplayers"); });
			if (!OutlawsLobbyPage::Old())
			{
				// (REQ-LOBBYREDO: every player in the panel, no More players; nothing drawn under the floor)
				const OutlawsLobbyPage::FDrawn& D = OutlawsLobbyPage::Drawn();
				float Smallest = 1000.f;
				for (const OutlawsLobbyPage::FInk& I : D.Texts) if (I.CapPx > 0.f) Smallest = FMath::Min(Smallest, I.CapPx);
				const bool bAll = D.Names == Listed.Num() && !bMore;
				const bool bFloor = Smallest >= OutlawsLobbyPage::FloorPx() - 0.01f;
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLFIT check %s: the lobby's panel shows all %d players (%d drawn, %d column%s, %d items, no More players)"), bAll ? TEXT("PASS") : TEXT("FAIL"),
					Listed.Num(), D.Names, D.Columns, D.Columns == 1 ? TEXT("") : TEXT("s"), Page.Items.Num());
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLFIT check %s: the lobby's smallest letters %.1f deg, at least the menus' floor %.2f deg (%d texts)"), bFloor ? TEXT("PASS") : TEXT("FAIL"),
					OutlawsLobbyPage::SeenDeg(Smallest), OutlawsLobbyPage::FloorDeg, D.Texts.Num());
			}
			else
			{
				const int32 Room = FrontRowRoom(Lines.Num());
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLFIT check %s: the lobby's rows (%d) within the room under its %d lines (%d)%s"), Page.Items.Num() <= Room ? TEXT("PASS") : TEXT("FAIL"),
					Page.Items.Num(), Lines.Num(), Room, bMore ? TEXT("; More players shown") : TEXT(""));
			}
			if (bMore && P->CoopPressRow(TEXT("coop:moreplayers"))) LobbyMoreAt = Now + 1.0;
		}
		else if (LobbyMoreAt > 0.0 && Now >= LobbyMoreAt)
		{
			LobbyMoreAt = 0.0;
			OutlawsMenu::FPage Page;
			BuildMenuPage(P, TEXT("#cooplobby"), Page);
			TArray<FString> Rows;
			for (const OutlawsMenu::FItem& I : Page.Items) Rows.Add(I.Label);
			FString Fit;
			const bool bFits = PageFits(P, Fit);
			const OutlawsMenu::FItem* More = Page.Items.FindByPredicate([](const OutlawsMenu::FItem& I) { return I.Name == TEXT("coop:moreplayers"); });
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby shot %s_more: rows (%d): %s"), *LobbyShotName, Rows.Num(), *FString::Join(Rows, TEXT(" | ")));
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLFIT check %s: More players shows the next page (%s), and it fits the panel (%s)"), bFits && More && !More->Label.Contains(TEXT("(1-")) ? TEXT("PASS") : TEXT("FAIL"),
				More ? *More->Label : TEXT("no More players row"), *Fit);
			Shot(LobbyShotName + TEXT("_more"));
		}
		else if (Now - LobbyPageAt >= LobbyEnterAfter && IsHost() && bLobbyEnterAfterPeer && (LobbyPeerInAt == 0.0 || Now - LobbyPeerInAt < 5.0))
		{
			// (-OLLobbyEnterAfterPeer: the host waits for a joiner in the game, then 5 s more, so that joiner sees no body here)
			if (LobbyPeerInAt == 0.0)
			{
				TArray<AOutlawsCoopLink*> Links;
				GetLinks(Links);
				for (const AOutlawsCoopLink* K : Links)
				{
					if (K->bBot || K->PlayerIndex <= 0 || !IsPlayerEntered(K) || K->BuiltSerial != StartedSerial) continue;
					LobbyPeerInAt = Now;
					UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby test: the %s is in the game, the host still in the lobby: it enters in 5 s"), WhoWord(K->PlayerIndex));
					break;
				}
			}
		}
		else if (Now - LobbyPageAt >= LobbyEnterAfter)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby test: Enter Game %.0f s after the lobby came up"), Now - LobbyPageAt);
			EnterGame(P, 0);
		}
	}
	// (the host) its first level built: it lands in the lobby until it enters (a launched or test host enters by itself)
	if (!IsHost() || S->StartSerial == 0 || S->bHostReady) return;
	AOutlawsLevelActor* L = FindLevel();
	if (!P || !L || !bLevelBuilt || StartedSerial != S->StartSerial || L->bNeedPlace || L->IsRebuildPending() || L->IsMoviePlaying() || L->IsFrontEnd()) return;
	if (LobbyEnterAfter <= 0.f && ((bAutoReady && TestOnly != TEXT("readyflip")) || !LaunchMatchMap.IsEmpty()))
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby: the %s host enters by itself"), !LaunchMatchMap.IsEmpty() ? TEXT("launched") : TEXT("test's"));
		bEnterPressed = true;
		SetReadyFor(0, true);
		if (P->IsMenuOpen()) P->CoopCloseMenu();
		return;
	}
	if (P->IsMenuOpen() || Now < LobbyOpenTryAt) return;
	LobbyOpenTryAt = Now + 1.0;
	P->CoopOpenFrontMenu();
	if (!bHostLobbyLogged)
	{
		bHostLobbyLogged = true;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby: the host lands in the lobby over %s (%s)"), *S->Level, *P->CoopMenuPage());
	}
}

void UOutlawsCoop::TickLobbyRule(float Dt, AOutlawsCoopSession* S, int32 People)
{
	FOutlawsMatchFlow& F = OutlawsMpHost::Flow();
	F.bLobbyGate = !bLobbyRuleOff;
	float Show = -1.f;
	if (F.GetState() == FOutlawsMatchFlow::EState::Countdown || F.GetState() == FOutlawsMatchFlow::EState::Playing) Lobby.bDone = true;      // (started: by the rule or from pause)
	else if (F.GetState() == FOutlawsMatchFlow::EState::WarmUp && !bLobbyRuleOff && !Lobby.bDone)
	{
		TArray<AOutlawsCoopLink*> Links;
		GetLinks(Links);
		int32 Humans = 0, InLobby = 0;
		bool bBotsIn = false;
		for (const AOutlawsCoopLink* K : Links)
		{
			if (K->bBot) { bBotsIn = true; continue; }
			++Humans;
			if (!IsPlayerEntered(K)) ++InLobby;
		}
		if (Lobby.Tick(Dt, Humans, InLobby, MatchOptions.bBots && bBotsIn, People, MatchOptions.StartWhen))
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH the lobby starts the match: %s"), *Lobby.Why);
			if (!F.HostStart()) Lobby.bDone = false;          // (refused: tried again)
		}
		Show = Lobby.bLoneWait ? -2.f : Lobby.Left >= 0.f ? FMath::CeilToFloat(Lobby.Left) : -1.f;
	}
	if (S && S->LobbyLeft != Show)
	{
		S->LobbyLeft = Show;
		S->ForceNetUpdate();
		++Version;
	}
}

// ---- -OLLobbyRuleTest: the start rule on simulated time --------------------------------------------------------------------

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::RunLobbyRuleTest()
{
	int32 Pass = 0, Fail = 0;
	auto Check = [&](bool bOk, const FString& What)
	{
		(bOk ? Pass : Fail)++;
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLLOBBY test %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
	};
	// a step of the session as the host's tick feeds it: Humans in the session, InLobby of them, bots, People in the match
	struct FSim
	{
		FOutlawsLobbyStart R;
		float T = 0.f, StartedAt = -1.f;
		bool Run(float Seconds, int32 Humans, int32 InLobby, bool bBots, int32 People, int32 StartWhen = 0)
		{
			for (float k = 0.f; k < Seconds - 0.001f; k += 0.1f)
			{
				T += 0.1f;
				if (R.Tick(0.1f, Humans, InLobby, bBots, People, StartWhen) && StartedAt < 0.f) StartedAt = T;
			}
			return StartedAt >= 0.f;
		}
	};
	{
		FSim S;
		S.Run(100.f, 1, 1, false, 0);
		Check(S.R.Left < 0.f && S.StartedAt < 0.f, TEXT("nobody has entered: no countdown, however long"));
	}
	{
		FSim S;
		S.Run(59.f, 1, 0, true, 1);
		const bool bEarly = S.StartedAt >= 0.f;
		S.Run(2.f, 1, 0, true, 1);
		Check(!bEarly && S.StartedAt >= 59.9f && S.StartedAt <= 60.5f, FString::Printf(TEXT("lone host, bots on: the full 60 s, then the match with bots (started at %.1f s)"), S.StartedAt));
	}
	{
		FSim S;
		S.Run(90.f, 1, 0, false, 1);
		Check(S.StartedAt < 0.f && S.R.bLoneWait, TEXT("lone host, bots off: 60 s, then it waits for a second real player"));
		S.Run(5.f, 2, 1, false, 1);
		Check(S.StartedAt < 0.f && S.R.Left > 24.5f && S.R.Left <= 30.f, FString::Printf(TEXT("a second player joins the lobby: 60 x 1/2, %.1f s left"), S.R.Left));
		S.Run(1.f, 2, 0, false, 2);
		Check(S.StartedAt > 95.f && S.StartedAt < 96.5f, FString::Printf(TEXT("the second enters: everyone in, it starts now (at %.1f s)"), S.StartedAt));
	}
	{
		FSim S;
		S.Run(1.f, 2, 1, false, 1);
		Check(S.R.Left > 29.f && S.R.Left <= 30.f, FString::Printf(TEXT("two players, the host enters first: 60 x 1/2 = 30 s (%.1f s left)"), S.R.Left));
		S.Run(9.f, 2, 1, false, 1);
		S.Run(1.f, 2, 0, false, 2);
		Check(S.StartedAt > 10.f && S.StartedAt < 11.5f, FString::Printf(TEXT("the second enters 10 s later: it starts now (at %.1f s)"), S.StartedAt));
	}
	{
		FSim S;
		S.Run(35.f, 2, 1, false, 1);
		Check(S.StartedAt < 0.f && S.R.bHeld, TEXT("two players, the second never enters: at 30 s held (nobody to play against)"));
		S.Run(1.f, 2, 0, false, 2);
		Check(S.StartedAt > 35.f && S.StartedAt < 36.5f, FString::Printf(TEXT("then it enters: it starts (at %.1f s)"), S.StartedAt));
	}
	{
		FSim S;
		S.Run(0.1f, 3, 2, false, 1);
		Check(FMath::IsNearlyEqual(S.R.Left, 40.f, 0.2f), FString::Printf(TEXT("three players, the first enters: 60 x 2/3 = 40 s (%.1f s left)"), S.R.Left));
		S.Run(5.f, 3, 2, false, 1);
		S.Run(0.1f, 3, 1, false, 2);
		Check(FMath::IsNearlyEqual(S.R.Left, 20.f, 0.2f), FString::Printf(TEXT("the second enters 5 s later: 60 x 1/3 = 20 s (%.1f s left)"), S.R.Left));
		S.Run(25.f, 3, 1, false, 2);
		Check(S.StartedAt > 24.5f && S.StartedAt < 26.f, FString::Printf(TEXT("the third stays in the lobby: the countdown runs out, the match starts with two (at %.1f s)"), S.StartedAt));
		const float At = S.StartedAt;
		S.Run(5.f, 4, 2, false, 2);
		Check(S.StartedAt == At && S.R.bDone, TEXT("a late joiner after the start changes nothing: it enters into the running match"));
	}
	{
		FSim S;
		S.Run(3.f, 3, 2, false, 1);
		S.Run(2.f, 3, 1, false, 2);
		S.Run(0.1f, 3, 0, false, 3);
		Check(S.StartedAt > 5.f && S.StartedAt < 5.5f, FString::Printf(TEXT("three players entering at 0, 3 and 5 s: everyone in at 5 s, it starts now (at %.1f s)"), S.StartedAt));
	}
	{
		FSim S;
		S.Run(70.f, 1, 0, true, 1, 3);
		Check(S.StartedAt < 0.f && S.R.bHeld, TEXT("a launched host, -OLMpStartWhen=3, bots on: after 60 s still held at 1 person"));
		S.Run(10.f, 2, 0, true, 2, 3);
		Check(S.StartedAt < 0.f, TEXT("2 people: still held"));
		S.Run(1.f, 3, 0, true, 3, 3);
		Check(S.StartedAt > 80.f && S.StartedAt < 81.5f, FString::Printf(TEXT("3 people: it starts (at %.1f s)"), S.StartedAt));
	}
	UE_LOG(LogOutlawsCoop, Display, TEXT("OLLOBBY test done: %d passed, %d failed"), Pass, Fail);
	FGenericPlatformMisc::RequestExit(false);
}
#endif // WITH_OUTLAWS_TESTS
