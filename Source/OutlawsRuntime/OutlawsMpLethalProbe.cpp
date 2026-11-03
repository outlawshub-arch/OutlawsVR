// REQ-MPLETHAL's probe (user 3 Nov, multiplayer matches only: two punches kill, a knife's stab kills, a rifle head shot while
// scoped kills; a thrown knife that strikes kills too, by default; unscoped and body shots as before).  Two machines:
//   host:   -OLMpHost=town_2p -OLMpClass=dm -OLMpBots=6 -OLMpBotChars=San,Jam,Two,Doc,Bob,Mar -OLMpChar=Doc -OLMpHostRespawn=2
//           -OLMpMinutes=30 -OLMpFrags=0 -OLMpLethalProbe [-OLOldMpLethal]
//   client: -OLCoopJoin=<host>:<port> -OLMpChar=Mar -OLMpLethalProbe
// The bots hold their fire; every blow is dealt through the game's own calls (StrikeAtPlayers, SweepAtPlayers, ShotAtPlayers,
// the bots' knife's OnMatchHit) with fixed damages (a fist 0.8, a knife 4, a bot's knife 3, the rifle 2), one every 0.7 s
// until the victim dies; the blows it took are checked against the rule's (punches 2, the first leaving 5 health; a stab, a
// thrown knife, a scoped head shot 1; unscoped head and body shots as before) or, -OLOldMpLethal, against the old numbers
// (as DamagePlayer has it: the plate first, then the blow times the victim's DAMAGE_MULTIPLIER).
// 1. Six rounds, one a kind, the host striking every bot (one of each character) and the client (a character a round: Mar,
//    San, Jam, Two, Doc, Bob).
// 2. The co-op pair: the client strikes the host (Dr. Death), each kind, decided on the host; its stab with no knife in its
//    hands is not taken (dealt as before); in 150 armour the host still dies to a stab and to two punches.
// 3. A bot's thrown knife on the host and on the client.
// -OLMpLethalProbe=story -OLShots -OLLevel=hideout -OLNoFrontEnd: a story level: no match, no player struck, the rule off, a
// bad guy's fist, stab and rifle numbers as before.
// Lines "OLSHOT mplethal check PASS|FAIL: ...", "OLSHOT mplethal <blows line>" and "OLSHOT mplethal <host|story> RESULT".

