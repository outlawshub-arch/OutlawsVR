#pragma once

#include "CoreMinimal.h"
#include "OutlawsInf.h"
#include "OutlawsLevel.h"

/**
 * Runs a level's INF: elevators (doors, lifts, moving floors and ceilings) move between their
 * stops, triggers forward messages.  Plain C++: the level actor feeds it player events and
 * applies what it reports (group transforms, height changes, sounds, text) to components.
 * Semantics follow the Jedi engine as The Force Engine reconstructs it (TFE_Jedi/InfSystem).
 *
 * Elevator values: MOVE_FLOOR / MOVE_FC = floor height, MOVE_CEILING = ceiling height (level
 * units); MORPH_MOVE = distance along ANGLE (units); MORPH_SPIN = degrees about CENTER, positive
 * counter-clockwise seen from above.
 * Elevators start at their START stop (0), then keep a queued next stop: HOLD sleeps until a
 * trigger or message, a number waits that many seconds and moves on, TERMINATE / COMPLETE end it
 * (their stop messages are not sent).  After the last stop comes stop 0.  A stop without a delay
 * waits 4 seconds.  MASTER_OFF freezes an elevator where it is; MASTER_ON resumes it.
 *
 * Locks: "OBJECT: n" on an elevator needs the inventory item whose ITM has USE_ID n (100 iron,
 * 101 brass, 102 steel key...).  Like the Jedi key check, it applies to player events and to
 * whatever triggers forward, not to messages sent from elevator stops.  The "You need the brass
 * key." line comes from the level's own OBJECT_EXCLUDE trigger, which fires only without the item.
 */
struct FOutlawsElevator
{
	int32 Item = -1;
	int32 ClassIndex = -1;
	EOutlawsInfClass Class = EOutlawsInfClass::None;
	FString Name;
	TArray<int32> Sectors;                 // master first, then slaves
	TArray<double> BaseFloor, BaseCeil;    // authored heights, per sector
	double Current = 0.0;
	double Speed = 0.0;                    // units (or degrees) per second, 0 = instant
	bool bTrigLast = false;                // a trigger sends it to its last stop (MOVE_CEILING)
	int32 Stop = 0;                        // last stop reached
	int32 Target = -1;                     // stop being moved to, -1 when not moving
	double WaitLeft = 0.0;
	bool bWaiting = false;
	bool bTerminated = false;
	bool bMasterOn = true;
	int32 Group = -1;                      // geometry group index (actor side)

	bool IsMorph() const { return Class == EOutlawsInfClass::MorphMove || Class == EOutlawsInfClass::MorphSpin; }
	bool IsVertical() const { return Class == EOutlawsInfClass::MoveFloor || Class == EOutlawsInfClass::MoveCeiling || Class == EOutlawsInfClass::MoveFC; }
	// (a CHANGE_LIGHT counts with the scrolls: no geometry of its own, it only gates a scroll; REQ-PIANO)
	bool IsScroll() const { return Class == EOutlawsInfClass::ScrollFloor || Class == EOutlawsInfClass::ScrollCeiling || Class == EOutlawsInfClass::ScrollWall || Class == EOutlawsInfClass::ChangeLight; }
};

/** SCROLL_FLOOR / SCROLL_CEILING / SCROLL_WALL: a surface texture sliding forever (the train's ground
 *  and scenery). Only the stop-less kind (runs from the level start, as TFE's "no stops" elevators). */
struct FOutlawsScroll
{
	int32 Item = -1;
	EOutlawsInfClass Class = EOutlawsInfClass::None;
	TArray<int32> Sectors;                 // master first, then slaves
	FVector2D Dir = FVector2D(0.0, 1.0);   // ANGLE as (sin, cos): 0 = +z
	double Speed = 0.0;                    // units a second (SPEED is texels a second, 8 to the unit)
	FVector2D Offset = FVector2D::ZeroVector;   // units the texture has slid
	int32 Elevator = -1;                   // with STOPs: the elevator whose value is the distance scrolled (texels)
	TArray<int32> OnlyWalls;               // walls of its (single) sector to move, when not all its scroll-flagged ones
	int32 Light = -1;                      // (REQ-PIANO) a CHANGE_LIGHT in its SEQ: the scroll runs only while that light is off its first stop
};

