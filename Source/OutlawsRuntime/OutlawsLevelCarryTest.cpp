// BUG-LEVELCARRY (user 2026-10-17): a level won tired, or with guns part fired, carried that into the next level.  Every new level
// now starts rested with the guns in hand loaded from the carried rounds as the reload does (TopUpForLevelStart); a save loaded
// mid-level keeps exactly what it saved.  BUG-HOLSTERRELOAD (user 2026-10-18): the holstered guns the same way (they came back
// full for nothing when drawn), and a restart gives every gun the rounds it held as the level was entered.  The checks:
// -OLShots -OLLevel=hideout -OLNoFrontEnd -OLLevelCarryTest (single player; with -OLMemLoadMapAbove=1 every level start is a map
// load, the Quest's way when its memory is high), and -OLCoopAuto -OLCoopTest=levelcarry on a story co-op host and its client
// (hideout), each machine checking its own player.

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "HAL/PlatformMisc.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Engine/World.h"
#include "OutlawsShotTests.h"

#if WITH_OUTLAWS_TESTS

namespace
{
	constexpr int32 LcRevolver = 0, LcRifle = 1, LcDouble = 3, LcSawed = 4;   // (GWeapons: ibullit, icart, and two on ishells)

	void LcSetCount(AOutlawsLevelActor* L, const TCHAR* Item, int32 N)
	{
		L->TakeCount(Item, L->GetCount(Item));
		if (N > 0) L->AddCount(Item, N);
	}
}

void AOutlawsPlayer::LevelCarryArrange(float InStamina, int32 RifleRounds, int32 RevolverRounds, int32 Bullets, int32 Carts)
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L) return;
	if (WeaponsOwned(LcRevolver) <= 0 || WeaponsOwned(LcRifle) <= 0 || WeaponsOwned(LcDouble) <= 0 || WeaponsOwned(LcSawed) <= 0) L->GiveAllWeapons();
	if (Equipped[0] != LcRifle) Equip(0, LcRifle);
	if (Equipped[1] != LcRevolver) Equip(1, LcRevolver);
	Mag[0] = FMath::Clamp(RifleRounds, 0, WeaponCapacity(LcRifle));
	Mag[1] = FMath::Clamp(RevolverRounds, 0, WeaponCapacity(LcRevolver));
	Spent[0] = Spent[1] = 0;                            // (both throw their cases out on the shot)
	LcSetCount(L, TEXT("ibullit"), Bullets);
	LcSetCount(L, TEXT("icart"), Carts);
	Stamina = InStamina;
}

void AOutlawsPlayer::LevelCarryHolster(int32 DoubleRounds, int32 SawedRounds, int32 Shells)
{
	// the double barrel and the sawed-off drawn into the left hand, given these rounds and put away; the left hand's gun back
	AOutlawsLevelActor* L = Level.Get();
	if (!L) return;
	const int32 W0 = Equipped[0], K = Mag[0];
	Equip(0, LcDouble);
	Mag[0] = FMath::Clamp(DoubleRounds, 0, WeaponCapacity(LcDouble));
	Equip(0, LcSawed);
	Mag[0] = FMath::Clamp(SawedRounds, 0, WeaponCapacity(LcSawed));
	Equip(0, W0);
	if (W0 >= 0)
	{
		Mag[0] = K;
		Spent[0] = 0;
	}
	LcSetCount(L, TEXT("ishells"), Shells);
}

int32 AOutlawsPlayer::LevelCarryDrawRounds(int32 Hand, int32 Weapon)
{
	const int32 W0 = Equipped[Hand], K = Mag[Hand];
	Equip(Hand, Weapon);
	const int32 Rounds = Equipped[Hand] == Weapon ? Mag[Hand] : -1;
	Equip(Hand, W0);
	if (W0 >= 0) Mag[Hand] = K;
	return Rounds;
}

FString AOutlawsPlayer::LevelCarryState() const
{
	const AOutlawsLevelActor* L = Level.Get();
	return FString::Printf(TEXT("stamina %.1f, left %d with %d/%d, right %d with %d/%d, ibullit %d, icart %d, ishells %d"), Stamina,
		Equipped[0], Mag[0], WeaponCapacity(Equipped[0]), Equipped[1], Mag[1], WeaponCapacity(Equipped[1]),
		L ? L->GetCount(TEXT("ibullit")) : -1, L ? L->GetCount(TEXT("icart")) : -1, L ? L->GetCount(TEXT("ishells")) : -1);
}

