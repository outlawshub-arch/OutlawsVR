#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "OutlawsTypes.h"
#include "AudioMixerBlueprintLibrary.h"
#include "OutlawsGlyphs.h"
#include "OutlawsPlayer.generated.h"

class UCameraComponent;
class UMotionControllerComponent;
class UOutlawsBoxHand;
class UTextRenderComponent;
namespace OutlawsMenu { class FSource; struct FPage; struct FItem; }
class AOutlawsLevelActor;
class FOutlawsAssets;
namespace OutlawsObj { struct FObjMesh; }
namespace OutlawsHeldClash { struct FState; }
struct FOutlawsThrownLook;

/**
 * The Outlaws player, for VR (OpenXR; test rig Quest 3 over Meta Quest Link) and for desktop
 * (mouse and keyboard) when no headset is on.  Shared: Dark Forces style step-up (3.5 units),
 * crouch and crouch-jump (a crouch in mid-air lifts the feet), swimming in water sectors, the INF
 * use action.  VR adds room-scale head following, comfort locomotion and turning, floating box
 * hands that curl with grip and trigger, grabbing ledges to pull up and climb (also mid-air), and
 * a wrist read-out.  Reference numbers: TFE_DarkForces/player.cpp (height 5.8 units, step 3.5,
 * jump impulse 43.5 units/s, gravity 150 units/s^2, crouch speed 11.4 units/s; 1 unit = 30 cm).
 *
 * VR, right-handed (left-handed mirrors):
 *   left stick      move (toward the head or the left hand); click = run
 *   right stick     left/right turn (snap or smooth: bSnapTurn); up = jump; held down = crouch
 *                   (as deep as it is held); click = crouch toggle (seated play)
 *   X / A (hold)    the wheel, in front of the eyes: weapons on the inner ring, carried items on
 *                   the outer.  Each hand holding its button has a laser; letting go takes what
 *                   that laser points at (a weapon into that hand, an item used).  Holding both
 *                   buttons keeps one wheel with two lasers.  Game time runs at half speed.
 *   trigger (gun)   fire; revolver: swipe the other hand past the hammer to fan it (a revolver
 *                   in that hand is put away while it fans); Y / B reload that hand's gun (the
 *                   Classic layout, Options > VR: a tap of X / A instead; its top buttons then
 *                   jump on the dominant side and use the medical bag on the other).
 *                   Reloading is a button, never a gesture (user 2026-09-27).
 *                   Only revolvers pair up: a long gun takes both hands
 *   medical bag     from the wheel's outer ring (no button of its own)
 *   grip            curl fingers; the first that applies: take an item (let go at the chest to
 *                   keep it, elsewhere to drop it), use a door or switch the hand touches,
 *                   hold a ledge (pull down to climb, let go to drop or swing; let go with
 *                   the hand at mid-torso or lower and you climb onto it, or over a fence)
 *   trigger         curl the index finger
 *   duck for real   the body is as tall as the head is high: low enough, and any gap is passable
 *   raise off wrist health, armour, medical bag, gold and items
 *   push a door     a hand moving into a closed door opens it, no button
 * Desktop: WASD, mouse, Space jump, C or Ctrl crouch (hold), Shift run, E use.
 */
UCLASS()
class OUTLAWSRUNTIME_API AOutlawsPlayer : public ACharacter
{
	GENERATED_BODY()

public:
	AOutlawsPlayer(const FObjectInitializer& ObjectInitializer);
	friend class AOutlawsMovieScreen;            // (user 2026-10-02: the level's end panels are drawn in the menus' fonts, KpfFont)
	friend class UOutlawsStats;                  // (the panels' tests fire the gun and throw a bad guy as the player does)
	friend class UOutlawsCoop;                   // (M4: spectating reads the grips; the tests press the sticks)
	friend struct FFragTableArt;                 // (FT: the frag table's art and fonts, KpfArt and KpfFont)
	friend class AOutlawsJoinBubble;             // (R11: the join bubble's lasers, triggers and the menus' art)
	friend class AOutlawsTutorialCard;           // (REQ-TUTORIAL2: the tutorial's card, in the bubble's look)
	friend class UOutlawsBeacon;
	friend class AOutlawsLevelActor;              // (BUG-STUTTER2: -OLStutterProbe times the scope rig)

	// ---- settings -------------------------------------------------------------------------------
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|VR")
	EOutlawsHand DominantHand = EOutlawsHand::Right;

	/** Stick forward = where the head looks, or where the off (non-dominant) hand points. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|VR")
	EOutlawsMoveDirection MoveDirection = EOutlawsMoveDirection::Head;

	/** VR speeds (cm/s).  User 2026-09-26: Dark Forces speeds in VR too, toned down later if too
	 *  much.  Dark Forces: velocity capped at 32 units/s (960 cm/s), running doubles it. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|VR")
	float VRWalkSpeed = 960.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|VR")
	float VRRunSpeed = 1920.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|VR")
	float VRCrouchSpeed = 480.f;

	/** VR jump: a gentle hop; high ledges are for hand climbing. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|VR")
	float VRJumpVelocity = 745.f;       // the Dark Forces jump height (6.3 units, 189 cm) under the gentler VR gravity

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|VR")
	bool bSnapTurn = true;
	/** VR options: teleport instead of the stick's smooth walk (user 2026-09-28; smooth stays the default). */
	bool bTeleportMove = false;
	bool bMenuRebuild = false;          // the open menu page to be built again next frame (its rows changed)
	FString PauseTimeShown;             // REQ-LEVELTIMER: the pause page's time line as last drawn ("Time 12:34"; empty: none)

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|VR")
	float SnapTurnDegrees = 45.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|VR")
	float SmoothTurnSpeed = 90.f;

	/** Darken the view edges while moving (off: user 2026-09-26). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|VR")
	bool bComfortVignette = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|VR")
	float StickDeadZone = 0.2f;
	/** VR comfort (ol.vr.MoveSmoothing): the move stick's push and the walk / run speed eased over a few frames, so
	 *  a stick's wobble, the head's sway and a run's start are not steps in velocity (Meta: keep accelerations brief). */
	FVector SmoothMove = FVector::ZeroVector;
	uint64 SmoothMoveFrame = 0;          // the frame it was last eased on: a gap (a climb, the teleport aim) starts it afresh
	float SmoothSpeed = 0.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|VR")
	float UseReach = 200.f;

	/** Eye height a recenter (both sticks clicked) puts the view at, whatever the real head height. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|VR")
	float VRStandingEyeHeight = 165.f;

	/** Hand-to-chest distance (cm) at which a held item counts as picked up when let go. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|VR")
	float ChestReach = 35.f;


	/** How fast (cm/s) a hand must move into a door, away from the eyes, to push it open without
	 *  the grip button; walking into it with a hand out counts. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|VR")
	float DoorPushSpeed = 35.f;

	/** Head height (cm above the play-space floor) below which the body crouches (desktop-style
	 *  crouch, only used by the mantle fallback; VR sizes the body to the head continuously). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|VR")
	float PhysicalCrouchHeight = 115.f;

	/** Desktop speeds (cm/s) and Dark Forces jump physics. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|Desktop")
	float DesktopWalkSpeed = 960.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|Desktop")
	float DesktopRunSpeed = 1920.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|Desktop")
	float DesktopCrouchSpeed = 480.f;

	/** Dark Forces PLAYER_FORWARD_SPEED 256 units/s^2: full speed in about a tenth of a second. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws")
	float Acceleration = 7680.f;

	/** VR in the air, in water and on slippery floors: a gentler start (user 2026-09-27, comfort): walking
	 *  speed in about 0.3 s, running in 0.6.  On a gripping floor VR uses VRInstantAcceleration instead. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|VR")
	float VRAcceleration = 3200.f;

	/** VR on a gripping floor: acceleration and braking both, high enough that walking, running and
	 *  stopping switch within a frame.  The platform comfort guidance (Meta: fixed speeds switched
	 *  instantly, no ramps) and Pavlov (2,000,048 / 16,000) do the same; user 2026-09-29. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|VR")
	float VRInstantAcceleration = 100000.f;

	/** VR: move this game's sound output to the headset's audio device (name contains this). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|VR")
	FString HeadsetAudioDevice = TEXT("Oculus Virtual Audio");

	/** Private placeholder guns (never shipped): folder with python/ and ruger/ OBJs; empty means
	 *  <the local item folder>/ve_weapons/hd_xint (FOutlawsAssets::PlaceholderDir). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|Weapons")
	FString PlaceholderWeaponDir;

	/** 43.5 units/s with gravity 150 units/s^2 (4.59 g): a 6.3 unit (189 cm) Dark Forces jump. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|Desktop")
	float DesktopJumpVelocity = 1305.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|Desktop")
	float DesktopGravityScale = 4.59f;

	/** Aim assist: a trigger shot within BoreCone degrees of the bad guy nearest the middle of the view
	 *  (within ViewCone of it) goes to him; a fanned shot takes that bad guy within FanViewCone. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|VR")
	float AimAssistViewCone = 15.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|VR")
	float AimAssistBoreCone = 6.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|VR")
	float FanViewCone = 30.f;

	/** Swimming as a share of the land speed (walk or run): Outlaws WATER.PHY MAX_VEL 15 against
	 *  DEFAULT.PHY 30 units/s, so half. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws")
	float SwimSpeedFraction = 0.5f;

	/** Dark Forces PLAYER_STEP: floors up to 3.5 units higher are walked onto. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws")
	float StepHeight = 105.f;

	// ---- components -----------------------------------------------------------------------------
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Outlaws")
	TObjectPtr<USceneComponent> VROrigin;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Outlaws")
	TObjectPtr<UCameraComponent> Camera;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Outlaws|VR")
	TObjectPtr<UMotionControllerComponent> LeftGrip;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Outlaws|VR")
	TObjectPtr<UMotionControllerComponent> RightGrip;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Outlaws|VR")
	TObjectPtr<UMotionControllerComponent> LeftAim;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Outlaws|VR")
	TObjectPtr<UMotionControllerComponent> RightAim;

	/** The hand's own frame under each grip pose: fingers along its X.  Rotated at runtime so X
	 *  follows the controller's aim (pointing) direction, whatever axes the runtime's grip uses. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Outlaws|VR")
	TObjectPtr<USceneComponent> LeftHandFrame;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Outlaws|VR")
	TObjectPtr<USceneComponent> RightHandFrame;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Outlaws|VR")
	TObjectPtr<UOutlawsBoxHand> LeftHand;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Outlaws|VR")
	TObjectPtr<UOutlawsBoxHand> RightHand;

	/** Floating read-out behind the off hand (no arm or wrist model), shown when raised into view. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Outlaws|VR")
	TObjectPtr<UTextRenderComponent> WristText;
	// the wrist display as icons (user 2026-09-27): the original HUD's heart and plate cards, the remaster's wheel
	// cards for the medical bag and every key, numbers beside them; drawn over everything (M_OLR_WristIcon /
	// M_OLR_WristText, no depth test) so the hand in front never hides them
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> WristRoot;
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStaticMeshComponent>> WristIcons;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTextRenderComponent>> WristNumbers;
	struct FWristIconSrc { UTexture* Tex = nullptr; FVector2D Scale = FVector2D(1.0, 1.0); FVector2D Offset = FVector2D::ZeroVector; float Aspect = 1.f; };
	TMap<FString, FWristIconSrc> WristIconCache;     // "nwx:intheart" / "kpf:idocbag" (+ the HD switch) -> its picture
	bool WristIcon(const FString& Key, FWristIconSrc& Out);
	class UStaticMeshComponent* WristIconPlane(int32 Index);
	void LayoutWrist();

	// ---- calls ----------------------------------------------------------------------------------
	UFUNCTION(BlueprintCallable, Category = "Outlaws")
	bool IsVR() const { return bVR; }

	UFUNCTION(BlueprintCallable, Category = "Outlaws")
	bool IsSwimming() const { return bSwimming; }
	/** (BUG-SURFACEJOLT) swimming and held at the surface (a current's downward part does not take it under) */
	bool IsSurfaceHeld() const { return bSwimming && bSurfaceLocked; }

	UFUNCTION(BlueprintCallable, Category = "Outlaws")
	bool IsClimbing() const { return GrabHand >= 0; }

	// ---- stuck rescue (BUG-STUCKGAP, user 2026-10-05; OutlawsPlayerUnstuck.cpp), single player and co-op ----
	/** Back to the last safe floor spot (else the middle of the last sector the head was in), through a blink of black.
	 *  Trigger: what asked ("head", "body", a test's word; REQ-NOUNSTUCK: no menu row asks any more); logged with the level
	 *  and both places. */
	void RequestUnstuck(const TCHAR* Trigger);
	/** The last spot the feet stood on a sector's floor with the head inside a sector and a standing body's room, still
	 *  clear now (re-checked at the floor as it is now); false when none. */
	bool LastSafeFeet(FVector& OutFeet) const;
	/** Tests: the head point the check reads is moved by this much (a head pushed through a wall). */
	FVector UnstuckTestHeadOffset = FVector::ZeroVector;
	/** Tests: the safe spots and the last rescues forgotten (the fallback then). */
	void ForgetSafeSpots() { SafeSpots.Reset(); UnstuckLastAt = -1.0; UnstuckStreak = 0; UnstuckTried.Reset(); }
	int32 UnstuckCount = 0;
	FString LastUnstuck;                  // the last rescue's log line (tests)

	/** VR: face forward, centre on the play space, and take the current head as standing height; the held gear is
	 *  re-seated in the hands too. Quiet: no "Recentered" notice (a level start). */
	UFUNCTION(BlueprintCallable, Category = "Outlaws")
	void Recenter(bool bQuiet = false);

	/** Re-seat what each hand really holds (the gun rebuilt on its seat with its rounds, poses cleared) and hide what an
	 *  empty hand shows: the fix for guns out of line with the controllers, or left showing after a level's movie. */
	void ResetHeldGear();

	/** The level is won: remember the hands' weapons for the next level (user 2026-09-28). */
	void StashHandsForNextLevel()
	{
		for (int32 H = 0; H < 2; ++H) { CarryHands[H] = Equipped[H]; CarryMag[H] = Equipped[H] >= 0 ? Mag[H] : -1; }
		bCarryHands = true;
		CarrySavedMag = SavedMag;            // (BUG-HOLSTERRELOAD: the holstered guns' rounds go along too, ResetForLevel clears them)
		bCarrySaved = true;
	}
	/** The next level has placed the player: take up the stashed weapons that are still carried. */
	void RestoreCarriedHands();
	/** (BUG-LEVELCARRY) a new level begins (the story's next level, never a save loaded): stamina full, and each gun in
	 *  hand keeps the rounds it had and is loaded to full from the carried ammunition as the reload does (as many as
	 *  there are).  Throwables are left as they are.  (BUG-HOLSTERRELOAD) the holstered guns the same way, after the hands. */
	void TopUpForLevelStart();
	/** (BUG-HOLSTERRELOAD) the rounds every gun holds as this level was entered (after the top-up), for a restart of it. */
	void SnapEntryMags();
	/** A restart of the level as it was entered: the guns hold what they held then (no gun comes back full for nothing). */
	void RestoreEntryMags();
	/** A level's opening as a save or a map load writes it: the carried guns' rounds ("carry_mags"), and whether they are
	 *  loaded from the carried ammunition when it is read (the next level) or are the entry's own (a restart). */
	void WriteCarryMags(class FJsonObject& O, bool bRestart) const;
	void ReadCarryMags(const class FJsonObject& O);
	/** A gun's rounds when full (0 outside the table). */
	static int32 WeaponCapacity(int32 Weapon);
	/** -OLShots -OLLevelCarryTest (BUG-LEVELCARRY, OutlawsLevelCarryTest.cpp): tired and part-loaded into the next level,
	 *  then a mid-level save loaded unchanged. */
	void LevelCarryTestTick();
	/** (the SP and co-op checks) tired, a rifle in the left hand and a revolver in the right with these rounds, these
	 *  bullets and cartridges carried; and this player's state as one line. */
	void LevelCarryArrange(float InStamina, int32 RifleRounds, int32 RevolverRounds, int32 Bullets, int32 Carts);
	FString LevelCarryState() const;
	/** (BUG-HOLSTERRELOAD checks) the double barrel and the sawed-off put away in the left hand with these rounds, the shells carried;
	 *  and the rounds a gun comes out with when drawn into a hand (the hand's own gun put back after). */
	void LevelCarryHolster(int32 DoubleRounds, int32 SawedRounds, int32 Shells);
	int32 LevelCarryDrawRounds(int32 Hand, int32 Weapon);

	/** Turn the player about their head. */
	UFUNCTION(BlueprintCallable, Category = "Outlaws")
	void TurnBy(float Degrees);

	/** Test hooks: hold the move stick / crouch as if the player did. */
	UFUNCTION(BlueprintCallable, Category = "Outlaws")
	void HoldMoveStick(FVector2D Stick, float Seconds);

	UFUNCTION(BlueprintCallable, Category = "Outlaws")
	void SetCrouchHeld(bool bHeld);
	/** A blast at Where (radius R cm): the player's thrown dynamite in it goes off a split second later, the chain's
	 *  next domino (user 2026-09-28). */
	void BlastNear(const FVector& Where, float R);
	void BlastThrow(const FVector& From, float Dealt);   // a blast's push on the body

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	/** (BUG-MILLCLIMB) a teleport is a jump for the level's zone gates: the side is decided again from where the body lands. */
	virtual void TeleportSucceeded(bool bIsATest) override;
	/** (BUG-WHARFPLANK) after a teleport where a standing body has no room under the roof: crouched to fit, if that fits. */
	void FitUnderRoofAfterTeleport();
	/** Fall damage (user 2026-09-27), by how hard the landing is. */
	virtual void Landed(const FHitResult& Hit) override;
	double FallGraceUntil = 0.0;         // no fall damage until then (world seconds): a level start or a respawn drops the player in
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual void OnStartCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust) override;
	virtual void OnEndCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust) override;

