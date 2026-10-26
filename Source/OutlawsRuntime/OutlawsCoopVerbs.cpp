// Story co-op (tools/olue/COOP_DESIGN.md), milestone 6: the shared world's verbs.  A blast, a wall blown, a pane of glass
// shattered, a prop's reaction or a prop taken off its shelf, a lamp's shade broken or a lamp swung happens on the machine
// where it was done (as in single player) and is told to the other, which does the same by index: the walls, panes,
// props and lamps are built in the same order on both machines.  A blast told from the other machine is its sight, its
// sound and this machine's own player's hurt only: the bad guys (the host deals every hit), the props, the glass, the next
// sticks in a chain and the INF each come as events of their own, so nothing happens twice.  A gunshot is heard and seen
// at the other player's muzzle, and a throw flies there as a stand-in.
//
// None of this runs in single player: the level actor and the player call in only when UOutlawsCoop::IsActive.
#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsRemotePlayer.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsBoxHand.h"
#include "OutlawsGeometry.h"
#include "ProceduralMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "Camera/CameraComponent.h"
#include "Engine/World.h"

namespace
{
	const TCHAR* WhoAmI(bool bHost) { return bHost ? TEXT("host") : TEXT("client"); }
	const TCHAR* ThrowName(int32 Kind)
	{
		return Kind == 1 ? TEXT("a stick of dynamite") : Kind == 2 ? TEXT("a knife") : Kind == 3 ? TEXT("a bottle") : TEXT("a prop");
	}
	constexpr int32 MaxVerbLogLines = 400;          // (a long session's swings and shots are counted, not logged)
}

// ---- the link's milestone 6 messages ----------------------------------------------------------------------------------

void AOutlawsCoopLink::ServerBlast_Implementation(int32 Epoch, FVector_NetQuantize10 Where, int32 Stick)
{
	if (!Admit(EOLGate::Blast, Finite(Where) && Stick >= -1)) return;
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnBlast(PlayerIndex, Epoch, Where, Stick);
}

void AOutlawsCoopLink::ServerWorldBit_Implementation(int32 Epoch, uint8 Kind, int32 Index, int32 Arg, FVector_NetQuantize100 Vec, float F)
{
	if (!Admit(EOLGate::WorldBit, Index >= 0 && Index < 1000000 && Finite(Vec) && Finite(F))) return;
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnWorldBit(PlayerIndex, Epoch, Kind, Index, Arg, Vec, F);
}

void AOutlawsCoopLink::ServerShotFx_Implementation(FVector_NetQuantize10 Muzzle, const FString& Sound)
{
	if (!Admit(EOLGate::ShotFx, Finite(Muzzle) && Sound.Len() <= 32)) return;
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnShotFx(PlayerIndex, Muzzle, Sound);
}

void AOutlawsCoopLink::ServerThrow_Implementation(int32 Epoch, uint8 Kind, FVector_NetQuantize10 Pos, FVector_NetQuantize10 Vel, float Grav, float Fuse)
{
	if (!Admit(EOLGate::Throw, Kind >= 1 && Kind <= 4 && Finite(Pos) && Finite(Vel, 1.0e5) && Finite(Grav, 100.f) && Finite(Fuse, 1000.f))) return;
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnThrow(PlayerIndex, Epoch, Kind, Pos, Vel, Grav, Fuse);
}

void AOutlawsCoopLink::ClientBlast_Implementation(int32 From, int32 Epoch, FVector_NetQuantize10 Where, int32 Stick)
{
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnBlast(From, Epoch, Where, Stick);
}

void AOutlawsCoopLink::ClientWorldBit_Implementation(int32 From, int32 Epoch, uint8 Kind, int32 Index, int32 Arg, FVector_NetQuantize100 Vec, float F)
{
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnWorldBit(From, Epoch, Kind, Index, Arg, Vec, F);
}

void AOutlawsCoopLink::ClientShotFx_Implementation(int32 From, FVector_NetQuantize10 Muzzle, const FString& Sound)
{
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnShotFx(From, Muzzle, Sound);
}

void AOutlawsCoopLink::ClientThrow_Implementation(int32 From, int32 Epoch, uint8 Kind, FVector_NetQuantize10 Pos, FVector_NetQuantize10 Vel, float Grav, float Fuse)
{
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnThrow(From, Epoch, Kind, Pos, Vel, Grav, Fuse);
}

void AOutlawsCoopLink::ServerBreakFx_Implementation(uint8 Hand, bool bOpen, uint8 Shells, FVector_NetQuantize10 Breech, FVector_NetQuantize10 Vel)
{
	// (REQ-FLICKRELOAD: a double gun holds two; a shut gun throws nothing out; REQ-RELOADSLICES: the high nibble, two going in at most)
	if (!Admit(EOLGate::BreakFx, Hand <= 1 && (Shells & 15) <= 2 && (Shells >> 4) <= 2 && (bOpen || Shells == 0) && Finite(Breech) && Finite(Vel, 1.0e4))) return;
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnBreakFx(PlayerIndex, Hand, bOpen, Shells, Breech, Vel);
}

void AOutlawsCoopLink::ClientBreakFx_Implementation(int32 From, uint8 Hand, bool bOpen, uint8 Shells, FVector_NetQuantize10 Breech, FVector_NetQuantize10 Vel)
{
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnBreakFx(From, Hand, bOpen, Shells, Breech, Vel);
}

// ---- set up at each level build ---------------------------------------------------------------------------------------

