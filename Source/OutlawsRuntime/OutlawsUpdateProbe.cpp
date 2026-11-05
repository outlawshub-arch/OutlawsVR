// REQ-AUTOUPDATE's probe: -OLUpdateProbe -OLUpdateProbeDir=<folder>, the folder tools/olue/release/fake_feed.py wrote: fake
// releases as folders, a fake install. The update's whole way through, headless: new, same, older, offline, a feed not
// understood, bad hash, skip this build, not now, Options off, a game under way, the PC swap (its real script on a scratch
// copy, the theme and maps kept, the rest of the folder and Saved as they were), the Quest's unpack; then one real GET of the
// feed and one Range read of the public release (no sign-in). -OLOldUpdater: only the switch's own case. A line a case, then
// OLSHOT update RESULT.

#include "OutlawsUpdate.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"

#if WITH_OUTLAWS_TESTS

#include "Containers/Ticker.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

void OLUpdateStartProbe(UOutlawsUpdate* U);

namespace
{
	int32 GPass = 0, GFail = 0;
	using EStep = UOutlawsUpdate::EStep;

	void Line(bool bOk, const FString& What)
	{
		++(bOk ? GPass : GFail);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT update %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
	}

	bool Same(const FString& File, const FString& Text)
	{
		FString Got;
		return FFileHelper::LoadFileToString(Got, *File) && Got.TrimStartAndEnd() == Text;
	}

	void Pump(UOutlawsUpdate* U, double Secs = 40.0)
	{
		const double End = FPlatformTime::Seconds() + Secs;
		while (FPlatformTime::Seconds() < End)
		{
			FHttpModule::Get().GetHttpManager().Tick(0.02f);
			U->Tick(0.02f);
			const EStep S = U->TestGetStep();
			if (S != EStep::Wait && S != EStep::Checking && S != EStep::Downloading && S != EStep::Working && S != EStep::Ready) break;
			FPlatformProcess::Sleep(0.005f);
		}
		U->Tick(0.02f);
	}

	UOutlawsUpdate::FTestRun RunOf(const FString& Dir, const TCHAR* Feed, const FString& Root, int32 Build)
	{
		UOutlawsUpdate::FTestRun R;
		R.Feed = FPaths::Combine(Dir, Feed);
		R.Root = Root;
		R.Build = Build;
		return R;
	}

	FString FreshRoot(const FString& Dir, const TCHAR* Name)
	{
		const FString Root = FPaths::Combine(Dir, TEXT("run"), Name);
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		FPlatformFileManager::Get().GetPlatformFile().CopyDirectoryTree(*Root, *FPaths::Combine(Dir, TEXT("install")), true);
		return Root;
	}

	int32 RunSwap(const FString& Root)
	{
		FString Script = OLUpdate::SwapScript();
		Script.ReplaceInline(TEXT("\n"), TEXT("\r\n"));
		const FString Path = FPaths::Combine(Root, TEXT("Update"), TEXT("swap.cmd"));
		if (!FPaths::FileExists(Path)) FFileHelper::SaveStringToFile(Script, *Path, FFileHelper::EEncodingOptions::ForceAnsi);
		FString Native = Path;
		FPaths::MakePlatformFilename(Native);
		const FString Args = FString::Printf(TEXT("/c \"\"%s\" 999999 test\""), *Native);
		FProcHandle P = FPlatformProcess::CreateProc(TEXT("cmd.exe"), *Args, false, true, true, nullptr, 0, nullptr, nullptr);
		if (!P.IsValid()) return -1;
		FPlatformProcess::WaitForProc(P);
		int32 Code = -1;
		FPlatformProcess::GetProcReturnCode(P, &Code);
		FPlatformProcess::CloseProc(P);
		return Code;
	}

