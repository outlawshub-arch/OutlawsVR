// The wheel's drop (user 2026-10-02: "if the player presses grip on a selection on the weapon wheel, it should drop that
// item, whether its a weapon, a plate, a medbag, key, etc. this would allow co op players to pass items or weapons between
// each other").
// - The wheel up, a squeeze of the grip on the hand whose laser points at a slot drops one of it (the trigger and the
//   button's release still pick as before): a weapon with what it carries (its ammunition goes with it when no gun left in
//   the pockets fires the same: the death drop's rule, OutlawsCoopRules.cpp, where the start kit's guns are the ones kept),
//   one stick of dynamite or one knife, the boiler plate with its armour points, one medical bag's worth of points, one key,
//   puzzle piece or other carried item.  It lands in front of the player on the floor (this side of any wall), a normal
//   pickup that gives exactly that back; in co-op a shared one numbered by the host, as a gun dropped at a death is.
// - Revolvers: every one picked up is kept (AddCount: two make the pair, the rest are spares, "x3" on the wheel).  Drops
//   go one at a time: the spares, then the second (the pair is gone), then the last; the fists always remain.
// - Never dropped: the lantern (and its oil), the map, the fists, the grip editor; "You can't drop that".
// - The dropper cannot take it again (walk-over, reach or pull) until they have been 1.5 m from it, across, or 30 s on
//   (user 2026-10-02); the other player in co-op can take it at once (their machine's copy carries no lock).
// - Tests: -OLShots -OLWheelDropTest (single player, the hideout) and -OLCoopAuto -OLCoopTest=pass (co-op).
#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsCoop.h"
#include "OutlawsLab.h"
#include "OutlawsFormats.h"
#include "OutlawsInfRuntime.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/TextRenderComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "UnrealClient.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "OutlawsShotTests.h"

namespace OutlawsWheelDropLocal
{
	// the weapons in GWeapons' order (OutlawsPlayerWeapons.cpp), each with the ammunition it fires (the throwing weapons
	// are their own), as the death drop has them
	struct FDropWeapon { const TCHAR* Item; const TCHAR* Ammo; bool bThrown; };
	const FDropWeapon DropWeapons[] = { { TEXT("ipistol"), TEXT("ibullit"), false }, { TEXT("irifle"), TEXT("icart"), false },
		{ TEXT("isgun"), TEXT("ishells"), false }, { TEXT("idbsgun"), TEXT("ishells"), false }, { TEXT("isawgun"), TEXT("ishells"), false },
		{ TEXT("igatgun"), TEXT("iclip"), false }, { TEXT("idynam"), TEXT("idynam"), true }, { TEXT("iknife"), TEXT("iknife"), true } };
	constexpr double DropAhead = 55.0;           // cm in front of the body (in reach of a hand, out of the way of the feet)
	constexpr double DropSideStep = 15.0;        // drops in a row lie side by side (five places, then round again)
	constexpr double DropOffWall = 25.0;         // kept this far this side of a wall the drop would have gone into

	int32 DropWeaponIndex(const FString& Item)
	{
		for (int32 i = 0; i < int32(UE_ARRAY_COUNT(DropWeapons)); ++i)
		{
			if (Item.Equals(DropWeapons[i].Item, ESearchCase::IgnoreCase)) return i;
		}
		return INDEX_NONE;
	}

	FString DropCountsLine(const AOutlawsLevelActor* L)
	{
		return FString::Printf(TEXT("ipistol %d, isgun %d, ishells %d, ibullit %d, idocbag %d, ibrsskey %d, armour %d"), L->GetCount(TEXT("ipistol")),
			L->GetCount(TEXT("isgun")), L->GetCount(TEXT("ishells")), L->GetCount(TEXT("ibullit")), L->GetCount(TEXT("idocbag")),
			L->GetCount(TEXT("ibrsskey")), L->GetArmour());
	}

	TSharedPtr<FJsonObject> DropReadSave(int32 Slot)
	{
		FString Text;
		TSharedPtr<FJsonObject> O;
		if (!FFileHelper::LoadFileToString(Text, *AOutlawsLevelActor::SavePath(Slot))) return O;
		const TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Text);
		FJsonSerializer::Deserialize(R, O);
		return O;
	}

	int32 DropSavedCount(const TSharedPtr<FJsonObject>& O, const TCHAR* Item)
	{
		const TSharedPtr<FJsonObject>* C = nullptr;
		if (!O.IsValid() || !O->TryGetObjectField(TEXT("counts"), C)) return -1;
		double N = 0.0;
		return (*C)->TryGetNumberField(Item, N) ? int32(N) : 0;
	}

	// the single-player test's state (real time; it outlives a load's rebuild of the level)
	struct FWheelDropTestState
	{
		int32 Step = 0;
		double At = 0.0;
		FVector Home = FVector::ZeroVector;      // where the player stood to drop (the capsule's middle)
		FVector Ahead = FVector::ForwardVector;
		int32 Next = 0;
		int32 Dropped = 0, Refused = 0, Failed = 0;
		TArray<TWeakObjectPtr<AActor>> Drops;
		TArray<TWeakObjectPtr<AActor>> Back;     // the dropped things after the load, to walk over
		int32 BackAt = -1;
		FString BackDesc;
		bool bLockHeld = false;
		int32 WalkedBack = 0;
		int32 PlateArmour = 0;
	};
	FWheelDropTestState WheelDropTestState;
}

// ---- the level's side: out of the pockets, onto the floor ------------------------------------------------------------------

