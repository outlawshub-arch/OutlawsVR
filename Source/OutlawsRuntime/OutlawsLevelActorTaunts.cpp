// BUG-TAUNTRATE (user, 21 Oct 2026, Quest): "Where are you, Marshal?" and the rest were barely heard any more.  The rate is
// per player, not per bad guy: while any bad guys hunt, one taunt every 10 to 15 s (20 at most), the same with one bad guy
// as with the 4x Bad guys multiplier.  One clock for the player listening on this machine; the clock decides only when.
// Who says it: a real hunting bad guy (chasing, or flinching from a hit, on the host's side for a co-op client's copies)
// within earshot of this machine's eye, not the one who said the last when there is another; his line from his own lists
// as the original chooses them, played 3D at his eyes with the usual falloff, so the player can tell which way he is.
//
// The original [EXE 0x14008eeb0, 0x14008bbdc; research/outlaws_enemy_exe.md 7]: each bad guy every TAUNT_TIMER_TIME
// (25 s, 15 s for most named ones) +-4 s, then a coin flip and one level-wide gap of 1.9..10 s, the player within 200 units.
// BUG-TAUNTS (3 Oct) doubled each bad guy's timer and kept the coin flip: about one taunt a bad guy every 100 s, and only
// from the few thinking every frame (the CPU2 throttle's WANDER and FROZEN tiers skip the combat hook that ran the timer),
// so a fight with two or three of them went a minute or two without a word; and one said from 40 to 60 m (inside the
// original's 200 units) took the level-wide gap but was never started (ol.FarSoundDistance, 40 m: beyond the attenuation).
// -OLOldTauntRate puts those timers back.
//
// Co-op: every machine runs its own clock for its own player and plays the line itself (the host's bad guys' copies on a
// client know their state from the host's snapshots); nothing goes over the link, so two players never double it.
// The boss's own taunts (UpdateBoss, "OLRT boss taunt", before he appears) keep their own clock; they and these never play
// over each other (TauntPlaying), and a clock taunt due while one plays waits for it to end.
#include "OutlawsLevelActor.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsEnemy.h"
#include "OutlawsCoop.h"
#include "Components/AudioComponent.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/App.h"

namespace
{
	TAutoConsoleVariable<float> CVarOLTauntGapMin(TEXT("ol.TauntGapMin"), 10.f, TEXT("Bad guys' taunts: least seconds between two for this machine's player (BUG-TAUNTRATE)."));
	TAutoConsoleVariable<float> CVarOLTauntGapMax(TEXT("ol.TauntGapMax"), 15.f, TEXT("Bad guys' taunts: most seconds between two while one in earshot hunts (BUG-TAUNTRATE)."));
	TAutoConsoleVariable<float> CVarOLTauntEarshot(TEXT("ol.TauntEarshot"), 3500.f, TEXT("Bad guys' taunts: a speaker within this many cm of the eye (the far-sound cull is the limit too)."));

	constexpr float LookEvery = 0.25f;         // the hunters looked over four times a second

	/** Hunting: alive and awake, chasing or flinching (a client's copy: the host's state), a bad guy with a line to say. */
	bool Hunts(const AOutlawsEnemy* E)
	{
		if (!E || E->IsDead() || E->IsDormant() || E->CoopSelf >= 0 || E->IsCritter()) return false;
		const EOutlawsEnemyState S = E->GetState();
		if (S != EOutlawsEnemyState::Chase && S != EOutlawsEnemyState::Shoot && S != EOutlawsEnemyState::Hit) return false;
		return E->Def.LosTaunts.Num() > 0 || E->Def.NoLosTaunts.Num() > 0 || E->Def.AlertTaunts.Num() > 0;
	}

	float Gap()
	{
		const float Lo = FMath::Max(1.f, CVarOLTauntGapMin.GetValueOnGameThread());
		return FMath::FRandRange(Lo, FMath::Max(Lo, CVarOLTauntGapMax.GetValueOnGameThread()));
	}
}

bool AOutlawsLevelActor::OldTauntRate()
{
	static const bool bOld = OLFlags::Param(TEXT("OLOldTauntRate"));
	return bOld;
}

