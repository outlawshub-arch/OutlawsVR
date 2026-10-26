// Story co-op (tools/olue/COOP_DESIGN.md), R7 (user 2026-10-03: "the host to be able to start the single player game in
// co-op. While they wait for the other player to join, they can start progressing through the levels ... the enemy count
// should still be 150% ... An autosave should still work. In a separate co-op autosave"): the lobby's "Start now" takes the
// host into the level alone, its listen server still open; a player who connects while a level is under way joins it from a
// snapshot of the host's game (the save format, sent as milestone 8's load sends a save), built and put back on the joiner's
// machine as a co-op load, the joiner standing beside the host with its own kit (its player2 block if the host has one, else
// the start kit) and full health.  While the host is between levels (the level's end, the movie, a load, a rebuild, its
// own death) the joiner waits in its lobby and joins the next level once it is built.  A player who leaves mid-level leaves
// the host playing on alone.
//
// None of this runs in single player: every entry is behind a session (IsHost / IsClient).
#include "OutlawsCoop.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsEnemy.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsRemotePlayer.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Policies/CondensedJsonPrintPolicy.h"
#include "Misc/FileHelper.h"
#include "Misc/SecureHash.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/IConsoleManager.h"

namespace
{
	// Q2 step 4: the late join's replay spread over frames (UOutlawsCoop::TickJoinReplay)
	TAutoConsoleVariable<int32> CVarOljrPerFrame(TEXT("ol.coop.JoinReplayPerFrame"), 24,
		TEXT("A late join puts back this many bad guys a frame (first snapshots, then saved states); 0 = all in one frame (as before). -OLJoinReplayPerFrame=<n>"));
	TAutoConsoleVariable<float> CVarOljrMs(TEXT("ol.coop.JoinReplayMs"), 2.f,
		TEXT("A late join's saved bad guys put back in a frame stop after this many ms (at least one)"));
	TAutoConsoleVariable<float> CVarOljrWarm(TEXT("ol.coop.JoinWarmSec"), 0.5f,
		TEXT("A spread late join holds the view black while it puts the world back, and this long after (the new views drawn, their PSOs made, under it); 0 = no hold"));

	int32 OljrPerFrame()
	{
		static int32 FromCmd = -2;
		if (FromCmd == -2)
		{
			FromCmd = -1;
			int32 V = 0;
			if (OLFlags::Value(TEXT("OLJoinReplayPerFrame"), V)) FromCmd = FMath::Max(0, V);
		}
		return FromCmd >= 0 ? FromCmd : FMath::Max(0, CVarOljrPerFrame.GetValueOnGameThread());
	}

	constexpr int32 JoinPartChars = 8000;          // as milestone 8's load: one part per reliable call, well under its limit

	bool ParseJoinJson(const FString& Text, TSharedPtr<FJsonObject>& Out)
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		return FJsonSerializer::Deserialize(Reader, Out) && Out.IsValid();
	}

	AOutlawsEnemy* JoinEnemyAt(const AOutlawsLevelActor* L, int32 Index)
	{
		const TArray<TWeakObjectPtr<AOutlawsEnemy>>* List = L ? &L->GetEnemies() : nullptr;
		return List && List->IsValidIndex(Index) ? (*List)[Index].Get() : nullptr;
	}

	/** (N1) the late joiner in the log: nothing more for the first client (the two-player words), else its number */
	FString LateWho(int32 Who)
	{
		return Who == 1 ? FString() : FString::Printf(TEXT(" (%s)"), UOutlawsCoop::WhoWord(Who));
	}

	const TCHAR* JoinStateText(uint8 State)
	{
		switch (State)
		{
		case UOutlawsCoop::JoinHostBusy: return TEXT("the host is between levels: you join at the next one");
		case UOutlawsCoop::JoinSent: return TEXT("the host's game is on its way");
		default: return TEXT("asking the host for its game");
		}
	}
}

// ---- the link's message -------------------------------------------------------------------------------------------------

void AOutlawsCoopLink::ServerJoinHello_Implementation()
{
	if (!Admit(EOLGate::JoinHello, true)) return;
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnJoinHello(PlayerIndex);
}

// ---- the host -------------------------------------------------------------------------------------------------------------

bool UOutlawsCoop::IsHostAlone(const UObject* Context)
{
	const UOutlawsCoop* C = Get(Context);
	if (!C || !C->IsHost()) return false;
	const AOutlawsCoopSession* S = C->GetSession();
	return !S || !S->bClientConnected;
}

void UOutlawsCoop::OnJoinHello(int32 Who)
{
	const AOutlawsCoopLink* CL = GetLink(Who);
	if (!IsHost() || Who <= 0 || !CL || !CL->bLateJoin) return;
	JoinAsked.Add(Who);                      // (N1: each late joiner its own snapshot)
	JoinBusyLogged.Reset();
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP join: the late player%s is in this world and asks for the game"), *LateWho(Who));
}

FString UOutlawsCoop::JoinBusyReason() const
{
	const AOutlawsCoopSession* S = GetSession();
	const AOutlawsLevelActor* L = FindLevel();
	const AOutlawsPlayer* P = LocalPlayer();
	if (!S || !L || !P || L->IsFrontEnd()) return TEXT("no level here");
	if (StartedSerial != S->StartSerial || !bLevelBuilt || L->IsRebuildPending() || L->bNeedPlace) return TEXT("the host's level is being built");
	if (bHostLoadPending || L->PendingLoad.IsValid()) return TEXT("a save is being put back");
	if (L->IsBetweenLevels() || L->LevelEndTimer >= 0.f || L->IsMoviePlaying() || L->bBossDefeated) return TEXT("the host is between levels");
	if (L->bDead || P->IsDeadView()) return TEXT("the host is down");
	return FString();
}

