#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "Engine/EngineBaseTypes.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Info.h"
#include "OutlawsStats.h"
#include "OutlawsMpHost.h"
#include "OutlawsDeathmatch.h"
#include "OutlawsMatchModes.h"
#include "OutlawsKillFeed.h"
#include "OutlawsCoop.generated.h"

class AOutlawsLevelActor;
class AOutlawsPlayer;
class AOutlawsEnemy;
class AOutlawsCoopLink;
class AOutlawsCoopSession;
class AOutlawsRemotePlayer;
class AOutlawsSpriteActor;
class UNetDriver;
class FJsonObject;
struct FOutlawsElevator;
namespace OutlawsMenu { struct FPage; struct FItem; }

/**
 * Story co-op (tools/olue/COOP_DESIGN.md).  Two players, direct IP: the host runs the one map (L_Runtime) as a listen server
 * with AOutlawsCoopGameMode, the client travels to <ip>:7777.  Each machine builds every Outlaws level from its own LABs,
 * exactly as single player does; only the session's small state (the lobby, the level to start, later the poses and events)
 * crosses the network, through AOutlawsCoopSession (session-wide) and one AOutlawsCoopLink per player.
 *
 * Single player never sees any of it: Mode stays None, and every co-op branch in the game's code asks IsActive() first.
 */
UENUM()
enum class EOutlawsCoopMode : uint8
{
	None,
	Host,
	Client
};

/** One player's body, head and hands as last sent (about 20 a second, unreliable), and what each hand holds. */
USTRUCT()
struct FOutlawsCoopPose
{
	GENERATED_BODY()

	UPROPERTY()
	FVector_NetQuantize10 Body = FVector::ZeroVector;       // the capsule's centre
	UPROPERTY()
	float BodyYaw = 0.f;
	UPROPERTY()
	float HalfHeight = 88.f;                                // the capsule's half height (the VR body follows the head)
	UPROPERTY()
	FVector_NetQuantize10 Head = FVector::ZeroVector;       // the camera
	UPROPERTY()
	FRotator HeadRot = FRotator::ZeroRotator;
	UPROPERTY()
	FVector_NetQuantize10 LeftHand = FVector::ZeroVector;
	UPROPERTY()
	FRotator LeftRot = FRotator::ZeroRotator;
	UPROPERTY()
	FVector_NetQuantize10 RightHand = FVector::ZeroVector;
	UPROPERTY()
	FRotator RightRot = FRotator::ZeroRotator;
	/** The weapon index in each hand (-1 bare), and a tool or gear item held there (empty: none). */
	UPROPERTY()
	int32 LeftWeapon = -1;
	UPROPERTY()
	int32 RightWeapon = -1;
	UPROPERTY()
	FString LeftTool;
	UPROPERTY()
	FString RightTool;
	UPROPERTY()
	bool bVR = false;
	/** BUG-LANTERNNET (user 2026-10-10, RC2: a partner's lit lantern lit nothing on the other machine): this player's lantern,
	 *  0 put away (in neither hand, not lit), 1 in a hand but out, 2 lit (shining on this player's own machine: held with
	 *  oil in VR, the swim's put-away putting it out); the light's place as this machine has it (the held flame, else the
	 *  chest) and the oil left (0..255 of a full 150): the other machines light their copy of it as this one does. */
	UPROPERTY()
	uint8 Lamp = 0;
	UPROPERTY()
	uint8 LampOil = 0;
	UPROPERTY()
	FVector_NetQuantize10 LampAt = FVector::ZeroVector;
	/** REQ-FLICKRELOAD: this player's double guns broken open (bit 0 the left hand, bit 1 the right).  For a client the host's
	 *  word: what it accepted from that client's ServerBreakFx, never the bits the client's own pose carries. */
	UPROPERTY()
	uint8 Break = 0;
	/** REQ-CHESTPLATE: the boiler plate this player wears, as the other machines draw it on them: its size (the plate picked
	 *  up, 50 / 100 / 150; 0 none worn) and the yaw it faces (its wearer's body, in VR held within 35 degrees of the head). */
	UPROPERTY()
	int16 PlateSize = 0;
	UPROPERTY()
	float ChestYaw = 0.f;
	/** The body's speed across the ground (cm/s) as its own movement has it: the other machine's sprite walks or runs by it
	 *  (user 2026-10-02; the eased place lags and overshoots, and an eased teleport is no walk). */
	UPROPERTY()
	float GroundSpeed = 0.f;
	/** Milestone 6 item 4: the bad guy each hand holds (its index in the level's list; -1 none) and where this machine has
	 *  him (his capsule's centre), so the host can put its copy there. */
	UPROPERTY()
	int32 LeftHeld = -1;
	UPROPERTY()
	int32 RightHeld = -1;
	UPROPERTY()
	FVector_NetQuantize10 LeftHeldAt = FVector::ZeroVector;
	UPROPERTY()
	FVector_NetQuantize10 RightHeldAt = FVector::ZeroVector;
	/** R8: when the sender sent it (its own seconds since its game started): the other machine draws the puppet a little
	 *  behind the newest pose, between two it has, so a late or lost packet on a real internet link does not show. */
	UPROPERTY()
	float SentAt = 0.f;
	/** Counts up with every send (0: never sent). */
	UPROPERTY()
	int32 Seq = 0;
};

/** One player's health, armour and death as that player's own machine has them (milestone 3), for the other to read. */
USTRUCT()
struct FOutlawsCoopStats
{
	GENERATED_BODY()

	UPROPERTY()
	float Health = 10.f;
	/** Full health (the bar over this player's head on the other machine: Health / MaxHealth; user 2026-10-02). */
	UPROPERTY()
	float MaxHealth = 10.f;
	UPROPERTY()
	int32 Armour = 0;
	UPROPERTY()
	bool bDead = false;
	/** 2026-10-02: this player's pause menu is open (the game goes on for the other; the bad guys leave this one be). */
	UPROPERTY()
	bool bInMenu = false;
	/** BUG-STUCKGAP (user 2026-10-05: keys are never lost in co-op): the keys and puzzle pieces this player carries (one
	 *  name per piece), and its last safe floor spot (bSafe: one is known): a player who leaves has them put back there. */
	UPROPERTY()
	TArray<FString> Keys;
	UPROPERTY()
	FVector_NetQuantize10 SafeFeet = FVector::ZeroVector;
	UPROPERTY()
	bool bSafe = false;
	/** BUG-MPDEATHSOUND: dead, the character's death cry (its ITM's SND_DIE: szdie, bmdie...), heard from its body on the
	 *  other machines; empty while alive. */
	UPROPERTY()
	FString DieSound;
	/** REQ-KILLFEED: dead, what killed this player when nobody else did (EOutlawsHurtCause: a fall, its own dynamite...): the
	 *  host's kill line for it. */
	UPROPERTY()
	uint8 DeathBy = 0;
	/** Counts up with every change sent (0: never sent). */
	UPROPERTY()
	int32 Seq = 0;
};

/** 2026-10-02 (the user's request: more bad guys for each player beyond the first): one extra bad guy, standing beside the
 *  level's own bad guy at OBT row Row, at Feet (UE cm, the floor under it) and facing Yaw (65536 to the turn).  Since the
 *  user's later word the same day he is not a copy: Kind is his own kind (an ordinary bad guy's ITM, "bgy3") and Sprite its
 *  NWX, drawn by the host.  The host's list rides on the session with the level start; both machines make them in its
 *  order, after all the level's own bad guys. */
USTRUCT()
struct FOutlawsCoopExtra
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Row = -1;
	UPROPERTY()
	FVector_NetQuantize10 Feet = FVector::ZeroVector;
	UPROPERTY()
	uint16 Yaw = 0;
	UPROPERTY()
	FString Kind;
	UPROPERTY()
	FString Sprite;
};

/** One enemy as the host has it (milestone 4), by its index in the level's list: sent about 15 times a second. */
USTRUCT()
struct FOutlawsCoopEnemySnap
{
	GENERATED_BODY()

	UPROPERTY()
	FVector_NetQuantize Pos = FVector::ZeroVector;      // the capsule's centre
	UPROPERTY()
	uint16 Yaw = 0;                                     // its heading, 65536 to the turn
	UPROPERTY()
	uint8 State = 0;                                    // EOutlawsEnemyState
	UPROPERTY()
	uint8 Sub = 0;                                      // the combat hook's sub-state (shooting shows as such)
	UPROPERTY()
	int16 Chor = -1;                                    // the choreography on show
	UPROPERTY()
	uint8 Plays = 0;                                    // counts each choreography start (the same one again: a new burst)
	UPROPERTY()
	uint8 Shots = 0;                                    // counts its shots (the client plays the gun's sound and flash)
	UPROPERTY()
	uint8 Flags = 0;                                    // 1 the chor loops, 2 dormant, 4 held or thrown, 8 after the client,
	                                                    // 16 its place sent every 2nd snapshot, 32 every 4th (CPU2 d)
	UPROPERTY()
	float Health = 0.f;
};

/** One INF elevator (a door, a lift, a moving floor) as the host has it (milestone 5), by its index in the level's list. */
USTRUCT()
struct FOutlawsCoopInfSnap
{
	GENERATED_BODY()

	UPROPERTY()
	float Value = 0.f;                                  // where it is (a height, a distance or an angle)
	UPROPERTY()
	int16 Stop = 0;                                     // the stop it last reached
	UPROPERTY()
	int16 Target = -1;                                  // the stop it moves to (-1: still)
	UPROPERTY()
	uint8 Flags = 0;                                    // 1 waiting at its stop, 2 finished for good, 4 switched on

	bool operator==(const FOutlawsCoopInfSnap& O) const { return Value == O.Value && Stop == O.Stop && Target == O.Target && Flags == O.Flags; }
	bool operator!=(const FOutlawsCoopInfSnap& O) const { return !(*this == O); }
};

/** How the level of a start serial begins (milestone 5; AOutlawsCoopSession::StartKind). */
enum class EOutlawsCoopStart : uint8
{
	Lobby,                // the lobby's Start: the level from its start kit (StartMission)
	NextLevel,            // the story goes on: each player keeps what they carry (StartNextLevel's carry)
	Restart,              // the same level again: each player with what they entered it with (RestartLevel)
	Jump,                 // the cheat's level skip: each player keeps what they carry, as NextLevel (BUG-SKIPLEVELINV)
	Load,                 // a save loaded for both (milestone 8): the host puts it back, the client takes its world as a snapshot
};

/** The client's player as the host's enemies see it (milestone 4): from its pose and its stats, each frame. */
struct FOutlawsCoopRemoteView
{
	bool bValid = false;
	bool bDead = false;
	bool bAway = false;                     // (2026-10-02) in its pause menu: the bad guys leave it be
	FVector Eye = FVector::ZeroVector;
	FVector Feet = FVector::ZeroVector;
	float Radius = 34.f;
	float Half = 87.f;
};

/** M1: one player's line in a match's scores, as the host's rules have it (the frag table on every machine reads these). */
USTRUCT()
struct FOutlawsMatchScore
{
	GENERATED_BODY()

	UPROPERTY()
	int32 Who = -1;                                     // the player's number
	UPROPERTY()
	FString Name;
	UPROPERTY()
	FString Character;                                  // San, Jam, Two, Doc, Bob, Mar
	UPROPERTY()
	int16 Frags = 0;
	UPROPERTY()
	int16 Deaths = 0;
	UPROPERTY()
	int16 Shots = 0;
	UPROPERTY()
	int16 Hits = 0;
	UPROPERTY()
	int32 LongestCm = 0;
	UPROPERTY()
	bool bAlive = false;
	/** M2: the player's team (1 red, 2 blue, 0 none), captures (CTF, Secret Document) and seconds held (Chicken, Tag). */
	UPROPERTY()
	uint8 Team = 0;
	UPROPERTY()
	int16 Captures = 0;
	UPROPERTY()
	int16 HeldSeconds = 0;

	bool operator==(const FOutlawsMatchScore& O) const
	{
		return Who == O.Who && Name == O.Name && Character == O.Character && Frags == O.Frags && Deaths == O.Deaths && Shots == O.Shots && Hits == O.Hits
			&& LongestCm == O.LongestCm && bAlive == O.bAlive && Team == O.Team && Captures == O.Captures && HeldSeconds == O.HeldSeconds;
	}
};

/** M2: a mode item (a flag, the chicken, the document) as the host's rules have it, for every machine to draw. */
USTRUCT()
struct FOutlawsModeItemRep
{
	GENERATED_BODY()

	UPROPERTY()
	uint8 Kind = 0;                                     // EOutlawsModeItemKind
	UPROPERTY()
	uint8 State = 0;                                    // EOutlawsModeItemState
	UPROPERTY()
	int8 Carrier = -1;
	UPROPERTY()
	FVector_NetQuantize At = FVector::ZeroVector;       // its feet (home or dropped); the carrier's while carried

	bool operator==(const FOutlawsModeItemRep& O) const { return Kind == O.Kind && State == O.State && Carrier == O.Carrier && FVector(At).Equals(FVector(O.At), 5.0); }
};

/** The session's shared state, replicated from the host to the client: the lobby and the level start. */
UCLASS(NotPlaceable)
class OUTLAWSRUNTIME_API AOutlawsCoopSession : public AInfo
{
	GENERATED_BODY()

public:
	AOutlawsCoopSession();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** N1: a player besides the host is connected, the host is ready, every connected player besides the host is ready (with
	 *  two players: the client's own). */
	UPROPERTY(Replicated)
	bool bClientConnected = false;
	UPROPERTY(Replicated)
	bool bHostReady = false;
	UPROPERTY(Replicated)
	bool bClientReady = false;
	/** N1: the host's lobby row, the most players this game lets in (2 to 8), and how many are in now. */
	UPROPERTY(Replicated)
	int32 MaxPlayers = 2;
	UPROPERTY(Replicated)
	int32 Connected = 1;
	/** The level both machines build when StartSerial moves on, and at which difficulty (EOutlawsDifficulty as a number). */
	UPROPERTY(Replicated)
	FString Level;
	UPROPERTY(Replicated)
	int32 Difficulty = 0;
	UPROPERTY(Replicated)
	int32 StartSerial = 0;
	/** Milestone 4: the host's enemies by index, for the level of EnemyEpoch (its start serial). */
	UPROPERTY(Replicated)
	TArray<FOutlawsCoopEnemySnap> Enemies;
	UPROPERTY(Replicated)
	int32 EnemyEpoch = 0;
	/** Milestone 5: how the level of StartSerial begins (EOutlawsCoopStart as a number). */
	UPROPERTY(Replicated)
	uint8 StartKind = 0;
	/** Milestone 5: the host's INF for the level of InfEpoch: every elevator by index, the triggers' arming as bits. */
	UPROPERTY(Replicated)
	TArray<FOutlawsCoopInfSnap> InfElevators;
	UPROPERTY(Replicated)
	TArray<uint8> InfTriggers;
	UPROPERTY(Replicated)
	int32 InfEpoch = 0;
	/** Milestone 8: a start from a save (StartKind Load): the difficulty its level was built with, and its name. */
	UPROPERTY(Replicated)
	int32 SpawnDifficulty = -1;
	UPROPERTY(Replicated)
	FString LoadName;
	/** Co-op Historical Missions: the host's session at this start (a mission from the office's poster, or the office again):
	 *  each machine builds the level as one, the mission and its poster's return sector the host's. */
	UPROPERTY(Replicated)
	bool bHom = false;
	UPROPERTY(Replicated)
	FString HomMission;
	UPROPERTY(Replicated)
	FString HomBack;
	/** 2026-10-02 (the user's request): the extra bad guys for the players beyond the first, the host's list for the level of
	 *  ExtrasSerial (set in the same frame as the start serial; the client starts a level only once the two agree): which
	 *  of the level's own bad guys each stands beside, his kind and where he stands.  Players: the players the list was made for. */
	UPROPERTY(Replicated)
	int32 Players = 1;
	/** R14: the bad guys' total multiplier the list was made for (the host's lobby row; the client's lobby shows it). */
	UPROPERTY(Replicated)
	float Multiplier = 1.f;
	UPROPERTY(Replicated)
	TArray<FOutlawsCoopExtra> Extras;
	UPROPERTY(Replicated)
	int32 ExtrasSerial = 0;
	UPROPERTY(Replicated)
	FString ExtrasLevel;
	UPROPERTY(Replicated)
	int32 ExtrasDifficulty = -1;
	UPROPERTY(Replicated)
	int32 ExtrasEligible = 0;
	/** REQ-COOPRANDO (user 2026-10-05): the host's randomizer run for the start of ExtrasSerial (set with the extras, in the same
	 *  frame as the start serial): its options, seed and the level's attempt, so every machine builds the same roll. */
	UPROPERTY(Replicated)
	TArray<int32> Rando;

	// ---- M1: a match (OutlawsCoopMatch.cpp); MatchClass 0 is story co-op ----
	/** The game class (EOutlawsGameClass: 6 Death Match), 0 in story co-op. */
	UPROPERTY(Replicated)
	uint8 MatchClass = 0;
	/** The host's flow (FOutlawsMatchFlow::EState), its limits and the time left (-1: none). */
	UPROPERTY(Replicated)
	uint8 MatchState = 0;
	UPROPERTY(Replicated)
	int32 FragLimit = 0;
	UPROPERTY(Replicated)
	int32 TimeLimitMinutes = 0;
	UPROPERTY(Replicated)
	float TimeLeft = -1.f;
	UPROPERTY(Replicated)
	FString MapName;
	/** M1 hardening: a game password is set (the join page asks for it; the check is the host's, in PreLogin). */
	UPROPERTY(Replicated)
	bool bPassworded = false;
	UPROPERTY(Replicated)
	TArray<FOutlawsMatchScore> Scores;
	// ---- M2: the modes (OutlawsCoopModes.cpp) ----
	UPROPERTY(Replicated)
	TArray<FOutlawsModeItemRep> ModeItems;
	UPROPERTY(Replicated)
	int32 RedScore = 0;
	UPROPERTY(Replicated)
	int32 BlueScore = 0;
	/** Tag: the player who is IT (-1: nobody). */
	UPROPERTY(Replicated)
	int32 ItPlayer = -1;
	/** The class is played red against blue (Team Play, CTF; Secret Document when the host picked teams). */
	UPROPERTY(Replicated)
	bool bTeams = false;
	/** REQ-REGEN2X: the host's auto health regen a second (OutlawsFeel::RegenPerSecond), set where the host makes the session;
	 *  every player heals at it.  -1 until it arrives (a client then uses its own). */
	UPROPERTY(Replicated)
	float RegenRate = -1.f;
	/** BUG-ALLOWCHEATS: the match's host has Allow Cheats off (the Host Game page's row); false in story co-op (it has no such row).
	 *  The host's own answer to a client's cheat is the authority (ServerCheatAsk); this is what the client's menu reads. */
	UPROPERTY(Replicated)
	bool bCheatsOff = false;
	/** REQ-MPLOBBY: the lobby's start countdown on the host, seconds left (-1: not running), for every lobby page to show. */
	UPROPERTY(Replicated)
	float LobbyLeft = -1.f;
	/** REQ-MPLOBBY (b): the match ended and the host has not started the next one yet (every machine's score screen; the
	 *  joiners' lobby has Enter Game greyed out).  MatchCycle counts the host's next matches (0: the first).  RandomBits: the
	 *  match's map (1) and game (2) were rolled ("Random each match"), for the lobby and the score screen to say. */
	UPROPERTY(Replicated)
	bool bBetweenMatches = false;
	UPROPERTY(Replicated)
	int32 MatchCycle = 0;
	UPROPERTY(Replicated)
	uint8 RandomBits = 0;
};

/** M1 hardening: every kind of message a player sends the host, each with its own rate limit (OutlawsCoopGuard.cpp). */
enum class EOLGate : uint8
{
	Ready, Pose, Stats, Built, PickupClaim, PickupDrop, GroundItem, TestNote, EnemyHit, Noise, InfEvent, MovieSkip, LevelEnd,
	PlayerHit, MatchRespawn, MatchCharacter, Revive, Pace, Name, LevelStats, Blast, WorldBit, ShotFx, Throw, Player2, EnemyGrab,
	EnemyRelease, Voice, JoinHello, BuildLine, Team, Taunt, AppPause, Cheat, BreakFx, Count
};

/** One per player, owned by that player's controller: its own way to the host (Server RPCs) and its pose for the other. */
UCLASS(NotPlaceable)
class OUTLAWSRUNTIME_API AOutlawsCoopLink : public AInfo
{
	GENERATED_BODY()

public:
	AOutlawsCoopLink();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** 0 the host, 1 the client (N1: the clients 1 to 7, the lowest free number as each joins). */
	UPROPERTY(Replicated)
	int32 PlayerIndex = 0;
	/** REQ-MPBOTS: a bot's link (the host made it; no connection owns it): its pose, stats, name and character are the host's
	 *  bot's (OutlawsCoopBots.cpp).  Every Client or Server call on it is absorbed (GetFunctionCallspace): on the host an
	 *  unowned actor's Client call would otherwise run on the host itself. */
	UPROPERTY(Replicated)
	bool bBot = false;
	virtual int32 GetFunctionCallspace(UFunction* Function, FFrame* Stack) override;
	/** N1: this player's Ready in the lobby (the session's bClientReady is all of the clients'). */
	UPROPERTY(Replicated)
	bool bLobbyReady = false;
	/** This player's latest pose (not sent back to its own machine). */
	UPROPERTY(Replicated)
	FOutlawsCoopPose Pose;
	/** REQ-COOPHORSES: this player's horse slot (1..7; 0 the host, who rides James's), the host's to give: the lowest free. */
	UPROPERTY(Replicated)
	int32 HorseSlot = 0;

