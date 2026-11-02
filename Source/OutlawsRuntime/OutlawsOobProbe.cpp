// BUG-ICEJUMP's out-of-the-map probe (user 2 Nov: the rescue only where there is no floor under the player; a sector floor
// below, however deep, is a fall): -OLShots -OLNoFrontEnd -OLLevel=icecaves -OLOobProbe[=vr] [-OLOldOob].  Not invincible.
// (1) stood still on 78's lip (the ice: it slides off by itself): the fall into the death pit 232 (508 units) goes on past
// 4.25 s with no rescue (no move of over 3 m in a tick), the player dies on 232's floor, and the normal respawn (the level
// restarted, alive); (2) a body put outside every sector, falling: rescued (back inside a sector, over its floor, within
// 8 s); (3) dropped from WATERFALL03's floor height over POOL04 (53, floor -501, 525 units down; not water, its swirling
// floor only): it lands in 53, and leaves it, by the landing's death or, alive, by the one way on, the step into 232; then
// the normal respawn.  -OLOldOob: (1) only, the rescue mid-fall as before.  Lines "OLSHOT oob check PASS|FAIL: ..." and
// "OLSHOT oob RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsLevel.h"
#include "OutlawsGeometry.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsOobProbeLocal
{
	int32 Stage = 0, Passes = 0, Fails = 0, Teleports = 0;
	bool bArmed = false, bVRMode = false, bOld = false, bWasFalling = false, bDied = false, bLanded = false, bAskedRespawn = false;
	float T = 0.f, FallT = 0.f, LongestFall = 0.f, SettleT = 0.f;
	int32 LandedIn = -1, DiedIn = -1;
	FVector PrevAt = FVector::ZeroVector;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT oob check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}
}

OL_SHOT_MATCH(OLOobProbe) { return OLFlags::Has(TEXT("OLOobProbe")); }