private:
	UPROPERTY(Transient)
	TArray<TObjectPtr<UObject>> KeepAlive;

	TWeakObjectPtr<AOutlawsLevelActor> Level;
	bool bVR = false;
	bool bSwimming = false;
	/** Breaststroke (user 2026-09-28; any direction 2026-10-02): in the water each hand swept past a stroke's speed
	 *  pushes the body the opposite way, in proportion; one hand alone is half the push. */
	void SwimStrokes(float Dt);
	bool bRunning = false;
	bool bCrouchHeld = false;          // desktop key / VR toggle
	bool bTurnLatched = false;
	bool bDesktopMoving = false;       // a move key was down this frame
	float Vignette = 0.f;
	float CameraEye = 160.f;           // desktop eye height above the feet, eased on crouch
	FVector2D HeldStick = FVector2D::ZeroVector;
	float HeldStickLeft = 0.f;
	FString WristLast;                 // what the wrist last showed (health and armour)
	FString WristShownKeys;            // (tests) its cards as laid out last ("nwx:intheart nwx:ibplate")
	int32 WristIconsUsed = 0;          // the cards the last layout placed; the rest stay hidden (BUG-WRISTPLATE)
	FString WristRowsLog;              // (tests) each row's cards and where they sit, as laid out last
	FVector SmokeStart = FVector::ZeroVector;
	float HeightOffset = 0.f;          // added to the tracked head height (recenter / eye-level fix)
	float TrainRock = 0.f;
	float UnderwaterBlend = 0.f;       // 0..1: the underwater tint eased in and out
	float BubbleWait = 2.f;            // seconds to the next breath of bubbles underwater
	FLinearColor UnderwaterFilter = FLinearColor::White;
	FLinearColor UnderwaterHue = FLinearColor(0.3f, 0.62f, 1.f);      // the water's surface colour, brightest channel 1             // 0..1: how much of the train level's view rock is in (eased in and out)
	bool bHeightChecked = false;
	float HeightCheckDelay = 0.6f;
	bool bPendingHeight = false;
	bool bStickHeld[2] = { false, false };
	double StickDownAt[2] = { 0.0, 0.0 };       // (M4: a long press of the move stick says a taunt)
	/** Both sticks kept in this long after the recentre chord: a perf mark (OutlawsPerf, "lag right now"). */
	float StickChordHeld = 0.f;
	bool bPerfMarked = false;
	void TickStickChordTest();
	bool bStickChord = false;
	float CrouchDrop = 0.f;            // VR stick crouch: the view's drop now (the full drop times how deep)
	float CrouchFull = 0.f;            // VR stick crouch: the full drop, captured when it began
	float CrouchAmount = 1.f;          // VR stick crouch: how deep, by how far down the stick is held (1 = a full crouch)
	bool bStandPending = false;        // stick crouch off, body still growing: keep the eye under its top
	float ShownDrop = 0.f;             // the view drop as shown: eased toward ViewDrop(), never a snap (comfort)
	bool bAutoCrouched = false;        // a climb into a low space put the crouch on: it comes off by itself with room
	float MantleDrop0 = -1.f;          // that crouch's view drop at the climb's start (it rides down with the climb)
	float ShownDropVel = 0.f;
	// a step up or down while walking (stairs, a ledge the body steps off): the view starts where it was and settles onto
	// the new floor height, critically damped (user 2026-10-02: the snap down was janky)
	float StepEase = 0.f;              // cm the view is shown off the body's true height, easing to 0
	float StepEaseVel = 0.f;
	FVector StepPrevAt = FVector::ZeroVector;     // the feet last frame
	FVector StepPrevBase = FVector::ZeroVector;   // what they stood on, where it was (a lift moving: no ease)
	TWeakObjectPtr<const UPrimitiveComponent> StepPrevBaseComp;
	bool bStepPrevGround = false;
	void UpdateStepEase(float Dt);
	bool bFrameAligned[2] = { false, false };
	FVector FrameSample[2];            // the aim-in-grip direction being measured; taken once it holds steady
	int32 FrameSteady[2] = { 0, 0 };
	bool bGripArmed[2] = { true, true };
	bool bVaulting = false;
	int32 PushDoor[2] = { -1, -1 };    // door sector a hand has pushed and is still touching
	int32 NudgedDoor = -1;             // last door sector a hand nudged (push or grip), with when
	double NudgedTime = -1.0;
	FVector HandWorldVel[2];           // hand motion through the world: what a push into a door is scored by
	FVector PrevHandWorld[2];
	TMap<FString, UMaterialInterface*> GunMaterials;   // kept alive by KeepAlive
	TEnumAsByte<EMovementMode> MenuSavedMode = MOVE_Walking;
	bool bHostPauseFrozen = false;                 // BUG-HOSTPAUSE: CoopHostPauseFreeze
	TEnumAsByte<EMovementMode> HostPauseSavedMode = MOVE_Walking;
	FVector MenuSavedVelocity = FVector::ZeroVector;
	// weapons (placeholders) and the wheel
	int32 Equipped[2] = { -1, -1 };    // weapon index per hand, -1 = bare hand
	bool bWheelOpen = false;
	bool bWheelHeld[2] = { false, false };   // hands holding their wheel button (each has a laser)
	int32 WheelPick[2] = { -1, -1 };         // slot each laser points at
	TArray<int32> WheelWeapon;               // per slot: weapon index, -1 bare hands, -2 an item
	TArray<FString> WheelItem;               // per slot: the item it uses
	UPROPERTY(Transient)
	TObjectPtr<class UProceduralMeshComponent> WeaponMesh[2];
	// the revolver's cylinder: its own mesh on a crane pivot, swung out while reloading and turned a
	// chamber per round (loaded or fired)
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> CylPivot[2];
	UPROPERTY(Transient)
	TObjectPtr<class UProceduralMeshComponent> CylMesh[2];
	float CylOut[2] = { 0.f, 0.f };         // 0 closed .. 1 swung out
	float CylSpin[2] = { 0.f, 0.f };        // degrees turned
	float CylSpinTarget[2] = { 0.f, 0.f };
	bool bReloadingNow[2] = { false, false };
	void UpdateCylinder(int32 Hand, float Dt);
	// the sprite revolver's rounds, one per chamber (brass head behind, lead nose in front) on the cylinder: shown
	// for the Mag chambers from the top in firing order (user 2026-09-28); index Hand * 12 + chamber * 2 + part
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStaticMeshComponent>> RoundParts;
	void BuildRounds(int32 Hand);
	void ShowRounds(int32 Hand, bool bShow);
	// the sprite guns' moving parts: hammer(s) and trigger on pivots (user 2026-09-28: animated when used)
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> HammerPivot[2];
	UPROPERTY(Transient)
	TObjectPtr<class UProceduralMeshComponent> HammerMesh[2];
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> TriggerPivot[2];
	UPROPERTY(Transient)
	TObjectPtr<class UProceduralMeshComponent> TriggerMesh[2];
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> LeverPivot[2];
	UPROPERTY(Transient)
	TObjectPtr<class UProceduralMeshComponent> LeverMesh[2];
	/** The rifle's telescope (item3d GSCOPE) on its receiver while the scope is carried (user 2026-09-29). */
	UPROPERTY(Transient)
	TObjectPtr<class UProceduralMeshComponent> ScopeMesh[2];
	/** The gatling set up in front of the player (user 2026-09-29): its tripod level on the floor, turning and moving
	 *  with the body; the gun on its pivot aimed by a hand gripping the handle behind the breech. */
	UPROPERTY(Transient)
	TObjectPtr<class UProceduralMeshComponent> GatTripod;
	/** The gatling's crank (user 2026-09-29): the other hand grips its handle and winds it, a shot every sixth of a
	 *  turn; the trigger's shots turn it too. */
	UPROPERTY(Transient)
	TObjectPtr<class UProceduralMeshComponent> GatCrank;
	/** The barrel drum, turned about the bore with the crank (user 2026-10-01: it did not turn while firing). */
	UPROPERTY(Transient)
	TObjectPtr<class UProceduralMeshComponent> GatSpin;
	float GatSpinShown = 0.f;
	float GatCrankAngle = 0.f, GatCrankLast = 0.f, GatCrankFire = 0.f;
	bool bCranking = false, bCrankShot = false;
	uint64 GatShownFrame = 0;
	void EnsureGatTripod();
	/** A gatling in either hand: the player stands at it (no stick movement). */
	bool GatlingOut() const;
	/** Held things are drawn over the level (M_OLR_Held pulls them toward the eye): bounds wide enough to reach
	 *  the eye, so occlusion culling never drops them while they are behind a wall. */
	static void KeepDrawnOverWorld(class UPrimitiveComponent* C);
	// the double guns' barrel selector (user 2026-09-28): a lever on the gun's left, flicked by the other hand (empty
	// or holding the other sawed-off); forward = both barrels (primary), back = one barrel (secondary)
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> SelPivot[2];
	bool bOneBarrel[2] = { false, false };
	float SelAngle[2] = { -35.f, -35.f };
	float SelPrevDist[2] = { 99.f, 99.f };
	FVector SelPrevRel[2] = { FVector::ZeroVector, FVector::ZeroVector };
	void UpdateSelector(int32 Hand, float Dt);
	/** The turn stick pushed down with a double gun in hand: both hands' double guns switch barrels together. False
	 *  when neither hand holds one. */
	bool ToggleBarrels();
	bool bBarrelLatched = false;
	/** The index finger's curl while this hand holds a gun: resting on the trigger, pulled with it; < 0 = no gun (the
	 *  fist closes as before). */
	float TriggerFingerCurl(int32 Hand) const;
	float HammerCock[2] = { 1.f, 1.f };     // 1 cocked (as drawn) .. 0 fallen
	float TriggerPull[2] = { 0.f, 0.f };
	bool bDebugCylOut = false;              // -OLCylSideTest: both cylinders held out
	void BuildGunParts(int32 Hand, int32 Wi);
	/** A sprite gun's part model (hammer, trigger, lever) about its pivot, from the session's cache (read there once). */
	TSharedPtr<OutlawsObj::FObjMesh>* GunPartModel(int32 Wi, const TCHAR* Suffix, const FVector& Pivot);
	void UpdateGunParts(int32 Hand, float Dt);
	// dynamite and knives: lit / ready in the hand, then thrown with the hand's swing
	bool bFuseLit[2] = { false, false };
	float Fuse[2] = { 0.f, 0.f };
	// the lit stick's wick burns down (its own section of the held mesh, squeezed toward the bundle's cap) with a
	// flame on its end (user 2026-09-28)
	int32 FuseSection[2] = { -1, -1 };
	TArray<FVector> FuseRestPos[2];
	TArray<FVector> FuseRestNrm[2];
	TArray<FVector2D> FuseRestUV[2];
	TArray<FLinearColor> FuseRestCol[2];
	float FuseShown[2] = { 1.f, 1.f };
	UPROPERTY(Transient)
	TObjectPtr<class UStaticMeshComponent> FuseFlame[2];
	UPROPERTY(Transient)
	TObjectPtr<class UStaticMeshComponent> FuseCore[2];
	void UpdateFuseLook(int32 Hand);
	/** bHeld: the flame takes the held gear's pulled depth (M_OLR_FxGlowHeld), as the stick it burns on does. */
	void MakeFuseFlame(USceneComponent* Parent, class UStaticMeshComponent*& OutFlame, class UStaticMeshComponent*& OutCore, bool bHeld = true);
	/** The wick at F of its length (0..1) on StickMesh's Section (when bWick), the flame on its end, upright. */
	void PlaceFuse(class UProceduralMeshComponent* StickMesh, int32 Section, const TArray<FVector>& Pos, const TArray<FVector>& Nrm,
		const TArray<FVector2D>& UV, const TArray<FLinearColor>& Col, float F, bool bWick, class UStaticMeshComponent* Flame, class UStaticMeshComponent* Core, float Size = 1.f);
	/** The fuse's flame: the game's fire sprite (fire01) on a quad turned to the eyes (user 2026-10-01), its frames. */
	TArray<FLinearColor> FireCells;
	float FireFps = 10.f, FireAspect = 0.66f;
	/** Sparks spat from a burning fuse's end (user 2026-10-01): a pool of little glowing beads thrown up and falling. */
	struct FFuseSpark { int32 Bead = -1; FVector P = FVector::ZeroVector, V = FVector::ZeroVector; float Life = 0.f, Life0 = 0.f; };
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStaticMeshComponent>> SparkBeads;
	TArray<FFuseSpark> FuseSparks;
	float SparkDue = 0.f;
	void EmitFuseSparks(const FVector& At, float Dt, float Size);
	void UpdateFuseSparks(float Dt);
	/** A thrown knife's aim assist (user 2026-09-28): the live bad guy nearest where a throw from From at Vel would land,
	 *  within 5 units (150 cm) of it; null when there is none. */
	class AOutlawsEnemy* KnifeTargetNear(const FVector& From, const FVector& Vel) const;
	/** A shot into a thrown stick of dynamite (lit or not) sets it off; false when the actor is not one. */
	bool ShootThrown(AActor* A);
	/** A fist, knife or tool driven into dynamite (a thrown stick, or the level's crates and sticks) sets it off. */
	bool MeleeDynamite(const FVector& At);
	/** Two different one-handed weapons trade hands (rounds and barrel setting with them); false when they cannot. */
	bool TradeWeapons();
	float ThrowRestock[2] = { 0.f, 0.f };
	bool bKnifeReady[2] = { false, false };
	float KnifeCool[2] = { 0.f, 0.f };
	TWeakObjectPtr<class UAudioComponent> FuseSound[2];
	struct FThrown
	{
		TWeakObjectPtr<AActor> Actor;
		FVector Pos = FVector::ZeroVector;
		FVector Vel = FVector::ZeroVector;
		FRotator Spin = FRotator::ZeroRotator;
		float Fuse = -1.f;
		float Age = 0.f;
		float Damage = 0.f;
		int32 Type = 0;             // 1 dynamite, 2 knife, 3 a bottle, 4 a prop that does not break (a pot, a rock)
		FString BreakWav;           // a bottle's: the sound its burst plays
		FString Hold;               // its "hold" trait, for the hand that catches it
		FVector Axis = FVector::UpVector;   // a bottle's own up, in its actor's frame (set down, it stands on it)
		TWeakObjectPtr<class AOutlawsEnemy> Homing;     // a knife's aim assist: the bad guy it bends toward in flight
		TArray<TWeakObjectPtr<class AOutlawsEnemy>> Flinched;     // the bad guys it went past (each flinches once)
		bool bResting = false;
		bool bLaid = false;         // a resting stick laid flat on the floor
		float Grav = 1.f;           // gravity times this in flight (1 now: every throw falls at its Pull, user 2026-10-02)
		float Pull = 0.f;           // its fall, cm/s^2: ThrowGravity where it left the hand (0: the player's plain one)
		bool bDud = false;          // a stick thrown or dropped unlit: at rest it lies there to be picked up again
		float GrowTo = 1.f;         // grows from the hand's size to the level item's over a second (a stick, a knife, a pot or a
		                            // rock: user 2026-09-29, 2026-10-01)
		FVector Scale0 = FVector::OneVector;
		TWeakObjectPtr<class UAudioComponent> Sound;
		// a lit stick keeps burning in flight: its wick section and the flame on it
		int32 FuseSection = -1;
		TArray<FVector> WickPos, WickNrm;
		TArray<FVector2D> WickUV;
		TArray<FLinearColor> WickCol;
		TWeakObjectPtr<class UStaticMeshComponent> Flame, Core;
	};
	TArray<FThrown> Thrown;
	float MountYaw = 0.f;              // the mounted gatling's heading
	FRotator GatAim = FRotator::ZeroRotator;   // the carried gatling's aim, eased (mostly the head's, a fifth the hand's)
	// long guns held two-handed: the front hand's grip on the forend, the stock at the shoulder
	bool bSupport[2] = { false, false };     // this hand holds the other hand's long gun by the forend
	bool bShouldered[2] = { false, false };  // this hand's long gun rests on the shoulder
	/** Aiming down the sights: a long gun shouldered or scoped, or any gun brought up to the eye along the look
	 *  (its bore within 10 cm of the eye, pointing where the eye looks) (user 2026-09-29: the counts and the wrist hide). */
	bool AimingDownSights(int32 Hand) const;
	bool bWasShouldered[2] = { false, false };
	uint64 ForeSnapFrame[2] = { 0, 0 };      // the frame this hand was last drawn snapped onto the other hand's forend
	/** The forend spot of a long gun (model space): under the barrel, 38 percent back from the muzzle. */
	FVector ForendSpot(int32 Wi) const;
	float StockBlend[2] = { 0.f, 0.f };      // 0 the hands alone .. 1 laid along the aiming eye (the virtual stock)
	FBox GunBounds[2] = { FBox(ForceInit), FBox(ForceInit) };
	bool NearForegrip(int32 Hand) const;
	/** This (empty) hand is close enough to the other hand's shovel shaft to take it two-handed. */
	bool NearToolShaft(int32 Hand) const;
	FVector SupportAnchor[2] = { FVector::ZeroVector, FVector::ZeroVector };   // where the front hand took this hand's long gun (model space)
	/** The gatling is out: it stands on the ground, and the player stands behind it. */
	bool IsMountedGun() const;
	void UpdateThrowable(int32 Hand, float Trigger, float Dt);
	/** Throw what the hand holds along the arc; bUnlit: a stick of dynamite let go unlit (the grip, user 2026-09-29),
	 *  bDrop: just let fall from the hand. */
	void ThrowHeld(int32 Hand, bool bUnlit = false, bool bDrop = false);
	bool bTntGrip[2] = { false, false };        // the grip closed on a stick in hand: let go, the stick goes unlit
	bool bGripUsed[2] = { false, false };       // this grip's press took or worked something (a door, a bad guy)
	void UpdateThrown(float Dt);
	/** The menu settings: the player's own read at start, everything written when one changes. */
	void LoadPlayerSettings();
	void SaveSettings();
	/** A menu setting the game really keeps (movement speed, volumes): its value, or null. */
	const float* LiveSetting(const FString& Name) const;
	/** Menu scrubbing: does this item (page) do anything in the VR build. */
	bool ItemWorks(const OutlawsMenu::FItem& I, TSet<FString>& Visiting) const;
	bool PageWorks(const FString& File, TSet<FString>& Visiting) const;
	void ScrubPage(OutlawsMenu::FPage& Page) const;
	/** The throw a hand would make now, the first throw's rule again (user 2026-10-06: the hand-speed throw was too weak):
	 *  the hand's height over the room's floor sets how far (waist: a lob, raised overhead: the longest), the weapon its
	 *  angle (dynamite 35, the rest 12 degrees), and the hand's own path over its last tenth of a second which way.  No
	 *  head in it and no wrist: the controller's twist and pitch are never read. */
	FVector ThrowVelocity(int32 Hand) const;
	/** The hand's height over the room's floor at the eyes' standing scale (the tracked height plus the recenter's offset;
	 *  a crouch on the stick does not count), and the throw's power (0..1) from it: 87 cm (waist) a lob, 207 cm (the
	 *  hand stretched up 42 over the standing eyes) the longest; build 29's overarm and underarm powers at 175 and 115 cm). */
	float ThrowPower(int32 Hand) const;
	/** Which way the hand is throwing along the ground: from the head's position out through the hand (never the look,
	 *  the stick or a turn), else, the hand right under the eyes, the way it points. */
	FVector ThrowHeading(int32 Hand) const;
	/** One throw for everything the hand lets fly (user 2026-10-02: a held bad guy flew slow, a rock too fast; each had
	 *  its own fall and pace).  Whatever is thrown falls as the player does where it leaves the hand: 980 times
	 *  BaseGravityScale (1.5 in the headset) times the sector's GRAVITY over townex's -60, with no pace of its own.  The
	 *  aimed throw kept its ranges (user 2026-09-29: the distances felt right) but lost the old ThrowPace of 2 (twice the
	 *  speed under four times the fall): the hardest throw leaves the hand at 26 m/s, a hard baseball throw, not 52, and
	 *  a 12 degree throw is in the air about 0.7 s, not 0.37.  Weight only changes how much of the speed carries (a man
	 *  ManThrowScale of it, the rest all of it); the aimed throw carries none of the body's running (a toss does: BodyCarry). */
	float ThrowGravity(const FVector& At) const;
	/** A man in the hand (not a chicken or a tarantula) is thrown half as far as the rest (user 2026-09-30: twice too
	 *  strong): his launch speed times this (the range goes with its square); the palm launch and a toss the same. */
	static constexpr float ManThrowScale = 0.70710678f;
	/** The aimed throw of a man (user 2026-10-06: "about as far as the push release"): at 45 degrees, the palm-up push's
	 *  own launch, his speed the same curve times this: the hand at full height (207 cm) leaves at about 1500 cm/s as the
	 *  palm-up push does, about 16.5 m to the hand's height with a 4.1 m apex in the headset (-OLThrowTest's throw compare
	 *  line).  Lower hands scale down the same curve as before. */
	static constexpr float ManAimThrowScale = 0.60f;
	static constexpr float ManAimThrowPitch = 45.f;
	/** Let go with the grip alone (a toss): the hand's own swing times this, times the weight's share, plus the body's
	 *  running (user 2026-10-02).  The tracked grip trails the throw (the wrist snaps last), so a flick carries half again
	 *  its speed: 1.5, between a bottle's old 1.25 and a man's old 1.6. */
	static constexpr float TossGain = 1.5f;
	FVector TossVelocity(int32 Hand, float Share) const;
	/** The body's own running along the ground: every throw carries it. */
	FVector BodyCarry() const { return FVector(GetVelocity().X, GetVelocity().Y, 0.0); }
	/** Carrying a man (user 2026-09-30): moving at this of the speed, stamina spent CarryDrain times as fast. */
	static constexpr float CarrySpeedScale = 0.75f;
	static constexpr float CarryDrain = 1.3f;
	/** A man (not a critter) held in either hand. */
	bool CarryingMan() const;
	/** The dotted flight path and landing mark while a throw is held (hidden with bShow false). */
	void ShowThrowArc(int32 Hand, bool bShow);
	/** (BUG-ARCSTUCK) the arc is drawn afresh each frame something is held to throw; one not drawn this frame is hidden at
	 *  the end of Tick, whatever took the thing from the hand (a death, a punch, a drop, the menu, the wheel, a level change). */
	void SweepThrowArcs();
	bool ArcDotsVisible(int32 Hand) const;
	uint64 ArcDrawnFrame[2] = { 0, 0 };
	bool bArcUp[2] = { false, false };
	/** -OLShots -OLArcStuckTest (BUG-ARCSTUCK): held men killed and held dynamite taken away, each arc checked gone. */
	void ArcStuckTestTick();
	static constexpr int32 ArcDotCount = 28;
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStaticMeshComponent>> ArcDots;      // ArcDotCount + 1 (the landing mark) per hand
	// teleport (VR option): the move stick pushed forward shows a yellow arc from that hand to the ground; let go, the
	// player is there. Only ground takes it (a floor the body fits on, not under water, not far above the feet)
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStaticMeshComponent>> TeleportDots; // ArcDotCount + 1 (the landing ring)
	bool bTeleportAim = false;
	bool bTeleportTurnLatched = false;  // a sideways flick of the move stick snap-turned; back to the middle to go again
	bool bTeleportValid = false;
	FVector TeleportTarget = FVector::ZeroVector;
	void UpdateTeleport(float Dt);
	void ShowTeleportArc(bool bShow);
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> WheelRoot;
	FVector WheelDir = FVector::ForwardVector;   // where the wheel hangs from the eyes (follows the head lazily)
	float WheelYawVel = 0.f, WheelPitchVel = 0.f;         // (its springs' speeds, degrees a second)
	bool bWheelYawFollow = false, bWheelPitchFollow = false;
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStaticMeshComponent>> WheelSlots;
	// the remaster's wheel art (Common.kpf gfx/wheel): the horseshoe, the pick frames, the icons
	UPROPERTY(Transient)
	TObjectPtr<class UStaticMeshComponent> WheelBase;
	UPROPERTY(Transient)
	TObjectPtr<class UStaticMeshComponent> WheelDial;        // behind the weapon pointed at
	UPROPERTY(Transient)
	TObjectPtr<class UStaticMeshComponent> WheelItemDial;    // behind the item pointed at
	UPROPERTY(Transient)
	TObjectPtr<class UTextRenderComponent> WheelBanner;      // the name of what is pointed at
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UMaterialInstanceDynamic>> WheelSlotMids;
	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<class UTexture2D>> KpfTextures;
	TArray<FString> WheelIconOn, WheelIconOff;              // per slot: the kpf art (empty: none)
	TArray<float> WheelSlotAngle;                            // per slot: degrees round the wheel (items: none)
	// the item row pages through what is carried, three at a time, with the remaster's arrows
	static constexpr int32 WheelItemsShown = 64;     // every item shows (rows by type; no paging)
	TArray<FString> AllWheelItems, AllWheelItemLabels;
	TArray<FString> WheelSlotLabels;                         // the weapons' names (the items' follow the page)
	int32 WheelWeaponCount = 0;
	int32 WheelItemStart = 0;
	int32 WheelArrowHover[2] = { 0, 0 };                     // -1 the prev arrow, +1 the next, 0 neither
	bool bWheelTriggerArmed[2] = { false, false };
	UPROPERTY(Transient)
	TObjectPtr<class UStaticMeshComponent> WheelPrev;
	UPROPERTY(Transient)
	TObjectPtr<class UStaticMeshComponent> WheelNext;
	/** Lay out every slot: the weapons round the ring, this page of items over the banner, the arrows. */
	void RebuildWheelSlots();
	/** (BUG-WHEELOCCLUDE) every wheel part's bounds reach past the eyes: never occlusion culled behind a near wall */
	int32 KeepWheelOverWorld();
	/** (BUG-WHEELOCCLUDE) the wheel's overlay: the PC's, or the headset's 18 cm depth-tested copy */
	UMaterialInterface* WheelOverlayMaterial(bool bText) const;
	void UpdateWheelArrows();
	/** A picture from Common.kpf as a texture (cached; null when missing). */
	class UTexture2D* KpfTexture(const FString& Path);
	/** A flat picture facing the wheel's viewer, in the wheel art's unlit see-through material. */
	class UStaticMeshComponent* WheelPlane(class UMaterialInstanceDynamic*& OutMid);
	void SetPlaneTexture(class UStaticMeshComponent* Plane, class UTexture2D* Tex);
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTextRenderComponent>> WheelLabels;
	UPROPERTY(Transient)
	TObjectPtr<class UStaticMeshComponent> Lasers[2];
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> SlotMat;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> SlotLitMat;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> LaserMat;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> CaseMat;
	// guns: rounds loaded per hand, reserve per ammo type (0 bullets, 1 cartridges)
	int32 Mag[2] = { 0, 0 };
	// the reserve lives in the level actor's inventory (ibullit / icart counts, from cidjames.itm)
	TMap<int32, int32> SavedMag;       // hand * 16 + weapon -> rounds, kept across swaps
	float FireCooldown[2] = { 0.f, 0.f };
	float ReloadTimer[2] = { 0.f, 0.f };
	bool bButtonReload[2] = { false, false };   // Y / B pressed (Classic: X / A tapped): this hand loads to full
	double WheelHeldSince[2] = { 0.0, 0.0 };    // real time the wheel button went down (a tap reloads, Classic)
	void PressBReleased();
	void PressYReleased();
	/** A grip on nothing: does nothing (the jump is the right stick up, the items are used from the wheel). */
	void GripOnNothing(int32 Hand);
	// a physical jump: the head rising fast from standing height
	float PrevHeadZ = -1.f;
	float StandEyeZ = -1.f;
	float JumpCooldown = 0.f;
	int32 CarryHands[2] = { -1, -1 };  // the weapons in hand when the last level was won
	int32 CarryMag[2] = { -1, -1 };    // and their rounds (BUG-LEVELCARRY: topped up from the carried ammunition, not given)
	bool bCarryHands = false;
	TMap<int32, int32> CarrySavedMag;  // (BUG-HOLSTERRELOAD) the holstered guns' rounds when the last level was won
	bool bCarrySaved = false;
	TMap<int32, int32> EntryMags;      // every gun's rounds as this level was entered (a restart's)
	/** The holstered guns loaded from the carried ammunition as the reload does; a line for the log. */
	void TopUpHolstered(FString& Line);
	/** Rounds loaded into this gun from the carried ammunition, up to full (as many as there are); the new count. */
	int32 LoadFromCarried(int32 Wi, int32 Rounds);
	/** A gun put away with these rounds: one gun carried is one gun, the same rounds whichever hand draws it next. */
	void NoteMag(TMap<int32, int32>& M, int32 Hand, int32 Wi, int32 Rounds) const;
	double JumpDipStart = -1.0;        // when the head last dipped below standing (the jump's wind-up)
	bool bJumpDipDeep = false;         // that dip went to a crouch: no jump until standing again
	void DetectBodyJump(float Dt);
	float RecoilPitch[2] = { 0.f, 0.f };
	FRotator GunBaseRot[2];
	bool bTriggerArmed[2] = { true, true };
	float FanHide[2] = { 0.f, 0.f };   // seconds this hand's own gun stays put away while it fans the other
	float FxLeft[2] = { 0.f, 0.f };
	UPROPERTY(Transient)
	TObjectPtr<UTextRenderComponent> AmmoText[2];
	// text plates: pictures of text in the remaster's Montagu font (text components cannot use a runtime font):
	// the ammo readout over a gun, the round counts on the wheel (user 2026-09-27)
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UCanvasRenderTarget2D>> PlateRTs;
	UPROPERTY(Transient)
	TArray<TObjectPtr<USceneComponent>> PlateHolders;      // X toward the eyes; the plane hangs on it
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStaticMeshComponent>> PlatePlanes;
	TArray<FString> PlateShown;
	TArray<bool> PlateLeftAligned;                    // drawn from the canvas's left edge (the wrist's numbers), not centred
	TArray<bool> PlateLow;                       // its letters in the warning red (health low)
	/** The wrist panel's numbers as outlined plates (user 2026-10-01: outlined as the ammo counts are). */
	TArray<int32> WristPlates;
	int32 WristPlatesUsed = 0;
	int32 PlateDrawing = -1;
	UPROPERTY(Transient)
	TObjectPtr<class UFont> PlateFont;
	int32 AmmoPlate[2] = { -1, -1 };
	TArray<int32> WheelCountPlates;                        // per wheel slot
	// a weapon not carried, or with nothing left to fire, is blacked out on the wheel and cannot be picked
	// (user 2026-09-27); one revolver is one revolver until a second gpistol is found
	TArray<bool> WheelUsable;                              // per wheel slot
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStaticMeshComponent>> WheelShades;
	UPROPERTY(Transient)
	TObjectPtr<class UTexture2D> WheelShadeTex;
	UPROPERTY(Transient)
	TObjectPtr<class UTexture2D> WheelShadeDiscTex;         // (BUG-WHEELGREY: a weapon's round slot darkened as a disc)
	int32 WeaponsOwned(int32 Wi) const;
	bool WeaponHasAmmo(int32 Wi) const;
	bool WeaponUsable(int32 Wi) const { return Wi < 0 || (WeaponsOwned(Wi) > 0 && WeaponHasAmmo(Wi)); }
	int32 MakeTextPlate(USceneComponent* Parent, float HeightCm, bool bLeft = false);
	void SetTextPlate(int32 Index, const FString& Text, bool bLow = false);
	/** The pointing lasers' red: the held gear's material, full bright, at the world's depth (on the headset the world
	 *  material takes the menu's and the wheel's tone, which hid the lasers; the held gear is spared it). */
	UMaterialInterface* LaserMaterial();
	UFUNCTION()
	void DrawTextPlate(class UCanvas* Canvas, int32 Width, int32 Height);
	UPROPERTY(Transient)
	TObjectPtr<class UStaticMeshComponent> MuzzleFx[2];
	UPROPERTY(Transient)
	TObjectPtr<class UStaticMeshComponent> TracerFx[2];
	UPROPERTY(Transient)
	TObjectPtr<class UStaticMeshComponent> SparkFx[2];
	UPROPERTY(Transient)
	TObjectPtr<class UStaticMeshComponent> FlashStar[2];
	// gunfire effects (OutlawsPlayerWeapons.cpp): pooled smoke puffs and spent cases
	struct FFxBit { FVector Pos = FVector::ZeroVector; FVector Vel = FVector::ZeroVector; FRotator Rot = FRotator::ZeroRotator;
		FRotator Spin = FRotator::ZeroRotator; float Age = 0.f; float Life = 0.f; float Size0 = 0.f; float Size1 = 0.f; float Alpha = 0.f;
		bool bBounce = false; bool bRest = false; };       // (a break gun's shell: it meets the floor, hops and lies there)
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStaticMeshComponent>> PuffFx;
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UMaterialInstanceDynamic>> PuffMat;
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UStaticMeshComponent>> CaseFx;
	TArray<FFxBit> Puffs;
	TArray<FFxBit> Cases;
	// a movie shows alone: the hands, guns, lasers, wrist, HUD and the smoke and cases are put out of sight while it
	// plays and what was showing comes back after (user 2026-09-28)
	bool bMovieHidden = false;
	float PushCool[2] = { 0.f, 0.f };      // a push has pressed a switch: the hand waits before another
	TArray<TWeakObjectPtr<class UPrimitiveComponent>> MovieHiddenParts;
	void HideForMovie(bool bHide);
	float RecoilKick[2] = { 0.f, 0.f };    // cm the gun has slid back along the barrel
	FVector GunBaseLoc[2];
	int32 Spent[2] = { 0, 0 };             // fired cases still in a revolver's cylinder

	// climbing: one hand holds a ledge; the body moves so that hand stays where it grabbed
	int32 GrabHand = -1;               // 0 left, 1 right
	FVector GrabPoint = FVector::ZeroVector;
	float LedgeTopZ = 0.f;
	FVector LedgeIn = FVector::ForwardVector;
	float PrevGrip[2] = { 0.f, 0.f };
	FVector HandVelocity[2];
	FVector PrevHandPos[2];
	float MantleLeft = 0.f;
	FVector MantleFrom = FVector::ZeroVector;
	FVector MantleTo = FVector::ZeroVector;
	FString ClimbRefusal;              // (BUG-SLOPECLIMB) why the last StartMantle found no climb, for the let-go's log line
	TWeakObjectPtr<AActor> HeldItem[2];   // a key / puzzle item in each hand
	// a bottle off a shelf (user 2026-09-28): held at its real size where the hand closed on it, thrown on the let-go,
	// set down gently it stands; swung into a bad guy it is a melee blow; it bursts on whatever it strikes hard
	UPROPERTY(Transient)
	TObjectPtr<class UProceduralMeshComponent> HeldBottle[2];
	FString BottleSound[2];
	FVector BottleAxis[2] = { FVector::UpVector, FVector::UpVector };   // the bottle's own up, in the hand's frame
	FVector PrevBottlePos[2] = { FVector::ZeroVector, FVector::ZeroVector };
	int32 BottleClear = -1;               // the level's Clear() count when last looked: a new level takes the bottles
	FVector BottleTestAt = FVector::ZeroVector;   // -OLBottleTest: where the bottle left the hand
	static constexpr double BottleReach = 45.0;       // cm from the fist to the bottle (user 2026-09-29: loose as a bad guy's)
	static constexpr float BottleThrowDamage = 0.4f;   // a common bad guy has 2: three to the head (x2) fell one, two do not
	static constexpr float BottleSmashDamage = 0.8f;   // over the head: a punch's worth, and the bottle goes
	/** A bottle or a prop into the hand: the nearest in reach of the fist, or the one given (a resting thrown one by its
	 *  index in Thrown, or one of the level's). */
	bool TryHoldBottle(int32 Hand, int32 PickThrown = -1, class AOutlawsSpriteActor* PickProp = nullptr);
	/** The loose reach (user 2026-09-29): a bad guy, a bottle or a prop pointed near, up to 4 m off, taken to the hand;
	 *  the best pointed of them all.  Last in the grip's chain: what the hand touches comes first. */
	bool TryReachGrab(int32 Hand);
	/** TryReachGrab's pick, taking nothing: the best pointed bad guy, resting thrown bottle (index) or prop. */
	void FindReachGrab(int32 Hand, class AOutlawsEnemy*& OutEnemy, int32& OutThrown, class AOutlawsSpriteActor*& OutProp) const;
	/** The grab glow (user 2026-09-30): what each hand's grip would take now (an item, a bottle or prop, a bad guy),
	 *  shown by a white rim: a shell round a 3D thing, a bigger white copy behind a sprite (M_OLR_GrabHalo). */
	struct FGrabPick
	{
		TWeakObjectPtr<class UProceduralMeshComponent> Model;
		TWeakObjectPtr<class AOutlawsSpriteActor> Sprite;
	};
	bool PreviewGrab(int32 Hand, FGrabPick& Out);
	void UpdateGrabGlow(float Dt);
	/** The ledge the hand would take (TryGrab's probe): the lip to hang from, into the ledge, the top's point. */
	bool FindLedge(int32 Hand, FVector& OutGrabPoint, FVector& OutIn, FVector& OutTop, bool bLog) const;
	/** If it glows you can grab it (user 2026-10-01): a ledge in reach gets a white band along its lip, a door or switch
	 *  a thin white frame where the hand would push; one strip mesh a hand, in world space. */
	void UpdateSurfaceGlow(int32 Hand, bool bOn);
	UPROPERTY(Transient)
	TObjectPtr<class UProceduralMeshComponent> SurfaceGlow[2];
	int32 SurfaceGlowKind[2] = { 0, 0 };      // 0 none, 1 a ledge, 2 a door or switch (logged on change)
	FGrabPick GlowPick[2];
	float GlowLook = 0.f;                 // the picks are looked for every 0.05 s; the glow itself every frame
	float GlowTime = 0.f;
	TMap<TWeakObjectPtr<UPrimitiveComponent>, TWeakObjectPtr<UPrimitiveComponent>> GlowHalos;   // target -> its halo
	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInterface> GlowShellMat;
	/** The gatling's crank glows as a thing to grab does when the free hand comes near it (user 2026-10-01): the frame it
	 *  last should, and its white at the held gear's depth (it is held gear). */
	uint64 CrankGlowFrame = 0;
	/** How far apart the fists may be to pass or trade what they hold (user 2026-10-01: a bigger thing held further
	 *  apart): 15 cm, more by a fifth of the larger held thing's length, 30 at most (not so far a two-handed aim does it). */
	double SwapReach() const;
	/** Where a taken bottle or prop sits in the hand (relative to FrameOf(Hand)), by its shape (user 2026-10-01): one with
	 *  a handle by the handle, its body over the hand and its inside to the other palm; a tall one by the neck, upright,
	 *  its body under the fist; anything else in the palm. M: the hand-size copy, its up +Z. */
	FTransform HoldPoseFor(int32 Hand, const class UProceduralMeshComponent* M, const FString& Mode = FString()) const;
	/** How a shape is held by the hand's own reading of it (backlog 4: traits.py records these as they are): "handle"
	 *  (a corner far out past the rest), "neck" (tall for its girth) or "palm". */
	static const TCHAR* HoldModeOf(const TArray<FVector>& P, double* OutRmax = nullptr, double* OutRmed = nullptr, double* OutH = nullptr);
	FString BottleHold[2];                     // the held thing's "hold" trait (empty: the shape decides)
	/** The map's hint blobs drawn for this many lock items (drawn again when one is taken). */
	int32 MapSpotsDrawn = -1;
	/** The held map turned halfway from the hand's own angle toward facing the eyes, eased (user 2026-10-01), pinned at
	 *  the corner the fingers hold: that corner in the hand's frame and in the page's, and the hand's angle. */
	FVector MapPinchHand[2], MapPinchPage[2];
	FQuat MapAnchorRel[2] = { FQuat::Identity, FQuat::Identity };
	bool bMapTwoHand[2] = { false, false };   // the other hand holds the page's far corner (user 2026-10-03, BUG-MAP)
	bool bMapOtherGrip[2] = { false, false }; // (that hand's grip last frame: the hold latches on a fresh press)
	float MapSpotsWait = 0.f;
	/** This hand holds the gatling (the other hand is its crank's: nothing else is drawn into it, user 2026-10-01). */
	bool HoldsGatling(int32 Hand) const;
	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInterface> GlowShellHeldMat;
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UMaterialInstanceDynamic>> GlowHaloMids;
	/** How near the hand points to the span A..B (a body's height, a thing's middle), up to 4 m: false out of reach,
	 *  else the ranking score (smaller better) and the point of the span nearest the pointing. */
	bool PointedNear(int32 Hand, const FVector& A, const FVector& B, double Slack, double& OutScore, FVector& OutOn) const;
	/** The bottle leaves the hand: thrown as the knife is (ThrowVelocity, homing) when bThrow, else let go with the
	 *  hand's own motion (set down, or tossed). */
	void ReleaseBottle(int32 Hand, bool bThrow = false);
	/** An overlay material (the menu, the wheel, the HUD text) by name: on the mobile renderer its depth-tested "_Q" copy. */
	UMaterialInterface* LoadOverlayMaterial(const TCHAR* Name) const;
	bool bBottleReady[2] = { false, false };
	// the hands and the water: a quick slap through its surface splashes; a hand or gun behind the surface from the
	// eyes is left for the surface to draw over (MPC WaterZ / WaterOn, the held material's near pull; user 2026-09-28)
	FVector PrevHandWater[2] = { FVector::ZeroVector, FVector::ZeroVector };
	float HandSplashCool[2] = { 0.f, 0.f };
	void UpdateHandWater(float Dt);   // the trigger squeezed on a held bottle: the arc shows, letting go throws
	/** The bottle in From passes to the empty hand To, as a weapon changes hands (user 2026-09-28). */
	void PassBottle(int32 From, int32 To);
	/** What a hand held when it took a bottle (user 2026-09-29): moved into the other hand if that was free, else put
	 *  away; back where it was when the bottle goes (thrown, set down, smashed, passed on). */
	int32 StowWeapon[2] = { -1, -1 };
	FString StowTool[2];
	int32 StowMovedTo[2] = { -1, -1 };
	void StowForBottle(int32 Hand);
	void RestoreStowed(int32 Hand);
	/** A bad guy held by the chest (user 2026-09-29): grip held keeps him, let go he drops or is thrown. */
	TWeakObjectPtr<class AOutlawsEnemy> GrabbedEnemy[2];
	float GroanWait = 0.f;             // (HM1: an outlaw taken alive groans while held)
	FVector GrabOffset[2] = { FVector::ZeroVector, FVector::ZeroVector };
	TArray<TWeakObjectPtr<class AOutlawsEnemy>> FlungEnemies;      // thrown ones the body walks through till they land
	TArray<double> FlungUntil;                   // ... and at least till then (a second after the let-go), and till clear of him
	/** The hand's own swing for a held bad guy let go: through the world standing, against the body on the move (the
	 *  world speed of a hand on a running player passes the 4 m/s sanity cap and read as none: user 2026-10-01). */
	FVector HeldPushVel(int32 Hand) const;
	bool TryGrabEnemy(int32 Hand, class AOutlawsEnemy* Pick = nullptr);
	/** The bad guy a hand would take: touching (the fist within 60 cm of his height) or, bRemote, pointed near. */
	class AOutlawsEnemy* FindGrabEnemy(int32 Hand, bool bRemote, double& OutScore) const;
	bool TryTakeEnemyFromOther(int32 Hand);
	/** From the first bad guy taken till the last let go: what each hand held before, given back at the end (user
	 *  2026-09-29: two held at once, the weapons holstered or moved meanwhile). */
	bool bGrabSession = false;
	int32 SessionWeapon[2] = { -1, -1 };
	void EndGrabSession();
	float SmashCool = 0.f;
	float ShoveCool[2] = { 0.f, 0.f };
	void UpdateEnemyGrab(float Dt);
	/** REQ-HELDCLASH (OutlawsPlayerHeldClash.cpp): after the hands have placed them, two held men collide where they are drawn
	 *  (their frames' opaque pixels, within a body's thickness along the view), each pushed off the other as an offset from
	 *  his hand; the smash comes on the first contact.  -OLOldSmash (OldSmash): the chests' 55 cm smash, no collision. */
	void UpdateHeldClash(float Dt);
	static bool OldSmash();
	/** Both held men hurt by the smash, its sound and haptics, the cooldown (user 2026-09-29). */
	void SmashHeld(class AOutlawsEnemy* A, class AOutlawsEnemy* B, const FVector& Rel, const TCHAR* How);
	/** How far the other has pushed this hand's man off it (zero for any other man). */
	FVector HeldClashOffset(int32 Hand, const class AOutlawsEnemy* E) const;
	TSharedPtr<OutlawsHeldClash::FState> HeldClash;
	int32 HeldSmashCount = 0;               // (the probe counts the smashes)
	void LetGoEnemy(int32 Hand, bool bThrow, const FVector* Launch = nullptr);
	/** The held one let go along the arc the trigger showed (ThrowVelocity). */
	void ThrowEnemyAimed(int32 Hand);
	bool bLaunchArmed[2] = { false, false };
	/** The palm launch (user 2026-09-30): the fastest push of the holding hand toward where its palm faces, lately (kept
	 *  a third of a second, so a push and then the grip let go counts), and that palm's way then. */
	float PushPeak[2] = { 0.f, 0.f };
	double PushAt[2] = { 0.0, 0.0 };
	FVector PushDir[2] = { FVector::ForwardVector, FVector::ForwardVector };
	bool bGrabBack[2] = { false, false };
	bool bGrabAiming[2] = { false, false };      // the trigger held on a held bad guy: the throw's arc shows
	float GrabGlide[2] = { 1.f, 1.f };
	bool bHeldFaded[2] = { false, false };       // a held body see-through while its throw is lined up
	FVector PrevCarry[2][2];                     // a held body's chest and middle last frame: panes it is pushed through break          // 0..1: a bad guy taken from afar gliding into the fist
	FVector GrabFrom[2] = { FVector::ZeroVector, FVector::ZeroVector };      // taken from behind: his back shows, however the player turns   // the holding hand's trigger, pulled fresh, launches him
	/** What the hand holds as a bottle is breakable (bottles, vases, pottery) or not (pots, pans, the skull). */
	bool bHeldBreaks[2] = { true, true };
	float BottleGrow[2] = { 1.f, 1.f };      // a held prop's size in the level over its size in the hand (thrown, it grows back)
	FTransform BottleZoomTo[2];                // where it settles in the hand: as grabbed, shrunk to hand size about the grip
	float SwingCool[2] = { 0.f, 0.f };
	/** A held bottle (pot, pan...) drawn half see-through while its throw is lined up: its own materials kept to put back. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInterface>> GhostSaved0;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInterface>> GhostSaved1;
	bool bBottleGhost[2] = { false, false };
	TWeakObjectPtr<class UProceduralMeshComponent> GhostOf[2];   // (the ghosting belongs to that one thing)
	void SetBottleGhost(int32 Hand, bool bOn);
	bool bJumpLatched = false;
	// taken, it flies into the fist (quick, eased) and is held as the dynamite is: upright, its middle in the fist
	float BottleZoom[2] = { -1.f, -1.f };     // 0..1 on the way in; below 0 held
	FTransform BottleZoomFrom[2];             // where it started, in the hand frame
	void BreakHeldBottle(int32 Hand, const FVector& Dir);
	void UpdateBottles(float Dt);
	/** A second of a pair-able gun picked up while the first is drawn: the free hand draws it (user 2026-09-29). */
	void UpdateDualPickup(float Dt);
	TMap<int32, int32> PrevOwned;              // each pair-able gun's count last frame
	TMap<int32, float> DualPending;            // a second just picked up: seconds left to draw it in the free hand
	// items pulled by pointing a gripping hand at them: they fly to the chest, then are taken
	// bTaken: already counted (walked over, tucked away, auto-collected): only its look flies, then it goes (user 2026-10-02)
	struct FPull { TWeakObjectPtr<AActor> Item; FVector Start = FVector::ZeroVector; FVector Scale = FVector::OneVector; float T = 0.f; bool bTaken = false; };
	TArray<FPull> Pulls;
	TWeakObjectPtr<AActor> PullLanding;     // a pull at the chest being taken now: it has flown already
	bool TryPull(int32 Hand);
	void UpdatePulls(float Dt);
	UPROPERTY(EditAnywhere, Category = "Outlaws|VR")
	float PullRange = 450.f;                // how far a pointing hand reaches for items
	UPROPERTY(EditAnywhere, Category = "Outlaws|VR")
	float PullAngle = 10.f;                 // degrees off the eye-to-hand line

	// smoke test (-OLSmokeTest)
	float SmokeTime = -1.f;
	int32 SmokeStep = 0;
	// screenshots of chosen spots (-OLShots, desktop)
	float ShotTime = -1.f;
	int32 ShotStep = 0;
	void ShotTest(float Dt);
