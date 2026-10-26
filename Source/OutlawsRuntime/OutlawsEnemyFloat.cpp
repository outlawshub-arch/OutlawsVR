// REQ-ENEMYFLOAT (user 2026-10-14, snapshot 2026-10-14_21-16-22, the canyon's water, sector 166): bad guys float, and drown
// if held under.
// - A living man in water deeper than he stands floats with the water line between his chest and his neck (FloatLine of
//   his standing height over his feet), bobbing a little.  He still wades (slower, MaxFlySpeed), turns and shoots from
//   there; points over water count as places to go while he floats (MoveAlong, PickPoint).  Where the bed comes up under
//   his feet he stands and walks out as before.
// - Thrown or fallen in, the water takes him once he is down at his float height: a throw ends there (no landing, no fall
//   damage) and he bobs up to the line.  A hit's knock that lifts him lets him fall back in the same way.
// - Held under by a player's grab (his head under the surface) for DrownSeconds, he dies: a drowning death (his chest
//   death, no gib), the kill the holder's.  Let go sooner, he lives and floats again.  The time held under adds up over
//   short breaths and starts again once his head has been up for SurfaceResetSeconds (3 s).
// - Not the critters, voices, the dormant, match bots or the dead (a body sinks as before).  Bosses and scripted men
//   follow the same rule (none of them stands in water in their scenes).
// - Co-op: the host decides (its copy floats and drowns; the client's copy shows the host's place, and a client's hold is
//   the host's copy held at the client's hand, so the host counts it too).
// - Test: -OLShots -OLLevel=canyon -OLNoFrontEnd -OLEnemyFloatTest (OutlawsEnemyFloatTest in OutlawsPlayer's test chain),
//   and -OLCoopAuto -OLCoopTest=float for the co-op host and client.
#include "OutlawsEnemy.h"
#include "OutlawsLevelActor.h"
#include "OutlawsStats.h"
#include "OutlawsRuntime.h"
#include "OutlawsSpriteActor.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"

namespace OutlawsEnemyFloatLocal
{
	constexpr float Rise = 3.f;          // the float's pull toward the line: cm/s per cm off it
	constexpr float Ease = 4.f;          // how fast his up-and-down speed takes that pull (under it he overshoots: a bob)
	constexpr float BobCm = 2.5f;        // the bob's own swell
	constexpr float WadeShare = 0.6f;    // his speed in the water, of his walk
	constexpr float FlyBrake = 900.f;    // floating, he comes to rest when he stops wading
}

float AOutlawsEnemy::FloatTargetZ(float SurfaceZ) const
{
	// the capsule's middle where his standing height's FloatLine is at the surface (his feet kept there when he ducks)
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	return SurfaceZ - FloatLine * 2.f * StandHalf + Half;
}

