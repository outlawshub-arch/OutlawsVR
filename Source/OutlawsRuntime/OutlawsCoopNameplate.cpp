// Story co-op (tools/olue/COOP_DESIGN.md): the options' Multiplayer page's player name in a session (user 2026-10-02: "I'd
// like in co op for the player name to appear above the persons head if they fill this, and we should also be able to see
// the other players health like a bar under that name").
// - Each machine sends its player's name (AOutlawsLevelActor::MpPlayerName) through its link when the lobby is up and again
//   whenever the player changes it in the menu; the host keeps it on the link, which replicates it to the other machine.
// - The other machine's puppet (AOutlawsRemotePlayer) draws the name over the head (only when one was set), the health bar
//   under it from the stats the links already carry (FOutlawsCoopStats: Health / MaxHealth, death), and a speaker beside
//   the name while the voice chat says that player's voice is coming through.
// - Its test: -OLCoopAuto -OLCoopTest=nameplate, each instance with -OLMpName=<name>.
// None of this runs in single player.
#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsRemotePlayer.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/PlayerController.h"

void AOutlawsCoopLink::ServerPlayerName_Implementation(const FString& InName)
{
	if (!Admit(EOLGate::Name, InName.Len() <= 64)) return;
	PlayerName = AOutlawsLevelActor::CleanMpName(InName);
	++NameSerial;
	ForceNetUpdate();
}

void UOutlawsCoop::TickName()
{
	// this player's name out when it is first known and whenever it changes (the menu's keyboard sets it on the level actor)
	AOutlawsLevelActor* L = FindLevel();
	if (!L || !L->HasActorBegunPlay()) return;           // (its settings, the name among them, are read at its BeginPlay)
	if (AOutlawsCoopLink* MyLink = GetLink(MyIndex()); MyLink && (!bNameSent || L->MpPlayerName != SentName))
	{
		bNameSent = true;
		SentName = L->MpPlayerName;
		MyLink->ServerPlayerName(SentName);           // (the host's own: set here at once)
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP this player's name sent: %s"), SentName.IsEmpty() ? TEXT("(none)") : *FString::Printf(TEXT("'%s'"), *SentName));
	}
	// the others', said once each time each comes (N1: every other player's, over each one's own puppet)
	TArray<AOutlawsCoopLink*> Links;
	GetLinks(Links);
	// (REQ-MPLOBBY c: a name or a microphone that came, this player's own among them, redraws the lobby and the Players page; they showed
	// "Player 2 (you)" until something else changed)
	uint32 Names = 0;
	for (const AOutlawsCoopLink* OL : Links) Names = Names * 31u + uint32(OL->NameSerial) + uint32(OL->PlayerIndex + 1) * 7u + (OL->bMic ? 3u : 0u);
	if (Names != NamesSeen)
	{
		NamesSeen = Names;
		++Version;
	}
	for (const AOutlawsCoopLink* OL : Links)
	{
		const int32 Who = OL->PlayerIndex;
		if (Who == MyIndex() || Who < 0 || Who >= MaxPlayers || OL->NameSerial == 0 || OL->NameSerial == LoggedNameSerial[Who]) continue;
		LoggedNameSerial[Who] = OL->NameSerial;
		if (IsClient() && !bAuto && Who == 0 && !OL->PlayerName.IsEmpty())
		{
			// (R8: the join page's Recent list names the host it joined)
			NoteRecent(ConnectingPort.IsEmpty() || ConnectingPort == FString::FromInt(Port) ? ConnectingTo : ConnectingTo + TEXT(":") + ConnectingPort, OL->PlayerName);
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP the other player's name (the %s's): %s"), WhoWord(Who),
			OL->PlayerName.IsEmpty() ? TEXT("(none: the health bar alone over their head)") : *FString::Printf(TEXT("'%s'"), *OL->PlayerName));
	}
}

// ---- its test (-OLCoopAuto -OLCoopTest=nameplate) ----------------------------------------------------------------------