bool AOutlawsLevelActor::NeverDropped(const FString& Item)
{
	// the lantern is always carried (the original's L key) and its oil burns in it; the map is the level's own; the grip
	// editor is a dev switch; the badge is a power, not an item
	static const TCHAR* Never[] = { TEXT("ilantern"), TEXT("ioil"), TEXT("imap"), TEXT("idev_grip"), TEXT("idev_gripreset"), TEXT("ibadge") };
	if (Item.IsEmpty()) return true;
	for (const TCHAR* N : Never)
	{
		if (Item.Equals(N, ESearchCase::IgnoreCase)) return true;
	}
	return false;
}

AActor* AOutlawsLevelActor::DropCarried(const FString& InItem, const FVector& Eye, const FVector& Want, FString& OutWhat, FString& OutWhy)
{
	using namespace OutlawsWheelDropLocal;
	OutWhat.Reset();
	OutWhy.Reset();
	const FString Key = InItem.ToLower();
	UOutlawsCoop* C = UOutlawsCoop::Get(this);
	UWorld* World = GetWorld();
	if (NeverDropped(Key) || !C || !World)
	{
		OutWhy = TEXT("You can't drop that");
		return nullptr;
	}
	// one of it, and what goes with it
	FString Ground;
	TArray<TPair<FString, int32>> Give;
	if (Key == TEXT("ibplate"))
	{
		// the plate worn, with the armour points it has left; it lies as the ground plate nearest its size
		if (Armour <= 0) return nullptr;
		Give.Emplace(Key, Armour);
		const int32 Size = FMath::Max(ArmourMax, Armour);
		int32 BestCount = 0;
		if (AssetsRef)
		{
			TArray<FString> Itms;
			AssetsRef->ListByExtension(TEXT(".itm"), Itms);
			for (const FString& Name : Itms)
			{
				FString Text, Func;
				TMap<FString, FString> Data;
				if (!AssetsRef->GetText(Name, Text)) continue;
				OutlawsFormats::ParseItm(Text, Func, Data);
				if (!Func.Contains(TEXT("GroundObject")) || !FPaths::GetBaseFilename(Data.FindRef(TEXT("PICKUP"))).Equals(Key, ESearchCase::IgnoreCase)) continue;
				const int32 N = FCString::Atoi(*Data.FindRef(TEXT("COUNT")));
				// the smallest that holds it, else the largest there is
				const bool bBetter = Ground.IsEmpty() || (N >= Size && (BestCount < Size || N < BestCount)) || (BestCount < Size && N > BestCount);
				if (bBetter)
				{
					Ground = FPaths::GetBaseFilename(Name).ToLower();
					BestCount = N;
				}
			}
		}
		if (Ground.IsEmpty()) Ground = C->GroundItemFor(this, Key);
	}
	else
	{
		const int32 Have = GetCount(Key);
		if (Have <= 0) return nullptr;
		Give.Emplace(Key, Key == TEXT("idocbag") ? FMath::Min(Have, C->MedBagPoints(this)) : 1);    // one bag is a bag's worth of points
		// (the dynamite's own ground item is a lit stick, not a pickup: a box of it, as the death drop does)
		Ground = Key == TEXT("idynam") ? FString(TEXT("gamboxt")) : C->GroundItemFor(this, Key);
		const int32 Wd = DropWeaponIndex(Key);
		if (Wd != INDEX_NONE && !DropWeapons[Wd].bThrown)
		{
			// its ammunition goes with it unless a gun still carried fires the same (a spare revolver goes alone)
			const FString Ammo = DropWeapons[Wd].Ammo;
			bool bKept = false;
			for (const FDropWeapon& G : DropWeapons)
			{
				if (Ammo.Equals(G.Ammo) && GetCount(G.Item) - (Key.Equals(G.Item) ? 1 : 0) > 0) bKept = true;
			}
			if (!bKept && GetCount(Ammo) > 0) Give.Emplace(Ammo, GetCount(Ammo));
		}
	}
	if (Ground.IsEmpty())
	{
		OutWhy = TEXT("You can't drop that");
		UE_LOG(LogOutlaws, Log, TEXT("OLRT wheel drop: %s has no ground item"), *Key);
		return nullptr;
	}
	TArray<FString> Parts;
	for (const TPair<FString, int32>& Kv : Give) Parts.Add(FString::Printf(TEXT("%s=%d"), *Kv.Key, Kv.Value));
	OutWhat = FString::Join(Parts, TEXT(","));
	// the floor in front of the player, this side of any wall (as a hand puts a pickup down: DropPickupActor)
	APawn* Pawn = World->GetFirstPlayerController() ? World->GetFirstPlayerController()->GetPawn() : nullptr;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsWheelDrop), true);
	if (Pawn) Q.AddIgnoredActor(Pawn);
	FCollisionResponseParams Rp;
	Rp.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
	FVector Over = Want;
	FHitResult Hit;
	if (World->LineTraceSingleByChannel(Hit, Eye, Want, ECC_Visibility, Q, Rp))
	{
		Over = Hit.ImpactPoint - (Want - Eye).GetSafeNormal() * DropOffWall;
	}
	FVector Feet = Pawn ? Pawn->GetActorLocation() - FVector(0.0, 0.0, Pawn->GetSimpleCollisionHalfHeight()) : Want;
	if (World->LineTraceSingleByChannel(Hit, Over, Over - FVector(0.0, 0.0, 5000.0), ECC_Visibility, Q, Rp))
	{
		Feet = Hit.ImpactPoint;
	}
	// made as the death drop makes its guns: the other machine is told the name with the contents (NoteGroundItem)
	C->GroundNameSuffix = TEXT("#") + OutWhat;
	AActor* A = SpawnGroundItem(Ground, Feet);
	C->GroundNameSuffix.Reset();
	if (!A)
	{
		OutWhy = TEXT("You can't drop that");
		UE_LOG(LogOutlaws, Log, TEXT("OLRT wheel drop: %s could not be made"), *Ground);
		return nullptr;
	}
	UOutlawsCoop::ApplyGroundContents(this, A, OutWhat);
	// out of the pockets and the wrist's list (the last lines first; all of them when none is left)
	for (const TPair<FString, int32>& Kv : Give)
	{
		if (Kv.Key == TEXT("ibplate"))
		{
			Armour = 0;
			ArmourMax = 0;
			continue;
		}
		int32& N = Counts.FindOrAdd(Kv.Key);
		N = FMath::Max(0, N - Kv.Value);
		const bool bNone = N == 0;
		if (bNone) Counts.Remove(Kv.Key);
		int32 Unlist = bNone ? MAX_int32 : Kv.Value;
		for (int32 i = Inventory.Num() - 1; i >= 0 && Unlist > 0; --i)
		{
			if (!Inventory[i].Equals(Kv.Key, ESearchCase::IgnoreCase)) continue;
			Inventory.RemoveAt(i);
			if (InventoryLabels.IsValidIndex(i)) InventoryLabels.RemoveAt(i);
			--Unlist;
		}
		// a key's lock forgets it once no item answering its use id is left (igear1..3 share one)
		const FItemDef* D = ItemDefs.Find(Kv.Key);
		if (bNone && D && D->UseId >= 0 && Inf && !IsToolUseId(D->UseId))
		{
			bool bOther = false;
			for (const TPair<FString, FItemDef>& Dv : ItemDefs) if (Dv.Value.UseId == D->UseId && GetCount(Dv.Key) > 0) bOther = true;
			if (!bOther) Inf->Items.Remove(D->UseId);
		}
	}
	for (FPickup& P : Pickups)
	{
		if (P.Actor.Get() != A) continue;
		P.bInstant = false;                      // (it gives what was carried, as an item)
		P.bWaitClear = false;
		P.bDropLock = true;
		P.DropLockAt = FPlatformTime::Seconds();
	}
	PlayWav(TEXT("weapgrab"), Feet);
	++WheelDrops;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT wheel drop: %s (%s) at %s, locked for its dropper; now %s"), *Ground, *OutWhat, *Feet.ToCompactString(), *DropCountsLine(this));
	return A;
}

