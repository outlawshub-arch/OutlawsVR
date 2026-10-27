#pragma once

// REQ-HELDCLASH (user 2026-10-27): two bad guys held one in each hand collide where they are drawn.  Each man's frame on show
// is a mask of its opaque pixels (FSpriteAtlas::Alpha, the 1x atlas; palette index 0 is see-through), placed as his quad is
// drawn (its transform, the left hand's mirror, REQ-HELDSWAY's trailing legs).  Both are laid along the eye's view onto a
// plane through their middle: they touch where the masks overlap there, if no more than a body's thickness apart along the
// view.  The see-through pixels never touch (between the legs, between an arm and the body).
// The player's side (offsets, the smash on the first contact) is in OutlawsPlayerHeldClash.cpp.

#include "CoreMinimal.h"

namespace OutlawsTextures { struct FSpriteAtlas; }
class AOutlawsSpriteActor;
class AOutlawsEnemy;

namespace OutlawsHeldClash
{
	/** One atlas cell's opaque pixels as bits, row 0 at the top (the cell as the quad shows it, before any mirror). */
	struct FMask
	{
		int32 W = 0, H = 0, Words = 0;           // the cell (px) and the 64-bit words a row
		TArray<uint64> Bits;
		int32 X0 = 0, Y0 = 0, X1 = -1, Y1 = -1;  // the opaque pixels' bounds (inclusive)
		FVector2D Mid = FVector2D::ZeroVector;   // their middle (px, pixel corners: a pixel's centre is x + 0.5)
		int32 Count = 0;
		bool IsEmpty() const { return Count == 0; }
		bool At(int32 X, int32 Y) const
		{
			return X >= 0 && Y >= 0 && X < W && Y < H && ((Bits[Y * Words + (X >> 6)] >> (X & 63)) & 1ull) != 0;
		}
	};
	/** The mask of Cell (false: no opacity kept for this atlas, or the cell is off it). */
	bool BuildMask(const OutlawsTextures::FSpriteAtlas& A, int32 Cell, FMask& Out);

	/** A mask as drawn this frame: the pixel corner (X, Y) is at O + X * Ex + Y * Ey in the world (X across, Y down). */
	struct FDrawn
	{
		FVector O = FVector::ZeroVector, Ex = FVector::ZeroVector, Ey = FVector::ZeroVector;
		FVector At(double X, double Y) const { return O + Ex * X + Ey * Y; }
	};
	/** The quad of S as it is drawn now (its transform with the mirror in its scale, and REQ-HELDSWAY's trailing bottom edge
	 *  as a lean: the top stays at the hand, the bottom row moved by the sprite's swing offset; the swing's small turn of the
	 *  bottom edge is left out).  False when it has no quad. */
	bool DrawnOf(const AOutlawsSpriteActor* S, const FMask& M, FDrawn& Out);

	struct FResult
	{
		bool bNear = false;        // within the thickness along the view and their opaque boxes meet in the plane
		bool bContact = false;     // the opaque pixels overlap in the plane (and bNear)
		int32 Overlap = 0;         // plane samples opaque in both
		FVector Normal = FVector::ZeroVector;   // world, the way B is moved (A the other way)
		FVector2D Normal2 = FVector2D::ZeroVector;   // the same in the plane (zero when parted along the view)
		double Push = 0.0;         // cm along Normal, both together, to part them (with a sample's margin)
		double Gap = 0.0;          // cm between them in the plane when apart (0 touching, or their boxes meeting but too far
		                           // apart along the view; negative never)
		double DepthGap = 0.0;     // cm along the view between their middles
		double BoxGap = 0.0;       // cm between their opaque boxes in the plane (negative: overlapping by that, the narrower side)
		double CellCm = 0.0;       // the plane's sample size, about a pixel
		bool bAlongView = false;   // parted along the view (fewer cm that way than across)
		FVector PlaneR = FVector::ZeroVector, PlaneU = FVector::ZeroVector, PlaneAt = FVector::ZeroVector, View = FVector::ZeroVector;
	};
	/** A against B seen from Eye.  ThickCm: a body's thickness; PrevNormal2: last frame's way across (plane coords, kept when
	 *  nearly as short: no flicker between ways), zero for none; bMayPassAlongView: held one well in front of the other, so
	 *  parting them along the view (one sliding past behind the other) may be taken when shorter. */
	FResult Test(const FMask& MA, const FDrawn& DA, const FMask& MB, const FDrawn& DB, const FVector& Eye, double ThickCm, const FVector2D& PrevNormal2, bool bMayPassAlongView);

	/** The plane the test lays them on: through the middle of the two, facing Eye. */
	void PlaneOf(const FMask& MA, const FDrawn& DA, const FMask& MB, const FDrawn& DB, const FVector& Eye, FVector& OutAt, FVector& OutView, FVector& OutR, FVector& OutU);

	/** The player's per-hand state: the man, his mask's frame, his offset from the hand and its speed. */
	struct FHand
	{
		TWeakObjectPtr<AOutlawsEnemy> Who;
		const OutlawsTextures::FSpriteAtlas* Atlas = nullptr;
		int32 Cell = -2;
		bool bMask = false;
		FMask Mask;
		FVector Off = FVector::ZeroVector;
		FVector Vel = FVector::ZeroVector;
	};
	struct FState
	{
		FHand Hand[2];
		bool bTouching = false;              // in contact (till clearly apart: no second smash from one press)
		float SinceContact = 100.f;          // s since the last frame in contact
		FVector2D PrevN = FVector2D::ZeroVector;
		// the last pair test, for the probe
		bool bLastValid = false;
		uint64 LastFrame = 0;
		bool bLastFirst = false;             // the first frame of a contact
		bool bLastSmash = false;
		FResult Last;
		FDrawn PreA, PreB, PostA, PostB;     // as tested (before the push) and as left (after it)
		FVector Eye = FVector::ZeroVector;
		int32 Smashes = 0;
		int32 Contacts = 0;
	};

	/** What it costs (game thread, microseconds), for the probe. */
	struct FCost
	{
		int64 Builds = 0, Tests = 0;
		double BuildUs = 0.0, BuildMaxUs = 0.0, TestUs = 0.0, TestMaxUs = 0.0;
		int64 FullTests = 0;                 // tests that sampled the plane (their boxes met), not just the boxes
		double FullUs = 0.0, FullMaxUs = 0.0;
	};
	FCost& Cost();

	/** ol.heldclash.ThickCm, EaseSec, BounceCm, MaxOffsetCm. */
	double ThickCm();
	double EaseSec();
	double BounceCm();
	double MaxOffsetCm();
}
