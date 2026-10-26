// REQ-KILLFEED (dotard and the user, 12 Oct): who killed whom, in a match.  The host decides each death where it already
// credits the kill (OutlawsCoopMatch.cpp, TickMatch: whoever struck last within 10 s, and whether that strike was dynamite;
// nobody's kill by the victim's own cause, a fall or its own stick, which its machine sends up with its stats) and tells
// every machine the killer, the victim and the kind (ClientMatchKill).  Each machine words it for its own player from its
// local.msg (OutlawsKillFeed.h: the original's death messages, 20040..20062, 41..62 and 823) and shows it in the kill feed
// above the notices (AOutlawsPlayer::ShowKillLine).  Bots die and kill through the same path.
//
// The test (-OLKillFeedTest on the host and its client; the host with -OLMpHost=<map> -OLMpBots=1): the bots hold their fire,
// and the host deals six deaths: its gun on the bot, the bot's gun on the host, its dynamite on the client, the client's own
// stick, the host's fall and the client's plain death.  Each machine checks the line it shows ("OLSHOT kf check PASS|FAIL")
// and photographs its feed (ol_kf_host.png, ol_kf_client.png).
#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsRemotePlayer.h"
#include "OutlawsMpHost.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"

void UOutlawsCoop::SayKill(int32 Killer, int32 Victim, EOutlawsObit Kind)
{
	if (!IsHost()) return;
	auto NameOf = [this](int32 Who)
	{
		const FOutlawsDmPlayer* P = Dm.Find(Who);
		if (P && !P->Name.IsEmpty()) return P->Name;
		const AOutlawsCoopLink* K = GetLink(Who);
		return K && !K->PlayerName.IsEmpty() ? K->PlayerName : Who == 0 ? FString(TEXT("Host")) : FString::Printf(TEXT("Player %d"), Who + 1);
	};
	const FString KillerName = NameOf(Killer), VictimName = NameOf(Victim);
	OnMatchKill(Killer, Victim, uint8(Kind), KillerName, VictimName);
	ForClients([&](AOutlawsCoopLink& K) { if (!K.bBot) K.ClientMatchKill(Killer, Victim, uint8(Kind), KillerName, VictimName); });
}

void UOutlawsCoop::OnMatchKill(int32 Killer, int32 Victim, uint8 Kind, const FString& KillerName, const FString& VictimName)
{
	const AOutlawsLevelActor* L = FindLevel();
	const int32 Me = MyIndex();
	const bool bMine = Me >= 0 && (Killer == Me || Victim == Me);
	const FString Line = OutlawsKillFeed::Line(static_cast<EOutlawsObit>(Kind), KillerName, VictimName, Me >= 0 && Killer == Me && Killer != Victim, Me >= 0 && Victim == Me,
		[L](int32 Id, const TCHAR* Fallback) { return L ? L->UserMessageOr(Id, Fallback) : FString(Fallback); });
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLKILLFEED shown (%s): %s  [the %s, %s, the %s]"), IsHost() ? TEXT("host") : WhoWord(Me), *Line, WhoWord(FMath::Clamp(Killer, 0, MaxPlayers - 1)),
		OutlawsKillFeed::Name(static_cast<EOutlawsObit>(Kind)), WhoWord(FMath::Clamp(Victim, 0, MaxPlayers - 1)));
	KillLinesSeen.Add(Line);
	AOutlawsPlayer* P = LocalPlayer();
	if (P) P->ShowKillLine(Line, bMine);
	else UE_LOG(LogOutlawsCoop, Warning, TEXT("OLKILLFEED no player here to show it"));
}

// ---- the test --------------------------------------------------------------------------------------------------------------

#if WITH_OUTLAWS_TESTS
bool UOutlawsCoop::KillFeedTestCheck(const FString& Want)
{
	const bool bOk = KillLinesSeen.Contains(Want);
	UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT kf check %s: the %s shows \"%s\"%s"), bOk ? TEXT("PASS") : TEXT("FAIL"), IsHost() ? TEXT("host") : WhoWord(MyIndex()), *Want,
		bOk ? TEXT("") : *FString::Printf(TEXT(" (shown: %s)"), *FString::Join(KillLinesSeen, TEXT(" | "))));
	if (!bOk) ++KfFails;
	return bOk;
}

