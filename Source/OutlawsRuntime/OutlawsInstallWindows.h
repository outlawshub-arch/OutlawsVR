#pragma once

#include "CoreMinimal.h"

#if PLATFORM_WINDOWS
/** The Windows side of finding the player's Outlaws (FOutlawsAssets::FindInstallRoot, AskForInstallRoot). */
namespace OutlawsInstallWin
{
	/** Outlaws Remastered in each Steam library (libraryfolders.vdf), the libraries holding its app manifest first. */
	void SteamFolders(TArray<FString>& Out);
	/** GOG's installs from the registry: the remaster (id 1536913668, then any other Outlaws remaster) in OutRemaster,
	 *  the classic Outlaws in OutClassic. */
	void GogFolders(TArray<FString>& OutRemaster, TArray<FString>& OutClassic);
	/** The system's folder picker; empty when cancelled. Blocks until it closes. */
	FString PickFolder(const FString& Title);
}
#endif
