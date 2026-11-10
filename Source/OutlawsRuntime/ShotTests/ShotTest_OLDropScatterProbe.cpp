// -OLDropScatterProbe (REQ-DROPSCATTER, the user, 9 Nov: a dropped thing grows from the body, flies out a random way, lands and
// bounces once; in MP the host picks the landing spot and tells everyone; the pickup's reach as now, at the landing spot).
// Story (-OLShots -OLNoFrontEnd -OLLevel=<level>): spots found in the level itself, an open floor, one by a wall, one on a
// ledge's edge, one beside water (each that the level has); 8 drops from each through SpawnDropItem (the knife's 3D model and
// a box of bullets, a sprite): each is held while it flies and starts small (grows), bounces once, rests on floor within a step
// of the body's with nothing between, not in water, away from the body (or the hop in place when nothing passed), and is
// taken when walked onto; none lost.  Then a bad guy with a death drop killed: his drop flies the same.
// MP (-OLShots -OLMpHost=..., and a joiner with -OLCoopJoin=...): the host drops 6 round its feet, the joiner 2 round its own
// (it asks the host); each machine prints every landed drop as "OLSHOT dropscatter mp land <id> <x> <y> <z>" (the check
// compares the two logs).  -OLDropScatterStrip: one drop in view, slowed to a quarter, 8 frames to Saved/OLShots.
// Lines "OLSHOT dropscatter check PASS|FAIL: ..." and "OLSHOT dropscatter RESULT <n> PASS, <m> FAIL".

