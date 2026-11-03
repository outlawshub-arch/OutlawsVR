// REQ-LOBBYREDO's probe (user 3 Nov: the lobby redone after the original's lobby_main.rml, the plan's P1, P2, P3 and P5 taken):
//   -OLShots -OLLevel=town -OLNoFrontEnd -OLLobbyRedoProbe [-OLOldLobby] [-ResX=1832 -ResY=1920 -ForceRes]
// The lobby page (#cooplobby) and the co-op host's page (#coophost) are opened over a made-up session in turn (the view the page
// would have, OutlawsLobbyPage::Test): Death Match, Team Play, Capture the Flag, a co-op joiner's and the co-op host's own, each
// with 1, 4, 8 and 16 players, and the two team games and Death Match between matches.  Each case checks the items in their
// order (ENTER GAME (n) with the players in the game, JOIN RED (n) and JOIN BLUE (n) in a team game, CHARACTER, OPTIONS, QUIT
// GAME; START GAME, LEVEL, OPTIONS, QUIT GAME on the co-op host's), ENTER greyed and out of the laser's reach between matches,
// every text on the clear parchment and none under the menus' floor (AOutlawsPlayer::MenuPageFits), every player in the panel
// (two columns over 8, RED left and BLUE right in a team game), the items' laser boxes apart.  The sizes seen are logged once
// (title, items, lines and names, in degrees).  One line a case "OLSHOT lobbyredo case PASS|FAIL: ...", then "OLSHOT lobbyredo
// RESULT".  -OLOldLobby: today's page, every case FAILs (the new page is not drawn).
// REQ-LOBBYUNDER (user 3 Nov): every case also checks this player's own name is underlined and no other text is: the line under
// its letters (its descenders too, or where the row step leaves no room for that, under its baseline), as wide as the name's
// letters, a fifth of the capitals thick (0.15 to 0.3 of them), clear of every text under it and inside the row step; and a
// team joiner with no side yet, first on the "No side yet" line, underlined there.

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsCoop.h"
#include "OutlawsFlags.h"
#include "OutlawsLobbyPage.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsShotTests.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsLobbyRedoProbeLocal
{
	struct FCase
	{
		FString Name;
		FString Mode;                    // dm, team, ctf, coop, coophost
		int32 Players = 1;
		bool bLocked = false;
		bool bHostHere = false;
		bool bShot = false;
		bool bNoSide = false;            // (a team game: this player has not picked a side yet)
	};
	TArray<FCase> RedoCases;
	int32 Passes = 0, Fails = 0, RedoAt = -1;
	float RedoWait = 0.f;
	bool bSizesSaid = false;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lobbyredo case %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}

	/** The made-up session a case shows: the host (Marshal, a star), this player, stand-ins (one long name, cut to its column). */
	OutlawsLobbyPage::FView MakeView(const FCase& C)
	{
		using namespace OutlawsLobbyPage;
		FView V;
		const bool bTeams = C.Mode == TEXT("team") || C.Mode == TEXT("ctf");
		V.bMatch = C.Mode != TEXT("coop") && C.Mode != TEXT("coophost");
		V.bTeams = bTeams;
		V.bHostPage = C.Mode == TEXT("coophost");
		V.bCharacter = C.Mode != TEXT("coophost");
		V.bLocked = C.bLocked;
		V.Title = V.bMatch ? TEXT("OUTLAWS LOBBY") : TEXT("CO-OP LOBBY");
		V.Level = TEXT("Hideout");
		if (C.Mode == TEXT("dm")) V.State = { C.Players == 1 ? FString(TEXT("Kill The Fool With The Chicken on Hargrove's Pattern Repair")) : FString(TEXT("Death Match on Dry Gulch")) };
		else if (C.Mode == TEXT("team")) V.State = { TEXT("Team Play on Dry Gulch") };
		else if (C.Mode == TEXT("ctf")) V.State = { TEXT("Capture the Flag on Hargrove's Pattern Repair") };
		else if (C.Mode == TEXT("coop")) V.State = { TEXT("The host is playing Hideout"), TEXT("Bad guys x1.50 (the host's choice)") };
		else V.State = { TEXT("Waiting for friends to join"), TEXT("Bad guys x1.00") };
		if (V.bMatch) V.State.Add(C.bLocked ? TEXT("Waiting for the host to start the next match") : C.Players == 1 ? TEXT("Waiting for a second player") : TEXT("Starts in 24 s (sooner as players enter)"));
		if (C.bHostHere) V.Foot = TEXT("Your address  LAN-ADDRESS-1    port 7797");
		static const TCHAR* Names[] = { TEXT("Marshal"), TEXT("Deputy1"), TEXT("Kate"), TEXT("Slim"), TEXT("Big Bad Barbara Le Fou of New Orleans"), TEXT("Doc"),
			TEXT("Bob Graham"), TEXT("Matt Morgan"), TEXT("Kid Calico"), TEXT("Ugly Jim"), TEXT("Cactus Pete"), TEXT("Two-Gun Tess"), TEXT("Rattler"), TEXT("Ol' Clem"),
			TEXT("Spittin' Jack"), TEXT("Gatling Jim") };
		const int32 Me = C.bHostHere ? 0 : FMath::Min(1, C.Players - 1);
		for (int32 i = 0; i < C.Players && i < int32(UE_ARRAY_COUNT(Names)); ++i)
		{
			FPlayer& P = V.Players.AddDefaulted_GetRef();
			P.Name = Names[i];
			P.Who = i;
			P.bHost = i == 0;
			P.bYou = i == Me;
			P.bInGame = C.Mode != TEXT("coophost") && i % 3 != 1;
			// (a team game: red, blue in turn, every fifth with no side yet; 16 players: 7 red, 6 blue, 3 none)
			P.Team = bTeams ? uint8(i % 5 == 4 ? 0 : 1 + i % 2) : 0;
			if (C.bNoSide && P.bYou) P.Team = 0;
			V.InGame += P.bInGame ? 1 : 0;
			V.Red += P.Team == 1 ? 1 : 0;
			V.Blue += P.Team == 2 ? 1 : 0;
		}
		V.bInLobby = true;
		V.Bots = C.Mode == TEXT("dm") && C.Players == 4 ? 3 : 0;
		return V;
	}
}

