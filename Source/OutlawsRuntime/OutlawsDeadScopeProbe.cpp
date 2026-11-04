// BUG-DEADSCOPE's probe (the user, 4 Nov: dying while looking down a scope left the scope stuck on the eye): -OLShots
// -OLNoFrontEnd -OLLevel=hideout -OLDeadScopeProbe [-OLOldDeadScope], the headset's rules (the scope's picture is the
// headset's).  A level that saves: thudtowr, a match map with no .inf, has no INF runtime, and SaveGame's first guard refuses
// it without a line.  -OLDeadScopeProbe=stuck: after a load that fails, wait for the feet anyway, as the build that hung to its
// 420 s on 5 Nov did, until the watch below ends it.
// The rifle raised by the scope tests' own raise (bScopeTestForce), the player killed, three times, one for each
// way back: the respawn, the revive (ResetForLevel, as UOutlawsCoop::OnRevive) and a load.  Each round: at the death neither
// hand scoped, the picture hidden, the capture stopped, the held gear back in the ink; so still through the death view with
// its fade come up in full; back on the feet the same with no fade left; and the next raise brings the picture up.
// -OLOldDeadScope: the picture stays on the eye through the death view, as before (one round).  Lines
// "OLSHOT deadscope check PASS|FAIL: ..." and "OLSHOT deadscope RESULT"; a shot of the death view, deadscope_dead.png.

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "HAL/PlatformMisc.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsDeadScopeProbeLocal
{
	constexpr float WatchSeconds = 90.f;       // (a whole run is about 30 s of this clock)
	int32 Stage = 0, Round = 0, Passes = 0, Fails = 0, Frames = 0;
	float StageT = 0.f, WatchT = 0.f;
	bool bAsked = false, bShot = false;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT deadscope check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}
}

OL_SHOT_MATCH(OLDeadScopeProbe) { return OLFlags::Has(TEXT("OLDeadScopeProbe")); }