void UOutlawsCoop::SetupVerbs(AOutlawsLevelActor* L)
{
	// the props by their place in the build (a prop taken off its shelf or shattered leaves the list, so the list's own
	// index is not the same on both machines for long)
	for (int32 i = 0; i < L->Props.Num(); ++i)
	{
		L->Props[i].CoopId = i;
		L->Props[i].bCoopRemote = false;
	}
	int32 Sticks = 0;
	for (const AOutlawsLevelActor::FDynamite& D : L->Dynamite) Sticks += D.bGone ? 0 : 1;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP verbs set up: %d props, %d panes, %d explodable walls, %d sticks of dynamite, %d hanging lamps (%s)"), L->Props.Num(), L->Panes.Num(),
		L->ExplodeWalls.Num(), Sticks, L->HangModels.Num(), WhoAmI(IsHost()));
}

FString UOutlawsCoop::VerbsSummary() const
{
	return FString::Printf(TEXT("blasts sent %d, shown %d; world changes sent %d, applied %d; shots sent %d, heard %d; throws sent %d, seen %d; plate stops %d"),
		VerbBlastsSent, VerbBlastsShown, VerbBitsSent, VerbBitsApplied, VerbShotsSent, VerbShotsShown, VerbThrowsSent, VerbThrowsShown, PlateStops);
}

int32 UOutlawsCoop::StickIdOf(const AOutlawsLevelActor* L, const AActor* Owner)
{
	if (!L || !Owner || Owner == L) return -1;
	for (const AOutlawsLevelActor::FPickup& P : L->Pickups)
	{
		if (P.Actor.Get() == Owner) return P.CoopId >= 0 ? P.CoopId : -1;
	}
	return -1;
}

// ---- this machine's verbs, told to the other ---------------------------------------------------------------------------

void UOutlawsCoop::NoteBlast(AOutlawsLevelActor* L, const FVector& Where, int32 Stick)
{
	UOutlawsCoop* C = Get(L);
	if (!C || !C->bLevelBuilt || L->bRestoring) return;
	const int32 Epoch = C->LevelEpoch();
	++C->VerbBlastsSent;
	if (C->VerbLogLines++ < MaxVerbLogLines)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP blast at %s (%s)%s"), *Where.ToCompactString(), WhoAmI(C->IsHost()),
			Stick >= 0 ? *FString::Printf(TEXT(" stick %d"), Stick) : TEXT(""));
	}
	if (C->IsHost())
	{
		C->ForClients([&](AOutlawsCoopLink& CL) { CL.ClientBlast(0, Epoch, Where, Stick); });
	}
	else if (AOutlawsCoopLink* Mine = C->GetLink(C->MyIndex()))
	{
		Mine->ServerBlast(Epoch, Where, Stick);
	}
}

void UOutlawsCoop::NoteWorldBit(AOutlawsLevelActor* L, uint8 Kind, int32 Index, int32 Arg, const FVector& Vec, float F)
{
	UOutlawsCoop* C = Get(L);
	if (!C || C->bApplyingVerb || !C->bLevelBuilt || L->bRestoring || Index < 0) return;
	const int32 Epoch = C->LevelEpoch();
	++C->VerbBitsSent;
	const TCHAR* Me = WhoAmI(C->IsHost());
	if (Kind != BitSwing && C->VerbLogLines++ < MaxVerbLogLines)
	{
		switch (Kind)
		{
		case BitWall: UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP wall %d blown (%s) here, told the other machine"), Index, Me); break;
		case BitPane: UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pane %d shattered (%s) here, told the other machine"), Index, Me); break;
		case BitProp: UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP prop %d reacts: chor %d (%s) here%s"), Index, Arg, Me, F > 0.f ? TEXT(", by hand") : TEXT("")); break;
		case BitPropTaken: UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP prop %d taken off its shelf (%s) here"), Index, Me); break;
		case BitShade: UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lamp %d shade %d broken (%s) here"), Index, Arg, Me); break;
		default: break;
		}
	}
	if (C->IsHost())
	{
		C->ForClients([&](AOutlawsCoopLink& CL) { CL.ClientWorldBit(0, Epoch, Kind, Index, Arg, Vec, F); });
	}
	else if (AOutlawsCoopLink* Mine = C->GetLink(C->MyIndex()))
	{
		Mine->ServerWorldBit(Epoch, Kind, Index, Arg, Vec, F);
	}
}

void UOutlawsCoop::NoteShot(AOutlawsLevelActor* L, const FVector& Muzzle, const TCHAR* Sound)
{
	UOutlawsCoop* C = Get(L);
	if (!C || !C->bLevelBuilt) return;
	if (C->IsHost() && C->bMatchSetUp) C->Dm.NoteShot(0, FPlatformTime::Seconds());       // (M1: the host's own shot, for its accuracy)
	++C->VerbShotsSent;
	const FString Snd = Sound ? FString(Sound) : FString();
	if (C->IsHost())
	{
		C->ForClients([&](AOutlawsCoopLink& CL) { CL.ClientShotFx(0, Muzzle, Snd); });
	}
	else if (AOutlawsCoopLink* Mine = C->GetLink(C->MyIndex()))
	{
		Mine->ServerShotFx(Muzzle, Snd);
	}
}

void UOutlawsCoop::NoteThrow(AOutlawsLevelActor* L, int32 Kind, const FVector& Pos, const FVector& Vel, float Grav, float Fuse)
{
	UOutlawsCoop* C = Get(L);
	if (!C || !C->bLevelBuilt) return;
	++C->VerbThrowsSent;
	if (C->VerbLogLines++ < MaxVerbLogLines)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP throw: %s from %s at %.0f cm/s (%s), told the other machine"), ThrowName(Kind), *Pos.ToCompactString(), Vel.Size(), WhoAmI(C->IsHost()));
	}
	if (C->IsHost())
	{
		const int32 Epoch = C->LevelEpoch();
		C->ForClients([&](AOutlawsCoopLink& CL) { CL.ClientThrow(0, Epoch, uint8(Kind), Pos, Vel, Grav, Fuse); });
	}
	else if (AOutlawsCoopLink* Mine = C->GetLink(C->MyIndex()))
	{
		Mine->ServerThrow(C->LevelEpoch(), uint8(Kind), Pos, Vel, Grav, Fuse);
	}
}

