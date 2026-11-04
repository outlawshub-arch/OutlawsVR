// BUG-UNDERWATERFIRE's probe (the user, 4 Nov: "enemies are able to shoot me while I'm underwater when they shouldn't. They
// should cease fire if I'm underwater, but continue to shoot if I'm surfaced and bobbing on the surface of the water"): -OLShots
// -OLNoFrontEnd -OLLevel=canyon -OLUnderwaterFireProbe [-OLOldUnderwaterFire].  At the float test's water in the canyon, with
// one bad guy floating in it 4 m off and one on the bank, awake, the bad guys' pace at its full: (1) the player bobbing, the eyes
// 30 cm over the surface: both shoot; (2) the head held 30 cm under: neither fires a shot, both stay after the player and alive,
// the one on the bank goes on moving or stands at the edge; (3) just coming up: the first shot, and how long it took.
// -OLOldUnderwaterFire: (2) they fire on, as before.  Lines "OLSHOT underwaterfire check PASS|FAIL: ..." and
// "OLSHOT underwaterfire RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsEnemy.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsUnderwaterFireProbeLocal
{
	const FVector WaterSpot(22825.247, 1800.963, -2085.545);       // (the float test's: the user's spot in the canyon's water)
	constexpr float EyesOffSurface = 30.f, BobSeconds = 14.f, UnderSeconds = 16.f;
	int32 Stage = 0, Passes = 0, Fails = 0, ShotsA = 0, ShotsB = 0;
	float StageT = 0.f, Surface = 0.f, FirstShotAt = -1.f;
	bool bUnderSeen = false, bBankMan = false;
	FVector Deep = FVector::ZeroVector, BankWas = FVector::ZeroVector;
	TWeakObjectPtr<AOutlawsEnemy> Floater, BankMan;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT underwaterfire check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}

	double BedDepth(UWorld* W, const FVector& P, float S)
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsUnderFireBed), false);
		FCollisionResponseParams RP;
		RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
		FHitResult Bed;
		return W->LineTraceSingleByChannel(Bed, FVector(P.X, P.Y, S + 50.0), FVector(P.X, P.Y, S - 2000.0), ECC_Visibility, Q, RP) ? S - Bed.ImpactPoint.Z : 2000.0;
	}

	bool ClearLine(UWorld* W, const FVector& A, const FVector& B)
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsUnderFireLine), false);
		FCollisionResponseParams RP;
		RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
		FHitResult Hit;
		return !W->LineTraceSingleByChannel(Hit, A, B, ECC_Visibility, Q, RP);
	}

	AOutlawsEnemy* NearestAwakeable(AOutlawsLevelActor* L, const FVector& At, const AOutlawsEnemy* Not)
	{
		AOutlawsEnemy* Best = nullptr;
		double BestD = 1e18;
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
		{
			AOutlawsEnemy* E = Wk.Get();
			if (!E || E == Not || E->IsDead() || E->bBoss || E->IsCritter() || E->IsVoiceOnly() || E->bNotBadGuy || E->bStationary || E->CoopSelf >= 0 || E->CoopRole == 2 || E->IsThrower()) continue;
			const double D = FVector::DistSquared(E->GetActorLocation(), At);
			if (D < BestD) { BestD = D; Best = E; }
		}
		return Best;
	}

	void Rouse(AOutlawsEnemy* E, const FVector& Feet)
	{
		E->SetDormant(false);
		E->SetThrottleTier(0);
		E->WakeForTest();
		E->SetActorLocation(Feet + FVector(0.0, 0.0, E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 5.0), false, nullptr, ETeleportType::TeleportPhysics);
		E->GetCharacterMovement()->StopMovementImmediately();
		E->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	}
}

OL_SHOT_MATCH(OLUnderwaterFireProbe) { return OLFlags::Has(TEXT("OLUnderwaterFireProbe")); }

