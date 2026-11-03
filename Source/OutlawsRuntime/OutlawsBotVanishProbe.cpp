// BUG-BOTVANISH's probe (user 3 Nov, on the Quest: "a bot vanishes into thin air mid-fight" while it is shot or shoots, no
// death, no frag; many on Orleans, none seen on Sanctuary):
//   -OLShots -OLMpHost=<map> -OLMpClass=dm -OLMpBots=7 -OLMpBotSkill=0 -OLMpMinutes=10 -OLMpFrags=0 -OLBotVanishProbe
//   [-OLBotVanishSeconds=150] [-OLDeviceProfile=Quest2] [-OLOldBotVanish]
// The host (no harm comes to it) keeps its eyes on the nearest living bot within 60 m and walks toward it while it is over 10 m
// off; the match started (everyone put at a spawn point as it goes live: those steps counted apart).
// Every frame each bot is watched: alive or not; in sight (in the view, 40 degrees either side and 25 up or down, 1.5 to 60 m
// off, a clear line from the eyes to its body); drawn or not (its puppet shown, a sprite on it, not hidden or culled, a frame
// with something in it, at its body's place); and how far its brain and its puppet moved since the frame before (over 4 m
// while alive is a jump: the shut-in rescue's spawn point).  Lines: each jump and the first undrawn frames as they come, a
// tally every 30 s, the map's summary "OLSHOT botvanish map ...", one line a case "OLSHOT botvanish case PASS|FAIL: ..." and
// "OLSHOT botvanish RESULT".  -OLOldBotVanish: the way-finding and the rescue as before (a level with no INF: no way anywhere).

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsCoop.h"
#include "OutlawsMpHost.h"
#include "OutlawsEnemy.h"
#include "OutlawsRemotePlayer.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsBotVanishProbeLocal
{
	struct FWatch
	{
		bool bHave = false, bWasAlive = false, bWasSeen = false;
		FVector Brain = FVector::ZeroVector, Body = FVector::ZeroVector;
		double AliveSince = 0.0;
		int32 Jumps = 0, SeenJumps = 0, Undrawn = 0, UndrawnSeen = 0, SeenFrames = 0, AliveFrames = 0;
	};
	TMap<int32, FWatch> Watch;
	TMap<FString, int32> UndrawnWhy;                 // (in sight: what kept it from being drawn)
	int32 Stage = 0, Passes = 0, Fails = 0, Logged = 0, JumpLines = 0;
	double StartAt = 0.0, NextTally = 0.0;
	int64 AliveFrames = 0, SeenFrames = 0, Undrawn = 0, UndrawnSeen = 0, PuppetJumps = 0;
	int32 Jumps = 0, SeenJumps = 0, StartJumps = 0, FreedWas = 0, HeldWas = 0, Bots = 0;
	double PlayingAt = -100.0;                       // (when the match went live: the rules put everyone at a spawn point then)
	bool bWasPlaying = false;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT botvanish case %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}
}

OL_SHOT_MATCH(OLBotVanishProbe) { return OLFlags::Param(TEXT("OLBotVanishProbe")); }

