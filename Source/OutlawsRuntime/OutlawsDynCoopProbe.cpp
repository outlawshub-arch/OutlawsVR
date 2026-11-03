// BUG-DYNTHROW phase 2 (user 3 Nov: in co-op the client heard a bad guy's throw and its blast and never saw the stick or the
// knife): -OLCoopAuto -OLCoopTest=dyncoop -OLCoopLevel=<shgulch|cliff> [-OLOldDynCoop] on the host and its client
// (run_coop.ps1).  shgulch: the host puts the client 9 m from Dick Farmer (SH_DICK), in his sight with a lob that reaches, and
// itself out of his sight far off; Dick wakes and throws at the client.  The client: a copy of each stick, drawn, its place
// 0.35 s out within 40 cm of the host's stick's then, put out by the host's blast within 60 cm of where it lay; one blast, one
// blast sound, one throw sound and one fuse a stick; nothing of its own goes off.  cliff: Two Feathers' knife let go onto the
// floor as ThrowAtPlayer lets one go: its copy's place 0.12 s out within 40 cm of the host's, it comes down where the host's
// did, the host's gknife is made on the client by its own word and the client takes it (gone on the host, a knife more on the
// client).  -OLOldDynCoop: no copy on the client, the blast and its sound once, as before.  Lines "OLSHOT dyncoop check
// PASS|FAIL (host|client): ..." and "OLSHOT dyncoop host|client RESULT".

#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsEnemy.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsDynCoopProbeLocal
{
	int32 Step = 0, Passes = 0, Fails = 0, Thrower = -1, ThrowsWas = 0, BlastsWas = 0, KnivesWas = 0, KnifeId = -1, SpotSet = 0, TheirsSet = 0;
	double At = 0.0, Deadline = 0.0;
	TWeakObjectPtr<AOutlawsEnemy> T;
	FVector Spot = FVector::ZeroVector;            // (the client's place, as the host chose it: the floor + 100 cm)
	bool bKnife = false, bMine = false, bSampleChecked = false;
	// the first stick's or knife's place this many seconds after it left the hand: this machine's own, and the host's as told
	constexpr float StickAge = 0.35f, KnifeAge = 0.12f;
	float PrevAge = -1.f;
	FVector PrevPos = FVector::ZeroVector, Mine = FVector::ZeroVector, Theirs = FVector::ZeroVector;
	TArray<FString> Heard;                         // (the client's ear on PlayWav: AOutlawsLevelActor::WavSpy)

	FVector FeetOf(const AOutlawsEnemy* E) { return E->GetActorLocation() - FVector(0.0, 0.0, E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()); }

	/** This level's first bad guy's stick or knife at Age seconds from the hand, between the two frames round it. */
	bool Sample(const AOutlawsLevelActor* L, float Age, FVector& Out)
	{
		FVector Pos;
		float A = 0.f;
		int32 Kind = 0;
		bool bRest = false, bDrawn = false;
		if (!L->DebugEnemyThrown(Pos, A, Kind, bRest, bDrawn)) { PrevAge = -1.f; return false; }
		bool bGot = false;
		if (PrevAge >= 0.f && PrevAge < Age && A >= Age)
		{
			Out = FMath::Lerp(PrevPos, Pos, double((Age - PrevAge) / FMath::Max(1.0e-4f, A - PrevAge)));
			bGot = true;
		}
		PrevAge = A;
		PrevPos = Pos;
		return bGot;
	}

	/** A place for a player Radius cm (flat) from E, on a floor within 3 m of his (bSameFloor), his eye's line to it clear (bClear)
	 *  or blocked; bSolve: a throw of his reaches it.  The player's middle there (the floor + 100 cm), or zero. */
	FVector FindSpot(UWorld* W, const AOutlawsEnemy* E, const AActor* Ignore, std::initializer_list<double> Radii, bool bClear, bool bSameFloor, bool bSolve)
	{
		const FVector Home = FeetOf(E), Eye = E->GetActorLocation() + FVector(0.0, 0.0, E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() * 0.8);
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsDynCoopProbe), false, Ignore);
		Q.AddIgnoredActor(E);
		for (const double R : Radii)
		{
			for (int32 k = 0; k < 32; ++k)
			{
				const double A = 2.0 * PI * k / 32.0;
				const FVector P = Home + FVector(FMath::Cos(A) * R, FMath::Sin(A) * R, 150.0);
				FHitResult H;
				if (!W->LineTraceSingleByChannel(H, P, P - FVector(0.0, 0.0, 450.0), ECC_Visibility, Q) || H.ImpactNormal.Z < 0.7) continue;
				const FVector Floor = H.ImpactPoint;
				if (bSameFloor && FMath::Abs(Floor.Z - Home.Z) > 300.0) continue;
				const FVector Mid = Floor + FVector(0.0, 0.0, 100.0);
				FHitResult Up;
				if (W->LineTraceSingleByChannel(Up, Floor + FVector(0.0, 0.0, 5.0), Floor + FVector(0.0, 0.0, 200.0), ECC_Visibility, Q)) continue;     // (room to stand)
				const bool bBlocked = W->LineTraceSingleByChannel(H, Eye, Mid + FVector(0.0, 0.0, 50.0), ECC_Visibility, Q)
					|| W->LineTraceSingleByChannel(H, Eye, Floor + FVector(0.0, 0.0, 20.0), ECC_Visibility, Q);
				if (bClear == bBlocked) continue;
				FVector Hand, Vel, Mark;
				double Tm = 0.0;
				if (bSolve && !E->SolveThrow(Floor, 175.f, 10, false, Hand, Vel, Tm, Mark)) continue;
				return Mid;
			}
		}
		return FVector::ZeroVector;
	}
}

