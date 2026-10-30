// BUG-LIGHTSAVE (the weekly gate 2026-10-30, fp3's M8: the hideout's ALIEN1, elevator 30, at 31 before the co-op save and
// at its first stop in the print after the load): -OLShots -OLLevel=hideout -OLNoFrontEnd -OLLightSaveProbe.  (a) the
// sector light (ALIEN1, else the first) caught at its second stop part way through its wait, saved to slot 7: the save holds
// it as it was; (b) slot 7 loaded: on the first frame after the load is put back the light stands at the saved stop, its
// value and the wait left as saved (the load's first long frames aside, under a second), its sectors' AMBIENT at its level, and its faces drawn at that
// level a moment later; (c) the same save as one written before REQ-SECTORLIGHT (its elevators cut at the first light) as
// slot 9, loaded: every elevator before the lights put back, the light at one of its own stops with its sectors at it.
// -OLOldSectorLight: no light made, nothing to look at.  Lines "OLSHOT lightsave check PASS|FAIL: ..." and
// "OLSHOT lightsave RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsGeometry.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLLightSaveProbe) { return OLFlags::Param(TEXT("OLLightSaveProbe")); }

OL_SHOT_TEST(OLLightSaveProbe, 1290)
{
	static int32 Passes = 0, Fails = 0, Light = -1, SavedStop = -1, Before = 0;
	static double SavedV = 0.0, SavedWait = 0.0;
	static float Wait = 0.f;
	static bool bSeenPending = false;
	static TArray<double> SavedBefore;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lightsave check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const FOutlawsInfRuntime* Inf = L->DebugInf();
	const bool bUp = Inf && !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView && ShotTime > Wait;
	auto Next = [&](float Secs) { Wait = ShotTime + Secs; ++ShotStep; bSeenPending = false; };
	auto State = [&]()
	{
		const FOutlawsElevator& E = Inf->Elevators[Light];
		return FString::Printf(TEXT("stop %d, value %.2f, target %d, waiting %d with %.3f s left, AMBIENT %d"), E.Stop, E.Current, E.Target, E.bWaiting ? 1 : 0, E.WaitLeft,
			Inf->Live.Sectors[E.Sectors[0]].Ambient);
	};
	// (after a LoadGame: the rebuild, then the save put back on a later tick; true on the first frame after that)
	auto Loaded = [&]()
	{
		bSeenPending |= L->PendingLoad.IsValid() || L->IsRebuildPending();
		return bSeenPending && !L->PendingLoad.IsValid() && bUp && Inf->Elevators.IsValidIndex(Light);
	};
	const FString Dir = FPaths::ProjectSavedDir() / TEXT("OutlawsSaves");
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 3.f || !bUp) return;
		if (Inf->LightsFrom < 0)
		{
			Check(OLFlags::Param(TEXT("OLOldSectorLight")), FString::Printf(TEXT("%s: no sector light made (-OLOldSectorLight: %d)"), *L->LevelName, OLFlags::Param(TEXT("OLOldSectorLight")) ? 1 : 0));
			ShotStep = 6;
			return;
		}
		Light = Inf->LightsFrom;
		for (int32 e = Inf->LightsFrom; e < Inf->Elevators.Num(); ++e) if (Inf->Elevators[e].Name.Equals(TEXT("ALIEN1"), ESearchCase::IgnoreCase)) Light = e;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lightsave start in %s: light %d (%s) of %d elevators, the lights from %d: %s"), *L->LevelName, Light, *Inf->Elevators[Light].Name,
			Inf->Elevators.Num(), Inf->LightsFrom, *State());
		{
			// stood over the light's master sector (its faces are drawn within 40 m of the eye; the save keeps the place)
			const FOutlawsSector& S = Inf->Live.Sectors[Inf->Elevators[Light].Sectors[0]];
			FVector2D C = FVector2D::ZeroVector;
			for (const FVector2D& V : S.Verts) C += V;
			C /= double(FMath::Max(1, S.Verts.Num()));
			L->bInvincible = true;
			TeleportTo(FOutlawsGeometry::ToUE(C.X, S.Floor.Y + 0.5, C.Y) + FVector(-300.0, 0.0, 120.0), FRotator::ZeroRotator, false, true);
		}
		Wait = ShotTime + 1.f;
		ShotStep = 1;
		return;
	}
	case 1:
	{
		// (a) at a stop other than its start, with more than a second of its wait left
		if (!bUp) return;
		const FOutlawsElevator& E = Inf->Elevators[Light];
		const FOutlawsInfClass& C = Inf->Inf.Items[E.Item].Classes[E.ClassIndex];
		if (E.Stop == FMath::Clamp(C.StartStop, 0, C.Stops.Num() - 1) || E.Target >= 0 || !E.bWaiting || E.WaitLeft < 1.0)
		{
			if (ShotTime > 40.f) { Check(false, FString::Printf(TEXT("(a) the light never stood at another stop in 40 s: %s"), *State())); ShotStep = 6; }
			return;
		}
		SavedStop = E.Stop;
		SavedV = E.Current;
		SavedWait = E.WaitLeft;
		Before = Inf->LightsFrom;
		SavedBefore.Reset();
		for (int32 e = 0; e < Before; ++e) SavedBefore.Add(Inf->Elevators[e].Current);
		const bool bSaved = L->SaveGame(7);
		FString Text;
		TSharedPtr<FJsonObject> O;
		const TArray<TSharedPtr<FJsonValue>>* Es = nullptr;
		const TSharedPtr<FJsonObject>* In = nullptr;
		bool bHeld = false;
		if (bSaved && FFileHelper::LoadFileToString(Text, *(Dir / TEXT("slot7.json"))) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), O) && O.IsValid() &&
			O->TryGetObjectField(TEXT("inf"), In) && (*In)->TryGetArrayField(TEXT("elevators"), Es) && Es->IsValidIndex(Light))
		{
			const TSharedPtr<FJsonObject> J = (*Es)[Light]->AsObject();
			bHeld = J.IsValid() && int32(J->GetNumberField(TEXT("stop"))) == SavedStop && FMath::Abs(J->GetNumberField(TEXT("v")) - SavedV) < 1e-6;
			// (c)'s save: the same with its elevators cut at the first light, as a save from before the lights
			TArray<TSharedPtr<FJsonValue>> Cut = *Es;
			Cut.SetNum(Before);
			TSharedPtr<FJsonObject> InCut = MakeShared<FJsonObject>(**In);
			InCut->SetArrayField(TEXT("elevators"), Cut);
			O->SetObjectField(TEXT("inf"), InCut);
			FString Out;
			if (!FJsonSerializer::Serialize(O.ToSharedRef(), TJsonWriterFactory<>::Create(&Out)) || !FFileHelper::SaveStringToFile(Out, *(Dir / TEXT("slot9.json")))) bHeld = false;
		}
		Check(bSaved && bHeld, FString::Printf(TEXT("(a) saved to slot 7 with the light at %s: the save holds stop %d value %.2f; slot 9 written with %d elevators"), *State(), SavedStop, SavedV, Before));
		Check(L->LoadGame(7), TEXT("(b) loading slot 7"));
		Next(0.f);
		return;
	}
	case 2:
	{
		if (!Loaded()) return;
		// (b) the first frame after the load: the light as saved (a frame or two of its wait gone)
		const FOutlawsElevator& E = Inf->Elevators[Light];
		const bool bOk = E.Stop == SavedStop && E.Target < 0 && FMath::Abs(E.Current - SavedV) < 1e-3 && E.bWaiting && E.WaitLeft <= SavedWait + 1e-6 && E.WaitLeft > SavedWait - 1.0 &&
			Inf->Live.Sectors[E.Sectors[0]].Ambient == FMath::FloorToInt(E.Current);
		Check(bOk, FString::Printf(TEXT("(b) slot 7 loaded, the first frame: the light at the saved stop %d, value %.2f, %.3f s of its wait left (saved %.3f): %s"), SavedStop, SavedV, E.WaitLeft, SavedWait, *State()));
		Next(0.3f);
		return;
	}
	case 3:
	{
		if (!bUp) return;
		const FOutlawsElevator& E = Inf->Elevators[Light];
		const AOutlawsLevelActor::FLightStats St = L->DebugLight(Light);
		const bool bSame = E.Stop == SavedStop;
		Check(!bSame || (St.Faces > 0 && FMath::Abs(St.FaceBright - St.FaceWant) <= 1.f / 255.f + 1e-4f && St.bParsedSame),
			FString::Printf(TEXT("(b) a moment later its faces drawn at its level: %d faces, brightness %.4f (wanted %.4f), the parsed level the same %d: %s"), St.Faces, St.FaceBright, St.FaceWant, St.bParsedSame ? 1 : 0, *State()));
		Check(L->LoadGame(9), FString::Printf(TEXT("(c) loading slot 9 (a save from before the lights: %d elevators)"), Before));
		Next(0.f);
		return;
	}
	case 4:
	{
		if (!Loaded()) return;
		int32 Back = 0;
		for (int32 e = 0; e < Before && e < SavedBefore.Num(); ++e) Back += FMath::Abs(Inf->Elevators[e].Current - SavedBefore[e]) < 1.0 ? 1 : 0;
		const FOutlawsElevator& E = Inf->Elevators[Light];
		const FOutlawsInfClass& C = Inf->Inf.Items[E.Item].Classes[E.ClassIndex];
		bool bAtStop = false;
		for (int32 k = 0; k < C.Stops.Num(); ++k) bAtStop |= FMath::Abs(E.Current - Inf->StopValue(E, k)) < 1e-3;
		Check(Back == Before && bAtStop && Inf->Live.Sectors[E.Sectors[0]].Ambient == FMath::FloorToInt(E.Current),
			FString::Printf(TEXT("(c) slot 9 loaded: %d of the %d elevators before the lights put back, the light at one of its stops with its sectors at it: %s"), Back, Before, *State()));
		Next(0.5f);
		return;
	}
	case 5:
		if (!bUp) return;
		++ShotStep;
		return;
	case 6:
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lightsave RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		Wait = ShotTime + 1.f;
		ShotStep = 7;
		return;
	case 7:
		if (ShotTime < Wait) return;
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
