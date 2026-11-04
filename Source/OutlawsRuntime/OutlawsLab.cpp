#include "OutlawsLab.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "Misc/Paths.h"
#include "Misc/Crc.h"
#include "Misc/ScopeLock.h"
#include "HAL/ThreadSafeCounter.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/FileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/MessageDialog.h"
#include "Misc/Parse.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "OutlawsLevelActor.h"         // the settings file (InstallPath=)
#include "OutlawsInstallWindows.h"
#if PLATFORM_ANDROID
#include <dirent.h>                    // (BUG-QUESTNATIVEMAPS: the native maps listing's diagnosis)
#include <errno.h>
#include <string.h>
#endif

namespace
{
	FThreadSafeCounter GLiveLabFiles;          // (REQ-NATIVEMAPS: the overlay probe's leak check)
	FThreadSafeCounter GOverlaySerial;
}

FOutlawsLabFile::FOutlawsLabFile() { GLiveLabFiles.Increment(); }
FOutlawsLabFile::~FOutlawsLabFile() { GLiveLabFiles.Decrement(); }
int32 FOutlawsLabFile::NumLive() { return GLiveLabFiles.GetValue(); }

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
	Stamp = FCrc::MemCrc32(Pool.GetData(), Pool.Num(), FCrc::MemCrc32(Dir.GetData(), Dir.Num(), uint32(Handle->Size())));
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
	// (REQ-MAPQUICK, user 2 Nov: the 19 community maps playable now) every <native root>/<map>/*.lab after the stock order, at
	// the LOWEST priority: a stock name stays the stock file, a map's own names (its .lvt, .inf, .obt, .itm, new textures) are
	// found.  The quick step only: the per-map overlay of REQ-NATIVEMAPS replaces it (the original puts the level's LAB first,
	// so an overriding map shows the stock art here, and the first map mounted wins a name two maps share: ColdWar and
	// Frostbite share 20).  -OLNoNativeMaps: none.
	// (REQ-NATIVEMAPS, user 2 Nov) that mount is now -OLOldNativeMount only: by default each native LAB's directory is read
	// here for the levels it holds and the file is closed again; the level build opens its own map's LAB in front of the stock
	// order (UseLevelOverlay) and no other.
	NumStockLabs = Labs.Num();
	NativeRoot.Reset();
	NativeLabs.Reset();
	NativeStamps.Reset();
	NativeLevels.Reset();
	Overlay.Reset();
	OverlayOwner = nullptr;
	bOldNativeMount = OldNativeMount();
	if (NumStockLabs > 0)
	{
		const FString Natives = NativeMapsRoot();
		if (!Natives.IsEmpty())
		{
			NativeRoot = Natives;
			// (BUG-QUESTNATIVEMAPS) the maps as ListNativeMaps finds them (on the Quest the old FindFiles found none)
			for (const FNativeMap& Map : ListNativeMaps(Natives))
			{
				const FString& D = Map.Dir;
				for (const FString& F : Map.Labs)
				{
					TSharedPtr<FOutlawsLabFile> Lab = MakeShared<FOutlawsLabFile>();
					if (!Lab->Open(Natives / D / F)) continue;
					if (!bOldNativeMount)
					{
						// its levels (the first LAB found wins a level two maps hold); a stock level stays the install's
						const int32 At = NativeLabs.Add(Natives / D / F);
						NativeStamps.Add(Lab->GetStamp());
						for (const TPair<FString, int32>& Kv : Lab->GetIndex())
						{
							if (!Kv.Key.EndsWith(TEXT(".lvt"))) continue;
							const FString Level = Kv.Key.LeftChop(4);
							if (!Index.Contains(Kv.Key) && !NativeLevels.Contains(Level)) NativeLevels.Add(Level, At);
						}
						continue;                  // (closed: its handle goes with Lab)
					}
					const int32 LabIdx = Labs.Add(Lab);
					for (const TPair<FString, int32>& Kv : Lab->GetIndex())
					{
						if (!Index.Contains(Kv.Key))
						{
							Index.Add(Kv.Key, TPair<int32, int32>(LabIdx, Kv.Value));
						}
					}
				}
			}
			if (bOldNativeMount)
			{
				UE_LOG(LogOutlaws, Log, TEXT("OLRT native maps: %d LABs from %s (%d names in all; -OLOldNativeMount)"), Labs.Num() - NumStockLabs, *Natives, Index.Num());
			}
			else
			{
				UE_LOG(LogOutlaws, Log, TEXT("OLRT native maps: %d LABs in %s hold %d levels; none mounted (each opens over its own level)"), NativeLabs.Num(), *Natives, NativeLevels.Num());
			}
		}
	}
	return Labs.Num();
}

