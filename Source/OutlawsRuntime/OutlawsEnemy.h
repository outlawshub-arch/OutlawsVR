#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "OutlawsTypes.h"
#include "OutlawsEnemy.generated.h"

class AOutlawsLevelActor;
class AOutlawsSpriteActor;
class UOutlawsCoop;
struct FOutlawsCoopEnemySnap;
namespace OutlawsTextures { struct FSpriteAtlas; }

/**
 * An Outlaws bad guy (ITM FUNC BGY_New): a capsule that walks the level with a character movement
 * component (no controller: bRunPhysicsWithNoController), shown by an NWX sprite carrying every
 * choreography, and driven by the Outlaws enemy logic as read from the remaster exe
 * (research/outlaws_enemy_exe.md, Enemy.cpp): watch, alert with a combat hook (face, decide from the
 * aggressiveness x confidence matrix, advance, retreat, take cover, duck, shoot bursts, shoot on the
 * move), hit reactions and death, with the difficulty rescaling of the ITM values.  The Jedi actor
 * code (The Force Engine) fills the gaps the exe reading left.
 *
 * The capsule is a Pawn that also blocks Visibility, so the player's bullet trace hits it; the
 * player's own capsule ignores Visibility, so an enemy's shot is a world trace plus a test against
 * the player's body.  Its height follows the frame on show (a crouch is short), as the exe takes the
 * actor height from the playing frame.  A corpse keeps colliding with the world only.
 */
USTRUCT()
struct FOutlawsEnemyDef
{
	GENERATED_BODY()
	// BGYStatic defaults [EXE 0x1408fee60]; Outlaws units (30 cm), seconds, degrees
	float Height = 6.f;
	float Radius = 1.f;
	float Health = 2.f;
	int32 Aggressiveness = 4;         // column of the decision matrix: how much confidence it needs to charge
	int32 BaseMode = 0;               // 0 normal, 1 sniper, 2 hides until it sees your legs, 3 ambush within 15 u
	float WalkSpeed = 8.f;
	float RunSpeed = 12.f;
	float Fov = 145.f;                // half-angle each side of the facing
	float FovDist = 300.f;
	float LosTimer = 0.25f;           // sight rechecks, x distance / 25 u, at most 5 s
	float ShootTimer = 2.f;
	float TauntTimer = 25.f;
	float Confidence = 0.75f;         // confidence meter growth per second
	float PanicRate = 2.f;            // panic meter decay per second
	float MoveInaccuracy = 20.f;
	float WeaponInaccuracy = 4.f;
	float PanicInaccuracy = 7.f;
	float Spread = 0.f;               // the enemy weapon's SPREAD_1: extra pellets scatter this much
	float Range = 120.f;              // the enemy weapon's EFFRANGE_1: how far a pellet reaches
	float MaxStep = 3.f;
	float MaxDrop = 15.f;
	float Damage = 1.f;
	int32 ShotMultiples = 1;
	float ThrowMax = 0.f;             // a thrower (dynamite, knives) instead of a shooter when set
	float ThrowMin = 0.f;
	int32 WatchChor = 0, WalkChor = 1, RunChor = 2, DuckChor = 3, ShootChor = 10, HitChor = 20;
	int32 ChestDieChor = 21, LegDieChor = 22, WalkShootChor = 31, RunShootChor = 32;
	FString ShootSound = TEXT("enmyshot");
	FString HitSound = TEXT("bgy1hit");
	FString DieSound = TEXT("bgy1die");
	FString DropItem;
	TArray<FString> LosTaunts, NoLosTaunts, AlertTaunts;

	/** Take the ITM DATA fields over the defaults. */
	void Read(const TMap<FString, FString>& Data);
	/** The game's difficulty pass over a freshly loaded block [EXE 0x1400901fc]. */
	void ScaleForDifficulty(EOutlawsDifficulty Difficulty);
};

UENUM()
enum class EOutlawsEnemyState : uint8
{
	Watch,      // at its post; alerts when it sees the player (or hears a shot it can see)
	Chase,      // alert: the combat hook runs (face, decide, advance, retreat, cover, duck, shoot)
	Shoot,      // (reported for callers: alert and in a shooting sub-state)
	Hit,        // flinch
	Dead,
};

/** CPU2 (2026-10-05): the bad guys' CharacterMovement ticks apart from the actor, so it gets a perf bucket of its own. */
UCLASS()
class OUTLAWSRUNTIME_API UOutlawsEnemyMovement : public UCharacterMovementComponent
{
	GENERATED_BODY()

public:
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	/** QSPEED-1: a launch, an impulse, a force or a requested velocity waits for the next move (the driver's rest test). */
	bool HasPendingMove() const;
	static void CountMove();

protected:
	/** QSPEED-1: in a game world the enemy driver (OutlawsEnemyDriver.cpp) ticks it, not the engine's tick queue. */
	virtual void RegisterComponentTickFunctions(bool bRegister) override;
};

