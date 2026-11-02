// BUG-CRACKYOFF (user 2 Nov, snapshot #31: civlwar1's dynamite crack wall, sector 449 wall 8, its bricks 6.4 units low against
// its neighbours'): -OLShots -OLLevel=civlwar1 -OLNoFrontEnd -OLShutPartProbe [-OLOldShutPart] [-OLShutPartNoShots].
// Every stock level built both ways (the rule and -OLOldShutPart's) from its LVT with the INF's elevators at their first stop
// (FOutlawsInfRuntime::Init, as the level starts): each wall shut by the sector beyond whose part the rule now draws where its
// mid was drawn (not a DEFAULT mid, not sky) listed, "OLSHOT shutpart wall <level> <sector> <wall> TOP|BOT ...", with whether
// it looks different (the part's texture or offsets differ from the mid's) and whether the sector beyond is a door; the list
// also goes to Saved\OLShots\shutpart_list.txt.  Checks: civlwar1 449 wall 8 is drawn as its bottom part, its y offset the
// neighbours' mids' (walls 4, 6, 7, 9, 10, 11); after BLAMMO's blast (its floor at its second stop, 0.25) the wall has nothing
// left to draw; the mill's flume gates (BUG-MILLVIS, DEFAULT mids) are shut parts both ways; every other count of every level
// is the same both ways.  Then a sample of the walls that look different (every level that has one, the doors first; at
// least 30) is photographed from inside its sector, shutpart_<level>_<sector>_<wall>[_old].png in Saved\OLShots.  Lines
// "OLSHOT shutpart check PASS|FAIL: ..." and "OLSHOT shutpart RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsLevel.h"
#include "OutlawsLab.h"
#include "OutlawsGeometry.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsShutPartProbeLocal
{
	struct FSample { FString Level; int32 Sector = -1, Wall = -1; bool bTop = false, bDoor = false; FVector Spot = FVector::ZeroVector, Target = FVector::ZeroVector; };
	TArray<FSample> Samples;
	int32 Passes = 0, Fails = 0, Next = 0;
	float Wait = 0.f;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT shutpart check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}

	int32 Tris(const FOutlawsGeometry& G)
	{
		int32 N = 0;
		for (const TPair<FString, FOutlawsMeshSection>& Kv : G.World) N += Kv.Value.NumTriangles();
		return N;
	}

	/** A spot inside the wall's own sector a few units off its middle, and the point on the wall to look at. */
	bool SpotFor(const FOutlawsLevel& Lv, const FOutlawsSector& S, const FOutlawsWall& W, FVector& Spot, FVector& Target)
	{
		if (!S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) return false;
		const FVector2D A = S.Verts[W.V1], B = S.Verts[W.V2], M = (A + B) * 0.5;
		const FVector2D D = (B - A).GetSafeNormal();
		const FVector2D N(-D.Y, D.X);
		for (const double Off : { 20.0, 14.0, 9.0, 5.0, 2.5 })
		{
			for (const double Side : { 1.0, -1.0 })
			{
				const FVector2D P = M + N * Off * Side;
				if (!Lv.ContainsXZ(S, P.X, P.Y)) continue;
				const double Fl = Lv.SurfaceY(S, P, true), Fm = Lv.SurfaceY(S, M, true), Cm = Lv.SurfaceY(S, M, false);
				Spot = FOutlawsGeometry::ToUE(P, Fl);
				Target = FOutlawsGeometry::ToUE(M, Fm + FMath::Min((Cm - Fm) * 0.5, 9.0));
				return true;
			}
		}
		return false;
	}
}

OL_SHOT_MATCH(OLShutPartProbe) { return OLFlags::Param(TEXT("OLShutPartProbe")); }

