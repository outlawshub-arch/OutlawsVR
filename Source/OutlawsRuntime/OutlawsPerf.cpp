// Performance diagnostics: see OutlawsPerf.h.
#include "OutlawsPerf.h"
#include "OutlawsFlags.h"
#include "OutlawsShipLog.h"
#include "Misc/CommandLine.h"
#include "OutlawsRuntime.h"
#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsEnemy.h"
#include "OutlawsSpriteActor.h"
#include "ProceduralMeshComponent.h"
#include "ConvexVolume.h"
#include "SceneManagement.h"
#include "Engine/LocalPlayer.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/PlayerController.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "RenderCore.h"
#include "RHI.h"
#include "DynamicRHI.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/CoreDelegates.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDevice.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UObjectGlobals.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include <atomic>

CSV_DEFINE_CATEGORY(Outlaws, true);

namespace
{
	constexpr int32 SmoothFrames = 8;          // the lag trigger's window (as EX_PERF_WATCH)
	constexpr double DropTrigger = 1.25;       // smoothed frame time over budget x this = a lag moment
	constexpr double MaxSaneMs = 250.0;        // longer frames are loads, not lag
	constexpr double CooloffSeconds = 10.0;
	constexpr int32 MaxScenarios = 10;
	constexpr int32 MaxMarks = 12;
	constexpr int32 TrailSize = 160;
	constexpr double HangSeconds = 10.0;       // a level build takes ~4 s on the headset

	TAutoConsoleVariable<int32> CVarFreezeMeshUpdates(TEXT("ol.debug.FreezeMeshUpdates"), 0,
		TEXT("1: skip the per-frame vertex rewrites of drawn meshes (scrolls, glass, blood pools), to test the GPU hang."));
	TAutoConsoleVariable<float> CVarTargetFps(TEXT("ol.perf.TargetFps"), 72.f,
		TEXT("Frame rate the lag detector measures against (the headset's refresh)."));
	TAutoConsoleVariable<float> CVarFakeXRWait(TEXT("ol.throttle.FakeXRWaitMs"), 0.f,
		TEXT("Tests on the desktop: the game thread spins this many ms in the world's start each frame, as the headset's xrWaitFrame holds it."));

	double Ms(uint32 Cycles) { return FPlatformTime::ToMilliseconds(Cycles); }

	struct FBucketStat
	{
		FString Name;
		FName CsvName;
		uint64 FrameCycles = 0;
		double Recent[SmoothFrames] = {};
		double SecSum = 0.0, SecPeak = 0.0;
		double LiveSum = 0.0, LivePeak = 0.0;
		double SessionSum = 0.0, SessionPeak = 0.0;
	};

	struct FPerfFrame
	{
		double FrameMs = 0.0, GameMs = 0.0, RenderMs = 0.0, RhiMs = 0.0, GpuMs = 0.0;
		int32 Draws = 0, Prims = 0;
	};

	struct FSnapshot
	{
		double Severity = 0.0;                 // smoothed ms / budget ms
		FString Text;
	};

	struct FWindow                             // sums for the 1 s timeline row and the 5 s live file
	{
		int32 Frames = 0;
		double Frame = 0, FrameMax = 0, Game = 0, Render = 0, Rhi = 0, Gpu = 0, GpuMax = 0;
		double Draws = 0, Prims = 0;
		void Add(const FPerfFrame& F)
		{
			++Frames;
			Frame += F.FrameMs; FrameMax = FMath::Max(FrameMax, F.FrameMs);
			Game += F.GameMs; Render += F.RenderMs; Rhi += F.RhiMs;
			Gpu += F.GpuMs; GpuMax = FMath::Max(GpuMax, F.GpuMs);
			Draws += F.Draws; Prims += F.Prims;
		}
		double Avg(double Sum) const { return Frames ? Sum / Frames : 0.0; }
	};

	FCriticalSection BucketLock;
	TArray<FBucketStat> BucketStats;
	FCriticalSection TrailLock;
	TArray<FString> Trail;
	int32 TrailNext = 0;
	FString TrailLast;
	int32 TrailRepeat = 0;

	FDelegateHandle EndFrameHandle;
	FString PerfDir;
	FString PriorPerformance;                  // performance.txt as it was before this session
	FString SessionStamp;
	double SessionStart = 0.0;
	double LastEnd = 0.0;
	FPerfFrame FrameRing[SmoothFrames];
	int32 RingNext = 0, RingCount = 0;
	FWindow SecWindow, LiveWindow;
	double SecStart = 0.0, LiveStart = 0.0, LastLag = -1e9;
	TArray<FSnapshot> Scenarios;
	TArray<FString> Marks;
	int64 SessionFrames = 0;
	int32 TexturesMadeSec = 0;
	int64 TexelsMadeSec = 0;
	int32 TexturesMadeTotal = 0;
	bool bGpuTiming = false;

	// the context, refreshed once a second on the game thread (the hang watchdog reads the copy)
	TWeakObjectPtr<AOutlawsPlayer> PlayerRef;
	TWeakObjectPtr<AOutlawsLevelActor> LevelRef;
	FCriticalSection ContextLock;
	FString ContextText;

	std::atomic<int32> LoadingDepth{0};
	/** TEXLOAD: the frame the last level load ended in, and its length: that frame is loading, not play. */
	std::atomic<uint64> LoadEndFrame{0};
	std::atomic<bool> bLoadEnded{false};
	std::atomic<bool> bLoadSinceRow{false};
	double LastLoadMs = 0.0;
	double LoadStartTime = 0.0;
	std::atomic<bool> bAppPaused{false};      // the headset taken off / the app sent to the background: no frames, no hang
	std::atomic<uint64> HeartbeatFrame{0};
	std::atomic<double> HeartbeatTime{0.0};

	UWorld* GameWorld()
	{
		if (!GEngine) return nullptr;
		for (const FWorldContext& C : GEngine->GetWorldContexts())
		{
			if (C.WorldType == EWorldType::Game || C.WorldType == EWorldType::PIE)
			{
				return C.World();
			}
		}
		return nullptr;
	}

	void FindActors()
	{
		UWorld* W = GameWorld();
		if (!W) return;
		if (!PlayerRef.IsValid())
		{
			for (TActorIterator<AOutlawsPlayer> It(W); It; ++It) { PlayerRef = *It; break; }
		}
		if (!LevelRef.IsValid())
		{
			for (TActorIterator<AOutlawsLevelActor> It(W); It; ++It) { LevelRef = *It; break; }
		}
	}

	/** A level load ran in this frame (or one is running): the frame counts as loading. */
	bool IsLoadFrame()
	{
		return LoadingDepth.load() > 0 || (bLoadEnded.load() && GFrameCounter <= LoadEndFrame.load() + 1);
	}

	bool InPlay()
	{
		const AOutlawsPlayer* P = PlayerRef.Get();
		const AOutlawsLevelActor* L = LevelRef.Get();
		return P && L && !P->IsMenuOpen() && !P->IsDeadView() && !L->IsMoviePlaying() && !L->IsFrontEnd() && !IsLoadFrame() && !L->IsRebuildPending();
	}

	/** Level, position and what is alive: the scene the numbers came from. */
	FString DescribeScene()
	{
		UWorld* W = GameWorld();
		const AOutlawsPlayer* P = PlayerRef.Get();
		const AOutlawsLevelActor* L = LevelRef.Get();
		int32 Enemies = 0, Alive = 0, Sprites = 0, Meshes = 0, MeshesVisible = 0;
		if (W)
		{
			for (TActorIterator<AOutlawsEnemy> It(W); It; ++It) { ++Enemies; if (!It->IsDead()) ++Alive; }
			for (TActorIterator<AOutlawsSpriteActor> It(W); It; ++It) ++Sprites;
			for (TObjectIterator<UProceduralMeshComponent> It; It; ++It)
			{
				if (It->GetWorld() != W || !It->IsRegistered()) continue;
				++Meshes;
				if (It->IsVisible()) ++MeshesVisible;
			}
		}
		const FVector Pos = P ? P->GetActorLocation() : FVector::ZeroVector;
		const float Yaw = P ? P->GetControlRotation().Yaw : 0.f;
		return FString::Printf(TEXT("level %s%s | player (%.0f, %.0f, %.0f) yaw %.0f | enemies %d (%d alive), sprites %d, meshes %d (%d visible), runtime textures %d"),
			L ? *L->LevelName : TEXT("?"), InPlay() ? TEXT("") : TEXT(" (menu/movie/dead)"), Pos.X, Pos.Y, Pos.Z, Yaw,
			Enemies, Alive, Sprites, Meshes, MeshesVisible, TexturesMadeTotal);
	}

