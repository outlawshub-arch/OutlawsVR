// BUG-COOPTIMEOUT (10 Oct co-op, DOTARD's Quest 2 logs, tools/olue/out/micstatic/dotard_quest_logs/COOPTIMEOUT.md): all five
// of his drops were the Quest pausing the app (its menu, Guardian, the headset off): the host timed him out 12 s after
// each pause with no network loss.  Only one of the five came back by itself: the host had already let his place go, and
// his client learned of it only 12 s after the app came back (its own timeout), then waited 2 s more before trying; in
// that half minute of a frozen game he closed the app or it paused again, and four times he came back by a relaunch.
// Now:
// - the client says "pausing" as the app goes (ApplicationWillEnterBackground, flushed at once) and "back" as it returns;
// - the host holds that player's place (its link, pawn, kit, keys, stats) for ol.CoopPauseGrace (120 s) instead of
//   timing it out: its connection is kept from timing out and sent to slowly meanwhile; its puppet stands still with
//   "(paused)" on its plate; the bad guys leave it be and the idle rule counts it idle;
// - a connection that goes quiet with no notice times out as before (the engine's 12 s);
// - back, the client asks the host; no answer in ol.CoopResumeAckSeconds (4 s): the connection is stale and it reconnects
//   to the same host at once, taking its place back through the late join; a reconnect that was under way when the app
//   went is tried again at once, with its full two minutes (the time away is not counted against it).
// Story co-op and the matches alike; nothing here runs in single player (every entry is behind a session).
// Its tests: -OLCoopAuto -OLCoopTest=pausehold | resumerejoin, the client with -OLCoopPauseSec=<s> (run_pausehold.ps1).
// BUG-HOSTPAUSE (11 Oct): the host's own app pausing (a Quest host's headset off, its system menu) timed every client out
// after 12 s and ended the session.  Now:
// - the host says "host pausing" to every client as its app goes (flushed at once) and "back" as it returns;
// - each client holds the game for the host's ol.CoopPauseGrace: its connection kept from timing out and sent to slowly
//   (as the host does for a paused client), its world held still as behind the single-player menu (the bad guys, the
//   puppets, the timers; this player's head and hands follow the headset, no walking, no guns), "Host paused" on its view;
// - back, the world runs again in the same connection; a hold that ran out ends in the normal timeout, and the client then
//   reconnects to the same host by itself (R8's reconnect, the late join brings its kit back);
// - a host gone quiet with no notice times out as before.
// Its tests: -OLCoopTest=hostpause | hoststall, the host with -OLCoopPauseSec=<s> (run_hostpause.ps1).
#include "OutlawsCoop.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsRemotePlayer.h"
#include "Engine/World.h"
#include "Engine/NetDriver.h"
#include "Engine/NetConnection.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/ThreadHeartBeat.h"
#include "Misc/CoreDelegates.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/CharacterMovementComponent.h"

TAutoConsoleVariable<float> CVarOLCoopPauseGrace(TEXT("ol.CoopPauseGrace"), 120.f,
	TEXT("Co-op host: how long a player whose app said it was pausing keeps its place before the normal timeout (seconds)."));
TAutoConsoleVariable<float> CVarOLCoopResumeAckSeconds(TEXT("ol.CoopResumeAckSeconds"), 4.f,
	TEXT("Co-op client: back from a pause, how long to wait for the host's answer before reconnecting (seconds)."));
TAutoConsoleVariable<int32> CVarOLCoopHeldNetSpeed(TEXT("ol.CoopHeldNetSpeed"), 1500,
	TEXT("Co-op host: the net speed (bytes a second) a paused player's connection is held to, so the packets sent while it cannot answer stay well inside the sequence window (0: unchanged)."));

namespace
{
	constexpr double PauseReconnectWindow = 120.0;      // (as R8's reconnect: OutlawsCoopJoin.cpp)
	const TCHAR* const PauseHomeMap = TEXT("/Game/Outlaws/Maps/L_Runtime");

	UNetConnection* PauseLinkConnection(const AOutlawsCoopLink* K)
	{
		const APlayerController* PC = K ? Cast<APlayerController>(K->GetOwner()) : nullptr;
		return PC ? PC->GetNetConnection() : nullptr;
	}

	float PauseGraceSeconds()
	{
		return FMath::Max(15.f, CVarOLCoopPauseGrace.GetValueOnGameThread());
	}

	const TCHAR* const HostPausedLine = TEXT("Host paused");

	FString PauseNameOf(const AOutlawsCoopLink* K)
	{
		if (!K) return TEXT("a player");
		return K->PlayerName.IsEmpty() ? FString(UOutlawsCoop::WhoWord(K->PlayerIndex)) : K->PlayerName;
	}
}

// ---- the link's messages --------------------------------------------------------------------------------------------------

void AOutlawsCoopLink::ServerAppPause_Implementation(bool bPausing)
{
	if (!Admit(EOLGate::AppPause, true)) return;
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnPauseNotice(PlayerIndex, bPausing);
}

