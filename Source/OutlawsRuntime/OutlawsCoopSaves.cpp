// Story co-op (tools/olue/COOP_DESIGN.md), milestone 8: saves.  Only the host saves, in the same format as single player (user
// 2026-10-02: "co op saves should perhaps be interchangeable"): the pause menu's Save Game writes the slot the host picks, so a
// game started in either can be carried on in the other; since R7 (user 2026-10-03) the host's autosave is a file of its own,
// coop_autosave.json, never single player's autosave.json.  Each co-op save carries
// the client's own state as a "player2" block, which a single-player load ignores.  A load in a session (the host's pause
// menu, or the lobby's Continue row) goes to both: each machine builds the save's level from its own files,
// the host puts the save back as single player does, and the client takes the save's world (bad guys, pickups, props,
// glass, walls, dynamite, doors) from the save's JSON, sent to it in parts, and its own player from "player2" (or, from a
// save without one, a copy of the host's weapons and ammo and no keys), standing beside the host.
//
// None of this runs in single player: the save code calls in only when UOutlawsCoop::IsActive; the single-player load test
// (-OLLoadTest=<slot>) only reports.
#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsEnemy.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsInf.h"
#include "OutlawsMovieScreen.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	constexpr int32 SnapPartChars = 8000;          // a reliable call carries one part (the engine's limit is 64 KB)
	constexpr int32 RestoredIdBase = 90000;        // things dropped before the save, made again by the load (both machines in its order)

	FString SlotName(int32 Slot)
	{
		return Slot == -2 ? FString(TEXT("coop_autosave")) : Slot < 0 ? FString(TEXT("autosave")) : FString::Printf(TEXT("slot%d"), Slot);
	}

	bool ParseJson(const FString& Text, TSharedPtr<FJsonObject>& Out)
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, Out) && Out.IsValid();
	}

	FVector JsonVec(const FJsonObject& O)
	{
		return FVector(O.GetNumberField(TEXT("x")), O.GetNumberField(TEXT("y")), O.GetNumberField(TEXT("z")));
	}
}

// ---- the link's milestone 8 messages ----------------------------------------------------------------------------------

void AOutlawsCoopLink::ServerPlayer2_Implementation(const FString& Json)
{
	if (!Admit(EOLGate::Player2, Json.Len() <= 65536)) return;
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnPlayer2(PlayerIndex, Json);
}

void AOutlawsCoopLink::ClientSnapshotPart_Implementation(int32 Epoch, int32 Part, int32 Parts, const FString& Text)
{
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnSnapshotPart(Epoch, Part, Parts, Text);
}

// ---- saving (the host) -------------------------------------------------------------------------------------------------

bool UOutlawsCoop::MaySave(AOutlawsLevelActor* L, int32& Slot)
{
	UOutlawsCoop* C = Get(L);
	if (!C || !C->IsHost()) return false;          // (the client's game is the host's to save)
	// R7 (user 2026-10-03: "an autosave should still work. In a separate co-op autosave"): the host's autosave goes to
	// coop_autosave.json, never single player's autosave.json; Save Game's slots as before
	if (Slot == -1) Slot = -2;
	return true;
}

bool UOutlawsCoop::HoldStartAutosave(AOutlawsLevelActor* L)
{
	UOutlawsCoop* C = Get(L);
	if (!C || !C->IsHost() || !C->bLevelBuilt) return false;
	const AOutlawsCoopSession* S = C->GetSession();
	if (!S || !S->bClientConnected) return false;
	if (FPlatformTime::Seconds() - C->LevelBuiltAt > 10.0) return false;
	// (N1: every connected client's level and kit)
	bool bWait = false;
	C->ForClients([&](AOutlawsCoopLink& K)
	{
		bWait = bWait || K.BuiltSerial != C->StartedSerial || !C->PlayerJsons.Contains(C->PlayerKey(K.PlayerIndex));
	});
	if (bWait && !C->bAutosaveHoldLogged)
	{
		C->bAutosaveHoldLogged = true;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the start autosave waits for the partner's level and kit"));
	}
	return bWait;
}

void UOutlawsCoop::AddPlayer2(AOutlawsLevelActor* L, FJsonObject& O)
{
	UOutlawsCoop* C = Get(L);
	// (R14: the bad guys' multiplier this game was made with, so a load keeps it)
	if (C && C->IsHost())
	{
		if (const AOutlawsCoopSession* S = C->GetSession()) O.SetNumberField(TEXT("coop_badguys"), S->Multiplier);
	}
	if (!C || !C->IsHost()) return;
	// "player2": the first client's block, as two-player saves always had it (a single-player load ignores it)
	const FString First = C->PlayerJsonFor(C->PartnerIndex());
	TSharedPtr<FJsonObject> P2;
	if (!First.IsEmpty() && ParseJson(First, P2)) O.SetObjectField(TEXT("player2"), P2);
	// N1: "players": every client's block by its key (its name, else "#<number>"), those here and those a loaded save carried
	// for players not here now, kept for when they come back
	TArray<TSharedPtr<FJsonValue>> All;
	TArray<FString> Keys;
	C->PlayerJsons.GetKeys(Keys);
	Keys.Sort();
	for (const FString& Key : Keys)
	{
		TSharedPtr<FJsonObject> B;
		if (!ParseJson(C->PlayerJsons[Key], B)) continue;
		const TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("key"), Key);
		Entry->SetObjectField(TEXT("state"), B);
		All.Add(MakeShared<FJsonValueObject>(Entry));
	}
	if (All.Num() > 0) O.SetArrayField(TEXT("players"), All);
}

// ---- N1: every client's block, by name ---------------------------------------------------------------------------------

