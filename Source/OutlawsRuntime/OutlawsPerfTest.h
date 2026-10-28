#pragma once

// REQ-PERFTESTMODE (user 28 Oct): New Game > Performance test, for perf runs.  The row is hidden until a whole turn on the
// spot with New Game's page up (the pause menu's Cheats rule and its detector, AOutlawsPlayer::UpdateMenu), single player
// only.  It plays the story's second level alone (the run's second, RandoLevelOrder, with the mission order shuffled), as a
// story start there at Bad: what a finish of the first level carries in (James's start kit and the guns and ammunition that
// level places, after the Randomizer's roll of it), full health and stamina, the Bad guys multiplier, the Randomizer and its
// seed and the graphics as a New Game takes them.  When play begins the game's own per-frame capture goes on (OutlawsPerf,
// as Saved/Perf/capture.on does) with a settings dump beside it, Saved/Perf/perftest_<the capture's stamp>_settings.txt,
// one "key: value" line each.  A death and its respawn stay in the run.  The level's exit (the score card first) or End
// Game from the pause menu ends it and goes back to the main menu, no level 3; so does a game closed.  Nothing is written
// from the start to the main menu again: no save or autosave, achievement, best time, collectible, tally or unlock.
// The log lines tools/olue/questperf keys on:
//   OLPERF test start: level <map>, capture <full path of the capture csv>, settings <full path of the settings txt>
//   OLPERF test death <n>
//   OLPERF test end: <level exit | quit from the pause menu | game closed> (<frames> frames)
// The level actor's side (the start, the carry, the dump) is in OutlawsPerfTest.cpp; -OLPerfTestProbe in
// OutlawsPerfTestProbe.cpp.

#include "CoreMinimal.h"

namespace OutlawsPerfTest
{
	/** A run is on, from its start to the main menu again: nothing is written while it is. */
	OUTLAWSRUNTIME_API bool IsOn();
	/** In the run's level with its capture on (its start line written, its end not yet). */
	OUTLAWSRUNTIME_API bool IsRunning();
	/** The run's level ends for this reason (one of the three above): the capture off, the end line; nothing when not running. */
	OUTLAWSRUNTIME_API void End(const TCHAR* Why);
	/** A death in the run: its marker line. */
	OUTLAWSRUNTIME_API void NoteDeath();

	/** The level actor's side (OutlawsPerfTest.cpp). */
	void Begin(const FString& Level, const TMap<FString, int32>& Carry);
	void Started(const FString& CapturePath, const FString& SettingsPath);
	void Finished();
	bool IsStarting();

	/** What the run is and did (the probe's checks). */
	OUTLAWSRUNTIME_API const FString& Level();
	OUTLAWSRUNTIME_API const TMap<FString, int32>& Carry();
	OUTLAWSRUNTIME_API const FString& CapturePath();
	OUTLAWSRUNTIME_API const FString& SettingsPath();
	OUTLAWSRUNTIME_API int32 Deaths();
	OUTLAWSRUNTIME_API const FString& LastEndWhy();
	OUTLAWSRUNTIME_API int64 LastEndFrames();
}
