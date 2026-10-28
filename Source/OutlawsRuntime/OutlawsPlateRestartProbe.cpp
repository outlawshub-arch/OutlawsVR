// BUG-PLATERESTART (lane SNAPA's question 4, 2026-10-28; the planner's rule: a restart gives back what the level was entered
// with): -OLShots -OLLevel=hideout -OLNoFrontEnd -OLPlateRestartProbe.  (a) a medium plate dented once in the hideout,
// carried into the town as a won level carries it: 85 of 100; (b) a heavy plate picked up in the town (150 of 150), the
// town restarted: the plate it was entered with, 85 of 100, marked by its wear, no message; (c) the heavy plate again, a
// save, that save loaded and the town restarted: 85 of 100 again (the save keeps the entry size); (d) the same save as
// one written before this (no entry_armour_max): loaded and restarted as before, the entered armour on the plate size worn
// (85 of 150).  The plate is shown on the desktop for the checks (bTestPlatePickup).  Lines "OLSHOT platerestart check
// PASS|FAIL: ..." and "OLSHOT platerestart RESULT".

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

OL_SHOT_MATCH(OLPlateRestartProbe) { return OLFlags::Param(TEXT("OLPlateRestartProbe")); }

OL_SHOT_TEST(OLPlateRestartProbe, 19)
{
	static int32 Passes = 0, Fails = 0, Shows = 0, ShowsSeen = 0, ShowsMark = 0;
	static float Wait = 0.f;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT platerestart check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
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
	auto State = [&]() { return FString::Printf(TEXT("armour %d of %d, shown %d, %d dents, 'breaks apart' shown %d times since the case began"), L->GetArmour(), L->ArmourMax,
		bShown ? 1 : 0, ChestDents(), Shows - ShowsMark); };
	auto Hit = [&](float Damage) { L->bInvincible = false; L->DamagePlayer(Damage, 1.f, false); L->bInvincible = true; };
	auto WearDents = [&]() { const int32 Max = FMath::Max(1, FMath::Max(L->ArmourMax, L->GetArmour())); return FMath::Clamp(int32((1.0 - double(L->GetArmour()) / Max) * 10), 0, 10); };
	auto Next = [&](float Secs) { Wait = ShotTime + Secs; ++ShotStep; };
	auto Mark = [&]() { ShowsMark = Shows; };
	auto Entered = [&](const TCHAR* What)
	{
		Check(L->GetArmour() == 85 && L->ArmourMax == 100 && bShown && ChestDents() == WearDents() && Shows == ShowsMark,
			FString::Printf(TEXT("%s: the plate the town was entered with, 85 of 100, marked by its wear (%d dents), no 'breaks apart': %s"), What, WearDents(), *State()));
	};
	switch (ShotStep)
	{
	case 0:
		if (ShotTime < 3.f || !bUp) return;
		bTestPlatePickup = true;
		L->bInvincible = true;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT platerestart start in %s: %s"), *L->LevelName, *State());
		// (a) a medium plate (as a pickup takes it), one hit, carried into the town
		Mark();
		L->ArmourMax = 0;
		L->SetArmour(0);
		L->AddCount(TEXT("ibplate"), 100);
		Next(0.5f);
		return;
	case 1:
		if (!bUp) return;
		Hit(1.5f);                                 // (15 points, the plate takes all of it)
		Next(0.5f);
		return;
	case 2:
		if (!bUp) return;
		Check(L->GetArmour() == 85 && L->ArmourMax == 100 && bShown, FString::Printf(TEXT("(a) a medium plate dented once in the hideout: %s"), *State()));
		L->JumpToLevel(TEXT("town"), true);
		Next(3.f);
		return;
	case 3:
		if (!bUp || !L->LevelName.Equals(TEXT("town"), ESearchCase::IgnoreCase)) return;
		Entered(TEXT("(a) carried into the town"));
		// (b) a heavy plate picked up in the town, then the town restarted
		Mark();
		L->AddCount(TEXT("ibplate"), 150);
		Next(0.5f);
		return;
	case 4:
		if (!bUp) return;
		Check(L->GetArmour() == 150 && L->ArmourMax == 150 && bShown, FString::Printf(TEXT("(b) a heavy plate picked up in the town is worn: %s"), *State()));
		L->RestartLevel();
		Next(3.f);
		return;
	case 5:
		if (!bUp || L->GetArmour() == 150) return;
		Entered(TEXT("(b) the town restarted"));
		// (c) the heavy plate again, a save, the save loaded, the town restarted
		Mark();
		L->AddCount(TEXT("ibplate"), 150);
		Next(0.5f);
		return;
	case 6:
		if (!bUp) return;
		Check(L->GetArmour() == 150 && L->SaveGame(7), FString::Printf(TEXT("(c) saved to slot 7 wearing the heavy plate: %s"), *State()));
		{
			// the same save as one written before BUG-PLATERESTART (no entry_armour_max), for (d)
			FString Text;
			TSharedPtr<FJsonObject> O;
			const FString Dir = FPaths::ProjectSavedDir() / TEXT("OutlawsSaves");
			bool bOld = FFileHelper::LoadFileToString(Text, *(Dir / TEXT("slot7.json"))) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), O) && O.IsValid();
			const bool bHad = bOld && O->HasField(TEXT("entry_armour_max")) && int32(O->GetNumberField(TEXT("entry_armour_max"))) == 100;
			if (bOld)
			{
				O->RemoveField(TEXT("entry_armour_max"));
				FString Out;
				bOld = FJsonSerializer::Serialize(O.ToSharedRef(), TJsonWriterFactory<>::Create(&Out)) && FFileHelper::SaveStringToFile(Out, *(Dir / TEXT("slot9.json")));
			}
			Check(bHad && bOld, FString::Printf(TEXT("(c) slot 7 keeps the entry plate's size (entry_armour_max 100: %d); slot 9 written without it: %d"), bHad ? 1 : 0, bOld ? 1 : 0));
		}
		Check(L->LoadGame(7), TEXT("(c) loading slot 7"));
		Next(3.f);
		return;
	case 7:
		if (!bUp) return;
		Check(L->GetArmour() == 150 && L->ArmourMax == 150, FString::Printf(TEXT("(c) slot 7 loaded: the heavy plate worn: %s"), *State()));
		Mark();
		L->RestartLevel();
		Next(3.f);
		return;
	case 8:
		if (!bUp || L->GetArmour() == 150) return;
		Entered(TEXT("(c) slot 7 loaded and the town restarted"));
		// (d) the old save: loaded and restarted as before
		Mark();
		Check(L->LoadGame(9), TEXT("(d) loading slot 9 (a save from before, no entry_armour_max)"));
		Next(3.f);
		return;
	case 9:
		if (!bUp) return;
		Check(L->GetArmour() == 150 && L->ArmourMax == 150, FString::Printf(TEXT("(d) slot 9 loaded: the heavy plate worn: %s"), *State()));
		Mark();
		L->RestartLevel();
		Next(3.f);
		return;
	case 10:
		if (!bUp || L->GetArmour() == 150) return;
		Check(L->GetArmour() == 85 && L->ArmourMax == 150 && Shows == ShowsMark,
			FString::Printf(TEXT("(d) the old save's restart as before: the entered 85 on the plate size worn (85 of 150), no 'breaks apart': %s"), *State()));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT platerestart RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		Next(1.f);
		return;
	case 11:
		if (ShotTime < Wait) return;
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