UCLASS()
class OUTLAWSRUNTIME_API AOutlawsEnemy : public ACharacter
{
	GENERATED_BODY()

public:
	AOutlawsEnemy(const FObjectInitializer& ObjectInitializer);

	/** Set up from the ITM and give it its sprite (already spawned, every chor in its atlas). */
	void Init(AOutlawsLevelActor* InLevel, const FString& InName, const TMap<FString, FString>& ItmData, AOutlawsSpriteActor* InSprite, float Yaw);

	/** A bullet: base damage in health points, where it hit, which way it travelled, and from where. */
	void TakeHit(float Damage, const FVector& Point, const FVector& Dir, const FVector& From, bool bBlast = false, float KnockScale = 1.f);
	/** Fall damage (user 2026-09-30): a landing hurts by the height it came down from. */
	virtual void Landed(const FHitResult& Hit) override;
	/** A deadly floor or sector (FLR_/SEC_ DIE, the train's ground) under him: dead at once, as the player is (BUG-TRAINFALL). */
	void FloorKill(const FVector& Feet);
	/** Thrown along a hit: health points dealt set the speed (a blast throws up and away). */
	void Knock(const FVector& Dir, float Dealt, bool bBlast);

	/** The player's gunfire within earshot (150 u): a watcher that can see the shooter (or is lucky) alerts. */
	void HearShot(const FVector& Where);
	/** A neighbour spotted the player (message 125, within 50 u): a mode 0 bad guy joins in. */
	void ShareTarget();
	/** A neighbour's news (messages 126..131): nudges the confidence and panic meters. */
	void AdjustMeters(float Confidence, float Panic);
	/** Alert now (tests, scripted scenes); bSpread also tells the others within 50 u. */
	void Wake(bool bSpread);

	bool IsDead() const { return State == EOutlawsEnemyState::Dead; }
	double DiedAt = 0.0;               // (world seconds: a body's fall cry only within 5 s of the death)
	/** HM1: the next TakeHit is a melee one (a fist, a knife's stab or a thrown knife: the exe's damage types 0 and 2), set
	 *  by the caller just before it.  A boss brought down by one is taken alive (OutlawsHom.cpp), and in a Historical
	 *  Missions session such a hit on a boss does x100 (the exe's rule: one punch drops him). */
	static bool GNextHitMelee;
	bool bTakenAlive = false;
	/** REQ-ENEMYFLOAT (OutlawsEnemyFloat.cpp): in water deeper than he stands a living man floats, the water line at FloatLine
	 *  of his height over his feet, and still wades, turns and shoots; thrown or fallen in he comes up to it; held under by a
	 *  player's grab for DrownSeconds he drowns (the count starts again once his head has been up for SurfaceResetSeconds).
	 *  The host's or single player's copy only (Tick, before TickCarried). */
	void TickWater(float Dt);
	bool IsFloating() const { return bFloating; }
	/** Tests: a wade to P (a move as Decide picks one). */
	void DebugSwimTo(const FVector& P);
	/** Tests: his feet, the surface, the water line's share of his height, the mode, the float, the time held under. */
	FString DebugWater() const;
	static constexpr float FloatLine = 0.78f;        // the water line between the chest and the neck
	static constexpr float DrownSeconds = 30.f;
	static constexpr float SurfaceResetSeconds = 3.f;
	float DrownHeld = 0.f;                           // seconds held under since he last had SurfaceResetSeconds up
	float DrownSurfaced = 0.f;                       // seconds his head has been up since the last time under
	bool bDrowned = false;
	bool bHeadUnder = false;
	/** REQ-LEDGEKNOCK: a drop (no floor within 2.5 m down) within 1.5 m, any way round (Way first): its direction and distance. */
	bool FindDropEdge(const FVector& Way, FVector& OutDir, float& OutDist) const;

