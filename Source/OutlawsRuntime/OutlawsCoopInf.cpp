// Story co-op (tools/olue/COOP_DESIGN.md), milestone 5: the INF and the level's flow.  The host's INF decides for both
// players: doors, lifts, switches, triggers and their timing run there only.  The client's INF shows the host's (every
// elevator's value, stop, move and switch, the triggers' arming, about 30 times a second with only what changed sent) and
// eases a move on between two words; its player's own events (a sector entered or left, a line crossed, a use, a hand
// push, a shot or a blast on a scripted surface) go to the host with that player's keys, and the host runs them as its
// own player's.  The sounds, messages, locks and wall textures the host's INF sets off are told to the client.  The level
// end is the host's: both machines run the fade and the movie (either player's skip ends it for both), then the host's
// start serial builds the next level on both, each player keeping what they carry.
//
// None of this runs in single player: the INF runtime's co-op hooks stay unset, and the level actor calls in only when
// UOutlawsCoop::IsActive.
#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsUnlocks.h"
#include "OutlawsPlayer.h"
#include "OutlawsInf.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsGeometry.h"
#include "OutlawsMovieScreen.h"
#include "Components/AudioComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"

namespace
{
	constexpr int32 InfCauserEnemy = 100;              // (an enemy's door push; N1: past every player's number)

	const TCHAR* InfWho(int32 Who)
	{
		return Who == InfCauserEnemy ? TEXT("an enemy's") : Who >= 0 && Who < UOutlawsCoop::MaxPlayers ? UOutlawsCoop::WhoWord(Who) : TEXT("level's own");
	}

	/** A client player's event (N1: any of them), not the host's, an enemy's or the level's own. */
	bool IsClientCauser(int32 Who) { return Who >= 1 && Who < UOutlawsCoop::MaxPlayers; }

	const TCHAR* InfEventWord(uint32 Event)
	{
		using namespace OutlawsInf;
		if (Event & (EvNudgeFront | EvNudgeBack)) return TEXT("use");
		if (Event & (EvCrossFront | EvCrossBack)) return TEXT("line crossed");
		if (Event & EvEnter) return TEXT("enter");
		if (Event & EvLeave) return TEXT("leave");
		if (Event & EvShoot) return TEXT("shot");
		return TEXT("blast or landing");
	}

	constexpr double InfSnapInterval = 1.0 / 30.0;     // the host's INF out about 30 times a second (sent at the session's rate)
	constexpr int32 InfMoveLogCap = 400;               // elevator moves logged per level (a timed lift goes on for ever)
	constexpr int32 InfEventLogCap = 60;               // sector and line events (not uses) logged per level
}

// ---- the link's milestone 5 messages ----------------------------------------------------------------------------------

void AOutlawsCoopLink::ServerInfEvent_Implementation(int32 Epoch, int32 Sector, int32 Wall, int32 Event, const TArray<int32>& Items)
{
	if (!Admit(EOLGate::InfEvent, Items.Num() <= 64 && Sector >= -1 && Sector < 100000 && Wall >= -1 && Wall < 1000000)) return;
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnInfEvent(PlayerIndex, Epoch, Sector, Wall, Event, Items);
}

void AOutlawsCoopLink::ServerMovieSkip_Implementation(int32 Epoch)
{
	if (!Admit(EOLGate::MovieSkip, true)) return;
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnMovieSkip(PlayerIndex, Epoch);
}

void AOutlawsCoopLink::ClientInfSound_Implementation(int32 Epoch, const FString& Wav, FVector_NetQuantize10 At, int32 Loop, uint8 Mode)
{
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnInfSound(Epoch, Wav, At, Loop, Mode);
}

void AOutlawsCoopLink::ClientInfNews_Implementation(int32 Epoch, uint8 Kind, int32 A, int32 B, int32 C, int32 D)
{
	if (UOutlawsCoop* Co = UOutlawsCoop::Get(this)) Co->OnInfNews(Epoch, Kind, A, B, C, D);
}

void AOutlawsCoopLink::ClientLevelWon_Implementation(int32 Epoch, const FString& Next, const FString& Movie, const FString& Why)
{
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnLevelWonByHost(Epoch, Next, Movie, Why);
}

// ---- setting the level's INF up for the session ------------------------------------------------------------------------

FOutlawsCoopInfSnap UOutlawsCoop::InfSnapOf(const FOutlawsElevator& E)
{
	FOutlawsCoopInfSnap S;
	S.Value = float(E.Current);
	S.Stop = int16(FMath::Clamp(E.Stop, -32768, 32767));
	S.Target = int16(FMath::Clamp(E.Target, -32768, 32767));
	S.Flags = uint8((E.bWaiting ? 1 : 0) | (E.bTerminated ? 2 : 0) | (E.bMasterOn ? 4 : 0));
	return S;
}

