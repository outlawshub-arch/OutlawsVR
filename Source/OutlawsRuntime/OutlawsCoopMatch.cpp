// M1 (tools/olue/PLANS.md "M1. Skeleton"): a Death Match over the co-op session.  The Host Game page's Start hosts a listen
// session that goes straight into the map (no lobby: the warm-up is play); friends join it as R7's late joiners do.  The host
// runs the match: FOutlawsMatchFlow (OutlawsMpHost.h) owns the phases (warm-up, the host's "Start the game" or "start when N
// have joined", the 10 s countdown, the match and its limits, the end, warm-up again) and says the message lines;
// FOutlawsDeathmatch (OutlawsDeathmatch.h) is the rules under it (frags, deaths, spawn points, spawn protection, the
// character worn from the next spawn).  A shot is the shooter's to judge (the accepted limit: shooter-reported hits): a pellet
// through another player's body on the shooter's machine is reported to the host, which checks it and deals it through the
// co-op hurt; that player's death is a kill for whoever struck last.  The scores ride on the session for every machine's
// frag table (at the match's end in front of each player, and from the pause menu's Scoreboard row).
//
// Story co-op never sees any of it: AOutlawsCoopSession::MatchClass stays 0 and every hook asks IsMatch() first.
#include "OutlawsCoop.h"
#include "OutlawsAchievements.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsLevel.h"
#include "OutlawsPlayer.h"
#include "OutlawsRemotePlayer.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsFragTable.h"
#include "OutlawsFormats.h"
#include "OutlawsLab.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

TAutoConsoleVariable<int32> CVarOLScoreboardOnTop(TEXT("ol.ScoreboardOnTop"), 1, TEXT("Matches: the frag table drawn over the world, never cut by a wall (1), or in the picture's material as before (0)."));

namespace
{
	constexpr double KillCreditSeconds = 10.0;       // a death within this long of a player's hit is that player's kill
	constexpr float MaxHitDamage = 60.f;             // (hardening) one reported pellet can deal no more than this
	constexpr double MaxShotReachCm = 20000.0;       // (hardening) a reported hit farther from its shooter than this is refused
	constexpr int32 MaxHitsPerSecond = 40;           // (hardening) a shotgun's pellets and a fan of shots, with room

	FString MatchClock(float Seconds)
	{
		const int32 S = FMath::Max(0, FMath::CeilToInt(Seconds));
		return FString::Printf(TEXT("%d:%02d"), S / 60, S % 60);
	}

	const TCHAR* FlowStateName(uint8 S)
	{
		return FOutlawsMatchFlow::StateName(static_cast<FOutlawsMatchFlow::EState>(S));
	}
}

/** The frag table's rows from the session's scores (every machine the same). */
class FOutlawsMatchTableSource : public IOutlawsFragTableSource
{
public:
	explicit FOutlawsMatchTableSource(UOutlawsCoop* InCoop) : Coop(InCoop) {}
	virtual bool FillFragTable(FFragTableData& Out) const override
	{
		const UOutlawsCoop* C = Coop.Get();
		return C && C->FillMatchTable(Out);
	}
private:
	TWeakObjectPtr<UOutlawsCoop> Coop;
};

// ---- the links ------------------------------------------------------------------------------------------------------------

void AOutlawsCoopLink::ServerPlayerHit_Implementation(int32 Epoch, int32 Victim, float Damage, FVector_NetQuantize10 At, FVector_NetQuantize10 From, bool bBlast)
{
	if (!Admit(EOLGate::PlayerHit, Finite(Damage, 1000.f) && Finite(At) && Finite(From))) return;
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnMatchHit(PlayerIndex, Epoch, Victim, Damage, At, From, bBlast);
}

void AOutlawsCoopLink::ServerMatchRespawn_Implementation(int32 Epoch)
{
	if (!Admit(EOLGate::MatchRespawn, true)) return;
	UOutlawsCoop* C = UOutlawsCoop::Get(this);
	if (C && Epoch == C->LevelEpoch()) C->OnMatchRespawnAsk(PlayerIndex);
}

void AOutlawsCoopLink::ServerMatchCharacter_Implementation(const FString& Short)
{
	if (!Admit(EOLGate::MatchCharacter, Short.Len() <= 8)) return;
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnMatchCharacter(PlayerIndex, Short.Left(8));
}

void AOutlawsCoopLink::ClientMatchRespawn_Implementation(int32 Epoch, FVector_NetQuantize10 At, float Yaw, const FString& InCharacter)
{
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnMatchRespawnHere(Epoch, At, Yaw, InCharacter.Left(8));
}

void AOutlawsCoopLink::ClientMatchMessage_Implementation(const FString& Text, float Seconds)
{
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnMatchMessage(Text.Left(120), FMath::Clamp(Seconds, 0.5f, 20.f));
}

void AOutlawsCoopLink::ClientMatchKill_Implementation(int32 Killer, int32 Victim, uint8 Kind, const FString& KillerName, const FString& VictimName)
{
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnMatchKill(Killer, Victim, FMath::Min<uint8>(Kind, uint8(EOutlawsObit::Count) - 1), KillerName.Left(32), VictimName.Left(32));
}

// ---- what the rest of the game asks ------------------------------------------------------------------------------------------

bool UOutlawsCoop::IsMatch() const
{
	if (Mode == EOutlawsCoopMode::None) return false;
	if (IsHost() && bMatchPending) return true;
	const AOutlawsCoopSession* S = GetSession();
	return S && S->MatchClass != 0;
}

bool UOutlawsCoop::IsMatchActive(const UObject* Context)
{
	const UOutlawsCoop* C = Get(Context);
	return C && C->IsMatch();
}

FString UOutlawsCoop::MatchDeathPrompt(bool bVR)
{
	return bVR ? TEXT("You are dead\nA / X: respawn") : TEXT("You are dead\nSpace: respawn");
}

