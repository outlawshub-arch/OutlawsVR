// REQ-MPLOBBY, landing (c): the session's pages fit their panel (BUG-MENUOFFPANEL, dotard 12 Oct on a Quest 2: "Switch
// character" half off the pause menu's Players page, under a line per player and bot), and the pause Players page's mute is
// per player (one row for each player with a microphone, the lobby's mute bits).  The player lists of the lobby and of the
// Players page are cut to what the page holds under its lines: a page of them and a "More players" row that shows the next
// page (8 players on a Quest host, 16 on a PC host).
#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsPlayer.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsBuildInfo.h"
#include "OutlawsDeathmatch.h"
#include "Engine/NetConnection.h"
#include "GameFramework/PlayerController.h"

void UOutlawsCoop::PlayerRows(TArray<FPlayerRow>& Out) const
{
	// the players in the session in their order, bots left out; then a test's stand-ins (-OLFakePlayers) in the free places
	const AOutlawsCoopSession* S = GetSession();
	TArray<AOutlawsCoopLink*> Links;
	GetLinks(Links);
	Links.Sort([](const AOutlawsCoopLink& A, const AOutlawsCoopLink& B) { return A.PlayerIndex < B.PlayerIndex; });
	TSet<int32> Used;
	for (const AOutlawsCoopLink* K : Links)
	{
		Used.Add(K->PlayerIndex);
		if (K->bBot) continue;
		FPlayerRow R;
		R.Who = K->PlayerIndex;
		R.Name = K->PlayerName.TrimStartAndEnd().IsEmpty() ? FString::Printf(TEXT("Player %d"), R.Who + 1) : K->PlayerName.TrimStartAndEnd();
		R.bMic = K->bMic;
		R.bInGame = R.Who == 0 ? S && S->StartSerial > 0 && S->bHostReady : IsPlayerEntered(K) && K->BuiltSerial > 0;
		if (IsHost() && R.Who != 0)
		{
			const APlayerController* PC = Cast<APlayerController>(K->GetOwner());
			R.PingMs = PC && PC->NetConnection ? PC->NetConnection->AvgLag * 1000.f : -1.f;
		}
		else if (!IsHost() && R.Who == 0)
		{
			R.PingMs = PingMs();
		}
		Out.Add(R);
	}
	for (int32 k = 0, Who = 0; k < FakePlayers && Who < MaxPlayers; ++Who)
	{
		if (Used.Contains(Who)) continue;
		FPlayerRow R;
		R.Who = Who;
		R.Name = FString::Printf(TEXT("Stand-in %d"), Who + 1);
		R.bMic = k % 2 == 0;
		R.bInGame = k % 3 != 0;
		R.PingMs = 30.f + 7.f * k;
		Out.Add(R);
		++k;
	}
	Out.Sort([](const FPlayerRow& A, const FPlayerRow& B) { return A.Who < B.Who; });
}

void UOutlawsCoop::MenuPageChanged(const UObject* Context)
{
	if (UOutlawsCoop* C = Get(Context)) C->PlayerListPage = 0;
}

int32 UOutlawsCoop::BotsInSession() const
{
	TArray<AOutlawsCoopLink*> Links;
	GetLinks(Links);
	int32 N = 0;
	for (const AOutlawsCoopLink* K : Links) N += K->bBot ? 1 : 0;
	return N;
}

int32 UOutlawsCoop::FrontRowRoom(int32 NumLines)
{
	// DrawFrontCanvas's compact #coop pages (view units, 1920 x 1080): the lines from y 362, 38 apart; the rows from 12 under
	// them, at least 50 apart, the last one's middle at 975 at most (the help line's letters start at about 1015)
	const float Top = 362.f + 38.f * float(FMath::Max(NumLines, 0)) + 12.f;
	return FMath::Max(1, FMath::FloorToInt((975.f - Top) / 50.f + 0.001f) + 1);
}

void UOutlawsCoop::AddPlayerRows(OutlawsMenu::FPage& Page, int32 At, TArray<OutlawsMenu::FItem>& Players, int32 NumLines)
{
	using namespace OutlawsMenu;
	const int32 Room = FrontRowRoom(NumLines);
	const int32 Others = Page.Items.Num();
	At = FMath::Clamp(At, 0, Others);
	if (Others + Players.Num() <= Room)
	{
		Page.Items.Insert(Players, At);
		return;
	}
	// (a page of them, then "More players (9-14 of 16)": the next page, round again after the last)
	const int32 Shown = FMath::Max(1, Room - Others - 1);
	const int32 Pages = (Players.Num() + Shown - 1) / Shown;
	const int32 Pg = ((PlayerListPage % Pages) + Pages) % Pages;
	const int32 From = Pg * Shown, To = FMath::Min(Players.Num(), From + Shown);
	TArray<FItem> Part;
	for (int32 i = From; i < To; ++i) Part.Add(Players[i]);
	FItem More;
	More.Kind = EItemKind::Action;
	More.Label = FString::Printf(TEXT("More players  (%d-%d of %d)  >"), From + 1, To, Players.Num());
	More.Name = TEXT("coop:moreplayers");
	Part.Add(More);
	Page.Items.Insert(Part, At);
}