void UOutlawsCoop::SetupInf(AOutlawsLevelActor* L)
{
	if (!L || !L->Inf) return;
	FOutlawsInfRuntime& Inf = *L->Inf;
	InfSeen.Reset();
	for (const FOutlawsElevator& E : Inf.Elevators) InfSeen.Add(InfSnapOf(E));
	InfTriggersSeen.Reset();
	InfLogLines = 0;
	InfMovesThisLevel = 0;
	InfCauser = -1;
	InfScopeDepth = 0;
	bInfCauserFromScope = false;
	NextInfSnapAt = 0.0;
	TWeakObjectPtr<UOutlawsCoop> WeakThis(this);
	TWeakObjectPtr<AOutlawsLevelActor> WeakL(L);
	if (IsClient())
	{
		// the client's INF shows the host's: its player's events go there, with its keys
		Inf.OnForward = [WeakThis, WeakL](int32 Sector, int32 Wall, uint32 Event)
		{
			UOutlawsCoop* C = WeakThis.Get();
			AOutlawsLevelActor* Lv = WeakL.Get();
			AOutlawsCoopLink* Mine = C ? C->GetLink(C->MyIndex()) : nullptr;
			if (!Mine || !Lv || !Lv->Inf || C->LevelEpoch() < 0) return;
			Mine->ServerInfEvent(C->LevelEpoch(), Sector, Wall, int32(Event), Lv->Inf->Items.Array());
			++C->InfEventsSent;
			const bool bUse = (Event & (OutlawsInf::EvNudgeFront | OutlawsInf::EvNudgeBack)) != 0;
			if (bUse || C->InfLogLines++ < InfEventLogCap)
			{
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP inf: %s by the %s player on sector %d%s, sent to the host (keys %d)"), InfEventWord(Event), InfWho(C->MyIndex()), Sector,
					Wall >= 0 ? *FString::Printf(TEXT(" wall %d"), Wall) : TEXT(""), Lv->Inf->Items.Num());
			}
		};
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP inf set up: %d elevators, %d triggers, shown as the host's (this player's events go to the host)"), Inf.Elevators.Num(), Inf.Triggers.Num());
		return;
	}
	if (!IsHost()) return;
	// the host's: whose event it runs (for the logs, the locks and who sees a message) ...
	Inf.OnEventScope = [WeakThis](int32 Sector, int32 Wall, uint32 Event, int32 Phase)
	{
		UOutlawsCoop* C = WeakThis.Get();
		if (!C) return;
		if (Phase == 0)
		{
			if (C->InfScopeDepth++ == 0 && C->InfCauser < 0)
			{
				C->InfCauser = 0;                     // (sent in on this machine by nobody else: the host's own player)
				C->bInfCauserFromScope = true;
			}
			return;
		}
		if (--C->InfScopeDepth > 0) return;
		C->InfScopeDepth = 0;
		const bool bUse = (Event & (OutlawsInf::EvNudgeFront | OutlawsInf::EvNudgeBack)) != 0;
		if (C->InfCauser != InfCauserEnemy && ((bUse) || (Phase == 2 && C->InfLogLines++ < InfEventLogCap)))
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP inf: %s by the %s player on sector %d%s (%s)"), InfEventWord(Event), InfWho(C->InfCauser), Sector,
				Wall >= 0 ? *FString::Printf(TEXT(" wall %d"), Wall) : TEXT(""), Phase == 2 ? TEXT("taken") : TEXT("nothing took it"));
		}
		if (C->bInfCauserFromScope)
		{
			C->InfCauser = -1;
			C->bInfCauserFromScope = false;
		}
	};
	// ... and what it sets off goes to the client too: the sounds (once, the loops, the locks), the messages (to the player
	// who caused them; the level's own to both), an item spent in a lock (from that player's pockets), the walls' textures
	const TFunction<void(const FString&, const FOutlawsElevator&)> Sound = Inf.OnSound;
	Inf.OnSound = [WeakThis, WeakL, Sound](const FString& Wav, const FOutlawsElevator& E)
	{
		if (Sound) Sound(Wav, E);
		UOutlawsCoop* C = WeakThis.Get();
		AOutlawsLevelActor* Lv = WeakL.Get();
		if (!C || !Lv) return;
		const int32 Epoch = C->LevelEpoch();
		const FVector At = Lv->ElevatorLocation(E);
		C->ForClients([&](AOutlawsCoopLink& CL) { CL.ClientInfSound(Epoch, Wav, At, -1, 0); ++C->InfSoundsSent; });
	};
	const TFunction<void(const FString&, const FOutlawsElevator&, bool)> Loop = Inf.OnLoopSound;
	Inf.OnLoopSound = [WeakThis, WeakL, Loop](const FString& Wav, const FOutlawsElevator& E, bool bStart)
	{
		if (Loop) Loop(Wav, E, bStart);
		UOutlawsCoop* C = WeakThis.Get();
		AOutlawsLevelActor* Lv = WeakL.Get();
		if (!C || !Lv || !Lv->Inf) return;
		const int32 Epoch = C->LevelEpoch();
		const FVector At = Lv->ElevatorLocation(E);
		const int32 Index = int32(&E - Lv->Inf->Elevators.GetData());
		C->ForClients([&](AOutlawsCoopLink& CL) { CL.ClientInfSound(Epoch, Wav, At, Index, bStart ? 1 : 2); ++C->InfSoundsSent; });
	};
	const TFunction<void(int32, bool, const FOutlawsElevator&)> Key = Inf.OnKeyCheck;
	Inf.OnKeyCheck = [WeakThis, WeakL, Key](int32 UseId, bool bHasIt, const FOutlawsElevator& E)
	{
		if (Key) Key(UseId, bHasIt, E);           // (the lock's own sound here: "unlock", or the rattle)
		UOutlawsCoop* C = WeakThis.Get();
		AOutlawsLevelActor* Lv = WeakL.Get();
		if (!C || !Lv) return;
		const int32 Who = C->InfCauser;
		if (bHasIt)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP inf: key opens %s for the %s player (item %d)"), *E.Name, InfWho(Who), UseId);
		}
		else
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP inf: key refused for the %s player (item %d, %s)"), InfWho(Who), UseId, *E.Name);
		}
		const int32 Epoch = C->LevelEpoch();
		if (const AOutlawsLevelActor::FItemInfo* Info = Lv->ItemsByUseId.Find(UseId))
		{
			const FString& Wav = bHasIt ? Info->UseOk : Info->UseFail;
			const FVector At = Lv->ElevatorLocation(E);
			if (!Wav.IsEmpty()) C->ForClients([&](AOutlawsCoopLink& CL) { CL.ClientInfSound(Epoch, Wav, At, -1, 0); });
		}
		if (IsClientCauser(Who))
		{
			if (AOutlawsCoopLink* CL = C->GetLink(Who)) CL->ClientInfNews(Epoch, 1, UseId, bHasIt ? 1 : 0, 0, 0);
		}
	};
	const TFunction<void(int32)> UsedUp = Inf.OnItemUsedUp;
	Inf.OnItemUsedUp = [WeakThis, UsedUp](int32 UseId)
	{
		UOutlawsCoop* C = WeakThis.Get();
		if (C && IsClientCauser(C->InfCauser))
		{
			// the client's item went into the lock: out of the client's pockets (this machine's player keeps theirs)
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP inf: the %s's item %d goes into the lock"), InfWho(C->InfCauser), UseId);
			if (AOutlawsCoopLink* CL = C->GetLink(C->InfCauser)) CL->ClientInfNews(C->LevelEpoch(), 2, UseId, 0, 0, 0);
			return;
		}
		if (UsedUp) UsedUp(UseId);
	};
	const TFunction<void(int32)> Message = Inf.OnUserMessage;
	Inf.OnUserMessage = [WeakThis, Message](int32 N)
	{
		UOutlawsCoop* C = WeakThis.Get();
		const int32 Who = C ? C->InfCauser : -1;
		if (!IsClientCauser(Who) && Message) Message(N);         // (the host's own, or the level's: shown here)
		// (the client's own: shown there alone; the level's: on every client; N1)
		if (C && IsClientCauser(Who))
		{
			if (AOutlawsCoopLink* CL = C->GetLink(Who)) CL->ClientInfNews(C->LevelEpoch(), 0, N, 0, 0, 0);
		}
		else if (C && Who != 0)
		{
			const int32 Epoch = C->LevelEpoch();
			C->ForClients([&](AOutlawsCoopLink& CL) { CL.ClientInfNews(Epoch, 0, N, 0, 0, 0); });
		}
		if (C) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP inf: message %d for %s"), N, Who == 0 ? TEXT("the host") : Who == 1 ? TEXT("the client") : IsClientCauser(Who) ? InfWho(Who) : TEXT("both"));
	};
	const TFunction<void(int32, int32, const FOutlawsInfMessage&)> WallMsg = Inf.OnWallMessage;
	Inf.OnWallMessage = [WeakThis, WallMsg](int32 S, int32 W, const FOutlawsInfMessage& M)
	{
		if (WallMsg) WallMsg(S, W, M);
		UOutlawsCoop* C = WeakThis.Get();
		if (!C) return;
		const int32 Epoch = C->LevelEpoch();
		C->ForClients([&](AOutlawsCoopLink& CL) { CL.ClientInfNews(Epoch, 3, S, W, int32(M.Type), M.Arg); });
	};
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP inf set up: %d elevators, %d triggers, deciding here for both players"), Inf.Elevators.Num(), Inf.Triggers.Num());
}

