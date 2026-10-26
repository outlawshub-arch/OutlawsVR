// BUG-RESPAWNCRASH (2026-10-10): the Quest 3 ran out of memory over a long single player session.  Every in-place level
// rebuild (a level change, Restart Level, a death respawn) kept about 75 MB after its two collections (873 to 1701 MB over
// 11 rebuilds), and a respawn's rebuild tipped the system into thrashing (tools/olue/out/respawncrash/FINDINGS.md).
// -OLMemSoak[=plan] rebuilds the level step by step and logs the memory after each (OLMEM lines):
//   r        Restart Level (a reload of the same level)
//   d        a death, then the respawn (the player's own RequestRespawn)
//   j<name>  a jump to that level (a level change)
//   n<name>  the level won, the story on to that level with the kit carried
//   l        the autosave written and loaded
//   ho       (BUG-RESPAWNCRASH HM) a Historical Missions session in the office, its score seeded at 9000 (Deputy at 9300)
//   hm<mission>/<sector>  that mission's poster ridden (the way back to that office sector)
//   hw       the mission won: its outlaw knocked out with a fist, laid by the player, the horse, back to the office
// With h steps every step from ho on checks the session ("OLMEM hom check"): still a session, the mission and the way back
// kept, a death or restart on the mission's roll, the stamp, score and rank after a win; and with -OLMemLoadMapAbove each
// rebuild a real map load whose memory came down.  "OLMEM hom PASS|FAIL" at the end.
// The default plan is five reloads.  After each step the level settles 3 s, is collected twice, and a snapshot is logged:
// the process's resident memory (the working set on a PC, VmRSS on the Quest), the Quest's free swap, the textures, meshes,
// sounds and our caches, and which classes of object grew since the last.  The table at the end is the empty baseline (each
// rebuild's memory after the old level's two collections, before its build); -OLMemSoakTolerance=<MB> (50) is how far it
// may move from the third rebuild on, "OLMEM soak PASS|FAIL".
// The safety net (ol.mem.LoadMapAboveMB, 1300 on the Quest): when the memory with the old level collected is still over it
// before a rebuild's build, the rebuild is written as a level start (the save's own "opening" form, or the save being
// loaded) and L_Runtime is loaded for real (a map load gives back what the in-place rebuilds keep); the new world builds it.
// A historical mission's session (single player) goes with it in the same JSON (WriteHomCarry); co-op and the main menu never.

#include "OutlawsLevelActor.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsPlayer.h"
#include "OutlawsEnemy.h"
#include "OutlawsLab.h"
#include "OutlawsTextures.h"
#include "OutlawsCoop.h"
#include "Kismet/GameplayStatics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/IConsoleManager.h"
#include "ProceduralMeshComponent.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Sound/SoundWave.h"
#include "Components/AudioComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMemory.h"
#include "Misc/CommandLine.h"
#include "RenderingThread.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UObjectArray.h"
#include "HAL/LowLevelMemTracker.h"
#include "RHI.h"
#include "DynamicRHI.h"
#if PLATFORM_ANDROID
#include <stdio.h>
#endif
#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/PreWindowsApi.h"
#include <dxgi1_4.h>
#include "Windows/PostWindowsApi.h"
#include "Windows/HideWindowsPlatformTypes.h"
#undef LoadImage          // (a unity file's later FImageUtils::LoadImage)
#endif

int64 OlItemCacheBytes(int32& OutTemplates, int32& OutModels);

namespace
{
	uint32 GSoakGcCount = 0;
	TSharedPtr<FJsonObject> GMapLoadRebuild;      // a rebuild handed from one world to the next by a map load
	int32 GMapLoads = 0;
	double GMapLoadFromMB = 0.0, GMapLoadToMB = 0.0;     // (the soak) the last map load's memory as it fired and once done
	double GLastGpuBefore = -1.0;
	TMap<FName, int32> GSoakClassCounts[2];  // the last snapshot's objects by class, for what grew (settled, before a build)
	TMap<FString, int64> GSoakLlm[2];        // (-LLM) the last snapshot's memory by tag (the same two kinds)

