#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "OutlawsStats.generated.h"

class AOutlawsLevelActor;
class AOutlawsEnemy;
class AOutlawsMovieScreen;
class AOutlawsPlayer;

/**
 * One player's counts for the level being played, shown on a panel beside the cinema screen at the level's end (user
 * 2026-10-02: "player stats (one for each player on each side of the cinema screen) at the end of each level").  Each count is
 * kept on the machine where its event is decided: in co-op the host's for the bad guys' deaths (kills, dynamite kills,
 * the longest kill), each player's own machine for the rest.  Replicated on each player's co-op link (LevelStats).
 */
USTRUCT()
struct FOutlawsLevelStats
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Kills = 0;
	UPROPERTY()
	int32 Deaths = 0;
	UPROPERTY()
	int32 Shots = 0;
	/** Shots that struck a living bad guy (a shotgun's volley counts once). */
	UPROPERTY()
	int32 OnTarget = 0;
	UPROPERTY()
	int32 LongestKillCm = 0;
	UPROPERTY()
	int32 DynamiteKills = 0;
	UPROPERTY()
	int32 Thrown = 0;
	/** Co-op: the other player brought back with a medical bag. */
	UPROPERTY()
	int32 Revives = 0;
	UPROPERTY()
	int32 Seconds = 0;
	/** (REQ-STATS) Secret sectors (SECFLAG1_SECRET_TAG) entered, each once a level. */
	UPROPERTY()
	int32 Secrets = 0;
	/** Pickups taken (each pickup once, whatever it held). */
	UPROPERTY()
	int32 Items = 0;
	/** Health taken off living bad guys (no more than each had left); co-op: credited where kills are. */
	UPROPERTY()
	float DamageDealt = 0.f;
	/** Health this player lost (after the plate). */
	UPROPERTY()
	float DamageTaken = 0.f;
	/** Of the Deaths, those whose last hurt was a bad guy's shot (the rest: falls, water, fire, blasts). */
	UPROPERTY()
	int32 KilledByBadGuys = 0;
	/** Co-op, the client's own counts sent to the host: the start serial they belong to. */
	UPROPERTY()
	int32 Epoch = -1;

	bool operator==(const FOutlawsLevelStats& O) const
	{
		return Kills == O.Kills && Deaths == O.Deaths && Shots == O.Shots && OnTarget == O.OnTarget && LongestKillCm == O.LongestKillCm
			&& DynamiteKills == O.DynamiteKills && Thrown == O.Thrown && Revives == O.Revives && Seconds == O.Seconds && Epoch == O.Epoch
			&& Secrets == O.Secrets && Items == O.Items && DamageDealt == O.DamageDealt && DamageTaken == O.DamageTaken && KilledByBadGuys == O.KilledByBadGuys;
	}
	bool operator!=(const FOutlawsLevelStats& O) const { return !(*this == O); }
	FString Describe() const;
};

/** A panel's words: the player's name, the level, and its rows (a label and a value). */
struct FOutlawsStatsPanel
{
	FString Name;
	FString Sub;
	TArray<TPair<FString, FString>> Rows;
	bool operator==(const FOutlawsStatsPanel& O) const { return Name == O.Name && Sub == O.Sub && Rows == O.Rows; }
	bool operator!=(const FOutlawsStatsPanel& O) const { return !(*this == O); }
};

/** (REQ-HANDSCORE) The card each player holds in the hand through the level's cutscene (VR): single player, the player's own
 *  rows; co-op, a table with a row for each player, the holder's first. */
struct FOutlawsStatsCard
{
	FString Title;
	FString Sub;
	bool bTable = false;
	TArray<TPair<FString, FString>> Rows;      // (single player) a label and a value
	TArray<FString> Cols;                      // (co-op) the table's heads, the first the players'
	TArray<TArray<FString>> Cells;             // (co-op) a row for each player, the holder's first
	bool operator==(const FOutlawsStatsCard& O) const { return Title == O.Title && Sub == O.Sub && bTable == O.bTable && Rows == O.Rows && Cols == O.Cols && Cells == O.Cells; }
	bool operator!=(const FOutlawsStatsCard& O) const { return !(*this == O); }
	/** The card's lines, one string (the logs). */
	FString Lines() const;
};

/** Who struck a bad guy last (AOutlawsEnemy::StatsBy): his death is that player's kill.  (N1: Other is the client of two;
 *  with more players each client k is 1 + k, StatsByPlayer.) */
enum class EOutlawsStatsBy : uint8 { Nobody = 0, Me = 1, Other = 2 };
inline EOutlawsStatsBy StatsByPlayer(int32 Who) { return EOutlawsStatsBy(uint8(1 + FMath::Clamp(Who, 1, 15))); }
inline int32 StatsPlayerOf(EOutlawsStatsBy By) { return uint8(By) >= 2 ? int32(uint8(By)) - 1 : -1; }