#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsDeathmatch.h"
#include "OutlawsEnemy.h"
#include "OutlawsFlags.h"
#include "OutlawsLab.h"
#include "OutlawsLevelActor.h"
#include "OutlawsMpHost.h"
#include "OutlawsPlayer.h"
#include "OutlawsRemotePlayer.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsShotTests.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsMpLethalProbeLocal
{
	enum EKind : int32 { Punch, Stab, Throw, Scoped, Head, Body, BotKnife, NumKinds };
	const TCHAR* const KindNames[] = { TEXT("punch"), TEXT("stab"), TEXT("throw"), TEXT("scoped"), TEXT("head"), TEXT("body"), TEXT("botknife") };
	const float KindDamage[] = { 0.8f, 4.f, 4.f, 2.f, 2.f, 2.f, 3.f };
	const TCHAR* const ClientChars[] = { TEXT("Mar"), TEXT("San"), TEXT("Jam"), TEXT("Two"), TEXT("Doc"), TEXT("Bob") };
	constexpr double BlowEvery = 0.5;
	constexpr int32 MaxBlows = 40;

	struct FVictim
	{
		int32 Who = -1; FString Char; int32 Blows = 0; bool bDone = false, bDied = false; float After1 = -1.f; int32 Armour = 0;
		float LastHealth = 0.f; int32 LastArmour = 0; double BlowAt = 0.0;      // (before the last blow, and when)
	};
	/** One job of part 2 or 3: Striker (a client or a bot) on Victim, Kind; bBare: no knife in hand for a stab. */
	struct FJob { int32 Kind; bool bClientStrikes; int32 VictimSlot; bool bArmour; bool bBare; };
	TArray<FVictim> Victims;
	int32 Passes = 0, Fails = 0, Round = 0, Job = 0, Phase = 0, Client = -1, Bot = -1, Lines = 0;
	double PhaseAt = 0.0, NextBlowAt = 0.0;
	TArray<FString> Table;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT mplethal check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}

	float MultOf(const AOutlawsLevelActor* L, const FString& Short)
	{
		const FOutlawsMpCharacter* Ch = FOutlawsMpCharacter::Find(Short);
		const FOutlawsCharKit& Kit = FOutlawsCharKit::Of(L ? L->GetLabAssets() : nullptr, Short);
		return Ch ? (Kit.bRead ? Kit.DamageMult : Ch->DamageMult) : 1.f;
	}

	/** Blows of Per (the multiplier on it) to kill from 10 health in Armour, the plate first, as DamagePlayer has it. */
	int32 OldBlows(float Per, int32 Armour)
	{
		float Health = 10.f;
		int32 N = 0;
		while (Health > 0.f && N < 99)
		{
			float D = Per;
			const float Plate = Armour / 10.f;
			if (Plate > 0.f)
			{
				if (Plate >= D) { Armour -= FMath::RoundToInt(D * 10.f); D = 0.f; }
				else { D -= Plate; Armour = 0; }
			}
			Health = FMath::Max(0.f, Health - D);
			++N;
		}
		return N;
	}

	/** The blows the rule wants (bOld: the old numbers). */
	int32 WantBlows(int32 Kind, bool bOld, bool bBare, float Mult, int32 Armour)
	{
		const int32 Was = OldBlows(KindDamage[Kind] * Mult, Armour);
		if (bOld || bBare) return Was;
		switch (Kind)
		{
		case Punch: return 2;
		case Stab: case Throw: case Scoped: case BotKnife: return 1;
		default: return Was;
		}
	}

	/** Player Who's drawn head (the top of the figure; bScoped false: under the capsule's top, as high as an unscoped shot
	 *  meets the body) or chest, as this machine draws it. */
	bool AimPoint(const UOutlawsCoop* C, int32 Who, bool bHead, bool bScoped, FVector& Out)
	{
		const AOutlawsRemotePlayer* R = C->PuppetOf(Who);
		if (!R) return false;
		const double Half = FMath::Max(40.0, double(R->ShownHalf()));
		const FVector Feet = R->ShownBody() - FVector(0.0, 0.0, Half);
		const AOutlawsSpriteActor* Spr = R->ShownSprite();
		const double Tall = Spr && Spr->DrawnHeightCm() > 50.0 ? Spr->DrawnHeightCm() : 2.0 * Half;
		Out = Feet + FVector(0.0, 0.0, !bHead ? Tall * 0.55 : bScoped ? Tall * 0.94 : FMath::Min(Tall * 0.94, 2.0 * Half - 3.0));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLSHOT mplethal aim at the %s: figure %.0f cm, capsule %.0f, %.0f up"), UOutlawsCoop::WhoWord(Who), Tall, 2.0 * Half, Out.Z - Feet.Z);
		return true;
	}
}

// ---- this machine's blows (the host's on any victim, the client's on the host) ---------------------------------------------

static bool OLLethalBlow(UOutlawsCoop* C, AOutlawsLevelActor* L, AOutlawsPlayer* P, int32 Kind, int32 Who, bool bTeleportForShot)
{
	using namespace OutlawsMpLethalProbeLocal;
	const AOutlawsRemotePlayer* R = C->PuppetOf(Who);
	if (!R || !L || !P) return false;
	const FVector Chest = R->ShownBody() + FVector(0.0, 0.0, 20.0);
	const UCameraComponent* Cam = P->Camera.Get();
	const FVector Eye = Cam ? Cam->GetComponentLocation() : P->GetActorLocation();
	switch (Kind)
	{
	case Punch: return UOutlawsCoop::StrikeAtPlayers(L, Chest, 22.f, KindDamage[Punch], TEXT("a fist"));
	case Stab: return UOutlawsCoop::StrikeAtPlayers(L, Chest, 12.f, KindDamage[Stab], TEXT("a knife's jab"));
	case Throw:
	{
		// (the thrower's ServerThrow first, as a knife leaves the hand: the host's check wants it)
		const FVector Dir = (Chest - Eye).GetSafeNormal();
		UOutlawsCoop::NoteThrow(L, 2, Eye, Dir * 1500.0, 1.f, 0.f);
		FVector At;
		return UOutlawsCoop::SweepAtPlayers(L, Chest - Dir * 60.0, Chest + Dir * 60.0, KindDamage[Throw], TEXT("a thrown knife"), At);
	}
	default:
	{
		FVector To;
		if (!AimPoint(C, Who, Kind != Body, Kind == Scoped, To)) return false;
		FVector From = Eye;
		FVector Dir = (To - From).GetSafeNormal();
		if (bTeleportForShot)
		{
			// (the host: from 3 m off the victim, level with the point, the host's body put there: its check wants the
			// muzzle near the shooter, and nobody else on the way)
			FVector Flat = (To - P->GetActorLocation()).GetSafeNormal2D();
			if (Flat.IsNearlyZero()) Flat = FVector::ForwardVector;
			From = To - Flat * 300.0;
			Dir = Flat;
			P->SetActorLocation(From - FVector(0.0, 0.0, 60.0), false, nullptr, ETeleportType::TeleportPhysics);
		}
		FVector At;
		return UOutlawsCoop::ShotAtPlayers(L, From, To + Dir * 300.0, KindDamage[Kind], At, Kind == Scoped);
	}
	}
}