	FString DescribeSettings()
	{
		auto Get = [](const TCHAR* Name) -> FString
		{
			IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(Name);
			return V ? V->GetString() : FString(TEXT("-"));
		};
		return FString::Printf(TEXT("vr.PixelDensity %s, r.MobileMSAA %s, r.MSAACount %s, r.AllowOcclusionQueries %s, r.ViewDistanceScale %s, GPU timing %s"),
			*Get(TEXT("vr.PixelDensity")), *Get(TEXT("r.MobileMSAA")), *Get(TEXT("r.MSAACount")), *Get(TEXT("r.AllowOcclusionQueries")),
			*Get(TEXT("r.ViewDistanceScale")), bGpuTiming ? TEXT("on") : TEXT("off (r.Vulkan.SupportsTimestampQueries 0)"));
	}

	FString DescribeMemory()
	{
		const FPlatformMemoryStats M = FPlatformMemory::GetStats();
		return FString::Printf(TEXT("memory used %.0f MB, peak %.0f MB, free %.0f MB"),
			M.UsedPhysical / 1048576.0, M.PeakUsedPhysical / 1048576.0, M.AvailablePhysical / 1048576.0);
	}

	/** The last SmoothFrames frames: averages, and the buckets costliest first. */
	FString DescribeRecent()
	{
		FPerfFrame Avg;
		const int32 N = FMath::Max(1, RingCount);
		for (int32 i = 0; i < RingCount; ++i)
		{
			const FPerfFrame& F = FrameRing[i];
			Avg.FrameMs += F.FrameMs / N; Avg.GameMs += F.GameMs / N; Avg.RenderMs += F.RenderMs / N;
			Avg.RhiMs += F.RhiMs / N; Avg.GpuMs += F.GpuMs / N; Avg.Draws += F.Draws / N; Avg.Prims += F.Prims / N;
		}
		FString S = FString::Printf(TEXT("frame %.1f ms (%.0f fps) | game %.1f, render %.1f, RHI %.1f, GPU %.1f ms | %d draws, %d triangles\n"),
			Avg.FrameMs, Avg.FrameMs > 0 ? 1000.0 / Avg.FrameMs : 0.0, Avg.GameMs, Avg.RenderMs, Avg.RhiMs, Avg.GpuMs, Avg.Draws, Avg.Prims);
		TArray<TPair<double, FString>> Rows;
		{
			FScopeLock Lock(&BucketLock);
			for (const FBucketStat& B : BucketStats)
			{
				double Sum = 0;
				for (double V : B.Recent) Sum += V;
				if (Sum > 0) Rows.Add({Sum / SmoothFrames, B.Name});
			}
		}
		Rows.Sort([](const TPair<double, FString>& A, const TPair<double, FString>& B) { return A.Key > B.Key; });
		S += TEXT("    our CPU (ms/frame):");
		for (int32 i = 0; i < FMath::Min(8, Rows.Num()); ++i)
		{
			S += FString::Printf(TEXT(" %s %.2f%s"), *Rows[i].Value, Rows[i].Key, i + 1 < FMath::Min(8, Rows.Num()) ? TEXT(",") : TEXT(""));
		}
		return S;
	}

	FString TakeSnapshot(const TCHAR* Why, double SmoothedMs, double BudgetMs)
	{
		const FDateTime Now = FDateTime::Now();
		FString S = FString::Printf(TEXT("[%s, %.0f s in] %s: smoothed %.1f ms vs %.1f ms budget (%.0f of %.0f fps)\n"),
			*Now.ToString(TEXT("%H:%M:%S")), FPlatformTime::Seconds() - SessionStart, Why, SmoothedMs, BudgetMs,
			SmoothedMs > 0 ? 1000.0 / SmoothedMs : 0.0, 1000.0 / BudgetMs);
		S += TEXT("    ") + DescribeRecent() + TEXT("\n");
		S += TEXT("    ") + DescribeScene() + TEXT("\n");
		S += TEXT("    ") + DescribeMemory() + TEXT("\n");
		S += TEXT("    ") + DescribeSettings() + TEXT("\n");
		return S;
	}

	void WritePerformance()
	{
		FString S = FString::Printf(TEXT("=== session %s (%s) ===\n"), *SessionStamp, ANSI_TO_TCHAR(FPlatformProperties::IniPlatformName()));
		S += FString::Printf(TEXT("-- worst lag moments (smoothed over %d frames, %.2fx the budget or worse), worst first --\n"), SmoothFrames, DropTrigger);
		if (Scenarios.Num() == 0) S += TEXT("(none)\n");
		for (const FSnapshot& Sc : Scenarios) S += FString::Printf(TEXT("severity %.2fx  "), Sc.Severity) + Sc.Text;
		S += TEXT("-- manual marks, in order --\n");
		if (Marks.Num() == 0) S += TEXT("(none)\n");
		for (const FString& M : Marks) S += M;
		FFileHelper::SaveStringToFile(S + TEXT("\n") + PriorPerformance, *FPaths::Combine(PerfDir, TEXT("performance.txt")));
	}

	void WriteLive(double Seconds)
	{
		const FWindow& W = LiveWindow;
		FString S = FString::Printf(TEXT("Outlaws perf, %s, %.0f s into the session, last %.1f s (%d frames)\n"),
			*FDateTime::Now().ToString(TEXT("%H:%M:%S")), FPlatformTime::Seconds() - SessionStart, Seconds, W.Frames);
		S += FString::Printf(TEXT("frame  avg %6.2f ms  peak %6.2f ms  (%.1f fps, budget %.1f ms)\n"), W.Avg(W.Frame), W.FrameMax,
			W.Frame > 0 ? 1000.0 * W.Frames / W.Frame : 0.0, 1000.0 / FMath::Max(1.f, CVarTargetFps.GetValueOnGameThread()));
		S += FString::Printf(TEXT("game   avg %6.2f ms\nrender avg %6.2f ms\nRHI    avg %6.2f ms\nGPU    avg %6.2f ms  peak %6.2f ms%s\n"),
			W.Avg(W.Game), W.Avg(W.Render), W.Avg(W.Rhi), W.Avg(W.Gpu), W.GpuMax, bGpuTiming ? TEXT("") : TEXT("  (no GPU timing on this device)"));
		S += FString::Printf(TEXT("draws  avg %6.0f\ntris   avg %6.0f\n"), W.Avg(W.Draws), W.Avg(W.Prims));
		S += DescribeMemory() + TEXT("\n") + DescribeScene() + TEXT("\n") + DescribeSettings() + TEXT("\n\n");
		S += TEXT("our CPU buckets (game thread), costliest over the last 5 s first; session = live play only:\n  last5s avg   last5s peak   % of frame   session avg   session peak   name\n");
		TArray<FBucketStat*> Sorted;
		FScopeLock Lock(&BucketLock);
		for (FBucketStat& B : BucketStats) Sorted.Add(&B);
		Sorted.Sort([](const FBucketStat& A, const FBucketStat& B) { return A.LiveSum > B.LiveSum; });
		const double FrameAvg = FMath::Max(0.001, W.Avg(W.Frame));
		for (FBucketStat* B : Sorted)
		{
			const double Avg = W.Frames ? B->LiveSum / W.Frames : 0.0;
			S += FString::Printf(TEXT("  %10.3f   %11.3f   %9.1f%%   %11.3f   %12.3f   %s\n"), Avg, B->LivePeak, 100.0 * Avg / FrameAvg,
				SessionFrames ? B->SessionSum / SessionFrames : 0.0, B->SessionPeak, *B->Name);
			B->LiveSum = 0.0;
			B->LivePeak = 0.0;
		}
#if UE_BUILD_SHIPPING
		S += TEXT("\n(GPU ms spans the frame's first to last GPU command, waits for the CPU included. A row a frame: Saved/Perf/capture.on, which tools/olue/questperf record / stop makes and removes)\n");
#else
		S += TEXT("\n(per-pass GPU: `csvprofile start` / `stop`, which tools/olue/questperf does for you; our own row a frame: ol.perf.Capture 1)\n");
#endif
		FFileHelper::SaveStringToFile(S, *FPaths::Combine(PerfDir, TEXT("perf_live.txt")));
	}

	FString TimelinePending;
	int32 TimelineRowsPending = 0;
	// CPU1 step 0a (2026-10-04): every bucket each second (perf_buckets.csv, a row per bucket that ran), and the counts that
	// drive them (perf_counts.csv, a row a second); written with the timeline
	FString BucketsPending;
	FString CountsPending;

