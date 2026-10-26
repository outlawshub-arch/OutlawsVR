#pragma once

#include "CoreMinimal.h"

/**
 * Which build this is: the packaging scripts (tools/olue/package_pc.cmd, package_quest.cmd) write the commit, branch and
 * date into OutlawsBuildStamp.h (not in git) before they build; a plain editor build without one says "dev".
 */
namespace OutlawsBuildInfo
{
	OUTLAWSRUNTIME_API const TCHAR* Commit();
	OUTLAWSRUNTIME_API const TCHAR* Branch();
	OUTLAWSRUNTIME_API const TCHAR* Date();
	/** One line for the log and the main menu: "build 99b6bd0 (coop, 2026-10-02)". */
	OUTLAWSRUNTIME_API FString Line();
}