// ---- each frame: the host's INF out, the client's in ----------------------------------------------------------------------

void UOutlawsCoop::LogInfMove(const AOutlawsLevelActor* L, int32 Index, const FOutlawsCoopInfSnap& Was, const FOutlawsCoopInfSnap& Now)
{
	++InfMovesLogged;
	if (++InfMovesThisLevel > InfMoveLogCap)
	{
		if (InfMovesThisLevel == InfMoveLogCap + 1) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP inf: more than %d elevator moves in this level; no more logged"), InfMoveLogCap);
		return;
	}
	const FOutlawsElevator* E = L && L->Inf && L->Inf->Elevators.IsValidIndex(Index) ? &L->Inf->Elevators[Index] : nullptr;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP inf: elevator %d at %.2f target %d (%s) [%s, stop %d%s]"), Index, Now.Value, Now.Target, IsHost() ? TEXT("host") : TEXT("client"),
		E ? *E->Name : TEXT("?"), Now.Stop, (Now.Flags & 4) == 0 ? TEXT(", switched off") : (Now.Flags & 2) ? TEXT(", finished") : (Now.Flags & 1) ? TEXT(", waiting") : TEXT(""));
}

void UOutlawsCoop::TickInf(double Now)
{
	if (!bLevelBuilt) return;
	AOutlawsLevelActor* L = FindLevel();
	AOutlawsCoopSession* S = GetSession();
	if (!L || !S || !L->Inf) return;
	FOutlawsInfRuntime& Inf = *L->Inf;
	if (!Inf.OnForward && !Inf.OnEventScope)
	{
		// (this machine's level built again on its own, a death's restart: its new INF set up for the session again)
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP this machine's INF was built again on its own: set up for the session again"));
		SetupInf(L);
	}
	const int32 N = Inf.Elevators.Num();
	if (IsHost())
	{
		if (Now < NextInfSnapAt) return;
		NextInfSnapAt = Now + InfSnapInterval;
		if (InfSeen.Num() != N) InfSeen.SetNum(N);
		S->InfElevators.SetNum(N);
		for (int32 i = 0; i < N; ++i)
		{
			const FOutlawsCoopInfSnap Sn = InfSnapOf(Inf.Elevators[i]);
			// (REQ-SECTORLIGHT) a flickering light's stops are not logged: a few a second each would use up the moves' lines
			if ((InfSeen[i].Target != Sn.Target || InfSeen[i].Stop != Sn.Stop) && !Inf.Elevators[i].IsLight()) LogInfMove(L, i, InfSeen[i], Sn);
			InfSeen[i] = Sn;
			if (S->InfElevators[i] != Sn) S->InfElevators[i] = Sn;
		}
		TArray<uint8> Bits;
		Bits.SetNumZeroed((Inf.Triggers.Num() + 7) / 8);
		for (int32 t = 0; t < Inf.Triggers.Num(); ++t)
		{
			if (Inf.Triggers[t].bMaster) Bits[t >> 3] |= uint8(1 << (t & 7));
		}
		if (Bits != S->InfTriggers) S->InfTriggers = MoveTemp(Bits);
		S->InfEpoch = StartedSerial;
		return;
	}
	if (!IsClient() || S->InfEpoch != StartedSerial || S->InfElevators.Num() == 0) return;
	if (S->InfElevators.Num() != N && InfMovesThisLevel == 0 && InfSeen.Num() == N)
	{
		UE_LOG(LogOutlawsCoop, Warning, TEXT("OLCOOP inf: the host has %d elevators, this machine %d: the first %d are shown"), S->InfElevators.Num(), N, FMath::Min(N, S->InfElevators.Num()));
	}
	if (InfSeen.Num() != N) InfSeen.SetNum(N);
	const int32 M = FMath::Min(N, S->InfElevators.Num());
	for (int32 i = 0; i < M; ++i)
	{
		const FOutlawsCoopInfSnap& Sn = S->InfElevators[i];
		if (InfSeen[i] == Sn) continue;
		const FOutlawsCoopInfSnap Was = InfSeen[i];
		InfSeen[i] = Sn;
		Inf.SetFromHost(i, Sn.Value, Sn.Stop, Sn.Target, (Sn.Flags & 1) != 0, (Sn.Flags & 2) != 0, (Sn.Flags & 4) != 0);
		if ((Was.Target != Sn.Target || Was.Stop != Sn.Stop) && !Inf.Elevators[i].IsLight()) LogInfMove(L, i, Was, InfSnapOf(Inf.Elevators[i]));
	}
	if (S->InfTriggers != InfTriggersSeen)
	{
		InfTriggersSeen = S->InfTriggers;
		for (int32 t = 0; t < Inf.Triggers.Num() && (t >> 3) < InfTriggersSeen.Num(); ++t)
		{
			Inf.Triggers[t].bMaster = (InfTriggersSeen[t >> 3] & (1 << (t & 7))) != 0;
		}
	}
}

// ---- the host runs the client's events; the client takes the host's news ---------------------------------------------

void UOutlawsCoop::OnInfEvent(int32 Who, int32 Epoch, int32 Sector, int32 Wall, int32 Event, const TArray<int32>& Items)
{
	if (!IsHost()) return;
	RunFor(Epoch, [this, Who, Sector, Wall, Event, Items]()
	{
		AOutlawsLevelActor* L = FindLevel();
		if (!L || !L->Inf || !L->Inf->Live.Sectors.IsValidIndex(Sector)) return;
		FOutlawsInfRuntime& Inf = *L->Inf;
		// as if this machine's own player had done it, with the other player's keys (the lock asks that player's pockets;
		// this machine's player keeps theirs)
		TSet<int32> Mine = MoveTemp(Inf.Items);
		Inf.Items = TSet<int32>(Items);
		const int32 WasCauser = InfCauser;
		InfCauser = Who;
		if (Wall >= 0) Inf.WallEvent(Sector, Wall, uint32(Event));
		else Inf.SectorEvent(Sector, uint32(Event));
		InfCauser = WasCauser;
		Inf.Items = MoveTemp(Mine);
		++InfEventsRun;
	});
}

