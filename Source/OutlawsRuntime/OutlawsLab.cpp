#include "OutlawsLab.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/MessageDialog.h"
#include "Misc/Parse.h"
#include "Misc/ConfigCacheIni.h"
#include "OutlawsLevelActor.h"         // the settings file (InstallPath=)
#include "OutlawsInstallWindows.h"

FOutlawsLabFile::FOutlawsLabFile() = default;
FOutlawsLabFile::~FOutlawsLabFile() = default;

bool FOutlawsLabFile::Open(const FString& InPath)
{
	Path = InPath;
	Entries.Reset();
	Index.Reset();
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	Handle.Reset(PF.OpenRead(*InPath, /*bAllowWrite*/ false));
	if (!Handle)
	{
		UE_LOG(LogOutlaws, Warning, TEXT("%s: cannot open"), *InPath);
		return false;
	}
	uint8 Header[16];
	if (!Handle->Read(Header, 16) || Header[0] != 'L' || Header[1] != 'A' || Header[2] != 'B' || Header[3] != 'N')
	{
		UE_LOG(LogOutlaws, Warning, TEXT("%s: not a LAB"), *InPath);
		Handle.Reset();
		return false;
	}
	auto U32 = [](const uint8* P) { uint32 V; FMemory::Memcpy(&V, P, 4); return V; };
	const uint32 NFiles = U32(Header + 8);
	const uint32 PoolSize = U32(Header + 12);
	if (NFiles > 1000000 || PoolSize > 64u * 1024u * 1024u)
	{
		UE_LOG(LogOutlaws, Warning, TEXT("%s: implausible directory (%u files)"), *InPath, NFiles);
		Handle.Reset();
		return false;
	}
	TArray<uint8> Dir;
	Dir.SetNumUninitialized(NFiles * 16);
	TArray<uint8> Pool;
	Pool.SetNumUninitialized(PoolSize);
	if (!Handle->Read(Dir.GetData(), Dir.Num()) || !Handle->Read(Pool.GetData(), Pool.Num()))
	{
		UE_LOG(LogOutlaws, Warning, TEXT("%s: truncated directory"), *InPath);
		Handle.Reset();
		return false;
	}
	Entries.Reserve(NFiles);
	for (uint32 i = 0; i < NFiles; ++i)
	{
		const uint8* E = Dir.GetData() + i * 16;
		const uint32 NameOff = U32(E);
		FEntry Entry;
		Entry.Offset = U32(E + 4);
		Entry.Size = U32(E + 8);
		if (NameOff >= PoolSize)
		{
			continue;
		}
		int32 End = int32(NameOff);
		while (End < Pool.Num() && Pool[End] != 0) { ++End; }
		Entry.Name = FString(End - int32(NameOff), reinterpret_cast<const ANSICHAR*>(Pool.GetData() + NameOff));
		Index.Add(Entry.Name.ToLower(), Entries.Num());
		Entries.Add(MoveTemp(Entry));
	}
	return true;
}

bool FOutlawsLabFile::Read(int32 EntryIndex, TArray<uint8>& Out) const
{
	if (!Entries.IsValidIndex(EntryIndex) || !Handle)
	{
		return false;
	}
	const FEntry& E = Entries[EntryIndex];
	FScopeLock Lock(&ReadLock);
	if (!Handle->Seek(E.Offset))
	{
		return false;
	}
	Out.SetNumUninitialized(E.Size);
	return E.Size == 0 || Handle->Read(Out.GetData(), E.Size);
}