void UOutlawsCoop::NoteBreak(AOutlawsLevelActor* L, int32 Hand, bool bOpen, int32 Shells, const FVector& Breech, const FVector& Vel, int32 Loads)
{
	UOutlawsCoop* C = Get(L);
	if (!C || !C->bLevelBuilt || Hand < 0 || Hand > 1) return;
	const int32 In = bOpen ? FMath::Clamp(Loads, 0, 2) : 0;
	const uint8 N = uint8(FMath::Clamp(Shells, 0, 2) | (In << 4));       // (shells out, low nibble; going in, high)
	if (C->VerbLogLines++ < MaxVerbLogLines)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP break: the %s hand's gun %s, %d shell%s out, %d going in (%s), told the others"), Hand == 0 ? TEXT("left") : TEXT("right"), bOpen ? TEXT("open") : TEXT("shut"),
			N & 15, (N & 15) == 1 ? TEXT("") : TEXT("s"), In, WhoAmI(C->IsHost()));
	}
	if (C->IsHost())
	{
		C->ForClients([&](AOutlawsCoopLink& CL) { CL.ClientBreakFx(0, uint8(Hand), bOpen, N, Breech, Vel); });
	}
	else if (AOutlawsCoopLink* Mine = C->GetLink(C->MyIndex()))
	{
		Mine->ServerBreakFx(uint8(Hand), bOpen, N, Breech, Vel);
	}
}

void UOutlawsCoop::OnBreakFx(int32 Who, uint8 Hand, bool bOpen, uint8 Packed, const FVector& Breech, const FVector& Vel)
{
	const uint8 Shells = Packed & 15, Loads = Packed >> 4;
	if (IsHost())
	{
		// the host's word on that player's barrels: its pose says so to everyone from now on (a client's own bits are not taken)
		if (AOutlawsCoopLink* K = GetLink(Who))
		{
			const uint8 Bit = uint8(1 << Hand);
			K->BreakAccepted = bOpen ? uint8(K->BreakAccepted | Bit) : uint8(K->BreakAccepted & ~Bit);
			K->Pose.Break = K->BreakAccepted;
		}
		ForClients([&](AOutlawsCoopLink& CL) { CL.ClientBreakFx(Who, Hand, bOpen, Packed, Breech, Vel); }, Who);     // (N1: passed on)
	}
	FIntVector& Seen = BreakSeen.FindOrAdd(Who);
	(bOpen ? Seen.X : Seen.Y) += 1;
	Seen.Z += Shells;
	AOutlawsLevelActor* L = FindLevel();
	if (!L || !bLevelBuilt) return;
	// heard where that player's gun is, as that player hears it (REQ-RELOADSLICES): the open slice and a shell slice per shell going
	// in, then the shut slice (cutting off any shells still sounding); no empty-chamber click (BUG-RELOADCLINK)
	const int32 RunKey = 2 + Who * 2 + Hand;          // (0 and 1: this machine's own hands)
	if (bOpen) L->PlaySoundRun(RunKey, AOutlawsLevelActor::ReloadOpenRun(TEXT("sreload"), Loads), Breech);
	else L->PlaySoundRun(RunKey, { AOutlawsLevelActor::ReloadShutSlice(TEXT("sreload")) }, Breech);
	AOutlawsRemotePlayer* R = PuppetOf(Who);
	if (R)
	{
		for (int32 i = 0; i < Shells; ++i)
		{
			const FVector Spread(FMath::FRandRange(-40.f, 40.f), FMath::FRandRange(-40.f, 40.f), FMath::FRandRange(-20.f, 30.f));
			R->AddFlyer(5, Breech, Vel + Spread, 1.f, 0.f);
		}
	}
	if (VerbLogLines++ < MaxVerbLogLines)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP break seen: the %s's %s hand gun %s, %d shell%s from %s (%s)%s"), WhoWord(Who), Hand == 0 ? TEXT("left") : TEXT("right"),
			bOpen ? TEXT("open") : TEXT("shut"), Shells, Shells == 1 ? TEXT("") : TEXT("s"), *Breech.ToCompactString(), WhoAmI(IsHost()), R ? TEXT("") : TEXT(" (no puppet here yet: not drawn)"));
	}
}

// ---- the other machine's verbs, done here -----------------------------------------------------------------------------

