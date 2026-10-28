// REQ-PERFTESTMODE: New Game > Performance test (see OutlawsPerfTest.h): the run's state and its log lines, the start at the
// story's second level with what the first level's finish carries in, the settings dump beside the capture, the battery, and
// the run's end.
#include "OutlawsPerfTest.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsPerf.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsFormats.h"
#include "OutlawsLevel.h"
#include "OutlawsLab.h"
#include "OutlawsCoop.h"
#include "OutlawsDeathmatch.h"
#include "OutlawsBuildInfo.h"
#include "OutlawsDeviceProfile.h"
#include "OutlawsDynRes.h"
#include "OutlawsXRRefresh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#if PLATFORM_ANDROID
#include "Android/AndroidJavaEnv.h"
#endif

namespace
{
	enum class EPerfTestState : uint8 { Off, Starting, Running, Leaving };
	EPerfTestState GPerfState = EPerfTestState::Off;
	FString GPerfLevel, GPerfCapture, GPerfSettings, GPerfEndWhy;
	TMap<FString, int32> GPerfCarry;
	int32 GPerfDeaths = 0;
	int64 GPerfEndFrames = 0;
	int32 GPerfNewGameDiff = -1;                   // the difficulty last pressed on New Game this session (-1 none)
	bool GPerfSweep = false;                       // (REQ-PERFSWEEP) the run is a Settings sweep
	bool GPerfPicked = false;                      // (REQ-PERFDIFF) its difficulty picked on its own page, until the run is over
	// (REQ-PERFCARD) the ended run's results card, until the main menu
	FString GPerfCardLevel;
	TArray<TPair<FString, FString>> GPerfCardRows;
	OutlawsPerfTest::FCardNumbers GPerfCardShown;
	bool GPerfCardAlone = false;
	FDelegateHandle GPerfExitHandle, GPerfTerminateHandle;

	// (the game closed in the run: the engine's exit; on the Quest the app's end, which may come without it)
	void PerfTestClosing() { OutlawsPerfTest::End(TEXT("game closed")); }

	/** The battery's level (percent) and temperature (degrees C) where the platform gives them; OutWhy says why not. */
	bool PerfTestBattery(int32& OutLevel, float& OutTempC, FString& OutWhy)
	{
		OutLevel = -1;
		OutTempC = -1000.f;
#if PLATFORM_ANDROID
		// the sticky ACTION_BATTERY_CHANGED intent: registerReceiver(null, filter) hands back the last one, no receiver kept
		JNIEnv* Env = AndroidJavaEnv::GetJavaEnv();
		jobject Activity = AndroidJavaEnv::GetGameActivityThis();
		if (!Env || !Activity)
		{
			OutWhy = TEXT("no Java environment");
			return false;
		}
		bool bOk = false;
		jclass FilterCls = Env->FindClass("android/content/IntentFilter");
		jclass IntentCls = Env->FindClass("android/content/Intent");
		jclass ActivityCls = Env->GetObjectClass(Activity);
		jmethodID FilterNew = FilterCls ? Env->GetMethodID(FilterCls, "<init>", "(Ljava/lang/String;)V") : nullptr;
		jmethodID Register = ActivityCls ? Env->GetMethodID(ActivityCls, "registerReceiver", "(Landroid/content/BroadcastReceiver;Landroid/content/IntentFilter;)Landroid/content/Intent;") : nullptr;
		jmethodID GetInt = IntentCls ? Env->GetMethodID(IntentCls, "getIntExtra", "(Ljava/lang/String;I)I") : nullptr;
		if (Env->ExceptionCheck()) { Env->ExceptionClear(); FilterNew = Register = GetInt = nullptr; }
		if (FilterNew && Register && GetInt)
		{
			jstring Action = Env->NewStringUTF("android.intent.action.BATTERY_CHANGED");
			jobject Filter = Env->NewObject(FilterCls, FilterNew, Action);
			jobject Sticky = Filter ? Env->CallObjectMethod(Activity, Register, (jobject)nullptr, Filter) : nullptr;
			if (Env->ExceptionCheck()) { Env->ExceptionClear(); Sticky = nullptr; }
			if (Sticky)
			{
				auto Extra = [Env, Sticky, GetInt](const char* Key) -> int32
				{
					jstring K = Env->NewStringUTF(Key);
					const jint V = Env->CallIntMethod(Sticky, GetInt, K, jint(-1));
					if (Env->ExceptionCheck()) Env->ExceptionClear();
					Env->DeleteLocalRef(K);
					return int32(V);
				};
				const int32 Level = Extra("level"), Scale = Extra("scale"), Temp = Extra("temperature");
				if (Level >= 0 && Scale > 0) { OutLevel = Level * 100 / Scale; bOk = true; }
				if (Temp != -1) OutTempC = float(Temp) / 10.f;          // (tenths of a degree)
				Env->DeleteLocalRef(Sticky);
			}
			else
			{
				OutWhy = TEXT("no battery intent");
			}
			if (Filter) Env->DeleteLocalRef(Filter);
			Env->DeleteLocalRef(Action);
		}
		else
		{
			OutWhy = TEXT("the Java calls were not found");
		}
		if (FilterCls) Env->DeleteLocalRef(FilterCls);
		if (IntentCls) Env->DeleteLocalRef(IntentCls);
		if (ActivityCls) Env->DeleteLocalRef(ActivityCls);
		return bOk;
#else
		OutWhy = TEXT("desktop");
		return false;
#endif
	}

