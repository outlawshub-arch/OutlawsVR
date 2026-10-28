#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "OutlawsCoop.h"
#include "OutlawsRemotePlayer.generated.h"

class UStaticMeshComponent;
class UMaterialInterface;
class UTextRenderComponent;
class AOutlawsSpriteActor;
class AOutlawsLevelActor;
class UProceduralMeshComponent;

/** (BUG-REMOTETNT) A stick or a knife another player threw, drawn here as this machine's own thrown copy is
 *  (AOutlawsPlayer::BuildThrownLook): the held model, its textures on the held material, a lit stick's wick and flame. */
struct FOutlawsThrownLook
{
	TWeakObjectPtr<UProceduralMeshComponent> Mesh;
	TWeakObjectPtr<UStaticMeshComponent> Flame, Core;
	int32 FuseSection = -1;
	TArray<FVector> WickPos, WickNrm;
	TArray<FVector2D> WickUV;
	TArray<FLinearColor> WickCol;
	float GrowTo = 1.f;               // the level item's size, grown to over a second out of the hand (as ThrowHeld's)
};

/**
 * The other co-op player as this machine draws them (co-op milestone 2): never replicated, spawned here once a level is
 * built, eased toward the pose the other machine last sent (AOutlawsCoopLink::Pose).  Drawn (user 2026-10-02) as James
 * Anderson's own sprite (JAMES.NWX, the hero's multiplayer character), lit and portal-culled like a bad guy, facing where
 * that player looks, its choreography picked from what they do: standing, walking, running, crouched, crouch-walking, a
 * shot, a throw, a hurt, dying and lying dead.  The sprite is a separate actor owned by the level (UOutlawsCoop makes it,
 * a friend of the level); this actor places it.  No collision of any kind.  It only exists in a session.
 */
UCLASS(NotPlaceable)
class OUTLAWSRUNTIME_API AOutlawsRemotePlayer : public AActor
{
	GENERATED_BODY()

public:
	AOutlawsRemotePlayer();
	virtual void Tick(float DeltaSeconds) override;
	virtual void Destroyed() override;

	/** Which player this draws (0 the host, 1 the client), and the link their pose comes through. */
	int32 PlayerIndex = 0;
	TWeakObjectPtr<AOutlawsCoopLink> Link;

	/** BUG-LANTERNNET: this player's lantern lit (its pose's Lamp 2), its light's place as drawn here (the sent place, carried
	 *  with the body drawn) and its oil (0..1); false: put away, out, dead, a movie, nothing placed yet. */
	bool LanternShown(FVector& OutAt, float& OutOil) const { OutAt = LanternAt; OutOil = LanternOil; return bLanternLit; }

	/** (tests) The sprite drawn for this player (null: none yet). */
	const AOutlawsSpriteActor* ShownSprite() const { return Sprite.Get(); }

	/** Where the body is drawn now (the eased pose). */
	FVector ShownBody() const { return Body; }
	/** (tests) The pose buffer's jumps (a respawn, a teleport: no easing across) and its late frames carried on. */
	int32 PoseCuts() const { return BufferCuts; }
	int32 PoseExtrapolated() const { return Extrapolated; }
	/** (tests, GATE-FLAKES) this frame's draw: how far past the newest pose it ran on (0: between two poses), and the sender's
	 *  time between the newest two poses (a hitch on the sender: a long one). */
	double PoseLateBy() const { return LastLateBy; }
	double PoseNewestGap() const { return LastNewestGap; }
	FVector ShownHead() const { return Head; }
	float ShownHalf() const { return Half; }
	/** (tests, BUG-MPDEATHSOUND / BUG-BOTTNT) this machine's tallies: death cries heard from bodies, bodies a blast threw,
	 *  and the cries' and throws' lines (who, what, how far). */
	static int32 DeathCriesHeard;
	static int32 BodiesFlung;
	static int32 BodiesLanded;
	static TArray<FString> CryLines;
	static TArray<FString> FlingLines;
	/** (tests, BUG-TNTVANISH) bodies still in the air when their player came back, handed on as a body of their own: how many,
	 *  how many landed, the frames one was not drawn before it landed, the comebacks drawn at the new place (its pose's body
	 *  within a metre, a sprite of its own, half a second on) and their lines. */
	static int32 BodiesLeft;
	static int32 BodiesLeftBots;
	static int32 BodiesVanished;             // (-OLOldTntVanish: the bodies that went from the air with their player)
	static int32 BodiesLeftLanded;
	static int32 BodiesLeftUnseen;
	static int32 ComebacksAtSpawn;
	static int32 ComebacksChecked;
	static TArray<FString> LeftLines;
	/** BUG-TNTVANISH: a body a blast threw, still in the air when its player respawned here (or its sprite was changed): its
	 *  sprite flies on and lies where it lands as a corpse; a new sprite comes for the player (UOutlawsCoop: at once). */
	bool SpriteGivenAway() const { return bSpriteGiven; }

