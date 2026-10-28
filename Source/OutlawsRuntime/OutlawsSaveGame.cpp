// Saves and the autosave (user 2026-09-27): the game as it stands written to Saved/OutlawsSaves as JSON
// (slot0..9.json and autosave.json).  A save names its level; loading rebuilds that level from the LABs as a
// restart does and then puts back what changed: the player (where, which way, health, plate, gold, every
// count and listed item, the guns in hand and their rounds, the lantern), the bad guys (where, facing,
// health, alert, dead, a boss not yet out), what is left to pick up (and anything dropped since), broken
// props, glass and dynamite, and the INF (elevator values and stops, trigger arming, scroll offsets, keys).
// The autosave comes at the start of every level and every AutosaveMinutes after (options, default 2).
#include "OutlawsLevelActor.h"
#include "OutlawsStats.h"
#include "OutlawsPerf.h"
#include "OutlawsPerfTest.h"
#include "OutlawsRuntime.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsEnemy.h"
#include "OutlawsPlayer.h"
#include "OutlawsCoop.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsMovieScreen.h"
#include "ProceduralMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"

namespace
{
	using FJsonArray = TArray<TSharedPtr<FJsonValue>>;

	TSharedPtr<FJsonValue> Num(double V) { return MakeShared<FJsonValueNumber>(V); }

	void SetVec(FJsonObject& O, const FVector& V)
	{
		O.SetNumberField(TEXT("x"), V.X);
		O.SetNumberField(TEXT("y"), V.Y);
		O.SetNumberField(TEXT("z"), V.Z);
	}

	FVector GetVec(const FJsonObject& O)
	{
		return FVector(O.GetNumberField(TEXT("x")), O.GetNumberField(TEXT("y")), O.GetNumberField(TEXT("z")));
	}

	const FJsonArray* Array(const FJsonObject& O, const TCHAR* Name)
	{
		const FJsonArray* A = nullptr;
		return O.TryGetArrayField(Name, A) ? A : nullptr;
	}

	FString GameTimeText(double Seconds)
	{
		const int32 S = FMath::Max(0, FMath::FloorToInt(Seconds));
		return FString::Printf(TEXT("%d:%02d:%02d"), S / 3600, (S / 60) % 60, S % 60);
	}

	FString StampText(const FString& Iso)
	{
		FDateTime T;
		if (!FDateTime::ParseIso8601(*Iso, T)) return Iso;
		static const TCHAR* Months[] = { TEXT("Jan"), TEXT("Feb"), TEXT("Mar"), TEXT("Apr"), TEXT("May"), TEXT("Jun"), TEXT("Jul"), TEXT("Aug"), TEXT("Sep"), TEXT("Oct"), TEXT("Nov"), TEXT("Dec") };
		const int32 H = T.GetHour();
		return FString::Printf(TEXT("%d %s %d  %d:%02d %s"), T.GetDay(), Months[FMath::Clamp(T.GetMonth(), 1, 12) - 1], T.GetYear(),
			H % 12 == 0 ? 12 : H % 12, T.GetMinute(), H < 12 ? TEXT("AM") : TEXT("PM"));
	}

	bool ReadJson(const FString& Path, TSharedPtr<FJsonObject>& Out)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path)) return false;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, Out) && Out.IsValid();
	}
}

// ---- level actor ---------------------------------------------------------------------------------------

namespace
{
	// BUG-DEATHAUTOSAVE: a slot loaded or saved with no autosave since (-1 none); a death goes back to it, not the autosave
	int32 GDeathSlot = -1;
}

void AOutlawsLevelActor::NoteDeathSlot(int32 Slot)
{
	if (GDeathSlot != Slot) UE_LOG(LogOutlaws, Log, TEXT("OLRT a death now goes back to %s"), Slot >= 0 ? *FString::Printf(TEXT("slot %d"), Slot) : TEXT("the autosave"));
	GDeathSlot = Slot;
}

bool AOutlawsLevelActor::DeathSave(int32& OutSlot) const
{
	// (the user's Quest: dying after a load, before the next autosave, loaded an autosave of another level)
	OutSlot = -1;
	if (OutlawsPerfTest::IsOn()) return false;         // (REQ-PERFTESTMODE: a run's death restarts its level, never a save of the player's)
	if (GDeathSlot >= 0)
	{
		const FSaveInfo S = ReadSaveInfo(GDeathSlot);
		if (!S.bEmpty && S.Level.Equals(LevelName, ESearchCase::IgnoreCase))
		{
			OutSlot = GDeathSlot;
			return true;
		}
	}
	const FSaveInfo A = ReadSaveInfo(-1);
	const bool bOk = !A.bEmpty && A.Level.Equals(LevelName, ESearchCase::IgnoreCase);
	if (!bOk && !A.bEmpty) UE_LOG(LogOutlaws, Log, TEXT("OLRT death: the autosave is of %s, not %s: not offered"), *A.Level, *LevelName);
	return bOk;
}

FString AOutlawsLevelActor::SavePath(int32 Slot)
{
	// (-2: a co-op host's own autosave, user 2026-10-03: "in a separate co-op autosave")
	return FPaths::ProjectSavedDir() / TEXT("OutlawsSaves") / (Slot == -2 ? FString(TEXT("coop_autosave.json")) : Slot < 0 ? FString(TEXT("autosave.json"))
		: FString::Printf(TEXT("slot%d.json"), Slot));
}

bool AOutlawsLevelActor::PatchSaveDifficulty(int32 Slot, EOutlawsDifficulty To, EOutlawsDifficulty Was)
{
	TSharedPtr<FJsonObject> O;
	if (!ReadJson(SavePath(Slot), O) || !O.IsValid()) return false;
	if (!O->HasField(TEXT("spawn_difficulty"))) O->SetNumberField(TEXT("spawn_difficulty"), O->HasField(TEXT("difficulty")) ? O->GetNumberField(TEXT("difficulty")) : double(int32(Was)));
	O->SetNumberField(TEXT("difficulty"), int32(To));
	FString Text;
	if (!FJsonSerializer::Serialize(O.ToSharedRef(), TJsonWriterFactory<>::Create(&Text))) return false;
	return FFileHelper::SaveStringToFile(Text, *SavePath(Slot));
}

AOutlawsLevelActor::FSaveInfo AOutlawsLevelActor::ReadSaveInfo(int32 Slot)
{
	FSaveInfo I;
	I.Slot = Slot;
	TSharedPtr<FJsonObject> O;
	if (!ReadJson(SavePath(Slot), O)) return I;
	I.Level = O->GetStringField(TEXT("level"));
	if (I.Level.IsEmpty()) return I;
	I.bEmpty = false;
	I.Name = O->GetStringField(TEXT("name"));
	I.GameTime = GameTimeText(O->GetNumberField(TEXT("playtime")));
	I.Stamp = StampText(O->GetStringField(TEXT("stamp")));
	return I;
}

