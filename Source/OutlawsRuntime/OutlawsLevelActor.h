#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "OutlawsGeometry.h"
#include "OutlawsFormats.h"
#include "OutlawsTypes.h"
#include "OutlawsKillFeed.h"
#include "Async/Future.h"
#include "OutlawsLevelActor.generated.h"

class UProceduralMeshComponent;
class UMaterialInterface;
class UMaterialInstanceDynamic;
class UStaticMesh;
class UTexture2D;
class FOutlawsAssets;
class FOutlawsInfRuntime;
class FOutlawsLevel;
struct FOutlawsElevator;
class AOutlawsEnemy;
class AOutlawsSpriteActor;
struct FOutlawsCreditLine;
namespace OutlawsTextures { struct FSpriteAtlas; }

/**
 * Builds an Outlaws level at load time from the player's own LAB archives: sector geometry
 * (procedural mesh, complex collision), water surfaces, the sky dome, object sprites
 * (AOutlawsSpriteActor) and 3DO meshes.  Nothing it builds is saved with the map.
 *
 * Place one at the origin of an otherwise empty map, set LevelName, and it builds on
 * BeginPlay (and from the Build button in the editor).  Reference implementation:
 * tools/olue/olue_export.py + ue_olue_import.py.
 */
UCLASS()
class OUTLAWSRUNTIME_API AOutlawsLevelActor : public AActor
{
	GENERATED_BODY()

public:
	AOutlawsLevelActor();

	/** Level file stem inside the LABs, e.g. townex, train, hideout. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws")
	FString LevelName = TEXT("hideout");

	/** Outlaws Remastered folder; empty = next to the exe, then the Steam defaults. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws")
	FString InstallPath;

	/** Use the remaster's HD textures (HDX, BC7) where they exist. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws")
	bool bHDTextures = true;

	/** Use the remaster's HD sound effects and taunts (olsfxhd, oltaunthd) rather than the originals. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws")
	bool bHDSounds = true;

	/** Degrees per second the sky dome turns (the clouds drift left). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws")
	float SkySpinSpeed = 0.8f;

	/** The level music's loudness (the soundtrack converted to Content/Music by convert_movies.py). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws")
	float MusicVolume = 0.5f;

	/** Fill the empty panes of TINT walls with a pale translucent sheet.  Off: the user prefers the
	 *  clear panes (townex's windows read as open glass without it, 2026-09-27). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws")
	bool bTintedGlass = false;

	/** The VR menu's movement speed slider (0.2 .. 0.75 here, SHOWN as 40 % .. 150 %: BUG-SPEEDCAP, user 2026-10-13, it was 0.2 .. 1,
	 *  200 %; user 2026-10-02, "the 100 % currently
	 *  should equal 200 %, that way 50 % becomes 100 %, which is the default"): the player's walk / run / swim, and the bad
	 *  guys' whole pace (their moves, decisions and time between shots) by time dilation.  1.0 is the Dark Forces pace
	 *  halved (user 2026-10-01): Pace() = slider x 0.5.  Starts at 0.5 (shown 100 %); the menu warns over 0.6 (shown
	 *  120 %).  Meta's 1.4 m/s walk is 0.3 on it (shown 60 %), VRChat's 2 m/s 0.42 (84 %). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws")
	float GameSpeed = 0.5f;
	/** The dial's top (shown 150 %) and the lowest (40 %); a saved, loaded, hosted or command-line value outside them is moved in. */
	static constexpr float MinGameSpeed = 0.2f;
	static constexpr float MaxGameSpeed = 0.75f;
	static float ClampGameSpeed(float Dial) { return FMath::Clamp(Dial, MinGameSpeed, MaxGameSpeed); }
	/** The bad guys never drop below this pace however low the slider goes (user 2026-09-29: 30 %):
	 *  a comfort-speed player still gets enemies that move and shoot like enemies. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws")
	float EnemyPaceFloor = 0.3f;
	/** The player teleports (VR option): the game runs at half pace whatever the slider says (user 2026-09-28). */
	bool bTeleportPace = false;
	/** The movement speed dial (GameSpeed, shown x200 %) to the speed it gives (user 2026-10-03: "ninety percent movement
	 *  speed should be the new one hundred percent, and be the new default ... keep the other anchor points for the lowest
	 *  and highest settings the same"): through the dial's 0.2 (40 %) -> 0.2, 0.5 (100 %) -> 0.45 (the old 90 %), 1.0
	 *  (200 %) -> 1.0, straight between.  SpeedDialFor is its inverse (settings saved before it are moved over once). */
	static float SpeedFromDial(float Dial)
	{
		const float D = ClampGameSpeed(Dial);
		return D <= 0.5f ? 0.2f + (D - 0.2f) * (0.25f / 0.3f) : 0.45f + (D - 0.5f) * (0.55f / 0.5f);
	}
	static float SpeedDialFor(float Speed)
	{
		const float V = FMath::Clamp(Speed, 0.2f, 1.f);
		return V <= 0.45f ? 0.2f + (V - 0.2f) * (0.3f / 0.25f) : 0.5f + (V - 0.45f) * (0.5f / 0.55f);
	}
	/** The quickest pace any player's dial gives (what a co-op host takes from a client). */
	static float MaxPace() { return SpeedFromDial(MaxGameSpeed) * 0.5f; }
	/** The pace the player moves at: the dial's speed, or a half while teleporting. */
	float Pace() const { return bTeleportPace ? 0.5f : SpeedFromDial(GameSpeed) * 0.5f; }
	/** The pace the bad guys move at: the player's, but never under EnemyPaceFloor. */
	float EnemyPace() const { return FMath::Max(CoopEnemyPaceBase >= 0.f ? CoopEnemyPaceBase : Pace(), FMath::Clamp(EnemyPaceFloor, 0.1f, 1.f)) * CoopPaceScale; }
	/** Co-op: the bad guys 1.5 times as quick in a session (user 2026-10-02: "kind of like if you turned up walking speed");
	 *  1 in single player (OutlawsCoopEnemies.cpp sets it). */
	float CoopPaceScale = 1.f;
	/** Co-op (user 2026-10-02, decision 6): the pace the bad guys go from, the two players' average, and the pace the timed
	 *  doors and lifts wait at, the slower player's (OutlawsCoopRules.cpp sets both).  -1 in single player: Pace() itself. */
	float CoopEnemyPaceBase = -1.f;
	float CoopWaitPace = -1.f;
	/** Co-op's -OLCoopSpeed test flag set GameSpeed for its run: the settings file keeps this (-1 in single player). */
	float GameSpeedInFile = -1.f;
	/** Co-op (milestone 6, OutlawsCoopVerbs.cpp): a blast the other machine had, shown here (its sight, sound and this
	 *  machine's own player's hurt; the rest comes as events of its own), and the stick a Detonate is setting off (its
	 *  pickup's number).  Never set in single player. */
	bool bCoopRemoteBlast = false;
	int32 CoopBlastStick = -1;

	/** The menu's master and movie volumes (sound effects: SoundVolume, music: MusicVolume). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws")
	float MasterVolume = 1.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws")
	float MovieVolume = 1.f;
	/** The menu's Tinting: the sector palette grades (underwater and the like). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws")
	bool bPaletteGrades = true;
	/** The menu's Stochastic Textures (user 2026-09-28): natural ground and rock (dirt, gravel, sand, mud, rock,
	 *  canyon, cliff) drawn without a visible repeat; buildings and wood never. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws")
	bool bStochastic = true;
	/** Switch it (every world material of a natural texture takes it at once). */
	void SetStochastic(bool bOn);
	/** The options' Multiplayer page (user 2026-10-02), saved at once in OutlawsSettings.ini with the rest:
	 *  "MpMicrophone" 0 / 1 (default 0): this player's voice goes to the other player (normal loudness in the lobby,
	 *  placed in the level in play); the voice chat reads bMpMicrophone (loaded with the other settings, changed live by
	 *  the menu).  "MpPlayerName": the name the other player sees over this player's head in co-op (at most MpNameMax
	 *  characters; empty, the default: no name line, the health bar alone). */
	bool bMpMicrophone = false;
	FString MpPlayerName;
	/** R8: "MpPushToTalk" 0 off (an open microphone), 1 the left grip, 2 the right grip, 3 either; "MpMicSensitivity" 0 low,
	 *  1 normal, 2 high (the voice gate's level: ol.VoiceGate set from it). */
	int32 MpPushToTalk = 0;
	/** R14 (user 2026-10-03: "the host should be able to choose an enemy multiplier, maybe a slider with a maximum of four
	 *  times"): the co-op host's bad guys, the total multiplier of the level's ordinary ones (1 to 4 in quarters), 0 = Auto
	 *  (1 + 0.5 per player beyond the first, at most 4).  "CoopBadGuys" in OutlawsSettings.ini ("auto" or the number). */
	float CoopBadGuys = 0.f;
	/** REQ-SPMULT (user 2026-10-05): co-op's bad guys multiplier in single player (Settings > Gameplay), 1 to 4 in quarters,
	 *  1 = off (the default).  The same list as a co-op host's (OutlawsCoopExtras.cpp): the level's ordinary bad guys only,
	 *  keys and the INF's story bad guys stay single.  "SoloBadGuys" in OutlawsSettings.ini; -OLSoloBadGuys=<x> for a test. */
	float SoloBadGuys = 1.f;
	/** N1 (user 2026-10-03: "push co-op to 8 players"): the co-op host's lobby row, the most players its game lets in (2 to 8;
	 *  a Quest host 4 until its cost is measured).  "CoopMaxPlayers" in OutlawsSettings.ini. */
#if PLATFORM_ANDROID
	int32 CoopMaxPlayers = 4;
#else
	int32 CoopMaxPlayers = 8;
#endif
	int32 MpMicSensitivity = 1;
	/** REQ-VOICEVOL (user 2026-10-05: voices quiet on both sides): "MpPartnerVolume" the other players' voices, percent
	 *  (50 to 300, default 200: a boost); "MpVoiceFade" 0 / 1 (default 0): the voice fades with distance in a level (off:
	 *  placed and panned, never quieter far off). */
	int32 MpPartnerVolume = 200;
	bool bMpVoiceFade = false;
	static constexpr int32 MpNameMax = 16;
	/** A name as it may be kept and shown: no line breaks or other control characters, the ends trimmed, MpNameMax at most. */
	static FString CleanMpName(const FString& In);
	/** -OLMpName=<name> (a co-op test's own name for this instance): the settings file keeps MpPlayerNameInFile. */
	bool bMpNameArg = false;
	FString MpPlayerNameInFile;
	/** R10 (user 2026-10-03): the options' Randomizer page, saved in OutlawsSettings.ini: "RandoEnemies" 0 off, 1 the same
	 *  strength, 2 anything; "RandoItems" 0 off, 1 shuffle places, 2 re-roll; "RandoOrder" 0 off, 1 shuffle the middle (the
	 *  hideout first, the finale last), 2 shuffle all; "RandoSeed" empty for a new seed at each new game, or what was typed.
	 *  A run takes them at New Game (OutlawsLevelActorRando.cpp).  Single player only for now: off in a co-op session. */
	int32 RandoEnemies = 0;
	int32 RandoItems = 0;
	int32 RandoOrder = 0;
	int32 RandoBosses = 0;                 // (REQ-RANDOBOSS) "Bosses in the mix": 0 off, 1 on (with Enemies on)
	int32 RandoCritters = 0;               // (REQ-RANDOCRITTERS) "Critters": 0 off (only gunmen as replacements), 1 on
	FString RandoSeedText;
	static constexpr int32 RandoSeedMax = 10;
	/** A run's randomizer: fixed at New Game from the page (or from the first level built, a test's), saved with the game. */
	struct FRandoRun { bool bSet = false; int32 Enemies = 0; int32 Items = 0; int32 Order = 0; int32 Bosses = 0; int32 Critters = 0; uint32 Seed = 0; };
	FRandoRun RandoRun;
	/** Any option on in this run (in co-op, the host's run, handed to every machine with the start). */
	bool RandoOn() const;
	/** REQ-COOPRANDO: the attempt the next build of Level takes (rule 1), worked out before it (the co-op host's, for the start). */
	int32 NextRandoAttempt(const FString& Level) const;
	/** The page's settings (and a test's -OLRando=<enemies>,<items>,<order> / -OLRandoSeed=<n>) become this run's. */
	void BeginRandoRun(const TCHAR* Why);
	/** The seed a typed text stands for: its number when it is all digits, else a hash of it. */
	static uint32 RandoSeedFromText(const FString& Text);
	/** The level's attempt (rule 1: every start and restart rolls again; a load keeps the save's): which level it counts for,
	 *  the load's own for the next build, and the build's roll checksum (0 when nothing changed). */
	int32 RandoAttempt = 0;
	FString RandoAttemptLevel;
	int32 RandoPendingAttempt = -1;
	uint32 RandoSum = 0;
	/** REQ-UNLOCKS (OutlawsUnlocks.h): the main story's last level was won on this difficulty; earned when its cutscene (and
	 *  the credits) end, or when the co-op session ends after it.  -1: nothing waiting. */
	int32 UnlockStoryPending = -1;
	bool UnlockNoLevelAfter() const { return NextLevelName.IsEmpty(); }
	/** REQ-SPMULT under REQ-UNLOCKS: the single-player bad guys multiplier the next build makes (the settings' once the
	 *  multiplier is unlocked, else 1; a test's -OLSoloBadGuys=<x> always). */
	float SoloBadGuysWanted() const;
	/** The run (and this level's attempt) into a save, and back (a save without it: the randomizer off for that game). */
	void WriteRandoSave(class FJsonObject& O, bool bOpening) const;
	void ReadRandoSave(const class FJsonObject& O);
	/** The story's levels (OUTLAWS.RCS, the HD builds) and the movie after each, in the original order. */
	bool RandoStory(TArray<FString>& OutLevels, TArray<FString>& OutMoviesAfter, TArray<FString>* OutSdNames = nullptr) const;
	/** This run's level order (the story's, shuffled by RandoRun.Order from the run's seed). */
	void RandoLevelOrder(TArray<FString>& Out) const;
	/** StoryNext in a shuffled run: the next level by the run's order, the cutscene by position.  False: not in the run's story. */
	bool RandoStoryNext(FString& OutMovie, FString& OutNext) const;
	/** One level's roll: the rows renamed (bad guys to another kind, pickups to another item), what is pinned and why. */
	struct FRandoRoll
	{
		TMap<int32, FString> Rename;                   // row -> the name it is built as (upper case)
		TMap<int32, TMap<FString, FString>> Keep;      // a bad guy's own BASE_MODE, WATCH_CHOR, DROP_ITEM ("" = he had none)
		TArray<FString> Pinned;                        // "name (why)", one per pinned pickup
		TSet<int32> RandomBoss;                        // (REQ-RANDOBOSS) rows built as a named outlaw that ends nothing
		TMap<FString, int32> ItemKinds;                // (tests) the pickups' kinds after the roll, by item class
		int32 Eligible = 0, Swapped = 0, Movable = 0, Changed = 0, PinnedMoved = 0;
		int32 SpecialPicks = 0;                        // (REQ-RANDOCRITTERS) bad guys drawn as a critter or a special enemy
		int32 OwnSpecials = 0;                         // (tests) the critters and special enemies the level itself places (kept)
		bool bCountsKept = true;                       // Shuffle: the same items, only their places changed
		bool bWeaponsKept = true;                      // every weapon the level places is still placed somewhere
		uint32 Sum = 0;
		FString Kinds;
	};
	bool RollRando(const FOutlawsLevel& L, const FString& Level, const FString& InfText, uint32 DiffBit, const FRandoRun& Run, int32 Attempt, FRandoRoll& Out) const;
	/** Build's part: the attempt counted, the roll made and put on the level's rows (L is Build's own copy). */
	void PrepareRando(FOutlawsLevel& L, const FString& InfText, uint32 DiffBit, FRandoRoll& Out);
	/** -OLRandoTest (OutlawsLevelActorRando.cpp). */
	void TickRandoTest(float Dt);
	/** -OLRandoBossTest (RANDOPOOL, RANDOBOSS): the item kinds over 20 seeds before and after, the random bosses per level, each killed without the level ending. */
	void TickRandoBossTest(float Dt);
	/** -OLRandoCritterTest (REQ-RANDOCRITTERS): the town's critters and special enemies over three seeds, Critters off and on, the old weights and the new. */
	void TickRandoCritterTest(float Dt);
	/** (BUG-RESPAWNCRASH) -OLMemSoak[=plan]: level rebuilds one after another, the memory logged after each (OutlawsMemSoak.cpp). */
	void TickMemSoak(float Dt);
	/** One line of the process's memory and the counts that grew with it: textures, meshes, our caches (OLMEM). */
	void LogMemSnapshot(const FString& Tag, bool bClasses);
	/** The process's resident memory now, in MB (the working set on a PC, VmRSS on the Quest). */
	static double ResidentMB();
	/** The system's free swap in MB (the Quest's zram); -1 where there is none to read. */
	static double SwapFreeMB();
	double LastBuildBeforeMB = 0.0, LastBuildAfterMB = 0.0;     // (the soak's table) the last rebuild's two collections, then its build
	double SoakEmptyMB = 1e12;          // (-OLMemSoak) the least of the empty world's samples before this build
	FString LastBuiltLevel;             // (BUG-RESPAWNCRASH) the level the caches were filled for: emptied when another is built
	/** Before a rebuild's Build: the caches of another level emptied, the freed memory handed back (the old level collected twice). */
	void ReleaseBeforeBuild();
	TArray<TWeakObjectPtr<class AOutlawsEnemy>> RandomBossActors;
	/** (BUG-HOMCELL) the figure generators by object id (the office's cell outlaws): shown on the INF's MASTER_ON to them. */
	TMap<FString, TWeakObjectPtr<class AOutlawsSpriteActor>> CellGens;
	/** A level texture of natural ground or rock, by its name (GFDIRTMN, GWREDROK, GWCANYON, FGRND00, GFBRN2...). */
	static bool IsNaturalSurface(const FString& Stem);

	/** The saved menu settings (Saved/OutlawsSettings.ini, key=value lines). */
	static TMap<FString, FString> ReadSettings();
	static void WriteSettings(const TMap<FString, FString>& Settings);
	/** Take this actor's settings from the file (before the build) / put them in a map to save. */
	void LoadSettings();
	void GatherSettings(TMap<FString, FString>& Out) const;
	/** The music's loudness changed (master or music volume): the playing track follows. */
	void ApplyMusicVolume();