	/** Something this player threw (co-op milestone 6): a stand-in flies from Pos at Vel under gravity (times Grav), comes
	 *  to rest where it meets the level and goes after a moment (a lit stick: when its fuse is out or a blast comes near).
	 *  Kind: 1 dynamite, 2 a knife, 3 a bottle, 4 a prop that does not break, 5 a fired shotgun shell (REQ-FLICKRELOAD: out of
	 *  that player's broken-open gun, two seconds). */
	void AddFlyer(int32 Kind, const FVector& Pos, const FVector& Vel, float Grav, float Fuse);
	/** A blast here: a thrown stick's stand-in close to it is gone (it was that stick). */
	void EndFlyersNear(const FVector& At, double Radius);
	/** (tests) The shells drawn flying from this player so far, and those in the air or lying now. */
	int32 ShellsShown = 0;
	int32 ShellFlyers() const { int32 N = 0; for (const FFlyer& F : Flyers) N += F.Kind == 5 ? 1 : 0; return N; }
	/** (tests, BUG-REMOTETNT) Sticks and knives drawn with the thrower's own look, and those drawn as a stand-in shape. */
	int32 LooksShown = 0;
	int32 StandInsShown = 0;

	// ---- the sprite (user 2026-10-02) ----
	/** No sprite yet, or it is gone (a build of the level destroys every sprite the level owns): UOutlawsCoop makes one. */
	bool NeedsSprite() const { return !Sprite.IsValid(); }
	/** M1: the sprite set it is drawn with (james.nwx in co-op; the player's character in a match). */
	FString SpriteNwx;
	/** The sprite to draw this player with (null: none could be made), and the level it stands in (its light). */
	void SetSprite(AOutlawsSpriteActor* InSprite, AOutlawsLevelActor* InLevel);
	/** This player's gunshot, seen here (UOutlawsCoop::OnShotFx): the shoot choreography for its length. */
	void NoteShot();
	/** (REQ-MUZZLEFLASH3D) Which way a shot of this player's from Muzzle went, as near as this machine can tell: the head's
	 *  pitch, turned toward the muzzle (a gun held out to the side shoots that way). */
	FVector ShotDirFrom(const FVector& Muzzle) const
	{
		FRotator R = HeadRot;
		const FVector Out(Muzzle.X - Head.X, Muzzle.Y - Head.Y, 0.0);
		if (Out.SizeSquared() > 15.0 * 15.0) R.Yaw = Out.Rotation().Yaw;
		R.Roll = 0.0;
		return R.Vector();
	}
	/** This player threw something (UOutlawsCoop::OnThrow; Kind as AddFlyer's). */
	void NoteThrow(int32 Kind);
	/** What the sprite shows now (tests): "stand", "walk", "run", "crouch", "crouch-walk", "shoot", "throw", "hit", "die";
	 *  "none" before the first. */
	const TCHAR* ShownState() const;
	int32 ShownChor() const;
	/** The choreographies shown so far, in order of first showing, and how many changes were logged (tests). */
	FString ChorsSeen() const;

	// ---- the name and health bar over the head (user 2026-10-02) ----
	/** What the plate over this player's head shows now (tests and the log): the name line (or none), the health, the bar's
	 *  colour, the speaker. */
	FString PlateState() const;
	const FString& PlateName() const { return ShownName; }
	float PlateFraction() const { return ShownFrac; }
	/** The bar's colour: 0 green (over 66 %), 1 orange (33 to 66 %), 2 red (under 33 %, and dead: an empty red bar). */
	int32 PlateBand() const { return ShownBand; }
	bool PlateTalkShown() const { return bTalkShown; }
	bool PlateDead() const { return bShownDead; }
	static int32 HealthBand(float Fraction);
	/** (REQ-PLATEINITIAL) The plate's name line: the name and, when a character is worn on the link, its initial, e.g.
	 *  "T4H_Dingle (D)"; empty when no name is set (the bar alone). */
	static FString PlateLabel(const FString& Name, const FString& Character);
	static const TCHAR* BandName(int32 Band);
	/** Tests: the speaker shown as if this player were talking. */
	bool bTestTalk = false;