void UOutlawsCoop::OnBlast(int32 Who, int32 Epoch, const FVector& Where, int32 Stick)
{
	// (N1: a client's word, passed on by the host to the other clients)
	if (IsHost()) ForClients([&](AOutlawsCoopLink& CL) { CL.ClientBlast(Who, Epoch, Where, Stick); }, Who);
	RunFor(Epoch, [this, Who, Where, Stick]()
	{
		AOutlawsLevelActor* L = FindLevel();
		if (!L) return;
		const TCHAR* Me = WhoAmI(IsHost());
		const TCHAR* Other = WhoWord(Who);
		// the stick that went off there is gone here too (one that already went off here: this blast was here already)
		FString StickNote;
		if (Stick >= 0)
		{
			const int32 Pi = PickupIndex(L, Stick);
			AActor* A = Pi >= 0 ? L->Pickups[Pi].Actor.Get() : nullptr;
			int32 Di = INDEX_NONE;
			for (int32 i = 0; i < L->Dynamite.Num() && A; ++i)
			{
				const UProceduralMeshComponent* M = L->Dynamite[i].Mesh.Get();
				if (M && M->GetOwner() == A) { Di = i; break; }
			}
			if (Pi >= 0 && !A)
			{
				// (its actor is gone: it went off here as well, at the same moment)
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP blast at %s (%s) from the %s, stick %d: already went off here"), *Where.ToCompactString(), Me, Other, Stick);
				return;
			}
			if (Di != INDEX_NONE)
			{
				AOutlawsLevelActor::FDynamite& D = L->Dynamite[Di];
				D.bGone = true;
				D.bArmed = false;
				D.Mesh = nullptr;
			}
			if (A)
			{
				if (AOutlawsPlayer* Mine = LocalPlayer()) Mine->CoopLetGoOf(A);
				A->Destroy();
			}
			StickNote = FString::Printf(TEXT(", stick %d gone here too"), Stick);
		}
		for (int32 i = 0; i < MaxPlayers; ++i)
		{
			if (AOutlawsRemotePlayer* R = PuppetOf(i)) R->EndFlyersNear(Where, 200.0);
		}
		++VerbBlastsShown;
		if (VerbLogLines++ < MaxVerbLogLines)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP blast at %s (%s) from the %s%s"), *Where.ToCompactString(), Me, Other, *StickNote);
		}
		TGuardValue<bool> Remote(L->bCoopRemoteBlast, true);
		L->Explode(Where);
	});
}

void UOutlawsCoop::OnWorldBit(int32 Who, int32 Epoch, uint8 Kind, int32 Index, int32 Arg, const FVector& Vec, float F)
{
	if (IsHost()) ForClients([&](AOutlawsCoopLink& CL) { CL.ClientWorldBit(Who, Epoch, Kind, Index, Arg, Vec, F); }, Who);     // (N1: passed on)
	RunFor(Epoch, [this, Who, Kind, Index, Arg, Vec, F]()
	{
		AOutlawsLevelActor* L = FindLevel();
		if (!L) return;
		const TCHAR* Me = WhoAmI(IsHost());
		const bool bLog = Kind != BitSwing && VerbLogLines++ < MaxVerbLogLines;
		TGuardValue<bool> Applying(bApplyingVerb, true);
		++VerbBitsApplied;
		switch (Kind)
		{
		case BitWall:
			if (!L->ExplodeWalls.IsValidIndex(Index)) break;
			if (L->ExplodeWalls[Index].bBlown)
			{
				if (bLog) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP wall %d blown (%s): already down here"), Index, Me);
				break;
			}
			L->BlowExplodeWall(L->ExplodeWalls[Index], false, Vec);
			if (bLog) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP wall %d blown (%s) by the other player's blast"), Index, Me);
			break;
		case BitPane:
			if (!L->Panes.IsValidIndex(Index)) break;
			if (bLog) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pane %d shattered (%s) by the other player%s"), Index, Me, L->Panes[Index].bBroken ? TEXT(" (already broken here)") : TEXT(""));
			L->ShatterPane(Index);
			break;
		case BitProp:
		{
			const int32 i = L->Props.IndexOfByPredicate([Index](const AOutlawsLevelActor::FProp& P) { return P.CoopId == Index; });
			AOutlawsSpriteActor* S = i >= 0 ? L->Props[i].Sprite.Get() : nullptr;
			if (!S)
			{
				if (bLog) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP prop %d chor %d (%s): not here any more"), Index, Arg, Me);
				break;
			}
			AOutlawsLevelActor::FProp& P = L->Props[i];
			if (P.bActive || P.Current >= Arg || !S->Chors.IsValidIndex(Arg))
			{
				if (bLog) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP prop %d chor %d (%s): already so here (chor %d%s)"), Index, Arg, Me, P.Current, P.bActive ? TEXT(", reacting") : TEXT(""));
				break;
			}
			L->PropHitDir = Vec;
			L->PropEnter(i, Arg);
			L->Props[i].bCoopRemote = true;          // (its INF events were the other machine's to tell)
			if (bLog) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP prop %d reacts: chor %d (%s) as the other player's (%s)"), Index, Arg, Me, *L->Props[i].Name);
			break;
		}
		case BitPropTaken:
		{
			const int32 i = L->Props.IndexOfByPredicate([Index](const AOutlawsLevelActor::FProp& P) { return P.CoopId == Index; });
			if (i < 0) break;
			if (bLog) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP prop %d taken off its shelf by the other player: gone here (%s, %s)"), Index, *L->Props[i].Name, Me);
			if (AOutlawsSpriteActor* S = L->Props[i].Sprite.Get()) S->Destroy();
			L->Props.RemoveAt(i);
			break;
		}
		case BitShade:
			if (!L->HangModels.IsValidIndex(Index)) break;
			L->BreakShade(L->HangModels[Index], Arg);
			if (bLog) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lamp %d shade %d broken (%s) by the other player"), Index, Arg, Me);
			break;
		case BitSwing:
			if (L->HangModels.IsValidIndex(Index)) L->SwingHanging(L->HangModels[Index], Vec, F);
			break;
		default:
			break;
		}
	});
}