bool FOutlawsAssets::OldNativeMount()
{
	static const bool bOld = OLFlags::Param(TEXT("OLOldNativeMount"));
	return bOld;
}

uint32 FOutlawsAssets::OverlaySerial()
{
	return uint32(GOverlaySerial.GetValue());
}

FString FOutlawsAssets::NativeLabFor(const FString& Level) const
{
	const int32* At = NativeLevels.Find(Level.ToLower());
	return At ? NativeLabs[*At] : FString();
}

bool FOutlawsAssets::HasLevel(const FString& Level) const
{
	return Index.Contains(Level.ToLower() + TEXT(".lvt")) || NativeLevels.Contains(Level.ToLower());
}

FString FOutlawsAssets::NativeStampFor(const FString& Level) const
{
	if (Index.Contains(Level.ToLower() + TEXT(".lvt"))) return FString();      // (a stock level is the install's)
	const int32* At = NativeLevels.Find(Level.ToLower());
	return At && NativeStamps.IsValidIndex(*At) ? FString::Printf(TEXT("%08x"), NativeStamps[*At]) : FString();
}

FString FOutlawsAssets::NativeStampList() const
{
	TArray<FString> Levels;
	NativeLevels.GetKeys(Levels);
	Levels.Sort();
	TArray<FString> Out;
	for (const FString& Lv : Levels) Out.Add(Lv + TEXT(".") + NativeStampFor(Lv));
	return FString::Join(Out, TEXT("~"));
}

FString FOutlawsAssets::GetOverlay() const
{
	return Overlay ? Overlay->GetPath() : FString();
}

bool FOutlawsAssets::UseLevelOverlay(const FString& Level, const void* Owner)
{
	if (bOldNativeMount) return false;
	const FString Want = Index.Contains(Level.ToLower() + TEXT(".lvt")) ? FString() : NativeLabFor(Level);
	if (Want.IsEmpty() ? !Overlay : (Overlay && Overlay->GetPath() == Want))
	{
		OverlayOwner = Overlay ? Owner : nullptr;            // (the same map again: this build owns it now)
		return false;
	}
	const FString Was = GetOverlay();
	Overlay.Reset();                                         // (the old map's handle closed)
	OverlayOwner = nullptr;
	if (!Want.IsEmpty())
	{
		TSharedPtr<FOutlawsLabFile> Lab = MakeShared<FOutlawsLabFile>();
		if (Lab->Open(Want))
		{
			Overlay = Lab;
			OverlayOwner = Owner;
		}
	}
	GOverlaySerial.Increment();
	UE_LOG(LogOutlaws, Log, TEXT("OLRT native map overlay: %s (%d names) over the stock order for %s, was %s"), Overlay ? *FPaths::GetCleanFilename(Want) : TEXT("none"),
		Overlay ? Overlay->GetIndex().Num() : 0, *Level, Was.IsEmpty() ? TEXT("none") : *FPaths::GetCleanFilename(Was));
	return true;
}

