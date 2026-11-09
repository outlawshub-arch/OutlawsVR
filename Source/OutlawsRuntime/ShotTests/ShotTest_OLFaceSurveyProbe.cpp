// -OLShots -OLFaceSurveyProbe (BUG-SHOVELHEAD, 9 Nov; the user, widened: every model): measure only, nothing changed.  Every model
// file a load path can draw: the floor's item3d models (OutlawsItem3d::Dir), the held ones (OutlawsItem3d::HeldDir: tools,
// throwables, the gatling and its tripod, the lantern) and the shipped held guns (Content/HeldGuns).  Each loaded as the game
// loads it (OutlawsItem3d::Load, painted; the guns' files plain) and its faces measured as FaceItemModelOutward would turn
// them, nothing applied.  A line per model:
//   "OLSHOT facesurvey <folder> <file>: <tris> triangles in <pieces> pieces, facing out <pct>%, would turn <n>"
// then "OLSHOT facesurvey RESULT <models> models, <k> under 95% facing out: <names>".

#include "ShotTestCommon.h"
#include "OutlawsObjMesh.h"
#include "OutlawsItem3d.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLFaceSurveyProbe) { return OLFlags::Param(TEXT("OLFaceSurveyProbe")); }

OL_SHOT_TEST(OLFaceSurveyProbe, 2582)
{
	static bool bDone = false;
	if (bDone || !L || ShotTime < 5.f || !OutlawsItem3d::CanPaint()) return;
	bDone = true;
	OutlawsItem3d::SetLevel(L);
	struct FDir { const TCHAR* Tag; FString Path; bool bItem3d; };
	const FDir Dirs[] = {
		{ TEXT("floor"), OutlawsItem3d::Dir(), true },
		{ TEXT("held"), OutlawsItem3d::HeldDir(), true },
		{ TEXT("guns"), FPaths::Combine(FPaths::ProjectContentDir(), TEXT("HeldGuns")), false },
	};
	int32 Models = 0;
	TArray<FString> Under;
	for (const FDir& D : Dirs)
	{
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *FPaths::Combine(D.Path, TEXT("*.obj")), true, false);
		Files.Sort();
		for (const FString& F : Files)
		{
			const FString P = FPaths::Combine(D.Path, F);
			OutlawsObj::FObjMesh M;
			const bool bOk = D.bItem3d ? (OutlawsItem3d::Exists(P) && OutlawsItem3d::Load(P, 1.0, true, M, L)) : OutlawsObj::Load(P, 100.0, true, M);
			if (!bOk)
			{
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT facesurvey %s %s: NOT LOADED"), D.Tag, *F);
				continue;
			}
			const OutlawsObj::FFaceTurn Ft = OutlawsObj::TurnFacesOutward(M, false);
			++Models;
			const double Pct = Ft.ShareBefore() * 100.0;
			if (Pct < 95.0) Under.Add(FString::Printf(TEXT("%s/%s %.0f%%"), D.Tag, *F, Pct));
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT facesurvey %s %s: %d triangles in %d pieces, facing out %.1f%%, would turn %d"), D.Tag, *F, Ft.Faces, Ft.Pieces, Pct, Ft.Turned);
		}
	}
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT facesurvey RESULT %d models, %d under 95%% facing out: %s"), Models, Under.Num(), *FString::Join(Under, TEXT(", ")));
	FGenericPlatformMisc::RequestExit(false);
}

#endif // WITH_OUTLAWS_TESTS
