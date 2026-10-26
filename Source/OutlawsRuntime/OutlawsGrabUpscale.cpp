// REQ-GRABUPSCALE: the held frames' 2x upscales, made on the player's PC and shown while a bad guy is held (see the header).
#include "OutlawsGrabUpscale.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsLab.h"
#include "OutlawsFormats.h"
#include "OutlawsTextures.h"
#include "OutlawsEnemy.h"
#include "OutlawsLevelActor.h"
#include "OutlawsSpriteActor.h"
#include "Async/Async.h"
#include "Containers/Ticker.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include <atomic>

// (inside OutlawsGrabUpscale: the unity build sees these names from no other file)
namespace OutlawsGrabUpscale { namespace
{
	TAutoConsoleVariable<int32> CVarGrabUpscale(TEXT("ol.GrabUpscale"), 1,
		TEXT("REQ-GRABUPSCALE: 1 a held bad guy shows the 2x upscales of his held frames where the PC made them; 0 the HD atlas (none made)."));

	// the stock sets' run is done for this install (the front end does not look again on later launches)
	const TCHAR* const MarkerName = TEXT("made_v1.txt");

	FCriticalSection JobLock;
	struct FJob
	{
		TSharedPtr<FOutlawsAssets> Assets;
		TArray<TPair<FString, int32>> Sets;
		bool bAll = false;
		FString Why;
	};
	TArray<FJob> Queue;
	bool bRunning = false;
	TSet<FString> Asked;                  // sets already handed to a job this session (no second try per launch)
	std::atomic<int32> Done{ 0 };
	std::atomic<int32> TmpSerial{ 0 };

	IImageWrapperModule* ImageModule()
	{
		static IImageWrapperModule* M = nullptr;
		if (!M && IsInGameThread()) M = &FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		return M;
	}

	bool SavePng(IImageWrapperModule& IW, const FString& Path, const TArray<FColor>& Px, int32 W, int32 H)
	{
		TSharedPtr<IImageWrapper> Png = IW.CreateImageWrapper(EImageFormat::PNG);
		if (!Png || !Png->SetRaw(Px.GetData(), int64(Px.Num()) * sizeof(FColor), W, H, ERGBFormat::BGRA, 8)) return false;
		const TArray64<uint8>& Bytes = Png->GetCompressed(0);
		return Bytes.Num() > 0 && FFileHelper::SaveArrayToFile(Bytes, *Path);
	}

	bool LoadPng(IImageWrapperModule& IW, const FString& Path, TArray<FColor>& Px, int32& W, int32& H)
	{
		TArray<uint8> File;
		if (!FFileHelper::LoadFileToArray(File, *Path)) return false;
		TSharedPtr<IImageWrapper> Png = IW.CreateImageWrapper(EImageFormat::PNG);
		TArray64<uint8> Raw;
		if (!Png || !Png->SetCompressed(File.GetData(), File.Num()) || !Png->GetRaw(ERGBFormat::BGRA, 8, Raw)) return false;
		W = int32(Png->GetWidth());
		H = int32(Png->GetHeight());
		if (W <= 0 || H <= 0 || Raw.Num() != int64(W) * H * 4) return false;
		Px.SetNumUninitialized(W * H);
		FMemory::Memcpy(Px.GetData(), Raw.GetData(), Raw.Num());
		return true;
	}

