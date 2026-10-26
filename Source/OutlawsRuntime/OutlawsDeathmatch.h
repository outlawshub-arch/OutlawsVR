#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "OutlawsFragTable.h"
#include "OutlawsDeathmatch.generated.h"

class FOutlawsLevel;
class FOutlawsAssets;

/**
 * M1, the match side: Death Match's rules on their own, with no network in them.  The session (N1's) feeds it what happens
 * (players joining, shots, hits, kills, pickups taken) with the time, and does what it says (respawn a player there, put an
 * item back, end the match); the frag table reads it as a source.  Time is always passed in (seconds), so the rules can be
 * run fast in a test.
 */

/** The six multiplayer characters, the remaster's character page order (lobby_character.rml ol-char 1..6). */
struct OUTLAWSRUNTIME_API FOutlawsMpCharacter
{
	const TCHAR* Short;      // the art's name (Char_<Short>OFF.dds ...)
	const TCHAR* Name;       // LOCAL.MSG 5054..5059
	const TCHAR* Itm;        // the character's ITM (start kit, sounds, PLAYER_WAX)
	const TCHAR* Nwx;        // the sprite set (olobj.lab; the remaster's .spx beside it by name)
	const TCHAR* DieWav;     // SND_DIE
	float DamageMult;        // DAMAGE_MULTIPLIER (1 where the ITM has none)
	uint32 NailRGB;          // REQ-MARYNAILS: the VR hands' nail colour (0xRRGGBB; 0: the hands' own)
	uint8 StartPistols;      // REQ-SANCHEZDUAL: revolvers in the start kit at least (0: the ITM's count); two are drawn as a pair

	static const FOutlawsMpCharacter* All(int32& OutCount);
	/** By short name (San, Jam, Two, Doc, Bob, Mar), case ignored; nullptr when none. */
	static const FOutlawsMpCharacter* Find(const FString& Short);
	static int32 IndexOf(const FString& Short);

	/** The pause menu shows "Switch character" while this is set (the session sets it for a match; tests too). */
	static bool bMatchMenu;
	/** This machine's player's character (OutlawsSettings.ini MpCharacter; Jam when none). */
	static FString LocalChoice();
	/** Chosen on the character page: saved, and handed to the match (OnLocalChoice: SetPendingCharacter, the next spawn). */
	static void SetLocalChoice(const FString& Short);
	static TFunction<void(const FString&)> OnLocalChoice;
};

/**
 * BUG-CHARLOADOUT: what a character's ITM gives a player (cidmary.itm and the like, read from the player's files, so a mod's
 * character comes with its own): the start kit (START_ITEM_n / START_COUNT_n), the weapon drawn first (START_WEAPON, a weapon
 * ITM's SLOT), DAMAGE_MULTIPLIER, and the character's own copies of the weapons: the remaster ships one per character, named
 * by the letter after "cid" (cidsanch -> s: ipistol -> ispistol, BADGUN for a sound and 2.5 damage; cidmary -> m: impistol,
 * pistol3), the plain one where a character has none.
 */
struct OUTLAWSRUNTIME_API FOutlawsCharKit
{
	FString Itm;
	bool bRead = false;
	TArray<TPair<FString, int32>> Start;       // in the ITM's order; an item named twice counts both
	int32 StartSlot = -1;                      // START_WEAPON (-1: none given)
	float DamageMult = 1.f;
	TCHAR Variant = 0;                         // the weapons' letter (0: the plain ones)
	FString DieSound;                          // BUG-MPDEATHSOUND: SND_DIE without its .wav (szdie); the table's where the ITM has none

	/** The character's (by short name), read once per run; an empty kit (bRead false) when the files lack its ITM. */
	static const FOutlawsCharKit& Of(const FOutlawsAssets* A, const FString& Short);
	/** The character's own copy of an inventory weapon (ipistol -> ispistol) when the files have one, else Item. */
	FString WeaponVariant(const FOutlawsAssets* A, const FString& Item) const;
	/** BUG-MPDEATHSOUND: the character's death cry (its ITM's SND_DIE, the table's DieWav without one; jadie for none). */
	static FString DieSoundOf(const FOutlawsAssets* A, const FString& Short);
	/** The start kit, one line (the logs). */
	FString KitLine() const;
};

/** A spawn point: a STARTPOS object (PLAYER where a map has none), UE units. */
struct FOutlawsDmSpawn
{
	FVector Location = FVector::ZeroVector;
	float Yaw = 0.f;
};

/** A pickup generator (an N* object, FUNC Gen_New): what it makes, its period, whether it is out now. */
struct FOutlawsDmItemGen
{
	FString Object;          // the object's name (NRIFLE1)
	FString Generate;        // GENERATE (GRIFLE.ITM)
	float Period = 30.f;     // seconds before a taken one is back
	FVector Location = FVector::ZeroVector;
	bool bOut = true;        // on the ground to be taken
	double BackAt = 0.0;
};

struct FOutlawsDmPlayer
{
	int32 Id = -1;
	FString Name;
	FString Character = TEXT("Jam");
	FString PendingCharacter;         // chosen in the pause menu: worn from the next spawn
	bool bLocal = false;
	int32 Frags = 0;
	int32 Deaths = 0;
	int32 Suicides = 0;
	int32 Shots = 0;
	int32 Hits = 0;
	int32 LongestCm = 0;
	bool bAlive = false;
	double DiedAt = 0.0;
	double ProtectedUntil = 0.0;
	FVector Location = FVector::ZeroVector;
	int32 Ping = -1;
};