// Later patches and the remaster archives override the originals (olue_export.py LAB_SEARCH).
const TCHAR* FOutlawsAssets::LabSearchOrder[] = {
	TEXT("olpatch5.lab"), TEXT("olpatch4.lab"), TEXT("olpatch3.lab"), TEXT("olpatch2.lab"), TEXT("olpatch1.lab"),
	TEXT("olremaster2.lab"), TEXT("olremaster1.lab"), TEXT("olobj.lab"), TEXT("oltex.lab"), TEXT("olweap.lab"),
	TEXT("olgeo.lab"), TEXT("outlaws.lab"), TEXT("olsfxhd.lab"), TEXT("olsfx.lab"),
	TEXT("oltaunthd.lab"), TEXT("oltaunt.lab"), nullptr   // bad-guy taunts (c105.wav...) live only here
};

int32 FOutlawsAssets::Open(const FString& InRoot)
{
	Root = InRoot;
	Labs.Empty();
	Index.Empty();
	for (int32 i = 0; LabSearchOrder[i]; ++i)
	{
		const FString Candidates[2] = { FPaths::Combine(Root, LabSearchOrder[i]), FPaths::Combine(Root, TEXT("assets"), LabSearchOrder[i]) };
		for (const FString& P : Candidates)
		{
			if (!FPaths::FileExists(P))
			{
				continue;
			}
			TSharedPtr<FOutlawsLabFile> Lab = MakeShared<FOutlawsLabFile>();
			if (!Lab->Open(P))
			{
				break;
			}
			const int32 LabIdx = Labs.Add(Lab);
			for (const TPair<FString, int32>& Kv : Lab->GetIndex())
			{
				if (!Index.Contains(Kv.Key))
				{
					Index.Add(Kv.Key, TPair<int32, int32>(LabIdx, Kv.Value));
				}
			}
			break;
		}
	}
	UE_LOG(LogOutlaws, Log, TEXT("Opened %d LAB archives under %s (%d names)"), Labs.Num(), *Root, Index.Num());
	return Labs.Num();
}

bool FOutlawsAssets::GetSound(const FString& Name, bool bHD, TArray<uint8>& Out) const
{
	if (bHD)
	{
		// the Remastered sets first: olsfxhd keeps the names, oltaunthd holds the taunts as .ogg
		const FString Key = Name.ToLower();
		const FString Ogg = FPaths::GetBaseFilename(Key) + TEXT(".ogg");
		for (const TSharedPtr<FOutlawsLabFile>& Lab : Labs)
		{
			const FString Base = FPaths::GetCleanFilename(Lab->GetPath()).ToLower();
			if (Base != TEXT("olsfxhd.lab") && Base != TEXT("oltaunthd.lab")) continue;
			const int32* E = Lab->GetIndex().Find(Key);
			if (!E) E = Lab->GetIndex().Find(Ogg);
			if (E) return Lab->Read(*E, Out);
		}
	}
	if (!bHD)
	{
		// the first archive holding it that is not one of the HD sound sets
		const FString Key = Name.ToLower();
		for (const TSharedPtr<FOutlawsLabFile>& Lab : Labs)
		{
			const FString Base = FPaths::GetCleanFilename(Lab->GetPath()).ToLower();
			if (Base == TEXT("olsfxhd.lab") || Base == TEXT("oltaunthd.lab")) continue;
			if (const int32* E = Lab->GetIndex().Find(Key))
			{
				return Lab->Read(*E, Out);
			}
		}
	}
	return Get(Name, Out);
}

bool FOutlawsAssets::Has(const FString& Name) const
{
	return Index.Contains(Name.ToLower());
}

bool FOutlawsAssets::Get(const FString& Name, TArray<uint8>& Out) const
{
	const TPair<int32, int32>* Hit = Index.Find(Name.ToLower());
	if (!Hit)
	{
		return false;
	}
	return Labs[Hit->Key]->Read(Hit->Value, Out);
}

bool FOutlawsAssets::GetText(const FString& Name, FString& Out) const
{
	TArray<uint8> Bytes;
	if (!Get(Name, Bytes))
	{
		return false;
	}
	// latin-1: every byte maps to the same code point
	TArray<TCHAR>& Chars = Out.GetCharArray();
	Chars.SetNumUninitialized(Bytes.Num() + 1);
	for (int32 i = 0; i < Bytes.Num(); ++i)
	{
		Chars[i] = TCHAR(Bytes[i]);
	}
	Chars[Bytes.Num()] = 0;
	return true;
}

