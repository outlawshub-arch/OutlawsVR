#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "OutlawsSpriteActor.generated.h"

class UStaticMeshComponent;
class UMaterialInstanceDynamic;
class UTexture;
class UProceduralMeshComponent;

/** One NWX choreography in a sprite's atlas (characters carry stand, walk, shoot, hit, die...). */
USTRUCT()
struct FOutlawsSpriteChor
{
	GENERATED_BODY()
	UPROPERTY() int32 Dirs = 1;
	UPROPERTY() int32 Frames = 1;
	UPROPERTY() float FPS = 0.f;
	UPROPERTY() int32 GridStart = 0;   // into AOutlawsSpriteActor::ChorGrid
	UPROPERTY() int32 Length = 1;      // frames a one-shot plays before it holds its last
	UPROPERTY() int32 HeightPx = 0;    // tallest frame it shows
	/** Opcodes of the first row: (frames shown before it, opcode, arg1, arg2); EVENT is -5 1 n,
	 *  n = 1 fire, 3 footstep. */
	TArray<FIntVector4> Events;
};

/**
 * An Outlaws object sprite: a vertical quad (pivot at the bottom centre) that turns about its
 * Z axis to face the viewer every frame, like the original engine's cylindrical billboards,
 * and shows the atlas cell for the direction the viewer sees it from.
 *
 * The quad mesh is SM_SpriteQuad (1 unit wide, 1 unit tall, normal along +X); the actor scale
 * (1, cell width cm, cell height cm) sizes it.  The material is M_OL_Sprite (or an instance of
 * it) with the parameters Tex, Brightness, UVScale and UVOffset; the actor drives them on its
 * own dynamic instance.  Grid holds Directions x FrameCount atlas cell indices (row-major by
 * direction), as built from the NWX choreography.
 *
 * Moved from the OutlawsUE module on 2026-09-26 (CoreRedirect in DefaultEngine.ini).
 */
UCLASS(Blueprintable)
class OUTLAWSRUNTIME_API AOutlawsSpriteActor : public AActor
{
	GENERATED_BODY()

public:
	AOutlawsSpriteActor();

	/** The actor's place: the object's foot point on the floor (the quad hangs under it, see SetFeet). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Outlaws")
	TObjectPtr<USceneComponent> Root;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Outlaws")
	TObjectPtr<UStaticMeshComponent> Quad;

	/**
	 * Stand on the frames' insertion points (user 2026-10-02: sprites floated, the horse most): per atlas cell the
	 * rows of the cell and of the drawing under the insertion point (FSpriteAtlas::BelowPx / FootPx, 1x px of a
	 * CellH tall cell).  The quad is lowered so the insertion point is on the actor's place, as the original draws
	 * it, and the drawing under the floor (the near feet) is shown by drawing the whole quad nearer the eye, scaled
	 * about the eye so the picture is the same (up to MaxFootPullCm); past that it is raised onto the floor.  Only
	 * while the sprite stands facing the viewer; empty arrays (hanging things, effects): the cell's bottom on the
	 * actor's place as before.
	 */
	void SetFeet(const TArray<int16>& BelowPx, const TArray<int16>& FootPx, int32 CellH);
	/** R4: and across (FSpriteAtlas::SidePx2, of a CellW wide cell): the quad moved sideways too, so the insertion
	 *  point (not the drawing's middle) is on the actor's place and a wide frame stays put as the view turns. */
	void SetFeetAcross(const TArray<int16>& SidePx2, int32 CellW);
	/** Rows (1x) the shown cell's drawing reaches under its insertion point; 0 when no feet are set. */
	int32 GetFootPx() const { return CellFootPx.IsValidIndex(CurrentCell) ? CellFootPx[CurrentCell] : 0; }
	static constexpr double MaxFootPullCm = 60.0;
	/** (BUG-ENEMYWALLCLIP) the drawn-nearer quad stops this far short of a wall toward the eye (its half width turned). */
	static constexpr double PullWallMarginCm = 15.0;
	/** The floor is this far under the actor's place (a walking bad guy's capsule rides 2 cm over the floor, more on a
	 *  slope): the insertion point goes down onto it. */
	void SetFloorGap(float Cm) { FloorGapCm = FMath::Clamp(Cm, 0.f, 30.f); }
	/** (BUG-SHOVELWHACK) How tall the drawing of what plays now stands over its insertion point, in cm: its choreography's
	 *  tallest frame (16 px an Outlaws unit, as a bad guy's body takes it from the frame on show; a crouch is short), never
	 *  over the cell itself. */
	double DrawnHeightCm() const;
	/** (BUG-SHOVELWHACK) A blow's point in this picture as it is drawn: inside the quad where PlaceQuad put it (drawn nearer
	 *  the eye and scaled down for the near feet), a little inside its sides, within Reach of its plane, from just under the
	 *  insertion point up to DrawnHeightCm.  False while it is hidden. */
	bool DrawnTouches(const FVector& At, double Reach) const;
	/** -OLAnchorShots (with -OLShots): close, low shots of the horse and other floor sprites, ol_anchor_<name>_<tag>.png
	 *  (-OLAnchorTag=, default "after"), and a log line per floor sprite (floor, insertion point, lowest drawn row). */
	static void TickAnchorShots(UWorld* World, float DeltaSeconds);