	// the Randomizer page's names for its choices
	const TCHAR* const PerfRandoModes[3][3] = { { TEXT("Off"), TEXT("Same strength"), TEXT("Anything") }, { TEXT("Off"), TEXT("Shuffle places"), TEXT("Re-roll") },
		{ TEXT("Off"), TEXT("Shuffle middle"), TEXT("Shuffle all") } };
}

// ---- the run's state ----------------------------------------------------------------------------------------------------------

bool OutlawsPerfTest::IsOn() { return GPerfState != EPerfTestState::Off; }
bool OutlawsPerfTest::IsRunning() { return GPerfState == EPerfTestState::Running; }
bool OutlawsPerfTest::IsStarting() { return GPerfState == EPerfTestState::Starting; }
const FString& OutlawsPerfTest::Level() { return GPerfLevel; }
const TMap<FString, int32>& OutlawsPerfTest::Carry() { return GPerfCarry; }
const FString& OutlawsPerfTest::CapturePath() { return GPerfCapture; }
const FString& OutlawsPerfTest::SettingsPath() { return GPerfSettings; }
int32 OutlawsPerfTest::Deaths() { return GPerfDeaths; }
const FString& OutlawsPerfTest::LastEndWhy() { return GPerfEndWhy; }
int64 OutlawsPerfTest::LastEndFrames() { return GPerfEndFrames; }

void OutlawsPerfTest::Begin(const FString& InLevel, const TMap<FString, int32>& InCarry, bool bInSweep)
{
	GPerfState = EPerfTestState::Starting;
	GPerfSweep = bInSweep;
	GPerfLevel = InLevel.ToLower();
	GPerfCarry = InCarry;
	GPerfDeaths = 0;
	GPerfCapture.Reset();
	GPerfSettings.Reset();
	GPerfEndWhy.Reset();
	GPerfEndFrames = 0;
	if (!GPerfExitHandle.IsValid()) GPerfExitHandle = FCoreDelegates::OnEnginePreExit.AddStatic(&PerfTestClosing);
	if (!GPerfTerminateHandle.IsValid()) GPerfTerminateHandle = FCoreDelegates::ApplicationWillTerminateDelegate.AddStatic(&PerfTestClosing);
}

void OutlawsPerfTest::Started(const FString& InCapture, const FString& InSettings)
{
	if (GPerfState != EPerfTestState::Starting) return;
	GPerfState = EPerfTestState::Running;
	GPerfCapture = InCapture;
	GPerfSettings = InSettings;
	if (GPerfSweep) return;                         // (REQ-PERFSWEEP: the sweep's own start line, once the player is at its spot)
	UE_LOG(LogOutlaws, Log, TEXT("OLPERF test start: level %s, capture %s, settings %s"), *GPerfLevel, *GPerfCapture, *GPerfSettings);
}