FString FOutlawsAssets::Where(const FString& Name) const
{
	const TPair<int32, int32>* Hit = Index.Find(Name.ToLower());
	return Hit ? FPaths::GetCleanFilename(Labs[Hit->Key]->GetPath()) : FString();
}

void FOutlawsAssets::ListByExtension(const FString& LowerExt, TArray<FString>& Out) const
{
	for (const TPair<FString, TPair<int32, int32>>& Kv : Index)
	{
		if (Kv.Key.EndsWith(LowerExt))
		{
			Out.Add(Kv.Key);
		}
	}
	Out.Sort();
}

bool FOutlawsAssets::LooksLikeInstall(const FString& Dir)
{
	// a real install carries the level, object and texture archives together (a folder with
	// stray copies of one or two LABs, like the LawMaker repo, must not qualify)
	if (Dir.IsEmpty())
	{
		return false;
	}
	for (const TCHAR* Sub : { TEXT(""), TEXT("assets") })
	{
		const FString Base = FPaths::Combine(Dir, Sub);
		if (FPaths::FileExists(FPaths::Combine(Base, TEXT("olgeo.lab"))) &&
			FPaths::FileExists(FPaths::Combine(Base, TEXT("olobj.lab"))) &&
			FPaths::FileExists(FPaths::Combine(Base, TEXT("oltex.lab"))))
		{
			return true;
		}
	}
	return false;
}

// found once per configured path, then remembered (several places ask; the store lookups read the registry and files)
static FCriticalSection OlInstallRootLock;
static TMap<FString, FString> OlInstallRootFound;
#if PLATFORM_WINDOWS
static bool bOlInstallAsked = false;          // (the folder picker: once a run)
#endif

FString FOutlawsAssets::FindInstallRoot(const FString& ConfiguredPath)
{
	FScopeLock Lock(&OlInstallRootLock);
	if (const FString* Hit = OlInstallRootFound.Find(ConfiguredPath))
	{
		return *Hit;
	}
	TArray<TPair<FString, const TCHAR*>> Candidates;   // (folder, where it came from: for the log)
	auto Add = [&Candidates](const FString& Dir, const TCHAR* From)
	{
		if (!Dir.IsEmpty()) Candidates.Emplace(Dir, From);
	};
	// asked for: the command line (-OLInstall="D:\Games\Outlaws Remastered"), the folder the player picked once (the
	// settings file), the level actor's own property
	FString Arg;
	if (OLFlags::Value(TEXT("OLInstall"), Arg, /*bShouldStopOnSeparator*/ false))
	{
		Add(FPaths::ConvertRelativePathToFull(Arg.TrimQuotes()), TEXT("-OLInstall"));
	}
	const TMap<FString, FString> Settings = AOutlawsLevelActor::ReadSettings();
	if (const FString* Saved = Settings.Find(TEXT("InstallPath")))
	{
		Add(*Saved, TEXT("InstallPath in OutlawsSettings.ini"));
	}
	Add(ConfiguredPath, TEXT("the configured InstallPath"));
	// a tester package: the data ships in a folder named Outlaws beside the game (wins over an installed copy)
	Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::RootDir(), TEXT("Outlaws"))), TEXT("the Outlaws folder beside the game"));
	Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::LaunchDir(), TEXT("Outlaws"))), TEXT("the Outlaws folder beside the game"));
	// the drop-in layout: the packaged build sits in (or one folder inside) Outlaws Remastered
	Add(FPaths::ConvertRelativePathToFull(FPaths::RootDir()), TEXT("beside the game"));
	Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::RootDir(), TEXT(".."))), TEXT("beside the game"));
	Add(FPaths::ConvertRelativePathToFull(FPaths::LaunchDir()), TEXT("beside the game"));
	Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::LaunchDir(), TEXT(".."))), TEXT("beside the game"));
	Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT(".."))), TEXT("beside the game"));
	// a headset (Android): the game's own files folder (adb push the LABs, Common.kpf and UI.kpf into
	// <files>/Outlaws), or a folder on the shared storage
	Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectPersistentDownloadDir(), TEXT("Outlaws"))), TEXT("the game's files folder"));
	Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("Outlaws"))), TEXT("the game's files folder"));
	Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectSavedDir(), TEXT(".."), TEXT("Outlaws"))), TEXT("the game's files folder"));
	Add(TEXT("/sdcard/OutlawsVR"), TEXT("shared storage"));
	Add(TEXT("/storage/emulated/0/OutlawsVR"), TEXT("shared storage"));
	// a PC: where Steam and GOG installed it, then the usual Steam folders; GOG's classic Outlaws (no remaster
	// archives, no menu art) only when nothing else is there
	TArray<FString> GogClassic;
