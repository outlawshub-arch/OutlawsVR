#include "OutlawsMpHost.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsLab.h"
#include "OutlawsLevelActor.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

namespace
{
	// the RCM MODES letters in class order (1 c, 2 t, 3 k, 4 s, 5 m, 6 d)
	const TCHAR ModeLetters[] = TEXT("ctksmd");
}

FString FOutlawsMpMap::ModesText() const
{
	FString Out;
	for (int32 c = 1; c <= 6; ++c) if (Allows(EOutlawsGameClass(c))) Out.AppendChar(FChar::ToUpper(ModeLetters[c - 1]));
	return Out;
}

// ---- the options --------------------------------------------------------------------------------------------------------------

FString FOutlawsMpHostOptions::Describe() const
{
	return FString::Printf(TEXT("map %s, %s, time %s, %s %s, max players %d, start %s, friendly fire %s, cheats %s"), *MapFile, *OutlawsMpHost::ClassLabel(GameClass),
		TimeLimitMinutes > 0 ? *FString::Printf(TEXT("%d min"), TimeLimitMinutes) : TEXT("unlimited"), GameClass == EOutlawsGameClass::CaptureTheFlag ? TEXT("captures") : TEXT("kills"),
		ScoreLimit > 0 ? *FString::FromInt(ScoreLimit) : TEXT("unlimited"), MaxPlayers, StartWhen > 0 ? *FString::Printf(TEXT("when %d have joined"), StartWhen) : TEXT("by the host"),
		bFriendlyFire ? TEXT("on") : TEXT("off"), bAllowCheats ? TEXT("on") : TEXT("off")) + (Password.IsEmpty() ? FString(TEXT(", no password")) : FString(TEXT(", a password")))
		+ (bBots && OutlawsMpHost::BotsAllowed(GameClass) ? FString::Printf(TEXT(", %d bots (%s)"), BotCount, OutlawsMpHost::BotSkillLabel(BotSkill)) : FString(TEXT(", no bots")))
		+ (bRandomMap ? FString(TEXT(", random map each match")) : FString()) + (bRandomClass ? FString(TEXT(", random game each match")) : FString());
}

void FOutlawsMpHostOptions::Load()
{
	const TMap<FString, FString> S = AOutlawsLevelActor::ReadSettings();
	auto I = [&S](const TCHAR* K, int32& V) { if (const FString* X = S.Find(K)) V = FCString::Atoi(**X); };
	if (const FString* X = S.Find(TEXT("MpHostMap"))) MapFile = X->ToLower();
	int32 C = int32(GameClass);
	I(TEXT("MpHostClass"), C);
	GameClass = EOutlawsGameClass(FMath::Clamp(C, 1, 6));
	I(TEXT("MpHostTime"), TimeLimitMinutes);
	I(TEXT("MpHostScore"), ScoreLimit);
	I(TEXT("MpHostMaxPlayers"), MaxPlayers);
	I(TEXT("MpHostStartWhen"), StartWhen);
	if (const FString* X = S.Find(TEXT("MpHostFriendlyFire"))) bFriendlyFire = X->ToBool();
	if (const FString* X = S.Find(TEXT("MpHostCheats"))) bAllowCheats = X->ToBool();
	if (const FString* X = S.Find(TEXT("MpHostPassword"))) Password = *X;
	if (const FString* X = S.Find(TEXT("MpHostTeams"))) bTeams = X->ToBool();
	if (const FString* X = S.Find(TEXT("MpHostBots"))) bBots = X->ToBool();
	I(TEXT("MpHostBotCount"), BotCount);
	I(TEXT("MpHostBotSkill"), BotSkill);
	if (const FString* X = S.Find(TEXT("MpHostRandomMap"))) bRandomMap = X->ToBool();
	if (const FString* X = S.Find(TEXT("MpHostRandomGame"))) bRandomClass = X->ToBool();
	Clamp(nullptr);
}