void UOutlawsCoop::TickJoin(double Now)
{
	if (!IsHost() || JoinAsked.Num() == 0) return;
	// (N1: each late joiner in turn, by player number)
	TArray<int32> Askers = JoinAsked.Array();
	Askers.Sort();
	for (const int32 Who : Askers) TickJoinFor(Now, Who);
}

void UOutlawsCoop::TickJoinFor(double Now, int32 Who)
{
	AOutlawsCoopSession* S = GetSession();
	AOutlawsCoopLink* CL = GetLink(Who);
	if (!S || !CL)
	{
		JoinAsked.Remove(Who);
		return;
	}
	if (CL->BuiltSerial > 0)
	{
		// (built a level: joined; from here it follows the host's starts as any other player does)
		JoinAsked.Remove(Who);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP join: the late player%s has built %s (serial %d): joined"), *LateWho(Who), *S->Level, CL->BuiltSerial);
		if (AOutlawsLevelActor* Lv = FindLevel())
		{
			const FString Name = CL->PlayerName;
			const bool bBack = !PlayerJsonFor(Who).IsEmpty();
			Lv->Notify(!Name.IsEmpty() ? Name + (bBack ? TEXT(" is back") : TEXT(" has joined"))
				: FString(bBack ? TEXT("Your partner is back") : TEXT("A friend has joined")), 2.5f);
		}
		return;
	}
	if (CL->JoinSerial > 0 && CL->JoinSerial == S->StartSerial) return;     // (sent for this level: its build is coming)
	const FString Busy = JoinBusyReason();
	if (!Busy.IsEmpty())
	{
		if (CL->JoinState != JoinHostBusy)
		{
			CL->JoinState = JoinHostBusy;
			CL->ForceNetUpdate();
		}
		if (Busy != JoinBusyLogged)
		{
			JoinBusyLogged = Busy;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP join: the late player%s waits (%s)"), *LateWho(Who), *Busy);
		}
		return;
	}
	JoinBusyLogged.Reset();
	AOutlawsLevelActor* L = FindLevel();
	AOutlawsPlayer* P = LocalPlayer();
	FString Text;
	int32 NumEnemies = 0, NumPickups = 0;
	if (!L || !P || !L->BuildSaveText(P, Text, NumEnemies, NumPickups)) return;
	// the joiner's own kit (N1: its block by its name, else its number): what it last told this host (a player back after a
	// drop, or a loaded save's block for it), else the co-op autosave's for this level
	const bool bOwn = !PlayerJsonFor(Who).IsEmpty();
	FString P2From = bOwn ? FString(TEXT("the player's own, as last told")) : FString(TEXT("none (the start kit)"));
	Text = SaveTextFor(Text, Who);
	if (!bOwn)
	{
		FString AutoText;
		TSharedPtr<FJsonObject> Auto, O;
		if (FFileHelper::LoadFileToString(AutoText, *AOutlawsLevelActor::SavePath(-2)) && ParseJoinJson(AutoText, Auto)
			&& Auto->GetStringField(TEXT("level")).Equals(L->LevelName, ESearchCase::IgnoreCase) && ParseJoinJson(Text, O))
		{
			const TSharedPtr<FJsonObject> Block = SaveBlockFor(*Auto, Who);
			if (Block.IsValid())
			{
				O->SetObjectField(TEXT("player2"), Block);
				Text.Reset();
				FJsonSerializer::Serialize(O.ToSharedRef(), TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Text));
				P2From = TEXT("the co-op autosave's");
			}
		}
	}
	const int32 Epoch = S->StartSerial;
	const int32 Parts = FMath::Max(1, FMath::DivideAndRoundUp(Text.Len(), JoinPartChars));
	for (int32 k = 0; k < Parts; ++k) CL->ClientSnapshotPart(Epoch, k, Parts, Text.Mid(k * JoinPartChars, JoinPartChars));
	CL->JoinSerial = Epoch;
	CL->JoinState = JoinSent;
	CL->ForceNetUpdate();
	++JoinsSent;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP join: the game sent to the late player%s: %s, serial %d, %d bytes in %d parts (%d bad guys, %d pickups; its kit: %s)"),
		*LateWho(Who), *L->LevelName, Epoch, Text.Len(), Parts, NumEnemies, NumPickups, *P2From);
}

