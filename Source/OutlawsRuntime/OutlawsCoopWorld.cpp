// Story co-op (tools/olue/COOP_DESIGN.md), milestone 3: the shared world's takeables.  Pickups have one number on both
// machines (the level's own order; the host numbers anything made in play); a take or a grab on one machine is reported,
// the host decides who had it first, and the other machine takes it away without giving anything.  The players' health,
// armour and death go to each other, and the host can hurt the client on the client's own machine.
//
// Each machine's level actor still holds only its own player's state, and none of this runs outside a session: the level
// actor calls in only when UOutlawsCoop::IsActive.
#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsEnemy.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsRemotePlayer.h"
#include "ProceduralMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "Camera/CameraComponent.h"
#include "Engine/World.h"

namespace
{
	const TCHAR* WhoName(int32 Who) { return UOutlawsCoop::WhoWord(Who); }
	constexpr int32 LevelLooseIdBase = 50000;       // the level's own pickups without a serial (its sticks of dynamite)
	constexpr int32 ClaimHold = 0, ClaimTake = 1;
	bool IsStickName(const FString& Name) { return Name.StartsWith(TEXT("gdynam"), ESearchCase::IgnoreCase); }
}

// ---- the link's milestone 3 messages ----------------------------------------------------------------------------------

void AOutlawsCoopLink::ServerStats_Implementation(const FOutlawsCoopStats& In)
{
	bool bKeysOk = In.Keys.Num() <= 64 && FMath::Abs(In.SafeFeet.X) < 1.0e7 && FMath::Abs(In.SafeFeet.Y) < 1.0e7 && FMath::Abs(In.SafeFeet.Z) < 1.0e7;
	for (const FString& K : In.Keys) bKeysOk = bKeysOk && K.Len() <= 32;
	bKeysOk = bKeysOk && In.DieSound.Len() <= 32;
	if (!Admit(EOLGate::Stats, Finite(In.Health, 1.0e4f) && Finite(In.MaxHealth, 1.0e4f) && In.Armour >= 0 && In.Armour < 100000 && bKeysOk)) return;
	Stats = In;
	Stats.DeathBy = FMath::Min<uint8>(In.DeathBy, uint8(EOutlawsHurtCause::Count) - 1);
}

void AOutlawsCoopLink::ServerLevelBuilt_Implementation(int32 Serial)
{
	if (!Admit(EOLGate::Built, Serial >= 0)) return;
	BuiltSerial = Serial;
}

void AOutlawsCoopLink::ServerPickupClaim_Implementation(int32 Id, int32 Epoch, uint8 Kind)
{
	if (!Admit(EOLGate::PickupClaim, Id >= 0)) return;
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnPickupClaim(PlayerIndex, Id, Epoch, Kind);
}

void AOutlawsCoopLink::ServerPickupDrop_Implementation(int32 Id, int32 Epoch, FVector_NetQuantize10 At)
{
	if (!Admit(EOLGate::PickupDrop, Id >= 0 && Finite(At))) return;
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnPickupDrop(PlayerIndex, Id, Epoch, At);
}

void AOutlawsCoopLink::ServerGroundItem_Implementation(const FString& Name, FVector_NetQuantize10 At, int32 Tag, int32 Epoch)
{
	if (!Admit(EOLGate::GroundItem, Name.Len() <= 32 && Finite(At))) return;
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnGroundItemRequest(PlayerIndex, Name, At, Tag, Epoch);
}

void AOutlawsCoopLink::ServerTestNote_Implementation(const FString& Note)
{
	if (!Admit(EOLGate::TestNote, Note.Len() <= 256)) return;
#if WITH_OUTLAWS_TESTS
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnTestNote(Note);
#endif
}

void AOutlawsCoopLink::ClientPickup_Implementation(int32 Id, int32 Epoch, uint8 Event, FVector_NetQuantize10 At)
{
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnPickupEvent(Id, Epoch, Event, At);
}

void AOutlawsCoopLink::ClientGroundItem_Implementation(int32 Id, const FString& Name, FVector_NetQuantize10 At, int32 Tag, int32 Epoch)
{
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnGroundItem(Id, Name, At, Tag, Epoch);
}

void AOutlawsCoopLink::ClientHurt_Implementation(float Damage, float LocationMult, bool bSkipArmour, bool bHasFrom, FVector_NetQuantize10 From, int32 SourceEnemy)
{
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnHurt(Damage, LocationMult, bSkipArmour, bHasFrom, From, SourceEnemy);
}

void AOutlawsCoopLink::ClientTest_Implementation(const FString& Step, int32 Arg)
{
#if WITH_OUTLAWS_TESTS
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnTestStep(Step, Arg);
#endif
}

// ---- bookkeeping ------------------------------------------------------------------------------------------------------

const FOutlawsCoopStats* UOutlawsCoop::OtherStats() const
{
	const AOutlawsCoopLink* OL = GetLink(PartnerIndex());
	return OL ? &OL->Stats : nullptr;
}

void UOutlawsCoop::ResetWorld()
{
	PickupHolder.Reset();
	PickupGone.Reset();
	NextGroundId = 100000;
	PendingTakes.Reset();
	PendingGround.Reset();
	PendingGroundTaken.Reset();
	ClientHolds.Reset();                         // (milestone 6 item 4: the bad guys held for the client's hand, by index)
	ClientThrown.Reset();
	ClientThrownBy.Reset();
	const int32 Keep = StartedSerial;
	Deferred.RemoveAll([Keep](const FDeferred& D) { return D.Epoch < Keep; });
}

void UOutlawsCoop::AssignPickupIds(AOutlawsLevelActor* L)
{
	// the level's pickups in the order the build made them, the same on both machines: a placed one by its serial (as a
	// save names it), the level's sticks of dynamite (no serial) after LevelLooseIdBase
	int32 Loose = 0;
	for (AOutlawsLevelActor::FPickup& P : L->Pickups)
	{
		P.CoopId = P.Serial >= 0 ? P.Serial : LevelLooseIdBase + Loose++;
	}
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pickups numbered: %d (%d of them without a serial, from %d)"), L->Pickups.Num(), Loose, LevelLooseIdBase);
}

void UOutlawsCoop::RunFor(int32 Epoch, TFunction<void()> Do)
{
	const int32 Here = LevelEpoch();
	if (Epoch == Here)
	{
		Do();
	}
	else if (Epoch >= StartedSerial && Deferred.Num() < 1024)
	{
		Deferred.Add(FDeferred{ Epoch, MoveTemp(Do) });           // (a level this machine is still building, or about to)
	}
}

void UOutlawsCoop::RunDeferred()
{
	const int32 Here = LevelEpoch();
	TArray<FDeferred> Ready;
	for (int32 i = 0; i < Deferred.Num();)
	{
		if (Deferred[i].Epoch == Here) { Ready.Add(MoveTemp(Deferred[i])); Deferred.RemoveAt(i); }
		else if (Deferred[i].Epoch < Here) Deferred.RemoveAt(i);
		else ++i;
	}
	if (Ready.Num() > 0) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP %d pieces of the other player's news held for this level, applied now"), Ready.Num());
	for (FDeferred& D : Ready) D.Do();
}

int32 UOutlawsCoop::PickupIndex(const AOutlawsLevelActor* L, int32 Id)
{
	if (!L || Id < 0) return INDEX_NONE;
	return L->Pickups.IndexOfByPredicate([Id](const AOutlawsLevelActor::FPickup& P) { return P.CoopId == Id; });
}

int32 UOutlawsCoop::PickupIndexOf(const AOutlawsLevelActor* L, const AActor* A)
{
	if (!L || !A) return INDEX_NONE;
	for (int32 i = L->Pickups.Num() - 1; i >= 0; --i)
	{
		if (L->Pickups[i].Actor.Get() == A) return i;
	}
	return INDEX_NONE;
}

void UOutlawsCoop::RemovePickupHere(AOutlawsLevelActor* L, int32 Index)
{
	// as CollectPickup's end, nothing given: the actor goes, a stick taken sets nothing off any more
	AOutlawsLevelActor::FPickup& P = L->Pickups[Index];
	AActor* A = P.Actor.Get();
	if (A)
	{
		for (AOutlawsLevelActor::FDynamite& D : L->Dynamite)
		{
			if (D.Mesh.IsValid() && D.Mesh->GetOwner() == A) { D.bGone = true; D.Mesh = nullptr; }
		}
		if (AOutlawsPlayer* Me = LocalPlayer()) Me->CoopLetGoOf(A);
	}
	L->Pickups.RemoveAt(Index);
	if (A) A->Destroy();
}

void UOutlawsCoop::HidePickupHere(AOutlawsLevelActor* L, int32 Index)
{
	// in the other player's hand: nobody here walks over it or grabs it (bHeld), and it is not drawn
	AOutlawsLevelActor::FPickup& P = L->Pickups[Index];
	P.bHeld = true;
	P.bWaitClear = false;
	if (AActor* A = P.Actor.Get())
	{
		if (AOutlawsPlayer* Me = LocalPlayer()) Me->CoopLetGoOf(A);
		A->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		if (L->IsDynamitePickup(A)) A->SetActorScale3D(FVector::OneVector);      // (a stick in this machine's fist was made hand size)
		A->SetActorHiddenInGame(true);
	}
}