	// (user 2026-10-03, BUG-STUTTER) the stutters themselves: every single frame in play over twice the headset's budget
	// (the lag trigger above averages 8 frames and misses a lone long one), one row each in Saved/Perf/hitches.csv with
	// what that frame did: our log lines, a garbage collection, textures made; the player's marks in the same file
	FString HitchPending;
	int32 HitchRowsPending = 0;
	int32 HitchesSession = 0;
	TArray<FString> FrameEvents;              // (under TrailLock: lines may come from any thread)
	std::atomic<bool> bGcThisFrame{false};
	int32 TexturesAtFrameStart = 0;

	// CPU2 step 0 (2026-10-05): the game thread's time outside our buckets, by phase of the world's tick, from the engine's own
	// delegates (a timestamp each, so nothing worth measuring, in every build):
	//  - world start: OnWorldTickStart to OnWorldPreActorTick, which holds the XR runtime's xrWaitFrame (OpenXR blocks the
	//    game thread there for the headset's pacing, and that wait is not taken off GGameThreadTime), the net tick, navigation;
	//  - actor ticks: every tick group (ours, the bad guys' CharacterMovement, physics, the rest), our buckets included;
	//  - world end: timers, latent actions, tickable objects, cameras, level streaming and garbage collection;
	//  - garbage collection on its own, with a count a second in perf_counts.csv.
	// What is left of game_ms past the world's tick (Slate, the viewport's draw, the end-of-frame sync) cpu2_buckets.py derives.
	uint64 PhaseStart = 0, PhasePre = 0, PhasePost = 0, GcStart = 0;
	double StartMsNow = 0.0, StartMsLast = 0.0;    // this frame's world start, and the last whole frame's (XRWaitMs)
	int32 GcSec = 0;
	double GcFrameMs = 0.0;
	TWeakObjectPtr<UWorld> PhaseWorld;

	void OnWorldTickStart(UWorld* W, ELevelTick, float)
	{
		if (!W || !W->IsGameWorld()) return;
		PhaseWorld = W;
		PhaseStart = FPlatformTime::Cycles64();
		PhasePre = PhasePost = 0;
		StartMsLast = StartMsNow;
		StartMsNow = 0.0;
		// (the desktop's stand-in for xrWaitFrame: a spin, not a sleep, so GGameThreadTime counts it as the headset's wait is)
		const float Fake = CVarFakeXRWait.GetValueOnGameThread();
		if (Fake > 0.f)
		{
			const uint64 Until = PhaseStart + uint64(double(Fake) / 1000.0 / FPlatformTime::GetSecondsPerCycle64());
			while (FPlatformTime::Cycles64() < Until) {}
		}
	}
	void OnWorldPreActorTick(UWorld* W, ELevelTick, float)
	{
		if (!PhaseStart || PhaseWorld.Get() != W) return;
		static const int32 B = OutlawsPerf::Bucket(TEXT("Eng: world start (XR wait net)"));
		PhasePre = FPlatformTime::Cycles64();
		OutlawsPerf::Add(B, PhasePre - PhaseStart);
		StartMsNow = FPlatformTime::ToMilliseconds64(PhasePre - PhaseStart);
	}
	void OnWorldPostActorTick(UWorld* W, ELevelTick, float)
	{
		if (!PhasePre || PhaseWorld.Get() != W) return;
		static const int32 B = OutlawsPerf::Bucket(TEXT("Eng: actor ticks (total)"));
		PhasePost = FPlatformTime::Cycles64();
		OutlawsPerf::Add(B, PhasePost - PhasePre);
	}
	void OnWorldTickEnd(UWorld* W, ELevelTick, float)
	{
		if (!PhaseStart || PhaseWorld.Get() != W) return;
		static const int32 BStart = OutlawsPerf::Bucket(TEXT("Eng: world start (XR wait net)"));
		static const int32 BEnd = OutlawsPerf::Bucket(TEXT("Eng: world end (timers GC streaming)"));
		static const int32 BAll = OutlawsPerf::Bucket(TEXT("Eng: world tick (total)"));
		const uint64 Now = FPlatformTime::Cycles64();
		if (PhasePost) OutlawsPerf::Add(BEnd, Now - PhasePost);
		else if (!PhasePre)      // (a time-only tick: no actor ticks)
		{
			OutlawsPerf::Add(BStart, Now - PhaseStart);
			StartMsNow = FPlatformTime::ToMilliseconds64(Now - PhaseStart);
		}
		OutlawsPerf::Add(BAll, Now - PhaseStart);
		PhaseStart = PhasePre = PhasePost = 0;
	}
	void OnPreGc() { GcStart = IsInGameThread() ? FPlatformTime::Cycles64() : 0; }
	void OnPostGc()
	{
		bGcThisFrame.store(true);
		if (!GcStart || !IsInGameThread()) return;
		static const int32 B = OutlawsPerf::Bucket(TEXT("Eng: GC"));
		const uint64 C = FPlatformTime::Cycles64() - GcStart;
		GcStart = 0;
		OutlawsPerf::Add(B, C);
		GcFrameMs += FPlatformTime::ToMilliseconds64(C);
		++GcSec;
	}

	void FlushHitches()
	{
		if (HitchPending.IsEmpty() || PerfDir.IsEmpty()) return;
		FFileHelper::SaveStringToFile(HitchPending, *FPaths::Combine(PerfDir, TEXT("hitches.csv")), FFileHelper::EEncodingOptions::AutoDetect,
			&IFileManager::Get(), FILEWRITE_Append);
		HitchPending.Reset();
		HitchRowsPending = 0;
	}

	FString CsvSafe(FString S)
	{
		S.ReplaceInline(TEXT(","), TEXT(";"));
		S.ReplaceInline(TEXT("\n"), TEXT(" "));
		S.ReplaceInline(TEXT("\r"), TEXT(" "));
		S.ReplaceInline(TEXT("\""), TEXT("'"));
		return S;
	}

	void HitchRow(double T, const TCHAR* Kind, const FPerfFrame* F, const FString& Top, double TopMs, const FString& What)
	{
		const AOutlawsPlayer* P = PlayerRef.Get();
		const AOutlawsLevelActor* L = LevelRef.Get();
		const FVector Pos = P ? P->GetActorLocation() : FVector::ZeroVector;
		HitchPending += FString::Printf(TEXT("%.2f,%s,%s,%.0f,%.0f,%.0f,%.1f,%.1f,%.1f,%.1f,%.1f,%s,%.2f,\"%s\"\n"), T, Kind, L ? *L->LevelName : TEXT(""),
			Pos.X, Pos.Y, Pos.Z, F ? F->FrameMs : 0.0, F ? F->GameMs : 0.0, F ? F->RenderMs : 0.0, F ? F->RhiMs : 0.0, F ? F->GpuMs : 0.0,
			*Top, TopMs, *CsvSafe(What));
		if (++HitchRowsPending >= 10) FlushHitches();
	}

