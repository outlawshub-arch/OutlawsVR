// REQ-ASHIKA's probe (the user 4 Nov: "I want to take Nuketown out and replace it with Ashika_RM.zip from myself"):
//   -OLShots -OLNativeMaps=<dir> -OLMpHost=drygulch -OLMpClass=dm -OLMpBots=0 -OLMpStartWhen=0 -OLAshikaProbe[=<level>]
//   [-OLAshikaSeconds=<n>] [-OLDeviceProfile=Quest2]
// The list: 19 native maps, no Nuketown, the map's RCM modes.  The launched host, alone, hosts the map (ashika, or <level> for a
// map to set its numbers beside) in Death Match with 3 bots: the level builds under its own overlay, the names its LAB shares
// with the stock LABs with other bytes (igatgun.itm, stool.nwx and the like) are the map's own there, a shot at the spawn; the
// bots run for 60 s: how far each goes, how many 3 m cells it has stood in, its routes; the frame time and the memory over them.
// Then a stock map: no overlay, every one of those names back to the stock bytes, the level's sound cache too: nothing of the
// map's reaches outside it.  One line a check "OLSHOT ashika check PASS|FAIL: ...", then "OLSHOT ashika RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsCoop.h"
#include "OutlawsMpHost.h"
#include "OutlawsLab.h"
#include "OutlawsFlags.h"
#include "OutlawsRemotePlayer.h"
#include "OutlawsShotTests.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/Crc.h"
#include "Misc/Paths.h"
#include "UObject/UObjectIterator.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsAshikaProbeLocal
{
	int32 Stage = 0, Passes = 0, Fails = 0, Frames = 0;
	double StageAt = 0.0, FrameSum = 0.0, FrameWorst = 0.0, MemBuilt = 0.0;
	FString Want, Target;
	FOutlawsMpMap Map, Stock;
	TSharedPtr<FOutlawsLabFile> Lab;
	TUniquePtr<FOutlawsAssets> StockView;                                    // the install alone (no overlay ever opened on it)
	TArray<FString> Overrides;                                               // the map's names with other bytes than the stock file's
	uint32 ChachingOwn = 0;
	struct FBotTrack { FVector Last = FVector::ZeroVector; bool bHave = false; double Moved = 0.0; TSet<FIntPoint> Cells; int32 Stucks = 0; };
	TMap<int32, FBotTrack> Tracks;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT ashika check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}

	uint32 Crc(const TArray<uint8>& B) { return B.Num() > 0 ? FCrc::MemCrc32(B.GetData(), B.Num()) : 1u; }

	uint32 LabCrc(const FString& Key)
	{
		const int32* E = Lab->GetIndex().Find(Key);
		TArray<uint8> B;
		return E && Lab->Read(*E, B) ? Crc(B) : 0u;
	}

	uint32 StockCrc(const FString& Key)
	{
		TArray<uint8> B;
		return StockView->Get(Key, B) ? Crc(B) : 0u;
	}

	/** How many of the override names resolve in Assets to what the map's LAB (bOwn) or the stock LABs hold; the first wrong ones. */
	int32 Resolved(const FOutlawsAssets* A, bool bOwn, TArray<FString>& Wrong)
	{
		int32 Right = 0;
		for (const FString& N : Overrides)
		{
			TArray<uint8> B;
			const uint32 Got = A && A->Get(N, B) ? Crc(B) : 0u;
			if (Got == (bOwn ? LabCrc(N) : StockCrc(N))) ++Right;
			else if (Wrong.Num() < 6) Wrong.Add(FString::Printf(TEXT("%s (from %s)"), *N, A ? *A->Where(N) : TEXT("-")));
		}
		return Right;
	}
}

OL_SHOT_MATCH(OLAshikaProbe) { return OLFlags::Has(TEXT("OLAshikaProbe")); }