void UOutlawsCoop::ShowPickupHere(AOutlawsLevelActor* L, int32 Index, const FVector& At)
{
	AOutlawsLevelActor::FPickup& P = L->Pickups[Index];
	P.bHeld = false;
	P.bWaitClear = false;
	if (AActor* A = P.Actor.Get())
	{
		A->SetActorLocation(At);
		A->SetActorHiddenInGame(false);
	}
}

bool UOutlawsCoop::Arbitrate(int32 Who, int32 Id, uint8 Kind, FString& OutWhy)
{
	// the host's word: first claim wins.  A pickup taken for good stays taken; one in a hand is that hand's until it is put
	// down or taken
	if (const int32* By = PickupGone.Find(Id))
	{
		OutWhy = FString::Printf(TEXT("already taken by the %s"), WhoName(*By));
		return false;
	}
	if (const int32* H = PickupHolder.Find(Id); H && *H != Who)
	{
		OutWhy = FString::Printf(TEXT("in the %s's hand"), WhoName(*H));
		return false;
	}
	if (Kind == ClaimHold)
	{
		PickupHolder.Add(Id, Who);
	}
	else
	{
		PickupHolder.Remove(Id);
		PickupGone.Add(Id, Who);
	}
	return true;
}

void UOutlawsCoop::SendPickupEvent(int32 Id, EOutlawsCoopPickupEvent Event, const FVector& At, int32 Who)
{
	// (N1) the host's answer to a claim goes to the claimant, Who; news of a take, a hold or a put-down goes to every client
	// but Who (the host's own: to all of them)
	if (!IsHost()) return;
	const int32 Epoch = LevelEpoch();
	const bool bAnswer = Event == EOutlawsCoopPickupEvent::ConfirmHold || Event == EOutlawsCoopPickupEvent::ConfirmTake
		|| Event == EOutlawsCoopPickupEvent::DenyHold || Event == EOutlawsCoopPickupEvent::DenyTake;
	if (bAnswer)
	{
		if (AOutlawsCoopLink* CL = GetLink(Who > 0 ? Who : 1)) CL->ClientPickup(Id, Epoch, uint8(Event), At);
		return;
	}
	ForClients([&](AOutlawsCoopLink& CL) { CL.ClientPickup(Id, Epoch, uint8(Event), At); }, Who);
}

// ---- this machine's takes, grabs, drops and new ground items (the level actor's hooks) ----------------------------------

void UOutlawsCoop::BeforeTake(AOutlawsLevelActor* L)
{
	UOutlawsCoop* C = Get(L);
	if (!C || !C->IsClient()) return;           // (the host's own takes are never refused)
	FTakeBefore& B = C->TakeBefore;
	B.Health = L->Health;
	B.Armour = L->Armour;
	B.ArmourMax = L->ArmourMax;
	B.Gold = L->Gold;
	B.Listed = L->Inventory.Num();
	B.Badge = L->BadgeLeft;
	B.Counts = L->Counts;
	B.UseIds = L->Inf ? L->Inf->Items : TSet<int32>();
}

void UOutlawsCoop::NotePickupTaken(AOutlawsLevelActor* L, int32 Index)
{
	UOutlawsCoop* C = Get(L);
	if (!C || !L->Pickups.IsValidIndex(Index)) return;
	const AOutlawsLevelActor::FPickup& P = L->Pickups[Index];
	const int32 Id = P.CoopId;
	if (C->IsHost())
	{
		if (Id < 0) return;
		FString Why;
		if (!C->Arbitrate(0, Id, ClaimTake, Why))
		{
			// (cannot happen: a pickup the client has is hidden and held here)
			UE_LOG(LogOutlawsCoop, Warning, TEXT("OLCOOP pickup %d taken by host although %s"), Id, *Why);
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pickup %d taken by host (%s)"), Id, *P.Ground);
		C->SendPickupEvent(Id, EOutlawsCoopPickupEvent::TakenByOther);
		return;
	}
	if (!C->IsClient()) return;
	if (Id <= -2)
	{
		// one of this machine's own new ground items, its number not back yet: the take is told once it is
		C->PendingGroundTaken.Add(-2 - Id);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP our new ground item %s taken before the host numbered it"), *P.Ground);
		return;
	}
	if (Id < 0) return;
	// what the take gave, kept until the host answers (a refusal gives it all back)
	const FTakeBefore& B = C->TakeBefore;
	FTakeUndo U;
	U.Ground = P.Ground;
	U.Health = FMath::Max(0.f, L->Health - B.Health);
	U.bArmour = L->Armour != B.Armour || L->ArmourMax != B.ArmourMax;
	U.Armour = B.Armour;
	U.ArmourMax = B.ArmourMax;
	U.Gold = FMath::Max(0, L->Gold - B.Gold);
	for (const TPair<FString, int32>& Kv : L->Counts)
	{
		const int32 D = Kv.Value - B.Counts.FindRef(Kv.Key);
		if (D > 0) U.Counts.Add(Kv.Key, D);
	}
	for (int32 k = B.Listed; k < L->Inventory.Num(); ++k) U.Listed.Add(L->Inventory[k]);
	if (L->Inf)
	{
		for (const int32 Use : L->Inf->Items) if (!B.UseIds.Contains(Use)) U.UseIds.Add(Use);
	}
	U.bBadge = L->BadgeLeft > B.Badge + 0.01f;
	U.Badge = B.Badge;
	C->PendingTakes.Add(Id, MoveTemp(U));
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pickup %d taken by client (%s), the host asked"), Id, *P.Ground);
	if (AOutlawsCoopLink* Mine = C->GetLink(C->MyIndex())) Mine->ServerPickupClaim(Id, C->LevelEpoch(), ClaimTake);
}

void UOutlawsCoop::NotePickupHeld(AOutlawsLevelActor* L, int32 Index)
{
	UOutlawsCoop* C = Get(L);
	if (!C || !L->Pickups.IsValidIndex(Index)) return;
	const AOutlawsLevelActor::FPickup& P = L->Pickups[Index];
	const int32 Id = P.CoopId;
	if (Id < 0) return;                         // (a new one not numbered yet: its take is told later)
	if (C->IsHost())
	{
		FString Why;
		if (!C->Arbitrate(0, Id, ClaimHold, Why)) UE_LOG(LogOutlawsCoop, Warning, TEXT("OLCOOP pickup %d held by host although %s"), Id, *Why);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pickup %d held by host (%s)"), Id, *P.Ground);
		C->SendPickupEvent(Id, EOutlawsCoopPickupEvent::HeldByOther);
		return;
	}
	if (!C->IsClient()) return;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pickup %d held by client (%s), the host asked"), Id, *P.Ground);
	if (AOutlawsCoopLink* Mine = C->GetLink(C->MyIndex())) Mine->ServerPickupClaim(Id, C->LevelEpoch(), ClaimHold);
}

void UOutlawsCoop::NotePickupDropped(AOutlawsLevelActor* L, AActor* Item)
{
	UOutlawsCoop* C = Get(L);
	const int32 i = PickupIndexOf(L, Item);
	if (!C || i < 0) return;
	const int32 Id = L->Pickups[i].CoopId;
	if (Id < 0) return;
	const FVector At = Item->GetActorLocation();
	if (C->IsHost())
	{
		if (C->PickupHolder.FindRef(Id) == 0) C->PickupHolder.Remove(Id);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pickup %d put down by the host at %s"), Id, *At.ToString());
		C->SendPickupEvent(Id, EOutlawsCoopPickupEvent::DroppedByOther, At);
		return;
	}
	if (!C->IsClient()) return;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pickup %d put down by the client at %s, the host told"), Id, *At.ToString());
	if (AOutlawsCoopLink* Mine = C->GetLink(C->MyIndex())) Mine->ServerPickupDrop(Id, C->LevelEpoch(), At);
}

void UOutlawsCoop::NoteGroundItem(AOutlawsLevelActor* L, AActor* A, const FString& Name, const FVector& FeetPos)
{
	UOutlawsCoop* C = Get(L);
	if (!C || !A || C->bApplyingRemote || !C->bLevelBuilt || L->bRestoring) return;     // (a save being put back: not shared yet, M8)
	const int32 i = PickupIndexOf(L, A);
	if (i < 0) return;                          // (not a pickup: nothing to share)
	// (2026-10-02: a dead player's gun carries what it gives, "gsgun#isgun=1,ishells=17": the other machine's copy gives the same)
	const FString Told = Name + C->GroundNameSuffix;
	if (C->IsHost())
	{
		const int32 Id = C->NextGroundId++;
		L->Pickups[i].CoopId = Id;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP ground item %d %s made by the host at %s"), Id, *Told, *FeetPos.ToString());
		const int32 Epoch = C->LevelEpoch();
		C->ForClients([&](AOutlawsCoopLink& CL) { CL.ClientGroundItem(Id, Told, FeetPos, 0, Epoch); });
		return;
	}
	if (!C->IsClient()) return;
	const int32 Tag = ++C->NextGroundTag;
	L->Pickups[i].CoopId = -2 - Tag;            // (waiting for the host's number)
	C->PendingGround.Add(Tag, A);
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP ground item %s made by the client at %s, the host asked for its number (tag %d)"), *Told, *FeetPos.ToString(), Tag);
	if (AOutlawsCoopLink* Mine = C->GetLink(C->MyIndex())) Mine->ServerGroundItem(Told, FeetPos, Tag, C->LevelEpoch());
}

// ---- the host's side of the client's claims ----------------------------------------------------------------------------

void UOutlawsCoop::OnPickupClaim(int32 Who, int32 Id, int32 Epoch, uint8 Kind)
{
	if (!IsHost()) return;
	RunFor(Epoch, [this, Who, Id, Kind]()
	{
		AOutlawsLevelActor* L = FindLevel();
		const int32 i = PickupIndex(L, Id);
		const FString Ground = i >= 0 ? L->Pickups[i].Ground : FString(TEXT("not here"));
		FString Why;
		if (!Arbitrate(Who, Id, Kind, Why))
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pickup %d claim by the %s denied (%s)"), Id, WhoName(Who), *Why);
			SendPickupEvent(Id, Kind == ClaimHold ? EOutlawsCoopPickupEvent::DenyHold : EOutlawsCoopPickupEvent::DenyTake, FVector::ZeroVector, Who);
			return;
		}
		if (Kind == ClaimHold)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pickup %d held by %s (%s): hidden here"), Id, WhoName(Who), *Ground);
			if (i >= 0) HidePickupHere(L, i);
			SendPickupEvent(Id, EOutlawsCoopPickupEvent::ConfirmHold, FVector::ZeroVector, Who);
			SendPickupEvent(Id, EOutlawsCoopPickupEvent::HeldByOther, FVector::ZeroVector, Who);     // (N1: the other clients)
			return;
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pickup %d taken by %s (%s)"), Id, WhoName(Who), *Ground);
		if (i >= 0)
		{
			RemovePickupHere(L, i);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pickup %d removed here (taken by the other player)"), Id);
		}
		SendPickupEvent(Id, EOutlawsCoopPickupEvent::ConfirmTake, FVector::ZeroVector, Who);
		SendPickupEvent(Id, EOutlawsCoopPickupEvent::TakenByOther, FVector::ZeroVector, Who);     // (N1: the other clients)
	});
}