void UOutlawsCoop::OnShotFx(int32 Who, const FVector& Muzzle, const FString& Sound)
{
	if (IsHost() && bMatchSetUp && Who > 0 && Who < MaxPlayers) Dm.NoteShot(Who, FPlatformTime::Seconds());     // (M1: a client's shot)
	if (IsHost()) ForClients([&](AOutlawsCoopLink& CL) { CL.ClientShotFx(Who, Muzzle, Sound); }, Who);     // (N1: passed on)
	AOutlawsLevelActor* L = FindLevel();
	if (!L || !bLevelBuilt) return;
	if (TestOnly == TEXT("remotefx")) RfHeard.Add(Who, Sound);      // (the test's: what this machine was told to play)
	if (!Sound.IsEmpty()) L->PlaySoundAt(Sound, Muzzle);
	L->MuzzleFlash(Muzzle);
	bool bFlash3D = false;
	if (AOutlawsRemotePlayer* R = PuppetOf(Who))
	{
		R->NoteShot();      // (the shooter's sprite shows the shot: its shoot chor)
		// (REQ-MUZZLEFLASH3D) the 3D flash at that player's (or bot's) muzzle, its gun's kind by the sound
		if (AOutlawsLevelActor::IsGunSound(Sound)) bFlash3D = L->MuzzleFlash3D(Muzzle, R->ShotDirFrom(Muzzle), AOutlawsLevelActor::FlashKindOfSound(Sound));
	}
	if (TestOnly == TEXT("remotefx") && bFlash3D) RfFlash3D.Add(Who, AOutlawsLevelActor::FlashKindName(AOutlawsLevelActor::FlashKindOfSound(Sound)));
	if (VerbShotsShown++ < 20)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP shot seen: %s at %s (%s), the %s's%s"), *Sound, *Muzzle.ToCompactString(), WhoAmI(IsHost()), WhoWord(Who),
			bFlash3D ? TEXT(", 3D flash") : TEXT(""));
	}
}

void UOutlawsCoop::OnThrow(int32 Who, int32 Epoch, uint8 Kind, const FVector& Pos, const FVector& Vel, float Grav, float Fuse)
{
	if (IsHost()) ForClients([&](AOutlawsCoopLink& CL) { CL.ClientThrow(Who, Epoch, Kind, Pos, Vel, Grav, Fuse); }, Who);     // (N1: passed on)
	RunFor(Epoch, [this, Who, Kind, Pos, Vel, Grav, Fuse]()
	{
		AOutlawsRemotePlayer* R = PuppetOf(Who);
		if (R)
		{
			R->AddFlyer(Kind, Pos, Vel, Grav, Fuse);
			R->NoteThrow(Kind);                  // (the thrower's sprite: the throwing arm out a moment)
		}
		++VerbThrowsShown;
		if (VerbLogLines++ < MaxVerbLogLines)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP throw seen: %s from %s at %.0f cm/s (%s), the %s's%s"), ThrowName(Kind), *Pos.ToCompactString(), Vel.Size(), WhoAmI(IsHost()),
				WhoWord(Who), R ? TEXT("") : TEXT(" (no puppet here yet: not drawn)"));
		}
	});
}

// ---- the client's held plate --------------------------------------------------------------------------------------------

bool UOutlawsCoop::PlateTakesHurt(AOutlawsLevelActor* L, const FVector& From, float& Damage)
{
	// a host's bad guy's shot, its line from the bad guy's gun (a little over its middle) through this player's chest: the
	// plate held up in either hand meets it as it would here in single player (worn, struck, broken)
	AOutlawsPlayer* P = LocalPlayer();
	if (!P || !L) return false;
	const float Half = P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const FVector Gun = From + FVector(0.0, 0.0, 20.0);
	const FVector Chest = P->GetActorLocation() + FVector(0.0, 0.0, Half * 0.4);
	FVector To = Chest + (Chest - Gun).GetSafeNormal() * 40.0;
	float Left = Damage;
	const int32 Before = L->GetArmour();
	if (P->PlateStops(Gun, To, Left))
	{
		++PlateStops;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP plate stopped a shot (%s): %.2f, armour %d -> %d"), WhoAmI(IsHost()), Damage, Before, L->GetArmour());
		return true;
	}
	if (Left < Damage)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP plate broke on a shot (%s): %.2f of %.2f goes on"), WhoAmI(IsHost()), Left, Damage);
		Damage = Left;
	}
	return false;
}

// ---- tests (-OLCoopAuto -OLCoopTest=verbs, on a level with walls, glass and sticks: the wharf) --------------------------

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TestLogVerbs(AOutlawsLevelActor* L, const TCHAR* Why)
{
	TArray<FString> Walls, Panes, Moved;
	for (int32 i = 0; i < L->ExplodeWalls.Num(); ++i) if (L->ExplodeWalls[i].bBlown) Walls.Add(FString::FromInt(i));
	for (int32 i = 0; i < L->Panes.Num(); ++i) if (L->Panes[i].bBroken) Panes.Add(FString::FromInt(i));
	int32 Sticks = 0;
	for (const AOutlawsLevelActor::FDynamite& D : L->Dynamite) Sticks += D.bGone ? 0 : 1;
	for (const AOutlawsLevelActor::FProp& Pr : L->Props) if (Pr.Current != 0) Moved.Add(FString::Printf(TEXT("%d=%d"), Pr.CoopId, Pr.Current));
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: verbs %s (%s): walls blown [%s], panes broken [%s], sticks left %d, props %d, props off rest [%s]"), Why, WhoAmI(IsHost()),
		*FString::Join(Walls, TEXT(",")), *FString::Join(Panes, TEXT(",")), Sticks, L->Props.Num(), *FString::Join(Moved, TEXT(",")));
}