void AOutlawsCoopLink::ClientResumeAck_Implementation(int32 HeldSeconds)
{
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnResumeAck(HeldSeconds);
}

void AOutlawsCoopLink::ClientHostPause_Implementation(bool bPausing, float GraceSeconds)
{
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnHostPauseNotice(bPausing, GraceSeconds);
}

// ---- the app's pause and return -------------------------------------------------------------------------------------------

void UOutlawsCoop::BindAppPause()
{
	// (on Android both come from the game thread's app events: the backgrounding callback runs before the thread blocks.
	// Desktop builds never send them, so a window losing focus is no pause)
	if (!AppPauseHandle.IsValid()) AppPauseHandle = FCoreDelegates::ApplicationWillEnterBackgroundDelegate.AddUObject(this, &UOutlawsCoop::OnAppWillPause);
	if (!AppResumeHandle.IsValid()) AppResumeHandle = FCoreDelegates::ApplicationHasEnteredForegroundDelegate.AddUObject(this, &UOutlawsCoop::OnAppResumed);
}

void UOutlawsCoop::UnbindAppPause()
{
	FCoreDelegates::ApplicationWillEnterBackgroundDelegate.Remove(AppPauseHandle);
	FCoreDelegates::ApplicationHasEnteredForegroundDelegate.Remove(AppResumeHandle);
	AppPauseHandle.Reset();
	AppResumeHandle.Reset();
}

void UOutlawsCoop::OnAppWillPause()
{
	AppWillPause(TEXT("the app goes to the background"));
}

void UOutlawsCoop::OnAppResumed()
{
	AppResumed(TEXT("the app is in the foreground again"));
}

void UOutlawsCoop::AppWillPause(const TCHAR* Why)
{
	if (bAppPausedHere) return;
	bAppPausedHere = true;
	AppPausedAt = FPlatformTime::Seconds();
	bAwaitAck = false;
	UWorld* W = CurrentWorld();
	if (Mode == EOutlawsCoopMode::Host && W && W->GetNetMode() == NM_ListenServer)
	{
		// (BUG-HOSTPAUSE) the players told before the thread blocks: they hold the game instead of timing this host out
		const float Grace = PauseGraceSeconds();
		const int32 Told = TellClientsHostPause(true, Grace);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pause: this host's app is pausing (%s): %d players told, they hold the game for %.0f s"), Why, Told, Grace);
		return;
	}
	if (Mode != EOutlawsCoopMode::Client || !W || W->GetNetMode() != NM_Client)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pause: this app is pausing (%s); %s"), Why, Mode == EOutlawsCoopMode::None ? TEXT("no session") : TEXT("not a joined client: nothing to tell"));
		return;
	}
	AOutlawsCoopLink* Mine = GetLink(MyIndex());
	if (!Mine)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pause: this app is pausing (%s); this player's link has not come: the host is not told"), Why);
		return;
	}
	Mine->ServerAppPause(true);
	// (the thread blocks once this returns: out now, not at the end of the frame)
	if (UNetDriver* D = W->GetNetDriver(); D && D->ServerConnection) D->ServerConnection->FlushNet(true);
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pause: this app is pausing (%s): the host told, it holds this player's place"), Why);
}

void UOutlawsCoop::AppResumed(const TCHAR* Why)
{
	const bool bWasPaused = bAppPausedHere;
	bAppPausedHere = false;
	const double Now = FPlatformTime::Seconds();
	AppResumedAt = Now;
	ResumeAckAt = 0.0;
	const FString Away = bWasPaused ? FString::Printf(TEXT("after %.1f s"), Now - AppPausedAt) : FString(TEXT("(no pause was said)"));
	if (!ReconnectTo.IsEmpty())
	{
		// a reconnect under way when the app went (the 12:41 drop): tried again at once, its full time from now
		ReconnectUntil = Now + PauseReconnectWindow;
		NextReconnectAt = Now;
		if (Mode != EOutlawsCoopMode::None && bReconnectAttempt) JoinStartedAt = Now - 100.0;      // (the try in flight dropped next tick)
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP resume: this app is back %s (%s): the reconnect to %s goes again at once, %.0f s more"), *Away, Why, *ReconnectTo, PauseReconnectWindow);
		return;
	}
	UWorld* W = CurrentWorld();
	if (Mode == EOutlawsCoopMode::Host && W && W->GetNetMode() == NM_ListenServer)
	{
		// (BUG-HOSTPAUSE) the time away is no silence of the players': they held, and what they sent meanwhile may be lost.
		// Past the grace they have timed out and come back by their reconnect, so their stale connections time out here
		UNetDriver* D = W->GetNetDriver();
		int32 Kept = 0;
		if (bWasPaused && D && Now - AppPausedAt < PauseGraceSeconds())
		{
			for (UNetConnection* Conn : D->ClientConnections)
			{
				if (!Conn) continue;
				Conn->LastReceiveRealtime = FMath::Max(Conn->LastReceiveRealtime, Now);
				++Kept;
			}
		}
		const int32 Told = TellClientsHostPause(false, 0.f);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP resume: this host's app is back %s (%s): %d players told, %d connections kept; play goes on"), *Away, Why, Told, Kept);
		return;
	}
	if (Mode != EOutlawsCoopMode::Client || !W || W->GetNetMode() != NM_Client)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP resume: this app is back %s (%s); %s"), *Away, Why, Mode == EOutlawsCoopMode::None ? TEXT("no session") : TEXT("not a joined client"));
		return;
	}
	UNetDriver* D = W->GetNetDriver();
	UNetConnection* Conn = D ? D->ServerConnection : nullptr;
	// (the time away is no silence of the host's: the engine's timeout counts from now, and the host's answer decides)
	if (Conn) Conn->LastReceiveRealtime = FMath::Max(Conn->LastReceiveRealtime, Now);
	AOutlawsCoopLink* Mine = GetLink(MyIndex());
	if (Mine)
	{
		Mine->ServerAppPause(false);
		if (Conn) Conn->FlushNet(true);
	}
	bAwaitAck = true;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP resume: this app is back %s (%s): asked the host whether it kept this player's place (%.0f s for its answer)"), *Away, Why,
		CVarOLCoopResumeAckSeconds.GetValueOnGameThread());
}

