// BUG-DYNTHROW (user 2 Nov, the user's snapshot #41, shgulch: Dick Farmer pressed on, slid, was hit and taunted, and never
// threw a thing): the bad guys whose ITM has a THROW_MAX (SH_DICK and DICKCLIF, BGY2BOOM and EBGY2BOOM, Two Feathers and
// ETWOFEAT) held their fire, "not built yet".  Now, as the original [EXE 0x140089fcb..0x140089fe5, 0x14008c4c2, 0x14008cfe0,
// 0x140090670; research/outlaws_enemy_exe.md 5.7]:
// - his shoot timer (SHOOT_TIMER_TIME, the difficulty's) starts a throw only with the player between THROW_MIN and THROW_MAX
//   of him and a lob that reaches clear (AOutlawsEnemy::Tick); one throw a timer, never on the move;
// - the shoot chor's EVENT(FIRE) (DickClif.NWX chor 10, a second in; BGY2.NWX chor 12) lets it go (ThrowAtPlayer): up to 10
//   marks by the player's feet, from 0.8 of his height, at THROW_VELOCITY, the lower arc and then the upper, the whole flight
//   clear (OutlawsThrowArc.h: the match bots' own lob, UOutlawsCoop::BotThrow);
// - a stick (pdynam) flies under the bots' gravity, falls off walls, lies lit and goes off its LIFE (4.5 s) after it left
//   the hand: the level's own blast (Explode), its kills no one's (bEnemyBlast); a knife (pknife, Two Feathers') strikes the
//   player for his DAMAGE, as his shot would, and is spent where it meets the level;
// - both drawn as the player's own (AOutlawsPlayer::BuildThrownLook), the stick's wick burning down under its flame, its
//   fuse heard.
// Co-op: the host's bad guys throw (the client's copies think nothing); the blast reaches the client as every blast does, the
// throw's sound with the shot count.  (BUG-DYNTHROW phase 2, user 3 Nov) the throw is told to the clients by the players' own
// throw message (UOutlawsCoop::NoteEnemyThrow): each client flies a copy here from the same hand at the same speed, so it takes
// the host's path, falls off the same walls and lies lit with its fuse heard; the copy hurts no one, leaves nothing and never
// goes off: the host's blast (its Stick -2, "a bad guy's") puts it out where it lies (EndEnemyThrownCopyNear), the one blast
// and its one sound; a missed knife's gknife is the host's ground item, made on the clients as every one is, and taken as
// every pickup is (the host says who has it).  -OLOldDynCoop: not told, as before.

#include "OutlawsEnemy.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsRemotePlayer.h"
#include "OutlawsCoop.h"
#include "OutlawsFlags.h"
#include "OutlawsGeometry.h"
#include "OutlawsRuntime.h"
#include "OutlawsThrowArc.h"
#include "Components/AudioComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/PlayerController.h"
#include "ProceduralMeshComponent.h"
#include "Engine/World.h"

namespace OLDynThrow
{
	constexpr double UnitCm = FOutlawsGeometry::UnitsToCm;

	/** A bad guy's stick or knife in the air or (a stick) lying lit. */
	struct FFlying
	{
		TWeakObjectPtr<AOutlawsLevelActor> Level;
		TWeakObjectPtr<AOutlawsEnemy> By;
		FString ByName;
		int32 Kind = 1;                      // 1 a stick of dynamite, 2 a knife (BuildThrownLook's kinds)
		FVector Pos = FVector::ZeroVector, Vel = FVector::ZeroVector;
		float Age = 0.f;                     // seconds since it left the hand
		float Fuse = 4.5f;                   // a stick: goes off at this age; a knife: given up at it
		float Damage = 0.f;                  // a knife's
		FString DeadItem;                    // (the thrown ITM's DEAD_ITEM: what a knife leaves where it comes down)
		float Spin = 0.f, Yaw = 0.f;
		bool bRest = false;
		bool bCopy = false;                  // (co-op: a client's copy of the host's: drawn and flown only)
		FOutlawsThrownLook Look;
		TWeakObjectPtr<UAudioComponent> Sound;
	};
	TArray<FFlying> GFlying;
	int32 GLooksMissed = 0, GLooksLogged = 0;