	UFUNCTION(Server, Reliable)
	void ServerSetReady(bool bReady);
	UFUNCTION(Server, Unreliable)
	void ServerPose(const FOutlawsCoopPose& InPose);

	// ---- milestone 3 (OutlawsCoopWorld.cpp) ----
	/** This player's health, armour and death (not sent back to its own machine). */
	UPROPERTY(Replicated)
	FOutlawsCoopStats Stats;
	/** The start serial whose level this player's machine has built (0: none yet). */
	UPROPERTY(Replicated)
	int32 BuiltSerial = 0;

	// owner to host
	UFUNCTION(Server, Reliable)
	void ServerStats(const FOutlawsCoopStats& In);
	UFUNCTION(Server, Reliable)
	void ServerLevelBuilt(int32 Serial);
	/** A pickup taken (Kind 1) or taken into a hand (Kind 0) on the owner's machine: the host says who has it.  Epoch: the
	 *  start serial of the level it belongs to. */
	UFUNCTION(Server, Reliable)
	void ServerPickupClaim(int32 Id, int32 Epoch, uint8 Kind);
	/** A pickup the owner held, put back on the ground at At. */
	UFUNCTION(Server, Reliable)
	void ServerPickupDrop(int32 Id, int32 Epoch, FVector_NetQuantize10 At);
	/** A ground item made on the owner's machine in play (a thrown knife come down, a stick put down): the host numbers it
	 *  and makes it there too.  Tag: the owner's own name for it until the number comes back. */
	UFUNCTION(Server, Reliable)
	void ServerGroundItem(const FString& Name, FVector_NetQuantize10 At, int32 Tag, int32 Epoch);
	/** Tests: the owner's step done, said to the host. */
	UFUNCTION(Server, Reliable)
	void ServerTestNote(const FString& Note);

	// host to the client (on the client's own link only)
	/** News of a pickup: EOutlawsCoopPickupEvent as a number; At where it was put down. */
	UFUNCTION(Client, Reliable)
	void ClientPickup(int32 Id, int32 Epoch, uint8 Event, FVector_NetQuantize10 At);
	/** A ground item made in play, numbered by the host (Tag > 0: the client's own, Tag as it sent it). */
	UFUNCTION(Client, Reliable)
	void ClientGroundItem(int32 Id, const FString& Name, FVector_NetQuantize10 At, int32 Tag, int32 Epoch);
	/** The host hurts this player: DamagePlayer's damage, location scale and plate skip, and where it came from. */
	UFUNCTION(Client, Reliable)
	void ClientHurt(float Damage, float LocationMult, bool bSkipArmour, bool bHasFrom, FVector_NetQuantize10 From, int32 SourceEnemy);
	/** Tests: a step for the client to take. */
	UFUNCTION(Client, Reliable)
	void ClientTest(const FString& Step, int32 Arg);

	// ---- milestone 4 (OutlawsCoopEnemies.cpp) ----
	/** The client's hit on an enemy (its copy there is a puppet): the host deals it.  Flags: 1 a blast, 2 a shove only. */
	UFUNCTION(Server, Reliable)
	void ServerEnemyHit(int32 Epoch, int32 Index, float Damage, FVector_NetQuantize10 Point, FVector_NetQuantizeNormal Dir, FVector_NetQuantize10 From, uint8 Flags, float KnockScale);
	/** The client's gunshot, for the host's enemies to hear. */
	UFUNCTION(Server, Reliable)
	void ServerNoise(int32 Epoch, FVector_NetQuantize10 Where, float Radius);
	/** An enemy died on the host (its death choreography, the way it was hit): the client's copy dies the same way. */
	UFUNCTION(Client, Reliable)
	void ClientEnemyDied(int32 Epoch, int32 Index, int32 Chor, FVector_NetQuantizeNormal Dir);

	// ---- milestone 5 (OutlawsCoopInf.cpp) ----
	/** The owner's player set off an INF event on its own machine (a sector entered or left, a line crossed, a use, a hand
	 *  push, a shot or a blast on a scripted surface): the host runs it with the owner's own keys (Items: its INF item ids). */
	UFUNCTION(Server, Reliable)
	void ServerInfEvent(int32 Epoch, int32 Sector, int32 Wall, int32 Event, const TArray<int32>& Items);
	/** The owner skipped the movie between two levels: it ends for both. */
	UFUNCTION(Server, Reliable)
	void ServerMovieSkip(int32 Epoch);
	/** A sound the host's INF played (Mode 0 once, 1 a loop starts, 2 a loop stops; Loop: the elevator's index). */
	UFUNCTION(Client, Reliable)
	void ClientInfSound(int32 Epoch, const FString& Wav, FVector_NetQuantize10 At, int32 Loop, uint8 Mode);
	/** INF news: Kind 0 a message to show (A its number), 1 a lock this player tried (A the item, B 1 when it opened),
	 *  2 an item of this player's spent in a lock (A its use id), 3 a wall's animated texture told (A the sector, B the
	 *  wall, C the message type, D its argument), 4 the level's mid-level movie, 5 a Historical Mission's outlaw down (A 1
	 *  captured or 2 killed, B his name's local.msg number). */
	UFUNCTION(Client, Reliable)
	void ClientInfNews(int32 Epoch, uint8 Kind, int32 A, int32 B, int32 C, int32 D);
	/** The level is won on the host (its boss, its INF): this machine runs the same end (the fade, the movie); the host's
	 *  start serial then brings the next level to both. */
	UFUNCTION(Client, Reliable)
	void ClientLevelWon(int32 Epoch, const FString& Next, const FString& Movie, const FString& Why);
	/** N1: the owner's level end (its fade, panels and movie) is over for the level of Serial, or it had none (dead): the
	 *  host may start the next level once every player has said so (or 20 s). */
	UFUNCTION(Server, Reliable)
	void ServerLevelEndDone(int32 Serial);
	/** Host: the level whose end this player has finished (not replicated). */
	int32 EndDoneSerial = 0;

	// ---- M1 hardening (OutlawsCoopGuard.cpp): what this player sends the host, checked before anything is done with it ----
	/** False (and said in the log, at most once a second for each kind) when this player has sent too many of Kind this
	 *  second, or bValid is false (out of bounds: a size, an index, a number that is not finite); the message is dropped. */
	bool Admit(EOLGate Kind, bool bValid);
	static const TCHAR* GateName(EOLGate Kind);
	static bool Finite(const FVector& V, double Bound = 1.0e7) { return !V.ContainsNaN() && V.GetAbsMax() < Bound; }
	static bool Finite(float F, float Bound = 1.0e7f) { return FMath::IsFinite(F) && FMath::Abs(F) < Bound; }
	int32 GateDropped[int32(EOLGate::Count)] = {};
	int32 DroppedTotal = 0;
	double GateAt[int32(EOLGate::Count)] = {};
	int32 GateN[int32(EOLGate::Count)] = {};
	double GateLogAt[int32(EOLGate::Count)] = {};

	// ---- M1: a match (OutlawsCoopMatch.cpp) ----
	/** This player's character in the match (the others draw its sprite set). */
	UPROPERTY(Replicated)
	FString Character;
	/** The owner's shot struck player Victim (a pellet through its body as the owner's machine draws it): the host checks
	 *  and deals it.  At: where it struck; From: the muzzle.  bBlast: the owner's dynamite (REQ-KILLFEED: "blew up"). */
	UFUNCTION(Server, Reliable)
	void ServerPlayerHit(int32 Epoch, int32 Victim, float Damage, FVector_NetQuantize10 At, FVector_NetQuantize10 From, bool bBlast);
	/** The owner (dead) asks to come back. */
	UFUNCTION(Server, Reliable)
	void ServerMatchRespawn(int32 Epoch);
	/** M2: this player's team (1 red, 2 blue, 0 none: a free-for-all). */
	UPROPERTY(Replicated)
	uint8 Team = 0;
	/** M2: the owner asks to play for the other team (the pause menu's row). */
	UFUNCTION(Server, Reliable)
	void ServerMatchTeam(uint8 InTeam);
	/** The owner chose a character (worn from its next spawn). */
	UFUNCTION(Server, Reliable)
	void ServerMatchCharacter(const FString& Short);
	/** The host: this player comes back (or is placed) at a spawn point, as Character, with the start kit and full health. */
	UFUNCTION(Client, Reliable)
	void ClientMatchRespawn(int32 Epoch, FVector_NetQuantize10 At, float Yaw, const FString& InCharacter);
	/** The host's message line ("Waiting for players (2/8)", "Starting in 3", "Marshal wins!") on this player's machine. */
	UFUNCTION(Client, Reliable)
	void ClientMatchMessage(const FString& Text, float Seconds);
	/** BUG-ALLOWCHEATS: a client asks the host before it uses a cheat row (Name: the row's name, "cheat_weapons"); the host
	 *  answers with ClientCheatReply (Id as sent).  Refused while the match's Allow Cheats is off; nothing is changed on either machine. */
	UFUNCTION(Server, Reliable)
	void ServerCheatAsk(int32 Id, const FString& Name);
	UFUNCTION(Client, Reliable)
	void ClientCheatReply(int32 Id, bool bAllowed);
	/** REQ-KILLFEED: the host's word of a death (Killer == Victim: nobody else's; Kind an EOutlawsObit), worded on this machine
	 *  for its own player and shown in the kill feed. */
	UFUNCTION(Client, Reliable)
	void ClientMatchKill(int32 Killer, int32 Victim, uint8 Kind, const FString& KillerName, const FString& VictimName);
	/** REQ-MPLOBBY (b): the host ended the game (End game between matches, or its Quit): this player to its main menu, told so. */
	UFUNCTION(Client, Reliable)
	void ClientHostEnded();

	// ---- milestone 7 (OutlawsCoopRules.cpp) ----
	/** The owner (a client) revived player Target (N1: the host's, or another client's, passed on) with its medical bag
	 *  points: back where it fell, with Health (the points given, 2026-10-02 rule: at most half health). */
	UFUNCTION(Server, Reliable)
	void ServerRevive(int32 Epoch, int32 Target, int32 Health);
	/** Player By (the host, or N1 another client through the host) revived this player with its medical bag points. */
	UFUNCTION(Client, Reliable)
	void ClientRevive(int32 Epoch, int32 By, int32 Health);

	// ---- 2026-10-02 decision 6: the movement speed settings combined (OutlawsCoopRules.cpp) ----
	/** This player's own pace (AOutlawsLevelActor::Pace(): the speed setting, or the teleport pace) as its machine last
	 *  said (0: not heard yet).  The bad guys take the two players' average, the timed doors the slower one. */
	UPROPERTY(Replicated)
	float Pace = 0.f;
	UFUNCTION(Server, Reliable)
	void ServerPace(float InPace);

	// ---- the options' Multiplayer page (user 2026-10-02; OutlawsCoopNameplate.cpp) ----
	/** The name this player set (empty: none; the other machine then shows the health bar alone over their head), and a
	 *  count of the names the host has taken (0: none yet), so the other machine knows a name has come. */
	UPROPERTY(Replicated)
	FString PlayerName;
	UPROPERTY(Replicated)
	int32 NameSerial = 0;
	/** The owner's name, sent when it is first known and whenever the player changes it. */
	UFUNCTION(Server, Reliable)
	void ServerPlayerName(const FString& InName);

	// ---- the level's end panels (user 2026-10-02; OutlawsStats.cpp) ----
	/** This player's counts for the level, written on the host only: the host's own, or the client's own as it sent them
	 *  with the bad guys' deaths the host credited it.  Both machines draw both panels from these. */
	UPROPERTY(Replicated)
	FOutlawsLevelStats LevelStats;
	/** The owner's (the client's) own counts, sent as they change (at most twice a second). */
	UFUNCTION(Server, Reliable)
	void ServerLevelStats(const FOutlawsLevelStats& In);

	// ---- milestone 6 (OutlawsCoopVerbs.cpp): the shared world's verbs ----
	/** A blast on the owner's machine (Stick: the pickup number of the stick that went off, -1 none known). */
	UFUNCTION(Server, Reliable)
	void ServerBlast(int32 Epoch, FVector_NetQuantize10 Where, int32 Stick);
	/** A change to the level on the owner's machine (UOutlawsCoop::EBit as a number): a wall blown, a pane shattered, a
	 *  prop's reaction or a prop taken off its shelf, a lamp's shade broken, a lamp swung. */
	UFUNCTION(Server, Reliable)
	void ServerWorldBit(int32 Epoch, uint8 Kind, int32 Index, int32 Arg, FVector_NetQuantize100 Vec, float F);
	/** The owner's gunshot: its sound and flash at the muzzle on the other machine. */
	UFUNCTION(Server, Unreliable)
	void ServerShotFx(FVector_NetQuantize10 Muzzle, const FString& Sound);
	/** The owner threw something (Kind: 1 dynamite, 2 a knife, 3 a bottle, 4 a prop that does not break): it flies there too. */
	UFUNCTION(Server, Reliable)
	void ServerThrow(int32 Epoch, uint8 Kind, FVector_NetQuantize10 Pos, FVector_NetQuantize10 Vel, float Grav, float Fuse);
	// (to a client: From is the player who did it, the host or, N1, another client whose word the host passes on)
	UFUNCTION(Client, Reliable)
	void ClientBlast(int32 From, int32 Epoch, FVector_NetQuantize10 Where, int32 Stick);
	UFUNCTION(Client, Reliable)
	void ClientWorldBit(int32 From, int32 Epoch, uint8 Kind, int32 Index, int32 Arg, FVector_NetQuantize100 Vec, float F);
	UFUNCTION(Client, Unreliable)
	void ClientShotFx(int32 From, FVector_NetQuantize10 Muzzle, const FString& Sound);
	/** M4: the owner said a taunt (a wav of the LABs): heard from where that player stands on every other machine. */
	UFUNCTION(Server, Reliable)
	void ServerTaunt(const FString& Wav);
	UFUNCTION(Client, Reliable)
	void ClientTaunt(int32 From, const FString& Wav);
	UFUNCTION(Client, Reliable)
	void ClientThrow(int32 From, int32 Epoch, uint8 Kind, FVector_NetQuantize10 Pos, FVector_NetQuantize10 Vel, float Grav, float Fuse);
	/** REQ-FLICKRELOAD: the owner's double gun in Hand broke open (Shells fired shells out of Breech at about Vel) or snapped
	 *  shut: the host takes it as that player's open barrels (the pose's Break) and passes it on; each machine hears it and sees
	 *  the shells fly from that player. */
	UFUNCTION(Server, Reliable)
	void ServerBreakFx(uint8 Hand, bool bOpen, uint8 Shells, FVector_NetQuantize10 Breech, FVector_NetQuantize10 Vel);
	UFUNCTION(Client, Reliable)
	void ClientBreakFx(int32 From, uint8 Hand, bool bOpen, uint8 Shells, FVector_NetQuantize10 Breech, FVector_NetQuantize10 Vel);
	/** (the host) the open barrels it has accepted for this player: what its pose's Break says to everyone. */
	uint8 BreakAccepted = 0;

	// ---- milestone 8 (OutlawsCoopSaves.cpp): saves ----
	/** The owner's (the client's) own player as a save writes it, for the host's saves to keep as "player2" (sent when it
	 *  changes, at most once a second). */
	UFUNCTION(Server, Reliable)
	void ServerPlayer2(const FString& Json);
	/** A save loaded for both: its JSON, Part of Parts (each well under a reliable call's size limit), for the level of Epoch. */
	UFUNCTION(Client, Reliable)
	void ClientSnapshotPart(int32 Epoch, int32 Part, int32 Parts, const FString& Text);

	// ---- milestone 6 item 4 (OutlawsCoopGrabs.cpp): the client's hands on bad guys ----
	/** The owner (the client) took bad guy Index into hand Hand on its own machine: the host holds its copy for it. */
	UFUNCTION(Server, Reliable)
	void ServerEnemyGrab(int32 Epoch, int32 Index, uint8 Hand);
	/** The owner let go of bad guy Index at From with the hand's throw Vel (Gravity: the throw's fall, as FlightGravity):
	 *  the host throws its copy the same way. */
	UFUNCTION(Server, Reliable)
	void ServerEnemyRelease(int32 Epoch, int32 Index, FVector_NetQuantize10 From, FVector_NetQuantize10 Vel, float Gravity);
	/** The host's word on a bad guy in a hand (UOutlawsCoop::EHold as a number; Speed: a throw's). */
	UFUNCTION(Client, Reliable)
	void ClientEnemyHold(int32 Epoch, int32 Index, uint8 Kind, float Speed);
	/** 2026-10-02: the host's copy of a bad guy the client threw struck another on the way and hopped back: the client's own
	 *  flight of him goes on from Pos at Vel. */
	UFUNCTION(Client, Reliable)
	void ClientEnemyFlight(int32 Epoch, int32 Index, FVector_NetQuantize10 Pos, FVector_NetQuantize10 Vel);

	// ---- voice chat (user 2026-10-02; OutlawsCoopVoice.cpp) ----
	/** The owner's (the client's) voice: a few Opus frames as the engine's Voice module encodes them (unreliable: a lost
	 *  packet is a short gap, never a stall). */
	UFUNCTION(Server, Unreliable)
	void ServerVoice(const TArray<uint8>& Data);
	/** Player From's voice (the host's, or N1 another client's passed on by the host), to this link's owner. */
	UFUNCTION(Client, Unreliable)
	void ClientVoice(int32 From, const TArray<uint8>& Data);

	// ---- R7 (OutlawsCoopJoin.cpp): a player joining while the host is already playing a level ----
	/** Set by the host as this link is made, when a level is already under way: the owner joins it from a snapshot of
	 *  the host's game instead of the lobby's start. */
	UPROPERTY(Replicated)
	bool bLateJoin = false;
	/** The start serial whose snapshot the host has sent this owner (0: none yet), and how the join stands (EJoin as a
	 *  number: the host busy between levels, the snapshot on its way), for the owner's lobby to say. */
	UPROPERTY(Replicated)
	int32 JoinSerial = 0;
	UPROPERTY(Replicated)
	uint8 JoinState = 0;
	/** The owner (a late joiner) is in the host's world and ready for the snapshot. */
	UFUNCTION(Server, Reliable)
	void ServerJoinHello();
	/** REQ-MPLOBBY: the owner has a microphone on (its Multiplayer page's setting), for the lobby's per-player mute rows. */
	UPROPERTY(Replicated)
	bool bMic = false;
	UFUNCTION(Server, Reliable)
	void ServerMic(bool bOn);

	// ---- R8 (OutlawsCoopConnect.cpp): the pause menu's Co-op page ----
	/** This player's build line (OutlawsBuildInfo::Line), sent once: the other's page shows both. */
	UPROPERTY(Replicated)
	FString BuildLine;
	UFUNCTION(Server, Reliable)
	void ServerBuildLine(const FString& Line);