void AOutlawsPlayer::LevelCarryTestTick()
{
	static int32 Step = 0, Passes = 0, Fails = 0;
	static double Until = 0.0;
	static const bool bMapMode = OLFlags::Has(TEXT("OLMemLoadMapAbove"));     // (each level start a map load: a new player)
	static TWeakObjectPtr<AOutlawsPlayer> LeftFrom;                           // (the player the hideout was left with)
	AOutlawsLevelActor* L = Level.Get();
	if (!L || ShotTime < 3.f || Step < 0) return;
	const double Now = FPlatformTime::Seconds();
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT levelcarry check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	auto Expect = [&](const TCHAR* What, float MinStamina, float MaxStamina, int32 Rifle, int32 Revolver, int32 Bullets, int32 Carts)
	{
		const bool bOk = Stamina >= MinStamina && Stamina <= MaxStamina && Equipped[0] == LcRifle && Equipped[1] == LcRevolver && Mag[0] == Rifle
			&& Mag[1] == Revolver && L->GetCount(TEXT("ibullit")) == Bullets && L->GetCount(TEXT("icart")) == Carts;
		Check(bOk, FString::Printf(TEXT("%s: %s (wanted stamina %.1f..%.1f, rifle %d, revolver %d, ibullit %d, icart %d)"), What, *LevelCarryState(),
			MinStamina, MaxStamina, Rifle, Revolver, Bullets, Carts));
	};
	// (BUG-HOLSTERRELOAD) the holstered guns as they come out when drawn into the left hand, and the shells carried
	auto ExpectHolstered = [&](const TCHAR* What, int32 Double, int32 Sawed, int32 Shells)
	{
		const int32 D = LevelCarryDrawRounds(0, LcDouble), S = LevelCarryDrawRounds(0, LcSawed), Sh = L->GetCount(TEXT("ishells"));
		Check(D == Double && S == Sawed && Sh == Shells, FString::Printf(TEXT("%s, holstered: double barrel drawn with %d, sawed-off %d, ishells %d (wanted %d, %d, %d)"),
			What, D, S, Sh, Double, Sawed, Shells));
	};
	// every gun drawn from empty hands (a restart, or a map load's new player: nothing in hand), and the ammunition carried
	auto ExpectDrawn = [&](const TCHAR* What, int32 Rifle, int32 Revolver, int32 Double, int32 Sawed, int32 Bullets, int32 Carts, int32 Shells)
	{
		const int32 R = LevelCarryDrawRounds(0, LcRifle), V = LevelCarryDrawRounds(1, LcRevolver);
		const int32 D = LevelCarryDrawRounds(0, LcDouble), S = LevelCarryDrawRounds(0, LcSawed);
		const int32 B = L->GetCount(TEXT("ibullit")), C = L->GetCount(TEXT("icart")), Sh = L->GetCount(TEXT("ishells"));
		Check(Stamina >= 99.9f && R == Rifle && V == Revolver && D == Double && S == Sawed && B == Bullets && C == Carts && Sh == Shells,
			FString::Printf(TEXT("%s: stamina %.1f, drawn rifle %d, revolver %d, double barrel %d, sawed-off %d; ibullit %d, icart %d, ishells %d (wanted 100, %d, %d, %d, %d; %d, %d, %d)"),
			What, Stamina, R, V, D, S, B, C, Sh, Rifle, Revolver, Double, Sawed, Bullets, Carts, Shells));
	};
	switch (Step)
	{
	case 0:
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT levelcarry %s"), bMapMode ? TEXT("(every level start a map load)") : TEXT("(level starts built in place)"));
		// (a) the top-up itself, with a short reserve: 3 bullets for the revolver's 4 empty chambers, 9 cartridges for the rifle's 8;
		// holstered, the double barrel empty and the sawed-off with 1 of 2, 2 shells: the double barrel takes both
		LevelCarryArrange(12.f, 4, 2, 3, 9);
		LevelCarryHolster(0, 1, 2);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT levelcarry (a) before the top-up: %s"), *LevelCarryState());
		TopUpForLevelStart();
		Expect(TEXT("(a) topped up, short reserve"), 100.f, 100.f, 12, 5, 0, 1);
		ExpectHolstered(TEXT("(a) topped up, short reserve"), 2, 1, 0);
		// (b) tired and part loaded, on to the next level the way a won level goes (StartNextLevelThen; the skip with the pockets);
		// the same holstered guns and 2 shells: the next level loads them as (a) did
		LevelCarryArrange(12.f, 4, 2, 40, 50);
		LevelCarryHolster(0, 1, 2);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT levelcarry (b) leaving %s: %s"), *L->LevelName, *LevelCarryState());
		LeftFrom = this;
		L->JumpToLevel(TEXT("town"), true);
		Until = Now + 90.0;
		Step = 1;
		return;
	}
	case 1:
		// (a map load: the old world until the new one's player, then the town's start kit, 16 cartridges, until the opening's
		// carried kit is read in)
		if (!L->LevelName.Equals(TEXT("town"), ESearchCase::IgnoreCase) || L->IsRebuildPending() || !L->HasActorBegunPlay() || L->GetCount(TEXT("icart")) == 16
			|| (bMapMode && LeftFrom.Get() == this))
		{
			if (Now > Until) { Check(false, FString::Printf(TEXT("(b) the town was not reached: %s"), *LevelCarryState())); Step = 9; }
			return;
		}
		// the first tick in the town: 40 bullets and 50 cartridges carried (over the start kit's 24 and 16); 4 and 8 loaded
		if (!bMapMode)
		{
			Expect(TEXT("(b) the next level"), 99.9f, 100.f, 12, 6, 36, 42);
			ExpectHolstered(TEXT("(b) the next level"), 2, 1, 0);
		}
		else
		{
			ExpectDrawn(TEXT("(b) the next level (map load)"), 12, 6, 2, 1, 36, 42, 0);
		}
		Until = Now + 3.0;
		Step = 2;
		return;
	case 2:
		if (Now < Until) return;
		// (b2) everything fired and spent, then the level restarted: back to what the town was entered with, guns and all
		LevelCarryArrange(50.f, 3, 1, 5, 5);
		LevelCarryHolster(0, 0, 0);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT levelcarry (b2) restarting %s: %s"), *L->LevelName, *LevelCarryState());
		L->RestartLevel();
		Until = Now + 90.0;
		Step = 4;
		return;
	case 4:
		if (L->IsRebuildPending() || !L->HasActorBegunPlay() || L->GetCount(TEXT("icart")) != 42)
		{
			if (Now > Until) { Check(false, FString::Printf(TEXT("(b2) the restart never came in: %s"), *LevelCarryState())); Step = 9; }
			return;
		}
		ExpectDrawn(TEXT("(b2) the level restarted"), 12, 6, 2, 1, 36, 42, 0);
		Until = Now + 3.0;
		Step = 5;
		return;
	case 5:
		if (Now < Until) return;
		// (c) a mid-level save while tired with part-loaded guns, the values then changed, and the save loaded: as saved
		LevelCarryArrange(33.f, 7, 1, 4, 9);
		LevelCarryHolster(1, 0, 3);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT levelcarry (c) saving: %s"), *LevelCarryState());
		Check(L->SaveGame(7), TEXT("(c) saved to slot 7"));
		LevelCarryArrange(100.f, 12, 6, 50, 50);
		LevelCarryHolster(2, 2, 9);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT levelcarry (c) changed before the load: %s"), *LevelCarryState());
		Check(L->LoadGame(7), TEXT("(c) the load started"));
		Until = Now + 60.0;
		Step = 3;
		return;
	case 3:
		// the load is in once the counts are the save's (the player read in the same call)
		if (L->IsRebuildPending() || L->GetCount(TEXT("ibullit")) != 4)
		{
			if (Now > Until) { Check(false, FString::Printf(TEXT("(c) the load never came in: %s"), *LevelCarryState())); Step = 9; }
			return;
		}
		{
			// the saved stamina as written, and the player's now: at most this frame's rest on top (the quickest, crouched and
			// still: 13 a second since REQ-STAMINA15, user 2026-10-27, 6.5 with -OLOldStamina; a frame after a load is long headless)
			const float Dt = GetWorld() ? GetWorld()->GetDeltaSeconds() : 0.f;
			const float MaxRest = StaminaRate(false, false, true, 0.5f, false, OldStamina());
			FString Text;
			TSharedPtr<FJsonObject> O;
			double SavedStamina = -1.0;
			const TSharedPtr<FJsonObject>* Pl = nullptr;
			if (FFileHelper::LoadFileToString(Text, *(FPaths::ProjectSavedDir() / TEXT("OutlawsSaves/slot7.json"))) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), O)
				&& O.IsValid() && O->TryGetObjectField(TEXT("player"), Pl)) SavedStamina = (*Pl)->GetNumberField(TEXT("stamina"));
			Check(FMath::IsNearlyEqual(SavedStamina, 33.0, 0.01), FString::Printf(TEXT("(c) slot 7 holds stamina %.2f"), SavedStamina));
			// (a map load: the new player's first 3 s go by before these checks run, resting)
			const float Rest = bMapMode ? Dt + ShotTime : Dt;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT levelcarry (c) %.3f s: up to %.2f stamina of rest since the load"), Rest, MaxRest * Rest);
			Expect(TEXT("(c) the save loaded"), 33.f, 33.f + MaxRest * Rest + 0.01f, 7, 1, 4, 9);
			ExpectHolstered(TEXT("(c) the save loaded"), 1, 0, 3);
		}
		// (d) the loaded level restarted: the save's own entry (its entry_mags) gives the guns back as the town was entered
		L->RestartLevel();
		Until = Now + 90.0;
		Step = 6;
		return;
	case 6:
		if (L->IsRebuildPending() || !L->HasActorBegunPlay() || L->GetCount(TEXT("icart")) != 42)
		{
			if (Now > Until) { Check(false, FString::Printf(TEXT("(d) the restart never came in: %s"), *LevelCarryState())); Step = 9; }
			return;
		}
		ExpectDrawn(TEXT("(d) restarted after the load"), 12, 6, 2, 1, 36, 42, 0);
		Step = 9;
		return;
	case 9:
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT levelcarry RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		Step = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

