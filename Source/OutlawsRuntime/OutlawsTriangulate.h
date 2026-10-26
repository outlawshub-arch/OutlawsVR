#pragma once

#include "CoreMinimal.h"

/**
 * Polygon-with-holes triangulation: a port of mapbox earcut, the algorithm the exporter uses
 * (mapbox_earcut), so runtime floors and ceilings match the Python reference.
 */
namespace OutlawsTriangulate
{
	/**
	 * Points: all rings concatenated. RingEnds: exclusive end index of each ring; the first ring
	 * is the outer one.  OutTriangles index into Points.  Returns how many times earcut needed
	 * its self-intersection cure pass (0 for a clean polygon).
	 */
	OUTLAWSRUNTIME_API int32 Earcut(const TArray<FVector2D>& Points, const TArray<int32>& RingEnds, TArray<int32>& OutTriangles);

	/** Sum of |area| of the triangles (validation against the polygon area). */
	OUTLAWSRUNTIME_API double TrianglesArea(const TArray<FVector2D>& Points, const TArray<int32>& Triangles);
}
