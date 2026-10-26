// M1, the match side: Death Match's rules (PLANS.md "M1. Skeleton" items 4 to 6, the characters from M4's decided list).
// From the player's own files:
// - The six characters: cidsanch / cidjames / cidfeath / ciddeath / cidbob / cidmary.itm (olobj, olpatch4), each naming its
//   PLAYER_WAX sprite set, SND_DIE and DAMAGE_MULTIPLIER; in the character page's order (lobby_character.rml ol-char 1..6).
// - Spawn points: the maps' STARTPOS objects (drygulch has 8, town_2p 14; the exe's level_LoadObjects knows "startpos");
//   the one farthest from every living player is used.  A map with none falls back to its PLAYER start.
// - Pickups in a multiplayer map are generators (N* objects, FUNC Gen_New, GENERATE the item, FREQ): FREQ negative is a fixed
//   period in seconds, positive a rate a minute (as the sound generators read; INFERRED: the ITMs' own comments are stale).
//   nrifle1 -30 (30 s), nsgun1 -55, ngatgun -180, nbplate1 3 (20 s)...
// - No respawn delay or spawn protection exists in the original's data or its exe's settings (it respawns on a key press:
//   "Press <KEY> to Respawn"); the delays here are ours, in FOutlawsDmConfig.  mp_killsToEnd and mp_timeToEnd are the limits.

#include "OutlawsDeathmatch.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsFormats.h"
#include "OutlawsGeometry.h"
#include "OutlawsLab.h"
#include "OutlawsLevel.h"
#include "OutlawsLevelActor.h"
#include "Camera/CameraComponent.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

// ---- the characters -----------------------------------------------------------------------------------------------------

namespace
{
	const FOutlawsMpCharacter GMpCharacters[] = {
		{ TEXT("San"), TEXT("Spit'n Jack"), TEXT("cidsanch.itm"), TEXT("sanchez.nwx"), TEXT("szdie"), 1.15f, 0u, 2 },      // (REQ-SANCHEZDUAL: his pair; the ITM gives one)
		{ TEXT("Jam"), TEXT("James Anderson"), TEXT("cidjames.itm"), TEXT("james.nwx"), TEXT("jadie"), 1.0f, 0u, 0 },
		{ TEXT("Two"), TEXT("Two-Feathers"), TEXT("cidfeath.itm"), TEXT("twofeath.nwx"), TEXT("tfdie"), 0.90f, 0u, 0 },
		{ TEXT("Doc"), TEXT("Dr. Death"), TEXT("ciddeath.itm"), TEXT("drdeath.nwx"), TEXT("drdie"), 1.05f, 0u, 0 },
		{ TEXT("Bob"), TEXT("Bob Graham"), TEXT("cidbob.itm"), TEXT("bobgrahm.nwx"), TEXT("bodie"), 0.95f, 0u, 0 },
		{ TEXT("Mar"), TEXT("'Bloody' Mary"), TEXT("cidmary.itm"), TEXT("mary.nwx"), TEXT("bmdie"), 0.80f, 0xA8141Eu, 0 } };      // (REQ-MARYNAILS: Bloody Mary's red nails)

	const TCHAR* PhaseName(EOutlawsDmPhase P)
	{
		return P == EOutlawsDmPhase::Warmup ? TEXT("warm-up") : P == EOutlawsDmPhase::Countdown ? TEXT("countdown") : P == EOutlawsDmPhase::Live ? TEXT("live") : TEXT("ended");
	}

	FString DmClock(float Seconds)
	{
		const int32 S = FMath::Max(0, FMath::CeilToInt(Seconds));
		return FString::Printf(TEXT("%d:%02d"), S / 60, S % 60);
	}
}

const FOutlawsMpCharacter* FOutlawsMpCharacter::All(int32& OutCount)
{
	OutCount = UE_ARRAY_COUNT(GMpCharacters);
	return GMpCharacters;
}

int32 FOutlawsMpCharacter::IndexOf(const FString& Short)
{
	for (int32 i = 0; i < int32(UE_ARRAY_COUNT(GMpCharacters)); ++i) if (Short.Equals(GMpCharacters[i].Short, ESearchCase::IgnoreCase)) return i;
	return -1;
}

const FOutlawsMpCharacter* FOutlawsMpCharacter::Find(const FString& Short)
{
	const int32 i = IndexOf(Short);
	if (i >= 0) return &GMpCharacters[i];
	// (-OLMpChar=Death and the like) a word of the full name
	if (Short.Len() >= 3) for (const FOutlawsMpCharacter& C : GMpCharacters) if (FString(C.Name).Contains(Short, ESearchCase::IgnoreCase)) return &C;
	return nullptr;
}

bool FOutlawsMpCharacter::bMatchMenu = false;
TFunction<void(const FString&)> FOutlawsMpCharacter::OnLocalChoice;

FString FOutlawsMpCharacter::LocalChoice()
{
	const TMap<FString, FString> S = AOutlawsLevelActor::ReadSettings();
	const FString* X = S.Find(TEXT("MpCharacter"));
	return X && Find(*X) ? *X : FString(TEXT("Jam"));
}

void FOutlawsMpCharacter::SetLocalChoice(const FString& Short)
{
	const FOutlawsMpCharacter* C = Find(Short);
	if (!C) return;
	TMap<FString, FString> S = AOutlawsLevelActor::ReadSettings();
	S.Add(TEXT("MpCharacter"), C->Short);
	AOutlawsLevelActor::WriteSettings(S);
	UE_LOG(LogOutlaws, Log, TEXT("OLDM character chosen: %s (%s, %s); the match %s"), C->Name, C->Itm, C->Nwx, OnLocalChoice ? TEXT("takes it from the next spawn") : TEXT("is not running: kept for the next"));
	if (OnLocalChoice) OnLocalChoice(C->Short);
}

