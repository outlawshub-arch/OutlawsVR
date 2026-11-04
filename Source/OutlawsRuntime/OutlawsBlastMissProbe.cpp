// BUG-BLASTMISS's probe (the user, 3 Nov: in matches a stick went off right next to a bot several times and the bot took no
// damage and wasn't thrown; is the blast's hit test weak, across sectors?).  It logs and changes nothing:
//   -OLShots -OLMpHost=<map> -OLMpClass=dm -OLMpBots=7 -OLMpBotSkill=1 -OLMpStartWhen=0 -OLMpMinutes=10 -OLMpFrags=0 -OLBotThrowMany
//   -OLBlastMissProbe [-OLBlastMissSeconds=90] [-OLBlastMissRate=24]
// Round the living bots, some sites a tick with nothing set off: a stick lying in the open, lying against a wall, lying at a step's
// lip on either side.  And every real blast of the match on every body (the hook in BlastAtPlayers, GOLBlastMissNote).  Each case
// is read four ways:
//   ours    the game's test: within the radius and CanSee's one ray, stick + 20 cm to the body's middle; the first thing the ray
//           meets when it is stopped (actor, component, floor/ceil/side, how far from the stick).
//   orig    the original's test, 16 tries: the range gate (the plan's octagon distance from the stick, RADIUS) and the sector walk,
//           jitter and all [EXE 0x1402cd0a0, 0x1402cc8e0, 0x1402ccc90, 0x1402dd070]: from the stick's top less 0..2 u, 0..1 u
//           off in plan, to the body's top less 0..2 u, 0..1.5 u off.  Walls crossed by adjoin only, the height at each crossing within
//           both sectors' floor and ceiling.  (The exe's visible-sector shortlist 0x1402ccf80 is not ported: a little kinder.)
//   rays3   three rays from stick + 30 cm: to the feet, the middle and the head; any clear.
//   head    the one ray from stick + 30 cm to the head.
// Lines: "OLSHOT blastmiss real ..." a blast a body, "OLSHOT blastmiss miss ..." for the first sites ours stops and the original
// passes, the map's tables ("table", "blocker", "orig why"), then "OLSHOT blastmiss RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsLevel.h"
#include "OutlawsCoop.h"
#include "OutlawsGeometry.h"
#include "OutlawsMpHost.h"
#include "OutlawsRemotePlayer.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

extern void (*GOLBlastMissNote)(AOutlawsLevelActor* L, const FVector& From, const AOutlawsRemotePlayer* Body, double R, float Damage);

namespace OutlawsBlastMissProbeLocal
{
	enum ESite : int32 { Open, Wall, Step, Real, NumSites };
	const TCHAR* const SiteNames[] = { TEXT("open"), TEXT("wall"), TEXT("step"), TEXT("real") };
	constexpr int32 NumBands = 3, OrigTries = 16;
	const TCHAR* const BandNames[] = { TEXT("under 3 m"), TEXT("3 to 7.5 m"), TEXT("7.5 to 15 m") };

	struct FTally { int32 N = 0, OursBlocked = 0, OursBlockedOrigAll = 0, OursBlockedOrigSome = 0, OursClearOrigNone = 0, Rays3Clear = 0, HeadClear = 0, BlockedRays3 = 0; double OrigHits = 0.0; };
	FTally Tally[NumSites][NumBands];
	TMap<FString, int32> Blockers;       // (what stopped our ray where the original's passes at least half its tries)
	TMap<FString, int32> AllBlockers;    // (what stopped our ray, any case)
	int32 OrigWhy[5] = {};               // (0 passed, 1 a solid wall, 2 over the ceiling, 3 under the floor, 4 ended in another sector)
	int32 Stage = 0, Bot = 0, MissLines = 0, RealLines = 0, NoSector = 0;
	double StartAt = 0.0, Credit = 0.0;
	FRandomStream Rng(2190);

	double Oct(double Dx, double Dz)
	{
		Dx = FMath::Abs(Dx);
		Dz = FMath::Abs(Dz);
		return Dx + Dz - 0.5 * FMath::Min(Dx, Dz);
	}