	// prep.py: the drawn pixels' colours spread into the clear area, 24 steps of the 4 neighbours' mean, so the upscaler
	// sees no black round the figure (a dark halo once the alpha comes back)
	void BleedEdges(TArray<FColor>& Px, int32 W, int32 H)
	{
		TArray<uint8> Known;
		Known.SetNumUninitialized(W * H);
		for (int32 i = 0; i < W * H; ++i) Known[i] = Px[i].A > 0 ? 1 : 0;
		TArray<int32> Fill;
		for (int32 Step = 0; Step < 24; ++Step)
		{
			Fill.Reset();
			TArray<FColor> NewC;
			for (int32 y = 0; y < H; ++y)
			{
				for (int32 x = 0; x < W; ++x)
				{
					const int32 i = y * W + x;
					if (Known[i]) continue;
					int32 R = 0, G = 0, B = 0, N = 0;
					const int32 Nb[4][2] = { { x - 1, y }, { x + 1, y }, { x, y - 1 }, { x, y + 1 } };
					for (const auto& P : Nb)
					{
						if (P[0] < 0 || P[0] >= W || P[1] < 0 || P[1] >= H) continue;
						const int32 j = P[1] * W + P[0];
						if (!Known[j]) continue;
						R += Px[j].R; G += Px[j].G; B += Px[j].B; ++N;
					}
					if (N == 0) continue;
					Fill.Add(i);
					NewC.Add(FColor(uint8(R / N), uint8(G / N), uint8(B / N), 0));
				}
			}
			if (Fill.Num() == 0) break;
			for (int32 k = 0; k < Fill.Num(); ++k)
			{
				Px[Fill[k]] = NewC[k];
				Known[Fill[k]] = 1;
			}
		}
	}

	// a 2x Lanczos-3 resize of one channel (merge.py's alpha resize)
	void Lanczos2x(const TArray<float>& In, int32 W, int32 H, TArray<float>& Out)
	{
		auto L3 = [](double X) -> double
		{
			X = FMath::Abs(X);
			if (X < 1e-8) return 1.0;
			if (X >= 3.0) return 0.0;
			const double P = PI * X;
			return 3.0 * FMath::Sin(P) * FMath::Sin(P / 3.0) / (P * P);
		};
		// the two phases of a 2x resize: destination 2i samples the source at i - 0.25 (taps i-3..i+2), 2i + 1 at i + 0.25
		// (taps i-2..i+3)
		double Wt[2][6];
		for (int32 Ph = 0; Ph < 2; ++Ph)
		{
			double Sum = 0.0;
			for (int32 k = 0; k < 6; ++k) { Wt[Ph][k] = L3((Ph == 0 ? 2.75 : 2.25) - k); Sum += Wt[Ph][k]; }
			for (int32 k = 0; k < 6; ++k) Wt[Ph][k] /= Sum;
		}
		const int32 W2 = W * 2, H2 = H * 2;
		TArray<float> Mid;
		Mid.SetNumUninitialized(W2 * H);
		for (int32 y = 0; y < H; ++y)
		{
			for (int32 x2 = 0; x2 < W2; ++x2)
			{
				const int32 Ph = x2 & 1, Base = (x2 >> 1) - (Ph == 0 ? 3 : 2);
				double V = 0.0;
				for (int32 k = 0; k < 6; ++k) V += Wt[Ph][k] * In[y * W + FMath::Clamp(Base + k, 0, W - 1)];
				Mid[y * W2 + x2] = float(V);
			}
		}
		Out.SetNumUninitialized(W2 * H2);
		for (int32 y2 = 0; y2 < H2; ++y2)
		{
			const int32 Ph = y2 & 1, Base = (y2 >> 1) - (Ph == 0 ? 3 : 2);
			for (int32 x2 = 0; x2 < W2; ++x2)
			{
				double V = 0.0;
				for (int32 k = 0; k < 6; ++k) V += Wt[Ph][k] * Mid[FMath::Clamp(Base + k, 0, H - 1) * W2 + x2];
				Out[y2 * W2 + x2] = float(V);
			}
		}
	}