void UOutlawsCoop::ForgetClient(int32 Who)
{
	AOutlawsLevelActor* L = FindLevel();
	int32 Bodies = 0, Pickups = 0;
	if (L)
	{
		// the bad guys this machine held for that player's hand drop where they are
		for (const TPair<int32, int32>& H : ClientHolds)
		{
			if (H.Value != Who) continue;
			AOutlawsEnemy* E = JoinEnemyAt(L, H.Key);
			if (E && E->bHeldByPlayer)
			{
				E->EndHeld(FVector::ZeroVector);
				++Bodies;
			}
		}
		// pickups in the other player's hand show again where they were
		for (auto It = PickupHolder.CreateIterator(); It; ++It)
		{
			if (It.Value() != Who) continue;
			const int32 i = PickupIndex(L, It.Key());
			if (i >= 0)
			{
				const AActor* A = L->Pickups[i].Actor.Get();
				ShowPickupHere(L, i, A ? A->GetActorLocation() : FVector::ZeroVector);
				++Pickups;
			}
			It.RemoveCurrent();
		}
	}
	// (N1: only that player's; the others' holds and throws stay)
	for (auto It = ClientHolds.CreateIterator(); It; ++It) if (It.Value() == Who) It.RemoveCurrent();
	for (auto It = ClientThrownBy.CreateIterator(); It; ++It)
	{
		if (It.Value() != Who) continue;
		ClientThrown.Remove(It.Key());
		It.RemoveCurrent();
	}
	if (Who > 0 && Who < MaxPlayers)
	{
		RemoteViews[Who] = FOutlawsCoopRemoteView();
		if (AOutlawsRemotePlayer* R = Puppets[Who].Get()) R->Destroy();
		Puppets[Who].Reset();
		LastPoseSeq[Who] = 0;
		OtherDeadAt[Who] = OtherAwayAt[Who] = false;
	}
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the %s player's things let go here: %d bad guys from their hands, %d pickups shown again"), WhoWord(Who), Bodies, Pickups);
}

// ---- the joiner -----------------------------------------------------------------------------------------------------------

bool UOutlawsCoop::TickClientJoin(double Now, AOutlawsCoopSession* S)
{
	if (!IsClient() || !S || StartedSerial > 0 || S->StartSerial == 0) return false;
	AOutlawsCoopLink* Mine = GetLink(MyIndex());
	if (!Mine || Mine->PlayerIndex == 0) return true;      // (its link says whether this is a late join: not here yet)
	if (!Mine->bLateJoin) return false;
	if (!bEnterPressed) return true;                     // (REQ-MPLOBBY: in the lobby until Enter Game; its Enter asks for the game)
	if (!bJoinHelloSent)
	{
		bJoinHelloSent = true;
		Mine->ServerJoinHello();
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP joining late: the host is playing %s (serial %d); asked for its game"), *S->Level, S->StartSerial);
	}
	if (Mine->JoinState != LoggedJoinState)
	{
		LoggedJoinState = Mine->JoinState;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP joining late: %s"), JoinStateText(Mine->JoinState));
		++Version;
	}
	if (Mine->JoinSerial == 0 || Mine->JoinSerial != S->StartSerial) return true;
	if (!SnapObj.IsValid() || SnapEpoch != Mine->JoinSerial) return true;       // (the parts still coming)
	if (S->ExtrasSerial != S->StartSerial) return true;                          // (the level's extra bad guys, as a start waits for them)
	const FString Lv = SnapObj->GetStringField(TEXT("level")).ToLower();
	const int32 Diff = FMath::Clamp(int32(SnapObj->GetNumberField(TEXT("difficulty"))), 0, 2);
	JoinSpawnDifficulty = SnapObj->HasField(TEXT("spawn_difficulty")) ? FMath::Clamp(int32(SnapObj->GetNumberField(TEXT("spawn_difficulty"))), 0, 2) : Diff;
	bJoinApply = true;
	StartedSerial = Mine->JoinSerial;
	StartJoinReplay();
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP joining late: building %s from the host's game (serial %d)"), *Lv, StartedSerial);
	BeginLevel(Lv, Diff, StartedSerial, uint8(EOutlawsCoopStart::Load));
	return true;
}

// ---- Q2 step 4: the late join's replay, a few bad guys a frame ------------------------------------------------------------

void UOutlawsCoop::StartJoinReplay()
{
	JoinReplayQueue.Reset();
	JoinReplayObj.Reset();
	JoinReplayStep = 1;
	JoinReplayEpoch = StartedSerial;
	bJoinReplayFade = false;
	JoinReplayReadyAt = JoinReplayWarmUntil = JoinReplayTailUntil = JoinReplayLastTick = JoinReplayBuiltAt = 0.0;
	JoinReplayWorkFrame = JoinReplayLastPlaceFrame = JoinReplayBuiltFrame = 0;
	JoinReplayWorkMs = JoinReplayMaxWorkMs = JoinReplayMaxFrameMs = JoinReplayMaxWorkFrameMs = 0.0;
	JoinReplayFramesMeasured = JoinReplayPlaced = JoinReplayPlaceFrames = JoinReplayRead = JoinReplayReadFrames = 0;
	JoinReplayBodies = JoinReplayDropNext = JoinReplayDropsMade = 0;
	const int32 N = OljrPerFrame();
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP join replay: %s"), N > 0
		? *FString::Printf(TEXT("%d bad guys a frame (ms cap %.1f), the view held black and %.2f s after"), N, CVarOljrMs.GetValueOnGameThread(), CVarOljrWarm.GetValueOnGameThread())
		: TEXT("all in one frame (ol.coop.JoinReplayPerFrame 0)"));
}

int32 UOutlawsCoop::JoinPlaceBudget() const
{
	if (JoinReplayStep != 1 || !bJoinApply) return 0;
	const int32 N = OljrPerFrame();
	return N > 0 && GFrameCounter == JoinReplayBuiltFrame ? -1 : N;
}

