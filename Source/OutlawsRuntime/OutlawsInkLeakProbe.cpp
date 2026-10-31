// BUG-INKLEAK (the user's Quest 3, 1 Nov, build 8e79f9c, civlwar1: 72 fps down to about 7 over four minutes by the start
// area's flags, "Level: flags + lamps" 92 ms a frame): -OLShots -OLLevel=civlwar1 -OLNoFrontEnd -featureleveles31
// -OLInkLeakProbe [-OLOldAnimInk] [-OLOldInkSlots].  At ES3_1 the ink shells are made (OutlawsInk::Wanted).  First AddShell
// on a test mesh re-shelled 50 times (its count; a part hidden and shown; BUG-TNT's hand mesh built again with another own
// count; MakeHalo); then the level's animated models by kind and stem, shelled or not, and its still ones; then the player
// by the start area flag nearest the user's place (UE -32500, -15000; 4.5 to 12 m off its cloth, the first place with it in
// sight), 180 s (-OLInkLeakSeconds=), a line every 10 s: the waving and bending models' sections in all and the bucket's ms
// (mean and most over the 10 s).  Shots inkleak_<mode>_flag_0s.png and _180s.png in Saved\OLShots.  On another level the
// census only, or with -OLInkLeakLook=<stem> a shot of that still model, inkleak_<mode>_<stem>.png.  Lines
// "OLSHOT inkleak check PASS|FAIL: ..." and "OLSHOT inkleak RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsInk.h"
#include "OutlawsPerf.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "ProceduralMeshComponent.h"
#include "Materials/Material.h"
#include "RHI.h"
#include "RHIDefinitions.h"
#include "GameFramework/Controller.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "CollisionQueryParams.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

namespace
{
	bool InkShelled(const UProceduralMeshComponent* M) { return M && OutlawsInk::OwnSections(M) < M->GetNumSections(); }

	/** A box of 8 corners and 12 faces about C, as section Sec. */
	void InkTestBox(UProceduralMeshComponent* M, int32 Sec, const FVector& C, double R)
	{
		TArray<FVector> Pos, Nrm;
		TArray<FVector2D> UV;
		for (int32 i = 0; i < 8; ++i)
		{
			const FVector D((i & 1) ? 1.0 : -1.0, (i & 2) ? 1.0 : -1.0, (i & 4) ? 1.0 : -1.0);
			Pos.Add(C + D * R);
			Nrm.Add(D.GetSafeNormal());
			UV.Add(FVector2D(0.5, 0.5));
		}
		const TArray<int32> Tris = { 0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4, 2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5 };
		M->CreateMeshSection_LinearColor(Sec, Pos, Tris, Nrm, UV, TArray<FLinearColor>(), TArray<FProcMeshTangent>(), false);
	}

	int32 InkVerts(const UProceduralMeshComponent* M, int32 Sec)
	{
		const FProcMeshSection* S = M ? const_cast<UProceduralMeshComponent*>(M)->GetProcMeshSection(Sec) : nullptr;
		return S ? S->ProcVertexBuffer.Num() : -1;
	}
}

OL_SHOT_MATCH(OLInkLeakProbe) { return OLFlags::Param(TEXT("OLInkLeakProbe")); }

