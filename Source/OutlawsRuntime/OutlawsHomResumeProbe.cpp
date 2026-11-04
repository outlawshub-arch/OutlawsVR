// REQ-NOHOMRESUME's probe (the user 4 Nov: Historical Missions has its own autosave slot now, so "Resume Marshal Training" only
// clutters that page): -OLShots -OLLevel=hideout -OLNoFrontEnd -OLHomResumeProbe [-OLOldHomResume]
// The Historical Missions page with hom_autosave.json and autosave_hom.json laid out by hand: no session; a session and no slot (a
// player from v0.95); a session and a slot with no session in it (their old autosave, moved by the migration); a slot holding the
// session; a slot behind the file; a slot ahead of it.  The row shows only where the slot does not hold the session
// (-OLOldHomResume: whenever there is a session), Marshal Training then at the top under it.  Then the row's own way in with the
// Autosave option off, the v0.95 player's: the office is up, the session back, and the Historical autosave is written with the
// session in it (the file's arrival, so the row is gone; -OLOldHomResume: no slot).  The two files put back at the end; what the run
// adds to Saved goes with saved.ps1.  Lines "OLSHOT homresume check PASS|FAIL: ...", then "OLSHOT homresume RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsHomResumeProbeLocal
{
	int32 Stage = 0, Passes = 0, Fails = 0;
	double StageAt = 0.0;
	bool bWasAutosave = true;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT homresume check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}

	FString FilePath() { return FPaths::GetPath(AOutlawsLevelActor::SavePath(-1)) / TEXT("hom_autosave.json"); }
	FString SlotPath() { return AOutlawsLevelActor::SavePath(AOutlawsLevelActor::AutosaveHom); }

	/** A file out of the way, never deleted: the first time to <file>.was, the player's own, and it comes back at the end. */
	void Clear(const FString& Path)
	{
		const FString Was = Path + TEXT(".was");
		if (IFileManager::Get().FileExists(*Path) && !IFileManager::Get().FileExists(*Was)) IFileManager::Get().Move(*Was, *Path);
		else if (IFileManager::Get().FileExists(*Path)) IFileManager::Get().Move(*(Path + TEXT(".probe")), *Path);
	}

	void PutBack(const FString& Path)
	{
		const FString Was = Path + TEXT(".was");
		if (IFileManager::Get().FileExists(*Was)) { Clear(Path); IFileManager::Get().Move(*Path, *Was); }
	}

	FString Session(int32 Score, int32 Done)
	{
		FString Porches;
		for (int32 i = 0; i < Done; ++i) Porches += FString::Printf(TEXT("%s\"%s\": 131072"), i ? TEXT(", ") : TEXT(""), i == 0 ? TEXT("PORCH3D") : *FString::Printf(TEXT("PORCH%d"), i));
		return FString::Printf(TEXT("\"score\": %d, \"gold\": 1, \"kills\": 1, \"wounds\": 0, \"bounty\": 1000, \"porches\": { %s }"), Score, *Porches);
	}

	void PutFile(int32 Score, int32 Done)
	{
		Clear(FilePath());
		FFileHelper::SaveStringToFile(FString::Printf(TEXT("{ \"kind\": \"olue-hom\", \"difficulty\": 1, %s }"), *Session(Score, Done)), *FilePath());
	}

	void PutSlot(bool bSession, int32 Score, int32 Done)
	{
		Clear(SlotPath());
		const FString Held = bSession ? FString::Printf(TEXT(", \"hom_save\": { %s }"), *Session(Score, Done)) : FString();
		FFileHelper::SaveStringToFile(FString::Printf(TEXT("{ \"version\": 1, \"level\": \"office\", \"name\": \"Marshal's Office\", \"playtime\": 10, \"stamp\": \"2026-11-04T12:00:00.000Z\"%s }"), *Held), *SlotPath());
	}
}

OL_SHOT_MATCH(OLHomResumeProbe) { return OLFlags::Param(TEXT("OLHomResumeProbe")); }