#if WITH_OUTLAWS_TESTS
// -OLTauntRateTest [-OLTauntSec=120] (BUG-TAUNTRATE), single player (-OLLevel=<it> -OLNoFrontEnd [-OLSoloBadGuys=4]) or both
// co-op machines (-OLCoopReady -OLCoopBench=170, the host's -OLCoopLevel and -OLCoopBadGuys): the player cannot be hurt and
// stands still;
// at 3 s every living bad guy is woken (the host's or single player's; a client's copies follow the host's), then from
// 20 s for OLTauntSec s this machine's taunts are counted: per minute (expect 4 to 6), the gaps (none under 10 s), the
// speakers (how many, never the last one again with another in earshot) and the sound's place against the speaker's eyes.
// -OLTauntOne: only the nearest bad guy hunts (the rest dormant), for one bad guy against the crowd.
// The result is one "OLSHOT tauntrate RESULT" line; single player quits then (co-op: when the bench ends).
static void TauntRateTest(AOutlawsLevelActor* L, float Dt)
{
	static float T = 0.f, NextLog = 0.f;
	static bool bWoke = false, bDone = false;
	static int32 Repeat0 = -1;
	static float Sec = 120.f;
	static const bool bOn = OLFlags::Param(TEXT("OLTauntRateTest"));
	static const bool bOne = OLFlags::Param(TEXT("OLTauntOne"));
	if (!bOn || bDone) return;
	UWorld* World = L->GetWorld();
	if (!World || !World->GetFirstPlayerController() || !World->GetFirstPlayerController()->GetPawn()) return;
	if (T == 0.f) OLFlags::Value(TEXT("OLTauntSec"), Sec);
	L->bInvincible = true;
	L->bEnemiesActive = true;
	T += Dt;
	const UOutlawsCoop* C = UOutlawsCoop::Get(L);
	const bool bClient = C && C->IsClient();
	const TCHAR* Me = bClient ? TEXT("client") : C && UOutlawsCoop::IsActive(L) ? TEXT("host") : TEXT("single player");
	constexpr float From = 20.f;
	if (!bWoke && T > 3.f && !bClient)
	{
		bWoke = true;
		int32 N = 0;
		// -OLTauntOne: one bad guy alone (the nearest; the rest put away, dormant), to set against the crowd
		AOutlawsEnemy* Nearest = nullptr;
		const FVector P = World->GetFirstPlayerController()->GetPawn()->GetActorLocation();
		for (const TWeakObjectPtr<AOutlawsEnemy>& W : L->GetEnemies())
		{
			AOutlawsEnemy* E = W.Get();
			if (bOne && E && !E->IsDead() && !E->IsDormant() && !E->IsVoiceOnly() && E->CoopSelf < 0 && E->Def.NoLosTaunts.Num() > 0 && !E->IsCritter() && !E->bNotBadGuy
				&& (!Nearest || FVector::DistSquared(E->GetActorLocation(), P) < FVector::DistSquared(Nearest->GetActorLocation(), P))) Nearest = E;
		}
		for (const TWeakObjectPtr<AOutlawsEnemy>& W : L->GetEnemies())
		{
			AOutlawsEnemy* E = W.Get();
			if (!E || E->IsDead() || E->IsDormant() || E->IsVoiceOnly() || E->CoopSelf >= 0) continue;
			if (bOne && E != Nearest) { E->SetDormant(true); continue; }
			E->Wake(false);
			++N;
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT tauntrate (%s) woke %d of %d bad guys"), Me, N, L->GetEnemies().Num());
	}
	if (T >= From && Repeat0 < 0) Repeat0 = L->TauntsRepeatSpeaker;
	if (T >= NextLog)
	{
		NextLog = T + 10.f;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT tauntrate (%s) t=%.0f: %d said (%d by the clock), %d hunting, %d in earshot"), Me, T, L->TauntsSaid, L->TauntsByTimer, L->TauntsHunters, L->TauntsInEar);
	}
	if (T < From + Sec) return;
	bDone = true;
	const double Now = World->GetTimeSeconds();
	const double W0 = Now - (T - From), W1 = W0 + Sec;
	int32 N = 0;
	double MinGap = 1e9, MaxGap = 0.0, Prev = -1.0;
	TSet<const AOutlawsEnemy*> Who;
	for (int32 i = 0; i < L->TauntTimes.Num(); ++i)
	{
		const double At = L->TauntTimes[i];
		if (At < W0 || At > W1) { Prev = At; continue; }
		++N;
		Who.Add(L->TauntSpeakers[i].Get());
		if (Prev >= 0.0) { MinGap = FMath::Min(MinGap, At - Prev); MaxGap = FMath::Max(MaxGap, At - Prev); }
		Prev = At;
	}
	const double PerMin = N / (Sec / 60.0);
	const int32 Repeats = L->TauntsRepeatSpeaker - FMath::Max(0, Repeat0);
	const bool bOk = PerMin >= 3.5 && PerMin <= 6.5 && MinGap >= 9.9 && Repeats == 0 && L->TauntsRepeatLine == 0 && (N < 2 || Who.Num() >= 2 || bOne) && L->TauntPosErrMax <= 1.0;
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT tauntrate RESULT %s (%s, %s, bad guys x%.2f): %d taunts in %.0f s = %.1f a minute (expect 4 to 6); gaps %.1f to %.1f s; %d speakers, %d the same twice running, %d the same kind twice running, %d the same line twice running; %s; %d hunting, %d in earshot at the end"),
		bOk ? TEXT("PASS") : TEXT("FAIL"), Me, *L->LevelName, C && UOutlawsCoop::IsActive(L) ? C->BadGuysMultiplier() : L->SoloBadGuysWanted(), N, Sec, PerMin,
		MinGap > 1e8 ? 0.0 : MinGap, MaxGap, Who.Num(), Repeats, L->TauntsRepeatVoice, L->TauntsRepeatLine,
		L->TauntPosErrMax > 0.0 || FApp::CanEverRenderAudio() ? *FString::Printf(TEXT("sound at most %.1f cm from the speaker's eyes"), L->TauntPosErrMax) : TEXT("no audio device (-nosound): played at the speaker's eyes by PlaySoundAt(EyePoint)"),
		L->TauntsHunters, L->TauntsInEar);
	if (!C || !UOutlawsCoop::IsActive(L)) FGenericPlatformMisc::RequestExit(false);      // (co-op: -OLCoopBench quits both)
}
#endif

