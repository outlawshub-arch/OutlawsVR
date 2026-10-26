// REQ-PIANO: the town piano's checks (townex).
//   -OLShots -OLLevel=townex -OLNoFrontEnd -OLPianoTest: one player before the keys presses them as a hand push does: taken, the
//   tune starts (logged), the roll behind the doors scrolls while it plays; a press while it plays is refused, and one 1 s after
//   it ends (inside the 2 s lockout); a press after the lockout plays it again.  Prints "OLSHOT piano check PASS|FAIL: ..." and
//   "OLSHOT piano RESULT".  First (PianoOldSaveTick) an old townex save: the doors and lifts set off their start, saved to
//   slot 8, cut to the RC2/RC4 layout (no piano light) and loaded: each as saved, the light and the box at their start.
//   -OLPianoOldSave=N loads slot N instead (a save made by older code), every elevator logged ("OLSHOT oldsave loaded").
//   -OLCoopAuto -OLCoopTest=piano on a story co-op host (-OLCoopLevel=townex) and its client: the host presses, the client hears the
//   tune and sees the roll move; the client's press inside the lockout is refused by the host, its press after it plays the tune
//   again on both.  Prints "OLCOOP piano check PASS|FAIL (host|client): ..." and "OLCOOP piano <host|client> RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsFlags.h"
#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsGeometry.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "OutlawsShotTests.h"

#if WITH_OUTLAWS_TESTS

bool AOutlawsLevelActor::DebugPressPianoKeys()
{
	UWorld* W = GetWorld();
	if (!Inf || KeySectors.Num() == 0 || !W) return false;
	const FOutlawsSector& S = Inf->Live.Sectors[*KeySectors.CreateConstIterator()];
	FVector2D C = FVector2D::ZeroVector;
	for (const FVector2D& V : S.Verts) C += V;
	C /= FMath::Max(1, S.Verts.Num());
	const FVector Top = FOutlawsGeometry::ToUE(C.X, S.Floor.Y, C.Y);
	// the hand comes down onto them: the same short trace along its motion the push makes, from 4 cm over to 3 cm into them
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsPianoTest), true);
	FHitResult Hit;
	const bool bHit = W->LineTraceSingleByChannel(Hit, Top + FVector(0.0, 0.0, 4.0), Top - FVector(0.0, 0.0, 3.0), ECC_Visibility, Q);
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT piano: the hand comes down on the keys at %s: %s"), *Top.ToCompactString(),
		bHit ? *FString::Printf(TEXT("hit at z %.1f (normal z %.2f)"), Hit.ImpactPoint.Z, Hit.ImpactNormal.Z) : TEXT("no surface hit"));
	return PushByHand((bHit ? FVector(Hit.ImpactPoint) : Top) - FVector(0.0, 0.0, 1.0));
}

FVector2D AOutlawsLevelActor::DebugPianoRoll() const
{
	if (!Inf) return FVector2D::ZeroVector;
	for (const FOutlawsScroll& Sc : Inf->Scrolls) if (Sc.Light >= 0) return Sc.Offset;
	return FVector2D::ZeroVector;
}

FVector AOutlawsLevelActor::DebugPianoStand(double Along) const
{
	// the bug snap's spot (X 125.95 Z 111.87, sector 591, floor 0.25), Along units further down the keys' length
	return FOutlawsGeometry::ToUE(125.95, 0.25, 111.87 - Along) + FVector(0.0, 0.0, 92.0);
}

void AOutlawsLevelActor::DebugLogElevatorStates(const TCHAR* Tag) const
{
	if (!Inf) return;
	for (int32 i = 0; i < Inf->Elevators.Num(); ++i)
	{
		const FOutlawsElevator& E = Inf->Elevators[i];
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT oldsave %s e%d %s v %.3f stop %d target %d waiting %d term %d on %d speed %.4f"), Tag, i, *E.Name, E.Current, E.Stop, E.Target,
			E.bWaiting ? 1 : 0, E.bTerminated ? 1 : 0, E.bMasterOn ? 1 : 0, E.Speed);
	}
}