	/** Grabbed by the chest (user 2026-09-29): held up by the player as a shield, frozen on the first frame of its
	 *  death (swept off its feet), thinking nothing; let go it drops, or flies as thrown (the death's next frames,
	 *  the fall cry), knocking over any bad guy it strikes, and is itself again once it lands. */
	bool bHeldByPlayer = false;
	bool bThrown = false;
	/** (BUG-GRABFALL) let go before the far grab's pull brought him to the fist: the fist's height, his fall measured from
	 *  no higher (EndHeld takes it and clears it) */
	bool bPullCap = false;
	float PullCapZ = 0.f;
	bool CanBeGrabbed() const;
	FVector GrabChest() const;
	void BeginHeld(AActor* By);
	/** Something thrown went past within a metre (user 2026-10-01, backlog 1d): a half-second flinch, no dodge. */
	void Flinch();
	/** A test aid (-OLBodyTest): dead, held, thrown, its pool, the blood it has left, a critter's remains laid. */
	FString DebugBodyState() const;
	void EndHeld(const FVector& Vel);
	/** Set before EndHeld for an aimed throw: his gravity times this till he lands (the throw's pace, user 2026-09-29). */
	float FlightGravity = 1.f;
	float SavedGravityScale = -1.f;
	/** A chicken or a tarantula (HEIGHT under 4.5): held in the hand every which way, not stood up as a man is. */
	bool IsCritter() const { return Def.Height < 4.5f; }
	float GetMaxHealth() const { return Def.Health; }
	float GetHeading() const { return Heading; }
	/** The difficulty changed in play: this one's speed, shot timer, health (the share it has left kept), confidence,
	 *  damage, aim and aggressiveness as that difficulty gives them. */
	void Rescale(EOutlawsDifficulty To);
	/** Held critter: its sprite turned with the hand (the tarantula by its back, legs away from the palm; the chicken
	 *  by its neck, hanging from the fist), its side to the eye, the anchor point read off the frame on show. */
	void PoseHeldCritter(const FTransform& Grip, bool bLeftHand, const FVector& Eye);
	void SetAtlas(const TSharedPtr<OutlawsTextures::FSpriteAtlas>& A) { AtlasRef = A; }

	/** The level's boss (Sanchez, Two Feathers): its death ends the level. */
	bool bBoss = false;
	/** A boss with one place of his own (not Sanchez, not Two Feathers): hunts only once alerted (sight, a shot heard,
	 *  a hit), so he is at his place when the player comes (user 2026-10-01: the canyon's Dick Farmer hunted from the
	 *  level's start and was gone from it at the end). */
	bool bHuntOnceAlerted = false;
	// the named outlaws who hold their ground (user 2026-09-28): Slim (the hideout), DrDeath_New (Henry at the mill,
	// Dr. Death in the mine) and Bob (the ranch) never walk; they turn, shoot, duck and fall where they stand
	bool bStationary = false;
	/** A roaming boss (Sanchez, Two Feathers, Tim Bloodworth...; not the ones that hold their ground): once about, it
	 *  always makes its way toward the player, walking round walls through the level's openings, seen or not
	 *  (user 2026-09-28). */
	bool bHunter = false;
	/** A roaming named outlaw (not a match bot): with the player in plain view and within his gun's reach he stands and
	 *  fights instead of walking about (BUG-BOSSPASSIVE, user 2026-10-15). */
	bool IsFightingBoss() const { return bBoss && bHunter && CoopSelf < 0; }
	float HuntWait = 0.f;
	// Bob_New: each shot leans out from the curtain (chor 33), fires (34, its fire event) and pulls back (35)
	bool bPeekShooter = false;
	int32 PeekStage = -1;
	double LastPeekAt = -1.0;          // (Bob's peeks, for the log: the seconds between them)
	float NextPeekGap = 3.f;           // ... and the wait before the next (1.5 to 4.5 s, now and then under 1 s; drawn at each)
	/** Bob until his door opens (user 2026-10-04, BUG-BOBFROZEN): still, unaware, not alerted by shots, neighbours or hits,
	 *  and not shooting; WakeCurtainBoss lets him go. */
	bool bBehindDoor = false;
	/** BUG-DRDEATHANIM (user 2026-10-12; the hideout's Slim too, 2026-10-14): the mine's Dr. Death and the ranch's Bob, whose fall is in the cutscene the level
	 *  ends on (the gun shot from Doc's hand, Bob going down).  Their handler [EXE 0x14006a750] asks for the level's end on
	 *  the killing shot itself [0x14006a81e..0x14006a823], not on the death chor's END as Sanchez's does; here their death
	 *  plays no choreography: the frame on show is kept (HoldForCutscene) for the fade.  Every other boss falls as before. */
	bool bFallInCutscene = false;
	/** Dead into the cutscene: his live frame held, no push, not hidden.  A level that does not end on it after all
	 *  (FallbackFromHold) plays the death after 1.5 s. */
	bool bHeldForCutscene = false;
	void HoldForCutscene(const TCHAR* Why);
	void FallbackFromHold();
	/** (user 2026-10-14, BUG-DRDEATHANIM again) a pose a living man stands in: not the hit stun (its art is the first of
	 *  the fall), not a death, not none.  A killing shot that lands in the stun of the shot before holds his stand instead. */
	bool IsLivePoseChor(int32 Chor) const;
	int32 HeldFromChor = -1;           // (tests: the choreography on show when the killing shot landed)
	bool bHoldLetGo = false;           // (FallbackFromHold's own Play; every other Play on a held one is refused)
	int32 HeldPlaysRefused = 0;        // (tests: Plays refused while held)
	int32 DieChorsPlayed = 0;          // (tests: a death choreography started, by Die, the puppet's death or a snapshot)
	int32 ShotsTotal = 0;              // (tests: shots he has fired)
	/** Tests: state, sub-state, peek stage, health, whether the player is in sight now. */
	FString DebugBossState() const;
	/** Tests: each of SightCheck's gates for the player now (range, angle, light, the trace and what stops it). */
	FString DebugSight() const;
	// an outlaw's voice alone (RADIUS 0: the ranch's BOBVCE, 4 px tall, 999 health, no damage): taunts, nothing else
	bool bVoiceOnly = false;
	/** Not a bad guy for the boss count (chickens). */
	bool bNotBadGuy = false;
	/** Hidden, not solid, no thinking (the boss before he appears: the exe's VISIBLE off, NO_COLLIDE). */
	void SetDormant(bool bOn);
	bool IsDormant() const { return bDormant; }
	void WakeForTest() { WakeTick(); }          // (-OLVb2Test: a man put in the air ticks every frame, as one thrown does)
	/** The boss appears here: seen, solid, and straight after the player. */
	void Appear(const FVector& FeetLoc);
	/** A taunt from its list whatever the rules (the boss's forced taunts), played at a place. */
	/** A taunt from At (the late boss calling from somewhere unseen); returns its sound (null: none played). */
	class UAudioComponent* ForcedTaunt(const FVector& At);
	EOutlawsEnemyState GetState() const;
	float GetHealth() const { return Health; }
	const FString& GetOutlawsName() const { return OutlawsName; }
	AOutlawsSpriteActor* GetSprite() const { return Sprite.Get(); }
	int32 GetSubState() const { return Sub; }
	int32 GetMode() const { return Mode; }            // (tests: only a mode 0 watcher takes a neighbour's word)
	/** A save: where it is, which way it faces, its health, alive or dead, hidden (a boss not yet out). */
	void WriteSave(class FJsonObject& O) const;
	void ReadSave(const class FJsonObject& O);