void OutlawsPerfTest::End(const TCHAR* Why)
{
	if (GPerfState != EPerfTestState::Running) return;
	GPerfState = EPerfTestState::Leaving;           // (nothing written still, until the main menu)
	// (REQ-PERFSWEEP) a sweep's rows put back and its table written first, with the capture still on; its own end line after
	if (GPerfSweep) SweepFinish(Why);
	GPerfEndFrames = OutlawsPerf::EndTestCapture();
	GPerfEndWhy = Why;
	if (GPerfSweep)
	{
		const FSweep& S = Sweep();
		UE_LOG(LogOutlaws, Log, TEXT("OLPERF sweep end: %s (%d of %d), sweep %s"), Why, S.Done, S.Phases.Num(), *S.Path);
		return;
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLPERF test end: %s (%lld frames)"), Why, GPerfEndFrames);
	// (REQ-PERFCARD) the results card from the capture just closed, its settings from the run's dump; none as the game closes
	GPerfCardRows.Reset();
	GPerfCardLevel.Reset();
	if (FCString::Strcmp(Why, TEXT("game closed")) == 0) return;
	TMap<FString, FString> Settings;
	TArray<FString> Lines;
	FFileHelper::LoadFileToStringArray(Lines, *GPerfSettings);
	for (const FString& Line : Lines)
	{
		FString K, V;
		if (Line.Split(TEXT(": "), &K, &V)) Settings.Add(K, V);
	}
	const double Hz = FCString::Atod(*Settings.FindRef(TEXT("refresh_rate")));       // ("72 Hz", or "not available": 0)
	if (CardNumbers(GPerfCapture, Hz, GPerfCardShown))
	{
		GPerfCardRows = CardRowsFor(GPerfCardShown, Settings);
		GPerfCardLevel = GPerfLevel;
		FString Said;
		for (const TPair<FString, FString>& R : GPerfCardRows) Said += FString::Printf(TEXT("%s%s %s"), Said.IsEmpty() ? TEXT("") : TEXT(" | "), *R.Key, *R.Value);
		UE_LOG(LogOutlaws, Log, TEXT("OLPERF test card (%d live frames): %s"), GPerfCardShown.Frames, *Said);
	}
}

bool OutlawsPerfTest::CardNumbers(const FString& Capture, double Hz, FCardNumbers& Out)
{
	Out = FCardNumbers();
	Out.Hz = Hz;
	TArray<FString> Lines;
	if (Capture.IsEmpty() || !FFileHelper::LoadFileToStringArray(Lines, *Capture)) return false;
	int32 CF = -1, CG = -1, CR = -1, CGpu = -1, CPlay = -1, CWait = -1;
	TArray<double> Frames;
	double SumG = 0.0, SumR = 0.0, SumGpu = 0.0;
	TArray<FString> T;
	for (const FString& Line : Lines)
	{
		if (Line.StartsWith(TEXT("t_s,")))
		{
			// (the header: at the start and again at the end, a bucket's column perhaps added between)
			TArray<FString> Head;
			Line.ParseIntoArray(Head, TEXT(","), false);
			CF = Head.IndexOfByKey(TEXT("frame_ms")); CG = Head.IndexOfByKey(TEXT("game_ms")); CR = Head.IndexOfByKey(TEXT("render_ms"));
			CGpu = Head.IndexOfByKey(TEXT("gpu_ms")); CPlay = Head.IndexOfByKey(TEXT("in_play")); CWait = Head.IndexOfByKey(TEXT("xr_wait_ms"));
			continue;
		}
		if (Line.IsEmpty() || CF < 0 || CPlay < 0) continue;
		Line.ParseIntoArray(T, TEXT(","), false);
		if (!T.IsValidIndex(CPlay) || T[CPlay] != TEXT("1") || !T.IsValidIndex(CF)) continue;
		const double F = FCString::Atod(*T[CF]);
		Frames.Add(F);
		const double Wait = T.IsValidIndex(CWait) ? FCString::Atod(*T[CWait]) : 0.0;
		SumG += T.IsValidIndex(CG) ? FMath::Max(0.0, FCString::Atod(*T[CG]) - Wait) : 0.0;
		SumR += T.IsValidIndex(CR) ? FCString::Atod(*T[CR]) : 0.0;
		SumGpu += T.IsValidIndex(CGpu) ? FCString::Atod(*T[CGpu]) : 0.0;
		Out.Stutters += F > 30.0 ? 1 : 0;
	}
	Out.Frames = Frames.Num();
	if (Frames.Num() == 0) return false;
	double Sum = 0.0;
	for (const double F : Frames) Sum += F;
	const double Budget = 1000.0 / (Hz > 0.0 ? Hz : 72.0);      // (no rate known: the Quest's lowest)
	int32 OnTime = 0;
	for (const double F : Frames) OnTime += F <= Budget * 1.05 ? 1 : 0;
	const double N = double(Frames.Num());
	Out.Fps = Sum > 0.0 ? 1000.0 * N / Sum : 0.0;
	Out.OnTimePct = 100.0 * OnTime / N;
	TArray<double> Slow = Frames;
	Slow.Sort([](double A, double B) { return A > B; });
	Out.WorstMs = Slow[0];
	const int32 N1 = FMath::Max(1, Frames.Num() / 100);
	double S1 = 0.0;
	for (int32 i = 0; i < N1; ++i) S1 += Slow[i];
	Out.LowFps = S1 > 0.0 ? 1000.0 * N1 / S1 : 0.0;
	Out.GameMs = SumG / N;
	Out.RenderMs = SumR / N;
	Out.GpuMs = SumGpu / N;
	Out.Limit = Out.GpuMs >= Out.GameMs && Out.GpuMs >= Out.RenderMs ? TEXT("the GPU") : Out.RenderMs >= Out.GameMs ? TEXT("rendering") : TEXT("the game");
	return true;
}

TArray<TPair<FString, FString>> OutlawsPerfTest::CardRowsFor(const FCardNumbers& N, const TMap<FString, FString>& S)
{
	// short rows in plain words (user 28 Oct), then the run's settings a row each, as the dump has them
	auto Get = [&S](const TCHAR* K) { FString V = S.FindRef(K).TrimStartAndEnd(); int32 At = V.Find(TEXT(" (")); return At > 0 ? V.Left(At) : V; };
	TArray<TPair<FString, FString>> R;
	R.Emplace(TEXT("Frame rate"), N.Hz > 0.0 ? FString::Printf(TEXT("%.0f fps of %.0f Hz"), N.Fps, N.Hz) : FString::Printf(TEXT("%.0f fps"), N.Fps));
	R.Emplace(TEXT("Frames on time"), FString::Printf(TEXT("%.0f %%"), N.OnTimePct));
	R.Emplace(TEXT("Worst 1 %"), FString::Printf(TEXT("%.0f fps"), N.LowFps));
	R.Emplace(TEXT("Worst frame"), FString::Printf(TEXT("%.0f ms"), N.WorstMs));
	R.Emplace(TEXT("Game, render, GPU"), FString::Printf(TEXT("%.1f, %.1f, %.1f ms"), N.GameMs, N.RenderMs, N.GpuMs));
	R.Emplace(TEXT("Held back by"), N.Limit);
	R.Emplace(TEXT("Stutters over 30 ms"), FString::FromInt(N.Stutters));
	const FString HzText = Get(TEXT("refresh_rate"));
	R.Emplace(TEXT("Resolution"), FString::Printf(TEXT("%s %%%s"), *Get(TEXT("resolution_percent")), HzText.EndsWith(TEXT("Hz")) ? *(TEXT(", ") + HzText) : TEXT("")));
	R.Emplace(TEXT("Lamps"), Get(TEXT("lamp_lights")));
	R.Emplace(TEXT("Remastered"), Get(TEXT("hd_art")));
	R.Emplace(TEXT("Object Detail"), Get(TEXT("object_detail")));
	R.Emplace(TEXT("Quest 2 Mode"), Get(TEXT("quest2_mode")));
	R.Emplace(TEXT("Difficulty"), Get(TEXT("difficulty")));
	R.Emplace(TEXT("Bad guys"), Get(TEXT("bad_guys")));
	return R;
}

const FString& OutlawsPerfTest::CardLevel() { return GPerfCardLevel; }
const TArray<TPair<FString, FString>>& OutlawsPerfTest::CardRows() { return GPerfCardRows; }
const OutlawsPerfTest::FCardNumbers& OutlawsPerfTest::CardShown() { return GPerfCardShown; }
bool OutlawsPerfTest::CardAlone() { return GPerfCardAlone; }
void OutlawsPerfTest::SetCardAlone(bool bAlone) { GPerfCardAlone = bAlone; }

bool OutlawsPerfTest::IsSweep() { return GPerfSweep && GPerfState != EPerfTestState::Off; }

void OutlawsPerfTest::NoteDeath()
{
	if (GPerfState != EPerfTestState::Running) return;
	++GPerfDeaths;
	UE_LOG(LogOutlaws, Log, TEXT("OLPERF test death %d"), GPerfDeaths);
}

void OutlawsPerfTest::NoteNewGameDifficulty(int32 InDifficulty)
{
	GPerfNewGameDiff = FMath::Clamp(InDifficulty, 0, 2);
}

int32 OutlawsPerfTest::NewGameDifficulty() { return GPerfNewGameDiff; }

void OutlawsPerfTest::NotePicked() { GPerfPicked = true; }

FString OutlawsPerfTest::DifficultyFrom()
{
	if (GPerfPicked) return TEXT("picked");
	return GPerfNewGameDiff >= 0 ? TEXT("the last New Game choice") : TEXT("none chosen this session (Bad)");
}

void OutlawsPerfTest::Finished()
{
	if (GPerfState == EPerfTestState::Off) return;
	if (GPerfSweep) SweepOver();                    // (REQ-PERFSWEEP: god mode back, the marker gone)
	GPerfState = EPerfTestState::Off;
	GPerfSweep = false;
	GPerfPicked = false;
	GPerfCardRows.Reset();                          // (REQ-PERFCARD: shown, the main menu up)
	GPerfCardLevel.Reset();
	GPerfCardAlone = false;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT performance test over: the main menu again (saves, achievements, best times and the settings file as ever from here)"));
}

bool AOutlawsPlayer::PerfTestOffered() const
{
	return !UOutlawsCoop::IsActive(this) && !FOutlawsMpCharacter::bMatchMenu;      // (single player only: never a session or a match)
}

// ---- the level actor's side -------------------------------------------------------------------------------------------------

void AOutlawsLevelActor::StartPerfTest(bool bSweep)
{
	// a story start at the run's second level, as StartStory starts the first: the page's Randomizer and seed, the Bad guys
	// multiplier and the graphics as they are now; the difficulty last pressed on New Game this session (user 28 Oct, via the
	// planner: the player's own), else Bad, the original's normal
	const int32 Chosen = OutlawsPerfTest::NewGameDifficulty();
	Difficulty = Chosen >= 0 ? static_cast<EOutlawsDifficulty>(Chosen) : EOutlawsDifficulty::Bad;
	bHom = false;
	bTutorial = false;
	TutorialRun.Reset();
	bFrontEnd = false;
	bMovieToMenu = false;
	PlayTime = 0.0;
	Inventory.Reset();
	InventoryLabels.Reset();
	BeginRandoRun(bSweep ? TEXT("settings sweep") : TEXT("performance test"));
	TArray<FString> Order;
	RandoLevelOrder(Order);
	FString One = Order.IsValidIndex(0) ? Order[0] : FString(TEXT("hideout"));
	FString Two = Order.IsValidIndex(1) ? Order[1] : FString(TEXT("townex"));
	if (bSweep)
	{
		// (REQ-PERFSWEEP) always The Town, its spot's level, with what the level before it in the run's order carries in (the
		// hideout's when the shuffled order starts there)
		const int32 At = Order.IndexOfByKey(TEXT("townex"));
		One = At > 0 ? Order[At - 1] : FString(TEXT("hideout"));
		Two = TEXT("townex");
	}
	PerfTestCarry(One);
	OutlawsPerfTest::Begin(Two, Counts, bSweep);
	if (bSweep)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT settings sweep: %s (%s) (the level before it %s), difficulty %s, carried in: %s; nothing saved, the settings file neither, until the main menu"),
			*Two, *LevelTitle(Two), *One, OutlawsDifficulty::Name(Difficulty), *CarrySignature());
	}
	else
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT performance test: %s (%s), the run's level 2 (level 1 %s), difficulty %s, carried in: %s; nothing saved until the main menu"),
			*Two, *LevelTitle(Two), *One, OutlawsDifficulty::Name(Difficulty), *CarrySignature());
	}
	NextLevelName = Two;
	NextMovie.Reset();
	EntryLevel.Reset();
	StopMusic();
	bMenuMusic = false;
	StartNextLevelThen(nullptr);           // (as a won level goes on: the carry, health and stamina full; a map load's way too)
}

