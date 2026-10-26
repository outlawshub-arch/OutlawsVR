// -OLShots -OLLevel=hideout -OLNoFrontEnd -OLDeathSaveTest: BUG-DEATHAUTOSAVE (user 2026-10-20: after loading a save, dying
// before the next autosave sometimes loaded an autosave from another level).  A slot saved in the hideout, an autosave made in
// the town, the slot loaded: a death before any autosave goes back to the slot (the hideout as loaded), never the town's
// autosave; then the usual path (an autosave, a death, back to it); an autosave of another level never offered; and Restart
// Level after a load keeps the slot until the level start's autosave.  One of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLDeathSaveTest) { return OLFlags::Param(TEXT("OLDeathSaveTest")); }

OL_SHOT_TEST(OLDeathSaveTest, 1215)
{
	static int32 Step = 0, Passes = 0, Fails = 0;
	static double Until = 0.0, At = 0.0;
	constexpr int32 Slot = 6;
	if (!L || ShotTime < 3.f || Step < 0) return;
	const double Now = FPlatformTime::Seconds();
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT deathsave check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	// the marker: the bullets carried (what was saved comes back with the save)
	auto SetMark = [L](int32 N) { L->TakeCount(TEXT("ibullit"), L->GetCount(TEXT("ibullit"))); if (N > 0) L->AddCount(TEXT("ibullit"), N); };
	auto Mark = [L]() { return L->GetCount(TEXT("ibullit")); };
	auto Ready = [L](const TCHAR* Lv) { return L->LevelName.Equals(Lv, ESearchCase::IgnoreCase) && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsPlayerDead(); };
	auto Fail = [&](const FString& What) { Check(false, What); Step = 99; };
	auto Wait = [&](double S, int32 Next) { Until = Now + S; Step = Next; };
	switch (Step)
	{
	case 0:
		if (!Ready(TEXT("hideout"))) return;
		SetMark(31);
		Check(L->SaveGame(Slot), FString::Printf(TEXT("slot %d saved in the hideout with 31 bullets"), Slot));
		L->JumpToLevel(TEXT("town"), true);
		Wait(90.0, 1);
		return;
	case 1:
		// the town's own autosave (its level start's; made by hand if danger keeps it waiting)
		if (!Ready(TEXT("town"))) { if (Now > Until) Fail(TEXT("the town was not reached")); return; }
		if (!AOutlawsLevelActor::ReadSaveInfo(-1).Level.Equals(TEXT("town"), ESearchCase::IgnoreCase))
		{
			if (At == 0.0) At = Now + 10.0;
			if (Now > At) L->SaveGame(-1);
			return;
		}
		Check(true, TEXT("an autosave of the town written"));
		Check(L->LoadGame(Slot), FString::Printf(TEXT("slot %d (the hideout) loading from the town"), Slot));
		Wait(90.0, 2);
		return;
	case 2:
		if (!Ready(TEXT("hideout")) || Mark() != 31) { if (Now > Until) Fail(TEXT("the slot never came in")); return; }
		{
			int32 S = -9;
			const bool bOk = L->DeathSave(S);
			Check(bOk && S == Slot, FString::Printf(TEXT("after the load, a death goes back to slot %d (got %d, offered %d)"), Slot, S, bOk ? 1 : 0));
		}
		SetMark(3);
		L->ShotOwnHead();               // (a death past any cheat or badge)
		Wait(3.0, 3);
		return;
	case 3:
		if (Now < Until) return;
		Check(L->IsPlayerDead() && bDeathAutosave && DeathSaveSlot == Slot, FString::Printf(TEXT("dead before any autosave: B / Y offers slot %d (dead %d, offered %d, slot %d)"),
			Slot, L->IsPlayerDead() ? 1 : 0, bDeathAutosave ? 1 : 0, DeathSaveSlot));
		RequestRespawn(true);
		Wait(90.0, 4);
		return;
	case 4:
		if (Now > Until) { Fail(TEXT("the respawn never came in")); return; }
		if (L->IsRebuildPending() || L->IsPlayerDead() || (Mark() != 31 && Now < Until - 60.0)) return;      // (the save read in a moment after the rebuild)
		Check(L->LevelName.Equals(TEXT("hideout"), ESearchCase::IgnoreCase) && Mark() == 31,
			FString::Printf(TEXT("respawned in %s with %d bullets (wanted the hideout as loaded, 31; never the town's autosave)"), *L->LevelName, Mark()));
		// the usual path: an autosave here, a death, back to it
		SetMark(17);
		Check(L->SaveGame(-1), TEXT("an autosave in the hideout with 17 bullets"));
		{
			int32 S = -9;
			const bool bOk = L->DeathSave(S);
			Check(bOk && S == -1, FString::Printf(TEXT("after the autosave, a death goes back to it (got %d, offered %d)"), S, bOk ? 1 : 0));
		}
		SetMark(4);
		L->ShotOwnHead();               // (a death past any cheat or badge)
		Wait(3.0, 5);
		return;
	case 5:
		if (Now < Until) return;
		Check(bDeathAutosave && DeathSaveSlot == -1, FString::Printf(TEXT("dead: B / Y offers the autosave (offered %d, slot %d)"), bDeathAutosave ? 1 : 0, DeathSaveSlot));
		RequestRespawn(true);
		Wait(90.0, 6);
		return;
	case 6:
		if (Now > Until) { Fail(TEXT("the respawn never came in")); return; }
		if (L->IsRebuildPending() || L->IsPlayerDead() || (Mark() != 17 && Now < Until - 60.0)) return;
		Check(L->LevelName.Equals(TEXT("hideout"), ESearchCase::IgnoreCase) && Mark() == 17, FString::Printf(TEXT("respawned from the autosave with %d bullets (wanted 17)"), Mark()));
		{
			// an autosave of another level (as a level start before its own autosave, or the office's last mission): never offered
			FString Text;
			TSharedPtr<FJsonObject> O;
			const FString Path = AOutlawsLevelActor::SavePath(-1);
			if (FFileHelper::LoadFileToString(Text, *Path) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), O) && O.IsValid())
			{
				O->SetStringField(TEXT("level"), TEXT("town"));
				FString Out;
				FJsonSerializer::Serialize(O.ToSharedRef(), TJsonWriterFactory<>::Create(&Out));
				FFileHelper::SaveStringToFile(Out, *Path);
			}
			int32 S = -9;
			const bool bOk = L->DeathSave(S);
			Check(!bOk, FString::Printf(TEXT("an autosave of the town is not offered in the hideout (offered %d, slot %d)"), bOk ? 1 : 0, S));
		}
		L->ShotOwnHead();               // (a death past any cheat or badge)
		Wait(3.0, 7);
		return;
	case 7:
		if (Now < Until) return;
		Check(L->IsPlayerDead() && !bDeathAutosave, FString::Printf(TEXT("dead: only Restart Level offered (offered %d)"), bDeathAutosave ? 1 : 0));
		RequestRespawn(true);          // (B / Y: nothing)
		At = Now + 2.0;
		Step = 8;
		return;
	case 8:
		if (Now < At) return;
		Check(L->IsPlayerDead() && L->LevelName.Equals(TEXT("hideout"), ESearchCase::IgnoreCase) && !L->IsRebuildPending(), TEXT("B / Y did nothing: still dead in the hideout"));
		RequestRespawn(false);
		Wait(90.0, 9);
		return;
	case 9:
		// Restart Level after a load: the slot until the level start's autosave, then that autosave
		if (!Ready(TEXT("hideout"))) { if (Now > Until) Fail(TEXT("the restart never came in")); return; }
		Check(L->LoadGame(Slot), TEXT("slot loaded again"));
		Wait(90.0, 10);
		return;
	case 10:
		if (!Ready(TEXT("hideout")) || Mark() != 31) { if (Now > Until) Fail(TEXT("the second load never came in")); return; }
		L->RestartLevel();
		Wait(90.0, 11);
		return;
	case 11:
		if (!Ready(TEXT("hideout")) || L->StartSaveDelay < 0.f) { if (Now > Until) Fail(TEXT("the Restart Level never came in")); return; }
		{
			int32 S = -9;
			const bool bOk = L->DeathSave(S);
			Check(bOk && S == Slot, FString::Printf(TEXT("restarted after the load, before its autosave: slot %d offered (got %d, offered %d)"), Slot, S, bOk ? 1 : 0));
		}
		Wait(30.0, 12);
		return;
	case 12:
		if (L->StartSaveDelay >= 0.f || !AOutlawsLevelActor::ReadSaveInfo(-1).Level.Equals(TEXT("hideout"), ESearchCase::IgnoreCase))
		{
			if (Now > Until) Fail(TEXT("the level start's autosave never came"));
			return;
		}
		{
			int32 S = -9;
			const bool bOk = L->DeathSave(S);
			Check(bOk && S == -1, FString::Printf(TEXT("after the level start's autosave: the autosave offered (got %d, offered %d)"), S, bOk ? 1 : 0));
		}
		Step = 99;
		return;
	case 99:
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT deathsave RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		Step = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
