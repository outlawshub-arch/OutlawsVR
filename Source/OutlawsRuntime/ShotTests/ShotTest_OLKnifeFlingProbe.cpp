// -OLShots -OLKnifeFlingProbe (BUG-KNIFEFLING, 9 Nov; the user: "while stabbing people, their body can sometimes go flying off in
// the distance ... when stabbing people normally, their body should only get pushed back about as far as it does if I was to throw
// them").  Measure: a live bad guy on open floor (the first clear way, 25 m of it), each case started from the same spot, his
// travel over the floor (start to rest, 4 s) logged:
//   stab_alive, stab_kill: the knife's jab as the stab gives it (TakeHit, the knife's damage, from 30 cm back along the swing)
//   toss_<cm/s>:            let go of with a grip toss at a hand speed (TossVelocity's rule: x1.5, a man's x0.71)
//   aimed:                  the aimed throw's launch (1500 cm/s at 45 degrees, ManAimThrowScale's note)
//   stab_after_toss:        tossed at 300 cm/s and stabbed the same frame (the release case)
// Lines "OLSHOT knifefling <case>: travel <cm> cm, peak <cm> cm up, alive <0|1>", then "OLSHOT knifefling RESULT".

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLKnifeFlingProbe) { return OLFlags::Param(TEXT("OLKnifeFlingProbe")); }

