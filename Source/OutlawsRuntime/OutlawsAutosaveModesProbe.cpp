// REQ-AUTOSAVESLOTS (user 2 Nov: one single-player autosave a mode, so story, Historical Missions, A Handful of Missions
// and custom maps "don't overwrite each other"): -OLShots -OLLevel=hideout -OLNoFrontEnd -OLAutosaveModesProbe
// [-OLOldAutosave].  (a) the classifier, level by level; (b) an autosave in the story (hideout), then the Handful (villa),
// Historical Missions (shfort, a Marshal session on) and a custom map (town_2p): each mode's file holds its level, the
// others untouched, the death offer and the Beacon's slot this mode's; (c) the load page: the newest the resume point,
// the other modes by name, newest first, then co-op's, then the slots; (d) the Historical Missions autosave loaded from the
// custom map: the Marshal session back, the mission's counts as saved; then the story's: the session ends; (e) the
// migration of an old autosave.json of the Handful's villa: moved to its file when that file is absent (the same bytes),
// left alone when it is there.  -OLOldAutosave: every mode in autosave.json, no migration.  Lines "OLSHOT autosavemodes
// check PASS|FAIL: ..." and "OLSHOT autosavemodes RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "HAL/FileManager.h"
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
	const int32 ModeSlots[] = { ALA::AutosaveStory, ALA::AutosaveHandful, ALA::AutosaveHom, ALA::AutosaveCustom };       // (in (b)'s order)

	FString FileMd5(const FString& Path)
	{
		const FMD5Hash H = FMD5Hash::HashFile(*Path);
		return H.IsValid() ? LexToString(H) : FString(TEXT("absent"));
	}

	TSharedPtr<FJsonObject> ReadSaveFile(int32 Slot)
	{
		FString Text;
		TSharedPtr<FJsonObject> O;
		if (!FFileHelper::LoadFileToString(Text, *ALA::SavePath(Slot)) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), O)) return nullptr;
		return O;
	}
}

OL_SHOT_MATCH(OLAutosaveModesProbe) { return OLFlags::Param(TEXT("OLAutosaveModesProbe")); }