// ---- BUG-CHARLOADOUT: the character's kit and weapons, from its ITM ----------------------------------------------------------

const FOutlawsCharKit& FOutlawsCharKit::Of(const FOutlawsAssets* A, const FString& Short)
{
	static TMap<FString, FOutlawsCharKit> Cache;
	static const FOutlawsCharKit None;
	const FOutlawsMpCharacter* Ch = FOutlawsMpCharacter::Find(Short);
	if (!Ch) return None;
	if (const FOutlawsCharKit* K = Cache.Find(Ch->Short)) return *K;
	FString Text;
	if (!A || !A->GetText(Ch->Itm, Text)) return None;          // (not cached: the files may come later)
	FOutlawsCharKit& K = Cache.Add(Ch->Short);
	K.Itm = Ch->Itm;
	K.bRead = true;
	FString Func;
	TMap<FString, FString> D;
	OutlawsFormats::ParseItm(Text, Func, D);
	for (int32 i = 1; i < 16; ++i)
	{
		const FString* Item = D.Find(FString::Printf(TEXT("START_ITEM_%d"), i));
		if (!Item) continue;
		const FString Key = Item->TrimStartAndEnd().ToLower();
		const FString* N = D.Find(FString::Printf(TEXT("START_COUNT_%d"), i));
		const int32 Count = N ? FCString::Atoi(**N) : 1;
		TPair<FString, int32>* Was = K.Start.FindByPredicate([&Key](const TPair<FString, int32>& P) { return P.Key == Key; });
		if (Was) Was->Value += Count;                            // (Two-Feathers: iknife 5, then iknife 1)
		else K.Start.Add({ Key, Count });
	}
	// REQ-SANCHEZDUAL (user 2026-10-09): the table's revolvers at least (cidsanch.itm gives one; Spit'n Jack starts with the pair)
	if (Ch->StartPistols > 0)
	{
		TPair<FString, int32>* Pistol = K.Start.FindByPredicate([](const TPair<FString, int32>& P) { return P.Key == TEXT("ipistol"); });
		if (!Pistol) K.Start.Add({ FString(TEXT("ipistol")), int32(Ch->StartPistols) });
		else Pistol->Value = FMath::Max(Pistol->Value, int32(Ch->StartPistols));
	}
	if (const FString* W = D.Find(TEXT("START_WEAPON"))) K.StartSlot = FCString::Atoi(**W);
	// BUG-MPDEATHSOUND: the death cry, SND_DIE (szdie.wav), the table's where the ITM has none
	const FString Die = FPaths::GetBaseFilename(D.FindRef(TEXT("SND_DIE")).TrimStartAndEnd()).ToLower();
	K.DieSound = !Die.IsEmpty() && Die != TEXT("null") ? Die : FString(Ch->DieWav);
	if (const FString* M = D.Find(TEXT("DAMAGE_MULTIPLIER"))) K.DamageMult = FCString::Atof(**M);
	// the letter after "cid" (cidsanch: s); the remaster's own per-character weapons carry it (ispistol, imsgun...)
	const FString Base = FPaths::GetBaseFilename(K.Itm).ToLower();
	if (Base.StartsWith(TEXT("cid")) && Base.Len() > 3) K.Variant = Base[3];
	UE_LOG(LogOutlaws, Log, TEXT("OLCHAR %s from %s: start %s; first weapon slot %d; DAMAGE_MULTIPLIER %.2f; weapons i%c*; dies %s"), Ch->Name, *K.Itm, *K.KitLine(),
		K.StartSlot, K.DamageMult, K.Variant ? K.Variant : TCHAR('-'), *K.DieSound);
	return K;
}

FString FOutlawsCharKit::DieSoundOf(const FOutlawsAssets* A, const FString& Short)
{
	const FOutlawsCharKit& K = Of(A, Short);
	if (K.bRead && !K.DieSound.IsEmpty()) return K.DieSound;
	const FOutlawsMpCharacter* Ch = FOutlawsMpCharacter::Find(Short);
	return Ch ? FString(Ch->DieWav) : FString(TEXT("jadie"));
}

FString FOutlawsCharKit::WeaponVariant(const FOutlawsAssets* A, const FString& Item) const
{
	const FString Low = Item.ToLower();
	if (!A || !Variant || !Low.StartsWith(TEXT("i")) || Low.Len() < 2) return Low;
	const FString Own = FString::Printf(TEXT("i%c%s"), Variant, *Low.Mid(1));
	return A->Has(Own + TEXT(".itm")) ? Own : Low;
}

FString FOutlawsCharKit::KitLine() const
{
	FString Out;
	for (const TPair<FString, int32>& S : Start) Out += FString::Printf(TEXT("%s%s %d"), Out.IsEmpty() ? TEXT("") : TEXT(", "), *S.Key, S.Value);
	return Out.IsEmpty() ? FString(TEXT("(none)")) : Out;
}

// ---- the level ------------------------------------------------------------------------------------------------------------