#if PLATFORM_WINDOWS
	{
		TArray<FString> Steam, Gog;
		OutlawsInstallWin::SteamFolders(Steam);
		OutlawsInstallWin::GogFolders(Gog, GogClassic);
		for (const FString& D : Steam) Add(D, TEXT("Steam"));
		for (const FString& D : Gog) Add(D, TEXT("GOG"));
	}
#endif
	Add(TEXT("C:/SteamLibrary/steamapps/common/Outlaws Remastered"), TEXT("a usual Steam folder"));
	Add(TEXT("C:/Program Files (x86)/Steam/steamapps/common/Outlaws Remastered"), TEXT("a usual Steam folder"));
	Add(TEXT("D:/SteamLibrary/steamapps/common/Outlaws Remastered"), TEXT("a usual Steam folder"));
	for (const FString& D : GogClassic) Add(D, TEXT("GOG (the classic Outlaws)"));
	TArray<FString> Looked;
	for (const TPair<FString, const TCHAR*>& C : Candidates)
	{
		if (LooksLikeInstall(C.Key))
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLRT Outlaws install: %s (from %s)%s%s"), *C.Key, C.Value,
				Looked.Num() > 0 ? TEXT("; not an install: ") : TEXT(""), *FString::Join(Looked, TEXT(" | ")));
			OlInstallRootFound.Add(ConfiguredPath, C.Key);
			return C.Key;
		}
		Looked.AddUnique(C.Key);
	}
	UE_LOG(LogOutlaws, Warning, TEXT("OLRT no Outlaws install found; looked in: %s"), *FString::Join(Looked, TEXT(" | ")));
	return FString();
}

bool FOutlawsAssets::CanAskForInstall()
{
#if PLATFORM_WINDOWS
	return !bOlInstallAsked && !FApp::IsUnattended() && !GIsEditor && !IsRunningCommandlet() && FApp::CanEverRender();
#else
	return false;
#endif
}