bool FOutlawsAssets::CloseOverlay(const void* Owner)
{
	if (!Overlay || Owner != OverlayOwner) return false;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT native map overlay: %s closed (its level ended)"), *FPaths::GetCleanFilename(Overlay->GetPath()));
	Overlay.Reset();
	OverlayOwner = nullptr;
	GOverlaySerial.Increment();
	return true;
}

bool FOutlawsAssets::GetSound(const FString& Name, bool bHD, TArray<uint8>& Out) const
{
	// (REQ-NATIVEMAPS) the level's own LAB first, in either set: a map's own sound is the one its author meant (Bankrupt_RM
	// keeps its dog1/dog2/chaching.wav over the stock ones)
	if (Overlay)
	{
		if (const int32* E = Overlay->GetIndex().Find(Name.ToLower())) return Overlay->Read(*E, Out);
	}
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
	const FString Key = Name.ToLower();
	return (Overlay && Overlay->GetIndex().Contains(Key)) || Index.Contains(Key);
}

bool FOutlawsAssets::Get(const FString& Name, TArray<uint8>& Out) const
{
	// (REQ-NATIVEMAPS) the level's own LAB in front of the stock order
	if (Overlay)
	{
		if (const int32* E = Overlay->GetIndex().Find(Name.ToLower())) return Overlay->Read(*E, Out);
	}
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
	if (Overlay && Overlay->GetIndex().Contains(Name.ToLower())) return FPaths::GetCleanFilename(Overlay->GetPath());
	const TPair<int32, int32>* Hit = Index.Find(Name.ToLower());
	return Hit ? FPaths::GetCleanFilename(Labs[Hit->Key]->GetPath()) : FString();
}

void FOutlawsAssets::ListByExtension(const FString& LowerExt, TArray<FString>& Out) const
{
	for (const TPair<FString, TPair<int32, int32>>& Kv : Index)
	{
		// (REQ-MAPQUICK) the install's names only: the level pickers, the item tables and the whole-install scans stay as they
		// were with the native maps mounted (their files are found by name; ListMaps lists the maps from their RCMs)
		if (Kv.Key.EndsWith(LowerExt) && Kv.Value.Key < NumStockLabs)
		{
			Out.Add(Kv.Key);
		}
	}
	Out.Sort();
}

FString FOutlawsAssets::NativeMapsRoot()
{
	// (REQ-MAPQUICK) the MenuOverlay lookup's order (OutlawsMenuDoc.cpp FSource::Load): the command line, the settings file
	// (editor runs), the PC package's Tools folder, then the headset's files folder (Install on Quest pushes it there)
	if (OLFlags::Param(TEXT("OLNoNativeMaps"))) return FString();
	TArray<FString> Tries;
	FString Arg;
	if (OLFlags::Value(TEXT("OLNativeMaps"), Arg, /*bShouldStopOnSeparator*/ false)) Tries.Add(FPaths::ConvertRelativePathToFull(Arg.TrimQuotes()));
	const TMap<FString, FString> Settings = AOutlawsLevelActor::ReadSettings();
	if (const FString* Set = Settings.Find(TEXT("NativeMapsPath"))) Tries.Add(FPaths::ConvertRelativePathToFull(Set->TrimQuotes()));
	Tries.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::RootDir(), TEXT("Tools"), TEXT("NativeMaps"))));
#if PLATFORM_ANDROID
	Tries.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectPersistentDownloadDir(), TEXT("Outlaws"), TEXT("NativeMaps"))));
#endif
	for (const FString& T : Tries)
	{
		if (!T.IsEmpty() && FPaths::DirectoryExists(T)) return T;
	}
	return FString();
}