void UOutlawsCoop::OnPickupDrop(int32 Who, int32 Id, int32 Epoch, const FVector& At)
{
	if (!IsHost()) return;
	RunFor(Epoch, [this, Who, Id, At]()
	{
		const int32* H = PickupHolder.Find(Id);
		if (!H || *H != Who)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pickup %d put down by the %s: not in their hand here, left as it is"), Id, WhoName(Who));
			return;
		}
		PickupHolder.Remove(Id);
		AOutlawsLevelActor* L = FindLevel();
		const int32 i = PickupIndex(L, Id);
		if (i >= 0) ShowPickupHere(L, i, At);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pickup %d put down by the %s at %s: shown here again"), Id, WhoName(Who), *At.ToString());
		SendPickupEvent(Id, EOutlawsCoopPickupEvent::DroppedByOther, At, Who);       // (N1: the other clients)
	});
}

void UOutlawsCoop::OnGroundItemRequest(int32 Who, const FString& Name, const FVector& At, int32 Tag, int32 Epoch)
{
	if (!IsHost()) return;
	RunFor(Epoch, [this, Who, Name, At, Tag]()
	{
		AOutlawsLevelActor* L = FindLevel();
		if (!L) return;
		FString Contents;
		const FString Base = SplitGroundName(Name, Contents);
		bApplyingRemote = true;
		AActor* A = L->SpawnGroundItem(Base, At);
		bApplyingRemote = false;
		const int32 Id = NextGroundId++;
		const int32 i = PickupIndexOf(L, A);
		if (i >= 0)
		{
			L->Pickups[i].CoopId = Id;
			if (IsStickName(Base)) L->AddLooseDynamite(A);        // (as the dropper's own: a blast or a shot sets it off)
			ApplyGroundContents(L, A, Contents);                  // (a dropped gun: what the dead player carried)
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP ground item %d %s made by the %s, made here too at %s%s"), Id, *Name, WhoName(Who), *At.ToString(),
			i < 0 ? TEXT(" (no pickup of that name here)") : TEXT(""));
		if (bAuto) TestClientDropId = Id;
		const int32 Epoch = LevelEpoch();
		if (AOutlawsCoopLink* CL = GetLink(Who)) CL->ClientGroundItem(Id, Name, At, Tag, Epoch);
		ForClients([&](AOutlawsCoopLink& K) { K.ClientGroundItem(Id, Name, At, 0, Epoch); }, Who);     // (N1: made on the other clients too)
	});
}

// ---- the client's side of the host's news ------------------------------------------------------------------------------

void UOutlawsCoop::OnPickupEvent(int32 Id, int32 Epoch, uint8 Event, const FVector& At)
{
	if (!IsClient()) return;
	RunFor(Epoch, [this, Id, Event, At]()
	{
		AOutlawsLevelActor* L = FindLevel();
		if (!L) return;
		const int32 i = PickupIndex(L, Id);
		switch (static_cast<EOutlawsCoopPickupEvent>(Event))
		{
		case EOutlawsCoopPickupEvent::HeldByOther:
			if (i >= 0) HidePickupHere(L, i);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pickup %d held by the other player: hidden here"), Id);
			break;
		case EOutlawsCoopPickupEvent::TakenByOther:
			if (PendingTakes.Contains(Id))
			{
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pickup %d: the other player took it too; the host's word on ours is coming"), Id);
				break;
			}
			if (i >= 0)
			{
				RemovePickupHere(L, i);
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pickup %d removed here (taken by the other player)"), Id);
			}
			break;
		case EOutlawsCoopPickupEvent::DroppedByOther:
			if (i >= 0) ShowPickupHere(L, i, At);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pickup %d put down by the other player at %s: shown here again"), Id, *At.ToString());
			break;
		case EOutlawsCoopPickupEvent::ConfirmHold:
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pickup %d: the host says it is in our hand"), Id);
			if (i >= 0 && !L->Pickups[i].bHeld && !PendingTakes.Contains(Id))
			{
				// (it left our hand meanwhile, the other player's news first: put down where it lies, so the host shows it)
				if (AActor* A = L->Pickups[i].Actor.Get())
				{
					if (AOutlawsCoopLink* Mine = GetLink(MyIndex())) Mine->ServerPickupDrop(Id, LevelEpoch(), A->GetActorLocation());
				}
			}
			break;
		case EOutlawsCoopPickupEvent::ConfirmTake:
			PendingTakes.Remove(Id);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pickup %d: the host confirms our take"), Id);
			break;
		case EOutlawsCoopPickupEvent::DenyHold:
			if (PendingTakes.Contains(Id)) break;             // (taken meanwhile: the take's own answer settles it)
			if (i >= 0) HidePickupHere(L, i);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pickup %d claim denied (the other player had it first): out of our hand"), Id);
			break;
		case EOutlawsCoopPickupEvent::DenyTake:
		{
			FTakeUndo U;
			if (PendingTakes.RemoveAndCopyValue(Id, U))
			{
				UndoTake(L, U);
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pickup %d claim denied (the other player took it first): what %s gave is given back"), Id, *U.Ground);
				if (bAuto && Id == TestGrabId)
				{
					const FString Now = Fingerprint(L);
					UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client's things are back as before the take: %s (%s)"), Now == TestGrabPrint ? TEXT("yes") : TEXT("NO"), *Now);
				}
			}
			else
			{
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pickup %d claim denied"), Id);
			}
			break;
		}
		default:
			break;
		}
	});
}

void UOutlawsCoop::OnGroundItem(int32 Id, const FString& Name, const FVector& At, int32 Tag, int32 Epoch)
{
	if (!IsClient()) return;
	RunFor(Epoch, [this, Id, Name, At, Tag]()
	{
		AOutlawsLevelActor* L = FindLevel();
		if (!L) return;
		if (Tag > 0)
		{
			// one of ours: its number
			TWeakObjectPtr<AActor> Ours;
			PendingGround.RemoveAndCopyValue(Tag, Ours);
			const int32 i = PickupIndexOf(L, Ours.Get());
			if (i >= 0) L->Pickups[i].CoopId = Id;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP ground item %d: our %s numbered by the host (tag %d)"), Id, *Name, Tag);
			if (PendingGroundTaken.Remove(Tag) > 0)
			{
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pickup %d taken by client before its number came: the host told"), Id);
				if (AOutlawsCoopLink* Mine = GetLink(MyIndex())) Mine->ServerPickupClaim(Id, LevelEpoch(), ClaimTake);
			}
			return;
		}
		FString Contents;
		const FString Base = SplitGroundName(Name, Contents);
		bApplyingRemote = true;
		const double MadeFrom = FPlatformTime::Seconds();
		AActor* A = L->SpawnGroundItem(Base, At);
		const double MadeMs = (FPlatformTime::Seconds() - MadeFrom) * 1000.0;
		bApplyingRemote = false;
		const int32 i = PickupIndexOf(L, A);
		if (i >= 0)
		{
			L->Pickups[i].CoopId = Id;
			if (IsStickName(Base)) L->AddLooseDynamite(A);
			ApplyGroundContents(L, A, Contents);                  // (a dropped gun: what the dead player carried)
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP ground item %d %s made by the host, appeared here at %s%s (made in %.2f ms)"), Id, *Name, *At.ToString(),
			i < 0 ? TEXT(" (no pickup of that name here)") : TEXT(""), MadeMs);
	});
}