	// every stock bad guy that can be held: an ITM with a _New logic of a person (not the generators, the nav nodes or the
	// chickens and dinosaurs, which kick in the hand on their walk), a picture of its own, man height (critters play their walk)
	void StockSets(const FOutlawsAssets& A, TArray<TPair<FString, int32>>& Out)
	{
		TArray<FString> Itms;
		A.ListByExtension(TEXT(".itm"), Itms);
		Itms.Sort();
		TSet<FString> Seen;
		for (const FString& Name : Itms)
		{
			FString Text;
			const FString Anim = OutlawsFormats::ResolveAnim(A, FPaths::GetBaseFilename(Name), &Text);
			if (!Anim.EndsWith(TEXT(".nwx"), ESearchCase::IgnoreCase)) continue;
			FString Func;
			TMap<FString, FString> Data;
			OutlawsFormats::ParseItm(Text, Func, Data);
			Func.ToLowerInline();
			if (!Func.EndsWith(TEXT("_new")) || Func == TEXT("gen_new") || Func == TEXT("node_new") || Func == TEXT("chicken_new")) continue;
			const float Height = Data.Contains(TEXT("HEIGHT")) ? FCString::Atof(*Data[TEXT("HEIGHT")]) : 6.f;
			if (Height < 4.5f) continue;             // (AOutlawsEnemy::IsCritter)
			const int32 Hit = Data.Contains(TEXT("HIT_CHOR")) ? FCString::Atoi(*Data[TEXT("HIT_CHOR")]) : 20;
			const FString Key = Anim.ToLower() + TEXT("|") + FString::FromInt(Hit);
			if (Seen.Contains(Key)) continue;
			Seen.Add(Key);
			Out.Emplace(Anim.ToLower(), Hit);
		}
	}

	struct FItem
	{
		FString Name;            // <stem>_<frame>
		FString Final;
		TArray<FColor> Hd;       // the 4x HD crop, its own alpha
		int32 W = 0, H = 0;
	};

