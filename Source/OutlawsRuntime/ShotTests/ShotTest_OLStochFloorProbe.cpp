// -OLShots -OLStochFloorProbe (REQ-STOCHFLOOR, the user 8 Nov: Stochastic Textures on floors only, never walls, never animated or
// scrolling floors; fifteen ice, granite, stone and hay floors join the natural list).  Starts in -OLLevel (hideout) and goes
// through the levels the fifteen are laid in (hideout, icecaves, civlwar1, villa, miner, cliff, mill; -OLStochFloorLevels=a,b to
// choose), the option on.  In each, every drawn world section's faces by kind (a vertex normal up: floor, down: ceiling, else
// wall) and its material's Stochastic: a floor of a stochastic texture (AOutlawsLevelActor::IsStochasticFloorStem) that is not
// animated (ATX), not swirling and not scrolling must have 1, every wall and ceiling and every other floor 0.  Lines
// "OLSHOT stochfloor <level> <stem>: floor <n> wall <n> ceil <n> triangles ..." per stem of the fifteen there, the level's
// sections and materials, and "OLSHOT stochfloor RESULT <n> PASS, <m> FAIL".  -OLOldStochAll (as before: every surface of a
// natural texture, walls too, none of the fifteen) must FAIL.
// -OLStochFloorShots: also a look at the floor of each of the fifteen in its level, from over its biggest floor triangle
// (stochfloor_<stem>_<level>.png in Saved\OLShots), and the GPU frame time there with the option on and off ("OLSHOT
// stochfloor gpu").  Bad guys are hidden and stilled so nothing stands in the view.

#include "ShotTestCommon.h"
#include "RHI.h"

#if WITH_OUTLAWS_TESTS

namespace
{
	const TCHAR* const GStochFloorStems[] = { TEXT("GFDRYTBL"), TEXT("Y1FICE2"), TEXT("Y1FICE3"), TEXT("Y1CIRK2"), TEXT("Y1FIUW"), TEXT("FGRNTE01"),
		TEXT("FGRNTE02"), TEXT("GCGRNIT1"), TEXT("GCGRNIT2"), TEXT("DFSTON2"), TEXT("DFSTON3"), TEXT("DC3"), TEXT("HFHAYFLR"), TEXT("FWHAYFLR"), TEXT("MFEX1") };

	/** The level each stem's floor shot is taken in (where it is laid most as a floor, by the levels' own files). */
	FString ShotLevelOf(const FString& Stem)
	{
		static const TMap<FString, FString> Where = {
			{ TEXT("GFDRYTBL"), TEXT("hideout") }, { TEXT("HFHAYFLR"), TEXT("hideout") },
			{ TEXT("Y1FICE2"), TEXT("icecaves") }, { TEXT("Y1FICE3"), TEXT("icecaves") }, { TEXT("Y1CIRK2"), TEXT("icecaves") }, { TEXT("Y1FIUW"), TEXT("icecaves") },
			{ TEXT("FGRNTE01"), TEXT("civlwar1") }, { TEXT("FWHAYFLR"), TEXT("civlwar1") }, { TEXT("FGRNTE02"), TEXT("villa") },
			{ TEXT("GCGRNIT1"), TEXT("miner") }, { TEXT("GCGRNIT2"), TEXT("miner") },
			{ TEXT("DFSTON2"), TEXT("cliff") }, { TEXT("DFSTON3"), TEXT("cliff") }, { TEXT("DC3"), TEXT("cliff") }, { TEXT("MFEX1"), TEXT("mill") } };
		const FString* L = Where.Find(Stem.ToUpper());
		return L ? *L : FString();
	}
}

