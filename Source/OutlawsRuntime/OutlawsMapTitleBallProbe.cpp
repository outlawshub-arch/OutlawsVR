// REQ-NATIVEMAPS part 1's second probe (user 2 Nov): the native maps' titles and the chicken's BALLSTAR places.
//   -OLShots -OLNativeMaps=<dir> -OLMpHost=drygulch -OLMpClass=dm -OLMpBots=0 -OLMpStartWhen=0 -OLMapTitleBallProbe
// (1) the titles: the match line of a native map names it by its RCM's NAME (Armadillo: "Capture The Flag on Armadillo by
// T4H_Dingle", was "... on armad"), and every native map's title is its RCM's NAME, not its file's.  (2) the launched host
// hosts arizonas (no flag place) in Kill The Fool With The Chicken: the chicken starts on one of the map's BALLSTAR places,
// not on a player start (it landed on one before).  One shot of the chicken's place.  "OLSHOT mapball RESULT".  With
// -OLOldBallStar the chicken goes back to the old place (its check FAILs: the old behaviour shown).

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsLevel.h"
#include "OutlawsGeometry.h"
#include "OutlawsCoop.h"
#include "OutlawsMpHost.h"
#include "OutlawsMatchModes.h"
#include "OutlawsLab.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsMapTitleBallProbeLocal
{
	int32 Stage = 0, Passes = 0, Fails = 0;
	double StageAt = 0.0;
	FString Target;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mapball check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}
}

OL_SHOT_MATCH(OLMapTitleBallProbe) { return OLFlags::Param(TEXT("OLMapTitleBallProbe")); }

