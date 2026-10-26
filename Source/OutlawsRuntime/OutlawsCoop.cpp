// Story co-op (tools/olue/COOP_DESIGN.md): milestone 1, the session and the lobby; milestone 2, the players' poses and the
// other player's puppet.  Everything here runs only once a player picks Co-op (or a -OLCoop* launch flag is given); single
// player keeps Mode at None and never reaches it.
#include "OutlawsCoop.h"
#include "OutlawsFlags.h"
#include "OutlawsPerf.h"
#include "OutlawsFeel.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsGeometry.h"
#include "OutlawsUnlocks.h"
#include "OutlawsStats.h"
#include "OutlawsPlayer.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsRemotePlayer.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsTextures.h"
#include "OutlawsEnemy.h"
#include "OutlawsBoxHand.h"
#include "OutlawsMovieScreen.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Engine/NetDriver.h"
#include "Engine/NetConnection.h"
#include "Engine/ActorChannel.h"
#include "Engine/NetworkObjectList.h"
#include "EngineUtils.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/WorldSettings.h"
#include "Components/CapsuleComponent.h"
#include "Camera/CameraComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Net/UnrealNetwork.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "IPAddress.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformMisc.h"
#include "UnrealClient.h"

namespace
{
	const TCHAR* const CoopMap = TEXT("/Game/Outlaws/Maps/L_Runtime");
	const TCHAR* const CoopGameMode = TEXT("/Script/OutlawsRuntime.OutlawsCoopGameMode");
	constexpr double AutoClientQuitAfter = 10.0;      // -OLCoopAuto: seconds after this machine's level is built
	constexpr double AutoHostQuitAfter = 12.0;        // (the host a little later: the client goes first)
	constexpr double AutoGiveUpAfter = 420.0;         // -OLCoopAuto: never left running (seconds since the process started)
	constexpr double AutoWorldTestGiveUp = 150.0;     // -OLCoopAuto's world checks (milestone 3 on): seconds after the level is built
	const TCHAR* const PuppetNwx = TEXT("james.nwx");   // James Anderson, the hero: cidjames.itm's PLAYER_WAX (olobj.lab; HD JAMES.SPX)

	FString AddressFile()
	{
		return FPaths::GetPath(AOutlawsLevelActor::SavePath(-1)) / TEXT("coop_address.txt");
	}

	const TCHAR* ModeName(EOutlawsCoopMode M)
	{
		return M == EOutlawsCoopMode::Host ? TEXT("host") : M == EOutlawsCoopMode::Client ? TEXT("client") : TEXT("none");
	}
}

// ---- the session actor ------------------------------------------------------------------------------------------------

AOutlawsCoopSession::AOutlawsCoopSession()
{
	bReplicates = true;
	bAlwaysRelevant = true;
	NetUpdateFrequency = 15.f;                   // (milestone 4: the enemies' places go with it, about 15 a second)
	NetPriority = 3.f;
}

void AOutlawsCoopSession::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AOutlawsCoopSession, bClientConnected);
	DOREPLIFETIME(AOutlawsCoopSession, bHostReady);
	DOREPLIFETIME(AOutlawsCoopSession, bClientReady);
	DOREPLIFETIME(AOutlawsCoopSession, LobbyLeft);
	DOREPLIFETIME(AOutlawsCoopSession, bBetweenMatches);
	DOREPLIFETIME(AOutlawsCoopSession, MatchCycle);
	DOREPLIFETIME(AOutlawsCoopSession, RandomBits);
	DOREPLIFETIME(AOutlawsCoopSession, MaxPlayers);
	DOREPLIFETIME(AOutlawsCoopSession, Connected);
	DOREPLIFETIME(AOutlawsCoopSession, Level);
	DOREPLIFETIME(AOutlawsCoopSession, Difficulty);
	DOREPLIFETIME(AOutlawsCoopSession, StartSerial);
	DOREPLIFETIME(AOutlawsCoopSession, Enemies);
	DOREPLIFETIME(AOutlawsCoopSession, EnemyEpoch);
	DOREPLIFETIME(AOutlawsCoopSession, StartKind);
	DOREPLIFETIME(AOutlawsCoopSession, InfElevators);
	DOREPLIFETIME(AOutlawsCoopSession, InfTriggers);
	DOREPLIFETIME(AOutlawsCoopSession, InfEpoch);
	DOREPLIFETIME(AOutlawsCoopSession, SpawnDifficulty);
	DOREPLIFETIME(AOutlawsCoopSession, LoadName);
	DOREPLIFETIME(AOutlawsCoopSession, bHom);
	DOREPLIFETIME(AOutlawsCoopSession, HomMission);
	DOREPLIFETIME(AOutlawsCoopSession, HomBack);
	DOREPLIFETIME(AOutlawsCoopSession, Players);
	DOREPLIFETIME(AOutlawsCoopSession, Multiplier);
	DOREPLIFETIME(AOutlawsCoopSession, Extras);
	DOREPLIFETIME(AOutlawsCoopSession, ExtrasSerial);
	DOREPLIFETIME(AOutlawsCoopSession, ExtrasLevel);
	DOREPLIFETIME(AOutlawsCoopSession, ExtrasDifficulty);
	DOREPLIFETIME(AOutlawsCoopSession, ExtrasEligible);
	DOREPLIFETIME(AOutlawsCoopSession, Rando);
	DOREPLIFETIME(AOutlawsCoopSession, MatchClass);
	DOREPLIFETIME(AOutlawsCoopSession, MatchState);
	DOREPLIFETIME(AOutlawsCoopSession, FragLimit);
	DOREPLIFETIME(AOutlawsCoopSession, TimeLimitMinutes);
	DOREPLIFETIME(AOutlawsCoopSession, TimeLeft);
	DOREPLIFETIME(AOutlawsCoopSession, MapName);
	DOREPLIFETIME(AOutlawsCoopSession, bPassworded);
	DOREPLIFETIME(AOutlawsCoopSession, Scores);
	DOREPLIFETIME(AOutlawsCoopSession, ModeItems);
	DOREPLIFETIME(AOutlawsCoopSession, RedScore);
	DOREPLIFETIME(AOutlawsCoopSession, BlueScore);
	DOREPLIFETIME(AOutlawsCoopSession, ItPlayer);
	DOREPLIFETIME(AOutlawsCoopSession, bTeams);
	DOREPLIFETIME(AOutlawsCoopSession, RegenRate);
	DOREPLIFETIME(AOutlawsCoopSession, bCheatsOff);
}

// ---- the link (one per player) ----------------------------------------------------------------------------------------

AOutlawsCoopLink::AOutlawsCoopLink()
{
	bReplicates = true;
	bAlwaysRelevant = true;
	NetUpdateFrequency = 30.f;
	NetPriority = 2.f;
}

int32 AOutlawsCoopLink::GetFunctionCallspace(UFunction* Function, FFrame* Stack)
{
	// (REQ-MPBOTS) a bot's link: nobody at the other end; its Client calls would run on the host itself
	if (bBot && Function && (Function->FunctionFlags & (FUNC_NetClient | FUNC_NetServer | FUNC_NetMulticast))) return FunctionCallspace::Absorbed;
	return Super::GetFunctionCallspace(Function, Stack);
}

void AOutlawsCoopLink::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AOutlawsCoopLink, PlayerIndex);
	DOREPLIFETIME(AOutlawsCoopLink, HorseSlot);
	DOREPLIFETIME(AOutlawsCoopLink, bBot);
	DOREPLIFETIME(AOutlawsCoopLink, bLobbyReady);
	DOREPLIFETIME_CONDITION(AOutlawsCoopLink, Pose, COND_SkipOwner);
	DOREPLIFETIME_CONDITION(AOutlawsCoopLink, Stats, COND_SkipOwner);
	DOREPLIFETIME(AOutlawsCoopLink, BuiltSerial);
	DOREPLIFETIME(AOutlawsCoopLink, Character);
	DOREPLIFETIME(AOutlawsCoopLink, Team);
	DOREPLIFETIME(AOutlawsCoopLink, Pace);
	DOREPLIFETIME(AOutlawsCoopLink, PlayerName);
	DOREPLIFETIME(AOutlawsCoopLink, NameSerial);
	DOREPLIFETIME(AOutlawsCoopLink, LevelStats);
	DOREPLIFETIME(AOutlawsCoopLink, bLateJoin);
	DOREPLIFETIME(AOutlawsCoopLink, JoinSerial);
	DOREPLIFETIME(AOutlawsCoopLink, JoinState);
	DOREPLIFETIME(AOutlawsCoopLink, BuildLine);
	DOREPLIFETIME(AOutlawsCoopLink, bAppPaused);
	DOREPLIFETIME(AOutlawsCoopLink, bMic);
}

void AOutlawsCoopLink::ServerSetReady_Implementation(bool bReady)
{
	if (!Admit(EOLGate::Ready, true)) return;
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->SetReadyFor(PlayerIndex, bReady);
}

void AOutlawsCoopLink::ServerPose_Implementation(const FOutlawsCoopPose& InPose)
{
	if (!Admit(EOLGate::Pose, Finite(InPose.Body) && Finite(InPose.Head) && Finite(InPose.LeftHand) && Finite(InPose.RightHand) && Finite(InPose.LeftHeldAt) && Finite(InPose.RightHeldAt) && Finite(InPose.LampAt) && InPose.Lamp <= 2 && !InPose.HeadRot.ContainsNaN() && Finite(InPose.HalfHeight, 500.f) && Finite(InPose.GroundSpeed, 1.0e5f) && Finite(InPose.SentAt, 1.0e9f) && InPose.LeftTool.Len() <= 32 && InPose.RightTool.Len() <= 32)) return;
	if (InPose.Seq > Pose.Seq || InPose.Seq < Pose.Seq - 1000)        // (unreliable: an older one arriving late is dropped)
	{
		Pose = InPose;
		Pose.Break = BreakAccepted;              // (REQ-FLICKRELOAD: the open barrels the host took from ServerBreakFx)
	}
}

// ---- the controller ---------------------------------------------------------------------------------------------------

AOutlawsCoopController::AOutlawsCoopController()
{
	bCanPossessWithoutAuthority = true;          // the client possesses the player it made itself (TryPlacePlayer)
}

void AOutlawsCoopController::ClientGotoState_Implementation(FName NewState)
{
	// (the host never gave this machine a pawn, so it still thinks we spectate: ignored once our own player is possessed)
	if (GetLocalRole() < ROLE_Authority && GetPawn() && NewState != NAME_Playing)
	{
		return;
	}
	Super::ClientGotoState_Implementation(NewState);
}

void AOutlawsCoopController::ClientSetViewTarget_Implementation(AActor* A, FViewTargetTransitionParams TransitionParams)
{
	if (GetLocalRole() < ROLE_Authority && GetPawn() && A != GetPawn())
	{
		return;                                  // (the view stays on our own player)
	}
	Super::ClientSetViewTarget_Implementation(A, TransitionParams);
}

// ---- the game mode (host only) ----------------------------------------------------------------------------------------

AOutlawsCoopGameMode::AOutlawsCoopGameMode()
{
	PlayerControllerClass = AOutlawsCoopController::StaticClass();
	DefaultPawnClass = nullptr;                  // each machine makes its own player (AOutlawsLevelActor::TryPlacePlayer)
	bUseSeamlessTravel = false;
}

void AOutlawsCoopGameMode::InitGameState()
{
	Super::InitGameState();
	FActorSpawnParameters SP;
	SP.Owner = this;
	SP.ObjectFlags |= RF_Transient;
	if (AOutlawsCoopSession* S = GetWorld()->SpawnActor<AOutlawsCoopSession>(AOutlawsCoopSession::StaticClass(), FTransform::Identity, SP))
	{
		S->RegenRate = OutlawsFeel::RegenPerSecond;   // REQ-REGEN2X: the host's rate, for every player
	}
}

void AOutlawsCoopGameMode::PreLogin(const FString& Options, const FString& Address, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage)
{
	Super::PreLogin(Options, Address, UniqueId, ErrorMessage);
	// N1: the host's lobby row says how many may play (2 to 8)
	const UOutlawsCoop* C = UOutlawsCoop::Get(this);
	// M1: the game's password first, before anything else is let in (optional: none set, anyone may join)
	if (C && ErrorMessage.IsEmpty())
	{
		ErrorMessage = C->CheckJoinPassword(Options);
		if (!ErrorMessage.IsEmpty())
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby: a player from %s turned away (%s)"), *Address, *ErrorMessage);
			return;
		}
	}
	const int32 Max = C ? C->MaxPlayersSetting() : 2;
	if (ErrorMessage.IsEmpty() && GetNumPlayers() >= Max)
	{
		ErrorMessage = Max == 2 ? FString(TEXT("This co-op game already has two players")) : FString::Printf(TEXT("This co-op game is full (%d players)"), Max);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby: a player from %s turned away (the game is full: %d of %d)"), *Address, GetNumPlayers(), Max);
	}
}

void AOutlawsCoopGameMode::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnPlayerLogin(NewPlayer);
}

void AOutlawsCoopGameMode::Logout(AController* Exiting)
{
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnPlayerLogout(Exiting);
	Super::Logout(Exiting);
}

void AOutlawsCoopGameMode::RestartPlayer(AController* NewPlayer)
{
	// nothing: no pawn from here for anybody (the level actor places this machine's own player; the other machine's is
	// drawn by a puppet, never a pawn of the host's)
}

// ---- the subsystem ----------------------------------------------------------------------------------------------------

UOutlawsCoop* UOutlawsCoop::Get(const UObject* Context)
{
	const UWorld* W = Context ? Context->GetWorld() : nullptr;
	UGameInstance* GI = W ? W->GetGameInstance() : nullptr;
	return GI ? GI->GetSubsystem<UOutlawsCoop>() : nullptr;
}

bool UOutlawsCoop::IsActive(const UObject* Context)
{
	const UOutlawsCoop* C = Get(Context);
	return C && C->Mode != EOutlawsCoopMode::None;
}

void UOutlawsCoop::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	bInitialized = true;
	bAuto = OLFlags::Param(TEXT("OLCoopAuto"));
	bAutoReady = bAuto || OLFlags::Param(TEXT("OLCoopReady"));
	bWorldTest = bAuto && !OLFlags::Param(TEXT("OLCoopPosesOnly"));     // (milestone 3 on: the scripted world checks)
	OLFlags::Value(TEXT("OLCoopLeaveTest"), LeaveTestWho);
	if (LeaveTestWho.IsEmpty() && OLFlags::Param(TEXT("OLCoopLeaveTest"))) LeaveTestWho = TEXT("client");
	LeaveTestWho = LeaveTestWho.ToLower();
	bMenuTest = OLFlags::Param(TEXT("OLCoopMenuTest"));
	OLFlags::Value(TEXT("OLCoopLobbyShot"), LobbyShotPrefix);     // (REQ-COOPLOBBY: the host's lobby pages photographed)
	OLFlags::Value(TEXT("OLCoopTest"), TestOnly);       // (-OLCoopAuto -OLCoopTest=inf: only milestone 5's checks)
	TestOnly = TestOnly.ToLower();
	if (OLFlags::Param(TEXT("OLKillFeedTest")) && !(OLFlags::ValueIs(TEXT("OLKillFeedTest"), TEXT("shot")) || OLFlags::ValueIs(TEXT("OLKillFeedTest"), TEXT("shotq3"))))
	{
		// (REQ-KILLFEED's two-machine test: OutlawsCoopKillFeed.cpp; the host starts the match itself)
		bAuto = bAutoReady = bWorldTest = true;
		TestOnly = TEXT("killfeed");
	}
	if (OLFlags::Param(TEXT("OLRemoteFxTest")))
	{
		// (BUG-REMOTEGUNSOUND, BUG-TNTDEATHFREEZE's two-machine test: OutlawsCoopRemoteFxTest.cpp; the host starts the match itself)
		bAuto = bAutoReady = bWorldTest = true;
		TestOnly = TEXT("remotefx");
	}
	if (OLFlags::Param(TEXT("OLFlickNetTest")))
	{
		// (REQ-FLICKRELOAD's two-machine test: OutlawsCoopFlickTest.cpp; the host starts the match itself)
		bAuto = bAutoReady = bWorldTest = true;
		TestOnly = TEXT("flicknet");
	}
	// REQ-MPLOBBY: the lobby's start rule, off where a test starts the match itself (M1's Death Match test presses Start the game)
	bLobbyRuleOff = OLFlags::Param(TEXT("OLMpNoLobbyRule")) || TestOnly == TEXT("dm") || TestOnly == TEXT("killfeed") || TestOnly == TEXT("remotefx") || TestOnly == TEXT("flicknet");
	OLFlags::Value(TEXT("OLLobbyEnterAfter"), LobbyEnterAfter);
	bLobbyEnterAfterPeer = OLFlags::Param(TEXT("OLLobbyEnterAfterPeer"));
	OLFlags::Value(TEXT("OLLobbyShot"), LobbyShotName);
	OLFlags::Value(TEXT("OLFakePlayers"), FakePlayers);           // (REQ-MPLOBBY c: stand-in players in the lists, for the fit tests)
	FakePlayers = FMath::Clamp(FakePlayers, 0, MaxPlayers - 1);
#if WITH_OUTLAWS_TESTS
	if (OLFlags::Param(TEXT("OLLobbyRuleTest"))) RunLobbyRuleTest();
	{
		extern bool OlExitAt(const TCHAR* Point);
		OlExitAt(TEXT("init"));                 // (BUG-EXITCRASH: before the first frame, as the old lobby rule test quit)
	}
#endif
	bLaunchHost = OLFlags::Param(TEXT("OLCoopHost"));
	OLFlags::Value(TEXT("OLCoopJoin"), LaunchJoin);
	OLFlags::Value(TEXT("OLCoopJoinDelay"), JoinDelay);
	// REQ-MPMENU: -OLMenuWalk=coop|comp with -OLCoopHost or -OLCoopJoin=<address>: the host or join made through the title
	// menu's rows as a player presses them (New Game > Multiplayer > Co-op or Competitive > Host Game / Join Game)
	OLFlags::Value(TEXT("OLMenuWalk"), MenuWalk);
	OLFlags::Value(TEXT("OLCoopBadGuys"), TestBadGuys);
	OLFlags::Value(TEXT("OLCoopBench"), CoopBenchSec);
	OLFlags::Value(TEXT("OLCoopMaxPlayers"), TestMaxPlayers);     // (N1: the host's lobby row for this run)
	bTestNetAddr = OLFlags::Param(TEXT("OLCoopNetAddr"));      // (the lobby's Show internet address pressed for this run)
	OLFlags::Value(TEXT("OLHorseTest"), HorseTestTag);             // (REQ-COOPHORSES: OutlawsCoopHorses.cpp)
	OLFlags::Value(TEXT("OLHorseQuitAfter"), HorseQuitAfter);
	OLFlags::Value(TEXT("OLCoopPlayers"), PartyPlayers);          // (N1's party test: the host starts once that many are in)
	if (PartyPlayers > 2 && TestMaxPlayers < PartyPlayers) TestMaxPlayers = PartyPlayers;      // (R14's test: the host's multiplier for this run)      // (R7's test: the client connects once the host is playing)
	FString Lv;
	if (OLFlags::Value(TEXT("OLCoopLevel"), Lv) && !Lv.IsEmpty()) LevelToStart = Lv.ToLower();
	// M1: -OLMpHost=<map: its RCM or its level> hosts a Death Match (as the Host Game page's Start), -OLMpFrags=<n>
	// -OLMpMinutes=<n> its limits for a test
	if (OLFlags::Value(TEXT("OLMpHost"), LaunchMatchMap) && !LaunchMatchMap.IsEmpty()) bLaunchHost = true;
	OLFlags::Value(TEXT("OLMpFrags"), LaunchMatchFrags);
	// M2: -OLMpClass=<1..6, or ctf tag chicken document team dm> -OLMpTeams (Secret Document red against blue) -OLMpScoreSeconds=<n>
	OLFlags::Value(TEXT("OLMpClass"), LaunchMatchClass);
	bLaunchMatchTeams = OLFlags::Param(TEXT("OLMpTeams"));
	OLFlags::Value(TEXT("OLMpCheats"), LaunchMatchCheats);
	OLFlags::Value(TEXT("OLMpScoreSeconds"), LaunchScoreSeconds);
	// M1: -OLCoopPassword=<it> (a host's game password, for a test), -OLCoopJoinPassword=<it> (a joiner's)
	FString Pw;
	if (OLFlags::Value(TEXT("OLCoopPassword"), Pw)) HostPassword = CleanPassword(Pw);
	if (OLFlags::Value(TEXT("OLCoopJoinPassword"), Pw)) JoinPassword = CleanPassword(Pw);
	OLFlags::Value(TEXT("OLMpMinutes"), LaunchMatchMinutes);
	OLFlags::Value(TEXT("OLMpStartWhen"), LaunchStartWhen);   // (a desktop host has no pause menu to press Start the game)
	OLFlags::Value(TEXT("OLMpHostRespawn"), LaunchHostRespawn);
	bLaunchRotate = OLFlags::Param(TEXT("OLMpRotate"));         // (a launched match: a random stock map after each one)
	// REQ-MPLOBBY (b): a launched host's "Random each match" rows; the joiners' wait for the host's next match (decision 3: 3
	// minutes); the tests' presses on the score screen, the locked lobby and the Host Game page, and their pictures
	bLaunchRandomMap = OLFlags::Param(TEXT("OLMpRandomMap"));
	bLaunchRandomClass = OLFlags::Param(TEXT("OLMpRandomGame"));
	OLFlags::Value(TEXT("OLMpNextTimeout"), NextTimeout);
	NextTimeout = FMath::Max(5.f, NextTimeout);
	OLFlags::Value(TEXT("OLMpNextTest"), NextTest);
	NextTest = NextTest.ToLower();
	OLFlags::Value(TEXT("OLMpNextShot"), NextShotTag);
