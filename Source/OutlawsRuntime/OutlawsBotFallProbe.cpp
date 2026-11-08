// REQ-BOTFALLRULE's probe (the user's answer of 5 Nov): a match bot gets the player's fall rule, about 10 m free, about 45 m lethal.
//   host:   -OLMpHost=<map> -OLMpClass=dm -OLMpBots=2 -OLMpBotChars=Jam,Jam -OLMpChar=Jam -OLMpHostRespawn=2 -OLMpMinutes=30 -OLMpFrags=0
//           -OLBotFallProbe [-OLOldBotFall]
//   client: -OLCoopJoin=<host>:<port> -OLMpChar=Jam -OLBotFallProbe [-OLOldBotFall]
//   story:  -OLShots -OLNoFrontEnd -OLLevel=<lv> -OLBotFallProbe=story [-OLOldBotFall]: a bad guy's landing, its numbers to the digit
// The bots hold their fire.  1. A bot let go over the tallest column of the map from rest: 8 m takes nothing, 15 m the rule's number
// off the match's health (the host's own body dropped as far, side by side, -OLOldBotFall: dead).  2. A real blast's throw of a bot
// (the stick the host's): the rise, the landing, the rule's damage for that drop; the client's copy of its health the same.
// 3. In its protection a fall takes nothing.  4. A drop over 45 m kills, the client sees it.  5. A thrown bot that dies on landing is
// the thrower's kill, as ever.  Lines "OLSHOT botfall check PASS|FAIL: ..." and "OLSHOT botfall <host|client|story> RESULT".

#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsDeathmatch.h"
#include "OutlawsEnemy.h"
#include "OutlawsFlags.h"
#include "OutlawsLevelActor.h"
#include "OutlawsMpHost.h"
#include "OutlawsPlayer.h"
#include "OutlawsShotTests.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "TimerManager.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsBotFallProbeLocal
{
	int32 Passes = 0, Fails = 0, Client = -1, Bot = -1, Phase = 0, Case = 0;
	double PhaseAt = 0.0;
	float BaseHealth = 0.f, BotDrop = 0.f, BotTook = 0.f, TallRoom = 0.f;
	FVector Floor = FVector::ZeroVector, Tall = FVector::ZeroVector, Stick = FVector::ZeroVector;
	enum : int32 { StepWait = 3700, StepLive, StepScan, StepDrops, StepHostDrop, StepThrow, StepGrace, StepHigh, StepCredit, StepFinish, StepQuit, StepDone };

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT botfall check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}
	void Result(const TCHAR* Who)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT botfall %s RESULT %s (%d passed, %d failed%s)"), Who, Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails, OLFlags::Param(TEXT("OLOldBotFall")) ? TEXT(", -OLOldBotFall") : TEXT(""));
	}
	// what the original's rule says of a drop, written out apart from the game's own code: 0.2 x (sqrt(|GRAVITY| x h) - 45), h in units
	float Sum(float Gravity, float DropCm) { return FMath::Max(0.f, 0.2f * (FMath::Sqrt(FMath::Abs(Gravity) * DropCm / 30.f) - 45.f)); }
}