void UOutlawsCoop::NoteJoinReplayWork(double Ms)
{
	if (JoinReplayWorkFrame != GFrameCounter)
	{
		JoinReplayWorkFrame = GFrameCounter;
		JoinReplayWorkMs = 0.0;
	}
	JoinReplayWorkMs += Ms;
	JoinReplayMaxWorkMs = FMath::Max(JoinReplayMaxWorkMs, JoinReplayWorkMs);
}

bool UOutlawsCoop::DrainJoinReplay(AOutlawsLevelActor* L)
{
	if (!L) return false;
	TGuardValue<bool> Quiet(L->bRestoring, true);      // (a save being put back: no sounds, as ApplyLoad)
	const int32 PerFrame = FMath::Max(1, OljrPerFrame());
	const double Cap = FMath::Max(0.1f, CVarOljrMs.GetValueOnGameThread());
	const double T0 = FPlatformTime::Seconds();
	int32 Done = 0;
	// the things dropped in play first, in the save's order (numbered once all are made), a sixth as many a frame
	const int32 DropsPerFrame = FMath::Max(1, PerFrame / 6);
	int32 Drops = 0;
	while (JoinReplayDropNext < L->DeferredDrops.Num() && Drops < DropsPerFrame)
	{
		const AOutlawsLevelActor::FDeferredDrop& D = L->DeferredDrops[JoinReplayDropNext++];
		AActor* Made = L->SpawnGroundItem(D.Ground, D.At);
		if (Made && !D.Contents.IsEmpty()) ApplyGroundContents(L, Made, D.Contents);
		++Drops;
		if ((FPlatformTime::Seconds() - T0) * 1000.0 > Cap) break;
	}
	JoinReplayDropsMade += Drops;
	if (JoinReplayDropNext < L->DeferredDrops.Num())
	{
		if (Drops > 0) ++JoinReplayReadFrames;
		return false;
	}
	while (JoinReplayQueue.Num() > 0 && Done < PerFrame && (FPlatformTime::Seconds() - T0) * 1000.0 <= Cap)
	{
		// (from the back: RemoveAt(0) on 265 would move the rest each time; the order does not matter, each its own)
		TPair<TWeakObjectPtr<AOutlawsEnemy>, TSharedPtr<FJsonObject>> It = JoinReplayQueue.Pop(false);
		if (AOutlawsEnemy* E = It.Key.Get(); E && It.Value.IsValid()) E->ReadSave(*It.Value);
		++Done;
		if ((FPlatformTime::Seconds() - T0) * 1000.0 > Cap) break;
	}
	JoinReplayRead += Done;
	if (Done > 0 || Drops > 0) ++JoinReplayReadFrames;
	if (JoinReplayQueue.Num() > 0) return false;
	L->DeferredDrops.Reset();
	return true;
}

void UOutlawsCoop::EndJoinReplay(const TCHAR* Why)
{
	if (JoinReplayStep == 0) return;
	if (bJoinReplayFade)
	{
		if (AOutlawsLevelActor* L = FindLevel()) L->FadeCamera(1.f, 0.f, 0.5f, false);
		bJoinReplayFade = false;
	}
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP join replay: %s: first snapshots %d over %d frames (%d bodies put back), saved states %d and things dropped %d over %d frames, most replay work in a frame %.1f ms, longest frame with replay work %.1f ms, longest frame %.1f ms over %d frames (from the level's build to 1 s after the view came back), %d bad guys a frame"),
		Why, JoinReplayPlaced, JoinReplayPlaceFrames, JoinReplayBodies, JoinReplayRead, JoinReplayDropsMade, JoinReplayReadFrames, JoinReplayMaxWorkMs, JoinReplayMaxWorkFrameMs, JoinReplayMaxFrameMs, JoinReplayFramesMeasured, OljrPerFrame());
	if (AOutlawsLevelActor* Lv = FindLevel()) Lv->DeferredDrops.Reset();
	JoinReplayDropNext = JoinReplayDropsMade = 0;
	JoinReplayQueue.Reset();
	JoinReplayObj.Reset();
	JoinReplayStep = 0;
	JoinReplayTailUntil = 0.0;
}

