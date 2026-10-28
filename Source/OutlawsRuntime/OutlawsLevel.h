#pragma once

#include "CoreMinimal.h"

/**
 * Parsed Outlaws level text (LVT) and object list (OBT).  Reference: olue_export.py
 * (Level, Sector, WALL_RE, OBJ_RE, surface_y, loops_from_walls).  Values are kept in
 * Outlaws units and axes (x, y up, z); FOutlawsGeometry converts to Unreal.
 */
namespace OutlawsFlags
{
	// sector FLAGS word 1 (Level.pas)
	constexpr uint32 ExtCeiling = 0x1;
	constexpr uint32 ExtFloor = 0x2;
	constexpr uint32 ExtTopAdjoin = 0x4;
	constexpr uint32 ExtBtmAdjoin = 0x8;
	constexpr uint32 NoWalls = 0x10;
	constexpr uint32 NoSlip = 0x20;
	constexpr uint32 VelFloorOnly = 0x40;        // the sector's VELOCITY carries only what stands on its floor (SECFLAG1_VELFLOORONLY)              // no gravity slide on its slope (SECFLAG1_NOSLIP)
	constexpr uint32 Water = 0x80;
	constexpr uint32 SectorDoor = 0x100;         // an automatic door (SECFLAG1_DOOR): the engine makes it a door by itself
	constexpr uint32 SectorDoorReverse = 0x200;
	constexpr uint32 SwirlFloor = 0x800;         // the floor texture swirls (SECFLAG1_SWIRLFLOORTEX): water surfaces, from above and below  // ... swinging the other way (SECFLAG1_REVERSE)
	// damage (LawMaker Level.pas; the amounts are INFERRED from Dark Forces: 5 and 10 a second, /10 here):
	// SEC_ anywhere in the sector, FLR_ standing on its floor, DIE kills
	constexpr uint32 SecDamageSmall = 0x40000;
	constexpr uint32 SecDamageLarge = 0x80000;
	constexpr uint32 SecDamageDie = 0x100000;
	constexpr uint32 FloorDamageSmall = 0x200000;
	constexpr uint32 FloorDamageLarge = 0x400000;
	constexpr uint32 FloorDamageDie = 0x800000;
	constexpr uint32 SecTerminate = 0x1000000;
	constexpr uint32 SecSecretTag = 0x2000000;   // SECFLAG1_SECRET_TAG: counted as a secret found when entered (REQ-STATS)   // what is carried into it is gone (the train's scenery, off the map's end)
	constexpr uint32 SlopedFloor = 0x40000000u;
	constexpr uint32 SlopedCeiling = 0x80000000u;
	// wall FLAGS word 1
	constexpr uint32 WallMidTex = 0x1;
	constexpr uint32 WallHFlip = 0x4;
	constexpr uint32 WallAnchor = 0x8;
	constexpr uint32 WallScrollAny = 0x780;  // SCROLLTOP 0x80, SCROLLMID 0x100, SCROLLBOTTOM 0x200, SCROLLSIGN 0x400 (Level.pas)
	constexpr uint32 WallTint = 0x20;        // see-through mid drawn as tinted glass (WALLFLAG1_TINT)
	constexpr uint32 WallShatter = 0x4000;   // a glass pane that breaks when shot (the opening becomes walkable)
	constexpr uint32 WallNoPass = 0x800;     // nobody walks through the opening (Level.pas WALLFLAG1_NOPASS)
	constexpr uint32 WallFence = 0x2000;     // bad guys do not pass (WALLFLAG1_FENCE)
	constexpr uint32 WallProjectileOk = 0x8000;   // shots pass the mid texture (WALLFLAG1_PROJECTILE_OK)
}

struct FOutlawsPlane
{
	double Y = 0.0;
	int32 Tex = -1;
	double OffX = 0.0;
	double OffY = 0.0;
	double Angle = 0.0;     // the texture's turn, degrees ("FLOOR Y y tex offx offy angle"; LawMaker's plane "extra")
};

struct FOutlawsSlope
{
	int32 Sector = -1;
	int32 Wall = -1;
	double Angle = 0.0;     // units of 90/4096 degrees
	bool bSet = false;
};

struct FOutlawsWall
{
	FString Id;             // "WALL: 9CB1F458" (INF line items address walls by id)
	int32 V1 = 0, V2 = 0;
	int32 Mid = -1, Top = -1, Bot = -1, Overlay = -1;
	double MidX = 0, MidY = 0, TopX = 0, TopY = 0, BotX = 0, BotY = 0, OvX = 0, OvY = 0;
	int32 Adjoin = -1, Mirror = -1, DAdjoin = -1, DMirror = -1;
	uint32 Flags1 = 0, Flags2 = 0;
	int32 Light = 0;
};