public:
	/** One -OLShots test, defined and registered by OL_SHOT_TEST (OutlawsShotTests.h); N is its place in the order. */
	template<int32 N> void ShotCase(float Dt, AOutlawsLevelActor* L);
private:
	/** -OLShots -OLThrowTest: a rock and a held man thrown with the same hand, each flight logged (OutlawsPlayerWeapons.cpp). */
	void ThrowTestTick();
	/** -OLShots -OLGrabFallTest (BUG-GRABFALL): a man taken from 8 m up by the hand and let go, twice; no fall damage. */
	void GrabFallTestTick();
	/** -OLShots -OLHeldSwayTest (REQ-HELDSWAY): a man held and moved sideways, then toward, then stopped and let go;
	 *  the legs' lag and the bottom edge's turn logged each frame (OutlawsPlayerHeldSway.cpp). */
	void HeldSwayTestTick(float Dt);
	/** -OLShots -OLHeldClashTest[=perf] (REQ-HELDCLASH): two men held and brought together, side by side, one higher, one
	 *  further along the view, and fast claps; the contacts against a 1 cm truth, the smashes counted, the cost logged
	 *  (OutlawsPlayerHeldClashTest.cpp). */
	void HeldClashTestTick(float Dt);
	/** -OLShots -OLGrabUpscaleTest (REQ-GRABUPSCALE): a man held before the eyes shows his held frame's upscale (or, with
	 *  -OLGrabUpscaleMissing, the HD atlas), and the atlas again after the let-go (OutlawsPlayerGrabUpscale.cpp). */
	void GrabUpscaleTestTick(float Dt);
	/** -OLShots -OLLanternSwimTest (REQ-LANTERNSWIM): a lit lantern in the left hand, then the right (the map in the left),
	 *  put away once by the wheel's path, taken out again and carried into the water; out of the water it stays away. */
	void LanternSwimTestTick();
	FString LanternSwimState() const;
	/** BUG-CHARLOADOUT: the guns as a character's own copies have them (FOutlawsCharKit::WeaponVariant: Sanchez's revolver
	 *  BADGUN and 2.5 damage, Mary's pistol3...): sound, damage, pellets, spread, variance, range.  Empty or James: the
	 *  guns as built (James's own, ij*), single player untouched.  The guns are this machine's player's alone. */
	static void SetWeaponCharacter(const FOutlawsAssets* A, const FString& Short);
	/** BUG-CHARLOADOUT: the kit's first weapon (the level's StartWeaponSlot, a weapon ITM's SLOT) drawn in the dominant hand. */
	bool EquipStartWeapon();
	/** REQ-MARYNAILS: the hands' nails in the character's colour (FOutlawsMpCharacter::NailRGB; the hands' own for none). */
	void WearNails(const FString& Short);
	uint32 NailsWorn = 0;
	/** -OLShots -OLCharLoadoutTest (BUG-CHARLOADOUT): each multiplayer character's kit made and worn in turn: the first weapon
	 *  drawn, the counts and the revolver's sound and damage, checked against its ITMs (OutlawsPlayerWeapons.cpp). */
	void CharLoadoutTestTick();
	void DecalEdgeTestTick();            // (BUG-DECALEDGE: OutlawsDecalEdgeTest.cpp)
	/** -OLShots -OLFeelTest (user 2026-10-02): a pickup walked over (its flight to the chest), a flight of stairs walked
	 *  down (the view's height against the body's, per frame) and a man held at the chest (his view frame, per frame);
	 *  OutlawsPlayerFeelTest.cpp. */
	void FeelTestTick(float Dt);
	/** -OLShots -OLFreezeTest / -OLFreezeSoak (user 2026-10-03): the far-and-unseen bad guys' freeze and the far sounds
	 *  skipped, checked on a big open level; OutlawsPlayerFreezeTest.cpp. */
	void FreezeTestTick(float Dt);
	float StepTimer = 0.f;
	float MovieHold = 0.f;             // seconds any button has been held during the movie (3 skips it)
	int32 FaceHeld = 0;                // A 1, B 2, X 4, Y 8 held right now
	FVector SmokeMark = FVector::ZeroVector;
	float SmokeValue = 0.f;
	TWeakObjectPtr<class AOutlawsEnemy> SmokeEnemy;

	// screen feedback: a red flash per hit, a vignette as health runs low, sepia when dead
	float HurtFlash = 0.f;
	float DeathBlend = 0.f;
	// low health's look at the moment of death, carried into death's (no flash of full colour between)
	float WeakAtDeath = 0.f, LowAtDeath = 0.f, LastWeak = 0.f, LastLow = 0.f;
	bool bFxDead = false;
	bool bDeadView = false;
	float DeathTime = 0.f;
	float DeathEye = 160.f;           // eye height above the feet when the player died
	float DeathDrop = 0.f;            // how far the view has fallen since
	UPROPERTY(Transient)
	TObjectPtr<UTextRenderComponent> RespawnText;
	UPROPERTY(Transient)
	TObjectPtr<UTextRenderComponent> HudText;
	float HudDist = 80.f;             // the notices' distance from the eyes: nearer than the first wall along their line
	bool bHudDistLogged = false;
	/** The Graphics page's Performance HUD (OutlawsDeviceProfile::PerfHud): frame rate and thread ms above the view's middle. */
	UPROPERTY(Transient)
	TObjectPtr<UTextRenderComponent> PerfHudText;
	float PerfHudTime = 0.f;
	int32 PerfHudFrames = 0;
	void UpdatePerfHud();
	TArray<FString> HudLines;
	FVector StepPrev = FVector::ZeroVector;
	bool bUnlimitedAmmo = false;       // cheat: guns never empty, never need a reload
	void UpdateFootsteps(float Dt);
	TArray<float> HudLeft;
	bool bHudDirty = false;
	// revolver fanning swings the gun onto the nearest bad guy in range
	float FanActive[2] = { 0.f, 0.f };

	// the in-game menu (OutlawsPlayerMenu.cpp): the remaster's pages from Common.kpf on a floating panel
	bool bMenuOpen = false;
	TSharedPtr<OutlawsMenu::FSource> MenuSource;
	TSharedPtr<OutlawsMenu::FPage> MenuPage;
	TArray<FString> MenuStack;
	TMap<FString, int32> MenuValues;       // "page:item" -> shown value (nothing is applied yet)
	int32 MenuScroll = 0;
	int32 MenuHover[2] = { -1, -1 };
	bool bMenuHoverHeld = false;          // (REQ-VOICEPAGE) a row lit by the menu itself, kept until a hand points at another row
	bool MenuBackHover[2] = { false, false };   // (REQ-BACKCLICK) each hand's laser on the page's Back prompt
	bool bMenuTriggerArmed[2] = { true, true };
	bool bDesktopMenuMouse = false;        // (REQ-TRIGGERSTEP) a mouse button pressed in a desktop menu: the view's middle points
	bool bMenuStepHitEnd = false;          // (REQ-TRIGGERSTEP) the last press found its number row at the end (a tick given)
	int32 MenuStepTicks[2] = { 0, 0 };     // (REQ-TRIGGERSTEP) end ticks given to each hand (the test reads them)
	float MenuYaw = 0.f;
	float MenuPitch = 0.f;                // up and down too (user 2026-09-28), lazily as the yaw
	float MenuYawVel = 0.f, MenuPitchVel = 0.f;     // its follow's spring (degrees a second)
	bool bMenuYawFollow = false, bMenuPitchFollow = false;   // following the head back to the middle
	// the Cheats row: hidden till the player turns a whole circle on the spot with the pause menu up (user 2026-10-01)
	bool bCheatsShown = false;
	bool bCheatReplay = false;
	float CheatSpin = 0.f;                // degrees turned (signed) since the menu came up (New Game's page: since it came up)
	float CheatSpinLastYaw = 0.f;
	// REQ-PERFTESTMODE (OutlawsPerfTest.h): New Game's Performance test row, shown by the same whole turn with that page up
	bool bPerfTestShown = false;
	/** Single player only: never in a co-op session or a match. */
	bool PerfTestOffered() const;
	float MenuScrollWait = 0.f;
	float MenuToastLeft = 0.f;
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> MenuRoot;
	UPROPERTY(Transient)
	TObjectPtr<class UStaticMeshComponent> MenuPanel;
	UPROPERTY(Transient)
	TObjectPtr<class UStaticMeshComponent> MenuHoverBox[2];
	UPROPERTY(Transient)
	TObjectPtr<class UStaticMeshComponent> MenuLaser[2];
	UPROPERTY(Transient)
	TObjectPtr<UTextRenderComponent> MenuTitle;
	UPROPERTY(Transient)
	TObjectPtr<UTextRenderComponent> MenuMessage;
	UPROPERTY(Transient)
	TObjectPtr<UTextRenderComponent> MenuToast;
	UPROPERTY(Transient)
	TObjectPtr<UTextRenderComponent> MenuMoreUp;
	UPROPERTY(Transient)
	TObjectPtr<UTextRenderComponent> MenuMoreDown;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTextRenderComponent>> MenuRows;
	// the remaster's look: the parchment page (Common.kpf menu/assets/Main_Base.dds) with the menu drawn
	// on it in the remaster's fonts (Rye, Montagu Slab) through a canvas render target; the text
	// renders above stay as the model and are hidden
	UPROPERTY(Transient)
	TObjectPtr<class UCanvasRenderTarget2D> MenuCanvas;
	UPROPERTY(Transient)
	TObjectPtr<class UStaticMeshComponent> MenuSheet;
	UPROPERTY(Transient)
	TObjectPtr<class UTexture2D> MenuParchment;
	UPROPERTY(Transient)
	TObjectPtr<class UFont> MenuTitleFont;
	UPROPERTY(Transient)
	TObjectPtr<class UFont> MenuRowFont;
	UPROPERTY(Transient)
	TObjectPtr<class UFont> MenuSmallFont;
	/** A font from Common.kpf (copied out to Saved/KpfCache), loaded at runtime at this size. */
	class UFont* KpfFont(const FString& KpfPath, int32 Size);
	/** An uncompressed DDS from Common.kpf (the menu art) as a texture. */
	class UTexture2D* KpfDds(const FString& KpfPath);
	UFUNCTION()
	void DrawMenuCanvas(class UCanvas* Canvas, int32 Width, int32 Height);
	// the same page in white: its coverage (runtime-font text never writes alpha, so the sheet's
	// opacity comes from here)
	UPROPERTY(Transient)
	TObjectPtr<class UCanvasRenderTarget2D> MenuMask;
	bool bMenuMaskPass = false;
	// the remaster's save and load screens (load_game.rml / save_game.rml, drawn from UI.kpf's Load_* art) on a
	// 16:9 sheet; SheetW / SheetH are the sheet's size in menu cm (the parchment pages: 100 x 80)
	struct FSlotShown { int32 Slot = -1; bool bEmpty = true; FString Level, Name, GameTime, Stamp; };
	TArray<FSlotShown> SlotsShown;        // one per item of the save / load page
	int32 PendingSaveSlot = -1;
	bool bWideSheet = false;
	bool bTallSheet = false;              // (REQ-SUBMENUSPACE) a titled page's taller sheet: 100 x 120
	float SheetW = 100.f, SheetH = 80.f;
	bool IsSavePage() const;
	/** The title flow's pages (#front, #difficulty:*, #missions): the remaster's own screens on the wide sheet. */
	bool IsFrontPage() const;
	bool bFrontMenu = false;              // the main menu is up (it cannot be closed, only left by starting a game)
	TArray<TPair<FBox2D, int32>> FrontHits;   // where each item is on the 1920 x 1080 view, for the lasers
	TArray<FBox2D> FrontItemInk;          // (REQ-TITLEMENU) each front item's label as last drawn (greyed ones too), for the tests
	/** The front item a laser meeting the view at Px, Py (1920 x 1080) points at, or -1 (the pointer and the tests). */
	int32 FrontHitAt(const FVector2D& At) const;
	FBox2D FrontInk = FBox2D(ForceInit);  // (REQ-MPLOBBY b) a co-op page's lines and rows as last drawn, for the fit check
	// (REQ-MPLOBBY c) a sheet page as last drawn (canvas pixels): each row's text, the title, the help / "more below" line
	TArray<FBox2D> SheetRowInk;
	FBox2D SheetTitleInk = FBox2D(ForceInit);
	TArray<FBox2D> SheetFootInk;
	TArray<FBox2D> SheetEdgeInk;          // (REQ-SUBMENUSPACE) the title, "more above", the help line and "more below" as drawn
	FBox2D SheetMoreUpInk = FBox2D(ForceInit);    // (REQ-PROMPTCENTRE) "more above" as drawn
	int32 TestSheetRowCap = 0;                    // (-OLPromptCentreTest) at most this many rows a sheet page (0: as laid out), so it scrolls
	FIntPoint SheetCanvasSize = FIntPoint(0, 0);
	float SheetSmallHalfPx = 10.f;                // (BUG-GFXTALL) the small font's letters' half height on the sheet's canvas, as last drawn
	/** REQ-PROMPTCENTRE's check on the open sheet page: the foot line's middle (its parts as one) and "more above"'s, each its
	 *  offset across from the sheet's middle (world cm; false: not shown) and the line's text. */
	bool PromptFootOffset(float& OutCm, FString& OutText) const;
	bool PromptMoreUpOffset(float& OutCm) const;
	/** The sheet's rows that sit on it from the top (at most MenuRowsShown): a row that would reach below the panel, or into the
	 *  help line, is left for the scroll (REQ-MPLOBBY c: section gaps pushed the last row off the VR page). */
	int32 SheetRowsFit() const;
	/** BUG-GFXTALL: a titled sheet page showing its first N rows at this scroll, its block's top (the top of "more above" or the
	 *  title) and foot (the foot of the prompt line, its drawings in VR) as heights on the sheet (cm), worked out, not as drawn. */
	void SheetBlockZ(int32 N, float& OutTopZ, float& OutFootZ) const;
	/** (tests) the open sheet page's block as last drawn: its height and its top's height over the rows' middle (cm on the sheet,
	 *  as the limits), and that top's angle over the line of sight (degrees); false: not a titled sheet page or not drawn yet. */
	bool SheetBlockMeasure(float& OutBlockCm, float& OutTopCm, float& OutTopDeg) const;
	static float SheetBlockLimitCm();
	static float SheetTopLimitCm();
	int32 SheetMaxScroll();
	float FrontSpin = 0.f;                // the lens turning behind the main menu
	void DrawFrontCanvas(class UCanvas* C, int32 Width, int32 Height);
	float PageRowTop() const;
	float PageRowStep() const;
	/** REQ-SUBMENUSPACE: the open page is a titled parchment page (not the pause page, a title page or the save screen), drawn on
	 *  the taller sheet with its rows further apart; that sheet's height (cm), its help line's height, its most rows. */
	bool IsTallSheet() const;
	float PageSheetH() const;
	float PageFootZ() const;
	/** (REQ-BACKRIGHT) how much further down the prompt line sits (sheet cm) so Back's controllers beside it clear the last row. */
	float BackBesideDrop() const;
	int32 PageRowsShown() const;
	/** REQ-MENUCENTRE: a parchment-sheet page (the pause page and the titled pages; not a title page, the save screen or a wide
	 *  sheet), the rows it shows now (from the top, the scroll's), and the middle of those rows (cm on the sheet): the height
	 *  that sits at eye level, the prompts one gap (MenuFootGap) under the last of them. */
	bool IsSheetPage() const;
	int32 SheetRowsShownNow() const;
	float SheetRowsMiddleZ() const;
	/** The sheet's height (cm) that the eyes' line of sight meets (REQ-MENUCENTRE: the rows' middle on a sheet page, 12 cm over
	 *  the sheet's middle on the others, as before). */
	float MenuEyeZ() const;
	/** The menu put where it faces now (the same as the menu's own tick does; tests call it after showing a page). */
	void PlaceMenuRoot(double PauseYawOff = 0.0, double Dist = -1.0);
	/** Row r's height on the sheet (cm): the rows step down from the top, a gap above each section heading. */
	float RowZ(int32 r) const;
	/** The row at a height on the sheet, or -1 between / outside them. */
	int32 RowAt(float Z) const;
	void SetSheetShape(bool bWide, bool bTall = false);
	void DrawSaveCanvas(class UCanvas* C, int32 Width, int32 Height);
	/** menu/assets/<Name>.dds from UI.kpf (BC7) or Common.kpf, cached. */
	class UTexture2D* KpfArt(const FString& Name);
	UPROPERTY(Transient)
	TMap<FString, TObjectPtr<class UTexture2D>> MenuArt;
	UPROPERTY(Transient)
	TMap<int32, TObjectPtr<class UFont>> SaveFonts;
	class UFont* SaveFont(int32 Kind);
	bool bDeathAutosave = false;
	int32 DeathSaveSlot = -1;             // (BUG-DEATHAUTOSAVE: what B / Y loads: a slot >= 0, or the autosave)
	double SwimExitUntil = 0.0;
	float StepSpeed = 0.f;                // the feet's speed over the ground, smoothed (the step cadence follows it)
	double FallSince = -1.0;              // world time a fall began (out of the map after 3 s of it)
	// stuck rescue (BUG-STUCKGAP; OutlawsPlayerUnstuck.cpp)
	struct FSafeSpot { FVector Feet; double At = 0.0; int32 Sector = -1; };
	TArray<FSafeSpot> SafeSpots;          // newest last, a few kept (one may be gone when it is wanted: a lift moved)
	int32 SafeClear = -1;                 // the level's Clear() count they belong to
	int32 LastHeadSector = -1;            // the last sector the head was in (the fallback's)
	double HeadOutSince = -1.0, BodyStuckSince = -1.0, NextUnstuckLook = 0.0, UnstuckWarpAt = -1.0, UnstuckAskedAt = 0.0;
	FString UnstuckTrigger, BodyStuckWhy;
	// (BUG-UNSTUCKLOOP) the last rescue: when, where it put the body, how many came one on another at the same spot (the
	// player not moved off it between), and the spots those tries used (none is tried again in the run)
	double UnstuckLastAt = -1.0, UnstuckHeldLogAt = -1.0;
	FVector UnstuckLastTo = FVector::ZeroVector;
	int32 UnstuckStreak = 0;
	TArray<FVector> UnstuckTried;
	/** Whether an automatic rescue waits: one came under BackOff s ago and the body has not moved off its spot. */
	bool UnstuckHeldBack(double Now, const TCHAR* Why);
	FVector StuckProgressFrom = FVector::ZeroVector;   // (the wall test's headway: where the body was half a second ago)
	double StuckProgressAt = -1.0;
	bool bStuckProgress = false;
	void UpdateUnstuck(float Dt);
	/** Whether Feet is a safe floor spot now: OutFeet the feet on the sector's floor there. */
	bool SafeFeetAt(const FVector& Feet, FVector& OutFeet) const;
	void UnstuckWarp();
	/** -OLShots -OLUnstuckTest: the head through a wall for 0.3 s and 1.5 s, a body pinned in the floor, the fallback with
	 *  no safe spot. */
	void UnstuckShotTick();
	/** -OLShots -OLNarrowWalkTest=<sector>|narrow: stood on a thin floor (a fence top, a beam) and walked its length; logs
	 *  each stuck check that starts and each rescue (none must fire) and how far the walk got (OutlawsPlayerUnstuck.cpp) */
	void NarrowWalkShotTick();
	/** -OLShots -OLUnstuckLoopTest (BUG-UNSTUCKLOOP): the 10 Oct townex spot, the head held 60 cm off the body through the
	 *  wall as a room-scale step leaves it; at most 2 rescues may fire and the player must end up standing safe. */
	void UnstuckLoopShotTick();
	TArray<int32> NarrowTestSectors;
	TArray<FVector> NarrowTestLine;
	int32 NarrowTestAt = 0, NarrowTestPoint = 0, NarrowTestFalls = 0, NarrowTestChecks = 0, NarrowTestCount0 = 0;
	float NarrowTestT0 = 0.f;
	double NarrowTestWidth = 0.0, NarrowTestLength = 0.0;
	FVector UnstuckTestDir = FVector::ZeroVector;
	float UnstuckTestT0 = 0.f, UnstuckTestMaxFade = 0.f;
	int32 UnstuckTestCount0 = 0;
	FVector FallFrom = FVector::ZeroVector;
	/** An item's type for grouping (isqrkeyA..E -> isqrkey, igear1..3 -> igear). */
	static FString ItemStem(const FString& Item);
	/** The remaster's wheel card for an item, by its name, then its type. */
	class UTexture2D* ItemCard(const FString& Item, bool bOn);
	// the Sheriff's Badge spinning (ibadge.nwx) under the off-hand wrist while its power lasts (the original
	// spun it at the top of the HUD)
	TWeakObjectPtr<class AOutlawsSpriteActor> BadgeIcon;
	void UpdateBadgeIcon();           // just jumped out of the water: the swim does not catch the body again yet
	UPROPERTY(Transient)
	TObjectPtr<class UMaterialParameterCollection> ToneMpc;          // died with an autosave to go back to (B / Y)
	UFUNCTION()
	void DrawMenuMask(class UCanvas* Canvas, int32 Width, int32 Height);
	/** The page changed (rows, hover, toast): draw it again. */
	void RedrawMenu();

	bool LeftDominant() const { return DominantHand == EOutlawsHand::Left; }
	/** The title screens' pointing hand: the dominant one, or the other while only the other is tracked (RC1.1: a player
	 *  with one working controller must still reach the front menu). */
	int32 MenuPointerHand() const;
	float Axis(const TCHAR* Name) const;
	FVector2D MoveStick() const;
	FVector2D TurnStick() const;
	FVector2D OneStick() const;
	static bool OneStickTurning(const FVector2D& S);
	UMotionControllerComponent* OffAim() const { return LeftDominant() ? RightAim : LeftAim; }
	UMotionControllerComponent* DominantAim() const { return LeftDominant() ? LeftAim : RightAim; }
	UMotionControllerComponent* GripOf(int32 Hand) const { return Hand == 0 ? LeftGrip : RightGrip; }
	UMotionControllerComponent* AimOf(int32 Hand) const { return Hand == 0 ? LeftAim : RightAim; }
	USceneComponent* FrameOf(int32 Hand) const { return Hand == 0 ? LeftHandFrame : RightHandFrame; }
	/** Where the open palm faces: the hand frame's palm axis (right hand -Y, the left mirrored). */
	FVector PalmDir(int32 Hand) const { return FrameOf(Hand)->GetRightVector() * (Hand == 1 ? -1.0 : 1.0); }
	UMaterialInterface* SolidMaterial(const FColor& Colour);
	// the hands and guns darken with the sector light around the player, as the enemies do (user
	// 2026-09-27): their materials (M_OLR_Held) take a Brightness set from the light at the head
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UMaterialInstanceDynamic>> HeldMids;
	float HeldLight = -1.f;
	float HandWaterDim[2] = { 1.f, 1.f };     // (BUG-WATERHANDS) each hand's held gear dimmed under the water (1: not)
	float HandWaterWant[2] = { 1.f, 1.f };
	UMaterialInterface* HeldBase() const;
	UMaterialInterface* HeldSolidMaterial(const FColor& Colour);
	UPROPERTY(Transient) UMaterialInterface* FuseMaterial = nullptr;   // (BUG-TNT) the dynamite's wick and cord, hemp
	/** Weapon Wi's held model (built once a session; Equip's), whether it is a card, and the path it is cached by. */
	TSharedPtr<OutlawsObj::FObjMesh> HeldModelFor(int32 Wi, bool& bOutCard, FString& OutPath);
	/** A held model section's material (Key its section, a wick's "fuse:..."), one per texture for the session. */
	UMaterialInterface* HeldSectionMaterial(const FString& Key, const FString& Texture);
	void UpdateHeldLight(float Dt);
	void AlignHandFrames();
	float ViewDrop();
	// weapon wheel + placeholder weapons (OutlawsPlayerWeapons.cpp)
	void OpenWheel(int32 Hand);
	void ReleaseWheel(int32 Hand);
	void CloseWheel();
	void UpdateWheel();
	// tools from the wheel (the shovel, the crowbar): into the hand that opened the wheel, held until a
	// weapon (or fists) replaces them; the spade digs where it is driven into the floor, the crowbar
	// jabs and pushes doors (user 2026-09-27). The game has only sprites of them: simple models here
	FString HeldTool[2];
	UPROPERTY(Transient)
	TObjectPtr<class USceneComponent> ToolRoot[2];
	FString ToolBuilt[2];
	FVector ToolTipPrev[2] = { FVector::ZeroVector, FVector::ZeroVector };
	bool bToolTipValid[2] = { false, false };
	double ToolTipX[2] = { 78.0, 78.0 };         // the tool's business end along its X (cm from the fist)
	bool bToolSupport[2] = { false, false };     // this hand holds the other hand's shovel by the shaft
	bool bToolTwoHanded[2] = { false, false };   // this hand's tool is laid between both hands (its root moved)
	float ToolCooldown[2] = { 0.f, 0.f };
	static bool IsTool(const FString& Item) { return Item.Equals(TEXT("ishovel"), ESearchCase::IgnoreCase) || Item.Equals(TEXT("icrowbar"), ESearchCase::IgnoreCase)
		|| Item.Equals(TEXT("iink"), ESearchCase::IgnoreCase); }
	void EquipTool(int32 Hand, const FString& Item);
	/** A tool's rest in one hand: the crowbar stands up out of the fist (turned as the knife and dynamite were; user
	 *  2026-09-28), the shovel lies along the hand. */
	FTransform ToolRest(int32 Hand) const;
	void DropTool(int32 Hand);
	void BuildToolModel(int32 Hand, bool bShovel);
	FVector ToolTip(int32 Hand) const;
	void UpdateTools(float Dt);
