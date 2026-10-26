// REQ-EXTRAS (user 13 Oct, v1.1): the achievements the Extras pages show, how each is earned, and the collectible boxes' list.
//
// Stored in the existing achievements.json (OutlawsUnlocks.h), per profile, beside what REQ-UNLOCKS and REQ-LEVELTIMER keep there:
//
//   "Marshal": { "earned": { "story": {...}, "boss_drdeath": { "when": "...", "difficulty": "Bad", "coop": false } },
//                "unseen": [...], "times": {...},
//                "progress": { "mt_killed": [ "shfort" ], "mt_captured": [], "boxes": [ "hideout" ] } }
//
// "earned" keeps its old shape (an older file reads as it always did; "story" and "story_ugly" are the same two ids the unlocks
// use); "progress" is new and an older build leaves it alone.  The version stays 1.
//
// The names: where an achievement mirrors one of the remaster's, its name, description and icon (Achiev_IconNN) are read from the
// player's own Common.kpf / UI.kpf at runtime (loc_english.txt out_*_name / out_*_desc); the English lines here are only what is
// shown without the remaster.  Nothing of the remaster's is shipped.
#pragma once

#include "CoreMinimal.h"

class AOutlawsLevelActor;
class AOutlawsPlayer;

namespace OutlawsAchievements
{
	struct FDef
	{
		const TCHAR* Id;           // its key under "earned"
		const TCHAR* LocKey;       // the remaster's out_<key>_name / out_<key>_desc, or nullptr (ours)
		const TCHAR* Title;        // the English name without the remaster
		const TCHAR* Desc;         // the English line saying how to earn it
		int32 Icon;                // the remaster's Achiev_IconNN (1..26); 0 none
		bool bHidden;              // "???" until earned
		bool bOwnDesc = false;     // the name and icon the remaster's, the line ours (it is earned another way here)
	};

	OUTLAWSRUNTIME_API const TArray<FDef>& All();
	const FDef* Find(const FString& Id);
	/** Its name and how to earn it: the remaster's words where the player has them (P's Common.kpf), else ours. */
	FString TitleOf(const FDef& D, AOutlawsPlayer* P);
	FString DescOf(const FDef& D, AOutlawsPlayer* P);
	OUTLAWSRUNTIME_API bool IsEarned(const AOutlawsLevelActor* L, const FString& Id);
	/** "Earned 13 Oct 2026" (empty: not earned). */
	FString EarnedLine(const AOutlawsLevelActor* L, const FString& Id);
	/** Its progress so far ("Boxes found: 3 of 9"); empty when it has none to show. */
	FString Progress(const AOutlawsLevelActor* L, const FString& Id);
	int32 EarnedCount(const AOutlawsLevelActor* L);
	/** Each of Ids not yet earned on this profile: stored (when, difficulty, co-op), logged ("OLACH earned"), a toast queued. */
	OUTLAWSRUNTIME_API TArray<FString> Earn(AOutlawsLevelActor* L, const TArray<FString>& Ids, const TCHAR* Where);
	/** A toast for Id, earned elsewhere (OutlawsUnlocks::EarnPending writes "story" and "story_ugly" with their unlock pages). */
	void QueueToast(AOutlawsLevelActor* L, const FString& Id);

	// ---- where they are earned (each on the machine whose player did it, into its own profile) ----
	/** The main story's ending is over (OutlawsUnlocks::EarnPending): story, story_bad, story_ugly, story_coop. */
	void NoteStoryFinish(AOutlawsLevelActor* L, int32 Difficulty, bool bCoop);
	/** A named outlaw of a story level is down (OnBossDefeated): boss_first and his own (by the level and his FUNC: Henry at the
	 *  mill is a DrDeath_New too). */
	OUTLAWSRUNTIME_API void NoteBossDown(AOutlawsLevelActor* L, const FString& Level, const FString& Func);
	/** A level is won (CompleteLevel, NextLevelName worked out): true_grit; a Handful mission's last level: its achievement. */
	void NoteLevelComplete(AOutlawsLevelActor* L, float Health, const FString& NextLevel);
	/** A Marshal Training mission ended (HomMissionEnd): Outcome 1 captured, 2 killed; the career's game score. */
	void NoteHomEnd(AOutlawsLevelActor* L, const FString& Mission, int32 Outcome, int32 GameScore);
	/** This machine's player found a secret (UOutlawsStats::NoteSecret): secret; Sector kept for the level, and with every secret
	 *  of every story level found (Total: the level's secret sectors), secrets_all. */
	OUTLAWSRUNTIME_API void NoteSecret(AOutlawsLevelActor* L, const FString& Level, int32 Sector, int32 Total);
	/** A story level's count of secret sectors kept (its first secret, its end), so secrets_all knows what "every" is. */
	OUTLAWSRUNTIME_API void NoteSecretTotal(AOutlawsLevelActor* L, const FString& Level, int32 Total);
	/** This machine's player fired the gatling: gatling. */
	OUTLAWSRUNTIME_API void NoteGatlingFired(AOutlawsLevelActor* L);
	/** One blast of this machine's dynamite killed Kills bad guys: three or more, dynamite3. */
	OUTLAWSRUNTIME_API void NoteBlastKills(AOutlawsLevelActor* L, int32 Kills);
	/** A finish kept as a best time (no cheats, no Randomizer, no multiplier): par, under that level's par time. */
	OUTLAWSRUNTIME_API void NoteKeptTime(AOutlawsLevelActor* L, const FString& LevelKey, int32 Seconds);
	/** A match ended on this machine: bWon this player (or its team) came first; bOnlyBots every other player was a bot. */
	void NoteMatchEnd(AOutlawsLevelActor* L, bool bWon, bool bOnlyBots);
	/** The tutorial's last card done (EndTutorial, not skipped). */
	void NoteTutorialDone(AOutlawsLevelActor* L);

	// ---- REQ-COLLECTBOX's list (OutlawsCollectBox.cpp places them; the shelf shows an outline for each not found) ----
	/** The main story's levels, one box each, in play order, with their names. */
	OUTLAWSRUNTIME_API void BoxLevels(TArray<FString>& OutLevels, TArray<FString>& OutLabels);
	OUTLAWSRUNTIME_API bool BoxFound(const AOutlawsLevelActor* L, const FString& Level);
	/** A box picked up in Level: saved on this profile, the box achievements earned as the count passes first, half and all. */
	OUTLAWSRUNTIME_API void NoteBoxFound(AOutlawsLevelActor* L, const FString& Level);
	/** The boxes are in the levels (REQ-COLLECTBOX, OutlawsCollectBox.cpp): the shelf's note says so. */
	constexpr bool BoxesPlaced = true;
#if WITH_OUTLAWS_TESTS
	/** (-OLCollectBoxTest) every box of this profile forgotten, and the three box achievements with them. */
	OUTLAWSRUNTIME_API void DebugForgetBoxes(AOutlawsLevelActor* L);
#endif

	/** The par time of a main story level (seconds); -1 for any other level. */
	OUTLAWSRUNTIME_API int32 ParSeconds(const FString& Level);

	/** The player's tick: a toast waiting shown once there is somewhere to show it (a menu, or the game being played). */
	void Tick(AOutlawsPlayer* P, AOutlawsLevelActor* L, float Dt);
	/** The toasts shown so far this run (for the tests). */
	OUTLAWSRUNTIME_API const TArray<FString>& ToastsShown();
}
