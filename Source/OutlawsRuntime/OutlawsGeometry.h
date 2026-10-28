#pragma once

#include "CoreMinimal.h"

class FOutlawsLevel;
struct FOutlawsSector;

/** Size used for UV maths and anchoring rules for one level texture. */
struct FOutlawsTexInfo
{
	int32 W = 64;
	int32 H = 64;
	/** ATX-authored (or 640x480 canvas) mid texture over an opening: pinned to the opening's top-left. */
	bool bTopLeftMid = false;
	bool bAtxStops = false;   // an ATX with STOPs: each wall runs its own program (INF NEXT_STOP / GOTO_STOP to the wall)
	bool bAtxLoop = false;    // an ATX that loops on its own: every surface of the stem animates together
};

/** One material's worth of triangles, in Unreal space (cm), unlit brightness in the vertex colour. */
struct OUTLAWSRUNTIME_API FOutlawsMeshSection
{
	FString Texture;         // texture stem (e.g. GFPLANKV) or PALETTE
	bool bMasked = false;    // palette index 0 transparent (windows, fences, overlays)
	TArray<FVector> Positions;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FLinearColor> Colors;
	TArray<int32> Triangles;
	int32 WaterGroup = -1;   // faces of an underwater sector: its body of water (the level tints their material)
	int64 AtxWall = -1;      // a wall whose ATX texture stops: FOutlawsBuildFilter::Key of it (its material animates alone)
	// (REQ-SECTORLIGHT) faces of a sector whose light an INF CHANGE_LIGHT moves: that elevator, the sector's AMBIENT less its
	// master's, and the walls' LIGHT (0 for floors and ceilings), so the section's light is LightLevel(level + LightOff, LightWall)
	int32 LightElev = -1, LightOff = 0, LightWall = 0;

	/**
	 * Add a convex polygon.  Pts are oriented the exporter's way: the right-handed cross product
	 * (p1-p0) x (p2-p0) points to the visible side.  ObjUVs use the OBJ convention (v up) and are
	 * converted to Unreal's (1 - v).  bReverse emits Unreal's front-face order (default).
	 */
	/** Swirl: the texture wobbles (SECFLAG1_SWIRLFLOORTEX), carried to the materials as vertex alpha 0.5. */
	void AddFace(const FVector* Pts, const FVector2D* ObjUVs, int32 N, float Brightness, bool bReverse, bool bSwirl = false);
	int32 NumTriangles() const { return Triangles.Num() / 3; }
};

/**
 * Which surfaces a build emits.  The static world skips everything INF elevators move; each
 * door or lift group is then built on its own with the include-only lists.
 */
struct OUTLAWSRUNTIME_API FOutlawsBuildFilter
{
	TSet<int32> SkipFloor, SkipCeil;       // sector floors / ceilings emitted elsewhere
	TSet<uint64> SkipWalls;                 // Key(sector, wall)
	TSet<int32> FillHolesOf;                // hole rings made only of walls adjoining these sectors are filled
	bool bOnly = false;                     // emit only the listed surfaces and walls
	TSet<int32> OnlyFloor, OnlyCeil;
	TSet<uint64> OnlyWalls;
	TMap<int32, double> FloorShift;         // lift sectors: live floor minus authored floor (units)
	TSet<uint64> SkipMid;                   // walls whose see-through mid (glass) is built elsewhere
	bool bMidOnly = false;                  // with bOnly: just the see-through mids of OnlyWalls
	/** The bands of a listed wall meant (1 top, 2 mid, 4 bottom, 8 sign / overlay); a wall not here: all of them.  A
	 *  scrolling wall moves only the bands its flags name (user 2026-09-29: canyon's whole walls ran like falls). */
	TMap<uint64, uint8> WallBands;
	/** (BUG-OUTHOUSEWALL) a double-adjoined wall beside an explodable wall, filled (WallMidTex) only in its opening to this
	 *  sector, its sign drawn as the original draws it while that opening is shut: on the wall's solid part, anchored there. */
	TMap<uint64, int32> FillToward;
	static uint64 Key(int32 S, int32 W) { return (uint64(uint32(S)) << 32) | uint32(W); }

	bool WantFloor(int32 S) const { return bOnly ? OnlyFloor.Contains(S) : !SkipFloor.Contains(S); }
	bool WantCeil(int32 S) const { return bOnly ? OnlyCeil.Contains(S) : !SkipCeil.Contains(S); }
	bool WantWall(int32 S, int32 W) const { const uint64 K = Key(S, W); return bOnly ? OnlyWalls.Contains(K) : (!SkipWalls.Contains(K) || WallBands.Contains(K)); }
	bool WantBand(int32 S, int32 W, uint8 Bit) const
	{
		const uint64 K = Key(S, W);
		const uint8* M = WallBands.Find(K);
		if (bOnly) return OnlyWalls.Contains(K) && (!M || (*M & Bit));
		return !SkipWalls.Contains(K) || (M && !(*M & Bit));       // a partial skip: the named bands go elsewhere
	}
	bool WantMid(int32 S, int32 W) const { return !SkipMid.Contains(Key(S, W)); }
};

/** Level geometry (reference: olue_export.py build_mesh, build_sky). */
struct OUTLAWSRUNTIME_API FOutlawsGeometry
{
	static constexpr double UnitsToCm = 30.0;
	static constexpr double TexelsPerUnit = 8.0;
	static constexpr double MinSegment = 0.01;
	static constexpr double SkyRadiusCm = 60000.0;