	void CasesPure(const FString& Dir)
	{
		OLUpdate::FSha256 A;
		A.Update(reinterpret_cast<const uint8*>("abc"), 3);
		Line(A.Finish() == TEXT("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"), TEXT("sha256 of abc"));
		OLUpdate::FSha256 B;
		Line(B.Finish() == TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"), TEXT("sha256 of nothing"));
		FString Want;
		FFileHelper::LoadFileToString(Want, *FPaths::Combine(Dir, TEXT("feed_new"), TEXT("zip.sha256")));
		const FString Got = OLUpdate::Sha256OfFile(FPaths::Combine(Dir, TEXT("feed_new"), TEXT("OutlawsVR-fake.zip")));
		Line(!Got.IsEmpty() && Got == Want.TrimStartAndEnd(), FString::Printf(TEXT("sha256 of the fake zip as hashlib has it (%s)"), *Got.Left(12)));

		OLUpdate::FLatest L;
		FString Json;
		FFileHelper::LoadFileToString(Json, *FPaths::Combine(Dir, TEXT("feed_new"), TEXT("latest.json")));
		Line(OLUpdate::ParseLatest(Json, L) && L.Build == 50 && L.Tag == TEXT("v9.9") && L.Version == TEXT("9.9"), TEXT("latest.json parsed"));
		Line(OLUpdate::ZipUrl(L).StartsWith(TEXT("https://github.com/outlawshub-arch/OutlawsVR/releases/download/v9.9/")), *OLUpdate::ZipUrl(L));
		OLUpdate::FLatest Z;
		Line(!OLUpdate::ParseLatest(TEXT("not json"), Z), TEXT("garbage refused"));
		Line(!OLUpdate::ParseLatest(Json.Replace(TEXT("OutlawsVR-fake.zip"), TEXT("../evil.zip")), Z), TEXT("a zip name with a path refused"));
		Line(!OLUpdate::ParseLatest(Json.Replace(TEXT("\"schema\": 1"), TEXT("\"schema\": 2")), Z), TEXT("another schema refused"));
		Line(OLUpdate::Decide(L, 45, 0) == OLUpdate::EDecide::Prompt, TEXT("newer: offered"));
		Line(OLUpdate::Decide(L, 50, 0) == OLUpdate::EDecide::Nothing, TEXT("same: nothing"));
		Line(OLUpdate::Decide(L, 51, 0) == OLUpdate::EDecide::Nothing, TEXT("this one newer: nothing"));
		Line(OLUpdate::Decide(L, 45, 50) == OLUpdate::EDecide::Skipped, TEXT("skipped: nothing"));
		Line(OLUpdate::Decide(L, 45, 48) == OLUpdate::EDecide::Prompt, TEXT("an older skip: a newer build is offered"));
		Line(!OLUpdate::SafeEntryName(TEXT("a/../../b")) && !OLUpdate::SafeEntryName(TEXT("C:/x")) && !OLUpdate::SafeEntryName(TEXT("/x")) && OLUpdate::SafeEntryName(TEXT("a/b.c")), TEXT("zip names outside the folder refused"));
		TArray<OLUpdate::FZipEntry> Idx;
		Line(OLUpdate::ReadZipIndex(FPaths::Combine(Dir, TEXT("feed_new"), TEXT("OutlawsVR-fake.zip")), Idx) && Idx.Num() >= 10, FString::Printf(TEXT("zip index, %d entries"), Idx.Num()));
		Line(!OLUpdate::ReadZipIndex(FPaths::Combine(Dir, TEXT("feed_new"), TEXT("latest.json")), Idx), TEXT("not a zip refused"));
		Line(OLUpdate::SwapScript()[0] == TEXT('@') && !FString(OLUpdate::SwapScript()).Contains(TEXT("\\Users")), TEXT("the swap script holds no paths"));
		// the Quest's folder is used only while no newer install came over it
		const FString Base = FPaths::Combine(Dir, TEXT("run"), TEXT("stamp"));
		IFileManager::Get().MakeDirectory(*FPaths::Combine(Base, TEXT("data")), true);
		FFileHelper::SaveStringToFile(TEXT("50"), *FPaths::Combine(Base, TEXT("build.txt")));
		Line(!OLUpdate::UpdatedDataIn(Base, 50).IsEmpty() && !OLUpdate::UpdatedDataIn(Base, 49).IsEmpty(), TEXT("updated data used at its build"));
		Line(OLUpdate::UpdatedDataIn(Base, 51).IsEmpty(), TEXT("updated data left alone under a newer install"));
	}

	void CasesFlow(UOutlawsUpdate* U, const FString& Dir)
	{
		const FString Root = FreshRoot(Dir, TEXT("flow"));
		auto Quiet = [&](const TCHAR* Name, UOutlawsUpdate::FTestRun R)
		{
			U->TestStart(R);
			Pump(U);
			const bool bQuiet = !U->TestPromptUp() && U->TestWords().IsEmpty() && !UOutlawsUpdate::BubbleUp(U);
			Line(bQuiet && (U->TestGetStep() == EStep::Done || U->TestGetStep() == EStep::Off), FString::Printf(TEXT("%s: silent, step %d"), Name, int32(U->TestGetStep())));
			U->TestStop();
		};
		Quiet(TEXT("same build"), RunOf(Dir, TEXT("feed_same"), Root, 50));
		Quiet(TEXT("offline"), RunOf(Dir, TEXT("feed_nowhere"), Root, 45));
		Quiet(TEXT("no latest.json in the release"), RunOf(Dir, TEXT("feed_none"), Root, 45));
		Quiet(TEXT("a feed not understood"), RunOf(Dir, TEXT("feed_garbage"), Root, 45));
		UOutlawsUpdate::FTestRun Off = RunOf(Dir, TEXT("feed_new"), Root, 45);
		Off.Settings.Add(TEXT("UpdateCheck"), TEXT("false"));
		Quiet(TEXT("Options off"), Off);

		// a game under way: nothing shows until the main menu
		UOutlawsUpdate::FTestRun G = RunOf(Dir, TEXT("feed_new"), Root, 45);
		G.bFrontEnd = false;
		U->TestStart(G);
		Pump(U, 2.0);
		Line(U->TestGetStep() == EStep::Found && !U->TestPromptUp() && !UOutlawsUpdate::BubbleUp(U), TEXT("in a game: found, nothing shown"));
		U->TestSetFrontEnd(true);
		U->Tick(0.02f);
		Line(U->TestPromptUp() && UOutlawsUpdate::BubbleUp(U), FString::Printf(TEXT("on the main menu: \"%s\""), *U->TestWords()));
		U->TestSetFrontEnd(false);
		U->Tick(0.02f);
		Line(!U->TestPromptUp() && !UOutlawsUpdate::BubbleUp(U), TEXT("back in a game: the prompt goes"));
		U->TestSetFrontEnd(true);
		U->Tick(0.02f);
		Line(U->TestPromptUp(), TEXT("the menu again: the prompt is back"));
		// Not now: nothing kept, the next launch asks again
		U->Pick(1);
		const bool bNotNow = U->TestGetStep() == EStep::Done && !UOutlawsUpdate::BubbleUp(U) && U->TestGetRun().Settings.Num() == 0;
		Line(bNotNow, TEXT("not now: gone, nothing written"));
		U->TestStop();

		// Skip this build: written, not asked again, a later build is
		UOutlawsUpdate::FTestRun S = RunOf(Dir, TEXT("feed_new"), Root, 45);
		U->TestStart(S);
		Pump(U, 2.0);
		U->Tick(0.02f);
		U->Pick(2);
		const TMap<FString, FString> Kept = U->TestGetRun().Settings;
		Line(Kept.FindRef(TEXT("UpdateSkipBuild")) == TEXT("50"), TEXT("skip this build: written"));
		U->TestStop();
		S.Settings = Kept;
		Quiet(TEXT("the skipped build"), S);
		UOutlawsUpdate::FTestRun S2 = RunOf(Dir, TEXT("feed_newer"), Root, 45);
		S2.Settings = Kept;
		U->TestStart(S2);
		Pump(U, 2.0);
		U->Tick(0.02f);
		Line(U->TestPromptUp(), TEXT("skip: a later build is offered"));
		U->TestStop();

		// bad hash: a clear word, the install untouched, the bad file gone
		UOutlawsUpdate::FTestRun H = RunOf(Dir, TEXT("feed_badhash"), Root, 45);
		U->TestStart(H);
		Pump(U, 2.0);
		U->Tick(0.02f);
		U->Pick(0);
		Pump(U);
		TArray<FString> Zips;
		IFileManager::Get().FindFiles(Zips, *FPaths::Combine(Root, TEXT("Update"), TEXT("*.zip")), true, false);
		Line(U->TestGetStep() == EStep::Failed && U->TestWhy().Contains(TEXT("hash")) && Zips.Num() == 0 && Same(FPaths::Combine(Root, TEXT("OutlawsUE.exe")), TEXT("old exe")),
			FString::Printf(TEXT("bad hash: \"%s\" (%s), the zip gone, the install as it was"), *U->TestWords(), *U->TestWhy()));
		U->Pick(1);
		Line(U->TestGetStep() == EStep::Done && !UOutlawsUpdate::BubbleUp(U), TEXT("bad hash: X closes it"));
		U->TestStop();

		// a build without the theme is not put in
		U->TestStart(RunOf(Dir, TEXT("feed_notheme"), Root, 45));
		Pump(U, 2.0);
		U->Tick(0.02f);
		U->Pick(0);
		Pump(U);
		Line(U->TestGetStep() == EStep::Failed && U->TestWhy().Contains(TEXT("menu theme")) && !FPaths::FileExists(FPaths::Combine(Root, TEXT("Update"), TEXT("swap.cmd"))), FString::Printf(TEXT("a zip without the theme refused (%s)"), *U->TestWhy()));
		U->TestStop();
	}

	void CasePc(UOutlawsUpdate* U, const FString& Dir)
	{
		const FString Root = FreshRoot(Dir, TEXT("pc"));
		const FString Saved = FPaths::Combine(Root, TEXT("OutlawsUE"), TEXT("Saved"), TEXT("OutlawsSaves"), TEXT("keep.sav"));
		// a stopped download first: the first chunk there, the rest fetched
		FString Sha;
		FFileHelper::LoadFileToString(Sha, *FPaths::Combine(Dir, TEXT("feed_new"), TEXT("zip.sha256")));
		Sha.TrimStartAndEndInline();
		TArray<uint8> All;
		FFileHelper::LoadFileToArray(All, *FPaths::Combine(Dir, TEXT("feed_new"), TEXT("OutlawsVR-fake.zip")));
		const FString Part = FPaths::Combine(Root, TEXT("Update"), Sha + TEXT(".zip.part"));
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Part), true);
		FFileHelper::SaveArrayToFile(TArrayView<const uint8>(All.GetData(), 8 * 1024 * 1024), *Part);
		U->TestStart(RunOf(Dir, TEXT("feed_new"), Root, 45));
		Pump(U, 2.0);
		U->Tick(0.02f);
		U->Pick(0);
		Pump(U, 60.0);
		Line(U->TestGetStep() == EStep::Exiting && FPaths::FileExists(FPaths::Combine(Root, TEXT("Update"), TEXT("swap.cmd"))), FString::Printf(TEXT("update now: resumed, hash fine, unpacked, swap.cmd written, step %d %s"), int32(U->TestGetStep()), *U->TestWhy()));
		U->TestStop();
		const int32 Code = RunSwap(Root);
		Line(Code == 0 && FPaths::FileExists(FPaths::Combine(Root, TEXT("Update"), TEXT("done"))), FString::Printf(TEXT("swap.cmd ran, exit %d"), Code));
		Line(Same(FPaths::Combine(Root, TEXT("OutlawsUE.exe")), TEXT("new exe")), TEXT("the new exe is in"));
		Line(Same(FPaths::Combine(Root, TEXT("Tools"), TEXT("MenuOverlay.kpf")), TEXT("new theme")), TEXT("the menu theme is the new build's"));
		Line(Same(FPaths::Combine(Root, TEXT("Tools"), TEXT("NativeMaps"), TEXT("mapa"), TEXT("mapa.rcm")), TEXT("map a new")), TEXT("the native maps are the new build's"));
		Line(FPaths::FileExists(FPaths::Combine(Root, TEXT("Tools"), TEXT("NativeMaps"), TEXT("mapb"), TEXT("mapb.lab"))), TEXT("all the native maps are there"));
		Line(Same(Saved, TEXT("the player's save")), TEXT("Saved kept, in the build's own folder too"));
		Line(Same(FPaths::Combine(Root, TEXT("my_note.txt")), TEXT("the player's note")), TEXT("the player's own file kept"));
		Line(!FPaths::FileExists(FPaths::Combine(Root, TEXT("OutlawsUE"), TEXT("Content"), TEXT("Paks"), TEXT("stale_old.pak"))), TEXT("a pak the new build dropped is gone"));
		Line(Same(FPaths::Combine(Root, TEXT("relaunched.txt")), TEXT("relaunched")), TEXT("the game relaunched through Play Outlaws VR.bat"));
		Line(Same(FPaths::Combine(Root, TEXT("Update"), TEXT("old"), TEXT("OutlawsUE.exe")), TEXT("old exe")), TEXT("the old build kept for the way back"));