void AOutlawsLevelActor::PerfTestCarry(const FString& LevelOne)
{
	// what a finish of the first level carries in: James's start kit (as that level gives it: Bob's ranch none), every gun and
	// ammunition pickup the level places for this difficulty and what its own bad guys drop (the shotgun the hideout's give:
	// a finish shoots them), after this run's roll of it (its first, attempt 0); nothing fired, capped as a pickup is
	// (AddCount); the Bad guys multiplier's copies left out
	LoadGameTables();
	LevelName = LevelOne;
	ApplyStartKit();
	FString Lvt, Obt, InfText;
	if (!AssetsRef || !AssetsRef->GetText(LevelOne + TEXT(".lvt"), Lvt))
	{
		UE_LOG(LogOutlaws, Warning, TEXT("OLRT performance test: %s.lvt not found: the start kit alone carried in"), *LevelOne);
		return;
	}
	AssetsRef->GetText(LevelOne + TEXT(".obt"), Obt);
	AssetsRef->GetText(LevelOne + TEXT(".inf"), InfText);
	FOutlawsLevel Lx;
	Lx.ParseLvt(Lvt);
	Lx.ParseObt(Obt);
	const uint32 DiffBit = OutlawsDifficulty::ObjectBit(Difficulty);
	FRandoRoll Roll;
	if (RandoOn() && (RandoRun.Enemies > 0 || RandoRun.Items > 0)) RollRando(Lx, LevelOne.ToLower(), InfText, DiffBit, RandoRun, 0, Roll);
	// a ground item's guns and ammunition, its PICKUP entries as RegisterPickup reads them (a stick of dynamite lying about:
	// its one PICKUP): how many it gives, -1 when it is no ground item
	auto Ground = [this](const FString& Func, const TMap<FString, FString>& Data) -> int32
	{
		const bool bStick = Func.Equals(TEXT("shot_ProjectileDispatch"), ESearchCase::IgnoreCase);
		if (!bStick && !Func.Equals(TEXT("Inv_GroundObject"), ESearchCase::IgnoreCase)) return -1;
		int32 Given = 0;
		const int32 N = !bStick && Data.Contains(TEXT("PICKUP_COUNT")) ? FMath::Max(1, FCString::Atoi(*Data[TEXT("PICKUP_COUNT")])) : 1;
		for (int32 e = 0; e < N; ++e)
		{
			const FString Sfx = e == 0 ? FString() : FString::FromInt(e);
			FString Item = FPaths::GetBaseFilename(Data.FindRef(TEXT("PICKUP") + Sfx)).ToLower();
			if (Item.IsEmpty() && bStick) Item = TEXT("idynam");
			if (Item.IsEmpty()) continue;
			FString Class = Data.FindRef(TEXT("ITEM_CLASS") + Sfx).ToLower();
			if (Class.IsEmpty())
			{
				if (const FItemDef* D = ItemDefs.Find(Item)) Class = D->Class.ToLower();
			}
			if (Class != TEXT("weap") && Class != TEXT("ammo")) continue;
			AddCount(Item, Data.Contains(TEXT("COUNT") + Sfx) ? FCString::Atoi(*Data[TEXT("COUNT") + Sfx]) : 1);
			++Given;
		}
		return Given;
	};
	int32 Placed = 0, Drops = 0;
	for (int32 i = 0; i < Lx.Objects.Num(); ++i)
	{
		const FOutlawsObject& O = Lx.Objects[i];
		if (!O.OnDifficulty(DiffBit) || (!Lx.SectorById(O.SectorId) && !Lx.SectorAt(O.Pos.X, O.Pos.Y, O.Pos.Z))) continue;     // (off the map: never built)
		const FString* Renamed = Roll.Rename.Find(i);
		FString Itm, Func;
		OutlawsFormats::ResolveAnim(*AssetsRef, Renamed ? *Renamed : O.Name, &Itm);
		TMap<FString, FString> Data;
		OutlawsFormats::ParseItm(Itm, Func, Data);
		if (const int32 Given = Ground(Func, Data); Given >= 0)
		{
			Placed += Given > 0 ? 1 : 0;
			continue;
		}
		// a bad guy's drop (one another kind by the roll keeps his own, as the build does); the townsfolk drop nothing
		if (const TMap<FString, FString>* Keep = Roll.Keep.Find(i))
		{
			for (const TPair<FString, FString>& Kv : *Keep)
			{
				if (Kv.Value.IsEmpty()) Data.Remove(Kv.Key);
				else Data.Add(Kv.Key, Kv.Value);
			}
		}
		const FString Drop = FPaths::GetBaseFilename(Data.FindRef(TEXT("DROP_ITEM"))).ToLower();
		if (Drop.IsEmpty() || Drop == TEXT("null") || Func.Equals(TEXT("Chicken_New"), ESearchCase::IgnoreCase) || Func.Equals(TEXT("Civilian_New"), ESearchCase::IgnoreCase)) continue;
		FString DropItm, DropFunc;
		OutlawsFormats::ResolveAnim(*AssetsRef, Drop, &DropItm);
		TMap<FString, FString> DropData;
		OutlawsFormats::ParseItm(DropItm, DropFunc, DropData);
		Drops += Ground(DropFunc, DropData) > 0 ? 1 : 0;
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT performance test: %s's finish carries the guns and ammunition of %d pickups and %d bad guys' drops on top of the start kit (%s)"),
		*LevelOne, Placed, Drops, *CarrySignature());
}