#include "ShotTestCommon.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "OutlawsGeometry.h"
#include "OutlawsLevel.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsDropScatterProbeLocal
{
	int32 Pass = 0, Fail = 0;
	FString ProbeRole;

	void Check(bool bOk, const FString& What)
	{
		(bOk ? Pass : Fail)++;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dropscatter check %s: %s %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *ProbeRole, *What);
	}

	FCollisionResponseParams NoPawns()
	{
		FCollisionResponseParams R;
		R.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
		return R;
	}

	bool Floor(UWorld* W, const FVector& P, FVector& Out, double Up = 60.0, double Down = 150.0)
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OLDropProbe), true);
		FHitResult H;
		if (!W->LineTraceSingleByChannel(H, P + FVector(0.0, 0.0, Up), P - FVector(0.0, 0.0, Down), ECC_Visibility, Q, NoPawns())) return false;
		Out = H.ImpactPoint;
		return H.ImpactNormal.Z > 0.7;
	}

	bool Clear(UWorld* W, const FVector& A, const FVector& B)
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OLDropProbe), true);
		FHitResult H;
		return !W->LineTraceSingleByChannel(H, A, B, ECC_Visibility, Q, NoPawns());
	}

	/** Open floor: nothing within R at the hip in 8 ways, floor level round it. */
	bool IsOpen(UWorld* W, const FVector& P, double R)
	{
		for (int32 k = 0; k < 8; ++k)
		{
			const FVector D = FRotator(0.0, 45.0 * k, 0.0).Vector();
			if (!Clear(W, P + FVector(0.0, 0.0, 40.0), P + FVector(0.0, 0.0, 40.0) + D * R)) return false;
			FVector F;
			if (!Floor(W, P + D * R * 0.7, F) || FMath::Abs(F.Z - P.Z) > 10.0) return false;
		}
		return true;
	}

	struct FSpot { FString Kind; FVector Body; };
	struct FDrop { TWeakObjectPtr<AActor> Actor; FString Name; bool bHeldEarly = false; float ScaleEarly = 1.f; bool bSeen = false; };

	TArray<FSpot> Spots;
	TArray<FDrop> Drops;
	int32 SpotAt = 0, WalkAt = 0, DoneSeen = 0, MpOwn = 0, MpLanded = 0;
	FVector Park = FVector::ZeroVector;
	TWeakObjectPtr<AOutlawsEnemy> Victim;
	FVector VictimFeet = FVector::ZeroVector;
	double StepAt = 0.0;
	TSet<int32> MpPrinted;
	bool bStepAway = false;

	void FindSpots(AOutlawsLevelActor* L, UWorld* W, const FVector& StartFeet)
	{
		Spots.Reset();
		const FOutlawsLevel* Lv = L->DebugLevelData();
		TArray<FVector> Centres;
		FVector F;
		if (Floor(W, StartFeet, F)) Centres.Add(F);
		if (Lv)
		{
			for (const FOutlawsSector& S : Lv->Sectors)
			{
				if (S.Verts.Num() < 3) continue;
				FVector2D C = FVector2D::ZeroVector;
				for (const FVector2D& V : S.Verts) C += V;
				C /= S.Verts.Num();
				const FVector P = FOutlawsGeometry::ToUE(C.X, Lv->SurfaceY(S, C, true) + 0.2, C.Y);
				float Surf = 0.f;
				if (Floor(W, P, F, 30.0, 60.0) && !L->WaterAt(F + FVector(0.0, 0.0, 15.0), Surf)) Centres.Add(F);
			}
		}
		// open floor
		const FVector* Open = Centres.FindByPredicate([&](const FVector& P) { return IsOpen(W, P, 150.0); });
		if (Open) Spots.Add({ TEXT("open"), *Open });
		// by a wall: from the open spot to the nearest wall, 25 cm short of it
		if (Open)
		{
			for (int32 k = 0; k < 8; ++k)
			{
				const FVector D = FRotator(0.0, 45.0 * k, 0.0).Vector();
				FCollisionQueryParams Q(SCENE_QUERY_STAT(OLDropProbe), true);
				FHitResult H;
				const FVector Eye = *Open + FVector(0.0, 0.0, 40.0);
				if (!W->LineTraceSingleByChannel(H, Eye, Eye + D * 1500.0, ECC_Visibility, Q, NoPawns()) || H.Distance < 80.0) continue;
				const FVector B = *Open + D * (H.Distance - 25.0);
				if (Floor(W, B, F) && FMath::Abs(F.Z - Open->Z) < 10.0) { Spots.Add({ TEXT("wall"), F }); break; }
			}
		}
		// on a ledge's edge: from a sector's middle out to where the floor falls away (1.2 m or more, or none), 30 cm short of it
		bool bLedge = false;
		for (int32 c = 0; c < Centres.Num() && !bLedge; ++c)
		{
			const FVector P = Centres[c];
			for (int32 k = 0; k < 16 && !bLedge; ++k)
			{
				const FVector D = FRotator(0.0, 22.5 * k, 0.0).Vector();
				FVector Last = P;
				for (double t = 20.0; t <= 600.0; t += 15.0)
				{
					const FVector Q = P + D * t;
					FVector G;
					const bool bFloor = Floor(W, FVector(Q.X, Q.Y, Last.Z), G, 40.0, 400.0);
					if (!Clear(W, Last + FVector(0.0, 0.0, 40.0), FVector(Q.X, Q.Y, Last.Z + 40.0))) break;      // a wall: no ledge this way
					if (!bFloor || G.Z < Last.Z - 120.0)
					{
						const FVector B = Last - D * 15.0;
						FVector Bf;
						// (room along the ledge: floor at its height 70 cm out in 4 of the 8 ways at least)
						int32 Room = 0;
						FVector Rf;
						for (int32 r = 0; r < 8; ++r)
						{
							const FVector Way = FRotator(0.0, 45.0 * r, 0.0).Vector() * 70.0;
							Room += Floor(W, B + Way, Rf) && FMath::Abs(Rf.Z - Last.Z) < 10.0 && Clear(W, B + FVector(0.0, 0.0, 12.0), Rf + FVector(0.0, 0.0, 12.0))
								&& Clear(W, B + FVector(0.0, 0.0, 40.0), Rf + FVector(0.0, 0.0, 40.0)) ? 1 : 0;
						}
						if (t > 60.0 && Room >= 4 && Floor(W, B, Bf) && FMath::Abs(Bf.Z - Last.Z) < 10.0) { Spots.Add({ TEXT("ledge"), Bf }); bLedge = true; }
						break;
					}
					if (FMath::Abs(G.Z - Last.Z) > 20.0) break;      // (steps or a slope: not this way)
					Last = G;
				}
			}
		}
		// beside water: a dry sector's floor 40 cm from a wall it shares with a water sector
		if (Lv)
		{
			bool bWater = false;
			for (const TPair<int32, FLinearColor>& Kv : L->WaterTints)
			{
				const FOutlawsSector* Ws = Lv->Sector(Kv.Key);
				if (!Ws || bWater) continue;
				for (const FOutlawsWall& Wl : Ws->Walls)
				{
					const FOutlawsSector* Dry = Lv->Sector(Wl.Adjoin);
					if (!Dry || L->WaterTints.Contains(Wl.Adjoin) || !Ws->Verts.IsValidIndex(Wl.V1) || !Ws->Verts.IsValidIndex(Wl.V2)) continue;
					const FVector2D A = Ws->Verts[Wl.V1], B = Ws->Verts[Wl.V2];
					const FVector2D M = (A + B) * 0.5;
					FVector2D N(-(B - A).Y, (B - A).X);
					if (!N.Normalize()) continue;
					for (int32 s = -1; s <= 1 && !bWater; s += 2)
					{
						const FVector2D Q = M + N * double(s) * (40.0 / FOutlawsGeometry::UnitsToCm);
						const FVector P = FOutlawsGeometry::ToUE(Q.X, Lv->SurfaceY(*Dry, Q, true) + 0.2, Q.Y);
						float Surf = 0.f;
						FVector G;
						if (!Floor(W, P, G, 30.0, 60.0) || L->WaterAt(G + FVector(0.0, 0.0, 15.0), Surf)) continue;
						const FOutlawsSector* At = Lv->SectorAt(FOutlawsGeometry::ToOutlaws(G, 0.5));
						if (!At || At->Index != Dry->Index) continue;
						Spots.Add({ TEXT("water"), G });
						bWater = true;
					}
					if (bWater) break;
				}
			}
		}
	}

	/** The probe's own look at where a drop rests: floor under it within a step of the body's, on it, nothing between, no water. */
	FString RestFault(AOutlawsLevelActor* L, UWorld* W, const FVector& Body, const FVector& Rest)
	{
		FVector F;
		if (!Floor(W, Rest, F, 50.0, 80.0)) return TEXT("no floor under it");
		if (FMath::Abs(F.Z - Body.Z) > 34.0) return FString::Printf(TEXT("floor %.0f cm off the body's"), F.Z - Body.Z);
		if (Rest.Z - F.Z < -1.0 || Rest.Z - F.Z > 8.0) return FString::Printf(TEXT("%.0f cm off its floor"), Rest.Z - F.Z);
		float Surf = 0.f;
		if (L->WaterAt(Rest + FVector(0.0, 0.0, 15.0), Surf)) return TEXT("in water");
		if (!Clear(W, Body + FVector(0.0, 0.0, 40.0), Rest + FVector(0.0, 0.0, 40.0))) return TEXT("something between the body and it");
		return FString();
	}

	const TCHAR* const Names[2] = { TEXT("gknife"), TEXT("gamboxb") };
	constexpr int32 PerSpot = 8;
}

