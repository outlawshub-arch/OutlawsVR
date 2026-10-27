// BUG-SLOPECLIMB: -OLClimbProbe drives climbs on the desktop as a hand makes them (any build, Shipping too, as -OLShipSmoke:
// through OLFlags, not under WITH_OUTLAWS_TESTS; without the flag ClimbProbeOn is false and nothing runs); see OutlawsPlayer.h
// (TickClimbProbe).  The headset's body (bVR, the head 170 cm over the play-space floor, as -OLPauseZTest has it), so a climb
// into a low space crouches as it does on the Quest.  Each case: the body stood on the ground by the ledge, raised against
// its face (so the tracking follows it), the right hand's grip put on the lip and the ledge taken (TryGrab), then let go at
// chest height (ReleaseGrab, which climbs when there is room), and where the body is a moment on.
//   -OLClimbProbe[=townex+mill] (the checks, PASS or FAIL each):
//     townex: every sloped roof 0.6 to 6 m over the ground beside it with standing room on it, from its lowest edge (the
//             eave), a roof over a room among them (555 over 556, the user's 24 Oct climb); the window 204 (glass) it must
//             not climb through into 550
//     mill:   BUG-MILLCLIMB's climbs in the hall (box 108 from the hall's floor, the walkway 572 from the box and from the
//             floor: never thrown into the yard), then a landing in the yard by the doorway refused by the climb's own rule
//   -OLClimbProbe=survey[:level+level] (every story level by default): of each sector, its lowest edge 0.6 to 6 m over the
//     ground beside it (sloped roofs first, then the rest spread over the level, -OLClimbProbeMax=N a level, 60), climbed;
//     a line each (climbed, vaulted, fell off, refused and why, no ledge) and a count a level, to set a run beside another

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsLevel.h"
#include "OutlawsGeometry.h"
#include "OutlawsLevelActor.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "MotionControllerComponent.h"
#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"

namespace
{
	enum class EClimbWant : uint8 { Roof, OnTop, Refuse, LandCheck, Survey };
	struct FClimbCase
	{
		FString What;
		FString Id;                                // (survey) s<sector>w<wall>
		FVector StandFeet = FVector::ZeroVector;   // UE: put there first (a teleport)
		bool bStep = false;
		FVector StepFeet = FVector::ZeroVector;    // then walked there, 12 cm a tick
		FVector Lip = FVector::ZeroVector;         // UE: the ledge's top at its edge (the wall)
		FVector In = FVector::ForwardVector;       // UE: into the ledge
		EClimbWant Want = EClimbWant::Roof;
		double WantFeetZ = 0.0;                    // (OnTop) UE
		TArray<int32> Sectors;                     // (OnTop) where on top counts; (Refuse) where it must not end
		bool bRoof = false;                        // (survey) a sloped top rising from the edge with standing room
	};
	struct FSurveyCount { int32 Cases = 0, Roofs = 0, Climbed = 0, Vaulted = 0, FellOff = 0, Refused = 0, RoofRefused = 0, NoLedge = 0, BadSpot = 0; TMap<FString, int32> Why; };
	struct FClimbProbe
	{
		TArray<FString> Levels;
		bool bSurvey = false;
		int32 LevelI = 0;
		TWeakObjectPtr<AOutlawsLevelActor> Level;
		FString LevelName;
		TArray<FClimbCase> Cases;
		int32 CaseI = -1, Step = 0;
		float T = 0.f, At = 0.f;
		int32 Pass = 0, Fail = 0, Skip = 0;
		bool bArmed = false, bStarted = false, bVault = false, bAsked = false, bDone = false;
		FString Grab, Refusal;
		FVector Hang = FVector::ZeroVector;
		FSurveyCount Count;
	};
	FClimbProbe GClimb;

	constexpr double ProbeU = FOutlawsGeometry::UnitsToCm;
	constexpr float ProbeStandHalf = 87.f;           // (OutlawsPlayer.cpp's StandHalfHeight)
	const TCHAR* const StoryLevels = TEXT("hideout+townex+train+canyon+mill+simms+miner+cliff+ranch");