OL_SHOT_TEST(OLAutosaveModesProbe, 1500)
{
	static int32 Passes = 0, Fails = 0;
	static float Wait = 0.f;
	static int32 Mode = 0;                     // (b)'s level: 0 hideout, 1 villa, 2 shfort, 3 town_2p
	static TMap<int32, FString> Before;        // each mode file's md5 before this mode's save
	static FString WantLevel;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT autosavemodes check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const bool bOld = OLFlags::Param(TEXT("OLOldAutosave"));
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView && !L->PendingLoad.IsValid();
	static const TCHAR* Levels[] = { TEXT("hideout"), TEXT("villa"), TEXT("shfort"), TEXT("town_2p") };
	auto Snapshot = []() { Before.Reset(); for (int32 s : ModeSlots) Before.Add(s, FileMd5(ALA::SavePath(s))); };
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT autosavemodes RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 4.f || !bUp) return;
		L->bInvincible = true;
		// (a) the classifier
		struct FCase { const TCHAR* Level; int32 Slot; };
		const FCase Kinds[] = { { TEXT("hideout"), ALA::AutosaveStory }, { TEXT("townex"), ALA::AutosaveStory }, { TEXT("town"), ALA::AutosaveStory }, { TEXT("ranch"), ALA::AutosaveStory },
			{ TEXT("office"), ALA::AutosaveHom }, { TEXT("shfort"), ALA::AutosaveHom }, { TEXT("shwildns"), ALA::AutosaveHom }, { TEXT("gallery"), ALA::AutosaveHom },
			{ TEXT("shoot"), ALA::AutosaveHom }, { TEXT("indy"), ALA::AutosaveHom }, { TEXT("civlwar1"), ALA::AutosaveHandful }, { TEXT("civlwar2"), ALA::AutosaveHandful },
			{ TEXT("icecaves"), ALA::AutosaveHandful }, { TEXT("villa"), ALA::AutosaveHandful }, { TEXT("wharf"), ALA::AutosaveHandful }, { TEXT("town_2p"), ALA::AutosaveCustom },
			{ TEXT("granary"), ALA::AutosaveCustom }, { TEXT("SHFORT.LVT"), ALA::AutosaveHom } };
		FString Bad;
		for (const FCase& C : Kinds)
		{
			const int32 Want = bOld ? int32(ALA::AutosaveStory) : C.Slot;
			const int32 Got = ALA::AutosaveSlotOf(C.Level);
			if (Got != Want || !ALA::IsAutosaveSlot(Got)) Bad += FString::Printf(TEXT(" %s=%d (want %d)"), C.Level, Got, Want);
		}
		Check(Bad.IsEmpty(), FString::Printf(TEXT("(a) %d levels classified%s (and each a single-player autosave slot, which co-op's MaySave turns to -2)%s"), int32(UE_ARRAY_COUNT(Kinds)),
			bOld ? TEXT(" all to autosave.json (-OLOldAutosave)") : TEXT(""), *Bad));
		Mode = 0;
		Snapshot();
		Wait = ShotTime + 3.f;
		ShotStep = 2;
		return;
	}
	case 1:
		// the next mode's level, from its start
		if (!bUp || !L->LevelName.Equals(Levels[Mode], ESearchCase::IgnoreCase)) return;
		Wait = ShotTime + 3.f;
		ShotStep = 2;
		return;
	case 2:
	{
		if (ShotTime < Wait) return;
		const int32 Want = bOld ? int32(ALA::AutosaveStory) : ModeSlots[Mode];
		if (Mode == 2)
		{
			// a Marshal session on, as HomRide leaves it, with counts of its own
			L->bHom = true;
			L->HomMission = TEXT("shfort");
			L->HomReturnSector = TEXT("PORCH1D");
			L->HomGameScore = 4321;
			L->HomPorchEvents.Reset();
			L->HomPorchEvents.Add(TEXT("PORCH3D"), 131072);
			L->HomWounds = 3;
			L->HomGoldAtStart = 7;
			L->HomKillsAtStart = 2;
		}
		const bool bSaved = L->SaveGame(-1);
		const FString Lv = ALA::ReadSaveInfo(Want).Level;
		Check(bSaved && Lv.Equals(Levels[Mode], ESearchCase::IgnoreCase), FString::Printf(TEXT("(b) %s: the autosave written to %s, holding %s"), Levels[Mode], *FPaths::GetCleanFilename(ALA::SavePath(Want)), *Lv));
		FString Touched;
		for (int32 s : ModeSlots) if (s != Want && FileMd5(ALA::SavePath(s)) != Before.FindRef(s)) Touched += TEXT(" ") + FPaths::GetCleanFilename(ALA::SavePath(s));
		Check(Touched.IsEmpty(), FString::Printf(TEXT("(b) %s: the other modes' autosaves untouched%s"), Levels[Mode], Touched.IsEmpty() ? TEXT("") : *(TEXT(": CHANGED") + Touched)));
		int32 Ds = -9;
		const bool bOffer = L->DeathSave(Ds);
		Check(bOffer && Ds == -1 && L->AutosaveSlotOf(L->LevelName) == Want, FString::Printf(TEXT("(b) %s: a death offers this mode's autosave (offered %d, slot %d, loads %d)"), Levels[Mode], bOffer ? 1 : 0, Ds, L->AutosaveSlotOf(L->LevelName)));
		Check(L->ResolveAutosave(-1) == Want, FString::Printf(TEXT("(b) %s: the Beacon's Leave/Accept autosave and HostFromSave's slot: %d"), Levels[Mode], L->ResolveAutosave(-1)));
		if (Mode == 2 && !bOld)
		{
			const TSharedPtr<FJsonObject> O = ReadSaveFile(Want);
			const TSharedPtr<FJsonObject>* H = nullptr;
			const bool bHas = O.IsValid() && O->TryGetObjectField(TEXT("hom_save"), H) && H && H->IsValid();
			Check(bHas && (*H)->GetStringField(TEXT("mission")) == TEXT("shfort") && int32((*H)->GetNumberField(TEXT("score"))) == 4321,
				FString::Printf(TEXT("(b) the Historical Missions autosave carries the Marshal session (%s)"), bHas ? *FString::Printf(TEXT("mission %s, score %d"), *(*H)->GetStringField(TEXT("mission")), int32((*H)->GetNumberField(TEXT("score")))) : TEXT("none")));
		}
		if (Mode < 3)
		{
			++Mode;
			Snapshot();
			L->StartMission(Levels[Mode], EOutlawsDifficulty::Good);
			ShotStep = 1;
			return;
		}
		// (c) the load page
		CoopOpenMenu();
		CoopShowPage(TEXT("load_game.rml"), true);
		Wait = ShotTime + 1.f;
		ShotStep = 3;
		return;
	}
	case 3:
	{
		if (ShotTime < Wait) return;
		TArray<int32> Slots;
		TArray<FString> Rows;
		if (MenuPage.IsValid())
		{
			for (const OutlawsMenu::FItem& I : MenuPage->Items)
			{
				Slots.Add(I.Name.StartsWith(TEXT("load:")) ? FCString::Atoi(*I.Name.Mid(5)) : -99);
				Rows.Add(FString::Printf(TEXT("%s [%s]"), *I.Label, *I.Name));
			}
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT autosavemodes load page: %s"), *FString::Join(Rows, TEXT(" | ")));
		TArray<int32> Want;
		if (bOld) Want.Add(ALA::AutosaveStory);
		else Want = { ALA::AutosaveCustom, ALA::AutosaveHom, ALA::AutosaveHandful, ALA::AutosaveStory };      // (newest first: saved in the reverse order)
		const int32 Co = ALA::ReadSaveInfo(-2).bEmpty ? 0 : 1;
		bool bOrder = Slots.Num() == Want.Num() + Co + ALA::NumSaveSlots;
		for (int32 k = 0; bOrder && k < Want.Num(); ++k) bOrder = Slots[k] == Want[k];
		if (bOrder && Co) bOrder = Slots[Want.Num()] == -2;
		for (int32 k = 0; bOrder && k < ALA::NumSaveSlots; ++k) bOrder = Slots[Want.Num() + Co + k] == k;
		Check(bOrder, FString::Printf(TEXT("(c) the load page: the modes' autosaves newest first, %sthen the %d slots"), Co ? TEXT("co-op's after them, ") : TEXT(""), int32(ALA::NumSaveSlots)));
		bool bLabels = MenuPage.IsValid() && MenuPage->Items.Num() >= Want.Num() && MenuPage->Items[0].Label.StartsWith(TEXT("Resume: "));
		static const TCHAR* Names[] = { TEXT("Story: "), TEXT("Historical: "), TEXT("Handful: "), TEXT("Custom: ") };
		for (int32 k = 1; bLabels && k < Want.Num(); ++k)
		{
			const int32 i = Want[k] == ALA::AutosaveStory ? 0 : Want[k] == ALA::AutosaveHom ? 1 : Want[k] == ALA::AutosaveHandful ? 2 : 3;
			bLabels = MenuPage->Items[k].Label.StartsWith(Names[i]);
		}
		Check(bLabels, TEXT("(c) the newest is \"Resume:\", the others named by their mode"));
		Check(ALA::NewestAutosave() == Want[0], FString::Printf(TEXT("(c) the title's Resume Game loads the newest (%d)"), ALA::NewestAutosave()));
		CloseMenu();
		if (bOld) { Mode = 9; Wait = ShotTime + 0.5f; ShotStep = 8; return; }
		// (d) the Historical Missions autosave from the custom map, the session gone first
		L->bHom = false;
		L->HomMission.Reset();
		L->HomGameScore = 0;
		L->HomWounds = L->HomGoldAtStart = L->HomKillsAtStart = 0;
		Check(L->LoadGame(ALA::AutosaveHom), TEXT("(d) loading the Historical Missions autosave"));
		WantLevel = TEXT("shfort");
		ShotStep = 4;
		return;
	}
	case 4:
	case 6:
		if (!bUp || !L->LevelName.Equals(WantLevel, ESearchCase::IgnoreCase)) return;
		Wait = ShotTime + 3.f;
		++ShotStep;
		return;
	case 5:
	{
		if (ShotTime < Wait) return;
		Check(L->bHom && L->HomMission == TEXT("shfort") && L->HomReturnSector == TEXT("PORCH1D") && L->HomGameScore == 4321 && L->HomPorchEvents.FindRef(TEXT("PORCH3D")) == 131072,
			FString::Printf(TEXT("(d) the Marshal session back (hom %d, mission %s, back to %s, score %d, porches %d)"), L->bHom ? 1 : 0, *L->HomMission, *L->HomReturnSector, L->HomGameScore, L->HomPorchEvents.Num()));
		Check(L->HomWounds == 3 && L->HomGoldAtStart == 7 && L->HomKillsAtStart == 2, FString::Printf(TEXT("(d) the mission's counts as saved (wounds %d, gold at start %d, kills at start %d)"),
			L->HomWounds, L->HomGoldAtStart, L->HomKillsAtStart));
		Check(L->LoadGame(ALA::AutosaveStory), TEXT("(d) loading the story's autosave from the mission"));
		WantLevel = TEXT("hideout");
		ShotStep = 6;
		return;
	}
	case 7:
	{
		if (ShotTime < Wait) return;
		Check(!L->bHom, FString::Printf(TEXT("(d) the story's autosave ends the Marshal session (hom %d)"), L->bHom ? 1 : 0));
		Wait = ShotTime + 0.5f;
		ShotStep = 8;
		return;
	}
	case 8:
	{
		if (ShotTime < Wait) return;
		// (e) the migration: autosave.json made an old one of the Handful's villa (its bytes those of the villa autosave), the
		// two files it touches put aside first (moved, never deleted; the run's Saved is put back after)
		const FString Dir = FPaths::GetPath(ALA::SavePath(-1));
		const FString Story = ALA::SavePath(ALA::AutosaveStory), Handful = ALA::SavePath(ALA::AutosaveHandful);
		const FString AsideStory = Dir / TEXT("probe_aside_autosave.json"), AsideHandful = Dir / TEXT("probe_aside_autosave_handful.json");
		IFileManager& F = IFileManager::Get();
		if (bOld)
		{
			// (autosave.json holds town_2p, the last level saved)
			const FString Md5 = FileMd5(Story);
			ALA::MigrateAutosave(true);
			Check(FileMd5(Story) == Md5 && Md5 != TEXT("absent") && FileMd5(Handful) == Before.FindRef(ALA::AutosaveHandful),
				FString::Printf(TEXT("(e) -OLOldAutosave: no migration, autosave.json (%s) left where it is"), *ALA::ReadSaveInfo(ALA::AutosaveStory).Level));
			Finish();
			return;
		}
		const bool bAside = F.Move(*AsideStory, *Story) && F.Move(*AsideHandful, *Handful) && F.Copy(*Story, *AsideHandful) == COPY_OK;
		const FString Md5 = FileMd5(Story);
		Check(bAside && ALA::ReadSaveInfo(ALA::AutosaveStory).Level.Equals(TEXT("villa"), ESearchCase::IgnoreCase) && FileMd5(Handful) == TEXT("absent"),
			TEXT("(e) an old autosave.json of villa made, autosave_handful.json put aside"));
		ALA::MigrateAutosave(true);
		Check(FileMd5(Story) == TEXT("absent") && FileMd5(Handful) == Md5 && ALA::ReadSaveInfo(ALA::AutosaveHandful).Level.Equals(TEXT("villa"), ESearchCase::IgnoreCase),
			FString::Printf(TEXT("(e) the old autosave.json of villa moved to autosave_handful.json, its bytes the same (%s; autosave.json %s)"), *FileMd5(Handful), *FileMd5(Story)));
		// again, with the Handful's file there: both left as they are
		F.Copy(*Story, *AsideHandful);
		const FString StoryMd5 = FileMd5(Story), HandfulMd5 = FileMd5(Handful);
		ALA::MigrateAutosave(true);
		Check(FileMd5(Story) == StoryMd5 && StoryMd5 != TEXT("absent") && FileMd5(Handful) == HandfulMd5,
			FString::Printf(TEXT("(e) with autosave_handful.json there, autosave.json is left alone (%s) and the Handful's untouched"), *FileMd5(Story)));
		Finish();
		return;
	}
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