// ---- the host -------------------------------------------------------------------------------------------------------------

void UOutlawsCoop::TickHostLethalTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	using namespace OutlawsMpLethalProbeLocal;
	FOutlawsMatchFlow& F = OutlawsMpHost::Flow();
	const bool bOld = OLFlags::Param(TEXT("OLOldMpLethal"));
	const double Wall = FPlatformTime::Seconds();
	L->bEnemiesActive = false;          // (the bots' brains stand where they spawned, apart: a blow meant for one meets no other)
	auto CharOf = [this](int32 Who) { const FOutlawsDmPlayer* D = Dm.Find(Who); return D ? D->Character : FString(TEXT("?")); };
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
	auto ArmourOf = [this, L](int32 Who) -> int32
	{
		if (Who == 0) return L->GetArmour();
		const AOutlawsCoopLink* K = IsBot(Who) ? nullptr : GetLink(Who);
		return K ? K->Stats.Armour : 0;
	};
	// the last blow seen through on the victim's own machine (its health or plate down, or dead; 2 s at most) before the next,
	// so the blows counted are the ones dealt
	auto Landed = [&](FVictim& V, double T)
	{
		if (V.Blows == 0) return true;
		const float H = HealthOf(V.Who);
		const bool bDown = H < V.LastHealth - 0.005f || ArmourOf(V.Who) < V.LastArmour;
		if (DeadOf(V.Who) || H <= 0.f)
		{
			if (V.Blows == 1 && V.After1 < 0.f) V.After1 = 0.f;
			V.bDone = V.bDied = true;
			return false;
		}
		if (bDown && V.Blows == 1 && V.After1 < 0.f) V.After1 = H;
		return bDown || T - V.BlowAt >= 2.0;
	};
	auto Struck = [&](FVictim& V, double T) { V.LastHealth = HealthOf(V.Who); V.LastArmour = ArmourOf(V.Who); V.BlowAt = T; ++V.Blows; };
	// the blows to kill as wanted; a player (not a bot) taking eight or more slow blows gets back a little health between them
	// (its own machine's regain, 4.9 -> 5.0 under half), so up to three more there
	auto CountOk = [this](int32 Who, int32 Blows, int32 Want) { return Blows == Want || (!IsBot(Who) && Want >= 8 && Blows > Want && Blows <= Want + 3); };
	// fresh: alive, past its spawn protection, at full health (the rules and its own machine agree)
	auto Fresh = [&](int32 Who)
	{
		FOutlawsDmPlayer* D = Dm.Find(Who);
		return D && D->bAlive && Wall > D->ProtectedUntil + 0.3 && MatchInRules[Who] && !MatchDead[Who] && !DeadOf(Who) && HealthOf(Who) >= 9.99f && (Who == 0 || PuppetOf(Who) != nullptr);
	};
	auto Finish = [&]()
	{
		for (const FString& T : Table) UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT mplethal table: %s"), *T);
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT mplethal host RESULT %s (%d passed, %d failed%s; lethal dealt %d, first punches %d, doubted %d)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"),
			Passes, Fails, bOld ? TEXT(", -OLOldMpLethal") : TEXT(""), MatchLethalDealt, MatchPunchHalves, MatchLethalDoubted);
		TestSay(TEXT("quit"));
		TestStep = 2199;
		TestAt = Now + 3.0;
	};
	if (TestStep < 2120) { TestStep = 2120; TestAt = 0.0; }
	switch (TestStep)
	{
	case 2120:
	{
		// the six bots and the client in the rules; then Start, and past the spawn protection
		TArray<AOutlawsCoopLink*> Links;
		GetLinks(Links);
		Victims.Reset();
		Client = -1;
		for (const AOutlawsCoopLink* K : Links)
		{
			const int32 Who = K->PlayerIndex;
			if (Who <= 0 || Who >= MaxPlayers || !MatchInRules[Who]) continue;
			if (K->bBot) { FVictim& V = Victims.AddDefaulted_GetRef(); V.Who = Who; }
			else if (Client < 0) Client = Who;
		}
		if (TestAt == 0.0) TestAt = Now + 240.0;
		if ((Victims.Num() < 6 || Client < 0 || !bMatchSetUp) && Now < TestAt) return;
		Check(Victims.Num() >= 6 && Client > 0, FString::Printf(TEXT("the match has %d bots and %s"), Victims.Num(), Client > 0 ? TEXT("the client") : TEXT("no client")));
		if (Victims.Num() == 0 || Client < 0) { Finish(); return; }
		Bot = Victims[0].Who;
		F.HostStart();
		TestStep = 2121;
		return;
	}
	case 2121:
		if (F.GetState() != FOutlawsMatchFlow::EState::Playing) return;
		Round = 0;
		Phase = 0;
		PhaseAt = Now;
		TestStep = 2130;
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT mplethal live: the host %s, the client the %s, %d bots%s"), *CharOf(0), WhoWord(Client), Victims.Num(), bOld ? TEXT(" (-OLOldMpLethal)") : TEXT(""));
		return;

	// 1. the rounds: the host on every bot and the client, one kind a round
	case 2130:
	{
		if (Round > Body)
		{
			// the host off to the spawn point farthest from every bot (the rounds' shots left it beside one), for the client's blows
			double Best = -1.0;
			FVector Far = P->GetActorLocation();
			for (const FOutlawsDmSpawn& Sp : Dm.GetSpawns())
			{
				double Near = 1e12;
				for (const FVictim& V : Victims) if (const AOutlawsCoopLink* K = GetLink(V.Who)) Near = FMath::Min(Near, FVector::Dist(Sp.Location, FVector(K->Pose.Body)));
				if (Near > Best) { Best = Near; Far = Sp.Location; }
			}
			P->SetActorLocation(Far, false, nullptr, ETeleportType::TeleportPhysics);
			UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT mplethal the host stands at %s, %.0f m from the nearest bot"), *Far.ToCompactString(), Best / 100.0);
			Job = 0; Phase = 0; PhaseAt = Now; TestStep = 2140;
			return;
		}
		if (Phase == 0)
		{
			// every victim fresh (the client with this round's character), 40 s at most
			bool bAll = Fresh(Client);
			for (const FVictim& V : Victims) bAll = bAll && Fresh(V.Who);
			if (!bAll && Now - PhaseAt < 40.0) return;
			for (FVictim& V : Victims) { V.Blows = 0; V.bDone = !Fresh(V.Who); V.bDied = false; V.After1 = -1.f; V.Char = CharOf(V.Who); }
			FVictim& CV = Victims.AddDefaulted_GetRef();
			CV.Who = Client;
			CV.Char = CharOf(Client);
			CV.bDone = !Fresh(Client);
			for (const FVictim& V : Victims) if (V.bDone) Check(false, FString::Printf(TEXT("%s round: the %s (%s) never came back fresh"), KindNames[Round], WhoWord(V.Who), *V.Char));
			Phase = 1;
			NextBlowAt = Now;
			return;
		}
		if (Now < NextBlowAt) return;
		NextBlowAt = Now + BlowEvery;
		bool bAny = false;
		for (FVictim& V : Victims)
		{
			if (V.bDone) continue;
			bAny = true;
			if (!Landed(V, Now)) continue;
			if (V.Blows >= MaxBlows) { V.bDone = true; continue; }
			Struck(V, Now);
			const bool bHit = OLLethalBlow(this, L, P, Round, V.Who, true);
			if (!bHit && Lines++ < 40) UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT mplethal the host's %s at the %s missed"), KindNames[Round], WhoWord(V.Who));
		}
		if (bAny) return;
		// the round's checks, a line a victim
		for (FVictim& V : Victims)
		{
			if (V.Char == TEXT("?")) continue;
			const float Mult = MultOf(L, V.Char);
			const int32 Want = WantBlows(Round, bOld, false, Mult, 0);
			const FString Line = FString::Printf(TEXT("%s on the %s (%s, x%.2f): %d blows to kill (want %d)%s"), KindNames[Round], IsBot(V.Who) ? TEXT("bot") : TEXT("client"), *V.Char, Mult, V.Blows, Want,
				Round == Punch ? *FString::Printf(TEXT(", %.2f health after the first"), V.After1) : TEXT(""));
			Table.Add(Line);
			Check(V.bDied && CountOk(V.Who, V.Blows, Want), Line);
			if (Round == Punch && !bOld) Check(FMath::IsNearlyEqual(V.After1, 5.f, 0.01f), FString::Printf(TEXT("the first punch leaves the %s (%s) at 5: %.2f"), WhoWord(V.Who), *V.Char, V.After1));
		}
		Victims.Pop();                                   // (the client: its own place each round)
		// the client back, as the next round's character
		if (Round + 1 < UE_ARRAY_COUNT(ClientChars)) Dm.SetPendingCharacter(Client, ClientChars[Round + 1]);
		TestSayTo(Client, TEXT("ml:respawn"), 0);
		++Round;
		Phase = 0;
		PhaseAt = Now;
		return;
	}

	// 2 and 3. the jobs: the client on the host, a bot's knife on the host and on the client
	case 2140:
	{
		static const FJob Jobs[] = {
			{ Punch, true, 0, false, false }, { Stab, true, 0, false, false }, { Throw, true, 0, false, false }, { Scoped, true, 0, false, false },
			{ Head, true, 0, false, false }, { Body, true, 0, false, false }, { Stab, true, 0, false, true }, { Stab, true, 0, true, false },
			{ Punch, true, 0, true, false }, { BotKnife, false, 0, false, false }, { BotKnife, false, 1, false, false } };
		if (Job >= int32(UE_ARRAY_COUNT(Jobs))) { Finish(); return; }
		const FJob& J = Jobs[Job];
		const int32 Victim = J.VictimSlot == 0 ? 0 : Client;
		const TCHAR* VictimWord = Victim == 0 ? TEXT("host") : TEXT("client");
		if (Phase == 0)
		{
			// (and the client striking alive: a dead one's blows are refused, and its respawn would move it off the victim)
			const bool bReady = Fresh(Victim) && (!J.bClientStrikes || (!MatchDead[Client] && !DeadOf(Client)));
			if (!bReady && Now - PhaseAt < 40.0) return;
			if (!bReady) { Check(false, FString::Printf(TEXT("job %d: the %s never came back fresh"), Job, VictimWord)); ++Job; PhaseAt = Now; return; }
			// the striker beside the victim with what it strikes with in hand; the armour on
			if (J.bClientStrikes)
			{
				TestSayTo(Client, TEXT("ml:near"), Victim);
				TestSayTo(Client, FString::Printf(TEXT("ml:hold:%d"), J.Kind == Punch || J.bBare ? -1 : J.Kind == Stab || J.Kind == Throw ? 7 : 1), Victim);
			}
			if (J.bArmour)
			{
				if (Victim == 0) L->SetArmour(150);
				else TestSayTo(Client, TEXT("ml:armour"), 0);
			}
			Victims.Reset();
			FVictim& V = Victims.AddDefaulted_GetRef();
			V.Who = Victim;
			V.Char = CharOf(Victim);
			V.Armour = J.bArmour ? 150 : 0;
			Phase = 1;
			NextBlowAt = Now + 1.2;                      // (its pose's hands and place at the host)
			return;
		}
		if (Now < NextBlowAt) return;
		NextBlowAt = Now + BlowEvery;
		FVictim& V = Victims[0];
		if (!V.bDone && !Landed(V, Now) && !V.bDone) return;
		const bool bDead = V.bDied;
		if (!bDead && V.Blows < MaxBlows)
		{
			Struck(V, Now);
			if (J.bClientStrikes) TestSayTo(Client, FString::Printf(TEXT("ml:%s"), KindNames[J.Kind]), Victim);
			else if (Victim == 0)
			{
				// a bot's knife on the host, as TickBotThrown deals it
				const AOutlawsCoopLink* BK = GetLink(Bot);
				const FVector Chest = P->GetActorLocation() + FVector(0.0, 0.0, 10.0);
				OnMatchHit(Bot, LevelEpoch(), 0, KindDamage[BotKnife], Chest, BK ? FVector(BK->Pose.Body) : Chest, false, uint8(EOutlawsMatchBlow::ThrownKnife));
			}
			else
			{
				// a bot's knife on the client, through its body as drawn here (TickBotThrown's SweepAtPlayers)
				const AOutlawsRemotePlayer* R = PuppetOf(Victim);
				const FVector Chest = R ? R->ShownBody() + FVector(0.0, 0.0, 20.0) : FVector::ZeroVector;
				TGuardValue<int32> By(BlastShooter, Bot);
				FVector At;
				SweepAtPlayers(L, Chest - FVector(60.0, 0.0, 0.0), Chest + FVector(60.0, 0.0, 0.0), KindDamage[BotKnife], TEXT("a bot's knife"), At);
			}
			return;
		}
		const float Mult = MultOf(L, V.Char);
		const int32 Want = WantBlows(J.Kind, bOld, J.bBare, Mult, V.Armour);
		const FString Line = FString::Printf(TEXT("%s%s%s by the %s on the %s (%s, x%.2f%s): %d blows to kill (want %d)%s"), KindNames[J.Kind], J.bBare ? TEXT(" (no knife in hand)") : TEXT(""),
			J.bArmour ? TEXT(" in 150 armour") : TEXT(""), J.bClientStrikes ? TEXT("client") : TEXT("bot"), VictimWord, *V.Char, Mult, J.bClientStrikes ? TEXT(", decided by the host") : TEXT(""), V.Blows, Want,
			J.Kind == Punch ? *FString::Printf(TEXT(", %.2f health after the first"), V.After1) : TEXT(""));
		Table.Add(Line);
		Check(bDead && CountOk(V.Who, V.Blows, Want), Line);
		if (J.Kind == Punch && !bOld) Check(FMath::IsNearlyEqual(V.After1, 5.f, 0.01f), FString::Printf(TEXT("the first punch leaves the %s at 5: %.2f"), VictimWord, V.After1));
		if (Victim == Client) TestSayTo(Client, TEXT("ml:respawn"), 0);
		TestSayTo(Client, TEXT("ml:hold:-1"), 0);
		++Job;
		Phase = 0;
		PhaseAt = Now;
		return;
	}
	case 2199:
		if (Now < TestAt) return;
		TestStep = 2200;
		QuitForTest();
		return;
	default:
		return;
	}
}