void UOutlawsCoop::UndoTake(AOutlawsLevelActor* L, const FTakeUndo& U)
{
	// what the refused take gave goes again (what the player used meanwhile is not given back twice: counts never go
	// under nothing)
	if (U.Health > 0.f && !L->bDead) L->Health = FMath::Max(0.1f, L->Health - U.Health);
	if (U.bArmour)
	{
		L->Armour = U.Armour;
		L->ArmourMax = U.ArmourMax;
	}
	for (const TPair<FString, int32>& Kv : U.Counts)
	{
		if (int32* N = L->Counts.Find(Kv.Key)) *N = FMath::Max(0, *N - Kv.Value);
	}
	L->Gold = FMath::Max(0, L->Gold - U.Gold);
	for (int32 k = U.Listed.Num() - 1; k >= 0; --k)
	{
		const int32 At = L->Inventory.FindLast(U.Listed[k]);
		if (At == INDEX_NONE) continue;
		L->Inventory.RemoveAt(At);
		if (L->InventoryLabels.IsValidIndex(At)) L->InventoryLabels.RemoveAt(At);
	}
	if (L->Inf)
	{
		for (const int32 Use : U.UseIds)
		{
			bool bStill = false;
			for (const TPair<FString, AOutlawsLevelActor::FItemDef>& Kv : L->ItemDefs)
			{
				if (Kv.Value.UseId == Use && L->GetCount(Kv.Key) > 0) { bStill = true; break; }
			}
			if (!bStill) L->Inf->Items.Remove(Use);
		}
	}
	if (U.bBadge) L->BadgeLeft = U.Badge;
	L->Notify(TEXT("The other player got there first"), 2.5f);
}

// ---- hurt and the players' state ---------------------------------------------------------------------------------------

void UOutlawsCoop::HurtOther(float Damage, float LocationMult, bool bSkipArmour, const FVector* From, int32 SourceEnemy, int32 Who)
{
	if (!IsHost() || Who < 1) return;
	AOutlawsCoopLink* CL = GetLink(Who);
	if (!CL) return;
	CL->ClientHurt(Damage, LocationMult, bSkipArmour, From != nullptr, From ? *From : FVector::ZeroVector, SourceEnemy);
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP hurt sent to the %s: %.2f (where %.2f%s%s)"), WhoName(Who), Damage, LocationMult, bSkipArmour ? TEXT(", past the plate") : TEXT(""),
		SourceEnemy >= 0 ? *FString::Printf(TEXT(", enemy %d's shot"), SourceEnemy) : TEXT(""));
}

void UOutlawsCoop::OnHurt(float Damage, float LocationMult, bool bSkipArmour, bool bHasFrom, const FVector& From, int32 SourceEnemy)
{
	AOutlawsLevelActor* L = FindLevel();
	if (!IsClient() || !L || !bLevelBuilt) return;
	// (2026-10-02) this player in its pause menu: out of harm, nothing taken (not the plate either)
	if (MenuShields(L, Damage)) return;
	const float Before = L->GetHealth();
	// milestone 6: a bad guy's shot meets this player's held plate here first (the host does not know where it is held)
	float Dealt = Damage;
	if (bHasFrom && SourceEnemy >= 0 && !bSkipArmour && PlateTakesHurt(L, From, Dealt)) return;
	if (bHasFrom) L->SetHurtSource(From);
	L->bHurtByBadGuy = SourceEnemy >= 0;
	L->DamagePlayer(Dealt, LocationMult, bSkipArmour);
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP hurt by the host: %.2f, health %.1f -> %.1f%s"), Dealt, Before, L->GetHealth(),
		SourceEnemy >= 0 ? *FString::Printf(TEXT(" (enemy %d's shot, x%.1f where it struck)"), SourceEnemy, LocationMult) : TEXT(""));
}

void UOutlawsCoop::TickWorld(double Now)
{
	if (!bLevelBuilt) return;
	// this player's health, armour and death out when they change (looked at ten times a second)
	AOutlawsLevelActor* L = FindLevel();
	if (L && Now >= NextStatsAt)
	{
		NextStatsAt = Now + 0.1;
		FOutlawsCoopStats S;
		S.Health = L->GetHealth();
		S.MaxHealth = 10.f;                          // (full health: the medical bag and the health pickups top up to 10)
		S.Armour = L->GetArmour();
		S.bDead = L->IsPlayerDead();
		if (S.bDead) S.DieSound = L->DieSound;      // (BUG-MPDEATHSOUND: the worn character's cry, heard from the body elsewhere)
		if (S.bDead) S.DeathBy = L->DeathCause;     // (REQ-KILLFEED: a fall, its own dynamite: the host's kill line for it)
		const AOutlawsPlayer* Me = LocalPlayer();
		S.bInMenu = Me && Me->IsMenuOpen();          // (2026-10-02: the pause menu pauses nothing; the host's bad guys leave this player be)
		KeyItems(L, S.Keys);                         // (BUG-STUCKGAP: a player who leaves has its keys put back)
		S.bSafe = Me && Me->SafeSpots.Num() > 0;
		if (S.bSafe) S.SafeFeet = Me->SafeSpots.Last().Feet;
		if (SentStats.Seq == 0 || !FMath::IsNearlyEqual(S.Health, SentStats.Health, 0.01f) || S.Armour != SentStats.Armour || S.bDead != SentStats.bDead || S.bInMenu != SentStats.bInMenu
			|| S.Keys != SentStats.Keys || S.bSafe != SentStats.bSafe || FVector::Dist(S.SafeFeet, SentStats.SafeFeet) > 100.0)
		{
			S.Seq = SentStats.Seq + 1;
			SentStats = S;
			if (AOutlawsCoopLink* Mine = GetLink(MyIndex())) Mine->ServerStats(S);
		}
	}
	// the others' in: said once each time they change (N1: every other player's)
	TArray<AOutlawsCoopLink*> Links;
	GetLinks(Links);
	for (const AOutlawsCoopLink* OL : Links)
	{
		const int32 Who = OL->PlayerIndex;
		if (Who == MyIndex() || OL->Stats.Seq == 0 || OL->Stats.Seq == LoggedOtherStats[Who]) continue;
		LoggedOtherStats[Who] = OL->Stats.Seq;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the %s's health %.1f, armour %d, %s"), WhoName(Who), OL->Stats.Health, OL->Stats.Armour,
			OL->Stats.bDead ? TEXT("dead") : TEXT("alive"));
	}
}

// ---- tests (-OLCoopAuto): the host leads, the client follows ------------------------------------------------------------

FString UOutlawsCoop::Fingerprint(const AOutlawsLevelActor* L)
{
	TArray<FString> Keys;
	L->Counts.GetKeys(Keys);
	Keys.Sort();
	FString S = FString::Printf(TEXT("health %.1f armour %d gold %d"), L->Health, L->Armour, L->Gold);
	for (const FString& K : Keys)
	{
		const int32 N = L->Counts.FindRef(K);
		if (N != 0) S += FString::Printf(TEXT(" %s=%d"), *K, N);
	}
	S += TEXT(" listed [") + FString::Join(L->Inventory, TEXT(",")) + TEXT("]");
	return S;
}

bool UOutlawsCoop::WouldGive(const AOutlawsLevelActor* L, int32 Index)
{
	// a test aid: GiveEntry's rules, nothing given
	const AOutlawsLevelActor::FPickup& P = L->Pickups[Index];
	if (P.bInstant) return L->Health < 10.f && !L->bDead;
	for (const AOutlawsLevelActor::FPickup::FEntry& E : P.Entries)
	{
		const FString Key = E.Item.ToLower();
		if (Key.IsEmpty() || Key == TEXT("ibadge")) continue;
		const AOutlawsLevelActor::FItemDef* D = L->ItemDefs.Find(Key);
		if (D && D->Class == TEXT("armr"))
		{
			if (E.Count > L->Armour) return true;
			continue;
		}
		int32 Max = D ? D->MaxCount : 1;
		if (Key == TEXT("ipistol") || Key == TEXT("isawgun")) Max = FMath::Max(Max, 2);
		if (E.Count > 0 && L->Counts.FindRef(Key) < Max) return true;
	}
	return false;
}

#if WITH_OUTLAWS_TESTS
int32 UOutlawsCoop::TestCandidate(AOutlawsLevelActor* L, const FVector& Near) const
{
	// the nearest of the level's own pickups that would give something here: not a key (the level needs those), not the
	// badge (it halves the hurt test), not a canteen, not riding a lift, not one tried already
	int32 Best = INDEX_NONE;
	double BestD = 1e30;
	for (int32 i = 0; i < L->Pickups.Num(); ++i)
	{
		const AOutlawsLevelActor::FPickup& P = L->Pickups[i];
		const AActor* A = P.Actor.Get();
		if (!A || P.bHeld || P.CoopId < 0 || P.CoopId >= LevelLooseIdBase || TestTried.Contains(P.CoopId) || P.UseId >= 0 || P.bInstant) continue;
		if (P.Item.Equals(TEXT("ibadge"), ESearchCase::IgnoreCase) || (A->GetRootComponent() && A->GetRootComponent()->GetAttachParent())) continue;
		if (!WouldGive(L, i)) continue;
		const double D = FVector::DistSquared(A->GetActorLocation(), Near);
		if (D < BestD) { BestD = D; Best = i; }
	}
	return Best;
}

