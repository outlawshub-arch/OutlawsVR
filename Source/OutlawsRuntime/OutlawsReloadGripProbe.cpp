// BUG-RELOADGRIP's probe (the user, 4 Nov: reload pressed on a two-handed gun, the hand on the front grip must stay gripped, however far
// it moves while the reload plays; it was lost and had to grip again):
//   -OLShots -OLLevel=hideout -OLNoFrontEnd -OLReloadGripProbe [-OLOldReloadGrip]
// The headset's own code runs, at 90 Hz with bVR on for the probe's frames (Climbing: the grips; UpdateGuns: the reload), on the Rifle (it
// loads in the hand, no break, the hand stays on it), the Shotgun, the Double Barrel and the Sawed-Off. Each case squeezes the forend with
// the left hand, presses reload, then:
//   held:   the hand goes 70 cm from the forend until the reload ends: gripped at the end, the fist drawn on the forend, still gripped
//           with the hand back at it
//   eased:  the grip eases to 0.5 (a held grip's slack, a let-go being under 0.4): gripped at the end
//   letgo:  the hand goes 70 cm away and the grip is let go: free that frame, free at the end
//   again:  let go as letgo, then squeezed again out there: free at the end (a real let-go is not undone)
// -OLOldReloadGrip: held and eased end free on the break guns, as before. Lines "OLSHOT reloadgrip check PASS|FAIL: ..." and
// "OLSHOT reloadgrip RESULT PASS|FAIL (<n> passed, <n> failed)".

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsBoxHand.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "MotionControllerComponent.h"
#include "ProceduralMeshComponent.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsReloadGripProbeLocal
{
	int32 Passes = 0, Fails = 0;
	constexpr float FrameDt = 1.f / 90.f;
	constexpr float Far = 70.f;         // (cm: 60+ away)
	constexpr double OutOfReach = 25.0; // (cm: past NearForegrip's 16; a rifle held all through swings toward the hand, so not the full 70)

	enum class ECase : uint8 { Held, Eased, LetGo, Again };
	const TCHAR* CaseName(ECase C)
	{
		return C == ECase::Held ? TEXT("held") : C == ECase::Eased ? TEXT("eased") : C == ECase::LetGo ? TEXT("letgo") : TEXT("again");
	}
	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT reloadgrip check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}
}

OL_SHOT_MATCH(OLReloadGripProbe) { return OLFlags::Param(TEXT("OLReloadGripProbe")); }