	/** The ledges of a level: of each sector, its lowest edge 0.6 to 6 m over the ground beside it, with room over the lip
	 *  on the ground's side to hang there.  bRoofsOnly: sloped tops rising away from the edge with standing room (2 m)
	 *  over them only. */
	void LedgeCases(const FOutlawsLevel& Lv, TArray<FClimbCase>& Out, bool bRoofsOnly)
	{
		for (const FOutlawsSector& S : Lv.Sectors)
		{
			if (S.Verts.Num() < 3 || (S.Flags & OutlawsFlags::SectorDoor)) continue;
			const bool bSloped = S.SlopeFloor.bSet && (S.Flags & OutlawsFlags::SlopedFloor);
			const double Deg = bSloped ? FMath::Abs(S.SlopeFloor.Angle) * (90.0 / 4096.0) : 0.0;
			if (bRoofsOnly && (!bSloped || Deg < 3.0 || Deg > 40.0)) continue;
			if (Deg > 50.0) continue;
			double BestRise = 1e9;
			FClimbCase Best;
			int32 BestGround = -1;
			for (int32 k = 0; k < S.Walls.Num(); ++k)
			{
				const FOutlawsWall& W = S.Walls[k];
				if (!S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) continue;
				if (bRoofsOnly && (W.Flags1 & (OutlawsFlags::WallNoPass | OutlawsFlags::WallMidTex))) continue;
				const FVector2D A = S.Verts[W.V1], B = S.Verts[W.V2];
				if (FVector2D::Distance(A, B) * ProbeU < 90.0) continue;
				const FVector2D M = (A + B) * 0.5;
				FVector2D N = FVector2D(-(B - A).Y, (B - A).X).GetSafeNormal();
				if (!Lv.ContainsXZ(S, M.X + N.X * 0.25, M.Y + N.Y * 0.25)) N = -N;
				if (!Lv.ContainsXZ(S, M.X + N.X * 0.25, M.Y + N.Y * 0.25)) continue;
				const double LipY = Lv.SurfaceY(S, M, true);
				const double Rising = Lv.SurfaceY(S, M + N, true) - LipY;
				const double Room = (Lv.SurfaceY(S, M + N * 2.0, false) - Lv.SurfaceY(S, M + N * 2.0, true)) * ProbeU;
				if (Room <= 0.0 || (bRoofsOnly && (Rising < 0.1 || Room < 200.0))) continue;
				for (const int32 Gi : { W.Adjoin, W.DAdjoin })
				{
					if (!Lv.Sectors.IsValidIndex(Gi) || Gi == S.Index) continue;
					const FOutlawsSector& G = Lv.Sectors[Gi];
					const FVector2D Outside = M - N * 0.6;
					if (!Lv.ContainsXZ(G, Outside.X, Outside.Y)) continue;
					const double GroundY = Lv.SurfaceY(G, Outside, true), GroundC = Lv.SurfaceY(G, Outside, false);
					const double Rise = (LipY - GroundY) * ProbeU;
					if (Rise < 60.0 || Rise > 600.0 || (GroundC - LipY) * ProbeU < 150.0) continue;
					if (Rise >= BestRise) continue;
					BestRise = Rise;
					BestGround = G.Index;
					Best = FClimbCase();
					Best.Want = bRoofsOnly ? EClimbWant::Roof : EClimbWant::Survey;
					Best.Id = FString::Printf(TEXT("s%dw%d"), S.Index, k);
					Best.bRoof = bSloped && Deg >= 3.0 && Deg <= 40.0 && Rising >= 0.1 && Room >= 200.0;
					Best.Lip = FOutlawsGeometry::ToUE(M, LipY);
					Best.In = FOutlawsGeometry::ToUE(N.X, 0.0, N.Y).GetSafeNormal();
					Best.StandFeet = FOutlawsGeometry::ToUE(M - N * 2.0, GroundY);
					Best.Sectors = { S.Index };
					Best.What = FString::Printf(TEXT("%s %d (%s%s%s) from sector %d, %.0f cm up, %.0f cm of room on it"), Best.bRoof ? TEXT("roof") : bSloped ? TEXT("slope") : TEXT("ledge"),
						S.Index, bSloped ? *FString::Printf(TEXT("%.0f deg"), Deg) : TEXT("flat"), (W.Flags1 & OutlawsFlags::WallNoPass) ? TEXT(", nopass") : TEXT(""),
						(W.Flags1 & OutlawsFlags::WallMidTex) ? TEXT(", mid") : TEXT(""), G.Index, Rise, Room);
				}
			}
			if (BestGround >= 0) Out.Add(Best);
		}
	}

