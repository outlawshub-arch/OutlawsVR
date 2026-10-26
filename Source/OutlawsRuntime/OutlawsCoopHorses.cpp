// Co-op horses (REQ-COOPHORSES, user 2026-10-05): on a story or Historical Missions level that starts beside James's horse
// (never the office), each other player has a horse of its own beside his, the same sprite, facing the same way.
//   the places: worked out from the level's data as it is built (its doors and lifts at their first stops), so every machine
//   has the same ones: beside the horse in its sector or a neighbouring one, on a flat floor not far above or below its own,
//   the whole footprint on that floor and clear of walls, the other horses, the players' start spots and the objects there;
//   slot 1 for player 2 and so on, the nearest first;
//   the slots: the host gives each joiner the lowest free one (its link carries it to everyone), so a later joiner takes the
//   place of one who left; each machine makes the horse for each link's slot and takes it away when the link goes;
//   a copy of a horse that is worked by hand (HORSEND, the missions' exit) is a nudge prop whose event goes to James's
//   horse's sector, so gripping any player's horse ends the level as his does (the outlaw's fate settled as he went down).
//
// None of this runs in single player or in a match.
#include "OutlawsCoop.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsLevel.h"
#include "OutlawsLab.h"
#include "OutlawsFormats.h"
#include "OutlawsTextures.h"
#include "OutlawsGeometry.h"
#include "OutlawsInf.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsEnemy.h"
#include "Components/CapsuleComponent.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"

namespace
{
	// a horse's footprint (Outlaws units, 30 cm): its length along its facing, its width across
	constexpr double HorseHalfLength = 4.0;
	constexpr double HorseHalfWidth = 1.5;
	constexpr double HorseGap = 0.6;              // between two horses side by side
	constexpr double WallClear = 0.5;             // from a wall or a step
	constexpr double StepTall = 1.0;              // a step this tall or less is no wall
	constexpr double MaxFloorDiff = 3.5;          // a slot's floor from the horse's (the hideout's horse stands on a ledge 3 units up)
	constexpr double HorseTall = 7.0;             // the room a horse needs over its floor
	constexpr double SpotClear = 1.5;             // from a player's start spot
	constexpr double ObjectClear = 1.0;           // from a pickup or any other object on the floor there
	constexpr double OtherHorseClear = 4.0;       // from the level's other horses (their own facing)
	constexpr double StartReach = 40.0;           // James's horse is within this of the player's start (12 m)

	bool IsHorseObjectName(const FString& N) { return N.StartsWith(TEXT("HOR")); }

	double HorsePointBoxDist(const FVector2D& P, double Hx, double Hy)
	{
		const double Dx = FMath::Max(FMath::Abs(P.X) - Hx, 0.0), Dy = FMath::Max(FMath::Abs(P.Y) - Hy, 0.0);
		return FMath::Sqrt(Dx * Dx + Dy * Dy);
	}
	double HorsePointSegDist(const FVector2D& P, const FVector2D& A, const FVector2D& B)
	{
		const FVector2D Ab = B - A;
		const double T = FMath::Clamp(FVector2D::DotProduct(P - A, Ab) / FMath::Max(FVector2D::DotProduct(Ab, Ab), 1e-9), 0.0, 1.0);
		return (A + Ab * T - P).Size();
	}
	bool HorseSegHitsBox(const FVector2D& A, const FVector2D& B, double Hx, double Hy)
	{
		// Liang-Barsky against the box [-Hx, Hx] x [-Hy, Hy]
		double T0 = 0.0, T1 = 1.0;
		const FVector2D D = B - A;
		const double P[4] = { -D.X, D.X, -D.Y, D.Y };
		const double Q[4] = { A.X + Hx, Hx - A.X, A.Y + Hy, Hy - A.Y };
		for (int32 i = 0; i < 4; ++i)
		{
			if (FMath::Abs(P[i]) < 1e-12) { if (Q[i] < 0.0) return false; continue; }
			const double T = Q[i] / P[i];
			if (P[i] < 0.0) T0 = FMath::Max(T0, T); else T1 = FMath::Min(T1, T);
			if (T0 > T1) return false;
		}
		return true;
	}
	double HorseSegBoxDist(const FVector2D& A, const FVector2D& B, double Hx, double Hy)
	{
		if (HorseSegHitsBox(A, B, Hx, Hy)) return 0.0;
		double D = FMath::Min(HorsePointBoxDist(A, Hx, Hy), HorsePointBoxDist(B, Hx, Hy));
		for (const FVector2D C : { FVector2D(Hx, Hy), FVector2D(-Hx, Hy), FVector2D(Hx, -Hy), FVector2D(-Hx, -Hy) }) D = FMath::Min(D, HorsePointSegDist(C, A, B));
		return D;
	}
	bool HorseSegsCross(const FVector2D& A, const FVector2D& B, const FVector2D& C, const FVector2D& D)
	{
		auto Cross = [](const FVector2D& U, const FVector2D& V) { return U.X * V.Y - U.Y * V.X; };
		const double D1 = Cross(B - A, C - A), D2 = Cross(B - A, D - A), D3 = Cross(D - C, A - C), D4 = Cross(D - C, B - C);
		return ((D1 > 0) != (D2 > 0)) && ((D3 > 0) != (D4 > 0));
	}
}

