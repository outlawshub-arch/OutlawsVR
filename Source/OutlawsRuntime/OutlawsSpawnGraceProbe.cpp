// REQ-SPAWNGRACE's probe (the user after the Quest test of 4 Nov, "stop spawn killing and spawn deaths"): a match's fresh spawn is
// invincible and harmless for 3 s and flashes while it lasts.  Up to three machines:
//   host:   -OLMpHost=town_2p -OLMpClass=dm -OLMpBots=2 -OLMpBotChars=Jam,Jam -OLMpChar=Jam -OLMpHostRespawn=2 -OLMpMinutes=30 -OLMpFrags=0
//           -OLSpawnGraceProbe [-OLOldSpawnGrace]
//   client: -OLCoopJoin=<host>:<port> -OLMpChar=Jam -OLSpawnGraceProbe [-OLOldSpawnGrace]
//   joiner: the same with -OLLobbyEnterAfter=2 -OLSpawnGraceJoiner, started when the host's log says "joiner now": it comes in on
//           a match under way, a bot held in its protection, and says when it sees that bot's sprite flash.
//   co-op:  -OLCoopAuto -OLCoopHost -OLCoopLevel=<lv> -OLSpawnGraceProbe, and its client: no protection, no flash, the first hurt taken.
// The bots hold their fire; every blow goes through the game's own calls (ShotAtPlayers, StrikeAtPlayers, Explode, BotPellet).
// 1. The match's start: the flag on every link, off on time; the flash on the hands and on a sprite, the host's and the client's.
// 2. A protected victim, a bot and the client: a shot, a knife, a fist and a stick deal nothing, throw nothing and draw no blood; a
//    fist just before the end is refused, one just after it deals and bleeds.
//    Then the client comes up on a stick, three times: set off in the very call that sends its respawn, so the blast goes ahead of
//    the flag, and the client is not thrown (-OLOldSpawnGrace: it is, the control).
// 3. A protected striker, the client, the host and a bot: its gun, knife, fist and stick (a bot's shot and stick) deal nothing and
//    draw no blood, its shots do not end the protection, its own stick and a hurt of its own leave it whole and where it stood;
//    once it ends its hit deals.
// 4. REQ-SPAWNSTICK: a stick is judged at its throw, each of the bot, the client and the host throwing: after its protection, full;
//    before a death and going off in the protection after it, full, the thrower neither hurt nor thrown; in its protection and going
//    off after the end, nothing.  -OLOldSpawnStick: judged at the blast, those last two the other way.
// 5. A joiner comes in on a flash under way; flat shots of it, the host's hands held up with the bot in view
//    (Saved\OLShots\ol_spawngrace_*).
// -OLOldSpawnGrace: no flag, no flash, no shield; a protected striker deals, and its shot ends its protection.
// A stall that jumps a protection's window does the wait over, "OLSHOT spawngrace done over: ...", counted in the host's RESULT.
// -OLSpawnGraceStall=<ms> makes one on the host's first respawn in part 4; -OLSpawnGraceNoRedo keeps the wait from being done over.
// Lines "OLSHOT spawngrace check PASS|FAIL: ..." and "OLSHOT spawngrace <host|client|joiner|coop> RESULT".

#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsBoxHand.h"
#include "OutlawsDeathmatch.h"
#include "OutlawsEnemy.h"
#include "OutlawsFlags.h"
#include "OutlawsLevelActor.h"
#include "OutlawsMpHost.h"
#include "OutlawsPlayer.h"
#include "OutlawsRemotePlayer.h"
#include "OutlawsSpriteActor.h"
#include "Camera/CameraComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/PlatformMisc.h"
#include "MotionControllerComponent.h"
#include "TimerManager.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsSpawnGraceProbeLocal
{
	int32 Passes = 0, Fails = 0, Client = -1, Bot = -1, Part = 0, Phase = 0, Target = -1;
	int32 BaseDealt = 0, BaseRefused = 0, BaseBlasts = 0;
	float BaseHealth = 0.f, BaseOther = 0.f, MaxHeld = 0.f, MaxSprite = 0.f;
	double PhaseAt = 0.0, Until = 0.0, GraceOnAt = 0.0, ClientOnFor = 0.0;
	bool bStarted = false, bCheckedOn = false, bClientFlag = false, bSaid = false, bShotTop = false, bShotLow = false, bQuit = false;
	FVector BaseAt = FVector::ZeroVector;
	double OffAt[16] = {}, UntilOf[16] = {};      // (by seat: a bot's is 14 or 15, MaxPlayers 16)
	// the client up on a stick (StepWindow; the client's half: sg:window)
	const int32 WindowTries = 3;
	int32 WindowShields = 0, WindowBlasts = 0;
	bool bWindowHooked = false, bWindowArmed = false, bWindowWatch = false;
	double WindowUpAt = 0.0;
	float WindowMoved = 0.f, WindowSpeed = 0.f;
	FVector WindowFrom = FVector::ZeroVector;
	// (part 4: the sticks; 0 the bot's, 1 the client's, 2 the host's)
	int32 StickThrower = 0;
	int32 StickRounds = 0;      // (-OLSpawnGraceSticks=<n>: part 4 alone, n times round, for the rates)
	double StickStillAt = 0.0;
	FVector StickLast = FVector::ZeroVector, StickFeet = FVector::ZeroVector, BaseSelf = FVector::ZeroVector;
	// (a stall against a window: a wait done over (Redo), the blast as it reached the host, this phase's longest frame)
	const int32 RedoMost = 6;
	int32 Redos = 0, StallMs = 0;           // (-OLSpawnGraceStall=<ms>: that stall by hand, the host's frame just after its first respawn in part 4)
	bool bStallDone = false, bBlastSeen = false, bFlagAtBlast = false;
	double BlastSeenAt = 0.0, LastTickWall = 0.0, LongFrame = 0.0;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT spawngrace check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}
	enum : int32 { StepWait = 3900, StepLive, StepStart, StepVictims, StepWindow, StepStrikers, StepSticks, StepHold, StepFinish, StepQuit, StepDone };
}

void UOutlawsCoop::SpawnGraceBlows(AOutlawsLevelActor* L, AOutlawsPlayer* P, int32 Who, bool bStick)
{
	const AOutlawsRemotePlayer* R = PuppetOf(Who);
	if (!R || !L || !P) return;
	const FVector Chest = R->ShownBody() + FVector(0.0, 0.0, 20.0);
	const UCameraComponent* Cam = P->Camera.Get();
	const FVector Eye = Cam ? Cam->GetComponentLocation() : P->GetActorLocation();
	const FVector Dir = (Chest - Eye).GetSafeNormal();
	FVector At;
	NoteShot(L, Eye, TEXT(""));
	ShotAtPlayers(L, Eye, Chest + Dir * 300.0, 2.f, At);
	StrikeAtPlayers(L, Chest, 22.f, 0.8f, TEXT("a fist"));
	ReportStrike(L, Who, 4.f, Chest, TEXT("a knife's jab"));       // (straight to the report: StrikePlayer takes a second blow within 0.4 s as the first still going through)
	if (bStick) L->Explode(R->ShownBody() - FVector(0.0, 0.0, 60.0));
}

