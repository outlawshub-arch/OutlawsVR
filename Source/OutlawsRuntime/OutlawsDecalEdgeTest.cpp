// -OLShots -OLDecalEdgeTest (BUG-DECALEDGE): TNT soot cut to the surface it lands on.
// Blasts at many points round the start, each made twice (ol.DecalClip 0, the whole rectangles as before, then 1): every corner of
// every soot piece is looked for on the level (a trace straight down onto it); a corner with nothing under it hangs out over an
// edge. The worst floor edge, wall end and doorway-like wall case of the "before" run are then photographed, before and after.

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsRuntime.h"
#include "OutlawsDecalClip.h"
#include "ProceduralMeshComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
#include "DrawDebugHelpers.h"
#include "OutlawsShotTests.h"

#if WITH_OUTLAWS_TESTS

namespace
{
	struct FCase
	{
		FVector Blast = FVector::ZeroVector;
		int32 Off = 0, OffFloor = 0, OffWall = 0;   // corners hanging over nothing, the whole-rectangle run (all, on floor soot, on wall soot)
		bool bFloorFocus = true;
		FVector FloorPt = FVector::ZeroVector, WallPt = FVector::ZeroVector, WallN = FVector::ForwardVector;   // the farthest hanging corner of each
	};

	/** Corners of the soot meshes in Added that have no level surface under them (within a centimetre or two of the decal's own plane). */
	void CountOff(UWorld* W, const AOutlawsLevelActor* L, const TArray<UProceduralMeshComponent*>& Added, const FVector& From, int32& Corners, int32& Off, int32& OffFloor, int32& OffWall, FVector& FloorPt, FVector& WallPt, FVector& WallN)
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsDecalEdge), false);
		FCollisionObjectQueryParams Obj;
		Obj.AddObjectTypesToQuery(ECC_WorldStatic);
		Obj.AddObjectTypesToQuery(ECC_WorldDynamic);
		for (UProceduralMeshComponent* M : Added)
		{
			const FProcMeshSection* S = M ? M->GetProcMeshSection(0) : nullptr;
			if (!S) continue;
			const FTransform Tr = M->GetComponentTransform();
			for (int32 i = 0; i + 2 < S->ProcIndexBuffer.Num(); i += 3)
			{
				FVector P[3];
				for (int32 k = 0; k < 3; ++k) P[k] = Tr.TransformPosition(FVector(S->ProcVertexBuffer[S->ProcIndexBuffer[i + k]].Position));
				const FVector N = Tr.TransformVectorNoScale(FVector(S->ProcVertexBuffer[S->ProcIndexBuffer[i]].Normal)).GetSafeNormal();
				const FVector Mid = (P[0] + P[1] + P[2]) / 3.0;
				// (the surface under the piece: the level's own world and sector meshes, or a prop's, a door's: the photographs are of the former)
				bool bWorldHost = false;
				{
					TArray<FHitResult> Hs;
					W->LineTraceMultiByObjectType(Hs, Mid + N * 4.0, Mid - N * 8.0, Obj, Q);
					for (const FHitResult& H : Hs) if (H.GetComponent() && H.GetComponent()->GetOwner() == L && (H.GetComponent()->GetName().StartsWith(TEXT("OutlawsWorld")) || H.GetComponent()->GetName().StartsWith(TEXT("OutlawsSector")))) { bWorldHost = true; break; }
				}
				for (int32 k = 0; k < 3; ++k)
				{
					++Corners;
					// (the corner itself, and a hair in toward the piece's middle: a corner right on a surface's rim is not hanging out)
					bool bOn = false;
					for (const FVector& V : { P[k], P[k] + (Mid - P[k]).GetSafeNormal() * 0.8 })
					{
						TArray<FHitResult> Hits;
						W->LineTraceMultiByObjectType(Hits, V + N * 4.0, V - N * 8.0, Obj, Q);
						for (const FHitResult& H : Hits)
						{
							if (H.GetComponent() && H.GetComponent()->GetOwner() == L && H.GetComponent()->IsVisible() && FMath::Abs(H.Distance - 4.3) < 1.6) { bOn = true; break; }
						}
						if (bOn) break;
					}
					if (!bOn)
					{
						++Off;
						// (the middle of the hanging corners of each kind, summed here and divided below)
						if (!bWorldHost) continue;
						if (FMath::Abs(N.Z) > 0.7) { FloorPt += P[k]; ++OffFloor; }
						else { WallPt += P[k]; WallN = N; ++OffWall; }
					}
				}
			}
		}
	}
}

