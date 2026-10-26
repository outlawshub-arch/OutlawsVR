// REQ-UNLOCKS: the achievements store and the unlock rules (OutlawsUnlocks.h has the file's layout and the rules).
#include "OutlawsUnlocks.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsCoop.h"
#include "OutlawsAchievements.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformProcess.h"
#include "HAL/CriticalSection.h"

namespace OutlawsUnlocks
{
	const TCHAR* const StoryAny = TEXT("story");
	const TCHAR* const StoryUgly = TEXT("story_ugly");
}

namespace
{
	const TCHAR* const NoName = TEXT("(no name)");

	struct FStore
	{
		TSharedPtr<FJsonObject> Root;
		FDateTime Stamp;                 // the file's when last read (FDateTime::MinValue(): none)
		double CheckedAt = -1.0;
		bool bDamaged = false;
	};

	FStore& Store()
	{
		static FStore S;
		return S;
	}

	// the file read again (missing: empty; unreadable: empty and damaged)
	void ReadStore(FStore& S)
	{
		const FString Path = OutlawsUnlocks::StorePath();
		S.Root = MakeShared<FJsonObject>();
		S.bDamaged = false;
		S.Stamp = IFileManager::Get().GetTimeStamp(*Path);
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path)) return;           // (no file: a fresh start)
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
		const TSharedPtr<FJsonObject>* Profiles = nullptr;
		if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid() || (Root->HasField(TEXT("profiles")) && !Root->TryGetObjectField(TEXT("profiles"), Profiles)))
		{
			S.bDamaged = true;
			UE_LOG(LogOutlaws, Warning, TEXT("OLUNLOCK the achievements file is damaged (%d bytes, %s): everything starts locked"), Text.Len(), *Path);
			return;
		}
		S.Root = Root;
	}

	// the cached store, read again when the file changed (another of this machine's games wrote it), looked at once a second
	FStore& Fresh()
	{
		FStore& S = Store();
		const double Now = FPlatformTime::Seconds();
		if (!S.Root.IsValid())
		{
			ReadStore(S);
			S.CheckedAt = Now;
			const FString Path = OutlawsUnlocks::StorePath();
			UE_LOG(LogOutlaws, Log, TEXT("OLUNLOCK store %s: %s"), *Path, S.bDamaged ? TEXT("damaged, started locked") : IFileManager::Get().FileExists(*Path) ? TEXT("read") : TEXT("none yet (everything locked)"));
		}
		else if (Now - S.CheckedAt > 1.0)
		{
			S.CheckedAt = Now;
			if (IFileManager::Get().GetTimeStamp(*OutlawsUnlocks::StorePath()) != S.Stamp) ReadStore(S);
		}
		return S;
	}

	// the profile's record (bCreate: made when missing); its name matched without regard to case
	TSharedPtr<FJsonObject> ProfileRecord(const TSharedPtr<FJsonObject>& Root, const FString& Name, bool bCreate)
	{
		if (!Root.IsValid()) return nullptr;
		const TSharedPtr<FJsonObject>* Profiles = nullptr;
		if (!Root->TryGetObjectField(TEXT("profiles"), Profiles) || !Profiles || !Profiles->IsValid())
		{
			if (!bCreate) return nullptr;
			Root->SetObjectField(TEXT("profiles"), MakeShared<FJsonObject>());
			Root->TryGetObjectField(TEXT("profiles"), Profiles);
		}
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Kv : (*Profiles)->Values)
		{
			if (Kv.Key.Equals(Name, ESearchCase::IgnoreCase) && Kv.Value.IsValid() && Kv.Value->Type == EJson::Object) return Kv.Value->AsObject();
		}
		if (!bCreate) return nullptr;
		const TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
		P->SetObjectField(TEXT("earned"), MakeShared<FJsonObject>());
		P->SetArrayField(TEXT("unseen"), TArray<TSharedPtr<FJsonValue>>());
		(*Profiles)->SetObjectField(Name, P);
		return P;
	}

	TArray<FString> Unseen(const TSharedPtr<FJsonObject>& P)
	{
		TArray<FString> Out;
		const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
		if (P.IsValid() && P->TryGetArrayField(TEXT("unseen"), A))
		{
			for (const TSharedPtr<FJsonValue>& V : *A) if (V.IsValid() && V->Type == EJson::String) Out.Add(V->AsString());
		}
		return Out;
	}

	void SetUnseen(const TSharedPtr<FJsonObject>& P, const TArray<FString>& Ids)
	{
		TArray<TSharedPtr<FJsonValue>> A;
		for (const FString& Id : Ids) A.Add(MakeShared<FJsonValueString>(Id));
		P->SetArrayField(TEXT("unseen"), A);
	}

	bool EarnedIn(const TSharedPtr<FJsonObject>& P, const FString& Id)
	{
		const TSharedPtr<FJsonObject>* E = nullptr;
		return P.IsValid() && P->TryGetObjectField(TEXT("earned"), E) && E && E->IsValid() && (*E)->HasField(Id);
	}

	// a change made to the file as it is now on the disk (another game on this machine may have written its own profile since
	// this one read it), then written; a damaged file is kept beside it first
	bool WriteStore(TFunctionRef<void(const TSharedPtr<FJsonObject>&)> Change)
	{
		FStore& S = Store();
		const FString Path = OutlawsUnlocks::StorePath();
		// (REQ-LEVELTIMER: two games on one machine, a co-op pair, may both end a level this instant: one at a time, each reading
		// the file as the other left it; no lock to be had in 3 s: it goes on without)
		// (BUG-UNLOCKALLQUEST: Android and iOS have no system-wide lock, and UE's stand-in there is a fatal error that quit the Quest
		// at the first write; one game per headset, so those go on without it)
#if PLATFORM_WINDOWS || PLATFORM_MAC || PLATFORM_UNIX
		FSystemWideCriticalSection StoreLock(TEXT("OutlawsAchievementsStore"), FTimespan::FromSeconds(3.0));
#endif
		ReadStore(S);
		if (S.bDamaged)
		{
			// (REQ-LEVELTIMER: another game on this machine may be writing it this instant: once more before it is called damaged)
			FPlatformProcess::Sleep(0.15f);
			ReadStore(S);
		}
		if (S.bDamaged)
		{
			const FString Keep = Path + TEXT(".damaged");
			IFileManager::Get().Move(*Keep, *Path, true, true);
			UE_LOG(LogOutlaws, Log, TEXT("OLUNLOCK the damaged achievements file kept as %s"), *Keep);
			S.bDamaged = false;
		}
		S.Root->SetNumberField(TEXT("version"), 1);
		Change(S.Root);
		FString Text;
		const TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Text);
		FJsonSerializer::Serialize(S.Root.ToSharedRef(), W);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		// (written beside it and moved over, so a game reading at that moment never sees half a file; tried again when another
		// game on this machine has the file that instant)
		bool bOk = false;
		const FString Tmp = Path + TEXT(".tmp");
		for (int32 Try = 0; Try < 6 && !bOk; ++Try)
		{
			if (Try > 0) FPlatformProcess::Sleep(0.05f);
			bOk = FFileHelper::SaveStringToFile(Text, *Tmp, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM) && IFileManager::Get().Move(*Path, *Tmp, true, true);
		}
		S.Stamp = IFileManager::Get().GetTimeStamp(*Path);
		S.CheckedAt = FPlatformTime::Seconds();
		if (!bOk) UE_LOG(LogOutlaws, Warning, TEXT("OLUNLOCK could not write %s"), *Path);
		return bOk;
	}

}