bool AOutlawsLevelActor::PlanHorseSlots(const FOutlawsLevel& Live, const TSet<int32>* Moving, const FString& Level, int32 Want, FHorsePlan& Out)
{
	Out = FHorsePlan();
	if (Level.Equals(TEXT("office"), ESearchCase::IgnoreCase))
	{
		Out.Why = TEXT("the office: no horses");
		return false;
	}
	// the player's start (the last PLAYER row on the map, as Build takes on a tie)
	const FOutlawsObject* Start = nullptr;
	for (const FOutlawsObject& O : Live.Objects)
	{
		if (O.Name == TEXT("PLAYER") && (Live.SectorById(O.SectorId) || Live.SectorAt(O.Pos.X, O.Pos.Y, O.Pos.Z))) Start = &O;
	}
	if (!Start)
	{
		Out.Why = TEXT("no player start");
		return false;
	}
	const FVector2D Start2(Start->Pos.X, Start->Pos.Z);
	// James's horse: the mission's exit (HORSEND) where there is one, else the horse nearest the start, if near
	const FOutlawsObject* Horse = nullptr;
	double Best = 1e30;
	for (int32 Pass = 0; Pass < 2 && !Horse; ++Pass)
	{
		for (const FOutlawsObject& O : Live.Objects)
		{
			if (!IsHorseObjectName(O.Name) || (Pass == 0) != (O.Name == TEXT("HORSEND"))) continue;
			const double D = (FVector2D(O.Pos.X, O.Pos.Z) - Start2).Size();
			if (D < Best) { Best = D; Horse = &O; }
		}
		if (Pass == 1 && Horse && Best > StartReach)
		{
			Out.Why = FString::Printf(TEXT("no horse at the start (the nearest, %s, is %.0f units away)"), *Horse->Name, Best);
			return false;
		}
	}
	if (!Horse)
	{
		Out.Why = TEXT("no horse");
		return false;
	}
	int32 Hs = Live.Which3D(Horse->Pos.X, Horse->Pos.Y, Horse->Pos.Z);
	if (Hs < 0)
	{
		if (const FOutlawsSector* S = Live.SectorAt(Horse->Pos.X, Horse->Pos.Y, Horse->Pos.Z)) Hs = S->Index;
	}
	if (!Live.Sectors.IsValidIndex(Hs))
	{
		Out.Why = TEXT("the horse is off the map");
		return false;
	}
	const FOutlawsSector& HorseSec = Live.Sectors[Hs];
	const double HorseFloor = Live.SurfaceY(HorseSec, FVector2D(Horse->Pos.X, Horse->Pos.Z), true);
	Out.Horse = Horse->Name;
	Out.Pos = FVector(Horse->Pos.X, HorseFloor, Horse->Pos.Z);
	Out.Yaw = Horse->Yaw;
	Out.Sector = Hs;
	Out.StartDist = Best;

	// the sectors a slot may stand in (the horse's and its neighbours) and those whose walls matter (theirs too)
	TSet<int32> Allowed, Near;
	Allowed.Add(Hs);
	for (const FOutlawsWall& W : HorseSec.Walls)
	{
		for (const int32 A : { W.Adjoin, W.DAdjoin }) if (Live.Sectors.IsValidIndex(A)) Allowed.Add(A);
	}
	Near = Allowed;
	for (const int32 s : Allowed)
	{
		for (const FOutlawsWall& W : Live.Sectors[s].Walls)
		{
			for (const int32 A : { W.Adjoin, W.DAdjoin }) if (Live.Sectors.IsValidIndex(A)) Near.Add(A);
		}
	}
	auto Standable = [&](const FOutlawsSector& S)
	{
		return !(Moving && Moving->Contains(S.Index)) && !S.SlopeFloor.bSet && S.Velocity.IsNearlyZero() && S.Ceil.Y - S.Floor.Y >= HorseTall;
	};
	// the sector of Near holding a point whose floor is nearest Y
	auto SectorFor = [&](const TSet<int32>& Set, const FVector2D& P, double Y) -> int32
	{
		int32 Found = -1;
		double Bd = 1e30;
		for (const int32 s : Set)
		{
			const FOutlawsSector& S = Live.Sectors[s];
			if (!Live.ContainsXZ(S, P.X, P.Y)) continue;
			const double D = FMath::Abs(S.Floor.Y - Y);
			if (D < Bd) { Bd = D; Found = s; }
		}
		return Found;
	};
	// the walls (and steps over StepTall, and openings too low for a horse) round the place
	struct FWallSeg { FVector2D A, B; bool bSolid; };
	TArray<FWallSeg> Walls;
	for (const int32 s : Near)
	{
		const FOutlawsSector& S = Live.Sectors[s];
		for (const FOutlawsWall& W : S.Walls)
		{
			if (!S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) continue;
			bool bBlock = !Live.Sectors.IsValidIndex(W.Adjoin);
			if (!bBlock)
			{
				const FOutlawsSector& O = Live.Sectors[W.Adjoin];
				bBlock = FMath::Abs(O.Floor.Y - S.Floor.Y) > StepTall || FMath::Min(O.Ceil.Y, S.Ceil.Y) - FMath::Max(O.Floor.Y, S.Floor.Y) < HorseTall
					|| (Moving && (Moving->Contains(O.Index) || Moving->Contains(S.Index)));
			}
			if (bBlock) Walls.Add({ S.Verts[W.V1], S.Verts[W.V2], !Live.Sectors.IsValidIndex(W.Adjoin) });
		}
	}
	// what a horse keeps clear of: the players' start spots (the start and the twelve a joiner may take, OnLevelBuilt),
	// the other horses, every other object on the floor there
	const double Sy = FMath::DegreesToRadians(Start->Yaw);
	const FVector2D SFwd(FMath::Sin(Sy), FMath::Cos(Sy)), SRight(FMath::Cos(Sy), -FMath::Sin(Sy));
	TArray<FVector2D> Spots;
	Spots.Add(Start2);
	{
		const FVector2D Ways[] = { SRight, -SRight, -SFwd, SFwd };
		for (int32 k = 0; k < 12; ++k) Spots.Add(Start2 + Ways[k % 4] * ((k < 4 ? 110.0 : k < 8 ? 160.0 : 220.0) / FOutlawsGeometry::UnitsToCm));
	}
	TArray<FVector2D> OtherHorses, Things;
	for (const FOutlawsObject& O : Live.Objects)
	{
		if (&O == Horse || O.Name == TEXT("PLAYER")) continue;
		if (O.Pos.Y < HorseFloor - MaxFloorDiff - 1.0 || O.Pos.Y > HorseFloor + HorseTall + 2.0) continue;
		(IsHorseObjectName(O.Name) ? OtherHorses : Things).Add(FVector2D(O.Pos.X, O.Pos.Z));
	}

	const double Hy = FMath::DegreesToRadians(Horse->Yaw);
	const FVector2D Fwd(FMath::Sin(Hy), FMath::Cos(Hy)), Right(FMath::Cos(Hy), -FMath::Sin(Hy));
	const FVector2D H2(Horse->Pos.X, Horse->Pos.Z);
	// the places tried, nearest first (across the horse's body counts less than along it: side by side first)
	TArray<FVector2D> Tries;
	for (double a = -24.0; a <= 24.0; a += 0.5)
	{
		for (double b = -24.0; b <= 24.0; b += 1.0)
		{
			if (FMath::Abs(a) < 1e-6 && FMath::Abs(b) < 1e-6) continue;
			Tries.Add(FVector2D(a, b));
		}
	}
	Tries.Sort([](const FVector2D& A, const FVector2D& B) { return FMath::Abs(A.X) + 1.5 * FMath::Abs(A.Y) < FMath::Abs(B.X) + 1.5 * FMath::Abs(B.Y); });
	TArray<FVector2D> Taken;                   // (across, along) of James's horse and each slot so far
	Taken.Add(FVector2D::ZeroVector);
	int32 Rejected[6] = {};                    // why: floor, footprint, wall, horse, spot, object
	for (const FVector2D& T : Tries)
	{
		if (Out.Slots.Num() >= Want) break;
		bool bClash = false;
		for (const FVector2D& K : Taken)
		{
			if (FMath::Abs(T.X - K.X) < 2.0 * HorseHalfWidth + HorseGap && FMath::Abs(T.Y - K.Y) < 2.0 * HorseHalfLength + HorseGap) { bClash = true; break; }
		}
		if (bClash) { ++Rejected[3]; continue; }
		const FVector2D C = H2 + Right * T.X + Fwd * T.Y;
		const int32 Cs = SectorFor(Allowed, C, HorseFloor);
		if (Cs < 0 || !Standable(Live.Sectors[Cs]) || FMath::Abs(Live.Sectors[Cs].Floor.Y - HorseFloor) > MaxFloorDiff) { ++Rejected[0]; continue; }
		const double Fc = Live.Sectors[Cs].Floor.Y;
		// the whole footprint on that floor
		bool bOn = true;
		for (int32 i = -1; i <= 1 && bOn; ++i)
		{
			for (int32 j = -2; j <= 2 && bOn; ++j)
			{
				const FVector2D P = C + Right * (i * HorseHalfWidth) + Fwd * (j * HorseHalfLength * 0.5);
				const int32 Ps = SectorFor(Near, P, Fc);
				bOn = Ps >= 0 && Standable(Live.Sectors[Ps]) && FMath::Abs(Live.Sectors[Ps].Floor.Y - Fc) < 0.05;
			}
		}
		if (!bOn) { ++Rejected[1]; continue; }
		auto Local = [&](const FVector2D& W) { const FVector2D D = W - C; return FVector2D(FVector2D::DotProduct(D, Right), FVector2D::DotProduct(D, Fwd)); };
		bool bWall = false;
		for (const FWallSeg& W : Walls)
		{
			if (HorseSegBoxDist(Local(W.A), Local(W.B), HorseHalfWidth, HorseHalfLength) < WallClear || (W.bSolid && HorseSegsCross(H2, C, W.A, W.B))) { bWall = true; break; }
		}
		if (bWall) { ++Rejected[2]; continue; }
		bool bNear = false;
		for (const FVector2D& O : OtherHorses) if (HorsePointBoxDist(Local(O), HorseHalfWidth, HorseHalfLength) < OtherHorseClear) { bNear = true; break; }
		if (bNear) { ++Rejected[3]; continue; }
		for (const FVector2D& O : Spots) if (HorsePointBoxDist(Local(O), HorseHalfWidth, HorseHalfLength) < SpotClear) { bNear = true; break; }
		if (bNear) { ++Rejected[4]; continue; }
		for (const FVector2D& O : Things) if (HorsePointBoxDist(Local(O), HorseHalfWidth, HorseHalfLength) < ObjectClear) { bNear = true; break; }
		if (bNear) { ++Rejected[5]; continue; }
		Taken.Add(T);
		Out.Slots.Add(FVector(C.X, Fc, C.Y));
		Out.SlotSectors.Add(Cs);
	}
	if (Out.Slots.Num() < Want)
	{
		Out.Why = FString::Printf(TEXT("room for %d of %d (places refused: floor %d, footprint %d, wall %d, horse %d, start spot %d, object %d)"),
			Out.Slots.Num(), Want, Rejected[0], Rejected[1], Rejected[2], Rejected[3], Rejected[4], Rejected[5]);
		Out.Why += TEXT("; sectors there:");
		for (const int32 s : Allowed)
		{
			const FOutlawsSector& S = Live.Sectors[s];
			Out.Why += FString::Printf(TEXT(" s%d floor %.1f ceiling %.1f%s%s%s"), s, S.Floor.Y, S.Ceil.Y, S.SlopeFloor.bSet ? TEXT(" sloped") : TEXT(""),
				Moving && Moving->Contains(s) ? TEXT(" moving") : TEXT(""), S.Velocity.IsNearlyZero() ? TEXT("") : TEXT(" carried"));
		}
	}
	return Out.Slots.Num() > 0;
}