void AOutlawsLevelActor::StochFloorSurvey(const TArray<FString>& Stems, int32& Pass, int32& Fail, TMap<FString, FVector>& OutSpots)
{
	TMap<const UMaterialInterface*, FString> KeyOf;
	int32 FloorMats = 0;
	for (const TPair<FString, TObjectPtr<UMaterialInstanceDynamic>>& Kv : Materials)
	{
		if (!Kv.Value) continue;
		KeyOf.Add(Kv.Value.Get(), Kv.Key);
		FloorMats += Kv.Key.Contains(TEXT("~f|")) ? 1 : 0;
	}
	TSet<const UPrimitiveComponent*> Scrolls;
	for (const FScrollMesh& SM : ScrollMeshes) if (SM.Mesh) Scrolls.Add(SM.Mesh);
	struct FUse
	{
		int32 Floor = 0, Wall = 0, Ceil = 0;                 // triangles drawn
		int32 FloorOn = 0, FloorOff = 0, WallOn = 0, CeilOn = 0;
		int32 Scroll = 0, Swirl = 0, Atx = 0;                // floor triangles left out by the rule
		TSet<const UMaterialInterface*> Mats;
		double Best = 0.0;
		FVector Spot = FVector::ZeroVector;
	};
	TMap<FString, FUse> Use;
	int32 BadWall = 0, BadCeil = 0, BadOff = 0, BadLeftOut = 0, WorldSections = 0;
	TSet<FString> BadStems;
	TInlineComponentArray<UProceduralMeshComponent*> Meshes;
	GetComponents(Meshes);
	for (UProceduralMeshComponent* M : Meshes)
	{
		if (!M || !M->IsVisible() || M->bHiddenInGame) continue;
		const bool bScroll = Scrolls.Contains(M);
		for (int32 s = 0; s < M->GetNumSections(); ++s)
		{
			const FProcMeshSection* Sec = M->GetProcMeshSection(s);
			UMaterialInterface* Mat = M->GetMaterial(s);
			const FString* Key = Mat ? KeyOf.Find(Mat) : nullptr;
			if (!Sec || !Key || !Sec->bSectionVisible || Sec->ProcIndexBuffer.Num() == 0) continue;
			FString Head, Parent, Stem;
			if (!Key->Split(TEXT("|"), &Head, &Stem)) continue;
			if (!Head.Split(TEXT("~"), &Parent, nullptr)) Parent = Head;
			if (!(Parent == TEXT("M_OLR_World") || Parent == TEXT("M_OLR_WorldMasked") || Parent == TEXT("M_OLR_WorldSkyThru"))) continue;
			++WorldSections;
			float V = 0.f;
			Mat->GetScalarParameterValue(FName(TEXT("Stochastic")), V);
			const bool bOn = V > 0.5f;
			const FOutlawsTexInfo* TI = TexInfoCache.Find(Stem);
			const bool bAtx = TI && (TI->bAtxLoop || TI->bAtxStops);
			const bool bStem = IsStochasticFloorStem(Stem);
			FUse& U = Use.FindOrAdd(Stem.ToUpper());
			U.Mats.Add(Mat);
			const TArray<FProcMeshVertex>& Vb = Sec->ProcVertexBuffer;
			const TArray<uint32>& Ib = Sec->ProcIndexBuffer;
			for (int32 t = 0; t + 2 < Ib.Num(); t += 3)
			{
				const FProcMeshVertex& A = Vb[Ib[t]];
				const FProcMeshVertex& B = Vb[Ib[t + 1]];
				const FProcMeshVertex& C = Vb[Ib[t + 2]];
				const double Nz = (A.Normal.Z + B.Normal.Z + C.Normal.Z) / 3.0;
				if (Nz > 0.2)
				{
					++U.Floor;
					const bool bSwirl = A.Color.A < 200;
					const bool bWant = bStem && !bAtx && !bSwirl && !bScroll;
					U.Scroll += bStem && bScroll;
					U.Swirl += bStem && bSwirl;
					U.Atx += bStem && bAtx;
					if (bOn) ++U.FloorOn; else ++U.FloorOff;
					if (bWant && !bOn)
					{
						++BadOff;
						if (!BadStems.Contains(Stem)) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT stochfloor %s off floor: %s in %s section %d"), *LevelName, **Key, *M->GetName(), s);
						BadStems.Add(Stem);
					}
					if (!bWant && bOn) { ++BadLeftOut; BadStems.Add(Stem); }
					if (bWant)
					{
						const double Area = FVector::CrossProduct(FVector(B.Position - A.Position), FVector(C.Position - A.Position)).Size() * 0.5;
						if (Area > U.Best) { U.Best = Area; U.Spot = FVector(A.Position + B.Position + C.Position) / 3.0; }
					}
				}
				else if (Nz < -0.2) { ++U.Ceil; if (bOn) { ++U.CeilOn; ++BadCeil; BadStems.Add(Stem); } }
				else { ++U.Wall; if (bOn) { ++U.WallOn; ++BadWall; BadStems.Add(Stem); } }
			}
		}
	}
	auto Check = [&Pass, &Fail, this](bool bOk, const FString& What)
	{
		(bOk ? Pass : Fail)++;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT stochfloor %s check %s: %s"), *LevelName, *What, bOk ? TEXT("PASS") : TEXT("FAIL"));
	};
	for (const FString& St : Stems)
	{
		const FUse* U = Use.Find(St.ToUpper());
		if (!U) continue;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT stochfloor %s %s: floor %d wall %d ceil %d triangles; stochastic on floor %d off floor %d, on wall %d on ceil %d; floors left out: scroll %d swirl %d atx %d; %d materials"),
			*LevelName, *St, U->Floor, U->Wall, U->Ceil, U->FloorOn, U->FloorOff, U->WallOn, U->CeilOn, U->Scroll, U->Swirl, U->Atx, U->Mats.Num());
		if (U->Floor > 0)
		{
			// a listed stem laid as a floor here: every one of its own floors (not left out) takes it
			const int32 Want = U->Floor - FMath::Max(U->Scroll, FMath::Max(U->Swirl, U->Atx));
			Check(Want > 0 && U->FloorOn >= Want && !BadStems.Contains(St), FString::Printf(TEXT("%s floors on (%d of %d)"), *St, U->FloorOn, Want));
			if (U->Best > 0.0) OutSpots.Add(St, U->Spot);
		}
	}
	Check(BadWall == 0, FString::Printf(TEXT("walls off (%d wall triangles on)"), BadWall));
	Check(BadCeil == 0, FString::Printf(TEXT("ceilings off (%d ceiling triangles on)"), BadCeil));
	Check(BadOff == 0, FString::Printf(TEXT("every stochastic floor on (%d off)"), BadOff));
	Check(BadLeftOut == 0, FString::Printf(TEXT("other floors off, animated, swirling and scrolling ones too (%d on)"), BadLeftOut));
	TArray<FString> Bad = BadStems.Array();
	Bad.Sort();
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT stochfloor %s sections: %d world sections drawn, %d materials (%d floor-only), %d scroll meshes; stems against the rule: %s"),
		*LevelName, WorldSections, Materials.Num(), FloorMats, ScrollMeshes.Num(), Bad.Num() ? *FString::Join(Bad, TEXT(" ")) : TEXT("none"));
}

