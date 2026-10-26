// -OLShots -OLMillClimbTest=<part>: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"
#include "OutlawsLevel.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLMillClimbTest)
{
	FString Part;
	return OLFlags::Value(TEXT("OLMillClimbTest"), Part) && !Part.IsEmpty();
}

// (BUG-MILLCLIMB) -OLLevel=mill -OLNoFrontEnd -OLShots -OLMillClimbTest=climb|wall|walk|save|die|scan|check, with
// -OLOldMillclimb for the before runs.  The hall is sector 576 (floor 21), its box 108 (floor 25), the walkway 572 (floor
// 29); the yard's 602 (floor 20) shares their space; the gate is room 404 ("A"), between BUST3 (364, through the ACCESS1
// doorway 132) on the yard's side and 417 on the hall's.
//   climb: a mantle onto box 108 from the hall's floor: where the body is two seconds on
//   wall:  the body stepped from the hall's floor onto a ledge over the walkway's edge (a move the tracking cannot explain,
//          as standing on a narrow wall is): the yard drawn or not, at ten spots along the walkway
//   walk:  in and out through the gate, backtracking in it both ways, the side checked after each leg; then the yard's
//          ground and the hall's floor stood on after jumps
//   save:  saved in the hall, loaded from the yard; saved in the yard over the hall, loaded from the hall
//   die:   the autosave in the hall, killed, respawned from it; killed again, the level restarted; a restart from the hall
//   scan:  of the sectors sharing space across the gate, which ones each sector's floods reach (OLZONESCAN lines)
//   check: the gate's sides never see what they switch off
//   pick:  (BUG-OVERLAPZONES, BUG-JUMPPICK; any level) -OLZonePickSpots=pX_Y_Z_P_Q+...: saved on sector P's floor where it
//          shares space with Q, loaded: the sector, the feet and what is drawn 6 s on.  scan takes -OLZoneScanTargets=a+b+c
OL_SHOT_TEST(OLMillClimbTest, 581)
{
	FString Part;
	OLFlags::Value(TEXT("OLMillClimbTest"), Part);
	static int32 Passes = 0, Fails = 0;
	static double At = 0.0;
	static int32 Leg = 0, SubStep = 0;
	static FVector Target = FVector::ZeroVector;
	if (!L) return;
	const double U = FOutlawsGeometry::UnitsToCm;
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	auto Feet = [&]() { return (GetActorLocation().Z - Half) / U; };
	auto Put = [&](double X, double Y, double Z, bool bWalk)
	{
		if (bWalk) GetCharacterMovement()->SetMovementMode(MOVE_Walking);
		else GetCharacterMovement()->DisableMovement();
		TeleportTo(FOutlawsGeometry::ToUE(X, Y, Z) + FVector(0.0, 0.0, Half + 2.0), GetActorRotation(), false, true);      // (a teleport: a jump)
		GetCharacterMovement()->Velocity = FVector::ZeroVector;
	};
	auto Nudge = [&](double X, double Y, double Z)        // (a small move, not a teleport: the tracking follows it as a walk)
	{
		GetCharacterMovement()->DisableMovement();
		SetActorLocation(FOutlawsGeometry::ToUE(X, Y, Z) + FVector(0.0, 0.0, Half + 2.0), false, nullptr, ETeleportType::TeleportPhysics);
	};
	auto Check = [&](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT millclimb %s %s: %s"), *Part, bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	auto State = [&]()
	{
		return FString::Printf(TEXT("sector %d, feet %.2f, %s; off 602 %d, 576 %d; drawn 602 %d, 552 %d, 576 %d"), L->PlayerSectorIndex(), Feet(), *L->DebugZoneReport(),
			L->DebugZoneOffNow(602) ? 1 : 0, L->DebugZoneOffNow(576) ? 1 : 0, L->DebugSectorDrawn(602) ? 1 : 0, L->DebugSectorDrawn(552) ? 1 : 0, L->DebugSectorDrawn(576) ? 1 : 0);
	};
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT millclimb %s RESULT %s (%d passed, %d failed)"), *Part, Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	};
	const int32 Out = L->DebugZoneOf(364), In = L->DebugZoneOf(417);      // (-2 with no gate: -OLOldMillclimb)
	L->bInvincible = !(Part == TEXT("die") && (ShotStep == 3 || ShotStep == 6));      // (the deaths only)
	if (ShotStep == 0)
	{
		if (ShotTime < 4.f) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT millclimb %s: level %s, %s, side of 364 %d, of 417 %d, gate %d"), *Part, *L->LevelName, *L->DebugZoneReport(), Out, In, L->DebugZoneGateSector());
		Passes = Fails = 0;
		At = ShotTime;
		Leg = SubStep = 0;
		ShotStep = 1;
		return;
	}
	if (Part == TEXT("scan"))
	{
		TSet<int32> Targets = { 552, 602, 355, 473, 429, 607, 394, 461, 541, 550, 572, 576 };
		FString List;
		if (OLFlags::Value(TEXT("OLZoneScanTargets"), List))          // (BUG-OVERLAPZONES: any level's overlapping sectors, a+b+c)
		{
			Targets.Reset();
			TArray<FString> Words;
			List.ParseIntoArray(Words, TEXT("+"));
			for (const FString& Wd : Words) Targets.Add(FCString::Atoi(*Wd));
		}
		L->DebugZoneScan(Targets);
		Finish();
		return;
	}
	if (Part == TEXT("pick"))
	{
		// (BUG-OVERLAPZONES) any level: -OLZonePickSpots=pX_Y_Z_P_Q+pX_Y_Z_P_Q..., a body stood on sector P's floor at X Y Z
		// where P shares space with Q: the sector the tracking puts it in after the move there, then after a save and a
		// load there (the pick by the feet), the feet, and whether P and Q are drawn
		FString List;
		OLFlags::Value(TEXT("OLZonePickSpots"), List);
		TArray<FString> Spots;
		List.ParseIntoArray(Spots, TEXT("+"));
		if (!Spots.IsValidIndex(Leg)) { Finish(); return; }
		TArray<FString> F;
		Spots[Leg].Mid(1).ParseIntoArray(F, TEXT("_"));
		if (F.Num() < 5) { ++Leg; return; }
		if (Spots[Leg].StartsWith(TEXT("w")) && F.Num() >= 6)
		{
			// (BUG-WHARFPLANK) wX_Y_Z_X2_Z2_SECS: put walking on the floor at X Y Z, then walked toward X2 Z2 for SECS: the
			// sector, the feet and the body's height each half second; HELD if the feet are still on that floor at the end
			const double X = FCString::Atod(*F[0]), Y = FCString::Atod(*F[1]), Z = FCString::Atod(*F[2]);
			const FVector Dir = FOutlawsGeometry::ToUE(FCString::Atod(*F[3]), Y, FCString::Atod(*F[4])) - FOutlawsGeometry::ToUE(X, Y, Z);
			const float Secs = FCString::Atof(*F[5]);
			auto Report = [&](const TCHAR* What)
			{
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT zonewalk %d from %.2f %.2f %.2f %s: sector %d, feet %.2f, at %s, body half %.0f, crouched %d, mode %d"), Leg, X, Y, Z, What,
					L->PlayerSectorIndex(), Feet(), *FOutlawsGeometry::ToOutlawsPlan(GetActorLocation()).ToString(), GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight(),
					bIsCrouched ? 1 : 0, int32(GetCharacterMovement()->MovementMode.GetValue()));
			};
			if (SubStep == 0)
			{
				Put(X, Y, Z, true);
				const FRotator Face(0.0, Dir.Rotation().Yaw, 0.0);
				SetActorRotation(Face);
				if (Controller) Controller->SetControlRotation(Face);
				At = ShotTime; SubStep = 1; Report(TEXT("put")); return;
			}
			if (SubStep == 1 && ShotTime > At + 1.f) { WalkTestLeft = Secs; At = ShotTime; SubStep = 2; Report(TEXT("walk")); return; }
			if (SubStep >= 2 && SubStep < 100 && ShotTime > At + 0.5f * (SubStep - 1))
			{
				Report(TEXT("t"));
				if (0.5f * (SubStep - 1) >= Secs + 1.f)
				{
					const bool bHeld = FMath::Abs(Feet() - Y) < 1.0;
					UE_LOG(LogOutlaws, Display, TEXT("OLSHOT zonewalk %d from %.2f %.2f %.2f: %s"), Leg, X, Y, Z, bHeld ? TEXT("HELD") : TEXT("SANK"));
					if (bHeld) ++Passes; else ++Fails;
					++Leg; SubStep = 0; return;
				}
				++SubStep;
			}
			return;
		}
		const double X = FCString::Atod(*F[0]), Y = FCString::Atod(*F[1]), Z = FCString::Atod(*F[2]);
		const int32 P = FCString::Atoi(*F[3]), Q = FCString::Atoi(*F[4]);
		auto Where = [&]() { return FString::Printf(TEXT("sector %d, feet %.2f (floor %.2f), drawn P %d Q %d"), L->PlayerSectorIndex(), Feet(), Y, L->DebugSectorDrawn(P) ? 1 : 0, L->DebugSectorDrawn(Q) ? 1 : 0); };
		// (BUG-JUMPPICK) the body is held at the spot (no fall) while the save is made, so the save holds the spot itself
		// whatever the move's pick was (a teleport's first tick could drop it through a floor not yet stood on, on a long
		// frame); after the load it walks
		if (SubStep == 0) { Put(X, Y, Z, false); At = ShotTime; SubStep = 1; return; }
		if (SubStep == 1 && ShotTime > At + 2.f)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT zonepick %d/%d at %.2f %.2f %.2f moved there (held): %s; saved %d"), P, Q, X, Y, Z, *Where(), L->SaveGame(7) ? 1 : 0);
			L->LoadGame(7);
			GetCharacterMovement()->SetMovementMode(MOVE_Walking);
			At = ShotTime;
			SubStep = 2;
			return;
		}
		if (SubStep == 2 && ShotTime > At + 6.f)
		{
			const int32 S = L->PlayerSectorIndex();
			const bool bOk = S == P && FMath::Abs(Feet() - Y) < 1.0 && !L->DebugSectorDrawn(Q);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT zonepick %d/%d at %.2f %.2f %.2f after a load: %s: %s"), P, Q, X, Y, Z, *Where(),
				bOk ? TEXT("RIGHT") : S != P ? TEXT("WRONG SECTOR") : FMath::Abs(Feet() - Y) >= 1.0 ? TEXT("FELL") : TEXT("Q DRAWN"));
			if (bOk) ++Passes; else ++Fails;
			++Leg;
			SubStep = 0;
		}
		return;
	}
	if (Part == TEXT("check"))
	{
		FString Rep;
		const int32 Bad = L->DebugZoneGateCheck(Rep);
		Check(Bad == 0 && L->DebugZoneGateSector() >= 0, FString::Printf(TEXT("eyes that see what their side switches off: %d; %s"), Bad, *Rep));
		Finish();
		return;
	}
	if (Part == TEXT("climb"))
	{
		static FVector Lip, LIn;
		// on the box first (the pick by the feet takes it, the smallest), then off it onto the hall's floor as a walk would go
		// (the tracking follows the crossing into 576), then the climb back onto it
		if (ShotStep == 1) { Put(324.5, 25.0, -4.0, false); At = ShotTime; ShotStep = 10; return; }
		if (ShotStep == 10)
		{
			if (ShotTime < At + 1.f) return;
			const FVector To = FOutlawsGeometry::ToUE(320.5, 25.0, -4.0) + FVector(0.0, 0.0, Half + 2.0);
			const FVector D = To - GetActorLocation();
			if (D.Size() > 12.0) { SetActorLocation(GetActorLocation() + D.GetSafeNormal() * 12.0, false, nullptr, ETeleportType::TeleportPhysics); return; }
			SetActorLocation(To, false, nullptr, ETeleportType::TeleportPhysics);
			GetCharacterMovement()->SetMovementMode(MOVE_Walking);
			At = ShotTime;
			ShotStep = 2;
			return;
		}
		if (ShotStep == 2 && ShotTime > At + 1.5f)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT millclimb climb: before the box: %s"), *State());
			// hanging from its lip, the body against its face, as a grab from the floor does when let go at chest height (the
			// climb onto the top starts from there, ReleaseGrab): the feet 30 cm over the hall's floor
			const double R = GetCapsuleComponent()->GetScaledCapsuleRadius() / U;
			Nudge(322.5 - R - 0.1, 22.0, -4.0);
			GetCharacterMovement()->SetMovementMode(MOVE_Flying);
			At = ShotTime;
			ShotStep = 20;
			return;
		}
		if (ShotStep == 20 && ShotTime > At + 0.5f)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT millclimb climb: hanging at the box's lip: %s"), *State());
			Lip = FOutlawsGeometry::ToUE(322.5, 25.0, -4.0);
			LIn = FOutlawsGeometry::ToUE(1.0, 0.0, 0.0).GetSafeNormal();
			GrabPoint = Lip - LIn * 3.0 - FVector(0.0, 0.0, 4.0);
			LedgeTopZ = float(Lip.Z);
			LedgeIn = LIn;
			const bool bStarted = StartMantle();
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT millclimb climb: the mantle onto box 108 (top z %.0f) %s %s"), Lip.Z, bStarted ? TEXT("started, to") : TEXT("REFUSED"), bStarted ? *MantleTo.ToCompactString() : TEXT(""));
			At = ShotTime;
			ShotStep = 3;
			return;
		}
		if (ShotStep == 3 && ShotTime > At + 2.f)
		{
			const int32 S = L->PlayerSectorIndex();
			const bool bOnBox = FMath::Abs(Feet() - 25.0) < 0.7 && (S == 108 || S == 576);
			Check(bOnBox, FString::Printf(TEXT("two seconds on: %s (%s)"), *State(), bOnBox ? TEXT("on the box") : S == 602 || S == 552 ? TEXT("THROWN INTO THE YARD") : TEXT("not on the box")));
			// then up onto the walkway 572 (floor 29) over the hall, twice: from the box's top at x 325, and from the hall's floor
			// at x 308, where the yard's 552 stands over the walkway (the snapshot's spot, 308.8 14.2, is just past it).  The
			// walkway's edge runs (301,16) -> (360,-24), its side the way (0.561, 0.8276)
			Leg = 0;
			SubStep = 0;
			ShotStep = 4;
		}
		if (ShotStep == 4)
		{
			const double WX = Leg == 0 ? 325.0 : 308.0, WZ = 16.0 - (WX - 301.0) * 40.0 / 59.0, StandY = Leg == 0 ? 25.0 : 21.0;
			if (SubStep == 0) { Put(WX - 0.561 * 3.0, StandY, WZ - 0.8276 * 3.0, true); At = ShotTime; SubStep = 1; return; }
			if (SubStep == 1)
			{
				if (ShotTime < At + 1.5f) return;
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT millclimb climb: below the walkway at x %.0f: %s"), WX, *State());
				SubStep = 2;
				return;
			}
			if (SubStep == 2)
			{
				// hanging from the walkway's lip (let go at chest height after a grab: the climb onto the top starts there), the feet
				// at 26 against its face over the hall, moved there as a walk and a pull up would go (the tracking follows)
				const double R = GetCapsuleComponent()->GetScaledCapsuleRadius() / U;
				const FVector To = FOutlawsGeometry::ToUE(WX - 0.561 * (R + 0.1), 26.0, WZ - 0.8276 * (R + 0.1)) + FVector(0.0, 0.0, Half + 2.0);
				const FVector D = To - GetActorLocation();
				GetCharacterMovement()->SetMovementMode(MOVE_Flying);
				GetCharacterMovement()->Velocity = FVector::ZeroVector;
				if (D.Size() > 12.0) { SetActorLocation(GetActorLocation() + D.GetSafeNormal() * 12.0, false, nullptr, ETeleportType::TeleportPhysics); return; }
				SetActorLocation(To, false, nullptr, ETeleportType::TeleportPhysics);
				At = ShotTime;
				SubStep = 3;
				return;
			}
			if (SubStep == 3)
			{
				if (ShotTime < At + 0.5f) return;
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT millclimb climb: hanging at the walkway's lip at x %.0f: %s"), WX, *State());
				Lip = FOutlawsGeometry::ToUE(WX, 29.0, WZ);
				LIn = FOutlawsGeometry::ToUE(0.561, 0.0, 0.8276).GetSafeNormal();
				GrabPoint = Lip - LIn * 3.0 - FVector(0.0, 0.0, 4.0);
				LedgeTopZ = float(Lip.Z);
				LedgeIn = LIn;
				const bool bStarted = StartMantle();
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT millclimb climb: the mantle onto the walkway at x %.0f (top z %.0f) %s %s"), WX, Lip.Z, bStarted ? TEXT("started, to") : TEXT("REFUSED"),
					bStarted ? *MantleTo.ToCompactString() : TEXT(""));
				At = ShotTime;
				SubStep = 4;
				return;
			}
			if (SubStep == 4 && ShotTime > At + 2.f)
			{
				const int32 S = L->PlayerSectorIndex();
				const bool bOn = FMath::Abs(Feet() - 29.0) < 0.7 && (S == 572 || S == 550 || S == 394);
				Check(bOn, FString::Printf(TEXT("two seconds after the walkway climb at x %.0f: %s (%s)"), WX, *State(),
					bOn ? TEXT("on the walkway") : S == 602 || S == 552 || Feet() < 24.0 ? TEXT("THROWN INTO THE YARD") : TEXT("not on the walkway")));
				SubStep = 0;
				if (++Leg >= 2)
				{
					// the climb's own rule, asked directly from the walkway: a landing further along it is reached; one in the yard by
					// the doorway 87 (sector 605, not a zone gate's far side) is not reached through openings near the climb
					FString Why;
					const FVector From = GetActorLocation();
					const int32 Along = L->ClimbLandingSector(FOutlawsGeometry::ToUE(312.0, 29.0, 9.5), From, &Why);
					Check(Along == 572 || Along == 550 || Along == 394, FString::Printf(TEXT("a landing further along the walkway: sector %d"), Along));
					const int32 Yard = L->ClimbLandingSector(FOutlawsGeometry::ToUE(236.0, 7.5, 49.0), From, &Why);
					Check(Yard == -1, FString::Printf(TEXT("a landing in the yard by the doorway: %d (%s)"), Yard, *Why));
					Finish();
				}
			}
		}
		return;
	}
	if (Part == TEXT("wall"))
	{
		// ten spots along the walkway 572 (its edge over the hall runs (301,16) -> (360,-24))
		if (ShotStep == 1)
		{
			const double X = 310.0 + 4.5 * Leg;
			const double Edge = 16.0 - (X - 301.0) * 40.0 / 59.0;          // the hall's side of the edge at X
			if (SubStep == 0) { Put(X, 21.0, Edge - 1.5, false); At = ShotTime; SubStep = 1; return; }
			if (SubStep == 1 && ShotTime > At + 1.f) { Nudge(X, 26.0, Edge + 1.2); At = ShotTime; SubStep = 2; return; }
			if (SubStep == 2 && ShotTime > At + 0.5f)
			{
				const bool bYard = L->DebugSectorDrawn(602) || L->DebugSectorDrawn(552);
				const int32 S = L->PlayerSectorIndex();
				Check(!bYard && S != 602 && S != 552, FString::Printf(TEXT("spot %d (x %.1f) on a ledge over the walkway's edge: %s (%s)"), Leg, X, *State(), bYard ? TEXT("THE YARD FLASHES IN") : TEXT("no yard drawn")));
				SubStep = 0;
				if (++Leg >= 10) Finish();
			}
		}
		return;
	}
	if (Part == TEXT("walk"))
	{
		// legs through the gate 404: BUST3 (364) at (341, 140) on the yard's side, 417 at (347, 168) on the hall's, the gate's
		// middle at (343, 156); the straight lines between them pass the ACCESS1 doorway and the opening onto 417
		struct FLeg { double X, Z; int32 Want; const TCHAR* What; };
		const FLeg Legs[] = {
			{ 347.0, 168.0, In, TEXT("in through the gate") }, { 341.0, 140.0, Out, TEXT("out through the gate") }, { 347.0, 168.0, In, TEXT("in again") },
			{ 343.0, 156.0, In, TEXT("back into the gate from the hall's side") }, { 347.0, 168.0, In, TEXT("back to the hall's side (no crossing)") },
			{ 343.0, 156.0, In, TEXT("into the gate") }, { 341.0, 140.0, Out, TEXT("on out to the yard's side") },
			{ 343.0, 156.0, Out, TEXT("into the gate from the yard's side") }, { 341.0, 140.0, Out, TEXT("back to the yard's side (no crossing)") },
			{ 347.0, 168.0, In, TEXT("in a third time") }, { 341.0, 140.0, Out, TEXT("out a third time") },
		};
		const int32 NumLegs = UE_ARRAY_COUNT(Legs);
		if (ShotStep == 1) { Put(341.0, 8.0, 140.0, false); At = ShotTime; ShotStep = 2; return; }
		if (ShotStep == 2 && ShotTime > At + 1.f)
		{
			Check(L->DebugZoneNow() == Out, FString::Printf(TEXT("on the yard's side at the start: %s"), *State()));
			ShotStep = 3;
			Leg = 0;
			SubStep = 0;
			return;
		}
		if (ShotStep == 3)
		{
			if (Leg >= NumLegs) { ShotStep = 4; At = ShotTime; return; }
			const FVector Now = GetActorLocation();
			const FVector To = FOutlawsGeometry::ToUE(Legs[Leg].X, 8.0, Legs[Leg].Z) + FVector(0.0, 0.0, Half + 2.0);
			if (SubStep == 0)
			{
				const FVector D = To - Now;
				if (D.Size() > 12.0) { SetActorLocation(Now + D.GetSafeNormal() * 12.0, false, nullptr, ETeleportType::TeleportPhysics); return; }      // (12 cm a tick, a walk)
				SetActorLocation(To, false, nullptr, ETeleportType::TeleportPhysics);
				At = ShotTime;
				SubStep = 1;
				return;
			}
			if (ShotTime > At + 0.3f)
			{
				const bool bOff = L->DebugZoneOffNow(602) && L->DebugZoneOffNow(552);
				const bool bWantOff = Legs[Leg].Want == In;
				Check(L->DebugZoneNow() == Legs[Leg].Want && bOff == bWantOff, FString::Printf(TEXT("leg %d, %s: side %d (want %d), yard sectors off %d (want %d); %s"), Leg, Legs[Leg].What,
					L->DebugZoneNow(), Legs[Leg].Want, bOff ? 1 : 0, bWantOff ? 1 : 0, *State()));
				++Leg;
				SubStep = 0;
			}
			return;
		}
		// the yard's ground where the hall stands over it, after going back out, then the hall's floor (jumps both; 5 s each,
		// past the unstuck's 3 s in a wall)
		if (ShotStep == 4) { Put(320.5, 20.0, -4.0, true); At = ShotTime; ShotStep = 5; return; }
		if (ShotStep == 5 && ShotTime > At + 5.f)
		{
			const int32 S = L->PlayerSectorIndex();
			Check(L->DebugZoneNow() == Out && FMath::Abs(Feet() - 20.0) < 0.7 && S == 602 && !L->DebugZoneOffNow(602) && L->DebugSectorDrawn(602) && L->DebugZoneOffNow(576),
				FString::Printf(TEXT("the yard's ground under the hall after going back out: stood on, drawn, the hall off: %s"), *State()));
			Put(320.5, 21.0, -4.0, true);
			At = ShotTime;
			ShotStep = 6;
			return;
		}
		if (ShotStep == 6 && ShotTime > At + 5.f)
		{
			const int32 S = L->PlayerSectorIndex();
			Check(L->DebugZoneNow() == In && FMath::Abs(Feet() - 21.0) < 0.7 && S == 576 && L->DebugZoneOffNow(602) && !L->DebugSectorDrawn(602) && !L->DebugSectorDrawn(552),
				FString::Printf(TEXT("the hall's floor after a jump in: stood on, the yard off and undrawn: %s"), *State()));
			Finish();
		}
		return;
	}
	if (Part == TEXT("save"))
	{
		auto Settle = [&](int32 Next) { At = ShotTime; ShotStep = Next; };
		switch (ShotStep)
		{
		case 1: Put(320.5, 21.0, -4.0, true); Settle(2); return;
		case 2:
			if (ShotTime < At + 2.5f) return;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT millclimb save: in the hall: %s; saved %d"), *State(), L->SaveGame(7) ? 1 : 0);
			Put(236.0, 8.0, 49.0, true);
			Settle(3);
			return;
		case 3:
			if (ShotTime < At + 2.f) return;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT millclimb save: in the yard before the load: %s"), *State());
			L->LoadGame(7);
			Settle(4);
			return;
		case 4:
			if (ShotTime < At + 6.f) return;
			Check(L->DebugZoneNow() == In && L->PlayerSectorIndex() == 576 && FMath::Abs(Feet() - 21.0) < 0.7 && L->DebugZoneOffNow(602) && !L->DebugSectorDrawn(602),
				FString::Printf(TEXT("saved in the hall, loaded from the yard: %s"), *State()));
			Put(320.5, 20.0, -4.0, true);
			Settle(5);
			return;
		case 5:
			if (ShotTime < At + 2.5f) return;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT millclimb save: on the yard's ground under the hall: %s; saved %d"), *State(), L->SaveGame(7) ? 1 : 0);
			Put(320.5, 21.0, -4.0, true);
			Settle(6);
			return;
		case 6:
			if (ShotTime < At + 2.f) return;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT millclimb save: in the hall before the load: %s"), *State());
			L->LoadGame(7);
			Settle(7);
			return;
		case 7:
			if (ShotTime < At + 6.f) return;
			Check(L->DebugZoneNow() == Out && L->PlayerSectorIndex() == 602 && FMath::Abs(Feet() - 20.0) < 0.7 && !L->DebugZoneOffNow(602) && L->DebugZoneOffNow(576),
				FString::Printf(TEXT("saved in the yard under the hall, loaded from the hall: %s"), *State()));
			Finish();
			return;
		default: return;
		}
	}
	if (Part == TEXT("die"))
	{
		auto Settle = [&](int32 Next) { At = ShotTime; ShotStep = Next; };
		switch (ShotStep)
		{
		case 1: Put(320.5, 21.0, -4.0, true); Settle(2); return;
		case 2:
			if (ShotTime < At + 2.5f) return;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT millclimb die: in the hall: %s; autosaved %d"), *State(), L->SaveGame(-1) ? 1 : 0);
			Settle(3);
			return;
		case 3:
			L->DamagePlayer(1e6f, 1.f, true);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT millclimb die: killed in the hall (dead %d)"), L->IsPlayerDead() ? 1 : 0);
			Settle(4);
			return;
		case 4:
			if (ShotTime < At + 3.f) return;
			RequestRespawn(true);
			Settle(5);
			return;
		case 5:
			if (ShotTime < At + 7.f) return;
			Check(L->DebugZoneNow() == In && L->PlayerSectorIndex() == 576 && FMath::Abs(Feet() - 21.0) < 0.7 && L->DebugZoneOffNow(602) && !L->IsPlayerDead(),
				FString::Printf(TEXT("respawned from the hall's autosave: %s"), *State()));
			Settle(6);
			return;
		case 6:
			L->DamagePlayer(1e6f, 1.f, true);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT millclimb die: killed in the hall again (dead %d)"), L->IsPlayerDead() ? 1 : 0);
			Settle(7);
			return;
		case 7:
			if (ShotTime < At + 3.f) return;
			RequestRespawn(false);
			Settle(8);
			return;
		case 8:
			if (ShotTime < At + 7.f) return;
			Check(L->DebugZoneNow() == L->DebugZoneOf(L->PlayerSectorIndex()) && L->DebugZoneNow() == Out && !L->DebugZoneOffNow(602) && !L->IsPlayerDead(),
				FString::Printf(TEXT("the level restarted after the death: at its start, in the yard: %s"), *State()));
			Put(320.5, 21.0, -4.0, true);
			Settle(10);
			return;
		case 10:
			if (ShotTime < At + 2.5f) return;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT millclimb die: in the hall before a restart: %s"), *State());
			L->RestartLevel();
			Settle(11);
			return;
		case 11:
			if (ShotTime < At + 6.f) return;
			Check(L->DebugZoneNow() == Out && !L->DebugZoneOffNow(602) && L->DebugZoneOffNow(576), FString::Printf(TEXT("restarted from the hall: at the start, in the yard: %s"), *State()));
			Finish();
			return;
		default: return;
		}
	}
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT millclimb: no part %s"), *Part);
	Finish();
}

#endif // WITH_OUTLAWS_TESTS