FString AOutlawsLevelActor::LevelTitle(const FString& Level)
{
	// levelselect.rml's names for the story levels
	static const TMap<FString, FString> Titles = {
		{ TEXT("hideout"), TEXT("The Hideout") }, { TEXT("townex"), TEXT("The Town") }, { TEXT("town"), TEXT("The Town") },
		{ TEXT("train"), TEXT("The Train") }, { TEXT("canyon"), TEXT("The Canyon") }, { TEXT("mill"), TEXT("The Sawmill") },
		{ TEXT("simms"), TEXT("Simm's Fort") }, { TEXT("miner"), TEXT("The Mine") }, { TEXT("cliff"), TEXT("The Cliff Dwellings") },
		{ TEXT("ranch"), TEXT("Bob's Ranch") },
		// (REQ-HMCAREER) the Handful of Missions, by the remaster's names
		{ TEXT("civlwar1"), TEXT("Civil War") }, { TEXT("civlwar2"), TEXT("Civil War") }, { TEXT("icecaves"), TEXT("Ice Caves") },
		{ TEXT("villa"), TEXT("Villa") }, { TEXT("wharf"), TEXT("Wharf Town") } };
	if (const FString* T = Titles.Find(Level.ToLower())) return *T;
	FString T = Level.ToLower();
	if (T.Len() > 0) T[0] = FChar::ToUpper(T[0]);
	return T;
}

int32 AOutlawsLevelActor::LevelPicture(const FString& Level)
{
	// UI.kpf Load_Level01..09 are the story levels in order (checked by eye 2026-09-27); 16 is the "???" card
	static const TCHAR* Story[] = { TEXT("hideout"), TEXT("townex"), TEXT("train"), TEXT("canyon"), TEXT("mill"), TEXT("simms"), TEXT("miner"), TEXT("cliff"), TEXT("ranch") };
	const FString L = Level.ToLower();
	for (int32 i = 0; i < UE_ARRAY_COUNT(Story); ++i)
	{
		if (L == Story[i] || (i == 1 && L == TEXT("town"))) return i + 1;
	}
	return 16;
}

bool AOutlawsLevelActor::SaveLevelOpening()
{
	// (co-op: the host's goes to its own co-op autosave, R7; the client never saves)
	const bool bCoop = UOutlawsCoop::IsActive(this);
	int32 Slot = -1;
	// (REQ-TUTORIAL, REQ-PERFTESTMODE: nothing saved)
	if (!IsBetweenLevels() || (bCoop && !UOutlawsCoop::MaySave(this, Slot)) || bTutorial || OutlawsPerfTest::IsOn())
	{
		return false;
	}
	const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetNumberField(TEXT("version"), 1);
	O->SetBoolField(TEXT("opening"), true);            // (no level state: the level from its start, the kit below carried in)
	O->SetStringField(TEXT("level"), NextLevelName);
	O->SetStringField(TEXT("name"), LevelTitle(NextLevelName));
	O->SetNumberField(TEXT("difficulty"), int32(Difficulty));
	O->SetNumberField(TEXT("spawn_difficulty"), int32(Difficulty));
	O->SetNumberField(TEXT("playtime"), PlayTime);
	O->SetStringField(TEXT("stamp"), FDateTime::Now().ToIso8601());
	O->SetNumberField(TEXT("health"), 10.0);
	O->SetNumberField(TEXT("armour"), Armour);
	O->SetNumberField(TEXT("armour_max"), ArmourMax);
	O->SetNumberField(TEXT("gold"), Gold);
	O->SetNumberField(TEXT("kills"), 0);
	O->SetBoolField(TEXT("lamp"), false);
	O->SetNumberField(TEXT("oilburn"), OilBurn);
	O->SetNumberField(TEXT("badge"), 0.0);
	// what goes on to the next level: all but the keys and puzzle pieces of the level left (as StartNextLevel carries)
	const TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
	for (const TPair<FString, int32>& Kv : Counts)
	{
		const FItemDef* D = ItemDefs.Find(Kv.Key);
		const FString Class = D ? D->Class.ToLower() : FString();
		if (!Class.Contains(TEXT("key")) && !Class.Contains(TEXT("puzz"))) C->SetNumberField(Kv.Key, Kv.Value);
	}
	O->SetObjectField(TEXT("counts"), C);
	// (BUG-HOLSTERRELOAD) and the guns' rounds, loaded from that ammunition when it is read, as the next level's start does
	if (UWorld* Wd = GetWorld(); Wd && Wd->GetFirstPlayerController())
	{
		if (const AOutlawsPlayer* P = Cast<AOutlawsPlayer>(Wd->GetFirstPlayerController()->GetPawn())) P->WriteCarryMags(*O, false);
	}
	WriteRandoSave(*O, true);                          // (R10: the run's randomizer; the next level from its first roll)
	FString Text;
	if (!FJsonSerializer::Serialize(O, TJsonWriterFactory<>::Create(&Text)))
	{
		return false;
	}
	const FString Path = SavePath(Slot);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	const bool bOk = FFileHelper::SaveStringToFile(Text, *Path);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT %s %s: the opening of %s (the level before it is won), %.0f s played"), Slot == -2 ? TEXT("co-op autosave") : TEXT("autosave"),
		bOk ? TEXT("written") : TEXT("FAILED"), *NextLevelName, PlayTime);
	if (bOk && Slot == -1) NoteDeathSlot(-1);          // (BUG-DEATHAUTOSAVE: the autosave is the newest again)
	return bOk;
}

bool AOutlawsLevelActor::SaveGame(int32 Slot)
{
	UWorld* W = GetWorld();
	AOutlawsPlayer* P = W && W->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(W->GetFirstPlayerController()->GetPawn()) : nullptr;
	// (never the main menu's level: bFrontEnd)
	if (!P || !Inf || bDead || P->IsDeadView() || bFrontEnd || LevelEndTimer >= 0.f)      // (nor a level on its way out: its boss is down)
	{
		return false;
	}
	if (bTutorial)
	{
		// (REQ-TUTORIAL: the tutorial never writes over the player's autosave or a slot)
		UE_LOG(LogOutlaws, Log, TEXT("OLRT save %d: not in the tutorial"), Slot);
		return false;
	}
	if (UOutlawsCoop::IsActive(this) && !UOutlawsCoop::MaySave(this, Slot))
	{
		return false;              // (co-op: only the host saves; its autosave to its own file, R7)
	}
	FString Text;
	int32 NumEnemies = 0, NumPickups = 0;
	if (!BuildSaveText(P, Text, NumEnemies, NumPickups))
	{
		return false;
	}
	if (OutlawsPerfTest::IsOn())
	{
		// (REQ-PERFTESTMODE: a performance test run's autosave is made as in play, its cost in the capture, and never written)
		UE_LOG(LogOutlaws, Log, TEXT("OLRT %s %d: made (%d characters), not written in a performance test run"), Slot < 0 ? TEXT("autosave") : TEXT("save"), Slot, Text.Len());
		return false;
	}
	const FString Path = SavePath(Slot);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	const bool bOk = FFileHelper::SaveStringToFile(Text, *Path);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT %s %s: %s, %d bad guys, %d pickups, %.0f s played -> %s"), Slot == -2 ? TEXT("co-op autosave") : Slot < 0 ? TEXT("autosave") : TEXT("save"),
		bOk ? TEXT("written") : TEXT("FAILED"), *LevelName, NumEnemies, NumPickups, PlayTime, *Path);
	if (bOk && UOutlawsCoop::IsActive(this)) UOutlawsCoop::NoteSaved(this, Slot, Text.Len());
	else if (bOk) NoteDeathSlot(Slot >= 0 ? Slot : -1);    // (BUG-DEATHAUTOSAVE: a death goes back to the newest save)
	return bOk;
}