	void DropFlying(FFlying& F)
	{
		if (UAudioComponent* S = F.Sound.Get()) S->Stop();
		if (UProceduralMeshComponent* M = F.Look.Mesh.Get()) M->DestroyComponent();
		if (UStaticMeshComponent* Fl = F.Look.Flame.Get()) Fl->DestroyComponent();
		if (UStaticMeshComponent* Co = F.Look.Core.Get()) Co->DestroyComponent();
		F.Look = FOutlawsThrownLook();
	}

	AOutlawsPlayer* LocalThrowViewer(const UWorld* W)
	{
		const APlayerController* PC = W ? W->GetFirstPlayerController() : nullptr;
		return PC ? Cast<AOutlawsPlayer>(PC->GetPawn()) : nullptr;
	}
}

bool AOutlawsEnemy::OldDynThrow()
{
	static const bool bOld = OLFlags::Param(TEXT("OLOldDynThrow"));
	return bOld;
}

bool AOutlawsEnemy::OldDynCoop()
{
	static const bool bOld = OLFlags::Param(TEXT("OLOldDynCoop"));
	return bOld;
}

bool AOutlawsEnemy::TargetInThrowRange(float* OutDist) const
{
	FVector Eye, Feet;
	float R = 0.f, H = 0.f;
	if (!IsThrower() || !PlayerView(Eye, Feet, R, H)) return false;
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const FVector MyFeet = GetActorLocation() - FVector(0.0, 0.0, Half);
	float D = float(FVector::Dist2D(Feet, MyFeet));
	// (the DYNTHROW follow-up) by the original's own measure, the octagonal one (max + half the min [EXE 0x14008d04a..0x14008d0ee]),
	// as the close advance's 10..20 units are: the flat one left a man the advance put at 10 units just inside THROW_MIN
	if (!OldDynThrow2())
	{
		const double DX = FMath::Abs(Feet.X - MyFeet.X), DY = FMath::Abs(Feet.Y - MyFeet.Y);
		D = float(FMath::Max(DX, DY) + 0.5 * FMath::Min(DX, DY));
	}
	if (OutDist) *OutDist = D;
	// [EXE 0x14008cfe0]: the target's distance strictly between THROW_MIN and THROW_MAX
	return D > Def.ThrowMin * OLDynThrow::UnitCm && D < Def.ThrowMax * OLDynThrow::UnitCm;
}

bool AOutlawsEnemy::SolveThrow(const FVector& TargetFeet, float TargetHeight, int32 Tries, bool bScatter, FVector& OutHand, FVector& OutVel, double& OutTime, FVector& OutMark) const
{
	UWorld* W = GetWorld();
	if (!W || !IsThrower()) return false;
	const bool bStick = Def.ThrowItem != TEXT("pknife");
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const FVector MyFeet = GetActorLocation() - FVector(0.0, 0.0, Half);
	FVector Toward = (TargetFeet - MyFeet).GetSafeNormal2D();
	if (Toward.IsNearlyZero()) Toward = FRotator(0.0, Heading, 0.0).Vector();
	// from 0.8 of his height [EXE 0x14008cfe0], a hand's reach out in front of him
	OutHand = MyFeet + FVector(0.0, 0.0, HeightCm() * 0.8) + Toward * (GetCapsuleComponent()->GetScaledCapsuleRadius() + 20.0);
	const double G = 980.0 * OutlawsThrowArc::Grav;
	const double Speed = FMath::Max(1.f, Def.ThrowVelocity) * OLDynThrow::UnitCm;
	// a stick to land at the feet, a knife into the body
	const FVector Rise(0.0, 0.0, bStick ? 10.0 : FMath::Max(40.f, TargetHeight) * 0.55);
	for (int32 k = 0; k < Tries; ++k)
	{
		// the first mark on the feet (bScatter: within 60 cm of them; a knife 15), the next ones wider round them
		const double Spread = bStick ? 60.0 + 30.0 * k : 15.0 + 10.0 * k;
		const FVector Off = bScatter || k > 0 ? FRotator(0.0, FMath::FRandRange(0.f, 360.f), 0.0).Vector() * FMath::FRandRange(0.0, Spread) : FVector::ZeroVector;
		const FVector Mark = TargetFeet + Rise + Off;
		for (int32 Arc = 0; Arc < (bStick ? 2 : 1); ++Arc)
		{
			FVector Vel;
			if (!OutlawsThrowArc::LobVelocity(OutHand, Mark, Speed, G, Vel, Arc == 1)) continue;
			const double T = FVector::Dist2D(Mark, OutHand) / FMath::Max(1.0, Vel.Size2D());
			if (!OutlawsThrowArc::ArcClear(W, this, OutHand, Vel, G, T, Mark, bStick ? 200.0 : 120.0)) continue;
			OutVel = Vel;
			OutTime = T;
			OutMark = Mark;
			return true;
		}
	}
	return false;
}