OL_SHOT_MATCH(OLLevelCarryTest) { return OLFlags::Param(TEXT("OLLevelCarryTest")); }
OL_SHOT_TEST(OLLevelCarryTest, 10)
{
	LevelCarryTestTick();            // (BUG-LEVELCARRY: OutlawsLevelCarryTest.cpp)
}

void UOutlawsCoop::TickLevelCarryTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	const bool bHost = IsHost();
	const TCHAR* Me = bHost ? TEXT("host") : TEXT("client");
	auto Check = [this, Me](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLCOOP levelcarry check %s (%s): %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), Me, *What);
		if (bOk) ++LcPasses; else ++LcFails;
	};
	// (BUG-HOLSTERRELOAD) every gun drawn, from the hands as they are (the hands' own put back), and the ammunition carried
	auto Drawn = [P, L](int32& R, int32& V, int32& D, int32& S)
	{
		R = P->LevelCarryDrawRounds(0, 1);
		V = P->LevelCarryDrawRounds(1, 0);
		D = P->LevelCarryDrawRounds(0, 3);
		S = P->LevelCarryDrawRounds(0, 4);
		return FString::Printf(TEXT("drawn rifle %d, revolver %d, double barrel %d, sawed-off %d; %s"), R, V, D, S, *P->LevelCarryState());
	};
	switch (LcStep)
	{
	case 0:
		if (!L->LevelName.Equals(TEXT("hideout"), ESearchCase::IgnoreCase)) return;
		if (bHost)
		{
			const AOutlawsCoopLink* CL = GetLink(PartnerIndex());
			if (!CL || CL->BuiltSerial != StartedSerial || !PartnerPuppet()) return;
		}
		if (LcAt == 0.0) { LcAt = Now + 4.0; return; }
		if (Now < LcAt) return;
		// each machine's own player: tired, the rifle 4 of 12 and the revolver 2 of 6, 40 bullets and 50 cartridges carried;
		// holstered, the double barrel empty and the sawed-off 1 of 2, with 2 shells
		P->LevelCarryArrange(12.f, 4, 2, 40, 50);
		P->LevelCarryHolster(0, 1, 2);
		UE_LOG(LogOutlaws, Log, TEXT("OLCOOP levelcarry (%s) in the hideout: %s"), Me, *P->LevelCarryState());
		LcAt = Now + 8.0;                          // (the host's start: the client has made its own ready by then)
		LcStep = 1;
		return;
	case 1:
		if (L->IsRebuildPending() || !L->LevelName.Equals(TEXT("hideout"), ESearchCase::IgnoreCase))
		{
			LcAt = Now + 90.0;
			LcStep = 2;
			return;
		}
		P->Stamina = 12.f;                         // (kept tired up to the level's end)
		if (bHost && Now >= LcAt && LcAt > 0.0)
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLCOOP levelcarry: the host starts the next level (the town) for both"));
			RequestStart(EOutlawsCoopStart::NextLevel, TEXT("town"));
			LcAt = -1.0;
		}
		return;
	case 2:
		if (!L->LevelName.Equals(TEXT("town"), ESearchCase::IgnoreCase) || L->IsRebuildPending() || !L->HasActorBegunPlay())
		{
			if (Now > LcAt) { Check(false, TEXT("the town was not reached")); LcStep = 3; }
			return;
		}
		{
			const bool bOk = P->Stamina >= 99.9f && P->Equipped[0] == 1 && P->Equipped[1] == 0 && P->Mag[0] == 12 && P->Mag[1] == 6
				&& L->GetCount(TEXT("ibullit")) == 36 && L->GetCount(TEXT("icart")) == 42;
			Check(bOk, FString::Printf(TEXT("the first tick in the town: %s (wanted stamina 100, rifle 12, revolver 6, ibullit 36, icart 42)"), *P->LevelCarryState()));
			int32 R, V, D, S;
			const FString Line = Drawn(R, V, D, S);
			Check(D == 2 && S == 1 && L->GetCount(TEXT("ishells")) == 0, FString::Printf(TEXT("the holstered guns in the town: %s (wanted double barrel 2, sawed-off 1, ishells 0)"), *Line));
		}
		// then everything fired and spent, and the host restarts the town for both: each back to what it entered with
		P->LevelCarryArrange(50.f, 3, 1, 5, 5);
		P->LevelCarryHolster(0, 0, 0);
		UE_LOG(LogOutlaws, Log, TEXT("OLCOOP levelcarry (%s) spent before the restart: %s"), Me, *P->LevelCarryState());
		LcAt = Now + 8.0;
		LcStep = 10;
		return;
	case 10:
		// (the restart under way, or done already: the 5 cartridges left are the entry's 42 again)
		if (L->IsRebuildPending() || L->GetCount(TEXT("icart")) == 42)
		{
			LcAt = Now + 90.0;
			LcStep = 11;
			return;
		}
		if (bHost && LcAt > 0.0 && Now >= LcAt)
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLCOOP levelcarry: the host restarts the town for both"));
			RequestStart(EOutlawsCoopStart::Restart, TEXT("town"));
			LcAt = -1.0;
		}
		else if (!bHost && LcAt > 0.0 && Now > LcAt + 60.0)
		{
			Check(false, TEXT("the host's restart never came"));
			LcStep = 3;
		}
		return;
	case 11:
		if (L->IsRebuildPending() || !L->HasActorBegunPlay() || L->GetCount(TEXT("icart")) != 42)
		{
			if (Now > LcAt) { Check(false, FString::Printf(TEXT("the restart never came in: %s"), *P->LevelCarryState())); LcStep = 3; }
			return;
		}
		{
			int32 R, V, D, S;
			const FString Line = Drawn(R, V, D, S);
			Check(P->Stamina >= 99.9f && R == 12 && V == 6 && D == 2 && S == 1 && L->GetCount(TEXT("ibullit")) == 36 && L->GetCount(TEXT("ishells")) == 0,
				FString::Printf(TEXT("the town restarted: %s (wanted stamina 100, rifle 12, revolver 6, double barrel 2, sawed-off 1, ibullit 36, icart 42, ishells 0)"), *Line));
		}
		LcStep = 3;
		return;
	case 3:
		UE_LOG(LogOutlaws, Log, TEXT("OLCOOP levelcarry %s RESULT %s (%d passed, %d failed)"), Me, LcFails == 0 && LcPasses > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), LcPasses, LcFails);
		LcAt = Now + (bHost ? 25.0 : 120.0);          // (the host waits for the client's turn, the client for the host's quit)
		LcStep = bHost ? 4 : 20;
		return;
	case 4:
		if (Now < LcAt) return;
		TestSay(TEXT("quit"), 0);
		LcAt = Now + 2.0;
		LcStep = 5;
		return;
	case 5:
		if (Now < LcAt) return;
		Summary(TEXT("done"));
		QuitForTest();
		LcStep = 6;
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