OL_SHOT_TEST(OLHomResumeProbe, 2366)
{
	using namespace OutlawsHomResumeProbeLocal;
	if (!L || Stage < 0) return;
	const double Now = FPlatformTime::Seconds();
	const bool bOld = OLFlags::Param(TEXT("OLOldHomResume"));
	auto Finish = [&]()
	{
		L->bAutosave = bWasAutosave;
		L->StartSaveDelay = -1.f;
		PutBack(FilePath());
		PutBack(SlotPath());
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT homresume RESULT %s (%d passed, %d failed%s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails, bOld ? TEXT(", -OLOldHomResume") : TEXT(""));
		ShotTime = -1.f;
		Stage = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	if (Stage == 0)
	{
		L->StartSaveDelay = -1.f;       // (no autosaves until the office's)
		L->AutosaveWait = 0.f;
		if (L->IsRebuildPending() || L->IsFrontEnd() || ShotTime < 3.f) return;
		bWasAutosave = L->bAutosave;
		auto Page = [&](const TCHAR* What, bool bWant)
		{
			if (!bMenuOpen) OpenMenu();
			ShowMenuPage(TEXT("#missions"), false);
			const int32 Row = MenuPage ? MenuPage->Items.IndexOfByPredicate([](const OutlawsMenu::FItem& I) { return I.Name == TEXT("hom_resume"); }) : INDEX_NONE;
			const int32 Mt = MenuPage ? MenuPage->Items.IndexOfByPredicate([](const OutlawsMenu::FItem& I) { return I.Label == TEXT("Marshal Training"); }) : INDEX_NONE;
			const int32 Rows = MenuPage ? MenuPage->Items.Num() : 0;
			Check((Row != INDEX_NONE) == bWant && (Row == INDEX_NONE ? Mt == 0 : Row == 0 && Mt == 1), FString::Printf(TEXT("%s: the row %s (%d rows, Marshal Training row %d)"), What, Row != INDEX_NONE ? TEXT("on top") : TEXT("gone"), Rows, Mt));
		};
		Clear(FilePath());
		Clear(SlotPath());
		Page(TEXT("no session"), false);
		PutFile(100, 1);
		Page(TEXT("a session and no slot (v0.95)"), true);
		PutSlot(false, 0, 0);
		Page(TEXT("a session and a slot with none in it (v0.95's autosave, migrated)"), true);
		PutSlot(true, 100, 1);
		Page(TEXT("a slot holding the session"), bOld);
		PutFile(200, 2);
		Page(TEXT("a slot behind the session file"), true);
		PutSlot(true, 300, 3);
		Page(TEXT("a slot ahead of the file (a mission after it)"), bOld);
		// the v0.95 player's way back in: the row, with the Autosave option off
		PutFile(100, 1);
		Clear(SlotPath());
		CloseMenu();
		L->bAutosave = false;
		L->HomResume();
		StageAt = Now;
		Stage = 1;
		return;
	}
	if (Stage == 1)
	{
		const bool bUp = L->LevelName.Equals(TEXT("office"), ESearchCase::IgnoreCase) && !L->IsRebuildPending() && L->bHom;
		if (!bUp)
		{
			if (Now - StageAt < 90.0) return;
			Check(false, TEXT("the row's way in: the office up in 90 s"));
			Finish();
			return;
		}
		StageAt = Now;
		Stage = 2;
		return;
	}
	if (Stage == 2)
	{
		// (the level-start autosave: 1.5 s on the level's clock)
		if (Now - StageAt < 12.0) return;
		const AOutlawsLevelActor::FSaveInfo I = AOutlawsLevelActor::ReadSaveInfo(AOutlawsLevelActor::AutosaveHom);
		if (bOld)
		{
			Check(I.bEmpty, TEXT("old: the Autosave option off, no Historical autosave at the arrival"));
			Check(AOutlawsLevelActor::HomResumeOffered(), TEXT("old: the row stays"));
		}
		else
		{
			Check(!I.bEmpty && I.Level.Equals(TEXT("office"), ESearchCase::IgnoreCase), FString::Printf(TEXT("the arrival wrote the Historical autosave with the Autosave option off (%s)"), I.bEmpty ? TEXT("none") : *I.Level));
			Check(!AOutlawsLevelActor::HomResumeOffered(), TEXT("it holds the session, so the row is gone"));
		}
		Check(AOutlawsLevelActor::HasHomSave(), TEXT("the Marshal session still lives in its file"));
		Finish();
	}
}
#endif // WITH_OUTLAWS_TESTS