void FOutlawsDeathmatch::ReadLevel(const FOutlawsLevel& L, const FOutlawsAssets* A)
{
	Spawns.Reset();
	Items.Reset();
	FOutlawsDmSpawn Fallback;
	bool bFallback = false;
	for (const FOutlawsObject& O : L.Objects)
	{
		const FVector At = FOutlawsGeometry::ToUE(O.Pos.X, O.Pos.Y, O.Pos.Z);
		if (O.Name == TEXT("STARTPOS"))
		{
			FOutlawsDmSpawn& S = Spawns.AddDefaulted_GetRef();
			S.Location = At + FVector(0.0, 0.0, 110.0);       // (as the PLAYER start: the eye over the floor)
			S.Yaw = O.Yaw;
			continue;
		}
		if (O.Name == TEXT("PLAYER"))
		{
			Fallback.Location = At + FVector(0.0, 0.0, 110.0);
			Fallback.Yaw = O.Yaw;
			bFallback = true;
			continue;
		}
		if (!A || !O.Name.StartsWith(TEXT("N"))) continue;
		FString Text;
		if (!A->GetText(O.Name.ToLower() + TEXT(".itm"), Text)) continue;
		FString Func;
		TMap<FString, FString> Data;
		OutlawsFormats::ParseItm(Text, Func, Data);
		const FString Generate = Data.FindRef(TEXT("GENERATE"));
		if (!Func.Equals(TEXT("Gen_New"), ESearchCase::IgnoreCase) || !Data.FindRef(TEXT("TYPE")).IsEmpty() || Generate.IsEmpty()) continue;
		if (Generate.StartsWith(TEXT("BGY"), ESearchCase::IgnoreCase) || Generate.Contains(TEXT("CHCK"))) continue;   // (bad guys, the chicken)
		const float Freq = Data.Contains(TEXT("FREQ")) ? FCString::Atof(*Data[TEXT("FREQ")]) : -30.f;
		FOutlawsDmItemGen& G = Items.AddDefaulted_GetRef();
		G.Object = O.Name;
		G.Generate = Generate;
		G.Period = Freq < 0.f ? -Freq : 60.f / FMath::Max(0.1f, Freq);
		G.Location = At;
	}
	if (Spawns.Num() == 0 && bFallback) Spawns.Add(Fallback);
	UE_LOG(LogOutlaws, Log, TEXT("OLDM level %s: %d spawn points (%s), %d pickup generators"), *L.LevelName, Spawns.Num(),
		Spawns.Num() == 1 && bFallback ? TEXT("the PLAYER start alone") : TEXT("STARTPOS"), Items.Num());
}

// ---- the players -----------------------------------------------------------------------------------------------------------

FOutlawsDmPlayer& FOutlawsDeathmatch::AddPlayer(int32 Id, const FString& Name, const FString& Character, bool bLocal, double Now)
{
	if (FOutlawsDmPlayer* Was = Find(Id)) return *Was;
	FOutlawsDmPlayer& P = Players.AddDefaulted_GetRef();
	P.Id = Id;
	P.Name = Name;
	P.Character = FOutlawsMpCharacter::Find(Character) ? Character : FString(TEXT("Jam"));
	P.bLocal = bLocal;
	UE_LOG(LogOutlaws, Log, TEXT("OLDM %s joins as %s (%d players, %s)"), *Name, FOutlawsMpCharacter::Find(P.Character)->Name, Players.Num(), PhaseName(Phase));
	SpawnPlayer(P, Now);
	return P;
}

void FOutlawsDeathmatch::RemovePlayer(int32 Id)
{
	Players.RemoveAll([Id](const FOutlawsDmPlayer& P) { return P.Id == Id; });
}

FOutlawsDmPlayer* FOutlawsDeathmatch::Find(int32 Id)
{
	return Players.FindByPredicate([Id](const FOutlawsDmPlayer& P) { return P.Id == Id; });
}

int32 FOutlawsDeathmatch::ChooseSpawn(int32 ForId) const
{
	int32 Best = -1;
	double BestD = -1.0;
	for (int32 i = 0; i < Spawns.Num(); ++i)
	{
		double Near = TNumericLimits<double>::Max();
		for (const FOutlawsDmPlayer& P : Players)
		{
			if (P.Id == ForId || !P.bAlive) continue;
			Near = FMath::Min(Near, FVector::DistSquared(P.Location, Spawns[i].Location));
		}
		if (Near > BestD)
		{
			BestD = Near;
			Best = i;
		}
	}
	return Best;
}

void FOutlawsDeathmatch::SpawnPlayer(FOutlawsDmPlayer& P, double Now)
{
	if (!P.PendingCharacter.IsEmpty())
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLDM %s is now %s (chosen while playing as %s)"), *P.Name, FOutlawsMpCharacter::Find(P.PendingCharacter)->Name, FOutlawsMpCharacter::Find(P.Character)->Name);
		P.Character = P.PendingCharacter;
		P.PendingCharacter.Reset();
	}
	const int32 S = ChooseSpawn(P.Id);
	FOutlawsDmSpawn At;
	if (Spawns.IsValidIndex(S)) At = Spawns[S];
	P.bAlive = true;
	P.Location = At.Location;
	P.ProtectedUntil = Now + Config.SpawnProtect;
	UE_LOG(LogOutlaws, Log, TEXT("OLDM %s spawns at spawn point %d of %d %s (yaw %.0f) as %s, protected %.0f s"), *P.Name, S, Spawns.Num(), *At.Location.ToCompactString(), At.Yaw,
		FOutlawsMpCharacter::Find(P.Character)->Name, Config.SpawnProtect);
	if (OnRespawn) OnRespawn(P, At);
}

void FOutlawsDeathmatch::SetPendingCharacter(int32 Id, const FString& Short)
{
	FOutlawsDmPlayer* P = Find(Id);
	if (!P || !FOutlawsMpCharacter::Find(Short)) return;
	P->PendingCharacter = Short.Equals(P->Character, ESearchCase::IgnoreCase) ? FString() : Short;
	UE_LOG(LogOutlaws, Log, TEXT("OLDM %s chose %s: from the next spawn"), *P->Name, FOutlawsMpCharacter::Find(Short)->Name);
}

void FOutlawsDeathmatch::ResetScores()
{
	for (FOutlawsDmPlayer& P : Players)
	{
		P.Frags = P.Deaths = P.Suicides = P.Shots = P.Hits = P.LongestCm = 0;
	}
}

