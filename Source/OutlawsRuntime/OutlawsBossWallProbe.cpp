// BUG-BOSSWALL's probe (from lane SNAPA's local check, 2026-10-28): -OLLevel=<level> -OLNoFrontEnd -OLBossWallProbe
// [-OLOldBossWall] [-OLDifficulty=bad].  Snapshot 5 (townex, a399256): the boss Sanchez, chasing, pressed on the boardwalk
// building's door frame at UE (8340, -1240) for 20 s and more, never seeing the player.  Every other bad guy put dormant, the
// player invincible.  In the town: (1) the boss put in that doorway at UE (8272, -1246), the player at the snapshot's spot;
// (2) the same, the player 8 to 20 m off out of his view.  In any level: (3) a frame on a hunter's way, found in the level: a
// place where the hunt's next step through an opening lies square behind something solid (a door's leaf, a jamb), a bad guy
// made a hunter put there, the player past the opening out of his view; (4) an ordinary bad guy (no hunter) there, sent
// straight at that step.  Each: the longest spell he stands still on a move (under 25 cm in a second, not shooting), how
// long from the first such press till he is a metre off it, how long till he sees the player; his move, what blocked it and
// the sector's walls by him as he is first pressed.  Pass: off it within 6.5 s of the first press, no press over 3 s, and a
// hunter sees the player in the case's time.  Lines "OLSHOT bosswall check PASS|FAIL" and "OLSHOT bosswall RESULT".
// All times are his own (BUG-BOSSWALL 2026-10-29: the bad guys run at the player's pace, EnemyPace, 0.3 of real time for a
// player standing still, so real seconds read three times long; the real ones are printed beside them).
// Co-op: -OLCoopAuto -OLCoopTest=bosswall -OLCoopLevel=townex on the host and its client (UOutlawsCoop::TickBossWallTest).
#include "OutlawsEnemy.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsLevel.h"
#include "OutlawsGeometry.h"
#include "OutlawsFlags.h"
#include "OutlawsCoop.h"
#include "OutlawsPlayer.h"
#include "Components/CapsuleComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsBossWallProbeLocal
{
	struct FCase
	{
		FString Name;
		double Sec = 30.0;
		int32 Kind = 0;                  // 0 the town's doorway, player in view; 1 the same, out of view; 2 a hunter at the frame; 3 an ordinary one
	};
	/** A press measured on one bad guy: still on a move (under 25 cm a second), how long, and when he got off it. */
	struct FRun
	{
		// (Time and the rest in his own time; the Real* twins in real seconds, for the report)
		double Time = 0, Pressed = 0, LongestPress = 0, FirstPressAt = -1, FreeAt = -1, SeenAt = -1, Path = 0, StillSince = 0;
		double RealTime = 0, RealLongestPress = 0, RealFirstPressAt = -1, RealFreeAt = -1, RealSeenAt = -1, RealStillSince = 0, PaceSum = 0;
		FVector PressAt = FVector::ZeroVector, Start = FVector::ZeroVector, Last = FVector::ZeroVector, Anchor = FVector::ZeroVector;
		int32 Slides0 = 0, Shots0 = 0;
		bool bDumped = false;
		/** One frame of Dt real seconds at his Pace: bMoving (on a move, not shooting) and where he is; true while he is pressed. */
		bool Step(double Dt, double Pace, const FVector& Now, bool bMoving)
		{
			Time += Dt * Pace;
			RealTime += Dt;
			PaceSum += Dt * Pace;
			Path += FVector::Dist2D(Now, Last);
			Last = Now;
			if (!bMoving || FVector::Dist2D(Now, Anchor) > 25.0) { Anchor = Now; StillSince = Time; RealStillSince = RealTime; }
			const double Still = Time - StillSince;
			// (a spell counts from where he stopped: under 25 cm for 1 s of his)
			const bool bPress = bMoving && Still > 1.0;
			if (bPress)
			{
				Pressed += Dt * Pace;
				LongestPress = FMath::Max(LongestPress, Still);
				RealLongestPress = FMath::Max(RealLongestPress, RealTime - RealStillSince);
				if (FirstPressAt < 0.0) { FirstPressAt = StillSince; RealFirstPressAt = RealStillSince; PressAt = Anchor; }
			}
			if (FirstPressAt >= 0.0 && FreeAt < 0.0 && FVector::Dist2D(Now, PressAt) > 100.0) { FreeAt = Time; RealFreeAt = RealTime; }
			return bPress;
		}
		void Seen() { if (SeenAt < 0.0) { SeenAt = Time; RealSeenAt = RealTime; } }
		double Free() const { return FirstPressAt < 0.0 ? 0.0 : (FreeAt < 0.0 ? -1.0 : FreeAt - FirstPressAt); }
		double RealFree() const { return RealFirstPressAt < 0.0 ? 0.0 : (RealFreeAt < 0.0 ? -1.0 : RealFreeAt - RealFirstPressAt); }
		FString Describe() const
		{
			return FString::Printf(TEXT("in his own time (pace %.2f; real seconds in brackets): %s; still on a move %.1f s in all, the longest spell %.1f s (%.1f); a metre off the first press %s; sees the player %s; walked %.1f m in %.1f s (%.1f)"),
				RealTime > 0.0 ? PaceSum / RealTime : 1.0,
				FirstPressAt < 0.0 ? TEXT("never pressed") : *FString::Printf(TEXT("first pressed at %.1f s (%.1f) at %s"), FirstPressAt, RealFirstPressAt, *PressAt.ToCompactString()), Pressed, LongestPress, RealLongestPress,
				FirstPressAt < 0.0 ? TEXT("(none)") : (Free() < 0.0 ? TEXT("never") : *FString::Printf(TEXT("%.1f s (%.1f) after it"), Free(), RealFree())),
				SeenAt < 0.0 ? TEXT("never") : *FString::Printf(TEXT("at %.1f s (%.1f)"), SeenAt, RealSeenAt), Path / 100.0, Time, RealTime);
		}
	};
	/** (3) and (4): where the hunt's step from a spot lies square behind something solid. */
	struct FFrame
	{
		bool bFound = false;
		FVector EnemyFeet = FVector::ZeroVector, PlayerFeet = FVector::ZeroVector, Step = FVector::ZeroVector;
		FString What;
	};
	double T = 0.0, NextLog = 0.0;
	int32 CaseI = -1, Passes = 0, Fails = 0;
	FRun R;
	TArray<FCase> Cases;
	TWeakObjectPtr<AOutlawsEnemy> Boss, Man;
	FFrame Frame;
	bool bManWasHunter = false;
	const FVector SnapPlayer(8690.738, -1974.481, 89.009);
	const FVector2D Doorway(8272.331, -1245.695);

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT bosswall check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}

	FVector EyeOf(const AOutlawsEnemy* E) { return E->GetActorLocation() + FVector(0.0, 0.0, E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() * 0.8); }
	bool IsMoving(const AOutlawsEnemy* E)
	{
		const FString St = E->DebugBossState();
		return E->GetState() != EOutlawsEnemyState::Shoot && (St.Contains(TEXT("sub 3 ")) || St.Contains(TEXT("sub 4 ")) || St.Contains(TEXT("sub 8 ")));
	}

	bool Clear(UWorld* W, const FVector& A, const FVector& B, const AActor* I1, const AActor* I2)
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OLBossWallProbe), true, I1);
		Q.AddIgnoredActor(I2);
		FCollisionResponseParams RP;
		RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
		FHitResult H;
		return !W->LineTraceSingleByChannel(H, A, B, ECC_Visibility, Q, RP);
	}

	bool FloorAt(UWorld* W, const FVector2D& XY, double FromZ, FVector& Out, const AActor* I1, const AActor* I2)
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OLBossWallFloor), true, I1);
		Q.AddIgnoredActor(I2);
		FHitResult H;
		if (!W->LineTraceSingleByChannel(H, FVector(XY, FromZ), FVector(XY, FromZ - 800.0), ECC_Visibility, Q)) return false;
		Out = H.ImpactPoint;
		return true;
	}

	void PutAt(AOutlawsEnemy* E, const FVector& Feet)
	{
		if (E->IsDormant()) { E->SetDormant(false); E->Appear(Feet); }
		else E->SetActorLocation(Feet + FVector(0.0, 0.0, E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0), false, nullptr, ETeleportType::TeleportPhysics);
		E->Wake(false);
	}

	void PutPlayer(APawn* Pawn, const FVector& At, double Yaw)
	{
		Pawn->TeleportTo(At, FRotator(0.0, Yaw, 0.0), false, true);
		if (AController* C = Pawn->GetController()) C->SetControlRotation(FRotator(0.0, Yaw, 0.0));
	}

	bool PutPlayerOutOfView(UWorld* W, AOutlawsEnemy* E, APawn* Pawn)
	{
		const float Half = Pawn->GetSimpleCollisionHalfHeight();
		for (int32 Ring = 0; Ring < 3; ++Ring)
		{
			for (int32 i = 0; i < 24; ++i)
			{
				const double D = 800.0 + Ring * 600.0;
				const FVector Dir = FRotator(0.0, i * 15.0, 0.0).Vector();
				FVector Floor;
				const FVector2D XY(E->GetActorLocation().X + Dir.X * D, E->GetActorLocation().Y + Dir.Y * D);
				if (!FloorAt(W, XY, E->GetActorLocation().Z + 250.0, Floor, E, Pawn)) continue;
				if (FMath::Abs(Floor.Z - (E->GetActorLocation().Z - E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight())) > 120.0) continue;
				const FVector At = Floor + FVector(0.0, 0.0, Half + 5.0);
				if (Clear(W, EyeOf(E), At + FVector(0.0, 0.0, Half * 0.8), E, Pawn) || Clear(W, EyeOf(E), At, E, Pawn)) continue;
				PutPlayer(Pawn, At, (E->GetActorLocation() - At).Rotation().Yaw);
				UE_LOG(LogOutlaws, Display, TEXT("OLBOSSWALL the player out of his view at %s, %.0f m off"), *At.ToCompactString(), D / 100.0);
				return true;
			}
		}
		return false;
	}

	/** (3)/(4): over every opening a body walks through (the hunt's own rules), a spot 1.5..4 u in from it and off to one
	 *  side, the player 4..9 u out past it out of sight, where the hunt's step from the spot goes through that opening and
	 *  the body's straight way to the step meets something solid square on (within 25 degrees), a door's leaf first. */
	FFrame FindFrame(UWorld* W, AOutlawsLevelActor* L, const AOutlawsEnemy* E, const APawn* Pawn)
	{
		FFrame Out;
		const FOutlawsLevel* Lv = L->LiveLevel();
		if (!Lv) return Out;
		const float Half = E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		const float Rad = E->GetCapsuleComponent()->GetScaledCapsuleRadius();
		const float PHalf = Pawn->GetSimpleCollisionHalfHeight();
		const double HeightU = 2.0 * Half / FOutlawsGeometry::UnitsToCm, StepU = 3.0;
		const FCollisionShape Body = FCollisionShape::MakeCapsule(Rad * 0.9f, FMath::Max(Half * 0.9f - 30.f, Rad));
		const FCollisionShape Whole = FCollisionShape::MakeCapsule(Rad, Half);
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OLBossWallScan), true, E);
		Q.AddIgnoredActor(Pawn);
		FCollisionResponseParams RP;
		RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
		auto InSector = [Lv](const FVector2D& P, const FOutlawsSector& S)
		{
			const FOutlawsSector* At = Lv->SectorAt(P.X, S.Floor.Y + 0.2, P.Y);
			return At && At->Index == S.Index;
		};
		auto Feet = [&](const FVector2D& P, double FloorY, FVector& OutFeet)
		{
			const FVector U = FOutlawsGeometry::ToUE(P, FloorY);
			return FloorAt(W, FVector2D(U.X, U.Y), U.Z + 150.0, OutFeet, E, Pawn) && FMath::Abs(OutFeet.Z - U.Z) < 40.0;
		};
		double Best = 0.0;
		int32 Openings = 0, Spots = 0;
		for (const FOutlawsSector& S : Lv->Sectors)
		{
			if (S.Flags & OutlawsFlags::Water) continue;
			for (int32 w = 0; w < S.Walls.Num(); ++w)
			{
				const FOutlawsWall& Wl = S.Walls[w];
				if (!Lv->Sectors.IsValidIndex(Wl.Adjoin) || (Wl.Flags1 & (OutlawsFlags::WallNoPass | OutlawsFlags::WallFence)) || !S.Verts.IsValidIndex(Wl.V1) || !S.Verts.IsValidIndex(Wl.V2)) continue;
				const FOutlawsSector& D = Lv->Sectors[Wl.Adjoin];
				const double Lo = FMath::Max(S.Floor.Y, D.Floor.Y), Hi = FMath::Min(S.Ceil.Y, D.Ceil.Y);
				if (Hi - Lo < HeightU * 0.9 || FMath::Abs(D.Floor.Y - S.Floor.Y) > 1.0) continue;
				const FVector2D A = S.Verts[Wl.V1], B = S.Verts[Wl.V2];
				const double Width = FVector2D::Distance(A, B);
				if (Width < 2.0 || Width > 10.0) continue;
				const FVector2D Mid = (A + B) * 0.5, Tan = (B - A) / Width;
				FVector2D N(-Tan.Y, Tan.X);
				if (!InSector(Mid + N * 0.5, S)) N = -N;
				if (!InSector(Mid + N * 0.5, S)) continue;
				++Openings;
				// the player out past it, on a floor, in a sector of its own
				FVector PFeet;
				bool bPlayer = false;
				for (const double K : { 4.0, 6.0, 9.0 })
				{
					for (const double Tt : { 0.0, -3.0, 3.0 })
					{
						const FVector2D PP = Mid - N * K + Tan * Tt;
						const FOutlawsSector* PS = Lv->SectorAt(PP.X, D.Floor.Y + 0.2, PP.Y);
						if (!PS || PS->Index == S.Index || !Feet(PP, PS->Floor.Y, PFeet)) continue;
						bPlayer = true;
						break;
					}
					if (bPlayer) break;
				}
				if (!bPlayer) continue;
				for (const double Dd : { 1.5, 2.5, 4.0 })
				{
					for (const double Tt : { -5.0, -3.5, -2.0, 2.0, 3.5, 5.0 })
					{
						const FVector2D P2 = Mid + N * Dd + Tan * (Tt + (Tt > 0.0 ? Width * 0.5 : -Width * 0.5));
						FVector EFeet;
						if (!InSector(P2, S) || !Feet(P2, S.Floor.Y, EFeet)) continue;
						const FVector At = EFeet + FVector(0.0, 0.0, Half + 3.0);
						if (W->OverlapAnyTestByChannel(At, FQuat::Identity, ECC_Pawn, Whole, Q, RP)) continue;
						FVector Step;
						FIntPoint Via;
						if (!L->PathStepToward(EFeet, PFeet, StepU, HeightU, Step, &Via) || Via != FIntPoint(S.Index, w)) continue;
						++Spots;
						FVector Dir = Step - EFeet;
						Dir.Z = 0.0;
						const double Dist = Dir.Size();
						Dir /= FMath::Max(Dist, 1.0);
						const FVector From = EFeet + FVector(0.0, 0.0, Half + 30.0);
						FHitResult Hit;
						if (!W->SweepSingleByChannel(Hit, From, From + Dir * Dist, FQuat::Identity, ECC_Pawn, Body, Q, RP) || Hit.Distance < 30.0 || Hit.Distance > Dist - 30.0) continue;
						const FVector Nn = FVector(Hit.ImpactNormal.X, Hit.ImpactNormal.Y, 0.0).GetSafeNormal();
						const double Dot = FVector::DotProduct(Dir, Nn);
						if (Dot > -0.9) continue;
						if (Clear(W, At + FVector(0.0, 0.0, Half * 0.8), PFeet + FVector(0.0, 0.0, PHalf * 1.7), E, Pawn)) continue;
						const FString Comp = Hit.GetComponent() ? Hit.GetComponent()->GetName() : TEXT("?");
						const double Score = -Dot + (Comp.StartsWith(TEXT("OutlawsDoor")) ? 0.5 : 0.0);
						if (Score <= Best) continue;
						Best = Score;
						Out.bFound = true;
						Out.EnemyFeet = EFeet;
						Out.PlayerFeet = PFeet;
						Out.Step = Step;
						Out.What = FString::Printf(TEXT("sector %d's opening %d (to %d, %.1f u wide): the spot %s, the step %s, %s %.0f cm on, square to %.0f degrees; the player at %s"),
							S.Index, w, D.Index, Width, *EFeet.ToCompactString(), *Step.ToCompactString(), *Comp, Hit.Distance, FMath::RadiansToDegrees(FMath::Acos(FMath::Min(1.0, -Dot))), *PFeet.ToCompactString());
					}
				}
			}
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLBOSSWALL frame scan of %s: %d openings, %d spots whose step goes through them; %s"), *L->LevelName, Openings, Spots, Out.bFound ? *Out.What : TEXT("none square behind anything"));
		return Out;
	}

	AOutlawsEnemy* CaseEnemy(int32 Kind) { return Kind <= 1 ? Boss.Get() : Man.Get(); }

	void Begin(UWorld* W, AOutlawsLevelActor* L, APawn* Pawn)
	{
		const FCase& C = Cases[CaseI];
		AOutlawsEnemy* E = CaseEnemy(C.Kind);
		R = FRun();
		if (!E) return;
		if (C.Kind <= 1)
		{
			FVector Feet;
			if (!FloorAt(W, Doorway, 300.0, Feet, E, Pawn)) Feet = FVector(Doorway, 7.65);
			PutAt(E, Feet);
			if (C.Kind == 0) PutPlayer(Pawn, SnapPlayer, (E->GetActorLocation() - SnapPlayer).Rotation().Yaw);
			else if (!PutPlayerOutOfView(W, E, Pawn)) UE_LOG(LogOutlaws, Display, TEXT("OLBOSSWALL no spot out of his view found: the player at the snapshot's spot"));
		}
		else
		{
			const FVector PAt = Frame.PlayerFeet + FVector(0.0, 0.0, Pawn->GetSimpleCollisionHalfHeight() + 5.0);
			PutPlayer(Pawn, PAt, (Frame.EnemyFeet - Frame.PlayerFeet).Rotation().Yaw);
			E->bHunter = C.Kind == 2 || bManWasHunter;
			PutAt(E, Frame.EnemyFeet);
			if (C.Kind == 3) E->DebugSwimTo(Frame.Step + FVector(0.0, 0.0, E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()));
		}
		R.Slides0 = E->DebugSlides();
		R.Shots0 = E->ShotsTotal;
		R.Start = R.Last = R.Anchor = E->GetActorLocation();
		UE_LOG(LogOutlaws, Display, TEXT("OLBOSSWALL case %s: %s (hunter %d) at %s, %s"), *C.Name, *E->GetOutlawsName(), E->bHunter ? 1 : 0, *E->GetActorLocation().ToCompactString(), *E->DebugBossState());
	}

	void Finish(AOutlawsEnemy* E)
	{
		const FCase& C = Cases[CaseI];
		const double Free = R.Free();
		const bool bHunterCase = C.Kind != 3;
		const bool bOk = Free >= 0.0 && Free <= 6.5 && R.LongestPress <= 3.0 && (!bHunterCase || R.SeenAt >= 0.0);
		Check(bOk, FString::Printf(TEXT("%s, %s: %s, %d slides, %d shots; now %s"), *C.Name, *E->GetOutlawsName(), *R.Describe(), E->DebugSlides() - R.Slides0, E->ShotsTotal - R.Shots0,
			*E->GetActorLocation().ToCompactString()));
		if (C.Kind >= 2) { E->bHunter = bManWasHunter; E->SetDormant(true); }
	}

	bool Tick(float Dt)
	{
		UWorld* W = nullptr;
		if (GEngine) for (const FWorldContext& Ctx : GEngine->GetWorldContexts()) if (Ctx.World() && Ctx.World()->IsGameWorld()) W = Ctx.World();
		if (!W) return true;
		AOutlawsLevelActor* L = nullptr;
		for (TActorIterator<AOutlawsLevelActor> It(W); It; ++It) { L = *It; break; }
		APlayerController* PC = W->GetFirstPlayerController();
		APawn* Pawn = PC ? PC->GetPawn() : nullptr;
		if (!L || !Pawn || L->IsFrontEnd() || L->GetEnemies().Num() == 0) return true;
		if (L->IsMoviePlaying()) { L->SkipMovie(); return true; }
		Dt = FMath::Min(Dt, 0.1f);
		T += Dt;
		if (T < 5.0 || !L->AreEnemiesActive()) return true;
		L->bInvincible = true;
		if (CaseI < 0)
		{
			const bool bTown = L->LevelName.Equals(TEXT("townex"), ESearchCase::IgnoreCase);
			AOutlawsEnemy* B = L->GetBoss();
			for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies()) if (!B && Wk.IsValid() && Wk->bBoss && !Wk->bVoiceOnly) B = Wk.Get();
			AOutlawsEnemy* M = nullptr;
			for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
			{
				AOutlawsEnemy* E = Wk.Get();
				if (!M && E && E != B && !E->bBoss && !E->IsDead() && !E->IsDormant() && !E->bVoiceOnly && !E->bStationary && !E->IsCritter() && !E->bBehindDoor) M = E;
			}
			Boss = bTown ? B : nullptr;
			Man = M;
			bManWasHunter = M && M->bHunter;
			int32 Asleep = 0;
			for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies()) if (Wk.IsValid() && !Wk->IsDead() && !Wk->IsDormant()) { Wk->SetDormant(true); ++Asleep; }
			UE_LOG(LogOutlaws, Display, TEXT("OLBOSSWALL %s (difficulty %d), %s: the boss %s, the ordinary one %s; %d put dormant"), *L->LevelName, int32(L->Difficulty),
				AOutlawsEnemy::OldBossWall() ? TEXT("-OLOldBossWall (no slide)") : TEXT("the slide along walls"), B ? *B->GetOutlawsName() : TEXT("none"), M ? *M->GetOutlawsName() : TEXT("none"), Asleep);
			if (bTown && B)
			{
				Cases.Add({ TEXT("(1) the boss in the boardwalk doorway, the player at the snapshot's spot"), 30.0, 0 });
				Cases.Add({ TEXT("(2) the boss in the boardwalk doorway, the player out of his view"), 40.0, 1 });
			}
			else if (bTown) Check(false, TEXT("the town's boss"));
			if (M)
			{
				Frame = FindFrame(W, L, M, Pawn);
				if (Frame.bFound)
				{
					Cases.Add({ FString::Printf(TEXT("(3) a hunter where his step lies behind a frame in %s"), *L->LevelName), 30.0, 2 });
					Cases.Add({ FString::Printf(TEXT("(4) an ordinary bad guy sent at it in %s"), *L->LevelName), 20.0, 3 });
				}
				else Check(false, FString::Printf(TEXT("a frame on a hunter's way in %s"), *L->LevelName));
			}
			else Check(false, TEXT("an ordinary bad guy to try"));
			CaseI = 0;
			if (Cases.Num() > 0) Begin(W, L, Pawn);
			return true;
		}
		AOutlawsEnemy* E = CaseI < Cases.Num() ? CaseEnemy(Cases[CaseI].Kind) : nullptr;
		if (CaseI < Cases.Num() && (!E || E->IsDead())) { Check(false, FString::Printf(TEXT("%s: the bad guy is gone"), *Cases[CaseI].Name)); CaseI = Cases.Num(); }
		if (CaseI >= Cases.Num())
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT bosswall RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
			FGenericPlatformMisc::RequestExit(false);
			return false;
		}
		const FVector Now = E->GetActorLocation();
		const bool bPress = R.Step(Dt, E->CustomTimeDilation, Now, IsMoving(E));
		if (R.SeenAt < 0.0 && E->DebugBossState().Contains(TEXT("sees 1"))) R.Seen();
		if (bPress && !R.bDumped)
		{
			// what he is after, what holds him, the walls by him, and the hunt's next opening from here
			R.bDumped = true;
			const float Half = E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			FVector Step2;
			FIntPoint Via;
			const FVector PFeet = Pawn->GetActorLocation() - FVector(0.0, 0.0, Pawn->GetSimpleCollisionHalfHeight());
			const bool bWay = L->PathStepToward(Now - FVector(0.0, 0.0, Half), PFeet, 3.0, 2.0 * Half / FOutlawsGeometry::UnitsToCm, Step2, &Via);
			UE_LOG(LogOutlaws, Display, TEXT("OLBOSSWALL pressed at %.1f s: %s"), R.Time, *E->DebugMoveState());
			UE_LOG(LogOutlaws, Display, TEXT("OLBOSSWALL the hunt's way from here: %s"), bWay ? *FString::Printf(TEXT("the step %s through sector %d's wall %d"), *Step2.ToCompactString(), Via.X, Via.Y) : TEXT("none"));
			UE_LOG(LogOutlaws, Display, TEXT("OLBOSSWALL his feet: %s"), *L->DebugSectorWalls(Now - FVector(0.0, 0.0, Half), 4.0));
		}
		if (T >= NextLog)
		{
			NextLog = T + 2.0;
			UE_LOG(LogOutlaws, Display, TEXT("OLBOSSWALL %d t %.0f: at %s (%.0f cm from the start, %.0f from the player), %s; %s"), CaseI + 1, R.Time, *Now.ToCompactString(),
				FVector::Dist2D(Now, R.Start), FVector::Dist2D(Now, Pawn->GetActorLocation()), *E->DebugBossState(), *E->DebugMoveState());
		}
		if (R.Time >= Cases[CaseI].Sec)
		{
			Finish(E);
			if (++CaseI < Cases.Num()) Begin(W, L, Pawn);
		}
		return true;
	}

	struct FReg
	{
		FReg()
		{
			if (OLFlags::Param(TEXT("OLBossWallProbe"))) FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&Tick));
		}
	} GReg;

	// ---- the co-op pair (-OLCoopTest=bosswall) ----
	// (Bw: UOutlawsCoop has Cw members of its own, the climb wheel test's)
	int32 BwStep = 0, BwPasses = 0, BwFails = 0;
	double BwAt = 0.0;
	FRun BwRun;
	TWeakObjectPtr<AOutlawsEnemy> BwBoss;
}

