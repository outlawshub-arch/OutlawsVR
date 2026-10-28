// BUG-PLATECARRY (user 2026-10-28, snapshot 18: "the boiler plate breaks apart" as the hideout began, after End Game and a
// new game): -OLShots -OLLevel=hideout -OLNoFrontEnd -OLPlateCarryProbe wears a plate through every way its count is set
// without a hit, and checks that only a hit breaks it.  (a) a plate picked up in the hideout, the level restarted: gone, no
// message; (g) a plate broken by the hit that kills, the respawn: no message after it; (b) a dented plate (two hits) carried
// into the town: worn, as dented, its size kept; (c) dented further, the town restarted: the plate it was entered with, its
// marks those of its wear; (d) saves loaded over another plate: one with a plate, the same one written as a save from before
// 2026-10-01 (no armour_max) and one with none; (e) a hit bigger than the plate: the message, once; (f) a plate worn to End
// Game, then a new game from the main menu: the hideout with no plate, no size and no message.  The plate is shown on the
// desktop for the checks (bTestPlatePickup).  Lines "OLSHOT platecarry check PASS|FAIL: ..." and "OLSHOT platecarry RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "HAL/PlatformMisc.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLPlateCarryProbe) { return OLFlags::Param(TEXT("OLPlateCarryProbe")); }

