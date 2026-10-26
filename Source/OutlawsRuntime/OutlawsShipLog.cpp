// REQ-SHIPLOG (see OutlawsShipLog.h): the Shipping build's own log file, each line handed to the system as it comes (no
// buffer of our own and no flush to disk, so a crash of the game keeps everything before it).  The file of the run
// before is kept beside it as OutlawsUE-backup-<its date>.log, as the engine does.
#include "OutlawsShipLog.h"
#include "OutlawsPerf.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformOutputDevices.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"

FString OLShipLog::ReportSavedDir()
{
#if PLATFORM_ANDROID && UE_BUILD_SHIPPING
	// (Development's Saved: GFilePathBase is the external files folder there, the internal one in Shipping; see
	// CarryOverDevelopmentSaved in OutlawsRuntime.cpp)
	return FPaths::ProjectPersistentDownloadDir() / TEXT("UnrealGame") / FApp::GetProjectName() / FApp::GetProjectName() / TEXT("Saved");
#else
	return FPaths::ProjectSavedDir();
#endif
}

#if NO_LOGGING
namespace
{
	FCriticalSection GShipLogLock;
	IFileHandle* GShipLogFile = nullptr;
	bool GShipLogTried = false;
	FString GShipLogName;

	const FString& ShipLogName()
	{
		if (GShipLogName.IsEmpty())
		{
			GShipLogName = FPaths::ConvertRelativePathToFull(OLShipLog::ReportSavedDir() / TEXT("Logs") / FString(FApp::GetProjectName()) + TEXT(".log"));
		}
		return GShipLogName;
	}

	void ShipLogWriteRaw(const FString& S)
	{
		const FTCHARToUTF8 U(*S);
		GShipLogFile->Write(reinterpret_cast<const uint8*>(U.Get()), U.Length());
	}

	bool OpenShipLog()
	{
		if (GShipLogTried) return GShipLogFile != nullptr;
		GShipLogTried = true;
		IFileManager& FM = IFileManager::Get();
		const FString& Name = ShipLogName();
		FM.MakeDirectory(*FPaths::GetPath(Name), true);
		if (FM.FileExists(*Name))
		{
			const FDateTime Was = FM.GetTimeStamp(*Name);
			const FString Backup = FPaths::GetPath(Name) / FString::Printf(TEXT("%s-backup-%s.log"), FApp::GetProjectName(), *Was.ToString());
			FM.Move(*Backup, *Name, /*Replace*/ true, /*EvenIfReadOnly*/ true);
		}
		GShipLogFile = FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*Name, /*bAppend*/ false, /*bAllowRead*/ true);
		if (!GShipLogFile) return false;
		static const uint8 Bom[] = { 0xEF, 0xBB, 0xBF };
		GShipLogFile->Write(Bom, sizeof(Bom));
		ShipLogWriteRaw(FString::Printf(TEXT("Log file open, %s") LINE_TERMINATOR, *FDateTime::Now().ToString()));
		ShipLogWriteRaw(TEXT("(Shipping: the game's own Log, Display, Warning and Error lines; the engine's lines are not in a Shipping build)") LINE_TERMINATOR);
		return true;
	}
}

FString OLShipLog::FileName()
{
	return ShipLogName();
}

void OLShipLog::Write(const TCHAR* Category, ELogVerbosity::Type Verbosity, const TCHAR* Text)
{
	// the engine's line: [UTC date-time:ms][frame % 1000]Category: Verbosity: text (no verbosity word for Log)
	const FDateTime Now = FDateTime::UtcNow();
	const TCHAR* V = Verbosity == ELogVerbosity::Fatal ? TEXT("Fatal: ") : Verbosity == ELogVerbosity::Error ? TEXT("Error: ")
		: Verbosity == ELogVerbosity::Warning ? TEXT("Warning: ") : Verbosity == ELogVerbosity::Display ? TEXT("Display: ") : TEXT("");
	const FString Line = FString::Printf(TEXT("[%s:%03d][%3d]%s: %s%s") LINE_TERMINATOR, *Now.ToString(TEXT("%Y.%m.%d-%H.%M.%S")), Now.GetMillisecond(),
		int32(GFrameCounter % 1000), Category, V, Text);
	OutlawsPerf::Breadcrumb(FString::Printf(TEXT("%s: %s"), Category, Text));      // (PERF-SHIPSUMMARY: what a hitch's frame did, hitches.csv)
	FScopeLock Lock(&GShipLogLock);
	if (!OpenShipLog()) return;
	ShipLogWriteRaw(Line);
}
#else
FString OLShipLog::FileName()
{
	return FGenericPlatformOutputDevices::GetAbsoluteLogFilename();
}
#endif
