#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformTime.h"

/**
 * Performance diagnostics, for the headset first (the LE/VE EX_PERF_WATCH + GPU profiler idea):
 *  - every frame: frame, game, render, RHI and GPU ms, draw calls, triangles, memory, and our own CPU buckets
 *    (OL_PERF_SCOPE("Enemies") in the hot paths);
 *  - Saved/Perf/perf_live.txt, rewritten every 5 s: averages, peaks and the buckets, costliest first;
 *  - Saved/Perf/perf_timeline.csv: one row a second (level, player position, timings, counts);
 *  - Saved/Perf/performance.txt: the 10 worst lag moments of the session (smoothed frame time 1.25x over the
 *    headset's budget) and manual marks (both sticks held in 1.5 s, or `ol.perf.mark`), kept across sessions;
 *  - Saved/Perf/hang.txt: a watchdog thread notices the game thread stopping (a GPU hang leaves it spinning on
 *    occlusion results) and writes the last LogOutlaws lines, texture uploads and the last good context.
 * tools/olue/questperf pulls and reads them.  Console: ol.perf.mark, ol.perf.dump, ol.perf.Capture.
 * PERF-SHIPSUMMARY: all of it but hang.txt in Shipping too (no watchdog thread there), in the Saved a player sends in
 * (OLShipLog::ReportSavedDir: the external one on the Quest), and in every build:
 *  - a log line every 60 s and at each level's end ("OLPERF summary, ..."): avg, p95 and worst frame, the threads, the
 *    resolution, hitches over 30 ms (live play only);
 *  - the detailed capture, a row a frame into Saved/Perf/capture_<date_time>.csv while Saved/Perf/capture.on or
 *    Saved/capture.on exists (polled once a second; questperf record / stop; BUG-PERFFLAG: the Quest's Saved is writable for
 *    adb's shell, and Saved/Perf is made group-writable there too), with -OLPerfCapture (the whole run), or ol.perf.Capture 1.
 *    -OLPerfFlagProbe checks both flag files on the desktop.
 * -OLNoPerfFiles: no files, the summary lines only (to measure the writer's cost against).
 */
namespace OutlawsPerf
{
	/** A named CPU bucket (game thread), registered once per call site by OL_PERF_SCOPE. */
	OUTLAWSRUNTIME_API int32 Bucket(const TCHAR* Name);
	OUTLAWSRUNTIME_API void Add(int32 BucketIndex, uint64 Cycles);
	/** A bucket's last whole frame, ms (tests: the same-run A/B of the enemy driver). */
	OUTLAWSRUNTIME_API double LastFrameMs(const TCHAR* Name);
	/** A line for the hang report's trail (any thread). LogOutlaws lines are added on their own. */
	OUTLAWSRUNTIME_API void Breadcrumb(const FString& What);
	/** "Lag right now": a snapshot into performance.txt's manual marks. */
	OUTLAWSRUNTIME_API void Mark(const TCHAR* Why);
	/** While one is alive the hang watchdog stands down (a level build holds the game thread 5-12 s on the Quest). */
	/** A level build is running (an FLoading is alive). */
	OUTLAWSRUNTIME_API bool IsLoading();
	struct OUTLAWSRUNTIME_API FLoading
	{
		FLoading();
		~FLoading();
	};
	/** GPU-hang hunt: ol.debug.FreezeMeshUpdates 1 stops the per-frame vertex rewrites of already-drawn meshes
	 *  (scrolling surfaces, flying glass, spreading blood pools), which Vulkan does in place on static buffers. */
	OUTLAWSRUNTIME_API bool FreezeMeshUpdates();
	/** CPU1: what could be drawn now, by source (the world's sections, sprites, movers, ...): each shown component's sections
	 *  (a procedural mesh) or materials (a static mesh), with the last frame's draw calls as the RHI counted them. */
	OUTLAWSRUNTIME_API FString DrawCensus(UWorld* World);
	/** Counted per frame for the timeline: runtime textures made (with their texels). */
	OUTLAWSRUNTIME_API void CountTexture(int32 W, int32 H);
	/** The last whole frame's world start (OnWorldTickStart to the actor ticks), ms: on the headset the XR runtime's
	 *  xrWaitFrame (its pacing, counted in GGameThreadTime), with the net tick; the throttle takes it off the game thread. */
	OUTLAWSRUNTIME_API double XRWaitMs();
	/** REQ-PERFTESTMODE (OutlawsPerfTest.cpp): a performance test run's own detailed capture, a fresh capture_<date_time>.csv
	 *  started at once (one already running is closed first) and kept on until EndTestCapture whatever the other switches
	 *  say; its full path (empty: -OLNoPerfFiles).  EndTestCapture stops it and gives its frames (its rows); capture.on, the
	 *  console variable or -OLPerfCapture may start another at the next second's poll. */
	OUTLAWSRUNTIME_API FString StartTestCapture();
	OUTLAWSRUNTIME_API int64 EndTestCapture();

	void Startup();
	void Shutdown();

	struct FScope
	{
		int32 B;
		uint64 T0;
		explicit FScope(int32 InB) : B(InB), T0(FPlatformTime::Cycles64()) {}
		~FScope() { Add(B, FPlatformTime::Cycles64() - T0); }
	};
}

#define OL_PERF_SCOPE(Name) \
	static const int32 PREPROCESSOR_JOIN(OlPerfBucket_, __LINE__) = OutlawsPerf::Bucket(TEXT(Name)); \
	OutlawsPerf::FScope PREPROCESSOR_JOIN(OlPerfScope_, __LINE__)(PREPROCESSOR_JOIN(OlPerfBucket_, __LINE__))
