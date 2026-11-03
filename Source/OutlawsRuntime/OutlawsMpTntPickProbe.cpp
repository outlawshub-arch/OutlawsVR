// BUG-MPTNTPICK's probe (the user's stream, 3 Nov: in a match the TNT boxes are picked up, the single sticks of dynamite never;
// the log had 'picked up gamboxt' 10 times, the bots' NAMBOXT1 55, no stick and no refusal).  A match's sticks are its NDYNAM1
// generators' (town_2p has eight: GENERATE GDYNAM.ITM, FREQ 2, a rate a minute, 30 s).  Two machines:
//   host:   -OLMpHost=town_2p -OLMpClass=dm -OLMpBots=1 -OLMpChar=Doc -OLMpHostRespawn=2 -OLMpMinutes=30 -OLMpFrags=0
//           -OLMpTntPickProbe [-OLOldMpTntPick]
//   client: -OLCoopJoin=<host>:<port> -OLMpChar=Mar -OLMpTntPickProbe [-OLOldMpTntPick]
// The bot's brain stands still; the sticks are walked onto where they lie.  The host takes one (a stick more, gone on the client),
// the client one (a stick more there, gone on the host, the host's word), the bot one (its own take, BotLookForPickups); the first
// comes back after its generator's period, a new pickup on both machines.  -OLOldMpTntPick: none is a pickup anywhere, none taken.
// -OLMpTntPickProbe=story -OLShots -OLLevel=hideout -OLNoFrontEnd: a story level, no match: the level's own stick still a pickup
// and walked over, a stick made in play still no pickup, as before.
// Lines "OLSHOT mptntpick check PASS|FAIL (host|client): ..." and "OLSHOT mptntpick <host|client|story> RESULT".

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
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsMpTntPickProbeLocal
{
	constexpr int32 NumTaken = 3;
	const TCHAR* const Stick = TEXT("idynam");
	int32 Passes = 0, Fails = 0, Client = -1, Bot = -1, CountWas = 0, Gens[NumTaken] = {}, Ids[NumTaken] = {};
	FVector Where[NumTaken], Home = FVector::ZeroVector, Told = FVector::ZeroVector;
	TWeakObjectPtr<AActor> Actors[NumTaken];
	double TakenAt = 0.0, Deadline = 0.0;

	void Check(bool bOk, const TCHAR* Who, const FString& What)
	{
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT mptntpick check %s (%s): %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), Who, *What);
		if (bOk) ++Passes; else ++Fails;
	}

	/** This machine's sticks that are pickups. */
	int32 StickPickups(const AOutlawsLevelActor* L)
	{
		TArray<AOutlawsLevelActor::FPickupDebug> All;
		L->DebugPickups(All);
		int32 N = 0;
		for (const AOutlawsLevelActor::FPickupDebug& Pk : All) N += Pk.Actor && AOutlawsLevelActor::IsStickGround(Pk.Ground) ? 1 : 0;
		return N;
	}

	/** Stood on a place on the floor, as walked there: the level's own walk-over takes what lies there. */
	void StandOn(AOutlawsPlayer* P, const FVector& At)
	{
		P->SetActorLocation(At + FVector(0.0, 0.0, P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 3.0), false, nullptr, ETeleportType::TeleportPhysics);
	}
}

// ---- the host -------------------------------------------------------------------------------------------------------------

