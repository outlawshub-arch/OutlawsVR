// -OLCoopAuto -OLCoopTest=bossend -OLCoopLevel=ranch (user 2026-10-12, BUG-RANCHDOOR and BUG-DRDEATHANIM in co-op), the host and
// its client each.  The client uses Bob's door (BOBDOOR) with the key: the use runs on the host's INF, the host plays the
// door's movie and tells the client to play it too.  Each machine checks the movie starts, the door does not move before its
// own movie covers the level (nor ever while the level shows), and the door is open once the movie is over.  Then the host
// shoots Bob down: each checks no death choreography of his is played (his live frame held) and the level ends.  Each logs
// "OLCOOP bossend check PASS|FAIL" lines and its RESULT; the host has the client quit.
// (user 2026-10-14) -OLCoopLevel=hideout (Slim) or miner (Dr. Death): no door, the boss summoned and shot down; the host
// hurts him first and kills him 0.15 s later, inside the stun of the first shot, as in play; each machine checks the frame
// held is a live pose (not the stun, not a death).
#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsEnemy.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsSpriteActor.h"
#include "Engine/World.h"

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TickBossEndTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	const bool bHost = IsHost();
	const TCHAR* Me = bHost ? TEXT("host") : TEXT("client");
	L->bInvincible = true;
	auto Check = [this, Me](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP bossend check %s (%s): %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), Me, *What);
		if (bOk) ++BePasses; else ++BeFails;
	};
	// the door and Bob, as this machine has them
	if (BeDoor < 0 && L->Inf)
	{
		for (int32 i = 0; i < L->Inf->Elevators.Num(); ++i)
		{
			if (L->Inf->Elevators[i].Name.Equals(TEXT("BOBDOOR"), ESearchCase::IgnoreCase)) { BeDoor = i; BeDoorStart = L->Inf->Elevators[i].Current; }
		}
	}
	if (!BeBob.IsValid())
	{
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies()) if (Wk.IsValid() && Wk->bFallInCutscene && !Wk->bVoiceOnly) BeBob = Wk;
	}
	const FOutlawsElevator* Door = L->Inf && L->Inf->Elevators.IsValidIndex(BeDoor) ? &L->Inf->Elevators[BeDoor] : nullptr;
	// each frame through the door's part: how far it went before this machine's movie covered the level, and any move seen
	if (Door && BeStep >= 1 && BeStep < 5)
	{
		const double Off = FMath::Abs(Door->Current - BeDoorStart);
		if (L->MidMovieShown == 0) BeDoorMoved = FMath::Max(BeDoorMoved, Off);
		static double LastValue = 0.0;
		if (FMath::Abs(Door->Current - LastValue) > 1e-6 && L->MidMovieHidden.Num() == 0 && BeStep < 4) ++BeVisibleMoves;
		LastValue = Door->Current;
	}
	static int32 HiddenFrames = 0, HeldFrames = 0, NotLiveFrames = 0;
	if (BeStep >= 6)
	{
		bBeEnded = bBeEnded || L->DebugLevelEnding() || L->IsMoviePlaying() || L->LevelEndTimer >= 0.f;
		if (AOutlawsEnemy* Bob = BeBob.Get())
		{
			BeBobDieChors = Bob->DieChorsPlayed;
			bBeHeldSeen = bBeHeldSeen || Bob->bHeldForCutscene;
			if (Bob->bHeldForCutscene && !L->Movie)
			{
				++HeldFrames;
				if (Bob->IsHidden() || (Bob->GetSprite() && Bob->GetSprite()->IsHidden())) ++HiddenFrames;
				if (!Bob->GetSprite() || !Bob->IsLivePoseChor(Bob->GetSprite()->GetPlayingChor())) ++NotLiveFrames;
			}
		}
	}
	switch (BeStep)
	{
	case 0:
	{
		const AOutlawsCoopLink* Other = GetLink(PartnerIndex());
		const bool bDoorLevel = L->LevelName.Equals(TEXT("ranch"), ESearchCase::IgnoreCase);
		if ((bDoorLevel && !Door) || !BeBob.IsValid() || !Other) return;
		if (bHost && Other->BuiltSerial != StartedSerial) return;
		if (BeAt == 0.0) { BeAt = Now + 6.0; return; }
		if (Now < BeAt) return;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP bossend: the %s ready: BOBDOOR elevator %d at %.1f (stop %d), Bob %s behind his door %d"), Me, BeDoor, Door ? Door->Current : -1.0, Door ? Door->Stop : -1,
			*BeBob->GetOutlawsName(), BeBob->bBehindDoor ? 1 : 0);
		if (!bDoorLevel)
		{
			// (the hideout, the mine: no door; the boss brought out on the host, and shot down once both see him)
			if (bHost) { L->WakeCurtainBoss(TEXT("bossend test")); L->DebugSummonBoss(); }
			BeAt = Now + 6.0;
			BeStep = 5;
			return;
		}
		if (!bHost)
		{
			// the client's use, with the key in its own pockets (the host's lock asks them)
			L->GiveItem(101);
			const bool bTaken = Door->Sectors.Num() > 0 && L->PushDoorSector(Door->Sectors[0]);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP bossend: the client pushes BOBDOOR with key 101: %s"), bTaken ? TEXT("taken (sent to the host)") : TEXT("NOT taken"));
		}
		BeAt = Now + 20.0;
		BeStep = 1;
		return;
	}
	case 1:
		// the movie: the host's from its INF, the client's on the host's word
		if (!L->bMidMovieOn && L->MidMovieShown == 0 && Now < BeAt) return;
		Check(L->bMidMovieOn || L->MidMovieShown > 0, FString::Printf(TEXT("the door's movie started here (%s)"), bHost ? TEXT("the host's INF, set off by the client's use") : TEXT("the host's word")));
		if (!bHost) Check(MidMoviesPlayed == 1, FString::Printf(TEXT("the host's word to play it came once (%d)"), MidMoviesPlayed));
		else Check(MidMoviesSent >= 1, FString::Printf(TEXT("the host told its client to play it (%d)"), MidMoviesSent));
		BeAt = Now + 10.0;
		BeStep = 2;
		return;
	case 2:
		if (L->MidMovieShown == 0 && Now < BeAt) return;
		Check(L->MidMovieShown > 0, TEXT("the movie covers the level here"));
		Check(BeDoorMoved < 0.01, FString::Printf(TEXT("the door had not moved before the movie covered the level (%.2f degrees)"), BeDoorMoved));
		BeAt = Now + 3.0;
		BeStep = 3;
		return;
	case 3:
		if (Now < BeAt) return;
		if (L->IsMoviePlaying()) L->SkipMovie();
		BeAt = Now + 6.0;
		BeStep = 4;
		return;
	case 4:
		// the movie over here, and the door open (it opened behind the movie)
		if (L->bMidMovieOn && Now < BeAt) return;
		if (Now < BeAt - 3.0) return;
		Check(!L->bMidMovieOn, TEXT("the movie is over here, the level back"));
		Check(Door && Door->Target < 0 && Door->Stop == 1, FString::Printf(TEXT("BOBDOOR open after the movie (at %.1f, stop %d, moving %d)"), Door ? Door->Current : -1.0, Door ? Door->Stop : -1, Door ? Door->Target : -2));
		Check(BeVisibleMoves == 0, FString::Printf(TEXT("the door never moved while the level showed (%d frames)"), BeVisibleMoves));
		if (bHost && BeBob.IsValid()) Check(!BeBob->bBehindDoor, TEXT("Bob awake behind his opened door"));
		BeAt = Now + 4.0;
		BeStep = 5;
		return;
	case 5:
		if (Now < BeAt) return;
		if (bHost)
		{
			// the host hurts him from afar first (his stun plays), then 0.15 s on shoots him down (his health the host's; the
			// client's copy dies on its word)
			AOutlawsEnemy* Bob = BeBob.Get();
			const FVector Far = Bob ? Bob->GetActorLocation() - FVector(2000.0, 0.0, 0.0) : FVector::ZeroVector;
			if (Bob) Bob->TakeHit(FMath::Max(0.2f, Bob->GetHealth() * 0.3f), Bob->GetActorLocation(), FVector(1.0, 0.0, 0.0), Far);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP bossend: the host hurts %s first: health %.2f, chor %d"), Bob ? *Bob->GetOutlawsName() : TEXT("nobody"), Bob ? Bob->GetHealth() : -1.f,
				Bob && Bob->GetSprite() ? Bob->GetSprite()->GetPlayingChor() : -1);
		}
		BeAt = Now + 0.15;
		BeStep = 10;
		return;
	case 10:
		if (Now < BeAt) return;
		if (bHost)
		{
			AOutlawsEnemy* Bob = BeBob.Get();
			const FVector From = P->GetActorLocation();
			const int32 Was = Bob && Bob->GetSprite() ? Bob->GetSprite()->GetPlayingChor() : -1;
			for (int32 k = 0; Bob && k < 60 && !Bob->IsDead(); ++k) Bob->TakeHit(50.f, Bob->GetActorLocation(), (Bob->GetActorLocation() - From).GetSafeNormal(), From);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP bossend: the host shoots %s down (chor %d on show): dead %d, end movie ready %d"), Bob ? *Bob->GetOutlawsName() : TEXT("nobody"), Was,
				Bob && Bob->IsDead() ? 1 : 0, L->EndMovieReady() ? 1 : 0);
		}
		BeAt = Now + 4.0;
		BeStep = 6;
		return;
	case 6:
	{
		if (Now < BeAt) return;
		if (!bHost)
		{
			// (the client: 1.5 s from when its copy of him is seen dead; the host's kill may come later than its own clock)
			static double DeadSeenAt = 0.0;
			const AOutlawsEnemy* Bob = BeBob.Get();
			if (Bob && !Bob->IsDead() && !bBeEnded && Now < BeAt + 20.0) return;
			if (DeadSeenAt == 0.0) DeadSeenAt = Now;
			if (Now < DeadSeenAt + 1.5) return;
		}
		Check(bBeHeldSeen, TEXT("the boss died into the cutscene: his live frame held here"));
		Check(HeldFrames > 0 && NotLiveFrames == 0, FString::Printf(TEXT("the frame held is a live pose, not the stun or a death (%d of %d frames not)"), NotLiveFrames, HeldFrames));
		Check(BeBobDieChors == 0, FString::Printf(TEXT("no death choreography of Bob's played here (%d)"), BeBobDieChors));
		Check(HeldFrames > 0 && HiddenFrames == 0, FString::Printf(TEXT("Bob seen as he stood for the fade, not hidden (%d of %d frames hidden)"), HiddenFrames, HeldFrames));
		Check(bBeEnded, TEXT("the level ends on his death here"));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP bossend %sRESULT %s (%d passed, %d failed)"), bHost ? TEXT("") : TEXT("client "), BeFails == 0 && BePasses > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), BePasses, BeFails);
		BeAt = Now + 2.0;
		BeStep = bHost ? 7 : 20;
		return;
	}
	case 7:
		if (Now < BeAt) return;
		TestSay(TEXT("quit"), 0);
		BeAt = Now + 2.0;
		BeStep = 8;
		return;
	case 8:
		if (Now < BeAt) return;
		Summary(TEXT("done"));
		QuitForTest();
		BeStep = 9;
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