	/** The sky's own surfaces: exterior ceilings and floors, sky top / bottom wall bands. The Jedi engine
	 *  draws these as sky, so nothing beyond them shows (a floating sector above the civlwar1 start). */
	FOutlawsMeshSection SkyFaces;
	TMap<FString, FOutlawsMeshSection> World;   // key "TEX" or "TEX|m"
	TMap<FString, FOutlawsMeshSection> Water;   // key "TEX"
	/** Collision only, from the opening flags: ALL (walkers and shots), PAWN (walkers; shots pass),
	 *  SHOT (shots only), ENEMY (bad guys only: FENCE).  Mid textures themselves do not collide
	 *  (their World keys start with '~'). */
	TMap<FString, FOutlawsMeshSection> Blockers;
	/** The glass of TINT walls: one translucent quad per opening in the wall plane, behind both
	 *  sides' mid textures (their frames stay in front, the holes show the tint). */
	FOutlawsMeshSection Tint;
	FBox Bounds = FBox(ForceInit);
	TMap<FString, int32> Stats;
	int32 TriangulationFallbacks = 0;
	double AreaError = 0.0;                     // summed |polygon area - triangle area| (units^2)
	TArray<int32> SuspectSectors;               // sectors whose triangles do not cover their area

	/** The one conversion pair: Outlaws units and axes (x, y up, z) to UE cm (UE X = z, Y = x, Z = y), and back. Each
	 *  part is one multiply or one divide by UnitsToCm, the arithmetic of the inline forms these replaced, so results
	 *  match them to the bit. LiftUnits is added to y after the divide (a point just over the feet, for sector lookups).
	 *  Plan: the floor plan's (x, z). */
	static FVector ToUE(double X, double Y, double Z) { return FVector(Z * UnitsToCm, X * UnitsToCm, Y * UnitsToCm); }
	static FVector ToUE(const FVector2D& Plan, double Y) { return ToUE(Plan.X, Y, Plan.Y); }
	static FVector ToOutlaws(const FVector& P) { return FVector(P.Y / UnitsToCm, P.Z / UnitsToCm, P.X / UnitsToCm); }
	static FVector ToOutlaws(const FVector& P, double LiftUnits) { return FVector(P.Y / UnitsToCm, P.Z / UnitsToCm + LiftUnits, P.X / UnitsToCm); }
	static FVector2D ToOutlawsPlan(const FVector& P) { return FVector2D(P.Y / UnitsToCm, P.X / UnitsToCm); }

	void Build(const FOutlawsLevel& L, const TMap<FString, FOutlawsTexInfo>& Tex, bool bReverseWinding, const FOutlawsBuildFilter* Filter = nullptr);
	static FString PickSkyTexture(const FOutlawsLevel& L);
	static void BuildSky(const FOutlawsLevel& L, const FOutlawsTexInfo& SkyInfo, const FBox& LevelBounds, bool bReverseWinding, FOutlawsMeshSection& Out);
	/** Triangles as sector vertex indices; returns false when the sector has no usable ring.  FillHolesOf: door sectors
	 *  whose holes and notches in this sector are filled (OutCutNotches counts the notches cut). */
	static bool TriangulateSector(const FOutlawsSector& S, TArray<int32>& OutTris, int32* OutFallbacks = nullptr, double* OutAreaError = nullptr,
		const TSet<int32>* FillHolesOf = nullptr, int32* OutCutNotches = nullptr);

	/** Sector index -> its body of water (joined underwater sectors), set by the level before it builds: every face
	 *  an underwater sector owns (floor, ceiling, walls, overlays) is put in its water's sections, as the LawMaker
	 *  viewer tints by the owning sector's UNDERWATER flag (user 2026-09-28: tinting by position broke in spots). */
	static TMap<int32, int32> SectorWaterGroup;
	/** (REQ-SECTORLIGHT) Sector index -> (its CHANGE_LIGHT elevator, its AMBIENT less the elevator's master's, 0), set by the
	 *  level before it builds: the faces a lit sector owns are put in sections of their own (per elevator, offset and wall
	 *  light), so the level can change their light without touching the rest of the world. */
	static TMap<int32, FIntVector> SectorLight;

	/** -OLAdjoinScan only: build with the sign rule as it was before BUG-ADJOINDEFAULT (each sign drawn as one panel
	 *  over the wall's whole floor-to-ceiling height, across any opening, its DEFAULT placeholder too). */
	static bool bOldSignRule;
	/** -OLOldSkygap (before-and-after shots and sweeps): a door's notch in a ring stays out of the room's floor and
	 *  ceiling, as before BUG-SKYGAP. */
	static bool bOldSkygap;
	/** -OLAdjoinScan only: when set, one line per surface on an adjoined wall that the original would not draw. */
	TArray<FString>* Audit = nullptr;

private:
	void AddTo(TMap<FString, FOutlawsMeshSection>& Map, const FString& Tex, bool bMasked, const FVector* Pts, const FVector2D* UVs, int32 N, float Brightness, bool bReverse, bool bNoCollision = false, bool bSwirl = false, int32 WaterGroup = -1, int64 AtxWall = -1,
		const FIntVector& Light = FIntVector(-1, 0, 0));
};