FString OutlawsUnlocks::StorePath()
{
	static const FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("OutlawsSaves") / TEXT("achievements.json"));
	return Path;
}

FString OutlawsUnlocks::ProfileOf(const AOutlawsLevelActor* L)
{
	FString Name;
	if (L) Name = L->MpPlayerName;
	else if (const FString* N = AOutlawsLevelActor::ReadSettings().Find(TEXT("MpPlayerName"))) Name = *N;
	Name = AOutlawsLevelActor::CleanMpName(Name);
	return Name.IsEmpty() ? FString(NoName) : Name;
}

bool OutlawsUnlocks::IsEarned(const AOutlawsLevelActor* L, const TCHAR* Id)
{
	return EarnedIn(ProfileRecord(Fresh().Root, ProfileOf(L), false), Id);
}

bool OutlawsUnlocks::UnlockAll()
{
	static const bool bArg = OLFlags::Param(TEXT("OLUnlockAll"));
	if (bArg) return true;
	bool bOn = false;
	const FStore& S = Fresh();
	return S.Root.IsValid() && S.Root->TryGetBoolField(TEXT("unlockAllModes"), bOn) && bOn;
}

void OutlawsUnlocks::SetUnlockAll(bool bOn)
{
	WriteStore([bOn](const TSharedPtr<FJsonObject>& Root) { Root->SetBoolField(TEXT("unlockAllModes"), bOn); });
	UE_LOG(LogOutlaws, Log, TEXT("OLUNLOCK unlock all modes: %s (nothing earned is changed)"), bOn ? TEXT("on") : TEXT("off"));
}

