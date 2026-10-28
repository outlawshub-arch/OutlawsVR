// REQ-PERFTESTMODE: -OLPerfTestProbe (any build, through OLFlags: nothing runs without the flag), from the main menu as a
// player goes: the title page's turn shows nothing; New Game's page has no Performance test row until a whole turn on the
// spot there (20 degree steps of the view, as the pause test turns for the Cheats row), and the pause Cheats stay hidden; the
// row picked: the run's second level up (OutlawsPerfTest::Level, RandoLevelOrder's second) with the carry counted in, full
// health and stamina, Bad; the capture and the settings dump there with the stamp they share and the values applied; a death
// (its marker) and the respawn kept in the run, the carry given back; the level's exit (CompleteLevel) ending the capture
// with as many rows as the end line's frames and the main menu back; then a second run quit with End Game from the pause
// menu (no Save or Load rows there; with a typed seed, the same roll and bad guys as the first).  Around each run the Saved
// folder's files (but Perf and Logs, and the editor's own folders) are md5-checked unchanged.  Then New Game > Main Game >
// Bad as ever: the story's first level with the start kit and its level-start autosave written again.  -OLPerfTestProbe=closed
// quits in the first run instead (its "game closed" end line is the runner's to find).  "OLPERFTEST check PASS|FAIL: ..." a
// check, then "OLSHOT perftest: PASS|FAIL" and the exit.  See OutlawsPlayer.h (TickPerfTestProbe).

#include "OutlawsPlayer.h"
#include "OutlawsPerfTest.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsLevelActor.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsBuildInfo.h"
#include "OutlawsDeviceProfile.h"
#include "GameFramework/Controller.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

namespace
{
	struct FPerfProbe
	{
		int32 Step = 0;
		double At = 0.0;              // real seconds: the step waits until then
		double Deadline = 0.0;        // a wait past this fails
		int32 Turns = 0;
		int32 Run = 0;                // 1: left by the exit, 2: by End Game
		int32 Pass = 0, Fail = 0;
		FString Expect;               // the run's level as RandoLevelOrder gives it
		uint32 Seed1 = 0, Sum1 = 0;   // the first run's seed and roll, and its bad guys: the second's with the same seed
		int32 Enemies1 = -1;
		TMap<FString, FString> SavedBefore;
		bool bDone = false;
	};
	FPerfProbe GPp;

	void PpCheck(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLPERFTEST check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		(bOk ? GPp.Pass : GPp.Fail)++;
	}

	// the Saved folder's files and their md5: Perf and Logs (the run's own) left out, and the folders the editor and the engine
	// write on their own (Config is checked from outside, before and after the whole run)
	TMap<FString, FString> PpSavedMd5()
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

	FString PpSavedDiff(const TMap<FString, FString>& Was, const TMap<FString, FString>& Now)
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

	TMap<FString, FString> PpReadSettings(const FString& Path)
	{
		TMap<FString, FString> Out;
		TArray<FString> Lines;
		if (FFileHelper::LoadFileToStringArray(Lines, *Path))
		{
			for (const FString& Line : Lines)
			{
				FString K, V;
				if (Line.Split(TEXT(": "), &K, &V)) Out.Add(K, V);
			}
		}
		return Out;
	}

	const TCHAR* PpOnOff(bool bOn) { return bOn ? TEXT("on") : TEXT("off"); }
}

bool AOutlawsPlayer::PerfTestProbeOn()
{
	static const bool bOn = OLFlags::Has(TEXT("OLPerfTestProbe"));
	return bOn;
}

