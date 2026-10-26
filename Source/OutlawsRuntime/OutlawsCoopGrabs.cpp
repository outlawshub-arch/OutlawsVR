// Story co-op (tools/olue/COOP_DESIGN.md), milestone 6 item 4: bad guys grabbed, carried and thrown by either player.
//
// Each player's own hands work on their own machine at once, with the single-player code (AOutlawsPlayer::TryGrabEnemy,
// UpdateEnemyGrab, LetGoEnemy).  The host's grabs were already seen on the client through the enemy snapshot.  New here:
// the client's grabs.  On the client its copy of the bad guy (a puppet of the host's) is taken into its hand there and
// then, so it feels instant; the host is told (ServerEnemyGrab) and holds its own copy for the client: no thinking, his
// friends react as for the host's own grab (BeginHeld), and every frame it is put where the client's pose says the
// client's hand has him.  Let go, the client's copy flies its own arc at once (a simple flight against the level, as the
// puppet has no movement of its own) and the host throws its copy with the same velocity through the same EndHeld; once
// the host's copy is down too, the client's shows the host's place again.  The host decides when both reach for the same
// one (its own grabs are decided at once; a client's grab of one the host already has is refused and let go).
//
// None of this runs in single player: the player's two hooks ask UOutlawsCoop::IsActive first, and the enemy's ask
// CoopRole (0 there).
#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsEnemy.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsRemotePlayer.h"
#include "OutlawsInfRuntime.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "MotionControllerComponent.h"
#include "Engine/World.h"
#include "Misc/App.h"

namespace
{
	const TCHAR* GrabWho(bool bHost) { return bHost ? TEXT("host") : TEXT("client"); }
	const TCHAR* GrabWhoOf(int32 Who) { return UOutlawsCoop::WhoWord(Who); }
	constexpr float HeldEase = 25.f;              // host: how fast its copy closes on the client's hand (per second)
	constexpr double SettleMax = 2.5;             // client: its own throw down, the host's place taken at the latest this long after

	AOutlawsEnemy* EnemyAt(const AOutlawsLevelActor* L, int32 Index)
	{
		const TArray<TWeakObjectPtr<AOutlawsEnemy>>* List = L ? &L->GetEnemies() : nullptr;
		return List && List->IsValidIndex(Index) ? (*List)[Index].Get() : nullptr;
	}
}

// ---- the link's messages ----------------------------------------------------------------------------------------------

void AOutlawsCoopLink::ServerEnemyGrab_Implementation(int32 Epoch, int32 Index, uint8 Hand)
{
	if (!Admit(EOLGate::EnemyGrab, Index >= 0 && Index < 100000 && Hand < 2)) return;
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnEnemyGrab(PlayerIndex, Epoch, Index, Hand);
}

void AOutlawsCoopLink::ServerEnemyRelease_Implementation(int32 Epoch, int32 Index, FVector_NetQuantize10 From, FVector_NetQuantize10 Vel, float Gravity)
{
	if (!Admit(EOLGate::EnemyRelease, Index >= 0 && Index < 100000 && Finite(From) && Finite(Vel, 1.0e5) && Finite(Gravity, 100.f))) return;
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnEnemyRelease(PlayerIndex, Epoch, Index, From, Vel, Gravity);
}

void AOutlawsCoopLink::ClientEnemyHold_Implementation(int32 Epoch, int32 Index, uint8 Kind, float Speed)
{
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnEnemyHold(Epoch, Index, Kind, Speed);
}

void AOutlawsCoopLink::ClientEnemyFlight_Implementation(int32 Epoch, int32 Index, FVector_NetQuantize10 Pos, FVector_NetQuantize10 Vel)
{
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnEnemyFlight(Epoch, Index, Pos, Vel);
}

// ---- the player's side --------------------------------------------------------------------------------------------------

AOutlawsEnemy* AOutlawsPlayer::CoopGrabbed(int32 Hand) const
{
	return GrabbedEnemy[Hand & 1].Get();
}

FVector AOutlawsPlayer::CoopGripAt(int32 Hand) const
{
	return GripOf(Hand & 1)->GetComponentLocation();
}

void AOutlawsPlayer::CoopIgnoreEnemy(AOutlawsEnemy* E, bool bFling)
{
	if (!E) return;
	GetCapsuleComponent()->IgnoreActorWhenMoving(E, true);
	E->GetCapsuleComponent()->IgnoreActorWhenMoving(this, true);
	if (bFling)
	{
		// (UpdateEnemyGrab's own list: the two collide again a second after the let-go at the soonest, once clear)
		const int32 Fi = FlungEnemies.AddUnique(E);
		if (FlungUntil.Num() < FlungEnemies.Num()) FlungUntil.SetNumZeroed(FlungEnemies.Num());
		FlungUntil[Fi] = GetWorld()->GetTimeSeconds() + 1.0;
	}
}

#if WITH_OUTLAWS_TESTS
void AOutlawsPlayer::CoopTestHand(int32 Hand, const FVector& At, bool bGrip)
{
	// (a desktop has no tracked hand: the grip put where a hand would be, as -OLStoryTest's aim does; its squeeze held as
	// -OLBodyTest holds it)
	const int32 H = Hand & 1;
	if (!At.IsZero()) GripOf(H)->SetWorldLocation(At);
	const TCHAR* Name = H == 0 ? TEXT("OLVR_LeftGrip") : TEXT("OLVR_RightGrip");
	if (bGrip)
	{
		TestAxes.Add(Name, 1.f);
		bGripArmed[H] = false;
	}
	else
	{
		TestAxes.Remove(Name);
	}
}
#endif // WITH_OUTLAWS_TESTS

// ---- the client's copy of the bad guy -----------------------------------------------------------------------------------

bool AOutlawsEnemy::CoopPuppetGrabbable() const
{
	// (the host's copy in a hand or flying is the host's to place; this one still flying or settling from this player's own
	// throw waits for the host's to land)
	return Coop.IsValid() && bPuppetPlaced && !bPuppetHostCarried && !CoopLocalCarry();
}

