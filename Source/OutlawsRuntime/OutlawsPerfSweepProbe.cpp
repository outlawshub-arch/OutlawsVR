// REQ-PERFSWEEP: -OLPerfSweepProbe (any build, through OLFlags: nothing runs without the flag; on the desktop, best as the
// headset's path: -featureleveles31 -OLDeviceProfile=Quest2, with -OLPerfSweepPhaseSec=3 for short phases), from the main
// menu as a player goes: New Game's turn shows Settings sweep beside Performance test; picked, The Town with the player at the
// sweep's spot in god mode, the marker ahead, the phases as built (Quest 2 Mode skipped when the profile is forced); at each
// switch the rows applied as the phase says and the settings file's md5 unchanged; at the end every phase measured with sane
// numbers, the table in Saved/Perf/sweep_<stamp>.txt, every row, the level's switches and the cvars put back, the top-3 card,
// then the main menu with god mode as before and the Saved folder (but Perf and Logs) unchanged.  Then a second sweep quit with
// End Game in its third phase (no Restart Level, Save or Load on its pause page): two phases done, the rows put back.
// -OLPerfSweepProbe=closed quits the game in the first sweep's second phase (its "game closed" end line is the runner's).
// -OLPerfSweepProbe=spots: the survey that chose the spot (a Performance test run in The Town; the user's three busy spots
// twice over, eight headings each: bad guys awake, sectors, draws, triangles and timings), no checks.
// "OLPERFSWEEP check PASS|FAIL: ..." a check, then "OLSHOT perfsweep: PASS|FAIL" and the exit.

#include "OutlawsPlayer.h"
#include "OutlawsPerfTest.h"
#include "OutlawsPerf.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsLevelActor.h"
#include "OutlawsEnemy.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsDeviceProfile.h"
#include "GameFramework/Controller.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

namespace
{
	struct FSweepProbe
	{
		int32 Step = 0;
		double At = 0.0, Deadline = 0.0;
		int32 Turns = 0, Run = 0, Pass = 0, Fail = 0;
		int32 Seen = 0;                       // the last phase checked
		FString Mode;                         // "", "closed" or "spots"
		TMap<FString, FString> SavedBefore;   // Saved's files and md5 (Perf and Logs left out)
		FString IniMd5;                       // OutlawsSettings.ini's ("absent")
		TMap<FString, FString> Rows;          // the Graphics rows (OutlawsDeviceProfile::GatherSettings)
		bool bHD = false, bHDChosen = false, bStochastic = false, bTinting = false, bGod = false;
		FString Pd, Fps;                      // vr.PixelDensity, ol.perf.TargetFps
		int32 Spot = 0, Pass2 = 0, Heading = 0, Sub = 0;     // (=spots)
		bool bDone = false;
	};
	FSweepProbe GSp;

	void SpCheck(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLPERFSWEEP check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		(bOk ? GSp.Pass : GSp.Fail)++;
	}