#if WITH_OUTLAWS_TESTS
	if (OLFlags::Param(TEXT("OLMpRandomTest"))) RunRandomTest();
#endif
	OLFlags::Value(TEXT("OLMpBots"), LaunchBots);              // (REQ-MPBOTS: a test's bots, -OLMpBotSkill=<0..2>)
	OLFlags::Value(TEXT("OLMpBotSkill"), LaunchBotSkill);
	// the Host Game page's Start (OutlawsMpHost::Host) hosts through the session
	{
		TWeakObjectPtr<UOutlawsCoop> Weak(this);
		OutlawsMpHost::OnHost() = [Weak](const FOutlawsMpHostOptions& O)
		{
			// (REQ-MPLOBBY b: between matches the page's Start is the next match in this session, not a new one)
			UOutlawsCoop* C = Weak.Get();
			if (!C) return false;
			const AOutlawsCoopSession* S = C->GetSession();
			if (C->IsHost() && S && S->bBetweenMatches) return C->StartNextMatch(O, TEXT("the Host Game page's Start"));
			return C->HostMatch(O);
		};
	}
	// milestone 8: -OLCoopLoad=<slot|auto> (the host's lobby starts from that save), -OLLoadTest=<...> (single player loads it
	// from the title menu and reports; a test).  (R7: "coopauto" is the co-op host's own autosave again, coop_autosave.json)
	auto SlotOf = [](const FString& V) { return V.Equals(TEXT("auto"), ESearchCase::IgnoreCase) ? -1 : V.Equals(TEXT("coopauto"), ESearchCase::IgnoreCase) ? -2 : FCString::Atoi(*V); };
	FString Ld;
	if (OLFlags::Value(TEXT("OLCoopLoad"), Ld) && !Ld.IsEmpty())
	{
		if (Ld.Equals(TEXT("resume"), ESearchCase::IgnoreCase)) bTestResume = true;     // (the lobby's Resume Game row, picked by the test)
		else LobbyLoadSlot = SlotOf(Ld);
	}
	if (OLFlags::Value(TEXT("OLLoadTest"), Ld) && !Ld.IsEmpty()) SoloLoadSlot = SlotOf(Ld);
	// 2026-10-02 decision 6's test: -OLCoopSpeed=<the movement speed slider's value, 0.2..0.75> for this instance's session only
	// (the settings file keeps the player's own)
	FString Sp;
	if (OLFlags::Value(TEXT("OLCoopSpeed"), Sp) && !Sp.IsEmpty()) CoopSpeedArg = AOutlawsLevelActor::ClampGameSpeed(FCString::Atof(*Sp));       // (BUG-SPEEDCAP: the top is 150 %)
	OLFlags::Value(TEXT("OLCoopPauseSec"), PauseTestSec);   // (BUG-COOPTIMEOUT's tests: the client's pause, seconds)
	bLaunchPending = bLaunchHost || !LaunchJoin.IsEmpty();
#if WITH_OUTLAWS_TESTS
	OLFlags::Value(TEXT("OLExtrasSurvey"), ExtrasSurveyDir);   // (2026-10-02: the extra bad guys' lists for every story level, a test)
	InitUpnpTests();                 // (REQ-UPNP: -OLUpnpSelfTest, -OLUpnpProbe; OutlawsCoopUpnp.cpp)
#endif
	if (GEngine)
	{
		NetFailHandle = GEngine->OnNetworkFailure().AddUObject(this, &UOutlawsCoop::HandleNetworkFailure);
		BindAppPause();                  // (BUG-COOPTIMEOUT: the app's pause and return, OutlawsCoopPauseHold.cpp)
		TravelFailHandle = GEngine->OnTravelFailure().AddUObject(this, &UOutlawsCoop::HandleTravelFailure);
	}
	if (bLaunchPending)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP launch: %s%s, level %s"), bLaunchHost ? TEXT("host") : *(TEXT("join ") + LaunchJoin),
			bAuto ? TEXT(" (auto test)") : bAutoReady ? TEXT(" (auto ready)") : TEXT(""), *LevelToStart);
	}
}

void UOutlawsCoop::Deinitialize()
{
	if (GEngine)
	{
		GEngine->OnNetworkFailure().Remove(NetFailHandle);
		GEngine->OnTravelFailure().Remove(TravelFailHandle);
	}
	UnbindAppPause();
	ResetVoice();
	StopUpnp(true);                                  // (REQ-UPNP: quitting removes the router's mapping before the game goes)
	bInitialized = false;
	Super::Deinitialize();
}

TStatId UOutlawsCoop::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UOutlawsCoop, STATGROUP_Tickables);
}

ETickableTickType UOutlawsCoop::GetTickableTickType() const
{
	return IsTemplate() ? ETickableTickType::Never : ETickableTickType::Conditional;
}

bool UOutlawsCoop::IsTickable() const
{
	if (!bInitialized) return false;
	if (Mode != EOutlawsCoopMode::None || IsTestRun() || !PendingToast.IsEmpty() || UpnpBusy()) return true;
	// (REQ-VOICEPAGE: outside a session too while the Voice Chat page is up, so that its meter opens the microphone and moves,
	// and once more after, so that it closes)
	const AOutlawsPlayer* P = LocalPlayer();
	return (P && P->IsMenuOpen() && P->CoopMenuPage() == TEXT("#voice")) || MicMeterOpen(this);
}

UWorld* UOutlawsCoop::GetTickableGameObjectWorld() const
{
	return CurrentWorld();
}

UWorld* UOutlawsCoop::CurrentWorld() const
{
	const UGameInstance* GI = GetGameInstance();
	return GI ? GI->GetWorld() : nullptr;
}

AOutlawsLevelActor* UOutlawsCoop::FindLevel() const
{
	if (UWorld* W = CurrentWorld())
	{
		for (TActorIterator<AOutlawsLevelActor> It(W); It; ++It) return *It;
	}
	return nullptr;
}

AOutlawsPlayer* UOutlawsCoop::LocalPlayer() const
{
	UWorld* W = CurrentWorld();
	APlayerController* PC = W ? W->GetFirstPlayerController() : nullptr;
	return PC ? Cast<AOutlawsPlayer>(PC->GetPawn()) : nullptr;
}

AOutlawsCoopSession* UOutlawsCoop::GetSession() const
{
	if (!SessionCache.IsValid())
	{
		if (UWorld* W = CurrentWorld())
		{
			for (TActorIterator<AOutlawsCoopSession> It(W); It; ++It) { SessionCache = *It; break; }
		}
	}
	return SessionCache.Get();
}

AOutlawsCoopLink* UOutlawsCoop::GetLink(int32 PlayerIndex) const
{
	if (UWorld* W = CurrentWorld())
	{
		for (TActorIterator<AOutlawsCoopLink> It(W); It; ++It)
		{
			if (It->PlayerIndex != PlayerIndex || It->IsActorBeingDestroyed()) continue;
			// (R8, found under simulated lag: the client's own link can arrive before its owner does; its Server calls then go
			// nowhere, "No owning connection", and the ready, the name and the pace, each sent once, were lost. Not ours
			// to use until it is)
			if (Mode == EOutlawsCoopMode::Client && PlayerIndex > 0 && PlayerIndex == MyIndex() && !It->GetNetConnection()) continue;
			return *It;
		}
	}
	return nullptr;
}

int32 UOutlawsCoop::MyIndex() const
{
	if (Mode != EOutlawsCoopMode::Client) return 0;
	if (MyIndexCache > 0) return MyIndexCache;
	// (N1) the client's own link is the one this machine's connection owns; the other clients' come without an owner here
	if (UWorld* W = CurrentWorld())
	{
		for (TActorIterator<AOutlawsCoopLink> It(W); It; ++It)
		{
			if (It->PlayerIndex > 0 && It->GetNetConnection())
			{
				MyIndexCache = It->PlayerIndex;
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP this machine is player %d (%s)"), MyIndexCache, WhoWord(MyIndexCache));
				return MyIndexCache;
			}
		}
	}
	return 1;
}

void UOutlawsCoop::GetLinks(TArray<AOutlawsCoopLink*>& Out) const
{
	Out.Reset();
	if (UWorld* W = CurrentWorld())
	{
		for (TActorIterator<AOutlawsCoopLink> It(W); It; ++It)
		{
			if (!It->IsActorBeingDestroyed() && It->PlayerIndex >= 0 && It->PlayerIndex < MaxPlayers) Out.Add(*It);
		}
	}
	Out.Sort([](const AOutlawsCoopLink& A, const AOutlawsCoopLink& B) { return A.PlayerIndex < B.PlayerIndex; });
}

bool UOutlawsCoop::OtherPlayerHolds(int32 EnemyIndex) const
{
	if (EnemyIndex < 0) return false;
	TArray<AOutlawsCoopLink*> Links;
	GetLinks(Links);
	const int32 Me = MyIndex();
	for (const AOutlawsCoopLink* K : Links)
	{
		if (K->PlayerIndex != Me && K->Pose.Seq > 0 && (K->Pose.LeftHeld == EnemyIndex || K->Pose.RightHeld == EnemyIndex)) return true;
	}
	return false;
}

void UOutlawsCoop::ForClients(TFunctionRef<void(AOutlawsCoopLink&)> Do, int32 Except) const
{
	if (!IsHost()) return;
	TArray<AOutlawsCoopLink*> Links;
	GetLinks(Links);
	for (AOutlawsCoopLink* K : Links)
	{
		if (K->PlayerIndex > 0 && K->PlayerIndex != Except) Do(*K);
	}
}

int32 UOutlawsCoop::PartnerIndex() const
{
	if (Mode != EOutlawsCoopMode::Host) return 0;
	TArray<AOutlawsCoopLink*> Links;
	GetLinks(Links);
	for (const AOutlawsCoopLink* K : Links)
	{
		if (K->PlayerIndex > 0) return K->PlayerIndex;
	}
	return 1;
}

AOutlawsRemotePlayer* UOutlawsCoop::PuppetOf(int32 Who) const
{
	return Who >= 0 && Who < MaxPlayers ? Puppets[Who].Get() : nullptr;
}

void UOutlawsCoop::DestroyPuppets()
{
	for (TWeakObjectPtr<AOutlawsRemotePlayer>& R : Puppets)
	{
		if (AOutlawsRemotePlayer* A = R.Get()) A->Destroy();
		R.Reset();
	}
}

int32 UOutlawsCoop::MaxPlayersSetting() const
{
	if (TestMaxPlayers > 0) return FMath::Clamp(TestMaxPlayers, 2, int32(MaxPlayers));
	if (IsMatch()) return FMath::Clamp(MatchOptions.MaxPlayers, 2, int32(MaxPlayers));          // (M1: the Host Game page's row, 16 on a PC)
	const AOutlawsLevelActor* L = FindLevel();
	return L ? FMath::Clamp(L->CoopMaxPlayers, 2, int32(MaxCoopPlayers)) : 2;
}

const TCHAR* UOutlawsCoop::WhoWord(int32 Who)
{
	// (with two players the words the logs always had; N1's other clients by their number)
	static const TCHAR* const Words[] = { TEXT("host"), TEXT("client"), TEXT("client 2"), TEXT("client 3"), TEXT("client 4"), TEXT("client 5"), TEXT("client 6"), TEXT("client 7"),
		TEXT("client 8"), TEXT("client 9"), TEXT("client 10"), TEXT("client 11"), TEXT("client 12"), TEXT("client 13"), TEXT("client 14"), TEXT("client 15") };
	static_assert(UE_ARRAY_COUNT(Words) == MaxPlayers, "a word for every player");
	return Who >= 0 && Who < MaxPlayers ? Words[Who] : TEXT("nobody");
}

void UOutlawsCoop::RefreshLobby()
{
	// (N1, the host) the session's lobby words from the links: anyone besides the host here, all of them ready, how many
	AOutlawsCoopSession* S = GetSession();
	if (!S || Mode != EOutlawsCoopMode::Host) return;
	TArray<AOutlawsCoopLink*> Links;
	GetLinks(Links);
	int32 Clients = 0;
	bool bAllReady = true;
	for (const AOutlawsCoopLink* K : Links)
	{
		if (K->PlayerIndex == 0) continue;
		++Clients;
		bAllReady = bAllReady && K->bLobbyReady;
	}
	const bool bConnected = Clients > 0, bReady = Clients > 0 && bAllReady;
	if (S->bPassworded != !HostPassword.IsEmpty())
	{
		S->bPassworded = !HostPassword.IsEmpty();       // (M1: the lobby says a password is set)
		S->ForceNetUpdate();
	}
	const int32 Max = MaxPlayersSetting();
	if (S->bClientConnected != bConnected || S->bClientReady != bReady || S->Connected != Clients + 1 || S->MaxPlayers != Max)
	{
		S->bClientConnected = bConnected;
		S->bClientReady = bReady;
		S->Connected = Clients + 1;
		S->MaxPlayers = Max;
		S->ForceNetUpdate();
		++Version;
	}
}

void UOutlawsCoop::ResetSession()
{
	RemoveAllBots(TEXT("the session ends"));
	StopUpnp(false);                                 // (REQ-UPNP: the router's mapping goes with the session: Leave, End Game)
	Mode = EOutlawsCoopMode::None;
	ConnectingTo.Reset();
	bJoinFromMenu = false;
	StartedSerial = 0;
	bLoggedConnected = false;
	LoggedReady = -1;
	LoggedReadyBits = MAX_uint32;
	bAutoReadySent = false;
	RFStep = 0;
	RFClientStep = 0;
	BothReadyAt = 0.0;
	bLobbyShot = false;
	bLevelBuilt = false;
	BuiltLevel.Reset();
	SessionCache.Reset();
	FMemory::Memzero(LastPoseSeq);
	FMemory::Memzero(LastPoseLogAt);
	FMemory::Memzero(LoggedOtherStats);
	FMemory::Memzero(OtherDeadAt);
	FMemory::Memzero(OtherAwayAt);
	PosesReceived = 0;
	FirstPoseAt = 0.0;
	ShotStep = 0;
	SeenDistance = -1.f;
	DestroyPuppets();
	MyIndexCache = -1;
	FMemory::Memzero(LastPoseSeq, sizeof(LastPoseSeq));
	ResetWorld();
	Deferred.Reset();
	SentStats = FOutlawsCoopStats();
	FMemory::Memzero(LoggedOtherStats, sizeof(LoggedOtherStats));
	NextStatsAt = 0.0;
	TestStep = 0;
	TestAt = 0.0;
	for (FOutlawsCoopRemoteView& V : RemoteViews) V = FOutlawsCoopRemoteView();
	EnemySnapsSent = EnemySnapsApplied = EnemyHitsSent = EnemyHitsDealt = EnemyDeathsSent = EnemyDeathsSeen = EnemyShotsAtClient = 0;
	bEnemyCountWarned = false;
	InfSeen.Reset();
	InfTriggersSeen.Reset();
	InfCauser = -1;
	InfScopeDepth = 0;
	bInfCauserFromScope = false;
	InfEventsSent = InfEventsRun = InfSoundsSent = InfSoundsPlayed = InfMovesLogged = 0;
	bStartingLevel = bApplyingLevelEnd = bWaitingForStart = bApplyingSkip = false;
	bLocalDead = bOtherDead = bBothDeadRestart = false;
	bLocalAway = false;
	FMemory::Memzero(OtherDeadAt, sizeof(OtherDeadAt));
	FMemory::Memzero(OtherAwayAt, sizeof(OtherAwayAt));
	PoseRateLogged.Reset();
	BothDeadSince = ReviveSentAt = MenuShieldLogAt = 0.0;
	KeysDropped = Revives = Respawns = MenusOpened = HurtsDropped = 0;
	ExtrasMadeHere = ExtrasInWalls = ExtrasListed = 0;
	ExtrasHow.Reset();
	ExtrasWaitSince = 0.0;
	VerbBlastsSent = VerbBlastsShown = VerbBitsSent = VerbBitsApplied = VerbShotsSent = VerbShotsShown = VerbThrowsSent = VerbThrowsShown = PlateStops = VerbLogLines = 0;
	bApplyingVerb = false;
	Player2Json.Reset();
	PlayerJsons.Reset();
	SentPlayer2.Reset();
	NextPlayer2At = Player2FullAt = 0.0;
	SnapParts.Reset();
	SnapEpoch = LoadEpoch = -1;
	SnapGot = 0;
	SnapObj.Reset();
	bHostLoadPending = false;
	SavesWritten = SnapshotsSent = SnapshotsApplied = 0;
	GrabsByHost = GrabsByClient = ThrowsByHost = ThrowsByClient = GrabsRefused = GrabsLetGo = 0;
	TestGrabHand = 0;
	// 2026-10-02 decisions: the gun drops, the body knocks, the shared paces (the level's own pace again: single player's)
	GunsDropped = BodyKnocks = 0;
	GroundNameSuffix.Reset();
	bResumeChosen = false;
	FirstLobbyAt = 0.0;
	SentPace = -1.f;
	LoggedPace.Reset();
	SentName.Reset();
	bNameSent = false;
	FMemory::Memzero(LoggedNameSerial);
	// R7: the late join's own state (a second join in the same run says hello again)
	JoinAsked.Reset();
	JoinBusyLogged.Reset();
	NextHoldLevel.Reset();
	EndDoneSent = 0;
	bJoinHelloSent = false;
	bJoinApply = false;
	JoinSpawnDifficulty = -1;
	LoggedJoinState = 0;
	EndJoinReplay(TEXT("the session ended"));
	bReconnectAttempt = false;
	bBuildSent = false;
	PlayerListPage = 0;
	Lobby.Reset();
	FMemory::Memzero(LobbyTeam);
	bEnterPressed = false;
	MutedBits = 0;
	SentMic = -1;
	LobbyOpenTryAt = 0.0;
	// REQ-MPLOBBY (b): the match's end and the next match
	EndedCycle = SeenCycle = -1;
	BetweenSince = HostEndAt = ScoreAt = WaitSince = NextMenuTryAt = 0.0;
	ScorePhase = 0;
	bNextLobbyArmed = false;
	// M1: the match's state (the Host Game page's options stay: they are the page's)
	bMatchPending = false;
	bMatchSetUp = false;
	MatchSerial = -1;
	Dm = FOutlawsDeathmatch();
	if (IsHostFlowOwner()) OutlawsMpHost::Flow().Stop();
	if (AOutlawsFragTablePanel* Panel = MatchPanel.Get()) Panel->Destroy();
	MatchPanel.Reset();
	if (AOutlawsFragTablePanel* Panel = PausePanel.Get()) Panel->Destroy();      // (REQ-SCOREABOVE)
	PausePanel.Reset();
	LoggedMatchState = 255;
	LastMatchTickAt = 0.0;
	FMemory::Memzero(MatchInRules);
	FMemory::Memzero(MatchDead);
	FMemory::Memzero(LastAttackAt);
	MatchHitsSent = MatchHitsDealt = MatchHitsRefused = MatchRespawns = 0;
	FOutlawsMpCharacter::bMatchMenu = false;
	FOutlawsMpCharacter::OnLocalChoice = nullptr;
	// M2: the modes' state and their sprites
	Modes = FOutlawsMatchModes();
	for (TPair<int32, TWeakObjectPtr<AOutlawsSpriteActor>>& Kv : ModeSprites) if (AOutlawsSpriteActor* A = Kv.Value.Get()) A->Destroy();
	ModeSprites.Reset();
	ModeSpriteNwx.Reset();
	LoggedModeItems = -1;
	if (AOutlawsLevelActor* L = FindLevel())
	{
		L->CoopEnemyPaceBase = -1.f;
		L->CoopWaitPace = -1.f;
		L->CoopExtras.Reset();                       // (2026-10-02: a session's extra bad guys never reach single player)
		L->CoopExtrasLevel.Reset();
		L->CoopExtrasDifficulty = -1;
	}
	if (UWorld* W = SweptWorld.Get()) W->OnPreTickFlush().Remove(SweepHandle);
	SweptWorld.Reset();
	ResetVoice();                                    // (voice chat, user 2026-10-02: the microphone closed, the talker gone)
	++Version;
}