bool UOutlawsCoop::HostMatch(const FOutlawsMpHostOptions& InOptions)
{
	FOutlawsMpHostOptions Options = InOptions;
	// the map's level, from the same list the page showed (a stock RCM in the LABs, or a custom one beside its LAB)
	const AOutlawsLevelActor* L = FindLevel();
	TArray<FOutlawsMpMap> Maps;
	TSharedPtr<FOutlawsAssets> Assets = L ? L->AssetsRef : nullptr;
	if (!Assets)
	{
		// (a launch flag's host: the level actor has not opened the game's files yet; the shared set is the same files)
		const FString Root = FOutlawsAssets::FindInstallRoot(L ? L->InstallPath : FString());
		if (!Root.IsEmpty()) Assets = FOutlawsAssets::GetShared(Root);
	}
	OutlawsMpHost::ListMaps(Assets.Get(), Maps);
	if (Options.bRandomMap || Options.bRandomClass)
	{
		// (REQ-MPLOBBY b, decision 4: "Random each match" rolled for the first match too)
		FString Said;
		if (OutlawsMpHost::RollMatch(Maps, Options, FString(), EOutlawsGameClass(0), Said))
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH random each match: the first match is %s"), *Said);
		}
		else
		{
			UE_LOG(LogOutlawsCoop, Warning, TEXT("OLMATCH random each match: nothing to roll (%s): the chosen map and game"), *Said);
		}
	}
	const FOutlawsMpMap* Map = Maps.FindByPredicate([&](const FOutlawsMpMap& M) { return M.File.Equals(Options.MapFile, ESearchCase::IgnoreCase); });
	if (!Map) Map = Maps.FindByPredicate([&](const FOutlawsMpMap& M) { return M.Level.Equals(Options.MapFile, ESearchCase::IgnoreCase); });
	if (!Map)
	{
		UE_LOG(LogOutlawsCoop, Warning, TEXT("OLMATCH host: no map %s among %d"), *Options.MapFile, Maps.Num());
		return false;
	}
	if (!Map->Allows(Options.GameClass))
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH host: %s does not play %s (its modes %s): Death Match"), *Map->Name, *OutlawsMpHost::ClassLabel(Options.GameClass), *Map->ModesText());
	}
	Host();                                      // (the listen session; its ResetSession first)
	MatchOptions = Options;
	MatchOptions.MapFile = Map->File;
	if (!Map->Allows(MatchOptions.GameClass) || !OutlawsMpHost::IsClassOn(MatchOptions.GameClass))
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH host: %s not offered here (%s): Death Match"), *OutlawsMpHost::ClassLabel(MatchOptions.GameClass),
			OutlawsMpHost::IsClassOn(MatchOptions.GameClass) ? TEXT("the map's modes") : TEXT("switched off in this build"));
		MatchOptions.GameClass = EOutlawsGameClass::DeathMatch;     // (M2: the six classes)
	}
	MatchOptions.Clamp(Map);
	LevelToStart = Map->Level.ToLower();
	bMatchPending = true;
	if (AOutlawsCoopSession* CS = GetSession()) CS->bCheatsOff = !MatchOptions.bAllowCheats;       // (BUG-ALLOWCHEATS: at once, before the level is built)
	if (!Options.Password.IsEmpty() || HostPassword.IsEmpty()) HostPassword = CleanPassword(Options.Password);     // (the page's; a test's flag stands when the page has none)
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH hosting %s (%s, level %s): %s"), *Map->Name, *Map->File, *LevelToStart, *MatchOptions.Describe());
	return true;
}

// ---- the host's match ------------------------------------------------------------------------------------------------------

void UOutlawsCoop::SetUpMatchLevel(AOutlawsLevelActor* L)
{
	// (the host, each build of the match's level) the rules read its spawn points and generators; the flow begins once
	const FOutlawsLevel* Lv = L ? L->LiveLevel() : nullptr;
	if (!Lv || !IsHost()) return;
	MatchSerial = StartedSerial;
	FMemory::Memzero(MatchInRules);
	FMemory::Memzero(MatchDead);
	FMemory::Memzero(LastAttackAt);
	Dm = FOutlawsDeathmatch();
	Dm.bFlowDriven = true;
	Dm.Config.FragLimit = MatchOptions.ScoreLimit;
	Dm.Config.TimeLimit = MatchOptions.TimeLimitMinutes * 60.f;
	Dm.ReadLevel(*Lv, L->AssetsRef.Get());
	SetUpModes(L);                               // (M2: the class's own rules: OutlawsCoopModes.cpp)
	TWeakObjectPtr<UOutlawsCoop> Weak(this);
	Dm.OnRespawn = [Weak](const FOutlawsDmPlayer& P, const FOutlawsDmSpawn& At)
	{
		UOutlawsCoop* C = Weak.Get();
		if (!C) return;
		++C->MatchRespawns;
		C->Modes.NoteAlive(P.Id, true);
		if (AOutlawsCoopLink* K = C->GetLink(P.Id))
		{
			K->Character = P.Character;
			K->ForceNetUpdate();
		}
		if (P.Id == 0) C->OnMatchRespawnHere(C->LevelEpoch(), At.Location, At.Yaw, P.Character);
		else if (C->IsBot(P.Id)) C->BotSpawn(P.Id, At.Location, At.Yaw);          // (REQ-MPBOTS: OutlawsCoopBots.cpp)
		else if (AOutlawsCoopLink* K = C->GetLink(P.Id)) K->ClientMatchRespawn(C->LevelEpoch(), At.Location, At.Yaw, P.Character);
	};
	FOutlawsMatchFlow& F = OutlawsMpHost::Flow();
	F.OnMessage = [Weak](const FString& Text, float Seconds) { if (UOutlawsCoop* C = Weak.Get()) C->SayToAll(Text, Seconds); };
	F.OnMatchStart = [Weak]()
	{
		UOutlawsCoop* C = Weak.Get();
		if (!C) return;
		FMemory::Memzero(C->LastAttackAt);
		C->Dm.GoLive(FPlatformTime::Seconds(), TEXT("the countdown ran out"));
		C->Modes.GoLive(FPlatformTime::Seconds());
	};
	F.OnMatchEnd = [Weak](const FString& Why)
	{
		if (UOutlawsCoop* C = Weak.Get())
		{
			C->Dm.EndByFlow(FPlatformTime::Seconds(), Why);
			C->Modes.SetLive(false);
		}
	};
	Lobby.Reset();                               // (REQ-MPLOBBY b: the lobby's start rule for every match, the next ones too)
	F.bLobbyGate = !bLobbyRuleOff;
	F.bHoldEnd = NextMatchCycleOn();
	if (F.GetState() == FOutlawsMatchFlow::EState::Idle) F.Begin(MatchOptions);
	else
	{
		Dm.WarmupByFlow(FPlatformTime::Seconds());
	}
	bMatchSetUp = true;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH the match on %s: %d spawn points, %d pickup generators; flow %s"), *L->LevelName, Dm.GetSpawns().Num(), Dm.GetItems().Num(),
		FOutlawsMatchFlow::StateName(F.GetState()));
}

