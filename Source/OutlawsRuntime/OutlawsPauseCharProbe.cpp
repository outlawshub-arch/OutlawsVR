// REQ-PAUSECHAR's probe (the user, 3 Nov: "drop the players button in multiplayer pause menu, since we can see the scorecard below
// the weapon wheel now. And we should just have a change character button instead to replace it. It should just take you into
// the character selection screen").  Two machines, -OLPauseCharProbe on both:
//   host:   -OLMpHost=town_2p -OLMpClass=dm|team -OLMpBots=0 -OLMpChar=Doc -OLMpHostRespawn=2 -OLMpMinutes=30 -OLMpFrags=0
//           -OLFakePlayers=2 [-OLOldPauseChar]
//   client: -OLCoopJoin=<host>:<port> -OLMpChar=San -OLFakePlayers=2 [-OLOldPauseChar]
// In the warm-up, then in play, on both machines: Change character where Players was (the row over Options; a shot of the pause
// menu), Players under Options; Change character opens the six tiles; a pick says "You'll be <name> from your next spawn", reaches
// the rules, is not worn yet; Back returns to the pause menu; the Players page has no Switch character, each other person's ping
// and mute (a mute pressed and pressed back), the team switch in a team game.  The warm-up's picks are worn from the match's
// start, play's (Mar for both) from the next spawn: each machine's link and its puppet of the other say so.
// The host alone (-OLMpBots=1 -OLPauseCharSolo, no client): no Players row, the same picks.
// The story co-op (-OLCoopAuto -OLCoopHost -OLCoopLevel=hideout, -OLCoopAuto -OLCoopJoin=<host>:<port>): its pause menu as before.
// Single player (-OLPauseCharProbe=sp -OLShots -OLLevel=hideout -OLNoFrontEnd): neither row.
// -OLOldPauseChar: the rows as before (the row checks FAIL, the control; nothing is picked).
// REQ-PAUSECHAR2 (the user, 4 Nov: "move mute and ping"): no Players row in a match at all, in a team match a team switch row under
// Change character (pressed, the team changes).  In the warm-up and in play: the wheel's card has each other person's ping and
// mute (the other machine's person given a microphone, none being here; -OLFakePlayers's stand-ins in the table), a hand pointed
// at their row lights it, a pull mutes them (the same bit the page's row set; a shot), a second pull hears them again, a pull
// on this player's own row does nothing.  Tab's table on the desktop the same, by the view's middle and the left button.  The
// host alone has neither column; the warm-up's "Waiting for players" is one line, not three.  -OLOldPauseChar: neither column.