void AOutlawsLevelActor::TickTaunts(float Dt)
{
#if WITH_OUTLAWS_TESTS
	TauntRateTest(this, Dt);
#endif
	if (OldTauntRate() || !AreEnemiesActive() || bBackdrop) return;
	TauntLook -= Dt;
	if (TauntLook > 0.f) return;
	TauntLook = LookEvery;
	UWorld* World = GetWorld();
	const APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC || !PC->PlayerCameraManager) return;
	const double Now = World->GetTimeSeconds();
	const FVector Ear = PC->PlayerCameraManager->GetCameraLocation();
	const float Far = []{ static IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(TEXT("ol.FarSoundDistance")); return V ? V->GetFloat() : 0.f; }();
	const double Reach = Far > 0.f ? FMath::Min(double(CVarOLTauntEarshot.GetValueOnGameThread()), double(Far)) : double(CVarOLTauntEarshot.GetValueOnGameThread());
	int32 Hunters = 0;
	TArray<TPair<AOutlawsEnemy*, double>, TInlineAllocator<32>> InEar;
	for (const TWeakObjectPtr<AOutlawsEnemy>& W : Enemies)
	{
		AOutlawsEnemy* E = W.Get();
		if (!Hunts(E)) continue;
		++Hunters;
		const double D = FVector::Dist(E->EyePoint(), Ear);
		if (D <= Reach) InEar.Emplace(E, D);
	}
	TauntsHunters = Hunters;
	TauntsInEar = InEar.Num();
	if (Hunters == 0)
	{
		TauntDueAt = -1.0;                 // nobody after the player: the first comes 10..15 s after the hunt begins
		return;
	}
	if (TauntDueAt < 0.0) TauntDueAt = FMath::Max(Now + Gap(), TauntLastAt + CVarOLTauntGapMin.GetValueOnGameThread());
	if (Now < TauntDueAt || InEar.Num() == 0 || IsTauntPlaying()) return;      // (due: said as soon as one is in earshot)
	// the speaker: any of them, not the last one again while there is another, and a different voice (another kind of bad
	// guy: the x4 copies share their kind's lines) when one is in earshot
	const AOutlawsEnemy* Last = TauntLastSpeaker.Get();
	TArray<int32, TInlineAllocator<32>> Others, OtherVoices;
	for (int32 i = 0; i < InEar.Num(); ++i)
	{
		if (InEar[i].Key == Last) continue;
		Others.Add(i);
		if (!Last || InEar[i].Key->GetOutlawsName() != Last->GetOutlawsName()) OtherVoices.Add(i);
	}
	const TArray<int32, TInlineAllocator<32>>& From = OtherVoices.Num() > 0 ? OtherVoices : Others;
	const int32 Pick = From.Num() > 0 ? From[FMath::RandHelper(From.Num())] : 0;
	AOutlawsEnemy* E = InEar[Pick].Key;
	// his line as the original chooses it [EXE 0x14008eeb0]: not seeing the player, NO_LOS; seeing, LOS, or half the time
	// ALERT when his last sight is over 10 s old.  The host's (or single player's) own bad guy after this player knows
	// his sight; a co-op client's copy (or one after the other player) looks from his eyes to this eye
	const bool bMine = E->CoopRole != 2 && E->CoopTarget == 0;
	const bool bSees = bMine ? E->bSeen : E->CanSeeFrom(E->EyePoint(), Ear);
	const bool bOld = bMine && E->Now() - E->LastSeenTime > 10.0;
	const TArray<FString>* List = !bSees ? &E->Def.NoLosTaunts : (bOld && FMath::RandBool() ? &E->Def.AlertTaunts : &E->Def.LosTaunts);
	if (List->Num() == 0) List = E->Def.NoLosTaunts.Num() > 0 ? &E->Def.NoLosTaunts : E->Def.LosTaunts.Num() > 0 ? &E->Def.LosTaunts : &E->Def.AlertTaunts;
	++TauntsByTimer;
	SayTauntFrom(E, *List, !bSees ? TEXT("timer, unseen") : List == &E->Def.AlertTaunts ? TEXT("timer, alert") : TEXT("timer, in sight"), InEar[Pick].Value, Now);
}