	// one job: the sets' missing upscales made (any thread; blocking).  True when nothing failed.
	bool RunJob(const FJob& Job, IImageWrapperModule& IW)
	{
		const double T0 = FPlatformTime::Seconds();
		const FOutlawsAssets& A = *Job.Assets;
		const FString Exe = OutlawsGrabUpscale::ToolExe();
		if (Exe.IsEmpty())
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLRT grabup: no upscaler beside the game (ThirdParty/RealESRGAN): the held bad guys keep the HD frame"));
			return false;
		}
		TArray<TPair<FString, int32>> Sets = Job.Sets;
		if (Job.bAll) StockSets(A, Sets);
		UE_LOG(LogOutlaws, Log, TEXT("OLRT grabup: %s: looking over %d sprite sets"), *Job.Why, Sets.Num());
		TArray<FColor> Palette;
		OutlawsFormats::FindSpritePalette(A, Palette);
		const FString OutDir = OutlawsGrabUpscale::OutDir();
		IFileManager::Get().MakeDirectory(*OutDir, true);
		const bool bDump = OLFlags::Param(TEXT("OLGrabUpscaleDump"));
		const FString DumpDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("GrabUpscaleDump"));
		if (bDump) IFileManager::Get().MakeDirectory(*DumpDir, true);
		TArray<FItem> Items;
		int32 NoHd = 0, Have = 0, NoSprite = 0;
		for (const TPair<FString, int32>& S : Sets)
		{
			const TSharedPtr<OutlawsTextures::FSpriteAtlas> Atlas = OutlawsTextures::BuildSpriteAtlas(A, S.Key, Palette, false, true);
			if (!Atlas) { ++NoSprite; continue; }
			TArray<int32> Cells;
			OutlawsGrabUpscale::HeldCells(*Atlas, S.Value, Cells);
			TArray<int32> Todo;
			for (int32 c : Cells)
			{
				const FString F = OutlawsGrabUpscale::FileName(S.Key, Atlas->CellFrame[c]);
				if (FPaths::FileExists(OutDir / F)) ++Have;
				else Todo.Add(c);
			}
			if (Todo.Num() == 0) continue;
			TArray<uint8> Spx;
			TArray<OutlawsFormats::FSprxFrame> Frames;
			if (!A.Get(FPaths::GetBaseFilename(S.Key) + TEXT(".spx"), Spx) || !OutlawsFormats::ParseSprx(Spx, Frames))
			{
				++NoHd;                           // (no remaster: skipped, the hold shows the original)
				continue;
			}
			for (int32 c : Todo)
			{
				FItem It;
				if (!OutlawsTextures::DecodeSpriteCellHd(Spx, Frames, *Atlas, c, It.Hd, It.W, It.H)) { ++NoHd; continue; }
				It.Name = FPaths::GetBaseFilename(OutlawsGrabUpscale::FileName(S.Key, Atlas->CellFrame[c]));
				It.Final = OutDir / It.Name + TEXT(".png");
				Items.Add(MoveTemp(It));
			}
		}
		if (Items.Num() == 0)
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLRT grabup: %s: nothing to make (%d sets, %d upscales already there, %d without an HD sprite, %d without a sprite)"),
				*Job.Why, Sets.Num(), Have, NoHd, NoSprite);
			return true;
		}
		// the HD crops, edges bled, into a folder of their own; the upscaler takes the folder in one run (one Vulkan start)
		const FString Tmp = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPlatformProcess::UserTempDir(),
			FString::Printf(TEXT("OutlawsVR_grabup_%u_%d"), FPlatformProcess::GetCurrentProcessId(), TmpSerial++)));
		const FString InDir = Tmp / TEXT("in"), UpDir = Tmp / TEXT("up");
		IFileManager::Get().DeleteDirectory(*Tmp, false, true);
		IFileManager::Get().MakeDirectory(*InDir, true);
		IFileManager::Get().MakeDirectory(*UpDir, true);
		for (FItem& It : Items)
		{
			TArray<FColor> Rgb = It.Hd;
			BleedEdges(Rgb, It.W, It.H);
			for (FColor& C : Rgb) C.A = 255;
			SavePng(IW, InDir / It.Name + TEXT(".png"), Rgb, It.W, It.H);
			if (bDump) SavePng(IW, DumpDir / It.Name + TEXT("_hd.png"), It.Hd, It.W, It.H);
		}
		const FString Models = FPaths::GetPath(Exe) / TEXT("models");
		// the upscaler's tiles: 128 px (its own choice on a 16 GB RTX 5070 Ti, 400 px and 256 px, lost the Vulkan device in a
		// folder run and wrote black pictures, 13 Oct); what still comes out wrong is tried again one at a time at 64 px
		FString Said;
		int32 Code = -1;
		bool bTimedOut = false;
		auto RunTool = [&](const FString& From, const FString& To, int32 Tile, const TCHAR* Threads)
		{
			const FString Args = FString::Printf(TEXT("-i \"%s\" -o \"%s\" -n realesr-animevideov3 -s 2 -m \"%s\" -f png -t %d -j %s"), *From, *To, *Models, Tile, Threads);
			void* ReadPipe = nullptr;
			void* WritePipe = nullptr;
			FPlatformProcess::CreatePipe(ReadPipe, WritePipe);
			const double TRun = FPlatformTime::Seconds();
			FProcHandle P = FPlatformProcess::CreateProc(*Exe, *Args, false, true, true, nullptr, -1, *FPaths::GetPath(Exe), WritePipe, nullptr);
			if (P.IsValid())
			{
				while (FPlatformProcess::IsProcRunning(P))
				{
					Said += FPlatformProcess::ReadPipe(ReadPipe);
					if (FPlatformTime::Seconds() - TRun > 300.0)
					{
						bTimedOut = true;
						FPlatformProcess::TerminateProc(P, true);
						break;
					}
					FPlatformProcess::Sleep(0.05f);
				}
				Said += FPlatformProcess::ReadPipe(ReadPipe);
				FPlatformProcess::GetProcReturnCode(P, &Code);
				FPlatformProcess::CloseProc(P);
			}
			else
			{
				Said += TEXT("(the upscaler would not start) ");
			}
			FPlatformProcess::ClosePipe(ReadPipe, WritePipe);
		};
		const double TRun = FPlatformTime::Seconds();
		TArray<TArray<FColor>> UpPx;
		UpPx.SetNum(Items.Num());
		TArray<int32> Left;
		for (int32 i = 0; i < Items.Num(); ++i) Left.Add(i);
		int32 Tries = 0;
		for (int32 Pass = 0; Pass < 2 && Left.Num() > 0; ++Pass)
		{
			FString From = InDir, To = UpDir;
			if (Pass > 0)
			{
				From = Tmp / TEXT("in2");
				To = Tmp / TEXT("up2");
				IFileManager::Get().MakeDirectory(*From, true);
				IFileManager::Get().MakeDirectory(*To, true);
				for (int32 i : Left) IFileManager::Get().Copy(*(From / Items[i].Name + TEXT(".png")), *(InDir / Items[i].Name + TEXT(".png")));
			}
			++Tries;
			RunTool(From, To, Pass == 0 ? 128 : 64, Pass == 0 ? TEXT("1:2:2") : TEXT("1:1:1"));
			TArray<int32> Still;
			for (int32 i : Left)
			{
				const FItem& It = Items[i];
				TArray<FColor> Up;
				int32 UW = 0, UH = 0;
				bool bOk = LoadPng(IW, To / It.Name + TEXT(".png"), Up, UW, UH) && UW == It.W * 2 && UH == It.H * 2;
				if (bOk)
				{
					// (a lost device writes black: some colour must have come out where the HD frame has some)
					uint32 Any = 0;
					for (const FColor& C : Up) Any |= uint32(C.R) | C.G | C.B;
					bOk = Any != 0;
				}
				if (bOk) UpPx[i] = MoveTemp(Up);
				else Still.Add(i);
			}
			Left = MoveTemp(Still);
		}
		const double RunS = FPlatformTime::Seconds() - TRun;
		// merge.py: the upscaled colour, and the HD alpha resized with Lanczos and sharpened ((a - 0.35) / 0.3), a 1 to 2 px soft edge
		int32 Made = 0, Failed = 0;
		int64 Bytes = 0;
		for (int32 Ix = 0; Ix < Items.Num(); ++Ix)
		{
			const FItem& It = Items[Ix];
			TArray<FColor>& Up = UpPx[Ix];
			const int32 UW = It.W * 2, UH = It.H * 2;
			if (Up.Num() != UW * UH)
			{
				++Failed;
				continue;
			}
			TArray<float> Alpha, Alpha2;
			Alpha.SetNumUninitialized(It.W * It.H);
			for (int32 i = 0; i < It.W * It.H; ++i) Alpha[i] = It.Hd[i].A / 255.f;
			Lanczos2x(Alpha, It.W, It.H, Alpha2);
			for (int32 i = 0; i < UW * UH; ++i)
			{
				const float Av = FMath::Clamp((Alpha2[i] - 0.35f) / 0.3f, 0.f, 1.f);
				Up[i].A = uint8(FMath::RoundToInt(Av * 255.f));
			}
			const FString Part = It.Final + TEXT(".part");
			if (SavePng(IW, Part, Up, UW, UH) && IFileManager::Get().Move(*It.Final, *Part, true, true))
			{
				++Made;
				Bytes += IFileManager::Get().FileSize(*It.Final);
				if (bDump) IFileManager::Get().Copy(*(DumpDir / It.Name + TEXT("_up.png")), *It.Final);
			}
			else
			{
				IFileManager::Get().Delete(*Part, false, true, true);
				++Failed;
			}
		}
		if (!bDump) IFileManager::Get().DeleteDirectory(*Tmp, false, true);
		Said.TrimStartAndEndInline();
		UE_LOG(LogOutlaws, Log, TEXT("OLRT grabup: %s: %d of %d upscales made (%d failed, %d already there, %d without an HD sprite) in %.1f s (upscaler %.1f s in %d run%s, exit %d%s), %.2f MB, into %s"),
			*Job.Why, Made, Items.Num(), Failed, Have, NoHd, FPlatformTime::Seconds() - T0, RunS, Tries, Tries == 1 ? TEXT("") : TEXT("s"), Code, bTimedOut ? TEXT(", timed out") : TEXT(""),
			Bytes / 1048576.0, *OutDir);
		if (Failed > 0)
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLRT grabup: the upscaler said: %s"), *Said.Right(600).Replace(TEXT("\r"), TEXT(" ")).Replace(TEXT("\n"), TEXT(" | ")));
		}
		return Failed == 0;
	}

	void WriteMarker(const FJob& Job)
	{
		FFileHelper::SaveStringToFile(Job.Assets->GetRoot(), *(OutlawsGrabUpscale::OutDir() / MarkerName));
	}

	void PumpQueue()
	{
		FJob Job;
		{
			FScopeLock Guard(&JobLock);
			if (bRunning || Queue.Num() == 0) return;
			Job = Queue[0];
			Queue.RemoveAt(0);
			bRunning = true;
		}
		IImageWrapperModule* IW = ImageModule();
		if (!IW)
		{
			FScopeLock Guard(&JobLock);
			bRunning = false;
			return;
		}
		Async(EAsyncExecution::Thread, [Job, IW]()
		{
			const bool bOk = RunJob(Job, *IW);
			if (bOk && Job.bAll) WriteMarker(Job);
			{
				FScopeLock Guard(&JobLock);
				bRunning = false;
			}
			++Done;
			AsyncTask(ENamedThreads::GameThread, []() { PumpQueue(); });
		});
	}
} }