	virtual void Tick(float DeltaSeconds) override;
	virtual void Destroyed() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	/** QSPEED-1 (tests, both ways): the bad guys' ticks and moves a frame since the last call. */
	static FString DriverStats();
	static void CountTick(float Interval);

	UPROPERTY(VisibleAnywhere, Category = "Outlaws")
	FOutlawsEnemyDef Def;
	/** What ScaleForDifficulty scales, as the ITM gave it (Init): the difficulty changed in play (user 2026-10-02) scales
	 *  from these again. */
	struct FUnscaled { float WalkSpeed = 0.f, RunSpeed = 0.f, ShootTimer = 0.f, Health = 0.f, Confidence = 0.f, Damage = 0.f, MoveInaccuracy = 0.f, WeaponInaccuracy = 0.f; int32 Aggressiveness = 0; };
	FUnscaled Unscaled;

	/** Far and unseen (user 2026-10-03): more than ol.FarFreezeDistance from the nearest player and in no sector the eye's
	 *  portals reach, a living bad guy holds still: his tick and his movement's at 0.5 s, no thinking, walking, shooting or
	 *  taunting, his sprite on the frame it shows; all he was is kept, and he goes on from it when a player comes near
	 *  (a tenth inside the distance), his sector shows, he is hit, held, carried by a lift, or an alert reaches him (every
	 *  wake goes through WakeTick, which thaws).  Never a boss, a voice, the dormant, the dead, or the client's puppets. */
	bool IsFarFrozen() const { return bFarFrozen; }
	/** CPU1's counts: awake (neither dormant nor far-frozen). */
	bool PerfAwake() const { return !bDormant && !bFarFrozen; }
	bool IsVoiceOnly() const { return bVoiceOnly; }
	/** CPU2 throttle (OutlawsEnemyThrottle.cpp): 0 FULL (thinks and moves every frame), 1 MOVE-ONLY (moves every frame on
	 *  its last plan, thinks every ol.throttle.ThinkInterval s), 2 WANDER (no movement component: a slow kinematic walk
	 *  inside its own sector), 3 FROZEN (no tick at all, its sprite's neither).  Set only by the level's manager. */
	uint8 GetThrottleTier() const { return ThrottleTier; }
	void SetThrottleTier(uint8 NewTier);
	uint8 ThrottleTier = 0;
	double ThrottleTierSince = 0.0;
	double ThrottleHitAt = -100.0;          // last damage (top priority for a while; never left throttled)
	double ThrottleWakeAt = -100.0;         // last wake (alert, heard shot): high priority, promoted through the wake queue
	float ThinkAcc = 0.f;
	FVector WanderDir = FVector::ZeroVector;
	float WanderLeft = 0.f;
	float WanderFloorCheck = 0.f;
	int32 WanderSector = -2;
	int32 ThrottleSector = -2;
	FVector ThrottleSectorAt = FVector::ZeroVector;
	/** "<pos> h<heading> st<state> sub<sub> hp<health> conf<meter> chor<chor>/<frame>": the freeze test's before-and-after. */
	FString DebugFreezeState() const;