	/** OBT object name, e.g. BARREL05 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws")
	FString OutlawsName;

	/** Original OBT yaw in degrees. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws")
	float OutlawsYaw = 0.f;

	/** Sector light level 0..31 the object sits in. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws")
	int32 LightLevel = 31;

	/** The sector light where the sprite now stands (a walking bad guy crossing into a lit sector). */
	void SetLightLevel(int32 Level);

	/** A 3D stand-in drawn instead of the quad (the sprite-to-3D items and barrels), or null. */
	UPROPERTY(Transient)
	TObjectPtr<class UPrimitiveComponent> Model;

	/** Portal culling (2026-10-01): the level publishes which sectors the eye's portal flood reaches; a sprite (and
	 *  whatever hangs on it: its 3D model, a grab halo) in a sector it does not reach, and more than CullSafeCm from
	 *  the eye, is hidden in game.  Its clock still runs (props fire their chor events), collision and AI are untouched.
	 *  Occlusion queries are off on the Quest (the GPU hang), so this is the only thing keeping sprites behind walls
	 *  out of the draw list. */
	static void SetCulling(bool bOn, const TBitArray<>* Visible, const FVector& Eye, TFunction<int32(int32, const FVector&, const FVector&)> Track);
	int32 HomeSector = -1;                  // (V1) the OB's own sector, its first membership (the level sets it at spawn)
	int32 DebugCullSector() const { return CullSector; }
	bool DebugCulled() const { return bCulled; }
	/** (BUG-STUTTER2 item 6) A move kept back while the sprite is culled (a scrolling floor carries it out of sight): put on
	 *  when it is drawn again, or when its place is next asked for (FlushHeldMove). Its cull follows the place with it. */
	void HoldMove(const FVector& By) { HeldMove += By; }
	void FlushHeldMove() { if (!HeldMove.IsZero()) { const FVector By = HeldMove; HeldMove = FVector::ZeroVector; AddActorWorldOffset(By); } }
	static int32 NumCulled;
	/** The model's sprite stem (BOTTLE01...) and its world_scale: the sprite's size over the real thing's. */
	FString ModelStem;
	float ModelScale = 1.f;
	/** The model shown (the quad hidden). */
	void ShowModel(class UPrimitiveComponent* InModel);
	/** Back to the sprite for good (a prop's reaction: the barrel breaks as the game drew it). */
	void ShowSprite();
	/**
	 * CPU1 object LOD (user 2026-10-04): the 3D model near, the game's own sprite further off, nothing far away. Past
	 * SpriteAtCm (from the eye) the model dithers out (M_OLR_ItemModel's LodFade, the primitive's custom data 0) while
	 * the sprite fades in over a band of a few metres; past GoneAtCm the sprite fades out (never for bLodKeep: keys,
	 * story and mission items). Both distances are scaled by the Graphics page's Object Detail (and the Quest 2 profile).
	 * SpriteAtCm 0: no LOD (the model always). ol.ObjectLod 0 or -OLNoObjectLod: off everywhere.
	 */
	void SetLod(float SpriteAtCm, float GoneAtCm);
	bool bLodKeep = false;
	static int32 NumLodSprites;               // (the census: models drawn as their sprite last frame)
	static int32 NumLodGone;                  // (and faded out)
	static float LodMaxStep;                   // (-OLLodSweep) the largest one-frame change of any model's fade or sprite's alpha
	/** See-through by Alpha (1 solid .. 0 gone): the first call moves the quad onto M_OLR_SpriteFade, a translucent
	 *  copy of the sprite material (flying blood, dirt and glass fading as they spread; user 2026-09-28). */
	void SetFade(float Alpha);

