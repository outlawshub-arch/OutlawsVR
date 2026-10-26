#include "OutlawsRuntime.h"
#include "OutlawsPerf.h"
#include "OutlawsBuildInfo.h"
#include "OutlawsDeviceProfile.h"
#include "OutlawsFlags.h"
#include "Engine/World.h"
#include "UObject/UObjectHash.h"
#include "OutlawsShipSmoke.h"
#if PLATFORM_ANDROID && UE_BUILD_SHIPPING
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#endif

DEFINE_LOG_CATEGORY(LogOutlaws);
DEFINE_LOG_CATEGORY(LogOutlawsCoop);
DEFINE_LOG_CATEGORY(LogOutlawsAI);

namespace
{
	FDelegateHandle GOlKeepPoolsHandle;

	// BUG-EXITCRASH: a world made after the game was asked to quit (a quit before the first frame, or one asked for in the
	// frame that then loads a map) gets a Niagara world manager whose FGCObject never registers (FGCObject::Init skips it
	// once exit is requested), so nothing holds that manager's component pool.  A collection on the way out (in the editor
	// the Python plugin's shutdown runs one) then frees the pool while the world lives on, and the world's FinishDestroy
	// in the exit purge cleans the freed pool: a crash whenever the memory had been reused.  Rooting such pools keeps them
	// to the exit purge, which destroys them with their world (every FinishDestroy runs before anything is freed there).
	void OlKeepNiagaraPoolsPastExit(UWorld* World, const UWorld::InitializationValues)
	{
		if (!IsEngineExitRequested()) return;
		static UClass* PoolClass = FindObject<UClass>(nullptr, TEXT("/Script/Niagara.NiagaraComponentPool"));
		if (!PoolClass) return;
		TArray<UObject*> Pools;
		GetObjectsOfClass(PoolClass, Pools, false);
		int32 Rooted = 0;
		for (UObject* P : Pools)
		{
			if (P && !P->IsRooted())
			{
				P->AddToRoot();
				++Rooted;
			}
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLRT %s made after the quit was asked for: %d Niagara component pools kept to the exit purge"), World ? *World->GetName() : TEXT("a world"), Rooted);
	}
}

#if PLATFORM_ANDROID && UE_BUILD_SHIPPING
namespace
{
	// (v1 Shipping) with bUseExternalFilesDir a Shipping build keeps Saved in the app's internal storage (AndroidJNI.cpp:
	// GFilePathBase = GInternalFilePath), where the Development builds up to v0.9 kept it under the external files folder.
	// Copy a v0.9 player's saves and settings across once, never over a file already there.  The LABs, placeholders
	// and the rest of the data stay where they were (ProjectPersistentDownloadDir is the external folder in both).
	void CarryOverDevelopmentSaved()
	{
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		const FString To = FPaths::ProjectSavedDir();
		const FString Marker = To / TEXT("carried_over.txt");
		if (PF.FileExists(*Marker)) return;
		const FString From = FPaths::ProjectPersistentDownloadDir() / TEXT("UnrealGame") / FApp::GetProjectName() / FApp::GetProjectName() / TEXT("Saved");
		FString Done = From + TEXT("\n");
		if (PF.DirectoryExists(*From))
		{
			PF.CreateDirectoryTree(*To);
			const FString Ini = TEXT("OutlawsSettings.ini");
			if (PF.FileExists(*(From / Ini)) && !PF.FileExists(*(To / Ini)) && PF.CopyFile(*(To / Ini), *(From / Ini))) Done += Ini + TEXT("\n");
			const FString Saves = TEXT("OutlawsSaves");
			if (PF.DirectoryExists(*(From / Saves)) && PF.CopyDirectoryTree(*(To / Saves), *(From / Saves), /*bOverwriteAllExisting*/ false)) Done += Saves + TEXT("\n");
		}
		PF.CreateDirectoryTree(*To);
		FFileHelper::SaveStringToFile(Done, *Marker);
	}
}
#endif

void FOutlawsRuntimeModule::StartupModule()
{
#if PLATFORM_ANDROID && UE_BUILD_SHIPPING
	CarryOverDevelopmentSaved();
#endif
	GOlKeepPoolsHandle = FWorldDelegates::OnPostWorldInitialization.AddStatic(&OlKeepNiagaraPoolsPastExit);
	UE_LOG(LogOutlaws, Log, TEXT("OutlawsRuntime module loaded, %s"), *OutlawsBuildInfo::Line());
	UE_LOG(LogOutlaws, Log, TEXT("OLFlags (%d):%s"), FOutlawsFlags::Get().Num(), *FOutlawsFlags::Get().Line());
	OutlawsPerf::Startup();
	OutlawsDeviceProfile::Startup();       // which headset: "OLRT headset: <model>, profile <Quest2|default>"
	OutlawsShipSmoke::Startup();           // (REQ-SHIPLOG: -OLShipSmoke only)
}

void FOutlawsRuntimeModule::ShutdownModule()
{
	FWorldDelegates::OnPostWorldInitialization.Remove(GOlKeepPoolsHandle);
	OutlawsDeviceProfile::Shutdown();
	OutlawsPerf::Shutdown();
}

IMPLEMENT_MODULE(FOutlawsRuntimeModule, OutlawsRuntime)