	/** The sector walk [EXE 0x1402ccc90, its step 0x1402dd070, the crossing 0x1402df970]: 0 clear, else OrigWhy's reason. */
	int32 Walk(const FOutlawsLevel& Lv, int32 A, int32 B, const FVector& P0, const FVector& P1)
	{
		const double Dx = P1.X - P0.X, Dz = P1.Z - P0.Z;
		if (Dx == 0.0 && Dz == 0.0) return A == B ? 0 : 4;
		const double Slope = (P1.Y - P0.Y) / Oct(Dx, Dz);
		TSet<uint64> Stamped;
		int32 Cur = A;
		double Last = -2147483648.0;
		const FVector2D P(P0.X, P0.Z), Seg(Dx, Dz);
		for (int32 Hop = 0; Hop < 512; ++Hop)
		{
			const FOutlawsSector& S = Lv.Sectors[Cur];
			double Best = 2147483648.0;
			int32 BestW = -1;
			for (int32 w = 0; w < S.Walls.Num(); ++w)
			{
				const FOutlawsWall& W = S.Walls[w];
				if (Stamped.Contains((uint64(Cur) << 32) | uint32(w)) || !S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) continue;
				const FVector2D Q1 = S.Verts[W.V1], E = S.Verts[W.V2] - Q1;
				const double Den = FVector2D::CrossProduct(Seg, E);
				if (FMath::Abs(Den) < 1e-12) continue;
				const FVector2D QP = Q1 - P;
				const double T = FVector2D::CrossProduct(QP, E) / Den, U = FVector2D::CrossProduct(QP, Seg) / Den;
				if (T < 0.0 || T > 1.0 || U < 0.0 || U > 1.0) continue;
				const double At = Oct(Seg.X * T, Seg.Y * T);
				if (At < Best && At > Last) { Best = At; BestW = w; }
			}
			if (BestW < 0) return Cur == B ? 0 : 4;
			const FOutlawsWall& W = S.Walls[BestW];
			Stamped.Add((uint64(Cur) << 32) | uint32(BestW));
			Last = Best;
			if (!Lv.Sectors.IsValidIndex(W.Adjoin)) return 1;
			if (W.Mirror >= 0) Stamped.Add((uint64(W.Adjoin) << 32) | uint32(W.Mirror));
			if (Lv.Sectors.IsValidIndex(W.DAdjoin) && W.DMirror >= 0) Stamped.Add((uint64(W.DAdjoin) << 32) | uint32(W.DMirror));
			const double Y = P0.Y + Best * Slope;
			const FVector2D Hit = P + Seg * (Oct(Dx, Dz) > 0.0 ? Best / Oct(Dx, Dz) : 0.0);
			int32 Next = W.Adjoin;
			if (Lv.Sectors.IsValidIndex(W.DAdjoin) && Lv.SurfaceY(Lv.Sectors[Next], Hit, true) > Y) Next = W.DAdjoin;
			const FOutlawsSector& N = Lv.Sectors[Next];
			if (Y > Lv.SurfaceY(S, Hit, false) || Y > Lv.SurfaceY(N, Hit, false)) return 2;
			if (Lv.SurfaceY(S, Hit, true) > Y || Lv.SurfaceY(N, Hit, true) > Y) return 3;
			Cur = Next;
		}
		return 4;
	}

	/** One try of the test [EXE 0x1402cc8e0, height 2 x COLLIDE_RADIUS 0x1400cffb4]. */
	int32 OrigTry(const FOutlawsLevel& Lv, int32 A, int32 B, const FVector& Stick, const FVector& Feet, double Tall)
	{
		const double R0 = Rng.FRandRange(0.f, 1.f), R1 = Rng.FRandRange(0.f, 1.5f), A0 = Rng.FRandRange(0.f, 6.2832f), A1 = Rng.FRandRange(0.f, 6.2832f);
		const FVector P0(Stick.X + FMath::Cos(A0) * R0, Stick.Y + 2.0 - Rng.FRandRange(0.f, 2.f), Stick.Z + FMath::Sin(A0) * R0);
		const FVector P1(Feet.X + FMath::Cos(A1) * R1, Feet.Y + Tall - Rng.FRandRange(0.f, 2.f), Feet.Z + FMath::Sin(A1) * R1);
		return Walk(Lv, A, B, P0, P1);
	}

