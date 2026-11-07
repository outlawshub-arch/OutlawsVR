// BUG-FUSEAFTERDEATH's probe (thudtowr, the user's snapshot #1: a fuse's flame burned on the revolver after a death): -OLShots
// -OLNoFrontEnd -OLLevel=thudtowr -OLFuseDeathProbe [-OLOldFuseReset], the headset's rules (the throwables' update runs only
// in VR).  (1) a stick of dynamite (weapon 6) lit in each hand, the flames shown, then the player killed: at once nothing lit
// in the hands (no flame, no fuse sound on them) and, since BUG-DEATHFUSE (the lit sticks fall with the body, live), the two
// dropped sticks in the bad guys' lit list (LitDynamite) and nothing else; after the normal respawn the same, on whatever is in
// hand, and the dropped sticks listed until they go off, then none.  (2) a lit stick in the right hand swapped for the
// revolver (weapon 0): no flame on the gun, then or half a second on.  -OLOldFuseReset: the flames burn on through the death,
// as before.  -OLOldDeathFuse (the sticks put out with the player, nothing dropped): (1) FAILs at the death, none listed.
// Lines "OLSHOT fusedeath check PASS|FAIL: ..." and "OLSHOT fusedeath RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Components/AudioComponent.h"
#include "Components/StaticMeshComponent.h"
#include "ProceduralMeshComponent.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsFuseDeathProbeLocal
{
	int32 Stage = 0, Passes = 0, Fails = 0, Mismatch = 0;
	float StageT = 0.f;
	bool bAskedRespawn = false;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT fusedeath check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}
}

OL_SHOT_MATCH(OLFuseDeathProbe) { return OLFlags::Has(TEXT("OLFuseDeathProbe")); }

