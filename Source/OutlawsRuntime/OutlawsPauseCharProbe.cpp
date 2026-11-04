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
// Lines "OLSHOT pausechar check PASS|FAIL (host|client|sp): ..." and "OLSHOT pausechar <host|client|sp> RESULT".

#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsDeathmatch.h"
#include "OutlawsFlags.h"
#include "OutlawsLevelActor.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsMpHost.h"
#include "OutlawsPlayer.h"
#include "OutlawsRemotePlayer.h"
#include "OutlawsShotTests.h"
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

	struct FLine
	{
		const TCHAR* Step;
		int32 Arg;
		double Wait;
	};
	// the host's script: each line done here and on the client, Wait seconds before the next (start, alive and end the host's own)
	const FLine InMatch[] = {
		{ TEXT("rows"), 0, 1.5 }, { TEXT("change"), 0, 1.5 }, { TEXT("pick"), WarmPicks, 1.5 }, { TEXT("rules"), WarmPicks, 0.5 }, { TEXT("worn"), Starts, 0.5 },
		{ TEXT("back"), 0, 1.0 }, { TEXT("players"), 0, 1.5 }, { TEXT("close"), 0, 1.0 }, { TEXT("start"), 0, 3.0 }, { TEXT("worn"), WarmPicks, 0.5 },
		{ TEXT("rows"), 1, 1.5 }, { TEXT("change"), 1, 1.5 }, { TEXT("pick"), PlayPicks, 1.5 }, { TEXT("rules"), PlayPicks, 0.5 }, { TEXT("worn"), WarmPicks, 0.5 },
		{ TEXT("back"), 1, 1.0 }, { TEXT("players"), 1, 1.5 }, { TEXT("close"), 1, 1.0 }, { TEXT("die"), 0, 3.0 }, { TEXT("respawn"), 0, 0.5 },
		{ TEXT("alive"), 0, 3.0 }, { TEXT("worn"), PlayPicks, 0.5 }, { TEXT("end"), 0, 0.0 } };
	const FLine InStory[] = { { TEXT("rows"), 0, 1.5 }, { TEXT("players"), 0, 1.5 }, { TEXT("close"), 0, 1.0 }, { TEXT("end"), 0, 0.0 } };
	const FLine OldMatch[] = { { TEXT("rows"), 0, 1.5 }, { TEXT("close"), 0, 1.0 }, { TEXT("end"), 0, 0.0 } };

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
			Mine(PageIs(TEXT("main_game.rml")) && Opt > 0 && Char == Opt - 1 && Items[Char].Label == TEXT("Change character"), FString::Printf(TEXT("(%s) Change character is where Players was, just over Options (row %d, Options %d)"), When, Char, Opt));
			const bool bWant = !OLFlags::Param(TEXT("OLPauseCharSolo")) || IsTeamMatch(P);
			Mine(bWant ? Play != INDEX_NONE && Play == Opt + 1 && Items[Play].Label == TEXT("Players") : Play == INDEX_NONE, FString::Printf(TEXT("(%s) Players %s (row %d)"), When, bWant ? TEXT("just under Options") : TEXT("gone: nothing of its own for it to show"), Play));
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