void AOutlawsPlayer::DecalEdgeTestTick()
{
	AOutlawsLevelActor* L = Level.Get();
	UWorld* W = GetWorld();
	if (!L || !W) return;
	static TArray<FCase> Picks;
	static int32 Phase = 0, Fails = 0;
	static float At = 0.f;
	static FVector EyeOff = FVector::ZeroVector;
	static int32 BeforeCorners = 0, BeforeOff = 0, AfterCorners = 0, AfterOff = 0, Samples = 0;
	IConsoleVariable* Clip = IConsoleManager::Get().FindConsoleVariable(TEXT("ol.DecalClip"));
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLDECAL check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (!bOk) ++Fails;
	};
	auto Shot = [](const FString& Name)
	{
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
	};
	// (the soot itself is faint at its rim, so each photograph also draws every piece's outline in yellow)
	auto Outline = [L, W]()
	{
		int32 Lines = 0;
		for (const TWeakObjectPtr<UProceduralMeshComponent>& Wk : L->DebugScorches())
		{
			const UProceduralMeshComponent* M = Wk.Get();
			const FProcMeshSection* S = M ? const_cast<UProceduralMeshComponent*>(M)->GetProcMeshSection(0) : nullptr;
			if (!S) continue;
			for (int32 i = 0; i + 2 < S->ProcIndexBuffer.Num(); i += 6)
			{
				FVector P[3];
				for (int32 k = 0; k < 3; ++k) P[k] = FVector(S->ProcVertexBuffer[S->ProcIndexBuffer[i + k]].Position) + FVector(S->ProcVertexBuffer[S->ProcIndexBuffer[i + k]].Normal) * 0.6;
				for (int32 k = 0; k < 3; ++k) { DrawDebugLine(W, P[k], P[(k + 1) % 3], FColor::Yellow, true, -1.f, 0, 1.4f); ++Lines; }
			}
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLDECAL outlined %d edges"), Lines);
	};
	if (ShotTime < 3.f || !Clip) return;
	if (Phase == 0)
	{
		EyeOff = Camera->GetComponentLocation() - GetActorLocation();
		UE_LOG(LogOutlaws, Display, TEXT("OLDECAL the eye stands %s from the body"), *EyeOff.ToCompactString());
		// the sample: blast points on the floor in a wide ring round the start, kept clear of walls (no wall within a metre is not
		// required: soot meets edges wherever it is); the same point blasted whole, then cut
		FRandomStream R(0xDECA1);
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsDecalEdgeSample), false, this);
		const FVector Mid = GetActorLocation();
		TArray<FCase> All;
		for (int32 n = 0; n < 700 && Samples < 220; ++n)
		{
			const FVector Top = Mid + FVector(R.FRandRange(-2600.f, 2600.f), R.FRandRange(-2600.f, 2600.f), 250.f);
			FHitResult Hit;
			if (!W->LineTraceSingleByChannel(Hit, Top, Top - FVector(0.0, 0.0, 900.0), ECC_Visibility, Q) || !Hit.GetComponent() || Hit.GetComponent()->GetOwner() != L || Hit.ImpactNormal.Z < 0.9 || !(Hit.GetComponent()->GetName().StartsWith(TEXT("OutlawsWorld")) || Hit.GetComponent()->GetName().StartsWith(TEXT("OutlawsSector")))) continue;
			const FVector Blast = FVector(Hit.ImpactPoint) + FVector(0.0, 0.0, 15.0);
			++Samples;
			FCase C;
			C.Blast = Blast;
			for (int32 Mode = 0; Mode < 2; ++Mode)
			{
				Clip->Set(Mode, ECVF_SetByCode);
				TSet<UProceduralMeshComponent*> Before;
				for (const TWeakObjectPtr<UProceduralMeshComponent>& Wk : L->DebugScorches()) if (UProceduralMeshComponent* M = Wk.Get()) Before.Add(M);
				L->DebugScorch(Blast);
				TArray<UProceduralMeshComponent*> Added;
				for (const TWeakObjectPtr<UProceduralMeshComponent>& Wk : L->DebugScorches()) if (UProceduralMeshComponent* M = Wk.Get()) if (!Before.Contains(M)) Added.Add(M);
				int32 Corners = 0, Off = 0, OffF = 0, OffW = 0;
				FVector Fp = FVector::ZeroVector, Wp = FVector::ZeroVector, Wn = FVector::ForwardVector;
				CountOff(W, L, Added, Blast, Corners, Off, OffF, OffW, Fp, Wp, Wn);
				if (OffF > 0) Fp /= OffF;
				if (OffW > 0) Wp /= OffW;
				if (Mode == 0) { BeforeCorners += Corners; BeforeOff += Off; C.Off = Off; C.OffFloor = OffF; C.OffWall = OffW; C.FloorPt = Fp; C.WallPt = Wp; C.WallN = Wn; }
				else { AfterCorners += Corners; AfterOff += Off; }
				L->DebugClearScorches();
			}
			All.Add(C);
		}
		// the worst floor edge, the worst wall end, and a second wall case far from the first (a doorway's jamb) of the whole-rectangle run
		auto Worst = [&All](bool bFloor, const FVector* Away) -> FCase
		{
			FCase Best;
			Best.Off = -1;
			int32 BestN = 0;
			for (const FCase& C : All)
			{
				const int32 N = bFloor ? C.OffFloor : C.OffWall;
				if (Away && FVector::Dist(C.Blast, *Away) < 400.0) continue;
				if (N > BestN) { BestN = N; Best = C; }
			}
			return Best;
		};
		const FCase A = Worst(true, nullptr);
		const FCase B = Worst(false, nullptr);
		const FCase Cc = Worst(false, B.Off >= 0 ? &B.Blast : nullptr);
		if (A.Off >= 0) { FCase P = A; P.bFloorFocus = true; Picks.Add(P); }
		if (B.Off >= 0) { FCase P = B; P.bFloorFocus = false; Picks.Add(P); }
		if (Cc.Off >= 0) { FCase P = Cc; P.bFloorFocus = false; Picks.Add(P); }
		UE_LOG(LogOutlaws, Display, TEXT("OLDECAL sampled %d blasts: soot corners hanging over nothing: whole rectangles %d of %d (%.1f%%), cut %d of %d (%.2f%%); %s"), Samples, BeforeOff, BeforeCorners,
			BeforeCorners ? 100.0 * BeforeOff / BeforeCorners : 0.0, AfterOff, AfterCorners, AfterCorners ? 100.0 * AfterOff / AfterCorners : 0.0, *OutlawsDecalClip::Describe());
		Check(BeforeOff > 0 && AfterOff * 20 <= BeforeOff, FString::Printf(TEXT("hanging corners fall from %d to %d with the cut"), BeforeOff, AfterOff));
		Check(AfterCorners > 0 && AfterOff * 200 <= AfterCorners, FString::Printf(TEXT("under 0.5%% of the cut corners hang (%d of %d)"), AfterOff, AfterCorners));
		Phase = 1;
		At = ShotTime;
		return;
	}
	// the photographs: each pick stood before from 2.8 m, whole rectangles then cut
	const int32 Pick = (Phase - 1) / 4;
	const int32 Stage = (Phase - 1) % 4;
	if (Pick >= Picks.Num())
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLDECAL done: %d failed"), Fails);
		Clip->Set(1, ECVF_SetByCode);
		FPlatformMisc::RequestExit(false);
		return;
	}
	if (ShotTime - At < (Stage == 0 ? 0.3f : 1.0f)) return;
	const FCase& C = Picks[Pick];
	static const TCHAR* Names[] = { TEXT("floor_edge"), TEXT("wall_end"), TEXT("doorway") };
	if (Stage == 0)
	{
		// look at the farthest hanging corner of the whole-rectangle soot (the middle of it and the blast), from the open side, 3 m off
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsDecalEdgeStand), false, this);
		const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		const FVector Edge = C.bFloorFocus ? C.FloorPt : C.WallPt;
		const FVector Target = Edge;
		const double Elev = C.bFloorFocus ? 62.0 : 14.0;
		FVector EyeAt = Target + FVector(0.0, 0.0, 200.0);
		double BestScore = -1e9;
		for (int32 k = 0; k < 24; ++k)
		{
			const FVector Dir = FRotator(0.0, 15.0 * k, 0.0).Vector();
			if (!C.bFloorFocus && FVector::DotProduct(Dir, FVector(C.WallN.X, C.WallN.Y, 0.0).GetSafeNormal()) < 0.3) continue;
			const FVector Cand = Target + (Dir * FMath::Cos(FMath::DegreesToRadians(Elev)) + FVector::UpVector * FMath::Sin(FMath::DegreesToRadians(Elev))) * 450.0;
			FHitResult H;
			if (W->LineTraceSingleByChannel(H, Target + FVector(0.0, 0.0, 10.0), Cand, ECC_Visibility, Q)) { if (H.Distance > BestScore) { BestScore = H.Distance; EyeAt = Target + (Cand - Target).GetSafeNormal() * (H.Distance - 15.0); } continue; }
			EyeAt = Cand;
			break;
		}
		// (the body is held there, the eye at its height: the capsule hangs under the camera)
		const FVector Stand = EyeAt - EyeOff;
		GetCharacterMovement()->DisableMovement();
		SetActorLocation(Stand, false, nullptr, ETeleportType::TeleportPhysics);
		if (AController* Ctl = GetController()) Ctl->SetControlRotation((Target - EyeAt).Rotation());
		UE_LOG(LogOutlaws, Display, TEXT("OLDECAL pick %d: looking at %s from %s"), Pick + 1, *Target.ToCompactString(), *EyeAt.ToCompactString());
		Clip->Set(0, ECVF_SetByCode);
		L->DebugScorch(C.Blast);
		Outline();
		UE_LOG(LogOutlaws, Display, TEXT("OLDECAL pick %d (%s) at %s: whole rectangles, %d corners over nothing"), Pick + 1, Names[FMath::Min(Pick, 2)], *C.Blast.ToCompactString(), C.Off);
	}
	else if (Stage == 1)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLDECAL camera at %s looking %s (soot meshes %d)"), *Camera->GetComponentLocation().ToCompactString(), *Camera->GetComponentRotation().ToCompactString(), L->DebugScorches().Num());
		Shot(FString::Printf(TEXT("ol_decaledge_%d_%s_before"), Pick + 1, Names[FMath::Min(Pick, 2)]));
		L->DebugClearScorches();
		FlushPersistentDebugLines(W);
		Clip->Set(1, ECVF_SetByCode);
		L->DebugScorch(C.Blast);
		Outline();
	}
	else if (Stage == 2)
	{
		Shot(FString::Printf(TEXT("ol_decaledge_%d_%s_after"), Pick + 1, Names[FMath::Min(Pick, 2)]));
	}
	else
	{
		L->DebugClearScorches();
		FlushPersistentDebugLines(W);
	}
	++Phase;
	At = ShotTime;
}

OL_SHOT_MATCH(OLDecalEdgeTest) { return OLFlags::Param(TEXT("OLDecalEdgeTest")); }
OL_SHOT_TEST(OLDecalEdgeTest, 60)
{
	DecalEdgeTestTick();             // (BUG-DECALEDGE: OutlawsDecalEdgeTest.cpp)
}
#endif // WITH_OUTLAWS_TESTS
