// BUG-STATUEYAW (user 2026-10-28, snapshots 1-4: townex's statues in the booths along the dirt path faced right as seen
// from the path, "rotate them 90 degrees clockwise"): -OLShots -OLLevel=townex -OLNoFrontEnd -OLStatueYawProbe
// [-OLOldStatueYaw].  The 3D models in the booths (STATUE1-4, the ammo box GAMBOXS beside them, the cow skull further down)
// each face out of their booth onto the path (Unreal +Y, the booths open to the level's +x); with -OLOldStatueYaw the
// statues face along the path as before (+X) and the cow skull, already facing out, is the same either way.  Then shots
// from the path of STATUE2 and STATUE4, statueyaw_after_<stem>.png (-OLOldStatueYaw: statueyaw_before_<stem>.png) in
// Saved\OLShots.  Lines "OLSHOT statueyaw check PASS|FAIL: ..." and "OLSHOT statueyaw RESULT".

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

OL_SHOT_MATCH(OLStatueYawProbe) { return OLFlags::Param(TEXT("OLStatueYawProbe")); }

OL_SHOT_TEST(OLStatueYawProbe, 18)
{
	static int32 Passes = 0, Fails = 0, Shot = 0;
	static float Wait = 0.f;
	static TMap<FString, TWeakObjectPtr<AOutlawsSpriteActor>> Found;
	static const TCHAR* const ShotOf[] = { TEXT("STATUE2"), TEXT("STATUE4") };
	if (!L || ShotStep < 0) return;
	const bool bOld = OLFlags::Param(TEXT("OLOldStatueYaw"));
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT statueyaw check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	// the booths: the models in the row along townex's path (Outlaws x -23..-15, z 456..524)
	auto InBooths = [](const FVector& At) { return At.Y > -23.5 * 30.0 && At.Y < -14.5 * 30.0 && At.X > 455.0 * 30.0 && At.X < 525.0 * 30.0; };
	auto Look = [&](const TCHAR* Stem)
	{
		const TWeakObjectPtr<AOutlawsSpriteActor>* S = Found.Find(Stem);
		if (!S || !S->IsValid()) return false;
		const FVector At = (*S)->GetActorLocation();
		const FVector From(At.X, At.Y + 330.0, At.Z);      // on the path, 3.3 m out from the statue, looking into the booth
		TeleportTo(From, FRotator(0.0, -90.0, 0.0), false, true);
		if (Controller) Controller->SetControlRotation(FRotator(-8.0, -90.0, 0.0));
		return true;
	};
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 4.f || !bUp) return;
		L->bInvincible = true;
		int32 Models = 0;
		for (TActorIterator<AOutlawsSpriteActor> It(GetWorld()); It; ++It)
		{
			AOutlawsSpriteActor* S = *It;
			if (!S->Model || S->ModelStem.IsEmpty()) continue;
			++Models;
			if (InBooths(S->GetActorLocation())) Found.Add(S->ModelStem, S);
		}
		struct FWant { const TCHAR* Stem; bool bOldAlong; };
		static const FWant Wants[] = { { TEXT("STATUE1"), true }, { TEXT("STATUE2"), true }, { TEXT("STATUE3"), true }, { TEXT("STATUE4"), true },
			{ TEXT("GAMBOXS"), false }, { TEXT("COWSKULL"), false } };
		for (const FWant& W : Wants)
		{
			const TWeakObjectPtr<AOutlawsSpriteActor>* S = Found.Find(W.Stem);
			if (!S || !S->IsValid())
			{
				Check(false, FString::Printf(TEXT("%s: no 3D model in the booths (%d models in %s)"), W.Stem, Models, *L->LevelName));
				continue;
			}
			const FVector Front = FRotator(0.0, (*S)->ModelYaw, 0.0).RotateVector(FVector(0.0, 1.0, 0.0));
			const FVector At = (*S)->GetActorLocation();
			const TCHAR* Way = Front.Y > 0.99 ? TEXT("out to the path") : Front.X > 0.99 ? TEXT("along the path (+X)") : Front.X < -0.99 ? TEXT("along the path (-X)") : TEXT("aslant");
			bool bOk = Front.Y > 0.99;
			const TCHAR* Wanted = TEXT("out to the path");
			if (bOld && W.bOldAlong) { bOk = Front.X > 0.99; Wanted = TEXT("along the path (+X), as before"); }
			else if (bOld && FCString::Strcmp(W.Stem, TEXT("GAMBOXS")) == 0) { bOk = Front.X < -0.99; Wanted = TEXT("along the path (-X), as before"); }
			Check(bOk, FString::Printf(TEXT("%s at Outlaws x %.2f z %.2f: yaw %.1f, its front faces %s (wanted %s)"), W.Stem, At.Y / 30.0, At.X / 30.0, (*S)->ModelYaw, Way, Wanted));
		}
		Shot = 0;
		if (!Look(ShotOf[0])) Check(false, TEXT("no STATUE2 for the shot"));
		Wait = ShotTime + 2.f;
		ShotStep = 1;
		return;
	}
	case 1:
	{
		if (ShotTime < Wait) return;
		if (Controller) Controller->SetControlRotation(FRotator(-8.0, -90.0, 0.0));
		const FString Name = FString::Printf(TEXT("statueyaw_%s_%s"), bOld ? TEXT("before") : TEXT("after"), *FString(ShotOf[Shot]).ToLower());
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT statueyaw shot %s.png from %s"), *Name, *GetActorLocation().ToString());
		Wait = ShotTime + 1.5f;
		if (++Shot < UE_ARRAY_COUNT(ShotOf) && Look(ShotOf[Shot]))
		{
			Wait = ShotTime + 2.f;
			return;
		}
		ShotStep = 2;
		return;
	}
	case 2:
		if (ShotTime < Wait) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT statueyaw RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
