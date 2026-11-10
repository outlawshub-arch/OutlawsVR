// REQ-BOTAMMO's probe (the user's play notes of 3 Nov: "bots aren't dropping ammo as they die. They should drop a random
// amount, kind of like how other players will... just make it a random number"):
//   -OLShots -OLMpHost=<map> -OLMpClass=dm -OLMpBots=7 -OLMpMinutes=10 -OLMpFrags=0 -OLBotAmmoProbe [-OLBotAmmoKills=20]
//   [-OLBotAmmoClient] [-OLOldBotDrop]
// The host (out of harm) kills a living bot every 0.4 s until 20 have died: each death leaves its drop round the body (within
// 80 cm of its feet across, a metre up or down), every count 1 to its item's carry max, and the counts not all the same.
// Then a human takes two ammunition drops: one with none of it carried (the count rises by the drop's), one a round under the
// carry max (it rises to the max, capped as pickups are).  -OLBotAmmoClient: it waits for a joiner first; each drop is logged
// "OLSHOT botammo drop id=<n> <ground>#<contents>" for the joiner's "OLCOOP ground item <n> ... made by the host" lines.
// -OLOldBotDrop: no drop at any death.  Lines "OLSHOT botammo check PASS|FAIL: ..." and "OLSHOT botammo RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsCoop.h"
#include "OutlawsMpHost.h"
#include "OutlawsEnemy.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsBotAmmoProbeLocal
{
	int32 Stage = 0, Passes = 0, Fails = 0, Kills = 0, Empty = 0, Away = 0, OutOfRange = 0, Drops = 0;
	double StartAt = 0.0, NextKillAt = 0.0, StageAt = 0.0, ClientAt = 0.0;
	TSet<int32> Counts;
	TArray<int32> AmmoDrops;          // (BotDropNotes indexes: a drop that gives one ammunition only)
	int32 TakeIdx = -1, TakeBefore = 0, TakeWant = 0, TakeCase = 0;
	FString TakeItem;
	TArray<int32> TakeNear;           // (the drops of the same ammunition lying within 3 m of the one taken)

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT botammo check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}
	bool Parse(const FString& Contents, TArray<TPair<FString, int32>>& Out)
	{
		Out.Reset();
		TArray<FString> Parts;
		Contents.ParseIntoArray(Parts, TEXT(","));
		for (const FString& P : Parts)
		{
			FString Item, Num;
			if (P.Split(TEXT("="), &Item, &Num)) Out.Emplace(Item.ToLower(), FCString::Atoi(*Num));
		}
		return Out.Num() > 0;
	}
	int32 GivesOfNote(const FString& Contents)
	{
		TArray<TPair<FString, int32>> G;
		return Parse(Contents, G) ? G[0].Value : 0;
	}
}

OL_SHOT_MATCH(OLBotAmmoProbe) { return OLFlags::Param(TEXT("OLBotAmmoProbe")); }

