// BUG-STUCKGAP's co-op test (user 2026-10-05, the first internet session on the hideout: a player stuck in a gap after a
// climb, then the host stuck in a like space, dead there, the keys gone with him and the level soft-locked).  The rescue
// itself is the player's (OutlawsPlayerUnstuck.cpp, single player too); the keys' rule is the co-op rules' (keys never
// lost: a death where no body can stand leaves them at the last safe floor spot, OnLocalDeath; a player who leaves has
// them put back there, OnPlayerLogout).
// -OLCoopAuto -OLCoopTest=unstuck on both (the hideout): the client holds the brass key; the host's body put behind a wall
// (rescued); the client's likewise; the client put behind a wall again and killed there: where its key lies on the host.
// -OLCoopTest=unstuckleave: the client holds the key and leaves; the host puts it back (OnPlayerLogout logs and ends).
#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GenericPlatform/GenericPlatformMisc.h"

namespace
{
	/** A test aid: the body put just past the nearest of the level's walls whose far side is outside every sector. */
	bool BehindWall(AOutlawsLevelActor* L, AOutlawsPlayer* P, FString& Out)
	{
		UWorld* W = P->GetWorld();
		const FVector Body = P->GetActorLocation();
		const float R = P->GetCapsuleComponent()->GetScaledCapsuleRadius();
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsCoopBehindWall), false, P);
		double Best = 1e9;
		FVector To;
		for (int32 k = 0; k < 16; ++k)
		{
			const FVector D = FRotator(0.0, 22.5 * k, 0.0).Vector();
			FHitResult H;
			if (!W->LineTraceSingleByChannel(H, Body, Body + D * 1500.0, ECC_Visibility, Q) || H.GetActor() != L || H.Distance >= Best) continue;
			const FVector At = Body + D * (H.Distance + R + 40.0);
			if (L->SectorHoldingPoint(At, 2.0) != -1) continue;
			Best = H.Distance;
			To = At;
		}
		if (Best > 1e8) { Out = TEXT("no wall with nothing behind it"); return false; }
		P->TeleportTo(To, P->GetActorRotation(), false, true);
		Out = FString::Printf(TEXT("from %s put %.0f cm away past a wall, to %s (outside every sector)"), *Body.ToCompactString(), FVector::Dist(Body, To), *To.ToCompactString());
		return true;
	}
}

#if WITH_OUTLAWS_TESTS
bool UOutlawsCoop::OnUnstuckTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	FString How;
	if (Step == TEXT("ustuck"))
	{
		const bool bOk = BehindWall(L, P, How);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: unstuck: the client's body %s%s"), bOk ? TEXT("") : TEXT("NOT MOVED: "), *How);
		return true;
	}
	if (Step == TEXT("ustuckdie"))
	{
		// behind a wall, then dead there at once (before any rescue): the keys must not be left there
		const bool bOk = BehindWall(L, P, How);
		const bool bWas = L->bInvincible;
		L->bInvincible = false;
		L->DamagePlayer(1e6f, 1.f, true);
		L->bInvincible = bWas;
		FVector Safe;
		const bool bSafe = P->LastSafeFeet(Safe);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: unstuck: the client dies past a wall (%s%s), its last safe spot %s"), bOk ? TEXT("") : TEXT("NOT MOVED: "), *How,
			bSafe ? *Safe.ToCompactString() : TEXT("NONE"));
		return true;
	}
	if (Step == TEXT("uleave"))
	{
		TArray<FString> Keys;
		KeyItems(L, Keys);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: unstuckleave: the client leaves holding %s"), *FString::Join(Keys, TEXT(", ")));
		EndSession(TEXT("left the game (unstuckleave test)"), false);
		FGenericPlatformMisc::RequestExit(false);
		return true;
	}
	return false;
}

void UOutlawsCoop::TickHostUnstuckTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	if (Now < TestAt) return;
	FString How;
	switch (TestStep)
	{
	case 700:
		// both out of the bad guys' reach; the client holds the brass key; both stand a while (their safe spots known)
		L->bInvincible = true;
		TestSay(TEXT("godmode"), 1);
		TestSay(TEXT("rkey"), 0);
		TestStep = TestOnly == TEXT("unstuckleave") ? 710 : 701;
		TestAt = Now + 4.0;
		return;
	case 701:
	{
		// the host's body past a wall: back on safe ground within about a second (the head outside every sector)
		TestRulesKeys = P->UnstuckCount;
		const bool bOk = BehindWall(L, P, How);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: unstuck: the host's body %s%s"), bOk ? TEXT("") : TEXT("NOT MOVED: "), *How);
		TestStep = 702;
		TestAt = Now + 3.0;
		return;
	}
	case 702:
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: unstuck: the host %s; %s"), P->UnstuckCount > TestRulesKeys ? TEXT("rescued") : TEXT("NOT RESCUED"), *P->LastUnstuck);
		TestSay(TEXT("ustuck"), 0);
		TestStep = 703;
		TestAt = Now + 3.5;
		return;
	case 703:
		TestSay(TEXT("ustuckdie"), 0);
		TestStep = 704;
		TestAt = Now + 4.0;
		return;
	case 704:
	{
		// where the client's key lies here: by its last safe spot (as its stats said), a floor a body can stand on
		const FOutlawsCoopStats* O = OtherStats();
		const FVector Safe = O ? FVector(O->SafeFeet) : FVector::ZeroVector;
		int32 Found = 0;
		for (const AOutlawsLevelActor::FPickup& Pk : L->Pickups)
		{
			const AActor* A = Pk.Actor.Get();
			if (!A || Pk.CoopId < 100000 || !Pk.Ground.Contains(TEXT("key"))) continue;
			++Found;
			FVector OnFloor;
			const bool bStand = P->SafeFeetAt(A->GetActorLocation(), OnFloor) || P->SafeFeetAt(A->GetActorLocation() - FVector(0.0, 0.0, 30.0), OnFloor);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: unstuck: the client's key %s (%d) lies at %s, %.0f cm from the client's last safe spot %s; a body can stand there: %s; the client %s"),
				*Pk.Ground, Pk.CoopId, *A->GetActorLocation().ToCompactString(), FVector::Dist2D(A->GetActorLocation(), Safe), *Safe.ToCompactString(), bStand ? TEXT("yes") : TEXT("NO"),
				bOtherDead ? TEXT("dead") : TEXT("ALIVE"));
		}
		if (Found == 0) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: unstuck: NO KEY on the ground here (the client %s)"), bOtherDead ? TEXT("dead") : TEXT("ALIVE"));
		TestSay(TEXT("quit"));
		TestStep = 100;
		TestAt = Now + 2.0;
		return;
	}
	case 710:
		TestSay(TEXT("uleave"), 0);          // (the host's OnPlayerLogout puts the key back and ends the test)
		TestStep = 711;
		TestAt = Now + 20.0;
		return;
	case 711:
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP test: unstuckleave: the client did NOT leave"));
		TestStep = 100;
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
