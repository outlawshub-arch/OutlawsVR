// Story co-op (tools/olue/COOP_DESIGN.md), milestone 7: the co-op rules.
// - Death: a player who dies sees the death view and the session goes on for the other (the level does not wait while
//   one player lies dead, as it does in single player).  The other machine draws the body lying where it fell.
// - The user's rule (2026-10-02): a player who dies drops their keys and puzzle pieces where they fell, as ground items
//   any player can take (numbered by the host as any dropped item is, milestone 3).
// - Revive: the other player grips the body; their medical bag points bring the dead player back where they fell (user
//   2026-10-02: a bag is 3 points; a revive needs at least a bag's worth and gives back at most half health, 5 of 10, taking
//   from the reviver exactly what it gives).  Without a bag's worth the grip only says so.
// - Respawn: the dead player's own button takes them back to the level's start with the start kit (user 2026-10-02: "game
//   start kit is fine, as long as the guns they acquired were dropped at death and they can go pick them up"): at a death
//   the weapons beyond the start kit, with their own ammunition, drop where the player fell as shared pickups that give
//   the same back, beside the keys; either player can take them up.
// - The movement speed settings (user 2026-10-02): the bad guys go at the two players' average pace, the timed doors and
//   lifts wait at the slower player's; each player's own body keeps its own.
// - Both dead: the host restarts the level for both, as single player's death restarts it.
// - The pause menu (2026-10-02, the user: "one player should be able to continue if the other pauses"): a player's menu
//   opens for that player only and pauses nothing; the game goes on for both.  While it is open that player is out of
//   harm (UOutlawsCoop::bMenuKeepsSafe): the host's bad guys leave them be, as they leave a dead player, and nothing hurts
//   them; a lift still carries them.
//
// None of this runs in single player: the level actor and the player call in only when UOutlawsCoop::IsActive.
#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsEnemy.h"
#include "OutlawsRemotePlayer.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsLab.h"
#include "OutlawsFormats.h"
#include "OutlawsBoxHand.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "Misc/Paths.h"

namespace
{
	const TCHAR* RuleWho(int32 Who) { return UOutlawsCoop::WhoWord(Who); }
	constexpr int32 ReviveCap = 5;                   // a revive gives back at most half health (user 2026-10-02)
	constexpr double ReviveReach = 150.0;            // a hand this near the body's feet, across, and low enough, grips it
	constexpr double DropRing = 35.0;                // the keys and guns dropped at a death lie this far round the feet (a VR player
	                                                 // stood up there takes them: the feet's reach is 45 cm and half the item)
	constexpr double BothDeadWait = 3.0;             // both dead this long: the level again (the death views fall first)
}

// ---- the link's milestone 7 messages ----------------------------------------------------------------------------------

void AOutlawsCoopLink::ServerRevive_Implementation(int32 Epoch, int32 Target, int32 Health)
{
	if (!Admit(EOLGate::Revive, Health >= 0 && Health <= 10 && Target >= -1 && Target < UOutlawsCoop::MaxPlayers)) return;
	UOutlawsCoop* C = UOutlawsCoop::Get(this);
	if (!C) return;
	if (Target <= 0)
	{
		C->OnRevive(Epoch, PlayerIndex, Health);
		return;
	}
	// (N1: one client revived another: passed on to that player's machine)
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the %s revives the %s: passed on (health %d)"), UOutlawsCoop::WhoWord(PlayerIndex), UOutlawsCoop::WhoWord(Target), Health);
	if (AOutlawsCoopLink* T = C->GetLink(Target)) T->ClientRevive(Epoch, PlayerIndex, Health);
}

void AOutlawsCoopLink::ClientRevive_Implementation(int32 Epoch, int32 By, int32 Health)
{
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnRevive(Epoch, By, Health);
}

void AOutlawsCoopLink::ServerPace_Implementation(float InPace)
{
	if (!Admit(EOLGate::Pace, Finite(InPace, 10.f) && InPace > 0.f)) return;
	Pace = FMath::Min(InPace, AOutlawsLevelActor::MaxPace());          // (BUG-SPEEDCAP: no one's pace is over the dial's top)
	ForceNetUpdate();
}

// ---- each frame: deaths, both dead, a player in the menu ----------------------------------------------------------------

void UOutlawsCoop::TickRules(double Now)
{
	AOutlawsCoopSession* S = GetSession();
	if (!bLevelBuilt || !S) return;
	AOutlawsLevelActor* L = FindLevel();
	AOutlawsPlayer* P = LocalPlayer();
	if (!L) return;
	// this machine's player died: logged once, its keys left where it fell
	const bool bDeadNow = L->bDead;
	if (bDeadNow && !bLocalDead) OnLocalDeath(L, P);
	if (!bDeadNow && DeathDropParts.Num() > 0) ShowDeathDrops(TEXT("up again"));      // (REQ-DEATHDROPHIDE; a respawn clears bLocalDead itself)
	bLocalDead = bDeadNow;
	// the others' deaths and menus, as their stats say (N1: each other player; a player in the session whose level is not
	// built yet, or who has said nothing, counts as alive)
	TArray<AOutlawsCoopLink*> Links;
	GetLinks(Links);
	bool bAnyOtherDead = false, bAllOthersDead = true, bAnyOther = false;
	for (const AOutlawsCoopLink* K : Links)
	{
		const int32 Who = K->PlayerIndex;
		if (Who == MyIndex()) continue;
		bAnyOther = true;
		const FOutlawsCoopStats& O = K->Stats;
		const bool bDead = O.Seq > 0 && O.bDead;
		bAnyOtherDead = bAnyOtherDead || bDead;
		bAllOthersDead = bAllOthersDead && bDead;
		if (bDead != OtherDeadAt[Who])
		{
			OtherDeadAt[Who] = bDead;
			const AOutlawsRemotePlayer* R = PuppetOf(Who);
			if (bDead)
			{
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP player died: %s (the other player, lying at %s)"), RuleWho(Who), R ? *R->ShownBody().ToCompactString() : TEXT("?"));
			}
			else
			{
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the %s player is up again (health %.1f)"), RuleWho(Who), O.Health);
			}
		}
		const bool bAway = O.Seq > 0 && O.bInMenu && !O.bDead;
		if (bAway != OtherAwayAt[Who])
		{
			OtherAwayAt[Who] = bAway;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the %s player's menu %s: the game goes on here%s"), RuleWho(Who), bAway ? TEXT("is open") : TEXT("closed"),
				!bAway ? TEXT("") : bMenuKeepsSafe ? TEXT(" (the bad guys leave that player be)") : TEXT(""));
		}
	}
	bOtherDead = bAnyOtherDead;
	const bool bOtherNow = bAnyOther && bAllOthersDead;
	// all dead (with two: both): the level again for all, as single player's death restarts it (the host's word)
	if (IsHost())
	{
		if (bDeadNow && bOtherNow && L->LevelEndTimer < 0.f && !IsMatch())      // (M1: a match has respawns, never a restart)
		{
			if (BothDeadSince == 0.0) BothDeadSince = Now;
			if (!bBothDeadRestart && Now - BothDeadSince > BothDeadWait)
			{
				bBothDeadRestart = true;
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP both dead: the level restarts"));
				if (Links.Num() > 2) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP all %d players dead: the level restarts"), Links.Num());
				RequestStart(EOutlawsCoopStart::Restart, L->LevelName);
			}
		}
		else
		{
			BothDeadSince = 0.0;
		}
	}
	// a pause menu (2026-10-02): this player's opens for this player only and the game goes on for both; while it is open
	// this player is out of harm (the host's bad guys leave it be: TickEnemies, CoopChooseTarget; no hurt: MenuShields)
	// (R7: alone in the session the menu stops the world as single player's does: nobody to keep playing, nothing to shield)
	const bool bAwayNow = P && P->IsMenuOpen() && !L->bDead && !IsHostAlone(L);
	if (bAwayNow != bLocalAway)
	{
		bLocalAway = bAwayNow;
		if (bAwayNow) ++MenusOpened;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP this player's menu %s (%s): the game goes on for both%s"), bAwayNow ? TEXT("opened") : TEXT("closed"), RuleWho(MyIndex()),
			!bAwayNow ? TEXT("") : bMenuKeepsSafe ? TEXT(", this player out of harm while it is open") : TEXT(", this player a target as ever"));
	}
}