FString AOutlawsLevelActor::DescribePickup(const AActor* A, bool* bOutLocked) const
{
	if (bOutLocked) *bOutLocked = false;
	for (const FPickup& P : Pickups)
	{
		if (!A || P.Actor.Get() != A) continue;
		TArray<FString> Parts;
		for (const FPickup::FEntry& E : P.Entries) Parts.Add(FString::Printf(TEXT("%s=%d"), *E.Item, E.Count));
		if (bOutLocked) *bOutLocked = P.bDropLock;
		return FString::Printf(TEXT("%s: %s"), *P.Ground, *FString::Join(Parts, TEXT(",")));
	}
	return FString();
}

void AOutlawsLevelActor::DroppedPickups(TArray<AActor*>& Out) const
{
	Out.Reset();
	for (const FPickup& P : Pickups)
	{
		if (P.Serial < 0 && P.Actor.IsValid()) Out.Add(P.Actor.Get());
	}
}

// ---- the player's side: the slot pointed at, the hands, the wheel again ------------------------------------------------------

bool AOutlawsPlayer::WheelDrop(int32 Hand)
{
	using namespace OutlawsWheelDropLocal;
	AOutlawsLevelActor* L = Level.Get();
	if (!L || !bWheelOpen || Hand < 0 || Hand > 1) return false;
	const int32 Pick = WheelDropPick[Hand] >= 0 ? WheelDropPick[Hand] : WheelPick[Hand];
	if (!WheelWeapon.IsValidIndex(Pick)) return false;           // (the laser on nothing: the squeeze does nothing)
	const int32 Wi = WheelWeapon[Pick];
	const FString Item = Wi >= 0 && Wi < int32(UE_ARRAY_COUNT(DropWeapons)) ? FString(DropWeapons[Wi].Item) : Wi == -2 && WheelItem.IsValidIndex(Pick) ? WheelItem[Pick] : FString();
	const FString Label = WheelLabels.IsValidIndex(Pick) && WheelLabels[Pick] ? WheelLabels[Pick]->Text.ToString() : Item;
	if (L->NeverDropped(Item))
	{
		L->Notify(TEXT("You can't drop that"), 2.f);
		Refuse(Hand);
		UE_LOG(LogOutlaws, Log, TEXT("OLRT wheel drop refused: %s (%s) is never dropped"), Wi == -1 ? TEXT("the fists") : *Item, *Label);
		return false;
	}
	// in front of the player, along the way the eyes face, the drops in a row side by side
	const FVector Eye = Camera->GetComponentLocation();
	FVector Fwd = FVector(Camera->GetForwardVector().X, Camera->GetForwardVector().Y, 0.0).GetSafeNormal();
	if (Fwd.IsNearlyZero()) Fwd = FVector(GetActorForwardVector().X, GetActorForwardVector().Y, 0.0).GetSafeNormal();
	const FVector Side = FVector::CrossProduct(FVector::UpVector, Fwd);
	const double Across = double((L->WheelDrops % 5) - 2) * DropSideStep;
	const FVector Want = GetActorLocation() + Fwd * DropAhead + Side * Across;
	FString What, Why;
	AActor* A = L->DropCarried(Item, Eye, Want, What, Why);
	if (!A)
	{
		if (!Why.IsEmpty()) L->Notify(Why, 2.f);
		Refuse(Hand);
		UE_LOG(LogOutlaws, Log, TEXT("OLRT wheel drop refused: %s (%s): %s"), *Item, *Label, Why.IsEmpty() ? TEXT("none carried") : *Why);
		return false;
	}
	LastWheelDrop = A;
	// the hands: no more of a gun in them than are carried (a pair dropped to one: the off hand's goes; none left: both);
	// a tool whose last went leaves the hand (the plate's own hand empties on the gear's tick when its armour is gone)
	const int32 Wd = DropWeaponIndex(Item);
	if (Wd != INDEX_NONE)
	{
		const int32 Left = L->GetCount(Item);
		const int32 Off = LeftDominant() ? 1 : 0;
		const int32 Order[2] = { Off, 1 - Off };
		int32 Holding = (Equipped[0] == Wd ? 1 : 0) + (Equipped[1] == Wd ? 1 : 0);
		const int32 Allowed = DropWeapons[Wd].bThrown ? (Left > 0 ? 2 : 0) : FMath::Min(Left, 2);
		for (const int32 H : Order)
		{
			if (Holding <= Allowed) break;
			if (Equipped[H] != Wd) continue;
			Equip(H, -1);
			--Holding;
		}
	}
	for (int32 H = 0; H < 2; ++H)
	{
		if (!HeldTool[H].IsEmpty() && HeldTool[H].Equals(Item, ESearchCase::IgnoreCase) && Item != TEXT("ibplate") && L->GetCount(Item) <= 0) DropTool(H);
	}
	Pulse(Hand, 0.5f);
	L->Notify(TEXT("Dropped: ") + Label, 1.5f);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT wheel drop by the %s hand: %s (slot %d, %s), hands %d %d"), Hand == 0 ? TEXT("left") : TEXT("right"), *Label, Pick, *What, Equipped[0], Equipped[1]);
	// the wheel stays up, its counts and greyed slots as they are now
	L->GetWheelItems(AllWheelItems, AllWheelItemLabels);
	WheelItemStart = FMath::Clamp(WheelItemStart, 0, FMath::Max(0, AllWheelItems.Num() - WheelItemsShown));
	RebuildWheelSlots();
	WheelPick[0] = WheelPick[1] = -1;
	WheelDropPick[0] = WheelDropPick[1] = -1;
	UpdateWheel();
	return true;
}