void AOutlawsPlayer::TickPerfTestProbe(float Dt)
{
	FPerfProbe& Pp = GPp;
	AOutlawsLevelActor* L = Level.Get();
	if (Pp.bDone || !L) return;
	const double Now = FPlatformTime::Seconds();
	if (Now < Pp.At) return;
	auto Rows = [this]()
	{
		FString R;
		if (MenuPage) for (const OutlawsMenu::FItem& It : MenuPage->Items) R += (R.IsEmpty() ? TEXT("") : TEXT(" | ")) + It.Label;
		return R;
	};
	auto AtMenu = [this, L]() { return L->IsFrontEnd() && !L->IsRebuildPending() && bMenuOpen && bFrontMenu && MenuPage && MenuPage->File == TEXT("#front"); };
	auto Turn = [this](float Deg) { if (Controller) { FRotator R = Controller->GetControlRotation(); R.Yaw += Deg; Controller->SetControlRotation(R); } };
	auto Wait = [&Pp, Now](double Seconds, double Limit) { Pp.At = Now + Seconds; Pp.Deadline = Now + Limit; ++Pp.Step; };
	auto Late = [&Pp, Now](const TCHAR* What)
	{
		if (Now < Pp.Deadline) return false;
		PpCheck(false, FString::Printf(TEXT("timed out waiting for %s"), What));
		Pp.Step = 99;
		return true;
	};
	// the run's start checks: the level, the carry, the capture and the settings dump (with its values)
	auto RunChecks = [&]()
	{
		const FString Lv = OutlawsPerfTest::Level();
		PpCheck(Lv == Pp.Expect && L->LevelName.Equals(Lv, ESearchCase::IgnoreCase), FString::Printf(TEXT("run %d: the level is the run's second, %s (%s; wanted %s)"), Pp.Run, *L->LevelName,
			*AOutlawsLevelActor::LevelTitle(L->LevelName), *Pp.Expect));
		PpCheck(L->Difficulty == EOutlawsDifficulty::Bad, FString::Printf(TEXT("run %d: difficulty %s (wanted Bad)"), Pp.Run, OutlawsDifficulty::Name(L->Difficulty)));
		FString Bad, Carry;
		for (const TPair<FString, int32>& Kv : OutlawsPerfTest::Carry())
		{
			Carry += FString::Printf(TEXT("%s%s=%d"), Carry.IsEmpty() ? TEXT("") : TEXT(" "), *Kv.Key, Kv.Value);
			if (L->GetCount(Kv.Key) != Kv.Value) Bad += FString::Printf(TEXT(" %s %d (carried %d)"), *Kv.Key, L->GetCount(Kv.Key), Kv.Value);
		}
		PpCheck(Bad.IsEmpty() && OutlawsPerfTest::Carry().Num() > 0, FString::Printf(TEXT("run %d: the carry is in: %s%s"), Pp.Run, *Carry, Bad.IsEmpty() ? TEXT("") : *(TEXT("; differs:") + Bad)));
		PpCheck(L->GetHealth() >= 9.99f && Stamina >= 99.9f, FString::Printf(TEXT("run %d: health %.1f, stamina %.1f (wanted full)"), Pp.Run, L->GetHealth(), Stamina));
		const FString Cap = OutlawsPerfTest::CapturePath(), Set = OutlawsPerfTest::SettingsPath();
		FString Stamp = FPaths::GetBaseFilename(Cap);
		const bool bCapName = Stamp.RemoveFromStart(TEXT("capture_"));
		PpCheck(bCapName && FPaths::FileExists(Cap) && FPaths::IsSamePath(FPaths::GetPath(Cap), FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Perf"))),
			FString::Printf(TEXT("run %d: the capture is up in Saved/Perf: %s"), Pp.Run, *Cap));
		PpCheck(FPaths::FileExists(Set) && FPaths::GetCleanFilename(Set) == FString::Printf(TEXT("perftest_%s_settings.txt"), *Stamp) && FPaths::IsSamePath(FPaths::GetPath(Set), FPaths::GetPath(Cap)),
			FString::Printf(TEXT("run %d: the settings dump beside it with its stamp: %s"), Pp.Run, *Set));
		const TMap<FString, FString> S = PpReadSettings(Set);
		static const TCHAR* const Keys[] = { TEXT("build"), TEXT("started"), TEXT("level"), TEXT("level_title"), TEXT("level_one"), TEXT("difficulty"), TEXT("capture"), TEXT("headset"),
			TEXT("device_model"), TEXT("vr"), TEXT("profile"), TEXT("quest2_mode"), TEXT("resolution_percent"), TEXT("vr_pixel_density"), TEXT("screen_percentage"), TEXT("eye_buffer"),
			TEXT("refresh_rate"), TEXT("refresh_rate_asked"), TEXT("lamp_lights"), TEXT("hd_art"), TEXT("object_detail"), TEXT("stochastic"), TEXT("tinting"), TEXT("brightness"),
			TEXT("perf_hud"), TEXT("grab_glow"), TEXT("movement_speed"), TEXT("bad_guys"), TEXT("randomizer"), TEXT("randomizer_enemies"), TEXT("randomizer_items"), TEXT("randomizer_order"),
			TEXT("randomizer_bosses"), TEXT("randomizer_critters"), TEXT("randomizer_seed"), TEXT("cpu_gpu_level"), TEXT("xr_perf"), TEXT("device_state"), TEXT("battery_level"),
			TEXT("battery_temperature"), TEXT("carried_in"), TEXT("counts_at_start") };
		FString Missing;
		for (const TCHAR* K : Keys) if (!S.Contains(K)) Missing += FString(TEXT(" ")) + K;
		PpCheck(Missing.IsEmpty(), FString::Printf(TEXT("run %d: the dump has its %d keys, one \"key: value\" line each%s"), Pp.Run, int32(UE_ARRAY_COUNT(Keys)), Missing.IsEmpty() ? TEXT("") : *(TEXT("; missing:") + Missing)));
		auto Is = [&S](const TCHAR* K, const FString& Want) { const FString* V = S.Find(K); return V && *V == Want; };
		auto Starts = [&S](const TCHAR* K, const FString& Want) { const FString* V = S.Find(K); return V && V->StartsWith(Want); };
		TArray<FString> Wrong;
		if (!Is(TEXT("build"), OutlawsBuildInfo::Line())) Wrong.Add(TEXT("build"));
		if (!Is(TEXT("level"), L->LevelName)) Wrong.Add(TEXT("level"));
		if (!Is(TEXT("difficulty"), TEXT("Bad"))) Wrong.Add(TEXT("difficulty"));
		if (!Is(TEXT("capture"), Cap)) Wrong.Add(TEXT("capture"));
		if (!Starts(TEXT("resolution_percent"), FString::FromInt(OutlawsDeviceProfile::ResolutionPercent()))) Wrong.Add(TEXT("resolution_percent"));
		if (!Is(TEXT("lamp_lights"), FString::FromInt(OutlawsDeviceProfile::LampLights()))) Wrong.Add(TEXT("lamp_lights"));
		if (!Is(TEXT("hd_art"), PpOnOff(L->bHDTextures))) Wrong.Add(TEXT("hd_art"));
		if (!Is(TEXT("stochastic"), PpOnOff(L->bStochastic))) Wrong.Add(TEXT("stochastic"));
		if (!Is(TEXT("tinting"), PpOnOff(L->bPaletteGrades))) Wrong.Add(TEXT("tinting"));
		if (!Is(TEXT("brightness"), FString::Printf(TEXT("%.2f"), OutlawsDeviceProfile::Gamma()))) Wrong.Add(TEXT("brightness"));
		if (!Is(TEXT("perf_hud"), PpOnOff(OutlawsDeviceProfile::PerfHud()))) Wrong.Add(TEXT("perf_hud"));
		if (!Is(TEXT("bad_guys"), FString::Printf(TEXT("x%.2f"), L->SoloBadGuysWanted()))) Wrong.Add(TEXT("bad_guys"));
		if (!Is(TEXT("randomizer"), PpOnOff(L->RandoOn()))) Wrong.Add(TEXT("randomizer"));
		if (!Starts(TEXT("randomizer_seed"), FString::Printf(TEXT("%u ("), L->RandoRun.Seed))) Wrong.Add(TEXT("randomizer_seed"));
#if !PLATFORM_ANDROID
		if (!Starts(TEXT("battery_level"), TEXT("not available"))) Wrong.Add(TEXT("battery_level"));
#endif
		const FString* Seed = S.Find(TEXT("randomizer_seed"));
		const FString* Res = S.Find(TEXT("resolution_percent"));
		const FString* Pd = S.Find(TEXT("vr_pixel_density"));
		PpCheck(Wrong.Num() == 0, FString::Printf(TEXT("run %d: the dump's values are the ones applied (resolution %s, vr.PixelDensity %s, seed %s)%s"), Pp.Run, Res ? **Res : TEXT("?"),
			Pd ? **Pd : TEXT("?"), Seed ? **Seed : TEXT("?"), Wrong.Num() == 0 ? TEXT("") : *(TEXT("; wrong: ") + FString::Join(Wrong, TEXT(", ")))));
		// the same seed (or the Randomizer off): the same bad guys and items as the first run
		const int32 Enemies = L->GetEnemies().Num();
		if (Pp.Run == 1)
		{
			Pp.Seed1 = L->RandoRun.Seed;
			Pp.Sum1 = L->RandoSum;
			Pp.Enemies1 = Enemies;
		}
		else if (!L->RandoOn() || L->RandoRun.Seed == Pp.Seed1)
		{
			PpCheck(L->RandoSum == Pp.Sum1 && Enemies == Pp.Enemies1, FString::Printf(TEXT("run 2: the same seed, the same level: roll %08x and %d bad guys (run 1: %08x, %d)"), L->RandoSum, Enemies, Pp.Sum1, Pp.Enemies1));
		}
		else
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLPERFTEST run 2: a new random seed (%u, run 1 %u): the levels not compared"), L->RandoRun.Seed, Pp.Seed1);
		}
	};
	auto SavedCheck = [&](const TCHAR* After)
	{
		const TMap<FString, FString> Was = Pp.SavedBefore;
		const TMap<FString, FString> NowMd5 = PpSavedMd5();
		const FString Diff = PpSavedDiff(Was, NowMd5);
		PpCheck(Diff == TEXT("none"), FString::Printf(TEXT("run %d: Saved md5 identical after %s (%d files, Perf and Logs left out); changed: %s"), Pp.Run, After, NowMd5.Num(), *Diff));
	};
	auto CaptureRows = []()
	{
		TArray<FString> Lines;
		FFileHelper::LoadFileToStringArray(Lines, *OutlawsPerfTest::CapturePath());
		int64 N = 0;
		for (const FString& Line : Lines) N += Line.IsEmpty() || Line.StartsWith(TEXT("t_s,")) ? 0 : 1;
		return N;
	};
	auto Press = [&](const TCHAR* Label) { return TutorialTestPressLabel(Label); };
	switch (Pp.Step)
	{
	case 0:
		// the main menu up and settled
		if (!AtMenu()) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLPERFTEST the main menu: %s"), *Rows());
		Pp.Turns = 0;
		Wait(2.0, 30.0);
		return;
	case 1:
		// a whole turn on the title page: no row (the turn counts on New Game's page alone)
		if (Pp.Turns < 20) { Turn(20.f); ++Pp.Turns; Pp.At = Now + 0.05; return; }
		if (Pp.Turns == 20) { ++Pp.Turns; Pp.At = Now + 0.5; return; }      // (the view's last step in before the page changes)
		PpCheck(!bPerfTestShown && !bCheatsShown, FString::Printf(TEXT("a whole turn on the title page shows nothing (performance test %d, cheats %d)"), bPerfTestShown ? 1 : 0, bCheatsShown ? 1 : 0));
		ShowMenuPage(TEXT("#newgame"), true);
		Wait(0.5, 10.0);
		return;
	case 2:
	{
		const FString R = Rows();
		PpCheck(MenuPage && MenuPage->File == TEXT("#newgame") && R == TEXT("Main Game | Historical Missions | Multiplayer | Tutorial"),
			FString::Printf(TEXT("New Game's page before the turn: %s (no Performance test row)"), *R));
		Pp.Turns = 0;
		Wait(0.2, 10.0);
		return;
	}
	case 3:
		// the turn on the spot, 20 degrees a step, until the row shows (40 steps at most)
		if (!bPerfTestShown && Pp.Turns < 40) { Turn(20.f); ++Pp.Turns; Pp.At = Now + 0.05; return; }
		// (17: on the desktop the view's yaw overshoots a 20 degree step for a frame as the body catches up with the controller,
		// so the count can reach the whole turn one step early; the Cheats reveal counts the same way)
		PpCheck(bPerfTestShown && Pp.Turns >= 17 && Pp.Turns <= 20, FString::Printf(TEXT("the whole turn with New Game's page up shows the row (%d steps of 20 degrees)"), Pp.Turns));
		PpCheck(!bCheatsShown, TEXT("the pause menu's Cheats stay hidden"));
		if (Controller) Controller->SetControlRotation(FRotator(0.0, MenuYaw, 0.0));
		Wait(0.5, 10.0);
		return;
	case 4:
	{
		const FString R = Rows();
		PpCheck(R == TEXT("Main Game | Historical Missions | Multiplayer | Tutorial | Performance test"), FString::Printf(TEXT("New Game's page after the turn: %s"), *R));
		Pp.SavedBefore = PpSavedMd5();
		Pp.Run = 1;
		const bool bPressed = Press(TEXT("Performance test"));
		TArray<FString> Order;
		L->RandoLevelOrder(Order);
		Pp.Expect = Order.IsValidIndex(1) ? Order[1] : FString();
		PpCheck(bPressed && OutlawsPerfTest::IsOn(), FString::Printf(TEXT("run 1: Performance test pressed (the run's order %s; %d files in Saved to keep)"), *FString::Join(Order, TEXT(" ")), Pp.SavedBefore.Num()));
		Wait(0.0, 240.0);
		return;
	}
	case 5:
	{
		if (!OutlawsPerfTest::IsRunning()) { Late(TEXT("the run's start")); return; }
		L->bInvincible = true;                // (no stray death before the probe's own)
		RunChecks();
		// (-OLPerfTestProbe=closed: the game quits here, in the run: its end line says so, read from the log by the runner)
		FString Mode;
		if (OLFlags::Value(TEXT("OLPerfTestProbe"), Mode) && Mode == TEXT("closed")) { Pp.Step = 99; Pp.At = Now + 3.0; return; }
		Wait(3.0, 10.0);
		return;
	}
	case 6:
		// a death: its marker, no save of the player's offered; the run goes on
		L->bInvincible = false;
		L->DamagePlayer(1000.f, 1.f, true);
		PpCheck(L->IsPlayerDead() && !bDeathAutosave && OutlawsPerfTest::Deaths() == 1 && OutlawsPerfTest::IsRunning(),
			FString::Printf(TEXT("run 1: a death counted (%d), the run going on, no save offered (%d)"), OutlawsPerfTest::Deaths(), bDeathAutosave ? 1 : 0));
		Wait(2.0, 10.0);
		return;
	case 7:
		RequestRespawn(false);
		Wait(1.0, 180.0);
		return;
	case 8:
	{
		if (L->IsRebuildPending() || L->IsPlayerDead() || IsDeadView() || !HasActorBegunPlay()) { Late(TEXT("the respawn")); return; }
		L->bInvincible = true;
		FString Bad;
		for (const TPair<FString, int32>& Kv : OutlawsPerfTest::Carry()) if (L->GetCount(Kv.Key) != Kv.Value) Bad += FString::Printf(TEXT(" %s %d (carried %d)"), *Kv.Key, L->GetCount(Kv.Key), Kv.Value);
		PpCheck(OutlawsPerfTest::IsRunning() && L->LevelName.Equals(Pp.Expect, ESearchCase::IgnoreCase) && OutlawsPerfTest::Deaths() == 1 && Bad.IsEmpty(),
			FString::Printf(TEXT("run 1: respawned in %s, still the run (deaths %d), the carry given back%s"), *L->LevelName, OutlawsPerfTest::Deaths(), Bad.IsEmpty() ? TEXT("") : *(TEXT("; differs:") + Bad)));
		Wait(3.0, 10.0);
		return;
	}
	case 9:
		// the level's exit
		L->CompleteLevel(TEXT("the performance test probe's exit"));
		Wait(0.5, 10.0);
		return;
	case 10:
	{
		const int64 Rows0 = CaptureRows();
		PpCheck(!OutlawsPerfTest::IsRunning() && OutlawsPerfTest::LastEndWhy() == TEXT("level exit") && OutlawsPerfTest::LastEndFrames() > 0 && Rows0 == OutlawsPerfTest::LastEndFrames(),
			FString::Printf(TEXT("run 1: the exit ended it: %s, %lld frames, %lld rows in the capture"), *OutlawsPerfTest::LastEndWhy(), OutlawsPerfTest::LastEndFrames(), Rows0));
		Wait(1.0, 120.0);
		return;
	}
	case 11:
		if (!AtMenu() || OutlawsPerfTest::IsOn()) { Late(TEXT("the main menu after the exit")); return; }
		PpCheck(true, TEXT("run 1: the main menu back after the exit (no level after it)"));
		SavedCheck(TEXT("the run and its exit"));
		Wait(2.0, 10.0);
		return;
	case 12:
	{
		// the second run: the row still there, picked again
		ShowMenuPage(TEXT("#newgame"), true);
		const bool bRow = Rows().EndsWith(TEXT("| Performance test"));
		Pp.SavedBefore = PpSavedMd5();
		Pp.Run = 2;
		PpCheck(bRow && Press(TEXT("Performance test")), FString::Printf(TEXT("run 2: the row stays shown and is pressed (%s)"), *Rows()));
		Wait(0.0, 240.0);
		return;
	}
	case 13:
		if (!OutlawsPerfTest::IsRunning()) { Late(TEXT("the second run's start")); return; }
		L->bInvincible = true;
		RunChecks();
		Wait(3.0, 10.0);
		return;
	case 14:
	{
		// End Game from the pause menu (its confirmation): no Save or Load rows there
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
		PpCheck(bMenuOpen && !bSave && !bLoad && bRestart && End != INDEX_NONE, FString::Printf(TEXT("run 2: the pause page has no Save or Load (Restart Level and End Game kept): %s"), *Rows()));
		if (End != INDEX_NONE) MenuActivate(1, End);
		Wait(0.5, 10.0);
		return;
	}
	case 15:
	{
		int32 Yes = INDEX_NONE;
		if (MenuPage) Yes = MenuPage->Items.IndexOfByPredicate([](const OutlawsMenu::FItem& It) { return It.Action == TEXT("ev_exit_game_confirm"); });
		PpCheck(Yes != INDEX_NONE, FString::Printf(TEXT("run 2: End Game asks first (%s: %s)"), MenuPage ? *MenuPage->File : TEXT("none"), *Rows()));
		if (Yes != INDEX_NONE) MenuActivate(1, Yes);
		Wait(0.5, 10.0);
		return;
	}
	case 16:
		PpCheck(!OutlawsPerfTest::IsRunning() && OutlawsPerfTest::LastEndWhy() == TEXT("quit from the pause menu") && OutlawsPerfTest::LastEndFrames() > 0 && CaptureRows() == OutlawsPerfTest::LastEndFrames(),
			FString::Printf(TEXT("run 2: End Game ended it: %s, %lld frames, %lld rows in the capture"), *OutlawsPerfTest::LastEndWhy(), OutlawsPerfTest::LastEndFrames(), CaptureRows()));
		Wait(1.0, 120.0);
		return;
	case 17:
		if (!AtMenu() || OutlawsPerfTest::IsOn()) { Late(TEXT("the main menu after End Game")); return; }
		PpCheck(true, TEXT("run 2: the main menu back after End Game"));
		SavedCheck(TEXT("the run and End Game"));
		Wait(1.0, 10.0);
		return;
	case 18:
	{
		// after the runs, New Game > Main Game > Bad as ever: the story's first level with the start kit, and its level-start
		// autosave written again (the outside md5 check puts the Saved folder back after the probe)
		ShowMenuPage(TEXT("#newgame"), true);
		const bool bMain = Press(TEXT("Main Game"));
		const bool bBad = bMain && MenuPage && MenuPage->File == TEXT("#difficulty:story") && Press(TEXT("Bad"));
		Pp.SavedBefore.Add(TEXT("#autosave"), FString::Printf(TEXT("%lld"), IFileManager::Get().GetTimeStamp(*AOutlawsLevelActor::SavePath(-1)).GetTicks()));
		PpCheck(bBad && !OutlawsPerfTest::IsOn(), TEXT("New Game > Main Game > Bad pressed: the story starts, no performance test run"));
		Wait(1.0, 180.0);
		return;
	}
	case 19:
	{
		if (L->IsMoviePlaying()) { L->SkipMovie(); Pp.At = Now + 0.5; return; }
		TArray<FString> Order;
		L->RandoLevelOrder(Order);
		Pp.Expect = Order.IsValidIndex(0) ? Order[0] : FString(TEXT("hideout"));
		if (L->IsFrontEnd() || L->IsRebuildPending() || !L->LevelName.Equals(Pp.Expect, ESearchCase::IgnoreCase) || L->GetEnemies().Num() == 0) { Late(TEXT("the story's first level")); return; }
		const bool bKit = Pp.Expect == TEXT("ranch") || (L->GetCount(TEXT("ibullit")) == 24 && L->GetCount(TEXT("icart")) == 16);
		PpCheck(!OutlawsPerfTest::IsOn() && bKit && L->Difficulty == EOutlawsDifficulty::Bad,
			FString::Printf(TEXT("the story's start unchanged: %s at %s with the start kit (%s)"), *L->LevelName, OutlawsDifficulty::Name(L->Difficulty), *L->CarrySignature()));
		Wait(1.0, 40.0);
		return;
	}
	case 20:
	{
		const FString Was = Pp.SavedBefore.FindRef(TEXT("#autosave"));
		const FString NowStamp = FString::Printf(TEXT("%lld"), IFileManager::Get().GetTimeStamp(*AOutlawsLevelActor::SavePath(-1)).GetTicks());
		if (NowStamp == Was) { Late(TEXT("the story's level-start autosave")); return; }
		PpCheck(AOutlawsLevelActor::ReadSaveInfo(-1).Level.Equals(Pp.Expect, ESearchCase::IgnoreCase), TEXT("the story's level-start autosave is written again (the saves as ever after a run)"));
		Pp.Step = 99;
		return;
	}
	default:
		Pp.bDone = true;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT perftest: %s (%d passed, %d failed)"), Pp.Fail == 0 && Pp.Pass > 0 ? TEXT("PASS") : TEXT("FAIL"), Pp.Pass, Pp.Fail);
		FPlatformMisc::RequestExit(false);
		return;
	}
}
