#include "OutlawsInfRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsPerf.h"
#include "OutlawsRuntime.h"

namespace
{
	/** Messages can loop (a trigger whose client is its own sector); the Jedi engine has no guard. */
	int32 GSendDepth = 0;
	constexpr int32 MaxSendDepth = 16;

	bool IsSwitchTrigger(const FString& RawClass)
	{
		const FString U = RawClass.ToUpper();
		return U.Contains(TEXT("SWITCH1")) || U.Contains(TEXT("TOGGLE")) || U.Contains(TEXT("SINGLE"));
	}
}

bool FOutlawsInfRuntime::SectorLightsOn()
{
	static const bool bOld = OLFlags::Param(TEXT("OLOldSectorLight"));
	return !bOld;
}

double FOutlawsInfRuntime::DefaultSpeed(EOutlawsInfClass Class)
{
	switch (Class)
	{
	case EOutlawsInfClass::MoveFloor:
	case EOutlawsInfClass::MoveCeiling:
	case EOutlawsInfClass::MoveFC:
		return 8.0;
	case EOutlawsInfClass::MorphMove:
		return 15.0;
	case EOutlawsInfClass::MorphSpin:
		return 1024.0 * 360.0 / 16384.0;   // 1024 angle units per second = 22.5 degrees
	default:
		return 4.0;
	}
}

void FOutlawsInfRuntime::FindSectors(const FString& Name, const FString& Id, TArray<int32>& Out) const
{
	Out.Reset();
	for (const FOutlawsSector& S : Live.Sectors)
	{
		const bool bIdOk = Id.IsEmpty() || S.Id.Equals(Id, ESearchCase::IgnoreCase);
		const bool bNameOk = Name.IsEmpty() || S.Name.Equals(Name, ESearchCase::IgnoreCase);
		if (bIdOk && bNameOk && (!Name.IsEmpty() || !Id.IsEmpty()))
		{
			Out.Add(S.Index);
		}
	}
	if (Out.Num() == 0 && !Id.IsEmpty() && !Name.IsEmpty())
	{
		// id qualifiers sometimes point at the wall; fall back to the name alone
		FindSectors(Name, FString(), Out);
	}
}