void UOutlawsCoop::TickMatch(double Now)
{
	AOutlawsCoopSession* S = GetSession();
	AOutlawsLevelActor* L = FindLevel();
	FOutlawsMpCharacter::bMatchMenu = IsMatch();
	// (every machine) the character page's choice: to the rules (the host's own) or through this player's link; in co-op
	// (REQ-COOPCHAR) a joiner's goes to the host, who wears it on that player's link (the look and name; the host keeps his)
	if (!FOutlawsMpCharacter::OnLocalChoice)
	{
		TWeakObjectPtr<UOutlawsCoop> Weak(this);
		FOutlawsMpCharacter::OnLocalChoice = [Weak](const FString& Short)
		{
			UOutlawsCoop* C = Weak.Get();
			if (!C) return;
			if (C->IsHost()) { if (C->IsMatch()) C->OnMatchCharacter(0, Short); }
			else if (AOutlawsCoopLink* Mine = C->GetLink(C->MyIndex())) Mine->ServerMatchCharacter(Short);
		};
	}
	// REQ-COOPCHAR: a co-op joiner's character, sent once its link is there (and again on a new link); BUG-CHARLOADOUT: a match
	// joiner's too (it joined as James Anderson whatever it had chosen, until it chose again in the pause menu)
	if (!IsHost())
	{
		AOutlawsCoopLink* Mine = GetLink(MyIndex());
		if (Mine && Mine != CoopCharSentLink.Get())
		{
			CoopCharSentLink = Mine;
			const FString Short = CoopCharChoice();
			Mine->ServerMatchCharacter(Short);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP my character sent to the host: %s"), FOutlawsMpCharacter::Find(Short) ? FOutlawsMpCharacter::Find(Short)->Name : *Short);
		}
	}
	if (!S || !L || !IsMatch()) return;
	if (!MatchTable.IsValid()) MatchTable = MakeShared<FOutlawsMatchTableSource>(this);
	TickModeSprites(Now);                        // (M2: the flags, the chicken, the document and IT drawn: OutlawsCoopModes.cpp)
	// (every machine) the phase as the host says it: the table at the match's end, in front of this player
	if (S->MatchState != LoggedMatchState)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH flow here: %s -> %s (%s)"), LoggedMatchState == 255 ? TEXT("-") : FlowStateName(LoggedMatchState), FlowStateName(S->MatchState),
			IsHost() ? TEXT("host") : WhoWord(MyIndex()));
		if (bMatchPanelHeld) HideMatchPanel(TEXT("the match moved on"));      // (BUG-SCOREBOARD)
		// (REQ-MPLOBBY b: with the next-match cycle the end is the score screen instead, TickNextMatch)
		if (S->MatchState == uint8(FOutlawsMatchFlow::EState::Ended) && !S->bBetweenMatches) ShowMatchPanel(FOutlawsMatchFlow::EndedSeconds);
		if (S->MatchState == uint8(FOutlawsMatchFlow::EState::Ended) && LoggedMatchState != 255)
		{
			// REQ-EXTRAS: this machine's player won it (first in the table alone; in a team game, its team's score the highest
			// alone), against players or against bots only
			FFragTableData T;
			bool bWon = false;
			if (FillMatchTable(T) && T.Rows.Num() > 0)
			{
				TMap<int32, int32> TeamScore;
				for (const FFragTableHeaderRow& H : T.HeaderRows) if (H.Team != 0) TeamScore.Add(H.Team, FCString::Atoi(*H.Value));
				const FFragTableRow* Mine = T.Rows.FindByPredicate([](const FFragTableRow& R) { return R.bLocal; });
				if (TeamScore.Num() >= 2 && Mine && Mine->Team != 0)
				{
					bWon = true;
					for (const TPair<int32, int32>& Ts : TeamScore) if (Ts.Key != Mine->Team && Ts.Value >= TeamScore.FindRef(Mine->Team)) bWon = false;
				}
				else bWon = T.Rows[0].bLocal && (T.Rows.Num() < 2 || T.Rows[0].SortA > T.Rows[1].SortA);
			}
			bool bOnlyBots = true;
			for (const FOutlawsMatchScore& R : S->Scores) if (R.Who != MyIndex() && !IsBot(R.Who)) bOnlyBots = false;
			OutlawsAchievements::NoteMatchEnd(L, bWon, bOnlyBots);
		}
		LoggedMatchState = S->MatchState;
	}
	TickNextMatch(Now, S, LocalPlayer());            // (REQ-MPLOBBY b: the fade, the score screen, Continue: OutlawsCoopNextMatch.cpp)
	if (Mode == EOutlawsCoopMode::None) return;      // (the joiner left: the host ended the game, or never started the next)
	if (MatchPanel.IsValid() && !bMatchPanelHeld && Now >= MatchPanelUntil) HideMatchPanel(TEXT("its time"));
#if WITH_OUTLAWS_TESTS
	if (OLFlags::Param(TEXT("OLScoreboardTest")) && IsHost()) TickScoreboardTest(Now);
	if (OLFlags::Param(TEXT("OLScoreAboveTest"))) TickScoreAboveTest(Now);
