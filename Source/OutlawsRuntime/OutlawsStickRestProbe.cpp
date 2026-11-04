// REQ-STICKREST's probe (user 4 Nov: an unlit thrown stick that comes to rest is a pickup in story and co-op too, as in matches).
// Story: -OLShots -OLNoFrontEnd -OLLevel=hideout -OLStickRestProbe [-OLOldStickRest].  A stick let go unlit comes to rest: a pickup,
// not taken by the player it lies under; saved (slot 8) and loaded: that stick once, the level's own as they were, no stick
// given by the load; walked onto: one more, once; saved and loaded again: gone, the count kept.  -OLOldStickRest: it lies there, no pickup.
// -OLStickRestProbe=oldsave -OLShots -OLLevel=ranch -OLNoFrontEnd: saves from before this change load, the level's own sticks as built.
// Co-op pair: host -OLCoopHost -OLCoopLevel=hideout -OLStickRestProbe=coop, client -OLCoopJoin=<host>:<port> -OLStickRestProbe=coop.
// The host's stick at rest is numbered and made on the client (and in the host's save for a late joiner), the client takes it (one
// more there, none on the host, gone on both); then the client's own stick at rest, made on the host, taken by the host.
// Lines "OLSHOT stickrest check PASS|FAIL (story|host|client): ..." and "OLSHOT stickrest <story|host|client> RESULT".

#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsShotTests.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsStickRestProbeLocal
{
	const TCHAR* const Stick = TEXT("idynam");
	constexpr int32 Slot = 8;
	int32 Passes = 0, Fails = 0, Base = -1, CountWas = 0, Step = 0, Taken = 0;
	FVector Rest = FVector::ZeroVector, Home = FVector::ZeroVector;
	double StepAt = 0.0;

	void Check(bool bOk, const TCHAR* Who, const FString& What)
	{
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT stickrest check %s (%s): %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), Who, *What);
		if (bOk) ++Passes; else ++Fails;
	}
	void Result(const TCHAR* Who)
	{
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT stickrest %s RESULT %s (%d passed, %d failed)"), Who, Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
	}
	int32 StickPickups(const AOutlawsLevelActor* L, FVector* Near = nullptr, int32* NearN = nullptr)
	{
		TArray<AOutlawsLevelActor::FPickupDebug> All;
		L->DebugPickups(All);
		int32 N = 0;
		for (const AOutlawsLevelActor::FPickupDebug& Pk : All)
		{
			if (!Pk.Actor || !AOutlawsLevelActor::IsStickGround(Pk.Ground)) continue;
			++N;
			if (Near && FVector::Dist2D(Pk.Actor->GetActorLocation(), Rest) < 40.0)
			{
				*Near = Pk.Actor->GetActorLocation();
				if (NearN) ++*NearN;
			}
		}
		return N;
	}
	/** The stick that came to rest: the last in the list. */
	FVector LastStick(const AOutlawsLevelActor* L)
	{
		TArray<AOutlawsLevelActor::FPickupDebug> All;
		L->DebugPickups(All);
		FVector At = FVector::ZeroVector;
		for (const AOutlawsLevelActor::FPickupDebug& Pk : All) if (Pk.Actor && AOutlawsLevelActor::IsStickGround(Pk.Ground)) At = Pk.Actor->GetActorLocation();
		return At;
	}
	void StandOn(AOutlawsPlayer* P, const FVector& At)
	{
		P->SetActorLocation(At + FVector(0.0, 0.0, P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 3.0), false, nullptr, ETeleportType::TeleportPhysics);
	}
}

bool AOutlawsPlayer::DropUnlitStickTest()
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L) return false;
	L->GiveAllWeapons();
	L->TakeCount(OutlawsStickRestProbeLocal::Stick, FMath::Max(0, L->GetCount(OutlawsStickRestProbeLocal::Stick) - 5));
	Equip(1, 6);
	Mag[1] = 1;
	const int32 Before = Thrown.Num();
	ThrowHeld(1, true, true);
	return Thrown.Num() > Before;
}

