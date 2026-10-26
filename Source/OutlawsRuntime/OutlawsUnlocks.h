// REQ-UNLOCKS (user 2026-10-13, v1.0): achievements and the modes they unlock.
//
// The store: Saved/OutlawsSaves/achievements.json, on this machine only (never a server), one record per player profile (the
// player name from Options > Multiplayer; no name is the "(no name)" profile).  Each record says which achievements are
// earned, when, on which difficulty and whether in co-op, and which unlock pages are still to be shown:
//
//   { "version": 1, "unlockAllModes": false,
//     "profiles": { "Marshal": { "earned": { "story": { "when": "2026-10-13 16:02:11", "difficulty": "Good", "coop": false } },
//                               "unseen": [ "story" ] } } }
//
// REQ-LEVELTIMER adds one field to a profile, "times": level key -> difficulty name -> the best finish, whole seconds:
//
//   "Marshal": { "earned": {...}, "unseen": [...],
//                "times": { "hideout": { "Good": { "seconds": 412, "when": "2026-10-13 17:02:11", "coop": false } },
//                           "hom:granary": { "Bad": { "seconds": 95, "when": "...", "coop": false } } } }
//
// The version stays 1: a file without "times" reads as no best times, and an older build leaves the field alone.
//
// A missing file is a fresh start (everything locked).  A file that cannot be read as this is "damaged": everything starts
// locked, nothing crashes, and the next write keeps it beside the new one as achievements.json.damaged.
//
// The rules: the main story finished on any difficulty earns "story" (Il Buono); finished on Ugly also earns "story_ugly" (Il
// Cattivo).  Co-op: every player in the session when the final cutscene ends earns it on their own profile (their own machine's
// store).  Historical Missions never count.  UNLOCKFREE (user decision for 0.95): the Randomizer and the Bad guys multiplier no
// longer wait for these; both are open to every profile from the very first start (off by default, as their settings start), so
// the achievements unlock nothing now and no unlock page is queued.  Older files load as before: an "unseen" list there is left
// alone and never shown, and "unlockAllModes" (the old Cheats row, gone with the locks) is kept but changes nothing.  Test
// switches (-OLRando, -OLSoloBadGuys, -OLCoopBadGuys) are taken as before.
#pragma once

#include "CoreMinimal.h"

class AOutlawsLevelActor;

namespace OutlawsUnlocks
{
	enum class EMode : uint8 { Randomizer, BadGuys };

	/** The achievements' ids in the store. */
	extern const TCHAR* const StoryAny;        // "story": the main story finished on any difficulty
	extern const TCHAR* const StoryUgly;       // "story_ugly": the main story finished on Ugly

	FString StorePath();
	/** The profile this machine's player uses (the player name; "(no name)" when there is none). */
	FString ProfileOf(const AOutlawsLevelActor* L);
	bool IsEarned(const AOutlawsLevelActor* L, const TCHAR* Id);
	/** UNLOCKFREE: always true now (both modes are open from the start); kept so the modes' callers read the same. */
	bool IsUnlocked(const AOutlawsLevelActor* L, EMode Mode);
	/** The store's old unlockAllModes (or -OLUnlockAll for a run): read and written as before, nothing depends on it now. */
	bool UnlockAll();
	void SetUnlockAll(bool bOn);
	/** The store file could not be read as a store (it was started locked). */
	bool StoreDamaged();
	/** The level was won (CompleteLevel): when it is the main story's last, what the end of its cutscene earns is noted on L. */
	void NoteLevelWon(AOutlawsLevelActor* L);
	/** The final cutscene and the credits are over on this machine (or its co-op session ended after them): the noted finish
	 *  earned on this profile, each new unlock's page queued.  Nothing noted: nothing happens. */
	void EarnPending(AOutlawsLevelActor* L, const TCHAR* Where);
	/** The first unlock page not yet shown for this profile: its id, its title line and where to find it ('|' between lines).
	 *  UNLOCKFREE: none any more (nothing unlocks), so this is false; an old file's queued pages are never shown. */
	bool NextNotice(const AOutlawsLevelActor* L, FString& OutId, FString& OutTitle, FString& OutWhere);
	/** That page was shown and carried on from: never again. */
	void MarkSeen(const AOutlawsLevelActor* L, const FString& Id);
	/** REQ-LEVELTIMER: the best time (whole seconds) stored on this profile for a level on a difficulty (false: none yet).  The level
	 *  is the stats' level key (the level's name; "hom:<mission>" for a Historical Mission). */
	bool BestTime(const AOutlawsLevelActor* L, const FString& LevelKey, int32 Difficulty, int32& OutSeconds);
	/** A finish of Seconds: stored when it is the first or beats the best (strictly faster).  True: a new best; OutPrior is the best
	 *  it beat (-1: there was none). */
	bool NoteBestTime(const AOutlawsLevelActor* L, const FString& LevelKey, int32 Difficulty, int32 Seconds, bool bCoop, int32& OutPrior);
	/** REQ-EXTRAS: this profile's record as read (nullptr: none yet), for the Extras pages' reads (OutlawsAchievements.cpp). */
	TSharedPtr<class FJsonObject> ProfileRead(const AOutlawsLevelActor* L);
	/** REQ-EXTRAS: a change to this profile's record (made when missing), written at once the same safe way as every other. */
	void ProfileChange(const AOutlawsLevelActor* L, TFunctionRef<void(const TSharedPtr<class FJsonObject>&)> Change);
	/** -OLUnlockTest=<fresh|good|ugly|toggle|damaged> (OutlawsUnlocksTest.cpp), from the level's tick. */
	void TickTest(AOutlawsLevelActor* L, float Dt);
}