void FOutlawsInfRuntime::Init(const FOutlawsLevel& Level, const FString& InfText)
{
	Live = Level;
	Elevators.Reset();
	SavedElevatorsBefore = 0;
	LightsFrom = -1;
	Triggers.Reset();
	ElevatorsBySector.Reset();
	LightsBySector.Reset();
	TriggersBySector.Reset();
	TriggersByWall.Reset();
	Warnings.Reset();
	Inf.Parse(InfText);

	TArray<int32> Found;
	TArray<FIntVector4> GatedLights;     // (REQ-PIANO) scroll, item, class, sector: lights made elevators after the rest
	TArray<FIntVector> SectorLights;     // (REQ-SECTORLIGHT) item, class, sector: the other lights, made after those
	for (int32 i = 0; i < Inf.Items.Num(); ++i)
	{
		const FOutlawsInfItem& It = Inf.Items[i];
		FindSectors(It.Name, It.bLine ? FString() : It.NameId, Found);
		if (Found.Num() == 0)
		{
			Warnings.Add(FString::Printf(TEXT("item %s: no sector named so"), *It.Name));
			continue;
		}
		if (Found.Num() > 1 && !It.bLine)
		{
			Warnings.Add(FString::Printf(TEXT("item %s: %d sectors share the name, using the first"), *It.Name, Found.Num()));
		}
		const int32 Sec = Found[0];
		int32 WallSec = Sec;
		int32 WallIdx = -1;
		if (It.bLine)
		{
			for (int32 s : Found)
			{
				const FOutlawsSector& S = Live.Sectors[s];
				for (int32 w = 0; w < S.Walls.Num(); ++w)
				{
					if (S.Walls[w].Id.Equals(It.WallId, ESearchCase::IgnoreCase)) { WallIdx = w; WallSec = s; break; }
				}
				if (WallIdx >= 0) break;
			}
			if (WallIdx < 0)
			{
				// NUM can also be a plain wall index
				if (!It.WallId.IsEmpty() && FChar::IsDigit(It.WallId[0]) && Live.Sectors[Sec].Walls.IsValidIndex(FCString::Atoi(*It.WallId)))
				{
					WallIdx = FCString::Atoi(*It.WallId);
				}
				else
				{
					Warnings.Add(FString::Printf(TEXT("line item %s: wall #%s not found"), *It.Name, *It.WallId));
					continue;
				}
			}
		}
		bool bHaveElevator = false;
		for (int32 c = 0; c < It.Classes.Num(); ++c)
		{
			const FOutlawsInfClass& C = It.Classes[c];
			if (C.IsTrigger())
			{
				FOutlawsTrigger T;
				T.Item = i;
				T.ClassIndex = c;
				T.bLine = It.bLine;
				T.Sector = It.bLine ? WallSec : Sec;
				T.Wall = WallIdx;
				// no EVENT_MASK: every event; switch-type line triggers ignore walking through them
				T.Mask = C.bHasEventMask ? C.EventMask : 0xFFFFFFFFu;
				const bool bSwitch = It.bLine && IsSwitchTrigger(C.RawClass);
				if (bSwitch && !C.bHasEventMask)
				{
					T.Mask &= ~(OutlawsInf::EvCrossFront | OutlawsInf::EvCrossBack);
				}
				// no EVENT_MASK also means the player alone (the Jedi engine's default ENTITY_MASK): a bullet is not the
				// player, so it only reaches triggers that list the shoot bit (the ranch 256, the mill 368, the office
				// 304); the train's boxcar doors no longer open to a shot (user 2026-09-28)
				if (!C.bHasEventMask)
				{
					T.Mask &= ~OutlawsInf::EvShoot;
				}
				T.bOneShot = C.Class == EOutlawsInfClass::TriggerSingle || C.RawClass.ToUpper().Contains(TEXT("SWITCH1"));
				T.bMaster = C.bMasterOn;
				const int32 Ti = Triggers.Add(T);
				if (It.bLine) TriggersByWall.FindOrAdd(WallKey(WallSec, WallIdx)).Add(Ti);
				else TriggersBySector.FindOrAdd(Sec).Add(Ti);
			}
			else if (!It.bLine && (C.Class == EOutlawsInfClass::ScrollFloor || C.Class == EOutlawsInfClass::ScrollCeiling || C.Class == EOutlawsInfClass::ScrollWall))
			{
				// a scroll rides alongside any mover in the same item
				FOutlawsScroll Sc;
				Sc.Item = i;
				Sc.Class = C.Class;
				Sc.Sectors.Add(Sec);
				// "ITEM: SECTOR NAME: X NUM: #wallid" on a SCROLL_WALL: that one wall of the sector (canyon's river banks;
				// user 2026-09-29: every flagged wall of the sector ran)
				if (C.Class == EOutlawsInfClass::ScrollWall && !It.WallId.IsEmpty())
				{
					const FOutlawsSector& SS = Live.Sectors[Sec];
					for (int32 w = 0; w < SS.Walls.Num(); ++w)
					{
						if (SS.Walls[w].Id.Equals(It.WallId, ESearchCase::IgnoreCase)) Sc.OnlyWalls.Add(w);
					}
				}
				for (const FString& SlaveName : C.Slaves)
				{
					FString N, Id;
					FOutlawsInfFile::SplitNameId(SlaveName, N, Id);
					TArray<int32> SlaveSecs;
					FindSectors(N, Id, SlaveSecs);
					for (int32 s : SlaveSecs) Sc.Sectors.AddUnique(s);
				}
				const double A = FMath::DegreesToRadians(C.Angle);
				Sc.Dir = FVector2D(FMath::Sin(A), FMath::Cos(A));
				// on a sloped floor (ceiling) ANGLE is in the slope's own frame, along its hinge wall and up/down
				// its normal, and moves the texture that way: the cliff's falls and mudslides then run straight
				// downhill, canyon's rivers down their beds (checked against all seven, 2026-09-27). Our floor
				// textures lie flat in world x/z, where the offset runs opposite the texture's motion
				FVector2D Along, Normal;
				const bool bFloorScroll = C.Class == EOutlawsInfClass::ScrollFloor;
				if (C.Class != EOutlawsInfClass::ScrollWall && Live.SlopeFrame(Live.Sectors[Sec], bFloorScroll, Along, Normal))
				{
					Sc.Dir = -(Along * FMath::Sin(A) + Normal * FMath::Cos(A));
				}
				Sc.Speed = (C.bHasSpeed ? C.Speed : 32.0) / 8.0;
				if (C.Stops.Num() > 0)
				{
					// with stops it runs as an elevator (triggers, messages, holds and waits as any other); its
					// value is how far it has scrolled, in texels, at SPEED texels a second
					FOutlawsElevator E;
					E.Item = i;
					E.ClassIndex = c;
					E.Class = C.Class;
					E.Name = It.Name;
					E.bMasterOn = C.bMasterOn;
					E.Speed = C.bHasSpeed ? C.Speed : 32.0;
					E.Sectors = Sc.Sectors;
					for (int32 s : E.Sectors)
					{
						E.BaseFloor.Add(Live.Sectors[s].Floor.Y);
						E.BaseCeil.Add(Live.Sectors[s].Ceil.Y);
					}
					Sc.Elevator = Elevators.Add(E);
					ElevatorsBySector.FindOrAdd(Sec).Add(Sc.Elevator);
				}
				Scrolls.Add(Sc);
			}
			else if (!It.bLine && C.Class == EOutlawsInfClass::ChangeLight && C.Stops.Num() > 0)
			{
				// (REQ-PIANO) a CHANGE_LIGHT after a stop-less scroll in the same SEQ (town's and townex's SCROLL, the piano's
				// roll: lit by the doors as the tune starts, dark again as they shut): the scroll runs while the light is up.
				// Only that pairing (no other level has it); lights on their own come after (REQ-SECTORLIGHT)
				bool bGates = false;
				for (int32 k = 0; k < Scrolls.Num(); ++k)
				{
					if (Scrolls[k].Item == i && Scrolls[k].Elevator < 0) { GatedLights.Add(FIntVector4(k, i, c, Sec)); bGates = true; }
				}
				if (!bGates && SectorLightsOn()) SectorLights.Add(FIntVector(i, c, Sec));
			}
			else if (C.IsElevator() && !It.bLine)
			{
				// (an item may carry several: ranch's HAY1 moves its floor and its ceiling in one SEQ; user 2026-09-29)
				const bool bSupported = C.Class == EOutlawsInfClass::MoveFloor || C.Class == EOutlawsInfClass::MoveCeiling || C.Class == EOutlawsInfClass::MoveFC ||
					C.Class == EOutlawsInfClass::MorphMove || C.Class == EOutlawsInfClass::MorphSpin;
				if (!bSupported || C.Stops.Num() == 0)
				{
					continue;   // scrolls, lights, velocities: later
				}
				bHaveElevator = true;
				FOutlawsElevator E;
				E.Item = i;
				E.ClassIndex = c;
				E.Class = C.Class;
				E.Name = It.Name;
				E.bMasterOn = C.bMasterOn;
				E.Speed = C.bHasSpeed ? C.Speed : DefaultSpeed(C.Class);
				E.bTrigLast = C.Class == EOutlawsInfClass::MoveCeiling;
				E.Sectors.Add(Sec);
				for (const FString& SlaveName : C.Slaves)
				{
					FString N, Id;
					FOutlawsInfFile::SplitNameId(SlaveName, N, Id);
					TArray<int32> SlaveSecs;
					FindSectors(N, Id, SlaveSecs);
					for (int32 s : SlaveSecs) E.Sectors.AddUnique(s);
				}
				for (int32 s : E.Sectors)
				{
					E.BaseFloor.Add(Live.Sectors[s].Floor.Y);
					E.BaseCeil.Add(Live.Sectors[s].Ceil.Y);
				}
				const int32 Ei = Elevators.Add(E);
				ElevatorsBySector.FindOrAdd(Sec).Add(Ei);
			}
		}
	}
	// scroll-flagged walls of a sector with VELOCITY and no scroll of its own (the train's edge walls: the
	// ground rushes by under them at 62.5 u/s, their SCROLLMID art with it): each slides along its length at
	// the velocity's share along it, as the floor's texture moves (walls facing the motion do not move)
	{
		TSet<int32> Scrolled;
		for (const FOutlawsScroll& Sc : Scrolls)
		{
			if (Sc.Class == EOutlawsInfClass::ScrollWall) Scrolled.Append(Sc.Sectors);
		}
		// ... and where nothing names their motion (the train's map edge: flagged, no velocity, no scroll),
		// the level's ground motion, when all its floor scrolls move alike (INFERRED: the train's edge walls
		// move with the ground in the original, user 2026-09-27)
		FVector2D Ground = FVector2D::ZeroVector;
		bool bGround = false, bMixed = false;
		for (const FOutlawsScroll& Sc : Scrolls)
		{
			if (Sc.Class != EOutlawsInfClass::ScrollFloor || Sc.Elevator >= 0) continue;
			const FVector2D Tex = -Sc.Dir * Sc.Speed;          // the texture's own motion (the offset grows the other way)
			if (bGround && !Tex.Equals(Ground, 1e-3)) bMixed = true;
			Ground = Tex;
			bGround = true;
		}
		if (bMixed) bGround = false;
		// only where a VELFLOORONLY ground carries the level along (the train): canyon's currents and winds move no
		// wall art, and its flagged banks have SCROLL_WALLs of their own (user 2026-09-29: walls ran like falls)
		bool bGroundLevel = false;
		for (const FOutlawsSector& S : Live.Sectors)
		{
			if ((S.Flags & OutlawsFlags::VelFloorOnly) && !S.Velocity.IsNearlyZero()) { bGroundLevel = true; break; }
		}
		// (BUG-WATERSCROLL) a sector's own VELOCITY moves its walls' art only when it is a carrying ground itself
		// (VELFLOORONLY, the train's): a current (the sawmill's channels 359 and 386, flagged walls, no SCROLL_WALL)
		// pushes the swimmer and moves no wall (user 2026-10-19: metal walls under the water ran to the left)
		const bool bOldWaterScroll = OLFlags::Param(TEXT("OLOldWaterScroll"));     // (-OLOldWaterScroll: as before)
		for (const FOutlawsSector& S : Live.Sectors)
		{
			FVector2D V(S.Velocity.X, S.Velocity.Z);
			if (!V.IsNearlyZero(1e-3) && !(S.Flags & OutlawsFlags::VelFloorOnly) && !bOldWaterScroll) continue;
			if (V.IsNearlyZero(1e-3) && bGround) V = Ground;
			if (!bGroundLevel || V.IsNearlyZero(1e-3) || Scrolled.Contains(S.Index)) continue;
			for (int32 w = 0; w < S.Walls.Num(); ++w)
			{
				if (!(S.Walls[w].Flags1 & OutlawsFlags::WallScrollAny)) continue;
				const FVector2D T = (S.Verts[S.Walls[w].V2] - S.Verts[S.Walls[w].V1]).GetSafeNormal();
				const double Rate = -FVector2D::DotProduct(V, T);
				if (FMath::Abs(Rate) < 1e-2) continue;
				FOutlawsScroll Sc;
				Sc.Class = EOutlawsInfClass::ScrollWall;
				Sc.Sectors.Add(S.Index);
				Sc.OnlyWalls.Add(w);
				Sc.Dir = FVector2D(1.0, 0.0);
				Sc.Speed = Rate;
				Scrolls.Add(Sc);
				UE_LOG(LogOutlaws, Log, TEXT("OLRT ground scroll: sector %d wall %d flags 0x%x, sector flags 0x%x velocity (%.1f,%.1f,%.1f)%s, rate %.2f"),
					S.Index, w, S.Walls[w].Flags1, S.Flags, S.Velocity.X, S.Velocity.Y, S.Velocity.Z,
					FVector2D(S.Velocity.X, S.Velocity.Z).IsNearlyZero(1e-3) ? TEXT(" (the level's ground)") : TEXT(""), Rate);
			}
		}
	}
	// automatic doors (sector flag 0x100, SECFLAG1_DOOR; 0x200 swings the other way) have no INF: the
	// Jedi engine makes them elevators itself (held shut, a nudge from either side opens it; in Outlaws it
	// stays open until nudged again, it does not close by itself). Outlaws doors swing: a MORPH_SPIN a quarter turn about one end
	// of the leaf's longest wall (the hinge end, the speed and the sound are INFERRED, not traced in the exe)
	AutoDoors.Reset();
	for (const FOutlawsSector& S : Live.Sectors)
	{
		if (!(S.Flags & OutlawsFlags::SectorDoor) || ElevatorsBySector.Contains(S.Index) || S.Walls.Num() == 0)
		{
			continue;
		}
		int32 Long = 0;
		double Best = -1.0;
		for (int32 w = 0; w < S.Walls.Num(); ++w)
		{
			const double Len = FVector2D::Distance(S.Verts[S.Walls[w].V1], S.Verts[S.Walls[w].V2]);
			if (Len > Best) { Best = Len; Long = w; }
		}
		FOutlawsInfItem It;
		It.Name = S.Name.IsEmpty() ? FString::Printf(TEXT("AUTODOOR%d"), S.Index) : S.Name;
		FOutlawsInfClass C;
		C.Class = EOutlawsInfClass::MorphSpin;
		C.RawClass = TEXT("AUTO_DOOR");
		C.EventMask = OutlawsInf::EvNudgeFront | OutlawsInf::EvNudgeBack;
		C.bHasEventMask = true;
		C.Speed = 90.0;
		C.bHasSpeed = true;
		C.Center = S.Verts[S.Walls[Long].V1];
		C.bHasCenter = true;
		FOutlawsInfStop Shut;
		Shut.Value = 0.0;
		Shut.Action = EOutlawsStopAction::Hold;
		FOutlawsInfStop Open;
		Open.Value = (S.Flags & OutlawsFlags::SectorDoorReverse) ? -90.0 : 90.0;
		Open.Action = EOutlawsStopAction::Hold;      // stays open until nudged again (user 2026-09-27: no timed close)
		C.Stops.Add(Shut);
		C.Stops.Add(Open);
		C.Sounds.Add(1, TEXT("DOOR3.WAV"));
		It.Classes.Add(C);
		FOutlawsElevator E;
		E.Item = Inf.Items.Add(It);
		E.ClassIndex = 0;
		E.Class = C.Class;
		E.Name = It.Name;
		E.Speed = C.Speed;
		E.Sectors.Add(S.Index);
		E.BaseFloor.Add(S.Floor.Y);
		E.BaseCeil.Add(S.Ceil.Y);
		ElevatorsBySector.FindOrAdd(S.Index).Add(Elevators.Add(E));
		AutoDoors.Add(S.Index);
	}
	// (REQ-PIANO) the scroll-gating lights, last of all so no other elevator's index moves; the SEQ's one SPEED (60, given
	// with the scroll) is theirs too.  A save from before them holds only the elevators up to here
	SavedElevatorsBefore = Elevators.Num();
	for (const FIntVector4& G : GatedLights)
	{
		const FOutlawsInfItem& It = Inf.Items[G.Y];
		const FOutlawsInfClass& C = It.Classes[G.Z];
		FOutlawsElevator E;
		E.Item = G.Y;
		E.ClassIndex = G.Z;
		E.Class = C.Class;
		E.Name = It.Name;
		E.bMasterOn = C.bMasterOn;
		E.Speed = C.bHasSpeed ? C.Speed : 0.0;
		for (const FOutlawsInfClass& O : It.Classes) if (!C.bHasSpeed && O.bHasSpeed) { E.Speed = O.Speed; break; }
		E.Sectors.Add(G.W);
		E.BaseFloor.Add(Live.Sectors[G.W].Floor.Y);
		E.BaseCeil.Add(Live.Sectors[G.W].Ceil.Y);
		E.BaseLight.Add(Live.Sectors[G.W].Ambient);
		Scrolls[G.X].Light = Elevators.Add(E);
		ElevatorsBySector.FindOrAdd(G.W).Add(Scrolls[G.X].Light);
	}
	// (REQ-SECTORLIGHT, user 2026-10-28, snapshot 17: shsimms's campfires did not flicker) every other CHANGE_LIGHT, last of
	// all so no earlier elevator's index moves.  As the Jedi engine runs them (TFE inf_allocateElevItem, updateElevator,
	// infUpdate_changeAmbient): the elevator's value is its master's AMBIENT, moved between the stops at SPEED light levels
	// a second (32 when the INF gives none), each slave's AMBIENT moved by as much; a SPEED of 0 or less jumps, as every
	// elevator here (gallery's FLICKER -100, hideout's ALIEN1 -200: TFE's arithmetic would run them off their stops for
	// good).  They are kept apart from the sectors' elevators (LightsBySector): messages reach them (gallery's targets, indy's
	// LIGHT; no light in any level takes a player's event: EVENT_MASK 0 or none), and no door, lift or drop logic counts
	// their sectors as moving
	LightsFrom = SectorLights.Num() > 0 ? Elevators.Num() : -1;
	for (const FIntVector& Sl : SectorLights)
	{
		const FOutlawsInfItem& It = Inf.Items[Sl.X];
		const FOutlawsInfClass& C = It.Classes[Sl.Y];
		FOutlawsElevator E;
		E.Item = Sl.X;
		E.ClassIndex = Sl.Y;
		E.Class = C.Class;
		E.Name = It.Name;
		E.bMasterOn = C.bMasterOn;
		E.Speed = C.bHasSpeed ? C.Speed : 32.0;
		E.Sectors.Add(Sl.Z);
		for (const FString& SlaveName : C.Slaves)
		{
			FString N, Id;
			FOutlawsInfFile::SplitNameId(SlaveName, N, Id);
			TArray<int32> SlaveSecs;
			FindSectors(N, Id, SlaveSecs);
			for (int32 s : SlaveSecs) E.Sectors.AddUnique(s);
		}
		for (int32 s : E.Sectors)
		{
			E.BaseFloor.Add(Live.Sectors[s].Floor.Y);
			E.BaseCeil.Add(Live.Sectors[s].Ceil.Y);
			E.BaseLight.Add(Live.Sectors[s].Ambient);
		}
		LightsBySector.FindOrAdd(Sl.Z).Add(Elevators.Add(E));
	}
	// every elevator jumps to its START stop, then that stop's delay applies
	for (FOutlawsElevator& E : Elevators)
	{
		const FOutlawsInfClass& C = ClassOf(E);
		const int32 Start = FMath::Clamp(C.StartStop, 0, C.Stops.Num() - 1);
		E.Stop = Start;
		E.Current = StopValue(E, Start);
		Apply(E);
		const FOutlawsInfStop& S0 = C.Stops[Start];
		if (S0.Action == EOutlawsStopAction::Wait) { E.bWaiting = true; E.WaitLeft = S0.Wait; }
		else if (S0.Action == EOutlawsStopAction::Terminate) E.bTerminated = true;
	}
}

