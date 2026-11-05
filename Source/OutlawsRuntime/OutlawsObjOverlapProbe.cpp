// BUG-OBJOVERLAP (user 5 Nov, snapshots #26-#28, mill, Quest: "an object in another sector shows in this one", the sharing
// sectors' walls and floors rightly undrawn): -OLShots -OLLevel=mill -OLNoFrontEnd -OLObjOverlapProbe [-OLOldObjOverlap]
// [-OLObjOverlapNoScan].  (1) Every stock level: its sharing pairs, the things in those sectors, and those within the sprite
// culling's arm's reach of an eye standing or crouching in the partner with the flood not reaching their own sector: one line
// "OLSHOT objoverlap level ...", one "OLSHOT objoverlap leak ..." a thing.  (2) The live level: the mill's snapshot eyes, the
// thing's own sector as a control, then the same thing as the eye goes back in, and up to 12 of the level's own leaks.  A
// thing the overlap choice leaves undrawn must be hidden; shots objoverlap_<n>[_old].png in Saved\OLShots.  Lines "OLSHOT
// objoverlap check PASS|FAIL: ..." and "OLSHOT objoverlap RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsLevel.h"
#include "OutlawsLab.h"
#include "OutlawsRuntime.h"
#include "OutlawsRuntimeLibrary.h"
#include "OutlawsFlags.h"
#include "OutlawsGeometry.h"
#include "OutlawsShotTests.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/PrimitiveComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

void AOutlawsLevelActor::DebugObjOverlapScan(const FOutlawsLevel& Lv, int32& OutPairs, int32& OutSectors, int32& OutThings, int32& OutWithin, TArray<FObjOverlapLeak>& OutLeaks)
{
	// the runtime's own pair search over this plan, its lists put back
	TArray<TPair<int32, int32>> KeptPairs;
	FOutlawsBuildFilter KeptCarried = OverlapCarried;
	Swap(KeptPairs, OverlapPairs);
	TSet<int32> Sectors;
	FindOverlaps(Lv, FOutlawsBuildFilter(), Sectors);
	TMultiMap<int32, int32> Partners;
	for (const TPair<int32, int32>& P : OverlapPairs) { Partners.Add(P.Key, P.Value); Partners.Add(P.Value, P.Key); }
	OutPairs = OverlapPairs.Num();
	OutSectors = Sectors.Num();
	Swap(KeptPairs, OverlapPairs);
	OverlapCarried = KeptCarried;
	OutThings = OutWithin = 0;
	const double Reach = 60.0 / FOutlawsGeometry::UnitsToCm;
	const double Step = 0.25;
	for (const FOutlawsObject& O : Lv.Objects)
	{
		const FOutlawsSector* A = Lv.SectorById(O.SectorId);
		if (!A || !Sectors.Contains(A->Index)) continue;
		++OutThings;
		const FVector2D Plan(O.Pos.X, O.Pos.Z);
		TArray<int32> Bs;
		Partners.MultiFind(A->Index, Bs);
		bool bInside = false, bLeak = false;
		int32 EyesLeak = 0, EyesSeen = 0;
		FObjOverlapLeak First;
		for (const int32 Bi : Bs)
		{
			const FOutlawsSector& B = Lv.Sectors[Bi];
			if (Lv.ContainsXZ(B, Plan.X, Plan.Y) && O.Pos.Y >= Lv.SurfaceY(B, Plan, true) - 0.05 && O.Pos.Y <= Lv.SurfaceY(B, Plan, false) + 0.05) bInside = true;
			for (double dy = -Reach; dy <= Reach; dy += Step)
			{
				for (double dx = -Reach; dx <= Reach; dx += Step)
				{
					const FVector2D E = Plan + FVector2D(dx, dy);
					if (!Lv.ContainsXZ(B, E.X, E.Y)) continue;
					const double Fl = Lv.SurfaceY(B, E, true), Ce = Lv.SurfaceY(B, E, false);
					for (const double Up : { 5.4, 2.6 })
					{
						const double Ez = Fl + Up;
						if (Ez > Ce - 0.1 || FVector(dx, Ez - O.Pos.Y, dy).Size() >= Reach) continue;
						TSet<int32> Seen;
						DebugFloodBy(&Lv, Bi, E, Ez, false, &Seen);
						if (Seen.Contains(A->Index)) { ++EyesSeen; continue; }
						++EyesLeak;
						if (!bLeak) First = { O.Name, FVector(O.Pos), A->Index, Bi, E, Ez };
						bLeak = true;
					}
				}
			}
		}
		OutWithin += bInside ? 1 : 0;
		if (bLeak)
		{
			OutLeaks.Add(First);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT objoverlap leak %s %s (id %s) at %.2f %.2f %.2f in %d, from %d: %d eyes in reach unseen, %d seen; inside the partner's height %s"),
				*LevelName, *O.Name, *O.Id, O.Pos.X, O.Pos.Y, O.Pos.Z, A->Index, First.From, EyesLeak, EyesSeen, bInside ? TEXT("yes") : TEXT("no"));
		}
		else if (bInside || EyesSeen > 0)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT objoverlap seen %s %s at %.2f %.2f %.2f in %d: %d eyes in reach all flood to it; inside the partner's height %s"),
				*LevelName, *O.Name, O.Pos.X, O.Pos.Y, O.Pos.Z, A->Index, EyesSeen, bInside ? TEXT("yes") : TEXT("no"));
		}
	}
}