	/** The menu's Remastered Graphics switch: every level texture swaps at once, no rebuild. */
	void SetHDTextures(bool bOn);
	/** The player pressed Remastered Graphics (saved as RemasteredGraphicsChosen): until then the headset's default
	 *  holds (off under the Quest 2 profile, OutlawsDeviceProfile), and the file's RemasteredGraphics is kept as it was. */
	bool bHDChosen = false;
	bool bHDTexturesInFile = true;
	/** The headset's default for HD textures, when the player has not chosen (the Quest 2 profile switched live). */
	void ApplyHDDefault();
	/** The remastered sounds switch (new sounds from now on). */
	void SetHDSounds(bool bOn);
	/** The boss's death animation has played out: the level is won. */
	void OnBossDefeated();
	/** The level is won (the boss, or the INF END_LEVEL / COMPLETE): the story movie, then the next level. */
	void CompleteLevel(const FString& Why);
	/** The level's INF ends it itself (a MESSAGE: END_LEVEL / COMPLETE: the train's LAST_DOOR, the sheriff missions):
	 *  then killing its boss does not (user 2026-09-28: the train ended on Bloodworth's death, not at the door). */
	bool bInfEndsLevel = false;
	/** A 3DO item model (the gatling, dynamite, knife held in the hand), textured from the level. */
	bool BuildItemModel(const FString& File, TMap<FString, FOutlawsMeshSection>& OutSections, TMap<FString, UTexture2D*>& OutTextures);
	/** Skip straight to the boss (debug: kills = total, as the exe's cheat does). */
	void DebugSummonBoss();
	/** Every level to jump to (cheat): the story in order (OUTLAWS.RCS, HD builds), then the rest. */
	void GetLevelChoices(TArray<FString>& OutNames, TArray<FString>& OutLabels) const;
	/** Load another level now (the cheat's level skip). */
	/** The level skip cheat.  bCarry (the menu's row; BUG-SKIPLEVELINV): the pockets go along as a won level carries them; false: the start kit. */
	void JumpToLevel(const FString& Name, bool bCarry = false);
	/** The pockets as one line (sorted items, gold, armour), for the tests. */
	FString CarrySignature() const;
	/** Debug (-OLShatterTest): break pane i. */
	void DebugShatter(int32 Index) { if (Panes.IsValidIndex(Index)) ShatterPane(Index); }
	/** (tests, BUG-GLASS8BIT) the pane of sector S's wall W (either side), -1 none; its break frames, broken, the frame shown. */
	int32 DebugPaneAt(int32 S, int32 W) const { return Panes.IndexOfByPredicate([S, W](const FPane& P) { return (P.S == S && P.W == W) || (P.MS == S && P.MW == W); }); }
	FString DebugPaneState(int32 Index) const;
	/** (tests) the level's SHADES as parsed (FOutlawsLevel::Shades). */
	TArray<FColor> DebugShades() const;
	/** (tests, BUG-SCROLLSRGB) the scroll sections' vertex colours as drawn: sections, those slid, vertices, as the build gave them
	 *  (the still faces' rule), as sRGB-converted, not black or white; the largest section's middle and a side to see it from. */
	void DebugScrollColours(int32& OutSections, int32& OutSlid, int32& OutVerts, int32& OutAsStill, int32& OutAsOld, int32& OutLit, FVector& OutAt, FVector& OutFacing,
		FString& OutWhere) const;
	/** A story movie is on the screen (between levels): the player only watches, a trigger skips. */
	bool IsMoviePlaying() const;
	void SkipMovie();
	/** The movie held behind the pause menu, and toned as the world is there (user 2026-10-01). */
	void PauseMovie(bool bPause, float Tone);
	/** The movie's first second cannot be skipped; a held button shows "keep holding" on the screen. */
	bool CanSkipMovie() const;
	void SetMovieSkipHold(float Held, float Need, int32 Buttons = 1);
	AOutlawsEnemy* GetBoss() const { return Boss.Get(); }
	AOutlawsEnemy* DebugBossByFunc(const FString& Func) const
	{
		for (const FBossSlot& S : BossSlots) if (S.Func.Equals(Func, ESearchCase::IgnoreCase) && S.Actor.IsValid()) return S.Actor.Get();
		return nullptr;
	}
	bool DebugLevelEnding() const { return bBossDefeated || LevelEndTimer >= 0.f; }
	// ---- the front end (user 2026-09-27): the opening movie, then the remaster's main menu on the player's own
	// panel; Main Game plays the story's first movie into the hideout, a historical mission loads at once ----
	bool bFrontEnd = false;                 // the title flow is up: no level
	bool bMovieToMenu = false;              // the movie playing leads to the main menu, not a level
	bool bFrontPending = false;             // the player is not placed yet: the flow starts on the next tick
	bool bFrontSkipIntro = false;           // straight to the menu (End Game), no opening movie
	void StartFrontEnd(bool bSkipIntro);
	void ShowFrontMenu();
	void StartStory(EOutlawsDifficulty InDifficulty);
	void StartMission(const FString& Level, EOutlawsDifficulty InDifficulty, TFunction<void()> Then = nullptr);
	/** Historical Missions as the original runs them (HM1, OutlawsHom.cpp; sheriff.rca): the intro crawl, then the
	 *  Marshal's office, its wanted posters starting the missions and the horse bringing the player back. */
	void StartHom(EOutlawsDifficulty InDifficulty);
	/** REQ-TUTORIAL (OutlawsTutorial.cpp): New Game > Tutorial: single player in town_2p (Sanctuary), one street walked
	 *  forward through cards that each wait for the action they teach; the horse at the end, back to the main menu.  No
	 *  save, no best time, no unlock, and the player cannot die. */
	void StartTutorial();
	/** The tutorial left: skipped from the pause page (bSkipped) or finished at the horse; back to the main menu. */
	void EndTutorial(bool bSkipped);
	bool IsTutorial() const { return bTutorial; }
	/** Tests: the card up now (its id, empty: none). */
	FString TutorialCardId() const;
	/** A tutorial card's line and hint for this player's settings (OutlawsTutorial.cpp; also the glyph test's cards). */
	static void TutorialCardLines(const class AOutlawsPlayer* P, const FString& CardId, FString& Line, FString& Hint);
	bool bTutorial = false;
	TSharedPtr<struct FOutlawsTutorialRun> TutorialRun;
	void TickTutorial(float Dt);
	void TickTutorialTest(float Dt);
	/** REQ-PERFTESTMODE (OutlawsPerfTest.cpp): New Game > Performance test: the story's second level alone (the run's second
	 *  with the mission order shuffled) as a story start there, at the difficulty last pressed on New Game (else Bad), with
	 *  what a hideout finish carries in, the capture on
	 *  from the moment play begins and nothing written until the main menu is back (OutlawsPerfTest.h).  bSweep: REQ-PERFSWEEP's
	 *  Settings sweep, the same start in The Town always (OutlawsPerfSweep.cpp). */
	void StartPerfTest(bool bSweep = false);
	/** REQ-PERFSWEEP (OutlawsPerfSweep.cpp): the sweep's start once play begins (the player's rows kept, god mode, the spot
	 *  and its marker, the phases, the start line), its phases and end card each tick, the rows set for a phase or put back,
	 *  the player put at the spot (the feet found under it) facing Yaw, and the table. */
	void PerfSweepBegin(const FString& CapturePath);
	void TickPerfSweep(float Dt);
	void PerfSweepApply(int32 Phase);
	void PerfSweepRestore(bool bWorld);
	bool PerfSweepPlace(const FVector& Spot, float Yaw, bool bTurn, FVector* OutFeet = nullptr);
	FString PerfSweepTable(const FString& Why) const;
	/** The run's start once its level is in play, and its end when that level is left some other way. */
	void TickPerfTest(float Dt);
	/** StartNextLevelThen's question: a run in its level goes back to the main menu instead of on (no level 3). */
	bool PerfTestLeave();
	/** What a finish of the run's first level carries in: James's start kit and the guns and ammunition the level places
	 *  for this difficulty (after the Randomizer's roll of it). */
	void PerfTestCarry(const FString& LevelOne);
	/** The settings dump's lines ("key: value"), from the values applied now. */
	FString PerfTestSettings(const FString& CapturePath) const;
	bool bHom = false;                      // a Historical Missions session (the office and its missions)
	/** HM1: SPAWN_LEVEL "<mission> <return sector>" from an office poster: the player is asked "Ride out to ...?" first. */
	void HomSpawnLevel(const FString& Args);
	/** HM1: the player said yes: the mission starts (its own start kit), the session and the way back kept. */
	void HomRide();
	FString HomMission;                      // the mission asked for / ridden to
	FString HomReturnSector;                 // its poster's return sector in the office (PORCHxD)
	FString HomMissionLabel() const;
	/** HM1: a mission's horse (its END_LEVEL): the outcome (captured, killed, escaped), the score, the way back to the office. */
	void HomMissionEnd();
	/** REQ-HMBOSSEND: a mission's outlaw is down, his fate settled there (1 captured: punched or stabbed out; 2 killed): the
	 *  original's two lines, local.msg 822/823 with his name (831..836), then 830.  A co-op host's word to every client. */
	void HomBossDown(int32 Outcome, int32 NameId);
	/** REQ-HMBOSSEND: the local.msg number of the outlaw's name, by his ITM as the original's by FUNC (SH_MARY 831 .. SH_BLOOD
	 *  835; anyone else 836, "the outlaw"). */
	static int32 HomBossNameId(const AOutlawsEnemy* B);
	/** REQ-HMBOSSEND: the mission's outlaw (the level's boss, else the first bad guy marked as one). */
	AOutlawsEnemy* HomBoss() const;
	bool bHomBossDown = false;               // (REQ-HMBOSSEND) the outlaw's lines shown this level
	/** HM1: the office built again: every poster's stamp and cell from the session so far, the rank badges, the result. */
	void HomOnOffice();
	int32 HomWounds = 0;                     // hits taken this mission (the results' Wounds)
	int32 HomGoldAtStart = 0;
	int32 HomKillsAtStart = 0;
	TMap<FString, int32> HomPorchEvents;     // return sector -> its event (131072 captured, 262144 killed), every mission so far
	int32 HomGameScore = 0;
	int32 HomTotalGold = 0, HomTotalKills = 0, HomTotalWounds = 0, HomTotalBounty = 0;
	FString HomResultText;                   // the last mission's result, shown on arrival back
	/** HM1: the results card (the original's captured.rml rows), shown beside the counts on the black between the mission
	 *  and the office (UOutlawsStats::Panels). */
	FString HomCardTitle;
	TArray<TPair<FString, FString>> HomCardRows;
	/** HM1: the session kept in Saved/OutlawsSaves/hom_autosave.json (each arrival back in the office): the stamps, the
	 *  totals, the score, the difficulty.  HomResume starts the office from it (no crawl). */
	bool SaveHom() const;
	static bool HasHomSave();
	void HomResume();
	/** (BUG-RESPAWNCRASH) the session as it stands (the mission and its way back, the totals, the stamps, the result and
	 *  card, the mission's own counts) for a map load's rebuild, and taken back in the fresh world before it builds. */
	void WriteHomCarry(class FJsonObject& O) const;
	void ReadHomCarry(const class FJsonObject& O);
	/** -OLHomTest (HM1): Marshal Training from the start, the crawl skipped, the Granary's poster, Mary knocked out by a
	 *  fist and brought to the horse, back in the office: the stamp's event, the score, the save.  Seconds since the start. */
	void TickHomTest(float T);
	int32 HomTestStep = 0;
	TMap<FString, FVector> DebugModelAt;      // (tests) where each 3D item model first stands
	/** (BUG-SLOPES test) the Nth sloped-floor sector standing 1.2 to 2.4 m over the walkable sector beside it: the ground
	 *  point in front of that wall, the lip's middle, its height and the way in (UE units).  False when none. */
	bool DebugRoofLip(int32 Nth, FVector& OutGround, FVector& OutLip, FVector& OutIn, int32& OutSector) const;
	/** (BUG-CLIMBCEIL) the ceiling over a point, in cm: the sector the point stands in (its floor at or under it); huge when none.
	 *  OutSector: that sector (-1 none). */
	double CeilingZAt(const FVector& P, int32* OutSector = nullptr) const;
	/** (BUG-STUCKGAP) the sector whose walls hold a point (UE cm) and whose floor..ceiling holds its height (slopes
	 *  included, Slack cm either way): its index; -1 outside every sector; -2 no level. */
	int32 SectorHoldingPoint(const FVector& P, double Slack = 2.0) const;
	/** (BUG-STUCKGAP) a standing body's centre in sector Si at its floor, near its middle (the centroid when the sector
	 *  holds it, else the nearest clear inside point), the capsule clear of walls and objects. */
	bool SectorMiddleSpot(int32 Si, float Radius, float Half, const AActor* Ignore, FVector& OutCentre) const;
	/** (tests, -OLNarrowWalkTest) a line down the middle of the sector's floor plan: three points along each wall at
	 *  least MinWall long, in ring order, pushed in from it to halfway across (UE x/y, z at the floor's height); the narrowest width
	 *  across met (cm) and the line's length (cm).  False for a missing sector or one with no such wall. */
	bool SectorWalkLine(int32 Si, double MinWallCm, TArray<FVector>& Out, double& OutWidthCm, double& OutLengthCm) const;
	/** (tests) a line about the sector: its id, name, floor, ceiling, flags and its walls' adjoins with their floors */
	FString DescribeSector(int32 Si) const;
	int32 NumSectors() const;
	/** (tests) the sector's room over its floor (cm) and how many of its walls drop to a lower floor beyond */
	double SectorHeadroomCm(int32 Si, int32& OutDrops) const;
	/** (tests) the first box: a raised sector 0.45 to 1.8 m over its neighbour, its ceiling low (under 2.4 m over its top) with
	 *  a room over it within 1.2 m: where to stand before it, its lip, the way in, the box's top, ceiling and the room's floor. */
	bool DebugCeilBoxSpot(FVector& OutStand, FVector& OutLip, FVector& OutIn, FString& OutWhat, double& OutTopZ, double& OutCeilZ, double& OutAboveZ) const;
	/** (BUG-STUCKGAP tests) the Nth such box in the level's order, the room over it anywhere up to 2.4 m over the box's
	 *  ceiling (the ceiling-to-floor gaps between them included); false past the last. */
	bool DebugCeilBoxSpotN(int32 Nth, FVector& OutStand, FVector& OutLip, FVector& OutIn, FString& OutWhat, double& OutTopZ, double& OutCeilZ, double& OutAboveZ) const;
	double HomTestAt = 0.0;
	double HomTestHorseAt = 0.0;            // (BUG-SIMMSINF) -OLHomHorse: when the real horse was gripped
	void ReturnToFrontEnd();
	bool IsFrontEnd() const { return bFrontEnd; }
	/** The historical missions (the sh* levels) and their names: Marshal Training's five (REQ-HMCAREER: never a row of
	 *  their own on the Historical Missions page). */
	static void GetMissionChoices(TArray<FString>& OutNames, TArray<FString>& OutLabels);
	/** REQ-HMCAREER (user 14 Oct): one of Marshal Training's five missions, by its level file (shfort, shgranry, shgulch,
	 *  shsimms, shwildns). */
	static bool IsCareerMission(const FString& Level);
	/** REQ-HMCAREER: the Handful of Missions (civilwar.rca, icecaves.rca, villa.rca, wharf.rca): each mission's first level and
	 *  its name (the remaster's m_local_26001..26004); with Assets, only the missions whose first level the LABs hold. */
	static void GetHandfulChoices(TArray<FString>& OutNames, TArray<FString>& OutLabels, const FOutlawsAssets* Assets = nullptr);
	/** REQ-HMCAREER: a Handful mission's level (any of its levels, Civil War's second included). */
	static bool IsHandfulLevel(const FString& Level);
	const FOutlawsAssets* GetAssets() const { return AssetsRef.Get(); }
	/** -OLHmCareerTest[=fit] (REQ-HMCAREER): the Historical Missions page without the five and with every Handful mission, its
	 *  rows' fit and pointer boxes; then (not fit) Marshal Training to its office and the Fort's poster, a save there, an
	 *  old-style save in the Fort (the single-mission start the page used to make), and both loaded back. */
	void TickHmCareerTest(float Dt);
	/** Ground near a place for a player who fell out of the map (user 2026-09-27): the nearest bad-guy spawn
	 *  point with no bad guy standing on it, else the level start. Pawn centre height. False with no level. */
	bool FindGroundNear(const FVector& Near, float HalfHeight, FVector& OutCentre) const;
	/** The lowest floor of the level (UE cm); below it by far is out of the map. */
	float LowestFloorZ() const { return LowestFloor; }
	TArray<FVector> EnemySpawns;            // where the level put its bad guys (feet)
	float LowestFloor = 0.f;
	/** A movie on the screen in front of the player (the level cleared away); OnFinished goes to StartNextLevel.
	 *  False (nothing shown) when the file is missing. */
	bool PlayMovie(const FString& Name, bool bHardCut);
	/** The INF's TRIGGER_MOVIE (user 2026-10-03: the ranch's boss door, "as it starts to open, the cutscene plays. When the
	 *  cutscene finishes, the game resumes at the same spot with the door open, and Bob starts to step out and shoot"):
	 *  once per level, the movie over the level (the world held still, the player too), then the level again where it was;
	 *  then the curtain boss wakes (WakeCurtainBoss).  No movie converted: straight to the wake.
	 *  BUG-RANCHDOOR (user 2026-10-12): the use that set it off starts the fade at once, and the doors that use opens wait
	 *  shut (FOutlawsInfRuntime::HoldEventStarts) until the movie covers the level (StartMidMovieNow).  Co-op: the host's
	 *  INF decides (a client's use runs there); the host plays it and tells its clients to play it too (bFromHost). */
	void TriggerMidLevelMovie(const FString& File, bool bFromHost = false);
	/** The level INF's TRIGGER_MOVIE file (a co-op client's copy of the host's word), empty when it has none. */
	FString InfMidMovieFile() const;
	/** BUG-DRDEATHANIM: the bosses whose fall is in the cutscene their level ends on: Dr. Death in the mine and Bob at the
	 *  ranch, by the level and the outlaw's FUNC (Henry at the mill is a DrDeath_New too, and falls as before). */
	static bool BossFallsInCutscene(const FString& Level, const FString& Func);
	int32 MidMovieShown = 0;           // (tests: the mid-level movie put over the level)
	double MidMovieDoorAtShow = -1.0;  // (tests: the furthest a held door had moved when it was let go)
	void TickBossTest(float Dt);
	void TickRanchDoorTest(float Dt);
	void TickBossTauntTest(float Dt);
	void TickThrottleBossTest(float Dt);
	/** REQ-CREDITS (OutlawsLevelActorCredits.cpp): the story's end (outlaws.rcs CREDITS: rae.san, the ride-off) rolls the
	 *  credits: the Outlaws VR ones (Content/Credits/vr_credits.txt) then the original's (credits.txt in the player's LABs).
	 *  ArmEndCredits puts them on the end movie's screen (PlayMovie); StartEndCreditsIfDue plays them on the black when that
	 *  movie never rolled them (missing, failed), then the end goes on as before (StartNextLevel). */
	bool IsStoryEndMovie(const FString& MovieName) const;
	bool BuildEndCredits(TArray<FOutlawsCreditLine>& Out) const;
	void ArmEndCredits(const FString& MovieName);
	bool StartEndCreditsIfDue();
	/** REQ-EXTRAS: Extras > Credits on the main menu: the end credits roll on the black (the usual skip ends it), then the Extras
	 *  page again.  False: nothing to roll (the main menu stays). */
	bool PlayExtrasCredits();
	void ExtrasCreditsDone();
	void TickCreditsTest(float Dt);
	/** (BUG-CLIMBBARS) tests: the first wall with a barred see-through middle (a mid texture that stops the body), on the floor or
	 *  a sill up to 1.8 m: where to stand before it, a lip on the bars at chest height, the way in. */
	bool DebugBarsSpot(FVector& OutStand, FVector& OutLip, FVector& OutIn, FString& OutWhat) const;
	int32 BossTauntsMade = 0;          // (R12's test counts them)
	void OnMidLevelMovieFinished();
	/** Bob Graham behind his curtain (bPeekShooter): the cloth in front of him stops hiding him and stops stopping shots
	 *  (both ways), and he knows the player is there: alerted, his peek and shoot from then on (user 2026-10-03). */
	void WakeCurtainBoss(const TCHAR* Why);
	bool bMidMovieDone = false;
	bool bMidMovieOn = false;
	bool bMidMovieEnemiesWere = true;
	/** BUG-BOBCUT: the level hidden behind a mid-level movie (the black cinema, as at a level's end), kept loaded: what was
	 *  hidden, to show again after. */
	TArray<TWeakObjectPtr<AActor>> MidMovieHidden;
	FString MidMoviePath;
	void HideWorldForMovie(bool bHide);
	void StartMidMovieNow();
	void ReleaseMidMovieDoors(const TCHAR* Why);
	/** BUG-BOBSOUND (user 2026-10-04): the level's sounds (ambient, bad guys, music, doors) at a third while a mid-level movie
	 *  plays, back after; the movie's own sound is spared.  New sounds and the music take LevelSoundDuck as they start. */
	void DuckLevelSound(bool bOn);
	float LevelSoundDuck = 1.f;
	TArray<TWeakObjectPtr<UAudioComponent>> DuckedSounds;
	/** BUG-BOBDEATH: the level ends now on a cutscene that is there to play (the boss's fall is in it). */
	bool EndMovieReady() const;
	bool IsLevelWon() const { return bBossDefeated; }

	/** Place the objects of this single-player difficulty (the remaster's Good / Bad / Ugly). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws")
	EOutlawsDifficulty Difficulty = EOutlawsDifficulty::Good;
	/** The difficulty this level's objects were placed for (Build): the chosen one at the level's start; a save keeps it,
	 *  so its bad guys are the same ones when it loads after the choice has changed (user 2026-10-02). */
	EOutlawsDifficulty SpawnDifficulty = EOutlawsDifficulty::Good;
	int32 PendingSpawnDifficulty = -1;          // a load's, for the build it waits on (-1: the chosen one)
	/** The Gameplay options' Difficulty, changed in play: the bad guys here take that difficulty's speed, aim, damage and
	 *  health at once; who is placed changes from the next level (or a restart of this one). */
	void ChangeDifficulty(EOutlawsDifficulty To);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws")
	bool bBuildOnBeginPlay = true;

	/** Replace the game mode's stand-in pawn with AOutlawsPlayer (VR with a headset, else mouse and keyboard). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws")
	bool bUseOutlawsPlayer = true;

	/** Spawn the bad guys (ITM FUNC BGY_New) as live enemies in game worlds; editor builds show them as sprites. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|Enemies")
	bool bSpawnEnemies = true;

	/** Enemies think, move and shoot (off: they stand at their posts; tests and scripted scenes). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|Enemies")
	bool bEnemiesActive = true;

	/** How far (cm) a gunshot wakes enemies (research: a shot makes a noise of 150 units). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|Enemies")
	float GunshotNoiseCm = 4500.f;

	/** True when P is inside a water sector; OutSurfaceZ = its surface (the sector ceiling), cm. */
	UFUNCTION(BlueprintCallable, Category = "Outlaws|VR")
	bool WaterAt(FVector P, float& OutSurfaceZ) const;
	/** (BUG-SURFACEJOLT) P in a water sector: the surface over it (up through stacked water sectors, a sloped ceiling's
	 *  height at P), the top water sector, and the sector P is in; true whether P is under the surface or not. OutAir: the top
	 *  water sector has air on its ceiling (a real surface; else a flooded passage's solid roof). */
	bool WaterSurfaceAt(const FVector& P, float& OutSurfaceZ, int32& OutTopSector, int32* OutSector = nullptr, bool* OutAir = nullptr) const;
	/** (BUG-SURFACEJOLT test) a water sector's heights, current and stacking as the swim reads them, one line */
	FString DebugWaterSectorInfo(int32 Sector) const;
	/** (BUG-SURFACEJOLT test) a point well inside a sector (furthest from its walls on a grid), at its ceiling */
	bool DebugPointInSector(int32 Sector, FVector& OutAtCeiling) const;
	/** (BUG-WATERHANDS test) eyes over / a hand under each water surface: how many WaterSurfaceHit finds (and the old rule) */
	FString DebugWaterSurfaceCheck(int32& OutChecked, int32& OutFound, int32& OutOldFound, int32 OnlySector = -1) const;

	/** Tests (user 2026-09-29): a shootable prop by OBT name reacts as if shot (the number found); a nudge on wall
	 *  NAME(#id) as a hand would give it; where that wall's middle is; the INF elevators named so and their sectors'
	 *  live heights; a sector's floor-plan middle on its floor (by name or index); the ATX programs' states. */
	int32 DebugShootProp(const FString& Name);
	int32 PlayerSectorIndex() const { return PlayerSector; }
	bool DebugSectorDrawn(int32 S) const { return VisibleSectors.IsValidIndex(S) && VisibleSectors[S]; }
	int32 DebugSectorsDrawn() const { int32 N = 0; for (TConstSetBitIterator<> It(VisibleSectors); It; ++It) ++N; return N; }
	int32 DebugNumSectors() const;
#if WITH_OUTLAWS_TESTS
	void DebugMovingFloors(TArray<int32>& OutSectors, TArray<FString>& OutNames, TArray<FVector>& OutTops) const;
	double DebugFloorZ(int32 Sector, const FVector& At) const;   // the sector's floor under At now (cm)
#endif
	bool DebugNudgeWall(const FString& Name, const FString& WallId);
	bool DebugWallSpot(const FString& Name, const FString& WallId, FVector& Out) const;
	FString DebugElevatorReport(const FString& Prefix) const;
	FString DebugDoorProbe(const FVector& Eye, const FVector& Hand) const;   // what a hand push there finds (-OLTrainDoorTest)
	FString DebugWallProbe(const FVector& Feet, double Radius) const;        // the walls and collision around a spot (-OLWallStuckTest)
	FString DebugSectorBrief(int32 Sector) const;                             // name, flags, heights, extent and movers of a sector
	bool DebugSectorSpot(const FString& NameOrIndex, FVector& OutFloorPoint) const;
	/** (-OLWallShot) where to stand to see one wall (its sign, if it has one) from BackUnits in front of it: the floor
	 *  there and the point looked at, in Unreal space. */
	bool DebugWallView(int32 Sector, int32 Wall, double BackUnits, FVector& OutEye, FVector& OutAt) const;
	FString DebugSectorVerts(const FString& Name, int32 Count) const;    // the live (morphed) first vertices of the named sectors
	bool DebugWallSlides(int32 Sector, int32 Wall) const;
	FString DebugAtxReport() const;
	/** A point 60 cm under the surface of the deepest open water (tests). */
	UFUNCTION(BlueprintCallable, Category = "Outlaws|VR")
	bool FindWaterPoint(FVector& OutPoint) const;
	/** Debug views (-OLShots): the Nth see-through mid on an opening (glass or not), seen from 2 m
	 *  inside its own sector: where the eye stands (the floor there) and the yaw facing it. */
	bool DebugMidView(int32 Nth, bool bShatter, FVector& OutFloor, float& OutYaw, FString& OutWhat) const;

	/** Build in the editor when the map opens and whenever LevelName / HD / InstallPath change. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws")
	bool bBuildInEditor = true;

	/** Move the player to the OBT PLAYER object after building. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws")
	bool bPlacePlayer = true;

	/** Emit triangles in Unreal's front-face order (verified with a floor trace; flip only to test). */
	UPROPERTY(EditAnywhere, AdvancedDisplay, Category = "Outlaws")
	bool bReverseWinding = true;

	UPROPERTY(EditAnywhere, Category = "Outlaws|Assets")
	TObjectPtr<UMaterialInterface> WorldMaterial;

	UPROPERTY(EditAnywhere, Category = "Outlaws|Assets")
	TObjectPtr<UMaterialInterface> WorldMaskedMaterial;

	/** See-through mid textures (windows, fences): translucent with the texture's own alpha, so the
	 *  remaster's half see-through HD glass shows (M_OLR_WorldGlass, ue_olue_glass_material.py). */
	UPROPERTY(EditAnywhere, Category = "Outlaws|Assets")
	TObjectPtr<UMaterialInterface> WorldGlassMaterial;

	/** ... and their solid parts drawn again under the glass, masked at 0.95 so they write depth and near and far mids
	 *  sort pixel by pixel (M_OLR_WorldMaskedSolid, ue_olue_masked_solid_material.py; user 2026-10-01). */
	UPROPERTY(EditAnywhere, Category = "Outlaws|Assets")
	TObjectPtr<UMaterialInterface> WorldMaskedSolidMaterial;

	UPROPERTY(EditAnywhere, Category = "Outlaws|Assets")
	TObjectPtr<UMaterialInterface> WaterMaterial;

	UPROPERTY(EditAnywhere, Category = "Outlaws|Assets")
	TObjectPtr<UMaterialInterface> SkyMaterial;

	UPROPERTY(EditAnywhere, Category = "Outlaws|Assets")
	TObjectPtr<UMaterialInterface> SpriteMaterial;