void UOutlawsCoop::TickHostBotFallTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	using namespace OutlawsBotFallProbeLocal;
	const double Wall = FPlatformTime::Seconds();
	FOutlawsMatchFlow& F = OutlawsMpHost::Flow();
	const bool bOld = OLFlags::Param(TEXT("OLOldBotFall"));
	L->bEnemiesActive = false;          // (the bots stand where they spawned, holding their fire)
	UWorld* W = L->GetWorld();
	FBot* B = FindBot(Bot);
	AOutlawsEnemy* E = B ? B->Brain.Get() : nullptr;
	auto Next = [&](int32 NewStep, int32 NewPhase = 0) { TestStep = NewStep; Phase = NewPhase; PhaseAt = Wall; };
	auto Finish = [&]()
	{
		Result(TEXT("host"));
		TestSayTo(Client, TEXT("bf:result"));
		TestSay(TEXT("quit"));
		TestAt = Now + 3.0;
		TestStep = StepQuit;
	};
	auto Ready = [&]()
	{
		// a live brain, past its protection, standing still
		const FOutlawsDmPlayer* D = Dm.Find(Bot);
		return E && B && !B->bDead && !E->IsDead() && D && D->bAlive && Wall > D->ProtectedUntil + 0.2 && !SpawnGraceOf(Bot);
	};
	auto DropFrom = [&](double HeightCm)
	{
		// the bot's feet HeightCm over the column's floor, at rest
		BaseHealth = B->Health;
		BotFallDropCm = BotFallRule = -1.f;
		const double Half = E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		E->SetActorLocation(Floor + FVector(0.0, 0.0, Half + 2.0 + HeightCm), false, nullptr, ETeleportType::TeleportPhysics);
		E->GetCharacterMovement()->StopMovementImmediately();
		E->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	};
	auto StandAway = [&]()
	{
		// the host out of the stick's reach, in reach of the hit's own check (MaxShotReachCm)
		double Best = 1e12;
		FVector To = P->GetActorLocation();
		for (const FOutlawsDmSpawn& Sp : Dm.GetSpawns())
		{
			const double D = FVector::Dist(Sp.Location, Floor);
			if (D > L->BlastRadiusCm() + 800.0 && D < Best) { Best = D; To = Sp.Location; }
		}
		P->SetActorLocation(To, false, nullptr, ETeleportType::TeleportPhysics);
	};
	auto StickBy = [&]()
	{
		// a stick 1.5 m off the bot's body on a clear line
		const FVector Mid = E->GetActorLocation();
		const double Half = E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsBotFallStick), false, E);
		for (int32 a = 0; a < 8; ++a)
		{
			const FVector At = Mid + FRotator(0.0, 45.0 * a, 0.0).Vector() * 150.0 - FVector(0.0, 0.0, Half - 10.0);
			if (!W->LineTraceTestByChannel(At + FVector(0.0, 0.0, 20.0), Mid, ECC_Visibility, Q)) return At;
		}
		return Mid;
	};
	auto Down = [&]() { return !E || E->IsDead() || !B || B->bDead || (Wall - PhaseAt > 0.3 && E->GetCharacterMovement()->IsMovingOnGround()); };
	switch (TestStep)
	{
	case 0: case StepWait:
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
		TestStep = StepWait;
		if (TestAt == 0.0) TestAt = Now + 150.0;
		if ((Count < 2 || Client < 0 || !bMatchSetUp) && Now < TestAt) return;
		Check(Count >= 2 && Client > 0, FString::Printf(TEXT("the match has %d bots and %s"), Count, Client > 0 ? TEXT("the client") : TEXT("no client")));
		if (Count == 0 || Client < 0) { Finish(); return; }
		F.HostStart();
		TestStep = StepLive;
		return;
	}
	case StepLive:
		if (F.GetState() != FOutlawsMatchFlow::EState::Playing || Dm.GetPhase() != EOutlawsDmPhase::Live) return;
		if (!Ready()) { if (Wall - PhaseAt > 30.0 && PhaseAt > 0.0) { Check(false, TEXT("the bot ready")); Finish(); } if (PhaseAt == 0.0) PhaseAt = Wall; return; }
		Next(StepScan);
		return;
	case StepScan:
	{
		// the tallest clear column over a spawn point: traced up from its floor and down from 60 m over it (the sky's top has no skin)
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsBotFallScan), false, E);
		Q.AddIgnoredActor(P);
		double Best = -1.0;
		FString Lines;
		for (const FOutlawsDmSpawn& Sp : Dm.GetSpawns())
		{
			FHitResult G;
			if (!W->LineTraceSingleByChannel(G, Sp.Location + FVector(0.0, 0.0, 100.0), Sp.Location - FVector(0.0, 0.0, 600.0), ECC_Visibility, Q)) continue;
			const FVector Fl = G.ImpactPoint;
			FHitResult T;
			const double Up = W->LineTraceSingleByChannel(T, Fl + FVector(0.0, 0.0, 20.0), Fl + FVector(0.0, 0.0, 6000.0), ECC_Pawn, Q) ? T.Distance : 6000.0;
			FHitResult D;
			const double From = W->LineTraceSingleByChannel(D, Fl + FVector(0.0, 0.0, 6000.0), Fl + FVector(0.0, 0.0, 10.0), ECC_Pawn, Q) ? D.Distance : 6000.0;
			// (the room: to the first thing over the floor; from above, the first thing under 60 m, none but the floor means it is open all the way)
			const double Room = From >= 5900.0 ? 6000.0 : Up;
			if (Lines.Len() < 400) Lines += FString::Printf(TEXT("%.0f/%.0f "), Up, From);
			if (Room > Best) { Best = Room; Floor = Fl; }
		}
		TallRoom = float(Best);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT botfall the columns (up / clear from 60 m): %s; the tallest %.0f cm at %s, the host %.0f cm off"), *Lines, Best, *Floor.ToCompactString(), FVector::Dist2D(P->GetActorLocation(), Floor));
		StandAway();      // (the host's head under the column is a landing 1.7 m short: its own drop puts it there when it is wanted)
		Case = 0;
		Next(StepDrops);
		return;
	}
	case StepDrops:
	{
		static const float Heights[] = { 800.f, 1500.f, 2090.f };
		if (Case >= int32(UE_ARRAY_COUNT(Heights)))
		{
			Next(StepHostDrop);
			return;
		}
		const float H = Heights[Case];
		if (Phase == 0)
		{
			if (!Ready()) { if (Wall - PhaseAt > 25.0) { Check(false, TEXT("the bot ready for a drop")); Finish(); } return; }
			if (Wall - PhaseAt < 0.3) return;
			DropFrom(H);
			Next(StepDrops, 1);
			return;
		}
		if (!Down())
		{
			if (Wall - PhaseAt > 8.0) { Check(false, FString::Printf(TEXT("the bot down from %.0f m"), H / 100.f)); Finish(); }
			return;
		}
		if (Wall - PhaseAt < 0.5) return;
		const float G = L->GravityAt(Floor);
		const bool bDead = !B || B->bDead;
		const float Took = bDead ? BaseHealth : BaseHealth - B->Health;
		if (bOld)
		{
			Check(H > 1200.f ? bDead : !bDead && Took == 0.f, FString::Printf(TEXT("-OLOldBotFall: %.0f m from rest: %s, the match's health down %.2f"), H / 100.f, bDead ? TEXT("dead") : TEXT("alive"), Took));
		}
		else
		{
			const float Want = Sum(G, H);
			Check(!bDead && FMath::Abs(Took - Sum(G, BotFallDropCm)) < 0.02f && FMath::Abs(BotFallDropCm - H) < 40.f && FMath::Abs(Took - Want) < 0.25f,
				FString::Printf(TEXT("%.0f m from rest: %s, measured %.1f m, %.2f off the match's health, the rule %.2f for that drop, %.2f for the height (gravity %.0f)"),
				H / 100.f, bDead ? TEXT("dead") : TEXT("alive"), BotFallDropCm / 100.f, Took, Sum(G, BotFallDropCm), Want, G));
			if (H > 2000.f)
			{
				BotDrop = BotFallDropCm;
				BotTook = Took;
				TestSayTo(Client, TEXT("bf:health"), Bot + 1000 * FMath::RoundToInt(B->Health * 100.f));
			}
		}
		++Case;
		Next(StepDrops);
		return;
	}
	case StepHostDrop:
		// the host's own body as far, side by side: what the game's player takes (invincible: the rule's number is read, LastFallDamage)
		if (bOld) { Next(StepThrow); return; }
		if (Phase == 0)
		{
			if (Wall - PhaseAt < 0.8) return;
			L->bInvincible = true;
			const double Half = P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			P->SetActorLocation(Floor + FVector(120.0, 0.0, Half + 2.0 + BotDrop), false, nullptr, ETeleportType::TeleportPhysics);
			P->GetCharacterMovement()->StopMovementImmediately();
			P->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
			Next(StepHostDrop, 1);
			return;
		}
		if (Wall - PhaseAt < 0.3 || !P->GetCharacterMovement()->IsMovingOnGround())
		{
			if (Wall - PhaseAt > 8.0) { Check(false, TEXT("the host down from the bot's drop")); Finish(); }
			return;
		}
		Check(FMath::Abs(P->LastFallDamage - BotTook) < 0.15f, FString::Printf(TEXT("the host's own body from the same drop: %.1f m, %.2f by the player's own code; the bot took %.2f from %.1f m"),
			P->LastFallDropCm / 100.f, P->LastFallDamage, BotTook, BotDrop / 100.f));
		L->bInvincible = false;
		Next(StepThrow);
		return;
	case StepThrow:
	{
		// a real blast's throw: the host's stick beside it, the host well away, the bot with health to spare for the blast itself
		if (Phase == 0)
		{
			if (!Ready()) { if (Wall - PhaseAt > 25.0) { Check(false, TEXT("the bot ready for the throw")); Finish(); } return; }
			if (Wall - PhaseAt < 0.3) return;
			E->SetActorLocation(Floor + FVector(0.0, 0.0, E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 3.0), false, nullptr, ETeleportType::TeleportPhysics);
			E->GetCharacterMovement()->StopMovementImmediately();
			StandAway();
			Next(StepThrow, 1);
			return;
		}
		if (Phase == 1)
		{
			if (Wall - PhaseAt < 0.6) return;
			B->Health = 1010.f;
			Stick = StickBy();
			L->Explode(Stick);
			BaseHealth = B->Health;
			BotFallDropCm = BotFallRule = -1.f;
			Next(StepThrow, 2);
			return;
		}
		if (!Down() || Wall - PhaseAt < 1.5)
		{
			if (Wall - PhaseAt > 12.0) { Check(false, TEXT("the thrown bot down")); Finish(); }
			return;
		}
		if (Wall - PhaseAt < 2.0) return;
		const float G = L->GravityAt(E ? E->GetActorLocation() : Floor);
		const float Took = B ? BaseHealth - B->Health : -1.f;
		if (bOld) Check(!B || B->bDead, FString::Printf(TEXT("-OLOldBotFall: a real blast's throw kills it on landing: %s"), !B || B->bDead ? TEXT("dead") : TEXT("alive")));
		else
		{
			Check(B && !B->bDead && BotFallDropCm >= 1000.f && FMath::Abs(Took - Sum(G, BotFallDropCm)) < 0.02f,
				FString::Printf(TEXT("a real blast's throw: %.1f m down, %.2f taken, the rule %.2f, alive at %.1f"), BotFallDropCm / 100.f, Took, Sum(G, BotFallDropCm), B ? B->Health : -1.f));
			// (no word to the client here: the probe's 998 health for the blast goes out capped at 10; the 21 m drop is its check)
		}
		if (B && !B->bDead) { B->Health = 10.f; BotPublish(*B, Wall); }
		Next(StepGrace);
		return;
	}
	case StepGrace:
	{
		// a fresh spawn: the protection holds off a fall as off anything else
		if (Phase == 0)
		{
			if (!B || B->bDead) { if (Wall - PhaseAt > 25.0) { Check(false, TEXT("the bot back for the protection's fall")); Finish(); } return; }
			if (Wall - PhaseAt < 1.5) return;
			BotHurt(Bot, B->Health + 1.f, FVector::ZeroVector, FVector::ZeroVector, -1);
			Next(StepGrace, 1);
			return;
		}
		if (Phase == 1)
		{
			if (!E || !B || B->bDead || !SpawnGraceOf(Bot)) { if (Wall - PhaseAt > 25.0) { Check(false, TEXT("the bot back fresh")); Finish(); } return; }
			DropFrom(1500.0);
			Next(StepGrace, 2);
			return;
		}
		if (!Down()) { if (Wall - PhaseAt > 3.0) { Check(false, TEXT("the fresh bot down")); Finish(); } return; }
		if (Wall - PhaseAt < 0.5) return;
		if (!bOld) Check(B && !B->bDead && B->Health == BaseHealth && BotFallRule > 0.f,
			FString::Printf(TEXT("in its protection a 15 m drop takes nothing: the rule %.2f, health %.2f -> %.2f"), BotFallRule, BaseHealth, B ? B->Health : -1.f));
		Next(StepHigh);
		return;
	}
	case StepHigh:
	{
		// over 45 m: dead on landing, the client sees it.  A map's column under 46 m gets the drop it has, told as such
		const float H = 4700.f;
		if (Phase == 0)
		{
			if (!Ready()) { if (Wall - PhaseAt > 25.0) { Check(false, TEXT("the bot ready for the long drop")); Finish(); } return; }
			if (Wall - PhaseAt < 0.3) return;
			if (TallRoom < H + 200.f) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT botfall this map's tallest column is %.1f m: no drop past 45 m here"), TallRoom / 100.f);
			DropFrom(H);
			Next(StepHigh, 1);
			return;
		}
		if (!Down()) { if (Wall - PhaseAt > 10.0) { Check(false, TEXT("the bot down from 47 m")); Finish(); } return; }
		if (Wall - PhaseAt < 0.5) return;
		const float G = L->GravityAt(Floor);
		Check(!B || B->bDead, FString::Printf(TEXT("47 m from rest: measured %.1f m, the rule %.2f (the sum for the height %.2f), %s"), BotFallDropCm / 100.f, BotFallRule, Sum(G, H), !B || B->bDead ? TEXT("dead") : TEXT("alive")));
		TestSayTo(Client, TEXT("bf:dead"), Bot);
		Next(StepCredit);
		return;
	}
	case StepCredit:
	{
		// a thrown bot that dies on landing: the host's stick leaves it with 1, the landing kills it, the kill the host's
		const FOutlawsDmPlayer* Me = Dm.Find(0);
		if (Phase == 0)
		{
			if (!Ready()) { if (Wall - PhaseAt > 25.0) { Check(false, TEXT("the bot ready for the credit")); Finish(); } return; }
			if (Wall - PhaseAt < 0.3) return;
			E->SetActorLocation(Floor + FVector(0.0, 0.0, E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 3.0), false, nullptr, ETeleportType::TeleportPhysics);
			E->GetCharacterMovement()->StopMovementImmediately();
			Next(StepCredit, 1);
			return;
		}
		if (Phase == 1)
		{
			if (Wall - PhaseAt < 0.6) return;
			B->Health = 12.f * MatchDamageFor(Bot, 1.f) + 1.f;
			BaseHealth = Me ? float(Me->Frags) : 0.f;
			Stick = StickBy();
			L->Explode(Stick);
			Check(B && !B->bDead && B->Health < 2.f, FString::Printf(TEXT("the stick left the bot %.2f"), B ? B->Health : -1.f));
			Next(StepCredit, 2);
			return;
		}
		if (!B || !B->bDead || !MatchDead[Bot]) { if (Wall - PhaseAt > 12.0) { Check(false, TEXT("the thrown bot dead on landing")); Finish(); } return; }
		Check(Me && float(Me->Frags) == BaseHealth + 1.f && LastAttackAt[Bot] == 0.0, FString::Printf(TEXT("the thrown bot dead on landing is the host's kill: frags %.0f -> %d"), BaseHealth, Me ? Me->Frags : -1));
		Finish();
		return;
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

void UOutlawsCoop::OnBotFallTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	using namespace OutlawsBotFallProbeLocal;
	if (Step == TEXT("health") || Step == TEXT("dead"))
	{
		// (the client's copy of the bot's stats, a moment on from the word)
		const int32 Who = Step == TEXT("dead") ? Arg : Arg % 1000;
		const float Want = Step == TEXT("dead") ? 0.f : float(Arg / 1000) / 100.f;
		const bool bDeadWord = Step == TEXT("dead");
		FTimerHandle H;
		P->GetWorldTimerManager().SetTimer(H, FTimerDelegate::CreateWeakLambda(this, [this, Who, Want, bDeadWord]()
		{
			const AOutlawsCoopLink* K = GetLink(Who);
			if (bDeadWord) Check(K && K->Stats.bDead, FString::Printf(TEXT("the client sees the %s dead"), WhoWord(Who)));
			else Check(K && FMath::Abs(K->Stats.Health - Want) < 0.02f, FString::Printf(TEXT("the client sees the %s's health %.2f, the host's %.2f"), WhoWord(Who), K ? K->Stats.Health : -1.f, Want));
		}), 0.4f, false);
	}
	else if (Step == TEXT("result"))
	{
		Result(TEXT("client"));
	}
}