void UOutlawsCoop::Host()
{
	UWorld* W = CurrentWorld();
	if (!W) return;
	ResetSession();
	Mode = EOutlawsCoopMode::Host;
	bInternetAsked = false;
	InternetAddress.Reset();
	{
		const TMap<FString, FString> Set = AOutlawsLevelActor::ReadSettings();
		const FString* X = Set.Find(TEXT("CoopShowInternetAddress"));
		bShowInternet = bTestNetAddr || (!IsTestRun() && X && *X == TEXT("1"));
	}
	LanAddress = FindLanAddress(&OtherAddresses);
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP hosting: reopening %s as a listen server (this machine's address %s)"), CoopMap, *LanAddress);
	if (OLFlags::Param(TEXT("OLRootDiag"))) AOutlawsLevelActor::LogRootedWorldObjects(W, TEXT("hosting, before the map load"));
	UGameplayStatics::OpenLevel(W, FName(CoopMap), true, FString::Printf(TEXT("listen?game=%s"), CoopGameMode));
}

void UOutlawsCoop::Join(const FString& Address, bool bFromMenu)
{
	UWorld* W = CurrentWorld();
	FString Ip = Address.TrimStartAndEnd();
	if (!W || !GEngine || Ip.IsEmpty()) return;
	ResetSession();
	Mode = EOutlawsCoopMode::Client;
	JoinError.Reset();
	bJoinFromMenu = bFromMenu;
	JoinStartedAt = FPlatformTime::Seconds();
	FString Host = Ip, PortText;
	if (!Ip.Split(TEXT(":"), &Host, &PortText))
	{
		// (REQ-MPMENU's -OLMenuWalk test: the pad has no colon, so a typed join goes to 7777; the test's host listens on the
		// launch address's port, kept off the player's own game)
		FString WalkHost, WalkPort;
		PortText = !MenuWalk.IsEmpty() && LaunchJoin.Split(TEXT(":"), &WalkHost, &WalkPort) ? WalkPort : FString::FromInt(Port);
	}
	ConnectingTo = Host;
	ConnectingPort = PortText;
	if (bFromMenu)
	{
		JoinAddress = Host;
		WriteAddressFile();              // (remembered for next time; a -OLCoopJoin launch leaves the file alone)
	}
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP client connecting to %s port %s"), *Host, *PortText);
	// (M1: the game's password, if this player has one for it, rides on the address as an option the host's PreLogin reads)
	if (JoinPassword.IsEmpty()) { ReadRecentPasswords(); JoinPassword = RecentPasswords.FindRef(Host); }
	const FString Url = FString::Printf(TEXT("%s:%s"), *Host, *PortText) + (JoinPassword.IsEmpty() ? FString() : TEXT("?Password=") + JoinPassword);
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP joining %s"), JoinPassword.IsEmpty() ? TEXT("without a password") : TEXT("with a password"));
	GEngine->SetClientTravel(W, *Url, TRAVEL_Absolute);
}

void UOutlawsCoop::EndSession(const FString& Reason, bool bTravel)
{
	if (Mode == EOutlawsCoopMode::None) return;
	// (REQ-UNLOCKS) the session ending after the story's final level was won here (the host ends it as its cutscene ends):
	// this player was in it, so this profile earns
	if (AOutlawsLevelActor* LU = FindLevel()) OutlawsUnlocks::EarnPending(LU, TEXT("the co-op session's end"));
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP session ended: %s"), *Reason);
	PendingToast = FString::Printf(TEXT("Co-op ended: %s"), *Reason);
	if (IsHost() && IsMatch() && bTravel && HostEndAt == 0.0)        // (End game told them already)
	{
		// (REQ-MPLOBBY b: a host's match ends for everyone: each joiner to its main menu with "The host ended the game")
		int32 Told = 0;
		ForClients([&Told](AOutlawsCoopLink& CL) { if (!CL.bBot) { CL.ClientHostEnded(); ++Told; } });
		if (Told > 0) UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH the host ended the game: %d joiners told"), Told);
	}
	EndedAt = FPlatformTime::Seconds();
	ResetSession();
	UWorld* W = CurrentWorld();
	if (!bTravel || !W) return;
	if (W->GetNetMode() != NM_Standalone)
	{
		// back to this machine's own single-player world: the map again, no listen, the project's game mode
		UGameplayStatics::OpenLevel(W, FName(CoopMap), true);
	}
	else
	{
		// (never connected: a join still on its way is dropped, the title menu as it was)
		if (GEngine) GEngine->CancelPending(W);
		if (AOutlawsLevelActor* L = FindLevel()) L->ShowFrontMenu();
	}
}

void UOutlawsCoop::LeaveSession(const UObject* Context, const FString& Reason)
{
	if (UOutlawsCoop* C = Get(Context)) C->EndSession(Reason, true);
}

void UOutlawsCoop::NotifyLevelBeginPlay(AOutlawsLevelActor* Level)
{
	if (UOutlawsCoop* C = Level ? Get(Level) : nullptr) C->OnLevelBeginPlay(Level);
}

void UOutlawsCoop::OnLevelBeginPlay(AOutlawsLevelActor* Level)
{
	UWorld* W = Level->GetWorld();
	if (!W || !W->IsGameWorld()) return;
	const ENetMode NM = W->GetNetMode();
	if (bLaunchPending && NM == NM_Standalone)
	{
		// the launch flags act once, on the first world (-OLCoopJoinDelay: the join a while later, from Tick)
		if (!bLaunchHost || !MenuWalk.IsEmpty())
		{
			// (REQ-MPMENU: a menu-walk host waits for its title menu too, and presses its way there from Tick)
			// (M1, found with three players: a join on the first frame raced this machine's own title screen, whose backdrop
			// level was still being set up and was built over the joined one; a joiner now waits for its title menu, as a
			// player on the join page always does, 10 s at most)
			if (LaunchAt == 0.0) LaunchAt = FPlatformTime::Seconds() + FMath::Max(0.f, JoinDelay);
			return;
		}
		bLaunchPending = false;
		if (bLaunchHost && !LaunchMatchMap.IsEmpty())
		{
			// (M1's test: a match on that map, as the Host Game page would host it)
			FOutlawsMpHostOptions O;
			O.Load();
			O.MapFile = LaunchMatchMap;
			O.GameClass = EOutlawsGameClass::DeathMatch;
			const FString Cl = LaunchMatchClass.ToLower();
			const int32 N = FCString::Atoi(*Cl);
			if (N >= 1 && N <= 6) O.GameClass = EOutlawsGameClass(N);
			else if (Cl == TEXT("ctf")) O.GameClass = EOutlawsGameClass::CaptureTheFlag;
			else if (Cl == TEXT("tag")) O.GameClass = EOutlawsGameClass::Tag;
			else if (Cl == TEXT("chicken")) O.GameClass = EOutlawsGameClass::Chicken;
			else if (Cl == TEXT("document")) O.GameClass = EOutlawsGameClass::SecretDocument;
			else if (Cl == TEXT("team")) O.GameClass = EOutlawsGameClass::TeamPlay;
			O.bTeams = bLaunchMatchTeams;
			if (LaunchMatchCheats >= 0) O.bAllowCheats = LaunchMatchCheats != 0;     // (BUG-ALLOWCHEATS)
			if (LaunchMatchFrags >= 0) O.ScoreLimit = LaunchMatchFrags;
			if (LaunchMatchMinutes >= 0) O.TimeLimitMinutes = LaunchMatchMinutes;
			O.MaxPlayers = TestMaxPlayers > 0 ? FMath::Clamp(TestMaxPlayers, 2, OutlawsMpHost::MaxPlayersCap()) : OutlawsMpHost::MaxPlayersCap();
			// (it starts itself once that many people are in, the host counted, bots not; 3 unless -OLMpStartWhen= says, 0 the host starts it)
			O.StartWhen = LaunchStartWhen >= 0 ? LaunchStartWhen : 3;
			if (LaunchBots >= 0)
			{
				O.bBots = LaunchBots > 0;
				O.BotCount = FMath::Max(1, LaunchBots);
			}
			if (LaunchBotSkill >= 0) O.BotSkill = LaunchBotSkill;
			O.bRandomMap = bLaunchRandomMap;         // (REQ-MPLOBBY b: the launch's own, never the page's saved rows)
			O.bRandomClass = bLaunchRandomClass;
			if (!HostMatch(O)) Host();
		}
		else if (bLaunchHost) Host();
		else Join(LaunchJoin, false);
		return;
	}
	if (CoopSpeedArg > 0.f && Mode != EOutlawsCoopMode::None)
	{
		// -OLCoopSpeed (decision 6's test): this instance's speed setting for the session; the settings file keeps the player's
		if (Level->GameSpeedInFile < 0.f) Level->GameSpeedInFile = Level->GameSpeed;
		Level->GameSpeed = AOutlawsLevelActor::ClampGameSpeed(CoopSpeedArg);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP speed setting for this test: %.2f (pace %.3f; the settings file keeps %.2f)"), Level->GameSpeed, Level->Pace(), Level->GameSpeedInFile);
	}
	if (Mode == EOutlawsCoopMode::Host)
	{
		if (NM != NM_ListenServer || !W->GetNetDriver())
		{
			EndSession(TEXT("this machine could not listen for the other player (is port 7777 in use?)"), false);
			return;
		}
		const TSharedPtr<const FInternetAddr> Local = W->GetNetDriver()->GetLocalAddr();
		ListenPort = Local.IsValid() ? Local->GetPort() : Port;
		if (LanAddress.IsEmpty()) LanAddress = FindLanAddress(&OtherAddresses);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP host listening on port %d (local network address %s%s)"), ListenPort, *LanAddress,
			OtherAddresses.IsEmpty() ? TEXT("") : *FString::Printf(TEXT("; also %s"), *OtherAddresses));
		// (R8: the internet address is asked only once the host has pressed the lobby's Show internet address, coop:netaddr;
		// remembered on this machine after that)
		if (bShowInternet) AskInternetAddress();
		else UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP internet address not asked (the lobby's Show internet address is off)"));
		StartUpnp();                                 // (REQ-UPNP: once per hosting; the router asked on a worker thread)
		// what the host's machine replicates: the session's own actors and the engine's player bookkeeping, nothing of the
		// level (each machine builds its own); swept just before every network send
		if (SweptWorld.Get() != W)
		{
			SweepHandle = W->OnPreTickFlush().AddUObject(this, &UOutlawsCoop::SweepReplication);
			SweptWorld = W;
		}
		++Version;
	}
	else if (Mode == EOutlawsCoopMode::Client)
	{
		if (NM != NM_Client)
		{
			// a single-player world while still joining: the engine brought this machine home (the join failed)
			EndSession(TEXT("the connection to the host did not hold"), false);
			return;
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP client joined %s: the lobby"), *ConnectingTo);
		if (!bAuto && !JoinPassword.IsEmpty()) NoteRecentPassword(ConnectingTo, JoinPassword);     // (M1: remembered for this host)
		if (!ReconnectTo.IsEmpty())
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP reconnected to %s (try %d)"), *ReconnectTo, ReconnectTries);     // (R8)
			ReconnectTo.Reset();
		}
		if (!bAuto) NoteRecent(ConnectingPort.IsEmpty() || ConnectingPort == FString::FromInt(Port) ? ConnectingTo : ConnectingTo + TEXT(":") + ConnectingPort, FString());
		++Version;
	}
}

void UOutlawsCoop::SweepReplication(float)
{
	UWorld* W = SweptWorld.Get();
	UNetDriver* D = W ? W->GetNetDriver() : nullptr;
	if (!D || Mode != EOutlawsCoopMode::Host) return;
	TArray<AActor*> Off;
	for (const TSharedPtr<FNetworkObjectInfo>& Info : D->GetNetworkObjectList().GetAllObjects())
	{
		AActor* A = Info.IsValid() ? Info->Actor : nullptr;
		if (!A || A->GetRemoteRole() == ROLE_None) continue;
		if (A->IsA<AOutlawsCoopSession>() || A->IsA<AOutlawsCoopLink>() || A->IsA<APlayerController>() || A->IsA<APlayerState>() || A->IsA<AGameStateBase>())
		{
			continue;
		}
		Off.Add(A);
	}
	for (AActor* A : Off)
	{
		// (the world settings too: the pause menu's time dilation stays on the machine that paused)
		A->SetReplicates(false);
		if (!LocalOnlyLogged.Contains(A->GetClass()->GetFName()))
		{
			LocalOnlyLogged.Add(A->GetClass()->GetFName());
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP kept on this machine only: %s"), *A->GetClass()->GetName());
		}
	}
}

APawn* UOutlawsCoop::SpawnLocalPlayer(APlayerController* PC, const FVector& At, const FRotator& Rot)
{
	UWorld* W = PC ? PC->GetWorld() : nullptr;
	if (!W) return nullptr;
	FActorSpawnParameters SP;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
	AOutlawsPlayer* P = W->SpawnActor<AOutlawsPlayer>(AOutlawsPlayer::StaticClass(), At, Rot, SP);
	if (!P) return nullptr;
	PC->Possess(P);
	// (possession in a network game marks the pawn replicated: this one is this machine's alone, the other sees a puppet)
	P->SetReplicates(false);
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLRT Outlaws player spawned (co-op %s, possessed %d)"), ModeName(Get(PC) ? Get(PC)->Mode : EOutlawsCoopMode::None), PC->GetPawn() == P ? 1 : 0);
	return P;
}

void UOutlawsCoop::OnPlayerLogin(APlayerController* PC)
{
	UWorld* W = PC ? PC->GetWorld() : nullptr;
	if (!W) return;
	// N1: the host is 0; each client the lowest number no other player has (with two players: 1, as ever)
	int32 Index = 0;
	if (!PC->IsLocalController())
	{
		BotsMakeRoom();                  // (REQ-MPBOTS: a bot leaves when a human needs its slot)
		TArray<AOutlawsCoopLink*> Links;
		GetLinks(Links);
		Index = 1;
		while (Index < MaxPlayers - 1 && Links.ContainsByPredicate([Index](const AOutlawsCoopLink* K) { return K->PlayerIndex == Index; })) ++Index;
	}
	FActorSpawnParameters SP;
	SP.Owner = PC;
	SP.ObjectFlags |= RF_Transient;
	if (AOutlawsCoopLink* L = W->SpawnActor<AOutlawsCoopLink>(AOutlawsCoopLink::StaticClass(), FTransform::Identity, SP))
	{
		L->PlayerIndex = Index;
		if (Index >= 1)
		{
			// (REQ-COOPHORSES) the lowest horse slot no other player has: a later joiner takes a freed one
			TArray<AOutlawsCoopLink*> Others;
			GetLinks(Others);
			int32 Slot = 1;
			while (Slot < MaxCoopPlayers - 1 && Others.ContainsByPredicate([Slot, L](const AOutlawsCoopLink* K) { return K != L && K->HorseSlot == Slot; })) ++Slot;
			L->HorseSlot = Slot;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP horses: player %d takes horse slot %d"), Index, Slot);
		}
		L->ForceNetUpdate();
	}
	if (Index >= 1)
	{
		AOutlawsCoopSession* S = GetSession();
		RefreshLobby();                  // (N1: connected, not all ready now, one more in)
		bLoggedConnected = true;
		// R7: a level is already under way (the host started alone, or this player is back after a drop): this player joins it
		// from a snapshot of the host's game once it says hello, not from the lobby's start
		const bool bLate = S && S->StartSerial > 0;
		if (bLate)
		{
			for (TActorIterator<AOutlawsCoopLink> It(W); It; ++It)
			{
				if (It->GetOwner() == PC)
				{
					It->bLateJoin = true;
					// (REQ-MPLOBBY: a late joiner lands in the lobby; its Enter Game asks for the host's game)
					It->bLobbyReady = false;
				}
			}
			RefreshLobby();
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby: player %d joins a game under way: in the lobby until it enters"), Index);
		}
		// (a host alone in its menu, the lobby's too, has the world stopped: it runs again now, as the menu does in a session.
		// N1, found with three players: a stopped clock stalls replication to every player after the first, whose world then
		// never begins play: the game state is sent once and its next update never comes)
		if (AOutlawsPlayer* P = LocalPlayer(); P && P->IsMenuOpen() && UGameplayStatics::GetGlobalTimeDilation(P) < 1.f)
		{
			UGameplayStatics::SetGlobalTimeDilation(P, 1.f);
			P->CustomTimeDilation = 1.f;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the host's menu is open: the world runs again now that a player has joined"));
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby: client connected%s"), bLate ? *FString::Printf(TEXT(" (joining late: the host is in %s, serial %d)"), *S->Level, S->StartSerial) : TEXT(""));
		if (Index > 1 || (S && S->Connected > 2)) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby: player %d joined (%d of %d players)"), Index, S ? S->Connected : 0, MaxPlayersSetting());
		++Version;
	}
}

void UOutlawsCoop::OnPlayerLogout(AController* PC)
{
	APlayerController* P = Cast<APlayerController>(PC);
	if (!P || P->IsLocalController()) return;
	int32 Who = -1;
	FString Name;
	FOutlawsCoopStats Gone;
	for (TActorIterator<AOutlawsCoopLink> It(P->GetWorld()); It; ++It)
	{
		if (It->GetOwner() != P) continue;
		Who = It->PlayerIndex;
		Name = It->PlayerName;
		Gone = It->Stats;
		It->Destroy();
	}
	AOutlawsCoopSession* S = GetSession();
	if (Who > 0 && Who < MaxPlayers)
	{
		// (BUG-COOPTIMEOUT) a paused player whose place was held: said, and the hold ends with it
		++PauseHoldLogouts;
		if (PauseHolds[Who].Since > 0.0) EndPauseHold(Who, PauseHolds[Who].bOver ? TEXT("its hold ran out and it timed out") : TEXT("its connection ended during the hold"));
	}
	RefreshLobby();                      // (N1: the others stay; with two players nobody is left)
	if (Mode != EOutlawsCoopMode::Host || bExitRequested) return;
	if (AOutlawsLevelActor* L = bLevelBuilt ? FindLevel() : nullptr; L && Gone.Seq > 0 && Gone.Keys.Num() > 0 && !Gone.bDead)
	{
		// (BUG-STUCKGAP, user 2026-10-05: keys are never lost in co-op) a player who leaves holding keys: they go back on the
		// ground at its last safe floor spot (else this player's own), for those who stay
		FVector Feet = Gone.SafeFeet;
		const AOutlawsPlayer* Me = LocalPlayer();
		if (!Gone.bSafe && !(Me && Me->LastSafeFeet(Feet))) Feet = L->GetPlayerStartLocation() - FVector(0.0, 0.0, 88.0);
		ReturnKeys(L, Gone.Keys, Feet, Who, TEXT("left the game"));
#if WITH_OUTLAWS_TESTS
		if (bAuto && TestOnly == TEXT("unstuckleave"))
		{
			FVector OnFloor;
			const bool bStand = Me && Me->SafeFeetAt(Feet, OnFloor);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: unstuckleave: the client left with %d keys; its last safe spot %s was %s; a body can stand there: %s"), Gone.Keys.Num(),
				*Feet.ToCompactString(), Gone.bSafe ? TEXT("known") : TEXT("NOT KNOWN"), bStand ? TEXT("yes") : TEXT("NO"));
			Summary(TEXT("done"));
			QuitForTest();
			return;
		}
#endif
	}
	const bool bOthersStay = S && S->bClientConnected;
	if (bOthersStay && (S->StartSerial > 0 || !bAuto))
	{
		// N1: one of several left: the session goes on for the rest (in a level as R7's lone host; in the lobby as it was)
		JoinAsked.Remove(Who);
		if (S->StartSerial > 0) ForgetClient(Who);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP player %d (%s) left: %d players go on"), Who, Name.IsEmpty() ? WhoWord(Who) : *Name, S->Connected);
		if (AOutlawsLevelActor* L = FindLevel(); L && S->StartSerial > 0) L->Notify(FString::Printf(TEXT("%s left the game"), Name.IsEmpty() ? TEXT("A player") : *Name), 3.f);
		++Version;
		return;
	}
	if (S && S->StartSerial > 0 && (!bAuto || IsDropoutTest() || IsPauseTest()))
	{
		// R7: a level is under way: the host plays on alone and the session stays open for the player to come back (or
		// another to join); the other player's things in this machine's hands let go
		JoinAsked.Reset();
		ForgetClient(Who);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the other player left: the host plays on alone (friends can join again)"));
		if (AOutlawsLevelActor* L = FindLevel()) L->Notify(TEXT("The other player left. Friends can join again any time."), 3.f);
		++Version;
		return;
	}
	if (bOthersStay) return;             // (a test's client gone while others play on: the host's own test ends it)
#if WITH_OUTLAWS_TESTS
	if (bAuto)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP session ended: the other player left"));
		Summary(TEXT("the other player left"));
		QuitForTest();
		return;
	}
#endif
	// (from inside the game mode's logout: the travel home happens on the next tick)
	EndSession(TEXT("the other player left"), true);
}

void UOutlawsCoop::SetReadyFor(int32 PlayerIndex, bool bReady)
{
	AOutlawsCoopSession* S = GetSession();
	if (!S || Mode != EOutlawsCoopMode::Host) return;
	if (PlayerIndex == 0)
	{
		S->bHostReady = bReady;
		S->ForceNetUpdate();
	}
	if (AOutlawsCoopLink* K = GetLink(PlayerIndex))
	{
		K->bLobbyReady = bReady;              // (N1: each player's own, for the lobby's list; the session's bClientReady is all of them)
		K->ForceNetUpdate();
	}
	RefreshLobby();
	++Version;
}

void UOutlawsCoop::SetReady(bool bReady)
{
	if (AOutlawsCoopLink* L = GetLink(MyIndex()))
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby: this player (%s) asks for Ready %d"), WhoWord(MyIndex()), bReady ? 1 : 0);
		L->ServerSetReady(bReady);       // (the host's own: runs here at once)
	}
	else UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby: Ready %d not sent: this player's link has not come yet"), bReady ? 1 : 0);
}