double FOutlawsInfRuntime::StopValue(const FOutlawsElevator& E, int32 StopIndex) const
{
	const FOutlawsInfClass& C = ClassOf(E);
	if (!C.Stops.IsValidIndex(StopIndex))
	{
		return E.Current;
	}
	const FOutlawsInfStop& S = C.Stops[StopIndex];
	double Base = 0.0;
	if (E.IsVertical())
	{
		Base = E.Class == EOutlawsInfClass::MoveCeiling ? E.BaseCeil[0] : E.BaseFloor[0];
	}
	if (!S.SectorRef.IsEmpty())
	{
		// a sector name as the stop: that sector's floor height (Jedi uses the floor for every class)
		TArray<int32> Ref;
		FindSectors(S.SectorRef, FString(), Ref);
		return Ref.Num() > 0 ? Live.Sectors[Ref[0]].Floor.Y : Base;
	}
	// "@n" is relative for floors and ceilings; slides take n as is, and spins read 0 (a Jedi parser bug)
	if (S.bRelative && E.Class == EOutlawsInfClass::MorphSpin) return 0.0;
	return (S.bRelative && E.IsVertical()) ? Base + S.Value : S.Value;
}

void FOutlawsInfRuntime::Apply(FOutlawsElevator& E)
{
	if (E.IsVertical())
	{
		const bool bFloor = E.Class == EOutlawsInfClass::MoveFloor || E.Class == EOutlawsInfClass::MoveFC;
		const bool bCeil = E.Class == EOutlawsInfClass::MoveCeiling || E.Class == EOutlawsInfClass::MoveFC;
		const double Delta = E.Current - (E.Class == EOutlawsInfClass::MoveCeiling ? E.BaseCeil[0] : E.BaseFloor[0]);
		if (SectorHeightGen.Num() != Live.Sectors.Num()) SectorHeightGen.Init(0, Live.Sectors.Num());
		for (int32 i = 0; i < E.Sectors.Num(); ++i)
		{
			FOutlawsSector& S = Live.Sectors[E.Sectors[i]];
			const double F0 = S.Floor.Y, C0 = S.Ceil.Y;
			if (bFloor) S.Floor.Y = E.BaseFloor[i] + Delta;
			if (bCeil) S.Ceil.Y = E.BaseCeil[i] + Delta;
			if (S.Floor.Y != F0 || S.Ceil.Y != C0) SectorHeightGen[E.Sectors[i]] = ++HeightGen;     // (QSPEED-3: the flood's cache reads it)
		}
	}
	else if (E.IsLight() && E.BaseLight.Num() == E.Sectors.Num() && E.BaseLight.Num() > 0 && SectorLightsOn())
	{
		// (REQ-SECTORLIGHT) the master's AMBIENT is the value (drawn by its whole part, as Jedi's floor16), each slave's moved
		// by as much
		const int32 Delta = FMath::FloorToInt(E.Current) - E.BaseLight[0];
		for (int32 i = 0; i < E.Sectors.Num(); ++i) Live.Sectors[E.Sectors[i]].Ambient = E.BaseLight[i] + Delta;
	}
	if (OnElevatorMoved)
	{
		OnElevatorMoved(E);
	}
}