// ---- the match ------------------------------------------------------------------------------------------------------------

void FOutlawsDeathmatch::SetPhase(EOutlawsDmPhase P, double Now, const FString& Why)
{
	Phase = P;
	PhaseAt = Now;
	UE_LOG(LogOutlaws, Log, TEXT("OLDM phase: %s (%s)"), PhaseName(P), *Why);
	if (OnPhase) OnPhase(P, Why);
}

void FOutlawsDeathmatch::StartCountdown(double Now)
{
	if (Phase == EOutlawsDmPhase::Countdown || Phase == EOutlawsDmPhase::Live) return;
	SetPhase(EOutlawsDmPhase::Countdown, Now, TEXT("the host started the game"));
}

float FOutlawsDeathmatch::TimeLeft(double Now) const
{
	return Phase == EOutlawsDmPhase::Live && Config.TimeLimit > 0.f ? FMath::Max(0.f, Config.TimeLimit - float(Now - LiveAt)) : Config.TimeLimit;
}

FString FOutlawsDeathmatch::MessageLine(double Now, int32 MaxPlayers) const
{
	switch (Phase)
	{
	case EOutlawsDmPhase::Warmup: return FString::Printf(TEXT("Waiting for players (%d/%d)"), Players.Num(), MaxPlayers);
	case EOutlawsDmPhase::Countdown: return FString::Printf(TEXT("Starting in %d"), FMath::Max(1, FMath::CeilToInt(Config.Countdown - float(Now - PhaseAt))));
	case EOutlawsDmPhase::Ended: return TEXT("The game is over!");          // (LOCAL.MSG 5126)
	default: return FString();
	}
}

void FOutlawsDeathmatch::Tick(double Now)
{
	TableNow = Now;
	if (Phase == EOutlawsDmPhase::Countdown && Now - PhaseAt >= Config.Countdown && !bFlowDriven)
	{
		GoLive(Now, FString::Printf(TEXT("frag limit %d, time limit %s"), Config.FragLimit, *DmClock(Config.TimeLimit)));
	}
	else if (Phase == EOutlawsDmPhase::Live)
	{
		CheckEnd(Now);
	}
	else if (Phase == EOutlawsDmPhase::Ended && Now - PhaseAt >= Config.EndShow && !bFlowDriven)
	{
		SetPhase(EOutlawsDmPhase::Warmup, Now, TEXT("the next round's warm-up"));
	}
	for (int32 i = 0; i < Items.Num(); ++i)
	{
		FOutlawsDmItemGen& G = Items[i];
		if (G.bOut || Now < G.BackAt) continue;
		G.bOut = true;
		UE_LOG(LogOutlaws, Log, TEXT("OLDM item back: %s (%s) after %.0f s"), *G.Object, *G.Generate, G.Period);
		if (OnItemBack) OnItemBack(G, i);
	}
}

void FOutlawsDeathmatch::GoLive(double Now, const FString& Why)
{
	// everyone back at a spawn point, scores and items reset, the clock starts
	ResetScores();
	for (FOutlawsDmPlayer& P : Players) P.bAlive = false;
	for (FOutlawsDmPlayer& P : Players) SpawnPlayer(P, Now);
	for (int32 i = 0; i < Items.Num(); ++i)
	{
		if (!Items[i].bOut) { Items[i].bOut = true; if (OnItemBack) OnItemBack(Items[i], i); }
	}
	LiveAt = Now;
	SetPhase(EOutlawsDmPhase::Live, Now, Why);
}

void FOutlawsDeathmatch::EndByFlow(double Now, const FString& Why)
{
	if (Phase == EOutlawsDmPhase::Ended) return;
	SetPhase(EOutlawsDmPhase::Ended, Now, Why);
}

void FOutlawsDeathmatch::WarmupByFlow(double Now)
{
	if (Phase == EOutlawsDmPhase::Warmup) return;
	SetPhase(EOutlawsDmPhase::Warmup, Now, TEXT("the next round's warm-up"));
}

int32 FOutlawsDeathmatch::TopFrags(FString& OutLeader) const
{
	const FOutlawsDmPlayer* Top = nullptr;
	for (const FOutlawsDmPlayer& P : Players) if (!Top || P.Frags > Top->Frags) Top = &P;
	OutLeader = Top ? Top->Name : FString();
	return Top ? Top->Frags : -1;
}

void FOutlawsDeathmatch::CheckEnd(double Now)
{
	if (Phase != EOutlawsDmPhase::Live || bFlowDriven) return;
	const FOutlawsDmPlayer* Top = nullptr;
	for (const FOutlawsDmPlayer& P : Players) if (!Top || P.Frags > Top->Frags) Top = &P;
	if (Config.FragLimit > 0 && Top && Top->Frags >= Config.FragLimit)
	{
		SetPhase(EOutlawsDmPhase::Ended, Now, FString::Printf(TEXT("%s reached the frag limit, %d"), *Top->Name, Config.FragLimit));
	}
	else if (Config.TimeLimit > 0.f && Now - LiveAt >= Config.TimeLimit)
	{
		SetPhase(EOutlawsDmPhase::Ended, Now, FString::Printf(TEXT("time's up, %s; %s leads with %d"), *DmClock(Config.TimeLimit), Top ? *Top->Name : TEXT("nobody"), Top ? Top->Frags : 0));
	}
}

void FOutlawsDeathmatch::NoteShot(int32 Shooter, double Now)
{
	FOutlawsDmPlayer* P = Find(Shooter);
	if (!P || !P->bAlive) return;
	P->ProtectedUntil = FMath::Min(P->ProtectedUntil, Now);      // (shooting ends the protection)
	if (Phase == EOutlawsDmPhase::Live) ++P->Shots;
}