// -OLCoopAuto -OLCoopTest=bosswall -OLCoopLevel=townex: the host puts the boss in the boardwalk doorway (case (1) above), both
// players at the snapshot's spot; the host's copy of the boss and the client's are each watched 30 s: off the frame within
// 6 s of a press, no press over 3 s (the client's copy follows the host's, which alone thinks)
void UOutlawsCoop::TickBossWallTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	using namespace OutlawsBossWallProbeLocal;
	const bool bHost = IsHost();
	const TCHAR* Me = bHost ? TEXT("host") : TEXT("client");
	auto BwCheck = [Me](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlawsAI, Log, TEXT("OLCOOP bosswall check %s (%s): %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), Me, *What);
		if (bOk) ++BwPasses; else ++BwFails;
	};
	auto TheBoss = [L]() -> AOutlawsEnemy*
	{
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies()) if (Wk.IsValid() && Wk->bBoss && !Wk->bVoiceOnly) return Wk.Get();
		return nullptr;
	};
	UWorld* W = L->GetWorld();
	static double LastTick = 0.0;
	const double Dt = LastTick > 0.0 ? FMath::Min(Now - LastTick, 0.1) : 0.0;
	LastTick = Now;
	switch (BwStep)
	{
	case 0:
		if (!L->LevelName.Equals(TEXT("townex"), ESearchCase::IgnoreCase) || !L->HasActorBegunPlay() || !L->AreEnemiesActive()) return;
		if (bHost)
		{
			const AOutlawsCoopLink* CL = GetLink(PartnerIndex());
			if (!CL || CL->BuiltSerial != StartedSerial || !PartnerPuppet()) return;
		}
		if (BwAt == 0.0) { BwAt = Now + 4.0; return; }
		if (Now < BwAt) return;
		L->bInvincible = true;
		P->TeleportTo(SnapPlayer + FVector(bHost ? 0.0 : 120.0, 0.0, 0.0), FRotator(0.0, 107.0, 0.0), false, true);
		BwStep = bHost ? 1 : 20;
		BwAt = Now + 2.0;
		return;
	// ---- the host: the others asleep, the boss in the doorway ----
	case 1:
	{
		if (Now < BwAt) return;
		AOutlawsEnemy* B = TheBoss();
		if (!B) { BwCheck(false, TEXT("the town's boss")); BwStep = 5; BwAt = Now + 34.0; return; }
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies()) if (Wk.IsValid() && Wk.Get() != B && !Wk->IsDead() && !Wk->IsDormant()) Wk->SetDormant(true);
		FVector Feet;
		if (!FloorAt(W, Doorway, 300.0, Feet, B, P)) Feet = FVector(Doorway, 7.65);
		PutAt(B, Feet);
		BwBoss = B;
		BwRun = FRun();
		BwRun.Start = BwRun.Last = BwRun.Anchor = B->GetActorLocation();
		UE_LOG(LogOutlawsAI, Log, TEXT("OLCOOP bosswall: the host puts %s (copy %d) in the boardwalk doorway at %s, %s"), *B->GetOutlawsName(), B->CoopIndex, *B->GetActorLocation().ToCompactString(), *B->DebugBossState());
		BwStep = 2;
		return;
	}
	case 2:
	{
		AOutlawsEnemy* B = BwBoss.Get();
		if (!B || B->IsDead()) { BwCheck(false, TEXT("the boss is gone")); BwStep = 5; BwAt = Now + 6.0; return; }
		BwRun.Step(Dt, B->CustomTimeDilation, B->GetActorLocation(), IsMoving(B));
		if (BwRun.SeenAt < 0.0 && B->DebugBossState().Contains(TEXT("sees 1"))) BwRun.Seen();
		if (BwRun.Time < 30.0) return;
		const double Free = BwRun.Free();
		BwCheck(Free >= 0.0 && Free <= 6.5 && BwRun.LongestPress <= 3.0 && BwRun.SeenAt >= 0.0, FString::Printf(TEXT("the host's %s: %s, %d slides; now %s"), *B->GetOutlawsName(), *BwRun.Describe(),
			B->DebugSlides(), *B->GetActorLocation().ToCompactString()));
		UE_LOG(LogOutlawsAI, Log, TEXT("OLCOOP bosswall host RESULT %s (%d passed, %d failed)"), BwFails == 0 && BwPasses > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), BwPasses, BwFails);
		BwStep = 5;
		BwAt = Now + 6.0;            // (the client looks at its copy meanwhile)
		return;
	}
	case 5:
		if (Now < BwAt) return;
		TestSay(TEXT("quit"), 0);
		BwAt = Now + 2.0;
		BwStep = 6;
		return;
	case 6:
		if (Now < BwAt) return;
		Summary(TEXT("done"));
		QuitForTest();
		BwStep = 7;
		return;
	// ---- the client: its copy of the boss, once the host has him in the doorway ----
	case 20:
	{
		if (Now < BwAt) return;
		AOutlawsEnemy* B = TheBoss();
		if (!B || B->IsDormant() || FVector::Dist2D(B->GetActorLocation(), FVector(Doorway, 0.0)) > 300.0)
		{
			if (Now > BwAt + 30.0) { BwCheck(false, TEXT("the boss's copy came to the doorway")); BwStep = 22; }
			return;
		}
		BwBoss = B;
		BwRun = FRun();
		BwRun.Start = BwRun.Last = BwRun.Anchor = B->GetActorLocation();
		UE_LOG(LogOutlawsAI, Log, TEXT("OLCOOP bosswall: the client's copy of %s (role %d) is in the doorway at %s"), *B->GetOutlawsName(), B->CoopRole, *B->GetActorLocation().ToCompactString());
		BwStep = 21;
		return;
	}
	case 21:
	{
		AOutlawsEnemy* B = BwBoss.Get();
		if (!B) { BwCheck(false, TEXT("the boss's copy is gone")); BwStep = 22; return; }
		// (the copy's move is the host's, its sub-state not sent: still under 25 cm a second counts within 1.5 m of the
		// corner by the door's leaf where the snapshot's boss stood pressed, UE 8340 -1240; the copy is not paced itself, it
		// follows the host's at the shared pace: EnemyPace)
		BwRun.Step(Dt, L->EnemyPace(), B->GetActorLocation(), !B->IsDead() && FVector::Dist2D(B->GetActorLocation(), FVector(8340.0, -1240.0, 0.0)) < 150.0);
		if (BwRun.Time < 28.0) return;
		const double Free = BwRun.Free();
		BwCheck(Free >= 0.0 && Free <= 6.5 && BwRun.LongestPress <= 3.0, FString::Printf(TEXT("the client's copy of %s: %s; now %s"), *B->GetOutlawsName(), *BwRun.Describe(), *B->GetActorLocation().ToCompactString()));
		BwStep = 22;
		return;
	}
	case 22:
		UE_LOG(LogOutlawsAI, Log, TEXT("OLCOOP bosswall client RESULT %s (%d passed, %d failed)"), BwFails == 0 && BwPasses > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), BwPasses, BwFails);
		BwStep = 23;
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
