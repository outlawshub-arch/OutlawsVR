// -OLShots -OLKnifePickupProbe: one of the ShotTest tests (OutlawsShotTests.h).
// BUG-KNIFEPICKUP (user 27 Oct: "I can't pick up knife pickups unless they're ones I dropped"): every level's placed knives
// (gknife, a 3D model) walked into or gripped, each level started as a historical mission (the -OLRando=... randomizer with
// it), the headset's body as -OLClimbProbe stands it.  A line each (picked by walking, picked by the grip, or REFUSED and
// why), a COUNT a level, then a RESULT.
//   -OLKnifePickupProbe[=townex+train+miner] (the story levels that place knives; =story: all nine)
//   -OLKnifeProbeOldSave=<file>: on the first level, that save (made by a build from before the fix) loaded first, one of its
//     sprite pickups taken out of it (as taken): the level's knives must be there once each, every sprite where its serial
//     says, the one taken gone
//   -OLKnifeProbeRoundTrip: on the first level with two knives or more, one taken, a save, a second taken, the save loaded:
//     the first gone, the second back, one knife carried

#include "ShotTestCommon.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

#if WITH_OUTLAWS_TESTS

void AOutlawsLevelActor::DebugPickups(TArray<FPickupDebug>& Out) const
{
	Out.Reset();
	for (const FPickup& P : Pickups)
	{
		AActor* A = P.Actor.Get();
		if (!A || P.bHeld) continue;
		FPickupDebug& D = Out.AddDefaulted_GetRef();
		D.Actor = A;
		D.Serial = P.Serial;
		D.Ground = P.Ground;
		D.Where = A->GetActorLocation();
	}
}

namespace
{
	struct FKnifeProbe
	{
		TArray<FString> Levels;
		int32 LevelI = 0;
		bool bArmed = false, bAsked = false, bDone = false, bOldDone = false, bTripDone = false, bWalking = false;
		FVector WalkAt = FVector::ZeroVector;
		FString LevelName;
		int32 Phase = 0, KnifeI = 0, Before = 0;
		float T = 0.f, At = 0.f;
		TArray<TWeakObjectPtr<AActor>> Knives;
		TWeakObjectPtr<AActor> Trip[2];
		TMap<int32, TPair<FString, FVector>> SavedSprites;      // (old save) serial -> ground, place
		int32 TakenSerial = -1;
		int32 Placed = 0, Made = 0, Walked = 0, Gripped = 0, Refused = 0, Checks = 0, ChecksFailed = 0;
		int32 TotPlaced = 0, TotPicked = 0, TotRefused = 0;
	};
	FKnifeProbe GKnife;

	const TCHAR* const KnifeStory = TEXT("hideout+townex+train+canyon+mill+simms+miner+cliff+ranch");

	void Knives(AOutlawsLevelActor* L, TArray<AOutlawsLevelActor::FPickupDebug>& Out)
	{
		TArray<AOutlawsLevelActor::FPickupDebug> All;
		L->DebugPickups(All);
		Out.Reset();
		for (const AOutlawsLevelActor::FPickupDebug& D : All) if (D.Ground == TEXT("gknife")) Out.Add(D);
	}

	void Check(FKnifeProbe& K, bool bOk, const FString& What)
	{
		++K.Checks;
		K.ChecksFailed += bOk ? 0 : 1;
		UE_LOG(LogOutlaws, Display, TEXT("OLKNIFEPROBE %s check: %s: %s"), *K.LevelName, *What, bOk ? TEXT("PASS") : TEXT("FAIL"));
	}
}

OL_SHOT_MATCH(OLKnifePickupProbe) { return OLFlags::Has(TEXT("OLKnifePickupProbe")); }

