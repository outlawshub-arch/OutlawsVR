// REQ-ENEMYFLOAT's tests (OutlawsEnemyFloat.cpp).
// -OLShots -OLLevel=canyon -OLNoFrontEnd -OLEnemyFloatTest: at the user's spot in the canyon's water (snapshot
// 2026-10-14_21-16-22, sector 166) a bad guy dropped in from 1.5 m over the surface: his water line logged as he comes up and
// settles; alerted, he shoots at the player (swimming there) and wades 2.5 m; then held under: dead (drowned) at 30 s.  A
// second one thrown in comes up to the line too, is held under 20 s and let go: alive, floating again, the count back to 0
// after 6 s up; held under 12 s more he lives (32 s in all, but not in a row).  Then 15 s under, up only 1.5 s (the count
// stays), under again: he drowns 15 s into that dunk.
// -OLCoopAuto -OLCoopTest=float -OLCoopLevel=canyon: the host drops one in; the host's and the client's copies each log their
// water line (the host decides; the client's copy shows it).
#include "OutlawsPlayer.h"
#include "OutlawsEnemy.h"
#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "OutlawsShotTests.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsEnemyFloatTestLocal
{
	// the user's spot: his body in the water there (UE)
	const FVector Spot(22825.247, 1800.963, -2085.545);
	TWeakObjectPtr<AOutlawsEnemy> CoopDropped;       // (the co-op host's dropped man)

	/** Water deep enough to float in near the spot (Ring cm out), its surface; false when none. */
	bool DeepWaterNear(const AOutlawsLevelActor* L, UWorld* W, double Ring, int32 Skip, FVector& OutAt, float& OutSurface)
	{
		float S0 = 0.f;
		if (!L->WaterAt(Spot, S0)) return false;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsFloatTestBed), false);
		FCollisionResponseParams RP;
		RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
		auto Depth = [&](const FVector& P, float S)
		{
			FHitResult Bed;
			return W->LineTraceSingleByChannel(Bed, FVector(P.X, P.Y, S + 50.0), FVector(P.X, P.Y, S - 2000.0), ECC_Visibility, Q, RP) ? S - float(Bed.ImpactPoint.Z) : 2000.f;
		};
		UE_LOG(LogOutlawsAI, Display, TEXT("OLSHOT enemyfloat: the water at the spot is %.0f cm deep (surface z %.1f)"), Depth(Spot, S0), S0);
		// (deep enough: the bed under a floating man's feet, 78% of about 1.9 m under the surface, with room to bob)
		for (const double R : { Ring, Ring * 0.6, Ring * 1.5, Ring * 0.35 })
		{
			for (int32 k = 0; k < 16; ++k)
			{
				const FVector Dir = FRotator(0.0, (k + Skip * 5) * 22.5, 0.0).Vector();
				const FVector P = Spot + Dir * R;
				float S = 0.f;
				if (!L->WaterAt(FVector(P.X, P.Y, S0 - 20.0), S) || FMath::Abs(S - S0) > 5.f) continue;
				if (Depth(P, S) < 175.f) continue;
				// and open water between the spot and there (no rock in the way)
				FHitResult Rock;
				if (W->LineTraceSingleByChannel(Rock, FVector(Spot.X, Spot.Y, S - 30.0), FVector(P.X, P.Y, S - 30.0), ECC_Visibility, Q, RP)) continue;
				OutAt = FVector(P.X, P.Y, S);
				OutSurface = S;
				UE_LOG(LogOutlawsAI, Display, TEXT("OLSHOT enemyfloat: deep water %.0f cm out, %.0f cm deep"), R, Depth(P, S));
				return true;
			}
		}
		return false;
	}

	AOutlawsEnemy* NearestMan(UWorld* W, const FVector& At, const AOutlawsEnemy* Not)
	{
		AOutlawsEnemy* Best = nullptr;
		double BestD = 1e18;
		for (TActorIterator<AOutlawsEnemy> It(W); It; ++It)
		{
			AOutlawsEnemy* E = *It;
			if (E == Not || E->IsDead() || E->bBoss || E->IsCritter() || E->IsVoiceOnly() || E->bNotBadGuy || E->bStationary || E->CoopSelf >= 0 || E->CoopRole == 2) continue;
			const double D = FVector::DistSquared(E->GetActorLocation(), At);
			if (D < BestD) { BestD = D; Best = E; }
		}
		return Best;
	}

	/** Dropped (or thrown, Vel) in at At from Over cm over the surface, awake. */
	void PutIn(AOutlawsEnemy* E, const FVector& At, double Over, AActor* By, const FVector& Vel)
	{
		E->SetDormant(false);
		E->SetThrottleTier(0);
		E->WakeForTest();
		const float Half = E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		E->SetActorLocation(At + FVector(0.0, 0.0, Over + Half), false, nullptr, ETeleportType::TeleportPhysics);
		if (Vel.IsZero())
		{
			E->GetCharacterMovement()->StopMovementImmediately();
			E->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
		}
		else
		{
			E->BeginHeld(By);
			E->EndHeld(Vel);
		}
	}
}

