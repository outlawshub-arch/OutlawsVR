#pragma once

// M1, the host's side (tools/olue/PLANS.md "M1. Skeleton", M4 decided points 3 and 4): the options a host picks on the Host Game
// page (the remaster's own multiplayer_host_start.rml: Start, Max Players, Locations, Games, Minutes, Kills / Captures,
// Friendly Fire, Allow Cheats; ours: Start when N have joined), the maps to pick from (the stock multiplayer maps' RCMs in the
// LABs, and every custom map in the game folder: a .rcm beside its .lab), and the match flow (warm-up, the countdown, the
// match, its end, warm-up again) with its messages.  Data and UI only: the session (OutlawsCoop*) is told through
// OutlawsMpHost::OnHost and FOutlawsMatchFlow's callbacks, stubs until the session is wired.

#include "CoreMinimal.h"

class FOutlawsAssets;

/** The original's game classes (the remaster's strings $m_local_5060 to 5065, the RCM MODES letters). */
enum class EOutlawsGameClass : uint8
{
	CaptureTheFlag = 1,       // c
	Tag = 2,                  // t
	Chicken = 3,              // k (Kill The Fool With The Chicken)
	SecretDocument = 4,       // s
	TeamPlay = 5,             // m
	DeathMatch = 6,           // d
};

/** One multiplayer map: its RCM (stock in the LABs, or a custom one in the game folder beside its LAB). */
struct OUTLAWSRUNTIME_API FOutlawsMpMap
{
	FString File;             // the RCM's file name, lower case (fort.rcm, bourbon.rcm)
	FString Name;             // its NAME: line (Fort de Chiens)
	FString Level;            // the STORY's LEVEL (fort)
	FString Lab;              // a custom map's LAB (full path); empty for a stock map
	uint8 Modes = 0;          // bit (class - 1) per allowed game class
	bool bCustom = false;
	/** (M2, guess) no stock map lists Secret Document's letter, though most have its DOCSTART places: it is offered on every map
	 *  (the document falls back to the chicken's or a flag place on a map without them). */
	bool Allows(EOutlawsGameClass C) const { return C == EOutlawsGameClass::SecretDocument || (Modes & (1u << (uint8(C) - 1))) != 0; }
	FString ModesText() const;
};

/** What the host chose; saved in OutlawsSettings.ini (MpHost*) and handed to the session at Start. */
struct OUTLAWSRUNTIME_API FOutlawsMpHostOptions
{
	FString MapFile;                          // an FOutlawsMpMap's File
	EOutlawsGameClass GameClass = EOutlawsGameClass::DeathMatch;
	int32 TimeLimitMinutes = 10;              // 0: no time limit
	int32 ScoreLimit = 20;                    // kills (captures in Capture The Flag); 0: none
	int32 MaxPlayers = 8;                     // 2 to MaxPlayersCap()
	int32 StartWhen = 0;                      // 0: the host starts it ("Start the game"); else when this many have joined
	bool bFriendlyFire = false;
	bool bAllowCheats = false;
	/** M1 (user 2026-10-03: "make passwords optional"): empty, anyone may join; else a joiner must give it. */
	FString Password;
	/** M2: Secret Document played red against blue (each team's base its capture point); else free-for-all. */
	bool bTeams = false;
	/** REQ-MPBOTS: server-side players driven by the bad guys' AI (Death Match and Team Play): on or off, how many (1 to
	 *  MaxPlayers - 1; in play fewer when humans need the slots), and their skill (0 easy, 1 normal, 2 hard). */
	bool bBots = false;
	int32 BotCount = 3;
	int32 BotSkill = 1;
	/** REQ-MPLOBBY (b), decision 4: "Random each match" for the map and for the game, rolled again for every match
	 *  (OutlawsMpHost::RollMatch); MapFile and GameClass then hold the last roll. */
	bool bRandomMap = false;
	bool bRandomClass = false;

	FString Describe() const;
	void Load();                              // from OutlawsSettings.ini
	void Save() const;                        // into it (merged with the file's other keys)
	/** Kept in range: the cap, StartWhen within 2..MaxPlayers (or 0), a class the map allows. */
	void Clamp(const FOutlawsMpMap* Map);
};