void FOutlawsMpHostOptions::Save() const
{
	TMap<FString, FString> S = AOutlawsLevelActor::ReadSettings();
	S.Add(TEXT("MpHostMap"), MapFile);
	S.Add(TEXT("MpHostClass"), FString::FromInt(int32(GameClass)));
	S.Add(TEXT("MpHostTime"), FString::FromInt(TimeLimitMinutes));
	S.Add(TEXT("MpHostScore"), FString::FromInt(ScoreLimit));
	S.Add(TEXT("MpHostMaxPlayers"), FString::FromInt(MaxPlayers));
	S.Add(TEXT("MpHostStartWhen"), FString::FromInt(StartWhen));
	S.Add(TEXT("MpHostFriendlyFire"), bFriendlyFire ? TEXT("true") : TEXT("false"));
	S.Add(TEXT("MpHostCheats"), bAllowCheats ? TEXT("true") : TEXT("false"));
	S.Add(TEXT("MpHostPassword"), Password);
	S.Add(TEXT("MpHostTeams"), bTeams ? TEXT("true") : TEXT("false"));
	S.Add(TEXT("MpHostBots"), bBots ? TEXT("true") : TEXT("false"));
	S.Add(TEXT("MpHostBotCount"), FString::FromInt(BotCount));
	S.Add(TEXT("MpHostBotSkill"), FString::FromInt(BotSkill));
	S.Add(TEXT("MpHostRandomMap"), bRandomMap ? TEXT("true") : TEXT("false"));
	S.Add(TEXT("MpHostRandomGame"), bRandomClass ? TEXT("true") : TEXT("false"));
	AOutlawsLevelActor::WriteSettings(S);
}

void FOutlawsMpHostOptions::Clamp(const FOutlawsMpMap* Map)
{
	MaxPlayers = FMath::Clamp(MaxPlayers, 2, OutlawsMpHost::MaxPlayersCap());
	StartWhen = StartWhen <= 0 ? 0 : FMath::Clamp(StartWhen, 2, MaxPlayers);
	TimeLimitMinutes = FMath::Clamp(TimeLimitMinutes, 0, 120);
	ScoreLimit = FMath::Clamp(ScoreLimit, 0, 100);
	BotCount = FMath::Clamp(BotCount, 1, MaxPlayers - 1);
	BotSkill = FMath::Clamp(BotSkill, 0, 2);
	if (Map && Map->Modes != 0 && !Map->Allows(GameClass) && !bRandomMap && !bRandomClass)
	{
		// (another map: Death Match where it may be played, else its first class)
		if (Map->Allows(EOutlawsGameClass::DeathMatch)) GameClass = EOutlawsGameClass::DeathMatch;
		else for (int32 c = 1; c <= 6; ++c) if (Map->Allows(EOutlawsGameClass(c))) { GameClass = EOutlawsGameClass(c); break; }
	}
}

// ---- the maps -----------------------------------------------------------------------------------------------------------------

const TArray<int32>& OutlawsMpHost::TimeSteps()
{
	static const TArray<int32> V = { 0, 5, 10, 15, 20, 30, 45, 60, 90, 120 };
	return V;
}

const TArray<int32>& OutlawsMpHost::ScoreSteps()
{
	static const TArray<int32> V = { 0, 5, 10, 15, 20, 25, 30, 40, 50, 75, 100 };
	return V;
}

int32 OutlawsMpHost::MaxPlayersCap()
{
#if PLATFORM_ANDROID
	return 8;
#else
	static const bool bQuest = OLFlags::Param(TEXT("OLMpQuestCap"));
	return bQuest ? 8 : 16;
#endif
}

FString OutlawsMpHost::ClassLabel(EOutlawsGameClass C)
{
	// (the remaster's strings: $m_local_5060 to 5065)
	switch (C)
	{
	case EOutlawsGameClass::CaptureTheFlag: return TEXT("Capture The Flag");
	case EOutlawsGameClass::Tag: return TEXT("Tag");
	case EOutlawsGameClass::Chicken: return TEXT("Kill The Fool With The Chicken");
	case EOutlawsGameClass::SecretDocument: return TEXT("Secret Document");
	case EOutlawsGameClass::TeamPlay: return TEXT("Team Play");
	default: return TEXT("Death Match");
	}
}

bool OutlawsMpHost::BotsAllowed(EOutlawsGameClass C)
{
	return C == EOutlawsGameClass::DeathMatch || C == EOutlawsGameClass::TeamPlay;     // (Team Play: bots on the teams, never after a teammate)
}

