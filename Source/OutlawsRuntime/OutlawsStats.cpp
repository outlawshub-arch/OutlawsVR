// The level's counts and the panels beside the cinema screen at its end (user 2026-10-02: "maybe we can have player stats (one
// for each player on each side of the cinema screen) at the end of each level, kills, deaths, and any other you think might be
// fun").
// - Counted where each event is decided: the shots fired, the shots on target, the deaths, the throws, the revives and the
//   time on each player's own machine; a bad guy's death on the host's in co-op (single player: here), credited to whoever
//   struck him last (AOutlawsEnemy::StatsBy, set by AOutlawsEnemy::TakeHit through NoteHit).  A hit the client reports is
//   dealt by the host inside an FOutlawsStatsScope(Other), so its kill is the client's.
// - Co-op: the host puts both players' counts on their links (AOutlawsCoopLink::LevelStats, replicated): its own, and the
//   client's own counts (sent by the client, ServerLevelStats) with the kills the host credited it.  Both machines draw both
//   panels from the links, so they show the same.
// - The counts start afresh at each new level and go on through a restart or a load of the same one; not saved.

#include "OutlawsStats.h"
#include "OutlawsFlags.h"
#include "OutlawsPerf.h"
#include "OutlawsPerfTest.h"
#include "OutlawsRuntime.h"
#include "OutlawsCoop.h"
#include "OutlawsEnemy.h"
#include "OutlawsLevel.h"
#include "OutlawsLevelActor.h"
#include "OutlawsMovieScreen.h"
#include "OutlawsMpPolish.h"
#include "OutlawsPlayer.h"
#include "OutlawsUnlocks.h"
#include "OutlawsAchievements.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "EngineUtils.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "HAL/PlatformMisc.h"
#include "UnrealClient.h"

namespace
{
	// who strikes the bad guys hit meanwhile (FOutlawsStatsScope); this machine's player unless a scope says otherwise
	EOutlawsStatsBy GOLStatsBy = EOutlawsStatsBy::Me;
	bool bGOLStatsBlast = false;
	bool bGOLStatsDirect = true;
	constexpr float StatsThrowSpeed = 300.f;      // cm/s: a bad guy let go faster than this was thrown, not put down
	constexpr float StatsHoldSeconds = 8.f;       // a level with no cutscene: the panels on the black this long

	bool StatsCountsAsBadGuy(const AOutlawsEnemy* E)
	{
		return E && !E->bVoiceOnly && !E->bNotBadGuy;
	}

	FString StatsClock(int32 Seconds)
	{
		const int32 H = Seconds / 3600, M = (Seconds / 60) % 60, S = Seconds % 60;
		return H > 0 ? FString::Printf(TEXT("%d:%02d:%02d"), H, M, S) : FString::Printf(TEXT("%d:%02d"), M, S);
	}
}

FOutlawsStatsScope::FOutlawsStatsScope(EOutlawsStatsBy By, bool bBlast, bool bDirect)
	: SavedBy(GOLStatsBy), bSavedBlast(bGOLStatsBlast), bSavedDirect(bGOLStatsDirect)
{
	GOLStatsBy = By;
	bGOLStatsBlast = bBlast;
	bGOLStatsDirect = bDirect;
}

FOutlawsStatsScope::~FOutlawsStatsScope()
{
	GOLStatsBy = SavedBy;
	bGOLStatsBlast = bSavedBlast;
	bGOLStatsDirect = bSavedDirect;
}

FString FOutlawsLevelStats::Describe() const
{
	return FString::Printf(TEXT("kills %d, deaths %d (by bad guys %d), shots %d, on target %d, longest kill %d cm, dynamite kills %d, thrown %d, revives %d, time %d s, secrets %d, items %d, damage dealt %.0f, damage taken %.0f"),
		Kills, Deaths, KilledByBadGuys, Shots, OnTarget, LongestKillCm, DynamiteKills, Thrown, Revives, Seconds, Secrets, Items, DamageDealt, DamageTaken);
}

UOutlawsStats* UOutlawsStats::Get(const UObject* Context)
{
	const UWorld* W = GEngine && Context ? GEngine->GetWorldFromContextObject(Context, EGetWorldErrorMode::ReturnNull) : nullptr;
	UGameInstance* GI = W ? W->GetGameInstance() : nullptr;
	return GI ? GI->GetSubsystem<UOutlawsStats>() : nullptr;
}

void UOutlawsStats::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
}

UWorld* UOutlawsStats::GetTickableGameObjectWorld() const
{
	const UGameInstance* GI = GetGameInstance();
	return GI ? GI->GetWorld() : nullptr;
}

TStatId UOutlawsStats::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UOutlawsStats, STATGROUP_Tickables);
}

AOutlawsLevelActor* UOutlawsStats::FindLevel() const
{
	if (AOutlawsLevelActor* L = LevelRef.Get()) return L;
	UWorld* W = GetTickableGameObjectWorld();
	if (!W) return nullptr;
	TActorIterator<AOutlawsLevelActor> It(W);
	return It ? *It : nullptr;
}

AOutlawsPlayer* UOutlawsStats::LocalPlayer() const
{
	UWorld* W = GetTickableGameObjectWorld();
	return W ? Cast<AOutlawsPlayer>(UGameplayStatics::GetPlayerPawn(W, 0)) : nullptr;
}

void UOutlawsStats::ResetCounts()
{
	Mine = FOutlawsLevelStats();
	for (FOutlawsLevelStats& X : OtherHere) X = FOutlawsLevelStats();
	for (FOutlawsLevelStats& X : OtherOwn) X = FOutlawsLevelStats();
	Sent = FOutlawsLevelStats();
	SecretsSeen.Reset();
	SecondsF = 0.f;
	CountedShot = -1;
	ShotFrame = 0;
	SentAt = 0.0;
	// REQ-LEVELTIMER (the Cheats page's toggles outlive the level: a level begun with one on keeps no best time)
	bCheated = bToggleGod || bToggleAmmo || bToggleFreeze;
	bSkipped = false;
	bTimeUnknown = false;
	EasiestDiff = 99;
}

FOutlawsLevelStats& UOutlawsStats::CountsFor(EOutlawsStatsBy By, bool& bOk)
{
	const int32 Who = StatsPlayerOf(By);
	bOk = By == EOutlawsStatsBy::Me || (Who >= 1 && Who < 16);
	return Who >= 1 && Who < 16 ? OtherHere[Who] : Mine;
}

// ---- the events ---------------------------------------------------------------------------------------------------------

void UOutlawsStats::OnLevelBuild(AOutlawsLevelActor* L)
{
	UOutlawsStats* S = L ? Get(L) : nullptr;
	if (!S) return;
	S->LevelRef = L;
	S->bFinished = S->bResolved = false;           // (REQ-LEVELTIMER: a level is built again, the last finish's card is over)
	if (L->IsFrontEnd())
	{
		// (the title menu's backdrop: nothing counts there, and whatever is played next starts afresh)
		S->bToggleGod = S->bToggleAmmo = S->bToggleFreeze = S->bJumpMark = false;
		if (!S->Level.IsEmpty()) S->ResetCounts();
		S->Level.Reset();
		return;
	}
	if (S->Level.Equals(L->LevelName, ESearchCase::IgnoreCase))
	{
		// (REQ-LEVELTIMER: a restart is the level from its start, the clock at nothing; a load sets it to the save's)
		S->SecondsF = 0.f;
		S->Mine.Seconds = 0;
		S->bTimeUnknown = false;
		UE_LOG(LogOutlaws, Log, TEXT("OLSTATS counts go on in %s (the same level again: a restart or a load): %s"), *L->LevelName, *S->Mine.Describe());
	}
	else
	{
		S->ResetCounts();
		S->Level = L->LevelName;
		UE_LOG(LogOutlaws, Log, TEXT("OLSTATS counts start afresh for %s"), *L->LevelName);
	}
	if (S->bJumpMark)
	{
		S->bJumpMark = false;
		S->bCheated = S->bSkipped = true;
	}
}