bool UOutlawsCoop::MenuShields(const UObject* Context, float Damage)
{
	// (the level's DamagePlayer and the host's hurt arriving on the client ask this first, in a session only)
	UOutlawsCoop* C = Get(Context);
	if (C && C->IsMatch()) return false;             // (M1: in a match the menu hides nobody)
	if (!bMenuKeepsSafe || !C || C->Mode == EOutlawsCoopMode::None || !C->bLevelBuilt || IsHostAlone(Context)) return false;
	const AOutlawsPlayer* P = C->LocalPlayer();
	if (!P || !P->IsMenuOpen()) return false;
	++C->HurtsDropped;
	const double Now = FPlatformTime::Seconds();
	if (Now - C->MenuShieldLogAt > 1.0)
	{
		C->MenuShieldLogAt = Now;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP hurt dropped here: this player is in the menu (%.2f; %d dropped so far)"), Damage, C->HurtsDropped);
	}
	return true;
}

FString UOutlawsCoop::GroundItemFor(AOutlawsLevelActor* L, const FString& Item)
{
	const FString Key = Item.ToLower();
	if (const FString* Have = GroundOfItem.Find(Key)) return *Have;
	FString Ground;
	if (L && L->AssetsRef)
	{
		// the inventory item's own ITM names it (GROUND_ITEM gbrsskey); else a ground item whose PICKUP gives it
		FString Text, Func;
		TMap<FString, FString> Data;
		if (L->AssetsRef->GetText(Key + TEXT(".itm"), Text))
		{
			OutlawsFormats::ParseItm(Text, Func, Data);
			Ground = FPaths::GetBaseFilename(Data.FindRef(TEXT("GROUND_ITEM"))).ToLower();
		}
		if (Ground.IsEmpty())
		{
			TArray<FString> Itms;
			L->AssetsRef->ListByExtension(TEXT(".itm"), Itms);
			for (const FString& Name : Itms)
			{
				FString T, F;
				TMap<FString, FString> D;
				if (!L->AssetsRef->GetText(Name, T)) continue;
				OutlawsFormats::ParseItm(T, F, D);
				if (!F.Contains(TEXT("GroundObject"))) continue;
				if (FPaths::GetBaseFilename(D.FindRef(TEXT("PICKUP"))).Equals(Key, ESearchCase::IgnoreCase))
				{
					Ground = FPaths::GetBaseFilename(Name).ToLower();
					break;
				}
			}
		}
	}
	GroundOfItem.Add(Key, Ground);
	return Ground;
}

void UOutlawsCoop::KeyItems(const AOutlawsLevelActor* L, TArray<FString>& Out)
{
	Out.Reset();
	if (!L) return;
	for (const TPair<FString, int32>& Kv : L->Counts)
	{
		const AOutlawsLevelActor::FItemDef* D = Kv.Value > 0 ? L->ItemDefs.Find(Kv.Key) : nullptr;
		const FString Class = D ? D->Class.ToLower() : FString();
		if (!Class.Contains(TEXT("key")) && !Class.Contains(TEXT("puzz"))) continue;
		for (int32 k = 0; k < FMath::Min(Kv.Value, 8); ++k) Out.Add(Kv.Key);
	}
	Out.Sort();
}

void UOutlawsCoop::ReturnKeys(AOutlawsLevelActor* L, const TArray<FString>& Keys, const FVector& Feet, int32 Who, const TCHAR* Why)
{
	TArray<FString> Made;
	for (int32 i = 0; i < Keys.Num(); ++i)
	{
		const FString Ground = GroundItemFor(L, Keys[i]);
		const FVector At = Feet + FRotator(0.0, 360.0 * double(i) / double(FMath::Max(1, Keys.Num())) + 30.0, 0.0).Vector() * DropRing;
		if (!Ground.IsEmpty() && L->SpawnGroundItem(Ground, At)) Made.Add(Ground);
	}
	KeysDropped += Made.Num();
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP keys: the %s player %s holding %s: %d put back on the ground at %s (level %s): %s"), WhoWord(Who), Why, *FString::Join(Keys, TEXT(", ")), Made.Num(),
		*Feet.ToCompactString(), *L->LevelName, *FString::Join(Made, TEXT(", ")));
}

