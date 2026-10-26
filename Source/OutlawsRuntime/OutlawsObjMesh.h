#pragma once

#include "CoreMinimal.h"
#include "OutlawsGeometry.h"

class UTexture;
class UTexture2D;

/**
 * Minimal Wavefront OBJ + MTL reader for private placeholder models kept OUTSIDE the project
 * (the VE revolver and rifle under the local item folder's ve_weapons, FOutlawsAssets::PlaceholderDir).  Loaded at
 * runtime into FOutlawsMeshSection lists, so nothing from them is ever cooked or shipped.
 */
namespace OutlawsObj
{
	struct FObjMesh
	{
		TMap<FString, FOutlawsMeshSection> Sections;   // by material name; Texture = the map_Kd file path
		FBox Bounds = FBox(ForceInit);                   // Unreal space, cm
	};

	/**
	 * Read Path.  Source coordinates are converted with Scale (e.g. 100 for metres) and, when
	 * bRightHanded, a Y flip (right-handed Z-up sources such as RenderWare) with the winding
	 * reversed to match.
	 */
	OUTLAWSRUNTIME_API bool Load(const FString& Path, double Scale, bool bRightHanded, FObjMesh& Out);

	/** A texture from a PNG/TGA/BMP file on disk (nearest filtering, no mips); null on failure. */
	OUTLAWSRUNTIME_API UTexture2D* LoadTextureFile(const FString& Path);

	/** Which texels of a texture are solid (alpha at least a half), row by row, 1 a byte. */
	struct FSolidMask
	{
		int32 W = 0;
		int32 H = 0;
		TArray<uint8> Solid;
		bool At(double U, double V) const;   // wrapped, as the material samples
	};
	/** The solid texels of a texture LoadTextureFile made (read from its file once); null for any other texture. */
	OUTLAWSRUNTIME_API const FSolidMask* SolidMaskOf(const UTexture* T);
}