// ---- REQ-LEVELTIMER ---------------------------------------------------------------------------------------------------------

FString UOutlawsStats::Clock(int32 Seconds)
{
	return StatsClock(FMath::Max(0, Seconds));
}

void UOutlawsStats::NoteCheat(AOutlawsLevelActor* L, const FString& Name, bool bGodWas, bool bAmmoWas, bool bEnemiesActiveWas)
{
	UOutlawsStats* S = L ? Get(L) : nullptr;
	if (!S) return;
	if (Name == TEXT("cheat_god")) S->bToggleGod = !bGodWas;
	else if (Name == TEXT("cheat_ammo")) S->bToggleAmmo = !bAmmoWas;
	else if (Name == TEXT("cheat_freeze")) S->bToggleFreeze = bEnemiesActiveWas;       // (it was active: it is frozen now)
	if (S->Level.IsEmpty() || S->bCheated) return;
	S->bCheated = true;
	UE_LOG(LogOutlaws, Log, TEXT("OLTIMER %s used in %s: no best time for this level"), *Name, *S->Level);
}

void UOutlawsStats::NoteJump(AOutlawsLevelActor* L)
{
	if (UOutlawsStats* S = L ? Get(L) : nullptr) S->bJumpMark = true;
}

void UOutlawsStats::NoteLoaded(AOutlawsLevelActor* L, float Seconds)
{
	UOutlawsStats* S = L ? Get(L) : nullptr;
	if (!S || S->Level.IsEmpty()) return;
	++S->LoadsApplied;
	S->bTimeUnknown = Seconds < 0.f;
	S->SecondsF = FMath::Max(0.f, Seconds);
	S->Mine.Seconds = FMath::FloorToInt(S->SecondsF);
	UE_LOG(LogOutlaws, Log, TEXT("OLTIMER the clock goes on from %s (%.1f s)"), Seconds < 0.f ? TEXT("an older save with no clock: 0, no best time kept") : TEXT("the save's"), S->SecondsF);
}

float UOutlawsStats::SavedSeconds(const UObject* Context)
{
	const UOutlawsStats* S = Get(Context);
	return S && !S->Level.IsEmpty() ? S->SecondsF : 0.f;
}

bool UOutlawsStats::LevelSeconds(const UObject* Context, int32& OutSeconds)
{
	UOutlawsStats* S = Get(Context);
	AOutlawsLevelActor* L = S ? S->FindLevel() : nullptr;
	if (!S || !L || S->Level.IsEmpty() || L->IsFrontEnd()) return false;
	OutSeconds = S->Mine.Seconds;
	if (UOutlawsCoop::IsActive(L))
	{
		// (a session: the host's clock is the one shown to everyone; a client has it on the host's link)
		UOutlawsCoop* C = UOutlawsCoop::Get(L);
		if (C && !C->IsHost())
		{
			TArray<AOutlawsCoopLink*> Links;
			C->GetLinks(Links);
			for (const AOutlawsCoopLink* K : Links)
			{
				if (K->PlayerIndex == 0) { OutSeconds = K->LevelStats.Seconds; break; }
			}
		}
	}
	return true;
}

FString UOutlawsStats::PauseLine(const UObject* Context)
{
	UOutlawsStats* S = Get(Context);
	AOutlawsLevelActor* L = S ? S->FindLevel() : nullptr;
	int32 Secs = 0;
	if (!L || !LevelSeconds(Context, Secs)) return FString();
	if (UOutlawsCoop::IsActive(L))
	{
		const UOutlawsCoop* C = UOutlawsCoop::Get(L);
		if (!C || C->IsMatch()) return FString();               // (a match has no level time)
	}
	return TEXT("Time ") + Clock(Secs);
}

FString UOutlawsStats::TimeKeyFor(const AOutlawsLevelActor* L) const
{
	return (L && L->bHom ? FString(TEXT("hom:")) : FString()) + Level.ToLower();
}

void UOutlawsStats::NoteLevelEnd(AOutlawsLevelActor* L)
{
	UOutlawsStats* S = L ? Get(L) : nullptr;
	if (!S || S->Level.IsEmpty() || L->IsFrontEnd()) return;
	S->bFinished = true;
	S->bResolved = false;
	S->bResultNew = false;
	S->bResultCounts = false;
}

// the finish settled against the best times (once, when the end card is first made: a client's copy of the host's clock has
// had the card's fade to arrive)
void UOutlawsStats::ResolveFinish()
{
	if (!bFinished || bResolved) return;
	AOutlawsLevelActor* L = FindLevel();
	if (!L) return;
	bResolved = true;
	bResultCounts = false;
	const bool bCoop = UOutlawsCoop::IsActive(L);
	if (bCoop)
	{
		const UOutlawsCoop* C = UOutlawsCoop::Get(L);
		if (!C || C->IsMatch()) return;                 // (matches do not use it)
	}
	if (Level.IsEmpty() || L->IsFrontEnd() || (L->bHom && Level.Equals(TEXT("office"), ESearchCase::IgnoreCase))) return;
	int32 Secs = 0;
	if (!LevelSeconds(L, Secs)) return;
	bResultCounts = true;
	ResultKey = TimeKeyFor(L);
	ResultDiff = FMath::Clamp(EasiestDiff < 99 ? EasiestDiff : int32(L->Difficulty), 0, 2);
	ResultWhyNot.Reset();
	if (OutlawsPerfTest::IsOn()) ResultWhyNot = TEXT("performance test");      // (REQ-PERFTESTMODE: nothing kept)
	else if (bSkipped) ResultWhyNot = TEXT("level skipped");
	else if (bCheated) ResultWhyNot = TEXT("cheats used");
	else if (L->RandoOn()) ResultWhyNot = TEXT("Randomizer run");
	else if (!bCoop && L->SoloBadGuysWanted() > 1.001f) ResultWhyNot = TEXT("bad guys multiplier");
	else if (bTimeUnknown) ResultWhyNot = TEXT("older save");
	ResultBest = -1;
	ResultPrior = -1;
	bResultNew = false;
	OutlawsUnlocks::BestTime(L, ResultKey, ResultDiff, ResultBest);
	if (ResultWhyNot.IsEmpty())
	{
		bResultNew = OutlawsUnlocks::NoteBestTime(L, ResultKey, ResultDiff, Secs, bCoop, ResultPrior);
		if (bResultNew) ResultBest = Secs;
		OutlawsAchievements::NoteKeptTime(L, ResultKey, Secs);       // (REQ-EXTRAS: under par)
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLTIMER finish %s on %s: time %d s (%s), best %s%s%s"), *ResultKey, OutlawsDifficulty::Name(static_cast<EOutlawsDifficulty>(ResultDiff)), Secs, *Clock(Secs),
		ResultBest >= 0 ? *Clock(ResultBest) : TEXT("none"), bResultNew ? TEXT(", NEW BEST") : TEXT(""),
		ResultWhyNot.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", no best kept: %s"), *ResultWhyNot));
}