// ---- the client: the host's word, done as its player would ------------------------------------------------------------------

void UOutlawsCoop::OnLethalTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	using namespace OutlawsMpLethalProbeLocal;
	const int32 Hand = P->LeftDominant() ? 0 : 1;
	if (Step == TEXT("respawn"))
	{
		// the death screen's button, once the respawn delay is past
		TWeakObjectPtr<AOutlawsPlayer> WeakP(P);
		FTimerHandle H;
		P->GetWorldTimerManager().SetTimer(H, FTimerDelegate::CreateWeakLambda(this, [WeakP]() { if (AOutlawsPlayer* Pl = WeakP.Get()) Pl->RequestRespawn(false); }), 2.3f, false);
		return;
	}
	if (Step == TEXT("armour")) { L->SetArmour(150); UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT mplethal the client wears 150 armour")); return; }
	if (Step.StartsWith(TEXT("hold:")))
	{
		const int32 Wi = FCString::Atoi(*Step.Mid(5));
		if (Wi == 7) L->AddCount(TEXT("iknife"), 1);
		if (Wi == 1) L->AddCount(TEXT("irifle"), 1);
		P->Equip(Hand, Wi);
		P->Equip(1 - Hand, -1);
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT mplethal the client holds weapon %d (the %s hand: %d)"), Wi, Hand == 0 ? TEXT("left") : TEXT("right"), P->Equipped[Hand]);
		return;
	}
	const AOutlawsRemotePlayer* R = PuppetOf(Arg);
	if (!R) { UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT mplethal client: the %s is not drawn here for %s"), WhoWord(Arg), *Step); return; }
	if (Step == TEXT("near"))
	{
		// 1.2 m from the victim's drawn body, facing it
		FVector Flat = (P->GetActorLocation() - R->ShownBody()).GetSafeNormal2D();
		if (Flat.IsNearlyZero()) Flat = FVector::ForwardVector;
		const FVector To = R->ShownBody() + Flat * 120.0 + FVector(0.0, 0.0, P->GetActorLocation().Z - R->ShownBody().Z);
		P->SetActorLocation(To, false, nullptr, ETeleportType::TeleportPhysics);
		if (AController* Ct = P->GetController()) Ct->SetControlRotation((-Flat).Rotation());
		return;
	}
	for (int32 k = 0; k < NumKinds; ++k)
	{
		if (Step != KindNames[k]) continue;
		const bool bHit = OLLethalBlow(this, L, P, k, Arg, false);
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT mplethal client: its %s at the %s: %s"), KindNames[k], WhoWord(Arg), bHit ? TEXT("a hit, reported to the host") : TEXT("a miss"));
		return;
	}
}

// ---- a story level: untouched -----------------------------------------------------------------------------------------------

OL_SHOT_MATCH(OLMpLethalProbe) { return OLFlags::ValueIs(TEXT("OLMpLethalProbe"), TEXT("story")); }

OL_SHOT_TEST(OLMpLethalProbe, 2120)
{
	using namespace OutlawsMpLethalProbeLocal;
	if (!L || ShotStep < 0) return;
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	if (ShotStep == 0)
	{
		if (ShotTime < 4.f || !bUp) return;
		L->bInvincible = true;
		UOutlawsCoop* C = UOutlawsCoop::Get(this);
		Check(!UOutlawsCoop::IsMatchActive(L), FString::Printf(TEXT("%s: no match"), *L->LevelName));
		Check(C && !C->MatchLethalOn(), TEXT("the rule is off"));
		for (uint8 b = 1; b <= uint8(EOutlawsMatchBlow::ScopedHead) && C; ++b)
		{
			Check(C->LethalBlowDamage(1, 2, b, FVector::ZeroVector, FVector::ZeroVector) < 0.f, FString::Printf(TEXT("blow %d: dealt as before"), b));
		}
		const FVector Eye = Camera ? Camera->GetComponentLocation() : GetActorLocation();
		FVector At;
		Check(!UOutlawsCoop::StrikeAtPlayers(L, Eye + GetActorForwardVector() * 40.0, 22.f, 0.8f, TEXT("a fist")), TEXT("a fist strikes no player"));
		Check(!UOutlawsCoop::ShotAtPlayers(L, Eye, Eye + GetActorForwardVector() * 5000.0, 2.f, At, true), TEXT("a scoped shot strikes no player"));
		Check(!UOutlawsCoop::SweepAtPlayers(L, Eye, Eye + GetActorForwardVector() * 300.0, 4.f, TEXT("a thrown knife"), At), TEXT("a thrown knife strikes no player"));
		// a bad guy's fist, stab and rifle hits as before: the damage itself, each on a bad guy of its own (2..4 health), struck in
		// the middle from 3 m (x1)
		TArray<AOutlawsEnemy*> Es;
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
		{
			AOutlawsEnemy* X = Wk.Get();
			if (X && !X->IsDead() && !X->IsDormant() && !X->bBoss && !X->IsCritter() && Es.Num() < 3) Es.Add(X);
		}
		Check(Es.Num() == 3, FString::Printf(TEXT("three bad guys awake to strike (%d)"), Es.Num()));
		const int32 Ks[] = { int32(Punch), int32(Stab), int32(Scoped) };
		for (int32 i = 0; i < Es.Num(); ++i)
		{
			AOutlawsEnemy* E = Es[i];
			const int32 k = Ks[i];
			const float Before = E->GetHealth();
			const FVector C0 = E->GetActorLocation() - FVector(0.0, 0.0, 10.0);
			const FVector Dir = E->GetActorForwardVector();
			if (k != Scoped) AOutlawsEnemy::GNextHitMelee = true;
			E->TakeHit(KindDamage[k], C0, -Dir, C0 + Dir * 300.0);
			Check(FMath::IsNearlyEqual(Before - E->GetHealth(), KindDamage[k], 0.01f), FString::Printf(TEXT("%s on %s: %.2f taken, health %.2f -> %.2f (as before: %.2f)"), KindNames[k], *E->GetOutlawsName(),
				Before - E->GetHealth(), Before, E->GetHealth(), KindDamage[k]));
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mplethal story RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
	}
}

#endif // WITH_OUTLAWS_TESTS
