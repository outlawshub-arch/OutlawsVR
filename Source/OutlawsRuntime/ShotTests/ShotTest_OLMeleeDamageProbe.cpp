// -OLShots -OLMeleeDamageProbe (BUG-MELEEDAMAGE, 9 Nov; the user: "Use the base damage" for the hand's blows).  In a story level, one
// bad guy at a time healed to 100 and struck as each blow strikes (from 30 cm back along the swing, as the hand's code does) at the
// body's middle and at the chest: the knife's jab 4, a punch 0.8, the crowbar 1.2, the shovel 1.4, the lantern 0.6.  Lines
// "OLSHOT meleedamage <blow> <body|chest>: dealt <d> (<the bad guy's full health> full: <n> blows)", checks that each is its base
// (x1 body, x2 chest), then "OLSHOT meleedamage RESULT <n> PASS, <m> FAIL".  -OLOldMeleeDamage: the point-blank x4, must FAIL.

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLMeleeDamageProbe) { return OLFlags::Param(TEXT("OLMeleeDamageProbe")); }

OL_SHOT_TEST(OLMeleeDamageProbe, 2587)
{
	static bool bDone = false;
	if (bDone || !L || ShotTime < 6.f) return;
	bDone = true;
	AOutlawsEnemy* E = nullptr;
	for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies()) if (AOutlawsEnemy* F = Wk.Get()) if (!F->IsDead() && !F->bBoss) { E = F; break; }
	if (!E) { UE_LOG(LogOutlaws, Display, TEXT("OLSHOT meleedamage RESULT 0 PASS, 1 FAIL (no bad guy)")); FGenericPlatformMisc::RequestExit(false); return; }
	const float Full = E->GetMaxHealth();
	struct FBlow { const TCHAR* Name; float Base; bool bMelee; };
	const FBlow Blows[] = { { TEXT("jab"), 4.f, true }, { TEXT("punch"), 0.8f, true }, { TEXT("crowbar"), 1.2f, false }, { TEXT("shovel"), 1.4f, false }, { TEXT("lantern"), 0.6f, false } };
	int32 Pass = 0, Fail = 0;
	const FVector Way = (E->GetActorLocation() - GetActorLocation()).GetSafeNormal2D();
	for (const FBlow& B : Blows)
	{
		for (int32 Chest = 0; Chest < 2; ++Chest)
		{
			E->TestSetHealth(100.f);
			const FVector C = E->GetActorLocation();
			const FVector At = C + FVector(0.0, 0.0, Chest ? 25.0 : -10.0) - Way * E->GetCapsuleComponent()->GetScaledCapsuleRadius() * 0.5;
			AOutlawsEnemy::GNextHitMelee = B.bMelee;
			AOutlawsEnemy::GNextHitPushCap = MeleePushCap();
			AOutlawsEnemy::GNextHitHand = true;
			E->TakeHit(B.Base, At, Way, At - Way * 30.f);
			const float Dealt = 100.f - E->GetHealth();
			const float Want = B.Base * (Chest ? 2.f : 1.f);
			const bool bOk = FMath::IsNearlyEqual(Dealt, Want, 0.01f);
			(bOk ? Pass : Fail)++;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT meleedamage %s %s: dealt %.2f (%.1f full: %d blows) want %.2f %s"), B.Name, Chest ? TEXT("chest") : TEXT("body"), Dealt, Full,
				Dealt > 0.f ? FMath::CeilToInt(Full / Dealt) : 0, Want, bOk ? TEXT("PASS") : TEXT("FAIL"));
		}
	}
	E->TestSetHealth(Full);
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT meleedamage RESULT %d PASS, %d FAIL"), Pass, Fail);
	FGenericPlatformMisc::RequestExit(false);
}

#endif // WITH_OUTLAWS_TESTS
