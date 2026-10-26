#pragma once

#include "CoreMinimal.h"

/**
 * REQ-KILLFEED: the match's kill lines ("Marshal killed Deputy!"), in the original's own words.  The host decides each death
 * (OutlawsCoopMatch.cpp: who struck last, with what, or the victim's own cause) and tells every machine the killer, the
 * victim and the kind; each machine words it for its own player ("You killed Deputy!", "Marshal killed you!") from its
 * local.msg, so a German install reads German, and draws it in the feed above the notices (AOutlawsPlayer::ShowKillLine).
 *
 * The words: LOCAL.MSG's death messages (olpatch4.lab).  The original sent "$$20042$$%s$$%s$$" templates that each machine
 * filled from its own table: 20040..20062 the line everyone else sees, 41..62 the victim's own, 823 the killer's.
 */

/** Why this machine's player last died, when nobody else is to blame (the level actor's, sent up with the co-op stats). */
enum class EOutlawsHurtCause : uint8
{
	None,           // anything else (the floor's hurt, a crush, the host's word)
	Fall,
	Drown,
	OwnBlast,       // this player's own dynamite
	OwnShot,        // this player's own bullet through the head
	Count
};

/** How the feed words a death. */
enum class EOutlawsObit : uint8
{
	Killed,         // 20042 "%s killed %s!", 20043 "%s killed you!", 823 "You killed %s!"
	BlewUp,         // 20061 "%s blew up %s!", 20062 "%s blew you up!"
	SelfDestructed, // 20059 "%s self-destructed!", 60 "You blew yourself up!"
	Suicide,        // 20040 "%s committed suicide!", 41 "You killed yourself!"
	Fell,           // 20044 "%s died from a fall!", 45 "You fell to your death!"
	Drowned,        // 20048 "%s drowned!", 49 "You drowned!"
	Died,           // 20046 "%s died!", 47 "You died!"
	Count
};

namespace OutlawsKillFeed
{
	/** A death nobody else is credited with, by its cause. */
	OUTLAWSRUNTIME_API EOutlawsObit FromCause(uint8 Cause);
	OUTLAWSRUNTIME_API const TCHAR* Name(EOutlawsObit Kind);
	/** The line as this machine's player reads it.  Msg(Id, Fallback) gives local.msg's text for Id (the fallback when the
	 *  install has none). */
	OUTLAWSRUNTIME_API FString Line(EOutlawsObit Kind, const FString& Killer, const FString& Victim, bool bKillerIsMe, bool bVictimIsMe,
		TFunctionRef<FString(int32, const TCHAR*)> Msg);
}