OL_SHOT_TEST(OLKnifeFlingProbe, 2585)
{
	static int32 Case = 0, Phase = 0, Knife = -1;
	static float At = 0.f, PeakZ = 0.f;
	static FVector Start = FVector::ZeroVector, Way = FVector::ZeroVector;
	static TWeakObjectPtr<AOutlawsEnemy> Man;
	static TSet<const AOutlawsEnemy*> Used;
	static TArray<FString> Names;
	static TMap<FString, double> Travels;
	static int32 KfPass = 0, KfFail = 0;
	if (Case < 0 || !L || ShotTime < 6.f) return;
	if (Names.Num() == 0)
	{
		Names = { TEXT("stab_alive"), TEXT("stab_kill"), TEXT("punch_alive"), TEXT("punch_kill"), TEXT("punch_x3_kill"), TEXT("toss_200"), TEXT("toss_400"),
			TEXT("toss_600"), TEXT("aimed"), TEXT("stab_after_toss"), TEXT("knife_hand_throw") };
		for (int32 i = 0; i < 32; ++i) if (WeaponLabel(i).Contains(TEXT("knife"))) { Knife = i; break; }
		// the open way: from the player, the first of 16 with 25 m clear at waist height
		const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
		FCollisionQueryParams Q(TEXT("OLKnifeFling"), false, this);
		Way = FRotator(0.0, GetActorRotation().Yaw, 0.0).Vector();
		for (int32 d = 0; d < 16; ++d)
		{
			const FVector Try = FRotator(0.0, GetActorRotation().Yaw + 22.5 * d, 0.0).Vector();
			if (!GetWorld()->LineTraceTestByChannel(Feet + FVector(0.0, 0.0, 80.0), Feet + FVector(0.0, 0.0, 80.0) + Try * 2700.0, ECC_Visibility, Q)) { Way = Try; break; }
		}
		Start = Feet + Way * 200.0;
	}
	if (Case >= Names.Num())
	{
		// a hand's blow no farther than a firm toss (a 400 cm/s swing), a fifth over for the floor's give
		const double Toss = Travels.FindRef(TEXT("toss_400"));
		for (const TCHAR* K : { TEXT("stab_kill"), TEXT("punch_kill"), TEXT("punch_x3_kill") })
		{
			const bool bOk = Toss > 0.0 && Travels.FindRef(K) <= Toss * 1.2;
			(bOk ? KfPass : KfFail)++;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT knifefling check %s: %s travels %.0f cm, a firm toss %.0f (at most a fifth over)"), bOk ? TEXT("PASS") : TEXT("FAIL"), K, Travels.FindRef(K), Toss);
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT knifefling RESULT %d PASS, %d FAIL (knife damage %.1f)"), KfPass, KfFail, Knife >= 0 ? WeaponDamage(Knife) : -1.f);
		Case = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	}
	At += Dt;
	const FString C = Names[Case];
	if (Phase == 0)
	{
		// a fresh man each case (one not used yet, alive) where the cases start, whole, standing
		Man = nullptr;
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
			if (AOutlawsEnemy* F = Wk.Get()) if (!F->IsDead() && !F->bBoss && !Used.Contains(F)) { Man = F; Used.Add(F); break; }
		if (!Man.IsValid()) { UE_LOG(LogOutlaws, Display, TEXT("OLSHOT knifefling: no fresh bad guy for %s"), *C); Case = Names.Num(); return; }
		AOutlawsEnemy* E = Man.Get();
		E->TestSetHealth(C.EndsWith(TEXT("_kill")) ? (C == TEXT("punch_x3_kill") ? 7.f : 0.5f) : 100.f);
		E->SetActorLocation(Start + FVector(0.0, 0.0, E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0), false, nullptr, ETeleportType::TeleportPhysics);
		E->GetCharacterMovement()->StopMovementImmediately();
		Phase = 1; At = 0.f;
		return;
	}
	AOutlawsEnemy* E = Man.Get();
	if (!E) { ++Case; Phase = 0; return; }
	if (Phase == 1 && At > 0.5f)
	{
		const FVector Chest = E->GetActorLocation() + FVector(0.0, 0.0, 25.0);
		const FVector Tip = Chest - Way * 5.0;
		auto Stab = [&]()
		{
			// as the stab gives it: the knife's damage, the swing's way, from 30 cm back along it (OutlawsPlayerWeapons.cpp, the jab)
			AOutlawsEnemy::GNextHitMelee = true;
			AOutlawsEnemy::GNextHitPushCap = MeleePushCap();
			E->TakeHit(WeaponDamage(Knife), Tip, Way, Tip - Way * 30.f);
		};
		auto Punch = [&]()
		{
			// as the punch gives it (OutlawsPlayerWeapons.cpp: 0.8, from 30 cm back along the swing)
			AOutlawsEnemy::GNextHitMelee = true;
			AOutlawsEnemy::GNextHitPushCap = MeleePushCap();
			E->TakeHit(0.8f, Tip, Way, Tip - Way * 30.f);
		};
		auto Toss = [&](float HandCms) { E->BeginHeld(this); E->EndHeld((Way * HandCms) * TossGain * ManThrowScale); };
		Start = E->GetActorLocation();
		PeakZ = Start.Z;
		if (C.StartsWith(TEXT("stab_alive")) || C == TEXT("stab_kill")) Stab();
		else if (C == TEXT("punch_alive") || C == TEXT("punch_kill")) Punch();
		else if (C == TEXT("punch_x3_kill")) { Punch(); Punch(); Punch(); }
		else if (C == TEXT("toss_200")) Toss(200.f);
		else if (C == TEXT("toss_400")) Toss(400.f);
		else if (C == TEXT("toss_600")) Toss(600.f);
		else if (C == TEXT("aimed")) { E->BeginHeld(this); E->EndHeld((Way + FVector::UpVector).GetSafeNormal() * 1500.f); }
		else if (C == TEXT("stab_after_toss")) { Toss(300.f); Stab(); }
		else if (C == TEXT("knife_hand_throw"))
		{
			// the real let-go from the right hand with the knife in it: that knife's jab rests a second after
			Equip(1, Knife);
			KnifeCool[1] = 0.f;
			GrabbedEnemy[1] = E;
			E->BeginHeld(this);
			const FVector Vel = Way * 600.f;
			LetGoEnemy(1, true, &Vel);
			const bool bOk = FMath::IsNearlyEqual(KnifeCool[1], 0.5f, 0.01f);
			(bOk ? KfPass : KfFail)++;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT knifefling check %s: thrown from the knife hand, its jab rests %.2f s (half a second)"), bOk ? TEXT("PASS") : TEXT("FAIL"), KnifeCool[1]);
			Equip(1, -1);
		}
		Phase = 2; At = 0.f;
		return;
	}
	if (Phase == 2)
	{
		PeakZ = FMath::Max(PeakZ, float(E->GetActorLocation().Z));
		if (At < 4.f) return;
		const double Travel = FVector::Dist2D(Start, E->GetActorLocation());
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT knifefling %s: travel %.0f cm, peak %.0f cm up, alive %d"), *C, Travel, PeakZ - Start.Z, E->IsDead() ? 0 : 1);
		Travels.Add(C, Travel);
		++Case; Phase = 0; At = 0.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
