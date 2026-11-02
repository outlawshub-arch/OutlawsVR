// BUG-FALLDAMAGE (user 2 Nov: icecaves' WATERFALL03 into POOL04, 157.5 m, left the desktop player alive):
//   -OLShots -OLLevel=icecaves -OLNoFrontEnd -OLFallDamageProbe [-OLOldFallDamage]
// The player dropped from rest over POOL04's floor from 3, 5, 6, 10, 15, 22.6 and 30 m and from WATERFALL03's floor height
// (157.5 m), on the desktop's gravity (BaseGravityScale 4.59) and the headset's (1.5); invincible, so the damage is only
// worked out (LastFallDamage).  Each landing's damage within 0.2 of the original's 0.2 x (sqrt(|GRAVITY| x h) - 45), the two
// paths within 0.2 of each other, POOL04 a death on both.  -OLOldFallDamage: the old numbers printed (the speed's height).
// Lines "OLSHOT falldamage drop ...", "OLSHOT falldamage check PASS|FAIL: ..." and "OLSHOT falldamage RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsGeometry.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsFallDamageProbeLocal
{
	int32 Passes = 0, Fails = 0, Drop = 0, Path = 0;
	float Wait = 0.f, DropAt = 0.f;
	FVector Pool = FVector::ZeroVector;
	TArray<float> Heights;                       // cm
	TArray<float> Got[2];                        // the damage on each path

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT falldamage check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}
}

OL_SHOT_MATCH(OLFallDamageProbe) { return OLFlags::Param(TEXT("OLFallDamageProbe")); }

OL_SHOT_TEST(OLFallDamageProbe, 1660)
{
	using namespace OutlawsFallDamageProbeLocal;
	if (!L || ShotStep < 0) return;
	const bool bOld = OLFlags::Param(TEXT("OLOldFallDamage"));
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView && ShotTime > Wait;
	static const TCHAR* PathName[2] = { TEXT("desktop"), TEXT("headset") };
	auto Formula = [&](float Cm) { return FMath::Max(0.f, 0.2f * (FMath::Sqrt(FMath::Abs(L->GravityAt(Pool)) * Cm / float(FOutlawsGeometry::UnitsToCm)) - 45.f)); };
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 6.f || !bUp || !L->LevelName.Equals(TEXT("icecaves"), ESearchCase::IgnoreCase)) return;
		L->bInvincible = true;
		FVector Top;
		const bool bPool = L->DebugSectorSpot(TEXT("POOL04"), Pool), bTop = L->DebugSectorSpot(TEXT("WATERFALL03"), Top);
		Check(bPool && bTop, FString::Printf(TEXT("POOL04's floor at %s, WATERFALL03's at %s (%.1f m above)"), *Pool.ToString(), *Top.ToString(), (Top.Z - Pool.Z) / 100.0));
		if (!bPool || !bTop) { ShotStep = 9; return; }
		Heights = { 300.f, 500.f, 600.f, 1000.f, 1500.f, 2260.f, 3000.f, float(Top.Z - Pool.Z) };
		Got[0].Reset();
		Got[1].Reset();
		Drop = 0;
		Path = 0;
		ShotStep = 1;
		return;
	}
	case 1:
	{
		// from rest, the capsule's bottom the height over the floor
		if (!bUp) return;
		BaseGravityScale = Path == 0 ? DesktopGravityScale : 1.5f;
		const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		TeleportTo(Pool + FVector(0.0, 0.0, Heights[Drop] + Half + 1.0), GetActorRotation(), false, true);
		GetCharacterMovement()->Velocity = FVector::ZeroVector;
		GetCharacterMovement()->SetMovementMode(MOVE_Falling);
		LastFallDropCm = -1.f;
		DropAt = ShotTime;
		ShotStep = 2;
		return;
	}
	case 2:
	{
		if (LastFallDropCm < 0.f && ShotTime - DropAt < 20.f) return;
		const float H = Heights[Drop], Want = Formula(H);
		Got[Path].Add(LastFallDropCm < 0.f ? -1.f : LastFallDamage);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT falldamage drop %s %.1f m: measured %.1f m, damage %.2f (the original's %.2f)%s"), PathName[Path], H / 100.f, LastFallDropCm / 100.f,
			LastFallDamage, Want, bOld ? TEXT(" -OLOldFallDamage") : TEXT(""));
		if (!bOld)
		{
			Check(LastFallDropCm >= 0.f && FMath::Abs(LastFallDamage - Want) <= 0.2f, FString::Printf(TEXT("%s %.1f m: damage %.2f within 0.2 of %.2f (landed %d)"), PathName[Path], H / 100.f,
				LastFallDamage, Want, LastFallDropCm >= 0.f ? 1 : 0));
		}
		if (++Drop >= Heights.Num())
		{
			Drop = 0;
			if (++Path >= 2) { ShotStep = 3; return; }
		}
		Wait = ShotTime + 0.5f;
		ShotStep = 1;
		return;
	}
	case 3:
	{
		BaseGravityScale = bVR ? 1.5f : DesktopGravityScale;
		const int32 Last = Heights.Num() - 1;
		float Worst = 0.f;
		for (int32 i = 0; i < Heights.Num(); ++i) Worst = FMath::Max(Worst, FMath::Abs(Got[0][i] - Got[1][i]));
		if (bOld)
		{
			Check(Got[0][Last] >= 0.f && Got[1][Last] >= 0.f, FString::Printf(TEXT("-OLOldFallDamage: the old numbers (POOL04 desktop %.2f, headset %.2f; the paths %.2f apart at most)"), Got[0][Last], Got[1][Last], Worst));
		}
		else
		{
			Check(Worst <= 0.2f, FString::Printf(TEXT("the desktop and the headset agree within 0.2 on every drop (%.2f at most)"), Worst));
			Check(Got[0][Last] >= 10.f && Got[1][Last] >= 10.f, FString::Printf(TEXT("WATERFALL03 into POOL04 (%.1f m) kills on both (health 10): desktop %.2f, headset %.2f"), Heights[Last] / 100.f, Got[0][Last], Got[1][Last]));
		}
		ShotStep = 9;
		return;
	}
	case 9:
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT falldamage RESULT %s (%d passed, %d failed)%s"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails, bOld ? TEXT(" -OLOldFallDamage") : TEXT(""));
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
