// BUG-UNDERWATERFIRE's co-op probe: the host decides for the bad guys, so a client's head under water has to reach it.  Two machines,
// -OLUnderwaterFireProbe=coop on each:
//   host:   -OLCoopAuto -OLCoopHost -OLCoopLevel=canyon
//   client: -OLCoopAuto -OLCoopJoin=<host>:<port>
// The client goes to the float test's water: its eyes 30 cm over for 24 s, its head 30 cm under for 20 s, its eyes over again for
// 20 s.  The host's bad guy nearest the water, put on the bank with a clear line to that spot and set on the client, wakes and is
// watched: head up he fires at it (the control: he would have fired); head under the client's remote view says so, his
// TargetHeadUnder with it, and he fires nothing in that time (-OLOldUnderwaterFire: he fires on); up again he goes back to firing.
// The host says its result and stays until the client has left, so each machine's half ends.  Lines "OLSHOT uwfirecoop check PASS|FAIL: ..." and
// "OLSHOT uwfirecoop RESULT".

#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsEnemy.h"
#include "OutlawsFlags.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsUwFireCoopProbeLocal
{
	const FVector CoopWaterSpot(22825.247, 1800.963, -2085.545);      // (the float test's, OutlawsEnemyFloatTest.cpp)
	constexpr double UpSeconds = 24.0, UnderSeconds = 20.0, UpAgainSeconds = 20.0, ClientGrace = 30.0;
	int32 Stage = 0, Passes = 0, Fails = 0, Client = -1, ShotsAt = 0;
	double At = -1.0, Deadline = -1.0;
	float Surface = 0.f;
	bool bDone = false;
	TWeakObjectPtr<AOutlawsEnemy> Man;

	double DropFrom(UWorld* W, const FVector& P, float Top)
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsUwFireCoopDrop), false);
		FCollisionResponseParams RP;
		RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
		FHitResult Bed;
		return W->LineTraceSingleByChannel(Bed, FVector(P.X, P.Y, Top + 50.0), FVector(P.X, P.Y, Top - 2000.0), ECC_Visibility, Q, RP) ? Top - Bed.ImpactPoint.Z : 2000.0;
	}

	bool LineIsClear(UWorld* W, const FVector& A, const FVector& B)
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsUwFireCoopLine), false);
		FCollisionResponseParams RP;
		RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
		FHitResult Hit;
		return !W->LineTraceSingleByChannel(Hit, A, B, ECC_Visibility, Q, RP);
	}

	/** A dry floor 5 to 26 m from the spot with a clear line from a man's eyes to the eyes over the water (the story probe's bank). */
	bool FindBank(AOutlawsLevelActor* L, UWorld* W, float S, FVector& OutFloor)
	{
		for (double R = 500.0; R <= 2600.0; R += 150.0)
		{
			for (int32 k = 0; k < 24; ++k)
			{
				const FVector P = CoopWaterSpot + FRotator(0.0, k * 15.0, 0.0).Vector() * R;
				const double Under = DropFrom(W, P, S + 300.f);
				const FVector Floor(P.X, P.Y, S + 300.0 - Under);
				float Wet = 0.f;
				if (Under > 800.0 || Floor.Z < S + 5.0 || L->WaterAt(Floor + FVector(0.0, 0.0, 10.0), Wet)) continue;
				if (!LineIsClear(W, Floor + FVector(0.0, 0.0, 150.0), FVector(CoopWaterSpot.X, CoopWaterSpot.Y, S + 30.0))) continue;
				OutFloor = Floor;
				return true;
			}
		}
		return false;
	}

	void Check(bool bOk, const TCHAR* Who, const FString& What)
	{
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT uwfirecoop check %s: (%s) %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), Who, *What);
		if (bOk) ++Passes; else ++Fails;
	}
}