bool AOutlawsEnemy::SwimAt(const FVector& P) const
{
	// water at P's plan position, at his own feet's height (where he floats)
	const AOutlawsLevelActor* L = Level.Get();
	if (!L) return false;
	const float Feet = float(GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	float Surface = 0.f;
	return L->WaterAt(FVector(P.X, P.Y, Feet + 5.0), Surface);
}

void AOutlawsEnemy::StartFloat(float SurfaceZ)
{
	UCharacterMovementComponent* Move = GetCharacterMovement();
	bFloating = true;
	SavedFlySpeed = Move->MaxFlySpeed;
	SavedFlyBrake = Move->BrakingDecelerationFlying;
	Move->MaxFlySpeed = Move->MaxWalkSpeed * OutlawsEnemyFloatLocal::WadeShare;
	Move->BrakingDecelerationFlying = OutlawsEnemyFloatLocal::FlyBrake;
	// the water takes his fall: down no faster than this, slowing, then up to the line
	FloatVz = FMath::Clamp(float(Move->Velocity.Z), -350.f, 0.f);
	Move->SetMovementMode(MOVE_Flying);
	Move->Velocity.Z = 0.f;
	FloatSince = Now();
	UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s floats (surface z %.0f, the line at %.0f%% of his height)"), *OutlawsName, SurfaceZ, FloatLine * 100.f);
}

void AOutlawsEnemy::StopFloat(const TCHAR* Why, bool bKeepMode)
{
	if (!bFloating) return;
	bFloating = false;
	UCharacterMovementComponent* Move = GetCharacterMovement();
	if (SavedFlySpeed >= 0.f) Move->MaxFlySpeed = SavedFlySpeed;
	if (SavedFlyBrake >= 0.f) Move->BrakingDecelerationFlying = SavedFlyBrake;
	SavedFlySpeed = SavedFlyBrake = -1.f;
	if (!bKeepMode && Move->MovementMode == MOVE_Flying) Move->SetMovementMode(MOVE_Falling);
	UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s stops floating: %s"), *OutlawsName, Why);
}

void AOutlawsEnemy::TickWater(float Dt)
{
	using namespace OutlawsEnemyFloatLocal;
	AOutlawsLevelActor* L = Level.Get();
	if (!L || CoopRole == 2 || Dt <= 0.f) return;
	UCharacterMovementComponent* Move = GetCharacterMovement();
	if (IsDead() || bVoiceOnly || bDormant || IsCritter() || CoopSelf >= 0)
	{
		if (bFloating) StopFloat(IsDead() ? TEXT("dead") : TEXT("not a floater"), false);
		return;
	}
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const FVector Here = GetActorLocation();
	if (bHeldByPlayer)
	{
		// in a hand: no float of his own; his head under the surface counts toward drowning
		if (bFloating) StopFloat(TEXT("taken in a hand"), true);
		float HeadSurface = 0.f;
		bHeadUnder = L->WaterAt(Here + FVector(0.0, 0.0, Half * 0.8), HeadSurface);
		if (!bHeadUnder) NoteSurfaced(Dt);
		else
		{
			DrownSurfaced = 0.f;
			DrownHeld += Dt;
			if (DrownHeld >= DrownLogged + 5.f)
			{
				DrownLogged = FMath::FloorToFloat(DrownHeld / 5.f) * 5.f;
				UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s held under: %.0f s of %.0f"), *OutlawsName, DrownHeld, DrownSeconds);
			}
			if (DrownHeld >= DrownSeconds) Drown();
		}
		return;
	}
	bHeadUnder = false;
	NoteSurfaced(Dt);
	const float FeetZ = float(Here.Z) - Half;
	float Surface = 0.f;
	if (!L->WaterAt(FVector(Here.X, Here.Y, FeetZ + 5.0), Surface))
	{
		if (bFloating) StopFloat(TEXT("out of the water"), false);
		return;
	}
	const float TargetZ = FloatTargetZ(Surface);
	const float FloatFeet = TargetZ - Half;
	// deep enough to float: the bed under him lower than his floating feet (else he stands on it and wades)
	FHitResult Bed;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsEnemyBed), false, this);
	FCollisionResponseParams RP;
	RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
	const bool bBed = GetWorld()->LineTraceSingleByChannel(Bed, Here, FVector(Here.X, Here.Y, FMath::Min(FeetZ, FloatFeet) - 10.0), ECC_Visibility, Q, RP);
	if (bBed && Bed.ImpactPoint.Z > FloatFeet - 2.0)
	{
		if (bFloating) StopFloat(TEXT("the bed comes up under his feet"), false);
		return;
	}
	if (bThrown)
	{
		if (Here.Z > TargetZ + 5.f) return;            // still coming down to the water
		// in: the throw is over (no landing, no fall's hurt), he is himself again and comes up to the line
		EndCarry();
		Play(Def.WalkChor, true);
		SetState(EOutlawsEnemyState::Chase);
		AdjustMeters(-1.f, 1.f);
		SetSub(SubDecide);
		UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s thrown into the water at %.0f cm/s: comes up"), *OutlawsName, GetVelocity().Size());
	}
	if (!bFloating)
	{
		if (Here.Z > TargetZ + 5.f) return;            // falling in: the water takes him at his float height
		StartFloat(Surface);
	}
	if (Move->MovementMode != MOVE_Flying)
	{
		if (Here.Z > TargetZ + 5.f) return;            // a knock lifted him out of it: back once down at the water
		FloatVz = FMath::Clamp(float(Move->Velocity.Z), -350.f, 0.f);
		Move->SetMovementMode(MOVE_Flying);
	}
	if (bKnockFlight)
	{
		// (a hit's flight ends in the water as on the ground: he keeps to his FENCE walls again)
		bKnockFlight = false;
		GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_GameTraceChannel2, ECR_Block);
	}
	// the pull to the line, eased in (so he overshoots a little coming up: a bob), and the swell's own small rise and fall;
	// his up and down is the float's own (FloatVz, moved here), the movement's flying only takes him across (its braking
	// would eat a rise put in its velocity)
	const float Want = TargetZ + BobCm * FMath::Sin(float(Now()) * 2.1f + float(GetUniqueID() % 7));
	const float Pull = FMath::Clamp((Want - float(Here.Z)) * Rise, -400.f, 400.f);
	const float Step = FMath::Min(Dt, 0.5f);
	FloatVz = FMath::Lerp(FloatVz, Pull, 1.f - FMath::Exp(-Ease * Step));
	Move->Velocity.Z = 0.f;
	FHitResult Hit;
	Move->SafeMoveUpdatedComponent(FVector(0.0, 0.0, FloatVz * Step), GetActorQuat(), true, Hit);
	if (Hit.bBlockingHit) FloatVz = 0.f;          // (the bed, or a rock over him)
}