void FOutlawsInfRuntime::StartMove(FOutlawsElevator& E, int32 TargetStop)
{
	const FOutlawsInfClass& C = ClassOf(E);
	if (E.bTerminated || !C.Stops.IsValidIndex(TargetStop))
	{
		return;
	}
	const bool bWasMoving = E.Target >= 0;
	E.Target = TargetStop;
	E.bWaiting = false;
	if (!bWasMoving && FMath::Abs(StopValue(E, TargetStop) - E.Current) > 1e-9)
	{
		if (OnSound) if (const FString* S1 = C.Sounds.Find(1)) OnSound(*S1, E);
		if (OnLoopSound) if (const FString* S2 = C.Sounds.Find(2)) OnLoopSound(*S2, E, true);
	}
}

void FOutlawsInfRuntime::Arrive(FOutlawsElevator& E)
{
	const FOutlawsInfClass& C = ClassOf(E);
	E.Stop = E.Target;
	E.Target = -1;
	const FOutlawsInfStop& S = C.Stops[E.Stop];
	// a stop with a delay ends the move (sound 3); a zero wait rolls straight on
	if (!(S.Action == EOutlawsStopAction::Wait && S.Wait <= 0.0))
	{
		if (OnLoopSound) if (const FString* S2 = C.Sounds.Find(2)) OnLoopSound(*S2, E, false);
		if (OnSound) if (const FString* S3 = C.Sounds.Find(3)) OnSound(*S3, E);
	}
	switch (S.Action)
	{
	case EOutlawsStopAction::Terminate:
		E.bTerminated = true;
		return;                      // the elevator is gone: its stop messages are not sent
	case EOutlawsStopAction::Complete:
		E.bTerminated = true;
		if (OnLevelEvent) OnLevelEvent(TEXT("COMPLETE"));
		return;
	case EOutlawsStopAction::Wait:
		E.bWaiting = true;
		E.WaitLeft = S.Wait;
		break;
	default:
		break;                       // HOLD: sleep until a trigger or message
	}
	for (const FOutlawsInfMessage& M : S.Messages)
	{
		if (M.Type == EOutlawsInfMsg::UserMsg && M.Target.IsEmpty())
		{
			if (OnUserMessage) OnUserMessage(M.Arg);
		}
		else
		{
			Send(M.Target, M.TargetId, M);
		}
	}
}