#endif
	TickSpectate(Now);                               // (M4: following a player while dead; OutlawsCoopMatchExtras.cpp)
	if (!IsHost()) return;

	// ---- the host ----
	if (bMatchPending && S->StartSerial == 0 && LocalPlayer())
	{
		// hosting a match: straight into its map (R7's "Start now": friends join it as late joiners)
		bMatchPending = false;
		S->MatchClass = uint8(MatchOptions.GameClass);
		S->FragLimit = MatchOptions.ScoreLimit;
		S->TimeLimitMinutes = MatchOptions.TimeLimitMinutes;
		S->MapName = MatchOptions.MapFile;
		S->RandomBits = (MatchOptions.bRandomMap ? 1 : 0) | (MatchOptions.bRandomClass ? 2 : 0);
		S->ForceNetUpdate();
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH the host loads %s behind the lobby (warm-up)"), *LevelToStart);
		if (!LaunchMatchMap.IsEmpty() && LobbyEnterAfter <= 0.f) SetReadyFor(0, true);     // (REQ-MPLOBBY: a launched host enters by itself)
		StartGame(true);
		return;
	}
	if (!bLevelBuilt || L->bNeedPlace) return;
	if (MatchSerial != StartedSerial) SetUpMatchLevel(L);
	if (!bMatchSetUp) return;
	const float Dt = LastMatchTickAt > 0.0 ? float(FMath::Min(0.5, Now - LastMatchTickAt)) : 0.f;
	LastMatchTickAt = Now;
	TickBots(Now, L);                                // (REQ-MPBOTS: bots in and out, their brains, pickups, deaths: OutlawsCoopBots.cpp)

	// the players in the rules: each once it has built this level; one who left goes
	TArray<AOutlawsCoopLink*> Links;
	GetLinks(Links);
	bool Present[MaxPlayers] = {};
	for (AOutlawsCoopLink* K : Links)
	{
		const int32 Who = K->PlayerIndex;
		if (Who < 0 || Who >= MaxPlayers) continue;
		Present[Who] = true;
		// (REQ-MPLOBBY: the host once it enters from the lobby; b: a joiner too, which builds the next match's level behind it)
		const bool bBuilt = Who == 0 ? S->bHostReady : K->BuiltSerial == StartedSerial && (K->bBot || K->bLobbyReady);
		if (!bBuilt || MatchInRules[Who]) continue;
		MatchInRules[Who] = true;
		MatchDead[Who] = false;
		const FString Name = !K->PlayerName.IsEmpty() ? K->PlayerName : Who == 0 ? FString(TEXT("Host")) : FString::Printf(TEXT("Player %d"), Who + 1);
		const FString Char = Who == 0 ? CoopCharChoice() : (K->Character.IsEmpty() ? FString(TEXT("Jam")) : K->Character);
		Dm.AddPlayer(Who, Name, Char, Who == 0, Now);
		Modes.AddPlayer(Who, Now);
		if (LobbyTeam[Who] != 0)
		{
			const int32 Team = LobbyTeam[Who];
			LobbyTeam[Who] = 0;
			OnMatchTeam(Who, Team);              // (REQ-MPLOBBY: the team picked in the lobby)
		}
	}
	for (int32 Who = 0; Who < MaxPlayers; ++Who)
	{
		if (!MatchInRules[Who] || Present[Who]) continue;
		MatchInRules[Who] = false;
		Dm.RemovePlayer(Who);
		Modes.RemovePlayer(Who, Now);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH the %s left the match"), WhoWord(Who));
	}
	FOutlawsMatchFlow& F = OutlawsMpHost::Flow();
	F.SetPlayers(Dm.GetPlayers().Num());
	int32 People = 0;
	for (const AOutlawsCoopLink* K : Links) if (!K->bBot && K->PlayerIndex >= 0 && K->PlayerIndex < MaxPlayers && MatchInRules[K->PlayerIndex]) ++People;
	// (a match launched by -OLMpHost: -OLMpStartWhen counts people, not bots; the Host Game page's match as before)
	if (!LaunchMatchMap.IsEmpty()) F.SetHumans(People);
	if (LaunchHostRespawn > 0.f)
	{
		// (-OLMpHostRespawn: nobody at the host's machine to press for the respawn, and a dead host makes late joiners wait)
		AOutlawsPlayer* Me = LocalPlayer();
		if (!L->bDead || !Me) HostDeadAt = 0.0;
		else if (HostDeadAt == 0.0) HostDeadAt = Now;
		else if (Now - HostDeadAt >= LaunchHostRespawn && Now >= HostRespawnTryAt)
		{
			HostRespawnTryAt = Now + 1.0;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH the unattended host respawns (dead %.1f s)"), Now - HostDeadAt);
			Me->RequestRespawn(false);
		}
	}

	// deaths: each one a kill for whoever struck last (a suicide or the world otherwise)
	for (const AOutlawsCoopLink* K : Links)
	{
		const int32 Who = K->PlayerIndex;
		if (Who < 0 || Who >= MaxPlayers || !MatchInRules[Who]) continue;
		const bool bDead = Who == 0 ? L->bDead : (K->Stats.Seq > 0 && K->Stats.bDead);
		if (bDead && !MatchDead[Who])
		{
			const bool bCredit = LastAttackAt[Who] > 0.0 && Now - LastAttackAt[Who] < KillCreditSeconds;
			const int32 Killer = bCredit ? LastAttacker[Who] : Who;
			const AOutlawsCoopLink* KL = bCredit ? GetLink(Killer) : nullptr;
			const FVector Victim = Who == 0 && LocalPlayer() ? LocalPlayer()->GetActorLocation() : FVector(K->Pose.Body);
			const FVector By = Killer == 0 && LocalPlayer() ? LocalPlayer()->GetActorLocation() : KL ? FVector(KL->Pose.Body) : Victim;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH kill: the %s %s"), WhoWord(Who), Killer == Who ? TEXT("died (no one's kill)") : *FString::Printf(TEXT("killed by the %s at %.0f m"), WhoWord(Killer), FVector::Dist(Victim, By) / 100.0));
			// (REQ-KILLFEED: the kill line, before the rules can drop a player who left; nobody's kill worded by the victim's
			// own cause, the host's level's or the one its stats said)
			const uint8 Cause = Who == 0 ? L->DeathCause : K->Stats.DeathBy;
			SayKill(Killer, Who, Killer != Who ? (LastAttackBlast[Who] ? EOutlawsObit::BlewUp : EOutlawsObit::Killed) : OutlawsKillFeed::FromCause(Cause));
			Dm.NoteKill(Killer, Who, float(FVector::Dist(Victim, By)), Now);
			Modes.NoteKill(Killer, Who, Who == 0 ? Victim - FVector(0.0, 0.0, 88.0) : Victim - FVector(0.0, 0.0, K->Pose.HalfHeight), Now);     // (M2: a carried item falls where it lay)
			LastAttackAt[Who] = 0.0;
			if (MatchOptions.GameClass == EOutlawsGameClass::DeathMatch)
			{
				FString Leader;
				const int32 Top = Dm.TopFrags(Leader);
				F.ReportScore(Top, Leader);
			}
		}
		MatchDead[Who] = bDead;
	}

	// the flow, then the rules under it (a new warm-up: the rules' too)
	const FOutlawsMatchFlow::EState Was = F.GetState();
	F.Tick(Dt);
	if (Was != F.GetState() && F.GetState() == FOutlawsMatchFlow::EState::WarmUp)
	{
		Dm.WarmupByFlow(Now);
		Modes.SetLive(false);
	}
	TickLobbyRule(Dt, S, People);                    // (REQ-MPLOBBY: the lobby's start rule: OutlawsCoopLobby.cpp)
	TickNextMatchHost(Now, S);                       // (REQ-MPLOBBY b: the end, the score screen, the next match: OutlawsCoopNextMatch.cpp)
	TickMatchItems(Now);                             // (the map's pickup generators: OutlawsCoopMatchStrikes.cpp)
	Dm.Tick(Now);
	TickModesHost(Now, Dt);                      // (M2: carries, captures, the time held; OutlawsCoopModes.cpp)
	PublishScores();
}

void UOutlawsCoop::PublishScores()
{
	AOutlawsCoopSession* S = GetSession();
	if (!S || !IsHost()) return;
	TArray<FOutlawsMatchScore> Now;
	for (const FOutlawsDmPlayer& P : Dm.GetPlayers())
	{
		FOutlawsMatchScore& R = Now.AddDefaulted_GetRef();
		R.Who = P.Id;
		R.Name = P.Name;
		R.Character = P.Character;
		R.Frags = int16(P.Frags);
		R.Deaths = int16(P.Deaths);
		R.Shots = int16(FMath::Min(P.Shots, 32767));
		R.Hits = int16(FMath::Min(P.Hits, 32767));
		R.LongestCm = P.LongestCm;
		R.bAlive = P.bAlive;
		if (const FOutlawsModePlayer* M = Modes.Find(P.Id))
		{
			R.Team = uint8(M->Team);
			R.Captures = int16(M->Captures);
			R.HeldSeconds = int16(FMath::Min(32767, FMath::FloorToInt(M->Held)));
		}
	}
	const FOutlawsMatchFlow& F = OutlawsMpHost::Flow();
	const uint8 State = uint8(F.GetState());
	const float Left = F.TimeLeft() < 0.f ? -1.f : float(FMath::CeilToInt(F.TimeLeft()));
	if (Now != S->Scores || State != S->MatchState || Left != S->TimeLeft)
	{
		if (Now != S->Scores) UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH scores: %s"), *MatchSummary());
		S->Scores = MoveTemp(Now);
		S->MatchState = State;
		S->TimeLeft = Left;
		S->ForceNetUpdate();
	}
}

