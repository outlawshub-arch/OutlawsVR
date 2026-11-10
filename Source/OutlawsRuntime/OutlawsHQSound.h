#pragma once

#include "CoreMinimal.h"

/**
 * REQ-HQSOUND (the user, 8 and 9 Nov): the HQ Sound Pack by T4H_Dingle and Dxierzan for every player, as a release asset
 * (sound files only, never in the source).  HQSoundPack.zip holds the pack's 237 sounds as Ogg Vorbis (<name>.ogg, stored)
 * and manifest.txt ("<sha256> <bytes> <name>.ogg" a line after its "#" lines).
 *
 * Where it is found, first that exists: -OLHQSoundPackFile=<zip> (tests only), Tools\HQSoundPack.zip in the PC package, the
 * Quest's updated data folder (an in-game update brings it there), HQSoundPack.zip beside the install's LABs (the headset's
 * files/Outlaws, where Install on Quest.bat pushes it).
 *
 * Decoded ONCE: at the first start each sound is checked against the manifest's SHA-256, decoded on a worker (the engine's
 * Vorbis decoder) and kept as 16-bit wav in the media cache's HQSound folder (a PC: %LOCALAPPDATA%\OutlawsVR\media\HQSound,
 * as the music decoded from flac; the Quest: the app's own files folder, HQSound), with stamp.txt written last.  A later
 * start whose stamp names the same manifest and finds every wav at its size decodes nothing.  A pack missing, unreadable, or
 * with any sound that does not match or decode: none of it is used (today's sounds), nothing crashes.
 *
 * When it is on, a sound the pack holds is the pack's, with the Remastered Sounds row on or off: it wins the 43 names it
 * shares with the remaster's olsfxhd set and every other name it holds; a native map's own LAB and a language's voice LAB
 * still come first (FOutlawsAssets::GetSound).  The Sound row "HQ Sound Pack" (On/Off, settings key HQSoundPack, default On);
 * Off is today's sounds exactly.  -OLOldSoundPack: today's sounds whatever the row says (no decode either).
 */
namespace OLHQSound
{
	/** The row: true On (the default). */
	OUTLAWSRUNTIME_API bool Chosen();
	OUTLAWSRUNTIME_API void SetChosen(bool bOn);
	/** -OLOldSoundPack. */
	OUTLAWSRUNTIME_API bool OldFlag();
	/** Finds the pack and makes it ready (the cache, or the decode on a worker), once a run; InstallRoot is the LABs' folder.
	 *  Nothing while the row is Off or with -OLOldSoundPack: turning the row On later starts it. */
	OUTLAWSRUNTIME_API void Start(const FString& InstallRoot);
	/** Chosen, no -OLOldSoundPack, and the pack decoded and checked. */
	OUTLAWSRUNTIME_API bool Active();
	/** The decoded wav for a sound name (any case, "name.wav"), when Active() and the pack holds it; else empty. */
	OUTLAWSRUNTIME_API FString WavFor(const FString& Name);
	/** Moves on whenever Active() or the set of sounds changes: a cache of decoded sounds filled under another count is stale. */
	OUTLAWSRUNTIME_API uint32 Serial();

	/** For the probe and the log: none, off, decoding, ready, failed (with why). */
	OUTLAWSRUNTIME_API FString State();
	OUTLAWSRUNTIME_API bool Busy();
	OUTLAWSRUNTIME_API FString PackPath();
	OUTLAWSRUNTIME_API FString CacheDir();
	/** The pack's sound names ("name.wav", lower case), once ready. */
	OUTLAWSRUNTIME_API TArray<FString> Names();
	/** How many sounds this run decoded (0 on a start that found the cache), and how long the decode took. */
	OUTLAWSRUNTIME_API int32 DecodedThisRun();
	OUTLAWSRUNTIME_API double DecodeSeconds();
}
