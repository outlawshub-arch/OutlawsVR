// BUG-ENEMYVIS (user 2 Nov, snapshots #32-#34, civlwar2: from the covered porch 885, looking in through the broken saloon
// window, the room's bad guys and bodies were not drawn; from inside the room, 812, they were): -OLShots -OLLevel=civlwar2
// -OLNoFrontEnd -OLFloodWindingProbe [-OLOldFloodWinding].  (1) the eye's flood from the three snapshots' eyes by the
// vertex list's winding (as before) and by the walls' (the fix), the sectors reached and whether 352 (the window), 812 (the
// saloon), 206, 205 and 319 are among them; (2) every stock level's sectors whose vertex-list winding differs from their
// walls' and that have an opening: floods from eyes a little in from each wall, both ways, and the openings of the eye's own
// sector the eye is in front of (by the walls' winding) that the flood still left out ("seen from behind"); (3) the floods'
// sizes both ways from one eye in every sector of five levels; (4) the player at each snapshot's spot: the sprites in 812
// culled or drawn, shots floodwinding_<n>[_old].png in Saved\OLShots.  Lines "OLSHOT floodwinding check PASS|FAIL: ..."
// and "OLSHOT floodwinding RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsLevel.h"
#include "OutlawsLab.h"
#include "OutlawsRuntime.h"
#include "OutlawsRuntimeLibrary.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "OutlawsInfRuntime.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

const FOutlawsLevel* AOutlawsLevelActor::DebugLiveLevel() const
{
	return Inf ? &Inf->Live : nullptr;
}

int32 AOutlawsLevelActor::DebugFloodBy(const FOutlawsLevel* L, int32 From, const FVector2D& Eye, double EyeZ, bool bOldWinding, TSet<int32>* OutSeen)
{
	const FOutlawsLevel* Live = DebugLiveLevel();
	const FOutlawsLevel& Lv = L ? *L : *Live;
	// another level's plan: none of this level's explodable walls apply to it
	TArray<FExplodeWall> Kept;
	const bool bOther = &Lv != Live;
	if (bOther) Swap(Kept, ExplodeWalls);
	TSet<int32> Seen;
	const int32 WasMode = DebugFloodMode, WasWinding = DebugFloodWinding, WasPops = DebugFloodPops;
	DebugFloodMode = 0;
	DebugFloodWinding = bOldWinding ? 1 : 0;
	PortalFlood(Lv, From, Eye, EyeZ, Seen);
	DebugFloodMode = WasMode;
	DebugFloodWinding = WasWinding;
	DebugFloodPops = WasPops;
	if (bOther) Swap(Kept, ExplodeWalls);
	if (OutSeen) *OutSeen = MoveTemp(Seen);
	return OutSeen ? OutSeen->Num() : Seen.Num();
}

namespace
{
	// eyes a little in from each wall of a sector (by the walls' winding), inside it, at standing height
	void FloodEyes(const FOutlawsSector& S, int32 Max, TArray<FVector2D>& Out)
	{
		const bool bCW = S.IsClockwise();
		for (const FOutlawsWall& W : S.Walls)
		{
			if (Out.Num() >= Max) return;
			if (!S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) continue;
			const FVector2D A = S.Verts[W.V1], B = S.Verts[W.V2];
			const double EX = B.X - A.X, EZ = B.Y - A.Y;
			const double Len = FMath::Sqrt(EX * EX + EZ * EZ);
			if (Len < 0.5) continue;
			const FVector2D N = FVector2D(bCW ? EZ : -EZ, bCW ? -EX : EX) / Len;
			const FVector2D P = (A + B) * 0.5 + N * FMath::Min(1.0, Len * 0.25);
			if (FOutlawsLevel::InsideWalls(S, P.X, P.Y)) Out.Add(P);
		}
	}

	double FloodEyeY(const FOutlawsSector& S)
	{
		return S.Floor.Y + FMath::Min(5.5, (S.Ceil.Y - S.Floor.Y) * 0.8);
	}