void UOutlawsCoop::OnResumeAck(int32 HeldSeconds)
{
	if (!bAwaitAck) return;
	bAwaitAck = false;
	ResumeAckAt = FPlatformTime::Seconds();
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP resume: the host kept this player's place (held %d s; its answer %.1f s after the return): carrying on"), HeldSeconds,
		ResumeAckAt - AppResumedAt);
	if (AOutlawsLevelActor* L = FindLevel(); L && !L->IsFrontEnd()) L->Notify(TEXT("Back in the game"), 2.f);
}

// ---- the host -------------------------------------------------------------------------------------------------------------

bool UOutlawsCoop::IsPauseHeld(int32 Who) const
{
	return IsHost() && Who > 0 && Who < MaxPlayers && PauseHolds[Who].Since > 0.0 && !PauseHolds[Who].bOver;
}

void UOutlawsCoop::OnPauseNotice(int32 Who, bool bPausing)
{
	if (!IsHost() || Who <= 0 || Who >= MaxPlayers) return;
	AOutlawsCoopLink* K = GetLink(Who);
	if (!K) return;
	const double Now = FPlatformTime::Seconds();
	FPauseHold& H = PauseHolds[Who];
	if (!bPausing)
	{
		const double Held = H.Since > 0.0 ? Now - H.Since : 0.0;
		if (H.Since > 0.0) EndPauseHold(Who, TEXT("its app is back"));
		else UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pause: player %d (%s) says its app is back (no hold here)"), Who, *PauseNameOf(K));
		K->ClientResumeAck(int32(Held + 0.5));
		return;
	}
	const float Grace = FMath::Max(15.f, CVarOLCoopPauseGrace.GetValueOnGameThread());
	if (H.Since <= 0.0)
	{
		H = FPauseHold();
		H.Since = Now;
		H.NextLogAt = Now + 20.0;
		if (UNetConnection* Conn = PauseLinkConnection(K))
		{
			// (a frozen client acknowledges nothing: at the host's frame rate two minutes of packets would run past the
			// sequence window, after which neither side accepts the other's; held slow, they stay well inside it)
			H.SavedSpeed = Conn->CurrentNetSpeed;
			H.PacketsAtStart = Conn->OutTotalPackets;
			const int32 Slow = CVarOLCoopHeldNetSpeed.GetValueOnGameThread();
			if (Slow > 0 && Conn->CurrentNetSpeed > Slow) Conn->CurrentNetSpeed = Slow;
		}
	}
	H.Until = Now + Grace;
	H.bOver = false;
	K->bAppPaused = true;
	K->ForceNetUpdate();
	++Version;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pause: player %d (%s) says its app is pausing: its place held for %.0f s (its pawn, kit, keys and stats kept; the bad guys leave it be)"),
		Who, *PauseNameOf(K), Grace);
}

void UOutlawsCoop::EndPauseHold(int32 Who, const TCHAR* Why)
{
	if (Who <= 0 || Who >= MaxPlayers) return;
	FPauseHold& H = PauseHolds[Who];
	if (H.Since <= 0.0) return;
	AOutlawsCoopLink* K = GetLink(Who);
	UNetConnection* Conn = PauseLinkConnection(K);
	int32 Sent = 0;
	if (Conn)
	{
		Sent = Conn->OutTotalPackets - H.PacketsAtStart;
		if (H.SavedSpeed > 0) Conn->CurrentNetSpeed = H.SavedSpeed;
	}
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pause: player %d (%s): the hold ended after %.1f s (%s); %d packets sent to it meanwhile"), Who, *PauseNameOf(K),
		FPlatformTime::Seconds() - H.Since, Why, Sent);
	H = FPauseHold();
	if (K && IsValid(K) && K->bAppPaused)
	{
		K->bAppPaused = false;
		K->ForceNetUpdate();
	}
	++Version;
}