OL_SHOT_TEST(OLUnderwaterFireProbe, 2464)
{
	using namespace OutlawsUnderwaterFireProbeLocal;
	if (!L || ShotTime < 0.f || Stage < 0) return;
	const bool bOld = OLFlags::Param(TEXT("OLOldUnderwaterFire"));
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT underwaterfire RESULT %s (%d passed, %d failed, %s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			bOld ? TEXT("-OLOldUnderwaterFire") : TEXT("new"));
		ShotTime = -1.f;
		Stage = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	if (L->IsRebuildPending() || L->IsMoviePlaying() || bMenuOpen) { StageT = 0.f; return; }
	L->StartSaveDelay = -1.f;
	L->AutosaveWait = 0.f;
	L->bInvincible = true;
	L->bEnemiesActive = true;           // (-OLShots holds them still: here they think and shoot)
	L->EnemyPaceFloor = 1.f;            // (their timers at full speed: a third of the wait)
	StageT += Dt;
	auto Hold = [&](float EyesOver)
	{
		// the eyes that far over the surface, the body straight under them, every frame
		const double Dz = (Surface + EyesOver) - Camera->GetComponentLocation().Z;
		SetActorLocation(FVector(Deep.X, Deep.Y, GetActorLocation().Z + Dz), false, nullptr, ETeleportType::TeleportPhysics);
		GetCharacterMovement()->StopMovementImmediately();
		// (under: a dive's, or the surface's hold has the body back up at its float depth before the air's check)
		if (EyesOver < 0.f) DiveUntil = GetWorld()->GetTimeSeconds() + 0.5;
	};
	const AOutlawsEnemy* A = Floater.Get();
	const AOutlawsEnemy* B = BankMan.Get();
	const int32 SaNow = A ? A->ShotsTotal : 0, SbNow = B ? B->ShotsTotal : 0;
	auto Mark = [&]() { ShotsA = SaNow; ShotsB = SbNow; StageT = 0.f; };
	switch (Stage)
	{
	case 0:
	{
		if (StageT < 4.f) return;
		UWorld* W = GetWorld();
		if (!L->WaterAt(WaterSpot, Surface)) { Check(false, FString::Printf(TEXT("water at the spot in %s"), *L->LevelName)); Finish(); return; }
		// the deepest place about the spot, for the body; a floater's water 4 m off; a dry floor with a line to the player
		double Best = BedDepth(W, WaterSpot, Surface);
		Deep = FVector(WaterSpot.X, WaterSpot.Y, Surface);
		for (const double R : { 200.0, 300.0, 400.0, 600.0 })
		{
			for (int32 k = 0; k < 16; ++k)
			{
				const FVector P = WaterSpot + FRotator(0.0, k * 22.5, 0.0).Vector() * R;
				float S = 0.f;
				if (!L->WaterAt(FVector(P.X, P.Y, Surface - 20.0), S) || FMath::Abs(S - Surface) > 5.f) continue;
				const double D = BedDepth(W, P, S);
				if (D > Best && ClearLine(W, FVector(WaterSpot.X, WaterSpot.Y, S - 30.0), FVector(P.X, P.Y, S - 30.0))) { Best = D; Deep = FVector(P.X, P.Y, S); }
			}
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT underwaterfire the body's water %.0f cm deep at %s, surface z %.1f"), Best, *Deep.ToCompactString(), Surface);
		FVector FloatAt = FVector::ZeroVector, BankAt = FVector::ZeroVector;
		bool bFloat = false;
		bBankMan = false;
		for (int32 k = 0; k < 16 && !bFloat; ++k)
		{
			const FVector P = Deep + FRotator(0.0, k * 22.5, 0.0).Vector() * 400.0;
			float S = 0.f;
			if (!L->WaterAt(FVector(P.X, P.Y, Surface - 20.0), S) || FMath::Abs(S - Surface) > 5.f || BedDepth(W, P, S) < 175.0) continue;
			if (!ClearLine(W, FVector(Deep.X, Deep.Y, S + 40.0), FVector(P.X, P.Y, S + 40.0))) continue;
			FloatAt = FVector(P.X, P.Y, S + 150.0);
			bFloat = true;
		}
		for (double R = 500.0; R <= 2600.0 && !bBankMan; R += 150.0)
		{
			for (int32 k = 0; k < 24 && !bBankMan; ++k)
			{
				const FVector P = Deep + FRotator(0.0, k * 15.0, 0.0).Vector() * R;
				float S = 0.f;
				const double Under = BedDepth(W, FVector(P.X, P.Y, Surface), Surface + 300.f);     // (from 3.5 m over the surface, down)
				const FVector Floor(P.X, P.Y, Surface + 300.0 - Under);
				if (Under > 800.0 || Floor.Z < Surface + 5.0 || L->WaterAt(Floor + FVector(0.0, 0.0, 10.0), S)) continue;
				if (!ClearLine(W, Floor + FVector(0.0, 0.0, 150.0), FVector(Deep.X, Deep.Y, Surface + 30.0))) continue;
				BankAt = Floor;
				bBankMan = true;
			}
		}
		AOutlawsEnemy* MA = NearestAwakeable(L, Deep, nullptr);
		AOutlawsEnemy* MB = bBankMan ? NearestAwakeable(L, Deep, MA) : nullptr;
		Check(bFloat && MA, FString::Printf(TEXT("a man for the water (%d, water 4 m off %d)"), MA ? 1 : 0, bFloat ? 1 : 0));
		Check(bBankMan && MB, FString::Printf(TEXT("a man for the bank (%d, dry floor in view %d at %s)"), MB ? 1 : 0, bBankMan ? 1 : 0, *BankAt.ToCompactString()));
		if (!MA || !bFloat) { Finish(); return; }
		TeleportTo(FVector(Deep.X, Deep.Y, Surface - 40.0), FRotator::ZeroRotator, false, true);
		Rouse(MA, FloatAt);
		if (MB) Rouse(MB, BankAt);
		Floater = MA;
		BankMan = MB;
		Stage = 1;
		StageT = 0.f;
		return;
	}
	case 1:
		// (settling: the floater comes up to his line)
		Hold(EyesOffSurface);
		if (StageT < 6.f) return;
		if (AOutlawsEnemy* E = Floater.Get()) E->Wake(false);
		if (AOutlawsEnemy* E = BankMan.Get()) E->Wake(false);
		Mark();
		Stage = 2;
		return;
	case 2:
		// (1) bobbing: a shot from each, the longest wait about half a minute (a man's shoot timer, a burst's end)
		Hold(EyesOffSurface);
		if (StageT < BobSeconds || ((SaNow == ShotsA || (B && SbNow == ShotsB)) && StageT < 45.f)) return;
		Check(!bHeadUnder, FString::Printf(TEXT("(1) the eyes %.0f cm over the surface: the head is not under"), EyesOffSurface));
		Check(A && SaNow > ShotsA, FString::Printf(TEXT("(1) bobbing: the man in the water shoots (%d in %.0f s; %s)"), SaNow - ShotsA, StageT, A ? *A->DebugSight() : TEXT("gone")));
		if (bBankMan && B) Check(SbNow > ShotsB, FString::Printf(TEXT("(1) bobbing: the man on the bank shoots (%d in %.0f s; %s)"), SbNow - ShotsB, StageT, *B->DebugSight()));
		bUnderSeen = false;
		StageT = 0.f;
		Stage = 3;
		return;
	case 3:
		// (2) the head under: counted from the first frame it is
		Hold(-EyesOffSurface);
		if (!bUnderSeen)
		{
			if (!bHeadUnder) { if (StageT > 2.f) { Check(false, TEXT("(2) the head went under")); Finish(); } return; }
			bUnderSeen = true;
			if (B) BankWas = B->GetActorLocation();
			Mark();
			return;
		}
		if (StageT < UnderSeconds) return;
		{
			const int32 DA = SaNow - ShotsA, DB = SbNow - ShotsB;
			if (bOld) Check(DA + DB > 0, FString::Printf(TEXT("(2, old rule) the head under: they fire on: %d and %d shots in %.0f s"), DA, DB, UnderSeconds));
			else
			{
				Check(bHeadUnder && DA == 0 && DB == 0, FString::Printf(TEXT("(2) the head under for %.0f s (under %d): %d shots from the man in the water, %d from the one on the bank"), UnderSeconds, bHeadUnder ? 1 : 0, DA, DB));
				Check(A && !A->IsDead() && A->GetState() == EOutlawsEnemyState::Chase && A->TestSeesTarget(), FString::Printf(TEXT("(2) the man in the water stays after the player: %s; %s"), A ? *A->DebugBossState() : TEXT("gone"), A ? *A->DebugSight() : TEXT("")));
				if (bBankMan && B)
				{
					Check(!B->IsDead() && B->GetState() == EOutlawsEnemyState::Chase, FString::Printf(TEXT("(2) the man on the bank stays after the player, moved %.0f cm: %s; %s"), FVector::Dist(BankWas, B->GetActorLocation()), *B->DebugBossState(), *B->DebugSight()));
				}
			}
		}
		FirstShotAt = -1.f;
		Mark();
		Stage = 4;
		return;
	case 4:
		// (3) just coming up
		Hold(EyesOffSurface);
		if (FirstShotAt < 0.f && !bHeadUnder && (SaNow > ShotsA || SbNow > ShotsB)) FirstShotAt = StageT;
		if (FirstShotAt < 0.f && StageT < 45.f) return;
		Check(!bHeadUnder && FirstShotAt >= 0.f, FString::Printf(TEXT("(3) just coming up: the first shot %.1f s after the head came up; %d and %d shots in %.0f s"), FirstShotAt, SaNow - ShotsA, SbNow - ShotsB, StageT));
		Finish();
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