FString UOutlawsCoop::PlayerKey(int32 Who) const
{
	// the player's name (lower case, so a save from one session finds it in the next), unless another player here has the
	// same one; else its number
	const AOutlawsCoopLink* K = GetLink(Who);
	const FString Name = K ? K->PlayerName.TrimStartAndEnd().ToLower() : FString();
	if (!Name.IsEmpty())
	{
		TArray<AOutlawsCoopLink*> Links;
		GetLinks(Links);
		int32 Same = 0;
		for (const AOutlawsCoopLink* O : Links) Same += O->PlayerIndex > 0 && O->PlayerName.TrimStartAndEnd().ToLower() == Name ? 1 : 0;
		if (Same <= 1) return TEXT("name:") + Name;
	}
	return FString::Printf(TEXT("#%d"), Who);
}

FString UOutlawsCoop::PlayerJsonFor(int32 Who) const
{
	if (Who <= 0) return FString();
	if (const FString* J = PlayerJsons.Find(PlayerKey(Who))) return *J;
	if (const FString* J = PlayerJsons.Find(FString::Printf(TEXT("#%d"), Who))) return *J;      // (a save from before names, by number)
	return Who == 1 ? Player2Json : FString();
}

TSharedPtr<FJsonObject> UOutlawsCoop::SaveBlockFor(const FJsonObject& Save, int32 Who) const
{
	// a save's block for player Who: by its key in "players", else its number there, else (player 1, or a save from before
	// N1) "player2"
	const FString Key = PlayerKey(Who), ByNumber = FString::Printf(TEXT("#%d"), Who);
	const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
	if (Save.TryGetArrayField(TEXT("players"), A))
	{
		TSharedPtr<FJsonObject> Numbered;
		for (const TSharedPtr<FJsonValue>& V : *A)
		{
			const TSharedPtr<FJsonObject> E = V.IsValid() ? V->AsObject() : nullptr;
			const TSharedPtr<FJsonObject>* St = nullptr;
			if (!E.IsValid() || !E->TryGetObjectField(TEXT("state"), St)) continue;
			const FString K = E->GetStringField(TEXT("key"));
			if (K == Key) return *St;
			if (K == ByNumber) Numbered = *St;
		}
		if (Numbered.IsValid()) return Numbered;
		return nullptr;                          // (an N1 save that has no block for this player: the start kit)
	}
	const TSharedPtr<FJsonObject>* P2 = nullptr;
	return Who == 1 && Save.TryGetObjectField(TEXT("player2"), P2) ? *P2 : nullptr;
}

FString UOutlawsCoop::SaveTextFor(const FString& Text, int32 Who) const
{
	// the save as player Who's machine wants it: its own block as "player2" (none: no "player2", so it takes the save's
	// no-player2 kit), the others' left out
	TSharedPtr<FJsonObject> O;
	if (!ParseJson(Text, O)) return Text;
	TSharedPtr<FJsonObject> Mine;
	const FString Heard = PlayerJsonFor(Who);
	if (!Heard.IsEmpty()) ParseJson(Heard, Mine);
	if (!Mine.IsValid()) Mine = SaveBlockFor(*O, Who);
	O->RemoveField(TEXT("players"));
	if (Mine.IsValid()) O->SetObjectField(TEXT("player2"), Mine);
	else O->RemoveField(TEXT("player2"));
	FString Out;
	FJsonSerializer::Serialize(O.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out));
	return Out;
}

void UOutlawsCoop::NoteSaved(AOutlawsLevelActor* L, int32 Slot, int32 Bytes)
{
	UOutlawsCoop* C = Get(L);
	if (!C) return;
	++C->SavesWritten;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP save written: %s %s (%d bytes, level %s)"), *SlotName(Slot), C->PlayerJsonFor(C->PartnerIndex()).IsEmpty() ? TEXT("without player2 (nothing heard from the client yet)") : TEXT("with player2"),
		Bytes, *L->LevelName);
	if (C->PlayerJsons.Num() > 1 || (C->GetSession() && C->GetSession()->Connected > 2))
	{
		TArray<FString> Keys;
		C->PlayerJsons.GetKeys(Keys);
		Keys.Sort();
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP save written: %s keeps %d players' blocks: %s"), *SlotName(Slot), Keys.Num(), *FString::Join(Keys, TEXT(", ")));
	}
}

void UOutlawsCoop::OnPlayer2(int32 Who, const FString& Json)
{
	if (!IsHost() || Who <= 0 || Who >= MaxPlayers) return;
	// (N1: each client's own, by its name; the first client's also as the old "player2")
	const FString Key = PlayerKey(Who);
	if (!PlayerJsons.Contains(Key)) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the %s player's own state heard (kept as '%s')"), WhoWord(Who), *Key);
	PlayerJsons.Add(Key, Json);
	if (Who == PartnerIndex()) Player2Json = Json;
}

// ---- loading in a session ----------------------------------------------------------------------------------------------

bool UOutlawsCoop::LoadInSession(AOutlawsLevelActor* L, int32 Slot)
{
	UOutlawsCoop* C = Get(L);
	if (!C) return false;
	if (!C->IsHost())
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the client's load of %s refused: the host's to load"), *SlotName(Slot));
		L->Notify(TEXT("In co-op the host loads a save"), 2.5f);
		return false;
	}
	return C->RequestLoad(Slot);
}