void AOutlawsEnemy::CoopPuppetRelease(const FVector& Vel)
{
	// as EndHeld, for the client's copy: the picture and the sounds as there, and its own flight shown at once (the copy has
	// no movement of its own: a simple arc against the level, CoopTickCarry); the host's copy flies the same throw
	// (UOutlawsCoop::NoteEnemyRelease told it) and has the say on where it comes down
	bHeldByPlayer = false;
	bThrown = true;
	ThrowTime = 0.f;
	ThrowPrev = GetActorLocation();
	ThrowHits.Reset();
	RestoreSpritePose();
	if (Sprite)
	{
		Sprite->SetOnTop(false);
		Sprite->CustomTimeDilation = 1.f;
	}
	PuppetCoastGravZ = GetCharacterMovement()->GetGravityZ() * FlightGravity;
	FlightGravity = 1.f;
	PuppetCoastVel = Vel;
	bPuppetCoastSlide = false;
	bPuppetSawHostCarry = bPuppetSawHostCarry || bPuppetHostCarried;
	// let go with his bottom under the floor (the fist goes where it likes): stood on it, and a throw slides along it
	// (EndHeld's own rule)
	{
		const double Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		const FVector Here = GetActorLocation();
		const AActor* By = HeldBy.Get();
		const double Top = FMath::Max(Here.Z + Half, By ? By->GetActorLocation().Z : Here.Z + Half) + 20.0;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsCoopHeldFloor), false, this);
		if (By) Q.AddIgnoredActor(By);
		FCollisionResponseParams RP;
		RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
		FHitResult Floor;
		double Bottom = Here.Z - Half;
		if (Sprite) Bottom = FMath::Min(Bottom, Sprite->GetActorLocation().Z);
		UWorld* W = GetWorld();
		if (W && W->LineTraceSingleByChannel(Floor, FVector(Here.X, Here.Y, Top), FVector(Here.X, Here.Y, Here.Z - Half - 60.0), ECC_Visibility, Q, RP)
			&& Floor.ImpactNormal.Z > 0.7 && Bottom < Floor.ImpactPoint.Z - 0.5)
		{
			SetActorLocation(FVector(Here.X, Here.Y, Floor.ImpactPoint.Z + Half + 2.0), false, nullptr, ETeleportType::TeleportPhysics);
			FVector Flat(Vel.X, Vel.Y, 0.0);
			if (Vel.Size() > 150.f && Flat.Normalize())
			{
				bPuppetCoastSlide = true;
				PuppetCoastVel = Flat * Vel.Size();
				if (Sprite) Sprite->CustomTimeDilation = 1.f;
			}
		}
	}
	if (AOutlawsLevelActor* L = Level.Get())
	{
		const float Speed = Vel.Size();
		const float Up = Speed > 1.f ? FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(float(Vel.Z) / Speed, -1.f, 1.f))) : 0.f;
		if (IsDead()) {}                                    // (a body makes no cry)
		else if (IsCritter() && Speed > 300.f) L->PlaySoundAt(Def.DieSound, GetActorLocation());
		else if (!IsCritter() && Speed > 300.f && Up > 45.f) L->PlaySoundAt(TEXT("bgfall"), GetActorLocation());
	}
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLRT %s let go at %.0f cm/s (the client's copy, flown here)"), *OutlawsName, Vel.Size());
}

void AOutlawsEnemy::CoopTickCarry(float Dt)
{
	if (bHeldByPlayer)
	{
		// in this player's hand: the hand places him (AOutlawsPlayer::UpdateEnemyGrab); a man stopped on the stun's frame
		if (Sprite) Sprite->CustomTimeDilation = IsCritter() ? 1.f : 0.f;
		return;
	}
	if (bThrown)
	{
		ThrowTime += Dt;
		if (Sprite) Sprite->CustomTimeDilation = Sprite->GetChorFrame() >= 2 ? 0.f : 1.f;
		const FVector From = GetActorLocation();
		bool bDown = ThrowTime > 8.f;
		FHitResult Hit;
		if (bPuppetCoastSlide)
		{
			// along the floor, braked evenly to rest (EndHeld's slide: about 1.2 g)
			const double Speed = PuppetCoastVel.Size2D();
			const double Slower = FMath::Max(0.0, Speed - FMath::Abs(PuppetCoastGravZ) * 1.23 * Dt);
			PuppetCoastVel = Speed > 0.0 ? FVector(PuppetCoastVel.X, PuppetCoastVel.Y, 0.0) * (Slower / Speed) : FVector::ZeroVector;
			SetActorLocation(From + PuppetCoastVel * Dt, true, &Hit);
			if (Hit.bBlockingHit) PuppetCoastVel = FVector::VectorPlaneProject(PuppetCoastVel, Hit.ImpactNormal) * FVector(1.0, 1.0, 0.0);
			CoopKnockBody(From, PuppetCoastVel);                         // (the host's body stops him, as the host's own does there)
			bDown = bDown || Slower < 40.0;
		}
		else
		{
			PuppetCoastVel.Z += PuppetCoastGravZ * Dt;
			SetActorLocation(From + PuppetCoastVel * Dt, true, &Hit);
			if (Hit.bBlockingHit)
			{
				if (Hit.ImpactNormal.Z > 0.7) bDown = true;               // (a floor)
				else PuppetCoastVel = FVector::VectorPlaneProject(PuppetCoastVel, Hit.ImpactNormal) * 0.5;     // (a wall: along it)
			}
			CoopKnockBody(From, PuppetCoastVel);
		}
		if (bDown) CoopEndLocalCarry();
		return;
	}
	// down here: the host's place again once its copy is down too (or at the latest SettleMax after)
	if (Sprite) Sprite->OutlawsYaw = Heading;
	if ((bPuppetSawHostCarry && !bPuppetHostCarried) || FPlatformTime::Seconds() >= PuppetSettleUntil)
	{
		PuppetSettleUntil = 0.0;
		bPuppetSawHostCarry = false;
	}
}

bool AOutlawsEnemy::CoopKnockBody(const FVector& From, FVector& Vel)
{
	// (user 2026-10-02: a thrown bad guy, or a body, "knocks into the other player"; no hurt, friendly fire stays off.)  Each
	// machine draws the other player with no collision of its own and flies the thrower's copy by a different route (the host
	// by its movement, the client by CoopTickCarry), so the body is met here by the same sum on both: the two capsules as one
	// upright cylinder (the radii added, the half heights added); the way he came this frame meets its side; he is set at the
	// meeting and keeps only the speed along the body (straight on, he drops at its feet).  The thrower's own body is not
	// looked at (it lets go of him as in single player).
	UOutlawsCoop* C = Coop.Get();
	if (!C || From.IsZero()) return false;
	if (ThrowTime < CoopKnockClock) bCoopKnocked = false;          // (a new flight)
	CoopKnockClock = ThrowTime;
	// the thrower: on the host its own player's hand held him (HeldBy), else a client's did; a client flies only its own.  (N1:
	// every other player's body is met, in player order)
	const int32 Thrower = CoopRole == 1 && HeldBy.IsValid() ? 0 : C->ThrowerOf(this);
	for (int32 Who = 0; Who < UOutlawsCoop::MaxPlayers; ++Who)
	{
		if (Who != Thrower && CoopKnockBodyOf(Who, From, Vel)) return true;
	}
	return false;
}