OL_SHOT_MATCH(OLStochFloorProbe) { return OLFlags::Param(TEXT("OLStochFloorProbe")); }

OL_SHOT_TEST(OLStochFloorProbe, 2588)
{
	// the state lives here, not on the player: a level change can give a new pawn
	static TArray<FString> Levels;
	static int32 At = -1, Step = 0, Pass = 0, Fail = 0, Frames = 0;
	static double Since = 0.0, GpuOn = 0.0, GpuOff = 0.0;
	static TArray<FString> ShotStems;
	static TMap<FString, FVector> Spots;
	static const bool bShots = OLFlags::Param(TEXT("OLStochFloorShots"));
	if (!L) return;
	const double Now = FPlatformTime::Seconds();
	if (At < 0)
	{
		FString List = TEXT("hideout,icecaves,civlwar1,villa,miner,cliff,mill");
		OLFlags::Value(TEXT("OLStochFloorLevels"), List);
		List.ParseIntoArray(Levels, TEXT(","));
		At = 0;
		Since = Now;
		if (Levels.Num() > 0 && !L->LevelName.Equals(Levels[0], ESearchCase::IgnoreCase)) L->JumpToLevel(Levels[0]);
		return;
	}
	if (At >= Levels.Num())
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT stochfloor RESULT %d PASS, %d FAIL"), Pass, Fail);
		FGenericPlatformMisc::RequestExit(false);
		At = 1 << 20;
		return;
	}
	auto Next = [&]()
	{
		++At;
		Step = 0;
		Since = Now;
		if (Levels.IsValidIndex(At)) L->JumpToLevel(Levels[At]);
	};
	auto Still = [L]()
	{
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
		{
			if (AOutlawsEnemy* E = Wk.Get()) { E->SetActorHiddenInGame(true); E->SetActorTickEnabled(false); }
		}
	};
	if (Step == 0)
	{
		if (!L->LevelName.Equals(Levels[At], ESearchCase::IgnoreCase) || Now - Since < 8.0) return;
		L->SetStochastic(true);
		Still();
		TArray<FString> Stems;
		for (const TCHAR* S : GStochFloorStems) Stems.Add(S);
		Spots.Reset();
		L->StochFloorSurvey(Stems, Pass, Fail, Spots);
		ShotStems.Reset();
		if (bShots) for (const TCHAR* S : GStochFloorStems) if (ShotLevelOf(S) == Levels[At].ToLower() && Spots.Contains(S)) ShotStems.Add(S);
		for (const TCHAR* S : GStochFloorStems)
		{
			if (bShots && ShotLevelOf(S) == Levels[At].ToLower() && !Spots.Contains(S))
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT stochfloor %s %s: no floor of it to look at"), *L->LevelName, S);
		}
		if (ShotStems.Num() == 0) { Next(); return; }
		Step = 1;
		Since = Now;
		return;
	}
	// the shots: one stem at a time, over its biggest floor triangle, looking down at it
	const FString Stem = ShotStems.IsValidIndex(0) ? ShotStems[0] : FString();
	if (Stem.IsEmpty()) { Next(); return; }
	if (Step == 1)
	{
		Still();
		TeleportTo(Spots[Stem] + FVector(0.0, 0.0, 92.0), GetActorRotation(), false, true);
		if (Controller) Controller->SetControlRotation(FRotator(-50.0, GetActorRotation().Yaw, 0.0));
		Step = 2;
		Since = Now;
	}
	else if (Step == 2 && Now - Since > 2.0)
	{
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("stochfloor_%s_%s.png"), *Stem, *L->LevelName.ToLower()), false, false);
		Step = 3;
		Since = Now;
		Frames = 0;
		GpuOn = GpuOff = 0.0;
	}
	else if (Step == 3 && Now - Since > 0.7)
	{
		// the GPU's frame time here, the option on (40 frames after 10), then off (the same), then on again
		++Frames;
		const double Ms = FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0));
		if (Frames > 10 && Frames <= 50) GpuOn += Ms;
		if (Frames == 50) L->SetStochastic(false);
		if (Frames > 60 && Frames <= 100) GpuOff += Ms;
		if (Frames == 100)
		{
			L->SetStochastic(true);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT stochfloor gpu %s %s: on %.3f ms, off %.3f ms (40 frames each)"), *L->LevelName, *Stem, GpuOn / 40.0, GpuOff / 40.0);
			ShotStems.RemoveAt(0);
			Step = ShotStems.Num() > 0 ? 1 : 0;
			if (Step == 0) Next();
		}
	}
}

#endif // WITH_OUTLAWS_TESTS
