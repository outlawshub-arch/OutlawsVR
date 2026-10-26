// Story co-op (tools/olue/COOP_DESIGN.md), milestone 4: the enemies.  The host's bad guys think for both players: wherever
// they ask about "the player" (AOutlawsEnemy::PlayerView) they may now be after the client's player, seen through its
// pose, and their shots at it hurt it on the client's own machine.  The client's bad guys are puppets of the host's: no
// thinking, shooting or hearing; they take place, facing, state, choreography and health from the host about 15 times a
// second, smoothed; a death comes as a message of its own.  The client's hits on them go to the host, which deals them.
// In a session the bad guys are 1.5 times as quick.
//
// None of this runs in single player: AOutlawsEnemy::CoopRole and CoopTarget stay 0 and the level actor's hooks ask
// UOutlawsCoop::IsActive first.
#include "OutlawsCoop.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsEnemy.h"
#include "OutlawsSpriteActor.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "Engine/NetDriver.h"
#include "Misc/CommandLine.h"

namespace
{
	const TCHAR* WhoOf(int32 Who) { return UOutlawsCoop::WhoWord(Who); }
	constexpr float CoopPace = 1.5f;              // the user's "1.5 times as effective"
	constexpr double SnapInterval = 1.0 / 15.0;   // the host's enemies out about 15 times a second
	constexpr float PuppetEase = 12.f;            // how fast a puppet closes on the host's place (per second)
}

// ---- the link's milestone 4 messages ----------------------------------------------------------------------------------

void AOutlawsCoopLink::ServerEnemyHit_Implementation(int32 Epoch, int32 Index, float Damage, FVector_NetQuantize10 Point, FVector_NetQuantizeNormal Dir, FVector_NetQuantize10 From, uint8 Flags, float KnockScale)
{
	if (!Admit(EOLGate::EnemyHit, Index >= 0 && Index < 100000 && Finite(Damage, 1000.f) && Damage >= 0.f && Finite(Point) && Finite(Dir, 2.0) && Finite(From) && Finite(KnockScale, 100.f))) return;
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnEnemyHit(PlayerIndex, Epoch, Index, Damage, Point, Dir, From, Flags, KnockScale);
}

void AOutlawsCoopLink::ServerNoise_Implementation(int32 Epoch, FVector_NetQuantize10 Where, float Radius)
{
	if (!Admit(EOLGate::Noise, Finite(Where) && Finite(Radius, 1.0e5f) && Radius >= 0.f)) return;
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnNoise(PlayerIndex, Epoch, Where, Radius);
}

void AOutlawsCoopLink::ClientEnemyDied_Implementation(int32 Epoch, int32 Index, int32 Chor, FVector_NetQuantizeNormal Dir)
{
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnEnemyDied(Epoch, Index, Chor, Dir);
}

// ---- the enemy's co-op side -------------------------------------------------------------------------------------------

void AOutlawsEnemy::CoopSetup(UOutlawsCoop* InCoop, int32 Index, bool bHost)
{
	Coop = InCoop;
	CoopIndex = Index;
	CoopRole = bHost ? 1 : 2;
	CoopTarget = 0;
	if (bHost)
	{
		// (the first look waits for the view to settle on the new level: on the build's own frame the camera is still
		// where the lobby left it, and both players' copies of the hideout alerted a dozen bad guys from there)
		LosWait = FMath::Max(LosWait, 0.75f);
		return;
	}
	// the client's copy has no movement of its own (the host's place is shown); it counts as standing, so its blood pool
	// and a critter's remains still come when the body lies still
	UCharacterMovementComponent* Move = GetCharacterMovement();
	Move->StopMovementImmediately();
	Move->SetMovementMode(MOVE_Walking);
	Move->Deactivate();
	PendingKnock = FVector::ZeroVector;
	bPuppetPlaced = false;
	bPuppetDeadSynced = false;
	PuppetPos = GetActorLocation();
	PuppetYaw = Heading;
}

FString AOutlawsEnemy::CoopDescribe() const
{
	static const TCHAR* Names[] = { TEXT("watch"), TEXT("chase"), TEXT("shoot"), TEXT("hit"), TEXT("dead") };
	return FString::Printf(TEXT("at %s state %s"), *GetActorLocation().ToCompactString(), Names[FMath::Clamp(int32(GetState()), 0, 4)]);
}

float AOutlawsEnemy::CoopLagCm() const
{
	return CoopRole == 2 && bPuppetPlaced ? float(FVector::Dist(GetActorLocation(), PuppetPos)) : -1.f;
}

bool AOutlawsEnemy::CoopTargetView(FVector& OutEye, FVector& OutFeet, float& OutRadius, float& OutHalf) const
{
	// (N1: the client player CoopTarget names, as the host has it this frame)
	const UOutlawsCoop* C = Coop.Get();
	const FOutlawsCoopRemoteView* V = C ? C->RemoteViewOf(CoopTarget) : nullptr;
	if (!V || !V->bValid) return false;
	OutEye = V->Eye;
	OutFeet = V->Feet;
	OutRadius = V->Radius;
	OutHalf = V->Half;
	return true;
}

bool AOutlawsEnemy::TargetDead(const AOutlawsLevelActor* L) const
{
	// (2026-10-02: a player in the pause menu counts as gone too, while it is open: UOutlawsCoop::bMenuKeepsSafe)
	if (CoopTarget != 0)
	{
		const UOutlawsCoop* C = Coop.Get();
		const FOutlawsCoopRemoteView* V = C ? C->RemoteViewOf(CoopTarget) : nullptr;
		return !V || !V->bValid || V->bDead || V->bAway;     // (R7: a player who left is gone too)
	}
	if (CoopRole == 1)
	{
		const UOutlawsCoop* C = Coop.Get();
		if (C && UOutlawsCoop::bMenuKeepsSafe && C->bLocalAway) return true;
	}
	return L->IsPlayerDead();
}

bool AOutlawsEnemy::CoopRemoteNear(double Cm) const
{
	// (N1: any client's player)
	const UOutlawsCoop* C = Coop.Get();
	if (!C) return false;
	for (const FOutlawsCoopRemoteView& V : C->RemoteViews)
	{
		if (V.bValid && FVector::DistSquared(V.Feet, GetActorLocation()) <= Cm * Cm) return true;
	}
	return false;
}