	/** Turn to face the viewer each frame (yaw only). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws")
	bool bFaceViewer = true;

	/** Atlas texture set on the actor's material instance ("Tex"); null keeps the material's own. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|Atlas")
	TObjectPtr<UTexture> SpriteTexture;

	/** The original atlas (set the first time the look changes). */
	UPROPERTY(Transient)
	TObjectPtr<UTexture> SpriteTextureLo;

	/** Show this atlas instead (the remaster's HD one), or the original again with null. */
	void SetLook(UTexture* Hd);

	/** REQ-GRABUPSCALE: while a bad guy is held, these cells show a picture of their own (the 2x upscale of the HD frame,
	 *  the whole texture over the cell's quad) instead of their part of the atlas; any other cell shows the atlas as before.
	 *  Cleared on the let-go. */
	void SetHeldLook(int32 Cell, UTexture* Tex);
	void ClearHeldLook();
	bool HasHeldLook() const { return HeldLook.Num() > 0; }
	/** The held look's texture on show now (the cell shown has one), or null. */
	UTexture* HeldLookShown() const { return bHeldLookShown ? HeldLook.FindRef(CurrentCell).Get() : nullptr; }
	/** The texture on the quad's material now (tests). */
	UTexture* DebugQuadTexture() const;
	UPROPERTY(Transient)
	TMap<int32, TObjectPtr<UTexture>> HeldLook;

	// ---- atlas / choreography -------------------------------------------------------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|Atlas")
	int32 AtlasCols = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|Atlas")
	int32 AtlasRows = 1;

	/** Number of view directions in the choreography (32 for characters, 1 for props). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|Atlas")
	int32 Directions = 1;

	/** Animation frames per direction. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|Atlas")
	int32 FrameCount = 1;

	/** Choreography frame rate; 0 = static. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|Atlas")
	float FPS = 0.f;

	/** Directions x FrameCount atlas cell indices, row-major by direction. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|Atlas")
	TArray<int32> Grid;

	/** Added to the computed direction index (tuning the WAX view convention). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|Atlas")
	int32 DirectionOffset = 0;

	/**
	 * Direction index grows clockwise (seen from above) around the object.  Outlaws WAX views run
	 * counter-clockwise: view 8 is the profile seen from the object's own left side (it faces
	 * screen-left in the art), so the default is false (verified 2026-09-26 on SANCHEZ, BGY3, MAN1).
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|Atlas")
	bool bClockwiseDirections = false;

	/** Added to the object's facing yaw (degrees, UE space) when mapping OBT yaw to UE. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|Atlas")
	float FacingYawOffset = 0.f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Outlaws|Atlas")
	int32 CurrentDirection = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Transient, Category = "Outlaws|Atlas")
	int32 CurrentCell = -1;

	virtual void Tick(float DeltaSeconds) override;
	virtual void OnConstruction(const FTransform& Transform) override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	/** The frame shown now (texture and atlas cell) onto another material: the grab glow's halo follows it. */
	void CopyLookTo(class UMaterialInstanceDynamic* M) const;
	virtual bool ShouldTickIfViewportsOnly() const override { return true; }
	/** Game worlds: every sprite in one loop a frame (a tickable driver), not one actor tick each: 321 of them cost the
	 *  Quest ~2 ms with the engine's per-tick overhead, and each looked the viewer up again. */
	static void TickAll(UWorld* World, float DeltaSeconds);