bool UOutlawsCoop::RequestLoad(int32 Slot)
{
	AOutlawsCoopSession* S = GetSession();
	if (!IsHost() || !S) return false;
	FString Text;
	TSharedPtr<FJsonObject> O;
	if (!FFileHelper::LoadFileToString(Text, *AOutlawsLevelActor::SavePath(Slot)) || !ParseJson(Text, O) || O->GetStringField(TEXT("level")).IsEmpty())
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP load of %s: no save there"), *SlotName(Slot));
		return false;
	}
	const FString Lv = O->GetStringField(TEXT("level")).ToLower();
	const int32 Diff = FMath::Clamp(int32(O->GetNumberField(TEXT("difficulty"))), 0, 2);
	double SavedMult = 0.0;
	LoadMultiplier = O->TryGetNumberField(TEXT("coop_badguys"), SavedMult) ? float(SavedMult) : 0.f;     // (R14)
	SnapObj = O;
	// N1: the save's blocks for every client (those not here wait for them: a save loaded with fewer players keeps them)
	PlayerJsons.Reset();
	Player2Json.Reset();
	{
		const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
		const TSharedPtr<FJsonObject>* P2 = nullptr;
		if (O->TryGetArrayField(TEXT("players"), A))
		{
			for (const TSharedPtr<FJsonValue>& V : *A)
			{
				const TSharedPtr<FJsonObject> E = V.IsValid() ? V->AsObject() : nullptr;
				const TSharedPtr<FJsonObject>* St = nullptr;
				FString B;
				if (!E.IsValid() || !E->TryGetObjectField(TEXT("state"), St)) continue;
				FJsonSerializer::Serialize(St->ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&B));
				PlayerJsons.Add(E->GetStringField(TEXT("key")), B);
			}
		}
		else if (O->TryGetObjectField(TEXT("player2"), P2))
		{
			FString B;
			FJsonSerializer::Serialize(P2->ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&B));
			PlayerJsons.Add(TEXT("#1"), B);
		}
		if (PlayerJsons.Num() > 0)
		{
			TArray<FString> Keys;
			PlayerJsons.GetKeys(Keys);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the save carries %d players' blocks: %s"), Keys.Num(), *FString::Join(Keys, TEXT(", ")));
		}
	}
	S->Level = Lv;
	S->Difficulty = Diff;
	S->SpawnDifficulty = O->HasField(TEXT("spawn_difficulty")) ? FMath::Clamp(int32(O->GetNumberField(TEXT("spawn_difficulty"))), 0, 2) : Diff;
	S->LoadName = SlotName(Slot);
	S->StartKind = uint8(EOutlawsCoopStart::Load);
	if (AOutlawsLevelActor* LR = FindLevel()) LR->ReadRandoSave(*O);    // (REQ-COOPRANDO: the save's run and attempt, for the session)
	PrepareExtras(S, Lv, S->SpawnDifficulty);    // (2026-10-02: the extra bad guys for the save's level, with the start)
	++S->StartSerial;
	S->ForceNetUpdate();
	// the save itself to the client, in parts well under a reliable call's limit, for the level of this start
	const int32 Epoch = S->StartSerial;
	const int32 Parts = FMath::Max(1, FMath::DivideAndRoundUp(Text.Len(), SnapPartChars));
	// (N1: each client its own copy, with its own block as "player2")
	int32 Matched = 0;
	ForClients([&](AOutlawsCoopLink& K)
	{
		const FString Own = SaveTextFor(Text, K.PlayerIndex);
		const int32 N = FMath::Max(1, FMath::DivideAndRoundUp(Own.Len(), SnapPartChars));
		for (int32 k = 0; k < N; ++k) K.ClientSnapshotPart(Epoch, k, N, Own.Mid(k * SnapPartChars, SnapPartChars));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP snapshot sent: %d bytes in %d parts%s"), Own.Len(), N, K.PlayerIndex == 1 ? TEXT("") : *FString::Printf(TEXT(" (to the %s)"), WhoWord(K.PlayerIndex)));
		TSharedPtr<FJsonObject> Ob;
		const bool bHas = ParseJson(Own, Ob) && Ob->HasField(TEXT("player2"));
		Matched += bHas ? 1 : 0;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the save for the %s player ('%s'): %s"), WhoWord(K.PlayerIndex), *PlayerKey(K.PlayerIndex),
			bHas ? TEXT("its own block") : TEXT("no block for it (the host's weapons, no keys)"));
	});
	++SnapshotsSent;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the host loads %s (%s) for both, serial %d"), *SlotName(Slot), *Lv, Epoch);
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the save is %d bytes (%d parts as it is; each client's copy above carries its own block)"), Text.Len(), Parts);
	return true;
}

void UOutlawsCoop::OnSnapshotPart(int32 Epoch, int32 Part, int32 Parts, const FString& Text)
{
	if (!IsClient() || Parts <= 0 || Parts > 4096 || Part < 0 || Part >= Parts || Text.Len() > 65536) return;     // (M1 hardening: bounded)
	if (Epoch != SnapEpoch || SnapParts.Num() != Parts)
	{
		SnapEpoch = Epoch;
		SnapParts.Reset();
		SnapParts.SetNum(Parts);
		SnapGot = 0;
		SnapObj.Reset();
	}
	if (SnapParts[Part].IsEmpty())
	{
		SnapParts[Part] = Text;
		++SnapGot;
	}
	if (SnapGot < Parts) return;
	const FString Whole = FString::Join(SnapParts, TEXT(""));
	TSharedPtr<FJsonObject> O;
	if (!ParseJson(Whole, O))
	{
		UE_LOG(LogOutlawsCoop, Warning, TEXT("OLCOOP snapshot received: %d bytes in %d parts, NOT READABLE"), Whole.Len(), Parts);
		return;
	}
	SnapObj = O;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP snapshot received: %d bytes in %d parts (client), level %s"), Whole.Len(), Parts, *O->GetStringField(TEXT("level")));
}

