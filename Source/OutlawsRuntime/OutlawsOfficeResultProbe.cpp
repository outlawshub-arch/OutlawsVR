// REQ-OFFICECAPTURED (user 2 Nov, the user's snapshot #38, the office): -OLShots -OLLevel=office -OLNoFrontEnd
// -OLOfficeResultProbe [-OLOldOfficeResult].  (a) back in the office from the Wilderness with Bloodeye Tim captured (the
// session as HomMissionEnd leaves it): no "You captured" line, the poster's stamp (PORCH5D's event) and the cell's figure
// (NATIM, generator #B05D) still there; (b) HomResume from the session that arrival saved: no "Welcome back" line, the
// stamp and the figure again.  -OLOldOfficeResult: both lines shown as before.  Lines "OLSHOT officeresult check
// PASS|FAIL: ..." and "OLSHOT officeresult RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLOfficeResultProbe) { return OLFlags::Param(TEXT("OLOfficeResultProbe")); }

OL_SHOT_TEST(OLOfficeResultProbe, 1490)
{
	static int32 Passes = 0, Fails = 0;
	static float Wait = 0.f;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT officeresult check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	const bool bOld = OLFlags::Param(TEXT("OLOldOfficeResult"));
	auto Shown = [this](const TCHAR* Part)
	{
		for (const FString& Line : HudLines) if (Line.Contains(Part)) return true;
		return false;
	};
	// the porch's event done (PORCH5D's elevator steps to its stop 1 and straight back to 0, so it shows in what it sent): the
	// cell's figure shown, the poster's wall moved on from its first frame by the GOTO_STOP 3
	auto Stamped = [L, &Check](const TCHAR* When)
	{
		const TWeakObjectPtr<AOutlawsSpriteActor>* G = L->CellGens.Find(TEXT("B05D"));
		Check(G && G->IsValid() && !(*G)->IsHidden(), FString::Printf(TEXT("%s: the cell's figure (#B05D, NATIM) shown (%s)"), When, G && G->IsValid() ? *(*G)->OutlawsName : TEXT("none")));
		TArray<FString> Walls;
		L->DebugAtxReport().ParseIntoArray(Walls, TEXT(" | "));
		const FString* Poster = Walls.FindByPredicate([](const FString& W) { return W.StartsWith(TEXT("wall ")) && W.Contains(TEXT("BLDEYE")); });
		Check(Poster && !Poster->Contains(TEXT(" pc 0 ")), FString::Printf(TEXT("%s: the poster's wall on its stamped frame (%s)"), When, Poster ? **Poster : TEXT("none")));
	};
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 4.f || !bUp) return;
		L->bInvincible = true;
		// the session as HomMissionEnd leaves it after the Wilderness, Tim taken alive (StartMission clears it, so it is set
		// after, as HomResume does)
		L->StartMission(TEXT("office"), EOutlawsDifficulty::Good);
		L->bHom = true;
		L->HomMission = TEXT("shwildns");
		L->HomReturnSector = TEXT("PORCH5D");
		L->HomPorchEvents.Reset();
		L->HomPorchEvents.Add(TEXT("PORCH5D"), 131072);
		L->HomGameScore = 3100;
		L->HomResultText = TEXT("You captured Bloodeye Tim!  Gold 2, kills 6, wounds 3, bounty 1000: level score 1335, game score 3100.");
		HudLines.Reset();
		HudLeft.Reset();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT officeresult: back to the office with Tim captured%s"), bOld ? TEXT(" (-OLOldOfficeResult)") : TEXT(""));
		ShotStep = 1;
		return;
	}
	case 1:
	case 3:
		if (!bUp || !L->LevelName.Equals(TEXT("office"), ESearchCase::IgnoreCase)) return;
		Wait = ShotTime + 2.f;
		++ShotStep;
		return;
	case 2:
	{
		if (ShotTime < Wait) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT officeresult: arrival: hud lines %d: %s"), HudLines.Num(), *FString::Join(HudLines, TEXT(" / ")));
		const bool bLine = Shown(TEXT("You captured"));
		Check(bOld ? bLine : !bLine, FString::Printf(TEXT("arrival: the \"You captured\" line %s"), bLine ? TEXT("shown") : TEXT("not shown")));
		Check(L->bHom && L->HomResultText.IsEmpty(), FString::Printf(TEXT("arrival: the session on (hom %d), the result taken (\"%s\")"), L->bHom ? 1 : 0, *L->HomResultText));
		Stamped(TEXT("arrival"));
		Check(AOutlawsLevelActor::HasHomSave(), TEXT("arrival: the session saved for a resume"));
		HudLines.Reset();
		HudLeft.Reset();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT officeresult: Historical Missions resumed from that save"));
		L->HomResume();
		ShotStep = 3;
		return;
	}
	case 4:
	{
		if (ShotTime < Wait) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT officeresult: resume: hud lines %d: %s"), HudLines.Num(), *FString::Join(HudLines, TEXT(" / ")));
		const bool bLine = Shown(TEXT("Welcome back"));
		Check(bOld ? bLine : !bLine, FString::Printf(TEXT("resume: the \"Welcome back\" line %s"), bLine ? TEXT("shown") : TEXT("not shown")));
		Check(L->bHom && L->HomGameScore == 3100, FString::Printf(TEXT("resume: the session back (hom %d, score %d)"), L->bHom ? 1 : 0, L->HomGameScore));
		Stamped(TEXT("resume"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT officeresult RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	}
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