const TCHAR* OutlawsMpHost::BotSkillLabel(int32 Skill)
{
	return Skill <= 0 ? TEXT("Easy") : Skill >= 2 ? TEXT("Hard") : TEXT("Normal");
}

bool OutlawsMpHost::ParseRcm(const FString& Text, FOutlawsMpMap& Out, FString& Why)
{
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	bool bStory = false;
	for (const FString& Raw : Lines)
	{
		const FString Line = Raw.TrimStartAndEnd();
		if (Line.IsEmpty() || Line.StartsWith(TEXT("#"))) continue;
		FString Key, Value;
		if (!Line.Split(TEXT(":"), &Key, &Value))
		{
			if (Line.Equals(TEXT("STORY"), ESearchCase::IgnoreCase)) bStory = true;
			continue;
		}
		Key = Key.TrimStartAndEnd().ToUpper();
		Value = Value.TrimStartAndEnd();
		if (Key == TEXT("NAME")) Out.Name = Value;
		else if (Key == TEXT("STORY")) bStory = true;
		else if (Key == TEXT("MODES"))
		{
			Out.Modes = 0;
			for (const TCHAR Ch : Value)
			{
				for (int32 c = 0; c < 6; ++c) if (FChar::ToLower(Ch) == ModeLetters[c]) Out.Modes |= uint8(1u << c);
			}
		}
		else if ((Key == TEXT("LEVEL") || Key == TEXT("LEVELHD")) && Out.Level.IsEmpty())
		{
			TArray<FString> T;
			Value.ParseIntoArrayWS(T);
			if (T.Num() > 0) Out.Level = FPaths::GetBaseFilename(T[0]).ToLower();
		}
	}
	if (Out.Level.IsEmpty())
	{
		Why = bStory ? TEXT("its STORY has no LEVEL") : TEXT("no STORY / LEVEL");
		return false;
	}
	if (Out.Name.IsEmpty()) Out.Name = Out.Level;
	if (Out.Modes == 0) Out.Modes = 1u << (uint8(EOutlawsGameClass::DeathMatch) - 1);     // (no MODES line: Death Match)
	return true;
}

void OutlawsMpHost::ListMaps(const FOutlawsAssets* Assets, TArray<FOutlawsMpMap>& Out)
{
	Out.Reset();
	if (!Assets) return;
	int32 Bad = 0;
	// the stock maps: the RCMs the LABs hold (the patches' over the originals', as every other file)
	TArray<FString> Stock;
	Assets->ListByExtension(TEXT(".rcm"), Stock);
	Stock.Sort();
	for (const FString& F : Stock)
	{
		FString Text, Why;
		FOutlawsMpMap M;
		M.File = F.ToLower();
		if (!Assets->GetText(F, Text) || !ParseRcm(Text, M, Why))
		{
			UE_LOG(LogOutlaws, Warning, TEXT("OLMPHOST map %s left out: %s"), *F, Why.IsEmpty() ? TEXT("not readable") : *Why);
			++Bad;
			continue;
		}
		Out.Add(M);
	}
	const int32 NumStock = Out.Num();
	// the custom maps: <game folder>/<name>.rcm beside <name>.lab (how the original and the launcher's map loader find them)
	const FString Root = Assets->GetRoot();
	TArray<FString> Found;
	IFileManager::Get().FindFiles(Found, *(Root / TEXT("*.rcm")), true, false);
	Found.Sort([](const FString& A, const FString& B) { return A.ToLower() < B.ToLower(); });
	int32 NoLab = 0;
	for (const FString& F : Found)
	{
		const FString Base = FPaths::GetBaseFilename(F);
		FString Lab;
		for (const TCHAR* Ext : { TEXT(".lab"), TEXT(".LAB"), TEXT(".Lab") })
		{
			if (FPaths::FileExists(Root / Base + Ext)) { Lab = Root / Base + Ext; break; }
		}
		if (Lab.IsEmpty()) { ++NoLab; continue; }
		if (Out.ContainsByPredicate([&F](const FOutlawsMpMap& M) { return M.File == F.ToLower(); })) continue;    // (a stock name)
		FString Text, Why;
		FOutlawsMpMap M;
		M.File = F.ToLower();
		M.Lab = Lab;
		M.bCustom = true;
		if (!FFileHelper::LoadFileToString(Text, *(Root / F)) || !ParseRcm(Text, M, Why))
		{
			UE_LOG(LogOutlaws, Warning, TEXT("OLMPHOST map %s left out: %s"), *F, Why.IsEmpty() ? TEXT("not readable") : *Why);
			++Bad;
			continue;
		}
		Out.Add(M);
	}
	Out.Sort([](const FOutlawsMpMap& A, const FOutlawsMpMap& B) { return A.bCustom != B.bCustom ? !A.bCustom : A.Name.ToLower() < B.Name.ToLower(); });
	UE_LOG(LogOutlaws, Log, TEXT("OLMPHOST maps: %d stock, %d custom (%d .rcm without a .lab skipped, %d unreadable)"), NumStock, Out.Num() - NumStock, NoLab, Bad);
}