void AOutlawsEnemy::CoopChooseTarget(bool bSay)
{
	// the host's bad guy and the players: the one it can see; several or none, the nearest (once it is after one, another only
	// once that one is a fifth nearer, so it does not swap to and fro; a fresh alert takes the nearest); a dead player is
	// nobody's target.  (N1: the same rules among every player in the session; with two, as before.)  BUG-BOSSIDLEHOST:
	// the active players first; an idle one only when no active one is left (OutlawsCoopIdle.cpp)
	UOutlawsCoop* C = Coop.Get();
	const AOutlawsLevelActor* L = Level.Get();
	const UWorld* W = GetWorld();
	const APlayerController* PC = W ? W->GetFirstPlayerController() : nullptr;
	const bool bLocal = PC && PC->GetPawn() && L && !L->IsPlayerDead() && !(C && UOutlawsCoop::bMenuKeepsSafe && C->bLocalAway)
		&& !(C && C->PlayerInLobby(0));          // (REQ-MPLOBBY: the host still in the lobby is not in the level)
	TArray<int32, TInlineAllocator<UOutlawsCoop::MaxPlayers>> Live;
	if (bLocal && (CoopSelf < 0 || !C || C->BotFoe(CoopSelf, 0))) Live.Add(0);
	for (int32 k = 1; C && k < UOutlawsCoop::MaxPlayers; ++k)
	{
		const FOutlawsCoopRemoteView& V = C->RemoteViews[k];
		if (V.bValid && !V.bDead && !V.bAway && (CoopSelf < 0 || C->BotFoe(CoopSelf, k))) Live.Add(k);      // (REQ-MPBOTS: a bot's target is never itself nor a teammate)
	}
	if (C && CoopSelf < 0 && Live.Num() > 1 && C->IdleRuleOn())
	{
		TArray<int32, TInlineAllocator<UOutlawsCoop::MaxPlayers>> Busy;
		for (const int32 k : Live) if (!C->IsPlayerIdle(k)) Busy.Add(k);
		if (Busy.Num() > 0 && Busy.Num() < Live.Num()) Live = Busy;
	}
	int32 Want = CoopTarget;
	if (Live.Num() == 1) Want = Live[0];
	else if (Live.Num() > 1)
	{
		const int32 Was = CoopTarget;
		bool bSee[UOutlawsCoop::MaxPlayers] = {};
		double Dist[UOutlawsCoop::MaxPlayers] = {};
		bool bAnySeen = false;
		for (const int32 k : Live)
		{
			FVector Eye, Feet;
			float R = 0.f, H = 0.f;
			CoopTarget = k;
			bSee[k] = SightCheck();
			PlayerView(Eye, Feet, R, H);
			Dist[k] = FVector::Dist(Feet, GetActorLocation());
			bAnySeen = bAnySeen || bSee[k];
		}
		CoopTarget = Was;
		// those it sees if any, else all; among them the nearest, the one it is after kept unless another is a fifth nearer
		const double Margin = bSay ? 1.0 : 0.8;
		const bool bWasLive = Live.Contains(Was) && (!bAnySeen || bSee[Was]);
		int32 Best = -1;
		for (const int32 k : Live)
		{
			if (bAnySeen && !bSee[k]) continue;
			if (Best < 0 || Dist[k] < Dist[Best]) Best = k;
		}
		if (Best >= 0) Want = !bWasLive ? Best : (Best != Was && Dist[Best] < Dist[Was] * Margin) ? Best : Was;
	}
	if (Want == CoopTarget && !bSay) return;
	CoopTarget = Want;
	if (C && (bSay || State != EOutlawsEnemyState::Watch)) C->NoteEnemyTarget(this);
}

void AOutlawsEnemy::HearShotBy(const FVector& Where, int32 Who)
{
	// HearShot looks for the shooter: the player who fired; one that does not alert keeps the target it had
	if (CoopRole != 1 || State != EOutlawsEnemyState::Watch || bDormant)
	{
		HearShot(Where);
		return;
	}
	const int32 Was = CoopTarget;
	CoopTarget = Who;
	HearShot(Where);
	if (State == EOutlawsEnemyState::Watch) CoopTarget = Was;
}

void AOutlawsEnemy::CoopReportHit(float Damage, const FVector& Point, const FVector& Dir, const FVector& From, uint8 Flags, float KnockScale)
{
	// the hit shows here at once (blood where it struck); the host deals it, and the health, the flinch or the death come
	// back with its word
	if ((Flags & 2) == 0 && Damage > 0.f)
	{
		if (AOutlawsLevelActor* L = Level.Get()) L->SpawnBlood(Point, Dir, IsDead() ? 1 : FMath::Clamp(FMath::CeilToInt(Damage), 1, 4));
		PuppetHitSentAt = Now();
	}
	if (UOutlawsCoop* C = Coop.Get()) C->ReportEnemyHit(this, Damage, Point, Dir, From, Flags, KnockScale);
}

void AOutlawsEnemy::CoopFillSnap(FOutlawsCoopEnemySnap& S) const
{
	S.Pos = GetActorLocation();
	S.Yaw = uint16(FMath::RoundToInt(FRotator::ClampAxis(Heading) * (65536.f / 360.f)) & 0xFFFF);
	S.State = uint8(State);
	S.Sub = uint8(FMath::Clamp(Sub, 0, 255));
	S.Chor = int16(Sprite ? Sprite->GetPlayingChor() : -1);
	S.Plays = CoopPlays;
	S.Shots = CoopShots;
	S.Flags = uint8((Sprite && Sprite->IsChorLooping() ? 1 : 0) | (bDormant ? 2 : 0) | (bHeldByPlayer || bThrown ? 4 : 0) | (CoopTarget != 0 ? 8 : 0));
	S.Health = Health;
}