	// ---- BUG-COOPTIMEOUT (OutlawsCoopPauseHold.cpp): a client whose app the Quest pauses ----
	/** The host holds this player's place: its app said it was pausing (the system menu, the headset off, a sleep) and has
	 *  not said it is back.  Its puppet stands still with "(paused)" on its plate; the bad guys leave it be. */
	UPROPERTY(Replicated)
	bool bAppPaused = false;
	/** The owner's app is pausing (true) or back (false). */
	UFUNCTION(Server, Reliable)
	void ServerAppPause(bool bPausing);
	/** The host heard the owner's "back": the connection lives (no answer: the owner reconnects). */
	UFUNCTION(Client, Reliable)
	void ClientResumeAck(int32 HeldSeconds);
	/** BUG-HOSTPAUSE: the host's own app is pausing (true: hold the game for GraceSeconds) or back (false). */
	UFUNCTION(Client, Reliable)
	void ClientHostPause(bool bPausing, float GraceSeconds);
};

/** This machine's microphone in co-op voice chat (UOutlawsCoop::MicState; user 2026-10-02). */
enum class EOutlawsCoopMic : uint8
{
	Off,                  // the Multiplayer page's Microphone is off (nothing captured, nothing sent)
	On,                   // on: open mic in a session or its lobby (sent while the voice gate is open)
	Asking,               // on, the headset is asking the player for the microphone permission
	NotAllowed,           // on, but the player refused the microphone permission (Quest): it stays off
	NoDevice,             // on, but no microphone could be opened
};

/** What the host says about a pickup (AOutlawsCoopLink::ClientPickup). */
enum class EOutlawsCoopPickupEvent : uint8
{
	HeldByOther,          // the other player took it into a hand: hidden here, nobody else can take it
	TakenByOther,         // the other player took it for good: gone here, nothing given
	DroppedByOther,       // the other player put it down again (At)
	ConfirmHold,          // this machine's claim stands
	ConfirmTake,
	DenyHold,             // this machine's claim is refused: the other player had it first
	DenyTake,
};

/** The player controller of a co-op session (both machines).  The client makes and possesses its own player locally, as
 *  single player does, so the host never sends it a pawn: possession without authority is allowed, and the host's
 *  "you are spectating" and view-target nudges are ignored once the local player is possessed. */
UCLASS(NotPlaceable)
class OUTLAWSRUNTIME_API AOutlawsCoopController : public APlayerController
{
	GENERATED_BODY()

public:
	AOutlawsCoopController();

protected:
	virtual void ClientGotoState_Implementation(FName NewState) override;
	virtual void ClientSetViewTarget_Implementation(AActor* A, FViewTargetTransitionParams TransitionParams) override;
};

/** The game mode of a co-op session (only ever opened with ?game=, never the project's default).  No pawn is spawned for
 *  anyone: each machine's AOutlawsLevelActor makes its own player.  It hands out the links and keeps the session actor. */
UCLASS(NotPlaceable)
class OUTLAWSRUNTIME_API AOutlawsCoopGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AOutlawsCoopGameMode();
	virtual void InitGameState() override;
	virtual void PreLogin(const FString& Options, const FString& Address, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage) override;
	virtual void PostLogin(APlayerController* NewPlayer) override;
	virtual void Logout(AController* Exiting) override;
	virtual void RestartPlayer(AController* NewPlayer) override;
};

/** The co-op state that lives across map travel (the game instance's), the lobby's logic and the session's tick. */
UCLASS()
class OUTLAWSRUNTIME_API UOutlawsCoop : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	enum { Port = 7777 };
	/** N1 (user 2026-10-03: "push co-op to 8 players"): story co-op's most; M1: a match's most (16 on a PC host). The host is
	 *  player 0. */
	static constexpr int32 MaxCoopPlayers = 8;
	static constexpr int32 MaxPlayers = 16;

	/** R7: this machine hosts a session nobody else has joined (or everyone left): its menu stops the world as single
	 *  player's does, and nobody is kept out of harm by a menu. */
	static bool IsHostAlone(const UObject* Context);
	/** R8: the Multiplayer page's Microphone sensitivity (0 low, 1 normal, 2 high) as the voice gate's level (ol.VoiceGate). */
	static void ApplyMicSensitivity(int32 Sensitivity);
	/** R8: the Voice Chat page's meter (REQ-VOICEPAGE: under Options > Multiplayer): this machine's microphone level now (0..1,
	 *  held a moment), and whether it would be sent (the gate open, or the push-to-talk grip held).  The page opens the
	 *  microphone while it is shown (MicMeterOpen). */
	static float MicMeterLevel(const UObject* Context);
	static bool MicMeterSending(const UObject* Context);
	static bool MicMeterOpen(const UObject* Context);
	/** R8: the push-to-talk grip is held (true too when push to talk is off). */
	bool TalkKeyHeld() const;
	/** R8: the Multiplayer page shown outside a session: the microphone open for its meter only (nothing sent). */
	void TickMicMeter(double Now);
	/** The held map's partner marker (user 2026-10-03): where the other player stands now (their body's feet, as drawn
	 *  here) and their name; false outside a session or before they are drawn.  (Competitive play will show team-mates
	 *  only, by the nameplate rule.) */
	static bool PartnerForMap(const UObject* Context, FVector& OutFeet, FString& OutName, int32 Slot = 0);
	/** REQ-HMBOSSEND: a Historical Mission's outlaw down (Outcome 1 captured, 2 killed; NameId his local.msg name): true when
	 *  this machine shows it now (no session, or the host, whose word then goes to every client); false on a client, which
	 *  waits for the host's word (INF news kind 5). */
	static bool HomBossDown(const UObject* Context, int32 Outcome, int32 NameId);
	/** R7: how a late join stands on the joiner's link (AOutlawsCoopLink::JoinState). */
	enum EJoin : uint8 { JoinNone, JoinHostBusy, JoinSent };
	/** R7 (user 2026-10-03: "the enemy count should still be 150% in this situation"): the players a host's level is made
	 *  for at least (the extra bad guys), however many have joined.  A hook for a later lobby setting (up to 4). */
	int32 PlayersExpected = 2;

	/** The co-op state of the game instance Context belongs to (null in the editor world). */
	static UOutlawsCoop* Get(const UObject* Context);
	/** A session is on (hosting, joining or joined).  False always in single player. */
	static bool IsActive(const UObject* Context);
	/** REQ-MPLOBBY (c): another menu page shown: a player list starts again at its first page. */
	static void MenuPageChanged(const UObject* Context);
	/** The page the title menu opens on: "#front", or the lobby while a session is on. */
	static FString FrontPage(const UObject* Context);
	/** Co-op menu pages and rows (OutlawsPlayerMenu.cpp hands them over). */
	static void BuildMenuPage(AOutlawsPlayer* P, const FString& File, OutlawsMenu::FPage& Page);
	static void MenuAction(AOutlawsPlayer* P, const FString& Name);
	static void StatusLines(const UObject* Context, const FString& File, TArray<FString>& Out);
	/** Bumps whenever the lobby changes (the open co-op page is built again). */
	static uint32 PageVersion(const UObject* Context);
	/** The session's end from the game's own menu (End Game in a co-op level). */
	static void LeaveSession(const UObject* Context, const FString& Reason);
	/** TryPlacePlayer in a session: the controller has no pawn of its own; ours is made and possessed here. */
	static APawn* SpawnLocalPlayer(APlayerController* PC, const FVector& At, const FRotator& Rot);
	/** AOutlawsLevelActor::BeginPlay calls this first (the launch flags, the lobby's world). */
	static void NotifyLevelBeginPlay(AOutlawsLevelActor* Level);

	// ---- milestone 3: the shared world's takeables and the players' state (OutlawsCoopWorld.cpp); the level actor calls
	// these only in a session ----
	/** CollectPickup is about to give a pickup's contents: what this machine has now (the host may yet give it to the
	 *  other player, and then it all goes back). */
	static void BeforeTake(AOutlawsLevelActor* L);
	/** CollectPickup gave pickup Index's contents here (Index still names it). */
	static void NotePickupTaken(AOutlawsLevelActor* L, int32 Index);
	/** Pickup Index taken into a hand here (TakePickupNear, PullPickupAlong). */
	static void NotePickupHeld(AOutlawsLevelActor* L, int32 Index);
	/** A held pickup put back on the ground here (DropPickupActor). */
	static void NotePickupDropped(AOutlawsLevelActor* L, AActor* Item);
	/** A ground item made here in play (SpawnGroundItem: an enemy's drop, a thrown knife come down, a stick put down). */
	static void NoteGroundItem(AOutlawsLevelActor* L, AActor* A, const FString& Name, const FVector& FeetPos);
	/** The host hurts player Who (N1; the client of two by default), on that player's own machine (DamagePlayer there with
	 *  these values).  SourceEnemy: the enemy whose shot it was (-1: none). */
	void HurtOther(float Damage, float LocationMult, bool bSkipArmour, const FVector* From, int32 SourceEnemy = -1, int32 Who = 1);
	/** The partner's health, armour and death as their machine last said (Seq 0: nothing heard yet; N1: PartnerIndex's). */
	const FOutlawsCoopStats* OtherStats() const;
	// (the links hand these over)
	void OnPickupClaim(int32 Who, int32 Id, int32 Epoch, uint8 Kind);
	void OnPickupDrop(int32 Who, int32 Id, int32 Epoch, const FVector& At);
	void OnGroundItemRequest(int32 Who, const FString& Name, const FVector& At, int32 Tag, int32 Epoch);
	void OnPickupEvent(int32 Id, int32 Epoch, uint8 Event, const FVector& At);
	void OnGroundItem(int32 Id, const FString& Name, const FVector& At, int32 Tag, int32 Epoch);
	void OnHurt(float Damage, float LocationMult, bool bSkipArmour, bool bHasFrom, const FVector& From, int32 SourceEnemy);

	// ---- milestone 4: enemies (OutlawsCoopEnemies.cpp).  The host thinks for every enemy; the client's are puppets ----
	/** Host: each client's player as the enemies see it, by player index (updated each frame; N1: one per player). */
	FOutlawsCoopRemoteView RemoteViews[MaxPlayers];
	const FOutlawsCoopRemoteView* RemoteViewOf(int32 Who) const { return Who > 0 && Who < MaxPlayers ? &RemoteViews[Who] : nullptr; }
	/** Client: a hit on a puppet enemy, sent for the host to deal (Flags 1 a blast, 2 a shove only). */
	void ReportEnemyHit(const AOutlawsEnemy* E, float Damage, const FVector& Point, const FVector& Dir, const FVector& From, uint8 Flags, float KnockScale);
	/** Host: an enemy died; the client's copy dies the same way. */
	void NoteEnemyDied(const AOutlawsEnemy* E, int32 Chor, const FVector& Dir);
	/** Host: an enemy's shot struck the client's player: hurt there. */
	void HurtFromEnemy(const AOutlawsEnemy* E, float Damage, float LocationMult);
	/** Host: which player an alerted enemy is after, said when it changes. */
	void NoteEnemyTarget(const AOutlawsEnemy* E);
	// BUG-BOSSIDLEHOST (OutlawsCoopIdle.cpp; 10 Oct co-op test: the late boss went for an idle desktop host at the level's
	// start, not the two players at work).  The host watches each player: no move, turn, shot or hit for ol.CoopIdleSeconds
	// (60), or a desktop player who has not moved since the level's start for ol.CoopIdleDesktopSeconds (20), is idle.
	// The bad guys choose among the active players first and fall back to an idle one only when no active one is left
	// alive and in the level; a late boss comes in by the active players.  Story co-op on the host only (never single
	// player, never a match)
	/** True when the idle rule is on here (a story co-op host, ol.CoopIdleTargets 1). */
	bool IdleRuleOn() const;
	/** Player Who (0 the host) counts as idle now. */
	bool IsPlayerIdle(int32 Who) const;
	/** Player Who did something (fired, was hurt by a bad guy): active again. */
	void NotePlayerActive(int32 Who, const TCHAR* Why);
	/** The players a late boss comes in by (feet and eyes): the active living ones, else the living ones.  False outside a
	 *  story co-op host (the level's own player then, as ever). */
	static bool BossAnchors(const UObject* Context, TArray<FVector>& OutFeet, TArray<FVector>& OutEyes, FString& OutWho);
	/** A player's gunshot for the bad guys to hear (AlertEnemies in a session): the host's own go to its enemies at once
	 *  (they turn on the host), the client's are sent to the host (they turn on the client). */
	static void NoteNoise(AOutlawsLevelActor* L, const FVector& Where, float RadiusCm);
	void OnEnemyHit(int32 Who, int32 Epoch, int32 Index, float Damage, const FVector& Point, const FVector& Dir, const FVector& From, uint8 Flags, float KnockScale);
	void OnNoise(int32 Who, int32 Epoch, const FVector& Where, float Radius);
	void OnEnemyDied(int32 Epoch, int32 Index, int32 Chor, const FVector& Dir);
	void OnTestStep(const FString& Step, int32 Arg);
	void OnTestNote(const FString& Note);

	// ---- milestone 5: the INF and the level's flow (OutlawsCoopInf.cpp).  The host's INF decides for both; the client's
	// shows it.  The level actor calls these only in a session ----
	/** CompleteLevel in a session, its next level and movie chosen: the host's is told to the client (true: go on); the
	 *  client's own is left to the host's word (false), unless it is that word being carried out here. */
	static bool OnLevelWon(AOutlawsLevelActor* L, const FString& Why);
	/** StartNextLevel in a session (the movie over, or none to play): the host starts the next level for both through the
	 *  start serial, the client waits for it.  True: nothing started here. */
	static bool HoldNextLevel(AOutlawsLevelActor* L);
	/** The movie between two levels skipped on this machine: the client's skip is told to the host (it ends for both). */
	static void NoteMovieSkip(AOutlawsLevelActor* L);
	/** BUG-RANCHDOOR: the host's INF set off a mid-level movie (the ranch's boss door, whoever used it): its clients play
	 *  their copy of the level INF's movie too (ClientInfNews kind 4).  Nothing on a client. */
	static void NoteMidLevelMovie(AOutlawsLevelActor* L, const FString& Base);
	int32 MidMoviesSent = 0, MidMoviesPlayed = 0;
	/** An enemy pushes a door open on the host: the INF event is the enemy's, not a player's. */
	static bool EnemyDoorEvent(AOutlawsLevelActor* L, int32 Sector, uint32 Event);
	/** The pause menu's Restart Level (Restart) or the cheat's level skip (Jump) in a session: the host's goes to both
	 *  through the start serial; the client's is refused with a note (the host decides).  True: handled here. */
	static bool MenuLevelChange(AOutlawsPlayer* P, EOutlawsCoopStart Kind, const FString& Level, bool bAskOnly = false);
	/** An office poster's ride in co-op: the host starts the mission for every player as a Historical Mission; a client's is
	 *  refused (the host's to choose).  False outside co-op (single player rides as ever). */
	static bool HomRide(AOutlawsLevelActor* L, const FString& Mission, const FString& Back);
	// (the links hand these over)
	void OnInfEvent(int32 Who, int32 Epoch, int32 Sector, int32 Wall, int32 Event, const TArray<int32>& Items);
	void OnMovieSkip(int32 Who, int32 Epoch);
	void OnInfSound(int32 Epoch, const FString& Wav, const FVector& At, int32 Loop, uint8 SoundMode);
	void OnInfNews(int32 Epoch, uint8 Kind, int32 A, int32 B, int32 C, int32 D);
	void OnLevelWonByHost(int32 Epoch, const FString& Next, const FString& Movie, const FString& Why);

	// ---- milestone 7: the co-op rules (OutlawsCoopRules.cpp): death, keys dropped, revive, respawn, both dead.  The level
	// actor and the player call these only in a session.  (2026-10-02, the user: "one player should be able to continue if
	// the other pauses": a pause menu in a session pauses nothing; the pause for both of milestone 7 is gone) ----
	/** A hand's grip (or a test) at Hand: the other player lies dead within reach: a medical bag of this player's brings
	 *  them back where they fell (one spent); without one, nothing but a note.  True: the grip was this. */
	static bool TryRevive(AOutlawsPlayer* P, const FVector& Hand);
	/** The death screen's button in a session: back at the level's start with the start kit (the keys stay where they
	 *  were dropped). */
	static void RespawnAtStart(AOutlawsPlayer* P);
	/** The death screen's words in a session. */
	static FString DeathPrompt(bool bVR);
	/** 2026-10-02: while a player's pause menu is open in a session the game goes on for both, and that player is out of
	 *  harm: the bad guys leave them be (as a dead player is nobody's target) and nothing hurts them.  The one switch:
	 *  false makes a player in the menu a target like any other who stands still (QUESTION for the user). */
	static constexpr bool bMenuKeepsSafe = true;
	/** True when this machine's own player is in its pause menu and so takes no hurt (Damage: for the log). */
	static bool MenuShields(const UObject* Context, float Damage);
	// (the links hand these over)
	void OnRevive(int32 Epoch, int32 By, int32 Health);

	// ---- 2026-10-02 (the user's request): more bad guys for each player beyond the first (OutlawsCoopExtras.cpp).  The
	// host decides the list at each level start (from its cache, else worked out from its own game files and cached) and
	// puts it on the session with the start serial; both machines make the copies after all the level's own bad guys ----
	/** The players in the session now (one link each). */
	int32 PlayerCount() const;
	/** R14: the bad guys' multiplier a host's level gets now: the lobby row's, else Auto (1 + 0.5 per player beyond the
	 *  first, at most 4); a save being loaded keeps its own (LoadMultiplier); -OLCoopBadGuys=<x> for a test. */
	float BadGuysMultiplier() const;
	bool IsBadGuysAuto() const;
	float LoadMultiplier = 0.f;                  // host: the multiplier of the save being loaded (0: none)
	float TestBadGuys = 0.f;                     // -OLCoopBadGuys=<x> (never written to the settings)
	/** Host, just before the start serial moves on: the list for Level at SpawnDifficulty put on the session. */
	void PrepareExtras(AOutlawsCoopSession* S, const FString& Level, int32 SpawnDifficulty);
	/** Both, as a level starts: the session's list (for start Serial) handed to the level actor for its builds. */
	void HandExtrasToLevel(AOutlawsLevelActor* L, int32 Serial);
	/** Both, the level built: each extra bad guy's body tested against the level's walls (logged). */
	void CheckExtras(AOutlawsLevelActor* L);

	// ---- milestone 6: the shared world's verbs (OutlawsCoopVerbs.cpp).  Each change happens on the machine where it was
	// done and is told to the other by index (the walls, panes, props and lamps are built in the same order on both); the
	// level actor and the player call these only in a session ----
	enum EBit : uint8 { BitWall, BitPane, BitProp, BitPropTaken, BitShade, BitSwing };
	/** A blast went off here (Explode): shown on the other machine at the same place (Stick: its pickup number). */
	static void NoteBlast(AOutlawsLevelActor* L, const FVector& Where, int32 Stick);
	/** A wall blown, a pane shattered, a prop's reaction (Arg its choreography, Vec the hit's way, F 1 a hand's nudge), a
	 *  prop taken off its shelf, a lamp's shade broken (Arg the section) or a lamp swung (Vec the way, F radians a second). */
	static void NoteWorldBit(AOutlawsLevelActor* L, uint8 Kind, int32 Index, int32 Arg, const FVector& Vec, float F);
	/** This player's gunshot (FireGun): heard and seen at the muzzle on the other machine. */
	static void NoteShot(AOutlawsLevelActor* L, const FVector& Muzzle, const TCHAR* Sound);
	/** This player threw something (AOutlawsPlayer::FThrown's type, its start, speed, gravity scale and fuse). */
	static void NoteThrow(AOutlawsLevelActor* L, int32 Kind, const FVector& Pos, const FVector& Vel, float Grav, float Fuse);
	/** REQ-FLICKRELOAD: this machine's double gun broke open (Shells out) or shut: told to the others (OutlawsCoopVerbs.cpp).
	 *  REQ-RELOADSLICES: Loads, the shells going in (the shell slices the others hear), rides in the Shells byte's high nibble. */
	static void NoteBreak(AOutlawsLevelActor* L, int32 Hand, bool bOpen, int32 Shells, const FVector& Breech, const FVector& Vel, int32 Loads = 0);
	/** The pickup number of the stick of dynamite whose actor is Owner (-1: none known). */
	static int32 StickIdOf(const AOutlawsLevelActor* L, const AActor* Owner);
	// ---- milestone 8: saves (OutlawsCoopSaves.cpp).  The host saves in single player's format (its autosave, since R7, to
	// coop_autosave.json; the pause menu's Save Game into the slot chosen) with the client's own state as an optional
	// "player2" block; a load in a session goes to both: the host puts the save back, the client takes its world from the
	// save's JSON sent in parts ----
	/** R8: the host's start-of-level autosave waits (true) while a connected partner has not built this level or told its
	 *  kit yet (a slow link: the save would carry no player2), for at most 10 s after the level was built here. */
	static bool HoldStartAutosave(AOutlawsLevelActor* L);
	/** SaveGame in a session: the client never saves (false); the host saves where single player would, its autosave
	 *  (slot -1) turned into the co-op autosave (slot -2, R7). */
	static bool MaySave(AOutlawsLevelActor* L, int32& Slot);
	/** SaveGame in a session, its JSON made: the client's last reported state added as "player2". */
	static void AddPlayer2(AOutlawsLevelActor* L, class FJsonObject& O);
	static void NoteSaved(AOutlawsLevelActor* L, int32 Slot, int32 Bytes);
	/** LoadGame in a session: the host's starts the save for both (true: on its way); the client's is refused. */
	static bool LoadInSession(AOutlawsLevelActor* L, int32 Slot);
	void OnPlayer2(int32 Who, const FString& Json);
	void OnSnapshotPart(int32 Epoch, int32 Part, int32 Parts, const FString& Text);
	// (the links hand these over)
	void OnBlast(int32 Who, int32 Epoch, const FVector& Where, int32 Stick);
	void OnWorldBit(int32 Who, int32 Epoch, uint8 Kind, int32 Index, int32 Arg, const FVector& Vec, float F);
	void OnShotFx(int32 Who, const FVector& Muzzle, const FString& Sound);
	void OnThrow(int32 Who, int32 Epoch, uint8 Kind, const FVector& Pos, const FVector& Vel, float Grav, float Fuse);
	void OnBreakFx(int32 Who, uint8 Hand, bool bOpen, uint8 Shells, const FVector& Breech, const FVector& Vel);

	// ---- milestone 6 item 4: bad guys grabbed and thrown by either player (OutlawsCoopGrabs.cpp).  Each player's own hand
	// holds and throws on its own machine at once (the single-player code); the host's copy follows the client's hand and
	// is thrown by the same code when the client lets go; the host decides when both reach for the same one.  The player
	// calls these only in a session ----
	enum EHold : uint8 { HoldByHost, ThrowByHost, HoldRefused, HoldLetGo };
	/** This machine's player took bad guy E into hand Hand (AOutlawsPlayer::TryGrabEnemy). */
	static void NoteEnemyGrab(AOutlawsPlayer* P, AOutlawsEnemy* E, int32 Hand);
	/** This machine's player is letting go of bad guy E with the throw Vel (AOutlawsPlayer::LetGoEnemy, before EndHeld). */
	static void NoteEnemyRelease(AOutlawsPlayer* P, AOutlawsEnemy* E, const FVector& Vel);
	// (the links hand these over)
	void OnEnemyGrab(int32 Who, int32 Epoch, int32 Index, uint8 Hand);
	void OnEnemyRelease(int32 Who, int32 Epoch, int32 Index, const FVector& From, const FVector& Vel, float Gravity);
	void OnEnemyHold(int32 Epoch, int32 Index, uint8 Kind, float Speed);
	/** 2026-10-02 decision 3: player Who's body as this machine has it (0 the host, 1 the client: its own capsule here, the
	 *  other's puppet), the capsule's centre, radius and half height; a thrown bad guy meets it on both machines alike.
	 *  False: nobody to meet (dead, not drawn yet). */
	bool BodyOf(int32 Who, FVector& OutCentre, float& OutRadius, float& OutHalf) const;
	/** A thrown bad guy stopped by that body here (logged; counted for the summary). */
	void NoteBodyKnock(const AOutlawsEnemy* E, int32 Who, const FVector& At);
	/** Host: a thrown bad guy struck another (TickCarried): a client's throw is told, so its own flight hops the same. */
	static void NoteThrownStrike(AOutlawsEnemy* E);
	void OnEnemyFlight(int32 Epoch, int32 Index, const FVector& Pos, const FVector& Vel);

	// ---- voice chat (user 2026-10-02: "in a multiplayer section in the options ... they can enable microphone, which is
	// normal volume in lobbies, but spatially calculated ingame"; OutlawsCoopVoice.cpp).  The engine's Voice module (the
	// microphone and its Opus codec) over the co-op links; the other player's voice plays at normal volume in the lobby and
	// from their puppet's head in a level ----
	/** The other player (PlayerIndex: 0 the host, 1 the client) is talking now: their voice has been arriving and playing
	 *  here within the last 0.3 s (for the nameplate's sound icon).  False for this machine's own index and outside a session. */
	static bool IsRemoteTalking(const UObject* Context, int32 PlayerIndex);
	/** This machine's microphone (for the Multiplayer page: NotAllowed is "microphone not allowed"). */
	static EOutlawsCoopMic MicState(const UObject* Context);
	/** A few words for MicState: "off", "on", "asking for the microphone", "microphone not allowed", "no microphone found". */
	static const TCHAR* MicStateText(EOutlawsCoopMic State);
	/** The menu changed MpMicrophone (optional: the setting file is also read once a second in a session): taken at once, and
	 *  on the Quest the microphone permission is asked for the first time it is turned on (or again after a refusal). */
	static void NoteMicSetting(const UObject* Context, bool bOn);
	// (the links hand these over)
	void OnVoice(int32 Who, const TArray<uint8>& Data);

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// FTickableGameObject
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual ETickableTickType GetTickableTickType() const override;
	virtual bool IsTickable() const override;
	virtual bool IsTickableWhenPaused() const override { return true; }
	virtual bool IsTickableInEditor() const override { return false; }
	virtual UWorld* GetTickableGameObjectWorld() const override;

	friend class AOutlawsEnemy;                  // (milestone 4: the enemies' puppets count what they see)
	friend class AOutlawsCoopLink;
	friend class AOutlawsLevelActor;             // (user 2026-10-02: the wheel's drop and the saves make ground items as the death drop does)
	friend class AOutlawsPlayer;                 // (the wheel drop's single-player test finds its ground items so)
	friend class UOutlawsStats;                  // (user 2026-10-02: the level's end panels, the counts on the links)
	EOutlawsCoopMode GetMode() const { return Mode; }
	bool IsHost() const { return Mode == EOutlawsCoopMode::Host; }
	bool IsClient() const { return Mode == EOutlawsCoopMode::Client; }

	/** Host Game: this machine opens the map again as a listen server with the co-op game mode. */
	void Host();
	/** R11: a game played alone opens for co-op from a save (the lobby's Load path), then goes in alone (OutlawsBeacon.cpp). */
	void HostFromSave(int32 Slot);
	bool StartAloneFromLobby();
	/** Join Game: travel to the host's address (port 7777 unless given). */
	void Join(const FString& Address, bool bFromMenu);
	/** The session ends: logged, the mode back to None, and (bTravel) this machine back to its single-player front end. */
	void EndSession(const FString& Reason, bool bTravel);

	// called by the game mode and the links (host side)
	void OnPlayerLogin(APlayerController* PC);
	void OnPlayerLogout(AController* PC);
	void OnJoinHello(int32 Who);
	void SetReadyFor(int32 PlayerIndex, bool bReady);
	AOutlawsCoopSession* GetSession() const;
	AOutlawsCoopLink* GetLink(int32 PlayerIndex) const;
	/** This machine's player: 0 the host; a client its own link's number (N1: 1 to 7; 1 until its link has come). */
	int32 MyIndex() const;
	// ---- N1: up to eight players (the host is 0; each client the lowest free number as it joins) ----
	/** Every player's link this machine has, in player order. */
	void GetLinks(TArray<AOutlawsCoopLink*>& Out) const;
	/** REQ-HELDSWAY: another player's last pose has bad guy EnemyIndex in a hand (held, not flying from a throw). */
	bool OtherPlayerHolds(int32 EnemyIndex) const;
	/** Host: Do for each client's link (Except: one player's left out, the one whose word is being passed on). */
	void ForClients(TFunctionRef<void(AOutlawsCoopLink&)> Do, int32 Except = -1) const;
	/** The partner of the two-player rules and tests: the host's is the first client, a client's the host. */
	int32 PartnerIndex() const;
	/** Player Who's puppet here (null: this machine's own, or not drawn yet); the partner's. */
	AOutlawsRemotePlayer* PuppetOf(int32 Who) const;
	AOutlawsRemotePlayer* PartnerPuppet() const { return PuppetOf(PartnerIndex()); }
	/** The host's lobby row (2 to 8; -OLCoopMaxPlayers=<n> for a test). */
	int32 MaxPlayersSetting() const;
	/** Words for player Who in the log: "host", "client" (with two), "client 3". */
	static const TCHAR* WhoWord(int32 Who);

	// ---- M1: a match over the session (OutlawsCoopMatch.cpp) ----
	/** This session is a match (the Host Game page's), not story co-op. */
	bool IsMatch() const;
	static bool IsMatchActive(const UObject* Context);
	/** M2: a team class is being played, and this machine's player's team (1 red, 2 blue). */
	static bool IsTeamMatch(const UObject* Context);
	/** BUG-ALLOWCHEATS: this machine is a client in a match whose host has Allow Cheats off (the replicated flag; the menu's word). */
	static bool CheatsOffHere(const UObject* Context);
	/** The client's cheat row, asked of the host (a reply of ClientCheatReply runs Done).  False: no host to ask (nothing sent). */
	static bool AskHostCheat(const UObject* Context, const FString& Name, TFunction<void(bool)> Done);
	void OnCheatAsk(int32 Who, int32 Id, const FString& Name);       // (the host)
	void OnCheatReply(int32 Id, bool bAllowed);                       // (the client)
	TMap<int32, TFunction<void(bool)>> CheatWaiting;
	int32 CheatAskId = 0;
	static int32 MyTeam(const UObject* Context);
	/** M2: the plate over player Who's head is hidden here (a free-for-all: all; a team class: the other team's). */
	static bool PlateHiddenFor(const UObject* Context, int32 Who);
	/** REQ-MPLOBBY: that player is still in the lobby (PlayerInLobby): nothing of it drawn on this machine. */
	static bool InLobbyFor(const UObject* Context, int32 Who);
	// ---- M1 security (user 2026-10-03: "add the password and hardening to M1, but make passwords optional") ----
	/** The host's game password (empty: anyone may join) and the one this machine joins with. */
	FString HostPassword;
	FString JoinPassword;
	/** A password as kept and sent: letters, digits and . - _ only, 16 at most. */
	static FString CleanPassword(const FString& In);
	/** The keyboard's text for the co-op lobby's row or the join page's (true: it was one of those). */
	static bool PasswordTyped(AOutlawsPlayer* P, const FString& Purpose, const FString& Text);
	/** PreLogin: a joiner's password checked (empty: let in). */
	FString CheckJoinPassword(const FString& Options) const;
	TMap<FString, FString> RecentPasswords;      // (the join page: the password a Recent host took, by its address)
	void ReadRecentPasswords();
	void NoteRecentPassword(const FString& Address, const FString& Password);
	/** The Host Game page's Start: host a match with these options (a listen session straight into the map). */
	bool HostMatch(const FOutlawsMpHostOptions& Options);
	/** A pellet from From to To (what it struck, or its reach) on this machine: the first other player's body it passes
	 *  through, reported to the host as a hit (true: OutAt where; the pellet stops there). */
	static bool ShotAtPlayers(AOutlawsLevelActor* L, const FVector& From, const FVector& To, float Damage, FVector& OutAt);
	/** M1's gaps (OutlawsCoopMatchStrikes.cpp): a melee blow at At (Reach round it), a thrown thing's step A..B, a blast's reach on
	 *  the other players' bodies drawn here, reported to the host as a pellet is; false (0) when no one is struck. */
	static bool StrikeAtPlayers(AOutlawsLevelActor* L, const FVector& At, float Reach, float Damage, const TCHAR* Kind);
	/** (BUG-SHOVELWHACK) A tool's blow along its striking length A..B (the shovel, the crowbar): the first other player whose
	 *  body (Reach round it) or drawn picture it meets at a point Seen passes, struck as StrikeAtPlayers strikes. */
	static bool StrikeAlongAtPlayers(AOutlawsLevelActor* L, const FVector& A, const FVector& B, float Reach, float Damage, const TCHAR* Kind,
		TFunctionRef<bool(const FVector&)> Seen);
	/** (BUG-SHOVELWHACK's probe, any build) the melee blows sent at other players so far, and the last one's damage. */
	static int32 MatchStrikesSent();
	static float LastMatchStrikeDamage();
	/** (the probe, on the host) hits the host checked and dealt so far. */
	int32 MatchHitsDealtSoFar() const { return MatchHitsDealt; }
	static bool SweepAtPlayers(AOutlawsLevelActor* L, const FVector& A, const FVector& B, float Damage, const TCHAR* Kind, FVector& OutAt);
	static int32 BlastAtPlayers(AOutlawsLevelActor* L, const FVector& Where, float RadiusCm, float Damage);
	/** A match whose blasts the host deals (another machine's blast does not hurt this player directly). */
	static bool BlastsDealtByHost(const UObject* Context);
	/** M4: this machine's player said a taunt: to the other players (OutlawsCoopMatchExtras.cpp). */
	static void NoteTaunt(AOutlawsLevelActor* L, const FString& Wav);
	/** The death screen's button in a match: ask the host for a spawn point. */
	static bool MatchRespawn(AOutlawsPlayer* P);
	static FString MatchDeathPrompt(bool bVR);
	/** BUG-SCOREBOARD: the match's frag table up until closed, or closed (the pause menu's row, Tab on the PC). */
	static void ToggleScoreboard(AOutlawsPlayer* P, const TCHAR* Why);
	/** REQ-SCOREABOVE / REQ-SCORESIDE: while the pause menu (or the score screen at a match's end) is up in a match, the live frag
	 *  table as its own panel to the left of the pause panel; placed each frame by the menu (Centre its middle, RightHint the
	 *  pair's right). Hidden again by the menu closing or no match. */
	static void UpdatePauseScoreboard(AOutlawsPlayer* P, const FVector& Eye, const FVector& Centre, const FVector& RightHint, float PanelWidthCm);
	/** REQ-SCORESIDE: whether the menu shows the table beside it now (a match with a game class); the menu makes room for it. */
	static bool WantsPauseScoreboard(const AOutlawsPlayer* P);
	static void HidePauseScoreboard(AOutlawsPlayer* P);
	/** Closes the table if it is up (the Menu button while it shows); false when nothing was up. */
	static bool CloseScoreboard(AOutlawsPlayer* P, const TCHAR* Why);
	/** The words on the table for closing it: the Menu button in a headset, Tab on the PC. */
	static FString ScoreboardCloseHint(const AOutlawsPlayer* P);
	/** REQ-MPLOBBY (b): this machine hosts a match between matches (the Host Game page sets up the next one: End game, no new
	 *  session); End game pressed there. */
	static bool IsHostBetweenMatches(const UObject* Context);
	static void HostEndGameFor(const UObject* Context);
	/** -OLPauseTidyMode=<sp|cohost|coclient|dm|dmclient> (REQ-PAUSETIDY): the pause page and Players opened in turn, their rows
	 *  logged with whether they scroll, a shot each, Back checked; then (REQ-NOUNSTUCK) the pause page's own rows (no Tools,
	 *  Cheats only once shown) before and after the turn that shows the cheats, a shot of each (ol_pause_<mode>_cheats), the
	 *  Cheats row opened and Back; -OLPauseTidyQuit ends the run after. */
	static void PauseTidyTestTick(AOutlawsPlayer* P, float Dt);
	/** The match's scores as the frag table wants them (every machine: the session's). */
	bool FillMatchTable(struct FFragTableData& Out) const;
	FOutlawsMpHostOptions MatchOptions;