	void BuildCases(const FString& LevelName, const FOutlawsLevel& Lv, bool bSurvey, TArray<FClimbCase>& Out)
	{
		Out.Reset();
		auto UE = [](double X, double Y, double Z) { return FOutlawsGeometry::ToUE(X, Y, Z); };
		auto Dir = [](double X, double Z) { return FOutlawsGeometry::ToUE(X, 0.0, Z).GetSafeNormal(); };
		if (bSurvey)
		{
			// sloped roofs first, then the rest spread over the level
			int32 Max = 60;
			OLFlags::Value(TEXT("OLClimbProbeMax"), Max);
			TArray<FClimbCase> All;
			LedgeCases(Lv, All, false);
			TArray<FClimbCase> Roofs, Rest;
			for (FClimbCase& C : All) (C.bRoof ? Roofs : Rest).Add(MoveTemp(C));
			for (int32 i = 0; i < Roofs.Num() && Out.Num() < Max; ++i) Out.Add(Roofs[i]);
			const int32 Left = Max - Out.Num();
			for (int32 i = 0; i < Left && Rest.Num() > 0; ++i)
			{
				const int32 Pick = Rest.Num() <= Left ? i : int32(int64(i) * Rest.Num() / Left);
				if (!Rest.IsValidIndex(Pick)) break;
				Out.Add(Rest[Pick]);
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLCLIMBPROBE survey %s: %d ledges found (%d roofs), %d climbed here"), *LevelName, All.Num(), Roofs.Num(), Out.Num());
			return;
		}
		if (LevelName.Equals(TEXT("townex"), ESearchCase::IgnoreCase))
		{
			LedgeCases(Lv, Out, true);
			// the window into the upstairs room 550 (sector 204's sill 13.25 behind glass from the street 599): no climb
			if (Lv.Sectors.Num() == 635)
			{
				FClimbCase C;
				C.What = TEXT("the glass window 204 from the street 599 (into 550): must be refused");
				C.Want = EClimbWant::Refuse;
				C.StandFeet = UE(27.5, 0.25, 262.0);
				C.Lip = UE(27.5, 13.25, 264.0);
				C.In = Dir(0.0, 1.0);
				C.Sectors = { 204, 550 };
				Out.Add(C);
			}
		}
		else if (LevelName.Equals(TEXT("mill"), ESearchCase::IgnoreCase) && Lv.Sectors.Num() == 611)
		{
			// (ShotTest_OLMillClimbTest's climb part, made by the hand) the hall 576 (floor 21), its box 108 (top 25), the walkway
			// 572 (floor 29) whose edge over the hall runs (301,16) -> (360,-24), its side the way (0.561, 0.8276)
			FClimbCase Box;
			Box.What = TEXT("box 108 from the hall's floor");
			Box.Want = EClimbWant::OnTop;
			Box.StandFeet = UE(324.5, 25.0, -4.0);           // (on the box first: the pick by the feet takes it, the hall's side)
			Box.bStep = true;
			Box.StepFeet = UE(320.5, 21.0, -4.0);            // (then off it onto the hall's floor, as a walk goes)
			Box.Lip = UE(322.5, 25.0, -4.0);
			Box.In = Dir(1.0, 0.0);
			Box.WantFeetZ = 25.0 * ProbeU;
			Box.Sectors = { 108, 576 };
			Out.Add(Box);
			for (const double WX : { 325.0, 308.0 })
			{
				const double WZ = 16.0 - (WX - 301.0) * 40.0 / 59.0, StandY = WX > 320.0 ? 25.0 : 21.0;
				FClimbCase C;
				C.What = FString::Printf(TEXT("the walkway 572 at x %.0f from the %s"), WX, WX > 320.0 ? TEXT("box's top") : TEXT("hall's floor"));
				C.Want = EClimbWant::OnTop;
				C.StandFeet = UE(WX - 0.561 * 3.0, StandY, WZ - 0.8276 * 3.0);
				C.Lip = UE(WX, 29.0, WZ);
				C.In = Dir(0.561, 0.8276);
				C.WantFeetZ = 29.0 * ProbeU;
				C.Sectors = { 572, 550, 394 };
				Out.Add(C);
			}
			FClimbCase Land;
			Land.What = TEXT("from the walkway: a landing along it reached, one in the yard by the doorway refused");
			Land.Want = EClimbWant::LandCheck;
			Out.Add(Land);
		}
	}

	FString RefusalKind(const FString& Why)
	{
		if (Why.Contains(TEXT("ceiling"))) return TEXT("ceiling");
		if (Why.Contains(TEXT("not reached"))) return TEXT("landing");
		if (Why.Contains(TEXT("way across"))) return TEXT("way shut");
		if (Why.Contains(TEXT("no room"))) return TEXT("no room");
		return Why.IsEmpty() ? TEXT("unknown") : TEXT("other");
	}

	void ProbeQuit(bool bOk)
	{
		// (a second on, from the core ticker: BUG-EXITCRASH)
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([bOk](float)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLCLIMBPROBE quitting (exit code %d)"), bOk ? 0 : 1);
			FPlatformMisc::RequestExitWithStatus(false, bOk ? 0 : 1);
			return false;
		}), 1.f);
	}
}

