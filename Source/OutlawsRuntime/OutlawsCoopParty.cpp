// Story co-op for up to eight players (tools/olue/PLANS.md, N1; user 2026-10-03: "push co-op to 8 players"): the party
// test, -OLCoopAuto -OLCoopTest=party -OLCoopPlayers=<n> on the host and on each of its n - 1 clients.  The host waits in
// the lobby for all of them, starts, and when every client has built the level runs the checks one after another:
// 1. each machine draws every other player (a puppet each);
// 2. one client's shot, blast and voice reach every other client through the host;
// 3. the host's bad guy picks each client in turn when that client stands nearest it;
// 4. one client revives another (the bag-points rule between two clients, through the host);
// 5. a save keeps every client's block;
// 6. everyone dead: the level restarts for all;
// 7. the level won: the next level waits for every player's level end (their movies skipped), not the 20 s cap.
// Each check logs "OLCOOP party check PASS|FAIL: ..." on the machine that can judge it; the checker counts them.
//
// None of this runs outside that test.
#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsEnemy.h"
#include "OutlawsRemotePlayer.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"

namespace
{
	constexpr int32 PartyFirstStep = 600;

	AOutlawsEnemy* PartyEnemyAt(const AOutlawsLevelActor* L, int32 Index)
	{
		const TArray<TWeakObjectPtr<AOutlawsEnemy>>* List = L ? &L->GetEnemies() : nullptr;
		return List && List->IsValidIndex(Index) ? (*List)[Index].Get() : nullptr;
	}

	/** A free spot for this player's body Dist from At (eight ways round it), on a floor; At itself if none. */
	FVector PartySpotNear(AOutlawsPlayer* P, const FVector& At, double Dist, const AActor* Also = nullptr)
	{
		UWorld* W = P ? P->GetWorld() : nullptr;
		if (!W) return At;
		const float Half = P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), Rad = P->GetCapsuleComponent()->GetScaledCapsuleRadius();
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsCoopPartySpot), false, P);
		if (Also) Q.AddIgnoredActor(Also);           // (the bad guy's own body: the search starts inside it)
		for (int32 k = 0; k < 8; ++k)
		{
			const FVector To = At + FRotator(0.0, 45.0 * k, 0.0).Vector() * Dist;
			FHitResult Hit;
			if (W->LineTraceSingleByChannel(Hit, At, To, ECC_Visibility, Q)) continue;
			if (W->OverlapBlockingTestByChannel(To, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(Rad, Half), Q)) continue;
			if (!W->LineTraceSingleByChannel(Hit, To, To - FVector(0.0, 0.0, Half + 200.0), ECC_Visibility, Q)) continue;
			return Hit.ImpactPoint + FVector(0.0, 0.0, Half + 3.0);
		}
		return At;
	}

	void PartyCheck(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP party check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
	}
}

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TestSayTo(int32 Who, const FString& Step, int32 Arg)
{
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host asks the %s to %s (%d)"), WhoWord(Who), *Step, Arg);
	if (AOutlawsCoopLink* K = GetLink(Who)) K->ClientTest(Step, Arg);
}
#endif // WITH_OUTLAWS_TESTS

