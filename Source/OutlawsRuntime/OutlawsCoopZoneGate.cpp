// BUG-MILLCLIMB's co-op test: -OLCoopAuto -OLCoopTest=zonegate on both, -OLCoopLevel=mill.  Each machine keeps its own
// player's side of the mill's zone gate (OutlawsZoneGate.cpp).  The host walks in through the gate (room 404) and stands on
// the hall's floor (sector 576); the client stays in the yard and stands on the yard's ground under the hall (sector 602), the
// two in the same place on the plan, 30 cm apart in height.  Each checks its own side, sector, footing and what is drawn.
#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsGeometry.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TickZoneGateTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	const bool bHost = IsHost();
	const TCHAR* Me = bHost ? TEXT("host") : TEXT("client");
	L->bInvincible = true;
	const double U = FOutlawsGeometry::UnitsToCm;
	const float Half = P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	auto Feet = [&]() { return (P->GetActorLocation().Z - Half) / U; };
	auto Put = [&](double X, double Y, double Z, bool bWalk)
	{
		if (bWalk) P->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
		else P->GetCharacterMovement()->DisableMovement();
		P->TeleportTo(FOutlawsGeometry::ToUE(X, Y, Z) + FVector(0.0, 0.0, Half + 2.0), P->GetActorRotation(), false, true);      // (a teleport: a jump)
		P->GetCharacterMovement()->Velocity = FVector::ZeroVector;
	};
	auto Check = [&](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP zonegate check %s (%s): %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), Me, *What);
		if (bOk) ++ZgPasses; else ++ZgFails;
	};
	auto State = [&]()
	{
		return FString::Printf(TEXT("sector %d, feet %.2f, %s; off 602 %d, 576 %d; drawn 602 %d, 576 %d"), L->PlayerSectorIndex(), Feet(), *L->DebugZoneReport(),
			L->DebugZoneOffNow(602) ? 1 : 0, L->DebugZoneOffNow(576) ? 1 : 0, L->DebugSectorDrawn(602) ? 1 : 0, L->DebugSectorDrawn(576) ? 1 : 0);
	};
	const int32 Out = L->DebugZoneOf(364), In = L->DebugZoneOf(417);
	switch (ZgStep)
	{
	case 0:
		if (!L->LevelName.Equals(TEXT("mill"), ESearchCase::IgnoreCase) || !L->HasActorBegunPlay()) return;
		if (bHost)
		{
			const AOutlawsCoopLink* CL = GetLink(PartnerIndex());
			if (!CL || CL->BuiltSerial != StartedSerial || !PartnerPuppet()) return;
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP zonegate (%s): %s; side of 364 %d, of 417 %d"), Me, *L->DebugZoneReport(), Out, In);
		ZgAt = Now + 3.0;
		ZgStep = 1;
		return;
	case 1:
		if (Now < ZgAt) return;
		if (bHost) Put(341.0, 8.0, 140.0, false);       // (BUST3, before the gate on the yard's side)
		else Put(236.0, 8.0, 49.0, true);               // (the yard)
		ZgAt = Now + 2.0;
		ZgStep = 2;
		return;
	case 2:
		if (Now < ZgAt) return;
		Check(L->DebugZoneNow() == Out, FString::Printf(TEXT("on the yard's side of the gate: %s"), *State()));
		if (!bHost)
		{
			Put(320.5, 20.0, -4.0, true);           // the yard's ground under the hall
			ZgAt = Now + 4.0;
			ZgStep = 10;
			return;
		}
		ZgStep = 3;
		return;
	case 3:
	{
		// the host walks in through the gate, 12 cm a tick, to 417 on the hall's side
		const FVector To = FOutlawsGeometry::ToUE(347.0, 8.0, 168.0) + FVector(0.0, 0.0, Half + 2.0);
		const FVector D = To - P->GetActorLocation();
		if (D.Size() > 12.0) { P->SetActorLocation(P->GetActorLocation() + D.GetSafeNormal() * 12.0, false, nullptr, ETeleportType::TeleportPhysics); return; }
		P->SetActorLocation(To, false, nullptr, ETeleportType::TeleportPhysics);
		ZgAt = Now + 0.5;
		ZgStep = 4;
		return;
	}
	case 4:
		if (Now < ZgAt) return;
		Check(L->DebugZoneNow() == In && L->DebugZoneOffNow(602), FString::Printf(TEXT("walked in through the gate: %s"), *State()));
		Put(320.5, 21.0, -4.0, true);               // the hall's floor, over the client's spot
		ZgAt = Now + 4.0;
		ZgStep = 5;
		return;
	case 5:
		if (Now < ZgAt) return;
		Check(L->DebugZoneNow() == In && L->PlayerSectorIndex() == 576 && FMath::Abs(Feet() - 21.0) < 0.7 && L->DebugZoneOffNow(602) && !L->DebugSectorDrawn(602),
			FString::Printf(TEXT("on the hall's floor (the client on the yard's ground under it): %s"), *State()));
		ZgAt = Now + 4.0;
		ZgStep = 6;
		return;
	case 6:
		if (Now < ZgAt) return;
		Check(L->PlayerSectorIndex() == 576 && FMath::Abs(Feet() - 21.0) < 0.7, FString::Printf(TEXT("still on the hall's floor 4 s on: %s"), *State()));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP zonegate host RESULT %s (%d passed, %d failed)"), ZgFails == 0 && ZgPasses > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), ZgPasses, ZgFails);
		ZgAt = Now + 4.0;
		ZgStep = 7;
		return;
	case 7:
		if (Now < ZgAt) return;
		TestSay(TEXT("quit"), 0);
		ZgAt = Now + 2.0;
		ZgStep = 8;
		return;
	case 8:
		if (Now < ZgAt) return;
		Summary(TEXT("done"));
		QuitForTest();
		ZgStep = 99;
		return;
	case 10:
		if (Now < ZgAt) return;
		Check(L->DebugZoneNow() == Out && L->PlayerSectorIndex() == 602 && FMath::Abs(Feet() - 20.0) < 0.7 && !L->DebugZoneOffNow(602) && L->DebugZoneOffNow(576) && L->DebugSectorDrawn(602),
			FString::Printf(TEXT("on the yard's ground under the hall (the host in the hall over it): %s"), *State()));
		ZgAt = Now + 4.0;
		ZgStep = 11;
		return;
	case 11:
		if (Now < ZgAt) return;
		Check(L->PlayerSectorIndex() == 602 && FMath::Abs(Feet() - 20.0) < 0.7, FString::Printf(TEXT("still on the yard's ground 4 s on: %s"), *State()));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP zonegate client RESULT %s (%d passed, %d failed)"), ZgFails == 0 && ZgPasses > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), ZgPasses, ZgFails);
		ZgStep = 99;
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