	/** The object's facing direction as a UE yaw in degrees. */
	UFUNCTION(BlueprintCallable, Category = "Outlaws")
	float GetFacingYaw() const;

	/** Force a specific atlas cell (e.g. from Blueprint logic); pass -1 to resume automatic selection. */
	UFUNCTION(BlueprintCallable, Category = "Outlaws")
	void SetForcedCell(int32 Cell) { ForcedCell = Cell; CurrentCell = -1; }

	/** Every choreography packed in the atlas, and their cells (Dirs x Frames each from GridStart). */
	UPROPERTY(Transient)
	TArray<FOutlawsSpriteChor> Chors;

	UPROPERTY(Transient)
	TArray<int32> ChorGrid;

	/** Play a choreography from its start (-1: back to the atlas default); a one-shot holds its last frame. */
	UFUNCTION(BlueprintCallable, Category = "Outlaws")
	void PlayChor(int32 Chor, bool bLoop);

	UFUNCTION(BlueprintCallable, Category = "Outlaws")
	int32 GetPlayingChor() const { return PlayingChor; }
	/** The playing choreography repeats (co-op sends it with the enemy's state). */
	bool IsChorLooping() const { return bChorLoop; }

	/** Frames shown so far in the playing choreography (keeps counting past a one-shot's end). */
	UFUNCTION(BlueprintCallable, Category = "Outlaws")
	int32 GetChorFrame() const;

	/** A one-shot choreography has shown all its frames. */
	UFUNCTION(BlueprintCallable, Category = "Outlaws")
	bool IsChorDone() const;

	/** Straight to the end of the playing one-shot (a body put back by a save lies where it fell). */
	void FinishChor() { AnimTime = 1000.f; CurrentCell = -1; }
	/** BUG-DRDEATHANIM: the frame on show kept (the choreography's clock stopped) until the next PlayChor; the view still
	 *  turns with the viewer. */
	void HoldClock() { bClockHeld = true; }
	bool IsClockHeld() const { return bClockHeld; }

	/** Drawn over the level (M_OLR_SpriteOnTop's near pull) while the player holds it, or back to normal. */
	void SetOnTop(bool bOn);
	/** Held in a hand, set each frame (user 2026-10-02): the quad faces FaceYaw (UE yaw toward the eye) and the view
	 *  direction is picked as if seen from there, whatever the eye's own place.  A man pulled to the chest is a hand's
	 *  width from the eyes, where the angle to them swung across whole view directions with every small move (his frame
	 *  flickered, he seemed to turn).  It lapses a frame after the last call. */
	void HoldView(float FaceYaw) { HeldFaceYaw = FaceYaw; HeldViewFrame = GFrameCounter; }
	/** REQ-HELDSWAY (OutlawsSpriteSwing.cpp): held and swung, the quad's two bottom corners lag the grip on a damped spring
	 *  (world X and Y only, never Z) and the bottom edge turns about its middle by the part of the lag toward or away from the
	 *  viewer; the top stays at the hand.  Set each frame by the holder's tick; off, the spring is reset at once (a drop or a
	 *  throw never stretches him).  The ol.heldsway.* console variables tune it. */
	void SetSwingDriven(bool bOn);
	/** The lag now (cm, horizontal) and the bottom edge's turn (degrees): for the test's log. */
	FVector SwingOffset() const { return SwingOff; }
	float SwingYaw() const { return SwingYawDeg; }
	/** The bottom corners as last drawn and where they would be unswung (world): false when the plain quad shows. */
	bool SwingBottom(FVector& OutL, FVector& OutR, FVector& OutRestL, FVector& OutRestR) const;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> OffTopBase;

private:
	int32 PlayingChor = -1;
	bool bChorLoop = true;
	bool bClockHeld = false;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> MID;

