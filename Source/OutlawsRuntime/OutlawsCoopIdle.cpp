// BUG-BOSSIDLEHOST (10 Oct co-op test, townex, three players): the late boss came at 201 of 250 kills and went for the
// host, a desktop player (-nohmd) standing idle at the level's start, not the two Quest players at work.  He came in by
// the host (the boss's place is chosen round the level's own player) and chose the host (the nearest of the players).
// Now the host watches each player:
// - idle: no move (a metre), turn (30 degrees), shot or bad guy's hit for ol.CoopIdleSeconds (60); a desktop player who
//   has not moved, turned, fired or been hit since the level's start is idle after ol.CoopIdleDesktopSeconds (20);
// - a dead player, or one in its pause menu, is nobody's target as before;
// - the bad guys choose among the active players first (AOutlawsEnemy::CoopChooseTarget); an idle player is a target
//   only when no active one is alive and in the level; one who fires or is hit is active again at once;
// - a late boss comes in by the active players (BossAnchors).
// Story co-op on the host only: single player and the matches as ever.
// Its test: -OLCoopAuto -OLCoopTest=idlehost -OLCoopLevel=townex on the host and its client (run_idlehost.ps1).
#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsEnemy.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

TAutoConsoleVariable<int32> CVarOLCoopIdleTargets(TEXT("ol.CoopIdleTargets"), 1,
	TEXT("Co-op host: the bad guys go for the active players before an idle one, and a late boss comes in by them (1), or the nearest or seen player as before (0)."));
TAutoConsoleVariable<float> CVarOLCoopIdleSeconds(TEXT("ol.CoopIdleSeconds"), 60.f,
	TEXT("Co-op host: a player with no move, turn, shot or bad guy's hit for this many seconds counts as idle."));
TAutoConsoleVariable<float> CVarOLCoopIdleDesktopSeconds(TEXT("ol.CoopIdleDesktopSeconds"), 20.f,
	TEXT("Co-op host: a desktop player who has done nothing since the level's start counts as idle after this many seconds."));

namespace
{
	const TCHAR* IdleWhoOf(int32 Who) { return UOutlawsCoop::WhoWord(Who); }   // (its own name: OutlawsCoopEnemies.cpp has a WhoOf, and a unity build puts both in one file)
	constexpr double MoveCm = 100.0;              // a step that counts (across the floor: a fall or a lift is not one)
	constexpr float TurnDeg = 30.f;               // a turn that counts
	constexpr double PlacingSeconds = 3.0;        // a player's first moments in a level are its placing, not a move
}

bool UOutlawsCoop::IdleRuleOn() const
{
	return IsHost() && bLevelBuilt && !IsMatch() && CVarOLCoopIdleTargets.GetValueOnGameThread() != 0;
}

bool UOutlawsCoop::IsPlayerIdle(int32 Who) const
{
	return Who >= 0 && Who < MaxPlayers && IdleWatch[Who].bSeen && IdleWatch[Who].bIdle && IdleRuleOn();
}

void UOutlawsCoop::NotePlayerActive(int32 Who, const TCHAR* Why)
{
	if (!IsHost() || Who < 0 || Who >= MaxPlayers) return;
	FIdleWatch& W = IdleWatch[Who];
	W.LastActiveAt = FPlatformTime::Seconds();
	W.bEverActive = true;
	if (!W.bIdle) return;
	W.bIdle = false;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP idle: the %s player is active again (%s)"), IdleWhoOf(Who), Why);
}