void AOutlawsEnemy::ThrowAtPlayer()
{
	AOutlawsLevelActor* L = Level.Get();
	FVector Eye, Feet;
	float R = 0.f, H = 0.f;
	if (!L || !PlayerView(Eye, Feet, R, H)) return;
	const bool bStick = Def.ThrowItem != TEXT("pknife");
	float Dist = 0.f;
	const bool bInRange = TargetInThrowRange(&Dist);
	FVector Hand, Vel, Mark;
	double T = 0.0;
	if (!bInRange || !SolveThrow(Feet, float(Eye.Z - Feet.Z), 10, true, Hand, Vel, T, Mark))
	{
		UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s holds his throw: %s (%.1f m; THROW_MIN %.0f, THROW_MAX %.0f units)"), *OutlawsName,
			bInRange ? TEXT("no clear lob reaches") : TEXT("the player is out of his throw's reach"), Dist / 100.f, Def.ThrowMin, Def.ThrowMax);
		return;
	}
	++ThrowsTotal;
	LastThrowAt = Now();
	LastThrowMark = Mark;
	LastThrowDist = Dist;
	Heading = (Mark - GetActorLocation()).Rotation().Yaw;
	L->PlaySoundAt(Def.ShootSound, Hand);         // (THROW.wav)
	if (CoopRole == 1) ++CoopShots;               // (co-op: the client's copy plays the throw's sound too)
	L->AddEnemyThrown(this, bStick ? 1 : 2, Hand, Vel, bStick ? Def.ThrowLife : 3.f, Def.Damage);
	if (CoopRole == 1 && !OldDynCoop()) UOutlawsCoop::NoteEnemyThrow(this, bStick ? 1 : 2, Hand, Vel, bStick ? Def.ThrowLife : 3.f);     // (phase 2: the clients' copies)
	UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s throws %s at the player %.1f m off: %.1f m/s, %.0f deg up, %.2f s to its mark %.0f cm from the feet%s"), *OutlawsName,
		bStick ? TEXT("a stick of dynamite") : TEXT("a knife"), Dist / 100.f, Vel.Size() / 100.0, Vel.Rotation().Pitch, T, FVector::Dist2D(Mark, Feet),
		bStick ? *FString::Printf(TEXT(", fuse %.1f s"), Def.ThrowLife) : TEXT(""));
}

// ---- the level's side: the sticks and knives in the air ------------------------------------------------------------------