bool OutlawsGrabUpscale::IsOff()
{
	static const bool bOffParam = OLFlags::Param(TEXT("OLGrabUpscaleOff"));
	return bOffParam || CVarGrabUpscale.GetValueOnAnyThread() == 0;
}

bool OutlawsGrabUpscale::PretendMissing()
{
	static const bool b = OLFlags::Param(TEXT("OLGrabUpscaleMissing"));
	return b;
}

FString OutlawsGrabUpscale::FileName(const FString& Nwx, int32 Frame)
{
	return FString::Printf(TEXT("%s_%d.png"), *FPaths::GetBaseFilename(Nwx).ToLower(), Frame);
}

FString OutlawsGrabUpscale::Find(const FString& InstallRoot, const FString& File)
{
	if (IsOff() || PretendMissing()) return FString();
	if (!InstallRoot.IsEmpty())
	{
		const FString Beside = FPaths::ConvertRelativePathToFull(FPaths::Combine(InstallRoot, TEXT("Grab"), File));
		if (FPaths::FileExists(Beside)) return Beside;
	}
#if PLATFORM_WINDOWS
	const FString Mine = OutDir() / File;
	if (FPaths::FileExists(Mine)) return Mine;
#endif
	return FString();
}

void OutlawsGrabUpscale::HeldCells(const OutlawsTextures::FSpriteAtlas& Atlas, int32 HitChor, TArray<int32>& Out)
{
	Out.Reset();
	if (!Atlas.Chors.IsValidIndex(HitChor)) return;
	const OutlawsTextures::FSpriteAtlas::FChor& C = Atlas.Chors[HitChor];
	const int32 Dirs = FMath::Max(1, C.Dirs);
	for (int32 d : { 0, Dirs / 2 })
	{
		const int32 i = C.GridStart + d * FMath::Max(1, C.Frames);
		if (!Atlas.ChorGrid.IsValidIndex(i)) continue;
		const int32 Cell = Atlas.ChorGrid[i];
		if (Atlas.CellFrame.IsValidIndex(Cell)) Out.AddUnique(Cell);
	}
}