OL_SHOT_TEST(OLInkLeakProbe, 1350)
{
	static int32 Passes = 0, Fails = 0, Sample = 0, FirstCount = -1, LastCount = -1;
	static float T0 = 0.f, Wait = 0.f;
	static double SumMs = 0.0, MaxMs = 0.0, SumFrame = 0.0, FirstMean = -1.0, LastMean = -1.0;
	static int32 Frames = 0, Spot = 0;
	static TArray<FVector> Spots;
	static FVector FlagAt = FVector::ZeroVector;
	static TWeakObjectPtr<UProceduralMeshComponent> Flag;
	// 180 s, a line each 10 s (-OLInkLeakSeconds=: shorter, for the shots alone)
	static const int32 Samples = []() { FString S; return OLFlags::Value(TEXT("OLInkLeakSeconds"), S) ? FMath::Clamp(FCString::Atoi(*S) / 10, 1, 60) : 18; }();
	// -OLInkLeakLook=<stem>: a shot of that still model's ink instead of the flags (BUG-LEAFINK's SHRUB01 in hideout)
	static const FString LookStem = []() { FString S; OLFlags::Value(TEXT("OLInkLeakLook"), S); return S.ToUpper(); }();
	if (!L || ShotStep < 0) return;
	const bool bOldAnim = OLFlags::Param(TEXT("OLOldAnimInk"));
	const bool bOldSlots = OLFlags::Param(TEXT("OLOldInkSlots"));
	const TCHAR* const Tag = bOldAnim ? (bOldSlots ? TEXT("before") : TEXT("slots")) : (bOldSlots ? TEXT("oldslots") : TEXT("after"));
	const bool bGrows = bOldAnim && bOldSlots;      // a moving model re-shelled and its slots appended: as on the Quest
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT inkleak check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	auto Moving = [L](TSet<UProceduralMeshComponent*>& Out, bool bClothsOnly)
	{
		TArray<TPair<FString, UProceduralMeshComponent*>> Anim;
		L->DebugAnimatedModels(Anim);
		Out.Reset();
		for (const TPair<FString, UProceduralMeshComponent*>& A : Anim)
		{
			if (!bClothsOnly || A.Key == TEXT("wave") || A.Key == TEXT("bend")) Out.Add(A.Value);
		}
	};
	auto Sections = [&](int32& OutWave, int32& OutBend) -> int32
	{
		TArray<TPair<FString, UProceduralMeshComponent*>> Anim;
		L->DebugAnimatedModels(Anim);
		TSet<UProceduralMeshComponent*> Seen;
		OutWave = OutBend = 0;
		int32 N = 0;
		for (const TPair<FString, UProceduralMeshComponent*>& A : Anim)
		{
			if (A.Key != TEXT("wave") && A.Key != TEXT("bend")) continue;
			(A.Key == TEXT("wave") ? OutWave : OutBend)++;
			bool bDup = false;
			Seen.Add(A.Value, &bDup);
			if (!bDup) N += A.Value->GetNumSections();
		}
		return N;
	};
	auto Shot = [&](const TCHAR* What)
	{
		const FString Name = FString::Printf(TEXT("inkleak_%s_%s"), Tag, What);
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT inkleak shot %s.png"), *Name);
	};
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT inkleak RESULT %s %s (%d passed, %d failed)"), Tag, Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 5.f || !bUp) return;
		L->bInvincible = true;
		Check(OutlawsInk::Wanted(), FString::Printf(TEXT("the mobile renderer's ink shells are made (feature level %d, below SM5 %d)"), int32(GMaxRHIFeatureLevel), int32(ERHIFeatureLevel::SM5)));
		// 1. AddShell itself, on a test mesh of two boxes (hidden: not in the shots)
		UMaterialInterface* Ink = UMaterial::GetDefaultMaterial(MD_Surface);
		UProceduralMeshComponent* M = NewObject<UProceduralMeshComponent>(this, NAME_None, RF_Transient);
		M->bUseAsyncCooking = false;
		M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		M->SetupAttachment(GetRootComponent());
		M->RegisterComponent();
		M->SetHiddenInGame(true);
		InkTestBox(M, 0, FVector(0.0, 0.0, -400.0), 10.0);
		InkTestBox(M, 1, FVector(30.0, 0.0, -400.0), 5.0);
		for (int32 r = 0; r < 50; ++r) OutlawsInk::AddShell(M, 0.6f, Ink);
		if (bOldSlots) Check(M->GetNumSections() > 50, FString::Printf(TEXT("as before, 2 own sections shelled 50 times: %d sections (the slots appended)"), M->GetNumSections()));
		else Check(M->GetNumSections() == 4 && OutlawsInk::OwnSections(M) == 2 && InkVerts(M, 2) == 8 && InkVerts(M, 3) == 8,
			FString::Printf(TEXT("2 own sections shelled 50 times: %d sections, %d own, shell corners %d and %d (wanted 4, 2, 8 and 8)"), M->GetNumSections(), OutlawsInk::OwnSections(M), InkVerts(M, 2), InkVerts(M, 3)));
		if (!bOldSlots)
		{
			// a part hidden (a lamp's flame kept out of the ink): its shell's slot emptied, the count kept; shown again
			M->SetMeshSectionVisible(1, false);
			OutlawsInk::AddShell(M, 0.6f, Ink);
			const int32 HidN = M->GetNumSections(), Hid2 = InkVerts(M, 2), Hid3 = InkVerts(M, 3);
			M->SetMeshSectionVisible(1, true);
			OutlawsInk::AddShell(M, 0.6f, Ink);
			Check(HidN == 4 && Hid2 == 8 && Hid3 == 0 && M->GetNumSections() == 4 && InkVerts(M, 3) == 8,
				FString::Printf(TEXT("a part hidden: %d sections, shell corners %d and %d (wanted 4, 8, 0); shown again: %d sections, %d corners (wanted 4, 8)"), HidN, Hid2, Hid3, M->GetNumSections(), InkVerts(M, 3)));
		}
		// BUG-TNT: a hand's mesh cleared for another weapon (the revolver's 1 section, then the dynamite's 2: stick, wick)
		M->ClearAllMeshSections(); OutlawsInk::Forget(M);
		InkTestBox(M, 0, FVector(0.0, 0.0, -400.0), 10.0);
		OutlawsInk::AddShell(M, 0.6f, Ink);
		const int32 RevN = M->GetNumSections();
		M->ClearAllMeshSections(); OutlawsInk::Forget(M);
		InkTestBox(M, 0, FVector(0.0, 0.0, -400.0), 10.0);
		InkTestBox(M, 1, FVector(0.0, 0.0, -370.0), 3.0);
		for (int32 r = 0; r < 10; ++r) OutlawsInk::AddShell(M, 0.6f, Ink);
		Check(RevN == 2 && OutlawsInk::OwnSections(M) == 2 && InkVerts(M, 1) == 8 && (bOldSlots ? M->GetNumSections() > 10 : M->GetNumSections() == 4),
			FString::Printf(TEXT("BUG-TNT: the revolver %d sections (wanted 2), the dynamite shelled 10 times %d sections (wanted %s), %d own, its wick %d corners (wanted 2, 8)"),
				RevN, M->GetNumSections(), bOldSlots ? TEXT("more than 10, as before") : TEXT("4"), OutlawsInk::OwnSections(M), InkVerts(M, 1)));
		if (UProceduralMeshComponent* H = OutlawsInk::MakeHalo(M, 0.9f, Ink))
		{
			Check(H->GetNumSections() == 2 && InkVerts(H, 0) == 8 && InkVerts(H, 1) == 8, FString::Printf(TEXT("the grab glow's halo round it: %d sections (wanted 2: its own, not its ink)"), H->GetNumSections()));
			H->DestroyComponent();
		}
		else Check(false, TEXT("no halo made"));
		M->DestroyComponent();
		// 2. the census: the animated models (by kind and stem) and the still ones, shelled or not
		UProceduralMeshComponent* LookMesh = nullptr;
		TArray<TPair<FString, UProceduralMeshComponent*>> Anim;
		L->DebugAnimatedModels(Anim);
		TMap<FString, int32> ByStem;
		TSet<UProceduralMeshComponent*> AnimSet;
		int32 AnimShelled = 0;
		for (const TPair<FString, UProceduralMeshComponent*>& A : Anim)
		{
			const AOutlawsSpriteActor* S = Cast<AOutlawsSpriteActor>(A.Value->GetOwner());
			ByStem.FindOrAdd(FString::Printf(TEXT("%s %s"), *A.Key, S ? *S->ModelStem : TEXT("?")))++;
			bool bDup = false;
			AnimSet.Add(A.Value, &bDup);
			if (!bDup && InkShelled(A.Value)) ++AnimShelled;
		}
		ByStem.KeySort(TLess<FString>());
		FString List;
		for (const TPair<FString, int32>& Kv : ByStem) List += FString::Printf(TEXT(" %s x%d,"), *Kv.Key, Kv.Value);
		int32 Still = 0, StillShelled = 0;
		TMap<FString, int32> StillStems;
		for (TActorIterator<AOutlawsSpriteActor> It(GetWorld()); It; ++It)
		{
			if (It->ModelStem.IsEmpty()) continue;
			TArray<UProceduralMeshComponent*> Pm;
			It->GetComponents<UProceduralMeshComponent>(Pm);
			for (UProceduralMeshComponent* P : Pm)
			{
				if (AnimSet.Contains(P)) continue;
				++Still;
				if (InkShelled(P)) { ++StillShelled; StillStems.FindOrAdd(It->ModelStem)++; }
				if (!LookMesh && !LookStem.IsEmpty() && It->ModelStem.Equals(LookStem, ESearchCase::IgnoreCase)) LookMesh = P;
			}
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT inkleak census %s: %d animated models (%d shelled):%s still models %d (%d shelled, %d stems)"), *L->LevelName, AnimSet.Num(), AnimShelled, *List.LeftChop(1), Still, StillShelled, StillStems.Num());
		if (bOldAnim) Check(AnimSet.Num() == 0 || AnimShelled > 0, FString::Printf(TEXT("as before, %d of the %d animated models shelled"), AnimShelled, AnimSet.Num()));
		else Check(AnimShelled == 0, FString::Printf(TEXT("%d of the %d animated models shelled (wanted none: user ruling 1 Nov)"), AnimShelled, AnimSet.Num()));
		Check(Still == 0 || StillShelled > 0, FString::Printf(TEXT("the still models keep their ink: %d of %d shelled"), StillShelled, Still));
		if (!LookStem.IsEmpty())
		{
			Check(LookMesh != nullptr, FString::Printf(TEXT("a still %s here (%s)"), *LookStem, LookMesh ? (InkShelled(LookMesh) ? TEXT("shelled") : TEXT("no shell")) : TEXT("none")));
		}
		else if (!L->LevelName.Equals(TEXT("civlwar1"), ESearchCase::IgnoreCase))
		{
			Finish();
			return;
		}
		// 3. the start area's flags: the one nearest the user's place (-OLInkLeakLook=: that still model, a shot alone)
		const FVector User = LookStem.IsEmpty() ? FVector(-32500.0, -15000.0, 89.0) : GetActorLocation();
		UProceduralMeshComponent* Best = LookMesh;
		int32 InBox = 0;
		for (const TPair<FString, UProceduralMeshComponent*>& A : Anim)
		{
			if (A.Key != TEXT("wave") || !LookStem.IsEmpty()) continue;
			const FVector P = A.Value->Bounds.Origin;
			if (P.X > -36000.0 && P.X < -31000.0 && P.Y > -19000.0 && P.Y < -9800.0) ++InBox;
			if (!Best || FVector::DistSquared2D(P, User) < FVector::DistSquared2D(Best->Bounds.Origin, User)) Best = A.Value;
		}
		if (LookStem.IsEmpty()) Check(InBox > 0, FString::Printf(TEXT("%d waving models in the user's area (UE X -36000..-31000, Y -19000..-9800)"), InBox));
		if (!Best)
		{
			Finish();
			return;
		}
		Flag = Best;
		FlagAt = Best->Bounds.Origin;
		const FVector E = Best->Bounds.BoxExtent;
		FVector Face = E.X < E.Y ? FVector(1.0, 0.0, 0.0) : FVector(0.0, 1.0, 0.0);
		if (FVector::DotProduct(User - FlagAt, Face) < 0.0) Face = -Face;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT inkleak flag %s at %s (extent %s)"), Cast<AOutlawsSpriteActor>(Best->GetOwner()) ? *Cast<AOutlawsSpriteActor>(Best->GetOwner())->ModelStem : TEXT("?"), *FlagAt.ToString(), *E.ToString());
		// places to look at it from, facing its cloth: level with its foot (on what it stands on), then from the ground;
		// near first, each side; the first with a clear line from the eye to the cloth's middle (after the fall) is used
		Spots.Reset();
		for (const double Z : { FlagAt.Z - E.Z + 110.0, User.Z + 20.0 })
		{
			for (const double D : { 450.0, 800.0, 1200.0 })
			{
				for (const double Side : { 1.0, -1.0 }) Spots.Add(FVector(FlagAt.X + Face.X * D * Side, FlagAt.Y + Face.Y * D * Side, Z));
			}
		}
		Spot = 0;
		ShotStep = 1;
		return;
	}
	case 1:
	{
		TeleportTo(Spots[Spot], FRotator::ZeroRotator, false, false);
		Wait = ShotTime + 1.2f;
		ShotStep = 2;
		return;
	}
	case 2:
	{
		if (ShotTime < Wait) return;
		const APlayerController* Pc = Cast<APlayerController>(Controller);
		const FVector Eye = Pc && Pc->PlayerCameraManager ? Pc->PlayerCameraManager->GetCameraLocation() : GetActorLocation() + FVector(0.0, 0.0, 70.0);
		FCollisionQueryParams Q(TEXT("OLInkLeakSight"), false, this);
		if (Flag.IsValid()) Q.AddIgnoredActor(Flag->GetOwner());
		FHitResult Hit;
		const bool bClear = !GetWorld()->LineTraceSingleByChannel(Hit, Eye, FlagAt, ECC_Visibility, Q) || FVector::Dist(Hit.ImpactPoint, FlagAt) < 80.0;
		if (!bClear && Spot + 1 < Spots.Num())
		{
			++Spot;
			ShotStep = 1;
			return;
		}
		const FRotator Look = (FlagAt - Eye).Rotation();
		SetActorRotation(FRotator(0.0, Look.Yaw, 0.0));
		if (Controller) Controller->SetControlRotation(Look);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT inkleak the player at %s (place %d of %d), the eye %.0f cm from the flag, %s"), *GetActorLocation().ToString(), Spot + 1, Spots.Num(), FVector::Dist(Eye, FlagAt), bClear ? TEXT("in sight") : TEXT("NOT in sight from any"));
		T0 = ShotTime;
		Wait = T0 + 2.f;
		Sample = 0;
		SumMs = MaxMs = SumFrame = 0.0;
		Frames = 0;
		ShotStep = 3;
		return;
	}
	case 3:
	{
		// a shot of the flag 2 s in, then a line each 10 s for 180 s (the still model: the shot alone)
		if (ShotTime < Wait) return;
		Shot(LookStem.IsEmpty() ? TEXT("flag_0s") : *LookStem);
		Wait = LookStem.IsEmpty() ? T0 + 10.f : ShotTime + 1.5f;
		ShotStep = LookStem.IsEmpty() ? 4 : 5;
		return;
	}
	case 4:
	{
		const double V = OutlawsPerf::LastFrameMs(TEXT("Level: flags + lamps"));
		SumMs += V;
		MaxMs = FMath::Max(MaxMs, V);
		SumFrame += Dt * 1000.0;
		++Frames;
		if (ShotTime < Wait) return;
		int32 NW = 0, NB = 0;
		const int32 N = Sections(NW, NB);
		const double Mean = Frames ? SumMs / Frames : 0.0;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT inkleak at %3.0f s: %d waving + %d bending models, %d sections in all; flags + lamps %.3f ms mean, %.3f most; frame %.1f ms mean (%d frames)"),
			ShotTime - T0, NW, NB, N, Mean, MaxMs, Frames ? SumFrame / Frames : 0.0, Frames);
		if (FirstCount < 0) { FirstCount = N; FirstMean = Mean; }
		LastCount = N;
		LastMean = Mean;
		SumMs = MaxMs = SumFrame = 0.0;
		Frames = 0;
		if (++Sample < Samples)
		{
			Wait = T0 + 10.f * (Sample + 1);
			return;
		}
		Shot(*FString::Printf(TEXT("flag_%ds"), Samples * 10));
		TSet<UProceduralMeshComponent*> Cloths;
		Moving(Cloths, true);
		int32 Shelled = 0;
		for (UProceduralMeshComponent* P : Cloths) Shelled += InkShelled(P) ? 1 : 0;
		if (bGrows) Check(LastCount > FirstCount * 2, FString::Printf(TEXT("as before (re-shelled, slots appended): the waving and bending models' sections %d at 10 s, %d at %d s (growing)"), FirstCount, LastCount, Samples * 10));
		else Check(LastCount == FirstCount, FString::Printf(TEXT("the waving and bending models' sections %d at 10 s, %d at %d s (wanted flat)"), FirstCount, LastCount, Samples * 10));
		if (!bOldAnim) Check(Shelled == 0 && LastMean < 1.0, FString::Printf(TEXT("%d s on, %d of the %d cloths and plants shelled (wanted none), flags + lamps %.3f ms mean over the last 10 s (wanted under 1)"), Samples * 10, Shelled, Cloths.Num(), LastMean));
		else UE_LOG(LogOutlaws, Display, TEXT("OLSHOT inkleak flags + lamps %.3f ms at 10 s, %.3f ms at %d s (%d of the %d cloths and plants shelled)"), FirstMean, LastMean, Samples * 10, Shelled, Cloths.Num());
		Wait = ShotTime + 1.5f;
		ShotStep = 5;
		return;
	}
	case 5:
		if (ShotTime < Wait) return;
		Finish();
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