	// the eye's own sector's openings it is in front of (by the walls' winding) with room to see through, that the flood left out
	int32 DroppedOpenings(const FOutlawsLevel& Lv, const FOutlawsSector& S, const FVector2D& Eye, const TSet<int32>& Seen)
	{
		const double Inside = S.IsClockwise() ? -1.0 : 1.0;
		int32 N = 0;
		for (const FOutlawsWall& W : S.Walls)
		{
			const FOutlawsSector* R = Lv.Sector(W.Adjoin);
			if (!R || !S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) continue;
			const FVector2D A = S.Verts[W.V1], B = S.Verts[W.V2];
			const double Len = FVector2D::Distance(A, B);
			if (Len < 1e-4) continue;
			const double Side = FVector2D::CrossProduct(B - A, Eye - A) / Len * Inside;
			const double Gap = FMath::Min(S.Ceil.Y, R->Ceil.Y) - FMath::Max(S.Floor.Y, R->Floor.Y);
			if (Side > 0.05 && Gap > 0.05 && !Seen.Contains(R->Index)) ++N;
		}
		return N;
	}
}

OL_SHOT_MATCH(OLFloodWindingProbe) { return OLFlags::Param(TEXT("OLFloodWindingProbe")); }

OL_SHOT_TEST(OLFloodWindingProbe, 1450)
{
	static int32 Passes = 0, Fails = 0, Spot = 0;
	static float Wait = 0.f;
	static FVector Fix = FVector::ZeroVector;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT floodwinding check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	const bool bOld = OLFlags::Param(TEXT("OLOldFloodWinding"));
	// the snapshots (info.txt): the player's UE location, the eye's, the look; the eye in game units and its sector
	struct FSpot { int32 Shot; FVector At, Eye; FRotator Look; FVector2D GEye; double GEyeY; int32 Sector; };
	static const FSpot Spots[] = {
		{ 33, FVector(769.700, 3872.704, 43.822), FVector(769.466, 3872.647, 106.027), FRotator(3.56, 59.73, 0.0), FVector2D(129.09, 25.65), 3.53, 885 },
		{ 32, FVector(223.564, 3808.521, 44.304), FVector(224.422, 3808.025, 104.722), FRotator(7.92, 27.06, 0.0), FVector2D(126.93, 7.48), 3.49, 885 },
		{ 34, FVector(737.053, 4344.117, 43.626), FVector(736.175, 4344.704, 106.601), FRotator(2.22, 63.46, 0.0), FVector2D(144.82, 24.54), 3.55, 812 },
	};
	APlayerController* PC = Cast<APlayerController>(Controller);
	auto EyeNow = [PC]() { return PC && PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : FVector::ZeroVector; };
	auto Place = [this](const FVector& At, const FRotator& Look)
	{
		TeleportTo(At + Fix, FRotator(0.0, Look.Yaw, 0.0), false, true);
		if (Controller) Controller->SetControlRotation(Look);
	};
	auto Has = [](const TSet<int32>& S, int32 i) { return S.Contains(i) ? TEXT("yes") : TEXT("no"); };
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 4.f || !bUp) return;
		L->bInvincible = true;
		const FOutlawsLevel* Live = L->DebugLiveLevel();
		Check(Live && L->LevelName.Equals(TEXT("civlwar2"), ESearchCase::IgnoreCase), FString::Printf(TEXT("the level is civlwar2 (%s)"), *L->LevelName));
		if (!Live) { ShotStep = 9; return; }
		// (1) the snapshots' eyes, both rules
		for (const FSpot& Sp : Spots)
		{
			const FOutlawsSector* S = Live->Sector(Sp.Sector);
			const bool bIn = S && Live->ContainsXZ(*S, Sp.GEye.X, Sp.GEye.Y);
			TSet<int32> Was, Now;
			const int32 NWas = L->DebugFloodBy(nullptr, Sp.Sector, Sp.GEye, Sp.GEyeY, true, &Was);
			const int32 NNow = L->DebugFloodBy(nullptr, Sp.Sector, Sp.GEye, Sp.GEyeY, false, &Now);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT floodwinding snapshot %d (eye in %d: %s): vertex list %d sectors (352 %s, 812 %s, 206 %s, 205 %s, 319 %s); walls %d sectors (352 %s, 812 %s, 206 %s, 205 %s, 319 %s)"),
				Sp.Shot, Sp.Sector, bIn ? TEXT("yes") : TEXT("NO"), NWas, Has(Was, 352), Has(Was, 812), Has(Was, 206), Has(Was, 205), Has(Was, 319),
				NNow, Has(Now, 352), Has(Now, 812), Has(Now, 206), Has(Now, 205), Has(Now, 319));
			if (Sp.Shot == 34)
			{
				Check(Now.Contains(812) && Was.Contains(812) && NNow >= NWas, FString::Printf(TEXT("snapshot 34 from the saloon: 812 both ways, %d -> %d sectors"), NWas, NNow));
			}
			else
			{
				Check(Now.Contains(352) && Now.Contains(812), FString::Printf(TEXT("snapshot %d from the porch: the flood reaches the window 352 and the saloon 812 (%d sectors)"), Sp.Shot, NNow));
				Check(!Was.Contains(812), FString::Printf(TEXT("snapshot %d: the vertex list's rule reproduces the bug, 812 not reached (%d sectors)"), Sp.Shot, NWas));
			}
		}
		// (2) every level's sectors whose two windings differ
		{
			const double T0 = FPlatformTime::Seconds();
			const FString Root = FOutlawsAssets::FindInstallRoot(L->InstallPath);
			TSharedPtr<FOutlawsAssets> AP = Root.IsEmpty() ? nullptr : FOutlawsAssets::GetShared(Root);
			const TArray<FString> Levels = UOutlawsRuntimeLibrary::ListLevels(L->InstallPath);
			int32 Sectors = 0, Eyes = 0, DropWas = 0, DropNow = 0;
			int64 SumWas = 0, SumNow = 0;
			static const TCHAR* SizeLevels[] = { TEXT("civlwar2"), TEXT("train"), TEXT("shgranry"), TEXT("office"), TEXT("drygulch") };
			for (const FString& Name : Levels)
			{
				FString Lvt;
				if (!AP || !AP->GetText(Name + TEXT(".lvt"), Lvt)) continue;
				FOutlawsLevel Lv;
				Lv.ParseLvt(Lvt);
				for (const FOutlawsSector& S : Lv.Sectors)
				{
					if (S.NoWalls() || S.Verts.Num() < 3 || S.IsClockwise() == S.IsClockwiseByVerts()) continue;
					bool bOpen = false;
					for (const FOutlawsWall& W : S.Walls) if (W.Adjoin >= 0) bOpen = true;
					if (!bOpen) continue;
					++Sectors;
					TArray<FVector2D> At;
					FloodEyes(S, 12, At);
					int32 SW = 0, SN = 0, DW = 0, DN = 0;
					for (const FVector2D& P : At)
					{
						TSet<int32> Was, Now;
						SW += L->DebugFloodBy(&Lv, S.Index, P, FloodEyeY(S), true, &Was);
						SN += L->DebugFloodBy(&Lv, S.Index, P, FloodEyeY(S), false, &Now);
						DW += DroppedOpenings(Lv, S, P, Was);
						DN += DroppedOpenings(Lv, S, P, Now);
					}
					Eyes += At.Num();
					SumWas += SW; SumNow += SN; DropWas += DW; DropNow += DN;
					UE_LOG(LogOutlaws, Display, TEXT("OLSHOT floodwinding sector %s s%d (%d walls): %d eyes, sectors reached %.1f -> %.1f a flood; openings in front dropped %d -> %d"),
						*Name, S.Index, S.Walls.Num(), At.Num(), At.Num() ? double(SW) / At.Num() : 0.0, At.Num() ? double(SN) / At.Num() : 0.0, DW, DN);
				}
				// (3) the floods' sizes from one eye in every sector of five levels
				bool bSize = false;
				for (const TCHAR* Sl : SizeLevels) if (Name.Equals(Sl, ESearchCase::IgnoreCase)) bSize = true;
				if (!bSize) continue;
				int32 N = 0, Differ = 0, Up = 0, Down = 0, MaxUp = 0, MaxDown = 0;
				int64 TW = 0, TN = 0;
				for (const FOutlawsSector& S : Lv.Sectors)
				{
					if (S.NoWalls() || S.Verts.Num() < 3 || S.Ceil.Y - S.Floor.Y < 1.0) continue;
					TArray<FVector2D> At;
					FloodEyes(S, 1, At);
					if (At.Num() == 0) continue;
					const int32 W0 = L->DebugFloodBy(&Lv, S.Index, At[0], FloodEyeY(S), true);
					const int32 N0 = L->DebugFloodBy(&Lv, S.Index, At[0], FloodEyeY(S), false);
					++N; TW += W0; TN += N0;
					if (W0 != N0) { ++Differ; if (N0 > W0) { ++Up; MaxUp = FMath::Max(MaxUp, N0 - W0); } else { ++Down; MaxDown = FMath::Max(MaxDown, W0 - N0); } }
				}
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT floodwinding sizes %s: %d eyes (one a sector), sectors a flood %.2f -> %.2f; %d differ (%d larger, most +%d; %d smaller, most -%d)"),
					*Name, N, N ? double(TW) / N : 0.0, N ? double(TN) / N : 0.0, Differ, Up, MaxUp, Down, MaxDown);
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT floodwinding sweep: %d sectors over %d levels whose windings differ, %d eyes; sectors reached %.1f -> %.1f a flood; openings in front dropped %d -> %d (%.1f s)"),
				Sectors, Levels.Num(), Eyes, Eyes ? double(SumWas) / Eyes : 0.0, Eyes ? double(SumNow) / Eyes : 0.0, DropWas, DropNow, FPlatformTime::Seconds() - T0);
			Check(Sectors > 0 && DropNow == 0, FString::Printf(TEXT("the sweep: %d sectors, openings in front of the eye dropped by the walls' rule %d (the vertex list's %d)"), Sectors, DropNow, DropWas));
		}
		Spot = 0;
		Fix = FVector::ZeroVector;
		Place(Spots[0].At, Spots[0].Look);
		Wait = ShotTime + 1.f;
		ShotStep = 1;
		return;
	}
	case 1:
		// the eye put where the snapshot's was (this pawn's eye stands a little apart from the headset's)
		if (ShotTime < Wait) return;
		Fix += Spots[Spot].Eye - EyeNow();
		Place(Spots[Spot].At, Spots[Spot].Look);
		Wait = ShotTime + 1.5f;
		ShotStep = 2;
		return;
	case 2:
	{
		// (4) the sprites whose sector is the saloon: drawn by the fix, culled by the old rule from the porch
		if (ShotTime < Wait) return;
		const FSpot& Sp = Spots[Spot];
		const FVector E = EyeNow();
		int32 In812 = 0, Culled = 0, Near = 0;
		for (TActorIterator<AOutlawsSpriteActor> It(GetWorld()); It; ++It)
		{
			if (It->DebugCullSector() != 812) continue;
			if (FVector::Dist(It->GetActorLocation(), E) < 100.0) { ++Near; continue; }
			++In812;
			if (It->DebugCulled()) ++Culled;
		}
		const bool bWantCulled = bOld && Sp.Sector != 812;
		const FString What = FString::Printf(TEXT("snapshot %d, eye %s (%.0f cm from the snapshot's), player in %d, %d sectors drawn: %d of %d sprites in 812 culled (%d by the eye left out)%s"),
			Sp.Shot, *E.ToCompactString(), FVector::Dist(E, Sp.Eye), L->PlayerSectorIndex(), L->DebugSectorsDrawn(), Culled, In812, Near, bWantCulled ? TEXT(" (old rule: all culled wanted)") : TEXT(""));
		Check(In812 > 0 && (bWantCulled ? Culled == In812 : Culled == 0), What);
		const FString Name = FString::Printf(TEXT("floodwinding_%d%s"), Sp.Shot, bOld ? TEXT("_old") : TEXT(""));
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT floodwinding shot %s.png"), *Name);
		Wait = ShotTime + 1.f;
		ShotStep = 3;
		return;
	}
	case 3:
		if (ShotTime < Wait) return;
		if (++Spot < UE_ARRAY_COUNT(Spots))
		{
			Fix = FVector::ZeroVector;
			Place(Spots[Spot].At, Spots[Spot].Look);
			Wait = ShotTime + 1.f;
			ShotStep = 1;
			return;
		}
		ShotStep = 9;
		return;
	case 9:
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT floodwinding RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
