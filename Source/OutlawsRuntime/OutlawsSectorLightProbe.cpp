// REQ-SECTORLIGHT (user 2026-10-28, snapshot 17: shsimms at night, the light round the campfire did not flicker):
// -OLShots -OLLevel=<map> -OLNoFrontEnd -OLSectorLightProbe [-OLOldSectorLight].  (1) the level's CHANGE_LIGHT elevators, each
// with its sectors, faces, sprites and models; the shade summed over every mesh of the level (a level with no CHANGE_LIGHT
// draws the same either way: compare the line of a run with -OLOldSectorLight).  (2) on a level with lights, the one named
// START (shsimms: the campfire's, 13 then 8 and 9 from AMBIENT 4) else the first, watched for 4 s: each stop reached when the
// INF says (the stop's wait plus the move at its SPEED, within a frame), its sectors' AMBIENT, their faces' brightness, the
// sprites' light and the models' shade following it, and LightAt there; the time the light updates took a frame.  (3) shots
// at the snapshot's place, sectorlight_after_hi.png and _lo.png at its top and bottom level (-OLOldSectorLight:
// sectorlight_before.png) in Saved\OLShots.  Lines "OLSHOT sectorlight check PASS|FAIL: ..." and "OLSHOT sectorlight RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsGeometry.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "ProceduralMeshComponent.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLSectorLightProbe) { return OLFlags::Param(TEXT("OLSectorLightProbe")); }