// (BUG-QUESTNATIVEMAPS) how a listing of the native maps' folder went, the line beside ListNativeMaps' own: the engine's two
// listings (the entry's type from readdir, the type from stat) and, on Android, the call underneath (opendir's errno, readdir's
// d_type), so the headset's log says which of the two it was
static FString OlNativeListDiagnosis(const FString& Root)
{
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	int32 Entries = 0, Folders = 0, StatEntries = 0, StatFolders = 0;
	const bool bIter = PF.IterateDirectory(*Root, [&Entries, &Folders](const TCHAR*, bool bDir) { ++Entries; Folders += bDir ? 1 : 0; return true; });
	const bool bStat = PF.IterateDirectoryStat(*Root, [&StatEntries, &StatFolders](const TCHAR*, const FFileStatData& S) { ++StatEntries; StatFolders += S.bIsDirectory ? 1 : 0; return true; });
	FString Out = FString::Printf(TEXT("IterateDirectory %s, %d entries, %d of them folders (by the entry's type); IterateDirectoryStat %s, %d entries, %d folders (by stat)"),
		bIter ? TEXT("true") : TEXT("false"), Entries, Folders, bStat ? TEXT("true") : TEXT("false"), StatEntries, StatFolders);
#if PLATFORM_ANDROID
	errno = 0;
	if (DIR* D = opendir(TCHAR_TO_UTF8(*Root)))
	{
		int32 N = 0, DtDir = 0, DtUnknown = 0;
		while (const dirent* E = readdir(D))
		{
			if (!strcmp(E->d_name, ".") || !strcmp(E->d_name, "..")) continue;
			++N;
			DtDir += E->d_type == DT_DIR ? 1 : 0;
			DtUnknown += E->d_type == DT_UNKNOWN ? 1 : 0;
		}
		closedir(D);
		Out += FString::Printf(TEXT("; opendir ok, readdir %d entries: %d DT_DIR, %d DT_UNKNOWN"), N, DtDir, DtUnknown);
	}
	else
	{
		const int Err = errno;
		Out += FString::Printf(TEXT("; opendir refused: errno %d (%s)"), Err, UTF8_TO_TCHAR(strerror(Err)));
	}
#endif
	return Out;
}

