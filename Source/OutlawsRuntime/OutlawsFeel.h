#pragma once

#include "CoreMinimal.h"

/**
 * The feel constants every swing shares (the consistency plan, user 2026-10-01: rules the player can learn, not one number
 * per feature).  Speeds are the hand's own swing, cm/s, against the body (AOutlawsPlayer::HandVelocity): running with a
 * thing held out is not a swing, and the world speed broke in a run (it went past the 4 m/s sanity cap and was zeroed).
 */
namespace OutlawsFeel
{
	/** Auto health regen below half health, standing still (REQ-REGEN2X, user 2026-10-11: twice the 0.1 a second it was), in
	 *  points of the 10 a second; walking a quarter of it, running none, crouched twice.  It starts at once and stops at 5 of 10.
	 *  Every mode and difficulty; in co-op and multiplayer the host's value (AOutlawsCoopSession::RegenRate) applies. */
	constexpr float RegenPerSecond = 0.2f;
	constexpr float RegenCap = 5.f;

	/** A light touch that still moves things: a hanging lamp swung, a held map's shove. */
	constexpr float HitSpeedLight = 150.f;
	/** A real swing: fists, props, the shield, tools, carried gear, held bad guys; glass breaks, bad guys are hurt. */
	constexpr float HitSpeedHard = 250.f;

	/** The haptic for a hit at Speed: 0.3 at a light touch up to 1 at twice a hard swing (state changes keep fixed pulses). */
	inline float ImpactPulse(float Speed)
	{
		return FMath::Clamp(0.3f + 0.7f * (Speed - HitSpeedLight) / (2.f * HitSpeedHard - HitSpeedLight), 0.3f, 1.f);
	}
}