FString FOutlawsAssets::AskForInstallRoot()
{
#if PLATFORM_WINDOWS
	bOlInstallAsked = true;
	const FText Title = FText::FromString(TEXT("Outlaws VR"));
	const FText Intro = FText::FromString(TEXT("Outlaws VR plays from your own copy of Outlaws (the Remastered edition from Steam or GOG), ")
		TEXT("and it could not find one on this PC.\n\nIn the next window, choose the folder Outlaws is installed in: the one ")
		TEXT("holding olgeo.lab, Common.kpf and UI.kpf.\n\nThe game remembers it (InstallPath in OutlawsSettings.ini)."));
	if (FMessageDialog::Open(EAppMsgType::OkCancel, Intro, &Title) != EAppReturnType::Ok)
	{
		UE_LOG(LogOutlaws, Warning, TEXT("OLRT install folder: the player declined to choose one"));
		return FString();
	}
	for (;;)
	{
		const FString Dir = OutlawsInstallWin::PickFolder(TEXT("Choose the Outlaws Remastered folder"));
		if (Dir.IsEmpty())
		{
			UE_LOG(LogOutlaws, Warning, TEXT("OLRT install folder: the picker was cancelled"));
			return FString();
		}
		if (LooksLikeInstall(Dir))
		{
			TMap<FString, FString> Settings = AOutlawsLevelActor::ReadSettings();
			Settings.Add(TEXT("InstallPath"), Dir);
			AOutlawsLevelActor::WriteSettings(Settings);
			{
				FScopeLock Lock(&OlInstallRootLock);
				OlInstallRootFound.Reset();      // (found again through the settings file, for every configured path)
			}
			UE_LOG(LogOutlaws, Log, TEXT("OLRT install folder chosen: %s (kept in OutlawsSettings.ini)"), *Dir);
			return Dir;
		}
		const FText Again = FText::FromString(FString::Printf(TEXT("%s does not hold Outlaws: olgeo.lab, olobj.lab and oltex.lab ")
			TEXT("are not in it.\n\nChoose another folder?"), *Dir));
		if (FMessageDialog::Open(EAppMsgType::YesNo, Again, &Title) != EAppReturnType::Yes)
		{
			return FString();
		}
	}
#else
	return FString();
#endif
}

// the optional local item folder (private placeholder art, never in the repo): -OLItemDir=<dir>, else the
// OUTLAWSVR_ITEMDIR environment variable, else ItemDir under [Outlaws] in the per-user Config/UserGame.ini; a headset
// keeps its copy under the game's files folder
static FString GDevItemDir;

static const FString& ResolveItemDirs()
{
	static const FString Dir = []()
	{
		FString Dev;
		if (!OLFlags::Value(TEXT("OLItemDir"), Dev))
		{
			Dev = FPlatformMisc::GetEnvironmentVariable(TEXT("OUTLAWSVR_ITEMDIR"));
		}
		if (Dev.IsEmpty() && GConfig)
		{
			GConfig->GetString(TEXT("Outlaws"), TEXT("ItemDir"), Dev, GGameIni);
		}
		Dev.ReplaceInline(TEXT("\\"), TEXT("/"));
		while (Dev.EndsWith(TEXT("/")))
		{
			Dev.LeftChopInline(1);
		}
		if (!Dev.IsEmpty() && FPaths::DirectoryExists(Dev))
		{
			GDevItemDir = Dev;
			return Dev;
		}
		const FString Headset = FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectPersistentDownloadDir(), TEXT("Placeholders")));
		if (!FPaths::DirectoryExists(Headset))
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLRT no local item folder (%s) or %s"), Dev.IsEmpty() ? TEXT("none set") : *Dev, *Headset);
		}
		return Headset;
	}();
	return Dir;
}

const FString& FOutlawsAssets::PlaceholderDir()
{
	return ResolveItemDirs();
}

bool FOutlawsAssets::HasDevItemDir()
{
	ResolveItemDirs();
	return !GDevItemDir.IsEmpty();
}

namespace
{
	TMap<FString, TSharedPtr<FOutlawsAssets>> GSharedAssets;     // the LAB index of each install root, open for the game's life
}

int32 FOutlawsAssets::SharedCount()
{
	return GSharedAssets.Num();
}

TSharedPtr<FOutlawsAssets> FOutlawsAssets::GetShared(const FString& InRoot)
{
	TMap<FString, TSharedPtr<FOutlawsAssets>>& Cache = GSharedAssets;
	if (TSharedPtr<FOutlawsAssets>* Found = Cache.Find(InRoot))
	{
		return *Found;
	}
	TSharedPtr<FOutlawsAssets> A = MakeShared<FOutlawsAssets>();
	if (A->Open(InRoot) == 0)
	{
		return nullptr;
	}
	Cache.Add(InRoot, A);
	return A;
}
