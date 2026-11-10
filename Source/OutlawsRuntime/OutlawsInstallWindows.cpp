// The Windows side of finding the player's Outlaws: Steam's libraries, GOG's installs from the registry, the Epic Games
// Launcher's install manifests, and the
// folder picker for when neither has it (a packaged build away from the install; tools/olue/TESTER_GUIDE.md).
#include "OutlawsInstallWindows.h"

#if PLATFORM_WINDOWS
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformMisc.h"
#include "HAL/FileManager.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "OutlawsFlags.h"
#include "OutlawsLab.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include <shobjidl.h>

namespace OutlawsInstallWinPrivate
{
	// the quoted value after each "Key" in a Valve key/value file (libraryfolders.vdf, appmanifest_*.acf); paths in
	// them double their backslashes
	void VdfValues(const FString& Text, const TCHAR* Key, TArray<FString>& Out)
	{
		const FString Quoted = FString::Printf(TEXT("\"%s\""), Key);
		int32 From = 0;
		for (;;)
		{
			const int32 At = Text.Find(Quoted, ESearchCase::IgnoreCase, ESearchDir::FromStart, From);
			if (At == INDEX_NONE) return;
			const int32 Open = Text.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, At + Quoted.Len());
			const int32 Close = Open == INDEX_NONE ? INDEX_NONE : Text.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart, Open + 1);
			if (Close == INDEX_NONE) return;
			Out.Add(Text.Mid(Open + 1, Close - Open - 1).Replace(TEXT("\\\\"), TEXT("\\")));
			From = Close + 1;
		}
	}

	FString Clean(FString Dir)
	{
		FPaths::NormalizeDirectoryName(Dir);
		return Dir;
	}

	const TCHAR* const GogGames = TEXT("SOFTWARE\\WOW6432Node\\GOG.com\\Games");
	const TCHAR* const GogRemasterId = TEXT("1536913668");    // "Outlaws + Handful of Missions: Remaster"
}

void OutlawsInstallWin::SteamFolders(TArray<FString>& Out)
{
	using namespace OutlawsInstallWinPrivate;
	FString Steam;
	if (!FWindowsPlatformMisc::QueryRegKey(HKEY_CURRENT_USER, TEXT("Software\\Valve\\Steam"), TEXT("SteamPath"), Steam) || Steam.IsEmpty())
	{
		FWindowsPlatformMisc::QueryRegKey(HKEY_LOCAL_MACHINE, TEXT("SOFTWARE\\WOW6432Node\\Valve\\Steam"), TEXT("InstallPath"), Steam);
	}
	if (Steam.IsEmpty())
	{
		return;
	}
	TArray<FString> Libraries = { Steam };
	FString Vdf;
	if (FFileHelper::LoadFileToString(Vdf, *FPaths::Combine(Steam, TEXT("steamapps"), TEXT("libraryfolders.vdf"))))
	{
		VdfValues(Vdf, TEXT("path"), Libraries);
	}
	// Outlaws Remastered is Steam app 3366780; its manifest names the folder (installdir) and says which library has it
	TArray<FString> WithManifest, Without;
	for (const FString& Lib : Libraries)
	{
		FString Acf;
		if (FFileHelper::LoadFileToString(Acf, *FPaths::Combine(Lib, TEXT("steamapps"), TEXT("appmanifest_3366780.acf"))))
		{
			TArray<FString> Names;
			VdfValues(Acf, TEXT("installdir"), Names);
			WithManifest.AddUnique(Clean(FPaths::Combine(Lib, TEXT("steamapps"), TEXT("common"), Names.Num() > 0 && !Names[0].IsEmpty() ? Names[0] : FString(TEXT("Outlaws Remastered")))));
		}
		else
		{
			Without.AddUnique(Clean(FPaths::Combine(Lib, TEXT("steamapps"), TEXT("common"), TEXT("Outlaws Remastered"))));
		}
	}
	for (const FString& D : WithManifest) Out.AddUnique(D);
	for (const FString& D : Without) Out.AddUnique(D);
}

