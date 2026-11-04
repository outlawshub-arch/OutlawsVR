// REQ-COOPSAVES (user 4 Nov): the probes of the co-op saves.
// -OLShots -OLNoFrontEnd -OLLevel=hideout -OLCoopSavesProbe (2448): single player makes a save with a key in its pockets (slot 7)
// for the runs below, and says how a co-op level list sorts.
// -OLCoopSavesMenu=<slot> [-OLOldCoopSaves]: from the title menu, New Game, Multiplayer, Co-op: each page's rows and fit, Story and
// Historical Missions and their level lists, Load Game, the row of <slot> pressed, the lobby's Level row, Start, alone; then
// the save is up and an autosave goes to its mode's file.
// -OLCoopAuto -OLCoopTest=coopsaves -OLCoopLevel=<level> on a host and its client: the start autosave in the mode's file with the
// client's block, no other autosave touched, a hand save, then the client's every way of saving refused and its own files
// unchanged; then the level's end: the story's opening of the next level (its mode's file), a Handful mission's "the game
// is won", or Marshal Training's ride to the Fort and back to the office with the session in the save.
// The same with -OLCoopSavesCase=load -OLCoopLoad=<slot>: the save up on both, what each player holds said for the runner, and
// the next autosave in its mode's file.  Lines "OLCOOP coopsaves check PASS|FAIL (host|client): ...", "... RESULT".
#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_OUTLAWS_TESTS
namespace
{
	using ALA = AOutlawsLevelActor;

	const int32 CsAutos[] = { ALA::AutosaveStory, -2, ALA::AutosaveHom, ALA::AutosaveHandful, ALA::AutosaveCustom };

	FString CsMd5(int32 Slot)
	{
		const FString Path = ALA::SavePath(Slot);
		return FPaths::FileExists(Path) ? LexToString(FMD5Hash::HashFile(*Path)) : FString(TEXT("none"));
	}

	FString CsAllFiles()
	{
		FString H;
		for (int32 a : CsAutos) H += CsMd5(a) + TEXT(" ");
		for (int32 s = 0; s < ALA::NumSaveSlots; ++s) H += CsMd5(s) + TEXT(" ");
		return H;
	}

	TSharedPtr<FJsonObject> CsRead(int32 Slot)
	{
		FString Text;
		TSharedPtr<FJsonObject> O;
		if (!FFileHelper::LoadFileToString(Text, *ALA::SavePath(Slot)) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), O)) return nullptr;
		return O;
	}

	/** The autosave the test wants for a level: its mode's, -OLOldCoopSaves R7's own file. */
	int32 CsWant(const FString& Level) { return UOutlawsCoop::OldCoopSaves() ? -2 : ALA::AutosaveSlotOf(Level); }
}

FString UOutlawsCoop::CsState(const AOutlawsLevelActor* L) const
{
	return FString::Printf(TEXT("level %s, difficulty %d, gold %d, marshal %d (mission %s, back to %s, score %d, %d outlaws), kit %s"), *L->LevelName, int32(L->Difficulty), L->GetGold(),
		L->bHom ? 1 : 0, L->HomMission.IsEmpty() ? TEXT("none") : *L->HomMission, L->HomReturnSector.IsEmpty() ? TEXT("none") : *L->HomReturnSector, L->HomGameScore, L->HomPorchEvents.Num(),
		*L->CarrySignature());
}

FString UOutlawsCoop::CsGiveKit(AOutlawsLevelActor* L)
{
	FString Key;
	for (const TPair<FString, AOutlawsLevelActor::FItemDef>& D : L->ItemDefs)
	{
		if (D.Value.Class.ToLower().Contains(TEXT("key"))) { Key = D.Key; break; }
	}
	L->AddCount(TEXT("idocbag"), 2);
	L->Gold += 37;
	if (!Key.IsEmpty()) L->AddCount(Key, 1);
	return Key;
}

void UOutlawsCoop::CsCheck(bool bOk, const FString& What)
{
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP coopsaves check %s (%s): %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), IsHost() ? TEXT("host") : TEXT("client"), *What);
	if (bOk) ++CsvPasses; else ++CsvFails;
}

void UOutlawsCoop::CsResult()
{
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP coopsaves %s RESULT %s (%d passed, %d failed)"), IsHost() ? TEXT("host") : TEXT("client"), CsvFails == 0 && CsvPasses > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), CsvPasses, CsvFails);
}