FString UOutlawsCoop::MatchSummary() const
{
	const AOutlawsCoopSession* S = GetSession();
	FString Out;
	for (const FOutlawsDmPlayer& P : Dm.GetPlayers())
	{
		Out += FString::Printf(TEXT("%s%s (%s) %d/%d"), Out.IsEmpty() ? TEXT("") : TEXT(", "), *P.Name, WhoWord(P.Id), P.Frags, P.Deaths);
	}
	if (Out.IsEmpty() && S)
	{
		for (const FOutlawsMatchScore& R : S->Scores) Out += FString::Printf(TEXT("%s%s %d/%d"), Out.IsEmpty() ? TEXT("") : TEXT(", "), *R.Name, R.Frags, R.Deaths);
	}
	return FString::Printf(TEXT("%s; hits sent %d, dealt %d, refused %d; respawns %d"), Out.IsEmpty() ? TEXT("nobody") : *Out, MatchHitsSent, MatchHitsDealt, MatchHitsRefused, MatchRespawns);
}

void UOutlawsCoop::SayToAll(const FString& Text, float Seconds)
{
	OnMatchMessage(Text, Seconds);
	ForClients([&](AOutlawsCoopLink& K) { K.ClientMatchMessage(Text, Seconds); });
}

void UOutlawsCoop::OnMatchMessage(const FString& Text, float Seconds)
{
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH message here (%s): %s"), IsHost() ? TEXT("host") : WhoWord(MyIndex()), *Text);
	if (AOutlawsLevelActor* L = FindLevel()) L->Notify(Text, Seconds);
}

// ---- shots, hits and kills -------------------------------------------------------------------------------------------------

bool UOutlawsCoop::ShotAtPlayers(AOutlawsLevelActor* L, const FVector& From, const FVector& To, float Damage, FVector& OutAt)
{
	UOutlawsCoop* C = Get(L);
	if (!C || !C->IsMatch() || !C->bLevelBuilt || Damage <= 0.f) return false;
	// the first other player's body (an upright capsule round its drawn place) the pellet passes through
	const FVector Seg = To - From;
	const double Len = Seg.Size();
	if (Len < 1.0) return false;
	const FVector Dir = Seg / Len;
	int32 Best = -1;
	double BestT = Len;
	for (int32 Who = 0; Who < MaxPlayers; ++Who)
	{
		if (Who == C->MyIndex()) continue;
		const AOutlawsRemotePlayer* R = C->PuppetOf(Who);
		if (!R || R->IsHidden() || R->PlateDead()) continue;
		const double Radius = 32.0, Half = FMath::Max(40.0, double(R->ShownHalf()));
		const FVector Mid = R->ShownBody();
		// closest approach of the pellet's line to the body's axis, within the body's height
		const FVector Axis = FVector::UpVector;
		const FVector W0 = From - Mid;
		const double B = FVector::DotProduct(Dir, Axis), Dd = FVector::DotProduct(Dir, W0), E = FVector::DotProduct(Axis, W0);
		const double Den = 1.0 - B * B;
		double T = Den > 1e-6 ? (B * E - Dd) / Den : -Dd;
		T = FMath::Clamp(T, 0.0, Len);
		const FVector P = From + Dir * T;
		const double Along = FMath::Clamp(double(P.Z - Mid.Z), -Half, Half);
		const FVector OnAxis = Mid + Axis * Along;
		if (FVector::Dist(P, OnAxis) > Radius || FMath::Abs(P.Z - Mid.Z) > Half) continue;
		if (T < BestT)
		{
			BestT = T;
			Best = Who;
		}
	}
	if (Best < 0) return false;
	OutAt = From + Dir * BestT;
	++C->MatchHitsSent;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH shot hits the %s at %.0f m for %.1f (%s)"), WhoWord(Best), BestT / 100.0, Damage, C->IsHost() ? TEXT("the host deals it") : TEXT("reported to the host"));
	L->SpawnBlood(OutAt, Dir, 2);
	if (C->IsHost()) C->OnMatchHit(0, C->LevelEpoch(), Best, Damage, OutAt, From);
	else if (AOutlawsCoopLink* Mine = C->GetLink(C->MyIndex())) Mine->ServerPlayerHit(C->LevelEpoch(), Best, Damage, OutAt, From, false);
	return true;
}

void UOutlawsCoop::OnMatchHit(int32 Shooter, int32 Epoch, int32 Victim, float Damage, const FVector& At, const FVector& From, bool bBlast)
{
	// (the host) checked, then dealt on the victim's machine (the host's own player here)
	if (!IsHost() || !IsMatch() || !bMatchSetUp) return;
	auto Refuse = [&](const TCHAR* Why)
	{
		++MatchHitsRefused;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH hit by the %s on the %s refused: %s"), WhoWord(FMath::Clamp(Shooter, 0, MaxPlayers - 1)), WhoWord(FMath::Clamp(Victim, 0, MaxPlayers - 1)), Why);
	};
	if (Epoch != LevelEpoch()) return Refuse(TEXT("another level"));
	if (Shooter < 0 || Shooter >= MaxPlayers || Victim < 0 || Victim >= MaxPlayers || Shooter == Victim) return Refuse(TEXT("no such player"));
	if (!MatchInRules[Shooter] || !MatchInRules[Victim]) return Refuse(TEXT("not in the match"));
	// (BUG-TNTDEATHFREEZE: a stick the shooter threw before it died still goes off and counts, "blew up" on the kill feed)
	if ((MatchDead[Shooter] && !bBlast) || MatchDead[Victim]) return Refuse(TEXT("a dead player"));
	if (!FMath::IsFinite(Damage) || Damage <= 0.f || Damage > MaxHitDamage || At.ContainsNaN() || From.ContainsNaN()) return Refuse(TEXT("out of bounds"));
	// (the shooter's rate, and its muzzle near where the host has it)
	const double Now = FPlatformTime::Seconds();
	static double RateAt[MaxPlayers] = {};
	static int32 RateCount[MaxPlayers] = {};
	if (Now - RateAt[Shooter] >= 1.0) { RateAt[Shooter] = Now; RateCount[Shooter] = 0; }
	if (++RateCount[Shooter] > MaxHitsPerSecond) return Refuse(TEXT("too many hits in a second"));
	const AOutlawsCoopLink* SL = GetLink(Shooter);
	const FVector ShooterAt = Shooter == 0 && LocalPlayer() ? LocalPlayer()->GetActorLocation() : SL ? FVector(SL->Pose.Body) : From;
	if (FVector::Dist(ShooterAt, From) > 600.0 || FVector::Dist(From, At) > MaxShotReachCm) return Refuse(TEXT("not where the shooter is"));
	if (!Modes.HitAllowed(Shooter, Victim)) return Refuse(TEXT("a teammate (friendly fire off)"));
	if (!Dm.NoteHit(Shooter, Victim, Now)) return Refuse(TEXT("the rules: protected, or not now"));
	Damage = MatchDamageFor(Victim, Damage);         // (the victim's character: its DAMAGE_MULTIPLIER; OutlawsCoopMatchStrikes.cpp)
	LastAttacker[Victim] = Shooter;
	LastAttackAt[Victim] = Now;
	LastAttackBlast[Victim] = bBlast;
	++MatchHitsDealt;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH hit: the %s struck the %s for %.1f%s"), WhoWord(Shooter), WhoWord(Victim), Damage, bBlast ? TEXT(" (dynamite)") : TEXT(""));
	if (Victim == 0)
	{
		if (AOutlawsLevelActor* L = FindLevel())
		{
			L->SetHurtSource(From);
			L->DamagePlayer(Damage, 1.f, false);
		}
	}
	else if (IsBot(Victim))
	{
		BotHurt(Victim, Damage, At, From, Shooter);   // (REQ-MPBOTS: the host's own bot)
	}
	else
	{
		HurtOther(Damage, 1.f, false, &From, -1, Victim);
	}
}

