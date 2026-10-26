#pragma once

#include "CoreMinimal.h"

/**
 * REQ-SHIPLOG: the Shipping build's log file.  The launcher engine's Shipping libraries are built with NO_LOGGING (Core's
 * FMsg::Logf and FOutputDeviceFile do nothing there), and a target cannot turn bUseLoggingInShipping on without a source
 * engine (a unique build environment is refused with an installed engine).  So in a build without logging the game's
 * own UE_LOG lines (LogOutlaws, LogOutlawsCoop, LogOutlawsAI and any other UE_LOG in a file that includes this) are
 * written by OutlawsShipLog.cpp instead: Log, Display, Warning and Error (the categories' own default; Verbose and
 * VeryVerbose are left out), in the engine's line format, to the file a Development build writes:
 *   PC:    <ProjectSavedDir>/Logs/OutlawsUE.log (%LOCALAPPDATA%\OutlawsUE\Saved\Logs, or the -userdir sandbox's Saved\Logs)
 *   Quest: <external files>/UnrealGame/OutlawsUE/OutlawsUE/Saved/Logs/OutlawsUE.log (Development's Saved; Shipping's own
 *          Saved is in internal storage)
 * The engine's own lines are compiled out of its Shipping libraries and are not in it.  A Development build is unchanged.
 */
namespace OLShipLog
{
	/** The log file the game writes (the engine's in a build with logging). */
	OUTLAWSRUNTIME_API FString FileName();
	/** The Saved folder a Development build uses, for what players send in (the logs, the bug snapshots). */
	OUTLAWSRUNTIME_API FString ReportSavedDir();
#if NO_LOGGING
	OUTLAWSRUNTIME_API void Write(const TCHAR* Category, ELogVerbosity::Type Verbosity, const TCHAR* Text);
#endif
}

#if NO_LOGGING
	// the lines kept: Log and above (Fatal 1, Error 2, Warning 3, Display 4, Log 5); Display alone would be <= Display
	#define OL_SHIP_LOG_MAX ELogVerbosity::Log
	#undef UE_LOG
	#define UE_LOG(CategoryName, Verbosity, Format, ...) \
	{ \
		if constexpr (ELogVerbosity::Verbosity <= OL_SHIP_LOG_MAX) \
		{ \
			OLShipLog::Write(TEXT(#CategoryName), ELogVerbosity::Verbosity, *FString::Printf(Format, ##__VA_ARGS__)); \
		} \
		if (ELogVerbosity::Verbosity == ELogVerbosity::Fatal) \
		{ \
			LowLevelFatalError(Format, ##__VA_ARGS__); \
		} \
	}
#endif