	/** REQ-FIREBRIGHT2: M_OL_Sprite with a Mask of the pixels the light dims (SpriteMaterialFor). */
	UPROPERTY(EditAnywhere, Category = "Outlaws|Assets")
	TObjectPtr<UMaterialInterface> SpriteFireMaterial;

	UPROPERTY(EditAnywhere, Category = "Outlaws|Assets")
	TObjectPtr<UStaticMesh> SpriteQuad;

	/** Summary of the last build (counts, timings, validation). */
	UPROPERTY(VisibleAnywhere, Transient, Category = "Outlaws")
	FString LastBuildReport;

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Outlaws")
	void Build();

	UFUNCTION(CallInEditor, BlueprintCallable, Category = "Outlaws")
	void Clear();

	UFUNCTION(BlueprintCallable, Category = "Outlaws")
	FString GetBuildReport() const { return LastBuildReport; }

	/** Trace down under every floor-standing sprite object; returns "tested N, misses M: names". */
	UFUNCTION(BlueprintCallable, Category = "Outlaws")
	FString RunFloorCheck();

	UFUNCTION(BlueprintCallable, Category = "Outlaws")
	FVector GetPlayerStartLocation() const { return PlayerStartLocation; }

	UFUNCTION(BlueprintCallable, Category = "Outlaws")
	float GetPlayerStartYaw() const { return PlayerStartYaw; }

	/** Co-op (OutlawsCoop.cpp): this machine's player starts at At instead (the second player beside the first), and is put
	 *  there now if already placed. */
	void MovePlayerStart(const FVector& At);

	/** Number of actors spawned by the last build (sprites). */
	UFUNCTION(BlueprintCallable, Category = "Outlaws")
	int32 GetSpawnedActorCount() const { return SpawnedActors.Num(); }

	// ---- REQ-COLLECTBOX (OutlawsCollectBox.cpp): one hidden box a story level ----------------------------------------
	/** The box of this level put in its place, unless this profile found it already (or it is no story level, a match, the
	 *  tutorial, Historical Missions, or a changed level); the build report's line. */
	FString PlaceCollectBox(const FOutlawsLevel& Lv);
	/** Its turn and glint, and this machine's player walking into it. */
	void UpdateCollectBox(float Dt);
	/** A hand closing within Radius of the box takes it (VR, as a grip takes a pickup). */
	bool TakeCollectBoxByHand(const FVector& Eye, const FVector& Hand, float Radius);
	AActor* GetCollectBox() const { return CollectBoxActor.Get(); }
	/** A story level's box spot: its feet point, the walk test's start just outside its side room, its sector, why there. */
	static bool CollectBoxSpot(const FString& Level, FVector& OutFeet, FVector& OutWalkFrom, int32& OutSector, FString* OutWhy = nullptr);
	/** Placement's safety (the sector the feet pick, overlaps, zone gates) as found at placement; empty when not placed. */
	const FString& CollectBoxSafety() const { return CollectBoxSafe; }
	/** PathRoute / PathRouteWhy by the player's rules while set (fences pass, every door and lift opens: a key's doors with
	 *  the key the level holds); off, the bad guys' and bots' as ever.  The box test's reachability walk. */
	mutable bool bRoutePlayer = false;
	bool RoutePlayerDoor(int32 Sector) const;

	// ---- INF ------------------------------------------------------------------------------------
	/** Run the level's INF script: doors, lifts, moving floors and ceilings, switches. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|INF")
	bool bRunInf = true;

	/** Reach of the use key (cm). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|INF")
	float UseDistance = 250.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|INF")
	bool bPlaySounds = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|INF")
	float SoundVolume = 1.f;

	/** Press "use" from a view (nudges the sector or wall in front; also the sector you stand in). */
	UFUNCTION(BlueprintCallable, Category = "Outlaws|INF")
	void Use(FVector ViewOrigin, FVector ViewDirection);

	// (TIDY-8) the headless INF test's calls (tools/olue/ue_olue_inf_validate.py, editor Python only): editor builds only.
	// WITH_EDITOR, not WITH_OUTLAWS_TESTS: UHT takes a UFUNCTION only inside WITH_EDITOR or WITH_EDITORONLY_DATA blocks
#if WITH_EDITOR
	/** Send an INF message to a sector by name (TRIGGER = a nudge, NEXT_STOP, PREV_STOP, GOTO_STOP, MASTER_ON, MASTER_OFF). */
	UFUNCTION(BlueprintCallable, Category = "Outlaws|INF")
	void SendInfMessage(const FString& Target, const FString& Message, int32 Arg = 0);

	/** Advance the INF clock without playing (headless tests), in steps. */
	UFUNCTION(BlueprintCallable, Category = "Outlaws|INF")
	void AdvanceInf(float Seconds, float Step = 0.05f);

	/** "name: stop n, value v, moving/idle/waiting/terminated" for every elevator matching (empty = all). */
	UFUNCTION(BlueprintCallable, Category = "Outlaws|INF")
	FString ElevatorStates(const FString& NameFilter) const;
#endif // WITH_EDITOR

	/** Inventory by USE_ID (the number INF locks name: 100 iron, 101 brass, 102 steel key, 111 crowbar...). */
	UFUNCTION(BlueprintCallable, Category = "Outlaws|INF")
	void GiveItem(int32 UseId);
	/** Tools (the shovel, USE_ID 110, and the crowbar, 111) open their locks only while held in a hand
	 *  (user 2026-09-27): owning one is not enough. */
	static bool IsToolUseId(int32 Id) { return Id == 109 || Id == 110 || Id == 111; }     // the pen, the shovel, the crowbar
	void SetToolHeld(int32 UseId, bool bHeld);
	int32 UseIdOf(const FString& Item) const;
	/** A spade driven into the floor at this point: the floor's sector is nudged (the dig spot). */
	bool ToolStabFloor(const FVector& Tip);
	/** -OLDigTest: step 0 gives and holds the shovel and digs townex's DIG_ME once, 1 and 2 log and dig again. */
	void DebugDig(int32 Step);
	/** Test logs: the sectors just below and above a point, with their heights, flags and textures. */
	FString DescribeSectorsAt(const FVector& UE) const;
	/** -OLWalkTest=<sector>: the foot of a sloped sector (just inside its lowest corner) and the yaw up it. */
	bool SlopeFoot(int32 Sector, FVector& OutFeet, float& OutYaw) const;
	/** The floor under the feet: its FRICTION, whether it slides (a slope without NOSLIP), and its downhill
	 *  direction (UE, horizontal, length = the slope's tangent). False off the map. */
	bool FloorUnder(const FVector& Feet, float& OutFriction, bool& bOutSlides, FVector& OutDownhill) const;

#if WITH_EDITOR
	UFUNCTION(BlueprintCallable, Category = "Outlaws|INF")
	void TakeItem(int32 UseId);     // (TIDY-8: the headless INF test's, editor only as SendInfMessage)
#endif

	UFUNCTION(BlueprintCallable, Category = "Outlaws|INF")
	bool HasItem(int32 UseId) const;

	/** Every item this level's locks ask for (console: Outlaws.GiveKeys). */
	UFUNCTION(BlueprintCallable, Category = "Outlaws|INF")
	void GiveAllKeys();
	/** Where the items the level's locks need still lie, not yet taken (the map's hint blobs; user 2026-10-01). */
	void KeyItemSpots(TArray<FVector>& Out) const;
	/** REQ-MAPITEMS (user 16 and 20 Oct): the keys, stones, gears, crowbars, shovels and the other listed items (IsListedItem,
	 *  or one a lock needs) the player has seen lying about: within MapSeenCm of the eyes, inside MapSeenConeDeg of where
	 *  they look, nothing in the way.  Seen, one stays on the map (the wheel's and the held one) as its own ground sprite
	 *  until it is picked up; its hint blob gives way to it.  Per level, this machine's player only (co-op: each their own),
	 *  kept in saves ("map_seen" on the pickup). */
	static constexpr double MapSeenCm = 500.0;
	static constexpr double MapSeenConeDeg = 45.0;
	struct FMapItemMark
	{
		FVector Where = FVector::ZeroVector;
		UTexture* Tex = nullptr;         // its ground sprite's atlas (the HD one when shown) and the first cell's UV rect
		FVector2D UV0 = FVector2D::ZeroVector, UV1 = FVector2D(1.0, 1.0);
		float Aspect = 1.f;              // the cell's width over its height
		FString Item;                    // (no sprite: the player's wheel card for it)
	};
	/** The eyes this frame: any map item now seen is marked (a few times a second; cheap when nothing is near). */
	void UpdateMapSeen(const FVector& Eye, const FVector& Forward, float Dt);
	/** The seen items still lying about (not held in a hand), for the map. */
	void MapItemMarks(TArray<FMapItemMark>& Out) const;
	/** Changes whenever the map's marks do (the blobs and the seen items): the page is drawn again then. */
	int32 MapMarksSignature() const;
	float MapSeenWait = 0.f;
#if WITH_OUTLAWS_TESTS
	/** -OLMapItemsTest: the map items lying about (their pickup index, place, item, seen), seen state set or cleared. */
	struct FMapItemDebug { int32 Index = -1; FVector Where = FVector::ZeroVector; FString Item; bool bSeen = false; AActor* Actor = nullptr; };
	void DebugMapItems(TArray<FMapItemDebug>& Out) const;
	void DebugSetMapSeen(int32 Index, bool bSeen);
	void DebugClearMapSeen();
	bool DebugCanSee(const FVector& From, const FVector& To, const AActor* Target) const { return CanSee(From, To, Target); }
	bool DebugIsMapItemSeen(const AActor* A) const;
	static bool bDebugMapItemsOff;           // (the shots' "before": the map drawn without the items)
	/** -OLKnifePickupProbe: the pickups lying about (not in a hand), and how many placed ground items this build drew as
	 *  3D models (pickups or, -OLOldKnifePickup, level scenery). */
	struct FPickupDebug { AActor* Actor = nullptr; int32 Serial = -1; FString Ground; FVector Where = FVector::ZeroVector; };
	void DebugPickups(TArray<FPickupDebug>& Out) const;
	int32 DebugPlacedGroundModels() const { return PlacedGroundModels; }
	int32 DebugModelSerialsFrom() const { return ModelSerialsFrom; }
#endif
	/** A shot's line From..To past the hanging lamps: one it passes near swings; a chandelier's shade it passes through
	 *  breaks alone (user 2026-10-01). */
	void ShotNearHanging(const FVector& From, const FVector& To);
	/** A hand moving fast through a hanging lamp swings it; a hard smack breaks a chandelier's shade it strikes. */
	void HandNearHanging(const FVector& At, const FVector& Vel);

	/** Extra reach (cm) past the player capsule for walking over pickups. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|Items")
	float PickupReach = 30.f;

	/** Pick up every key / puzzle item a pawn standing here would touch; returns how many. */
	UFUNCTION(BlueprintCallable, Category = "Outlaws|Items")
	int32 CollectPickupsAt(FVector PawnLocation, float CapsuleRadius = 40.f, float CapsuleHalfHeight = 90.f, bool bFeetOnly = false);

	/** VR hands: take the key / puzzle item nearest Hand (within Radius cm) into a hand; null if none.
	 *  A held item is not collected by walking over it. */
	UFUNCTION(BlueprintCallable, Category = "Outlaws|Items")
	AActor* TakePickupNear(FVector Eye, FVector Hand, float Radius = 16.f);
	/** What TakePickupNear / PullPickupAlong / TakeBottleNear would take, left where it is (the grab glow). */
	AActor* FindPickupNear(FVector Eye, FVector Hand, float Radius = 16.f) const;
	AActor* FindPickupAlong(FVector Eye, FVector Hand, float MaxRange, float MaxAngleDeg) const;
	AOutlawsSpriteActor* FindBottleNear(const FVector& Hand, double Reach);
	int32 PickupNearIndex(FVector Eye, FVector Hand, float Radius) const;
	int32 PickupAlongIndex(FVector Eye, FVector Hand, float MaxRange, float MaxAngleDeg) const;
	/** The pickup the hand points at from the eye (the hand between the face and it), within range;
	 *  taken like TakePickupNear (no walk-over while it flies), its pickup sound played. */
	AActor* PullPickupAlong(FVector Eye, FVector Hand, float MaxRange, float MaxAngleDeg);

	/** A held item brought to the chest: into the inventory (message, sound), like walking over it. */
	UFUNCTION(BlueprintCallable, Category = "Outlaws|Items")
	bool CollectPickupActor(AActor* Item, bool bSilent = false);

	/** A held item let go elsewhere: it stays in the world, and is not collected again by walking
	 *  until the player has stepped away from it. */
	UFUNCTION(BlueprintCallable, Category = "Outlaws|Items")
	void DropPickupActor(AActor* Item, FVector Eye);

	/** VR hands: a hand pressed against a door or a scripted wall / switch uses it (like the use key,
	 *  without nudging the sector the player stands in).  False when nothing usable is there. */
	UFUNCTION(BlueprintCallable, Category = "Outlaws|INF")
	bool UseByHand(FVector Eye, FVector Hand, bool bIncludePlatforms = false);
	/** What UseByHand would reach, nothing fired (the glow on doors and switches, user 2026-10-01): a door leaf, a wall
	 *  whose script listens to a nudge, or an opening onto a door; where the hand meets it and its face. */
	bool HandUseSpot(FVector Eye, FVector Hand, FVector& OutAt, FVector& OutNormal) const;

	/** VR hands: the door (MORPH_*, MOVE_CEILING) a hand is touching, as its master sector, or -1.
	 *  bOutClosed is true when it rests at its first stop with nothing queued, so a push opens it. */
	int32 DoorAtHand(FVector Eye, FVector Hand, bool& bOutClosed, int32 FromSector = -1) const;

	/** A hand push: nudge that door sector alone (no wall triggers beside the frame). */
	bool PushDoorSector(int32 Sector);

	// ---- shootables (OutlawsLevelActorShoot.cpp): props, glass panes, ground dynamite ----
	/** A shot from From to End (where the world trace stopped): breaks the glass panes it crosses
	 *  (bullets pass through glass) and hits the nearest prop cylinder before End; a prop hit moves
	 *  End to it and returns true. */
	bool ShootAlong(const FVector& From, FVector& InOutEnd);
	/** A bullet into a dynamite crate / stick: it goes off. */
	bool HitDynamite(const UPrimitiveComponent* Comp);
	/** Glass panes crossed between two points break (enemy pellets use this too). */
	void ShatterAlong(const FVector& From, const FVector& To);
	/** A dynamite blast (pdynam.itm): falloff damage, props, glass and other dynamite in range. */
	void Explode(const FVector& Where);
	/** Blood from a wound at Where, thrown on along Dir (the shot's way): Count spurts (the lantern's embers, in red). */
	void SpawnBlood(const FVector& Where, const FVector& Dir, int32 Count);
	/** A spade driven into the ground (user 2026-09-28): clods thrown out round it and a puff of dust. Up: the ground's
	 *  normal. */
	void SpawnDirt(const FVector& Where, const FVector& Up, int32 Count);
	/** Headless test: shoot a bottle, a barrel, a window and a dynamite stick; the report comes back. */
	FString SelfTestShootables();
	// ---- bottles (user 2026-09-28): 3D, they burst into glass when shot, thrown or smashed, and a hand can take one
	/** The bottle nearest Hand within Reach cm of its model, taken out of the props (the caller copies its model, then
	 *  destroys the actor); null when none.  OutSound: the wav its break plays; OutScale: its model's world_scale. */
	AOutlawsSpriteActor* TakeBottleNear(const FVector& Hand, double Reach, FString& OutSound, float& OutScale, bool& bOutBreaks);
	/** The bottles and small props a hand could take now (standing, whole, with a model). */
	void GrabbableProps(TArray<AOutlawsSpriteActor*>& Out);
	/** An item's traits from its model json (backlog 4, user 2026-10-01: behaviour in data, item3d/traits.py), keys
	 *  absent where the json has none: "hold" {mode handle|neck|palm, in_hand}, "breaks" {on, sound}. */
	struct FItemTraits { FString HoldMode; float InHand = 0.f; int8 Breaks = -1; FString BreakSound; };
	static const FItemTraits& ItemTraits(const FString& Stem);
	/** How the hand takes it: the json's hold mode, else the Grabbable table's (BUG-PANHOLD: the pan and the saucepan by
	 *  their handles), else empty (the shape decides, AOutlawsPlayer::HoldModeOf). */
	static FString HoldModeFor(const FString& Stem);
	/** Take this one off its shelf (one GrabbableProps gave): as TakeBottleNear's. */
	AOutlawsSpriteActor* TakeBottle(AOutlawsSpriteActor* S, FString& OutSound, float& OutScale, bool& bOutBreaks);
	/** A bottle bursting: Src's triangles fly off as pieces of glass that tumble, land flat and shrink away, glitter
	 *  thrown out with them along Dir, Sound played there (empty: none; a shot prop's reaction plays its own). */
	void ShatterGlass(class UProceduralMeshComponent* Src, const FVector& Dir, const FString& Sound);
	/** The glitter alone: Count bits of glass sprite thrown out round Where, fading as they go. */
	void SpawnGlass(const FVector& Where, const FVector& Dir, int32 Count);
	/** Test hook: the middle of the 3D bottle nearest Near still standing on its shelf; false when none. */
	bool DebugBottleSpot(const FVector& Near, FVector& Out) const;
	/** Counts Clear()s: what the player carries of a level (a bottle) goes when it changes. */
	int32 GetClearCount() const { return ClearCount; }
	FString ShootablesReport() const;

	/** The footstep wav of the floor at a place (the sector's FLOOR SOUND), empty where silent. */
	FString FloorSoundAt(const FVector& Feet) const;
	/** The sector's GRAVITY at a place (units/s^2, negative is down; -60 where there is no sector). */
	float GravityAt(const FVector& Feet) const;
	/** The colour grade for the palette of the sector the eyes are in (rows: rgb weights, offset in
	 *  alpha), when it differs from the level's main palette. */
	bool PaletteGradeAt(const FVector& Eye, FLinearColor OutRows[3]) const;
	/** Underwater: the tint of the water the eyes are in (the average colour of its chain's surface
	 *  textures). False above water. */
	bool UnderwaterTintAt(const FVector& Eye, FLinearColor& OutTint) const;
	/** A test aid (-OLDrownTest): the deepest sector under water, -1 when the level has none. */
	int32 DebugWaterSector() const;
	/** A test aid (-OLTntBlueTest): "sector N palette P (main M)" at a place. */
	FString DebugSectorPaletteAt(const FVector& At) const;
	/** The first open water surface (a water sector's ceiling with a vertical adjoin) the line A->B crosses: its point
	 *  in Out; false when it crosses none. */
	bool WaterSurfaceHit(const FVector& A, const FVector& B, FVector& Out) const;
	/** The original's splash on a water surface (FX system [EXE 0x14006b6e0]): splash4.nwx with smsplash.wav (a shot, a
	 *  thing falling in, a hand slapping it), or splash3.nwx with splash.wav (bBig: a blast under it); user 2026-09-28. */
	void SpawnSplash(const FVector& At, bool bBig, bool bSound = true, float Scale = 1.f, float Volume = 1.f);
	/** The colour the water at P gives what lies in it (its chain's hue: surfaces and the view's filter the same). */
	bool WaterHueAt(const FVector& P, FLinearColor& OutHue) const;
	// every underwater sector (WATER flag) -> its chain's colour: the chain is the underwater sectors joined
	// through their walls; its colour the average of the water surfaces over it (the ceilings of its
	// sectors under a vertical adjoin), textures averaged together when a chain has more than one
	TMap<int32, FLinearColor> WaterTints;
	/** A breath of bubbles at the player's face (bubble1/2.nwx, bubbles1..5.wav): they rise to the surface and
	 *  are gone there; one stuck under a ceiling that is not a water surface is gone after 10 s. */
	void BlowBubbles(const FVector& At);
	struct FBubble { TWeakObjectPtr<AActor> Actor; float Stuck = 0.f; float Age = 0.f; float Phase = 0.f; };
	/** Air bubbles as small spheres, a white rim and a highlight (user 2026-09-28: some sprite bubbles were red). */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> BubbleMaterial;
	TArray<FBubble> Bubbles;
	void UpdateBubbles(float Dt);
	void BuildWaterTints(const FOutlawsLevel& L, const class FOutlawsAssets& A, const TArray<FColor>& Palette);

	/** Cheats (the menu's Cheats page): no damage to the player. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|Cheats")
	bool bInvincible = false;

	/** Every weapon and full ammunition (Outlaws' own all-weapons cheat). */
	void GiveAllWeapons();
	/** OLJACKPOT (user 2026-10-03): every item a lock in this level asks for (the INF's OBJECT ids) not yet carried; how
	 *  many were given. */
	int32 CheatJackpot();
	/** OLER: health back to full. */
	void CheatFullHealth() { Health = 10.f; }

	/** A message for the player (pickups, locks, INF texts): shown in front of their eyes. */
	void Notify(const FString& Line, float Seconds = 3.f);
	/** CPU1 step 0a's counts (OutlawsPerf's perf_counts.csv): the sectors V1 shows now, the mover groups. */
	int32 PerfVisibleSectors() const;
	int32 PerfMoverGroups() const;
	/** CPU1: the world mesh's solid sections drawn as a mesh per texture (ol.WorldParts), each culled by its own bounds. */
	void SplitWorldDraws();

	/** Play an Outlaws WAV (from the LABs) at a place in the world. */
	class UAudioComponent* PlaySoundAt(const FString& Wav, const FVector& Where) { return PlayWav(Wav, Where); }
	/** (FIRSTHITCH) a sound read and decoded into the level's cache now (behind the load), not on its first play. */
	void WarmWav(const FString& Name);
	const FString& GetBlastSound() const { return BlastSound; }
	/** REQ-RELOADSLICES: WAVs (or slices, "name@from-to" in seconds, "name@from-" to the end) played back to back from one place
	 *  under a key; a new run on the key, or StopSoundRun, cuts off what is left of the old one (the parts not started and the one
	 *  sounding). Each part is logged ("OLSND"). */
	void PlaySoundRun(int32 Key, const TArray<FString>& Wavs, const FVector& Where);
	void StopSoundRun(int32 Key);
	void TickSoundRuns(float Dt);
	bool SoundRunBusy(int32 Key) const { const FSoundRun* R = SoundRuns.Find(Key); return R && (R->Next < R->Wavs.Num() || R->Wait > 0.f); }
	int32 SoundRunCuts = 0;                   // runs cut off before they finished (a test's count)
	double SoundRunClock = 0.0;               // seconds of TickSoundRuns (the "OLSND" log's times)
	/** (a test's) each run part as it starts: its name, SoundRunClock then and its length */
	struct FRunHeard { FString Wav; double At = 0.0; float Len = 0.f; };
	static TArray<FRunHeard>* RunSpy;
	/** The break shotguns' reload in slices of its RELOAD wave: the open, one per shell going in, the shut. BUG-RELOADPACE: the
	 *  shell slice is its clicks alone and the slices run back to back, one shell about the whole wave's 0.97 s, two about 1.1. */
	static constexpr float ReloadOpenLen = 0.25f;                 // 0 .. 0.25 s
	static constexpr float ReloadShellFrom = 0.30f;
	static constexpr float ReloadShellTo = 0.55f;
	static constexpr float ReloadShellLen = ReloadShellTo - ReloadShellFrom;
	static constexpr float ReloadShutFrom = 0.65f;                // .. the end
	static TArray<FString> ReloadOpenRun(const FString& Wav, int32 Shells);
	static FString ReloadShutSlice(const FString& Wav);
	/** (the tests) the reload's sounds among Wavs as letters: O open, S shell, C shut, X the empty-chamber click, R the whole reload. */
	static FString ReloadCodes(const TArray<FString>& Wavs);
	/** R13: gunshots, taunts and blasts in the open: one soft echo off a face behind them (OutlawsRoomSound.cpp). */
	void EchoOf(const FString& Wav, const FVector& At);
	/** A little off true pitch each time (0.875 .. 1.125), so a repeated cry or crackle is never twice the same (user 2026-09-27). */
	static void VaryPitch(class UAudioComponent* A);
	/** A sound that keeps playing where it is put until stopped (the dynamite fuse). */
	class UAudioComponent* PlayLoopAt(const FString& Wav, const FVector& Where) { return PlayWav(Wav, Where, true); }
	/** A menu sound, heard in the head (not in the world). */
	void PlayMenuSound(const FString& Wav) { PlayWav(Wav, FVector::ZeroVector, false, true); }
	/** A loop heard in the head (the player's own breathing). */
	class UAudioComponent* PlayLoop2D(const FString& Wav) { return PlayWav(Wav, FVector::ZeroVector, true, true); }
	/** A one-shot heard in the head, its component returned (for its loudness). */
	class UAudioComponent* PlayOnce2D(const FString& Wav) { return PlayWav(Wav, FVector::ZeroVector, false, true); }

