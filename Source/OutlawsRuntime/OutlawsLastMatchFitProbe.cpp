// BUG-LASTMATCHFIT's probe (3 Nov: at the Quest 2 panel size the joiner's locked lobby line "Last match: <game> on <map>  (random
// map and game)" ran past the parchment with a long map name):
//   -OLShots -OLLevel=town -OLNoFrontEnd -OLLastMatchFitProbe [-OLOldLastMatchFit] [-ResX=1832 -ResY=1920 -ForceRes]
// The co-op Join page is opened with one last-match line drawn under its own lines at a time, as the lobby draws its lines (the
// same font and place), and each is checked to sit on the parchment (AOutlawsPlayer::MenuPageFits, and clear of the art's torn
// right edge): the longest stock line (Kill The Fool With The Chicken on Hargrove's Pattern Repair), the reported one (Death
// Match on it), every native map listed (when any are mounted), a long custom map's title, a short one, and the longest lobby
// line from before (its size kept).  One line a case "OLSHOT lastmatchfit
// case PASS|FAIL: ...", then "OLSHOT lastmatchfit RESULT".  -OLOldLastMatchFit: every line at its size, as before (the long
// cases fail there).

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsMpHost.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsLastMatchFit
{
	extern TArray<FString> ProbeLines;
}

OL_SHOT_MATCH(OLLastMatchFitProbe) { return OLFlags::Param(TEXT("OLLastMatchFitProbe")); }

OL_SHOT_TEST(OLLastMatchFitProbe, 1720)
{
	static int32 Passes = 0, Fails = 0, Case = -1;
	static float Wait = 0.f;
	static TArray<FString> FitCases;
	if (!L || ShotStep < 0) return;
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	const bool bOld = OLFlags::Param(TEXT("OLOldLastMatchFit"));
	auto Line = [](EOutlawsGameClass Game, const FString& Map) { return FString::Printf(TEXT("Last match: %s on %s  (random map and game)"), *OutlawsMpHost::ClassLabel(Game), *Map); };
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 3.f || !bUp) return;
		L->bInvincible = true;
		const FString Hargrove = OutlawsMpHost::MapTitle(TEXT("hargrove.rcm"));
		FitCases.Add(Line(EOutlawsGameClass::Chicken, Hargrove));
		FitCases.Add(Line(EOutlawsGameClass::DeathMatch, Hargrove));
		// the native maps by the title the line gives them (their RCM's NAME since REQ-NATIVEMAPS), the longest first
		TArray<FOutlawsMpMap> Maps;
		OutlawsMpHost::ListMaps(L->GetAssets(), Maps);
		TArray<FString> Natives;
		for (const FOutlawsMpMap& M : Maps) if (M.bNative) Natives.Add(OutlawsMpHost::MapTitle(M.File));
		Natives.Sort([](const FString& A, const FString& B) { return A.Len() > B.Len(); });
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lastmatchfit %d maps listed, %d native (longest title: %s)"), Maps.Num(), Natives.Num(), Natives.Num() ? *Natives[0] : TEXT("none mounted"));
		for (const FString& N : Natives) FitCases.Add(Line(EOutlawsGameClass::Chicken, N));
		FitCases.Add(Line(EOutlawsGameClass::Chicken, TEXT("The Long Custom Map Of The Seven Canyons Revisited")));
		FitCases.Add(TEXT("Last match: Death Match on Dry Gulch"));
		FitCases.Add(TEXT("Enter Game when you are ready: the host's Start takes you in"));     // (the longest lobby line before: its size kept)
		if (!bMenuOpen) OpenMenu();
		ShowMenuPage(TEXT("#coopjoin"), true);
		Case = 0;
		Wait = ShotTime + 0.5f;
		ShotStep = 1;
		return;
	}
	case 1:
	{
		if (ShotTime < Wait) return;
		if (Case >= FitCases.Num())
		{
			OutlawsLastMatchFit::ProbeLines.Reset();
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lastmatchfit RESULT %s (%d passed, %d failed%s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
				bOld ? TEXT(", -OLOldLastMatchFit") : TEXT(""));
			ShotStep = -1;
			FGenericPlatformMisc::RequestExit(false);
			return;
		}
		OutlawsLastMatchFit::ProbeLines = { FitCases[Case] };
		RedrawMenu();
		FString Said;
		// the page's fit check (the art's frame, x 100..1530) and the parchment itself: clear of its torn right edge (x 1430 at
		// the lines' height) and centred, the line's ink x 215..1415; a line that fits keeps its size (no wider than 1200 shrunk)
		const bool bFits = MenuPageFits(Said) && FrontInk.bIsValid && FrontInk.Min.X >= 214.f && FrontInk.Max.X <= 1416.f;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lastmatchfit case %s: \"%s\" (%d letters) fits the parchment, ink x %.0f..%.0f, %.0f wide (%s)"), bFits ? TEXT("PASS") : TEXT("FAIL"), *FitCases[Case],
			FitCases[Case].Len(), FrontInk.Min.X, FrontInk.Max.X, FrontInk.Max.X - FrontInk.Min.X, *Said);
		if (bFits) ++Passes; else ++Fails;
		if (Case == 0)
		{
			const FString Name = FString::Printf(TEXT("lastmatchfit_long%s"), bOld ? TEXT("_old") : TEXT(""));
			FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		}
		++Case;
		Wait = ShotTime + 0.5f;
		return;
	}
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