	// ---- co-op (OutlawsCoopEnemies.cpp): set only in a session.  In single player CoopRole and CoopTarget stay 0 and none
	// of it runs ----
	/** 1: the host's (it thinks for both players); 2: the client's copy of the host's (a puppet: no thinking, shooting or
	 *  hearing; it shows where the host's is and what it does, and its hits go to the host). */
	uint8 CoopRole = 0;
	int32 CoopIndex = -1;
	/** 2026-10-02 (OutlawsCoopExtras.cpp): one of a session's extra bad guys, standing beside the level's own bad guy at this
	 *  index in the list (-1: the level's own; always so in single player).  (Named for when he was that one's copy; since
	 *  user 2026-10-02 he is a kind of his own.)  CoopExtraIndex: his place in the host's list.  Saves mark him with both. */
	int32 CoopCopyOf = -1;
	int32 CoopExtraIndex = -1;
	/** Host: the player it is after (0 the host's own, 1 the client's). */
	int32 CoopTarget = 0;
	/** REQ-MPBOTS: a match bot's brain: the player number it plays as (never its own target; its shots and hits go through
	 *  the match as a player's: UOutlawsCoop::BotFired / BotPellet; it keeps the players' pace).  -1: a bad guy. */
	int32 CoopSelf = -1;
	/** The level's end panels (user 2026-10-02, OutlawsStats.cpp): who struck him last (EOutlawsStatsBy: his death is their
	 *  kill), from where, by a blast (a dynamite kill), and whether the distance counts (not for a fall or a throw). */
	uint8 StatsBy = 0;
	bool bStatsBlast = false;
	bool bStatsDirect = true;
	FVector StatsFrom = FVector::ZeroVector;
	uint8 CoopShots = 0;
	uint8 CoopPlays = 0;
	TWeakObjectPtr<UOutlawsCoop> Coop;
	void CoopSetup(UOutlawsCoop* InCoop, int32 Index, bool bHost);
	void CoopFillSnap(FOutlawsCoopEnemySnap& S) const;
	void CoopApplySnap(const FOutlawsCoopEnemySnap& S);
	/** Client: the host's says it died (its death and sound here; no drop: the host's comes as a ground item). */
	void CoopPuppetDie(int32 Chor, const FVector& Dir, const TCHAR* How, bool bQuiet = false);
	/** Host: a player's shot heard (Who 0 the host, 1 the client): a watcher that alerts turns on that player. */
	void HearShotBy(const FVector& Where, int32 Who);
	/** "at <pos> state <state>" for the co-op log. */
	FString CoopDescribe() const;
	/** Client: how far the copy is drawn from the host's last word on its place (-1: none yet). */
	float CoopLagCm() const;
	/** Milestone 6 item 4 (OutlawsCoopGrabs.cpp), the client's copy: it may be taken into this player's hand (the host's is
	 *  neither held nor flying); and the session's word that this machine's hold of it is over (refused, or the host's copy
	 *  left the hand): out of the hand at once, showing the host's again. */
	bool CoopPuppetGrabbable() const;
	void CoopCancelHeld(const TCHAR* Why);
	/** The client's own throw of its copy: the host's copy struck another bad guy on the way (TickCarried's "both fall") and
	 *  hopped back: this flight goes on from where the host's did, at its hop (2026-10-02: so both still land together). */
	void CoopFollowHostFlight(const FVector& Pos, const FVector& Vel);
	/** The client's copy is in this player's hand, flying from it or just landed: it shows its own place, not the host's. */
	bool CoopLocalCarry() const { return bHeldByPlayer || bThrown || PuppetSettleUntil > 0.0; }
	/** REQ-GRABUPSCALE (OutlawsGrabUpscale.cpp): held in this machine's hand, his held frames (the hit stun's first frame,
	 *  front and back) show their 2x upscales where the player's PC made them; loaded on the grab, dropped on the let-go.
	 *  Missing, or Remastered off: the HD atlas as before. */
	void TickGrabUpscale();
	const TSharedPtr<OutlawsTextures::FSpriteAtlas>& GetAtlasRef() const { return AtlasRef; }
	bool bGrabUpOn = false;
	uint32 GrabUpGen = 0;
	double GrabUpAskedAt = 0.0;

protected:
	/** QSPEED-1: in a game world the enemy driver (OutlawsEnemyDriver.cpp) ticks him, not the engine's tick queue. */
	virtual void RegisterActorTickFunctions(bool bRegister) override;

private:
	// combat hook sub-states [EXE 0x14008b670]
	enum ESub : int32 { SubFace = 1, SubAdvance = 3, SubRetreat = 4, SubShoot = 5, SubShootMove = 6, SubCover = 8, SubDuck = 9, SubDecide = 10 };

