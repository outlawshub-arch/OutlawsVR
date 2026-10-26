// -OLShots -OLDiffTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLDiffTest) { return OLFlags::Param(TEXT("OLDiffTest")); }

OL_SHOT_TEST(OLDiffTest, 380)
{
	// the difficulty changed in play (user 2026-10-02): the first bad guy's stats at each, an autosave made at the first,
	// the choice changed (the autosave follows), the autosave loaded: the same bad guys, the new choice
	static int32 Step = 0;
	static int32 CountAtSave = 0;
	AOutlawsLevelActor* DL = Level.Get();
	if (!DL || ShotTime < 3.f) return;
	auto First = [DL]() -> AOutlawsEnemy*
	{
		for (const TWeakObjectPtr<AOutlawsEnemy>& W : DL->GetEnemies()) if (W.IsValid() && !W->IsCritter() && !W->IsDead()) return W.Get();
		return nullptr;
	};
	auto Show = [&](const TCHAR* When)
	{
		int32 N = 0;
		for (const TWeakObjectPtr<AOutlawsEnemy>& W : DL->GetEnemies()) if (W.IsValid()) ++N;
		const AOutlawsEnemy* E = First();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT diff %s: chosen %s, placed for %s, %d enemies; %s walk %.2f shoot timer %.2f health %.2f / %.2f damage %.2f aim %.2f aggr %d"), When,
			OutlawsDifficulty::Name(DL->Difficulty), OutlawsDifficulty::Name(DL->SpawnDifficulty), N, E ? *E->GetOutlawsName() : TEXT("-"),
			E ? E->Def.WalkSpeed : 0.f, E ? E->Def.ShootTimer : 0.f, E ? E->GetHealth() : 0.f, E ? E->Def.Health : 0.f, E ? E->Def.Damage : 0.f, E ? E->Def.WeaponInaccuracy : 0.f, E ? E->Def.Aggressiveness : 0);
		return N;
	};
	if (Step == 0) { Show(TEXT("at the start")); CountAtSave = Show(TEXT("saved")); DL->SaveGame(-1); DL->ChangeDifficulty(EOutlawsDifficulty::Ugly); Show(TEXT("changed to Ugly")); DL->ChangeDifficulty(EOutlawsDifficulty::Good); Show(TEXT("changed to Good")); Step = 1; ShotTime = 3.f; }
	else if (Step == 1 && ShotTime > 4.f) { UE_LOG(LogOutlaws, Display, TEXT("OLSHOT diff: loading the autosave")); DL->LoadGame(-1); Step = 2; ShotTime = 3.f; }
	else if (Step == 2 && ShotTime > 12.f)
	{
		const int32 N = Show(TEXT("after the load"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT diff: enemies %d at the save, %d after the load (%s)"), CountAtSave, N, N == CountAtSave ? TEXT("the same") : TEXT("DIFFERENT"));
		// a restart after a load keeps what the level was entered with (the save carries it): gold given, then restarted
		DL->DebugDumpCarry(TEXT("loaded"));
		DL->RestartLevel();
		Step = 3;
		ShotTime = 3.f;
	}
	else if (Step == 3 && ShotTime > 10.f)
	{
		DL->DebugDumpCarry(TEXT("after the restart"));
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
