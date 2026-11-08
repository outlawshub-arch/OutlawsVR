// M4's standalone polish (PLANS.md "M4. Polish"; the user's list: "taunts, hit confirmation sounds, the original's
// multiplayer music").  Found in the player's own files:
// - Music: every level names a music script (LVT/LVB "MUSIC x.msc"); the 1.x multiplayer maps all said NULL, and olpatch4 gives
//   most of them one (drygulch, fort, granary, mltsimms, town_2p, civpow2, hargrove, orleans, thudtowr).  It ships
//   wilderns.msc that no wilderns level names, and the community maps are NULL nearly all: a multiplayer map (one with an .rcm)
//   left at NULL takes "<map>.msc" where there is one, else town.msc (the rotation the remaster gives its town maps).
// - Hit confirmation: the original has none for the shooter (only the struck one's own HIT_SOUND, at him); BULHIT.WAV, the
//   bullet striking, is played in the shooter's head, quietly and high for a hit, fuller for a kill.  ol.HitConfirm 0 turns
//   it off.
// - Taunts: oltaunt.lab / oltaunthd.lab; the original's ten slots (LOCAL.MSG 5000-5009) and the remaster's quick taunts
//   (KEY_ORD_QUICK_TAUNT_1..10, g_taunt1..10).  F1..F10 say slots 1..10 at the player; OutlawsSettings.ini Taunt1..Taunt10
//   name other wavs (any in the LABs, as g_tauntN does).

#include "OutlawsMpPolish.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsCoop.h"
#include "OutlawsEnemy.h"
#include "OutlawsLab.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "GameFramework/PlayerController.h"
#include "OutlawsStats.h"
#include "Camera/CameraComponent.h"
#include "Components/AudioComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "InputCoreTypes.h"

namespace
{
	TAutoConsoleVariable<int32> CVarOLVrTaunt(TEXT("ol.VrTaunt"), 1, TEXT("VR: the move stick held 0.6 s and let go says the next taunt slot (1), or nothing (0)."));
	TAutoConsoleVariable<int32> CVarOLHitConfirm(TEXT("ol.HitConfirm"), 1, TEXT("1: a tick in the head when this player's shot strikes a bad guy (fuller on a kill); 0: none."));

	// the original's ten (LOCAL.MSG 5000-5009, the remaster's g_taunt1..10 defaults)
	const TCHAR* const TauntDefaults[10] = { TEXT("c106"), TEXT("c209"), TEXT("c316"), TEXT("c418"), TEXT("c521"), TEXT("c652"), TEXT("c755"),
		TEXT("sz04"), TEXT("max"), TEXT("sm01") };
	constexpr TCHAR const* HitWav = TEXT("bulhit");

	// (the test's counts)
	int32 GHits = 0, GKills = 0, GTaunts = 0, GTauntsRefused = 0;
	bool GHitConfirmTest = false;         // (-OLM4Test runs in single player: the tick is let through for its check)
	FString GLastTaunt;
	// (the killing shot's own hit tick gives way to the kill's)
	TWeakObjectPtr<UAudioComponent> GHitVoice;
	uint64 GHitFrame = 0;

	FString CleanStem(const FString& Name)
	{
		FString S = FPaths::GetBaseFilename(Name.TrimStartAndEnd());
		return S.ToLower();
	}
}

bool OutlawsMpPolish::IsMultiplayerMap(const FOutlawsAssets* A, const FString& Level)
{
	if (!A || Level.IsEmpty()) return false;
	const FString Rcm = Level + TEXT(".rcm");
	return A->Has(Rcm) || FPaths::FileExists(FPaths::Combine(A->GetRoot(), Rcm));
}

FString OutlawsMpPolish::MusicFor(const FOutlawsAssets* A, const FString& Level, const FString& Msc)
{
	const bool bNone = Msc.IsEmpty() || Msc.Equals(TEXT("NULL"), ESearchCase::IgnoreCase);
	if (!bNone || !IsMultiplayerMap(A, Level)) return Msc;
	const FString Own = Level.ToLower() + TEXT(".msc");
	if (A->Has(Own)) return Own;
	return A->Has(TEXT("town.msc")) ? FString(TEXT("town.msc")) : Msc;
}