void UOutlawsStats::TimeRows(const UObject* Context, TArray<TPair<FString, FString>>& Out)
{
	UOutlawsStats* S = Get(Context);
	if (!S) return;
	S->ResolveFinish();
	if (!S->bFinished || !S->bResultCounts) return;
	Out.Emplace(FString(TEXT("Best time")), S->ResultBest >= 0 ? Clock(S->ResultBest) : FString(TEXT("-")));
	if (S->bResultNew) Out.Emplace(FString(TEXT("New best!")), S->ResultPrior >= 0 ? TEXT("was ") + Clock(S->ResultPrior) : FString(TEXT("first finish")));
	else if (!S->ResultWhyNot.IsEmpty()) Out.Emplace(FString(TEXT("No best kept")), S->ResultWhyNot);
}

FString UOutlawsStats::TimeSummary(const UObject* Context)
{
	UOutlawsStats* S = Get(Context);
	int32 Secs = 0;
	if (!S || !LevelSeconds(Context, Secs)) return FString();
	S->ResolveFinish();
	if (!S->bFinished || !S->bResultCounts) return Clock(Secs);
	if (S->bResultNew) return Clock(Secs) + TEXT(", New best!");
	return S->ResultBest >= 0 ? Clock(Secs) + TEXT(", best ") + Clock(S->ResultBest) : Clock(Secs);
}

void UOutlawsStats::NoteShotFired(AOutlawsLevelActor* L)
{
	UOutlawsStats* S = L ? Get(L) : nullptr;
	if (!S || S->Level.IsEmpty()) return;
	++S->Mine.Shots;
	S->ShotFrame = GFrameCounter;
}

void UOutlawsStats::NoteHit(AOutlawsEnemy* E, const FVector& From)
{
	if (!StatsCountsAsBadGuy(E) || E->IsDead()) return;
	UOutlawsStats* S = Get(E);
	if (!S) return;
	// a pellet of this player's shot (this frame's) on a living bad guy: the shot was on target (once a volley); the client
	// counts its own on its copies, before the host deals them
	if (GOLStatsBy == EOutlawsStatsBy::Me && S->ShotFrame == GFrameCounter && S->CountedShot != S->Mine.Shots)
	{
		S->CountedShot = S->Mine.Shots;
		++S->Mine.OnTarget;
		if (!bGOLStatsBlast) OutlawsMpPolish::HitConfirm(S->FindLevel(), false);      // (M4: the shooter hears it struck; BUG-ENEMYCLUNK: not a blast)
	}
	if (E->CoopRole == 2) return;           // (the client's copy: the host decides who struck him)
	E->StatsBy = uint8(GOLStatsBy);
	E->StatsFrom = From;
	E->bStatsBlast = bGOLStatsBlast;
	E->bStatsDirect = bGOLStatsDirect;
}

void UOutlawsStats::NoteKill(AOutlawsEnemy* E)
{
	if (!StatsCountsAsBadGuy(E) || E->CoopRole == 2) return;
	UOutlawsStats* S = Get(E);
	if (!S || S->Level.IsEmpty()) return;
	bool bOk = false;
	FOutlawsLevelStats& T = S->CountsFor(EOutlawsStatsBy(E->StatsBy), bOk);
	if (!bOk)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLSTATS kill: %s by nobody's hand"), *E->GetOutlawsName());
		return;
	}
	++T.Kills;
	// (BUG-ENEMYCLUNK) the kill's tick only for a kill by this player's shot, in the frame it was fired: not a fall, a blast, a
	// throw or a thrown knife (they ticked as a kill whoever struck him last)
	if (EOutlawsStatsBy(E->StatsBy) == EOutlawsStatsBy::Me && S->ShotFrame == GFrameCounter && E->bStatsDirect && !E->bStatsBlast)
	{
		OutlawsMpPolish::HitConfirm(S->FindLevel(), true);
	}
	if (E->bStatsBlast) ++T.DynamiteKills;
	const int32 Cm = FMath::RoundToInt(FVector::Dist(E->StatsFrom, E->GetActorLocation()));
	if (E->bStatsDirect && !E->bStatsBlast) T.LongestKillCm = FMath::Max(T.LongestKillCm, Cm);
	UE_LOG(LogOutlaws, Log, TEXT("OLSTATS kill: %s by %s%s, %d cm away (kills %d)"), *E->GetOutlawsName(),
		E->StatsBy >= uint8(EOutlawsStatsBy::Other) ? TEXT("the other player") : TEXT("this machine's player"),
		E->bStatsBlast ? TEXT(" with dynamite") : !E->bStatsDirect ? TEXT(" (a throw or a fall)") : TEXT(""), Cm, T.Kills);
}

void UOutlawsStats::NoteRelease(AOutlawsEnemy* E, const FVector& Vel)
{
	if (!StatsCountsAsBadGuy(E) || E->IsDead()) return;
	UOutlawsStats* S = Get(E);
	if (!S || S->Level.IsEmpty()) return;
	const bool bThrow = Vel.Size() >= StatsThrowSpeed;
	if (GOLStatsBy != EOutlawsStatsBy::Me)
	{
		// (the host throwing its copy for the client: the client counted the throw; the fall or the knock is the client's)
		if (E->CoopRole != 2 && bThrow) { E->StatsBy = uint8(GOLStatsBy); E->bStatsBlast = false; E->bStatsDirect = false; }
		return;
	}
	if (!bThrow) return;
	++S->Mine.Thrown;
	if (E->CoopRole != 2) { E->StatsBy = uint8(EOutlawsStatsBy::Me); E->bStatsBlast = false; E->bStatsDirect = false; }
	UE_LOG(LogOutlaws, Log, TEXT("OLSTATS thrown: %s at %.0f cm/s (thrown %d)"), *E->GetOutlawsName(), Vel.Size(), S->Mine.Thrown);
}

void UOutlawsStats::NoteDeath(AOutlawsLevelActor* L, bool bByBadGuy)
{
	UOutlawsStats* S = L ? Get(L) : nullptr;
	if (!S || S->Level.IsEmpty()) return;
	++S->Mine.Deaths;
	if (bByBadGuy) ++S->Mine.KilledByBadGuys;
	UE_LOG(LogOutlaws, Log, TEXT("OLSTATS death (deaths %d, by bad guys %d)"), S->Mine.Deaths, S->Mine.KilledByBadGuys);
}

void UOutlawsStats::NoteDamage(AOutlawsEnemy* E, float Dealt)
{
	if (Dealt <= 0.f || !StatsCountsAsBadGuy(E) || E->CoopRole == 2) return;
	UOutlawsStats* S = Get(E);
	if (!S || S->Level.IsEmpty()) return;
	bool bOk = false;
	FOutlawsLevelStats& T = S->CountsFor(EOutlawsStatsBy(E->StatsBy), bOk);
	if (bOk) T.DamageDealt += Dealt;
}

void UOutlawsStats::NoteHurt(AOutlawsLevelActor* L, float Dealt)
{
	UOutlawsStats* S = L ? Get(L) : nullptr;
	if (!S || S->Level.IsEmpty() || Dealt <= 0.f) return;
	S->Mine.DamageTaken += Dealt;
}

