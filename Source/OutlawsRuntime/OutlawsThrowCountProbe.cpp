// BUG-LASTTHROWABLE's probe (user 2 Nov: the 6-stick kit threw 8): -OLShots -OLNoFrontEnd -OLLevel=townex -OLThrowCountProbe
// [-OLOldThrowCount], the headset's rules (the throwables' update and restock run only in VR).  For dynamite (weapon 6,
// idynam) and then knives (7, iknife): (1) a count of 2, one in each hand: the right thrown, the count 1, the right hand still
// empty 1.5 s on (the one left is the left hand's); the left thrown, the count 0, both empty; (2) a kit of 6, one in each
// hand: thrown as each hand has one, exactly 6 throws before 4 s pass with none, the count 0.  Each throw an unlit toss whose
// flight is taken off at once (the flight is DynThrow's and the arc test's; here only the count; and none lands to be picked
// up again).  -OLOldThrowCount: (2) throws 8, as before.  Lines "OLSHOT throwcount check PASS|FAIL: ..." and
// "OLSHOT throwcount RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsThrowCountProbeLocal
{
	int32 Stage = 0, Passes = 0, Fails = 0, Throws = 0, Kind = 0;
	float StageT = 0.f, SinceThrow = 0.f;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT throwcount check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}
}

OL_SHOT_MATCH(OLThrowCountProbe) { return OLFlags::Has(TEXT("OLThrowCountProbe")); }

OL_SHOT_TEST(OLThrowCountProbe, 1580)
{
	using namespace OutlawsThrowCountProbeLocal;
	if (!L || ShotTime < 0.f || Stage < 0) return;
	const bool bOld = OLFlags::Param(TEXT("OLOldThrowCount"));
	bVR = true;
	if (L->IsRebuildPending() || L->IsMoviePlaying() || bMenuOpen) return;
	L->StartSaveDelay = -1.f;           // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	StageT += Dt;
	const int32 Wi = Kind == 0 ? 6 : 7;
	const TCHAR* Item = Kind == 0 ? TEXT("idynam") : TEXT("iknife");
	const TCHAR* What = Kind == 0 ? TEXT("dynamite") : TEXT("knives");
	auto SetCount = [&](int32 N)
	{
		L->TakeCount(Item, L->GetCount(Item));
		if (N > 0) L->AddCount(Item, N);
	};
	auto Toss = [&](int32 H)
	{
		ThrowHeld(H, true, false);
		for (FThrown& T : Thrown) if (AActor* A = T.Actor.Get()) A->Destroy();
		Thrown.Reset();
		++Throws;
		SinceThrow = 0.f;
	};
	auto Line = [&]() { return FString::Printf(TEXT("count %d, in hand left %d right %d"), L->GetCount(Item), Mag[0], Mag[1]); };
	switch (Stage)
	{
	case 0:
		if (StageT < 3.f) return;
		L->GiveAllWeapons();
		bUnlimitedAmmo = false;
		SetCount(2);
		Equip(0, Wi);
		Equip(1, Wi);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT throwcount %s (1): 2 carried, drawn into both hands: %s"), What, *Line());
		Stage = 1;
		StageT = 0.f;
		return;
	case 1:
		if (StageT < 0.5f) return;
		if (bOld) { Stage = 4; StageT = 0.f; return; }
		Check(Mag[0] == 1 && Mag[1] == 1, FString::Printf(TEXT("%s (1): 2 carried, one in each hand: %s"), What, *Line()));
		Toss(1);
		Check(L->GetCount(Item) == 1, FString::Printf(TEXT("%s (1): the right thrown: %s"), What, *Line()));
		Stage = 2;
		StageT = 0.f;
		return;
	case 2:
		if (StageT < 1.5f) return;
		Check(Mag[1] == 0 && Mag[0] == 1, FString::Printf(TEXT("%s (1): 1.5 s on the right hand still empty, the left holds the last: %s"), What, *Line()));
		Toss(0);
		Stage = 3;
		StageT = 0.f;
		return;
	case 3:
		if (StageT < 1.5f) return;
		Check(L->GetCount(Item) == 0 && Mag[0] == 0 && Mag[1] == 0, FString::Printf(TEXT("%s (1): the left thrown, 1.5 s on: %s"), What, *Line()));
		Stage = 4;
		StageT = 0.f;
		return;
	case 4:
		SetCount(6);
		Equip(0, Wi);
		Equip(1, Wi);
		Throws = 0;
		SinceThrow = 0.f;
		Stage = 5;
		StageT = 0.f;
		return;
	case 5:
	{
		SinceThrow += Dt;
		if (StageT < 0.3f) return;
		for (int32 H = 0; H < 2; ++H)
		{
			if (Mag[H] > 0 && Equipped[H] == Wi && SinceThrow > 0.1f) { Toss(H); break; }
		}
		if (SinceThrow < 4.f && Throws < 20) return;
		const int32 Want = bOld ? 8 : 6;
		Check(Throws == Want && L->GetCount(Item) == 0, FString::Printf(TEXT("%s (2%s): a kit of 6 threw %d (want %d), %s"), What, bOld ? TEXT(", old rule") : TEXT(""), Throws, Want, *Line()));
		if (Kind == 0) { Kind = 1; Stage = 0; StageT = 3.f; return; }
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT throwcount RESULT %s (%d passed, %d failed, %s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			bOld ? TEXT("-OLOldThrowCount") : TEXT("new"));
		ShotTime = -1.f;
		Stage = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	}
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