void UOutlawsCoop::TickIdleWatch(double Now)
{
	if (IdleSerial != StartedSerial)
	{
		for (FIdleWatch& W : IdleWatch) W = FIdleWatch();
		IdleSerial = StartedSerial;
	}
	const AOutlawsLevelActor* L = FindLevel();
	const float IdleAfter = FMath::Max(1.f, CVarOLCoopIdleSeconds.GetValueOnGameThread());
	const float DesktopAfter = FMath::Max(1.f, CVarOLCoopIdleDesktopSeconds.GetValueOnGameThread());
	for (int32 k = 0; k < MaxPlayers; ++k)
	{
		bool bHere = false, bVR = false;
		FVector At = FVector::ZeroVector;
		float Yaw = 0.f;
		if (k == 0)
		{
			const AOutlawsPlayer* P = LocalPlayer();
			if (P && L && !L->IsPlayerDead())
			{
				bHere = true;
				At = P->GetActorLocation();
				const UCameraComponent* Cam = P->Camera.Get();
				Yaw = Cam ? float(Cam->GetComponentRotation().Yaw) : float(P->GetControlRotation().Yaw);
				bVR = P->IsVR();
			}
		}
		else if (RemoteViews[k].bValid && !RemoteViews[k].bDead)
		{
			if (const AOutlawsCoopLink* CL = GetLink(k))
			{
				bHere = true;
				At = CL->Pose.Body;
				Yaw = float(CL->Pose.HeadRot.Yaw);
				bVR = CL->Pose.bVR;
			}
		}
		FIdleWatch& W = IdleWatch[k];
		if (!bHere)
		{
			if (W.bSeen) W = FIdleWatch();                 // (dead or gone: watched afresh when it is back)
			continue;
		}
		W.bVR = bVR;
		if (!W.bSeen || Now - W.SeenAt < PlacingSeconds)
		{
			if (!W.bSeen) W.SeenAt = W.LastActiveAt = Now;
			W.bSeen = true;
			W.Anchor = At;
			W.AnchorYaw = Yaw;
			continue;
		}
		const bool bMoved = FVector::Dist2D(At, W.Anchor) > MoveCm;
		if (bMoved || FMath::Abs(FRotator::NormalizeAxis(Yaw - W.AnchorYaw)) > TurnDeg)
		{
			W.Anchor = At;
			W.AnchorYaw = Yaw;
			NotePlayerActive(k, bMoved ? TEXT("moved") : TEXT("turned"));
		}
		if (k > 0 && !W.bIdle && IsPauseHeld(k))
		{
			// (BUG-COOPTIMEOUT) a paused app is idle at once: nobody chases a player whose headset is off
			W.bIdle = true;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP idle: the %s player is idle now (its app is paused, its place held)"), IdleWhoOf(k));
		}
		const bool bFresh = !W.bVR && !W.bEverActive;
		if (!W.bIdle && Now - W.LastActiveAt > (bFresh ? DesktopAfter : IdleAfter))
		{
			W.bIdle = true;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP idle: the %s player is idle now (%s for %.0f s; at %s)"), IdleWhoOf(k),
				bFresh ? TEXT("a desktop player who has done nothing since the level's start") : TEXT("no move, turn, shot or hit"), Now - W.LastActiveAt, *At.ToCompactString());
		}
	}
}