UENUM()
enum class EOutlawsDmPhase : uint8
{
	Warmup,       // play freely; nothing counts
	Countdown,    // "Starting in 10 ... 1"
	Live,
	Ended
};

struct FOutlawsDmConfig
{
	int32 FragLimit = 25;             // mp_killsToEnd (0: none)
	float TimeLimit = 600.f;          // mp_timeToEnd, seconds (0: none)
	float Countdown = 10.f;
	float RespawnDelay = 2.f;         // the least time dead before a respawn is granted
	float SpawnProtect = 3.f;         // a fresh spawn takes no hurt this long (ended early by its own shot)
	float EndShow = 10.f;             // the table at the end, then back to warm-up
};

class OUTLAWSRUNTIME_API FOutlawsDeathmatch : public IOutlawsFragTableSource
{
public:
	FOutlawsDmConfig Config;

	// what the rules ask of the session
	TFunction<void(const FOutlawsDmPlayer& P, const FOutlawsDmSpawn& At)> OnRespawn;
	TFunction<void(const FOutlawsDmItemGen& G, int32 Index)> OnItemBack;
	TFunction<void(EOutlawsDmPhase Phase, const FString& Why)> OnPhase;

	/** The level's spawn points and pickup generators, read from its objects (A: the generators' ITMs). */
	void ReadLevel(const FOutlawsLevel& L, const FOutlawsAssets* A);
	const TArray<FOutlawsDmSpawn>& GetSpawns() const { return Spawns; }
	TArray<FOutlawsDmItemGen>& GetItems() { return Items; }

	FOutlawsDmPlayer& AddPlayer(int32 Id, const FString& Name, const FString& Character, bool bLocal, double Now);
	void RemovePlayer(int32 Id);
	FOutlawsDmPlayer* Find(int32 Id);
	const TArray<FOutlawsDmPlayer>& GetPlayers() const { return Players; }

	/** The host's Start the game: the countdown, then everyone respawns, scores and items reset, the clock starts. */
	void StartCountdown(double Now);
	/** M1 wiring: the session's match flow (FOutlawsMatchFlow) owns the phases, the countdown and the limits; these rules
	 *  then only count, spawn and bring items back, and change phase when the flow says (GoLive / EndByFlow / WarmupByFlow). */
	bool bFlowDriven = false;
	void GoLive(double Now, const FString& Why);
	void EndByFlow(double Now, const FString& Why);
	void WarmupByFlow(double Now);
	/** The leader (most frags) and the frags, for the flow's score limit; -1 with nobody. */
	int32 TopFrags(FString& OutLeader) const;
	void Tick(double Now);
	EOutlawsDmPhase GetPhase() const { return Phase; }
	/** The message line: "Waiting for players (n/max)", "Starting in n", the time left. */
	FString MessageLine(double Now, int32 MaxPlayers) const;
	float TimeLeft(double Now) const;

	void NoteShot(int32 Shooter, double Now);
	/** A hit on Victim by Shooter: false when it does not count (the victim protected, dead, or no such player). */
	bool NoteHit(int32 Shooter, int32 Victim, double Now);
	/** Victim died; Killer the shooter (-1 or Victim: a suicide or the world). */
	void NoteKill(int32 Killer, int32 Victim, float DistCm, double Now);
	/** A dead player asks to come back: granted after RespawnDelay, at the spawn farthest from the living. */
	bool RequestRespawn(int32 Id, double Now);
	/** Choose the next character (applied on the next spawn). */
	void SetPendingCharacter(int32 Id, const FString& Short);
	/** A pickup taken from generator Index: it comes back after its period. */
	void NoteItemTaken(int32 Index, double Now);
	/** The spawn farthest from every living player (the original's rule); -1 when there are none. */
	int32 ChooseSpawn(int32 ForId) const;

	// IOutlawsFragTableSource
	virtual bool FillFragTable(FFragTableData& Out) const override;
	double TableNow = 0.0;           // (the time FillFragTable reads the clock at)

private:
	TArray<FOutlawsDmPlayer> Players;
	TArray<FOutlawsDmSpawn> Spawns;
	TArray<FOutlawsDmItemGen> Items;
	EOutlawsDmPhase Phase = EOutlawsDmPhase::Warmup;
	double PhaseAt = 0.0;
	double LiveAt = 0.0;
	void SetPhase(EOutlawsDmPhase P, double Now, const FString& Why);
	void SpawnPlayer(FOutlawsDmPlayer& P, double Now);
	void ResetScores();
	void CheckEnd(double Now);
};

/** -OLDmTest: the rules with fake players on the level (a multiplayer map: -OLLevel=drygulch), proved by log lines. */
UCLASS()
class OUTLAWSRUNTIME_API UOutlawsDeathmatchTest : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Conditional; }
	virtual bool IsTickable() const override { return !IsTemplate(); }
	virtual bool IsTickableInEditor() const override { return false; }
	virtual UWorld* GetTickableGameObjectWorld() const override;

private:
	int32 Step = 0;
	double At = 0.0;
	UPROPERTY(Transient)
	TObjectPtr<AOutlawsFragTablePanel> Panel;
};
