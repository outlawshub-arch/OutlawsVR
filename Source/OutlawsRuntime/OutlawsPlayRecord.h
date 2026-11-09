// REQ-BOTMIMIC: the play recorder.  Off by default.  It is on when the game starts with -OLRecordPlay, or when the settings
// file (Saved/OutlawsSettings.ini) holds RecordPlay=1, which is how the Quest turns it on (no launch flags there; the build
// session sets the key with adb).  There is no menu row.  Never during a performance test run.
//
// It records only in a multiplayer match (not co-op story, not single player), one file per match and map:
//   Saved/PlayRecord/<yyyymmdd_hhmmss>_<map>.jsonl
// The file stays on this machine.  Nothing of it goes over the network, and Saved is in neither repo.  It holds no names, no
// addresses, no serials, no user names and no paths: opponents are slot numbers only, the map is its short file name.
//
// One JSON object a line, "k" its kind (REQ-BOTMIMIC, the five measures of the plan):
//   hdr   once: v (format), char, map, dial, pace, speed (the dial shown in percent), hpmax, hz (movement samples a second), plat
//   tag   the character or the match speed changed: t, char, dial, pace, speed
//   mv    10 a second: t, f and s (forward and side speed in the view's frame), rm, rf and rs (the head's own walk in the
//         room, over the floor: its speed and its forward and side parts in the view's frame; the headset's tracked move in
//         the play space only, so the capsule's stick-driven move is never in it twice), g (on the ground), hp, w (the
//         weapon in the dominant hand, -1 none), od and ov (distance to the opponent last engaged and whether it is in sight
//         now; left out when none is engaged)
//   see   an opponent comes into sight after 1 s or more out of it: t, who, d
//   shot  every trigger pull that fires: t, w, wl (its name), pel (pellets), sc (scoped), f, s, rm (the last sample's room
//         speed), mov (moving when it fired: 0 still, 1 the stick (f and s 50 cm/s or more), 2 walking in the room (rm 30
//         cm/s or more), 3 both), tgt and td and ta (the living opponent nearest the aim line within 20 degrees, its distance
//         and the angle off, tgt -1 when none), los (a clear line to it), hit (pellets that struck a player), head (of those,
//         in the drawn head), hwho and hd (the first struck and its distance)
//   heal  health went up while alive (a pickup): t, hp0, hp1
//   died, spawn   t
//   end   the match or the map ended: t
// Units: distances in cm; room speeds (rm, rf, rs) in real cm/s (a person's own steps, which no match speed changes; a jump
// over 400 cm/s in one sample is the headset recentring, written as 0); speeds (f, s) in cm/s at the match speed of 100 %
// (the raw speed times 0.225 over the pace, the plan's speed rule, so one profile serves every speed); times (t) in game
// seconds from the file's start, which in this game are real seconds at any match speed (no time dilation, the plan's
// section 2), so they need no scaling.

#pragma once

#include "CoreMinimal.h"

class AOutlawsLevelActor;
class AOutlawsPlayer;

namespace OutlawsPlayRecord
{
	/** What the player's tick hands the recorder each frame. */
	struct FFrame
	{
		FVector Eye = FVector::ZeroVector;
		FRotator View = FRotator::ZeroRotator;
		FVector Velocity = FVector::ZeroVector;
		FVector HeadInSpace = FVector::ZeroVector;    // the camera in the play space (the headset's tracked place; fixed on a desktop)
		FQuat Space = FQuat::Identity;                // the play space's turn in the world
		bool bOnGround = true;
		int32 Weapon = -1;            // the dominant hand's, -1 bare
		float Health = 0.f;
		bool bDead = false;
		FString Character;            // San, Jam, Two, Doc, Bob, Mar (the match's)
		bool bMatchLive = false;      // the match's own level is built (not the level before it while the match is pending)
	};

	/** The player's tick: opens and closes the file, the movement samples, sight and health. */
	OUTLAWSRUNTIME_API void Tick(AOutlawsPlayer* P, AOutlawsLevelActor* L, const FFrame& F);
	/** FireGun: a shot that fires (its pellets' hits follow through NoteHit, the line is written once they are in). */
	OUTLAWSRUNTIME_API void NoteShot(AOutlawsLevelActor* L, int32 Weapon, const TCHAR* Label, int32 Pellets, bool bScoped, const FVector& From, const FVector& Dir);
	/** UOutlawsCoop::ShotAtPlayers: a pellet of the shot struck player Who at DistCm, in the drawn head or not. */
	OUTLAWSRUNTIME_API void NoteHit(int32 Who, double DistCm, bool bHead);
	/** A file is open now. */
	OUTLAWSRUNTIME_API bool IsRecording();
	/** The open file's full path, or the last one closed (empty: none this run). */
	OUTLAWSRUNTIME_API FString LastFile();
	/** Writes what is held to the file now. */
	OUTLAWSRUNTIME_API void Flush();
	/** The folder the files go in. */
	OUTLAWSRUNTIME_API FString Folder();
}