void UOutlawsCoop::TickHostTntPickTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	using namespace OutlawsMpTntPickProbeLocal;
	FOutlawsMatchFlow& F = OutlawsMpHost::Flow();
	const bool bOld = OLFlags::Param(TEXT("OLOldMpTntPick"));
	L->bEnemiesActive = false;          // (the bot's brain stands where it spawned: it takes only the stick it is put on)
	L->bInvincible = true;
	const TArray<FOutlawsDmItemGen>& Items = Dm.GetItems();
	auto Mine = [](bool bOk, const FString& What) { Check(bOk, TEXT("host"), What); };
	auto Finish = [&]()
	{
		TestSayTo(Client, TEXT("tk:end"), 0);
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT mptntpick host RESULT %s (%d passed, %d failed)%s"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails, bOld ? TEXT(" -OLOldMpTntPick") : TEXT(""));
		TestStep = 2319;
		TestAt = Now + 3.0;
	};
	if (TestStep < 2310) { TestStep = 2310; TestAt = 0.0; }
	switch (TestStep)
	{
	case 2310:
	{
		// the bot and the client in the rules; then Start
		TArray<AOutlawsCoopLink*> Links;
		GetLinks(Links);
		Client = Bot = -1;
		for (const AOutlawsCoopLink* K : Links)
		{
			const int32 Who = K->PlayerIndex;
			if (Who <= 0 || Who >= MaxPlayers || !MatchInRules[Who]) continue;
			if (K->bBot) Bot = Who;
			else if (Client < 0) Client = Who;
		}
		if (TestAt == 0.0) TestAt = Now + 240.0;
		if ((Bot < 0 || Client < 0 || !bMatchSetUp) && Now < TestAt) return;
		Mine(Bot > 0 && Client > 0, FString::Printf(TEXT("the match has %s and %s"), Bot > 0 ? TEXT("the bot") : TEXT("no bot"), Client > 0 ? TEXT("the client") : TEXT("no client")));
		if (Bot < 0 || Client < 0) { Finish(); return; }
		F.HostStart();
		TestStep = 2311;
		return;
	}
	case 2311:
		if (F.GetState() != FOutlawsMatchFlow::EState::Playing) return;
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT mptntpick live on %s: the client the %s, the bot the %s%s"), *L->LevelName, WhoWord(Client), WhoWord(Bot), bOld ? TEXT(" (-OLOldMpTntPick)") : TEXT(""));
		TestAt = Now + 4.0;
		TestStep = 2312;
		return;
	case 2312:
	{
		// the map's stick generators, each with its stick out: pickups (the old: none), numbered by the host, made on the client
		if (Now < TestAt) return;
		int32 Sticks = 0, Listed = 0, Have = 0;
		for (int32 i = 0; i < Items.Num() && i < MatchItemActors.Num(); ++i)
		{
			if (!AOutlawsLevelActor::IsStickGround(Items[i].Generate)) continue;
			++Sticks;
			AActor* A = MatchItemActors[i].Get();
			const int32 Pi = PickupIndexOf(L, A);
			Listed += Pi >= 0 ? 1 : 0;
			if (!A || Have >= NumTaken) continue;
			Gens[Have] = i;
			Ids[Have] = Pi >= 0 ? L->Pickups[Pi].CoopId : -1;
			Where[Have] = A->GetActorLocation();
			Actors[Have] = A;
			++Have;
		}
		Mine(Have == NumTaken, FString::Printf(TEXT("%s has %d stick generators, %d of them with a stick out here"), *L->LevelName, Sticks, Have));
		if (Have < NumTaken) { Finish(); return; }
		Mine(FMath::IsNearlyEqual(Items[Gens[0]].Period, 30.f, 0.1f), FString::Printf(TEXT("a stick's generator comes round in %.0f s (NDYNAM1: FREQ 2)"), Items[Gens[0]].Period));
		Mine(bOld ? Listed == 0 : Listed == Sticks, FString::Printf(TEXT("%d of the %d sticks are pickups here (%s)"), Listed, Sticks, bOld ? TEXT("the old: none") : TEXT("all")));
		L->TakeCount(Stick, L->GetCount(Stick));
		Home = P->GetActorLocation();
		TestSayTo(Client, TEXT("tk:arm"), bOld ? 0 : Sticks);
		for (int32 k = 0; k < NumTaken; ++k) if (Ids[k] >= 0) TestSayTo(Client, TEXT("tk:have"), Ids[k]);
		TestAt = Now + 2.0;
		TestStep = 2313;
		return;
	}
	case 2313:
		// the host walks onto the first
		if (Now < TestAt) return;
		CountWas = L->GetCount(Stick);
		StandOn(P, Where[0]);
		TestAt = Now + 2.5;
		TestStep = 2314;
		return;
	case 2314:
	{
		if (Now < TestAt) return;
		const int32 Count = L->GetCount(Stick);
		if (bOld)
		{
			Mine(Count == CountWas && Actors[0].IsValid() && Items[Gens[0]].bOut, FString::Printf(TEXT("the old: the host on a stick took nothing (%d -> %d), it still lies there, its generator does not run"), CountWas, Count));
		}
		else
		{
			Mine(Count == CountWas + 1, FString::Printf(TEXT("the host walked onto a stick: %d -> %d"), CountWas, Count));
			Mine(PickupIndex(L, Ids[0]) < 0 && PickupGone.Contains(Ids[0]) && PickupGone[Ids[0]] == 0, FString::Printf(TEXT("pickup %d gone here, the host's"), Ids[0]));
			Mine(!Items[Gens[0]].bOut, FString::Printf(TEXT("%s's clock runs, %.0f s"), *Items[Gens[0]].Object, Items[Gens[0]].Period));
			TestSayTo(Client, TEXT("tk:gone"), Ids[0]);
		}
		TakenAt = Now;
		P->SetActorLocation(Home, false, nullptr, ETeleportType::TeleportPhysics);       // (off the place: a stick back there would be taken at once)
		// the client walks onto the second, where the host has it (the old: nothing of it on the client)
		TestSayTo(Client, TEXT("tk:x"), FMath::RoundToInt(Where[1].X));
		TestSayTo(Client, TEXT("tk:y"), FMath::RoundToInt(Where[1].Y));
		TestSayTo(Client, TEXT("tk:z"), FMath::RoundToInt(Where[1].Z));
		TestSayTo(Client, TEXT("tk:walk"), 0);
		TestAt = Now + 3.5;
		TestStep = 2315;
		return;
	}
	case 2315:
	{
		if (Now < TestAt) return;
		if (bOld)
		{
			Mine(Actors[1].IsValid() && Items[Gens[1]].bOut, TEXT("the old: the client on a stick: it still lies here, its generator does not run"));
		}
		else
		{
			const int32* By = PickupGone.Find(Ids[1]);
			Mine(PickupIndex(L, Ids[1]) < 0 && By && *By == Client, FString::Printf(TEXT("the client's pickup %d: the host's word, gone here (by the %s)"), Ids[1], By ? WhoWord(*By) : TEXT("nobody")));
			Mine(!Items[Gens[1]].bOut, FString::Printf(TEXT("%s's clock runs"), *Items[Gens[1]].Object));
		}
		Mine(L->GetCount(Stick) == (bOld ? 0 : 1), FString::Printf(TEXT("the client's take gave the host nothing: %d here"), L->GetCount(Stick)));
		TestSayTo(Client, TEXT("tk:count"), 0);
		// the bot on the third: its own take, as it does each look round
		FBot* B = FindBot(Bot);
		AOutlawsEnemy* E = B ? B->Brain.Get() : nullptr;
		Mine(E != nullptr, TEXT("the bot's brain is here"));
		if (!E) { Finish(); return; }
		const FVector Back = E->GetActorLocation();
		const int32 PickupsWas = B->Pickups;
		B->Sticks = 0;
		E->SetActorLocation(Where[2] + FVector(0.0, 0.0, E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 3.0), false, nullptr, ETeleportType::TeleportPhysics);
		BotLookForPickups(*B, L, Now);
		E->SetActorLocation(Back, false, nullptr, ETeleportType::TeleportPhysics);
		if (bOld)
		{
			Mine(B->Pickups == PickupsWas && B->Sticks == 0 && Actors[2].IsValid(), FString::Printf(TEXT("the old: the bot on a stick took nothing: %d sticks"), B->Sticks));
			Deadline = Now + 3.0;
		}
		else
		{
			const int32* By = PickupGone.Find(Ids[2]);
			Mine(B->Pickups == PickupsWas + 1 && B->Sticks == 1, FString::Printf(TEXT("the bot took a stick: %d of them, %d pickups more"), B->Sticks, B->Pickups - PickupsWas));
			Mine(PickupIndex(L, Ids[2]) < 0 && By && *By == Bot, FString::Printf(TEXT("the bot's pickup %d gone here (by the %s)"), Ids[2], By ? WhoWord(*By) : TEXT("nobody")));
			TestSayTo(Client, TEXT("tk:gone"), Ids[2]);
			Deadline = TakenAt + Items[Gens[0]].Period + 6.0;
		}
		TestStep = 2316;
		return;
	}
	case 2316:
	{
		// the first stick back after its period: a new pickup, on both machines (the old: never gone)
		const FOutlawsDmItemGen& G = Items[Gens[0]];
		AActor* A = MatchItemActors[Gens[0]].Get();
		if (bOld)
		{
			if (Now < Deadline) return;
			Mine(G.bOut && A == Actors[0].Get(), TEXT("the old: the same stick lies there at the end"));
			Finish();
			return;
		}
		const bool bBack = G.bOut && A;
		if (!bBack && Now < Deadline) return;
		const double Took = Now - TakenAt;
		const int32 Pi = PickupIndexOf(L, A);
		const int32 Id = Pi >= 0 ? L->Pickups[Pi].CoopId : -1;
		Mine(bBack && Took >= G.Period - 3.0 && Took <= G.Period + 3.0, FString::Printf(TEXT("the first stick came back after %.1f s (its generator's %.0f)"), Took, G.Period));
		Mine(Id >= 0 && Id != Ids[0], FString::Printf(TEXT("it is a new pickup, %d (it was %d)"), Id, Ids[0]));
		if (Id >= 0) TestSayTo(Client, TEXT("tk:have"), Id);
		Deadline = Now + 2.0;
		TestStep = 2317;
		return;
	}
	case 2317:
		if (Now < Deadline) return;
		Finish();
		return;
	case 2319:
		if (Now < TestAt) return;
		TestSay(TEXT("quit"));
		TestAt = Now + 3.0;
		TestStep = 2320;
		return;
	case 2320:
		if (Now < TestAt) return;
		TestStep = 2321;
		QuitForTest();
		return;
	default:
		return;
	}
}