#if WITH_OUTLAWS_TESTS
bool AOutlawsPlayer::WheelDropTest(int32 Hand, int32 Weapon, const FString& Item, bool bClose)
{
	if (!bWheelOpen || !bWheelHeld[Hand]) OpenWheel(Hand);
	if (!bWheelOpen) return false;
	int32 Slot = -1;
	for (int32 i = 0; i < WheelWeapon.Num() && Slot < 0; ++i)
	{
		if (Weapon >= -1 ? WheelWeapon[i] == Weapon : (WheelWeapon[i] == -2 && WheelItem.IsValidIndex(i) && WheelItem[i].Equals(Item, ESearchCase::IgnoreCase))) Slot = i;
	}
	bool bOk = false;
	if (Slot < 0)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT wheel drop test: %s is not on the wheel"), Weapon >= -1 ? *FString::Printf(TEXT("weapon %d"), Weapon) : *Item);
	}
	else
	{
		WheelPick[Hand] = Slot;
		WheelDropPick[Hand] = Slot;
		bOk = WheelDrop(Hand);
	}
	if (bClose) CloseWheel();
	return bOk;
}
#endif // WITH_OUTLAWS_TESTS

// ---- -OLShots -OLWheelDropTest (single player, the hideout) -------------------------------------------------------------------
// Four revolvers picked up (from the floor, as pickups; alone the pair is all, BUG-PISTOLCAP), a shotgun, a medical bag, the
// brass key and a boiler plate; slot 9 saved (the pair in the file); then from the wheel: the second (the pair goes from the
// hands), the shotgun, the bag, the key, the plate; the map, the lantern and the fists refused; the last revolver.  Standing on the first drop takes nothing (it is
// locked); 2.4 m away they arm and are photographed (Saved/OLShots/ol_wheel_drops.png); slot 9 saved with them lying there;
// back onto the first, it is taken; slot 9 loaded (each comes back giving what it gave); walked over, each is taken and the
// counts are as they were.