void UOutlawsCoop::OnLocalDeath(AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	UWorld* W = L->GetWorld();
	const FVector Body = P ? P->GetActorLocation() : L->GetPlayerStartLocation();
	const float Half = P ? P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 88.f;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP player died: %s (here, at %s)"), RuleWho(MyIndex()), *Body.ToCompactString());
	// the floor under the body (a death in the air leaves its keys on the ground below)
	FVector Feet = Body - FVector(0.0, 0.0, Half);
	if (W)
	{
		FHitResult Hit;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsCoopDeathFloor), false, P);
		if (W->LineTraceSingleByChannel(Hit, Body, Body - FVector(0.0, 0.0, Half + 2000.0), ECC_Visibility, Q)) Feet = Hit.ImpactPoint;
	}
	// (BUG-STUCKGAP, user 2026-10-05: keys are never lost in co-op) a death where no body can stand (in a wall, in a gap
	// between sectors: the first internet session's host died in one and the keys went with him) leaves the keys and guns
	// at this player's last safe floor spot, where it stood last and anyone can walk to; with none known, the middle of the
	// last sector its head was in, else the level's start
	FVector OnFloor;
	if (P && !P->SafeFeetAt(Feet, OnFloor))
	{
		const FVector Was = Feet;
		FVector Safe, Mid;
		const TCHAR* Where = TEXT("its last safe floor spot");
		if (P->LastSafeFeet(Safe)) Feet = Safe;
		else if (L->SectorMiddleSpot(P->LastHeadSector, P->GetCapsuleComponent()->GetScaledCapsuleRadius(), Half, P, Mid))
		{
			Feet = Mid - FVector(0.0, 0.0, Half + 3.0);
			Where = TEXT("the middle of the last sector its head was in");
		}
		else
		{
			Feet = L->GetPlayerStartLocation() - FVector(0.0, 0.0, Half);
			Where = TEXT("the level's start");
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP keys: the %s player died where no body can stand (%s, level %s): its keys and guns go to %s, %s"), RuleWho(MyIndex()), *Was.ToCompactString(),
			*L->LevelName, Where, *Feet.ToCompactString());
	}
	// the keys and puzzle pieces (the item classes StartNextLevel leaves behind): out of the pockets and the wrist's list,
	// out of the INF's items, onto the ground round the body
	TArray<TPair<FString, int32>> Drop;
	for (const TPair<FString, int32>& Kv : L->Counts)
	{
		if (Kv.Value <= 0) continue;
		const AOutlawsLevelActor::FItemDef* D = L->ItemDefs.Find(Kv.Key);
		const FString Class = D ? D->Class.ToLower() : FString();
		if (Class.Contains(TEXT("key")) || Class.Contains(TEXT("puzz"))) Drop.Emplace(Kv.Key, Kv.Value);
	}
	// (user 2026-10-02) and the guns found beyond the start kit, with their own ammunition, each as its ground item giving
	// exactly what was carried (GunDrops takes them out of the pockets)
	TArray<FDrop> Guns;
	GunDrops(L, Guns);
	int32 Total = Guns.Num();
	for (const TPair<FString, int32>& Kv : Drop) Total += Kv.Value;
	int32 Slot = 0;
	auto RingAt = [&Slot, Total, &Feet]()
	{
		// in a ring round the feet, one after another
		return Feet + FRotator(0.0, 360.0 * double(Slot++) / double(FMath::Max(1, Total)) + 30.0, 0.0).Vector() * DropRing;
	};
	int32 Made = 0;
	TArray<FString> Names;
	for (const TPair<FString, int32>& Kv : Drop)
	{
		L->Counts.Remove(Kv.Key);
		for (int32 i = L->Inventory.Num() - 1; i >= 0; --i)
		{
			if (!L->Inventory[i].Equals(Kv.Key, ESearchCase::IgnoreCase)) continue;
			L->Inventory.RemoveAt(i);
			if (L->InventoryLabels.IsValidIndex(i)) L->InventoryLabels.RemoveAt(i);
		}
		if (const AOutlawsLevelActor::FItemDef* D = L->ItemDefs.Find(Kv.Key); D && D->UseId >= 0 && L->Inf) L->Inf->Items.Remove(D->UseId);
		const FString Ground = GroundItemFor(L, Kv.Key);
		for (int32 k = 0; k < Kv.Value && !Ground.IsEmpty(); ++k)
		{
			if (AActor* KeyA = L->SpawnGroundItem(Ground, RingAt()))
			{
				HideDeathDrop(KeyA);                 // (REQ-DEATHDROPHIDE: not before this player's own eyes till it is up)
				++Made;
				Names.Add(Ground);
			}
		}
	}
	KeysDropped += Made;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP keys dropped by the %s player: %d%s"), RuleWho(MyIndex()), Made,
		Names.Num() ? *FString::Printf(TEXT(" (%s at %s)"), *FString::Join(Names, TEXT(", ")), *Feet.ToCompactString()) : TEXT(""));
	TArray<FString> GunNames;
	for (const FDrop& G : Guns)
	{
		// (the other machine is told the name with the contents, so its copy gives the same: NoteGroundItem)
		GroundNameSuffix = TEXT("#") + G.Contents;
		AActor* A = L->SpawnGroundItem(G.Ground, RingAt());
		GroundNameSuffix.Reset();
		if (!A) continue;
		ApplyGroundContents(L, A, G.Contents);
		HideDeathDrop(A);                            // (REQ-DEATHDROPHIDE)
		++GunsDropped;
		TArray<FString> What;
		for (const TPair<FString, int32>& Kv : G.Give) What.Add(FString::Printf(TEXT("%s %d"), *Kv.Key, Kv.Value));
		GunNames.Add(FString::Printf(TEXT("%s (%s)"), *G.Ground, *FString::Join(What, TEXT(", "))));
	}
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP guns dropped by the %s player: %s"), RuleWho(MyIndex()), GunNames.Num() ? *FString::Join(GunNames, TEXT(", ")) : TEXT("none"));
	if (DeathDropParts.Num() > 0)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP death drops hidden here: %d things (%d parts) kept out of the %s player's own view till it is up"), Made + GunNames.Num(),
			DeathDropParts.Num(), RuleWho(MyIndex()));
	}
	if (Made > 0 || GunNames.Num() > 0) L->Notify(GunNames.Num() > 0 ? TEXT("Your guns and keys lie where you fell") : TEXT("Your keys lie where you fell"), 3.f);
}

void UOutlawsCoop::HideDeathDrop(AActor* A)
{
	// the drawing only, on this machine: out of the main and depth passes (the portal culling owns the parts' hidden flag and
	// the pickups' own rules the actor's), so it stays a pickup here and is drawn on every other machine
	if (!A) return;
	TArray<UPrimitiveComponent*> Parts;
	A->GetComponents<UPrimitiveComponent>(Parts);
	for (UPrimitiveComponent* C : Parts)
	{
		if (!C || !C->bRenderInMainPass) continue;
		C->SetRenderInMainPass(false);
		C->SetRenderInDepthPass(false);
		DeathDropParts.Add(C);
	}
	++DeathDropsHidden;
}