private:
	EOutlawsCoopMode Mode = EOutlawsCoopMode::None;
	bool bInitialized = false;

	// launch flags (-OLCoopHost, -OLCoopJoin=<ip>, -OLCoopAuto, -OLCoopReady, -OLCoopLevel=<name>)
	bool bLaunchPending = false;
	bool bLaunchHost = false;
	FString LaunchJoin;
	bool bAuto = false;                      // ready, start, run, log a summary and quit (tests)
	bool bAutoReady = false;                 // ready and start by themselves (desktop tries), no quitting
	FString LeaveTestWho;                    // -OLCoopLeaveTest[=client|host]: that side leaves 5 s into the level; both check the way home
	bool bMenuTest = false;                  // -OLCoopMenuTest: single player's title menu walks New Game, Multiplayer, Co-op, Join Game (screenshots)
	int32 MenuTestStep = 0;
	FString MenuWalk;                        // REQ-MPMENU: -OLMenuWalk=coop|comp: the launch's host or join pressed through the menu
	int32 MenuWalkStep = 0;
	double MenuWalkAt = 0.0;
	double MenuTestAt = 0.0;
	double EndedAt = 0.0;
	double BothReadyAt = 0.0;
	bool bLobbyShot = false;
	FString LobbyShotPrefix;                 // -OLCoopLobbyShot=<prefix>: a host alone photographs its lobby pages, then quits
	int32 LobbyShotStep = 0;
	double LobbyShotAt = 0.0;
	void TickLobbyShots(double Now, class AOutlawsPlayer* P);
	bool IsTestRun() const { return !LobbyShotPrefix.IsEmpty() || bAuto || bMenuTest || !LeaveTestWho.IsEmpty() || SoloLoadSlot != -100 ||!ExtrasSurveyDir.IsEmpty() || !HorseTestTag.IsEmpty() || CoopBenchSec > 0.f; }
	static void Shot(const FString& Name);
	FString LevelToStart = TEXT("hideout");

	// the session
	FString JoinAddress;                     // typed on the join page (remembered beside the saves)
	FString ConnectingTo;                    // the address being joined
	FString ConnectingPort;                  // ... and its port (R8: a reconnect tries the same)
	bool bJoinFromMenu = false;
	bool bJoinMatchTitle = false;            // REQ-MPMENU: the join page opened from Competitive ("JOIN A MATCH"), else co-op's
	double JoinStartedAt = 0.0;
	FString LanAddress;                      // this machine's address on the local network (shown to the host)
	FString OtherAddresses;                  // its other addresses (a VPN, ZeroTier, a virtual adapter), shown under it
	int32 ListenPort = Port;
	int32 StartedSerial = 0;
	uint32 Version = 1;
	bool bLoggedConnected = false;
	int32 LoggedReady = -1;
	uint32 LoggedReadyBits = MAX_uint32;     // (BUG-READYFLIP: every Ready a lobby page shows, LobbyReadyBits)
	bool bAutoReadySent = false;
	bool bLevelBuilt = false;
	FString BuiltLevel;
	double LevelBuiltAt = 0.0;
	float CoopBenchSec = 0.f;                // (CPU2 b: -OLCoopBench=<s>, the client's bad guys measured; TickCoopBench)
	bool bCoopBenchAlerted = false;
	void TickCoopBench(double Now);
	double LoadingUntil = 0.0;               // (TickLoadTimeout) the long timeout held until then
	float ShortTimeout = -1.f;               //   the driver's own (config) timeout, read once
	double HostStartSeenAt = 0.0;            //   the host saw its new start serial then: its build waits 0.4 s for the clients to hear it
	bool bSummaryDone = false;
	bool bExitRequested = false;
	FString PendingToast;                    // said on the next menu (why a session ended)
	mutable TWeakObjectPtr<AOutlawsCoopSession> SessionCache;
	TWeakObjectPtr<UWorld> SweptWorld;
	FDelegateHandle SweepHandle;
	TSet<FName> LocalOnlyLogged;
	FDelegateHandle NetFailHandle, TravelFailHandle;

	// M2: poses and the other players' puppets (N1: one per other player, by player index)
	double NextPoseAt = 0.0;
	int32 PoseSeq = 0;
	int32 LastPoseSeq[MaxPlayers] = {};
	uint8 LampSent = 0;                          // BUG-LANTERNNET: this player's lantern as last sent, and each other's as last heard
	uint8 LampHeard[MaxPlayers] = {};
	int32 PosesReceived = 0;
	double LastPoseLogAt[MaxPlayers] = {};
	double FirstPoseAt = 0.0;
	TWeakObjectPtr<AOutlawsRemotePlayer> Puppets[MaxPlayers];
	mutable int32 MyIndexCache = -1;             // N1: a client's own number, once its link is here
	int32 TestMaxPlayers = 0;                    // -OLCoopMaxPlayers=<n> (never written to the settings)
	int32 PartyPlayers = 0;                      // -OLCoopPlayers=<n>: the N1 party test waits for that many
	FString PoseRateLogged;                      // N1: the pose rate as last logged (interest management)
	/** N1: this player's pose out at 20 a second, fewer when the nearest other player is far (three or more players only). */
	double PoseInterval(const AOutlawsPlayer* P) const;
	void DestroyPuppets();
	/** N1, the host: the session's lobby words (anyone joined, all ready, how many) worked out again from the links. */
	void RefreshLobby();
	/** N1: the lobby's list of players (three or more): each slot's name and Ready. */
	void LobbySlotLines(TArray<FString>& Out) const;
	int32 ShotStep = 0;
	double ShotAt = 0.0;
	float SeenDistance = -1.f;

	UWorld* CurrentWorld() const;
	AOutlawsLevelActor* FindLevel() const;
	AOutlawsPlayer* LocalPlayer() const;
	void ResetSession();
	void OnLevelBeginPlay(AOutlawsLevelActor* Level);
	void BeginLevel(const FString& Level, int32 Difficulty, int32 Serial, uint8 Kind = 0);
	void OnLevelBuilt(const FString& Level);
	/** The lobby's Start (both ready), or (bAlone, R7) "Start now": the host goes in at once and friends join when they come. */
	void StartGame(bool bAlone = false);
	void SetReady(bool bReady);
	bool MyReady() const;
	/** BUG-READYFLIP: every Ready a lobby page shows, one bit each (this player's, the session's words, each link's). */
	uint32 LobbyReadyBits(const AOutlawsCoopSession* S) const;
	// BUG-READYFLIP's check (-OLCoopAuto -OLCoopTest=readyflip -OLCoopPlayers=3; OutlawsCoopReadyFlip.cpp): the rows shown
	// against what the host counts, after each press
	void TickReadyFlipHost(double Now, AOutlawsPlayer* P);
	void TickReadyFlipClient(double Now, AOutlawsPlayer* P);
	bool OnReadyFlipStep(const FString& Step, int32 Arg);
	void OnReadyFlipNote(const FString& Note);
	void ReadyFlipCheck(bool bOk, const FString& What);
	int32 RFStep = 0, RFK = 0, RFPass = 0, RFFail = 0;
	double RFAt = 0.0, RFStartAt = 0.0;
	FString RFNote;
	int32 RFClientStep = 0, RFClientK = 0;
	double RFClientAt = 0.0, RFClientDeadline = 0.0;
	void CycleDifficulty();
	// REQ-COOPLOBBY: the host lobby's campaign (Main Game or Historical Missions, from LevelToStart) and its one level list
	static bool IsMissionLevel(const FString& Level);
	bool IsHistorical() const { return IsMissionLevel(LevelToStart); }
	void CampaignLevels(bool bHistorical, TArray<FString>& OutNames, TArray<FString>& OutLabels) const;
	/** REQ-HMCAREER: the Historical Missions campaign's levels: Marshal Training's office, then the Handful of Missions. */
	static void HistoricalLevels(const AOutlawsLevelActor* L, TArray<FString>& OutNames);
	static FString CampaignLevelTitle(const FString& Level);
	/** The co-op autosave when it belongs to the lobby's campaign: its level's name and date (the Continue row), else false. */
	bool CampaignAutosave(FString& OutWhat) const;
	void SwitchCampaign();
	void TypeKey(const FString& Key);
	void ReadAddressFile();
	bool bAddressRead = false;               // (read when the join page first opens: single player never touches the network)
	void WriteAddressFile() const;
	/** The address this machine reaches its network from (the route a UDP connect picks; nothing is sent), else the
	 *  likeliest private one; OutOthers: its other IPv4 addresses. */
	static FString FindLanAddress(FString* OutOthers = nullptr);
	void SweepReplication(float DeltaSeconds);
	void HandleNetworkFailure(UWorld* World, UNetDriver* NetDriver, ENetworkFailure::Type FailureType, const FString& ErrorString);
	void HandleTravelFailure(UWorld* World, ETravelFailure::Type FailureType, const FString& ErrorString);
	void Summary(const FString& Why);
	void QuitForTest();
	// M2
	void SendPose(AOutlawsPlayer* P);
	void TickRemote(double Now);
	// the puppet drawn as James Anderson's sprite (user 2026-10-02): made here (the level's GetAtlas and MakeSprite are a
	// friend's to call), owned by the level as a bad guy's is; its atlas (and the remaster's HD look) built with the level
	AOutlawsSpriteActor* MakePuppetSprite(AOutlawsLevelActor* L, const FVector& Feet, float Yaw, const FString& Nwx = FString());
	void WarmPuppetSprite(AOutlawsLevelActor* L);
	double NextPuppetSpriteAt = 0.0;
	bool bPuppetSpriteWarned = false;
	// its test (-OLCoopTest=puppet): the client walks, stops, runs, crouches, crouch-walks, shoots, throws, is hurt, dies and
	// is revived while the host looks at it and photographs each (Saved/OLShots/ol_puppet_<state>.png)
	void TickHostPuppetTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	void OnPuppetTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	bool PuppetTestStandOff(AOutlawsPlayer* P, double Dist);
	int32 PuppetTestSeen = 0, PuppetTestMissed = 0;
	// voice chat (user 2026-10-02; OutlawsCoopVoice.cpp): its state lives in the .cpp (the capture, the codec, the talker)
	TSharedPtr<struct FOutlawsCoopVoice> Voice;
	struct FOutlawsCoopVoice& VoiceState();
	void TickVoice(double Now);
	void ResetVoice();
	bool MicWanted(double Now);
	bool MicPermitted();
	void PumpMic(double Now);
	void SendVoice(const TArray<uint8>& Packet);
	void TickVoiceTalker(double Now);
	FString VoiceSummary() const;
	// its test (-OLCoopTest=voice): a test tone instead of the microphone, the setting switched by the test
	void TickHostVoiceTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	void OnVoiceTestStep(const FString& Step, int32 Arg);
	double PuppetAimUntil = 0.0;                 // the host's eyes follow the client's body till then (still while it moves: no blur)
	float PuppetTestAcross = 0.f;                // the yaw the client last walked across the host's view

	// M3: the shared world's takeables (OutlawsCoopWorld.cpp)
	/** The start serial of the level built here (-1 while one is being built or none is). */
	int32 LevelEpoch() const { return bLevelBuilt ? StartedSerial : -1; }
	TMap<int32, int32> PickupHolder;             // host: pickup -> who has it in a hand (0 host, 1 client)
	TMap<int32, int32> PickupGone;               // host: pickups taken for good in this level, and by whom
	int32 NextGroundId = 100000;                 // host: the number of the next thing made in play
	struct FTakeBefore
	{
		float Health = 0.f, Badge = 0.f;
		int32 Armour = 0, ArmourMax = 0, Gold = 0, Listed = 0;
		TMap<FString, int32> Counts;
		TSet<int32> UseIds;
	};
	struct FTakeUndo
	{
		FString Ground;
		float Health = 0.f, Badge = 0.f;
		bool bArmour = false, bBadge = false;
		int32 Armour = 0, ArmourMax = 0, Gold = 0;
		TMap<FString, int32> Counts;
		TArray<FString> Listed;
		TArray<int32> UseIds;
	};
	FTakeBefore TakeBefore;
	TMap<int32, FTakeUndo> PendingTakes;         // client: taken here, the host's answer not yet in
	TMap<int32, TWeakObjectPtr<AActor>> PendingGround;   // client: its own new ground items waiting for their number, by tag
	TSet<int32> PendingGroundTaken;              // ... of those, the tags taken here before the number came
	int32 NextGroundTag = 0;
	bool bApplyingRemote = false;                // the other machine's change being made here: not reported back
	struct FDeferred { int32 Epoch = 0; TFunction<void()> Do; };
	TArray<FDeferred> Deferred;                  // the other machine's news for a level this one has not finished building
	FOutlawsCoopStats SentStats;
	double NextStatsAt = 0.0;
	int32 LoggedOtherStats[MaxPlayers] = {};
	void ResetWorld();
	void AssignPickupIds(AOutlawsLevelActor* L);
	void RunDeferred();
	/** Do now if Epoch is the level built here; later if that level is still to come; never if it is gone. */
	void RunFor(int32 Epoch, TFunction<void()> Do);
	static int32 PickupIndex(const AOutlawsLevelActor* L, int32 Id);
	static int32 PickupIndexOf(const AOutlawsLevelActor* L, const AActor* A);
	void RemovePickupHere(AOutlawsLevelActor* L, int32 Index);
	void HidePickupHere(AOutlawsLevelActor* L, int32 Index);
	void ShowPickupHere(AOutlawsLevelActor* L, int32 Index, const FVector& At);
	bool Arbitrate(int32 Who, int32 Id, uint8 Kind, FString& OutWhy);
	void SendPickupEvent(int32 Id, EOutlawsCoopPickupEvent Event, const FVector& At = FVector::ZeroVector, int32 Who = 0);
	void UndoTake(AOutlawsLevelActor* L, const FTakeUndo& U);
	void TickWorld(double Now);
	static FString Fingerprint(const AOutlawsLevelActor* L);
	static bool WouldGive(const AOutlawsLevelActor* L, int32 Index);

	// M3 tests (-OLCoopAuto): the host leads, the client follows its steps
	bool bWorldTest = false;                     // -OLCoopAuto unless -OLCoopPosesOnly
	int32 TestStep = 0;
	double TestAt = 0.0;
	int32 TestTries = 0;
	int32 TestPickup = -1;                       // the pickup the step is about
	int32 TestArg = -1;
	FVector TestPickupHome = FVector::ZeroVector;
	FString TestPrint;
	TSet<int32> TestTried;
	TWeakObjectPtr<AActor> TestHeld;
	double TestHeldUntil = 0.0;
	FString TestGrabPrint;                       // client: its things before the same-instant grab
	int32 TestGrabId = -1;
	int32 TestDropId = -1;                       // host: the ground item it made itself
	int32 TestClientDropId = -1;                 // host: the one the client made
	TArray<FString> TestNotes;
	void TickTest(double Now);
	void TickHostTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	void TickClientTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	int32 TestCandidate(AOutlawsLevelActor* L, const FVector& Near) const;
	/** Tests: a pickup moved under this machine's own player for the walk-over to take. */
	bool TestWalkOver(AOutlawsLevelActor* L, AOutlawsPlayer* P, int32 Id);
	void TestSay(const FString& Step, int32 Arg = 0);

	// M4: enemies (OutlawsCoopEnemies.cpp)
	void SetupEnemies(AOutlawsLevelActor* L);
	void TickEnemies(double Now);
	/** This machine's level built again on its own (a death's restart): its enemies and pickups set up for the session again. */
	void CheckRebuilt(AOutlawsLevelActor* L);
	double NextEnemySnapAt = 0.0;
	int32 EnemySnapsSent = 0, EnemySnapsApplied = 0, EnemyHitsSent = 0, EnemyHitsDealt = 0, EnemyDeathsSent = 0, EnemyDeathsSeen = 0, EnemyShotsAtClient = 0;
	bool bEnemyCountWarned = false;
	TArray<FOutlawsCoopEnemySnap> EnemyApplied;  // (CPU2 d, client: each enemy's snapshot as last applied here)
	int64 EnemyPlacesHeld = 0, EnemyApplies = 0; // (CPU2 d: the bench's counts)
	// CPU2 d: -OLEnemyRatesOff (the host sends every place every snapshot), -OLEnemyApplyAll (the client applies every
	// enemy every frame): the befores of a measure
	bool bEnemyRates = true, bEnemyApplyAll = false;
	FString EnemySummary() const;
	// M4 tests
	int32 TestEnemy = -1;
	int32 TestEnemyB = -1;
	int32 TestEnemyLogs = 0;
	void TestLogEnemy(AOutlawsLevelActor* L, int32 Index, const TCHAR* Why);
	bool TestPlaceEnemyByClient(AOutlawsLevelActor* L, AOutlawsPlayer* P, int32 Index);
	int32 TestPickEnemy(AOutlawsLevelActor* L, const FVector& Near, int32 Not, bool bLevelsOwnOnly = false, int32 Not2 = INDEX_NONE, bool bStable = false) const;
	void TickHostEnemyTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	/** Tests: a free spot on this player's floor 1.5 m away, on the side away from the other player (out of both reaches). */
	FVector TestDropSpot(AOutlawsPlayer* P) const;

	// M5: the INF and the level's flow (OutlawsCoopInf.cpp)
	/** The level's INF set up for the session: the client's shows the host's and sends its player's events on; the host's
	 *  tells whose event it runs, and its sounds, messages, locks and wall textures go to the client too. */
	void SetupInf(AOutlawsLevelActor* L);
	void TickInf(double Now);
	/** Host: a start for both (the start serial moves on with the level and how it begins). */
	void RequestStart(EOutlawsCoopStart Kind, const FString& Level);
	void LogInfMove(const AOutlawsLevelActor* L, int32 Index, const FOutlawsCoopInfSnap& Was, const FOutlawsCoopInfSnap& Now);
	static FOutlawsCoopInfSnap InfSnapOf(const FOutlawsElevator& E);
	int32 InfCauser = -1;                        // host: whose event the INF runs (-1 its own timing, 0 the host's player, a client's number (N1: 1 to 7), 100 an enemy)
	int32 InfScopeDepth = 0;
	bool bInfCauserFromScope = false;
	TArray<FOutlawsCoopInfSnap> InfSeen;         // host: as last written; client: as last applied
	TArray<uint8> InfTriggersSeen;
	int32 InfLogLines = 0;
	double NextInfSnapAt = 0.0;
	int32 InfEventsSent = 0, InfEventsRun = 0, InfSoundsSent = 0, InfSoundsPlayed = 0, InfMovesLogged = 0, InfMovesThisLevel = 0;
	bool bStartingLevel = false;                 // the session's own start running: StartNextLevel is let through
	bool bApplyingLevelEnd = false;              // client: the host's level end carried out here
	bool bWaitingForStart = false;               // client: the level is over here, the host's start not come yet
	bool bApplyingSkip = false;                  // host: the client's skip carried out here (not told back)
	FString InfSummary() const;
	// R7: a late join (OutlawsCoopJoin.cpp)
	/** Host: a late joiner's snapshot sent once this machine's level is steady (not between levels, loading or dead). */
	void TickJoin(double Now);
	/** Client: a late join's level begun once the host's snapshot for the current start serial is all here (true: this
	 *  machine is joining late and the lobby's normal start is not for it). */
	bool TickClientJoin(double Now, AOutlawsCoopSession* S);
	/** Host: the reason it cannot send a snapshot now (empty: it can). */
	FString JoinBusyReason() const;
	/** Client: the late join's snapshot applied (TickSaves): the joiner's own kit (player2, else the start kit) and full health. */
	void ApplyJoinKit(AOutlawsLevelActor* L, AOutlawsPlayer* P, const FJsonObject& O, FString& OutHow);
	/** A save's "player2" block (the client's own state as it last told the host) put on this machine's player. */
	static void ApplyPlayer2(AOutlawsLevelActor* L, AOutlawsPlayer* P, const FJsonObject& J);
	/** Host: the other player left mid-level (R7: the host plays on): their hands' bad guys and pickups let go here, their
	 *  puppet gone, the bad guys back on the host alone. */
	void ForgetClient(int32 Who);
	/** N1: host: the next level held until every player in this level has finished its end (20 s at most). */
	FString NextHoldLevel;
	double NextHoldSince = 0.0;
	int32 NextHoldLoggedWaiting = -1;
	int32 NextHoldLastWaiting = -1;              // (the party test: how many were still in the level's end when the last hold let go)
	void TickNextLevelHold(double Now);
	/** REQ-COOPHORSES (OutlawsCoopHorses.cpp): each other player's own horse beside James's, in the slot its link carries
	 *  (the host gives each joiner the lowest free one), made and taken away on every machine as the links come and go. */
	void TickHorses(double Now);
	TMap<int32, TWeakObjectPtr<AActor>> HorseCopies;   // slot -> its horse here
	int32 HorseCopiesBuild = -1;
	int32 HorseLoggedNoPlan = -1;
	TSet<int32> HorseFailed;              // slots whose copy could not be put up this build (tried once)
	/** -OLHorseTest=<tag>: the horses photographed with everyone in, after a leave and after a rejoin; -OLHorseQuitAfter=<s>. */
	FString HorseTestTag;
	float HorseQuitAfter = 0.f;
	int32 HorseTestStep = 0;
	int32 HomOfficeLogged = -1;
	double HorseTestAt = 0.0;
	int32 HorseTestSeen = -1;
	FString HorseSlotsText() const;
	/** N1: client: this player's level end is over (or it had none): told to the host once per level. */
	void SayLevelEndDone();
	int32 EndDoneSent = 0;
	/** R7 for one late joiner (N1: each in turn). */
	void TickJoinFor(double Now, int32 Who);
	// N1's party test (-OLCoopAuto -OLCoopTest=party -OLCoopPlayers=<n>, the host and n - 1 clients; OutlawsCoopParty.cpp):
	// each machine draws every other player; a client's shot, blast and voice reach the others through the host; the bad
	// guys pick among all; one client revives another; all dead restarts the level; a save keeps every client's block
	void TickHostPartyTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	void OnPartyTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	void TestSayTo(int32 Who, const FString& Step, int32 Arg = 0);
	void PartyReport(const TCHAR* Why) const;
	int32 PartyEnemy = -1, PartyVictim = -1, PartyReviver = -1, PartyShooter = -1;
	FString PartyNote;
	FVector PartyHome = FVector::ZeroVector;     // client: where it stood before the test sent it beside a bad guy
	// ---- M1: the match (OutlawsCoopMatch.cpp) ----
	bool bMatchPending = false;                  // host: hosting a match: straight into its map once the listen session is up
	FString LaunchMatchMap;                      // -OLMpHost=<map>
	int32 LaunchMatchFrags = -1, LaunchMatchMinutes = -1;
	int32 LaunchStartWhen = -1;                  // -OLMpStartWhen=<n>: the match starts when n people are in (the host included; 3 when absent)
	float LaunchHostRespawn = 0.f;               // -OLMpHostRespawn=<s>: an unattended host back up s seconds after it dies (a dead host holds late joiners)
	double HostDeadAt = 0.0, HostRespawnTryAt = 0.0;
	bool bLaunchRotate = false;                  // -OLMpRotate: each next match on another stock map (a random map each match), the same settings
	bool bLaunchRandomMap = false, bLaunchRandomClass = false;      // -OLMpRandomMap / -OLMpRandomGame: a launched host's "Random each match"
	bool IsHostFlowOwner() const { return true; }      // (the flow is the one host's; a client's is never begun)
	bool bMatchSetUp = false;                    // host: the rules read the map's spawn points for the level of MatchSerial
	int32 MatchSerial = -1;
	FOutlawsDeathmatch Dm;                       // host: the rules
	TSharedPtr<class FOutlawsMatchTableSource> MatchTable;
	TWeakObjectPtr<class AOutlawsFragTablePanel> MatchPanel;
	TWeakObjectPtr<class AOutlawsFragTablePanel> PausePanel;     // (REQ-SCOREABOVE) the table above the pause panel
	// -OLScoreAboveTest (a bots match, any machine): the pause menu opened and closed with the panel above it checked
	void TickScoreAboveTest(double Now);
	int32 SaTestStep = 0;
	double SaTestAt = 0.0;
	FString SaTestFirst;
	bool ScoreSideOk(const AOutlawsPlayer* P) const;                // (REQ-SCORESIDE) the -OLMpNextTest check of the score screen's table
	FString ScoreSideSays(const AOutlawsPlayer* P) const;
	TWeakObjectPtr<class UStaticMeshComponent> SaTestWall;      // (BUG-SCOREOCCLUDE) the test's wall before the eyes
	double MatchPanelUntil = 0.0;
	bool bMatchPanelHeld = false;                // (BUG-SCOREBOARD) toggled up: no timer, closed by the same input, a respawn or the next phase
	void HideMatchPanel(const TCHAR* Why);
	uint8 LoggedMatchState = 255;
	double LastMatchTickAt = 0.0;
	int32 LastAttacker[MaxPlayers] = {};         // host: who last struck each player (for the kill), and when
	double LastAttackAt[MaxPlayers] = {};
	bool LastAttackBlast[MaxPlayers] = {};       // host: that strike was dynamite (REQ-KILLFEED: "blew up")
	bool MatchDead[MaxPlayers] = {};             // host: each player's death as last seen
	bool MatchInRules[MaxPlayers] = {};          // host: each player added to the rules
	int32 MatchHitsSent = 0, MatchHitsDealt = 0, MatchHitsRefused = 0, MatchRespawns = 0;
	void TickMatch(double Now);
	// ---- REQ-MPLOBBY (b): the match's end and the next match (OutlawsCoopNextMatch.cpp) ----
	/** Whether a match's end goes to the score screen and the host's next match (off where a test runs the old round, -OLCoopTest=dm). */
	bool NextMatchCycleOn() const { return !bLobbyRuleOff; }
	/** host: a launched host sets the next match up itself (-OLMpHost without a test that plays the host's pages). */
	bool HostSetsNextItself() const { return !LaunchMatchMap.IsEmpty() && NextTest != TEXT("start") && NextTest != TEXT("end") && NextTest != TEXT("idle"); }
	/** host: the flow ended this cycle's match: the session says so (the score screen everywhere, the joiners' lobby locked). */
	void TickNextMatchHost(double Now, AOutlawsCoopSession* S);
	/** every machine: the fade to black at the match's end, the score screen, Continue, the lobby reopened over the next level,
	 *  the joiner's 3-minute wait for the host's next match; the tests' presses. */
	void TickNextMatch(double Now, AOutlawsCoopSession* S, AOutlawsPlayer* P);
	/** host: the next match with these options (the Host Game page's Start, or the launched host's own): the random rows rolled,
	 *  everyone back in the lobby, the map started for every machine (the co-op start the clients follow). */
	bool StartNextMatch(const FOutlawsMpHostOptions& Options, const TCHAR* Who);
	/** host: End game between matches: every joiner to its main menu with "The host ended the game", then this host's too. */
	void HostEndGame(const TCHAR* Why);
	/** this player's Continue on the score screen: a joiner to the lobby (Enter greyed out until the host starts the next match),
	 *  the host to the Host Game page with the last match's settings. */
	void ScoreContinue(AOutlawsPlayer* P);
	void BuildScorePage(AOutlawsPlayer* P, OutlawsMenu::FPage& Page);
	void ScoreLines(TArray<FString>& Out) const;
	/** "Death Match on Dry Gulch" with what was rolled ("(random map)"), from the session. */
	FString MatchLine() const;
	static FString MatchLineFor(uint8 Class, const FString& MapFile, uint8 RandomBits);
	void GoToNextLobby(AOutlawsPlayer* P, const TCHAR* Why);
	void TickNextTest(double Now, AOutlawsCoopSession* S, AOutlawsPlayer* P);
	void LeaveForHostEnd(const TCHAR* Why);
	int32 EndedCycle = -1;                       // host: the cycle whose match's end was said
	double BetweenSince = 0.0;                   // host: when the match ended (the launched host's own next match 10 s on)
	double HostEndAt = 0.0;                      // host: End game pressed: its own session ends then (the joiners told first)
	int32 SeenCycle = -1;                        // every machine: the cycle whose end this machine has shown
	uint8 ScorePhase = 0;                        // every machine: 0 none, 1 fading to black, 2 the score screen up
	double ScoreAt = 0.0;                        // when the fade ends and the score screen comes up
	bool bNextLobbyArmed = false;                // this player is back in the lobby for the next match (it reopens over the level until Enter)
	double WaitSince = 0.0;                      // joiner: since when it waits for the host's next match
	float NextTimeout = 180.f;                   // -OLMpNextTimeout=<s>: the joiners' wait for the host's next match (decision 3)
	FString NextTest;                            // -OLMpNextTest=start|end|idle (the host's pages), client (a joiner's presses)
	double NextTestAt = 0.0;
	int32 NextTestStep = 0;
	FString NextShotTag;                         // -OLMpNextShot=<tag>: the score screen, the locked lobby, the Host Game page photographed
	double NextMenuTryAt = 0.0;
	/** -OLMpRandomTest: random map, random game and both rolled on the real map list, each roll checked, then quit. */
	void RunRandomTest();
	/** A #coop page as drawn (the extent of its lines and rows): within the parchment, above the help line. */
	static bool PageFits(AOutlawsPlayer* P, FString& Said);
	void SetUpMatchLevel(AOutlawsLevelActor* L);
	void OnMatchHit(int32 Shooter, int32 Epoch, int32 Victim, float Damage, const FVector& At, const FVector& From, bool bBlast = false);
	// ---- REQ-KILLFEED (OutlawsCoopKillFeed.cpp) ----
	/** The host: a death's kill line to every machine (and this one). */
	void SayKill(int32 Killer, int32 Victim, EOutlawsObit Kind);
	/** Every machine: a death worded for this machine's player, into its kill feed. */
	void OnMatchKill(int32 Killer, int32 Victim, uint8 Kind, const FString& KillerName, const FString& VictimName);
	TArray<FString> KillLinesSeen;               // (the test's: every line shown here)
	void TickHostKillFeedTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	void OnKillFeedTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	bool KillFeedTestCheck(const FString& Want);
	int32 KfBot = -1, KfClient = -1, KfFails = 0, KfTries = 0;
	// ---- BUG-REMOTEGUNSOUND / BUG-TNTDEATHFREEZE test (-OLRemoteFxTest; OutlawsCoopRemoteFxTest.cpp) ----
	void TickHostRemoteFxTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	void OnRemoteFxTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	bool RemoteFxCheck(bool bOk, const FString& What);
	/** The sound a character's revolver makes (its ITM's FIRE_SOUND_1; James's and any without its own: pistol2). */
	FString RemoteFxExpectedPistol(const FString& Short) const;
	TMap<int32, FString> RfHeard;                // (the test's: the sound of the last shot of each player shown here)
	TMap<int32, FString> RfFlash3D;              // (REQ-MUZZLEFLASH3D: the kind of 3D flash each player's last shot showed here)
	int32 RfBot = -1, RfClient = -1, RfFails = 0, RfHits0 = 0, RfBlasts0 = 0;
	// ---- REQ-FLICKRELOAD test (-OLFlickNetTest; OutlawsCoopFlickTest.cpp) ----
	void TickHostFlickNetTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	void OnFlickNetTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	bool FlickNetCheck(bool bOk, const FString& What);
	/** Each player's barrel breaks and shells seen here (opens, shuts, shells; the test's). */
	TMap<int32, FIntVector> BreakSeen;
	/** (REQ-RELOADSLICES) the reload slices heard here, as letters (O open, S shell, C shut, X the empty click, R the whole reload). */
	static FString FlHeard(bool bReset);
	int32 FlCuts0 = 0;                        // (the level's SoundRunCuts at the last look)
	int32 FlClient = -1, FlFails = 0;
	void OnMatchRespawnAsk(int32 Who);
	void OnMatchCharacter(int32 Who, const FString& Short);
	void OnMatchRespawnHere(int32 Epoch, const FVector& At, float Yaw, const FString& Character);
	void OnMatchMessage(const FString& Text, float Seconds);
	void SayToAll(const FString& Text, float Seconds);
	void PublishScores();
	void ShowMatchPanel(float Seconds);
	FString MatchSummary() const;
	// -OLMpTest=dm (OutlawsCoopMatch.cpp): host + 2 clients; warm-up, countdown, a client shoots another, a kill, respawn,
	// the frag limit ends the match on all three
	void TickHostMatchTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	// ---- REQ-MPBOTS: bots in a match (OutlawsCoopBots.cpp) ----
	struct FBot
	{
		int32 Who = -1;
		FString Short;                           // San, Jam, Two, Doc, Bob, Mar
		FString Name;
		TWeakObjectPtr<AOutlawsCoopLink> Link;
		TWeakObjectPtr<AOutlawsEnemy> Brain;     // host only: the bad guy's AI that moves, aims and fires for it (unseen)
		float Health = 10.f;
		bool bDead = false;
		double DiedAt = 0.0;
		double RespawnAt = 0.0;
		int32 Weapon = 0;                        // the player's weapon order: 0 revolver, 1 rifle, 2 shotgun, 3 double barrel, 4 sawed-off
		bool bHas[5] = {};
		int32 Ammo[3] = {};                      // ibullit, icart, ishells
		int32 PoseSeq = 0, StatsSeq = 0;
		double NextLookAt = 0.0, NextChooseAt = 0.0, SeekUntil = 0.0;
		int32 Pickups = 0, ShotsFired = 0;
		double NextDriveAt = 0.0, StillSince = 0.0, NextDebugAt = 0.0, GoalUntil = 0.0;
		FVector Goal = FVector::ZeroVector;      // where it roams when nobody is in sight (a far spawn point)
		int32 Drives = 0;
		bool bDriving = false;                   // BotDrive has it on the move (until it sees someone or stops)
		int32 Side = 1;                          // the way it slides along a wall in its way
		FVector Dest = FVector::ZeroVector;
		FString DestWhat;
		double NextPlanAt = 0.0;
		FVector LastGo = FVector::ZeroVector;    // the way it went last (kept while it is clear: along a wall, not to and fro)
		double ProgressAt = 0.0, ProgressDist = 0.0, CommitUntil = 0.0;
		// the way-finding (BotRoute): the way through the level's sectors to Dest, the point it heads for on it
		TArray<FVector> Route;
		int32 RouteIdx = 0;
		FVector RouteTo = FVector::ZeroVector;
		double RouteLen = 0.0, RouteMadeAt = 0.0, PullAt = 0.0;
		int32 HuntWho = -1;                      // the opponent it hunts when it sees nobody (the nearest; BotHuntTarget)
		int32 DestPickup = -1;                   // the pickup it heads for (its CoopId), -1 none
		double PickupUntil = 0.0;                // kept heading for it until then (BUG-BOTTEAMPLAY: no flip-flop with the hunt)
		TMap<int32, double> AvoidPickup;         // pickups it found no way to, left alone until then
		int32 Sticks = 0, Knives = 0, Throws = 0; // dynamite and knives it carries (its kit, pickups), and how many it has thrown
		double NextThrowAt = 0.0;
		int32 Stucks = 0, StuckRun = 0, Routes = 0, RouteFails = 0, Contacts = 0;
		bool bSawLast = false;
		double UnstickUntil = 0.0;
		FVector UnstickDir = FVector::ZeroVector;
		// the test's measures: ground covered on its own drive, and how much nearer (by the way) it came to whoever it hunted
		FVector LastFeet = FVector::ZeroVector;
		double DriveCm = 0.0, ApproachCm = 0.0, HuntLen = -1.0, HuntSampleAt = 0.0;
		double OwnApproachCm = 0.0;              // (the same by its own way only: from where it was, to where they stand now)
		FVector HuntFeet = FVector::ZeroVector;
		int32 HuntSampleWho = -1;
		int32 TierSeen = 0, FrozenSeen = 0;      // (the throttle: any tier but FULL, or far frozen, seen on its brain)
		FVector ProgressFeet = FVector::ZeroVector;
		// (GATE-FLAKES, the hunt measured on its own as bots11 has it: over each second it spent all on its own drive after
		// whoever it hunts, the ground it covered and how much nearer by the way it came; pickups, far places and fights out)
		double IvDriveCm = 0.0, HuntDriveCm = 0.0, HuntOwnCm = 0.0, HuntNearCm = 0.0;
		bool bIvHunt = true;
	};
	/** The opponent a bot hunts when it sees nobody: the nearest living one not on its team (kept unless another is a third
	 *  nearer); -1 none.  OutFeet: where that one stands. */
	int32 BotHuntTarget(FBot& B, const FVector& Feet, FVector& OutFeet, class AOutlawsLevelActor* L);
	/** Where player Who stands (feet) and whether alive, as the host sees it. */
	bool PlayerFeetOf(int32 Who, FVector& OutFeet) const;
	/** The way-finding: the next place to head for on the way to B.Dest (the sector route, its corners cut where the body
	 *  walks straight on); false: no way through the sectors. */
	bool BotRouteStep(FBot& B, AOutlawsLevelActor* L, const FVector& Feet, double Now, FVector& OutStep);
	/** Nobody in sight: on to a useful pickup, its target or a far place, a step through the level's openings at a time. */
	void BotDrive(FBot& B, AOutlawsLevelActor* L, double Now);
	TArray<FBot> Bots;
	int32 LaunchBots = -1, LaunchBotSkill = -1;  // -OLMpBots=<n> -OLMpBotSkill=<0..2>
	int32 BotPickups = 0, BotRespawns = 0;
	int32 BotPickupTrips = 0;                    // (the test: the times a bot set out for a pickup it had a way to)
	FBot* FindBot(int32 Who);
	bool IsBot(int32 Who) const;
	int32 HumanCount() const;
	void TickBots(double Now, AOutlawsLevelActor* L);
	void AddBot(AOutlawsLevelActor* L);
	void RemoveBot(int32 Who, const TCHAR* Why);
	void RemoveAllBots(const TCHAR* Why);
	/** A human is joining a full match: the last bot leaves to make the slot (OnPlayerLogin). */
	void BotsMakeRoom();
	/** The rules' respawn for a bot: a fresh brain at the spawn point, the character's start kit. */
	void BotSpawn(int32 Who, const FVector& At, float Yaw);
	void BotHurt(int32 Who, float Damage, const FVector& At, const FVector& From, int32 Shooter);
	void BotPublish(FBot& B, double Now);
	void BotApplyWeapon(FBot& B);
	void BotChooseWeapon(FBot& B, const TCHAR* Why);
	void BotLookForPickups(FBot& B, AOutlawsLevelActor* L, double Now);
	/** A stick of dynamite or a knife thrown at whoever it sees, when it carries one and they stand at the right range; the
	 *  thing's flight and its end (a blast, a knife in a body) on the host, the flight drawn on every machine. */
	void BotThrow(FBot& B, AOutlawsLevelActor* L, double Now);
	void TickBotThrown(AOutlawsLevelActor* L, double Dt);
	int32 BotThrowsMade = 0, BotThrowHits = 0;
	int32 BotThrowClear = 0;                     // (the test: chances with someone at its range and a clear arc, thrown or not)
	int32 BotThrowsBy[2][2] = {}, BotHitsBy[2][2] = {};  // (the test: sticks [0] and knives [1], at one standing [0] or moving [1])
	FString BotLine(const FBot& B) const;