OL_SHOT_TEST(OLBotAmmoProbe, 2145)
{
	using namespace OutlawsBotAmmoProbeLocal;
	if (!L || Stage < 0) return;
	UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	L->StartSaveDelay = -1.f;           // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	L->bInvincible = true;
	UOutlawsCoop* C = UOutlawsCoop::Get(this);
	const bool bOld = OLFlags::Param(TEXT("OLOldBotDrop"));
	const bool bClient = OLFlags::Param(TEXT("OLBotAmmoClient"));
	int32 Want = 20;
	OLFlags::Value(TEXT("OLBotAmmoKills"), Want);
	const bool bUp = C && C->IsHost() && C->IsMatch() && C->bLevelBuilt && !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay();
	// an item's carry max, as a pickup caps it: what AddCount leaves of a huge count, the pocket put back after
	auto Max = [L](const FString& Item)
	{
		const int32 Had = L->GetCount(Item);
		L->AddCount(Item, 1000000);
		const int32 Mx = L->GetCount(Item);
		L->TakeCount(Item, Mx);
		if (Had > 0) L->AddCount(Item, Had);
		return Mx;
	};
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT botammo RESULT %s (%d passed, %d failed; %s, %d kills, %d drops%s%s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			*L->LevelName, Kills, Drops, bClient ? TEXT(", a joiner") : TEXT(""), bOld ? TEXT(", -OLOldBotDrop") : TEXT(""));
		Stage = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	if (StartAt == 0.0) StartAt = FPlatformTime::Seconds();
	if (Stage == 0)
	{
		if (!bUp || C->Bots.Num() == 0)
		{
			if (FPlatformTime::Seconds() - StartAt < 240.0) return;
			Check(false, TEXT("needs a launched host with bots: -OLMpHost=<map> -OLMpClass=dm -OLMpBots=7"));
			Finish();
			return;
		}
		if (bClient && C->HumanCount() < 2)
		{
			if (FPlatformTime::Seconds() - StartAt < 300.0) return;
			Check(false, TEXT("no joiner came in 300 s"));
			Finish();
			return;
		}
		if (bClient && ClientAt == 0.0) ClientAt = Now;
		if (bClient && Now - ClientAt < 8.0) return;          // (the joiner's level built and in the match)
		FOutlawsMatchFlow& F = OutlawsMpHost::Flow();
		if (F.GetState() == FOutlawsMatchFlow::EState::WarmUp) F.HostStart();
		C->BotDropNotes.Reset();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT botammo start: %d bots on %s, %d kills to make%s%s"), C->Bots.Num(), *L->LevelName, Want, bClient ? TEXT(", a joiner in") : TEXT(""),
			bOld ? TEXT(" (-OLOldBotDrop)") : TEXT(""));
		NextKillAt = Now + 3.0;
		StageAt = Now;
		Stage = 1;
		return;
	}
	if (Stage == 1)
	{
		if (Kills >= Want || Now - StageAt > 180.0)
		{
			Check(Kills >= Want, FString::Printf(TEXT("%d bots killed (wanted %d)"), Kills, Want));
			if (bOld)
			{
				Check(Drops == 0, FString::Printf(TEXT("(old rule) no drop at any of the %d deaths: %d made"), Kills, Drops));
				Finish();
				return;
			}
			Check(Empty == 0, FString::Printf(TEXT("each death left its drop: %d of %d left none"), Empty, Kills));
			Check(Away == 0, FString::Printf(TEXT("each drop lies at its body (80 cm across, a metre up or down): %d of %d away"), Away, Drops));
			Check(OutOfRange == 0, FString::Printf(TEXT("every count 1 to its carry max: %d out of range"), OutOfRange));
			Check(Counts.Num() >= 3, FString::Printf(TEXT("the counts not all the same: %d different counts over %d drops"), Counts.Num(), Drops));
			Stage = 2;
			StageAt = Now;
			return;
		}
		if (Now < NextKillAt) return;
		NextKillAt = Now + 0.4;
		for (UOutlawsCoop::FBot& B : C->Bots)
		{
			AOutlawsEnemy* E = B.Brain.Get();
			if (B.bDead || !E || E->IsDead()) continue;
			const FVector Body = E->GetActorLocation();
			const FVector Feet = Body - FVector(0.0, 0.0, E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
			const FString Kit = C->BotLine(B);
			const int32 Before = C->BotDropNotes.Num();
			C->BotHurt(B.Who, 100.f, Body, Body, 0);
			++Kills;
			TArray<FString> Line;
			const int32 Made = C->BotDropNotes.Num() - Before;
			if (Made == 0 && !bOld) ++Empty;
			for (int32 i = Before; i < C->BotDropNotes.Num(); ++i)
			{
				const UOutlawsCoop::FBotDropNote& N = C->BotDropNotes[i];
				++Drops;
				const AActor* A = N.Actor.Get();
				// (where it lies: its place across, and up or down from the feet)
				const FVector At = A ? A->GetActorLocation() : FVector(1e9);
				const double Dz = A ? At.Z - Feet.Z : 1e9;
				const bool bNear = A && FVector::Dist2D(At, Feet) < 80.0 && FMath::Abs(Dz) < 100.0;
				if (!bNear) ++Away;
				TArray<TPair<FString, int32>> Give;
				Parse(N.Contents, Give);
				for (const TPair<FString, int32>& Kv : Give)
				{
					const bool bGun = Kv.Key == TEXT("ipistol") || Kv.Key == TEXT("irifle") || Kv.Key == TEXT("isgun") || Kv.Key == TEXT("idbsgun") || Kv.Key == TEXT("isawgun");
					if (bGun) continue;
					if (Kv.Value < 1 || Kv.Value > Max(Kv.Key)) ++OutOfRange;
					Counts.Add(Kv.Value);
				}
				if (Give.Num() == 1 && (Give[0].Key == TEXT("ibullit") || Give[0].Key == TEXT("icart") || Give[0].Key == TEXT("ishells"))) AmmoDrops.Add(i);
				Line.Add(FString::Printf(TEXT("%s#%s %.0f cm off, %.0f up%s"), *N.Ground, *N.Contents, A ? FVector::Dist2D(At, Feet) : -1.0, Dz, bNear ? TEXT("") : TEXT(" AWAY")));
				if (bClient) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT botammo drop id=%d %s#%s"), N.Id, *N.Ground, *N.Contents);
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT botammo kill %d: %s (%s) at %s: %s"), Kills, *B.Name, *Kit, *Feet.ToCompactString(), Line.Num() ? *FString::Join(Line, TEXT("; ")) : TEXT("no drop"));
			break;
		}
		return;
	}
	if (Stage == 2)
	{
		// a human takes two ammunition drops: one with none carried, one a round under the max (capped)
		if (TakeIdx < 0)
		{
			if (TakeCase >= 2) { Finish(); return; }
			// (bots killed at one spawn point leave drops lying together: one with no other drop within 3 m first; else the
			// walk-over takes those beside it too, and the rise wanted is all of the same ammunition taken)
			auto GivesOf = [&](int32 Ni, FString& OutItem) { TArray<TPair<FString, int32>> G; if (!Parse(C->BotDropNotes[Ni].Contents, G)) return 0; OutItem = G[0].Key; return G[0].Value; };
			auto Neighbours = [&](int32 Ni)
			{
				int32 N = 0;
				const AActor* A = C->BotDropNotes[Ni].Actor.Get();
				for (const UOutlawsCoop::FBotDropNote& O : C->BotDropNotes)
				{
					const AActor* B2 = O.Actor.Get();
					if (A && B2 && B2 != A && !B2->IsHidden() && FVector::Dist(B2->GetActorLocation(), A->GetActorLocation()) < 300.0) ++N;
				}
				return N;
			};
			for (int32 Pass = 0; Pass < 2 && TakeIdx < 0; ++Pass)
			{
				for (int32 k = AmmoDrops.Num() - 1; k >= 0 && TakeIdx < 0; --k)
				{
					const UOutlawsCoop::FBotDropNote& N = C->BotDropNotes[AmmoDrops[k]];
					FString Item;
					const int32 Count = GivesOf(AmmoDrops[k], Item);
					if (!N.Actor.IsValid() || N.Actor->IsHidden() || Count <= 0) continue;
					if (TakeCase == 1 && Count < 2) continue;       // (the capped case wants a drop of 2 or more)
					if (Pass == 0 && Neighbours(AmmoDrops[k]) > 0) continue;
					TakeIdx = AmmoDrops[k];
					TakeItem = Item;
					AmmoDrops.RemoveAt(k);
				}
			}
			if (TakeIdx >= 0)
			{
				const int32 Mx = Max(TakeItem);
				L->TakeCount(TakeItem, L->GetCount(TakeItem));
				if (TakeCase == 1) L->AddCount(TakeItem, Mx - 1);
				TakeBefore = L->GetCount(TakeItem);
				TakeNear.Reset();
				const AActor* A = C->BotDropNotes[TakeIdx].Actor.Get();
				for (int32 i = 0; i < C->BotDropNotes.Num(); ++i)
				{
					const AActor* B2 = C->BotDropNotes[i].Actor.Get();
					FString Item;
					if (B2 && !B2->IsHidden() && FVector::Dist(B2->GetActorLocation(), A->GetActorLocation()) < 300.0 && GivesOf(i, Item) > 0 && Item == TakeItem) TakeNear.Add(i);
				}
			}
			if (TakeIdx < 0)
			{
				Check(false, FString::Printf(TEXT("a human takes a drop (%s): no ammunition drop left lying"), TakeCase == 0 ? TEXT("none carried") : TEXT("capped")));
				++TakeCase;
				return;
			}
			const AActor* A = C->BotDropNotes[TakeIdx].Actor.Get();
			const FVector At = L->DropRestAt(A);       // (REQ-DROPSCATTER: one still in the air, where it comes down)
			TeleportTo(At + FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 5.0), GetActorRotation(), false, true);
			StageAt = Now;
			return;
		}
		const UOutlawsCoop::FBotDropNote& N = C->BotDropNotes[TakeIdx];
		const bool bGone = !N.Actor.IsValid() || N.Actor->IsHidden();
		if (!bGone && Now - StageAt < 4.0) return;
		if (Now - StageAt < 1.0) return;                         // (any beside it taken in the same steps)
		int32 Sum = 0, Taken = 0;
		for (int32 i : TakeNear)
		{
			const AActor* B2 = C->BotDropNotes[i].Actor.Get();
			if (!B2 || B2->IsHidden()) { Sum += GivesOfNote(C->BotDropNotes[i].Contents); ++Taken; }
		}
		TakeWant = FMath::Min(Max(TakeItem), TakeBefore + Sum);
		const int32 After = L->GetCount(TakeItem);
		Check(bGone && After == TakeWant, FString::Printf(TEXT("a human takes %s#%s (%s; %d drop%s of it taken there, %d in all): %s %d -> %d (wanted %d)"), *N.Ground, *N.Contents,
			TakeCase == 0 ? TEXT("none carried") : TEXT("a round under the max, capped"), Taken, Taken == 1 ? TEXT("") : TEXT("s"), Sum, *TakeItem, TakeBefore, After, TakeWant));
		TeleportTo(GetActorLocation() + FVector(0.0, 0.0, 5.0), GetActorRotation(), false, true);
		TakeIdx = -1;
		++TakeCase;
		return;
	}
	Finish();
}

#endif // WITH_OUTLAWS_TESTS
