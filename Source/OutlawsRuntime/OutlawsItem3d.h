#pragma once

#include "CoreMinimal.h"
#include "OutlawsObjMesh.h"

class AOutlawsLevelActor;
class UTexture2D;

/**
 * (BUG-DEVDIR2, user 2026-11-01, option 2: "the meshes ship, the sprite's pixels never do") The sprite-made item3d models
 * (the floor props and pickups, the held Gatling, lantern, armour plate, tools and scope) as a tester's install has them:
 * Content/Item3d/<NAME>.obj and held/<NAME>.obj hold geometry and UVs only, and <NAME>_tex.json says how the texture is
 * painted at runtime from the player's own HD sprite (tools/olue/item3d/make_item_ship.py writes them; item_blocks.py's
 * OPS is the op list this file runs, matching its Python evaluator texel for texel where it can).  TABLE02 and the stool
 * ship no mesh at all: the Remaster's own TABLE01 3DO scaled to the sprite's size.
 *
 * The dev PC's placeholders (tools/olue/placeholders/items3d, and the copy pushed to the user's headset) stay the source
 * where they are; -OLShippedItems forces the shipped form there (tests); -OLOldDevDir2 reads the placeholders only, as
 * before (a tester then sees the flat sprites).
 */
namespace OutlawsItem3d
{
	/** True when the shipped form is read (no placeholders items3d folder, or -OLShippedItems; never with -OLOldDevDir2). */
	OUTLAWSRUNTIME_API bool UseShipped();
	/** Content/Item3d. */
	OUTLAWSRUNTIME_API FString ShippedDir();
	/** Where the item models are read from: the placeholders' items3d, or ShippedDir when UseShipped. */
	OUTLAWSRUNTIME_API FString Dir();
	/** Dir()/held: the held tools, the Gatling's parts, the 3D knife and bundle. */
	OUTLAWSRUNTIME_API FString HeldDir();
	/** Whether Path (a file under Dir or HeldDir) can be loaded: the file, or a shipped model's recipe. */
	OUTLAWSRUNTIME_API bool Exists(const FString& Path);
	/**
	 * OutlawsObj::Load for an item model.  A shipped model's sections are given texture keys that Texture resolves, and its
	 * texture is painted then (from L's sprites, else the last level seen): false when it can not be (no HD sprite), so the
	 * caller falls back as it did with no model.
	 */
	OUTLAWSRUNTIME_API bool Load(const FString& Path, double Scale, bool bRightHanded, OutlawsObj::FObjMesh& Out, AOutlawsLevelActor* L = nullptr);
	/** The texture a section names: a shipped model's painted texture (built again if it was collected), else the file. */
	OUTLAWSRUNTIME_API UTexture2D* Texture(const FString& Key, AOutlawsLevelActor* L = nullptr);
	/** The level whose sprites the textures are painted from when a caller has none at hand. */
	OUTLAWSRUNTIME_API void SetLevel(AOutlawsLevelActor* L);
	/** False while the shipped form is read and no level has been seen yet (a model looked for then is not "none"). */
	OUTLAWSRUNTIME_API bool CanPaint();

	/** Pillow's Image.resize(size, LANCZOS) of RGBA texels (premultiplied as Pillow does): olue_export's for an HD frame
	 *  whose size is not 4x its cell's (the sprites the generators traced were made so). */
	OUTLAWSRUNTIME_API void ResizeLanczos(const TArray<FColor>& In, int32 W, int32 H, int32 OW, int32 OH, TArray<FColor>& Out);

	/** (tests) A shipped recipe's texture, painted from L's sprites: W x H BGRA.  False with OutError. */
	OUTLAWSRUNTIME_API bool Paint(AOutlawsLevelActor* L, const FString& RecipePath, TArray<FColor>& Out, int32& OutW, int32& OutH, FString& OutError);
}