public:
	// held gear from the items wheel (user 2026-10-01), held as the tools are (HeldTool) and put away the same way: the
	// lantern (its light from the hand, hanging and swinging under it) and the map (a torn page of the level's walls, the
	// layer underfoot in full and the rest faint, the player's arrow on it).  The boiler plate is worn, not held
	// (REQ-CHESTPLATE): ChestRoot
	static bool IsGear(const FString& Item) { return Item.Equals(TEXT("ilantern"), ESearchCase::IgnoreCase) || Item.Equals(TEXT("imap"), ESearchCase::IgnoreCase); }
	/** (REQ-CHESTPLATE) No plate is held up any more: armour takes its share of every hit as in the original
	 *  (AOutlawsLevelActor::DamagePlayer), so no shot is stopped here; always false. */
	bool PlateStops(const FVector& From, FVector& InOutTo, float& InOutDamage) { return false; }
	/** (REQ-CHESTPLATE) No plate is held up between a bad guy and the chest any more; always false. */
	bool PlateShields(const FVector& From, const FVector& To) const { return false; }
	/** REQ-CHESTPLATE (user 2026-10-15): the boiler plate worn from its pickup, floating under the head at the chest and
	 *  turned with the body (ChestYaw), dented as the armour goes, gone when it breaks.  The body's yaw (world degrees): the
	 *  head's, held within 35 degrees of it and eased toward it (not while looking well down); the view's on the desktop. */
	float ChestYaw() const;
	/** The worn plate's size for the other machines (the plate picked up: 50, 100, 150; 0 none worn). */
	int32 ChestPlateSize() const;
	/** The worn plate's look as the other machines draw it on this player: an undented plate of size PlateMax, on OnActor
	 *  (absolute placement), on the held material with the near pull off.  False: no model. */
	bool BuildPlateLook(AActor* OnActor, int32 PlateMax, class UProceduralMeshComponent*& Out);
	/** (tests) The worn plate as shown now: its middle, its rotation, whether it is drawn, and its dents. */
	bool ChestPlateShown(FVector& OutAt, FRotator& OutRot) const;
	int32 ChestDents() const { return PlateMarks.Num(); }
	/** Where lit dynamite is: a stick lit in a hand, a lit one in flight or lying (the bad guys run from it). */
	void LitDynamite(TArray<FVector>& Out) const;