bool UOutlawsCoop::EnemyDoorEvent(AOutlawsLevelActor* L, int32 Sector, uint32 Event)
{
	if (!L || !L->Inf) return false;
	UOutlawsCoop* C = Get(L);
	if (!C || !C->IsHost()) return L->Inf->SectorEvent(Sector, Event);
	TGuardValue<int32> Who(C->InfCauser, InfCauserEnemy);
	return L->Inf->SectorEvent(Sector, Event);
}

void UOutlawsCoop::OnInfSound(int32 Epoch, const FString& Wav, const FVector& At, int32 Loop, uint8 SoundMode)
{
	if (!IsClient()) return;
	RunFor(Epoch, [this, Wav, At, Loop, SoundMode]()
	{
		AOutlawsLevelActor* L = FindLevel();
		if (!L) return;
		++InfSoundsPlayed;
		if (SoundMode == 0)
		{
			L->PlayWav(Wav, At);
			return;
		}
		if (L->TuneSound(Loop, Wav, At, SoundMode == 1)) return;     // (REQ-PIANO: the piano's tune, once to its end)
		// a lift's loop, by its elevator as the host's own are
		if (TWeakObjectPtr<UAudioComponent>* Old = L->LoopSounds.Find(Loop))
		{
			if (Old->IsValid()) (*Old)->Stop();
			L->LoopSounds.Remove(Loop);
		}
		if (SoundMode == 1)
		{
			if (UAudioComponent* AC = L->PlayWav(Wav, At, true)) L->LoopSounds.Add(Loop, AC);
		}
	});
}

bool UOutlawsCoop::HomBossDown(const UObject* Context, int32 Outcome, int32 NameId)
{
	UOutlawsCoop* C = Get(Context);
	if (!C || C->Mode == EOutlawsCoopMode::None) return true;
	if (C->IsClient())
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP hom: the outlaw is down here; the host's word settles his fate"));
		return false;
	}
	const int32 Epoch = C->LevelEpoch();
	C->ForClients([&](AOutlawsCoopLink& CL) { CL.ClientInfNews(Epoch, 5, Outcome, NameId, 0, 0); });
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP hom: the outlaw is down (outcome %d, name %d): the host's word sent to every client"), Outcome, NameId);
	return true;
}

void UOutlawsCoop::OnInfNews(int32 Epoch, uint8 Kind, int32 A, int32 B, int32 C, int32 D)
{
	if (!IsClient()) return;
	RunFor(Epoch, [this, Kind, A, B, C, D]()
	{
		AOutlawsLevelActor* L = FindLevel();
		if (!L || !L->Inf) return;
		switch (Kind)
		{
		case 0:
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP inf: message %d from the host's INF"), A);
			if (L->Inf->OnUserMessage) L->Inf->OnUserMessage(A);
			break;
		case 1:
			if (B)
			{
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP inf: key opens the lock for the client player (item %d)"), A);
			}
			else
			{
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP inf: key refused for the client player (item %d)"), A);
			}
			break;
		case 2:
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP inf: this player's item %d went into the lock"), A);
			L->UseUpItem(A);
			break;
		case 3:
		{
			FOutlawsInfMessage M;
			M.Type = static_cast<EOutlawsInfMsg>(C);
			M.Arg = D;
			L->AtxWallMessage(A, B, M);
			break;
		}
		case 4:
		{
			// (BUG-RANCHDOOR) the host's INF set off the level's mid-level movie: this player's copy plays (the door shut here
			// too: it moves on the host's word, and the host holds it until its own movie is up)
			const FString File = L->InfMidMovieFile();
			++MidMoviesPlayed;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP inf: the host's mid-level movie (%s) plays here too"), File.IsEmpty() ? TEXT("none in this INF") : *File);
			if (!File.IsEmpty()) L->TriggerMidLevelMovie(File, true);
			break;
		}
		case 5:
			// (REQ-HMBOSSEND) the host's Historical Mission outlaw down: A his fate (1 captured, 2 killed), B his name's number
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP hom: the host's word: the outlaw is down (outcome %d, name %d)"), A, B);
			L->HomBossDown(A, B);
			break;
		default:
			break;
		}
	});
}

// ---- the level's flow ---------------------------------------------------------------------------------------------------

void UOutlawsCoop::RequestStart(EOutlawsCoopStart Kind, const FString& Level)
{
	AOutlawsCoopSession* S = GetSession();
	AOutlawsLevelActor* L = FindLevel();
	if (!IsHost() || !S || !L || Level.IsEmpty()) return;
	S->Level = Level.ToLower();
	S->StartKind = uint8(Kind);
	S->Difficulty = int32(L->Difficulty);
	S->bHom = L->bHom;                           // (REQ-COOPHM: the Historical Missions session goes with every start)
	S->HomMission = L->bHom ? L->HomMission : FString();
	S->HomBack = L->bHom ? L->HomReturnSector : FString();
	PrepareExtras(S, S->Level, S->Difficulty);   // (2026-10-02: the extra bad guys, with the start)
	++S->StartSerial;
	S->ForceNetUpdate();
	static const TCHAR* Kinds[] = { TEXT("from the lobby"), TEXT("the story goes on"), TEXT("the level again"), TEXT("a level skip") };
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the host starts %s for both (%s), serial %d"), *S->Level, Kinds[FMath::Min<int32>(int32(Kind), 3)], S->StartSerial);
}

bool UOutlawsCoop::HomRide(AOutlawsLevelActor* L, const FString& Mission, const FString& Back)
{
	UOutlawsCoop* C = Get(L);
	if (!C || C->Mode == EOutlawsCoopMode::None || !L) return false;
	AOutlawsPlayer* P = C->LocalPlayer();
	if (!C->IsHost())
	{
		if (P) P->CoopToast(TEXT("In co-op the host chooses the mission"));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP hom: the client's ride to %s refused: the host's to choose"), *Mission);
		return true;
	}
	if (P && P->IsMenuOpen()) P->CoopCloseMenu();
	L->bHom = true;
	L->HomMission = Mission;
	L->HomReturnSector = Back;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP hom: the host rides out to %s (back to %s): for every player"), *Mission, Back.IsEmpty() ? TEXT("none") : *Back);
	C->RequestStart(EOutlawsCoopStart::Lobby, Mission);
	return true;
}