FString OutlawsGrabUpscale::OutDir()
{
	return FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("OutlawsVR"), TEXT("media"), TEXT("Grab"));
}

FString OutlawsGrabUpscale::ToolExe()
{
#if PLATFORM_WINDOWS
	const FString Exe = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("ThirdParty/RealESRGAN/realesrgan-ncnn-vulkan.exe"));
	if (FPaths::FileExists(Exe) && FPaths::FileExists(FPaths::GetPath(Exe) / TEXT("models/realesr-animevideov3-x2.param"))) return Exe;
#endif
	return FString();
}

void OutlawsGrabUpscale::Kick(const TSharedPtr<FOutlawsAssets>& Assets, const TArray<TPair<FString, int32>>& Sets, const FString& Why)
{
#if PLATFORM_WINDOWS
	if (!Assets || IsOff() || PretendMissing() || ToolExe().IsEmpty()) return;
	FJob Job;
	Job.Assets = Assets;
	Job.Why = Why;
	{
		FScopeLock Guard(&JobLock);
		for (const TPair<FString, int32>& S : Sets)
		{
			const FString Key = S.Key.ToLower() + TEXT("|") + FString::FromInt(S.Value);
			if (Asked.Contains(Key)) continue;
			Asked.Add(Key);
			Job.Sets.Add(S);
		}
		if (Job.Sets.Num() == 0) return;
		Queue.Add(MoveTemp(Job));
	}
	PumpQueue();
#endif
}

