#pragma once

#include "CoreMinimal.h"

/**
 * Outlaws INF scripts (Dark Forces lineage): per sector or wall "items", each a SEQ of class
 * blocks.  Elevators move sectors between STOPs; triggers send MESSAGEs to CLIENT sectors.
 *
 *   ITEM: SECTOR NAME: BANKDOOR01            ITEM: LINE NAME: ELLES1 NUM: #F491
 *     SEQ                                      SEQ
 *       CLASS: ELEVATOR MORPH_SPIN2              CLASS: TRIGGER STANDARD
 *       CENTER: 35.875 136.750                   CLIENT: ELLE
 *       EVENT_MASK: 32                           MESSAGE: GOTO_STOP 1
 *       SOUND: 1 DOOR5.WAV                       EVENT_MASK: 48
 *       SPEED: 60                              SEQEND
 *       STOP: 0 HOLD
 *       STOP: 85 HOLD
 *         MESSAGE: 1 OTHERDOOR NEXT_STOP     (at stop 1, tell OTHERDOOR)
 *     SEQEND
 */
namespace OutlawsInf
{
	// EVENT_MASK bits (Dark Forces meanings; Outlaws adds higher bits we do not use yet)
	constexpr uint32 EvCrossFront = 1;
	constexpr uint32 EvCrossBack = 2;
	constexpr uint32 EvEnter = 4;
	constexpr uint32 EvLeave = 8;
	constexpr uint32 EvNudgeFront = 16;   // nudge a line from its front / a sector from inside
	constexpr uint32 EvNudgeBack = 32;    // nudge a line from its back / a sector from outside
	constexpr uint32 EvExplosion = 64;
	constexpr uint32 EvShoot = 256;
	constexpr uint32 EvLand = 512;
}

enum class EOutlawsInfClass : uint8
{
	None,
	MoveFloor, MoveCeiling, MoveFC,
	MorphMove, MorphSpin,
	ScrollFloor, ScrollCeiling, ScrollWall,
	ChangeLight,
	VelocityX, VelocityY, VelocityZ,
	Explode, SlopeFloor, SlopeCeiling,
	TriggerStandard, TriggerSingle,
	Other
};

/** Trigger = Dark Forces m_trigger, what nudges and MESSAGE-less triggers send (elevators then
 * continue to their queued stop, MOVE_CEILING goes to its last stop).  Done re-arms a SWITCH1. */
/** Movie: TRIGGER_MOVIE <file> (the ranch's boss door plays ram.san mid-level; user 2026-10-03). */
enum class EOutlawsInfMsg : uint8 { NextStop, PrevStop, GotoStop, MasterOn, MasterOff, UserMsg, EndLevel, SpawnLevel, Trigger, Done, Movie, Other };

enum class EOutlawsStopAction : uint8 { Hold, Wait, Terminate, Complete };

struct FOutlawsInfMessage
{
	FString Target;          // sector name (empty = the trigger's clients / system)
	FString TargetId;        // "(#A8B2)" qualifier: sector or wall id, hex
	EOutlawsInfMsg Type = EOutlawsInfMsg::Trigger;
	int32 Arg = 0;
};

struct FOutlawsInfStop
{
	double Value = 0.0;
	bool bRelative = false;          // "@n": relative to the authored position
	FString SectorRef;               // a sector name as the stop value (its floor height)
	EOutlawsStopAction Action = EOutlawsStopAction::Wait;   // no delay given: wait 4 s, then go on (Dark Forces)
	double Wait = 4.0;
	TArray<FOutlawsInfMessage> Messages;
};

struct FOutlawsInfClass
{
	EOutlawsInfClass Class = EOutlawsInfClass::None;
	FString RawClass;
	bool bMoveObjects = false;       // MORPH_*2: carries objects standing on it
	uint32 EventMask = 0;
	bool bHasEventMask = false;
	bool bAnyEntity = false;         // ENTITY_MASK: * (or OBJECT_MASK: *): bad guys may work it too (the Jedi default: the player alone)
	double Speed = 0.0;
	bool bHasSpeed = false;          // no SPEED: the class default (8 u/s lifts, 15 u/s slides, 22.5 deg/s spins)
	int32 StartStop = 0;             // START: n
	double Angle = 0.0;
	FVector2D Center = FVector2D::ZeroVector;
	bool bHasCenter = false;
	bool bMasterOn = true;           // "MASTER: OFF" starts disabled
	int32 KeyObject = -1;            // OBJECT: n  (inventory item required)
	uint32 KeyObjectFlags = 0;       // OBJECT: n flags: 0x80000 uses the item up (gears, stone and round keys, the lever, the crank)
	int32 ExcludeObject = -1;        // OBJECT_EXCLUDE: n (fires only without that item)
	TArray<FOutlawsInfStop> Stops;
	TArray<FString> Slaves;
	TArray<FString> Clients;
	TArray<FOutlawsInfMessage> ClientMsg;   // each client's own MESSAGE line (the one after it), aligned with Clients
	TArray<bool> ClientHasMsg;
	TArray<FOutlawsInfMessage> TriggerMessages;   // messages of the class with no client before them
	TMap<int32, FString> Sounds;     // 1 start, 2 moving, 3 arrive

	bool IsElevator() const { return Class != EOutlawsInfClass::None && Class != EOutlawsInfClass::TriggerStandard && Class != EOutlawsInfClass::TriggerSingle && Class != EOutlawsInfClass::Other; }
	bool IsTrigger() const { return Class == EOutlawsInfClass::TriggerStandard || Class == EOutlawsInfClass::TriggerSingle; }
};

struct FOutlawsInfItem
{
	bool bLine = false;
	FString Name;                    // sector name
	FString NameId;                  // "(#id)" qualifier on the name, if any
	FString WallId;                  // LINE items: wall id (hex, without '#')
	TArray<FOutlawsInfClass> Classes;
};

class OUTLAWSRUNTIME_API FOutlawsInfFile
{
public:
	TArray<FOutlawsInfItem> Items;
	bool Parse(const FString& Text);
	/** "NAME(#ID)" -> ("NAME", "ID"). */
	static void SplitNameId(const FString& In, FString& OutName, FString& OutId);
	static EOutlawsInfClass ClassFromString(const FString& Kind, const FString& Sub, bool& bMoveObjects);
	FString Summary() const;
};