void AOutlawsLevelActor::DebugScrambleElevators()
{
	// every elevator a save from before the piano holds (the doors among them) put at a later stop and held there for 10
	// minutes, some with their master flipped or ended, so the save holds each off its start.  Not the piano's own (its
	// doors and the box: no older build could move them, and the piano's checks follow)
	if (!Inf) return;
	const int32 N = Inf->SavedElevatorsBefore > 0 ? Inf->SavedElevatorsBefore : Inf->Elevators.Num();
	for (int32 i = 0; i < N; ++i)
	{
		FOutlawsElevator& E = Inf->Elevators[i];
		if (E.Name.Contains(TEXT("PIANO")) || E.Name.Equals(TEXT("MUSIC_BOX"), ESearchCase::IgnoreCase) || TuneSeconds.Contains(i)) continue;
		const int32 Stops = Inf->Inf.Items[E.Item].Classes[E.ClassIndex].Stops.Num();
		if (Stops >= 2)
		{
			const int32 S = 1 + i % (Stops - 1);
			E.Stop = S;
			E.Target = -1;               // (arrived there: no move queued)
			E.Current = Inf->StopValue(E, S);
			E.bWaiting = true;
			E.WaitLeft = 600.0 + i;
		}
		if (i % 5 == 0) E.bMasterOn = !E.bMasterOn;
		if (i % 7 == 3) E.bTerminated = true;
	}
}

namespace
{
	// REQ-PIANO's old-save check: a townex save as RC2 and RC4 wrote it (the elevators up to the piano's light, none after)
	struct FOldSaveState
	{
		double V = 0.0;
		int32 Stop = 0, Target = 0;
		bool bWaiting = false, bTerm = false, bOn = false;
	};

	bool StripToOldLayout(const FString& Path, int32 Keep, int32& Had)
	{
		FString Text;
		TSharedPtr<FJsonObject> O;
		if (!FFileHelper::LoadFileToString(Text, *Path) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), O) || !O.IsValid()) return false;
		const TSharedPtr<FJsonObject>* In = nullptr;
		if (!O->TryGetObjectField(TEXT("inf"), In)) return false;
		TArray<TSharedPtr<FJsonValue>> Es = (*In)->GetArrayField(TEXT("elevators"));
		Had = Es.Num();
		if (Es.Num() < Keep) return false;
		Es.SetNum(Keep);
		(*In)->SetArrayField(TEXT("elevators"), Es);
		FString Out;
		return FJsonSerializer::Serialize(O.ToSharedRef(), TJsonWriterFactory<>::Create(&Out)) && FFileHelper::SaveStringToFile(Out, *Path);
	}
}