bool UOutlawsCoop::OnLevelWon(AOutlawsLevelActor* L, const FString& Why)
{
	UOutlawsCoop* C = Get(L);
	if (!C || C->Mode == EOutlawsCoopMode::None) return true;
	// (2026-10-02: a menu no longer pauses the other player, so the level may end while this player's is open: it closes,
	// so the fade and the movie are not behind it)
	auto CloseMenuHere = [C]()
	{
		AOutlawsPlayer* P = C->LocalPlayer();
		if (!P || !P->IsMenuOpen()) return;
		P->CoopCloseMenu();
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP this player's menu closed: the level is won"));
	};
	if (C->IsHost())
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP level won: next %s (host) (%s, movie %s)"), L->NextLevelName.IsEmpty() ? TEXT("none") : *L->NextLevelName, *Why,
			L->NextMovie.IsEmpty() ? TEXT("none") : *L->NextMovie);
		const int32 Epoch = C->LevelEpoch();
		// (REQ-COOPHORSES) a Historical Mission's result is the host's (whoever held the outlaw, whichever horse was gripped):
		// its card goes to every client with the word, after a unit separator
		FString Sent = Why;
		if (L->bHom && !L->HomCardRows.IsEmpty())
		{
			Sent += TEXT("\x1f") + L->HomMission + TEXT("\t") + L->HomReturnSector + TEXT("\t") + L->HomCardTitle + TEXT("\t") + L->HomResultText;
			for (const TPair<FString, FString>& Row : L->HomCardRows) Sent += TEXT("\t") + Row.Key + TEXT("\t") + Row.Value;
		}
		C->ForClients([&](AOutlawsCoopLink& CL) { CL.ClientLevelWon(Epoch, L->NextLevelName, L->NextMovie, Sent); });
		CloseMenuHere();
		return true;
	}
	if (C->bApplyingLevelEnd)
	{
		CloseMenuHere();
		return true;
	}
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the level's end here (%s) is the host's to call"), *Why);
	return false;
}

void UOutlawsCoop::OnLevelWonByHost(int32 Epoch, const FString& Next, const FString& Movie, const FString& Why)
{
	if (!IsClient()) return;
	RunFor(Epoch, [this, Next, Movie, Sent = Why]()
	{
		AOutlawsLevelActor* L = FindLevel();
		if (!L) return;
		FString Why = Sent, Hom;
		Sent.Split(TEXT("\x1f"), &Why, &Hom);
		if (Hom.IsEmpty()) Why = Sent;
		TArray<FString> HomParts;
		Hom.ParseIntoArray(HomParts, TEXT("\t"), false);
		if (HomParts.Num() >= 4)
		{
			// (REQ-COOPHORSES) the host's Historical Mission: this machine ends it as one too, with the host's result
			L->bHom = true;
			L->HomMission = HomParts[0];
			L->HomReturnSector = HomParts[1];
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP level won: next %s (client) (the host's word: %s, movie %s)"), Next.IsEmpty() ? TEXT("none") : *Next, *Why, Movie.IsEmpty() ? TEXT("none") : *Movie);
		// (REQ-COOPHM) the session's totals as they were: the host's scores replace this machine's own (its own wounds) below
		const int32 Score0 = L->HomGameScore, Gold0 = L->HomTotalGold, Kills0 = L->HomTotalKills, Wounds0 = L->HomTotalWounds, Bounty0 = L->HomTotalBounty;
		const TMap<FString, int32> Porches0 = L->HomPorchEvents;
		{
			TGuardValue<bool> Apply(bApplyingLevelEnd, true);
			L->CompleteLevel(Why);
		}
		if (HomParts.Num() >= 4)
		{
			L->HomCardTitle = HomParts[2];
			L->HomResultText = HomParts[3];
			L->HomCardRows.Reset();
			for (int32 i = 4; i + 1 < HomParts.Num(); i += 2) L->HomCardRows.Emplace(HomParts[i], HomParts[i + 1]);
			const TPair<FString, FString>* Res = L->HomCardRows.FindByPredicate([](const TPair<FString, FString>& R) { return R.Key == TEXT("Result"); });
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP hom: %s ended: %s (the host's result, client): %s"), *L->HomMission, Res ? *Res->Value : TEXT("?"), *L->HomResultText);
			auto RowNum = [L](const TCHAR* Key)
			{
				const TPair<FString, FString>* R = L->HomCardRows.FindByPredicate([Key](const TPair<FString, FString>& X) { return X.Key == Key; });
				return R ? FCString::Atoi(*R->Value) : 0;
			};
			const int32 OwnScore = L->HomGameScore;
			L->HomGameScore = Score0;
			L->HomTotalGold = Gold0;
			L->HomTotalKills = Kills0;
			L->HomTotalWounds = Wounds0;
			L->HomTotalBounty = Bounty0;
			L->HomPorchEvents = Porches0;
			const bool bScored = Res && Res->Value != TEXT("ESCAPED");
			if (bScored)
			{
				L->HomTotalGold += RowNum(TEXT("Gold"));
				L->HomTotalKills += RowNum(TEXT("Kills"));
				L->HomTotalWounds += RowNum(TEXT("Wounds"));
				L->HomTotalBounty += RowNum(TEXT("Bounty"));
				L->HomGameScore = RowNum(TEXT("Game score"));
				if (!L->HomReturnSector.IsEmpty()) L->HomPorchEvents.Add(L->HomReturnSector, Res->Value == TEXT("CAPTURED") ? 131072 : 262144);
			}
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP hom: the host's scores kept (client): level score %d, game score %d (this machine's own game score was %d)"),
				RowNum(TEXT("Level score")), L->HomGameScore, OwnScore);
		}
		L->NextLevelName = Next;             // (the host's story order: the same here)
		L->NextMovie = Movie;
		if (L->LevelEndTimer < 0.f)
		{
			bWaitingForStart = true;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the level's end does not run here (this player is dead): the host's start brings the next level"));
			SayLevelEndDone();
		}
	});
}

bool UOutlawsCoop::HoldNextLevel(AOutlawsLevelActor* L)
{
	UOutlawsCoop* C = Get(L);
	if (!C || C->Mode == EOutlawsCoopMode::None || C->bStartingLevel || L->bMovieToMenu || L->bFrontEnd) return false;
	// (REQ-UNLOCKS) the final cutscene and the credits over on this machine, in the session: each player's own profile earns
	if (L->NextLevelName.IsEmpty()) OutlawsUnlocks::EarnPending(L, TEXT("the co-op ending"));
	if (L->Movie)
	{
		L->Movie->Destroy();
		L->Movie = nullptr;
	}
	L->FadeCamera(1.f, 1.f, 0.f, true);          // (dark until the next level is built)
	if (C->IsHost())
	{
		if (L->NextLevelName.IsEmpty())
		{
			// the story's end (or a mission won): the session is over, both back to their title menus
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the game is won: no level follows"));
#if WITH_OUTLAWS_TESTS
			if (C->bAuto)
			{
				C->Summary(TEXT("the game is won"));
				C->TestSay(TEXT("quit"));
				C->QuitForTest();
			}
			else
#endif
			{
				C->EndSession(TEXT("the game is won"), true);
			}
			return true;
		}
		// N1 (the spec's 20 s cap): every player still in this level finishes its own end first (its movie, its panels)
		C->NextHoldLevel = L->NextLevelName;
		C->NextHoldSince = FPlatformTime::Seconds();
		C->NextHoldLoggedWaiting = -1;
		C->TickNextLevelHold(C->NextHoldSince);
		return true;
	}
	C->bWaitingForStart = true;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the level's end is over here: waiting for the host to start %s"), L->NextLevelName.IsEmpty() ? TEXT("the next") : *L->NextLevelName);
	C->SayLevelEndDone();
	return true;
}

void AOutlawsCoopLink::ServerLevelEndDone_Implementation(int32 Serial)
{
	if (!Admit(EOLGate::LevelEnd, Serial >= 0)) return;
	EndDoneSerial = FMath::Max(EndDoneSerial, Serial);
}

void UOutlawsCoop::SayLevelEndDone()
{
	if (!IsClient() || EndDoneSent == StartedSerial) return;
	EndDoneSent = StartedSerial;
	if (AOutlawsCoopLink* Mine = GetLink(MyIndex())) Mine->ServerLevelEndDone(StartedSerial);
}

void UOutlawsCoop::TickNextLevelHold(double Now)
{
	if (NextHoldLevel.IsEmpty() || !IsHost()) return;
	// the players in this level (built it) who have not finished its end yet; one who left, or never built it, is not waited for
	constexpr double Cap = 20.0;
	int32 Waiting = 0, InLevel = 0;
	ForClients([&](AOutlawsCoopLink& K)
	{
		if (K.BuiltSerial != StartedSerial) return;
		++InLevel;
		Waiting += K.EndDoneSerial >= StartedSerial ? 0 : 1;
	});
	const bool bCapped = Now - NextHoldSince >= Cap;
	if (Waiting > 0 && !bCapped)
	{
		if (Waiting != NextHoldLoggedWaiting)
		{
			NextHoldLoggedWaiting = Waiting;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the next level (%s) waits for %d of %d players to finish the level's end"), *NextHoldLevel, Waiting, InLevel);
		}
		return;
	}
	if (InLevel > 0)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the next level (%s) starts: %s after %.1f s"), *NextHoldLevel,
			Waiting > 0 ? *FString::Printf(TEXT("%d of %d players still in the level's end (the %.0f s cap)"), Waiting, InLevel, Cap) : TEXT("every player has finished the level's end"),
			Now - NextHoldSince);
	}
	NextHoldLastWaiting = Waiting;
	const FString Next = NextHoldLevel;
	NextHoldLevel.Reset();
	RequestStart(EOutlawsCoopStart::NextLevel, Next);
}