bool AOutlawsEnemy::CoopKnockBodyOf(int32 Who, const FVector& From, FVector& Vel)
{
	UOutlawsCoop* C = Coop.Get();
	FVector Centre;
	float BodyR = 0.f, BodyHalf = 0.f;
	if (!C || !C->BodyOf(Who, Centre, BodyR, BodyHalf)) return false;
	const UCapsuleComponent* Cap = GetCapsuleComponent();
	const double R = double(BodyR) + Cap->GetScaledCapsuleRadius();
	const double H = double(BodyHalf) + Cap->GetScaledCapsuleHalfHeight();
	const FVector To = GetActorLocation();
	auto Inside = [&Centre, R, H](const FVector& P) { return FVector::DistSquared2D(P, Centre) < R * R && FMath::Abs(P.Z - Centre.Z) < H; };
	if (Inside(From)) return false;          // (let go within it: through it till clear, as a thrower's own body is)
	// the first point of From..To on the cylinder's side
	const FVector2D P0(From.X - Centre.X, From.Y - Centre.Y), D(To.X - From.X, To.Y - From.Y);
	const double A = D.SizeSquared(), B = 2.0 * FVector2D::DotProduct(P0, D), Cc = P0.SizeSquared() - R * R;
	bool bSide = false;
	FVector At = To;
	if (Cc > 0.0 && A > 1e-6)
	{
		const double Disc = B * B - 4.0 * A * Cc;
		if (Disc >= 0.0)
		{
			const double T = (-B - FMath::Sqrt(Disc)) / (2.0 * A);
			const FVector Meet = From + (To - From) * T;
			if (T >= 0.0 && T <= 1.0 && FMath::Abs(Meet.Z - Centre.Z) < H)
			{
				bSide = true;
				At = Meet;
			}
		}
	}
	if (!bSide && !Inside(To)) return false; // (past it, or over the head)
	// out to the side of the body (from over the head too: he slides off it), the speed into it gone
	FVector2D Out(At.X - Centre.X, At.Y - Centre.Y);
	if (!Out.Normalize())
	{
		Out = -D.GetSafeNormal();
		if (Out.IsNearlyZero()) Out = FVector2D(1.0, 0.0);
	}
	At.X = Centre.X + Out.X * (R + 0.5);
	At.Y = Centre.Y + Out.Y * (R + 0.5);
	const FVector N(Out.X, Out.Y, 0.0);
	const double Into = FVector::DotProduct(Vel, N);
	if (Into < 0.0) Vel -= N * Into;
	SetActorLocation(At, false, nullptr, ETeleportType::TeleportPhysics);
	if (!bCoopKnocked)
	{
		bCoopKnocked = true;
		C->NoteBodyKnock(this, Who, At);
	}
	return true;
}

