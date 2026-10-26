#pragma once

#include "CoreMinimal.h"
#include "OutlawsObjMesh.h"

/**
 * The held guns built at runtime from the player's own HD pickup sprite (REQ-RUNTIMEGUNS, user 2026-10-05), by the
 * recipe tools/olue/item3d/make_held_recipe.py writes (Content/HeldGuns/<NAME>_build.json): the body and the action
 * parts traced and inflated off the sprite as the generator does (guns.inflate_round, proto.inflate), then our own
 * detail geometry, its solid colours read off the sprite at the recipe's sample points.  The result is what the OBJ
 * loader gives for the shipped <NAME>.obj and <NAME>_<group>.obj (group "" is the gun itself), plus the gun's
 * texture before its transparent texels are filled.  Cached in Saved/HeldGuns/<NAME>.bin, keyed by the sprite's and
 * the recipe's content and the builder's version.
 */
namespace OutlawsGunBuilder
{
	struct FBuiltGun
	{
		FString Name;
		int32 TexW = 0, TexH = 0;
		int32 ImageRows = 0;                         // the rows the fill covers (sprite and blocks; the swatches below)
		TArray<FColor> Tex;                          // TexW x TexH, sprite + blocks + swatches, not yet filled
		TMap<FString, OutlawsObj::FObjMesh> Groups;  // "" the gun, else "hammer", "trigger", "lever", "cyl"
		double BuildMs = 0.0;
		bool bCacheHit = false;
	};

	/** Sprite: the player's HD pickup sprite (SW x SH).  TexturePath: what each section's Texture names (as the
	 *  shipped .mtl's map_Kd would).  False with OutError when the recipe or the sprite does not fit. */
	OUTLAWSRUNTIME_API bool BuildOrLoad(const FString& RecipePath, const TArray<FColor>& Sprite, int32 SW, int32 SH,
		const FString& TexturePath, FBuiltGun& Out, FString& OutError);
}