void UOutlawsCoop::TickHostKillFeedTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	FOutlawsMatchFlow& F = OutlawsMpHost::Flow();
	TArray<AOutlawsCoopLink*> Links;
	GetLinks(Links);
	if (TestStep < 900) { TestStep = 900; TestAt = 0.0; }
	auto NameOf = [this](int32 Who) { const FOutlawsDmPlayer* D = Dm.Find(Who); return D ? D->Name : FString(TEXT("?")); };
	auto BodyAt = [this](int32 Who) { const AOutlawsCoopLink* K = GetLink(Who); return K ? FVector(K->Pose.Body) : FVector::ZeroVector; };
	auto Alive = [this](int32 Who) { return Who >= 0 && MatchInRules[Who] && !MatchDead[Who]; };
	// each death: its lines checked here, and on the client by what the host expects it to read
	auto Expect = [this](const FString& Here, const FString& There)
	{
		KillFeedTestCheck(Here);
		TestSayTo(KfClient, TEXT("kf:expect:") + There, 0);
	};
	const FString Host = NameOf(0);
	switch (TestStep)
	{
	case 900:
		// the client and a bot in the rules, a few seconds of the warm-up, then the host's Start
		for (const AOutlawsCoopLink* K : Links)
		{
			const int32 Who = K->PlayerIndex;
			if (Who <= 0 || Who >= MaxPlayers || !MatchInRules[Who]) continue;
			if (K->bBot && KfBot < 0) KfBot = Who;
			if (!K->bBot && KfClient < 0) KfClient = Who;
		}
		if (KfBot < 0 || KfClient < 0 || !bMatchSetUp) return;
		if (TestAt == 0.0) { TestAt = Now + 6.0; return; }
		if (Now < TestAt) return;
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT kf: the host %s, the client %s (the %s), the bot %s (the %s): Start"), *Host, *NameOf(KfClient), WhoWord(KfClient), *NameOf(KfBot), WhoWord(KfBot));
		F.HostStart();
		TestStep = 901;
		return;
	case 901:
		if (F.GetState() != FOutlawsMatchFlow::EState::Playing) return;
		TestStep = 910;
		TestAt = Now + 5.0;                      // (past the spawn protection)
		KfTries = 0;
		return;

	// 1. the host's gun kills the bot: "You killed <bot>!" here, "<host> killed <bot>!" there
	case 910:
		if (Now < TestAt) return;
		if (MatchDead[KfBot])
		{
			Expect(FString::Printf(TEXT("You killed %s!"), *NameOf(KfBot)), FString::Printf(TEXT("%s killed %s!"), *Host, *NameOf(KfBot)));
			TestStep = 920;
			TestAt = Now + 1.0;
			KfTries = 0;
			return;
		}
		if (++KfTries > 12) { KillFeedTestCheck(TEXT("(the bot never died to the host's gun)")); TestStep = 920; KfTries = 0; return; }
		OnMatchHit(0, LevelEpoch(), KfBot, 50.f, BodyAt(KfBot), P->GetActorLocation());
		TestAt = Now + 0.6;
		return;

	// 2. the bot's gun kills the host: "<bot> killed you!" here, "<bot> killed <host>!" there
	case 920:
		if (Now < TestAt) return;
		if (KfTries == 0 && !(Alive(KfBot) && Alive(0) && !L->bDead)) { TestAt = Now + 0.5; return; }
		if (KfTries == 0) { KfTries = 1; TestAt = Now + 4.0; return; }       // (the bot's spawn protection)
		if (L->bDead || MatchDead[0])
		{
			if (!MatchDead[0]) { TestAt = Now + 0.2; return; }
			TestAt = Now + 1.0;
			TestStep = 921;
			return;
		}
		if (++KfTries > 12) { KillFeedTestCheck(TEXT("(the host never died to the bot's gun)")); TestStep = 930; KfTries = 0; return; }
		OnMatchHit(KfBot, LevelEpoch(), 0, 50.f, P->GetActorLocation(), BodyAt(KfBot));
		TestAt = Now + 0.6;
		return;
	case 921:
		if (Now < TestAt) return;
		Expect(FString::Printf(TEXT("%s killed you!"), *NameOf(KfBot)), FString::Printf(TEXT("%s killed %s!"), *NameOf(KfBot), *Host));
		TestStep = 930;
		KfTries = 0;
		return;

	// 3. the host's dynamite kills the client: "You blew up <client>!" here, "<host> blew you up!" there
	case 930:
		if (Now < TestAt) return;
		if (KfTries == 0 && !(Alive(KfClient) && Alive(0) && !L->bDead)) { TestAt = Now + 0.5; return; }
		if (MatchDead[KfClient])
		{
			TestAt = Now + 1.0;
			TestStep = 931;
			return;
		}
		if (++KfTries > 12) { KillFeedTestCheck(TEXT("(the client never died to the host's dynamite)")); TestStep = 940; KfTries = 0; return; }
		{
			// (BlastAtPlayers: the blast's reach on the bodies drawn here, as a stick going off at the client's feet would)
			const AOutlawsRemotePlayer* R = PuppetOf(KfClient);
			const int32 Struck = R ? BlastAtPlayers(L, R->ShownBody(), L->BlastRadiusCm(), 50.f) : 0;
			UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT kf: the host's blast at the client's feet struck %d"), Struck);
		}
		TestAt = Now + 0.8;
		return;
	case 931:
		if (Now < TestAt) return;
		Expect(FString::Printf(TEXT("You blew up %s!"), *NameOf(KfClient)), FString::Printf(TEXT("%s blew you up!"), *Host));
		TestSayTo(KfClient, TEXT("dm:respawn"), 0);
		TestStep = 940;
		KfTries = 0;
		TestAt = Now + 1.0;
		return;

	// 4. the client's own stick: "<client> self-destructed!" here, "You blew yourself up!" there
	case 940:
		if (Now < TestAt) return;
		if (KfTries == 0 && !Alive(KfClient)) { TestAt = Now + 0.5; return; }
		if (KfTries == 0) { KfTries = 1; TestAt = Now + 2.0; return; }
		if (MatchDead[KfClient])
		{
			TestAt = Now + 1.0;
			TestStep = 941;
			return;
		}
		if (++KfTries > 10) { KillFeedTestCheck(TEXT("(the client never died to its own stick)")); TestStep = 950; KfTries = 0; return; }
		TestSayTo(KfClient, TEXT("kf:selfblast"), 0);
		TestAt = Now + 1.5;
		return;
	case 941:
		if (Now < TestAt) return;
		Expect(FString::Printf(TEXT("%s self-destructed!"), *NameOf(KfClient)), TEXT("You blew yourself up!"));
		TestSayTo(KfClient, TEXT("dm:respawn"), 0);
		TestStep = 950;
		KfTries = 0;
		return;

	// 5. the host falls: "You fell to your death!" here, "<host> died from a fall!" there
	case 950:
		if (Now < TestAt) return;
		if (KfTries == 0 && !(Alive(0) && !L->bDead)) { TestAt = Now + 0.5; return; }
		if (KfTries == 0) { KfTries = 1; TestAt = Now + 3.0; return; }       // (past a respawn's fall grace)
		if (L->bDead)
		{
			if (!MatchDead[0]) { TestAt = Now + 0.2; return; }
			TestAt = Now + 1.0;
			TestStep = 951;
			return;
		}
		if (++KfTries > 10) { KillFeedTestCheck(TEXT("(the host never died of a fall)")); TestStep = 960; KfTries = 0; return; }
		// (a hard landing, as the character movement reports it)
		P->GetCharacterMovement()->Velocity = FVector(0.0, 0.0, -8000.0);
		static_cast<ACharacter*>(P)->Landed(FHitResult());
		TestAt = Now + 0.6;
		return;
	case 951:
		if (Now < TestAt) return;
		Expect(TEXT("You fell to your death!"), FString::Printf(TEXT("%s died from a fall!"), *Host));
		TestStep = 960;
		KfTries = 0;
		return;

	// 6. the client dies of nothing in particular: "<client> died!" here, "You died!" there; both feeds photographed
	case 960:
		if (Now < TestAt) return;
		if (KfTries == 0 && !Alive(KfClient)) { TestAt = Now + 0.5; return; }
		if (KfTries == 0) { KfTries = 1; TestAt = Now + 2.0; return; }
		if (MatchDead[KfClient])
		{
			TestAt = Now + 1.0;
			TestStep = 961;
			return;
		}
		if (++KfTries > 6) { KillFeedTestCheck(TEXT("(the client never died)")); TestStep = 970; KfTries = 0; return; }
		TestSayTo(KfClient, TEXT("kf:die"), 0);
		TestAt = Now + 1.5;
		return;
	case 961:
		if (Now < TestAt) return;
		Expect(FString::Printf(TEXT("%s died!"), *NameOf(KfClient)), TEXT("You died!"));
		Shot(TEXT("ol_kf_host"));
		TestSayTo(KfClient, TEXT("kf:shot"), 0);
		TestStep = 970;
		TestAt = Now + 3.0;
		return;

	case 970:
		if (Now < TestAt) return;
		TestSayTo(KfClient, TEXT("kf:report"), 0);
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT kf host lines: %s"), *FString::Join(KillLinesSeen, TEXT(" | ")));
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT kf host RESULT %s (%d failed)"), KfFails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), KfFails);
		TestStep = 971;
		TestAt = Now + 3.0;
		return;
	case 971:
		if (Now < TestAt) return;
		TestSay(TEXT("quit"), 0);
		TestStep = 990;
		TestAt = Now + 3.0;
		return;
	case 990:
		if (Now < TestAt) return;
		Summary(TEXT("done"));
		QuitForTest();
		TestStep = 991;
		return;
	default:
		return;
	}
}

void UOutlawsCoop::OnKillFeedTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	if (Step.StartsWith(TEXT("expect:")))
	{
		KillFeedTestCheck(Step.Mid(7));
	}
	else if (Step == TEXT("selfblast"))
	{
		// a stick gone off at this player's own feet (its own blast: this machine hurts its own player, its own cause)
		if (L->bDead) return;
		const FVector Feet = P->GetActorLocation() - FVector(0.0, 0.0, P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() - 20.0);
		L->Explode(Feet + P->GetActorForwardVector() * 30.0);
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT kf: the %s's own stick at its feet: health %.1f"), WhoWord(MyIndex()), L->GetHealth());
	}
	else if (Step == TEXT("die"))
	{
		if (!L->bDead) L->DamagePlayer(1e6f, 1.f, true);
	}
	else if (Step == TEXT("shot"))
	{
		Shot(TEXT("ol_kf_client"));
	}
	else if (Step == TEXT("report"))
	{
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT kf client lines: %s"), *FString::Join(KillLinesSeen, TEXT(" | ")));
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT kf client RESULT %s (%d failed)"), KfFails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), KfFails);
	}
}
#endif // WITH_OUTLAWS_TESTS