void OutlawsGrabUpscale::KickAll(const TSharedPtr<FOutlawsAssets>& Assets, const FString& Why)
{
#if PLATFORM_WINDOWS
	if (!Assets || IsOff() || PretendMissing() || ToolExe().IsEmpty()) return;
	FString Was;
	if (FFileHelper::LoadFileToString(Was, *(OutDir() / MarkerName)) && Was == Assets->GetRoot()) return;     // (done for this install)
	{
		FScopeLock Guard(&JobLock);
		if (Asked.Contains(TEXT("*all*"))) return;
		Asked.Add(TEXT("*all*"));
		FJob Job;
		Job.Assets = Assets;
		Job.bAll = true;
		Job.Why = Why;
		Queue.Add(MoveTemp(Job));
	}
	PumpQueue();
#endif
}

bool OutlawsGrabUpscale::RunAllNow(const TSharedPtr<FOutlawsAssets>& Assets)
{
#if PLATFORM_WINDOWS
	IImageWrapperModule* IW = ImageModule();
	if (!Assets || !IW || IsOff()) return false;
	FJob Job;
	Job.Assets = Assets;
	Job.bAll = true;
	Job.Why = TEXT("the converter (every stock bad guy)");
	const bool bOk = RunJob(Job, *IW);
	if (bOk) WriteMarker(Job);
	++Done;
	return bOk;
#else
	return false;
#endif
}

bool OutlawsGrabUpscale::IsBusy()
{
	FScopeLock Guard(&JobLock);
	return bRunning || Queue.Num() > 0;
}

int32 OutlawsGrabUpscale::JobsDone()
{
	return Done.load();
}

// ---- the held look ---------------------------------------------------------------------------------------------------