namespace OutlawsMpHost
{
	/** The value steps the page cycles through (the RML's ranges: minutes 0..120, kills 0..100). */
	OUTLAWSRUNTIME_API const TArray<int32>& TimeSteps();
	OUTLAWSRUNTIME_API const TArray<int32>& ScoreSteps();
	/** 16 on a PC host, 8 on a Quest host until measured (M4 point 3); -OLMpQuestCap tries the Quest's on a desktop. */
	OUTLAWSRUNTIME_API int32 MaxPlayersCap();
	OUTLAWSRUNTIME_API FString ClassLabel(EOutlawsGameClass C);         // "Death Match" (the remaster's string)
	/** REQ-MPBOTS: the classes bots play (Death Match, Team Play); the Host Game page hides the Bots rows for the others. */
	OUTLAWSRUNTIME_API bool BotsAllowed(EOutlawsGameClass C);
	OUTLAWSRUNTIME_API const TCHAR* BotSkillLabel(int32 Skill);
	/** M2: the game classes this build offers (each switched on as it passes its test; one not ready ships off: hidden from
	 *  the Host Game page, hosted as Death Match).  -OLMpAllModes turns every one on (tests). */
	OUTLAWSRUNTIME_API bool IsClassOn(EOutlawsGameClass C);
	/** An RCM's text: NAME, MODES, the STORY's LEVEL.  False (with why) when it has no level. */
	OUTLAWSRUNTIME_API bool ParseRcm(const FString& Text, FOutlawsMpMap& Out, FString& Why);
	/** The stock maps (every .rcm the LABs hold) then the custom ones (Root\NAME.rcm beside a NAME.lab); a map
	 *  whose RCM does not read is logged and left out, never fatal. */
	OUTLAWSRUNTIME_API void ListMaps(const FOutlawsAssets* Assets, TArray<FOutlawsMpMap>& Out);
	/** The page's Start: the stub until the session takes it (logs the options; M1's wiring sets it). */
	OUTLAWSRUNTIME_API TFunction<bool(const FOutlawsMpHostOptions&)>& OnHost();
	OUTLAWSRUNTIME_API bool Host(const FOutlawsMpHostOptions& Options);
	/** A map's English name for the lines every machine shows (the stock maps' names as the remaster says them; a custom
	 *  map by its file). */
	OUTLAWSRUNTIME_API FString MapTitle(const FString& File);
	/** REQ-MPLOBBY (b), decision 4: the map and the game of the next match when either is "Random each match" (the flags stay
	 *  set; MapFile and GameClass take the roll).  A random map comes from the stock maps that allow the game (a custom map is
	 *  never rolled: a joiner may not have it); a random game from the games the map allows and this build offers; both
	 *  random, a valid pair.  Another map (or game) than LastMap / LastClass whenever there is a choice.  False, Out unchanged,
	 *  when nothing fits.  Said: the roll in words for the log. */
	OUTLAWSRUNTIME_API bool RollMatch(const TArray<FOutlawsMpMap>& Maps, FOutlawsMpHostOptions& Out, const FString& LastMap, EOutlawsGameClass LastClass, FString& Said);
}

class FOutlawsMatchFlow;
namespace OutlawsMpHost
{
	/** The host's match flow (the session ticks it once wired; the pause menu's "Start the game" asks it). */
	OUTLAWSRUNTIME_API FOutlawsMatchFlow& Flow();
}

/** The match flow on the host: warm-up (play, kills do not count) until the host starts it or StartWhen have joined, a
 *  10 s countdown, the match (the clock runs; the time or the score limit ends it), its end (the scoreboard), then warm-up
 *  again.  Ticked by its owner with real seconds; it says what to show (OnMessage, the pickup line on every machine) and
 *  when to act (OnMatchStart: respawn everyone at spawn points, reset scores and items, start the clock; OnMatchEnd). */
class OUTLAWSRUNTIME_API FOutlawsMatchFlow
{
public:
	enum class EState : uint8 { Idle, WarmUp, Countdown, Playing, Ended };
	static const TCHAR* StateName(EState S);

