#pragma once

#include "CoreMinimal.h"
#include "OutlawsTypes.generated.h"

/**
 * The remaster's single-player difficulties (common.kpf menu/difficulty.rml: Good 0x100000,
 * Bad 0x200000, Ugly 0x800000) and the OBT object FLAGS word 2 bit that places an object on each
 * (LawMaker Level.pas LEV_OBJ_FLAG2_OBJECT_NOVICE / NORMAL / EXPERT).  The fourth bit, ADVANCED
 * (0x40000000), has no menu entry.  An object with no difficulty bits is on every difficulty.
 */
UENUM(BlueprintType)
enum class EOutlawsDifficulty : uint8
{
	Good UMETA(DisplayName = "Good (easy)"),
	Bad UMETA(DisplayName = "Bad (normal)"),
	Ugly UMETA(DisplayName = "Ugly (hard)"),
	All UMETA(DisplayName = "All objects (no filter)")
};

UENUM(BlueprintType)
enum class EOutlawsHand : uint8
{
	Left,
	Right
};

/** What "stick forward" means for VR locomotion. */
UENUM(BlueprintType)
enum class EOutlawsMoveDirection : uint8
{
	Head UMETA(DisplayName = "Where I look"),
	OffHand UMETA(DisplayName = "Where my off hand points"),
	DominantHand UMETA(DisplayName = "Where my dominant hand points")
};

/** A thing's last sector for AOutlawsLevelActor::CullSectorOf: where to look first, never the answer (a stale one, after
 *  a teleport, costs the full search). Gen: the level's sector boxes it was found with (a rebuild drops it). */
struct FOutlawsSectorHint
{
	int32 Sector = -1;
	uint32 Gen = 0;
};

namespace OutlawsDifficulty
{
	/** The object FLAGS word 2 bit for a difficulty; 0 = keep every object. */
	inline uint32 ObjectBit(EOutlawsDifficulty D)
	{
		switch (D)
		{
		case EOutlawsDifficulty::Good: return 0x10000000u;
		case EOutlawsDifficulty::Bad: return 0x20000000u;
		case EOutlawsDifficulty::Ugly: return 0x80000000u;
		default: return 0u;
		}
	}

	inline const TCHAR* Name(EOutlawsDifficulty D)
	{
		switch (D)
		{
		case EOutlawsDifficulty::Good: return TEXT("Good");
		case EOutlawsDifficulty::Bad: return TEXT("Bad");
		case EOutlawsDifficulty::Ugly: return TEXT("Ugly");
		default: return TEXT("All");
		}
	}
}