OL_SHOT_TEST(OLAshikaProbe, 2416)
{
	using namespace OutlawsAshikaProbeLocal;
	if (!L || Stage < 0) return;
	const double Now = FPlatformTime::Seconds();
	L->StartSaveDelay = -1.f;           // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	L->bEnemiesActive = true;           // (ShotTest stills them each tick: the bots' brains are bad guys')
	UOutlawsCoop* C = UOutlawsCoop::Get(this);
	AOutlawsCoopSession* S = C ? C->GetSession() : nullptr;
	int32 Seconds = 60;
	OLFlags::Value(TEXT("OLAshikaSeconds"), Seconds);
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT ashika RESULT %s (%d passed, %d failed; %s%s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails, *Want,
			OLFlags::HasValue(TEXT("OLDeviceProfile")) ? TEXT(", a device profile") : TEXT(""));
		StockView.Reset();
		Lab.Reset();
		ShotTime = -1.f;
		Stage = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	auto Host = [&](const FOutlawsMpMap& M, bool bBots)
	{
		FOutlawsMpHostOptions O = C->MatchOptions;
		O.bRandomMap = O.bRandomClass = false;
		O.bBots = bBots;
		O.BotCount = 3;
		O.MapFile = M.File;
		O.GameClass = EOutlawsGameClass::DeathMatch;
		Target = M.Level;
		StageAt = Now;
		return C->StartNextMatch(O, TEXT("the Ashika probe"));
	};
	auto Built = [&]()
	{
		return C->bLevelBuilt && C->BuiltLevel.Equals(Target, ESearchCase::IgnoreCase) && L->LevelName.Equals(Target, ESearchCase::IgnoreCase)
			&& !L->IsRebuildPending() && C->bMatchSetUp && S && S->bHostReady && !bMenuOpen;
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
		if (!OLFlags::Value(TEXT("OLAshikaProbe"), Want) || Want.IsEmpty()) Want = TEXT("ashika");
		Want = Want.ToLower();
		TArray<FOutlawsMpMap> Maps;
		OutlawsMpHost::ListMaps(A, Maps);
		int32 Natives = 0;
		for (const FOutlawsMpMap& M : Maps) Natives += M.bNative ? 1 : 0;
		const bool bNuke = Maps.ContainsByPredicate([](const FOutlawsMpMap& M) { return M.bNative && M.Level.Equals(TEXT("nuketown"), ESearchCase::IgnoreCase); });
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT ashika list: %d maps, %d native, Nuketown %s"), Maps.Num(), Natives, bNuke ? TEXT("listed") : TEXT("not listed"));
		const FOutlawsMpMap* M = Maps.FindByPredicate([](const FOutlawsMpMap& X) { return X.bNative && X.Level.Equals(Want, ESearchCase::IgnoreCase); });
		const FString Launched = C->MatchOptions.MapFile;
		const FOutlawsMpMap* St = Maps.FindByPredicate([&Launched](const FOutlawsMpMap& X) { return !X.bCustom && X.File != Launched && X.Allows(EOutlawsGameClass::DeathMatch); });
		if (!M || !St)
		{
			Check(false, FString::Printf(TEXT("the map %s listed (%d), a stock map (%d): -OLNativeMaps=<dir>"), *Want, M ? 1 : 0, St ? 1 : 0));
			Finish();
			return;
		}
		Map = *M;
		Stock = *St;
		if (Want == TEXT("ashika"))
		{
			Check(!bNuke, TEXT("Nuketown is not on the list"));
			Check(Map.Allows(EOutlawsGameClass::CaptureTheFlag) && Map.Allows(EOutlawsGameClass::Chicken) && Map.Allows(EOutlawsGameClass::TeamPlay) && Map.Allows(EOutlawsGameClass::DeathMatch)
				&& !Map.Allows(EOutlawsGameClass::Tag), FString::Printf(TEXT("%s plays %s (the RCM's CKMD; no Tag)"), *Map.Name, *Map.ModesText()));
		}
		Lab = MakeShared<FOutlawsLabFile>();
		if (!Lab->Open(Map.Lab)) { Check(false, FString::Printf(TEXT("its LAB %s does not open"), *Map.Lab)); Finish(); return; }
		StockView = MakeUnique<FOutlawsAssets>();
		StockView->Open(A->GetRoot());
		for (const TPair<FString, int32>& Kv : Lab->GetIndex())
		{
			const uint32 Stk = StockCrc(Kv.Key);
			if (Stk && Stk != LabCrc(Kv.Key)) Overrides.Add(Kv.Key);
		}
		Overrides.Sort();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT ashika overrides: %d of the LAB's %d names shadow a stock file with other bytes: %s"), Overrides.Num(), Lab->GetIndex().Num(), *FString::Join(Overrides, TEXT(" ")));
		if (!Host(Map, true)) { Check(false, TEXT("StartNextMatch refused the map")); Finish(); return; }
		Stage = 1;
		return;
	}
	if (!C) return;
	if (Stage == 1)
	{
		if (!Built())
		{
			if (Now - StageAt < 120.0) return;
			Check(false, FString::Printf(TEXT("%s: not ready in 120 s (report: %s)"), *Map.Name, *L->GetBuildReport().Left(200)));
			Finish();
			return;
		}
		const FOutlawsAssets* A = L->GetLabAssets();
		TArray<FString> Wrong;
		const int32 Right = Resolved(A, true, Wrong);
		Check(A && A->GetOverlay() == Lab->GetPath(), FString::Printf(TEXT("on %s the overlay is its LAB (%s)"), *Map.Name, A ? *FPaths::GetCleanFilename(A->GetOverlay()) : TEXT("-")));
		Check(Right == Overrides.Num(), FString::Printf(TEXT("%d of %d shadowing names are the map's own bytes here%s%s"), Right, Overrides.Num(), Wrong.Num() ? TEXT(": wrong ") : TEXT(""), *FString::Join(Wrong, TEXT(", "))));
		Check(!L->GetBuildReport().StartsWith(TEXT("ERROR")), FString::Printf(TEXT("the build: %s"), *L->GetBuildReport().Left(120)));
		if (Overrides.Contains(TEXT("chaching.wav")))
		{
			L->WarmWav(TEXT("chaching"));
			ChachingOwn = L->DebugCachedWavCrc(TEXT("chaching"));
		}
		int32 Textures = 0;
		for (TObjectIterator<UTexture2D> It; It; ++It) ++Textures;
		MemBuilt = double(FPlatformMemory::GetStats().UsedPhysical) / (1024.0 * 1024.0);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT ashika numbers %s built: %d textures alive, %.0f MB used, LAB %.1f MB"), *Want, Textures, MemBuilt, double(IFileManager::Get().FileSize(*Map.Lab)) / (1024.0 * 1024.0));
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("ol_ashika_%s.png"), *Want), false, false);
		FOutlawsMatchFlow& F = OutlawsMpHost::Flow();
		if (F.GetState() == FOutlawsMatchFlow::EState::WarmUp) F.HostStart();
		Tracks.Reset();
		Frames = 0;
		FrameSum = FrameWorst = 0.0;
		StageAt = Now;
		Stage = 2;
		return;
	}
	if (Stage == 2)
	{
		// the bots run: each one's way, the frame time over it (from 5 s on, the level's first frames past)
		TArray<UOutlawsCoop::FBotProbeView> Views;
		C->DebugBotViews(Views);
		for (const UOutlawsCoop::FBotProbeView& V : Views)
		{
			const AOutlawsRemotePlayer* R = C->PuppetOf(V.Who);
			if (!R || V.bDead) continue;
			FBotTrack& T = Tracks.FindOrAdd(V.Who);
			const FVector At = R->GetActorLocation();
			const double Step = T.bHave ? FVector::Dist2D(At, T.Last) : 0.0;
			if (Step < 500.0) T.Moved += Step;             // (a respawn is no walk)
			T.Last = At;
			T.bHave = true;
			T.Cells.Add(FIntPoint(FMath::FloorToInt(At.X / 300.0), FMath::FloorToInt(At.Y / 300.0)));
			T.Stucks = V.Stucks;
		}
		if (Now - StageAt > 5.0)
		{
			FrameSum += Dt * 1000.0;
			FrameWorst = FMath::Max(FrameWorst, double(Dt) * 1000.0);
			++Frames;
		}
		if (Now - StageAt < double(Seconds)) return;
		int32 Round = 0;
		FString Each;
		for (const TPair<int32, FBotTrack>& Kv : Tracks)
		{
			Round += Kv.Value.Moved > 3000.0 && Kv.Value.Cells.Num() >= 8 ? 1 : 0;
			Each += FString::Printf(TEXT(" bot %d: %.0f m, %d cells, %d stuck, ends at %.0f %.0f;"), Kv.Key, Kv.Value.Moved / 100.0, Kv.Value.Cells.Num(), Kv.Value.Stucks, Kv.Value.Last.X, Kv.Value.Last.Y);
		}
		int32 Routes = 0, RouteFails = 0;
		for (const UOutlawsCoop::FBotProbeView& V : Views) { Routes += V.Routes; RouteFails += V.RouteFails; }
		Check(Tracks.Num() == 3 && Round == Tracks.Num(), FString::Printf(TEXT("the bots get round %s: %d of %d went 30 m and over 8 cells in %d s;%s routes %d, failed %d"), *Map.Name, Round, Tracks.Num(), Seconds, *Each, Routes, RouteFails));
		const double Mem = double(FPlatformMemory::GetStats().UsedPhysical) / (1024.0 * 1024.0);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT ashika numbers %s with 3 bots: %.1f ms a frame on average, the worst %.1f, over %d frames; %.0f MB used (built %.0f)"),
			*Want, FrameSum / FMath::Max(1, Frames), FrameWorst, Frames, Mem, MemBuilt);
		if (!Host(Stock, false)) { Check(false, TEXT("StartNextMatch refused the stock map")); Finish(); return; }
		Stage = 3;
		return;
	}
	if (Stage == 3)
	{
		if (!Built())
		{
			if (Now - StageAt < 120.0) return;
			Check(false, FString::Printf(TEXT("%s: not ready in 120 s"), *Stock.File));
			Finish();
			return;
		}
		const FOutlawsAssets* A = L->GetLabAssets();
		TArray<FString> Wrong;
		const int32 Right = Resolved(A, false, Wrong);
		Check(A && A->GetOverlay().IsEmpty(), FString::Printf(TEXT("on the stock map %s there is no overlay"), *Stock.File));
		Check(Right == Overrides.Num(), FString::Printf(TEXT("%d of %d shadowing names are the stock bytes there%s%s"), Right, Overrides.Num(), Wrong.Num() ? TEXT(": wrong ") : TEXT(""), *FString::Join(Wrong, TEXT(", "))));
		if (ChachingOwn)
		{
			const uint32 Before = L->DebugCachedWavCrc(TEXT("chaching"));
			L->WarmWav(TEXT("chaching"));
			const uint32 After = L->DebugCachedWavCrc(TEXT("chaching"));
			Check(Before != ChachingOwn && After != ChachingOwn, FString::Printf(TEXT("the sound cache forgot the map's chaching (%08x; before its warm %08x, after %08x)"), ChachingOwn, Before, After));
		}
		Finish();
	}
}
#endif // WITH_OUTLAWS_TESTS
