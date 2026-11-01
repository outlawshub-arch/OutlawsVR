// BUG-DAMAFTER (user 2026-11-01, round 9 Quest snapshots 3 and 4: after civlwar2's dam blew, the tipped dam top showed
// stretched grass on its sloped part and a thin strip of rock and blue at the bend; the crack hung in the air; and a "use"
// hand showed on the track's face where the TNT went): -OLShots -OLLevel=civlwar2 -OLNoFrontEnd -OLDamAfterProbe
// [-OLOldDamAfter].  (0) every slope elevator's sector (SLOPE_FLOOR / SLOPE_CEILING, BUG-CWBLASTFLOOR) and how its texture
// is laid: a unit along its hinge wall and a unit up the incline, in texels of its texture (the original: along the
// hinge only v moves, 8 texels a unit; up the incline only u, 8 a unit measured along the surface); (1) on civlwar2 the
// dam blown with the big TNT from the track (as -OLTntDamProbe), the sides at their last stop; (2) the hand at the track's
// face where the crack was (DAMBOOM, #1E44F, master off since DAMDUMMY's stop 5): no use offered; the cannon's lock
// (CANBOOM, #17D78, still live) offers one; the crack's wall has no solid part left (snapshot 4, BUG-CWWALL2); (3) the
// walls of each tilted sector: the part each neighbour shows under the tilted edge at its two ends; (4) shots:
// damafter_<new|old>_<snap3|track|right|east>.png, snapshot 3's eye and pose among them.  On any other level (civlwar1,
// shgulch) every slope elevator is sent to its last stop as its trigger does, then (0) and (3) again and a shot of each
// slope (damafter_<tag>_<level>_<sector>.png).  Lines "OLSHOT damafter check PASS|FAIL: ..." and "OLSHOT damafter RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsEnemy.h"
#include "OutlawsGeometry.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLDamAfterProbe) { return OLFlags::Param(TEXT("OLDamAfterProbe")); }