private:
	void ToggleGear(int32 Hand, const FString& Item);
	void BuildGearModel(int32 Hand);
	void UpdateGearCommon(float Dt);
	/** The Sheriff's Badge's gold round the view (user 2026-10-01): steady, then pulsing out with the badge's blink. */
	void UpdateBadgeVignette(float Dt);
	UPROPERTY(Transient)
	TObjectPtr<class UStaticMeshComponent> BadgeVeil;
	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> BadgeVeilMid;
	float BadgeVeilIn = 0.f;            // eased in over a moment when the badge is taken
	/** The red of a hit on the headset (no post process there): M_OLR_HurtDir's look on a plane before the eyes. */
	void UpdateHurtVeil();
	UPROPERTY(Transient)
	TObjectPtr<class UStaticMeshComponent> HurtVeil;
	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> HurtVeilMid;
	void UpdateGear(int32 Hand, float Dt);
	void BuildPlate();
	void EnsurePlateMarks();
	void UpdateChestPlate(float Dt);
	/** A dent for armour points lost (where a hit from HurtFrom would strike, else anywhere on the plate). */
	void DentPlate(int32 Lost);
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> ChestRoot;                 // the worn plate's frame: X out from the chest, Z up
	UPROPERTY(Transient)
	TObjectPtr<class UProceduralMeshComponent> ChestMesh;
	float ChestYawRel = 0.f;                               // the body's yaw in the play space (degrees)
	bool bChestYawSet = false;
	int32 ChestArmourSeen = -1;                            // the armour last frame (a drop dents the plate)
	int32 ChestMaxSeen = -1;                               // (BUG-PLATECARRY) the plate's size last frame (a load sets it)
	FVector ChestHurtFrom = FVector::ZeroVector;           // the last hit's source (OnHurt), for where its dent goes
	double ChestHurtAt = -1.0;
	void BuildMapPage(int32 Hand);
	void PlaceMapArrow(int32 Hand);
	UFUNCTION()
	void DrawMapCanvas(class UCanvas* Canvas, int32 Width, int32 Height);
	struct FPlateMark
	{
		FVector2D At = FVector2D::ZeroVector;     // on the plate (cm across, cm up from its middle)
		float Radius = 2.f;
		float Depth = 0.f;                        // a dent's depth (cm, toward the wearer)
	};
	TArray<FPlateMark> PlateMarks;
	/** The plate's mesh, size PlateMax, dented by Marks, into Pm's sections (the held material's sections set by the caller). */
	static void FillPlateMesh(class UProceduralMeshComponent* Pm, int32 PlateMax, const TArray<FPlateMark>& Marks);
	int32 PlateSerialSeen = -1;
	int32 PlateHitsSeen = -1;                  // (BUG-PLATECARRY: the level's PlateHits last frame; a drop with none new is no blow)
	TMap<FString, float> TestAxes;             // (desktop tests: an input held, as Axis reads it)
	TWeakObjectPtr<class AOutlawsLevelActor> PlateLevelSeen;     // (a new level's count starts again: no pickup)
	FVector2D PlateDims = FVector2D::ZeroVector;
	UPROPERTY(Transient)
	TObjectPtr<class UProceduralMeshComponent> GearMesh[2];          // the plate, or the map's page
	UPROPERTY(Transient)
	TObjectPtr<class UProceduralMeshComponent> MapArrow[2];
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> LanternHang[2];
	UPROPERTY(Transient)
	TObjectPtr<class UStaticMeshComponent> LanternFlame[2];
	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> LanternGlassMid[2];
	FVector LanternBob[2], LanternBobPrev[2];
	bool bLanternBob[2] = { false, false };
	FVector LanternFlameAt[2];                   // the flame's foot in the lantern body's frame (where its painted flame stood)
	float LanternFlameH[2] = { 0.f, 0.f };       // ... and that flame's height (cm); 0: the glowing bead instead
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> LanternBody[2];
	FVector LanternOriginPrev[2];                // the play space last frame (the swing feels a share of its move)
	FVector LanternMoveVel[2];                   // the play space's speed through the world, eased (cm/s)
	float LanternYawPrev[2] = { 0.f, 0.f };      // the body's turn last frame (a snap turn's jump is not a move)
	UPROPERTY(Transient)
	TObjectPtr<class UCanvasRenderTarget2D> MapCanvas;
	UPROPERTY(Transient)
	TObjectPtr<class UTexture2D> MapPaper;
	int32 MapLayer = TNumericLimits<int32>::Min();    // the layer drawn in full on the page
	TSet<int32> MapSectors;                           // the sectors on it: those joined up with the one underfoot
	FVector2D MapCentre = FVector2D::ZeroVector;      // the level's middle (Unreal X, Y), at the page's
	double MapScale = 0.01;                           // page cm per world cm
	// the co-op partner on the page (user 2026-10-03): a marker where they stand, their name beside it, or an arrow at the
	// page's edge toward them when they are off the drawing (another layer, a part not joined to this one)
	// (N1: one per other player, up to seven, per hand: Hand * MapMarks + the slot)
	static constexpr int32 MapMarks = 15;
	UPROPERTY(Transient)
	TObjectPtr<class UProceduralMeshComponent> MapPartner[2 * MapMarks];
	UPROPERTY(Transient)
	TObjectPtr<class UTextRenderComponent> MapPartnerName[2 * MapMarks];
	FString MapPartnerLogged;
	FString MapOthersLogged[MapMarks];
	/** How far the map's markers sit off the drawing, cm (the page is 0.06 thick) */
	static constexpr double MapMarkLift = 0.35;
	void PlaceMapPartner(int32 Hand);
	void PlaceMapPartnerSlot(int32 Hand, int32 Slot);
	FVector2D MapPageSize = FVector2D(22.0, 28.0);    // the page, cm (across, up)
	/** The page's drawing (the sectors joined to the one underfoot, its scale, MapCanvas), shared by the held map and the
	 *  wheel's; false with no level */
	bool BuildMapDrawing();
	TWeakObjectPtr<class AOutlawsLevelActor> MapDrawnFor;
	const void* MapDrawnLevel = nullptr;                 // (the level data drawn, and its sector count: a new level draws again)
	int32 MapDrawnSectorCount = -1;
	// REQ-WHEELMAP (user 2026-10-15): the map is no wheel item; it lies flat in the wheel's middle, inside the horseshoe,
	// whenever the wheel is open, with the held map's drawing and markers (the red arrow the way the player faces, the
	// others' blue diamonds). Pointed at, it darkens with a "Trigger to zoom" prompt (BUG-MAPZOOMTRIGGER); the pointing
	// hand's trigger brings it closer (about 0.2 s), and the trigger again, the pointer leaving it or the wheel closing
	// sends it back
	void ShowWheelMap();
	void UpdateWheelMap(float RealDt);
	/** (BUG-MAPZOOMTRIGGER) the lasers on the map: the hand pointing, the darkening and prompt, the trigger's zoom */
	void UpdateWheelMapPointer();
	/** Where a pointer from From along Dir crosses the wheel, on the map (its resting page, a margin once it is close) */
	bool WheelMapHit(const FVector& From, const FVector& Dir) const;
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> WheelMapRoot;                 // on the wheel; moved toward the eyes when pointed at
	UPROPERTY(Transient)
	TObjectPtr<class UStaticMeshComponent> WheelMapPage;
	UPROPERTY(Transient)
	TObjectPtr<class UStaticMeshComponent> WheelMapArrow;
	UPROPERTY(Transient)
	TObjectPtr<class UStaticMeshComponent> WheelMapMark[MapMarks];
	UPROPERTY(Transient)
	TObjectPtr<class UTextRenderComponent> WheelMapName[MapMarks];
	UPROPERTY(Transient)
	TObjectPtr<class UTexture2D> WheelMapArrowTex;
	UPROPERTY(Transient)
	TObjectPtr<class UTexture2D> WheelMapDiamondTex;
	UPROPERTY(Transient)
	TObjectPtr<class UTexture2D> WheelMapEdgeTex;
	FVector2D WheelMapHalf = FVector2D(7.5, 7.5);            // the page's half size on the wheel (its cm at scale 1: across, up)
	float WheelMapNear = 0.f;                                // 0 resting in the horseshoe, 1 pulled close
	bool bWheelMapHover = false;
	bool bWheelMapZoom = false;                              // (BUG-MAPZOOMTRIGGER) pulled close by a trigger
	int32 WheelMapPointHand = -1;                            // the hand whose laser is on the map (-1 none)
	bool bWheelMapTrigArmed[2] = { false, false };           // (a trigger let go since the wheel opened: one zoom per pull)
	UPROPERTY(Transient)
	TObjectPtr<class UStaticMeshComponent> WheelMapShade;     // the slight darkening over the pointed-at page
	UPROPERTY(Transient)
	TObjectPtr<class UOutlawsGlyphPlate> WheelMapPrompt;      // "Trigger to zoom" with the trigger drawn
	int32 WheelMapTestHover = -1;                            // (desktop tests: 1 pointed at, 0 not; -1 the lasers decide)
	float WheelMapArrowYaw = 0.f;                            // the arrow's heading drawn (degrees, as yaw: 0 north up the page)
	/** -OLShots -OLLevel=hideout -OLNoFrontEnd -OLWheelMapTest[=left|one]: the wheel's map photographed and its ease logged */
	void WheelMapTestTick();
	/** REQ-MAPITEMS: the seen items' sprites on the page (DrawMapCanvas, over the walls), K pixels a page cm. */
	void DrawMapItems(class UCanvas* Canvas, int32 Width, int32 Height, double K);
	/** The page drawn again when its marks have changed (a blob's item taken, an item seen); every half second at most. */
	void RefreshMapMarks(float Dt);
	/** -OLShots -OLMapItemsTest[=vr|shots|shotsvr]: OutlawsMapItems.cpp */
	void MapItemsTestTick();
	// punches: a hand pushed fast into a bad guy hits him; bare fists at the fist's full strength (James 0.8,
	// ijfist), anything in the hand at a quarter (user 2026-09-27); the knife keeps its own jab
	float PunchCool[2] = { 0.f, 0.f };
	// stamina (the original's energy, 0..100; James' cidjames.itm rates): running drains E_MOVE 10 a second,
	// a jump E_JUMP 5; resting gives back E_REST 11 (walking half that, INFERRED), crouched E_CROUCH 2 more.
	// Spent, running slows to a walk and jumps reach half as high; every shot strays (100 - energy) x 0.05
	// degrees more [EXE 0x1400de9d1]; below 99, tired.wav breathes, louder the lower it gets.  Ours (StaminaRate):
	// running and jumps at a quarter of the exe's over 1.5, resting, walking and crouched at the exe's own (REQ-STAMINA15)
	float Stamina = 100.f;
	TWeakObjectPtr<class UAudioComponent> TiredSound;
	float TiredWait = 0.f;
	// (REQ-TIREDVOL, -OLStaminaProbe only) the first breath since the probe set TiredBreathStamina to -1: its stamina and multiplier
	float TiredBreathStamina = -1.f;
	float TiredBreathGain = 0.f;
	/** tired.wav's curve: 0 at 99 and over (no breath), else ((99 - Stamina) / 99) ^ 1.5, faint at 99 and full at 0 (user 2026-09-27). */
	static float TiredWant(float Stamina);
	/** REQ-TIREDVOL (user 2026-10-27): tired.wav's volume multiplier for a Want off TiredWant, before the sound and master
	 *  volumes: 1.5 times max(0.04, Want), 1.5 times as loud on the same curve; bOld (-OLOldTiredVolume): max(0.04, Want). */
	static float TiredGain(float Want, bool bOld);
	/** -OLOldTiredVolume (REQ-TIREDVOL's A/B switch): tired.wav at its level before 27 Oct. */
	static bool OldTiredVolume();
	void SwimKick(float Dir);
	float StaminaFrac() const { return FMath::Clamp(Stamina / 100.f, 0.f, 1.f); }
	void UpdateStamina(float Dt);
	/** REQ-STAMINA15 (user 2026-10-27, "out of breath in VR"): stamina a second (minus: spent) for what the body does:
	 *  running on the ground (at Pace, the level's Pace(); carrying a man CarryDrain times as fast), moving otherwise (a
	 *  walk), or still, and crouched or not.  The breath lasts 1.5 times as long running and comes back twice as fast as
	 *  before; bOld (-OLOldStamina): the rates before it. */
	static float StaminaRate(bool bRunOnGround, bool bMoving, bool bCrouched, float Pace, bool bCarrying, bool bOld);
	/** REQ-STAMINA15: what a jump spends (E_JUMP 5 at a quarter, over 1.5; bOld: at a quarter, as before). */
	static float StaminaJumpCost(bool bCarrying, bool bOld);
	/** -OLOldStamina (REQ-STAMINA15's A/B switch): the running, jumping and recovery rates before 27 Oct. */
	static bool OldStamina();
	/** (REQ-STAMINA15, OutlawsPlayerStaminaProbe.cpp) -OLStaminaProbe, in any build (Shipping too, through OLFlags, not
	 *  under WITH_OUTLAWS_TESTS): the times StaminaRate gives (a run from full to spent at each speed setting, carrying a
	 *  man, the rests from spent to full, the jumps to spent) under both rules, then, in the level, a rest from spent to
	 *  full timed on the game's own tick under the rule in use; "OLSTAMINA ..." lines and "OLSTAMINA RESULT PASS|FAIL".
	 *  With -OLShipSmoke the first level is played until it is done; without it the game quits once done. */
	static bool StaminaProbeOn();
	void TickStaminaProbe(float Dt);
	/** REQ-REGEN2X: the auto health regen a second standing still (the host's in co-op and multiplayer). */
	float RegenRate() const;
	/** Air under water (user 2026-10-01): it runs down as the head stays under, 30 s at the slider's 100 % and longer as
	 *  the speed is turned down (as far on a breath); out of it, a tenth of health every 5 s with choke.wav. */
	void UpdateBreath(float Dt);
	float Air = 1.f;                         // 0..1
	float DrownWait = 0.f;
	bool bHeadUnder = false;
	float BreathPhase = 0.f;                 // the dark's pulse
	/** The view darkening as the air runs out, pulsing, darker still while drowning as health goes (user 2026-10-01): the
	 *  badge's filter plane (M_OLR_Vignette, modulate) in near-black. */
	void UpdateBreathVeil(float Amount);
	/** The camera's fade (a level's end, a movie's start, the dark while a level builds) on the headset, which draws no post
	 *  process (user 2026-10-02: the fades to and from the cutscenes did not show on the Quest): a black veil over the view as
	 *  dark as the camera manager's FadeAmount. */
	void UpdateFadeVeil();
	/** (BUG-STUTTER2) the effects' materials first drawn mid-play (the lantern's glow on the wheel, the lit fuse, the blast,
	 *  the muzzle flash, the scope) cost the Quest 168-178 ms each in new pipeline states: drawn once behind the load's
	 *  dark instead, on small cards far in front of the eye (on a plane and on a procedural quad, inked), for a few frames */
	void UpdatePlayWarm(class AOutlawsLevelActor* L);
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UPrimitiveComponent>> WarmCards;
	int32 WarmFrames = -1;                 // -1: not yet; then the frames the cards have stood; done past 3
	/** (FIRSTHITCH) -OLOldFirstUse: the first-use prebuild as BUG-STUTTER2 left it (no held guns, gun sounds or scope picture of
	 *  the cards below), to set the first scope, shot and throw after a load against. */
	static bool OldFirstUse();
	/** (FIRSTHITCH) behind the load's dark, with the warm cards: every gun's held model and its materials built (the session's
	 *  cache: a first draw of the rifle, the shotguns or the gatling built them, 10 to 14 ms on the desktop) and the guns' and
	 *  the dynamite's sounds read into the level's cache (each first shot of a level read its wave). */
	void WarmFirstUse(class AOutlawsLevelActor* L);
	/** (FIRSTHITCH) one scope picture with the warm cards in it (the effects' pipelines in the scope's own pass). */
	void WarmScopeEffects(class AOutlawsLevelActor* L);
	/** (FIRSTHITCH, OutlawsPlayerFirstUse.cpp) -OLFirstUseProbe, in any build: once a level is up, the rifle drawn, scoped and
	 *  fired, every gun drawn and fired, a knife and a lit stick of dynamite thrown (and its blast), one every 0.5 s, the whole
	 *  list twice; each one's frames after it ("OLFIRSTUSE <level> r1|r2 <what>: worst N ms (Kx median ...)") against the
	 *  level's median frame.  With -OLShipSmoke each level is played until it is done. */
	static bool FirstUseProbeOn();
	void TickFirstUseProbe(float Dt);
	/** (BUG-SLOPECLIMB, OutlawsPlayerClimbProbe.cpp) -OLClimbProbe[=townex+mill], in any build (Shipping too, through OLFlags,
	 *  not under WITH_OUTLAWS_TESTS): climbs made as a hand makes them (the ledge taken by the right hand's grip, let go at
	 *  chest height): the town's sloped roofs from the ground beside them and a window it must not climb through, then the
	 *  mill's climbs of BUG-MILLCLIMB (the box and the walkway in the hall, a landing in the yard refused); a line each
	 *  ("OLCLIMBPROBE <level> <n> ...: PASS|FAIL|SKIP"), a RESULT line, then it quits (exit code 0 when all pass). */
	static bool ClimbProbeOn();
	void TickClimbProbe(float Dt);
	/** (BUG-SHOVELWHACK, OutlawsPlayerToolWhack.cpp) -OLToolWhackProbe, in any build: once a level is up, the shovel and the
	 *  crowbar swung at bad guys stood before the player, or at a bot in a match (across at the head, the body and low at 40,
	 *  80 and 120 cm, standing and ducking, chopped, resting on him, walked into him, a multiplied copy, a bad guy of another
	 *  kind, straight after a dig), each swing a frame at a time through UpdateTools, and the fist the same way through
	 *  UpdatePunches; "OLWHACK <level> <case>: PASS|FAIL|SKIP" and "OLWHACK RESULT".  With -OLShipSmoke each level is played
	 *  until its cases are done.  BUG-TOOLGRIP: across and chopped with the grip let go too (a tool lands, with -OLOldToolGrip
	 *  it does not), and a punch with it let go (no blow). */
	static bool ToolWhackProbeOn();
	void TickToolWhackProbe(float Dt);
	/** (REQ-PERFTESTMODE, OutlawsPerfTestProbe.cpp) -OLPerfTestProbe, in any build, from the main menu: no Performance test row
	 *  on New Game's page before the turn (nor after a turn on the title page), the row after a whole turn there (the pause
	 *  Cheats left hidden); picked, the run's second level up with the carry, the capture and the settings dump with the
	 *  values applied; a death and its respawn kept in the run; the exit back to the main menu; then a second run quit with
	 *  End Game from the pause menu; the Saved folder (but Perf and Logs) unchanged by each.  "OLPERFTEST ..." a check, then
	 *  "OLSHOT perftest: PASS|FAIL", and it quits. */
	static bool PerfTestProbeOn();
	void TickPerfTestProbe(float Dt);

	UPROPERTY(Transient)
	TObjectPtr<class UStaticMeshComponent> FadeVeil;
	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> FadeVeilMid;
	UPROPERTY(Transient)
	TObjectPtr<class UStaticMeshComponent> BreathVeil;
	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> BreathVeilMid;