const TCHAR* const OutlawsMpPolish::ShuffleMusic = TEXT("*shuffle*");

bool OutlawsMpPolish::MusicMapIsMultiplayer(const FOutlawsAssets* A, const FString& Level)
{
	return IsMultiplayerMap(A, Level) || (A && !Level.IsEmpty() && !A->NativeLabFor(Level).IsEmpty());
}

FString OutlawsMpPolish::MpMusicFor(const FOutlawsAssets* A, const FString& Level, const FString& Msc, bool bMusicOn)
{
	if (!MusicMapIsMultiplayer(A, Level)) return Msc;
	if (!bMusicOn) return TEXT("NULL");
	const bool bNone = Msc.IsEmpty() || Msc.Equals(TEXT("NULL"), ESearchCase::IgnoreCase);
	if (!bNone) return Msc;
	const FString Own = Level.ToLower() + TEXT(".msc");
	return A->Has(Own) ? Own : FString(ShuffleMusic);
}

void OutlawsMpPolish::MusicTrackKeys(const FOutlawsAssets* A, TArray<int32>& Out)
{
	Out.Reset();
	if (!A) return;
	TArray<FString> Scripts;
	A->ListByExtension(TEXT("msc"), Scripts);
	TSet<int32> Seen;
	for (const FString& S : Scripts)
	{
		const FString Name = FPaths::GetCleanFilename(S).ToLower();
		if (Name == TEXT("intro.msc") || Name == TEXT("credits.msc")) continue;
		FString Text;
		if (!A->GetText(S, Text)) continue;
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			TArray<FString> T;
			Line.ParseIntoArrayWS(T);
			if (T.Num() > 2 && T[0].Equals(TEXT("CD_TRACK"), ESearchCase::IgnoreCase))
			{
				const int32 Track = FCString::Atoi(*T[2]);
				if (Track > 0) Seen.Add(Track * 2 + (T[1].EndsWith(TEXT("2")) ? 1 : 0));      // (the label's last character picks the disc, as StartMusic)
			}
		}
	}
	Out = Seen.Array();
	Out.Sort();
}

void OutlawsMpPolish::ShuffleDeck(int32 N, int32 AvoidFirst, FRandomStream& R, TArray<int32>& Out)
{
	Out.Reset(N);
	for (int32 i = 0; i < N; ++i) Out.Add(i);
	for (int32 i = N - 1; i > 0; --i) Out.Swap(i, R.RandRange(0, i));
	if (N > 1 && Out[0] == AvoidFirst) Out.Swap(0, 1 + R.RandRange(0, N - 2));
}

FString OutlawsMpPolish::LevelMusic(const AOutlawsLevelActor* L, const FString& Msc)
{
	if (!L) return Msc;
	if (OLFlags::Param(TEXT("OLOldMpMusic")))
	{
		const FString Old = MusicFor(L->GetLabAssets(), L->LevelName, Msc);
		if (Old != Msc) UE_LOG(LogOutlaws, Log, TEXT("OLM4 music: %s is a multiplayer map with no music of its own: %s (-OLOldMpMusic)"), *L->LevelName, *Old);
		return Old;
	}
	const FString Out = MpMusicFor(L->GetLabAssets(), L->LevelName, Msc, L->bMpMusic);
	if (Out != Msc) UE_LOG(LogOutlaws, Log, TEXT("OLM4 music: %s is a multiplayer map, music %s: %s"), *L->LevelName, L->bMpMusic ? TEXT("on") : TEXT("off"), *Out);
	return Out;
}