void UOutlawsCoop::BeginLoad(AOutlawsLevelActor* L, const FString& Level, int32 Difficulty, TFunction<void()> Then)
{
	// as LoadGame on both machines: the save's level built again from its start, at the difficulty its bad guys were
	// placed for (so both have the same list); the host's save is put back once its player is placed (TickSaves there),
	// the client's world when the save's JSON has come (TickSaves here)
	const AOutlawsCoopSession* S = GetSession();
	if (L->Movie)
	{
		L->Movie->Destroy();
		L->Movie = nullptr;
	}
	L->LevelEndTimer = -1.f;
	L->bBossDefeated = false;
	L->bLampOn = false;
	L->LevelName = Level;
	L->bFrontEnd = false;
	L->bMovieToMenu = false;
	L->Difficulty = static_cast<EOutlawsDifficulty>(FMath::Clamp(Difficulty, 0, 2));
	// (R7: a late join builds the host's level as the host built it, the snapshot's own spawn difficulty)
	L->PendingSpawnDifficulty = bJoinApply && JoinSpawnDifficulty >= 0 ? JoinSpawnDifficulty : S && S->SpawnDifficulty >= 0 ? S->SpawnDifficulty : FMath::Clamp(Difficulty, 0, 2);
	const TSharedPtr<FJsonObject> O = IsHost() ? SnapObj : nullptr;
	const int32 Epoch = StartedSerial;
	TWeakObjectPtr<UOutlawsCoop> WeakThis(this);
	L->RestartLevelThen([WeakThis, Then, O, Epoch]()
	{
		if (Then) Then();
		UOutlawsCoop* C = WeakThis.Get();
		AOutlawsLevelActor* Lv = C ? C->FindLevel() : nullptr;
		if (!C || !Lv || !O.IsValid() || !C->IsHost()) return;
		// (a load is no level start: no autosave over it at once)
		Lv->StartSaveDelay = -1.f;
		Lv->AutosaveWait = 0.f;
		Lv->PendingLoad = O;
		Lv->SetActorTickEnabled(true);
		C->LoadEpoch = Epoch;
		C->bHostLoadPending = true;
	});
}

namespace
{
	/** A free spot for a second body a step beside At (to the side, behind, ahead), on a floor, not through a wall. */
	FVector SpotBeside(UWorld* W, AOutlawsPlayer* P, const FVector& At, float Yaw)
	{
		if (!W || !P) return At;
		const float Half = P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), Radius = P->GetCapsuleComponent()->GetScaledCapsuleRadius();
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsCoopLoadSpot), false, P);
		const FRotator Facing(0.f, Yaw, 0.f);
		const FVector Ways[] = { FRotationMatrix(Facing).GetUnitAxis(EAxis::Y), -FRotationMatrix(Facing).GetUnitAxis(EAxis::Y),
			-FRotationMatrix(Facing).GetUnitAxis(EAxis::X), FRotationMatrix(Facing).GetUnitAxis(EAxis::X) };
		for (const float Dist : { 110.f, 160.f, 220.f })
		{
			for (const FVector& Way : Ways)
			{
				const FVector To = At + Way * Dist;
				FHitResult Hit;
				if (W->LineTraceSingleByChannel(Hit, At, To, ECC_Visibility, Q)) continue;
				if (W->OverlapBlockingTestByChannel(To, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(Radius, Half), Q)) continue;
				if (!W->LineTraceSingleByChannel(Hit, To, To - FVector(0.0, 0.0, Half + 120.0), ECC_Visibility, Q)) continue;
				return To;
			}
		}
		return At;
	}
}

void UOutlawsCoop::ApplyPlayer2(AOutlawsLevelActor* L, AOutlawsPlayer* P, const FJsonObject& J)
{
	L->Health = FMath::Clamp(float(J.GetNumberField(TEXT("health"))), 0.1f, 10.f);
	L->Armour = int32(J.GetNumberField(TEXT("armour")));
	L->ArmourMax = FMath::Max(int32(J.GetNumberField(TEXT("armour_max"))), L->Armour);
	L->Gold = int32(J.GetNumberField(TEXT("gold")));
	L->BadgeLeft = float(J.GetNumberField(TEXT("badge")));
	L->OilBurn = float(J.GetNumberField(TEXT("oilburn")));
	L->Counts.Reset();
	const TSharedPtr<FJsonObject>* Cn = nullptr;
	if (J.TryGetObjectField(TEXT("counts"), Cn))
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Kv : (*Cn)->Values) L->Counts.Add(Kv.Key, int32(Kv.Value->AsNumber()));
	}
	L->Inventory.Reset();
	L->InventoryLabels.Reset();
	const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
	if (J.TryGetArrayField(TEXT("inventory"), A)) for (const TSharedPtr<FJsonValue>& V : *A) L->Inventory.Add(V->AsString());
	if (J.TryGetArrayField(TEXT("labels"), A)) for (const TSharedPtr<FJsonValue>& V : *A) L->InventoryLabels.Add(V->AsString());
	if (L->Inf && J.TryGetArrayField(TEXT("items"), A))
	{
		L->Inf->Items.Reset();
		for (const TSharedPtr<FJsonValue>& V : *A) L->Inf->Items.Add(int32(V->AsNumber()));
	}
	const TSharedPtr<FJsonObject>* Own = nullptr;
	if (J.TryGetObjectField(TEXT("player"), Own)) P->ReadSave(**Own);     // (its hands, rounds and stamina)
	L->bLampOn = J.GetBoolField(TEXT("lamp")) && L->GetCount(TEXT("ioil")) > 0;
}

void UOutlawsCoop::ApplyJoinKit(AOutlawsLevelActor* L, AOutlawsPlayer* P, const FJsonObject& O, FString& OutHow)
{
	// R7: the late joiner's own kit: its player2 block (the host's word of it, or the co-op autosave's), else the start kit;
	// full health either way, alive, beside the host
	const TSharedPtr<FJsonObject>* P2 = nullptr;
	if (O.TryGetObjectField(TEXT("player2"), P2))
	{
		ApplyPlayer2(L, P, **P2);
		OutHow = TEXT("its own kit from player2, full health");
	}
	else
	{
		L->Inventory.Reset();
		L->InventoryLabels.Reset();
		if (L->Inf) L->Inf->Items.Reset();
		L->ApplyStartKit();
		OutHow = TEXT("the start kit, full health");
	}
	L->Health = 10.f;
	L->bDead = false;
}