void UOutlawsCoop::ShowDeathDrops(const TCHAR* Why)
{
	int32 Parts = 0;
	TSet<const AActor*> Things;
	for (const TWeakObjectPtr<UPrimitiveComponent>& Wk : DeathDropParts)
	{
		UPrimitiveComponent* C = Wk.Get();
		if (!C) continue;
		C->SetRenderInMainPass(true);
		C->SetRenderInDepthPass(true);
		++Parts;
		Things.Add(C->GetOwner());
	}
	if (DeathDropParts.Num() > 0)
	{
		DeathDropsShown += Things.Num();
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP death drops shown again here (%s): %d things (%d parts; %d gone meanwhile); hidden %d, shown %d in all"), Why, Things.Num(), Parts,
			DeathDropParts.Num() - Parts, DeathDropsHidden, DeathDropsShown);
	}
	DeathDropParts.Reset();
}

bool UOutlawsCoop::DrawnHere(const AActor* A)
{
	if (!A) return false;
	TArray<UPrimitiveComponent*> Parts;
	A->GetComponents<UPrimitiveComponent>(Parts);
	for (const UPrimitiveComponent* C : Parts) if (C && !C->bRenderInMainPass) return false;
	return true;
}

TMap<FString, int32> UOutlawsCoop::StartKitCounts(AOutlawsLevelActor* L) const
{
	// as ApplyStartKit gives it in the story (BUG-CHAROIL: James's basics with the worn character's own on top; the ranch:
	// fists only), the kit itself untouched
	TMap<FString, int32> Kit;
	if (!L) return Kit;
	if (L->LevelName.Equals(TEXT("ranch"), ESearchCase::IgnoreCase)) return Kit;
	return AOutlawsLevelActor::StoryKit(L->AssetsRef.Get(), L->WornCharacter);
}

void UOutlawsCoop::GunDrops(AOutlawsLevelActor* L, TArray<FDrop>& Out)
{
	// the weapons in the order the wheel has them, and the ammunition each fires (the weapon ITMs' AMMO; the dynamite and the
	// knives are their own).  A weapon beyond the start kit drops (a second revolver: the one; a shotgun: it); its ammunition
	// goes with it unless a weapon of the start kit fires the same (the revolver's bullets, the rifle's cartridges stay: the
	// kit's gun keeps them).  The dynamite's own ground item is a lit-stick projectile, not a pickup: a box of it (gamboxt).
	struct FGun { const TCHAR* Item; const TCHAR* Ammo; };
	static const FGun Guns[] = { { TEXT("ipistol"), TEXT("ibullit") }, { TEXT("irifle"), TEXT("icart") }, { TEXT("isgun"), TEXT("ishells") },
		{ TEXT("idbsgun"), TEXT("ishells") }, { TEXT("isawgun"), TEXT("ishells") }, { TEXT("igatgun"), TEXT("iclip") }, { TEXT("idynam"), TEXT("idynam") },
		{ TEXT("iknife"), TEXT("iknife") } };
	const TMap<FString, int32> Kit = StartKitCounts(L);
	TSet<FString> KitAmmo;
	for (const FGun& G : Guns)
	{
		if (FCString::Strcmp(G.Item, G.Ammo) != 0 && Kit.FindRef(G.Item) > 0) KitAmmo.Add(G.Ammo);
	}
	TSet<FString> AmmoGone;
	for (const FGun& G : Guns)
	{
		const FString Item = G.Item, Ammo = G.Ammo;
		const int32 Have = L->GetCount(Item);
		const int32 Extra = Have - Kit.FindRef(Item);
		if (Extra <= 0) continue;
		const FString Ground = Item == TEXT("idynam") ? FString(TEXT("gamboxt")) : GroundItemFor(L, Item);
		if (Ground.IsEmpty()) continue;
		FDrop D;
		D.Ground = Ground;
		D.Give.Emplace(Item, Extra);
		if (Ammo != Item && !KitAmmo.Contains(Ammo) && !AmmoGone.Contains(Ammo) && L->GetCount(Ammo) > 0)
		{
			D.Give.Emplace(Ammo, L->GetCount(Ammo));
			AmmoGone.Add(Ammo);
		}
		TArray<FString> Parts;
		for (const TPair<FString, int32>& Kv : D.Give) Parts.Add(FString::Printf(TEXT("%s=%d"), *Kv.Key, Kv.Value));
		D.Contents = FString::Join(Parts, TEXT(","));
		// out of this player's pockets and the wrist's list (the gun's lines there, the last ones first)
		for (const TPair<FString, int32>& Kv : D.Give)
		{
			int32& N = L->Counts.FindOrAdd(Kv.Key);
			N = FMath::Max(0, N - Kv.Value);
			if (N == 0) L->Counts.Remove(Kv.Key);
			int32 Unlist = L->GetCount(Kv.Key) == 0 ? MAX_int32 : Kv.Value;
			for (int32 i = L->Inventory.Num() - 1; i >= 0 && Unlist > 0; --i)
			{
				if (!L->Inventory[i].Equals(Kv.Key, ESearchCase::IgnoreCase)) continue;
				L->Inventory.RemoveAt(i);
				if (L->InventoryLabels.IsValidIndex(i)) L->InventoryLabels.RemoveAt(i);
				--Unlist;
			}
		}
		Out.Add(MoveTemp(D));
	}
}

FString UOutlawsCoop::SplitGroundName(const FString& Name, FString& OutContents)
{
	FString Base;
	if (Name.Split(TEXT("#"), &Base, &OutContents)) return Base;
	OutContents.Reset();
	return Name;
}

void UOutlawsCoop::ApplyGroundContents(AOutlawsLevelActor* L, AActor* A, const FString& Contents)
{
	// "isgun=1,ishells=17": the pickup gives those, in place of its ITM's own counts (each entry keeps the ITM's message and
	// class where it has that item; one it lacks takes the item table's class)
	const int32 i = PickupIndexOf(L, A);
	if (i < 0 || Contents.IsEmpty()) return;
	AOutlawsLevelActor::FPickup& P = L->Pickups[i];
	TArray<AOutlawsLevelActor::FPickup::FEntry> Entries;
	TArray<FString> Parts;
	Contents.ParseIntoArray(Parts, TEXT(","));
	for (const FString& Part : Parts)
	{
		FString Item, Num;
		if (!Part.Split(TEXT("="), &Item, &Num)) continue;
		Item = Item.ToLower();
		const int32 N = FCString::Atoi(*Num);
		if (Item.IsEmpty() || N <= 0) continue;
		AOutlawsLevelActor::FPickup::FEntry E;
		if (const AOutlawsLevelActor::FPickup::FEntry* Own = P.Entries.FindByPredicate([&Item](const AOutlawsLevelActor::FPickup::FEntry& X) { return X.Item == Item; }))
		{
			E = *Own;
		}
		else
		{
			E.Item = Item;
			if (const AOutlawsLevelActor::FItemDef* D = L->ItemDefs.Find(Item)) E.Class = D->Class;
		}
		E.Count = N;
		Entries.Add(E);
	}
	if (Entries.Num() == 0) return;
	P.Entries = MoveTemp(Entries);
	P.Item = P.Entries[0].Item;
	P.Message = P.Entries[0].Message;
	const int32* Id = L->UseIdByItem.Find(P.Item);
	P.UseId = Id ? *Id : -1;
}

