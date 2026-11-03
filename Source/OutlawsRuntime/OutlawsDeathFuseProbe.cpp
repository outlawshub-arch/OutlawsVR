// BUG-DEATHFUSE's probe (the user's play notes of 3 Nov: "TNT doesn't drop if I light one and I die before I throw it. It
// should fall with my body."):
//   -OLShots -OLNoFrontEnd -OLLevel=<level> -OLDeathFuseProbe [-OLDeathFuseDesktop] [-OLOldDeathFuse]
//   a session: -OLShots -OLDeathFuseProbe -OLDeathFuseCoop on the host (a match's, -OLMpHost=<map>) and its joiner
//   (-OLCoopAuto -OLCoopJoin=<host> -OLLobbyEnterAfter=4)
// Alone (the headset's rules, or the desktop's with -OLDeathFuseDesktop): a stick lit in the right hand with 2 s of fuse, the
// nearest bad guy stood 2.5 m before the player, then a lethal hit before the throw.  At once: the hands hold nothing lit (no
// flame shown, no fuse heard: BUG-FUSEAFTERDEATH's part kept), one lit stick in flight with the fuse it had, let go at the
// hand's own speed (no throw's boost), listed for the bad guys to flee (LitDynamite).  Then it lands near the body and goes
// off at its fuse's end: the time, the place and the bad guy's health logged.  After the respawn no flame on either hand.
// -OLOldDeathFuse: no stick, no blast, as before.  Co-op: the client does the same; the host sees the client's stick drawn
// falling (its own look) and its blast at the client's body.  Lines "OLSHOT deathfuse check PASS|FAIL: ..." and
// "OLSHOT deathfuse RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsEnemy.h"
#include "OutlawsCoop.h"
#include "OutlawsRemotePlayer.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Components/AudioComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "ProceduralMeshComponent.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsDeathFuseProbeLocal
{
	int32 Stage = 0, Passes = 0, Fails = 0;
	float StageT = 0.f;
	double DeathAt = 0.0, ReadyAt = 0.0;
	float FuseAtDeath = 0.f;
	FVector Feet = FVector::ZeroVector;
	int32 BlastsBefore = 0, LooksBefore = 0;
	TWeakObjectPtr<AOutlawsEnemy> Victim;
	float VictimBefore = 0.f;
	bool bAskedRespawn = false, bRestSeen = false;
	constexpr float FuseSet = 2.f;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT deathfuse check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}
}

OL_SHOT_MATCH(OLDeathFuseProbe) { return OLFlags::Param(TEXT("OLDeathFuseProbe")); }