	bool Blocked(UWorld* W, const AActor* Pawn, const AActor* Target, const FVector& From, const FVector& To, FHitResult& Hit)
	{
		// (CanSee's own query: the player's pawn and the target left out, pawns ignored)
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsBlastMiss), true);
		if (Pawn) Q.AddIgnoredActor(Pawn);
		if (Target) Q.AddIgnoredActor(Target);
		FCollisionResponseParams RP;
		RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
		return W->LineTraceSingleByChannel(Hit, From, To, ECC_Visibility, Q, RP);
	}

	FString BlockerName(const FHitResult& Hit)
	{
		const AActor* A = Hit.GetActor();
		const UPrimitiveComponent* C = Hit.GetComponent();
		const double Nz = Hit.ImpactNormal.Z;
		return FString::Printf(TEXT("%s/%s/%s"), A ? *A->GetClass()->GetName() : TEXT("none"), C ? *C->GetName() : TEXT("none"), Nz > 0.6 ? TEXT("floor") : Nz < -0.6 ? TEXT("ceiling") : TEXT("side"));
	}

	void Case(AOutlawsLevelActor* L, int32 Site, const FVector& Stick, const AOutlawsRemotePlayer* Rp, double R, float Damage)
	{
		UWorld* W = L->GetWorld();
		const FOutlawsLevel* Lv = L->LiveLevel();
		if (!W || !Lv) return;
		const FVector From = Stick + FVector(0.0, 0.0, Site == Real ? 0.0 : 20.0);    // (a real blast: BlastAtPlayers' From, 20 cm up already)
		const FVector StickAt = Site == Real ? Stick - FVector(0.0, 0.0, 20.0) : Stick;
		const FVector Mid = Rp->ShownBody();
		const double Half = FMath::Max(40.0, double(Rp->ShownHalf()));
		const FVector Feet = Mid - FVector(0.0, 0.0, Half);
		const double D = FVector::Dist(From, Mid);
		const FVector SU = FOutlawsGeometry::ToOutlaws(StickAt), FU = FOutlawsGeometry::ToOutlaws(Feet);
		const int32 A = Lv->Floor3D(SU.X, SU.Y, SU.Z), B = Lv->Floor3D(FU.X, FU.Y, FU.Z);
		if (A < 0 || B < 0) { ++NoSector; return; }
		const bool bOrigReach = Oct(SU.X - FU.X, SU.Z - FU.Z) < R / FOutlawsGeometry::UnitsToCm;
		if (D >= R && !bOrigReach) return;
		const APlayerController* PC = W->GetFirstPlayerController();
		const AActor* Pawn = PC ? PC->GetPawn() : nullptr;
		FHitResult Hit;
		const bool bStopped = D >= R || Blocked(W, Pawn, Rp, From, Mid, Hit);
		int32 Passes = 0;
		if (bOrigReach)
		{
			for (int32 t = 0; t < OrigTries; ++t)
			{
				const int32 Why = OrigTry(*Lv, A, B, SU, FU, 2.0 * Half / FOutlawsGeometry::UnitsToCm);
				++OrigWhy[Why];
				Passes += Why == 0 ? 1 : 0;
			}
		}
		const FVector Up = StickAt + FVector(0.0, 0.0, 30.0);
		FHitResult Unused;
		const bool bHead = !Blocked(W, Pawn, Rp, Up, Feet + FVector(0.0, 0.0, 2.0 * Half - 15.0), Unused);
		const bool bRays3 = bHead || !Blocked(W, Pawn, Rp, Up, Mid, Unused) || !Blocked(W, Pawn, Rp, Up, Feet + FVector(0.0, 0.0, 20.0), Unused);
		const int32 Band = D < 300.0 ? 0 : D < 750.0 ? 1 : 2;
		FTally& T = Tally[Site][Band];
		++T.N;
		T.OrigHits += double(Passes) / OrigTries;
		T.Rays3Clear += bRays3 ? 1 : 0;
		T.HeadClear += bHead ? 1 : 0;
		const double Fall = D <= R * 0.25 ? 1.0 : FMath::Square(FMath::Max(0.0, (R - D) / (0.75 * R)));
		const FString Name = !bStopped ? FString() : D >= R ? FString(TEXT("(out of our reach)")) : BlockerName(Hit);
		if (bStopped)
		{
			++T.OursBlocked;
			T.BlockedRays3 += bRays3 ? 1 : 0;
			T.OursBlockedOrigAll += Passes == OrigTries ? 1 : 0;
			T.OursBlockedOrigSome += Passes > 0 && Passes < OrigTries ? 1 : 0;
			AllBlockers.FindOrAdd(Name)++;
			if (Passes * 2 >= OrigTries) Blockers.FindOrAdd(Name)++;
		}
		else if (Passes == 0) ++T.OursClearOrigNone;
		const double FromStick = bStopped && D < R ? FVector::Dist(FVector(Hit.ImpactPoint), StickAt) : 0.0;
		if (Site == Real && RealLines < 400)
		{
			++RealLines;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT blastmiss real: D %.0f cm of R %.0f, dealt %.2f%s, ours %s%s, orig %d/%d, rays3 %s, head %s"), D, R, Damage * Fall, bStopped ? TEXT(" (not dealt)") : TEXT(""),
				bStopped ? TEXT("stopped by ") : TEXT("clear"), *Name, Passes, OrigTries, bRays3 ? TEXT("clear") : TEXT("stopped"), bHead ? TEXT("clear") : TEXT("stopped"));
		}
		else if (Site != Real && bStopped && Passes * 2 >= OrigTries && MissLines < 60)
		{
			++MissLines;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT blastmiss miss (%s): stick %s, body %s, D %.0f, would deal %.2f, stopped by %s %.0f cm from the stick, at %s; orig %d/%d, rays3 %s, same sector %s"), SiteNames[Site], *StickAt.ToCompactString(), *Mid.ToCompactString(), D, Damage * Fall,
				*Name, FromStick, *FVector(Hit.ImpactPoint).ToCompactString(), Passes, OrigTries, bRays3 ? TEXT("clear") : TEXT("stopped"), A == B ? TEXT("yes") : TEXT("no"));
		}
	}

	void Note(AOutlawsLevelActor* L, const FVector& From, const AOutlawsRemotePlayer* Body, double R, float Damage)
	{
		Case(L, Real, From, Body, R, Damage);
	}

	/** A stick lying on what is under Try: true with where it lies (3 cm off the floor, as a thrown one comes to rest). */
	bool Rest(UWorld* W, const FVector& Try, FVector& Out)
	{
		FHitResult Hit;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsBlastMissRest), true);
		if (!W->LineTraceSingleByChannel(Hit, Try + FVector(0.0, 0.0, 120.0), Try - FVector(0.0, 0.0, 500.0), ECC_Visibility, Q) || Hit.ImpactNormal.Z < 0.6) return false;
		Out = FVector(Hit.ImpactPoint) + FVector(0.0, 0.0, 3.0);
		return true;
	}

	bool PickSite(AOutlawsLevelActor* L, int32 Kind, const FVector& Feet, FVector& Out)
	{
		UWorld* W = L->GetWorld();
		const double Ang = Rng.FRandRange(0.f, 6.2832f);
		const FVector Dir(FMath::Cos(Ang), FMath::Sin(Ang), 0.0);
		const FVector Low = Feet + FVector(0.0, 0.0, 30.0);
		if (Kind == Open) return Rest(W, Low + Dir * Rng.FRandRange(60.f, 900.f), Out);
		if (Kind == Wall)
		{
			FHitResult Hit;
			FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsBlastMissRest), true);
			if (!W->LineTraceSingleByChannel(Hit, Low, Low + Dir * 900.0, ECC_Visibility, Q) || FMath::Abs(Hit.ImpactNormal.Z) > 0.3) return false;
			return Rest(W, FVector(Hit.ImpactPoint) + FVector(Hit.ImpactNormal) * 8.0, Out);
		}
		// a lip: walking out along Dir, the first floor 8 to 120 cm from the one before; the stick 10 cm to one side of it
		FVector Was;
		if (!Rest(W, Low, Was)) return false;
		for (double S = 30.0; S < 700.0; S += 30.0)
		{
			FVector Here;
			if (!Rest(W, Low + Dir * S, Here)) return false;
			const double Rise = FMath::Abs(Here.Z - Was.Z);
			if (Rise >= 8.0 && Rise <= 120.0) return Rest(W, Low + Dir * (S - 15.0 + (Rng.FRand() < 0.5f ? -25.0 : 25.0)), Out);
			Was = Here;
		}
		return false;
	}
}