void AOutlawsPlayer::EnemyFloatTestTick()
{
	using namespace OutlawsEnemyFloatTestLocal;
	AOutlawsLevelActor* L = Level.Get();
	UWorld* W = GetWorld();
	if (!L || !W) return;
	static TWeakObjectPtr<AOutlawsEnemy> A, B;
	static FVector AAt, BAt, Pos0;
	static float Surface = 0.f;
	static int32 Fails = 0, Passes = 0, Logged = 0, Shots0 = 0;
	static double Mark = 0.0, HoldStart = 0.0, DiedAt = -1.0;
	auto Check = [](bool bOk, const FString& What)
	{
		(bOk ? Passes : Fails) += 1;
		UE_LOG(LogOutlawsAI, Display, TEXT("OLSHOT enemyfloat %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
	};
	auto Line = [](const AOutlawsEnemy* E, float S)
	{
		// the water line as a share of his standing height over his feet
		const float Half = E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		const float Feet = float(E->GetActorLocation().Z) - Half;
		return (S - Feet) / (2.f * Half);
	};
	auto Finish = [&]()
	{
		UE_LOG(LogOutlawsAI, Display, TEXT("OLSHOT enemyfloat RESULT %s (%d pass, %d fail)"), Fails == 0 && Passes > 0 ? TEXT("PASS") : TEXT("FAIL"), Passes, Fails);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	};
	auto HoldUnder = [&](AOutlawsEnemy* E, const FVector& At)
	{
		// held at the hand, his head well under the surface (a grab's hold; the hand's place set as the test's)
		if (!E || E->IsDead() || !E->bHeldByPlayer) return;
		const float Half = E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		E->SetActorLocation(FVector(At.X, At.Y, Surface - Half - 60.0), false, nullptr, ETeleportType::TeleportPhysics);
	};
	const double T = ShotTime;
	L->bEnemiesActive = true;            // (-OLShots holds them still: here they think, wade and shoot)
	switch (ShotStep)
	{
	case 0:
		if (T < 4.0) return;
		L->bInvincible = true;
		if (!L->WaterAt(Spot, Surface))
		{
			UE_LOG(LogOutlawsAI, Display, TEXT("OLSHOT enemyfloat: no water at the spot in %s"), *L->LevelName);
			Fails++;
			Finish();
			return;
		}
		// the player swimming at the spot, the man dropped in 4 m off
		TeleportTo(FVector(Spot.X, Spot.Y, Surface - 40.0), FRotator::ZeroRotator, false, true);
		A = NearestMan(W, Spot, nullptr);
		if (!DeepWaterNear(L, W, 400.0, 0, AAt, Surface) || !A.IsValid())
		{
			UE_LOG(LogOutlawsAI, Display, TEXT("OLSHOT enemyfloat: no deep water or no bad guy near the spot (man %d)"), A.IsValid() ? 1 : 0);
			Fails++;
			Finish();
			return;
		}
		PutIn(A.Get(), AAt, 150.0, this, FVector::ZeroVector);
		UE_LOG(LogOutlawsAI, Display, TEXT("OLSHOT enemyfloat: surface z %.1f at the spot (sector %d); %s dropped in from 1.5 m at %s"), Surface, L->PlayerSectorIndex(), *A->GetOutlawsName(),
			*AAt.ToCompactString());
		Mark = T;
		Logged = 0;
		ShotStep = 1;
		return;
	case 1:
		// his water line every half second for 6 s: down into the water, up to the line, settled
		if (!A.IsValid()) { Fails++; Finish(); return; }
		if (T < Mark + 0.5 * (Logged + 1)) return;
		++Logged;
		UE_LOG(LogOutlawsAI, Display, TEXT("OLSHOT enemyfloat drop t+%.1f: %s"), 0.5 * Logged, *A->DebugWater());
		if (Logged < 12) return;
		{
			const float Ln = Line(A.Get(), Surface);
			Check(A->IsFloating() && Ln > 0.68f && Ln < 0.9f, FString::Printf(TEXT("dropped in, he floats with the line at %.0f%% of his height (chest to neck: 68 to 90)"), Ln * 100.f));
		}
		// alerted: he shoots at the player, and wades 2.5 m toward the spot
		A->Wake(false);
		Shots0 = A->ShotsTotal;
		Pos0 = A->GetActorLocation();
		A->DebugSwimTo(Pos0 + (FVector(Spot.X, Spot.Y, Pos0.Z) - Pos0).GetSafeNormal() * 250.0);
		Mark = T;
		ShotStep = 2;
		return;
	case 2:
		if (!A.IsValid()) { Fails++; Finish(); return; }
		// (the bad guys' pace is the menu's, a third of real time by default: his shot timer runs that slowly; his first shot,
		// or 45 s)
		if (T < Mark + 10.0 || (A->ShotsTotal == Shots0 && T < Mark + 45.0)) return;
		{
			const double Moved = FVector::Dist2D(A->GetActorLocation(), Pos0);
			const float Ln = Line(A.Get(), Surface);
			UE_LOG(LogOutlawsAI, Display, TEXT("OLSHOT enemyfloat awake %.0f s: %s, moved %.0f cm; %s; sight: %s"), T - Mark, *A->DebugWater(), Moved, *A->DebugBossState(), *A->DebugSight());
			Check(A->ShotsTotal > Shots0, FString::Printf(TEXT("he shoots from the water (%d shots)"), A->ShotsTotal - Shots0));
			Check(Moved > 100.0, FString::Printf(TEXT("he wades (%.0f cm across)"), Moved));
			Check(!A->IsDead() && A->IsFloating() && Ln > 0.68f && Ln < 0.9f, FString::Printf(TEXT("still at the surface (line %.0f%%)"), Ln * 100.f));
		}
		// held under
		A->BeginHeld(this);
		AAt = A->GetActorLocation();
		HoldStart = T;
		DiedAt = -1.0;
		Logged = 0;
		ShotStep = 3;
		return;
	case 3:
		if (!A.IsValid()) { Fails++; Finish(); return; }
		if (!A->IsDead())
		{
			HoldUnder(A.Get(), AAt);
			if (T > HoldStart + 25.0 && Logged == 0)
			{
				Logged = 1;
				Check(!A->IsDead(), FString::Printf(TEXT("held under 25 s, alive (%s)"), *A->DebugWater()));
			}
			if (T < HoldStart + 40.0) return;
		}
		else if (DiedAt < 0.0)
		{
			DiedAt = T - HoldStart;
			UE_LOG(LogOutlawsAI, Display, TEXT("OLSHOT enemyfloat: %s died %.1f s into the hold (drowned %d): %s"), *A->GetOutlawsName(), DiedAt, A->bDrowned ? 1 : 0, *A->DebugWater());
		}
		Check(A->IsDead() && A->bDrowned && DiedAt > 29.0 && DiedAt < 31.5, FString::Printf(TEXT("held under, he drowns at 30 s (died %.1f s in, held %.1f s)"), DiedAt, A->DrownHeld));
		Check(!A->bHeldByPlayer, TEXT("the drowned man is out of the hand"));
		// the second: thrown in from 2 m over the water
		if (!DeepWaterNear(L, W, 450.0, 1, BAt, Surface) || !(B = NearestMan(W, Spot, A.Get())).IsValid())
		{
			UE_LOG(LogOutlawsAI, Display, TEXT("OLSHOT enemyfloat: no second man or water"));
			Fails++;
			Finish();
			return;
		}
		{
			// from a metre back, thrown on toward the spot (he comes down about over the deep water found)
			const FVector Way = (FVector(Spot.X, Spot.Y, BAt.Z) - BAt).GetSafeNormal2D();
			PutIn(B.Get(), BAt - Way * 100.0, 200.0, this, Way * 150.0 + FVector(0.0, 0.0, 150.0));
		}
		Mark = T;
		Logged = 0;
		ShotStep = 4;
		return;
	case 4:
		if (!B.IsValid()) { Fails++; Finish(); return; }
		if (T < Mark + 0.5 * (Logged + 1)) return;
		++Logged;
		UE_LOG(LogOutlawsAI, Display, TEXT("OLSHOT enemyfloat throw t+%.1f: %s (thrown %d)"), 0.5 * Logged, *B->DebugWater(), B->bThrown ? 1 : 0);
		if (Logged < 16) return;
		{
			const float Ln = Line(B.Get(), Surface);
			Check(!B->bThrown && B->IsFloating() && Ln > 0.68f && Ln < 0.9f && !B->IsDead(), FString::Printf(TEXT("thrown in, he bobs up to the line (%.0f%%), unhurt (health %.1f)"), Ln * 100.f, B->GetHealth()));
		}
		B->BeginHeld(this);
		BAt = B->GetActorLocation();
		HoldStart = T;
		ShotStep = 5;
		return;
	case 5:
		if (!B.IsValid()) { Fails++; Finish(); return; }
		if (T < HoldStart + 20.0 && !B->IsDead())
		{
			HoldUnder(B.Get(), BAt);
			return;
		}
		Check(!B->IsDead(), FString::Printf(TEXT("held under 20 s and let go: alive (held under %.1f s)"), B->DrownHeld));
		B->EndHeld(FVector::ZeroVector);
		Mark = T;
		ShotStep = 6;
		return;
	case 6:
		if (!B.IsValid()) { Fails++; Finish(); return; }
		if (T < Mark + 6.0) return;
		{
			const float Ln = Line(B.Get(), Surface);
			UE_LOG(LogOutlawsAI, Display, TEXT("OLSHOT enemyfloat let go 6 s: %s"), *B->DebugWater());
			Check(!B->IsDead() && B->IsFloating() && Ln > 0.68f && Ln < 0.9f, FString::Printf(TEXT("let go, he comes up and floats again (line %.0f%%)"), Ln * 100.f));
			Check(B->DrownHeld <= 0.f, FString::Printf(TEXT("3 s or more at the surface: the time held under starts again (%.1f s)"), B->DrownHeld));
		}
		// a 20 s dunk, the surface, then a 12 s one (32 s in all): he lives
		B->BeginHeld(this);
		BAt = B->GetActorLocation();
		HoldStart = T;
		ShotStep = 7;
		return;
	case 7:
		if (!B.IsValid()) { Fails++; Finish(); return; }
		if (T < HoldStart + 12.0 && !B->IsDead())
		{
			HoldUnder(B.Get(), BAt);
			return;
		}
		Check(!B->IsDead(), FString::Printf(TEXT("20 s under, up 6 s, then 12 s under: alive (held under %.1f s)"), B->DrownHeld));
		B->EndHeld(FVector::ZeroVector);
		Mark = T;
		ShotStep = 8;
		return;
	case 8:
		if (!B.IsValid()) { Fails++; Finish(); return; }
		if (T < Mark + 6.0) return;
		Check(!B->IsDead() && B->DrownHeld <= 0.f, FString::Printf(TEXT("up 6 s again: count back to 0 (%.1f s)"), B->DrownHeld));
		// a short breath does not start it again: 15 s under, up 1.5 s, under again; he drowns 15 s into the second dunk
		B->BeginHeld(this);
		BAt = B->GetActorLocation();
		HoldStart = T;
		ShotStep = 9;
		return;
	case 9:
		if (!B.IsValid()) { Fails++; Finish(); return; }
		if (T < HoldStart + 15.0 && !B->IsDead())
		{
			HoldUnder(B.Get(), BAt);
			return;
		}
		Check(!B->IsDead(), FString::Printf(TEXT("15 s under: alive (held under %.1f s)"), B->DrownHeld));
		B->EndHeld(FVector::ZeroVector);
		Mark = T;
		ShotStep = 10;
		return;
	case 10:
		if (!B.IsValid()) { Fails++; Finish(); return; }
		if (T < Mark + 1.5) return;
		Check(!B->IsDead() && B->DrownHeld > 14.0f, FString::Printf(TEXT("up only 1.5 s: the count stays (%.1f s)"), B->DrownHeld));
		B->BeginHeld(this);
		BAt = B->GetActorLocation();
		HoldStart = T;
		DiedAt = -1.0;
		ShotStep = 11;
		return;
	case 11:
		if (!B.IsValid()) { Fails++; Finish(); return; }
		if (!B->IsDead())
		{
			HoldUnder(B.Get(), BAt);
			if (T < HoldStart + 25.0) return;
		}
		else if (DiedAt < 0.0)
		{
			DiedAt = T - HoldStart;
			UE_LOG(LogOutlawsAI, Display, TEXT("OLSHOT enemyfloat: %s died %.1f s into the second dunk (drowned %d): %s"), *B->GetOutlawsName(), DiedAt, B->bDrowned ? 1 : 0, *B->DebugWater());
		}
		Check(B->IsDead() && B->bDrowned && DiedAt > 13.5 && DiedAt < 16.5, FString::Printf(TEXT("15 s, a 1.5 s breath, then under again: drowns at 30 s in all (died %.1f s into the second dunk)"), DiedAt));
		Finish();
		ShotStep = 12;
		return;
	default:
		return;
	}
}

OL_SHOT_MATCH(OLEnemyFloatTest) { return OLFlags::Param(TEXT("OLEnemyFloatTest")); }
OL_SHOT_TEST(OLEnemyFloatTest, 290)
{
	EnemyFloatTestTick();            // (REQ-ENEMYFLOAT: OutlawsEnemyFloatTest.cpp)
}

void UOutlawsCoop::TickFloatTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	using namespace OutlawsEnemyFloatTestLocal;
	const bool bHost = IsHost();
	const TCHAR* Me = bHost ? TEXT("host") : TEXT("client");
	auto Check = [this, Me](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlawsAI, Log, TEXT("OLCOOP float check %s (%s): %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), Me, *What);
		if (bOk) ++EfPasses; else ++EfFails;
	};
	auto Result = [this, Me]()
	{
		UE_LOG(LogOutlawsAI, Log, TEXT("OLCOOP float %s RESULT %s (%d passed, %d failed)"), Me, EfFails == 0 && EfPasses > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), EfPasses, EfFails);
	};
	UWorld* W = L->GetWorld();
	// the floating man by the spot: a living one whose feet are in the water within 30 m of it (he wades toward the players),
	// his line as a share of his height
	auto Floater = [W, L](float& OutLine, FString& OutWho) -> bool
	{
		for (TActorIterator<AOutlawsEnemy> It(W); It; ++It)
		{
			const AOutlawsEnemy* E = *It;
			if (E->IsDead() || E->IsCritter() || FVector::Dist2D(E->GetActorLocation(), Spot) > 3000.0) continue;
			const float Half = E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			const float Feet = float(E->GetActorLocation().Z) - Half;
			float S = 0.f;
			if (!L->WaterAt(FVector(E->GetActorLocation().X, E->GetActorLocation().Y, Feet + 5.0), S)) continue;
			OutLine = (S - Feet) / (2.f * Half);
			OutWho = FString::Printf(TEXT("%s (role %d) feet z %.1f, surface z %.1f"), *E->GetOutlawsName(), E->CoopRole, Feet, S);
			return true;
		}
		return false;
	};
	switch (EfStep)
	{
	case 0:
		if (!L->LevelName.Equals(TEXT("canyon"), ESearchCase::IgnoreCase) || !L->HasActorBegunPlay()) return;
		if (bHost)
		{
			const AOutlawsCoopLink* CL = GetLink(PartnerIndex());
			if (!CL || CL->BuiltSerial != StartedSerial || !PartnerPuppet()) return;
		}
		if (EfAt == 0.0) { EfAt = Now + 4.0; return; }
		if (Now < EfAt) return;
		L->bInvincible = true;
		{
			float S = 0.f;
			L->WaterAt(Spot, S);
			P->TeleportTo(FVector(Spot.X + (bHost ? 0.0 : -150.0), Spot.Y, S - 40.0), FRotator::ZeroRotator, false, true);
		}
		EfAt = Now + (bHost ? 2.0 : 14.0);
		EfStep = bHost ? 1 : 20;
		return;

	// ---- the host ----
	case 1:
		if (Now < EfAt) return;
		{
			FVector At;
			float S = 0.f;
			AOutlawsEnemy* E = NearestMan(W, Spot, nullptr);
			if (!E || !DeepWaterNear(L, W, 400.0, 0, At, S))
			{
				Check(false, TEXT("a bad guy and deep water by the spot"));
				Result();
				EfAt = Now + 20.0;
				EfStep = 5;
				return;
			}
			PutIn(E, At, 150.0, P, FVector::ZeroVector);
			CoopDropped = E;
			UE_LOG(LogOutlawsAI, Log, TEXT("OLCOOP float: the host drops %s (copy %d) into the water at %s"), *E->GetOutlawsName(), E->CoopIndex, *At.ToCompactString());
		}
		EfAt = Now + 8.0;
		EfStep = 2;
		return;
	case 2:
		if (Now < EfAt) return;
		{
			float Ln = 0.f;
			FString Who;
			const bool bOk = Floater(Ln, Who);
			if (CoopDropped.IsValid()) UE_LOG(LogOutlawsAI, Log, TEXT("OLCOOP float: the dropped man now: %s, %.0f cm from the spot"), *CoopDropped->DebugWater(),
				FVector::Dist2D(CoopDropped->GetActorLocation(), Spot));
			Check(bOk && Ln > 0.68f && Ln < 0.9f, FString::Printf(TEXT("the host's copy floats with the line at %.0f%% (%s)"), Ln * 100.f, *Who));
		}
		Result();
		EfAt = Now + 14.0;               // (the client looks at its copy meanwhile)
		EfStep = 5;
		return;
	case 5:
		if (Now < EfAt) return;
		TestSay(TEXT("quit"), 0);
		EfAt = Now + 2.0;
		EfStep = 6;
		return;
	case 6:
		if (Now < EfAt) return;
		Summary(TEXT("done"));
		QuitForTest();
		EfStep = 7;
		return;

	// ---- the client ----
	case 20:
		if (Now < EfAt) return;
		{
			float Ln = 0.f;
			FString Who;
			const bool bOk = Floater(Ln, Who);
			Check(bOk && Ln > 0.68f && Ln < 0.9f, FString::Printf(TEXT("the client's copy shows him floating, the line at %.0f%% (%s)"), Ln * 100.f, *Who));
			EfAt = Now + 2.0;
		}
		EfStep = 21;
		return;
	case 21:
		if (Now < EfAt) return;
		{
			float Ln = 0.f;
			FString Who;
			const bool bOk = Floater(Ln, Who);
			Check(bOk && Ln > 0.68f && Ln < 0.9f, FString::Printf(TEXT("two seconds on, still at the surface on the client (%.0f%%, %s)"), Ln * 100.f, *Who));
		}
		Result();
		EfStep = 30;
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