void UOutlawsCoop::NameplateCheck(bool bOk, const FString& What)
{
	++NameplateChecks;
	if (!bOk) ++NameplateFails;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: nameplate check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
}

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TickHostNameplateTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	AOutlawsRemotePlayer* R = PartnerPuppet();
	const AOutlawsCoopLink* CL = GetLink(PartnerIndex());
	// the host's eyes on the client's plate (over its head) while the test runs
	if (R && Now < PuppetAimUntil)
	{
		const UCameraComponent* Cam = P->Camera.Get();
		const FVector Eye = Cam ? Cam->GetComponentLocation() : P->GetActorLocation();
		const double Up = R->PlateDead() ? 40.0 : 150.0;
		const FVector Target = R->ShownBody() - FVector(0.0, 0.0, R->ShownHalf() - Up);
		if (APlayerController* PC = Cast<APlayerController>(P->GetController())) PC->SetControlRotation((Target - Eye).Rotation());
	}
	if (R && TestStep >= 260 && TestStep < 273) R->bTestTalk = true;     // (the speaker forced on until step 8 lets it go)
	auto Aim = [this, Now](double Seconds) { PuppetAimUntil = Now + Seconds; };
	auto State = [R]() { return R ? R->PlateState() : FString(TEXT("no puppet")); };
	auto Health = [CL]() { return CL ? FString::Printf(TEXT("%.1f of %.0f"), CL->Stats.Health, CL->Stats.MaxHealth) : FString(TEXT("?")); };
	// the client's health brought down a point at a time (it mortal for each hurt) until the bar shows Band
	auto HurtTo = [&](int32 Band, int32 NextStep, const TCHAR* ShotName)
	{
		if (R && R->PlateBand() == Band && !R->PlateDead())
		{
			const int32 Want = AOutlawsRemotePlayer::HealthBand(R->PlateFraction());
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: nameplate after the hurts: %s; the client's health here %s"), *State(), *Health());
			NameplateCheck(Want == Band && R->PlateBand() == Band, FString::Printf(TEXT("the bar at %d %% is %s (wanted %s)"), FMath::RoundToInt(R->PlateFraction() * 100.f),
				AOutlawsRemotePlayer::BandName(R->PlateBand()), AOutlawsRemotePlayer::BandName(Band)));
			if (ShotName) Shot(ShotName);
			TestStep = NextStep;
			TestAt = Now + 1.0;
			NameplateTries = 0;
			return;
		}
		if (Now < TestAt) return;
		if (++NameplateTries > 14)
		{
			NameplateCheck(false, FString::Printf(TEXT("the bar never turned %s: %s"), AOutlawsRemotePlayer::BandName(Band), *State()));
			TestStep = NextStep;
			TestAt = Now;
			NameplateTries = 0;
			return;
		}
		TestSay(TEXT("godmode"), 0);
		const FVector From = P->GetActorLocation();
		HurtOther(1.0f, 1.f, true, &From);
		TestSay(TEXT("godmode"), 1);
		TestAt = Now + 0.8;
	};
	switch (TestStep)
	{
	case 260:
		// both out of the bad guys' reach; the host 3.5 m from the client, the client facing it; the speaker forced on
		L->bInvincible = true;
		TestSay(TEXT("godmode"), 1);
		TestSay(TEXT("p:reset"), 0);
		PuppetTestStandOff(P, 350.0);
		Aim(60.0);
		TestStep = 261;
		TestAt = Now + 1.0;
		return;
	case 261:
	{
		if (Now < TestAt) return;
		if (R)
		{
			const FVector Body = R->ShownBody(), Me = P->GetActorLocation();
			TestSay(TEXT("p:yaw"), FMath::RoundToInt(FMath::RadiansToDegrees(FMath::Atan2(Me.Y - Body.Y, Me.X - Body.X))));
		}
		TestStep = 262;
		TestAt = Now + 3.0;
		return;
	}
	case 262:
	{
		// 1: the client's name and a full green bar, the speaker beside the name
		if (Now < TestAt) return;
		NameplateFirstName = CL ? CL->PlayerName : FString();
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: nameplate at the start: %s; the client's name on its link '%s', health here %s"), *State(), *NameplateFirstName, *Health());
		NameplateFirstName = AOutlawsRemotePlayer::PlateLabel(NameplateFirstName, CL ? CL->Character : FString());
		NameplateCheck(R && !NameplateFirstName.IsEmpty() && R->PlateName() == NameplateFirstName, FString::Printf(TEXT("the client's name '%s' over its head (shown '%s')"),
			*NameplateFirstName, R ? *R->PlateName() : TEXT("?")));
		NameplateCheck(R && R->PlateBand() == 0 && R->PlateFraction() > 0.99f, TEXT("a full green bar"));
		NameplateCheck(R && R->PlateTalkShown(), TEXT("the speaker shown while talking (forced on)"));
		Shot(TEXT("ol_nameplate_host"));
		TestStep = 263;
		TestAt = Now + 1.0;
		return;
	}
	case 263:
		// 2: hurt to the middle: orange
		HurtTo(1, 264, TEXT("ol_nameplate_hurt"));
		return;
	case 264:
		// 3: hurt low: red
		HurtTo(2, 265, TEXT("ol_nameplate_low"));
		return;
	case 265:
		// 4: dead: an empty red bar, low over the body
		if (Now < TestAt) return;
		TestSay(TEXT("die"), 0);
		TestStep = 266;
		TestAt = Now + 4.0;
		return;
	case 266:
		if (!(R && R->PlateDead()) && Now < TestAt) return;
		TestStep = 267;
		TestAt = Now + 1.5;
		return;
	case 267:
	{
		if (Now < TestAt) return;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: nameplate dead: %s; the client's health here %s"), *State(), *Health());
		NameplateCheck(R && R->PlateDead() && R->PlateBand() == 2 && R->PlateFraction() <= 0.f, TEXT("dead: an empty red bar"));
		Shot(TEXT("ol_nameplate_dead"));
		// 5: revived by the host's bag (half health at most): the bar again
		L->Counts.Remove(TEXT("idocbag"));
		L->AddCount(TEXT("idocbag"), 6);
		const FVector Feet = R ? R->ShownBody() - FVector(0.0, 0.0, R->ShownHalf()) : P->GetActorLocation();
		const bool bTook = TryRevive(P, Feet + FVector(0.0, 0.0, 25.0));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host grips the client's body: %s"), bTook ? TEXT("the grip took it") : TEXT("NOT TAKEN"));
		TestStep = 268;
		TestAt = Now + 5.0;
		return;
	}
	case 268:
		if (R && R->PlateDead() && Now < TestAt) return;
		TestStep = 269;
		TestAt = Now + 1.5;
		return;
	case 269:
	{
		if (Now < TestAt) return;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: nameplate revived: %s; the client's health here %s"), *State(), *Health());
		const float F = R ? R->PlateFraction() : -1.f;
		NameplateCheck(R && !R->PlateDead() && F > 0.f && R->PlateBand() == AOutlawsRemotePlayer::HealthBand(F), FString::Printf(TEXT("revived: the bar at %d %%, %s"),
			FMath::RoundToInt(F * 100.f), R ? AOutlawsRemotePlayer::BandName(R->PlateBand()) : TEXT("?")));
		// 6: the client changes its name mid-session
		TestSay(TEXT("n:name"), 1);
		TestStep = 270;
		TestAt = Now + 4.0;
		return;
	}
	case 270:
	{
		const FString Want = AOutlawsRemotePlayer::PlateLabel(TEXT("Deputy Two"), CL ? CL->Character : FString());
		if (!(R && R->PlateName() == Want) && Now < TestAt) return;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: nameplate after the client's new name: %s"), *State());
		NameplateCheck(R && R->PlateName() == Want, FString::Printf(TEXT("the name changed mid-session ('%s')"), *Want));
		// 7: the client clears its name: the bar alone (the speaker beside it)
		TestSay(TEXT("n:name"), 2);
		TestStep = 271;
		TestAt = Now + 4.0;
		return;
	}
	case 271:
		if (!(R && R->PlateName().IsEmpty()) && Now < TestAt) return;
		TestStep = 272;
		TestAt = Now + 1.0;
		return;
	case 272:
		if (Now < TestAt) return;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: nameplate with no name: %s"), *State());
		NameplateCheck(R && R->PlateName().IsEmpty() && R->PlateTalkShown(), TEXT("no name: no name line, the bar and the speaker alone"));
		Shot(TEXT("ol_nameplate_noname"));
		// 8: the talk stops: the speaker held a moment (no flicker), then gone
		if (R) R->bTestTalk = false;
		TestStep = 273;
		TestAt = Now + 0.12;
		return;
	case 273:
		if (Now < TestAt) return;
		NameplateCheck(R && R->PlateTalkShown(), TEXT("the speaker still shown 0.12 s after the talk stopped (the hold)"));
		TestStep = 274;
		TestAt = Now + 0.6;
		return;
	case 274:
		if (Now < TestAt) return;
		NameplateCheck(R && !R->PlateTalkShown(), TEXT("the speaker gone 0.7 s after the talk stopped"));
		// the client's name back, as given at its launch
		TestSay(TEXT("n:name"), 0);
		TestStep = 275;
		TestAt = Now + 3.0;
		return;
	case 275:
		if (!(R && R->PlateName() == NameplateFirstName) && Now < TestAt) return;
		NameplateCheck(R && R->PlateName() == NameplateFirstName, FString::Printf(TEXT("the launch name back ('%s')"), *NameplateFirstName));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: nameplate checks %d, failed %d"), NameplateChecks, NameplateFails);
		TestSay(TEXT("p:reset"), 0);
		TestSay(TEXT("quit"), 0);
		Aim(0.0);
		TestStep = 100;
		TestAt = Now + 2.0;
		return;
	default:
		return;
	}
}

void UOutlawsCoop::OnNameplateTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	// the client's name changed as the menu's keyboard changes it (never saved: the test's own): 1 a new one, 2 none, 0 the
	// launch name (-OLMpName) again
	if (Step == TEXT("name"))
	{
		static FString LaunchName;
		if (LaunchName.IsEmpty() && !L->MpPlayerName.IsEmpty() && L->MpPlayerName != TEXT("Deputy Two")) LaunchName = L->MpPlayerName;
		L->MpPlayerName = Arg == 1 ? FString(TEXT("Deputy Two")) : Arg == 2 ? FString() : LaunchName;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the client's name is now %s"), L->MpPlayerName.IsEmpty() ? TEXT("(none)") : *FString::Printf(TEXT("'%s'"), *L->MpPlayerName));
	}
}
#endif // WITH_OUTLAWS_TESTS
