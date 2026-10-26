// BUG-JUMPPICK's co-op test: -OLCoopAuto -OLCoopTest=jumppick | jumppicklate on both, -OLCoopLevel=<level> on the host and
// -OLJumpPickSpot=pX_Y_Z_P_Q on both: a spot on sector P's floor (a catwalk, pier or tunnel floor running through the air of
// a bigger sector Q).  A client's start and a late joiner's place beside the host are jumps for its own tracking (the
// level's start, a teleport): each must take the floor stood on, not the room under it.
//   jumppick:     the client starts on the spot (its start moved there in OnLevelBuilt); 6 s on it checks its sector, its
//                 feet and that Q is not drawn
//   jumppicklate: the host goes in alone and stands on the spot; the client joins late (-OLCoopJoinDelay) and is put beside
//                 the host; 6 s on it checks that it stands on its own sector's floor at the host's height (not fallen through)
#include "OutlawsCoop.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsLevel.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsPlayer.h"
#include "OutlawsRemotePlayer.h"
#include "OutlawsGeometry.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"

#if WITH_OUTLAWS_TESTS
bool UOutlawsCoop::JumpPickSpot(FVector& OutOl, int32& OutP, int32& OutQ) const
{
	FString Spot;
	if (!OLFlags::Value(TEXT("OLJumpPickSpot"), Spot) || Spot.Len() < 2) return false;
	TArray<FString> F;
	Spot.Mid(1).ParseIntoArray(F, TEXT("_"));
	if (F.Num() < 5) return false;
	OutOl = FVector(FCString::Atod(*F[0]), FCString::Atod(*F[1]), FCString::Atod(*F[2]));
	OutP = FCString::Atoi(*F[3]);
	OutQ = FCString::Atoi(*F[4]);
	return true;
}

void UOutlawsCoop::TickJumpPickTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	const bool bHost = IsHost();
	const bool bLate = TestOnly == TEXT("jumppicklate");
	const TCHAR* Me = bHost ? TEXT("host") : TEXT("client");
	L->bInvincible = true;
	FVector Ol;
	int32 Ps = -1, Qs = -1;
	if (!JumpPickSpot(Ol, Ps, Qs) || !L->Inf) return;
	const double U = FOutlawsGeometry::UnitsToCm;
	const float Half = P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	auto Feet = [&]() { return (P->GetActorLocation().Z - Half) / U; };
	auto Check = [&](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP jumppick check %s (%s): %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), Me, *What);
		if (bOk) ++ZgPasses; else ++ZgFails;
	};
	// the sector the body is tracked in, its floor under the body, the feet, and whether P and Q are drawn
	auto State = [&](double& OutFloor)
	{
		const int32 S = L->PlayerSectorIndex();
		const FVector2D Plan = FOutlawsGeometry::ToOutlawsPlan(P->GetActorLocation());
		OutFloor = L->Inf->Live.Sectors.IsValidIndex(S) ? L->Inf->Live.SurfaceY(L->Inf->Live.Sectors[S], Plan, true) : -1e9;
		return FString::Printf(TEXT("sector %d (floor %.2f there), feet %.2f, at %.2f %.2f; drawn P %d Q %d"), S, OutFloor, Feet(), Plan.X, Plan.Y,
			L->DebugSectorDrawn(Ps) ? 1 : 0, L->DebugSectorDrawn(Qs) ? 1 : 0);
	};
	const AOutlawsCoopLink* CL = bHost ? GetLink(PartnerIndex()) : nullptr;
	const bool bPartnerUp = CL && CL->BuiltSerial == StartedSerial && PartnerPuppet();
	switch (ZgStep)
	{
	case 0:
		if (!L->HasActorBegunPlay() || !L->LevelName.Equals(BuiltLevel, ESearchCase::IgnoreCase)) return;
		if (bHost && !bLate && !bPartnerUp) return;
		if (!bHost && bLate && JoinsApplied == 0) return;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP jumppick (%s, %s): level %s, the spot %.2f %.2f %.2f on sector %d (sharing space with %d)"), Me, *TestOnly, *L->LevelName, Ol.X, Ol.Y, Ol.Z, Ps, Qs);
		ZgAt = Now + (bHost && bLate ? 1.0 : 6.0);
		ZgStep = bHost ? (bLate ? 1 : 10) : 20;
		return;
	case 1:
		// the host held on the spot while the tracking picks, then it walks there
		if (Now < ZgAt) return;
		P->GetCharacterMovement()->DisableMovement();
		P->TeleportTo(FOutlawsGeometry::ToUE(Ol.X, Ol.Y, Ol.Z) + FVector(0.0, 0.0, Half + 2.0), P->GetActorRotation(), false, true);
		P->GetCharacterMovement()->Velocity = FVector::ZeroVector;
		ZgAt = Now + 2.0;
		ZgStep = 2;
		return;
	case 2:
		if (Now < ZgAt) return;
		P->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
		ZgAt = Now + 3.0;
		ZgStep = 3;
		return;
	case 3:
	{
		if (Now < ZgAt) return;
		double Fl = 0.0;
		const FString St = State(Fl);
		Check(L->PlayerSectorIndex() == Ps && FMath::Abs(Feet() - Ol.Y) < 1.0, FString::Printf(TEXT("the host stands on the spot: %s"), *St));
		ZgStep = 10;
		return;
	}
	case 10:
		// the host waits for the client (a late one joins now) and its checks
		if (!bPartnerUp) return;
		ZgAt = Now + 14.0;
		ZgStep = 11;
		return;
	case 11:
		if (Now < ZgAt) return;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP jumppick host RESULT %s (%d passed, %d failed)"), ZgFails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), ZgPasses, ZgFails);
		TestSay(TEXT("quit"), 0);
		ZgAt = Now + 2.0;
		ZgStep = 12;
		return;
	case 12:
		if (Now < ZgAt) return;
		Summary(TEXT("done"));
		QuitForTest();
		ZgStep = 99;
		return;
	case 20:
	{
		if (Now < ZgAt) return;
		double Fl = 0.0;
		const FString St = State(Fl);
		const int32 S = L->PlayerSectorIndex();
		if (!bLate)
		{
			Check(S == Ps && FMath::Abs(Feet() - Ol.Y) < 1.0 && !L->DebugSectorDrawn(Qs), FString::Printf(TEXT("the client started on the spot: %s"), *St));
		}
		else
		{
			// beside the host: on a floor up to 4 units under the host's (SpotBeside's reach), the one its sector says
			const bool bOnOwnFloor = FMath::Abs(Feet() - Fl) < 1.0;
			const bool bAtHeight = Feet() > Ol.Y - 4.5;
			Check(bOnOwnFloor && bAtHeight, FString::Printf(TEXT("the client joined late beside the host (on %d at %.2f): %s; %.0f cm from the host"), Ps, Ol.Y, *St,
				PartnerPuppet() ? FVector::Dist(PartnerPuppet()->GetActorLocation(), P->GetActorLocation()) : -1.0));
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP jumppick client RESULT %s (%d passed, %d failed)"), ZgFails == 0 && ZgPasses > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), ZgPasses, ZgFails);
		ZgStep = 99;
		return;
	}
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