void AOutlawsEnemy::CoopApplySnap(const FOutlawsCoopEnemySnap& S)
{
	AOutlawsLevelActor* L = Level.Get();
	// (CPU2 b: its speed from the last two snapshots, carried on a little past the newest in TickPuppet; none while dead, held,
	// flying from a hand, hidden, or after a gap or a jump too long to be walking)
	// (CPU2 d) applied when its snapshot changes (TickEnemies), so the speed is from its last two places and the time
	// between them; a far, throttled one's place comes every 2nd or 4th snapshot (Flags 16, 32): carried on further
	const double SnapNow = FPlatformTime::Seconds();
	const bool bStillKind = (S.Flags & (2 | 4)) != 0 || S.State == uint8(EOutlawsEnemyState::Dead);
	const int32 Every = (S.Flags & 32) ? 4 : (S.Flags & 16) ? 2 : 1;
	PuppetAhead = Every == 4 ? 0.35f : Every == 2 ? 0.2f : 0.1f;
	const double MaxGap = Every == 4 ? 0.8 : Every == 2 ? 0.45 : 0.3;
	if (bStillKind) PuppetVel = FVector::ZeroVector;
	if (!bPuppetPlaced || FVector(S.Pos) != PuppetPos)
	{
		const double SnapGap = SnapNow - PuppetSnapAt;
		PuppetVel = FVector::ZeroVector;
		if (bPuppetPlaced && !bStillKind && SnapGap > 0.02 && SnapGap < MaxGap)
		{
			const FVector V = (FVector(S.Pos) - PuppetPos) / SnapGap;
			if (V.SizeSquared() < FMath::Square(1500.0)) PuppetVel = V;
		}
		PuppetSnapAt = SnapNow;
		PuppetPos = S.Pos;
	}
	PuppetYaw = float(S.Yaw) * (360.f / 65536.f);
	// (milestone 6 item 4: the host's copy held or flying, by either player; while this player's own hand has this one, or
	// its throw is in the air or just down, the host's choreography is noted here, not shown: CoopLocalCarry)
	bPuppetHostCarried = (S.Flags & 4) != 0;
	if (bPuppetHostCarried && CoopLocalCarry()) bPuppetSawHostCarry = true;
	const bool bFirst = !bPuppetPlaced;
	if (bFirst)
	{
		bPuppetPlaced = true;
		SetActorLocation(PuppetPos, false, nullptr, ETeleportType::TeleportPhysics);
		Heading = PuppetYaw;
		PuppetShots = S.Shots;
	}
	// hidden while the host's is (a boss before he appears)
	const bool bDorm = (S.Flags & 2) != 0;
	if (bDorm != bDormant)
	{
		SetDormant(bDorm);
		GetCharacterMovement()->Deactivate();      // (SetDormant wakes the movement: the copy has none of its own)
	}
	const EOutlawsEnemyState NewState = static_cast<EOutlawsEnemyState>(FMath::Min<uint8>(S.State, uint8(EOutlawsEnemyState::Dead)));
	if (NewState == EOutlawsEnemyState::Dead)
	{
		// (Q2 step 4: a body already down when first placed here is put back, not killed: no cry; 121 at once on a late join)
		if (!IsDead()) CoopPuppetDie(S.Chor, FRotator(0.f, PuppetYaw + 180.f, 0.f).Vector(), bFirst ? TEXT("from its state, put back") : TEXT("from its state"), bFirst);
		// (the host's body played its death again where it landed: here too)
		if (bPuppetDeadSynced && S.Plays != PuppetPlays && S.Chor >= 0 && !CoopLocalCarry()) Play(S.Chor, false);
		PuppetPlays = S.Plays;
		bPuppetDeadSynced = true;
	}
	else if (!IsDead() && CoopLocalCarry())
	{
		PuppetChor = S.Chor;
		PuppetPlays = S.Plays;
	}
	else if (!IsDead())
	{
		if (State != NewState)
		{
			State = NewState;
			StateTime = 0.f;
		}
		Sub = S.Sub;
		if (bFirst || S.Chor != PuppetChor || S.Plays != PuppetPlays)
		{
			PuppetChor = S.Chor;
			PuppetPlays = S.Plays;
			if (S.Chor >= 0) Play(S.Chor, (S.Flags & 1) != 0);
		}
	}
	// its shots: the gun's sound and flash here too (the shot itself was the host's)
	if (S.Shots != PuppetShots)
	{
		PuppetShots = S.Shots;
		if (L)
		{
			const FVector Gun = GetActorLocation() + FVector(0.0, 0.0, HeightCm() * 0.25) + FRotator(0.0, Heading, 0.0).Vector() * GetCapsuleComponent()->GetScaledCapsuleRadius();
			L->PlaySoundAt(Def.ShootSound, Gun);
			if (AOutlawsLevelActor::IsGunSound(Def.ShootSound)) L->MuzzleFlash(Gun, 0.8f);
		}
	}
	// its health: a hit shows here (blood unless this machine's own hit already drew it, its cry)
	if (Health != S.Health)
	{
		const float Was = Health;
		Health = S.Health;
		if (!bFirst)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP enemy %d health %.2f (client; was %.2f)"), CoopIndex, Health, Was);
			if (Health < Was && L && !IsDead())
			{
				if (Now() - PuppetHitSentAt > 0.6)
				{
					L->SpawnBlood(GetActorLocation() + FVector(0.0, 0.0, HeightCm() * 0.2), FRotator(0.f, Heading + 180.f, 0.f).Vector(), FMath::Clamp(FMath::CeilToInt(Was - Health), 1, 4));
				}
				L->PlaySoundAt(Def.HitSound, GetActorLocation());
			}
		}
	}
}

void AOutlawsEnemy::CoopPuppetDie(int32 Chor, const FVector& Dir, const TCHAR* How, bool bQuiet)
{
	// as Die, for the copy: the death choreography, no longer solid to bullets or bodies, the cry; no drop (the host's drop
	// comes as a ground item)
	if (IsDead()) return;
	WakeTick();
	if (CoopLocalCarry()) CoopCancelHeld(TEXT("it died on the host"));     // (milestone 6 item 4: as Die lets go of a held one)
	if (bDormant)
	{
		SetDormant(false);
		GetCharacterMovement()->Deactivate();
	}
	const int32 C = Chor >= 0 ? Chor : Def.ChestDieChor;
	SetBodyHalf(StandHalf);
	State = EOutlawsEnemyState::Dead;
	StateTime = 0.f;
	// (BUG-DRDEATHANIM: Doc and Bob dying into their level's cutscene: here too as he stood, no death played; a body put
	// back on a late join lies as it fell)
	const AOutlawsLevelActor* LE = Level.Get();
	if (bFallInCutscene && bBoss && !bQuiet && LE && LE->EndMovieReady())
	{
		HoldForCutscene(TEXT("the client's copy"));
	}
	else
	{
		Heading = (-Dir).Rotation().Yaw;
		Play(C, false);
	}
	DeadChor = C;
	PuppetChor = C;
	UCapsuleComponent* Cap = GetCapsuleComponent();
	Cap->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
	Cap->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
	Cap->SetCollisionResponseToChannel(ECC_GameTraceChannel2, ECR_Ignore);
	if (AOutlawsLevelActor* L = Level.Get())
	{
		if (!bQuiet) L->PlaySoundAt(Def.DieSound, GetActorLocation());
		L->OnEnemyKilled(this);
	}
	if (UOutlawsCoop* Co = Coop.Get())
	{
		++Co->EnemyDeathsSeen;
		if (bQuiet) ++Co->JoinReplayBodies;
	}
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP enemy %d died (client) (%s, %s, chor %d)"), CoopIndex, *OutlawsName, How, C);
}