bool UOutlawsCoop::TestWalkOver(AOutlawsLevelActor* L, AOutlawsPlayer* P, int32 Id)
{
	const int32 i = Id >= 0 ? PickupIndex(L, Id) : TestCandidate(L, P->GetActorLocation());
	if (i < 0) return false;
	AOutlawsLevelActor::FPickup& Pk = L->Pickups[i];
	AActor* A = Pk.Actor.Get();
	if (!A) return false;
	TestPickup = Pk.CoopId;
	TestTried.Add(Pk.CoopId);
	TestPickupHome = A->GetActorLocation();
	// at this player's feet: the level's own walk-over (UpdatePickups) takes it on its next tick
	A->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
	A->SetActorLocation(P->GetActorLocation() - FVector(0.0, 0.0, P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() - 2.0));
	Pk.bWaitClear = false;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the %s walks over pickup %d (%s, moved from %s)"), WhoName(MyIndex()), TestPickup, *Pk.Ground, *TestPickupHome.ToString());
	return true;
}

FVector UOutlawsCoop::TestDropSpot(AOutlawsPlayer* P) const
{
	const FVector Me = P->GetActorLocation();
	const AOutlawsCoopLink* OL = GetLink(PartnerIndex());
	FVector Away = OL ? (Me - FVector(OL->Pose.Body)).GetSafeNormal2D() : FVector::ZeroVector;
	if (Away.IsNearlyZero()) Away = FRotator(0.0, P->GetActorRotation().Yaw, 0.0).Vector();
	const float Half = P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const FVector Ways[] = { Away, FVector(-Away.Y, Away.X, 0.0), FVector(Away.Y, -Away.X, 0.0), -Away };
	UWorld* W = P->GetWorld();
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsCoopTestDrop), false, P);
	for (const FVector& Way : Ways)
	{
		const FVector To = Me + Way * 150.0;
		FHitResult Hit;
		if (W && W->LineTraceSingleByChannel(Hit, Me, To, ECC_Visibility, Q)) continue;            // (a wall in the way)
		if (W && W->LineTraceSingleByChannel(Hit, To, To - FVector(0.0, 0.0, Half + 60.0), ECC_Visibility, Q)) return Hit.ImpactPoint;
	}
	return Me - FVector(0.0, 0.0, Half) + Away * 150.0;
}

void UOutlawsCoop::TestSay(const FString& Step, int32 Arg)
{
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host asks the client to %s (%d)"), *Step, Arg);
	if (Step == TEXT("quit"))
	{
		ForClients([&](AOutlawsCoopLink& K) { K.ClientTest(Step, Arg); });      // (N1: every client ends with the test)
		return;
	}
	if (AOutlawsCoopLink* CL = GetLink(1)) CL->ClientTest(Step, Arg);
}

void UOutlawsCoop::OnTestNote(const FString& Note)
{
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client says: %s"), *Note);
	TestNotes.Add(Note);
	if (TestStep == 1 && Note.StartsWith(TEXT("walkover"))) TestAt = FPlatformTime::Seconds() + 0.5;
	if (Note.StartsWith(TEXT("acdone"))) AcClientDone = true;           // (BUG-ALLOWCHEATS's check: the client has tried its cheat)
	if (Note.StartsWith(TEXT("readyflip "))) OnReadyFlipNote(Note);       // (BUG-READYFLIP's check)
	if (Note.StartsWith(TEXT("pause "))) OnPauseTestNote(Note);           // (BUG-COOPTIMEOUT's checks)
}

void UOutlawsCoop::TickTest(double Now)
{
	if (!bWorldTest || !bLevelBuilt || bExitRequested || bSummaryDone) return;
	AOutlawsLevelActor* L = FindLevel();
	AOutlawsPlayer* P = LocalPlayer();
	if (!L || !P) return;
	if (IsDropoutTest()) return;                 // (R8: the dropout tests run on their own: TickDropoutTest)
	if (IsPauseTest()) return;                   // (BUG-COOPTIMEOUT's run on their own too: TickPauseTest)
	if (IsHost() && TestOnly == TEXT("party")) TickHostPartyTest(Now, L, P);          // (N1: three or more players, OutlawsCoopParty.cpp)
	else if (IsHost() && TestOnly == TEXT("dm")) TickHostMatchTest(Now, L, P);       // (M1: a Death Match, OutlawsCoopMatch.cpp)
	else if (IsHost() && TestOnly == TEXT("modes")) TickHostModesTest(Now, L, P);    // (M2: each class's checks, OutlawsCoopModes.cpp)
	else if (TestOnly == TEXT("modes")) return;
	else if (IsHost() && TestOnly == TEXT("dmstrike")) TickHostStrikeTest(Now, L, P);   // (M1's gaps: OutlawsCoopMatchStrikes.cpp)
	else if (IsHost() && TestOnly == TEXT("killfeed")) TickHostKillFeedTest(Now, L, P);  // (REQ-KILLFEED: OutlawsCoopKillFeed.cpp)
	else if (TestOnly == TEXT("killfeed")) return;
	else if (IsHost() && TestOnly == TEXT("remotefx")) TickHostRemoteFxTest(Now, L, P);   // (BUG-REMOTEGUNSOUND: OutlawsCoopRemoteFxTest.cpp)
	else if (TestOnly == TEXT("remotefx")) return;
	else if (IsHost() && TestOnly == TEXT("flicknet")) TickHostFlickNetTest(Now, L, P);   // (REQ-FLICKRELOAD: OutlawsCoopFlickTest.cpp)
	else if (TestOnly == TEXT("flicknet")) return;
	else if (IsHost() && TestOnly == TEXT("fuzz")) TickHostFuzzTest(Now, L, P);      // (M1 hardening: OutlawsCoopGuard.cpp)
	else if (IsHost() && TestOnly == TEXT("password"))
	{
		// (M1: the password test, run_password.ps1) the joiners without it or with a wrong one were turned away in PreLogin;
		// the one with it gets in: its level built, it is said and the run ends
		const AOutlawsCoopLink* CL = GetLink(PartnerIndex());
		if (TestStep == 0 && CL && CL->BuiltSerial == StartedSerial)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: password: a player got in with the password and built %s"), *BuiltLevel);
			TestStep = 1;
			TestAt = Now + 2.0;
		}
		else if (TestStep == 1 && Now >= TestAt)
		{
			TestSay(TEXT("quit"));
			TestStep = 100;
			TestAt = Now + 2.0;
		}
		else if (TestStep == 100 && Now >= TestAt)
		{
			Summary(TEXT("done"));
			QuitForTest();
			TestStep = 101;
		}
	}
	else if (TestOnly == TEXT("bots")) TickBotTest(Now, L, P);                      // (REQ-MPBOTS: OutlawsCoopBots.cpp; the host and its client each)
	else if (TestOnly == TEXT("lanternswim")) TickLanternSwimTest(Now, L, P);       // (REQ-LANTERNSWIM: OutlawsCoopVerbs.cpp; the host and its client each)
	else if (TestOnly == TEXT("lanternnet")) TickLanternNetTest(Now, L, P);         // (BUG-LANTERNNET: OutlawsCoopLanternNet.cpp; the host and its client each)
	else if (TestOnly == TEXT("climbwheel")) TickClimbWheelTest(Now, L, P);         // (REQ-CLIMBWHEEL: OutlawsClimbWheel.cpp; the host and its client each)
	else if (TestOnly == TEXT("zonegate")) TickZoneGateTest(Now, L, P);             // (BUG-MILLCLIMB: OutlawsCoopZoneGate.cpp; the host and its client each)
	else if (TestOnly.StartsWith(TEXT("jumppick"))) TickJumpPickTest(Now, L, P);    // (BUG-JUMPPICK: OutlawsCoopJumpPick.cpp; the host and its client each)
	else if (TestOnly == TEXT("allowcheats")) TickAllowCheatsTest(Now, L, P);       // (BUG-ALLOWCHEATS: OutlawsCoopCheats.cpp; the host and its client each)
	else if (TestOnly == TEXT("cheatscope")) TickCheatScopeTest(Now, L, P);         // (REQ-CHEATSCOPE: OutlawsCoopCheatScope.cpp; the host and its client each)
	else if (TestOnly == TEXT("skipinv")) TickSkipInvTest(Now, L, P);                   // (BUG-SKIPLEVELINV: OutlawsCoopSkipInv.cpp; the host and its client each)
	else if (TestOnly == TEXT("levelcarry")) TickLevelCarryTest(Now, L, P);             // (BUG-LEVELCARRY: OutlawsLevelCarryTest.cpp; the host and its client each)
	else if (TestOnly == TEXT("unlock")) TickUnlockTest(Now, L, P);                 // (REQ-UNLOCKS: OutlawsUnlocksTest.cpp; the host and its client each)
	else if (TestOnly == TEXT("bossend")) TickBossEndTest(Now, L, P);               // (BUG-RANCHDOOR, BUG-DRDEATHANIM: OutlawsCoopBossEnd.cpp; each)
	else if (TestOnly == TEXT("piano")) TickPianoTest(Now, L, P);                   // (REQ-PIANO: OutlawsPianoTest.cpp; the host and its client each)
	else if (TestOnly == TEXT("float")) TickFloatTest(Now, L, P);                   // (REQ-ENEMYFLOAT: OutlawsEnemyFloatTest.cpp; the host and its client each)
	else if (TestOnly == TEXT("dm") || TestOnly == TEXT("dmstrike") || TestOnly == TEXT("lobbyrule") || TestOnly == TEXT("readyflip")) return;      // (REQ-MPLOBBY's lobby run: the script watches)                                         // (its clients only follow the host's steps)
	else if (IsHost() && TestOnly.StartsWith(TEXT("latejoin"))) TickHostLateJoinTest(Now, L, P);     // (R7: alone first: OutlawsCoopJoin.cpp)
	else if (IsHost()) TickHostTest(Now, L, P);
	else if (IsClient()) TickClientTest(Now, L, P);
}