void UOutlawsCoop::PartyReport(const TCHAR* Why) const
{
	// what this machine has of the others: their puppets, their voices, the shots and blasts it was shown
	TArray<FString> Drawn, Voices;
	for (int32 i = 0; i < MaxPlayers; ++i)
	{
		if (i == MyIndex()) continue;
		if (Puppets[i].IsValid()) Drawn.Add(FString::FromInt(i));
	}
	for (int32 i = 0; i < MaxPlayers; ++i)
	{
		if (VoiceReceivedFrom(i) > 0) Voices.Add(FString::Printf(TEXT("%d:%d"), i, VoiceReceivedFrom(i)));
	}
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP party report (%s, %s, player %d): puppets [%s]; voice packets by speaker [%s], passed on %d; shots shown %d, blasts shown %d, throws shown %d"),
		Why, WhoWord(MyIndex()), MyIndex(), *FString::Join(Drawn, TEXT(",")), *FString::Join(Voices, TEXT(",")), VoiceRelayed(),
		VerbShotsShown, VerbBlastsShown, VerbThrowsShown);
}

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TickHostPartyTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	AOutlawsCoopSession* S = GetSession();
	if (!S) return;
	TArray<AOutlawsCoopLink*> Links;
	GetLinks(Links);
	TArray<int32> Clients;
	for (const AOutlawsCoopLink* K : Links) if (K->PlayerIndex > 0) Clients.Add(K->PlayerIndex);
	auto ToAll = [&](const TCHAR* Step, int32 Arg) { for (const int32 k : Clients) TestSayTo(k, Step, Arg); };
	if (TestStep < PartyFirstStep)
	{
		TestStep = PartyFirstStep;
		TestAt = 0.0;
	}
	switch (TestStep)
	{
	case 600:
	{
		// everyone in and every level built (the host waited in the lobby for all of them)
		bool bAllBuilt = Clients.Num() > 0 && Clients.Num() + 1 >= FMath::Max(PartyPlayers, 3);
		for (const AOutlawsCoopLink* K : Links) if (K->PlayerIndex > 0) bAllBuilt = bAllBuilt && K->BuiltSerial == StartedSerial;
		if (!bAllBuilt) return;
		if (TestAt == 0.0)
		{
			FString Who;
			for (const int32 k : Clients) Who += FString::Printf(TEXT("%s%d"), Who.IsEmpty() ? TEXT("") : TEXT(","), k);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP party: %d players in %s (the host and clients %s)"), Clients.Num() + 1, *L->LevelName, *Who);
			L->bInvincible = true;
			ToAll(TEXT("godmode"), 1);
			TestAt = Now + 5.0;                  // (the poses come in and every puppet is drawn)
			return;
		}
		if (Now < TestAt) return;
		// 1: the host draws every client
		int32 Drawn = 0;
		for (const int32 k : Clients) Drawn += Puppets[k].IsValid() ? 1 : 0;
		PartyCheck(Drawn == Clients.Num(), FString::Printf(TEXT("the host draws %d of %d other players"), Drawn, Clients.Num()));
		PartyReport(TEXT("the start"));
		ToAll(TEXT("pt:report"), Clients.Num());          // (each client checks it draws everyone else: the others are Clients.Num())
		PartyShooter = Clients.Last();
		TestStep = 601;
		TestAt = Now + 2.0;
		return;
	}
	case 601:
		// 2: the last client shoots, blows a stick far over its head and talks; the others must be shown all of it
		if (Now < TestAt) return;
		TestSayTo(PartyShooter, TEXT("pt:shot"), 0);
		TestSayTo(PartyShooter, TEXT("pt:blast"), 0);
		TestSayTo(PartyShooter, TEXT("pt:mic"), 1);
		TestStep = 602;
		TestAt = Now + 4.0;
		return;
	case 602:
	{
		if (Now < TestAt) return;
		TestSayTo(PartyShooter, TEXT("pt:mic"), 0);
		const int32 Heard = VoiceReceivedFrom(PartyShooter);
		PartyCheck(Heard > 0, FString::Printf(TEXT("the host hears the %s (%d packets)"), WhoWord(PartyShooter), Heard));
		const int32 Relayed = VoiceRelayed();
		PartyCheck(Relayed > 0 || Clients.Num() < 2, FString::Printf(TEXT("the host passed the %s's voice on (%d packets)"), WhoWord(PartyShooter), Relayed));
		PartyReport(TEXT("after the shooter"));
		for (const int32 k : Clients) if (k != PartyShooter) TestSayTo(k, TEXT("pt:heard"), PartyShooter);
		// 3: a bad guy; each client in turn stands beside it
		PartyEnemy = TestPickEnemy(L, P->GetActorLocation(), -1, false);
		PartyVictim = 0;                         // (the index into Clients of the one standing beside it)
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP party: the bad guy for the targeting checks: %d"), PartyEnemy);
		TestStep = PartyEnemy >= 0 ? 603 : 606;
		TestAt = Now + 1.5;
		return;
	}
	case 603:
		if (Now < TestAt) return;
		if (!Clients.IsValidIndex(PartyVictim))
		{
			TestStep = 606;
			TestAt = Now;
			return;
		}
		TestSayTo(Clients[PartyVictim], TEXT("pt:stand"), PartyEnemy);
		TestStep = 604;
		TestAt = Now + 2.0;
		return;
	case 604:
	{
		if (Now < TestAt) return;
		AOutlawsEnemy* E = PartyEnemyAt(L, PartyEnemy);
		const int32 Want = Clients[PartyVictim];
		if (E && !E->IsDead())
		{
			E->CoopChooseTarget(true);
			const FOutlawsCoopRemoteView* V = RemoteViewOf(Want);
			PartyCheck(E->CoopTarget == Want, FString::Printf(TEXT("bad guy %d picks the %s standing beside it (picked the %s; that player %.0f cm away, the host %.0f cm)"), PartyEnemy,
				WhoWord(Want), WhoWord(E->CoopTarget), V && V->bValid ? FVector::Dist(V->Feet, E->GetActorLocation()) : -1.0, FVector::Dist(P->GetActorLocation(), E->GetActorLocation())));
		}
		else
		{
			PartyCheck(false, FString::Printf(TEXT("bad guy %d is gone before the %s's turn"), PartyEnemy, WhoWord(Want)));
		}
		TestSayTo(Want, TEXT("pt:home"), 0);
		++PartyVictim;
		TestStep = 603;
		TestAt = Now + 1.5;
		return;
	}
	case 606:
		// 4: one client dies, another revives it (through the host)
		if (Now < TestAt) return;
		if (Clients.Num() < 2)
		{
			TestStep = 609;
			TestAt = Now;
			return;
		}
		PartyVictim = Clients[0];
		PartyReviver = Clients[1];
		TestSayTo(PartyVictim, TEXT("die"), 0);
		TestStep = 607;
		TestAt = Now + 3.0;
		return;
	case 607:
	{
		if (Now < TestAt) return;
		const AOutlawsCoopLink* VL = GetLink(PartyVictim);
		PartyCheck(VL && VL->Stats.bDead, FString::Printf(TEXT("the %s is dead here (its stats)"), WhoWord(PartyVictim)));
		TestSayTo(PartyReviver, TEXT("pt:revive"), PartyVictim);
		TestStep = 608;
		TestAt = Now + 5.0;
		return;
	}
	case 608:
	{
		if (Now < TestAt) return;
		const AOutlawsCoopLink* VL = GetLink(PartyVictim);
		PartyCheck(VL && !VL->Stats.bDead && VL->Stats.Health > 0.f, FString::Printf(TEXT("the %s revived by the %s (health %.1f here)"), WhoWord(PartyVictim), WhoWord(PartyReviver),
			VL ? VL->Stats.Health : -1.f));
		TestSayTo(PartyVictim, TEXT("rstate"), 0);
		TestStep = 609;
		TestAt = Now + 2.0;
		return;
	}
	case 609:
	{
		// 5: a save with every client's block
		if (Now < TestAt) return;
		const bool bOk = L->SaveGame(9);
		int32 Have = 0;
		for (const int32 k : Clients) Have += PlayerJsonFor(k).IsEmpty() ? 0 : 1;
		PartyCheck(bOk && Have == Clients.Num(), FString::Printf(TEXT("the save in slot 9 %s, with %d of %d clients' blocks (%d kept)"), bOk ? TEXT("written") : TEXT("NOT WRITTEN"),
			Have, Clients.Num(), PlayerJsons.Num()));
		// 6: everyone dies
		ToAll(TEXT("die"), 0);
		TestStep = 610;
		TestAt = Now + 1.5;
		return;
	}
	case 610:
	{
		if (Now < TestAt) return;
		const bool bWas = L->bInvincible;
		L->bInvincible = false;
		L->DamagePlayer(1e6f, 1.f, true);
		L->bInvincible = bWas;
		PartyNote = L->LevelName;
		PartyEnemy = StartedSerial;              // (the level's start as it was: the restart comes as a new one, or a rebuild)
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP party: the host dies too (dead %d); all %d players dead"), L->bDead ? 1 : 0, Clients.Num() + 1);
		TestStep = 611;
		TestAt = Now + 20.0;
		return;
	}
	case 611:
	{
		// the restart: everyone alive again in the same level
		const bool bBack = !L->bDead && bLevelBuilt && !L->bNeedPlace && BothDeadSince == 0.0;
		bool bAllUp = bBack;
		for (const AOutlawsCoopLink* K : Links) if (K->PlayerIndex > 0) bAllUp = bAllUp && K->Stats.Seq > 0 && !K->Stats.bDead;
		if (!bAllUp && Now < TestAt) return;
		PartyCheck(bAllUp, FString::Printf(TEXT("all dead: the level (%s) restarted for everyone (host alive %d)"), *PartyNote, L->bDead ? 0 : 1));
		TestStep = 612;
		TestAt = Now + 6.0;                      // (every machine's puppets drawn again in the rebuilt level)
		return;
	}
	case 612:
	{
		if (Now < TestAt) return;
		int32 Drawn = 0;
		for (const int32 k : Clients) Drawn += Puppets[k].IsValid() ? 1 : 0;
		PartyCheck(Drawn == Clients.Num(), FString::Printf(TEXT("after the restart the host draws %d of %d other players"), Drawn, Clients.Num()));
		PartyReport(TEXT("the end"));
		ToAll(TEXT("pt:report"), Clients.Num());
		TestStep = 613;
		TestAt = Now + 1.5;
		return;
	}
	case 613:
		// 7: the level won; every machine's movie skipped a few seconds in (an offscreen run's may never end)
		if (Now < TestAt) return;
		PartyEnemy = StartedSerial;
		NextHoldLastWaiting = -1;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP party: the host wins the level (serial %d)"), StartedSerial);
		L->CompleteLevel(TEXT("the party test"));
		PartyReviver = 0;
		TestStep = 614;
		TestAt = Now + 4.0;
		PartyVictim = 0;                         // (skips sent)
		return;
	case 614:
	{
		// skips every 2 s while the level's end runs anywhere; then every machine built the next level
		const AOutlawsCoopSession* Ss = GetSession();
		bool bAllBuilt = Ss && Ss->StartSerial > PartyEnemy && StartedSerial == Ss->StartSerial && bLevelBuilt;
		for (const AOutlawsCoopLink* K : Links) if (K->PlayerIndex > 0) bAllBuilt = bAllBuilt && K->BuiltSerial == StartedSerial;
		if (bAllBuilt)
		{
			PartyCheck(NextHoldLastWaiting == 0, FString::Printf(TEXT("the next level (%s) started once every player had finished the level's end (%d still in it when it went)"),
				*L->LevelName, NextHoldLastWaiting));
			TestStep = 615;
			TestAt = Now + 2.0;
			return;
		}
		if (Now >= TestAt && PartyVictim < 12)
		{
			// the last client is left in its level's end at first: the host must hold the next level for it, then it skips too
			++PartyVictim;
			if (L->IsMoviePlaying()) L->SkipMovie();
			const bool bHoldingForLast = !NextHoldLevel.IsEmpty() && FPlatformTime::Seconds() - NextHoldSince >= 5.0;
			if (bHoldingForLast && PartyReviver != 1)
			{
				PartyReviver = 1;                // (once)
				PartyCheck(true, FString::Printf(TEXT("the next level (%s) held %.1f s for the %s still in the level's end"), *NextHoldLevel,
					FPlatformTime::Seconds() - NextHoldSince, WhoWord(Clients.Last())));
			}
			for (const int32 k : Clients) if (k != Clients.Last() || PartyReviver == 1) TestSayTo(k, TEXT("skip"), 0);
			TestAt = Now + 2.0;
		}
		if (PartyVictim >= 12 && Now > TestAt + 40.0)
		{
			PartyCheck(false, TEXT("the next level never came after the level was won"));
			TestStep = 615;
		}
		return;
	}
	case 615:
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

void UOutlawsCoop::TickClientPartyTest(double Now)
{
	if (PartyReportWant < 0) return;
	int32 Drawn = 0;
	for (int32 i = 0; i < MaxPlayers; ++i) Drawn += i != MyIndex() && Puppets[i].IsValid() ? 1 : 0;
	const double Took = Now - PartyReportAskedAt;
	if (Drawn != PartyReportWant && Took < 25.0) return;
	PartyCheck(Drawn == PartyReportWant, FString::Printf(TEXT("the %s draws %d of %d other players (%.1f s after the host asked)"), WhoWord(MyIndex()), Drawn, PartyReportWant, Took));
	PartyReportWant = -1;
	PartyReport(TEXT("asked"));
}

void UOutlawsCoop::OnPartyTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	if (Step == TEXT("report"))
	{
		// Arg: how many clients the host has; this client draws them all but itself, and the host (judged in TickClientPartyTest:
		// after a slow host build the host's own link can come late, so the check waits up to 25 s and says how long it took)
		PartyReportWant = Arg;
		PartyReportAskedAt = FPlatformTime::Seconds();
		TickClientPartyTest(PartyReportAskedAt);
	}
	else if (Step == TEXT("shot"))
	{
		const FVector Muzzle = P->GetActorLocation() + P->GetActorForwardVector() * 60.0 + FVector(0.0, 0.0, 40.0);
		NoteShot(L, Muzzle, TEXT(""));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP party: the %s fires (a shot told to the others)"), WhoWord(MyIndex()));
	}
	else if (Step == TEXT("blast"))
	{
		// (high over its head: seen, harming nobody)
		NoteBlast(L, P->GetActorLocation() + FVector(0.0, 0.0, 6000.0), -1);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP party: the %s's blast told to the others"), WhoWord(MyIndex()));
	}
	else if (Step == TEXT("mic"))
	{
		SetVoiceTestMic(Arg);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP party: the %s's test tone %s"), WhoWord(MyIndex()), Arg ? TEXT("on") : TEXT("off"));
	}
	else if (Step == TEXT("heard"))
	{
		// Arg: the shooter; its shot, blast and voice must all have come here through the host
		const int32 Voices = VoiceReceivedFrom(Arg);
		PartyCheck(VerbShotsShown > 0, FString::Printf(TEXT("the %s saw the %s's shot (%d shown)"), WhoWord(MyIndex()), WhoWord(Arg), VerbShotsShown));
		PartyCheck(VerbBlastsShown > 0, FString::Printf(TEXT("the %s saw the %s's blast (%d shown)"), WhoWord(MyIndex()), WhoWord(Arg), VerbBlastsShown));
		PartyCheck(Voices > 0, FString::Printf(TEXT("the %s heard the %s's voice (%d packets)"), WhoWord(MyIndex()), WhoWord(Arg), Voices));
	}
	else if (Step == TEXT("stand"))
	{
		// beside bad guy Arg (2 m from this machine's copy of him), facing him
		AOutlawsEnemy* E = PartyEnemyAt(L, Arg);
		if (!E) return;
		PartyHome = P->GetActorLocation();
		const FVector At = PartySpotNear(P, E->GetActorLocation(), 200.0, E);
		P->TeleportTo(At, FRotator(0.0, (E->GetActorLocation() - At).Rotation().Yaw, 0.0), false, true);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP party: the %s stands %.0f cm from bad guy %d"), WhoWord(MyIndex()), FVector::Dist(At, E->GetActorLocation()), Arg);
	}
	else if (Step == TEXT("home"))
	{
		if (!PartyHome.IsNearlyZero()) P->TeleportTo(PartyHome, P->GetActorRotation(), false, true);
		PartyHome = FVector::ZeroVector;
	}
	else if (Step == TEXT("revive"))
	{
		// Arg: the dead player; this client, with a med bag (one bag, 3 points), grips its body
		L->Counts.Remove(TEXT("idocbag"));
		L->AddCount(TEXT("idocbag"), 3);
		const AOutlawsRemotePlayer* R = PuppetOf(Arg);
		FVector Hand = P->GetActorLocation();
		if (R)
		{
			const FVector Feet = R->ShownBody() - FVector(0.0, 0.0, R->ShownHalf());
			const FVector At = PartySpotNear(P, Feet + FVector(0.0, 0.0, P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 3.0), 140.0);
			P->TeleportTo(At, FRotator(0.0, (Feet - At).Rotation().Yaw, 0.0), false, true);
			Hand = Feet + FVector(0.0, 0.0, 25.0);
		}
		const bool bTook = TryRevive(P, Hand);
		PartyCheck(bTook, FString::Printf(TEXT("the %s's grip on the %s's body took it (%s)"), WhoWord(MyIndex()), WhoWord(Arg), R ? TEXT("its puppet here") : TEXT("NO PUPPET")));
	}
}
#endif // WITH_OUTLAWS_TESTS