bool OutlawsMpHost::IsClassOn(EOutlawsGameClass C)
{
	// (M2) Death Match always; the others as each lands green (set here, by hand, before a release)
	static const bool On[7] = {
		false,
		true,       // 1 Capture The Flag
		true,       // 2 Tag
		true,       // 3 Kill The Fool With The Chicken
		true,       // 4 Secret Document
		true,       // 5 Team Play
		true,       // 6 Death Match
	};
	static const bool bAll = OLFlags::Param(TEXT("OLMpAllModes"));
	const int32 i = int32(C);
	return C == EOutlawsGameClass::DeathMatch || bAll || (i >= 1 && i <= 6 && On[i]);
}

TFunction<bool(const FOutlawsMpHostOptions&)>& OutlawsMpHost::OnHost()
{
	static TFunction<bool(const FOutlawsMpHostOptions&)> Fn;
	return Fn;
}

bool OutlawsMpHost::Host(const FOutlawsMpHostOptions& Options)
{
	UE_LOG(LogOutlaws, Log, TEXT("OLMPHOST host: %s"), *Options.Describe());
	if (OnHost()) return OnHost()(Options);
	UE_LOG(LogOutlaws, Log, TEXT("OLMPHOST host: no session wired yet (stub): the options are kept, nothing is hosted"));
	return false;
}

FString OutlawsMpHost::MapTitle(const FString& File)
{
	// (the stock maps' English names, as the remaster's strings say them; a custom map by its file)
	static const TMap<FString, FString> Names = {
		{ TEXT("civpow2.rcm"), TEXT("Buckshotville") }, { TEXT("drygulch.rcm"), TEXT("Dry Gulch") }, { TEXT("fort.rcm"), TEXT("The Fort") },
		{ TEXT("granary.rcm"), TEXT("The Granary") }, { TEXT("hargrove.rcm"), TEXT("Hargrove's Pattern Repair") }, { TEXT("lilbatl.rcm"), TEXT("Bunkers") },
		{ TEXT("mltsimms.rcm"), TEXT("Simms At Night") }, { TEXT("orleans.rcm"), TEXT("Orleans") }, { TEXT("thudtowr.rcm"), TEXT("Thud Tower") },
		{ TEXT("town_2p.rcm"), TEXT("Sanctuary") }, { TEXT("wilderns.rcm"), TEXT("Wilderness") } };
	const FString* Nice = Names.Find(File.ToLower());
	return Nice ? *Nice : FPaths::GetBaseFilename(File);
}