OL_SHOT_MATCH(OLBlastMissProbe) { return OLFlags::Param(TEXT("OLBlastMissProbe")); }

OL_SHOT_TEST(OLBlastMissProbe, 2190)
{
	using namespace OutlawsBlastMissProbeLocal;
	if (!L || Stage < 0) return;
	UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	L->StartSaveDelay = -1.f;           // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	L->bEnemiesActive = true;           // (ShotTest stills them each tick: the bots' brains are bad guys')
	UOutlawsCoop* C = UOutlawsCoop::Get(this);
	const bool bUp = C && C->IsHost() && C->IsMatch() && C->bLevelBuilt && !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay();
	int32 Seconds = 90, Rate = 24;
	OLFlags::Value(TEXT("OLBlastMissSeconds"), Seconds);
	OLFlags::Value(TEXT("OLBlastMissRate"), Rate);
	const FString Lv = L->LevelName.ToLower();
	if (Stage == 0)
	{
		if (StartAt == 0.0) StartAt = FPlatformTime::Seconds();
		if (!bUp)
		{
			if (FPlatformTime::Seconds() - StartAt < 180.0) return;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT blastmiss RESULT FAIL: needs a launched host with bots: -OLMpHost=<map> -OLMpClass=dm -OLMpBots=7"));
			Stage = -1;
			FGenericPlatformMisc::RequestExit(false);
			return;
		}
		TArray<UOutlawsCoop::FBotProbeView> Views;
		C->DebugBotViews(Views);
		if (Views.Num() == 0) return;
		FOutlawsMatchFlow& F = OutlawsMpHost::Flow();
		if (F.GetState() == FOutlawsMatchFlow::EState::WarmUp) F.HostStart();
		GOLBlastMissNote = &OutlawsBlastMissProbeLocal::Note;
		StartAt = Now;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT blastmiss watching %d bots on %s for %d s, %d sites a second; radius %.0f cm"), Views.Num(), *Lv, Seconds, Rate, L->BlastRadiusCm());
		Stage = 1;
		return;
	}
	if (!bUp) return;
	if (Now - StartAt < Seconds)
	{
		TArray<UOutlawsCoop::FBotProbeView> Views;
		C->DebugBotViews(Views);
		Credit += Dt * Rate;
		while (Credit >= 1.0 && Views.Num() > 0)
		{
			Credit -= 1.0;
			const UOutlawsCoop::FBotProbeView& V = Views[Bot++ % Views.Num()];
			const AOutlawsRemotePlayer* R = C->PuppetOf(V.Who);
			if (V.bDead || !R || R->IsHidden() || R->PlateDead()) continue;
			const FVector Feet = R->ShownBody() - FVector(0.0, 0.0, FMath::Max(40.0, double(R->ShownHalf())));
			const int32 Kind = Rng.RandRange(0, 2);
			FVector Stick;
			if (PickSite(L, Kind, Feet, Stick)) Case(L, Kind, Stick, R, L->BlastRadiusCm(), 12.f);
		}
		return;
	}
	GOLBlastMissNote = nullptr;
	int32 Counted = 0, Stopped = 0;
	for (int32 s = 0; s < NumSites; ++s)
	{
		for (int32 b = 0; b < NumBands; ++b)
		{
			const FTally& T = Tally[s][b];
			Counted += T.N;
			Stopped += T.OursBlocked;
			if (T.N == 0) continue;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT blastmiss table %s, %s, %s: %d cases; ours stopped %d (%.1f%%); of those the original passes all tries %d, some %d; orig expected hits %.1f%%; ours clear and orig never %d; rays3 clear %.1f%% (saves %d of ours stopped), head clear %.1f%%"),
				*Lv, SiteNames[s], BandNames[b], T.N, T.OursBlocked, 100.0 * T.OursBlocked / T.N, T.OursBlockedOrigAll, T.OursBlockedOrigSome, 100.0 * T.OrigHits / T.N, T.OursClearOrigNone,
				100.0 * T.Rays3Clear / T.N, T.BlockedRays3, 100.0 * T.HeadClear / T.N);
		}
	}
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		TMap<FString, int32>& M = Pass == 0 ? Blockers : AllBlockers;
		M.ValueSort([](int32 A, int32 B) { return A > B; });
		int32 n = 0;
		for (const TPair<FString, int32>& Kv : M)
		{
			if (++n > 12) break;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT blastmiss blocker %s %s: %d"), Pass == 0 ? TEXT("(the original passes)") : TEXT("(all)"), *Kv.Key, Kv.Value);
		}
	}
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT blastmiss orig why %s: passed %d, solid wall %d, over a ceiling %d, under a floor %d, ended elsewhere %d; no sector %d"), *Lv, OrigWhy[0], OrigWhy[1], OrigWhy[2], OrigWhy[3], OrigWhy[4], NoSector);
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT blastmiss RESULT logged: %d cases on %s, ours stopped %d, real %d"), Counted, *Lv, Stopped, RealLines);
	Stage = -1;
	FGenericPlatformMisc::RequestExit(false);
}

#endif // WITH_OUTLAWS_TESTS