OL_SHOT_TEST(OLOobProbe, 1550)
{
	using namespace OutlawsOobProbeLocal;
	if (!L || ShotTime < 0.f) return;
	if (!bArmed)
	{
		bArmed = true;
		FString Mode;
		OLFlags::Value(TEXT("OLOobProbe"), Mode);
		bVRMode = Mode.Equals(TEXT("vr"), ESearchCase::IgnoreCase);
		bOld = OLFlags::Param(TEXT("OLOldOob"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT oob start: %s, %s, %s"), *L->LevelName, bOld ? TEXT("-OLOldOob") : TEXT("new"), bVRMode ? TEXT("vr") : TEXT("flat"));
	}
	if (bVRMode) { bVR = true; BaseGravityScale = 1.5f; }     // (the headset's rules and its fall, this run only)
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT oob RESULT %s (%d passed, %d failed, %s, %s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			bOld ? TEXT("-OLOldOob") : TEXT("new"), bVRMode ? TEXT("vr") : TEXT("flat"));
		ShotTime = -1.f;
		Stage = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	if (Stage < 0) return;
	if (L->IsRebuildPending() || L->IsMoviePlaying() || bMenuOpen) { SettleT = 0.f; return; }
	L->StartSaveDelay = -1.f;           // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	L->bInvincible = false;
	const FOutlawsLevel* Lv = L->LiveLevel();
	UCharacterMovementComponent* M = GetCharacterMovement();
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, Half);
	const FVector Ol = FOutlawsGeometry::ToOutlaws(Feet, 0.2);
	SettleT += Dt;
	if (!Lv || SettleT < 3.f) { PrevAt = GetActorLocation(); return; }
	auto Put = [&](double X, double Y, double Z)
	{
		const FVector At = FOutlawsGeometry::ToUE(X, Y, Z) + FVector(0.0, 0.0, Half);
		TeleportTo(At, GetActorRotation(), false, true);
		M->StopMovementImmediately();
		L->HealPlayer(100.f);
		PrevAt = At;
		T = FallT = LongestFall = 0.f;
		Teleports = 0;
		bWasFalling = bDied = bLanded = bAskedRespawn = false;
		LandedIn = DiedIn = -1;
	};
	// each tick: a move of over 3 m is a rescue (or an unstuck); the longest unbroken fall; the landing's sector
	auto Watch = [&]()
	{
		T += Dt;
		if (FVector::Dist(GetActorLocation(), PrevAt) > 300.0) ++Teleports;
		PrevAt = GetActorLocation();
		const bool bFalling = M->IsFalling();
		const float FellFor = FallT;
		FallT = bFalling ? FallT + Dt : 0.f;
		LongestFall = FMath::Max(LongestFall, FallT);
		if (bWasFalling && !bFalling && !bLanded && FellFor > 0.5f)     // (not the settle onto the floor it was put on)
		{
			bLanded = true;
			const FOutlawsSector* S = Lv->SectorAt(Ol);
			LandedIn = S ? S->Index : -1;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT oob landed in sector %d after a %.1f s fall, health %.1f, dead %d"), LandedIn, LongestFall, L->GetHealth(), bDeadView ? 1 : 0);
		}
		bWasFalling = bFalling;
		if (bDeadView && !bDied)
		{
			const FOutlawsSector* S = Lv->SectorAt(Ol);
			DiedIn = S ? S->Index : -1;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT oob died in sector %d after a %.1f s fall"), DiedIn, LongestFall);
		}
		if (bDeadView) bDied = true;
	};
	// the normal respawn: the button's (RequestRespawn) a second into the death view, then the level back, alive
	auto Respawned = [&]() -> bool
	{
		if (bDeadView)
		{
			if (!bAskedRespawn && DeathTime >= 1.2f) { bAskedRespawn = true; RequestRespawn(false); SettleT = 0.f; }
			return false;
		}
		return bAskedRespawn;
	};
	switch (Stage)
	{
	case 0:
	{
		// 78's lip: the point on the slide line just short of x 571, in 78, 104 or 109
		const FOutlawsSector* S = Lv->SectorAt(568.0, 0.0, 800.0);
		if (!S || (S->Index != 78 && S->Index != 104 && S->Index != 109))
		{
			Check(false, FString::Printf(TEXT("78's lip at (568, 800) in 78/104/109 (sector %d)"), S ? S->Index : -1));
			Finish();
			return;
		}
		Put(568.0, Lv->SurfaceY(*S, FVector2D(568.0, 800.0), true) + 0.3, 800.0);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT oob (1) stood on sector %d's lip"), S->Index);
		Stage = 1;
		return;
	}
	case 1:
	{
		Watch();
		if (bOld)
		{
			if (Teleports == 0 && !bDied && T < 12.f) return;
			Check(Teleports > 0 && !bDied, FString::Printf(TEXT("(1, old rule) off 78's lip: rescued mid-fall (%d rescues, the fall %.1f s, dead %d)"), Teleports, LongestFall, bDied ? 1 : 0));
			Finish();
			return;
		}
		if (!bDied && T < 15.f) return;
		// (the headset's fall is past the old rescue's 4.25 s; the desktop's gravity, 4.59 g, ends it sooner)
		Check(Teleports == 0 && (!bVRMode || LongestFall > 4.25f), FString::Printf(TEXT("(1) off 78's lip: no rescue in a %.1f s fall (%d moves over 3 m)"), LongestFall, Teleports));
		Check(bDied && DiedIn == 232, FString::Printf(TEXT("(1) dies on 232's floor (died %d in sector %d)"), bDied ? 1 : 0, DiedIn));
		Stage = bDied ? 2 : 3;
		return;
	}
	case 2:
	case 6:
	{
		if (!Respawned()) return;
		if (SettleT < 3.f) return;
		const FOutlawsSector* S = Lv->SectorAt(Ol);
		Check(!bDeadView && S && L->GetHealth() > 0.f, FString::Printf(TEXT("(%d) the normal respawn: alive in sector %d, health %.1f"), Stage == 2 ? 1 : 3, S ? S->Index : -1, L->GetHealth()));
		Stage = Stage == 2 ? 3 : 9;
		return;
	}
	case 3:
	{
		// a body outside every sector: past the level's plan box, at its middle height
		FBox2D Box(ForceInit);
		double Lo = 1e30, Hi = -1e30;
		for (const FOutlawsSector& S : Lv->Sectors)
		{
			for (const FVector2D& V : S.Verts) Box += V;
			Lo = FMath::Min(Lo, double(S.Floor.Y));
			Hi = FMath::Max(Hi, double(S.Ceil.Y));
		}
		Put(Box.Max.X + 300.0, 0.5 * (Lo + Hi), Box.Max.Y + 300.0);
		Stage = 4;
		return;
	}
	case 4:
	{
		Watch();
		const FOutlawsSector* S = Lv->SectorAt(Ol);
		const bool bIn = S && Lv->SurfaceY(*S, FVector2D(Ol.X, Ol.Z), true) <= Ol.Y + 0.01;
		if (!(Teleports > 0 && bIn) && T < 8.f) return;
		Check(Teleports > 0 && bIn, FString::Printf(TEXT("(2) outside every sector: rescued in %.1f s (%d moves over 3 m, now in sector %d)"), T, Teleports, S ? S->Index : -1));
		Stage = 5;
		return;
	}
	case 5:
	{
		// over POOL04 at WATERFALL03's floor height (24), let go
		const FOutlawsSector* Pool = Lv->Sectors.IsValidIndex(53) ? &Lv->Sectors[53] : nullptr;
		if (!Pool) { Check(false, TEXT("(3) sector 53")); Finish(); return; }
		FVector2D C = FVector2D::ZeroVector;
		for (const FVector2D& V : Pool->Verts) C += V;
		C /= FMath::Max(1, Pool->Verts.Num());
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT oob (3) POOL04 '%s' flags 0x%x (water %d), floor %.0f, dropped from 24 at (%.1f, %.1f)"), *Pool->Name, Pool->Flags,
			(Pool->Flags & OutlawsFlags::Water) ? 1 : 0, Pool->Floor.Y, C.X, C.Y);
		Put(C.X, 24.0, C.Y);
		Stage = 7;
		return;
	}
	case 7:
	{
		Watch();
		if (!(bLanded && LongestFall > 1.f) && !bDied && T < 15.f) return;
		if (bDied && LandedIn < 0) LandedIn = DiedIn;
		Check(Teleports == 0 && LandedIn == 53, FString::Printf(TEXT("(3) POOL04 reached: landed in %d after a %.1f s fall, no rescue (%d), health %.1f, dead %d"), LandedIn, LongestFall, Teleports,
			L->GetHealth(), bDied ? 1 : 0));
		if (bDied)
		{
			Check(true, TEXT("(3) POOL04 left: the landing killed"));
			Stage = 6;
			return;
		}
		T = 0.f;
		Stage = 8;
		return;
	}
	case 8:
	{
		// alive in the pool: toward its opening into 232 (the death pit, one unit up), the only way on
		Watch();
		const FOutlawsSector& Pool = Lv->Sectors[53];
		FVector Toward = FVector::ZeroVector;
		for (const FOutlawsWall& W : Pool.Walls)
		{
			if (W.Adjoin != 232) continue;
			const FVector2D Mid = 0.5 * (Pool.Verts[W.V1] + Pool.Verts[W.V2]);
			Toward = (FOutlawsGeometry::ToUE(Mid.X, 0.0, Mid.Y) - FVector(GetActorLocation().X, GetActorLocation().Y, 0.0)).GetSafeNormal2D();
			break;
		}
		const FRotator From(0.0, GetControlRotation().Yaw, 0.0);
		const FVector Fwd = bVR ? FRotator(0.0, Camera->GetComponentRotation().Yaw, 0.0).Vector() : From.Vector();
		const FVector Right = FRotator(0.0, Fwd.Rotation().Yaw + 90.0, 0.0).Vector();
		HeldStick = FVector2D(FVector::DotProduct(Toward, Right), FVector::DotProduct(Toward, Fwd));
		HeldStickLeft = 0.2f;
		if (!bDied && T < 10.f) return;
		HeldStickLeft = 0.f;
		const FOutlawsSector* S = Lv->SectorAt(Ol);
		Check(bDied, FString::Printf(TEXT("(3) POOL04 left, alive, by the step into 232: died %d in sector %d after %.1f s"), bDied ? 1 : 0, S ? S->Index : -1, T));
		Stage = bDied ? 6 : 9;
		return;
	}
	default:
		Finish();
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