bool UOutlawsCoop::MyReady() const
{
	// what the host counts for this player: the host's own flag, or (a client) its own link's, which only the host sets.
	// (BUG-READYFLIP: no fallback to the session's client ready, every client's together, before the link has come; with
	// three or more players that is someone else's state)
	const AOutlawsCoopSession* S = GetSession();
	if (Mode == EOutlawsCoopMode::Client)
	{
		const AOutlawsCoopLink* Mine = GetLink(MyIndex());
		return Mine && Mine->bLobbyReady;
	}
	return S && S->bHostReady;
}

uint32 UOutlawsCoop::LobbyReadyBits(const AOutlawsCoopSession* S) const
{
	// every Ready a lobby page shows (its row, its status and slot lines, its Start row), one bit each
	uint32 Bits = (MyReady() ? 1u : 0u) | (S && S->bHostReady ? 2u : 0u) | (S && S->bClientReady ? 4u : 0u) | (S && S->bClientConnected ? 8u : 0u);
	TArray<AOutlawsCoopLink*> Links;
	GetLinks(Links);
	for (const AOutlawsCoopLink* K : Links)
	{
		if (K->bLobbyReady) Bits |= 1u << (4 + K->PlayerIndex);
	}
	return Bits;
}

void UOutlawsCoop::StartGame(bool bAlone)
{
	AOutlawsCoopSession* S = GetSession();
	AOutlawsLevelActor* L = FindLevel();
	if (!S || !L || Mode != EOutlawsCoopMode::Host || S->StartSerial > 0) return;
	if (!bAlone && (!S->bClientConnected || !S->bHostReady || !S->bClientReady)) return;
	if (bAlone) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby: Start now (the host goes in%s; friends can join any time)"), S->bClientConnected ? TEXT(" with the other player") : TEXT(" alone"));
	{
		// REQ-MPLOBBY: a player who has not pressed Enter Game stays in the lobby; its Enter joins the host's level then, as a
		// late joiner does (the host's snapshot)
		TArray<AOutlawsCoopLink*> Links;
		GetLinks(Links);
		for (AOutlawsCoopLink* K : Links)
		{
			if (K->PlayerIndex <= 0 || K->bBot || K->bLobbyReady || K->bLateJoin) continue;
			K->bLateJoin = true;
			K->ForceNetUpdate();
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby: the %s is still in the lobby at the start: it joins when it enters"), WhoWord(K->PlayerIndex));
		}
	}
	if (LobbyLoadSlot != -100 && RequestLoad(LobbyLoadSlot)) return;       // (milestone 8: a save for both: the lobby's Continue, or -OLCoopLoad)
	S->Level = LevelToStart;
	S->Difficulty = int32(L->Difficulty);
	S->StartKind = uint8(EOutlawsCoopStart::Lobby);
	// (REQ-HMCAREER) Marshal Training's office starts a Historical Missions session for every player (REQ-COOPHM: the
	// posters' rides go on from it); any other lobby level none
	S->bHom = LevelToStart.Equals(TEXT("office"), ESearchCase::IgnoreCase);
	S->HomMission.Reset();
	S->HomBack.Reset();
	L->BeginRandoRun(TEXT("co-op lobby start"));     // (REQ-COOPRANDO: the host's page makes the session's run)
	PrepareExtras(S, S->Level, S->Difficulty);   // (2026-10-02: the extra bad guys, with the start)
	++S->StartSerial;
	S->ForceNetUpdate();
}

void UOutlawsCoop::TickLoadTimeout(double Now, AOutlawsCoopSession* S)
{
	UWorld* W = GetWorld();
	UNetDriver* D = W ? W->GetNetDriver() : nullptr;
	if (!D) return;
	if (ShortTimeout < 0.f) ShortTimeout = D->ConnectionTimeout;
	bool bLoading = false;
	if (S && S->StartSerial > 0)
	{
		bLoading = StartedSerial < S->StartSerial || !bLevelBuilt;
		for (TActorIterator<AOutlawsCoopLink> It(W); It && !bLoading; ++It) bLoading = It->BuiltSerial < S->StartSerial;
	}
	const double Wall = FPlatformTime::Seconds();
	if (bLoading) LoadingUntil = Wall + 3.0;
	const float Want = Wall < LoadingUntil ? 60.f : ShortTimeout;
	if (D->ConnectionTimeout == Want) return;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP timeout %.0f s (%s)"), Want, Want > ShortTimeout ? TEXT("a level is being built") : TEXT("every machine has built the level"));
	D->ConnectionTimeout = D->InitialConnectTimeout = Want;
}

void UOutlawsCoop::BeginLevel(const FString& Level, int32 Difficulty, int32 Serial, uint8 Kind)
{
	AOutlawsLevelActor* L = FindLevel();
	static const TCHAR* Kinds[] = { TEXT("from the lobby"), TEXT("the story goes on"), TEXT("the level again"), TEXT("a level skip"), TEXT("from a save") };
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP start: level %s, difficulty %d, serial %d (%s) (%s)"), *Level, Difficulty, Serial, ModeName(Mode), Kinds[FMath::Min<int32>(Kind, 4)]);
	if (!L || Level.IsEmpty()) return;
	if (AOutlawsPlayer* P = LocalPlayer()) P->CoopCloseMenu();
	// milestone 7: both dead (said here too on the client), the deaths forgotten
	if (Kind == uint8(EOutlawsCoopStart::Restart) && IsClient() && L && L->bDead && bOtherDead)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP both dead: the level restarts"));
	}
	if (UNetDriver* D = GetWorld() ? GetWorld()->GetNetDriver() : nullptr)
	{
		// (this machine's own build: the long timeout before it blocks, whatever the tick saw)
		if (ShortTimeout < 0.f) ShortTimeout = D->ConnectionTimeout;
		LoadingUntil = FMath::Max(LoadingUntil, FPlatformTime::Seconds() + 3.0);
		D->ConnectionTimeout = D->InitialConnectTimeout = 60.f;
	}
	float SlowBuild = 0.f;
	if (IsHost() && OLFlags::Value(TEXT("OLCoopSlowBuild"), SlowBuild) && SlowBuild > 0.f)
	{
		// (test: a slow host's level build, the game thread held)
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host's build held %.0f s"), SlowBuild);
		FPlatformProcess::Sleep(SlowBuild);
	}
	HandExtrasToLevel(L, Serial);                // (2026-10-02: the host's extra bad guys for this level, before it is built)
	BothDeadSince = 0.0;
	bBothDeadRestart = false;
	bLocalDead = false;
	bLevelBuilt = false;
	BuiltLevel = Level;
	bWaitingForStart = false;
	DestroyPuppets();
	ResetWorld();                    // (milestone 3: this level's pickups are numbered again once it is built)
	TWeakObjectPtr<UOutlawsCoop> WeakThis(this);
	const FString Name = Level.ToLower();
	auto Then = [WeakThis, Name]()
	{
		if (UOutlawsCoop* C = WeakThis.Get()) C->OnLevelBuilt(Name);
	};
	const EOutlawsDifficulty D = static_cast<EOutlawsDifficulty>(FMath::Clamp(Difficulty, 0, 2));
	switch (static_cast<EOutlawsCoopStart>(Kind))
	{
	case EOutlawsCoopStart::NextLevel:
	{
		// milestone 5: as StartNextLevel after the movie, each machine's player keeps what they carry
		L->Difficulty = D;
		L->NextLevelName = Name;
		TGuardValue<bool> Own(bStartingLevel, true);
		L->StartNextLevelThen(Then);
		break;
	}
	case EOutlawsCoopStart::Jump:
	{
		// the cheat's level skip (BUG-SKIPLEVELINV): as the story goes on, each machine's player keeps what they carry
		if (L->Movie)
		{
			L->Movie->Destroy();
			L->Movie = nullptr;
		}
		L->LevelEndTimer = -1.f;
		L->Difficulty = D;
		UOutlawsStats::NoteJump(L);               // (REQ-LEVELTIMER: no best time for the level skipped to)
		L->NextLevelName = Name;
		TGuardValue<bool> Own(bStartingLevel, true);
		L->StartNextLevelThen(Then);
		break;
	}
	case EOutlawsCoopStart::Restart:
		// milestone 5: Restart Level (each with what they entered the level with)
		if (L->Movie)
		{
			L->Movie->Destroy();
			L->Movie = nullptr;
		}
		L->LevelEndTimer = -1.f;
		L->Difficulty = D;
		L->LevelName = Name;
		L->RestartLevelThen(Then);
		break;
	case EOutlawsCoopStart::Load:
		// milestone 8: a save for both (OutlawsCoopSaves.cpp)
		BeginLoad(L, Name, Difficulty, Then);
		break;
	default:
		L->StartMission(Name, D, Then);
		break;
	}
	if (const AOutlawsCoopSession* HS = GetSession(); HS && Kind != uint8(EOutlawsCoopStart::Load))
	{
		// (REQ-COOPHM) the host's Historical Missions session with the start (StartMission clears it; the level is built after)
		L->bHom = HS->bHom;
		if (HS->bHom && Kind == uint8(EOutlawsCoopStart::Lobby) && Name == TEXT("office"))
		{
			// (REQ-HMCAREER) Marshal Training from the lobby: a fresh session (no score, no stamps)
			L->HomGameScore = L->HomTotalGold = L->HomTotalKills = L->HomTotalWounds = L->HomTotalBounty = 0;
			L->HomPorchEvents.Reset();
			L->HomResultText.Reset();
		}
		if (HS->bHom)
		{
			L->HomMission = HS->HomMission;
			L->HomReturnSector = HS->HomBack;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP hom: %s starts as a Historical Mission (%s, back to %s) (%s)"), *Name, *L->HomMission,
				L->HomReturnSector.IsEmpty() ? TEXT("none") : *L->HomReturnSector, IsHost() ? TEXT("host") : TEXT("client"));
		}
	}
}

void UOutlawsCoop::OnLevelBuilt(const FString& Level)
{
	if (Mode == EOutlawsCoopMode::None) return;
	AOutlawsLevelActor* L = FindLevel();
	AOutlawsPlayer* P = LocalPlayer();
	if (Mode == EOutlawsCoopMode::Client && L && P)
	{
		// the second player stands beside the first, never inside: a free spot a step to the side (or behind, or ahead); N1:
		// each client starts its search at its own spot of the twelve (player 1 at the first, as ever), so nobody overlaps
		UWorld* W = L->GetWorld();
		const FVector Start = L->GetPlayerStartLocation();
		const FRotator Facing(0.f, L->GetPlayerStartYaw(), 0.f);
		const float Half = P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), Radius = P->GetCapsuleComponent()->GetScaledCapsuleRadius();
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsCoopStart), false, P);
		const FVector Ways[] = { FRotationMatrix(Facing).GetUnitAxis(EAxis::Y), -FRotationMatrix(Facing).GetUnitAxis(EAxis::Y),
			-FRotationMatrix(Facing).GetUnitAxis(EAxis::X), FRotationMatrix(Facing).GetUnitAxis(EAxis::X) };
		const int32 First = FMath::Max(0, MyIndex() - 1);
		bool bPlaced = false;
#if WITH_OUTLAWS_TESTS
		FVector JpOl;
		int32 JpP = -1, JpQ = -1;
		if (TestOnly == TEXT("jumppick") && JumpPickSpot(JpOl, JpP, JpQ))
		{
			// (BUG-JUMPPICK's test: the client's start on a catwalk, pier or tunnel floor)
			const FVector At = FOutlawsGeometry::ToUE(JpOl.X, JpOl.Y, JpOl.Z) + FVector(0.0, 0.0, Half + 2.0);
			L->MovePlayerStart(At);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP client starts on the test's spot: %s (sector %d's floor)"), *At.ToString(), JpP);
			bPlaced = true;
		}
#endif
		for (int32 k = 0; k < 12 && !bPlaced; ++k)
		{
			const int32 Spot = (First + k) % 12;
			const float Dist = Spot < 4 ? 110.f : Spot < 8 ? 160.f : 220.f;
			const FVector At = Start + Ways[Spot % 4] * Dist;
			FHitResult Wall;
			if (W->LineTraceSingleByChannel(Wall, Start, At, ECC_Visibility, Q)) continue;                     // (not through a wall)
			if (W->OverlapBlockingTestByChannel(At, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(Radius, Half), Q)) continue;
			FHitResult Floor;
			if (!W->LineTraceSingleByChannel(Floor, At, At - FVector(0.0, 0.0, Half + 120.0), ECC_Visibility, Q)) continue;   // (ground under it)
			L->MovePlayerStart(At);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP client starts beside the host: %s (%.0f cm from the level's start)"), *At.ToString(), Dist);
			bPlaced = true;
		}
	}
	bLevelBuilt = true;
	LevelBuiltAt = FPlatformTime::Seconds();
	bAutosaveHoldLogged = false;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP level built: %s (%s)"), *Level, ModeName(Mode));
	++Version;
	// milestone 3: the level's pickups numbered the same way on both machines (its own order), the host told this
	// machine's level is up, and the other machine's news that came while it was being built put in now
	if (L) AssignPickupIds(L);
	if (L) SetupEnemies(L);                      // (milestone 4: the host's think for both players; the client's are puppets)
	if (L) SetupInf(L);                          // (milestone 5: the host's INF decides for both; the client's shows it)
	if (L) SetupVerbs(L);                        // (milestone 6: the props numbered; blasts, walls, glass, props, lamps told by index)
	if (L) WarmPuppetSprite(L);                  // (the other player's sprite and its HD look built now, not in play)
	if (L) CheckExtras(L);                       // (2026-10-02: the extra bad guys made here, each clear of the walls)
	if (AOutlawsCoopLink* Mine = GetLink(MyIndex())) Mine->ServerLevelBuilt(StartedSerial);
	RunDeferred();
}

void UOutlawsCoop::Tick(float DeltaTime)
{
	OL_PERF_SCOPE("Other ours");
	TickUpnp(FPlatformTime::Seconds());              // (REQ-UPNP: the worker's answers, the lease's renewal)
	UWorld* W = CurrentWorld();
	if (!W || !W->IsGameWorld() || bExitRequested) return;
	const double Now = FPlatformTime::Seconds();
#if WITH_OUTLAWS_TESTS
	if (IsTestRun() && Now - GStartTime > AutoGiveUpAfter)
	{
		Summary(TEXT("gave up: the test did not finish in time"));
		QuitForTest();
		return;
	}
#endif
	AOutlawsPlayer* P = LocalPlayer();
	TickReconnect(Now);                  // (R8: a dropped client tries the same address again)
	TickPauseHold(Now);                  // (BUG-COOPTIMEOUT: a paused player's place held; back, the connection checked)
#if WITH_OUTLAWS_TESTS
	if (bAuto && IsDropoutTest()) TickDropoutTest(Now);
	if (bAuto && IsPauseTest()) TickPauseTest(Now);
	if (CoopBenchSec > 0.f) TickCoopBench(Now);
	if (Mode == EOutlawsCoopMode::None && bLaunchPending && LaunchAt > 0.0 && Now >= LaunchAt && !MenuWalk.IsEmpty())
	{
		if (P && P->IsMenuOpen()) TickMenuWalk(P, Now);
		return;
	}
#endif
	if (Mode == EOutlawsCoopMode::None && bLaunchPending && LaunchAt > 0.0 && Now >= LaunchAt
		&& (JoinDelay > 0.f || (P && P->IsFrontMenuUp()) || Now - LaunchAt > 10.0))
	{
		bLaunchPending = false;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the launch's join goes now (%s)"), JoinDelay > 0.f ? *FString::Printf(TEXT("after %.0f s"), JoinDelay) : TEXT("the title menu is up"));
		Join(LaunchJoin, false);
		return;
	}
#if WITH_OUTLAWS_TESTS
	if (Mode == EOutlawsCoopMode::None && !LeaveTestWho.IsEmpty() && EndedAt > 0.0 && P && P->IsMenuOpen() && Now - EndedAt > 6.0)
	{
		// -OLCoopLeaveTest: the session over, this machine is back on its own title menu
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP leave test: back at the title menu (%s), mode none, network %s"), *P->CoopMenuPage(),
			W->GetNetMode() == NM_Standalone ? TEXT("standalone") : TEXT("STILL NETWORKED"));
		QuitForTest();
		return;
	}
	if (Mode == EOutlawsCoopMode::None && bMenuTest && P && P->IsMenuOpen())
	{
		// -OLCoopMenuTest: New Game, Multiplayer, Co-op, Join Game (a few keys typed), each shown and photographed, then quit
		if (MenuTestAt == 0.0) MenuTestAt = Now + 3.0;
		else if (Now >= MenuTestAt)
		{
			MenuTestAt = Now + 1.0;
			switch (MenuTestStep++)
			{
			// (REQ-MPMENU: New Game > Multiplayer > Co-op > Join Game)
			case 0: P->CoopShowPage(TEXT("#newgame"), true); break;
			case 1: Shot(TEXT("ol_coop_menu_newgame")); P->CoopShowPage(TEXT("#coopmulti"), true); break;
			case 2: P->CoopShowPage(TEXT("#coop"), true); break;
			case 3: Shot(TEXT("ol_coop_menu_coop")); break;
			case 4: P->CoopShowPage(TEXT("#coopjoin"), true); break;
			case 5: TypeKey(TEXT("del")); TypeKey(TEXT("7")); P->CoopShowPage(TEXT("#coopjoin"), false); break;
			case 6: Shot(TEXT("ol_coop_menu_join")); break;
			default:
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP menu test: shown #newgame, #coopmulti, #coop, #coopjoin (now on %s, address typed %s)"), *P->CoopMenuPage(), *JoinAddress);
				QuitForTest();
				return;
			}
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP menu test: step %d, page %s"), MenuTestStep, *P->CoopMenuPage());
		}
		return;
	}
	if (Mode == EOutlawsCoopMode::None && SoloLoadSlot != -100)
	{
		TickSoloLoadTest(Now);           // (-OLLoadTest: single player loads a save and reports; milestone 8's check)
		return;
	}
	if (Mode == EOutlawsCoopMode::None && !ExtrasSurveyDir.IsEmpty())
	{
		// (-OLExtrasSurvey=<folder>: the extra bad guys worked out for every story level, then quit; OutlawsCoopExtras.cpp)
		if (FindLevel())
		{
			RunExtrasSurvey();
			QuitForTest();
		}
		return;
	}