#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsDeathmatch.h"
#include "OutlawsFlags.h"
#include "OutlawsFragTable.h"
#include "OutlawsLevelActor.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsMpHost.h"
#include "OutlawsPlayer.h"
#include "OutlawsRemotePlayer.h"
#include "OutlawsShotTests.h"
#include "Camera/CameraComponent.h"
#include "Components/TextRenderComponent.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsPauseCharProbeLocal
{
	// the probe's characters: the two the machines start as, the warm-up's picks, play's
	const TCHAR* const Chars[] = { TEXT("Doc"), TEXT("San"), TEXT("Bob"), TEXT("Two"), TEXT("Mar") };
	constexpr int32 Pack(int32 Host, int32 Joiner) { return Host << 4 | Joiner; }
	constexpr int32 Starts = Pack(0, 1), WarmPicks = Pack(2, 3), PlayPicks = Pack(4, 4);

	int32 Passes = 0, Fails = 0, Client = -1, Line = 0;
	int32 TeamWas = 0, ThePerson = INDEX_NONE;           // (the team before the switch; the row's person the steps press)

	struct FLine
	{
		const TCHAR* Step;
		int32 Arg;
		double Wait;
	};
	// the host's script: each line done here and on the client, Wait seconds before the next (start, alive and end the host's own)
	const FLine InMatch[] = {
		{ TEXT("rows"), 0, 1.5 }, { TEXT("change"), 0, 1.5 }, { TEXT("pick"), WarmPicks, 1.5 }, { TEXT("rules"), WarmPicks, 0.5 }, { TEXT("worn"), Starts, 0.5 },
		{ TEXT("back"), 0, 1.0 }, { TEXT("team"), 0, 1.5 }, { TEXT("teamis"), 0, 0.5 }, { TEXT("close"), 0, 1.0 },
		{ TEXT("card"), 0, 1.5 }, { TEXT("cardpoint"), 0, 0.6 }, { TEXT("cardpull"), 0, 0.6 }, { TEXT("cardmuted"), 0, 0.6 }, { TEXT("cardpull"), 0, 0.6 }, { TEXT("cardheard"), 0, 0.6 },
		{ TEXT("cardself"), 0, 0.6 }, { TEXT("cardpull"), 0, 0.6 }, { TEXT("cardselfis"), 0, 0.6 },
		{ TEXT("cardwide"), 0, 1.0 }, { TEXT("cardwidepoint"), 0, 0.6 }, { TEXT("cardpull"), 0, 0.6 }, { TEXT("cardmuted"), 0, 0.6 }, { TEXT("cardpull"), 0, 0.6 }, { TEXT("cardheard"), 0, 0.6 }, { TEXT("cardnarrow"), 0, 0.8 },
		{ TEXT("cardshut"), 0, 0.6 }, { TEXT("tab"), 0, 0.8 }, { TEXT("tabpoint"), 0, 0.6 }, { TEXT("tabpress"), 0, 0.6 }, { TEXT("tabshut"), 0, 0.8 },
		{ TEXT("start"), 0, 3.0 }, { TEXT("worn"), WarmPicks, 0.5 },
		{ TEXT("rows"), 1, 1.5 }, { TEXT("change"), 1, 1.5 }, { TEXT("pick"), PlayPicks, 1.5 }, { TEXT("rules"), PlayPicks, 0.5 }, { TEXT("worn"), WarmPicks, 0.5 },
		{ TEXT("back"), 1, 1.0 }, { TEXT("close"), 1, 1.0 },
		{ TEXT("card"), 1, 1.5 }, { TEXT("cardpoint"), 1, 0.6 }, { TEXT("cardpull"), 1, 0.6 }, { TEXT("cardmuted"), 1, 0.6 }, { TEXT("cardpull"), 1, 0.6 }, { TEXT("cardheard"), 1, 0.6 },
		{ TEXT("cardshut"), 1, 0.6 }, { TEXT("tab"), 1, 0.8 }, { TEXT("tabpoint"), 1, 0.6 }, { TEXT("tabpress"), 1, 0.6 }, { TEXT("tabshut"), 1, 0.8 },
		{ TEXT("die"), 0, 3.0 }, { TEXT("respawn"), 0, 0.5 }, { TEXT("alive"), 0, 3.0 }, { TEXT("worn"), PlayPicks, 0.5 }, { TEXT("end"), 0, 0.0 } };
	const FLine InStory[] = { { TEXT("rows"), 0, 1.5 }, { TEXT("players"), 0, 1.5 }, { TEXT("close"), 0, 1.0 }, { TEXT("end"), 0, 0.0 } };
	const FLine OldMatch[] = { { TEXT("rows"), 0, 1.5 }, { TEXT("close"), 0, 1.0 }, { TEXT("card"), 0, 1.5 }, { TEXT("cardpoint"), 0, 0.6 }, { TEXT("cardshut"), 0, 0.6 }, { TEXT("end"), 0, 0.0 } };

	int32 ColumnOf(const FFragTableData& D, const TCHAR* Header)
	{
		// (into a row's Values)
		return D.Columns.IndexOfByPredicate([Header](const FFragTableColumn& C) { return C.Header == Header; }) - 1;
	}

	FString Cell(const FFragTableRow& R, int32 Column)
	{
		return R.Values.IsValidIndex(Column) ? R.Values[Column] : FString();
	}

	/** Where a row's middle is in the world, from the last drawing (false: not drawn). */
	bool RowPlace(const AOutlawsFragTablePanel* Panel, int32 Row, FVector& Out)
	{
		const FFragTableRowBox* B = Panel->LastRows.FindByPredicate([Row](const FFragTableRowBox& X) { return X.Row == Row; });
		if (!B) return false;
		const FVector2D Size = Panel->GetSizeCm(), Px = (B->Min + B->Max) * 0.5;
		Out = Panel->GetActorLocation() + Panel->GetActorForwardVector() * ((Px.X / Panel->GetCanvasW() - 0.5) * Size.X) + Panel->GetActorRightVector() * ((Px.Y / Panel->GetCanvasH() - 0.5) * Size.Y);
		return true;
	}

	int32 RowOf(const FFragTableData& D, int32 Who)
	{
		return Who == INDEX_NONE ? INDEX_NONE : D.Rows.IndexOfByPredicate([Who](const FFragTableRow& R) { return R.Who == Who; });
	}

	void Check(bool bOk, const TCHAR* Who, const FString& What)
	{
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT pausechar check %s (%s): %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), Who, *What);
		if (bOk) ++Passes; else ++Fails;
	}

	void Shoot(const FString& Name)
	{
		const FString Path = FPaths::ProjectSavedDir() / TEXT("OLShots") / (Name + TEXT(".png"));
		FScreenshotRequest::RequestScreenshot(Path, false, false);
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT pausechar shot: %s"), *Path);
	}

	void Result(const TCHAR* Who)
	{
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT pausechar %s RESULT %s (%d passed, %d failed)%s"), Who, Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			UOutlawsCoop::OldPauseChar() ? TEXT(" -OLOldPauseChar") : TEXT(""));
	}

	int32 RowTo(const TArray<OutlawsMenu::FItem>& Items, const TCHAR* Target)
	{
		return Items.IndexOfByPredicate([Target](const OutlawsMenu::FItem& I) { return I.Target.Equals(Target, ESearchCase::IgnoreCase); });
	}

	FString Labels(const TArray<OutlawsMenu::FItem>& Items)
	{
		TArray<FString> Out;
		for (const OutlawsMenu::FItem& I : Items) Out.Add(I.Label);
		return FString::Join(Out, TEXT(" | "));
	}
}