void AOutlawsEnemy::TickPuppet()
{
	// eased toward the host's place in real seconds (the pace is the host's to apply: its place already carries it)
	const UWorld* W = GetWorld();
	const float Dt = W ? FMath::Min(W->GetDeltaSeconds(), 0.1f) : 0.f;
	StateTime += Dt;
	SubTime += Dt;
	if (CoopLocalCarry())
	{
		CoopTickCarry(Dt);                       // (milestone 6 item 4: in this player's hand or flying from it; OutlawsCoopGrabs.cpp)
		if (CoopLocalCarry()) return;
	}
	if (bPuppetPlaced && !bDormant && Dt > 0.f)
	{
		// (CPU2 b) the host's place carried on at its speed (at most 0.1 s past the newest snapshot), the gap to it closed
		// smoothly; carried along at that speed too, so a walker or a lift's rider does not trail behind
		const FVector Here = GetActorLocation();
		const double Since = FPlatformTime::Seconds() - PuppetSnapAt;
		if (Since > PuppetAhead * 1.5) PuppetVel = FVector::ZeroVector;      // (CPU2 d: no new place when one was due: it stopped)
		const double Ahead = FMath::Clamp(Since, 0.0, double(PuppetAhead));
		const FVector Goal = PuppetPos + PuppetVel * Ahead;
		const FVector Next = FVector::DistSquared(Here, Goal) > FMath::Square(500.0) ? Goal
			: Here + (PuppetVel.IsZero() || Ahead >= PuppetAhead ? FVector::ZeroVector : PuppetVel * Dt) + (Goal - Here) * FMath::Min(1.f, Dt * PuppetEase);
		if (FVector::DistSquared(Here, Next) > 0.0001)
		{
			SetActorLocation(Next, false, nullptr, ETeleportType::TeleportPhysics);
		}
		GetCharacterMovement()->Velocity = (Next - Here) / Dt;
		Heading = FRotator::NormalizeAxis(Heading + FRotator::NormalizeAxis(PuppetYaw - Heading) * FMath::Min(1.f, Dt * PuppetEase));
	}
	// a boss down: the level's own end, as the host's (the level's flow is not shared yet)
	if (IsDead() && bBoss && !bBossReported)
	{
		bBossReported = true;
		if (AOutlawsLevelActor* L = Level.Get()) L->OnBossDefeated();
	}
	if (bHeldForCutscene && StateTime > 1.5f)
	{
		// (the host's word of the level's end did not come: his death after all)
		const AOutlawsLevelActor* L = Level.Get();
		if (!L || (!L->DebugLevelEnding() && !L->IsMoviePlaying())) FallbackFromHold();
	}
	if (Sprite) Sprite->OutlawsYaw = Heading;
}

bool AOutlawsEnemy::TickPuppetCulled(float Dt)
{
	// (CPU2 b) far from this player (15 m) and in a sector this player's view does not reach: no smoothing, no floor, light or
	// blood looks; put at the host's place four times a second.  Never a boss, one dormant, one not yet placed, one this
	// player holds or threw (or the host's held or flying), nor one dead less than 3 s (its fall plays out where it is seen
	// first); looked at again every quarter second.
	AOutlawsLevelActor* L = Level.Get();
	PuppetCullCheck -= Dt;
	if (PuppetCullCheck <= 0.f)
	{
		PuppetCullCheck = 0.25f;
		bool bCull = L && bPuppetPlaced && !bBoss && !bDormant && !CoopLocalCarry() && !bPuppetHostCarried && !(IsDead() && StateTime < 3.f);
		if (bCull)
		{
			const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
			const APawn* P = PC ? PC->GetPawn() : nullptr;
			const FVector Here = GetActorLocation();
			bCull = P && FVector::DistSquared(P->GetActorLocation(), Here) > FMath::Square(bPuppetCulled ? 1350.0 : 1500.0);
			if (bCull)
			{
				if (FreezeSector == -2 || FVector::DistSquared(Here, FreezeSectorAt) > 900.0)
				{
					FreezeSector = L->CullSectorOf(Here, &CullHint);
					FreezeSectorAt = Here;
				}
				bCull = !L->IsSectorSeen(FreezeSector);
			}
		}
		if (bCull != bPuppetCulled)
		{
			bPuppetCulled = bCull;
			SetActorTickInterval(bCull ? 0.25f : 0.f);
			if (!bCull) LightCheck = 0.f;            // (its light looked at again at once)
		}
		if (bCull && GetActorTickInterval() < 0.25f) SetActorTickInterval(0.25f);     // (a wake set it back to every frame)
	}
	if (!bPuppetCulled) return false;
	const float Real = Dt / FMath::Max(CustomTimeDilation, 0.01f);     // (the actor's Dt carries its pace)
	StateTime += Real;
	SubTime += Real;
	if (!PuppetPos.Equals(GetActorLocation(), 1.0)) SetActorLocation(PuppetPos, false, nullptr, ETeleportType::TeleportPhysics);
	GetCharacterMovement()->Velocity = PuppetVel;
	Heading = PuppetYaw;
	if (Sprite) Sprite->OutlawsYaw = Heading;
	return true;
}

// ---- the session's side -----------------------------------------------------------------------------------------------

void UOutlawsCoop::SetupEnemies(AOutlawsLevelActor* L)
{
	const TArray<TWeakObjectPtr<AOutlawsEnemy>>& List = L->GetEnemies();
	for (int32 i = 0; i < List.Num(); ++i)
	{
		if (AOutlawsEnemy* E = List[i].Get()) E->CoopSetup(this, i, IsHost());
	}
	L->CoopPaceScale = CoopPace;
	NextEnemySnapAt = 0.0;
	EnemyApplied.Reset();
	bEnemyRates = !OLFlags::Param(TEXT("OLEnemyRatesOff"));
	bEnemyApplyAll = OLFlags::Param(TEXT("OLEnemyApplyAll"));
	bEnemyCountWarned = false;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP pace x1.5: enemy pace %.3f (%.3f in single player at this speed)"), L->EnemyPace(), L->EnemyPace() / CoopPace);
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP enemies set up: %d, %s"), List.Num(), IsHost() ? TEXT("thinking here for both players") : TEXT("puppets of the host's"));
}

void UOutlawsCoop::CheckRebuilt(AOutlawsLevelActor* L)
{
	// a death's restart builds this machine's level again on its own: its new enemies and pickups are not the session's yet
	bool bEnemiesNew = false, bPickupsNew = false;
	for (const TWeakObjectPtr<AOutlawsEnemy>& W : L->GetEnemies())
	{
		if (const AOutlawsEnemy* E = W.Get()) { bEnemiesNew = E->CoopRole == 0; break; }
	}
	for (const AOutlawsLevelActor::FPickup& P : L->Pickups)
	{
		if (P.Serial >= 0) { bPickupsNew = P.CoopId < 0; break; }
	}
	if (!bEnemiesNew && !bPickupsNew) return;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP this machine's level was built again on its own (a restart): its %s set up for the session again"),
		bEnemiesNew && bPickupsNew ? TEXT("enemies and pickups") : bEnemiesNew ? TEXT("enemies") : TEXT("pickups"));
	if (bPickupsNew) AssignPickupIds(L);
	if (bEnemiesNew) SetupEnemies(L);
	if (L->Props.Num() > 0 && L->Props[0].CoopId < 0) SetupVerbs(L);      // (milestone 6: its props numbered again)
}