void UOutlawsCoop::OnMatchCharacter(int32 Who, const FString& Short)
{
	if (!IsHost() || !FOutlawsMpCharacter::Find(Short)) return;
	if (!IsMatch())
	{
		// REQ-COOPCHAR: co-op: worn on the joiner's link at once (replicated: every machine draws its puppet so); the look and
		// the name only, the health and speed stay the campaign's.  The host is James Anderson for the story.
		AOutlawsCoopLink* K = Who > 0 ? GetLink(Who) : nullptr;
		if (!K) return;
		const FOutlawsMpCharacter* Ch = FOutlawsMpCharacter::Find(Short);
		K->Character = Ch->Short;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the %s chose %s: drawn as %s (%s)"), WhoWord(Who), Ch->Name, Ch->Name, Ch->Nwx);
		return;
	}
	// (BUG-CHARLOADOUT: a joiner's word before the rules have it: worn on its link, which its first spawn takes)
	if (!Dm.Find(Who))
	{
		if (AOutlawsCoopLink* K = Who > 0 ? GetLink(Who) : nullptr) K->Character = FOutlawsMpCharacter::Find(Short)->Short;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH the %s will join as %s"), WhoWord(Who), FOutlawsMpCharacter::Find(Short)->Name);
		return;
	}
	Dm.SetPendingCharacter(Who, Short);
}

FString UOutlawsCoop::KitCharacterFor(const UObject* Context)
{
	UOutlawsCoop* C = Get(Context);
	if (!C || !IsActive(Context)) return FString();
	if (C->IsMatch())
	{
		const AOutlawsCoopLink* Mine = C->GetLink(C->MyIndex());
		return Mine && FOutlawsMpCharacter::Find(Mine->Character) ? Mine->Character : CoopCharChoice();
	}
	return C->IsHost() ? FString() : CoopCharChoice();
}

FString UOutlawsCoop::CoopCharChoice()
{
	FString Arg;
	if (OLFlags::Value(TEXT("OLMpChar"), Arg) && FOutlawsMpCharacter::Find(Arg)) return FOutlawsMpCharacter::Find(Arg)->Short;
	return FOutlawsMpCharacter::LocalChoice();
}

// ---- respawns --------------------------------------------------------------------------------------------------------------

bool UOutlawsCoop::MatchRespawn(AOutlawsPlayer* P)
{
	UOutlawsCoop* C = Get(P);
	AOutlawsLevelActor* L = C ? C->FindLevel() : nullptr;
	if (!C || !L || !C->IsMatch()) return false;
	if (!L->bDead) return true;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH the %s asks to respawn"), WhoWord(C->MyIndex()));
	if (C->IsHost()) C->OnMatchRespawnAsk(0);
	else if (AOutlawsCoopLink* Mine = C->GetLink(C->MyIndex())) Mine->ServerMatchRespawn(C->LevelEpoch());
	return true;
}

void UOutlawsCoop::OnMatchRespawnAsk(int32 Who)
{
	if (!IsHost() || !bMatchSetUp || Who < 0 || Who >= MaxPlayers || !MatchInRules[Who]) return;
	const bool bDead = MatchDead[Who];
	if (!bDead || !Dm.RequestRespawn(Who, FPlatformTime::Seconds()))
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH respawn for the %s not now (%s)"), WhoWord(Who), bDead ? TEXT("the delay") : TEXT("not dead"));
	}
}

void UOutlawsCoop::OnMatchRespawnHere(int32 Epoch, const FVector& At, float Yaw, const FString& Character)
{
	// this machine's player at a spawn point: the start kit, full health, alive (dead or not: the match's start places all)
	RunFor(Epoch, [this, At, Yaw, Character]()
	{
		AOutlawsLevelActor* L = FindLevel();
		AOutlawsPlayer* P = LocalPlayer();
		if (!L || !P) return;
		const bool bWasDead = L->bDead;
		if (AOutlawsCoopLink* Mine = GetLink(MyIndex())) Mine->Character = Character;
		L->ApplyStartKit(Character);                 // (BUG-CHARLOADOUT: the character's own kit and weapons)
		L->ArmourMax = 0;
		L->Inventory.Reset();
		L->InventoryLabels.Reset();
		if (L->Inf) L->Inf->Items.Reset();
		P->ResetForLevel();
		P->EquipStartWeapon();                       // (BUG-CHARLOADOUT: START_WEAPON in the hand: Mary's shotgun, Bob's double barrel)
		const FRotator Face(0.0, Yaw, 0.0);
		P->TeleportTo(At, Face, false, true);
		if (AController* Ctl = P->GetController()) Ctl->SetControlRotation(Face);
		P->GetCharacterMovement()->StopMovementImmediately();
		bLocalDead = false;
		const FOutlawsMpCharacter* Ch = FOutlawsMpCharacter::Find(Character);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH respawned here (%s) at %s as %s%s"), IsHost() ? TEXT("host") : WhoWord(MyIndex()), *At.ToCompactString(), Ch ? Ch->Name : *Character,
			bWasDead ? TEXT(" (was dead)") : TEXT(""));
	});
}

// ---- the frag table ------------------------------------------------------------------------------------------------------------

bool UOutlawsCoop::FillMatchTable(FFragTableData& Out) const
{
	const AOutlawsCoopSession* S = GetSession();
	if (!S || S->MatchClass == 0) return false;
	Out = FFragTableData();
	Out.Title = TEXT("Death Match");
	Out.SetMatchColumns(false);
	Out.HeaderRows.Add({ TEXT("Time Limit"), S->TimeLimitMinutes > 0 ? MatchClock(S->TimeLeft >= 0.f ? S->TimeLeft : S->TimeLimitMinutes * 60.f) : FString(TEXT("None")), 0 });
	Out.HeaderRows.Add({ TEXT("Kill Limit"), S->FragLimit > 0 ? FString::FromInt(S->FragLimit) : FString(TEXT("None")), 0 });
	for (const FOutlawsMatchScore& R : S->Scores)
	{
		FFragTableRow& Row = Out.Rows.AddDefaulted_GetRef();
		Row.Name = R.Name;
		Row.bLocal = R.Who == MyIndex();
		Row.Values = { FString::FromInt(R.Frags), FString::FromInt(R.Deaths), FString::FromInt(R.Shots),
			R.Shots > 0 ? FString::Printf(TEXT("%d%%"), FMath::Min(100, FMath::RoundToInt(100.f * float(R.Hits) / float(R.Shots)))) : FString(TEXT("-")),
			R.LongestCm > 0 ? FString::Printf(TEXT("%d m"), FMath::Max(1, FMath::RoundToInt(R.LongestCm / 100.f))) : FString(TEXT("-")) };
		Row.SortA = R.Frags;
		Row.SortB = R.Deaths;
	}
	Out.SortRows();
	FillModeColumns(Out);                        // (M2: the class's title, column and team scores)
	if (bMatchPanelHeld) Out.HeaderRows.Add({ ScoreboardCloseHint(LocalPlayer()), FString(), 0 });      // (BUG-SCOREBOARD)
	return Out.Rows.Num() > 0;
}

