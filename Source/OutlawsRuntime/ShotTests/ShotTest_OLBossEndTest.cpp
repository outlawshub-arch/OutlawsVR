// -OLShots -OLBossEndTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLBossEndTest)
{
	FString EndFuncs;
	return OLFlags::Value(TEXT("OLBossEndTest"), EndFuncs);
}

OL_SHOT_TEST(OLBossEndTest, 1210)
{
	FString EndFuncs;
	OLFlags::Value(TEXT("OLBossEndTest"), EndFuncs);
	// -OLBossEndTest=plain+ender (mine: mary_new+drdeath_new, ranch: buckshot_new+bob_new): the first down ends
	// nothing, the second ends the level (user 2026-09-29)
	TArray<FString> F;
	EndFuncs.ParseIntoArray(F, TEXT("+"));
	auto Kill = [](AOutlawsEnemy* B) { for (int32 i = 0; B && i < 60 && !B->IsDead(); ++i) B->TakeHit(50.f, B->GetActorLocation(), FVector(1.0, 0.0, 0.0), B->GetActorLocation() - FVector(100.0, 0.0, 0.0)); };
	if (ShotStep == 0 && ShotTime > 3.f && L && F.Num() == 2)
	{
		AOutlawsEnemy* A = L->DebugBossByFunc(F[0]);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT boss %s found %d"), *F[0], A ? 1 : 0);
		Kill(A);
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 5.f && L)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT after %s down: level ending %d (expect 0)"), *F[0], L->DebugLevelEnding() ? 1 : 0);
		AOutlawsEnemy* B = L->DebugBossByFunc(F[1]);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT boss %s found %d"), *F[1], B ? 1 : 0);
		Kill(B);
		++ShotStep;
	}
	else if (ShotStep == 2 && ShotTime > 7.f && L)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT after %s down: level ending %d (expect 1)"), *F[1], L->DebugLevelEnding() ? 1 : 0);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
