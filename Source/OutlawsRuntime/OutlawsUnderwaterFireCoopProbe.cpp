// BUG-UNDERWATERFIRE's co-op probe: the host decides for the bad guys, so a client's head under water has to reach it.  Two machines,
// -OLUnderwaterFireProbe=coop on each:
//   host:   -OLCoopAuto -OLCoopHost -OLCoopLevel=canyon
//   client: -OLCoopAuto -OLCoopJoin=<host>:<port>
// The client goes to the float test's water and holds its head 30 cm under for 8 s, then its eyes 30 cm over for 8 s.  The host's
// nearest bad guy, set on the client, wakes, and is watched: the client's remote view says under and his TargetHeadUnder with it, and
// he fires nothing at it in that time; then up, he goes back to firing.  Lines "OLSHOT uwfirecoop check PASS|FAIL: ..." and
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
	constexpr double UnderSeconds = 10.0, UpSeconds = 14.0;
	int32 Stage = 0, Passes = 0, Fails = 0, Client = -1, ShotsAt = 0;
	double At = -1.0, Deadline = -1.0;
	float Surface = 0.f;
	bool bDone = false, bUnderSeen = false;
	TWeakObjectPtr<AOutlawsEnemy> Man;

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
	if (Deadline < 0.0) Deadline = Now + 200.0;
	auto Finish = [&]()
	{
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT uwfirecoop RESULT %s (%s: %d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Who, Passes, Fails);
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
			Stage = 1;
			return;
		}
		case 1:
			// (the client's head under: seen here, and he holds)
			if (!E) { Check(false, Who, TEXT("the man is gone")); Finish(); return; }
			E->CoopTarget = Client;
			if (!V || !V->bHeadUnder) return;
			Check(E->TargetHeadUnder(), Who, FString::Printf(TEXT("the client's head under reached the host %.1f s into the run: the man's target is under"), Now - (Deadline - 200.0)));
			ShotsAt = E->ShotsTotal;
			At = Now;
			Stage = 2;
			return;
		case 2:
			if (!E) { Check(false, Who, TEXT("the man is gone")); Finish(); return; }
			E->CoopTarget = Client;
			if (V && V->bHeadUnder) { bUnderSeen = true; return; }
			Check(E->ShotsTotal == ShotsAt, Who, FString::Printf(TEXT("held %d shots at the client in the %.1f s its head was under"), E->ShotsTotal - ShotsAt, Now - At));
			Check(!E->TargetHeadUnder(), Who, TEXT("the head up reached the host: the man's target is not under"));
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
		At = Now + UnderSeconds;
		Stage = 1;
		return;
	case 1:
		Hold(-30.f);
		if (Now < At) return;
		Check(P->IsHeadUnderwater(), Who, TEXT("the head held under"));
		At = Now + UpSeconds;
		Stage = 2;
		return;
	case 2:
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