// ---- revive and respawn ---------------------------------------------------------------------------------------------------

bool UOutlawsCoop::TryRevive(AOutlawsPlayer* P, const FVector& Hand)
{
	UOutlawsCoop* C = Get(P);
	if (C && C->IsMatch()) return false;            // (M1: no revives in a match)
	if (!C || !C->bLevelBuilt || !C->bOtherDead) return false;
	AOutlawsLevelActor* L = C->FindLevel();
	if (!L || L->bDead) return false;
	// the body lies on the floor round its feet: the hand near them across and low enough (N1: the nearest dead player's)
	int32 Target = -1;
	FVector Feet = FVector::ZeroVector;
	double Best = ReviveReach;
	for (int32 Who = 0; Who < MaxPlayers; ++Who)
	{
		const AOutlawsRemotePlayer* R = C->PuppetOf(Who);
		if (!R || !C->OtherDeadAt[Who]) continue;
		const FVector At = R->ShownBody() - FVector(0.0, 0.0, R->ShownHalf());
		const double D = FVector::Dist2D(Hand, At);
		if (D > Best || Hand.Z > At.Z + 120.0) continue;
		Best = D;
		Target = Who;
		Feet = At;
	}
	if (Target < 0) return false;
	const double Now = FPlatformTime::Seconds();
	if (Now - C->ReviveSentAt < 2.0) return true;           // (a revive on its way: the same grip again spends nothing)
	// (user 2026-10-02) the medical bag is counted in healing points (idocbag's count; a bag on the ground gives 3, and single
	// player's bag heals one point of health for each): a revive needs a bag's worth, and gives the points back as health, but
	// never more than half health, so it takes from the reviver exactly what it gives and wastes none
	const int32 Points = L->GetCount(TEXT("idocbag"));
	const int32 Bag = C->MedBagPoints(L);
	if (Points < Bag)
	{
		if (Points <= 0)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP revive refused: no med bag (the %s player tried)"), RuleWho(C->MyIndex()));
		}
		else
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP revive refused: %d med bag points, a bag's worth (%d) needed (the %s player tried)"), Points, Bag, RuleWho(C->MyIndex()));
		}
		L->Notify(TEXT("A medical bag would bring them back"), 3.f);
		return true;
	}
	const int32 Give = FMath::Min(Points, ReviveCap);
	L->TakeCount(TEXT("idocbag"), Give);
	C->ReviveSentAt = Now;
	++C->Revives;
	UOutlawsStats::NoteRevive(L);           // (the level's end panels)
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP revived the %s player: %d health given, med bag points left %d"), RuleWho(Target), Give, L->GetCount(TEXT("idocbag")));
	const AOutlawsLevelActor::FItemDef* D = L->ItemDefs.Find(TEXT("idocbag"));
	L->PlayWav(D && !D->UseOk.IsEmpty() ? D->UseOk : FString(TEXT("cool")), Feet);
	L->Notify(TEXT("You brought them back"), 2.5f);
	if (C->IsHost())
	{
		if (AOutlawsCoopLink* CL = C->GetLink(Target)) CL->ClientRevive(C->LevelEpoch(), 0, Give);
	}
	else if (AOutlawsCoopLink* Mine = C->GetLink(C->MyIndex()))
	{
		Mine->ServerRevive(C->LevelEpoch(), Target, Give);         // (N1: another client's revive passes through the host)
	}
	return true;
}

int32 UOutlawsCoop::MedBagPoints(AOutlawsLevelActor* L)
{
	// a bag on the ground gives its COUNT of points (gdocbag.itm: 3); the inventory item's own COUNT says the same
	if (BagPointsCache > 0) return BagPointsCache;
	int32 N = 0;
	FString Text;
	const FString Ground = GroundItemFor(L, TEXT("idocbag"));
	if (L && L->AssetsRef && !Ground.IsEmpty() && L->AssetsRef->GetText(Ground + TEXT(".itm"), Text))
	{
		FString Func;
		TMap<FString, FString> Data;
		OutlawsFormats::ParseItm(Text, Func, Data);
		N = FCString::Atoi(*Data.FindRef(TEXT("COUNT")));
	}
	BagPointsCache = N > 0 ? N : 3;
	return BagPointsCache;
}

void UOutlawsCoop::OnRevive(int32 Epoch, int32 By, int32 Given)
{
	RunFor(Epoch, [this, By, Given]()
	{
		AOutlawsLevelActor* L = FindLevel();
		AOutlawsPlayer* P = LocalPlayer();
		if (!L || !P) return;
		if (!L->bDead)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP a revive by the %s player came, but this player is not dead"), RuleWho(By));
			return;
		}
		// on its feet where it fell, with the health the reviver's bag points gave (at most half), hands empty, keys and guns
		// still on the ground round it (walked over at once: it stands among them)
		L->bDead = false;
		L->DeathTimer = 0.f;
		L->Health = float(FMath::Clamp(Given, 1, ReviveCap));
		P->ResetForLevel();
		bLocalDead = false;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP revived here by the %s player: health %.1f at %s"), RuleWho(By), L->Health, *P->GetActorLocation().ToCompactString());
		L->Notify(TEXT("Your partner brought you back"), 3.f);
	});
}