	/** REQ-CHESTPLATE (tests): the boiler plate drawn on this player's chest now (false: none drawn). */
	bool ChestPlateShown(FVector& OutAt, FRotator& OutRot) const;

	/** The crouch walk's legs for a crouched player going Speed (cm/s) at that player's Pace: the art's own pace at half
	 *  the walk, slower when slower; up to twice it (REQ-CROUCHRUN: a crouched run is twice the crouch walk), 1.5 times
	 *  with -OLOldCrouchRun as before. */
	static float CrouchWalkLegs(float Speed, float Pace);

private:
	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> ChestPlate;
	int32 ChestPlateBuilt = 0;
	float ChestYawShown = 0.f;
	bool bChestYawEased = false;
	double ChestLogAt = 0.0;
	void TickChestPlate(float Dt, const FOutlawsCoopPose& P, const FOutlawsCoopStats& St, bool bHide);
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> Root;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UObject>> KeepAlive;

	struct FFlyer
	{
		TWeakObjectPtr<USceneComponent> Mesh;
		FOutlawsThrownLook Look;           // (a stick or a knife: the thrower's own look; else Mesh is a stand-in shape)
		FVector Pos = FVector::ZeroVector, Vel = FVector::ZeroVector;
		float Grav = 1.f, Life = 3.f, Spin = 0.f, Fuse = -1.f, Age = 0.f;
		bool bShot = false;                // (-OLRemoteFlyerShots: shot once)
		int32 Kind = 0;
		bool bRest = false;
	};
	TArray<FFlyer> Flyers;
	void TickFlyers(float Dt);
	bool bPlaced = false;
	bool bLobbyHidden = false;
	/** R8: the poses as they came (by the sender's time), drawn BufferSeconds behind the newest: between the two around
	 *  that moment, or on past the newest along its way for up to ExtrapolateSeconds when the next is late. */
	struct FPoseSample { double T = 0.0; FVector Body = FVector::ZeroVector, Head = FVector::ZeroVector; FQuat HeadRot = FQuat::Identity; float Half = 88.f, Speed = 0.f; };
	TArray<FPoseSample> Samples;
	int32 LastSeq = 0;
	double ClockOffset = 0.0;                // this machine's seconds minus the sender's, at the quickest arrival seen
	bool bHaveOffset = false;
	int32 BufferCuts = 0, Extrapolated = 0;
	double LastLateBy = 0.0, LastNewestGap = 0.0;
	/** The place to draw now from Samples (false: none yet).  bOutCut: a jump (a respawn, a new level): no easing across it. */
	bool SamplePose(double Now, FPoseSample& Out, bool& bOutCut);
	FVector Body = FVector::ZeroVector, Head = FVector::ZeroVector;
	FRotator HeadRot = FRotator::ZeroRotator;
	bool bLanternLit = false;
	FVector LanternAt = FVector::ZeroVector;
	float LanternOil = 0.f;
	float Half = 88.f;

	UStaticMeshComponent* Part(const TCHAR* Shape, const FColor& Colour, const TCHAR* Name);
	UMaterialInterface* Flat(const FColor& Colour);