OL_SHOT_TEST(OLDeathFuseProbe, 2140)
{
	using namespace OutlawsDeathFuseProbeLocal;
	if (!L || ShotTime < 0.f || Stage < 0) return;
	const bool bOld = OLFlags::Param(TEXT("OLOldDeathFuse"));
	const bool bDesk = OLFlags::Param(TEXT("OLDeathFuseDesktop"));
	const bool bCoop = OLFlags::Param(TEXT("OLDeathFuseCoop"));
	constexpr int32 Dynamite = 6, Revolver = 0;
	UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	UOutlawsCoop* C = bCoop ? UOutlawsCoop::Get(this) : nullptr;
	const FString Mode = FString::Printf(TEXT("%s%s%s"), bCoop ? (C && C->IsHost() ? TEXT("co-op host") : TEXT("co-op client")) : bDesk ? TEXT("desktop") : TEXT("vr"),
		bOld ? TEXT(", -OLOldDeathFuse") : TEXT(""), *FString::Printf(TEXT(", %s"), *L->LevelName));
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT deathfuse RESULT %s (%d passed, %d failed; %s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails, *Mode);
		ShotTime = -1.f;
		Stage = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	if (L->IsRebuildPending() || L->IsMoviePlaying() || bMenuOpen) { StageT = 0.f; return; }
	L->StartSaveDelay = -1.f;           // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	// (the headset's rules: the throwables' update runs only in VR; co-op keeps the instance's own, its lobby menu a desktop's)
	if (!bDesk && !bCoop) bVR = true;
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
	auto LitInHand = [&]() { return (bFuseLit[0] ? 1 : 0) + (bFuseLit[1] ? 1 : 0); };
	auto LitCount = [&]() { TArray<FVector> Lit; LitDynamite(Lit); return Lit.Num(); };
	auto LitThrown = [&]() -> const FThrown* { for (const FThrown& T : Thrown) if (T.Type == 1 && T.Fuse >= 0.f && !T.bDud) return &T; return nullptr; };
	// ---- the co-op host: watches for the client's stick and its blast ----
	if (bCoop && C && C->IsHost())
	{
		AOutlawsRemotePlayer* R = C->PuppetOf(1);
		if (Stage == 0)
		{
			if (!R || !C->bLevelBuilt) { if (StageT > 150.f) { Check(false, TEXT("no client came")); Finish(); } return; }
			LooksBefore = R->LooksShown + R->StandInsShown;
			BlastsBefore = L->BlastsSeen;
			Stage = 1;
			StageT = 0.f;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT deathfuse host: the client is here, watching for its stick (%d sticks and knives drawn so far, %d blasts)"), LooksBefore, BlastsBefore);
			return;
		}
		if (Stage == 1)
		{
			if (R && R->LooksShown + R->StandInsShown > LooksBefore)
			{
				Check(R->LooksShown > 0, FString::Printf(TEXT("the client's dropped stick drawn here, falling (%d with the thrower's own look, %d stand-ins)"), R->LooksShown, R->StandInsShown));
				ReadyAt = Now;
				Stage = 2;
				StageT = 0.f;
				return;
			}
			if (StageT > 90.f) { Check(bOld, FString::Printf(TEXT("no stick of the client's drawn here in 90 s%s"), bOld ? TEXT(" (the old rule: none, as wanted)") : TEXT(""))); Finish(); }
			return;
		}
		if (Stage == 2)
		{
			if (L->BlastsSeen > BlastsBefore && L->RecentBlasts.Num() > 0)
			{
				const FVector At = L->RecentBlasts.Last().At;
				const FVector Body = R ? R->GetActorLocation() : FVector::ZeroVector;
				const double Off = FVector::Dist2D(At, Body);
				Check(R && Off < 200.0, FString::Printf(TEXT("its blast here %.2f s after the stick showed, at %s, %.0f cm from the client's body (%s)"), Now - ReadyAt, *At.ToCompactString(), Off, *Body.ToCompactString()));
				Stage = 3;
				StageT = 0.f;
				return;
			}
			if (StageT > 10.f) { Check(false, TEXT("the client's stick showed but no blast came here in 10 s")); Finish(); }
			return;
		}
		if (Stage == 3 && StageT < 15.f) return;     // (the client's respawn and its checks while the host is still here)
		Finish();
		return;
	}
	switch (Stage)
	{
	case 0:
	{
		if (bCoop)
		{
			// (the client: its level built and the host there, then a few seconds for the session to settle)
			if (!C || !C->IsClient() || !C->bLevelBuilt || !C->PuppetOf(0)) { ReadyAt = 0.0; if (StageT > 150.f) { Check(false, TEXT("no co-op session came up")); Finish(); } return; }
			if (ReadyAt == 0.0) ReadyAt = Now;
			if (Now - ReadyAt < 8.0) return;
		}
		else if (StageT < 3.f) return;
		L->bInvincible = true;
		L->GiveAllWeapons();
		L->TakeCount(TEXT("idynam"), L->GetCount(TEXT("idynam")));
		L->AddCount(TEXT("idynam"), 6);
		Equip(1, Dynamite);
		// the nearest bad guy (anywhere) stood 2.5 m before the player, for the blast to meet (stilled, as the shot tests have them)
		const FVector Ahead = FVector(GetActorForwardVector().X, GetActorForwardVector().Y, 0.0).GetSafeNormal();
		double Best = 1e12;
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
		{
			AOutlawsEnemy* E = Wk.Get();
			if (!E || E->IsDead() || E->bVoiceOnly || E->bNotBadGuy) continue;
			const double D = FVector::Dist(E->GetActorLocation(), GetActorLocation());
			if (D < Best) { Best = D; Victim = E; }
		}
		if (AOutlawsEnemy* E = Victim.Get())
		{
			const double Up = E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() - GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			E->SetActorLocation(GetActorLocation() + Ahead * 250.0 + FVector(0.0, 0.0, Up + 2.0), false, nullptr, ETeleportType::TeleportPhysics);
			VictimBefore = E->GetHealth();
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT deathfuse start (%s): dynamite in the right hand (%d), %s"), *Mode, Equipped[1],
			Victim.IsValid() ? *FString::Printf(TEXT("%s stood 2.5 m ahead, health %.1f"), *Victim->GetName(), VictimBefore) : TEXT("no bad guy near"));
		Stage = 1;
		StageT = 0.f;
		return;
	}
	case 1:
	{
		if (StageT < 0.5f) return;
		Mag[1] = 1;
		bFuseLit[1] = true;
		Fuse[1] = FuseSet;
		if (WeaponMesh[1]) FuseSound[1] = L->PlayLoopAt(TEXT("fuse"), WeaponMesh[1]->GetComponentLocation());
		UpdateFuseLook(1);
		const int32 FlBefore = Flames(), LitBefore = LitCount();
		const int32 ThrownBefore = Thrown.Num();
		Check(WeaponMesh[1] && LitBefore == 1, FString::Printf(TEXT("lit in the right hand: %d flame parts shown, %d lit listed, the stick's mesh %s"), FlBefore, LitBefore, WeaponMesh[1] ? TEXT("there") : TEXT("missing")));
		const FVector HandVel = HandWorldVel[1];
		const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		Feet = GetActorLocation() - FVector(0.0, 0.0, Half);
		BlastsBefore = L->BlastsSeen;
		L->bInvincible = false;
		L->DamagePlayer(1e6f, 1.f, true);
		DeathAt = Now;
		const FThrown* T = LitThrown();
		const bool bNew = Thrown.Num() > ThrownBefore;
		FuseAtDeath = T ? T->Fuse : 0.f;
		Check(bDeadView, FString::Printf(TEXT("the lethal hit before the throw: dead %d"), bDeadView ? 1 : 0));
		Check(Flames() == 0 && Hissing() == 0 && LitInHand() == 0, FString::Printf(TEXT("the hands hold nothing lit: %d flame parts shown, %d fuse sounds in hand, %d lit in hand"), Flames(), Hissing(), LitInHand()));
		if (bOld)
		{
			Check(!T && !bNew && LitCount() == 0, FString::Printf(TEXT("(old rule) put out: %d lit sticks in flight, %d listed"), T ? 1 : 0, LitCount()));
		}
		else
		{
			Check(T && FMath::Abs(T->Fuse - FuseSet) < 0.1f, FString::Printf(TEXT("a live stick left the hand: %s, its fuse %.2f s (it had %.2f)"), T ? TEXT("in flight") : TEXT("NONE"), FuseAtDeath, FuseSet));
			Check(T && FVector::Dist(T->Vel, HandVel) < 1.0, FString::Printf(TEXT("let go at the hand's own speed, no throw's boost: %.0f cm/s (the hand %.0f cm/s)"), T ? T->Vel.Size() : -1.0, HandVel.Size()));
			Check(LitCount() == 1, FString::Printf(TEXT("listed for the bad guys to flee (LitDynamite): %d"), LitCount()));
		}
		Stage = 2;
		StageT = 0.f;
		return;
	}
	case 2:
	{
		// in the death view: the stick falls and goes off at its fuse's end (the old rule: nothing goes off); once it has come
		// to rest (or 1.2 s on) it is still lit where it lies: its fuse burning down, its flame shown, its fuse heard
		if (!bOld && !bRestSeen)
		{
			const FThrown* T = LitThrown();
			if (T && (T->bResting || Now - DeathAt > 1.2))
			{
				bRestSeen = true;
				const bool bFlame = T->Flame.IsValid() && T->Flame->IsVisible();
				const bool bHiss = T->Sound.IsValid() && T->Sound->IsPlaying();
				const double Across = FVector::Dist2D(T->Pos, Feet);
				Check(T->Fuse > 0.f && T->Fuse < FuseAtDeath && bFlame, FString::Printf(TEXT("still lit after the drop (%s, %.2f s on): %.0f cm across from the feet, %.0f cm up, its fuse %.2f s left, flame shown %d, fuse heard %d"),
					T->bResting ? TEXT("at rest") : TEXT("not yet at rest"), Now - DeathAt, Across, T->Pos.Z - Feet.Z, T->Fuse, bFlame ? 1 : 0, bHiss ? 1 : 0));
			}
		}
		if (L->BlastsSeen > BlastsBefore && L->RecentBlasts.Num() > 0)
		{
			const FVector At = L->RecentBlasts.Last().At - FVector(0.0, 0.0, 20.0);
			const double After = Now - DeathAt;
			const double Off = FVector::Dist2D(At, Feet), Dz = At.Z - Feet.Z;
			const float VictimNow = Victim.IsValid() ? Victim->GetHealth() : 0.f;
			const bool bHurt = Victim.IsValid() && (VictimNow < VictimBefore || Victim->IsDead());
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT deathfuse blast: %.2f s after the death (fuse %.2f s left then), at %s, %.0f cm from the body's feet across, %.0f cm up; the bad guy's health %.1f -> %.1f%s"),
				After, FuseAtDeath, *At.ToCompactString(), Off, Dz, VictimBefore, VictimNow, Victim.IsValid() && Victim->IsDead() ? TEXT(" (killed)") : TEXT(""));
			if (bOld) Check(false, FString::Printf(TEXT("(old rule) a blast came %.2f s after the death"), After));
			else
			{
				Check(FMath::Abs(After - FuseAtDeath) < 0.2, FString::Printf(TEXT("it went off at its fuse's end: %.2f s after the death (fuse %.2f)"), After, FuseAtDeath));
				Check(Off < 150.0 && FMath::Abs(Dz) < 100.0, FString::Printf(TEXT("it landed by the body: %.0f cm across from the feet, %.0f cm up"), Off, Dz));
				Check(!Victim.IsValid() || bHurt, FString::Printf(TEXT("its blast hurt the bad guy 2.5 m off as a thrown stick's: %.1f -> %.1f"), VictimBefore, VictimNow));
			}
			Stage = 3;
			StageT = 0.f;
			return;
		}
		if (Now - DeathAt > FuseSet + 2.0)
		{
			Check(bOld, FString::Printf(TEXT("no blast in %.1f s after the death%s"), FuseSet + 2.0, bOld ? TEXT(" (old rule: put out, as wanted)") : TEXT("")));
			Stage = 3;
			StageT = 0.f;
		}
		return;
	}
	case 3:
	{
		if (bDeadView)
		{
			if (!bAskedRespawn && DeathTime >= 1.2f) { bAskedRespawn = true; RequestRespawn(false); }
			if (StageT > 30.f) { Check(false, TEXT("no respawn in 30 s")); Finish(); }
			return;
		}
		if (StageT < 3.f) return;
		const int32 Fl = Flames(), Lit = LitCount(), Hiss = Hissing();
		Check(Fl == 0 && Lit == 0 && Hiss == 0 && LitInHand() == 0, FString::Printf(TEXT("after the respawn (hands %d and %d): %d flame parts shown, %d lit listed, %d fuse sounds, %d lit in hand"),
			Equipped[0], Equipped[1], Fl, Lit, Hiss, LitInHand()));
		L->bInvincible = true;
		Equip(1, Revolver);
		Stage = 4;
		StageT = 0.f;
		return;
	}
	case 4:
	{
		if (StageT < 0.5f) return;
		Check(Flames() == 0 && Equipped[1] == Revolver, FString::Printf(TEXT("the revolver drawn after (%d): no flame on it (%d flame parts)"), Equipped[1], Flames()));
		Finish();
		return;
	}
	default:
		Finish();
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