void UOutlawsStats::NoteItem(AOutlawsLevelActor* L)
{
	UOutlawsStats* S = L ? Get(L) : nullptr;
	if (!S || S->Level.IsEmpty()) return;
	++S->Mine.Items;
}

void UOutlawsStats::NoteSecret(AOutlawsLevelActor* L, int32 Sector)
{
	UOutlawsStats* S = L ? Get(L) : nullptr;
	if (!S || S->Level.IsEmpty() || S->SecretsSeen.Contains(Sector)) return;
	S->SecretsSeen.Add(Sector);
	++S->Mine.Secrets;
	OutlawsAchievements::NoteSecret(L, L->LevelName, Sector, L->SecretSectorCount());           // (REQ-EXTRAS)
	UE_LOG(LogOutlaws, Log, TEXT("OLSTATS secret found: sector %d (secrets %d)"), Sector, S->Mine.Secrets);
}

void UOutlawsStats::NoteRevive(AOutlawsLevelActor* L)
{
	UOutlawsStats* S = L ? Get(L) : nullptr;
	if (!S || S->Level.IsEmpty()) return;
	++S->Mine.Revives;
	UE_LOG(LogOutlaws, Log, TEXT("OLSTATS revive given (revives %d)"), S->Mine.Revives);
}

// ---- co-op: both players' counts on their links ---------------------------------------------------------------------------

void AOutlawsCoopLink::ServerLevelStats_Implementation(const FOutlawsLevelStats& In)
{
	if (!Admit(EOLGate::LevelStats, true)) return;
	if (PlayerIndex < 1) return;
	if (UOutlawsStats* S = UOutlawsStats::Get(this)) S->OnClientStats(PlayerIndex, In);
}

void UOutlawsStats::OnClientStats(int32 Who, const FOutlawsLevelStats& In)
{
	UOutlawsCoop* C = UOutlawsCoop::Get(this);
	if (!C || !C->IsHost()) return;
	if (In.Epoch != C->LevelEpoch())
	{
		UE_LOG(LogOutlaws, Verbose, TEXT("OLSTATS the client's counts for start %d dropped (here %d)"), In.Epoch, C->LevelEpoch());
		return;
	}
	if (Who >= 1 && Who < 16) OtherOwn[Who] = In;
}

void UOutlawsStats::TickCoop(double Now)
{
	AOutlawsLevelActor* L = FindLevel();
	if (!L || !UOutlawsCoop::IsActive(L)) return;
	UOutlawsCoop* C = UOutlawsCoop::Get(L);
	if (!C) return;
	const int32 Epoch = C->LevelEpoch();
	if (C->IsHost())
	{
		FOutlawsLevelStats Host = Mine;
		Host.Epoch = Epoch;
		if (AOutlawsCoopLink* H = C->GetLink(0); H && H->LevelStats != Host) H->LevelStats = Host;
		// each client's: its own counts as it sent them, the bad guys' deaths as credited here (N1: every client)
		C->ForClients([&](AOutlawsCoopLink& K)
		{
			const int32 k = FMath::Clamp(K.PlayerIndex, 1, 7);
			FOutlawsLevelStats Client = OtherOwn[k];
			Client.Kills = OtherHere[k].Kills;
			Client.DynamiteKills = OtherHere[k].DynamiteKills;
			Client.LongestKillCm = OtherHere[k].LongestKillCm;
			Client.DamageDealt = OtherHere[k].DamageDealt;
			Client.Epoch = Epoch;
			if (K.LevelStats != Client) K.LevelStats = Client;
		});
	}
	else if (C->IsClient() && Epoch >= 0 && !Level.IsEmpty())
	{
		FOutlawsLevelStats Own = Mine;
		Own.Epoch = Epoch;
		AOutlawsCoopLink* MyLink = C->GetLink(C->MyIndex());
		if (MyLink && Own != Sent && Now - SentAt >= 0.5)
		{
			MyLink->ServerLevelStats(Own);
			Sent = Own;
			SentAt = Now;
		}
	}
}

// ---- the panels ---------------------------------------------------------------------------------------------------------

FOutlawsStatsPanel UOutlawsStats::MakePanel(const FString& Name, const FString& LevelTitle, const FOutlawsLevelStats& S, bool bCoop, int32 ShownSeconds, const TArray<TPair<FString, FString>>* Extra)
{
	FOutlawsStatsPanel P;
	P.Name = Name;
	P.Sub = LevelTitle;
	auto Row = [&P](const TCHAR* Label, const FString& Value) { P.Rows.Emplace(FString(Label), Value); };
	Row(TEXT("Bad guys killed"), FString::FromInt(S.Kills));
	Row(TEXT("Deaths"), FString::FromInt(S.Deaths));
	Row(TEXT("Killed by bad guys"), FString::FromInt(S.KilledByBadGuys));
	Row(TEXT("Shots fired"), FString::FromInt(S.Shots));
	Row(TEXT("Accuracy"), S.Shots > 0 ? FString::Printf(TEXT("%d%%"), FMath::RoundToInt(100.f * float(S.OnTarget) / float(S.Shots))) : FString(TEXT("-")));
	Row(TEXT("Longest kill"), S.LongestKillCm > 0 ? FString::Printf(TEXT("%d m"), FMath::Max(1, FMath::RoundToInt(S.LongestKillCm / 100.f))) : FString(TEXT("-")));
	Row(TEXT("Dynamite kills"), FString::FromInt(S.DynamiteKills));
	Row(TEXT("Bad guys thrown"), FString::FromInt(S.Thrown));
	Row(TEXT("Damage dealt"), FString::FromInt(FMath::RoundToInt(S.DamageDealt)));
	Row(TEXT("Damage taken"), FString::FromInt(FMath::RoundToInt(S.DamageTaken)));
	Row(TEXT("Items picked up"), FString::FromInt(S.Items));
	Row(TEXT("Secrets found"), FString::FromInt(S.Secrets));
	if (bCoop) Row(TEXT("Partner revived"), FString::FromInt(S.Revives));
	Row(TEXT("Time"), StatsClock(ShownSeconds >= 0 ? ShownSeconds : S.Seconds));
	if (Extra) for (const TPair<FString, FString>& R : *Extra) P.Rows.Add(R);
	return P;
}

