#pragma once

#include "CoreMinimal.h"

/**
 * The headset's display refresh rate through OpenXR XR_FB_display_refresh_rate (Quest). Nothing here asks for a rate
 * by itself: the game (OutlawsDeviceProfile) calls Request when the player or the Quest 2 profile picks one.
 */
namespace OutlawsXRRefresh
{
	/** The extension is enabled and a session is up (false on a desktop run, or a runtime without it). */
	OUTLAWSXR_API bool IsAvailable();
	/** The rates the headset offers, lowest first (empty when not available). */
	OUTLAWSXR_API TArray<float> SupportedRates();
	/** The rate the display runs at now (0 when not available). */
	OUTLAWSXR_API float CurrentRate();
	/** The rate the session started at, before any request of ours (0 when not available). */
	OUTLAWSXR_API float InitialRate();
	/** Ask for a rate (kept and asked for when a session starts, also when called before it). Only a rate the headset
	 *  offers is asked for. False when not available, not offered or refused. Logs "OLRT refresh: requested, actual". */
	OUTLAWSXR_API bool Request(float Hz);
	/** Called every frame or so (user 2026-10-03, BUG-REFRESH: asked 90, the display stayed at 72): while the display is
	 *  not at the rate asked for, ask again every 3 s, ten times at most per request. */
	OUTLAWSXR_API void Maintain(double Now);
}

/**
 * Q2PROFILE (tester dotard, Quest 2, 10 Oct): the CPU and GPU clock levels through OpenXR XR_EXT_performance_settings.
 * Nothing is asked unless the game asks (OutlawsDeviceProfile: SUSTAINED_HIGH under the Quest 2 profile; elsewhere the
 * runtime's own dynamic levels). The level asked for is kept and asked again when a session starts and whenever the
 * session gets the focus back (an app resume), and Reapply asks again at once. The runtime's perf notifications (CPU or
 * GPU, compositing, rendering or thermal, normal / warning / impaired) are logged as "OLRT xr perf: runtime ...".
 */
namespace OutlawsXRPerf
{
	/** XR_EXT_performance_settings is enabled and a session is up. */
	OUTLAWSXR_API bool IsAvailable();
	/** Ask for a level for both the CPU and the GPU: 0 none (the runtime's own), 1 power savings, 2 sustained low,
	 *  3 sustained high, 4 boost. Kept, and asked when a session starts if there is none yet. Logs the result. */
	OUTLAWSXR_API bool Request(int32 Level, const TCHAR* Why);
	/** Ask for the kept level again (an app resume). */
	OUTLAWSXR_API bool Reapply(const TCHAR* Why);
	/** The level kept (0..4). */
	OUTLAWSXR_API int32 Wanted();
	/** One line: "cpu/gpu sustained high asked (result 0), runtime cpu thermal normal, ..." */
	OUTLAWSXR_API FString StatusText();
	/** The level's name ("sustained high"). */
	OUTLAWSXR_API const TCHAR* LevelName(int32 Level);
}

namespace OutlawsXRRefresh
{
	/** Ask for the rate kept again at once (an app resume); false when none is kept or there is no session. */
	OUTLAWSXR_API bool Reapply(const TCHAR* Why);
	/** The rate kept (0 = none asked). */
	OUTLAWSXR_API float Wanted();
}
