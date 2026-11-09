// (REQ-LANGUAGE, the user 9 Nov: "Draw the accent over the menu font's own letter") The menu overlay's western fonts hold only
// letters and digits, so an accented letter used to come whole from the Noto fallback, smaller and in another style.  This
// writes a copy of such a font in which each accented Latin-1 letter is the font's own base letter with an accent mark drawn
// over (or a cedilla under) it, the mark's outline taken from the Noto font and scaled to the theme letters.  The AE and OE ligatures and the
// sharp s are the font's own A+E, O+E and s+s.  A font that already has the letters is left as it is.  -OLOldMenuAccents: no copy, the
// accented letters from Noto as before.
#pragma once

#include "CoreMinimal.h"

namespace OLFontCompose
{
	/** True with -OLOldMenuAccents (the accented letters from Noto, as before). */
	bool OldAccents();

	/** The accented letters this composes (Latin-1 and capital Y with dieresis), in a fixed order. */
	const TArray<uint32>& Letters();

	/**
	 * Theme: a TrueType font's bytes; Marks: a full font with the spacing accents (grave, acute, circumflex, tilde, dieresis, ring,
	 * cedilla).  On true, Out holds the new font and Added the code points it gained (each one the theme lacked and could build).
	 * False (Out empty) when the theme already has the letters, is not a plain glyf font, or nothing could be built.
	 */
	bool Compose(const TArray<uint8>& Theme, const TArray<uint8>& Marks, TArray<uint8>& Out, TArray<uint32>& Added);

	/** True when the font's character map has the code point. */
	bool HasChar(const TArray<uint8>& Font, uint32 Cp);

	/**
	 * For the language guard: true when the glyph for Cp begins with the very outline of the glyph for BaseCp (same points, same
	 * contour ends), so it is drawn from the font's own letter.  Pairs (AE) check their first letter.
	 */
	bool StartsWithBase(const TArray<uint8>& Font, uint32 Cp, uint32 BaseCp);

	/** The base letter a composed letter is built on (E acute: E, ae: a, sharp s: s), 0 when it is not one of Letters(). */
	uint32 BaseOf(uint32 Cp);
}