void UOutlawsCoop::TickJoinReplay(double Now)
{
	if (JoinReplayStep == 0) return;
	// the frames from the level's build on, each measured at the next tick (the work's own frame included)
	if (JoinReplayBuiltFrame == 0 && bLevelBuilt)
	{
		JoinReplayBuiltFrame = GFrameCounter;
		JoinReplayBuiltAt = Now;
	}
	if (JoinReplayBuiltFrame != 0 && GFrameCounter > JoinReplayBuiltFrame && JoinReplayLastTick > 0.0)
	{
		const double Ms = (Now - JoinReplayLastTick) * 1000.0;
		JoinReplayMaxFrameMs = FMath::Max(JoinReplayMaxFrameMs, Ms);
		// (the frame just gone had replay work in it: the longest of those, apart from what else the level's start does)
		if (JoinReplayWorkFrame == GFrameCounter - 1) JoinReplayMaxWorkFrameMs = FMath::Max(JoinReplayMaxWorkFrameMs, Ms);
		++JoinReplayFramesMeasured;
	}
	JoinReplayLastTick = Now;
	if (!IsClient() || StartedSerial != JoinReplayEpoch)
	{
		EndJoinReplay(TEXT("left before it was done"));
		return;
	}
	AOutlawsLevelActor* L = FindLevel();
	const bool bSpread = OljrPerFrame() > 0 && CVarOljrWarm.GetValueOnGameThread() > 0.f;
	if (JoinReplayStep <= 2 && bSpread && bLevelBuilt && L)
	{
		if (Now - JoinReplayBuiltAt < 10.0)
		{
			// black while the world goes back (asked each frame: the level's own start fade must not lift it)
			L->FadeCamera(1.f, 1.f, 0.f, true);
			bJoinReplayFade = true;
		}
		else if (bJoinReplayFade)
		{
			// (never black for long: a snapshot that waits on something else shows the level as before)
			L->FadeCamera(1.f, 0.f, 0.5f, false);
			bJoinReplayFade = false;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP join replay: still not put back 10 s after the level was built (step %d): the view shown"), JoinReplayStep);
		}
	}
	if (JoinReplayStep == 3)
	{
		if (JoinReplayWarmUntil <= 0.0) JoinReplayWarmUntil = Now + (bJoinReplayFade ? CVarOljrWarm.GetValueOnGameThread() : 0.0);
		if (Now < JoinReplayWarmUntil)
		{
			if (L && bJoinReplayFade) L->FadeCamera(1.f, 1.f, 0.f, true);
			return;
		}
		if (bJoinReplayFade && L) L->FadeCamera(1.f, 0.f, 0.5f, false);
		bJoinReplayFade = false;
		JoinReplayStep = 4;
		JoinReplayTailUntil = Now + 1.0;
		return;
	}
	if (JoinReplayStep == 4 && Now >= JoinReplayTailUntil) EndJoinReplay(TEXT("done"));
}

FString UOutlawsCoop::JoinSummary() const
{
	return FString::Printf(TEXT("late joins sent %d, applied %d"), JoinsSent, JoinsApplied);
}

// ---- R8: a dropped client comes back -------------------------------------------------------------------------------------

namespace
{
	constexpr double ReconnectEvery = 5.0, ReconnectFor = 120.0;
}

void UOutlawsCoop::StartReconnect(const FString& Why)
{
	ReconnectTo = ConnectingPort.IsEmpty() ? ConnectingTo : FString::Printf(TEXT("%s:%s"), *ConnectingTo, *ConnectingPort);
	const double Now = FPlatformTime::Seconds();
	ReconnectUntil = Now + ReconnectFor;
	NextReconnectAt = Now + 2.0;         // (the engine takes this machine home first)
	ReconnectTries = 0;
	bBackInPending = true;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP connection lost (%s): trying %s again every %.0f s for %.0f s"), *Why, *ReconnectTo, ReconnectEvery, ReconnectFor);
	EndSession(TEXT("connection lost, trying to reconnect"), false);
	PendingToast = TEXT("Connection lost, trying to reconnect ...");
}

void UOutlawsCoop::TickReconnect(double Now)
{
	if (ReconnectTo.IsEmpty()) return;
	AOutlawsLevelActor* L = FindLevel();
	AOutlawsPlayer* P = LocalPlayer();
	if (Mode != EOutlawsCoopMode::None)
	{
		// (a try on its way; joined, OnLevelBeginPlay ends the reconnect and the late join brings "Back in". The world says
		// "client" while the engine is still only trying, so that is no sign of it)
		const UWorld* W = CurrentWorld();
		if (bReconnectAttempt && Now - JoinStartedAt > 12.0)
		{
			// (no answer: the engine's own wait is longer)
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP reconnect try %d: no answer in 12 s"), ReconnectTries);
			if (GEngine && W) GEngine->CancelPending(const_cast<UWorld*>(W));
			const FString Keep = ReconnectTo;
			ResetSession();
			ReconnectTo = Keep;
		}
		return;
	}
	// the player went off to do something else (a single-player game started): no more tries
	// (not while the engine takes this machine home after the drop: the level there is the title menu's once it is up)
	if (L && !L->IsFrontEnd() && !L->bNeedPlace && Now > ReconnectUntil - ReconnectFor + 8.0 && !(P && P->IsMenuOpen()))
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP reconnect to %s given up: a single-player game started"), *ReconnectTo);
		ReconnectTo.Reset();
		bBackInPending = false;
		return;
	}
	if (Now > ReconnectUntil)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP reconnect to %s given up after %d tries"), *ReconnectTo, ReconnectTries);
		if (P && P->IsMenuOpen()) P->CoopToast(FString::Printf(TEXT("Could not reconnect to %s"), *ReconnectTo));
		else PendingToast = FString::Printf(TEXT("Could not reconnect to %s"), *ReconnectTo);
		ReconnectTo.Reset();
		bBackInPending = false;
		return;
	}
	if (Now - LastReconnectToastAt > 4.0 && P && P->IsMenuOpen())
	{
		LastReconnectToastAt = Now;
		P->CoopToast(FString::Printf(TEXT("Connection lost, trying to reconnect (%.0f s) ..."), ReconnectUntil - Now));
	}
	if (Now < NextReconnectAt || !L || !L->IsFrontEnd()) return;
	NextReconnectAt = Now + ReconnectEvery;
	++ReconnectTries;
	const FString To = ReconnectTo;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP reconnect try %d to %s"), ReconnectTries, *To);
	Join(To, false);
	ReconnectTo = To;                    // (Join's ResetSession keeps it: it is not session state)
	bReconnectAttempt = true;
	bEnterPressed = true;                // (REQ-MPLOBBY: it had entered before the drop: back into the game, not the lobby)
}