void UOutlawsCoop::TickPauseHold(double Now)
{
	UWorld* W = CurrentWorld();
	if (Mode == EOutlawsCoopMode::None)
	{
		bAwaitAck = false;
		FMemory::Memzero(LoggedAppPaused);
		for (FPauseHold& H : PauseHolds) H = FPauseHold();
		if (HostHold.Since > 0.0 || bHostPauseFrozen)
		{
			HostHold = FHostPauseHold();
			SetHostPauseFreeze(false);
		}
		return;
	}
	if (IsClient()) TickHostPauseHold(Now);
	if (IsHost())
	{
		UNetDriver* D = W ? W->GetNetDriver() : nullptr;
		for (int32 k = 1; k < MaxPlayers; ++k)
		{
			FPauseHold& H = PauseHolds[k];
			if (H.Since <= 0.0) continue;
			AOutlawsCoopLink* K = GetLink(k);
			if (!K)
			{
				H = FPauseHold();
				continue;
			}
			UNetConnection* Conn = PauseLinkConnection(K);
			if (!H.bOver && Now < H.Until)
			{
				// held: its silence never reaches the engine's timeout before the hold's end (a connection that talks
				// meanwhile, a menu with the game still running, is untouched)
				if (Conn && D)
				{
					const double Keep = FMath::Min(Now, H.Until - double(D->ConnectionTimeout));
					if (Conn->LastReceiveRealtime < Keep) Conn->LastReceiveRealtime = Keep;
				}
				if (Now >= H.NextLogAt)
				{
					H.NextLogAt = Now + 20.0;
					UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pause: player %d (%s) held %.0f s: its link here, its connection open %d, silent %.0f s, %.0f s of the hold left"), k,
						*PauseNameOf(K), Now - H.Since, Conn ? 1 : 0, Conn ? Now - Conn->LastGoodPacketRealtime : -1.0, H.Until - Now);
				}
			}
			else if (!H.bOver)
			{
				H.bOver = true;
				if (Conn && H.SavedSpeed > 0) Conn->CurrentNetSpeed = H.SavedSpeed;
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pause: player %d (%s): its hold of %.0f s is over: the normal timeout from now"), k, *PauseNameOf(K), H.Until - H.Since);
			}
		}
	}
	else if (bAwaitAck && IsClient())
	{
		const float Wait = FMath::Max(1.f, CVarOLCoopResumeAckSeconds.GetValueOnGameThread());
		if (Now - AppResumedAt > Wait)
		{
			bAwaitAck = false;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP resume: no answer from the host in %.0f s after the app's return: the connection is stale, reconnecting now"), Wait);
			StartReconnect(TEXT("no answer from the host after the app's pause"));
			NextReconnectAt = Now;               // (as soon as this machine is home: no menu, no wait)
			if (W && W->GetNetMode() != NM_Standalone) UGameplayStatics::OpenLevel(W, FName(PauseHomeMap), true);
			return;
		}
	}
	// every machine: the others' pauses said as they come and go
	TArray<AOutlawsCoopLink*> Links;
	GetLinks(Links);
	bool bHere[MaxPlayers] = {};
	for (const AOutlawsCoopLink* K : Links)
	{
		const int32 Who = K->PlayerIndex;
		if (Who < 0 || Who >= MaxPlayers) continue;
		bHere[Who] = true;
		if (Who == MyIndex() || K->bAppPaused == LoggedAppPaused[Who]) continue;
		LoggedAppPaused[Who] = K->bAppPaused;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pause: the %s player's app %s (seen here)"), WhoWord(Who), K->bAppPaused ? TEXT("is paused: its place is held") : TEXT("is back"));
		if (AOutlawsLevelActor* L = FindLevel(); L && !L->IsFrontEnd())
		{
			L->Notify(K->bAppPaused ? FString::Printf(TEXT("%s paused: holding their place"), *PauseNameOf(K)) : FString::Printf(TEXT("%s is back"), *PauseNameOf(K)), 3.f);
		}
	}
	for (int32 k = 0; k < MaxPlayers; ++k) if (!bHere[k]) LoggedAppPaused[k] = false;
}

// ---- BUG-HOSTPAUSE: the host's own app pausing ---------------------------------------------------------------------------

int32 UOutlawsCoop::TellClientsHostPause(bool bPausing, float Grace)
{
	UWorld* W = CurrentWorld();
	UNetDriver* D = W ? W->GetNetDriver() : nullptr;
	int32 Told = 0;
	ForClients([&](AOutlawsCoopLink& K)
	{
		if (!PauseLinkConnection(&K)) return;      // (a bot's link has nobody to tell)
		K.ClientHostPause(bPausing, Grace);
		++Told;
	});
	// (pausing, the thread blocks once this returns: out now, not at the end of the frame)
	if (D) for (UNetConnection* Conn : D->ClientConnections) if (Conn) Conn->FlushNet(true);
	return Told;
}