bool UOutlawsStats::Panels(const UObject* Context, TArray<FOutlawsStatsPanel>& Out, bool& bTwo)
{
	Out.Reset();
	bTwo = false;
	UOutlawsStats* S = Get(Context);
	AOutlawsLevelActor* L = S ? S->FindLevel() : nullptr;
	if (!S || !L || S->Level.IsEmpty()) return false;
	// (a historical mission: its own name, The Granary, not the file's)
	const FString Title = L->bHom && S->Level.Equals(L->HomMission, ESearchCase::IgnoreCase) ? L->HomMissionLabel() : AOutlawsLevelActor::LevelTitle(S->Level);
	if (UOutlawsCoop::IsActive(L))
	{
		UOutlawsCoop* C = UOutlawsCoop::Get(L);
		TArray<AOutlawsCoopLink*> Links;
		if (C) C->GetLinks(Links);
		if (Links.Num() < 2 || Links[0]->PlayerIndex != 0) return false;
		// the name each player set (Options, Multiplayer), else the player's number: the host is Player 1
		auto NameOf = [](const AOutlawsCoopLink* K) { return K->PlayerName.TrimStartAndEnd().IsEmpty() ? FString::Printf(TEXT("Player %d"), K->PlayerIndex + 1) : K->PlayerName.TrimStartAndEnd(); };
		// (REQ-LEVELTIMER: the host's clock on every panel; the best times are this machine's player's own, on its panel only)
		int32 HostSecs = Links[0]->LevelStats.Seconds;
		LevelSeconds(L, HostSecs);
		TArray<TPair<FString, FString>> Best;
		TimeRows(L, Best);
		if (Links.Num() == 2)
		{
			const int32 MyIdx = C->MyIndex();
			Out.Add(MakePanel(NameOf(Links[0]), Title, Links[0]->LevelStats, true, HostSecs, Links[0]->PlayerIndex == MyIdx ? &Best : nullptr));
			Out.Add(MakePanel(NameOf(Links[1]), Title, Links[1]->LevelStats, true, HostSecs, Links[1]->PlayerIndex == MyIdx ? &Best : nullptr));
			bTwo = true;
			return true;
		}
		// N1, three or more (until the Frag Table, FT, has a row for each): this machine's player's own panel, and the posse's,
		// a row for each player with their kills and deaths
		const int32 Me = C->MyIndex();
		const AOutlawsCoopLink* const* Mine = Links.FindByPredicate([Me](const AOutlawsCoopLink* K) { return K->PlayerIndex == Me; });
		if (Mine) Out.Add(MakePanel(NameOf(*Mine), Title, (*Mine)->LevelStats, true, HostSecs, &Best));
		FOutlawsStatsPanel Posse;
		Posse.Name = FString::Printf(TEXT("The posse (%d)"), Links.Num());
		Posse.Sub = Title;
		for (const AOutlawsCoopLink* K : Links)
		{
			Posse.Rows.Emplace(NameOf(K), FString::Printf(TEXT("%d killed, %d deaths"), K->LevelStats.Kills, K->LevelStats.Deaths));
		}
		Out.Add(Posse);
		bTwo = true;
		return true;
	}
	// REQ-PERFCARD (user 28 Oct): a performance test's results card, the counts' style and size, to their right; alone when the
	// run was quit from the pause menu (no counts show then)
	FOutlawsStatsPanel PerfCard;
	const bool bPerfCard = OutlawsPerfTest::CardRows().Num() > 0 && OutlawsPerfTest::CardLevel().Equals(S->Level, ESearchCase::IgnoreCase);
	if (bPerfCard)
	{
		PerfCard.Name = TEXT("Performance");
		PerfCard.Sub = Title;
		PerfCard.Rows = OutlawsPerfTest::CardRows();
		if (OutlawsPerfTest::CardAlone())
		{
			Out.Add(PerfCard);
			return true;
		}
	}
	const FString Name = L->MpPlayerName.TrimStartAndEnd().IsEmpty() ? FString(TEXT("James Anderson")) : L->MpPlayerName.TrimStartAndEnd();
	TArray<TPair<FString, FString>> Best;
	TimeRows(L, Best);          // (REQ-LEVELTIMER: after the finish, the best time and the new best)
	Out.Add(MakePanel(Name, Title, S->Mine, false, -1, &Best));
	if (L->RandoOn()) Out.Last().Rows.Emplace(FString(TEXT("Seed")), FString::Printf(TEXT("%u"), L->RandoRun.Seed));     // (R10: the run's, to play it again)
	// (HM1) a historical mission's results card beside the counts: the original's poster rows
	if (L->bHom && L->HomCardRows.Num() > 0)
	{
		FOutlawsStatsPanel Card;
		Card.Name = L->HomCardTitle;
		Card.Sub = TEXT("Marshal Training");
		Card.Rows = L->HomCardRows;
		Out.Add(Card);
		bTwo = true;
	}
	if (bPerfCard)
	{
		Out.Add(PerfCard);
		bTwo = true;
	}
	return true;
}

FString FOutlawsStatsCard::Lines() const
{
	FString All = FString::Printf(TEXT("[%s] %s"), *Title, *Sub);
	if (!bTable)
	{
		for (const TPair<FString, FString>& R : Rows) All += FString::Printf(TEXT(" | %s %s"), *R.Key, *R.Value);
		return All;
	}
	All += TEXT(" | ") + FString::Join(Cols, TEXT(", "));
	for (const TArray<FString>& Row : Cells) All += TEXT(" | ") + FString::Join(Row, TEXT(", "));
	return All;
}

bool UOutlawsStats::HandCard(const UObject* Context, FOutlawsStatsCard& Out)
{
	Out = FOutlawsStatsCard();
	TArray<FOutlawsStatsPanel> Ps;
	bool bTwo = false;
	if (!Panels(Context, Ps, bTwo) || Ps.Num() == 0) return false;
	UOutlawsStats* S = Get(Context);
	AOutlawsLevelActor* L = S ? S->FindLevel() : nullptr;
	if (!L) return false;
	if (!UOutlawsCoop::IsActive(L))
	{
		// single player: the player's own panel as it is
		Out.Title = Ps[0].Name;
		Out.Sub = Ps[0].Sub;
		Out.Rows = Ps[0].Rows;
		return true;
	}
	// co-op: built here from the links' counts (each machine has every player's), this machine's player's row first
	UOutlawsCoop* C = UOutlawsCoop::Get(L);
	TArray<AOutlawsCoopLink*> Links;
	if (C) C->GetLinks(Links);
	if (Links.Num() < 2) return false;
	const int32 Me = C->MyIndex();
	Links.Sort([Me](const AOutlawsCoopLink& A, const AOutlawsCoopLink& B)
	{
		if ((A.PlayerIndex == Me) != (B.PlayerIndex == Me)) return A.PlayerIndex == Me;
		return A.PlayerIndex < B.PlayerIndex;
	});
	auto NameOf = [](const AOutlawsCoopLink* K) { return K->PlayerName.TrimStartAndEnd().IsEmpty() ? FString::Printf(TEXT("Player %d"), K->PlayerIndex + 1) : K->PlayerName.TrimStartAndEnd(); };
	Out.bTable = true;
	Out.Title = FString::Printf(TEXT("The posse (%d)"), Links.Num());
	Out.Sub = FString::Printf(TEXT("%s, %s"), *Ps[0].Sub, *TimeSummary(L));
	Out.Cols = { TEXT("Player"), TEXT("Kills"), TEXT("Deaths"), TEXT("Accuracy"), TEXT("Damage"), TEXT("Revives") };
	for (const AOutlawsCoopLink* K : Links)
	{
		const FOutlawsLevelStats& St = K->LevelStats;
		Out.Cells.Add({ NameOf(K), FString::FromInt(St.Kills), FString::FromInt(St.Deaths),
			St.Shots > 0 ? FString::Printf(TEXT("%d%%"), FMath::RoundToInt(100.f * float(St.OnTarget) / float(St.Shots))) : FString(TEXT("-")),
			FString::FromInt(FMath::RoundToInt(St.DamageDealt)), FString::FromInt(St.Revives) });
	}
	return true;
}

