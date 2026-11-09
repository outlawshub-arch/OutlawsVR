// -OLShots -OLShellSideProbe (BUG-SHOVELHEAD, 9 Nov; the user: the held shovel's blade draws black on the headset, it should be
// grey).  MEASURED cause: the headset's ink shell (OutlawsInk::AddShell, the outline the PC draws as a post process) went out on
// the wrong side of the blade's corners, whose file normals point against their faces, and drew over it.  This probe builds that
// shell (as the headset does, on any machine) for each held thing: the shovel, the crowbar, the pen, the revolver, the rifle, and
// counts the shell corners put out against every face at them.  A line per thing:
//   "OLSHOT shellside <thing>: <n> shell corners against their faces"
// checks "OLSHOT shellside check PASS|FAIL: ...", then "OLSHOT shellside RESULT <n> PASS, <m> FAIL".

#include "ShotTestCommon.h"
#include "OutlawsInk.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLShellSideProbe) { return OLFlags::Param(TEXT("OLShellSideProbe")); }

OL_SHOT_TEST(OLShellSideProbe, 2583)
{
	static int32 Step = 0, Pass = 0, Fail = 0;
	if (Step < 0 || !L || ShotTime < 5.f) return;
	auto Check = [&](bool bOk, const FString& What)
	{
		(bOk ? Pass : Fail)++;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT shellside check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
	};
	UMaterialInterface* Ink = HeldSolidMaterial(FColor(10, 8, 7));
	auto Shell = [&](const TCHAR* Thing, UPrimitiveComponent* C)
	{
		UProceduralMeshComponent* P = Cast<UProceduralMeshComponent>(C);
		if (!P)
		{
			Check(false, FString::Printf(TEXT("%s: a mesh to shell"), Thing));
			return;
		}
		OutlawsInk::ShellWrongCorners(true);
		OutlawsInk::AddShell(P, 0.15f, Ink);
		const int32 N = OutlawsInk::ShellWrongCorners(true);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT shellside %s: %d shell corners against their faces (%d sections)"), Thing, N, P->GetNumSections());
		Check(N == 0, FString::Printf(TEXT("%s: every shell corner behind its faces (%d against)"), Thing, N));
	};
	auto ToolMesh = [&](int32 H) -> UPrimitiveComponent*
	{
		if (!ToolRoot[H]) return nullptr;
		TArray<USceneComponent*> Kids;
		ToolRoot[H]->GetChildrenComponents(true, Kids);
		for (USceneComponent* K : Kids) if (UProceduralMeshComponent* P = Cast<UProceduralMeshComponent>(K)) return P;
		return nullptr;
	};
	if (Step == 0)
	{
		L->AddCount(TEXT("ishovel"), 1);
		L->AddCount(TEXT("icrowbar"), 1);
		EquipTool(0, TEXT("ishovel"));
		EquipTool(1, TEXT("icrowbar"));
		Step = 1;
		return;
	}
	if (Step == 1)
	{
		Shell(TEXT("shovel"), ToolMesh(0));
		Shell(TEXT("crowbar"), ToolMesh(1));
		L->AddCount(TEXT("iink"), 1);
		EquipTool(1, TEXT("iink"));
		Step = 2;
		return;
	}
	if (Step == 2)
	{
		Shell(TEXT("pen"), ToolMesh(1));
		DropTool(0);
		DropTool(1);
		int32 Rev = -1, Rifle = -1;
		for (int32 i = 0; i < 32; ++i)
		{
			if (Rev < 0 && WeaponLabel(i).Contains(TEXT("revolver"))) Rev = i;
			if (Rifle < 0 && WeaponLabel(i).Equals(TEXT("Rifle"))) Rifle = i;
		}
		Equip(0, Rifle);
		Equip(1, Rev);
		Step = 3;
		return;
	}
	if (Step == 3)
	{
		Shell(TEXT("revolver"), WeaponMesh[1]);
		Shell(TEXT("rifle"), WeaponMesh[0]);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT shellside RESULT %d PASS, %d FAIL"), Pass, Fail);
		Step = -1;
		FGenericPlatformMisc::RequestExit(false);
	}
}

#endif // WITH_OUTLAWS_TESTS