OL_SHOT_MATCH(OLDropScatterProbe) { return OLFlags::Param(TEXT("OLDropScatterProbe")); }

OL_SHOT_TEST(OLDropScatterProbe, 2730)
{
	using namespace OutlawsDropScatterProbeLocal;
	if (!L || ShotStep < 0) return;
	UWorld* W = GetWorld();
	const bool bUp = W && !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bMenuOpen;
	if (!bUp) { if (ShotStep == 0) ShotTime = 0.f; return; }
	const double Now = FPlatformTime::Seconds();
	const bool bOld = AOutlawsLevelActor::OldDropScatter();
	const bool bClient = OLFlags::Has(TEXT("OLCoopJoin"));
	const bool bHost = OLFlags::Has(TEXT("OLMpHost"));
	const bool bStrip = OLFlags::Param(TEXT("OLDropScatterStrip"));
	ProbeRole = bClient ? TEXT("client") : bHost ? TEXT("host") : *L->LevelName;
	const double Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dropscatter %s flights made %d, landed %d, hops in place %d"), *ProbeRole, L->DropFlightsMade, L->DropFlightsLanded, L->DropScatterFallbacks);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dropscatter RESULT %d PASS, %d FAIL%s"), Pass, Fail, bOld ? TEXT(" (-OLOldDropScatter)") : TEXT(""));
		UGameplayStatics::SetGlobalTimeDilation(this, 1.f);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	auto ParkUp = [&]()
	{
		GetCharacterMovement()->DisableMovement();
		SetActorLocation(Park, false, nullptr, ETeleportType::TeleportPhysics);
	};
	auto StandOn = [&](const FVector& Feet)
	{
		SetActorLocation(Feet + FVector(0.0, 0.0, Half + 3.0), false, nullptr, ETeleportType::TeleportPhysics);
	};
	// (each machine in a match: every landed drop with its number, for the check to compare)
	auto PrintLanded = [&]()
	{
		for (const AOutlawsLevelActor::FDropFlightDone& D : L->DropFlightsDone)
		{
			bool bHeld = false;
			int32 Id = -1;
			if (!L->DebugDropPickup(D.Actor.Get(), bHeld, Id) || Id < 0 || MpPrinted.Contains(Id)) continue;
			MpPrinted.Add(Id);
			++MpLanded;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dropscatter mp land %d %.1f %.1f %.1f (bounces %d, smallest %.2f%s)"), Id, D.Rest.X, D.Rest.Y, D.Rest.Z, D.Bounces, D.MinScale, D.bInPlace ? TEXT(", in place") : TEXT(""));
		}
	};

	if (ShotStep == 0)
	{
		if (ShotTime < (bClient ? 12.f : 6.f)) return;
		L->bInvincible = true;
		L->bEnemiesActive = false;
		const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, Half);
		Park = GetActorLocation() + FVector(0.0, 0.0, 1500.0);
		if (bStrip)
		{
			// the sign-off frames: one sprite drop 2.5 m ahead on open floor, slowed to a quarter
			FindSpots(L, W, Feet);
			const FSpot* S = Spots.FindByPredicate([](const FSpot& X) { return X.Kind == TEXT("open"); });
			const FVector Body = S ? S->Body : Feet;
			GetCharacterMovement()->DisableMovement();
			StandOn(Body + FVector(-200.0, 0.0, 0.0));
			StepAt = Now + 1.0;
			Spots = { { TEXT("strip"), Body } };
			ShotStep = 50;
			return;
		}
		if (bHost || bClient)
		{
			StepAt = Now;
			ShotStep = 30;
			return;
		}
		FindSpots(L, W, Feet);
		for (const FSpot& S : Spots) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dropscatter spot %s %s at %s"), *L->LevelName, *S.Kind, *S.Body.ToCompactString());
		Check(Spots.Num() >= 2, FString::Printf(TEXT("spots found in %s: %d"), *L->LevelName, Spots.Num()));
		// the bad guy for the real death drop: the nearest with one
		double Best = 1e18;
		for (TActorIterator<AOutlawsEnemy> It(W); It; ++It)
		{
			if (It->IsDead() || It->Def.DropItem.IsEmpty() || It->IsCritter()) continue;
			const double D = FVector::DistSquared(It->GetActorLocation(), GetActorLocation());
			if (D < Best) { Best = D; Victim = *It; }
		}
		ParkUp();
		SpotAt = 0;
		ShotStep = 1;
		StepAt = Now + 0.5;
		return;
	}

	// ---- story: the drops at each spot -------------------------------------------------------------------------------------
	if (ShotStep == 1)
	{
		if (Now < StepAt) return;
		if (SpotAt >= Spots.Num()) { ShotStep = 10; StepAt = Now; return; }
		const FSpot& S = Spots[SpotAt];
		Drops.Reset();
		DoneSeen = L->DropFlightsDone.Num();
		for (int32 k = 0; k < PerSpot; ++k)
		{
			FDrop D;
			D.Name = Names[k & 1];
			D.Actor = L->SpawnDropItem(D.Name, S.Body, S.Body, k, PerSpot);
			Drops.Add(D);
		}
		ShotStep = 2;
		StepAt = Now + 0.06;
		return;
	}
	if (ShotStep == 2)
	{
		// just made: held (nobody's to take) and small
		if (Now < StepAt) return;
		for (FDrop& D : Drops)
		{
			AActor* A = D.Actor.Get();
			bool bHeld = false;
			int32 Id = -1;
			const bool bPick = L->DebugDropPickup(A, bHeld, Id);
			D.bHeldEarly = bPick && bHeld;
			D.ScaleEarly = A ? float(A->GetActorScale3D().GetMax()) : 1.f;
		}
		ShotStep = 3;
		StepAt = Now + 2.5;
		return;
	}
	if (ShotStep == 3)
	{
		if (Now < StepAt) return;
		const FSpot& S = Spots[SpotAt];
		int32 Made = 0, Held = 0, Small = 0, Once = 0, Good = 0, Out = 0, InPlace = 0;
		TArray<FString> Faults;
		for (FDrop& D : Drops)
		{
			AActor* A = D.Actor.Get();
			if (!A) continue;
			++Made;
			Held += D.bHeldEarly ? 1 : 0;
			const AOutlawsLevelActor::FDropFlightDone* F = nullptr;
			for (int32 i = DoneSeen; i < L->DropFlightsDone.Num(); ++i) if (L->DropFlightsDone[i].Actor.Get() == A) F = &L->DropFlightsDone[i];
			// (its smallest: the flight's own, or what it was just after it was made; a 3D model's own scale is 1)
			const float Smallest = F ? F->MinScale : D.ScaleEarly;
			Small += Smallest < 0.3f ? 1 : 0;
			Once += F && F->Bounces == 1 ? 1 : 0;
			InPlace += F && F->bInPlace ? 1 : 0;
			const FVector Rest = A->GetActorLocation();
			const double Across = FVector::Dist2D(Rest, S.Body);
			Out += (Across >= 30.0 || (F && F->bInPlace)) ? 1 : 0;
			const FString Fault = RestFault(L, W, S.Body, Rest);
			if (Fault.IsEmpty()) ++Good; else Faults.Add(FString::Printf(TEXT("%s %s"), *D.Name, *Fault));
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dropscatter %s %s %s: rest %s, %.0f cm out, %.0f up, bounces %d, smallest %.2f, held at first %d%s%s"), *L->LevelName, *S.Kind, *D.Name,
				*Rest.ToCompactString(), Across, Rest.Z - S.Body.Z, F ? F->Bounces : -1, Smallest, D.bHeldEarly ? 1 : 0, F && F->bInPlace ? TEXT(", hop in place") : TEXT(""), Fault.IsEmpty() ? TEXT("") : *(TEXT(", ") + Fault));
		}
		const FString Where = FString::Printf(TEXT("%s %s"), *L->LevelName, *S.Kind);
		Check(Made == PerSpot, FString::Printf(TEXT("%s: %d of %d drops made"), *Where, Made, PerSpot));
		Check(Held == Made, FString::Printf(TEXT("%s: held while flying (nobody's to take): %d of %d"), *Where, Held, Made));
		Check(Small == Made, FString::Printf(TEXT("%s: each grew from small (under 0.3 of its size): %d of %d"), *Where, Small, Made));
		Check(Once == Made, FString::Printf(TEXT("%s: each bounced once: %d of %d"), *Where, Once, Made));
		Check(Out == Made, FString::Printf(TEXT("%s: each flew out from the body (30 cm or more, or the hop in place): %d of %d (%d in place)"), *Where, Out, Made, InPlace));
		Check(Good == Made, FString::Printf(TEXT("%s: each rests on reachable floor: %d of %d%s"), *Where, Good, Made, Faults.Num() ? *(TEXT(" (") + FString::Join(Faults, TEXT("; ")) + TEXT(")")) : TEXT("")));
		WalkAt = 0;
		ShotStep = 4;
		StepAt = Now;
		return;
	}
	if (ShotStep == 4)
	{
		// walked onto, one by one: each taken
		if (Now < StepAt) return;
		if (WalkAt > 0)
		{
			FDrop& Prev = Drops[WalkAt - 1];
			bool bHeld = false;
			int32 Id = -1;
			Prev.bSeen = !L->DebugDropPickup(Prev.Actor.Get(), bHeld, Id);
		}
		if (WalkAt >= Drops.Num())
		{
			int32 Taken = 0;
			for (const FDrop& D : Drops) Taken += D.bSeen ? 1 : 0;
			Check(Taken == PerSpot, FString::Printf(TEXT("%s %s: walked onto, each taken (none lost): %d of %d"), *L->LevelName, *Spots[SpotAt].Kind, Taken, PerSpot));
			ParkUp();
			++SpotAt;
			ShotStep = 1;
			StepAt = Now + 0.5;
			return;
		}
		// (stepped away first, and room in the pockets for every one still lying there: one taken in passing that did not fit
		// would wait for the player to step away, as the walk-over has it)
		if (!bStepAway)
		{
			ParkUp();
			bStepAway = true;
			StepAt = Now + 0.15;
			return;
		}
		bStepAway = false;
		bool bHeld = false;
		int32 Id = -1;
		for (int32 k = WalkAt; k < Drops.Num(); ++k) L->DebugDropPickup(Drops[k].Actor.Get(), bHeld, Id, true);
		FDrop& D = Drops[WalkAt++];
		AActor* A = D.Actor.Get();
		if (A && L->DebugDropPickup(A, bHeld, Id)) StandOn(A->GetActorLocation());
		StepAt = Now + 0.4;
		return;
	}
	if (ShotStep == 10)
	{
		// a bad guy's own death drop
		AOutlawsEnemy* E = Victim.Get();
		if (!E)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dropscatter %s: no bad guy with a death drop here"), *L->LevelName);
			Finish();
			return;
		}
		VictimFeet = E->GetActorLocation() - FVector(0.0, 0.0, E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
		DoneSeen = L->DropFlightsDone.Num();
		E->TakeHit(1000.f, E->GetActorLocation(), FVector(1.0, 0.0, 0.0), E->GetActorLocation() - FVector(200.0, 0.0, 0.0));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dropscatter %s: %s killed at %s (drops %s)"), *L->LevelName, *E->GetName(), *VictimFeet.ToCompactString(), *E->Def.DropItem);
		StepAt = Now + 15.0;
		ShotStep = 11;
		return;
	}
	if (ShotStep == 11)
	{
		const AOutlawsLevelActor::FDropFlightDone* F = nullptr;
		if (L->DropFlightsDone.Num() > DoneSeen) F = &L->DropFlightsDone[DoneSeen];      // (nothing else drops meanwhile: the first after the kill)
		if (!F && Now < StepAt) return;
		const AActor* A = F ? F->Actor.Get() : nullptr;
		const FString Fault = A ? RestFault(L, W, F->Body, A->GetActorLocation()) : FString(TEXT("no flight"));
		Check(F && F->Bounces == 1 && F->MinScale < 0.3f && Fault.IsEmpty(), FString::Printf(TEXT("the bad guy's death drop grew, flew, bounced once (%d) and rests on reachable floor (from %s, %.0f cm from where he fell, to %s)%s"),
			F ? F->Bounces : -1, F ? *F->Body.ToCompactString() : TEXT("-"), F ? FVector::Dist2D(F->Body, VictimFeet) : -1.0, A ? *A->GetActorLocation().ToCompactString() : TEXT("-"),
			Fault.IsEmpty() ? TEXT("") : *(TEXT(": ") + Fault)));
		Finish();
		return;
	}

	// ---- a match: host and joiner ------------------------------------------------------------------------------------------
	if (ShotStep == 30)
	{
		UOutlawsCoop* C = UOutlawsCoop::Get(this);
		if (!C) { Check(false, TEXT("a co-op session")); Finish(); return; }
		if (bHost && C->PlayerCount() < 2) { if (Now - StepAt > 180.0) { Check(false, TEXT("the joiner came")); Finish(); } return; }
		StepAt = Now + (bHost ? 20.0 : 14.0);
		ShotStep = 31;
		return;
	}
	if (ShotStep == 31)
	{
		PrintLanded();
		if (Now < StepAt) return;
		// the host 6 round its feet, the joiner 2 round its own (a few metres apart: each spawns at its own start)
		const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, Half);
		Park = GetActorLocation() + FVector(0.0, 0.0, 1500.0);
		const int32 N = bHost ? 6 : 2;
		Drops.Reset();
		for (int32 k = 0; k < N; ++k)
		{
			FDrop D;
			D.Name = Names[k & 1];
			// (the joiner's second: a dead player's shotgun with what it carried, the longest name a client sends)
			D.Actor = bClient && k == 1 ? L->DebugDropWithContents(D.Name = TEXT("gsgun"), TEXT("isgun=1,ishells=17"), Feet) : L->SpawnDropItem(D.Name, Feet, Feet, k, N);
			Drops.Add(D);
		}
		MpOwn = N;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dropscatter %s made %d drops round %s"), *ProbeRole, N, *Feet.ToCompactString());
		ParkUp();
		StepAt = Now + 60.0;
		ShotStep = 32;
		return;
	}
	if (ShotStep == 32)
	{
		// until both machines' 8 have landed here (then a few seconds for the other machine's last), or a minute
		PrintLanded();
		if (MpLanded >= 8 && StepAt > Now + 5.0) StepAt = Now + 5.0;
		if (Now < StepAt) return;
		int32 Own = 0;
		for (const FDrop& D : Drops)
		{
			bool bHeld = false;
			int32 Id = -1;
			Own += L->DebugDropPickup(D.Actor.Get(), bHeld, Id) && Id >= 0 && !bHeld && MpPrinted.Contains(Id) ? 1 : 0;
		}
		Check(Own == MpOwn, FString::Printf(TEXT("its own drops numbered, landed and lying there: %d of %d"), Own, MpOwn));
		Check(MpLanded >= 8, FString::Printf(TEXT("the drops of both machines landed here: %d (8 made)"), MpLanded));
		Finish();
		return;
	}

	// ---- the sign-off strip --------------------------------------------------------------------------------------------------
	if (ShotStep == 50)
	{
		if (Now < StepAt) return;
		UGameplayStatics::SetGlobalTimeDilation(this, 0.25f);
		Drops.Reset();
		FDrop D;
		D.Name = TEXT("gamboxb");
		D.Actor = L->SpawnDropItem(D.Name, Spots[0].Body, Spots[0].Body, 0, 1);
		Drops.Add(D);
		// the camera side on to its way, as far back as the room lets it (up to 2.6 m), looking at the middle of the flight
		const FVector Body = Spots[0].Body;
		const FVector Rest = D.Actor.IsValid() ? L->DropRestAt(D.Actor.Get()) : Body + FVector(60.0, 0.0, 0.0);
		const FVector Mid = (Body + Rest) * 0.5;
		FVector Side = FVector(-(Rest - Body).Y, (Rest - Body).X, 0.0).GetSafeNormal();
		if (Side.IsNearlyZero()) Side = FVector(1.0, 0.0, 0.0);
		double BestRoom = -1.0;
		FVector BestSide = Side;
		for (int32 sgn = -1; sgn <= 1; sgn += 2)
		{
			FCollisionQueryParams Q(SCENE_QUERY_STAT(OLDropProbe), true);
			FHitResult H;
			const FVector Eye = Mid + FVector(0.0, 0.0, 120.0);
			const double Room = W->LineTraceSingleByChannel(H, Eye, Eye + Side * double(sgn) * 400.0, ECC_Visibility, Q, NoPawns()) ? H.Distance : 400.0;
			if (Room > BestRoom) { BestRoom = Room; BestSide = Side * double(sgn); }
		}
		StandOn(Mid + BestSide * FMath::Clamp(BestRoom - 50.0, 80.0, 260.0));
		if (Controller) Controller->SetControlRotation(((Mid + FVector(0.0, 0.0, 45.0)) - Camera->GetComponentLocation()).Rotation());
		WalkAt = 0;
		StepAt = Now;
		ShotStep = 51;
		return;
	}
	if (ShotStep == 51)
	{
		// 8 frames over the flight (about 2.8 s slowed): 0.05 .. 3.0 s
		static const double Times[8] = { 0.08, 0.3, 0.7, 1.3, 2.0, 2.6, 3.1, 4.2 };
		const double T = Now - StepAt;
		if (WalkAt < 8 && T >= Times[WalkAt])
		{
			const AActor* A = Drops[0].Actor.Get();
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dropscatter strip frame %d at %.2f s: %s, scale %.2f"), WalkAt + 1, T, A ? *A->GetActorLocation().ToCompactString() : TEXT("none"), A ? A->GetActorScale3D().GetMax() : 0.f);
			FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("ol_dropscatter_%d.png"), WalkAt + 1), false, false);
			++WalkAt;
		}
		if (WalkAt >= 8 && T > 5.0) Finish();
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