void AOutlawsLevelActor::AddEnemyThrown(AOutlawsEnemy* By, int32 Kind, const FVector& From, const FVector& Vel, float Fuse, float Damage, bool bCoopCopy)
{
	using namespace OLDynThrow;
	FFlying F;
	F.Level = this;
	F.By = By;
	F.ByName = By ? By->GetOutlawsName() : FString(TEXT("a bad guy"));
	F.Kind = Kind;
	F.Pos = From;
	F.Vel = Vel;
	F.Fuse = Fuse;
	F.Damage = bCoopCopy ? 0.f : Damage;
	F.bCopy = bCoopCopy;
	if (By && !bCoopCopy) F.DeadItem = By->Def.ThrowDead;      // (a copy's knife leaves nothing: the host's gknife comes as a ground item)
	if (bCoopCopy)
	{
		++EnemyCopiesMade;
		LastEnemyCopyFrom = From;
	}
	F.Yaw = float(Vel.Rotation().Yaw);
	// the player's own look of it (the held model on the held material, a lit stick's wick and flame)
	AOutlawsPlayer* Me = LocalThrowViewer(GetWorld());
	if (Me && Me->BuildThrownLook(this, Kind, Kind == 1, F.Look))
	{
		if (UProceduralMeshComponent* M = F.Look.Mesh.Get())
		{
			M->SetWorldLocationAndRotation(From, FRotator(0.0, F.Yaw, 0.0));
			M->SetWorldScale3D(FVector::OneVector);
		}
		if (Kind == 1) Me->PlaceThrownFuse(F.Look, Fuse);
		if (GLooksLogged++ < 4)
		{
			UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s's %s drawn as the player's own (%d sections, wick %d, flame %s, grows to %.2f)"), *F.ByName, Kind == 1 ? TEXT("stick") : TEXT("knife"),
				F.Look.Mesh.IsValid() ? F.Look.Mesh->GetNumSections() : 0, F.Look.FuseSection, F.Look.Flame.IsValid() ? TEXT("yes") : TEXT("no"), F.Look.GrowTo);
		}
	}
	else if (GLooksMissed++ < 4)
	{
		UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s's %s: no look of it here (not drawn)"), *F.ByName, Kind == 1 ? TEXT("stick") : TEXT("knife"));
	}
	if (Kind == 1) F.Sound = PlayLoopAt(TEXT("fuse"), From);
	if (bCoopCopy)
	{
		UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s's %s's copy from the host: from %s at %.1f m/s, %s%s"), *F.ByName, Kind == 1 ? TEXT("stick") : TEXT("knife"), *From.ToCompactString(), Vel.Size() / 100.0,
			F.Look.Mesh.IsValid() ? TEXT("drawn") : TEXT("not drawn"), Kind == 1 ? *FString::Printf(TEXT(", fuse %.1f s heard"), Fuse) : TEXT(""));
	}
	GFlying.Add(MoveTemp(F));
}

int32 AOutlawsLevelActor::EnemyThrownCount() const
{
	int32 N = 0;
	for (const OLDynThrow::FFlying& F : OLDynThrow::GFlying) N += F.Level.Get() == this ? 1 : 0;
	return N;
}

bool AOutlawsLevelActor::DebugEnemyThrown(FVector& OutPos, float& OutAge, int32& OutKind, bool& bOutRest, bool& bOutDrawn) const
{
	for (const OLDynThrow::FFlying& F : OLDynThrow::GFlying)
	{
		if (F.Level.Get() != this) continue;
		OutPos = F.Pos;
		OutAge = F.Age;
		OutKind = F.Kind;
		bOutRest = F.bRest;
		bOutDrawn = F.Look.Mesh.IsValid();
		return true;
	}
	return false;
}

FString AOutlawsLevelActor::DebugEnemyThrownLook() const
{
	auto Part = [](const USceneComponent* C) -> FString
	{
		return C ? FString::Printf(TEXT("at %s scale %.2f %s"), *C->GetComponentLocation().ToCompactString(), C->GetComponentScale().X, C->IsVisible() ? TEXT("shown") : TEXT("hidden")) : FString(TEXT("none"));
	};
	for (const OLDynThrow::FFlying& F : OLDynThrow::GFlying)
	{
		if (F.Level.Get() != this) continue;
		return FString::Printf(TEXT("mesh %s; flame %s; core %s"), *Part(F.Look.Mesh.Get()), *Part(F.Look.Flame.Get()), *Part(F.Look.Core.Get()));
	}
	return TEXT("none in the air");
}