void UOutlawsCoop::TickHostPauseCharTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	using namespace OutlawsPauseCharProbeLocal;
	FOutlawsMatchFlow& F = OutlawsMpHost::Flow();
	const bool bSolo = OLFlags::Param(TEXT("OLPauseCharSolo"));
	if (TestStep < 2180) { TestStep = 2180; TestAt = 0.0; }
	if (TestStep == 2180)
	{
		// the client in the game (the rules have it in a match); or, on its own, the host in the rules
		TArray<AOutlawsCoopLink*> Links;
		GetLinks(Links);
		Client = -1;
		for (const AOutlawsCoopLink* K : Links)
		{
			const int32 Who = K->PlayerIndex;
			if (Who > 0 && Who < MaxPlayers && !K->bBot && K->BuiltSerial == StartedSerial && (IsMatch() ? MatchInRules[Who] : IsPlayerEntered(K))) Client = Who;
		}
		if (TestAt == 0.0) TestAt = Now + 240.0;
		const bool bReady = IsMatch() ? bMatchSetUp && MatchInRules[0] && (bSolo || Client > 0) : Client > 0;
		if (!bReady && Now < TestAt) return;
		Check(bReady, TEXT("host"), FString::Printf(TEXT("%s, the client the %s"), IsMatch() ? TEXT("a match") : TEXT("the story co-op"), Client > 0 ? WhoWord(Client) : TEXT("(none)")));
		TestStep = 2181;
		TestAt = Now + (bReady ? 4.0 : 0.0);
		Line = bReady ? 0 : -1;
		return;
	}
	if (Now < TestAt) return;
	if (TestStep == 2182) { TestStep = 2183; TestAt = Now + 3.0; TestSay(TEXT("quit")); return; }
	if (TestStep == 2183) { TestStep = 2184; QuitForTest(); return; }
	if (TestStep != 2181) return;
	const FLine* Script = !IsMatch() ? InStory : OldPauseChar() ? OldMatch : InMatch;
	const FLine Step = Line < 0 ? FLine{ TEXT("end"), 0, 0.0 } : Script[Line];
	const FString Name = Step.Step;
	if (Name == TEXT("end"))
	{
		OnPauseCharTestStep(Name, 0, L, P);
		if (Client > 0) TestSayTo(Client, TEXT("pc:end"), 0);
		TestStep = 2182;
		TestAt = Now + 3.0;
		return;
	}
	if (Name == TEXT("start"))
	{
		// M1's Start the game; the start places everyone, the warm-up's picks on
		if (F.GetState() == FOutlawsMatchFlow::EState::WarmUp) F.HostStart();
		if (F.GetState() != FOutlawsMatchFlow::EState::Playing) return;
	}
	else if (Name == TEXT("alive"))
	{
		const FOutlawsDmPlayer* A = Dm.Find(0);
		const FOutlawsDmPlayer* B = Client > 0 ? Dm.Find(Client) : nullptr;
		if (!A || !A->bAlive || (Client > 0 && (!B || !B->bAlive)) || L->bDead) return;
	}
	else
	{
		OnPauseCharTestStep(Name, Step.Arg, L, P);
		if (Client > 0 && Name != TEXT("rules")) TestSayTo(Client, TEXT("pc:") + Name, Step.Arg);
	}
	TestAt = Now + Step.Wait;
	++Line;
}