	void FlushTimeline()
	{
		if (PerfDir.IsEmpty()) return;
		auto Append = [](FString& Pending, const TCHAR* Name)
		{
			if (Pending.IsEmpty()) return;
			FFileHelper::SaveStringToFile(Pending, *FPaths::Combine(PerfDir, Name), FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), FILEWRITE_Append);
			Pending.Reset();
		};
		Append(TimelinePending, TEXT("perf_timeline.csv"));
		Append(BucketsPending, TEXT("perf_buckets.csv"));
		Append(CountsPending, TEXT("perf_counts.csv"));
		TimelineRowsPending = 0;
	}

	/** CPU1 step 0a: the counts behind the buckets, once a second (the scene's actors walked once). */
	FString CountsRow(double T)
	{
		UWorld* W = GameWorld();
		const AOutlawsLevelActor* L = LevelRef.Get();
		int32 Actors = 0, Ticking = 0, Sprites = 0, SpritesShown = 0, SpritesTicking = 0, Enemies = 0, EnemiesAwake = 0, Meshes = 0, MeshesShown = 0, Sections = 0;
		if (W)
		{
			for (TActorIterator<AActor> It(W); It; ++It)
			{
				++Actors;
				// (QSPEED-1: in the engine's tick queue; the bad guys' own ticks are the enemy driver's now)
				if (It->IsActorTickEnabled() && It->PrimaryActorTick.IsTickFunctionRegistered()) ++Ticking;
			}
			for (TActorIterator<AOutlawsSpriteActor> It(W); It; ++It)
			{
				++Sprites;
				if (!It->IsHidden() && It->Quad && It->Quad->IsVisible()) ++SpritesShown;
				if (It->IsActorTickEnabled()) ++SpritesTicking;
			}
			for (TActorIterator<AOutlawsEnemy> It(W); It; ++It)
			{
				++Enemies;
				if (!It->IsDead() && It->PerfAwake()) ++EnemiesAwake;
			}
			for (TObjectIterator<UProceduralMeshComponent> It; It; ++It)
			{
				if (It->GetWorld() != W || !It->IsRegistered()) continue;
				++Meshes;
				if (It->IsVisible())
				{
					++MeshesShown;
					Sections += It->GetNumSections();
				}
			}
		}
		const int32 VisSectors = L ? L->PerfVisibleSectors() : -1;
		const int32 Movers = L ? L->PerfMoverGroups() : -1;
		const int32 Gcs = GcSec;
		GcSec = 0;
		return FString::Printf(TEXT("%.1f,%s,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%.2f,%.2f,%.2f\n"), T, L ? *L->LevelName : TEXT(""), Actors, Ticking, Sprites, SpritesShown, SpritesTicking,
			Enemies, EnemiesAwake, Meshes, MeshesShown, Sections, VisSectors, Movers, Gcs,
			L ? L->ThrottleLevel : -1, L ? L->ThrottleSmoothMs : 0.0, L ? L->ThrottleSmoothWaitMs : 0.0, L ? L->ThrottleEffMs : 0.0);
	}

	void WriteTimelineRow(double T)
	{
		const FWindow& W = SecWindow;
		const bool bLoadRow = bLoadSinceRow.exchange(false);       // (TEXLOAD) a second with a level load in it is loading
		FString TopName = TEXT("-");
		double TopMs = 0.0;
		{
			FScopeLock Lock(&BucketLock);
			const FString Lv = LevelRef.IsValid() ? LevelRef->LevelName : FString();
			const int32 Play = InPlay() && !bLoadRow ? 1 : 0;
			for (FBucketStat& B : BucketStats)
			{
				const double Avg = W.Frames ? B.SecSum / W.Frames : 0.0;
				if (Avg > TopMs) { TopMs = Avg; TopName = B.Name; }
				// (CPU1 step 0a) every bucket that ran this second: its average and its peak frame
				if (B.SecSum > 0.0) BucketsPending += FString::Printf(TEXT("%.1f,%s,%s,%.3f,%.3f,%d\n"), T, *Lv, *CsvSafe(B.Name), Avg, B.SecPeak, Play);
				B.SecSum = 0.0;
				B.SecPeak = 0.0;
			}
		}
		const AOutlawsPlayer* P = PlayerRef.Get();
		const AOutlawsLevelActor* L = LevelRef.Get();
		const FVector Pos = P ? P->GetActorLocation() : FVector::ZeroVector;
		const FPlatformMemoryStats M = FPlatformMemory::GetStats();
		const FString Row = FString::Printf(TEXT("%.1f,%s,%.0f,%.0f,%.0f,%.0f,%.1f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.2f,%.0f,%.0f,%.0f,%d,%.2f,%s,%.3f,%d\n"),
			T, L ? *L->LevelName : TEXT(""), Pos.X, Pos.Y, Pos.Z, P ? P->GetControlRotation().Yaw : 0.f,
			W.Frame > 0 ? 1000.0 * W.Frames / W.Frame : 0.0, W.Avg(W.Frame), W.FrameMax, W.Avg(W.Game), W.Avg(W.Render), W.Avg(W.Rhi),
			W.Avg(W.Gpu), W.GpuMax, W.Avg(W.Draws), W.Avg(W.Prims), M.UsedPhysical / 1048576.0, TexturesMadeSec, TexelsMadeSec / 1e6,
			*TopName, TopMs, InPlay() && !bLoadRow ? 1 : 0);
		// (2026-10-03, the Quest's power: rows gathered and appended every ten seconds, not a file opened each second; the
		// rest goes out with the session's end and with a hang's report)
		TimelinePending += Row;
		CountsPending += CountsRow(T);
		if (++TimelineRowsPending >= 10)
		{
			FlushTimeline();
			FlushHitches();       // (PERF-SHIPSUMMARY: a killed game, not quit, lost the last few hitches)
		}
		TexturesMadeSec = 0;
		TexelsMadeSec = 0;
	}

	// PERF-SHIPSUMMARY: a log line every 60 s and at each level's end, in every build (the files may be off): live play's
	// frames only, the 95th percentile from a 0.25 ms histogram (no frame list kept)
	constexpr double SummarySeconds = 60.0;
	constexpr double SummaryHitchMs = 30.0;
	constexpr int32 SummaryBins = 400;         // 0.25 ms each, to 100 ms; one more bin for the longer frames
	struct FSummary
	{
		int32 Frames = 0, Hitches = 0;
		double Frame = 0, Game = 0, Render = 0, Rhi = 0, Gpu = 0, Worst = 0, Start = 0;
		uint32 Hist[SummaryBins + 1] = {};
		void Add(const FPerfFrame& F)
		{
			++Frames;
			Frame += F.FrameMs; Game += F.GameMs; Render += F.RenderMs; Rhi += F.RhiMs; Gpu += F.GpuMs;
			Worst = FMath::Max(Worst, F.FrameMs);
			if (F.FrameMs > SummaryHitchMs) ++Hitches;
			++Hist[FMath::Clamp(int32(F.FrameMs * 4.0), 0, SummaryBins)];
		}
		double P95() const
		{
			const int64 Want = int64(FMath::CeilToDouble(Frames * 0.95));
			int64 Seen = 0;
			for (int32 i = 0; i <= SummaryBins; ++i)
			{
				Seen += Hist[i];
				if (Seen >= Want) return i == SummaryBins ? Worst : (i + 1) * 0.25;
			}
			return Worst;
		}
	};
	FSummary MinuteSum, LevelSum;
	FString SummaryLevel;                      // the level LevelSum's frames were played in
	bool bSummaryOn = false;

	FString CVarText(const TCHAR* Name)
	{
		IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(Name);
		return V ? V->GetString() : FString(TEXT("-"));
	}

	void LogSummary(const FString& What, FSummary& S)
	{
		const AOutlawsLevelActor* L = LevelRef.Get();
		if (S.Frames == 0)
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLPERF summary, %s: no live play"), *What);
		}
		else
		{
			const double N = S.Frames;
			UE_LOG(LogOutlaws, Log, TEXT("OLPERF summary, %s: %d frames in play over %.0f s, avg %.2f ms (%.1f fps), p95 %.2f, worst %.1f | game %.2f, render %.2f, RHI %.2f, GPU %.2f ms (GPU spans the waits on the Quest) | vr.PixelDensity %s, r.ScreenPercentage %s, throttle %d | hitches over %.0f ms: %d"),
				*What, S.Frames, FPlatformTime::Seconds() - S.Start, S.Frame / N, S.Frame > 0 ? 1000.0 * N / S.Frame : 0.0, S.P95(), S.Worst,
				S.Game / N, S.Render / N, S.Rhi / N, S.Gpu / N, *CVarText(TEXT("vr.PixelDensity")), *CVarText(TEXT("r.ScreenPercentage")),
				L ? L->ThrottleLevel : -1, SummaryHitchMs, S.Hitches);
		}
		S = FSummary();
		S.Start = FPlatformTime::Seconds();
	}

	/** The level's own line, when it is left (a level build starting, the game ending). */
	void LogLevelSummary(const TCHAR* Why)
	{
		if (!bSummaryOn || LevelSum.Frames == 0) return;
		LogSummary(FString::Printf(TEXT("level %s %s"), SummaryLevel.IsEmpty() ? TEXT("?") : *SummaryLevel, Why), LevelSum);
	}

	// PERF-SHIPSUMMARY: the detailed capture, the stand-in for csvprofile (compiled out of Shipping): a row every frame into
	// Saved/Perf/capture_<date_time>.csv, gathered and appended every 2 s.  On while Saved/Perf/capture.on exists (polled once
	// a second; tools/olue/questperf makes and removes it over adb), with -OLPerfCapture (the whole run), or ol.perf.Capture 1.
	TAutoConsoleVariable<int32> CVarCapture(TEXT("ol.perf.Capture"), 0,
		TEXT("1: a row a frame into Saved/Perf/capture_<date_time>.csv (as Saved/Perf/capture.on does)."));
	bool bCapturing = false;
	bool bCaptureFlag = false;
	FString CaptureFile;
	FString CapturePending;
	int32 CaptureRowsPending = 0;
	int64 CaptureFrames = 0;
	double CaptureStart = 0.0;

	FString CaptureHeader()
	{
		FString S = TEXT("t_s,level,frame_ms,game_ms,render_ms,rhi_ms,gpu_ms,draws,tris,in_play,load,xr_wait_ms,gc_ms,textures_made");
		FScopeLock Lock(&BucketLock);
		for (const FBucketStat& B : BucketStats) S += TEXT(",") + CsvSafe(B.Name);
		return S + TEXT("\n");
	}

	void FlushCapture()
	{
		if (CapturePending.IsEmpty() || CaptureFile.IsEmpty()) return;
		FFileHelper::SaveStringToFile(CapturePending, *CaptureFile, FFileHelper::EEncodingOptions::AutoDetect, &IFileManager::Get(), FILEWRITE_Append);
		CapturePending.Reset();
		CaptureRowsPending = 0;
	}

	void SetCapture(bool bOn)
	{
		if (bOn == bCapturing || PerfDir.IsEmpty()) return;
		if (bOn)
		{
			CaptureFile = FPaths::Combine(PerfDir, FString::Printf(TEXT("capture_%s.csv"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"))));
			FFileHelper::SaveStringToFile(CaptureHeader(), *CaptureFile);
			CaptureFrames = 0;
			CaptureStart = FPlatformTime::Seconds();
			bCapturing = true;
			UE_LOG(LogOutlaws, Log, TEXT("OLPERF capture started: %s"), *CaptureFile);
		}
		else
		{
			// (the header again at the end: a bucket first run during the capture adds a column to the rows after it)
			CapturePending += CaptureHeader();
			FlushCapture();
			bCapturing = false;
			UE_LOG(LogOutlaws, Log, TEXT("OLPERF capture stopped: %lld frames over %.0f s in %s"), CaptureFrames, FPlatformTime::Seconds() - CaptureStart, *CaptureFile);
		}
	}

	void CaptureRow(double T, const FPerfFrame& F, int32 Slot, bool bPlaying, double GcMs, int32 Tex)
	{
		const AOutlawsLevelActor* L = LevelRef.Get();
		CapturePending += FString::Printf(TEXT("%.3f,%s,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%d,%d,%d,%.2f,%.2f,%d"), T, L ? *L->LevelName : TEXT(""),
			F.FrameMs, F.GameMs, F.RenderMs, F.RhiMs, F.GpuMs, F.Draws, F.Prims, bPlaying ? 1 : 0, IsLoadFrame() ? 1 : 0, StartMsLast, GcMs, Tex);
		{
			FScopeLock Lock(&BucketLock);
			for (const FBucketStat& B : BucketStats) CapturePending += FString::Printf(TEXT(",%.3f"), B.Recent[Slot]);
		}
		CapturePending += TEXT("\n");
		++CaptureFrames;
		if (++CaptureRowsPending >= 144) FlushCapture();
	}

	void OnEndFrame()
	{
		const double Now = FPlatformTime::Seconds();
		HeartbeatFrame.store(GFrameCounter);
		HeartbeatTime.store(Now);
		if (LastEnd <= 0.0)
		{
			LastEnd = SecStart = LiveStart = Now;
			MinuteSum.Start = LevelSum.Start = Now;
			return;
		}
		if (PerfDir.IsEmpty())
		{
			// (-OLNoPerfFiles: the summary lines alone, the writer's cost left out to compare against)
			FPerfFrame F;
			F.FrameMs = (Now - LastEnd) * 1000.0;
			LastEnd = Now;
			F.GameMs = Ms(GGameThreadTime);
			F.RenderMs = Ms(GRenderThreadTime);
			F.RhiMs = Ms(GRHIThreadTime);
			F.GpuMs = Ms(RHIGetGPUFrameCycles(0));
			if (InPlay())
			{
				if (LevelSum.Frames == 0 && LevelRef.IsValid()) SummaryLevel = LevelRef->LevelName;
				MinuteSum.Add(F);
				LevelSum.Add(F);
			}
			if (Now - SecStart >= 1.0)
			{
				FindActors();
				SecStart = Now;
			}
			if (Now - MinuteSum.Start >= SummarySeconds) LogSummary(TEXT("last 60 s"), MinuteSum);
			return;
		}
		FPerfFrame F;
		F.FrameMs = (Now - LastEnd) * 1000.0;
		LastEnd = Now;
		F.GameMs = Ms(GGameThreadTime);
		F.RenderMs = Ms(GRenderThreadTime);
		F.RhiMs = Ms(GRHIThreadTime);
		F.GpuMs = Ms(RHIGetGPUFrameCycles(0));
		F.Draws = GNumDrawCallsRHI[0];
		F.Prims = GNumPrimitivesDrawnRHI[0];
		bGpuTiming |= F.GpuMs > 0.0;
		const bool bPlaying = InPlay();
		if (bPlaying)
		{
			++SessionFrames;
			if (LevelSum.Frames == 0 && LevelRef.IsValid()) SummaryLevel = LevelRef->LevelName;
			MinuteSum.Add(F);
			LevelSum.Add(F);
		}
		FrameRing[RingNext] = F;
		const int32 Slot = RingNext;
		RingNext = (RingNext + 1) % SmoothFrames;
		RingCount = FMath::Min(RingCount + 1, SmoothFrames);
		{
			FScopeLock Lock(&BucketLock);
			for (FBucketStat& B : BucketStats)
			{
				const double V = FPlatformTime::ToMilliseconds64(B.FrameCycles);
				B.FrameCycles = 0;
				B.Recent[Slot] = V;
				B.SecSum += V; B.SecPeak = FMath::Max(B.SecPeak, V);
				B.LiveSum += V; B.LivePeak = FMath::Max(B.LivePeak, V);
				if (bPlaying) { B.SessionSum += V; B.SessionPeak = FMath::Max(B.SessionPeak, V); }
#if CSV_PROFILER
				FCsvProfiler::RecordCustomStat(B.CsvName, CSV_CATEGORY_INDEX(Outlaws), float(V), ECsvCustomStatOp::Set);
#endif
			}
		}
		SecWindow.Add(F);
		LiveWindow.Add(F);

		// a stutter: this one frame over twice the budget (and over the budget by 12 ms at least), not a load
		{
			const double Budget = 1000.0 / FMath::Max(1.f, CVarTargetFps.GetValueOnGameThread());
			TArray<FString> Events;
			{
				FScopeLock Lock(&TrailLock);
				Events = MoveTemp(FrameEvents);
				FrameEvents.Reset();
			}
			const bool bGc = bGcThisFrame.exchange(false);
			const double GcMs = GcFrameMs;
			GcFrameMs = 0.0;
			const int32 Tex = TexturesMadeTotal - TexturesAtFrameStart;
			TexturesAtFrameStart = TexturesMadeTotal;
			if (bCapturing) CaptureRow(Now - SessionStart, F, Slot, bPlaying, GcMs, Tex);
			if (bPlaying && F.FrameMs > FMath::Max(2.0 * Budget, Budget + 12.0) && F.FrameMs < MaxSaneMs)
			{
				FString Top = TEXT("-");
				double TopMs = 0.0;
				{
					FScopeLock Lock(&BucketLock);
					for (const FBucketStat& B : BucketStats) if (B.Recent[Slot] > TopMs) { TopMs = B.Recent[Slot]; Top = B.Name; }
				}
				FString What;
				{
					// (CPU1) the costliest bucket that is not a "(total)", so a hitch names the part of a total that spiked
					FScopeLock Lock(&BucketLock);
					FString Inner;
					double InnerMs = 0.0;
					for (const FBucketStat& B : BucketStats) if (!B.Name.Contains(TEXT("(total)")) && B.Recent[Slot] > InnerMs) { InnerMs = B.Recent[Slot]; Inner = B.Name; }
					if (InnerMs > 1.0) What += FString::Printf(TEXT("inner %s %.1f ms | "), *Inner, InnerMs);
				}
				if (bGc) What += GcMs > 0.0 ? FString::Printf(TEXT("GC %.1f ms | "), GcMs) : FString(TEXT("GC | "));
				if (Tex > 0) What += FString::Printf(TEXT("%d textures made | "), Tex);
				for (const FString& E : Events) What += E + TEXT(" | ");
				HitchRow(Now - SessionStart, TEXT("hitch"), &F, Top, TopMs, What.IsEmpty() ? FString(TEXT("(nothing logged)")) : What.LeftChop(3));
				++HitchesSession;
			}
			else if (!bPlaying && IsLoadFrame() && F.FrameMs > FMath::Max(2.0 * Budget, Budget + 12.0))
			{
				// (TEXLOAD) the level build's frame: a row of its own kind, out of play's numbers
				HitchRow(Now - SessionStart, TEXT("load"), &F, TEXT("level load"), LastLoadMs, FString::Printf(TEXT("level load %.0f ms, %d textures made"), LastLoadMs, Tex));
			}
		}

		// a lag moment: the smoothed frame time well over the headset's budget, in live play only
		const double BudgetMs = 1000.0 / FMath::Max(1.f, CVarTargetFps.GetValueOnGameThread());
		double Smoothed = 0.0;
		for (int32 i = 0; i < RingCount; ++i) Smoothed += FrameRing[i].FrameMs / RingCount;
		if (RingCount == SmoothFrames && Smoothed >= BudgetMs * DropTrigger && F.FrameMs < MaxSaneMs && Now - LastLag > CooloffSeconds && InPlay())
		{
			LastLag = Now;
			const double Severity = Smoothed / BudgetMs;
			if (Scenarios.Num() < MaxScenarios || Severity > Scenarios.Last().Severity)
			{
				Scenarios.Add({Severity, TakeSnapshot(TEXT("lag"), Smoothed, BudgetMs)});
				Scenarios.Sort([](const FSnapshot& A, const FSnapshot& B) { return A.Severity > B.Severity; });
				if (Scenarios.Num() > MaxScenarios) Scenarios.SetNum(MaxScenarios);
				WritePerformance();       // at once: lag often comes just before the crash we want it for
			}
		}

		if (Now - SecStart >= 1.0)
		{
			FindActors();
			WriteTimelineRow(Now - SessionStart);
#if !UE_BUILD_SHIPPING
			// (the hang watchdog's copy; Shipping has no watchdog)
			const FString Ctx = DescribeScene() + TEXT("\n") + DescribeRecent() + TEXT("\n") + DescribeMemory() + TEXT("\n") + DescribeSettings();
			{
				FScopeLock Lock(&ContextLock);
				ContextText = Ctx;
			}
#endif
			// (PERF-SHIPSUMMARY) the detailed capture's switches, once a second
			static const FString CaptureOn = FPaths::Combine(PerfDir, TEXT("capture.on"));
			SetCapture(bCaptureFlag || CVarCapture.GetValueOnGameThread() != 0 || IFileManager::Get().FileExists(*CaptureOn));
			SecWindow = FWindow();
			SecStart = Now;
		}
		if (Now - LiveStart >= 5.0)
		{
			WriteLive(Now - LiveStart);
			LiveWindow = FWindow();
			LiveStart = Now;
		}
		if (Now - MinuteSum.Start >= SummarySeconds) LogSummary(TEXT("last 60 s"), MinuteSum);
	}

	/** LogOutlaws, LogOutlawsCoop and LogOutlawsAI (and the RHI / XR warnings) into the hang trail. */
	class FTrailCapture : public FOutputDevice
	{
	public:
		using FOutputDevice::Serialize;
		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override
		{
			static const FName Outlaws(TEXT("LogOutlaws"));
			static const FName OutlawsCoop(TEXT("LogOutlawsCoop"));
			static const FName OutlawsAI(TEXT("LogOutlawsAI"));
			static const FName Rhi(TEXT("LogRHI"));
			static const FName Vulkan(TEXT("LogVulkanRHI"));
			static const FName Hmd(TEXT("LogHMD"));
			static const FName Load(TEXT("LogLoad"));
			const bool bWarn = Verbosity <= ELogVerbosity::Warning;
			if (Category == Outlaws || Category == OutlawsCoop || Category == OutlawsAI || Category == Load || (bWarn && (Category == Rhi || Category == Vulkan || Category == Hmd)))
			{
				OutlawsPerf::Breadcrumb(FString::Printf(TEXT("%s: %s"), *Category.ToString(), V));
			}
		}
		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }
	};
	FTrailCapture TrailCapture;

	/** Notices the game thread stopping (a GPU hang: it spins waiting on the GPU) and writes hang.txt. */
	class FHangWatch : public FRunnable
	{
	public:
		std::atomic<bool> bStop{false};
		virtual uint32 Run() override
		{
			uint64 Reported = MAX_uint64;
			while (!bStop.load())
			{
				FPlatformProcess::Sleep(0.5f);
				const double Last = HeartbeatTime.load();
				const uint64 Frame = HeartbeatFrame.load();
				if (Last <= 0.0 || Frame == Reported || LoadingDepth.load() > 0 || bAppPaused.load()) continue;
				const double Stalled = FPlatformTime::Seconds() - Last;
				if (Stalled < HangSeconds) continue;
				Reported = Frame;
				FString S = FString::Printf(TEXT("=== %s: the game thread has not finished a frame for %.0f s (stuck after frame %llu, %.0f s into the session) ===\n"),
					*FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")), Stalled, Frame, Last - SessionStart);
				{
					FScopeLock Lock(&ContextLock);
					S += TEXT("last context (up to 1 s old):\n") + ContextText + TEXT("\n");
				}
				S += TEXT("trail, oldest first:\n");
				{
					FScopeLock Lock(&TrailLock);
					for (int32 i = 0; i < Trail.Num(); ++i)
					{
						const FString& L = Trail[(TrailNext + i) % Trail.Num()];
						if (!L.IsEmpty()) S += TEXT("  ") + L + TEXT("\n");
					}
				}
				FFileHelper::SaveStringToFile(S + TEXT("\n"), *FPaths::Combine(PerfDir, TEXT("hang.txt")), FFileHelper::EEncodingOptions::AutoDetect,
					&IFileManager::Get(), FILEWRITE_Append);
				FPlatformMisc::LowLevelOutputDebugStringf(TEXT("OLPERF hang: game thread stuck %.0f s after frame %llu; see Saved/Perf/hang.txt"), Stalled, Frame);
			}
			return 0;
		}
		virtual void Stop() override { bStop.store(true); }
	};
	void OnAppPause() { bAppPaused.store(true); }
	void OnAppResume()
	{
		HeartbeatTime.store(FPlatformTime::Seconds());
		bAppPaused.store(false);
	}

	FHangWatch* HangWatch = nullptr;
	FRunnableThread* HangThread = nullptr;

	FAutoConsoleCommand CmdMark(TEXT("ol.perf.mark"), TEXT("Record a manual lag mark into Saved/Perf/performance.txt."),
		FConsoleCommandDelegate::CreateLambda([]() { OutlawsPerf::Mark(TEXT("console")); }));
	FAutoConsoleCommand CmdDump(TEXT("ol.perf.dump"), TEXT("Rewrite Saved/Perf/perf_live.txt now."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			if (!PerfDir.IsEmpty()) WriteLive(FPlatformTime::Seconds() - LiveStart);
		}));
}

int32 OutlawsPerf::Bucket(const TCHAR* Name)
{
	FScopeLock Lock(&BucketLock);
	for (int32 i = 0; i < BucketStats.Num(); ++i)
	{
		if (BucketStats[i].Name == Name) return i;
	}
	FBucketStat& B = BucketStats.AddDefaulted_GetRef();
	B.Name = Name;
	B.CsvName = FName(Name);
	return BucketStats.Num() - 1;
}

double OutlawsPerf::LastFrameMs(const TCHAR* Name)
{
	FScopeLock Lock(&BucketLock);
	const int32 Slot = (RingNext + SmoothFrames - 1) % SmoothFrames;
	for (const FBucketStat& B : BucketStats) if (B.Name == Name) return B.Recent[Slot];
	return 0.0;
}

void OutlawsPerf::Add(int32 BucketIndex, uint64 Cycles)
{
	if (!IsInGameThread()) return;
	FScopeLock Lock(&BucketLock);
	if (BucketStats.IsValidIndex(BucketIndex)) BucketStats[BucketIndex].FrameCycles += Cycles;
}

void OutlawsPerf::Breadcrumb(const FString& What)
{
	FScopeLock Lock(&TrailLock);
#if UE_BUILD_SHIPPING
	// (PERF-SHIPSUMMARY: OutlawsShipLog hands the game's lines here; no hang watchdog in Shipping, so no trail, only what
	// this frame did for hitches.csv)
	if (What != TrailLast && FrameEvents.Num() < 6) FrameEvents.Add(What.Left(110));
	TrailLast = What;
#else
	if (Trail.Num() == 0) Trail.SetNum(TrailSize);
	// a repeated line (the RHI's 0.5 s GPU wait warning) is counted, not stored again
	if (What == TrailLast)
	{
		++TrailRepeat;
		const int32 Prev = (TrailNext + TrailSize - 1) % TrailSize;
		Trail[Prev] = FString::Printf(TEXT("%s  (x%d)"), *What, TrailRepeat + 1);
		return;
	}
	TrailLast = What;
	if (FrameEvents.Num() < 6) FrameEvents.Add(What.Left(110));       // (BUG-STUTTER: what this frame did)
	TrailRepeat = 0;
	Trail[TrailNext] = FString::Printf(TEXT("%8.2f f%llu %s"), FPlatformTime::Seconds() - SessionStart, GFrameCounter, *What);
	TrailNext = (TrailNext + 1) % TrailSize;
#endif
}

void OutlawsPerf::Mark(const TCHAR* Why)
{
	if (PerfDir.IsEmpty() || !IsInGameThread()) return;
	double Smoothed = 0.0;
	for (int32 i = 0; i < RingCount; ++i) Smoothed += FrameRing[i].FrameMs / RingCount;
	Marks.Add(TakeSnapshot(*FString::Printf(TEXT("mark (%s)"), Why), Smoothed, 1000.0 / FMath::Max(1.f, CVarTargetFps.GetValueOnGameThread())));
	if (Marks.Num() > MaxMarks) Marks.RemoveAt(0);
	// (the stutter probe: the mark in hitches.csv too, so the player's "there!" sits among the long frames around it)
	HitchRow(FPlatformTime::Seconds() - SessionStart, TEXT("MARK"), nullptr, FString(TEXT("-")), 0.0, FString::Printf(TEXT("mark %d (%s), %d hitches so far"), Marks.Num(), Why, HitchesSession));
	FlushHitches();
	FlushTimeline();
	WritePerformance();
	UE_LOG(LogOutlaws, Log, TEXT("OLPERF mark %d (%s)"), Marks.Num(), Why);
}

bool OutlawsPerf::IsLoading()
{
	return LoadingDepth.load() > 0;
}

OutlawsPerf::FLoading::FLoading()
{
	if (LoadingDepth.fetch_add(1) == 0)
	{
		LoadStartTime = FPlatformTime::Seconds();
		if (IsInGameThread()) LogLevelSummary(TEXT("left (a level build starts)"));
	}
}

OutlawsPerf::FLoading::~FLoading()
{
	// the frame the load ends counts as a fresh heartbeat, or the watchdog would see the load's length at once
	HeartbeatTime.store(FPlatformTime::Seconds());
	if (LoadingDepth.fetch_sub(1) == 1)
	{
		LastLoadMs = (FPlatformTime::Seconds() - LoadStartTime) * 1000.0;
		LoadEndFrame.store(GFrameCounter);
		bLoadEnded.store(true);
		bLoadSinceRow.store(true);
	}
}

bool OutlawsPerf::FreezeMeshUpdates()
{
	return CVarFreezeMeshUpdates.GetValueOnGameThread() != 0;
}

FString OutlawsPerf::DrawCensus(UWorld* World)
{
	if (!World) return TEXT("no world");
	TMap<FString, int32> Draws, Parts, Seen;
	int32 Total = 0, SeenTotal = 0;
	FString WorldSplit;
	// (the view as the renderer sees it: what a cluster of the world mesh split up would have in front of it)
	FConvexVolume Frustum;
	bool bFrustum = false;
	if (APlayerController* Pc = World->GetFirstPlayerController())
	{
		ULocalPlayer* Lp = Pc->GetLocalPlayer();
		FSceneViewProjectionData Pd;
		if (Lp && Lp->ViewportClient && Lp->ViewportClient->Viewport && Lp->GetProjectionData(Lp->ViewportClient->Viewport, Pd))
		{
			GetViewFrustumBounds(Frustum, Pd.ComputeViewProjectionMatrix(), false);
			bFrustum = true;
		}
	}
	for (TObjectIterator<UPrimitiveComponent> It; It; ++It)
	{
		UPrimitiveComponent* C = *It;
		if (C->GetWorld() != World || !C->IsRegistered() || !C->IsVisible() || C->bHiddenInGame) continue;
		const AActor* Owner = C->GetOwner();
		if (Owner && Owner->IsHidden()) continue;
		int32 N = 0;
		if (UProceduralMeshComponent* Pm = Cast<UProceduralMeshComponent>(C))
		{
			for (int32 s = 0; s < Pm->GetNumSections(); ++s)
			{
				const FProcMeshSection* Sec = Pm->GetProcMeshSection(s);
				if (Sec && Sec->bSectionVisible && Sec->ProcIndexBuffer.Num() > 0) ++N;
			}
		}
		else if (const UStaticMeshComponent* Sm = Cast<UStaticMeshComponent>(C))
		{
			N = Sm->GetStaticMesh() ? FMath::Max(1, Sm->GetNumMaterials()) : 0;
		}
		else continue;                 // (capsules, arrows, text: no mesh draws counted)
		if (N == 0) continue;
		const FString Name = C->GetName();
		FString Kind;
		if (Owner && Owner->IsA<AOutlawsEnemy>()) Kind = TEXT("enemies");
		else if (Owner && Owner->IsA<AOutlawsSpriteActor>()) Kind = C->IsA<UProceduralMeshComponent>() ? TEXT("item models") : Name.Contains(TEXT("Quad")) ? TEXT("sprites") : TEXT("sprite extras");
		else if (Owner && Owner->IsA<AOutlawsPlayer>()) Kind = TEXT("player");
		else if (Name.StartsWith(TEXT("OutlawsWorld"))) Kind = TEXT("world");
		else if (Name.StartsWith(TEXT("OutlawsWater"))) Kind = TEXT("water");
		else if (Name.StartsWith(TEXT("OutlawsSky")) || Name.Contains(TEXT("Sky"))) Kind = TEXT("sky");
		else if (Name.StartsWith(TEXT("OutlawsDoor")) || Name.StartsWith(TEXT("OutlawsLift"))) Kind = TEXT("movers");
		else if (Name.StartsWith(TEXT("OutlawsSector"))) Kind = TEXT("overlap sectors");
		else if (Name.StartsWith(TEXT("OutlawsScroll"))) Kind = TEXT("scrolls");
		else if (Name.StartsWith(TEXT("OutlawsGlass"))) Kind = TEXT("panes");
		else if (Name.StartsWith(TEXT("Outlaws3DO"))) Kind = TEXT("3DO");
		else if (Name.StartsWith(TEXT("OutlawsExplode"))) Kind = TEXT("explodable walls");
		else if (Name.StartsWith(TEXT("OutlawsDecal")) || Name.StartsWith(TEXT("OutlawsShards"))) Kind = TEXT("decals and shards");
		else if (Owner && Owner->IsA<AOutlawsLevelActor>()) Kind = TEXT("level other");
		else Kind = FString::Printf(TEXT("other (%s)"), Owner ? *Owner->GetClass()->GetName() : TEXT("-"));
		Draws.FindOrAdd(Kind) += N;
		Parts.FindOrAdd(Kind) += 1;
		Total += N;
		// drawn in the last frames by the renderer (its own culling of the component's bounds), not only shown
		if (C->WasRecentlyRendered(0.2f)) { Seen.FindOrAdd(Kind) += N; SeenTotal += N; }
		if (Kind == TEXT("world") && bFrustum && !Name.StartsWith(TEXT("OutlawsWorldPart")))
		{
			// the world mesh split by a square grid (a section per texture per cell): how many such parts the view would keep
			UProceduralMeshComponent* Pm = Cast<UProceduralMeshComponent>(C);
			const FTransform Xf = Pm->GetComponentTransform();
			for (const double Cell : { 1024.0, 2048.0, 4096.0 })
			{
				int32 Cells = 0, InView = 0;
				for (int32 s = 0; s < Pm->GetNumSections(); ++s)
				{
					const FProcMeshSection* Sec = Pm->GetProcMeshSection(s);
					if (!Sec || !Sec->bSectionVisible) continue;
					TMap<FIntPoint, FBox> Boxes;
					const TArray<uint32>& Ix = Sec->ProcIndexBuffer;
					for (int32 i = 0; i + 2 < Ix.Num(); i += 3)
					{
						const FVector A = Xf.TransformPosition(FVector(Sec->ProcVertexBuffer[Ix[i]].Position));
						const FVector B = Xf.TransformPosition(FVector(Sec->ProcVertexBuffer[Ix[i + 1]].Position));
						const FVector Cv = Xf.TransformPosition(FVector(Sec->ProcVertexBuffer[Ix[i + 2]].Position));
						const FVector Mid = (A + B + Cv) / 3.0;
						FBox& Bx = Boxes.FindOrAdd(FIntPoint(FMath::FloorToInt(Mid.X / Cell), FMath::FloorToInt(Mid.Y / Cell)), FBox(ForceInit));
						Bx += A; Bx += B; Bx += Cv;
					}
					Cells += Boxes.Num();
					for (const TPair<FIntPoint, FBox>& Kv : Boxes) if (Frustum.IntersectBox(Kv.Value.GetCenter(), Kv.Value.GetExtent())) ++InView;
				}
				WorldSplit += FString::Printf(TEXT(" %.0fm cells: %d parts, %d in view;"), Cell / 100.0, Cells, InView);
			}
			// the world mesh split by texture instead (a part per section, culled by its own bounds): how many the view keeps,
			// and how many of those also touch a sector V1 draws now (each face's sector from its middle, nudged off the face)
			TActorIterator<AOutlawsLevelActor> Lv(World);
			int32 TexInView = 0, TexInViewV1 = 0;
			for (int32 s = 0; s < Pm->GetNumSections(); ++s)
			{
				const FProcMeshSection* Sec = Pm->GetProcMeshSection(s);
				if (!Sec || !Sec->bSectionVisible || Sec->ProcIndexBuffer.Num() == 0) continue;
				const FBox Bx = Sec->SectionLocalBox.TransformBy(Xf);
				if (!Frustum.IntersectBox(Bx.GetCenter(), Bx.GetExtent())) continue;
				++TexInView;
				bool bV1 = !Lv;
				const TArray<uint32>& Ix = Sec->ProcIndexBuffer;
				for (int32 i = 0; !bV1 && i + 2 < Ix.Num(); i += 3)
				{
					const FProcMeshVertex& A = Sec->ProcVertexBuffer[Ix[i]];
					const FVector Mid = Xf.TransformPosition(FVector(A.Position + Sec->ProcVertexBuffer[Ix[i + 1]].Position + Sec->ProcVertexBuffer[Ix[i + 2]].Position) / 3.0
						+ FVector(A.Normal) * 8.0);
					const int32 Sx = Lv->CullSectorOf(Mid);
					bV1 = Sx < 0 || Lv->DebugSectorDrawn(Sx);
				}
				if (bV1) ++TexInViewV1;
			}
			WorldSplit += FString::Printf(TEXT(" by texture: %d parts, %d in view, %d in view in V1's sectors;"), Pm->GetNumSections(), TexInView, TexInViewV1);
		}
	}
	Draws.ValueSort([](int32 A, int32 B) { return A > B; });
	FString S = FString::Printf(TEXT("RHI draws last frame %d, triangles %d; shown sections/materials %d:"), GNumDrawCallsRHI[0], GNumPrimitivesDrawnRHI[0], Total);
	for (const TPair<FString, int32>& Kv : Draws) S += FString::Printf(TEXT(" %s %d (%d parts, %d rendered),"), *Kv.Key, Kv.Value, Parts.FindRef(Kv.Key), Seen.FindRef(Kv.Key));
	S = S.LeftChop(1) + FString::Printf(TEXT("; rendered %d; world split by grid (frustum only):%s"), SeenTotal, *WorldSplit);
	return S.LeftChop(1);
}

void OutlawsPerf::CountTexture(int32 W, int32 H)
{
	if (!IsInGameThread()) return;
	++TexturesMadeSec;
	++TexturesMadeTotal;
	TexelsMadeSec += int64(W) * H;
}

double OutlawsPerf::XRWaitMs()
{
	return StartMsLast;
}

void OutlawsPerf::Startup()
{
	if (GIsEditor || IsRunningCommandlet() || !FApp::CanEverRender()) return;
	// (PERF-SHIPSUMMARY: the files in Shipping too, where adb reaches them: on the Quest the external Saved that
	// OutlawsShipLog writes its log beside, Saved/Perf on the PC; -OLNoPerfFiles leaves them out and keeps the summary lines.
	// The hang watchdog stays Development's.)
	if (!OLFlags::Param(TEXT("OLNoPerfFiles")))
	{
		PerfDir = FPaths::ConvertRelativePathToFull(FPaths::Combine(OLShipLog::ReportSavedDir(), TEXT("Perf")));
		OLFlags::Value(TEXT("OLPerfDir"), PerfDir);      // (a test's: two instances on one tree, the co-op bench)
		IFileManager::Get().MakeDirectory(*PerfDir, true);
		FFileHelper::LoadFileToString(PriorPerformance, *FPaths::Combine(PerfDir, TEXT("performance.txt")));
		if (PriorPerformance.Len() > 200000) PriorPerformance.LeftInline(200000);
		const FString Timeline = FPaths::Combine(PerfDir, TEXT("perf_timeline.csv"));
		// (only when there is one: on a fresh install the move retried a missing file for about 5 s at startup)
		if (IFileManager::Get().FileExists(*Timeline))
		{
			IFileManager::Get().Move(*FPaths::Combine(PerfDir, TEXT("perf_timeline_prev.csv")), *Timeline, true, true);
		}
		FFileHelper::SaveStringToFile(TEXT("t_s,level,x,y,z,yaw,fps,frame_ms,frame_max_ms,game_ms,render_ms,rhi_ms,gpu_ms,gpu_max_ms,draws,tris,mem_mb,textures_made,mtexels_made,top_bucket,top_bucket_ms,in_play\n"), *Timeline);
		// (CPU1 step 0a) every bucket each second, and the counts behind them
		for (const TCHAR* Name : { TEXT("perf_buckets.csv"), TEXT("perf_counts.csv") })
		{
			const FString F = FPaths::Combine(PerfDir, Name);
			if (IFileManager::Get().FileExists(*F)) IFileManager::Get().Move(*FPaths::Combine(PerfDir, FString(Name).Replace(TEXT(".csv"), TEXT("_prev.csv"))), *F, true, true);
		}
		FFileHelper::SaveStringToFile(TEXT("t_s,level,bucket,avg_ms,peak_ms,in_play\n"), *FPaths::Combine(PerfDir, TEXT("perf_buckets.csv")));
		FFileHelper::SaveStringToFile(TEXT("t_s,level,actors,actors_ticking,sprites,sprites_shown,sprites_ticking,enemies,enemies_awake,meshes,meshes_shown,sections_shown,sectors_visible,mover_groups,gcs,throttle_level,throttle_game_ms,throttle_xr_wait_ms,throttle_eff_ms\n"),
			*FPaths::Combine(PerfDir, TEXT("perf_counts.csv")));
		{
			const FString Hitches = FPaths::Combine(PerfDir, TEXT("hitches.csv"));
			if (IFileManager::Get().FileExists(*Hitches)) IFileManager::Get().Move(*FPaths::Combine(PerfDir, TEXT("hitches_prev.csv")), *Hitches, true, true);
			FFileHelper::SaveStringToFile(TEXT("t_s,kind,level,x,y,z,frame_ms,game_ms,render_ms,rhi_ms,gpu_ms,top_bucket,top_bucket_ms,what_happened\n"), *Hitches);
		}
		bCaptureFlag = OLFlags::Param(TEXT("OLPerfCapture"));
	}
	FCoreUObjectDelegates::GetPreGarbageCollectDelegate().AddStatic(&OnPreGc);
	FCoreUObjectDelegates::GetPostGarbageCollect().AddStatic(&OnPostGc);
	FWorldDelegates::OnWorldTickStart.AddStatic(&OnWorldTickStart);
	FWorldDelegates::OnWorldPreActorTick.AddStatic(&OnWorldPreActorTick);
	FWorldDelegates::OnWorldPostActorTick.AddStatic(&OnWorldPostActorTick);
	FWorldDelegates::OnWorldTickEnd.AddStatic(&OnWorldTickEnd);
	SessionStamp = FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S"));
	SessionStart = FPlatformTime::Seconds();
	bSummaryOn = true;
	EndFrameHandle = FCoreDelegates::OnEndFrame.AddStatic(&OnEndFrame);
#if UE_BUILD_SHIPPING
	UE_LOG(LogOutlaws, Log, TEXT("OLPERF on (Shipping): %s; summary lines every %.0f s and at each level's end; no hang watchdog"),
		PerfDir.IsEmpty() ? TEXT("no files (-OLNoPerfFiles)") : *PerfDir, SummarySeconds);
#else
	GLog->AddOutputDevice(&TrailCapture);
	// Android sends these when the Quest pauses the app (headset off, the dashboard): the game thread stops then too
	FCoreDelegates::ApplicationWillDeactivateDelegate.AddStatic(&OnAppPause);
	FCoreDelegates::ApplicationWillEnterBackgroundDelegate.AddStatic(&OnAppPause);
	FCoreDelegates::ApplicationHasReactivatedDelegate.AddStatic(&OnAppResume);
	FCoreDelegates::ApplicationHasEnteredForegroundDelegate.AddStatic(&OnAppResume);
	if (!PerfDir.IsEmpty())
	{
		HangWatch = new FHangWatch();
		HangThread = FRunnableThread::Create(HangWatch, TEXT("OutlawsHangWatch"), 0, TPri_BelowNormal);
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLPERF on: %s"), PerfDir.IsEmpty() ? TEXT("no files (-OLNoPerfFiles)") : *PerfDir);
#endif
}

void OutlawsPerf::Shutdown()
{
	if (!bSummaryOn) return;
	FCoreDelegates::OnEndFrame.Remove(EndFrameHandle);
	LogLevelSummary(TEXT("left (the game ends)"));
	bSummaryOn = false;
	if (PerfDir.IsEmpty()) return;
	SetCapture(false);
	if (GLog) GLog->RemoveOutputDevice(&TrailCapture);
	if (HangThread)
	{
		HangThread->Kill(true);
		delete HangThread;
		HangThread = nullptr;
	}
	delete HangWatch;
	HangWatch = nullptr;
	FlushHitches();
	FlushTimeline();
	WritePerformance();
	PerfDir.Empty();
}