void UOutlawsCoop::TickSaves(double Now)
{
	if (!bLevelBuilt) return;
	AOutlawsLevelActor* L = FindLevel();
	AOutlawsPlayer* P = LocalPlayer();
	if (!L || !P) return;
	auto NumberRestored = [](AOutlawsLevelActor* Lv)
	{
		// what the load made again (things dropped before the save) in the save's own order on both machines
		int32 K = 0;
		for (AOutlawsLevelActor::FPickup& Pk : Lv->Pickups)
		{
			if (Pk.CoopId == -1 && Pk.Actor.IsValid()) Pk.CoopId = RestoredIdBase + K++;
		}
		return K;
	};
	// (R7) things dropped in play keep the numbers the host knows them by (its save says), not a load's own
	auto RenumberJoin = [this](AOutlawsLevelActor* Lv, const TSharedPtr<FJsonObject>& O, int32 Made)
	{
		TArray<int32> HostIds;
		const TArray<TSharedPtr<FJsonValue>>* Pa = nullptr;
		if (O->TryGetArrayField(TEXT("pickups"), Pa))
		{
			for (const TSharedPtr<FJsonValue>& V : *Pa)
			{
				const TSharedPtr<FJsonObject> J = V->AsObject();
				if (!J.IsValid() || int32(J->GetNumberField(TEXT("serial"))) >= 0) continue;
				if (AOutlawsLevelActor::IsStickGround(J->GetStringField(TEXT("ground")))) continue;     // (not made again: ApplyLoad)
				double Id = -1.0;
				HostIds.Add(J->TryGetNumberField(TEXT("coop_id"), Id) ? int32(Id) : -1);
			}
		}
		int32 Renumbered = 0;
		for (AOutlawsLevelActor::FPickup& Pk : Lv->Pickups)
		{
			const int32 k = Pk.CoopId - RestoredIdBase;
			if (k >= 0 && k < Made && HostIds.IsValidIndex(k) && HostIds[k] >= 0)
			{
				Pk.CoopId = HostIds[k];
				++Renumbered;
			}
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP joining late: %d of %d things dropped in play numbered as the host has them"), Renumbered, Made);
	};
	auto DeadOf = [](const AOutlawsLevelActor* Lv)
	{
		int32 N = 0;
		for (const TWeakObjectPtr<AOutlawsEnemy>& E : Lv->GetEnemies()) N += E.IsValid() && E->IsDead() ? 1 : 0;
		return N;
	};
	if (IsHost())
	{
		if (bHostLoadPending && LevelEpoch() == LoadEpoch && !L->PendingLoad.IsValid() && !L->bNeedPlace)
		{
			bHostLoadPending = false;
			const int32 Made = NumberRestored(L);
			SnapObj.Reset();
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP save put back here (host): %d enemies (%d dead), %d pickups (%d made again, numbered from %d)"), L->GetEnemies().Num(), DeadOf(L),
				L->Pickups.Num(), Made, RestoredIdBase);
		}
		return;
	}
	if (!IsClient()) return;
	// (Q2 step 4) a late join's saved bad guys going back a few a frame: the rest of the join once they all are
	if (JoinReplayStep == 2)
	{
		if (JoinReplayEpoch != LevelEpoch() || !JoinReplayObj.IsValid())
		{
			EndJoinReplay(TEXT("the level changed under it"));
			return;
		}
		const double T0 = FPlatformTime::Seconds();
		const bool bDone = DrainJoinReplay(L);
		if (bDone)
		{
			const TSharedPtr<FJsonObject> O = JoinReplayObj;
			JoinReplayObj.Reset();
			JoinReplayStep = 3;
			const int32 Made = NumberRestored(L);
			if (bJoinApply) RenumberJoin(L, O, Made);
			FinishSnapshotApply(L, P, O, Made, Now);
		}
		NoteJoinReplayWork((FPlatformTime::Seconds() - T0) * 1000.0);
		return;
	}
	// the save's world, once its level is built here and this player is placed
	if (SnapObj.IsValid() && SnapEpoch == LevelEpoch() && !L->bNeedPlace && !L->PendingLoad.IsValid())
	{
		// (Q2 step 4) a late join spread over frames: the bad guys' first snapshots placed first (TickEnemies, a few a frame;
		// as the one frame did them before the save), at most 3 s
		const bool bSpread = bJoinApply && JoinReplayStep == 1 && JoinPlaceBudget() != 0;
		if (bSpread)
		{
			if (JoinReplayReadyAt <= 0.0) JoinReplayReadyAt = Now;
			const AOutlawsCoopSession* Ss = GetSession();
			const int32 Hosts = Ss && Ss->EnemyEpoch == StartedSerial ? FMath::Min(Ss->Enemies.Num(), L->GetEnemies().Num()) : 0;
			int32 Unplaced = 0;
			for (int32 i = 0; i < Hosts; ++i)
			{
				const AOutlawsEnemy* E = L->GetEnemies()[i].Get();
				Unplaced += E && !E->bPuppetPlaced ? 1 : 0;
			}
			if (Unplaced > 0 && Now - JoinReplayReadyAt < 3.0) return;
			if (Unplaced > 0) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP join replay: %d bad guys still not placed after 3 s: the save goes on"), Unplaced);
		}
		const double T0 = FPlatformTime::Seconds();
		const TSharedPtr<FJsonObject> O = SnapObj;
		SnapObj.Reset();
		JoinReplayQueue.Reset();
		L->DeferredDrops.Reset();
		L->bDeferDrops = bSpread;
		if (bSpread) L->DeferEnemyReads = &JoinReplayQueue;
		L->ApplyLoad(*O, true);
		L->DeferEnemyReads = nullptr;
		L->bDeferDrops = false;
		const int32 Made = bSpread ? 0 : NumberRestored(L);
		if (bJoinApply && !bSpread) RenumberJoin(L, O, Made);
		if (bSpread)
		{
			JoinReplayObj = O;
			JoinReplayStep = 2;
			NoteJoinReplayWork((FPlatformTime::Seconds() - T0) * 1000.0);
			return;
		}
		FinishSnapshotApply(L, P, O, Made, Now);
		if (JoinReplayStep == 1) JoinReplayStep = 3;
		NoteJoinReplayWork((FPlatformTime::Seconds() - T0) * 1000.0);
		return;
	}
	// this player's own state for the host's saves, when it changes (at most once a second; not while a load is coming)
	if (Now < NextPlayer2At || L->bNeedPlace || SnapObj.IsValid() || JoinReplayStep == 2 || (SnapEpoch == LevelEpoch() && SnapGot < SnapParts.Num())) return;
	NextPlayer2At = Now + 1.0;
	const TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
	J->SetNumberField(TEXT("health"), L->Health);
	J->SetNumberField(TEXT("armour"), L->Armour);
	J->SetNumberField(TEXT("armour_max"), L->ArmourMax);
	J->SetNumberField(TEXT("gold"), L->Gold);
	J->SetNumberField(TEXT("badge"), L->BadgeLeft);
	J->SetNumberField(TEXT("oilburn"), L->OilBurn);
	J->SetBoolField(TEXT("lamp"), L->bLampOn);
	J->SetBoolField(TEXT("dead"), L->bDead);
	const TSharedRef<FJsonObject> Cn = MakeShared<FJsonObject>();
	for (const TPair<FString, int32>& Kv : L->Counts) Cn->SetNumberField(Kv.Key, Kv.Value);
	J->SetObjectField(TEXT("counts"), Cn);
	TArray<TSharedPtr<FJsonValue>> Inv, Labels, Items;
	for (const FString& Sv : L->Inventory) Inv.Add(MakeShared<FJsonValueString>(Sv));
	for (const FString& Sv : L->InventoryLabels) Labels.Add(MakeShared<FJsonValueString>(Sv));
	if (L->Inf) for (const int32 Id : L->Inf->Items) Items.Add(MakeShared<FJsonValueNumber>(Id));
	J->SetArrayField(TEXT("inventory"), Inv);
	J->SetArrayField(TEXT("labels"), Labels);
	J->SetArrayField(TEXT("items"), Items);
	const TSharedRef<FJsonObject> Own = MakeShared<FJsonObject>();
	P->WriteSave(*Own);
	J->SetObjectField(TEXT("player"), Own);
	FString Text;
	if (!FJsonSerializer::Serialize(J, TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text))) return;
	// (its place and stamina change all the time: sent only when the rest changes, or every ten seconds)
	FString Key = Text;
	if (const int32 At = Key.Find(TEXT("\"player\"")); At != INDEX_NONE) Key = Key.Left(At);
	if (Key == SentPlayer2 && Now - Player2FullAt < 10.0) return;
	SentPlayer2 = Key;
	Player2FullAt = Now;
	if (AOutlawsCoopLink* Mine = GetLink(MyIndex())) Mine->ServerPlayer2(Text);
}