void UOutlawsCoop::TickHostTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	AOutlawsCoopLink* CL = GetLink(1);
	if (!CL) return;
	if (TestStep == 0)
	{
		if (CL->BuiltSerial != StartedSerial) return;                // (the client's level is not up yet)
		if (TestAt == 0.0) { TestAt = Now + 4.0; return; }          // (milestone 2's look at each other first)
		if (Now < TestAt) return;
		if (TestOnly.StartsWith(TEXT("inf")))
		{
			TestStep = 40;                       // (-OLCoopTest=inf or infend: milestone 5's checks alone, OutlawsCoopInf.cpp)
			TestAt = Now;
			return;
		}
		if (TestOnly.StartsWith(TEXT("unstuck")))
		{
			TestStep = 700;                      // (BUG-STUCKGAP: keys never lost, the stuck rescue; OutlawsCoopUnstuck.cpp)
			TestAt = Now;
			return;
		}
		if (TestOnly == TEXT("idlehost"))
		{
			TestStep = 820;                      // (BUG-BOSSIDLEHOST: the bad guys and an idle host, OutlawsCoopIdle.cpp)
			TestAt = Now;
			return;
		}
		if (TestOnly == TEXT("rules"))
		{
			TestStep = 60;                       // (-OLCoopTest=rules: milestone 7's checks alone, OutlawsCoopRules.cpp)
			TestAt = Now;
			return;
		}
		if (TestOnly == TEXT("save") || TestOnly == TEXT("load"))
		{
			TestStep = 140;                      // (-OLCoopTest=save | load: milestone 8's checks, OutlawsCoopSaves.cpp)
			TestAt = Now;
			return;
		}
		if (TestOnly == TEXT("verbs"))
		{
			TestStep = 110;                      // (-OLCoopTest=verbs: milestone 6's checks alone, OutlawsCoopVerbs.cpp)
			TestAt = Now;
			return;
		}
		if (TestOnly == TEXT("grab"))
		{
			TestStep = 150;                      // (-OLCoopTest=grab: milestone 6 item 4's checks alone, OutlawsCoopGrabs.cpp)
			TestAt = Now;
			return;
		}
		if (TestOnly == TEXT("puppet"))
		{
			TestStep = 190;                      // (-OLCoopTest=puppet: the other player's sprite and its choreographies, OutlawsCoop.cpp)
			TestAt = Now;
			return;
		}
		if (TestOnly == TEXT("voice"))
		{
			TestStep = 300;                      // (-OLCoopTest=voice: voice chat in the level, the lobby part before it; OutlawsCoopVoice.cpp)
			TestAt = Now;
			return;
		}
		if (TestOnly == TEXT("nameplate"))
		{
			TestStep = 260;                      // (-OLCoopTest=nameplate: the other player's name and health bar, OutlawsCoopNameplate.cpp)
			TestAt = Now;
			return;
		}
		if (TestOnly == TEXT("pass"))
		{
			TestStep = 400;                      // (-OLCoopTest=pass: the wheel's drop passed to the other player, OutlawsWheelDrop.cpp)
			TestAt = Now;
			return;
		}
		if (TestOnly == TEXT("extras"))
		{
			TestStep = 230;                     // (-OLCoopTest=extras: the extra bad guys for the second player, OutlawsCoopExtras.cpp)
			TestExtraLevel = 0;
			TestAt = Now;
			return;
		}
		// 1: the client walks over one of the level's pickups; the host's own things stay as they are
		TestPrint = Fingerprint(L);
		TestSay(TEXT("walkover"), -1);
		TestStep = 1;
		TestAt = Now + 8.0;
		return;
	}
	switch (TestStep)
	{
	case 1:
	{
		if (Now < TestAt) return;
		const FString After = Fingerprint(L);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host's own things after the client's take: %s (%s)"), After == TestPrint ? TEXT("unchanged") : TEXT("CHANGED"), *After);
		// 2: the host walks over one; the client looks at its own things before and after
		TestSay(TEXT("watch"));
		TestTried.Reset();
		TestTries = 0;
		TestPickup = -1;
		TestStep = 2;
		TestAt = Now + 0.5;
		return;
	}
	case 2:
	{
		if (Now < TestAt) return;
		if (TestPickup >= 0)
		{
			const int32 i = PickupIndex(L, TestPickup);
			if (i < 0)
			{
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host walked over pickup %d and took it"), TestPickup);
				TestSay(TEXT("check"), TestPickup);
				TestStep = 3;
				TestAt = Now + 1.5;
				return;
			}
			if (AActor* A = L->Pickups[i].Actor.Get()) A->SetActorLocation(TestPickupHome);     // (nothing of it fitted: put back)
		}
		if (++TestTries > 6 || !TestWalkOver(L, P, -1))
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host found nothing it could take by walking over it"));
			TestStep = 3;
			TestAt = Now;
			return;
		}
		TestAt = Now + 1.0;
		return;
	}
	case 3:
	{
		// 3: both take one pickup in the same instant: the client hears "grab" and takes it at once; the host takes it
		// straight after sending that, so its own take reaches the host's word first
		if (Now < TestAt) return;
		TestTried.Reset();
		const int32 i = TestCandidate(L, P->GetActorLocation());
		if (i >= 0)
		{
			TestGrabId = L->Pickups[i].CoopId;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: both players take pickup %d (%s) in the same instant"), TestGrabId, *L->Pickups[i].Ground);
			TestSay(TEXT("grab"), TestGrabId);
			L->CollectPickup(i);
		}
		else
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: no pickup left for the same-instant grab"));
		}
		TestStep = 4;
		TestAt = Now + 2.5;
		return;
	}
	case 4:
		// 4: the client takes one into a hand and puts it down again
		if (Now < TestAt) return;
		TestSay(TEXT("hold"));
		TestStep = 5;
		TestAt = Now + 3.5;
		return;
	case 5:
		// 5: things made in play: the client puts a knife down; then the host does, and the client walks over the host's
		if (Now < TestAt) return;
		TestSay(TEXT("drop"));
		TestStep = 6;
		TestAt = Now + 1.5;
		return;
	case 6:
	{
		if (Now < TestAt) return;
		AActor* A = L->SpawnGroundItem(TEXT("gknife"), TestDropSpot(P));
		const int32 i = PickupIndexOf(L, A);
		TestDropId = i >= 0 ? L->Pickups[i].CoopId : -1;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host put a knife down: ground item %d"), TestDropId);
		TestStep = 7;
		TestAt = Now + 1.0;
		return;
	}
	case 7:
		// the client walks over the host's knife
		if (Now < TestAt) return;
		if (TestDropId >= 0) TestSay(TEXT("walkover"), TestDropId);
		TestStep = 8;
		TestAt = Now + 2.5;
		return;
	case 8:
		// the host walks over the client's knife (numbered by the host, made on the client)
		if (Now < TestAt) return;
		TestTried.Reset();
		TestPickup = -1;
		if (TestClientDropId >= 0 && TestWalkOver(L, P, TestClientDropId))
		{
			TestStep = 81;
			TestAt = Now + 1.0;
			return;
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client's knife (%d) is not here to walk over"), TestClientDropId);
		TestStep = 82;
		TestAt = Now;
		return;
	case 81:
		if (Now < TestAt) return;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host walked over the client's knife %d: %s"), TestClientDropId, PickupIndex(L, TestClientDropId) < 0 ? TEXT("taken") : TEXT("STILL THERE"));
		TestStep = 82;
		TestAt = Now + 1.0;
		return;
	case 82:
	{
		// the hurt event: 1.5 from where the host stands
		if (Now < TestAt) return;
		const FVector From = P->GetActorLocation();
		HurtOther(1.5f, 1.f, false, &From);
		TestStep = 9;
		TestAt = Now + 2.0;
		return;
	}
	case 9:
		if (Now < TestAt) return;
		TestStep = 20;                       // (milestone 4's enemy checks: OutlawsCoopEnemies.cpp)
		TestAt = Now;
		return;
	case 100:
		if (Now < TestAt) return;
		Summary(TEXT("done"));
		QuitForTest();
		TestStep = 101;
		return;
	default:
		if (TestStep >= 20 && TestStep < 40) TickHostEnemyTest(Now, L, P);
		else if (TestStep >= 40 && TestStep < 60) TickHostInfTest(Now, L, P);       // (milestone 5: OutlawsCoopInf.cpp)
		else if (TestStep >= 60 && TestStep < 100) TickHostRulesTest(Now, L, P);    // (milestone 7: OutlawsCoopRules.cpp)
		else if (TestStep >= 110 && TestStep < 140) TickHostVerbsTest(Now, L, P);   // (milestone 6: OutlawsCoopVerbs.cpp)
		else if (TestStep >= 140 && TestStep < 150) TickHostSaveTest(Now, L, P);    // (milestone 8: OutlawsCoopSaves.cpp)
		else if (TestStep >= 150 && TestStep < 190) TickHostGrabTest(Now, L, P);    // (milestone 6 item 4: OutlawsCoopGrabs.cpp)
		else if (TestStep >= 190 && TestStep < 230) TickHostPuppetTest(Now, L, P);  // (the puppet's sprite: OutlawsCoop.cpp)
		else if (TestStep >= 230 && TestStep < 260) TickHostExtrasTest(Now, L, P);  // (2026-10-02, the extra bad guys: OutlawsCoopExtras.cpp)
		else if (TestStep >= 260 && TestStep < 290) TickHostNameplateTest(Now, L, P);   // (2026-10-02, the name and health bar: OutlawsCoopNameplate.cpp)
		else if (TestStep >= 300 && TestStep < 320) TickHostVoiceTest(Now, L, P);   // (2026-10-02, voice chat: OutlawsCoopVoice.cpp)
		else if (TestStep >= 400 && TestStep < 420) TickHostPassTest(Now, L, P);    // (2026-10-02, the wheel's drop: OutlawsWheelDrop.cpp)
		else if (TestStep >= 700 && TestStep < 720) TickHostUnstuckTest(Now, L, P); // (BUG-STUCKGAP: OutlawsCoopUnstuck.cpp)
		else if (TestStep >= 820 && TestStep < 840) TickHostIdleTest(Now, L, P);    // (BUG-BOSSIDLEHOST: OutlawsCoopIdle.cpp)
		return;
	}
}