bool AOutlawsLevelActor::BuildSaveText(AOutlawsPlayer* P, FString& OutText, int32& OutEnemies, int32& OutPickups)
{
	// the game as it stands, as JSON (SaveGame writes it to a file; a co-op host sends it to a player joining late, R7)
	// (DM-ROTATE: a level with no .inf, as six of the stock match maps, has no INF runtime: the rest is still the game, and a
	// late joiner waited for it for good)
	if (!P) return false;
	const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
	O->SetNumberField(TEXT("version"), 1);
	O->SetStringField(TEXT("level"), LevelName);
	O->SetStringField(TEXT("name"), LevelTitle(LevelName));
	O->SetNumberField(TEXT("difficulty"), int32(Difficulty));
	O->SetNumberField(TEXT("spawn_difficulty"), int32(SpawnDifficulty));
	O->SetNumberField(TEXT("playtime"), PlayTime);
	O->SetNumberField(TEXT("level_seconds"), UOutlawsStats::SavedSeconds(this));       // (REQ-LEVELTIMER: the level's clock, which a load carries on)
	O->SetStringField(TEXT("stamp"), FDateTime::Now().ToIso8601());

	// the player's stats and inventory
	O->SetNumberField(TEXT("health"), Health);
	O->SetNumberField(TEXT("armour"), Armour);
	O->SetNumberField(TEXT("armour_max"), ArmourMax);
	O->SetNumberField(TEXT("gold"), Gold);
	O->SetNumberField(TEXT("kills"), Kills);
	O->SetBoolField(TEXT("lamp"), bLampOn);
	O->SetNumberField(TEXT("oilburn"), OilBurn);
	O->SetNumberField(TEXT("badge"), BadgeLeft);
	const TSharedRef<FJsonObject> C = MakeShared<FJsonObject>();
	for (const TPair<FString, int32>& Kv : Counts) C->SetNumberField(Kv.Key, Kv.Value);
	O->SetObjectField(TEXT("counts"), C);
	// what this level was entered with (a restart gives it back): kept with the save, so it outlives the session
	const bool bEntry = !EntryLevel.IsEmpty() && EntryLevel.Equals(LevelName, ESearchCase::IgnoreCase);
	O->SetStringField(TEXT("entry_level"), bEntry ? EntryLevel : FString());
	if (bEntry)
	{
		const TSharedRef<FJsonObject> Ec = MakeShared<FJsonObject>();
		for (const TPair<FString, int32>& Kv : EntryCounts) Ec->SetNumberField(Kv.Key, Kv.Value);
		O->SetObjectField(TEXT("entry_counts"), Ec);
		O->SetNumberField(TEXT("entry_gold"), EntryGold);
		O->SetNumberField(TEXT("entry_armour"), EntryArmour);
		if (EntryArmourMax >= 0) O->SetNumberField(TEXT("entry_armour_max"), EntryArmourMax);     // (BUG-PLATERESTART)
	}
	FJsonArray Inv, Labels;
	for (const FString& S : Inventory) Inv.Add(MakeShared<FJsonValueString>(S));
	for (const FString& S : InventoryLabels) Labels.Add(MakeShared<FJsonValueString>(S));
	O->SetArrayField(TEXT("inventory"), Inv);
	O->SetArrayField(TEXT("labels"), Labels);
	const TSharedRef<FJsonObject> Pl = MakeShared<FJsonObject>();
	P->WriteSave(*Pl);
	O->SetObjectField(TEXT("player"), Pl);

	// the bad guys (and chickens), in the order the level spawned them
	FJsonArray Es;
	for (const TWeakObjectPtr<AOutlawsEnemy>& E : Enemies)
	{
		const TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		if (E.IsValid()) E->WriteSave(*J);
		Es.Add(MakeShared<FJsonValueObject>(J));
	}
	O->SetArrayField(TEXT("enemies"), Es);

	// what is still lying about: the level's own by their order there, dropped ones by name
	FJsonArray Ps;
	for (const FPickup& Pk : Pickups)
	{
		const AActor* A = Pk.Actor.Get();
		if (!A) continue;
		// the level's own sticks of dynamite (no serial): the build makes them again and the dynamite list below keeps the
		// ones gone; saved as dropped they came back twice, a second stick that was no pickup (10 Oct: 15 at a rejoin)
		if (Pk.Serial < 0 && IsDynamitePickup(A)) continue;
		const TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetNumberField(TEXT("serial"), Pk.Serial);
		J->SetStringField(TEXT("ground"), Pk.Ground);
		if (Pk.bMapSeen) J->SetBoolField(TEXT("map_seen"), true);      // (REQ-MAPITEMS: on this player's map; older saves: none seen)
		if (Pk.Serial < 0)
		{
			// a dropped thing: what it gives (a gun passed from the wheel or left at a death gives what was carried, user 2026-10-02)
			TArray<FString> Parts;
			for (const FPickup::FEntry& E : Pk.Entries) Parts.Add(FString::Printf(TEXT("%s=%d"), *E.Item, E.Count));
			if (Parts.Num()) J->SetStringField(TEXT("contents"), FString::Join(Parts, TEXT(",")));
			// (co-op R7: the number both machines know it by, so a player joining late numbers it the same)
			if (Pk.CoopId >= 0) J->SetNumberField(TEXT("coop_id"), Pk.CoopId);
		}
		SetVec(*J, A->GetActorLocation());
		Ps.Add(MakeShared<FJsonValueObject>(J));
	}
	O->SetArrayField(TEXT("pickups"), Ps);
	O->SetNumberField(TEXT("placed_serials"), PickupSerials);      // (BUG-KNIFEPICKUP: the serials this build gave, the placed knives' among them)

	// props (their choreography: a shot lamp stays broken), glass, dynamite
	FJsonArray Pr, Gl, Dy;
	for (const FProp& Q : Props) Pr.Add(Num(Q.Sprite.IsValid() ? Q.Current : -1));
	for (int32 i = 0; i < Panes.Num(); ++i) if (Panes[i].bBroken) Gl.Add(Num(i));
	for (int32 i = 0; i < Dynamite.Num(); ++i) if (Dynamite[i].bGone) Dy.Add(Num(i));
	O->SetArrayField(TEXT("props"), Pr);
	O->SetArrayField(TEXT("glass"), Gl);
	O->SetArrayField(TEXT("dynamite"), Dy);
	FJsonArray Ex;
	for (int32 i = 0; i < ExplodeWalls.Num(); ++i) if (ExplodeWalls[i].bBlown) Ex.Add(Num(i));
	O->SetArrayField(TEXT("exploded"), Ex);

	const TSharedRef<FJsonObject> In = MakeShared<FJsonObject>();
	if (Inf) Inf->WriteState(*In);                     // (none: an empty block, which a load reads as nothing)
	O->SetObjectField(TEXT("inf"), In);
	WriteRandoSave(*O, false);                         // (R10: the run's randomizer and this level's roll, so a load builds the same)
	if (UOutlawsCoop::IsActive(this)) UOutlawsCoop::AddPlayer2(this, *O);      // (co-op: the other player's own state, which a solo load ignores)

	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutText);
	OutEnemies = Es.Num();
	OutPickups = Ps.Num();
	return FJsonSerializer::Serialize(O, Writer);
}