void UOutlawsCoop::PausePageLines(TArray<FString>& Out) const
{
	const AOutlawsCoopLink* OL = GetLink(PartnerIndex());
	const AOutlawsCoopSession* S = GetSession();
	TArray<FPlayerRow> Rows;
	PlayerRows(Rows);
	if (IsHost() && !(S && S->bClientConnected) && Rows.Num() <= 1)
	{
		Out.Add(TEXT("No one else is here yet: friends can join any time"));
		Out.Add(FString::Printf(TEXT("Your address:  %s    internet: %s    port %d"), LanAddress.IsEmpty() ? TEXT("(unknown)") : *LanAddress,
			!bShowInternet || InternetAddress.IsEmpty() ? TEXT("(not asked)") : *InternetAddress, ListenPort));
	}
	else
	{
		// (REQ-MPLOBBY c: each other player is a row under these lines, its ping and its mute; the bots are counted here only)
		const int32 NumBots = BotsInSession();
		const int32 Most = FMath::Max(S ? S->MaxPlayers : 2, Rows.Num() + NumBots);
		Out.Add(NumBots > 0 ? FString::Printf(TEXT("Players: %d of %d, and %d bot%s"), Rows.Num(), Most, NumBots, NumBots == 1 ? TEXT("") : TEXT("s"))
			: FString::Printf(TEXT("Players: %d of %d"), Rows.Num(), Most));
	}
	Out.Add(FString::Printf(TEXT("Your build:  %s"), *OutlawsBuildInfo::Line()));
	if (OL && !OL->BuildLine.IsEmpty() && Rows.Num() <= 2)
	{
		Out.Add(FString::Printf(TEXT("Partner's build:  %s%s"), *OL->BuildLine, OL->BuildLine == OutlawsBuildInfo::Line() ? TEXT("") : TEXT("   (not the same)")));
	}
}

void UOutlawsCoop::BuildPausePage(AOutlawsPlayer* P, OutlawsMenu::FPage& Page)
{
	// REQ-PAUSETIDY's Players page: the match's rows (the Scoreboard, the character, the team), then the other players, each
	// with its ping and, for one with a microphone, its mute (REQ-MPLOBBY c: "Mute partner" for everyone is gone)
	using namespace OutlawsMenu;
	Page.Title = TEXT("PLAYERS");
	auto Add = [&Page](EItemKind Kind, const FString& Label, const FString& Name, const FString& Target = FString())
	{
		FItem I;
		I.Kind = Kind;
		I.Label = Label;
		I.Name = Name;
		I.Target = Target;
		Page.Items.Add(I);
	};
	if (FOutlawsMpCharacter::bMatchMenu)
	{
		Add(EItemKind::Action, TEXT("Scoreboard"), TEXT("coop:scoreboard"));                    // (M1: the match's frag table, up until closed)
		Add(EItemKind::Link, TEXT("Switch character"), FString(), TEXT("#character"));          // (M1: worn from the next spawn)
		if (IsTeamMatch(P)) Add(EItemKind::Action, MyTeam(P) == 1 ? TEXT("Team: Red (join Blue)") : TEXT("Team: Blue (join Red)"), TEXT("coop:team"));
	}
	TArray<FPlayerRow> Rows;
	PlayerRows(Rows);
	TArray<FItem> Players;
	for (const FPlayerRow& R : Rows)
	{
		if (R.Who == MyIndex()) continue;
		FItem I;
		const FString Text = FString::Printf(TEXT("%s%s%s"), *R.Name, R.Who == 0 ? TEXT(" (host)") : TEXT(""),
			R.PingMs >= 0.f ? *FString::Printf(TEXT("  -  ping %.0f ms"), R.PingMs) : TEXT(""));
		if (R.bMic)
		{
			I.Kind = EItemKind::Action;
			I.Label = Text + (IsVoiceMuted(R.Who) ? TEXT("  -  Muted") : TEXT("  -  Mute"));
			I.Name = FString::Printf(TEXT("coop:mute:%d"), R.Who);
		}
		else
		{
			I.Kind = EItemKind::Heading;
			I.Label = Text;
			I.Name = FString::Printf(TEXT("coop:player:%d"), R.Who);
		}
		Players.Add(I);
	}
	TArray<FString> Lines;
	PausePageLines(Lines);
	AddPlayerRows(Page, Page.Items.Num(), Players, Lines.Num());
}