#endif
	if (Mode == EOutlawsCoopMode::None)
	{
		TickMicMeter(Now);               // (R8: the Multiplayer page's microphone meter, outside a session)
		// why the last session ended, said on the title menu it came back to
		if (!PendingToast.IsEmpty() && P && P->IsMenuOpen())
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the menu (%s) says why the session ended: %s"), *P->CoopMenuPage(), *PendingToast);
			P->CoopToast(PendingToast);
			PendingToast.Reset();
		}
		return;
	}
	const ENetMode NM = W->GetNetMode();
	const bool bInSession = (Mode == EOutlawsCoopMode::Host && NM == NM_ListenServer) || (Mode == EOutlawsCoopMode::Client && NM == NM_Client);
	if (!bInSession)
	{
		// joining from the menu: going back off the join page drops the connection on its way
		if (Mode == EOutlawsCoopMode::Client && bJoinFromMenu && P && P->IsMenuOpen() && P->CoopMenuPage() != TEXT("#coopjoin"))
		{
			EndSession(TEXT("the join was cancelled"), true);
		}
		return;
	}
	AOutlawsCoopSession* S = GetSession();
	if (!S) return;
	if (Mode == EOutlawsCoopMode::Client && !W->HasBegunPlay() && Now >= NextBegunPlayLogAt)
	{
		// (N1 diagnosis: a later client whose world never began play; what of the host's has come)
		NextBegunPlayLogAt = Now + 3.0;
		const AGameStateBase* GS = W->GetGameState();
		const APlayerController* PC = W->GetFirstPlayerController();
		int32 Links = 0;
		for (TActorIterator<AOutlawsCoopLink> It(W); It; ++It) ++Links;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP client world not begun play: game state %s (begun %d), controller %s (net connection %d), links %d, my index %d"),
			GS ? *GS->GetName() : TEXT("none"), GS && GS->HasMatchStarted() ? 1 : 0, PC ? *PC->GetName() : TEXT("none"), PC && PC->GetNetConnection() ? 1 : 0, Links, MyIndex());
	}
	if (S->bClientConnected && !bLoggedConnected)
	{
		bLoggedConnected = true;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby: client connected"));
		++Version;
	}
	const int32 Ready = (S->bHostReady ? 2 : 0) | (S->bClientReady ? 1 : 0);
	if (Ready != LoggedReady)
	{
		LoggedReady = Ready;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby: host ready %d, client ready %d"), S->bHostReady ? 1 : 0, S->bClientReady ? 1 : 0);
		++Version;
	}
	// BUG-READYFLIP (RC2, 2026-10-10): a lobby page is built once and built again only when Version moves; its Ready row
	// was built from this player's Ready, but Version moved only with the session's two words above.  With three or more
	// players a client's own Ready moves its link alone (the session's client ready is every client's, unmoved while
	// another is not ready), so the row kept its old word while the host counted the new one, and the next press (made
	// from the real value) undid it: the row read No while the host counted Yes.  Now every Ready a page shows moves Version.
	const uint32 ReadyBits = LobbyReadyBits(S);
	if (ReadyBits != LoggedReadyBits)
	{
		const uint32 Moved = LoggedReadyBits == MAX_uint32 ? 0u : ReadyBits ^ LoggedReadyBits;
		LoggedReadyBits = ReadyBits;
		for (int32 i = 0; i < MaxPlayers; ++i)
		{
			if (!(Moved & (1u << (4 + i)))) continue;
			int32 Clients = 0, ClientsReady = 0;
			TArray<AOutlawsCoopLink*> Links;
			GetLinks(Links);
			for (const AOutlawsCoopLink* K : Links)
			{
				if (K->PlayerIndex == 0 || K->bBot) continue;
				++Clients;
				if (K->bLobbyReady) ++ClientsReady;
			}
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby: the %s's Ready as the host counts it: %d (clients ready %d of %d)%s"), WhoWord(i),
				(ReadyBits & (1u << (4 + i))) ? 1 : 0, ClientsReady, Clients, i == MyIndex() ? TEXT(", this player's row built again") : TEXT(""));
		}
		++Version;
	}
	TickLoadTimeout(Now, S);
	TickNextLevelHold(Now);          // (N1: the next level waits for every player's level end, 20 s at most)
	TickHorses(Now);                 // (REQ-COOPHORSES: the other players' horses beside James's, OutlawsCoopHorses.cpp)
	// (2026-10-02: the client starts a level once the host's extra bad guys for it are here too: they come with the start
	// serial, set in the same frame; a start that came without them goes on after 5 s with none, said in the log)
	// (M1, found with three players) a client's level actor begins play only once the host's game state has come; a level
	// started on it before then is built over by its own front end at its BeginPlay: a start waits for it
	const AOutlawsLevelActor* LvReady = FindLevel();
	const bool bLevelReady = IsHost() || (LvReady && LvReady->HasActorBegunPlay() && W->HasBegunPlay());
	if (!bLevelReady)
	{
		// (nothing: the next tick looks again)
	}
	else if (TickClientJoin(Now, S))
	{
		// (R7: this player joins a level already under way, from the host's snapshot: OutlawsCoopJoin.cpp)
	}
	else if (S->StartSerial > StartedSerial && IsClient() && StartedSerial == 0 && !(bEnterPressed && MyReady()))
	{
		// (REQ-MPLOBBY: a joiner still in the lobby never follows the host's start: its Enter Game joins it)
	}
	else if (S->StartSerial > StartedSerial && IsHost() && HostStartSeenAt == 0.0)
	{
		// (user 2026-10-04) the host's own build blocks its network: the clients hear the new serial (and go to the long
		// timeout) first
		HostStartSeenAt = Now;
	}
	else if (S->StartSerial > StartedSerial && ((IsHost() && Now - HostStartSeenAt >= 0.4) || (!IsHost() && (S->ExtrasSerial == S->StartSerial || (ExtrasWaitSince > 0.0 && Now - ExtrasWaitSince > 5.0)))))
	{
		HostStartSeenAt = 0.0;
		ExtrasWaitSince = 0.0;
		StartedSerial = S->StartSerial;
		BeginLevel(S->Level, S->Difficulty, S->StartSerial, S->StartKind);
	}
	else if (S->StartSerial > StartedSerial && ExtrasWaitSince == 0.0)
	{
		ExtrasWaitSince = Now;
	}
	TickLobby(Now, S, P);            // (REQ-MPLOBBY: the host's lobby over its first level, the mic flag: OutlawsCoopLobby.cpp)
	TickPace();                      // (2026-10-02 decision 6: both players' paces, the lobby on; OutlawsCoopRules.cpp)
	TickName();                      // (2026-10-02: the Multiplayer page's player name, the lobby on; OutlawsCoopNameplate.cpp)
	TickBuildLine();                 // (R8: both builds on the pause menu's Co-op page)
	TickMatch(Now);                  // (M1: a Death Match over the session; nothing in story co-op: OutlawsCoopMatch.cpp)
	if (IsClient() && bLevelBuilt && Now - LevelBuiltAt < 6.0 && !bNextLobbyArmed)       // (REQ-MPLOBBY b: not the next match's lobby)
	{
		// (found in M1's runs) a joiner coming from its title screen: that screen's own setup can finish a frame after the joined
		// level is built and put the main menu up over it; the level is what it joined, so the title's menu goes
		AOutlawsLevelActor* Lv = FindLevel();
		AOutlawsPlayer* Me = LocalPlayer();
		if (Lv && Me && Me->IsFrontMenuUp() && !Lv->bMovieToMenu && !Lv->LevelName.IsEmpty() && Lv->LevelName.Equals(BuiltLevel, ESearchCase::IgnoreCase))
		{
			Me->CloseGameMenu();
			Lv->bFrontEnd = false;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the title's main menu came up over the joined level (%s): closed"), *BuiltLevel);
		}
	}
	if (Mode == EOutlawsCoopMode::Host && S->StartSerial == 0 && !FMath::IsNearlyEqual(S->Multiplier, BadGuysMultiplier()))
	{
		S->Multiplier = BadGuysMultiplier();         // (R14: the client's lobby shows the host's choice)
		S->ForceNetUpdate();
	}
	if (Mode == EOutlawsCoopMode::Host && S->StartSerial == 0)
	{
		// decision 5: the lobby's Resume Game, picked before both were ready, starts the autosave for both once they are
		if (bTestResume && !bResumeChosen && P && P->IsMenuOpen() && Now - FirstLobbyAt > 1.0 && FirstLobbyAt > 0.0)
		{
			// (-OLCoopLoad=resume: the row picked as a hand would pick it, the rows the host saw logged)
			OutlawsMenu::FPage Page;
			BuildMenuPage(P, TEXT("#coophost"), Page);
			TArray<FString> Rows;
			for (const OutlawsMenu::FItem& I : Page.Items) Rows.Add(I.Label);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host's lobby rows: %s"), *FString::Join(Rows, TEXT(" | ")));
			MenuAction(P, TEXT("coop:resume"));
			bTestResume = false;
		}
		if (FirstLobbyAt == 0.0 && P && P->IsMenuOpen()) FirstLobbyAt = Now;
#if WITH_OUTLAWS_TESTS
		TickLobbyShots(Now, P);          // (-OLCoopLobbyShot: REQ-COOPLOBBY's pictures)
#endif
	}
	// BUG-READYFLIP's check: nobody readies by themselves or starts; each press made and looked at (OutlawsCoopReadyFlip.cpp)
#if WITH_OUTLAWS_TESTS
	if (bAuto && TestOnly == TEXT("readyflip") && (StartedSerial == 0 || Mode == EOutlawsCoopMode::Host))
	{
		if (Mode == EOutlawsCoopMode::Host) TickReadyFlipHost(Now, P);
		else TickReadyFlipClient(Now, P);
	}
	else
#endif
	// -OLCoopAuto / -OLCoopReady: ready once the lobby is up, the host starts once both are
	if (bAutoReady && StartedSerial == 0)
	{
		// (BUG-COOPTIMEOUT: a late joiner has no lobby Ready to press; the host counts it ready as it comes, so a test's
		// press here would hide that)
		// (REQ-MPLOBBY: Enter Game, a late joiner's too: its Enter asks for the host's game)
		if (!bAutoReadySent && LobbyEnterAfter <= 0.f && P && P->IsMenuOpen() && GetLink(MyIndex()) && (IsHost() || GetLink(MyIndex())->PlayerIndex > 0))
		{
			bAutoReadySent = true;
			bEnterPressed = true;
			SetReady(true);
		}
		// (R7's test: the host goes in alone two seconds after its lobby is up)
		if (Mode == EOutlawsCoopMode::Host && (TestOnly.StartsWith(TEXT("latejoin")) || TestOnly == TEXT("jumppicklate")) && S->StartSerial == 0 && FirstLobbyAt > 0.0 && Now - FirstLobbyAt > 2.0)
		{
			StartGame(true);
		}
		// (both ready: the lobby photographed a moment later, the host starts two seconds after)
		// (N1's party test: everyone it waits for in the lobby first)
		const bool bBoth = S->bClientConnected && S->bHostReady && S->bClientReady && (PartyPlayers <= 0 || S->Connected >= PartyPlayers || IsClient());
		if (bBoth && BothReadyAt == 0.0) BothReadyAt = Now;
		if (bBoth && !bLobbyShot && Now - BothReadyAt > 0.8)
		{
			bLobbyShot = true;
			Shot(Mode == EOutlawsCoopMode::Host ? TEXT("ol_coop_lobby_host") : TEXT("ol_coop_lobby_client"));
		}
		// (-OLCoopTest=voice: eight seconds in the lobby, for its voice to be heard there first)
		if (Mode == EOutlawsCoopMode::Host && bBoth && S->StartSerial == 0 && Now - BothReadyAt > (TestOnly == TEXT("voice") ? 8.0 : 2.0))
		{
			StartGame();
		}
	}
	// -OLCoopLeaveTest: one side leaves five seconds into the level
	if (!LeaveTestWho.IsEmpty() && bLevelBuilt && Now - LevelBuiltAt > 5.0 && (LeaveTestWho == TEXT("host")) == (Mode == EOutlawsCoopMode::Host))
	{
		EndSession(TEXT("left the game (leave test)"), true);
		return;
	}
	TickRemote(Now);
	TickVoice(Now);                  // (voice chat, the lobby on; user 2026-10-02; OutlawsCoopVoice.cpp)
	TickWorld(Now);
	TickJoinReplay(Now);             // (Q2 step 4: a late join's replay spread over frames, OutlawsCoopJoin.cpp)
	TickEnemies(Now);
	TickGrabs(Now);                  // (milestone 6 item 4: the host's copies held for the client's hand; OutlawsCoopGrabs.cpp)
	TickInf(Now);
	TickRules(Now);
	TickSaves(Now);
	TickJoin(Now);                   // (R7: a late player's snapshot, once this host's level is steady; OutlawsCoopJoin.cpp)
#if WITH_OUTLAWS_TESTS
	TickTest(Now);
	if (bAuto && !bWorldTest && bLevelBuilt && !bSummaryDone && Now - LevelBuiltAt > (Mode == EOutlawsCoopMode::Client ? AutoClientQuitAfter : AutoHostQuitAfter))
	{
		Summary(TEXT("done"));
		QuitForTest();
	}
	if (bWorldTest && bLevelBuilt && !bSummaryDone && Now - LevelBuiltAt > AutoWorldTestGiveUp)
	{
		Summary(TEXT("the world test did not finish in time"));
		QuitForTest();
	}
#endif
}

void UOutlawsCoop::Summary(const FString& Why)
{
	if (bSummaryDone) return;
	bSummaryDone = true;
	const AOutlawsCoopSession* S = GetSession();
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP summary (%s): %s, level %s built %d, other player connected %d, host ready %d, client ready %d, start serial %d, poses received %d, sees the other player %s"),
		*Why, ModeName(Mode), BuiltLevel.IsEmpty() ? TEXT("-") : *BuiltLevel, bLevelBuilt ? 1 : 0, S && S->bClientConnected ? 1 : 0, S && S->bHostReady ? 1 : 0, S && S->bClientReady ? 1 : 0,
		S ? S->StartSerial : 0, PosesReceived, SeenDistance >= 0.f ? *FString::Printf(TEXT("at %.0f cm"), SeenDistance) : TEXT("no"));
	{
		// N1: who was here, and whom this machine drew
		TArray<FString> Drawn;
		for (int32 i = 0; i < MaxPlayers; ++i) if (Puppets[i].IsValid()) Drawn.Add(FString::FromInt(i));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP summary of the players (%s): this machine is player %d, %d in the session (most %d), puppets drawn here [%s]"), ModeName(Mode), MyIndex(),
			S ? S->Connected : 0, S ? S->MaxPlayers : 0, *FString::Join(Drawn, TEXT(",")));
	}
	if (bLevelBuilt) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP summary of the enemies (%s): %s"), ModeName(Mode), *EnemySummary());
	if (bLevelBuilt) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP summary of the INF (%s): %s"), ModeName(Mode), *InfSummary());
	if (bLevelBuilt) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP summary of the rules (%s): %s"), ModeName(Mode), *RulesSummary());
	if (bLevelBuilt) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP summary of the verbs (%s): %s"), ModeName(Mode), *VerbsSummary());
	if (bLevelBuilt) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP summary of the saves (%s): %s"), ModeName(Mode), *SavesSummary());
	if (bLevelBuilt) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP summary of the grabs (%s): %s"), ModeName(Mode), *GrabsSummary());
	if (bLevelBuilt) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP summary of the extra bad guys (%s): %s"), ModeName(Mode), *ExtrasSummary());
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP summary of the voice (%s): %s"), ModeName(Mode), *VoiceSummary());
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP summary of the joins (%s): %s"), ModeName(Mode), *JoinSummary());
}

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TickLobbyShots(double Now, AOutlawsPlayer* P)
{
	// -OLCoopLobbyShot=<prefix> (REQ-COOPLOBBY's proof): a host alone photographs its lobby pages into Saved/OLShots in turn
	// (as it opens, Historical Missions and its level list, a mission picked, the Main Game list, the Options page, then
	// Show internet address pressed), each page's rows and lines logged, then quits
	if (LobbyShotPrefix.IsEmpty() || !P || !P->IsMenuOpen() || FirstLobbyAt <= 0.0 || Now - FirstLobbyAt < 3.0 || Now < LobbyShotAt) return;
	LobbyShotAt = Now + 1.5;
	auto Snap = [this, P](const TCHAR* What)
	{
		OutlawsMenu::FPage Page;
		BuildMenuPage(P, P->CoopMenuPage(), Page);
		TArray<FString> Rows;
		for (const OutlawsMenu::FItem& I : Page.Items) Rows.Add(I.Label);
		TArray<FString> Lines;
		StatusLines(P, P->CoopMenuPage(), Lines);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby shot %s: page %s, rows: %s; lines: %s"), What, *P->CoopMenuPage(), *FString::Join(Rows, TEXT(" | ")), *FString::Join(Lines, TEXT(" | ")));
		FString Fit;
		const bool bFits = PageFits(P, Fit);       // (REQ-MPLOBBY c)
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLFIT check %s: the co-op host's %s page fits the panel (%s)"), bFits ? TEXT("PASS") : TEXT("FAIL"), What, *Fit);
		Shot(LobbyShotPrefix + TEXT("_") + What);
	};
	switch (LobbyShotStep++)
	{
	case 0: Snap(TEXT("main")); break;
	case 1: MenuAction(P, TEXT("coop:campaign")); break;
	case 2: Snap(TEXT("historical")); break;
	case 3: MenuAction(P, TEXT("coop:newgame")); break;
	case 4: Snap(TEXT("levels_historical")); break;
	case 5: MenuAction(P, TEXT("coop:pick:icecaves")); break;      // (REQ-HMCAREER: a Handful mission; the five only through Marshal Training)
	case 6: Snap(TEXT("historical_picked")); break;
	case 7: MenuAction(P, TEXT("coop:campaign")); break;
	case 8: MenuAction(P, TEXT("coop:newgame")); break;
	case 9: Snap(TEXT("levels_main")); break;
	case 10: MenuAction(P, TEXT("coop:pick:hideout")); break;
	case 11: P->CoopShowPage(TEXT("#coophostopts"), true); break;
	case 12: Snap(TEXT("options")); break;
	case 13: MenuAction(P, TEXT("coop:back")); break;
	case 14: Snap(TEXT("back")); break;
	case 15:
		// (the lobby's Show internet address, pressed; -OLLobbyShotNoNet leaves it, so the run asks nothing of ipify)
		if (!bShowInternet && !OLFlags::Param(TEXT("OLLobbyShotNoNet"))) MenuAction(P, TEXT("coop:netaddr"));
		break;
	case 16: case 17: case 18: case 19: break;                                    // (the lookup's 5 s)
	case 20: Snap(TEXT("netaddr")); break;
	default:
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby shot: done (level %s, load slot %d)"), *LevelToStart, LobbyLoadSlot);
		QuitForTest();
		break;
	}
}
#endif // WITH_OUTLAWS_TESTS

void UOutlawsCoop::Shot(const FString& Name)
{
	const FString Path = FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png");
	FScreenshotRequest::RequestScreenshot(Path, false, false);
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP screenshot %s"), *Path);
}

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::QuitForTest()
{
	if (bExitRequested) return;
	bExitRequested = true;
	if (AOutlawsLevelActor* LU = FindLevel()) OutlawsUnlocks::EarnPending(LU, TEXT("the co-op session's end"));     // (REQ-UNLOCKS: a test's session ends here)
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP auto test: quitting"));
	FGenericPlatformMisc::RequestExit(false);
}
#endif // WITH_OUTLAWS_TESTS

void UOutlawsCoop::HandleNetworkFailure(UWorld* World, UNetDriver* NetDriver, ENetworkFailure::Type FailureType, const FString& ErrorString)
{
	if (Mode == EOutlawsCoopMode::None || bExitRequested) return;
	// (on the host, the other player's connection dropping is its logout, handled there)
	if (Mode == EOutlawsCoopMode::Host && NetDriver && NetDriver->GetNetMode() == NM_ListenServer
		&& (FailureType == ENetworkFailure::ConnectionLost || FailureType == ENetworkFailure::ConnectionTimeout))
	{
		return;
	}
	const bool bDropped = FailureType == ENetworkFailure::FailureReceived || FailureType == ENetworkFailure::ConnectionLost || FailureType == ENetworkFailure::ConnectionTimeout;
	if (AOutlawsLevelActor* LU = FindLevel()) OutlawsUnlocks::EarnPending(LU, TEXT("the co-op session's end"));     // (REQ-UNLOCKS: as EndSession)
	// R8: the host closing its game is said plainly (no trying again); a connection that went quiet or was lost on the way
	// is tried again by the client (a reconnect's own failures keep trying until its two minutes are up)
	const bool bHostLeft = Mode == EOutlawsCoopMode::Client && FailureType == ENetworkFailure::FailureReceived && ErrorString.Contains(TEXT("Host closed"));
	const bool bLost = Mode == EOutlawsCoopMode::Client && !bHostLeft && (FailureType == ENetworkFailure::ConnectionLost || FailureType == ENetworkFailure::ConnectionTimeout);
	const FString Why = bHostLeft ? FString(TEXT("the host left the game"))
		: FailureType == ENetworkFailure::PendingConnectionFailure ? FString::Printf(TEXT("could not reach %s (%s)"), *ConnectingTo, *ErrorString)
		: bDropped ? FString::Printf(TEXT("the connection to the other player ended (%s)"), *ErrorString)
		: FString::Printf(TEXT("network failure (%s) %s"), ENetworkFailure::ToString(FailureType), *ErrorString);
	if (FailureType == ENetworkFailure::PendingConnectionFailure && bJoinFromMenu)
	{
		// (R8: said on the join page, which stays up with Retry)
		const bool bNoAnswer = ErrorString.IsEmpty() || ErrorString.Contains(TEXT("imed out")) || ErrorString.Contains(TEXT("imeout"));
		JoinError = bNoAnswer ? FString::Printf(TEXT("Could not reach %s (no answer in 12 s)"), *ConnectingTo) : FString::Printf(TEXT("Refused: %s"), *ErrorString);
	}
	if (bReconnectAttempt && !bHostLeft)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP reconnect try %d failed: %s"), ReconnectTries, *Why);
		const FString Keep = ReconnectTo;
		ResetSession();                  // (back to the front end; Tick tries again in a few seconds)
		ReconnectTo = Keep;
		return;
	}
	if (bLost && (!bAuto || IsDropoutTest() || IsPauseTest()))
	{
		StartReconnect(Why);
		return;
	}
#if WITH_OUTLAWS_TESTS
	if (bAuto)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP session ended: %s"), *Why);
		Summary(Why);
		QuitForTest();
		return;
	}
#endif
	EndSession(Why, false);             // (the engine itself takes this machine back to the main map: the front end)
}

void UOutlawsCoop::HandleTravelFailure(UWorld* World, ETravelFailure::Type FailureType, const FString& ErrorString)
{
	if (Mode == EOutlawsCoopMode::None || bExitRequested) return;
	const FString Why = FString::Printf(TEXT("travel failure (%s) %s"), ETravelFailure::ToString(FailureType), *ErrorString);
#if WITH_OUTLAWS_TESTS
	if (bAuto)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP session ended: %s"), *Why);
		Summary(Why);
		QuitForTest();
		return;
	}
#endif
	EndSession(Why, false);
}