void UOutlawsCoop::OnTestStep(const FString& Step, int32 Arg)
{
	if (!bAuto) return;                          // (a test aid only)
	AOutlawsLevelActor* L = FindLevel();
	AOutlawsPlayer* P = LocalPlayer();
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test step for the client: %s (%d)"), *Step, Arg);
	if (OnReadyFlipStep(Step, Arg)) return;          // (BUG-READYFLIP's check: in the lobby, no level needed)
	if (!L || !P) return;
	const double Now = FPlatformTime::Seconds();
	const UCameraComponent* Cam = P->Camera.Get();
	const FVector Eye = Cam ? Cam->GetComponentLocation() : P->GetActorLocation();
	if (Step == TEXT("walkover"))
	{
		TestTried.Reset();
		TestTries = 0;
		TestPickup = -1;
		TestArg = Arg;
		TestStep = 1;
		TestAt = Now;
	}
	else if (Step == TEXT("watch"))
	{
		TestPrint = Fingerprint(L);
	}
	else if (Step == TEXT("check"))
	{
		const FString After = Fingerprint(L);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client's own things after the host's take of pickup %d: %s (%s)"), Arg, After == TestPrint ? TEXT("unchanged") : TEXT("CHANGED"), *After);
	}
	else if (Step == TEXT("grab"))
	{
		const int32 i = PickupIndex(L, Arg);
		TestGrabPrint = Fingerprint(L);
		TestGrabId = Arg;
		const bool bOk = i >= 0 && L->CollectPickup(i);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client takes pickup %d at once: %s"), Arg, bOk ? TEXT("taken here") : i < 0 ? TEXT("not here") : TEXT("nothing of it fitted"));
	}
	else if (Step == TEXT("hold"))
	{
		// the nearest pickup, brought into sight 1.5 m away (a hand needs to see what it takes), then taken by the hand's own
		// call; the host does not see it move until it is put down
		TestTried.Reset();
		const int32 Wi = TestCandidate(L, P->GetActorLocation());
		AActor* Want = Wi >= 0 ? L->Pickups[Wi].Actor.Get() : nullptr;
		if (Want)
		{
			Want->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
			Want->SetActorLocation(TestDropSpot(P));
		}
		AActor* Got = Want ? L->TakePickupNear(Eye, Want->GetActorLocation() + FVector(0.0, 0.0, FMath::Max(1.0, Want->GetActorScale3D().Z) * 0.5), 60.f) : nullptr;
		TestHeld = Got;
		TestHeldUntil = Now + 1.2;
		const int32 Gi = PickupIndexOf(L, Got);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client takes pickup %d into a hand"), Gi >= 0 ? L->Pickups[Gi].CoopId : -1);
	}
	else if (Step == TEXT("drop"))
	{
		AActor* A = L->SpawnGroundItem(TEXT("gknife"), TestDropSpot(P));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client put a knife down (%s)"), A ? TEXT("made") : TEXT("not made"));
	}
	else if (Step == TEXT("enemy"))
	{
		TestLogEnemy(L, Arg, TEXT("asked"));
	}
	else if (Step == TEXT("hit") || Step == TEXT("kill"))
	{
		// the client's own hit on its copy of an enemy, by the same call its guns and fists make (milestone 4)
		const TArray<TWeakObjectPtr<AOutlawsEnemy>>& List = L->GetEnemies();
		AOutlawsEnemy* E = List.IsValidIndex(Arg) ? List[Arg].Get() : nullptr;
		if (E)
		{
			const float Damage = Step == TEXT("hit") ? 1.f : 100.f;
			const FVector At = E->GetActorLocation();
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client %s enemy %d for %.2f (%s)"), Step == TEXT("hit") ? TEXT("hits") : TEXT("shoots"), Arg, Damage, *E->CoopDescribe());
			E->TakeHit(Damage, At, (At - Eye).GetSafeNormal(), Eye);
		}
	}
	else if (Step == TEXT("infdoor"))
	{
		// milestone 5: stand outside that elevator's door, then push it (the hand's push, a moment later)
		TestGoToDoor(L, P, Arg);
		TestPushAt = Now + 0.8;
	}
	else if (OnUnstuckTestStep(Step, Arg, L, P))
	{
		// (BUG-STUCKGAP's steps: OutlawsCoopUnstuck.cpp)
	}
	else if (Step == TEXT("rkey"))
	{
		// milestone 7: the brass key in this player's pockets (as a pickup gives it), to be dropped at a death
		L->AddCount(TEXT("ibrsskey"), 1);
		L->Inventory.Add(TEXT("ibrsskey"));
		L->InventoryLabels.Add(TEXT("Brass key"));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client holds the brass key (count %d, use id %d %s)"), L->GetCount(TEXT("ibrsskey")), 101, L->HasItem(101) ? TEXT("held") : TEXT("NOT HELD"));
	}
	else if (Step == TEXT("die"))
	{
		// milestone 7: a test's death, past the test's own invincibility (as a shot to the head is)
		const bool bWas = L->bInvincible;
		L->bInvincible = false;
		L->DamagePlayer(1e6f, 1.f, true);
		L->bInvincible = bWas;
		TestFellAt = P->GetActorLocation();
		TestRulesNote = TestFellAt.ToCompactString();
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client dies at %s (keys left %d)"), *TestRulesNote, L->GetCount(TEXT("ibrsskey")));
	}
	else if (Step == TEXT("rstate"))
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client after the host's grip: %s, health %.1f at %s (fell at %s), %s"), L->bDead ? TEXT("STILL DEAD") : TEXT("alive"), L->Health,
			*P->GetActorLocation().ToCompactString(), *TestRulesNote, *Fingerprint(L));
	}
	else if (Step == TEXT("rguns"))
	{
		// 2026-10-02: guns found in the level beyond the start kit, as their pickups give them (listed on the wrist as a gun
		// is): a second revolver, a shotgun and its shells, three sticks of dynamite, two knives
		const TPair<const TCHAR*, int32> Found[] = { { TEXT("ipistol"), 1 }, { TEXT("isgun"), 1 }, { TEXT("ishells"), 17 }, { TEXT("idynam"), 3 }, { TEXT("iknife"), 2 } };
		for (const TPair<const TCHAR*, int32>& F : Found)
		{
			AOutlawsLevelActor::FPickup::FEntry E;
			E.Item = F.Key;
			E.Count = F.Value;
			E.Class = FCString::Strcmp(F.Key, TEXT("ishells")) == 0 ? TEXT("ammo") : TEXT("weap");
			L->GiveEntry(E);
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client carries %s"), *Fingerprint(L));
	}
	else if (Step == TEXT("rback"))
	{
		// 2026-10-02: back from the level's start to where it fell, where its guns lie: walked over as any pickups
		const float Half = P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		FHitResult Floor;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsCoopTestBack), false, P);
		FVector At = TestFellAt;
		if (P->GetWorld()->LineTraceSingleByChannel(Floor, TestFellAt + FVector(0.0, 0.0, 50.0), TestFellAt - FVector(0.0, 0.0, Half + 200.0), ECC_Visibility, Q)) At = Floor.ImpactPoint + FVector(0.0, 0.0, Half + 3.0);
		P->TeleportTo(At, P->GetActorRotation(), false, true);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client goes back to where it fell (%s): %s"), *TestRulesNote, *Fingerprint(L));
	}
	else if (Step == TEXT("rbacklog"))
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client back where it fell: %s"), *Fingerprint(L));
	}
	else if (Step == TEXT("rrevive"))
	{
		// milestone 7: the client, with a med bag (2026-10-02: one bag, 3 points), grips the host's body
		L->Counts.Remove(TEXT("idocbag"));
		L->AddCount(TEXT("idocbag"), 3);
		const AOutlawsRemotePlayer* R = PuppetOf(0);  // (the host's body)
		FVector Hand = P->GetActorLocation();
		if (R)
		{
			const FVector Feet = R->ShownBody() - FVector(0.0, 0.0, R->ShownHalf());
			const float Half = P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), Rad = P->GetCapsuleComponent()->GetScaledCapsuleRadius();
			FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsCoopTestBody), false, P);
			for (int32 k = 0; k < 8; ++k)
			{
				const FVector At = Feet + FRotator(0.0, 45.0 * k, 0.0).Vector() * 140.0 + FVector(0.0, 0.0, Half + 3.0);
				if (P->GetWorld()->OverlapBlockingTestByChannel(At, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(Rad, Half), Q)) continue;
				P->TeleportTo(At, FRotator(0.0, (Feet - At).Rotation().Yaw, 0.0), false, true);
				break;
			}
			Hand = Feet + FVector(0.0, 0.0, 25.0);
		}
		const int32 Bags = L->GetCount(TEXT("idocbag"));
		const bool bTook = TryRevive(P, Hand);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client (med bag points %d) grips the host's body: %s"), Bags, bTook ? TEXT("the grip took it") : TEXT("NOT TAKEN"));
	}
	else if (Step == TEXT("respawn"))
	{
		// milestone 7: the death screen's button (the client's own choice): back at the level's start with the start kit
		P->RequestRespawn(false);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client after its respawn: %s at %s (the level's start %s), %s"), L->bDead ? TEXT("STILL DEAD") : TEXT("alive"),
			*P->GetActorLocation().ToCompactString(), *L->GetPlayerStartLocation().ToCompactString(), *Fingerprint(L));
	}
	else if (Step == TEXT("rdoor"))
	{
		TestPauseDoor = Arg;
	}
	else if (Step.StartsWith(TEXT("rlog:")))
	{
		TestLogEnemy(L, Arg, *Step.Mid(5));
		if (TestPauseDoor >= 0) TestLogElevator(L, TestPauseDoor, *Step.Mid(5));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client at %s, its menu %s [%s]"), *P->GetActorLocation().ToCompactString(), P->IsMenuOpen() ? TEXT("open") : TEXT("closed"), *Step.Mid(5));
	}
	else if (Step == TEXT("pause"))
	{
		// this player's pause menu opened or closed (2026-10-02: for this player only; the game goes on for both)
		if (Arg) P->CoopOpenMenu();
		else P->CoopCloseMenu();
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client's menu %s"), P->IsMenuOpen() ? TEXT("open") : TEXT("closed"));
	}
	else if (Step == TEXT("rwalk"))
	{
		TestWalkOpen(P, Arg / 1000.f);           // (2026-10-02: the client walks on while the host's menu is open)
	}
	else if (Step == TEXT("godmode"))
	{
		L->bInvincible = Arg != 0;              // (a test aid: milestone 5's checks are not about getting shot)
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client is %s"), Arg ? TEXT("invincible") : TEXT("mortal again"));
	}
	else if (Step == TEXT("infsector"))
	{
		TestDoorSector = Arg;                    // (milestone 5: a scripted sector pushed from where this player stands)
		TestPushAt = Now + 0.3;
	}
	else if (Step == TEXT("infstate"))
	{
		TestLogElevator(L, Arg, TEXT("asked"));
	}
	else if (Step == TEXT("infkey"))
	{
		L->GiveItem(Arg);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client holds key item %d"), Arg);
	}
	else if (Step == TEXT("skip"))
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client skips the movie (playing %d)"), L->IsMoviePlaying() ? 1 : 0);
		if (L->IsMoviePlaying()) L->SkipMovie();
	}
	else if (Step.StartsWith(TEXT("v:")))
	{
		OnVerbsTestStep(Step.Mid(2), Arg, L, P);     // (milestone 6: OutlawsCoopVerbs.cpp)
	}
	else if (Step.StartsWith(TEXT("s:")))
	{
		OnSaveTestStep(Step.Mid(2), Arg, L, P);      // (milestone 8: OutlawsCoopSaves.cpp)
	}
	else if (Step.StartsWith(TEXT("g:")))
	{
		OnGrabTestStep(Step.Mid(2), Arg, L, P);      // (milestone 6 item 4: OutlawsCoopGrabs.cpp)
	}
	else if (Step.StartsWith(TEXT("p:")))
	{
		OnPuppetTestStep(Step.Mid(2), Arg, L, P);    // (the puppet's sprite: OutlawsCoop.cpp)
	}
	else if (Step.StartsWith(TEXT("n:")))
	{
		OnNameplateTestStep(Step.Mid(2), Arg, L, P);     // (2026-10-02, the name and health bar: OutlawsCoopNameplate.cpp)
	}
	else if (Step.StartsWith(TEXT("vc:")))
	{
		OnVoiceTestStep(Step.Mid(3), Arg);           // (2026-10-02, voice chat: OutlawsCoopVoice.cpp)
	}
	else if (Step.StartsWith(TEXT("x:")))
	{
		OnExtrasTestStep(Step.Mid(2), Arg, L, P);    // (2026-10-02, the extra bad guys: OutlawsCoopExtras.cpp)
	}
	else if (Step.StartsWith(TEXT("lj:")))
	{
		OnLateJoinTestStep(Step.Mid(3), Arg, L, P);  // (R7, a late join: OutlawsCoopJoin.cpp)
	}
	else if (Step.StartsWith(TEXT("wd:")))
	{
		OnPassTestStep(Step.Mid(3), Arg, L, P);      // (2026-10-02, the wheel's drop: OutlawsWheelDrop.cpp)
	}
	else if (Step.StartsWith(TEXT("fz:")))
	{
		OnFuzzTestStep(Step.Mid(3), L, P);           // (M1 hardening's test: OutlawsCoopGuard.cpp)
	}
	else if (Step.StartsWith(TEXT("md:")))
	{
		OnModesTestStep(Step.Mid(3), Arg, L, P);     // (M2's test: OutlawsCoopModes.cpp)
	}
	else if (Step.StartsWith(TEXT("rf:")))
	{
		OnRemoteFxTestStep(Step.Mid(3), Arg, L, P);  // (BUG-REMOTEGUNSOUND's test: OutlawsCoopRemoteFxTest.cpp)
	}
	else if (Step.StartsWith(TEXT("fl:")))
	{
		OnFlickNetTestStep(Step.Mid(3), Arg, L, P);  // (REQ-FLICKRELOAD's test: OutlawsCoopFlickTest.cpp)
	}
	else if (Step.StartsWith(TEXT("kf:")))
	{
		OnKillFeedTestStep(Step.Mid(3), Arg, L, P);  // (REQ-KILLFEED's test: OutlawsCoopKillFeed.cpp)
	}
	else if (Step.StartsWith(TEXT("ds:")))
	{
		OnStrikeTestStep(Step.Mid(3), Arg, L, P);    // (M1's gaps' test: OutlawsCoopMatchStrikes.cpp)
	}
	else if (Step.StartsWith(TEXT("dm:")))
	{
		OnMatchTestStep(Step.Mid(3), Arg, L, P);     // (M1's Death Match test: OutlawsCoopMatch.cpp)
	}
	else if (Step.StartsWith(TEXT("pt:")))
	{
		OnPartyTestStep(Step.Mid(3), Arg, L, P);     // (N1's party test: OutlawsCoopParty.cpp)
	}
	else if (Step == TEXT("quit"))
	{
		Summary(TEXT("done"));
		QuitForTest();
		if (TestOnly == TEXT("unlock")) EndUnlockTestAtSessionEnd(L);     // (the client's RESULT: the host's skip ended the session)
	}
}