void AOutlawsLevelActor::TickPerfTest(float Dt)
{
	if (!OutlawsPerfTest::IsOn()) return;
	if (bFrontEnd)
	{
		// the main menu again (its level built): the run is over, and the saves are as ever from here
		if (OutlawsPerfTest::IsRunning()) OutlawsPerfTest::End(TEXT("quit from the pause menu"));
		OutlawsPerfTest::Finished();
		return;
	}
	if (UOutlawsCoop::IsActive(this) || (OutlawsPerfTest::IsRunning() && !LevelName.Equals(OutlawsPerfTest::Level(), ESearchCase::IgnoreCase)))
	{
		// (left for a session, or another level, some other way: the run ends there, and a session saves as it does)
		OutlawsPerfTest::End(TEXT("quit from the pause menu"));
		OutlawsPerfTest::Finished();
		return;
	}
	if (OutlawsPerfTest::IsSweep() && !OutlawsPerfTest::IsStarting())
	{
		TickPerfSweep(Dt);                     // (REQ-PERFSWEEP: its phases, then its end card; OutlawsPerfSweep.cpp)
		return;
	}
	if (!OutlawsPerfTest::IsStarting()) return;
	UWorld* World = GetWorld();
	AOutlawsPlayer* P = World && World->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(World->GetFirstPlayerController()->GetPawn()) : nullptr;
	if (!P || bNeedPlace || bDead || PendingLoad.IsValid() || IsMoviePlaying() || !LevelName.Equals(OutlawsPerfTest::Level(), ESearchCase::IgnoreCase)) return;
	// play begins: the capture on, the settings dump beside it with the capture's stamp
	const FString Capture = OutlawsPerf::StartTestCapture();
	if (OutlawsPerfTest::IsSweep())
	{
		// (REQ-PERFSWEEP) the sweep's table takes the dump's lines; the player to its spot, its lead-in
		OutlawsPerfTest::Started(Capture.IsEmpty() ? FString(TEXT("(none: -OLNoPerfFiles)")) : Capture, FString());
		PerfSweepBegin(Capture);
		return;
	}
	FString Settings;
	if (!Capture.IsEmpty())
	{
		FString Stamp = FPaths::GetBaseFilename(Capture);
		Stamp.RemoveFromStart(TEXT("capture_"));
		Settings = FPaths::Combine(FPaths::GetPath(Capture), FString::Printf(TEXT("perftest_%s_settings.txt"), *Stamp));
		if (!FFileHelper::SaveStringToFile(PerfTestSettings(Capture), *Settings, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogOutlaws, Warning, TEXT("OLRT performance test: the settings dump %s could not be written"), *Settings);
		}
		OutlawsPerf::MakeReadable(Settings);     // (BUG-PERFFLAG2: adb's shell reads it)
	}
	OutlawsPerfTest::Started(Capture.IsEmpty() ? FString(TEXT("(none: -OLNoPerfFiles)")) : Capture, Settings.IsEmpty() ? FString(TEXT("(none: -OLNoPerfFiles)")) : Settings);
}