void UOutlawsCoop::ShowMatchPanel(float Seconds)
{
	AOutlawsPlayer* P = LocalPlayer();
	UWorld* W = CurrentWorld();
	if (!P || !W) return;
	if (!MatchTable.IsValid()) MatchTable = MakeShared<FOutlawsMatchTableSource>(this);
	AOutlawsFragTablePanel* Panel = MatchPanel.Get();
	if (!Panel)
	{
		FActorSpawnParameters SP;
		SP.ObjectFlags |= RF_Transient;
		Panel = W->SpawnActor<AOutlawsFragTablePanel>(AOutlawsFragTablePanel::StaticClass(), FTransform::Identity, SP);
		if (Panel) Panel->bOnTop = CVarOLScoreboardOnTop.GetValueOnGameThread() != 0;
		MatchPanel = Panel;
	}
	if (!Panel) return;
	bMatchPanelHeld = Seconds <= 0.f;
	const UCameraComponent* Cam = P->Camera.Get();
	const FVector Eye = Cam ? Cam->GetComponentLocation() : P->GetActorLocation();
	const FVector Fwd = Cam ? Cam->GetForwardVector() : P->GetActorForwardVector();
	Panel->SetSource(MatchTable.Get());
	FFragTableData Now;
	if (FillMatchTable(Now)) Panel->SetData(Now);
	Panel->PlaceBefore(Eye, Fwd, 300.f, 320.f, 0.f);
	MatchPanelUntil = bMatchPanelHeld ? 0.0 : FPlatformTime::Seconds() + Seconds;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH frag table shown (%s, %s): %s"), IsHost() ? TEXT("host") : WhoWord(MyIndex()),
		bMatchPanelHeld ? TEXT("until closed") : *FString::Printf(TEXT("%.0f s"), Seconds), *Now.Describe());
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH frag table drawn in %s"), *Panel->DescribeMaterial());
}

void UOutlawsCoop::HideMatchPanel(const TCHAR* Why)
{
	if (AOutlawsFragTablePanel* Panel = MatchPanel.Get())
	{
		Panel->Destroy();
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH frag table closed (%s)"), Why);
	}
	MatchPanel.Reset();
	bMatchPanelHeld = false;
}

bool UOutlawsCoop::WantsPauseScoreboard(const AOutlawsPlayer* P)
{
	const UOutlawsCoop* C = Get(P);
	return C && C->IsMatch() && C->GetSession() && C->GetSession()->MatchClass != 0;
}

void UOutlawsCoop::UpdatePauseScoreboard(AOutlawsPlayer* P, const FVector& Eye, const FVector& Centre, const FVector& RightHint, float PanelWidthCm)
{
	UOutlawsCoop* C = Get(P);
	if (!WantsPauseScoreboard(P))
	{
		HidePauseScoreboard(P);
		return;
	}
	UWorld* W = C->CurrentWorld();
	if (!W) return;
	if (!C->MatchTable.IsValid()) C->MatchTable = MakeShared<FOutlawsMatchTableSource>(C);
	AOutlawsFragTablePanel* Panel = C->PausePanel.Get();
	if (!Panel)
	{
		FActorSpawnParameters SP;
		SP.ObjectFlags |= RF_Transient;
		Panel = W->SpawnActor<AOutlawsFragTablePanel>(AOutlawsFragTablePanel::StaticClass(), FTransform::Identity, SP);
		if (!Panel) return;
		Panel->bOnTop = true;                      // (over the world, as the pause panel is: the headset's 18 cm copy of the overlay is picked inside)
		Panel->SetSource(C->MatchTable.Get());     // (the same drawing as the Scoreboard row's, looked at twice a second)
		FFragTableData Now;
		if (C->FillMatchTable(Now)) Panel->SetData(Now);
		C->PausePanel = Panel;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH pause scoreboard shown (%s): %s"), C->IsHost() ? TEXT("host") : WhoWord(C->MyIndex()), *Now.Describe());
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH pause scoreboard drawn in %s"), *Panel->DescribeMaterial());
	}
	// (REQ-SCORESIDE) to the left of the pause panel, the menu's choice of place, tipped toward the eyes
	Panel->PlaceFacing(Centre, Eye, RightHint, PanelWidthCm);
	Panel->SetActorHiddenInGame(Panel->GetCanvas() == nullptr);
}

void UOutlawsCoop::HidePauseScoreboard(AOutlawsPlayer* P)
{
	UOutlawsCoop* C = Get(P);
	if (!C) return;
	if (AOutlawsFragTablePanel* Panel = C->PausePanel.Get())
	{
		Panel->Destroy();
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH pause scoreboard closed"));
	}
	C->PausePanel.Reset();
}

void UOutlawsCoop::ToggleScoreboard(AOutlawsPlayer* P, const TCHAR* Why)
{
	UOutlawsCoop* C = Get(P);
	if (!C || !C->IsMatch()) return;
	if (C->MatchPanel.IsValid()) C->HideMatchPanel(Why);
	else C->ShowMatchPanel(0.f);
}

bool UOutlawsCoop::CloseScoreboard(AOutlawsPlayer* P, const TCHAR* Why)
{
	UOutlawsCoop* C = Get(P);
	if (!C || !C->IsMatch() || !C->MatchPanel.IsValid()) return false;
	C->HideMatchPanel(Why);
	return true;
}

FString UOutlawsCoop::ScoreboardCloseHint(const AOutlawsPlayer* P)
{
	return P && P->bVR ? FString(TEXT("Press the Menu button to close")) : FString(TEXT("Press Tab to close"));
}