namespace
{
	FString HorsePlanText(const FString& Level, const AOutlawsLevelActor::FHorsePlan& P, int32 Want)
	{
		if (P.Horse.IsEmpty()) return FString::Printf(TEXT("%s: none (%s)"), *Level, *P.Why);
		FString S = FString::Printf(TEXT("%s: James's horse %s at (%.1f, %.1f, %.1f) yaw %.0f, sector %d, %.1f units from the start: %d of %d slots%s"),
			*Level, *P.Horse, P.Pos.X, P.Pos.Y, P.Pos.Z, P.Yaw, P.Sector, P.StartDist, P.Slots.Num(), Want,
			P.Slots.Num() < Want ? *FString::Printf(TEXT(" CANNOT FIT ALL: %s"), *P.Why) : TEXT(""));
		for (int32 i = 0; i < P.Slots.Num(); ++i)
		{
			S += FString::Printf(TEXT(" [%d] (%.1f, %.1f, %.1f) s%d"), i + 1, P.Slots[i].X, P.Slots[i].Y, P.Slots[i].Z, P.SlotSectors[i]);
		}
		return S;
	}

	void HorseMovingSectors(const FOutlawsInfRuntime& Inf, TSet<int32>& Out)
	{
		for (const FOutlawsElevator& E : Inf.Elevators)
		{
			const bool bMoves = E.Class == EOutlawsInfClass::MoveFloor || E.Class == EOutlawsInfClass::MoveFC || E.Class == EOutlawsInfClass::MorphMove
				|| E.Class == EOutlawsInfClass::MorphSpin || E.Class == EOutlawsInfClass::SlopeFloor;
			if (bMoves) Out.Append(E.Sectors);
		}
	}
}

