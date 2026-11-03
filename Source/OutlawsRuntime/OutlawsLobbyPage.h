// REQ-LOBBYREDO (user 3 Nov 2026: the plan's picks, "build it as planned"): the lobby page redone after the original's
// (Common.kpf menu/lobby_main.rml): the multiplayer art (Multiplayer_Base from the player's UI.kpf), OUTLAWS LOBBY between its
// flourishes, at most two lines of the session's state, a PLAYERS panel on the left (the score table's DialogBox frame over its
// wood, Embellish2 under the heading) and the big items on the right.  One page for the host and the joiners (#cooplobby) and
// the same layout for the co-op host's own page before its start (#coophost).  Nothing on it under the menus' smallest face
// (1.28 degrees seen); the items 2.0, the title 3.1.  -OLOldLobby: the page as it was (one list, the #coop drawing).
#pragma once

#include "CoreMinimal.h"

namespace OutlawsLobbyPage
{
	/** A player in the PLAYERS panel: marked by colour, not words (white in the game, gold still in the lobby, yellow you), a
	 *  star by the host; a team game's side (1 red, 2 blue, 0 none picked yet). */
	struct FPlayer
	{
		FString Name;
		int32 Who = -1;
		uint8 Team = 0;
		bool bInGame = false;
		bool bYou = false;
		bool bHost = false;
	};

	/** What the page shows, from the session (UOutlawsCoop::LobbyView) or a probe's made-up one. */
	struct FView
	{
		bool bMatch = true;                  // a match (else co-op)
		bool bTeams = false;                 // Team Play and CTF: JOIN RED and JOIN BLUE, the panel split RED | BLUE
		bool bHostPage = false;              // the co-op host's own page before its start: START GAME, LEVEL, OPTIONS, QUIT GAME
		bool bCharacter = true;              // a CHARACTER item (not for the co-op host, who stays James Anderson)
		bool bInLobby = true;                // this player has not entered (else ENTER greyed: it is on its way in)
		bool bLocked = false;                // between matches: ENTER greyed until the host starts the next match
		int32 InGame = 0;                    // ENTER GAME (n): the players in the game
		int32 Red = 0, Blue = 0;             // JOIN RED (n), JOIN BLUE (n)
		FString Level;                       // the co-op host page's LEVEL item
		FString Title;
		TArray<FString> State;               // at most two lines under the title
		FString Foot;                        // the host's address at the foot (the host only)
		TArray<FPlayer> Players;
		int32 Bots = 0;                      // "+ N bots" at the panel's foot (bots are not listed)
	};

	/** A text as last drawn (view units, 1920 x 1080): its box, its letters' cap height (the font's H as rasterised, times any
	 *  scale), and the page item it is (-1: a line, a heading or a name). */
	struct FInk
	{
		FString Text;
		FBox2D Box = FBox2D(ForceInit);
		float CapPx = 0.f;
		int32 Item = -1;
		bool bName = false;
	};

	/** The page as last drawn, for the probe and the lobby shot's fit checks. */
	struct FDrawn
	{
		FString File;
		TArray<FInk> Texts;
		FBox2D Panel = FBox2D(ForceInit);
		int32 Columns = 0;
		int32 Names = 0;
		float NameStep = 0.f;
	};
	FDrawn& Drawn();

	/** -OLOldLobby: the lobby and the co-op host page as before (one list of rows, the #coop drawing). */
	bool Old();

	/** The menus' smallest face seen in VR (MenuSmallFont, 24 pt on the pause sheet): nothing on the page smaller. */
	constexpr float FloorDeg = 1.28f;
	/** A cap height in view units seen in degrees at the middle of the wide sheet (1920 units over 128 cm at 96 cm). */
	float SeenDeg(float ViewPx);
	/** The view units a cap height of FloorDeg needs. */
	float FloorPx();

	/** The page's places (view units), shared by the drawing and the probe's checks: the art's guns and cloths are left and right
	 *  of these (Multiplayer_Base over the 2560 x 1440 stage, measured on the art). */
	constexpr float ArtLeft = 280.f;             // the pistol (to x 230) and the red cloth with its revolver (to x 270)
	constexpr float ArtRightTop = 1640.f;        // the rifle and the blue cloth down to y 960
	constexpr float ArtRightFoot = 1500.f;       // the blue cloth's tail under y 960
	constexpr float ArtFootY = 960.f;
	inline float ArtRightAt(float BottomY) { return BottomY > ArtFootY ? ArtRightFoot : ArtRightTop; }

#if WITH_OUTLAWS_TESTS
	/** -OLLobbyRedoProbe: a made-up page in place of the session's while bOn. */
	struct FTest
	{
		bool bOn = false;
		bool bHostHere = false;              // drawn as the host's (the foot line, the co-op host page)
		FView View;
	};
	FTest& Test();
#endif
}
