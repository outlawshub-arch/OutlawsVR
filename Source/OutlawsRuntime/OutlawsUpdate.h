#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "Tickable.h"
#include "OutlawsUpdate.generated.h"

class AOutlawsJoinBubble;
class AOutlawsLevelActor;

/**
 * REQ-AUTOUPDATE (user rulings 1 and 4 Nov): the game updates itself, on the Quest and PC VR, no separate app. An unseen
 * check at startup while the game launches as normal; a prompt in the join bubble only when a newer full release is found,
 * on the main menu only (Update now / Not now / Skip this build); offline or on any error, nothing. An Options switch turns
 * the check off. Hashes checked, saves kept, nothing uninstalled. The check tells GitHub the player's IP (notes and manual
 * say so). latest.json sits beside the release zip, written by tools/olue/release/make_latest_json.py; the one zip carries
 * both platforms, the APK and the menu theme and native maps with it.
 */
namespace OLUpdate
{
	/** latest.json. */
	struct FLatest
	{
		int32 Build = 0;           // the rising internal number: StoreVersion, the Android versionCode
		FString Version, Tag, Commit, Zip, ZipSha256, ApkSha256;
		int64 ZipSize = 0;
	};
	bool ParseLatest(const FString& Json, FLatest& Out);
	FString FeedUrl();
	FString ZipUrl(const FLatest& L);

	/** This build's own number (OL_UPDATE_BUILD, from the Build.cs). */
	OUTLAWSRUNTIME_API int32 MyBuild();

	enum class EDecide : uint8 { Nothing, Skipped, Prompt };
	EDecide Decide(const FLatest& L, int32 Mine, int32 SkipBuild);

	class FSha256
	{
	public:
		FSha256();
		void Update(const uint8* Data, int64 Len);
		FString Finish();          // lower-case hex
	private:
		void Block(const uint8* P);
		uint32 H[8];
		uint8 Buf[64];
		int32 Fill = 0;
		uint64 Total = 0;
	};
	FString Sha256OfFile(const FString& Path);          // empty when it cannot be read

	struct FZipEntry
	{
		FString Name;              // '/' separated
		uint16 Method = 0;         // 0 stored, 8 deflated
		uint32 Crc = 0, CompSize = 0, Size = 0, LocalOffset = 0;
	};
	bool ReadZipIndex(const FString& ZipPath, TArray<FZipEntry>& Out);
	bool SafeEntryName(const FString& Name);
	bool ExtractEntry(FArchive& Zip, const FZipEntry& E, const FString& ToFile);

	/** The PC package out of the zip into NewDir: its OutlawsVR folder's contents; refused without the exe, theme and maps. */
	bool ExtractForPc(const FString& ZipPath, const FString& NewDir, FString& Why);
	/** The Quest's part out of the zip: the theme and maps into DataDir, the APK to ApkTo, its hash checked. */
	bool ExtractForQuest(const FString& ZipPath, const FString& DataDir, const FString& ApkTo, const FString& ApkSha256, FString& Why);
	/** The swap's script: runs from <root>\Update\ after the game exits. Args: the game's pid, then "test" in the probe. */
	const TCHAR* SwapScript();

	/** Base/data when Base/build.txt names this build or a later one (older means a newer install came over it), else empty. */
	FString UpdatedDataIn(const FString& Base, int32 Mine);
	/** The Quest's updated data folder, or empty: the lookups of MenuOverlay.kpf and NativeMaps try it first. Empty on PC. */
	OUTLAWSRUNTIME_API FString UpdatedDataDir();
}

UCLASS()
class OUTLAWSRUNTIME_API UOutlawsUpdate : public UGameInstanceSubsystem, public FTickableGameObject
{
	GENERATED_BODY()

public:
	enum class EStep : uint8
	{
		Off, Wait, Checking, Found, Prompt, Downloading, Working, Ready, Allow, Installing, Exiting, Failed, Done
	};

	static UOutlawsUpdate* Get(const UObject* Context);
	/** The Options row "update_check": on unless the settings file says off. */
	static bool CheckOn();
	static void ToggleCheck();
	/** A hand's laser is on the update bubble's button: the trigger is the bubble's. */
	static bool HandOnBubble(const UObject* Context, int32 Hand);
	/** The update bubble is up: the join bubbles hold back. */
	static bool BubbleUp(const UObject* Context);

	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void Tick(float DeltaTime) override;
	virtual TStatId GetStatId() const override;
	virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Conditional; }
	virtual bool IsTickable() const override { return !IsTemplate() && Step != EStep::Off && Step != EStep::Done; }
	virtual bool IsTickableInEditor() const override { return false; }
	virtual UWorld* GetTickableGameObjectWorld() const override;

	/** Picked on the bubble: 0 the yes button, 1 the no button, 2 the third. */
	void Pick(int32 Button);

#if WITH_OUTLAWS_TESTS
	/** The probe's way in: Feed is a folder standing for the release (empty: the real one), Root the scratch install. */
	struct FTestRun
	{
		FString Feed, Root;
		int32 Build = 0;
		bool bFrontEnd = true, bQuest = false, bOld = false;
		TMap<FString, FString> Settings;
	};
	void TestStart(const FTestRun& Run);
	void TestStop();
	void TestSetFrontEnd(bool bOn) { Test.bFrontEnd = bOn; }
	EStep TestGetStep() const { return Step; }
	const FTestRun& TestGetRun() const { return Test; }
	bool TestPromptUp() const { return bPromptUp; }
	const FString& TestWords() const { return Words; }
	const FString& TestWhy() const { return WhyFailed; }
#endif

private:
	EStep Step = EStep::Off;
	double NextAt = 0.0;
	OLUpdate::FLatest Latest;
	FString WorkDir, Part, ZipFile;      // where the download goes
	int64 Have = 0;
	int32 Retries = 0;
	int32 Allowed = 0;                   // Quest: 1 the unknown-apps page is open
	bool bPromptUp = false;
	bool bQuest = false;
	FString Words, WhyFailed;
	TFuture<FString> Work;               // the hash and the unpack, off the game thread: "" is fine, else why not
	UPROPERTY(Transient)
	TObjectPtr<AOutlawsJoinBubble> Bubble;

#if WITH_OUTLAWS_TESTS
	FTestRun Test;
	bool bTest = false;
#endif

	FString RootDir() const;
	bool InMenu() const;
	bool MayShow() const;
	TMap<FString, FString> Settings() const;
	void WriteSetting(const FString& Key, const FString& Value);
	void Fetch(const FString& Url, int64 From, int64 To, TFunction<void(int32 Code, const TArray<uint8>& Body)> Done);
	void BeginCheck();
	void OnFeed(int32 Code, const TArray<uint8>& Body);
	void BeginDownload();
	void NextChunk();
	void OnChunk(int32 Code, const TArray<uint8>& Body);
	void BeginWork();
	void AfterWork(const FString& Why);
	void Finish();
	void Fail(const FString& Why, const FString& Text);
	void Say(const FString& Text, const FString& Yes = FString(), const FString& No = FString(), const FString& Third = FString());
	void Hide();
	void CleanOldUpdate();
};