	int32 ForcedCell = -1;
	float AnimTime = 0.f;
	float HeldFaceYaw = 0.f;               // HoldView's facing, and the frame it was last set in
	uint64 HeldViewFrame = 0;
	bool bFadeMaterial = false;
	// REQ-HELDSWAY (OutlawsSpriteSwing.cpp)
	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> SwingMesh;
	bool bSwingDrive = false, bSwingHavePrev = false, bSwingShown = false;
	FVector SwingOff = FVector::ZeroVector, SwingVel = FVector::ZeroVector, SwingPrevGrip = FVector::ZeroVector;
	FVector SwingDrawn[4];                 // the bottom corners drawn (left, right) and unswung (left, right), world
	float SwingYawDeg = 0.f, SwingPeak = 0.f;  // (the hold's farthest lag, for its log line at the let-go)
	void UpdateSwing();
	void EndSwing();
	void ShowSwingMesh(bool bOn);

	bool GetViewerLocation(FVector& OutLocation) const;
	/** One frame's facing and animation; Viewer is null when there is none to face. */
	void UpdateSprite(float DeltaSeconds, const FVector* Viewer);
	bool bDriven = false;                  // in the driver's loop (its own actor tick is off)
	bool EnsureMID();
	void ApplyCell(int32 Cell);
	bool bHeldLookShown = false;           // (REQ-GRABUPSCALE) the MID shows a held-look texture, not the atlas
	/** The quad's place for the cell shown and the viewer (SetFeet). */
	void PlaceQuad(const FVector* Viewer);
	TArray<int16> CellBelowPx;
	TArray<int16> CellFootPx;
	int32 CellPxH = 0;
	TArray<int16> CellSidePx2;
	int32 CellPxW = 0;
	float FloorGapCm = 0.f;
	bool bQuadMoved = false;               // the quad off the actor's place (lowered, drawn nearer, scaled)
	// (BUG-ENEMYWALLCLIP) how far the quad may be drawn nearer before a wall toward the eye: traced from PullClearAt
	// along PullClearDir, again only when the place moves 5 cm or the eye's bearing 2 degrees
	double PullClearCm = -1.0;
	FVector PullClearAt = FVector::ZeroVector;
	FVector2D PullClearDir = FVector2D::ZeroVector;
	double PullClearance(const FVector& At, const FVector2D& Dir);
	int32 DirectionFor(const FVector& Viewer) const;
private:
	int32 CullSector = -2;                  // -2 not looked up yet, -1 outside every sector (never culled)
	FVector CullPos = FVector::ZeroVector;
	float CullHold = 0.f;                   // a sprite out of sight stays drawn this long (no flicker at an opening's edge)
	bool bCulled = false;
	FVector HeldMove = FVector::ZeroVector;
	/** True when the sprite is culled this frame (its clock advanced, nothing else to do). */
	bool UpdateCull(float DeltaSeconds);
	void AdvanceClock(float DeltaSeconds);
	// (CPU1 object LOD) the distances, what was last applied, and the quad's two looks (solid, and fading)
	float LodSpriteAt = 0.f;
	float LodGoneAt = 0.f;
	float LodModelFade = 0.f;               // applied to the model: 0 solid .. 1 gone (hidden)
	float LodSpriteAlpha = 0.f;             // applied to the quad: 0 hidden .. 1 the sprite as drawn without LOD
	float LodFApplied = 0.f;                // the model's fade last applied, 1 = hidden
	uint64 LodFrame = 0;                    // the frame it was last updated (LodMaxStep counts frame-to-frame steps only)
	bool bLodActive = false;                // some LOD state applied (to undo when it is switched off)
	bool bLodQuadFade = false;              // the quad on its fading look
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> LodSolidMID;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> LodFadeMID;
	void UpdateLod(const FVector& Viewer, float Scale);
	void ApplyLod(float ModelFade, float SpriteAlpha);
	bool LodQuadLook(bool bFade);
};