void UOutlawsStats::ShowOn(AOutlawsLevelActor* L, AOutlawsMovieScreen* Screen)
{
	UOutlawsStats* S = L ? Get(L) : nullptr;
	if (!S || !Screen || L->IsFrontEnd() || !S->Level.Equals(L->LevelName, ESearchCase::IgnoreCase)) return;
	UE_LOG(LogOutlaws, Log, TEXT("OLSTATS the cutscene shows the counts for %s beside its picture"), *S->Level);
	Screen->ShowStats();
}

bool UOutlawsStats::HoldBetweenLevels(AOutlawsLevelActor* L)
{
	UOutlawsStats* S = L ? Get(L) : nullptr;
	UWorld* World = L ? L->GetWorld() : nullptr;
	AOutlawsPlayer* P = S ? S->LocalPlayer() : nullptr;
	if (!S || !World || !P || L->IsFrontEnd() || L->bMovieToMenu || !S->Level.Equals(L->LevelName, ESearchCase::IgnoreCase)) return false;
	// as a cutscene's screen (PlayMovie), with no picture: the level cleared away, the panels alone on the black
	L->Clear();
	P->GetCharacterMovement()->DisableMovement();
	P->GetCharacterMovement()->Velocity = FVector::ZeroVector;
	FActorSpawnParameters SP;
	SP.ObjectFlags |= RF_Transient;
	L->Movie = World->SpawnActor<AOutlawsMovieScreen>(AOutlawsMovieScreen::StaticClass(), FTransform::Identity, SP);
	if (!L->Movie) return false;
	L->Movie->OnFinished.AddUObject(L, &AOutlawsLevelActor::StartNextLevel);
	L->FadeCamera(1.f, 0.f, 1.0f, false);
	const UCameraComponent* Cam = P->FindComponentByClass<UCameraComponent>();
	UE_LOG(LogOutlaws, Log, TEXT("OLSTATS no cutscene after %s: the counts on the black for %.0f s"), *S->Level, StatsHoldSeconds);
	L->Movie->HoldStats(Cam ? Cam->GetComponentLocation() : P->GetActorLocation(), Cam ? Cam->GetForwardVector() : P->GetActorForwardVector(), StatsHoldSeconds);
	L->SetActorTickEnabled(true);
	return true;
}

bool UOutlawsStats::HoldPerfCard(AOutlawsLevelActor* L)
{
	UOutlawsStats* S = L ? Get(L) : nullptr;
	UWorld* World = L ? L->GetWorld() : nullptr;
	AOutlawsPlayer* P = S ? S->LocalPlayer() : nullptr;
	if (!S || !World || !P || L->IsFrontEnd() || L->Movie || OutlawsPerfTest::CardRows().Num() == 0 || !OutlawsPerfTest::CardLevel().Equals(S->Level, ESearchCase::IgnoreCase)) return false;
	// as HoldBetweenLevels, the card alone (Panels), then the main menu as End Game goes there
	OutlawsPerfTest::SetCardAlone(true);
	L->Clear();
	P->GetCharacterMovement()->DisableMovement();
	P->GetCharacterMovement()->Velocity = FVector::ZeroVector;
	FActorSpawnParameters SP;
	SP.ObjectFlags |= RF_Transient;
	L->Movie = World->SpawnActor<AOutlawsMovieScreen>(AOutlawsMovieScreen::StaticClass(), FTransform::Identity, SP);
	if (!L->Movie) return false;
	L->Movie->OnFinished.AddUObject(L, &AOutlawsLevelActor::ReturnToFrontEnd);
	L->FadeCamera(1.f, 0.f, 1.0f, false);
	const UCameraComponent* Cam = P->FindComponentByClass<UCameraComponent>();
	UE_LOG(LogOutlaws, Log, TEXT("OLSTATS performance test quit in %s: its results card alone on the black for %.0f s"), *S->Level, StatsHoldSeconds);
	L->Movie->HoldStats(Cam ? Cam->GetComponentLocation() : P->GetActorLocation(), Cam ? Cam->GetForwardVector() : P->GetActorForwardVector(), StatsHoldSeconds);
	L->SetActorTickEnabled(true);
	return true;
}

// ---- the tick: the time in the level, the links, the tests ---------------------------------------------------------------

void UOutlawsStats::Tick(float DeltaTime)
{
	OL_PERF_SCOPE("Other ours");
	AOutlawsLevelActor* L = FindLevel();
	if (L && !Level.IsEmpty() && Level.Equals(L->LevelName, ESearchCase::IgnoreCase) && !L->IsFrontEnd() && !L->IsRebuildPending()
		&& L->LevelEndTimer < 0.f && !L->Movie && L->GetWorld())
	{
		// REQ-LEVELTIMER: world time (single player's menu, which all but stops it, does not count; nor do the cutscenes, the
		// loads or the death screen: a session's clock runs on, its world does)
		if (!L->IsPlayerDead() || UOutlawsCoop::IsActive(L)) SecondsF += L->GetWorld()->GetDeltaSeconds();
		Mine.Seconds = FMath::FloorToInt(SecondsF);
		EasiestDiff = FMath::Min(EasiestDiff, int32(L->Difficulty));       // (the difficulty can be changed in play: the easiest it was counts)
	}
	const double Now = FPlatformTime::Seconds();
	TickCoop(Now);
#if WITH_OUTLAWS_TESTS
	static const bool bTimerTest = OLFlags::HasValue(TEXT("OLTimerTest"));
	if (bTimerTest) TickTimerTest(Now);
	if (OLFlags::Param(TEXT("OLStatsTest"))) TickTest(Now);
	if (OLFlags::Param(TEXT("OLStatsCoopTest"))) TickCoopTest(Now);
#endif
}

// ---- -OLStatsTest: single player ------------------------------------------------------------------------------------------

namespace
{
	/** The living ordinary bad guys about (no boss, no critter, no voice, none waiting out of sight), nearest first. */
	TArray<AOutlawsEnemy*> StatsTestTargets(AOutlawsLevelActor* L, const FVector& Near)
	{
		TArray<AOutlawsEnemy*> Out;
		for (const TWeakObjectPtr<AOutlawsEnemy>& W : L->GetEnemies())
		{
			AOutlawsEnemy* E = W.Get();
			if (E && StatsCountsAsBadGuy(E) && !E->IsDead() && !E->bBoss && !E->IsCritter() && !E->IsDormant()) Out.Add(E);
		}
		Out.Sort([&Near](const AOutlawsEnemy& A, const AOutlawsEnemy& B) { return FVector::DistSquared(A.GetActorLocation(), Near) < FVector::DistSquared(B.GetActorLocation(), Near); });
		return Out;
	}
}