// ---- the menu ---------------------------------------------------------------------------------------------------------

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TickMenuWalk(AOutlawsPlayer* P, double Now)
{
	// REQ-MPMENU (-OLMenuWalk=coop|comp): from the title menu, New Game > Multiplayer > Co-op or Competitive, then Host Game
	// (Competitive: the Host Game page's Start) or Join Game (the address typed on the pad, then Join), one press a second
	if (Now < MenuWalkAt) return;
	MenuWalkAt = Now + 1.0;
	const bool bComp = MenuWalk == TEXT("comp");
	const TCHAR* Labels[] = { TEXT("New Game"), TEXT("Multiplayer"), bComp ? TEXT("Competitive") : TEXT("Co-op"), bLaunchHost ? TEXT("Host Game") : TEXT("Join Game") };
	auto Said = [&](const FString& What, bool bOk)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP menu walk %s: %s %s (now on %s)"), *MenuWalk, *What, bOk ? TEXT("ok") : TEXT("FAILED"), *P->CoopMenuPage());
	};
	if (MenuWalkStep == 0 && !P->IsFrontMenuUp()) return;
	if (MenuWalkStep < 4)
	{
		const TCHAR* L = Labels[MenuWalkStep];
		const bool bOk = P->TutorialTestPressLabel(L);
		Said(FString::Printf(TEXT("%s pressed"), L), bOk);
		if (!bOk) { bLaunchPending = false; return; }
		if (++MenuWalkStep == 4 && bLaunchHost && !bComp) bLaunchPending = false;      // (co-op's Host Game hosts at once)
		return;
	}
	if (bLaunchHost)
	{
		// Competitive's Host Game page: Start hosts the match with the page's rows
		Said(TEXT("Start pressed on the Host Game page"), P->CoopPressRow(TEXT("mphost_start")));
		bLaunchPending = false;
		return;
	}
	// the join page, titled by the way in; the launch's address typed key by key (the pad has no colon: the port is this
	// machine's own, as a typed join's)
	Said(FString::Printf(TEXT("the join page is up, titled %s"), bJoinMatchTitle ? TEXT("JOIN A MATCH") : TEXT("JOIN A CO-OP GAME")),
		P->CoopMenuPage() == TEXT("#coopjoin") && bJoinMatchTitle == bComp);
	FString Addr = LaunchJoin;
	int32 Colon = INDEX_NONE;
	if (Addr.FindChar(':', Colon)) Addr.LeftInline(Colon);
	bool bTyped = true;
	for (int32 k = JoinAddress.Len(); k > 0; --k) bTyped &= P->CoopPressRow(TEXT("coop:key:del"));
	for (const TCHAR Ch : Addr) bTyped &= P->CoopPressRow(FString(TEXT("coop:key:")) + Ch);
	Said(FString::Printf(TEXT("typed %s (the page reads %s)"), *Addr, *JoinAddress), bTyped && JoinAddress == Addr);
	Said(TEXT("Join pressed"), P->CoopPressRow(TEXT("coop:join")));
	bLaunchPending = false;
}
#endif // WITH_OUTLAWS_TESTS

FString UOutlawsCoop::FrontPage(const UObject* Context)
{
	const UOutlawsCoop* C = Get(Context);
	if (!C || C->Mode == EOutlawsCoopMode::None) return TEXT("#front");
	const AOutlawsCoopSession* S = C->GetSession();
	// (REQ-MPLOBBY: the host's own page until it starts; from then, and for every joiner, the lobby)
	return C->Mode == EOutlawsCoopMode::Host ? (S && S->StartSerial > 0 ? TEXT("#cooplobby") : TEXT("#coophost"))
		: C->bJoinFromMenu && C->CurrentWorld() && C->CurrentWorld()->GetNetMode() == NM_Standalone ? TEXT("#coopjoin") : TEXT("#cooplobby");
}

uint32 UOutlawsCoop::PageVersion(const UObject* Context)
{
	const UOutlawsCoop* C = Get(Context);
	return C ? C->Version : 0;
}

void UOutlawsCoop::BuildMenuPage(AOutlawsPlayer* P, const FString& File, OutlawsMenu::FPage& Page)
{
	using namespace OutlawsMenu;
	UOutlawsCoop* C = Get(P);
	Page.File = File;
	auto Act = [&Page](const FString& Label, const FString& Name)
	{
		FItem I;
		I.Kind = EItemKind::Action;
		I.Label = Label;
		I.Name = Name;
		Page.Items.Add(I);
	};
	auto Link = [&Page](const TCHAR* Label, const TCHAR* Target)
	{
		FItem I;
		I.Kind = EItemKind::Link;
		I.Label = Label;
		I.Target = Target;
		Page.Items.Add(I);
	};
	// REQ-MPMENU: a Back row under each of the Multiplayer pages, up one level (not a "coop:" name: the menu's own Back)
	auto Back = [&Page](const TCHAR* Name)
	{
		FItem B;
		B.Kind = EItemKind::Action;
		B.Label = TEXT("Back");
		B.Name = Name;
		B.bGoBack = true;
		Page.Items.Add(B);
	};
	if (File == TEXT("#coopmulti"))
	{
		// REQ-MPMENU (user 15 Oct): New Game > Multiplayer: Co-op (the story together) or Competitive (a match)
		Page.Title = TEXT("MULTIPLAYER");
		Link(TEXT("Co-op"), TEXT("#coop"));
		Link(TEXT("Competitive"), TEXT("#coopcomp"));
		Back(TEXT("multi_back"));
	}
	else if (File == TEXT("#coop"))
	{
		Page.Title = TEXT("CO-OP");
		Act(TEXT("Host Game"), TEXT("coop:host"));
		Link(TEXT("Join Game"), TEXT("#coopjoin"));
		Back(TEXT("coopmenu_back"));
	}
	else if (File == TEXT("#coopcomp"))
	{
		// REQ-MPMENU: Competitive's Host Game is the match page (M1, OutlawsPlayerMpHost.cpp); its Join Game the same join
		// page as co-op's (the host decides what the joiner lands in)
		Page.Title = TEXT("COMPETITIVE");
		Link(TEXT("Host Game"), TEXT("#mphost"));
		Link(TEXT("Join Game"), TEXT("#coopjoin"));
		Back(TEXT("comp_back"));
	}
	else if (File == TEXT("#coopjoin"))
	{
		// a number pad (1 2 3 / 4 5 6 / 7 8 9 / . 0 Delete), then Join (Cancel while connecting).  REQ-MPMENU: titled by the
		// page it was opened from (Competitive: a match; Co-op: a co-op game); opened from anywhere else (the LAN bubble, a
		// session's own routing) it keeps the last one
		if (C)
		{
			C->ReadAddressFile();
			const FString From = P ? P->CoopMenuParent() : FString();
			if (From == TEXT("#coopcomp")) C->bJoinMatchTitle = true;
			else if (From == TEXT("#coop")) C->bJoinMatchTitle = false;
		}
		Page.Title = C && C->bJoinMatchTitle ? TEXT("JOIN A MATCH") : TEXT("JOIN A CO-OP GAME");
		for (const TCHAR* K : { TEXT("1"), TEXT("2"), TEXT("3"), TEXT("4"), TEXT("5"), TEXT("6"), TEXT("7"), TEXT("8"), TEXT("9"), TEXT("."), TEXT("0") })
		{
			Act(FCString::Strcmp(K, TEXT(".")) == 0 ? TEXT("Dot") : K, FString(TEXT("coop:key:")) + K);     // (a lone "." is too small to aim at)
		}
		Act(TEXT("Delete"), TEXT("coop:key:del"));
		const bool bConnecting = C && C->Mode == EOutlawsCoopMode::Client;
		if (!bConnecting) Act(FString::Printf(TEXT("Password: %s"), C && !C->JoinPassword.IsEmpty() ? *FString::ChrN(C->JoinPassword.Len(), '*') : TEXT("(none)")), TEXT("coop:joinpassword"));     // (M1)
		Act(bConnecting ? TEXT("Cancel") : C && !C->JoinError.IsEmpty() ? TEXT("Retry") : TEXT("Join"), bConnecting ? TEXT("coop:leave") : TEXT("coop:join"));
		// (R8) the last few hosts joined, newest first: one pick joins
		if (C && !bConnecting)
		{
			C->ReadRecent();
			for (int32 i = 0; i < C->RecentAddrs.Num(); ++i) Act(C->RecentLabel(i), FString::Printf(TEXT("coop:recent:%d"), i));
		}
	}
	else if (File == TEXT("#cooppause"))
	{
		if (C) C->BuildPausePage(P, Page);           // (R8: the pause menu's Co-op page: OutlawsCoopConnect.cpp)
	}
	else if (File == TEXT("#cooplobby") || File == TEXT("#coopclient"))
	{
		if (C) C->BuildLobbyPage(P, Page);           // (REQ-MPLOBBY: the shared lobby, host and joiners: OutlawsCoopLobby.cpp)
	}
	else if (File == TEXT("#coopscore"))
	{
		if (C) C->BuildScorePage(P, Page);           // (REQ-MPLOBBY b: the score screen at a match's end: OutlawsCoopNextMatch.cpp)
	}
	else if (File == TEXT("#coophost") && C)
	{
		// REQ-MPLOBBY / REQ-LOBBYTIDY (user 12 Oct): the co-op host before its start: Start, the level (or Continue) on its own
		// page, the options, Leave.  Start loads the level behind the lobby; the host lands in it and enters from there.
		Page.Title = TEXT("CO-OP LOBBY");
		Act(TEXT("Start"), TEXT("coop:start"));
		FString Saved;
		const bool bCont = C->CampaignAutosave(Saved) && C->LobbyLoadSlot == -2;
		Act(bCont ? FString::Printf(TEXT("Level:  Continue (%s)  >"), *Saved) : FString::Printf(TEXT("Level:  %s  >"), *CampaignLevelTitle(C->LevelToStart)), TEXT("coop:level"));
		FItem O;
		O.Kind = EItemKind::Link;
		O.Label = TEXT("Options");
		O.Target = TEXT("#coophostopts");
		Page.Items.Add(O);
		Act(TEXT("Leave"), TEXT("coop:leave"));
	}
	else if (File == TEXT("#cooplevels") && C)
	{
		// REQ-COOPLOBBY / REQ-LOBBYTIDY: the campaign, Continue (the co-op autosave, when it is this campaign's), then the
		// campaign's levels in story order; a pick is a New Game there
		const bool bHist = C->IsHistorical();
		Page.Title = TEXT("LEVEL");
		Act(FString::Printf(TEXT("Campaign: %s"), bHist ? TEXT("Historical Missions") : TEXT("Main Game")), TEXT("coop:campaign"));
		FString Saved;
		if (C->CampaignAutosave(Saved)) Act(FString::Printf(TEXT("%sContinue: %s"), C->LobbyLoadSlot == -2 ? TEXT("> ") : TEXT(""), *Saved), TEXT("coop:resume"));
		TArray<FString> Names, Labels;
		C->CampaignLevels(bHist, Names, Labels);
		for (int32 i = 0; i < Names.Num(); ++i)
		{
			Act(FString::Printf(TEXT("%s%s"), C->LobbyLoadSlot == -100 && Names[i] == C->LevelToStart ? TEXT("> ") : TEXT(""), *Labels[i]), TEXT("coop:pick:") + Names[i]);
		}
	}
	else if (File == TEXT("#coophostopts") && C)
	{
		// REQ-COOPLOBBY: everything else the host may set, each kept as it was on the lobby page before
		Page.Title = TEXT("CO-OP OPTIONS");
		const AOutlawsLevelActor* L = C->FindLevel();
		Act(FString::Printf(TEXT("Difficulty: %s"), L ? OutlawsDifficulty::Name(L->Difficulty) : TEXT("Good")), TEXT("coop:diff"));
		// (UNLOCKFREE, user decision for 0.95: the Bad guys row and the Randomizer for every host from the first start; Auto and
		// no randomizer until the host sets them)
		Act(C->IsBadGuysAuto() ? FString::Printf(TEXT("Bad guys: Auto (x%.2g)"), C->BadGuysMultiplier())
			: FString::Printf(TEXT("Bad guys: x%.2f"), C->BadGuysMultiplier()), TEXT("coop:badguys"));     // (R14)
		Act(FString::Printf(TEXT("Max players: %d"), C->MaxPlayersSetting()), TEXT("coop:maxplayers"));        // (N1)
		Act(FString::Printf(TEXT("Password: %s"), C->HostPassword.IsEmpty() ? TEXT("(none)") : *C->HostPassword), TEXT("coop:password"));     // (M1: optional)
		// (the Randomizer page, as in Options: the host's run goes to every machine with each start, REQ-COOPRANDO)
		{
			FItem R;
			R.Kind = EItemKind::Link;
			R.Label = TEXT("Randomizer");
			R.Target = TEXT("#rando");
			Page.Items.Add(R);
		}
		Act(C->bShowInternet ? TEXT("Hide internet address") : TEXT("Show internet address"), TEXT("coop:netaddr"));     // (REQ-LOBBYTIDY: from the lobby page)
		Act(TEXT("Back"), TEXT("coop:back"));
	}
}

void UOutlawsCoop::StatusLines(const UObject* Context, const FString& File, TArray<FString>& Out)
{
	const UOutlawsCoop* C = Get(Context);
	if (!C) return;
	const AOutlawsCoopSession* S = C->GetSession();
	if (File == TEXT("#coopjoin"))
	{
		Out.Add(FString::Printf(TEXT("Host address:  %s_"), *C->JoinAddress));
		// (R8: the dots move while it waits; a failure said plainly, Retry under it)
		const int32 Dots = int32(FPlatformTime::Seconds() * 2.0) % 4;
		Out.Add(C->Mode == EOutlawsCoopMode::Client ? FString::Printf(TEXT("Connecting to %s %s"), *C->ConnectingTo, *FString::ChrN(Dots + 1, '.'))
			: !C->JoinError.IsEmpty() ? C->JoinError : FString(TEXT("Type the address the host's lobby shows, then Join")));
	}
	else if (File == TEXT("#coophost"))
	{
		Out.Add(FString::Printf(TEXT("Your address:  %s    port %d"), C->LanAddress.IsEmpty() ? TEXT("(unknown)") : *C->LanAddress, C->ListenPort));
		if (!C->OtherAddresses.IsEmpty()) Out.Add(FString::Printf(TEXT("(this PC also answers on %s)"), *C->OtherAddresses));
		const FString Shown = C->ShownInternetAddress();
		if (C->bShowInternet && !Shown.IsEmpty() && !(C->UpnpState == EUpnp::Opened && !C->UpnpShared()))
		{
			// (R8: for friends over the internet, when the router passes UDP 7777 to this PC; REQ-UPNP: the router's own
			// address when it gave one, and no line of its own when the Internet line below already carries it)
			Out.Add(Shown == TEXT("unknown") ? FString(TEXT("Your internet address:  no answer (is this PC online?)"))
				: FString::Printf(TEXT("Your internet address:  %s%s"), *Shown,
				Shown.Contains(TEXT(".")) ? *FString::Printf(TEXT(":%d"), C->ListenPort) : TEXT("")));
		}
		C->UpnpLines(Out);
		// (REQ-MPLOBBY: no Ready; who is here, and who has pressed Enter Game, which takes them in with the host's Start)
		if (!S || !S->bClientConnected) Out.Add(TEXT("Waiting for friends to join (they can join any time)"));
		else C->LobbySlotLines(Out);
#if PLATFORM_ANDROID
		if (C->BadGuysMultiplier() > 2.5f) Out.Add(TEXT("Many bad guys: a Quest host may slow down"));       // (R14)
#endif
	}
	else if (File == TEXT("#cooppause"))
	{
		C->PausePageLines(Out);
	}
	else if (File == TEXT("#coopscore"))
	{
		C->ScoreLines(Out);                          // (REQ-MPLOBBY b: the score screen: OutlawsCoopNextMatch.cpp)
	}
	else if (File == TEXT("#cooplobby") || File == TEXT("#coopclient"))
	{
		C->LobbyLines(Out);                          // (REQ-MPLOBBY: OutlawsCoopLobby.cpp)
	}
}

void UOutlawsCoop::MenuAction(AOutlawsPlayer* P, const FString& Name)
{
	UOutlawsCoop* C = Get(P);
	if (!C || !P) return;
	if (Name == TEXT("coop:host"))
	{
		C->Host();
	}
	else if (Name == TEXT("coop:join"))
	{
		if (C->JoinAddress.Len() < 7)
		{
			P->CoopToast(TEXT("Type the host's address first"));
			return;
		}
		C->Join(C->JoinAddress, true);
		P->CoopShowPage(TEXT("#coopjoin"), false);       // (Join becomes Cancel)
	}
	else if (Name.StartsWith(TEXT("coop:key:")))
	{
		C->TypeKey(Name.Mid(9));
		P->CoopShowPage(TEXT("#coopjoin"), false);
	}
	else if (Name == TEXT("coop:ready") || Name == TEXT("coop:enter") || Name.StartsWith(TEXT("coop:enter:")))
	{
		// REQ-MPLOBBY: Enter Game (Join Red / Join Blue in a team match), one-way; Ready is gone
		C->EnterGame(P, Name.StartsWith(TEXT("coop:enter:")) ? FCString::Atoi(*Name.Mid(11)) : 0);
	}
	else if (Name == TEXT("coop:continue"))
	{
		C->ScoreContinue(P);                     // (REQ-MPLOBBY b: the score screen's Continue)
	}
	else if (Name == TEXT("coop:level"))
	{
		P->CoopShowPage(TEXT("#cooplevels"), true);
	}
	else if (Name.StartsWith(TEXT("coop:mute:")))
	{
		// REQ-MPLOBBY: one player's voice muted here (or heard again)
		const int32 Who = FCString::Atoi(*Name.Mid(10));
		if (Who >= 0 && Who < MaxPlayers)
		{
			C->MutedBits ^= 1u << Who;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP %s: the %s's voice %s"), P->CoopMenuPage() == TEXT("#cooppause") ? TEXT("pause Players page") : TEXT("lobby"), WhoWord(Who),
				(C->MutedBits & (1u << Who)) ? TEXT("muted") : TEXT("heard again"));
		}
	}
	else if (Name == TEXT("coop:moreplayers"))
	{
		++C->PlayerListPage;                     // (REQ-MPLOBBY c: the next page of the player list, round again after the last)
	}
	else if (Name == TEXT("coop:campaign"))
	{
		C->SwitchCampaign();
	}
	else if (Name == TEXT("coop:newgame"))
	{
		// (REQ-COOPLOBBY) a New Game: the campaign's level list, the level now chosen marked
		C->LobbyLoadSlot = -100;
		C->bResumeChosen = false;
		P->CoopShowPage(TEXT("#cooplevels"), true);
	}
	else if (Name.StartsWith(TEXT("coop:pick:")))
	{
		C->LevelToStart = Name.Mid(10).ToLower();
		C->LobbyLoadSlot = -100;
		C->bResumeChosen = false;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby: New Game at %s (%s)"), *C->LevelToStart, C->IsHistorical() ? TEXT("Historical Missions") : TEXT("Main Game"));
		P->CoopMenuBack();
	}
	else if (Name == TEXT("coop:back"))
	{
		P->CoopMenuBack();
	}
	else if (Name == TEXT("coop:diff"))
	{
		C->CycleDifficulty();
	}
	else if (Name == TEXT("coop:start"))
	{
		C->StartGame(true);              // (REQ-MPLOBBY: the level loads behind the lobby; whoever entered comes along)
	}
	else if (Name.StartsWith(TEXT("coop:recent:")))
	{
		// (R8) a Recent host joined at once
		const int32 i = FCString::Atoi(*Name.Mid(12));
		C->ReadRecent();
		if (C->RecentAddrs.IsValidIndex(i))
		{
			C->ReadRecentPasswords();
			FString Host = C->RecentAddrs[i];
			Host.Split(TEXT(":"), &Host, nullptr);
			if (const FString* Pw = C->RecentPasswords.Find(Host)) C->JoinPassword = *Pw;      // (M1: the password it took last time)
			C->Join(C->RecentAddrs[i], true);
			P->CoopShowPage(TEXT("#coopjoin"), false);
		}
	}
	else if (Name == TEXT("coop:badguys"))
	{
		// (R14) Auto, then x1.0 to x4.0 in quarters, then Auto again; saved with the settings
		if (AOutlawsLevelActor* L = C->FindLevel())
		{
			// (REQ-TRIGGERSTEP: a number row of 13, Auto first)
			const int32 Cur = L->CoopBadGuys <= 0.f ? 0 : FMath::Clamp(FMath::RoundToInt((L->CoopBadGuys - 1.f) / 0.25f) + 1, 1, 12);
			const int32 Next = OutlawsMenu::StepIndex(Cur, 13, true);
			L->CoopBadGuys = Next == 0 ? 0.f : 1.f + 0.25f * float(Next - 1);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby: bad guys %s"), L->CoopBadGuys <= 0.f ? *FString::Printf(TEXT("Auto (x%.2f)"), C->BadGuysMultiplier()) : *FString::Printf(TEXT("x%.2f"), L->CoopBadGuys));
			P->CoopSaveSettings();
		}
	}
	else if (Name == TEXT("coop:maxplayers"))
	{
		// (N1) 2 to 8 and round again; saved with the settings; never fewer than the players already in
		if (AOutlawsLevelActor* L = C->FindLevel())
		{
			const AOutlawsCoopSession* S = C->GetSession();
			const int32 In = S ? FMath::Max(2, S->Connected) : 2;
			const int32 Count = FMath::Max(1, MaxPlayers - In + 1);          // (REQ-TRIGGERSTEP: a number row)
			L->CoopMaxPlayers = In + OutlawsMenu::StepIndex(FMath::Clamp(L->CoopMaxPlayers - In, 0, Count - 1), Count, true);
			C->TestMaxPlayers = 0;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby: max players %d"), L->CoopMaxPlayers);
			P->CoopSaveSettings();
			C->RefreshLobby();
		}
	}
	else if (Name == TEXT("coop:netaddr"))
	{
		C->SetShowInternet(!C->bShowInternet);     // (asked only by the host's choice: a home-network game asks no one)
	}
	else if (Name == TEXT("coop:password"))
	{
		P->CoopOpenKeyboard(TEXT("coop_password"), TEXT("GAME PASSWORD"), C->HostPassword, 16);
	}
	else if (Name == TEXT("coop:joinpassword"))
	{
		P->CoopOpenKeyboard(TEXT("join_password"), TEXT("THE HOST'S PASSWORD"), C->JoinPassword, 16);
	}
	else if (Name == TEXT("coop:team"))
	{
		// (M2) the other team, from now (what this player carries is dropped)
		const int32 Other = MyTeam(P) == 1 ? 2 : 1;
		if (C->IsHost()) C->OnMatchTeam(0, Other);
		else if (AOutlawsCoopLink* Mine = C->GetLink(C->MyIndex())) Mine->ServerMatchTeam(uint8(Other));
		P->CoopCloseMenu();
	}
	else if (Name == TEXT("coop:scoreboard"))
	{
		ToggleScoreboard(P, TEXT("the menu's Scoreboard row"));      // (M1: the match's frag table in front of the player, up until closed)
		P->CoopCloseMenu();
	}
	else if (Name == TEXT("coop:startnow"))
	{
		C->StartGame(true);              // (R7: the host goes in now; from the co-op autosave if Continue is chosen)
	}
	else if (Name == TEXT("coop:resume"))
	{
		// Continue (REQ-COOPLOBBY; Resume Game before): the co-op autosave, chosen on the level page; Start loads it (REQ-MPLOBBY:
		// joiners still in the lobby enter into the restored level)
		C->LobbyLoadSlot = -2;
		C->bResumeChosen = true;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby: Continue (the co-op autosave, %s) chosen: Start loads it"), *AOutlawsLevelActor::ReadSaveInfo(-2).Name);
		if (P->CoopMenuPage() == TEXT("#cooplevels")) P->CoopMenuBack();
	}
	else if (Name == TEXT("coop:leave"))
	{
		const UWorld* W = C->CurrentWorld();
		C->EndSession(C->Mode == EOutlawsCoopMode::Client && W && !W->IsNetMode(NM_Client) ? TEXT("the join was cancelled") : TEXT("left the lobby"), true);
	}
	++C->Version;
}