	// (-LLM) the native memory by the engine's tags, the ones that moved most since the last snapshot
	void LogLlmDeltas(const FString& Tag, int32 Kind)
	{
#if ENABLE_LOW_LEVEL_MEM_TRACKER
		if (!FLowLevelMemTracker::IsEnabled()) return;
		FLowLevelMemTracker& T = FLowLevelMemTracker::Get();
		TMap<FString, int64> Now;
		for (const UE::LLMPrivate::FTagData* D : T.GetTrackedTags(ELLMTracker::Default))
		{
			Now.Add(T.GetTagDisplayPathName(D), T.GetTagAmountForTracker(ELLMTracker::Default, D));
		}
		for (const UE::LLMPrivate::FTagData* D : T.GetTrackedTags(ELLMTracker::Platform))
		{
			Now.Add(TEXT("os:") + T.GetTagDisplayPathName(D), T.GetTagAmountForTracker(ELLMTracker::Platform, D));
		}
		if (GSoakLlm[Kind].Num() > 0)
		{
			TArray<TPair<FString, int64>> Moved;
			for (const TPair<FString, int64>& Kv : Now)
			{
				const int64 D = Kv.Value - GSoakLlm[Kind].FindRef(Kv.Key);
				if (FMath::Abs(D) >= 256 * 1024) Moved.Add(TPair<FString, int64>(Kv.Key, D));
			}
			Moved.Sort([](const TPair<FString, int64>& A, const TPair<FString, int64>& B) { return FMath::Abs(A.Value) > FMath::Abs(B.Value); });
			TArray<FString> Parts;
			for (int32 i = 0; i < Moved.Num() && i < 16; ++i) Parts.Add(FString::Printf(TEXT("%s %+.1f"), *Moved[i].Key, Moved[i].Value / 1048576.0));
			UE_LOG(LogOutlaws, Display, TEXT("OLMEM %s: LLM MB since the last: %s"), *Tag, Parts.Num() ? *FString::Join(Parts, TEXT(", ")) : TEXT("no change"));
		}
		GSoakLlm[Kind] = MoveTemp(Now);
#endif
	}

	double MeminfoMB(const char* Key)
	{
#if PLATFORM_ANDROID
		FILE* F = fopen("/proc/meminfo", "r");
		if (!F) return -1.0;
		char Line[256];
		const size_t KeyLen = strlen(Key);
		double Out = -1.0;
		while (fgets(Line, sizeof(Line), F))
		{
			if (strncmp(Line, Key, KeyLen) == 0 && Line[KeyLen] == ':')
			{
				Out = double(atoll(Line + KeyLen + 1)) / 1024.0;     // (kB)
				break;
			}
		}
		fclose(F);
		return Out;
#else
		return -1.0;
#endif
	}

	// (a PC) this process's video memory in use, local (the card's) and non-local (system memory the card maps), in MB
	void GpuProcessMB(double& OutLocal, double& OutShared)
	{
		OutLocal = OutShared = -1.0;
#if PLATFORM_WINDOWS
		IDXGIFactory1* F = nullptr;
		if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&F)) || !F) return;
		IDXGIAdapter1* A = nullptr;
		if (SUCCEEDED(F->EnumAdapters1(0, &A)) && A)
		{
			IDXGIAdapter3* A3 = nullptr;
			if (SUCCEEDED(A->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&A3)) && A3)
			{
				DXGI_QUERY_VIDEO_MEMORY_INFO L = {}, N = {};
				if (SUCCEEDED(A3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &L))) OutLocal = L.CurrentUsage / 1048576.0;
				if (SUCCEEDED(A3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &N))) OutShared = N.CurrentUsage / 1048576.0;
				A3->Release();
			}
			A->Release();
		}
		F->Release();
#endif
	}

	int64 SectionBytes(const FOutlawsMeshSection& S)
	{
		return S.Positions.GetAllocatedSize() + S.Normals.GetAllocatedSize() + S.UVs.GetAllocatedSize() + S.Colors.GetAllocatedSize() + S.Triangles.GetAllocatedSize();
	}
}

double AOutlawsLevelActor::ResidentMB()
{
	return FPlatformMemory::GetStats().UsedPhysical / 1048576.0;
}

double AOutlawsLevelActor::SwapFreeMB()
{
	return MeminfoMB("SwapFree");
}