public:
	/** The head under water (the autosave waits for air). */
	bool IsHeadUnderwater() const { return bHeadUnder; }
	/** (tests, REQ-CHEATSCOPE) the rifle in hand 1, the desktop's raise forced but only while the scope is carried. */
	void CheatScopeTestRaise();
	/** (tests, REQ-CHEATSCOPE) the rifle's scope picture is up: the zoom works. */
	bool CheatScopeTestUp() const;
	/** BUG-ALLOWCHEATS: Name's cheat may run now (Do): at once in single player, for the host and for a local-only row; a client
	 *  of a match asks the host first (the host's Allow Cheats row is the authority; bForceAsk asks even when the replicated flag
	 *  says off, for the tests).  Result: told the answer. */
	void RunCheat(const FString& Name, TFunction<void()> Do, bool bForceAsk = false, TFunction<void(bool)> Result = nullptr);
	/** BUG-ALLOWCHEATS: the rows the host's Allow Cheats does not govern (they only change this player's own menus). */
	static bool CheatIsLocalOnly(const FString& Name);
	/** (tests, BUG-ALLOWCHEATS) the Cheats page's row labels joined with "|" (the pause menu opened to it, then closed). */
	FString CheatsPageTestLabels();
	/** (tests, BUG-ALLOWCHEATS) the Cheats page opened and the row Name pressed, as a hand would (the menu stays open); false: no such row. */
	bool CheatsMenuTestPress(const FString& Name);
	void CheatsMenuTestClose() { if (bMenuOpen) CloseMenu(); }
	/** (tests, BUG-SPEEDCAP) Options > VR opened and the Movement speed row pressed, as a hand would (the menu stays open); false: no such row. */
	bool VrSpeedMenuTestPress();
private:
	bool bScopeTestNeedsItem = false;
	bool bScopeNoItemLogged = false;
	void UpdatePunches(float Dt);
	// the rifle scope (the original's iscope): the rifle's grip brought up to the eye with the scope
	// carried shows a circle at twice the magnification, black round it, and its shots fly true
	UPROPERTY(Transient)
	TObjectPtr<class USceneCaptureComponent2D> ScopeCapture;
	UPROPERTY(Transient)
	TObjectPtr<class UTextureRenderTarget2D> ScopeTarget;
	UPROPERTY(Transient)
	TObjectPtr<class UStaticMeshComponent> ScopeView;
	bool bScoped[2] = { false, false };
	bool bScopeTestForce = false;
	FQuat ScopeAim = FQuat::Identity;  // the scope's look: steadied direction, the head's roll; scoped shots fly along it
	FVector ScopeAimDir = FVector::ForwardVector;   // the steadied direction alone (a shaky hand filtered out)
	TArray<TWeakObjectPtr<UPrimitiveComponent>> ScopeUninked;     // held gear taken out of the ink while scoped
	bool bScopeAimSet = false;
	float WalkTestLeft = 0.f;          // -OLWalkTest: walking forward while above zero
	void UpdateScope();
	/** (BUG-STUTTER2) the scope's target, capture and picture made once a level is up and one capture drawn behind the
	 *  load's dark (the first raise on the Quest made them: a 218 ms render-thread stall); kept for the session */
	void EnsureScopeRig(class AOutlawsLevelActor* L);
	/** -OLRightEye (recording, user 2026-09-30): the desktop mirror shows the RIGHT eye, cropped to fill the window, for
	 *  OBS (the scope is on that eye).  UE 5.1 mirrors only the left: the spectator screen's render hook is rebound to
	 *  copy the right eye's half; re-applied every few seconds (a mode change rebinds the stock one). */
	void UpdateRightEyeMirror(float Dt);
	float RightEyeMirrorWait = 0.f;
	/** REQ-SPECCAM (user 2026-10-13): the PC window's Steady view (Options > Graphics > PC Window View, or the V key): a
	 *  scene capture at the head's place, turning smoothed (a one-euro filter on yaw and pitch), the horizon level (no roll),
	 *  the pitch damped, 90 degrees wide, 16:9, shown as the spectator screen's texture (cropped to the window's shape).
	 *  Made when Steady is chosen and destroyed (capture and target) when it is not: Headset costs nothing.  Never on the
	 *  Quest (compiled out).  OutlawsPlayerSpecCam.cpp. */
	void UpdateSpecCam(float Dt);
	void DropSpecCam(const TCHAR* Why);
	void CycleSpecView();
	/** -OLSpecCamTest / -OLSpecCamDesk: the Steady view also runs in the desktop game (tests; no spectator screen there). */
	static bool SpecCamDesktop();
	UPROPERTY(Transient)
	TObjectPtr<class USceneCaptureComponent2D> SpecCapture;
	UPROPERTY(Transient)
	TObjectPtr<class UTextureRenderTarget2D> SpecTarget;
	bool bSpecInit = false;
	float SpecYaw = 0.f, SpecPitch = 0.f, SpecYawRate = 0.f, SpecPitchRate = 0.f, SpecRawYaw = 0.f, SpecRawPitch = 0.f;
	uint32 SpecFrame = 0;
	float SpecMirrorWait = 0.f;
	bool bSpecTestHead = false;          // (the test) the head's rotation given, not read from the camera
	FRotator SpecTestHead = FRotator::ZeroRotator;
	FRotator SpecLastRaw = FRotator::ZeroRotator, SpecLastOut = FRotator::ZeroRotator;
	UStaticMeshComponent* WheelSlotComponent(int32 Index, UTextRenderComponent*& OutLabel);
	void Equip(int32 Hand, int32 Weapon);
	/** The weapon in hand From goes to the empty hand To, its rounds with it (hands together, the fist's grip). */
	void SwapWeaponHands(int32 From, int32 To);
public:
	/** The revolver drawn in the leading hand (the first level's start). */
	void DrawStartPistol();
private:
	TArray<int32> WheelPairPlates;                         // per wheel slot: "x2" when two are carried
	void PressXReleased();
	void PressAReleased();
	void UpdateGuns(float Dt);
	class AOutlawsEnemy* FindFanTarget(int32 Hand) const;
	/** The living bad guy nearest the middle of the view within ConeDeg, in range of From and in sight. */
	class AOutlawsEnemy* FindViewTarget(float ConeDeg, float RangeCm, const FVector& From) const;
	float BaseGravityScale = 1.5f;
	// the sector palette's colour grade on the camera (M_OLR_PaletteGrade), faded in and out
	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> PaletteMid;
	float PaletteAmount = 0.f;
	bool bPaletteGraded = false;
	// the headset's screen effects (its materials do the post process's sums, ue_olue_quest_look.py): the palette
	// grade's rows as last set, the underwater filter's amount, and the held gear's ink shell material
	FLinearColor QuestPalRows[3] = { FLinearColor(1.f, 0.f, 0.f, 0.f), FLinearColor(0.f, 1.f, 0.f, 0.f), FLinearColor(0.f, 0.f, 1.f, 0.f) };
	float UnderwaterAmount = 0.f;
	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInterface> GearInk;
	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> DeathMid;   // the death duotone (same grade material)
	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> BackdropMid;   // behind the menu and wheel: blur, and the death tone under the menu
	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> InkMid;        // the ink line round held gear (custom stencil 1)
	float BackdropBlur = 0.f, BackdropTone = 0.f;
	float BackdropDim = 0.f;             // the wheel: the world round it half as bright (0..1)
	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> UnderwaterMid;   // the underwater colour filter (M_OLR_Underwater)
	float SinkVel = 0.f;                 // how fast the water is letting the body sink now (its own count: the fly mode's braking would undo the pull)
	float SwimSinkSpeed = 90.f;          // cm/s: the fastest the water lets a body sink       // the land feel (sector GRAVITY -60); other sectors scale it
	float SwimSurface = 0.f;             // the water's surface where the body swims (Swimming(), each frame)
	bool bSurfaceLocked = false;         // the surface holds the body: no sink, the stick and the strokes move along it (user 2026-09-29)
	bool bDived = false;                 // dived from the surface: it holds the body again only once it swims back up to it
	// cm: how far under the surface a standing body's middle floats: 55 puts the waterline at the neck of the 174 cm
	// body, the eyes about 20 cm above it (user 2026-10-02: at 20 it sat at the chest)
	float SwimFloatDepth = 55.f;
	float DivePitch = -27.f;             // degrees: the head tilted down this far (30% of straight down) and moving is a dive
	double DiveUntil = 0.0;              // a kick down (crouch) lets go of the surface for a moment
	// (BUG-SURFACEJOLT) the top water sector the surface was read from; since when no water was found under a swimming
	// body (-1: found); the swim log's pacing; when the turn stick was last pushed up (its spring back is not a kick down)
	int32 SwimSector = -1;
	bool bSwimSurfaceAir = true;         // air over the water where the body swims (a real surface; else a flooded passage's roof)
	double SwimLostSince = -1.0;
	double SwimLogNext = 0.0;
	double SwimLogWindow = 0.0;
	int32 SwimLogCount = 0;
	double StickUpAt = -10.0;
	float SwimLastMiddle = -1e9f;        // the body's middle at the end of the last frame's swim (a drop between frames is logged)
	void SwimLog(const FString& What);
	/** -OLVb2Test: the plate pickup's hand logic runs without a headset (REQ-SHIELDNOEQUIP) */
	bool bTestPlatePickup = false;
	/** -OLShots -OLChestTest (REQ-CHESTPLATE): the plate picked up (worn, the hands as they were), where it hangs as the
	 *  head turns and looks down, dented by hits, broken; the wrist on the off hand, right- and left-handed.  Photographs
	 *  chest_fresh, chest_dented, chest_broken, wrist_right, wrist_left; "OLSHOT chest" lines, PASS / FAIL. */
	void ChestTestTick(float Dt);
	int32 ChestTestFails = 0;
	float ChestTestBaseYaw = 0.f;
	int32 ChestTestFreshVerts = 0;
	/** -OLChestNetTest (co-op and matches): a heavy plate given a few seconds in (again whenever none is worn), so the other
	 *  machine has one to draw on this player. */
	float ChestNetWait = 0.f;
	int32 ChestNetGiven = 0;
	/** -OLShots -OLVb2Test[=badge|shield|enemy|tnt]: the videobatch2 fixes' checks (OutlawsPlayerVb2Test.cpp) */
	void Vb2TestTick(float Dt);
	/** -OLShots -OLLevel=townex -OLPianoTest (REQ-PIANO): the keys pressed, the tune, the roll, the lockout (OutlawsPianoTest.cpp) */
	void PianoTestTick(float Dt);
	/** -OLShots -OLLevel=shgulch|shgranry -OLBoxPushTest (BUG-BOXPUSHONCE): each SHOVE box pushed to its last stop (OutlawsBoxPushTest.cpp) */
	void BoxPushTestTick(float Dt);
	/** Its first part: a townex save in the RC2/RC4 layout loads with every elevator as saved (true once done). */
	bool PianoOldSaveTick(int32& OutFails, int32& OutPasses);
	/** -OLShots -OLTreadTest[=sector] (BUG-SURFACEJOLT): treading at the surface through idle, swim-up, stroke, fire and
	 *  wander phases; the body never goes under its float depth (OutlawsPlayerTreadTest.cpp) */
	void TreadTestTick(float Dt);
	/** -OLShots -OLWaterHandsTest[=sector] (BUG-WATERHANDS): every water surface found between eyes over it and a hand
	 *  under it; a gun held under the surface shot with the water exception on and off (OutlawsPlayerTreadTest.cpp) */
	void WaterHandsTestTick(float Dt);
	/** -OLShots -OLUwEdgeTest[=sector] (BUG-UNDERWATERFLICKER): the eyes under the water, shots with the underwater blur's
	 *  taps clamped to the eye's rectangle, unclamped (the control) and clamped again (OutlawsPlayerTreadTest.cpp) */
	void UwEdgeTestTick(float Dt);
	/** -OLShots -OLWheelTopTest[=nowiden] (BUG-WHEELOCCLUDE): the wheel opened behind a near "door": every part drawn */
	void WheelTopTestTick(float Dt);
	/** -OLShots -OLSpecCamTest=shots|perf (REQ-SPECCAM): the PC window's views switched as the V key does, shots of each,
	 *  the smoothing measured against a shaken head, nothing left behind when off; perf: GPU and frame time off vs on
	 *  (OutlawsPlayerSpecCam.cpp) */
	void SpecCamTestTick(float Dt);
	/** the held gear's water exception (MPC_OLR_Lights WaterOn / WaterZ) */
	void SetHandWater(bool bAcross, float SurfaceZ);
	double StrokeUntil = 0.0;            // a hand stroke counts as moving for this long (the dive wants movement)
	double StrokeGlideUntil = 0.0;       // a stroke's glide: the water's cap on a fast rise waits till then
	// (BUG-SUBMERGE) a stroke that swims forward (the hands pulled back past the body) with the head down dives from the surface:
	// seconds of such a pull lately (decays at half speed between pulls), and the dive it allows lasts till StrokeDiveUntil
	float StrokeDiveCharge = 0.f;
	double StrokeDiveUntil = 0.0;
	FVector SwimCarry = FVector::ZeroVector;   // a stroke's push still to come (cm/s), handed out over the glide
	void UpdateGravity(float Dt);
	void UpdateScreenFx(float Dt);
	/** The camera's post process to neutral: no menu tone or blur, no wheel dimming, no low-health greying or
	 *  edges, no grade.  Every frame of a movie, and when a game starts from the title screen. */
	void ClearScreenFx();
	/** REQ-GAMMA: the Brightness slider into the camera's colour grade (OutlawsDeviceProfile::Gamma). */
	void ApplyGamma(struct FPostProcessSettings& PP);
	void UpdateHud(float Dt);
	void UpdateDeathView(float Dt);
	void FireGun(int32 Hand, bool bFan);
	/** Fan fire for the gun in hand H (the other hand raking its hammer); true when it fired.  REQ-FANPALM: a palm up never fans. */
	bool UpdateFan(int32 H, bool bPutAway);
	bool bFanPalmUp[2] = { false, false };
	struct FWeaponDef_Public { FVector Hammer = FVector::ZeroVector; };
	static FWeaponDef_Public WeaponFanZone(int32 Weapon);
	/** -OLHeldGunTest: a held gun's name and where it came from (OutlawsPlayerWeapons.cpp), e.g. gpistol_shipped. */
	FString HeldGunTestName(int32 Wi) const;
	/** (REQ-MUZZLEFLASH3D) -OLShots -OLFlash3DTest: each gun fired and its flash photographed from the side, at 45 degrees and
	 *  from the front, then the frame time while firing fast (OutlawsPlayerFlashTest.cpp). */
	void Flash3DTestTick(float Dt);
	void UpdateFx(float Dt);
	void SpawnPuff(const FVector& At, const FVector& Vel, float Size0, float Size1, float Life, float Alpha);
	/** One more free puff in the smoke pool (SpawnPuff's; FIRSTHITCH makes a shot's worth behind the load). */
	int32 AddPuffSlot();
	/** The hand's muzzle flash, tracer and spark components, made once (its first shot, or FIRSTHITCH behind the load). */
	void EnsureShotFx(int32 Hand);
	void EjectCase(int32 Hand);
	void RouteAudioToHeadset();
	UFUNCTION()
	void OnAudioDevicesListed(const TArray<FAudioOutputDeviceInfo>& Devices);
	UFUNCTION()
	void OnAudioDeviceSwapped(const FSwapAudioOutputResult& Result);

	void FindLevel();
	void ApplyModeSettings();
	void BuildHands();
	void FollowHead();
	void UpdateCrouch();
	void PlaceOrigin(float Dt);
	void Locomotion(float Dt, bool& bOutMoving);
	void Turning(float Dt, bool& bOutTurning);
	void Swimming();
	void Climbing(float Dt);
	bool TryGrab(int32 Hand);
	bool TryHold(int32 Hand);
	void ReleaseHeld(int32 Hand);
	bool TryHandUse(int32 Hand);
	void PushDoors(int32 Hand);
	void NoteDoorUse(int32 Hand);
	void UseMedicalBag();
	void PressMenu();
	void OpenMenu(bool bFront = false);
	void CloseMenu();
	void MenuBack();
	/** REQ-UNLOCKS: the unlock page carried on from (any button): marked seen, the next one or the main menu. */
	void UnlockPageContinue();
	FString UnlockPageId;                  // the unlock page's achievement (OutlawsUnlocks)
	bool ShowMenuPage(const FString& File, bool bPush);
	void UpdateMenu(float Dt);
	void MenuActivate(int32 Hand, int32 Index);
	/** REQ-TRIGGERSTEP, the one place the menu input presses a row (every page): Dir +1 steps a value row up, -1 down;
	 *  bStopAtEnds: number rows stop at their ends (a short tick on Hand there), named lists go round (OutlawsMenu::StepPress). */
	void MenuPress(int32 Hand, int32 Index, int32 Dir, bool bStopAtEnds);
	/** A hand's trigger on a row: two controllers, the right up and the left down; one controller, up and round again. */
	void MenuTriggerPress(int32 Hand, int32 Index);
	/** The desktop's mouse in a menu: the row in the view's middle; the left button up, the right down. */
	void MenuMousePress(bool bRight);
	/** The mouse's press on a row (the row found): the left button up, the right down, a number stopping at its ends. */
	void MenuMouseStep(int32 Index, bool bRight) { MenuPress(1, Index, bRight ? -1 : 1, true); }
	void MenuMouseLeft() { MenuMousePress(false); }
	void MenuMouseRight() { MenuMousePress(true); }
	/** A row a step changes (a choice, a number, an on/off), which either hand's trigger may step when the other points at it. */
	bool IsMenuValueRow(int32 Index) const;
	/** The menu row a ray meets (-1: none; Length: how far along), as the lasers pick them. */
	int32 MenuRowUnderRay(const FVector& O, const FVector& D, float& Length);
	/** -OLShots -OLTriggerStepTest (REQ-TRIGGERSTEP): value rows stepped by each trigger, one controller and the mouse. */
	void TriggerStepTestTick();
	void MenuToastText(const FString& Text);
	// the letters keyboard (user 2026-10-02: the Multiplayer page's player name; any page can open it for a line of text, a
	// saved server's name later): letters, digits, space and . - _ ', Shift for small letters, Delete, Cancel and OK
	FString KbdText, KbdPurpose, KbdTitle, KbdNote;
	int32 KbdMax = 16;
	bool bKbdLower = false;
	void OpenKeyboard(const FString& Purpose, const FString& Title, const FString& Initial, int32 MaxLen);
	/** M1: the Host Game page's password typed (true: it was this page's keyboard). */
	bool MpHostKeyboardDone(const FString& Purpose, const FString& Text);
	void BuildKeyboardPage(OutlawsMenu::FPage& Page) const;
	void KeyboardKey(const FString& Key);
	void KeyboardDone();
	/** -OLShots -OLMpMenuShot: Options > Multiplayer and the keyboard, photographed and worked (ol_mp_options, ol_mp_keyboard). */
	void MpMenuShotTick();
	/** -OLDevChecksTest[=nodev] (REQ-DEVCHECKS, OutlawsDevChecks.cpp): the hidden Pause > Dev checks page walked through. */
	void DevChecksTestTick(float Dt);
	/** -OLShots -OLCharPageShot: the pause menu's Switch character row, the character page (photographed), a choice. */
	void CharPageShotTick();
	/** -OLGfxMenuShot: the Graphics page photographed, each performance row pressed with its knob logged before and after. */
	void GfxMenuShotTick();
	/** -OLGammaTest (REQ-GAMMA): the Brightness row pressed on the Graphics page (the grade, the headset's QHurt.w and the
	 *  settings file each checked), the file read back, Restore defaults; then the view photographed at 1.0 before any change
	 *  and at 0.7, 1.0 and 1.6 (ol_gamma_*.png; the gate's mean luminance). */
	void GammaTestTick();
	void TitleLoadTestTick();        // (REQ-TITLEMENU: -OLTitleLoadTest=saves|slot|none)
	/** -OLLanternCostTest (REQ-LANTERNBOOST): standing still, the lantern out / lit / out / lit, 6 s each: the GPU frame time
	 *  of each (the last 5 s), the lantern's reach and colour in MPC_OLR_Lights L0 / C0 averaged over the lit windows. */
	void LanternCostTestTick();
	/** The tests above: the world's materials compiled for this feature level, then Hold s more (Quiet: the caller's static). */
	bool TestShadersReady(float& Quiet, float Hold);
	/** M1 host (OutlawsPlayerMpHost.cpp): the Host Game page and its Locations list, a row pressed, and -OLMpHostShot. */
	void BuildMpHostPage(const FString& File, OutlawsMenu::FPage& Page);
	void MpHostPress(const FString& Name);
	void MpHostShotTick();
	/** REQ-EXTRAS (OutlawsPlayerExtras.cpp): the Extras pages built, the achievements page drawn, their rows pressed. */
	bool BuildExtrasPage(const FString& File, OutlawsMenu::FPage& Page);
	bool ExtrasPress(const OutlawsMenu::FItem& I, int32 Index);
	bool IsAchievPage() const;
	void DrawAchievCanvas(class UCanvas* C, int32 Width, int32 Height);
	int32 AchievRowAt(double Px, double Py) const;
	static void AchievListBox(float& OutX, float& OutY, float& OutW, float& OutH);
	int32 AchievFocus = 0;
	static constexpr int32 AchievRowsShown = 4;
	FString MenuRowText(int32 Index) const;
	void RefreshMenuRows();
	bool LoadMenuSource();