void AOutlawsLevelActor::BuildHorsePlan()
{
	++HorseBuildSerial;
	const int32 Want = UOutlawsCoop::MaxCoopPlayers - 1;
#if WITH_OUTLAWS_TESTS
	static bool bSurveyed = false;
	if (!bSurveyed && AssetsRef && OLFlags::Param(TEXT("OLHorseSlotsAll")))
	{
		bSurveyed = true;
		SurveyHorseSlots(*AssetsRef);
		FGenericPlatformMisc::RequestExit(false);
	}
#endif
	if (bFrontEnd || !LevelData)
	{
		HorsePlan = FHorsePlan();
		return;
	}
	TSet<int32> Moving;
	if (Inf) HorseMovingSectors(*Inf, Moving);
	PlanHorseSlots(Inf ? Inf->Live : *LevelData, &Moving, LevelName, Want, HorsePlan);
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLRT horses: %s"), *HorsePlanText(LevelName, HorsePlan, Want));
}

#if WITH_OUTLAWS_TESTS
void AOutlawsLevelActor::SurveyHorseSlots(const FOutlawsAssets& Assets)
{
	static const TCHAR* const Levels[] = { TEXT("hideout"), TEXT("town"), TEXT("townex"), TEXT("train"), TEXT("canyon"), TEXT("mill"), TEXT("simms"),
		TEXT("miner"), TEXT("cliff"), TEXT("ranch"), TEXT("office"), TEXT("shfort"), TEXT("shgranry"), TEXT("shgulch"), TEXT("shsimms"), TEXT("shwildns"),
		TEXT("gallery"), TEXT("shoot"), TEXT("indy") };
	const int32 Want = UOutlawsCoop::MaxCoopPlayers - 1;
	int32 Full = 0, Short = 0, None = 0;
	for (const TCHAR* Lv : Levels)
	{
		FString Lvt, Obt, InfText;
		if (!Assets.GetText(FString(Lv) + TEXT(".lvt"), Lvt))
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLHORSES %s: not in this install"), Lv);
			continue;
		}
		Assets.GetText(FString(Lv) + TEXT(".obt"), Obt);
		Assets.GetText(FString(Lv) + TEXT(".inf"), InfText);
		FOutlawsLevel Parsed;
		Parsed.ParseLvt(Lvt);
		Parsed.ParseObt(Obt);
		FOutlawsInfRuntime Inf;
		Inf.Init(Parsed, InfText);
		TSet<int32> Moving;
		HorseMovingSectors(Inf, Moving);
		FHorsePlan P;
		PlanHorseSlots(Inf.Live, &Moving, Lv, Want, P);
		if (P.Horse.IsEmpty()) ++None; else if (P.Slots.Num() < Want) ++Short; else ++Full;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLHORSES %s"), *HorsePlanText(Lv, P, Want));
	}
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLHORSES summary: %d levels with all %d slots, %d short, %d with no horse at the start"), Full, Want, Short, None);
}
#endif // WITH_OUTLAWS_TESTS