void FOutlawsInfRuntime::Tick(double Dt)
{
	OL_PERF_SCOPE("INF runtime");
	if (OnForward)
	{
		TickPuppet(Dt);                // (co-op client: the host's INF decides)
		return;
	}
	for (FOutlawsScroll& Sc : Scrolls)
	{
		if (Elevators.IsValidIndex(Sc.Elevator))
		{
			Sc.Offset = Sc.Dir * (Elevators[Sc.Elevator].Current / 8.0);
		}
		else if (ScrollRuns(Sc))
		{
			Sc.Offset += Sc.Dir * Sc.Speed * Dt;
		}
	}
	for (int32 i = 0; i < Elevators.Num(); ++i)
	{
		FOutlawsElevator& E = Elevators[i];
		if (E.bTerminated || !E.bMasterOn)
		{
			continue;                // MASTER_OFF freezes moves and waits alike
		}
		const int32 N = NumStops(E);
		if (E.Target >= 0)
		{
			const double Goal = StopValue(E, E.Target);
			const double Step = E.Speed > 0.0 ? E.Speed * Dt : TNumericLimits<double>::Max();
			const double Diff = Goal - E.Current;
			if (FMath::Abs(Diff) <= Step)
			{
				E.Current = Goal;
				Apply(E);
				Arrive(E);
			}
			else
			{
				E.Current += FMath::Sign(Diff) * Step;
				Apply(E);
			}
		}
		else if (E.bWaiting)
		{
			// (REQ-SECTORLIGHT) a light keeps the INF's own rate: the player's pace stretches the doors' waits, not a flicker
			E.WaitLeft -= Dt / (E.IsLight() ? 1.0 : FMath::Max(1.0, WaitScale));
			if (E.WaitLeft <= 0.0)
			{
				E.bWaiting = false;
				StartMove(E, (E.Stop + 1) % N);
			}
		}
	}
}

