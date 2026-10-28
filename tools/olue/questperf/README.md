# questperf: Outlaws VR performance captures on the Quest

The headset build runs **OutlawsPerf** (`Source/OutlawsRuntime/OutlawsPerf.*`) all the time. It is the LE/VE
EX_PERF_WATCH + GPU profiler idea, for Unreal on the Quest. It writes into the game's `Saved/Perf/` on the headset:

| file | what |
|---|---|
| `perf_live.txt` | rewritten every 5 s: frame / game / render / RHI / GPU ms, draws, triangles, memory, the scene, and every CPU bucket (last 5 s and whole session), costliest first |
| `perf_timeline.csv` | one row a second: level, player position, timings, the costliest bucket, textures made |
| `performance.txt` | the 10 worst lag moments of each session (smoothed 8-frame time 1.25x over the 72 Hz budget, live play only, 10 s apart) and manual marks, each with position, counts, memory and settings; kept across sessions |
| `hang.txt` | written by a watchdog thread when the game thread stops for 10 s (a GPU hang): the last context and the trail of LogOutlaws / RHI / XR lines before it |

**Mark a lag moment in the headset:** click both sticks in (which recentres) and keep holding for 1.5 s. The controllers buzz and "Perf mark saved" shows.

## Running it

`questperf <command>` below is `python questperf.py <command>` (and `quest_wifi` is `python quest_wifi.py`). adb comes
from the OL_ADB environment variable, else ANDROID_HOME's platform-tools, else adb on the PATH; Unreal Insights (for
`trace`) from UNREAL_INSIGHTS, else the engine folder in UE_ROOT. Only a Quest is used: any other Android device on adb
is left alone.

## A capture session

```
questperf record      # logcat + GPU hardware counters (ovrgpuprofiler) + UE's per-frame CSV (csvprofile start)
  ... play, mark the bad moments ...
questperf stop        # csvprofile stop, pull everything, write report.txt
```

Sessions go to `tools/olue/out/questperf/<date_time>/`. `report.txt` holds:

- **each level's frame, game, render, RHI and GPU ms**, and which thread limits it;
- **the worst seconds**, with the position and the costliest bucket;
- **GPU passes** (the CSV's `GPU/` columns) and **our CPU buckets** (`Outlaws/`), as avg / p95 / max per frame;
- **the GPU hardware counters**: utilisation, bus and memory stalls, texture fetch stalls, vertex vs fragment time;
- **the compositor's view** (delivered fps, stale frames, temperature), plus any **GPU faults** in the log;
- **the lag moments, marks and hang reports**.

Start the game before `record`, or send `questperf cmd "csvprofile start"` once it is running.

## Auto: one level, recorded by itself every time it is played

```
questperf auto                 # armed for the story's 2nd level, The Town (townex); --level N or --map NAME for another
  ... play; each start of that level is a capture, its end stops it ...
questperf auto stop            # from another window (a capture under way is finished first); Ctrl+C works too
questperf compare              # compare.txt again over the newest auto session
```

A capture **starts** when the level's build is done (the game log's `OLRT level load: <map>`; the main menu's own view of
the same map is not a start). It **ends** at the level's end: complete, Restart Level, a load, another level, the main menu,
the game closed, or the app paused (the headset off) for more than a minute. A death and its respawn stay in the same
capture with a marker. A level already under way when `auto` starts is left alone: the next start is captured.

With the Randomizer's mission order shuffled, level 2 is the map the run's seed puts second: `auto` learns it from the
run's lines at level 1's end. If you start from a save of that level instead, arm with `--map <name>`.

Each capture gets its own folder, `out/questperf/auto_<date_time>/capture_<n>_<map>_<time>/`:

| file | what |
|---|---|
| `report.txt` | the headline, then live play only: frame / game / render / RHI / GPU ms (avg, p95, p99), % of frames over the budget, delivered fps and stale frames, GPU utilisation and fragment share, draws, tris, memory, temperature by minute, the worst seconds with where they were, the markers, the settings, then questperf's general report over the capture's window |
| `settings.txt` | at the start: the build, versionCode, the headset and its profile, every Graphics row as saved (OutlawsSettings.ini, read with run-as), what the game applied (resolution, vr.PixelDensity, eye buffer, refresh, lamp lights, HD art, the HUD), the Gameplay rows (Bad guys, the Randomizer and its seed), the CPU and GPU levels, battery and temperature; anything it could not read is listed as NOT READ |
| `settings_change_<n>.txt` | the same, again, when a Graphics row changed during the capture (and a marker) |
| `markers.txt` | start, deaths and respawns, the pause menu, Graphics changes, perf marks, the end and why |
| `capture_<time>.csv`, `gpu_metrics.jsonl`, `logcat.txt`, `game.log` | the game's row a frame, the GPU counters, the compositor's lines, the game's log lines, all for this capture |