void UOutlawsCoop::TickHostVerbsTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	if (Now < TestAt) return;
	static const TCHAR* const Labels[] = { TEXT("after the walls"), TEXT("after the panes and props"), TEXT("after the sticks"), TEXT("at the end") };
	const double R = L->BlastRadius * FOutlawsGeometry::UnitsToCm, WallR = L->BlastWallRadius * FOutlawsGeometry::UnitsToCm;
	const FVector Me = P->GetActorLocation();
	const AOutlawsCoopLink* CL = GetLink(1);
	const FVector Them = CL ? FVector(CL->Pose.Body) : Me;
	auto Nearest = [&](const FVector& At) { return FMath::Min(FVector::Dist(At, Me), FVector::Dist(At, Them)); };
	// a stick lying in the level (a pickup with a number), the farthest from both players that is clear of Avoid
	auto PickStick = [&](const FVector* Avoid, int32& OutId) -> int32
	{
		int32 Best = INDEX_NONE;
		double BestD = 0.0;
		for (int32 i = 0; i < L->Dynamite.Num(); ++i)
		{
			const AOutlawsLevelActor::FDynamite& D = L->Dynamite[i];
			const UProceduralMeshComponent* M = D.Mesh.Get();
			if (D.bGone || D.bArmed || !M) continue;
			const int32 Id = StickIdOf(L, M->GetOwner());
			const FVector At = M->GetComponentLocation();
			if (Id < 0 || (Avoid && FVector::Dist(At, *Avoid) < R * 1.4)) continue;
			const double Dd = Nearest(At);
			if (Dd > BestD) { BestD = Dd; Best = i; OutId = Id; }
		}
		return Best;
	};
	switch (TestStep)
	{
	case 110:
	{
		// both out of harm's way (the blasts are far off, the bad guys are not); the two walls the farthest from both
		// players, apart enough that one blast leaves the other standing
		L->bInvincible = true;
		TestSay(TEXT("godmode"), 1);
		TArray<TPair<double, int32>> Order;
		for (int32 i = 0; i < L->ExplodeWalls.Num(); ++i)
		{
			if (!L->ExplodeWalls[i].bBlown && L->ExplodeWalls[i].Box.IsValid) Order.Emplace(Nearest(L->ExplodeWalls[i].Box.GetCenter()), i);
		}
		Order.Sort([](const TPair<double, int32>& A, const TPair<double, int32>& B) { return A.Key > B.Key; });
		TestWallA = TestWallB = -1;
		for (const TPair<double, int32>& O : Order)
		{
			if (TestWallA < 0) { TestWallA = O.Value; continue; }
			const FBox& BoxB = L->ExplodeWalls[O.Value].Box;
			const FVector CentreA = L->ExplodeWalls[TestWallA].Box.GetCenter();
			if (BoxB.ComputeSquaredDistanceToPoint(CentreA) > FMath::Square(WallR + 150.0)
				&& L->ExplodeWalls[TestWallA].Box.ComputeSquaredDistanceToPoint(BoxB.GetCenter()) > FMath::Square(WallR + 150.0))
			{
				TestWallB = O.Value;
				break;
			}
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: verbs on %s: %d walls, %d panes, %d props, %d sticks; the host blasts wall %d (%.0f cm from the nearer player), the client wall %d (%.0f cm)"),
			*BuiltLevel, L->ExplodeWalls.Num(), L->Panes.Num(), L->Props.Num(), L->Dynamite.Num(), TestWallA, TestWallA >= 0 ? Nearest(L->ExplodeWalls[TestWallA].Box.GetCenter()) : -1.0,
			TestWallB, TestWallB >= 0 ? Nearest(L->ExplodeWalls[TestWallB].Box.GetCenter()) : -1.0);
		TestLogVerbs(L, TEXT("before"));
		TestSay(TEXT("v:log"), -1);
		TestStep = 111;
		TestAt = Now + 1.5;
		return;
	}
	case 111:
		// 1: a blast at a wall on the host: the wall comes down on both
		if (TestWallA >= 0)
		{
			const FVector At = L->ExplodeWalls[TestWallA].Box.GetCenter();
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host sets off a blast at wall %d (%s)"), TestWallA, *At.ToCompactString());
			L->Explode(At);
		}
		TestStep = 112;
		TestAt = Now + 2.5;
		return;
	case 112:
		// 2: the other way round: a blast at the second wall on the client
		if (TestWallB >= 0) TestSay(TEXT("v:blastwall"), TestWallB);
		TestStep = 113;
		TestAt = Now + 3.0;
		return;
	case 113:
		TestLogVerbs(L, Labels[0]);
		TestSay(TEXT("v:log"), 0);
		TestStep = 114;
		TestAt = Now + 1.0;
		return;
	case 114:
	{
		// 3: glass: the host breaks one pane, the client another; 4: props: the host's shot at one, the client's at another
		int32 PaneA = -1, PaneB = -1;
		for (int32 i = 0; i < L->Panes.Num(); ++i)
		{
			if (L->Panes[i].bBroken) continue;
			if (PaneA < 0) PaneA = i;
			else { PaneB = i; break; }
		}
		if (PaneA >= 0)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host breaks pane %d"), PaneA);
			L->ShatterPane(PaneA);
		}
		if (PaneB >= 0) TestSay(TEXT("v:pane"), PaneB);
		int32 PropA = -1, PropB = -1;
		for (const AOutlawsLevelActor::FProp& Pr : L->Props)
		{
			const AOutlawsSpriteActor* S = Pr.Sprite.Get();
			if (!S || !Pr.bShoot || Pr.bActive || Pr.Current != 0 || Pr.CoopId < 0 || S->Chors.Num() < 2 || Pr.bGrabOnly) continue;
			if (PropA < 0) PropA = Pr.CoopId;
			else { PropB = Pr.CoopId; break; }
		}
		const int32 Ia = L->Props.IndexOfByPredicate([PropA](const AOutlawsLevelActor::FProp& Pr) { return Pr.CoopId == PropA; });
		if (Ia >= 0)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host shoots prop %d (%s)"), PropA, *L->Props[Ia].Name);
			L->PropHitDir = FVector::ForwardVector;
			L->PropReact(Ia, false);
		}
		if (PropB >= 0) TestSay(TEXT("v:prop"), PropB);
		TestStep = 115;
		TestAt = Now + 2.5;
		return;
	}
	case 115:
		TestLogVerbs(L, Labels[1]);
		TestSay(TEXT("v:log"), 1);
		TestStep = 116;
		TestAt = Now + 1.0;
		return;
	case 116:
	{
		// 5: a stick of dynamite lying in the level: the host's goes off (gone on the client too), then one of the client's
		int32 Id = -1;
		const int32 Di = PickStick(nullptr, Id);
		if (Di != INDEX_NONE)
		{
			TestPickupHome = L->Dynamite[Di].Mesh->GetComponentLocation();
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host sets off stick %d at %s"), Id, *TestPickupHome.ToCompactString());
			L->Detonate(Di);
		}
		else
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: no stick for the host to set off"));
		}
		TestStep = 117;
		TestAt = Now + 3.0;
		return;
	}
	case 117:
	{
		int32 Id = -1;
		const int32 Di = PickStick(&TestPickupHome, Id);
		if (Di != INDEX_NONE)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client is to set off stick %d at %s"), Id, *L->Dynamite[Di].Mesh->GetComponentLocation().ToCompactString());
			TestSay(TEXT("v:stick"), Id);
		}
		TestStep = 118;
		TestAt = Now + 3.5;
		return;
	}
	case 118:
		TestLogVerbs(L, Labels[2]);
		TestSay(TEXT("v:log"), 2);
		// 6: a gunshot each: heard and seen at the other's muzzle
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host fires its revolver"));
		P->CoopTestFire();
		TestSay(TEXT("v:fire"), 0);
		TestStep = 119;
		TestAt = Now + 2.0;
		return;
	case 119:
		// 7: a throw each: the host's knife, the client's unlit stick, each flying on the other machine
		L->GiveAllWeapons();
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host throws a knife"));
		P->CoopTestThrow(7);
		TestSay(TEXT("v:throw"), 6);
		TestStep = 120;
		TestAt = Now + 3.0;
		return;
	case 120:
		// 8: a host's bad guy's shot from here goes on to the client (REQ-CHESTPLATE: no plate is held up any more)
		TestSay(TEXT("godmode"), 0);
		TestStep = 123;
		TestAt = Now + 1.5;
		return;
	case 123:
	{
		const FVector From = P->GetActorLocation();
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: enemy 0's shot at the client from the host's place"));
		HurtOther(1.f, 1.f, false, &From, 0);
		TestStep = 124;
		TestAt = Now + 1.5;
		return;
	}
	case 124:
		TestSay(TEXT("godmode"), 1);
		TestLogVerbs(L, Labels[3]);
		TestSay(TEXT("v:log"), 3);
		TestStep = 125;
		TestAt = Now + 1.5;
		return;
	case 125:
		TestSay(TEXT("quit"), 0);
		TestStep = 100;
		TestAt = Now + 3.0;
		return;
	default:
		return;
	}
}