OL_SHOT_TEST(OLKnifePickupProbe, 1226)
{
	FKnifeProbe& K = GKnife;
	if (K.bDone || !L || L->IsRebuildPending() || L->IsMoviePlaying()) return;
	if (!K.bArmed)
	{
		K.bArmed = true;
		FString List = TEXT("townex+train+miner");
		OLFlags::Value(TEXT("OLKnifePickupProbe"), List);
		if (List.Equals(TEXT("story"), ESearchCase::IgnoreCase)) List = KnifeStory;
		List.ParseIntoArray(K.Levels, TEXT("+"));
		UE_LOG(LogOutlaws, Display, TEXT("OLKNIFEPROBE on: %s (%s)"), *List, OLFlags::Param(TEXT("OLOldKnifePickup")) ? TEXT("-OLOldKnifePickup") : TEXT("as built"));
	}
	if (!K.Levels.IsValidIndex(K.LevelI))
	{
		K.bDone = true;
		const bool bOk = K.TotRefused == 0 && K.TotPlaced > 0 && K.ChecksFailed == 0;
		UE_LOG(LogOutlaws, Display, TEXT("OLKNIFEPROBE RESULT %s (%d placed knives, %d picked, %d refused; %d checks, %d failed)"), bOk ? TEXT("PASS") : TEXT("FAIL"),
			K.TotPlaced, K.TotPicked, K.TotRefused, K.Checks, K.ChecksFailed);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
		return;
	}
	const FString Want = K.Levels[K.LevelI];
	if (!K.bAsked)
	{
		K.bAsked = true;
		UE_LOG(LogOutlaws, Display, TEXT("OLKNIFEPROBE starting %s"), *Want);
		L->StartMission(Want, EOutlawsDifficulty::Bad);
		K.LevelName.Reset();
		return;
	}
	if (L->IsFrontEnd() || !L->LevelName.Equals(Want, ESearchCase::IgnoreCase)) return;
	L->StartSaveDelay = -1.f;
	L->AutosaveWait = 0.f;
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	if (K.LevelName != L->LevelName)
	{
		K.LevelName = L->LevelName;
		K.T = 0.f;
		K.Phase = 0;
		K.bWalking = false;
		K.Placed = K.Made = K.Walked = K.Gripped = K.Refused = 0;
		// the headset's body (CollectPickupsAt's feet-only VR rule), as -OLClimbProbe has it
		bVR = true;
		Camera->SetRelativeLocation(FVector(0.0, 0.0, 170.0));
	}
	K.T += Dt;
	if (K.T < 3.f) return;
	auto Stand = [&](const FVector& Feet)
	{
		SetActorLocation(Feet + FVector(0.0, 0.0, Half + 2.0), false, nullptr, ETeleportType::TeleportPhysics);
		GetCharacterMovement()->Velocity = FVector::ZeroVector;
	};
	auto Present = [&](const AActor* A)
	{
		TArray<AOutlawsLevelActor::FPickupDebug> Ks;
		Knives(L, Ks);
		return Ks.ContainsByPredicate([A](const AOutlawsLevelActor::FPickupDebug& D) { return D.Actor == A; });
	};
	auto Snapshot = [&]()
	{
		TArray<AOutlawsLevelActor::FPickupDebug> Ks;
		Knives(L, Ks);
		K.Knives.Reset();
		for (const AOutlawsLevelActor::FPickupDebug& D : Ks) K.Knives.Add(D.Actor);
		K.KnifeI = 0;
		K.At = K.T;
	};
	switch (K.Phase)
	{
	case 0:
	{
		TArray<AOutlawsLevelActor::FPickupDebug> Ks;
		Knives(L, Ks);
		K.Placed = L->DebugPlacedGroundModels();
		K.Made = Ks.Num();
		FString Where;
		for (const AOutlawsLevelActor::FPickupDebug& D : Ks) Where += FString::Printf(TEXT(" [serial %d at %s]"), D.Serial, *D.Where.ToString());
		UE_LOG(LogOutlaws, Display, TEXT("OLKNIFEPROBE %s: %d placed knives (3D ground items), %d of them pickups (serials from %d)%s"), *K.LevelName, K.Placed, K.Made, L->DebugModelSerialsFrom(), *Where);
		FString OldSave;
		if (!K.bOldDone && OLFlags::Value(TEXT("OLKnifeProbeOldSave"), OldSave))
		{
			K.bOldDone = true;
			// the older save, one of its sprite pickups taken out (as taken), written to slot 9 and loaded
			FString Text;
			TSharedPtr<FJsonObject> O;
			if (FFileHelper::LoadFileToString(Text, *OldSave) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), O) && O.IsValid())
			{
				TArray<TSharedPtr<FJsonValue>> Ps = O->GetArrayField(TEXT("pickups"));
				K.SavedSprites.Reset();
				K.TakenSerial = -1;
				for (int32 i = Ps.Num() - 1; i >= 0; --i)
				{
					const TSharedPtr<FJsonObject> J = Ps[i]->AsObject();
					const int32 S = J.IsValid() ? int32(J->GetNumberField(TEXT("serial"))) : -1;
					if (S < 0) continue;
					if (K.TakenSerial < 0 && J->GetStringField(TEXT("ground")) != TEXT("gknife")) { K.TakenSerial = S; Ps.RemoveAt(i); continue; }
					const FVector P(J->GetNumberField(TEXT("x")), J->GetNumberField(TEXT("y")), J->GetNumberField(TEXT("z")));
					K.SavedSprites.Add(S, TPair<FString, FVector>(J->GetStringField(TEXT("ground")), P));
				}
				O->SetArrayField(TEXT("pickups"), Ps);
				Check(K, !O->HasField(TEXT("placed_serials")), FString::Printf(TEXT("the older save %s has no placed_serials (a save from before the fix); %d level pickups in it, serial %d taken out of it"),
					*FPaths::GetCleanFilename(OldSave), K.SavedSprites.Num(), K.TakenSerial));
				FString Out;
				FJsonSerializer::Serialize(O.ToSharedRef(), TJsonWriterFactory<>::Create(&Out));
				FFileHelper::SaveStringToFile(Out, *AOutlawsLevelActor::SavePath(9));
				L->LoadGame(9);
				K.Phase = 10;
				K.At = K.T;
				return;
			}
			Check(K, false, FString::Printf(TEXT("the older save %s read"), *OldSave));
		}
		if (!K.bTripDone && OLFlags::Param(TEXT("OLKnifeProbeRoundTrip")) && Ks.Num() >= 2)
		{
			K.bTripDone = true;
			K.Trip[0] = Ks[0].Actor;
			K.Trip[1] = Ks[1].Actor;
			K.Before = L->GetCount(TEXT("iknife"));
			Stand(Ks[0].Where);
			K.Phase = 20;
			K.At = K.T;
			return;
		}
		Snapshot();
		K.Phase = 1;
		return;
	}
	case 10:
	{
		// (the older save loaded: the level built again from it)
		if (K.T < K.At + 3.f) return;
		TArray<AOutlawsLevelActor::FPickupDebug> All, Ks;
		L->DebugPickups(All);
		Knives(L, Ks);
		int32 Right = 0, Wrong = 0, Dupes = 0;
		bool bTakenBack = false;
		for (const AOutlawsLevelActor::FPickupDebug& D : All)
		{
			if (D.Serial == K.TakenSerial) bTakenBack = true;
			if (D.Ground == TEXT("gknife") && D.Serial < 0) ++Dupes;
			if (D.Serial < 0 || D.Serial >= L->DebugModelSerialsFrom()) continue;
			const TPair<FString, FVector>* S = K.SavedSprites.Find(D.Serial);
			if (S && S->Key == D.Ground && (S->Value.IsZero() || FVector::Dist(S->Value, D.Where) < 5.0)) ++Right;
			else
			{
				++Wrong;
				UE_LOG(LogOutlaws, Display, TEXT("OLKNIFEPROBE %s: serial %d is %s at %s, the save says %s"), *K.LevelName, D.Serial, *D.Ground, *D.Where.ToString(), S ? *S->Key : TEXT("nothing"));
			}
		}
		K.Placed = L->DebugPlacedGroundModels();
		Check(K, Ks.Num() == K.Placed && K.Placed > 0 && Dupes == 0, FString::Printf(TEXT("after the older save: %d knives lying there of %d placed, %d of them dropped copies"), Ks.Num(), K.Placed, Dupes));
		Check(K, Wrong == 0 && Right == K.SavedSprites.Num(), FString::Printf(TEXT("after the older save: %d sprite pickups where their serials say, %d not, of %d in the save"), Right, Wrong, K.SavedSprites.Num()));
		Check(K, !bTakenBack, FString::Printf(TEXT("after the older save: the pickup taken out of it (serial %d) %s"), K.TakenSerial, bTakenBack ? TEXT("CAME BACK") : TEXT("stays gone")));
		K.Made = Ks.Num();
		Snapshot();
		K.Phase = 1;
		return;
	}
	case 20:
		// (round trip) the first knife walked over, then the save
		if (K.T < K.At + 0.8f) return;
		Check(K, !Present(K.Trip[0].Get()) && L->GetCount(TEXT("iknife")) == K.Before + 1, FString::Printf(TEXT("round trip: the first knife taken (iknife %d)"), L->GetCount(TEXT("iknife"))));
		Check(K, L->SaveGame(9), TEXT("round trip: saved to slot 9"));
		{
			TArray<AOutlawsLevelActor::FPickupDebug> Ks;
			Knives(L, Ks);
			for (const AOutlawsLevelActor::FPickupDebug& D : Ks) if (D.Actor == K.Trip[1].Get()) Stand(D.Where);
		}
		K.Phase = 21;
		K.At = K.T;
		return;
	case 21:
		if (K.T < K.At + 0.8f) return;
		Check(K, !Present(K.Trip[1].Get()) && L->GetCount(TEXT("iknife")) == K.Before + 2, FString::Printf(TEXT("round trip: the second knife taken after the save (iknife %d)"), L->GetCount(TEXT("iknife"))));
		L->LoadGame(9);
		K.Phase = 22;
		K.At = K.T;
		return;
	case 22:
	{
		if (K.T < K.At + 3.f) return;
		TArray<AOutlawsLevelActor::FPickupDebug> Ks;
		Knives(L, Ks);
		int32 Dupes = 0;
		for (const AOutlawsLevelActor::FPickupDebug& D : Ks) Dupes += D.Serial < 0 ? 1 : 0;
		Check(K, Ks.Num() == K.Made - 1 && Dupes == 0 && L->GetCount(TEXT("iknife")) == K.Before + 1,
			FString::Printf(TEXT("round trip: loaded: %d knives lying there (%d placed, the first taken before the save), %d dropped copies, iknife %d"), Ks.Num(), K.Made, Dupes, L->GetCount(TEXT("iknife"))));
		Snapshot();
		K.Phase = 1;
		return;
	}
	case 1:
	{
		// each knife in turn: even ones walked over (the level's own walk-over, next tick), odd ones taken by the right grip
		// and let go at the chest (TryHold, ReleaseHeld), at once
		if (K.T < K.At) return;
		if (K.bWalking)
		{
			// (walked onto it 0.8 s ago: taken, it is off the pickups and its look flew to the chest and went)
			K.bWalking = false;
			const AActor* A = K.Knives[K.KnifeI].Get();
			const int32 Now = L->GetCount(TEXT("iknife"));
			const bool bGot = (!A || !Present(A)) && Now == K.Before + 1;
			(bGot ? K.Walked : K.Refused) += 1;
			UE_LOG(LogOutlaws, Display, TEXT("OLKNIFEPROBE %s knife %d at %s: %s (iknife %d -> %d)"), *K.LevelName, K.KnifeI, *K.WalkAt.ToString(),
				bGot ? TEXT("picked by walking over it") : TEXT("REFUSED walking over it"), K.Before, Now);
			++K.KnifeI;
			K.At = K.T + 0.2f;
			return;
		}
		while (K.Knives.IsValidIndex(K.KnifeI) && !K.Knives[K.KnifeI].IsValid())
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLKNIFEPROBE %s knife %d: gone before its turn"), *K.LevelName, K.KnifeI);
			++K.Refused;
			++K.KnifeI;
		}
		if (!K.Knives.IsValidIndex(K.KnifeI))
		{
			const int32 NotMade = FMath::Max(0, K.Placed - K.Made);
			K.Refused += NotMade;
			if (NotMade > 0) UE_LOG(LogOutlaws, Display, TEXT("OLKNIFEPROBE %s: %d placed knives REFUSED: no pickup made for them (level scenery)"), *K.LevelName, NotMade);
			UE_LOG(LogOutlaws, Display, TEXT("OLKNIFEPROBE %s COUNT: %d placed, %d pickups, picked %d by walking and %d by the grip, refused %d"), *K.LevelName, K.Placed, K.Made, K.Walked, K.Gripped, K.Refused);
			K.TotPlaced += K.Placed;
			K.TotPicked += K.Walked + K.Gripped;
			K.TotRefused += K.Refused;
			++K.LevelI;
			K.bAsked = false;
			return;
		}
		AActor* A = K.Knives[K.KnifeI].Get();
		const FVector Where = A->GetActorLocation();
		const int32 Was = L->GetCount(TEXT("iknife"));
		if (K.KnifeI % 2 == 0)
		{
			Stand(Where);
			K.Before = Was;
			K.WalkAt = Where;
			K.bWalking = true;
			K.At = K.T + 0.8f;
			return;
		}
		else
		{
			Stand(Where);
			GripOf(1)->SetWorldLocation(Where + FVector(0.0, 0.0, 3.0));
			const bool bHeld = TryHold(1) && HeldItem[1].Get() == A;
			if (bHeld)
			{
				GripOf(1)->SetWorldLocation(ChestPoint());
				ReleaseHeld(1);
			}
			const bool bGot = !Present(A) && L->GetCount(TEXT("iknife")) == Was + 1;
			(bGot ? K.Gripped : K.Refused) += 1;
			UE_LOG(LogOutlaws, Display, TEXT("OLKNIFEPROBE %s knife %d at %s: %s (iknife %d -> %d)"), *K.LevelName, K.KnifeI, *Where.ToString(),
				bGot ? TEXT("picked by the grip") : bHeld ? TEXT("REFUSED at the chest") : TEXT("REFUSED: the grip found nothing"), Was, L->GetCount(TEXT("iknife")));
		}
		++K.KnifeI;
		K.At = K.T + 0.2f;
		return;
	}
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