`compare.txt` beside them, rewritten after each capture from the second on: every capture side by side, the worst moments
matched by area (what the other runs did within 10 m of the spot), and what is down to the play path rather than the
settings (the route, the bad guys awake, the time in play, deaths). Only live play counts: the menu, a pause, the death
view, movies and loads are left out (the capture's `in_play` column).

`--hz 120` measures against 120 Hz (8.33 ms) whatever the headset does: `settings.txt` has a refresh check (the saved
row, the rate the game asked, the compositor's), and a capture whose compositor ran another rate says REFRESH FELL BACK at
the top of its report. `compare.txt` also names what limits each run (each missed frame by what was over the budget: the
game thread's own work, the render or RHI thread, else the GPU) and what holds the rate back at the lowest settings.

A run of the hidden Performance test row (REQ-PERFTESTMODE) is picked up by the game's own lines (`OLPERF test start:
level <map>, capture <file>, settings <file>`, `OLPERF test death <n>`, `OLPERF test end: <why> (<n> frames)`): its capture
is the game's own (no capture.on), and its settings dump comes back as `game_settings.txt`.

If adb loses the headset (asleep, out of Wi-Fi), that is not taken for the game closing: a capture stops after a minute
without contact, and its files are pulled once the headset answers again.

In Shipping the game's log is a file (`Saved/Logs/OutlawsUE.log`, written a line at a time), not logcat: `auto` follows
that file over adb, and logcat for the compositor. It changes nothing on the headset but `Saved/Perf/capture.on`.

Testing without a headset: `questperf_fake.py build <a real Shipping OutlawsUE.log> feed.json` makes a feed from a real log
plus a scripted part (the menu, a load, a Graphics change, a death, the headset off, Restart Level, the level's end), and
`questperf auto --fake feed.json --out <dir>` plays it a second at a time.

## Adding a bucket

In any per-frame function: `OL_PERF_SCOPE("Player: guns");` (include `OutlawsPerf.h`). The bucket appears in
perf_live.txt, the lag snapshots and the CSV's `Outlaws/` columns. A `(total)` bucket contains the ones below it.

## Hunting the GPU hang

The Adreno driver reports `CP opcode error` and `Can't find a memory entry containing IB1BASE`. The GPU ran a
command buffer whose memory was gone. The game thread then waits for ever on occlusion results ("Timed out while
waiting for GPU"). To bisect, switch one thing off at a time while playing, and see whether the hang stops:

```
questperf cmd "r.AllowOcclusionQueries 0"
questperf cmd "r.Vulkan.SupportsTimestampQueries 0"     (takes effect only at start-up; for reference)
questperf cmd "ol.perf.dump"                             (rewrite perf_live.txt now)
```

`hang.txt` shows what the game was doing in the last frames before each hang.

## GPU timing

UE 5.1 leaves Vulkan timestamp queries off on Android. `Config/DefaultDeviceProfiles.ini` turns them on for the
Quest profile, because that cvar ignores ordinary ini settings. Without them, every GPU column reads 0.

## Untethered (adb over Wi-Fi)

With the cable in once per headset boot:

```
quest_wifi setup      # finds the headset's Wi-Fi address, switches adb to network mode, connects
```

Unplug and play. `questperf` finds the headset over Wi-Fi by itself, and reconnects to the saved address if the
link dropped. After a headset reboot, run `setup` with the cable again. `quest_wifi status` lists what adb sees.
With both USB and Wi-Fi up, `questperf` uses USB.

## Shipping (release) builds

Shipping has no console or csvprofile. `record` makes Saved/Perf/capture.on, and the game writes a row a frame to Saved/Perf/capture_<date_time>.csv until `stop` removes it. `questperf capture on|off` does the capture alone. The game log carries an "OLPERF summary" line every 60 s and at each level's end. hang.txt is Development only. `mark` and `cmd` need a Development build; in Shipping, mark by holding both sticks in for 1.5 s.