TArray<FOutlawsAssets::FNativeMap> FOutlawsAssets::ListNativeMaps(const FString& Root, FString* OutVia)
{
	// BUG-QUESTNATIVEMAPS (the Quest, 3 Nov, builds 7a8978b and 3c0e307: "OLRT native maps: 0 LABs in .../files/Outlaws/
	// NativeMaps" with the 19 maps pushed there; the PC lists them).  That folder was found (DirectoryExists: a stat) and the
	// install's LABs open (FileExists, a stat, then an open: by name), but FindFiles found no folder in it.  On Android that is
	// AndroidPlatformFile.cpp's IterateDirectory: an opendir and readdir whose entry is a folder only when readdir's d_type
	// says DT_DIR (its visitor, line 1784, never stats), and nothing at all when the opendir is refused (line 1967: false).
	// So the listing here stats each entry (IterateDirectoryStat: the type from stat, as DirectoryExists has it), and when it
	// finds no map's LAB all the same, the folder's own MANIFEST.txt names every file, each found by name as the install's
	// LABs are.  OlNativeListDiagnosis' line says which it was on the device.
	TArray<FNativeMap> Out;
	if (Root.IsEmpty()) return Out;
	static const bool bOldList = OLFlags::Param(TEXT("OLOldNativeList"));
	static const bool bManifestOnly = OLFlags::Param(TEXT("OLNativeListManifestOnly"));
	auto AddFile = [&Out](const FString& Dir, const FString& File)
	{
		const bool bLab = File.EndsWith(TEXT(".lab"), ESearchCase::IgnoreCase);
		if (!bLab && !File.EndsWith(TEXT(".rcm"), ESearchCase::IgnoreCase)) return;
		FNativeMap* M = Out.FindByPredicate([&Dir](const FNativeMap& X) { return X.Dir.Equals(Dir, ESearchCase::IgnoreCase); });
		if (!M)
		{
			M = &Out.AddDefaulted_GetRef();
			M->Dir = Dir;
		}
		TArray<FString>& To = bLab ? M->Labs : M->Rcms;
		if (!To.ContainsByPredicate([&File](const FString& X) { return X.Equals(File, ESearchCase::IgnoreCase); })) To.Add(File);
	};
	IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
	FString Via;
	if (bOldList)
	{
		// (as before: FindFiles, the folders as IterateDirectory types them, no manifest)
		TArray<FString> Dirs;
		IFileManager::Get().FindFiles(Dirs, *(Root / TEXT("*")), false, true);
		for (const FString& D : Dirs)
		{
			TArray<FString> Files;
			IFileManager::Get().FindFiles(Files, *(Root / D / TEXT("*")), true, false);
			for (const FString& F : Files) AddFile(D, F);
		}
		Via = TEXT("listing (-OLOldNativeList: FindFiles, no manifest)");
	}
	else
	{
		if (!bManifestOnly)
		{
			TArray<FString> Dirs;
			PF.IterateDirectoryStat(*Root, [&Dirs](const TCHAR* Path, const FFileStatData& S) { if (S.bIsDirectory) Dirs.Add(FPaths::GetCleanFilename(Path)); return true; });
			for (const FString& D : Dirs)
			{
				PF.IterateDirectoryStat(*(Root / D), [&AddFile, &D](const TCHAR* Path, const FFileStatData& S) { if (!S.bIsDirectory) AddFile(D, FPaths::GetCleanFilename(Path)); return true; });
			}
			Via = TEXT("listing");
		}
		if (!Out.ContainsByPredicate([](const FNativeMap& M) { return M.Labs.Num() > 0; }))
		{
			// the manifest: "md5 bytes <map>/<file> source" a line (tools' stage.sh writes it beside the maps)
			Out.Reset();
			TArray<FString> Lines;
			if (FFileHelper::LoadFileToStringArray(Lines, *(Root / TEXT("MANIFEST.txt"))))
			{
				int32 Missing = 0, SizeOff = 0;
				for (const FString& Raw : Lines)
				{
					const FString Line = Raw.TrimStartAndEnd();
					if (Line.IsEmpty() || Line.StartsWith(TEXT("#"))) continue;
					TArray<FString> T;
					Line.ParseIntoArrayWS(T);
					FString Dir, File;
					if (T.Num() < 3 || !T[2].Replace(TEXT("\\"), TEXT("/")).Split(TEXT("/"), &Dir, &File) || Dir.IsEmpty() || File.IsEmpty() || File.Contains(TEXT("/"))) continue;
					const FString Path = Root / Dir / File;
					if (!PF.FileExists(*Path))
					{
						++Missing;
						continue;
					}
					SizeOff += FCString::Atoi64(*T[1]) != PF.FileSize(*Path) ? 1 : 0;
					AddFile(Dir, File);
				}
				Via = FString::Printf(TEXT("manifest (%s%s%s)"), bManifestOnly ? TEXT("-OLNativeListManifestOnly") : TEXT("the listing found no map's LAB"),
					Missing ? *FString::Printf(TEXT("; %d of its files missing"), Missing) : TEXT(""), SizeOff ? *FString::Printf(TEXT("; %d sizes not the manifest's"), SizeOff) : TEXT(""));
			}
			else if (Via.IsEmpty())
			{
				Via = TEXT("manifest (-OLNativeListManifestOnly; no MANIFEST.txt)");
			}
			else
			{
				Via += TEXT(" (no MANIFEST.txt)");
			}
		}
	}
	// REQ-ASHIKA (the user 4 Nov: "take Nuketown out and replace it with Ashika"): a map no longer shipped, whose folder lingers
	// on an updated install (a drop unzipped over an older one, the headset's folder: the installers add, they do not take
	// away), is neither listed nor mounted.  This folder only: a copy in the player's own game folder stays their custom map
	static const TCHAR* const NoLongerShipped[] = { TEXT("Nuketown") };
	for (const TCHAR* Gone : NoLongerShipped)
	{
		if (Out.RemoveAll([Gone](const FNativeMap& M) { return M.Dir.Equals(Gone, ESearchCase::IgnoreCase); }) > 0) Via += FString::Printf(TEXT("; %s left out: no longer shipped"), Gone);
	}
	auto ByName = [](const FString& A, const FString& B) { return A.ToLower() < B.ToLower(); };
	Out.Sort([&ByName](const FNativeMap& A, const FNativeMap& B) { return ByName(A.Dir, B.Dir); });
	for (FNativeMap& M : Out)
	{
		M.Labs.Sort(ByName);
		M.Rcms.Sort(ByName);
	}
	// the line whenever it changes (Open, ListMaps and MapTitle each ask; once at the start while the folder stays as it is)
	TArray<FString> Names;
	for (const FNativeMap& M : Out) Names.Add(M.Dir);
	const FString Line = FString::Printf(TEXT("OLRT native maps: %d maps (%s) from %s via %s"), Out.Num(), Names.Num() ? *FString::Join(Names, TEXT(", ")) : TEXT("none"), *Root, *Via);
	static FCriticalSection LoggedLock;
	static FString Logged;
	bool bNew = false;
	{
		FScopeLock Lock(&LoggedLock);
		bNew = Line != Logged;
		if (bNew) Logged = Line;
	}
	if (bNew)
	{
		UE_LOG(LogOutlaws, Log, TEXT("%s"), *Line);
		UE_LOG(LogOutlaws, Log, TEXT("OLRT native maps listing of %s: %s"), *Root, *OlNativeListDiagnosis(Root));
	}
	if (OutVia) *OutVia = Via;
	return Out;
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
	// (1 Nov, user: tests read a stock install, not the player's modded one) the environment's OUTLAWSVR_INSTALL, which
	// the test scripts set for every run they start; a player's own launch never has it
	const FString EnvInstall = FPlatformMisc::GetEnvironmentVariable(TEXT("OUTLAWSVR_INSTALL"));
	if (!EnvInstall.IsEmpty())
	{
		Add(FPaths::ConvertRelativePathToFull(EnvInstall.TrimQuotes()), TEXT("OUTLAWSVR_INSTALL"));
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
		// (BUG-DEVDIR) -OLNoDevDir: this PC runs as a tester's install does, without the dev folder
		if (!OLFlags::Param(TEXT("OLNoDevDir")) && !Dev.IsEmpty() && FPaths::DirectoryExists(Dev))
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

const FString& FOutlawsAssets::DevPlaceholderDir()
{
	ResolveItemDirs();
	return GDevItemDir;
}

namespace
{
	FCriticalSection GDevPlaceholderLock;
	TSet<FString> GDevPlaceholderSeen;
}

void FOutlawsAssets::NoteDevPlaceholder(const FString& Path)
{
	FString Dir = PlaceholderDir();
	Dir.ReplaceInline(TEXT("\\"), TEXT("/"));
	if (Dir.IsEmpty() || !FPaths::DirectoryExists(Dir)) return;
	FString P = FPaths::ConvertRelativePathToFull(Path);
	P.ReplaceInline(TEXT("\\"), TEXT("/"));
	if (!P.StartsWith(Dir + TEXT("/"), ESearchCase::IgnoreCase)) return;
	const FString Rel = P.RightChop(Dir.Len() + 1);
	{
		FScopeLock Lock(&GDevPlaceholderLock);
		if (GDevPlaceholderSeen.Contains(Rel)) return;
		GDevPlaceholderSeen.Add(Rel);
	}
	// (the folder's own part only: no user's name in the log)
	UE_LOG(LogOutlaws, Display, TEXT("OLRT dev placeholder used: %s"), *Rel);
}

int32 FOutlawsAssets::DevPlaceholderUses()
{
	FScopeLock Lock(&GDevPlaceholderLock);
	return GDevPlaceholderSeen.Num();
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