void UOutlawsCoop::RespawnAtStart(AOutlawsPlayer* P)
{
	UOutlawsCoop* C = Get(P);
	AOutlawsLevelActor* L = C ? C->FindLevel() : nullptr;
	if (!L || !L->bDead) return;
	// the start kit (the brief's default 2: single player's start loadout; the guns found since go), full health, no
	// plate; the gold kept; the wrist's list and the INF's items cleared (the keys were dropped where the player fell)
	const int32 Gold = L->Gold;
	L->ApplyStartKit();
	L->Gold = Gold;
	L->ArmourMax = 0;
	L->Inventory.Reset();
	L->InventoryLabels.Reset();
	if (L->Inf) L->Inf->Items.Reset();
	P->ResetForLevel();
	if (!L->WornCharacter.IsEmpty()) P->EquipStartWeapon();      // (BUG-CHARLOADOUT: a joiner's character draws its first weapon)
	const FRotator Face(0.0, L->GetPlayerStartYaw(), 0.0);
	P->TeleportTo(L->GetPlayerStartLocation(), Face, false, true);
	if (AController* Ctl = P->GetController()) Ctl->SetControlRotation(Face);
	P->GetCharacterMovement()->StopMovementImmediately();
	C->bLocalDead = false;
	++C->Respawns;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP respawned at the level start: %s (%s)"), RuleWho(C->MyIndex()), *Fingerprint(L));
	L->Notify(TEXT("Back at the start, with the start kit"), 3.f);
}

FString UOutlawsCoop::DeathPrompt(bool bVR)
{
	return bVR ? TEXT("You are dead\nYour partner can bring you back here with a medical bag\nA / X: back to the start with the start kit; your guns stay here")
		: TEXT("You are dead\nYour partner can bring you back here with a medical bag\nSpace: back to the start with the start kit; your guns stay here");
}

// ---- the movement speed settings (user 2026-10-02) ----------------------------------------------------------------------

void UOutlawsCoop::TickPace()
{
	// each player's pace (the speed setting, or the teleport pace) told through its link when it changes; both machines
	// then go by the same two numbers: the bad guys at the average (the floor and the co-op x1.5 as before), the timed doors
	// and lifts at the slower (each player's own body keeps its own)
	AOutlawsLevelActor* L = FindLevel();
	if (!L || !L->HasActorBegunPlay()) return;           // (its settings, the speed among them, are read at its BeginPlay)
	const float Mine = L->Pace();
	if (AOutlawsCoopLink* MyLink = GetLink(MyIndex()); MyLink && !FMath::IsNearlyEqual(Mine, SentPace, 1e-4f))
	{
		SentPace = Mine;
		MyLink->ServerPace(Mine);            // (the host's own: set here at once)
	}
	// (N1: every player's pace heard so far, this machine's own as it is; the average and the slowest of them all)
	TArray<AOutlawsCoopLink*> Links;
	GetLinks(Links);
	TArray<float> Paces;
	FString Each;
	for (const AOutlawsCoopLink* K : Links)
	{
		const float Pc = K->PlayerIndex == MyIndex() ? Mine : K->Pace;
		if (Pc <= 0.f) continue;
		Paces.Add(Pc);
		Each += FString::Printf(TEXT("%s%s %.3f"), Each.IsEmpty() ? TEXT("") : TEXT(", "), RuleWho(K->PlayerIndex), Pc);
	}
	if (Paces.Num() < 2)
	{
		L->CoopEnemyPaceBase = -1.f;         // (the others' not heard yet: this machine's own, as single player)
		L->CoopWaitPace = -1.f;
		return;
	}
	float Sum = 0.f, Slowest = Paces[0];
	for (const float Pc : Paces)
	{
		Sum += Pc;
		Slowest = FMath::Min(Slowest, Pc);
	}
	L->CoopEnemyPaceBase = Sum / float(Paces.Num());
	L->CoopWaitPace = Slowest;
	const FString Line = FString::Printf(TEXT("%s, enemies %.3f, door waits x%.2f"), *Each, L->EnemyPace(), 1.0 / FMath::Max(0.1f, L->CoopWaitPace));
	if (Line != LoggedPace)
	{
		LoggedPace = Line;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pace: %s (%s)"), *Line, IsHost() ? TEXT("host") : TEXT("client"));
	}
}

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TestWalkOpen(AOutlawsPlayer* P, float Seconds)
{
	// (a test aid) turned to the most open of eight ways round at the waist, then walking that way as the stick would
	if (!P || !P->GetWorld()) return;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsCoopTestWalk), false, P);
	const FVector From = P->GetActorLocation();
	float BestYaw = float(P->GetActorRotation().Yaw), BestOpen = -1.f;
	for (int32 k = 0; k < 8; ++k)
	{
		const float Yaw = 45.f * float(k);
		FHitResult Hit;
		const FVector To = From + FRotator(0.f, Yaw, 0.f).Vector() * 600.0;
		const float Open = P->GetWorld()->LineTraceSingleByChannel(Hit, From, To, ECC_Visibility, Q) ? float(Hit.Distance) : 600.f;
		if (Open > BestOpen) { BestOpen = Open; BestYaw = Yaw; }
	}
	if (APlayerController* PC = Cast<APlayerController>(P->GetController())) PC->SetControlRotation(FRotator(0.f, BestYaw, 0.f));
	P->HoldMoveStick(FVector2D(0.0, 1.0), Seconds);
	TestWalkFrom = From;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the %s walks on for %.1f s, yaw %.0f (open %.0f cm), from %s"), RuleWho(MyIndex()), Seconds, BestYaw, BestOpen, *From.ToCompactString());
}
#endif // WITH_OUTLAWS_TESTS

FString UOutlawsCoop::RulesSummary() const
{
	return FString::Printf(TEXT("keys dropped %d, guns dropped %d, revives %d, respawns %d, menus opened %d (the game went on), hurts dropped in the menu %d"), KeysDropped, GunsDropped, Revives,
		Respawns, MenusOpened, HurtsDropped);
}