#if WITH_OUTLAWS_TESTS
void UOutlawsStats::TickTest(double Now)
{
	AOutlawsLevelActor* L = FindLevel();
	AOutlawsPlayer* P = LocalPlayer();
	if (!L || !P) return;
	const bool bReady = !L->IsFrontEnd() && !L->IsRebuildPending() && Level.Equals(L->LevelName, ESearchCase::IgnoreCase) && !L->IsPlayerDead();
	const UCameraComponent* Cam = P->FindComponentByClass<UCameraComponent>();
	const FVector Eye = Cam ? Cam->GetComponentLocation() : P->GetActorLocation();
	auto ShootDead = [&](AOutlawsEnemy* E)
	{
		// a shot fired, and its bullet into him the same frame (as a real shot's pellet): on target, his death the player's
		P->FireCooldown[1] = 0.f;
		P->FireGun(1, false);
		const FVector Dir = (E->GetActorLocation() - Eye).GetSafeNormal();
		E->TakeHit(100.f, E->GetActorLocation(), Dir, Eye);
	};
	switch (TestStep)
	{
	case 0:
		if (!bReady) return;
		TestAt = Now + 4.0;
		TestStep = 1;
		UE_LOG(LogOutlaws, Log, TEXT("OLSTATS test: %s is up"), *L->LevelName);
		return;
	case 1:
	{
		if (Now < TestAt) return;
		P->Equip(1, 0);
		for (int32 i = 0; i < 3; ++i) { P->FireCooldown[1] = 0.f; P->FireGun(1, false); }      // (three wherever the gun points)
		TArray<AOutlawsEnemy*> T = StatsTestTargets(L, Eye);
		for (int32 i = 0; i < 2 && i < T.Num(); ++i) ShootDead(T[i]);
		UE_LOG(LogOutlaws, Log, TEXT("OLSTATS test: 5 shots fired, 2 of them into bad guys (of %d about): %s"), T.Num(), *Mine.Describe());
		TestAt = Now + 1.0;
		TestStep = 2;
		return;
	}
	case 2:
	{
		if (Now < TestAt) return;
		// dynamite: a blast at a bad guy at least 9 m off (none of it reaches the player)
		TArray<AOutlawsEnemy*> T = StatsTestTargets(L, Eye);
		for (int32 i = T.Num() - 1; i >= 0; --i)
		{
			if (FVector::Dist(T[i]->GetActorLocation(), Eye) < 900.0) continue;
			L->Explode(T[i]->GetActorLocation() + FVector(0.0, 0.0, -30.0));
			break;
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLSTATS test: a blast set off: %s"), *Mine.Describe());
		TestAt = Now + 1.5;
		TestStep = 3;
		return;
	}
	case 3:
	{
		if (Now < TestAt) return;
		// a bad guy taken into the right hand and thrown, the one throw's way (LetGoEnemy)
		TArray<AOutlawsEnemy*> T = StatsTestTargets(L, Eye);
		if (T.Num() > 0 && P->TryGrabEnemy(1, T[0]))
		{
			const FVector Launch = P->GetActorForwardVector() * 700.0 + FVector(0.0, 0.0, 250.0);
			P->LetGoEnemy(1, true, &Launch);
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLSTATS test: a bad guy thrown: %s"), *Mine.Describe());
		TestAt = Now + 2.0;
		TestStep = 4;
		return;
	}
	case 4:
	{
		if (Now < TestAt) return;
		NoteItem(L);                           // (REQ-STATS: as a pickup taken, and the level's first secret sector entered, twice: counted once)
		int32 SecretSectors = 0;
		for (int32 i = 0; i < L->TestSectorCount(); ++i)
		{
			if (!(L->TestSectorFlags(i) & OutlawsFlags::SecSecretTag)) continue;
			if (SecretSectors++ == 0) { NoteSecret(L, i); NoteSecret(L, i); }
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLSTATS test: %d secret sectors in %s"), SecretSectors, *L->LevelName);
		L->bInvincible = false;
		L->bHurtByBadGuy = true;               // (as a bad guy's shot)
		L->DamagePlayer(100.f, 1.f, true);
		UE_LOG(LogOutlaws, Log, TEXT("OLSTATS test: the player died: %s"), *Mine.Describe());
		TestAt = Now + 2.5;
		TestStep = 5;
		return;
	}
	case 5:
		if (Now < TestAt) return;
		UE_LOG(LogOutlaws, Log, TEXT("OLSTATS test: the level restarts (as the death screen's restart)"));
		L->RestartLevel();
		TestStep = 6;
		TestAt = 0.0;
		return;
	case 6:
	{
		if (!bReady || !L->HasActorBegunPlay()) return;
		if (TestAt == 0.0) { TestAt = Now + 3.0; return; }
		if (Now < TestAt) return;
		TArray<AOutlawsEnemy*> T = StatsTestTargets(L, Eye);
		if (T.Num() > 0) ShootDead(T[0]);
		UE_LOG(LogOutlaws, Log, TEXT("OLSTATS test: after the restart, one more: %s"), *Mine.Describe());
		L->CompleteLevel(TEXT("the stats test"));
		if (OLFlags::Param(TEXT("OLStatsHold")))
		{
			L->NextMovie.Reset();              // (-OLStatsHold: as a level with no cutscene: the counts alone on the black)
			UE_LOG(LogOutlaws, Log, TEXT("OLSTATS test: no cutscene this time"));
		}
		TestStep = 7;
		return;
	}
	case 7:
		if (!L->IsMoviePlaying()) return;
		TestShownAt = Now;
		TestStep = 8;
		UE_LOG(LogOutlaws, Log, TEXT("OLSTATS test: the cinema is up"));
		return;
	case 8:
	{
		if (Now < TestShownAt + 4.0) return;
		const TCHAR* Shot = OLFlags::Param(TEXT("OLStatsHold")) ? TEXT("ol_stats_hold.png") : OLFlags::Param(TEXT("OLHandCard")) ? TEXT("ol_handcard_sp.png") : TEXT("ol_stats_sp.png");
		// (BUG-SCORESHEETSIZE: once the sheets' shaders are in, as the hom test's results card)
		const bool bWait = !OLFlags::Param(TEXT("OLShotNoShaderWait"));
		const int32 Pending = L->Movie ? L->Movie->SheetShadersPending(bWait) : 0;
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Shot, false, false);
		UE_LOG(LogOutlaws, Log, TEXT("OLSTATS test: screenshot %s, sheet shaders compiling %d%s"), Shot, Pending, Pending > 0 ? (bWait ? TEXT(" (waited)") : TEXT(" (not waited)")) : TEXT(""));
		TestStep = 9;
		return;
	}
	case 9:
	{
		if (Now < TestShownAt + 6.0) return;
		TArray<FOutlawsStatsPanel> Ps;
		bool bTwo = false;
		Panels(L, Ps, bTwo);
		for (const FOutlawsStatsPanel& Pn : Ps)
		{
			FString Rows;
			for (const TPair<FString, FString>& R : Pn.Rows) Rows += FString::Printf(TEXT("%s%s %s"), Rows.IsEmpty() ? TEXT("") : TEXT(", "), *R.Key, *R.Value);
			UE_LOG(LogOutlaws, Log, TEXT("OLSTATS test panel: [%s] %s"), *Pn.Name, *Rows);
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLSTATS test: done (%s)"), *Mine.Describe());
		TestStep = 10;
		// (-OLStatsHold: on until the hold is over and the next level is up, its counts afresh)
		if (!OLFlags::Param(TEXT("OLStatsHold"))) FGenericPlatformMisc::RequestExit(false);
		return;
	}
	case 10:
		if (Level.Equals(TEXT("hideout"), ESearchCase::IgnoreCase) && Now < TestShownAt + 40.0) return;
		UE_LOG(LogOutlaws, Log, TEXT("OLSTATS test: after the hold: counting %s (%s)"), *Level, *Mine.Describe());
		TestStep = 11;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS

// ---- -OLStatsCoopTest (with -OLCoopReady on both copies of a pair) -------------------------------------------------------

#if WITH_OUTLAWS_TESTS
void UOutlawsStats::TickCoopTest(double Now)
{
	AOutlawsLevelActor* L = FindLevel();
	AOutlawsPlayer* P = LocalPlayer();
	UOutlawsCoop* C = L ? UOutlawsCoop::Get(L) : nullptr;
	if (!L || !P || !C) return;
	const bool bHost = C->IsHost();
	const bool bActive = UOutlawsCoop::IsActive(L);
	const bool bReady = bActive && C->LevelEpoch() >= 0 && !L->IsFrontEnd() && !L->IsRebuildPending() && Level.Equals(L->LevelName, ESearchCase::IgnoreCase);
	const UCameraComponent* Cam = P->FindComponentByClass<UCameraComponent>();
	const FVector Eye = Cam ? Cam->GetComponentLocation() : P->GetActorLocation();
	if (TestStep == 0) bTestHost = bHost;
	const TCHAR* Who = bTestHost ? TEXT("host") : TEXT("client");
	auto Eligible = [&]()
	{
		TArray<AOutlawsEnemy*> Out;
		for (const TWeakObjectPtr<AOutlawsEnemy>& W : L->GetEnemies())
		{
			AOutlawsEnemy* E = W.Get();
			if (E && StatsCountsAsBadGuy(E) && !E->IsDead() && !E->bBoss && !E->IsCritter() && !E->IsDormant()) Out.Add(E);
		}
		return Out;
	};
	auto LogPanels = [&](const TCHAR* When)
	{
		TArray<FOutlawsStatsPanel> Ps;
		bool bTwo = false;
		Panels(L, Ps, bTwo);
		FString All;
		for (const FOutlawsStatsPanel& Pn : Ps)
		{
			FString Rows;
			for (const TPair<FString, FString>& R : Pn.Rows) Rows += FString::Printf(TEXT("%s%s %s"), Rows.IsEmpty() ? TEXT("") : TEXT(", "), *R.Key, *R.Value);
			All += FString::Printf(TEXT("%s[%s] %s"), All.IsEmpty() ? TEXT("") : TEXT(" | "), *Pn.Name, *Rows);
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLSTATS test panels %s (%s): %s"), When, Who, *All);
	};
	switch (TestStep)
	{
	case 0:
		if (!bReady) return;
		if (bHost)
		{
			AOutlawsCoopLink* K = C->GetLink(1);
			if (!K || K->BuiltSerial != C->StartedSerial) return;      // (the client's level not up yet)
		}
		TestAt = Now + (bHost ? 4.0 : 5.0);
		TestStep = 1;
		UE_LOG(LogOutlaws, Log, TEXT("OLSTATS test: %s up (%s)"), *L->LevelName, Who);
		return;
	case 1:
	{
		if (Now < TestAt) return;
		// each kills one: the host the first of the bad guys, the client the last (each a shot and its bullet the same frame)
		P->Equip(1, 0);
		if (float Set = -1.f; !bHost && OLFlags::Value(TEXT("OLTimerSet"), Set))
		{
			SecondsF = Set + 37.f;                 // (REQ-LEVELTIMER: the client's own clock, kept apart from the host's)
			Mine.Seconds = FMath::FloorToInt(SecondsF);
			UE_LOG(LogOutlaws, Log, TEXT("OLTIMER coop test: the client's own clock set to %d s"), Mine.Seconds);
		}
		const int32 Extra = bHost ? 1 : 2;
		for (int32 i = 0; i < Extra; ++i) { P->FireCooldown[1] = 0.f; P->FireGun(1, false); }
		TArray<AOutlawsEnemy*> T = Eligible();
		if (T.Num() > 0)
		{
			AOutlawsEnemy* E = bHost ? T[0] : T.Last();
			P->FireCooldown[1] = 0.f;
			P->FireGun(1, false);
			E->TakeHit(100.f, E->GetActorLocation(), (E->GetActorLocation() - Eye).GetSafeNormal(), Eye);
			UE_LOG(LogOutlaws, Log, TEXT("OLSTATS test: the %s shot enemy %d (%s) %.0f cm away"), Who, E->CoopIndex, *E->GetOutlawsName(), FVector::Dist(E->GetActorLocation(), Eye));
		}
		TestAt = Now + 2.0;
		TestStep = 2;
		return;
	}
	case 2:
		if (Now < TestAt) return;
		if (!bHost)
		{
			// the client dies once (its own death, on its own machine)
			L->DamagePlayer(100.f, 1.f, true);
			UE_LOG(LogOutlaws, Log, TEXT("OLSTATS test: the client died"));
			TestStep = 4;
			return;
		}
		TestAt = Now + 40.0;
		TestStep = 3;
		return;
	case 3:
	{
		// the host waits for the client's kill and death to be counted here, then ends the level for both
		AOutlawsCoopLink* K = C->GetLink(1);
		const bool bBoth = K && K->LevelStats.Kills >= 1 && K->LevelStats.Deaths >= 1;
		if (!bBoth && Now < TestAt) return;
		UE_LOG(LogOutlaws, Log, TEXT("OLSTATS test: the host ends the level (the client's kill and death %s)"), bBoth ? TEXT("counted") : TEXT("NOT COUNTED in 40 s"));
		LogPanels(TEXT("before the end"));
		if (float Set = -1.f; OLFlags::Value(TEXT("OLTimerSet"), Set))
		{
			// (REQ-LEVELTIMER: the host's clock set to a known time; the client's own is set apart, to show whose is on the cards)
			SecondsF = Set;
			Mine.Seconds = FMath::FloorToInt(Set);
			UE_LOG(LogOutlaws, Log, TEXT("OLTIMER coop test: the host's clock set to %d s"), Mine.Seconds);
		}
		L->CompleteLevel(TEXT("the stats test"));
		TestStep = 4;
		return;
	}
	case 4:
		if (!L->IsMoviePlaying()) return;
		TestShownAt = Now;
		TestStep = 5;
		UE_LOG(LogOutlaws, Log, TEXT("OLSTATS test: the cinema is up (%s)"), Who);
		return;
	case 5:
		if (Now < TestShownAt + 4.0) return;
		if (bHost)
		{
			FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_stats_coop.png"), false, false);
			UE_LOG(LogOutlaws, Log, TEXT("OLSTATS test: screenshot ol_stats_coop.png"));
		}
		else if (OLFlags::Param(TEXT("OLHandCard")))
		{
			// (REQ-HANDSCORE: the client's own card too)
			FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_handcard_client.png"), false, false);
			UE_LOG(LogOutlaws, Log, TEXT("OLSTATS test: screenshot ol_handcard_client.png"));
		}
		LogPanels(TEXT("on the screen"));
		{
			FOutlawsStatsCard Card;
			if (HandCard(L, Card)) UE_LOG(LogOutlaws, Log, TEXT("OLTIMER coop test hand card (%s): %s"), Who, *Card.Lines());
			UE_LOG(LogOutlaws, Log, TEXT("OLTIMER coop test (%s): own clock %d s, the card's time %s, the pause line '%s'"), Who, Mine.Seconds, *TimeSummary(L), *PauseLine(L));
		}
		TestStep = 6;
		return;
	case 6:
		// the client goes first (the host's screenshot is taken), then the host
		if (Now < TestShownAt + (bHost ? 9.0 : 7.0)) return;
		UE_LOG(LogOutlaws, Log, TEXT("OLSTATS test: done (%s)"), Who);
		TestStep = 7;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