void UOutlawsCoop::FinishSnapshotApply(AOutlawsLevelActor* L, AOutlawsPlayer* P, const TSharedPtr<FJsonObject>& O, int32 Made, double Now)
{
	if (!L || !P || !O.IsValid()) return;
	auto DeadOf = [](const AOutlawsLevelActor* Lv)
	{
		int32 N = 0;
		for (const TWeakObjectPtr<AOutlawsEnemy>& E : Lv->GetEnemies()) N += E.IsValid() && E->IsDead() ? 1 : 0;
		return N;
	};
	// this player: its own block, or a copy of the host's weapons and ammo and no keys; beside the host's saved place
	const TSharedPtr<FJsonObject>* HostPl = nullptr;
	FVector HostAt = P->GetActorLocation();
	float Yaw = float(P->GetActorRotation().Yaw);
	if (O->TryGetObjectField(TEXT("player"), HostPl))
	{
		HostAt = JsonVec(**HostPl);
		Yaw = float((*HostPl)->GetNumberField(TEXT("yaw")));
	}
	const TSharedPtr<FJsonObject>* P2 = nullptr;
	FString How;
	if (bJoinApply)
	{
		ApplyJoinKit(L, P, *O, How);         // (R7: a late join)
	}
	else if (O->TryGetObjectField(TEXT("player2"), P2))
	{
		ApplyPlayer2(L, P, **P2);
		How = TEXT("its own kit from the save's player2");
	}
	else
	{
		L->Counts.Reset();
		const TSharedPtr<FJsonObject>* Cn = nullptr;
		if (O->TryGetObjectField(TEXT("counts"), Cn))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Kv : (*Cn)->Values)
			{
				const AOutlawsLevelActor::FItemDef* D = L->ItemDefs.Find(Kv.Key);
				const FString Class = D ? D->Class.ToLower() : FString();
				if (!Class.Contains(TEXT("key")) && !Class.Contains(TEXT("puzz"))) L->Counts.Add(Kv.Key, int32(Kv.Value->AsNumber()));
			}
		}
		L->Health = 10.f;
		L->Inventory.Reset();
		L->InventoryLabels.Reset();
		if (L->Inf) L->Inf->Items.Reset();
		How = TEXT("a copy of the host's weapons and ammo, no keys (the save has no player2)");
	}
	L->bDead = false;
	const FVector At = SpotBeside(L->GetWorld(), P, HostAt, Yaw);
	P->TeleportTo(At, FRotator(0.f, Yaw, 0.f), false, true);
	if (AController* Ctl = P->GetController()) Ctl->SetControlRotation(FRotator(0.f, Yaw, 0.f));
	++SnapshotsApplied;
	if (bJoinApply)
	{
		bJoinApply = false;
		JoinSpawnDifficulty = -1;
		++JoinsApplied;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP joining late: in the host's game at %s, %.0f cm from the host (%s)"), *L->LevelName, FVector::Dist(At, HostAt), *How);
		if (bBackInPending)
		{
			bBackInPending = false;      // (R8: back after a dropped connection)
			L->Notify(TEXT("Back in"), 2.5f);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP back in after the dropped connection"));
		}
	}
	int32 SavedEnemies = 0, SavedPickups = 0;
	const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
	if (O->TryGetArrayField(TEXT("enemies"), A)) SavedEnemies = A->Num();
	if (O->TryGetArrayField(TEXT("pickups"), A)) SavedPickups = A->Num();
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP snapshot applied: %d enemies, %d pickups (client); %d dead here, %d made again; this player: %s, at %s (%.0f cm from the host's saved place)"),
		SavedEnemies, SavedPickups, DeadOf(L), Made, *How, *At.ToCompactString(), FVector::Dist(At, HostAt));
	SentPlayer2.Reset();
	NextPlayer2At = Now + 1.0;
}

