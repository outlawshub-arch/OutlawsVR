#pragma once

// REQ-PERFTESTMODE (user 28 Oct): New Game > Performance test, for perf runs.  The row is hidden until a whole turn on the
// spot with New Game's page up (the pause menu's Cheats rule and its detector, AOutlawsPlayer::UpdateMenu), single player
// only.  It plays the story's second level alone (the run's second, RandoLevelOrder, with the mission order shuffled), as a
// story start there at the difficulty last pressed on New Game this session, else Bad (user 28 Oct, via the planner): what a
// finish of the first level carries in (James's start kit and the guns and ammunition that level places, after the
// Randomizer's roll of it), full health and stamina, the Bad guys multiplier, the Randomizer and its seed and the graphics as
// a New Game takes them.  When play begins the game's own per-frame capture goes on (OutlawsPerf,
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
//
// REQ-PERFSWEEP (user 28 Oct, via the planner: which Graphics rows cost the most; all top against all lowest gave only the
// total): New Game > Settings sweep, beside Performance test (the same turn shows both).  The same start as a run (the carry,
// the difficulty, the seed and the Bad guys multiplier, the capture on, nothing written) but always in The Town (townex),
// god mode for its length, the bad guys as ever.  The player is put at a busy spot from the user's run 1 facing a glowing
// marker, a card asks them to stand still; then every Graphics row at its top (the refresh at the highest the headset
// offers) and each row in turn, A (all top) B (that row at its lowest) A B, 15 s a phase, the first 3 s after a switch not
// measured: Resolution 120 / 70 (then a phase each at 100 and 85, B-tagged, in no pair), Lamp Lights 4 / 0, Remastered
// Graphics, Object Detail Far / Near, Stochastic Textures, Tinting, Quest 2 Mode off / on (the other rows held at the top: what
// it adds by itself), and last All top / lowest.  Every row changes live (no reload); a row that cannot change is skipped and
// said so.  A small card shows the phase ("4 of 32: Resolution at lowest").  Each phase's game work, render, RHI and GPU ms,
// fps, frames over budget, bad guys awake and top buckets (OutlawsPerf::BeginWindow) go to the log and to
// Saved/Perf/sweep_<the capture's stamp>.txt: each row's saving ranked, with its two pairs' spread, the all pair against the
// rows' sum, and every phase.  The settings file is held for the sweep (AOutlawsLevelActor::WriteSettings) and every row,
// the refresh, Quest 2 Mode and the level's switches are put back when it ends or is quit; at its end a top-3 card, then the
// main menu.  The log lines (questperf adds the GPU counters per phase):
//   OLPERF sweep start: level <map>, phases <n>, spot (<x>, <y>, <z>), sweep <full path of sweep_<stamp>.txt>, capture <full path of the capture csv>
//   OLPERF sweep skipped: <row> (<why>)
//   OLPERF sweep phase <i> of <n>: <row> <value> (<A|B>)
//   OLPERF sweep phase <i> measured: <frames> frames, game work <ms>, render <ms>, RHI <ms>, GPU <ms>, fps <f>, over <pct> %, awake <n>
//   OLPERF sweep end: <done | quit from the pause menu | game closed> (<phases done> of <n>), sweep <full path>
// OutlawsPerfSweep.cpp; -OLPerfSweepProbe in OutlawsPerfSweepProbe.cpp.