bool AOutlawsLevelActor::LoadGame(int32 Slot)
{
	if (UOutlawsCoop::IsActive(this))
	{
		return UOutlawsCoop::LoadInSession(this, Slot);     // (co-op: the host's load goes to both; the client's is the host's to make)
	}
	TSharedPtr<FJsonObject> O;
	if (!ReadJson(SavePath(Slot), O))
	{
		UE_LOG(LogOutlaws, Warning, TEXT("OLRT load: no save in %s"), *SavePath(Slot));
		return false;
	}
	if (O->GetStringField(TEXT("level")).IsEmpty())
	{
		return false;
	}
	NoteDeathSlot(Slot >= 0 ? Slot : -1);              // (BUG-DEATHAUTOSAVE: until the next autosave, a death loads this again)
	LoadGameFrom(O, Slot < 0 ? TEXT("autosave") : FString::Printf(TEXT("slot %d"), Slot));
	return true;
}

void AOutlawsLevelActor::LoadGameFrom(const TSharedPtr<FJsonObject>& O, const FString& What)
{
	const FString Lv = O->GetStringField(TEXT("level"));
	if (Movie)
	{
		Movie->Destroy();
		Movie = nullptr;
	}
	LevelEndTimer = -1.f;
	bBossDefeated = false;
	bHomBossDown = false;
	bLampOn = false;
	LevelName = Lv;
	bFrontEnd = false;
	bMovieToMenu = false;
	Difficulty = static_cast<EOutlawsDifficulty>(FMath::Clamp(int32(O->GetNumberField(TEXT("difficulty"))), 0, 2));
	// (its level built with the difficulty it was built with: the saved bad guys are matched one for one)
	PendingSpawnDifficulty = O->HasField(TEXT("spawn_difficulty")) ? FMath::Clamp(int32(O->GetNumberField(TEXT("spawn_difficulty"))), 0, 2) : int32(Difficulty);
	ReadRandoSave(*O);                         // (R10: the save's run and roll: the same bad guys and items as it was saved with)
	UE_LOG(LogOutlaws, Log, TEXT("OLRT load %s: %s"), *What, *Lv);
	NextRebuildKind = ERebuildKind::Load;      // (BUG-RESPAWNCRASH: a map load may build it in this one's place)
	MapCarryLoad = O;
	RestartLevelThen([this, O]()
	{
		// a load is no level start: no autosave over what was just loaded (an opening is one: ApplyLoad sets its autosave)
		StartSaveDelay = -1.f;
		AutosaveWait = 0.f;
		PendingLoad = O;
		SetActorTickEnabled(true);
	});
}

FString AOutlawsLevelActor::AutosaveDanger() const
{
	// not while in danger (user 2026-10-01): shot at or hurt in the last 8 s, a bad guy who is after the player and can see
	// him within 30 m, or health under 30 % (it comes back up to half, still, out of danger)
	const UWorld* W = GetWorld();
	if (!W) return FString();
	if (W->GetTimeSeconds() - LastDangerTime < 8.0) return TEXT("shot at or hurt lately");
	if (Health < 3.f) return TEXT("low health");
	const APawn* P = W->GetFirstPlayerController() ? W->GetFirstPlayerController()->GetPawn() : nullptr;
	if (!P) return FString();
	if (const AOutlawsPlayer* OP = Cast<AOutlawsPlayer>(P); OP && OP->IsHeadUnderwater()) return TEXT("under water");
	const FVector Me = P->GetActorLocation();
	for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : Enemies)
	{
		const AOutlawsEnemy* E = Wk.Get();
		if (!E || E->IsDead() || E->IsDormant() || E->bHeldByPlayer || E->GetState() == EOutlawsEnemyState::Watch) continue;
		if (FVector::Dist(E->GetActorLocation(), Me) > 3000.0) continue;
		if (CanSee(E->GetActorLocation() + FVector(0.0, 0.0, 40.0), Me, P)) return FString::Printf(TEXT("%s after the player"), *E->GetOutlawsName());
	}
	return FString();
}

void AOutlawsLevelActor::TickSaves(float Dt)
{
	OL_PERF_SCOPE("Level: saves");
	UWorld* W = GetWorld();
	AOutlawsPlayer* P = W && W->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(W->GetFirstPlayerController()->GetPawn()) : nullptr;
	if (PendingLoad.IsValid())
	{
		if (bNeedPlace || !P) return;
		const TSharedPtr<FJsonObject> O = PendingLoad;
		PendingLoad.Reset();
		ApplyLoad(*O);
		return;
	}
	const bool bPlaying = P && !bDead && !P->IsDeadView() && !IsMoviePlaying() && LevelEndTimer < 0.f && !bBossDefeated && !bNeedPlace && !bFrontEnd;
	if (!bPlaying)
	{
		return;
	}
	PlayTime += Dt;                     // world time: the menu (which all but stops the world) does not count
	const bool bSteady = !P->GetCharacterMovement()->IsFalling() && !P->IsMenuOpen();
	if (StartSaveDelay >= 0.f)
	{
		StartSaveDelay -= Dt;
		if (StartSaveDelay < 0.f)
		{
			if (!bSteady || !AutosaveDanger().IsEmpty()) StartSaveDelay = 0.5f;       // not mid-fall, nor in danger: in a moment
			else if (UOutlawsCoop::IsActive(this) && UOutlawsCoop::HoldStartAutosave(this)) StartSaveDelay = 0.5f;   // (R8: the partner's kit first)
			else if (bAutosave) SaveGame(-1);
			AutosaveWait = 0.f;
		}
		return;
	}
	if (!bAutosave)
	{
		return;
	}
	AutosaveWait += Dt;
	if (AutosaveWait >= FMath::Max(1.f, AutosaveMinutes) * 60.f && bSteady)
	{
		// due: saved once out of danger, then the timer starts again from there (user 2026-10-01)
		const FString Danger = AutosaveDanger();
		if (!Danger.IsEmpty())
		{
			if (!bAutosaveHeld) UE_LOG(LogOutlaws, Log, TEXT("OLRT autosave due, waiting: %s"), *Danger);
			bAutosaveHeld = true;
			return;
		}
		if (bAutosaveHeld) UE_LOG(LogOutlaws, Log, TEXT("OLRT autosave: out of danger after %.0f s more"), AutosaveWait - FMath::Max(1.f, AutosaveMinutes) * 60.f);
		bAutosaveHeld = false;
		AutosaveWait = 0.f;
		if (SaveGame(-1)) Notify(TEXT("Autosaved"), 1.5f);
	}
}