void UOutlawsCoop::TickDynCoopTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	using namespace OutlawsDynCoopProbeLocal;
	const bool bHost = IsHost();
	const bool bOld = AOutlawsEnemy::OldDynCoop();
	const FString Lv = L->LevelName.ToLower();
	auto Check = [bHost](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dyncoop check %s (%s): %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), bHost ? TEXT("host") : TEXT("client"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	UWorld* W = L->GetWorld();
	if (!W) return;
	L->bInvincible = true;
	// (both machines) the first stick's or knife's place at the sample's age, this machine's own; the host's told to the client
	if (Step >= 3 && !bMine && Sample(L, bKnife ? KnifeAge : StickAge, Mine))
	{
		bMine = true;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dyncoop %s: the %s %.2f s from the hand at %s"), bHost ? TEXT("host") : TEXT("client"), bKnife ? TEXT("knife") : TEXT("stick"),
			bKnife ? KnifeAge : StickAge, *Mine.ToCompactString());
		if (bHost)
		{
			TestSay(TEXT("dynsx"), FMath::RoundToInt(Mine.X));
			TestSay(TEXT("dynsy"), FMath::RoundToInt(Mine.Y));
			TestSay(TEXT("dynsz"), FMath::RoundToInt(Mine.Z));
		}
	}
	if (!bHost && bMine && TheirsSet == 7 && !bSampleChecked)
	{
		bSampleChecked = true;
		const double D = FVector::Dist(Mine, Theirs);
		Check(D <= 40.0, FString::Printf(TEXT("the copy's %s %.2f s from the hand is %.0f cm from the host's then (here %s, the host's %s)"), bKnife ? TEXT("knife") : TEXT("stick"),
			bKnife ? KnifeAge : StickAge, D, *Mine.ToCompactString(), *Theirs.ToCompactString()));
	}
	AOutlawsEnemy* E = T.Get();
	switch (Step)
	{
	case 0:
		if ((Lv != TEXT("shgulch") && Lv != TEXT("cliff")) || !L->HasActorBegunPlay() || !L->AreEnemiesActive()) return;
		if (bHost)
		{
			const AOutlawsCoopLink* CL = GetLink(PartnerIndex());
			if (!CL || CL->BuiltSerial != StartedSerial || !PartnerPuppet()) return;
		}
		if (At == 0.0) { At = Now + 4.0; return; }
		if (Now < At) return;
		bKnife = Lv == TEXT("cliff");
		BlastsWas = L->BlastsSeen;
		if (!bHost)
		{
			AOutlawsLevelActor::WavSpy = &Heard;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dyncoop client: on %s, listening%s"), *Lv, bOld ? TEXT(" (-OLOldDynCoop)") : TEXT(""));
			Step = 20;
			return;
		}
		Step = 1;
		return;
	// ---- the host ----
	case 1:
	{
		const TArray<TWeakObjectPtr<AOutlawsEnemy>>& List = L->GetEnemies();
		for (int32 i = 0; i < List.Num() && Thrower < 0; ++i)
		{
			const AOutlawsEnemy* C = List[i].Get();
			if (C && !C->IsDead() && C->IsThrower() && (bKnife ? C->Def.ThrowItem == TEXT("pknife") : C->GetOutlawsName().StartsWith(TEXT("SH_DICK"), ESearchCase::IgnoreCase))) Thrower = i;
		}
		E = List.IsValidIndex(Thrower) ? List[Thrower].Get() : nullptr;
		Check(E != nullptr, FString::Printf(TEXT("%s: the thrower (%s) is in the level"), *Lv, bKnife ? TEXT("pknife") : TEXT("SH_DICK")));
		if (!E) { Step = 8; return; }
		T = E;
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : List) if (Wk.IsValid() && Wk.Get() != E && !Wk->IsDead() && !Wk->IsDormant()) Wk->SetDormant(true);
		if (E->IsDormant()) E->Appear(FeetOf(E));
		Spot = bKnife ? FindSpot(W, E, P, { 600.0, 500.0, 700.0, 400.0 }, true, true, false) : FindSpot(W, E, P, { 900.0, 800.0, 1000.0, 700.0 }, true, true, true);
		Check(!Spot.IsZero(), FString::Printf(TEXT("%s: a place for the client by %s (enemy %d at %s): %s"), *Lv, *E->GetOutlawsName(), Thrower, *E->GetActorLocation().ToCompactString(),
			Spot.IsZero() ? TEXT("none") : *Spot.ToCompactString()));
		if (Spot.IsZero()) { Step = 8; return; }
		// the host itself out of his sight, far off, so he goes for the client
		const FVector Away = FindSpot(W, E, P, { 2500.0, 2000.0, 1800.0, 1500.0, 3000.0 }, false, false, false);
		if (!Away.IsZero()) P->TeleportTo(Away, FRotator::ZeroRotator, false, true);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dyncoop host: %s is enemy %d; the client to %s, the host to %s"), *E->GetOutlawsName(), Thrower, *Spot.ToCompactString(),
			Away.IsZero() ? TEXT("(stays: no place out of his sight)") : *Away.ToCompactString());
		TestSay(TEXT("dynx"), FMath::RoundToInt(Spot.X));
		TestSay(TEXT("dyny"), FMath::RoundToInt(Spot.Y));
		TestSay(TEXT("dynz"), FMath::RoundToInt(Spot.Z));
		TestSay(TEXT("dyngo"), Thrower);
		Step = 2;
		Deadline = Now + 15.0;
		return;
	}
	case 2:
	{
		const AOutlawsCoopLink* CL = GetLink(PartnerIndex());
		if (!E || (!CL || FVector::Dist2D(FVector(CL->Pose.Body), Spot) > 150.0))
		{
			if (Now > Deadline) { Check(false, TEXT("the client came to its place")); Step = 8; }
			return;
		}
		ThrowsWas = E->ThrowsTotal;
		BlastsWas = L->EnemyBlasts;
		KnivesWas = L->EnemyKnivesLeft;
		if (!bKnife)
		{
			E->Wake(false);
			E->CoopTarget = PartnerIndex();
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dyncoop host: the client is in its place; %s woken, after the client"), *E->GetOutlawsName());
			Deadline = Now + 45.0;
		}
		else
		{
			// his knife let go onto the floor between him and the client, a step aside of the client's line, as ThrowAtPlayer lets
			// one go (his throw here, its copy told to the clients)
			const FVector Feet = FeetOf(E), SpotFeet = Spot - FVector(0.0, 0.0, 100.0);
			const FVector Toward = (SpotFeet - Feet).GetSafeNormal2D();
			const FVector Side(-Toward.Y, Toward.X, 0.0);
			const FVector Hand = Feet + FVector(0.0, 0.0, E->HeightCm() * 0.8) + Toward * (E->GetCapsuleComponent()->GetScaledCapsuleRadius() + 20.0);
			FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsDynCoopProbe), false, E);
			Q.AddIgnoredActor(P);
			FVector Mark = FVector::ZeroVector;
			for (const double S : { 150.0, -150.0, 100.0, -100.0 })
			{
				const FVector Over = Feet + (SpotFeet - Feet) * 0.45 + Side * S + FVector(0.0, 0.0, 150.0);
				FHitResult H;
				if (!W->LineTraceSingleByChannel(H, Over, Over - FVector(0.0, 0.0, 450.0), ECC_Visibility, Q)) continue;
				FHitResult Clear;
				if (W->LineTraceSingleByChannel(Clear, Hand, FVector(H.ImpactPoint) + FVector(0.0, 0.0, 20.0), ECC_Visibility, Q)) continue;
				Mark = H.ImpactPoint;
				break;
			}
			Check(!Mark.IsZero(), FString::Printf(TEXT("cliff: a floor mark for his knife between him and the client: %s"), Mark.IsZero() ? TEXT("none") : *Mark.ToCompactString()));
			if (Mark.IsZero()) { Step = 8; return; }
			const FVector Vel = (Mark - Hand).GetSafeNormal() * 1500.0;
			L->AddEnemyThrown(E, 2, Hand, Vel, 3.f, E->Def.Damage);
			if (!bOld) UOutlawsCoop::NoteEnemyThrow(E, 2, Hand, Vel, 3.f);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dyncoop host: %s's knife let go from %s at the floor by %s"), *E->GetOutlawsName(), *Hand.ToCompactString(), *Mark.ToCompactString());
			Deadline = Now + 8.0;
		}
		Step = 3;
		return;
	}
	case 3:
		if (!E) { Check(false, TEXT("the thrower is gone")); Step = 8; return; }
		if (!bKnife)
		{
			// the first stick's blast; then no more throws (he sleeps), and the ones in the air gone off
			if (L->EnemyBlasts == BlastsWas)
			{
				if (Now > Deadline) { Check(false, FString::Printf(TEXT("shgulch: %s threw and his stick went off in 45 s (%d throws)"), *E->GetOutlawsName(), E->ThrowsTotal - ThrowsWas)); Step = 8; }
				return;
			}
			if (!E->IsDormant()) E->SetDormant(true);
			if (L->EnemyThrownCount() > 0 && Now < Deadline) return;
			const int32 N = E->ThrowsTotal - ThrowsWas, Went = L->EnemyBlasts - BlastsWas;
			Check(N >= 1 && Went == N, FString::Printf(TEXT("shgulch: %s threw %d sticks at the client, %d went off on the host (the last at %s, %.2f s from the hand)"), *E->GetOutlawsName(), N, Went,
				*L->LastEnemyBlastAt.ToCompactString(), L->LastEnemyFuse));
			TestSay(TEXT("dyncheck"), N);
			Step = 4;
			At = Now + 3.0;
			return;
		}
		if (L->EnemyKnivesLeft == KnivesWas)
		{
			if (Now > Deadline) { Check(false, TEXT("cliff: the knife came down and left a gknife on the host")); Step = 8; }
			return;
		}
		{
			AActor* Drop = L->LastEnemyKnifeDrop.Get();
			const int32 i = PickupIndexOf(L, Drop);
			KnifeId = i >= 0 ? L->Pickups[i].CoopId : -1;
			Check(KnifeId >= 0, FString::Printf(TEXT("cliff: the knife that came down left one gknife on the host, ground item %d at %s"), KnifeId, Drop ? *Drop->GetActorLocation().ToCompactString() : TEXT("-")));
			if (KnifeId < 0) { Step = 8; return; }
			TestSay(TEXT("dyntake"), KnifeId);
			Step = 5;
			At = Now + 5.0;
		}
		return;
	case 4:
		if (Now < At) return;
		Step = 8;
		return;
	case 5:
	{
		if (Now < At) return;
		const int32 i = PickupIndex(L, KnifeId);
		Check(i < 0 || !L->Pickups[i].Actor.IsValid() || L->Pickups[i].Actor->IsHidden(), FString::Printf(TEXT("cliff: the gknife (ground item %d) taken by the client is gone here"), KnifeId));
		Step = 8;
		return;
	}
	case 8:
		TestSay(TEXT("dynend"), 0);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dyncoop host RESULT %s (%d passed, %d failed)%s"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails, bOld ? TEXT(" -OLOldDynCoop") : TEXT(""));
		At = Now + 3.0;
		Step = 9;
		return;
	case 9:
		if (Now < At) return;
		TestSay(TEXT("quit"), 0);
		At = Now + 2.0;
		Step = 10;
		return;
	case 10:
		if (Now < At) return;
		Summary(TEXT("done"));
		QuitForTest();
		Step = 11;
		return;
	// ---- the client: its steps come from the host (OnDynCoopTestStep) ----
	case 25:
		if (Now < At) return;
		Check(L->GetCount(TEXT("iknife")) == KnivesWas + 1, FString::Printf(TEXT("cliff: the client walked over the gknife and the host let it have it: knives %d -> %d"), KnivesWas, L->GetCount(TEXT("iknife"))));
		Step = 20;
		return;
	default:
		return;
	}
}