void FOutlawsInfRuntime::ElevatorMessage(FOutlawsElevator& E, const FOutlawsInfMessage& Msg, bool bKeyChecked)
{
	const FOutlawsInfClass& C = ClassOf(E);
	const int32 N = C.Stops.Num();
	if (Msg.Type == EOutlawsInfMsg::MasterOn)
	{
		E.bMasterOn = true;
		return;
	}
	if (!E.bMasterOn || E.bTerminated || N == 0)
	{
		return;
	}
	// locked: the player (or a trigger they set off) needs the item; stop messages pass
	if (bKeyChecked && C.KeyObject >= 0)
	{
		const bool bHasIt = HasItem(C.KeyObject);
		if (OnKeyCheck) OnKeyCheck(C.KeyObject, bHasIt, E);
		if (!bHasIt)
		{
			return;
		}
		UseUpKey(C);
	}
	switch (Msg.Type)
	{
	case EOutlawsInfMsg::MasterOff:
		if (E.Target >= 0)
		{
			if (OnLoopSound) if (const FString* S2 = C.Sounds.Find(2)) OnLoopSound(*S2, E, false);
			if (OnSound) if (const FString* S3 = C.Sounds.Find(3)) OnSound(*S3, E);
		}
		E.bMasterOn = false;         // frozen in place; MASTER_ON resumes the same move
		break;
	case EOutlawsInfMsg::Trigger:
		if (E.bTrigLast)
		{
			// MOVE_CEILING: straight to the last stop (a door that is open stays open)
			if (E.Target >= 0 || FMath::Abs(StopValue(E, N - 1) - E.Current) > 1e-9) StartMove(E, N - 1);
		}
		else if (E.Target < 0)
		{
			StartMove(E, (E.Stop + 1) % N);   // on to the queued stop, cutting a wait short
		}
		break;
	case EOutlawsInfMsg::NextStop:
		StartMove(E, E.Target >= 0 ? (E.Target + 1) % N : (E.Stop + 1) % N);
		break;
	case EOutlawsInfMsg::PrevStop:
		StartMove(E, E.Target >= 0 ? (E.Target + N - 1) % N : (E.Stop + N - 1) % N);
		break;
	case EOutlawsInfMsg::GotoStop:
		if (C.Stops.IsValidIndex(Msg.Arg) && FMath::Abs(StopValue(E, Msg.Arg) - E.Current) > 1e-9)
		{
			StartMove(E, Msg.Arg);
		}
		break;
	default:
		break;
	}
}

void FOutlawsInfRuntime::TriggerMessage(FOutlawsTrigger& T, const FOutlawsInfMessage& Msg)
{
	switch (Msg.Type)
	{
	case EOutlawsInfMsg::Trigger: FireTrigger(T); break;
	case EOutlawsInfMsg::MasterOn: T.bMaster = true; break;
	case EOutlawsInfMsg::MasterOff: T.bMaster = false; break;
	case EOutlawsInfMsg::Done: T.bMaster = true; break;    // re-arms a SWITCH1
	default: break;
	}
}

void FOutlawsInfRuntime::Send(const FString& Target, const FString& TargetId, const FOutlawsInfMessage& Msg, bool bKeyChecked)
{
	if (Msg.Type == EOutlawsInfMsg::UserMsg)
	{
		if (OnUserMessage) OnUserMessage(Msg.Arg);
		return;
	}
	if (Msg.Type == EOutlawsInfMsg::EndLevel || Msg.Type == EOutlawsInfMsg::SpawnLevel || Msg.Type == EOutlawsInfMsg::Movie)
	{
		if (OnLevelEvent) OnLevelEvent(Msg.Type == EOutlawsInfMsg::EndLevel ? FString(TEXT("END_LEVEL"))
			: (Msg.Type == EOutlawsInfMsg::Movie ? TEXT("TRIGGER_MOVIE ") : TEXT("SPAWN_LEVEL ")) + Msg.Target);
		return;
	}
	if ((Target.IsEmpty() && TargetId.IsEmpty()) || GSendDepth >= MaxSendDepth)
	{
		return;
	}
	TGuardValue<int32> Depth(GSendDepth, GSendDepth + 1);
	// "NAME(#WALLID)": the triggers on that wall
	if (!Target.IsEmpty() && !TargetId.IsEmpty())
	{
		TArray<int32> Named;
		FindSectors(Target, FString(), Named);
		bool bWall = false;
		auto ToWall = [&](int32 s, int32 w)
		{
			bWall = true;
			if (const TArray<int32>* Ts = TriggersByWall.Find(WallKey(s, w)))
			{
				for (int32 t : *Ts) TriggerMessage(Triggers[t], Msg);
			}
			if (OnWallMessage) OnWallMessage(s, w, Msg);     // the wall's animated texture takes NEXT_STOP / GOTO_STOP
		};
		for (int32 s : Named)
		{
			const FOutlawsSector& S = Live.Sectors[s];
			for (int32 w = 0; w < S.Walls.Num(); ++w)
			{
				if (S.Walls[w].Id.Equals(TargetId, ESearchCase::IgnoreCase)) ToWall(s, w);
			}
		}
		if (!bWall && !TargetId.IsEmpty() && FChar::IsDigit(TargetId[0]))
		{
			// "NAME(21)" with no '#': the wall's index in the sector, as a line item's bare NUM (the cliff's bar switches
			// CSDS1(20..22) and round-key slot BTRIG(16); user 2026-09-29: the bar switches never showed a move)
			const int32 Idx = FCString::Atoi(*TargetId);
			for (int32 s : Named)
			{
				if (Live.Sectors[s].Walls.IsValidIndex(Idx)) ToWall(s, Idx);
			}
		}
		if (bWall)
		{
			return;
		}
	}
	// (BUG-HOMCELL) "!#B05B": an object by its id (the office's cell generators take MASTER_ON)
	if (Target.IsEmpty() && !TargetId.IsEmpty() && OnObjectMessage) OnObjectMessage(TargetId, Msg);
	TArray<int32> Secs;
	FindSectors(Target, TargetId, Secs);
	for (int32 s : Secs)
	{
		if (const TArray<int32>* Es = ElevatorsBySector.Find(s))
		{
			for (int32 e : *Es) ElevatorMessage(Elevators[e], Msg, bKeyChecked);
		}
		if (const TArray<int32>* Ls = LightsBySector.Find(s))
		{
			for (int32 e : *Ls) ElevatorMessage(Elevators[e], Msg, bKeyChecked);     // (REQ-SECTORLIGHT: gallery's targets, indy's LIGHT)
		}
		if (const TArray<int32>* Ts = TriggersBySector.Find(s))
		{
			for (int32 t : *Ts) TriggerMessage(Triggers[t], Msg);
		}
	}
}

bool FOutlawsInfRuntime::TriggerAllowed(const FOutlawsTrigger& T) const
{
	// OBJECT_EXCLUDE: only without the item (the "You need the brass key." line); OBJECT: only with it
	const FOutlawsInfClass& C = ClassOf(T);
	return !((C.ExcludeObject >= 0 && HasItem(C.ExcludeObject)) || (C.KeyObject >= 0 && !HasItem(C.KeyObject)));
}