OL_SHOT_MATCH(OLLobbyRedoProbe) { return OLFlags::Param(TEXT("OLLobbyRedoProbe")); }

OL_SHOT_TEST(OLLobbyRedoProbe, 1860)
{
	using namespace OutlawsLobbyRedoProbeLocal;
	using namespace OutlawsLobbyPage;
	if (!L || ShotStep < 0) return;
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 3.f || !bUp) return;
		L->bInvincible = true;
		L->StartSaveDelay = -1.f;           // (no autosaves: the probe writes nothing but its pictures)
		L->AutosaveWait = 0.f;
		for (const TCHAR* Mode : { TEXT("dm"), TEXT("team"), TEXT("ctf"), TEXT("coop"), TEXT("coophost") })
		{
			for (const int32 N : { 1, 4, 8, 16 })
			{
				FCase& C = RedoCases.AddDefaulted_GetRef();
				C.Mode = Mode;
				C.Players = N;
				C.bHostHere = FCString::Strcmp(Mode, TEXT("dm")) == 0 || FCString::Strcmp(Mode, TEXT("team")) == 0 || FCString::Strcmp(Mode, TEXT("coophost")) == 0 ? N != 4 : false;
				C.bShot = N == 16 || (N == 4 && FCString::Strcmp(Mode, TEXT("dm")) == 0);
				C.Name = FString::Printf(TEXT("%s_%d%s"), Mode, N, C.bHostHere ? TEXT("_host") : TEXT("_joiner"));
			}
		}
		for (const TCHAR* Mode : { TEXT("dm"), TEXT("team"), TEXT("ctf") })
		{
			FCase& C = RedoCases.AddDefaulted_GetRef();
			C.Mode = Mode;
			C.Players = 8;
			C.bLocked = true;
			C.bShot = FCString::Strcmp(Mode, TEXT("team")) == 0;
			C.Name = FString::Printf(TEXT("%s_8_between_matches"), Mode);
		}
		{
			// (REQ-LOBBYUNDER: a team joiner who has not picked a side: its name on the "No side yet" line, underlined there)
			FCase& C = RedoCases.AddDefaulted_GetRef();
			C.Mode = TEXT("team");
			C.Players = 8;
			C.bNoSide = true;
			C.bShot = true;
			C.Name = TEXT("team_8_joiner_no_side");
		}
		if (!bMenuOpen) OpenMenu();
		RedoAt = 0;
		RedoWait = ShotTime + 0.5f;
		ShotStep = 1;
		return;
	}
	case 1:
	{
		if (ShotTime < RedoWait) return;
		if (RedoAt >= RedoCases.Num())
		{
			Test().bOn = false;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lobbyredo RESULT %s (%d passed, %d failed%s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
				Old() ? TEXT(", -OLOldLobby") : TEXT(""));
			ShotStep = -1;
			FGenericPlatformMisc::RequestExit(false);
			return;
		}
		const FCase& C = RedoCases[RedoAt];
		Test().bOn = true;
		Test().bHostHere = C.bHostHere;
		Test().View = MakeView(C);
		const FView& V = Test().View;
		const FString File = C.Mode == TEXT("coophost") ? TEXT("#coophost") : TEXT("#cooplobby");
		ShowMenuPage(File, false);
		RedrawMenu();
		// the items in their order
		TArray<FString> Want;
		if (V.bHostPage) Want = { TEXT("START GAME"), TEXT("LEVEL: HIDEOUT"), TEXT("OPTIONS"), TEXT("QUIT GAME") };
		else
		{
			if (V.bTeams) Want = { FString::Printf(TEXT("JOIN RED (%d)"), V.Red), FString::Printf(TEXT("JOIN BLUE (%d)"), V.Blue) };
			else Want = { FString::Printf(TEXT("ENTER GAME (%d)"), V.InGame) };
			Want.Append({ TEXT("CHARACTER"), TEXT("OPTIONS"), TEXT("QUIT GAME") });
		}
		TArray<FString> Got;
		if (MenuPage) for (const OutlawsMenu::FItem& I : MenuPage->Items) Got.Add(I.Label);
		const bool bOrder = Got == Want;
		// greyed between matches: ENTER (or both JOINs) out of the laser's reach, the rest in it
		int32 Greyed = 0, Reach = 0;
		if (MenuPage) for (int32 i = 0; i < MenuPage->Items.Num(); ++i)
		{
			const bool bOff = MenuPage->Items[i].Classes.Contains(TEXT("disabled"));
			Greyed += bOff ? 1 : 0;
			Reach += FrontHits.ContainsByPredicate([i](const TPair<FBox2D, int32>& H) { return H.Value == i; }) ? 1 : 0;
		}
		const int32 WantGrey = C.bLocked ? (V.bTeams ? 2 : 1) : 0;
		const bool bGrey = Greyed == WantGrey && Reach == Got.Num() - WantGrey;
		// the laser boxes apart
		bool bApart = true;
		for (int32 a = 0; a < FrontHits.Num(); ++a) for (int32 b = a + 1; b < FrontHits.Num(); ++b) bApart &= !FrontHits[a].Key.Intersect(FrontHits[b].Key);
		// the parchment, the floor
		FString Said;
		const bool bFits = MenuPageFits(Said);
		// every player in the panel, in one column up to 8, two over; a team game's sides left and right of the panel's middle
		const FDrawn& D = Drawn();
		const int32 WantCols = V.bTeams || V.Players.Num() > 8 ? 2 : 1;
		const bool bNames = D.Names == V.Players.Num() && D.Columns == WantCols && D.File == File;
		bool bSides = true;
		const float Mid = D.Panel.GetCenter().X;
		for (const FPlayer& Pl : V.Players)
		{
			if (!V.bTeams || Pl.Team == 0) continue;
			const FInk* I = D.Texts.FindByPredicate([&Pl](const FInk& X) { return X.bName && X.Text.StartsWith(Pl.Name.Left(8)); });
			if (I) bSides &= Pl.Team == 1 ? I->Box.Max.X < Mid : I->Box.Min.X > Mid;
		}
		float Title = 0.f, Item = 0.f, Small = 1000.f, Line = 0.f;
		for (const FInk& I : D.Texts)
		{
			if (I.Text == V.Title) Title = I.CapPx;
			else if (I.Item >= 0) Item = FMath::Max(Item, I.CapPx);
			else Line = FMath::Max(Line, I.CapPx);
			if (I.CapPx > 0.f) Small = FMath::Min(Small, I.CapPx);
		}
		// every line whole (a name may be cut to its column, "No side yet"'s one name to the panel; only the longest stock match line,
		// the 1-player case's, is cut)
		int32 CutLines = 0;
		for (const FInk& I : D.Texts)
		{
			const bool bOneName = I.Text.StartsWith(TEXT("No side yet:")) && !I.Text.Contains(TEXT(" + "));
			CutLines += !I.bName && I.Item < 0 && I.Text.EndsWith(TEXT("...")) && !bOneName && !I.Text.StartsWith(TEXT("Kill The Fool")) ? 1 : 0;
		}
		// REQ-LOBBYUNDER: this player's own name underlined, no other text; under its letters, as wide as them, about the font's
		// stroke thick, clear of every text under it (its shadow too) and inside the row step (above the next row's capitals)
		const FPlayer* MeP = V.Players.FindByPredicate([](const FPlayer& X) { return X.bYou; });
		int32 Unders = 0;
		const FInk* Mine = nullptr;
		for (const FInk& I : D.Texts) if (I.Under.bIsValid) { ++Unders; Mine = &I; }
		FString Under = FString::Printf(TEXT("%d underlined"), Unders);
		bool bUnder = MeP && Unders == 1 && Mine->Text.Contains(MeP->Name.Left(FMath::Min(7, MeP->Name.Len())));
		if (Mine)
		{
			const FBox2D& U = Mine->Under;
			const FBox2D& Of = Mine->UnderOf;
			const float Cap = Mine->CapPx, Thick = U.Max.Y - U.Min.Y, Gap = U.Min.Y - Of.Max.Y;
			const FBox2D Shade(U.Min, U.Max + FVector2D(ShadeX, ShadeY));
			const bool bDesc = Of.Max.Y > Mine->Base + Cap * 0.15f;
			const bool bLow = Gap >= 1.f || (bDesc && U.Min.Y >= Mine->Base + 1.f);
			const bool bWide = U.Min.X <= Of.Min.X + 0.5f && U.Max.X >= Of.Max.X - 0.5f && Of.Max.X <= Mine->Ink.Max.X + 0.01f;
			const bool bThick = Cap > 0.f && Thick >= Cap * 0.15f && Thick <= Cap * 0.3f;
			float Clear = D.Panel.Max.Y - 16.f - Shade.Max.Y;
			FString Next = TEXT("the frame");
			for (const FInk& I : D.Texts)
			{
				if (&I == Mine || !I.Ink.bIsValid || I.Ink.Max.X < Shade.Min.X || I.Ink.Min.X > Shade.Max.X || I.Ink.Max.Y < Shade.Min.Y) continue;
				if (I.Ink.Min.Y - Shade.Max.Y < Clear) { Clear = I.Ink.Min.Y - Shade.Max.Y; Next = I.Text; }
			}
			const bool bStep = Shade.Max.Y <= Mine->Base + D.NameStep - Cap;
			bUnder &= bLow && bWide && bThick && Clear >= 1.f && bStep;
			Under += FString::Printf(TEXT(": \"%s\" %s %.1f, %.1f thick (%.2f of its capitals), %.0f wide, %.1f clear of \"%s\"%s"), *Mine->Text.Mid(0, 24),
				Gap >= 0.f ? TEXT("under its letters by") : TEXT("under its baseline, across its descenders' tips by"), FMath::Abs(Gap), Thick, Cap > 0.f ? Thick / Cap : 0.f, U.GetSize().X,
				Clear, *Next.Left(20), bStep ? TEXT(", inside the row step") : TEXT(", OUT OF THE ROW STEP"));
		}
		const bool bOk = bOrder && bGrey && bApart && bFits && bNames && bSides && CutLines == 0 && bUnder;
		Check(bOk, FString::Printf(TEXT("%s: items [%s]%s; greyed %d, in reach %d; laser boxes %s; panel %d of %d players in %d column%s, step %.0f%s; %d lines cut; %s%s; %s"), *C.Name, *FString::Join(Got, TEXT(" | ")),
			bOrder ? TEXT("") : *FString::Printf(TEXT(" (wanted [%s])"), *FString::Join(Want, TEXT(" | "))), Greyed, Reach, bApart ? TEXT("apart") : TEXT("OVERLAP"), D.Names, V.Players.Num(), D.Columns,
			D.Columns == 1 ? TEXT("") : TEXT("s"), D.NameStep, V.bTeams ? (bSides ? TEXT(", red left and blue right") : TEXT(", SIDES WRONG")) : TEXT(""), CutLines, *Under, bUnder ? TEXT("") : TEXT(" (UNDERLINE WRONG)"), *Said));
		if (!bSizesSaid && Title > 0.f && Item > 0.f)
		{
			bSizesSaid = true;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lobbyredo sizes seen: title %.0f px cap = %.2f deg, items %.0f = %.2f deg, lines and names %.0f = %.2f deg, smallest %.0f = %.2f deg (floor %.2f deg = %.1f px)"),
				Title, SeenDeg(Title), Item, SeenDeg(Item), Line, SeenDeg(Line), Small, SeenDeg(Small), FloorDeg, FloorPx());
		}
		if (C.bShot)
		{
			FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("lobbyredo_%s%s.png"), *C.Name, Old() ? TEXT("_old") : TEXT("")), false, false);
		}
		++RedoAt;
		RedoWait = ShotTime + (C.bShot ? 1.2f : 0.4f);
		return;
	}
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