void UOutlawsCoop::TickEnemies(double Now)
{
	if (!bLevelBuilt) return;
	AOutlawsLevelActor* L = FindLevel();
	AOutlawsCoopSession* S = GetSession();
	if (!L || !S) return;
	CheckRebuilt(L);
	const TArray<TWeakObjectPtr<AOutlawsEnemy>>& List = L->GetEnemies();
	if (IsHost())
	{
		// each client's player as the enemies see it: its body, its eyes, alive or not (from its pose and its stats; N1: all)
		for (FOutlawsCoopRemoteView& V : RemoteViews) V = FOutlawsCoopRemoteView();
		TArray<AOutlawsCoopLink*> Links;
		GetLinks(Links);
		for (const AOutlawsCoopLink* CL : Links)
		{
			if (CL->PlayerIndex <= 0 || CL->Pose.Seq <= 0 || CL->BuiltSerial != StartedSerial) continue;
			FOutlawsCoopRemoteView& V = RemoteViews[CL->PlayerIndex];
			V.bValid = true;
			V.Half = CL->Pose.HalfHeight;
			V.Feet = FVector(CL->Pose.Body) - FVector(0.0, 0.0, CL->Pose.HalfHeight);
			V.Eye = CL->Pose.Head;
			V.bDead = CL->Stats.Seq > 0 && CL->Stats.bDead;
			V.bAway = bMenuKeepsSafe && CL->Stats.Seq > 0 && CL->Stats.bInMenu;      // (2026-10-02: in its pause menu, out of harm)
			V.bAway = V.bAway || CL->bAppPaused;     // (BUG-COOPTIMEOUT: its app paused, its place held: out of harm too)
			V.bAway = V.bAway || PlayerInLobby(CL->PlayerIndex);     // (REQ-MPLOBBY: still in the lobby: not in the level yet)
			if (const AOutlawsPlayer* P = LocalPlayer()) V.Radius = P->GetCapsuleComponent()->GetScaledCapsuleRadius();
		}
		TickIdleWatch(Now);                      // (BUG-BOSSIDLEHOST: who is idle, OutlawsCoopIdle.cpp)
		// every enemy's place and doing, about 15 times a second (replication sends what changed).  CPU2 d: a throttled
		// one's place (its doing is sent at once whatever its rate: a shot, a death, a hit, a choreography, held) every 2nd
		// snapshot in MOVE-ONLY, every 4th in WANDER or FROZEN; never one within 15 m of a player, fighting (FULL, firing,
		// flinching), a boss, held, thrown, dead or on a lift (the throttle's exempt ones)
		if (Now >= NextEnemySnapAt)
		{
			NextEnemySnapAt = Now + SnapInterval;
			S->EnemyEpoch = StartedSerial;
			const int32 Had = S->Enemies.Num();
			S->Enemies.SetNum(List.Num());
			TArray<FVector, TInlineAllocator<MaxPlayers>> Near;
			if (const AOutlawsPlayer* P = LocalPlayer()) Near.Add(P->GetActorLocation());
			for (const FOutlawsCoopRemoteView& V : RemoteViews)
			{
				if (V.bValid) Near.Add(V.Feet + FVector(0.0, 0.0, V.Half));
			}
			for (int32 i = 0; i < List.Num(); ++i)
			{
				const AOutlawsEnemy* E = List[i].Get();
				if (!E) continue;
				FOutlawsCoopEnemySnap New;
				E->CoopFillSnap(New);
				int32 Every = 1;
				if (bEnemyRates && E->ThrottleTier > 0 && !L->IsThrottleExempt(E) && E->State != EOutlawsEnemyState::Shoot)
				{
					Every = E->ThrottleTier == 1 ? 2 : 4;
					const FVector At = E->GetActorLocation();
					for (const FVector& F : Near)
					{
						if (FVector::DistSquared(F, At) < FMath::Square(1500.0)) { Every = 1; break; }
					}
				}
				New.Flags |= Every == 4 ? 32 : Every == 2 ? 16 : 0;
				FOutlawsCoopEnemySnap& Cur = S->Enemies[i];
				const bool bDoing = i >= Had || New.State != Cur.State || New.Sub != Cur.Sub || New.Chor != Cur.Chor || New.Plays != Cur.Plays
					|| New.Shots != Cur.Shots || New.Flags != Cur.Flags || New.Health != Cur.Health;
				if (bDoing || Every == 1 || (EnemySnapsSent + i) % Every == 0)
				{
					Cur = New;
				}
				else
				{
					++EnemyPlacesHeld;
				}
			}
			++EnemySnapsSent;
		}
		return;
	}
	if (!IsClient() || S->EnemyEpoch != StartedSerial || S->Enemies.Num() == 0 || List.Num() == 0) return;     // (none here: the level cleared for the movie)
	if (S->Enemies.Num() != List.Num() && !bEnemyCountWarned)
	{
		bEnemyCountWarned = true;
		UE_LOG(LogOutlawsCoop, Warning, TEXT("OLCOOP the host has %d enemies, this machine %d: the first %d are shown"), S->Enemies.Num(), List.Num(), FMath::Min(S->Enemies.Num(), List.Num()));
	}
	const int32 N = FMath::Min(S->Enemies.Num(), List.Num());
	const int32 PlaceBudget = JoinPlaceBudget();
	int32 Placed = 0, First = 0;
	const double PlaceT0 = FPlatformTime::Seconds();
	// (CPU2 d) each one applied when its snapshot changed, while it is held or flying (by either player), not yet placed,
	// or hidden here unlike the host's; and each one again about twice a second whatever (a slip here put right)
	if (EnemyApplied.Num() != N)
	{
		EnemyApplied.Reset();
		EnemyApplied.SetNum(N);
	}
	for (int32 i = 0; i < N; ++i)
	{
		AOutlawsEnemy* E = List[i].Get();
		if (!E) continue;
		const FOutlawsCoopEnemySnap& Sn = S->Enemies[i];
		FOutlawsCoopEnemySnap& Was = EnemyApplied[i];
		const bool bChanged = Sn.Pos != Was.Pos || Sn.Yaw != Was.Yaw || Sn.State != Was.State || Sn.Sub != Was.Sub || Sn.Chor != Was.Chor
			|| Sn.Plays != Was.Plays || Sn.Shots != Was.Shots || Sn.Flags != Was.Flags || Sn.Health != Was.Health;
		const bool bHot = !E->bPuppetPlaced || E->CoopLocalCarry() || E->bPuppetHostCarried || (Sn.Flags & 4) != 0 || E->bDormant != ((Sn.Flags & 2) != 0);
		if (!bChanged && !bHot && !bEnemyApplyAll && (GFrameCounter + uint64(i)) % 30 != 0) continue;
		if (!E->bPuppetPlaced && PlaceBudget != 0)
		{
			// (Q2 step 4) a late join: the first snapshots a few a frame, none in the level's build frame; the rest wait
			// (still not placed: hot next frame)
			if (Placed >= FMath::Max(0, PlaceBudget)) continue;
			++Placed;
		}
		if (!E->bPuppetPlaced) ++First;
		Was = Sn;
		E->CoopApplySnap(Sn);
		++EnemyApplies;
	}
	if (JoinReplayStep == 1 && First > 0)
	{
		JoinReplayPlaced += First;
		if (JoinReplayLastPlaceFrame != GFrameCounter) { JoinReplayLastPlaceFrame = GFrameCounter; ++JoinReplayPlaceFrames; }
		NoteJoinReplayWork((FPlatformTime::Seconds() - PlaceT0) * 1000.0);
	}
	++EnemySnapsApplied;
}