struct FOutlawsSector
{
	int32 Index = 0;
	FString Id;             // hex id as written
	FString Name;
	int32 Ambient = 31;
	FString FloorSound;     // FLOOR SOUND: the footstep wav (empty for NULL)
	float Gravity = -60.f;  // GRAVITY, units/s^2 (townex: -60 on land, -10 in its water)
	int32 Palette = 0;      // PALETTE: which of the level's PALETTES the sector is seen through
	int32 VAdjoin = -1;
	uint32 Flags = 0;
	float Friction = 1.f;   // FRICTION: 1 grips; toward 0 slides down its slope and skates on it (ice, mud)
	FVector Velocity = FVector::ZeroVector;   // VELOCITY x y z, units/s: what stands on its floor is carried (the train's ground)
	int32 Layer = 0;
	FOutlawsPlane Floor;
	FOutlawsPlane Ceil;
	FOutlawsSlope SlopeFloor;
	FOutlawsSlope SlopeCeil;
	TArray<FVector2D> Verts;   // (x, z), double like the exporter
	TArray<FOutlawsWall> Walls;

	/** The winding of the sector's walls taken together (V1 -> V2 each): true when the inside lies to the right of each. */
	bool IsClockwise() const;
	/** The rule before BUG-WALLSFLIPPED, the shoelace over the vertex list in its own order (-OLWallFacingScan only). */
	bool IsClockwiseByVerts() const;
	bool NoWalls() const { return (Flags & OutlawsFlags::NoWalls) != 0; }
};

struct FOutlawsObject
{
	FString Name;       // upper case
	FString Id;
	FString SectorId;
	FVector Pos;        // Outlaws units (x, y, z)
	float Pitch = 0, Yaw = 0, Roll = 0;
	uint32 Flags1 = 0, Flags2 = 0;

	/** On the difficulty whose FLAGS2 bit is Bit (0 = every difficulty); no difficulty bits = on all of them. */
	bool OnDifficulty(uint32 Bit) const { const uint32 D = Flags2 & 0xF0000000u; return Bit == 0 || D == 0 || (D & Bit) != 0; }
};

class OUTLAWSRUNTIME_API FOutlawsLevel
{
public:
	FString LevelName;
	TArray<FString> Textures;      // as listed (e.g. GFPLANKV.PCX, GW064WIN.ATX)
	FVector2D Parallax = FVector2D(1024.0, 1024.0);
	TArray<FString> Palettes;   // PALETTES: the PCX files whose palettes sectors pick (olpal, uwater...)
	TArray<FString> Cmaps;      // CMAPS: their colour maps, PCX images 256 wide whose rows are the light levels (REQ-FIREBRIGHT)
	FString Music;              // MUSIC: the level's music script (TOWNEX.MSC; NULL for none)
	TArray<FOutlawsSector> Sectors;
	TArray<FOutlawsObject> Objects;

	bool ParseLvt(const FString& Text);
	bool ParseObt(const FString& Text);

	const FOutlawsSector* Sector(int32 Index) const { return Sectors.IsValidIndex(Index) ? &Sectors[Index] : nullptr; }
	const FOutlawsSector* SectorById(const FString& Id) const;

	/**
	 * Sector containing map point (x, z); with stacked sectors, the one whose floor is the highest
	 * at or below y (else the lowest).  Null when the point is outside every sector.
	 */
	const FOutlawsSector* SectorAt(double X, double Y, double Z, const TBitArray<>* Skip = nullptr) const;
	/** SectorAt with each sector's plan box (x, z) given, Boxes[i] for Sectors[i] (the vertices as they are now): a sector
	 *  whose box misses the point is passed over before its rings are walked. Same pick as SectorAt. */
	const FOutlawsSector* SectorAtBoxed(double X, double Y, double Z, const TArray<FBox2D>& Boxes) const;
	/** The same with an Outlaws point (x, y, z), as FOutlawsGeometry::ToOutlaws gives it. */
	const FOutlawsSector* SectorAt(const FVector& P, const TBitArray<>* Skip = nullptr) const { return SectorAt(P.X, P.Y, P.Z, Skip); }
	const FOutlawsSector* SectorAtBoxed(const FVector& P, const TArray<FBox2D>& Boxes) const { return SectorAtBoxed(P.X, P.Y, P.Z, Boxes); }
	/** Whether the sector's floor plan holds (X, Z): inside its outer ring and outside its holes. */
	bool ContainsXZ(const FOutlawsSector& S, double X, double Z) const;
	/** The other rule, kept apart on purpose: the even-odd count over all the sector's walls at once (outer ring and
	 *  holes alike, in any order, no rings worked out). It differs from ContainsXZ where rings overlap or a wall is
	 *  stray, so the callers that used it keep it. */
	static bool InsideWalls(const FOutlawsSector& S, double X, double Z);