bool AOutlawsPlayer::PianoOldSaveTick(int32& OutFails, int32& OutPasses)
{
	// before the piano's own checks: doors and lifts set off their start, saved to slot 8, the save cut back to the old
	// layout (no piano light), loaded: every elevator as saved, the light and the box at their start, the box keeping its
	// tune speed.  -OLPianoOldSave=N loads slot N instead (a save written by RC2/RC4 code), every elevator logged for a
	// diff.  Slot 8's file is put back afterwards
	AOutlawsLevelActor* L = Level.Get();
	if (!L || !L->DebugInf()) return false;
	static int32 Phase = 0, Slot = 8;
	static double LoadAt = 0.0;
	static bool bSeenPending = false, bHadFile = false, bOwn = true;
	static FString Backup;
	static TArray<FOldSaveState> Saved;
	const double Now = FPlatformTime::Seconds();
	auto Check = [&OutFails, &OutPasses](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT piano check %s: old save: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++OutPasses; else ++OutFails;
	};
	if (Phase == 0)
	{
		if (ShotTime < 4.f) return false;
		int32 Given = -1;
		bOwn = !OLFlags::Value(TEXT("OLPianoOldSave"), Given);
		if (!bOwn)
		{
			Slot = Given;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT oldsave: loading slot %d (%s)"), Slot, *AOutlawsLevelActor::SavePath(Slot));
			Check(L->LoadGame(Slot), FString::Printf(TEXT("slot %d loads"), Slot));
		}
		else
		{
			const FString Path = AOutlawsLevelActor::SavePath(Slot);
			bHadFile = FFileHelper::LoadFileToString(Backup, *Path);
			L->DebugScrambleElevators();
			Saved.Reset();
			for (int32 i = 0; i < L->DebugInf()->SavedElevatorsBefore; ++i)
			{
				const FOutlawsElevator& E = L->DebugInf()->Elevators[i];
				FOldSaveState S;
				S.V = E.Current; S.Stop = E.Stop; S.Target = E.Target; S.bWaiting = E.bWaiting; S.bTerm = E.bTerminated; S.bOn = E.bMasterOn;
				Saved.Add(S);
			}
			L->DebugLogElevatorStates(TEXT("saved"));
			const bool bSaved = L->SaveGame(Slot);
			int32 Had = 0;
			const bool bStripped = bSaved && StripToOldLayout(Path, L->DebugInf()->SavedElevatorsBefore, Had);
			Check(bSaved && bStripped && Had == L->DebugInf()->Elevators.Num() && Had > L->DebugInf()->SavedElevatorsBefore,
				FString::Printf(TEXT("saved to slot %d and cut to the old layout (%d elevators -> %d)"), Slot, Had, L->DebugInf()->SavedElevatorsBefore));
			Check(L->LoadGame(Slot), TEXT("the old-layout save loads"));
		}
		LoadAt = Now;
		bSeenPending = false;
		Phase = 1;
		return false;
	}
	if (Phase == 1)
	{
		bSeenPending |= L->PendingLoad.IsValid();
		if (!(bSeenPending && !L->PendingLoad.IsValid() && Now > LoadAt + 3.0) && Now < LoadAt + 15.0) return false;
		L->DebugLogElevatorStates(TEXT("loaded"));
		const int32 Before = L->DebugInf()->SavedElevatorsBefore;
		if (bOwn)
		{
			int32 Bad = 0;
			FString First;
			for (int32 i = 0; i < Saved.Num() && i < L->DebugInf()->Elevators.Num(); ++i)
			{
				const FOutlawsElevator& E = L->DebugInf()->Elevators[i];
				const FOldSaveState& S = Saved[i];
				const bool bOk = FMath::Abs(E.Current - S.V) < 1e-3 && E.Stop == S.Stop && E.Target == S.Target && E.bWaiting == S.bWaiting && E.bTerminated == S.bTerm && E.bMasterOn == S.bOn;
				if (!bOk && Bad++ == 0) First = FString::Printf(TEXT(", first off: e%d %s v %.3f (saved %.3f) stop %d (saved %d)"), i, *E.Name, E.Current, S.V, E.Stop, S.Stop);
			}
			Check(Bad == 0 && Saved.Num() == Before && Before > 0, FString::Printf(TEXT("%d of %d elevators and doors as saved%s"), Saved.Num() - Bad, Saved.Num(), *First));
			// slot 8's file as it was
			const FString Path = AOutlawsLevelActor::SavePath(Slot);
			if (bHadFile) FFileHelper::SaveStringToFile(Backup, *Path); else IFileManager::Get().Delete(*Path);
		}
		for (int32 i = Before; i < L->DebugInf()->Elevators.Num(); ++i)
		{
			const FOutlawsElevator& E = L->DebugInf()->Elevators[i];
			Check(E.Stop == 0 && FMath::Abs(E.Current - L->DebugInf()->StopValue(E, 0)) < 1e-6, FString::Printf(TEXT("the piano's light e%d %s at its start (stop %d, %.3f)"), i, *E.Name, E.Stop, E.Current));
		}
		for (const TPair<int32, double>& Kv : L->TuneSpeeds)
		{
			const FOutlawsElevator& E = L->DebugInf()->Elevators[Kv.Key];
			Check(FMath::Abs(E.Speed - Kv.Value) < 1e-9, FString::Printf(TEXT("%s keeps the tune's speed %.4f (%.4f)"), *E.Name, Kv.Value, E.Speed));
			if (bOwn) Check(E.Stop == 0 && FMath::Abs(E.Current - L->DebugInf()->StopValue(E, 0)) < 1e-6, FString::Printf(TEXT("%s at its start"), *E.Name));
		}
		Phase = 2;
		ShotTime = 3.f;                  // (the piano's own checks from their start)
		return false;
	}
	return true;
}

