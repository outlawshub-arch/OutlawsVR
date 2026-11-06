#pragma once

#include "CoreMinimal.h"

/**
 * Which build this is: the packaging scripts (tools/olue/package_pc.cmd, package_quest.cmd) write the commit, branch and
 * date into OutlawsBuildStamp.h (not in git) before they build; a plain editor build without one says "dev".
 */
namespace OutlawsBuildInfo
{
	/** REQ-NETVERSION (the user 6 Nov, choice A): the number two machines must share to play together, sent in the join
	 *  handshake and said on the beacon.  Same number joins, whatever the nightly; another number is refused before the lobby.
	 *  One line to bump, up by one, in any landing that changes the network surface (an RPC, a replicated property, a struct
	 *  those carry: tools/olue/land/netcheck.py lists them).  0.96 starts it at 1; a joiner with none on its URL counts as 0. */
	inline constexpr int32 NetVersion = 1;

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