void UOutlawsCoop::TickClientTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	TickClientPartyTest(Now);                    // (N1's party test: the puppet check, waited for)
	TickGrabTestHand(P);                         // (milestone 6 item 4: the hand held out for a bad guy)
	TickClientGrabTest(P);                       // (BUG-TNTARCTRIGGER: the stick's stages, one a frame)
	if (TestPushAt > 0.0 && Now >= TestPushAt)
	{
		// milestone 5: the door the host sent this player to, pushed as a hand push does
		TestPushAt = 0.0;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client (keys %d) pushes sector %d: %s"), L->Inf ? L->Inf->Items.Num() : 0, TestDoorSector,
			L->PushDoorSector(TestDoorSector) ? TEXT("taken") : TEXT("not taken"));
	}
	if (AActor* H = TestHeld.Get(); H && Now >= TestHeldUntil)
	{
		const UCameraComponent* Cam = P->Camera.Get();
		L->DropPickupActor(H, Cam ? Cam->GetComponentLocation() : P->GetActorLocation());
		TestHeld.Reset();
	}
	if (TestStep != 1 || Now < TestAt) return;
	// walking over a pickup: taken (gone from the list), or put back and the next one tried
	if (TestPickup >= 0)
	{
		const int32 i = PickupIndex(L, TestPickup);
		if (i < 0)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client walked over pickup %d and took it"), TestPickup);
			if (AOutlawsCoopLink* Mine = GetLink(MyIndex())) Mine->ServerTestNote(FString::Printf(TEXT("walkover %d taken"), TestPickup));
			TestStep = 0;
			return;
		}
		if (TestArg < 0)
		{
			if (AActor* A = L->Pickups[i].Actor.Get()) A->SetActorLocation(TestPickupHome);
		}
	}
	if (++TestTries > (TestArg >= 0 ? 3 : 6) || !TestWalkOver(L, P, TestArg))
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client could not take %s by walking over it"), TestArg >= 0 ? *FString::Printf(TEXT("pickup %d"), TestArg) : TEXT("any pickup"));
		if (AOutlawsCoopLink* Mine = GetLink(MyIndex())) Mine->ServerTestNote(TEXT("walkover failed"));
		TestStep = 0;
		return;
	}
	TestAt = Now + 1.0;
}
#endif // WITH_OUTLAWS_TESTS