		// a new build that is not whole: nothing changes
		const FString Bad = FreshRoot(Dir, TEXT("pcbad"));
		IFileManager::Get().MakeDirectory(*FPaths::Combine(Bad, TEXT("Update"), TEXT("new")), true);
		const int32 BadCode = RunSwap(Bad);
		Line(BadCode == 0 && FPaths::FileExists(FPaths::Combine(Bad, TEXT("Update"), TEXT("failed"))) && Same(FPaths::Combine(Bad, TEXT("OutlawsUE.exe")), TEXT("old exe")) && Same(FPaths::Combine(Bad, TEXT("Tools"), TEXT("MenuOverlay.kpf")), TEXT("old theme")),
			TEXT("swap.cmd with no whole build: failed marked, the install as it was"));

		// a copy that fails half way (a read-only file in the way would need a lock; a folder where a file goes does it): back to the old
		const FString Back = FreshRoot(Dir, TEXT("pcback"));
		FPlatformFileManager::Get().GetPlatformFile().CopyDirectoryTree(*FPaths::Combine(Back, TEXT("Update"), TEXT("new")), *FPaths::Combine(Dir, TEXT("whole")), true);
		IFileManager::Get().Delete(*FPaths::Combine(Back, TEXT("Tools"), TEXT("MenuOverlay.kpf")));
		IFileManager::Get().MakeDirectory(*FPaths::Combine(Back, TEXT("Tools"), TEXT("MenuOverlay.kpf"), TEXT("blocker")), true);
		const int32 BackCode = RunSwap(Back);
		Line(BackCode == 0 && FPaths::FileExists(FPaths::Combine(Back, TEXT("Update"), TEXT("failed"))) && Same(FPaths::Combine(Back, TEXT("OutlawsUE.exe")), TEXT("old exe")) && FPaths::FileExists(FPaths::Combine(Back, TEXT("OutlawsUE"), TEXT("Content"), TEXT("Paks"), TEXT("stale_old.pak"))),
			FString::Printf(TEXT("swap.cmd with a copy that fails: back to the old build, exit %d"), BackCode));
	}

	void CaseQuest(UOutlawsUpdate* U, const FString& Dir)
	{
		const FString Root = FPaths::Combine(Dir, TEXT("run"), TEXT("quest"));
		IFileManager::Get().DeleteDirectory(*Root, false, true);
		IFileManager::Get().MakeDirectory(*Root, true);
		UOutlawsUpdate::FTestRun R = RunOf(Dir, TEXT("feed_new"), Root, 45);
		R.bQuest = true;
		U->TestStart(R);
		Pump(U, 2.0);
		U->Tick(0.02f);
		U->Pick(0);
		Pump(U, 60.0);
		const FString Base = FPaths::Combine(Root, TEXT("OutlawsUpdate"));
		Line(U->TestGetStep() == EStep::Exiting, FString::Printf(TEXT("Quest: fetched, checked, unpacked, step %d %s"), int32(U->TestGetStep()), *U->TestWhy()));
		U->TestStop();
		Line(Same(FPaths::Combine(Base, TEXT("data"), TEXT("MenuOverlay.kpf")), TEXT("new theme")), TEXT("Quest: the theme in the app's folder"));
		Line(Same(FPaths::Combine(Base, TEXT("data"), TEXT("NativeMaps"), TEXT("mapa"), TEXT("mapa.rcm")), TEXT("map a new")), TEXT("Quest: the maps in the app's folder"));
		Line(!OLUpdate::UpdatedDataIn(Base, 50).IsEmpty(), TEXT("Quest: the new build reads them (stamp 50)"));
		Line(FPaths::FileExists(FPaths::Combine(Base, TEXT("download"), TEXT("OutlawsVR.apk"))), TEXT("Quest: the APK ready for the installer, its hash checked"));

		UOutlawsUpdate::FTestRun A = RunOf(Dir, TEXT("feed_badapk"), Root, 45);
		A.bQuest = true;
		U->TestStart(A);
		Pump(U, 2.0);
		U->Tick(0.02f);
		U->Pick(0);
		Pump(U, 60.0);
		Line(U->TestGetStep() == EStep::Failed && U->TestWhy().Contains(TEXT("apk")), FString::Printf(TEXT("Quest: an APK that is not the one named is refused (%s)"), *U->TestWhy()));
		U->TestStop();
	}

	void CaseReal(UOutlawsUpdate* U, const FString& Dir)
	{
		// the real feed, whatever is there today (no latest.json before 0.96's): silent with a number above any release
		UOutlawsUpdate::FTestRun R;
		R.Root = FPaths::Combine(Dir, TEXT("run"), TEXT("real"));
		R.Build = 1000000;
		U->TestStart(R);
		Pump(U, 20.0);
		Line(U->TestGetStep() == EStep::Done && !U->TestPromptUp(), FString::Printf(TEXT("the real feed: silent, step %d"), int32(U->TestGetStep())));
		U->TestStop();
		// a Range read of the public zip through GitHub's redirect, as the download does
		int32 Code = 0, Size = 0;
		bool bPk = false, bDone = false;
		const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Rq = FHttpModule::Get().CreateRequest();
		Rq->SetURL(TEXT("https://github.com/outlawshub-arch/OutlawsVR/releases/download/v0.95/OutlawsVR-v0.95-hotfix-0bbe48c.zip"));
		Rq->SetVerb(TEXT("GET"));
		Rq->SetHeader(TEXT("User-Agent"), TEXT("OutlawsVR"));
		Rq->SetHeader(TEXT("Range"), TEXT("bytes=0-1023"));
		Rq->SetTimeout(30.f);
		Rq->OnProcessRequestComplete().BindLambda([&](FHttpRequestPtr, FHttpResponsePtr Rs, bool bOk)
		{
			bDone = true;
			if (!bOk || !Rs.IsValid()) return;
			Code = Rs->GetResponseCode();
			Size = Rs->GetContent().Num();
			bPk = Size > 2 && Rs->GetContent()[0] == 'P' && Rs->GetContent()[1] == 'K';
		});
		Rq->ProcessRequest();
		const double End = FPlatformTime::Seconds() + 35.0;
		while (!bDone && FPlatformTime::Seconds() < End)
		{
			FHttpModule::Get().GetHttpManager().Tick(0.02f);
			FPlatformProcess::Sleep(0.01f);
		}
		if (!bDone) Rq->CancelRequest();
		Line(Code == 206 && Size == 1024 && bPk, FString::Printf(TEXT("a Range read of the public zip: status %d, %d bytes, %s"), Code, Size, bPk ? TEXT("PK") : TEXT("not a zip's start")));
	}

	bool ProbeTick(float, TWeakObjectPtr<UOutlawsUpdate> Weak)
	{
		UOutlawsUpdate* U = Weak.Get();
		if (U)
		{
			FString Dir;
			OLFlags::Value(TEXT("OLUpdateProbeDir"), Dir, /*bShouldStopOnSeparator*/ false);
			Dir = FPaths::ConvertRelativePathToFull(Dir.TrimQuotes());
			if (OLFlags::Param(TEXT("OLOldUpdater")))
			{
				UOutlawsUpdate::FTestRun R = RunOf(Dir, TEXT("feed_new"), FreshRoot(Dir, TEXT("old")), 45);
				R.bOld = true;
				U->TestStart(R);
				Pump(U, 2.0);
				Line(U->TestGetStep() == EStep::Off && !U->TestPromptUp(), TEXT("-OLOldUpdater: no check at all"));
				Line(!U->IsTickable(), TEXT("-OLOldUpdater: not even ticking"));
				U->TestStop();
			}
			else
			{
				CasesPure(Dir);
				CasesFlow(U, Dir);
				CasePc(U, Dir);
				CaseQuest(U, Dir);
				if (!OLFlags::Param(TEXT("OLUpdateProbeNoNet"))) CaseReal(U, Dir);
			}
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT update RESULT %s (%d passed, %d failed)"), GFail == 0 && GPass > 0 ? TEXT("PASS") : TEXT("FAIL"), GPass, GFail);
		FPlatformMisc::RequestExit(false);
		return false;
	}
}

void OLUpdateStartProbe(UOutlawsUpdate* U)
{
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&ProbeTick, TWeakObjectPtr<UOutlawsUpdate>(U)), 6.0f);
}

#endif // WITH_OUTLAWS_TESTS