	/** Floor/ceiling height at a map point, honouring slopes (viewer's GetSurfaceYAtPoint). */
	double SurfaceY(const FOutlawsSector& S, const FVector2D& Pt, bool bFloor) const;
	/** V1, the Jedi engine's sector_which3D: of the sectors whose outline holds (X, Z) and whose floor..ceiling holds Y
	 *  (the feet; half a unit of step either way), the one with the smallest footprint; else SectorAt's pick.  -1 none.
	 *  Skip: sectors left out (BUG-MILLCLIMB: a zone gate's far side, switched off for the player). */
	int32 Which3D(double X, double Y, double Z, const TBitArray<>* Skip = nullptr) const;
	int32 Which3D(const FVector& P, const TBitArray<>* Skip = nullptr) const { return Which3D(P.X, P.Y, P.Z, Skip); }
	/** (BUG-JUMPPICK) the sector the feet stand in: of the sectors whose outline holds (X, Z) and whose floor..ceiling holds Y
	 *  (half a unit either way), the walled one whose floor is nearest the feet, the smaller on a tie; the sky round a
	 *  level only when nothing else holds them; else SectorAt's pick.  A catwalk, pier or tunnel floor running through a
	 *  bigger room's air is the one stood on, where Which3D takes whichever is smaller.  -1 none. */
	int32 Floor3D(double X, double Y, double Z, const TBitArray<>* Skip = nullptr) const;
	/** V1, the wall-cross list: from sector From, the move A -> B (plan) crosses the walls it crosses, adjoin after adjoin
	 *  (a double adjoin by the height Y); the sector it ends in when that holds B at height Y (or its vertical adjoin does);
	 *  -2 when the move does not explain where it ended (a solid wall in the way, a lift, a mantle): Which3D then. */
	int32 TrackMove(int32 From, const FVector2D& A, const FVector2D& B, double Y, const TBitArray<>* Skip = nullptr) const;
	/** Whether Skip leaves sector Si out. */
	static bool Skipped(const TBitArray<>* Skip, int32 Si) { return Skip && Si >= 0 && Si < Skip->Num() && (*Skip)[Si]; }
	/** A sloped floor's (ceiling's) own frame: along its hinge wall and the normal SurfaceY measures from it. */
	bool SlopeFrame(const FOutlawsSector& S, bool bFloor, FVector2D& OutAlong, FVector2D& OutNormal, FVector2D* OutOrigin = nullptr, double* OutDegrees = nullptr) const;
	/** The frame a floor (ceiling) texture is laid in: flat, world x/z from the origin; sloped, along the hinge wall
	 *  and up the incline from the hinge's first vertex, the incline measured along the surface (OutStretch per unit
	 *  across).  The original ties a sloped surface's texture to its slope (user 2026-09-29). */
	void SurfaceTexFrame(const FOutlawsSector& S, bool bFloor, FVector2D& OutOrigin, FVector2D& OutAlong, FVector2D& OutNormal, double& OutStretch) const;

	/** Rings in wall order; a ring may revisit a vertex (keyhole). */
	static void LoopsFromWalls(const FOutlawsSector& S, TArray<TArray<int32>>& OutRings);
	/** Same rings plus, per ring, the wall index of each edge. */
	static void LoopsFromWallsEx(const FOutlawsSector& S, TArray<TArray<int32>>& OutRings, TArray<TArray<int32>>& OutRingWalls);
	static double RingArea(const FOutlawsSector& S, const TArray<int32>& Ring);

	/** Sector light level 0..31 (+ wall light offset). */
	static int32 LightLevel(int32 Ambient, int32 WallLight = 0);
	/** Texture stem (upper case, no extension) for a texture index, or DEFAULT. */
	FString TexStem(int32 Index) const;

private:
	/** Each sector's rings, outer (largest) first then its holes, worked out once: SectorAt and ContainsXZ used to
	 *  rebuild them from the walls on every call, for every sector (the Quest's costliest game-thread work).  INF
	 *  moves vertices but never changes which ones a wall joins, so the rings hold for the level's life. */
	mutable TArray<TArray<TArray<int32>>> RingCache;
	const TArray<TArray<int32>>* CachedRings(const FOutlawsSector& S) const;
};