public:
	/** The level is being rebuilt (death): drop everything, bare hands, full magazines next time. */
	void ResetForLevel();
	/** The level actor took health or armour off the player: a red flash as strong as the hit and a
	 *  buzz in both hands (Damage = what reached health; 0 when the plate took it all). */
	void OnHurt(float Damage, const FVector* From = nullptr);
	// the red of a hit from where it came (M_OLR_HurtDir): the rim facing the shooter, fading as the flash does
	UPROPERTY(Transient)
	TObjectPtr<class UMaterialInstanceDynamic> HurtDirMid;
	FVector2D HurtDir = FVector2D::ZeroVector;     // screen: x right, y up; zero = all round
	float HurtDirAmount = 0.f;
	float HurtPin = -1.f;                          // (-OLHurtTintTest: the red held all round at this amount; -1 off)
	/** Health reached 0: the view turns sepia and drops to the floor until a button respawns. */
	void OnDied();
	/** A message on the line fixed in the view, a little above centre (newest at the bottom). */
	void ShowMessage(const FString& Text, float Seconds = 3.f);
	/** REQ-KILLFEED: a match's kill line in the kill feed (over the notices, newest at the bottom, four at most, each fading
	 *  out after a few seconds); bMine: this player's own kill or death (drawn warmer). */
	void ShowKillLine(const FString& Text, bool bMine);
	/** -OLKillFeedTest=shot[,q3]: the feed filled with sample lines and photographed at the headset's eye size. */
	void KillFeedShotTick(float Dt);
	/** BUG-REMOTEGUNSOUND / BUG-TNTDEATHFREEZE tests (OutlawsPlayerTntTest.cpp, OutlawsPlayerWeapons.cpp): the revolver fired once as
	 *  this player's character (the sound it made, empty when it did not fire); a lit stick thrown from At with Vel (false: not thrown);
	 *  the newest stick in flight; the -OLTntDeathTest tick (single player: ShotTest). */
	FString TestFireRevolver();
	bool TestThrowTnt(const FVector& At, const FVector& Vel, float FuseSec);
	bool TestTntState(FVector& OutPos, bool& bOutResting) const;
	void TntDeathTestTick(float Dt);
	/** BUG-TNTBLUE / BUG-FUSEMATCH: the held and the thrown stick side by side (-OLTntBlueTest, OutlawsPlayerTntTest.cpp). */
	void TntBlueTestTick(float Dt);
	/** REQ-FLICKRELOAD (v1.1): the double barrel and the sawed-off break open.  Their barrels (the mesh ahead of the breech) turn on
	 *  a hinge under the breech: a forward flick of the wrist (or the reload button) snaps them open 40 degrees and throws the fired
	 *  shells out; a flick back snaps them shut, loaded from the reserve as the reload button loads (as many as it has, up to two).
	 *  The flick is the hand's turn about its own side-to-side axis, fast and mostly that turn, with the hand itself not swinging:
	 *  aiming and melee never trip it.  Other guns: no flick, their reload unchanged.  (OutlawsPlayerWeapons.cpp)
	 *  (REQ-FLICKRELOAD rules) the single-barrel Shotgun breaks the same way (one shell out, one in); no flick at all while the
	 *  other hand holds the forend, and no flick open with the gun fully loaded; the reload button held two-handed lets the front
	 *  hand go for the cycle and gives it the gun back when it shuts (its grip still held, the hand still at the forend). */
	static bool IsBreakGun(int32 Wi) { return Wi == 2 || Wi == 3 || Wi == 4; }
	bool IsBreakOpen(int32 Hand) const { return Hand >= 0 && Hand < 2 && bBreakOpen[Hand]; }
	/** Co-op: the pose's open barrels, bit 0 the left hand, bit 1 the right. */
	uint8 CoopBreakBits() const { return uint8((bBreakOpen[0] ? 1 : 0) | (bBreakOpen[1] ? 2 : 0)); }
	/** The hand's flick, read from its frame's turn since the last call: +1 forward (open), -1 back (shut), 0 none. */
	int32 FlickStep(int32 Hand, float Dt);
	/** The flick, the reload button's cycle and the barrels' swing, each frame for a held gun (bBlocked: the wheel or a put-away). */
	void TickBreak(int32 Hand, float Dt, bool bBlocked);
	bool OpenBreak(int32 Hand, const TCHAR* Why);
	bool CloseBreak(int32 Hand, const TCHAR* Why);
	int32 FlickOpens = 0, FlickCloses = 0, ShellsOut = 0;     // (tests: this session's counts)
	int32 FlickRefusedTwoHands = 0, FlickRefusedFull = 0;     // (tests: flicks refused, held two-handed or fully loaded)
	/** Tests: the other hand on this hand's forend (bOn), or let go; whether it holds it now (TestFrontHandAt: its grip moved to
	 *  the forend, or Away cm from it). */
	void TestSetTwoHand(int32 Hand, bool bOn) { if (Hand >= 0 && Hand < 2) { bSupport[1 - Hand] = bOn; bRegrip[1 - Hand] = false; } }
	bool TestTwoHand(int32 Hand) const { return Hand >= 0 && Hand < 2 && bSupport[1 - Hand]; }
	void TestFrontHandAt(int32 Hand, float Away);
	/** REQ-RELOADHIDEHAND: this hand is not drawn (it let go of the other hand's forend for a button reload, until the shut). */
	bool IsFrontHidden(int32 Hand) const { return Hand >= 0 && Hand < 2 && bFrontHidden[Hand]; }
	/** (tests, -OLFlickTest and -OLFlickNetTest) a button reload of break gun Wi in the right hand, held two-handed by the left
	 *  (its grip held, at the forend): the left hand hidden from the open to the shut and back on the forend after, the grip as it
	 *  was before. Out: what was seen, frame by frame in short. */
	bool TestHiddenHandReload(int32 Wi, FString& Out);
	/** Tests (-OLFlickTest, OutlawsPlayerFlickTest.cpp; the two-machine half -OLFlickNetTest, OutlawsCoopFlickTest.cpp): a gun put in
	 *  the hand with Loaded rounds (the rest fired) and Reserve shells carried; a motion of the hand's frame at 90 Hz, each turn rate
	 *  (deg/s; pitch + is the muzzle up) a bell over Seconds, the hand moving at HandSpeed cm/s; the hand still for Seconds. */
	void TestBreakArm(int32 Hand, int32 Wi, int32 Loaded, int32 Reserve);
	void TestHandMotion(int32 Hand, float PitchPeak, float YawPeak, float RollPeak, float Seconds, float HandSpeed);
	void TestHandStill(int32 Hand, float Seconds);
	int32 TestMag(int32 Hand) const { return Hand >= 0 && Hand < 2 ? Mag[Hand] : 0; }
	/** The flying shells of this player (age < life), and how many of them lie still. */
	int32 TestShellsFlying(int32& OutResting) const;
	void FlickTestTick(float Dt);
private:
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> BreakPivot[2];
	UPROPERTY(Transient)
	TObjectPtr<class UProceduralMeshComponent> BarrelMesh[2];
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInterface> ShellMat;
	bool bBreakOpen[2] = { false, false };
	bool bRegrip[2] = { false, false };          // this hand let go of the other hand's forend for a button reload: it takes it back on the shut
	// (REQ-RELOADHIDEHAND) this hand hidden while it waits for the forend (bRegrip during the cycle); only the drawing, the grip
	// rules stay as they are
	bool bFrontHidden[2] = { false, false };
	void SetFrontHidden(int32 Hand, bool bHide);
	float BreakShown[2] = { 0.f, 0.f };          // 0 shut .. 1 open (the barrels' swing)
	bool bBreakCycle[2] = { false, false };      // the reload button's cycle: open, a shell at a time, shut
	float BreakCycleTimer[2] = { 0.f, 0.f };
	// (BUG-RELOADPACE) the shells the opening's sound run has slices for (the cycle keeps their time), and the sound clock at the
	// last shut (a test's: the barrels' shut against the shut slice's start)
	int32 BreakLoads[2] = { 0, 0 };
	int32 BreakCycleIn[2] = { 0, 0 };
	double BreakShutClock[2] = { 0.0, 0.0 };
	FVector BreakHinge[2] = { FVector::ZeroVector, FVector::ZeroVector };    // model space
	// the flick's reading: the frame's turn last time, the run of fast pitch so far (deg, signed), its turn off that axis, its
	// fastest rate, and a run that swung the hand or turned the wrong way (no flick until it ends)
	FQuat FlickPrevQ[2] = { FQuat::Identity, FQuat::Identity };
	bool bFlickPrev[2] = { false, false };
	float FlickPitch[2] = { 0.f, 0.f }, FlickSide[2] = { 0.f, 0.f }, FlickPeak[2] = { 0.f, 0.f }, FlickCool[2] = { 0.f, 0.f };
	bool bFlickVoid[2] = { false, false };
	// (BUG-FLICKFEEL) the run's fastest hand motion in all and across the barrel, whether it was taken, met the cooldown, and why it
	// was voided (each run not taken is logged with its reason); how long the barrels have been open (a shut wants FlickShutAfter)
	float FlickHandMax[2] = { 0.f, 0.f }, FlickAcrossMax[2] = { 0.f, 0.f };
	bool bFlickTaken[2] = { false, false }, FlickSawCool[2] = { false, false };
	const TCHAR* FlickWhy[2] = { nullptr, nullptr };
	float BreakOpenFor[2] = { 0.f, 0.f };
	bool TestHandAcross = false;                 // (-OLFlickTest: TestHandMotion moves the hand across the barrel, a chop)
	// (BUG-FLICKUP, -OLFlickTest) the wrist rolled this many degrees about the barrel: TestHandMotion turns the gun about the level
	// (unrolled) axes, as a muzzle raised or dropped with the wrist canted; TestRollWrist rolls it (TestBreakArm puts it back to 0)
	float TestWristRoll = 0.f;
	void TestRollWrist(int32 Hand, float Deg);
	uint64 FlickFrame[2] = { 0, 0 };             // the frame last read (a gap: the turn since then is not one frame's)
	float FlickLogCool = 0.f;
	void EjectShell(int32 Hand);
	/** The break gun in Hand has shut: the front hand that let go for its reload takes the forend again if its grip is still
	 *  held and it is still near the forend, else it stays free. */
	void RegripAfterBreak(int32 Hand);
	/** A free slot in the spent cases' pool (made if there is room; else the oldest). */
	int32 TakeCaseSlot();
	/** Equip: a break gun's barrels split off at its hinge onto BreakPivot (a card, or any other gun: none). */
	void BuildBreakBarrels(int32 Hand, int32 Wi, bool bCard);
	struct FKillLine { FString Text; float Left = 0.f; bool bMine = false; };
	TArray<FKillLine> KillLines;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UTextRenderComponent>> KillFeedText;
	UPROPERTY(Transient)
	TArray<TObjectPtr<class UMaterialInstanceDynamic>> KillFeedMids;
	float KillFeedDist = 80.f;
	int32 KillFeedLogged = 0;
	void UpdateKillFeed(float Dt);
public:
	/** A button while dead: the level from the start, or (B / Y) the autosave. */
	void RequestRespawn(bool bFromAutosave = false);
	bool IsDeadView() const { return bDeadView; }
	bool IsMenuOpen() const { return bMenuOpen; }
	/** REQ-EXTRAS: a line of the remaster's loc_english.txt by its key (out_quack_name); empty without the remaster or the key. */
	FString MenuLoc(const FString& Key);
	/** REQ-EXTRAS: the main menu's Extras page, the main menu under it (after Extras > Credits). */
	void ShowExtrasPage();
	/** The menu shut from outside (the level's: the main menu takes a stray pause menu's place). */
	void CloseGameMenu() { CloseMenu(); }
	/** The title's main menu is up (not a pause menu). */
	bool IsFrontMenuUp() const { return bMenuOpen && bFrontMenu; }
	// ---- co-op (OutlawsCoop.cpp): what a session asks of this machine's player; single player never calls these ----
	/** A co-op level starts: the menu shut and the screen clean, as the title menu's own start does. */
	void CoopCloseMenu() { CloseMenu(); ClearScreenFx(); }
	/** M1: the letters keyboard for a co-op row (the game password). */
	void CoopOpenKeyboard(const FString& Purpose, const FString& Title, const FString& Initial, int32 MaxLen) { OpenKeyboard(Purpose, Title, Initial, MaxLen); }
	/** R8: a hand's grip as squeezed now, 0..1 (Hand 0 left, 1 right): co-op voice's push to talk. */
	float CoopGripAxis(int32 Hand) const { return Axis(Hand == 0 ? TEXT("OLVR_LeftGrip") : TEXT("OLVR_RightGrip")); }
	/** REQ-TUTORIAL (OutlawsTutorial.cpp): what the player has done, counted where it happens, so each card completes on
	 *  the action itself whichever hand or order did it. */
	struct FTutorialNotes
	{
		int32 WheelTakes[2] = { 0, 0 };   // a weapon taken from that hand's wheel
		int32 Shots = 0;                  // a round fired
		int32 ShotsHolding = 0;           // ... with a bad guy held in the other hand
		int32 Throws = 0;                 // a knife, a stick or anything thrown
		int32 LitThrows = 0;              // a stick thrown burning
		int32 Jumps = 0;
		int32 HeldPunches = 0;            // a fist landed on the bad guy held in the other hand
		int32 BagsUsed = 0;               // the medical bag used from a wheel
	};
	FTutorialNotes TutNotes;
	/** Tests: hand Hand's wheel opened, the slot of Weapon pointed at and the button let go, as a player takes a weapon. */
	bool TutorialTestWheelTake(int32 Hand, int32 Weapon);
	/** Tests: the medical bag (or another item) picked on hand Hand's wheel and the button let go. */
	bool TutorialTestWheelItem(int32 Hand, const FString& Item);
	/** Tests: the open page's row with this label pressed as a hand's trigger presses it (false: no such row). */
	bool TutorialTestPressLabel(const FString& Label);
	/** Tests: the open page's row labels, joined by " | ". */
	FString TutorialTestRows() const;
	/** Tests: a punch from Hand lands on the bad guy held in the other hand (a desktop has no swinging fist): the punch's
	 *  own blow and count, as UpdatePunches lands it. */
	bool TutorialTestPunchHeld(int32 Hand);
	/** The weapon in each hand (-1 bare). */
	int32 TutorialEquipped(int32 Hand) const { return Equipped[Hand & 1]; }
	/** Tests: hand Hand turned so its gun's bore (the line FireGun shoots along) points at Target. */
	void TutorialTestAimGun(int32 Hand, const FVector& Target);
	/** Tests: the map into a hand (the partner's marker on it, user 2026-10-03), and what the marker shows. */
	void CoopTestHoldMap(int32 Hand) { EquipTool(Hand, TEXT("imap")); }
	/** Co-op's own settings rows (the lobby's Bad guys) saved as the menu's are. */
	void CoopSaveSettings() { SaveSettings(); }
	/** The co-op partner's name over their head (user 2026-10-03: "the same font as the text about guns for the ammo"): one
	 *  of this player's text plates (the ammo count's font and outline, lowercase as typed), hung on the partner's plate root
	 *  at RelLoc; made once, moved to each new puppet.  CoopNamePlateText shows Text (or hides it). */
	void CoopNamePlate(USceneComponent* Parent, const FVector& RelLoc);
	void CoopNamePlateText(const FString& Text, bool bShow);
	int32 CoopNamePlateIdx = -1;
	const FString& CoopMapPartnerState() const { return MapPartnerLogged; }
	/** The pause menu opened by the other player's pause (milestone 7). */
	void CoopOpenMenu() { OpenMenu(false); }
	/** REQ-MPLOBBY: the lobby over the host's first level, as the title menu is (the Menu button and Back do not close it). */
	void CoopOpenFrontMenu() { OpenMenu(true); }
	/** A co-op page shown now (bPush: Back returns to the page under it). */
	void CoopShowPage(const FString& File, bool bPush) { if (bMenuOpen) ShowMenuPage(File, bPush); }
	/** REQ-MPLOBBY (b): a page (the score screen, the Host Game page) up as the title menu is, the menu opened for it if shut. */
	void CoopOpenPage(const FString& File, bool bPush = false) { if (!bMenuOpen) OpenMenu(true); if (bMenuOpen) ShowMenuPage(File, bPush); }
	/** REQ-MPLOBBY (b): the Host Game page filled with these options (the last match's, for the next one); not saved. */
	static void MpHostPrefill(const struct FOutlawsMpHostOptions& O);
	/** REQ-MPLOBBY (b)'s fit check: a title page's lines and rows as last drawn (view units, 1920 x 1080). */
	FBox2D CoopFrontInk() const { return FrontInk; }
	/** REQ-MPLOBBY (c)'s fit check: the open page as last drawn sits on the panel (a sheet page: every row on the sheet, clear
	 *  of the title and the help line; a #coop page: its lines and rows inside the parchment, above the help line); Said: what
	 *  was measured. */
	bool MenuPageFits(FString& Said) const;
	/** REQ-MPLOBBY (c)'s test: Root and every page under it (its links, then theirs) shown, scrolled to the top and the bottom,
	 *  each checked to fit; Root shown again at the end.  Returns the pages walked. */
	int32 MenuFitWalk(const FString& Root, TFunctionRef<void(bool, const FString&)> Check);
	/** REQ-MENUCENTRE's check on the open sheet page (placed as it opens): the middle row's height against the eyes' line and
	 *  the gap from the last row to the prompt line, world cm; false when not a sheet page. */
	bool MenuCentreMeasure(float& OutMiddleOffCm, float& OutPromptGapCm) const;
	/** A page shown for a test (not pushed), and the page open now. */
	bool TestShowPage(const FString& File) { return bMenuOpen && ShowMenuPage(File, false); }
	const OutlawsMenu::FPage* TestMenuPage() const { return bMenuOpen ? MenuPage.Get() : nullptr; }
	/** BUG-MPRESUME: the title menu's Resume Game (the last single-player autosave) is offered: never in a co-op or match
	 *  session, where it would load the single-player game. */
	static bool ResumeGameOffered(const UObject* Context);
	/** REQ-TITLEMENU: the title menu's Load Game can be pressed: a save to load (the resume point, a co-op autosave or one of
	 *  the player's own), and never in a co-op or match session (BUG-MPRESUME: it would load a single-player game). */
	static bool TitleLoadOffered(const UObject* Context);
	static bool AnySaveToLoad();
	/** Back to the page under this one (a co-op sub-page done). */
	void CoopMenuBack() { if (bMenuOpen) MenuBack(); }
	/** HM1: the "Ride out to ...?" page over the office (the pause menu opened on it). */
	void HomAskRide() { if (!bMenuOpen) OpenMenu(); if (bMenuOpen) ShowMenuPage(TEXT("#homride"), false); }
	/** The open menu page's name (empty: none). */
	FString CoopMenuPage() const;
	/** REQ-MPMENU: the page the join page was opened from (while it is built: the open page, or under it on a rebuild). */
	FString CoopMenuParent() const;
	/** BUG-READYFLIP's test: the open page's row named Name pressed as a hand's trigger presses it (false: no such row). */
	bool CoopPressRow(const FString& Name);
	/** REQ-RANDOMOVE's test: the open page's link to Target pressed as a hand's trigger presses it (false: no such row). */
	bool TestPressLink(const FString& Target);
	/** BUG-READYFLIP's test: the label the open page shows on its row named Name (empty: no such row). */
	FString CoopRowLabel(const FString& Name) const;
	/** A line under the menu for a few seconds. */
	void CoopToast(const FString& Text) { MenuToastText(Text); MenuToastLeft = 5.f; }
	/** BUG-HOSTPAUSE: the host's app paused: this client's world held still as behind the single-player menu (the body,
	 *  head and hands follow the headset; no walking, no shooting).  Called again each frame while on (a menu closed
	 *  meanwhile sets the world's clock back). */
	void CoopHostPauseFreeze(bool bOn);
	bool IsCoopHostPauseFrozen() const { return bHostPauseFrozen; }
	/** A message on the line taken off before its time. */
	void DropMessage(const FString& Text);
	/** What hand H (0 left, 1 right) holds: a weapon index (-1 bare) and a tool or gear item (empty: none). */
	void CoopHeld(int32 Hand, int32& OutWeapon, FString& OutTool) const { OutWeapon = Equipped[Hand & 1]; OutTool = HeldTool[Hand & 1]; }
	/** A pickup in a hand (or flying to the chest) turned out to be the other player's: out of the hand, nothing taken, the
	 *  hand's weapon or tool back in it. */
	void CoopLetGoOf(const AActor* Item)
	{
		for (int32 H = 0; H < 2; ++H) if (Item && HeldItem[H].Get() == Item) { HeldItem[H] = nullptr; RestoreStowed(H); }
		Pulls.RemoveAll([Item](const FPull& Pl) { return Pl.Item.Get() == Item; });
	}
	/** A pickup just collected (any way: walked over, tucked away, a hand's pull): its look flies to the chest, shrinking,
	 *  then it goes; false when it has flown there already (the caller destroys it). */
	bool FlyCollected(AActor* Item);
	/** Co-op tests (milestone 6): a revolver shot, an unlit stick or a knife thrown, a tool taken in hand, as the hands do
	 *  them; and the hand's frame and box for holding the plate up on the desktop. */
	void CoopTestFire(bool bFan = false) { Equip(1, 0); FireGun(1, bFan); }
	void CoopTestThrow(int32 Weapon) { Equip(1, Weapon); ThrowHeld(1, true, false); }
	/** (BUG-REMOTETNT) A stick (Type 1) or a knife (2) another player threw, built on OnActor as this player's own thrown copy
	 *  is (ThrowHeld): the held model of that weapon, its textures on the held material with the pull off, and for a lit
	 *  stick its wick and flame.  False (Out empty) when there is no such model here. */
	bool BuildThrownLook(AActor* OnActor, int32 Type, bool bLit, FOutlawsThrownLook& Out);
	/** Its wick burnt down to FuseLeft seconds and the flame on its end, as a thrown stick's of this player's own. */
	void PlaceThrownFuse(const FOutlawsThrownLook& Look, float FuseLeft);
	/** (tests) This player's thrown things put away at once, none to go off. */
	void TestClearThrown();
	/** Co-op tests (the puppet's): the run held on or let go, as the run key does. */
	void CoopTestRun(bool bOn) { bRunning = bOn; }
	void CoopTestTool(int32 Hand, const FString& Item) { EquipTool(Hand & 1, Item); }
	/** (tests, BUG-LANTERNNET) the headset's lantern rule on the desktop: lit while held with oil, out once put away. */
	bool bTestLampFollowsHand = false;
	/** (tests) The gear put away from whichever hand holds it, as the wheel's second pick does. */
	void CoopTestToggleGear(int32 Hand, const FString& Item) { ToggleGear(Hand & 1, Item); }
	USceneComponent* CoopHandFrame(int32 Hand) const { return FrameOf(Hand & 1); }
	UOutlawsBoxHand* CoopHandBox(int32 Hand) const { return (Hand & 1) == 0 ? LeftHand.Get() : RightHand.Get(); }
	/** Co-op (milestone 6 item 4): the bad guy hand H holds (null: none).  Tests: hand H held out at At with its grip
	 *  squeezed (or let go) as a desktop has no tracked hand (OutlawsCoopGrabs.cpp), a bad guy taken into it, and let go of
	 *  with a throw, as the hands do them. */
	class AOutlawsEnemy* CoopGrabbed(int32 Hand) const;
	void CoopTestHand(int32 Hand, const FVector& At, bool bGrip);
	FVector CoopGripAt(int32 Hand) const;
	/** Co-op host: a bad guy in the other player's hand passes through this player's body (the other machine draws this
	 *  player with no body to meet), and thrown (bFling) till it lands and is clear, as this player's own throws do. */
	void CoopIgnoreEnemy(class AOutlawsEnemy* E, bool bFling);
	bool CoopTestGrab(int32 Hand, class AOutlawsEnemy* E) { return TryGrabEnemy(Hand & 1, E); }
	void CoopTestLetGo(int32 Hand, const FVector& Vel) { LetGoEnemy(Hand & 1, true, &Vel); }
	/** (BUG-ARCSTUCK) a throw of the held man lined up (the trigger held) or not, and whether that hand's arc stands. */
	void CoopTestAimArc(int32 Hand, bool bOn)
	{
		const TCHAR* Trig = (Hand & 1) ? TEXT("OLVR_RightTrigger") : TEXT("OLVR_LeftTrigger");
		bLaunchArmed[Hand & 1] = false;
		bGrabAiming[Hand & 1] = bOn;
		if (bOn) TestAxes.Add(Trig, 1.f);
		else TestAxes.Remove(Trig);
	}
	bool CoopTestArcShown(int32 Hand) const { return ArcDotsVisible(Hand & 1); }
	/** (BUG-TNTARCTRIGGER) one step of the dynamite arc check in that hand: 0 a stick taken, 1 no arc yet and the trigger held,
	 *  2 the fuse lit and the arc out, the trigger let go, 3 thrown and the arc gone.  Logs "<Who> tntarc check PASS|FAIL"; false on a fail. */
	bool TntArcTestStep(int32 Hand, int32 Stage, const TCHAR* Who);
	uint32 CoopPageSeen = 0;              // the co-op page's version last built (OutlawsPlayerMenu.cpp)
	/** The title flow: the main menu, which stays up until a game starts. */
	void OpenFrontMenu() { OpenMenu(true); }
	/** The title screens: bare hands but a revolver in the leading hand, no ammo readout, to point and shoot at
	 *  the menu (user 2026-09-27). */
	void HoldMenuPistol();
	bool bMenuPistol = false;
	/** A save: the guns in hand and their rounds, the tools, stamina. */
	void WriteSave(class FJsonObject& O) const;
	void ReadSave(const class FJsonObject& O);
	/** One of the five guns (revolver, rifle, shotgun, double barrel, sawed-off) as built, James's own: the weapon table's
	 *  values before any character's own ITM is worn (SetWeaponCharacter, BUG-CHARLOADOUT).  Pellets a barrel (at least
	 *  1) and barrels a shot (at least 1).  The bots' guns (OutlawsCoopBots.cpp).  False outside 0..4. */
	struct FStockGun
	{
		const TCHAR* Label = nullptr;
		const TCHAR* Sound = nullptr;
		float Damage = 0.f;
		int32 Pellets = 1;
		float Spread = 0.f;
		float RangeCm = 0.f;
		const TCHAR* AmmoItem = nullptr;
		int32 Rounds = 1;
		float Variance = 0.f;
		const TCHAR* Item = nullptr;
	};
	static bool StockGun(int32 Weapon, FStockGun& Out);

