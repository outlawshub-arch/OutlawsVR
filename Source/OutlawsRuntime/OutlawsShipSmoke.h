#pragma once

#include "CoreMinimal.h"

class AOutlawsLevelActor;

/**
 * REQ-SHIPLOG: -OLShipSmoke, the smoke check that stays in a Shipping build (not under WITH_OUTLAWS_TESTS).  The game starts
 * as usual to the main menu, then each story level is started in turn as a historical mission (Bad) and given two seconds
 * of play; one line each ("OLShipSmoke: <level> loaded in N ms", or "OLShipSmoke: <level> FAILED: <why>"), a summary
 * line, and the game quits from the core ticker a second later (BUG-EXITCRASH: a quit right after a load could crash in
 * Niagara's component pool on the way out).  Exit code 0 when everything loaded, 1 otherwise.  Without the flag the two
 * notes below return at once.  It writes the level-start autosaves of none of the levels; run it with -userdir anyway.
 */
namespace OutlawsShipSmoke
{
	bool Active();
	/** The module's start: with the flag, the ticker that times the waits and starts each level. */
	void Startup();
	/** The main menu is up (AOutlawsLevelActor::ShowFrontMenu). */
	void NoteMenu(AOutlawsLevelActor* Level);
	/** A level build has ended, built or not (AOutlawsLevelActor::Build, every way out). */
	void NoteBuilt(AOutlawsLevelActor* Level);
	/** (BUG-SHOVELWHACK) A probe with steps still to take (-OLToolWhackProbe): the level being played is kept at least Seconds
	 *  more, up to four minutes from its build.  Nothing while a level is loading, or without the flag. */
	void HoldLevel(double Seconds);
}