	// the Saved folder's files and their md5 (the Performance test probe's rule): Perf and Logs (the run's own) left out, and
	// the folders the editor and the engine write on their own; OutlawsSettings.ini, at the top, is in
	TMap<FString, FString> SpSavedMd5()
	{
		static const TCHAR* const Skip[] = { TEXT("Perf"), TEXT("Logs"), TEXT("Autosaves"), TEXT("Collections"), TEXT("Config"), TEXT("Cooked"), TEXT("Crashes"),
			TEXT("MaterialStats"), TEXT("MaterialStatsDebug"), TEXT("ShaderDebugInfo"), TEXT("Shaders"), TEXT("SourceControl"), TEXT("StagedBuilds"), TEXT("Temp"), TEXT("OLShots") };
		const FString Root = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());
		IFileManager& Fm = IFileManager::Get();
		TArray<FString> Files, Dirs;
		Fm.FindFiles(Files, *(Root / TEXT("*")), true, false);
		for (FString& F : Files) F = Root / F;
		Fm.FindFiles(Dirs, *(Root / TEXT("*")), false, true);
		for (const FString& D : Dirs)
		{
			bool bSkip = false;
			for (const TCHAR* S : Skip) bSkip |= D.Equals(S, ESearchCase::IgnoreCase);
			if (!bSkip) Fm.FindFilesRecursive(Files, *(Root / D), TEXT("*"), true, false, false);
		}
		TMap<FString, FString> Out;
		for (const FString& F : Files)
		{
			FString Rel = FPaths::ConvertRelativePathToFull(F).Mid(Root.Len());
			Rel.RemoveFromStart(TEXT("/"));
			Out.Add(Rel, LexToString(FMD5Hash::HashFile(*F)));
		}
		return Out;
	}

	FString SpDiff(const TMap<FString, FString>& Was, const TMap<FString, FString>& Now)
	{
		TArray<FString> D;
		for (const TPair<FString, FString>& Kv : Was)
		{
			const FString* N = Now.Find(Kv.Key);
			if (!N) D.Add(Kv.Key + TEXT(" gone"));
			else if (*N != Kv.Value) D.Add(Kv.Key + TEXT(" changed"));
		}
		for (const TPair<FString, FString>& Kv : Now) if (!Was.Contains(Kv.Key)) D.Add(Kv.Key + TEXT(" new"));
		return D.Num() == 0 ? FString(TEXT("none")) : FString::Join(D, TEXT(", "));
	}

	FString SpIniMd5()
	{
		const FString Ini = FPaths::ProjectSavedDir() / TEXT("OutlawsSettings.ini");
		return FPaths::FileExists(Ini) ? LexToString(FMD5Hash::HashFile(*Ini)) : FString(TEXT("absent"));
	}

	FString SpCvar(const TCHAR* Name)
	{
		IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(Name);
		return V ? V->GetString() : FString(TEXT("none"));
	}

	const TCHAR* const SpDetail[] = { TEXT("Near"), TEXT("Normal"), TEXT("Far") };
}

bool AOutlawsPlayer::PerfSweepProbeOn()
{
	static const bool bOn = OLFlags::Has(TEXT("OLPerfSweepProbe"));
	return bOn;
}