private:
	void Pulse(int32 Hand, float Amplitude);
	/** A hit's haptic, by the swing's speed (OutlawsFeel::ImpactPulse): a tap faint, a hard blow strong. */
	void PulseImpact(int32 Hand, float Speed);
	/** No (user 2026-10-01: one refusal everywhere): a short double buzz and the game's own "locked" rattle. */
	void Refuse(int32 Hand);
	void UpdateRefuse(float Dt);
	// the grip editor (a dev tool on the wheel, user 2026-10-01): the free fist squeezes on what the other hand holds (a gun,
	// a tool, the gear), moves and turns it about, and let go it stays so in that hand.  BUG-GRIPSAVE (user 2026-10-04): every
	// placement is kept in Saved/GripEditor/grips.json, loaded at the start over the baked defaults (OLGrips::Baked); a right
	// hand's placement mirrors onto the left (REQ-GRIPMIRROR) unless the left was placed itself; the wheel's "Reset grips"
	// deletes the file
	bool bGripEditor = false;
	TSet<FString> GripPlaced;                     // the keys placed by hand (a mirrored left is not one)
	static FString GripSavePath();
	void LoadGrips();
	void SaveGrips() const;
	void ResetGrips();
	void PlaceGrip(const FString& Key);
	/** A gun's place in its hand before any placement: the baked grip, else the weapon table's (Y mirrored on the left). */
	static FVector DefaultGunLoc(int32 Weapon, bool bLeft);
	/** The baked placement for "weapon:<Label>" or "tool:<item>", the right hand's (the left: Y mirrored by the caller). */
	static bool BakedGrip(const FString& What, FVector& Out);
	static FString WeaponLabel(int32 Weapon);
	int32 EditHand = -1;                          // the fist moving a thing
	TWeakObjectPtr<USceneComponent> EditComp;
	FTransform EditRel;                           // the thing in the moving fist's frame
	FString EditKey;
	TMap<FString, FTransform> GripAdjust;         // "weapon:Revolver@R", "tool:ilantern@L" -> the thing in its hand's frame
	TMap<FString, FTransform> GripDefault;        // what each was before it was moved
	bool GripEditorInput(int32 Hand, float Grip);
	void FinishGripEdit();
	USceneComponent* GripEditTarget(int32 Hand, FString& OutKey) const;
	float RefuseSecond[2] = { -1.f, -1.f };     // till the double buzz's second half
	double RefuseSoundAt = 0.0;
	FVector ChestPoint() const;
	void UpdateBodyHeight();
	void ResizeBody(float NewHalf);
	void ReleaseGrab(bool bFling);
	bool StartMantle();
	void UpdateHands(float Dt);
	void UpdateVignette(float Dt, bool bActive);
	void UpdateWrist();
	void SmokeTest(float Dt);

	// input
	void DesktopForward(float V);
	void DesktopRight(float V);
	void DesktopTurn(float V);
	void DesktopLookUp(float V);
	void CrouchPressed();
	void CrouchReleased();
	void RunPressed();
	void RunReleased();
	void PressA();
	void PressB();
	void PressX();
	void PressY();
public:
	/** Trial layout (user 2026-09-26, reload and wheel swapped 09-27; the default): Y / B reload that hand's gun, X / A hold for the wheel.
	 *  Off, the Classic layout: a tap of X / A reloads, the dominant top button jumps and the other uses the medical bag.  The grips on
	 *  nothing do nothing in either (the jump is the right stick up, items are used from the wheel). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|VR")
	bool bFaceReload = true;
	/** REQ-ONECTRL (accessibility): play with one controller.  0 off (both, as always), 1 left only, 2 right only.  That
	 *  hand's stick moves (forward / back, a light sideways push strafes) and turns (a hard sideways push), its upper face
	 *  button jumps, its lower one holds the wheel (a tap reloads), its stick click toggles run (held a second: the pause
	 *  menu); a long gun fires one-handed at full accuracy; the title screens point with that hand. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Outlaws|VR")
	int32 OneController = 0;
	/** The one controller's hand (0 left, 1 right), or -1 with both in use. */
	int32 OneHand() const { return OneController == 1 ? 0 : OneController == 2 ? 1 : -1; }
	/** REQ-ONEGLYPH: the hand whose controller a prompt either hand answers draws: the one controller's, else the dominant. */
	int32 MainHand() const { return OneHand() >= 0 ? OneHand() : (LeftDominant() ? 0 : 1); }
	/** REQ-BUTTONGLYPHS (OutlawsPlayerGlyphs.cpp): a VR prompt draws the controller with the button to press ringed.
	 *  UpdateGlyphPrompt: a text render's prompt ("A / X: restart level") shown as glyph rows on a plate where the text
	 *  stands (the text hidden); false when not VR or the text names no button.  TutGlyph: the tutorial card's control,
	 *  shown under the notices while its line (TutGlyphLine) is up. */
	bool UpdateGlyphPrompt(UTextRenderComponent* Over, const FString& Text, bool bShow);
	void UpdateHudGlyph(float Dt);
	FOutlawsGlyphRow TutorialGlyph(const FString& CardId) const;
	FOutlawsGlyphRow TutGlyph;
	FString TutGlyphLine;
	/** REQ-TUTORIAL2: the tutorial's card panel up this frame (AOutlawsTutorialCard), its distance and its bottom and top
	 *  edges in the camera's frame (cm up at that distance): the card's glyph plate stands under it, the notices over it. */
	bool bTutCardUp = false;
	float TutCardDist = 80.f, TutCardBottomZ = 0.f, TutCardTopZ = 0.f;
	UFont* GlyphFont(bool bTitle);
	/** A menu canvas's VR prompt line ("B or Y: back", "more below", the title screens' help line) drawn as short labels
	 *  centred on Centre with the controllers under them; false when not VR or the line is not one (the caller draws its
	 *  text).  GlyphLineParts: the line's prompts; GlyphLineWidth: its drawn width, -1 when not one. */
	bool DrawGlyphLine(UCanvas* C, const FString& Str, UFont* F, const FVector2D& Centre, const FLinearColor& Ink, const FLinearColor& Outline,
		bool bMask, FBox2D& Out);
	bool GlyphLineParts(const FString& Str, TArray<OutlawsGlyphs::FPart>& Parts) const;
	float GlyphLineWidth(UCanvas* C, const FString& Str, UFont* F) const;
	/** (user 15 Oct) a menu canvas's drawing height in pixels: the size the death screen's are seen at (OutlawsPlayerMenu.cpp). */
	float MenuGlyphPx(const UCanvas* C) const;
	/** (REQ-BACKCLICK) the menus' drawing height in sheet cm: three quarters of the death screen's seen size. */
	float MenuGlyphCm() const;
	float MenuGlyphSeenCm() const;               // (the death screen's seen size at the menu's distance, sheet cm)
	/** (REQ-DEATHGAP) a titled sheet's gap from the title's letters to its first row's, seen from Dist cm away (the Options
	 *  page's layout, measured with the fonts on C); and the gap as last drawn on the open sheet, the same way (-1: none). */
	float SheetTitleGapSeenCm(UCanvas* C, float Dist);
	float SheetTitleGapDrawnSeenCm(float Dist) const;
	static constexpr float MenuGlyphShrink = 0.75f;
	/** (REQ-BACKCLICK) true when the ray from O along D is on the page's Back prompt: the sheet's foot line, or the wide sheets'
	 *  help plate in VR.  Length: how far out it meets it. */
	bool MenuBackUnderRay(const FVector& O, const FVector& D, float& Length) const;
	bool IsMenuBackHot() const { return MenuBackHover[0] || MenuBackHover[1]; }
	FBox2D SheetBackInk = FBox2D(ForceInit);     // (REQ-BACKCLICK) the foot line's Back as last drawn, canvas pixels
	/** -OLBackClickTest: the laser pointed at Back and the trigger pulled on several pages, each one level back (OutlawsPlayerMenu.cpp). */
	void BackClickTestTick(float Dt);
	/** The wide sheets' (the save screen's, the title pages') help line in VR: on a plate, the words with the controllers under
	 *  them; (BUG-MENUGLYPH, BUG-MENUGLYPH2) inside the sheet at its bottom right, Select over Back when both show. */
	void UpdateMenuHelpPlate(const FString& Line);
	static constexpr float MenuHelpInsetCm = 2.5f;      // (a wide page's prompts in from the sheet's right edge or the clean parchment's, and its foot, menu cm)
	FBox2D WideHelpInk = FBox2D(ForceInit);      // (BUG-MENUGLYPH2) a wide sheet's flat-screen help line as last drawn, 1920 x 1080 view
	/** (BUG-MENUGLYPH2) along a wide page's foot, where its clean parchment ends (the torn edge's first burn) and where the torn
	 *  edge ends on the right (the art past it), view x of 1920; both 1920 where the parchment fills the sheet. */
	float WideParchmentRight() const;
	float WideTornRight() const;
	UPROPERTY(Transient)
	TObjectPtr<UOutlawsGlyphPlate> MenuHelpPlate;
	float SheetFootBelow = 16.f;                 // (the foot line's ink under its middle as last drawn, canvas pixels)
	/** The menus' Back (B / Y, as bound), the pointer's trigger and the scroll stick. */
	OutlawsGlyphs::FGlyph BackGlyph() const { return OutlawsGlyphs::ForActions(this, { TEXT("OLVR_B"), TEXT("OLVR_Y") }); }
	OutlawsGlyphs::FGlyph SelectGlyph() const { return OutlawsGlyphs::ForControl(this, OutlawsGlyphs::ECtl::Trigger, -1); }
	OutlawsGlyphs::FGlyph ScrollGlyph() const { return OutlawsGlyphs::ForControl(this, OutlawsGlyphs::ECtl::Stick, -1); }
	/** -OLShots -OLGlyphTest=<right|left|classic|one|oneleft|remap>: every prompt photographed, the rings checked against
	 *  the bound input by pressing the ringed key (OutlawsPlayerGlyphs.cpp). */
	void GlyphTestTick(float Dt);
	UPROPERTY(Transient)
	TObjectPtr<UOutlawsGlyphPlate> DeathPlate;
	UPROPERTY(Transient)
	TObjectPtr<UOutlawsGlyphPlate> HudPlate;
	FString GlyphTestPrompt;                     // (the test: the death plate shows this instead)
	/** (REQ-HANDSCORE) The hand that holds the cutscene's score card (0 left, 1 right): the one controller's, else the off hand (the map's). */
	int32 ScoreCardHand() const { return OneHand() >= 0 ? OneHand() : (LeftDominant() ? 1 : 0); }
	USceneComponent* ScoreCardFrame() const { return FrameOf(ScoreCardHand()); }
	/** No fall damage until this world time (a level placing the player is not a fall). */
	void GrantFallGrace(double Until) { FallGraceUntil = FMath::Max(FallGraceUntil, Until); }
	/** (user 2026-10-02) The wheel up, a grip on the slot that hand's laser points at drops one of it in front of the player
	 *  (a weapon with what it carries, the plate, one medical bag, one key or other item); the wheel stays open, updated.
	 *  OutlawsWheelDrop.cpp. */
	bool WheelDrop(int32 Hand);
	/** Tests: the wheel opened on Hand (if it is not up), the slot of Weapon (-1 the fists, -2 none) or of Item pointed at,
	 *  and the grip's drop made; false when the slot is not on the wheel or nothing dropped.  bClose: the wheel put away after. */
	bool WheelDropTest(int32 Hand, int32 Weapon, const FString& Item, bool bClose);
	TWeakObjectPtr<AActor> LastWheelDrop;        // the pickup the last wheel drop made
	int32 WheelDropPick[2] = { -1, -1 };         // slot each laser points at, greyed ones too (a gun with no rounds drops)
	/** -OLShots -OLWheelDropTest (single player): OutlawsWheelDrop.cpp. */
	void WheelDropTestTick();
	/** REQ-CLIMBWHEEL: -OLShots -OLLevel=townex -OLNoFrontEnd -OLClimbWheelTest (OutlawsClimbWheel.cpp). */
	void ClimbWheelTestTick();
	/** REQ-CLIMBWHEEL's test steps, shared with its co-op run (-OLCoopAuto -OLCoopTest=climbwheel -OLCoopLevel=townex): bare
	 *  hands at the snapshot spot; the hand on the nearest ledge (true: hanging by it); a wheel pick (what the hand holds
	 *  after; bOutOpened: the wheel opened); the ledge let go of high (a fall). */
	void ClimbWheelTestPrepare();
	bool ClimbWheelTestHang(int32 HoldHand, const TCHAR* Tag);
	int32 ClimbWheelTestPick(int32 PickHand, int32 Weapon, bool& bOutOpened);
	void ClimbWheelTestLetGo(int32 HoldHand);
	/** REQ-ENEMYFLOAT: -OLShots -OLLevel=canyon -OLNoFrontEnd -OLEnemyFloatTest (OutlawsEnemyFloatTest.cpp). */
	void EnemyFloatTestTick();
	/** REQ-BUGSNAP (OutlawsBugSnap.cpp): both triggers and both stick clicks held 0.75 s, or F9, save a bug snapshot to
	 *  Saved/BugSnaps/<date_time>/ (shot.png, info.txt).  While the chord forms the triggers read 0 to the game and the stick
	 *  clicks do nothing, until all four are let go.  3 s between snapshots. */
	void UpdateBugSnap(float Dt);
	void TakeBugSnap(const TCHAR* Why);
	void BugSnapKey();
	/** -OLLevel=hideout -OLNoFrontEnd -OLBugSnapTest: the chord in play and in the pause menu, the cooldown, F9, and each folder's files. */
	void BugSnapTestTick(float Dt);
	FString BugSnapAmmoLine() const;             // (OutlawsPlayerWeapons.cpp)
	float RawAxis(const TCHAR* Name) const;      // (Axis without the chord's muting)
	bool bBugChord = false;
	bool bBugChordFired = false;
	float BugChordHeld = 0.f;
	double BugSnapReadyAt = 0.0;
	int32 BugSnapsTaken = 0;
	int32 GunFireCalls = 0;
	FString LastBugSnapDir;
	UPROPERTY(Transient)
	TObjectPtr<class UTextureRenderTarget2D> BugSnapTarget;
private:
	void PressLeftStick();
	void PressRightStick();
	void ReleaseLeftStick();
	void ReleaseRightStick();
	void StickPressed(int32 Stick);
	void StickReleased(int32 Stick);
	float HeadHeight() const;
	void CheckHeight(float Dt);
	void DoJump();
	/** A jump at the lower movement speeds reaches further than its walk alone would carry it, without going any faster
	 *  (user 2026-10-02: a first version scaled the speed across up and launched the player forward; "keep this slow", half
	 *  the full reach at 20 %, linear to all of it at 100 %): the speed across stays the walk's own, and the jump lasts
	 *  longer, as high as ever (launch / K, gravity / K^2), until the landing or the fall past the take-off's height. */
	virtual void OnJumped_Implementation() override;
	bool bJumpBoost = false;
	bool bLanternLow[2] = { false, false };    // the lantern hung lower: its fist holds the other hand's long gun
	float JumpStretch = 1.f;                   // K: how many times longer the jump lasts
	float JumpOffZ = 0.f;                      // the feet at the take-off
};