void OutlawsInstallWin::GogFolders(TArray<FString>& OutRemaster, TArray<FString>& OutClassic)
{
	using namespace OutlawsInstallWinPrivate;
	auto PathOf = [](const FString& Id)
	{
		FString P;
		FWindowsPlatformMisc::QueryRegKey(HKEY_LOCAL_MACHINE, *(FString(GogGames) + TEXT("\\") + Id), TEXT("path"), P);
		return P.IsEmpty() ? P : Clean(P);
	};
	const FString Remaster = PathOf(GogRemasterId);
	if (!Remaster.IsEmpty())
	{
		OutRemaster.Add(Remaster);
	}
	HKEY Games = nullptr;
	if (::RegOpenKeyExW(HKEY_LOCAL_MACHINE, GogGames, 0, KEY_READ, &Games) != ERROR_SUCCESS)
	{
		return;
	}
	for (DWORD i = 0; ; ++i)
	{
		WCHAR Name[256];
		DWORD Len = 256;
		if (::RegEnumKeyExW(Games, i, Name, &Len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS)
		{
			break;
		}
		const FString Id(Name);
		FString Game;
		if (Id == GogRemasterId || !FWindowsPlatformMisc::QueryRegKey(HKEY_LOCAL_MACHINE, *(FString(GogGames) + TEXT("\\") + Id), TEXT("gameName"), Game)
			|| !Game.Contains(TEXT("Outlaws")))
		{
			continue;
		}
		const FString P = PathOf(Id);
		if (!P.IsEmpty())
		{
			(Game.Contains(TEXT("Remaster")) ? OutRemaster : OutClassic).AddUnique(P);
		}
	}
	::RegCloseKey(Games);
}

FString OutlawsInstallWin::EpicManifestsDir()
{
#if WITH_OUTLAWS_TESTS
	FString Test;
	if (OLFlags::Value(TEXT("OLTestEpicManifests"), Test, /*bShouldStopOnSeparator*/ false))
	{
		return OutlawsInstallWinPrivate::Clean(FPaths::ConvertRelativePathToFull(Test.TrimQuotes()));
	}
#endif
	// the launcher keeps one <id>.item (JSON) per installed app in %ProgramData%\Epic\EpicGamesLauncher\Data\Manifests
	FString ProgramData = FPlatformMisc::GetEnvironmentVariable(TEXT("ProgramData"));
	if (ProgramData.IsEmpty())
	{
		ProgramData = TEXT("C:/ProgramData");
	}
	return OutlawsInstallWinPrivate::Clean(FPaths::Combine(ProgramData, TEXT("Epic"), TEXT("EpicGamesLauncher"), TEXT("Data"), TEXT("Manifests")));
}

void OutlawsInstallWin::EpicFolders(TArray<FString>& Out)
{
	using namespace OutlawsInstallWinPrivate;
	if (OLFlags::Param(TEXT("OLOldInstallFinder")))
	{
		return;
	}
	const FString Dir = EpicManifestsDir();
	TArray<FString> Items;
	IFileManager::Get().FindFiles(Items, *FPaths::Combine(Dir, TEXT("*.item")), /*Files*/ true, /*Directories*/ false);
	Items.Sort();
	// Outlaws is told by the folder (its LABs), not by an app name; a manifest naming Outlaws only goes first
	TArray<FString> Named, Other;
	for (const FString& Item : Items)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(Dir, Item)))
		{
			continue;
		}
		TSharedPtr<FJsonObject> Json;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) || !Json.IsValid())
		{
			continue;
		}
		FString Location, Name;
		bool bIncomplete = false;
		if (!Json->TryGetStringField(TEXT("InstallLocation"), Location) || Location.IsEmpty())
		{
			continue;
		}
		Json->TryGetStringField(TEXT("DisplayName"), Name);
		Json->TryGetBoolField(TEXT("bIsIncompleteInstall"), bIncomplete);
		const FString Install = Clean(Location);
		if (bIncomplete || !FOutlawsAssets::LooksLikeInstall(Install))
		{
			continue;
		}
		(Name.Contains(TEXT("Outlaws")) ? Named : Other).AddUnique(Install);
	}
	for (const FString& D : Named) Out.AddUnique(D);
	for (const FString& D : Other) Out.AddUnique(D);
}

FString OutlawsInstallWin::PickFolder(const FString& Title)
{
	FString Out;
	const HRESULT Com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	IFileOpenDialog* Dialog = nullptr;
	if (SUCCEEDED(::CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&Dialog))) && Dialog)
	{
		DWORD Options = 0;
		Dialog->GetOptions(&Options);
		Dialog->SetOptions(Options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
		Dialog->SetTitle(*Title);
		if (SUCCEEDED(Dialog->Show(::GetActiveWindow())))
		{
			IShellItem* Item = nullptr;
			if (SUCCEEDED(Dialog->GetResult(&Item)) && Item)
			{
				PWSTR Path = nullptr;
				if (SUCCEEDED(Item->GetDisplayName(SIGDN_FILESYSPATH, &Path)) && Path)
				{
					Out = OutlawsInstallWinPrivate::Clean(FString(Path));
					::CoTaskMemFree(Path);
				}
				Item->Release();
			}
		}
		Dialog->Release();
	}
	if (SUCCEEDED(Com))
	{
		::CoUninitialize();
	}
	return Out;
}
#include "Windows/HideWindowsPlatformTypes.h"
#endif