void OutlawsMpPolish::HitConfirm(AOutlawsLevelActor* L, bool bKill)
{
	if (!L || CVarOLHitConfirm.GetValueOnGameThread() == 0) return;
	// (BUG-ENEMYCLUNK, user 2026-10-14) a multiplayer aid only: in single player this full-pitch BULHIT on every kill was the
	// "clunk" heard on bad guys killed by a fall, a blast or a thrown knife; the original has no tick for the shooter at all
	const UWorld* W = L->GetWorld();
	static const bool bAnyMode = OLFlags::Param(TEXT("OLHitConfirmSP"));     // (tests: the tick in single player too)
	if (!GHitConfirmTest && !bAnyMode && (!W || W->GetNetMode() == NM_Standalone)) return;
	(bKill ? GKills : GHits)++;
	if (bKill && GHitFrame == GFrameCounter && GHitVoice.IsValid()) GHitVoice->Stop();
	UAudioComponent* A = L->PlayOnce2D(HitWav);
	if (!bKill) { GHitVoice = A; GHitFrame = GFrameCounter; }
	if (A)
	{
		A->SetVolumeMultiplier(bKill ? 0.8f : 0.45f);
		A->SetPitchMultiplier(bKill ? 1.0f : 1.6f);
	}
	if (GHits + GKills <= 20)
	{
		const FOutlawsAssets* As = L->GetLabAssets();
		UE_LOG(LogOutlaws, Log, TEXT("OLM4 hit confirm: %s (%s.wav %s, %s)"), bKill ? TEXT("a kill") : TEXT("a hit"), HitWav,
			As && As->Has(FString(HitWav) + TEXT(".wav")) ? TEXT("in the LABs") : TEXT("MISSING"), A ? TEXT("playing") : TEXT("no voice"));
	}
}

FString OutlawsMpPolish::TauntSlotWav(int32 Slot)
{
	if (Slot < 1 || Slot > 10) return FString();
	const TMap<FString, FString> S = AOutlawsLevelActor::ReadSettings();
	if (const FString* X = S.Find(FString::Printf(TEXT("Taunt%d"), Slot)))
	{
		const FString Stem = CleanStem(*X);
		if (!Stem.IsEmpty()) return Stem;
	}
	return TauntDefaults[Slot - 1];
}

bool OutlawsMpPolish::Taunt(AOutlawsLevelActor* L, int32 Slot)
{
	if (!L || L->IsFrontEnd()) return false;
	const FString Wav = TauntSlotWav(Slot);
	if (Wav.IsEmpty()) return false;
	if (L->IsTauntPlaying())
	{
		++GTauntsRefused;
		UE_LOG(LogOutlaws, Log, TEXT("OLM4 taunt %d (%s): one still plays"), Slot, *Wav);
		return false;
	}
	APawn* P = UGameplayStatics::GetPlayerPawn(L, 0);
	const UCameraComponent* Cam = P ? P->FindComponentByClass<UCameraComponent>() : nullptr;
	const FVector At = Cam ? Cam->GetComponentLocation() : P ? P->GetActorLocation() : FVector::ZeroVector;
	// (said by the player, heard as the player's own voice: in the head; the bad guys' taunts keep their places)
	UAudioComponent* A = L->PlayOnce2D(Wav);
	L->TauntPlaying = A;
	++GTaunts;
	GLastTaunt = Wav;
	const FOutlawsAssets* As = L->GetLabAssets();
	// (co-op: heard here only for now; the session carries no taunt until M1's)
	UE_LOG(LogOutlaws, Log, TEXT("OLM4 taunt %d: %s.wav (%s, %s) at %s%s"), Slot, *Wav, As && As->Has(Wav + TEXT(".wav")) ? TEXT("in the LABs") : TEXT("MISSING"),
		A ? TEXT("playing") : TEXT("no voice"), *At.ToCompactString(), UOutlawsCoop::IsActive(L) ? TEXT(" (to the other players)") : TEXT(""));
	UOutlawsCoop::NoteTaunt(L, Wav);       // (M4: heard by the other players, from where this player stands)
	return true;
}