#if WITH_OUTLAWS_TESTS
void AOutlawsPlayer::WheelDropTestTick()
{
	using namespace OutlawsWheelDropLocal;
	FWheelDropTestState& T = WheelDropTestState;
	AOutlawsLevelActor* L = Level.Get();
	UWorld* World = GetWorld();
	UOutlawsCoop* C = UOutlawsCoop::Get(this);
	if (!L || !World || !C || L->IsFrontEnd() || L->PendingLoad.IsValid()) return;
	const double Now = FPlatformTime::Seconds();
	if (Now < T.At) return;
	const double Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	auto PutAt = [this, Half](const FVector& FeetPos)
	{
		SetActorLocation(FeetPos + FVector(0.0, 0.0, Half + 2.0), false, nullptr, ETeleportType::TeleportPhysics);
		GetCharacterMovement()->Velocity = FVector::ZeroVector;
	};
	auto Say = [](const FString& Line) { UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wheel drop test: %s"), *Line); };
	const FVector HomeFeet = T.Home - FVector(0.0, 0.0, Half);
	switch (T.Step)
	{
	case 0:
	{
		if (T.At == 0.0) { T.At = Now + 4.0; return; }          // (the level settles first)
		T.Home = GetActorLocation();
		T.Ahead = FVector(Camera->GetForwardVector().X, Camera->GetForwardVector().Y, 0.0).GetSafeNormal();
		if (T.Ahead.IsNearlyZero()) T.Ahead = FVector::ForwardVector;
		// the start kit's revolver put away and out of the pockets: four are picked up from the floor
		Equip(0, -1);
		Equip(1, -1);
		L->TakeCount(TEXT("ipistol"), L->GetCount(TEXT("ipistol")));
		const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, Half);
		auto Take = [&](const TCHAR* Item, int32 Times)
		{
			const FString Ground = Item == FString(TEXT("idynam")) ? FString(TEXT("gamboxt")) : C->GroundItemFor(L, Item);
			for (int32 k = 0; k < Times; ++k)
			{
				AActor* A = L->SpawnGroundItem(Ground, Feet);
				const bool bGot = A && L->CollectPickupActor(A, true);
				Say(FString::Printf(TEXT("picked up %s (%s): %s; %s"), *Ground, Item, bGot ? TEXT("taken") : TEXT("NOT TAKEN"), *DropCountsLine(L)));
			}
		};
		Take(TEXT("ipistol"), 4);
		Take(TEXT("isgun"), 1);
		Take(TEXT("idocbag"), 1);
		Take(TEXT("ibrsskey"), 1);
		Take(TEXT("ibplate"), 1);
		Equip(0, 0);
		Equip(1, 0);
		T.PlateArmour = L->GetArmour();
		Say(FString::Printf(TEXT("carried: %s; hands %d %d"), *DropCountsLine(L), Equipped[0], Equipped[1]));
		const bool bSaved = L->SaveGame(9);
		Say(FString::Printf(TEXT("saved to slot 9: %s; the file's ipistol %d (the pair, no spares alone: %s)"), bSaved ? TEXT("yes") : TEXT("NO"),
			DropSavedCount(DropReadSave(9), TEXT("ipistol")), DropSavedCount(DropReadSave(9), TEXT("ipistol")) == 2 ? TEXT("yes") : TEXT("NO")));
		T.Step = 1;
		T.At = Now + 1.0;
		return;
	}
	case 1:
	{
		// the second revolver (the pair goes from the hands); then standing on it takes nothing
		const bool bOk = WheelDropTest(1, 0, FString(), false);
		bool bLocked = false;
		const FString Desc = L->DescribePickup(LastWheelDrop.Get(), &bLocked);
		Say(FString::Printf(TEXT("drop 1 (second (dual) revolver): %s -> %s (locked %d); %s; hands %d %d"), bOk ? TEXT("dropped") : TEXT("NOT DROPPED"), *Desc, bLocked ? 1 : 0,
			*DropCountsLine(L), Equipped[0], Equipped[1]));
		if (bOk) { ++T.Dropped; T.Drops.Add(LastWheelDrop); } else ++T.Failed;
		if (LastWheelDrop.IsValid()) PutAt(LastWheelDrop->GetActorLocation());
		T.Step = 2;
		T.At = Now + 1.2;
		return;
	}
	case 2:
	{
		bool bLocked = false;
		const FString Desc = T.Drops.Num() ? L->DescribePickup(T.Drops[0].Get(), &bLocked) : FString();
		T.bLockHeld = !Desc.IsEmpty() && L->GetCount(TEXT("ipistol")) == 1;
		Say(FString::Printf(TEXT("standing on the first drop 1.2 s: %s (%s, locked %d, ipistol %d)"), T.bLockHeld ? TEXT("not taken, as it should be") : TEXT("TAKEN BACK AT ONCE"),
			Desc.IsEmpty() ? TEXT("gone") : *Desc, bLocked ? 1 : 0, L->GetCount(TEXT("ipistol"))));
		PutAt(HomeFeet);
		T.Step = 3;
		T.Next = 0;
		T.At = Now + 0.5;
		return;
	}
	case 3:
	{
		struct FDropTry { int32 Weapon; const TCHAR* Item; const TCHAR* Name; bool bRefuse; };
		static const FDropTry Tries[] = {
			{ 2, TEXT(""), TEXT("shotgun"), false }, { -2, TEXT("idocbag"), TEXT("medical bag"), false }, { -2, TEXT("ibrsskey"), TEXT("brass key"), false },
			{ -2, TEXT("ibplate"), TEXT("boiler plate"), false },
			{ -2, TEXT("imap"), TEXT("map (no wheel item now)"), true }, { -2, TEXT("ilantern"), TEXT("lantern"), true }, { -1, TEXT(""), TEXT("fists"), true },
			{ 0, TEXT(""), TEXT("last revolver"), false } };
		if (T.Next >= int32(UE_ARRAY_COUNT(Tries)))
		{
			CloseWheel();
			T.Step = 4;
			return;
		}
		const FDropTry& D = Tries[T.Next++];
		LastWheelDrop.Reset();
		const bool bOk = WheelDropTest(1, D.Weapon, D.Item, false);
		const FString Desc = bOk ? L->DescribePickup(LastWheelDrop.Get()) : FString();
		Say(FString::Printf(TEXT("drop %d (%s): %s%s; %s; hands %d %d"), T.Next + 1, D.Name, bOk ? TEXT("dropped -> ") : (D.bRefuse ? TEXT("refused, as it should be") : TEXT("NOT DROPPED")),
			*Desc, *DropCountsLine(L), Equipped[0], Equipped[1]));
		if (bOk && !D.bRefuse) { ++T.Dropped; T.Drops.Add(LastWheelDrop); }
		else if (!bOk && D.bRefuse) ++T.Refused;
		else ++T.Failed;
		T.At = Now + 0.6;
		return;
	}
	case 4:
	{
		// 2.4 m from where they lie (the most open way round), the eyes on them: every drop arms for the dropper
		FVector Cen = FVector::ZeroVector;
		int32 Here = 0, Locked = 0;
		for (const TWeakObjectPtr<AActor>& D : T.Drops)
		{
			bool bLocked = false;
			if (!D.IsValid() || L->DescribePickup(D.Get(), &bLocked).IsEmpty()) continue;
			Cen += D->GetActorLocation();
			++Here;
			if (bLocked) ++Locked;
		}
		Cen = Here ? Cen / double(Here) : HomeFeet;
		Say(FString::Printf(TEXT("before stepping away: %d of %d drops locked for the dropper"), Locked, Here));
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsWheelDropTest), false, this);
		const FVector Chest = Cen + FVector(0.0, 0.0, Half);
		FVector Best = Chest;
		double BestOpen = -1.0;
		for (int32 k = 0; k < 8; ++k)
		{
			const FVector Dir = FRotator(0.0, 45.0 * k, 0.0).Vector();
			FHitResult Hit;
			const double Open = World->LineTraceSingleByChannel(Hit, Chest, Chest + Dir * 300.0, ECC_Visibility, Q) ? Hit.Distance : 300.0;
			if (Open > BestOpen) { BestOpen = Open; Best = Chest + Dir * 240.0; }
		}
		PutAt(Best - FVector(0.0, 0.0, Half));
		const FVector Look = Cen - Camera->GetComponentLocation();
		if (APlayerController* PC = Cast<APlayerController>(GetController())) PC->SetControlRotation(FRotator(float(Look.Rotation().Pitch), float(Look.Rotation().Yaw), 0.f));
		Say(FString::Printf(TEXT("stepped %.0f cm from the drops (open %.0f cm)"), FVector::Dist2D(Best, Cen), BestOpen));
		T.Step = 5;
		T.At = Now + 1.2;
		return;
	}
	case 5:
	{
		int32 Armed = 0, Here = 0;
		for (const TWeakObjectPtr<AActor>& D : T.Drops)
		{
			bool bLocked = true;
			if (!D.IsValid() || L->DescribePickup(D.Get(), &bLocked).IsEmpty()) continue;
			++Here;
			if (!bLocked) ++Armed;
		}
		Say(FString::Printf(TEXT("after stepping away: %d of %d drops armed for the dropper"), Armed, Here));
		const FString Path = FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_wheel_drops.png");
		FScreenshotRequest::RequestScreenshot(Path, false, false);
		Say(FString::Printf(TEXT("photographed %s: %d of %d drops lie there"), *Path, Here, T.Drops.Num()));
		T.Step = 6;
		T.At = Now + 1.0;
		return;
	}
	case 6:
	{
		// saved with the drops lying there (each to come back giving what it gave), then back onto the first drop
		const bool bSaved = L->SaveGame(9);
		TArray<FString> InFile;
		if (const TSharedPtr<FJsonObject> O = DropReadSave(9))
		{
			const TArray<TSharedPtr<FJsonValue>>* Ps = nullptr;
			if (O->TryGetArrayField(TEXT("pickups"), Ps))
			{
				for (const TSharedPtr<FJsonValue>& V : *Ps)
				{
					const TSharedPtr<FJsonObject> J = V->AsObject();
					FString Ground, Contents;
					if (J.IsValid() && J->GetNumberField(TEXT("serial")) < 0 && J->TryGetStringField(TEXT("ground"), Ground))
					{
						J->TryGetStringField(TEXT("contents"), Contents);
						InFile.Add(Ground + TEXT(" ") + Contents);
					}
				}
			}
			Say(FString::Printf(TEXT("saved to slot 9 with the drops: %s; the file's ipistol %d; dropped in the file: %s"), bSaved ? TEXT("yes") : TEXT("NO"),
				DropSavedCount(O, TEXT("ipistol")), *FString::Join(InFile, TEXT(" | "))));
		}
		T.BackDesc = T.Drops.Num() && T.Drops[0].IsValid() ? L->DescribePickup(T.Drops[0].Get()) : FString();
		if (T.Drops.Num() && T.Drops[0].IsValid()) PutAt(T.Drops[0]->GetActorLocation());
		T.Step = 7;
		T.At = Now + 0.8;
		return;
	}
	case 7:
	{
		const bool bGone = T.Drops.Num() && (!T.Drops[0].IsValid() || L->DescribePickup(T.Drops[0].Get()).IsEmpty());
		Say(FString::Printf(TEXT("back onto the first drop (%s) after stepping away: %s; %s"), *T.BackDesc, bGone ? TEXT("taken") : TEXT("NOT TAKEN"), *DropCountsLine(L)));
		if (bGone) ++T.WalkedBack;
		const bool bLoading = L->LoadGame(9);
		Say(FString::Printf(TEXT("loading slot 9: %s"), bLoading ? TEXT("yes") : TEXT("NO")));
		T.Step = 8;
		T.At = Now + 3.0;
		return;
	}
	case 8:
	{
		TArray<AActor*> Dropped;
		L->DroppedPickups(Dropped);
		TArray<FString> Descs;
		T.Back.Reset();
		for (AActor* A : Dropped)
		{
			Descs.Add(L->DescribePickup(A));
			T.Back.Add(A);
		}
		Say(FString::Printf(TEXT("after the load: %s; dropped things back: %d (%s)"), *DropCountsLine(L), Dropped.Num(), *FString::Join(Descs, TEXT(" | "))));
		T.BackAt = -1;
		T.Step = 9;
		T.At = Now + 0.5;
		return;
	}
	case 9:
	{
		// each walked over in turn
		if (T.BackAt >= 0)
		{
			const bool bGone = !T.Back[T.BackAt].IsValid() || L->DescribePickup(T.Back[T.BackAt].Get()).IsEmpty();
			Say(FString::Printf(TEXT("walked over %s: %s; %s"), *T.BackDesc, bGone ? TEXT("taken") : TEXT("NOT TAKEN"), *DropCountsLine(L)));
		}
		do { ++T.BackAt; } while (T.BackAt < T.Back.Num() && (!T.Back[T.BackAt].IsValid() || L->DescribePickup(T.Back[T.BackAt].Get()).IsEmpty()));
		if (T.BackAt >= T.Back.Num())
		{
			T.Step = 10;
			return;
		}
		T.BackDesc = L->DescribePickup(T.Back[T.BackAt].Get());
		PutAt(T.Back[T.BackAt]->GetActorLocation());
		T.At = Now + 0.8;
		return;
	}
	case 10:
	{
		const bool bBack = L->GetCount(TEXT("ipistol")) == 2 && L->GetCount(TEXT("isgun")) == 1 && L->GetCount(TEXT("idocbag")) >= 3 && L->GetCount(TEXT("ibrsskey")) == 1
			&& L->GetArmour() == T.PlateArmour;
		Say(FString::Printf(TEXT("RESULT dropped %d, refused %d, failed %d, lock held %d, taken back after stepping away %d, all back after the load %d: %s"), T.Dropped,
			T.Refused, T.Failed, T.bLockHeld ? 1 : 0, T.WalkedBack, bBack ? 1 : 0, *DropCountsLine(L)));
		T.Step = 11;
		T.At = Now + 1.0;
		return;
	}
	default:
		T.Step = 12;
		T.At = Now + 1000.0;
		FGenericPlatformMisc::RequestExit(false);
		return;
	}
}