FString UOutlawsCoop::SavesSummary() const
{
	return FString::Printf(TEXT("saves written %d, snapshots sent %d, snapshots applied %d, player2 heard %s, player blocks %d"), SavesWritten, SnapshotsSent, SnapshotsApplied,
		Player2Json.IsEmpty() ? TEXT("no") : TEXT("yes"), PlayerJsons.Num());
}

// ---- tests ---------------------------------------------------------------------------------------------------------------

#if WITH_OUTLAWS_TESTS
FString UOutlawsCoop::TestFingerprint(AOutlawsLevelActor* L) const
{
	return Fingerprint(L) + FString::Printf(TEXT(" keys %d"), L->Inf ? L->Inf->Items.Num() : 0);
}

FString UOutlawsCoop::TestWorldPrint(AOutlawsLevelActor* L) const
{
	// the dead bad guys by index and the elevators away from their first stop, by index and value
	TArray<FString> Dead, Moved;
	// (2026-10-02: the level's own here; a session's extra bad guys, if any, after the elevators: single player has none)
	TArray<FString> ExtraDead;
	int32 Extras = 0;
	const TArray<TWeakObjectPtr<AOutlawsEnemy>>& List = L->GetEnemies();
	for (int32 i = 0; i < List.Num(); ++i)
	{
		if (!List[i].IsValid()) continue;
		const bool bExtra = List[i]->CoopCopyOf >= 0;
		Extras += bExtra ? 1 : 0;
		if (List[i]->IsDead()) (bExtra ? ExtraDead : Dead).Add(FString::FromInt(i));
	}
	if (L->Inf)
	{
		for (int32 i = 0; i < L->Inf->Elevators.Num(); ++i)
		{
			const FOutlawsElevator& E = L->Inf->Elevators[i];
			if (L->Inf->Inf.Items[E.Item].Classes[E.ClassIndex].Stops.Num() > 0 && FMath::Abs(E.Current - L->Inf->StopValue(E, 0)) > 0.5) Moved.Add(FString::Printf(TEXT("%d=%.0f"), i, E.Current));
		}
	}
	return FString::Printf(TEXT("dead [%s], elevators off their first stop [%s]%s"), *FString::Join(Dead, TEXT(",")), *FString::Join(Moved, TEXT(",")),
		Extras > 0 ? *FString::Printf(TEXT(", extra bad guys %d, dead [%s]"), Extras, *FString::Join(ExtraDead, TEXT(","))) : TEXT(""));
}