void AOutlawsEnemy::TickGrabUpscale()
{
	const bool bWant = Sprite && bHeldByPlayer && !bThrown && !IsCritter();
	if (bWant == bGrabUpOn) return;
	bGrabUpOn = bWant;
	++GrabUpGen;
	if (!bWant)
	{
		if (Sprite) Sprite->ClearHeldLook();
		UE_LOG(LogOutlaws, Log, TEXT("OLRT grabup: %s let go: the atlas frame again"), *OutlawsName);
		return;
	}
	AOutlawsLevelActor* L = Level.Get();
	const FOutlawsAssets* A = L ? L->GetLabAssets() : nullptr;
	const TCHAR* Why = !L || !A ? TEXT("no level") : !AtlasRef.IsValid() ? TEXT("no atlas") : !L->bHDTextures ? TEXT("Remastered graphics off")
		: OutlawsGrabUpscale::IsOff() ? TEXT("switched off") : nullptr;
	if (Why)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT grabup: %s held: %s, the frame as it is"), *OutlawsName, Why);
		return;
	}
	const OutlawsTextures::FSpriteAtlas& At = *AtlasRef;
	TArray<int32> Cells;
	OutlawsGrabUpscale::HeldCells(At, Def.HitChor, Cells);
	TArray<TPair<int32, FString>> Files;
	for (int32 c : Cells)
	{
		const FString F = OutlawsGrabUpscale::Find(A->GetRoot(), OutlawsGrabUpscale::FileName(At.Nwx, At.CellFrame[c]));
		if (!F.IsEmpty()) Files.Emplace(c, F);
	}
	if (Files.Num() == 0)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT grabup: %s held: no upscaled frames for %s (%d held cells%s), the HD frame"), *OutlawsName, *At.Nwx, Cells.Num(),
			OutlawsGrabUpscale::PretendMissing() ? TEXT(", -OLGrabUpscaleMissing") : TEXT(""));
		return;
	}
	IImageWrapperModule* IW = OutlawsGrabUpscale::ImageModule();
	if (!IW) return;
	const FIntPoint Want = OutlawsGrabUpscale::UpscaledSize(At.CellW, At.CellH);
	const TWeakObjectPtr<AOutlawsEnemy> Weak(this);
	const uint32 Gen = GrabUpGen;
	GrabUpAskedAt = FPlatformTime::Seconds();
	// (the PNG decodes and the mips on a worker; the game thread only makes the textures: no hitch on the grab)
	Async(EAsyncExecution::ThreadPool, [Weak, Gen, Files, Want, IW]()
	{
		struct FDone { int32 Cell; FString File; OutlawsTextures::FPreparedTex Tex; };
		TSharedRef<TArray<FDone>> Ready = MakeShared<TArray<FDone>>();
		TArray<FString> Bad;
		for (const TPair<int32, FString>& F : Files)
		{
			TArray<FColor> Px;
			int32 W = 0, H = 0;
			if (!OutlawsGrabUpscale::LoadPng(*IW, F.Value, Px, W, H) || W != Want.X || H != Want.Y)
			{
				Bad.Add(FString::Printf(TEXT("%s (%dx%d, wanted %dx%d)"), *FPaths::GetCleanFilename(F.Value), W, H, Want.X, Want.Y));
				continue;
			}
			FDone& D = Ready->AddDefaulted_GetRef();
			D.Cell = F.Key;
			D.File = F.Value;
			OutlawsTextures::PrepareBGRA(W, H, Px, true, TF_Bilinear, TA_Clamp, D.Tex);
		}
		AsyncTask(ENamedThreads::GameThread, [Weak, Gen, Ready, Bad]()
		{
			if (AOutlawsEnemy* E = Weak.Get())
			{
				for (const FString& B : Bad) UE_LOG(LogOutlaws, Log, TEXT("OLRT grabup: %s: %s not used (unreadable or the wrong size), the HD frame there"), *E->OutlawsName, *B);
			}
			// one texture a frame, the front view first (the game thread's share of a grab kept to one texture's upload)
			struct FTally { int32 Next = 0, On = 0; double Worst = 0.0, Sum = 0.0; FString Names; };
			TSharedRef<FTally> Tally = MakeShared<FTally>();
			FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak, Gen, Ready, Tally](float) -> bool
			{
				AOutlawsEnemy* E = Weak.Get();
				if (!E || E->GrabUpGen != Gen || !E->bGrabUpOn || !E->GetSprite() || Tally->Next >= Ready->Num()) return false;     // (let go meanwhile)
				auto& D = (*Ready)[Tally->Next++];
				const int32 W = D.Tex.W, H = D.Tex.H;
				const double T0 = FPlatformTime::Seconds();
				if (UTexture2D* T = OutlawsTextures::CreatePrepared(D.Tex))
				{
					E->GetSprite()->SetHeldLook(D.Cell, T);
					++Tally->On;
					Tally->Names += FString::Printf(TEXT("%s%s cell %d %dx%d"), Tally->Names.IsEmpty() ? TEXT("") : TEXT(", "), *FPaths::GetCleanFilename(D.File), D.Cell, W, H);
				}
				const double Ms = (FPlatformTime::Seconds() - T0) * 1000.0;
				Tally->Worst = FMath::Max(Tally->Worst, Ms);
				Tally->Sum += Ms;
				if (Tally->Next < Ready->Num()) return true;
				UE_LOG(LogOutlaws, Log, TEXT("OLRT grabup: %s held: %d upscaled frames on (%s); %.0f ms after the grab, %.2f ms on the game thread (%.2f ms at most in one frame)"),
					*E->OutlawsName, Tally->On, *Tally->Names, (FPlatformTime::Seconds() - E->GrabUpAskedAt) * 1000.0, Tally->Sum, Tally->Worst);
				return false;
			}));
		});
	});
}
