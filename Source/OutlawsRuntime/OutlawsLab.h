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
	/** (REQ-NATIVEMAPS, tests) how many LAB files are open in this run (each holds a read handle). */
	static int32 NumLive();

	bool Open(const FString& InPath);
	const FString& GetPath() const { return Path; }
	const TMap<FString, int32>& GetIndex() const { return Index; }   // lower-case name -> entry
	bool Read(int32 EntryIndex, TArray<uint8>& Out) const;
	/** (REQ-NATIVEMAPS, the join guard) a hash of this LAB: its directory (every file's name, place and size) and the file's
	 *  size, read with the directory (free): two copies of a map whose files differ in any way the directory shows differ. */
	uint32 GetStamp() const { return Stamp; }

private:
	FString Path;
	uint32 Stamp = 0;
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
	/** (REQ-LANGUAGE) the language's LABs (OLLanguage::LabsFor) open in front of the stock order, behind a native map's own
	 *  LAB: a name they hold is theirs, any other name the stock one's.  Open mounts OLLanguage::Current(); the level build
	 *  calls it again (AOutlawsLevelActor::Build, before it reads a file), so a change applies at the next level start, never
	 *  mid-level.  True when the mounted set changed: the caller forgets every file it holds by name (ForgetLabFiles), and
	 *  OverlaySerial moves on.  A language whose LABs are not in the install mounts none (English).  -OLOldLanguage: never
	 *  any. */
	bool UseLanguage(const FString& Code);
	/** (REQ-LANGUAGE step 4) the LAB GetSound would read a sound from (for the probe): the native map's, the language's
	 *  taunts (oltaunt_<l>), an HD set or the stock order's; empty when none holds it. */
	FString WhereSound(const FString& Name, bool bHD) const;
	/** The language's taunt LAB mounted now (oltaunt_<l>.lab), empty when none (English, French, or not in the install). */
	FString GetTauntLab() const;
	/** The language whose LABs are mounted now ("en" when none). */
	const FString& GetLanguage() const { return Language; }
	/** The search order as it stands: the native map's LAB (when one is open), the language's, then the stock ones. */
	TArray<FString> MountedOrder() const;
	/** (REQ-LANGUAGE, the message merge) one layer's copy of a text file: the stock order's alone (English), the language
	 *  LABs' alone, or the native map's alone.  False when that layer has none. */
	enum class ELayer : uint8 { Stock, Language, Overlay };
	bool GetTextFrom(ELayer Layer, const FString& Name, FString& Out) const;
	/** Every name with the given extension (lower case, e.g. ".lvt"), from the install's LABs (not the native maps'). */
	void ListByExtension(const FString& LowerExt, TArray<FString>& Out) const;
	/** (REQ-MAPQUICK) The native maps' folder (one sub-folder a map: its LAB, RCM and readme): -OLNativeMaps=<dir>, else
	 *  NativeMapsPath in OutlawsSettings.ini (editor runs), else Tools\NativeMaps in the PC package, else
	 *  <files>/Outlaws/NativeMaps on a headset.  Empty with -OLNoNativeMaps or when none of them exists. */
	static FString NativeMapsRoot();
	/** (BUG-QUESTNATIVEMAPS) one native map: its folder under the native maps' root and the .lab and .rcm files in it (names
	 *  only, as the folder or the manifest spells them, by name). */
	struct FNativeMap
	{
		FString Dir;
		TArray<FString> Labs;
		TArray<FString> Rcms;
	};
	/** (BUG-QUESTNATIVEMAPS, the Quest 3 Nov: the folder found, nothing in it) the native maps under Root, by folder name, as Open,
	 *  ListMaps and MapTitle read them: a listing that stats every entry (IterateDirectoryStat), else, when it finds no map's
	 *  LAB, the folder's own MANIFEST.txt ("md5 bytes <map>/<file> source" a line), each file it names found by name.  The
	 *  line "OLRT native maps: N maps (<folders>) from <root> via <listing|manifest>" (and the listing's own diagnosis line)
	 *  whenever what it finds changes, so once at the start.  -OLOldNativeList: the old FindFiles listing only.
	 *  -OLNativeListManifestOnly (tests): the manifest only. */
	static TArray<FNativeMap> ListNativeMaps(const FString& Root, FString* OutVia = nullptr);
	/** The native maps' folder Open read (empty when none), and how many of their LABs it found (mounted, -OLOldNativeMount). */
	const FString& GetNativeRoot() const { return NativeRoot; }
	int32 NumNativeLabs() const { return bOldNativeMount ? Labs.Num() - NumStockLabs : NativeLabs.Num(); }
	/** (REQ-NATIVEMAPS) the native LAB that holds <Level>.lvt (Open read the native LABs' directories, mounting none), empty
	 *  for a stock level or none.  HasLevel: the level's .lvt is in the install's LABs or a native map's. */
	FString NativeLabFor(const FString& Level) const;
	bool HasLevel(const FString& Level) const;
	/** (REQ-NATIVEMAPS, the join guard) a native map level's LAB stamp (FOutlawsLabFile::GetStamp, 8 hex digits), empty for a
	 *  stock level or none; NativeStampList: every native level as "level.stamp", joined by "~", by name (the joiner's URL
	 *  carries it as an option, the host's session too: no '?', '=' or ':' in it). */
	FString NativeStampFor(const FString& Level) const;
	FString NativeStampList() const;
	/** (REQ-NATIVEMAPS, user 2 Nov) the per-map overlay: a native map's level builds with THAT map's LAB in front of the stock
	 *  order (the original opens the level's LAB first, so its files win for its own level only), no other native LAB open; a
	 *  stock level with none.  The level build calls it (AOutlawsLevelActor::Build, on the game thread, before it reads a
	 *  file), so the overlay changes only between levels, never mid-frame.  True when it changed: the caller then forgets
	 *  every file it holds by name (AOutlawsLevelActor::ForgetLabFiles).  Owner: the level actor that opened it, so an old
	 *  world's end never closes a new world's overlay.  -OLOldNativeMount: MAPQUICK's lowest-priority mount, never an overlay. */
	bool UseLevelOverlay(const FString& Level, const void* Owner);
	/** The level ended: its owner closes the overlay (true when one was open and its own). */
	bool CloseOverlay(const void* Owner);
	/** The overlay LAB's path, empty when none. */
	FString GetOverlay() const;
	/** Counts every overlay change in this run (any root): a cache filled under another count holds an old map's files. */
	static uint32 OverlaySerial();
	static bool OldNativeMount();

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
	/** The dev PC's local item folder (the one HasDevItemDir reports; empty when none is set).  (BUG-DEVDIR)
	 *  -OLNoDevDir: empty, so this PC runs as a tester's install does (no dev placeholders). */
	static const FString& DevPlaceholderDir();
	/** (BUG-DEVDIR) A file about to be read: when it lies in PlaceholderDir (this PC's dev folder, or the copy pushed from it
	 *  to the user's headset: testers have neither), "OLRT dev placeholder used: <file>" once a file, in every build (the
	 *  package step's check reads it from the Shipping log).  OutlawsObj's OBJ and texture loads call it. */
	static void NoteDevPlaceholder(const FString& Path);
	/** (tests) how many placeholder files this run has read. */
	static int32 DevPlaceholderUses();

	/** Shared instance per root, so repeated builds do not reopen the archives. */
	static TSharedPtr<FOutlawsAssets> GetShared(const FString& InRoot);
	/** (the soak's count) how many install roots GetShared holds open. */
	static int32 SharedCount();