bool FOutlawsDeathmatch::NoteHit(int32 Shooter, int32 Victim, double Now)
{
	FOutlawsDmPlayer* S = Find(Shooter);
	FOutlawsDmPlayer* V = Find(Victim);
	if (!V || !V->bAlive || Phase == EOutlawsDmPhase::Ended || Phase == EOutlawsDmPhase::Countdown) return false;
	if (Now < V->ProtectedUntil)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLDM hit on %s refused: protected %.1f s more"), *V->Name, V->ProtectedUntil - Now);
		return false;
	}
	if (S && S != V && Phase == EOutlawsDmPhase::Live) ++S->Hits;
	return true;
}

void FOutlawsDeathmatch::NoteKill(int32 Killer, int32 Victim, float DistCm, double Now)
{
	FOutlawsDmPlayer* V = Find(Victim);
	if (!V || !V->bAlive) return;
	V->bAlive = false;
	V->DiedAt = Now;
	FOutlawsDmPlayer* K = Killer == Victim ? nullptr : Find(Killer);
	const bool bCounts = Phase == EOutlawsDmPhase::Live;
	if (bCounts)
	{
		++V->Deaths;
		if (K)
		{
			++K->Frags;
			K->LongestCm = FMath::Max(K->LongestCm, FMath::RoundToInt(DistCm));
		}
		else
		{
			// (a suicide or the world: a frag off, as the original's table shows it)
			++V->Suicides;
			--V->Frags;
		}
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLDM %s %s%s (%s: %d frags, %d deaths)"), *V->Name, K ? TEXT("killed by ") : TEXT("killed himself"), K ? *K->Name : TEXT(""),
		bCounts ? TEXT("counted") : *FString::Printf(TEXT("not counted in %s"), PhaseName(Phase)), K ? K->Frags : V->Frags, V->Deaths);
	CheckEnd(Now);
}

bool FOutlawsDeathmatch::RequestRespawn(int32 Id, double Now)
{
	FOutlawsDmPlayer* P = Find(Id);
	if (!P || P->bAlive) return false;
	if (Now - P->DiedAt < Config.RespawnDelay)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLDM respawn for %s refused: dead %.1f s of %.1f"), *P->Name, Now - P->DiedAt, Config.RespawnDelay);
		return false;
	}
	SpawnPlayer(*P, Now);
	return true;
}

void FOutlawsDeathmatch::NoteItemTaken(int32 Index, double Now)
{
	if (!Items.IsValidIndex(Index) || !Items[Index].bOut) return;
	FOutlawsDmItemGen& G = Items[Index];
	G.bOut = false;
	G.BackAt = Now + G.Period;
	UE_LOG(LogOutlaws, Log, TEXT("OLDM item taken: %s (%s), back in %.0f s"), *G.Object, *G.Generate, G.Period);
}

bool FOutlawsDeathmatch::FillFragTable(FFragTableData& Out) const
{
	Out = FFragTableData();
	Out.Title = TEXT("Death Match");                          // (LOCAL.MSG 5065)
	Out.SetMatchColumns(Players.ContainsByPredicate([](const FOutlawsDmPlayer& P) { return P.Ping >= 0; }));
	const bool bPing = Out.Columns.Num() > 6;
	Out.HeaderRows.Add({ TEXT("Time Limit"), Config.TimeLimit > 0.f ? DmClock(TimeLeft(TableNow)) : FString(TEXT("None")), 0 });
	Out.HeaderRows.Add({ TEXT("Kill Limit"), Config.FragLimit > 0 ? FString::FromInt(Config.FragLimit) : FString(TEXT("None")), 0 });
	for (const FOutlawsDmPlayer& P : Players)
	{
		FFragTableRow& R = Out.Rows.AddDefaulted_GetRef();
		R.Name = P.Name;
		R.bLocal = P.bLocal;
		R.Values = { FString::FromInt(P.Frags), FString::FromInt(P.Deaths), FString::FromInt(P.Shots),
			P.Shots > 0 ? FString::Printf(TEXT("%d%%"), FMath::Min(100, FMath::RoundToInt(100.f * float(P.Hits) / float(P.Shots)))) : FString(TEXT("-")),
			P.LongestCm > 0 ? FString::Printf(TEXT("%d m"), FMath::Max(1, FMath::RoundToInt(P.LongestCm / 100.f))) : FString(TEXT("-")) };
		if (bPing) R.Values.Add(P.Ping >= 0 ? FString::FromInt(P.Ping) : FString(TEXT("-")));
		R.SortA = P.Frags;
		R.SortB = P.Deaths;
	}
	Out.SortRows();
	return Out.Rows.Num() > 0;
}

// ---- -OLDmTest -------------------------------------------------------------------------------------------------------------

bool UOutlawsDeathmatchTest::ShouldCreateSubsystem(UObject* Outer) const
{
	return OLFlags::Param(TEXT("OLDmTest"));
}

UWorld* UOutlawsDeathmatchTest::GetTickableGameObjectWorld() const
{
	const UGameInstance* GI = GetGameInstance();
	return GI ? GI->GetWorld() : nullptr;
}

TStatId UOutlawsDeathmatchTest::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UOutlawsDeathmatchTest, STATGROUP_Tickables);
}