OL_SHOT_TEST(OLSectorLightProbe, 21)
{
	static int32 Passes = 0, Fails = 0, Light = -1, LastStop = -1, Matched = 0, Timed = 0, Frames = 0, Followed = 0, Samples = 0;
	static double LastAt = 0.0, WatchUntil = 0.0, WatchFrom = 0.0, MaxDt = 0.0, Cost0 = 0.0, ModelCost0 = 0.0;
	static int32 Ticks0 = 0, Updates0 = 0, ShotsLeft = 0, Faces0 = 0, Models0 = 0, UpFrames0 = 0;
	static double UpCost0 = 0.0;
	static float Wait = 0.f;
	static FString Seq;
	if (!L || ShotStep < 0) return;
	const bool bOld = OLFlags::Param(TEXT("OLOldSectorLight"));
	const FOutlawsInfRuntime* Inf = L->DebugInf();
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT sectorlight check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	auto MasterAt = [Inf](int32 e)
	{
		const FOutlawsSector& S = Inf->Live.Sectors[Inf->Elevators[e].Sectors[0]];
		FVector2D C = FVector2D::ZeroVector;
		for (const FVector2D& V : S.Verts) C += V;
		C /= double(FMath::Max(1, S.Verts.Num()));
		return FOutlawsGeometry::ToUE(C.X, S.Floor.Y + 0.5, C.Y);
	};
	auto Look = [&]()
	{
		// the snapshot's place in shsimms (sector START, the campfire ahead), else over the light's master sector
		const bool bShsimms = L->LevelName.Equals(TEXT("shsimms"), ESearchCase::IgnoreCase);
		const FVector From = bShsimms ? FVector(3361.6, -3310.4, 120.0) : (Light >= 0 ? MasterAt(Light) + FVector(-300.0, 0.0, 120.0) : GetActorLocation());
		const FRotator Rot(-9.0, bShsimms ? 72.0 : 0.0, 0.0);
		TeleportTo(From, FRotator(0.0, Rot.Yaw, 0.0), false, true);
		if (Controller) Controller->SetControlRotation(Rot);
	};
	auto Shot = [&](const TCHAR* Tag)
	{
		const FString Name = FString::Printf(TEXT("sectorlight_%s"), Tag);
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		const int32 Lv = Inf && Inf->Elevators.IsValidIndex(Light) ? FMath::FloorToInt(Inf->Elevators[Light].Current) : -1;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT sectorlight shot %s.png, the light at %d"), *Name, Lv);
	};
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 4.f || !bUp || !Inf) return;
		L->bInvincible = true;
		// (1) the lights, and the shade of every mesh the level drew
		int32 Lights = 0, Faces = 0;
		for (int32 e = 0; e < Inf->Elevators.Num(); ++e)
		{
			const FOutlawsElevator& E = Inf->Elevators[e];
			if (!E.IsLight()) continue;
			++Lights;
			const AOutlawsLevelActor::FLightStats St = L->DebugLight(e);
			Faces += St.Faces;
			const FOutlawsInfClass& C = Inf->Inf.Items[E.Item].Classes[E.ClassIndex];
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT sectorlight (1) %s (elevator %d): sectors %s, AMBIENT %s, %d stops, speed %.0f, now %d; %d faces, %d sprites, %d models"),
				*E.Name, e, *FString::JoinBy(E.Sectors, TEXT(","), [](int32 s) { return FString::FromInt(s); }), *FString::JoinBy(E.BaseLight, TEXT(","), [](int32 a) { return FString::FromInt(a); }),
				C.Stops.Num(), E.Speed, FMath::FloorToInt(E.Current), St.Faces, St.Sprites, St.Models);
			if (Light < 0 && E.Name.Equals(TEXT("START"), ESearchCase::IgnoreCase) && E.BaseLight.Num() > 0) Light = e;
		}
		if (Light < 0) for (int32 e = 0; e < Inf->Elevators.Num() && Light < 0; ++e) if (Inf->Elevators[e].IsLight() && Inf->Elevators[e].BaseLight.Num() > 0) Light = e;
		double Shade = 0.0;
		int32 Verts = 0, Sections = 0;
		TInlineComponentArray<UProceduralMeshComponent*> Meshes(L);
		for (UProceduralMeshComponent* M : Meshes)
		{
			for (int32 s = 0; s < M->GetNumSections(); ++s)
			{
				const FProcMeshSection* Ps = M->GetProcMeshSection(s);
				if (!Ps) continue;
				++Sections;
				for (const FProcMeshVertex& V : Ps->ProcVertexBuffer) { Shade += V.Color.R; ++Verts; }
			}
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT sectorlight (1) %s: %d sector lights, %d of their faces' sections; shade sum %.0f over %d vertices in %d sections of %d meshes (%s)"),
			*L->LevelName, Lights, Faces, Shade, Verts, Sections, Meshes.Num(), bOld ? TEXT("-OLOldSectorLight") : TEXT("new"));
		if (bOld) Check(Inf->LightsFrom < 0 && Faces == 0, FString::Printf(TEXT("(1) -OLOldSectorLight: no sector light made (%d lights, the piano's gate aside), no faces of theirs"), Inf->LightsFrom < 0 ? 0 : Inf->Elevators.Num() - Inf->LightsFrom));
		else if (Light < 0) Check(Inf->LightsFrom < 0 && Faces == 0 && FOutlawsGeometry::SectorLight.Num() == 0, FString::Printf(TEXT("(1) %s has no CHANGE_LIGHT: none made, no sector kept apart"), *L->LevelName));
		else Check(Faces > 0, FString::Printf(TEXT("(1) %s: %d lights drawn by %d sections of their own"), *L->LevelName, Lights, Faces));
		if (bOld || Light < 0 || !Inf->Elevators[Light].BaseLight.Num())
		{
			Look();
			Wait = ShotTime + 2.f;
			ShotsLeft = 1;
			ShotStep = 2;
			return;
		}
		// (2) the light watched
		LastStop = Inf->Elevators[Light].Stop;
		LastAt = GetWorld()->GetTimeSeconds();      // (the game's clock: the one the INF runs on)
		{
			// watched for a loop and a fifth (4 to 12 s): the hideout's ALIEN1 waits 4 s a stop
			const FOutlawsElevator& E = Inf->Elevators[Light];
			const FOutlawsInfClass& C = Inf->Inf.Items[E.Item].Classes[E.ClassIndex];
			double Loop = 0.0;
			for (int32 k = 0; k < C.Stops.Num(); ++k)
			{
				Loop += C.Stops[k].Wait + (E.Speed > 0.0 ? FMath::Abs(C.Stops[(k + 1) % C.Stops.Num()].Value - C.Stops[k].Value) / E.Speed : 0.0);
			}
			WatchUntil = LastAt + 0.3 + FMath::Clamp(Loop * 1.2, 4.0, 12.0);
		}
		WatchFrom = LastAt + 0.3;
		LastAt = -1.0;
		Cost0 = L->DebugLightSeconds;
		Ticks0 = L->DebugLightTicks;
		Updates0 = L->DebugLightUpdates;
		Faces0 = L->DebugLightFaceUpdates;
		Models0 = L->DebugLightModelUpdates;
		ModelCost0 = L->DebugLightModelSeconds;
		L->DebugLightMaxFrame = 0.0;
		UpCost0 = L->DebugLightUpdateFrameSeconds;
		UpFrames0 = L->DebugLightUpdateFrames;
		Look();
		ShotStep = 1;
		return;
	}
	case 1:
	{
		const double Now = GetWorld()->GetTimeSeconds();
		if (Now < WatchFrom)
		{
			// (REQ-SECTORLIGHT cheaper) the eye moved: 0.3 s for the models now in view to be drawn there (a model's light is
			// sent while it is drawn), the cost counted from then
			Cost0 = L->DebugLightSeconds;
			Ticks0 = L->DebugLightTicks;
			Updates0 = L->DebugLightUpdates;
			Faces0 = L->DebugLightFaceUpdates;
			Models0 = L->DebugLightModelUpdates;
			ModelCost0 = L->DebugLightModelSeconds;
			L->DebugLightMaxFrame = 0.0;           // (BUG-ALIENLIGHT: the longest frame and the update frames from here)
			UpCost0 = L->DebugLightUpdateFrameSeconds;
			UpFrames0 = L->DebugLightUpdateFrames;
			LastStop = Inf->Elevators[Light].Stop;
			return;
		}
		const FOutlawsElevator& E = Inf->Elevators[Light];
		const FOutlawsInfClass& C = Inf->Inf.Items[E.Item].Classes[E.ClassIndex];
		MaxDt = FMath::Max(MaxDt, double(Dt));
		++Frames;
		// each frame: the master's AMBIENT, its faces, a sprite's light and a model's shade as the light's level
		const int32 Lv = FMath::FloorToInt(E.Current);
		const AOutlawsLevelActor::FLightStats St = L->DebugLight(Light);
		const int32 Amb = Inf->Live.Sectors[E.Sectors[0]].Ambient;
		bool bOk = Amb == Lv && FMath::Abs(St.FaceBright - St.FaceWant) <= 1.f / 255.f + 1e-4f && St.bParsedSame;
		if (St.SpriteLight >= 0) bOk &= St.SpriteLight == St.SpriteWant;
		if (St.ModelWant >= 0.f) bOk &= FMath::Abs(St.ModelShade - St.ModelWant) <= 2.f / 255.f;
		if (!bOk && Samples - Followed < 3)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT sectorlight (2) frame %d off: level %d, AMBIENT %d, face %.4f (wanted %.4f), parsed level the same %d, sprite %d (wanted %d), model %.4f (wanted %.4f)"),
				Samples, Lv, Amb, St.FaceBright, St.FaceWant, St.bParsedSame ? 1 : 0, St.SpriteLight, St.SpriteWant, St.ModelShade, St.ModelWant);
		}
		++Samples;
		Followed += bOk ? 1 : 0;
		if (E.Stop != LastStop && E.Target < 0 && LastAt < 0.0)
		{
			LastStop = E.Stop;           // (the first stop seen: the watch began part way through the wait before it)
			LastAt = Now;
		}
		else if (E.Stop != LastStop && E.Target < 0)
		{
			// a stop reached: the time since the last one against the INF (the last stop's wait, then the move at SPEED)
			const int32 From = LastStop, To = E.Stop;
			const double Move = E.Speed > 0.0 ? FMath::Abs(C.Stops[To].Value - C.Stops[From].Value) / E.Speed : 0.0;
			const double Expect = C.Stops[From].Wait + Move, Took = Now - LastAt;
			const bool bOnTime = FMath::Abs(Took - Expect) <= MaxDt * 2.0 + 0.004;
			++Timed;
			Matched += bOnTime ? 1 : 0;
			if (Seq.Len() < 700) Seq += FString::Printf(TEXT(" %d@%.3f(%.3f)"), FMath::RoundToInt(C.Stops[To].Value), Took, Expect);
			LastStop = To;
			LastAt = Now;
		}
		if (Now < WatchUntil) return;
		const double Ms = (L->DebugLightSeconds - Cost0) * 1000.0 / FMath::Max(1, L->DebugLightTicks - Ticks0);
		Check(Timed >= 1 && Matched >= FMath::Max(1, Timed - 1), FString::Printf(TEXT("(2) %s: %d of %d stops reached on the INF's time (wait + move at speed %.0f, within 2 frames, the longest frame %.1f ms); stop@seconds(expected):%s"),
			*E.Name, Matched, Timed, E.Speed, MaxDt * 1000.0, *Seq));
		const AOutlawsLevelActor::FLightStats End = L->DebugLight(Light);
		Check(Followed == Samples && Samples > 0, FString::Printf(TEXT("(2) %s: the sector's AMBIENT, its faces' brightness, the parsed level's AMBIENT (LightAt's), a sprite's light (%s) and a model's shade (%s) at the light's level in %d of %d frames"),
			*E.Name, End.SpriteLight >= 0 ? TEXT("one") : TEXT("none here"), End.ModelWant >= 0.f ? TEXT("one") : TEXT("none here"), Followed, Samples));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT sectorlight cost: %.4f ms a frame for every light of %s (%.2f ms a second; %d frames, %d section and sprite updates, %.4f ms each; %d faces, %d models taking %.4f ms a frame; %s)"),
			Ms, *L->LevelName, (L->DebugLightSeconds - Cost0) * 1000.0 / FMath::Max(0.001, Now - WatchFrom), L->DebugLightTicks - Ticks0, L->DebugLightUpdates - Updates0,
			(L->DebugLightSeconds - Cost0) * 1000.0 / FMath::Max(1, L->DebugLightUpdates - Updates0),
			L->DebugLightFaceUpdates - Faces0, L->DebugLightModelUpdates - Models0, (L->DebugLightModelSeconds - ModelCost0) * 1000.0 / FMath::Max(1, L->DebugLightTicks - Ticks0),
			OLFlags::Param(TEXT("OLOldLightRedraw")) ? TEXT("-OLOldLightRedraw") : TEXT("copies shown"));
		// (BUG-ALIENLIGHT) "each" above is every frame's time over the updates; what one change of light costs, and the worst frame
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT sectorlight frames: %d frames sent an update, %.4f ms each on average; the longest frame of the sector lights %.4f ms"),
			L->DebugLightUpdateFrames - UpFrames0, (L->DebugLightUpdateFrameSeconds - UpCost0) * 1000.0 / FMath::Max(1, L->DebugLightUpdateFrames - UpFrames0), L->DebugLightMaxFrame * 1000.0);
		ShotsLeft = 2;
		Wait = ShotTime + 0.5f;
		ShotStep = 2;
		return;
	}
	case 2:
	{
		if (ShotTime < Wait) return;
		if (Controller) Controller->SetControlRotation(FRotator(-9.0, L->LevelName.Equals(TEXT("shsimms"), ESearchCase::IgnoreCase) ? 72.0 : 0.0, 0.0));
		if (ShotsLeft == 1 && (bOld || Light < 0))
		{
			Shot(bOld ? TEXT("before") : TEXT("nolights"));
			ShotsLeft = 0;
		}
		else if (Light >= 0)
		{
			// the light's top level, then its lowest (the first two stops' values bound a flicker)
			const FOutlawsElevator& E = Inf->Elevators[Light];
			const FOutlawsInfClass& C = Inf->Inf.Items[E.Item].Classes[E.ClassIndex];
			double Hi = -1e9, Lo = 1e9;
			for (const FOutlawsInfStop& S : C.Stops) { Hi = FMath::Max(Hi, S.Value); Lo = FMath::Min(Lo, S.Value); }
			const int32 Lv = FMath::FloorToInt(E.Current);
			if (ShotsLeft == 2 && Lv == FMath::FloorToInt(Hi)) { Shot(TEXT("after_hi")); ShotsLeft = 1; }
			else if (ShotsLeft == 1 && Lv == FMath::FloorToInt(Lo)) { Shot(TEXT("after_lo")); ShotsLeft = 0; }
			else if (ShotTime > Wait + 12.f) { UE_LOG(LogOutlaws, Display, TEXT("OLSHOT sectorlight shot: the light never reached its %s"), ShotsLeft == 2 ? TEXT("top") : TEXT("bottom")); ShotsLeft = 0; }
		}
		else ShotsLeft = 0;
		if (ShotsLeft > 0) return;
		Wait = ShotTime + 1.5f;
		ShotStep = 3;
		return;
	}
	case 3:
		if (ShotTime < Wait) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT sectorlight RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