void UOutlawsCoop::CsCheckAutosave(int32 Want, const FString& Level, bool bOpening)
{
	// the file wanted holds this level, every other autosave as it was when the run began
	const TSharedPtr<FJsonObject> O = CsRead(Want);
	const FString Saved = O.IsValid() ? O->GetStringField(TEXT("level")) : FString(TEXT("none"));
	bool bOpen = false;
	if (O.IsValid()) O->TryGetBoolField(TEXT("opening"), bOpen);
	CsCheck(Saved.Equals(Level, ESearchCase::IgnoreCase) && bOpen == bOpening, FString::Printf(TEXT("%s holds %s%s (want %s)"), *FPaths::GetCleanFilename(ALA::SavePath(Want)), *Saved, bOpen ? TEXT(", its opening") : TEXT(""), *Level));
	FString Moved;
	for (int32 a : CsAutos)
	{
		if (a != Want && CsMd5(a) != CsBefore.FindRef(a)) Moved += TEXT(" ") + FPaths::GetCleanFilename(ALA::SavePath(a));
	}
	CsCheck(Moved.IsEmpty(), FString::Printf(TEXT("no other autosave touched%s"), *Moved));
}

void UOutlawsCoop::TickCoopSavesTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	const bool bHost = IsHost();
	static const FString Case = []() { FString V; OLFlags::Value(TEXT("OLCoopSavesCase"), V); return V; }();
	const bool bLoad = Case == TEXT("load");
	const AOutlawsCoopSession* S = GetSession();
	auto Quit = [&]() { CsResult(); CsvAt = Now + 3.0; CsvStep = 90; };
	auto PartnerIn = [&]() { const AOutlawsCoopLink* CL = GetLink(PartnerIndex()); return CL && CL->BuiltSerial == StartedSerial; };
	// (a level's end may hold on a movie an offscreen test does not finish)
	auto SkipMovie = [&]() { if (L->IsMoviePlaying() && L->CanSkipMovie()) L->SkipMovie(); };
	switch (CsvStep)
	{
	case 0:
		for (int32 a : CsAutos) CsBefore.Add(a, CsMd5(a));
		CsAll = CsAllFiles();
		CsvStep = 1;
		return;
	case 1:
		if (!S || !bLevelBuilt || L->bNeedPlace || L->IsRebuildPending() || bHostLoadPending || SnapObj.IsValid() || (bHost && !PartnerIn())) return;
		L->bInvincible = true;
		CsCheck(L->bHom == S->bHom, FString::Printf(TEXT("the Marshal session as the host's: %d, session %d (%s)"), L->bHom ? 1 : 0, S->bHom ? 1 : 0, *L->LevelName));
		if (!bHost) { CsvStep = 40; return; }
		CsvAt = Now + (bLoad ? 6.0 : 45.0);
		CsvStep = bLoad ? 30 : 2;
		return;
	case 2:
	{
		// the start autosave, once the partner's kit is in (HoldStartAutosave)
		const int32 Want = CsWant(L->LevelName);
		const bool bWritten = CsMd5(Want) != CsBefore.FindRef(Want);
		if (!bWritten && Now < CsvAt) return;
		if (!bWritten)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP coopsaves: no start autosave in 45 s (danger: %s, autosave option %d): saved by the test"), *L->AutosaveDanger(), L->bAutosave ? 1 : 0);
			L->SaveGame(-1);
		}
		CsCheckAutosave(Want, L->LevelName, false);
		const TSharedPtr<FJsonObject> O = CsRead(Want);
		CsCheck(O.IsValid() && (O->HasField(TEXT("player2")) || O->HasField(TEXT("players"))), TEXT("the autosave carries the client's block"));
		CsCheck(O.IsValid() && O->HasField(TEXT("hom_save")) == (L->bHom && !OldCoopSaves()), FString::Printf(TEXT("the autosave's Marshal session: %d"), O.IsValid() && O->HasField(TEXT("hom_save")) ? 1 : 0));
		CsvAt = Now + 1.0;
		CsvStep = 3;
		return;
	}
	case 3:
	{
		if (Now < CsvAt) return;
		if (L->bHom)
		{
			// (a session with something in it, for the run that loads this save: both players' score and outlaws)
			L->HomGameScore = 1500;
			L->HomPorchEvents.Add(TEXT("PORCH1D"), 131072);
		}
		CsCheck(L->SaveGame(9), TEXT("the host's hand save into slot 9"));
		const TSharedPtr<FJsonObject> O = CsRead(9);
		CsCheck(O.IsValid() && O->HasField(TEXT("hom_save")) == (L->bHom && !OldCoopSaves()), FString::Printf(TEXT("the hand save's Marshal session: %d"), O.IsValid() && O->HasField(TEXT("hom_save")) ? 1 : 0));
		TestSay(TEXT("cs:try"));
		CsvAt = Now + 3.0;
		CsvStep = 4;
		return;
	}
	case 4:
		if (Now < CsvAt) return;
		if (L->LevelName.Equals(TEXT("office"), ESearchCase::IgnoreCase))
		{
			L->HomSpawnLevel(TEXT("SHFORT PORCH1D"));
			if (P->IsMenuOpen()) L->HomRide();       // (the ride-out page's Ride)
			CsvAt = Now + 90.0;
			CsvStep = 10;
		}
		else
		{
			// the level's end: the next level's opening in its mode's file, or (a Handful mission's last level) no level follows
			// and the session ends, HoldNextLevel quitting the auto test on "the game is won"
			L->CompleteLevel(TEXT("the co-op saves test"));
			CsNext = L->NextLevelName;
			CsvAt = Now + (CsNext.IsEmpty() ? 60.0 : 3.0);
			CsvStep = CsNext.IsEmpty() ? 50 : 20;
			if (CsNext.IsEmpty()) CsResult();
		}
		return;
	case 10:
	{
		// Marshal Training: the ride to the Fort; its start autosave in the Historical file with the session
		const bool bThere = L->LevelName.Equals(TEXT("shfort"), ESearchCase::IgnoreCase) && bLevelBuilt && !L->bNeedPlace && !L->IsRebuildPending();
		const int32 Want = CsWant(TEXT("shfort"));
		const TSharedPtr<FJsonObject> O = CsRead(Want);
		const bool bWritten = O.IsValid() && O->GetStringField(TEXT("level")).Equals(TEXT("shfort"), ESearchCase::IgnoreCase);
		if ((!bThere || !bWritten || !PartnerIn()) && Now < CsvAt) return;
		CsCheck(bThere, FString::Printf(TEXT("the ride reached the Fort (%s)"), *L->LevelName));
		CsCheck(bWritten, TEXT("the mission's start autosave is the Fort's"));
		const TSharedPtr<FJsonObject>* H = nullptr;
		const bool bSess = O.IsValid() && O->TryGetObjectField(TEXT("hom_save"), H) && H && H->IsValid() && (*H)->GetStringField(TEXT("mission")).Equals(TEXT("shfort"), ESearchCase::IgnoreCase);
		CsCheck(bSess == !OldCoopSaves(), TEXT("it carries the session, this mission's"));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP coopsaves state (host): %s"), *CsState(L));
		TestSay(TEXT("cs:report"));
		L->CompleteLevel(TEXT("the co-op saves test"));
		CsvAt = Now + 90.0;
		CsvStep = 11;
		return;
	}
	case 11:
		SkipMovie();
		if ((!L->LevelName.Equals(TEXT("office"), ESearchCase::IgnoreCase) || !bLevelBuilt || L->bNeedPlace || L->IsRebuildPending() || !PartnerIn()) && Now < CsvAt) return;
		CsCheck(L->LevelName.Equals(TEXT("office"), ESearchCase::IgnoreCase) && L->bHom, FString::Printf(TEXT("back in the office with the session (%s, marshal %d)"), *L->LevelName, L->bHom ? 1 : 0));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP coopsaves state (host): %s"), *CsState(L));
		TestSay(TEXT("cs:report"));
		Quit();
		return;
	case 20:
		if (Now < CsvAt) return;
		CsCheckAutosave(CsWant(CsNext), CsNext, true);
		Quit();
		return;
	case 30:
	{
		// (-OLCoopLoad) the save is up on both: what each player holds, for the runner; the next autosave in its mode's file
		if (Now < CsvAt) return;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP coopsaves state (host): %s"), *CsState(L));
		TestSay(TEXT("cs:report"));
		const int32 Want = CsWant(L->LevelName);
		CsCheck(L->SaveGame(-1), TEXT("an autosave after the load"));
		CsCheckAutosave(Want, L->LevelName, false);
		CsvAt = Now + 2.0;
		CsvStep = 31;
		return;
	}
	case 31:
		if (Now < CsvAt) return;
		TestSay(TEXT("cs:try"));
		CsvAt = Now + 3.0;
		CsvStep = 32;
		return;
	case 32:
		if (Now < CsvAt) return;
		Quit();
		return;
	case 50:
		// (a Handful mission: HoldNextLevel quits the auto test on "the game is won")
		SkipMovie();
		if (Now < CsvAt) return;
		CsCheck(false, TEXT("the mission's end did not end the session in 60 s"));
		Quit();
		return;
	case 90:
		if (Now < CsvAt) return;
		TestSay(TEXT("quit"), 0);
		CsvAt = Now + 2.0;
		CsvStep = 91;
		return;
	case 91:
		if (Now < CsvAt) return;
		Summary(TEXT("done"));
		QuitForTest();
		CsvStep = 92;
		return;
	default:
		return;
	}
}