void UOutlawsCoop::SetHostPauseFreeze(bool bOn)
{
	bHostPauseFrozen = bOn;
	if (AOutlawsPlayer* P = LocalPlayer()) P->CoopHostPauseFreeze(bOn);
}

void UOutlawsCoop::OnHostPauseNotice(bool bPausing, float Grace)
{
	if (!IsClient()) return;
	UWorld* W = CurrentWorld();
	UNetDriver* D = W ? W->GetNetDriver() : nullptr;
	UNetConnection* Conn = D ? D->ServerConnection : nullptr;
	const double Now = FPlatformTime::Seconds();
	AOutlawsLevelActor* L = FindLevel();
	if (!bPausing)
	{
		if (HostHold.Since <= 0.0)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP host pause: the host says its app is back (no hold here)"));
			return;
		}
		EndHostPauseHold(TEXT("the host is back"), true);
		if (L && !L->IsFrontEnd()) L->Notify(TEXT("Host is back"), 2.f);
		return;
	}
	Grace = FMath::Clamp(Grace, 15.f, 600.f);
	if (HostHold.Since <= 0.0)
	{
		HostHold = FHostPauseHold();
		HostHold.Since = Now;
		HostHold.NextLogAt = Now + 20.0;
		++HostHoldsSeen;
		if (Conn)
		{
			// (the host acknowledges nothing while it is away: this side's packets held slow stay inside the sequence window)
			HostHold.SavedSpeed = Conn->CurrentNetSpeed;
			HostHold.PacketsAtStart = Conn->OutTotalPackets;
			const int32 Slow = CVarOLCoopHeldNetSpeed.GetValueOnGameThread();
			if (Slow > 0 && Conn->CurrentNetSpeed > Slow) Conn->CurrentNetSpeed = Slow;
		}
	}
	HostHold.Until = Now + Grace;
	HostHold.bOver = false;
	SetHostPauseFreeze(true);
	if (L && !L->IsFrontEnd())
	{
		if (AOutlawsPlayer* P = LocalPlayer()) P->DropMessage(HostPausedLine);
		L->Notify(HostPausedLine, Grace + 30.f);
	}
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP host pause: the host's app is pausing: this player holds the game for %.0f s (the connection kept open, the world held still)"), Grace);
}

void UOutlawsCoop::EndHostPauseHold(const TCHAR* Why, bool bHostBack)
{
	if (HostHold.Since <= 0.0) return;
	const double Now = FPlatformTime::Seconds();
	UWorld* W = CurrentWorld();
	UNetDriver* D = W ? W->GetNetDriver() : nullptr;
	UNetConnection* Conn = D ? D->ServerConnection : nullptr;
	int32 Sent = 0;
	if (Conn)
	{
		Sent = Conn->OutTotalPackets - HostHold.PacketsAtStart;
		if (HostHold.SavedSpeed > 0) Conn->CurrentNetSpeed = HostHold.SavedSpeed;
		// (the host's silence ends with its "back": the engine's timeout counts from now)
		if (bHostBack) Conn->LastReceiveRealtime = FMath::Max(Conn->LastReceiveRealtime, Now);
	}
	HostHoldLasted = float(Now - HostHold.Since);
	if (bHostBack) HostHoldEndedAt = Now;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP host pause: the hold ended after %.1f s (%s); %d packets sent to the host meanwhile"), HostHoldLasted, Why, Sent);
	HostHold = FHostPauseHold();
	SetHostPauseFreeze(false);
	if (AOutlawsPlayer* P = LocalPlayer()) P->DropMessage(HostPausedLine);
}

void UOutlawsCoop::TickHostPauseHold(double Now)
{
	if (HostHold.Since <= 0.0) return;
	UWorld* W = CurrentWorld();
	UNetDriver* D = W ? W->GetNetDriver() : nullptr;
	UNetConnection* Conn = D ? D->ServerConnection : nullptr;
	if (!HostHold.bOver && Now < HostHold.Until)
	{
		// held: the host's silence never reaches the engine's timeout before the hold's end
		if (Conn && D)
		{
			const double Keep = FMath::Min(Now, HostHold.Until - double(D->ConnectionTimeout));
			if (Conn->LastReceiveRealtime < Keep) Conn->LastReceiveRealtime = Keep;
		}
		SetHostPauseFreeze(true);            // (a menu closed meanwhile set the clock back)
		if (Now >= HostHold.NextLogAt)
		{
			HostHold.NextLogAt = Now + 20.0;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP host pause: held %.0f s: the connection open %d, the host silent %.0f s, %.0f s of the hold left"), Now - HostHold.Since,
				Conn ? 1 : 0, Conn ? Now - Conn->LastGoodPacketRealtime : -1.0, HostHold.Until - Now);
		}
	}
	else if (!HostHold.bOver)
	{
		// (the world stays still until the timeout that follows ends the session; the reconnect then brings this player back)
		HostHold.bOver = true;
		if (Conn && HostHold.SavedSpeed > 0) Conn->CurrentNetSpeed = HostHold.SavedSpeed;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP host pause: the hold of %.0f s is over: the normal timeout from now, then the reconnect"), HostHold.Until - HostHold.Since);
	}
}

