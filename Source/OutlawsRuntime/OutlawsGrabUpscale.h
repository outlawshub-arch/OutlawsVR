#pragma once

#include "CoreMinimal.h"

class FOutlawsAssets;
namespace OutlawsTextures { struct FSpriteAtlas; }

/**
 * REQ-GRABUPSCALE: a held bad guy is seen at arm's length, so the frame he is held in (the first frame of his HIT_CHOR, seen
 * from the front or the back) shows a 2x AI upscale of its Remaster HD frame while he is held.
 *
 * Never shipped art: the upscales are made on the player's own PC from the player's own files, by Real-ESRGAN's portable
 * realesrgan-ncnn-vulkan (realesr-animevideov3 x2, ThirdParty/RealESRGAN beside the game: the exe, the x2 model and the
 * licence).  Each held cell's HD frame (4x) has its edge colours bled into the clear area (no dark halo), goes through the
 * upscaler, and takes back its own alpha resized with Lanczos and sharpened (a 1 to 2 px soft edge), as the planner's
 * prototype did (tools/olue/out/grabupscale/scripts: prep.py, merge.py).
 *
 * Where they go: %LOCALAPPDATA%/OutlawsVR/media/Grab/<nwx stem>_<WAX frame>.png on the PC (beside the converted movies);
 * a headset reads <its files>/Outlaws/Grab/, where Install on Quest.bat copies them as it copies the movies.
 * When they are made (PC only, in the background, one job at a time): the front end on a launch that finds them missing
 * (every stock bad guy), each level's own bad guys after its build (a custom map's), and -OLGrabUpscaleAll (the converter
 * step: every stock bad guy, then the game quits).
 * Missing (no remaster, no upscaler, no Vulkan, a headset not given them): the HD atlas as before, no error.
 * Switches: ol.GrabUpscale 0 or -OLGrabUpscaleOff (none used or made), -OLGrabUpscaleMissing (tests: treated as missing),
 * -OLGrabUpscaleDump (tests: the 4x HD crops kept beside the results in Saved/GrabUpscaleDump).
 */
namespace OutlawsGrabUpscale
{
	OUTLAWSRUNTIME_API bool IsOff();
	OUTLAWSRUNTIME_API bool PretendMissing();
	/** "<nwx stem>_<frame>.png", lower case. */
	OUTLAWSRUNTIME_API FString FileName(const FString& Nwx, int32 Frame);
	/** The upscale's path for a file name: <install root>/Grab/<file>, else (a PC) the media cache's Grab/<file>; empty when missing. */
	OUTLAWSRUNTIME_API FString Find(const FString& InstallRoot, const FString& File);
	/** The cells a hold shows: HitChor's first frame from the front (view 0) and the back (view Dirs/2). */
	OUTLAWSRUNTIME_API void HeldCells(const OutlawsTextures::FSpriteAtlas& Atlas, int32 HitChor, TArray<int32>& Out);
	/** The upscale's size for an atlas cell (8x the 1x cell: the HD frame's 4x, then 2x). */
	inline FIntPoint UpscaledSize(int32 CellW, int32 CellH) { return FIntPoint(CellW * 8, CellH * 8); }
	/** PC: the media cache's Grab folder, and the upscaler (empty where there is none). */
	OUTLAWSRUNTIME_API FString OutDir();
	OUTLAWSRUNTIME_API FString ToolExe();
	/** PC, game thread: make whatever is missing for these sprite sets (nwx, hit chor) in the background. */
	OUTLAWSRUNTIME_API void Kick(const TSharedPtr<FOutlawsAssets>& Assets, const TArray<TPair<FString, int32>>& Sets, const FString& Why);
	/** PC, game thread: every stock bad guy's, in the background, unless a run for this install already finished. */
	OUTLAWSRUNTIME_API void KickAll(const TSharedPtr<FOutlawsAssets>& Assets, const FString& Why);
	/** PC, game thread, blocking (the converter step): every stock bad guy's. True when nothing failed. */
	OUTLAWSRUNTIME_API bool RunAllNow(const TSharedPtr<FOutlawsAssets>& Assets);
	/** A job is running or queued. */
	OUTLAWSRUNTIME_API bool IsBusy();
	/** Jobs finished this session (tests wait on it). */
	OUTLAWSRUNTIME_API int32 JobsDone();
}