public:
	/** The brain's shot (AOutlawsEnemy::FireAtPlayer): seen and heard on every machine, the ammunition spent. */
	void BotFired(AOutlawsEnemy* E, const FVector& Muzzle);
	/** One of its pellets through its target's body: a hit in the match, judged by the host as a player's is. */
	void BotPellet(AOutlawsEnemy* E, float Damage, const FVector& At, const FVector& From);
	/** REQ-MPBOTS: while a bot's stick goes off on the host, the player credited with what the blast does (else -1). */
	static int32 BlastShooter;
	/** ... and what that blast would deal the host's own player (Explode leaves it to the match, credited to the bot). */
	static float BlastHostDealt;
	/** ... and the other players that blast struck (BlastAtPlayers), a bit each. */
	static uint32 BlastStruckMask;
	/** Another player is this bot's opponent: not itself, and in a team class not on its team. */
	bool BotFoe(int32 Bot, int32 Other) const;
private:
	// -OLCoopTest=bots (OutlawsCoopBots.cpp): the host with bots for two minutes; a client joining sees and fights them
	void TickBotTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	double BotTestAt = 0.0, BotTestLiveAt = 0.0, BotTestDeadAt = 0.0, BotTestNextReport = 0.0;
	int32 BotTestStep = 0;
	struct FBotSeen { FVector Last = FVector::ZeroVector; bool bHave = false; double MovedCm = 0.0, MaxStep = 0.0, DeadAt = -100.0; int32 Jumps = 0, Teleports = 0, Respawns = 0; bool bWasDead = false; };
	TMap<int32, FBotSeen> BotSeen;              // (the client's look at the bots it draws: how far and how smoothly they move)
	// -OLCoopTest=lanternswim (REQ-LANTERNSWIM, OutlawsCoopVerbs.cpp): each player in turn carries a lit lantern into the water;
	// the other machine logs the hands it hears in the pose
	void TickLanternSwimTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	int32 LanternTestStep = 0, LanternTestFails = 0;
	double LanternTestAt = 0.0;
	FString LanternTestSeen;                    // (the partner's hands as last logged)
	bool bLanternTestSawHeld = false, bLanternTestSawGone = false;
	// -OLCoopTest=climbwheel (REQ-CLIMBWHEEL, OutlawsClimbWheel.cpp): the host, then the client, hangs by the left hand at the
	// townex snapshot spot and picks a gun into the right from the wheel; the other machine checks the pick in the pose and
	// the hanging body held still
	void TickClimbWheelTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	// -OLCoopTest=zonegate on the mill (BUG-MILLCLIMB, OutlawsCoopZoneGate.cpp): the host walks in through the mill's gate and
	// stands in the hall, the client stays in the yard and stands on its ground under the hall; each checks its own side
	void TickZoneGateTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	int32 ZgStep = 0, ZgPasses = 0, ZgFails = 0;
	double ZgAt = 0.0;
	// -OLCoopTest=jumppick | jumppicklate with -OLJumpPickSpot=pX_Y_Z_P_Q on both (BUG-JUMPPICK, OutlawsCoopJumpPick.cpp): the
	// client starts on sector P's floor at X Y Z (a catwalk, pier or tunnel floor sharing space with Q), or the host goes in
	// alone, stands there and the client joins late beside it; the client checks the sector it is put in and its footing
	void TickJumpPickTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	bool JumpPickSpot(FVector& OutOl, int32& OutP, int32& OutQ) const;
	int32 CwStep = 0, CwPasses = 0, CwFails = 0;
	double CwAt = 0.0, CwSeenAt = -1.0;
	FVector CwHome = FVector::ZeroVector, CwHangAt = FVector::ZeroVector, CwSeenBody = FVector::ZeroVector;
	bool bCwArmed = false, bCwWatchDone = false;
	// -OLCoopTest=lanternnet (BUG-LANTERNNET, OutlawsCoopLanternNet.cpp): the host, then the client, lights a lantern, puts it
	// away by the wheel, lights it again and swims (the swim's put-away); the other machine checks its copy of the light each
	// time (in a slot at the holder's flame, the holder's reach and colour; gone once put away) and counts its lights
	void TickLanternNetTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	bool LanternNetObserve(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	int32 LnStep = 0, LnFails = 0, LnPasses = 0, LnSeen = 0, LnSlot = -1;
	double LnAt = 0.0, LnSince = 0.0;
	uint8 LnLamp = 0;
	FVector LnDry = FVector::ZeroVector;
	// -OLCoopTest=cheatscope (REQ-CHEATSCOPE, OutlawsCoopCheatScope.cpp): the host and its client each use the all-weapons
	// cheat on their own machine (each keeps its own pockets, a pickup's local half is the same): no scope before, the scope
	// owned and the rifle zooming after
	void TickCheatScopeTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	int32 CsStep = 0, CsFails = 0, CsPasses = 0;
	double CsAt = 0.0;
	// -OLCoopTest=skipinv (BUG-SKIPLEVELINV, OutlawsCoopSkipInv.cpp): the host and its client each fill their pockets (every weapon,
	// some armour); the host skips to another level by the menu's row (MenuLevelChange, the Jump start); each machine's pockets
	// are the same after it
	void TickSkipInvTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	int32 SiStep = 0, SiFails = 0, SiPasses = 0;
	double SiAt = 0.0;
	FString SiFull;
	// -OLCoopTest=levelcarry (BUG-LEVELCARRY, OutlawsLevelCarryTest.cpp): the host and its client each tired with part-loaded guns
	// in the hideout; the host starts the next level for both (the NextLevel start); each machine's player starts it rested and
	// loaded from its own carried rounds
	void TickLevelCarryTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	int32 LcStep = 0, LcFails = 0, LcPasses = 0;
	double LcAt = 0.0;
	// -OLCoopTest=piano (REQ-PIANO, OutlawsPianoTest.cpp) in townex: the host presses the piano's keys, the client hears the tune and
	// sees the roll scroll, the client's press inside the lockout is refused by the host, its press after it plays the tune again
	void TickPianoTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	int32 PnStep = 0, PnFails = 0, PnPasses = 0, PnStarts0 = 0, PnRefused0 = 0;
	double PnAt = 0.0, PnTuneAt = 0.0;
	FVector2D PnRoll = FVector2D::ZeroVector;
	// -OLCoopTest=allowcheats (BUG-ALLOWCHEATS, OutlawsCoopCheatScope.cpp) in a match (-OLMpCheats=0 or 1): the client's cheat
	// asked of the host, its menu page; the host's own cheat; -OLCoopTest=allowcheats is shared with cheatscope's tick
	void TickAllowCheatsTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	int32 AcStep = 0, AcFails = 0, AcPasses = 0;
	double AcAt = 0.0;
	int32 AcReply = -1;
	bool AcClientDone = false;
	int32 AcBaseRifle = 0;
	// -OLCoopTest=unlock -OLCoopLevel=ranch (REQ-UNLOCKS, OutlawsUnlocksTest.cpp): each machine starts locked (the host's lobby
	// options without the Bad guys row and the Randomizer, a saved multiplier taken as Auto); the host wins the story's last
	// level and skips the final movie; the client watches it until that skip ends the session (the story's end ends it for both),
	// and each player's own profile earns (the script reads the store)
	void TickUnlockTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	void EndUnlockTestAtSessionEnd(AOutlawsLevelActor* L);   // (the client, on the host's quit)
	int32 UlStep = 0, UlFails = 0, UlPasses = 0;
	double UlAt = 0.0;
	// -OLCoopTest=bossend -OLCoopLevel=ranch (BUG-RANCHDOOR, BUG-DRDEATHANIM; OutlawsCoopBossEnd.cpp): the client uses Bob's
	// door with the key; each machine checks its movie starts and the door stays shut until the movie covers its level, and
	// is open after; then the host shoots Bob down and each checks no death of his is played and the level ends
	void TickBossEndTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	// -OLCoopTest=float (REQ-ENEMYFLOAT, OutlawsEnemyFloatTest.cpp) in the canyon: the host drops a bad guy into the water; the
	// host's copy and the client's each show him floating at the line
	void TickFloatTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	// -OLCoopTest=bosswall (BUG-BOSSWALL, OutlawsBossWallProbe.cpp) in the town: the host puts the boss in the boardwalk
	// doorway where snapshot 5 had him pressed on the frame; the host's copy and the client's each get off it
	void TickBossWallTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	int32 EfStep = 0, EfFails = 0, EfPasses = 0;
	double EfAt = 0.0;
	int32 BeStep = 0, BeFails = 0, BePasses = 0, BeDoor = -1, BeVisibleMoves = 0;
	double BeAt = 0.0, BeDoorMoved = 0.0, BeDoorStart = 0.0;
	bool bBeEnded = false, bBeHeldSeen = false;
	int32 BeBobDieChors = -1;
	TWeakObjectPtr<class AOutlawsEnemy> BeBob;
	void OnMatchTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	int32 MatchTestShooter = -1, MatchTestVictim = -1, MatchTestShots = 0;
	// ---- M2: the modes (OutlawsCoopModes.cpp) ----
	FOutlawsMatchModes Modes;                    // host: the five classes' rules
	TMap<int32, TWeakObjectPtr<AOutlawsSpriteActor>> ModeSprites;      // every machine: the items drawn (key: item, 100 the IT marker)
	TMap<int32, FString> ModeSpriteNwx;
	int32 LoggedModeItems = -1;
	double NextModeLogAt = 0.0;
	void SetUpModes(AOutlawsLevelActor* L);
	void TickModesHost(double Now, float Dt);
	void TickModeSprites(double Now);
	void PublishModes();
	void OnMatchTeam(int32 Who, int32 Team);
	/** The rules' word that someone won: the flow ends the match. */
	void ModeWon(const FString& Winner, const FString& Why);
	/** The frag table's columns and rows for the class (M2: CAPT, TIME, the team scores). */
	void FillModeColumns(struct FFragTableData& Out) const;
	// -OLMpTest=modes (OutlawsCoopModes.cpp): each class's own checks, the host and two clients
	void TickHostModesTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	void OnModesTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	int32 ModeTestA = -1, ModeTestB = -1, ModeTestTries = 0;
	FString LaunchMatchClass;                    // -OLMpClass=<1..6 or a word>
	bool bLaunchMatchTeams = false;
	int32 LaunchMatchCheats = -1;               // -OLMpCheats=0|1 (BUG-ALLOWCHEATS: the Host Game page's Allow Cheats row for a launched match)              // -OLMpTeams (Secret Document red against blue)
	float LaunchScoreSeconds = 0.f;              // -OLMpScoreSeconds=<n> (the timed classes' limit, for a test)
	// M1's gaps (OutlawsCoopMatchStrikes.cpp)
	float MatchDamageFor(int32 Victim, float Damage) const;
	bool ReportStrike(AOutlawsLevelActor* L, int32 Victim, float Damage, const FVector& At, const TCHAR* Kind);
	/** The first other player a blow along A..B (a point when A is B) meets: within Reach of the body or in the picture
	 *  drawn here; -1 none.  OutAt the point. */
	int32 PlayerStruckAlong(const FVector& A, const FVector& B, float Reach, FVector& OutAt) const;
	/** That player struck (once in StrikeCoolSeconds for one blow going through) and reported. */
	bool StrikePlayer(AOutlawsLevelActor* L, int32 Who, float Damage, const FVector& At, const TCHAR* Kind);
	void TickHostStrikeTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	void OnStrikeTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	void OnStrikeExtraStep(const FString& Step, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	void TickMatchItems(double Now);                 // (host: the map's pickup generators' items out and back)
	int32 MatchItemSerial = -1;
	TArray<TWeakObjectPtr<AActor>> MatchItemActors;
	TArray<bool> MatchItemListed;
	// M4 (OutlawsCoopMatchExtras.cpp): spectating while dead, and the taunts heard
	void TickSpectate(double Now);
	void SpectateNext(const TCHAR* Why);
	void StopSpectating(const TCHAR* Why);
	void OnTaunt(int32 Who, const FString& Wav);
	TWeakObjectPtr<class ACameraActor> SpectateCam;
	int32 SpectateWho = -1;
	double DeadSince = 0.0;
	bool bSpectateGrip = false;
	// BUG-MPDEATHCAM: the death view's hold, watched (where it began, how far the eye went, whether it left this player)
	FVector DeathViewAt = FVector::ZeroVector;
	double DeathViewDrift = 0.0, DeathViewPending = 0.0, DeathViewPendingAt = 0.0;
	bool bDeathViewLeft = false;
	int32 DeathViewsHeld = 0, DeathViewsMoved = 0;
	// -OLScoreboardTest (a bots match, the host): the table against a wall, toggled, closed by a respawn
	void TickScoreboardTest(double Now);
	int32 SbTestStep = 0;
	double SbTestAt = 0.0;
	int32 MatchTauntsHeard = 0;                   // (each item: a pickup in the list, or apart from it as dynamite is)
	int32 MatchItemsBack = 0;
	// -OLCoopTest=fuzz (OutlawsCoopGuard.cpp): the M1 hardening's test
	void TickHostFuzzTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	void OnFuzzTestStep(const FString& Step, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	int32 FuzzPosesBefore = 0;
	int32 PartyReportWant = -1;                  // client: the host's "report": how many others it must draw (-1: none asked)
	double PartyReportAskedAt = 0.0;
	void TickClientPartyTest(double Now);
	// (the voice chat's own state lives in OutlawsCoopVoice.cpp; what the party test reads of it)
	int32 VoiceReceivedFrom(int32 Who) const;
	int32 VoiceRelayed() const;
	void SetVoiceTestMic(int32 On);
	// R8: connecting and the pause menu's Co-op page (OutlawsCoopConnect.cpp)
	FString JoinError;                           // the join page: why the last join from it failed ("Could not reach ...", "Refused: ...")
	TArray<FString> RecentAddrs, RecentNames;    // the join page's Recent list (coop_recent.txt beside the saves), newest first
	bool bRecentRead = false;
	void ReadRecent();
	void NoteRecent(const FString& Address, const FString& Name);
	FString InternetAddress;                     // host: its address on the internet, asked once per hosting ("unknown" if it failed)
	bool bInternetAsked = false;
	void AskInternetAddress();
	/** The host lobby's Show internet address (user 2026-10-06): asked only once the host presses it, then remembered on this
	 *  machine ("CoopShowInternetAddress" in OutlawsSettings.ini) so its later hostings show it at once; Hide forgets it.
	 *  -OLCoopNetAddr presses it for a test (never written to the settings). */
	bool bShowInternet = false;
	bool bTestNetAddr = false;
	void SetShowInternet(bool bOn);
	// ---- REQ-UPNP (v1.1): the host's router asked to forward the game's UDP port (OutlawsCoopUpnp.cpp, OutlawsUpnp.cpp) ----
	enum class EUpnp : uint8 { Idle, Off, Skipped, Asking, Opened, Refused };
	EUpnp UpnpState = EUpnp::Idle;
	FString UpnpExternal;                        // the router's own external address (GetExternalIPAddress), empty if none
	FString UpnpWhy;                             // why it failed, in the log's words
	TSharedPtr<struct FOutlawsUpnpState, ESPMode::ThreadSafe> Upnp;     // the gateway, the mapping, the worker's jobs
	/** host, once listening: asks the router (on a worker thread, 3 s at most) unless Options > Multiplayer's row is off. */
	void StartUpnp();
	/** the mapping removed (Leave, End Game, the session's end): on a worker, or at once (quit: bWait, 3 s at most). */
	void StopUpnp(bool bWait);
	void TickUpnp(double Now);
	bool UpnpBusy() const;
	void InitUpnpTests();
	/** the host lobby's "Internet: ..." lines. */
	void UpnpLines(TArray<FString>& Out) const;
	/** the address Show internet address shows: the router's own when UPnP gave a public one, else the ipify answer. */
	FString ShownInternetAddress() const;
	bool UpnpShared() const;
public:
	/** Options > Multiplayer "Open my router's port automatically" ("MpUpnp" in OutlawsSettings.ini, on by default). */
	static bool UpnpSetting();
	static void ToggleUpnp();
private:
	bool bBuildSent = false;
	void TickBuildLine();
	/** (user 2026-10-04) the connection timeout: the short one (DefaultEngine.ini, 12 s) in play, 60 s while any machine
	 *  builds a level (a start serial some connected player's link has not reported built: the first level, a level
	 *  change, a late joiner's load), back to the short one 3 s after the last report. */
	void TickLoadTimeout(double Now, class AOutlawsCoopSession* S);
	float PingMs() const;
	// ---- REQ-MPLOBBY (landing a): the shared lobby and its start rule (OutlawsCoopLobby.cpp) ----
	/** host: the start rule for the match of this warm-up; off for a test that starts the match itself (-OLCoopTest=dm). */
	FOutlawsLobbyStart Lobby;
	bool bLobbyRuleOff = false;
	/** host: a team picked in the lobby (Join Red / Join Blue), put on the player once the match's rules have it (0: none). */
	uint8 LobbyTeam[8] = {};
	/** this player pressed Enter Game (a joiner then joins the host's level; Enter is one-way). */
	bool bEnterPressed = false;
	/** per-player mute (the lobby's and the pause Players page's rows), one bit per player index; per session, never saved. */
	uint32 MutedBits = 0;
	int8 SentMic = -1;
	double LobbyOpenTryAt = 0.0;
	float LobbyEnterAfter = 0.f;                 // -OLLobbyEnterAfter=<s>: a test's Enter Game that many seconds after its lobby is up
	double LobbyPageAt = 0.0;
	bool bHostLobbyLogged = false;
	FString LobbyShotName;                       // -OLLobbyShot=<name>: this player's lobby photographed (Saved/OLShots) before its Enter
	bool bLobbyShotTaken = false;
	double LobbyMoreAt = 0.0;                    // (REQ-MPLOBBY c: the shot's "More players" pressed, its next page photographed then)
	bool MicSettingOn() const;
	/** The lobby page (#cooplobby) for host and joiners: its rows, its lines, Enter Game. */
	void BuildLobbyPage(AOutlawsPlayer* P, OutlawsMenu::FPage& Page);
	void LobbyLines(TArray<FString>& Out) const;
	void EnterGame(AOutlawsPlayer* P, int32 Team);
	/** Whether this player is still in the lobby (not entered). */
	bool InLobbyHere() const;
	bool IsPlayerEntered(const AOutlawsCoopLink* K) const;
	/** A player in the session, the level started, still in the lobby: not in the level yet.  Its body is drawn nowhere, the
	 *  host's own pawn is out of the bots' way, and no bad guy or bot hunts it (bots never are). */
	bool PlayerInLobby(int32 Who) const;
	/** host: its own pawn out of the others' way while it is in the lobby (its capsule ignores pawns), back once it enters. */
	bool bHostBodyAway = false;
	TWeakObjectPtr<AOutlawsPlayer> HostBodyPawn;
	void TickLobbyBody(AOutlawsPlayer* P);
	/** -OLLobbyEnterAfterPeer: the host's test Enter waits for a joiner in the game (and 5 s more), so the joiner sees its
	 *  body hidden first. */
	bool bLobbyEnterAfterPeer = false;
	double LobbyPeerInAt = 0.0;
	/** host: the lobby opened over the host's first level until it enters; the auto tests' Enter; the mic flag sent. */
	void TickLobby(double Now, AOutlawsCoopSession* S, AOutlawsPlayer* P);
	/** host: the start rule fed from the links (TickMatch, warm-up). */
	void TickLobbyRule(float Dt, AOutlawsCoopSession* S, int32 People);
	bool IsVoiceMuted(int32 Who) const { return Who >= 0 && Who < 32 && (MutedBits & (1u << Who)) != 0; }
	/** -OLLobbyRuleTest: the start rule on simulated time (lone host with and without bots, 2 and 3 players entering at different
	 *  times, a late join, a launched host's start count), each check "OLLOBBY test PASS|FAIL", then quit. */
	static void RunLobbyRuleTest();
	void PausePageLines(TArray<FString>& Out) const;
	void BuildPausePage(AOutlawsPlayer* P, OutlawsMenu::FPage& Page);
	// ---- REQ-MPLOBBY (landing c): the pages fit the panel, per-player mute in pause (OutlawsCoopMenuFit.cpp) ----
	/** A player as the lobby and the pause Players page list them (bots left out; a test's stand-ins included). */
	struct FPlayerRow
	{
		int32 Who = -1;
		FString Name;
		bool bMic = false;
		bool bInGame = false;
		float PingMs = -1.f;
	};
	void PlayerRows(TArray<FPlayerRow>& Out) const;
	int32 BotsInSession() const;
	/** How many rows a compact #coop page (the lobby, the score screen, the Players page) can hold under this many lines,
	 *  the rows no closer than 50 apart, the last above the help line (DrawFrontCanvas's layout). */
	static int32 FrontRowRoom(int32 NumLines);
	/** The player rows put into a page at At: all of them when they fit, else a page of them and a "More players" row that
	 *  shows the next page (PlayerListPage). */
	void AddPlayerRows(OutlawsMenu::FPage& Page, int32 At, TArray<OutlawsMenu::FItem>& Players, int32 NumLines);
	int32 PlayerListPage = 0;
	/** -OLFakePlayers=<n>: a test's stand-in players in the lists (named, half with a microphone), as if that many more were in. */
	int32 FakePlayers = 0;
	FString RecentLabel(int32 i) const;
	// R8: a dropped connection (Wi-Fi, a Quest asleep, a cable): the client tries the same address again every 5 s for two
	// minutes and comes back in through the late join, with the kit the host last knew for it
	FString ReconnectTo;                         // client: the host's address and port to try again (empty: none)
	double ReconnectUntil = 0.0, NextReconnectAt = 0.0;
	int32 ReconnectTries = 0;
	bool bReconnectAttempt = false;              // client: this join is a reconnect (a failure keeps trying)
	bool bBackInPending = false;                 // client: say "Back in" once the late join is applied
	bool bAutosaveHoldLogged = false;            // host: the start autosave's wait said once
	double LastReconnectToastAt = 0.0;
	/** Client: the connection dropped (not the host leaving): try again. */
	void StartReconnect(const FString& Why);
	void TickReconnect(double Now);
	/** R8's dropout tests (-OLCoopAuto -OLCoopTest=outage | rejoin | hostkill; OutlawsCoopJoin.cpp): a dropped player
	 *  comes back with its kit, a killed host leaves the client at its title menu with the reason. */
	bool IsDropoutTest() const { return TestOnly == TEXT("outage") || TestOnly == TEXT("rejoin") || TestOnly == TEXT("hostkill"); }
	// BUG-COOPTIMEOUT (OutlawsCoopPauseHold.cpp): the Quest pauses a client's app (its menu, the headset off, a sleep).
	// The client says so as it goes and again as it comes back; the host holds its place for ol.CoopPauseGrace (120 s)
	// instead of timing it out after 12 s.  Back with no answer from the host in ol.CoopResumeAckSeconds, the client
	// reconnects at once to the same host and takes its place back through the late join.
	struct FPauseHold
	{
		double Since = 0.0, Until = 0.0;         // host: when the notice came, and when the hold ends
		int32 SavedSpeed = 0;                    // host: the connection's net speed before the hold (restored after)
		int32 PacketsAtStart = 0;                // host: packets sent to it when the hold began (logged after)
		double NextLogAt = 0.0;
		bool bOver = false;                      // host: the grace ran out (the normal timeout from there)
	};
	FPauseHold PauseHolds[MaxPlayers];
	bool LoggedAppPaused[MaxPlayers] = {};               // every machine: each player's paused flag as last said
	FDelegateHandle AppPauseHandle, AppResumeHandle;
	bool bAppPausedHere = false;                 // client: this machine's app said it was pausing
	double AppPausedAt = 0.0, AppResumedAt = 0.0; // client: (wall clock) the pause and the return
	double ResumeAckAt = 0.0;                    // client: when the host's answer came (0: none since the return)
	bool bAwaitAck = false;                      // client: back, waiting for the host's answer
	int32 PauseHoldLogouts = 0;                  // host: players who left (a test counts them)
	void BindAppPause();
	void UnbindAppPause();
	void OnAppWillPause();
	void OnAppResumed();
	void AppWillPause(const TCHAR* Why);
	void AppResumed(const TCHAR* Why);
	void OnPauseNotice(int32 Who, bool bPausing);
	void OnResumeAck(int32 HeldSeconds);
	void TickPauseHold(double Now);
	void EndPauseHold(int32 Who, const TCHAR* Why);
	bool IsPauseHeld(int32 Who) const;
	// BUG-HOSTPAUSE: the host's own app pausing.  It tells every client as it goes and again as it comes back; each client
	// holds the game for the host's grace: its connection kept from timing out, its world held still, "Host paused" shown.
	// A host gone quiet with no notice times out as before (the engine's 12 s, then the reconnect).
	struct FHostPauseHold
	{
		double Since = 0.0, Until = 0.0, NextLogAt = 0.0;
		int32 SavedSpeed = 0;
		int32 PacketsAtStart = 0;
		bool bOver = false;
	};
	FHostPauseHold HostHold;                      // client: the host said its app was pausing
	double HostHoldEndedAt = 0.0;                // client: when the last hold ended by the host's return
	float HostHoldLasted = 0.f;
	int32 HostHoldsSeen = 0;
	void OnHostPauseNotice(bool bPausing, float Grace);
	void EndHostPauseHold(const TCHAR* Why, bool bHostBack);
	void TickHostPauseHold(double Now);
	void SetHostPauseFreeze(bool bOn);
	bool bHostPauseFrozen = false;
	int32 TellClientsHostPause(bool bPausing, float Grace);
	/** Its tests: -OLCoopTest=hostpause | hoststall, the host with -OLCoopPauseSec=<s> (the host's pause, notice or none). */
	bool IsHostPauseTest() const { return TestOnly == TEXT("hostpause") || TestOnly == TEXT("hoststall"); }
	void TickHostPauseTest(double Now);
	/** Its tests (-OLCoopAuto -OLCoopTest=pausehold | resumerejoin; the client with -OLCoopPauseSec=<s>). */
	bool IsPauseTest() const { return TestOnly == TEXT("pausehold") || TestOnly == TEXT("resumerejoin") || IsHostPauseTest(); }
	void TickPauseTest(double Now);
	void TickMenuWalk(AOutlawsPlayer* P, double Now);
	void OnPauseTestNote(const FString& Note);
	int32 PauseStep = 0;
	double PauseAt = 0.0, PauseHoldSeenAt = 0.0;
	int32 PauseJoinsBefore = 0, PauseLoggedHeld = 0;
	float PauseTestSec = 30.f;
	bool bPauseDropBacklog = false;
	FString PauseKitBefore;
	void TickDropoutTest(double Now);
	int32 DropStep = 0;
	double DropAt = 0.0;
	int32 DropJoins = 0;
	TSet<int32> JoinAsked;                       // host: the late joiners who said hello; their snapshots are owed (N1: each)
	FString JoinBusyLogged;                      // host: the last "busy" reason logged
	int32 JoinsSent = 0, JoinsApplied = 0;
	bool bJoinHelloSent = false;                 // client: said hello once
	bool bJoinApply = false;                     // client: the snapshot in SnapObj is a late join's (TickSaves applies the join kit)
	// Q2 step 4 (dotard's townex join, 10 Oct: 121 bodies and 265 saved bad guys put back in one frame, 26 new PSOs after):
	// a late join's replay a few bad guys a frame (ol.coop.JoinReplayPerFrame; 0 = all in one frame as before) behind a
	// black hold, the views drawn under it for ol.coop.JoinWarmSec before it fades in (OutlawsCoopJoin.cpp)
	int32 JoinReplayStep = 0;                    // 0 none, 1 placing (TickEnemies), 2 saved states (TickSaves), 3 warming
	int32 JoinReplayEpoch = 0;
	bool bJoinReplayFade = false;
	double JoinReplayReadyAt = 0.0, JoinReplayWarmUntil = 0.0, JoinReplayTailUntil = 0.0, JoinReplayLastTick = 0.0, JoinReplayBuiltAt = 0.0;
	uint64 JoinReplayWorkFrame = 0;
	double JoinReplayWorkMs = 0.0, JoinReplayMaxWorkMs = 0.0, JoinReplayMaxFrameMs = 0.0, JoinReplayMaxWorkFrameMs = 0.0;
	int32 JoinReplayFramesMeasured = 0, JoinReplayPlaced = 0, JoinReplayPlaceFrames = 0, JoinReplayRead = 0, JoinReplayReadFrames = 0;
	int32 JoinReplayBodies = 0, JoinReplayDropNext = 0, JoinReplayDropsMade = 0;
	uint64 JoinReplayLastPlaceFrame = 0, JoinReplayBuiltFrame = 0;
	TArray<TPair<TWeakObjectPtr<class AOutlawsEnemy>, TSharedPtr<class FJsonObject>>> JoinReplayQueue;
	TSharedPtr<class FJsonObject> JoinReplayObj;
	/** The first placements this frame may take (0: no limit, not in a join's replay; -1: none, the level's build frame). */
	int32 JoinPlaceBudget() const;
	/** A piece of the replay's own work timed (for its log's "most work in a frame"). */
	void NoteJoinReplayWork(double Ms);
	/** The replay's frames measured, the warm hold ended (the coop tick, every frame). */
	void TickJoinReplay(double Now);
	/** Its saved bad guys put back: a few this frame; true once none are left. */
	bool DrainJoinReplay(class AOutlawsLevelActor* L);
	void StartJoinReplay();
	void EndJoinReplay(const TCHAR* Why);
	/** The rest of a snapshot put back here (the player's kit, its place), once the world is. */
	void FinishSnapshotApply(class AOutlawsLevelActor* L, class AOutlawsPlayer* P, const TSharedPtr<class FJsonObject>& O, int32 Made, double Now);
	int32 JoinSpawnDifficulty = -1;              // client: the difficulty the host's level was built with (from the snapshot)
	uint8 LoggedJoinState = 0;
	float JoinDelay = 0.f;                       // -OLCoopJoinDelay=<s>: the client's join waits this long (the late-join test)
	double LaunchAt = 0.0;                       // ... until then (0: not armed)
	FString JoinSummary() const;
	// its test (-OLCoopAuto -OLCoopTest=latejoin | latejoinend; the client with -OLCoopJoinDelay)
	void TickHostLateJoinTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	void OnLateJoinTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	FString TestLateJoinPrint(AOutlawsLevelActor* L) const;
	int32 TestLjEnemyA = -1, TestLjEnemyB = -1, TestLjWalker = -1, TestLjPickup = -1, TestLjDoor = -1;
	FVector TestLjWalkerAt = FVector::ZeroVector;
	FString TestLjAutoHash;
	// M7: the co-op rules (OutlawsCoopRules.cpp)
	void TickRules(double Now);
	/** This machine's player has died: logged, and its keys and puzzle pieces left on the ground where it fell. */
	void OnLocalDeath(AOutlawsLevelActor* L, AOutlawsPlayer* P);
	/** The ground item that gives an inventory item (gbrsskey for ibrsskey), from the archives' item files. */
	FString GroundItemFor(AOutlawsLevelActor* L, const FString& Item);
	/** BUG-STUCKGAP: the keys and puzzle pieces in this machine's pockets, one name per piece, sorted. */
	static void KeyItems(const AOutlawsLevelActor* L, TArray<FString>& Out);
	/** BUG-STUCKGAP (host): a leaving player's keys back on the ground at Feet (its last safe spot). */
	void ReturnKeys(AOutlawsLevelActor* L, const TArray<FString>& Keys, const FVector& Feet, int32 Who, const TCHAR* Why);
	// its test (-OLCoopAuto -OLCoopTest=unstuck | unstuckleave; OutlawsCoopUnstuck.cpp)
	void TickHostUnstuckTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	// BUG-BOSSIDLEHOST: each player's last doing, as the host sees it (OutlawsCoopIdle.cpp), and its test
	// (-OLCoopAuto -OLCoopTest=idlehost -OLCoopLevel=townex)
	struct FIdleWatch
	{
		bool bSeen = false;                      // in the level at the last look
		bool bEverActive = false;                // moved, turned, fired or was hit since the level's start
		bool bIdle = false;
		bool bVR = false;
		FVector Anchor = FVector::ZeroVector;    // where it stood when it last counted as moving
		float AnchorYaw = 0.f;
		double LastActiveAt = 0.0;
		double SeenAt = 0.0;                     // when it was first seen in this level (its first moments are its placing)
	};
	FIdleWatch IdleWatch[MaxPlayers];
	int32 IdleSerial = -1;
	void TickIdleWatch(double Now);
	void TickHostIdleTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	void IdleCheck(bool bOk, const FString& What);
	bool IdlePlaceNear(AOutlawsLevelActor* L, AOutlawsPlayer* P, int32 Index);
	int32 IdlePass = 0, IdleFail = 0, IdleEnemy = -1;
	double IdleDeadline = 0.0, IdleWalkAt = 0.0;
	TArray<TWeakObjectPtr<AOutlawsEnemy>> IdleBosses;
	bool OnUnstuckTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	bool bLocalDead = false;                     // this machine's player was dead at the last look
	/** REQ-DEATHDROPHIDE (user 2026-10-09): what this machine's player dropped at its death, kept out of this machine's
	 *  view until it is up again (the ground-level death view looked into it: Sanchez's box of dynamite in his face); the
	 *  others see it, and it is a pickup as ever (only the drawing is held back here, never sent). */
	TArray<TWeakObjectPtr<UPrimitiveComponent>> DeathDropParts;
	int32 DeathDropsHidden = 0, DeathDropsShown = 0;
	void HideDeathDrop(AActor* A);
	void ShowDeathDrops(const TCHAR* Why);
public:
	/** (tests) A thing drawn on this machine (none of its parts held back from the view). */
	static bool DrawnHere(const AActor* A);
private:
	bool bOtherDead = false;                     // (N1: any other player)
	bool OtherDeadAt[MaxPlayers] = {};           // N1: each other player's death as its stats last said
	bool OtherAwayAt[MaxPlayers] = {};
	double BothDeadSince = 0.0;                  // (N1: all dead)
	bool bBothDeadRestart = false;               // host: the restart for both dead is asked
	double ReviveSentAt = 0.0;
	TMap<FString, FString> GroundOfItem;
	int32 KeysDropped = 0, Revives = 0, Respawns = 0;
	// 2026-10-02: a player's pause menu in a session (the game goes on; that player out of harm)
	bool bLocalAway = false;                     // this machine's player is in its pause menu (the host's bad guys leave it be)
	int32 MenusOpened = 0, HurtsDropped = 0;
	double MenuShieldLogAt = 0.0;
	FString RulesSummary() const;
	// 2026-10-02 decisions 1, 2 and 6 (OutlawsCoopRules.cpp)
	/** One medical bag's worth of healing points (the ground bag's COUNT, gdocbag.itm: 3); read once. */
	int32 MedBagPoints(AOutlawsLevelActor* L);
	int32 BagPointsCache = 0;
	/** The start kit's counts as ApplyStartKit gives them (cidjames.itm; nothing at the ranch), without touching the kit. */
	TMap<FString, int32> StartKitCounts(AOutlawsLevelActor* L) const;
	/** The weapons this machine's dead player carries beyond the start kit (with their own ammunition): out of the pockets,
	 *  onto the ground round the body as shared pickups that give the same back.  Returns the ground items and contents. */
	struct FDrop { FString Ground; FString Contents; TArray<TPair<FString, int32>> Give; };
	void GunDrops(AOutlawsLevelActor* L, TArray<FDrop>& Out);
	int32 GunsDropped = 0;
	/** A ground item made here with contents of its own (a dropped gun with the dead player's ammunition): its name as the
	 *  other machine is told it ("gsgun#isgun=1,ishells=17"); NoteGroundItem sends it while this is set. */
	FString GroundNameSuffix;
	/** A told ground item's name split into the ITM's own name and its contents (empty: the ITM's), and the contents put
	 *  in place of the pickup's own entries. */
	static FString SplitGroundName(const FString& Name, FString& OutContents);
	static void ApplyGroundContents(AOutlawsLevelActor* L, AActor* A, const FString& Contents);
	/** (user 2026-10-02) The wheel's drop passed to the other player, its test (-OLCoopTest=pass; OutlawsWheelDrop.cpp): the
	 *  host's steps 400 on, the client's "wd:" steps. */
	void TickHostPassTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	void OnPassTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	TArray<int32> PassIds;                       // host: the pickups its test dropped, by their numbers
	TArray<int32> PassQueue;                     // client: the pickups the host asked it to walk over, in turn
	double PassAt = 0.0;
	/** The Multiplayer page's player name (user 2026-10-02): this player's sent through its link when it is first known and
	 *  when it changes; the other's logged as it comes (OutlawsCoopNameplate.cpp). */
	void TickName();
	FString SentName;
	bool bNameSent = false;
	int32 LoggedNameSerial[MaxPlayers] = {};      // (N1: each player's name serial as last logged)
	uint32 NamesSeen = 0;                        // (REQ-MPLOBBY c: the links' names as last seen, for the pages' redraw)
	/** Its test (-OLCoopAuto -OLCoopTest=nameplate, each instance with -OLMpName=<name>): the host looks at the client's
	 *  puppet: its name and health bar (photographed: Saved/OLShots/ol_nameplate_host.png), the bar after hurts (orange,
	 *  red), dead, revived, a new name and no name, and the speaker shown while talking (forced on). */
	void TickHostNameplateTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	void OnNameplateTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	int32 NameplateTries = 0, NameplateChecks = 0, NameplateFails = 0;
	/** REQ-COOPCHAR: the link this client's co-op character was last sent through (sent again on a new link). */
	TWeakObjectPtr<AOutlawsCoopLink> CoopCharSentLink;
	/** REQ-COOPCHAR: this machine's character for co-op: -OLMpChar=<Short> (a test's, never saved), else the settings' MpCharacter. */
	static FString CoopCharChoice();
	/** BUG-CHARLOADOUT: whose start kit this machine's player gets: a match's character (its link's, else the choice), a co-op
	 *  joiner's choice; empty for single player and the co-op host (James Anderson, as ever). */
	static FString KitCharacterFor(const UObject* Context);
	FString NameplateFirstName;
	void NameplateCheck(bool bOk, const FString& What);
	/** Decision 6: each player's pace told through its link; the bad guys at the average, the timed doors at the slower. */
	void TickPace();
	float SentPace = -1.f;
	FString LoggedPace;
	float CoopSpeedArg = -1.f;                   // -OLCoopSpeed=<slider value>: this instance's speed setting (tests; never written)
	int32 BodyKnocks = 0;                        // decision 3: thrown bad guys stopped by the other player's body here
	// M6: the verbs (OutlawsCoopVerbs.cpp)
	void SetupVerbs(AOutlawsLevelActor* L);
	/** Client: a hurt from a host's enemy met this player's held plate first (true: all of it stopped; Damage: what is left). */
	bool PlateTakesHurt(AOutlawsLevelActor* L, const FVector& From, float& Damage);
	bool bApplyingVerb = false;                  // the other machine's change being made here: not told back
	int32 VerbBlastsSent = 0, VerbBlastsShown = 0, VerbBitsSent = 0, VerbBitsApplied = 0, VerbShotsSent = 0, VerbShotsShown = 0;
	int32 VerbThrowsSent = 0, VerbThrowsShown = 0, PlateStops = 0, VerbLogLines = 0;
	FString VerbsSummary() const;
	// M8: saves (OutlawsCoopSaves.cpp)
	/** Host: a save loaded for both (Slot as SaveGame's: -1 the autosave, a co-op game's or a single-player one, 0..9). */
	bool RequestLoad(int32 Slot);
	void BeginLoad(AOutlawsLevelActor* L, const FString& Level, int32 Difficulty, TFunction<void()> Then);
	void TickSaves(double Now);
	FString TestFingerprint(AOutlawsLevelActor* L) const;
	FString TestWorldPrint(AOutlawsLevelActor* L) const;
	FString TestLightsPrint(AOutlawsLevelActor* L) const;     // (BUG-LIGHTSAVE: the sector lights, which TestWorldPrint leaves out)
	int32 LobbyLoadSlot = -100;                  // host: the lobby's Start loads this save (-100: a new game)
	bool bResumeChosen = false;                  // host: the lobby's Resume Game picked before both were ready
	bool bTestResume = false;                    // -OLCoopLoad=resume: the test picks the lobby's Resume Game row
	double FirstLobbyAt = 0.0;
	bool bStartAloneFromSave = false;          // (R11: HostFromSave's lobby starts at once, alone)
	double NextBegunPlayLogAt = 0.0;               // (N1 diagnosis: a client whose world has not begun play)
	FString Player2Json;                         // host: the client's own state as last reported (N1: the first client's)
	/** N1: host: every client's own state as last reported, and the blocks a loaded save carried for players not here,
	 *  by PlayerKey (the player's name, else "#<number>"); a save keeps them all as "players". */
	TMap<FString, FString> PlayerJsons;
	FString PlayerKey(int32 Who) const;
	/** N1: host: the block for player Who (by its name, else its number; player 1 also the old "player2"), or empty. */
	FString PlayerJsonFor(int32 Who) const;
	/** N1: host: Text (a save's JSON) with player Who's block as its "player2", for that player's machine. */
	FString SaveTextFor(const FString& Text, int32 Who) const;
	/** N1: a save's block for player Who ("players" by its key or number; else player 1's "player2"), or null. */
	TSharedPtr<class FJsonObject> SaveBlockFor(const class FJsonObject& Save, int32 Who) const;
	FString SentPlayer2;                         // client: as last sent
	double NextPlayer2At = 0.0, Player2FullAt = 0.0;
	TArray<FString> SnapParts;                   // client: the save's JSON as it comes, for the level of SnapEpoch
	int32 SnapEpoch = -1, SnapGot = 0;
	TSharedPtr<class FJsonObject> SnapObj;       // client: the whole save, waiting for its level here
	int32 LoadEpoch = -1;                        // host: the level whose save is being put back here
	bool bHostLoadPending = false;
	int32 SavesWritten = 0, SnapshotsSent = 0, SnapshotsApplied = 0;
	FString SavesSummary() const;
	// M8 tests (-OLCoopTest=save, -OLCoopTest=load with -OLCoopLoad=<slot>; -OLLoadTest=<slot> for single player)
	void TickHostSaveTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	void OnSaveTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	void TickSoloLoadTest(double Now);
	int32 SoloLoadSlot = -100;                   // -OLLoadTest=<slot>: single player loads it from the title menu and reports
	int32 SoloLoadStep = 0;
	double SoloLoadAt = 0.0;
	int32 TestSaveEnemy = -1, TestSaveDoor = -1, TestSaveCopy = -1;
	// M6 tests (-OLCoopTest=verbs)
	void TickHostVerbsTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	void OnVerbsTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	void TestLogVerbs(AOutlawsLevelActor* L, const TCHAR* Why);
	int32 TestWallA = -1, TestWallB = -1;
	// M6 item 4: grabs (OutlawsCoopGrabs.cpp)
	TMap<int32, int32> ClientHolds;              // host: the bad guys (by index) its copy holds for a client's hand, and whose (N1)
	TMap<int32, double> ClientThrown;            // host: those a client let go of, still flying here (when let go)
	TMap<int32, int32> ClientThrownBy;           // ... and who threw each (N1)
	/** The player who threw bad guy Index, as this machine knows it (host: its own hand or a client's; client: itself). */
	int32 ThrowerOf(const AOutlawsEnemy* E) const;
	void TickGrabs(double Now);
	int32 GrabsByHost = 0, GrabsByClient = 0, ThrowsByHost = 0, ThrowsByClient = 0, GrabsRefused = 0, GrabsLetGo = 0;
	FString GrabsSummary() const;
	// M6 item 4 tests (-OLCoopTest=grab)
	void TickHostGrabTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	void OnGrabTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	/** Tests: this player's right hand held out before the eyes (a desktop player has no tracked hand): TestGrabHand 1 its
	 *  grip squeezed, 2 open, 0 left alone. */
	void TickGrabTestHand(AOutlawsPlayer* P);
	void TestLogGrab(AOutlawsLevelActor* L, AOutlawsPlayer* P, int32 Index, const TCHAR* Why);
	/** 2026-10-02 tests: the paces this machine applies (decision 6); both players' bodies as this machine has them (3). */
	void TestLogPace(AOutlawsLevelActor* L);
	void TestLogBodies(AOutlawsPlayer* P);
	int32 TestGrabHand = 0;
	int32 TestGrabA = -1, TestGrabB = -1, TestGrabC = -1;
	int32 TestArcMan = -1;                 // (BUG-ARCSTUCK: the man the host holds with the arc out, shot dead by the client)
	/** Tests, the client: the host's g:tntarc asks run one a frame (see the .cpp). */
	void TickClientGrabTest(AOutlawsPlayer* P);
	TArray<int32> TestTntArcQueue;         // (BUG-TNTARCTRIGGER: the host's asks not yet run here)
	uint64 TestTntArcFrame = 0;            // (the frame the last one ran)
	double TestTntArcAt = 0.0;
	// M7 tests
	void TickHostRulesTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	/** Tests (2026-10-02, the pause for one): this player turned to an open way and walking it for Seconds. */
	void TestWalkOpen(AOutlawsPlayer* P, float Seconds);
	int32 TestPauseEnemy = -1;
	int32 TestPauseDoor = -1;
	FVector TestWalkFrom = FVector::ZeroVector;
	int32 TestRulesKeys = 0;
	FString TestRulesNote;
	FVector TestFellAt = FVector::ZeroVector;    // client: where its test death was (its guns lie round it)
	// 2026-10-02: the extra bad guys (OutlawsCoopExtras.cpp)
	int32 ExtrasMadeHere = 0, ExtrasInWalls = 0, ExtrasListed = 0;
	FString ExtrasHow;                           // host: "computed" or "cached" for the last list
	double ExtrasWaitSince = 0.0;                // client: a start serial in, its list of extra bad guys not yet
	FString ExtrasSummary() const;
	// its test (-OLCoopTest=extras on the hideout: the hideout, the same again from the cache, the town, the canyon)
	void TickHostExtrasTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	void OnExtrasTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	/** Tests: this player stood where it sees one of the extra bad guys beside his original (the nearest that can be framed so). */
	int32 TestFrameExtra(AOutlawsLevelActor* L, AOutlawsPlayer* P);
	int32 TestExtraLevel = 0, TestExtraShown = -1, TestExtraFight = -1;
	/** -OLExtrasSurvey=<folder> (a test, no session): the list worked out for every story level at each difficulty for two,
	 *  three and four players, written there as the cache files would be (checked by scratchpad/extras/oracle). */
	FString ExtrasSurveyDir;
	void RunExtrasSurvey();
	// M5 tests
	FString TestOnly;                            // -OLCoopTest=inf: only that part of the scripted test
	int32 TestDoorSector = -1;
	int32 TestDoorA = -1, TestDoorB = -1, TestKeyDoor = -1;
	double TestPushAt = 0.0;                     // client: when to push the door it was sent to (0: none)
	void TickHostInfTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P);
	/** Tests: this machine's player stood just outside elevator Index's door (TestDoorSector: the door's sector). */
	bool TestGoToDoor(AOutlawsLevelActor* L, AOutlawsPlayer* P, int32 Index);
	void TestLogElevator(AOutlawsLevelActor* L, int32 Index, const TCHAR* Why);
};