// ---- a story level ----------------------------------------------------------------------------------------------------------

OL_SHOT_MATCH(OLStickRestProbe) { return OLFlags::Param(TEXT("OLStickRestProbe")) || OLFlags::ValueIs(TEXT("OLStickRestProbe"), TEXT("oldsave")); }

OL_SHOT_TEST(OLStickRestProbe, 2206)
{
	using namespace OutlawsStickRestProbeLocal;
	if (!L || ShotStep < 0) return;
	const bool bOld = OLFlags::Param(TEXT("OLOldStickRest"));
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	auto Mine = [](bool bOk, const FString& What) { Check(bOk, TEXT("story"), What); };
	auto Done = [&]()
	{
		Result(TEXT("story"));
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	static float Wait = 0.f;
	if (OLFlags::ValueIs(TEXT("OLStickRestProbe"), TEXT("oldsave")))
	{
		// -OLStickRestProbe=oldsave -OLLevel=ranch: slot 8 a real save written before this change (no "play_stick" in it), slot 9 the
		// same with one of the level's own sticks listed as dropped, as a build before 10 Oct wrote them (the run copies both in).
		// Both load, the level's own sticks as built, none twice; one taken gives +1 once, and this build's save of that holds
		if (ShotStep == 0)
		{
			if (ShotTime < 4.f || !bUp) return;
			L->bInvincible = true;
			Base = StickPickups(L);
			Mine(L->LoadGame(Slot), FString::Printf(TEXT("%s: a save from before this change loads (the level's own sticks: %d)"), *L->LevelName, Base));
			Wait = ShotTime + 3.f;
			ShotStep = 1;
			return;
		}
		if (ShotTime < Wait || !bUp) return;
		L->bInvincible = true;
		if (ShotStep == 1)
		{
			CountWas = L->GetCount(Stick);
			Mine(StickPickups(L) == Base, FString::Printf(TEXT("loaded: the level's own sticks as built, %d of %d, the count %d"), StickPickups(L), Base, CountWas));
			StandOn(this, LastStick(L));
			Wait = ShotTime + 2.5f;
			ShotStep = 2;
		}
		else if (ShotStep == 2)
		{
			Taken = Base - StickPickups(L);             // (ranch's last ones lie together: a walk takes them all)
			Mine(Taken >= 1 && L->GetCount(Stick) == CountWas + Taken, FString::Printf(TEXT("walked onto: %d -> %d, %d sticks left: +1 each"), CountWas, L->GetCount(Stick), StickPickups(L)));
			Mine(L->SaveGame(Slot) && L->LoadGame(Slot), TEXT("saved again by this build and loaded"));
			Wait = ShotTime + 3.f;
			ShotStep = 3;
		}
		else if (ShotStep == 3)
		{
			Mine(L->GetCount(Stick) == CountWas + Taken && StickPickups(L) == Base - Taken, FString::Printf(TEXT("loaded: those still gone, %d sticks, the count %d, +1 each once"), StickPickups(L), L->GetCount(Stick)));
			Mine(L->LoadGame(Slot + 1), TEXT("a save listing a level's own stick as dropped loads"));
			Wait = ShotTime + 3.f;
			ShotStep = 4;
		}
		else
		{
			Mine(StickPickups(L) == Base && L->GetCount(Stick) == CountWas, FString::Printf(TEXT("loaded: left to the build, %d of %d sticks, none made twice, the count %d"), StickPickups(L), Base, L->GetCount(Stick)));
			Done();
		}
		return;
	}
	if (ShotStep == 0)
	{
		if (ShotTime < 4.f || !bUp) return;
		L->bInvincible = true;
		Base = StickPickups(L);
		Home = GetActorLocation();
		Mine(DropUnlitStickTest(), FString::Printf(TEXT("%s: a stick let go unlit (the level's own sticks: %d)"), *L->LevelName, Base));
		CountWas = L->GetCount(Stick);
		Wait = ShotTime + 10.f;
		ShotStep = 1;
		return;
	}
	if (ShotStep == 1)
	{
		// at rest: a pickup, under the player that let it go and not taken
		if (StickPickups(L) == Base && ShotTime < Wait) return;
		if (bOld)
		{
			Mine(StickPickups(L) == Base, FString::Printf(TEXT("-OLOldStickRest: at rest, no pickup (%d listed)"), StickPickups(L)));
			Done();
			return;
		}
		Mine(StickPickups(L) == Base + 1, FString::Printf(TEXT("at rest, a pickup: %d listed, the level's %d"), StickPickups(L), Base));
		Rest = LastStick(L);
		Mine(L->GetCount(Stick) == CountWas, FString::Printf(TEXT("not taken by the player it lies by: %d"), L->GetCount(Stick)));
		StandOn(this, Rest + FVector(250.0, 0.0, 40.0));         // (off it, so a walk-over can take it after the load)
		Wait = ShotTime + 1.5f;
		ShotStep = 2;
		return;
	}
	if (ShotStep == 2)
	{
		if (ShotTime < Wait) return;
		Mine(L->GetCount(Stick) == CountWas, FString::Printf(TEXT("stood off it: %d"), L->GetCount(Stick)));
		Mine(L->SaveGame(Slot) && L->LoadGame(Slot), FString::Printf(TEXT("saved with the stick at rest and loaded, slot %d"), Slot));
		Wait = ShotTime + 3.f;
		ShotStep = 3;
		return;
	}
	if (ShotStep == 3)
	{
		if (ShotTime < Wait || !bUp) return;
		FVector Near = FVector::ZeroVector;
		int32 NearN = 0;
		const int32 N = StickPickups(L, &Near, &NearN);
		Mine(N == Base + 1 && NearN == 1, FString::Printf(TEXT("loaded: the stick at rest once (%d there), the level's own %d of %d"), NearN, N - NearN, Base));
		Mine(L->GetCount(Stick) == CountWas, FString::Printf(TEXT("loaded: the count as saved, %d"), L->GetCount(Stick)));
		StandOn(this, Near.IsZero() ? Rest : Near);
		Wait = ShotTime + 2.5f;
		ShotStep = 4;
		return;
	}
	if (ShotStep == 4)
	{
		if (ShotTime < Wait) return;
		Mine(L->GetCount(Stick) == CountWas + 1 && StickPickups(L) == Base, FString::Printf(TEXT("walked onto: %d -> %d, %d sticks left"), CountWas, L->GetCount(Stick), StickPickups(L)));
		StandOn(this, Rest + FVector(250.0, 0.0, 40.0));
		Wait = ShotTime + 1.f;
		ShotStep = 5;
		return;
	}
	if (ShotStep == 5)
	{
		if (ShotTime < Wait) return;
		Mine(L->SaveGame(Slot) && L->LoadGame(Slot), TEXT("saved with it taken and loaded"));
		Wait = ShotTime + 3.f;
		ShotStep = 6;
		return;
	}
	if (ShotTime < Wait || !bUp) return;
	Mine(StickPickups(L) == Base && L->GetCount(Stick) == CountWas + 1, FString::Printf(TEXT("loaded: gone, %d sticks, the count %d, +1 once"), StickPickups(L), L->GetCount(Stick)));
	Done();
}

// ---- a co-op pair (no talk between them: each goes by what it sees) -----------------------------------------------------------

void UOutlawsCoop::TickStickRestTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	using namespace OutlawsStickRestProbeLocal;
	const TCHAR* Who = IsHost() ? TEXT("host") : TEXT("client");
	auto Mine = [Who](bool bOk, const FString& What) { Check(bOk, Who, What); };
	L->bInvincible = true;
	if (Base < 0)
	{
		const AOutlawsCoopLink* K = IsHost() ? GetLink(1) : nullptr;
		if (IsHost() && (!K || K->BuiltSerial != StartedSerial)) return;
		Base = StickPickups(L);
		Home = P->GetActorLocation();
		StepAt = Now + (IsHost() ? 5.0 : 60.0);
		Step = IsHost() ? 1 : 10;
		return;
	}
	auto Finish = [&]()
	{
		Result(Who);
		Step = 90;
		StepAt = Now + (IsHost() ? 2.0 : 8.0);       // (the session ends with the client: the host has its last check still to make)
	};
	switch (Step)
	{
	case 1:          // the host lets its stick go
		if (Now < StepAt) return;
		Mine(!IsMatch() && P->DropUnlitStickTest(), FString::Printf(TEXT("%s, co-op: a stick let go unlit (the level's own: %d)"), *L->LevelName, Base));
		CountWas = L->GetCount(Stick);
		StepAt = Now + 10.0;
		Step = 2;
		return;
	case 2:
	{
		if (StickPickups(L) == Base && Now < StepAt) return;
		Rest = LastStick(L);
		int32 Id = -1;
		for (const AOutlawsLevelActor::FPickup& Pk : L->Pickups) if (Pk.bPlayStick) Id = Pk.CoopId;
		Mine(StickPickups(L) == Base + 1 && Id >= 100000, FString::Printf(TEXT("at rest, a pickup, number %d"), Id));
		FString Text;
		int32 Bad = 0, Pk = 0;
		Mine(L->BuildSaveText(P, Text, Bad, Pk) && Text.Contains(TEXT("play_stick")), TEXT("in the host's save, for a late joiner"));
		StandOn(P, Rest + FVector(250.0, 0.0, 40.0));
		StepAt = Now + 40.0;
		Step = 3;
		return;
	}
	case 3:          // the client takes it
		if (StickPickups(L) != Base && Now < StepAt) return;
		Mine(StickPickups(L) == Base && L->GetCount(Stick) == CountWas, FString::Printf(TEXT("taken by the client: gone here, %d here as it was"), L->GetCount(Stick)));
		StepAt = Now + 40.0;
		Step = 4;
		return;
	case 4:          // the client's own at rest, made here
		if (StickPickups(L) == Base && Now < StepAt) return;
		Mine(StickPickups(L) == Base + 1, FString::Printf(TEXT("the client's stick at rest is a pickup here: %d listed"), StickPickups(L)));
		StandOn(P, LastStick(L));
		StepAt = Now + 3.0;
		Step = 5;
		return;
	case 5:
		if (Now < StepAt) return;
		Mine(L->GetCount(Stick) == CountWas + 1 && StickPickups(L) == Base, FString::Printf(TEXT("walked onto: %d -> %d, %d sticks left"), CountWas, L->GetCount(Stick), StickPickups(L)));
		Finish();
		return;
	case 10:         // the client: the host's stick comes
		if (StickPickups(L) == Base && Now < StepAt) return;
		Mine(StickPickups(L) == Base + 1, FString::Printf(TEXT("the host's stick at rest is a pickup here: %d listed, the level's %d"), StickPickups(L), Base));
		CountWas = L->GetCount(Stick);
		StandOn(P, LastStick(L));
		StepAt = Now + 3.0;
		Step = 11;
		return;
	case 11:
		if (Now < StepAt) return;
		Mine(L->GetCount(Stick) == CountWas + 1 && StickPickups(L) == Base, FString::Printf(TEXT("walked onto: %d -> %d, %d sticks left"), CountWas, L->GetCount(Stick), StickPickups(L)));
		StandOn(P, Home);
		Mine(P->DropUnlitStickTest(), TEXT("a stick of its own let go unlit"));
		StepAt = Now + 10.0;
		Step = 12;
		return;
	case 12:
		if (StickPickups(L) == Base && Now < StepAt) return;
		Mine(StickPickups(L) == Base + 1, FString::Printf(TEXT("at rest, a pickup: %d listed"), StickPickups(L)));
		StandOn(P, Home + FVector(0.0, 0.0, 10.0));
		StepAt = Now + 40.0;
		Step = 13;
		return;
	case 13:         // the host takes it
		if (StickPickups(L) != Base && Now < StepAt) return;
		Mine(StickPickups(L) == Base, TEXT("taken by the host: gone here"));
		Finish();
		return;
	case 90:
		if (Now < StepAt) return;
		Step = 91;
		FPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