OL_SHOT_TEST(OLDeadScopeProbe, 2476)
{
	using namespace OutlawsDeadScopeProbeLocal;
	if (!L || ShotTime < 0.f || Stage < 0) return;
	const bool bOld = OLFlags::Param(TEXT("OLOldDeadScope")), bStuck = OLFlags::ValueIs(TEXT("OLDeadScopeProbe"), TEXT("stuck"));
	static const TCHAR* const Way[3] = { TEXT("the respawn"), TEXT("the revive"), TEXT("the load") };
	constexpr int32 Rifle = 1, SaveSlot = 7, NoSlot = 99;
	bVR = true;
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT deadscope RESULT %s (%d passed, %d failed, %s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			bOld ? TEXT("-OLOldDeadScope") : TEXT("new"));
		ShotTime = -1.f;
		Stage = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	// (a run with no end holds the UE lock past the nightly, as this one did on 5 Nov: past the watch it ends, a FAIL by name)
	WatchT += Dt;
	++Frames;
	if (WatchT > WatchSeconds)
	{
		Check(false, FString::Printf(TEXT("done in %.0f s: stage %d, round %d, dead view %d, %d frames (%.0f a second)"), WatchT, Stage, Round + 1, bDeadView ? 1 : 0, Frames, Frames / WatchT));
		Finish();
		return;
	}
	if (L->IsRebuildPending() || L->IsMoviePlaying() || bMenuOpen) { StageT = 0.f; return; }
	L->StartSaveDelay = -1.f;           // (no autosaves: the one save is the load round's)
	L->AutosaveWait = 0.f;
	StageT += Dt;
	const bool bPicture = ScopeView && ScopeView->IsVisible(), bCapture = ScopeCapture && ScopeCapture->bCaptureEveryFrame;
	auto Down = [&]() { return !bScoped[0] && !bScoped[1] && !bPicture && !bCapture && ScopeUninked.Num() == 0; };
	auto State = [&]()
	{
		return FString::Printf(TEXT("scoped %d %d, picture %d, capture %d, %d parts out of the ink"), bScoped[0] ? 1 : 0, bScoped[1] ? 1 : 0, bPicture ? 1 : 0, bCapture ? 1 : 0, ScopeUninked.Num());
	};
	switch (Stage)
	{
	case 0:
		if (StageT < 3.f) return;
		L->GiveAllWeapons();
		if (L->GetCount(TEXT("iscope")) == 0) L->AddCount(TEXT("iscope"), 1);
		Equip(1, Rifle);
		bScopeTestNeedsItem = false;
		bScopeTestForce = true;
		Stage = 1;
		StageT = 0.f;
		return;
	case 1:
	{
		if (StageT < 0.5f) return;
		Check(bScoped[1] && bPicture && bCapture, FString::Printf(TEXT("(%d) raised: %s"), Round + 1, *State()));
		const int32 Uninked = ScopeUninked.Num();
		if (Round == 0)
		{
			// (SaveGame's first guard says nothing, so what it reads goes on this line)
			const bool bSaved = L->SaveGame(SaveSlot);
			Check(bSaved, FString::Printf(TEXT("(1) saved to slot %d for the load in (3): level %s, INF runtime %d, dead %d, dead view %d, front end %d, won %d"), SaveSlot, *L->LevelName,
				L->DebugInf() ? 1 : 0, L->IsPlayerDead() ? 1 : 0, bDeadView ? 1 : 0, L->IsFrontEnd() ? 1 : 0, L->DebugLevelWon() ? 1 : 0));
		}
		L->bInvincible = false;
		L->DamagePlayer(1e6f, 1.f, true);
		bScopeTestForce = false;        // (the forced raise would hold a hand up past any death)
		if (bOld) Check(bDeadView && bPicture && bScoped[1], FString::Printf(TEXT("(1, old rule) dead (%d): the picture stays on the eye: %s"), bDeadView ? 1 : 0, *State()));
		else
		{
			// (bPicture was read before the blow: read again)
			const bool bP = ScopeView && ScopeView->IsVisible(), bC = ScopeCapture && ScopeCapture->bCaptureEveryFrame;
			Check(bDeadView && !bScoped[0] && !bScoped[1] && !bP && !bC && ScopeUninked.Num() == 0, FString::Printf(TEXT("(%d) dead (%d) at once: scoped %d %d, picture %d, capture %d, %d of %d parts still out of the ink"),
				Round + 1, bDeadView ? 1 : 0, bScoped[0] ? 1 : 0, bScoped[1] ? 1 : 0, bP ? 1 : 0, bC ? 1 : 0, ScopeUninked.Num(), Uninked));
		}
		bAsked = false;
		Stage = 2;
		StageT = 0.f;
		return;
	}
	case 2:
		if (!bDeadView) { Check(false, FString::Printf(TEXT("(%d) the death view went away by itself"), Round + 1)); Finish(); return; }
		if (StageT > 1.4f && !bShot)
		{
			bShot = true;
			FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("deadscope_dead.png"), false, false);
		}
		if (StageT < 2.2f) return;
		if (bOld) Check(bPicture, FString::Printf(TEXT("(1, old rule) 2 s into the death view: still on the eye: %s"), *State()));
		else Check(Down(), FString::Printf(TEXT("(%d) 2 s into the death view: %s"), Round + 1, *State()));
		Check(DeathBlend >= 0.99f, FString::Printf(TEXT("(%d) the death view's fade came up in full: %.2f"), Round + 1, DeathBlend));
		if (Round == 0) RequestRespawn(false);
		else if (Round == 1)
		{
			// (what UOutlawsCoop::OnRevive does, with no partner here to give the bag)
			L->TestRevive(5.f);
			ResetForLevel();
		}
		else if (!L->LoadGame(bStuck ? NoSlot : SaveSlot))      // (=stuck: a slot nobody has, whatever an earlier run left in 7)
		{
			Check(false, FString::Printf(TEXT("(3) loading slot %d"), bStuck ? NoSlot : SaveSlot));
			if (!bStuck) { Finish(); return; }
		}
		Stage = 3;
		StageT = 0.f;
		return;
	case 3:
		if (bDeadView) { StageT = 0.f; return; }
		if (StageT < 2.f) return;
		Check(Down() && DeathBlend == 0.f, FString::Printf(TEXT("(%d) back after %s: %s, fade %.2f"), Round + 1, Way[Round], *State(), DeathBlend));
		if (bOld) { Finish(); return; }
		if (L->GetCount(TEXT("iscope")) == 0) L->AddCount(TEXT("iscope"), 1);
		Equip(1, Rifle);
		bScopeTestForce = true;
		Stage = 4;
		StageT = 0.f;
		return;
	case 4:
		if (StageT < 0.5f) return;
		Check(bScoped[1] && bPicture && bCapture, FString::Printf(TEXT("(%d) the next raise after %s works: %s"), Round + 1, Way[Round], *State()));
		bScopeTestForce = false;
		Stage = 5;
		StageT = 0.f;
		return;
	case 5:
		if (StageT < 0.5f) return;
		Check(Down(), FString::Printf(TEXT("(%d) lowered again: %s"), Round + 1, *State()));
		if (++Round >= 3) { Finish(); return; }
		Stage = 0;
		StageT = 2.5f;
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