// ---- R8's dropout tests ---------------------------------------------------------------------------------------------------
// outage (both instances): five seconds into the level the client's packets all lost for 20 s (net emulation), then none:
//   both sides time out, the host plays on, the client tries again and comes back with its kit.
// rejoin (the host; the client is killed by the runner and started again): the restarted client joins late with the kit the
//   host last heard from it.
// hostkill (the client; the host is killed by the runner): the client is back at its title menu with the reason.

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TickDropoutTest(double Now)
{
	AOutlawsLevelActor* L = FindLevel();
	AOutlawsPlayer* P = LocalPlayer();
	if (!L || bExitRequested) return;
	if (IsClient() || (Mode == EOutlawsCoopMode::None && !bLaunchHost))
	{
		// the client's side (the outage ends on its time, whatever state the connection is in)
		if (TestOnly == TEXT("outage") && DropStep == 1 && Now > DropAt)
		{
			DropStep = 2;
			GEngine->Exec(CurrentWorld(), TEXT("NetEmulation.PktLoss 0"));
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: dropout: the outage over (packets get through again); %s"), ReconnectTo.IsEmpty() ? TEXT("not reconnecting") : TEXT("reconnecting"));
		}
		if (TestOnly == TEXT("hostkill"))
		{
			if ((Mode == EOutlawsCoopMode::None || bReconnectAttempt) && EndedAt > 0.0 && P && P->IsMenuOpen() && L->IsFrontEnd() && DropStep == 0)
			{
				DropStep = 1;
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: hostkill: back at the title menu %.1f s after the session ended (page %s); the message: %s"), Now - EndedAt,
					*P->CoopMenuPage(), PendingToast.IsEmpty() ? TEXT("(shown)") : *PendingToast);
				DropAt = Now + 3.0;
			}
			if (DropStep == 1 && Now > DropAt)
			{
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: hostkill: reconnect %s"), ReconnectTo.IsEmpty() ? TEXT("not trying") : *FString::Printf(TEXT("trying %s (%d tries)"), *ReconnectTo, ReconnectTries));
				Summary(TEXT("done"));
				QuitForTest();
			}
			return;
		}
		if (!bLevelBuilt || !P || L->bNeedPlace) return;
		if (JoinsApplied > DropJoins)
		{
			// in again (a late join applied): the kit then, a moment later
			DropJoins = JoinsApplied;
			DropAt = Now + 2.0;
			DropStep = 10;
			return;
		}
		if (DropStep == 10 && Now > DropAt)
		{
			DropStep = 11;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: dropout: back in, the kit: %s"), *TestFingerprint(L));
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: latejoin: client after the join: %s"), *TestLateJoinPrint(L));
			return;
		}
		if (DropStep == 0)
		{
			if (DropAt == 0.0) { DropAt = Now + 5.0; return; }
			if (Now < DropAt) return;
			DropStep = 1;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: dropout: the kit before: %s"), *TestFingerprint(L));
			if (TestOnly == TEXT("outage"))
			{
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: dropout: every packet lost from now for 20 s"));
				GEngine->Exec(CurrentWorld(), TEXT("NetEmulation.PktLoss 100"));
				DropAt = Now + 20.0;
			}
			return;
		}
		return;
	}
	if (IsHost())
	{
		// the host's side: carries on alone, takes the player back, then both quit
		const AOutlawsCoopLink* CL = GetLink(1);
		if (CL && CL->BuiltSerial == StartedSerial && JoinsSent > DropJoins)
		{
			DropJoins = JoinsSent;
			DropAt = Now + 6.0;
			DropStep = 20;
		}
		if (DropStep == 20 && Now > DropAt)
		{
			DropStep = 21;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: dropout: the player is back (%d late joins); host after the join: %s"), JoinsSent, *TestLateJoinPrint(L));
			TestSay(TEXT("quit"));
			DropAt = Now + 1.5;
		}
		if (DropStep == 21 && Now > DropAt)
		{
			DropStep = 22;
			Summary(TEXT("done"));
			QuitForTest();
		}
	}
}
#endif // WITH_OUTLAWS_TESTS

// ---- its test (-OLCoopAuto -OLCoopTest=latejoin | latejoinend; the client with -OLCoopJoinDelay=<s>) -----------------------

#if WITH_OUTLAWS_TESTS
FString UOutlawsCoop::TestLateJoinPrint(AOutlawsLevelActor* L) const
{
	// the shared world as this machine has it: the dead bad guys by index, the pickups gone, the doors
	TArray<FString> Dead;
	const TArray<TWeakObjectPtr<AOutlawsEnemy>>& List = L->GetEnemies();
	for (int32 i = 0; i < List.Num(); ++i) if (List[i].IsValid() && List[i]->IsDead()) Dead.Add(FString::FromInt(i));
	TArray<int32> Have;
	for (const AOutlawsLevelActor::FPickup& Pk : L->Pickups) if (Pk.Actor.IsValid() && Pk.CoopId >= 0 && Pk.CoopId < 50000) Have.Add(Pk.CoopId);
	Have.Sort();
	FString Doors;
	if (TestLjDoor >= 0 && L->Inf && L->Inf->Elevators.IsValidIndex(TestLjDoor))
	{
		Doors = FString::Printf(TEXT("%.0f"), double(L->Inf->Elevators[TestLjDoor].Current));
	}
	return FString::Printf(TEXT("enemies %d, dead [%s]; level pickups left %d, pickup %d %s; door %d at %s"), List.Num(), *FString::Join(Dead, TEXT(",")), Have.Num(),
		TestLjPickup, Have.Contains(TestLjPickup) ? TEXT("THERE") : TEXT("gone"), TestLjDoor, Doors.IsEmpty() ? TEXT("-") : *Doors);
}