double AOutlawsLevelActor::DebugOverlapOffNs(double* OutReadNs)
{
	const FOutlawsLevel* Lv = DebugLiveLevel();
	if (!Lv) return 0.0;
	if (OutReadNs)
	{
		// one sprite's read of the bits (UpdateCull), walking the sectors so the branch and the cache are not flattered
		const int32 Reads = 20000000, Num = Lv->Sectors.Num();
		int32 Hits = 0;
		const TBitArray<>* Bits = &OverlapOffBits;
		const double R0 = FPlatformTime::Seconds();
		for (int32 r = 0; r < Reads; ++r)
		{
			const int32 Si = int32((uint32(r) * 2654435761u) % uint32(Num));
			Hits += Bits && Bits->IsValidIndex(Si) && (*Bits)[Si] ? 1 : 0;
		}
		const double R1 = FPlatformTime::Seconds();
		for (int32 r = 0; r < Reads; ++r) Hits += int32((uint32(r) * 2654435761u) % uint32(Num)) & 1;
		const double R2 = FPlatformTime::Seconds();
		*OutReadNs = FMath::Max(0.0, ((R1 - R0) - (R2 - R1)) * 1e9 / Reads);
		UE_LOG(LogOutlaws, Verbose, TEXT("OLSHOT objoverlap reads %d"), Hits);
	}
	const int32 Runs = 2000;
	const double T0 = FPlatformTime::Seconds();
	for (int32 r = 0; r < Runs; ++r)
	{
		OverlapOffBits.Init(false, Lv->Sectors.Num());
		for (const int32 Hs : OverlapHidden) if (Hs >= 0 && Hs < OverlapOffBits.Num()) OverlapOffBits[Hs] = true;
	}
	return (FPlatformTime::Seconds() - T0) * 1e9 / Runs;
}

OL_SHOT_MATCH(OLObjOverlapProbe) { return OLFlags::Param(TEXT("OLObjOverlapProbe")); }

