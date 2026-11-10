// REQ-MENUSPACING (user 3 Nov 2026: "menu buttons too crowded (multiplayer etc.)", match the remaster): a title-menu page's
// rows as far apart, for their size, as the remaster's.  The remaster as the game draws it (the menu overlay's menu.rcss, the
// user's theme): the Multiplayer page's ONLINE / LAN rows are menu_link.main, 72 dp Rye on a 90 dp line with 18 + 24 dp of
// padding and -14 dp under it, 118 dp apart: 1.64 times the font's size.  Our New Game pages already had it (118 for 72); the
// co-op and competitive pages had 80 for the same 72, the compact co-op pages (42) down to 50.  The Options sheets keep theirs
// (6.6 cm for a 4.69 cm face, 1.41, the overlay's menu_link.options_burn: 95 dp for 68, 1.40).  Text sizes are unchanged.
// A list that does not fit at that pitch starts higher where its page has room, and past that scrolls (the stick, as a
// sheet does; a line under the rows says "more below"); its own count, as a front page's rows are picked by their place in
// the list, not by MenuScroll.  -OLOldMenuSpacing: the old pitches, no scroll.
#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "OutlawsFlags.h"

namespace OutlawsMenuSpacing
{
	/** the remaster's menu_link.main: 118 dp apart for a 72 dp font */
	constexpr float RowsApart = 118.f / 72.f;
	/** the Options sheets' reference, the overlay's menu_link.options_burn: 95 dp apart for 68 dp */
	constexpr float SheetRowsApart = 95.f / 68.f;

	inline bool Old()
	{
		static const bool bOld = OLFlags::Param(TEXT("OLOldMenuSpacing"));
		return bOld;
	}

	/** a list's row pitch for a font of size Em (view units): the remaster's, or OldPitch with -OLOldMenuSpacing */
	inline float Pitch(float OldPitch, float Em)
	{
		return Old() ? OldPitch : RowsApart * Em;
	}

	/** a front page's list as last laid out (DrawFrontCanvas) and its scroll */
	struct FFrontList
	{
		FString File;                         // the page (another page: back to its top)
		bool bList = false;                   // this page's rows were laid out as a list
		int32 Scroll = 0, MaxScroll = 0;      // rows scrolled past, and at most
		int32 From = 0, Shown = 0;            // the items drawn: From .. From + Shown - 1
		float Top = 0.f, Pitch = 0.f, Em = 0.f;   // the first drawn row's middle, the pitch and the rows' font size (view units)
		float LinesFoot = 0.f;                // the page's own lines above the rows end about here (0: none)
		FBox2D More = FBox2D(ForceInit);      // the "more below" line as drawn
	};

	inline FFrontList& Front()
	{
		static FFrontList F;
		return F;
	}

	/** (the probe's) every page MenuFitWalk shows, drawn: File and the walk's check */
	using FWalkHook = TFunction<void(const FString&, TFunctionRef<void(bool, const FString&)>)>;
	inline FWalkHook& WalkHook()
	{
		static FWalkHook H;
		return H;
	}
}