OL_SHOT_MATCH(OLWheelDropTest) { return OLFlags::Param(TEXT("OLWheelDropTest")); }
OL_SHOT_TEST(OLWheelDropTest, 260)
{
	WheelDropTestTick();             // (OutlawsWheelDrop.cpp)
}
#endif // WITH_OUTLAWS_TESTS

// ---- -OLCoopAuto -OLCoopTest=pass (co-op) -------------------------------------------------------------------------------------
// The host carries a pair of revolvers and a medical bag, the client one revolver and a spare.  The host drops a revolver
// and the bag from its wheel (each numbered by the host and made on the client) and stands on its own revolver (locked for
// it: not taken); the client sees both as pickups and walks over them: the counts move from the host to the client, and the
// host's copies go.  Then the host saves slot 9: the host's revolver count and the client's spares (player2) are in it.

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TickHostPassTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	using namespace OutlawsWheelDropLocal;
	if (Now < TestAt) return;
	const double Half = P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	switch (TestStep)
	{
	case 400:
	{
		L->bInvincible = true;
		L->AddCount(TEXT("ipistol"), 1);
		L->AddCount(TEXT("idocbag"), MedBagPoints(L));
		UE_LOG(LogOutlaws, Log, TEXT("OLCOOP test: pass: the host carries %s"), *DropCountsLine(L));
		TestSay(TEXT("wd:prep"), 0);
		TestStep = 401;
		TestAt = Now + 2.0;
		return;
	}
	case 401:
	{
		PassIds.Reset();
		TestNotes.RemoveAll([](const FString& N) { return N.StartsWith(TEXT("wd:")); });
		const bool bGun = P->WheelDropTest(1, 0, FString(), false);
		AActor* Gun = P->LastWheelDrop.Get();
		const bool bBag = P->WheelDropTest(1, -2, TEXT("idocbag"), true);
		AActor* Bag = P->LastWheelDrop.Get();
		for (AActor* A : { Gun, Bag })
		{
			const int32 i = PickupIndexOf(L, A);
			bool bLocked = false;
			const FString Desc = L->DescribePickup(A, &bLocked);
			PassIds.Add(i >= 0 ? L->Pickups[i].CoopId : -1);
			UE_LOG(LogOutlaws, Log, TEXT("OLCOOP test: pass: the host dropped %s as pickup %d (locked for the host %d)"), *Desc, PassIds.Last(), bLocked ? 1 : 0);
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLCOOP test: pass: drops %s; the host now carries %s"), bGun && bBag ? TEXT("both made") : TEXT("NOT MADE"), *DropCountsLine(L));
		// the host stands on its own revolver: it is not the host's to take yet
		if (Gun)
		{
			P->SetActorLocation(Gun->GetActorLocation() + FVector(0.0, 0.0, Half + 2.0), false, nullptr, ETeleportType::TeleportPhysics);
		}
		for (const int32 Id : PassIds) TestSay(TEXT("wd:take"), Id);
		TestStep = 402;
		TestAt = Now + 0.5;
		PassAt = Now + 20.0;
		return;
	}
	case 402:
	{
		const int32 PassNotes = TestNotes.FilterByPredicate([](const FString& N) { return N.StartsWith(TEXT("wd:")); }).Num();
		if (PassNotes < PassIds.Num() && Now < PassAt) { TestAt = Now + 0.25; return; }
		TArray<FString> Gone;
		for (const int32 Id : PassIds) Gone.Add(FString::Printf(TEXT("%d %s"), Id, PickupIndex(L, Id) < 0 ? TEXT("gone here") : TEXT("STILL HERE")));
		UE_LOG(LogOutlaws, Log, TEXT("OLCOOP test: pass: the client's notes %d of %d; the host's copies: %s; the host carries %s"), PassNotes, PassIds.Num(),
			*FString::Join(Gone, TEXT(", ")), *DropCountsLine(L));
		TestStep = 403;
		TestAt = Now + 3.0;                      // (the client's player2 block is sent once a second)
		return;
	}
	case 403:
	{
		const bool bSaved = L->SaveGame(9);
		const TSharedPtr<FJsonObject> O = DropReadSave(9);
		int32 Mine = DropSavedCount(O, TEXT("ipistol")), Theirs = -1;
		const TSharedPtr<FJsonObject>* P2 = nullptr;
		if (O.IsValid() && O->TryGetObjectField(TEXT("player2"), P2)) Theirs = DropSavedCount(*P2, TEXT("ipistol"));
		UE_LOG(LogOutlaws, Log, TEXT("OLCOOP test: pass: saved to slot 9: %s; the host's ipistol %d, the client's (player2) ipistol %d"), bSaved ? TEXT("yes") : TEXT("NO"), Mine, Theirs);
		TestSay(TEXT("quit"), 0);
		TestStep = 100;
		TestAt = Now + 3.0;
		return;
	}
	default:
		return;
	}
}