void UOutlawsCoop::NoteMidLevelMovie(AOutlawsLevelActor* L, const FString& Base)
{
	UOutlawsCoop* C = Get(L);
	if (!C || !C->IsHost()) return;
	const int32 Epoch = C->LevelEpoch();
	C->ForClients([&](AOutlawsCoopLink& CL) { CL.ClientInfNews(Epoch, 4, 0, 0, 0, 0); ++C->MidMoviesSent; });
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP inf: the mid-level movie %s set off by the %s player: its clients play it too"), *Base, InfWho(C->InfCauser));
}

void UOutlawsCoop::NoteMovieSkip(AOutlawsLevelActor* L)
{
	UOutlawsCoop* C = Get(L);
	if (!C || !L->IsMoviePlaying() || C->bApplyingSkip) return;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP movie skipped by the %s player"), C->IsHost() ? TEXT("host") : TEXT("client"));
	if (C->IsClient())
	{
		if (AOutlawsCoopLink* Mine = C->GetLink(C->MyIndex())) Mine->ServerMovieSkip(C->StartedSerial);
	}
}

void UOutlawsCoop::OnMovieSkip(int32 Who, int32 Epoch)
{
	AOutlawsLevelActor* L = FindLevel();
	if (!IsHost() || !L || Epoch != StartedSerial) return;
	if (L->IsMoviePlaying())
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP movie skipped by the %s player: ended here too"), InfWho(Who));
		TGuardValue<bool> Apply(bApplyingSkip, true);
		L->SkipMovie();                            // (its end starts the next level for both)
	}
	else if (L->LevelEndTimer >= 0.f && !L->NextLevelName.IsEmpty())
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP movie skipped by the %s player before it began here: the next level now"), InfWho(Who));
		L->LevelEndTimer = -1.f;
		RequestStart(EOutlawsCoopStart::NextLevel, L->NextLevelName);
	}
}

bool UOutlawsCoop::MenuLevelChange(AOutlawsPlayer* P, EOutlawsCoopStart Kind, const FString& Level, bool bAskOnly)
{
	UOutlawsCoop* C = Get(P);
	if (!C || C->Mode == EOutlawsCoopMode::None || !P) return false;
	if (!C->IsHost())
	{
		// the host decides where both players are: the client's menu says so and does nothing
		P->CoopToast(Kind == EOutlawsCoopStart::Restart ? TEXT("In co-op the host restarts the level") : TEXT("In co-op the host chooses the level"));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the client's %s refused: the host's to choose"), Kind == EOutlawsCoopStart::Restart ? TEXT("Restart Level") : TEXT("level skip"));
		return true;
	}
	if (bAskOnly) return false;                     // (the host: the confirmation page as in single player)
	AOutlawsLevelActor* L = C->FindLevel();
	if (!L) return false;
	P->CoopCloseMenu();
	C->RequestStart(Kind, Level.IsEmpty() ? L->LevelName : Level);
	return true;
}

FString UOutlawsCoop::InfSummary() const
{
	return FString::Printf(TEXT("events sent %d, events run for the client %d, sounds sent %d, sounds played %d, elevator moves seen %d"),
		InfEventsSent, InfEventsRun, InfSoundsSent, InfSoundsPlayed, InfMovesLogged);
}