// ---- the client: the host's word, done as its player would ------------------------------------------------------------------

void UOutlawsCoop::OnTntPickTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	using namespace OutlawsMpTntPickProbeLocal;
	const bool bOld = OLFlags::Param(TEXT("OLOldMpTntPick"));
	auto Mine = [](bool bOk, const FString& What) { Check(bOk, TEXT("client"), What); };
	if (Step == TEXT("arm"))
	{
		// Arg: the sticks the host has as pickups, each made here too (the old: nothing told)
		L->bInvincible = true;
		L->TakeCount(Stick, L->GetCount(Stick));
		Home = P->GetActorLocation();
		Mine(StickPickups(L) == Arg, FString::Printf(TEXT("%d sticks are pickups here, the host has %d"), StickPickups(L), Arg));
	}
	else if (Step == TEXT("have"))
	{
		const int32 i = PickupIndex(L, Arg);
		const AActor* A = i >= 0 ? L->Pickups[i].Actor.Get() : nullptr;
		Mine(A && !A->IsHidden() && L->Pickups[i].Item == Stick && L->IsDynamitePickup(A), FString::Printf(TEXT("the host's stick %d lies here, drawn, gives %s, a shot sets it off"), Arg, i >= 0 ? *L->Pickups[i].Item : TEXT("nothing")));
	}
	else if (Step == TEXT("gone"))
	{
		Mine(PickupIndex(L, Arg) < 0, FString::Printf(TEXT("the stick %d taken elsewhere is gone here"), Arg));
	}
	else if (Step == TEXT("x")) Told.X = Arg;
	else if (Step == TEXT("y")) Told.Y = Arg;
	else if (Step == TEXT("z")) Told.Z = Arg;
	else if (Step == TEXT("walk"))
	{
		CountWas = L->GetCount(Stick);
		StandOn(P, Told);
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT mptntpick client: onto the stick at %s"), *Told.ToCompactString());
	}
	else if (Step == TEXT("count"))
	{
		const int32 Count = L->GetCount(Stick);
		Mine(Count == CountWas + (bOld ? 0 : 1), FString::Printf(TEXT("the client walked onto a stick: %d -> %d%s"), CountWas, Count, bOld ? TEXT(" (the old: nothing)") : TEXT("")));
		Mine(PendingTakes.Num() == 0, FString::Printf(TEXT("the host's word is in: %d takes waiting"), PendingTakes.Num()));
		P->SetActorLocation(Home, false, nullptr, ETeleportType::TeleportPhysics);
	}
	else if (Step == TEXT("end"))
	{
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT mptntpick client RESULT %s (%d passed, %d failed)%s"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails, bOld ? TEXT(" -OLOldMpTntPick") : TEXT(""));
	}
}