void AOutlawsEnemy::CoopEndLocalCarry()
{
	// landed here: himself again (as TickCarried's landing), the copy standing (no movement of its own) till the host's is
	// down too
	EndCarry();
	GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	PuppetCoastVel = FVector::ZeroVector;
	bPuppetCoastSlide = false;
	PuppetSettleUntil = FPlatformTime::Seconds() + SettleMax;
	if (IsDead())
	{
		Play(DeadChor >= 0 ? DeadChor : Def.ChestDieChor, false);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLRT %s (dead) lands at %s (the client's copy)"), *OutlawsName, *GetActorLocation().ToCompactString());
	}
	else
	{
		Play(Def.WalkChor, true);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLRT %s lands at %s (the client's copy)"), *OutlawsName, *GetActorLocation().ToCompactString());
	}
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP throw: enemy %d down at %s (client, its own flight, %.2f s)"), CoopIndex, *GetActorLocation().ToCompactString(), ThrowTime);
}

void AOutlawsEnemy::CoopFollowHostFlight(const FVector& Pos, const FVector& Vel)
{
	// (only a flight still in the air here: one already down shows the host's place by itself once that is down too)
	if (!bThrown || bHeldByPlayer) return;
	SetActorLocation(Pos, false, nullptr, ETeleportType::TeleportPhysics);
	PuppetCoastVel = Vel;
	bPuppetCoastSlide = false;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP throw: enemy %d struck another bad guy on the host: this flight goes on from %s (client)"), CoopIndex, *Pos.ToCompactString());
}

void AOutlawsEnemy::CoopCancelHeld(const TCHAR* Why)
{
	// the session's word: this player's hold (or its throw in the air) is over here; the player's own tick lets go of an
	// enemy no longer held (UpdateEnemyGrab), and this copy shows the host's again
	if (!CoopLocalCarry()) return;
	if (bHeldByPlayer || bThrown) EndCarry();
	GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	PuppetCoastVel = FVector::ZeroVector;
	bPuppetCoastSlide = false;
	PuppetSettleUntil = 0.0;
	bPuppetSawHostCarry = false;
	if (IsDead()) Play(DeadChor >= 0 ? DeadChor : Def.ChestDieChor, false);
	else PuppetChor = -1;                         // (the host's choreography shown again with its next word)
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP grab: enemy %d out of this player's hand here (%s)"), CoopIndex, Why);
}

// ---- the session's side -------------------------------------------------------------------------------------------------

void UOutlawsCoop::NoteEnemyGrab(AOutlawsPlayer* P, AOutlawsEnemy* E, int32 Hand)
{
	UOutlawsCoop* C = Get(P);
	if (!C || !E || E->CoopIndex < 0) return;
	const TCHAR* Body = E->IsDead() ? TEXT(" (a body)") : TEXT("");
	if (C->IsHost())
	{
		++C->GrabsByHost;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP grab: enemy %d held by the host player%s"), E->CoopIndex, Body);
		const int32 Epoch = C->LevelEpoch();
		C->ForClients([&](AOutlawsCoopLink& CL) { CL.ClientEnemyHold(Epoch, E->CoopIndex, HoldByHost, 0.f); });
	}
	else if (C->IsClient())
	{
		++C->GrabsByClient;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP grab: enemy %d held by the client player%s (here at once; the host's copy follows)"), E->CoopIndex, Body);
		if (AOutlawsCoopLink* Mine = C->GetLink(C->MyIndex())) Mine->ServerEnemyGrab(C->LevelEpoch(), E->CoopIndex, uint8(Hand & 1));
	}
}

void UOutlawsCoop::NoteEnemyRelease(AOutlawsPlayer* P, AOutlawsEnemy* E, const FVector& Vel)
{
	UOutlawsCoop* C = Get(P);
	if (!C || !E || E->CoopIndex < 0) return;
	if (C->IsHost())
	{
		++C->ThrowsByHost;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP throw: enemy %d by the host player at %.0f cm/s"), E->CoopIndex, Vel.Size());
		const int32 Epoch = C->LevelEpoch();
		C->ForClients([&](AOutlawsCoopLink& CL) { CL.ClientEnemyHold(Epoch, E->CoopIndex, ThrowByHost, float(Vel.Size())); });
	}
	else if (C->IsClient())
	{
		++C->ThrowsByClient;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP throw: enemy %d by the client player at %.0f cm/s"), E->CoopIndex, Vel.Size());
		// (where he is let go and the throw's fall, read before EndHeld uses them)
		if (AOutlawsCoopLink* Mine = C->GetLink(C->MyIndex())) Mine->ServerEnemyRelease(C->LevelEpoch(), E->CoopIndex, E->GetActorLocation(), Vel, E->FlightGravity);
	}
}

void UOutlawsCoop::OnEnemyGrab(int32 Who, int32 Epoch, int32 Index, uint8 Hand)
{
	if (!IsHost() || Who < 1) return;
	RunFor(Epoch, [this, Who, Index]()
	{
		AOutlawsLevelActor* L = FindLevel();
		AOutlawsEnemy* E = EnemyAt(L, Index);
		AOutlawsCoopLink* CL = GetLink(Who);
		if (!E || !E->CanBeGrabbed())
		{
			// the host's word: first come; the host's own grabs were decided on its machine at once
			const AOutlawsPlayer* P = LocalPlayer();
			const TCHAR* Why = !E ? TEXT("not here") : (P && (P->CoopGrabbed(0) == E || P->CoopGrabbed(1) == E)) ? TEXT("the host's player holds it")
				: E->bHeldByPlayer ? TEXT("already held") : E->IsDormant() ? TEXT("not about") : TEXT("it cannot be taken up");
			++GrabsRefused;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP grab: enemy %d refused for the %s player (%s)"), Index, GrabWhoOf(Who), Why);
			if (CL) CL->ClientEnemyHold(LevelEpoch(), Index, HoldRefused, 0.f);
			return;
		}
		E->BeginHeld(nullptr);           // (no AI while held; his friends see it, as for the host's own grab)
		if (AOutlawsPlayer* Me = LocalPlayer()) Me->CoopIgnoreEnemy(E, false);     // (the host's capsule lets him by: his meeting with the body is CoopKnockBody's, alike on both machines)
		ClientHolds.Add(Index, Who);
		++GrabsByClient;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP grab: enemy %d held by the %s player%s (the host's copy at %s)"), Index, GrabWhoOf(Who), E->IsDead() ? TEXT(" (a body)") : TEXT(""),
			*E->GetActorLocation().ToCompactString());
		// (N1: the other clients hear of it as of the host's own grab: a hold of theirs on him ends)
		const int32 Ep = LevelEpoch();
		ForClients([&](AOutlawsCoopLink& K) { K.ClientEnemyHold(Ep, Index, HoldByHost, 0.f); }, Who);
	});
}

void UOutlawsCoop::OnEnemyRelease(int32 Who, int32 Epoch, int32 Index, const FVector& From, const FVector& Vel, float Gravity)
{
	if (!IsHost() || Who < 1) return;
	RunFor(Epoch, [this, Who, Index, From, Vel, Gravity]()
	{
		AOutlawsEnemy* E = EnemyAt(FindLevel(), Index);
		const int32* Holder = ClientHolds.Find(Index);
		const bool bMine = Holder && *Holder == Who;
		if (bMine) ClientHolds.Remove(Index);
		const bool bHeld = bMine && E && E->bHeldByPlayer;
		if (!bHeld)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP throw: enemy %d let go by the %s player: not held for it here any more"), Index, GrabWhoOf(Who));
			return;
		}
		++ThrowsByClient;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP throw: enemy %d by the %s player at %.0f cm/s"), Index, GrabWhoOf(Who), Vel.Size());
		// from where the client's hand let go, along the same throw (EndHeld, as the host's own let-go)
		E->SetActorLocation(From, false, nullptr, ETeleportType::TeleportPhysics);
		E->FlightGravity = Gravity;
		ClientThrownBy.Add(Index, Who);       // (before EndHeld: the knock asks who threw him)
		FOutlawsStatsScope ThrownBy(StatsByPlayer(Who));     // (the level's end panels: his fall or knock is that client's)
		E->EndHeld(Vel);
		if (AOutlawsPlayer* Me = LocalPlayer()) Me->CoopIgnoreEnemy(E, true);      // (the capsules apart till down and clear; the body stops him by CoopKnockBody, as there)
		ClientThrown.Add(Index, FPlatformTime::Seconds());
		const int32 Ep = LevelEpoch();
		ForClients([&](AOutlawsCoopLink& K) { K.ClientEnemyHold(Ep, Index, ThrowByHost, float(Vel.Size())); }, Who);
	});
}

void UOutlawsCoop::OnEnemyHold(int32 Epoch, int32 Index, uint8 Kind, float Speed)
{
	if (!IsClient()) return;
	RunFor(Epoch, [this, Index, Kind, Speed]()
	{
		AOutlawsEnemy* E = EnemyAt(FindLevel(), Index);
		switch (Kind)
		{
		case HoldByHost:
			++GrabsByHost;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP grab: enemy %d held by the host player%s"), Index, E && E->IsDead() ? TEXT(" (a body)") : TEXT(""));
			if (E && E->CoopLocalCarry()) E->CoopCancelHeld(TEXT("the host's player took it first"));
			break;
		case ThrowByHost:
			++ThrowsByHost;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP throw: enemy %d by the host player at %.0f cm/s"), Index, Speed);
			break;
		case HoldRefused:
			++GrabsRefused;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP grab: enemy %d refused for the client player (the host's word)"), Index);
			if (E) E->CoopCancelHeld(TEXT("refused by the host"));
			break;
		case HoldLetGo:
			++GrabsLetGo;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP grab: enemy %d left the client player's hand on the host (shot dead in it, or gone)"), Index);
			if (E) E->CoopCancelHeld(TEXT("the host's copy left the hand"));
			break;
		default:
			break;
		}
	});
}

void UOutlawsCoop::TickGrabs(double Now)
{
	// host: each copy held for the client's hand goes where the client's pose has it (eased: the pose comes 20 times a
	// second), drawn toward its holder; one that left the hand here (shot dead in it: TakeHit lets go) is let go there too
	if (!IsHost() || !bLevelBuilt || (ClientHolds.Num() == 0 && ClientThrown.Num() == 0)) return;
	AOutlawsLevelActor* L = FindLevel();
	if (!L) return;
	// (where the client's throws come down here, for the log: the client's own copy flew the same throw on its machine)
	for (auto It = ClientThrown.CreateIterator(); It; ++It)
	{
		const AOutlawsEnemy* E = EnemyAt(L, It.Key());
		if (E && (E->bThrown || E->bHeldByPlayer) && Now - It.Value() < 10.0) continue;
		if (E && !E->bThrown && !E->bHeldByPlayer)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP throw: enemy %d down at %s (host, %.2f s after the client's throw)"), It.Key(), *E->GetActorLocation().ToCompactString(), Now - It.Value());
		}
		ClientThrownBy.Remove(It.Key());
		It.RemoveCurrent();
	}
	const float Dt = FMath::Min(float(FApp::GetDeltaTime()), 0.1f);
	const AOutlawsPlayer* Me = LocalPlayer();
	const UCameraComponent* Cam = Me ? Me->Camera.Get() : nullptr;
	for (auto It = ClientHolds.CreateIterator(); It; ++It)
	{
		const int32 Index = It.Key();
		AOutlawsCoopLink* CL = GetLink(It.Value());      // (N1: the holder's)
		AOutlawsEnemy* E = EnemyAt(L, Index);
		if (!E || !E->bHeldByPlayer)
		{
			++GrabsLetGo;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP grab: enemy %d left the %s player's hand here (%s): the client lets go too"), Index, GrabWhoOf(It.Value()), !E ? TEXT("gone") : E->IsDead() ? TEXT("dead") : TEXT("let go"));
			if (CL) CL->ClientEnemyHold(LevelEpoch(), Index, HoldLetGo, 0.f);
			if (AOutlawsPlayer* HostP = LocalPlayer(); HostP && E) HostP->CoopIgnoreEnemy(E, true);
			It.RemoveCurrent();
			continue;
		}
		if (!CL) continue;
		const FOutlawsCoopPose& Po = CL->Pose;
		const int32 Hand = Po.LeftHeld == Index ? 0 : Po.RightHeld == Index ? 1 : -1;
		if (Hand < 0) continue;                  // (the pose is still from before the grab: he stays where he is)
		const FVector Want = Hand == 0 ? FVector(Po.LeftHeldAt) : FVector(Po.RightHeldAt);
		const FVector Here = E->GetActorLocation();
		E->SetActorLocation(FVector::DistSquared(Here, Want) > FMath::Square(500.0) ? Want : FMath::VInterpTo(Here, Want, Dt, HeldEase), false);
		if (E->IsCritter())
		{
			// a chicken or a tarantula turned with the client's hand, its side to this machine's eye
			const FVector Eye = Cam ? Cam->GetComponentLocation() : FVector(Po.Head);
			E->PoseHeldCritter(FTransform(Hand == 0 ? Po.LeftRot : Po.RightRot, Hand == 0 ? FVector(Po.LeftHand) : FVector(Po.RightHand)), Hand == 0, Eye);
		}
		else if (AOutlawsSpriteActor* Spr = E->GetSprite())
		{
			// a man faces his holder, as the holder sees him
			const FVector To = FVector(Po.Head) - Spr->GetActorLocation();
			Spr->OutlawsYaw = FRotator::NormalizeAxis(FMath::RadiansToDegrees(FMath::Atan2(To.Y, To.X)) - Spr->FacingYawOffset);
		}
	}
}

void UOutlawsCoop::NoteThrownStrike(AOutlawsEnemy* E)
{
	// (the host's copy of a client's throw met another bad guy: TickCarried hopped it back; the hop is the launch it set)
	UOutlawsCoop* C = E ? Get(E) : nullptr;
	if (!C || !C->IsHost() || E->CoopIndex < 0 || !C->ClientThrown.Contains(E->CoopIndex)) return;
	const int32 Thrower = C->ClientThrownBy.FindRef(E->CoopIndex);
	const FVector Vel = E->GetCharacterMovement()->PendingLaunchVelocity;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP throw: enemy %d (the client's throw) struck another bad guy here at %s: the client's flight told (host)"), E->CoopIndex, *E->GetActorLocation().ToCompactString());
	if (AOutlawsCoopLink* CL = C->GetLink(Thrower > 0 ? Thrower : 1)) CL->ClientEnemyFlight(C->LevelEpoch(), E->CoopIndex, E->GetActorLocation(), Vel);
}

void UOutlawsCoop::OnEnemyFlight(int32 Epoch, int32 Index, const FVector& Pos, const FVector& Vel)
{
	if (!IsClient()) return;
	RunFor(Epoch, [this, Index, Pos, Vel]()
	{
		if (AOutlawsEnemy* E = EnemyAt(FindLevel(), Index)) E->CoopFollowHostFlight(Pos, Vel);
	});
}

bool UOutlawsCoop::BodyOf(int32 Who, FVector& OutCentre, float& OutRadius, float& OutHalf) const
{
	const AOutlawsPlayer* Me = LocalPlayer();
	const AOutlawsLevelActor* L = FindLevel();
	if (!bLevelBuilt || !Me || !L) return false;
	OutRadius = Me->GetCapsuleComponent()->GetScaledCapsuleRadius();       // (both players' bodies are the same class)
	if (Who == MyIndex())
	{
		if (L->bDead) return false;              // (a body lying dead stops nothing: the other machine draws it lying)
		OutCentre = Me->GetActorLocation();
		OutHalf = Me->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		return true;
	}
	const AOutlawsRemotePlayer* R = PuppetOf(Who);
	const AOutlawsCoopLink* OL = GetLink(Who);
	if (!R || !OL || OL->Pose.Seq <= 0 || (OL->Stats.Seq > 0 && OL->Stats.bDead)) return false;
	OutCentre = R->ShownBody();
	OutHalf = R->ShownHalf();
	return true;
}

int32 UOutlawsCoop::ThrowerOf(const AOutlawsEnemy* E) const
{
	// (the host: the client whose throw or hold it is, its own hand's having been asked first by the enemy; a client flies
	// only its own)
	if (!E) return -1;
	if (!IsHost()) return MyIndex();
	const int32* By = ClientThrownBy.Find(E->CoopIndex);
	if (!By) By = ClientHolds.Find(E->CoopIndex);
	return By ? *By : 1;
}

void UOutlawsCoop::NoteBodyKnock(const AOutlawsEnemy* E, int32 Who, const FVector& At)
{
	++BodyKnocks;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP throw: enemy %d knocks into the %s player's body at %s (%s)"), E ? E->CoopIndex : -1, GrabWhoOf(Who), *At.ToCompactString(), GrabWho(IsHost()));
}

FString UOutlawsCoop::GrabsSummary() const
{
	return FString::Printf(TEXT("held by the host %d, by the client %d; thrown by the host %d, by the client %d; refused %d; let go by the host's copy %d; thrown into the other player's body %d"),
		GrabsByHost, GrabsByClient, ThrowsByHost, ThrowsByClient, GrabsRefused, GrabsLetGo, BodyKnocks);
}

// ---- tests (-OLCoopAuto -OLCoopTest=grab) -------------------------------------------------------------------------------

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TickGrabTestHand(AOutlawsPlayer* P)
{
	// the right hand held out before the eyes, as -OLBodyTest holds it (1: squeezing, 2: open)
	if (!P || TestGrabHand == 0) return;
	const UCameraComponent* Cam = P->Camera.Get();
	if (!Cam) return;
	const FVector Fl = FVector(Cam->GetForwardVector().X, Cam->GetForwardVector().Y, 0.0).GetSafeNormal();
	P->CoopTestHand(1, Cam->GetComponentLocation() + Fl * 50.0 + Cam->GetRightVector() * 15.0 - FVector(0.0, 0.0, 15.0), TestGrabHand == 1);
}

void UOutlawsCoop::TickClientGrabTest(AOutlawsPlayer* P)
{
	// (BUG-TNTARCTRIGGER) the host's asks, one a frame with a frame between: behind a long frame here they come in together,
	// and the trigger pressed is read by the hand's next update, so the lit fuse is waited for (and its going out on the
	// let-go), for a second at most
	if (!P || TestTntArcQueue.Num() == 0 || GFrameCounter < TestTntArcFrame + 2) return;
	const int32 Arg = TestTntArcQueue[0];
	const int32 H = Arg < 4 ? 1 : 0, Stage = Arg % 4;
	const double Now = FPlatformTime::Seconds();
	const bool bWaiting = (Stage == 2 && !P->bFuseLit[H]) || (Stage == 3 && P->bFuseLit[H]);
	if (bWaiting && Now < TestTntArcAt + 1.0) return;
	TestTntArcQueue.RemoveAt(0);
	TestTntArcFrame = GFrameCounter;
	TestTntArcAt = Now;
	P->TntArcTestStep(H, Stage, TEXT("OLCOOP test: client"));
}

void UOutlawsCoop::TestLogGrab(AOutlawsLevelActor* L, AOutlawsPlayer* P, int32 Index, const TCHAR* Why)
{
	const AOutlawsEnemy* E = EnemyAt(L, Index);
	if (!E) return;
	FString Extra;
	if (IsHost())
	{
		// where the client's pose has its hand's bad guy
		if (const AOutlawsCoopLink* CL = GetLink(1))
		{
			const FOutlawsCoopPose& Po = CL->Pose;
			if (Po.LeftHeld == Index || Po.RightHeld == Index)
			{
				Extra = FString::Printf(TEXT(", %.0f cm from where the client's hand has him"), FVector::Dist(E->GetActorLocation(), FVector(Po.LeftHeld == Index ? Po.LeftHeldAt : Po.RightHeldAt)));
			}
		}
	}
	else if (P)
	{
		for (int32 h = 0; h < 2; ++h)
		{
			if (P->CoopGrabbed(h) == E) Extra = FString::Printf(TEXT(", in this player's hand, his hold %.0f cm from its fist"), FVector::Dist(E->GrabChest(), P->CoopGripAt(h)));
		}
	}
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: grab: enemy %d %s (%s): at %s, held %d thrown %d dead %d%s"), Index, Why, GrabWho(IsHost()), *E->GetActorLocation().ToCompactString(),
		E->bHeldByPlayer ? 1 : 0, E->bThrown ? 1 : 0, E->IsDead() ? 1 : 0, *Extra);
}

void UOutlawsCoop::TestLogPace(AOutlawsLevelActor* L)
{
	// (decision 6) what this machine applies: the living bad guys' time dilation (the most common: one just hit or flying
	// goes at full speed for a moment) and the INF's wait scale, beside this player's own pace
	TMap<int32, int32> Seen;
	int32 Living = 0;
	for (const TWeakObjectPtr<AOutlawsEnemy>& W : L->GetEnemies())
	{
		const AOutlawsEnemy* E = W.Get();
		if (!E || E->IsDead()) continue;
		++Living;
		++Seen.FindOrAdd(FMath::RoundToInt(E->CustomTimeDilation * 1000.f));
	}
	int32 Common = 0, Most = 0;
	for (const TPair<int32, int32>& Kv : Seen) if (Kv.Value > Most) { Most = Kv.Value; Common = Kv.Key; }
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: pace here (%s): the bad guys' time dilation %.3f (%d of %d living), the INF's wait scale x%.2f, this player's own pace %.3f"),
		GrabWho(IsHost()), Common / 1000.f, Most, Living, L->Inf ? L->Inf->WaitScale : -1.0, L->Pace());
}

void UOutlawsCoop::TestLogBodies(AOutlawsPlayer* P)
{
	const AOutlawsRemotePlayer* R = PartnerPuppet();
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: grab: bodies (%s): this player at %s, the other at %s"), GrabWho(IsHost()), *P->GetActorLocation().ToCompactString(),
		R ? *R->ShownBody().ToCompactString() : TEXT("?"));
}

void UOutlawsCoop::TickHostGrabTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	TickGrabTestHand(P);
	// (a man thrown earlier is up again beside a player and shoots at it point blank: the man held up as a shield dies of it
	// before his step's check.  The test's held men kept above any such shot (a few points) and below the test's own 50)
	for (const int32 Idx : { TestGrabA, TestGrabB, TestGrabC, TestArcMan })
	{
		if (AOutlawsEnemy* E = EnemyAt(L, Idx); E && E->bHeldByPlayer && !E->IsDead()) E->Health = FMath::Max(E->Health, 20.f);
	}
	if (Now < TestAt) return;
	const AOutlawsCoopLink* CL = GetLink(1);
	const FVector Them = CL ? FVector(CL->Pose.Body) : P->GetActorLocation();
	const UCameraComponent* Cam = P->Camera.Get();
	const FVector Fl = Cam ? FVector(Cam->GetForwardVector().X, Cam->GetForwardVector().Y, 0.0).GetSafeNormal() : P->GetActorForwardVector();
	switch (TestStep)
	{
	case 150:
		// both out of harm's way; a bad guy on the client's floor for the client's hand, another on the host's for the host's (the same men every run: TestPickEnemy's bStable)
		L->bInvincible = true;
		TestSay(TEXT("godmode"), 1);
		TestGrabA = TestPickEnemy(L, Them, INDEX_NONE, true, INDEX_NONE, true);
		TestGrabB = TestPickEnemy(L, P->GetActorLocation(), TestGrabA, true, INDEX_NONE, true);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: grab on %s: the client is to take enemy %d, the host enemy %d"), *BuiltLevel, TestGrabA, TestGrabB);
		if (TestGrabA < 0 || TestGrabB < 0)
		{
			TestSay(TEXT("quit"), 0);
			TestStep = 100;
			TestAt = Now + 3.0;
			return;
		}
		TestSay(TEXT("g:hand"), 1);
		TestStep = 151;
		TestAt = Now + 1.0;
		return;
	case 151:
		// 1: the client takes enemy A into its right hand (at once there; the host's copy follows its hand)
		TestLogGrab(L, P, TestGrabA, TEXT("before"));
		TestSay(TEXT("g:grab"), TestGrabA);
		TestStep = 152;
		TestAt = Now + 2.0;
		return;
	case 152:
		TestLogGrab(L, P, TestGrabA, TEXT("held by the client"));
		TestSay(TEXT("g:log"), TestGrabA);
		TestStep = 153;
		TestAt = Now + 0.5;
		return;
	case 153:
		// 2: the client throws him: its copy flies at once, the host's the same throw
		TestSay(TEXT("g:throw"), TestGrabA);
		TestStep = 154;
		TestAt = Now + 5.0;
		return;
	case 154:
		TestLogGrab(L, P, TestGrabA, TEXT("after the client's throw"));
		TestSay(TEXT("g:log"), TestGrabA);
		TestStep = 155;
		TestAt = Now + 0.5;
		return;
	case 155:
	{
		// 3: the host takes enemy B, and the client reaches for the same one straight after: the host's grab stands
		TestGrabHand = 1;
		TickGrabTestHand(P);
		AOutlawsEnemy* B = EnemyAt(L, TestGrabB);
		const bool bOk = B && P->CoopTestGrab(1, B);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: grab: the host takes enemy %d: %s"), TestGrabB, bOk ? TEXT("taken") : TEXT("NOT TAKEN"));
		TestSay(TEXT("g:grab"), TestGrabB);
		TestStep = 156;
		TestAt = Now + 2.0;
		return;
	}
	case 156:
		TestLogGrab(L, P, TestGrabB, TEXT("held by the host"));
		TestSay(TEXT("g:log"), TestGrabB);
		TestStep = 157;
		TestAt = Now + 0.5;
		return;
	case 157:
		// 4: the host throws him
		if (P->CoopGrabbed(1)) P->CoopTestLetGo(1, Fl * 700.0 + FVector(0.0, 0.0, 300.0));
		TestGrabHand = 2;
		TickGrabTestHand(P);
		TestGrabHand = 0;
		TestStep = 158;
		TestAt = Now + 4.0;
		return;
	case 158:
		TestLogGrab(L, P, TestGrabB, TEXT("after the host's throw"));
		TestSay(TEXT("g:log"), TestGrabB);
		// 5: the client takes enemy A again, and the host shoots him dead in the client's hand: the host's copy drops (as a held
		// man shot dead does) and the client's hand lets go of its own
		TestSay(TEXT("g:grab"), TestGrabA);
		TestSay(TEXT("g:aim"), 1);              // (BUG-ARCSTUCK: his throw lined up, the arc out, when he dies)
		TestStep = 159;
		TestAt = Now + 2.0;
		return;
	case 159:
		TestSay(TEXT("g:arcpre"), TestGrabA);
		if (AOutlawsEnemy* A = EnemyAt(L, TestGrabA); A && !A->IsDead())
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: grab: the host shoots enemy %d dead (held %d)"), TestGrabA, A->bHeldByPlayer ? 1 : 0);
			for (int32 i = 0; i < 60 && !A->IsDead(); ++i) A->TakeHit(50.f, A->GetActorLocation(), FVector(1.0, 0.0, 0.0), A->GetActorLocation() - FVector(100.0, 0.0, 0.0));
		}
		TestStep = 160;
		TestAt = Now + 3.0;
		return;
	case 160:
		// 6: the body taken up and thrown by the client
		TestLogGrab(L, P, TestGrabA, TEXT("shot dead in the client's hand"));
		TestSay(TEXT("g:log"), TestGrabA);
		TestSay(TEXT("g:arc"), TestGrabA);      // (BUG-ARCSTUCK: his arc gone with him; the trigger let go)
		TestSay(TEXT("g:grab"), TestGrabA);
		TestStep = 161;
		TestAt = Now + 2.0;
		return;
	case 161:
		TestLogGrab(L, P, TestGrabA, TEXT("a body held by the client"));
		TestSay(TEXT("g:log"), TestGrabA);
		TestSay(TEXT("g:throw"), TestGrabA);
		TestStep = 162;
		TestAt = Now + 5.0;
		return;
	case 162:
		TestLogGrab(L, P, TestGrabA, TEXT("the body after the client's throw"));
		TestSay(TEXT("g:log"), TestGrabA);
		TestStep = 163;
		TestAt = Now + 1.0;
		return;
	case 163:
	{
		// 7 (user 2026-10-02, decision 6): the paces as both machines apply them (run with a different speed on each)
		TestLogPace(L);
		TestSay(TEXT("g:pace"), 0);
		// 8 (decision 3): the client stands 2.2 m from the host, facing it, takes a third bad guy and throws him at the host:
		// on both machines he meets the host's body and drops in front of it
		TestGrabC = TestPickEnemy(L, Them, TestGrabB, true, TestGrabA, true);       // (A lies dead: a living one, not B)
		// (B, for the host's throw at the client in step 9, is a body by then: killed now so it lies still when taken up)
		if (AOutlawsEnemy* Bk = EnemyAt(L, TestGrabB); Bk && !Bk->IsDead())
		{
			for (int32 i = 0; i < 60 && !Bk->IsDead(); ++i) Bk->TakeHit(50.f, Bk->GetActorLocation(), FVector(0.0, 0.0, -1.0), Bk->GetActorLocation() + FVector(0.0, 0.0, 200.0));
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: grab: the client is to throw enemy %d at the host"), TestGrabC);
		TestSay(TEXT("g:face"), 220);
		TestStep = 164;
		TestAt = Now + 1.0;
		return;
	}
	case 164:
		TestLogBodies(P);
		TestSay(TEXT("g:grab"), TestGrabC);
		TestStep = 165;
		TestAt = Now + 2.0;
		return;
	case 165:
		TestSay(TEXT("g:throw"), TestGrabC);
		TestStep = 166;
		TestAt = Now + 5.0;
		return;
	case 166:
	{
		TestLogGrab(L, P, TestGrabC, TEXT("after the client's throw at the host"));
		TestSay(TEXT("g:log"), TestGrabC);
		// (laid down for good, so the next throw does not bowl him over on its way)
		if (AOutlawsEnemy* Cg = EnemyAt(L, TestGrabC); Cg && !Cg->IsDead())
		{
			// (the shot's way straight off the line between the two players: his fall keeps out of the next throw's way)
			const FVector Line = (Them - P->GetActorLocation()).GetSafeNormal2D();
			const FVector Off = Cg->GetActorLocation() - P->GetActorLocation();
			FVector Away = (Off - Line * FVector::DotProduct(Off, Line)).GetSafeNormal2D();
			if (Away.IsNearlyZero()) Away = FVector(-Line.Y, Line.X, 0.0);
			for (int32 i = 0; i < 60 && !Cg->IsDead(); ++i) Cg->TakeHit(50.f, Cg->GetActorLocation(), Away, Cg->GetActorLocation() - Away * 100.0);
		}
		// (and wherever he lies, off the line: a body between the two stops the next throw short of the client)
		if (AOutlawsEnemy* Cg = EnemyAt(L, TestGrabC); Cg && Cg->IsDead())
		{
			const FVector Line = (Them - P->GetActorLocation()).GetSafeNormal2D();
			const FVector Side(-Line.Y, Line.X, 0.0);
			const FVector Mid = (Them + P->GetActorLocation()) * 0.5;
			if (FVector::DistSquared2D(Cg->GetActorLocation(), Mid) < FMath::Square(300.0))
			{
				Cg->SetActorLocation(Cg->GetActorLocation() + Side * 300.0, false, nullptr, ETeleportType::TeleportPhysics);
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: grab: enemy %d's body moved off the line of the next throw"), TestGrabC);
			}
		}
		// (a living man up again after the throw can walk through the client and shove it a metre on: back to 2.2 m first)
		TestSay(TEXT("g:face"), 220);
		TestStep = 170;
		TestAt = Now + 1.0;
		return;
	}
	case 170:
	{
		// 9: the host takes enemy B again (a body by now, which lies where it lands; the client's throw above was a living
		// man), turned to the client 2.2 m off, and throws him at the client
		const AOutlawsRemotePlayer* R = PartnerPuppet();
		if (APlayerController* PC = Cast<APlayerController>(P->GetController()); PC && R && Cam)
		{
			const FVector To = R->ShownBody() - Cam->GetComponentLocation();
			PC->SetControlRotation(FRotator(0.0, To.Rotation().Yaw, 0.0));
		}
		TestGrabHand = 1;
		TickGrabTestHand(P);
		AOutlawsEnemy* B = EnemyAt(L, TestGrabB);
		const bool bOk = B && P->CoopTestGrab(1, B);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: grab: the host takes enemy %d to throw at the client: %s"), TestGrabB, bOk ? TEXT("taken") : TEXT("NOT TAKEN"));
		TestStep = 167;
		TestAt = Now + 2.0;
		return;
	}
	case 167:
	{
		TestLogBodies(P);
		const AOutlawsRemotePlayer* R = PartnerPuppet();
		FVector Way = Fl;
		if (R) Way = (R->ShownBody() - P->GetActorLocation()).GetSafeNormal2D();
		if (P->CoopGrabbed(1)) P->CoopTestLetGo(1, Way * 700.0 + FVector(0.0, 0.0, 300.0));
		TestGrabHand = 2;
		TickGrabTestHand(P);
		TestGrabHand = 0;
		TestStep = 168;
		TestAt = Now + 5.0;
		return;
	}
	case 168:
		TestLogGrab(L, P, TestGrabB, TEXT("after the host's throw at the client"));
		TestSay(TEXT("g:log"), TestGrabB);
		TestStep = 171;
		TestAt = Now + 1.0;
		return;
	case 171:
	{
		// 10 (BUG-ARCSTUCK): the host takes a living man with his throw lined up, the arc out, and the client shoots him dead
		// in the host's hand: the host's arc goes with him
		TestArcMan = TestPickEnemy(L, P->GetActorLocation(), TestGrabA, true, TestGrabC, false);
		AOutlawsEnemy* D = EnemyAt(L, TestArcMan);
		TestGrabHand = 1;
		TickGrabTestHand(P);
		const bool bOk = D && P->CoopTestGrab(1, D);
		if (bOk) P->CoopTestAimArc(1, true);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: arcstuck host: the host takes enemy %d, the throw lined up: %s"), TestArcMan, bOk ? TEXT("taken") : TEXT("NOT TAKEN"));
		TestStep = 172;
		TestAt = Now + 1.5;
		return;
	}
	case 172:
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: arcstuck host: %s the arc shows before (held %d)"), P->CoopTestArcShown(1) ? TEXT("PASS") : TEXT("FAIL"), P->CoopGrabbed(1) ? 1 : 0);
		TestSay(TEXT("g:shoot"), TestArcMan);
		TestStep = 173;
		TestAt = Now + 3.0;
		return;
	case 173:
	{
		const AOutlawsEnemy* D = EnemyAt(L, TestArcMan);
		const bool bDead = D && D->IsDead();
		const bool bOk = bDead && !P->CoopGrabbed(1) && !P->CoopTestArcShown(1) && !P->CoopTestArcShown(0);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: arcstuck host: %s enemy %d shot dead by the client in the host's hand (dead %d, held %d, arc %d %d)"),
			bOk ? TEXT("PASS") : TEXT("FAIL"), TestArcMan, bDead ? 1 : 0, P->CoopGrabbed(1) ? 1 : 0, P->CoopTestArcShown(0) ? 1 : 0, P->CoopTestArcShown(1) ? 1 : 0);
		P->CoopTestAimArc(1, false);
		TestGrabHand = 2;
		TickGrabTestHand(P);
		TestGrabHand = 0;
		TestStep = 174;
		TestAt = Now + 1.0;
		return;
	}
	case 174: case 175: case 176: case 177: case 178: case 179: case 180: case 181:
		// 11 (BUG-TNTARCTRIGGER): a stick in the host's right hand then left: no arc just held, the arc while the trigger holds
		// the lit fuse, gone on the throw
		P->TntArcTestStep(TestStep < 178 ? 1 : 0, (TestStep - 174) % 4, TEXT("OLCOOP test: host"));
		++TestStep;
		TestAt = Now + 0.8;
		return;
	case 182: case 183: case 184: case 185: case 186: case 187: case 188: case 189:
		// and the same on the client
		TestSay(TEXT("g:tntarc"), TestStep - 182);
		TestStep = TestStep == 189 ? 169 : TestStep + 1;
		TestAt = Now + 0.8;
		return;
	case 169:
		TestSay(TEXT("g:hand"), 0);
		TestSay(TEXT("quit"), 0);
		TestStep = 100;
		TestAt = Now + 3.0;
		return;
	default:
		return;
	}
}