void UOutlawsCoop::OnCoopSavesTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	if (Step == TEXT("try"))
	{
		// a joiner's every way of saving, and its own files as they were at the start
		CsCheck(!L->SaveGame(-1), TEXT("the client's autosave is refused"));
		CsCheck(!L->SaveGame(8), TEXT("the client's hand save is refused"));
		CsCheck(!L->SaveLevelOpening(), TEXT("the client's level opening is refused"));
		CsCheck(CsAllFiles() == CsAll, TEXT("every autosave and slot of the client's own unchanged"));
		CsResult();
	}
	else if (Step == TEXT("report"))
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP coopsaves state (client): %s"), *CsState(L));
	}
}

void UOutlawsCoop::TickCoopSavesMenu(double Now, AOutlawsPlayer* P)
{
	// -OLCoopSavesMenu=<slot>: the pages as a hand would go through them, one press a second
	if (Now < CsMenuAt || !P) return;
	CsMenuAt = Now + 1.0;
	static const int32 Pick = []() { FString V; return OLFlags::Value(TEXT("OLCoopSavesMenu"), V) ? FCString::Atoi(*V) : 0; }();
	auto Press = [&](const TCHAR* Label)
	{
		const bool bOk = P->TutorialTestPressLabel(Label);
		CsCheck(bOk, FString::Printf(TEXT("pressed %s (now on %s)"), Label, *P->CoopMenuPage()));
	};
	auto Fits = [&](const TCHAR* What)
	{
		FString Fit;
		const bool bFits = PageFits(P, Fit);
		CsCheck(bFits, FString::Printf(TEXT("%s fits its panel (%s); rows: %s"), What, *Fit, *P->TutorialTestRows()));
	};
	auto RowsAre = [&](const TCHAR* Page, const TCHAR* Rows)
	{
		CsCheck(P->CoopMenuPage() == Page && P->TutorialTestRows() == Rows, FString::Printf(TEXT("%s: %s (want %s: %s)"), *P->CoopMenuPage(), *P->TutorialTestRows(), Page, Rows));
	};
	auto Finish = [&]()
	{
		CsResult();
		CsMenuStep = 100;
		FGenericPlatformMisc::RequestExit(false);
	};
	switch (CsMenuStep)
	{
	case 0:
		if (!P->IsFrontMenuUp()) return;
		for (int32 a : CsAutos) CsBefore.Add(a, CsMd5(a));
		Press(TEXT("New Game"));
		break;
	case 1: Press(TEXT("Multiplayer")); break;
	case 2: Press(TEXT("Co-op")); break;
	case 3:
		if (OldCoopSaves())
		{
			RowsAre(TEXT("#coop"), TEXT("Host Game | Join Game | Back"));
			Finish();
			return;
		}
		RowsAre(TEXT("#coop"), TEXT("New Game | Load Game | Join Game | Back"));
		Fits(TEXT("the Co-op page"));
		Press(TEXT("New Game"));
		break;
	case 4:
		RowsAre(TEXT("#coopcampaign"), TEXT("Story | Historical Missions | Back"));
		Press(TEXT("Story"));
		break;
	case 5:
		CsCheck(P->CoopMenuPage() == TEXT("#cooplevels") && !P->TutorialTestRows().Contains(TEXT("Campaign")) && !P->TutorialTestRows().Contains(TEXT("Continue")), TEXT("the Story levels: no Campaign or Continue row"));
		Fits(TEXT("the Story levels"));
		Press(TEXT("Back"));
		break;
	case 6: Press(TEXT("Historical Missions")); break;
	case 7:
		CsCheck(P->TutorialTestRows().StartsWith(TEXT("1. Marshal Training")), TEXT("the Historical Missions levels start with Marshal Training"));
		Fits(TEXT("the Historical Missions levels"));
		Press(TEXT("Back"));
		break;
	case 8: Press(TEXT("Back")); break;
	case 9:
		if (!AOutlawsPlayer::AnySaveToLoad())
		{
			Press(TEXT("Load Game"));
			CsCheck(P->CoopMenuPage() == TEXT("#coop"), TEXT("no saves: Load Game is greyed out, the page stays"));
			Finish();
			return;
		}
		Press(TEXT("Load Game"));
		break;
	case 10:
	{
		CsCheck(P->CoopMenuPage() == TEXT("load_game.rml"), TEXT("Load Game opens the normal Load page"));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP coopsaves: the Load page's rows: %s"), *P->TutorialTestRows());
		const AOutlawsLevelActor::FSaveInfo Old = ALA::ReadSaveInfo(-2);
		if (!Old.bEmpty) CsCheck(P->TutorialTestRows().Contains(FString::Printf(TEXT("Old co-op: %s"), *Old.Name)), FString::Printf(TEXT("an old co-op autosave is on the page under a clear name (Old co-op: %s)"), *Old.Name));
		CsMenuLevel = ALA::ReadSaveInfo(Pick).Level;
		CsCheck(P->CoopPressRow(FString::Printf(TEXT("load:%d"), Pick)), FString::Printf(TEXT("the row of %s pressed (%s)"), *FPaths::GetCleanFilename(ALA::SavePath(Pick)), *CsMenuLevel));
		break;
	}
	case 11:
	{
		// the session opens on it, the lobby's page up
		if (!IsHost() || P->CoopMenuPage() != TEXT("#coophost")) { if (++CsMenuWait > 40) { CsCheck(false, TEXT("no lobby in 40 s")); Finish(); } return; }
		CsCheck(P->TutorialTestRows().Contains(TEXT("LEVEL: LOAD ")), FString::Printf(TEXT("the lobby's Level row names the save: %s"), *P->TutorialTestRows()));
		Fits(TEXT("the lobby"));
		MenuAction(P, TEXT("coop:level"));
		break;
	}
	case 12:
		RowsAre(TEXT("#coopcampaign"), TEXT("Story | Historical Missions | Load Game | Back"));
		Press(TEXT("Back"));
		break;
	case 13:
		Press(TEXT("START GAME"));
		break;
	case 14:
	{
		const AOutlawsLevelActor* LvUp = FindLevel();
		if (!LvUp || !bLevelBuilt || !LvUp->LevelName.Equals(CsMenuLevel, ESearchCase::IgnoreCase) || LvUp->bNeedPlace || LvUp->IsRebuildPending() || bHostLoadPending)
		{
			if (++CsMenuWait > 100) { CsCheck(false, FString::Printf(TEXT("the save's level %s was not up in 100 s"), *CsMenuLevel)); Finish(); }
			return;
		}
		break;
	}
	case 15:
	{
		AOutlawsLevelActor* LvUp = FindLevel();
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP coopsaves state (host): %s"), *CsState(LvUp));
		CsCheck(LvUp->SaveGame(-1), TEXT("an autosave after the load"));
		CsCheckAutosave(CsWant(LvUp->LevelName), LvUp->LevelName, false);
		Finish();
		return;
	}
	default:
		return;
	}
	++CsMenuStep;
}