void UOutlawsDeathmatchTest::Tick(float DeltaTime)
{
	UWorld* W = GetTickableGameObjectWorld();
	if (!W || Step >= 99) return;
	TActorIterator<AOutlawsLevelActor> It(W);
	AOutlawsLevelActor* L = It ? *It : nullptr;
	APawn* Pawn = UGameplayStatics::GetPlayerPawn(W, 0);
	if (!L || !Pawn || L->IsFrontEnd() || L->IsRebuildPending() || !L->HasActorBegunPlay() || !L->LiveLevel()) return;
	const double Real = FPlatformTime::Seconds();
	if (Step == 0) { At = Real + 3.0; Step = 1; return; }
	if (Step == 1 && Real < At) return;
	if (Step == 2)
	{
		if (Real < At) return;
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_dm_table.png"), false, false);
		UE_LOG(LogOutlaws, Log, TEXT("OLDM test: screenshot ol_dm_table.png"));
		At = Real + 1.0;
		Step = 3;
		return;
	}
	if (Step == 3)
	{
		if (Real < At) return;
		Step = 99;
		FGenericPlatformMisc::RequestExit(false);
		return;
	}

	// the whole match at once, on a clock of its own (seconds), checked line by line
	int32 Bad = 0;
	auto Check = [&Bad](bool bOk, const FString& What)
	{
		if (!bOk) ++Bad;
		UE_LOG(LogOutlaws, Log, TEXT("OLDM test: %s: %s"), *What, bOk ? TEXT("ok") : TEXT("BAD"));
	};
	FOutlawsDeathmatch Dm;
	Dm.Config.FragLimit = 5;
	Dm.Config.TimeLimit = 600.f;
	int32 Respawns = 0, ItemsBack = 0;
	Dm.OnRespawn = [&Respawns](const FOutlawsDmPlayer&, const FOutlawsDmSpawn&) { ++Respawns; };
	Dm.OnItemBack = [&ItemsBack](const FOutlawsDmItemGen&, int32) { ++ItemsBack; };
	Dm.ReadLevel(*L->LiveLevel(), L->GetLabAssets());
	Check(Dm.GetSpawns().Num() >= 2, FString::Printf(TEXT("%s has %d spawn points"), *L->LevelName, Dm.GetSpawns().Num()));
	Check(Dm.GetItems().Num() > 0, FString::Printf(TEXT("%d pickup generators"), Dm.GetItems().Num()));
	double T = 1000.0;
	Dm.AddPlayer(0, TEXT("Marshal Anderson"), TEXT("Jam"), true, T);
	Dm.AddPlayer(1, TEXT("Dingus"), TEXT("San"), false, T);
	Dm.AddPlayer(2, TEXT("Slim"), TEXT("Mar"), false, T);
	Dm.AddPlayer(3, TEXT("Doc"), TEXT("Doc"), false, T);
	// the spawns are spread: no two of the four at one point while others are free
	{
		TSet<FString> Used;
		for (const FOutlawsDmPlayer& P : Dm.GetPlayers()) Used.Add(P.Location.ToCompactString());
		Check(Used.Num() == FMath::Min(4, Dm.GetSpawns().Num()), FString::Printf(TEXT("4 players on %d different spawn points"), Used.Num()));
	}
	Check(Dm.MessageLine(T, 8) == TEXT("Waiting for players (4/8)"), FString::Printf(TEXT("warm-up line \"%s\""), *Dm.MessageLine(T, 8)));
	// warm-up: a kill does not count
	T += 5.0;
	Dm.NoteKill(1, 2, 800.f, T);
	Check(Dm.Find(1)->Frags == 0 && Dm.Find(2)->Deaths == 0, TEXT("a warm-up kill does not count"));
	Check(!Dm.RequestRespawn(2, T + 0.5), TEXT("a respawn before the delay is refused"));
	Check(Dm.RequestRespawn(2, T + 2.5), TEXT("a respawn after the delay is granted"));
	// the countdown
	T += 10.0;
	Dm.StartCountdown(T);
	Dm.Tick(T + 0.5);
	Check(Dm.MessageLine(T + 0.5, 8) == TEXT("Starting in 10"), FString::Printf(TEXT("countdown line \"%s\""), *Dm.MessageLine(T + 0.5, 8)));
	Dm.Tick(T + 9.2);
	Check(Dm.MessageLine(T + 9.2, 8) == TEXT("Starting in 1"), FString::Printf(TEXT("countdown line \"%s\""), *Dm.MessageLine(T + 9.2, 8)));
	const int32 R0 = Respawns;
	T += 10.0;
	Dm.Tick(T);
	Check(Dm.GetPhase() == EOutlawsDmPhase::Live && Respawns == R0 + 4, TEXT("live: all four respawned at spawn points"));
	// spawn protection: a hit at once is refused, after it counts; shooting ends one's own
	Check(!Dm.NoteHit(1, 3, T + 1.0), TEXT("a hit on a fresh spawn is refused"));
	Check(Dm.NoteHit(1, 3, T + 3.5), TEXT("a hit after the protection counts"));
	Dm.NoteShot(0, T + 0.5);
	Check(Dm.NoteHit(1, 0, T + 0.6), TEXT("a fresh spawn who shoots loses the protection"));
	// kills, a suicide, the character switch
	T += 4.0;
	Dm.SetPendingCharacter(1, TEXT("Bob"));
	Dm.NoteShot(1, T); Dm.NoteHit(1, 2, T); Dm.NoteKill(1, 2, 1250.f, T);
	Check(Dm.Find(1)->Frags == 1 && Dm.Find(2)->Deaths == 1, TEXT("Dingus killed Slim: a frag and a death"));
	Dm.NoteKill(3, 3, 0.f, T + 1.0);
	Check(Dm.Find(3)->Frags == -1 && Dm.Find(3)->Suicides == 1 && Dm.Find(3)->Deaths == 1, TEXT("Doc's suicide: a frag off"));
	Check(Dm.Find(1)->Character == TEXT("San"), TEXT("Dingus is still Spit'n Jack until he spawns"));
	Dm.NoteKill(0, 1, 400.f, T + 2.0);
	Dm.RequestRespawn(1, T + 5.0);
	Check(Dm.Find(1)->Character == TEXT("Bob"), TEXT("Dingus spawns as Bob Graham"));
	// a pickup taken and back on its generator's period
	FOutlawsDmItemGen& G = Dm.GetItems()[0];
	const float Period = G.Period;
	Dm.NoteItemTaken(0, T);
	Dm.Tick(T + Period - 0.5);
	Check(!Dm.GetItems()[0].bOut, FString::Printf(TEXT("%s still gone at %.1f s"), *Dm.GetItems()[0].Object, Period - 0.5f));
	const int32 I0 = ItemsBack;
	Dm.Tick(T + Period + 0.1);
	Check(Dm.GetItems()[0].bOut && ItemsBack == I0 + 1, FString::Printf(TEXT("%s back after %.0f s"), *Dm.GetItems()[0].Object, Period));
	// to the frag limit: Marshal Anderson kills until 5
	T += 10.0;
	for (int32 k = 0; Dm.GetPhase() == EOutlawsDmPhase::Live && k < 10; ++k)
	{
		T += 3.0;
		Dm.RequestRespawn(2, T);
		Dm.NoteShot(0, T); Dm.NoteHit(0, 2, T + 4.0); Dm.NoteKill(0, 2, 900.f + 100.f * k, T + 4.0);
	}
	Check(Dm.GetPhase() == EOutlawsDmPhase::Ended && Dm.Find(0)->Frags == 5, FString::Printf(TEXT("the frag limit ends the match (%d frags)"), Dm.Find(0)->Frags));
	FFragTableData Table;
	Dm.TableNow = T;
	Dm.FillFragTable(Table);
	Check(Table.Rows.Num() == 4 && Table.Rows[0].Name == TEXT("Marshal Anderson") && Table.Rows.Last().Name == TEXT("Doc"), FString::Printf(TEXT("the table, sorted: %s"), *Table.Describe()));
	Dm.Tick(T + 4.0 + Dm.Config.EndShow + 0.1);
	Check(Dm.GetPhase() == EOutlawsDmPhase::Warmup, TEXT("back to warm-up after the end"));
	// the time limit, a second round of two minutes
	Dm.Config.TimeLimit = 120.f;
	T += 30.0;
	Dm.StartCountdown(T);
	Dm.Tick(T + 10.0);
	Check(Dm.Find(0)->Frags == 0, TEXT("the new round's scores reset"));
	Dm.NoteKill(1, 2, 500.f, T + 20.0);
	Dm.Tick(T + 10.0 + 119.0);
	Check(Dm.GetPhase() == EOutlawsDmPhase::Live, TEXT("still live at 1:59"));
	Dm.Tick(T + 10.0 + 120.5);
	Check(Dm.GetPhase() == EOutlawsDmPhase::Ended, TEXT("the time limit ends the match"));
	// the farthest spawn: with one player alive, the chosen point is the farthest from him
	{
		const FOutlawsDmPlayer* P0 = Dm.Find(0);
		const int32 S = Dm.ChooseSpawn(-1);
		double Far = 0.0;
		for (const FOutlawsDmSpawn& Sp : Dm.GetSpawns())
		{
			double Near = TNumericLimits<double>::Max();
			for (const FOutlawsDmPlayer& P : Dm.GetPlayers()) if (P.bAlive) Near = FMath::Min(Near, FVector::DistSquared(P.Location, Sp.Location));
			Far = FMath::Max(Far, Near);
		}
		double Mine = TNumericLimits<double>::Max();
		for (const FOutlawsDmPlayer& P : Dm.GetPlayers()) if (P.bAlive && Dm.GetSpawns().IsValidIndex(S)) Mine = FMath::Min(Mine, FVector::DistSquared(P.Location, Dm.GetSpawns()[S].Location));
		Check(Dm.GetSpawns().IsValidIndex(S) && FMath::IsNearlyEqual(Mine, Far), FString::Printf(TEXT("the chosen spawn %d is the farthest from the living (%.0f m)"), S, FMath::Sqrt(Mine) / 100.0));
		(void)P0;
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLDM test: done, %d bad"), Bad);

	// the end-of-round table in front of the player (the frag table panel)
	FActorSpawnParameters SP;
	SP.ObjectFlags |= RF_Transient;
	Panel = W->SpawnActor<AOutlawsFragTablePanel>(AOutlawsFragTablePanel::StaticClass(), FTransform::Identity, SP);
	if (Panel)
	{
		const UCameraComponent* Cam = Pawn->FindComponentByClass<UCameraComponent>();
		const FVector Eye = Cam ? Cam->GetComponentLocation() : Pawn->GetActorLocation();
		FVector F = Cam ? Cam->GetForwardVector() : Pawn->GetActorForwardVector();
		Panel->SetData(Table);
		Panel->PlaceBefore(Eye, F, 300.f, 320.f, 0.f);
	}
	IFileManager::Get().MakeDirectory(*(FPaths::ProjectSavedDir() / TEXT("OLShots")), true);
	At = Real + 2.0;
	Step = 2;
}

// ---- -OLShots -OLCharPageShot: the character page ------------------------------------------------------------------------

#include "OutlawsPlayer.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsTextures.h"
#include "OutlawsShotTests.h"

namespace
{
	FOutlawsDeathmatch& CharTestDm()
	{
		static FOutlawsDeathmatch Dm;
		return Dm;
	}
}

#if WITH_OUTLAWS_TESTS
void AOutlawsPlayer::CharPageShotTick()
{
	auto Shot = [](const TCHAR* Name)
	{
		const FString Path = FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(Name) + TEXT(".png");
		FScreenshotRequest::RequestScreenshot(Path, false, false);
		UE_LOG(LogOutlaws, Log, TEXT("OLDM char page: screenshot %s"), *Path);
	};
	auto Rows = [this]()
	{
		TArray<FString> Out;
		if (MenuPage) for (const OutlawsMenu::FItem& It : MenuPage->Items) Out.Add(It.Label);
		return FString::Join(Out, TEXT(" | "));
	};
	const AOutlawsLevelActor* L = Level.Get();
	if (ShotStep == 0)
	{
		if (!L || L->IsFrontEnd() || L->IsRebuildPending() || ShotTime < 4.f) return;
		FOutlawsMpCharacter::bMatchMenu = true;                  // (as the session sets it in a match)
		// a match with this player in it as James (the session's part: the choice handed to the rules, worn from the next spawn)
		FOutlawsMpCharacter::OnLocalChoice = nullptr;
		FOutlawsMpCharacter::SetLocalChoice(TEXT("Jam"));
		CharTestDm().AddPlayer(0, TEXT("Marshal"), FOutlawsMpCharacter::LocalChoice(), true, 0.0);
		FOutlawsMpCharacter::OnLocalChoice = [](const FString& Short)
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLDM char page: the match is told %s"), *Short);
			CharTestDm().SetPendingCharacter(0, Short);
		};
		OpenMenu(false);
		UE_LOG(LogOutlaws, Log, TEXT("OLDM char page: pause rows: %s; worn now %s"), *Rows(), *FOutlawsMpCharacter::LocalChoice());
		const int32 Row = MenuPage ? MenuPage->Items.IndexOfByPredicate([](const OutlawsMenu::FItem& It) { return It.Target == TEXT("#character"); }) : INDEX_NONE;
		UE_LOG(LogOutlaws, Log, TEXT("OLDM char page: Switch character row %s"), Row != INDEX_NONE ? TEXT("there") : TEXT("MISSING"));
		if (Row != INDEX_NONE) MenuActivate(1, Row);
		UE_LOG(LogOutlaws, Log, TEXT("OLDM char page: page %s, tiles: %s"), MenuPage ? *MenuPage->File : TEXT("?"), *Rows());
		MenuHover[1] = 4;                // (Bob pointed at)
		RedrawMenu();
		++ShotStep;
		ShotTime = 0.f;
	}
	else if (ShotStep == 1 && ShotTime > 1.5f)
	{
		Shot(TEXT("ol_char_page"));
		++ShotStep;
	}
	else if (ShotStep == 2 && ShotTime > 2.5f)
	{
		MenuActivate(1, 4);              // Bob chosen
		UE_LOG(LogOutlaws, Log, TEXT("OLDM char page: chose Bob: worn now %s (settings MpCharacter)"), *FOutlawsMpCharacter::LocalChoice());
		MenuHover[1] = 5;                // (Mary pointed at; Bob's tile in use)
		RedrawMenu();
		++ShotStep;
	}
	else if (ShotStep == 3 && ShotTime > 4.f)
	{
		Shot(TEXT("ol_char_page_chosen"));
		++ShotStep;
	}
	else if (ShotStep == 4 && ShotTime > 5.f)
	{
		// the six sprite sets the puppets would wear, each loaded from the player's LABs
		AOutlawsLevelActor* LL = Level.Get();
		int32 N = 0;
		const FOutlawsMpCharacter* Cs = FOutlawsMpCharacter::All(N);
		for (int32 i = 0; LL && i < N; ++i)
		{
			const TSharedPtr<OutlawsTextures::FSpriteAtlas> A = LL->LoadSpriteSet(Cs[i].Nwx);
			FString Itm;
			const bool bItm = LL->GetLabAssets() && LL->GetLabAssets()->GetText(Cs[i].Itm, Itm);
			UE_LOG(LogOutlaws, Log, TEXT("OLDM char page: %s: %s %s, %s %s, die %s"), Cs[i].Name, Cs[i].Nwx, A.IsValid() ? TEXT("loaded") : TEXT("MISSING"),
				Cs[i].Itm, bItm ? (Itm.Contains(Cs[i].Nwx, ESearchCase::IgnoreCase) ? TEXT("names it") : TEXT("does NOT name it")) : TEXT("MISSING"), Cs[i].DieWav);
		}
		// the choice worn from the next spawn: still James until he dies and comes back, then Bob
		FOutlawsDeathmatch& Dm = CharTestDm();
		FOutlawsDmPlayer* P = Dm.Find(0);
		const FString Before = P ? P->Character : FString();
		const FString Pending = P ? P->PendingCharacter : FString();
		Dm.NoteKill(-1, 0, 0.f, 10.0);
		const bool bEarly = Dm.RequestRespawn(0, 10.5);
		const bool bBack = Dm.RequestRespawn(0, 13.0);
		const FString After = P ? P->Character : FString();
		UE_LOG(LogOutlaws, Log, TEXT("OLDM char page: next spawn %s: playing as %s with %s pending; respawn at 0.5 s %s, at 3 s %s; now %s"),
			Before == TEXT("Jam") && Pending == TEXT("Bob") && !bEarly && bBack && After == TEXT("Bob") ? TEXT("ok") : TEXT("BAD"), *Before, *Pending,
			bEarly ? TEXT("granted") : TEXT("refused"), bBack ? TEXT("granted") : TEXT("refused"), *After);
		++ShotStep;
	}
	else if (ShotStep == 5 && ShotTime > 6.f)
	{
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

OL_SHOT_MATCH(OLCharPageShot) { return OLFlags::Param(TEXT("OLCharPageShot")); }
OL_SHOT_TEST(OLCharPageShot, 780)
{
	CharPageShotTick();              // (M1: the character page; OutlawsDeathmatch.cpp)
}
#endif // WITH_OUTLAWS_TESTS