// ---- its tests (-OLCoopAuto -OLCoopTest=pausehold | resumerejoin; the client with -OLCoopPauseSec=<s>) ---------------------
// pausehold: six seconds into the level the client says it is pausing and its game thread then sleeps for the pause (the
//   Quest's suspend: nothing sent, nothing read), then says it is back.  The host must hold its place throughout (no
//   timeout, its link and puppet there) and the client carry on in the same connection with its kit as before.
// resumerejoin: the same sleep with no notice (a stall the host cannot tell from a lost connection): the host times it out
//   after its 12 s; back, the client finds the connection stale and reconnects at once, taking its place (its kit) back.
// Both: after the join or the return each side logs the lobby's ready words (a late joiner is counted ready).

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TickPauseTest(double Now)
{
	if (IsHostPauseTest())
	{
		TickHostPauseTest(Now);
		return;
	}
	AOutlawsLevelActor* L = FindLevel();
	AOutlawsPlayer* P = LocalPlayer();
	if (!L || bExitRequested) return;
	const bool bStall = TestOnly == TEXT("resumerejoin");
	auto ReadyLine = [this](const TCHAR* Side)
	{
		const AOutlawsCoopSession* S = GetSession();
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: pause: ready as the %s sees it: host ready %d, client ready %d, this player's own %d, players %d"), Side,
			S && S->bHostReady ? 1 : 0, S && S->bClientReady ? 1 : 0, MyReady() ? 1 : 0, S ? S->Connected : 0);
	};
	if (IsClient() || (Mode == EOutlawsCoopMode::None && !bLaunchHost))
	{
		AOutlawsCoopLink* Mine = GetLink(MyIndex());
		if (PauseStep == 0)
		{
			if (!IsClient() || !bLevelBuilt || !P || L->bNeedPlace || !Mine) return;
			if (PauseAt == 0.0) { PauseAt = Now + 6.0; return; }
			if (Now < PauseAt) return;
			PauseKitBefore = TestFingerprint(L);
			PauseJoinsBefore = JoinsApplied;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: pause: the kit before: %s"), *PauseKitBefore);
			ReadyLine(TEXT("client before"));
			PauseStep = 1;
			if (!bStall) AppWillPause(TEXT("test"));
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: pause: the app stopped for %.0f s (the game thread asleep: nothing sent or read)%s"), PauseTestSec,
				bStall ? TEXT(", no notice") : TEXT(", after its notice"));
			{
				FSlowHeartBeatScope NoHang;
				FDisableHitchDetectorScope NoHitch;
				FPlatformProcess::Sleep(PauseTestSec);
			}
			// (-OLCoopDropBacklog: what the host sent meanwhile is lost, as a Quest's suspended socket loses it, so the
			// host's close never comes and only the missing answer tells this machine the connection is stale)
			bPauseDropBacklog = OLFlags::Param(TEXT("OLCoopDropBacklog"));
			if (bPauseDropBacklog) GEngine->Exec(CurrentWorld(), TEXT("NetEmulation.PktIncomingLoss 100"));
			AppResumed(TEXT("test"));
			PauseAt = FPlatformTime::Seconds();
			return;
		}
		if (PauseStep != 1) return;
		if (bPauseDropBacklog)
		{
			// (one frame of the backlog read and dropped, then packets get through again)
			bPauseDropBacklog = false;
			GEngine->Exec(CurrentWorld(), TEXT("NetEmulation.PktIncomingLoss 0"));
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: pause: the backlog from the pause dropped; packets get through again"));
		}
		const double T = FPlatformTime::Seconds();
		if (!bStall)
		{
			if (T - PauseAt < 8.0) return;
			PauseStep = 2;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: pause: after the pause: session kept %d, level built %d, the host's answer %s, reconnect tries %d, late joins applied since %d"),
				IsClient() ? 1 : 0, bLevelBuilt ? 1 : 0, ResumeAckAt > 0.0 ? *FString::Printf(TEXT("%.1f s after the return"), ResumeAckAt - AppResumedAt) : TEXT("NONE"),
				ReconnectTries, JoinsApplied - PauseJoinsBefore);
		}
		else
		{
			if (JoinsApplied <= PauseJoinsBefore || !IsClient() || !bLevelBuilt || !P || L->bNeedPlace || !Mine)
			{
				if (T - PauseAt > 150.0)
				{
					PauseStep = 3;
					UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: pause: NOT back in 150 s after the return (reconnect tries %d)"), ReconnectTries);
					Summary(TEXT("not back in"));
					QuitForTest();
				}
				return;
			}
			if (PauseHoldSeenAt == 0.0)
			{
				PauseHoldSeenAt = T;
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: pause: back in %.1f s after the app's return (reconnect tries %d)"), T - PauseAt, ReconnectTries);
				return;
			}
			if (T - PauseHoldSeenAt < 4.0) return;
			PauseStep = 2;
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: pause: the kit after: %s"), *TestFingerprint(L));
		ReadyLine(TEXT("client after"));
		if (Mine) Mine->ServerTestNote(TEXT("pause done"));
		return;
	}
	if (IsHost() && PauseStep == 20 && Now > PauseAt)
	{
		PauseStep = 21;
		Summary(TEXT("done"));
		QuitForTest();
	}
}