bool OutlawsMpHost::RollMatch(const TArray<FOutlawsMpMap>& Maps, FOutlawsMpHostOptions& Out, const FString& LastMap, EOutlawsGameClass LastClass, FString& Said)
{
	if (!Out.bRandomMap && !Out.bRandomClass) return true;
	// every pair that may be played: a stock map when the map is random (the chosen one otherwise), a game it allows that this
	// build offers when the game is random (the chosen one otherwise)
	struct FPair { const FOutlawsMpMap* Map; EOutlawsGameClass Class; };
	TArray<FPair> All;
	const FOutlawsMpMap* Fixed = Out.bRandomMap ? nullptr : Maps.FindByPredicate([&Out](const FOutlawsMpMap& M) { return M.File.Equals(Out.MapFile, ESearchCase::IgnoreCase); });
	if (!Out.bRandomMap && !Fixed)
	{
		Said = FString::Printf(TEXT("the map %s is not among the %d maps"), *Out.MapFile, Maps.Num());
		return false;
	}
	for (const FOutlawsMpMap& M : Maps)
	{
		if (Out.bRandomMap ? M.bCustom : &M != Fixed) continue;
		for (int32 c = 1; c <= 6; ++c)
		{
			const EOutlawsGameClass C = EOutlawsGameClass(c);
			if (!IsClassOn(C) || !M.Allows(C) || (!Out.bRandomClass && C != Out.GameClass)) continue;
			All.Add({ &M, C });
		}
	}
	if (All.Num() == 0)
	{
		Said = FString::Printf(TEXT("no %s plays %s"), Out.bRandomMap ? TEXT("stock map") : *Out.MapFile, Out.bRandomClass ? TEXT("any game") : *ClassLabel(Out.GameClass));
		return false;
	}
	// another map than the last (a random map), another game than the last (a random game), whenever one is left to pick
	TArray<FPair> Pick = All;
	if (Out.bRandomMap)
	{
		TArray<FPair> Other = Pick.FilterByPredicate([&LastMap](const FPair& P) { return !P.Map->File.Equals(LastMap, ESearchCase::IgnoreCase); });
		if (Other.Num() > 0) Pick = MoveTemp(Other);
	}
	if (Out.bRandomClass)
	{
		TArray<FPair> Other = Pick.FilterByPredicate([LastClass](const FPair& P) { return P.Class != LastClass; });
		if (Other.Num() > 0) Pick = MoveTemp(Other);
	}
	const FPair& Got = Pick[FMath::RandRange(0, Pick.Num() - 1)];
	Out.MapFile = Got.Map->File;
	Out.GameClass = Got.Class;
	Said = FString::Printf(TEXT("%s%s on %s%s (%d of %d valid pairs to pick from)"), *ClassLabel(Got.Class), Out.bRandomClass ? TEXT(" (random)") : TEXT(""),
		*MapTitle(Got.Map->File), Out.bRandomMap ? TEXT(" (random)") : TEXT(""), Pick.Num(), All.Num());
	return true;
}

FOutlawsMatchFlow& OutlawsMpHost::Flow()
{
	static FOutlawsMatchFlow F;
	return F;
}

// ---- the match flow -----------------------------------------------------------------------------------------------------------

const TCHAR* FOutlawsMatchFlow::StateName(EState S)
{
	switch (S)
	{
	case EState::WarmUp: return TEXT("warm-up");
	case EState::Countdown: return TEXT("countdown");
	case EState::Playing: return TEXT("playing");
	case EState::Ended: return TEXT("ended");
	default: return TEXT("idle");
	}
}

void FOutlawsMatchFlow::Say(const FString& Text, float Seconds)
{
	UE_LOG(LogOutlaws, Log, TEXT("OLMPHOST message: %s"), *Text);
	if (OnMessage) OnMessage(Text, Seconds);
}

void FOutlawsMatchFlow::SayWaiting()
{
	WaitingTimer = 0.f;
	LastCount = StartCount();
	FString Text = FString::Printf(TEXT("Waiting for players (%d/%d)"), StartCount(), Options.MaxPlayers);
	if (Options.StartWhen > 0) Text += FString::Printf(TEXT(": starts when %d have joined"), Options.StartWhen);
	Say(Text, WaitingEvery + 0.5f);
}

void FOutlawsMatchFlow::Enter(EState S, const FString& Why)
{
	UE_LOG(LogOutlaws, Log, TEXT("OLMPHOST flow: %s -> %s (%s; %d players)"), StateName(State), StateName(S), *Why, Players);
	State = S;
	StateTime = 0.f;
}

void FOutlawsMatchFlow::Begin(const FOutlawsMpHostOptions& InOptions)
{
	Options = InOptions;
	Options.Clamp(nullptr);
	Players = FMath::Max(1, Players);
	Clock = 0.f;
	bAutoCountdown = false;
	Humans = -1;
	Enter(EState::WarmUp, TEXT("hosting"));
	SayWaiting();
}

void FOutlawsMatchFlow::Stop()
{
	if (State != EState::Idle) Enter(EState::Idle, TEXT("stopped"));
}