void UOutlawsCoop::LobbySlotLines(TArray<FString>& Out) const
{
	// N1: "Players 3 of 8:" then each in turn, "1. Jesse (host, ready)", the name each set, else Player n
	const AOutlawsCoopSession* S = GetSession();
	TArray<AOutlawsCoopLink*> Links;
	GetLinks(Links);
	Out.Add(FString::Printf(TEXT("Players %d of %d:"), S ? S->Connected : Links.Num(), S ? S->MaxPlayers : MaxPlayersSetting()));
	FString Line;
	for (const AOutlawsCoopLink* K : Links)
	{
		const FString Name = K->PlayerName.TrimStartAndEnd().IsEmpty() ? FString::Printf(TEXT("Player %d"), K->PlayerIndex + 1) : K->PlayerName.TrimStartAndEnd();
		if (K->bBot) continue;
		const FString Slot = FString::Printf(TEXT("%d. %s (%s%s)"), K->PlayerIndex + 1, *Name, K->PlayerIndex == 0 ? TEXT("host, ") : K->PlayerIndex == MyIndex() ? TEXT("you, ") : TEXT(""),
			K->PlayerIndex == 0 ? (S && S->StartSerial > 0 && S->bHostReady ? TEXT("in game") : TEXT("in lobby")) : !IsPlayerEntered(K) ? TEXT("in lobby") : K->BuiltSerial > 0 ? TEXT("in game") : TEXT("entered"));
		Line += Line.IsEmpty() ? Slot : TEXT("    ") + Slot;
		if (Line.Len() > 60)
		{
			Out.Add(Line);
			Line.Reset();
		}
	}
	if (!Line.IsEmpty()) Out.Add(Line);
}

void UOutlawsCoop::TypeKey(const FString& Key)
{
	if (Mode != EOutlawsCoopMode::None) return;          // (not while connecting)
	ReadAddressFile();
	if (Key == TEXT("del"))
	{
		JoinAddress.LeftChopInline(1);
	}
	else if (JoinAddress.Len() < 21)
	{
		JoinAddress += Key;
	}
}

// ---- REQ-COOPLOBBY: the lobby's campaign and its one level list ----------------------------------------------------------

bool UOutlawsCoop::IsMissionLevel(const FString& Level)
{
	// (REQ-HMCAREER) Marshal Training's office and its five missions (a co-op autosave inside one still continues as
	// Historical Missions), and the Handful of Missions
	const FString L = Level.ToLower();
	return L == TEXT("office") || AOutlawsLevelActor::IsCareerMission(L) || AOutlawsLevelActor::IsHandfulLevel(L);
}

FString UOutlawsCoop::CampaignLevelTitle(const FString& Level)
{
	if (Level.Equals(TEXT("office"), ESearchCase::IgnoreCase)) return TEXT("Marshal Training");     // (REQ-HMCAREER)
	TArray<FString> Names, Labels;
	AOutlawsLevelActor::GetMissionChoices(Names, Labels);
	const int32 At = Names.IndexOfByKey(Level.ToLower());
	return At != INDEX_NONE ? Labels[At] : AOutlawsLevelActor::LevelTitle(Level);
}

void UOutlawsCoop::HistoricalLevels(const AOutlawsLevelActor* L, TArray<FString>& OutNames)
{
	// REQ-HMCAREER (user 14 Oct): as the Historical Missions page: Marshal Training (its office, a Historical Missions session
	// for every player; the five missions from its posters), then the Handful of Missions; never the five one by one
	OutNames.Add(TEXT("office"));
	TArray<FString> Labels;
	AOutlawsLevelActor::GetHandfulChoices(OutNames, Labels, L ? L->GetAssets() : nullptr);
	OutNames.RemoveAll([](const FString& N) { return AOutlawsLevelActor::IsCareerMission(N); });
}

void UOutlawsCoop::CampaignLevels(bool bHistorical, TArray<FString>& OutNames, TArray<FString>& OutLabels) const
{
	if (bHistorical)
	{
		HistoricalLevels(FindLevel(), OutNames);
	}
	else
	{
		// the story in play order as outlaws.rcs lists it (the install's own names: town or townex), never the other levels
		static const TCHAR* const Story[] = { TEXT("hideout"), TEXT("town"), TEXT("townex"), TEXT("train"), TEXT("canyon"), TEXT("mill"),
			TEXT("simms"), TEXT("miner"), TEXT("cliff"), TEXT("ranch") };
		auto IsStory = [](const FString& N) { for (const TCHAR* S : Story) { if (N == S) return true; } return false; };
		TArray<FString> All, AllLabels;
		if (const AOutlawsLevelActor* L = FindLevel()) L->GetLevelChoices(All, AllLabels);
		for (const FString& N : All) if (IsStory(N)) OutNames.Add(N);
		if (OutNames.Num() == 0)
		{
			for (const TCHAR* S : Story) if (FCString::Strcmp(S, TEXT("town")) != 0) OutNames.Add(S);
		}
	}
	for (int32 i = 0; i < OutNames.Num(); ++i) OutLabels.Add(FString::Printf(TEXT("%d. %s"), i + 1, *CampaignLevelTitle(OutNames[i])));
}

bool UOutlawsCoop::CampaignAutosave(FString& OutWhat) const
{
	const AOutlawsLevelActor::FSaveInfo Auto = AOutlawsLevelActor::ReadSaveInfo(-2);
	if (Auto.bEmpty || IsMissionLevel(Auto.Level) != IsHistorical()) return false;
	const FString Where = Auto.Name.IsEmpty() ? CampaignLevelTitle(Auto.Level) : Auto.Name;
	OutWhat = Auto.Stamp.IsEmpty() ? Where : FString::Printf(TEXT("%s, %s"), *Where, *Auto.Stamp);
	return true;
}

void UOutlawsCoop::SwitchCampaign()
{
	// the other campaign at its first level, a New Game unless its own autosave is there to continue (the user picks)
	TArray<FString> Names, Labels;
	CampaignLevels(!IsHistorical(), Names, Labels);
	if (Names.Num() == 0) return;
	LevelToStart = Names[0];
	LobbyLoadSlot = -100;
	bResumeChosen = false;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby: campaign %s (New Game at %s)"), IsHistorical() ? TEXT("Historical Missions") : TEXT("Main Game"), *LevelToStart);
	++Version;
}

void UOutlawsCoop::CycleDifficulty()
{
	if (AOutlawsLevelActor* L = FindLevel())
	{
		L->Difficulty = static_cast<EOutlawsDifficulty>(OutlawsMenu::StepIndex(int32(L->Difficulty), 3, false));       // (no level is up: nothing to rescale)
	}
	++Version;
}

void UOutlawsCoop::ReadAddressFile()
{
	if (bAddressRead) return;
	bAddressRead = true;
	FString Text;
	if (FFileHelper::LoadFileToString(Text, *AddressFile())) JoinAddress = Text.TrimStartAndEnd().Left(21);
	if (JoinAddress.IsEmpty())
	{
		// this machine's own network prefix (the host is most likely on the same one): 192.168.1.
		const FString Mine = FindLanAddress();
		int32 Dot = INDEX_NONE;
		if (Mine.FindLastChar(TEXT('.'), Dot)) JoinAddress = Mine.Left(Dot + 1);
	}
}

void UOutlawsCoop::WriteAddressFile() const
{
	FFileHelper::SaveStringToFile(JoinAddress, *AddressFile());
}

FString UOutlawsCoop::FindLanAddress(FString* OutOthers)
{
	ISocketSubsystem* SS = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SS) return FString();
	// the route's own source address: a UDP socket "connected" toward a public address (no packet leaves this machine)
	FString Route;
	{
		TSharedRef<FInternetAddr> To = SS->CreateInternetAddr();
		bool bValid = false;
		To->SetIp(TEXT("8.8.8.8"), bValid);
		To->SetPort(53);
		if (FSocket* S = bValid ? SS->CreateSocket(NAME_DGram, TEXT("OutlawsCoopRoute"), To->GetProtocolType()) : nullptr)
		{
			if (S->Connect(*To))
			{
				TSharedRef<FInternetAddr> Me = SS->CreateInternetAddr();
				S->GetAddress(*Me);
				Route = Me->ToString(false);
			}
			S->Close();
			SS->DestroySocket(S);
		}
		if (Route.StartsWith(TEXT("0.")) || Route.StartsWith(TEXT("127.")) || Route.Contains(TEXT(":"))) Route.Reset();
	}
	// every IPv4 address the adapters have (not loopback, not self-assigned): a private home-network one first, the usual
	// virtual adapters' ranges last (VirtualBox 192.168.56, the hotspot's 192.168.137, 172.16-31: Docker, WSL, ZeroTier)
	TArray<TSharedPtr<FInternetAddr>> Addrs;
	TArray<TPair<int32, FString>> Ranked;
	if (SS->GetLocalAdapterAddresses(Addrs))
	{
		for (const TSharedPtr<FInternetAddr>& A : Addrs)
		{
			if (!A.IsValid() || A->GetProtocolType() != FNetworkProtocolTypes::IPv4) continue;
			const FString Ip = A->ToString(false);
			if (Ip.StartsWith(TEXT("127.")) || Ip.StartsWith(TEXT("169.254.")) || Ip.StartsWith(TEXT("0."))) continue;
			TArray<FString> O;
			Ip.ParseIntoArray(O, TEXT("."));
			const int32 B = O.Num() == 4 ? FCString::Atoi(*O[1]) : -1, C = O.Num() == 4 ? FCString::Atoi(*O[2]) : -1;
			int32 Rank = 4;
			if (Ip.StartsWith(TEXT("192.168.")) && C != 56 && C != 137) Rank = 0;
			else if (Ip.StartsWith(TEXT("10."))) Rank = 1;
			else if (Ip.StartsWith(TEXT("192.168."))) Rank = 2;
			else if (Ip.StartsWith(TEXT("172.")) && B >= 16 && B <= 31) Rank = 3;
			if (!Ranked.ContainsByPredicate([&Ip](const TPair<int32, FString>& R) { return R.Value == Ip; })) Ranked.Emplace(Rank, Ip);
		}
	}
	Ranked.StableSort([](const TPair<int32, FString>& X, const TPair<int32, FString>& Y) { return X.Key < Y.Key; });
	const FString Primary = !Route.IsEmpty() ? Route : Ranked.Num() > 0 ? Ranked[0].Value : FString();
	if (OutOthers)
	{
		TArray<FString> Rest;
		for (const TPair<int32, FString>& R : Ranked) if (R.Value != Primary && Rest.Num() < 3) Rest.Add(R.Value);
		*OutOthers = FString::Join(Rest, TEXT(", "));
	}
	return Primary;
}

// ---- milestone 2: poses and the other player ----------------------------------------------------------------------------

void UOutlawsCoop::SendPose(AOutlawsPlayer* P)
{
	AOutlawsCoopLink* Mine = GetLink(MyIndex());
	if (!Mine || !P) return;
	FOutlawsCoopPose Pose;
	Pose.Body = P->GetActorLocation();
	Pose.BodyYaw = float(P->GetActorRotation().Yaw);
	Pose.HalfHeight = P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	Pose.PlateSize = int16(FMath::Clamp(P->ChestPlateSize(), 0, 30000));       // (REQ-CHESTPLATE: the plate worn, seen on them)
	Pose.ChestYaw = P->ChestYaw();
	if (const UCameraComponent* Cam = P->Camera.Get())
	{
		Pose.Head = Cam->GetComponentLocation();
		Pose.HeadRot = Cam->GetComponentRotation();
	}
	// the hands as this machine has them (in VR the controllers; on the desktop wherever the desktop leaves them)
	if (const UOutlawsBoxHand* L = P->LeftHand.Get())
	{
		Pose.LeftHand = L->GetComponentLocation();
		Pose.LeftRot = L->GetComponentRotation();
	}
	if (const UOutlawsBoxHand* R = P->RightHand.Get())
	{
		Pose.RightHand = R->GetComponentLocation();
		Pose.RightRot = R->GetComponentRotation();
	}
	P->CoopHeld(0, Pose.LeftWeapon, Pose.LeftTool);
	P->CoopHeld(1, Pose.RightWeapon, Pose.RightTool);
	// milestone 6 item 4: a bad guy in a hand, and where this machine has him (the host puts its copy there)
	for (int32 h = 0; h < 2; ++h)
	{
		const AOutlawsEnemy* E = P->CoopGrabbed(h);
		if (!E || E->CoopIndex < 0 || !E->bHeldByPlayer) continue;
		(h == 0 ? Pose.LeftHeld : Pose.RightHeld) = E->CoopIndex;
		(h == 0 ? Pose.LeftHeldAt : Pose.RightHeldAt) = E->GetActorLocation();
	}
	// BUG-LANTERNNET: the lantern: lit as this machine lights it (its own light's place and oil), else in a hand or put away
	{
		FVector LampAt;
		float Oil = 0.f;
		const AOutlawsLevelActor* Lv = FindLevel();
		if (Lv && Lv->LanternShining(LampAt, Oil))
		{
			Pose.Lamp = 2;
			Pose.LampAt = LampAt;
			Pose.LampOil = uint8(FMath::RoundToInt(FMath::Clamp(Oil, 0.f, 1.f) * 255.f));
		}
		else
		{
			Pose.Lamp = Pose.LeftTool == TEXT("ilantern") || Pose.RightTool == TEXT("ilantern") ? 1 : 0;
		}
		if (Pose.Lamp != LampSent)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lantern out to the others: %s%s"), Pose.Lamp == 2 ? TEXT("lit") : Pose.Lamp == 1 ? TEXT("in hand, out") : TEXT("put away"),
				Pose.Lamp == 2 ? *FString::Printf(TEXT(" at %s, oil %d/255"), *LampAt.ToString(), Pose.LampOil) : TEXT(""));
			LampSent = Pose.Lamp;
		}
	}
	// REQ-FLICKRELOAD: the open barrels (the host's own are its word; a client's are what the host takes from its ServerBreakFx)
	Pose.Break = P->CoopBreakBits();
	if (IsHost()) Mine->BreakAccepted = Pose.Break;
	Pose.bVR = P->IsVR();
	Pose.GroundSpeed = float(P->GetVelocity().Size2D());
	Pose.SentAt = float(FPlatformTime::Seconds() - GStartTime);
	Pose.Seq = ++PoseSeq;
	Mine->ServerPose(Pose);            // (the host's own link: set here at once; the client's: to the host, unreliable)
}

double UOutlawsCoop::PoseInterval(const AOutlawsPlayer* P) const
{
	// N1: twenty a second; with three or more players, fewer while every other player is far off (5 beyond 30 m, 2 beyond
	// 60 m, 10 beyond 16 m): the nearest decides, so a partner close by always sees this player smooth
	const AOutlawsCoopSession* S = GetSession();
	if (!P || !S || S->Connected <= 2) return 0.05;
	TArray<AOutlawsCoopLink*> Links;
	GetLinks(Links);
	double Nearest = 1e12;
	const int32 Me = MyIndex();
	for (const AOutlawsCoopLink* K : Links)
	{
		if (K->PlayerIndex == Me || K->Pose.Seq <= 0 || K->BuiltSerial != StartedSerial) continue;
		Nearest = FMath::Min(Nearest, FVector::Dist(P->GetActorLocation(), FVector(K->Pose.Body)));
	}
	return Nearest > 6000.0 ? 0.5 : Nearest > 3000.0 ? 0.2 : Nearest > 1600.0 ? 0.1 : 0.05;
}

void UOutlawsCoop::TickRemote(double Now)
{
	if (!bLevelBuilt) return;
	UWorld* W = CurrentWorld();
	AOutlawsPlayer* P = LocalPlayer();
	// this machine's pose out, twenty a second (N1: fewer when everybody else is far, PoseInterval)
	if (P && Now >= NextPoseAt)
	{
		const double Every = PoseInterval(P);
		NextPoseAt = Now + Every;
		SendPose(P);
		const FString Rate = FString::Printf(TEXT("%.0f a second"), 1.0 / Every);
		if (Rate != PoseRateLogged)
		{
			if (!PoseRateLogged.IsEmpty() || Every > 0.06) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP poses out at %s (the nearest other player decides)"), *Rate);
			PoseRateLogged = Rate;
		}
	}
	// every other player's in: noted (at most once a second each in the log) and drawn by a puppet of their own (N1)
	TArray<AOutlawsCoopLink*> Links;
	GetLinks(Links);
	const int32 Me = MyIndex();
	bool bSpriteTried = false;
	for (AOutlawsCoopLink* OL : Links)
	{
		const int32 Other = OL->PlayerIndex;
		if (Other == Me) continue;
		const TCHAR* OtherName = WhoWord(Other);
		if (OL->Pose.Seq > 0 && OL->Pose.Seq != LastPoseSeq[Other])
		{
			LastPoseSeq[Other] = OL->Pose.Seq;
			++PosesReceived;
			if (OL->Pose.Lamp != LampHeard[Other])
			{
				LampHeard[Other] = OL->Pose.Lamp;
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lantern of the %s: %s%s"), OtherName, OL->Pose.Lamp == 2 ? TEXT("lit") : OL->Pose.Lamp == 1 ? TEXT("in hand, out") : TEXT("put away"),
					OL->Pose.Lamp == 2 ? *FString::Printf(TEXT(" at %s, oil %d/255"), *FVector(OL->Pose.LampAt).ToString(), OL->Pose.LampOil) : TEXT(""));
			}
			if (FirstPoseAt == 0.0) FirstPoseAt = Now;
			if (Now - LastPoseLogAt[Other] >= 1.0)
			{
				LastPoseLogAt[Other] = Now;
				const FOutlawsCoopPose& Q = OL->Pose;
				UE_LOG(LogOutlawsCoop, Verbose, TEXT("OLCOOP pose from %s: %s (yaw %.0f, head %s, hands %s / %s, holding %d / %d, %s, #%d, %d received)"), OtherName, *FVector(Q.Body).ToString(),
					Q.BodyYaw, *FVector(Q.Head).ToString(), *FVector(Q.LeftHand).ToString(), *FVector(Q.RightHand).ToString(), Q.LeftWeapon, Q.RightWeapon, Q.bVR ? TEXT("VR") : TEXT("desktop"), Q.Seq, PosesReceived);
			}
		}
		// (only once the other machine has built this same level: before that its pose is still the last level's place)
		if (W && OL->Pose.Seq > 0 && OL->BuiltSerial == StartedSerial && !Puppets[Other].IsValid())
		{
			FActorSpawnParameters SP;
			SP.ObjectFlags |= RF_Transient;
			SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			if (AOutlawsRemotePlayer* R = W->SpawnActor<AOutlawsRemotePlayer>(AOutlawsRemotePlayer::StaticClass(), FVector(OL->Pose.Body), FRotator::ZeroRotator, SP))
			{
				R->PlayerIndex = Other;
				R->Link = OL;
				Puppets[Other] = R;
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the other player (%s) drawn here as a puppet at %s"), OtherName, *FVector(OL->Pose.Body).ToString());
			}
		}
		// the puppet's sprite (James Anderson): made once it is up, and again whenever it is gone (a build of the level destroys
		// every sprite the level owns); a failure is tried again every two seconds
		// (M1: in a match, the player's character's sprite set; it is made again when the character changes, on a respawn)
		// (REQ-COOPCHAR: in co-op too, a joiner's chosen character; the host's link has none: James Anderson)
		const FOutlawsMpCharacter* Ch = !OL->Character.IsEmpty() ? FOutlawsMpCharacter::Find(OL->Character) : nullptr;
		const FString WantNwx = Ch ? FString(Ch->Nwx) : FString(PuppetNwx);
		if (AOutlawsRemotePlayer* Rp = Puppets[Other].Get(); Rp && !Rp->NeedsSprite() && !Rp->SpriteNwx.Equals(WantNwx, ESearchCase::IgnoreCase))
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the %s is now drawn as %s (%s)"), OtherName, Ch ? Ch->Name : TEXT("James Anderson"), *WantNwx);
			Rp->SetSprite(nullptr, FindLevel());
			NextPuppetSpriteAt = 0.0;
		}
		// (BUG-TNTVANISH: its sprite went on with a body still in the air as it came back: a new one at once)
		if (AOutlawsRemotePlayer* Rp = Puppets[Other].Get(); Rp && Rp->NeedsSprite() && (Now >= NextPuppetSpriteAt || Rp->SpriteGivenAway()))
		{
			bSpriteTried = true;
			if (AOutlawsLevelActor* Lv = FindLevel())
			{
				const FVector Feet = FVector(OL->Pose.Body) - FVector(0.0, 0.0, OL->Pose.HalfHeight);
				Rp->SetSprite(MakePuppetSprite(Lv, Feet, float(OL->Pose.HeadRot.Yaw), WantNwx), Lv);
				Rp->SpriteNwx = WantNwx;
			}
		}
	}
	if (bSpriteTried) NextPuppetSpriteAt = Now + 2.0;
	// a puppet whose player left (its link gone) goes too
	for (int32 i = 0; i < MaxPlayers; ++i)
	{
		AOutlawsRemotePlayer* R = Puppets[i].Get();
		if (R && !R->Link.IsValid())
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the puppet of player %d (%s) goes: their link is gone"), i, WhoWord(i));
			R->Destroy();
			Puppets[i].Reset();
		}
	}
	// -OLCoopAuto: once the other's poses have flowed a while, turn to face them, photograph it, say how far they are
	AOutlawsRemotePlayer* R = PartnerPuppet();
	if (bAuto && P && R && FirstPoseAt > 0.0 && ShotStep < 2)
	{
		APlayerController* PC = Cast<APlayerController>(P->GetController());
		const UCameraComponent* Cam = P->Camera.Get();
		const FVector Eye = Cam ? Cam->GetComponentLocation() : P->GetActorLocation();
		const FVector Target = R->ShownBody() + FVector(0.0, 0.0, 30.0);
		if (ShotStep == 0 && Now - FirstPoseAt > 2.0)
		{
			if (PC) PC->SetControlRotation((Target - Eye).Rotation());
			ShotAt = Now;
			ShotStep = 1;
		}
		else if (ShotStep == 1 && Now - ShotAt > 0.6)
		{
			Shot(Mode == EOutlawsCoopMode::Host ? TEXT("ol_coop_host") : TEXT("ol_coop_client"));
			SeenDistance = float(FVector::Dist(Eye, R->ShownBody()));
			FHitResult Hit;
			FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsCoopSee), false, P);
			const bool bBlocked = W && W->LineTraceSingleByChannel(Hit, Eye, R->ShownBody(), ECC_Visibility, Q);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP sees the other player at %.0f cm (%s; looking %s)"), SeenDistance,
				bBlocked ? *FString::Printf(TEXT("something in between: %s"), Hit.GetActor() ? *Hit.GetActor()->GetName() : TEXT("?")) : TEXT("nothing in between"),
				*(Target - Eye).Rotation().ToString());
			ShotStep = 2;
		}
	}
}