	TWeakObjectPtr<AOutlawsLevelActor> Level;
	UPROPERTY(Transient)
	TObjectPtr<AOutlawsSpriteActor> Sprite;
	FString OutlawsName;
	EOutlawsEnemyState State = EOutlawsEnemyState::Watch;
	int32 Mode = 0;                   // BASE_MODE, drops to 0 when its condition is met while ducking
	int32 Sub = SubFace;
	int32 ResumeSub = SubFace;        // the move a shot interrupted, taken up again after it
	float SubTime = 0.f;
	float StateTime = 0.f;
	float Health = 2.f;
	float StandHalf = 90.f;           // the capsule half height when standing
	float Heading = 0.f;              // UE yaw it faces (the sprite shows it)
	float PrevShotYaw = 0.f;          // yaw to the target when this shot's wind-up began: turning spoils the aim
	float ConfidenceMeter = 7.f;
	float PanicMeter = 0.f;
	float LosWait = 0.f;
	float ShootWait = 0.f;
	float TauntWait = 0.f;
	float DuckLeft = 0.f;
	int32 BurstLeft = 0;
	int32 ShotsFired = 0;
	int32 HitsTaken = 0;
	bool bSeen = false;               // the player is in sight (last LOS check)
	bool bLegsHidden = true;          // ... but not at knee height (or not seen at all)
	double LastSeenTime = -100.0;
	double LastAcquireTime = -100.0;
	FVector TargetPos = FVector::ZeroVector;   // where it last saw the player: shots aim here
	FVector MoveTarget = FVector::ZeroVector;
	bool bRunning = false;
	float StuckTime = 0.f;
	bool bDoorTried = false;
	bool bDropped = false;
	bool bDormant = false;
	// (BUG-FALLSCREAM) the fall cry's thresholds: 6 m below where the fall began, 10 m/s down
	static constexpr float FallCryDropCm = 600.f;
	static constexpr float FallCrySpeed = 1000.f;
	// (BUG-FALLSCREAM 2) and in the air this long; never for a fall begun in his first seconds (the start's settling)
	static constexpr float FallCryAirSec = 0.45f;
	static constexpr float FallCrySettleSec = 5.f;
	double FallStartAt = 0.0;
	float FallLastZ = 0.f;
	bool bFallSettling = false;
	float FallTopZ = 0.f;                    // the highest point of a fall (kept for the saves; the cry now measures from FallStartZ)
	float FallStartZ = 0.f;                  // where it left the ground (the fall cry: bgfall.wav past FallCryDropCm below that)
	bool bFalling = false, bFallCried = false;
	// (BUG-GRABFALL) where a hand let him go and how fast up or down: his landing hurts by no more than a fall from there
	float ThrowFromZ = 0.f, ThrowFromVz = 0.f;
	bool bThrowFromSet = false;
	bool bBloodPool = false;                 // the pool under the body has been started
	TWeakObjectPtr<class UProceduralMeshComponent> BloodPoolMesh;     // that pool (stopped when the body is lifted)
	float BleedLeft = 1.f;                   // how much of a whole pool the body has still to bleed
	float DripWait = 0.f;                    // carried dead: till the next drop
	int32 DeadChor = -1;                     // the death it died (played again when the body lands)
	bool bRemainsLaid = false;               // a chicken's or a tarantula's last frame laid on the floor
	// reactions to the player's verbs (backlog 1, user 2026-10-01: small, adding to the verbs, never cancelling them)
	bool bAdvanceNext = false;               // a friend was grabbed: the next move is an advance
	float FlankYawNext = 0.f;                // the plate held up: the next move flanks this far off the straight line
	double HostageSince = -1.0, HostageLast = -10.0;   // a held man in the line of fire: hesitate 1.5 s, then shoot
	float FleeCheck = 0.f;
	double FleeUntil = 0.0;            // running from a lit stick
	float PlateCheck = 0.f, PlateSeen = 0.f;
	bool bPlatePaused = false;
	float FlinchLeft = 0.f;
	/** A friend grabbed (or a body taken up) where these can see it: they come on, one of them shouts. */
	void FriendsSeeGrab();
	/** Lit dynamite in sight within its blast: straight away from it; the plate held up a second: a flank next. */
	void ReactToVerbs(float Dt);
	double FullPaceUntil = 0.0;              // real seconds: a hit's reaction plays at full speed whatever the movement slider
	bool bKnockFlight = false;               // thrown by a hit and not yet landed (flies at full speed too)
	FVector PendingKnock = FVector::ZeroVector;   // this frame's hits, launched together
	float LightCheck = 0.f;                        // next look at the sector light underfoot
	/** Far-off watchers standing at their posts, bodies at rest and the dormant tick a few times a second (their
	 *  movement too): 101 bad guys every frame were the Quest's second-biggest game-thread cost (OutlawsPerf). */
	void UpdateTickRate();
	/** Back to every frame at once: alerted, hit, knocked, grabbed, thrown, woken or killed. */
	void WakeTick();
	bool bBossReported = false;
	FString LastTaunt;