AOutlawsSpriteActor* AOutlawsLevelActor::SpawnHorseCopy(int32 Slot, FString* OutWhy)
{
	auto Fail = [OutWhy](const TCHAR* Why) -> AOutlawsSpriteActor* { if (OutWhy) *OutWhy = Why; return nullptr; };
	UWorld* World = GetWorld();
	if (!World || !AssetsRef || !LevelData) return Fail(TEXT("no level"));
	if (!HorsePlan.Slots.IsValidIndex(Slot - 1)) return Fail(TEXT("no such slot"));
	if (!SpriteQuad || !SpriteMaterial) return Fail(TEXT("no sprite mesh"));
	// James's horse as the level made it: its look (HD or not) is copied as it stands
	const FVector HorseUE = FOutlawsGeometry::ToUE(HorsePlan.Pos.X, HorsePlan.Pos.Y, HorsePlan.Pos.Z);
	AOutlawsSpriteActor* Src = nullptr;
	double Bd = 1e30;
	for (const TObjectPtr<AActor>& A : SpawnedActors)
	{
		AOutlawsSpriteActor* S = Cast<AOutlawsSpriteActor>(A.Get());
		if (!S || S->OutlawsName != HorsePlan.Horse) continue;
		const double D = FVector::Dist2D(S->GetActorLocation(), HorseUE);
		if (D < Bd) { Bd = D; Src = S; }
	}
	if (!Src) return Fail(TEXT("James's horse is not in the level"));
	FString Itm;
	const FString Anim = OutlawsFormats::ResolveAnim(*AssetsRef, HorsePlan.Horse, &Itm);
	FString Func;
	TMap<FString, FString> Data;
	OutlawsFormats::ParseItm(Itm, Func, Data);
	const bool bProp = Func.Equals(TEXT("Inv_Object"), ESearchCase::IgnoreCase);
	const TSharedPtr<OutlawsTextures::FSpriteAtlas> Atlas = GetAtlas(Anim, bProp);
	if (!Atlas) return Fail(TEXT("no sprite atlas"));
	const FVector Feet = HorsePlan.Slots[Slot - 1];
	const int32 Sector = HorsePlan.SlotSectors[Slot - 1];
	FVector Loc = FOutlawsGeometry::ToUE(Feet.X, Feet.Y, Feet.Z);
	Loc.Z += 0.5;
	const FTransform Xf(FRotator::ZeroRotator, Loc, Src->GetActorScale3D());
	FActorSpawnParameters P;
	P.Owner = this;
	P.ObjectFlags |= RF_Transient;
	P.bDeferConstruction = true;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AOutlawsSpriteActor* S = World->SpawnActor<AOutlawsSpriteActor>(AOutlawsSpriteActor::StaticClass(), Xf, P);
	if (!S) return Fail(TEXT("the spawn failed"));
	S->OutlawsName = HorsePlan.Horse;
	S->OutlawsYaw = HorsePlan.Yaw;
	S->LightLevel = LevelData->Sectors.IsValidIndex(Sector) ? FOutlawsLevel::LightLevel(LevelData->Sectors[Sector].Ambient) : Src->LightLevel;
	S->HomeSector = Sector;
	S->AtlasCols = Src->AtlasCols;
	S->AtlasRows = Src->AtlasRows;
	S->Directions = Src->Directions;
	S->FrameCount = Src->FrameCount;
	S->FPS = Src->FPS;
	S->Grid = Src->Grid;
	S->SpriteTexture = Src->SpriteTexture;
	S->SpriteTextureLo = Src->SpriteTextureLo;
	S->DirectionOffset = Src->DirectionOffset;
	S->bClockwiseDirections = Src->bClockwiseDirections;
	S->FacingYawOffset = Src->FacingYawOffset;
	S->Chors = Src->Chors;
	S->ChorGrid = Src->ChorGrid;
	S->SetFeet(Atlas->BelowPx, Atlas->FootPx, Atlas->CellH);
	S->SetFeetAcross(Atlas->SidePx2, Atlas->CellW);
	S->Quad->SetStaticMesh(SpriteQuad);
	S->Quad->SetMaterial(0, SpriteMaterial);
	S->FinishSpawning(Xf);
#if WITH_EDITOR
	S->SetActorLabel(FString::Printf(TEXT("%s_coop%d"), *HorsePlan.Horse, Slot));
	S->SetFolderPath(TEXT("OutlawsRuntime"));
#endif
	SpawnedActors.Add(S);
	UseItemModel(S, Anim, HorsePlan.Yaw);
	if (bProp)
	{
		// worked by hand as James's is: its event into his horse's sector (the mission's END_LEVEL)
		AddProp(S, HorsePlan.Horse, Data, HorsePlan.Sector, false);
		Props.Last().CoopId = HorseCoopIdBase + Slot;
	}
	return S;
}