void UOutlawsCoop::OnVerbsTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	static const TCHAR* const Labels[] = { TEXT("after the walls"), TEXT("after the panes and props"), TEXT("after the sticks"), TEXT("at the end") };
	if (Step == TEXT("log"))
	{
		TestLogVerbs(L, Arg >= 0 && Arg < 4 ? Labels[Arg] : TEXT("before"));
	}
	else if (Step == TEXT("blastwall") && L->ExplodeWalls.IsValidIndex(Arg))
	{
		const FVector At = L->ExplodeWalls[Arg].Box.GetCenter();
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client sets off a blast at wall %d (%s)"), Arg, *At.ToCompactString());
		L->Explode(At);
	}
	else if (Step == TEXT("pane") && L->Panes.IsValidIndex(Arg))
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client breaks pane %d"), Arg);
		L->ShatterPane(Arg);
	}
	else if (Step == TEXT("prop"))
	{
		const int32 i = L->Props.IndexOfByPredicate([Arg](const AOutlawsLevelActor::FProp& Pr) { return Pr.CoopId == Arg; });
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client shoots prop %d (%s)"), Arg, i >= 0 ? *L->Props[i].Name : TEXT("not here"));
		if (i >= 0)
		{
			L->PropHitDir = FVector::ForwardVector;
			L->PropReact(i, false);
		}
	}
	else if (Step == TEXT("stick"))
	{
		int32 Di = INDEX_NONE;
		for (int32 i = 0; i < L->Dynamite.Num(); ++i)
		{
			const UProceduralMeshComponent* M = L->Dynamite[i].Mesh.Get();
			if (!L->Dynamite[i].bGone && M && StickIdOf(L, M->GetOwner()) == Arg) { Di = i; break; }
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client sets off stick %d (%s)"), Arg, Di != INDEX_NONE ? TEXT("here") : TEXT("NOT HERE"));
		if (Di != INDEX_NONE) L->Detonate(Di);
	}
	else if (Step == TEXT("fire"))
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client fires its revolver"));
		P->CoopTestFire();
	}
	else if (Step == TEXT("throw"))
	{
		L->GiveAllWeapons();
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client throws %s"), Arg == 6 ? TEXT("an unlit stick") : TEXT("a knife"));
		P->CoopTestThrow(Arg);
	}
	// (REQ-CHESTPLATE: the held plate's step is gone, the plate is worn; -OLChestNetTest checks how others see it)
}
#endif // WITH_OUTLAWS_TESTS