// ---- the puppet as James Anderson (user 2026-10-02) ---------------------------------------------------------------------

static FString PuppetCharacterName(const FString& Nwx)
{
	// (the log's words: "James Anderson" as ever in co-op; a match's character by its sprite set)
	int32 N = 0;
	const FOutlawsMpCharacter* All = FOutlawsMpCharacter::All(N);
	for (int32 i = 0; i < N; ++i) if (Nwx.Equals(All[i].Nwx, ESearchCase::IgnoreCase)) return All[i].Name;
	return TEXT("James Anderson");
}

AOutlawsSpriteActor* UOutlawsCoop::MakePuppetSprite(AOutlawsLevelActor* L, const FVector& Feet, float Yaw, const FString& Nwx)
{
	// the hero's own sprite, every choreography in one atlas, made the way a bad guy's is (the level owns it: its HD look
	// follows the remastered-textures switch, a build of the level clears it away)
	if (!L) return nullptr;
	const FString Want = Nwx.IsEmpty() ? FString(PuppetNwx) : Nwx;
	const TSharedPtr<OutlawsTextures::FSpriteAtlas> Atlas = L->GetAtlas(Want, true);
	AOutlawsSpriteActor* S = Atlas && Atlas->Chors.Num() > 0 ? L->MakeSprite(Atlas, FPaths::GetBaseFilename(Want).ToUpper(), Feet, Yaw, L->LightAt(Feet + FVector(0.0, 0.0, 5.0))) : nullptr;
	if (!S)
	{
		if (!bPuppetSpriteWarned)
		{
			bPuppetSpriteWarned = true;
			UE_LOG(LogOutlawsCoop, Warning, TEXT("OLCOOP the other player's sprite could not be made (%s: %s); tried again every 2 s"), *Want,
				!Atlas ? TEXT("not in the game files") : Atlas->Chors.Num() == 0 ? TEXT("no choreographies") : TEXT("the sprite quad or material missing"));
		}
		return nullptr;
	}
	// (R4) standing on the frames' insertion points, down and across, as the bad guys do
	S->SetFeet(Atlas->BelowPx, Atlas->FootPx, Atlas->CellH);
	S->SetFeetAcross(Atlas->SidePx2, Atlas->CellW);
	// the choreographies the puppet shows, as this build reads them (directions x frames a one-shot shows, at its rate)
	FString Used;
	for (const int32 c : { 0, 1, 2, 3, 4, 10, 15, 20, 21 })
	{
		if (!Atlas->Chors.IsValidIndex(c)) { Used += FString::Printf(TEXT(" #%d missing"), c); continue; }
		const OutlawsTextures::FSpriteAtlas::FChor& Ch = Atlas->Chors[c];
		Used += FString::Printf(TEXT(" #%d %dx%d@%.0f"), c, Ch.Dirs, Ch.Length, Ch.FPS);
	}
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the other player drawn as %s (%s: %d chors, cells %dx%d px, %s look):%s"), *PuppetCharacterName(Want), *Want, Atlas->Chors.Num(), Atlas->CellW, Atlas->CellH,
		S->SpriteTextureLo && S->SpriteTexture != S->SpriteTextureLo ? TEXT("the remaster's HD") : TEXT("the original"), *Used);
	return S;
}

void UOutlawsCoop::WarmPuppetSprite(AOutlawsLevelActor* L)
{
	// built with the level (as the bad guys' are): the HD atlas is a render target drawn once, a hitch if first asked mid-play
	const TSharedPtr<OutlawsTextures::FSpriteAtlas> Atlas = L ? L->GetAtlas(PuppetNwx, true) : nullptr;
	if (Atlas && L->bHDTextures) L->SpriteHdFor(Atlas);
}

// ---- its test (-OLCoopAuto -OLCoopTest=puppet) ---------------------------------------------------------------------------

#if WITH_OUTLAWS_TESTS
bool UOutlawsCoop::PuppetTestStandOff(AOutlawsPlayer* P, double Dist)
{
	// this (the host's) player set down Dist from the other's feet, on the same floor, nothing between its eyes and the body
	const AOutlawsRemotePlayer* R = PartnerPuppet();
	UWorld* W = P ? P->GetWorld() : nullptr;
	if (!R || !W) return false;
	const FVector Feet = R->ShownBody() - FVector(0.0, 0.0, R->ShownHalf());
	const float Half = P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), Rad = P->GetCapsuleComponent()->GetScaledCapsuleRadius();
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsCoopTestPuppet), false, P);
	for (const double D : { Dist, Dist * 0.75, Dist * 0.55 })
	{
		for (int32 k = 0; k < 8; ++k)
		{
			const FVector Way = FRotator(0.0, 45.0 * k, 0.0).Vector();
			const FVector At = Feet + Way * D + FVector(0.0, 0.0, Half + 3.0);
			// room round the spot (no wall at the view's edge), and the whole body in sight: its middle, its feet, both sides
			if (W->OverlapBlockingTestByChannel(At, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(Rad + 50.f, Half), Q)) continue;
			const FVector Eye = At + FVector(0.0, 0.0, 60.0), Side(-Way.Y * 80.0, Way.X * 80.0, 0.0);
			FHitResult Hit;
			bool bHidden = false;
			for (const FVector& To : { Feet + FVector(0.0, 0.0, 100.0), Feet + FVector(0.0, 0.0, 20.0), Feet + FVector(0.0, 0.0, 100.0) + Side, Feet + FVector(0.0, 0.0, 100.0) - Side })
			{
				bHidden = bHidden || W->LineTraceSingleByChannel(Hit, Eye, To, ECC_Visibility, Q);
			}
			if (bHidden) continue;
			if (!W->LineTraceSingleByChannel(Hit, At, At - FVector(0.0, 0.0, Half + 80.0), ECC_Visibility, Q) || FMath::Abs(Hit.ImpactPoint.Z - Feet.Z) > 60.0) continue;
			P->TeleportTo(At, FRotator(0.0, (Feet - At).Rotation().Yaw, 0.0), false, true);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host stands %.0f cm from the client's body to watch it"), D);
			return true;
		}
	}
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host found no free spot %.0f cm from the client's body (stays where it is)"), Dist);
	return false;
}

void UOutlawsCoop::TickHostPuppetTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	AOutlawsRemotePlayer* R = PartnerPuppet();
	// the host's eyes on the client's sprite (its middle; crouched lower, lying lower still) while it stands still; held still
	// while it moves, so the photographs are not smeared by a turning view
	if (R && Now < PuppetAimUntil)
	{
		const UCameraComponent* Cam = P->Camera.Get();
		const FVector Eye = Cam ? Cam->GetComponentLocation() : P->GetActorLocation();
		const TCHAR* State = R->ShownState();
		const double Up = FCString::Strcmp(State, TEXT("die")) == 0 ? 25.0 : FCString::Strncmp(State, TEXT("crouch"), 6) == 0 ? 55.0 : 95.0;
		const FVector Target = R->ShownBody() - FVector(0.0, 0.0, R->ShownHalf() - Up);
		if (APlayerController* PC = Cast<APlayerController>(P->GetController())) PC->SetControlRotation((Target - Eye).Rotation());
	}
	auto Aim = [this, Now](double Seconds) { PuppetAimUntil = Now + Seconds; };
	// the way the client is to face, worked out here (the client's own copy of the host's place can lag seconds behind right
	// after a level start, while the start's replication drains): 0 toward the host, 1 across the host's view on the side
	// with more room, 2 back the way of the last 1
	auto FaceClient = [this, R, P](int32 How)
	{
		if (!R) return;
		const FVector Body = R->ShownBody();
		const FVector Me = P->GetActorLocation();
		float Yaw = float(FMath::RadiansToDegrees(FMath::Atan2(Me.Y - Body.Y, Me.X - Body.X)));
		if (How == 1)
		{
			UWorld* W = P->GetWorld();
			auto Room = [W, P, &Body](float Way)
			{
				FHitResult Hit;
				FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsCoopTestRoom), false, P);
				return W && W->LineTraceSingleByChannel(Hit, Body, Body + FRotator(0.0, Way, 0.0).Vector() * 800.0, ECC_Visibility, Q) ? double(Hit.Distance) : 800.0;
			};
			Yaw = Room(Yaw + 90.f) >= Room(Yaw - 90.f) ? Yaw + 90.f : Yaw - 90.f;
			PuppetTestAcross = Yaw;
		}
		else if (How == 2)
		{
			Yaw = PuppetTestAcross + 180.f;
		}
		TestSay(TEXT("p:yaw"), FMath::RoundToInt(FRotator::NormalizeAxis(Yaw)));
	};
	// a step that waits for the puppet to show a choreography (TestAt: how long it may take), then photographs it
	auto See = [this, R, Now](const TCHAR* Want, const TCHAR* ShotName)
	{
		const bool bNow = R && FCString::Strcmp(R->ShownState(), Want) == 0;
		if (!bNow && Now < TestAt) return false;
		if (bNow) ++PuppetTestSeen;
		else ++PuppetTestMissed;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: puppet %s: shows %s (chor %d)%s"), ShotName ? ShotName : Want, R ? R->ShownState() : TEXT("nothing"), R ? R->ShownChor() : -1,
			bNow ? TEXT(", as wanted") : *FString::Printf(TEXT(", NOT %s"), Want));
		if (ShotName) Shot(FString(TEXT("ol_puppet_")) + ShotName);
		return true;
	};
	switch (TestStep)
	{
	case 190:
		// both out of the bad guys' reach; the host 3.5 m from the client
		L->bInvincible = true;
		TestSay(TEXT("godmode"), 1);
		TestSay(TEXT("p:reset"), 0);
		PuppetTestStandOff(P, 350.0);
		Aim(5.0);
		TestStep = 191;
		TestAt = Now + 1.0;
		return;
	case 191:
		// 1: standing, facing the host (its front)
		if (Now < TestAt) return;
		FaceClient(0);
		TestStep = 192;
		TestAt = Now + 2.0;
		return;
	case 192:
		if (Now < TestAt) return;
		TestStep = 193;
		TestAt = Now + 2.0;
		return;
	case 193:
		if (!See(TEXT("stand"), TEXT("stand"))) return;
		// 2: walking across the host's view (the view held still), then stopped
		Aim(0.0);
		FaceClient(1);
		TestSay(TEXT("p:walk"), 1000);
		TestStep = 194;
		TestAt = Now + 3.0;
		return;
	case 194:
		if (!See(TEXT("walk"), TEXT("walk"))) return;
		TestStep = 195;
		TestAt = Now + 1.5;
		return;
	case 195:
		if (Now < TestAt) return;
		Aim(1.0);
		TestStep = 196;
		TestAt = Now + 1.2;
		return;
	case 196:
		if (Now < TestAt) return;
		TestStep = 197;
		TestAt = Now + 2.0;
		return;
	case 197:
		if (!See(TEXT("stand"), TEXT("stop"))) return;
		// 3: running back the other way
		FaceClient(2);
		TestSay(TEXT("p:run"), 800);
		TestStep = 198;
		TestAt = Now + 3.0;
		return;
	case 198:
		if (!See(TEXT("run"), TEXT("run"))) return;
		TestStep = 199;
		TestAt = Now + 1.5;
		return;
	case 199:
		// 4: crouched (the host 3.5 m off again first), then crouch-walking
		if (Now < TestAt) return;
		TestSay(TEXT("p:runoff"), 0);
		PuppetTestStandOff(P, 350.0);
		Aim(1.5);
		TestStep = 200;
		TestAt = Now + 1.5;
		return;
	case 200:
		if (Now < TestAt) return;
		TestSay(TEXT("p:crouch"), 1);
		Aim(1.0);
		TestStep = 201;
		TestAt = Now + 1.2;
		return;
	case 201:
		if (Now < TestAt) return;
		TestStep = 202;
		TestAt = Now + 2.0;
		return;
	case 202:
		if (!See(TEXT("crouch"), TEXT("crouch"))) return;
		FaceClient(1);
		TestSay(TEXT("p:crouchwalk"), 1500);
		TestStep = 203;
		TestAt = Now + 3.0;
		return;
	case 203:
		if (!See(TEXT("crouch-walk"), TEXT("crouchwalk"))) return;
		TestStep = 204;
		TestAt = Now + 2.0;
		return;
	case 204:
		if (Now < TestAt) return;
		TestSay(TEXT("p:crouch"), 0);
		TestStep = 205;
		TestAt = Now + 3.0;
		return;
	case 205:
		// 5: a shot, seen from the side (the arm out); then two fanned (a quarter second apart), each from the chor's start
		if (!See(TEXT("stand"), nullptr)) return;
		PuppetTestStandOff(P, 350.0);
		Aim(1.5);
		TestStep = 206;
		TestAt = Now + 0.5;
		return;
	case 206:
		if (Now < TestAt) return;
		FaceClient(1);
		TestStep = 207;
		TestAt = Now + 1.5;
		return;
	case 207:
		if (Now < TestAt) return;
		TestSay(TEXT("p:fire"), 0);
		TestStep = 208;
		TestAt = Now + 3.0;
		return;
	case 208:
		if (!See(TEXT("shoot"), TEXT("shoot"))) return;
		TestStep = 209;
		TestAt = Now + 0.8;
		return;
	case 209:
		if (Now < TestAt) return;
		TestSay(TEXT("p:fire"), 1);
		TestStep = 210;
		TestAt = Now + 0.25;
		return;
	case 210:
		if (Now < TestAt) return;
		TestSay(TEXT("p:fire"), 1);
		TestStep = 221;                      // (221: the wait after the shots; 211 on goes on with the throw)
		TestAt = Now + 2.0;
		return;
	case 221:
		if (Now < TestAt) return;
		TestStep = 211;
		TestAt = Now + 3.0;
		return;
	case 211:
	{
		// 6: hurt (its flinch): the host's hurt, with the client mortal for that moment
		if (!See(TEXT("stand"), nullptr)) return;
		TestSay(TEXT("godmode"), 0);
		const FVector From = P->GetActorLocation();
		HurtOther(1.0f, 1.f, false, &From);
		TestSay(TEXT("godmode"), 1);
		TestStep = 212;
		TestAt = Now + 3.0;
		return;
	}
	case 212:
		if (!See(TEXT("hit"), TEXT("hit"))) return;
		TestStep = 213;
		TestAt = Now + 1.5;
		return;
	case 213:
		// 7: dead: the fall, then lying where it fell
		if (Now < TestAt) return;
		TestSay(TEXT("die"), 0);
		TestStep = 214;
		TestAt = Now + 4.0;
		return;
	case 214:
		if (!See(TEXT("die"), nullptr)) return;
		Aim(1.2);
		TestStep = 215;
		TestAt = Now + 1.5;
		return;
	case 215:
	{
		if (Now < TestAt) return;
		See(TEXT("die"), TEXT("dead"));
		// 8: revived by the host's bag (the grip at the feet, as a hand's), back on its feet
		L->Counts.Remove(TEXT("idocbag"));
		L->AddCount(TEXT("idocbag"), 6);
		const FVector Feet = R ? R->ShownBody() - FVector(0.0, 0.0, R->ShownHalf()) : P->GetActorLocation();
		const bool bTook = TryRevive(P, Feet + FVector(0.0, 0.0, 25.0));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host grips the client's body: %s"), bTook ? TEXT("the grip took it") : TEXT("NOT TAKEN"));
		TestStep = 216;
		TestAt = Now + 4.0;
		return;
	}
	case 216:
		if (!See(TEXT("stand"), nullptr)) return;
		Aim(1.0);
		TestStep = 217;
		TestAt = Now + 1.0;
		return;
	case 217:
		if (Now < TestAt) return;
		See(TEXT("stand"), TEXT("revived"));
		// 9: a knife thrown (the jab chor, the arm out; last, as the throw's step gives the client every weapon, which a death
		// would drop round the body)
		TestSay(TEXT("v:throw"), 7);
		TestStep = 218;
		TestAt = Now + 3.0;
		return;
	case 218:
		if (!See(TEXT("throw"), TEXT("throw"))) return;
		TestStep = 219;
		TestAt = Now + 1.5;
		return;
	case 219:
		if (Now < TestAt) return;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: puppet states seen %d, missed %d; chors shown here: %s"), PuppetTestSeen, PuppetTestMissed, R ? *R->ChorsSeen() : TEXT("no puppet"));
		TestSay(TEXT("p:reset"), 0);
		TestSay(TEXT("quit"), 0);
		TestStep = 100;
		TestAt = Now + 2.0;
		return;
	default:
		return;
	}
}

void UOutlawsCoop::OnPuppetTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	// the client's own body moved as its controls would: turned to the yaw the host worked out, walking, running, crouching
	// and shooting by the player's own test hooks
	if (Step == TEXT("reset"))
	{
		P->HoldMoveStick(FVector2D::ZeroVector, 0.f);
		P->CoopTestRun(false);
		P->SetCrouchHeld(false);
	}
	else if (Step == TEXT("yaw"))
	{
		if (APlayerController* PC = Cast<APlayerController>(P->GetController())) PC->SetControlRotation(FRotator(0.0, double(Arg), 0.0));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client faces yaw %d"), Arg);
	}
	else if (Step == TEXT("walk") || Step == TEXT("run") || Step == TEXT("crouchwalk"))
	{
		P->CoopTestRun(Step == TEXT("run"));
		P->HoldMoveStick(FVector2D(0.0, 1.0), Arg / 1000.f);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client %s ahead for %.1f s"), Step == TEXT("run") ? TEXT("runs") : Step == TEXT("walk") ? TEXT("walks") : TEXT("crouch-walks"), Arg / 1000.f);
	}
	else if (Step == TEXT("runoff"))
	{
		P->CoopTestRun(false);
	}
	else if (Step == TEXT("crouch"))
	{
		P->SetCrouchHeld(Arg != 0);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client %s"), Arg != 0 ? TEXT("crouches") : TEXT("stands up"));
	}
	else if (Step == TEXT("fire"))
	{
		// (Arg 1: fanned, as the free hand fans the hammer: a fifth of a second between shots, not the aimed half second)
		P->CoopTestFire(Arg == 1);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client fires its revolver%s (bullets left %d)"), Arg == 1 ? TEXT(", fanned") : TEXT(""), L->GetCount(TEXT("ibullit")));
	}
}
#endif // WITH_OUTLAWS_TESTS
