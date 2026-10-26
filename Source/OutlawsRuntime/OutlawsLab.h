#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"

class IFileHandle;

/**
 * One LAB archive (reference: tools/outlaws_lab.py).
 *   header 16B : 'LABN' + uint32 version(65536) + uint32 NFiles + uint32 NamePoolSize
 *   entries    : NFiles x 16B = uint32 NameOffset + uint32 Offset + uint32 Size + 4B ResType
 *   name pool  : null-terminated names
 *   payloads   : contiguous
 * Only the directory is read on open; payloads are read on demand through a kept-open
 * handle (olremaster1.lab is 2.3 GB, so the archive is never loaded whole).
 */
class OUTLAWSRUNTIME_API FOutlawsLabFile
{
public:
	struct FEntry
	{
		FString Name;      // as stored (case preserved)
		uint32 Offset = 0;
		uint32 Size = 0;
	};

	FOutlawsLabFile();
	~FOutlawsLabFile();

	bool Open(const FString& InPath);
	const FString& GetPath() const { return Path; }
	const TMap<FString, int32>& GetIndex() const { return Index; }   // lower-case name -> entry
	bool Read(int32 EntryIndex, TArray<uint8>& Out) const;

private:
	FString Path;
	TArray<FEntry> Entries;
	TMap<FString, int32> Index;
	mutable TUniquePtr<IFileHandle> Handle;
	mutable FCriticalSection ReadLock;
};

/**
 * The player's install: LABs opened in patch precedence order (later patches and the remaster
 * override the originals), each name resolved to the first archive that holds it.
 * Reference: olue_export.py Assets / LAB_SEARCH.
 */
class OUTLAWSRUNTIME_API FOutlawsAssets
{
public:
	/** Search order; each is tried at <Root>/<lab> then <Root>/assets/<lab>. */
	static const TCHAR* LabSearchOrder[];

	/** Open every LAB under Root (and Root/assets). Returns the number of archives opened. */
	int32 Open(const FString& InRoot);
	bool IsOpen() const { return Labs.Num() > 0; }
	const FString& GetRoot() const { return Root; }
	bool Has(const FString& Name) const;
	bool Get(const FString& Name, TArray<uint8>& Out) const;
	bool GetText(const FString& Name, FString& Out) const;   // latin-1 -> FString
	/** A sound: the remaster's (olsfxhd, oltaunthd) or, with !bHD, the original's where it has one. */
	bool GetSound(const FString& Name, bool bHD, TArray<uint8>& Out) const;
	/** Which archive a name resolves to (for diagnostics). */
	FString Where(const FString& Name) const;
	/** Every name with the given extension (lower case, e.g. ".lvt"). */
	void ListByExtension(const FString& LowerExt, TArray<FString>& Out) const;

	/** Install-folder discovery, first that qualifies (found once, then remembered): -OLInstall=<dir>, InstallPath= in
	 *  Saved/OutlawsSettings.ini, the configured path, the drop-in folders (beside the exe, a headset's files folder),
	 *  Steam's libraries, GOG's remaster, the usual Steam folders, GOG's classic Outlaws.  Never asks. */
	static FString FindInstallRoot(const FString& ConfiguredPath);
	static bool LooksLikeInstall(const FString& Dir);
	/** True where AskForInstallRoot can put a window up: a Windows game run with a person at it (not -unattended, not
	 *  the editor), and it has not asked yet this run. */
	static bool CanAskForInstall();
	/** Windows: explain, then a folder picker until the player picks an install or cancels; the pick is kept as
	 *  InstallPath= in OutlawsSettings.ini.  Blocks the game thread.  Empty when cancelled. */
	static FString AskForInstallRoot();
	/** Our own loose assets (tools/olue/placeholders: held and 3D item OBJs, VE weapons, grab points): the
	 *  dev folder on the PC, else <files>/Placeholders on a headset (olvr_install pushes it). No trailing slash. */
	static const FString& PlaceholderDir();
	/** True when the folder above came from -OLItemDir, OUTLAWSVR_ITEMDIR or [Outlaws] ItemDir (a dev PC). */
	static bool HasDevItemDir();

	/** Shared instance per root, so repeated builds do not reopen the archives. */
	static TSharedPtr<FOutlawsAssets> GetShared(const FString& InRoot);
	/** (the soak's count) how many install roots GetShared holds open. */
	static int32 SharedCount();

private:
	FString Root;
	TArray<TSharedPtr<FOutlawsLabFile>> Labs;
	TMap<FString, TPair<int32, int32>> Index;   // lower name -> (lab index, entry index)
};