void AOutlawsLevelActor::RemoveHorseCopy(int32 Slot, AActor* Copy)
{
	Props.RemoveAll([Slot](const FProp& Pr) { return Pr.CoopId == HorseCoopIdBase + Slot; });
	if (Copy)
	{
		SpawnedActors.Remove(Copy);
		Copy->Destroy();
	}
}

// ---- the session's side --------------------------------------------------------------------------------------------------

FString UOutlawsCoop::HorseSlotsText() const
{
	TArray<FString> Parts;
	TArray<AOutlawsCoopLink*> Links;
	GetLinks(Links);
	for (const AOutlawsCoopLink* K : Links)
	{
		if (K->HorseSlot <= 0) continue;
		const TWeakObjectPtr<AActor>* C = HorseCopies.Find(K->HorseSlot);
		Parts.Add(FString::Printf(TEXT("player %d slot %d %s"), K->PlayerIndex, K->HorseSlot,
			C && C->IsValid() ? *(*C)->GetActorLocation().ToCompactString() : TEXT("(no horse)")));
	}
	return Parts.Num() ? FString::Join(Parts, TEXT("; ")) : FString(TEXT("none"));
}

void UOutlawsCoop::TickHorses(double Now)
{
	AOutlawsLevelActor* L = FindLevel();
	if (!L || !bLevelBuilt || IsMatch() || L->IsFrontEnd()) return;
	// the level cleared away (the cutscene between levels): James's horse and the copies went with it; nothing to put
	// up until the next build (10 Oct: a FAILED line every tick for 20 s after the town's win)
	if (!L->Inf.IsValid()) return;
	if (HorseCopiesBuild != L->GetBuildSerial())
	{
		// a new build took the old copies with it
		HorseCopies.Reset();
		HorseFailed.Reset();
		HorseCopiesBuild = L->GetBuildSerial();
	}
	const AOutlawsLevelActor::FHorsePlan& Plan = L->GetHorsePlan();
	if (Plan.Slots.Num() == 0)
	{
		if (HorseLoggedNoPlan != HorseCopiesBuild)
		{
			HorseLoggedNoPlan = HorseCopiesBuild;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP horses: none on %s (%s)"), *L->LevelName, Plan.Why.IsEmpty() ? TEXT("no room") : *Plan.Why);
		}
	}
	else
	{
		TArray<AOutlawsCoopLink*> Links;
		GetLinks(Links);
		TSet<int32> Want;
		for (const AOutlawsCoopLink* K : Links)
		{
			if (K->HorseSlot <= 0) continue;
			Want.Add(K->HorseSlot);
			TWeakObjectPtr<AActor>& Have = HorseCopies.FindOrAdd(K->HorseSlot);
			if (Have.IsValid()) continue;
			if (K->HorseSlot > Plan.Slots.Num())
			{
				static int32 LoggedShort = -1;
				if (LoggedShort != K->HorseSlot + 100 * HorseCopiesBuild)
				{
					LoggedShort = K->HorseSlot + 100 * HorseCopiesBuild;
					UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP horses: no room on %s for slot %d (player %d): %d slots"), *L->LevelName, K->HorseSlot, K->PlayerIndex, Plan.Slots.Num());
				}
				continue;
			}
			if (HorseFailed.Contains(K->HorseSlot)) continue;     // (once a build: what failed once fails again)
			FString Why;
			Have = L->SpawnHorseCopy(K->HorseSlot, &Why);
			if (!Have.IsValid())
			{
				HorseFailed.Add(K->HorseSlot);
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP horses: slot %d for player %d on %s not put up: %s (%s, %s; not tried again this build)"), K->HorseSlot, K->PlayerIndex,
					*L->LevelName, *Why, *Plan.Horse, IsHost() ? TEXT("host") : TEXT("client"));
				continue;
			}
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP horses: slot %d up for player %d on %s at %s (%s, %s)"), K->HorseSlot, K->PlayerIndex, *L->LevelName,
				*Have->GetActorLocation().ToCompactString(), *Plan.Horse, IsHost() ? TEXT("host") : TEXT("client"));
		}
		for (auto It = HorseCopies.CreateIterator(); It; ++It)
		{
			if (Want.Contains(It.Key())) continue;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP horses: slot %d freed on %s (its player left; %s)"), It.Key(), *L->LevelName, IsHost() ? TEXT("host") : TEXT("client"));
			L->RemoveHorseCopy(It.Key(), It.Value().Get());
			It.RemoveCurrent();
		}
	}
	if (HorseTestTag.IsEmpty()) return;
