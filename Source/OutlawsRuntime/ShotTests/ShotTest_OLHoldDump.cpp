// -OLShots -OLHoldDump: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLHoldDump) { return OLFlags::Param(TEXT("OLHoldDump")); }

OL_SHOT_TEST(OLHoldDump, 360)
{
	// the hold each grabbable takes now (backlog 4, step 1: traits.py written from these): the level's props by stem,
	// their world corners read as the hand reads the held copy (upright, so the same plan and height)
	AOutlawsLevelActor* DumpL = Level.Get();
	if (ShotTime > 2.f && DumpL)
	{
		TArray<AOutlawsSpriteActor*> Props;
		DumpL->GrabbableProps(Props);
		TSet<FString> Done;
		for (AOutlawsSpriteActor* S : Props)
		{
			UProceduralMeshComponent* M = S ? Cast<UProceduralMeshComponent>(S->Model) : nullptr;
			if (!M || Done.Contains(S->ModelStem)) continue;
			Done.Add(S->ModelStem);
			const FTransform X = M->GetComponentTransform();
			TArray<FVector> P;
			for (int32 i = 0; i < M->GetNumSections(); ++i)
			{
				if (const FProcMeshSection* Sec = M->GetProcMeshSection(i)) for (const FProcMeshVertex& V : Sec->ProcVertexBuffer) P.Add(X.TransformPosition(V.Position));
			}
			double Rmax = 0.0, Rmed = 0.0, H = 0.0;
			const TCHAR* Mode = HoldModeOf(P, &Rmax, &Rmed, &H);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hold %s: %s (reach %.1f / %.1f, %.1f tall; ratios %.2f %.2f %.2f) trait '%s'"), *S->ModelStem, Mode, Rmax, Rmed, H,
				Rmax / Rmed, Rmax / H, H / Rmed, *AOutlawsLevelActor::HoldModeFor(S->ModelStem));
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hold dump done: %d stems"), Done.Num());
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