void UOutlawsCoop::TickHostLateJoinTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	AOutlawsCoopSession* S = GetSession();
	AOutlawsCoopLink* CL = GetLink(1);
	if (!S || Now < TestAt) return;
	const bool bEnd = TestOnly == TEXT("latejoinend");
	auto Hash = []() { return LexToString(FMD5Hash::HashFile(*AOutlawsLevelActor::SavePath(-1))); };
	auto WorldClock = [this]() { const UWorld* W = CurrentWorld(); return W ? W->GetTimeSeconds() : 0.0; };
	switch (TestStep)
	{
	case 0:
		if (L->bNeedPlace || L->IsRebuildPending()) return;
		TestLjAutoHash = Hash();
		L->bInvincible = true;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: latejoin: the host plays %s alone: extra bad guys for %d players, %d listed; nobody connected %d"), *L->LevelName, S->Players,
			S->Extras.Num(), S->bClientConnected ? 0 : 1);
		if (bEnd)
		{
			// the train's end at once: the late player connects in the fade or the movie and must wait for the canyon
			L->CompleteLevel(TEXT("co-op test"));
			TestStep = 520;
			TestAt = Now + 1.0;
			return;
		}
		// 1: alone, the menu stops the world as single player's does
		P->CoopOpenMenu();
		TestLjWalkerAt = FVector(WorldClock(), 0.0, 0.0);
		TestStep = 501;
		TestAt = Now + 1.5;
		return;
	case 501:
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: latejoin: alone, menu open 1.5 s: the world's clock moved %.4f s (dilation %.4f)"), WorldClock() - TestLjWalkerAt.X,
			UGameplayStatics::GetGlobalTimeDilation(P));
		P->CoopCloseMenu();
		TestStep = 502;
		TestAt = Now + 1.0;
		return;
	case 502:
	{
		// 2: two bad guys killed
		TestLjEnemyA = TestPickEnemy(L, P->GetActorLocation(), -1, true);
		TestLjEnemyB = TestPickEnemy(L, P->GetActorLocation(), TestLjEnemyA, true);
		for (const int32 Ix : { TestLjEnemyA, TestLjEnemyB })
		{
			AOutlawsEnemy* E = JoinEnemyAt(L, Ix);
			for (int32 k = 0; E && k < 60 && !E->IsDead(); ++k) E->TakeHit(50.f, E->GetActorLocation(), FVector(0.0, 0.0, -1.0), E->GetActorLocation() + FVector(0.0, 0.0, 200.0));
		}
		const AOutlawsEnemy* A = JoinEnemyAt(L, TestLjEnemyA);
		const AOutlawsEnemy* B = JoinEnemyAt(L, TestLjEnemyB);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: latejoin: the host killed enemies %d and %d (dead %d %d)"), TestLjEnemyA, TestLjEnemyB, A && A->IsDead() ? 1 : 0, B && B->IsDead() ? 1 : 0);
		if (TestOnly == TEXT("latejoinmass"))
		{
			// (Q2 step 4) a level with many bodies for the late joiner to put back (dotard's townex: 121 of 265): three in four
			int32 Killed = 0;
			const TArray<TWeakObjectPtr<AOutlawsEnemy>>& All = L->GetEnemies();
			for (int32 i = 0; i < All.Num(); ++i)
			{
				AOutlawsEnemy* E = All[i].Get();
				if (!E || E->bBoss || E->IsDead() || i % 4 == 3) continue;
				for (int32 k = 0; k < 60 && !E->IsDead(); ++k) E->TakeHit(50.f, E->GetActorLocation(), FVector(0.0, 0.0, -1.0), E->GetActorLocation() + FVector(0.0, 0.0, 200.0));
				Killed += E->IsDead() ? 1 : 0;
			}
			int32 Dead = 0;
			for (const TWeakObjectPtr<AOutlawsEnemy>& E : All) Dead += E.IsValid() && E->IsDead() ? 1 : 0;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: latejoin: mass: the host killed %d more, %d of %d dead"), Killed, Dead, All.Num());
		}
		// 3: a pickup taken (walked over)
		TestTried.Reset();
		TestWalkOver(L, P, -1);
		TestLjPickup = TestPickup;
		TestStep = 503;
		TestAt = Now + 1.0;
		return;
	}
	case 503:
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: latejoin: the host took pickup %d: %s"), TestLjPickup, PickupIndex(L, TestLjPickup) < 0 ? TEXT("taken") : TEXT("STILL THERE"));
		// 4: a door pushed (the hideout's hall door, else the first that swings)
		TestLjDoor = INDEX_NONE;
		if (L->Inf)
		{
			for (int32 i = 0; i < L->Inf->Elevators.Num(); ++i)
			{
				if (L->Inf->Elevators[i].Name.Equals(TEXT("HALL1"), ESearchCase::IgnoreCase)) TestLjDoor = i;
			}
			for (int32 i = 0; TestLjDoor < 0 && i < L->Inf->Elevators.Num(); ++i) if (L->Inf->Elevators[i].IsMorph()) TestLjDoor = i;
		}
		TestLogElevator(L, TestLjDoor, TEXT("before the host's push"));
		TestGoToDoor(L, P, TestLjDoor);
		TestStep = 504;
		TestAt = Now + 0.8;
		return;
	}
	case 504:
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: latejoin: the host pushes sector %d: %s"), TestDoorSector, L->PushDoorSector(TestDoorSector) ? TEXT("taken") : TEXT("not taken"));
		TestStep = 505;
		TestAt = Now + 4.0;
		return;
	case 505:
		// 5: twenty metres on
		TestLogElevator(L, TestLjDoor, TEXT("after the host's push"));
		TestWalkFrom = P->GetActorLocation();
		TestWalkOpen(P, 4.5f);
		TestStep = 506;
		TestAt = Now + 5.0;
		return;
	case 506:
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: latejoin: the host moved %.0f cm; waiting for the late player"), FVector::Dist2D(TestWalkFrom, P->GetActorLocation()));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: latejoin: host before the join: %s"), *TestLateJoinPrint(L));
		TestStep = 507;
		return;
	case 507:
		// 6: the late player's level built (plus a moment for its snapshot to be put back)
		if (!CL || CL->BuiltSerial != StartedSerial) return;
		TestStep = 508;
		TestAt = Now + 4.0;
		return;
	case 508:
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: latejoin: host after the join: %s"), *TestLateJoinPrint(L));
		TestSay(TEXT("lj:print"), TestLjDoor * 100000 + FMath::Max(TestLjPickup, 0));
		const FVector Them = CL ? FVector(CL->Pose.Body) : FVector::ZeroVector;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: latejoin: the late player stands %.0f cm from the host"), CL && CL->Pose.Seq > 0 ? FVector::Dist(Them, P->GetActorLocation()) : -1.f);
		// (user 2026-10-03) the map in the right hand: the partner's marker on it
		P->CoopTestHoldMap(1);
		// 7: with a second player in, the menu stops nothing
		P->CoopOpenMenu();
		TestLjWalkerAt = FVector(WorldClock(), 0.0, 0.0);
		TestStep = 509;
		TestAt = Now + 1.5;
		return;
	}
	case 509:
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: latejoin: joined, menu open 1.5 s: the world's clock moved %.4f s (dilation %.4f)"), WorldClock() - TestLjWalkerAt.X,
			UGameplayStatics::GetGlobalTimeDilation(P));
		P->CoopCloseMenu();
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: latejoin: the held map shows the partner: %s"), P->CoopMapPartnerState().IsEmpty() ? TEXT("NOTHING") : *P->CoopMapPartnerState());
		Shot(TEXT("ol_map_partner"));
		TestStep = 510;
		TestAt = Now + 0.5;
		return;
	case 510:
	{
		// 8: the autosaves: the co-op one written, single player's untouched
		const AOutlawsLevelActor::FSaveInfo Co = AOutlawsLevelActor::ReadSaveInfo(-2);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: latejoin: coop_autosave.json %s; autosave.json unchanged %s"), Co.bEmpty ? TEXT("NOT WRITTEN") : *FString::Printf(TEXT("written (%s)"), *Co.Name),
			Hash() == TestLjAutoHash ? TEXT("yes") : TEXT("NO"));
		TestSay(TEXT("quit"));
		TestStep = 100;
		TestAt = Now + 1.5;
		return;
	}
	case 520:
		// (the end test) waiting for the canyon and the late player's build of it; the movie skipped once the late player
		// has waited in it a few seconds (an offscreen test's movie may not end in the test's time)
		if (L->IsMoviePlaying() && CL && CL->JoinState == JoinHostBusy)
		{
			if (TestLjWalkerAt.Y == 0.0) TestLjWalkerAt.Y = Now;
			if (Now - TestLjWalkerAt.Y > 4.0)
			{
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: latejoin: the late player has waited in the movie: skipped"));
				L->SkipMovie();
				TestAt = Now + 2.0;
			}
			return;
		}
		if (!CL || CL->BuiltSerial != StartedSerial || !bLevelBuilt) return;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: latejoin: the late player joined at %s (serial %d)"), *BuiltLevel, StartedSerial);
		TestStep = 521;
		TestAt = Now + 3.0;
		return;
	case 521:
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: latejoin: host after the join: %s"), *TestLateJoinPrint(L));
		TestSay(TEXT("lj:print"), -100000);
		TestSay(TEXT("quit"));
		TestStep = 100;
		TestAt = Now + 1.5;
		return;
	case 100:
		Summary(TEXT("done"));
		QuitForTest();
		TestStep = 101;
		return;
	default:
		return;
	}
}

void UOutlawsCoop::OnLateJoinTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	if (Step == TEXT("print"))
	{
		TestLjDoor = Arg >= 0 ? Arg / 100000 : -1;
		TestLjPickup = Arg >= 0 ? Arg % 100000 : -1;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: latejoin: client after the join: %s"), *TestLateJoinPrint(L));
	}
}
#endif // WITH_OUTLAWS_TESTS