bool UOutlawsCoop::BossAnchors(const UObject* Context, TArray<FVector>& OutFeet, TArray<FVector>& OutEyes, FString& OutWho)
{
	OutFeet.Reset();
	OutEyes.Reset();
	OutWho.Reset();
	UOutlawsCoop* C = Get(Context);
	if (!C || !C->IdleRuleOn()) return false;
	const AOutlawsLevelActor* L = C->FindLevel();
	struct FOne { int32 Who; FVector Feet, Eye; };
	TArray<FOne, TInlineAllocator<MaxPlayers>> Alive;
	const AOutlawsPlayer* P = C->LocalPlayer();
	if (P && L && !L->IsPlayerDead() && !(bMenuKeepsSafe && C->bLocalAway))
	{
		FVector Eye;
		FRotator EyeRot;
		P->GetActorEyesViewPoint(Eye, EyeRot);
		Alive.Add({ 0, P->GetActorLocation(), Eye });
	}
	for (int32 k = 1; k < MaxPlayers; ++k)
	{
		const FOutlawsCoopRemoteView& V = C->RemoteViews[k];
		if (V.bValid && !V.bDead && !V.bAway) Alive.Add({ k, V.Feet + FVector(0.0, 0.0, V.Half), V.Eye });     // (the body's middle, as the host's own)
	}
	bool bAnyActive = false;
	for (const FOne& O : Alive) bAnyActive = bAnyActive || !C->IsPlayerIdle(O.Who);
	FString Passed;
	for (const FOne& O : Alive)
	{
		if (bAnyActive && C->IsPlayerIdle(O.Who))
		{
			Passed += FString::Printf(TEXT("%sthe %s"), Passed.IsEmpty() ? TEXT("") : TEXT(", "), IdleWhoOf(O.Who));
			continue;
		}
		OutFeet.Add(O.Feet);
		OutEyes.Add(O.Eye);
		OutWho += FString::Printf(TEXT("%sthe %s"), OutWho.IsEmpty() ? TEXT("") : TEXT(", "), IdleWhoOf(O.Who));
	}
	if (OutFeet.Num() == 0) return false;
	if (!Passed.IsEmpty()) OutWho += FString::Printf(TEXT(" (idle, passed over: %s)"), *Passed);
	return true;
}

// ---- its test (-OLCoopAuto -OLCoopTest=idlehost -OLCoopLevel=townex; run_idlehost.ps1) --------------------------------------
// The host stands still (idle: a desktop player who has done nothing); the client walks.  The late boss is brought on (the
// kills set past four fifths): he goes for the client.  A bad guy set by the host, facing the host, nearer the host than
// the client: he goes for the client.  The host fires: active again, and the same bad guy goes for the host.

void UOutlawsCoop::IdleCheck(bool bOk, const FString& What)
{
	(bOk ? IdlePass : IdleFail)++;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP idlehost check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
}