void AOutlawsLevelActor::ApplyLoad(const FJsonObject& O, bool bWorldOnly)
{
	TGuardValue<bool> Quiet(bRestoring, true);
	UWorld* W = GetWorld();
	AOutlawsPlayer* P = W && W->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(W->GetFirstPlayerController()->GetPawn()) : nullptr;

	if (O.HasField(TEXT("opening")) && O.GetBoolField(TEXT("opening")))
	{
		if (bWorldOnly) return;            // (an opening has no world of its own to take)
		// a level's opening (SaveLevelOpening): the level as Build left it, the carried kit on top of its start kit, as
		// StartNextLevel does when the story moves on; and from here it is this level's own start (its autosave follows)
		const bool bStripped = LevelName.Equals(TEXT("ranch"), ESearchCase::IgnoreCase);     // Bob's Ranch: fists only
		const TSharedPtr<FJsonObject>* Carry = nullptr;
		if (!bStripped && O.TryGetObjectField(TEXT("counts"), Carry))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Kv : (*Carry)->Values)
			{
				int32& N = Counts.FindOrAdd(Kv.Key);
				N = FMath::Max(N, int32(Kv.Value->AsNumber()));
			}
		}
		Gold = FMath::Max(Gold, int32(O.GetNumberField(TEXT("gold"))));
		Armour = bStripped ? 0 : FMath::Max(Armour, int32(O.GetNumberField(TEXT("armour"))));
		ArmourMax = FMath::Max(O.HasField(TEXT("armour_max")) ? int32(O.GetNumberField(TEXT("armour_max"))) : 0, Armour);
		PlayTime = O.GetNumberField(TEXT("playtime"));
		UOutlawsStats::NoteLoaded(this, 0.f);          // (REQ-LEVELTIMER: a level's opening is its start)
		OilBurn = float(O.GetNumberField(TEXT("oilburn")));
		if (P) P->ReadCarryMags(O);        // (BUG-HOLSTERRELOAD: the guns loaded from the carried ammunition before the entry kit is kept)
		EntryLevel = LevelName;
		EntryCounts = Counts;
		EntryGold = Gold;
		EntryArmour = Armour;
		EntryArmourMax = Armour > 0 ? ArmourMax : 0;        // (BUG-PLATERESTART: the plate carried in, at its size)
		StartSaveDelay = 1.5f;
		AutosaveWait = 0.f;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT load: the opening of %s, %d kinds of item carried in, gold %d, armour %d"), *LevelName, Counts.Num(), Gold, Armour);
		return;
	}

	// (co-op milestone 8: a client taking the host's save takes the world, never the host's player)
	if (!bWorldOnly)
	{
		Health = FMath::Clamp(float(O.GetNumberField(TEXT("health"))), 0.1f, 10.f);
		Armour = int32(O.GetNumberField(TEXT("armour")));
		// the plate's size (saves before 2026-10-01 have none: the smallest plate that holds what is left)
		ArmourMax = O.HasField(TEXT("armour_max")) ? int32(O.GetNumberField(TEXT("armour_max"))) : (Armour > 100 ? 150 : Armour > 50 ? 100 : Armour > 0 ? 50 : 0);
		ArmourMax = FMath::Max(ArmourMax, Armour);
		Gold = int32(O.GetNumberField(TEXT("gold")));
		Kills = int32(O.GetNumberField(TEXT("kills")));
		PlayTime = O.GetNumberField(TEXT("playtime"));
		UOutlawsStats::NoteLoaded(this, O.HasField(TEXT("level_seconds")) ? float(O.GetNumberField(TEXT("level_seconds"))) : -1.f);      // (REQ-LEVELTIMER)
		OilBurn = float(O.GetNumberField(TEXT("oilburn")));
		BadgeLeft = O.HasField(TEXT("badge")) ? float(O.GetNumberField(TEXT("badge"))) : 0.f;
		bDead = false;
		Counts.Reset();
		const TSharedPtr<FJsonObject>* C = nullptr;
		if (O.TryGetObjectField(TEXT("counts"), C))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Kv : (*C)->Values) Counts.Add(Kv.Key, int32(Kv.Value->AsNumber()));
		}
		// the kit this level was entered with: the save's own (always: one left by another game this session is not kept);
		// an older save has none: what it carries now, less the level's keys and puzzle pieces (as a level is entered)
		EntryCounts.Reset();
		if (O.HasField(TEXT("entry_level")))
		{
			EntryLevel = O.GetStringField(TEXT("entry_level"));
			const TSharedPtr<FJsonObject>* Ec = nullptr;
			if (O.TryGetObjectField(TEXT("entry_counts"), Ec))
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Kv : (*Ec)->Values) EntryCounts.Add(Kv.Key, int32(Kv.Value->AsNumber()));
			}
			EntryGold = O.HasField(TEXT("entry_gold")) ? int32(O.GetNumberField(TEXT("entry_gold"))) : 0;
			EntryArmour = O.HasField(TEXT("entry_armour")) ? int32(O.GetNumberField(TEXT("entry_armour"))) : 0;
			// (BUG-PLATERESTART) the entered plate's size; a save from before it has none: its restart keeps the size worn
			EntryArmourMax = O.HasField(TEXT("entry_armour_max")) ? int32(O.GetNumberField(TEXT("entry_armour_max"))) : -1;
		}
		else if (!LevelName.Equals(TEXT("ranch"), ESearchCase::IgnoreCase))        // (Bob's Ranch is always entered stripped)
		{
			EntryLevel = LevelName;
			for (const TPair<FString, int32>& Kv : Counts)
			{
				const FItemDef* D = ItemDefs.Find(Kv.Key);
				const FString Class = D ? D->Class.ToLower() : FString();
				if (!Class.Contains(TEXT("key")) && !Class.Contains(TEXT("puzz"))) EntryCounts.Add(Kv.Key, Kv.Value);
			}
			EntryGold = int32(O.GetNumberField(TEXT("gold")));
			EntryArmour = int32(O.GetNumberField(TEXT("armour")));
			EntryArmourMax = -1;                // (BUG-PLATERESTART: as such a save's restart always was)
		}
		else
		{
			EntryLevel.Reset();
		}
		Inventory.Reset();
		InventoryLabels.Reset();
		if (const FJsonArray* A = Array(O, TEXT("inventory"))) for (const TSharedPtr<FJsonValue>& V : *A) Inventory.Add(V->AsString());
		if (const FJsonArray* A = Array(O, TEXT("labels"))) for (const TSharedPtr<FJsonValue>& V : *A) InventoryLabels.Add(V->AsString());
	}

	// the bad guys, matched by spawn order (and name, in case the level's objects changed since).  (Co-op, 2026-10-02: a
	// session's extra bad guys follow the level's own in a co-op save, each marked with his place in the host's list
	// ("extra"), his kind ("kind") and the bad guy he stands by ("beside"); each goes back onto this build's extra at the same
	// place in the list when that one is of the same kind beside the same bad guy, or nowhere: single player has none, so it
	// leaves them out, and a save from single player leaves a session's extras as they were made, alive at their places.
	// An older co-op save's extras were copies ("copy_of", before the user's 2026-10-02 kinds): left out, the extras fresh)
	int32 Put = 0, CopiesPut = 0, CopiesOut = 0, CopiesOutDead = 0, OlderOut = 0;
	if (const FJsonArray* A = Array(O, TEXT("enemies")))
	{
		TMap<int32, AOutlawsEnemy*> ExtraAt;
		for (const TWeakObjectPtr<AOutlawsEnemy>& Extra : Enemies)
		{
			if (Extra.IsValid() && Extra->CoopCopyOf >= 0 && Extra->CoopExtraIndex >= 0) ExtraAt.Add(Extra->CoopExtraIndex, Extra.Get());
		}
		for (int32 i = 0; i < A->Num(); ++i)
		{
			const TSharedPtr<FJsonObject> J = (*A)[i]->AsObject();
			const bool bOlder = J.IsValid() && J->HasField(TEXT("copy_of")) && !J->HasField(TEXT("extra"));
			if (J.IsValid() && (J->HasField(TEXT("extra")) || bOlder))
			{
				AOutlawsEnemy* E = bOlder ? nullptr : ExtraAt.FindRef(int32(J->GetNumberField(TEXT("extra"))));
				if (E && J->GetStringField(TEXT("kind")).Equals(E->GetOutlawsName(), ESearchCase::IgnoreCase) && J->HasField(TEXT("beside"))
					&& int32(J->GetNumberField(TEXT("beside"))) == E->CoopCopyOf)
				{
					if (DeferEnemyReads) DeferEnemyReads->Emplace(E, J); else E->ReadSave(*J);
					++Put;
					++CopiesPut;
				}
				else
				{
					++CopiesOut;
					OlderOut += bOlder ? 1 : 0;
					if (J->HasField(TEXT("dead")) && J->GetBoolField(TEXT("dead"))) ++CopiesOutDead;
				}
				continue;
			}
			if (i >= Enemies.Num()) continue;
			AOutlawsEnemy* E = Enemies[i].Get();
			if (!E || E->CoopCopyOf >= 0 || !J.IsValid() || !J->HasField(TEXT("name")) || J->GetStringField(TEXT("name")) != E->GetOutlawsName()) continue;
			// (Q2 step 4: a late join puts them back a few a frame, UOutlawsCoop::DrainJoinReplay)
			if (DeferEnemyReads) DeferEnemyReads->Emplace(E, J); else E->ReadSave(*J);
			++Put;
		}
	}
	// the kills the save counted include the copies left out that were dead: not here, not counted (the late boss comes at a
	// share of the bad guys there are)
	if (!bWorldOnly && CopiesOutDead > 0)
	{
		const int32 KillsWas = Kills;
		Kills = FMath::Max(0, Kills - CopiesOutDead);
		UE_LOG(LogOutlaws, Log, TEXT("%s load: %d extra bad guys of a co-op game left out (%d of them dead: kills %d -> %d)"),
			UOutlawsCoop::IsActive(this) ? TEXT("OLCOOP") : TEXT("OLRT"), CopiesOut, CopiesOutDead, KillsWas, Kills);
	}
	else if (CopiesOut > 0)
	{
		UE_LOG(LogOutlaws, Log, TEXT("%s load: %d extra bad guys of a co-op game left out (none dead)"), UOutlawsCoop::IsActive(this) ? TEXT("OLCOOP") : TEXT("OLRT"), CopiesOut);
	}
	if (OlderOut > 0)
	{
		UE_LOG(LogOutlaws, Log, TEXT("%s load: %d of them from an older co-op save (copies of the bad guys they stood by): left out, the extra bad guys made fresh"),
			UOutlawsCoop::IsActive(this) ? TEXT("OLCOOP") : TEXT("OLRT"), OlderOut);
	}
	if (UOutlawsCoop::IsActive(this) && CoopExtrasMade > 0)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLCOOP extra bad guys after the load: %d put back from the save, %d fresh at their places"), CopiesPut, CoopExtrasMade - CopiesPut);
	}

	// pickups: the level's own that are gone are removed, the rest go where they were; dropped ones come back
	TMap<int32, FVector> Keep;
	TArray<TPair<FString, FVector>> Dropped;
	TArray<FString> DroppedContents;
	TArray<bool> DroppedSeen;
	TSet<int32> SeenSerials;            // (REQ-MAPITEMS: the items on the map; a co-op client taking the host's world keeps its own)
	int32 LevelSticks = 0;
	if (const FJsonArray* A = Array(O, TEXT("pickups")))
	{
		for (const TSharedPtr<FJsonValue>& V : *A)
		{
			const TSharedPtr<FJsonObject> J = V->AsObject();
			if (!J.IsValid()) continue;
			const int32 Serial = int32(J->GetNumberField(TEXT("serial")));
			bool bSeen = false;
			J->TryGetBoolField(TEXT("map_seen"), bSeen);
			bSeen &= !bWorldOnly;
			if (Serial >= 0 && bSeen) SeenSerials.Add(Serial);
			if (Serial >= 0) Keep.Add(Serial, GetVec(*J));
			else if (IsStickGround(J->GetStringField(TEXT("ground"))))
			{
				++LevelSticks;      // (an older save's copy of the level's own stick: the build has made it already)
			}
			else
			{
				Dropped.Emplace(J->GetStringField(TEXT("ground")), GetVec(*J));
				FString Contents;
				J->TryGetStringField(TEXT("contents"), Contents);
				DroppedContents.Add(Contents);
				DroppedSeen.Add(bSeen);
			}
		}
	}
	// (BUG-KNIFEPICKUP) a save from before the level's knives were pickups names the sprites' serials only: the knives (numbered
	// after them) it never knew stay as the build made them
	double SavedSerials = double(ModelSerialsFrom);
	O.TryGetNumberField(TEXT("placed_serials"), SavedSerials);
	int32 NotInSave = 0;
	for (int32 i = Pickups.Num() - 1; i >= 0; --i)
	{
		FPickup& Pk = Pickups[i];
		if (Pk.Serial < 0) continue;
		if (Pk.Serial >= int32(SavedSerials)) { ++NotInSave; continue; }
		AActor* A = Pk.Actor.Get();
		if (const FVector* Where = Keep.Find(Pk.Serial))
		{
			if (A) A->SetActorLocation(*Where);
			if (!bWorldOnly) Pk.bMapSeen = SeenSerials.Contains(Pk.Serial);
			continue;
		}
		if (A) A->Destroy();
		Pickups.RemoveAt(i);
	}
	if (NotInSave > 0) UE_LOG(LogOutlaws, Log, TEXT("OLRT save put back: %d of the level's own pickups (serials from %d) are not in this older save: left as built"), NotInSave, int32(SavedSerials));
	for (int32 d = 0; d < Dropped.Num(); ++d)
	{
		if (bDeferDrops)
		{
			DeferredDrops.Add({ Dropped[d].Key, Dropped[d].Value, DroppedContents[d] });     // (Q2 step 4: a late join's, a few a frame)
			continue;
		}
		AActor* Made = SpawnGroundItem(Dropped[d].Key, Dropped[d].Value);
		if (Made && !DroppedContents[d].IsEmpty()) UOutlawsCoop::ApplyGroundContents(this, Made, DroppedContents[d]);   // (as it was dropped)
		if (Made && DroppedSeen[d])
		{
			for (FPickup& Pk : Pickups) if (Pk.Actor.Get() == Made) Pk.bMapSeen = true;
		}
	}
	if (LevelSticks > 0) UE_LOG(LogOutlaws, Log, TEXT("OLRT save put back: %d of the level's own sticks of dynamite listed as dropped, left to the build"), LevelSticks);

	// props, glass, dynamite
	if (const FJsonArray* A = Array(O, TEXT("props")))
	{
		for (int32 i = 0; i < FMath::Min(A->Num(), Props.Num()); ++i)
		{
			const int32 Cur = int32((*A)[i]->AsNumber());
			if (Cur < 0)
			{
				if (AOutlawsSpriteActor* S = Props[i].Sprite.Get()) S->Destroy();
				continue;
			}
			if (Cur != Props[i].Current)
			{
				PropEnter(i, Cur);
				if (AOutlawsSpriteActor* S = Props[i].Sprite.Get()) S->FinishChor();
			}
		}
	}
	if (const FJsonArray* A = Array(O, TEXT("glass"))) for (const TSharedPtr<FJsonValue>& V : *A) { const int32 i = int32(V->AsNumber()); if (Panes.IsValidIndex(i)) ShatterPane(i); }
	if (const FJsonArray* A = Array(O, TEXT("exploded")))
	{
		for (const TSharedPtr<FJsonValue>& V : *A)
		{
			const int32 i = int32(V->AsNumber());
			if (ExplodeWalls.IsValidIndex(i) && !ExplodeWalls[i].bBlown) BlowExplodeWall(ExplodeWalls[i], true, FVector::ZeroVector);
		}
	}
	if (const FJsonArray* A = Array(O, TEXT("dynamite")))
	{
		for (const TSharedPtr<FJsonValue>& V : *A)
		{
			const int32 i = int32(V->AsNumber());
			if (!Dynamite.IsValidIndex(i)) continue;
			Dynamite[i].bGone = true;
			if (UProceduralMeshComponent* M = Dynamite[i].Mesh.Get())
			{
				if (AActor* Own = M->GetOwner(); Own && Own != this) Own->Destroy(); else M->DestroyComponent();
				Dynamite[i].Mesh = nullptr;
			}
		}
	}

	// the INF (after the counts: its key items are the saved ones)
	const TSharedPtr<FJsonObject>* In = nullptr;
	if (Inf && O.TryGetObjectField(TEXT("inf"), In))
	{
		if (bWorldOnly)
		{
			const TSet<int32> OwnKeys = Inf->Items;      // (co-op: the saved keys were the host's; this player keeps its own)
			Inf->ReadState(**In);
			Inf->Items = OwnKeys;
		}
		else
		{
			Inf->ReadState(**In);
		}
		// (REQ-PIANO: a tune's move keeps the tune's length; a save from before it holds the INF's own speed)
		for (const TPair<int32, double>& Kv : TuneSpeeds)
		{
			if (Inf->Elevators.IsValidIndex(Kv.Key)) Inf->Elevators[Kv.Key].Speed = Kv.Value;
		}
	}

	// the player last (the lifts under their feet are where they were)
	const TSharedPtr<FJsonObject>* Pl = nullptr;
	if (!bWorldOnly && P && O.TryGetObjectField(TEXT("player"), Pl)) P->ReadSave(**Pl);
	if (!bWorldOnly) bLampOn = O.GetBoolField(TEXT("lamp")) && GetCount(TEXT("ioil")) > 0;
	SetActorTickEnabled(true);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT save put back: %s, health %.1f, %d bad guys, %d pickups, %d dropped, kills %d"),
		*LevelName, Health, Put, Pickups.Num(), Dropped.Num(), Kills);
}