// ---- tests (-OLCoopAuto after milestone 4's, or -OLCoopTest=inf alone) ----------------------------------------------------

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TestLogElevator(AOutlawsLevelActor* L, int32 Index, const TCHAR* Why)
{
	const FOutlawsElevator* E = L && L->Inf && L->Inf->Elevators.IsValidIndex(Index) ? &L->Inf->Elevators[Index] : nullptr;
	if (!E) return;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: elevator %d %s at %.2f, stop %d, target %d (%s) [%s]"), Index, *E->Name, E->Current, E->Stop, E->Target,
		IsHost() ? TEXT("host") : TEXT("client"), Why);
	if (E->IsLight() && E->Sectors.Num() > 0)
	{
		// (REQ-SECTORLIGHT) a light: the AMBIENT its sectors are drawn at on this machine
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: sector light %s: sector %d AMBIENT %d (%s)"), *E->Name, E->Sectors[0], L->Inf->Live.Sectors[E->Sectors[0]].Ambient,
			IsHost() ? TEXT("host") : TEXT("client"));
	}
}
#endif // WITH_OUTLAWS_TESTS

namespace
{
	int32 ElevatorNamed(const FOutlawsInfRuntime& Inf, const TCHAR* Name)
	{
		for (int32 i = 0; i < Inf.Elevators.Num(); ++i)
		{
			if (Inf.Elevators[i].Name.Equals(Name, ESearchCase::IgnoreCase)) return i;
		}
		return INDEX_NONE;
	}
}

#if WITH_OUTLAWS_TESTS
bool UOutlawsCoop::TestGoToDoor(AOutlawsLevelActor* L, AOutlawsPlayer* P, int32 Index)
{
	// this machine's player set down just outside the door (in the room beside it, facing it): a free floor two thirds of a
	// metre or more past the middle of one of its open walls
	if (!L || !P || !L->Inf || !L->Inf->Elevators.IsValidIndex(Index) || L->Inf->Elevators[Index].Sectors.Num() == 0) return false;
	const FOutlawsInfRuntime& Inf = *L->Inf;
	const int32 Door = Inf.Elevators[Index].Sectors[0];
	const FOutlawsSector& D = Inf.Live.Sectors[Door];
	FVector2D Mid = FVector2D::ZeroVector;
	for (const FVector2D& V : D.Verts) Mid += V;
	Mid /= double(FMath::Max(1, D.Verts.Num()));
	UWorld* W = L->GetWorld();
	const float Half = P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const float Radius = P->GetCapsuleComponent()->GetScaledCapsuleRadius();
	for (const double Out : { 2.2, 3.0, 4.0 })
	{
		for (const FOutlawsWall& Wl : D.Walls)
		{
			if (Wl.Adjoin < 0 || !Inf.Live.Sectors.IsValidIndex(Wl.Adjoin) || Inf.ElevatorsBySector.Contains(Wl.Adjoin)) continue;
			if (!D.Verts.IsValidIndex(Wl.V1) || !D.Verts.IsValidIndex(Wl.V2)) continue;
			const FVector2D A = D.Verts[Wl.V1], B = D.Verts[Wl.V2], M = (A + B) * 0.5;
			FVector2D N(-(B - A).Y, (B - A).X);
			N.Normalize();
			if (FVector2D::DotProduct(N, M - Mid) < 0.0) N = -N;
			const FVector2D At = M + N * Out;
			const FOutlawsSector& Room = Inf.Live.Sectors[Wl.Adjoin];
			const FOutlawsSector* Here = Inf.Live.SectorAt(At.X, Room.Floor.Y + 0.5, At.Y);
			if (!Here || Inf.ElevatorsBySector.Contains(Here->Index) || Here->Ceil.Y - Here->Floor.Y < 7.0) continue;
			const FVector Feet = FOutlawsGeometry::ToUE(At.X, Here->Floor.Y, At.Y);
			const FVector Centre = Feet + FVector(0.0, 0.0, Half + 3.0);
			FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsCoopTestDoor), false, P);
			if (W && W->OverlapBlockingTestByChannel(Centre, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(Radius, Half), Q)) continue;
			const FVector Look = FOutlawsGeometry::ToUE(M.X, Here->Floor.Y, M.Y) - Feet;
			const FRotator Face(0.0, Look.Rotation().Yaw, 0.0);
			P->TeleportTo(Centre, Face, false, true);
			if (AController* Ctl = P->GetController()) Ctl->SetControlRotation(Face);
			TestDoorSector = Door;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the %s player stands outside %s (elevator %d, sector %d) in sector %d"), IsHost() ? TEXT("host") : TEXT("client"),
				*Inf.Elevators[Index].Name, Index, Door, Here->Index);
			return true;
		}
	}
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: no place found outside elevator %d (sector %d)"), Index, Door);
	TestDoorSector = Door;                      // (pushed from where the player stands)
	return false;
}

