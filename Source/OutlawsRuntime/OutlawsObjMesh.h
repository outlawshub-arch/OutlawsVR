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

	/** (SCOPEFACES) What TurnFacesOutward found and did. */
	struct FFaceTurn
	{
		int32 Faces = 0;           // triangles looked at
		int32 Pieces = 0;          // pieces: triangles joined edge to edge
		int32 Turned = 0;          // triangles turned over
		double Area = 0.0, OutBefore = 0.0, OutAfter = 0.0;      // their area, and the part of it facing out before and after
		double ShareBefore() const { return Area > 0.0 ? OutBefore / Area : 1.0; }
		double ShareAfter() const { return Area > 0.0 ? OutAfter / Area : 1.0; }
		void Add(const FFaceTurn& O) { Faces += O.Faces; Pieces += O.Pieces; Turned += O.Turned; Area += O.Area; OutBefore += O.OutBefore; OutAfter += O.OutAfter; }
	};

	/**
	 * (SCOPEFACES) Each piece of a section (triangles joined edge to edge, corners within 0.001 cm welded) wound one way and
	 * turned over when most of its area faces in: a ray off a face's front that crosses its own piece an odd number of
	 * times started inside it.  A piece is changed only when that leaves clearly more of it facing out; a turned face's
	 * corners take its new front as their normal.  item3d's tube() (rework.py) wound every tube wall inward and its caps and glass
	 * outward, so on the one-sided held and item materials the rifle's telescope and the lever, crowbar and crank pickups
	 * showed their insides.  bApply false only measures.  Flat cards and double-faced meshes come out as they went in.
	 * (FACETURN) A big piece's rays are tried only against the triangles whose boxes they pass (a box tree): the same
	 * crossings as trying every triangle, a gun pickup in a few ms instead of 170 to 350.
	 */
	OUTLAWSRUNTIME_API FFaceTurn TurnFacesOutward(FOutlawsMeshSection& S, bool bApply);
	OUTLAWSRUNTIME_API FFaceTurn TurnFacesOutward(FObjMesh& M, bool bApply);
	/**
	 * (FACETURN) An item3d model as the floor or a hand draws it single-faced: TurnFacesOutward applied, and one OLRT line,
	 * "<What> faces: ...".  item3d's generators wound some tubes and shells inward (rework.py's tube(), treegen's trunks and
	 * cactus arms, the cow skull, the flags, the gatling's drum...): on the one-sided held material those showed their
	 * insides, and on the headset the ink shell, pushed out along the normals, sat inside them (no outline).  Every model
	 * goes through it, so none needs naming; flat cards and double-faced models come out as they went in.
	 * -OLOldScopeFaces: every model as its file has it (what would turn is still logged).
	 */
	OUTLAWSRUNTIME_API FFaceTurn FaceItemModelOutward(FObjMesh& M, const FString& What);
	/** The same measure of triangles in Unreal's order (a live mesh section's), nothing changed. */
	OUTLAWSRUNTIME_API FFaceTurn MeasureFacesOutward(const TArray<FVector>& Positions, const TArray<int32>& Triangles);

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