OL_SHOT_TEST(OLObjOverlapProbe, 2495)
{
	static int32 Passes = 0, Fails = 0, Spot = 0, Leak = 0, LeakDrawn = 0, LeakHidden = 0;
	static float Wait = 0.f, OffAt = -1.f, CullAt = -1.f, StandOff = 61.4f;
	static bool bHopped = false, bPin = false;
	static FVector PinAt = FVector::ZeroVector;
	static TWeakObjectPtr<AOutlawsSpriteActor> Thing;
	static TArray<AOutlawsLevelActor::FObjOverlapLeak> Leaks;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT objoverlap check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	const bool bOld = OLFlags::Param(TEXT("OLOldObjOverlap"));
	APlayerController* PC = Cast<APlayerController>(Controller);
	auto EyeNow = [PC]() { return PC && PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : FVector::ZeroVector; };
	// the body under Eye, in two hops: 30 cm low first, a moment, so the pick by the feet takes the lower of two sharing
	// sectors as a walk along that floor would have, then up into place, which the tracking follows.  A teleport a tick lets
	// the tracking flip between the two; the body is held there by a plain move instead (no fall, no climb, no rescue)
	auto PlaceEye = [this, L](const FVector& Eye, const FRotator& Look, bool bLow)
	{
		if (bLow) L->NotePlayerTeleported();
		PinAt = Eye - FVector(0.0, 0.0, StandOff + (bLow ? 30.0 : 0.0));
		bPin = true;
		TeleportTo(PinAt, FRotator(0.0, Look.Yaw, 0.0), false, true);
		if (Controller) Controller->SetControlRotation(Look);
	};
	if (bPin) { SetActorLocation(PinAt, false, nullptr, ETeleportType::TeleportPhysics); GetCharacterMovement()->Velocity = FVector::ZeroVector; }
	auto Nearest = [this](const FVector& P)
	{
		AOutlawsSpriteActor* Best = nullptr;
		double Bd = 1e30;
		for (TActorIterator<AOutlawsSpriteActor> It(GetWorld()); It; ++It)
		{
			const double D = FVector::DistSquared(It->GetActorLocation(), P);
			if (D < Bd) { Bd = D; Best = *It; }
		}
		return Best;
	};
	auto Say = [L, &EyeNow](const TCHAR* Where, const AOutlawsSpriteActor* S)
	{
		const int32 Cs = S ? S->DebugCullSector() : -1;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT objoverlap %s: eye %s, %.1f cm from %s (sector %d, model %s), body sector %d; flood reaches it %s, overlap choice leaves it off %s, culled %s"),
			Where, *EyeNow().ToCompactString(), S ? FVector::Dist(EyeNow(), S->GetActorLocation()) : -1.0, S ? *S->OutlawsName : TEXT("?"), Cs, S && S->Model ? *S->Model->GetName() : TEXT("none"),
			L->PlayerSectorIndex(), L->DebugSeenNow(Cs) ? TEXT("yes") : TEXT("no"), L->DebugOverlapOff(Cs) ? TEXT("yes") : TEXT("no"), S && S->DebugCulled() ? TEXT("yes") : TEXT("no"));
	};
	auto Shot = [bOld](const FString& Name)
	{
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / (TEXT("objoverlap_") + Name + (bOld ? TEXT("_old") : TEXT("")) + TEXT(".png")), false, false);
	};
	// the snapshots (info.txt): the eye and the look; the thing 41 cm off the body
	struct FSnap { int32 Shot; FVector Eye; FRotator Look; bool bInReach; };
	static const FSnap Snaps[] = {
		{ 27, FVector(2942.827, 9436.220, 287.001), FRotator(13.52, -37.94, 0.0), true },
		{ 28, FVector(2995.520, 9423.688, 286.998), FRotator(13.60, -41.04, 0.0), true },
		{ 26, FVector(2942.824, 9436.323, 291.486), FRotator(13.96, -38.09, 0.0), false },
	};
	const FVector ThingAt(2967.9, 9408.9, 240.5);
	const bool bMill = L->LevelName.Equals(TEXT("mill"), ESearchCase::IgnoreCase);
	const int32 NSnaps = bMill ? UE_ARRAY_COUNT(Snaps) : 0;
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 4.f || !bUp) return;
		L->bInvincible = true;
		GetCharacterMovement()->DisableMovement();
		StandOff = FMath::Max(1.f, float(EyeNow().Z - GetActorLocation().Z));
		const FOutlawsLevel* Live = L->DebugLiveLevel();
		if (!Live) { Check(false, TEXT("no live level")); ShotStep = 8; return; }
		// (1) every stock level
		if (!OLFlags::Param(TEXT("OLObjOverlapNoScan")))
		{
			const double T0 = FPlatformTime::Seconds();
			const FString Root = FOutlawsAssets::FindInstallRoot(L->InstallPath);
			TSharedPtr<FOutlawsAssets> AP = Root.IsEmpty() ? nullptr : FOutlawsAssets::GetShared(Root);
			const TArray<FString> Levels = UOutlawsRuntimeLibrary::ListLevels(L->InstallPath);
			int32 WithThings = 0, TotalThings = 0, TotalLeaks = 0;
			for (const FString& Name : Levels)
			{
				FString Lvt, Obt;
				if (!AP || !AP->GetText(Name + TEXT(".lvt"), Lvt)) continue;
				AP->GetText(Name + TEXT(".obt"), Obt);
				FOutlawsLevel Lv;
				Lv.ParseLvt(Lvt);
				Lv.ParseObt(Obt);
				const FString KeptName = L->LevelName;
				L->LevelName = Name;
				int32 Pairs = 0, Sectors = 0, Things = 0, Within = 0;
				TArray<AOutlawsLevelActor::FObjOverlapLeak> Ls;
				L->DebugObjOverlapScan(Lv, Pairs, Sectors, Things, Within, Ls);
				L->LevelName = KeptName;
				if (Pairs > 0) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT objoverlap level %s: %d pairs, %d sectors, %d things in them, %d of them inside the partner's height, %d in reach of an eye that does not see them"), *Name, Pairs, Sectors, Things, Within, Ls.Num());
				WithThings += Things > 0 ? 1 : 0;
				TotalThings += Things;
				TotalLeaks += Ls.Num();
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT objoverlap scan: %d levels, %d with things in sharing sectors, %d things, %d in reach unseen (%.1f s)"), Levels.Num(), WithThings, TotalThings, TotalLeaks, FPlatformTime::Seconds() - T0);
		}
		int32 Pairs = 0, Sectors = 0, Things = 0, Within = 0;
		Leaks.Reset();
		L->DebugObjOverlapScan(*Live, Pairs, Sectors, Things, Within, Leaks);
		double ReadNs = 0.0;
		const double RebuildNs = L->DebugOverlapOffNs(&ReadNs);
		int32 Sprites = 0;
		for (TActorIterator<AOutlawsSpriteActor> It(GetWorld()); It; ++It) ++Sprites;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT objoverlap live %s: %d pairs, %d sectors, %d things, %d leaks; one rebuild of the bits %.0f ns, one read %.2f ns, %d sprites"), *L->LevelName, Pairs, Sectors, Things, Leaks.Num(), RebuildNs, ReadNs, Sprites);
		Thing = bMill ? Nearest(ThingAt) : nullptr;
		Spot = 0;
		Wait = 0.f;
		ShotStep = bMill ? 1 : 4;
		return;
	}
	case 1: // the snapshots' eyes
	{
		Wait += Dt;
		if (!bHopped) { bHopped = true; PlaceEye(Snaps[Spot].Eye, Snaps[Spot].Look, true); return; }
		if (Wait < 0.2f) return;
		if (Wait - Dt < 0.2f) { PlaceEye(Snaps[Spot].Eye, Snaps[Spot].Look, false); return; }
		if (Wait < 3.f) return;
		const FSnap& Sn = Snaps[Spot];
		AOutlawsSpriteActor* S = Thing.Get();
		Say(*FString::Printf(TEXT("snapshot %d"), Sn.Shot), S);
		const bool bOff = S && L->DebugOverlapOff(S->DebugCullSector());
		Check(bOff && L->PlayerSectorIndex() == 592, FString::Printf(TEXT("snapshot %d: body in 592, the walls and floor of the thing's sector off here"), Sn.Shot));
		Check(S && S->DebugCulled(), FString::Printf(TEXT("snapshot %d: the thing (%.1f cm off) goes with them"), Sn.Shot, S ? FVector::Dist(EyeNow(), S->GetActorLocation()) : -1.0));
		Shot(FString::Printf(TEXT("%d"), Sn.Shot));
		Wait = 0.f;
		bHopped = false;
		if (++Spot >= NSnaps) { Spot = 0; ShotStep = 2; }
		else ShotStep = 11;
		return;
	}
	case 11: // (the shot written before the next pose)
		Wait += Dt;
		if (Wait >= 0.3f) { Wait = 0.f; ShotStep = 1; }
		return;
	case 2: // control: an eye in the thing's own sector over it, the thing drawn
	{
		AOutlawsSpriteActor* S = Thing.Get();
		const FOutlawsLevel* Live = L->DebugLiveLevel();
		const FOutlawsSector* A = S && Live ? Live->Sector(S->DebugCullSector()) : nullptr;
		if (!A) { Check(false, TEXT("the thing's sector")); ShotStep = 4; return; }
		// (the belt's place along the same corridor, 2.8 m off: a jump, so the pick takes the floor stood on)
		const FVector2D P = FOutlawsGeometry::ToOutlawsPlan(S->GetActorLocation());
		const double Fl = Live->SurfaceY(*A, P, true);
		const FVector Eye = FOutlawsGeometry::ToUE(FVector2D(318.52, 89.65), Fl + 5.4);
		if (!bHopped) { bHopped = true; L->NotePlayerTeleported(); PlaceEye(Eye, (S->GetActorLocation() - Eye).Rotation(), false); }
		Wait += Dt;
		if (Wait < 3.f) return;
		Say(TEXT("control"), S);
		Check(!S->DebugCulled() && L->PlayerSectorIndex() == A->Index, TEXT("control: from its own sector the thing is drawn"));
		Shot(TEXT("control"));
		Wait = 0.f;
		bHopped = false;
		OffAt = CullAt = -1.f;
		ShotStep = 3;
		return;
	}
	case 3: // back to snapshot 26's eye, out of reach: the thing goes with the walls, not a hold later
	{
		AOutlawsSpriteActor* S = Thing.Get();
		if (!bHopped) { bHopped = true; PlaceEye(Snaps[2].Eye, Snaps[2].Look, true); return; }
		Wait += Dt;
		if (OffAt < 0.f && S && L->DebugOverlapOff(S->DebugCullSector())) OffAt = Wait;
		if (Wait >= 0.2f && Wait - Dt < 0.2f) PlaceEye(Snaps[2].Eye, Snaps[2].Look, false);
		if (OffAt >= 0.f && CullAt < 0.f && S && S->DebugCulled()) CullAt = Wait;
		if (CullAt >= 0.f && Wait > 0.5f)
		{
			Say(TEXT("back in"), S);
			Check(CullAt - OffAt < 0.2f, FString::Printf(TEXT("back in: hidden %.2f s after its walls went"), CullAt - OffAt));
			Wait = 0.f;
			bHopped = false;
			ShotStep = 4;
		}
		else if (Wait > 5.f) { Check(false, TEXT("back in: still drawn after 5 s")); Wait = 0.f; bHopped = false; ShotStep = 4; }
		return;
	}
	case 4: // the level's own leaks, from each one's eye
	{
		if (Leak >= FMath::Min(Leaks.Num(), 12)) { ShotStep = 8; return; }
		const AOutlawsLevelActor::FObjOverlapLeak& Lk = Leaks[Leak];
		const FVector Eye = FOutlawsGeometry::ToUE(Lk.Eye, Lk.EyeZ);
		const FVector At = FOutlawsGeometry::ToUE(double(Lk.Pos.X), double(Lk.Pos.Y), double(Lk.Pos.Z));
		if (!bHopped) { bHopped = true; PlaceEye(Eye, (At - Eye).Rotation(), true); return; }
		Wait += Dt;
		if (Wait < 0.2f) return;
		if (Wait - Dt < 0.2f) { PlaceEye(Eye, (At - Eye).Rotation(), false); return; }
		// (an eye a crouch's height over the floor: the body crouches, the eye goes down with it: put back)
		if ((Wait >= 1.f && Wait - Dt < 1.f) || (Wait >= 1.6f && Wait - Dt < 1.6f)) PinAt.Z += Eye.Z - EyeNow().Z;
		if (Wait < 2.5f) return;
		AOutlawsSpriteActor* S = Nearest(At);
		const bool bHere = S && FVector::Dist2D(S->GetActorLocation(), At) < 2.0;
		const FString Where = FString::Printf(TEXT("leak %d %s in %d from %d"), Leak, *Lk.Name, Lk.Sector, Lk.From);
		Say(*Where, S);
		if (!bHere)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT objoverlap %s: spawned nothing here (a logic or difficulty object)"), *Where);
		}
		else
		{
			const bool bOff = L->DebugOverlapOff(S->DebugCullSector());
			if (bOff && !S->DebugCulled()) ++LeakDrawn;
			else if (bOff) ++LeakHidden;
			if (bOff) Check(S->DebugCulled(), *FString::Printf(TEXT("%s: goes with its sector's walls"), *Where));
			else UE_LOG(LogOutlaws, Display, TEXT("OLSHOT objoverlap %s: its walls are drawn from this eye (the body's flood or a hold), the thing %s"), *Where, S->DebugCulled() ? TEXT("hidden") : TEXT("drawn"));
		}
		Shot(FString::Printf(TEXT("%s_%d"), *L->LevelName, Leak));
		++Leak;
		Wait = 0.f;
		bHopped = false;
		return;
	}
	case 8:
		Wait += Dt;
		if (Wait < 1.f) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT objoverlap RESULT %s: %d passed, %d failed (%s rule on %s; leaks hidden %d, drawn %d)"),
			Fails == 0 ? TEXT("PASS") : TEXT("FAIL"), Passes, Fails, bOld ? TEXT("old") : TEXT("new"), *L->LevelName, LeakHidden, LeakDrawn);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
