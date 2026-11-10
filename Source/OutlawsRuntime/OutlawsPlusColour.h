#pragma once

#include "CoreMinimal.h"

/**
 * REQ-REDPLUS (the user, 8 Nov): the cross on the canteen and the medical bags in red for everyone.  The remaster's HD sprites
 * (the SPX cells) of GCANTEEN, GDOCBAG and IDOCBAG draw it blue; the launcher's Red Plus Items mod (by T4H_Dingle) redraws them
 * red.  Here the HD pixels are recoloured as they are decoded (no art ships): the cross's blue texels take the mod's colour,
 * everything else is left as it is.  The 8-bit sprites (NWX) already draw the cross red and are not touched.
 *
 * Measured against the mod's SPX (GOG and Steam stock against redplus.zip): GCANTEEN and GDOCBAG's crosses go red (hue 0,
 * saturation about 0.7, value 0.57 x the blue's + 0.12); IDOCBAG's card cross goes the orange of the 1997 sprite and the wheel
 * card (hue 31, value 0.8 x + 0.155), as the mod draws it.  A cross already red (an install with the mod) is not blue, so it
 * stays as it is.
 *
 * The Display row "Plus Colour" (Red / Blue, settings key PlusColour, default Red).  -OLOldPlusColour: the stock blue whatever
 * the row says.  Every use of the HD sprite takes it: the floor sprite (BuildSpriteAtlasHD), the 3D canteen and bag painted
 * from the sprite and the wrist and map icons (DecodeSpriteAtlasHdPixels and the HD atlas).
 */
namespace OutlawsPlusColour
{
	/** The row: true Red (the default), false Blue (the stock look). */
	OUTLAWSRUNTIME_API bool RedChosen();
	OUTLAWSRUNTIME_API void SetRedChosen(bool bRed);
	/** -OLOldPlusColour (the stock blue whatever the row says). */
	OUTLAWSRUNTIME_API bool OldFlag();
	/** The recolour is on: Red chosen and no -OLOldPlusColour. */
	OUTLAWSRUNTIME_API bool Active();
	/** The NWX or SPX name (any case, with or without a path or extension) is one of the three. */
	OUTLAWSRUNTIME_API bool IsPlusItem(const FString& Name);
	/** The cross's blue texels of that item recoloured, in place (nothing when it is not one of the three); returns how many
	 *  texels changed. */
	OUTLAWSRUNTIME_API int32 RecolourPixels(const FString& Name, TArray<FColor>& Px);
	/** The same when Active(), else nothing. */
	OUTLAWSRUNTIME_API int32 Apply(const FString& Name, TArray<FColor>& Px);
}