bool AOutlawsLevelActor::PerfTestLeave()
{
	if (!OutlawsPerfTest::IsOn() || OutlawsPerfTest::IsStarting()) return false;
	// the run's level won (its end line written already at the exit) or left by the cheats' skip: the main menu, no level 3
	OutlawsPerfTest::End(TEXT("quit from the pause menu"));
	UE_LOG(LogOutlaws, Log, TEXT("OLRT performance test: %s left, back to the main menu (no level after it)"), *LevelName);
	ReturnToFrontEnd();
	return true;
}

FString AOutlawsLevelActor::PerfTestSettings(const FString& CapturePath) const
{
	// one "key: value" line each, the values as applied now (a tool reads it: keys never change, "not available" where the
	// platform gives nothing)
	TArray<FString> Lines;
	auto Add = [&Lines](const TCHAR* Key, const FString& Value) { Lines.Add(FString::Printf(TEXT("%s: %s"), Key, *Value)); };
	auto OnOff = [](bool bOn) { return FString(bOn ? TEXT("on") : TEXT("off")); };
	auto Cvar = [](const TCHAR* Name)
	{
		IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(Name);
		return V ? V->GetString() : FString(TEXT("not available"));
	};
	const UWorld* World = GetWorld();
	const AOutlawsPlayer* P = World && World->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(World->GetFirstPlayerController()->GetPawn()) : nullptr;
	TArray<FString> Order;
	RandoLevelOrder(Order);
	Add(TEXT("build"), OutlawsBuildInfo::Line());
	Add(TEXT("started"), FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")));
	Add(TEXT("level"), LevelName);
	Add(TEXT("level_title"), LevelTitle(LevelName));
	Add(TEXT("level_one"), Order.IsValidIndex(0) ? Order[0] : FString(TEXT("hideout")));
	Add(TEXT("difficulty"), OutlawsDifficulty::Name(Difficulty));
	Add(TEXT("difficulty_from"), OutlawsPerfTest::DifficultyFrom());
	Add(TEXT("capture"), CapturePath);
	Add(TEXT("headset"), OutlawsDeviceProfile::HeadsetName(OutlawsDeviceProfile::Headset()));
	Add(TEXT("device_model"), OutlawsDeviceProfile::Model());
	Add(TEXT("vr"), OnOff(P && P->bVR));
	Add(TEXT("profile"), OutlawsDeviceProfile::IsQuest2() ? TEXT("Quest 2") : TEXT("default"));
	Add(TEXT("quest2_mode"), OnOff(OutlawsDeviceProfile::Quest2Mode()));
	Add(TEXT("resolution_percent"), FString::FromInt(OutlawsDeviceProfile::ResolutionPercent()) + (OutlawsDynRes::IsActive() ? TEXT(" (the ceiling: dynamic resolution on)") : TEXT("")));
	Add(TEXT("vr_pixel_density"), Cvar(TEXT("vr.PixelDensity")));
	Add(TEXT("screen_percentage"), Cvar(TEXT("r.ScreenPercentage")));
	const FString Eye = OutlawsDeviceProfile::EyeBufferText();
	Add(TEXT("eye_buffer"), Eye.IsEmpty() ? FString(TEXT("not available")) : Eye);
	const float Hz = OutlawsXRRefresh::CurrentRate();
	Add(TEXT("refresh_rate"), Hz > 0.f ? FString::Printf(TEXT("%.0f Hz"), Hz) : FString(TEXT("not available")));
	const float HzAsked = OutlawsDeviceProfile::RefreshRate();
	Add(TEXT("refresh_rate_asked"), HzAsked > 0.f ? FString::Printf(TEXT("%.0f Hz"), HzAsked) : FString(TEXT("auto (the headset's own)")));
	Add(TEXT("lamp_lights"), FString::FromInt(OutlawsDeviceProfile::LampLights()));
	Add(TEXT("hd_art"), OnOff(bHDTextures));
	Add(TEXT("object_detail"), FString::Printf(TEXT("%s (x%.2f)"), *OutlawsDeviceProfile::RowValue(TEXT("r_objdetail")), OutlawsDeviceProfile::ObjectDetailScale()));
	Add(TEXT("stochastic"), OnOff(bStochastic));
	Add(TEXT("tinting"), OnOff(bPaletteGrades));
	Add(TEXT("brightness"), FString::Printf(TEXT("%.2f"), OutlawsDeviceProfile::Gamma()));
	Add(TEXT("perf_hud"), OnOff(OutlawsDeviceProfile::PerfHud()));
	Add(TEXT("grab_glow"), OnOff(OutlawsDeviceProfile::GrabGlow()));
	Add(TEXT("movement_speed"), FString::Printf(TEXT("%.0f %%"), GameSpeed * 200.f));
	Add(TEXT("bad_guys"), FString::Printf(TEXT("x%.2f"), SoloBadGuysWanted()));
	Add(TEXT("randomizer"), OnOff(RandoOn()));
	Add(TEXT("randomizer_enemies"), PerfRandoModes[0][FMath::Clamp(RandoRun.Enemies, 0, 2)]);
	Add(TEXT("randomizer_items"), PerfRandoModes[1][FMath::Clamp(RandoRun.Items, 0, 2)]);
	Add(TEXT("randomizer_order"), PerfRandoModes[2][FMath::Clamp(RandoRun.Order, 0, 2)]);
	Add(TEXT("randomizer_bosses"), OnOff(RandoRun.Bosses != 0));
	Add(TEXT("randomizer_critters"), OnOff(RandoRun.Critters != 0));
	FString SeedText = RandoSeedText;
	OLFlags::Value(TEXT("OLRandoSeed"), SeedText);         // (as BeginRandoRun takes it: a test's own seed)
	Add(TEXT("randomizer_seed"), FString::Printf(TEXT("%u (%s)"), RandoRun.Seed, SeedText.IsEmpty() ? TEXT("random") : TEXT("typed")));
	Add(TEXT("cpu_gpu_level"), OutlawsXRPerf::LevelName(OutlawsXRPerf::Wanted()));
	Add(TEXT("xr_perf"), OutlawsXRPerf::StatusText());
	Add(TEXT("device_state"), OutlawsDeviceProfile::StateText());
	int32 Battery = -1;
	float TempC = -1000.f;
	FString Why;
	const bool bBattery = PerfTestBattery(Battery, TempC, Why);
	Add(TEXT("battery_level"), bBattery ? FString::Printf(TEXT("%d %%"), Battery) : FString::Printf(TEXT("not available (%s)"), *Why));
	Add(TEXT("battery_temperature"), TempC > -999.f ? FString::Printf(TEXT("%.1f C"), TempC) : FString::Printf(TEXT("not available (%s)"), Why.IsEmpty() ? TEXT("not reported") : *Why));
	FString Carried;
	for (const TPair<FString, int32>& Kv : OutlawsPerfTest::Carry()) Carried += FString::Printf(TEXT("%s%s=%d"), Carried.IsEmpty() ? TEXT("") : TEXT(" "), *Kv.Key, Kv.Value);
	Add(TEXT("carried_in"), Carried);
	Add(TEXT("counts_at_start"), CarrySignature());
	return FString::Join(Lines, TEXT("\n")) + TEXT("\n");
}