void UOutlawsCoop::TickUnderwaterFireCoopProbe(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	using namespace OutlawsUwFireCoopProbeLocal;
	if (bDone || !L || !P) return;
	const bool bHost = IsHost();
	const TCHAR* Who = bHost ? TEXT("host") : TEXT("client");
	const bool bOld = OLFlags::Param(TEXT("OLOldUnderwaterFire"));
	if (Deadline < 0.0) Deadline = Now + 200.0;
	auto Result = [&]()
	{
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT uwfirecoop RESULT %s (%s: %d passed, %d failed, %s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Who, Passes, Fails, bOld ? TEXT("-OLOldUnderwaterFire") : TEXT("new"));
	};
	auto Finish = [&]()
	{
		Result();
		bDone = true;
		FGenericPlatformMisc::RequestExit(false);
	};
	if (Now > Deadline) { Check(false, Who, FString::Printf(TEXT("done in 200 s (stage %d, level %s)"), Stage, *L->LevelName)); Finish(); return; }
	if (L->IsRebuildPending() || L->IsMoviePlaying() || !L->HasActorBegunPlay()) return;
	L->bInvincible = true;
	if (bHost)
	{
		L->EnemyPaceFloor = 1.f;
		const FOutlawsCoopRemoteView* V = Client > 0 ? RemoteViewOf(Client) : nullptr;
		AOutlawsEnemy* E = Man.Get();
		switch (Stage)
		{
		case 0:
		{
			// the client in; the bad guy nearest the water, put on it
			TArray<AOutlawsCoopLink*> Links;
			GetLinks(Links);
			for (const AOutlawsCoopLink* K : Links) if (K->PlayerIndex > 0 && !K->bBot && K->BuiltSerial == StartedSerial && K->Pose.Seq > 0) Client = K->PlayerIndex;
			if (Client <= 0) return;
			double Best = 1e18;
			for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
			{
				AOutlawsEnemy* C = Wk.Get();
				if (!C || C->IsDead() || C->bBoss || C->IsCritter() || C->IsVoiceOnly() || C->bNotBadGuy || C->bStationary || C->IsThrower()) continue;
				const double D = FVector::DistSquared(C->GetActorLocation(), CoopWaterSpot);
				if (D < Best) { Best = D; Man = C; }
			}
			Check(Man.IsValid(), Who, FString::Printf(TEXT("a man to watch, %.0f m from the water"), FMath::Sqrt(Best) / 100.0));
			if (!Man.IsValid()) { Finish(); return; }
			float S = 0.f;
			FVector Bank = FVector::ZeroVector;
			const bool bBank = L->WaterAt(CoopWaterSpot, S) && FindBank(L, L->GetWorld(), S, Bank);
			Check(bBank, Who, FString::Printf(TEXT("a dry floor in view of the water, %.0f m from it"), FVector::Dist2D(Bank, CoopWaterSpot) / 100.0));
			if (!bBank) { Finish(); return; }
			AOutlawsEnemy* M = Man.Get();
			M->SetDormant(false);
			M->SetThrottleTier(0);
			M->WakeForTest();
			M->SetActorLocation(Bank + FVector(0.0, 0.0, M->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 5.0), false, nullptr, ETeleportType::TeleportPhysics);
			M->GetCharacterMovement()->StopMovementImmediately();
			M->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
			// (he holds his ground on that line: left to chase he walked behind the rock, 2 shots in 24 s and none in 12 s even with
			// the old rule, so a count of none meant nothing)
			M->bStationary = true;
			Stage = 1;
			return;
		}
		case 1:
			// (the client at the water: he wakes on it)
			if (!E) { Check(false, Who, TEXT("the man is gone")); Finish(); return; }
			E->CoopTarget = Client;
			if (!V || FVector::Dist2D(V->Feet, CoopWaterSpot) > 200.0) return;
			E->Wake(false);
			ShotsAt = E->ShotsTotal;
			At = Now;
			Stage = 2;
			return;
		case 2:
			// (the control: the same man at the same client with its head up)
			if (!E) { Check(false, Who, TEXT("the man is gone")); Finish(); return; }
			E->CoopTarget = Client;
			if (!V || !V->bHeadUnder) return;
			Check(E->ShotsTotal > ShotsAt, Who, FString::Printf(TEXT("the control: head up he fired %d shots at the client in %.1f s; %s"), E->ShotsTotal - ShotsAt, Now - At, *E->DebugSight()));
			Check(E->TargetHeadUnder() != bOld, Who, FString::Printf(TEXT("the client's head under reached the host %.1f s into the run: the man's target is under: %d"), Now - (Deadline - 200.0), E->TargetHeadUnder() ? 1 : 0));
			ShotsAt = E->ShotsTotal;
			At = Now;
			Stage = 3;
			return;
		case 3:
			if (!E) { Check(false, Who, TEXT("the man is gone")); Finish(); return; }
			E->CoopTarget = Client;
			if (V && V->bHeadUnder) return;
			if (bOld) Check(E->ShotsTotal > ShotsAt, Who, FString::Printf(TEXT("(old rule) fired on: %d shots at the client in the %.1f s its head was under"), E->ShotsTotal - ShotsAt, Now - At));
			else Check(E->ShotsTotal == ShotsAt, Who, FString::Printf(TEXT("held %d shots at the client in the %.1f s its head was under; %s; %s"), E->ShotsTotal - ShotsAt, Now - At, *E->DebugBossState(), *E->DebugSight()));
			Check(V && !E->TargetHeadUnder(), Who, TEXT("the head up reached the host: the man's target is not under"));
			ShotsAt = E->ShotsTotal;
			At = Now;
			Stage = 4;
			return;
		case 4:
			// (up again: he goes back to firing)
			if (!E) { Check(false, Who, TEXT("the man is gone")); Finish(); return; }
			E->CoopTarget = Client;
			if (E->ShotsTotal == ShotsAt && Now < At + UpAgainSeconds - 2.0) return;
			Check(E->ShotsTotal > ShotsAt, Who, FString::Printf(TEXT("up again: the first shot %.1f s after the head came up"), Now - At));
			Result();
			Stage = 5;
			return;
		case 5:
			// (the result said, the session stays up for the client's own end: it never reached that when the host left first.  The
			// client's going ends this machine too, the auto test's quit, so the host's result goes first)
			if (Now < At + UpAgainSeconds + ClientGrace) return;
			Check(false, Who, FString::Printf(TEXT("the client left within %.0f s of its head coming up"), UpAgainSeconds + ClientGrace));
			Finish();
			return;
		default:
			return;
		}
	}
	// the client
	auto Hold = [&](float EyesOver)
	{
		const double Dz = (Surface + EyesOver) - P->Camera->GetComponentLocation().Z;
		P->SetActorLocation(FVector(CoopWaterSpot.X, CoopWaterSpot.Y, P->GetActorLocation().Z + Dz), false, nullptr, ETeleportType::TeleportPhysics);
		P->GetCharacterMovement()->StopMovementImmediately();
		if (EyesOver < 0.f) P->DiveUntil = P->GetWorld()->GetTimeSeconds() + 0.5;      // (a dive's: the surface's hold lets go)
	};
	switch (Stage)
	{
	case 0:
		if (At < 0.0) At = Now + 8.0;
		if (Now < At) return;
		if (!L->WaterAt(CoopWaterSpot, Surface)) { Check(false, Who, FString::Printf(TEXT("water at the spot in %s"), *L->LevelName)); Finish(); return; }
		P->TeleportTo(FVector(CoopWaterSpot.X, CoopWaterSpot.Y, Surface - 40.0), FRotator::ZeroRotator, false, true);
		At = Now + UpSeconds;
		Stage = 1;
		return;
	case 1:
		Hold(30.f);
		if (Now < At) return;
		Check(!P->IsHeadUnderwater(), Who, TEXT("the head held up for the control"));
		At = Now + UnderSeconds;
		Stage = 2;
		return;
	case 2:
		Hold(-30.f);
		if (Now < At) return;
		Check(P->IsHeadUnderwater(), Who, TEXT("the head held under"));
		At = Now + UpAgainSeconds;
		Stage = 3;
		return;
	case 3:
		Hold(30.f);
		if (Now < At) return;
		Check(!P->IsHeadUnderwater(), Who, TEXT("the head up again"));
		Finish();
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