bool AOutlawsLevelActor::EventTaunt(AOutlawsEnemy* E, const TArray<FString>& List, const TCHAR* Why)
{
	UWorld* World = GetWorld();
	const APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!E || List.Num() == 0 || !PC || !PC->PlayerCameraManager || IsTauntPlaying()) return false;
	const double Now = World->GetTimeSeconds();
	if (Now - TauntLastAt < CVarOLTauntGapMin.GetValueOnGameThread()) return false;      // (the clock's least gap holds for these too)
	const double D = FVector::Dist(E->EyePoint(), PC->PlayerCameraManager->GetCameraLocation());
	const float Far = []{ static IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(TEXT("ol.FarSoundDistance")); return V ? V->GetFloat() : 0.f; }();
	if (D > CVarOLTauntEarshot.GetValueOnGameThread() || (Far > 0.f && D > Far)) return false;
	SayTauntFrom(E, List, Why, D, Now);
	return true;
}

void AOutlawsLevelActor::SayTauntFrom(AOutlawsEnemy* E, const TArray<FString>& List, const TCHAR* Why, double DistCm, double Now)
{
	FString Line;
	const FString PrevLine = TauntLastLine;
	const AOutlawsEnemy* Prev = TauntLastSpeaker.Get();
	UAudioComponent* A = E->SayLine(List, Line);
	const double Since = Now - TauntLastAt;
	const bool bRepeat = Prev == E && TauntsInEar > 1;
	TauntsRepeatSpeaker += bRepeat ? 1 : 0;
	TauntsRepeatLine += Line == PrevLine ? 1 : 0;
	TauntsRepeatVoice += Prev && Prev->GetOutlawsName() == E->GetOutlawsName() ? 1 : 0;
	if (A) TauntPosErrMax = FMath::Max(TauntPosErrMax, FVector::Dist(A->GetComponentLocation(), E->EyePoint()));
	TauntLastAt = Now;
	TauntLastSpeaker = E;
	TauntDueAt = Now + Gap();
	++TauntsSaid;
#if WITH_OUTLAWS_TESTS
	TauntTimes.Add(Now);
	TauntSpeakers.Add(E);
#endif
	UE_LOG(LogOutlawsAI, Log, TEXT("OLRT taunt %d (%s): %s says %s at %.1f m; %d hunting, %d in earshot; %.1f s since the last, next due in %.1f s%s%s"),
		TauntsSaid, Why, *E->GetOutlawsName(), *Line, DistCm / 100.0, TauntsHunters, TauntsInEar, Since > 1000.0 ? -1.0 : Since, TauntDueAt - Now,
		bRepeat ? TEXT(", the same speaker again") : TEXT(""), A ? TEXT("") : TEXT(" (no sound)"));
}