void FOutlawsInfRuntime::UseUpKey(const FOutlawsInfClass& C)
{
	// OBJECT: n 589824 (0x90000): the item goes into the lock and out of the pockets (the mill's gears, one a slot; the
	// cliff's stone and round keys; the wharf's lever and crank).  Keys and tools (65536 / 196608) stay (user 2026-09-29:
	// one gear worked all three slots)
	if (C.KeyObject >= 0 && (C.KeyObjectFlags & ObjUseUp) && OnItemUsedUp) OnItemUsedUp(C.KeyObject);
}

void FOutlawsInfRuntime::FireTriggers(const TArray<int32>& Ts, uint32 Event, bool& bTaken)
{
	// every item check goes against the pockets as they were before any trigger of the event fired: a slot's OBJECT
	// class takes the last gear, and its OBJECT_EXCLUDE class ("you need a gear") must not answer that same push
	TArray<int32> Fire;
	for (int32 t : Ts)
	{
		if ((Triggers[t].Mask & Event) && Triggers[t].bMaster)
		{
			bTaken = true;
			if (TriggerAllowed(Triggers[t])) Fire.Add(t);
		}
	}
	for (int32 t : Fire) FireTrigger(Triggers[t], true);
}

void FOutlawsInfRuntime::FireTrigger(FOutlawsTrigger& T, bool bAllowed)
{
	const FOutlawsInfClass& C = ClassOf(T);
	if (!T.bMaster || (!bAllowed && !TriggerAllowed(T)))
	{
		return;
	}
	UseUpKey(C);
	if (T.bOneShot)
	{
		T.bMaster = false;
	}
	// each MESSAGE line goes to the CLIENT just before it (the level files pair them so: a bar switch steps its counter
	// once and its own texture once; user 2026-09-29: the counters and the gear dummy stepped twice a push); a client
	// with no message of its own takes the class-wide ones, or the plain trigger
	FOutlawsInfMessage Default;          // no MESSAGE line: m_trigger to the clients
	auto Deliver = [&](const FString& Client, const FOutlawsInfMessage& M)
	{
		const FOutlawsInfMessage& Out = M;
		FString N, Id;
		FOutlawsInfFile::SplitNameId(Client, N, Id);
		if (M.Type == EOutlawsInfMsg::UserMsg || M.Type == EOutlawsInfMsg::EndLevel || M.Type == EOutlawsInfMsg::SpawnLevel || N.Equals(TEXT("SYSTEM"), ESearchCase::IgnoreCase))
		{
			Send(FString(), FString(), Out);
			return;
		}
		Send(N, Id, Out, true);
	};
	for (int32 c = 0; c < C.Clients.Num(); ++c)
	{
		if (C.ClientHasMsg.IsValidIndex(c) && C.ClientHasMsg[c]) Deliver(C.Clients[c], C.ClientMsg[c]);
		else if (C.TriggerMessages.Num() > 0) for (const FOutlawsInfMessage& M : C.TriggerMessages) Deliver(C.Clients[c], M);
		else Deliver(C.Clients[c], Default);
	}
	if (C.Clients.Num() == 0)
	{
		for (const FOutlawsInfMessage& M : C.TriggerMessages) Deliver(FString(), M);
	}
}

bool FOutlawsInfRuntime::WallSlides(int32 Sector, int32 Wall) const
{
	// a switch whose client steps through more than two stops (the cliff's bar counters): the hand may slide it either way
	const TArray<int32>* Ts = TriggersByWall.Find(WallKey(Sector, Wall));
	if (!Ts) return false;
	for (int32 t : *Ts)
	{
		for (const FString& Client : ClassOf(Triggers[t]).Clients)
		{
			FString N, Id;
			FOutlawsInfFile::SplitNameId(Client, N, Id);
			TArray<int32> Secs;
			FindSectors(N, FString(), Secs);
			for (int32 s : Secs)
			{
				if (const TArray<int32>* Es = ElevatorsBySector.Find(s))
				{
					for (int32 e : *Es) if (ClassOf(Elevators[e]).IsElevator() && NumStops(Elevators[e]) > 2) return true;
				}
			}
		}
	}
	return false;
}

bool FOutlawsInfRuntime::SectorEvent(int32 Sector, uint32 Event)
{
	if (OnForward)
	{
		return ForwardEvent(Sector, -1, Event);       // (co-op client: the host runs it)
	}
	if (OnEventGate && !OnEventGate(Sector, Event)) return false;
	if (OnEventScope) OnEventScope(Sector, -1, Event, 0);
	TGuardValue<int32> Depth(SectorEventDepth, SectorEventDepth + 1);
	TGuardValue<bool> Hold(bHoldThisEvent, false);
	bool bTaken = false;
	if (const TArray<int32>* Ts = TriggersBySector.Find(Sector))
	{
		FireTriggers(*Ts, Event, bTaken);
	}
	if (const TArray<int32>* Es = ElevatorsBySector.Find(Sector))
	{
		FOutlawsInfMessage Trig;
		for (int32 e : *Es)
		{
			FOutlawsElevator& E = Elevators[e];
			const FOutlawsInfClass& EC = ClassOf(E);
			if ((EC.EventMask & Event) && (Event != OutlawsInf::EvShoot || EC.bHasEventMask) && E.bMasterOn && !E.bTerminated)
			{
				bTaken = true;
				if (bHoldThisEvent && EC.Stops.Num() > 0)
				{
					// (BUG-RANCHDOOR) the lock answers now; the move waits for ReleaseHeld
					if (EC.KeyObject >= 0)
					{
						const bool bHasIt = HasItem(EC.KeyObject);
						if (OnKeyCheck) OnKeyCheck(EC.KeyObject, bHasIt, E);
						if (!bHasIt) continue;
						UseUpKey(EC);
					}
					HeldStarts.AddUnique(e);
					continue;
				}
				ElevatorMessage(E, Trig, true);
			}
		}
	}
	if (OnEventScope) OnEventScope(Sector, -1, Event, bTaken ? 2 : 1);
	return bTaken;
}

bool FOutlawsInfRuntime::HoldEventStarts()
{
	if (SectorEventDepth <= 0) return false;
	bHoldThisEvent = true;
	return true;
}