/** While one lives, the bad guys struck are struck by By (the host dealing a hit the client reported: Other; a bad guy's own
 *  shot: Nobody); bBlast: by a blast (a dynamite kill); bDirect false: no kill distance (a fall, a thrown man). */
struct FOutlawsStatsScope
{
	FOutlawsStatsScope(EOutlawsStatsBy By, bool bBlast = false, bool bDirect = true);
	~FOutlawsStatsScope();
	EOutlawsStatsBy SavedBy;
	bool bSavedBlast;
	bool bSavedDirect;
};

/**
 * The level's counts (user 2026-10-02).  They start at each new level (a new game, the next level, a mission, a co-op start,
 * a level skip); a restart or a load of the level being played keeps counting (so the deaths count, and the kills of every
 * try); nothing is written into saves (Resume Game after the game was closed counts from the load).
 */
UCLASS()
class OUTLAWSRUNTIME_API UOutlawsStats : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	static UOutlawsStats* Get(const UObject* Context);

	// ---- the events, each a line where it is decided ----
	/** The level is being built (AOutlawsLevelActor::Build): a new level starts the counts, the same one keeps them. */
	static void OnLevelBuild(AOutlawsLevelActor* L);
	/** This machine's player fired a gun (before its pellets fly: their hits this frame are this shot's). */
	static void NoteShotFired(AOutlawsLevelActor* L);
	/** A bad guy is struck (AOutlawsEnemy::TakeHit, first thing): the striker is the scope's (this machine's player unless
	 *  a scope says otherwise). */
	static void NoteHit(AOutlawsEnemy* E, const FVector& From);
	/** A bad guy dies where deaths are decided (AOutlawsEnemy::Die): the kill of whoever struck him last. */
	static void NoteKill(AOutlawsEnemy* E);
	/** A bad guy let go of a hand (AOutlawsEnemy::EndHeld): thrown hard, a throw of the holder's. */
	static void NoteRelease(AOutlawsEnemy* E, const FVector& Vel);
	/** This player died; bByBadGuy: the last hurt was a bad guy's shot. */
	static void NoteDeath(AOutlawsLevelActor* L, bool bByBadGuy = false);
	/** (REQ-STATS) A bad guy lost Dealt health (AOutlawsEnemy::TakeHit, where hits are dealt): the striker's. */
	static void NoteDamage(AOutlawsEnemy* E, float Dealt);
	/** This machine's player lost Dealt health. */
	static void NoteHurt(AOutlawsLevelActor* L, float Dealt);
	/** This machine's player took a pickup. */
	static void NoteItem(AOutlawsLevelActor* L);
	/** This machine's player entered a secret sector (counted once a level). */
	static void NoteSecret(AOutlawsLevelActor* L, int32 Sector);
	static void NoteRevive(AOutlawsLevelActor* L);
	// ---- REQ-LEVELTIMER: the level's time and best times ----
	/** The level is won (CompleteLevel, on every machine of a session): its time is settled against the best times the first time
	 *  the end card is made (Panels). */
	static void NoteLevelEnd(AOutlawsLevelActor* L);
	/** A Cheats page row (Name) ran on this machine, the toggles standing as given before the press: the level's best time is not kept. */
	static void NoteCheat(AOutlawsLevelActor* L, const FString& Name, bool bGodWas, bool bAmmoWas, bool bEnemiesActiveWas);
	/** The level is being skipped to (the cheat's Skip to level): the level that follows keeps no best time. */
	static void NoteJump(AOutlawsLevelActor* L);
	/** A save was loaded: the clock goes on from the saved seconds (Seconds < 0: a save from before the clock, no best kept). */
	static void NoteLoaded(AOutlawsLevelActor* L, float Seconds);
	/** The level's time so far, whole seconds (a session: the host's clock); false when no level is being timed. */
	static bool LevelSeconds(const UObject* Context, int32& OutSeconds);
	/** The pause menu's line, "Time 12:34" (empty: none to show). */
	static FString PauseLine(const UObject* Context);
	/** The time as the card shows it: 12:34, or 1:02:03 from an hour. */
	static FString Clock(int32 Seconds);
	/** The level's save block: the seconds so far. */
	static float SavedSeconds(const UObject* Context);
	/** This machine's player's end-card lines about the time: the best time and the new best (or why none was kept). */
	static void TimeRows(const UObject* Context, TArray<TPair<FString, FString>>& Out);
	/** This machine's player's end-card summary, e.g. "12:34, New best!" (the co-op card's subtitle). */
	static FString TimeSummary(const UObject* Context);
	/** -OLTimerTest=<finish|pause|saveload|cheat> (the REQ-LEVELTIMER checks) is part of Tick. */

	/** This machine's dynamite kills so far (the tests'). */
	static int32 MyDynamiteKills(const UObject* Context) { const UOutlawsStats* S = Get(Context); return S ? S->Mine.DynamiteKills : 0; }

	/** The panels: one in single player, the host's then the client's in co-op (the same on both machines). */
	static bool Panels(const UObject* Context, TArray<FOutlawsStatsPanel>& Out, bool& bTwo);
	/** (REQ-HANDSCORE) This machine's player's hand card (false: none to show). */
	static bool HandCard(const UObject* Context, FOutlawsStatsCard& Out);
	/** The level's cutscene has started: its screen shows the panels. */
	static void ShowOn(AOutlawsLevelActor* L, AOutlawsMovieScreen* Screen);
	/** No cutscene to play: the panels alone on the black for a few seconds, then the next level (false: none to show). */
	static bool HoldBetweenLevels(AOutlawsLevelActor* L);
	/** REQ-PERFCARD: a performance test quit from the pause menu: its results card alone on the black for a while, then the main
	 *  menu (false: no card to show). */
	static bool HoldPerfCard(AOutlawsLevelActor* L);

	// co-op (the links hand these over)
	void OnClientStats(int32 Who, const FOutlawsLevelStats& In);

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Conditional; }
	virtual bool IsTickable() const override { return !IsTemplate(); }
	virtual bool IsTickableInEditor() const override { return false; }
	virtual UWorld* GetTickableGameObjectWorld() const override;