void UOutlawsCoop::OnPauseTestNote(const FString& Note)
{
	if (IsHost() && IsHostPauseTest() && Note == TEXT("pause hostready") && PauseStep == 0)
	{
		PauseStep = 1;
		PauseAt = FPlatformTime::Seconds() + 2.0;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: hostpause: the client is in the level with its kit noted: this host stops in 2 s"));
		return;
	}
	if (!IsHost() || Note != TEXT("pause done") || PauseStep >= 20) return;
	const AOutlawsCoopLink* CL = GetLink(1);
	const AOutlawsCoopSession* S = GetSession();
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: pause: host after: the player's link here %d, its puppet %d, its app paused %d, players who left %d, late joins sent %d"),
		CL ? 1 : 0, PuppetOf(1) ? 1 : 0, CL && CL->bAppPaused ? 1 : 0, PauseHoldLogouts, JoinsSent);
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: pause: ready as the host after sees it: host ready %d, client ready %d, this player's own %d, players %d"),
		S && S->bHostReady ? 1 : 0, S && S->bClientReady ? 1 : 0, MyReady() ? 1 : 0, S ? S->Connected : 0);
	TestSay(TEXT("quit"));
	PauseStep = 20;
	PauseAt = FPlatformTime::Seconds() + 1.5;
}
#endif // WITH_OUTLAWS_TESTS

