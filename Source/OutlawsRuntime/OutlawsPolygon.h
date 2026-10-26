#pragma once

#include "CoreMinimal.h"

/**
 * The one point-in-polygon routine (REQ-TIDY tidy-5): the even-odd count of a ray from the point along +x. Sector plans,
 * route rings, the gun builder's outlines, the wheel map's shapes and the triangulator all count with Crosses.
 */
namespace OutlawsPolygon
{
	/** Whether the ray from (X, Y) along +x crosses the edge A -> B: one end strictly above Y and the other at or below
	 *  it (a vertex on the ray counts once), the crossing strictly right of X. The crossing is worked out from A's end,
	 *  so each caller keeps its own edge direction (the other way round can differ in the last bit). */
	FORCEINLINE bool Crosses(const FVector2D& A, const FVector2D& B, double X, double Y)
	{
		return (A.Y > Y) != (B.Y > Y) && X < (B.X - A.X) * (Y - A.Y) / (B.Y - A.Y) + A.X;
	}

	/** Inside the ring of Verts indices, edges Ring[i] -> Ring[i + 1]; bBackward: Ring[i] -> Ring[i - 1] (the route
	 *  rings' order, kept so their results stay the same to the bit). The indices must be valid. */
	inline bool InRing(const TArray<FVector2D>& Verts, const TArray<int32>& Ring, double X, double Y, bool bBackward = false)
	{
		bool bIn = false;
		const int32 N = Ring.Num();
		for (int32 i = 0; i < N; ++i)
		{
			const int32 j = bBackward ? (i == 0 ? N - 1 : i - 1) : (i + 1 == N ? 0 : i + 1);
			if (Crosses(Verts[Ring[i]], Verts[Ring[j]], X, Y)) bIn = !bIn;
		}
		return bIn;
	}
}