struct FOutlawsTrigger
{
	int32 Item = -1;
	int32 ClassIndex = -1;
	bool bLine = false;
	int32 Sector = -1;
	int32 Wall = -1;
	uint32 Mask = 0;
	bool bOneShot = false;                 // SINGLE, SWITCH1 (re-armed by DONE)
	bool bMaster = true;
};

class OUTLAWSRUNTIME_API FOutlawsInfRuntime
{
public:
	/** Callbacks the level actor implements. */
	TFunction<void(const FOutlawsElevator&)> OnElevatorMoved;          // apply geometry for its group
	TFunction<void(const FString& Wav, const FOutlawsElevator&)> OnSound;
	TFunction<void(const FString& Wav, const FOutlawsElevator&, bool bStart)> OnLoopSound;   // SOUND 2 while moving
	TFunction<void(int32 UserMsg)> OnUserMessage;
	TFunction<void(const FString& What)> OnLevelEvent;                  // END_LEVEL, SPAWN_LEVEL, COMPLETE
	TFunction<void(int32 ObjectId, bool bHasIt, const FOutlawsElevator&)> OnKeyCheck;   // a locked elevator was used
	TFunction<void(int32 ObjectId)> OnItemUsedUp;                       // an OBJECT with the use-up flag took the item (a gear into its slot)
	static constexpr uint32 ObjUseUp = 0x80000;                         // OBJECT: n 589824: the item goes into the lock
	TFunction<void(int32 Sector, int32 Wall, const FOutlawsInfMessage&)> OnWallMessage;   // "NAME(#wallid)" client: its ATX
	TFunction<void(const FString& ObjectId, const FOutlawsInfMessage&)> OnObjectMessage;   // "!#id": an object (a generator) by its id

	/** Live copy of the level: elevator heights are written here. */
	FOutlawsLevel Live;
	/** (QSPEED-3) bumped at every change of a sector's floor or ceiling height in Live; each sector keeps the count of its
	 *  last change (0 never), so a portal flood kept from before can tell whether a sector it read has moved since. */
	uint64 HeightGen = 0;
	TArray<uint64> SectorHeightGen;
	FOutlawsInfFile Inf;
	TArray<FOutlawsElevator> Elevators;
	/** The elevators a save from before REQ-PIANO holds (RC2, RC4): all but the scroll-gating lights appended after them.
	 *  Such a save puts these back by index; the lights keep their start. */
	int32 SavedElevatorsBefore = 0;
	TArray<FOutlawsTrigger> Triggers;
	TArray<FOutlawsScroll> Scrolls;
	TSet<int32> AutoDoors;                          // door-flagged sectors made doors here (their neighbours' faces move with them)
	TMap<int32, TArray<int32>> ElevatorsBySector;   // master sector -> elevators
	TMap<int32, TArray<int32>> TriggersBySector;    // sector items' triggers
	TMap<uint64, TArray<int32>> TriggersByWall;     // line items' triggers
	TArray<FString> Warnings;
	TSet<int32> Items;                              // inventory by USE_ID (keys, crowbar, badges...)
	bool HasItem(int32 Id) const { return Items.Contains(Id); }
	/** Every OBJECT / OBJECT_EXCLUDE id the level's INF refers to. */
	void RequiredObjects(TSet<int32>& Out) const;

	static uint64 WallKey(int32 S, int32 W) { return (uint64(uint32(S)) << 32) | uint32(W); }

	void Init(const FOutlawsLevel& Level, const FString& InfText);
	void Tick(double Dt);
	/** How much longer a stop's wait lasts than the level says (user 2026-10-02: the sawmill's timed doors could not be
	 *  made at the lower movement speeds): 1 / the player's pace, the walk and the wait in the original's balance. */
	double WaitScale = 1.0;