// ---- BUG-HOSTPAUSE's tests (-OLCoopAuto -OLCoopTest=hostpause | hoststall; the host with -OLCoopPauseSec=<s>) -------------
// hostpause: the client in the level with its kit noted says so; two seconds on the host says it is pausing and its game
//   thread sleeps for the pause, then says it is back.  The client must hold throughout (no timeout, its world still, its
//   movement off), then carry on in the same connection with its kit as before; the host keep its link and puppet.
// hoststall: the same sleep on the host with no notice: the client times out after its 12 s and is back at its title menu
//   with the reason (its reconnect under way), no hold; the host, back, finds it gone and plays on.

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TickHostPauseTest(double Now)
{
	AOutlawsLevelActor* L = FindLevel();
	AOutlawsPlayer* P = LocalPlayer();
	if (!L || bExitRequested) return;
	const bool bStall = TestOnly == TEXT("hoststall");
	auto ReadyLine = [this](const TCHAR* Side)
	{
		const AOutlawsCoopSession* S = GetSession();
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: pause: ready as the %s sees it: host ready %d, client ready %d, this player's own %d, players %d"), Side,
			S && S->bHostReady ? 1 : 0, S && S->bClientReady ? 1 : 0, MyReady() ? 1 : 0, S ? S->Connected : 0);
	};
	auto MoveWord = [](const AOutlawsPlayer* Q)
	{
		const UCharacterMovementComponent* M = Q ? Q->GetCharacterMovement() : nullptr;
		return !M ? TEXT("none") : M->MovementMode == MOVE_None ? TEXT("off") : TEXT("on");
	};
	if (bLaunchHost || IsHost())
	{
		if (!IsHost()) return;
		if (PauseStep == 1 && Now >= PauseAt)
		{
			PauseStep = 2;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: hostpause: the host's app stops for %.0f s (the game thread asleep: nothing sent or read)%s"), PauseTestSec,
				bStall ? TEXT(", no notice") : TEXT(", after its notice"));
			if (!bStall) AppWillPause(TEXT("test"));
			{
				FSlowHeartBeatScope NoHang;
				FDisableHitchDetectorScope NoHitch;
				FPlatformProcess::Sleep(PauseTestSec);
			}
			if (!bStall) AppResumed(TEXT("test"));
			else UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: hostpause: the host's game thread is back from its silent stall"));
			PauseAt = FPlatformTime::Seconds();
			return;
		}
		if (bStall && PauseStep == 2)
		{
			const double T = FPlatformTime::Seconds();
			if (PauseHoldLogouts == 0 && T - PauseAt < 30.0) return;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: hoststall: host after: players who left %d, the client's link here %d (%.1f s after the stall)"), PauseHoldLogouts,
				GetLink(1) ? 1 : 0, T - PauseAt);
			PauseStep = 20;
			PauseAt = T + 1.5;
			return;
		}
		if (PauseStep == 20 && Now > PauseAt)
		{
			PauseStep = 21;
			Summary(TEXT("done"));
			QuitForTest();
		}
		return;
	}
	// the client
	AOutlawsCoopLink* Mine = GetLink(MyIndex());
	auto GiveUp = [this](const TCHAR* What)
	{
		PauseStep = 9;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: hostpause: NOT %s (reconnect tries %d, holds seen %d)"), What, ReconnectTries, HostHoldsSeen);
		Summary(What);
		QuitForTest();
	};
	if (PauseStep == 0)
	{
		if (!IsClient() || !bLevelBuilt || !P || L->bNeedPlace || !Mine) return;
		if (PauseAt == 0.0) { PauseAt = Now + 6.0; return; }
		if (Now < PauseAt) return;
		PauseKitBefore = TestFingerprint(L);
		PauseJoinsBefore = JoinsApplied;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: pause: the kit before: %s"), *PauseKitBefore);
		ReadyLine(TEXT("client before"));
		PauseStep = 1;
		PauseAt = Now;
		Mine->ServerTestNote(TEXT("pause hostready"));
		return;
	}
	if (bStall)
	{
		if (PauseStep == 1)
		{
			if (!((Mode == EOutlawsCoopMode::None || bReconnectAttempt) && EndedAt > 0.0 && P && P->IsMenuOpen() && L->IsFrontEnd()))
			{
				if (Now - PauseAt > PauseTestSec + 60.0) GiveUp(TEXT("back at the title menu"));
				return;
			}
			PauseStep = 2;
			PauseHoldSeenAt = Now;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: hoststall: back at the title menu %.1f s after the session ended, %.1f s after its hello (page %s); host pause holds seen %d; the message: %s"),
				Now - EndedAt, Now - PauseAt, *P->CoopMenuPage(), HostHoldsSeen, PendingToast.IsEmpty() ? TEXT("(shown)") : *PendingToast);
			return;
		}
		if (PauseStep == 2 && Now - PauseHoldSeenAt > 3.0)
		{
			PauseStep = 3;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: hoststall: reconnect %s, the world's pace %.2f, this player frozen %d"),
				ReconnectTo.IsEmpty() ? TEXT("not trying") : *FString::Printf(TEXT("trying %s (%d tries)"), *ReconnectTo, ReconnectTries),
				UGameplayStatics::GetGlobalTimeDilation(P), P->IsCoopHostPauseFrozen() ? 1 : 0);
			Summary(TEXT("done"));
			QuitForTest();
		}
		return;
	}
	if (PauseStep == 1)
	{
		if (HostHold.Since > 0.0 && Now - HostHold.Since > 3.0)
		{
			PauseStep = 2;
			UWorld* W = CurrentWorld();
			const UNetDriver* D = W ? W->GetNetDriver() : nullptr;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: hostpause: during the hold: the world's pace %.4f, this player's movement %s, frozen %d, the connection open %d, reconnect tries %d"),
				P ? UGameplayStatics::GetGlobalTimeDilation(P) : -1.f, MoveWord(P), P && P->IsCoopHostPauseFrozen() ? 1 : 0, D && D->ServerConnection ? 1 : 0, ReconnectTries);
		}
		else if (Now - PauseAt > 60.0) GiveUp(TEXT("held: the host's notice never came"));
		return;
	}
	if (PauseStep == 4)
	{
		// (back by the reconnect: the kit a moment after the late join)
		if (Now - PauseHoldSeenAt < 4.0) return;
		PauseStep = 3;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: pause: the kit after: %s"), *TestFingerprint(L));
		ReadyLine(TEXT("client after"));
		if (Mine) Mine->ServerTestNote(TEXT("pause done"));
		return;
	}
	if (PauseStep != 2) return;
	if (JoinsApplied > PauseJoinsBefore && IsClient() && bLevelBuilt && P && !L->bNeedPlace && Mine && HostHold.Since <= 0.0)
	{
		// the host away past the hold: the connection timed out and the reconnect brought this player back
		PauseStep = 4;
		PauseHoldSeenAt = Now;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: hostpause: back by the reconnect %.1f s after the hello (reconnect tries %d, holds seen %d), the world's pace %.2f, this player's movement %s, frozen %d"),
			Now - PauseAt, ReconnectTries, HostHoldsSeen, UGameplayStatics::GetGlobalTimeDilation(P), MoveWord(P), P->IsCoopHostPauseFrozen() ? 1 : 0);
		return;
	}
	if (HostHoldEndedAt <= 0.0 || HostHold.Since > 0.0 || !P)
	{
		if (Now - PauseAt > PauseTestSec + 150.0) GiveUp(TEXT("back in play after the host's pause"));
		return;
	}
	if (Now - HostHoldEndedAt < 8.0) return;
	PauseStep = 3;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: hostpause: after the host's pause: session kept %d, level built %d, the hold %.1f s, reconnect tries %d, late joins applied since %d, the world's pace %.2f, this player's movement %s, frozen %d"),
		IsClient() ? 1 : 0, bLevelBuilt ? 1 : 0, HostHoldLasted, ReconnectTries, JoinsApplied - PauseJoinsBefore, UGameplayStatics::GetGlobalTimeDilation(P), MoveWord(P),
		P->IsCoopHostPauseFrozen() ? 1 : 0);
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: pause: the kit after: %s"), *TestFingerprint(L));
	ReadyLine(TEXT("client after"));
	if (Mine) Mine->ServerTestNote(TEXT("pause done"));
}
#endif // WITH_OUTLAWS_TESTS