void UOutlawsCoop::TickHostInfTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	if (!L->Inf && TestStep < 51) return;          // (from the level's end on, the INF is cleared away for the movie)
	auto Named = [L](const TCHAR* Name) { return L->Inf ? ElevatorNamed(*L->Inf, Name) : INDEX_NONE; };
	auto Both = [this]()
	{
		const AOutlawsCoopLink* CL = GetLink(1);
		return bLevelBuilt && StartedSerial > TestArg && CL && CL->BuiltSerial == StartedSerial;
	};
	switch (TestStep)
	{
	case 40:
		// (both players out of the bad guys' reach for these checks: a death here would stop the world they run in)
		L->bInvincible = true;
		TestSay(TEXT("godmode"), 1);
		if (TestOnly == TEXT("infend"))
		{
			// -OLCoopTest=infend: a level end the client's player sets off (the train's LAST_DOOR: a push there ends the
			// level): the first sector trigger whose message is END_LEVEL, pushed by the client
			int32 EndSector = INDEX_NONE;
			for (const FOutlawsTrigger& T : L->Inf->Triggers)
			{
				if (T.bLine || !L->Inf->Inf.Items.IsValidIndex(T.Item) || !L->Inf->Inf.Items[T.Item].Classes.IsValidIndex(T.ClassIndex)) continue;
				const FOutlawsInfClass& Cl = L->Inf->Inf.Items[T.Item].Classes[T.ClassIndex];
				for (const FOutlawsInfMessage& M : Cl.ClientMsg) if (M.Type == EOutlawsInfMsg::EndLevel) EndSector = T.Sector;
				for (const FOutlawsInfMessage& M : Cl.TriggerMessages) if (M.Type == EOutlawsInfMsg::EndLevel) EndSector = T.Sector;
				if (EndSector != INDEX_NONE) break;
			}
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client pushes the level's END_LEVEL trigger in sector %d"), EndSector);
			TestArg = StartedSerial;
			TestSay(TEXT("infsector"), EndSector);
			TestStep = 51;
			TestAt = Now + 5.5;
			return;
		}
		// (REQ-SECTORLIGHT) 0: a sector light as each machine has it (the hideout's ALIEN1: its stops 4 s apart)
		if (L->Inf->LightsFrom >= 0)
		{
			TestLogElevator(L, L->Inf->LightsFrom, TEXT("a sector light"));
			TestSay(TEXT("infstate"), L->Inf->LightsFrom);
		}
		// 1: the client pushes a door (the hideout's cow shed door; elsewhere the first door that swings)
		TestDoorA = Named(TEXT("COWDOOR"));
		for (int32 i = 0; TestDoorA < 0 && i < L->Inf->Elevators.Num(); ++i) if (L->Inf->Elevators[i].IsMorph()) TestDoorA = i;
		TestDoorB = Named(TEXT("HALL1")) >= 0 ? Named(TEXT("HALL1")) : Named(TEXT("RHSECRET"));
		for (int32 i = L->Inf->Elevators.Num() - 1; TestDoorB < 0 && i >= 0; --i) if (L->Inf->Elevators[i].IsMorph() && i != TestDoorA) TestDoorB = i;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: INF: the client pushes elevator %d, the host elevator %d"), TestDoorA, TestDoorB);
		TestLogElevator(L, TestDoorA, TEXT("before"));
		TestSay(TEXT("infdoor"), TestDoorA);
		TestStep = 41;
		TestAt = Now + 5.0;
		return;
	case 41:
		if (Now < TestAt) return;
		TestLogElevator(L, TestDoorA, TEXT("after the client's push"));
		TestSay(TEXT("infstate"), TestDoorA);
		// 2: the host pushes another; the client sees it move
		TestLogElevator(L, TestDoorB, TEXT("before"));
		TestGoToDoor(L, P, TestDoorB);
		TestStep = 42;
		TestAt = Now + 0.8;
		return;
	case 42:
		if (Now < TestAt) return;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host pushes sector %d: %s"), TestDoorSector, L->PushDoorSector(TestDoorSector) ? TEXT("taken") : TEXT("not taken"));
		TestStep = 43;
		TestAt = Now + 5.0;
		return;
	case 43:
		if (Now < TestAt) return;
		TestLogElevator(L, TestDoorB, TEXT("after the host's push"));
		TestSay(TEXT("infstate"), TestDoorB);
		// 3: a lock (the brass door): the client holds its key, the host does not
		TestKeyDoor = Named(TEXT("MHBRASS"));
		TestSay(TEXT("infkey"), 101);
		TestStep = 44;
		TestAt = Now + 1.0;
		return;
	case 44:
		if (Now < TestAt) return;
		TestLogElevator(L, TestKeyDoor, TEXT("before"));
		TestGoToDoor(L, P, TestKeyDoor);
		TestStep = 45;
		TestAt = Now + 0.8;
		return;
	case 45:
		if (Now < TestAt) return;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host (brass key %d) pushes sector %d: %s"), L->HasItem(101) ? 1 : 0, TestDoorSector, L->PushDoorSector(TestDoorSector) ? TEXT("taken") : TEXT("not taken"));
		TestStep = 46;
		TestAt = Now + 2.5;
		return;
	case 46:
		if (Now < TestAt) return;
		TestLogElevator(L, TestKeyDoor, TEXT("after the host's push without the key"));
		// ... and it opens for the client, who has it
		TestSay(TEXT("infdoor"), TestKeyDoor);
		TestStep = 47;
		TestAt = Now + 5.0;
		return;
	case 47:
		if (Now < TestAt) return;
		TestLogElevator(L, TestKeyDoor, TEXT("after the client's push with the key"));
		TestSay(TEXT("infstate"), TestKeyDoor);
		// 4: the other way round (the iron door): the host holds its key; the client is refused, then the host opens it
		TestKeyDoor = Named(TEXT("BIRON"));
		L->GiveItem(100);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host holds key item 100"));
		TestLogElevator(L, TestKeyDoor, TEXT("before"));
		TestSay(TEXT("infdoor"), TestKeyDoor);
		TestStep = 48;
		TestAt = Now + 3.0;
		return;
	case 48:
		if (Now < TestAt) return;
		TestLogElevator(L, TestKeyDoor, TEXT("after the client's push without the key"));
		TestGoToDoor(L, P, TestKeyDoor);
		TestStep = 49;
		TestAt = Now + 0.8;
		return;
	case 49:
		if (Now < TestAt) return;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host (iron key %d) pushes sector %d: %s"), L->HasItem(100) ? 1 : 0, TestDoorSector, L->PushDoorSector(TestDoorSector) ? TEXT("taken") : TEXT("not taken"));
		TestStep = 50;
		TestAt = Now + 5.0;
		return;
	case 50:
		if (Now < TestAt) return;
		TestLogElevator(L, TestKeyDoor, TEXT("after the host's push with the key"));
		TestSay(TEXT("infstate"), TestKeyDoor);
		// 5: the level is won on the host: both run its end, the client skips the movie, both build the next level
		TestArg = StartedSerial;
		L->CompleteLevel(TEXT("co-op test"));
		TestStep = 51;
		TestAt = Now + 4.5;
		return;
	case 51:
		if (Now < TestAt) return;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the level's end on the host: movie playing %d"), L->IsMoviePlaying() ? 1 : 0);
		TestSay(TEXT("skip"), 0);
		TestStep = 52;
		TestAt = Now + 40.0;                          // (the deadline for both builds)
		return;
	case 52:
	{
		const bool bBoth = Both();
		if (!bBoth && Now < TestAt) return;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: after the level's end both machines built %s: %s (serial %d)"), *BuiltLevel, bBoth ? TEXT("yes") : TEXT("NO"), StartedSerial);
		// 6: Restart Level by the host restarts both
		TestArg = StartedSerial;
		if (bBoth) RequestStart(EOutlawsCoopStart::Restart, BuiltLevel);
		TestStep = 53;
		TestAt = Now + 40.0;
		return;
	}
	case 53:
	{
		const bool bBoth = Both();
		if (!bBoth && Now < TestAt) return;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: after the host's Restart Level both machines built %s again: %s (serial %d)"), *BuiltLevel, bBoth ? TEXT("yes") : TEXT("NO"), StartedSerial);
		TestStep = 59;
		TestAt = Now + 2.0;
		return;
	}
	case 59:
		if (Now < TestAt) return;
		if (TestOnly.IsEmpty())
		{
			TestStep = 60;                               // (milestone 7's checks next: OutlawsCoopRules.cpp)
			TestAt = Now;
			return;
		}
		TestSay(TEXT("quit"), 0);
		TestStep = 100;
		TestAt = Now + 3.0;
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