// ---- -OLCoopAuto -OLCoopTest=lanternswim (REQ-LANTERNSWIM) ------------------------------------------------------------------
// The host first, then the client once it has heard the host's lantern go: a lit lantern taken in the right hand, carried
// into the level's deepest water and brought out again.  Each machine logs the other's hands as its poses carry them (the
// pose's tool names, CoopHeld) whenever they change; the host's last look is at the client's.

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TickLanternSwimTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	const bool bHost = IsHost();
	const TCHAR* Me = bHost ? TEXT("host") : TEXT("client");
	const TCHAR* Them = bHost ? TEXT("client") : TEXT("host");
	static FVector Dry = FVector::ZeroVector;
	L->bInvincible = true;
	auto Check = [this, Me](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lanternswim check %s (%s): %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), Me, *What);
		if (!bOk) ++LanternTestFails;
	};
	auto Hands = [P]()
	{
		int32 W = -1;
		FString T0, T1;
		P->CoopHeld(0, W, T0);
		P->CoopHeld(1, W, T1);
		return FString::Printf(TEXT("L '%s' R '%s'"), *T0, *T1);
	};
	if (const AOutlawsCoopLink* PL = GetLink(PartnerIndex()); PL && PL->Pose.Seq > 0)
	{
		const FString Seen = FString::Printf(TEXT("L '%s' R '%s'"), *PL->Pose.LeftTool, *PL->Pose.RightTool);
		if (Seen != LanternTestSeen)
		{
			LanternTestSeen = Seen;
			const bool bHeld = PL->Pose.LeftTool == TEXT("ilantern") || PL->Pose.RightTool == TEXT("ilantern");
			if (bHeld) bLanternTestSawHeld = true;
			else if (bLanternTestSawHeld) bLanternTestSawGone = true;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lanternswim: the %s hears the %s's hands: %s%s"), Me, Them, *Seen,
				bHeld ? TEXT(" (the lantern in hand)") : bLanternTestSawGone ? TEXT(" (the lantern gone)") : TEXT(""));
		}
	}
	switch (LanternTestStep)
	{
	case 0:
		if (bHost)
		{
			const AOutlawsCoopLink* CL = GetLink(PartnerIndex());
			if (!CL || CL->BuiltSerial != StartedSerial) return;
		}
		else
		{
			if (!bLanternTestSawGone) return;
			Check(bLanternTestSawHeld && bLanternTestSawGone, TEXT("the host's lantern heard in hand, then gone"));
			bLanternTestSawHeld = bLanternTestSawGone = false;
		}
		LanternTestAt = Now + 3.0;
		LanternTestStep = 1;
		return;
	case 1:
		if (Now < LanternTestAt) return;
		if (L->GetCount(TEXT("ilantern")) <= 0) L->AddCount(TEXT("ilantern"), 1);
		L->AddCount(TEXT("ioil"), 150);
		P->CoopTestTool(1, TEXT("ilantern"));
		if (!L->bLampOn) L->ToggleLamp();
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lanternswim: the %s takes a lit lantern: %s"), Me, *Hands());
		LanternTestAt = Now + 3.0;
		LanternTestStep = 2;
		return;
	case 2:
	{
		if (Now < LanternTestAt) return;
		const int32 Sec = L->DebugWaterSector();
		FVector Feet;
		float SlopeYaw = 0.f;
		if (Sec < 0 || !L->SlopeFoot(Sec, Feet, SlopeYaw))
		{
			Check(false, TEXT("a water sector in this level"));
			LanternTestAt = Now;
			LanternTestStep = 4;
			return;
		}
		Dry = P->GetActorLocation();
		P->TeleportTo(Feet + FVector(0.0, 0.0, P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0), P->GetActorRotation(), false, true);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lanternswim: the %s goes into water sector %d"), Me, Sec);
		LanternTestAt = Now + 3.0;
		LanternTestStep = 3;
		return;
	}
	case 3:
	{
		if (Now < LanternTestAt) return;
		const FString H = Hands();
		Check(P->IsSwimming() && !H.Contains(TEXT("ilantern")), FString::Printf(TEXT("in the water: swimming %d, hands %s"), P->IsSwimming() ? 1 : 0, *H));
		P->TeleportTo(Dry, P->GetActorRotation(), false, true);
		LanternTestAt = Now + 2.0;
		LanternTestStep = 4;
		return;
	}
	case 4:
		if (Now < LanternTestAt) return;
		if (!bHost)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lanternswim: the client's turn done: %s (%d failed)"), *Hands(), LanternTestFails);
			LanternTestStep = 5;
			return;
		}
		// the host: the client's turn next (it starts on hearing the host's lantern go); its lantern heard in hand, then gone
		LanternTestAt = Now + 40.0;
		LanternTestStep = 6;
		return;
	case 6:
		if (!bLanternTestSawGone && Now < LanternTestAt) return;
		Check(bLanternTestSawHeld && bLanternTestSawGone, TEXT("the client's lantern heard in hand, then gone"));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lanternswim RESULT %s (the host's checks, %d failed; the client's in its own log)"), LanternTestFails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), LanternTestFails);
		TestSay(TEXT("quit"), 0);
		LanternTestAt = Now + 2.0;
		LanternTestStep = 7;
		return;
	case 7:
		if (Now < LanternTestAt) return;
		Summary(TEXT("done"));
		QuitForTest();
		LanternTestStep = 8;
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