	TFunction<void(const FString& Text, float Seconds)> OnMessage;
	TFunction<void()> OnMatchStart;
	TFunction<void(const FString& Why)> OnMatchEnd;

	static constexpr int32 CountdownSeconds = 10;
	static constexpr float EndedSeconds = 15.f;          // the scoreboard up, then warm-up again
	static constexpr float WaitingEvery = 5.f;           // the warm-up line repeated while nothing changes

	void Begin(const FOutlawsMpHostOptions& InOptions);  // hosting: warm-up with the host alone
	void Stop();
	void SetPlayers(int32 N);                            // the session's count (the host included)
	/** The people in the match (the host included, bots not): StartWhen counts these once set (bots would start it at once). */
	void SetHumans(int32 N);
	/** The pause menu's "Start the game" (the host): the countdown from warm-up.  False when not in warm-up or alone. */
	bool HostStart();
	/** A kill in the match (warm-up kills do not count): the top score and who holds it, for the score limit. */
	void ReportScore(int32 TopScore, const FString& Leader);
	/** M2: the class's own rules decided it (a capture limit, a team's kills, the time held): the match ends, Winner wins. */
	void EndNow(const FString& Winner, const FString& Why);
	void Tick(float Dt);

	EState GetState() const { return State; }
	bool CountsKills() const { return State == EState::Playing; }
	int32 GetPlayers() const { return Players; }
	float TimeLeft() const;                              // -1 with no time limit
	const FOutlawsMpHostOptions& GetOptions() const { return Options; }

private:
	void Enter(EState S, const FString& Why);
	void Say(const FString& Text, float Seconds = 3.f);
	void SayWaiting();
	FOutlawsMpHostOptions Options;
	EState State = EState::Idle;
	int32 Players = 1;
	int32 Humans = -1;                                   // -1: not told, StartWhen counts Players
	int32 StartCount() const { return Humans >= 0 ? Humans : Players; }
	float StateTime = 0.f;
	float Clock = 0.f;
	float WaitingTimer = 0.f;
	int32 LastCount = -1;
	bool bAutoCountdown = false;
	bool bMinuteWarned = false;
public:
	/** REQ-MPLOBBY: the session's lobby rule (FOutlawsLobbyStart) starts the match, so warm-up never counts down by itself at
	 *  StartWhen (a launched host's -OLMpStartWhen is the rule's minimum instead). */
	bool bLobbyGate = false;
	/** REQ-MPLOBBY (b): the session's next-match cycle owns what follows the end (the score screen, the host's next match), so
	 *  Ended holds until the session stops the flow; off, warm-up again after EndedSeconds as before. */
	bool bHoldEnd = false;
};

/** REQ-MPLOBBY (landing a, the user 12 Oct): the lobby's start rule on the host, data only (the session feeds it; -OLLobbyRuleTest
 *  runs it on simulated time).  A 60 s countdown starts when the first player enters; time left = 60 x (players still in the
 *  lobby / all players), so it drops as each one enters; everyone in starts it now.  A lone host gets the full 60 s, then
 *  starts with bots when bots are on, else waits for a second real player (from then the shrinking rule applies).  Bots never
 *  count.  A launched host's -OLMpStartWhen stays a minimum on top. */
struct OUTLAWSRUNTIME_API FOutlawsLobbyStart
{
	static constexpr float FullSeconds = 60.f;
	float Left = -1.f;               // -1: nobody has entered yet
	bool bLoneWait = false;          // the lone host's 60 s ran out with bots off: waiting for a second real player
	bool bHeld = false;              // the rule says start, but it cannot yet (alone without bots, or below the start count)
	bool bDone = false;              // fired (or the host started it): the rule is over for this match
	FString Why;
	void Reset() { *this = FOutlawsLobbyStart(); }
	/** Humans: the real players in the session, the host included.  InLobby: those of them who have not entered.  bBots: bots
	 *  are on and in the match.  People: the real players in the match's rules (entered and built).  StartWhen: a launched
	 *  host's minimum (0: none).  True once, when the match should start now (Why says why). */
	bool Tick(float Dt, int32 Humans, int32 InLobby, bool bBots, int32 People, int32 StartWhen);
};