void AOutlawsLevelActor::ClearEnemyThrown()
{
	using namespace OLDynThrow;
	for (int32 i = GFlying.Num() - 1; i >= 0; --i)
	{
		if (GFlying[i].Level.IsValid() && GFlying[i].Level.Get() != this) continue;
		DropFlying(GFlying[i]);
		GFlying.RemoveAt(i);
	}
}

bool AOutlawsLevelActor::EndEnemyThrownCopyNear(const FVector& At, double Radius)
{
	using namespace OLDynThrow;
	int32 Best = INDEX_NONE;
	double BestD = Radius * Radius;
	for (int32 i = 0; i < GFlying.Num(); ++i)
	{
		const FFlying& F = GFlying[i];
		if (F.Level.Get() != this || !F.bCopy || F.Kind != 1) continue;
		const double D = FVector::DistSquared(F.Pos, At);
		if (D <= BestD) { BestD = D; Best = i; }
	}
	if (Best == INDEX_NONE)
	{
		UE_LOG(LogOutlawsAI, Log, TEXT("OLRT the host's bad guy's blast at %s: no copy of his stick lies within %.0f cm here"), *At.ToCompactString(), Radius);
		return false;
	}
	FFlying& F = GFlying[Best];
	++EnemyCopiesPutOut;
	LastEnemyCopyGap = float(FMath::Sqrt(BestD));
	LastEnemyCopyAt = F.Pos;
	UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s's stick's copy put out by the host's blast: it lay at %s, %.0f cm from the blast, %.2f s after it came (fuse %.2f s)"), *F.ByName,
		*F.Pos.ToCompactString(), LastEnemyCopyGap, F.Age, F.Fuse);
	DropFlying(F);
	GFlying.RemoveAt(Best);
	return true;
}