	/** Player events (Outlaws coordinates).  Event bits from OutlawsInf::Ev*.  True when a trigger
	 *  or an elevator took the event (a locked elevator counts: it answers with its rattle). */
	bool SectorEvent(int32 Sector, uint32 Event);
	bool WallEvent(int32 Sector, int32 Wall, uint32 Event);
	bool WallSlides(int32 Sector, int32 Wall) const;   // a bar switch (its client steps through more than two stops): the grip alone works it
	/** BUG-RANCHDOOR: while a sector event's triggers run (they go before its elevators), the level may ask that the
	 *  elevators the same event starts wait (the ranch's BOBDOOR: its TRIGGER_MOVIE and its own spin on one use).  Their
	 *  lock is checked and the key spent now; the move waits in HeldStarts until ReleaseHeld.  False outside a sector event. */
	bool HoldEventStarts();
	void ReleaseHeld();
	const TArray<int32>& GetHeldStarts() const { return HeldStarts; }
	/** Messages by name ("NAME" + optional id). */
	void Send(const FString& Target, const FString& TargetId, const FOutlawsInfMessage& Msg, bool bKeyChecked = false);

	/** Resolve a stop's value for an elevator. */
	double StopValue(const FOutlawsElevator& E, int32 StopIndex) const;
	/** Sectors named Name (and matching Id when given). */
	void FindSectors(const FString& Name, const FString& Id, TArray<int32>& Out) const;
	FString Summary() const;
	/** A save's copy of the moving state (elevator values and stops, trigger arming, scroll offsets, items). */
	void WriteState(class FJsonObject& O) const;
	void ReadState(const class FJsonObject& O);

	/** Default speed of an elevator class when the script gives none (Jedi defaults). */
	static double DefaultSpeed(EOutlawsInfClass Class);

	// ---- co-op (OutlawsCoopInf.cpp sets these in a session only; single player never does, and runs as before) ----
	/** The client's INF only shows the host's: a player event goes to the host here instead of running (when the level's
	 *  masks say anything listens for it), and Tick only eases moving elevators to their stops (no waits, no stop actions,
	 *  no sounds: the host's snapshot says what comes next). */
	TFunction<void(int32 Sector, int32 Wall, uint32 Event)> OnForward;
	/** The host: each event sent in on this machine (its own player's, an enemy's, the client's), as it begins (Phase 0)
	 *  and ends (1 nothing took it, 2 something did). */
	TFunction<void(int32 Sector, int32 Wall, uint32 Event, int32 Phase)> OnEventScope;
	/** The host (or single player): a sector event may be refused before anything runs (REQ-PIANO: the keys while the tune
	 *  plays and for 2 s after).  False refuses it: nothing takes it. */
	TFunction<bool(int32 Sector, uint32 Event)> OnEventGate;
	/** A scroll's texture is moving now (a light-gated one only while its light is up). */
	bool ScrollRuns(const FOutlawsScroll& Sc) const;
	/** The client: one elevator as the host has it.  A move both are making toward the same stop keeps its own easing
	 *  unless the host's value is more than a quarter second of travel away (no step back for the network's lag). */
	void SetFromHost(int32 Index, double Value, int32 Stop, int32 Target, bool bWaiting, bool bTerminated, bool bMasterOn);

private:
	bool ForwardEvent(int32 Sector, int32 Wall, uint32 Event);
	void TickPuppet(double Dt);
	void StartMove(FOutlawsElevator& E, int32 TargetStop);
	void Arrive(FOutlawsElevator& E);
	void Apply(FOutlawsElevator& E);
	bool TriggerAllowed(const FOutlawsTrigger& T) const;   // its OBJECT / OBJECT_EXCLUDE against the pockets
	void FireTrigger(FOutlawsTrigger& T, bool bAllowed = false);
	void FireTriggers(const TArray<int32>& Ts, uint32 Event, bool& bTaken);
	void UseUpKey(const FOutlawsInfClass& C);              // the OBJECT's item is spent when its flags say so
	void TriggerMessage(FOutlawsTrigger& T, const FOutlawsInfMessage& Msg);
	void ElevatorMessage(FOutlawsElevator& E, const FOutlawsInfMessage& Msg, bool bKeyChecked);
	int32 NumStops(const FOutlawsElevator& E) const { return ClassOf(E).Stops.Num(); }
	const FOutlawsInfClass& ClassOf(const FOutlawsElevator& E) const { return Inf.Items[E.Item].Classes[E.ClassIndex]; }
	const FOutlawsInfClass& ClassOf(const FOutlawsTrigger& T) const { return Inf.Items[T.Item].Classes[T.ClassIndex]; }
	int32 SectorEventDepth = 0;
	bool bHoldThisEvent = false;
	TArray<int32> HeldStarts;
};
