#pragma once

#include "CoreMinimal.h"

/**
 * Which headset this is, and the Graphics page's performance rows (user 2026-10-03).
 *
 * The Quest 2 profile: on a Meta Quest 2 (ro.hardware "hollywood", else Build.MODEL; see ClassifyHeadset. UE 5.1's
 * device-profile matching is not trusted, as the Quest 3 is newer than 5.1) the rows the player has not chosen take
 * lighter defaults: 85 % render resolution, 1 lamp light, HD textures off, object detail x 0.8, 72 Hz, the CPU and GPU
 * at sustained high; the enemy throttle takes its Quest 2 caps and one remote lantern lights the room. Every other
 * headset (Quest 3, 3S, Pro, unknown) and the PC keep today's values, and nothing is touched there until the player
 * changes a row.
 * Overrides for tests: -OLQuest2Profile / -OLNoQuest2Profile or -OLDeviceProfile=Quest2|Quest3|PC on the command line,
 * or the console variable ol.Quest2Profile (-1 by the headset, 0 off, 1 on), which applies at once.
 *
 * The rows (OutlawsSettings.ini; "auto" or absent = the headset's default):
 *  - Resolution (RenderScale, percent): vr.PixelDensity, the OpenXR eye buffer's size against the runtime's recommended
 *    size (it re-creates the swapchain; live). 70..120 %.  (Q2 step 3) With dynamic resolution on (every Quest) this is
 *    the ceiling: OutlawsDynRes owns the value below it and lowers it while the GPU is over its frame (OutlawsDynRes.h).
 *  - Refresh Rate (RefreshRate, Hz): XR_FB_display_refresh_rate through the OutlawsXR module (live); the rates the
 *    headset offers. ol.perf.TargetFps follows it.
 *  - Lamp Lights (LampLights, 0..4): how many lit lamps light the room (the level's lamp slots; live).
 *  - Performance HUD (PerfHud): frame rate and thread timings above the view (live).
 *  - Object Detail (ObjectDetail, 0 Near, 1 Normal, 2 Far): how far 3D items and props stay 3D before their sprite
 *    takes over, and how far before that fades out (CPU1 object LOD; live).
 *  - Quest 2 Mode (Quest2Mode, QSPEED-5, user 2026-10-20): the Quest 2 profile on any other headset (or the PC), to see
 *    the Quest 2's look and speed on a Quest 3; off by default, live, not shown on a Quest 2 itself. The command line
 *    and ol.Quest2Profile still win over it.
 */
namespace OutlawsDeviceProfile
{
	/** Detect the headset and log "OLRT headset: <model>, profile <Quest2|default>" (module startup). */
	void Startup();
	void Shutdown();

	/** The Quest 2 profile is on (detected or forced). */
	OUTLAWSRUNTIME_API bool IsQuest2();
	/** The device's model as the platform reports it ("Quest 2", "Quest 3", "PC"). */
	OUTLAWSRUNTIME_API const FString& Model();

	/** HD remaster textures by default (when the player has not chosen): false under the Quest 2 profile. */
	OUTLAWSRUNTIME_API bool DefaultHDTextures();

	/** The rows' values in effect (the player's choice, else the headset's default). */
	OUTLAWSRUNTIME_API int32 ResolutionPercent();
	OUTLAWSRUNTIME_API int32 LampLights();
	OUTLAWSRUNTIME_API bool PerfHud();
	/** REQ-HALOOPT (user 2026-10-04): the white glow round what a grip would take, and the ledge and door glow; Off draws none (the grabs are unchanged). */
	OUTLAWSRUNTIME_API bool GrabGlow();
	/** CPU1 object LOD (user 2026-10-04): Object Detail Near / Normal / Far scales every LOD distance (0.6, 1, 1.6), and the
	 *  Quest 2 profile shortens them (x 0.8). */
	OUTLAWSRUNTIME_API float ObjectDetailScale();
	/** REQ-SPECCAM (user 2026-10-13): what the PC window shows while the headset plays, 0 Headset (the mirror, the default)
	 *  or 1 Steady (a smoothed, level first-person view: AOutlawsPlayer::UpdateSpecCam).  Settings key PcWindowView
	 *  ("steady"; absent = Headset).  Always 0 on the Quest. */
	constexpr int32 PcWindowViewCount = 2;
	OUTLAWSRUNTIME_API int32 PcWindowView();
	OUTLAWSRUNTIME_API void SetPcWindowView(int32 View);
	OUTLAWSRUNTIME_API const TCHAR* PcWindowViewName(int32 View);
	/** The refresh rate asked for (0 = the headset's own, nothing asked). */
	OUTLAWSRUNTIME_API float RefreshRate();
	/** The rates offered (empty: no Refresh Rate row). -OLRefreshRates=60,72,90 stands in on a desktop test. */
	OUTLAWSRUNTIME_API TArray<float> RefreshRates();