OL_SHOT_TEST(OLMapTitleBallProbe, 1760)
{
	using namespace OutlawsMapTitleBallProbeLocal;
	if (!L || ShotTime < 0.f || Stage < 0) return;
	const double Now = FPlatformTime::Seconds();
	L->StartSaveDelay = -1.f;           // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	const bool bOld = OLFlags::Param(TEXT("OLOldBallStar"));
	UOutlawsCoop* C = UOutlawsCoop::Get(this);
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mapball RESULT %s (%d passed, %d failed%s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			bOld ? TEXT(", -OLOldBallStar") : TEXT(""));
		ShotTime = -1.f;
		Stage = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	if (Stage == 0)
	{
		const FOutlawsAssets* A = L->GetLabAssets();
		if (!A || !C || !C->IsHost() || !C->IsMatch() || !C->bLevelBuilt || ShotTime < 3.f)
		{
			if (ShotTime < 90.f) return;
			Check(false, TEXT("needs a launched host: -OLMpHost=drygulch -OLMpClass=dm -OLMpBots=0 -OLMpStartWhen=0"));
			Finish();
			return;
		}
		// (1) the titles
		const FString Line = UOutlawsCoop::MatchLineFor(uint8(EOutlawsGameClass::CaptureTheFlag), TEXT("armad.rcm"), 0);
		Check(Line.Contains(TEXT("on Armadillo")), FString::Printf(TEXT("the match line of armad.rcm: \"%s\" (want it to name Armadillo)"), *Line));
		TArray<FOutlawsMpMap> Maps;
		OutlawsMpHost::ListMaps(A, Maps);
		int32 Natives = 0, Named = 0;
		TArray<FString> Off;
		for (const FOutlawsMpMap& M : Maps)
		{
			if (!M.bNative) continue;
			++Natives;
			// the title is the RCM's NAME (Bankrupt's and Dool's NAME is their file's name, so not "differs from the file")
			const FString T = OutlawsMpHost::MapTitle(M.File);
			if (T == M.Name) ++Named; else Off.Add(FString::Printf(TEXT("%s: %s"), *M.File, *T));
		}
		Check(Natives > 0 && Named == Natives, FString::Printf(TEXT("%d of %d native maps titled by their RCM's NAME%s%s"), Named, Natives, Off.Num() ? TEXT(": off ") : TEXT(""), *FString::Join(Off, TEXT(", "))));
		// (2) arizonas in Kill The Fool With The Chicken
		const FOutlawsMpMap* Az = Maps.FindByPredicate([](const FOutlawsMpMap& M) { return M.bNative && M.Level == TEXT("arizonas"); });
		if (!Az) { Check(false, TEXT("arizonas is not listed (-OLNativeMaps=<dir>)")); Finish(); return; }
		FOutlawsMpHostOptions O = C->MatchOptions;
		O.bRandomMap = O.bRandomClass = false;
		O.bBots = false;
		O.MapFile = Az->File;
		O.GameClass = EOutlawsGameClass::Chicken;
		Target = Az->Level;
		if (!C->StartNextMatch(O, TEXT("the map title and BALLSTAR probe")))
		{
			Check(false, TEXT("arizonas: StartNextMatch refused"));
			Finish();
			return;
		}
		Stage = 1;
		StageAt = Now;
		return;
	}
	if (!C) return;
	AOutlawsCoopSession* S = C->GetSession();
	if (Stage == 1)
	{
		const bool bBuilt = C->bLevelBuilt && C->BuiltLevel.Equals(Target, ESearchCase::IgnoreCase) && L->LevelName.Equals(Target, ESearchCase::IgnoreCase)
			&& !L->IsRebuildPending() && C->bMatchSetUp && S && S->bHostReady && !bMenuOpen;
		if (!bBuilt)
		{
			if (Now - StageAt < 120.0) return;
			Check(false, FString::Printf(TEXT("arizonas not ready in 120 s (report: %s)"), *L->GetBuildReport().Left(200)));
			Finish();
			return;
		}
		Stage = 2;
		StageAt = Now;
		return;
	}
	if (Stage == 2)
	{
		if (Now - StageAt < 2.0) return;
		const FOutlawsLevel* Lv = L->LiveLevel();
		TArray<FVector> Balls, Starts;
		if (Lv)
		{
			for (const FOutlawsObject& Ob : Lv->Objects)
			{
				const FVector At = FOutlawsGeometry::ToUE(Ob.Pos.X, Ob.Pos.Y, Ob.Pos.Z);
				if (Ob.Name == TEXT("BALLSTAR")) Balls.Add(At);
				else if (Ob.Name == TEXT("STARTPOS") || Ob.Name == TEXT("PLAYER")) Starts.Add(At);
			}
		}
		const FOutlawsModeItem* Chicken = C->Modes.Items.FindByPredicate([](const FOutlawsModeItem& I) { return I.Kind == EOutlawsModeItemKind::Chicken; });
		double ToBall = 1e9, ToStart = 1e9;
		if (Chicken)
		{
			for (const FVector& B : Balls) ToBall = FMath::Min(ToBall, FVector::Dist(B, Chicken->Home));
			for (const FVector& St : Starts) ToStart = FMath::Min(ToStart, FVector::Dist(St, Chicken->Home));
		}
		const bool bClass = S && S->MatchClass == uint8(EOutlawsGameClass::Chicken);
		Check(bClass && Chicken && Balls.Num() > 0 && ToBall < 1.0 && ToStart > 100.0,
			FString::Printf(TEXT("arizonas, Kill The Fool With The Chicken (class %d): the chicken's home %s is %.0f cm from the nearest of the map's %d BALLSTAR, %.0f cm from the nearest of %d player starts (want on a BALLSTAR, off every start)"),
				S ? int32(S->MatchClass) : -1, Chicken ? *Chicken->Home.ToCompactString() : TEXT("none"), ToBall, Balls.Num(), ToStart, Starts.Num()));
		if (AController* Ctl = GetController(); Ctl && Chicken)
		{
			const FVector Eye = GetActorLocation();
			Ctl->SetControlRotation((Chicken->Home - Eye).Rotation());
		}
		StageAt = Now;
		Stage = 3;
		return;
	}
	if (Stage == 3)
	{
		if (Now - StageAt < 1.0) return;
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / (bOld ? TEXT("ol_mapball_arizonas_old.png") : TEXT("ol_mapball_arizonas.png")), false, false);
		StageAt = Now;
		Stage = 4;
		return;
	}
	if (Stage == 4 && Now - StageAt > 1.5) Finish();
}

#endif // WITH_OUTLAWS_TESTS