// ---- a story level: untouched -----------------------------------------------------------------------------------------------

OL_SHOT_MATCH(OLMpTntPickProbe) { return OLFlags::ValueIs(TEXT("OLMpTntPickProbe"), TEXT("story")); }

OL_SHOT_TEST(OLMpTntPickProbe, 2310)
{
	using namespace OutlawsMpTntPickProbeLocal;
	if (!L || ShotStep < 0) return;
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	auto Mine = [](bool bOk, const FString& What) { Check(bOk, TEXT("story"), What); };
	if (ShotStep == 0)
	{
		if (ShotTime < 4.f || !bUp) return;
		L->bInvincible = true;
		Mine(!UOutlawsCoop::IsMatchActive(L), FString::Printf(TEXT("%s: no match"), *L->LevelName));
		Mine(StickPickups(L) >= 1, FString::Printf(TEXT("the level's own sticks are pickups: %d"), StickPickups(L)));
		// a stick made in play: no pickup, as ever (its callers make it loose dynamite)
		const int32 Listed = StickPickups(L);
		AActor* Made = L->SpawnGroundItem(TEXT("gdynam"), GetActorLocation() + GetActorForwardVector() * 200.0 - FVector(0.0, 0.0, 100.0));
		Mine(Made && StickPickups(L) == Listed && !L->IsDynamitePickup(Made), FString::Printf(TEXT("a stick made in play: drawn, no pickup (%d listed, %d before)"), StickPickups(L), Listed));
		// the level's own stick walked onto
		Home = FVector::ZeroVector;
		TArray<AOutlawsLevelActor::FPickupDebug> All;
		L->DebugPickups(All);
		for (const AOutlawsLevelActor::FPickupDebug& Pk : All) if (Pk.Actor && AOutlawsLevelActor::IsStickGround(Pk.Ground)) Home = Pk.Actor->GetActorLocation();
		CountWas = L->GetCount(Stick);
		if (!Home.IsZero()) StandOn(this, Home);
		ShotTime = 0.f;
		ShotStep = 1;
		return;
	}
	if (ShotTime < 2.f) return;
	Mine(L->GetCount(Stick) == CountWas + 1 && StickPickups(L) == 0, FString::Printf(TEXT("the level's stick walked onto: %d -> %d, %d left"), CountWas, L->GetCount(Stick), StickPickups(L)));
	UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT mptntpick story RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
	ShotStep = -1;
	FGenericPlatformMisc::RequestExit(false);
}

#endif // WITH_OUTLAWS_TESTS