void UOutlawsCoop::ReportEnemyHit(const AOutlawsEnemy* E, float Damage, const FVector& Point, const FVector& Dir, const FVector& From, uint8 Flags, float KnockScale)
{
	AOutlawsCoopLink* Mine = GetLink(MyIndex());
	if (!E || !Mine || !IsClient() || E->CoopIndex < 0) return;
	Mine->ServerEnemyHit(LevelEpoch(), E->CoopIndex, Damage, Point, Dir.GetSafeNormal(), From, Flags, KnockScale);
	++EnemyHitsSent;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP hit sent: enemy %d damage %.2f%s"), E->CoopIndex, Damage, (Flags & 2) ? TEXT(" (a shove)") : (Flags & 1) ? TEXT(" (a blast)") : TEXT(""));
}

void UOutlawsCoop::OnEnemyHit(int32 Who, int32 Epoch, int32 Index, float Damage, const FVector& Point, const FVector& Dir, const FVector& From, uint8 Flags, float KnockScale)
{
	if (!IsHost()) return;
	RunFor(Epoch, [this, Who, Index, Damage, Point, Dir, From, Flags, KnockScale]()
	{
		AOutlawsLevelActor* L = FindLevel();
		const TArray<TWeakObjectPtr<AOutlawsEnemy>>* List = L ? &L->GetEnemies() : nullptr;
		AOutlawsEnemy* E = List && List->IsValidIndex(Index) ? (*List)[Index].Get() : nullptr;
		if (!E)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP hit reported: enemy %d is not here"), Index);
			return;
		}
		const float Before = E->GetHealth();
		FOutlawsStatsScope StruckBy(Who >= 1 ? StatsByPlayer(Who) : EOutlawsStatsBy::Me, (Flags & 1) != 0);     // (the level's end panels: the client's kill; N1: whose)
		if (Flags & 2)
		{
			E->Knock(Dir, Damage, (Flags & 1) != 0);
		}
		else
		{
			// (a watcher struck turns on the one who struck it, unless the other player is the better target)
			if (E->GetState() == EOutlawsEnemyState::Watch && !E->IsDormant()) E->CoopTarget = Who;
			E->TakeHit(Damage, Point, Dir, From, (Flags & 1) != 0, KnockScale);
		}
		++EnemyHitsDealt;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP hit reported: enemy %d damage %.2f by the %s%s (health %.2f -> %.2f%s)"), Index, Damage, WhoOf(Who),
			(Flags & 2) ? TEXT(", a shove") : (Flags & 1) ? TEXT(", a blast") : TEXT(""), Before, E->GetHealth(), E->IsDead() ? TEXT(", dead") : TEXT(""));
	});
}

void UOutlawsCoop::NoteEnemyDied(const AOutlawsEnemy* E, int32 Chor, const FVector& Dir)
{
	if (!IsHost() || !E) return;
	++EnemyDeathsSent;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP enemy %d died (host) (%s, chor %d)"), E->CoopIndex, *E->GetOutlawsName(), Chor);
	const int32 Epoch = LevelEpoch();
	ForClients([&](AOutlawsCoopLink& CL) { CL.ClientEnemyDied(Epoch, E->CoopIndex, Chor, Dir.GetSafeNormal()); });
}

void UOutlawsCoop::OnEnemyDied(int32 Epoch, int32 Index, int32 Chor, const FVector& Dir)
{
	if (!IsClient()) return;
	RunFor(Epoch, [this, Index, Chor, Dir]()
	{
		AOutlawsLevelActor* L = FindLevel();
		const TArray<TWeakObjectPtr<AOutlawsEnemy>>* List = L ? &L->GetEnemies() : nullptr;
		if (AOutlawsEnemy* E = List && List->IsValidIndex(Index) ? (*List)[Index].Get() : nullptr) E->CoopPuppetDie(Chor, Dir, TEXT("the host's word"));
	});
}

void UOutlawsCoop::HurtFromEnemy(const AOutlawsEnemy* E, float Damage, float LocationMult)
{
	if (!IsHost() || !E) return;
	++EnemyShotsAtClient;
	NotePlayerActive(E->CoopTarget, TEXT("hit by a bad guy"));
	const FVector From = E->GetActorLocation();
	HurtOther(Damage, LocationMult, false, &From, E->CoopIndex, E->CoopTarget);       // (N1: the player it shot at)
}

void UOutlawsCoop::NoteEnemyTarget(const AOutlawsEnemy* E)
{
	if (!E) return;
	FString Idle;
	for (int32 k = 0; k < MaxPlayers; ++k)
	{
		if (k != E->CoopTarget && IsPlayerIdle(k)) Idle += FString::Printf(TEXT("%s%s"), Idle.IsEmpty() ? TEXT("; passed over, idle: the ") : TEXT(", the "), WhoOf(k));
	}
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP enemy %d targets the %s player (%s, %s%s)"), E->CoopIndex, WhoOf(E->CoopTarget), *E->GetOutlawsName(), *E->CoopDescribe(), *Idle);
}

