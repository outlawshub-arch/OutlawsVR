// -OLShots -OLCarryTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLCarryTest) { return OLFlags::Param(TEXT("OLCarryTest")); }

OL_SHOT_TEST(OLCarryTest, 420)
{
	// keys and puzzle pieces stay with their level; bags, plates, oil, weapons and ammunition come along
	if (ShotStep == 0 && ShotTime > 3.f && L)
	{
		for (const TCHAR* K : { TEXT("ibrsskey"), TEXT("iironkey"), TEXT("igear1"), TEXT("isqrkeya"), TEXT("ishovel"), TEXT("icrowbar") }) L->DebugGiveListed(K, K);
		L->AddCount(TEXT("idocbag"), 2);
		L->AddCount(TEXT("ibplate"), 50);
		L->AddCount(TEXT("ioil"), 100);
		L->AddCount(TEXT("ishells"), 20);
		L->AddCount(TEXT("isawgun"), 1);
		// in the hands: the sawed-off in one, the crowbar in the other; the gun comes along, the crowbar does not
		Equip(1, 4);
		EquipTool(0, TEXT("icrowbar"));
		L->DebugDumpCarry(TEXT("before"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT carry hands before: left %d tool '%s', right %d tool '%s'"), Equipped[0], *HeldTool[0], Equipped[1], *HeldTool[1]);
		// as the level's closing movie does: everything shown is put away, then brought back after the level change
		HideForMovie(true);
		L->DebugNextLevel();
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 9.f && L)
	{
		L->DebugDumpCarry(TEXT("after"));
		auto Shown = [](const USceneComponent* C) { return C && C->IsVisible() ? TEXT("shown") : TEXT("hidden"); };
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT carry hands after: left %d tool '%s' (crowbar %s), right %d (gun %s, movie hidden %d)"),
			Equipped[0], *HeldTool[0], Shown(ToolRoot[0]), Equipped[1], Shown(WeaponMesh[1]), bMovieHidden ? 1 : 0);
		L->TakeCount(TEXT("idocbag"), 1);                  // used up in the level, then a death restart
		L->RestartLevel();
		++ShotStep;
	}
	else if (ShotStep == 2 && ShotTime > 14.f && L)
	{
		L->DebugDumpCarry(TEXT("after restart"));
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