void UOutlawsCoop::OnDynCoopTestStep(const FString& Step, int32 Arg)
{
	using namespace OutlawsDynCoopProbeLocal;
	AOutlawsLevelActor* L = FindLevel();
	AOutlawsPlayer* P = LocalPlayer();
	if (!L || !P) return;
	const bool bOld = AOutlawsEnemy::OldDynCoop();
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dyncoop check %s (client): %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const double Now = FPlatformTime::Seconds();
	auto Count = [](const FString& Wav) { int32 N = 0; for (const FString& H : Heard) N += H == Wav.ToLower() ? 1 : 0; return N; };
	if (Step == TEXT("dynx")) { Spot.X = Arg; SpotSet |= 1; }
	else if (Step == TEXT("dyny")) { Spot.Y = Arg; SpotSet |= 2; }
	else if (Step == TEXT("dynz")) { Spot.Z = Arg; SpotSet |= 4; }
	else if (Step == TEXT("dynsx")) { Theirs.X = Arg; TheirsSet |= 1; }
	else if (Step == TEXT("dynsy")) { Theirs.Y = Arg; TheirsSet |= 2; }
	else if (Step == TEXT("dynsz")) { Theirs.Z = Arg; TheirsSet |= 4; }
	else if (Step == TEXT("dyngo"))
	{
		// to the host's place for this player, facing the thrower's copy
		Thrower = Arg;
		const TArray<TWeakObjectPtr<AOutlawsEnemy>>& List = L->GetEnemies();
		T = List.IsValidIndex(Arg) ? List[Arg].Get() : nullptr;
		const AOutlawsEnemy* E = T.Get();
		const float Yaw = E ? float((E->GetActorLocation() - Spot).Rotation().Yaw) : 0.f;
		P->TeleportTo(Spot, FRotator(0.0, Yaw, 0.0), false, true);
		if (AController* Ctl = P->GetController()) Ctl->SetControlRotation(FRotator(0.0, Yaw, 0.0));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dyncoop client: to %s (%s), facing %s"), *Spot.ToCompactString(), SpotSet == 7 ? TEXT("told") : TEXT("not all told"), E ? *E->GetOutlawsName() : TEXT("no one"));
	}
	else if (Step == TEXT("dyncheck"))
	{
		// shgulch: Arg sticks thrown at this player on the host, each gone off there
		const int32 N = Arg;
		const AOutlawsEnemy* E = T.Get();
		const FString ThrowWav = E ? E->Def.ShootSound : FString();
		const int32 Blasts = L->BlastsSeen - BlastsWas, Booms = Count(L->GetBlastSound()), Fuses = Count(TEXT("fuse")), Throws = ThrowWav.IsEmpty() ? -1 : Count(ThrowWav);
		if (!bOld)
		{
			Check(L->EnemyCopiesMade == N, FString::Printf(TEXT("shgulch: a copy of each stick here: %d of %d (the last let go at %s)"), L->EnemyCopiesMade, N, *L->LastEnemyCopyFrom.ToCompactString()));
			Check(L->EnemyCopiesPutOut == N && L->LastEnemyCopyGap >= 0.f && L->LastEnemyCopyGap <= 60.f, FString::Printf(TEXT("shgulch: each copy put out by the host's blast where it lay: %d of %d, the last %.0f cm from the blast (at %s)"),
				L->EnemyCopiesPutOut, N, L->LastEnemyCopyGap, *L->LastEnemyCopyAt.ToCompactString()));
			Check(Fuses == N, FString::Printf(TEXT("shgulch: one fuse heard a stick: %d for %d"), Fuses, N));
			Check(bSampleChecked, TEXT("shgulch: the copy's flight compared with the host's"));
		}
		else
		{
			Check(L->EnemyCopiesMade == 0 && Fuses == 0, FString::Printf(TEXT("shgulch -OLOldDynCoop: no stick here, as before: %d copies, %d fuses"), L->EnemyCopiesMade, Fuses));
		}
		Check(Blasts == N && Booms == N, FString::Printf(TEXT("shgulch: one blast and one blast sound a stick: %d blasts, %d %s sounds for %d sticks"), Blasts, Booms, *L->GetBlastSound(), N));
		Check(Throws == N, FString::Printf(TEXT("shgulch: one throw sound a stick: %d %s for %d"), Throws, *ThrowWav, N));
		Check(L->EnemyBlasts == 0 && L->EnemyKnifeHits == 0, FString::Printf(TEXT("shgulch: nothing of the client's own went off or struck: %d blasts, %d strikes"), L->EnemyBlasts, L->EnemyKnifeHits));
	}
	else if (Step == TEXT("dyntake"))
	{
		// cliff: the host's gknife (ground item Arg) made here by its word, where the copy came down; this player takes it
		const int32 i = PickupIndex(L, Arg);
		const AActor* A = i >= 0 ? L->Pickups[i].Actor.Get() : nullptr;
		const double Gap = A ? FVector::Dist2D(A->GetActorLocation(), L->LastEnemyCopyAt) : -1.0;
		if (!bOld)
		{
			Check(L->EnemyCopiesMade == 1 && L->EnemyCopiesSpent == 1, FString::Printf(TEXT("cliff: the knife's copy flew and came down here: %d made, %d down at %s"), L->EnemyCopiesMade, L->EnemyCopiesSpent,
				*L->LastEnemyCopyAt.ToCompactString()));
			Check(bSampleChecked, TEXT("cliff: the copy's flight compared with the host's"));
			Check(A && Gap >= 0.0 && Gap <= 80.0, FString::Printf(TEXT("cliff: the host's gknife (ground item %d) is here %.0f cm from where the copy came down"), Arg, Gap));
		}
		Check(A && L->Pickups[i].Ground.Equals(TEXT("gknife"), ESearchCase::IgnoreCase), FString::Printf(TEXT("cliff: the host's ground item %d is a gknife here (%s)"), Arg, i >= 0 ? *L->Pickups[i].Ground : TEXT("not here")));
		KnivesWas = L->GetCount(TEXT("iknife"));
		if (A && TestWalkOver(L, P, Arg))
		{
			OutlawsDynCoopProbeLocal::Step = 25;
			At = Now + 3.0;
		}
	}
	else if (Step == TEXT("dynend"))
	{
		AOutlawsLevelActor::WavSpy = nullptr;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dyncoop client RESULT %s (%d passed, %d failed)%s"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails, bOld ? TEXT(" -OLOldDynCoop") : TEXT(""));
	}
}

#endif // WITH_OUTLAWS_TESTS