void UOutlawsCoop::NoteNoise(AOutlawsLevelActor* L, const FVector& Where, float RadiusCm)
{
	UOutlawsCoop* C = Get(L);
	if (!C) return;
	if (C->IsHost())
	{
		C->OnNoise(0, C->LevelEpoch(), Where, RadiusCm);
	}
	else if (C->IsClient())
	{
		if (AOutlawsCoopLink* Mine = C->GetLink(C->MyIndex())) Mine->ServerNoise(C->LevelEpoch(), Where, RadiusCm);
	}
}

void UOutlawsCoop::OnNoise(int32 Who, int32 Epoch, const FVector& Where, float Radius)
{
	if (!IsHost()) return;
	NotePlayerActive(Who, TEXT("fired"));
	RunFor(Epoch, [this, Who, Where, Radius]()
	{
		AOutlawsLevelActor* L = FindLevel();
		if (!L) return;
		for (const TWeakObjectPtr<AOutlawsEnemy>& W : L->GetEnemies())
		{
			AOutlawsEnemy* E = W.Get();
			if (E && FVector::Dist(E->GetActorLocation(), Where) < Radius) E->HearShotBy(Where, Who);
		}
	});
}

FString UOutlawsCoop::EnemySummary() const
{
	return FString::Printf(TEXT("snapshots %s %d, hits sent %d, hits dealt %d, deaths sent %d, deaths seen %d, shots that struck the client %d"),
		IsHost() ? TEXT("sent") : TEXT("applied in frames"), IsHost() ? EnemySnapsSent : EnemySnapsApplied, EnemyHitsSent, EnemyHitsDealt, EnemyDeathsSent, EnemyDeathsSeen, EnemyShotsAtClient);
}

// ---- tests (-OLCoopAuto, after milestone 3's) ---------------------------------------------------------------------------

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TestLogEnemy(AOutlawsLevelActor* L, int32 Index, const TCHAR* Why)
{
	const TArray<TWeakObjectPtr<AOutlawsEnemy>>& List = L->GetEnemies();
	const AOutlawsEnemy* E = List.IsValidIndex(Index) ? List[Index].Get() : nullptr;
	if (!E) return;
	const float Lag = E->CoopLagCm();
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP enemy %d %s (%s) [%s; health %.2f%s]"), Index, *E->CoopDescribe(), IsHost() ? TEXT("host") : TEXT("client"), Why, E->GetHealth(),
		Lag >= 0.f ? *FString::Printf(TEXT("; drawn %.0f cm from the host's last word"), Lag) : TEXT(""));
}

int32 UOutlawsCoop::TestPickEnemy(AOutlawsLevelActor* L, const FVector& Near, int32 Not, bool bLevelsOwnOnly, int32 Not2, bool bStable) const
{
	// the nearest living shooter that walks (no boss, no critter, no voice alone), a normal one (not a sniper or an
	// ambusher) if there is one; bLevelsOwnOnly: not one of the extra bad guys a session adds (2026-10-02).  bStable (the
	// grab test, BUG-GRABLAG): not the nearest (the bad guys chase, so the nearest differs run to run) but a BGY1, the plain
	// bad guy, then the shortest, then the first in the level's list, on the floor Near stands on: the same man every run,
	// and one the test's low hand lets go over the floor (BGY4 and BGY6, held by their own grab points, hang lower: let go
	// under it, they slide and stop short of the other player's body)
	const TArray<TWeakObjectPtr<AOutlawsEnemy>>& List = L->GetEnemies();
	for (const bool bNormalOnly : { true, false })
	{
		int32 Best = INDEX_NONE;
		double BestD = 1e30;
		for (int32 i = 0; i < List.Num(); ++i)
		{
			const AOutlawsEnemy* E = List[i].Get();
			if (!E || i == Not || i == Not2 || E->IsDead() || E->IsDormant() || E->bVoiceOnly || E->IsCritter() || E->bBoss || E->bStationary || E->bNotBadGuy) continue;
			if (bLevelsOwnOnly && E->CoopCopyOf >= 0) continue;
			if (bStable && FMath::Abs(E->GetActorLocation().Z - Near.Z) > 150.0) continue;     // (on this floor: one pulled down off a roof dies of the fall)
			if (E->Def.ShootChor < 0 || E->Def.ThrowMax > 0.f || (bNormalOnly && E->Def.BaseMode != 0)) continue;
			const double D = bStable ? (E->OutlawsName.Equals(TEXT("BGY1"), ESearchCase::IgnoreCase) ? 0.0 : 1e9) + double(E->StandHalf) * 1e6 + i
				: FVector::DistSquared(E->GetActorLocation(), Near);
			if (D < BestD) { BestD = D; Best = i; }
		}
		if (Best != INDEX_NONE) return Best;
	}
	return INDEX_NONE;
}

bool UOutlawsCoop::TestPlaceEnemyByClient(AOutlawsLevelActor* L, AOutlawsPlayer* P, int32 Index)
{
	// 2.5 to 4.5 m from the client, in its sight, on a floor, anywhere round it where the host is clearly further (a third)
	const TArray<TWeakObjectPtr<AOutlawsEnemy>>& List = L->GetEnemies();
	AOutlawsEnemy* E = List.IsValidIndex(Index) ? List[Index].Get() : nullptr;
	const AOutlawsCoopLink* CL = GetLink(1);
	UWorld* W = L->GetWorld();
	if (!E || !CL || !W) return false;
	const FVector Client = CL->Pose.Body, Head = CL->Pose.Head;
	FVector Away = (Client - P->GetActorLocation()).GetSafeNormal2D();
	if (Away.IsNearlyZero()) Away = FVector::ForwardVector;
	const float Half = E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsCoopTestEnemy), false, E);
	Q.AddIgnoredActor(P);
	for (const double Dist : { 350.0, 450.0, 250.0 })
	{
		for (int32 k = 0; k < 24; ++k)
		{
			const float Turn = (k & 1 ? -1.f : 1.f) * 15.f * float((k + 1) / 2);                       // 0, 15, -15, 30, -30 ... 180
			const FVector Way = FRotator(0.f, Turn, 0.f).RotateVector(Away);
			const FVector To = Client + Way * Dist;
			if (FVector::Dist2D(To, P->GetActorLocation()) < Dist * 1.3) continue;                                    // (the host well behind)
			FHitResult Hit;
			if (W->LineTraceSingleByChannel(Hit, Head, To + FVector(0.0, 0.0, 40.0), ECC_Visibility, Q)) continue;     // (in the client's sight)
			if (W->LineTraceSingleByChannel(Hit, Client, To, ECC_Visibility, Q)) continue;
			if (!W->LineTraceSingleByChannel(Hit, To + FVector(0.0, 0.0, 60.0), To - FVector(0.0, 0.0, 300.0), ECC_Visibility, Q)) continue;   // (a floor)
			E->SetActorLocation(Hit.ImpactPoint + FVector(0.0, 0.0, Half + 2.0), false, nullptr, ETeleportType::TeleportPhysics);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: enemy %d set %.0f cm from the client (%.0f from the host)"), Index, FVector::Dist(E->GetActorLocation(), Client),
				FVector::Dist(E->GetActorLocation(), P->GetActorLocation()));
			return true;
		}
	}
	return false;
}