// ---- tests (-OLCoopAuto after milestone 5's, or -OLCoopTest=rules alone) --------------------------------------------------

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TickHostRulesTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	auto Die = [](AOutlawsLevelActor* Lv)
	{
		// (a test's death: past the test's own invincibility, as a shot to the head is)
		const bool bWas = Lv->bInvincible;
		Lv->bInvincible = false;
		Lv->DamagePlayer(1e6f, 1.f, true);
		Lv->bInvincible = bWas;
	};
	auto Both = [this]()
	{
		const AOutlawsCoopLink* CL = GetLink(1);
		return bLevelBuilt && StartedSerial > TestArg && CL && CL->BuiltSerial == StartedSerial;
	};
	auto NextToBody = [this, P](double Dist = 140.0)
	{
		// this player set down beside the other's body (1.4 m off, a free spot, its hand reaching down to the feet), facing it;
		// (2026-10-02: far enough that its own walk-over leaves the client's dropped guns for the client to take back)
		const AOutlawsRemotePlayer* R = PartnerPuppet();
		if (!R || !P) return FVector::ZeroVector;
		const FVector Feet = R->ShownBody() - FVector(0.0, 0.0, R->ShownHalf());
		const float Half = P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), Rad = P->GetCapsuleComponent()->GetScaledCapsuleRadius();
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsCoopTestBody), false, P);
		for (int32 k = 0; k < 8; ++k)
		{
			const FVector At = Feet + FRotator(0.0, 45.0 * k, 0.0).Vector() * Dist + FVector(0.0, 0.0, Half + 3.0);
			if (P->GetWorld()->OverlapBlockingTestByChannel(At, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(Rad, Half), Q)) continue;
			const FRotator Face(0.0, (Feet - At).Rotation().Yaw, 0.0);
			P->TeleportTo(At, Face, false, true);
			break;
		}
		return Feet + FVector(0.0, 0.0, 25.0);
	};
	auto LogBoth = [this, L, P](const TCHAR* Label)
	{
		// the bad guy and the door, and each player's place and menu, here and (asked) on the client, under one label
		TestLogEnemy(L, TestPauseEnemy, Label);
		if (TestPauseDoor >= 0) TestLogElevator(L, TestPauseDoor, Label);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host at %s, its menu %s [%s]"), *P->GetActorLocation().ToCompactString(), P->IsMenuOpen() ? TEXT("open") : TEXT("closed"), Label);
		TestSay(FString(TEXT("rlog:")) + Label, TestPauseEnemy);
	};
	auto PushSlowDoor = [this, L]()
	{
		// the slowest idle door with no lock, pushed now: still on its way while a menu is open
		TestPauseDoor = INDEX_NONE;
		double Longest = 0.0;
		for (int32 i = 0; L->Inf && i < L->Inf->Elevators.Num(); ++i)
		{
			const FOutlawsElevator& E = L->Inf->Elevators[i];
			if (E.Target >= 0 || E.Stop != 0 || E.bTerminated || !E.bMasterOn || E.Speed <= 0.0 || E.IsScroll() || E.Sectors.Num() == 0) continue;
			const FOutlawsInfClass& Cl = L->Inf->Inf.Items[E.Item].Classes[E.ClassIndex];
			if (Cl.KeyObject >= 0 || !(Cl.EventMask & (OutlawsInf::EvNudgeFront | OutlawsInf::EvNudgeBack)) || Cl.Stops.Num() < 2) continue;
			const double Travel = FMath::Abs(L->Inf->StopValue(E, 1) - E.Current) / E.Speed;
			if (Travel > Longest) { Longest = Travel; TestPauseDoor = i; }
		}
		if (TestPauseDoor < 0) return;
		TestSay(TEXT("rdoor"), TestPauseDoor);
		const int32 Sector = L->Inf->Elevators[TestPauseDoor].Sectors[0];
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host pushes elevator %d (%s, %.1f s to open): %s"), TestPauseDoor, *L->Inf->Elevators[TestPauseDoor].Name, Longest,
			L->PushDoorSector(Sector) ? TEXT("taken") : TEXT("not taken"));
	};
	switch (TestStep)
	{
	case 60:
		// both out of the bad guys' reach (the test's deaths are its own); the client holds the brass key to drop
		L->bInvincible = true;
		TestSay(TEXT("godmode"), 1);
		TestSay(TEXT("rkey"), 0);
		TestSay(TEXT("rguns"), 0);          // (2026-10-02: guns found beyond the start kit, to be dropped at the death)
		TestStep = 61;
		TestAt = Now + 1.0;
		return;
	case 61:
		if (Now < TestAt) return;
		// 1: the client dies: the host sees it dead and its key (and its guns) as pickups on the ground.  (2026-10-02: the host
		// steps 2.5 m off first: its own walk-over would take what lands within its reach, which the rule allows, and this test
		// wants the client to take its guns back itself)
		NextToBody(250.0);
		TestSay(TEXT("die"), 0);
		TestStep = 62;
		TestAt = Now + 3.0;
		return;
	case 62:
	{
		if (Now < TestAt) return;
		const AOutlawsRemotePlayer* R = PartnerPuppet();
		const FVector Feet = R ? R->ShownBody() - FVector(0.0, 0.0, R->ShownHalf()) : FVector::ZeroVector;
		int32 Near = 0, NearDrawn = 0;
		TArray<FString> What;
		for (const AOutlawsLevelActor::FPickup& Pk : L->Pickups)
		{
			const AActor* A = Pk.Actor.Get();
			if (!A || Pk.CoopId < 100000 || FVector::Dist2D(A->GetActorLocation(), Feet) > 200.0) continue;
			++Near;
			if (DrawnHere(A)) ++NearDrawn;
			TArray<FString> Gives;
			for (const AOutlawsLevelActor::FPickup::FEntry& E : Pk.Entries) Gives.Add(FString::Printf(TEXT("%s %d"), *E.Item, E.Count));
			What.Add(FString::Printf(TEXT("%s %d [%s]"), *Pk.Ground, Pk.CoopId, *FString::Join(Gives, TEXT(", "))));
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client is %s here; dropped things by its body here: %d (%s)"), bOtherDead ? TEXT("dead") : TEXT("ALIVE"), Near, *FString::Join(What, TEXT(", ")));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client's death drops drawn here (the host): %d of %d"), NearDrawn, Near);      // (REQ-DEATHDROPHIDE)
		// 2: the host, without a med bag, grips the body: refused (the body photographed first, lying where it fell)
		L->Counts.Remove(TEXT("idocbag"));
		const FVector Hand = NextToBody();
		if (APlayerController* PC = Cast<APlayerController>(P->GetController()))
		{
			const FVector Eye = P->GetActorLocation() + FVector(0.0, 0.0, 60.0);
			PC->SetControlRotation(((R ? R->ShownBody() - FVector(0.0, 0.0, R->ShownHalf() - 20.0) : Hand) - Eye).Rotation());
			Shot(TEXT("ol_coop_dead_client"));
		}
		const int32 Bags = L->GetCount(TEXT("idocbag"));
		const bool bTook = TryRevive(P, Hand);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host (med bag points %d) grips the client's body: %s"), Bags, bTook ? TEXT("the grip took it") : TEXT("NOT TAKEN"));
		TestStep = 63;
		TestAt = Now + 2.5;
		return;
	}
	case 63:
	{
		if (Now < TestAt) return;
		// 3: two med bag points, less than a bag's worth: refused; with six (two bags): the client is back where it fell with
		// five (half health: no more), one point left with the host
		L->AddCount(TEXT("idocbag"), 2);
		const FVector Hand = NextToBody();
		const int32 Few = L->GetCount(TEXT("idocbag"));
		const bool bFew = TryRevive(P, Hand);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host (med bag points %d) grips the client's body: %s"), Few, bFew ? TEXT("the grip took it") : TEXT("NOT TAKEN"));
		L->AddCount(TEXT("idocbag"), 4);
		const int32 Bags = L->GetCount(TEXT("idocbag"));
		const bool bTook = TryRevive(P, Hand);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host (med bag points %d) grips the client's body: %s"), Bags, bTook ? TEXT("the grip took it") : TEXT("NOT TAKEN"));
		TestStep = 64;
		TestAt = Now + 2.5;
		return;
	}
	case 64:
		if (Now < TestAt) return;
		TestSay(TEXT("rstate"), 0);
		// 4: the other way round: the host dies, the client revives it
		Die(L);
		TestStep = 65;
		TestAt = Now + 3.0;
		return;
	case 65:
		if (Now < TestAt) return;
		TestSay(TEXT("rrevive"), 0);
		TestStep = 66;
		TestAt = Now + 3.0;
		return;
	case 66:
		if (Now < TestAt) return;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host after the client's grip: %s, health %.1f at %s"), L->bDead ? TEXT("STILL DEAD") : TEXT("alive"), L->Health, *P->GetActorLocation().ToCompactString());
		// 5: the client dies and goes back to the level's start (its own choice on the death screen)
		TestSay(TEXT("die"), 0);
		TestStep = 67;
		TestAt = Now + 2.5;
		return;
	case 67:
		if (Now < TestAt) return;
		TestSay(TEXT("respawn"), 0);
		TestStep = 85;
		TestAt = Now + 2.5;
		return;
	case 85:
		// 5b (2026-10-02): the client goes back to where it fell: its guns lie there, and it takes them up again
		if (Now < TestAt) return;
		TestSay(TEXT("rback"), 0);
		TestStep = 86;
		TestAt = Now + 2.5;
		return;
	case 86:
		if (Now < TestAt) return;
		TestSay(TEXT("rbacklog"), 0);
		TestStep = 68;
		TestAt = Now + 1.0;
		return;
	case 68:
		if (Now < TestAt) return;
		// 6: both dead: the level again for both
		TestArg = StartedSerial;
		TestSay(TEXT("die"), 0);
		Die(L);
		TestStep = 69;
		TestAt = Now + 40.0;
		return;
	case 69:
	{
		const bool bBoth = Both();
		if (!bBoth && Now < TestAt) return;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: after both died both machines built %s again: %s (serial %d), host %s"), *BuiltLevel, bBoth ? TEXT("yes") : TEXT("NO"), StartedSerial,
			L->bDead ? TEXT("STILL DEAD") : TEXT("alive"));
		L->bInvincible = true;
		TestSay(TEXT("godmode"), 1);
		TestStep = 70;
		TestAt = Now + 2.0;
		return;
	}
	case 70:
	{
		if (Now < TestAt) return;
		// 7 (2026-10-02, the user: "one player should be able to continue if the other pauses"): a bad guy woken (it moves), a
		// door set going, then the host's menu: the game goes on for both (the bad guy and the door move on, on both machines),
		// the client's menu stays shut and it walks on; then the client's menu, and the host walks on.  A hurt to the player in
		// the menu is dropped (out of harm while it is open)
		TestPauseEnemy = TestPickEnemy(L, P->GetActorLocation(), INDEX_NONE);
		const TArray<TWeakObjectPtr<AOutlawsEnemy>>& List = L->GetEnemies();
		if (AOutlawsEnemy* E = List.IsValidIndex(TestPauseEnemy) ? List[TestPauseEnemy].Get() : nullptr) E->Wake(false);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: enemy %d woken for the menus"), TestPauseEnemy);
		TestStep = 71;
		TestAt = Now + 1.5;
		return;
	}
	case 71:
		if (Now < TestAt) return;
		PushSlowDoor();
		TestStep = 72;
		TestAt = Now + 0.4;
		return;
	case 72:
		if (Now < TestAt) return;
		LogBoth(TEXT("before the host's menu"));
		P->CoopOpenMenu();
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host opens its menu: %s"), P->IsMenuOpen() ? TEXT("open") : TEXT("NOT OPEN"));
		TestSay(TEXT("rwalk"), 2600);       // (the client walks on meanwhile)
		TestStep = 73;
		TestAt = Now + 0.6;
		return;
	case 73:
		if (Now < TestAt) return;
		LogBoth(TEXT("the host's menu open A"));
		TestStep = 74;
		TestAt = Now + 1.5;
		return;
	case 74:
	{
		if (Now < TestAt) return;
		LogBoth(TEXT("the host's menu open B"));
		// out of harm while the menu is open: a hurt to the host's own player (past the test's invincibility) is dropped
		const float Was = L->GetHealth();
		const bool bWasInvincible = L->bInvincible;
		L->bInvincible = false;
		L->DamagePlayer(1.f, 1.f, true);
		L->bInvincible = bWasInvincible;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: a hurt of 1.0 to the host in its menu: health %.1f -> %.1f"), Was, L->GetHealth());
		P->CoopCloseMenu();
		TestStep = 75;
		TestAt = Now + 1.0;
		return;
	}
	case 75:
		if (Now < TestAt) return;
		LogBoth(TEXT("after the host's menu"));
		PushSlowDoor();
		TestStep = 76;
		TestAt = Now + 0.4;
		return;
	case 76:
		if (Now < TestAt) return;
		LogBoth(TEXT("before the client's menu"));
		TestSay(TEXT("pause"), 1);
		TestWalkOpen(P, 2.6f);              // (the host walks on meanwhile)
		TestStep = 77;
		TestAt = Now + 0.8;
		return;
	case 77:
		if (Now < TestAt) return;
		LogBoth(TEXT("the client's menu open A"));
		TestStep = 78;
		TestAt = Now + 1.5;
		return;
	case 78:
		if (Now < TestAt) return;
		LogBoth(TEXT("the client's menu open B"));
		// out of harm while the menu is open: the host's hurt to the client is dropped there
		HurtOther(1.f, 1.f, true, nullptr);
		TestSay(TEXT("pause"), 0);
		TestStep = 79;
		TestAt = Now + 1.2;
		return;
	case 79:
		if (Now < TestAt) return;
		LogBoth(TEXT("after the client's menu"));
		TestStep = 99;
		TestAt = Now + 1.0;
		return;
	case 99:
		if (Now < TestAt) return;
		TestSay(TEXT("quit"), 0);
		TestStep = 100;
		TestAt = Now + 3.0;
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