// ---- INF ---------------------------------------------------------------------------------------------

void FOutlawsInfRuntime::WriteState(FJsonObject& O) const
{
	FJsonArray Es, Ts, Ss, Is;
	for (const FOutlawsElevator& E : Elevators)
	{
		const TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetNumberField(TEXT("v"), E.Current);
		J->SetNumberField(TEXT("speed"), E.Speed);
		J->SetNumberField(TEXT("stop"), E.Stop);
		J->SetNumberField(TEXT("target"), E.Target);
		J->SetNumberField(TEXT("wait"), E.WaitLeft);
		J->SetBoolField(TEXT("waiting"), E.bWaiting);
		J->SetBoolField(TEXT("term"), E.bTerminated);
		J->SetBoolField(TEXT("on"), E.bMasterOn);
		Es.Add(MakeShared<FJsonValueObject>(J));
	}
	for (const FOutlawsTrigger& T : Triggers) Ts.Add(MakeShared<FJsonValueBoolean>(T.bMaster));
	for (const FOutlawsScroll& S : Scrolls)
	{
		Ss.Add(Num(S.Offset.X));
		Ss.Add(Num(S.Offset.Y));
	}
	for (const int32 Id : Items) Is.Add(Num(Id));
	O.SetArrayField(TEXT("elevators"), Es);
	O.SetArrayField(TEXT("triggers"), Ts);
	O.SetArrayField(TEXT("scrolls"), Ss);
	O.SetArrayField(TEXT("items"), Is);
}