OL_SHOT_TEST(OLDamAfterProbe, 1410)
{
	static int32 Passes = 0, Fails = 0, Boom = -1, Dam = -1, Cannon = -1, NextShot = 0;
	static float Wait = 0.f, UseAt = 0.f;
	static bool bPlaced = false;
	static FVector Start = FVector::ZeroVector;
	static TArray<int32> SlopeSecs;
	if (!L || ShotStep < 0) return;
	const bool bOld = OLFlags::Param(TEXT("OLOldDamAfter"));
	const TCHAR* const Tag = bOld ? TEXT("old") : TEXT("new");
	const FOutlawsInfRuntime* Inf = L->DebugInf();
	const double U = FOutlawsGeometry::UnitsToCm;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT damafter check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	auto SectorNamed = [Inf](const TCHAR* Name) { TArray<int32> S; Inf->FindSectors(Name, FString(), S); return S.Num() ? S[0] : -1; };
	auto Floor = [Inf](int32 s) { return Inf->Live.Sectors.IsValidIndex(s) ? Inf->Live.Sectors[s].Floor.Y : 0.0; };
	auto PutAt = [&](const FVector& Feet, double Yaw, double Pitch)
	{
		TeleportTo(Feet + FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0), FRotator(0.0, Yaw, 0.0), false, true);
		GetCharacterMovement()->Velocity = FVector::ZeroVector;
		if (Controller) Controller->SetControlRotation(FRotator(Pitch, Yaw, 0.0));
	};
	auto Shot = [&](const FString& What)
	{
		const FString Name = FString::Printf(TEXT("damafter_%s_%s"), Tag, *What);
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT damafter shot %s.png"), *Name);
	};
	auto Centroid = [](const FOutlawsSector& S)
	{
		FVector2D C = FVector2D::ZeroVector;
		for (const FVector2D& V : S.Verts) C += V;
		return C / double(FMath::Max(1, S.Verts.Num()));
	};
	// the slope elevators' sectors (each master and slave), floors and ceilings
	auto SlopeSectors = [&]()
	{
		TArray<int32> Out;
		for (int32 e = FMath::Max(0, Inf->SlopesFrom); Inf->SlopesFrom >= 0 && e < Inf->Elevators.Num(); ++e)
		{
			for (int32 s : Inf->Elevators[e].Sectors) Out.AddUnique(s);
		}
		return Out;
	};
	// (0) a slope's texture: a unit along its hinge and a unit up the incline (level, as SurfaceY measures it), in texels
	auto Texture = [&](const TCHAR* When, bool bCount)
	{
		int32 Aligned = 0, N = 0;
		FString Lines;
		for (int32 s : SlopeSecs)
		{
			const FOutlawsSector& S = Inf->Live.Sectors[s];
			for (const bool bFloor : { true, false })
			{
				FVector2D Along, Normal, Hinge;
				double Deg = 0.0;
				if (!Inf->Live.SlopeFrame(S, bFloor, Along, Normal, &Hinge, &Deg)) continue;
				const FOutlawsPlane& Pl = bFloor ? S.Floor : S.Ceil;
				const double W = 64.0, H = 64.0;
				const FVector2D P0 = Centroid(S);
				const FVector2D T0 = Inf->Live.SurfaceUV(S, bFloor, P0, W, H, bOld);
				const FVector2D TA = (Inf->Live.SurfaceUV(S, bFloor, P0 + Along, W, H, bOld) - T0) * FVector2D(W, H);
				const FVector2D TN = (Inf->Live.SurfaceUV(S, bFloor, P0 + Normal, W, H, bOld) - T0) * FVector2D(W, H);
				const double Up = 8.0 / FMath::Max(0.05, FMath::Cos(FMath::DegreesToRadians(Deg)));
				// (within 0.02 texels: the exe's turn is whole steps of 16384 to the turn, so a hinge off the axes sits up to half a
				// step, 0.003 texels a unit, off its own line, as DAMLEFT's does)
				const double Tol = 0.02;
				const bool bOk = FMath::Abs(TA.X) < Tol && FMath::Abs(FMath::Abs(TA.Y) - 8.0) < Tol && FMath::Abs(FMath::Abs(TN.X) - Up) < Tol && FMath::Abs(TN.Y) < Tol;
				Aligned += bOk ? 1 : 0;
				++N;
				Lines += FString::Printf(TEXT(" %s %d(%s) %.1f deg, its plane's angle %.2f, offsets %.2f %.2f: along the hinge u %.2f v %.2f, up the incline u %.2f v %.2f%s;"),
					*S.Name, s, bFloor ? TEXT("floor") : TEXT("ceiling"), Deg, Pl.Angle, Pl.OffX, Pl.OffY, TA.X, TA.Y, TN.X, TN.Y, bOk ? TEXT("") : TEXT(" (turned)"));
			}
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT damafter (0) %s, %s: %d of %d sloped surfaces laid along their hinge:%s"), *L->LevelName, When, Aligned, N, *Lines);
		if (!bCount) return;
		if (bOld) Check(N > 0 && Aligned < N, FString::Printf(TEXT("(0) -OLOldDamAfter: as before, a slope's texture turned by its plane's angle: %d of %d along the hinge"), Aligned, N));
		else Check(N > 0 && Aligned == N, FString::Printf(TEXT("(0) %s: every sloped surface's texture laid along its hinge as the original lays it, the incline at its length: %d of %d"), *L->LevelName, Aligned, N));
	};
	// (3) a tilted sector's walls: what the neighbour shows under the tilted edge (its bottom part) at the wall's two ends
	auto Walls = [&](const TCHAR* When)
	{
		for (int32 s : SlopeSecs)
		{
			const FOutlawsSector& S = Inf->Live.Sectors[s];
			FString Line;
			for (int32 w = 0; w < S.Walls.Num(); ++w)
			{
				const FOutlawsWall& Wl = S.Walls[w];
				const FOutlawsSector* N = Inf->Live.Sector(Wl.Adjoin);
				const FVector2D A = S.Verts[Wl.V1], B = S.Verts[Wl.V2];
				const double EA = Inf->Live.SurfaceY(S, A, true), EB = Inf->Live.SurfaceY(S, B, true);
				if (!N) { Line += FString::Printf(TEXT(" w%d solid, floor %.2f..%.2f;"), w, EA, EB); continue; }
				const double NA = Inf->Live.SurfaceY(*N, A, true), NB = Inf->Live.SurfaceY(*N, B, true);
				Line += FString::Printf(TEXT(" w%d to %d %s: edge %.2f..%.2f, next floor %.2f..%.2f, its bottom part %.2f..%.2f (BOT %d);"),
					w, Wl.Adjoin, *N->Name, EA, EB, NA, NB, FMath::Max(0.0, EA - NA), FMath::Max(0.0, EB - NB), Wl.Bot);
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT damafter (3) %s %s %d:%s"), When, *S.Name, s, *Line);
		}
	};
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT damafter RESULT %s (%d passed, %d failed, %s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails, bOld ? TEXT("-OLOldDamAfter") : TEXT("new"));
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	// a wall's face: its middle and a standing spot 2.5 units in front of it, inside its sector
	auto Facing = [&](int32 s, int32 w, FVector2D& Mid, FVector2D& Stand)
	{
		const FOutlawsSector& S = Inf->Live.Sectors[s];
		const FVector2D A = S.Verts[S.Walls[w].V1], B = S.Verts[S.Walls[w].V2];
		Mid = (A + B) * 0.5;
		FVector2D N = FVector2D(B.Y - A.Y, A.X - B.X).GetSafeNormal();
		if (!Inf->Live.ContainsXZ(S, Mid.X + N.X * 0.3, Mid.Y + N.Y * 0.3)) N = -N;
		Stand = Mid + N * 2.5;
	};
	auto StandAt = [&](int32 s, int32 w)
	{
		FVector2D Mid, Stand;
		Facing(s, w, Mid, Stand);
		const FVector2D To = Mid - Stand;
		PutAt(FOutlawsGeometry::ToUE(Stand, Inf->Live.Sectors[s].Floor.Y), FMath::RadiansToDegrees(FMath::Atan2(To.X, To.Y)), 0.0);
	};
	// the hand at a wall's face (the player standing in front of it, StandAt): Up units over this floor, 5 cm short of it
	auto HandAt = [&](int32 s, int32 w, double Up, FString& Out) -> bool
	{
		const FOutlawsSector& S = Inf->Live.Sectors[s];
		FVector2D Mid, Stand;
		Facing(s, w, Mid, Stand);
		const FVector Eye = FOutlawsGeometry::ToUE(Stand, S.Floor.Y + 5.2);
		const FVector Face = FOutlawsGeometry::ToUE(Mid, S.Floor.Y + Up);
		const FVector Hand = Face + (Eye - Face).GetSafeNormal() * 5.0;
		FVector At, Nrm;
		const bool bGlow = L->HandUseSpot(Eye, Hand, At, Nrm);
		Out = FString::Printf(TEXT("%s %d wall %d (#%s), the player in sector %d, the hand %.1f units over its floor %.2f: %s; a grip there meets: %s"), *S.Name, s, w, *S.Walls[w].Id,
			L->PlayerSectorIndex(), Up, S.Floor.Y, bGlow ? TEXT("a use offered") : TEXT("no use"), *L->DebugDoorProbe(Eye, Hand));
		return bGlow;
	};
	auto WallById = [Inf](int32 s, const TCHAR* Id) { if (s < 0) return -1; const FOutlawsSector& S = Inf->Live.Sectors[s]; for (int32 w = 0; w < S.Walls.Num(); ++w) if (S.Walls[w].Id.Equals(Id, ESearchCase::IgnoreCase)) return w; return -1; };
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	const bool bDam = L->LevelName.Equals(TEXT("civlwar2"), ESearchCase::IgnoreCase);
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 4.f || !bUp || !Inf) return;
		L->bInvincible = true;
		for (const TWeakObjectPtr<AOutlawsEnemy>& E : L->GetEnemies()) if (E.IsValid() && !E->IsDead() && !E->IsDormant()) E->SetDormant(true);
		Start = GetActorLocation();
		SlopeSecs = SlopeSectors();
		Texture(TEXT("at the start"), false);
		if (SlopeSecs.Num() == 0) { Check(false, FString::Printf(TEXT("%s has no slope elevators"), *L->LevelName)); Finish(); return; }
		if (!bDam)
		{
			// another level: every slope elevator sent to its last stop, as its trigger does
			FOutlawsInfRuntime* Run = const_cast<FOutlawsInfRuntime*>(Inf);
			for (int32 e = Inf->SlopesFrom; e < Inf->Elevators.Num(); ++e)
			{
				const FOutlawsElevator& E = Inf->Elevators[e];
				FOutlawsInfMessage M;
				M.Type = EOutlawsInfMsg::GotoStop;
				M.Arg = Inf->Inf.Items[E.Item].Classes[E.ClassIndex].Stops.Num() - 1;
				Run->Send(E.Name, FString(), M);
			}
			Wait = ShotTime + 2.f;
			ShotStep = 30;
			return;
		}
		Boom = SectorNamed(TEXT("DAMBOOM"));
		Dam = SectorNamed(TEXT("DAMDROP"));
		Cannon = SectorNamed(TEXT("CANBOOM"));
		if (Boom < 0 || Dam < 0 || Cannon < 0) { Check(false, FString::Printf(TEXT("civlwar2's DAMBOOM %d, DAMDROP %d, CANBOOM %d"), Boom, Dam, Cannon)); Finish(); return; }
		Walls(TEXT("before"));
		L->CheatJackpot();
		if (WallById(Boom, TEXT("1E44F")) >= 0) StandAt(Boom, WallById(Boom, TEXT("1E44F")));
		Wait = ShotTime + 1.f;
		ShotStep = 1;
		return;
	}
	case 1:
	{
		// (1) the dam blown with the big TNT from the track (the snapshot of -OLTntDamProbe: the crack to the right of the view)
		if (ShotTime < Wait) return;
		FString Before;
		const int32 Crack = WallById(Boom, TEXT("1E44F"));
		const bool bGlow0 = Crack >= 0 && HandAt(Boom, Crack, 8.0, Before);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT damafter (2) before the blast: %s"), *Before);
		const bool bUsed = L->UseItem(TEXT("idynable"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT damafter (1) the big TNT used at the dam: %d; %s (the hand before it: %d)"), bUsed ? 1 : 0, *L->LastItemUse, bGlow0 ? 1 : 0);
		UseAt = ShotTime;
		ShotStep = 2;
		return;
	}
	case 2:
	{
		// the dam's top down and its sides at their last stop
		const float Since = ShotTime - UseAt;
		int32 Done = 0, Sides = 0;
		for (int32 e = FMath::Max(0, Inf->SlopesFrom); Inf->SlopesFrom >= 0 && e < Inf->Elevators.Num(); ++e)
		{
			const FOutlawsElevator& E = Inf->Elevators[e];
			if (!E.Name.StartsWith(TEXT("DAM"))) continue;
			++Sides;
			Done += E.bTerminated && E.Target < 0 ? 1 : 0;
		}
		const bool bDown = FMath::Abs(Floor(Dam) + 26.5) < 0.01 && Sides > 0 && Done == Sides;
		if (!bDown && Since < 60.f) return;
		Check(bDown, FString::Printf(TEXT("(1) the dam blown: its top (DAMDROP) %.2f, its sides (DAMLEFT, DAMRIGHT) at their last stop %d of %d, %.1f s after the TNT"), Floor(Dam), Done, Sides, Since));
		Texture(TEXT("the dam blown"), true);
		Walls(TEXT("after"));
		// (2) the hand where the crack was: the track's face onto DAMDROP, now level with it.  At 0 the grip lands on the
		// seam in the floor, the line's wall still in reach of it (WallsAt): the spot the hand offered a use after the blast
		const int32 Crack = WallById(Boom, TEXT("1E44F"));
		FString Hand;
		bool bGlow = false;
		for (const double Up : { 0.0, 1.0, 3.0, 6.0 })
		{
			FString One;
			const bool bOne = Crack >= 0 && HandAt(Boom, Crack, Up, One);
			bGlow |= bOne;
			Hand += One + TEXT(" | ");
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT damafter (2) after the blast: %s"), *Hand);
		// (the old rule does not bring the hand back here either: the face has no solid part left and the seam is DAMDROP's
		// lift floor, which offers no use, so -OLOldDamAfter only logs it; the master rule is FireTriggers' own)
		if (bOld) { UE_LOG(LogOutlaws, Display, TEXT("OLSHOT damafter (2) -OLOldDamAfter: the hand at the track's face where the crack was: %s"), bGlow ? TEXT("a use offered") : TEXT("no use")); }
		else Check(Crack >= 0 && !bGlow, TEXT("(2) the hand offers no use at the track's face where the crack was (DAMBOOM's triggers: the SINGLE fired, the line MASTER_OFF)"));
		const FOutlawsSector& Bs = Inf->Live.Sectors[Boom];
		const FOutlawsWall& Cw = Bs.Walls[FMath::Max(0, Crack)];
		Check(Crack >= 0 && Cw.Adjoin == Dam && Floor(Dam) <= Floor(Boom) + 1e-6, FString::Printf(TEXT("(2) snapshot 4's crack: its wall (#%s, OVERLAY %d) has no solid part left to paint it on (the track %.2f, DAMDROP %.2f; BUG-CWWALL2 paints a sign on solid parts only)"),
			*Cw.Id, Cw.Overlay, Floor(Boom), Floor(Dam)));
		const int32 Lock = WallById(Cannon, TEXT("17D78"));
		if (Lock >= 0) StandAt(Cannon, Lock);
		Wait = ShotTime + 1.f;
		ShotStep = 3;
		return;
	}
	case 3:
	{
		if (ShotTime < Wait) return;
		FString Ctl;
		const int32 Lock = WallById(Cannon, TEXT("17D78"));
		// (the lock's face is the 2.5-unit step up to sector 642, floor 0.5 to 3.0: the hand on its middle)
		const bool bCtl = Lock >= 0 && HandAt(Cannon, Lock, 1.25, Ctl);
		Check(bCtl, FString::Printf(TEXT("(2) a live lock still offers a use: %s"), *Ctl));
		NextShot = 0;
		bPlaced = false;
		ShotStep = 10;
		return;
	}
	case 10:
	{
		// (4) the views: snapshot 3's feet and pose; the track looking at the dam (the TNT spot); the right side from the
		// track; the east end of the dam's top looking west along it
		if (ShotTime < Wait) return;
		struct FView { const TCHAR* Name; FVector Feet; double Yaw, Pitch; };
		static const FView Views[] = {
			{ TEXT("snap3"), FVector(-1872.970, 11799.231, -390.0), -80.103, -26.264 },
			{ TEXT("track"), FVector(-2040.685, 11255.092, -795.0), -64.03, -1.1 },
			{ TEXT("right"), FVector(-1980.0, 11100.0, -795.0), 54.5, -8.0 },
			{ TEXT("east"), FVector(-1740.0, 12540.0, -390.0), -90.0, -20.0 } };
		if (NextShot >= int32(UE_ARRAY_COUNT(Views))) { Finish(); return; }
		const FView& V = Views[NextShot];
		if (!bPlaced)
		{
			PutAt(V.Feet, V.Yaw, V.Pitch);
			bPlaced = true;
			Wait = ShotTime + 0.8f;
			return;
		}
		const FVector Eye = FOutlawsGeometry::ToOutlaws(Camera->GetComponentLocation());
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT damafter (4) %s: the eye at game x %.2f y %.2f z %.2f, sector %d"), V.Name, Eye.X, Eye.Y, Eye.Z, L->PlayerSectorIndex());
		Shot(V.Name);
		++NextShot;
		bPlaced = false;
		Wait = ShotTime + 0.6f;
		return;
	}
	case 30:
	{
		if (ShotTime < Wait || !bUp) return;
		Texture(TEXT("every slope at its last stop"), true);
		Walls(TEXT("after"));
		NextShot = 0;
		bPlaced = false;
		ShotStep = 31;
		return;
	}
	case 31:
	{
		// a shot of each slope from 10 units off its middle towards the start, on the floor there
		if (ShotTime < Wait) return;
		if (NextShot >= SlopeSecs.Num()) { Finish(); return; }
		const FOutlawsSector& S = Inf->Live.Sectors[SlopeSecs[NextShot]];
		const FVector2D C = Centroid(S);
		const FVector At = FOutlawsGeometry::ToUE(C, Inf->Live.SurfaceY(S, C, S.SlopeFloor.bSet || !S.SlopeCeil.bSet));
		if (!bPlaced)
		{
			FVector Feet = At + (FVector(Start.X, Start.Y, At.Z) - At).GetSafeNormal() * (10.0 * U);
			const FOutlawsSector* Under = Inf->Live.SectorAt(FOutlawsGeometry::ToOutlaws(FVector(Feet.X, Feet.Y, At.Z + 60.0)));
			Feet.Z = Under ? Under->Floor.Y * U : At.Z;
			const FRotator R = (At - (Feet + FVector(0.0, 0.0, 170.0))).Rotation();
			PutAt(Feet, R.Yaw, R.Pitch);
			bPlaced = true;
			Wait = ShotTime + 0.8f;
			return;
		}
		Shot(FString::Printf(TEXT("%s_%s"), *L->LevelName.ToLower(), *S.Name.ToLower()));
		++NextShot;
		bPlaced = false;
		Wait = ShotTime + 0.6f;
		return;
	}
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