	// ---- the sprite and its choreography ----
	enum class EAnim : uint8 { None, Stand, Walk, Run, Crouch, CrouchWalk, Shoot, Throw, Hit, Die };
	static const TCHAR* AnimName(EAnim A);
	static int32 AnimChor(EAnim A);
	TWeakObjectPtr<AOutlawsSpriteActor> Sprite;
	TWeakObjectPtr<AOutlawsLevelActor> Level;
	bool bFreshSprite = false;               // just handed over: shows the wanted choreography at once (a body already lying)
	double Clock = 0.0;                      // this actor's own seconds (the events come between its ticks)
	float Speed = 0.f;                       // across the ground, cm/s (the pose's, smoothed)
	bool bMoving = false, bRunning = false, bCrouched = false;
	EAnim Action = EAnim::None;              // a shot, a throw or a hurt on show until ActionUntil
	double ActionUntil = 0.0;
	bool bActionRestart = false;             // the same action again (the next shot of a burst): from its start
	FString ActionWhy;
	EAnim Shown = EAnim::None;
	double ShownAt = 0.0;
	bool bWasDead = false;
	bool bSeenStats = false;
	float LastHealth = 0.f;
	int32 LastStatsSeq = 0;
	float LightCheck = 0.f;
	float FacingYaw = 0.f;                   // where the sprite faces (where the player looks; held while dead)
	float LogTokens = 4.f;                   // the chor lines: a few a second at most
	int32 LogSkipped = 0;
	int32 LogLines = 0;
	TArray<FString> Seen;
	void TickSprite(float Dt, const FOutlawsCoopPose& P, const FOutlawsCoopStats& St, float Pace);

	// ---- BUG-MPDEATHSOUND, BUG-BOTTNT: the death as this machine shows it: the character's cry from the body, and a body
	// a blast killed thrown up and away as a bad guy's is (the match only), from the blast this machine saw go off
	double DeadSince = 0.0;                  // real seconds this body was first seen dead (0: alive)
	bool bAliveSeen = false;                 // seen alive here (a body first seen dead neither cries nor flies)
	bool bFlingDecided = false, bFlingOn = false, bFlying = false;
	FVector FlingFeet = FVector::ZeroVector, FlingVel = FVector::ZeroVector, FlingStart = FVector::ZeroVector;
	double FlingAt = 0.0;
	void TickDeath(float Dt, const FOutlawsCoopStats& St);
	// one step of a thrown body's flight (gravity, a floor stops it, a wall turns it back at a third); false once it lies
	bool StepFling(FVector& Feet, FVector& Vel, float Dt, const AActor* Ignore) const;

	// ---- BUG-TNTVANISH: thrown bodies whose player came back before they landed: each its own sprite, flying on, then
	// lying where it fell (two kept a player, the oldest goes; a build of the level clears them with every sprite)
	struct FLeftBody
	{
		TWeakObjectPtr<AOutlawsSpriteActor> S;
		FVector Feet = FVector::ZeroVector, Vel = FVector::ZeroVector, Start = FVector::ZeroVector;
		double At = 0.0;                     // when it was thrown (real seconds)
		bool bFlying = true;
		float LightCheck = 0.f;
	};
	TArray<FLeftBody> LeftBodies;
	bool bSpriteGiven = false;               // the sprite went with a left body: a new one wanted at once
	bool bBodyLeft = false;                  // this death's body was handed on: no other drawn till alive again
	double ComebackCheckAt = 0.0;            // (tests) when to see the comeback stands at its pose's place
	FString LeftWho;
	bool LeaveBody(const TCHAR* Why);        // the flying body's sprite handed on (false: nothing in the air, or -OLOldTntVanish)
	void TickLeftBodies(float Dt);

	// ---- the plate over the head: the name (only when that player set one), a health bar under it, a speaker beside it
	// while their voice comes through; each a part drawn with the depth test (walls hide it), turned to this machine's eyes
	UPROPERTY(Transient)
	TObjectPtr<USceneComponent> PlateRoot;
	UPROPERTY(Transient)
	TObjectPtr<UTextRenderComponent> NameText;
	UPROPERTY(Transient)
	TObjectPtr<UTextRenderComponent> NameShadow;
	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> BarBack;
	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> BarFill;
	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> TalkIcon;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInterface>> FillMats;     // green, orange, red
	FString ShownName;
	float ShownFrac = 1.f;
	int32 ShownBand = 0;
	bool bTalkShown = false;
	bool bShownDead = false;
	bool bPlateFresh = true;
	double TalkUntil = 0.0;
	FString LoggedPlate;
	void BuildPlate();
	void TickPlate(const AOutlawsCoopLink& L, bool bHide);
	/** This player's voice is coming through now (the voice chat's word; false until it says). */
	bool VoiceActiveNow() const;
	float ChorSeconds(EAnim A, float Fallback) const;
	void LogChor(EAnim A, const FString& Why);
};