void FOutlawsInfRuntime::ReadState(const FJsonObject& O)
{
	// only when the level's INF is the one saved (same counts); the items always.  A save from before the piano's gating
	// light (townex, town: RC2, RC4) holds every elevator but that one, which comes last: those come back by index, the
	// light keeps its start
	// (REQ-SECTORLIGHT) and a save from before the sector lights (shsimms, simms, cliff, shgranry, hideout) holds every
	// elevator before them: those come back, the lights run from their start
	const FJsonArray* Es = Array(O, TEXT("elevators"));
	const bool bOldLayout = Es && Es->Num() != Elevators.Num() && SavedElevatorsBefore > 0 && Es->Num() == SavedElevatorsBefore;
	const bool bBeforeLights = Es && !bOldLayout && LightsFrom > 0 && Es->Num() < Elevators.Num() && Es->Num() == LightsFrom;
	if (bOldLayout || bBeforeLights)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT save: %d elevators saved before the %s (%d now): those put back, the rest at their start"),
			Es->Num(), bOldLayout ? TEXT("piano's light") : TEXT("sector lights"), Elevators.Num());
	}
	if (Es && (Es->Num() == Elevators.Num() || bOldLayout || bBeforeLights))
	{
		for (int32 i = 0; i < Es->Num(); ++i)
		{
			const TSharedPtr<FJsonObject> J = (*Es)[i]->AsObject();
			if (!J.IsValid()) continue;
			FOutlawsElevator& E = Elevators[i];
			E.Current = J->GetNumberField(TEXT("v"));
			E.Speed = J->GetNumberField(TEXT("speed"));
			E.Stop = int32(J->GetNumberField(TEXT("stop")));
			E.Target = int32(J->GetNumberField(TEXT("target")));
			E.WaitLeft = J->GetNumberField(TEXT("wait"));
			E.bWaiting = J->GetBoolField(TEXT("waiting"));
			E.bTerminated = J->GetBoolField(TEXT("term"));
			E.bMasterOn = J->GetBoolField(TEXT("on"));
			Apply(E);
		}
	}
	if (const FJsonArray* A = Array(O, TEXT("triggers")); A && A->Num() == Triggers.Num())
	{
		for (int32 i = 0; i < Triggers.Num(); ++i) Triggers[i].bMaster = (*A)[i]->AsBool();
	}
	if (const FJsonArray* A = Array(O, TEXT("scrolls")); A && A->Num() == Scrolls.Num() * 2)
	{
		for (int32 i = 0; i < Scrolls.Num(); ++i) Scrolls[i].Offset = FVector2D((*A)[2 * i]->AsNumber(), (*A)[2 * i + 1]->AsNumber());
	}
	if (const FJsonArray* A = Array(O, TEXT("items")))
	{
		Items.Reset();
		for (const TSharedPtr<FJsonValue>& V : *A) Items.Add(int32(V->AsNumber()));
	}
}

// ---- enemies -----------------------------------------------------------------------------------------

