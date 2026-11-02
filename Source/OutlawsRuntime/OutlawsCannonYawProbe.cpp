// BUG-CANNONYAW (user 2 Nov, snapshot #35, civlwar2: the cannon 1F5B0 on the brick platform pointed its muzzle back at the
// player, south-south-west, where the original has it along its YAW 27.85): -OLShots -OLLevel=civlwar2 -OLNoFrontEnd
// -OLCannonYawProbe [-OLOldCannonYaw].  CANNON.obj is traced from atlas cell 9, the left profile (views 9-10), barrel along
// the model's X, the muzzle at its -X end.  (a) the muzzle's bearing (the model's yaw + 180) along the OB's YAW, within 10
// degrees; (b) every model the traced-view rule turned (only the cannon names an atlas_cell); (c) a shot from the
// snapshot's spot (cannonyaw_35[_old].png in Saved\OLShots).  Lines "OLSHOT cannonyaw check PASS|FAIL: ..." and
// "OLSHOT cannonyaw RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLCannonYawProbe) { return OLFlags::Param(TEXT("OLCannonYawProbe")); }

OL_SHOT_TEST(OLCannonYawProbe, 1480)
{
	static int32 Passes = 0, Fails = 0;
	static float Wait = 0.f;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cannonyaw check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	const bool bOld = OLFlags::Param(TEXT("OLOldCannonYaw"));
	// snapshot #35 (info.txt): the player's UE location and look
	const FVector At(-482.435, -9465.564, 89.150);
	const FRotator Look(8.65, 0.50, 0.0);
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 4.f || !bUp) return;
		L->bInvincible = true;
		Check(L->LevelName.Equals(TEXT("civlwar2"), ESearchCase::IgnoreCase), FString::Printf(TEXT("the level is civlwar2 (%s)"), *L->LevelName));
		int32 Cannons = 0;
		for (TActorIterator<AOutlawsSpriteActor> It(GetWorld()); It; ++It)
		{
			if (!It->ModelStem.Equals(TEXT("CANNON"), ESearchCase::IgnoreCase) || !It->Model) continue;
			++Cannons;
			const float Muzzle = FRotator::ClampAxis(It->ModelYaw + 180.f);
			const float Off = FMath::Abs(FMath::FindDeltaAngleDegrees(Muzzle, It->OutlawsYaw));
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cannonyaw CANNON at %s: YAW %.2f, model yaw %.2f, muzzle bearing %.2f (%.1f deg off the YAW)"),
				*It->GetActorLocation().ToCompactString(), It->OutlawsYaw, It->ModelYaw, Muzzle, Off);
			if (bOld) Check(Off > 170.f, FString::Printf(TEXT("old rule: the muzzle points back, %.2f (the snapshot's)"), Muzzle));
			else Check(Off <= 10.f, FString::Printf(TEXT("the muzzle points along the YAW %.2f: %.2f"), It->OutlawsYaw, Muzzle));
		}
		Check(Cannons == 1, FString::Printf(TEXT("one CANNON model in civlwar2 (%d)"), Cannons));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cannonyaw models turned by their traced view: %d: %s"), L->DebugTracedTurns.Num(), *FString::Join(L->DebugTracedTurns, TEXT("; ")));
		if (bOld) Check(L->DebugTracedTurns.Num() == 0, FString::Printf(TEXT("old rule: no model turned by its traced view (%d)"), L->DebugTracedTurns.Num()));
		else Check(L->DebugTracedTurns.Num() == 1 && L->DebugTracedTurns[0].StartsWith(TEXT("CANNON ")), FString::Printf(TEXT("only the cannon turned by its traced view (%d)"), L->DebugTracedTurns.Num()));
		TeleportTo(At, FRotator(0.0, Look.Yaw, 0.0), false, true);
		if (Controller) Controller->SetControlRotation(Look);
		Wait = ShotTime + 1.5f;
		ShotStep = 1;
		return;
	}
	case 1:
	{
		if (ShotTime < Wait) return;
		const FString Name = FString::Printf(TEXT("cannonyaw_35%s"), bOld ? TEXT("_old") : TEXT(""));
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cannonyaw shot %s.png"), *Name);
		Wait = ShotTime + 1.f;
		ShotStep = 2;
		return;
	}
	case 2:
		if (ShotTime < Wait) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT cannonyaw RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