OL_SHOT_TEST(OLBotVanishProbe, 1940)
{
	using namespace OutlawsBotVanishProbeLocal;
	if (!L || Stage < 0) return;
	UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	L->StartSaveDelay = -1.f;           // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	L->bEnemiesActive = true;           // (ShotTest stills them each tick: the bots' brains are bad guys')
	UOutlawsCoop* C = UOutlawsCoop::Get(this);
	const bool bOld = OLFlags::Param(TEXT("OLOldBotVanish"));
	int32 Seconds = 150;
	OLFlags::Value(TEXT("OLBotVanishSeconds"), Seconds);
	const FString Lv = L->LevelName.ToLower();
	const bool bInf = L->DebugInf() != nullptr;
	const bool bUp = C && C->IsHost() && C->IsMatch() && C->bLevelBuilt && !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay();
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT botvanish RESULT %s (%d passed, %d failed; %s%s%s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails, *Lv,
			OLFlags::HasValue(TEXT("OLDeviceProfile")) ? TEXT(", a device profile") : TEXT(""), bOld ? TEXT(", -OLOldBotVanish") : TEXT(""));
		Stage = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	if (Stage == 0)
	{
		if (StartAt == 0.0) StartAt = FPlatformTime::Seconds();
		if (!bUp)
		{
			if (FPlatformTime::Seconds() - StartAt < 180.0) return;
			Check(false, TEXT("needs a launched host with bots: -OLMpHost=<map> -OLMpClass=dm -OLMpBots=7"));
			Finish();
			return;
		}
		TArray<UOutlawsCoop::FBotProbeView> Views;
		C->DebugBotViews(Views);
		if (Views.Num() == 0) return;
		// (the match started: kills count, as in the user's games; the warm-up's bots fight as well)
		FOutlawsMatchFlow& F = OutlawsMpHost::Flow();
		if (F.GetState() == FOutlawsMatchFlow::EState::WarmUp) F.HostStart();
		Bots = Views.Num();
		FreedWas = UOutlawsCoop::DebugBotsFreed();
		HeldWas = UOutlawsCoop::DebugBotsFreeHeld();
		StartAt = Now;
		NextTally = Now + 30.0;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT botvanish watching %d bots on %s (INF %s) for %d s%s"), Views.Num(), *Lv, bInf ? TEXT("yes") : TEXT("none"), Seconds, bOld ? TEXT(" (-OLOldBotVanish)") : TEXT(""));
		Stage = 1;
		return;
	}
	if (!bUp) return;                   // (between levels: nothing to watch)
	TArray<UOutlawsCoop::FBotProbeView> Views;
	C->DebugBotViews(Views);
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC || !PC->PlayerCameraManager) return;
	const FVector Eye = PC->PlayerCameraManager->GetCameraLocation();
	const FRotator EyeRot = PC->PlayerCameraManager->GetCameraRotation();
	{
		const bool bPlaying = OutlawsMpHost::Flow().GetState() == FOutlawsMatchFlow::EState::Playing;
		if (bPlaying && !bWasPlaying) PlayingAt = Now;
		bWasPlaying = bPlaying;
	}
	// (the eyes on the nearest living bot within 60 m, a walk toward it when it is over 10 m off: the fights the user has)
	{
		double Best = 6000.0;
		FVector Look = FVector::ZeroVector;
		for (const UOutlawsCoop::FBotProbeView& V : Views)
		{
			const AOutlawsRemotePlayer* R = C->PuppetOf(V.Who);
			if (V.bDead || !R) continue;
			const double D = FVector::Dist(R->ShownBody(), Eye);
			if (D > 150.0 && D < Best) { Best = D; Look = R->ShownBody(); }
		}
		if (!Look.IsZero())
		{
			PC->SetControlRotation((Look - Eye).Rotation());
			if (Best > 1000.0) AddMovementInput((Look - Eye).GetSafeNormal2D(), 1.f);
		}
	}
	for (const UOutlawsCoop::FBotProbeView& V : Views)
	{
		FWatch& Wt = Watch.FindOrAdd(V.Who);
		const AOutlawsEnemy* E = V.Brain.Get();
		const AOutlawsRemotePlayer* R = C->PuppetOf(V.Who);
		const bool bAlive = !V.bDead && E && R && !R->PlateDead();
		if (!bAlive)
		{
			Wt.bWasAlive = false;
			Wt.bWasSeen = false;
			continue;
		}
		const FVector Brain = E->GetActorLocation();
		const FVector Body = R->ShownBody();
		if (!Wt.bWasAlive) Wt.AliveSince = Now;
		// a jump: its brain, or its puppet, over 4 m since the frame before while alive in both (a respawn after a death is none)
		if (Wt.bHave && Wt.bWasAlive)
		{
			const double Step = FVector::Dist(Brain, Wt.Brain), PStep = FVector::Dist(Body, Wt.Body);
			if (PStep > 400.0) ++PuppetJumps;
			if (Step > 400.0 && Now - PlayingAt < 1.5) ++StartJumps;
			else if (Step > 400.0)
			{
				++Jumps;
				++Wt.Jumps;
				if (Wt.bWasSeen) { ++SeenJumps; ++Wt.SeenJumps; }
				if (JumpLines++ < 40)
				{
					UE_LOG(LogOutlaws, Display, TEXT("OLSHOT botvanish jump: %s %.1f m in a frame, %s to %s (puppet %.1f m), alive, %s; %.0f s in, shut-in moves so far %d"), *V.Name, Step / 100.0,
						*Wt.Brain.ToCompactString(), *Brain.ToCompactString(), PStep / 100.0, Wt.bWasSeen ? TEXT("in sight the frame before") : TEXT("out of sight"), Now - StartAt,
						UOutlawsCoop::DebugBotsFreed() - FreedWas);
				}
			}
		}
		Wt.bHave = true;
		Wt.bWasAlive = true;
		Wt.Brain = Brain;
		Wt.Body = Body;
		++AliveFrames;
		++Wt.AliveFrames;
		// in sight: in the view, near enough, a clear line from the eyes to its body
		const FVector To = Body - Eye;
		const FVector Local = EyeRot.UnrotateVector(To);
		const double Dist = To.Size();
		bool bSeen = Local.X > 0.0 && Dist > 150.0 && Dist < 6000.0
			&& FMath::Abs(FMath::RadiansToDegrees(FMath::Atan2(Local.Y, Local.X))) < 40.0 && FMath::Abs(FMath::RadiansToDegrees(FMath::Atan2(Local.Z, Local.X))) < 25.0;
		const AOutlawsSpriteActor* S = R->ShownSprite();
		if (bSeen)
		{
			FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsBotVanishSight), false, this);
			Q.AddIgnoredActor(R);
			Q.AddIgnoredActor(E);
			if (S) Q.AddIgnoredActor(S);
			FHitResult Hit;
			bSeen = !World->LineTraceSingleByChannel(Hit, Eye, Body, ECC_Visibility, Q);
		}
		Wt.bWasSeen = bSeen;
		// drawn: its puppet shown with a sprite on it at its body's place, the sprite not hidden nor culled, something in its frame
		// (half a second's grace after it came back from a death: the sprite handed over)
		const TCHAR* Why = nullptr;
		FString More;
		if (R->IsHidden()) Why = TEXT("the puppet hidden");
		else if (!S) Why = TEXT("no sprite");
		else if (S->IsHidden()) Why = TEXT("the sprite hidden");
		else if (S->DebugCulled()) { Why = TEXT("the sprite culled"); More = FString::Printf(TEXT(" (its sector %d)"), S->DebugCullSector()); }
		else if (!S->Quad || !S->Quad->IsVisible() || S->Quad->bHiddenInGame) Why = TEXT("the quad hidden");
		else if (S->DebugEmptyCell()) { Why = TEXT("an empty frame"); More = FString::Printf(TEXT(" (cell %d)"), S->DebugCell()); }
		else if (FVector::Dist2D(S->GetActorLocation(), Body) > 200.0) { Why = TEXT("the sprite away from its body"); More = FString::Printf(TEXT(" (%.1f m)"), FVector::Dist2D(S->GetActorLocation(), Body) / 100.0); }
		const bool bGrace = Now - Wt.AliveSince < 0.5;
		if (bSeen) { ++SeenFrames; ++Wt.SeenFrames; }
		if (Why && !bGrace)
		{
			++Undrawn;
			++Wt.Undrawn;
			if (bSeen)
			{
				++UndrawnSeen;
				++Wt.UndrawnSeen;
				++UndrawnWhy.FindOrAdd(Why);
				if (Logged++ < 20)
				{
					UE_LOG(LogOutlaws, Display, TEXT("OLSHOT botvanish undrawn: %s in sight %.1f m off at %s, %s%s; %.0f s in"), *V.Name, Dist / 100.0, *Body.ToCompactString(), Why, *More, Now - StartAt);
				}
			}
		}
	}
	const int32 Freed = UOutlawsCoop::DebugBotsFreed() - FreedWas, Held = UOutlawsCoop::DebugBotsFreeHeld() - HeldWas;
	const double BotMinutes = FMath::Max(0.01, Bots * (Now - StartAt) / 60.0);
	auto Tally = [&]()
	{
		int32 Routes = 0, Fails0 = 0, Stucks = 0;
		for (const UOutlawsCoop::FBotProbeView& V : Views) { Routes += V.Routes; Fails0 += V.RouteFails; Stucks += V.Stucks; }
		FString Whys;
		for (const TPair<FString, int32>& Kv : UndrawnWhy) Whys += FString::Printf(TEXT("%s%s %d"), Whys.IsEmpty() ? TEXT("") : TEXT(", "), *Kv.Key, Kv.Value);
		return FString::Printf(TEXT("%.0f s, %d bots (%.1f bot-minutes); alive bot-frames %lld, in sight %lld; jumps over 4 m alive %d (%.2f a bot-minute; in sight the frame before %d; at the match's start %d more), puppet jumps %lld; shut-in moves %d (held while seen %d); undrawn alive %lld (%.1f in 1000), in sight %lld (%.1f in 1000 in sight%s%s); routes %d, no way %d, stucks %d"),
			Now - StartAt, Bots, BotMinutes, AliveFrames, SeenFrames, Jumps, Jumps / BotMinutes, SeenJumps, StartJumps, PuppetJumps, Freed, Held, Undrawn, AliveFrames > 0 ? 1000.0 * Undrawn / AliveFrames : 0.0,
			UndrawnSeen, SeenFrames > 0 ? 1000.0 * UndrawnSeen / SeenFrames : 0.0, Whys.IsEmpty() ? TEXT("") : TEXT(": "), *Whys, Routes, Fails0, Stucks);
	};
	if (Now >= NextTally)
	{
		NextTally = Now + 30.0;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT botvanish tally: %s"), *Tally());
	}
	if (Now - StartAt < Seconds) return;
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT botvanish map %s (INF %s%s): %s"), *Lv, bInf ? TEXT("yes") : TEXT("none"), bOld ? TEXT(", -OLOldBotVanish") : TEXT(""), *Tally());
	for (const UOutlawsCoop::FBotProbeView& V : Views)
	{
		const FWatch* Wt = Watch.Find(V.Who);
		if (!Wt) continue;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT botvanish bot %s: alive frames %d, in sight %d, jumps %d (in sight %d), undrawn %d (in sight %d); routes %d, no way %d, stucks %d"), *V.Name,
			Wt->AliveFrames, Wt->SeenFrames, Wt->Jumps, Wt->SeenJumps, Wt->Undrawn, Wt->UndrawnSeen, V.Routes, V.RouteFails, V.Stucks);
	}
	Check(Bots > 0 && AliveFrames >= 1000 && SeenFrames >= 100, FString::Printf(TEXT("%d bots watched: %lld alive bot-frames, %lld of them in sight"), Bots, AliveFrames, SeenFrames));
	Check(SeenJumps == 0, FString::Printf(TEXT("no live bot moved over 4 m in a frame while in sight (%d; %d in all)"), SeenJumps, Jumps));
	Check(Jumps <= BotMinutes / 10.0, FString::Printf(TEXT("at most one jump alive per 10 bot-minutes, out of sight (a real trap's rescue): %d in %.1f bot-minutes; shut-in moves %d, held while seen %d"), Jumps, BotMinutes, Freed, Held));
	Check(UndrawnSeen == 0, FString::Printf(TEXT("every live bot in sight drawn (%lld undrawn of %lld frames in sight)"), UndrawnSeen, SeenFrames));
	Finish();
}

#endif // WITH_OUTLAWS_TESTS