void UOutlawsCoop::TickHostSaveTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	if (Now < TestAt) return;
	const TArray<TWeakObjectPtr<AOutlawsEnemy>>& List = L->GetEnemies();
	switch (TestStep)
	{
	case 140:
		if (TestOnly == TEXT("load"))
		{
			// (the second run: the save was loaded for both by the lobby's Start; give the client time to take its world)
			TestStep = 145;
			TestAt = Now + 6.0;
			return;
		}
		// 1: the client takes a kit of its own (two medical bags, 37 gold) that the save must keep as player2
		L->bInvincible = true;
		TestSay(TEXT("godmode"), 1);
		TestSay(TEXT("s:kit"), 0);
		TestStep = 141;
		TestAt = Now + 1.0;
		return;
	case 141:
	{
		// 2: the host kills a bad guy of the level's own and opens a door (as milestone 7's pause test chooses one: a pushed
		// door with stops); 2026-10-02: and one of the extra bad guys, whom the save marks as such
		TestSaveEnemy = TestPickEnemy(L, P->GetActorLocation(), INDEX_NONE, true);
		if (AOutlawsEnemy* E = List.IsValidIndex(TestSaveEnemy) ? List[TestSaveEnemy].Get() : nullptr)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host shoots enemy %d dead (%s)"), TestSaveEnemy, *E->CoopDescribe());
			const FVector At = E->GetActorLocation();
			E->TakeHit(100.f, At, (At - P->GetActorLocation()).GetSafeNormal(), P->GetActorLocation());
		}
		TestSaveCopy = -1;
		double NearestCopy = 1e30;
		for (int32 i = 0; i < List.Num(); ++i)
		{
			const AOutlawsEnemy* E = List[i].Get();
			if (!E || E->CoopCopyOf < 0 || E->IsDead() || E->IsDormant()) continue;
			const double D = FVector::DistSquared(E->GetActorLocation(), P->GetActorLocation());
			if (D < NearestCopy) { NearestCopy = D; TestSaveCopy = i; }
		}
		if (AOutlawsEnemy* E = List.IsValidIndex(TestSaveCopy) ? List[TestSaveCopy].Get() : nullptr)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host shoots extra bad guy %d (a %s beside enemy %d) dead (%s); kills %d"), TestSaveCopy, *E->GetOutlawsName(), E->CoopCopyOf, *E->CoopDescribe(), L->GetKills());
			const FVector At = E->GetActorLocation();
			E->TakeHit(100.f, At, (At - P->GetActorLocation()).GetSafeNormal(), P->GetActorLocation());
		}
		TestSaveDoor = -1;
		double Longest = 0.0;
		for (int32 i = 0; L->Inf && i < L->Inf->Elevators.Num(); ++i)
		{
			const FOutlawsElevator& E = L->Inf->Elevators[i];
			const FOutlawsInfClass& Cl = L->Inf->Inf.Items[E.Item].Classes[E.ClassIndex];
			if (Cl.KeyObject >= 0 || !(Cl.EventMask & (OutlawsInf::EvNudgeFront | OutlawsInf::EvNudgeBack)) || Cl.Stops.Num() < 2 || E.Sectors.Num() == 0) continue;
			const double Travel = FMath::Abs(L->Inf->StopValue(E, 1) - E.Current) / FMath::Max(0.01, double(E.Speed));
			if (Travel < 6.0 && Travel > Longest) { Longest = Travel; TestSaveDoor = i; }
		}
		if (TestSaveDoor >= 0)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host pushes elevator %d (%s): %s"), TestSaveDoor, *L->Inf->Elevators[TestSaveDoor].Name,
				L->PushDoorSector(L->Inf->Elevators[TestSaveDoor].Sectors[0]) ? TEXT("taken") : TEXT("not taken"));
		}
		TestStep = 142;
		TestAt = Now + 7.0;
		return;
	}
	case 142:
	{
		// 3: the host saves into slot 9 (the pause menu's Save Game), with the client's kit as player2
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: before the save (host): %s; enemy %d, elevator %d"), *TestWorldPrint(L), TestSaveEnemy, TestSaveDoor);
		TestSay(TEXT("s:world"), 0);
		const bool bOk = L->SaveGame(9);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host saves into slot 9: %s"), bOk ? TEXT("written") : TEXT("NOT WRITTEN"));
		TestSay(TEXT("s:trysave"), 0);
		TestStep = 143;
		TestAt = Now + 2.0;
		return;
	}
	case 143:
		TestSay(TEXT("quit"), 0);
		TestStep = 100;
		TestAt = Now + 3.0;
		return;
	case 145:
		// the second run: both machines say what the save put back
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: after the load (host): %s; %s"), *TestWorldPrint(L), *TestFingerprint(L));
		TestSay(TEXT("s:world"), 1);
		// the host's pause menu's Load Game page in a session: the autosave listed first, as in single player (photographed)
		P->CoopOpenMenu();
		P->CoopShowPage(TEXT("load_game.rml"), true);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host's menu shows %s (an autosave here: %s)"), *P->CoopMenuPage(),
			AOutlawsLevelActor::ReadSaveInfo(-1).bEmpty ? TEXT("no") : TEXT("yes"));
		TestStep = 146;
		TestAt = Now + 2.0;
		return;
	case 146:
		Shot(TEXT("ol_coop_load_page"));
		TestStep = 147;
		TestAt = Now + 1.0;
		return;
	case 147:
		P->CoopCloseMenu();
		TestSay(TEXT("quit"), 0);
		TestStep = 100;
		TestAt = Now + 3.0;
		return;
	default:
		return;
	}
}

void UOutlawsCoop::OnSaveTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	if (Step == TEXT("kit"))
	{
		L->AddCount(TEXT("idocbag"), 2);
		L->Gold += 37;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client's own kit: %s"), *TestFingerprint(L));
		NextPlayer2At = 0.0;                     // (told to the host at once)
	}
	else if (Step == TEXT("world"))
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: %s (client): %s; %s"), Arg ? TEXT("after the load") : TEXT("before the save"), *TestWorldPrint(L), *TestFingerprint(L));
	}
	else if (Step == TEXT("trysave"))
	{
		const bool bOk = L->SaveGame(8);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client tries to save into slot 8: %s"), bOk ? TEXT("WRITTEN") : TEXT("refused (the host saves)"));
	}
}

void UOutlawsCoop::TickSoloLoadTest(double Now)
{
	// -OLLoadTest=<slot>: single player (no session) loads that save from the title menu as the menu's Load Game does, and
	// says what it put back (a co-op save's player2 block is not read)
	AOutlawsLevelActor* L = FindLevel();
	AOutlawsPlayer* P = LocalPlayer();
	if (!L || !P) return;
	if (SoloLoadStep == 0)
	{
		if (!P->IsMenuOpen()) return;
		if (SoloLoadAt == 0.0) { SoloLoadAt = Now + 3.0; return; }
		if (Now < SoloLoadAt) return;
		P->CoopCloseMenu();
		const bool bOk = L->LoadGame(SoloLoadSlot);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP single-player load test: %s from the title menu: %s"), *SlotName(SoloLoadSlot), bOk ? TEXT("loading") : TEXT("NOT LOADED"));
		SoloLoadStep = bOk ? 1 : 3;
		SoloLoadAt = Now + 1.0;
		return;
	}
	if (SoloLoadStep == 1)
	{
		if (Now < SoloLoadAt || L->PendingLoad.IsValid() || L->bNeedPlace || L->IsFrontEnd()) return;
		SoloLoadStep = 2;
		SoloLoadAt = Now + 2.0;
		return;
	}
	if (SoloLoadStep == 2 && Now >= SoloLoadAt)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP single-player load test: %s put back in %s: %s; %s"), *SlotName(SoloLoadSlot), *L->LevelName, *TestWorldPrint(L), *TestFingerprint(L));
		SoloLoadStep = 3;
	}
	if (SoloLoadStep == 3)
	{
		QuitForTest();
	}
}
#endif // WITH_OUTLAWS_TESTS