private:
	FString Root;
	FString NativeRoot;
	int32 NumStockLabs = 0;                      // Labs[0..NumStockLabs) are the install's; the native maps' follow (-OLOldNativeMount)
	TArray<TSharedPtr<FOutlawsLabFile>> Labs;
	TMap<FString, TPair<int32, int32>> Index;   // lower name -> (lab index, entry index)
	// (REQ-NATIVEMAPS) the native LABs found (full paths), each level's .lvt to its LAB, and the one open over its level
	bool bOldNativeMount = false;
	TArray<FString> NativeLabs;
	TArray<uint32> NativeStamps;                // NativeLabs' stamps, in step
	TMap<FString, int32> NativeLevels;          // lower level name -> NativeLabs index
	TSharedPtr<FOutlawsLabFile> Overlay;
	const void* OverlayOwner = nullptr;
	// (REQ-LANGUAGE) the language's LABs and their names (lower name -> (LangLabs index, entry index)), searched after the
	// overlay and before Index
	FString Language = TEXT("en");
	TArray<TSharedPtr<FOutlawsLabFile>> LangLabs;
	TMap<FString, TPair<int32, int32>> LangIndex;
	// (step 4) the language's taunts and voice lines (oltaunt_<l>.lab): GetSound looks here before the stock sets, in both
	// the HD and the classic branch (they hold WAV only, the originals' quality; a name they lack goes on as before)
	TSharedPtr<FOutlawsLabFile> TauntLab;
	const FOutlawsLabFile* FindSound(const FString& Name, bool bHD, int32& OutEntry) const;
	const TPair<int32, int32>* FindLang(const FString& Key) const { return LangIndex.Find(Key); }
};