void UOutlawsCoop::OnPassTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	using namespace OutlawsWheelDropLocal;
	if (Step == TEXT("prep"))
	{
		L->bInvincible = true;
		L->AddCount(TEXT("ipistol"), 1);
		UE_LOG(LogOutlaws, Log, TEXT("OLCOOP test: pass: the client carries %s"), *DropCountsLine(L));
		return;
	}
	if (Step != TEXT("take")) return;
	PassQueue.Add(Arg);
	if (PassQueue.Num() > 1) return;             // (the one before is being walked over)
	// each in turn: seen here as a pickup, walked over, taken (a timer looks a second and a half on)
	TWeakObjectPtr<UOutlawsCoop> WeakC(this);
	TSharedRef<TFunction<void()>> Walk = MakeShared<TFunction<void()>>();
	*Walk = [WeakC, Walk]()
	{
		UOutlawsCoop* C = WeakC.Get();
		AOutlawsLevelActor* Lv = C ? C->FindLevel() : nullptr;
		AOutlawsPlayer* Pl = C ? C->LocalPlayer() : nullptr;
		if (!C || !Lv || !Pl || C->PassQueue.Num() == 0) return;
		const int32 Id = C->PassQueue[0];
		const int32 i = PickupIndex(Lv, Id);
		AActor* A = i >= 0 ? Lv->Pickups[i].Actor.Get() : nullptr;
		bool bLocked = false;
		const FString Desc = Lv->DescribePickup(A, &bLocked);
		UE_LOG(LogOutlaws, Log, TEXT("OLCOOP test: pass: the client sees pickup %d: %s (locked here %d); it carries %s"), Id, Desc.IsEmpty() ? TEXT("NOT HERE") : *Desc,
			bLocked ? 1 : 0, *DropCountsLine(Lv));
		if (A)
		{
			const double Hh = Pl->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			Pl->SetActorLocation(A->GetActorLocation() + FVector(0.0, 0.0, Hh + 2.0), false, nullptr, ETeleportType::TeleportPhysics);
		}
		FTimerHandle Th;
		Pl->GetWorldTimerManager().SetTimer(Th, FTimerDelegate::CreateLambda([WeakC, Walk, Id, Desc]()
		{
			UOutlawsCoop* C2 = WeakC.Get();
			AOutlawsLevelActor* L2 = C2 ? C2->FindLevel() : nullptr;
			if (!C2 || !L2) return;
			const bool bTaken = PickupIndex(L2, Id) < 0;
			const FString Note = FString::Printf(TEXT("wd: pickup %d (%s) %s; the client carries %s"), Id, *Desc, bTaken ? TEXT("taken") : TEXT("NOT TAKEN"), *DropCountsLine(L2));
			UE_LOG(LogOutlaws, Log, TEXT("OLCOOP test: pass: %s"), *Note);
			if (AOutlawsCoopLink* Mine = C2->GetLink(C2->MyIndex())) Mine->ServerTestNote(Note);
			if (C2->PassQueue.Num()) C2->PassQueue.RemoveAt(0);
			if (C2->PassQueue.Num()) (*Walk)();
		}), 1.5f, false);
	};
	(*Walk)();
}
#endif // WITH_OUTLAWS_TESTS