// ---- the story's bad guy: his own rule, to the digit ------------------------------------------------------------------------

OL_SHOT_MATCH(OLBotFallProbe) { return OLFlags::ValueIs(TEXT("OLBotFallProbe"), TEXT("story")); }

OL_SHOT_TEST(OLBotFallProbe, 2331)
{
	using namespace OutlawsBotFallProbeLocal;
	if (!L || ShotStep < 0) return;
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	if (ShotStep == 0)
	{
		if (ShotTime < 4.f || !bUp) return;
		L->bEnemiesActive = false;
		AOutlawsEnemy* E = nullptr;
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
		{
			AOutlawsEnemy* C = Wk.Get();
			if (C && !C->IsDead() && !C->IsDormant() && !C->IsVoiceOnly() && !C->bBoss && C->GetCharacterMovement()->IsMovingOnGround()) { E = C; break; }
		}
		Check(E != nullptr && E->CoopSelf < 0, TEXT("a story bad guy, not a bot"));
		if (!E) { Result(TEXT("story")); ShotStep = -1; FGenericPlatformMisc::RequestExit(false); return; }
		// his landing itself at known speeds: the number printed to the digit; the same lines with and without -OLOldBotFall.  Each from
		// the same full health, under the height that kills
		UCharacterMovementComponent* M = E->GetCharacterMovement();
		const float G = FMath::Abs(M->GetGravityZ());
		const float Max = E->GetMaxHealth();
		for (const float Cm : { 250.f, 300.f, 310.f, 500.f, 800.f, 1000.f, 1190.f })
		{
			E->TestSetHealth(Max);
			M->Velocity = FVector(0.0, 0.0, -FMath::Sqrt(2.f * G * Cm));
			static_cast<ACharacter*>(E)->Landed(FHitResult());
			const float Share = Cm <= 300.f ? 0.f : (Cm - 300.f) / 900.f;
			const float Want = Cm <= 300.f ? Max : Max - (Max * Share + 0.01f);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT botfall story %s %.0f cm: health %.6f of %.2f"), *L->LevelName.ToLower(), Cm, E->GetHealth(), Max);
			Check(FMath::Abs(E->GetHealth() - Want) < 0.001f, FString::Printf(TEXT("%.0f cm: health %.4f, his rule says %.4f"), Cm, E->GetHealth(), Want));
			M->Velocity = FVector::ZeroVector;
		}
		E->TestSetHealth(Max);
		Result(TEXT("story"));
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
	}
}

#endif // WITH_OUTLAWS_TESTS