OL_SHOT_TEST(OLShutPartProbe, 1740)
{
	using namespace OutlawsShutPartProbeLocal;
	if (!L || ShotStep < 0) return;
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	const bool bOld = OLFlags::Param(TEXT("OLOldShutPart"));
	APlayerController* PC = Cast<APlayerController>(Controller);
	auto EyeNow = [PC]() { return PC && PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : FVector::ZeroVector; };
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 4.f || !bUp) return;
		L->bInvincible = true;
		const FOutlawsAssets* A = L->GetAssets();
		TArray<FString> Lvts;
		if (A) A->ListByExtension(TEXT(".lvt"), Lvts);
		Lvts.Sort();
		static const TCHAR* Kept[] = { TEXT("floor"), TEXT("ceil"), TEXT("sky_floor"), TEXT("sky_ceiling"), TEXT("open"), TEXT("ALL"), TEXT("PAWN"),
			TEXT("SHOT"), TEXT("fence"), TEXT("wall_midtex"), TEXT("tinted_glass"), TEXT("wall_overlay"), TEXT("water_surface"), TEXT("default_band_skipped") };
		const bool bWas = FOutlawsGeometry::bOldShutPart;
		const TMap<FString, FOutlawsTexInfo> NoTex;
		TArray<FString> List;
		TMap<FString, TArray<FSample>> Looks;          // per level: the walls that look different, doors first
		TArray<FString> LevelOrder;
		int32 Levels = 0, Total = 0, Differ = 0, Doors = 0, KeptBad = 0, QuietBad = 0, CountBad = 0, KeptMid = 0, TrisBad = 0;
		for (const FString& File : Lvts)
		{
			const FString Name = FPaths::GetBaseFilename(File).ToLower();
			FString Lvt, InfText;
			if (!A->Has(Name + TEXT(".obt")) || !A->GetText(Name + TEXT(".lvt"), Lvt)) continue;
			FOutlawsLevel Lv;
			if (!Lv.ParseLvt(Lvt)) continue;
			// the elevators at their first stop, as the level starts
			TUniquePtr<FOutlawsInfRuntime> Inf;
			if (A->GetText(Name + TEXT(".inf"), InfText))
			{
				Inf = MakeUnique<FOutlawsInfRuntime>();
				Inf->Init(Lv, InfText);
			}
			const FOutlawsLevel& Live = Inf ? Inf->Live : Lv;
			TArray<FOutlawsGeometry::FShutPart> Parts;
			FOutlawsGeometry GNew, GOld;
			GNew.ShutAudit = &Parts;
			FOutlawsGeometry::bOldShutPart = false;
			GNew.Build(Live, NoTex, true);
			FOutlawsGeometry::bOldShutPart = true;
			GOld.Build(Live, NoTex, true);
			FOutlawsGeometry::bOldShutPart = bWas;
			++Levels;
			int32 Tops = 0, Bots = 0, LvDiffer = 0;
			for (const FOutlawsGeometry::FShutPart& P : Parts)
			{
				const FOutlawsSector& S = Live.Sectors[P.Sector];
				const FOutlawsWall& W = S.Walls[P.Wall];
				const FOutlawsSector* Beyond = Live.Sector(W.Adjoin);
				const bool bDoor = Beyond && ((Beyond->Flags & OutlawsFlags::SectorDoor) || Beyond->Name.ToUpper().Contains(TEXT("DOOR")) || (P.bTop && Inf && Inf->ElevatorsBySector.Contains(W.Adjoin)));
				const bool bLooks = !P.bKeptMid && (P.Tex != P.MidTex || FMath::Abs(P.X - P.MidX) > 0.001 || FMath::Abs(P.Y - P.MidY) > 0.001);
				if (P.bKeptMid) ++KeptMid;
				else (P.bTop ? Tops : Bots) += 1;
				LvDiffer += bLooks ? 1 : 0;
				Doors += bDoor && bLooks ? 1 : 0;
				const FString Row = FString::Printf(TEXT("%-9s %4d %3d %s %-9s %7.2f %7.2f | mid %-9s %7.2f %7.2f | beyond %d %s%s%s"), *Name, P.Sector, P.Wall, P.bTop ? TEXT("TOP") : TEXT("BOT"),
					*Live.TexStem(P.Tex), P.X, P.Y, *Live.TexStem(P.MidTex), P.MidX, P.MidY, W.Adjoin, Beyond ? *Beyond->Name : TEXT("?"), bDoor ? TEXT(" | door") : TEXT(""),
					P.bKeptMid ? TEXT(" | KEPT ON ITS MID (the part is DEFAULT)") : bLooks ? TEXT("") : TEXT(" | same look"));
				List.Add(Row);
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT shutpart wall %s"), *Row);
				FSample Sm;
				if (bLooks && SpotFor(Live, S, W, Sm.Spot, Sm.Target))
				{
					Sm.Level = Name;
					Sm.Sector = P.Sector;
					Sm.Wall = P.Wall;
					Sm.bTop = P.bTop;
					Sm.bDoor = bDoor;
					if (!Looks.Contains(Name)) LevelOrder.Add(Name);
					Looks.FindOrAdd(Name).Add(Sm);
				}
			}
			Total += Tops + Bots;
			Differ += LvDiffer;
			// the counts: the shut parts grew by exactly the listed walls; nothing else the rule may touch changed
			const int32 OT = GOld.Stats.FindRef(TEXT("wall_top_shut")), NT = GNew.Stats.FindRef(TEXT("wall_top_shut"));
			const int32 OB = GOld.Stats.FindRef(TEXT("wall_bot_shut")), NB = GNew.Stats.FindRef(TEXT("wall_bot_shut"));
			CountBad += (NT - OT != Tops || NB - OB != Bots) ? 1 : 0;
			TArray<FString> Bad;
			for (const TCHAR* K : Kept)
			{
				const int32 O = GOld.Stats.FindRef(K), N = GNew.Stats.FindRef(K);
				if (O != N) Bad.Add(FString::Printf(TEXT("%s %d->%d"), K, O, N));
			}
			KeptBad += Bad.Num() > 0 ? 1 : 0;
			TSet<FString> Keys;
			for (const TPair<FString, int32>& Kv : GOld.Stats) Keys.Add(Kv.Key);
			for (const TPair<FString, int32>& Kv : GNew.Stats) Keys.Add(Kv.Key);
			TArray<FString> Diffs;
			for (const FString& K : Keys)
			{
				const int32 O = GOld.Stats.FindRef(K), N = GNew.Stats.FindRef(K);
				if (O != N) Diffs.Add(FString::Printf(TEXT("%s %d->%d"), *K, O, N));
			}
			Diffs.Sort();
			QuietBad += Tops + Bots == 0 && Diffs.Num() > 0 ? 1 : 0;
			TrisBad += Tris(GOld) != Tris(GNew) ? 1 : 0;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT shutpart level %-9s %d shut walls now drawn as their part (%d top, %d bottom), %d of them look different, %d kept on their mid (DEFAULT part) | world tris %d->%d | %s%s"),
				*Name, Tops + Bots, Tops, Bots, LvDiffer, Parts.Num() - Tops - Bots, Tris(GOld), Tris(GNew), Diffs.Num() ? *FString::Join(Diffs, TEXT(", ")) : TEXT("no count changed"),
				Bad.Num() ? *(TEXT(" | KEPT COUNTS CHANGED: ") + FString::Join(Bad, TEXT(", "))) : TEXT(""));
			// ---- the crack wall and the mill ----
			if (Name == TEXT("civlwar1"))
			{
				const FOutlawsGeometry::FShutPart* Crack = Parts.FindByPredicate([](const FOutlawsGeometry::FShutPart& P) { return P.Sector == 449 && P.Wall == 8; });
				const bool bWall = Live.Sectors.IsValidIndex(449) && Live.Sectors[449].Walls.Num() > 11;
				FString Ns;
				bool bSame = bWall && Crack && !Crack->bTop;
				if (bWall) for (const int32 k : { 4, 6, 7, 9, 10, 11 })
				{
					const FOutlawsWall& Nw = Live.Sectors[449].Walls[k];
					Ns += FString::Printf(TEXT(" w%d %s %.2f"), k, *Live.TexStem(Nw.Mid), Nw.MidY);
					bSame &= Crack && FMath::Abs(Nw.MidY - Crack->Y) < 0.01;
				}
				Check(bSame, FString::Printf(TEXT("civlwar1 449 wall 8 drawn as its %s part, %s x %.2f y %.2f (its mid %s y %.2f); the neighbours' mids:%s"),
					!Crack ? TEXT("(not listed)") : Crack->bTop ? TEXT("top") : TEXT("bottom"), Crack ? *Live.TexStem(Crack->Tex) : TEXT("-"), Crack ? Crack->X : 0.0, Crack ? Crack->Y : 0.0,
					Crack ? *Live.TexStem(Crack->MidTex) : TEXT("-"), Crack ? Crack->MidY : 0.0, *Ns));
				if (bWall)
				{
					// the wall alone, as its mover group builds it, before and after BLAMMO's blast drops its floor to 0.25
					const FOutlawsWall& W = Live.Sectors[449].Walls[8];
					FOutlawsBuildFilter Only;
					Only.bOnly = true;
					Only.OnlyWalls.Add(FOutlawsBuildFilter::Key(449, 8));
					Only.bJediSigns = true;
					FOutlawsGeometry GB, GA;
					GB.Build(Live, NoTex, true, &Only);
					FOutlawsLevel Blown = Live;
					const bool bBlammo = Blown.Sectors.IsValidIndex(W.Adjoin) && Blown.Sectors[W.Adjoin].Name.Equals(TEXT("BLAMMO"), ESearchCase::IgnoreCase);
					if (bBlammo) Blown.Sectors[W.Adjoin].Floor.Y = 0.25;
					GA.Build(Blown, NoTex, true, &Only);
					Check(GB.Stats.FindRef(TEXT("wall_bot_shut")) == 1 && Tris(GB) > 0, FString::Printf(TEXT("before the blast the wall is one bottom part (%d), %d triangles with its crack"), GB.Stats.FindRef(TEXT("wall_bot_shut")), Tris(GB)));
					Check(bBlammo && Tris(GA) == 0 && GA.Stats.FindRef(TEXT("wall_bot_shut")) == 0, FString::Printf(TEXT("after BLAMMO's blast (%s floor 0.25, the opening 0.25..12) no wall part is left: %d triangles"),
						bBlammo ? TEXT("BLAMMO") : TEXT("(beyond is not BLAMMO)"), Tris(GA)));
				}
			}
			if (Name == TEXT("mill"))
			{
				Check(OT > 0 && NT == OT + Tops, FString::Printf(TEXT("the mill's flume gates (DEFAULT mids, BUG-MILLVIS) are shut top parts both ways: %d, the rule adds %d (%d)"), OT, Tops, NT));
			}
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT shutpart total over %d levels: %d shut walls drawn as their part where the mid was, %d of them look different (%d onto doors); %d kept on their mid (a DEFAULT part)"),
			Levels, Total, Differ, Doors, KeptMid);
		const FString ListFile = FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("shutpart_list.txt");
		FFileHelper::SaveStringToFile(FString::Join(List, TEXT("\r\n")) + TEXT("\r\n"), *ListFile);
		Check(Levels >= 30, FString::Printf(TEXT("%d levels built both ways"), Levels));
		Check(Total > 0, FString::Printf(TEXT("%d walls listed (%s)"), Total, *ListFile));
		Check(CountBad == 0, FString::Printf(TEXT("every level's shut parts grew by exactly its listed walls: %d levels differ"), CountBad));
		Check(KeptBad == 0, FString::Printf(TEXT("floors, ceilings, openings' collision, see-through mids, signs and water the same both ways on every level: %d levels differ"), KeptBad));
		Check(QuietBad == 0, FString::Printf(TEXT("every level with no wall listed built the same both ways: %d differ"), QuietBad));
		Check(TrisBad == 0, FString::Printf(TEXT("every level's world triangles the same both ways (no wall left open, a DEFAULT part keeps its mid): %d levels differ"), TrisBad));
		// the sample: the crack wall first, then round by round a wall a level (doors first) until there are 30
		for (TPair<FString, TArray<FSample>>& Kv : Looks) Kv.Value.StableSort([](const FSample& X, const FSample& Y) { return X.bDoor && !Y.bDoor; });
		Samples.Reset();
		if (TArray<FSample>* C1 = Looks.Find(TEXT("civlwar1")))
		{
			const int32 i = C1->IndexOfByPredicate([](const FSample& X) { return X.Sector == 449 && X.Wall == 8; });
			if (i >= 0) { Samples.Add((*C1)[i]); C1->RemoveAt(i); }
		}
		for (int32 Round = 0; Round < 8 && (Round < 1 || Samples.Num() < 30); ++Round)
		{
			for (const FString& Lv : LevelOrder) if (Looks[Lv].IsValidIndex(Round)) Samples.Add(Looks[Lv][Round]);
		}
		// grouped by level (the tour loads each level once), civlwar1 first
		Samples.StableSort([&LevelOrder](const FSample& X, const FSample& Y)
		{
			const int32 a = X.Level == TEXT("civlwar1") ? -1 : LevelOrder.IndexOfByKey(X.Level), b = Y.Level == TEXT("civlwar1") ? -1 : LevelOrder.IndexOfByKey(Y.Level);
			return a < b;
		});
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT shutpart sample: %d walls over %d levels"), Samples.Num(), LevelOrder.Num());
		Next = 0;
		ShotStep = (OLFlags::Param(TEXT("OLShutPartNoShots")) || Samples.Num() == 0) ? 5 : 3;
		Wait = ShotTime;
		return;
	}
	case 1:
	{
		// settled at the spot: look at the wall from the eye
		if (ShotTime < Wait || !bUp) return;
		const FSample& Sm = Samples[Next];
		if (Controller) Controller->SetControlRotation((Sm.Target - EyeNow()).Rotation());
		Wait = ShotTime + 0.8f;
		ShotStep = 2;
		return;
	}
	case 2:
	{
		if (ShotTime < Wait) return;
		const FSample& Sm = Samples[Next];
		const FString Name = FString::Printf(TEXT("shutpart_%s_%d_%d%s"), *Sm.Level, Sm.Sector, Sm.Wall, bOld ? TEXT("_old") : TEXT(""));
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT shutpart shot %s.png (%s part%s; eye %s, %.0f cm from the wall, in sector %d)"), *Name, Sm.bTop ? TEXT("top") : TEXT("bottom"), Sm.bDoor ? TEXT(", a door") : TEXT(""),
			*EyeNow().ToCompactString(), FVector::Dist2D(EyeNow(), Sm.Target), L->PlayerSectorIndex());
		++Next;
		Wait = ShotTime + 0.8f;
		ShotStep = 3;
		return;
	}
	case 3:
	{
		if (ShotTime < Wait) return;
		if (Next >= Samples.Num()) { ShotStep = 5; return; }
		const FSample& Sm = Samples[Next];
		if (!L->LevelName.Equals(Sm.Level, ESearchCase::IgnoreCase))
		{
			L->JumpToLevel(Sm.Level);
			Wait = ShotTime + 3.f;
			ShotStep = 4;
			return;
		}
		TeleportTo(Sm.Spot + FVector(0.0, 0.0, 120.0), FRotator(0.0, (Sm.Target - Sm.Spot).Rotation().Yaw, 0.0), false, true);
		Wait = ShotTime + 1.2f;
		ShotStep = 1;
		return;
	}
	case 4:
		if (ShotTime < Wait || !bUp || !L->LevelName.Equals(Samples[Next].Level, ESearchCase::IgnoreCase)) return;
		L->bInvincible = true;
		Wait = ShotTime + 2.f;
		ShotStep = 3;
		return;
	case 5:
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT shutpart RESULT %s (%d passed, %d failed%s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails, bOld ? TEXT(", -OLOldShutPart") : TEXT(""));
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