OL_SHOT_TEST(OLReloadGripProbe, 2210)
{
	using namespace OutlawsReloadGripProbeLocal;
	if (!L || ShotTime < 1.5f) return;
	L->StartSaveDelay = -1.f;           // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	L->bInvincible = true;
	const bool bOldGrip = OLFlags::Param(TEXT("OLOldReloadGrip"));
	const bool bWasVR = bVR;
	bVR = true;
	auto Frames = [this, L](float Seconds)
	{
		for (int32 i = 0, N = FMath::Max(1, FMath::RoundToInt(Seconds / FrameDt)); i < N; ++i)
		{
			Climbing(FrameDt);
			UpdateGuns(FrameDt);
			L->TickSoundRuns(FrameDt);
		}
	};
	auto Grip = [this](float G) { TestAxes.Add(TEXT("OLVR_LeftGrip"), G); };
	auto ToForend = [this](const FVector& At, int32 Wi) { return FVector::Dist(At, WeaponMesh[1]->GetComponentTransform().TransformPosition(ForendSpot(Wi))); };
	TestAxes.Add(TEXT("OLVR_RightGrip"), 1.f);
	bGripArmed[1] = false;
	for (const int32 Wi : { 1, 2, 3, 4 })
	{
		const FString Gun = WeaponLabel(Wi);
		const bool bBreak = IsBreakGun(Wi);
		for (const ECase C : { ECase::Held, ECase::Eased, ECase::LetGo, ECase::Again })
		{
			const FString Tag = FString::Printf(TEXT("%s %s"), *Gun, CaseName(C));
			Grip(0.f);
			Frames(0.05f);              // (the last case's hand let go, armed for a new squeeze)
			TestBreakArm(1, Wi, 99, 10);
			const int32 Cap = Mag[1];
			TestBreakArm(1, Wi, FMath::Max(0, Cap - 2), 10);
			Frames(0.05f);              // (the gun in the hand's pose)
			TestFrontHandAt(1, 0.f);
			Frames(FrameDt);
			Grip(1.f);
			Frames(0.05f);              // (the real squeeze takes the forend)
			const bool bTook = bSupport[0];
			bButtonReload[1] = true;
			Frames(0.1f);
			const bool bAfterPress = bBreak ? bBreakOpen[1] && !bSupport[0] && bRegrip[0] && bFrontHidden[0] : bSupport[0];
			Check(bTook && bAfterPress, FString::Printf(TEXT("%s: squeezed on the forend (%d), reload pressed: open %d, on the gun %d, waiting %d, hidden %d"), *Tag, bTook ? 1 : 0,
				bBreakOpen[1] ? 1 : 0, bSupport[0] ? 1 : 0, bRegrip[0] ? 1 : 0, bFrontHidden[0] ? 1 : 0));
			if (C == ECase::Eased) Grip(0.5f);
			else TestFrontHandAt(1, Far);
			if (C == ECase::LetGo || C == ECase::Again)
			{
				Grip(0.f);
				Frames(FrameDt);
				Check(!bSupport[0] && !bRegrip[0] && !bFrontHidden[0], FString::Printf(TEXT("%s: free the frame the grip let go (on the gun %d, waiting %d, hidden %d)"), *Tag,
					bSupport[0] ? 1 : 0, bRegrip[0] ? 1 : 0, bFrontHidden[0] ? 1 : 0));
				Frames(0.05f);
				if (C == ECase::Again) Grip(1.f);
			}
			int32 Waited = 0;
			for (; Waited < 720 && (bBreak ? bBreakOpen[1] : bButtonReload[1]); ++Waited) Frames(FrameDt);
			const double Away = ToForend(GripOf(0)->GetComponentLocation(), Wi);
			const bool bWant = (C == ECase::Held || C == ECase::Eased) && !(bOldGrip && bBreak);
			const bool bEnded = (bBreak ? !bBreakOpen[1] : !bButtonReload[1]) && Mag[1] == Cap;
			const bool bFar = C == ECase::Eased || Away > OutOfReach;
			Check(bEnded && bFar && bSupport[0] == bWant && !bRegrip[0] && !bFrontHidden[0],
				FString::Printf(TEXT("%s: reload over in %.2f s, %d of %d loaded, the hand %.0f cm from the forend: %s (want %s), hidden %d"), *Tag, Waited * FrameDt, Mag[1], Cap, Away,
					bSupport[0] ? TEXT("gripped") : TEXT("free"), bWant ? TEXT("gripped") : TEXT("free"), bFrontHidden[0] ? 1 : 0));
			if (bWant)
			{
				Frames(0.05f);          // (the fist snaps to the forend, wherever the tracked hand still is)
				const double Fist = ToForend(LeftHand->GetComponentLocation(), Wi);
				TestFrontHandAt(1, 0.f);
				Frames(0.1f);
				Check(bSupport[0] && Fist < 5.0, FString::Printf(TEXT("%s: the fist drawn %.1f cm from the forend, the hand back on it: %s"), *Tag, Fist, bSupport[0] ? TEXT("gripped") : TEXT("free")));
			}
		}
	}
	Grip(0.f);
	Frames(0.05f);
	TestAxes.Remove(TEXT("OLVR_LeftGrip"));
	TestAxes.Remove(TEXT("OLVR_RightGrip"));
	bVR = bWasVR;
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT reloadgrip RESULT %s (%d passed, %d failed)%s"), Fails == 0 ? TEXT("PASS") : TEXT("FAIL"), Passes, Fails, bOldGrip ? TEXT(" -OLOldReloadGrip") : TEXT(""));
	ShotTime = -1.f;
	FGenericPlatformMisc::RequestExit(false);
}

#endif // WITH_OUTLAWS_TESTS
