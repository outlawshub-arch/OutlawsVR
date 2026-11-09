// -OLShots -OLBossBlastProbe (BUG-BOSSBLAST, 9 Nov; the user: "match the original, flat 3 damage from blasts on bosses").  In a
// story level: one live bad guy made a boss and one left plain, each with a dynamite blast set off 1 m and then 4 m from him (in plain
// sight, the stick's reach 50 units), his health read before and after each.  The boss takes 3 both times; the plain one the
// falloff's share of 12 (more near, less far).  Lines "OLSHOT bossblast <who> at <cm>: health <a> -> <b> (<dealt>)", checks
// "OLSHOT bossblast check PASS|FAIL: ...", then "OLSHOT bossblast RESULT <n> PASS, <m> FAIL".

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLBossBlastProbe) { return OLFlags::Param(TEXT("OLBossBlastProbe")); }

OL_SHOT_TEST(OLBossBlastProbe, 2581)
{
	static int32 Step = 0, Pass = 0, Fail = 0;
	static TWeakObjectPtr<AOutlawsEnemy> Boss, Plain;
	static float Dealt[2][2] = { { 0.f, 0.f }, { 0.f, 0.f } };
	if (Step < 0 || !L || ShotTime < 6.f) return;
	auto Check = [&](bool bOk, const FString& What)
	{
		(bOk ? Pass : Fail)++;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT bossblast check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
	};
	if (Step == 0)
	{
		TArray<AOutlawsEnemy*> Live;
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies()) if (AOutlawsEnemy* E = Wk.Get()) if (!E->IsDead() && !E->bBoss) Live.Add(E);
		if (Live.Num() < 2)
		{
			Check(false, FString::Printf(TEXT("two live bad guys (%d)"), Live.Num()));
			Step = 9;
			return;
		}
		Boss = Live[0]; Plain = Live[1];
		Boss->bBoss = true;
		Step = 1;
		return;
	}
	if (Step >= 1 && Step <= 4)
	{
		// each round: the man a long way from the player, healed to 100, the stick 1 m or 4 m off him on open floor
		const int32 Who = (Step - 1) / 2, Far = (Step - 1) % 2;
		AOutlawsEnemy* E = (Who == 0 ? Boss : Plain).Get();
		if (!E || E->IsDead())
		{
			Check(false, TEXT("the bad guy still alive for his round"));
			++Step;
			return;
		}
		E->TestSetHealth(100.f);
		const FVector At = E->GetActorLocation();
		FVector Away = (GetActorLocation() - At).GetSafeNormal2D();
		if (Away.IsNearlyZero()) Away = FVector::ForwardVector;
		const double Cm = Far ? 400.0 : 100.0;
		// (the floor between them clear: the stick set on the side the trace finds open)
		FVector Where = At + Away * Cm;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsBossBlastProbe), true, this);
		Q.AddIgnoredActor(E);
		FHitResult Hit;
		for (int32 k = 0; k < 8 && GetWorld()->LineTraceSingleByChannel(Hit, At, At + Away * Cm, ECC_Visibility, Q); ++k)
		{
			Away = Away.RotateAngleAxis(45.0, FVector::UpVector);
			Where = At + Away * Cm;
		}
		const float H0 = E->GetHealth();
		L->Explode(Where);
		const float H1 = E->GetHealth();
		Dealt[Who][Far] = H0 - H1;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT bossblast %s at %.0f cm: health %.2f -> %.2f (%.2f)"), Who == 0 ? TEXT("boss") : TEXT("plain"), Cm, H0, H1, H0 - H1);
		++Step;
		if (Step == 5)
		{
			Check(FMath::IsNearlyEqual(Dealt[0][0], 3.f, 0.01f) && FMath::IsNearlyEqual(Dealt[0][1], 3.f, 0.01f),
				FString::Printf(TEXT("the boss takes a flat 3 near and far (%.2f, %.2f)"), Dealt[0][0], Dealt[0][1]));
			Check(Dealt[1][0] > Dealt[1][1] && Dealt[1][1] > 0.f,
				FString::Printf(TEXT("the plain bad guy takes the falloff's share, more near than far (%.2f, %.2f)"), Dealt[1][0], Dealt[1][1]));
			Step = 9;
		}
		return;
	}
	if (Step == 9)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT bossblast RESULT %d PASS, %d FAIL"), Pass, Fail);
		Step = -1;
		FGenericPlatformMisc::RequestExit(false);
	}
}

#endif // WITH_OUTLAWS_TESTS