	// ---- enemies (OutlawsLevelActorEnemies.cpp) ----
	bool AreEnemiesActive() const { return bEnemiesActive && !bFrontEnd; }      // (none behind the main menu)
	bool IsRebuildPending() const { return bRebuildPending; }
	double FrontRetryAt = 0.0;                  // the main menu's watchdog: once a second at most
	/** The player's gunfire: every enemy within RadiusCm hears it (a watcher may alert). */
	void AlertEnemies(const FVector& Where, float RadiusCm);
	/** A bad guy spotted the player: the others within RadiusCm share the target (message 125). */
	void ShareTargetNear(AOutlawsEnemy* Source, const FVector& Where, float RadiusCm);
	/** At most one enemy shot per 50 ms level-wide [EXE 0x140088e64]. */
	bool TryEnemyShot();
	/** One taunt at a time level-wide: after one, the next waits 1.9..10 s; unforced taunts also
	 *  need a coin flip [EXE 0x14008eeb0].  (-OLOldTauntRate only since BUG-TAUNTRATE.) */
	bool TryTaunt(bool bForce);
	/** BUG-TAUNTRATE (OutlawsLevelActorTaunts.cpp): the bad guys' taunts by one clock for the player listening on this
	 *  machine, not one per bad guy.  While any bad guy hunts (chase or hit), a taunt every ol.TauntGapMin..ol.TauntGapMax s
	 *  (10..15), said by a hunting bad guy within earshot (not the last one again when there are others), at his eyes. */
	void TickTaunts(float Dt);
	/** An event's taunt (an alert, a friend taken, a lit stick): said if the clock's last one is ol.TauntGapMin s old and
	 *  he is in earshot; the clock starts again from it.  True when said. */
	bool EventTaunt(class AOutlawsEnemy* E, const TArray<FString>& List, const TCHAR* Why);
	/** -OLOldTauntRate: the timers before BUG-TAUNTRATE (each bad guy's own, twice the original's, a coin flip). */
	static bool OldTauntRate();
	FString TauntLastLine;                      // the last line any bad guy said (not said again straight after)
	int32 TauntsSaid = 0;                       // (tests) taunts this clock said, by its timer and by events
	int32 TauntsByTimer = 0;
	int32 TauntsRepeatSpeaker = 0;              // ... by the one who said the last, with someone else in earshot
	int32 TauntsRepeatLine = 0, TauntsRepeatVoice = 0;     // ... the last line again; the last kind of bad guy again
	int32 TauntsHunters = 0, TauntsInEar = 0;   // at the last look
	double TauntPosErrMax = 0.0;                // the farthest a taunt's sound stood from its speaker's eyes (cm)
	TArray<double> TauntTimes;                  // (tests: -OLTauntRateTest) when each was said
	TArray<TWeakObjectPtr<class AOutlawsEnemy>> TauntSpeakers;
	/** The taunt playing now (user 2026-10-03, BUG-TAUNTS: "Sanchez taunting me twice at once"): no other starts over it. */
	TWeakObjectPtr<class UAudioComponent> TauntPlaying;
	bool IsTauntPlaying() const;
	/** A taunt line with the level's TAUNT_TYPE suffix (c105 -> c105t in a TOWN level) when that
	 *  recording exists. */
	FString TauntWav(const FString& Name) const;
	/** (M4) F1..F10: the player's taunt slots 1..10 (OutlawsMpPolish::Taunt). */
	void OnTauntPressed(FKey Key);
	/** (BUG-SCOREBOARD) Tab: a match's frag table up, or away. */
	void OnScoreboardPressed();
	/** Drop a ground item (DROP_ITEM gdrop1, gcanteen...) at a place as a pickup; null if none. */
	AActor* SpawnGroundItem(const FString& Name, const FVector& FeetPos);
	/** A pickup just put down by the player: not walked over and taken again until the player has stepped away. */
	void HoldPickupTillClear(AActor* A) { for (FPickup& P : Pickups) if (P.Actor.Get() == A) P.bWaitClear = true; }
	/** (user 2026-10-02) The wheel's drop: one of a carried thing (a weapon with what it carries, the plate with its armour
	 *  points, one medical bag's worth, one key, puzzle piece or other item) out of the pockets onto the floor at Want (kept
	 *  this side of any wall between Eye and it), a normal pickup (shared in co-op as any dropped item); the dropper cannot
	 *  take it again until they have been 1.5 m from it (or 30 s).  Null with OutWhy set when it cannot be dropped, null
	 *  with OutWhy empty when none is carried.  OutWhat: what it gives ("ipistol=1"). (OutlawsWheelDrop.cpp) */
	AActor* DropCarried(const FString& Item, const FVector& Eye, const FVector& Want, FString& OutWhat, FString& OutWhy);
	/** The things the wheel never drops (the lantern, the map, the oil, the grip editor). */
	static bool NeverDropped(const FString& Item);
	/** Tests: a pickup's ground item and what it gives ("gpistol: ipistol=1"), empty when A is not a pickup; whether it is
	 *  still a wheel drop not armed for its dropper. */
	FString DescribePickup(const AActor* A, bool* bOutLocked = nullptr) const;
	/** Tests: every pickup made since the level was built (dropped things), as DescribePickup says them. */
	void DroppedPickups(TArray<AActor*>& Out) const;
	int32 WheelDrops = 0;
	void OnEnemyKilled(AOutlawsEnemy* Enemy);
	const TArray<TWeakObjectPtr<AOutlawsEnemy>>& GetEnemies() const { return Enemies; }
	/** (M1) A sprite set with all its chors (a multiplayer character's, by its NWX). */
	TSharedPtr<OutlawsTextures::FSpriteAtlas> LoadSpriteSet(const FString& Nwx) { return GetAtlas(Nwx, true); }
	UFUNCTION(BlueprintCallable, Category = "Outlaws|Enemies")
	int32 GetKills() const { return Kills; }
	// far and unseen (user 2026-10-03, AOutlawsEnemy::UpdateFarFreeze): the level's part of it
	/** The sector of the static plan under P, for the visibility test (-1: in none). Hint: the caller's last sector,
	 *  looked at first (then its adjoins, then every sector); the answer is the full search's either way. */
	int32 CullSectorOf(const FVector& P, FOutlawsSectorHint* Hint = nullptr) const;
	/** The main menu's place (BUG-SECTOR): the feet, arm's reach round them and the menu ahead (Yaw) all inside the level's
	 *  sectors, on their floors with a head's room under their ceilings. */
	bool SpotInSectors(const FVector& Feet, float Yaw) const;
	/** Whether a sector may be seen from the eye this tick (VisibleSectors: the portal flood plus one adjoin); true for no
	 *  sector (-1) and while culling is off or not yet valid, so nothing freezes by mistake. */
	bool IsSectorSeen(int32 Sector) const;
	/** A thing a moving sector carries along right now (riding a lift, a scrolling floor). */
	bool IsCarriedBySector(const AActor* A) const;
	/** CPU2: a bad guy the enemy throttle leaves alone (always FULL, outside the caps): the named outlaws, the dying,
	 *  the held, thrown or taken alive, and one a moving sector carries. */
	bool IsThrottleExempt(const AOutlawsEnemy* E) const;
	/** (tests) the velocity a moving sector carries this thing at now (zero: none). */
	FVector CarryVelocityOf(const AActor* A) const { const FCarry* C = Carries.Find(TWeakObjectPtr<AActor>(const_cast<AActor*>(A))); return C ? C->Vel : FVector::ZeroVector; }
	/** (tests) a moving floor's velocity at a point on it, cm/s (zero: none). */
	FVector FloorVelocityAt(const FVector& P) const;
	/** (tests) the middle of the open moving floor (VELFLOORONLY, moving) nearest a point; Kind 1: one that does not kill (the
	 *  train's gaps between its cars), 2: one that does (its ground), 0: either; false: none. */
	bool DebugMovingGround(const FVector& Near, FVector& Out, int32 Kind = 0) const;
	/** (tests) the train's floors' carry of the player now, cm/s. */
	FVector DebugPlayerCarry() const { return PlayerCarryVel; }
	/** (tests: -OLTrainBodyTest) the deadly floors leave the bad guys be, so a living one can be measured riding one. */
	bool bFloorKillsOff = false;
	/** A bad guy froze (+1) or thawed (-1): the count kept, a log line when it has moved much. */
	void NoteFarFreeze(int32 Delta);
	// ---- CPU2 enemy throttle (OutlawsEnemyThrottle.cpp, 2026-10-05) ----
	/** Each frame (the host's or single player's; a co-op client's copies only follow the host): the awake bad guys ranked
	 *  (fighting or just hit, then in a player's view or just woken, then the nearest to any player) and put in tiers
	 *  under the device's caps (AOutlawsEnemy::SetThrottleTier), tightened while the game thread is over its budget. */
	void TickEnemyThrottle(float Dt);
	/** A bad guy about to open fire at his target: false while that player already has the attack cap's worth on him. */
	bool TakeAttackToken(const AOutlawsEnemy* E);
	float ThrottleThinkInterval() const;
	/** "level L caps think/move/wander/attack | full F move M wander W frozen Z | queue Q" (tests and the log). */
	FString ThrottleSummary() const;
	/** The full tier filled: full / min(think cap, eligible) (the swarm test's measure). */
	float ThrottleFullShare() const;
	int32 ThrottleCounts[4] = { 0, 0, 0, 0 };
	int32 ThrottleEligible = 0;
	int32 ThrottleLevel = 0;
	int32 ThrottleCaps[4] = { 0, 0, 0, 0 };       // think, move, wander, attack in use (0: no cap)
	int32 ThrottleAttackCount[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
	int32 ThrottleWakeQueue = 0;
	int32 ThrottleBinds[4] = { 0, 0, 0, 0 };      // this second's frames each cap bound (attack: refusals)
	float ThrottleBindRate[4] = { 0.f, 0.f, 0.f, 0.f };
	double ThrottleBindSince = 0.0;
	double ThrottleSmoothMs = 0.0;
	double ThrottleSmoothWaitMs = 0.0;              // the XR runtime's frame wait in it (OutlawsPerf::XRWaitMs), smoothed alike
	double ThrottleEffMs = 0.0;                    // what the budget is held against: the game thread less that wait
	float ThrottleOverFor = 0.f, ThrottleUnderFor = 0.f;
	int32 ThrottleResumeSeen = 0;                   // (Q2PROFILE: OutlawsDeviceProfile::ResumeCount when last looked)
	bool bThrottleLogged = false;
	int32 FarFrozen = 0;                      // frozen right now
	int32 FarFrozenLogged = 0;
	int32 FarFrozenPeak = 0;
	double FarFrozenSum = 0.0;                // frozen x seconds, for the level's average
	double FarFrozenTicks = 0.0;              // the enemy ticks those did not run (about: a frozen one ticks twice a second)
	double FarFrozenTime = 0.0;
	int32 FarSoundsSkipped = 0;               // 3D one-shots not started, their source beyond hearing (PlayWav)
	/** The level's line (at its clearing): frozen on average and at peak, ticks saved, sounds skipped; then the counts reset. */
	void LogFarSummary();
	/** A bad guy walking into a door (Jedi smart object): opens it unless it is locked; true if it moves. */
	bool EnemyOpenDoor(const FVector& Eye, const FVector& Ahead, bool bPlayerRules = false);
	/** BUG-BOSSDOOR: a bad guy may work this door sector (the original's rule: an automatic door, or ENTITY_MASK *; no key). */
	bool EnemyMayWorkDoor(int32 Door) const;
	/** Where to walk next from FromFeet toward ToFeet through the level's sectors (user 2026-09-28: a roaming boss
	 *  finds its way round walls to the player): the opening out of the first sector on the way, a little into the
	 *  next; ToFeet itself when both are in one sector.  Openings too low (a shut door), too high a step (MaxStepU,
	 *  Outlaws units) or no-pass / fence are not taken.  False: no way (or off the map).
	 *  BUG-BOSSWALL: OutVia the opening taken (its sector, its wall; -1s for ToFeet itself), and the openings in Avoid (the
	 *  same pairs) not tried: a hunter that could not get through one goes round by another. */
	bool PathStepToward(const FVector& FromFeet, const FVector& ToFeet, double MaxStepU, double HeightU, FVector& OutStep,
		FIntPoint* OutVia = nullptr, const TArray<FIntPoint>* Avoid = nullptr) const;
	/** Tests (BUG-BOSSWALL): the sector at P and its walls within RadiusU (Outlaws units) of P: ends, opening, flags. */
	FString DebugSectorWalls(const FVector& P, double RadiusU) const;
	/** REQ-MPBOTS: the whole way from FromFeet to ToFeet over the same openings, shortest first: the crossing points (each
	 *  MarginU in from its opening's ends) then ToFeet, and the way's length.  False: no way (or off the map).
	 *  BUG-BOTPICKUP: MaxDropU (above 0) the deepest drop the body walks off; a glass pane (SHATTER) shut like no-pass. */
	bool PathRoute(const FVector& FromFeet, const FVector& ToFeet, double MaxStepU, double HeightU, double MarginU, TArray<FVector>& OutPoints, double& OutLenCm, double MaxDropU = 0.0) const;
	/** BUG-BOTPICKUP: where a body stands to reach a thing in a sector it cannot walk into (under town_2p's bar, on a ledge
	 *  a step too high): just over an opening of the thing's sector, on a floor a body stands on, the thing within ReachCm
	 *  across and ReachUpCm up or down from there.  The nearest such place; false when there is none. */
	bool PathReachPoint(const FVector& Thing, double ReachCm, double ReachUpCm, double HeightU, FVector& OutFeet) const;
	/** BUG-BOTPICKUP diagnosis: why PathRoute finds no way (the sectors at either end, how far the flood got, each opening into the goal and what shut it). */
	FString PathRouteWhy(const FVector& FromFeet, const FVector& ToFeet, double MaxStepU, double HeightU, double MaxDropU = 0.0) const;
	/** News for the bad guys within RadiusCm of Where (not Source): their confidence and panic change. */
	void BroadcastMeters(AOutlawsEnemy* Source, const FVector& Where, float RadiusCm, float Confidence, float Panic);
	/** Sector light 0..31 at a place (enemies see less far into the dark). */
	int32 LightAt(const FVector& UE) const;

	/** A bullet hit: shootable INF walls, switches and door / lift sectors get their shoot event. */
	void ShootAt(const FHitResult& Hit);

	/** Where the first remaining pickup of a ground item (e.g. GBRSSKEY) is, or zero. */
	UFUNCTION(BlueprintCallable, Category = "Outlaws|Items")
	FVector PickupLocation(const FString& GroundItem) const;

	/** Inventory item names picked up this level, in pickup order. */
	UFUNCTION(BlueprintCallable, Category = "Outlaws|Items")
	TArray<FString> GetInventory() const { return Inventory; }

	/** What the game calls them (PICKUP_MSG text: "Brass Key"), same order as GetInventory. */
	UFUNCTION(BlueprintCallable, Category = "Outlaws|Items")
	TArray<FString> GetInventoryLabels() const { return InventoryLabels; }

	/** Where the first remaining pickup of an ITEM_CLASS (ammo, heal, armr, weap...) is, or zero. */
	UFUNCTION(BlueprintCallable, Category = "Outlaws|Items")
	FVector PickupLocationByClass(const FString& ItemClass) const;

	// Player stats (research/outlaws_weapons_items.md 1.1): health 0..10 (the HUD shows it x10 as a
	// percent); armour is the ibplate count 0..150 and absorbs count / 10 damage points first.
	UFUNCTION(BlueprintCallable, Category = "Outlaws|Player")
	float GetHealth() const { return Health; }

	UFUNCTION(BlueprintCallable, Category = "Outlaws|Player")
	int32 GetArmour() const { return Armour; }

	UFUNCTION(BlueprintCallable, Category = "Outlaws|Player")
	int32 GetGold() const { return Gold; }

	/** How many of an inventory item (ibullit, icart, ishells, idocbag...) the player carries. */
	UFUNCTION(BlueprintCallable, Category = "Outlaws|Player")
	int32 GetCount(const FString& Item) const;

	/** Add to an item, clamped to its ITM MAX_COUNT; armour (ibplate) is set when better, gold is
	 *  counted.  False when nothing was taken. */
	UFUNCTION(BlueprintCallable, Category = "Outlaws|Player")
	bool AddCount(const FString& Item, int32 Count);

	/** Spend Count of an item; false, with nothing spent, when there are not enough. */
	UFUNCTION(BlueprintCallable, Category = "Outlaws|Player")
	bool TakeCount(const FString& Item, int32 Count);
	bool IsDynamitePickup(const AActor* A) const;                   // a level stick of dynamite, as a pickup
	/** A save's dropped entry that is a stick of dynamite: the level's own (the build makes it, the dynamite list keeps it). */
	static bool IsStickGround(const FString& Ground) { return Ground.StartsWith(TEXT("gdynam"), ESearchCase::IgnoreCase); }
	/** A stick come to rest on the ground (the player's, thrown or dropped unlit): one of the level's sticks from now on,
	 *  set off by a blast near it or a shot (user 2026-10-01: a lit one thrown into a pile of them set none off). */
	void AddLooseDynamite(AActor* A);
	void UseUpItem(int32 UseId);                                     // an INF lock spent one item with that USE_ID

	UFUNCTION(BlueprintCallable, Category = "Outlaws|Player")
	void HealPlayer(float Points);

	/** The medical bag: heals what is missing, one bag point per health point, spending only that. */
	UFUNCTION(BlueprintCallable, Category = "Outlaws|Player")
	bool UseMedicalBag();

	/** What the wheel's item ring shows: usable heals the player carries, then the listed items
	 *  (keys, documents...) with the game's names for them. */
	void GetWheelItems(TArray<FString>& OutItems, TArray<FString>& OutLabels) const;

	/** An item picked on the wheel: a heal is used; a key or other item shows its name (locks take
	 *  keys by themselves).  True when something happened. */
	UFUNCTION(BlueprintCallable, Category = "Outlaws|Player")
	bool UseItem(const FString& Item);

	/** Hurt the player: armour absorbs the raw damage first, then what is left is scaled by where it
	 *  hit (LocationMult: 1.5 chest, 1 body, 0.5 edge) and comes off health; at 0 the player dies. */
	UFUNCTION(BlueprintCallable, Category = "Outlaws|Player")
	void DamagePlayer(float Damage, float LocationMult = 1.f, bool bSkipArmour = false);
	/** Out of air under water (user 2026-10-01): a tenth of health, past the plate, choke.wav (the character's SND_CHOKE)
	 *  and no grunt. */
	void DrownPlayer();
	/** The player's own bullet through their own head (user 2026-10-02): dead at once, past the plate and the badge, with
	 *  the game's line for it (local.msg 1098). */
	void ShotOwnHead();
	/** Where the next DamagePlayer comes from (a shooter, a blast): the hit's red shows from that side. */
	void SetHurtSource(const FVector& From) { HurtFrom = From; bHurtFrom = true; }
	/** (REQ-STATS) The next DamagePlayer is a bad guy's shot (a death by it counts as killed by bad guys). */
	bool bHurtByBadGuy = false;
	/** (REQ-KILLFEED) What the next DamagePlayer is (a fall, this player's own dynamite...; EOutlawsHurtCause), and what the
	 *  last death was: a match's kill line for a death nobody else is credited with. */
	uint8 HurtCause = 0;
	uint8 DeathCause = 0;
	/** local.msg's line Id, or Fallback when the install has none. */
	FString UserMessageOr(int32 Id, const TCHAR* Fallback) const { const FString* T = UserMessages.Find(Id); return T ? *T : FString(Fallback); }
	int32 TestSectorCount() const;
	uint32 TestSectorFlags(int32 Sector) const;
	/** REQ-EXTRAS: the level's secret sectors (SECFLAG1_SECRET_TAG, as UOutlawsStats::NoteSecret counts them). */
	int32 SecretSectorCount() const;
	// omni light in an unlit world (MPC_OLR_Lights, ue_olue_lights_material.py): slot 0 the lantern, 1..3 the
	// latest muzzle flashes, added to the sector light by the world, water, sprite and held materials
	struct FMuzzleFlash { FVector Pos = FVector::ZeroVector; float Age = 99.f; float Strength = 0.f; float Radius = 1300.f; };
	FMuzzleFlash Flashes[3];
	int32 NextFlash = 0;
	UPROPERTY(Transient)
	TObjectPtr<class UMaterialParameterCollection> LightsMpc;
	/** A gunshot lights the room around it for a split second. */
	void MuzzleFlash(const FVector& At, float Strength = 1.f, float Radius = 1300.f);
	/** (REQ-TNTFLASH) A blast lights the area like a muzzle flash, 5 times the reach and the brightness; takes one of the
	 *  flash slots (3 of the 8 light slots) for its quarter second, so the lamp slots are not touched. */
	static constexpr float BlastFlashScale = 5.f;

	// ---- REQ-MUZZLEFLASH3D (OutlawsMuzzleFlash3D.cpp): 3D muzzle flashes cut at load from the enemies' own shooting
	// sprites in the player's files (the remaster's SPX): a shooter's front view gives the starburst, a side view the flame
	// jet.  Each gun kind (0 pistol: Sanchez, 1 rifle: a rifleman, 2 shotgun: a shotgunner) gets one picture with both, and
	// a flash is one draw: the jet on 3 planes crossed round the barrel, the starburst on 1 facing forward; additive, unlit,
	// a random roll and scale per shot, 1 to 2 frames long.  No sprite or no flash found: the kind is off and the caller
	// keeps today's flash.  -OLNoFlash3D turns every kind off (the fallback's test).
	static constexpr int32 NumFlashKinds = 3;
	/** The gun kind of a shot's sound: 0 pistol, 1 rifle (and the gatling), 2 shotgun (single, double, sawed-off). */
	static int32 FlashKindOfSound(const FString& Wav);
	static const TCHAR* FlashKindName(int32 Kind);
	/** The flash's picture for a kind is built (false: the caller's own flash). */
	bool HasFlash3D(int32 Kind) const { return Kind >= 0 && Kind < NumFlashKinds && Flash3D[Kind].bOk; }
	/** A 3D flash at Muzzle along Bore; Size scales the gun kind's own length.  False (nothing shown) when the kind is off. */
	bool MuzzleFlash3D(const FVector& Muzzle, const FVector& Bore, int32 Kind, float Size = 1.f);
	/** Tests: the flashes stay on until FlashHoldRelease (a picture can be taken of one). */
	bool bFlash3DHold = false;
	void FlashHoldRelease();
	/** Tests: flashes shown so far, by kind. */
	int32 Flash3DShown[NumFlashKinds] = { 0, 0, 0 };
	/** The build's line about the flashes (the log and the tests). */
	FString Flash3DReport;
	/** The flash slots drawn now (tests). */
	int32 Flash3DVisible() const;
	/** At the level's build: the pictures cut (once a session), the slots made.  -OLFlash3DSave: Saved/OLShots/flash3d_<kind>.png. */
	void BuildFlash3D(const FOutlawsAssets& A);
	void TickFlash3D(float Dt);
	struct FFlash3DKind
	{
		bool bOk = false;
		int32 W = 0, H = 0;                       // the picture: the jet on the left (muzzle at its left edge), the starburst right
		TArray<FColor> Px;                        // premultiplied by the cut's mask (additive)
		float JetU1 = 0.5f;                       // the jet's right edge in U (it starts at 0); the starburst from there to 1
		float JetV1 = 1.f, StarV1 = 1.f;          // each one's bottom in V (both start at 0)
		float JetAspect = 0.5f;                   // height over length
		float JetAxisV = 0.5f;                    // the barrel's line across the jet (0 top .. 1 bottom of its rect)
		FVector2f StarCentre = FVector2f(0.5f);   // the starburst's hot middle (0..1 of its rect)
		float StarAspect = 1.f;                   // height over width
		FString Source;                           // "sanchez.nwx f23 + f26"
	};
	FFlash3DKind Flash3D[NumFlashKinds];
	struct FFlash3DSlot { TWeakObjectPtr<class UProceduralMeshComponent> Mesh; int32 Kind = -1; float Age = 99.f; int32 Frames = 0; };
	FFlash3DSlot Flash3DSlots[9];                // 3 a kind
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UMaterialInstanceDynamic>> Flash3DMats;
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UTexture2D>> Flash3DTex;

	/** A test's ear on PlayWav (-OLVb2Test): every wave name asked for is added while set. */
	static TArray<FString>* WavSpy;
	struct FSoundRun
	{
		TArray<FString> Wavs;
		int32 Next = 0;                       // the next part to start
		float Wait = 0.f;                     // seconds until it starts
		FVector Where = FVector::ZeroVector;
		TWeakObjectPtr<class UAudioComponent> Playing;
	};
	TMap<int32, FSoundRun> SoundRuns;
	// the lamps' own light (backlog 8, user 2026-10-01: "fade by distance as smooth as possible ... closest lamps get
	// priority"): slots 4..7, the nearest lit lamps; a lamp keeps its slot until another is a fifth nearer, each slot
	// fades in and out, so no two lamps fight over one; ol.LampLights 2 is the six-slot fallback for the headset
	struct FLampLight { TWeakObjectPtr<class UProceduralMeshComponent> Mesh; float Fade = 0.f; float Phase = 0.f; };
	FLampLight LampLights[4];
	int8 bLampSlots = -1;                     // (the collection has L4..: -1 not yet looked)
	// BUG-LANTERNNET (user 2026-10-10, RC2): the other players' lit lanterns, drawn by this machine as its own lantern is
	// drawn, in the lamp slots from the top down (L7, L6): the nearest first, at most RemoteLanternCap() (2; 1 under the
	// Quest 2 profile), each taking the place of the farthest lamp light so the slots lit stay inside the Lamp Lights row
	// (with that row at 0 a partner's lantern still shines, as one's own does)
	struct FRemoteLanternShown { int32 Slot = -1; int32 Who = -1; FVector At = FVector::ZeroVector; float Oil = 0.f; float Flicker = 1.f; FLinearColor Pos, Col; };
	TArray<FRemoteLanternShown> RemoteLanternsShown;     // (this frame's; tests and the light count)
	int32 RemoteLanternsLit = 0;                         // lit and in reach of a slot this frame (shown, or waiting on a lamp's fade)
	int32 LampSlotsLit = 0;                              // the lamp lights shining this frame (the remote lanterns not counted)
	static int32 RemoteLanternCap();
	/** The lantern's light for its oil (0..1) at flicker time T: the place (xyz) and reach (w, cm), and the colour; the one
	 *  formula for this player's own lantern and the other players' (OutFlicker: the flicker it used). */
	static void LanternLight(const FVector& At, float Oil, float T, FLinearColor& OutPos, FLinearColor& OutCol, float* OutFlicker = nullptr);
	/** REQ-LANTERNBOOST: the full-oil lantern's reach (cm) and brightness before the flicker (were 1100 and 0.6). */
	static constexpr float LanternReach = 2200.f;
	static constexpr float LanternBright = 0.75f;
	/** The lantern's light shines here now: where (the held flame, else the chest) and its oil (0..1); false: out, dead, a movie. */
	bool LanternShining(FVector& OutAt, float& OutOil) const;
	void TickLampLights(float Dt, bool bMovieOn, class UMaterialParameterCollectionInstance* Mpc);
	/** The lit lamp fire nearest a point (false: none lit in the level); -OLLampTest stands by it. */
	bool NearestLampFire(const FVector& From, FVector& Out) const;
	/** An enemy's SHOOT_SOUND is a gun (not the tarantulas' null.wav, a throw, a swing, the poodle's fire). */
	static bool IsGunSound(const FString& Wav);
	// the lantern (ilantern, picked from the wheel): burns ioil, dimmer the less is left, flickers like a flame
	bool bLampOn = false;
	/** Where the lantern's light is: the held lantern's flame, set each frame it is held (VR); else the chest. */
	FVector LampAt = FVector::ZeroVector;
	double LampAtTime = -1.0;
	/** The plate picked up last: its size (50 light, 100 medium, 150 heavy) and a count that goes up with each new one. */
	int32 ArmourMax = 0;
	int32 PlateSerial = 0;
	/** Every hit the plate took a share of (DamagePlayer).  BUG-PLATECARRY: only these dent the worn plate or break it; the
	 *  count set any other way (a new game, a restart, a load, a take the co-op host refused) is no blow to it. */
	int32 PlateHits = 0;
	/** Armour takes its share of every hit (the original's rule; in VR too since the plate is worn, REQ-CHESTPLATE). */
	bool bArmourPassive = true;
	int32 BlastsSeen = 0;                                          // (the tests': blasts gone off here)
	float BlastRadiusCm() const { return BlastRadius * 30.f; }      // (pdynam's 50 units)
	FVector SettleLookAt = FVector::ZeroVector;     // (a test aid: the 3D item SettleItemModels moved most, -OLSettleLook)
	void SetArmour(int32 Points) { Armour = FMath::Max(0, Points); }
	/** The level as it stands (the INF's live copy), for the held map. */
	const class FOutlawsLevel* LiveLevel() const;
	float OilBurn = 0.f;
	float LampTime = 0.f;
	bool ToggleLamp();
	void TickLights(float Dt);
	int32 DarkestSector() const;
	/** Content/<Folder>/<File>, or <install root>/<Folder>/<File> when that exists (a headset carries them with the LABs). */
	FString DataFile(const TCHAR* Folder, const FString& File) const;
	// the Sheriff's Badge (gbadge -> ibadge, "Sheriff's Badge!"): for 30 s the player takes half damage and deals
	// double (the push of each hit doubles with it). Another badge sets it back to 30 s (user 2026-09-27)
	float BadgeLeft = 0.f;
	static constexpr float BadgeSeconds = 30.f;
	bool HasBadgePower() const { return BadgeLeft > 0.f; }
	/** What the player's hits are multiplied by (2 with the badge). */
	float PlayerDamageScale() const { return HasBadgePower() ? 2.f : 1.f; }
	/** A sprite's first cell as a picture: its atlas (the HD one when Remastered is on), the cell's UV rect and its
	 *  width / height (the original HUD's heart and plate cards, intheart.nwx / ibplate.nwx). */
	bool SpriteIcon(const FString& Nwx, UTexture*& OutTex, FVector2D& OutScale, FVector2D& OutOffset, float& OutAspect);
	/** One cell of a remaster sprite (the SPX beside an NWX: ijrifle.spx) as a texture at its own size, or null
	 *  (no remaster archives). Kept for the session. */
	UTexture2D* SpxCell(const FString& Spx, int32 Cell, FIntPoint& OutSize);
	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UTexture2D>> SpxCells;
	/** The cells' SPRX frame sizes: a headset decodes big BC7 cells from a smaller mip, so the texture can be smaller. */
	TMap<FString, FIntPoint> SpxCellSizes;
	/** Tests: a shot lamp's falling fire at a point. */
	void DebugSpawnSparks(const FVector& Where, int32 Count) { SpawnSparks(Where, Count); }
	/** Tests: a TNT blast's soot at a point. */
	void DebugScorch(const FVector& Where) { AddScorchMarks(Where); }
	/** Tests (-OLDecalEdgeTest): the soot meshes made so far, and putting them all away. */
	const TArray<TWeakObjectPtr<UProceduralMeshComponent>>& DebugScorches() const { return Scorches; }
	void DebugClearScorches();
	/** Tests: a listed item (a key, a paper) as if picked up, with its wrist label. */
	void DebugGiveListed(const FString& Item, const FString& Label) { AddCount(Item, 1); Inventory.Add(Item.ToLower()); InventoryLabels.Add(Label); }
	/** A hand at a NUDGE object (the mill's valve wheels): it is worked, its animation's event reaching its sector's
	 *  INF (EVENT_MASK 16384). True when one was in reach. */
	bool TurnPropByHand(const FVector& Hand);
	/** A hand pushed into a wall: a wall with a switch trigger is pressed (no grip needed); onto the piano's keys, they play. */
	bool PushByHand(const FVector& Point);
	// ---- REQ-PIANO (townex, town): the piano's tune (OutlawsLevelActorInf.cpp SetupTunes) ----
	/** An elevator's moving sound that is a tune (5 s or more): played once to its end, not looped and cut at the arrival.
	 *  True when Elevator's is one (the host's own and, in co-op, the client's from the host's word). */
	bool TuneSound(int32 Elevator, const FString& Wav, const FVector& At, bool bStart);
	double WavSeconds(const FString& Name) const;     // a level sound's length (the original files, alike on every machine)
	void SetupTunes();
	TMap<int32, double> TuneSeconds;                  // elevator -> its tune's length
	TMap<int32, double> TuneSpeeds;                   // elevator -> its move's speed for the tune's length (kept over a load)
	TSet<int32> KeySectors;                           // the piano keys' trigger sectors
	double TuneBusyUntil = -1.0;                      // world time the keys take a press again (the tune's end + 2 s)
	int32 TuneStarts = 0;                             // (tests) tunes started on this machine
	int32 TuneRefusals = 0;                           // (tests) presses the lockout refused here
	/** Tests (OutlawsPianoTest.cpp): a hand pressed down onto the middle of the keys, as the push finds them; and the roll's offset. */
	bool DebugPressPianoKeys();
	/** (BUG-BOXPUSHONCE, OutlawsBoxPushTest.cpp) a hand push on a mover's face, as PushDoors finds it: found, pushable, taken. */
	FString DebugHandPushMover(int32 Elevator, bool& bOutFound, bool& bOutClosed, bool& bOutTaken);
	FVector2D DebugPianoRoll() const;
	FVector DebugPianoStand(double Along) const;      // a spot on the floor before the keys, Along units off the snapshot's
	/** Tests: every elevator's saved state on one line each (Tag in front); and the townex old-save check's state setter:
	 *  doors pushed and the rest put at their next stop, held there, so a save holds them off their start. */
	void DebugLogElevatorStates(const TCHAR* Tag) const;
	void DebugScrambleElevators();
	const class FOutlawsInfRuntime* DebugInf() const { return Inf.Get(); }
	/** (REQ-SECTORLIGHT, tests) A CHANGE_LIGHT elevator's drawn faces, the sprites and models standing in its sectors, the
	 *  level last drawn (-1 none), its first face's vertex brightness; the time the light updates took and how many. */
	struct FLightStats
	{
		int32 Faces = 0, Sprites = 0, Models = 0, Shown = -1, SpriteLight = -1, SpriteWant = -1;
		float FaceBright = -1.f, FaceWant = -1.f, ModelShade = -1.f, ModelWant = -1.f;
		bool bParsedSame = false;             // the parsed level's AMBIENT (LightAt's) as the INF's, in every sector of it
	};
	FLightStats DebugLight(int32 Elevator) const;
	double DebugLightSeconds = 0.0, DebugLightModelSeconds = 0.0;
	int32 DebugLightUpdates = 0, DebugLightTicks = 0, DebugLightFaceUpdates = 0, DebugLightModelUpdates = 0;
	bool HandEvent(int32 Sector, int32 Wall, uint32 Event);     // a hand's press on a wall (Wall -1: the sector), once per 0.7 s
	FVector SteerCurrent(class ACharacter* Body, const FVector& Push);   // a water current round a closed gate; a swimmer beats it
	FVector FlowSteer = FVector::ZeroVector;
	bool bCurrentHeldLogged = false;                  // (BUG-SURFACEJOLT) a held swimmer's downward current logged once per stay
	// (BUG-WATERHANDS) which water sectors have air on their ceiling (WaterSurfaceSectors), for the level read
	mutable TBitArray<> SurfaceSectorBits;
	mutable const class FOutlawsLevel* SurfaceSectorLevel = nullptr;
	mutable FString SurfaceSectorLevelName;
	const TBitArray<>& WaterSurfaceSectors(const class FOutlawsLevel& Lv) const;
	FVector PlayerCarryVel = FVector::ZeroVector;     // the train's floors' carry of the player now, cm/s (eased; TrackPlayer)
	TMap<uint64, double> HandUseAt;
	/** Tests: every NUDGE prop worked at once. */
	int32 DebugNudgeAllProps();
	/** Tests: the story's next level as a won level starts it (the carry), no movie. */
	void DebugNextLevel() { NextLevelName = StoryNext(NextMovie); NextMovie.Reset(); StartNextLevel(); }
	/** Tests: what is carried (counts, the wrist list, gold, armour) to the log. */
	void DebugDumpCarry(const TCHAR* When) const;
	/** A loose animated sprite of the level's (the badge spinning by the wrist), gone with the level. */
	class AOutlawsSpriteActor* MakeLooseSprite(const FString& Anim, const FVector& At);

	// ---- saves and autosave (OutlawsSaveGame.cpp): Saved/OutlawsSaves/slot<n>.json and autosave.json ----
	enum { NumSaveSlots = 10 };
	struct FSaveInfo { int32 Slot = -1; bool bEmpty = true; FString Level; FString Name; FString GameTime; FString Stamp; };
	/** Slot 0 .. NumSaveSlots - 1, -1 for the autosave, or -2 for a co-op host's own autosave (coop_autosave.json, R7). */
	static FString SavePath(int32 Slot);
	/** Between two levels (the won level's fade, its cutscene, the story's first movie): the autosave written as the NEXT
	 *  level's opening, with what is carried into it (user 2026-10-02: End Game in a cutscene resumed in the level just
	 *  won).  Loading it starts that level from its beginning. */
	bool HasGradientSky() const { return bGradientSky; }      // (BUG-TRAINSKY, -OLVb2Test=sky)
	/** (tests, BUG-TRAINSKY8BIT) what the gradient dome shows now: "gradient", or "flat <r> <g> <b> (<the picture read>)"; "none". */
	FString DebugSkyLook() const;
	bool IsBetweenLevels() const { return !bFrontEnd && !bDead && !NextLevelName.IsEmpty() && (LevelEndTimer >= 0.f || (Movie != nullptr && !Inf)); }
	bool SaveLevelOpening();
	bool bPendingOpeningSave = false;
	/** A save's chosen difficulty rewritten in place (its spawn difficulty kept, or set to what it was). */
	static bool PatchSaveDifficulty(int32 Slot, EOutlawsDifficulty To, EOutlawsDifficulty Was);
	static FSaveInfo ReadSaveInfo(int32 Slot);
	/** Write the game as it stands (the level, the player, the bad guys, what is left to pick up, the doors). */
	bool SaveGame(int32 Slot);
	/** The same as JSON text, written nowhere (a co-op host's join snapshot for a player joining late, R7). */
	bool BuildSaveText(class AOutlawsPlayer* P, FString& OutText, int32& OutEnemies, int32& OutPickups);
	/** Rebuild the saved level and put it back as it was saved. */
	bool LoadGame(int32 Slot);
	static bool HasAutosave() { return !ReadSaveInfo(-1).bEmpty; }
	/** BUG-DEATHAUTOSAVE (user 2026-10-20): the save a death goes back to: the slot last loaded or saved when no autosave has
	 *  been written since (OutSlot >= 0), else the autosave (-1); either only when it is of this level (false: none). */
	bool DeathSave(int32& OutSlot) const;
	/** (the slot loaded or saved since the last autosave: -1 none; the title menu forgets it) */
	static void NoteDeathSlot(int32 Slot);
	/** The remaster's names for the story levels (levelselect.rml) and its Load_LevelNN pictures (UI.kpf). */
	static FString LevelTitle(const FString& Level);
	static int32 LevelPicture(const FString& Level);
	/** The options' autosave: at the start of every level and every AutosaveMinutes after (default 2). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|Saves")
	bool bAutosave = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|Saves")
	float AutosaveMinutes = 2.f;
	double PlayTime = 0.0;                  // seconds played in this game (a save's game time)
	float AutosaveWait = 0.f;
	/** World seconds of the last shot at the player or hurt taken (the autosave waits out danger, user 2026-10-01). */
	double LastDangerTime = -100.0;
	/** Why an autosave would wait now (empty: safe): shot at or hurt lately, an alerted bad guy in sight, low health. */
	FString AutosaveDanger() const;
	bool bAutosaveHeld = false;             // (the last wait was logged)
	float StartSaveDelay = -1.f;            // >= 0: the level-start autosave comes in this many seconds
	TSharedPtr<class FJsonObject> PendingLoad;
	bool bRestoring = false;                // putting a save back: no sounds
	/** (Q2 step 4) set round ApplyLoad by a late join: the bad guys' saved states listed here to be put back a few a frame */
	TArray<TPair<TWeakObjectPtr<class AOutlawsEnemy>, TSharedPtr<class FJsonObject>>>* DeferEnemyReads = nullptr;
	/** ... and the things dropped in play made again a few a frame (bDeferDrops set round ApplyLoad; DeferredDrops in save order) */
	struct FDeferredDrop { FString Ground; FVector At = FVector::ZeroVector; FString Contents; };
	bool bDeferDrops = false;
	TArray<FDeferredDrop> DeferredDrops;
	void TickSaves(float Dt);
	void ApplyLoad(const class FJsonObject& O, bool bWorldOnly = false);     // (bWorldOnly: co-op's client takes the host's world only)
	FVector HurtFrom = FVector::ZeroVector;
	bool bHurtFrom = false;

	UFUNCTION(BlueprintCallable, Category = "Outlaws|Player")
	bool IsPlayerDead() const { return bDead; }

	/** Rebuild the level from scratch with the start kit (what dying does after the fade). */
	UFUNCTION(BlueprintCallable, Category = "Outlaws|Player")
	void RestartLevel();
	/** RestartLevel, then Then once the new level is built (the build waits for the old level to be let go of: see
	 *  FinishRestart). */
	void RestartLevelThen(TFunction<void()> Then);
	/** The main menu over a level (user 2026-10-02: "a random bad guy object spawn in a random map, every time, to keep the
	 *  main menu fresh"; no bad guys in sight, no guns in the hands, stood on the floor): at the app's start and after End
	 *  Game a level picked at random is built behind the menu, the view from where one of its bad guys was placed, the
	 *  world held still and silent in the pause menu's tone.  Nothing of it is ever saved; Resume Game loads the autosave. */
	bool bBackdrop = false;                    // a built level lies behind the main menu
	bool PickBackdropLevel(FString& Out) const;
	void SetUpBackdrop();
	/** A restart waiting for the old level's collection (user 2026-10-01: two levels at once closed the Quest). */
	bool bRebuildPending = false;
	uint32 RebuildGcMark = 0;
	int32 RebuildFrames = 0;
	int32 RebuildStage = 0;                    // 0 first collection, 1 frames, 2 second collection, 3 frames, then the build
	TFunction<void()> AfterRestart;
	void FinishRestart();
	/** (BUG-RESPAWNCRASH) what the rebuild in hand is, for a map load in its place: the same level from its start (Restart
	 *  Level, a death, a skip), the next level of the story with the kit carried, a load; anything else (a co-op start, the
	 *  main menu's level) only ever rebuilds in place.  A historical mission's session goes along with it (WriteHomCarry). */
	enum class ERebuildKind : uint8 { Other, Restart, NextLevel, Load };
	ERebuildKind RebuildKind = ERebuildKind::Other;
	ERebuildKind NextRebuildKind = ERebuildKind::Other;     // (set by StartNextLevel and LoadGame just before RestartLevelThen)
	TMap<FString, int32> MapCarryCounts;                   // (NextLevel) what StartNextLevel carries
	int32 MapCarryGold = 0, MapCarryArmour = 0;
	TSharedPtr<class FJsonObject> MapCarryLoad;            // (Load) the save being loaded
	bool bMapLoadPending = false;                          // a map load is on its way: this world does nothing more
	bool bFromMapLoad = false;                             // this world's first build is the one a map load took over
	/** Over ol.mem.LoadMapAboveMB before the build: the rebuild as a level start JSON, and a real load of L_Runtime that
	 *  builds it (OutlawsMemSoak.cpp).  False: build in place as ever. */
	bool LoadMapInsteadOfRebuild();
	/** BeginPlay's part: a map load's rebuild taken over from the last world.  False: none waiting. */
	bool TakeMapLoadRebuild();
	/** LoadGame from a save already read (the file's, or a map load's level start). */
	void LoadGameFrom(const TSharedPtr<class FJsonObject>& O, const FString& What);

#if WITH_EDITOR
	// (TIDY-8: the headless INF test's, editor only as SendInfMessage)
	/** Nudge sectors named Name the way the use key does (from outside = the door in front of you). */
	UFUNCTION(BlueprintCallable, Category = "Outlaws|INF")
	void NudgeSector(const FString& Name, bool bFromOutside = true);

	/** World transform of the mesh that carries an elevator's geometry (morph groups). */
	UFUNCTION(BlueprintCallable, Category = "Outlaws|INF")
	FTransform ElevatorMeshTransform(const FString& Name) const;
#endif // WITH_EDITOR

	/** Logs every rooted object inside the world (its outer chain reaching it) and returns how many: a map load asserts
	 *  on any of them (UWorld::MarkObjectsPendingKill, "!IsRooted()"). Named when -OLRootDiag is given or any are found;
	 *  with bUnroot each is taken off the root (logged), so the map load that follows does not assert. */
	static int32 LogRootedWorldObjects(UWorld* World, const TCHAR* When, bool bUnroot = false);

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void Tick(float DeltaSeconds) override;
#if WITH_EDITOR
	virtual void PostLoad() override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
#endif

private:
	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> WorldMesh;

	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> WaterMesh;

	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> SkyMesh;
	/** The low cloud layer (user 2026-09-28): drawn by the sky's own material (M_OLR_SkySurface), so it shows on the
	 *  dome and on every sky surface alike (a separate see-through dome was hidden by the levels' sky ceilings): a flat
	 *  dome over the eye with the sky picture laid on it top-down, drifting, clear at its rim, solid overhead, turning
	 *  slowly so the wind seems to change. */
	void SetUpCloudLayer(bool bTiles, float Aspect);
public:
	/** Tests: the cloud layer's parameters. */
	FString CloudReport() const;
private:
	float CloudSpinSpeed = 1.2f;       // degrees a second (the sky turns 0.8 the other way)
	float CloudSpin = 0.f;             // radians
	/** The cloud picture's drift so far, 0..1 each way, kept here and handed to the material each tick as "CloudWind"
	 *  (user 2026-10-02: on the Quest the layer moved once a second; the material had worked it out from its own Time, a
	 *  half-precision float there, which steps by a second once the game has run 17 minutes). */
	FVector2D CloudDrift = FVector2D::ZeroVector;
	FVector2D CloudWindRate = FVector2D(0.012, 0.004);      // picture widths a second
	bool bCloudLayer = true;           // -OLNoCloudLayer
	bool bGradientSky = false;         // (BUG-TRAINSKY) this level's sky is the plain gradient dome (the train)
	// (BUG-TRAINSKY8BIT) with Remastered Graphics off the dome is one flat colour, the top row of the outer walls' 8-bit picture
	FLinearColor GradSkyTop = FLinearColor::Black, FlatSkyColour = FLinearColor::Black;
	FColor FlatSkyPx = FColor::Black;
	FString FlatSkyFrom;
	/** The dome's picture by the look: the flat colour with Remastered Graphics off, the gradient with it on. */
	void ApplyGradientSkyLook();
	// the sky's own surfaces (exterior ceilings, floors, sky wall bands), drawn with the sky by direction
	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> SkySurfaceMesh;
	static void SkyCollision(UProceduralMeshComponent* M);            // sky faces: bodies stop, everything else passes
	float WallFacingYaw(const FVector& Where, int32 SectorIndex, float Fallback) const;   // a pickup's back to its nearest wall
	void BuildSkyCeilingCollision(const FOutlawsLevel& L);             // the walled rooms' sky ceilings, invisible, bodies only
	AActor* MakeDynamiteActor(UProceduralMeshComponent* From, UProceduralMeshComponent*& OutMesh, const TCHAR* MeshName = TEXT("OutlawsDynamite"));
	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> SkySurfaceMid;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UProceduralMeshComponent>> ObjectMeshes;

	UPROPERTY(Transient)
	TArray<TObjectPtr<AActor>> SpawnedActors;

	// (REQ-COLLECTBOX) the level's box while it lies there (one of SpawnedActors), its glint, where it rests
	TWeakObjectPtr<AActor> CollectBoxActor;
	TWeakObjectPtr<class UStaticMeshComponent> CollectBoxGlint;
	FVector CollectBoxRest = FVector::ZeroVector;
	float CollectBoxTime = 0.f;
	FString CollectBoxSafe;
	void CollectBoxTaken(const TCHAR* How);

	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UTexture2D>> Textures;

	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UMaterialInstanceDynamic>> Materials;

	/** Both looks of every level texture (the original PCX, the remaster's HDX where there is one). */
	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UTexture2D>> TexturesLo;
	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UTexture2D>> TexturesHd;
	/** The HD sprite atlases (render targets), kept referenced. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTexture>> SpriteAtlasesHd;
	/** An atlas's HD look, built the first time it is asked for. */
	UTexture* SpriteHdFor(const TSharedPtr<OutlawsTextures::FSpriteAtlas>& Atlas);

	// the boss (Sanchez_New / TwoFeathers_New): one hidden actor, many places it may appear
	TWeakObjectPtr<AOutlawsEnemy> Boss;          // the first boss (taunts, cheats)
	TArray<FVector> BossSpawns;
	// every named outlaw of the level (Sanchez, Slim, Bloodeye, Bob Graham, Buckshot, Dr Death...): each has its
	// rows as places it may appear, one real actor hidden and still until most bad guys are down; the level
	// is won when all of them are dead (the ranch has three)
	struct FBossSlot { FString Func; FString Name; TWeakObjectPtr<AOutlawsEnemy> Actor; TArray<FVector> Spawns; };   // one per boss function
	TArray<FBossSlot> BossSlots;
	int32 TotalBadGuys = 0;
	float BossTimer = 2.f;
	float BossTauntWait = 15.f;
	/** R12 (user 2026-10-03): the late boss's calls come from one bearing that drifts (degrees; -1: none yet this level). */
	float BossTauntBearing = -1.f;
	bool bBossDefeated = false;
	bool bCutToMovie = false;               // this level ends on a hard cut to its movie (no fades)
	float LevelEndTimer = -1.f;
	FString NextLevelName;
	FString NextMovie;
	UPROPERTY(Transient)
	TObjectPtr<class AOutlawsMovieScreen> Movie;
	void StartNextLevel();
	/** StartNextLevel's work, then Then once the next level is built (co-op: the session's own start, OutlawsCoopInf.cpp). */
	void StartNextLevelThen(TFunction<void()> Then);
	void UpdateBoss(float Dt);
	/** The story order (OUTLAWS.RCS): the movie and level after this one. */
	FString StoryNext(FString& OutMovie) const;

	// sound generators (Gen_New TYPE AMB_SOUND / POINT_SOUND): wind and the like
	struct FSoundGen
	{
		FString Wav;
		float Freq = 5.f;
		FVector Pos = FVector::ZeroVector;
		bool bPoint = false;
		int32 Max = 1;
		float Wait = 0.f;
		TArray<TWeakObjectPtr<UAudioComponent>> Playing;
	};
	TArray<FSoundGen> SoundGens;
	// item generators (Gen_New TYPE ITEM: the train's rocks, shrubs and cacti dropped onto the rushing ground):
	// one scenery sprite every |FREQ| s (plus up to |RAND|), the first after |DELAY|, at most MAX_GENERATEES
	struct FItemGen
	{
		FString Name, Anim;
		FVector Pos = FVector::ZeroVector;
		float Yaw = 0.f;
		int32 Light = 31;
		float Freq = 15.f, Rand = 0.f, Wait = 0.f;
		int32 Max = 1;
		TArray<TWeakObjectPtr<AActor>> Alive;
	};
	TArray<FItemGen> ItemGens;
	void UpdateItemGens(float Dt);
	/** A still scenery sprite (no pickup, no prop) at these feet. */
	class AOutlawsSpriteActor* SpawnSceneSprite(const FString& Name, const FString& Anim, const FVector& Feet, float Yaw, int32 Light);
	// sector VELOCITY carries enemies and items as it does the player; an item carried into a TERMINATE
	// sector is gone. Each thing's sector is looked up ten times a second
	struct FCarry { float Check = 0.f; FVector Vel = FVector::ZeroVector; bool bTerminate = false; };
	TMap<TWeakObjectPtr<AActor>, FCarry> Carries;
	TArray<FBox2D> SectorBoxes;               // (CarryBySectors) each live sector's plan box this frame, for SectorAtBoxed
	void CarryBySectors(float Dt);
	/** The translucent material of tinted glass (M_OLR_Water with a pale grey-blue and a low opacity). */
	UMaterialInterface* GlassTintMaterial();
	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> GlassTintMid;

	// the level music: its MSC script's cues (research/outlaws_music.md), each track played once in
	// order, GOTO_CUE jumping back; stops at the end of the last cue
	struct FMusicCmd { bool bGoto = false; int32 Track = 0; TCHAR Disc = TEXT('A'); int32 Cue = 0; };
	TArray<TArray<FMusicCmd>> MusicCues;       // index = cue number - 1
	int32 MusicCue = -1;
	int32 MusicStep = 0;
	TWeakObjectPtr<class UAudioComponent> MusicAudio;
	double MusicEndReal = 0.0;                 // real seconds the track runs out (the next cue waits for it)
	/** (BUG-STUTTER2 (5)) the next track's wave and audio component made on a quiet frame before its cue, not playing yet:
	 *  the switch frame only starts it.  Held here (an unplayed component has nothing else keeping it) */
	UPROPERTY(Transient)
	TObjectPtr<class UAudioComponent> MusicArmed;
	FString MusicArmedKey;                     // its PlayWav key ("file:<path>" lower case)
	FString MusicArmFailedKey;                 // (made nothing for this one: no audio device, say; it plays the old way)
	FString MusicNextFile;                     // the track after the playing one, to read and make ready ahead
	int32 MusicAheadWait = 0;                  // frames after a switch before that starts (not on the switch frame)
	void TickMusicAhead();
	void WarmMusicAudio();                     // (in the load: a 2D sound's first making, 25 ms cold, not in play)
	bool bMusicAudioWarm = false;
	bool ArmMusic(const FString& File);
	void DisarmMusic();
	class UAudioComponent* StartArmedMusic();
	// sounds stopped when their samples run out, in real time: world timers crawl while the menu freezes
	// the world, and hover clicks left playing there stole the music's voice (the 2026-09-27 crash)
	TArray<TPair<TWeakObjectPtr<class UAudioComponent>, double>> TimedSounds;
	void StopSpentSounds();
	/** Every sound playing, and whether it is spared the underwater muffle (the bubbles). */
	TArray<TPair<TWeakObjectPtr<class UAudioComponent>, bool>> LiveSounds;
	bool bMuffled = false;
	static constexpr float MuffleHz = 700.f;
	void StartMusic(const FString& Msc);
	/** The main menu's music, looped: the remaster's music/TRK03A.flac (its exe names it among the menu's own files; the
	 *  Vault's is TRK05B); user 2026-09-28: the menu was silent. */
	void StartMenuMusic();
	bool bMenuMusic = false;
	void UpdateMusic();
	void StopMusic();

	// the level's palettes as colour grades against the one most sectors use (index MainPalette)
	struct FPaletteGrade { bool bValid = false; FLinearColor Rows[3]; };
	TArray<FPaletteGrade> PaletteGrades;
	int32 MainPalette = 0;
	void BuildPaletteGrades(const class FOutlawsAssets& A, const class FOutlawsLevel& L);
	void BuildFullBright(const class FOutlawsAssets& A, const class FOutlawsLevel& L);
public:
	/** REQ-FIREBRIGHT: the palette entries the main colour map keeps as they are on every row (no light dims them: the fire
	 *  ramp, 16..19 and 250..254, and white in the story's maps), the shade entries 0..15 aside. */
	TBitArray<> FullBrightIx;
	/** REQ-FIREBRIGHT (user 2026-10-28, snapshot 17: shsimms' campfire, its flame dark red at the top): art drawn full bright
	 *  whatever the light, by the original's rule: at least 40 % of its drawn pixels (the shade tints aside) are colours the
	 *  level's colour map never dims.  The campfires and fires come out at 44..100 %, the next (a51cow) at 31 %, the lamps
	 *  at 20 %, the characters under 1 %.  -OLOldFireBright: none (every sprite by its sector's light, as before). */
	bool IsFullBrightArt(const OutlawsTextures::FSpriteAtlas& Atlas, float* OutShare = nullptr) const;
	static bool OldFireBright();
	/** REQ-FIREBRIGHT2 (user 2026-10-28: the campfire's logs lit as bright as its flame, the HD flame's soft halo a flat light
	 *  red): the material for a sprite of this atlas.  Art drawn full bright gets M_OLR_SpriteFire and the atlas's mask of the
	 *  pixels its colours the colour map dims (S->FireMask, made on first use), so per pixel, as the original: the flame full
	 *  bright, the logs by the sector's light, the HD look's halo (outside the 8-bit drawing) its own colour, bright.  Every
	 *  other sprite M_OL_Sprite as before.  -OLOldFireBright2: the art full bright as a whole (REQ-FIREBRIGHT's look). */
	UMaterialInterface* SpriteMaterialFor(class AOutlawsSpriteActor* S, const OutlawsTextures::FSpriteAtlas& Atlas);
	static bool OldFireBright2();
	/** REQ-FIREBRIGHT2: a palette entry of full-bright art that stays bright whatever the light (its mask's 0). */
	bool FireKeepsBright(uint8 V) const;
	/** REQ-FIREBRIGHT3: the atlas's HD mask (FireMaskHd) from the remaster's own full-bright layer where its SPX has one, made
	 *  once when the HD look is first wanted (SpriteMaterialFor, SpriteHdFor); else the 8-bit colours' mask stays. */
	void FireMaskLayerFor(OutlawsTextures::FSpriteAtlas& At);
private:
	void UpdateSoundGens(float Dt);

	FVector PlayerStartLocation = FVector::ZeroVector;
	float PlayerStartYaw = 0.f;
	bool bHavePlayerStart = false;
	int32 PlacePlayerTries = 0;

	TArray<TWeakObjectPtr<AActor>> FloorStandingSprites;
	TArray<TWeakObjectPtr<AOutlawsEnemy>> Enemies;
	struct FProp
	{
		TWeakObjectPtr<AOutlawsSpriteActor> Sprite;
		FString Name;
		FVector Base = FVector::ZeroVector;   // cylinder bottom centre (world, refreshed from the sprite)
		FVector Offset = FVector::ZeroVector; // cylinder bottom relative to the sprite
		float Radius = 30.f, Height = 60.f;
		bool bShoot = true, bNudge = false, bNoCollide = false;
		int32 Current = 0, NextEvent = 0, Sector = -1;
		bool bActive = false;                 // a reaction choreography is running
		bool bGrabOnly = false;               // a rock (FUNC NULL): only there to be taken up, no shot or blast meets it
		int32 CoopId = -1;                    // co-op: its place in the level's build (the same on both machines)
		bool bCoopRemote = false;             // co-op: its running reaction is the other machine's (that one told its INF)
	};
	struct FPane
	{
		int32 S = -1, W = -1, MS = -1, MW = -1;
		FVector2D A, B;                        // the wall line (Outlaws x, z)
		double Bottom = 0.0, Top = 0.0;        // the opening (Outlaws y)
		UProceduralMeshComponent* Mesh = nullptr;
		TArray<UMaterialInstanceDynamic*> Mids;
		TArray<FString> Stems;                 // the texture each of Mids shows whole
		TArray<UTexture2D*> Frames;            // the broken sequence after the ATX STOP
		FString Sound = TEXT("shattr1");
		float Rate = 12.f;
		bool bBroken = false;
		int32 TintSection = -1;              // its tinted glass on Mesh (hidden once it breaks)
		float T = 0.f;
		int32 Shown = -1;
	};
	struct FDynamite { TWeakObjectPtr<UProceduralMeshComponent> Mesh; FVector Where = FVector::ZeroVector; bool bArmed = false; bool bGone = false;
		float Delay = 0.f; };            // armed by a blast: goes off this long after (the chain runs stick to stick)
	struct FSpark { TWeakObjectPtr<AOutlawsSpriteActor> Sprite; FVector Vel = FVector::ZeroVector; float Life = 0.f; bool bOneShot = false; bool bBlood = false;
		bool bFire = false; bool bLanded = false; float Size = 1.f; FVector Scale0 = FVector::OneVector;
		bool bDirt = false;             // a clod: crumbles where it strikes
		bool bFade = false; float Life0 = 0.f;      // fades out over the last 70 percent of its Life0 (user 2026-09-28)
		bool bGlass = false; };                     // a glint of a burst bottle: gone where it strikes        // lamp fire: falls, then burns where it lands
	float FireHurt = 0.f;                   // burning lamp fire's damage waiting to be dealt
	/** A blood spatter come down on a floor: a flat blood.nwx frame lying there (user 2026-09-28), the oldest going past
	 *  MaxBloodDecals. */
	void AddBloodDecal(const FVector& At, const FVector& Normal, UPrimitiveComponent* On = nullptr);
	TArray<TWeakObjectPtr<UProceduralMeshComponent>> BloodDecals;
	static constexpr int32 MaxBloodDecals = 150;
	/** A flat quad lying on a surface (Size cm across, turned Roll degrees about the normal), lit by its sector; on a
	 *  moving piece (a lift, a door) it rides that piece. Kept in Ring, the oldest dropped past Max. */
	UProceduralMeshComponent* AddDecal(const FVector& At, const FVector& Normal, double Width, double Height, float Roll, UMaterialInterface* Mat,
		const FVector2D& Uv0, const FVector2D& UvSize, UPrimitiveComponent* On, TArray<TWeakObjectPtr<UProceduralMeshComponent>>& Ring, int32 Max,
		const FLinearColor& Tint = FLinearColor::White, bool bSectorLight = true, bool bClip = true);
	/** The level's own visible surface (not a bad guy, the sky, water or an invisible blocker): where marks may go. */
	bool TakesMarks(const UPrimitiveComponent* C) const;
	/** A TNT blast's soot on the surfaces round it (user 2026-09-28): the nearer, the bigger and darker. */
	void AddScorchMarks(const FVector& Where);
	/** A darkening mark's instance (M_OLR_Soot, Modulate) for a texture drawn here: soft soot or cracks. */
	UMaterialInstanceDynamic* DarkMarkMaterial(bool bCrack);
	TArray<TWeakObjectPtr<UProceduralMeshComponent>> Scorches, Cracks;
	static constexpr int32 MaxScorches = 40;
	static constexpr int32 MaxCracks = 60;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> ScorchMat;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> CrackMat;
public:
	/** A bullet hole where a shot struck the level (user 2026-09-28); drawn here (the game has none). */
	void AddBulletHole(const FHitResult& Hit);
	/** A punch into a wall: cracks where the knuckles landed (user 2026-09-28). */
	void AddPunchMark(const FHitResult& Hit);
private:
	TArray<TWeakObjectPtr<UProceduralMeshComponent>> BulletHoles;
	static constexpr int32 MaxBulletHoles = 200;
	/** A dead bad guy's pool of blood spreading under him, 15 s, slowing as it goes (user 2026-09-29). */
	struct FBloodPool
	{
		TWeakObjectPtr<UProceduralMeshComponent> Mesh;
		FVector Corners[4];      // the full-size quad in the mesh's own space
		FVector Centre = FVector::ZeroVector;
		float Age = 0.f;
	};
	TArray<FBloodPool> BloodPools;
	TArray<TWeakObjectPtr<UProceduralMeshComponent>> BloodPoolRing;
	static constexpr int32 MaxBloodPools = 40;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> BloodPoolMat;
	void UpdateBloodPools(float Dt);
public:
	/** Starts a pool of blood on the floor under a body at rest (not in water). */
	/** A pool spreading under a body, Share of a whole one (what it has left to bleed); bDrop: a drop off a carried body,
	 *  small and at once.  The pool's mesh comes back (StopBloodPool stops it growing). */
	class UProceduralMeshComponent* StartBloodPool(const FVector& Body, float Share = 1.f, bool bDrop = false);
	/** The body under it was lifted: the pool stops where it has got to; how much of a whole one it had spread (0..1). */
	float StopBloodPool(class UProceduralMeshComponent* Pool);
	TArray<TWeakObjectPtr<UProceduralMeshComponent>> BloodDropRing;
private:
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> BulletHoleMat;
	TArray<FProp> Props;
	TArray<FPane> Panes;
	TArray<FDynamite> Dynamite;
	TArray<FSpark> Sparks;
	TArray<FString> ObjectSounds;                                   // outlaws.lab sounds.lst
	float BlastRadius = 50.f, BlastWallRadius = 15.f, BlastDamage = 12.f;   // pdynam.itm
	FString BlastSprite = TEXT("tntboom.nwx"), BlastSound = TEXT("explode");
	void LoadShootTables();
	void CollectPanes(const FOutlawsLevel& L, FOutlawsBuildFilter& StaticFilter);
	void BuildPanes(const FOutlawsLevel& L, const TMap<FString, struct FOutlawsTexInfo>& TexInfo, const TArray<FColor>& Palette);
	void AddProp(AOutlawsSpriteActor* S, const FString& Name, const TMap<FString, FString>& Data, int32 Sector, bool bHang);
	void PropReact(int32 Index, bool bNudge);
	void PropEnter(int32 Index, int32 Chor);
	void ShatterPane(int32 Index);
	void UpdateShootables(float Dt);
	void RefreshPropBases();
	void SpawnSparks(const FVector& Where, int32 Count);
	void Detonate(int32 Index);
	int32 Kills = 0;
	double NextTaunt = 0.0;
	// BUG-TAUNTRATE's clock (game time): the next taunt due (-1: nobody hunting), the last said, by whom; the next look
	double TauntDueAt = -1.0, TauntLastAt = -100.0;
	TWeakObjectPtr<class AOutlawsEnemy> TauntLastSpeaker;
	float TauntLook = 0.f;
	/** Say a taunt from E (BUG-TAUNTRATE): the line, the sound, the clock and its log line. */
	void SayTauntFrom(class AOutlawsEnemy* E, const TArray<FString>& List, const TCHAR* Why, double DistCm, double Now);
	double NextGrunt = 0.0;
	FString GotShotSound;                                           // the player hurt sound (hit.wav)
public:
	/** BUG-BOTTNT: the blasts this machine saw go off in the last few seconds (its own and those told from another), each with
	 *  its reach and full damage: a match body a blast killed is thrown from it (AOutlawsRemotePlayer::TickDeath). */
	struct FRecentBlast { FVector At; double Time; float RadiusCm; float Damage; };
	TArray<FRecentBlast> RecentBlasts;
	/** BUG-MPDEATHSOUND: this machine's player's death cry, the worn character's SND_DIE (James's jadie in single player). */
	FString DieSound = TEXT("jadie");
private:
	bool bQuietHurt = false;                                        // the hurt that follows makes no grunt (a damaging floor cries shriek1)
	double NextEnemyShot = 0.0;
	FString TauntSuffix;                                            // level ITM TAUNT_TYPE: t / m / c
	TMap<FString, TSharedPtr<OutlawsTextures::FSpriteAtlas>> AtlasCache;   // "nwx" or "all:nwx" -> atlas
	TArray<FColor> SpritePalette;
	TSharedPtr<OutlawsTextures::FSpriteAtlas> GetAtlas(const FString& Anim, bool bAllChors);
		AOutlawsSpriteActor* MakeSprite(const TSharedPtr<OutlawsTextures::FSpriteAtlas>& Atlas, const FString& Name, const FVector& FeetLoc, float Yaw, int32 Light);
	public:
		/** The game's animated fire (fire01.nwx) as a sprite material for a quad of one's own (the dynamite's fuse, user
		 *  2026-10-01): its cells' UV offsets in frame order, the rate, the cell's width over its height; null if missing. */
		UMaterialInstanceDynamic* MakeFireLook(UObject* Outer, TArray<FLinearColor>& OutCells, float& OutFps, float& OutAspect);
		UStaticMesh* GetSpriteQuad() const { return SpriteQuad; }
		/** For the held guns painted at runtime (OutlawsPlayerWeapons.cpp, user 2026-10-02): a pickup's atlas (its first
		 *  choreography, 1x) and the player's LABs it comes from; null when missing. */
		TSharedPtr<OutlawsTextures::FSpriteAtlas> GetPickupAtlas(const FString& Nwx) { return GetAtlas(Nwx, false); }
		const FOutlawsAssets* GetLabAssets() const { return AssetsRef.Get(); }
		/** REQ-COOPHORSES (OutlawsCoopHorses.cpp): James's horse at the level's start and the places beside it for each other
		 *  co-op player's own horse (slot 1 for player 2 and so on), worked out from the level's data as it is built, the
		 *  same on every machine.  Outlaws units. */
		struct FHorsePlan
		{
			FString Horse;                          // its object name (HORSEND in a Historical Mission, HOR1..3 in the story)
			FVector Pos = FVector::ZeroVector;      // James's horse (x, floor y, z)
			float Yaw = 0.f;
			int32 Sector = -1;                      // its sector (a nudge's event goes there: the mission's END_LEVEL)
			double StartDist = 0.0;                 // from the player's start
			TArray<FVector> Slots;                  // each slot's feet (x, floor y, z), slot 1 first
			TArray<int32> SlotSectors;
			FString Why;                            // no horse, or why fewer slots than wanted
		};
		static constexpr int32 HorseCoopIdBase = 900000;   // a copy's prop id in co-op: the base and its slot, alike everywhere
		/** The plan for a level (Live: its sectors with doors and lifts at their first stops; Moving: those sectors). */
		static bool PlanHorseSlots(const FOutlawsLevel& Live, const TSet<int32>* Moving, const FString& Level, int32 Want, FHorsePlan& Out);
		/** -OLHorseSlotsAll: every story and Historical Missions level's plan logged. */
		static void SurveyHorseSlots(const FOutlawsAssets& Assets);
		const FHorsePlan& GetHorsePlan() const { return HorsePlan; }
		int32 GetBuildSerial() const { return HorseBuildSerial; }
		/** Another player's horse in its slot (1..), a copy of James's: its sprite, its nudge (prop id HorseCoopIdBase + slot). */
		class AOutlawsSpriteActor* SpawnHorseCopy(int32 Slot, FString* OutWhy = nullptr);
		void RemoveHorseCopy(int32 Slot, AActor* Copy);
	private:
		FHorsePlan HorsePlan;
		int32 HorseBuildSerial = 0;
		void BuildHorsePlan();
	private:
	bool SpawnEnemy(const FString& Name, const TMap<FString, FString>& ItmData, const FString& Anim, const FVector& FeetLoc, float Yaw, int32 Light);
	/** Co-op (OutlawsCoopExtras.cpp, user 2026-10-02): a bad guy the level made, kept by Build for its copies. */
	struct FCoopCopySource { int32 Enemy = -1; FString Name; TMap<FString, FString> Data; FString Anim; int32 Light = 31; };
	/** Build's last step in a session: the host's extra bad guys (CoopExtras), each of the kind the list names (user
	 *  2026-10-02: an ordinary bad guy drawn at random, no longer his neighbour's copy), made after the level's own; never with
	 *  a key, a puzzle piece or anything the level's locks ask for to drop.  Returns how many. */
	int32 SpawnCoopExtras(const FOutlawsLevel& Built, const TMap<int32, FCoopCopySource>& Sources);
	/** REQ-SPREADSPAWN's test (-OLSpreadCheck): each extra against his sector, its lines and the other bad guys, then a capsule test; exits. */
	void CheckSpread(const FOutlawsLevel& Built);
	/** May an extra bad guy drop this (his kind's DROP_ITEM)?  No for keys, puzzle pieces and what the level's INF asks for. */
	bool CoopCopyMayDrop(const FString& Drop) const;
public:
	/** Co-op (user 2026-10-02): the host's extra bad guys for one level at one difficulty, handed over at each level start
	 *  (UOutlawsCoop::HandExtrasToLevel); every build of that level in the session makes them.  Empty in single player. */
	struct FCoopExtraSpot { int32 Row = -1; FVector Feet = FVector::ZeroVector; float Yaw = 0.f; FString Kind; FString Sprite; };
	TArray<FCoopExtraSpot> CoopExtras;
	FString CoopExtrasLevel;
	int32 CoopExtrasDifficulty = -1;
	int32 CoopExtrasPlayers = 1;
	/** Single player (REQ-SPMULT): the same extra bad guys from SoloBadGuys, listed here at the build (no session).  True when
	 *  there are any to make. */
	bool PrepareSoloExtras();
	/** What the last build made of them: the first one's index in the list of enemies (-1 none), how many, their checksum. */
	int32 CoopExtrasFirst = -1;
	int32 CoopExtrasMade = 0;
	uint32 CoopExtrasSum = 0;
private:

	// ---- INF (doors, lifts, triggers) ----------------------------------------------------------
	/** One moving piece of geometry per INF elevator (master + slaves). */
	struct FMoverGroup
	{
		int32 Elevator = -1;
		bool bMorph = false;
		UProceduralMeshComponent* Main = nullptr;     // morph: the whole group; vertical: the walls (rebuilt)
		UProceduralMeshComponent* Floor = nullptr;    // vertical: floors (translate in Z)
		UProceduralMeshComponent* Ceil = nullptr;     // vertical: ceilings (translate in Z)
		// vertical: the walls' collision, hidden copies of Main's faces, one per shape the walls have taken (up to 4), the
		// one for the present shape colliding. Main is redrawn at every change; a shape seen before only switches copies,
		// a new one is cooked: near a player at once, farther off at most once a second while it moves, at once when it
		// stops (BUG-LOGNOISE: miner's four crushers, going between their stops all level, cooked their walls ~13 times a
		// second)
		struct FWallSolid { UProceduralMeshComponent* Mesh = nullptr; uint32 Hash = 0; double UsedAt = 0.0; };
		TArray<FWallSolid> Solids;
		int32 ActiveSolid = -1;
		TMap<FString, FOutlawsMeshSection> WallShape; // vertical: Main's faces as last built (the solid's next cook)
		uint32 WallHash = 0;                         // vertical: WallShape's fingerprint
		bool bSolidStale = false;
		double SolidCookedAt = -1e9;
		double BuiltValue = 0.0;                      // elevator value the meshes were built at
		FVector Pivot = FVector::ZeroVector;          // morph spin centre (UE)
		FVector Dir = FVector::ForwardVector;         // morph move direction (UE, per unit)
		FOutlawsBuildFilter GroupFilter;              // morph: the whole group; vertical: the walls to rebuild
		FOutlawsBuildFilter FloorFilter;              // vertical: floors
		FOutlawsBuildFilter CeilFilter;               // vertical: ceilings
		bool bDirty = false;
		// (CPU1, ol.MoverMerge) a door at rest is drawn by its batch (Main hidden, still colliding)
		int32 Batch = -1;                             // its batch, -1 when never batched
		bool bInBatch = false;                        // baked into the batch at BatchedValue
		double BatchedValue = 0.0;
		double RestSince = -1.0;                      // seconds it has been still since it last moved
		TMap<FString, FOutlawsMeshSection> Shape;     // morph: the sections as built (the batch bakes them)
		int32 Updates = 0;                            // (tests, -OLMoverStats) UpdateGroup calls since the last report
		bool bWallsLate = false;                      // (QSPEED-3) vertical: moved out of sight, its walls not built since
	};
	double MoverStatsNext = 0.0;
	bool PlayerNearMover(const FMoverGroup& G) const;
	bool WallsCanWait(const FMoverGroup& G) const;
	TArray<FMoverGroup> Groups;
	// (CPU1) the train's carriage doors: resting morph doors near each other drawn as one mesh, a section per texture
	struct FMoverBatch
	{
		TObjectPtr<class UProceduralMeshComponent> Mesh;
		TArray<int32> Members;
		bool bDirty = false;
		bool bShown = false;            // drawn (half or more of its resting doors may be seen), else each seen door by its own mesh
	};
	TArray<FMoverBatch> MoverBatches;
	void BuildMoverBatches();
	void TickMoverBatches();
	void ShowMoverGroup(int32 g);
	void ShowMoverBatches(bool bForce = false);
	// scrolling surfaces (the train's ground and scenery): scrolls moving alike share one mesh, built
	// twice: a hidden copy that collides and a drawn copy whose UVs slide each frame
	struct FScrollMesh
	{
		int32 Scroll = -1;                  // the INF scroll whose offset it follows
		bool bWall = false;
		bool bCeil = false;
		// a floor's (ceiling's) texture frame (SurfaceTexFrame of the scroll's first sector) and the plane's turn: the
		// world offset goes into texture space through them
		FVector2D FrameA = FVector2D(1.0, 0.0), FrameN = FVector2D(0.0, 1.0);
		double Stretch = 1.0, Rad = 0.0;
		FOutlawsBuildFilter Filter;
		TObjectPtr<class UProceduralMeshComponent> Mesh;
		TArray<FOutlawsMeshSection> Sections;
		TArray<FVector2D> UVPerUnit;        // per section: UV per unit of offset (8 texels / texture size)
		FVector2D Shown = FVector2D(-1.0, -1.0);
	};
	TArray<FScrollMesh> ScrollMeshes;
	void BuildScrolls(const FOutlawsLevel& L, const TMap<FString, FOutlawsTexInfo>& TexInfo);
	void FillFloorShift(FOutlawsBuildFilter& F) const;
	void TickScrolls();
	// (REQ-SECTORLIGHT) the sectors an INF CHANGE_LIGHT lights: the drawn sections of their faces (the builder keeps them
	// apart: FOutlawsGeometry::SectorLight), and the sprites and 3D models standing in them, drawn at the light's level
	struct FLightFace
	{
		TWeakObjectPtr<class UProceduralMeshComponent> Mesh;     // a world part (no collision), the world mesh (a '~' section) or a scroll mesh
		int32 Section = 0;
		int32 Scroll = -1;                 // a scroll mesh's: ScrollMeshes index (TickScrolls draws its colours with its UVs)
		int32 ScrollSection = -1;
		int32 Elev = -1, Off = 0, Wall = 0;
		TArray<FVector> Pos;               // (a section update takes its positions back; normals and UVs stay as they are)
		TArray<FLinearColor> Col;
		float Shown = -1.f;                // the brightness drawn
		FBox Box = FBox(ForceInit);        // its faces' bounds (how near the eye is to them)
		// (cheaper) a world part's: a copy of its faces at each level it reaches, sections Section0 + k of its light's part,
		// the one at the light's level shown (a change shows another, no vertex sent); CopyLv: the level each copy is drawn at
		TArray<int32> CopyLv;
		int32 Section0 = -1;
		int32 ShownCopy = -1;
	};
	struct FLightThing
	{
		TWeakObjectPtr<class AOutlawsSpriteActor> Sprite;
		int32 Elev = -1, Off = 0;
		int32 Built = 31;                  // the light its model's colours were made at
		int32 Shown = -1;                  // the sprite's light set
		int32 ModelLv = -1;                // the light its model's colours are drawn at
		TArray<int32> Sections;            // its model's own sections (not the ink shell), their vertices as made
		TArray<TArray<FVector>> Pos;
		TArray<TArray<FLinearColor>> Col;
		TMap<int32, TArray<TArray<FColor>>> ColAt;     // (cheaper) its sections' colours at a light, made the first time it is drawn there
	};
	TArray<FLightFace> LightFaces;
	TArray<FLightThing> LightThings;
	// (cheaper) per CHANGE_LIGHT elevator: its faces and things, the level they were last looked at for, and whether one of
	// them waits (out of range) to be drawn: a light whose drawn level did not move is passed over
	struct FLightGroup
	{
		TArray<int32> Faces, Things;
		int32 Lv = MIN_int32;
		bool bWaiting = true;
	};
	TArray<FLightGroup> LightGroups;
	TArray<FIntVector> LightModelsDue;    // (a frame's: LightThings index, its light; drawn nearest first)
	bool bLightThingsFound = false;
	void FindLightThings();
	void TickSectorLights();
	float FloorHurt = 0.f;                  // damaging-floor damage waiting to be dealt
	double LastShriek = -10.0;              // real seconds of the last damaging-floor cry
	int32 HurtSector = -1;                  // the damaging sector last logged
	TSharedPtr<FOutlawsInfRuntime> Inf;
	TSharedPtr<FOutlawsLevel> LevelData;              // the parsed level, for water and sector queries
	TSharedPtr<FOutlawsAssets> AssetsRef;
	TMap<FString, FOutlawsTexInfo> TexInfoCache;
	// ---- ATX, the animated textures (user 2026-09-29: the train's wheels, switches, the stone key slots): a looping
	// script runs for every surface of its stem at once; a stopping one runs per wall, stepped by the INF's messages
	// to that wall (NEXT_STOP past its STOP, GOTO_STOP n from instruction n)
	struct FAtxState
	{
		FString Stem;
		TArray<OutlawsFormats::FOutlawsAtxInstr> Prog;
		int32 Pc = 0;
		float Rate = 12.f;
		float Wait = 0.f;
		bool bHalted = false;
		int32 Sector = -1, Wall = -1;            // a wall's own program; -1: a looping stem
		FString Frame;                           // the texture showing (its file)
		TArray<TWeakObjectPtr<UMaterialInstanceDynamic>> Mids;
	};
	TMap<FString, FAtxState> AtxLoops;          // by stem
	TMap<int64, FAtxState> AtxWalls;            // by FOutlawsBuildFilter::Key(sector, wall)
	void InitAtx(FAtxState& A, const FString& Stem, int32 Sector, int32 Wall);
	void RunAtx(FAtxState& A, float Dt);
	void ApplyAtxFrame(FAtxState& A, const FString& File);
	void TickAtx(float Dt);
	void AtxWallMessage(int32 Sector, int32 Wall, const struct FOutlawsInfMessage& Msg);
	UTexture2D* AtxFrame(const FString& File);
	void RefreshAtxFrames();
	FVector WallMidpoint(int32 Sector, int32 Wall) const;
	TMap<const UPrimitiveComponent*, int32> ComponentGroup;
	TArray<uint64> CrossWalls;                        // walls whose line triggers react to crossing
	int32 PlayerSector = -1;
	FVector2D PlayerPrev = FVector2D::ZeroVector;
	bool bHavePlayerPrev = false;
	bool bInputBound = false;
	bool bNeedPlace = false;

	struct FWav { int32 Rate = 22050; int32 Channels = 1; TArray<uint8> Pcm; };
	TMap<FString, TSharedPtr<FWav>> WavCache;
	/** (CPU1) the last few music tracks read from disk, newest last: a level's cues cycle through two or three short tracks
	 *  (the train's TRK03A/TRK04A every few seconds), and each read stalled the game thread 25 to 125 ms. */
	TArray<TPair<FString, TSharedPtr<FWav>>> MusicWavCache;
	/** (STUTTER) the next music track read and parsed on a worker while the current one plays (a cue's read on the game
	 *  thread was a 180 to 210 ms frame on the Quest); by the PlayWav key ("file:<path>" lower case) */
	TMap<FString, TFuture<TSharedPtr<FWav>>> MusicPrefetch;
	static void ParseRiffWav(const TArray<uint8>& D, FWav& Dest);
	void PrefetchMusic(const FString& File);
	bool MusicReady(const FString& File);
	FString PeekNextMusicFile() const;
	FString MusicFileFor(const FMusicCmd& C) const;
	/** (STUTTER) what a first use built in play (the blast, the scope picture, the dropped guns' models), built in the load */
	void PrebuildForPlay();
	void TickMusicTest(float Dt);        // (-OLMusicTest, BUG-STUTTER2 (5): cue switches forced every few seconds, each frame timed)
	float MusicTestT = 0.f;
	float MusicTestNextAt = 6.f;
	int32 MusicTestSwitches = 0;
	int32 MusicTestSeen = 0;             // (the switch count the test last read)
	TArray<float> MusicTestFrameMs, MusicTestMusicMs, MusicTestOtherMs;
	int32 MusicSwitchCount = 0;          // cues started (UpdateMusic)
	double MusicSwitchMs = 0.0;          // the last switch's time in UpdateMusic
	double MusicPlayMs[3] = { 0.0, 0.0, 0.0 };   // PlayWav's music path: wave made, sound spawned, the rest
	void TickStutterProbe(float Dt);     // (-OLStutterProbe: the first drop, blast, scope and music read timed, 8 s in)
	float StutterProbeT = 0.f;
	TMap<FString, TMap<FString, FOutlawsMeshSection>> GroundModels;     // (STUTTER) a 3D ground item's sections by .3do
	TMap<int32, TWeakObjectPtr<class UAudioComponent>> LoopSounds;   // elevator -> its SOUND 2 while moving<class UAudioComponent>> LoopSounds;   // elevator -> its SOUND 2 while moving
	TMap<int32, FString> UserMessages;                               // local.msg: USER_MSG id -> text
	struct FItemInfo { FString Name; FString UseOk; FString UseFail; };
	TMap<int32, FItemInfo> ItemsByUseId;                             // inventory ITMs: USE_ID -> name, SND_USE_S / SND_USE_F
	TMap<FString, int32> UseIdByItem;                                // inventory ITM name (lower) -> USE_ID
	/** An inventory ITM (FUNC Inv_GenericObject / Inv_HealthObject): stack limit, class, use sound. */
	struct FItemDef { FString Name; FString Class; int32 MaxCount = 1; int32 UseId = -1; FString UseOk; bool bWeapon = false; };
	/** (BUG-WHEELINV) what the wheel's item row and the wrist list: an inventory ITM, not a weapon, ammo or a counted stock. */
	bool IsListedItem(const FString& Item) const;
	TMap<FString, FItemDef> ItemDefs;                                // inventory ITM name (lower) -> def
	/** A ground item waiting to be walked over: FUNC Inv_GroundObject (keys, ammo, weapons, armour,
	 *  gold, medical bags) or Inv_InstantGroundObject (canteen / elixir: HEALTH_B on touch). */
	struct FPickup
	{
		struct FEntry { FString Item; int32 Count = 1; int32 Message = -1; FString Class; };
		TWeakObjectPtr<AActor> Actor;
		FString Ground;          // gbrsskey
		FString Item;            // ibrsskey (the first PICKUP)
		int32 UseId = -1;        // 101
		int32 Message = -1;      // its PICKUP_MSG, a local.msg line ("Brass Key")
		FString Sound;           // PICKUP_SOUND
		TArray<FEntry> Entries;  // PICKUP, PICKUP1.. with COUNT and PICKUP_MSG (PICKUP_COUNT of them)
		bool bInstant = false;   // Inv_InstantGroundObject
		int32 HealthB = 0;       // tenths of full health, instant
		bool bHeld = false;      // in a VR hand
		bool bWaitClear = false; // dropped or refused: no walk-over pickup until the player steps away
		int32 Serial = -1;       // placed by the level: its order there (a save names it so); -1 dropped since
		int32 CoopId = -1;       // co-op: the number both machines know it by (OutlawsCoopWorld.cpp); never set in single player
		bool bDropLock = false;  // (user 2026-10-02) dropped from the wheel by this machine's player: not theirs to take again
		double DropLockAt = 0.0; //   until they have been 1.5 m from it (or 30 s on, real time)
		bool bMapSeen = false;   // REQ-MAPITEMS: seen by this machine's player, on the map from then on
	};
	bool IsMapItem(const FPickup& P) const;
	int32 PickupSerials = 0;
	// (BUG-KNIFEPICKUP) placed ground items drawn as 3D models (the level's knives, gknife.3do) this build, and the first
	// serial they took: they are numbered after the sprites, so a save from before them names the sprites as it did
	int32 PlacedGroundModels = 0;
	int32 ModelSerialsFrom = 0;
	bool CollectPickup(int32 Index, bool bSilent = false);
	bool GiveEntry(const FPickup::FEntry& E);
	void ShowMessage(int32 Id, int32 Count = 0, const FString& Fallback = FString());
	// player stats
	float Health = 10.f;
	int32 Armour = 0;
	int32 Gold = 0;
	TMap<FString, int32> Counts;                                     // inventory item (lower) -> how many
	// what the marshal walked into this level with (the carry from the last one): a restart gives it back, not the
	// start kit (user 2026-09-28: bags, plates, oil, weapons and ammunition carry; keys and puzzles never do)
	FString EntryLevel;
	TMap<FString, int32> EntryCounts;
	int32 EntryGold = 0, EntryArmour = 0;
	// BUG-PLATERESTART: the worn plate's size then (ArmourMax; 0 none), so a bigger plate picked up since does not stay on
	// the restart's entered armour; -1 unknown (a save from before it): the restart keeps the size worn, as it did
	int32 EntryArmourMax = -1;
	bool bDead = false;
	float DeathTimer = 0.f;
public:
	/** The start kit: James's (cidjames.itm) in single player and for the co-op host; BUG-CHARLOADOUT: a chosen character's
	 *  own (Character, else UOutlawsCoop::KitCharacterFor: a match's or a co-op joiner's), its weapons worn with it.
	 *  KitMode: -1 as the session is (a match or the story), 0 the story's (StoryKit), 1 a match's (-OLCharLoadoutTest). */
	void ApplyStartKit(const FString& Character = FString(), int32 KitMode = -1);
	/** BUG-CHAROIL: the story's start kit (alone or co-op, campaign or Historical Missions): James's single-player kit with
	 *  Character's own ITM kit on top (each item the larger count); a match keeps the character's ITM kit alone. */
	static TMap<FString, int32> StoryKit(const FOutlawsAssets* A, const FString& Character);
	/** BUG-CHARLOADOUT: the character the kit was last made for (empty: James in single player) and its first weapon's SLOT. */
	FString WornCharacter;
	int32 StartWeaponSlot = -1;
private:
	void FadeCamera(float From, float To, float Time, bool bHold);
	bool CanSee(const FVector& From, const FVector& To, const AActor* Target) const;
	TArray<FPickup> Pickups;
	TArray<FString> Inventory;
	TArray<FString> InventoryLabels;
	void LoadGameTables();
	bool RegisterPickup(AActor* Sprite, const FString& ObjectName, const FString& ItmText);
	void UpdatePickups();

	UPROPERTY(Transient)
	TArray<TObjectPtr<UProceduralMeshComponent>> MoverMeshes;

	UPROPERTY(Transient)
	TObjectPtr<class USoundAttenuation> SoundAttenuation;

	void BuildInf(const FOutlawsLevel& L, const FString& InfText, FOutlawsBuildFilter& OutStaticFilter);
	FVector ElevatorLocation(const FOutlawsElevator& E) const;
	void CreateMoverMeshes();
	void UpdateGroup(FMoverGroup& G);
	void CookLiftWalls(FMoverGroup& G, bool bForce);
	void TickInf(float Dt);
	void TrackPlayer();
	void OnUsePressed();
	class UAudioComponent* PlayWav(const FString& Name, const FVector& Where, bool bLoop = false, bool b2D = false);
	/** A sound read from the LABs (or "file:<path>" from disk) and decoded whole; "name@from-to" a slice of it. */
	TSharedPtr<FWav> DecodeWav(const FString& Name, bool bDiskFile);
	// ---- R13: room sound (OutlawsRoomSound.cpp) ----
	/** The sector's sound class (0 open, 1 room, 2 cave), worked out once per level. */
	uint8 RoomClassOf(int32 Sector);
	bool EnsureRoomSound();
	/** The reverb bus let go: the audio mixer roots a submix it loads and never unroots it, so it is taken off the root here. */
	void ReleaseRoomSound();
	/** -OLTeardownTest=host | mphost | join (the questhost repro): a level, a movie skipped, the title menu, then that map load and End Game. */
	void TickTeardownTest(float Dt);
	/** (its ticker, on real time: the level actor stops ticking when idle, as on the title menu) */
	static void StartTeardownTest();
	void UpdateRoomSound(float Dt);
	/** A positioned sound's send to the reverb bus, by where it comes from. */
	void RoomSend(class UAudioComponent* AC, const FVector& Where);
	void TickReverbTest(float Dt);
	TArray<uint8> RoomClasses;
	FString RoomClassLevel;
	UPROPERTY(Transient)
	TObjectPtr<class USoundSubmix> RoomSubmix;
	UPROPERTY(Transient)
	TObjectPtr<class USubmixEffectReverbPreset> RoomReverb;
	uint8 RoomClassNow = 255;
	uint8 RoomClassLogged = 255;
	float RoomScaleNow = 1.f;
	float RoomFade = 1.f;
	FVector4f RoomFrom = FVector4f(0.f, 0.f, 0.f, 0.f);
	double LastEchoAt = 0.0;
	bool bRoomTestLog = false;
	public:
	/** The head under water: every sound but the bubbles through a low pass, playing ones and new ones (user 2026-09-30). */
	void SetMuffled(bool bOn);
	private:
	void WallsAt(const FVector& WorldPoint, int32 PreferSector, TArray<TPair<int32, int32>>& Out, bool bNearOnly = false, bool bAnyHeight = false) const;
	/**
	 * The moving piece an object in Sector rides (a lift floor it stands on, a MORPH_*2 door it is in),
	 * or null.  OutDzCm: lift meshes are built at their START stop, so a rider starts that far from
	 * its authored height.
	 */
	class USceneComponent* RideFor(int32 Sector, double ObjY, bool bHang, double& OutDzCm) const;
	void LoadDefaultAssets();
	bool TryPlacePlayer();
	/** -OLItemTest: the sprite-to-3D item prototypes (private OBJs outside the project) on the floor before the start. */
	void SpawnItemTest();
	TArray<TObjectPtr<UProceduralMeshComponent>> ItemTestMeshes;
	/** The sprite-to-3D models in place of item and barrel sprites (user 2026-09-28): <STEM>.obj + .json in the private
	 *  items3d folder (nothing cooked), as big as the sprite, turned by the object's yaw, lit by its sector; the quad
	 *  is hidden and the sprite actor keeps doing the pickup / prop work. -OLSpriteItems keeps the sprites. */
	bool UseItemModel(class AOutlawsSpriteActor* S, const FString& Anim, float Yaw);
	/** After the objects are placed: the 3D stand-ins moved out of the walls and apart from each other, a little, where
	 *  the floor under them stays the same (user 2026-10-01). -OLNoSettle leaves them as placed. */
	void SettleItemModels();
	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<UMaterialInstanceDynamic>> ItemModelMats;
	/** The headset's ink shell round item models (OutlawsInk): near-black, one-sided, at the world's depth. */
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> ItemInk;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> BloodMat;
	/** Each chain of joined underwater sectors: its surface colour's hue (brightest channel 1) and its surface height.
	 *  The faces those sectors own are drawn in sections of their own per chain (FOutlawsGeometry::SectorWaterGroup),
	 *  their material tinted by it, deeper more (user 2026-09-28: on the material, split by the underwater flag). */
	TArray<FLinearColor> WaterChainHue;
	TArray<double> WaterChainSurface;
	/** The clods' sprite (drawn here: the game has none): four frames of earth, kept for the session. */
	TSharedPtr<OutlawsTextures::FSpriteAtlas> DirtAtlas;
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> DirtTexture;
	/** The glitter of a burst bottle: four frames of glass slivers (drawn here), kept for the session. */
	TSharedPtr<OutlawsTextures::FSpriteAtlas> GlassAtlas;
	UPROPERTY(Transient)
	TObjectPtr<UTexture2D> GlassTexture;
	/** A burst bottle's pieces: one mesh, each piece moved on the CPU (two faces of a pair of the bottle's triangles). */
	struct FShard { FVector C = FVector::ZeroVector; FVector V = FVector::ZeroVector; FQuat R = FQuat::Identity; FVector W = FVector::ZeroVector;
		FVector N = FVector::UpVector; int32 First = 0; int32 Num = 0; bool bRest = false; };
	struct FGlassBurst { TWeakObjectPtr<UProceduralMeshComponent> Mesh; TArray<FShard> Shards; TArray<FVector> Local, LocalN;
		TArray<FVector2D> UV; TArray<FLinearColor> Col; float Age = 0.f; };
	TArray<FGlassBurst> GlassBursts;
	void UpdateGlass(float Dt);
	FString BreakSoundOf(const AOutlawsSpriteActor* S) const;
	FVector PropHitDir = FVector::ZeroVector;   // the latest shot's (or blast's) way into a prop: a bottle bursts along it
	int32 ClearCount = 0;
	int32 NumItemModels = 0;
	/** Item models that twirl (their json's "spin", degrees a second): turned each tick. */
	TArray<TPair<TWeakObjectPtr<UProceduralMeshComponent>, float>> SpinningModels;
	/** A model whose cloth waves as its sprite's idle loop did (the flags, user 2026-10-01: keyframes from the sprite's
	 *  frames, anim_catalogue): its rest vertices, and the wave's measured sag table along the cloth per frame. */
	struct FWavingModel
	{
		TWeakObjectPtr<UProceduralMeshComponent> Mesh;
		TArray<TArray<FVector>> Base, Nrm;
		TArray<TArray<FVector2D>> UV;
		TArray<TArray<FLinearColor>> Col;
		FVector Dir = FVector::ForwardVector;    // along the cloth from the pole (flat)
		FVector Side = FVector::RightVector;     // the cloth's face normal: the ripple's way
		float PoleR = 1.f, Fps = 10.f, Ripple = 9.f, Phase = 0.f, Wait = 0.f;
		TArray<float> X;                          // cm out from the pole where the sag is measured
		TArray<TArray<float>> Drop;               // per frame, the centre line's drop (cm) at each X
	};
	TArray<FWavingModel> WavingModels;
	TArray<FString> WavingStems;              // (each waving model's stem, for its ink shell)
	void UpdateWavingModels(float Dt);
	public:
	/** A model's keyframed reaction to a chor, from its sprite's frames (anim_catalogue, 2026-10-01): per step an angle
	 *  (about its top, or its foot) and a lift, at the chor's rate; the parts hidden when it plays (a lantern's flame). */
	struct FOutlawsKeyAnim
	{
		float Fps = 10.f;
		bool bTop = false;
		TArray<float> Deg, Lift;
		TArray<FString> Hide;
	};
	bool ItemKeyAnim(const FString& Stem, int32 Chor, FOutlawsKeyAnim& Out) const;
	private:
	void StartPropKeys(UProceduralMeshComponent* M, const FVector& Dir, const FOutlawsKeyAnim& KA);
	/** Hide these named parts (OBJ materials) of a model, and stop a flame among them flickering. */
	void HideItemSections(UProceduralMeshComponent* M, const FString& Stem, const TArray<FString>& Names);
	/** The ink shell round a model, leaving out its "noink" parts (a flame). */
	void ShellItemModel(UProceduralMeshComponent* M, const FString& Stem);
	/** A flame card flickering through its sprite's drawings (the lantern: sizes and brightness a frame, at its fps). */
	struct FFlickerModel
	{
		TWeakObjectPtr<UProceduralMeshComponent> Mesh;
		int32 Section = -1, Shown = -1;
		TArray<FVector> Base, Nrm;
		TArray<FVector2D> UV;
		TArray<FLinearColor> Col;
		FVector Root = FVector::ZeroVector;
		float Fps = 10.f, Phase = 0.f;
		TArray<float> Scale, Bright;
	};
	TArray<FFlickerModel> FlickerModels;
	/** A lamp's fire (user 2026-10-01: lit as the carried lantern is): the game's animated fire sprite in each glass of a
	 *  lit model ("flames"), turned to the eye, out when that glass is (shot); on the model, so it swings with it. */
	struct FLampFlame
	{
		TWeakObjectPtr<UStaticMeshComponent> Quad;
		TWeakObjectPtr<UProceduralMeshComponent> Mesh;
		int32 Section = -1;
	};
	TArray<FLampFlame> LampFlames;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> LampFireMid;
	TArray<FLinearColor> LampFireCells;
	float LampFireFps = 10.f, LampFireAspect = 0.66f;
	/** A hanging lamp ("hang": the lanterns, the hanging lamp, the chandelier) swung as a pendulum from its top when shot
	 *  or struck (user 2026-10-01); a chandelier's shades ("bulbs") each broken alone. */
	struct FHangModel
	{
		TWeakObjectPtr<UProceduralMeshComponent> Mesh;
		FString Stem;
		FVector PivotLocal = FVector::ZeroVector;      // its top, in the model's own frame (as placed: world-aligned)
		FVector2D Tilt = FVector2D::ZeroVector, Vel = FVector2D::ZeroVector;      // radians each way, and a second
		float Len = 50.f;
		bool bBulbs = false, bMoved = false;
		float Cool = 0.f;
		float HitCool = 0.f;                       // (a bad guy struck once a swing)
		TArray<int32> Glass;
	};
	TArray<FHangModel> HangModels;
	void UpdateLamps(float Dt);
	void SwingHanging(FHangModel& Hm, const FVector& Dir, float RadPerSec);
	void BreakShade(FHangModel& Hm, int32 Section);
	/** A plant bending about its base as its sprite's idle loop swayed (the river plants, the cattails' gust; user
	 *  2026-10-01): the top's measured sideways shift a step, the bend growing with the height squared, all leaning the
	 *  level's one wind way; a little flutter leaf by leaf. */
	struct FBendModel
	{
		TWeakObjectPtr<UProceduralMeshComponent> Mesh;
		FString Stem;
		TArray<TArray<FVector>> Base, Nrm;
		TArray<TArray<FVector2D>> UV;
		TArray<TArray<FLinearColor>> Col;
		float Height = 100.f, Fps = 10.f, Flutter = 0.5f, Phase = 0.f, Wait = 0.f;
		TArray<float> Top;
	};
	TArray<FBendModel> BendModels;
	/** A 3D prop knocked by a shot: it leans away along the hit, pivoting on its far bottom edge, and rocks back to
	 *  stand where it stood (user 2026-09-29: barrels went back to their sprite frames to react). */
	struct FPropWobble
	{
		TWeakObjectPtr<UProceduralMeshComponent> Mesh;
		FVector Foot = FVector::ZeroVector;      // the model's rest location (its foot point)
		FQuat Rest = FQuat::Identity;
		FVector Dir = FVector::ForwardVector;    // the way it leans (flat)
		float Radius = 20.f, Peak = 8.f, T = 0.f;
		// a keyframed swing instead (the bell): degrees a step at Fps, about Top (its hanging point), toward Dir
		TArray<float> Keys, Lift;
		float Fps = 10.f;
		bool bTop = true;
		FVector Top = FVector::ZeroVector;
	};
	TArray<FPropWobble> PropWobbles;
	void StartPropWobble(UProceduralMeshComponent* M, const FVector& Dir, float Radius);
	void UpdatePropWobbles(float Dt);
public:
	/** -OLWobbleTest: knock the first prop with a model that rocks (a barrel); report its lean. */
	FString DebugKnockModelProp(bool bKnock);
private:
	UProceduralMeshComponent* NewMeshComponent(const TCHAR* Name, bool bCollision);
	/** An invisible collision mesh from the opening flags (ALL, PAWN, SHOT, ENEMY). */
	UProceduralMeshComponent* MakeBlocker(const FString& Kind, const FOutlawsMeshSection& S);
	// sectors that share space with another they do not adjoin (the mill's rooms, the fort's room inside its yard).
	// The Jedi portal renderer only draws what the camera's sector sees through its openings, so the two are never
	// drawn together; here each such sector has its own meshes, and of each pair the one more openings away from the
	// player's sector is hidden, and the player's body and shots pass it (bad guys still stand on it)
	// (user 2026-09-28)
	struct FOverlapPart { TWeakObjectPtr<UPrimitiveComponent> Comp; bool bDrawn = true; };
	TArray<TPair<int32, int32>> OverlapPairs;
	TMap<int32, TArray<FOverlapPart>> OverlapParts;
	TSet<int32> OverlapHidden;               // out of sight (and shots pass them)
	TSet<int32> OverlapIgnored;              // the body passes through them (a superset: some stay drawn)
	/** An INF "ELEVATOR EXPLODE" sector (a plug in a passage: the cliff's cracked wall, the hideout's cattle pen, the
	 *  wharf's warehouse wall...): its open faces and their mirrors stand as solid walls, drawn with their mid texture and
	 *  signs (the crack), until a blast goes off within WALL_RADIUS; then they are gone (user 2026-09-29). */
	struct FExplodeWall
	{
		FString Name;
		int32 Sector = -1;
		TArray<uint64> Walls;               // FOutlawsBuildFilter::Key of each face
		FOutlawsMeshSection Blocker;        // the plug's open faces, floor to ceiling
		FBox Box = FBox(ForceInit);
		TArray<TWeakObjectPtr<UPrimitiveComponent>> Parts;
		bool bBlown = false;
	};
	TArray<FExplodeWall> ExplodeWalls;
	void BuildExplodeWalls(const FOutlawsLevel& L, const TMap<FString, FOutlawsTexInfo>& TexInfo);
	void BlowExplodeWall(FExplodeWall& X, bool bQuiet, const FVector& From);
	FOutlawsBuildFilter OverlapCarried;       // faces of the overlap sectors a lift or a scroll carries (not on the overlap meshes)
	int32 OverlapFrom = -2;
	void FindOverlaps(const FOutlawsLevel& L, const FOutlawsBuildFilter& Static, TSet<int32>& OutSectors);
	void BuildOverlapParts(const FOutlawsLevel& L, const TMap<FString, FOutlawsTexInfo>& TexInfo, const FOutlawsBuildFilter& Static);
	void UpdateOverlaps(int32 FromSector);
	void ApplyOverlapChoice(int32 BodySector, bool bLog);       // each tick: drawn if the eye's portal flood reaches it, solid if the body (in BodySector) can touch it
	// portal culling (2026-10-01): each tick the eye's portal flood (and the body's, when it stands in another sector),
	// plus one sector beyond as a margin, is the set of sectors that may be seen; sprites (AOutlawsSpriteActor::
	// SetCulling) and door / lift meshes outside it are hidden in game.  ol.cull 0 turns it off (A/B on the headset)
	TSet<int32> SeenFromEye;                  // this tick's floods (the overlap choice reads them)
	TMap<int32, int32> SeenDepth;             // ... and the fewest openings each was reached through
	/** Sectors sharing space with another, left out this tick: of two both seen, the one three or more openings further
	 *  than the other (user 2026-10-01: a crawlway showed the room it runs under, and its things; one cannot step from one
	 *  into the other) */
	TSet<int32> OverlapLosers;
	TBitArray<> VisibleSectors;               // V1: SeenFromEye, held 0.15 s
	TArray<double> SeenAt;                    // (V1) when each sector was last flooded
	/** V1: a thing's sector after its move From -> P (Was its sector, -1 none yet): the walls crossed, else Which3D at P. */
	int32 TrackThing(int32 Was, const FVector& From, const FVector& P) const;
	// (BUG-POPIN, instrumentation first) how often each sector and each door/lift group came and went from view in the last
	// two seconds; any that flipped three times or more is logged with where the player and the eye were
	TBitArray<> ChurnPrevVisible;
	TArray<uint16> ChurnSector, ChurnGroup;
	double ChurnNextLog = 0.0;
	int32 VisFlipsTotal = 0;                 // (tests) every may-be-seen flip since the level began
public:
	int32 DebugVisFlips() const { return VisFlipsTotal; }
	FString DebugVisScan(const AActor* Viewer, int32& OutFalse, int32& OutPairs) const;
	/** (BUG-DOORWAYGEO, -OLDoorwayGeoProbe) rays all round from EyeW to the level's faces: how many land on a face whose
	 *  sector this tick's flood left out, and why, by sector. */
	FString DebugEyeScan(const AActor* Viewer, const FVector& EyeW, int32& OutRays, int32& OutMissed, TArray<int32>* OutMissedSectors = nullptr) const;
	/** (BUG-INVISWALLS, -OLInvisScan) the body's capsule swept over every floor: where it stops but the original walks on. */
	FString DebugInvisScan(const APawn* Body, int32& OutCandidates) const;
	mutable int32 DebugFloodMode = 0;         // (tests) the flood's rules lifted, DebugVisScan
	mutable int32 DebugFloodPops = 0;         // (tests) the last flood's arrivals passed on
	FString DebugGroupsHidden() const;        // (tests) the doors and lifts the culling hides, by name
private:
	TArray<FBox2D> CullBoxes;                 // the static plan's sector boxes, for the sprites' sector lookups
	/** Per sector, the sectors whose CullBoxes meet its own (itself too), in index order: a point inside one sector's box
	 *  can only be inside the boxes on its list, so CullSectorOf scans that list instead of every sector. */
	TArray<TArray<int32>> CullNear;
	uint32 CullGen = 1;                       // bumped each time CullBoxes are made (older hints are dropped)
	void BuildCullNear();
	int32 CullScan(const FVector2D& Pt, double Y, int32 N, const TArray<int32>* Only) const;
	TArray<TArray<int32>> GroupCullSectors;   // each mover group's sectors (its own and those whose walls it moves)
	TBitArray<> GroupCulled;
	double NextCullLog = 0.0;
	bool bCullFnSent = false;
	void UpdateVisibility();
	/** (QSPEED-3) what a flood depended on: how far the eye may move (game units, in plan and in height alike) with every
	 *  choice the flood made coming out the same, and the sectors whose floor and ceiling heights it read. */
	struct FFloodBounds
	{
		double SafeR = 0.0;
		TBitArray<> Read;
		int32 Site = 0;                 // (tests) which choice set the radius
	};
	void PortalFlood(const FOutlawsLevel& L, int32 From, const FVector2D& Eye, double EyeZ, TSet<int32>& Out, TMap<int32, int32>* Depth = nullptr, FFloodBounds* Bounds = nullptr) const;
	/** (QSPEED-3) the eye's last flood, kept while nothing it read has changed: the same level, start sector and slack, no
	 *  explodable wall blown since, no height it read moved since (FOutlawsInfRuntime::SectorHeightGen), and the eye within
	 *  its safe radius of where it was.  Then a new flood would come out the same, to the sector (ol.VisCache 0: every tick). */
	struct FFloodCache
	{
		bool bValid = false;
		const FOutlawsLevel* Level = nullptr;
		int32 NumS = 0;
		int32 From = -1;
		FVector2D Eye = FVector2D::ZeroVector;
		double EyeZ = 0.0;
		float Slack = 0.f;
		int32 Blown = 0;
		uint64 HeightGen = 0;
		FFloodBounds Bounds;
		FVector2D PrevEye = FVector2D::ZeroVector;   // the eye at the last tick (a still eye's floods work out their bounds)
		double PrevEyeZ = 0.0;
		bool bHavePrev = false;
		bool bLastTracked = false;                    // the last flood worked out its bounds
		int32 KeptSince = 0;                          // ticks kept since the last flood
		int32 Backoff = 0, WaitLeft = 0;              // floods to wait before bounds again (doubling while they keep nothing)
	};
#if WITH_OUTLAWS_TESTS
	void PortalFloodRef(const FOutlawsLevel& L, int32 From, const FVector2D& Eye, double EyeZ, TSet<int32>& Out, TMap<int32, int32>* Depth) const;
#endif
	FFloodCache FloodCache;
	uint32 FloodGeomGen = 0;                 // (QSPEED-3) bumped when a level goes: the flood's plan lists are made again
	bool FloodStillHolds(const FOutlawsLevel& L, int32 From, const FVector2D& Eye, double EyeZ);
	int32 BlownWalls() const;
	// (QSPEED-3) counts since the level began: ticks, floods run, and (tests, -OLVisCacheCheck) floods kept that were checked
	// against a new one, and how many of those differed
	int64 VisTicks = 0, VisFloods = 0, VisTracked = 0, VisChecked = 0, VisDiffers = 0, VisPops = 0;
	int64 VisRadii[4] = { 0, 0, 0, 0 };       // the floods' safe radii: under 1 mm, 1 cm, 10 cm, more
	int64 VisWhyNot[5] = {};                  // why a flood was not kept: none kept, sector or level, eye moved, wall blown, height moved
	int64 VisSites[32] = {};                  // (tests) how often each choice set the radius
	double VisCacheNextLog = 0.0;
public:
	FString DebugVisCacheStats() const;
	void DebugVisCounts(int64& OutTicks, int64& OutFloods) const { OutTicks = VisTicks; OutFloods = VisFloods; }
private:
	void NearFlood(const FOutlawsLevel& L, int32 From, const FVector2D& Body, TSet<int32>& Out) const;
	int32 ClampThinOverlaps(FOutlawsLevel& L, const TSet<int32>& Moving);
public:
	FString DebugOverlapReport(int32 Sector) const;      // a test aid: what shares space with the sector (slopes included)
private:
public:
	/** What the player's shots pass through: the hidden side of each overlapping pair. */
	void IgnoreHiddenOverlaps(struct FCollisionQueryParams& Q) const;
	/** What the player's body passes through: the overlapping sectors out of its reach (a hand's ledge search the same). */
	void IgnorePassedOverlaps(struct FCollisionQueryParams& Q) const;
	// (BUG-MILLCLIMB, OutlawsZoneGate.cpp) a zone gate: a sector the player must pass through between two parts of a level
	// that share space (the mill's yard and the hall built inside it).  Which side the player is on comes from the order of
	// the sectors walked through (out of the gate into one side or the other); while on one side, the other side's
	// overlapping sectors are off for this machine's player: not drawn, passed through, and left out of the sector lookups
	// made for the player (its tracking, the climb's landing, the unstuck, the ground-near search).  A teleport, a load or
	// the level's start decides the side again from where the body stands.  The gates: GZoneGateDefs
	/** The sectors switched off for the local player now (null: none). */
	const TBitArray<>* PlayerSkip() const { return bZoneSkipOn ? &ZoneSkip : nullptr; }
	/** The player was teleported: the next tick's tracking takes it as a jump (a zone gate's side decided again). */
	void NotePlayerTeleported();         // (before the movement looks for a floor there: the overlap choice from the new place)
	/** The sector a climb from the player's sector lands in at FeetUE, when a sector holding the feet there can be reached
	 *  from the player's sector through openings near the climb (from FromUE); -1 when none can (no climb). */
	int32 ClimbLandingSector(const FVector& FeetUE, const FVector& FromUE, FString* OutWhy = nullptr) const;
	/** (logs) the zone gates and the player's side of each. */
	FString DebugZoneReport() const;
	/** (tests, -OLZoneScan) eyes over every sector (a grid and each opening, crouched and standing): of the Targets, the
	 *  ones each sector's floods reach, a log line for each sector that reaches any. */
	void DebugZoneScan(const TSet<int32>& Targets) const;
	/** (tests) for each zone gate: from every sector of each side, and the gate, whether a flood reaches a sector that side
	 *  switches off.  Returns the eyes that do (0: the gate holds). */
	int32 DebugZoneGateCheck(FString& OutReport) const;
	/** (tests) force the player's side of gate 0 (the next tick's tracking works from it). */
	void DebugSetZone(int32 Side);
	int32 DebugZoneNow() const { return ZoneNow.Num() > 0 ? ZoneNow[0] : -2; }
	int32 DebugZoneOf(int32 Si) const { return ZoneGates.Num() > 0 && ZoneGates[0].Zone.IsValidIndex(Si) ? ZoneGates[0].Zone[Si] : -2; }
	int32 DebugZoneGateSector() const { return ZoneGates.Num() > 0 ? ZoneGates[0].Gate : -1; }
	bool DebugZoneOffNow(int32 Si) const { return bZoneSkipOn && Si >= 0 && Si < ZoneSkip.Num() && ZoneSkip[Si]; }
private:
	struct FZoneGate
	{
		int32 Gate = -1;
		TArray<int8> Zone;                    // each sector's side (0 or 1); -1 the gate itself and what joins neither side
		TSet<int32> Off[2];                   // switched off while the player is on side 0 (1)
		int32 Into[2] = { -1, -1 };           // a sector next to the gate on each side (logs)
	};
	TArray<FZoneGate> ZoneGates;
	TArray<int8> ZoneNow;                     // per gate the player's side, -1 not known yet (nothing off)
	TBitArray<> ZoneSkip;                     // the union of the Off sets of the sides the player is on
	bool bZoneSkipOn = false;
	FVector ZonePrevBody = FVector::ZeroVector;   // the body last tick (a move of over 2.4 m in one tick is a jump)
	bool bZoneHavePrev = false;
	void BuildZoneGates(const FOutlawsLevel& L);
	void RefreshZoneSkip();
	/** The sector the body is in after a jump (a teleport, a load, the level's start), from where it stands: of the sectors
	 *  holding the feet, the one whose floor is nearest under them; each gate's side then follows from it. */
	int32 ZoneRedecide(double X, double Y, double Z, const TCHAR* Why);
	/** After the tracking's pick: a walk out of the gate (or any move the tracking explains) onto the other side switches. */
	void ZoneAfterMove(int32 From, int32 Now, bool bByCrossing);
	/** The off sectors added to the overlap choice's passed-through and hidden sets. */
	void ZoneForceOff(TSet<int32>& Hide, TSet<int32>& Unseen) const;
private:
	/** WaterChain >= 0: the instance for that texture under that chain's water (its WaterTint / WaterSurface set). */
	UMaterialInstanceDynamic* MaterialFor(UMaterialInterface* Parent, const FString& TexKey, int32 WaterChain = -1, int64 AtxWall = -1);
	/** Marks the sections to collide and cooks the mesh's collision once (TEXLOAD). */
	static void CookCollisionOnce(UProceduralMeshComponent* Mesh, const TArray<int32>& Collide);
	int32 AddSections(UProceduralMeshComponent* Mesh, const TMap<FString, FOutlawsMeshSection>& Sections, bool bCollision, UMaterialInterface* Opaque, UMaterialInterface* Masked, int32& OutTris, int32 FirstSection = 0, TArray<int32>* OutCollide = nullptr);
	friend class UOutlawsCoop;                // co-op (OutlawsCoopWorld.cpp): the pickups and the player's stats by index
	friend class AOutlawsRemotePlayer;        // (BUG-BOTTNT: a thrown body's blast seen through the level, CanSee)
	friend class UOutlawsStats;               // (user 2026-10-02: the level's end panels, OutlawsStats.cpp: its screen with no cutscene)
};