void AOutlawsEnemy::WriteSave(FJsonObject& O) const
{
	O.SetStringField(TEXT("name"), OutlawsName);
	SetVec(O, GetActorLocation());
	O.SetNumberField(TEXT("yaw"), Heading);
	O.SetNumberField(TEXT("health"), Health);
	O.SetNumberField(TEXT("health_of"), Def.Health);          // (its full health at the difficulty then: a load at another keeps the share)
	O.SetBoolField(TEXT("dead"), IsDead());
	if (bTakenAlive) O.SetBoolField(TEXT("taken_alive"), true);        // (REQ-HMBOSSEND: a mission's outlaw punched out: captured at the horse after a load)
	O.SetBoolField(TEXT("dormant"), bDormant);
	O.SetBoolField(TEXT("alert"), !IsDead() && State != EOutlawsEnemyState::Watch);
	if (bPeekShooter) O.SetBoolField(TEXT("behind_door"), bBehindDoor);
	if (CoopCopyOf >= 0)
	{
		// (co-op: an extra bad guy, after the level's own: his place in the host's list, his kind (user 2026-10-02) and the
		// bad guy he stands by; a co-op load matches all three)
		O.SetNumberField(TEXT("extra"), CoopExtraIndex);
		O.SetStringField(TEXT("kind"), OutlawsName.ToLower());
		O.SetNumberField(TEXT("beside"), CoopCopyOf);
	}
}

void AOutlawsEnemy::ReadSave(const FJsonObject& O)
{
	const bool bWasDormant = O.GetBoolField(TEXT("dormant"));
	if (bWasDormant != bDormant) SetDormant(bWasDormant);
	Heading = float(O.GetNumberField(TEXT("yaw")));
	// (hurt as much as it was: the difficulty chosen may have changed since, and its full health with it)
	const float Of = O.HasField(TEXT("health_of")) ? float(O.GetNumberField(TEXT("health_of"))) : 0.f;
	Health = Of > 0.f ? float(O.GetNumberField(TEXT("health"))) / Of * Def.Health : FMath::Min(float(O.GetNumberField(TEXT("health"))), Def.Health);
	if (O.GetBoolField(TEXT("dead")))
	{
		// a body where it fell: the death played out, no drop (what it dropped is saved as a pickup), no report
		SetBodyHalf(StandHalf);
		State = EOutlawsEnemyState::Dead;
		StateTime = 1000.f;
		bDropped = true;
		bBossReported = true;
		bTakenAlive = O.HasField(TEXT("taken_alive")) && O.GetBoolField(TEXT("taken_alive"));
		bFallCried = true;
		bKnockFlight = false;
		Play(Def.ChestDieChor, false);
		if (Sprite) Sprite->FinishChor();
		UCapsuleComponent* C = GetCapsuleComponent();
		C->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
		C->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
	}
	SetActorLocation(GetVec(O), false, nullptr, ETeleportType::TeleportPhysics);
	GetCharacterMovement()->StopMovementImmediately();
	// Bob: still behind his door, or let go (a save from before the field: alerted means the door had opened)
	if (bPeekShooter) bBehindDoor = O.HasField(TEXT("behind_door")) ? O.GetBoolField(TEXT("behind_door")) : bBehindDoor && !O.GetBoolField(TEXT("alert"));
	if (!IsDead() && !bDormant && O.GetBoolField(TEXT("alert")))
	{
		Wake(false);
	}
}

// ---- player ------------------------------------------------------------------------------------------

void AOutlawsPlayer::WriteSave(FJsonObject& O) const
{
	SetVec(O, GetActorLocation());
	O.SetNumberField(TEXT("half"), GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight());     // (BUG-WHARFPLANK: the feet are put back, ReadSave)
	O.SetNumberField(TEXT("yaw"), Controller ? Controller->GetControlRotation().Yaw : GetActorRotation().Yaw);
	O.SetNumberField(TEXT("stamina"), Stamina);
	FJsonArray Hands;
	TMap<int32, int32> Mags = SavedMag;
	for (int32 H = 0; H < 2; ++H)
	{
		const TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetNumberField(TEXT("weapon"), Equipped[H]);
		J->SetNumberField(TEXT("mag"), Mag[H]);
		J->SetStringField(TEXT("tool"), HeldTool[H]);
		Hands.Add(MakeShared<FJsonValueObject>(J));
		if (Equipped[H] >= 0) Mags.Add(H * 16 + Equipped[H], Mag[H]);
	}
	O.SetArrayField(TEXT("hands"), Hands);
	const TSharedRef<FJsonObject> M = MakeShared<FJsonObject>();
	for (const TPair<int32, int32>& Kv : Mags) M->SetNumberField(FString::FromInt(Kv.Key), Kv.Value);
	O.SetObjectField(TEXT("mags"), M);
	// (BUG-HOLSTERRELOAD) the guns' rounds as this level was entered: a restart after this save is loaded gives them back
	const TSharedRef<FJsonObject> E = MakeShared<FJsonObject>();
	for (const TPair<int32, int32>& Kv : EntryMags) E->SetNumberField(FString::FromInt(Kv.Key), Kv.Value);
	O.SetObjectField(TEXT("entry_mags"), E);
}

void AOutlawsPlayer::ReadSave(const FJsonObject& O)
{
	const FRotator Rot(0.0, O.GetNumberField(TEXT("yaw")), 0.0);
	// (BUG-WHARFPLANK) the feet where they were: a save made crouched (or by a shorter VR body) put the middle of a taller
	// body where the short one's was, its feet under the floor, and it fell through (a save from before the field: as it was)
	FVector At = GetVec(O);
	double SavedHalf = 0.0;
	if (O.TryGetNumberField(TEXT("half"), SavedHalf) && SavedHalf > 1.0) At.Z += GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() - SavedHalf;
	TeleportTo(At, Rot, false, true);
	if (Controller) Controller->SetControlRotation(Rot);
	GetCharacterMovement()->StopMovementImmediately();
	GrantFallGrace(GetWorld()->GetTimeSeconds() + 3.0);
	Stamina = FMath::Clamp(float(O.GetNumberField(TEXT("stamina"))), 0.f, 100.f);
	// (a gun drawn as the level was placed, the hideout's revolver, put away first: its rounds are not written over the save's)
	for (int32 H = 0; H < 2; ++H) if (Equipped[H] >= 0) Equip(H, -1);
	SavedMag.Reset();
	const TSharedPtr<FJsonObject>* M = nullptr;
	if (O.TryGetObjectField(TEXT("mags"), M))
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Kv : (*M)->Values) SavedMag.Add(FCString::Atoi(*Kv.Key), int32(Kv.Value->AsNumber()));
	}
	// (BUG-HOLSTERRELOAD) the entry's rounds for a restart; a save from before them: the rounds it holds now
	EntryMags = SavedMag;
	const TSharedPtr<FJsonObject>* E = nullptr;
	if (O.TryGetObjectField(TEXT("entry_mags"), E) && E && E->IsValid())
	{
		EntryMags.Reset();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Kv : (*E)->Values) EntryMags.Add(FCString::Atoi(*Kv.Key), int32(Kv.Value->AsNumber()));
	}
	if (const FJsonArray* A = Array(O, TEXT("hands")))
	{
		for (int32 H = 0; H < FMath::Min(2, A->Num()); ++H)
		{
			const TSharedPtr<FJsonObject> J = (*A)[H]->AsObject();
			if (!J.IsValid()) continue;
			const int32 Wp = int32(J->GetNumberField(TEXT("weapon")));
			if (Wp >= 0) Equip(H, Wp);
			const FString Tool = J->GetStringField(TEXT("tool"));
			if (!Tool.IsEmpty()) EquipTool(H, Tool);
		}
	}
}