void AOutlawsPlayer::TickPerfSweepProbe(float Dt)
{
	using namespace OutlawsPerfTest;
	FSweepProbe& Sp = GSp;
	AOutlawsLevelActor* L = Level.Get();
	if (Sp.bDone || !L) return;
	const double Now = FPlatformTime::Seconds();
	if (Now < Sp.At) return;
	if (Sp.Step == 0) OLFlags::Value(TEXT("OLPerfSweepProbe"), Sp.Mode);
	const FSweep& S = Sweep();
	auto Rows = [this]()
	{
		FString R;
		if (MenuPage) for (const OutlawsMenu::FItem& It : MenuPage->Items) R += (R.IsEmpty() ? TEXT("") : TEXT(" | ")) + It.Label;
		return R;
	};
	auto AtMenu = [this, L]() { return L->IsFrontEnd() && !L->IsRebuildPending() && bMenuOpen && bFrontMenu && MenuPage && MenuPage->File == TEXT("#front"); };
	auto Turn = [this](float Deg) { if (Controller) { FRotator R = Controller->GetControlRotation(); R.Yaw += Deg; Controller->SetControlRotation(R); } };
	auto Wait = [&Sp, Now](double Seconds, double Limit) { Sp.At = Now + Seconds; Sp.Deadline = Now + Limit; ++Sp.Step; };
	auto Late = [&Sp, Now](const TCHAR* What)
	{
		if (Now < Sp.Deadline) return false;
		SpCheck(false, FString::Printf(TEXT("timed out waiting for %s"), What));
		Sp.Step = 99;
		return true;
	};
	auto Press = [this](const TCHAR* Label)
	{
		const int32 i = bMenuOpen && MenuPage ? MenuPage->Items.IndexOfByPredicate([Label](const OutlawsMenu::FItem& It) { return It.Label.Equals(Label, ESearchCase::IgnoreCase); }) : INDEX_NONE;
		if (i != INDEX_NONE) MenuActivate(1, i);
		return i != INDEX_NONE;
	};
	// (REQ-PERFDIFF) a run's row opens the difficulty page, lit on the last difficulty pressed (else Bad); Pick pressed there
	auto PressRun = [&](const TCHAR* Row, int32 Pick)
	{
		static const TCHAR* const Names[] = { TEXT("Good"), TEXT("Bad"), TEXT("Ugly") };
		const int32 WantLit = NewGameDifficulty() >= 0 ? NewGameDifficulty() : int32(EOutlawsDifficulty::Bad);
		const bool bRow = Press(Row);
		const bool bPage = MenuPage && MenuPage->File.StartsWith(TEXT("#difficulty:perf"));
		const int32 Lit = MenuHover[1] >= 0 ? MenuHover[1] : MenuHover[0];
		SpCheck(bRow && bPage && Lit == WantLit && !IsOn(), FString::Printf(TEXT("sweep %d: %s opens the difficulty page (%s), lit on %d (wanted %d), %s picked"), Sp.Run, Row,
			MenuPage ? *MenuPage->File : TEXT("none"), Lit, WantLit, Names[Pick]));
		return bRow && bPage && Press(Names[Pick]);
	};
	// what is to be put back: the rows, the level's switches, the cvars, god mode, the settings file, the Saved folder
	auto Keep = [&]()
	{
		Sp.SavedBefore = SpSavedMd5();
		Sp.IniMd5 = SpIniMd5();
		Sp.Rows.Reset();
		OutlawsDeviceProfile::GatherSettings(Sp.Rows);
		Sp.bHD = L->bHDTextures;
		Sp.bHDChosen = L->bHDChosen;
		Sp.bStochastic = L->bStochastic;
		Sp.bTinting = L->bPaletteGrades;
		Sp.bGod = L->bInvincible;
		Sp.Pd = SpCvar(TEXT("vr.PixelDensity"));
		Sp.Fps = SpCvar(TEXT("ol.perf.TargetFps"));
	};
	auto Restored = [&](const TCHAR* After)
	{
		TMap<FString, FString> Now2;
		OutlawsDeviceProfile::GatherSettings(Now2);
		FString Bad;
		for (const TPair<FString, FString>& Kv : Sp.Rows) if (Now2.FindRef(Kv.Key) != Kv.Value) Bad += FString::Printf(TEXT(" %s %s (was %s)"), *Kv.Key, *Now2.FindRef(Kv.Key), *Kv.Value);
		if (Now2.Num() != Sp.Rows.Num()) Bad += TEXT(" (another key count)");
		if (L->bHDTextures != Sp.bHD) Bad += TEXT(" remastered");
		if (L->bHDChosen != Sp.bHDChosen) Bad += TEXT(" remastered-chosen");
		if (L->bStochastic != Sp.bStochastic) Bad += TEXT(" stochastic");
		if (L->bPaletteGrades != Sp.bTinting) Bad += TEXT(" tinting");
		if (SpCvar(TEXT("vr.PixelDensity")) != Sp.Pd) Bad += FString::Printf(TEXT(" vr.PixelDensity %s (was %s)"), *SpCvar(TEXT("vr.PixelDensity")), *Sp.Pd);
		if (SpCvar(TEXT("ol.perf.TargetFps")) != Sp.Fps) Bad += FString::Printf(TEXT(" ol.perf.TargetFps %s (was %s)"), *SpCvar(TEXT("ol.perf.TargetFps")), *Sp.Fps);
		SpCheck(Bad.IsEmpty() && SpIniMd5() == Sp.IniMd5, FString::Printf(TEXT("sweep %d: every row put back after %s (%d rows, resolution %s, refresh %s, lamps %s, detail %s, Quest 2 Mode %s; settings file %s)%s"),
			Sp.Run, After, Sp.Rows.Num(), *OutlawsDeviceProfile::RowValue(TEXT("r_resolution")), *OutlawsDeviceProfile::RowValue(TEXT("r_refresh")), *OutlawsDeviceProfile::RowValue(TEXT("r_lamps")),
			*OutlawsDeviceProfile::RowValue(TEXT("r_objdetail")), *OutlawsDeviceProfile::RowValue(TEXT("r_quest2mode")), SpIniMd5() == Sp.IniMd5 ? TEXT("unchanged") : TEXT("CHANGED"),
			Bad.IsEmpty() ? TEXT("") : *(TEXT("; differs:") + Bad)));
	};
	// the phase's rows as applied now, against what it says
	auto PhaseCheck = [&](int32 i)
	{
		const FSweepPhase& Ph = S.Phases[i - 1];
		TArray<FString> Bad;
		if (OutlawsDeviceProfile::ResolutionPercent() != Ph.Set.Resolution) Bad.Add(FString::Printf(TEXT("resolution %d"), OutlawsDeviceProfile::ResolutionPercent()));
		if (OutlawsDeviceProfile::LampLights() != Ph.Set.Lamps) Bad.Add(FString::Printf(TEXT("lamps %d"), OutlawsDeviceProfile::LampLights()));
		if (OutlawsDeviceProfile::RowValue(TEXT("r_objdetail")) != SpDetail[Ph.Set.Detail]) Bad.Add(TEXT("detail ") + OutlawsDeviceProfile::RowValue(TEXT("r_objdetail")));
		if (L->bHDTextures != Ph.Set.bRemastered) Bad.Add(TEXT("remastered"));
		if (L->bStochastic != Ph.Set.bStochastic) Bad.Add(TEXT("stochastic"));
		if (L->bPaletteGrades != Ph.Set.bTinting) Bad.Add(TEXT("tinting"));
		const bool bQ2Skipped = S.Skipped.ContainsByPredicate([](const FString& K) { return K.StartsWith(TEXT("Quest 2 Mode")); });
		if (!bQ2Skipped && OutlawsDeviceProfile::Quest2Mode() != Ph.Set.bQuest2) Bad.Add(TEXT("Quest 2 Mode"));
		const FString Ini = SpIniMd5();
		const double Off = FVector::Dist2D(GetActorLocation(), S.Spot);
		SpCheck(Bad.Num() == 0 && Ini == Sp.IniMd5 && L->bInvincible && Off < 100.0, FString::Printf(TEXT("sweep %d phase %d of %d: %s %s (%s) applied (resolution %d, lamps %d, detail %s, remastered %d, stochastic %d, tinting %d, Quest 2 Mode %d); settings file %s; god mode %d, %.0f cm off the spot%s"),
			Sp.Run, i, S.Phases.Num(), *Ph.Row, *Ph.Value, Ph.bB ? TEXT("B") : TEXT("A"), OutlawsDeviceProfile::ResolutionPercent(), OutlawsDeviceProfile::LampLights(),
			*OutlawsDeviceProfile::RowValue(TEXT("r_objdetail")), L->bHDTextures ? 1 : 0, L->bStochastic ? 1 : 0, L->bPaletteGrades ? 1 : 0, OutlawsDeviceProfile::Quest2Mode() ? 1 : 0,
			Ini == Sp.IniMd5 ? TEXT("unchanged") : TEXT("CHANGED"), L->bInvincible ? 1 : 0, Off, Bad.Num() ? *(TEXT("; wrong: ") + FString::Join(Bad, TEXT(", "))) : TEXT("")));
	};
	auto Sane = [](const FSweepPhase& Ph)
	{
		return Ph.bMeasured && Ph.M.Frames > 0 && Ph.M.Fps > 1.0 && Ph.M.Fps < 2000.0 && Ph.M.GameWorkMs > 0.0 && Ph.M.GameWorkMs < 1000.0 && Ph.M.RenderMs > 0.0 && Ph.M.GpuMs >= 0.0
			&& Ph.M.OverPct >= 0.0 && Ph.M.OverPct <= 100.0 && Ph.Awake >= 0.0;
	};
	// (=spots) the survey's heading: one window's numbers and the bad guys awake
	auto Survey = [&]()
	{
		static const FVector Spots[] = { FVector(13007.0, 249.0, 89.0), FVector(2897.0, 402.0, 96.0), FVector(4940.0, 1489.0, 89.0) };
		const FVector& At = Spots[Sp.Spot];
		const float Yaw = Sp.Heading * 45.f;
		if (Sp.Sub == 0)
		{
			if (Sp.Heading == 0) L->PerfSweepPlace(At, 0.f, true);
			if (Controller) Controller->SetControlRotation(FRotator(0.0, Yaw, 0.0));
			Sp.Sub = 1;
			Sp.At = Now + (Sp.Heading == 0 ? 4.0 : 1.0);
			return;
		}
		if (Sp.Sub == 1)
		{
			OutlawsPerf::BeginWindow(OutlawsPerf::BudgetMs());
			Sp.Sub = 2;
			Sp.At = Now + 1.5;
			return;
		}
		const OutlawsPerf::FWindow W = OutlawsPerf::EndWindow();
		int32 Awake = 0, Alive = 0;
		for (const TWeakObjectPtr<AOutlawsEnemy>& E : L->GetEnemies())
		{
			if (!E.IsValid() || E->IsDead()) continue;
			++Alive;
			Awake += E->PerfAwake() ? 1 : 0;
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLPERFSWEEP spot %d (%.0f, %.0f) pass %d yaw %3.0f: awake %d of %d alive, sectors %d, draws %.0f, tris %.0f, frame %.2f ms, work %.2f, render %.2f, GPU %.2f ms (at %s)"),
			Sp.Spot + 1, At.X, At.Y, Sp.Pass2 + 1, Yaw, Awake, Alive, L->PerfVisibleSectors(), W.Draws, W.Tris, W.FrameMs, W.GameWorkMs, W.RenderMs, W.GpuMs, *GetActorLocation().ToCompactString());
		Sp.Sub = 0;
		if (++Sp.Heading < 8) return;
		Sp.Heading = 0;
		if (++Sp.Spot < 3) return;
		Sp.Spot = 0;
		if (++Sp.Pass2 < 2) return;
		Sp.Step = 99;
	};
	switch (Sp.Step)
	{
	case 0:
		if (!AtMenu()) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLPERFSWEEP the main menu: %s (mode %s)"), *Rows(), Sp.Mode.IsEmpty() ? TEXT("whole") : *Sp.Mode);
		Wait(2.0, 30.0);
		return;
	case 1:
		ShowMenuPage(TEXT("#newgame"), true);
		Sp.Turns = 0;
		Wait(0.5, 10.0);
		return;
	case 2:
		// the turn on the spot with New Game's page up, 20 degrees a step (the Performance test's reveal)
		if (!bPerfTestShown && Sp.Turns < 40) { Turn(20.f); ++Sp.Turns; Sp.At = Now + 0.05; return; }
		if (Controller) Controller->SetControlRotation(FRotator(0.0, MenuYaw, 0.0));
		Wait(0.5, 10.0);
		return;
	case 3:
	{
		const FString R = Rows();
		SpCheck(R == TEXT("Main Game | Historical Missions | Multiplayer | Tutorial | Performance test | Settings sweep"), FString::Printf(TEXT("New Game's page after the turn: %s"), *R));
		Keep();
		Sp.Run = 1;
		Sp.Seen = 0;
		const bool bPressed = PressRun(Sp.Mode == TEXT("spots") ? TEXT("Performance test") : TEXT("Settings sweep"), int32(EOutlawsDifficulty::Ugly));
		SpCheck(bPressed && IsOn() && (IsSweep() == (Sp.Mode != TEXT("spots"))), FString::Printf(TEXT("sweep 1: %s pressed (%d files in Saved to keep, the settings file %s)"),
			Sp.Mode == TEXT("spots") ? TEXT("Performance test (the survey)") : TEXT("Settings sweep"), Sp.SavedBefore.Num(), *Sp.IniMd5));
		Wait(0.0, 240.0);
		return;
	}
	case 4:
	{
		if (!IsRunning()) { Late(TEXT("the sweep's start")); return; }
		if (Sp.Mode == TEXT("spots"))
		{
			L->bInvincible = true;
			Sp.Step = 50;
			Sp.At = Now + 2.0;
			return;
		}
		// the start: The Town, the difficulty's rule, god mode, the spot and the marker, the phases, the table's file
		const bool bQ2Forced = SpCvar(TEXT("ol.Quest2Profile")) != TEXT("-1");
		const int32 Want = bQ2Forced ? 28 : 32;           // (Quest 2 Mode's two pairs left out when it cannot change)
		const double Off = FVector::Dist2D(GetActorLocation(), S.Spot);
		SpCheck(L->LevelName.Equals(TEXT("townex"), ESearchCase::IgnoreCase) && L->Difficulty == (NewGameDifficulty() >= 0 ? static_cast<EOutlawsDifficulty>(NewGameDifficulty()) : EOutlawsDifficulty::Bad),
			FString::Printf(TEXT("sweep %d: in %s at %s (New Game's last choice %d)"), Sp.Run, *L->LevelName, OutlawsDifficulty::Name(L->Difficulty), NewGameDifficulty()));
		SpCheck(L->bInvincible && Off < 100.0 && !S.Marker.IsZero() && FVector::Dist2D(S.Marker, S.Spot) > 90.0,
			FString::Printf(TEXT("sweep %d: god mode on, at the spot %s (%.0f cm off), the marker %.0f cm ahead"), Sp.Run, *S.Spot.ToCompactString(), Off, FVector::Dist2D(S.Marker, S.Spot)));
		const bool bSkips = bQ2Forced ? S.Skipped.Num() == 1 && S.Skipped[0].StartsWith(TEXT("Quest 2 Mode")) : S.Skipped.Num() == 0;
		SpCheck(S.Phases.Num() == Want && bSkips, FString::Printf(TEXT("sweep %d: %d phases (wanted %d), skipped: %s"), Sp.Run, S.Phases.Num(), Want,
			S.Skipped.Num() ? *FString::Join(S.Skipped, TEXT("; ")) : TEXT("none")));
		FString Stamp = FPaths::GetBaseFilename(CapturePath());
		Stamp.RemoveFromStart(TEXT("capture_"));
		SpCheck(FPaths::FileExists(S.Path) && FPaths::GetCleanFilename(S.Path) == FString::Printf(TEXT("sweep_%s.txt"), *Stamp) && FPaths::FileExists(CapturePath()),
			FString::Printf(TEXT("sweep %d: the table's file %s beside the capture %s"), Sp.Run, *S.Path, *CapturePath()));
		Wait(0.0, S.Phases.Num() * S.PhaseSec + 120.0);
		return;
	}
	case 5:
	{
		// each switch: the rows as the phase says; the phase before it measured
		if (S.Now > Sp.Seen && S.Now >= 1)
		{
			Sp.Seen = S.Now;
			PhaseCheck(S.Now);
			if (Sp.Seen >= 2) SpCheck(Sane(S.Phases[Sp.Seen - 2]), FString::Printf(TEXT("sweep %d phase %d measured: %lld frames, game work %.2f, render %.2f, GPU %.2f, fps %.1f, awake %.0f"), Sp.Run,
				Sp.Seen - 1, S.Phases[Sp.Seen - 2].M.Frames, S.Phases[Sp.Seen - 2].M.GameWorkMs, S.Phases[Sp.Seen - 2].M.RenderMs, S.Phases[Sp.Seen - 2].M.GpuMs, S.Phases[Sp.Seen - 2].M.Fps,
				S.Phases[Sp.Seen - 2].Awake));
			if (Sp.Mode == TEXT("closed") && S.Now >= 2) { Sp.Step = 99; return; }     // (the game closes in the sweep)
			if (Sp.Run == 2 && S.Now >= 3) { Sp.Step = 20; Sp.At = Now + 1.0; return; }
		}
		if (IsRunning()) { Late(TEXT("the sweep's phases")); return; }
		Wait(0.0, 10.0);
		return;
	}
	case 6:
	{
		// the end: every phase measured, the table, the rows back; the top-3 card up, god mode kept until the main menu
		const FSweepPhase& Last = S.Phases.Last();
		SpCheck(Sane(Last), FString::Printf(TEXT("sweep %d phase %d measured: %lld frames, game work %.2f, render %.2f, GPU %.2f, fps %.1f, awake %.0f"), Sp.Run, S.Phases.Num(), Last.M.Frames,
			Last.M.GameWorkMs, Last.M.RenderMs, Last.M.GpuMs, Last.M.Fps, Last.Awake));
		int32 Measured = 0;
		for (const FSweepPhase& Ph : S.Phases) Measured += Sane(Ph) ? 1 : 0;
		SpCheck(LastEndWhy() == TEXT("done") && S.Done == S.Phases.Num() && Measured == S.Phases.Num() && LastEndFrames() > 0,
			FString::Printf(TEXT("sweep %d: ended %s, %d of %d phases done, %d measured sanely, %lld frames captured"), Sp.Run, *LastEndWhy(), S.Done, S.Phases.Num(), Measured, LastEndFrames()));
		FString Table;
		FFileHelper::LoadFileToString(Table, *S.Path);
		int32 PhaseRows = 0;
		TArray<FString> Lines;
		Table.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			FString T = Line.TrimStart();
			int32 i = 0;
			if (LexTryParseString(i, *T.Left(T.Find(TEXT(" ")))) && i >= 1 && i <= S.Phases.Num() && !Line.Contains(TEXT("not measured"))) ++PhaseRows;
		}
		SpCheck(Table.StartsWith(TEXT("Settings sweep, ")) && Table.Contains(TEXT("(done)")) && Table.Contains(TEXT("Each row's saving at its lowest")) && Table.Contains(TEXT("All top against all lowest: game work"))
			&& Table.Contains(TEXT("Resolution's steps")) && Table.Contains(TEXT("The player's settings as the sweep began")) && PhaseRows == S.Phases.Num(),
			FString::Printf(TEXT("sweep %d: the table has its ranking, the all pair, the resolution steps, %d phase rows (of %d) and the player's settings"), Sp.Run, PhaseRows, S.Phases.Num()));
		// (the ranking, the all pair and the resolution's steps into the log, for the report)
		bool bRank = false;
		for (const FString& Line : Lines)
		{
			if (Line.StartsWith(TEXT("Each row's saving"))) bRank = true;
			if (Line.StartsWith(TEXT("Every phase"))) bRank = false;
			if (bRank && !Line.IsEmpty()) UE_LOG(LogOutlaws, Display, TEXT("OLPERFSWEEP table: %s"), *Line);
		}
		Restored(TEXT("the whole sweep"));
		SpCheck(S.Top3.Num() == 3 && L->bInvincible, FString::Printf(TEXT("sweep %d: the top-3 card (%s), god mode kept while it shows"), Sp.Run, *FString::Join(S.Top3, TEXT(" | "))));
		Wait(0.0, 60.0);
		return;
	}
	case 7:
		if (!AtMenu() || IsOn()) { Late(TEXT("the main menu after the sweep")); return; }
		{
			const FString Diff = SpDiff(Sp.SavedBefore, SpSavedMd5());
			SpCheck(L->bInvincible == Sp.bGod && Diff == TEXT("none"), FString::Printf(TEXT("sweep %d: the main menu back, god mode %d as before, Saved md5 identical (Perf and Logs left out); changed: %s"),
				Sp.Run, L->bInvincible ? 1 : 0, *Diff));
		}
		Wait(2.0, 10.0);
		return;
	case 8:
		// the second sweep, quit with End Game in its third phase
		ShowMenuPage(TEXT("#newgame"), true);
		Keep();
		Sp.Run = 2;
		Sp.Seen = 0;
		SpCheck(PressRun(TEXT("Settings sweep"), int32(EOutlawsDifficulty::Good)) && IsSweep(), FString::Printf(TEXT("sweep 2: the row pressed again, Good picked (%s)"), *Rows()));
		Sp.Step = 4;
		Sp.At = Now;
		Sp.Deadline = Now + 240.0;
		return;
	case 20:
	{
		// End Game from the pause menu: no Restart Level, Save or Load there
		OpenMenu();
		bool bSave = false, bLoad = false, bRestart = false;
		int32 End = INDEX_NONE;
		if (MenuPage) for (int32 i = 0; i < MenuPage->Items.Num(); ++i)
		{
			const OutlawsMenu::FItem& It = MenuPage->Items[i];
			bSave |= It.Target.Equals(TEXT("save_game.rml"), ESearchCase::IgnoreCase);
			bLoad |= It.Target.Equals(TEXT("load_game.rml"), ESearchCase::IgnoreCase);
			bRestart |= It.Action == TEXT("ev_restart_level");
			if (It.Action == TEXT("ev_end_game")) End = i;
		}
		SpCheck(bMenuOpen && !bSave && !bLoad && !bRestart && End != INDEX_NONE, FString::Printf(TEXT("sweep 2: the pause page has no Restart Level, Save or Load: %s"), *Rows()));
		if (End != INDEX_NONE) MenuActivate(1, End);
		Wait(0.5, 10.0);
		return;
	}
	case 21:
	{
		const int32 Yes = MenuPage ? MenuPage->Items.IndexOfByPredicate([](const OutlawsMenu::FItem& It) { return It.Action == TEXT("ev_exit_game_confirm"); }) : INDEX_NONE;
		if (Yes != INDEX_NONE) MenuActivate(1, Yes);
		SpCheck(Yes != INDEX_NONE && !IsRunning() && LastEndWhy() == TEXT("quit from the pause menu") && S.Done == 2,
			FString::Printf(TEXT("sweep 2: End Game ended it: %s, %d of %d phases done"), *LastEndWhy(), S.Done, S.Phases.Num()));
		Restored(TEXT("End Game in its third phase"));
		FString Table;
		FFileHelper::LoadFileToString(Table, *S.Path);
		SpCheck(Table.Contains(TEXT("(quit from the pause menu)")) && Table.Contains(TEXT("(not measured)")), FString::Printf(TEXT("sweep 2: its table written as quit (%s)"), *S.Path));
		Wait(1.0, 120.0);
		return;
	}
	case 22:
		if (!AtMenu() || IsOn()) { Late(TEXT("the main menu after End Game")); return; }
		{
			const FString Diff = SpDiff(Sp.SavedBefore, SpSavedMd5());
			SpCheck(L->bInvincible == Sp.bGod && Diff == TEXT("none"), FString::Printf(TEXT("sweep 2: the main menu back, god mode %d as before, Saved md5 identical (Perf and Logs left out); changed: %s"),
				L->bInvincible ? 1 : 0, *Diff));
		}
		Sp.Step = 99;
		return;
	case 50:
		Survey();
		return;
	default:
		Sp.bDone = true;
		if (Sp.Mode == TEXT("spots")) UE_LOG(LogOutlaws, Display, TEXT("OLPERFSWEEP survey done"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT perfsweep: %s (%d passed, %d failed)"), Sp.Fail == 0 && Sp.Pass > 0 ? TEXT("PASS") : TEXT("FAIL"), Sp.Pass, Sp.Fail);
		FPlatformMisc::RequestExit(false);
		return;
	}
}