bool UOutlawsCoop::IdlePlaceNear(AOutlawsLevelActor* L, AOutlawsPlayer* P, int32 Index)
{
	// 1.5 to 2.5 m from the host, toward the client (nearer the host), on a floor, facing the host
	const TArray<TWeakObjectPtr<AOutlawsEnemy>>& List = L->GetEnemies();
	AOutlawsEnemy* E = List.IsValidIndex(Index) ? List[Index].Get() : nullptr;
	const AOutlawsCoopLink* CL = GetLink(1);
	UWorld* W = L->GetWorld();
	if (!E || !CL || !W) return false;
	const FVector Host = P->GetActorLocation(), Client = CL->Pose.Body;
	FVector Way = (Client - Host).GetSafeNormal2D();
	if (Way.IsNearlyZero()) Way = FVector::ForwardVector;
	const float Half = E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsCoopTestIdle), false, E);
	Q.AddIgnoredActor(P);
	for (const double Dist : { 200.0, 250.0, 150.0 })
	{
		for (int32 k = 0; k < 18; ++k)
		{
			const float Turn = (k & 1 ? -1.f : 1.f) * 20.f * float((k + 1) / 2);
			const FVector To = Host + FRotator(0.f, Turn, 0.f).RotateVector(Way) * Dist;
			if (FVector::Dist2D(To, Client) <= Dist + 50.0) continue;                                                   // (nearer the host)
			FHitResult Hit;
			if (W->LineTraceSingleByChannel(Hit, Host, To + FVector(0.0, 0.0, 40.0), ECC_Visibility, Q)) continue;       // (in the host's sight)
			if (!W->LineTraceSingleByChannel(Hit, To + FVector(0.0, 0.0, 60.0), To - FVector(0.0, 0.0, 300.0), ECC_Visibility, Q)) continue;
			E->SetActorLocation(Hit.ImpactPoint + FVector(0.0, 0.0, Half + 2.0), false, nullptr, ETeleportType::TeleportPhysics);
			E->Heading = float((Host - E->GetActorLocation()).Rotation().Yaw);
			E->SetActorRotation(FRotator(0.f, E->Heading, 0.f));
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP idlehost: enemy %d (%s) set %.0f cm from the host, %.0f cm from the client, facing the host"), Index, *E->GetOutlawsName(),
				FVector::Dist(E->GetActorLocation(), Host), FVector::Dist(E->GetActorLocation(), Client));
			return true;
		}
	}
	return false;
}

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TickHostIdleTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	const TArray<TWeakObjectPtr<AOutlawsEnemy>>& List = L->GetEnemies();
	auto Enemy = [&List](int32 i) { return List.IsValidIndex(i) ? List[i].Get() : nullptr; };
	auto TheBoss = [this]() -> AOutlawsEnemy*
	{
		for (const TWeakObjectPtr<AOutlawsEnemy>& B : IdleBosses) if (AOutlawsEnemy* E = B.Get(); E && !E->IsDormant()) return E;
		return nullptr;
	};
	const AOutlawsCoopLink* CL = GetLink(1);
	const FVector Client = CL ? FVector(CL->Pose.Body) : FVector::ZeroVector;
	switch (TestStep)
	{
	case 820:
	{
		// both out of harm (the checks are about whom the bad guys choose); the client walks, the host stands
		L->bInvincible = true;
		TestSay(TEXT("godmode"), 1);
		IdleBosses.Reset();
		FString Names;
		for (const AOutlawsLevelActor::FBossSlot& S : L->BossSlots)
		{
			if (S.Actor.IsValid() && S.Actor->IsDormant() && S.Spawns.Num() > 0)
			{
				IdleBosses.Add(S.Actor);
				Names += (Names.IsEmpty() ? TEXT("") : TEXT(", ")) + S.Name;
			}
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP idlehost: late bosses waiting: %s; %d kills of %d; the host stands at %s"), Names.IsEmpty() ? TEXT("none") : *Names, L->Kills, L->TotalBadGuys,
			*P->GetActorLocation().ToCompactString());
		TestSay(TEXT("rwalk"), 2500);
		IdleWalkAt = Now + 15.0;
		IdleDeadline = Now + 100.0;
		TestStep = 821;
		TestAt = Now + 4.0;
		return;
	}
	case 821:
		if (Now < TestAt) return;
		TestAt = Now + 1.0;
		if (Now >= IdleWalkAt)
		{
			TestSay(TEXT("rwalk"), 1500);           // (the client kept at work)
			IdleWalkAt = Now + 15.0;
		}
		if (IsPlayerIdle(0) && !IsPlayerIdle(1) && IdleWatch[1].bEverActive)
		{
			IdleCheck(true, FString::Printf(TEXT("the host counts idle (still at %s, %s), the client active (it walked)"), *P->GetActorLocation().ToCompactString(),
				IdleWatch[0].bEverActive ? TEXT("nothing for a while") : TEXT("a desktop player who has done nothing since the level's start")));
			TestStep = 822;
			TestAt = Now;
		}
		else if (Now > IdleDeadline)
		{
			IdleCheck(false, FString::Printf(TEXT("the host idle %d, the client idle %d (ever active: host %d, client %d)"), IsPlayerIdle(0) ? 1 : 0, IsPlayerIdle(1) ? 1 : 0,
				IdleWatch[0].bEverActive ? 1 : 0, IdleWatch[1].bEverActive ? 1 : 0));
			TestStep = 822;
			TestAt = Now;
		}
		return;
	case 822:
		if (IdleBosses.Num() == 0)
		{
			IdleCheck(false, TEXT("no late boss waits in this level"));
			TestStep = 824;
			TestAt = Now;
			return;
		}
		TestSay(TEXT("rwalk"), 1500);
		L->Kills = FMath::Max(L->Kills, FMath::FloorToInt(0.8f * float(L->TotalBadGuys)) + 1);
		L->BossTimer = 0.f;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP idlehost: the kills set to %d of %d: the late boss comes; the client at %s"), L->Kills, L->TotalBadGuys, *Client.ToCompactString());
		IdleDeadline = Now + 12.0;
		TestStep = 823;
		TestAt = Now + 2.5;
		return;
	case 823:
	{
		if (Now < TestAt) return;
		AOutlawsEnemy* B = TheBoss();
		if (!B)
		{
			if (Now > IdleDeadline)
			{
				IdleCheck(false, TEXT("the late boss did not appear"));
				TestStep = 824;
				TestAt = Now;
			}
			return;
		}
		IdleCheck(B->CoopTarget == 1, FString::Printf(TEXT("the late boss %s appeared %.0f m from the client, %.0f m from the idle host, and goes for the %s"), *B->GetOutlawsName(),
			FVector::Dist(B->GetActorLocation(), Client) / 100.0, FVector::Dist(B->GetActorLocation(), P->GetActorLocation()) / 100.0, IdleWhoOf(B->CoopTarget)));
		TestStep = 824;
		TestAt = Now + 4.0;
		return;
	}
	case 824:
	{
		if (Now < TestAt) return;
		if (AOutlawsEnemy* B = TheBoss(); B && !B->IsDead())
		{
			IdleCheck(B->CoopTarget == 1, FString::Printf(TEXT("4 s on, the late boss still goes for the %s (%s)"), IdleWhoOf(B->CoopTarget), *B->CoopDescribe()));
		}
		// a bad guy near both, nearer the host and facing it: the active client still
		IdleEnemy = TestPickEnemy(L, P->GetActorLocation(), INDEX_NONE);
		AOutlawsEnemy* E = Enemy(IdleEnemy);
		if (!E || !IdlePlaceNear(L, P, IdleEnemy))
		{
			IdleCheck(false, FString::Printf(TEXT("no bad guy could be set by the host (enemy %d)"), IdleEnemy));
			TestStep = 825;
			TestAt = Now;
			return;
		}
		E->CoopChooseTarget(true);
		IdleCheck(E->CoopTarget == 1 && IsPlayerIdle(0), FString::Printf(TEXT("a bad guy nearer the idle host, facing it, goes for the %s"), IdleWhoOf(E->CoopTarget)));
		TestStep = 825;
		TestAt = Now + 1.0;
		return;
	}
	case 825:
		if (Now < TestAt) return;
		L->AddCount(TEXT("ibullit"), 6);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP idlehost: the host fires its revolver (idle %d)"), IsPlayerIdle(0) ? 1 : 0);
		P->CoopTestFire();
		TestStep = 826;
		TestAt = Now + 1.5;
		return;
	case 826:
	{
		if (Now < TestAt) return;
		IdleCheck(!IsPlayerIdle(0), TEXT("the host counts active again after its shot"));
		AOutlawsEnemy* E = Enemy(IdleEnemy);
		if (!E || E->IsDead())
		{
			IdleEnemy = TestPickEnemy(L, P->GetActorLocation(), INDEX_NONE);
			E = Enemy(IdleEnemy);
		}
		if (!E || !IdlePlaceNear(L, P, IdleEnemy))
		{
			IdleCheck(false, TEXT("no bad guy could be set by the host after its shot"));
		}
		else
		{
			E->CoopChooseTarget(true);
			IdleCheck(E->CoopTarget == 0, FString::Printf(TEXT("the host active again: a bad guy nearer it, facing it, goes for the %s"), IdleWhoOf(E->CoopTarget)));
		}
		TestStep = 827;
		TestAt = Now + 1.0;
		return;
	}
	case 827:
		if (Now < TestAt) return;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP idlehost RESULT: %d passed, %d failed"), IdlePass, IdleFail);
		TestSay(TEXT("quit"));
		TestStep = 100;
		TestAt = Now + 2.0;
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