private:
	/** This machine's player's counts. */
	FOutlawsLevelStats Mine;
	/** Host: each client's counts decided here (its kills); and its own counts as it last sent them (N1: by player index). */
	FOutlawsLevelStats OtherHere[16];          // (M1: up to 16 players)
	FOutlawsLevelStats OtherOwn[16];
	FString Level;                     // the level these are for (empty: none)
	TSet<int32> SecretsSeen;           // the secret sectors counted this level
	float SecondsF = 0.f;
	// REQ-LEVELTIMER
	bool bCheated = false;             // a cheat row ran, or a cheat toggle was on, or the level was skipped to: no best time kept
	bool bSkipped = false;             // (of those, the skip)
	bool bJumpMark = false;            // a skip is on its way: the level built next is the skipped-to one
	bool bToggleGod = false, bToggleAmmo = false, bToggleFreeze = false;      // the Cheats page's toggles as they stand (they outlive a level)
	bool bTimeUnknown = false;         // loaded from a save that has no clock: no best kept
	int32 EasiestDiff = 99;            // the easiest difficulty the level was played on (it can be changed in play)
	bool bFinished = false;            // the level was won; the result below is made once
	bool bResolved = false;
	FString ResultKey;                 // the best-time key of the finish
	int32 ResultDiff = 0;
	int32 ResultBest = -1;             // the best time on this profile after the finish (-1: none kept)
	int32 ResultPrior = -1;
	bool bResultNew = false;
	FString ResultWhyNot;              // why this finish keeps no best time (empty: it counts)
	bool bResultCounts = false;        // the finish is one that has a best time (not a match, not the Historical Missions' office)
	void ResolveFinish();
	FString TimeKeyFor(const AOutlawsLevelActor* L) const;
	uint64 ShotFrame = 0;             // the frame of this player's last shot, and the shot a hit was last counted for
	int32 CountedShot = -1;
	FOutlawsLevelStats Sent;           // client: as last sent
	double SentAt = 0.0;
	TWeakObjectPtr<AOutlawsLevelActor> LevelRef;

	AOutlawsLevelActor* FindLevel() const;
	void TickCoop(double Now);
	FOutlawsLevelStats& CountsFor(EOutlawsStatsBy By, bool& bOk);
	void ResetCounts();
	static FOutlawsStatsPanel MakePanel(const FString& Name, const FString& LevelTitle, const FOutlawsLevelStats& S, bool bCoop, int32 ShownSeconds = -1, const TArray<TPair<FString, FString>>* Extra = nullptr);

	// -OLStatsTest (single player) and -OLStatsCoopTest (both copies of a co-op pair)
	int32 TestStep = 0;
	double TestAt = 0.0;
	double TestShownAt = 0.0;
	bool bTestHost = false;
	void TickTest(double Now);
	void TickCoopTest(double Now);
	// -OLTimerTest (OutlawsLevelTimerTest.cpp)
	void TickTimerTest(double Now);
	int32 TimerStep = 0;
	double TimerAt = 0.0;
	double TimerMark = 0.0;
	float TimerT0 = 0.f;
	int32 TimerPasses = 0, TimerFails = 0;
	void TimerCheck(bool bOk, const FString& What);
	int32 LoadsApplied = 0;            // saves loaded so far (NoteLoaded)
	int32 LoadsAtTest = 0;
	AOutlawsPlayer* LocalPlayer() const;
};
