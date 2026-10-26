// -OLShots -OLVisSpots: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLVisSpots)
{
	FString VisSpots;
	return OLFlags::Value(TEXT("OLVisSpots"), VisSpots, false);
}

OL_SHOT_TEST(OLVisSpots, 1030)
{
	FString VisSpots;
	OLFlags::Value(TEXT("OLVisSpots"), VisSpots, false);
	// (V1, -OLVisTest) each listed sector in turn: stood on its lowest corner, 1.5 s, then the body's tracked sector, how
	// many sectors are drawn, and for each watched sector (-OLVisWatch=) whether it is drawn and how many of its own
	// sprites show; then (-OLVisWalk=seconds) a walk straight ahead from the last spot for the churn log
	static TArray<int32> Spots, Watch;
	static int32 SpotI = -1;
	static float SpotAt = 0.f, WalkLeft = -1.f;
	static bool bVisWalking = false;
	if (SpotI < 0)
	{
		TArray<FString> P;
		VisSpots.ParseIntoArray(P, TEXT(","));
		for (const FString& S : P) Spots.Add(FCString::Atoi(*S));
		FString W;
		if (OLFlags::Value(TEXT("OLVisWatch"), W, false)) { P.Reset(); W.ParseIntoArray(P, TEXT(",")); for (const FString& S : P) Watch.Add(FCString::Atoi(*S)); }
		float Walk = 0.f;
		OLFlags::Value(TEXT("OLVisWalk"), Walk);
		WalkLeft = Walk;
		SpotI = 0;
		SpotAt = 3.f;
	}
	if (!L || ShotTime < SpotAt) { if (bVisWalking) AddMovementInput(GetActorForwardVector(), 1.f); return; }
	const int32 Phase = SpotI;
	if (Phase < Spots.Num() * 2)
	{
		const int32 Si = Spots[Phase / 2];
		if (Phase % 2 == 0)
		{
			FVector Feet;
			float SlopeYaw = 0.f;
			if (L->SlopeFoot(Si, Feet, SlopeYaw)) TeleportTo(Feet + FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0), GetActorRotation(), false, true);
			SpotAt = ShotTime + 1.5f;
		}
		else
		{
			FString W;
			for (const int32 Ws : Watch)
			{
				int32 Shown = 0, All = 0;
				for (TActorIterator<AOutlawsSpriteActor> It(GetWorld()); It; ++It)
				{
					if (It->DebugCullSector() != Ws) continue;
					++All;
					if (!It->DebugCulled()) ++Shown;
				}
				W += FString::Printf(TEXT(" s%d=%s(%d/%d sprites)"), Ws, L->DebugSectorDrawn(Ws) ? TEXT("drawn") : TEXT("hidden"), Shown, All);
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT vis spot %d: player in %d, %d sectors drawn;%s"), Si, L->PlayerSectorIndex(), L->DebugSectorsDrawn(), *W);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT draw census at spot %d: %s"), Si, *OutlawsPerf::DrawCensus(GetWorld()));     // (CPU1)
			// every bad guy drawn from here: his sector, and whether a wall stands between him and the eye (drawn yet
			// walled off, from a sector the flood reached by another way, or a real see-through: the log shows which)
			int32 Drawn = 0, Walled = 0;
			FString List;
			const FVector EyeP = Camera->GetComponentLocation();
			for (TActorIterator<AOutlawsEnemy> It(GetWorld()); It; ++It)
			{
				const AOutlawsSpriteActor* Spr = It->GetSprite();
				if (!Spr || Spr->DebugCulled() || It->IsDead()) continue;
				++Drawn;
				FHitResult Hb;
				FCollisionQueryParams Qv(SCENE_QUERY_STAT(OutlawsVisSee), false, this);
				Qv.AddIgnoredActor(*It);
				Qv.AddIgnoredActor(Spr);
				const bool bWall = GetWorld()->LineTraceSingleByChannel(Hb, EyeP, It->GetActorLocation() + FVector(0.0, 0.0, 40.0), ECC_Visibility, Qv);
				if (bWall) { ++Walled; if (Walled <= 8) List += FString::Printf(TEXT(" s%d@%.0fm"), Spr->DebugCullSector(), FVector::Dist(EyeP, It->GetActorLocation()) / 100.0); }
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT vis spot %d bad guys: %d drawn, %d of them walled off from the eye:%s"), Si, Drawn, Walled, *List);
			FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("ol_vis_%d.png"), Si), false, false);
			SpotAt = ShotTime + 0.5f;
		}
		++SpotI;
		return;
	}
	{
		// (CPU1, -OLCensusAt=X,Y,Yaw_X,Y,Yaw...) the draw census where a headset log saw the frame rate dip: stood on every
		// floor under each point (a carriage's roof and its inside), looking along Yaw, a census line and a screenshot each
		static TArray<FVector4> CensusAt;
		static int32 CensusI = -1;
		if (CensusI < 0)
		{
			CensusI = 0;
			FString Ca;
			if (OLFlags::Value(TEXT("OLCensusAt"), Ca, false))
			{
				TArray<FString> Pts;
				Ca.ParseIntoArray(Pts, TEXT("_"));
				FCollisionQueryParams Qc(SCENE_QUERY_STAT(OutlawsCensusFloor), false, this);
				for (const FString& Pt : Pts)
				{
					TArray<FString> V;
					Pt.ParseIntoArray(V, TEXT(","));
					if (V.Num() < 3) continue;
					const double X = FCString::Atod(*V[0]), Y = FCString::Atod(*V[1]), Yaw = FCString::Atod(*V[2]);
					FVector Top(X, Y, 100000.0);
					for (int32 k = 0; k < 8; ++k)
					{
						FHitResult H;
						if (!GetWorld()->LineTraceSingleByChannel(H, Top, FVector(X, Y, -100000.0), ECC_Visibility, Qc)) break;
						if (H.ImpactNormal.Z > 0.7) CensusAt.Add(FVector4(X, Y, H.ImpactPoint.Z, Yaw));
						Top = H.ImpactPoint - FVector(0.0, 0.0, 5.0);
					}
				}
			}
		}
		if (CensusI < CensusAt.Num() * 2)
		{
			const FVector4 C = CensusAt[CensusI / 2];
			if (CensusI % 2 == 0)
			{
				TeleportTo(FVector(C.X, C.Y, C.Z + GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0), FRotator(0.0, C.W, 0.0), false, true);
				if (Controller) Controller->SetControlRotation(FRotator(0.0, C.W, 0.0));
				SpotAt = ShotTime + 1.5f;
			}
			else
			{
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT census at %.0f,%.0f,%.0f yaw %.0f (shot %d; player in %d, %d sectors drawn): %s"), C.X, C.Y, C.Z, C.W, CensusI / 2,
					L->PlayerSectorIndex(), L->DebugSectorsDrawn(), *OutlawsPerf::DrawCensus(GetWorld()));
				FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("ol_census_%d.png"), CensusI / 2), false, false);
				SpotAt = ShotTime + 0.5f;
			}
			++CensusI;
			return;
		}
	}
	if (WalkLeft > 0.f)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT vis walk: %.0f s straight ahead from player sector %d"), WalkLeft, L->PlayerSectorIndex());
		SpotAt = ShotTime + WalkLeft;
		WalkLeft = 0.f;
		bVisWalking = true;
		return;
	}
	bVisWalking = false;
	{
		// (BUG-POPIN, -OLVisJitter=seconds) stood still at the last spot, the eye swaying a few cm as a head in a headset
		// does: how many sectors' may-be-seen flags flip meanwhile (none should, once the view has settled)
		static float JitterLeft = -2.f;
		static int32 FlipsAt = 0;
		static FVector EyeBase = FVector::ZeroVector;
		float Jitter = 0.f;
		if (JitterLeft == -2.f) JitterLeft = OLFlags::Value(TEXT("OLVisJitter"), Jitter) ? Jitter : -1.f;
		if (JitterLeft > 0.f)
		{
			if (EyeBase.IsZero())
			{
				// (-OLVisJitterAt=X,Y,Z: stood where a headset log saw the churn instead of at the last spot's corner)
				FString At;
				if (OLFlags::Value(TEXT("OLVisJitterAt"), At, false))
				{
					TArray<FString> P;
					At.ParseIntoArray(P, TEXT(","));
					if (P.Num() == 3) TeleportTo(FVector(FCString::Atod(*P[0]), FCString::Atod(*P[1]), FCString::Atod(*P[2])), GetActorRotation(), false, true);
				}
				EyeBase = Camera->GetRelativeLocation();
				FlipsAt = L->DebugVisFlips();
				SpotAt = ShotTime + 2.f;
				return;
			}
			if (ShotTime < SpotAt) return;
			const double T = ShotTime;
			float Cm = 15.f;
			OLFlags::Value(TEXT("OLVisJitterCm"), Cm);
			// (a head looking round: the eye sways the given cm each way, and the view turns 70 degrees either side)
			Camera->SetRelativeLocation(EyeBase + FVector(Cm * FMath::Sin(T * 1.7), Cm * FMath::Sin(T * 1.3), 0.3 * Cm * FMath::Sin(T * 2.9)));
			if (Controller) Controller->SetControlRotation(FRotator(0.0, 70.0 * FMath::Sin(T * 0.6), 0.0));
			// (-OLVisJitterWalk: walking as the head sways, the view's way, round the spot: the user saw bad guys flicker
			// "as I walk around sectors")
			if (OLFlags::Param(TEXT("OLVisJitterWalk"))) AddMovementInput(FRotator(0.0, 70.0 * FMath::Sin(T * 0.6) + 25.0 * T, 0.0).Vector(), 0.6f);
			// (what the user sees as flicker) a living bad guy the culling hides while the eye has a clear line to him in
			// front of it: counted per frame, and the bad guys it happened to
			static int32 FalseHidden = 0, Frames = 0;
			static TSet<FString> Who;
			{
				++Frames;
				const FVector EyeP = Camera->GetComponentLocation();
				const FQuat EyeQ = Camera->GetComponentQuat();
				const FOutlawsViewCone Cone = FOutlawsViewCone::Get();
				for (TActorIterator<AOutlawsEnemy> It(GetWorld()); It; ++It)
				{
					const AOutlawsSpriteActor* Spr = It->GetSprite();
					if (!Spr || !Spr->DebugCulled() || It->IsDead()) continue;
					const FVector To = It->GetActorLocation() + FVector(0.0, 0.0, 40.0);
					if (!Cone.Contains(EyeP, EyeQ, To) || FVector::Dist(EyeP, To) > 6000.0) continue;
					FHitResult Hb;
					FCollisionQueryParams Qv(SCENE_QUERY_STAT(OutlawsVisFalse), false, this);
					Qv.AddIgnoredActor(*It);
					Qv.AddIgnoredActor(Spr);
					if (GetWorld()->LineTraceSingleByChannel(Hb, EyeP, To, ECC_Visibility, Qv)) continue;
					++FalseHidden;
					Who.Add(FString::Printf(TEXT("%s@s%d"), *It->GetOutlawsName(), Spr->DebugCullSector()));
				}
			}
			if (ShotTime - SpotAt < JitterLeft) return;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT vis jitter: %.0f s of a still player's head sway in sector %d: %d sector flips; a bad guy in clear view culled %d times over %d frames (%s)"),
				JitterLeft, L->PlayerSectorIndex(), L->DebugVisFlips() - FlipsAt, FalseHidden, Frames, *FString::Join(Who.Array(), TEXT(" ")));
			Camera->SetRelativeLocation(EyeBase);
			JitterLeft = -1.f;
		}
	}
	{
		// (CPU1 object LOD, -OLLodSweep=Frames) stood still where the spots ended, every LOD distance swept from 3x to
		// 0.15x of its own over that many frames (every object through near, sprite and gone, faster than any walk), a
		// screenshot every frame (ol_lod_<n>.png: the frame-to-frame diffs show any pop) and the largest one-frame change
		// of any model's fade or sprite's alpha.  Without LOD (-OLNoObjectLod) the same frames: the still scene's noise
		static int32 SweepN = -2, SweepI = 0;
		if (SweepN == -2 && !OLFlags::Value(TEXT("OLLodSweep"), SweepN)) SweepN = -1;
		if (SweepN > 0 && SweepI <= SweepN + 30)
		{
			IConsoleVariable* Ls = IConsoleManager::Get().FindConsoleVariable(TEXT("ol.ObjectLodScale"));
			// (the world held still meanwhile: the train's ground, the bad guys, the animations; the frames differ by the LOD only)
			if (SweepI == 0)
			{
				AOutlawsSpriteActor::LodMaxStep = 0.f;
				if (AWorldSettings* Ws = GetWorld()->GetWorldSettings()) Ws->SetTimeDilation(0.0001f);
				// (-OLLodSweepAt=X,Y,FloorZ,Yaw: stood there for it, as a census point is)
				FString At;
				TArray<FString> P;
				if (OLFlags::Value(TEXT("OLLodSweepAt"), At, false) && At.ParseIntoArray(P, TEXT(",")) == 4)
				{
					const FRotator R(0.0, FCString::Atod(*P[3]), 0.0);
					TeleportTo(FVector(FCString::Atod(*P[0]), FCString::Atod(*P[1]), FCString::Atod(*P[2]) + GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0), R, false, true);
					if (Controller) Controller->SetControlRotation(R);
				}
			}
			if (SweepI < 30)
			{
				if (Ls) Ls->Set(3.f, ECVF_SetByCode);         // (settled at the start for a second)
				if (SweepI == 29) AOutlawsSpriteActor::LodMaxStep = 0.f;
			}
			else
			{
				const int32 k = SweepI - 30;
				if (Ls) Ls->Set(3.f * FMath::Pow(0.05f, float(k) / float(SweepN)), ECVF_SetByCode);
				FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("ol_lod_%03d.png"), k), false, false);
			}
			if (SweepI == SweepN + 30)
			{
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lod sweep: %d frames, largest one-frame step of a model's fade or a sprite's alpha %.3f; now %d as sprites, %d faded out"),
					SweepN, AOutlawsSpriteActor::LodMaxStep, AOutlawsSpriteActor::NumLodSprites, AOutlawsSpriteActor::NumLodGone);
				if (Ls) Ls->Set(1.f, ECVF_SetByCode);
				if (AWorldSettings* Ws = GetWorld()->GetWorldSettings()) Ws->SetTimeDilation(1.f);
			}
			++SweepI;
			return;
		}
	}
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT vis test done (player in %d)"), L->PlayerSectorIndex());
	FGenericPlatformMisc::RequestExit(false);
	ShotTime = -1.f;
	SpotI = 1 << 20;
}

#endif // WITH_OUTLAWS_TESTS