void UOutlawsCoop::OnGrabTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	const UCameraComponent* Cam = P->Camera.Get();
	const FVector Fl = Cam ? FVector(Cam->GetForwardVector().X, Cam->GetForwardVector().Y, 0.0).GetSafeNormal() : P->GetActorForwardVector();
	if (Step == TEXT("hand"))
	{
		TestGrabHand = Arg;
		if (TestGrabHand == 0) P->CoopTestHand(1, FVector::ZeroVector, false);
	}
	else if (Step == TEXT("grab"))
	{
		AOutlawsEnemy* E = EnemyAt(L, Arg);
		TestGrabHand = 1;
		TickGrabTestHand(P);
		const bool bOk = E && P->CoopTestGrab(1, E);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: grab: the client takes enemy %d: %s (%s)"), Arg, bOk ? TEXT("taken here") : TEXT("NOT TAKEN"),
			!E ? TEXT("not here") : E->IsDead() ? TEXT("a body") : TEXT("alive"));
	}
	else if (Step == TEXT("throw"))
	{
		AOutlawsEnemy* E = EnemyAt(L, Arg);
		const bool bHeld = E && P->CoopGrabbed(1) == E;
		if (bHeld) P->CoopTestLetGo(1, Fl * 700.0 + FVector(0.0, 0.0, 300.0));
		TestGrabHand = 2;
		TickGrabTestHand(P);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: grab: the client throws enemy %d: %s"), Arg, bHeld ? TEXT("thrown") : TEXT("NOT IN ITS HAND"));
	}
	else if (Step == TEXT("log"))
	{
		TestLogGrab(L, P, Arg, TEXT("asked"));
	}
	else if (Step == TEXT("aim"))
	{
		// (BUG-ARCSTUCK) the client's held man's throw lined up, the arc out
		P->CoopTestAimArc(1, Arg != 0);
	}
	else if (Step == TEXT("tntarc"))
	{
		// (BUG-TNTARCTRIGGER) the client's own stick, right hand then left (run by TickClientGrabTest)
		if (Arg == 0) TestTntArcAt = FPlatformTime::Seconds();
		TestTntArcQueue.Add(Arg);
	}
	else if (Step == TEXT("arcpre"))
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: arcstuck client: %s the arc shows before (held %d)"), P->CoopTestArcShown(1) ? TEXT("PASS") : TEXT("FAIL"), P->CoopGrabbed(1) ? 1 : 0);
	}
	else if (Step == TEXT("arc"))
	{
		const AOutlawsEnemy* E = EnemyAt(L, Arg);
		const bool bOk = E && E->IsDead() && !P->CoopGrabbed(1) && !P->CoopTestArcShown(1) && !P->CoopTestArcShown(0);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: arcstuck client: %s enemy %d shot dead by the host in the client's hand (dead %d, held %d, arc %d %d)"),
			bOk ? TEXT("PASS") : TEXT("FAIL"), Arg, E && E->IsDead() ? 1 : 0, P->CoopGrabbed(1) ? 1 : 0, P->CoopTestArcShown(0) ? 1 : 0, P->CoopTestArcShown(1) ? 1 : 0);
		P->CoopTestAimArc(1, false);
	}
	else if (Step == TEXT("shoot"))
	{
		// (BUG-ARCSTUCK) the client's shots on a man in the host's hand, sent as its own shots are
		if (const AOutlawsEnemy* E = EnemyAt(L, Arg))
		{
			const FVector From = P->GetActorLocation();
			for (int32 i = 0; i < 12; ++i) ReportEnemyHit(E, 50.f, E->GetActorLocation(), E->GetActorLocation() - From, From, 0, 1.f);
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: arcstuck client: shots sent at enemy %d"), Arg);
	}
	else if (Step == TEXT("pace"))
	{
		TestLogPace(L);
	}
	else if (Step == TEXT("face"))
	{
		// (decision 3) a free spot Arg cm from the host's body, on a floor, nothing in between, turned to face it
		const AOutlawsRemotePlayer* R = PartnerPuppet();
		if (!R) return;
		const FVector Host = R->ShownBody();
		const float Half = P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), Rad = P->GetCapsuleComponent()->GetScaledCapsuleRadius();
		UWorld* W = P->GetWorld();
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsCoopTestFace), false, P);
		bool bPlaced = false;
		for (int32 k = 0; k < 16 && !bPlaced; ++k)
		{
			const FVector At = Host + FRotator(0.0, 22.5 * k, 0.0).Vector() * double(Arg) + FVector(0.0, 0.0, Half - R->ShownHalf() + 3.0);
			FHitResult Hit;
			if (W->LineTraceSingleByChannel(Hit, Host, At, ECC_Visibility, Q)) continue;                 // (a wall or a bad guy between)
			if (W->OverlapBlockingTestByChannel(At, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(Rad, Half), Q)) continue;
			if (!W->LineTraceSingleByChannel(Hit, At, At - FVector(0.0, 0.0, Half + 60.0), ECC_Visibility, Q)) continue;
			const FRotator Face(0.0, (Host - At).Rotation().Yaw, 0.0);
			P->TeleportTo(At, Face, false, true);
			if (AController* Ctl = P->GetController()) Ctl->SetControlRotation(Face);
			bPlaced = true;
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: grab: the client stands %d cm from the host's body, facing it: %s"), Arg, bPlaced ? TEXT("yes") : TEXT("NO FREE SPOT"));
		TestLogBodies(P);
	}
}
#endif // WITH_OUTLAWS_TESTS