	void SetState(EOutlawsEnemyState NewState);
	void SetSub(int32 NewSub);
	void Play(int32 Chor, bool bLoop);
	/** The capsule as tall as the frame on show (feet stay put, the sprite stays on them). */
	void SetBodyHalf(float NewHalf);
	bool IsDucking() const;
	bool PlayerView(FVector& OutEye, FVector& OutFeet, float& OutRadius, float& OutHalf) const;
	FVector EyePoint() const;
	float HeightCm() const;
	bool CanSeeFrom(const FVector& From, const FVector& To) const;
	bool SightCheck() const;
	void LookForPlayer();
	void Alert(bool bBroadcast);
	void Decide();
	/** Decide's move; with bAvoidCrowd only to a place clear of the others (user 2026-10-01: they bunched up). */
	bool DecideMove(bool bAvoidCrowd);
	bool PickPoint(float MinU, float MaxU, float CentreYaw, float SpreadDeg, int32 Tries, bool bNeedCover, FVector& Out, bool bAvoidCrowd = false) const;
	/** How many other live bad guys stand within Radius of P (or are walking to there); OutCentre their middle. */
	int32 CrowdAround(const FVector& P, double Radius, bool bTheirGoals, FVector& OutCentre) const;
	float DisperseWait = 0.f;
	// far and unseen (user 2026-10-03, see IsFarFrozen)
	bool bFarFrozen = false;
	float FreezeCheck = 0.f;                  // the next look (four times a second; a frozen one looks every tick)
	double FreezeGraceUntil = 0.0;            // real seconds: woken by an alert or a hit, he acts this long before he may freeze again
	int32 FreezeSector = -2;                  // his sector for the visibility test, looked up again once he has moved 30 cm
	FOutlawsSectorHint CullHint;              // his last sector, where the level's sector lookups look first
	FVector FreezeSectorAt = FVector::ZeroVector;
	FString FreezeSnap;                       // -OLFreezeTest: what he was when he froze, checked against what he is when he thaws
	void UpdateFarFreeze(float Dt);
	void SetFarFrozen(bool bOn, const TCHAR* Why);
	bool FloorAt(const FVector& Probe, float Below, FVector& OutFloor) const;
	void MoveAlong(float Dt);
	void StartShooting(int32 FromSub);
	void StartShootMove();
	void BeginShotWindup();
	void FireAtPlayer();
	/** An event's taunt (an alert, a friend taken, a lit stick): through the level's taunt clock (BUG-TAUNTRATE). */
	void Taunt(const TArray<FString>& List, const TCHAR* Why = TEXT("event"));
	/** A line of List (not the last one twice running) played at this one's eyes; OutLine the line said. */
	class UAudioComponent* SayLine(const TArray<FString>& List, FString& OutLine);
	void Die(const FVector& Point, const FVector& Dir);
	void TickCarried(float Dt);
	// REQ-ENEMYFLOAT (OutlawsEnemyFloat.cpp)
	bool bFloating = false;
	float SavedFlySpeed = -1.f, SavedFlyBrake = -1.f;
	float DrownLogged = 0.f;
	double FloatSince = 0.0;
	float FloatVz = 0.f;                     // his rise or sink in the water, cm/s (the float moves him up and down itself)
	float FloatTargetZ(float SurfaceZ) const;
	/** Water at P's plan position at his feet's height (a place to wade to while he floats). */
	bool SwimAt(const FVector& P) const;
	void StartFloat(float SurfaceZ);
	void StopFloat(const TCHAR* Why, bool bKeepMode);
	void Drown();
	void NoteSurfaced(float Dt);
	TWeakObjectPtr<AActor> HeldBy;
	TSharedPtr<OutlawsTextures::FSpriteAtlas> AtlasRef;
	bool bCritterPosed = false;
	TMap<int32, FVector2D> NeckCache;             // atlas cell -> the neck (1x px from the cell's top left)
	bool NeckOf(int32 Cell, FVector2D& Out);
	void RestoreSpritePose();
	float ThrowTime = 0.f;
	FVector ThrowPrev = FVector::ZeroVector;
	// let go while the fist had him through the floor: up onto it and slid along it at the throw's speed (user
	// 2026-09-29), no ground friction, braked to rest about where the throw would have come down
	bool bFloorSlide = false;
	float SavedGroundFriction = -1.f;
	float SavedBrakeDecel = 0.f;
	TArray<TWeakObjectPtr<AOutlawsEnemy>> ThrowHits;
	void EndCarry();
	void FaceYaw(float Yaw, float Dt);
	double Now() const;
	// co-op (OutlawsCoopEnemies.cpp)
	bool CoopTargetView(FVector& OutEye, FVector& OutFeet, float& OutRadius, float& OutHalf) const;
	/** The player it is after is dead (single player: the level's own player). */
	bool TargetDead(const AOutlawsLevelActor* L) const;
	/** Host: the nearer of the two players, or the one it can see; bSay: logged even when it stays the same (an alert). */
	void CoopChooseTarget(bool bSay = false);
	friend class UOutlawsCoop;                    // (N1's party test asks a bad guy to choose among the players)
	friend class AOutlawsLevelActor;              // (CPU2: the enemy throttle, OutlawsEnemyThrottle.cpp, ranks and tiers them)
	friend struct FOutlawsEnemyDriver;            // (QSPEED-1: one tick for them all, OutlawsEnemyDriver.cpp)
	void TickWander(float Dt);
	/** CPU2 lossless pass: the enemy's collision and movement flags (-OLLossless=<mask> for A/B; all on by default). */
	void ApplyLossless();
	ECollisionEnabled::Type BodyCollision() const;
	bool CoopRemoteNear(double Cm) const;
	void CoopReportHit(float Damage, const FVector& Point, const FVector& Dir, const FVector& From, uint8 Flags, float KnockScale);
	void TickPuppet();
	/** CPU2 b: a puppet far from this player and in a sector this player cannot see is only put at the host's place four
	 *  times a second (its sprite still animates); true when this tick was that. */
	bool TickPuppetCulled(float Dt);
	FVector PuppetPos = FVector::ZeroVector;
	FVector PuppetVel = FVector::ZeroVector;      // (CPU2 b: from the last two snapshots; carried on at most 0.1 s past the last)
	double PuppetSnapAt = 0.0;
	float PuppetAhead = 0.1f;                     // (CPU2 d: how far past its newest place it is carried on, by its update rate)
	bool bPuppetCulled = false;
	float PuppetCullCheck = 0.f;
	float PuppetYaw = 0.f;
	bool bPuppetPlaced = false;
	bool bPuppetDeadSynced = false;
	int32 PuppetChor = -1;
	uint8 PuppetPlays = 0, PuppetShots = 0;
	double PuppetHitSentAt = -10.0;               // (this machine's own hit just showed its blood: the host's word adds none)
	// milestone 6 item 4 (OutlawsCoopGrabs.cpp): the client's own throw of its copy, flown here at once (the host's copy flies
	// the same throw; this one gives way to it once that has landed)
	void CoopPuppetRelease(const FVector& Vel);
	void CoopTickCarry(float Dt);
	void CoopEndLocalCarry();
	/** User 2026-10-02 (decision 3): thrown, the body of the player who did not throw him stops him on each machine the same
	 *  way.  From: where he was at the last look; here is where he is now.  Met on the way: put at the meeting, Vel loses what
	 *  went into the body (true). */
	bool CoopKnockBody(const FVector& From, FVector& Vel);
	bool CoopKnockBodyOf(int32 Who, const FVector& From, FVector& Vel);      // (N1: one player's body)
	bool bCoopKnocked = false;                    // (this flight met the body already: logged once)
	float CoopKnockClock = 0.f;                   // (the flight's time at the last look: smaller again, a new flight)
	bool bPuppetHostCarried = false;              // the host's copy is held or flying (its snapshot says so)
	bool bPuppetSawHostCarry = false;             // ... seen so since this machine's hold began
	FVector PuppetCoastVel = FVector::ZeroVector;
	float PuppetCoastGravZ = 0.f;
	bool bPuppetCoastSlide = false;               // let go under the floor: stood on it and slid along it (as EndHeld does)
	double PuppetSettleUntil = 0.0;               // landed here: the host's place is not taken till its copy is down too
};