void UOutlawsCoop::SpawnGraceTestStick(AOutlawsPlayer* P, const FVector& At, float FuseLeft)
{
	using namespace OutlawsSpawnGraceProbeLocal;
	P->Equip(1, 6);
	P->Mag[1] = 1;
	P->bFuseLit[1] = true;
	P->Fuse[1] = FuseLeft;
	const int32 Before = P->Thrown.Num();
	P->ThrowHeld(1, false, true);
	if (P->Thrown.Num() <= Before) { Check(false, TEXT("a real throw of a lit stick")); return; }
	AOutlawsPlayer::FThrown& T = P->Thrown.Last();
	T.Pos = At;
	T.Vel = FVector::ZeroVector;
	T.Fuse = FuseLeft;
	if (AActor* Ac = T.Actor.Get()) Ac->SetActorLocation(At);
}

void UOutlawsCoop::SpawnGraceTestStickGo(AOutlawsPlayer* P, float FuseLeft)
{
	using namespace OutlawsSpawnGraceProbeLocal;
	for (int32 i = P->Thrown.Num() - 1; i >= 0; --i)
	{
		if (P->Thrown[i].Type != 1) continue;
		P->Thrown[i].Fuse = FuseLeft;
		return;
	}
	Check(false, TEXT("the stick thrown before the death is still there"));
}

