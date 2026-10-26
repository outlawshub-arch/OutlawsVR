#pragma once

#include "CoreMinimal.h"

/**
 * Q2 step 3: the render resolution follows the GPU (tester dotard's Quest 2, 10 Oct: a frame over 13.9 ms drops the
 * headset to every other vsync, 27.8 ms, 36 fps; the train sat there for 5 minutes).
 *
 * While on, the controller owns vr.PixelDensity between a floor (ol.dynres.Min, 0.65) and the ceiling the Graphics page
 * gives (the Resolution row's choice, else the profile's: 0.85 on the Quest 2; the headset's own 1.0 elsewhere).
 *
 * QSPEED-4 (19 Oct): what it reads.  RHIGetGPUFrameCycles on Vulkan spans the frame's first to last GPU command, waits for
 * the CPU included, so on the Quest it read about 13.9 ms every frame and every v0.9 session sat at the floor while the
 * GPU's real work was about 9.4 ms.  Now a pair of GPU timestamps (RQT_AbsoluteTime, the engine's own dynamic resolution
 * method) brackets the game view's render each frame (a view extension, viewport families only, never scene captures):
 * the GPU's work for the scene, without the wait before it.  Results land a frame or two later and are read without
 * waiting.  Where timestamps are missing or stop landing for a second it falls back to the frame cycles (logged), with a
 * stricter down line (1.05 of the frame: only frames really missed).  ol.dynres.FakeGpuMs / FakeCpuMs stand in on tests.
 *
 * The rules, smoothed over about a quarter second, against the refresh rate's frame (ol.perf.TargetFps):
 *  - down only when the GPU stays over ol.dynres.DownAt (0.90) of the frame for 0.5 s AND the GPU is the long pole: its
 *    time at least the busier CPU thread's (the game thread less the XR runtime's xrWaitFrame, or the render thread) times
 *    ol.dynres.GpuOverCpu (1.0).  When a CPU thread is longer, a smaller picture cannot help and it holds;
 *    at most once a second, to the scale that would bring it to 80 % of the frame (the GPU's ms go with the pixels, the
 *    square of the scale), one to three 0.05 steps;
 *  - up one step when the GPU stays under ol.dynres.UpAt (0.70) for 3 s, at most every 3 s, and only when the step's cost
 *    (the pixels it adds) still leaves it under 85 % of the frame; a down soon after an up doubles the wait (to 30 s max).
 * A frame over 250 ms (a load, a hitch) and the 2 s after an app resume are not read; a single frame's reading counts at
 * most 4 frames' worth.  Each step is logged "OLRT dynres: down|up a -> b (...)"; "OLRT dynres status: ..." every
 * ol.dynres.LogEvery seconds (1 in Development, 10 in Shipping) with the source, GPU, CPU pole and what it decided;
 * per frame at Verbose (Development).
 *
 * ol.dynres.Enable: 0 off (the default on every platform since BUG-DYNRESOFF, 20 Oct: on the Quest 3 the timestamps
 * stopped landing after about 4 s, the frame-cycles fallback slid the scale to the floor and it never came back; the
 * resolution now stays at the Resolution row's value), 1 on, -1 by the platform (on with a headset on Android).
 * No timestamps are issued while it is off.
 * -OLDynResTest: the desktop test (fake loads stepped up and down; "OLSHOT dynres: PASS|FAIL", then quit).
 * -OLDynResUnitTest: the decision rules fed synthetic timings (no level; "OLSHOT dynres unit: PASS|FAIL", then quit).
 * -OLDynResOffTest: off by default: vr.PixelDensity holds the Resolution row for 60 s and follows a press at once
 * ("OLSHOT dynresoff: PASS|FAIL", then quit).
 */
namespace OutlawsDynRes
{
	void Startup();
	void Shutdown();

	/** On (the cvar, else the platform). */
	OUTLAWSRUNTIME_API bool WantsControl();
	/** OutlawsDeviceProfile::Apply: the value vr.PixelDensity takes under this ceiling: the ceiling itself while off; while
	 *  on, the controller's scale (back at the ceiling when the ceiling changed: a new choice; kept on a resume). */
	OUTLAWSRUNTIME_API float Claim(float Ceiling, const TCHAR* Why);

	OUTLAWSRUNTIME_API bool IsActive();
	OUTLAWSRUNTIME_API float Scale();
	OUTLAWSRUNTIME_API float Floor();
	OUTLAWSRUNTIME_API float Ceiling();
	/** "dynamic 0.80 (0.65..0.85)" or "dynamic off". */
	OUTLAWSRUNTIME_API FString ShortText();
	/** The GPU ms to show (the Performance HUD): the timestamped scene work while on and landing, else the RHI frame cycles. */
	OUTLAWSRUNTIME_API double GpuMs();
}