OL_SHOT_TEST(OLPlateCarryProbe, 15)
{
	static int32 Passes = 0, Fails = 0, Shows = 0, ShowsSeen = 0, ShowsMark = 0;
	static float Wait = 0.f;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT platecarry check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	// every "The boiler plate breaks apart" put up, whenever (the line stays up 2 s: counted as it comes)
	int32 Up = 0;
	for (const FString& H : HudLines) Up += H.Contains(TEXT("breaks apart")) ? 1 : 0;
	if (Up > ShowsSeen) Shows += Up - ShowsSeen;
	ShowsSeen = Up;
	FVector At;
	FRotator Rot;
	const bool bShown = ChestPlateShown(At, Rot);
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView && ShotTime > Wait;
	auto State = [&]() { return FString::Printf(TEXT("armour %d of %d, shown %d, %d dents, health %.1f, 'breaks apart' shown %d times since the case began"), L->GetArmour(), L->ArmourMax,
		bShown ? 1 : 0, ChestDents(), L->GetHealth(), Shows - ShowsMark); };
	auto NewPlate = [&](int32 Count) { L->ArmourMax = 0; L->SetArmour(0); L->AddCount(TEXT("ibplate"), Count); };     // (as a pickup takes it)
	auto Hit = [&](float Damage) { L->bInvincible = false; L->DamagePlayer(Damage, 1.f, false); L->bInvincible = true; };
	auto WearDents = [&]() { const int32 Max = FMath::Max(1, FMath::Max(L->ArmourMax, L->GetArmour())); return FMath::Clamp(int32((1.0 - double(L->GetArmour()) / Max) * 10), 0, 10); };
	auto Next = [&](float Secs) { Wait = ShotTime + Secs; ++ShotStep; };
	auto Mark = [&]() { ShowsMark = Shows; };
	switch (ShotStep)
	{
	case 0:
		if (ShotTime < 3.f || !bUp) return;
		bTestPlatePickup = true;
		L->bInvincible = true;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT platecarry start in %s: %s"), *L->LevelName, *State());
		// (a) a plate picked up in the level, then the level restarted
		Mark();
		NewPlate(100);
		Next(1.f);
		return;
	case 1:
		if (!bUp) return;
		Check(L->GetArmour() == 100 && bShown, FString::Printf(TEXT("(a) a plate picked up in the hideout is worn: %s"), *State()));
		L->RestartLevel();
		Next(3.f);
		return;
	case 2:
		if (!bUp) return;
		Check(L->GetArmour() == 0 && !bShown && Shows == ShowsMark, FString::Printf(TEXT("(a) the hideout restarted: entered with none, no plate and no 'breaks apart': %s"), *State()));
		// (g) the plate broken by the hit that kills (the tracker does not run in the death view), then the respawn
		Mark();
		NewPlate(50);
		Next(0.5f);
		return;
	case 3:
		if (!bUp) return;
		Hit(20.f);                                 // (5 points on the plate, 15 on 10 health)
		Next(0.f);
		return;
	case 4:
		if (!bDeadView || DeathTime < 1.5f) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT platecarry (g) dead: %s"), *State());
		ShowsMark = Shows;                         // (the hit itself may have put the line up before the death view took over)
		RequestRespawn(false);
		Next(3.f);
		return;
	case 5:
		if (!bUp) return;
		Check(L->GetArmour() == 0 && !bShown && Shows == ShowsMark && L->GetHealth() >= 10.f, FString::Printf(TEXT("(g) the respawn after a death that broke the plate: no 'breaks apart' at the start: %s"), *State()));
		// (b) a medium plate dented by two hits, carried into the town as a won level carries it
		Mark();
		NewPlate(100);
		Next(0.5f);
		return;
	case 6:
	case 7:
		if (!bUp) return;
		Hit(1.5f);                                 // (15 points each, the plate takes all of it)
		Next(0.4f);
		return;
	case 8:
		if (!bUp) return;
		Check(L->GetArmour() == 70 && ChestDents() == 2 && bShown && Shows == ShowsMark, FString::Printf(TEXT("(b) two hits dent it twice: %s"), *State()));
		L->JumpToLevel(TEXT("town"), true);
		Next(3.f);
		return;
	case 9:
		if (!bUp || !L->LevelName.Equals(TEXT("town"), ESearchCase::IgnoreCase)) return;
		Check(L->GetArmour() == 70 && L->ArmourMax == 100 && bShown && ChestDents() == 2 && Shows == ShowsMark,
			FString::Printf(TEXT("(b) carried into the town: worn, the same size, as dented, no 'breaks apart': %s"), *State()));
		// (c) two more hits, then the town restarted: the plate it was entered with
		Mark();
		Next(0.2f);
		return;
	case 10:
	case 11:
		if (!bUp) return;
		Hit(1.5f);
		Next(0.4f);
		return;
	case 12:
		if (!bUp) return;
		Check(L->GetArmour() == 40 && ChestDents() == 4, FString::Printf(TEXT("(c) dented twice more: %s"), *State()));
		L->RestartLevel();
		Next(3.f);
		return;
	case 13:
		if (!bUp) return;
		Check(L->GetArmour() == 70 && bShown && ChestDents() == WearDents() && Shows == ShowsMark,
			FString::Printf(TEXT("(c) the town restarted: the plate it was entered with, worn, marked by its wear (%d dents), no 'breaks apart': %s"), WearDents(), *State()));
		// (d) a save with this plate; a heavy plate put on; the save loaded
		Mark();
		Check(L->SaveGame(7), TEXT("(d) saved to slot 7 with the plate"));
		NewPlate(150);
		Next(0.5f);
		return;
	case 14:
		if (!bUp) return;
		Check(L->GetArmour() == 150 && bShown, FString::Printf(TEXT("(d) a heavy plate on before the load: %s"), *State()));
		Check(L->LoadGame(7), TEXT("(d) loading slot 7"));
		Next(3.f);
		return;
	case 15:
	{
		if (!bUp || L->GetArmour() == 150) return;
		Check(L->GetArmour() == 70 && L->ArmourMax == 100 && bShown && ChestDents() == WearDents() && Shows == ShowsMark,
			FString::Printf(TEXT("(d) slot 7 loaded: its plate, worn, marked by its wear, no 'breaks apart' and no dent for the drop: %s"), *State()));
		// the same save as one written before 2026-10-01 (no armour_max: the load gives the smallest plate that holds it)
		FString Text;
		TSharedPtr<FJsonObject> O;
		const FString Dir = FPaths::ProjectSavedDir() / TEXT("OutlawsSaves");
		bool bOld = FFileHelper::LoadFileToString(Text, *(Dir / TEXT("slot7.json"))) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), O) && O.IsValid();
		if (bOld)
		{
			O->RemoveField(TEXT("armour_max"));
			FString Out;
			bOld = FJsonSerializer::Serialize(O.ToSharedRef(), TJsonWriterFactory<>::Create(&Out)) && FFileHelper::SaveStringToFile(Out, *(Dir / TEXT("slot9.json")));
		}
		Check(bOld, TEXT("(d) slot 9 written: slot 7 without armour_max"));
		Mark();
		NewPlate(150);
		Next(0.5f);
		return;
	}
	case 16:
		if (!bUp) return;
		Check(L->LoadGame(9), TEXT("(d) loading slot 9 (an old save with a plate)"));
		Next(3.f);
		return;
	case 17:
		if (!bUp || L->GetArmour() == 150) return;
		Check(L->GetArmour() == 70 && L->ArmourMax == 100 && bShown && Shows == ShowsMark,
			FString::Printf(TEXT("(d) the old save loaded: its plate, worn, no 'breaks apart': %s"), *State()));
		// a save with no plate; a plate put on; that save loaded
		L->SetArmour(0);
		L->ArmourMax = 0;
		Next(0.5f);
		return;
	case 18:
		if (!bUp) return;
		Mark();
		Check(L->SaveGame(8), FString::Printf(TEXT("(d) saved to slot 8 with no plate: %s"), *State()));
		NewPlate(100);
		Next(0.5f);
		return;
	case 19:
		if (!bUp) return;
		Check(L->LoadGame(8), TEXT("(d) loading slot 8 over a medium plate"));
		Next(3.f);
		return;
	case 20:
		if (!bUp || L->GetArmour() == 100) return;
		Check(L->GetArmour() == 0 && !bShown && Shows == ShowsMark, FString::Printf(TEXT("(d) slot 8 loaded: no plate, no 'breaks apart': %s"), *State()));
		// (e) a light plate and a hit bigger than it: it breaks, and says so once
		Mark();
		NewPlate(50);
		Next(0.5f);
		return;
	case 21:
		if (!bUp) return;
		Hit(8.f);
		Next(1.f);
		return;
	case 22:
		if (!bUp) return;
		Check(L->GetArmour() == 0 && !bShown && Shows - ShowsMark == 1, FString::Printf(TEXT("(e) a hit past the plate breaks it, 'breaks apart' once: %s"), *State()));
		// (f) a plate worn to End Game, then a new game from the main menu
		Mark();
		NewPlate(100);
		Next(0.5f);
		return;
	case 23:
		if (!bUp) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT platecarry (f) End Game wearing: %s"), *State());
		L->ReturnToFrontEnd();
		Next(2.f);
		return;
	case 24:
		if (!L->IsFrontEnd() || L->IsRebuildPending() || !bMenuOpen || ShotTime < Wait) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT platecarry (f) the main menu over %s: a new game"), *L->LevelName);
		CloseMenu();
		ClearScreenFx();
		L->StartStory(EOutlawsDifficulty::Good);
		Next(3.f);
		return;
	case 25:
		if (L->IsMoviePlaying()) L->SkipMovie();
		if (!bUp || !L->LevelName.Equals(TEXT("hideout"), ESearchCase::IgnoreCase)) return;
		Check(L->GetArmour() == 0 && L->ArmourMax == 0 && !bShown && Shows == ShowsMark,
			FString::Printf(TEXT("(f) the new game's hideout: no plate, no size and no 'breaks apart': %s"), *State()));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT platecarry RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		Next(1.f);
		return;
	case 26:
		if (ShotTime < Wait) return;
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