void FOutlawsMatchFlow::SetPlayers(int32 N)
{
	N = FMath::Clamp(N, 1, Options.MaxPlayers);
	if (N == Players) return;
	UE_LOG(LogOutlaws, Log, TEXT("OLMPHOST players: %d -> %d (%s)"), Players, N, StateName(State));
	Players = N;
	if (State == EState::WarmUp) SayWaiting();
	else if (State == EState::Countdown && bAutoCountdown && StartCount() < Options.StartWhen)
	{
		Enter(EState::WarmUp, TEXT("below the start count again"));
		Say(TEXT("Not enough players: the countdown stopped"));
		SayWaiting();
	}
	else if (State == EState::Countdown && Players < 2)
	{
		Enter(EState::WarmUp, TEXT("alone again"));
		SayWaiting();
	}
}

void FOutlawsMatchFlow::SetHumans(int32 N)
{
	N = FMath::Max(1, N);
	if (N == Humans) return;
	UE_LOG(LogOutlaws, Log, TEXT("OLMPHOST people: %d -> %d (%s; start when %d)"), Humans, N, StateName(State), Options.StartWhen);
	Humans = N;
	if (State == EState::Countdown && bAutoCountdown && Humans < Options.StartWhen)
	{
		Enter(EState::WarmUp, TEXT("below the start count again"));
		Say(TEXT("Not enough players: the countdown stopped"));
		SayWaiting();
	}
}

bool FOutlawsMatchFlow::HostStart()
{
	if (State != EState::WarmUp || Players < 2)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLMPHOST start refused: %s, %d players"), StateName(State), Players);
		if (State == EState::WarmUp) Say(TEXT("Nobody has joined yet"));
		return false;
	}
	bAutoCountdown = false;
	Enter(EState::Countdown, TEXT("the host started it"));
	return true;
}

void FOutlawsMatchFlow::ReportScore(int32 TopScore, const FString& Leader)
{
	if (State != EState::Playing) return;                // (warm-up kills do not count)
	if (Options.ScoreLimit > 0 && TopScore >= Options.ScoreLimit)
	{
		const FString Why = FString::Printf(TEXT("%s reached the %s limit (%d)"), *Leader,
			Options.GameClass == EOutlawsGameClass::CaptureTheFlag ? TEXT("capture") : TEXT("kill"), Options.ScoreLimit);
		Enter(EState::Ended, Why);
		Say(FString::Printf(TEXT("%s wins!"), *Leader), 5.f);
		if (OnMatchEnd) OnMatchEnd(Why);
	}
}

void FOutlawsMatchFlow::EndNow(const FString& Winner, const FString& Why)
{
	if (State != EState::Playing) return;
	Enter(EState::Ended, Why);
	Say(FString::Printf(TEXT("%s wins!"), *Winner), 5.f);
	if (OnMatchEnd) OnMatchEnd(Why);
}

float FOutlawsMatchFlow::TimeLeft() const
{
	return Options.TimeLimitMinutes > 0 ? FMath::Max(0.f, Options.TimeLimitMinutes * 60.f - Clock) : -1.f;
}

