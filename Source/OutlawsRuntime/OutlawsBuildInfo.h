#pragma once

#include "CoreMinimal.h"

/**
 * Which build this is: the packaging scripts (tools/olue/package_pc.cmd, package_quest.cmd) write the commit, branch and
 * date into OutlawsBuildStamp.h (not in git) before they build; a plain editor build without one says "dev".
 */
namespace OutlawsBuildInfo
{
	/** The version the main menu shows ("0.95") and the working name of the builds after it ("hotfix 2", or empty). */
	OUTLAWSRUNTIME_API const TCHAR* Version();
	OUTLAWSRUNTIME_API const TCHAR* Label();
	OUTLAWSRUNTIME_API const TCHAR* Commit();
	OUTLAWSRUNTIME_API const TCHAR* Branch();
	OUTLAWSRUNTIME_API const TCHAR* Date();
	/** One line for the log: "build 99b6bd0 (coop, 2026-10-02)". */
	OUTLAWSRUNTIME_API FString Line();
	/** The main menu's line under its help: "v0.95 hotfix 2  build 99b6bd0" (Line() stays without the version and label, for
	 *  the co-op build compare, the beacon and the logs: the commit already tells two builds apart). */
	OUTLAWSRUNTIME_API FString MenuLine();
}