void AOutlawsEnemy::NoteSurfaced(float Dt)
{
	// his head up (let go, or held with it out of the water): SurfaceResetSeconds of it and the time held under starts again;
	// a shorter breath keeps it
	// (counted in real seconds: let go he ticks at the bad guys' pace, CustomTimeDilation, a third by default)
	if (DrownHeld <= 0.f) return;
	DrownSurfaced += CustomTimeDilation > 0.f ? Dt / CustomTimeDilation : Dt;
	if (DrownSurfaced < SurfaceResetSeconds) return;
	UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s up for %.0f s: the %.1f s held under starts again"), *OutlawsName, SurfaceResetSeconds, DrownHeld);
	DrownHeld = DrownSurfaced = DrownLogged = 0.f;
}

void AOutlawsEnemy::Drown()
{
	// a drowning death: the holder's kill, his chest death where the hand has him, no gib (no blast)
	UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s drowns: held under %.1f s"), *OutlawsName, DrownHeld);
	bDrowned = true;
	FOutlawsStatsScope By(HeldBy.IsValid() ? EOutlawsStatsBy::Me : StatsByPlayer(1), false, false);
	UOutlawsStats::NoteHit(this, GetActorLocation());
	UOutlawsStats::NoteDamage(this, FMath::Max(0.f, Health));
	Health = 0.f;
	EndHeld(FVector::ZeroVector);
	Die(GetActorLocation() + FVector(0.0, 0.0, 10.0), -FRotator(0.0, Heading, 0.0).Vector());
}

void AOutlawsEnemy::DebugSwimTo(const FVector& P)
{
	// (tests) a wade to P, as a move Decide would choose
	MoveTarget = FVector(P.X, P.Y, GetActorLocation().Z);
	bRunning = false;
	if (State == EOutlawsEnemyState::Watch) Alert(false);
	SetSub(SubAdvance);
}

FString AOutlawsEnemy::DebugWater() const
{
	const AOutlawsLevelActor* L = Level.Get();
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const FVector Here = GetActorLocation();
	const float FeetZ = float(Here.Z) - Half;
	float Surface = 0.f;
	const bool bWet = L && L->WaterAt(FVector(Here.X, Here.Y, FeetZ + 5.0), Surface);
	const float Tall = 2.f * StandHalf;
	return FString::Printf(TEXT("feet z %.1f, surface %s, the water line at %.0f%% of his height (%.0f cm deep), mode %d, floating %d, alive %d, held under %.1f s, shots %d"),
		FeetZ, bWet ? *FString::Printf(TEXT("z %.1f"), Surface) : TEXT("none"), bWet ? (Surface - FeetZ) / Tall * 100.f : 0.f, bWet ? Surface - FeetZ : 0.f,
		int32(GetCharacterMovement()->MovementMode), bFloating ? 1 : 0, IsDead() ? 0 : 1, DrownHeld, ShotsTotal);
}