bool OutlawsMpPolish::VrTauntPress(AOutlawsLevelActor* L, double HeldSeconds)
{
	if (!L || CVarOLVrTaunt.GetValueOnGameThread() == 0 || HeldSeconds < 0.6) return false;
	static int32 Next = 0;
	const int32 Slot = 1 + (Next++ % 10);
	UE_LOG(LogOutlaws, Log, TEXT("OLM4 VR taunt: the move stick held %.1f s: slot %d"), HeldSeconds, Slot);
	Taunt(L, Slot);
	return true;
}

void AOutlawsLevelActor::OnTauntPressed(FKey Key)
{
	static const FKey Keys[10] = { EKeys::F1, EKeys::F2, EKeys::F3, EKeys::F4, EKeys::F5, EKeys::F6, EKeys::F7, EKeys::F8, EKeys::F9, EKeys::F10 };
	for (int32 i = 0; i < 10; ++i) if (Key == Keys[i]) { OutlawsMpPolish::Taunt(this, i + 1); return; }
}

void AOutlawsLevelActor::OnScoreboardPressed()
{
	UWorld* W = GetWorld();
	APlayerController* PC = W ? W->GetFirstPlayerController() : nullptr;
	if (AOutlawsPlayer* P = PC ? Cast<AOutlawsPlayer>(PC->GetPawn()) : nullptr) UOutlawsCoop::ToggleScoreboard(P, TEXT("Tab"));
}

// ---- -OLM4Test ---------------------------------------------------------------------------------------------------------

bool UOutlawsMpPolishTest::ShouldCreateSubsystem(UObject* Outer) const
{
	return OLFlags::Param(TEXT("OLM4Test")) || OLFlags::Param(TEXT("OLMpMusicProbe"));
}

// (REQ-MPMUSIC) -OLMpMusicProbe: the rule on the install's own files, the decks, and this level's music as it is
static void MpMusicProbe(AOutlawsLevelActor* L)
{
	int32 Passes = 0, Fails = 0;
	auto Check = [&](bool bOk, const FString& What)
	{
		++(bOk ? Passes : Fails);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mpmusic check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
	};
	const FOutlawsAssets* A = L->GetLabAssets();
	using namespace OutlawsMpPolish;
	Check(MpMusicFor(A, TEXT("hideout"), TEXT("hideout.msc"), false) == TEXT("hideout.msc"), TEXT("a story level keeps its music with the row off"));
	Check(MpMusicFor(A, TEXT("drygulch"), TEXT("drygulch.msc"), true) == TEXT("drygulch.msc"), TEXT("a multiplayer map's own script is kept"));
	Check(MpMusicFor(A, TEXT("drygulch"), TEXT("drygulch.msc"), false) == TEXT("NULL"), TEXT("the row off: a multiplayer map's own script is not played"));
	Check(MpMusicFor(A, TEXT("wilderns"), TEXT("NULL"), true) == TEXT("wilderns.msc"), TEXT("wilderns at NULL keeps the remaster's wilderns.msc"));
	TArray<int32> Keys;
	MusicTrackKeys(A, Keys);
	FString Names;
	for (int32 K : Keys) Names += FString::Printf(TEXT(" TRK%02d%c"), K / 2, (K & 1) ? TEXT('B') : TEXT('A'));
	Check(Keys.Num() >= 6, FString::Printf(TEXT("the track list from the LABs' scripts: %d tracks:%s"), Keys.Num(), *Names));
	// the decks: three deals of the list, each every track once, the next's first not the last heard
	FRandomStream R(1234);
	bool bDecks = Keys.Num() > 1;
	int32 Last = -1;
	for (int32 d = 0; d < 3 && bDecks; ++d)
	{
		TArray<int32> Order;
		ShuffleDeck(Keys.Num(), Last, R, Order);
		TSet<int32> Seen(Order);
		bDecks = Order.Num() == Keys.Num() && Seen.Num() == Keys.Num() && Order[0] != Last;
		Last = Order.Last();
	}
	Check(bDecks, TEXT("three shuffled decks: every track once a deck, no deck starting on the track just heard"));
	// this level, as the game started its music
	const bool bMp = MusicMapIsMultiplayer(A, L->LevelName);
	const FString Rule = MpMusicFor(A, L->LevelName, L->LevelMusicScript, L->bMpMusic);
	const bool bShuffle = Rule == ShuffleMusic;
	Check(L->MusicIsShuffle() == bShuffle && (!bShuffle || L->MusicDeckSize() == Keys.Num()),
		FString::Printf(TEXT("%s (%s, MUSIC %s, row %s): %s, %d cues%s"), *L->LevelName, bMp ? TEXT("a multiplayer map") : TEXT("a story level"),
			*L->LevelMusicScript, L->bMpMusic ? TEXT("on") : TEXT("off"), L->MusicIsShuffle() ? TEXT("shuffled") : TEXT("not shuffled"),
			L->MusicCueCount(), bShuffle ? *FString::Printf(TEXT(", want the shuffle of %d"), Keys.Num()) : TEXT("")));
	if (bMp && !L->bMpMusic) Check(L->MusicCueCount() == 0, TEXT("the row off: no music in the match"));
	if (bMp)
	{
		// the row turned over and back, as the Multiplayer page does
		const bool bWas = L->bMpMusic;
		L->bMpMusic = !bWas;
		L->RestartLevelMusic();
		const int32 Turned = L->MusicCueCount();
		L->bMpMusic = bWas;
		L->RestartLevelMusic();
		Check(bWas ? Turned == 0 : Turned > 0, FString::Printf(TEXT("the row turned %s in the match: %d cues"), bWas ? TEXT("off") : TEXT("on"), Turned));
		Check(L->MusicIsShuffle() == bShuffle, TEXT("the row turned back: the music as it was"));
	}
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mpmusic RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
}