void FOutlawsInfRuntime::ReleaseHeld()
{
	// the held moves start as the use would have started them (the lock was answered then)
	FOutlawsInfMessage Trig;
	TArray<int32> Held = MoveTemp(HeldStarts);
	HeldStarts.Reset();
	for (const int32 e : Held)
	{
		if (Elevators.IsValidIndex(e)) ElevatorMessage(Elevators[e], Trig, false);
	}
}

bool FOutlawsInfRuntime::WallEvent(int32 Sector, int32 Wall, uint32 Event)
{
	if (OnForward)
	{
		return ForwardEvent(Sector, Wall, Event);     // (co-op client: the host runs it)
	}
	if (OnEventScope) OnEventScope(Sector, Wall, Event, 0);
	bool bTaken = false;
	if (const TArray<int32>* Ts = TriggersByWall.Find(WallKey(Sector, Wall)))
	{
		FireTriggers(*Ts, Event, bTaken);
	}
	if (OnEventScope) OnEventScope(Sector, Wall, Event, bTaken ? 2 : 1);
	return bTaken;
}

bool FOutlawsInfRuntime::ForwardEvent(int32 Sector, int32 Wall, uint32 Event)
{
	// co-op client: what the host's copy would take, by the same masks (nothing changes here).  Anything that listens for
	// the event by its mask is told to the host, armed or not here (the arming may be a snapshot behind)
	bool bAny = false, bTaken = false;
	auto Look = [&](const TArray<int32>* Ts)
	{
		if (!Ts) return;
		for (const int32 t : *Ts)
		{
			if (!(Triggers[t].Mask & Event)) continue;
			bAny = true;
			bTaken |= Triggers[t].bMaster;
		}
	};
	if (Wall >= 0)
	{
		Look(TriggersByWall.Find(WallKey(Sector, Wall)));
	}
	else
	{
		Look(TriggersBySector.Find(Sector));
		if (const TArray<int32>* Es = ElevatorsBySector.Find(Sector))
		{
			for (const int32 e : *Es)
			{
				const FOutlawsElevator& E = Elevators[e];
				const FOutlawsInfClass& EC = ClassOf(E);
				if (!(EC.EventMask & Event) || (Event == OutlawsInf::EvShoot && !EC.bHasEventMask)) continue;
				bAny = true;
				bTaken |= E.bMasterOn && !E.bTerminated;
			}
		}
	}
	if (bAny) OnForward(Sector, Wall, Event);
	return bTaken;
}

void FOutlawsInfRuntime::TickPuppet(double Dt)
{
	// co-op client: the textures scroll as the host's do; a moving elevator eases on toward the stop the host gave it at
	// its own speed and halts there (the host's word says what follows: its stop, a wait, the next move)
	for (FOutlawsScroll& Sc : Scrolls)
	{
		if (Elevators.IsValidIndex(Sc.Elevator)) Sc.Offset = Sc.Dir * (Elevators[Sc.Elevator].Current / 8.0);
		else if (ScrollRuns(Sc)) Sc.Offset += Sc.Dir * Sc.Speed * Dt;
	}
	for (FOutlawsElevator& E : Elevators)
	{
		if (E.bTerminated || !E.bMasterOn || E.Target < 0) continue;
		const double Goal = StopValue(E, E.Target);
		const double Diff = Goal - E.Current;
		if (FMath::Abs(Diff) < 1e-9) continue;
		const double Step = E.Speed > 0.0 ? E.Speed * Dt : TNumericLimits<double>::Max();
		E.Current = FMath::Abs(Diff) <= Step ? Goal : E.Current + FMath::Sign(Diff) * Step;
		Apply(E);
	}
}

bool FOutlawsInfRuntime::ScrollRuns(const FOutlawsScroll& Sc) const
{
	if (!Elevators.IsValidIndex(Sc.Light)) return true;
	const FOutlawsElevator& L = Elevators[Sc.Light];
	return FMath::Abs(L.Current - StopValue(L, 0)) > 1e-6;
}

void FOutlawsInfRuntime::SetFromHost(int32 Index, double Value, int32 Stop, int32 Target, bool bWaiting, bool bTerminated, bool bMasterOn)
{
	if (!Elevators.IsValidIndex(Index)) return;
	FOutlawsElevator& E = Elevators[Index];
	const bool bSameMove = Target >= 0 && E.Target == Target;
	E.Stop = FMath::Clamp(Stop, 0, FMath::Max(0, NumStops(E) - 1));
	E.Target = Target >= 0 && Target < NumStops(E) ? Target : -1;
	E.bWaiting = bWaiting;
	E.bTerminated = bTerminated;
	E.bMasterOn = bMasterOn;
	const double Tol = FMath::Max(E.Speed, 1.0) * 0.25;
	if ((!bSameMove || FMath::Abs(E.Current - Value) > Tol) && E.Current != Value)
	{
		E.Current = Value;
		Apply(E);
	}
}

void FOutlawsInfRuntime::RequiredObjects(TSet<int32>& Out) const
{
	for (const FOutlawsInfItem& It : Inf.Items)
	{
		for (const FOutlawsInfClass& C : It.Classes)
		{
			if (C.KeyObject >= 0) Out.Add(C.KeyObject);
			if (C.ExcludeObject >= 0) Out.Add(C.ExcludeObject);
		}
	}
}

FString FOutlawsInfRuntime::Summary() const
{
	TMap<FString, int32> Kinds;
	for (const FOutlawsElevator& E : Elevators)
	{
		Kinds.FindOrAdd(ClassOf(E).RawClass.ToUpper())++;
	}
	TArray<FString> Parts;
	for (const TPair<FString, int32>& Kv : Kinds) Parts.Add(FString::Printf(TEXT("%s=%d"), *Kv.Key, Kv.Value));
	Parts.Sort();
	int32 LineTriggers = 0;
	for (const FOutlawsTrigger& T : Triggers) LineTriggers += T.bLine ? 1 : 0;
	return FString::Printf(TEXT("INF: %d items, %d elevators [%s], %d automatic doors, %d scrolls, %d triggers (%d on walls), %d warnings"),
		Inf.Items.Num(), Elevators.Num(), *FString::Join(Parts, TEXT(", ")), AutoDoors.Num(), Scrolls.Num(), Triggers.Num(), LineTriggers, Warnings.Num());
}