	/** QSPEED-5: the Graphics page's Quest 2 Mode row is shown (every headset but a Quest 2, and the PC); its setting. */
	OUTLAWSRUNTIME_API bool ShowQuest2ModeRow();
	OUTLAWSRUNTIME_API bool Quest2Mode();

	/** The menu: is this one of our rows, its value as shown, a press (next value; returns a one-line hint). */
	OUTLAWSRUNTIME_API bool IsRow(const FString& Name);
	OUTLAWSRUNTIME_API FString RowValue(const FString& Name);
	OUTLAWSRUNTIME_API FString PressRow(const FString& Name);
	/** REQ-GAMMA: the Graphics page's Brightness slider (settings key Gamma), the picture's gamma: out = in ^ (1 / Gamma) on
	 *  linear colour, 0.7 up to 1.6 in tenths, 1.0 (the default) the look as made.  Applied in the passes the game already
	 *  runs: on the PC the camera's colour grade (ColorGamma, in the tonemapper's LUT), on the headset the materials' own
	 *  post process (OLQuestLook, MPC_OLR_Lights QHurt.w = Gamma - 1; ue_olue_quest_look.py).  Live; Restore defaults
	 *  puts back 1.0. */
	OUTLAWSRUNTIME_API float Gamma();
	OUTLAWSRUNTIME_API float* GammaSetting();
	OUTLAWSRUNTIME_API void SetGamma(float G);
	OUTLAWSRUNTIME_API FString PressGamma();
	OUTLAWSRUNTIME_API FString GammaText();
	/** Restore defaults on the Graphics page: every row back to the headset's default. */
	OUTLAWSRUNTIME_API void RestoreDefaults();

	/** The settings file: read the choices (and apply them), write them back. */
	OUTLAWSRUNTIME_API void LoadSettings(const TMap<FString, FString>& Settings);
	OUTLAWSRUNTIME_API void GatherSettings(TMap<FString, FString>& Out);

	/** Set the knobs to the rows' values (only what a choice or the Quest 2 profile asks for) and log them. */
	OUTLAWSRUNTIME_API void Apply(const TCHAR* Why);
	/** The eye buffer as last measured ("1680x1760 per eye"), for the HUD. */
	OUTLAWSRUNTIME_API FString EyeBufferText();
	/** The resume line's state ("refresh 72 Hz (asked 72), vr.PixelDensity ..., cpu/gpu level ..., clocks ..."), for
	 *  REQ-PERFTESTMODE's settings dump. */
	OUTLAWSRUNTIME_API FString StateText();

	/** Q2PROFILE: which headset, from ro.hardware (hollywood, eureka, panther, seacliff, monterey), then Build.MODEL
	 *  ("Quest 2", "Quest 3", "Quest 3S", "Quest Pro"; a plain "Quest" is a Quest 2), then the GPU (Adreno 650 / 740).
	 *  OutSignal says which decided. Logged at startup as "OLRT headset detect: <headset> by <signal> (...)". */
	enum class EHeadset : uint8 { Unknown, Quest1, Quest2, Quest3, QuestPro, PC };
	OUTLAWSRUNTIME_API EHeadset ClassifyHeadset(const FString& Model, const FString& Hardware, const FString& Gpu, FString* OutSignal);
	OUTLAWSRUNTIME_API const TCHAR* HeadsetName(EHeadset H);
	OUTLAWSRUNTIME_API EHeadset Headset();
	/** The CPU and GPU level asked through XR_EXT_performance_settings (3 sustained high under the Quest 2 profile; 0 none). */
	OUTLAWSRUNTIME_API int32 PerfLevel();
	/** Q2PROFILE step 2: the app came back from a suspend (Android foreground / reactivate): the refresh rate, the clock
	 *  levels, vr.PixelDensity (and the headset's pixel density) and the foveation put back, before and after logged as
	 *  "OLRT resume <n> (...): before: ..." / "after: ..." / "5 s on: ...". The enemy throttle starts again at level 0. */
	OUTLAWSRUNTIME_API void OnAppResume(const TCHAR* Why);
	/** How many resumes so far (the throttle watches it). */
	OUTLAWSRUNTIME_API int32 ResumeCount();
}