bool AOutlawsPlayer::ClimbProbeOn()
{
	static const bool bOn = OLFlags::Has(TEXT("OLClimbProbe"));
	return bOn;
}

void AOutlawsPlayer::TickClimbProbe(float Dt)
{
	FClimbProbe& P = GClimb;
	if (P.bDone) return;
	AOutlawsLevelActor* L = Level.Get();
	if (!L || L->IsRebuildPending() || L->IsMoviePlaying() || bMenuOpen) return;
	if (!P.bArmed)
	{
		P.bArmed = true;
		FString List = TEXT("townex+mill");
		OLFlags::Value(TEXT("OLClimbProbe"), List);
		if (List.StartsWith(TEXT("survey"), ESearchCase::IgnoreCase))
		{
			P.bSurvey = true;
			List = List.Len() > 7 ? List.Mid(7) : FString(StoryLevels);
		}
		List.ParseIntoArray(P.Levels, TEXT("+"));
		UE_LOG(LogOutlaws, Display, TEXT("OLCLIMBPROBE on: %s %s (%s)"), P.bSurvey ? TEXT("survey of") : TEXT("checks on"), *List,
			OLFlags::Param(TEXT("OLOldSlopeClimb")) ? TEXT("-OLOldSlopeClimb") : TEXT("as built"));
	}
	if (!P.Levels.IsValidIndex(P.LevelI))
	{
		P.bDone = true;
		const bool bOk = P.bSurvey ? P.Pass > 0 : P.Fail == 0 && P.Pass > 0;
		if (P.bSurvey)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLCLIMBPROBE RESULT survey done (%d levels)"), P.Pass);
		}
		else
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLCLIMBPROBE RESULT %s (%d passed, %d failed, %d skipped)"), bOk ? TEXT("ALL PASS") : TEXT("FAIL"), P.Pass, P.Fail, P.Skip);
		}
		ProbeQuit(bOk);
		return;
	}
	const FString Want = P.Levels[P.LevelI];
	if (L->IsFrontEnd() || !L->LevelName.Equals(Want, ESearchCase::IgnoreCase))
	{
		// (the next level: started as a historical mission, as -OLShipSmoke starts them)
		if (!P.bAsked)
		{
			P.bAsked = true;
			UE_LOG(LogOutlaws, Display, TEXT("OLCLIMBPROBE starting %s"), *Want);
			if (bMenuOpen) CloseGameMenu();
			L->StartMission(Want, EOutlawsDifficulty::Bad);
		}
		return;
	}
	L->bInvincible = true;
	L->bEnemiesActive = false;
	L->StartSaveDelay = -1.f;           // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	UCharacterMovementComponent* M = GetCharacterMovement();
	UCapsuleComponent* Cap = GetCapsuleComponent();
	const float Half = Cap->GetScaledCapsuleHalfHeight();
	const float R = Cap->GetScaledCapsuleRadius();
	if (P.Level.Get() != L || P.LevelName != L->LevelName)
	{
		P.Level = L;
		P.LevelName = L->LevelName;
		P.T = 0.f;
		P.CaseI = -1;
		P.Step = 0;
		P.Count = FSurveyCount();
		// the headset's body, as -OLPauseZTest stands it: the head 170 cm over the play-space floor
		bVR = true;
		bCrouchHeld = bStandPending = bAutoCrouched = false;
		CrouchAmount = 1.f;
		CrouchDrop = CrouchFull = 0.f;
		ShownDrop = ShownDropVel = 0.f;
		HeightOffset = 0.f;
		Camera->SetRelativeLocation(FVector(0.0, 0.0, 170.0));
		const FOutlawsLevel* Lv = L->LiveLevel();
		if (Lv) BuildCases(P.LevelName, *Lv, P.bSurvey, P.Cases); else P.Cases.Reset();
		UE_LOG(LogOutlaws, Display, TEXT("OLCLIMBPROBE %s: %d cases"), *P.LevelName, P.Cases.Num());
		for (int32 i = 0; i < P.Cases.Num(); ++i) UE_LOG(LogOutlaws, Display, TEXT("OLCLIMBPROBE %s %d: %s%s%s"), *P.LevelName, i, *P.Cases[i].Id, P.Cases[i].Id.IsEmpty() ? TEXT("") : TEXT(" "), *P.Cases[i].What);
	}
	P.T += Dt;
	if (P.T < 3.f) return;               // (the level settled, the fade in done)
	auto Feet = [&]() { return GetActorLocation() - FVector(0.0, 0.0, Half); };
	auto NextCase = [&]() { ++P.CaseI; P.Step = 0; };
	if (P.CaseI < 0) P.CaseI = 0;
	if (P.Step == 0)
	{
		if (!P.Cases.IsValidIndex(P.CaseI))
		{
			if (P.bSurvey)
			{
				const FSurveyCount& C = P.Count;
				FString Why;
				for (const TPair<FString, int32>& Kv : C.Why) Why += FString::Printf(TEXT("%s%s %d"), Why.IsEmpty() ? TEXT("") : TEXT(", "), *Kv.Key, Kv.Value);
				UE_LOG(LogOutlaws, Display, TEXT("OLCLIMBPROBE survey %s COUNT: %d ledges (%d roofs): climbed %d, vaulted %d, fell off %d, refused %d (%s), no ledge %d, bad spot %d; roofs refused %d"),
					*P.LevelName, C.Cases, C.Roofs, C.Climbed, C.Vaulted, C.FellOff, C.Refused, Why.IsEmpty() ? TEXT("none") : *Why, C.NoLedge, C.BadSpot, C.RoofRefused);
				++P.Pass;
			}
			++P.LevelI;
			P.bAsked = false;
			P.Level = nullptr;
			P.LevelName.Reset();
			return;
		}
		const FClimbCase& C = P.Cases[P.CaseI];
		if (C.Want == EClimbWant::LandCheck)
		{
			// (BUG-MILLCLIMB) the climb's own rule asked from where the body is (on the walkway after the last climb)
			FString Why;
			const FVector From = GetActorLocation();
			const int32 Along = L->ClimbLandingSector(FOutlawsGeometry::ToUE(312.0, 29.0, 9.5), From, &Why);
			const int32 Yard = L->ClimbLandingSector(FOutlawsGeometry::ToUE(236.0, 7.5, 49.0), From, &Why);
			const bool bOk = (Along == 572 || Along == 550 || Along == 394) && Yard == -1;
			UE_LOG(LogOutlaws, Display, TEXT("OLCLIMBPROBE %s %d %s: along the walkway sector %d, the yard %d (%s), the player in %d: %s"), *P.LevelName, P.CaseI, *C.What,
				Along, Yard, *Why, L->PlayerSectorIndex(), bOk ? TEXT("PASS") : TEXT("FAIL"));
			if (bOk) ++P.Pass; else ++P.Fail;
			NextCase();
			return;
		}
		// the last case's crouch (a climb into a low space) let go: stood up again on the ground here
		bCrouchHeld = bAutoCrouched = false;
		bStandPending = true;
		CrouchDrop = CrouchFull = 0.f;
		M->SetMovementMode(MOVE_Walking);
		TeleportTo(C.StandFeet + FVector(0.0, 0.0, Half + 2.0), FRotator(0.0, C.In.Rotation().Yaw, 0.0), false, true);
		M->Velocity = FVector::ZeroVector;
		if (Controller) Controller->SetControlRotation(FRotator(0.0, C.In.Rotation().Yaw, 0.0));
		P.At = P.T;
		P.Step = C.bStep ? 1 : 2;
		return;
	}
	FClimbCase& C = P.Cases[P.CaseI];
	auto MoveToward = [&](const FVector& To, double StepCm)
	{
		const FVector D = To - GetActorLocation();
		if (D.Size() > StepCm) { SetActorLocation(GetActorLocation() + D.GetSafeNormal() * StepCm, false, nullptr, ETeleportType::TeleportPhysics); return false; }
		SetActorLocation(To, false, nullptr, ETeleportType::TeleportPhysics);
		return true;
	};
	if (P.Step == 1)
	{
		// (off the box onto the hall's floor, as a walk goes: the tracking follows the crossing)
		if (P.T < P.At + 1.f) return;
		if (!MoveToward(C.StepFeet + FVector(0.0, 0.0, Half + 2.0), 12.0)) return;
		M->SetMovementMode(MOVE_Walking);
		P.At = P.T;
		P.Step = 2;
		return;
	}
	if (P.Step == 2)
	{
		// stood a moment (standing up again first), then raised against the ledge's face, the feet 30 cm under its top (where
		// the hanging hand's pull starts the climb)
		if (P.T < P.At + (P.bSurvey ? 0.6f : 1.f) || (Half < ProbeStandHalf - 6.f && P.T < P.At + 3.f)) return;
		if (P.bSurvey && FMath::Abs(Feet().Z - C.StandFeet.Z) > 40.0)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLCLIMBPROBE survey %s %s %s: BAD SPOT (stood at z %.0f, the ground there %.0f)"), *P.LevelName, *C.Id, *C.What, Feet().Z, C.StandFeet.Z);
			++P.Count.BadSpot;
			NextCase();
			return;
		}
		P.Hang = FVector(C.Lip.X, C.Lip.Y, C.Lip.Z - 30.0 + Half) - C.In * (R + 3.0);
		M->SetMovementMode(MOVE_Flying);
		M->Velocity = FVector::ZeroVector;
		P.Step = 3;
		return;
	}
	if (P.Step == 3)
	{
		M->Velocity = FVector::ZeroVector;
		if (!MoveToward(P.Hang, P.bSurvey ? 30.0 : 12.0)) return;
		P.At = P.T;
		P.Step = 4;
		return;
	}
	if (P.Step == 4)
	{
		M->Velocity = FVector::ZeroVector;
		if (P.T < P.At + 0.3f) return;
		// the right hand's grip on the lip (5 cm out from the face, 5 cm under the top), the ledge taken, then let go there:
		// the hand under the chest, so the climb onto the top starts if there is room (ReleaseGrab)
		UMotionControllerComponent* Hand = GripOf(1);
		const FTransform Was = Hand->GetRelativeTransform();
		Hand->SetWorldLocation(C.Lip - C.In * 5.0 - FVector(0.0, 0.0, 5.0));
		const bool bGrab = TryGrab(1);
		P.Grab = bGrab ? FString::Printf(TEXT("ledge top z %.0f"), LedgeTopZ) : TEXT("no ledge found by the hand");
		ClimbRefusal.Reset();
		if (bGrab) ReleaseGrab(true);
		else M->SetMovementMode(MOVE_Falling);          // (nothing taken: the body drops)
		Hand->SetRelativeTransform(Was);
		P.bStarted = bGrab && MantleLeft > 0.f;
		P.bVault = P.bStarted && bVaulting;
		P.Refusal = bGrab && !P.bStarted ? ClimbRefusal : FString();
		UE_LOG(LogOutlaws, Display, TEXT("OLCLIMBPROBE %s %d: hanging at z %.0f (feet), %s, %s"), *P.LevelName, P.CaseI, Feet().Z, *P.Grab,
			!bGrab ? TEXT("no climb") : P.bStarted ? *FString::Printf(TEXT("the climb started%s, to %s"), P.bVault ? TEXT(" (a vault)") : TEXT(""), *MantleTo.ToCompactString())
			: *FString::Printf(TEXT("the climb REFUSED, let go: falling (%s)"), *P.Refusal));
		if (!bGrab && C.Want != EClimbWant::Refuse)
		{
			if (P.bSurvey)
			{
				++P.Count.Cases;
				if (C.bRoof) ++P.Count.Roofs;
				++P.Count.NoLedge;
				UE_LOG(LogOutlaws, Display, TEXT("OLCLIMBPROBE survey %s %s %s: NO LEDGE"), *P.LevelName, *C.Id, *C.What);
			}
			else
			{
				UE_LOG(LogOutlaws, Display, TEXT("OLCLIMBPROBE %s %d %s: SKIP (the hand finds no ledge there)"), *P.LevelName, P.CaseI, *C.What);
				++P.Skip;
			}
			NextCase();
			return;
		}
		P.At = P.T;
		P.Step = 5;
		return;
	}
	if (P.Step == 5)
	{
		if (P.T < P.At + (P.bSurvey ? 1.5f : 2.f)) return;
		const FVector F = Feet();
		FHitResult Fl;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsClimbProbe), false, this);
		const bool bFl = GetWorld()->LineTraceSingleByChannel(Fl, F + FVector(0.0, 0.0, 50.0), F - FVector(0.0, 0.0, 600.0), ECC_Visibility, Q);
		const double Under = bFl ? double(Fl.ImpactPoint.Z) : -1e9;
		const int32 S = L->PlayerSectorIndex();
		const bool bWalking = M->MovementMode == MOVE_Walking;
		const bool bOnTop = bWalking && F.Z >= C.Lip.Z - 15.0 && F.Z - Under < 25.0;
		const FString Where = FString::Printf(TEXT("lip z %.0f, %s; %s s on: feet z %.0f over z %.0f, sector %d, mode %d, body half %.0f"), C.Lip.Z, *P.Grab, P.bSurvey ? TEXT("1.5") : TEXT("2"),
			F.Z, Under, S, int32(M->MovementMode.GetValue()), Half);
		if (C.Want == EClimbWant::Survey)
		{
			FSurveyCount& N = P.Count;
			++N.Cases;
			if (C.bRoof) ++N.Roofs;
			FString Out;
			if (!P.bStarted)
			{
				++N.Refused;
				if (C.bRoof) ++N.RoofRefused;
				++N.Why.FindOrAdd(RefusalKind(P.Refusal));
				Out = FString::Printf(TEXT("REFUSED (%s: %s)"), *RefusalKind(P.Refusal), *P.Refusal);
			}
			else if (P.bVault) { ++N.Vaulted; Out = TEXT("VAULTED"); }
			else if (bOnTop) { ++N.Climbed; Out = TEXT("CLIMBED"); }
			else { ++N.FellOff; Out = TEXT("FELL OFF"); }
			UE_LOG(LogOutlaws, Display, TEXT("OLCLIMBPROBE survey %s %s %s: %s; %s"), *P.LevelName, *C.Id, *C.What, *Out, *Where);
			NextCase();
			return;
		}
		bool bOk = false;
		FString Why;
		switch (C.Want)
		{
		case EClimbWant::Roof:
			bOk = P.bStarted && bOnTop;
			Why = bOk ? FString(TEXT("on the roof")) : !P.bStarted ? FString::Printf(TEXT("NO CLIMB (thrown up and dropped): %s"), *P.Refusal)
				: FString(F.Z < C.Lip.Z - 15.0 ? TEXT("DROPPED to the ground") : TEXT("not standing on the roof"));
			break;
		case EClimbWant::OnTop:
			bOk = P.bStarted && FMath::Abs(F.Z - C.WantFeetZ) < 21.0 && C.Sectors.Contains(S);
			Why = bOk ? TEXT("on top") : !P.bStarted ? TEXT("NO CLIMB") : S == 602 || S == 552 || F.Z < C.WantFeetZ - 100.0 ? TEXT("THROWN INTO THE YARD") : TEXT("not on top");
			break;
		case EClimbWant::Refuse:
			bOk = !C.Sectors.Contains(S) && F.Z < C.Lip.Z - 20.0;
			Why = bOk ? FString::Printf(TEXT("refused, on the ground: %s"), *P.Refusal) : FString(TEXT("CLIMBED THROUGH"));
			break;
		default:
			break;
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLCLIMBPROBE %s %d %s: %s: %s (%s)"), *P.LevelName, P.CaseI, *C.What, *Where, bOk ? TEXT("PASS") : TEXT("FAIL"), *Why);
		if (bOk) ++P.Pass; else ++P.Fail;
		NextCase();
	}
}