// ---- its test (-OLMpTest=dm: the host with -OLMpHost=<map>, two clients joining; -OLCoopAuto -OLCoopTest=dm on all three) ---------

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TickHostMatchTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	FOutlawsMatchFlow& F = OutlawsMpHost::Flow();
	TArray<AOutlawsCoopLink*> Links;
	GetLinks(Links);
	TArray<int32> Clients;
	for (const AOutlawsCoopLink* K : Links) if (K->PlayerIndex > 0 && MatchInRules[K->PlayerIndex]) Clients.Add(K->PlayerIndex);
	auto ToAll = [&](const TCHAR* Step, int32 Arg) { for (const AOutlawsCoopLink* K : Links) if (K->PlayerIndex > 0) TestSayTo(K->PlayerIndex, Step, Arg); };
	auto Check = [](bool bOk, const FString& What) { UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH test check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What); };
	if (TestStep < 700)
	{
		TestStep = 700;
		TestAt = 0.0;
	}
	switch (TestStep)
	{
	case 700:
		// everyone in the warm-up (two clients in the rules), a few seconds of it
		if (Clients.Num() < 2 || !bMatchSetUp) return;
		if (TestAt == 0.0) { TestAt = Now + 6.0; return; }
		if (Now < TestAt) return;
		Check(F.GetState() == FOutlawsMatchFlow::EState::WarmUp, FString::Printf(TEXT("warm-up with %d players (%s)"), Dm.GetPlayers().Num(), FOutlawsMatchFlow::StateName(F.GetState())));
		Check(F.HostStart(), TEXT("the host's Start the game"));
		TestStep = 701;
		return;
	case 701:
		// the countdown runs out: everyone at a spawn point
		if (F.GetState() != FOutlawsMatchFlow::EState::Playing) return;
		Check(Dm.GetPhase() == EOutlawsDmPhase::Live, TEXT("the match is live after the countdown"));
		MatchTestShooter = Clients[0];
		MatchTestVictim = Clients[1];
		TestStep = 702;
		TestAt = Now + 4.0;                      // (past the spawn protection)
		return;
	case 702:
	case 705:
		// the shooter beside the victim, then a shot each half second until the victim is dead
		if (Now < TestAt) return;
		TestSayTo(MatchTestShooter, TEXT("dm:aim"), MatchTestVictim);
		MatchTestShots = 0;
		TestStep += 1;
		TestAt = Now + 1.5;
		return;
	case 703:
	case 706:
		if (MatchDead[MatchTestVictim])
		{
			const FOutlawsDmPlayer* S = Dm.Find(MatchTestShooter);
			Check(S && S->Frags == (TestStep == 703 ? 1 : 2), FString::Printf(TEXT("the %s killed the %s in %d shots: %d frags"), WhoWord(MatchTestShooter), WhoWord(MatchTestVictim),
				MatchTestShots, S ? S->Frags : -1));
			if (TestStep == 703)
			{
				TestSayTo(MatchTestVictim, TEXT("dm:respawn"), 0);
				TestStep = 704;
				TestAt = Now + 8.0;
			}
			else
			{
				TestStep = 707;
				TestAt = Now + 3.0;
			}
			return;
		}
		if (Now < TestAt) return;
		if (++MatchTestShots > 20)
		{
			Check(false, FString::Printf(TEXT("the %s never died (%s)"), WhoWord(MatchTestVictim), *MatchSummary()));
			TestStep = 707;
			return;
		}
		TestSayTo(MatchTestShooter, TEXT("dm:shoot"), MatchTestVictim);
		TestAt = Now + 0.5;
		return;
	case 704:
		// the victim back at a spawn point, then the second kill (the test's frag limit, 2: the match ends)
		if (MatchDead[MatchTestVictim] && Now < TestAt) return;
		Check(!MatchDead[MatchTestVictim], FString::Printf(TEXT("the %s respawned"), WhoWord(MatchTestVictim)));
		TestStep = 705;
		TestAt = Now + 4.0;
		return;
	case 707:
		if (Now < TestAt) return;
		Check(F.GetState() == FOutlawsMatchFlow::EState::Ended, FString::Printf(TEXT("the frag limit (%d) ended the match (%s)"), MatchOptions.ScoreLimit, FOutlawsMatchFlow::StateName(F.GetState())));
		Check(MatchPanel.IsValid(), TEXT("the frag table in front of the host"));
		ToAll(TEXT("dm:report"), 0);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH test: the host's summary: %s"), *MatchSummary());
		TestStep = 708;
		TestAt = Now + 2.0;
		return;
	case 708:
		if (Now < TestAt) return;
		ToAll(TEXT("quit"), 0);
		TestStep = 100;
		TestAt = Now + 3.0;
		return;
	case 100:
		if (Now < TestAt) return;
		Summary(TEXT("done"));
		QuitForTest();
		TestStep = 101;
		return;
	default:
		return;
	}
}

void UOutlawsCoop::OnMatchTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	auto Check = [](bool bOk, const FString& What) { UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH test check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What); };
	const AOutlawsRemotePlayer* R = PuppetOf(Arg);
	if (Step == TEXT("aim"))
	{
		// beside player Arg (3 m off, facing it): its body drawn here
		if (!R) { Check(false, FString::Printf(TEXT("the %s is not drawn here"), WhoWord(Arg))); return; }
		const FVector Feet = R->ShownBody();
		UWorld* W = P->GetWorld();
		const float Half = P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), Rad = P->GetCapsuleComponent()->GetScaledCapsuleRadius();
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsMatchTestAim), false, P);
		for (int32 k = 0; k < 8; ++k)
		{
			const FVector At = Feet + FRotator(0.0, 45.0 * k, 0.0).Vector() * 300.0;
			FHitResult Hit;
			if (W->LineTraceSingleByChannel(Hit, Feet, At, ECC_Visibility, Q)) continue;
			if (W->OverlapBlockingTestByChannel(At, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(Rad, Half), Q)) continue;
			P->TeleportTo(At, FRotator(0.0, (Feet - At).Rotation().Yaw, 0.0), false, true);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH test: the %s stands 3 m from the %s"), WhoWord(MyIndex()), WhoWord(Arg));
			return;
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH test: no room 3 m from the %s"), WhoWord(Arg));
	}
	else if (Step == TEXT("shoot"))
	{
		// a revolver's pellet from this player's eyes through player Arg's chest, as FireGun's would go
		if (!R) return;
		const UCameraComponent* Cam = P->Camera.Get();
		const FVector Eye = Cam ? Cam->GetComponentLocation() : P->GetActorLocation();
		const FVector Chest = R->ShownBody() + FVector(0.0, 0.0, 20.0);
		const FVector To = Eye + (Chest - Eye).GetSafeNormal() * 5000.0;
		FVector At;
		const bool bHit = ShotAtPlayers(L, Eye, To, 3.f, At);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH test: the %s shoots at the %s: %s"), WhoWord(MyIndex()), WhoWord(Arg), bHit ? TEXT("a hit") : TEXT("a miss"));
	}
	else if (Step == TEXT("respawn"))
	{
		// the death screen's button, once the respawn delay is past
		TWeakObjectPtr<AOutlawsPlayer> WeakP(P);
		FTimerHandle H;
		P->GetWorldTimerManager().SetTimer(H, FTimerDelegate::CreateWeakLambda(this, [WeakP]()
		{
			AOutlawsPlayer* Pl = WeakP.Get();
			const bool bAsked = Pl && MatchRespawn(Pl);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH test check %s: the death screen's respawn asked the host"), bAsked ? TEXT("PASS") : TEXT("FAIL"));
		}), 3.f, false);
	}
	else if (Step == TEXT("report"))
	{
		FFragTableData T;
		const bool bAny = FillMatchTable(T);
		Check(bAny && T.Rows.Num() >= 3, FString::Printf(TEXT("the %s's frag table: %s"), WhoWord(MyIndex()), *T.Describe()));
		Check(MatchPanel.IsValid(), FString::Printf(TEXT("the frag table in front of the %s"), WhoWord(MyIndex())));
	}
}
#endif // WITH_OUTLAWS_TESTS