void UOutlawsCoop::TickHostSpawnGraceTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	using namespace OutlawsSpawnGraceProbeLocal;
	const double Wall = FPlatformTime::Seconds();
	if (LastTickWall > 0.0) LongFrame = FMath::Max(LongFrame, Wall - LastTickWall);
	LastTickWall = Wall;
	// co-op's story: none of it
	if (!IsMatch())
	{
		if (bQuit) { if (Now >= TestAt) { bQuit = false; TestAt = Now + 3600.0; QuitForTest(); } return; }
		if (!bSaid)
		{
			bSaid = true;
			TestAt = Now + 150.0;
			const float H = L->GetHealth();
			L->DamagePlayer(1.f, 1.f, true);
			Check(!SpawnGraceOf(0) && !SpawnGraceOf(1) && SpawnGraceFlashHere(L) == 0.f, TEXT("co-op: nobody protected, no flash"));
			Check(L->GetHealth() < H, FString::Printf(TEXT("co-op: the hurt at the level's start is taken (%.1f -> %.1f)"), H, L->GetHealth()));
		}
		const bool bClientDone = TestNotes.ContainsByPredicate([](const FString& N) { return N.StartsWith(TEXT("sg coop")); });
		if (!bClientDone && Now < TestAt) return;
		Check(bClientDone, TEXT("co-op: the client's checks ran"));
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT spawngrace coop RESULT %s (%d passed, %d failed)"), Fails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		TestSay(TEXT("quit"));
		bQuit = true;
		TestAt = Now + 3.0;
		return;
	}
	FOutlawsMatchFlow& F = OutlawsMpHost::Flow();
	const bool bOld = OLFlags::Param(TEXT("OLOldSpawnGrace"));
	L->bEnemiesActive = false;          // (the bots stand where they spawned, holding their fire)
	auto HealthOf = [this, L](int32 Who) -> float
	{
		if (Who == 0) return L->GetHealth();
		if (IsBot(Who)) { const FBot* B = FindBot(Who); return B ? B->Health : -1.f; }
		const AOutlawsCoopLink* K = GetLink(Who);
		return K ? K->Stats.Health : -1.f;
	};
	auto DeadOf = [this, L](int32 Who)
	{
		if (Who == 0) return L->IsPlayerDead();
		if (IsBot(Who)) { const FBot* B = FindBot(Who); return !B || B->bDead; }
		const AOutlawsCoopLink* K = GetLink(Who);
		return !K || (K->Stats.Seq > 0 && K->Stats.bDead);
	};
	auto BodyOf = [this, P](int32 Who)
	{
		const AOutlawsCoopLink* K = GetLink(Who);
		return Who == 0 ? P->GetActorLocation() : K ? FVector(K->Pose.Body) : FVector::ZeroVector;
	};
	auto Fresh = [&](int32 Who)
	{
		const FOutlawsDmPlayer* D = Dm.Find(Who);
		return D && D->bAlive && Wall < D->ProtectedUntil - 1.5 && !MatchDead[Who] && !DeadOf(Who) && HealthOf(Who) >= 9.99f && (Who == 0 || PuppetOf(Who) != nullptr);
	};
	auto Ready = [&](int32 Who)
	{
		// Fresh, and the word of it in everywhere: the flag on its link (written the frame after the rules take it), then 0.15 s for
		// the flag to reach the client (a blast's call outruns it: 2 ms from the write to the client's blast, measured), and its
		// puppet up on its new spot here (dead or hidden a puppet takes no blow, and it comes to a new spot over some frames)
		const FOutlawsDmPlayer* D = Dm.Find(Who);
		if (!Fresh(Who) || (!bOld && !SpawnGraceOf(Who)) || !D || Wall < D->ProtectedUntil - Dm.Config.SpawnProtect + 0.15) return false;
		const AOutlawsRemotePlayer* R = Who == 0 ? nullptr : PuppetOf(Who);
		return Who == 0 || (R && !R->IsHidden() && !R->PlateDead() && FVector::Dist(R->ShownBody(), BodyOf(Who)) < 60.0);
	};
	auto Beside = [&](int32 Who)
	{
		// the host 2.5 m off Who on a clear line (straight back can be inside a wall), its body put there: the host's check wants the
		// striker near, and nobody else on the way
		const FVector To = BodyOf(Who);
		FVector Flat = (To - P->GetActorLocation()).GetSafeNormal2D();
		if (Flat.IsNearlyZero()) Flat = FVector::ForwardVector;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsSpawnGraceBeside), false, P);
		for (int32 Turn = 0; Turn < 8; ++Turn)
		{
			const FVector Way = Flat.RotateAngleAxis(45.0 * Turn, FVector::UpVector);
			if (P->GetWorld()->LineTraceTestByChannel(To, To - Way * 330.0, ECC_Visibility, Q)) continue;
			Flat = Way;
			break;
		}
		P->SetActorLocation(To - Flat * 250.0, false, nullptr, ETeleportType::TeleportPhysics);
		if (AController* Ct = P->GetController()) Ct->SetControlRotation(Flat.Rotation());
	};
	auto Away = [&](int32 Who)
	{
		// the host off to the spawn point farthest from Who: out of reach of a stick at its feet
		double Best = -1.0;
		FVector Far = P->GetActorLocation();
		for (const FOutlawsDmSpawn& Sp : Dm.GetSpawns())
		{
			const double D = FVector::Dist(Sp.Location, BodyOf(Who));
			if (D > Best) { Best = D; Far = Sp.Location; }
		}
		P->SetActorLocation(Far, false, nullptr, ETeleportType::TeleportPhysics);
	};
	auto Kill = [&](int32 Who)
	{
		if (Who == 0) L->DamagePlayer(1e6f, 1.f, true);
		else if (IsBot(Who)) { if (const FBot* B = FindBot(Who)) BotHurt(Who, B->Health + 1.f, BodyOf(Who), BodyOf(Who), -1); }
		else TestSayTo(Who, TEXT("sg:die"));
	};
	auto Next = [&](int32 NewPhase) { Phase = NewPhase; PhaseAt = Wall; LongFrame = 0.0; };
	auto Missed = [&](int32 Who)
	{
		// Ready's window is 1.35 s of the 3 s, from the word of the respawn to 1.5 s left.  A stall can jump all of it: 1.78 s lost on
		// the host's frames right after its respawn, measured, then a live player with nothing to wait for.  Up, its window gone, 3 s
		// after the kill (a respawn takes 2 to 2.3 s): the wait is done over, by a new death; RedoMost bounds it, then the wait's FAIL
		const FOutlawsDmPlayer* D = Dm.Find(Who);
		return !OLFlags::Param(TEXT("OLSpawnGraceNoRedo")) && Redos < RedoMost && Wall - PhaseAt >= 3.0 && D && D->bAlive && Wall >= D->ProtectedUntil - 1.5 && !MatchDead[Who] && !DeadOf(Who);
	};
	auto Redo = [&](int32 Who, const TCHAR* What)
	{
		++Redos;
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT spawngrace done over: %s, the %s, %.2f s into the wait, the longest frame %.2f s"), What, WhoWord(Who), Wall - PhaseAt, LongFrame);
	};
	auto Base = [&]()
	{
		BaseDealt = MatchHitsDealt;
		BaseRefused = MatchHitsRefused;
		BaseHealth = HealthOf(0);
		BaseOther = HealthOf(Target);
		BaseAt = BodyOf(Target);
		if (const FOutlawsDmPlayer* D = Dm.Find(Target)) Until = D->ProtectedUntil;
	};
	auto Finish = [&]()
	{
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT spawngrace host RESULT %s (%d passed, %d failed%s; hits dealt %d, refused %d, done over %d)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"),
			Passes, Fails, bOld ? TEXT(", -OLOldSpawnGrace") : TEXT(""), MatchHitsDealt, MatchHitsRefused, Redos);
		if (Client > 0) TestSayTo(Client, TEXT("sg:result"));
		TestSay(TEXT("quit"));
		TestStep = StepQuit;
		TestAt = Now + 3.0;
	};
	if (TestStep < StepWait) { TestStep = StepWait; TestAt = 0.0; }
	// the flash as this machine shows it, all the way through
	MaxHeld = FMath::Max(MaxHeld, P->HeldGraceShown());
	if (const AOutlawsRemotePlayer* R = PuppetOf(Bot))
	{
		if (const AOutlawsSpriteActor* Sp = R->ShownSprite()) MaxSprite = FMath::Max(MaxSprite, Sp->FlashShown());
	}
	switch (TestStep)
	{
	case StepWait:
	{
		TArray<AOutlawsCoopLink*> Links;
		GetLinks(Links);
		int32 Count = 0;
		Client = Bot = -1;
		for (const AOutlawsCoopLink* K : Links)
		{
			const int32 Who = K->PlayerIndex;
			if (Who <= 0 || Who >= MaxPlayers || !MatchInRules[Who]) continue;
			if (K->bBot) { ++Count; Bot = Who; }
			else if (Client < 0) Client = Who;
		}
		if (TestAt == 0.0) TestAt = Now + 240.0;
		if ((Count < 2 || Client < 0 || !bMatchSetUp) && Now < TestAt) return;
		Check(Count >= 2 && Client > 0, FString::Printf(TEXT("the match has %d bots and %s"), Count, Client > 0 ? TEXT("the client") : TEXT("no client")));
		if (Count == 0 || Client < 0) { Finish(); return; }
		F.HostStart();
		TestStep = StepLive;
		return;
	}
	case StepLive:
	{
		// part 1: all at the spawn points as the match goes live
		const FOutlawsDmPlayer* D = Dm.Find(0);
		if (F.GetState() != FOutlawsMatchFlow::EState::Playing || Dm.GetPhase() != EOutlawsDmPhase::Live || !D || Wall >= D->ProtectedUntil) return;
		for (int32 Who : { 0, Client, Bot })
		{
			if (const FOutlawsDmPlayer* Dw = Dm.Find(Who)) UntilOf[Who] = Dw->ProtectedUntil;
		}
		Next(0);
		TestStep = OLFlags::Value(TEXT("OLSpawnGraceSticks"), StickRounds) && StickRounds > 0 ? StepSticks : StepStart;
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT spawngrace live: the client the %s, the bot the %s%s"), WhoWord(Client), WhoWord(Bot), bOld ? TEXT(" (-OLOldSpawnGrace)") : TEXT(""));
		return;
	}
	case StepStart:
	{
		const int32 Three[3] = { 0, Client, Bot };
		for (int32 Who : Three)
		{
			const AOutlawsCoopLink* K = GetLink(Who);
			if (K && K->bSpawnGrace) OffAt[Who] = Wall;             // (the last moment it was seen on)
		}
		if (!bCheckedOn && Wall - PhaseAt >= 1.0)
		{
			bCheckedOn = true;
			const bool bAll = SpawnGraceOf(0) && SpawnGraceOf(Client) && SpawnGraceOf(Bot);
			Check(bOld ? !SpawnGraceOf(0) && !SpawnGraceOf(Client) && !SpawnGraceOf(Bot) : bAll, FString::Printf(TEXT("at 1 s the flag on the host %d, the client %d, the bot %d"), SpawnGraceOf(0), SpawnGraceOf(Client), SpawnGraceOf(Bot)));
			TestSayTo(Client, TEXT("sg:report"));
		}
		if (Wall < FMath::Max3(UntilOf[0], UntilOf[Client], UntilOf[Bot]) + 0.7) return;
		for (int32 Who : Three)
		{
			const double Late = OffAt[Who] - UntilOf[Who];
			Check(bOld ? OffAt[Who] == 0.0 : Late >= -0.25 && Late <= 0.05, FString::Printf(TEXT("the %s: the flag last on %.2f s from the protection's end, %.2f s left at the first look"), WhoWord(Who), Late, UntilOf[Who] - PhaseAt));
		}
		Check(bOld ? MaxHeld == 0.f : MaxHeld >= 0.8f, FString::Printf(TEXT("the host's hands flashed, to %.2f of the top"), MaxHeld));
		Check(bOld ? MaxSprite == 0.f : MaxSprite >= 0.8f, FString::Printf(TEXT("the bot's sprite flashed here, to %.2f of the top"), MaxSprite));
		TestSayTo(Client, TEXT("sg:report"));
		Part = 0;
		Target = Bot;
		Next(0);
		TestStep = StepVictims;
		return;
	}
	case StepVictims:
	{
		// part 2: a protected victim, the host past its own protection
		switch (Phase)
		{
		case 0:
			if (Wall - PhaseAt < 1.0) return;
			Kill(Target);
			Next(1);
			return;
		case 1:
		{
			if (!Ready(Target))
			{
				if (Missed(Target)) { Redo(Target, TEXT("a victim's window went by")); Kill(Target); Next(1); return; }
				if (Wall - PhaseAt > 20.0) { Check(false, FString::Printf(TEXT("the %s back fresh"), WhoWord(Target))); Finish(); }
				return;
			}
			Base();
			Beside(Target);
			const int32 Blood = L->BloodSpawns;
			SpawnGraceBlows(L, P, Target, false);
			if (!bOld) Check(L->BloodSpawns == Blood, FString::Printf(TEXT("the protected %s: no blood from blows that deal nothing (%d spatters asked for)"), WhoWord(Target), L->BloodSpawns - Blood));
			Next(2);
			return;
		}
		case 2:
			if (Wall - PhaseAt < 0.4) return;
			Check(MatchHitsDealt == BaseDealt && HealthOf(Target) == BaseOther && MatchHitsRefused >= BaseRefused + 3, FString::Printf(TEXT("the protected %s: a shot, a fist and a knife dealt %d, refused %d, health %.1f -> %.1f"),
				WhoWord(Target), MatchHitsDealt - BaseDealt, MatchHitsRefused - BaseRefused, BaseOther, HealthOf(Target)));
			Away(Target);
			Next(3);
			return;
		case 3:
		{
			// the host's stick at its feet, the host out of its reach
			Check(FVector::Dist(P->GetActorLocation(), BodyOf(Target)) > L->BlastRadiusCm(), TEXT("the host out of the stick's reach"));
			BaseDealt = MatchHitsDealt;
			BaseRefused = MatchHitsRefused;
			if (const AOutlawsRemotePlayer* R = PuppetOf(Target)) L->Explode(R->ShownBody() - FVector(0.0, 0.0, 60.0));
			Next(4);
			return;
		}
		case 4:
			if (Wall - PhaseAt < 0.5) return;
			Check(MatchHitsDealt == BaseDealt && HealthOf(Target) == BaseOther && (bOld || FVector::Dist(BodyOf(Target), BaseAt) < 30.0), FString::Printf(TEXT("the protected %s: a stick at its feet dealt %d, health %.1f -> %.1f, moved %.0f cm"),
				WhoWord(Target), MatchHitsDealt - BaseDealt, BaseOther, HealthOf(Target), FVector::Dist(BodyOf(Target), BaseAt)));
			Beside(Target);
			Next(5);
			return;
		case 5:
			if (Wall < Until - 0.45) return;      // (StrikePlayer's cool-down is 0.4 s: this fist must not swallow the next)
			BaseDealt = MatchHitsDealt;
			StrikeAtPlayers(L, BodyOf(Target) + FVector(0.0, 0.0, 20.0), 22.f, 0.8f, TEXT("a fist"));
			Check(MatchHitsDealt == BaseDealt, FString::Printf(TEXT("the %s, %.2f s before the end: a fist refused"), WhoWord(Target), Until - Wall));
			Next(6);
			return;
		case 6:
		{
			if (Wall < Until + 0.05) return;
			BaseDealt = MatchHitsDealt;
			const int32 Blood = L->BloodSpawns;
			StrikeAtPlayers(L, BodyOf(Target) + FVector(0.0, 0.0, 20.0), 22.f, 0.8f, TEXT("a fist"));
			Check(MatchHitsDealt == BaseDealt + 1, FString::Printf(TEXT("the %s, %.2f s after the end: a fist dealt"), WhoWord(Target), Wall - Until));
			Check(L->BloodSpawns > Blood, FString::Printf(TEXT("the %s: that fist drew blood (%d spatters asked for)"), WhoWord(Target), L->BloodSpawns - Blood));
			Next(7);
			return;
		}
		default:
			if (Wall - PhaseAt < 0.6) return;
			Check(DeadOf(Target) || HealthOf(Target) < BaseOther, FString::Printf(TEXT("the %s: health %.1f -> %.1f once it ended"), WhoWord(Target), BaseOther, HealthOf(Target)));
			if (Part++ == 0) { Target = Client; Next(0); return; }
			Part = 0;
			Target = Client;
			Next(0);
			TestStep = StepWindow;
			return;
		}
	}
	case StepWindow:
	{
		// part 2, the case Ready waits out: a stick beside the client's spawn point, set off in the very call that sends its respawn.
		// The respawn and the blast leave together, the flag a net update behind.  The client measures it (TickClientSpawnGraceTest)
		const FOutlawsDmPlayer* D = Dm.Find(Client);
		switch (Phase)
		{
		case 0:
			if (Wall - PhaseAt < 1.0) return;
			if (!bWindowHooked)
			{
				bWindowHooked = true;
				TFunction<void(const FOutlawsDmPlayer&, const FOutlawsDmSpawn&)> Sends = Dm.OnRespawn;
				Dm.OnRespawn = [this, Sends](const FOutlawsDmPlayer& Who, const FOutlawsDmSpawn& At)
				{
					Sends(Who, At);
					AOutlawsLevelActor* Lv = FindLevel();
					AOutlawsPlayer* Me = LocalPlayer();
					if (!bWindowArmed || Who.Id != Client || !Lv || !Me) return;
					bWindowArmed = false;
					double Best = -1.0;
					FVector Far = Me->GetActorLocation();
					for (const FOutlawsDmSpawn& Sp : Dm.GetSpawns())
					{
						const double Off = FVector::Dist(Sp.Location, At.Location);
						if (Off > Best) { Best = Off; Far = Sp.Location; }
					}
					Me->SetActorLocation(Far, false, nullptr, ETeleportType::TeleportPhysics);
					Check(Best > Lv->BlastRadiusCm(), FString::Printf(TEXT("the host %.0f cm from the stick on the client's spawn point"), Best));
					Lv->Explode(At.Location + FVector(40.0, 0.0, -60.0));
				};
			}
			bWindowArmed = true;
			TestSayTo(Client, TEXT("sg:window"));
			Next(1);
			return;
		case 1:
			if (bWindowArmed) { if (Wall - PhaseAt > 20.0) { bWindowArmed = false; Check(false, TEXT("the client back up on the stick")); Finish(); } return; }
			Next(2);
			return;
		default:
			if (!D || !D->bAlive || MatchDead[Client] || Wall < D->ProtectedUntil + 0.3)
			{
				if (Wall - PhaseAt > 20.0) { Check(false, TEXT("the client up again after the stick")); Finish(); }
				return;
			}
			if (!bOld) Check(HealthOf(Client) >= 9.99f, FString::Printf(TEXT("the client up on a stick kept its health (%.1f)"), HealthOf(Client)));
			if (++Part < WindowTries) { Next(0); return; }
			Part = 0;
			Target = Client;
			Next(0);
			TestStep = StepStrikers;
			return;
		}
	}
	case StepStrikers:
	{
		// part 3: a protected striker (Target: the client, the host, a bot) on the host, or the host's on the client
		const bool bHostStrikes = Target == 0;
		const int32 Struck = bHostStrikes ? Client : 0;
		switch (Phase)
		{
		case 0:
			if (Wall - PhaseAt < 1.0) return;
			Kill(Target);
			Next(1);
			return;
		case 1:
		{
			if (!Ready(Target))
			{
				if (Missed(Target)) { Redo(Target, TEXT("a striker's window went by")); Kill(Target); Next(1); return; }
				if (Wall - PhaseAt > 20.0) { Check(false, FString::Printf(TEXT("the striker %s back fresh"), WhoWord(Target))); Finish(); }
				return;
			}
			if (!Fresh(Client) && bHostStrikes && Wall - PhaseAt < 2.0) return;
			Beside(bHostStrikes ? Client : Target);
			Base();
			BaseOther = HealthOf(Struck);
			BaseAt = BodyOf(Struck);
			if (bHostStrikes)
			{
				// its gun, fist, knife and stick on the client, the stick near enough for itself, and a hurt of its own
				const int32 Blood = L->BloodSpawns;
				SpawnGraceBlows(L, P, Client, true);
				L->DamagePlayer(2.f, 1.f, true);
				if (!bOld) Check(L->BloodSpawns == Blood, FString::Printf(TEXT("the protected host's blows drew no blood (%d spatters asked for)"), L->BloodSpawns - Blood));
			}
			else if (IsBot(Target))
			{
				const FBot* B = FindBot(Target);
				AOutlawsEnemy* E = B ? B->Brain.Get() : nullptr;
				if (E) { E->CoopTarget = 0; BotPellet(E, 2.f, P->GetActorLocation(), BodyOf(Target), 0); }
				bool bHarmless = false;      // (as TickBotThrown has it: a stick judged harmless sends nothing to the rules)
				{
					TGuardValue<int32> By(BlastShooter, Target);
					BlastHostDealt = -1.f;
					BlastStruckMask = 0;
					L->Explode(P->GetActorLocation() - FVector(0.0, 0.0, 60.0));
					bHarmless = SpawnStickHarmless(L);
				}
				if (BlastHostDealt > 0.f && !bHarmless) OnMatchHit(Target, LevelEpoch(), 0, BlastHostDealt, P->GetActorLocation(), BodyOf(Target), true);
				BlastHostDealt = -1.f;
				BlastStruckMask = 0;
			}
			else TestSayTo(Target, TEXT("sg:fire"));
			Next(2);
			return;
		}
		case 2:
		{
			if (Wall - PhaseAt < 0.7) return;
			const FOutlawsDmPlayer* D = Dm.Find(Target);
			const float H = HealthOf(Struck);
			const bool bHarmless = H == BaseOther && !DeadOf(Struck) && MatchHitsDealt == BaseDealt && FVector::Dist(BodyOf(Struck), BaseAt) < 30.0;
			Check(bOld ? !bHarmless || DeadOf(Struck) : bHarmless, FString::Printf(TEXT("the protected %s struck the %s: dealt %d, health %.1f -> %.1f, moved %.0f cm"), WhoWord(Target), WhoWord(Struck), MatchHitsDealt - BaseDealt, BaseOther, H, FVector::Dist(BodyOf(Struck), BaseAt)));
			if (!IsBot(Target))
			{
				Check(bOld ? D && D->ProtectedUntil < Until - 1.0 : D && D->ProtectedUntil == Until && SpawnGraceOf(Target), FString::Printf(TEXT("the %s's own shots %s its protection (%.1f s left, flag %d)"), WhoWord(Target), bOld ? TEXT("end") : TEXT("leave"), D ? D->ProtectedUntil - Wall : 0.0, SpawnGraceOf(Target)));
			}
			if (bHostStrikes && !bOld) Check(L->GetHealth() == BaseHealth && P->GetVelocity().Size() < 100.0, FString::Printf(TEXT("the host's own stick and a hurt: health %.1f -> %.1f, speed %.0f"), BaseHealth, L->GetHealth(), P->GetVelocity().Size()));
			if (Target == Client && !bOld) Check(HealthOf(Client) >= 9.99f && !DeadOf(Client), FString::Printf(TEXT("the client's own stick beside it left it whole (%.1f)"), HealthOf(Client)));
			Next(3);
			return;
		}
		case 3:
		{
			if (Wall < Until + 0.2) return;
			BaseOther = HealthOf(Struck);
			BaseDealt = MatchHitsDealt;
			if (bHostStrikes)
			{
				const int32 Blood = L->BloodSpawns;
				StrikeAtPlayers(L, BodyOf(Client) + FVector(0.0, 0.0, 20.0), 22.f, 0.8f, TEXT("a fist"));
				if (!bOld) Check(L->BloodSpawns > Blood, FString::Printf(TEXT("once it ended the host's fist drew blood (%d spatters asked for)"), L->BloodSpawns - Blood));
				L->DamagePlayer(0.5f, 1.f, true);
			}
			else if (IsBot(Target))
			{
				const FBot* B = FindBot(Target);
				AOutlawsEnemy* E = B ? B->Brain.Get() : nullptr;
				if (E) { E->CoopTarget = 0; BotPellet(E, 2.f, P->GetActorLocation(), BodyOf(Target), 0); }
			}
			else TestSayTo(Target, TEXT("sg:shot"));
			Next(4);
			return;
		}
		default:
			if (Wall - PhaseAt < 0.8) return;
			// (-OLOldSpawnGrace: its first blows dealt and killed, so the struck is back in its own protection by now: nothing to read)
			if (!bOld) Check(DeadOf(Struck) || HealthOf(Struck) < BaseOther, FString::Printf(TEXT("once it ended the %s's hit deals: the %s's health %.1f -> %.1f"), WhoWord(Target), WhoWord(Struck), BaseOther, HealthOf(Struck)));
			if (bHostStrikes && !bOld) Check(L->GetHealth() < BaseHealth, FString::Printf(TEXT("the host takes a hurt again (%.1f -> %.1f)"), BaseHealth, L->GetHealth()));
			Target = Target == Client ? 0 : Target == 0 ? Bot : -1;
			Next(0);
			if (Target < 0) { if (bOld) Finish(); else { Part = 0; Phase = 0; PhaseAt = Wall; TestStep = StepSticks; } }
			return;
		}
	}
	case StepSticks:
	{
		// part 4 (REQ-SPAWNSTICK): a stick is judged at its throw.  Each thrower in turn, the bot, the client, the host (real throws, the
		// bot's on its own list), its stick at the feet of someone unprotected: thrown after its protection ended, full; thrown before a
		// death and set off in the protection after the respawn, full (the host stands beside it: it is neither hurt nor thrown); thrown
		// in its protection and going off after it ended, nothing, nobody thrown.  -OLOldSpawnStick: the last two the other way
		const int32 Th = StickThrower == 0 ? Bot : StickThrower == 1 ? Client : 0;
		const int32 Vi = Th == 0 ? Bot : 0;
		const FVector Feet = BodyOf(Vi) - FVector(0.0, 0.0, 60.0);
		auto Who = [&]() { return FString(Th == 0 ? TEXT("the host") : Th == Client ? TEXT("the client") : TEXT("the bot")); };
		auto Stout = [&]()
		{
			if (Vi == 0) L->TestSetHealth(1000.f);
			else if (FBot* B = FindBot(Vi)) B->Health = 1000.f;
		};
		auto BaseStick = [&]()
		{
			BaseDealt = MatchHitsDealt;
			BaseBlasts = L->BlastsSeen;
			BaseOther = HealthOf(Vi);
			BaseHealth = HealthOf(0);
			BaseAt = BodyOf(Vi);
			bBlastSeen = false;
		};
		auto Put = [&](const FVector& At, double Fuse)
		{
			if (Th == 0) SpawnGraceTestStick(P, At, float(Fuse));
			else if (Th == Client) TestSayTo(Client, TEXT("sg:stick"), FMath::RoundToInt(Fuse * 1000.0));
			else TestBotStick(Bot, At, Fuse);
		};
		auto Go = [&](double Fuse)
		{
			if (Th == 0) SpawnGraceTestStickGo(P, float(Fuse));
			else if (Th == Client) TestSayTo(Client, TEXT("sg:stickgo"), FMath::RoundToInt(Fuse * 1000.0));
			else TestBotStick(Bot, FVector::ZeroVector, Fuse);
		};
		auto Settled = [&]()
		{
			// the victim's body still (a blast's throw over) and on the ground, and for the client's stick long enough for the puppet it
			// is laid under to be down too (-OLOldSpawnStick: its stick went under the floor, 3.3 m under the host, right after the bot's)
			const FVector At = BodyOf(Vi);
			if (FVector::Dist(At, StickLast) > 3.0 || (Vi == 0 && !P->GetCharacterMovement()->IsMovingOnGround())) { StickLast = At; StickStillAt = Wall; }
			return Wall - StickStillAt >= (Th == Client ? 1.0 : 0.4) && !DeadOf(Vi);
		};
		auto Took = [&]() { return FString::Printf(TEXT("%d blast, dealt %d, health %.1f -> %.1f, moved %.0f cm, the thrower's flag %d"), L->BlastsSeen - BaseBlasts, MatchHitsDealt - BaseDealt, BaseOther, HealthOf(Vi), FVector::Dist(BodyOf(Vi), BaseAt), SpawnGraceOf(Th)); };
		const bool bOldStick = OldSpawnStick();
		const FOutlawsDmPlayer* DT = Dm.Find(Th);
		if (!bBlastSeen && L->BlastsSeen > BaseBlasts) { bBlastSeen = true; bFlagAtBlast = SpawnGraceOf(Th); BlastSeenAt = Wall; }
		switch (Phase)
		{
		case 0:
			// everyone up, past any protection
			if (L->IsPlayerDead() || !DT || !DT->bAlive || DeadOf(Vi) || Wall < DT->ProtectedUntil + 0.2 || SpawnGraceOf(Th) || SpawnGraceOf(Vi) || !Settled())
			{
				if (Wall - PhaseAt > 25.0) { Check(false, FString::Printf(TEXT("%s and its victim up for the sticks"), *Who())); Finish(); }
				return;
			}
			Stout();
			BaseStick();
			Put(Feet, 0.3);
			Next(1);
			return;
		case 1:
			// (the blast's word, not a fixed wait: a client that stalled had the step 1.09 s late, its host's last blast 1.69 s late, measured, past 0.9 s)
			if (Wall - PhaseAt < 0.9 || ((L->BlastsSeen == BaseBlasts || MatchHitsDealt == BaseDealt) && Wall - PhaseAt < 5.0)) return;
			Check(L->BlastsSeen > BaseBlasts && MatchHitsDealt > BaseDealt && HealthOf(Vi) < BaseOther, FString::Printf(TEXT("thrown by %s after its protection, off 0.3 s on: %s"), *Who(), *Took()));
			Next(2);
			return;
		case 2:
			if (!Settled()) { if (Wall - PhaseAt > 25.0) { Check(false, TEXT("the victim down after the first stick")); Finish(); } return; }
			Stout();
			Put(Feet, 60.0);
			StickFeet = Feet;
			Kill(Th);
			Next(3);
			return;
		case 3:
			if (Wall - PhaseAt < 1.0) return;      // (the kill's word lands first: dying in a protection, it would read as fresh until then)
			if (const FOutlawsDmPlayer* DW = Dm.Find(Th); DW && FMath::Fmod(Wall - PhaseAt, 0.25) < 0.02)
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLSHOT spawngrace %s waits: +%.2f s, alive %d, left %.2f, match dead %d, dead %d, health %.2f, flag %d"), *Who(), Wall - PhaseAt, DW->bAlive, DW->ProtectedUntil - Wall, MatchDead[Th], DeadOf(Th), HealthOf(Th), SpawnGraceOf(Th));
			if (Th == 0 && DT && DT->bAlive && SpawnGraceOf(Th) && !bStallDone && OLFlags::Value(TEXT("OLSpawnGraceStall"), StallMs) && StallMs > 0)
			{
				bStallDone = true;
				FPlatformProcess::Sleep(float(StallMs) / 1000.f);
				return;
			}
			if (!Ready(Th))
			{
				if (Missed(Th)) { Redo(Th, TEXT("a thrower's window went by, its stick lit")); Kill(Th); Next(3); return; }
				if (Wall - PhaseAt > 25.0) { Check(false, FString::Printf(TEXT("%s back fresh with its stick lit"), *Who())); Finish(); }
				return;
			}
			Stout();
			BaseStick();
			if (Th == 0)
			{
				// (the thrower beside its own stick, its flash on: neither hurt nor thrown.  1.2 m to a clear side: over the victim it lands
				// on its head and is pushed off 70 cm, capsule by capsule)
				FVector Spot = StickFeet + FVector(0.0, 0.0, 150.0);
				FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsSpawnGraceSpot), false, P);
				for (int32 a = 0; a < 8; ++a)
				{
					const FVector To = Spot + FRotator(0.0, 45.0 * a, 0.0).Vector() * 120.0;
					if (!P->GetWorld()->LineTraceTestByChannel(Spot, To, ECC_Pawn, Q)) { Spot = To; break; }
				}
				P->SetActorLocation(Spot, false, nullptr, ETeleportType::TeleportPhysics);
				P->GetCharacterMovement()->StopMovementImmediately();
				BaseSelf = P->GetActorLocation();
				L->TestSetHealth(10.f);
			}
			Go(0.15);
			Next(4);
			return;
		case 4:
			if (Th == 0 && L->BlastsSeen == BaseBlasts) BaseSelf = P->GetActorLocation();      // (from the blast on: the tick after the teleport can shift the body 70 cm, measured, before any blast)
			if (Th == 0) UE_LOG(LogOutlawsCoop, Log, TEXT("OLSHOT spawngrace the host by its stick: +%.2f s, %s, speed %s, %d blast"), Wall - PhaseAt, *(P->GetActorLocation() - BaseSelf).ToCompactString(), *P->GetVelocity().ToCompactString(), L->BlastsSeen - BaseBlasts);
			// (the blast's word and the flag as it landed, not a look at 0.8 s; a stall that took the blast past the protection tries nothing:
			// the case wants a blast in it, so it is done over from the throw)
			if (Wall - PhaseAt < 0.8 || (!bBlastSeen && Wall - PhaseAt < 5.0) || (bBlastSeen && Wall - BlastSeenAt < 0.6)) return;
			if (bBlastSeen && !bFlagAtBlast && Redos < RedoMost && !OLFlags::Param(TEXT("OLSpawnGraceNoRedo")))
			{
				Redo(Th, TEXT("the stick's blast came after the protection"));
				Next(2);
				return;
			}
			Check(bBlastSeen && bFlagAtBlast && (bOldStick ? MatchHitsDealt == BaseDealt && HealthOf(Vi) == BaseOther : MatchHitsDealt > BaseDealt && HealthOf(Vi) < BaseOther),
				FString::Printf(TEXT("thrown by %s before its death, off in its protection: %s"), *Who(), *Took()));
			if (Th == 0)
			{
				// (put 150 cm over the stick's feet it settles straight down: a throw is sideways or up)
				const double Moved = FVector::Dist2D(P->GetActorLocation(), BaseSelf), Rose = P->GetActorLocation().Z - BaseSelf.Z;
				Check(L->GetHealth() == 10.f && Moved < 60.0 && Rose < 60.0, FString::Printf(TEXT("the protected host beside its own stick: health %.1f, moved %.0f cm across, %.0f cm up"), L->GetHealth(), Moved, Rose));
			}
			Next(5);
			return;
		case 5:
			if (!Settled()) { if (Wall - PhaseAt > 25.0) { Check(false, TEXT("the victim down after the second stick")); Finish(); } return; }
			Kill(Th);
			Next(6);
			return;
		case 6:
			if (Wall - PhaseAt < 1.0) return;      // (as case 3: case 4's thrower dies here still in the protection it came back to)
			if (!Ready(Th))
			{
				if (Missed(Th)) { Redo(Th, TEXT("a thrower's window went by, for its last stick")); Kill(Th); Next(6); return; }
				if (Wall - PhaseAt > 25.0) { Check(false, FString::Printf(TEXT("%s back fresh for its last stick"), *Who())); Finish(); }
				return;
			}
			Stout();
			if (DT) Until = DT->ProtectedUntil;
			BaseStick();
			Put(Feet, Until + 0.3 - Wall);
			Next(7);
			return;
		default:
			if (Wall < Until + 0.9 || (!bBlastSeen && Wall < Until + 5.0) || (bBlastSeen && Wall - BlastSeenAt < 0.6)) return;
			Check(L->BlastsSeen > BaseBlasts && !SpawnGraceOf(Th) && (bOldStick ? MatchHitsDealt > BaseDealt : MatchHitsDealt == BaseDealt && HealthOf(Vi) == BaseOther && FVector::Dist(BodyOf(Vi), BaseAt) < 30.0),
				FString::Printf(TEXT("thrown by %s in its protection, off 0.3 s after it: %s"), *Who(), *Took()));
			if (++StickThrower > 2)
			{
				if (--StickRounds > 0) { StickThrower = 0; Next(0); return; }
				if (StickRounds == 0) { Finish(); return; }
				L->TestSetHealth(10.f);
				if (FBot* B = FindBot(Bot)) B->Health = 10.f;
				Part = 0;
				Next(0);
				TestStep = StepHold;
				return;
			}
			Next(0);
			return;
		}
	}
	case StepHold:
	{
		// part 5: the bot and the host held in their protection: the flat shots, the host's hands up with the bot in view, and a joiner
		// comes in on it
		FOutlawsDmPlayer* DB = Dm.Find(Bot);
		FOutlawsDmPlayer* DH = Dm.Find(0);
		if (!DB || !DH) { Finish(); return; }
		if (Phase >= 2 && Phase <= 5)
		{
			// (a desktop has no tracked hands: both grips put before the eyes, as -OLBodyTest holds them, the view on the bot, and the
			// hand pictures shown, a headset's only otherwise)
			P->CoopHandBox(0)->SetVisibility(true, true);
			P->CoopHandBox(1)->SetVisibility(true, true);
			const FVector Eye = P->Camera->GetComponentLocation();
			const FVector To = (BodyOf(Bot) + FVector(0.0, 0.0, 20.0) - Eye).GetSafeNormal();
			if (AController* Ct = P->GetController()) Ct->SetControlRotation(To.Rotation());
			const FVector Side = FVector::CrossProduct(FVector::UpVector, To).GetSafeNormal();
			P->GripOf(1)->SetWorldLocationAndRotation(Eye + To * 45.0 + Side * 22.0 - FVector(0.0, 0.0, 14.0), To.Rotation());
			P->GripOf(0)->SetWorldLocationAndRotation(Eye + To * 45.0 - Side * 22.0 - FVector(0.0, 0.0, 14.0), To.Rotation());
		}
		switch (Phase)
		{
		case 0:
			if (L->IsPlayerDead()) { if (Wall - PhaseAt > 20.0) { Check(false, TEXT("the host up for the shots")); Finish(); } return; }
			Kill(Bot);
			Next(1);
			return;
		case 1:
			if (!Ready(Bot) || L->IsPlayerDead())
			{
				if (Missed(Bot)) { Redo(Bot, TEXT("the bot's window went by")); Kill(Bot); Next(1); return; }
				if (Wall - PhaseAt > 20.0) { Check(false, TEXT("the bot back fresh for the shots")); Finish(); }
				return;
			}
			DB->ProtectedUntil = Wall + 150.0;
			DH->ProtectedUntil = Wall + 150.0;
			Beside(Bot);
			Next(2);
			return;
		case 2:
			if (Wall - PhaseAt < 1.5) return;
			if (!bShotTop && SpawnGracePulse() > 0.85f) { bShotTop = true; Shot(TEXT("ol_spawngrace_flash_top")); }
			else if (bShotTop && !bShotLow && SpawnGracePulse() < 0.03f) { bShotLow = true; Shot(TEXT("ol_spawngrace_flash_low")); }
			if (!bShotLow) return;
			Next(3);
			return;
		case 3:
			if (Wall - PhaseAt < 1.0) return;
			UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT spawngrace joiner now"));
			Next(4);
			return;
		case 4:
		{
			const FString* Saw = TestNotes.FindByPredicate([](const FString& N) { return N.StartsWith(TEXT("sg joiner")); });
			if (!Saw && Wall - PhaseAt < 120.0) return;
			Check(Saw != nullptr, Saw ? *Saw : FString(TEXT("no word from the joiner")));
			DB->ProtectedUntil = Wall;
			DH->ProtectedUntil = Wall;
			Next(5);
			return;
		}
		case 5:
			if (Wall - PhaseAt < 0.8) return;
			Check(!SpawnGraceOf(Bot) && !SpawnGraceOf(0), TEXT("the held protection over: the flags off"));
			Shot(TEXT("ol_spawngrace_flash_after"));
			Next(6);
			return;
		default:
		{
			if (Wall - PhaseAt < 1.0) return;
			const float Left = PuppetOf(Bot) && PuppetOf(Bot)->ShownSprite() ? PuppetOf(Bot)->ShownSprite()->FlashShown() : -1.f;
			Check(Left == 0.f && P->HeldGraceShown() == 0.f, FString::Printf(TEXT("after it, no flash left on the sprite (%.2f) or the hands (%.2f)"), Left, P->HeldGraceShown()));
			Finish();
			return;
		}
		}
	}
	case StepQuit:
		if (Now < TestAt) return;
		TestStep = StepDone;
		QuitForTest();
		return;
	default:
		return;
	}
}