#if WITH_OUTLAWS_TESTS

	// -OLHorseTest: photographs of the horses (everyone in; a player gone; one come back), then quit
	UWorld* W = L->GetWorld();
	APlayerController* PC = W ? W->GetFirstPlayerController() : nullptr;
	if (!PC) return;
	TArray<AOutlawsCoopLink*> Links;
	GetLinks(Links);
	int32 Copies = 0;
	for (const TPair<int32, TWeakObjectPtr<AActor>>& Kv : HorseCopies) Copies += Kv.Value.IsValid() ? 1 : 0;
	const int32 Players = Links.Num();
	static FString HorseShotName;
	static double HorseShotAt = 0.0;
	const FString Who = IsHost() ? FString(TEXT("host")) : FString::Printf(TEXT("c%d"), MyIndex());
	auto Photo = [&](const FString& Name)
	{
		// from in front of the horses, a little above, all of them in view
		const double Y = FMath::DegreesToRadians(Plan.Yaw);
		const FVector Fwd(FMath::Cos(Y), FMath::Sin(Y), 0.0);    // (Outlaws yaw == UE yaw)
		FVector Mid = FOutlawsGeometry::ToUE(Plan.Pos.X, Plan.Pos.Y, Plan.Pos.Z);
		double Span = 300.0;
		int32 N = 1;
		for (const TPair<int32, TWeakObjectPtr<AActor>>& Kv : HorseCopies)
		{
			if (!Kv.Value.IsValid()) continue;
			Mid += Kv.Value->GetActorLocation();
			++N;
		}
		Mid /= N;
		for (const TPair<int32, TWeakObjectPtr<AActor>>& Kv : HorseCopies) if (Kv.Value.IsValid()) Span = FMath::Max(Span, FVector::Dist2D(Kv.Value->GetActorLocation(), Mid) * 2.0 + 300.0);
		// the first of the four sides (in front, behind, either flank), nearest first, with a clear line to them
		const FVector Look = Mid + FVector(0.0, 0.0, 80.0);
		const FVector Side(-Fwd.Y, Fwd.X, 0.0);
		FVector Eye = Look + Fwd * (Span * 0.9 + 250.0) + FVector(0.0, 0.0, 100.0);
		{
			FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsHorseShot), false);
			for (const AActor* A : TActorRange<AOutlawsPlayer>(W)) Q.AddIgnoredActor(A);
			bool bFound = false;
			for (const double Far : { 0.9, 0.6, 0.35 })
			{
				for (const FVector& Dir : { Fwd, -Fwd, Side, -Side })
				{
					const FVector Try = Look + Dir * (Span * Far + 250.0) + FVector(0.0, 0.0, 100.0);
					FHitResult Hit;
					if (!W->LineTraceSingleByChannel(Hit, Look + FVector(0.0, 0.0, 40.0), Try, ECC_Visibility, Q)) { Eye = Try; bFound = true; break; }
				}
				if (bFound) break;
			}
		}
		// this machine's player stands there looking at them (the player's own view is the one the screen shows)
		if (AOutlawsPlayer* Me = LocalPlayer())
		{
			const FRotator Face = (Look - Eye).Rotation();
			Me->TeleportTo(Eye, FRotator(0.f, Face.Yaw, 0.f), false, true);
			PC->SetControlRotation(Face);
			PC->SetViewTarget(Me);
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP horse test %s: %d players, %d horses beside James's on %s: %s"), *Name, Players, Copies, *L->LevelName, *HorseSlotsText());
		HorseTestAt = Now;
		HorseShotName = FString::Printf(TEXT("ol_horses_%s_%s"), *HorseTestTag, *Name);
		HorseShotAt = Now + 1.5;                    // (the camera's view first: a frame or two)
	};
	if (!HorseShotName.IsEmpty() && Now >= HorseShotAt)
	{
		Shot(HorseShotName);
		HorseShotName.Reset();
	}
	if (HorseQuitAfter > 0.f && Now - LevelBuiltAt > HorseQuitAfter)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP horse test: %s quits after %.0f s (%s)"), *Who, HorseQuitAfter, *HorseSlotsText());
		QuitForTest();
		return;
	}
	const bool bAllIn = Copies == Players - 1 && (PartyPlayers <= 0 || Players >= PartyPlayers);
	if (HorseTestSeen != Players * 100 + Copies) { HorseTestSeen = Players * 100 + Copies; HorseTestAt = Now; }
	const bool bSettled = Now - HorseTestAt > 5.0;
	const bool bRejoin = HorseTestTag.Contains(TEXT("rejoin"));
	if (HorseTestTag.Contains(TEXT("hom")))
	{
		// -OLHorseTest=...hom... (two players, a mission): the host makes it a Historical Mission (as its office's poster
		// would), stands the outlaw awake by player 2's horse and punches him out; player 2 grips its own horse empty-handed
		if (L->LevelName.Equals(TEXT("office"), ESearchCase::IgnoreCase))
		{
			// (REQ-COOPHM) ...homstart...: the office first (the host's lobby level); the host takes the Fort's poster and rides,
			// every player with it; back here after, each machine's saved game score
			if (HomOfficeLogged != L->GetBuildSerial() && Now - LevelBuiltAt > 6.0)
			{
				HomOfficeLogged = L->GetBuildSerial();
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP horse hom test: %s in the office (hom %d), game score %d"), IsHost() ? TEXT("host") : TEXT("client"), L->bHom ? 1 : 0, L->HomGameScore);
			}
			int32 Built = 0;
			ForClients([&](AOutlawsCoopLink& K) { Built += K.BuiltSerial == StartedSerial ? 1 : 0; });
			if (IsHost() && HorseTestTag.Contains(TEXT("homstart")) && HorseTestStep == 0 && L->HomGameScore == 0 && Built >= Players - 1 && Now - LevelBuiltAt > 10.0)
			{
				L->bHom = true;                          // (as Marshal Training's office)
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP horse hom test: host: the Fort's poster (SPAWN_LEVEL SHFORT PORCH1D)"));
				L->HomSpawnLevel(TEXT("SHFORT PORCH1D"));
				AOutlawsPlayer* Me = LocalPlayer();
				if (Me && Me->IsMenuOpen()) L->HomRide();          // (the ride-out page's Ride)
				HorseTestStep = 5;
			}
			return;
		}
		AOutlawsEnemy* Boss = nullptr;
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies()) if (Wk.IsValid() && Wk->bBoss) { Boss = Wk.Get(); break; }
		const TWeakObjectPtr<AActor>* Mine = HorseCopies.Find(1);
		AActor* Copy = Mine ? Mine->Get() : nullptr;
		AOutlawsPlayer* Me = LocalPlayer();
		if (!Boss || !Copy || !Me || !bAllIn || Now - LevelBuiltAt < 6.0) return;
		const FVector Across = FRotator(0.f, Plan.Yaw + 90.f, 0.f).Vector();
		if (IsHost() && (HorseTestStep == 0 || HorseTestStep == 5))
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP horse hom test: host: in %s (hom %d, %s, back to %s)"), *L->LevelName, L->bHom ? 1 : 0, *L->HomMission, *L->HomReturnSector);
			L->bHom = true;
			L->HomMission = L->LevelName;
			L->HomReturnSector = TEXT("PORCH1D");
			L->bInvincible = true;
			Boss->SetDormant(false);
			const float Half = Boss->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			Boss->SetActorLocation(Copy->GetActorLocation() + Across * 150.0 + FVector(0.0, 0.0, Half + 5.0), false, nullptr, ETeleportType::TeleportPhysics);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP horse hom test: host: %s as a Historical Mission (back to %s); the outlaw %s awake by player 2's horse at %s"),
				*L->HomMission, *L->HomReturnSector, *Boss->GetOutlawsName(), *Boss->GetActorLocation().ToCompactString());
			// (REQ-HMBOSSEND) the host punches him out: captured as he goes down, the lines on every machine
			for (int32 k = 0; k < 40 && !Boss->IsDead(); ++k)
			{
				AOutlawsEnemy::GNextHitMelee = true;
				Boss->TakeHit(1.f, Boss->GetActorLocation(), FVector(1.0, 0.0, 0.0), Boss->GetActorLocation() - FVector(30.0, 0.0, 0.0));
			}
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP horse hom test: host: punches the outlaw out: dead %d, taken alive %d, lines shown %d"), Boss->IsDead() ? 1 : 0, Boss->bTakenAlive ? 1 : 0,
				L->bHomBossDown ? 1 : 0);
			HorseTestStep = 1;
			HorseTestAt = Now;
		}
		else if (!IsHost())
		{
			if (HorseTestStep == 0 && Now - LevelBuiltAt > 14.0)
			{
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP horse hom test: client: in %s (hom %d, %s, back to %s)"), *L->LevelName, L->bHom ? 1 : 0, *L->HomMission, *L->HomReturnSector);
				L->bInvincible = true;
				Me->TeleportTo(Copy->GetActorLocation() - Across * 120.0 + FVector(0.0, 0.0, 100.0), FRotator(0.f, Plan.Yaw + 90.f, 0.f), false, true);
				// (REQ-HMBOSSEND) nothing taken up: the host's word on the outlaw shown here
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP horse hom test: client: the outlaw %s dead %d, taken alive %d (the host's word), lines shown %d"), *Boss->GetOutlawsName(),
					Boss->IsDead() ? 1 : 0, Boss->bTakenAlive ? 1 : 0, L->bHomBossDown ? 1 : 0);
				HorseTestStep = 1;
				HorseTestAt = Now;
			}
			else if (HorseTestStep == 1 && Now - HorseTestAt > 3.0)
			{
				// the hand at its own horse (slot 1), the other hand empty
				const FVector Hand = Copy->GetActorLocation() + FVector(0.0, 0.0, 60.0);
				const bool bWorked = L->TurnPropByHand(Hand);
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP horse hom test: client: grips its own horse (slot 1 at %s), the outlaw held %d: %s"), *Copy->GetActorLocation().ToCompactString(),
					Boss->bHeldByPlayer ? 1 : 0, bWorked ? TEXT("worked") : TEXT("NOT WORKED"));
				HorseTestStep = 2;
			}
		}
		return;
	}
	switch (HorseTestStep)
	{
	case 0:
		if (bAllIn && bSettled && Now - LevelBuiltAt > 8.0)
		{
			Photo(FString::Printf(TEXT("%s_%dp"), *Who, Players));
			HorseTestStep = (IsHost() && bRejoin) ? 1 : 9;
		}
		break;
	case 1:
		// a player left: its horse gone here
		if (Players < PartyPlayers && Copies == Players - 1 && bSettled)
		{
			Photo(FString::Printf(TEXT("%s_left_%dp"), *Who, Players));
			HorseTestStep = 2;
		}
		break;
	case 2:
		// one joined again: the freed slot taken
		if (Players >= PartyPlayers && bAllIn && bSettled)
		{
			Photo(FString::Printf(TEXT("%s_rejoin_%dp"), *Who, Players));
			HorseTestStep = 3;
		}
		break;
	case 3:
		if (Now - HorseTestAt > 3.0)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP horse test: done (%s)"), *HorseSlotsText());
			QuitForTest();
		}
		break;
	default:
		break;
	}
#endif // WITH_OUTLAWS_TESTS
}