void UOutlawsCoop::OnPauseCharTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	using namespace OutlawsPauseCharProbeLocal;
	const bool bHost = IsHost(), bOld = OldPauseChar();
	const TCHAR* Who = bHost ? TEXT("host") : TEXT("client");
	const TCHAR* When = !IsMatch() ? TEXT("story") : Arg == 0 ? TEXT("warm-up") : TEXT("in play");
	auto Mine = [Who](bool bOk, const FString& What) { Check(bOk, Who, What); };
	auto PageIs = [P](const TCHAR* File) { return P->MenuPage.IsValid() && P->MenuPage->File == File; };
	const int32 MyHalf = bHost ? Arg >> 4 : Arg & 15;    // (a packed Arg: this machine's half)
	if (Step == TEXT("rows"))
	{
		P->OpenMenu(false);
		const TArray<OutlawsMenu::FItem> Items = P->MenuPage.IsValid() ? P->MenuPage->Items : TArray<OutlawsMenu::FItem>();
		const int32 Opt = RowTo(Items, TEXT("options.rml")), Char = RowTo(Items, TEXT("#character")), Play = RowTo(Items, TEXT("#cooppause"));
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT pausechar (%s, %s) pause rows: %s"), Who, When, *Labels(Items));
		if (!IsMatch())
		{
			Mine(PageIs(TEXT("main_game.rml")) && Opt > 0 && Play == Opt - 1 && Items[Play].Label == TEXT("Players") && Char == INDEX_NONE, TEXT("the story co-op's pause menu as before: Players just over Options, no Change character"));
		}
		else
		{
			Mine(PageIs(TEXT("main_game.rml")) && Opt > 0 && Char == Opt - (IsTeamMatch(P) ? 2 : 1) && Items[Char].Label == TEXT("Change character"), FString::Printf(TEXT("(%s) Change character is where Players was, over Options (row %d, Options %d)"), When, Char, Opt));
			Mine(Play == INDEX_NONE, FString::Printf(TEXT("(%s) no Players row in a match (row %d)"), When, Play));
			const int32 Team = Items.IndexOfByPredicate([](const OutlawsMenu::FItem& I) { return I.Name == TEXT("coop:team"); });
			Mine(IsTeamMatch(P) ? Team == Char + 1 && Items[Team].Label.StartsWith(TEXT("Team: ")) : Team == INDEX_NONE, FString::Printf(TEXT("(%s) the team switch: %s (row %d)"), When, IsTeamMatch(P) ? TEXT("under Change character") : TEXT("no teams, no row"), Team));
			if (Arg == 0)
			{
				// (the warm-up: the new count takes the last one's place, the alone host's start said three in a frame)
				const int32 Waits = P->HudLines.FilterByPredicate([](const FString& H) { return H.StartsWith(TEXT("Waiting for players")); }).Num();
				Mine(Waits <= 1, FString::Printf(TEXT("(%s) %d \"Waiting for players\" line%s up as the menu opens"), When, Waits, Waits == 1 ? TEXT("") : TEXT("s")));
			}
			const int32 Go = Items.IndexOfByPredicate([](const OutlawsMenu::FItem& I) { return I.Name == TEXT("mphost_matchstart"); });
			Mine(Go == (bHost && Arg == 0 ? 1 : INDEX_NONE), FString::Printf(TEXT("(%s) Start the game row %d"), When, Go));
		}
		Shoot(FString::Printf(TEXT("ol_pausechar_%s_%s%s"), Who, IsMatch() ? (Arg == 0 ? TEXT("warmup") : TEXT("play")) : TEXT("story"), bOld ? TEXT("_old") : TEXT("")));
	}
	else if (Step == TEXT("change"))
	{
		const int32 Row = P->MenuPage.IsValid() ? RowTo(P->MenuPage->Items, TEXT("#character")) : INDEX_NONE;
		Mine(Row != INDEX_NONE, FString::Printf(TEXT("(%s) a Change character row to press"), When));
		if (Row == INDEX_NONE) return;
		P->MenuActivate(1, Row);
		Mine(PageIs(TEXT("#character")) && P->MenuPage->Items.Num() == 6, FString::Printf(TEXT("(%s) it opens the character page: %s"), When, P->MenuPage.IsValid() ? *Labels(P->MenuPage->Items) : TEXT("no page")));
		Shoot(FString::Printf(TEXT("ol_pausechar_%s_page_%d"), Who, Arg));
	}
	else if (Step == TEXT("pick"))
	{
		const FString Tile = FString(TEXT("char:")) + Chars[MyHalf];
		const int32 Row = P->MenuPage.IsValid() ? P->MenuPage->Items.IndexOfByPredicate([&Tile](const OutlawsMenu::FItem& I) { return I.Name == Tile; }) : INDEX_NONE;
		Mine(Row != INDEX_NONE, FString::Printf(TEXT("the %s tile is on the page"), Chars[MyHalf]));
		if (Row == INDEX_NONE) return;
		P->MenuActivate(1, Row);
		const FString Said = P->MenuToast ? P->MenuToast->Text.ToString() : FString();
		const FOutlawsMpCharacter* Ch = FOutlawsMpCharacter::Find(Chars[MyHalf]);
		Mine(PageIs(TEXT("#character")) && Said == FString::Printf(TEXT("You'll be %s from your next spawn"), Ch ? Ch->Name : TEXT("?")), FString::Printf(TEXT("the page says \"%s\" and stays up"), *Said));
		Shoot(FString::Printf(TEXT("ol_pausechar_%s_picked_%s"), Who, Chars[MyHalf]));
	}
	else if (Step == TEXT("rules"))
	{
		// (the host's: the rules have both picks, for the next spawn)
		const FOutlawsDmPlayer* A = Dm.Find(0);
		const FOutlawsDmPlayer* B = Client > 0 ? Dm.Find(Client) : nullptr;
		Mine(A && A->PendingCharacter == Chars[Arg >> 4], FString::Printf(TEXT("the rules hold the host's pick: %s"), A ? *A->PendingCharacter : TEXT("?")));
		if (Client > 0) Mine(B && B->PendingCharacter == Chars[Arg & 15], FString::Printf(TEXT("the rules hold the client's pick: %s"), B ? *B->PendingCharacter : TEXT("?")));
	}
	else if (Step == TEXT("back"))
	{
		P->MenuBack();
		Mine(PageIs(TEXT("main_game.rml")) && P->bMenuOpen, FString::Printf(TEXT("(%s) Back from the character page returns to the pause menu (%s)"), When, P->MenuPage.IsValid() ? *P->MenuPage->File : TEXT("none")));
	}
	else if (Step == TEXT("players"))
	{
		const int32 Row = P->MenuPage.IsValid() ? RowTo(P->MenuPage->Items, TEXT("#cooppause")) : INDEX_NONE;
		if (Row == INDEX_NONE) return;           // (the rows' check says if it should be there)
		P->MenuActivate(1, Row);
		const TArray<OutlawsMenu::FItem> Items = P->MenuPage.IsValid() ? P->MenuPage->Items : TArray<OutlawsMenu::FItem>();
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT pausechar (%s, %s) Players page: %s"), Who, When, *Labels(Items));
		int32 Muter = -1;
		for (const OutlawsMenu::FItem& I : Items) if (I.Name.StartsWith(TEXT("coop:mute:"))) Muter = FCString::Atoi(*I.Name.Mid(10));
		const bool bPing = Items.ContainsByPredicate([](const OutlawsMenu::FItem& I) { return I.Label.Contains(TEXT("ping")); });
		const bool bTeam = Items.ContainsByPredicate([](const OutlawsMenu::FItem& I) { return I.Name == TEXT("coop:team"); });
		Mine(PageIs(TEXT("#cooppause")) && !Items.ContainsByPredicate([](const OutlawsMenu::FItem& I) { return I.Target == TEXT("#character"); }), FString::Printf(TEXT("(%s) the Players page opens, no Switch character on it"), When));
		Mine(bTeam == (IsMatch() && IsTeamMatch(P)), FString::Printf(TEXT("(%s) the team switch %s"), When, bTeam ? TEXT("is on it") : TEXT("is not (no teams)")));
		if (FakePlayers > 0)
		{
			Mine(bPing && Muter >= 0, FString::Printf(TEXT("(%s) the others' pings and mutes are on it"), When));
			if (Muter >= 0)
			{
				P->CoopPressRow(FString::Printf(TEXT("coop:mute:%d"), Muter));
				const bool bMuted = IsVoiceMuted(Muter);
				P->CoopPressRow(FString::Printf(TEXT("coop:mute:%d"), Muter));
				Mine(bMuted && !IsVoiceMuted(Muter), FString::Printf(TEXT("(%s) a mute row mutes, and hears again"), When));
			}
		}
		Shoot(FString::Printf(TEXT("ol_pausechar_%s_players_%s"), Who, IsMatch() ? (Arg == 0 ? TEXT("warmup") : TEXT("play")) : TEXT("story")));
		P->MenuBack();
		Mine(PageIs(TEXT("main_game.rml")), FString::Printf(TEXT("(%s) Back from Players returns to the pause menu"), When));
	}
	else if (Step == TEXT("close"))
	{
		if (P->bMenuOpen) P->CloseMenu();
	}
	else if (Step == TEXT("team"))
	{
		// (REQ-PAUSECHAR2: the row is the page's switch: the other team, the menu shut)
		TeamWas = MyTeam(P);
		if (!IsTeamMatch(P)) return;
		Mine(P->CoopPressRow(TEXT("coop:team")), TEXT("the team switch row pressed"));
	}
	else if (Step == TEXT("teamis"))
	{
		if (IsTeamMatch(P)) Mine(MyTeam(P) == (TeamWas == 1 ? 2 : 1) && !P->bMenuOpen, FString::Printf(TEXT("the team switched: %d to %d"), TeamWas, MyTeam(P)));
	}
	else if (Step == TEXT("card"))
	{
		// REQ-PAUSECHAR2: the wheel up, the stand-ins in the table; the other machine's person has a microphone here (no real one)
		bScoreStandIns = true;
		ThePerson = bHost ? Client : 0;
		if (AOutlawsCoopLink* K = ThePerson != INDEX_NONE && ThePerson >= 0 ? GetLink(ThePerson) : nullptr) K->bMic = true;
		P->OpenWheel(1);
	}
	else if (Step == TEXT("cardpoint"))
	{
		AOutlawsFragTablePanel* Card = P->WheelScore.Get();
		const bool bUp = Card && !Card->IsHidden() && Card->GetCanvas();
		Mine(bUp, FString::Printf(TEXT("(%s) the card is up with the wheel"), When));
		if (!bUp) return;
		const FFragTableData& D = Card->GetData();
		const int32 PingC = ColumnOf(D, TEXT("PING")), MuteC = ColumnOf(D, TEXT("VOICE"));
		for (const FFragTableRow& R : D.Rows)
		{
			UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT pausechar (%s, %s) card row: player %d, \"%s\", ping \"%s\", \"%s\"%s"), Who, When, R.Who, *R.Name, *Cell(R, PingC), *Cell(R, MuteC), R.bCanMute ? TEXT(", a press mutes") : TEXT(""));
		}
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT pausechar (%s, %s) the card: %d x %d px, %.0f cm wide"), Who, When, Card->GetCanvasW(), Card->GetCanvasH(), Card->GetSizeCm().X);
		if (bOld)
		{
			Mine(PingC < 0 && MuteC < 0, TEXT("-OLOldPauseChar: no ping or mute on the card"));
			return;
		}
		const int32 Mine1 = D.Rows.IndexOfByPredicate([](const FFragTableRow& R) { return R.bLocal; });
		Mine(D.Rows.IsValidIndex(Mine1) && !D.Rows[Mine1].bCanMute && Cell(D.Rows[Mine1], PingC).IsEmpty() && Cell(D.Rows[Mine1], MuteC).IsEmpty(), FString::Printf(TEXT("(%s) this player's own row has no ping and no mute"), When));
		if (FakePlayers <= 0 && ThePerson <= 0 && bHost)
		{
			Mine(PingC < 0 && MuteC < 0, FString::Printf(TEXT("(%s) alone, the card has neither column"), When));
			ThePerson = INDEX_NONE;
			return;
		}
		const int32 Row = RowOf(D, ThePerson);
		Mine(D.Rows.IsValidIndex(Row) && Cell(D.Rows[Row], PingC).IsNumeric() && Cell(D.Rows[Row], MuteC) == TEXT("Mute"), FString::Printf(TEXT("(%s) the other person's row: ping \"%s\", \"%s\""), When, Row >= 0 ? *Cell(D.Rows[Row], PingC) : TEXT("?"), Row >= 0 ? *Cell(D.Rows[Row], MuteC) : TEXT("?")));
		// (a stand-in with a microphone has its Mute too, the one without none)
		TArray<FPlayerRow> Listed;
		PlayerRows(Listed);
		int32 Wrong = 0, Stand = 0;
		for (const FPlayerRow& R : Listed)
		{
			const int32 At = R.bStandIn ? RowOf(D, R.Who) : INDEX_NONE;
			if (!R.bStandIn) continue;
			++Stand;
			Wrong += D.Rows.IsValidIndex(At) && D.Rows[At].bCanMute == R.bMic && (Cell(D.Rows[At], MuteC) == TEXT("Mute")) == R.bMic && Cell(D.Rows[At], PingC) == FString::Printf(TEXT("%.0f"), R.PingMs) ? 0 : 1;
		}
		Mine(Stand == FakePlayers && Wrong == 0, FString::Printf(TEXT("(%s) the %d stand-ins' rows: their pings, a Mute on those with a microphone (%d wrong)"), When, Stand, Wrong));
		FVector At = FVector::ZeroVector;
		if (!RowPlace(Card, Row, At)) { Mine(false, TEXT("the row was drawn")); return; }
		P->WheelScoreTestAim = At;
		if (AController* Ctl = P->GetController()) Ctl->SetControlRotation((Card->GetActorLocation() - P->Camera->GetComponentLocation()).Rotation());      // (the shot's: the head on the card)
	}
	else if (Step == TEXT("cardpull"))
	{
		// (a pull of the hand holding the wheel, let go in the next step's frame)
		if (ThePerson != INDEX_NONE) P->TestAxes.Add(TEXT("OLVR_RightTrigger"), 1.f);
	}
	else if (Step == TEXT("cardmuted") || Step == TEXT("cardheard"))
	{
		P->TestAxes.Remove(TEXT("OLVR_RightTrigger"));
		AOutlawsFragTablePanel* Card = P->WheelScore.Get();
		if (ThePerson == INDEX_NONE || !Card) return;
		const bool bWant = Step == TEXT("cardmuted");
		const FFragTableData& D = Card->GetData();
		const int32 Row = RowOf(D, ThePerson);
		const FString Word = D.Rows.IsValidIndex(Row) ? Cell(D.Rows[Row], ColumnOf(D, TEXT("VOICE"))) : FString();
		Mine(IsVoiceMuted(ThePerson) == bWant && MutedBits == (bWant ? 1u << ThePerson : 0u), FString::Printf(TEXT("(%s) the pull %s that person (bits 0x%x, the bit the page's row set)"), When, bWant ? TEXT("mutes") : TEXT("hears again"), MutedBits));
		Mine(Word == (bWant ? TEXT("Muted") : TEXT("Mute")) && Card->GetHotRow() == Row, FString::Printf(TEXT("(%s) the row says \"%s\", still lit (row %d, lit %d)"), When, *Word, Row, Card->GetHotRow()));
		const bool bDrawn = Card->LastTexts.ContainsByPredicate([&Word](const FFragTableText& T) { return T.Str == Word; });
		Mine(bDrawn, FString::Printf(TEXT("(%s, %d rows) \"%s\" is drawn on the card"), When, D.Rows.Num(), *Word));
		Shoot(FString::Printf(TEXT("ol_pausechar_%s_card_%s_%s%s"), Who, bWant ? TEXT("muted") : TEXT("heard"), Arg == 0 ? TEXT("warmup") : TEXT("play"), D.Rows.Num() > 4 ? TEXT("_two") : TEXT("")));
	}
	else if (Step == TEXT("cardwide"))
	{
		// (two columns of players: the word has no column of its own, it shows on the row pointed at)
		if (ThePerson != INDEX_NONE) FakePlayers = 4;
	}
	else if (Step == TEXT("cardwidepoint"))
	{
		AOutlawsFragTablePanel* Card = P->WheelScore.Get();
		if (ThePerson == INDEX_NONE || !Card) return;
		const FFragTableData& D = Card->GetData();
		const int32 Row = D.Rows.IndexOfByPredicate([](const FFragTableRow& R) { return R.bCanMute; });
		const int32 Words = Card->LastTexts.FilterByPredicate([](const FFragTableText& T) { return T.Str == TEXT("Mute"); }).Num();
		FVector At = FVector::ZeroVector;
		Mine(D.Rows.Num() == 6 && Words == 0 && RowPlace(Card, Row, At), FString::Printf(TEXT("(%s) %d players in two columns, %d Mute words until a row is pointed at"), When, D.Rows.Num(), Words));
		if (D.Rows.IsValidIndex(Row)) ThePerson = D.Rows[Row].Who;
		P->WheelScoreTestAim = At;
	}
	else if (Step == TEXT("cardnarrow"))
	{
		if (ThePerson == INDEX_NONE) return;
		FakePlayers = 2;
		ThePerson = bHost ? Client : 0;
	}
	else if (Step == TEXT("cardself"))
	{
		AOutlawsFragTablePanel* Card = P->WheelScore.Get();
		if (ThePerson == INDEX_NONE || !Card) return;
		FVector At = FVector::ZeroVector;
		const int32 Row = Card->GetData().Rows.IndexOfByPredicate([](const FFragTableRow& R) { return R.bLocal; });
		if (RowPlace(Card, Row, At)) P->WheelScoreTestAim = At;
	}
	else if (Step == TEXT("cardselfis"))
	{
		P->TestAxes.Remove(TEXT("OLVR_RightTrigger"));
		AOutlawsFragTablePanel* Card = P->WheelScore.Get();
		if (ThePerson == INDEX_NONE || !Card) return;
		Mine(Card->GetHotRow() == INDEX_NONE && MutedBits == 0u, FString::Printf(TEXT("(%s) pointed at this player's own row nothing lights and a pull mutes no one (lit %d, bits 0x%x)"), When, Card->GetHotRow(), MutedBits));
	}
	else if (Step == TEXT("cardshut"))
	{
		P->TestAxes.Remove(TEXT("OLVR_RightTrigger"));
		P->WheelScoreTestAim = FVector::ZeroVector;
		P->CloseWheel();
		if (AController* Ctl = P->GetController()) Ctl->SetControlRotation(FRotator(0.0, Ctl->GetControlRotation().Yaw, 0.0));
	}
	else if (Step == TEXT("tab"))
	{
		ToggleScoreboard(P, TEXT("the test's Tab"));
	}
	else if (Step == TEXT("tabpoint"))
	{
		// (the desktop has no wheel: Tab's table has the columns, the view's middle points as in its menus)
		AOutlawsFragTablePanel* Table = HeldScoreboard(P);
		Mine(Table != nullptr, FString::Printf(TEXT("(%s) Tab's table is up"), When));
		if (!Table || ThePerson == INDEX_NONE) return;
		const FFragTableData& D = Table->GetData();
		const int32 Row = RowOf(D, ThePerson);
		FVector At = FVector::ZeroVector;
		const bool bPlaced = RowPlace(Table, Row, At);
		Mine(bPlaced && ColumnOf(D, TEXT("PING")) >= 0 && Cell(D.Rows[Row], ColumnOf(D, TEXT("VOICE"))) == TEXT("Mute"), FString::Printf(TEXT("(%s) the table has the person's ping and Mute"), When));
		if (bPlaced) if (AController* Ctl = P->GetController()) Ctl->SetControlRotation((At - P->Camera->GetComponentLocation()).Rotation());
	}
	else if (Step == TEXT("tabpress"))
	{
		AOutlawsFragTablePanel* Table = HeldScoreboard(P);
		if (!Table || ThePerson == INDEX_NONE) return;
		const int32 Row = RowOf(Table->GetData(), ThePerson);
		Mine(Table->GetHotRow() == Row, FString::Printf(TEXT("(%s) the view's middle lights the row (row %d, lit %d)"), When, Row, Table->GetHotRow()));
		P->MenuMouseLeft();
		const bool bMuted = IsVoiceMuted(ThePerson) && MutedBits == 1u << ThePerson;
		Shoot(FString::Printf(TEXT("ol_pausechar_%s_tab_muted_%s"), Who, Arg == 0 ? TEXT("warmup") : TEXT("play")));
		P->MenuMouseLeft();
		Mine(bMuted && MutedBits == 0u, FString::Printf(TEXT("(%s) the left button mutes that person, and hears them again"), When));
	}
	else if (Step == TEXT("tabshut"))
	{
		if (HeldScoreboard(P)) ToggleScoreboard(P, TEXT("the test's Tab"));
		if (AController* Ctl = P->GetController()) Ctl->SetControlRotation(FRotator(0.0, Ctl->GetControlRotation().Yaw, 0.0));
	}
	else if (Step == TEXT("worn"))
	{
		// every machine: the link's character (each machine's copy) and its puppet of the other
		const int32 Joiner = bHost ? Client : MyIndex();
		const FString WantHost = Chars[Arg >> 4], WantJoiner = Chars[Arg & 15];
		const AOutlawsCoopLink* KH = GetLink(0);
		const AOutlawsCoopLink* KJ = Joiner > 0 ? GetLink(Joiner) : nullptr;
		Mine(KH && KH->Character == WantHost, FString::Printf(TEXT("here the host wears %s (want %s)"), KH ? *KH->Character : TEXT("?"), *WantHost));
		if (Joiner <= 0) return;
		Mine(KJ && KJ->Character == WantJoiner, FString::Printf(TEXT("here the client wears %s (want %s)"), KJ ? *KJ->Character : TEXT("?"), *WantJoiner));
		const FOutlawsMpCharacter* Ch = FOutlawsMpCharacter::Find(bHost ? WantJoiner : WantHost);
		const AOutlawsRemotePlayer* R = PuppetOf(bHost ? Joiner : 0);
		Mine(R && Ch && R->SpriteNwx.Equals(Ch->Nwx, ESearchCase::IgnoreCase), FString::Printf(TEXT("the %s's puppet here is drawn as %s (%s, want %s)"), bHost ? TEXT("client") : TEXT("host"), Ch ? Ch->Name : TEXT("?"), R ? *R->SpriteNwx : TEXT("none"), Ch ? Ch->Nwx : TEXT("?")));
	}
	else if (Step == TEXT("die"))
	{
		// (past any invincibility, as a shot to the head is)
		L->bInvincible = false;
		L->DamagePlayer(1e6f, 1.f, true);
		Mine(L->bDead, TEXT("dead, for the next spawn"));
	}
	else if (Step == TEXT("respawn"))
	{
		P->RequestRespawn(false);
	}
	else if (Step == TEXT("end"))
	{
		Result(Who);
	}
}

OL_SHOT_MATCH(OLPauseCharProbe) { return OLFlags::ValueIs(TEXT("OLPauseCharProbe"), TEXT("sp")); }

OL_SHOT_TEST(OLPauseCharProbe, 2180)
{
	using namespace OutlawsPauseCharProbeLocal;
	if (!L || ShotStep < 0 || ShotTime < 4.f || L->IsFrontEnd() || L->IsRebuildPending() || !L->HasActorBegunPlay() || L->IsMoviePlaying()) return;
	OpenMenu(false);
	const TArray<OutlawsMenu::FItem> Items = MenuPage.IsValid() ? MenuPage->Items : TArray<OutlawsMenu::FItem>();
	UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT pausechar (sp) pause rows: %s"), *Labels(Items));
	Check(Items.Num() > 0 && RowTo(Items, TEXT("#character")) == INDEX_NONE && RowTo(Items, TEXT("#cooppause")) == INDEX_NONE, TEXT("sp"), TEXT("single player has neither Change character nor Players"));
	CloseMenu();
	Result(TEXT("sp"));
	ShotStep = -1;
	FGenericPlatformMisc::RequestExit(false);
}

#endif // WITH_OUTLAWS_TESTS
