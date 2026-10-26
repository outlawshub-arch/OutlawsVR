// BUG-DECALEDGE: a flat decal (TNT soot, blood spatter, a bullet hole, a knuckle crack) cut to the surface it lies on, so it never
// hangs out over a wall's end, a floor's edge or a doorway. The surface is the level mesh's own triangles lying in the decal's plane;
// the decal's rectangle is clipped against each of them. Plain triangles on a procedural mesh: no deferred decal, so the headset's
// mobile forward renderer draws it as it draws the rest.
#pragma once

#include "CoreMinimal.h"

class UProceduralMeshComponent;

namespace OutlawsDecalClip
{
	/** A piece of the decal: three corners in the decal's own plane (u along its X axis, v along its Y axis, cm from its centre). */
	struct FPiece
	{
		FVector2D P[3];
	};

	/**
	 * Clip the rectangle (Width x Height cm about Centre, spanned by the unit axes Ax and Ay on the plane with unit normal N) to the
	 * triangles of Comp that lie in that plane facing N (within a centimetre). Out gets the pieces that remain. Returns false when
	 * the component has no triangle there at all (a surface this cannot read: the caller keeps the whole rectangle); true with Out
	 * empty means the rectangle is entirely off the surface.
	 */
	bool Clip(UProceduralMeshComponent* Comp, const FVector& Centre, const FVector& N, const FVector& Ax, const FVector& Ay, double Width, double Height, TArray<FPiece>& Out);

	/** Index a component's triangles now (the level's build) rather than at the first decal. */
	void Prepare(UProceduralMeshComponent* Comp);

	/** The counts since the level began, for the logs: decals clipped, pieces made, decals dropped (off the surface), decals left whole. */
	FString Describe();
}