OL_SHOT_TEST(OLFuseDeathProbe, 1570)
{
	using namespace OutlawsFuseDeathProbeLocal;
	if (!L || ShotTime < 0.f || Stage < 0) return;
	const bool bOld = OLFlags::Param(TEXT("OLOldFuseReset"));
	constexpr int32 Dynamite = 6, Revolver = 0;
	bVR = true;
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT fusedeath RESULT %s (%d passed, %d failed, %s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			bOld ? TEXT("-OLOldFuseReset") : TEXT("new"));
		ShotTime = -1.f;
		Stage = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	if (L->IsRebuildPending() || L->IsMoviePlaying() || bMenuOpen) { StageT = 0.f; return; }
	L->StartSaveDelay = -1.f;           // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	StageT += Dt;
	auto Flames = [&]()
	{
		int32 N = 0;
		for (int32 H = 0; H < 2; ++H)
		{
			N += FuseFlame[H] && FuseFlame[H]->IsVisible();
			N += FuseCore[H] && FuseCore[H]->IsVisible();
		}
		return N;
	};
	auto Hissing = [&]()
	{
		int32 N = 0;
		for (int32 H = 0; H < 2; ++H) N += FuseSound[H].IsValid() && FuseSound[H]->IsPlaying();
		return N;
	};
	auto LitCount = [&]() { TArray<FVector> Lit; LitDynamite(Lit); return Lit.Num(); };
	// (BUG-DEATHFUSE) the hands lit, and the dropped sticks still burning (the lit list's other part)
	auto HandsLit = [&]() { return (bFuseLit[0] ? 1 : 0) + (bFuseLit[1] ? 1 : 0); };
	auto Dropped = [&]()
	{
		int32 N = 0;
		for (const FThrown& T : Thrown) N += T.Type == 1 && T.Fuse >= 0.f && !T.bDud ? 1 : 0;
		return N;
	};
	auto Light = [&](int32 H)
	{
		Mag[H] = 1;
		bFuseLit[H] = true;
		Fuse[H] = 4.5f;
		if (WeaponMesh[H]) FuseSound[H] = L->PlayLoopAt(TEXT("fuse"), WeaponMesh[H]->GetComponentLocation());
		UpdateFuseLook(H);
	};
	auto Kit = [&]()
	{
		L->TakeCount(TEXT("idynam"), L->GetCount(TEXT("idynam")));
		L->AddCount(TEXT("idynam"), 6);
	};
	switch (Stage)
	{
	case 0:
		if (StageT < 3.f) return;
		L->GiveAllWeapons();
		Kit();
		Equip(0, Dynamite);
		Equip(1, Dynamite);
		Stage = 1;
		StageT = 0.f;
		return;
	case 1:
	{
		if (StageT < 0.5f) return;
		Light(0);
		Light(1);
		const int32 Before = Flames(), LitBefore = LitCount(), HissBefore = Hissing();
		Check(Before >= 2 && LitBefore == 2, FString::Printf(TEXT("(1) both hands lit: %d flame parts shown, %d lit sticks listed, %d fuse sounds"), Before, LitBefore, HissBefore));
		L->bInvincible = false;
		L->DamagePlayer(1e6f, 1.f, true);
		const int32 After = Flames(), LitAfter = LitCount(), HissAfter = Hissing(), HandsAfter = HandsLit(), DropAfter = Dropped();
		if (bOld) Check(bDeadView && (After > 0 || LitAfter > 0), FString::Printf(TEXT("(1, old rule) dead (%d): the fuses burn on: %d flame parts, %d listed, %d sounds"), bDeadView ? 1 : 0, After, LitAfter, HissAfter));
		else Check(bDeadView && After == 0 && HandsAfter == 0 && HissAfter == 0 && DropAfter == 2 && LitAfter == DropAfter,
			FString::Printf(TEXT("(1) dead (%d): nothing lit in the hands (%d lit, %d flame parts shown, %d fuse sounds on them), the dropped sticks listed (%d dropped, 2 wanted, %d listed)"),
			bDeadView ? 1 : 0, HandsAfter, After, HissAfter, DropAfter, LitAfter));
		bAskedRespawn = false;
		Stage = 2;
		StageT = 0.f;
		return;
	}
	case 2:
	{
		if (bDeadView)
		{
			if (!bAskedRespawn && DeathTime >= 1.2f) { bAskedRespawn = true; RequestRespawn(false); }
			return;
		}
		if (StageT < 3.f) return;
		const int32 Fl = Flames(), Lit = LitCount(), Hiss = Hissing(), Hands = HandsLit(), Drop = Dropped();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT fusedeath (1) after the respawn: hands hold %d and %d, gun meshes shown %d %d"), Equipped[0], Equipped[1],
			WeaponMesh[0] && WeaponMesh[0]->IsVisible() ? 1 : 0, WeaponMesh[1] && WeaponMesh[1]->IsVisible() ? 1 : 0);
		if (bOld) Check(Fl > 0, FString::Printf(TEXT("(1, old rule) after the respawn a flame still shown: %d flame parts, %d listed, %d sounds"), Fl, Lit, Hiss));
		else Check(Fl == 0 && Hands == 0 && Hiss == 0 && Lit == Drop, FString::Printf(TEXT("(1) after the respawn: nothing lit in the hands (%d lit, %d flame parts shown, %d fuse sounds on them), the dropped sticks still burning listed (%d burning, %d listed)"),
			Hands, Fl, Hiss, Drop, Lit));
		if (bOld) { Finish(); return; }
		Mismatch = 0;
		Stage = 5;
		StageT = 0.f;
		return;
	}
	case 5:
	{
		// (BUG-DEATHFUSE) the dropped sticks listed until they go off (their fuse's 4.5 s from the death), then none
		const int32 Lit = LitCount(), Drop = Dropped(), Hands = HandsLit();
		if (Lit != Drop || Hands != 0) ++Mismatch;
		if (Drop > 0 && StageT < 6.f) return;
		Check(Drop == 0 && Lit == 0 && Mismatch == 0, FString::Printf(TEXT("(1) once the dropped sticks went off (%.1f s on): %d still burning, %d listed, %d ticks the list and the dropped sticks differed"),
			StageT, Drop, Lit, Mismatch));
		Kit();
		Equip(1, Dynamite);
		Stage = 3;
		StageT = 0.f;
		return;
	}
	case 3:
	{
		if (StageT < 0.5f) return;
		Light(1);
		const bool bShown = FuseFlame[1] && FuseFlame[1]->IsVisible();
		Equip(1, Revolver);
		const bool bGone = !(FuseFlame[1] && FuseFlame[1]->IsVisible()) && !(FuseCore[1] && FuseCore[1]->IsVisible()) && Hissing() == 0;
		Check(bShown && bGone && Equipped[1] == Revolver, FString::Printf(TEXT("(2) a lit stick (flame shown %d) swapped for the revolver (%d): no flame, no sound (%d)"), bShown ? 1 : 0, Equipped[1], bGone ? 1 : 0));
		Stage = 4;
		StageT = 0.f;
		return;
	}
	case 4:
	{
		if (StageT < 0.5f) return;
		const bool bGone = !(FuseFlame[1] && FuseFlame[1]->IsVisible()) && !(FuseCore[1] && FuseCore[1]->IsVisible());
		Check(bGone && WeaponMesh[1] && WeaponMesh[1]->IsVisible(), FString::Printf(TEXT("(2) half a second on: the revolver shown (%d), no flame on it (%d), %d lit listed"),
			WeaponMesh[1] && WeaponMesh[1]->IsVisible() ? 1 : 0, bGone ? 1 : 0, LitCount()));
		Finish();
		return;
	}
	default:
		Finish();
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