void FOutlawsMatchFlow::Tick(float Dt)
{
	if (State == EState::Idle || Dt <= 0.f) return;
	const float Before = StateTime;
	StateTime += Dt;
	switch (State)
	{
	case EState::WarmUp:
		if (!bLobbyGate && Options.StartWhen > 0 && StartCount() >= Options.StartWhen)
		{
			bAutoCountdown = true;
			Enter(EState::Countdown, FString::Printf(TEXT("%d have joined"), StartCount()));
			return;
		}
		WaitingTimer += Dt;
		if (WaitingTimer >= WaitingEvery || LastCount != StartCount()) SayWaiting();
		return;
	case EState::Countdown:
	{
		// "Starting in 10" at once, then each second down to 1
		const int32 Shown = CountdownSeconds - FMath::FloorToInt(Before);
		const int32 Now = CountdownSeconds - FMath::FloorToInt(StateTime);
		if (Before == 0.f || Now != Shown)
		{
			if (Now >= 1) Say(FString::Printf(TEXT("Starting in %d"), Now), 1.1f);
		}
		if (StateTime >= CountdownSeconds)
		{
			Clock = 0.f;
			bMinuteWarned = false;
			Enter(EState::Playing, TEXT("the countdown ran out"));
			Say(TEXT("Fight!"), 2.f);
			if (OnMatchStart) OnMatchStart();
		}
		return;
	}
	case EState::Playing:
		Clock += Dt;
		if (Options.TimeLimitMinutes > 0)
		{
			const float Left = TimeLeft();
			if (!bMinuteWarned && Left <= 60.f && Options.TimeLimitMinutes > 1)
			{
				bMinuteWarned = true;
				Say(TEXT("One minute left"));
			}
			if (Left <= 0.f)
			{
				const FString Why = FString::Printf(TEXT("the time limit (%d min)"), Options.TimeLimitMinutes);
				Enter(EState::Ended, Why);
				Say(TEXT("Time is up!"), 5.f);
				if (OnMatchEnd) OnMatchEnd(Why);
			}
		}
		return;
	case EState::Ended:
		if (!bHoldEnd && StateTime >= EndedSeconds)
		{
			Enter(EState::WarmUp, TEXT("the next round"));
			bAutoCountdown = false;
			SayWaiting();
		}
		return;
	default:
		return;
	}
}

// ---- REQ-MPLOBBY: the lobby's start rule ---------------------------------------------------------------------------------------

bool FOutlawsLobbyStart::Tick(float Dt, int32 Humans, int32 InLobby, bool bBots, int32 People, int32 StartWhen)
{
	if (bDone) return false;
	Humans = FMath::Max(1, Humans);
	InLobby = FMath::Clamp(InLobby, 0, Humans);
	const int32 Entered = Humans - InLobby;
	if (Left < 0.f)
	{
		if (Entered <= 0) return false;              // (the countdown starts when the first player enters)
		Left = FullSeconds;
		UE_LOG(LogOutlaws, Log, TEXT("OLLOBBY the first player entered: %.0f s (%d of %d in the lobby)"), Left, InLobby, Humans);
	}
	else Left = FMath::Max(0.f, Left - FMath::Max(0.f, Dt));
	if (Humans >= 2)
	{
		// time left = 60 x (still in the lobby / all); a lone host's wait that ran out starts again from there
		const float Cap = FullSeconds * float(InLobby) / float(Humans);
		if (bLoneWait)
		{
			bLoneWait = false;
			Left = Cap;
			UE_LOG(LogOutlaws, Log, TEXT("OLLOBBY a second real player is in: %.0f s (%d of %d in the lobby)"), Left, InLobby, Humans);
		}
		else if (Cap < Left - 0.01f)
		{
			Left = Cap;
			UE_LOG(LogOutlaws, Log, TEXT("OLLOBBY %d of %d still in the lobby: %.0f s left"), InLobby, Humans, Left);
		}
	}
	FString Go;
	if (Humans >= 2 && InLobby == 0) Go = TEXT("everyone is in");
	else if (Left <= 0.f)
	{
		if (Humans >= 2) Go = TEXT("the countdown ran out");
		else if (bBots) Go = TEXT("the lone host's 60 s ran out: the match starts with bots");
		else if (!bLoneWait)
		{
			bLoneWait = true;
			UE_LOG(LogOutlaws, Log, TEXT("OLLOBBY the lone host's 60 s ran out, bots off: waiting for a second real player"));
		}
	}
	if (Go.IsEmpty()) return false;
	const bool bAlone = People < 2 && !bBots;
	const bool bShort = StartWhen > 0 && People < StartWhen;
	if (bAlone || bShort)
	{
		if (!bHeld) UE_LOG(LogOutlaws, Log, TEXT("OLLOBBY %s, but held: %s"), *Go, bAlone ? TEXT("nobody to play against has entered yet")
			: *FString::Printf(TEXT("%d of the %d people the launch asked for (-OLMpStartWhen)"), People, StartWhen));
		bHeld = true;
		return false;
	}
	bDone = true;
	Why = Go;
	UE_LOG(LogOutlaws, Log, TEXT("OLLOBBY start: %s (%d of %d in the lobby, %d in the match, %.0f s left)"), *Why, InLobby, Humans, People, Left);
	return true;
}