void UOutlawsCoop::TickClientSpawnGraceTest(AOutlawsLevelActor* L)
{
	using namespace OutlawsSpawnGraceProbeLocal;
	AOutlawsPlayer* P = LocalPlayer();
	if (!L || !P) return;
	const double Wall = FPlatformTime::Seconds();
	if (!IsMatch())
	{
		if (PhaseAt == 0.0) PhaseAt = Wall;
		if (bSaid || Wall - PhaseAt < 3.0) return;
		bSaid = true;
		const float H = L->GetHealth();
		L->DamagePlayer(1.f, 1.f, true);
		Check(!SpawnGraceOf(MyIndex()) && SpawnGraceFlashHere(L) == 0.f, TEXT("co-op: this client not protected, no flash"));
		Check(L->GetHealth() < H, FString::Printf(TEXT("co-op: the client's first hurt is taken (%.1f -> %.1f)"), H, L->GetHealth()));
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT spawngrace coop client RESULT %s (%d passed, %d failed)"), Fails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		if (AOutlawsCoopLink* Mine = GetLink(MyIndex())) Mine->ServerTestNote(TEXT("sg coop done"));
		return;
	}
	// what this machine shows: its own hands, every sprite here
	const bool bMine = SpawnGraceOf(MyIndex());
	if (bMine && !bClientFlag) GraceOnAt = Wall;
	if (!bMine && bClientFlag) { ClientOnFor = Wall - GraceOnAt; UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT spawngrace client: its own flag on for %.2f s"), ClientOnFor); }
	bClientFlag = bMine;
	MaxHeld = FMath::Max(MaxHeld, P->HeldGraceShown());
	// up on a stick (the host's StepWindow): how far the blast moved this body, from its first frame alive
	if (bWindowWatch && (WindowUpAt > 0.0 || !L->bDead))
	{
		if (WindowUpAt == 0.0) { WindowUpAt = Wall; WindowFrom = P->GetActorLocation(); }
		WindowMoved = FMath::Max(WindowMoved, float(FVector::Dist(P->GetActorLocation(), WindowFrom)));
		WindowSpeed = FMath::Max(WindowSpeed, float(P->GetVelocity().Size()));
		if (Wall - WindowUpAt >= 2.0)
		{
			bWindowWatch = false;
			const int32 Ahead = GraceAheadOfFlag - WindowShields;
			const FString Saw = FString::Printf(TEXT("the blast here %d, shielded ahead of the flag %d, moved %.0f cm, top speed %.0f cm/s"), L->BlastsSeen - WindowBlasts, Ahead, WindowMoved, WindowSpeed);
			if (OLFlags::Param(TEXT("OLOldSpawnGrace"))) Check(L->BlastsSeen > WindowBlasts && WindowMoved >= 300.f, FString::Printf(TEXT("-OLOldSpawnGrace: the client up on a stick is thrown: %s"), *Saw));
			else Check(L->BlastsSeen > WindowBlasts && Ahead >= 1 && WindowMoved < 60.f && WindowSpeed < 1000.f, FString::Printf(TEXT("the client up on a stick is not thrown: %s"), *Saw));
			if (L->bDead) P->RequestRespawn(false);      // (thrown, the fall killed it)
		}
	}
	const bool bJoiner =OLFlags::Param(TEXT("OLSpawnGraceJoiner"));
	for (int32 Who = 0; Who < MaxPlayers; ++Who)
	{
		const AOutlawsRemotePlayer* R = PuppetOf(Who);
		const AOutlawsSpriteActor* Sp = R ? R->ShownSprite() : nullptr;
		const AOutlawsCoopLink* K = GetLink(Who);
		if (!Sp || !K || Who == MyIndex()) continue;
		MaxSprite = FMath::Max(MaxSprite, Sp->FlashShown());
		if (bJoiner && !bSaid && K->bSpawnGrace && K->bBot && Sp->FlashShown() > 0.5f && !R->IsHidden())
		{
			bSaid = true;
			const FString Note = FString::Printf(TEXT("sg joiner saw the %s's sprite flash at %.2f of the top"), WhoWord(Who), Sp->FlashShown());
			UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT spawngrace joiner RESULT ALL PASS (%s)"), *Note);
			if (AOutlawsCoopLink* Mine = GetLink(MyIndex())) Mine->ServerTestNote(Note);
		}
	}
}

void UOutlawsCoop::OnSpawnGraceTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	using namespace OutlawsSpawnGraceProbeLocal;
	if (Step == TEXT("die") || Step == TEXT("window"))
	{
		if (Step == TEXT("window"))
		{
			bWindowWatch = true;
			WindowUpAt = 0.0;
			WindowMoved = WindowSpeed = 0.f;
			WindowShields = GraceAheadOfFlag;
			WindowBlasts = L->BlastsSeen;
		}
		L->DamagePlayer(1e6f, 1.f, true);
		TWeakObjectPtr<AOutlawsPlayer> WeakP(P);
		FTimerHandle H;
		P->GetWorldTimerManager().SetTimer(H, FTimerDelegate::CreateWeakLambda(this, [WeakP]() { if (AOutlawsPlayer* Pl = WeakP.Get()) Pl->RequestRespawn(false); }), 2.3f, false);
	}
	else if (Step == TEXT("stick"))
	{
		// (a real throw of this machine's, the stick laid at the host's feet as this machine draws them)
		const AOutlawsRemotePlayer* R = PuppetOf(0);
		if (R) SpawnGraceTestStick(P, R->ShownBody() - FVector(0.0, 0.0, 60.0), float(Arg) / 1000.f);
	}
	else if (Step == TEXT("stickgo"))
	{
		SpawnGraceTestStickGo(P, float(Arg) / 1000.f);
	}
	else if (Step == TEXT("fire"))
	{
		const int32 Blood = L->BloodSpawns;
		SpawnGraceBlows(L, P, 0, true);
		if (!OLFlags::Param(TEXT("OLOldSpawnGrace"))) Check(L->BloodSpawns == Blood, FString::Printf(TEXT("the client's blows while protected drew no blood here (%d spatters asked for)"), L->BloodSpawns - Blood));
	}
	else if (Step == TEXT("shot"))
	{
		const AOutlawsRemotePlayer* R = PuppetOf(0);
		const UCameraComponent* Cam = P->Camera.Get();
		const FVector Eye = Cam ? Cam->GetComponentLocation() : P->GetActorLocation();
		FVector At;
		const int32 Blood = L->BloodSpawns;
		if (R) ShotAtPlayers(L, Eye, R->ShownBody() + FVector(0.0, 0.0, 20.0) + (R->ShownBody() - Eye).GetSafeNormal() * 300.0, 2.f, At);
		Check(L->BloodSpawns > Blood, FString::Printf(TEXT("the client's shot once it ended drew blood here (%d spatters asked for)"), L->BloodSpawns - Blood));
	}
	else if (Step == TEXT("report"))
	{
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT spawngrace client: the flash here: its hands to %.2f, a sprite to %.2f; flag now %d, on for %.2f s"), MaxHeld, MaxSprite, bClientFlag, ClientOnFor);
		if (!OLFlags::Param(TEXT("OLOldSpawnGrace")) && bClientFlag == false)
		{
			Check(MaxHeld >= 0.8f && MaxSprite >= 0.8f && ClientOnFor > 2.5 && ClientOnFor < 3.4, FString::Printf(TEXT("the client: its hands flashed to %.2f, a sprite to %.2f, its own flag on for %.2f s"), MaxHeld, MaxSprite, ClientOnFor));
		}
	}
	else if (Step == TEXT("result"))
	{
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT spawngrace client RESULT %s (%d passed, %d failed)"), Fails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
	}
}

#endif // WITH_OUTLAWS_TESTS