void UOutlawsCoop::TickHostEnemyTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	const TArray<TWeakObjectPtr<AOutlawsEnemy>>& List = L->GetEnemies();
	auto Enemy = [&List](int32 i) { return List.IsValidIndex(i) ? List[i].Get() : nullptr; };
	switch (TestStep)
	{
	case 20:
	{
		// 1: the nearest enemy to the client, set on the client's far side and woken: it should go after the client
		const AOutlawsCoopLink* CL = GetLink(1);
		TestEnemy = TestPickEnemy(L, CL ? FVector(CL->Pose.Body) : P->GetActorLocation(), INDEX_NONE);
		if (TestEnemy < 0)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: no enemy to try"));
			TestStep = 25;
			TestAt = Now;
			return;
		}
		TestLogEnemy(L, TestEnemy, TEXT("before"));
		if (!TestPlaceEnemyByClient(L, P, TestEnemy)) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: enemy %d could not be set by the client; woken where it is"), TestEnemy);
		if (AOutlawsEnemy* E = Enemy(TestEnemy)) E->Wake(false);
		TestEnemyLogs = 0;
		TestStep = 21;
		TestAt = Now + 3.0;
		return;
	}
	case 21:
		// 2: where it is on both machines, three times, a second apart, once it has moved
		if (Now < TestAt) return;
		TestLogEnemy(L, TestEnemy, TEXT("asked"));
		TestSay(TEXT("enemy"), TestEnemy);
		if (++TestEnemyLogs < 3)
		{
			TestAt = Now + 1.0;
			return;
		}
		TestStep = 22;
		TestAt = Now + 0.5;
		return;
	case 22:
		// 3: the client hits it (the host deals the hit)
		if (Now < TestAt) return;
		TestSay(TEXT("hit"), TestEnemy);
		TestStep = 23;
		TestAt = Now + 2.0;
		return;
	case 23:
	{
		// 4: the host kills another one; the client must see it die
		if (Now < TestAt) return;
		TestLogEnemy(L, TestEnemy, TEXT("after the client's hit"));
		TestSay(TEXT("enemy"), TestEnemy);
		TestEnemyB = TestPickEnemy(L, P->GetActorLocation(), TestEnemy);
		if (AOutlawsEnemy* B = Enemy(TestEnemyB))
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: the host shoots enemy %d dead (%s)"), TestEnemyB, *B->CoopDescribe());
			const FVector At = B->GetActorLocation();
			B->TakeHit(100.f, At, (At - P->GetActorLocation()).GetSafeNormal(), P->GetActorLocation());
		}
		TestStep = 24;
		TestAt = Now + 2.0;
		return;
	}
	case 24:
		// 5: the client kills the first (the host deals it; both see it die; its drop comes as a ground item)
		if (Now < TestAt) return;
		TestSay(TEXT("kill"), TestEnemy);
		TestStep = 25;
		TestAt = Now + 3.5;
		return;
	case 25:
		if (Now < TestAt) return;
		TestLogEnemy(L, TestEnemy, TEXT("at the end"));
		TestSay(TEXT("enemy"), TestEnemy);
		TestStep = 40;                       // (milestone 5's checks next: OutlawsCoopInf.cpp)
		TestAt = Now + 1.0;
		return;
	default:
		return;
	}
}

// ---- the co-op bench (CPU2 b, -OLCoopReady -OLCoopBench=<s> [-OLCoopBadGuys=4] [-OLPerfDir=<dir>]) ---------------------
// Both players stand where they start and cannot be hurt; at 3 s the host wakes every living bad guy (they come for the
// players), and each machine quits BenchSec seconds later: the client's Saved/Perf (or -OLPerfDir) has its own puppets'
// cost, the Enemies and CMC buckets.

namespace
{
	double CoopBenchNetAt = 0.0;             // (CPU2 d: the net driver's bytes over the bench, from the wake on)
	uint32 CoopBenchOut0 = 0, CoopBenchIn0 = 0;
}

void UOutlawsCoop::TickCoopBench(double Now)
{
	if (!bLevelBuilt || bExitRequested) return;
	AOutlawsLevelActor* L = FindLevel();
	if (!L) return;
	L->bInvincible = true;
	const double Since = Now - LevelBuiltAt;
	if (!bCoopBenchAlerted && Since > 3.0)
	{
		bCoopBenchAlerted = true;
		int32 N = 0, Total = 0;
		for (const TWeakObjectPtr<AOutlawsEnemy>& W : L->GetEnemies())
		{
			AOutlawsEnemy* E = W.Get();
			if (!E) continue;
			++Total;
			if (IsHost() && !E->IsDead() && !E->IsDormant() && !E->IsVoiceOnly()) { E->Wake(false); ++N; }
		}
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLCOOP bench: %s, %d bad guys (%d woken here)"), IsHost() ? TEXT("host") : TEXT("client"), Total, N);
		if (const UNetDriver* D = GetWorld() ? GetWorld()->GetNetDriver() : nullptr)
		{
			CoopBenchNetAt = Now;
			CoopBenchOut0 = D->OutTotalBytes;
			CoopBenchIn0 = D->InTotalBytes;
		}
	}
	if (Since > 3.0 + CoopBenchSec + (IsHost() ? 5.0 : 0.0))
	{
		// (CPU2 d) the bytes this machine sent and took over the bench, a second
		const UNetDriver* D = GetWorld() ? GetWorld()->GetNetDriver() : nullptr;
		if (D && CoopBenchNetAt > 0.0 && Now > CoopBenchNetAt + 1.0)
		{
			const double Secs = Now - CoopBenchNetAt;
			UE_LOG(LogOutlawsCoop, Display, TEXT("OLCOOP bench net: %s sent %.0f B/s, took %.0f B/s over %.0f s (enemy snapshots %d; places held back %lld, enemy applies %lld; rates %s)"), IsHost() ? TEXT("host") : TEXT("client"),
				double(D->OutTotalBytes - CoopBenchOut0) / Secs, double(D->InTotalBytes - CoopBenchIn0) / Secs, Secs, IsHost() ? EnemySnapsSent : EnemySnapsApplied,
				EnemyPlacesHeld, EnemyApplies, bEnemyRates ? TEXT("on") : TEXT("off"));
		}
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLCOOP bench done"));
		QuitForTest();
	}
}
#endif // WITH_OUTLAWS_TESTS