OL_SHOT_MATCH(OLCoopSavesProbe) { return OLFlags::Param(TEXT("OLCoopSavesProbe")); }

OL_SHOT_TEST(OLCoopSavesProbe, 2448)
{
	static int32 Passes = 0, Fails = 0;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT coopsaves check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	if (ShotTime < 4.f || L->IsFrontEnd() || L->IsRebuildPending() || !L->HasActorBegunPlay() || L->IsMoviePlaying()) return;
	// the lobby's campaign of each kind of level
	struct FKind { const TCHAR* Level; bool bHist; };
	for (const FKind& K : { FKind{ TEXT("hideout"), false }, FKind{ TEXT("office"), true }, FKind{ TEXT("icecaves"), true }, FKind{ TEXT("shfort"), true } })
	{
		Check(UOutlawsCoop::IsMissionLevel(K.Level) == K.bHist, FString::Printf(TEXT("%s is %s"), K.Level, K.bHist ? TEXT("Historical Missions") : TEXT("the story")));
	}
	Check(FCString::Strcmp(ALA::AutosaveModeName(-2), UOutlawsCoop::OldCoopSaves() ? TEXT("Co-op") : TEXT("Old co-op")) == 0, FString::Printf(TEXT("the old file's name on the Load page: %s"), ALA::AutosaveModeName(-2)));
	// a save for the runs that host it: a bag, gold and a key on top of the start kit
	const FString Key = UOutlawsCoop::CsGiveKit(L);
	Check(L->SaveGame(7), FString::Printf(TEXT("single player's save in slot 7 (level %s, gold %d, key %s, kit %s)"), *L->LevelName, L->GetGold(), *Key, *L->CarrySignature()));
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT coopsaves RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
	ShotStep = -1;
	FGenericPlatformMisc::RequestExit(false);
}
#endif // WITH_OUTLAWS_TESTS