bool OutlawsUnlocks::IsUnlocked(const AOutlawsLevelActor* L, EMode Mode)
{
	// UNLOCKFREE (user decision for 0.95): the Randomizer and the Bad guys multiplier are every player's from the first start
	return true;
}

bool OutlawsUnlocks::StoreDamaged()
{
	return Fresh().bDamaged;
}

void OutlawsUnlocks::NoteLevelWon(AOutlawsLevelActor* L)
{
	// the main story's last level: no level after it, and the level is one of the story's (OUTLAWS.RCS, a shuffled run's last
	// included); a Historical Mission (Marshal Training, or a single mission level) never
	if (!L || L->bHom || L->IsFrontEnd() || !L->UnlockNoLevelAfter()) return;
	TArray<FString> Story, Movies;
	if (!L->RandoStory(Story, Movies) || !Story.Contains(L->LevelName.ToLower())) return;
	L->UnlockStoryPending = int32(L->Difficulty);
	UE_LOG(LogOutlaws, Log, TEXT("OLUNLOCK the main story's last level (%s) won on %s: earned when its cutscene ends (profile %s)"), *L->LevelName,
		OutlawsDifficulty::Name(L->Difficulty), *ProfileOf(L));
}

void OutlawsUnlocks::EarnPending(AOutlawsLevelActor* L, const TCHAR* Where)
{
	if (!L || L->UnlockStoryPending < 0) return;
	const EOutlawsDifficulty Diff = static_cast<EOutlawsDifficulty>(L->UnlockStoryPending);
	L->UnlockStoryPending = -1;
	const FString Profile = ProfileOf(L);
	const bool bCoop = UOutlawsCoop::IsActive(L);
	TArray<const TCHAR*> Ids = { StoryAny };
	if (Diff == EOutlawsDifficulty::Ugly) Ids.Add(StoryUgly);
	TArray<FString> New;
	WriteStore([&](const TSharedPtr<FJsonObject>& Root)
	{
		const TSharedPtr<FJsonObject> P = ProfileRecord(Root, Profile, true);
		const TSharedPtr<FJsonObject>* E = nullptr;
		if (!P->TryGetObjectField(TEXT("earned"), E) || !E || !E->IsValid())
		{
			P->SetObjectField(TEXT("earned"), MakeShared<FJsonObject>());
			P->TryGetObjectField(TEXT("earned"), E);
		}
		for (const TCHAR* Id : Ids)
		{
			if ((*E)->HasField(Id)) continue;
			const TSharedRef<FJsonObject> Rec = MakeShared<FJsonObject>();
			Rec->SetStringField(TEXT("when"), FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")));
			Rec->SetStringField(TEXT("difficulty"), OutlawsDifficulty::Name(Diff));
			Rec->SetBoolField(TEXT("coop"), bCoop);
			(*E)->SetObjectField(Id, Rec);
			New.Add(Id);              // (UNLOCKFREE: no unlock page queued; the toast below says it was earned)
		}
	});
	// REQ-EXTRAS: a toast for each new one, and the finish's other achievements (Bad or Ugly, co-op)
	for (const FString& Id : New) OutlawsAchievements::QueueToast(L, Id);
	OutlawsAchievements::NoteStoryFinish(L, int32(Diff), bCoop);
	UE_LOG(LogOutlaws, Log, TEXT("OLUNLOCK earned on profile %s (%s, %s%s): %s"), *Profile, Where, OutlawsDifficulty::Name(Diff), bCoop ? TEXT(", co-op") : TEXT(""),
		New.Num() > 0 ? *FString::Join(New, TEXT(", ")) : TEXT("nothing new (already earned)"));
}

namespace
{
	// profile -> times -> level key (lower case) -> difficulty name: the record there (nullptr: none; bCreate makes the path,
	// replacing a "times" or level entry of the wrong kind)
	TSharedPtr<FJsonObject> TimeRecord(const TSharedPtr<FJsonObject>& P, const FString& LevelKey, int32 Difficulty, bool bCreate)
	{
		if (!P.IsValid()) return nullptr;
		const FString Key = LevelKey.ToLower();
		const FString Diff = OutlawsDifficulty::Name(static_cast<EOutlawsDifficulty>(FMath::Clamp(Difficulty, 0, 2)));
		auto Child = [bCreate](const TSharedPtr<FJsonObject>& Parent, const FString& Name) -> TSharedPtr<FJsonObject>
		{
			const TSharedPtr<FJsonObject>* O = nullptr;
			if (Parent->TryGetObjectField(Name, O) && O && O->IsValid()) return *O;
			if (!bCreate) return nullptr;
			Parent->SetObjectField(Name, MakeShared<FJsonObject>());
			return Parent->TryGetObjectField(Name, O) && O ? *O : nullptr;
		};
		const TSharedPtr<FJsonObject> Times = Child(P, TEXT("times"));
		const TSharedPtr<FJsonObject> Lv = Times.IsValid() ? Child(Times, Key) : nullptr;
		return Lv.IsValid() ? Child(Lv, Diff) : nullptr;
	}
}

bool OutlawsUnlocks::BestTime(const AOutlawsLevelActor* L, const FString& LevelKey, int32 Difficulty, int32& OutSeconds)
{
	const TSharedPtr<FJsonObject> R = TimeRecord(ProfileRecord(Fresh().Root, ProfileOf(L), false), LevelKey, Difficulty, false);
	double S = 0.0;
	if (!R.IsValid() || !R->TryGetNumberField(TEXT("seconds"), S) || S < 0.0) return false;
	OutSeconds = int32(S);
	return true;
}

bool OutlawsUnlocks::NoteBestTime(const AOutlawsLevelActor* L, const FString& LevelKey, int32 Difficulty, int32 Seconds, bool bCoop, int32& OutPrior)
{
	OutPrior = -1;
	BestTime(L, LevelKey, Difficulty, OutPrior);
	if (OutPrior >= 0 && Seconds >= OutPrior) return false;
	const FString Profile = ProfileOf(L);
	WriteStore([&](const TSharedPtr<FJsonObject>& Root)
	{
		const TSharedPtr<FJsonObject> P = ProfileRecord(Root, Profile, true);
		const TSharedPtr<FJsonObject> R = TimeRecord(P, LevelKey, Difficulty, true);
		if (!R.IsValid()) return;
		R->SetNumberField(TEXT("seconds"), Seconds);
		R->SetStringField(TEXT("when"), FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")));
		R->SetBoolField(TEXT("coop"), bCoop);
	});
	UE_LOG(LogOutlaws, Log, TEXT("OLUNLOCK best time on profile %s: %s on %s %d s (was %s)"), *Profile, *LevelKey, OutlawsDifficulty::Name(static_cast<EOutlawsDifficulty>(FMath::Clamp(Difficulty, 0, 2))), Seconds,
		OutPrior >= 0 ? *FString::Printf(TEXT("%d s"), OutPrior) : TEXT("none"));
	return true;
}

bool OutlawsUnlocks::NextNotice(const AOutlawsLevelActor* L, FString& OutId, FString& OutTitle, FString& OutWhere)
{
	// UNLOCKFREE: nothing unlocks any more, so no unlock page after the credits; the finish's achievements were toasted as they
	// were earned and are on Extras > Achievements.  An old file's "unseen" ids ("story", "story_ugly") stay in it unshown.
	return false;
}

void OutlawsUnlocks::MarkSeen(const AOutlawsLevelActor* L, const FString& Id)
{
	const FString Profile = ProfileOf(L);
	WriteStore([&](const TSharedPtr<FJsonObject>& Root)
	{
		if (const TSharedPtr<FJsonObject> P = ProfileRecord(Root, Profile, false))
		{
			TArray<FString> Q = Unseen(P);
			Q.Remove(Id);
			SetUnseen(P, Q);
		}
	});
	UE_LOG(LogOutlaws, Log, TEXT("OLUNLOCK page shown and carried on from: %s (profile %s)"), *Id, *Profile);
}

TSharedPtr<FJsonObject> OutlawsUnlocks::ProfileRead(const AOutlawsLevelActor* L)
{
	return ProfileRecord(Fresh().Root, ProfileOf(L), false);
}

void OutlawsUnlocks::ProfileChange(const AOutlawsLevelActor* L, TFunctionRef<void(const TSharedPtr<FJsonObject>&)> Change)
{
	const FString Profile = ProfileOf(L);
	WriteStore([&](const TSharedPtr<FJsonObject>& Root) { Change(ProfileRecord(Root, Profile, true)); });
}