void AOutlawsPlayer::PianoTestTick(float Dt)
{
	AOutlawsLevelActor* L = Level.Get();
	UWorld* W = GetWorld();
	if (!L || !W) return;
	static int32 Fails = 0, Passes = 0, Starts0 = 0, Refused0 = 0;
	static double T0 = 0.0, Still = 0.0;
	static FVector2D Roll = FVector2D::ZeroVector;
	if (!PianoOldSaveTick(Fails, Passes)) return;      // (first: an RC2/RC4 layout townex save loads as saved)
	const double Now = W->GetTimeSeconds();
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT piano check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	auto Report = [L](const TCHAR* When)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT piano %s: doors %s | box %s | roll %s"), When, *L->DebugElevatorReport(TEXT("LEFT_PIANO")),
			*L->DebugElevatorReport(TEXT("MUSIC_BOX")), *L->DebugPianoRoll().ToString());
	};
	switch (ShotStep)
	{
	case 0:
		if (ShotTime < 4.f) return;
		TeleportTo(L->DebugPianoStand(0.0), FRotator(0.0, 90.0, 0.0), false, true);
		if (Controller) Controller->SetControlRotation(FRotator(-25.0, 90.0, 0.0));
		Check(L->KeySectors.Num() == 1, FString::Printf(TEXT("the keys found (%d sectors)"), L->KeySectors.Num()));
		Check(L->TuneSeconds.Num() == 1, FString::Printf(TEXT("one tune (%d)"), L->TuneSeconds.Num()));
		++ShotStep;
		return;
	case 1:
		if (ShotTime < 5.f) return;
		Report(TEXT("before"));
		Starts0 = L->TuneStarts;
		Refused0 = L->TuneRefusals;
		Roll = L->DebugPianoRoll();
		Check(L->DebugPressPianoKeys(), TEXT("the press on the keys is taken"));
		T0 = Now;
		++ShotStep;
		return;
	case 2:
		if (Now < T0 + 1.0) return;
		Check(L->TuneStarts == Starts0 + 1, FString::Printf(TEXT("the tune started (%d)"), L->TuneStarts - Starts0));
		Report(TEXT("1 s in"));
		Roll = L->DebugPianoRoll();
		++ShotStep;
		return;
	case 3:
		if (Now < T0 + 2.0) return;
		{
			const FVector2D R = L->DebugPianoRoll();
			Check(!R.Equals(Roll, 1e-4), FString::Printf(TEXT("the roll scrolls while it plays (%s -> %s)"), *Roll.ToString(), *R.ToString()));
		}
		Check(!L->DebugPressPianoKeys(), TEXT("a press while it plays is refused"));
		Check(L->TuneRefusals == Refused0 + 1 && L->TuneStarts == Starts0 + 1, FString::Printf(TEXT("by the lockout (%d refused), no second tune"), L->TuneRefusals - Refused0));
		++ShotStep;
		return;
	case 4:
		// 1 s after the tune ended: the doors shut, the roll still
		if (Now < L->TuneBusyUntil - 1.0) return;
		Report(TEXT("the tune over"));
		Roll = L->DebugPianoRoll();
		Still = Now;
		++ShotStep;
		return;
	case 5:
		if (Now < Still + 0.4) return;
		Check(L->DebugPianoRoll().Equals(Roll, 1e-6), TEXT("the roll stands still once the doors shut"));
		Check(!L->DebugPressPianoKeys(), FString::Printf(TEXT("a press %.1f s after the tune ended is refused (the 2 s lockout)"), 2.0 - (L->TuneBusyUntil - Now)));
		++ShotStep;
		return;
	case 6:
		if (Now < L->TuneBusyUntil + 0.5) return;
		Check(L->DebugPressPianoKeys(), TEXT("a press after the lockout is taken"));
		T0 = Now;
		++ShotStep;
		return;
	case 7:
		if (Now < T0 + 1.0) return;
		Check(L->TuneStarts == Starts0 + 2, FString::Printf(TEXT("the tune plays again (%d tunes)"), L->TuneStarts - Starts0));
		Check(!L->DebugPianoRoll().Equals(Roll, 1e-6), TEXT("the roll scrolls again"));
		Report(TEXT("end"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT piano RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
		++ShotStep;
		return;
	default:
		return;
	}
}

OL_SHOT_MATCH(OLPianoTest) { return OLFlags::Param(TEXT("OLPianoTest")); }
OL_SHOT_TEST(OLPianoTest, 130)
{
	PianoTestTick(Dt);               // (REQ-PIANO: OutlawsPianoTest.cpp)
}

static double PianoLock(const AOutlawsLevelActor* L)
{
	// the lockout from the tune's start: its length and 2 s
	for (const TPair<int32, double>& Kv : L->TuneSeconds) return Kv.Value + 2.0;
	return 12.0;
}

void UOutlawsCoop::TickPianoTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	const bool bHost = IsHost();
	const TCHAR* Me = bHost ? TEXT("host") : TEXT("client");
	auto Check = [this, Me](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLCOOP piano check %s (%s): %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), Me, *What);
		if (bOk) ++PnPasses; else ++PnFails;
	};
	auto Result = [this, Me]()
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLCOOP piano %s RESULT %s (%d passed, %d failed)"), Me, PnFails == 0 && PnPasses > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), PnPasses, PnFails);
	};
	switch (PnStep)
	{
	case 0:
		if (!L->LevelName.Equals(TEXT("townex"), ESearchCase::IgnoreCase) || !L->HasActorBegunPlay()) return;
		if (bHost)
		{
			const AOutlawsCoopLink* CL = GetLink(PartnerIndex());
			if (!CL || CL->BuiltSerial != StartedSerial || !PartnerPuppet()) return;
		}
		if (PnAt == 0.0) { PnAt = Now + 4.0; return; }
		if (Now < PnAt) return;
		L->bInvincible = true;
		// both before the keys, a little apart along them
		P->TeleportTo(L->DebugPianoStand(bHost ? 0.0 : 2.5), FRotator(0.0, 90.0, 0.0), false, true);
		if (AController* PC = P->GetController()) PC->SetControlRotation(FRotator(-25.0, 90.0, 0.0));
		Check(L->KeySectors.Num() == 1 && L->TuneSeconds.Num() == 1, FString::Printf(TEXT("the keys (%d) and the tune (%d) found"), L->KeySectors.Num(), L->TuneSeconds.Num()));
		PnStarts0 = L->TuneStarts;
		PnRefused0 = L->TuneRefusals;
		PnAt = Now + (bHost ? 3.0 : 40.0);
		PnStep = bHost ? 1 : 20;
		return;

	// ---- the host ----
	case 1:
		if (Now < PnAt) return;
		Check(L->DebugPressPianoKeys(), TEXT("the host's press on the keys is taken"));
		PnAt = Now + 1.0;
		PnStep = 2;
		return;
	case 2:
		if (Now < PnAt) return;
		Check(L->TuneStarts == PnStarts0 + 1, TEXT("the tune started on the host"));
		PnAt = Now + 5.0;                       // (the client presses 2.5 s after it heard the tune start)
		PnStep = 3;
		return;
	case 3:
		if (Now < PnAt) return;
		Check(L->TuneRefusals >= PnRefused0 + 1 && L->TuneStarts == PnStarts0 + 1, FString::Printf(TEXT("the client's press inside the lockout refused by the host (%d refused, %d tunes)"),
			L->TuneRefusals - PnRefused0, L->TuneStarts - PnStarts0));
		PnAt = Now + 30.0;
		PnStep = 4;
		return;
	case 4:
		// the client's press after the lockout (its tune's end + 2.6 s)
		if (L->TuneStarts < PnStarts0 + 2 && Now < PnAt) return;
		Check(L->TuneStarts == PnStarts0 + 2, TEXT("the client's press after the lockout played the tune again (the host's word)"));
		Result();
		PnAt = Now + 8.0;
		PnStep = 5;
		return;
	case 5:
		if (Now < PnAt) return;
		TestSay(TEXT("quit"), 0);
		PnAt = Now + 2.0;
		PnStep = 6;
		return;
	case 6:
		if (Now < PnAt) return;
		Summary(TEXT("done"));
		QuitForTest();
		PnStep = 7;
		return;

	// ---- the client ----
	case 20:
		if (L->TuneStarts == PnStarts0)
		{
			if (Now < PnAt) return;
			Check(false, TEXT("the host's tune never reached the client"));
			Result();
			PnStep = 30;
			return;
		}
		Check(true, TEXT("the client hears the tune start (the host's word)"));
		PnTuneAt = Now;
		PnRoll = L->DebugPianoRoll();
		PnStep = 21;
		return;
	case 21:
		if (Now < PnTuneAt + 1.5) return;
		{
			const FVector2D R = L->DebugPianoRoll();
			Check(!R.Equals(PnRoll, 1e-4), FString::Printf(TEXT("the client sees the roll scroll (%s -> %s)"), *PnRoll.ToString(), *R.ToString()));
		}
		PnStep = 22;
		return;
	case 22:
		if (Now < PnTuneAt + 2.5) return;
		UE_LOG(LogOutlaws, Log, TEXT("OLCOOP piano: the client presses the keys inside the lockout (sent: %d)"), L->DebugPressPianoKeys() ? 1 : 0);
		PnStep = 23;
		return;
	case 23:
		if (Now < PnTuneAt + 4.5) return;
		Check(L->TuneStarts == PnStarts0 + 1, TEXT("no second tune from the press inside the lockout"));
		PnStep = 24;
		return;
	case 24:
		// (Now is the session's clock, not the world's: the lockout's end counted from when the tune was heard)
		if (Now < PnTuneAt + PianoLock(L) - 1.0) return;
		PnRoll = L->DebugPianoRoll();
		PnAt = Now + 0.4;
		PnStep = 25;
		return;
	case 25:
		if (Now < PnAt) return;
		Check(L->DebugPianoRoll().Equals(PnRoll, 1e-6), TEXT("the roll stands still on the client once the doors shut"));
		PnStep = 26;
		return;
	case 26:
		if (Now < PnTuneAt + PianoLock(L) + 0.6) return;
		UE_LOG(LogOutlaws, Log, TEXT("OLCOOP piano: the client presses the keys after the lockout (sent: %d)"), L->DebugPressPianoKeys() ? 1 : 0);
		PnAt = Now + 3.0;
		PnStep = 27;
		return;
	case 27:
		if (L->TuneStarts < PnStarts0 + 2 && Now < PnAt) return;
		Check(L->TuneStarts == PnStarts0 + 2, TEXT("the client's press after the lockout plays the tune again"));
		Result();
		PnStep = 30;
		return;
	default:
		return;                                 // (the client waits for the host's quit)
	}
}
#endif // WITH_OUTLAWS_TESTS