void AOutlawsLevelActor::TickEnemyThrown(float Dt)
{
	using namespace OLDynThrow;
	UWorld* W = GetWorld();
	if (GFlying.Num() == 0 || !W) return;
	AOutlawsPlayer* Me = LocalThrowViewer(W);
	const double G = 980.0 * OutlawsThrowArc::Grav;
	// a knife's step A..B into a standing body (its feet, its eye, its radius)
	auto MeetsBody = [](const FVector& Feet, const FVector& Eye, float R, const FVector& A, const FVector& B)
	{
		const FVector Mid(Feet.X, Feet.Y, (Feet.Z + Eye.Z) * 0.5);
		const FVector C = FMath::ClosestPointOnSegment(Mid, A, B);
		return FVector::Dist2D(C, Mid) < R + 10.0 && C.Z > Feet.Z && C.Z < Eye.Z + 15.0;
	};
	for (int32 i = GFlying.Num() - 1; i >= 0; --i)
	{
		if (i >= GFlying.Num()) continue;          // (a blast's aftermath cleared the list: a level end, a restart)
		FFlying& F = GFlying[i];
		if (!F.Level.IsValid()) { DropFlying(F); GFlying.RemoveAt(i); continue; }     // (its level gone)
		if (F.Level.Get() != this) continue;
		F.Age += Dt;
		bool bGone = false;
		if (!F.bRest)
		{
			F.Vel.Z -= G * Dt;
			const FVector To = F.Pos + F.Vel * Dt;
			bool bStruck = false;
			if (F.Kind == 2 && F.bCopy)
			{
				// (phase 2) a client's copy of a knife: gone into the body it meets here, this player's or another's as drawn here;
				// the hurt is the host's to deal (HurtFromEnemy), so none here
				AOutlawsEnemy* E = F.By.Get();
				FVector Eye, Feet;
				float R = 0.f, H = 0.f;
				const TCHAR* Whose = nullptr;
				if (E && E->PlayerView(Eye, Feet, R, H) && MeetsBody(Feet, Eye, R, F.Pos, To)) Whose = TEXT("this player");
				const UOutlawsCoop* Cp = Whose ? nullptr : UOutlawsCoop::Get(this);
				for (int32 k = 0; Cp && !Whose && k < UOutlawsCoop::MaxPlayers; ++k)
				{
					const AOutlawsRemotePlayer* Rp = Cp->PuppetOf(k);
					const FVector Mid = Rp ? Rp->GetActorLocation() : FVector::ZeroVector;      // (the puppet's place: its body's middle)
					if (Rp && MeetsBody(Mid - FVector(0.0, 0.0, 88.0), Mid + FVector(0.0, 0.0, 70.0), 34.f, F.Pos, To)) Whose = UOutlawsCoop::WhoWord(k);
				}
				if (Whose)
				{
					bStruck = true;
					bGone = true;
					++EnemyCopiesSpent;
					UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s's knife's copy meets %s here, %.2f s after it came (the host deals the hurt)"), *F.ByName, Whose, F.Age);
				}
			}
			else if (F.Kind == 2)
			{
				// a knife: into the body of the player he threw it at (the host's own here, or the client's as the host sees it)
				AOutlawsEnemy* E = F.By.Get();
				FVector Eye, Feet;
				float R = 0.f, H = 0.f;
				if (E && E->PlayerView(Eye, Feet, R, H))
				{
					if (MeetsBody(Feet, Eye, R, F.Pos, To))
					{
						bStruck = true;
						if (E->CoopTarget != 0)
						{
							if (UOutlawsCoop* Cp = E->Coop.Get()) Cp->HurtFromEnemy(E, F.Damage, 1.f);     // (co-op: on the client's machine)
						}
						else
						{
							if (UOutlawsCoop* Cp = E->CoopRole == 1 ? E->Coop.Get() : nullptr) Cp->NotePlayerActive(0, TEXT("hit by a bad guy's knife"));
							SetHurtSource(F.Pos);
							bHurtByBadGuy = true;
							DamagePlayer(F.Damage);
						}
						++EnemyKnifeHits;
						UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s's knife strikes the player (%.1f), %.2f s after it left his hand"), *F.ByName, F.Damage, F.Age);
						bGone = true;
					}
				}
			}
			if (!bStruck)
			{
				FHitResult Hit;
				FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsEnemyThrown), false, F.By.Get());
				FCollisionResponseParams RP;
				RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);         // (the level, not the bodies)
				if (W->LineTraceSingleByChannel(Hit, F.Pos, To, ECC_Visibility, Q, RP))
				{
					F.Pos = FVector(Hit.ImpactPoint) + FVector(Hit.ImpactNormal) * 2.0;
					// a stick that meets a wall or a beam falls off it (pdynam's ALLOW_BOUNCE), as the bots' do (TickBotThrown)
					if (F.Kind == 1 && Hit.ImpactNormal.Z < 0.6) F.Vel = F.Vel.MirrorByVector(FVector(Hit.ImpactNormal)) * 0.3;
					else
					{
						F.bRest = true;
						F.Vel = FVector::ZeroVector;
						if (UProceduralMeshComponent* M = F.Look.Mesh.Get(); M && F.Kind == 1)
						{
							// at rest it lies along the floor on its side, as the player's own (its length is its up)
							M->SetWorldLocationAndRotation(F.Pos, FRotator(90.f, F.Yaw, 0.f));
							M->UpdateBounds();
							F.Pos.Z += FMath::Max(0.0, F.Pos.Z - M->Bounds.GetBox().Min.Z) - 1.5;
							M->SetWorldLocation(F.Pos);
						}
					}
					if (F.Kind == 2 && F.bCopy)
					{
						// (phase 2) the copy comes down where the host's did; the gknife it leaves there is the host's ground item,
						// made here by its own word (UOutlawsCoop::NoteGroundItem)
						++EnemyCopiesSpent;
						LastEnemyCopyAt = F.Pos;
						UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s's knife's copy comes down at %s, %.2f s after it came"), *F.ByName, *F.Pos.ToCompactString(), F.Age);
						bGone = true;
					}
					else if (F.Kind == 2)
					{
						++EnemyKnivesSpent;
						UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s's knife misses: spent on the level at %s"), *F.ByName, *F.Pos.ToCompactString());
						// (the DYNTHROW follow-up, user 2 Nov) it lies where it came down as the knife the level places, its DEAD_ITEM
						// [EXE 0x1400d084b..0x1400d087e: a projectile come to rest makes its DEAD_ITEM there]; picked up, a knife.
						// One that strikes the player leaves none (user 2 Nov: none unless the original does; its strike path is
						// not read to the end yet)
						if (!AOutlawsEnemy::OldDynThrow2() && !F.DeadItem.IsEmpty())
						{
							if (AActor* Drop = SpawnGroundItem(F.DeadItem, F.Pos))
							{
								++EnemyKnivesLeft;
								LastEnemyKnifeDrop = Drop;
								UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s's knife left as %s at %s"), *F.ByName, *F.DeadItem, *Drop->GetActorLocation().ToCompactString());
							}
						}
						bGone = true;
					}
				}
				else
				{
					F.Pos = To;
				}
			}
			F.Spin += Dt * (F.Kind == 1 ? 430.f : 630.f);
			if (UProceduralMeshComponent* M = F.Look.Mesh.Get(); M && !F.bRest) M->SetWorldLocationAndRotation(F.Pos, FRotator(F.Spin, F.Yaw, 0.f));
		}
		if (UProceduralMeshComponent* M = F.Look.Mesh.Get(); M && F.Look.GrowTo != 1.f && F.Age <= 1.05f)
		{
			M->SetWorldScale3D(FVector(FMath::InterpEaseInOut(1.f, F.Look.GrowTo, FMath::Clamp(F.Age, 0.f, 1.f), 2.f)));    // (to the level item's size, as the thrower's copy grows)
		}
		if (F.Kind == 1)
		{
			if (Me && F.Look.Flame.IsValid()) Me->PlaceThrownFuse(F.Look, FMath::Max(0.f, F.Fuse - F.Age));
			if (UAudioComponent* S = F.Sound.Get()) S->SetWorldLocation(F.Pos);
			if (F.bCopy)
			{
				// (phase 2) a copy goes off nowhere: it lies, its wick burnt down, till the host's blast puts it out
				// (EndEnemyThrownCopyNear); one whose blast never came (a level changed under it) is put out quietly 2 s on
				if (F.Age >= F.Fuse + 2.f)
				{
					UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s's stick's copy: no blast from the host %.2f s after it came, put out here"), *F.ByName, F.Age);
					bGone = true;
				}
			}
			else if (F.Age >= F.Fuse)
			{
				// the fuse out: the level's own blast where it lies, no one's doing (bEnemyBlast)
				const FVector At = F.Pos;
				const FString By = F.ByName;
				const float Age = F.Age;
				DropFlying(F);
				GFlying.RemoveAt(i);
				++EnemyBlasts;
				LastEnemyBlastAt = At;
				LastEnemyFuse = Age;
				LastEnemyBlastBy = By;
				UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s's stick goes off at %s, %.2f s after it left his hand"), *By, *At.ToCompactString(), Age);
				TGuardValue<bool> ByEnemy(bEnemyBlast, true);
				if (UOutlawsCoop::IsActive(this) && !AOutlawsEnemy::OldDynCoop()) CoopBlastStick = -2;     // (phase 2: told as a bad guy's, it puts out the clients' copies)
				Explode(At);
				continue;
			}
		}
		else if (F.Age >= F.Fuse)
		{
			bGone = true;                         // (a knife that never came down: given up)
		}
		if (bGone)
		{
			DropFlying(F);
			GFlying.RemoveAt(i);
		}
	}
}