void AOutlawsLevelActor::LogMemSnapshot(const FString& Tag, bool bClasses)
{
	const FPlatformMemoryStats M = FPlatformMemory::GetStats();
	// the textures: the GPU's share (all mips), what is still held on the CPU (a transient texture's mips), the render targets
	int32 NTex = 0, NRt = 0, NMesh = 0, NProc = 0, NMid = 0, NWave = 0, NAudio = 0, NActor = 0;
	int64 TexGpu = 0, TexCpu = 0, RtBytes = 0, ProcBytes = 0, WaveBytes = 0;
	for (TObjectIterator<UTexture2D> It; It; ++It)
	{
		if (It->HasAnyFlags(RF_ClassDefaultObject)) continue;
		++NTex;
		TexGpu += It->CalcTextureMemorySizeEnum(TMC_AllMips);
		if (const FTexturePlatformData* PD = It->GetPlatformData())
		{
			for (const FTexture2DMipMap& Mip : PD->Mips)
			{
				if (Mip.BulkData.IsBulkDataLoaded()) TexCpu += Mip.BulkData.GetBulkDataSize();
			}
		}
	}
	for (TObjectIterator<UTextureRenderTarget2D> It; It; ++It)
	{
		if (It->HasAnyFlags(RF_ClassDefaultObject)) continue;
		++NRt;
		RtBytes += It->CalcTextureMemorySizeEnum(TMC_AllMips);
	}
	for (TObjectIterator<UStaticMesh> It; It; ++It) if (!It->HasAnyFlags(RF_ClassDefaultObject)) ++NMesh;
	for (TObjectIterator<UProceduralMeshComponent> It; It; ++It)
	{
		if (It->HasAnyFlags(RF_ClassDefaultObject)) continue;
		++NProc;
		for (int32 i = 0; i < It->GetNumSections(); ++i)
		{
			if (const FProcMeshSection* S = It->GetProcMeshSection(i)) ProcBytes += S->ProcVertexBuffer.GetAllocatedSize() + S->ProcIndexBuffer.GetAllocatedSize();
		}
	}
	for (TObjectIterator<UMaterialInstanceDynamic> It; It; ++It) if (!It->HasAnyFlags(RF_ClassDefaultObject)) ++NMid;
	for (TObjectIterator<USoundWave> It; It; ++It)
	{
		if (It->HasAnyFlags(RF_ClassDefaultObject)) continue;
		++NWave;
		WaveBytes += It->GetResourceSizeBytes(EResourceSizeMode::EstimatedTotal);
	}
	for (TObjectIterator<UAudioComponent> It; It; ++It) if (!It->HasAnyFlags(RF_ClassDefaultObject)) ++NAudio;
	for (TObjectIterator<AActor> It; It; ++It) if (!It->HasAnyFlags(RF_ClassDefaultObject)) ++NActor;
	// our caches
	int32 NTemplates = 0, NModels = 0;
	const int64 TemplateBytes = OlItemCacheBytes(NTemplates, NModels);
	int32 NGround = 0;
	int64 GroundBytes = 0;
	for (const TPair<FString, TMap<FString, FOutlawsMeshSection>>& Kv : GroundModels)
	{
		for (const TPair<FString, FOutlawsMeshSection>& S : Kv.Value) { ++NGround; GroundBytes += SectionBytes(S.Value); }
	}
	int64 WavBytes = 0;
	for (const TPair<FString, TSharedPtr<FWav>>& Kv : WavCache) if (Kv.Value) WavBytes += Kv.Value->Pcm.GetAllocatedSize();
	for (const TPair<FString, TSharedPtr<FWav>>& Kv : MusicWavCache) if (Kv.Value) WavBytes += Kv.Value->Pcm.GetAllocatedSize();
	const double MB = 1048576.0;
	double GpuLocal = -1.0, GpuShared = -1.0;
	GpuProcessMB(GpuLocal, GpuShared);
	if (Tag.StartsWith(TEXT("before"))) GLastGpuBefore = GpuLocal;
	FTextureMemoryStats Rts;
	if (GDynamicRHI) RHIGetTextureMemoryStats(Rts);
	UE_LOG(LogOutlaws, Display, TEXT("OLMEM %s: resident %.0f MB (peak %.0f), private %.0f MB, swap free %.0f MB, available %.0f MB; video memory %.0f MB local, %.0f MB shared, RHI textures %.0f MB; objects %d, actors %d; Texture2D %d (GPU %.1f MB, CPU copies %.1f MB), render targets %d (%.1f MB), StaticMesh %d, ProceduralMesh %d (%.1f MB of vertices), MIDs %d, SoundWave %d (%.1f MB), audio components %d; caches: item templates %d (%.1f MB), item models %d, ground models %d (%d sections, %.1f MB), LAB indexes %d, HD atlases %d, level textures %d lo %d hd %d, sprite atlases %d, sounds %d + music %d (%.1f MB)"),
		*Tag, M.UsedPhysical / MB, M.PeakUsedPhysical / MB, M.UsedVirtual / MB, SwapFreeMB(), M.AvailablePhysical / MB, GpuLocal, GpuShared, Rts.AllocatedMemorySize / MB, GUObjectArray.GetObjectArrayNum(), NActor,
		NTex, TexGpu / MB, TexCpu / MB, NRt, RtBytes / MB, NMesh, NProc, ProcBytes / MB, NMid, NWave, WaveBytes / MB, NAudio,
		NTemplates, TemplateBytes / MB, NModels, GroundModels.Num(), NGround, GroundBytes / MB, FOutlawsAssets::SharedCount(), SpriteAtlasesHd.Num(),
		Textures.Num(), TexturesLo.Num(), TexturesHd.Num(), AtlasCache.Num(), WavCache.Num(), MusicWavCache.Num(), WavBytes / MB);
	if (!bClasses) return;
	const int32 Kind = Tag.StartsWith(TEXT("before")) ? 1 : 0;     // (like with like: an empty world with the last empty one)
	LogLlmDeltas(Tag, Kind);
	// the classes that grew since the last snapshot (a leak of objects shows here; a leak of native memory does not)
	TMap<FName, int32> Now;
	for (FThreadSafeObjectIterator It; It; ++It)
	{
		if (const UClass* C = It->GetClass()) ++Now.FindOrAdd(C->GetFName());
	}
	if (GSoakClassCounts[Kind].Num() > 0)
	{
		TArray<TPair<FName, int32>> Grew;
		for (const TPair<FName, int32>& Kv : Now)
		{
			const int32 D = Kv.Value - GSoakClassCounts[Kind].FindRef(Kv.Key);
			if (D != 0) Grew.Add(TPair<FName, int32>(Kv.Key, D));
		}
		for (const TPair<FName, int32>& Kv : GSoakClassCounts[Kind])
		{
			if (!Now.Contains(Kv.Key)) Grew.Add(TPair<FName, int32>(Kv.Key, -Kv.Value));
		}
		Grew.Sort([](const TPair<FName, int32>& A, const TPair<FName, int32>& B) { return FMath::Abs(A.Value) > FMath::Abs(B.Value); });
		TArray<FString> Parts;
		for (int32 i = 0; i < Grew.Num() && i < 12; ++i) Parts.Add(FString::Printf(TEXT("%s %+d"), *Grew[i].Key.ToString(), Grew[i].Value));
		UE_LOG(LogOutlaws, Display, TEXT("OLMEM %s: objects by class since the last: %s"), *Tag, Parts.Num() ? *FString::Join(Parts, TEXT(", ")) : TEXT("no change"));
	}
	GSoakClassCounts[Kind] = MoveTemp(Now);
}