UWorld* UOutlawsMpPolishTest::GetTickableGameObjectWorld() const
{
	const UGameInstance* GI = GetGameInstance();
	return GI ? GI->GetWorld() : nullptr;
}

TStatId UOutlawsMpPolishTest::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UOutlawsMpPolishTest, STATGROUP_Tickables);
}

void UOutlawsMpPolishTest::Tick(float DeltaTime)
{
	UWorld* W = GetTickableGameObjectWorld();
	if (!W || Step >= 99) return;
	TActorIterator<AOutlawsLevelActor> It(W);
	AOutlawsLevelActor* L = It ? *It : nullptr;
	APawn* P = UGameplayStatics::GetPlayerPawn(W, 0);
	if (!L || !P || L->IsFrontEnd() || L->IsRebuildPending() || !L->HasActorBegunPlay()) return;
	const double Now = FPlatformTime::Seconds();
	auto Check = [this](bool bOk, const FString& What)
	{
		if (!bOk) ++Bad;
		UE_LOG(LogOutlaws, Log, TEXT("OLM4 test: %s: %s"), *What, bOk ? TEXT("ok") : TEXT("BAD"));
	};
	if (OLFlags::Param(TEXT("OLMpMusicProbe")))
	{
		if (Step == 0) { At = Now + 6.0; Step = 1; return; }
		if (Now < At) return;
		MpMusicProbe(L);
		Step = 99;
		FGenericPlatformMisc::RequestExit(false);
		return;
	}
	switch (Step)
	{
	case 0:
		At = Now + 4.0;
		Step = 1;
		UE_LOG(LogOutlaws, Log, TEXT("OLM4 test: %s is up"), *L->LevelName);
		return;
	case 1:
	{
		if (Now < At) return;
		// the music choice for a story level, the multiplayer map with an unused script, and a community map at NULL
		const FOutlawsAssets* A = L->GetLabAssets();
		Check(OutlawsMpPolish::MusicFor(A, TEXT("hideout"), TEXT("NULL")) == TEXT("NULL"), TEXT("a story level at NULL stays quiet"));
		Check(OutlawsMpPolish::MusicFor(A, TEXT("drygulch"), TEXT("drygulch.msc")) == TEXT("drygulch.msc"), TEXT("a map's own script is kept"));
		Check(OutlawsMpPolish::MusicFor(A, TEXT("wilderns"), TEXT("NULL")) == TEXT("wilderns.msc"), TEXT("wilderns at NULL takes wilderns.msc"));
		// (the first community map in the install: <stem>.rcm beside the LABs)
		TArray<FString> Rcms;
		if (A) IFileManager::Get().FindFiles(Rcms, *FPaths::Combine(A->GetRoot(), TEXT("*.rcm")), true, false);
		FString Community;
		for (const FString& R : Rcms)
		{
			const FString Stem = FPaths::GetBaseFilename(R).ToLower();
			if (!A->Has(Stem + TEXT(".msc"))) { Community = Stem; break; }
		}
		if (!Community.IsEmpty()) Check(OutlawsMpPolish::MusicFor(A, Community, TEXT("NULL")) == TEXT("town.msc"), FString::Printf(TEXT("the community map %s at NULL takes town.msc (of %d .rcm)"), *Community, Rcms.Num()));
		UE_LOG(LogOutlaws, Log, TEXT("OLM4 test: this level %s plays %s"), *L->LevelName, *OutlawsMpPolish::LevelMusic(L, TEXT("NULL")));
		At = Now + 0.5;
		Step = 2;
		return;
	}
	case 2:
	{
		if (Now < At) return;
		// a shot (as the player's own: its frame) into the nearest bad guy, twice: a hit, then his death
		AOutlawsEnemy* Best = nullptr;
		for (const TWeakObjectPtr<AOutlawsEnemy>& WE : L->GetEnemies())
		{
			AOutlawsEnemy* E = WE.Get();
			if (!E || E->IsDead() || E->bVoiceOnly || E->bNotBadGuy || E->bBoss || E->IsDormant()) continue;
			if (!Best || FVector::DistSquared(E->GetActorLocation(), P->GetActorLocation()) < FVector::DistSquared(Best->GetActorLocation(), P->GetActorLocation())) Best = E;
		}
		const int32 H0 = GHits, K0 = GKills;
		GHitConfirmTest = true;
		if (Best)
		{
			const FVector From = P->GetActorLocation();
			UOutlawsStats::NoteShotFired(L);
			Best->TakeHit(1.f, Best->GetActorLocation(), (Best->GetActorLocation() - From).GetSafeNormal(), From);
			UOutlawsStats::NoteShotFired(L);
			Best->TakeHit(500.f, Best->GetActorLocation(), (Best->GetActorLocation() - From).GetSafeNormal(), From);
		}
		GHitConfirmTest = false;
		Check(Best != nullptr, TEXT("a bad guy to shoot"));
		Check(GHits > H0, FString::Printf(TEXT("the hit confirmed (%d)"), GHits - H0));
		Check(GKills > K0, FString::Printf(TEXT("the kill confirmed (%d)"), GKills - K0));
		At = Now + 0.5;
		Step = 3;
		return;
	}
	case 3:
	{
		if (Now < At) return;
		// the key's own path (F1), then the other slots' wavs found in the LABs
		const int32 T0 = GTaunts;
		L->OnTauntPressed(EKeys::F1);
		Check(GTaunts == T0 + 1 && GLastTaunt == TEXT("c106"), TEXT("F1 says slot 1 (c106)"));
		const FOutlawsAssets* A = L->GetLabAssets();
		int32 Found = 0;
		for (int32 s = 1; s <= 10; ++s) if (A && A->Has(OutlawsMpPolish::TauntSlotWav(s) + TEXT(".wav"))) ++Found;
		Check(Found == 10, FString::Printf(TEXT("%d of the 10 slots' wavs in the LABs"), Found));
		// (with a voice: a second press while it plays is refused; -nosound has no voice to wait on)
		if (L->IsTauntPlaying()) Check(!OutlawsMpPolish::Taunt(L, 2) && GTauntsRefused > 0, TEXT("a second taunt waits for the first"));
		At = Now + 0.5;
		Step = 4;
		return;
	}
	default:
		if (Now < At) return;
		UE_LOG(LogOutlaws, Log, TEXT("OLM4 test: done, %d bad"), Bad);
		Step = 99;
		FGenericPlatformMisc::RequestExit(false);
		return;
	}
}