#include "CoreMinimal.h"
#include "OutlawsPerf.h"

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
	/** The run's difficulty: the one last pressed on a New Game difficulty page (Main Game, Marshal Training or a mission)
	 *  this session, remembered until the game closes (never written); Bad when none was.  The menu notes each press. */
	OUTLAWSRUNTIME_API void NoteNewGameDifficulty(int32 Difficulty);
	/** The difficulty last pressed on New Game (0 Good, 1 Bad, 2 Ugly), -1 none yet. */
	OUTLAWSRUNTIME_API int32 NewGameDifficulty();
	/** REQ-PERFDIFF: the next run's difficulty was picked on its own difficulty page (the rows open it); kept until the run is over. */
	OUTLAWSRUNTIME_API void NotePicked();
	/** Where the run's difficulty came from, for the dumps: "picked", or the last New Game choice, or none (Bad). */
	OUTLAWSRUNTIME_API FString DifficultyFrom();

	/** REQ-PERFCARD (user 28 Oct): a performance test's results, from its capture's live play frames (in_play 1, as questperf's
	 *  report reads them): the frame rate from the frames' own times against the refresh, the share on time (within 5 % of the
	 *  refresh's budget), the worst 1 % (the mean of the slowest 1 % of frames, as fps), the worst frame, the game's own work
	 *  (its time less the wait for the headset's frame), render and GPU ms a frame and the largest of the three, stutters
	 *  (frames over 30 ms). */
	struct FCardNumbers
	{
		int32 Frames = 0;
		double Hz = 0.0, Fps = 0.0, OnTimePct = 0.0, LowFps = 0.0, WorstMs = 0.0, GameMs = 0.0, RenderMs = 0.0, GpuMs = 0.0;
		int32 Stutters = 0;
		FString Limit;               // "the game", "rendering" or "the GPU"
	};
	OUTLAWSRUNTIME_API bool CardNumbers(const FString& Capture, double Hz, FCardNumbers& Out);
	/** The card's rows for those numbers and the run's settings dump ("key: value" lines). */
	OUTLAWSRUNTIME_API TArray<TPair<FString, FString>> CardRowsFor(const FCardNumbers& N, const TMap<FString, FString>& Settings);
	/** The ended run's card (made when a performance test's level ends, a sweep never; kept until the main menu): its level and
	 *  rows, empty when there is none; the numbers it shows. */
	OUTLAWSRUNTIME_API const FString& CardLevel();
	OUTLAWSRUNTIME_API const TArray<TPair<FString, FString>>& CardRows();
	OUTLAWSRUNTIME_API const FCardNumbers& CardShown();
	/** The run was quit from the pause menu, which shows no counts: the card is shown alone on the black (UOutlawsStats). */
	OUTLAWSRUNTIME_API bool CardAlone();
	OUTLAWSRUNTIME_API void SetCardAlone(bool bAlone);

	/** The level actor's side (OutlawsPerfTest.cpp). */
	void Begin(const FString& Level, const TMap<FString, int32>& Carry, bool bSweep = false);
	void Started(const FString& CapturePath, const FString& SettingsPath);
	void Finished();
	bool IsStarting();

	/** REQ-PERFSWEEP: the run is a Settings sweep (from its start to the main menu again): the settings file is held too. */
	OUTLAWSRUNTIME_API bool IsSweep();
	/** A sweep phase's Graphics rows: the top, or one (or every) row lower. */
	struct FSweepSet
	{
		int32 Resolution = 120;
		int32 Lamps = 4;
		bool bRemastered = true;
		int32 Detail = 2;            // (0 Near, 1 Normal, 2 Far)
		bool bStochastic = true;
		bool bTinting = true;
		bool bQuest2 = false;
	};
	struct FSweepPhase
	{
		FString Row, Value;          // as the log line names them ("Lamp Lights", "0")
		bool bB = false;             // B: the row lower (A: every row at its top)
		bool bStep = false;          // a Resolution step after its pairs (100, 85): B-tagged, in no pair
		FSweepSet Set;
		bool bMeasured = false;
		OutlawsPerf::FWindow M;
		double Awake = 0.0;          // bad guys awake: the mean of one count a second
	};
	struct FSweep
	{
		TArray<FSweepPhase> Phases;
		int32 Now = 0;               // the phase now, from 1 (0: the lead-in)
		int32 Done = 0;              // phases measured
		TArray<FString> Skipped;     // "<row> (<why>)"
		FString Path;
		FVector Spot = FVector::ZeroVector;
		FVector Marker = FVector::ZeroVector;    // (zero: none could be made)
		float Yaw = 0.f;
		double PhaseSec = 15.0, SettleSec = 3.0;
		TArray<FString> Top3;        // the end card's lines
	};
	/** The sweep as it stands (the probe's checks). */
	OUTLAWSRUNTIME_API const FSweep& Sweep();
	/** OutlawsPerfSweep.cpp: End's sweep part (the rows put back, the table written) and the main menu's (god mode back). */
	void SweepFinish(const TCHAR* Why);
	void SweepOver();

	/** What the run is and did (the probe's checks). */
	OUTLAWSRUNTIME_API const FString& Level();
	OUTLAWSRUNTIME_API const TMap<FString, int32>& Carry();
	OUTLAWSRUNTIME_API const FString& CapturePath();
	OUTLAWSRUNTIME_API const FString& SettingsPath();
	OUTLAWSRUNTIME_API int32 Deaths();
	OUTLAWSRUNTIME_API const FString& LastEndWhy();
	OUTLAWSRUNTIME_API int64 LastEndFrames();
}