#if WITH_OUTLAWS_TESTS
void AOutlawsLevelActor::TickMemSoak(float Dt)
{
	static TArray<FString> Plan;
	static int32 At = 0, Phase = -1, Frames = 0;
	static float Wait = 0.f, Total = 0.f, DeadFor = 0.f;
	static uint32 GcMark = 0;
	static int32 ClearsAtDeath = 0;
	static const void* DeathIn = nullptr;     // (the level actor of the death: a map load's respawn is another one)
	static double Tolerance = 50.0;
	struct FRow { FString Step, Level; double Before, After, Settled, Swap, Gpu; };
	static TArray<FRow> Rows;
	static FString StepNow;
	// (h steps) the Historical Missions checks
	static bool bHomPlan = false, bHomOn = false;
	static int32 HomChecks = 0, HomFails = 0, HomMapLoadsAt = 0, HomAttempt = -1, HomScore = 0, HomPorches = 0;
	static uint32 HomSum = 0;
	static FString HomWant, HomWantBack;
	if (Phase == 99) return;
	if (Phase < 0)
	{
		FString Arg;
		if (!OLFlags::Value(TEXT("OLMemSoak"), Arg, false) || Arg.IsEmpty()) Arg = TEXT("r,r,r,r,r");
		Arg.ParseIntoArray(Plan, TEXT(","));
		for (const FString& S : Plan) bHomPlan |= S.TrimStartAndEnd().StartsWith(TEXT("h"));
		OLFlags::Value(TEXT("OLMemSoakTolerance"), Tolerance);
		FCoreUObjectDelegates::GetPostGarbageCollect().AddLambda([]() { ++GSoakGcCount; });
		UE_LOG(LogOutlaws, Display, TEXT("OLMEM soak: %d steps (%s), HD %d, randomizer %d"), Plan.Num(), *FString::Join(Plan, TEXT(",")), bHDTextures ? 1 : 0, RandoOn() ? 1 : 0);
		StepNow = TEXT("start");
		Phase = 0;
	}
	Total += Dt;
	auto Finish = [&](bool bTimedOut)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLMEM table: step | level | before the build (after two collections) MB | after the build MB | settled MB | swap free MB | video memory before the build MB"));
		double Lo = 1e12, Hi = -1e12;
		for (int32 i = 0; i < Rows.Num(); ++i)
		{
			const FRow& R = Rows[i];
			UE_LOG(LogOutlaws, Display, TEXT("OLMEM row %2d | %-10s | %-8s | %6.0f | %6.0f | %6.0f | %6.0f | %6.0f"), i, *R.Step, *R.Level, R.Before, R.After, R.Settled, R.Swap, R.Gpu);
			if (i >= 3) { Lo = FMath::Min(Lo, R.Before); Hi = FMath::Max(Hi, R.Before); }
		}
		const double Spread = Rows.Num() > 3 ? Hi - Lo : 0.0;
		const double PerRebuild = Rows.Num() > 2 ? (Rows.Last().Before - Rows[1].Before) / double(Rows.Num() - 2) : 0.0;
		if (bHomPlan)
		{
			const bool bHomPass = !bTimedOut && HomChecks > 0 && HomFails == 0;
			UE_LOG(LogOutlaws, Display, TEXT("OLMEM hom %s: %d checks, %d failed, %d map loads%s"), bHomPass ? TEXT("PASS") : TEXT("FAIL"), HomChecks, HomFails, GMapLoads,
				bTimedOut ? TEXT(" (timed out)") : TEXT(""));
		}
		const bool bPass = !bTimedOut && Rows.Num() > 3 && Spread <= Tolerance;
		UE_LOG(LogOutlaws, Display, TEXT("OLMEM soak %s: %d rebuilds, the baseline from the third rebuild on moved %.0f MB (allowed %.0f), %.1f MB a rebuild on average from the first%s"),
			bPass ? TEXT("PASS") : TEXT("FAIL"), Rows.Num() - 1, Spread, Tolerance, PerRebuild, bTimedOut ? TEXT(" (timed out)") : TEXT(""));
		Phase = 99;
		FPlatformMisc::RequestExit(false);
	};
	if (Total > 120.f + 60.f * Plan.Num())
	{
		Finish(true);
		return;
	}
	bEnemiesActive = false;                    // (the test is of the builds, not the fight)
	if (Phase != 5) bInvincible = true;
	switch (Phase)
	{
	case 0:
	{
		const bool bReady = !bRebuildPending && !bNeedPlace && Inf && !PendingLoad.IsValid() && !Movie && LevelEndTimer < 0.f && !bFrontEnd;
		Wait = bReady ? Wait + Dt : 0.f;
		if (Wait < 3.f) return;
		Wait = 0.f;
		Frames = 0;
		{
			// a session's sounds: every bad guy's shot, hit, death and taunts played once (the Quest plays them all; the
			// soak's bad guys are asleep)
			TSet<FString> Played;
			for (const TWeakObjectPtr<AOutlawsEnemy>& E : Enemies)
			{
				if (!E.IsValid()) continue;
				const FOutlawsEnemyDef& D = E->Def;
				TArray<FString> Names = { D.ShootSound, D.HitSound, D.DieSound };
				Names.Append(D.LosTaunts);
				Names.Append(D.NoLosTaunts);
				Names.Append(D.AlertTaunts);
				for (const FString& N : Names)
				{
					if (N.IsEmpty() || Played.Contains(N.ToLower())) continue;
					Played.Add(N.ToLower());
					if (UAudioComponent* AC = PlayWav(N, FVector::ZeroVector, false, true)) AC->Stop();
				}
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLMEM soak: %d sounds of the level's bad guys played"), Played.Num());
		}
		GcMark = GSoakGcCount;
		if (GEngine) GEngine->ForceGarbageCollection(true);
		Phase = 1;
		return;
	}
	case 1:
		if (GSoakGcCount == GcMark && ++Frames < 60) return;
		Frames = 0;
		Phase = 2;
		return;
	case 2:
		if (++Frames < 3) return;
		Frames = 0;
		GcMark = GSoakGcCount;
		if (GEngine) GEngine->ForceGarbageCollection(true);
		Phase = 3;
		return;
	case 3:
		if (GSoakGcCount == GcMark && ++Frames < 60) return;
		Frames = 0;
		Phase = 4;
		return;
	case 4:
	{
		if (++Frames < 5) return;
		Frames = 0;
		FlushRenderingCommands();
		const FString Tag = FString::Printf(TEXT("rebuild %d (%s, %s) settled"), Rows.Num(), *StepNow, *LevelName);
		LogMemSnapshot(Tag, true);
		Rows.Add({ StepNow, LevelName, Rows.Num() == 0 ? ResidentMB() : LastBuildBeforeMB, Rows.Num() == 0 ? ResidentMB() : LastBuildAfterMB, ResidentMB(), SwapFreeMB(), GLastGpuBefore });
		if (bHomPlan && (bHomOn || StepNow == TEXT("ho")))
		{
			bHomOn = true;
			auto Check = [&](bool bOk, const FString& What)
			{
				++HomChecks;
				HomFails += bOk ? 0 : 1;
				UE_LOG(LogOutlaws, Display, TEXT("OLMEM hom check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
			};
			int32 Above = 0;
			OLFlags::Value(TEXT("OLMemLoadMapAbove"), Above);
			Check(bHom, FString::Printf(TEXT("after %s, %s is still a Historical Missions session (hom %d)"), *StepNow, *LevelName, bHom ? 1 : 0));
			if (Above > 0)
			{
				Check(GMapLoads > HomMapLoadsAt && GMapLoadToMB < GMapLoadFromMB, FString::Printf(TEXT("%s was a real map load (%d this run, %d before; %.0f MB as it fired, %.0f MB once loaded, down %.0f MB)"),
					*StepNow, GMapLoads, HomMapLoadsAt, GMapLoadFromMB, GMapLoadToMB, GMapLoadFromMB - GMapLoadToMB));
			}
			HomMapLoadsAt = GMapLoads;
			if (StepNow == TEXT("ho"))
			{
				Check(LevelName.Equals(TEXT("office"), ESearchCase::IgnoreCase) && HomGameScore == HomScore, FString::Printf(TEXT("the office (%s), score %d (want %d)"), *LevelName, HomGameScore, HomScore));
			}
			else if (StepNow.StartsWith(TEXT("hm")))
			{
				HomSum = RandoSum;
				HomAttempt = RandoAttempt;
				Check(LevelName.Equals(HomWant, ESearchCase::IgnoreCase) && HomMission.Equals(HomWant, ESearchCase::IgnoreCase) && HomReturnSector.Equals(HomWantBack, ESearchCase::IgnoreCase)
					&& HomGameScore == HomScore && HomPorchEvents.Num() == HomPorches,
					FString::Printf(TEXT("riding out: in %s, mission %s back to %s (want %s, %s), score %d (want %d), %d outlaws done (want %d), roll attempt %d checksum %08x"), *LevelName,
						*HomMission, *HomReturnSector, *HomWant, *HomWantBack, HomGameScore, HomScore, HomPorchEvents.Num(), HomPorches, RandoAttempt, RandoSum));
			}
			else if (StepNow == TEXT("d") || StepNow == TEXT("r"))
			{
				Check(LevelName.Equals(HomWant, ESearchCase::IgnoreCase) && HomMission.Equals(HomWant, ESearchCase::IgnoreCase) && HomReturnSector.Equals(HomWantBack, ESearchCase::IgnoreCase)
					&& HomGameScore == HomScore && HomPorchEvents.Num() == HomPorches && RandoAttempt == HomAttempt && RandoSum == HomSum,
					FString::Printf(TEXT("%s: in %s, mission %s back to %s, score %d (want %d), %d outlaws done, roll attempt %d checksum %08x (want %d, %08x)"),
						StepNow == TEXT("d") ? TEXT("the death respawn") : TEXT("Restart Level"), *LevelName, *HomMission, *HomReturnSector, HomGameScore, HomScore, HomPorchEvents.Num(),
						RandoAttempt, RandoSum, HomAttempt, HomSum));
			}
			else if (StepNow == TEXT("hw"))
			{
				const int32 Ev = HomPorchEvents.FindRef(HomWantBack);
				Check(LevelName.Equals(TEXT("office"), ESearchCase::IgnoreCase) && Ev == 131072 && HomPorchEvents.Num() == HomPorches + 1 && HomGameScore >= HomScore + 1000
					&& GetCount(TEXT("idep")) > 0 && HasHomSave(),
					FString::Printf(TEXT("won %s: in %s, %s stamped %d (want 131072, captured), %d outlaws done (want %d), score %d (was %d), Deputy's badge %d, session saved %d"),
						*HomWant, *LevelName, *HomWantBack, Ev, HomPorchEvents.Num(), HomPorches + 1, HomGameScore, HomScore, GetCount(TEXT("idep")), HasHomSave() ? 1 : 0));
				HomScore = HomGameScore;
				HomPorches = HomPorchEvents.Num();
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLMEM hom: %s after %s: mission %s back to %s, score %d (gold %d, kills %d, wounds %d, bounty %d), %d outlaws done"),
				*LevelName, *StepNow, *HomMission, *HomReturnSector, HomGameScore, HomTotalGold, HomTotalKills, HomTotalWounds, HomTotalBounty, HomPorchEvents.Num());
		}
		if (At >= Plan.Num())
		{
			Finish(false);
			return;
		}
		StepNow = Plan[At++].TrimStartAndEnd();
		UE_LOG(LogOutlaws, Display, TEXT("OLMEM soak step %d of %d: %s"), At, Plan.Num(), *StepNow);
		if (StepNow.StartsWith(TEXT("j")) && StepNow.Len() > 1)
		{
			JumpToLevel(StepNow.Mid(1));
			Phase = 0;
		}
		else if (StepNow.StartsWith(TEXT("n")) && StepNow.Len() > 1)
		{
			NextLevelName = StepNow.Mid(1);        // (the level won: the story's next, the kit carried)
			NextMovie.Reset();
			StartNextLevel();
			Phase = 0;
		}
		else if (StepNow == TEXT("ho"))
		{
			// a session in the office, part of the way to the Deputy's badge (9300)
			StartMission(TEXT("office"), Difficulty);
			bHom = true;
			HomMission.Reset();
			HomReturnSector.Reset();
			HomPorchEvents.Reset();
			HomTotalGold = HomTotalKills = HomTotalWounds = 0;
			HomTotalBounty = HomGameScore = HomScore = 9000;
			HomPorches = 0;
			Phase = 0;
		}
		else if (StepNow.StartsWith(TEXT("hm")) && StepNow.Len() > 2)
		{
			StepNow.Mid(2).Split(TEXT("/"), &HomWant, &HomWantBack);
			HomWant = HomWant.ToLower();
			HomWantBack = HomWantBack.ToUpper();
			HomMission = HomWant;
			HomReturnSector = HomWantBack;
			HomRide();
			Phase = 0;
		}
		else if (StepNow == TEXT("hw"))
		{
			// the outlaw knocked out with a fist, laid beside the player, the horse (HomMissionEnd), on to the office
			UWorld* W = GetWorld();
			APawn* Pawn = W && W->GetFirstPlayerController() ? W->GetFirstPlayerController()->GetPawn() : nullptr;
			AOutlawsEnemy* B = Boss.Get();
			if (!B) for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : Enemies) if (Wk.IsValid() && Wk->bBoss) { B = Wk.Get(); break; }
			if (B && Pawn)
			{
				B->SetDormant(false);
				for (int32 k = 0; k < 20 && !B->IsDead(); ++k)
				{
					AOutlawsEnemy::GNextHitMelee = true;
					B->TakeHit(1.f, B->GetActorLocation(), FVector(1.0, 0.0, 0.0), B->GetActorLocation() - FVector(30.0, 0.0, 0.0));
				}
				B->SetActorLocation(Pawn->GetActorLocation() + FVector(100.0, 0.0, 0.0), false, nullptr, ETeleportType::TeleportPhysics);
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLMEM hom: the outlaw of %s: %s, dead %d, taken alive %d"), *LevelName, B ? *B->GetOutlawsName() : TEXT("none"), B && B->IsDead() ? 1 : 0,
				B && B->bTakenAlive ? 1 : 0);
			HomMissionEnd();
			NextMovie.Reset();
			StartNextLevel();
			Phase = 0;
		}
		else if (StepNow == TEXT("l"))
		{
			if (!SaveGame(-1) || !LoadGame(-1)) RestartLevel();      // (the autosave written, then loaded)
			Phase = 0;
		}
		else if (StepNow == TEXT("d"))
		{
			DeadFor = 0.f;
			ClearsAtDeath = ClearCount;
			DeathIn = this;
			bInvincible = false;
			ShotOwnHead();
			Phase = 5;
		}
		else
		{
			RestartLevel();
			Phase = 0;
		}
		return;
	}
	case 5:
	{
		// the death view: the player's respawn once it lets one go (a second into it)
		DeadFor += Dt;
		if (bRebuildPending || ClearCount != ClearsAtDeath || DeathIn != this)     // (the respawn's rebuild: this tick waits it out)
		{
			Phase = 0;
			return;
		}
		UWorld* W = GetWorld();
		AOutlawsPlayer* P = W && W->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(W->GetFirstPlayerController()->GetPawn()) : nullptr;
		if (P && DeadFor > 1.5f) P->RequestRespawn(false);
		if (DeadFor > 20.f)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLMEM soak: the death did not respawn (dead view %d)"), P && P->IsDeadView() ? 1 : 0);
			Finish(true);
		}
		return;
	}
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS

static TAutoConsoleVariable<int32> CVarOLMemLoadMapAboveMB(TEXT("ol.mem.LoadMapAboveMB"), PLATFORM_ANDROID ? 1300 : 0,
	TEXT("(BUG-RESPAWNCRASH) a level rebuild with the process over this many MB (after the old level's collections) loads L_Runtime for real and builds there; 0 never (-OLMemLoadMapAbove=<MB>)"));

bool AOutlawsLevelActor::LoadMapInsteadOfRebuild()
{
	if (bFromMapLoad)
	{
		bFromMapLoad = false;                  // (the map load's own build: never a second load for it)
		return false;
	}
	int32 Above = CVarOLMemLoadMapAboveMB.GetValueOnGameThread();
	OLFlags::Value(TEXT("OLMemLoadMapAbove"), Above);
	if (Above <= 0) return false;
	const double Mb = ResidentMB();
	if (Mb <= Above) return false;
	UWorld* W = GetWorld();
	const TCHAR* Why = nullptr;
	if (!W || !W->IsGameWorld()) Why = TEXT("not a game world");
	else if (UOutlawsCoop::IsActive(this)) Why = TEXT("a co-op session");
	else if (bFrontEnd || bBackdrop) Why = TEXT("the main menu's level");
	else if (RebuildKind == ERebuildKind::Other) Why = TEXT("a rebuild with more to it than a level start");
	if (Why)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT memory %.0f MB before the build of %s (over %d MB), but %s: built in place"), Mb, *LevelName, Above, Why);
		return false;
	}
	TSharedPtr<FJsonObject> O;
	if (RebuildKind == ERebuildKind::Load && MapCarryLoad.IsValid())
	{
		O = MapCarryLoad;
	}
	else
	{
		// the level from its start, as SaveLevelOpening writes it: the same level's entry kit (a restart, a death), or
		// what the story carries into the next
		O = MakeShared<FJsonObject>();
		const bool bNext = RebuildKind == ERebuildKind::NextLevel;
		const bool bEntry = !bNext && !EntryLevel.IsEmpty() && EntryLevel.Equals(LevelName, ESearchCase::IgnoreCase);
		O->SetNumberField(TEXT("version"), 1);
		O->SetBoolField(TEXT("opening"), true);
		O->SetStringField(TEXT("level"), LevelName);
		O->SetStringField(TEXT("name"), LevelTitle(LevelName));
		O->SetNumberField(TEXT("difficulty"), int32(Difficulty));
		O->SetNumberField(TEXT("spawn_difficulty"), int32(Difficulty));
		O->SetNumberField(TEXT("playtime"), PlayTime);
		O->SetNumberField(TEXT("health"), 10.0);
		O->SetNumberField(TEXT("armour"), bNext ? MapCarryArmour : bEntry ? EntryArmour : 0);
		O->SetNumberField(TEXT("armour_max"), ArmourMax);
		O->SetNumberField(TEXT("gold"), bNext ? MapCarryGold : bEntry ? EntryGold : 0);
		O->SetNumberField(TEXT("kills"), 0);
		O->SetBoolField(TEXT("lamp"), false);
		O->SetNumberField(TEXT("oilburn"), OilBurn);
		O->SetNumberField(TEXT("badge"), 0.0);
		const TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
		const TMap<FString, int32> None;
		for (const TPair<FString, int32>& Kv : bNext ? MapCarryCounts : bEntry ? EntryCounts : None) C->SetNumberField(Kv.Key, Kv.Value);
		O->SetObjectField(TEXT("counts"), C);
		// (BUG-HOLSTERRELOAD) the guns' rounds go with it: the next level's loaded from the carried ammunition, a restart's
		// as the level was entered (the map load's new player would have them all full)
		if (const AOutlawsPlayer* P = W->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(W->GetFirstPlayerController()->GetPawn()) : nullptr; P && (bNext || bEntry))
		{
			P->WriteCarryMags(*O, !bNext);
		}
		WriteRandoSave(*O, false);
		// (the roll this rebuild would have built: the next level's first, a restart's the same roll)
		const TSharedPtr<FJsonObject>* R = nullptr;
		if (O->TryGetObjectField(TEXT("rando"), R) && R && R->IsValid())
		{
			(*R)->SetStringField(TEXT("level"), LevelName.ToLower());
			(*R)->SetNumberField(TEXT("attempt"), NextRandoAttempt(LevelName));
		}
	}
	if (bHom)
	{
		// (a historical mission: the session goes along; a load's own save is not written into)
		if (O == MapCarryLoad)
		{
			const TSharedPtr<FJsonObject> Copy = MakeShared<FJsonObject>();
			Copy->Values = O->Values;
			O = Copy;
		}
		WriteHomCarry(*O);
	}
	GMapLoadRebuild = O;
	++GMapLoads;
	GMapLoadFromMB = Mb;
	bMapLoadPending = true;
	AfterRestart = nullptr;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT memory %.0f MB before the build of %s, over %d MB: a map load of L_Runtime builds it instead (%s; map load %d this run)%s"),
		Mb, *LevelName, Above, RebuildKind == ERebuildKind::Load ? TEXT("a load") : RebuildKind == ERebuildKind::NextLevel ? TEXT("the next level") : TEXT("the level from its start"), GMapLoads,
		bHom ? TEXT(", the historical missions session with it") : TEXT(""));
	UGameplayStatics::OpenLevel(W, FName(TEXT("/Game/Outlaws/Maps/L_Runtime")), true);
#if WITH_OUTLAWS_TESTS
	extern bool OlExitAt(const TCHAR* Point);
	OlExitAt(TEXT("mapload"));
#endif
	return true;
}

bool AOutlawsLevelActor::TakeMapLoadRebuild()
{
	if (!GMapLoadRebuild.IsValid()) return false;
	TSharedPtr<FJsonObject> O = MoveTemp(GMapLoadRebuild);
	GMapLoadRebuild.Reset();
	UWorld* W = GetWorld();
	if (!W || !W->IsGameWorld() || UOutlawsCoop::IsActive(this)) return false;
	bFromMapLoad = true;
	GMapLoadToMB = ResidentMB();
	UE_LOG(LogOutlaws, Log, TEXT("OLRT map load done: %.0f MB resident; building %s"), GMapLoadToMB, *O->GetStringField(TEXT("level")));
	const TSharedPtr<FJsonObject>* H = nullptr;
	if (O->TryGetObjectField(TEXT("hom"), H) && H && H->IsValid()) ReadHomCarry(**H);       // (before the build: the office's stamps, a mission's pickups)
	LoadGameFrom(O, TEXT("a map load's rebuild"));
	return true;
}
