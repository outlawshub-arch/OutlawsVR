// R10, the randomizer (tools/olue/PLANS.md "R10. Randomizer", user 2026-10-03): Options > Gameplay > Randomizer sets it, a run takes it at
// New Game with its seed, and the save keeps both.
// - Re-roll (rule 1, user: "each time the mission is restarted or the level is restarted, it should re-roll"; changed by the
//   user 2026-10-10: a death respawn and Restart Level keep the roll): the bad guys'
//   kinds and the pickups are rolled from (the run's seed, the level's name, the level's attempt) at every level start and
//   restart (a new level is attempt 0, each Restart Level or death restart the next one); a load builds the roll its save was
//   made with.  The roll is worked out in Build from the level's own files (no cache).
// - Bad guys (rule 3): every ordinary bad guy (FUNC BGY_New, a BGY* sprite, a man's height; not one the INF names) keeps his
//   place, facing, BASE_MODE, WATCH_CHOR and DROP_ITEM and takes another kind from the standard pool (BGY1 to BGY6, BGY8,
//   BGY9 as the co-op extras have it): "Same strength" within his tier ({1,2,3}, {4,5,6}, {7,8,9}), "Anything" from all.
// - Items (rule 4): the movable pickups (ammunition, weapons, health, armour, gold, lamp oil) are shuffled among their own
//   spots ("Shuffle places": nothing added or lost) or each drawn from the level's own mix ("Re-roll": by the level's counts;
//   every weapon the level places still placed once at least).  Pinned: keys, puzzle pieces, the badge, the lantern, the
//   idol, anything whose use the INF's locks ask for or that the INF names, anything on a door, lift or trigger sector, and
//   the dynamite of a level with walls to blow.
// - Mission order (rule 5): the story's levels (OUTLAWS.RCS) shuffled once per run from its seed, "Shuffle middle" keeping
//   the first and the last; the cutscenes stay in the story's order by position (rule 2, user: "leave the cutscenes in the
//   same order no matter what level they finished"): the Nth level finished shows the story's Nth.
// Single player only for now: in a co-op session the randomizer is off (the host's roll is not sent; rule 6 is to come).
#include "OutlawsLevelActor.h"
#include "OutlawsFlags.h"
#include "OutlawsUnlocks.h"
#include "OutlawsRuntime.h"
#include "OutlawsCoop.h"
#include "OutlawsLab.h"
#include "OutlawsLevel.h"
#include "OutlawsFormats.h"
#include "OutlawsInf.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsGeometry.h"
#include "OutlawsEnemy.h"
#include "Engine/World.h"
#include "Dom/JsonObject.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Crc.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
#include "HAL/PlatformTime.h"
#include "HAL/PlatformMisc.h"
#include "HAL/IConsoleManager.h"

namespace OLRando
{
	constexpr uint32 EnemySalt = 0x454E454Du;          // ("ENEM")
	constexpr uint32 ItemSalt = 0x4954454Du;           // ("ITEM")
	constexpr uint32 OrderSalt = 0x4F524452u;          // ("ORDR")
	constexpr uint32 BossSalt = 0x424F5353u;           // ("BOSS")
	bool GOldItemPool = false;                         // (-OLRandoBossTest: the re-roll as it was, from the level's own mix, to compare)
	bool GOldKindWeights = false;                      // (-OLRandoCritterTest: every kind drawn as often as any other, as it was, to compare)
	double GSpecialShare = 0.0;                        // (tests) the special kinds' share of the game's own placements, in %
	// the standard pool (OLXtra::KindPool's: the ordinary bad guys all nine story levels place) and the strength tiers
	const TCHAR* const KindPool[] = { TEXT("bgy1"), TEXT("bgy2"), TEXT("bgy3"), TEXT("bgy4"), TEXT("bgy5"), TEXT("bgy6"), TEXT("bgy8"), TEXT("bgy9") };
	int32 Tier(TCHAR Family) { return Family <= TEXT('3') ? 0 : Family <= TEXT('6') ? 1 : 2; }
	// the item classes that may move (ITEM_CLASS): ammunition, weapons, health, armour, gold bars and sacks, lamp oil
	const TCHAR* const MovableClasses[] = { TEXT("ammo"), TEXT("weap"), TEXT("heal"), TEXT("armr"), TEXT("gbar"), TEXT("gsak"), TEXT("fuel") };
	const TCHAR* const Modes[3][3] = { { TEXT("off"), TEXT("same strength"), TEXT("anything") }, { TEXT("off"), TEXT("shuffle places"), TEXT("re-roll") },
		{ TEXT("off"), TEXT("shuffle middle"), TEXT("shuffle all") } };

	uint32 Fnv1a(const FString& Text)
	{
		uint32 H = 2166136261u;
		for (int32 i = 0; i < Text.Len(); ++i)
		{
			H ^= uint32(Text[i]) & 0xFFu;
			H *= 16777619u;
		}
		return H;
	}

	uint32 Mix(uint32 H)
	{
		H ^= H >> 16;
		H *= 0x85EBCA6Bu;
		H ^= H >> 13;
		H *= 0xC2B2AE35u;
		H ^= H >> 16;
		return H;
	}

	/** xorshift32, seeded from the run's seed, the level and the attempt: the same numbers on every machine. */
	struct FRng
	{
		uint32 S = 1u;
		uint32 Next() { S ^= S << 13; S ^= S >> 17; S ^= S << 5; return S; }
		int32 Below(int32 N) { return N > 0 ? int32(Next() % uint32(N)) : 0; }
	};

	FRng Rng(uint32 Seed, const FString& Level, int32 Attempt, uint32 Salt)
	{
		FRng R;
		R.S = Mix(Mix(Seed ^ Salt) ^ Fnv1a(Level.ToLower())) ^ Mix(uint32(Attempt) * 0x9E3779B1u + 1u);
		if (R.S == 0u) R.S = 1u;
		return R;
	}

	/** The word on its own in the (lower case) text: not part of a longer name. */
	bool HasWord(const FString& Text, const FString& Word)
	{
		if (Word.IsEmpty()) return false;
		int32 From = 0;
		while (true)
		{
			const int32 At = Text.Find(Word, ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
			if (At == INDEX_NONE) return false;
			const int32 End = At + Word.Len();
			const bool bLeft = At == 0 || !FChar::IsAlnum(Text[At - 1]);
			const bool bRight = End >= Text.Len() || !FChar::IsAlnum(Text[End]);
			if (bLeft && bRight) return true;
			From = At + 1;
		}
	}

	FString ModeText(int32 Which, int32 Mode) { return Modes[FMath::Clamp(Which, 0, 2)][FMath::Clamp(Mode, 0, 2)]; }
}

bool AOutlawsLevelActor::RandoOn() const
{
	return RandoRun.bSet && (RandoRun.Enemies > 0 || RandoRun.Items > 0 || RandoRun.Order > 0);
}

// (user 2026-10-10) a death respawn and Restart Level build the level's roll again, the same bad guys and pickups; a new level
// start still rolls its own
int32 AOutlawsLevelActor::NextRandoAttempt(const FString& Level) const
{
	const FString Here = Level.ToLower();
	if (RandoPendingAttempt >= 0 && RandoAttemptLevel == Here) return RandoPendingAttempt;
	if (RandoAttemptLevel != Here) return 0;
	return RandoAttempt;
}

uint32 AOutlawsLevelActor::RandoSeedFromText(const FString& Text)
{
	const FString T = Text.TrimStartAndEnd();
	bool bDigits = !T.IsEmpty();
	for (const TCHAR C : T) bDigits &= FChar::IsDigit(C);
	if (bDigits) return uint32(FMath::Min<uint64>(FCString::Strtoui64(*T, nullptr, 10), MAX_uint32));
	return OLRando::Fnv1a(T.ToUpper());
}

void AOutlawsLevelActor::BeginRandoRun(const TCHAR* Why)
{
	// REQ-COOPRANDO: in a session the run is the host's, begun only by the co-op start itself and handed to every machine with
	// each start; a level start of its own (a mission's, a new game's) keeps it
	if (UOutlawsCoop::IsActive(this) && FCString::Strncmp(Why, TEXT("co-op"), 5) != 0) return;
	RandoRun = FRandoRun();
	RandoRun.bSet = true;
	// the page's settings (UNLOCKFREE, user decision for 0.95: no story finish needed any more; all Off until the player sets
	// them).  A co-op session's run is the host's.
	RandoRun.Enemies = RandoEnemies;
	RandoRun.Items = RandoItems;
	RandoRun.Order = RandoOrder;
	RandoRun.Bosses = RandoBosses;
	RandoRun.Critters = RandoCritters;
	// a test's own (never written to the settings file): -OLRando=<enemies>,<items>,<order>, -OLRandoSeed=<text>
	FString Arg;
	if (OLFlags::Value(TEXT("OLRando"), Arg, false))
	{
		TArray<FString> P;
		Arg.ParseIntoArray(P, TEXT(","));
		if (P.Num() > 0) RandoRun.Enemies = FMath::Clamp(FCString::Atoi(*P[0]), 0, 2);
		if (P.Num() > 1) RandoRun.Items = FMath::Clamp(FCString::Atoi(*P[1]), 0, 2);
		if (P.Num() > 2) RandoRun.Order = FMath::Clamp(FCString::Atoi(*P[2]), 0, 2);
		if (P.Num() > 3) RandoRun.Bosses = FMath::Clamp(FCString::Atoi(*P[3]), 0, 1);
		if (P.Num() > 4) RandoRun.Critters = FMath::Clamp(FCString::Atoi(*P[4]), 0, 1);
	}
	FString SeedText = RandoSeedText;
	OLFlags::Value(TEXT("OLRandoSeed"), SeedText);
	// "Random": a new seed for each game, small enough to read off the frag table and type in again
	RandoRun.Seed = SeedText.IsEmpty() ? 1u + OLRando::Mix(uint32(FPlatformTime::Cycles64()) ^ uint32(FDateTime::Now().GetTicks())) % 999999u : RandoSeedFromText(SeedText);
	RandoAttempt = 0;
	RandoAttemptLevel.Reset();
	RandoPendingAttempt = -1;
	UE_LOG(LogOutlaws, Log, TEXT("OLRANDO run (%s): enemies %s, items %s, mission order %s, bosses %s, critters %s, seed %u (%s)%s"), Why, *OLRando::ModeText(0, RandoRun.Enemies),
		*OLRando::ModeText(1, RandoRun.Items), *OLRando::ModeText(2, RandoRun.Order), RandoRun.Bosses ? TEXT("in the mix") : TEXT("off"), RandoRun.Critters ? TEXT("on") : TEXT("off"),
		RandoRun.Seed, SeedText.IsEmpty() ? TEXT("random") : TEXT("typed"),
		UOutlawsCoop::IsActive(this) ? TEXT("; a co-op session: the host's, to every machine") : TEXT(""));
}

void AOutlawsLevelActor::WriteRandoSave(FJsonObject& O, bool bOpening) const
{
	if (!RandoRun.bSet) return;
	const TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
	J->SetNumberField(TEXT("enemies"), RandoRun.Enemies);
	J->SetNumberField(TEXT("items"), RandoRun.Items);
	J->SetNumberField(TEXT("order"), RandoRun.Order);
	J->SetNumberField(TEXT("bosses"), RandoRun.Bosses);
	J->SetNumberField(TEXT("critters"), RandoRun.Critters);
	J->SetNumberField(TEXT("seed"), double(RandoRun.Seed));
	// (an opening save is the next level from its start: its first roll, attempt 0)
	J->SetStringField(TEXT("level"), (bOpening ? NextLevelName : LevelName).ToLower());
	J->SetNumberField(TEXT("attempt"), bOpening ? 0 : RandoAttempt);
	O.SetObjectField(TEXT("rando"), J);
}

void AOutlawsLevelActor::ReadRandoSave(const FJsonObject& O)
{
	RandoRun = FRandoRun();
	RandoRun.bSet = true;
	RandoPendingAttempt = -1;
	const TSharedPtr<FJsonObject>* J = nullptr;
	if (!O.TryGetObjectField(TEXT("rando"), J) || !J || !J->IsValid())
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRANDO load: a save from before the randomizer (or with none): off for this game"));
		return;
	}
	RandoRun.Enemies = FMath::Clamp(int32((*J)->GetNumberField(TEXT("enemies"))), 0, 2);
	RandoRun.Items = FMath::Clamp(int32((*J)->GetNumberField(TEXT("items"))), 0, 2);
	RandoRun.Order = FMath::Clamp(int32((*J)->GetNumberField(TEXT("order"))), 0, 2);
	RandoRun.Bosses = (*J)->HasField(TEXT("bosses")) ? FMath::Clamp(int32((*J)->GetNumberField(TEXT("bosses"))), 0, 1) : 0;
	// (REQ-RANDOCRITTERS) a save from before the row: critters in the mix, as that run began
	RandoRun.Critters = (*J)->HasField(TEXT("critters")) ? FMath::Clamp(int32((*J)->GetNumberField(TEXT("critters"))), 0, 1) : 1;
	RandoRun.Seed = uint32(FMath::Clamp((*J)->GetNumberField(TEXT("seed")), 0.0, double(MAX_uint32)));
	RandoAttemptLevel = (*J)->GetStringField(TEXT("level")).ToLower();
	RandoPendingAttempt = FMath::Max(0, int32((*J)->GetNumberField(TEXT("attempt"))));
	UE_LOG(LogOutlaws, Log, TEXT("OLRANDO load: enemies %s, items %s, mission order %s, seed %u; %s at attempt %d (its roll as saved)"), *OLRando::ModeText(0, RandoRun.Enemies),
		*OLRando::ModeText(1, RandoRun.Items), *OLRando::ModeText(2, RandoRun.Order), RandoRun.Seed, *RandoAttemptLevel, RandoPendingAttempt);
}

// ---- the mission order ------------------------------------------------------------------------------------------------------

bool AOutlawsLevelActor::RandoStory(TArray<FString>& OutLevels, TArray<FString>& OutMoviesAfter, TArray<FString>* OutSdNames) const
{
	// OUTLAWS.RCS as StoryNext reads it: each level (the HD build of a LEVELHD), and the first MOVIE or CREDITS after it
	OutLevels.Reset();
	OutMoviesAfter.Reset();
	if (OutSdNames) OutSdNames->Reset();
	FString Text;
	if (!AssetsRef || !AssetsRef->GetText(TEXT("outlaws.rcs"), Text)) return false;
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	for (const FString& Line : Lines)
	{
		TArray<FString> T;
		Line.ParseIntoArrayWS(T);
		if (T.Num() < 2) continue;
		const FString Op = T[0].ToUpper();
		if (Op == TEXT("LEVEL:") || (Op == TEXT("LEVELHD:") && T.Num() > 2))
		{
			const bool bHd = Op == TEXT("LEVELHD:");
			OutLevels.Add((bHd ? T[2] : T[1]).ToLower());
			OutMoviesAfter.AddDefaulted();
			if (OutSdNames) OutSdNames->Add(T[1].ToLower());
		}
		else if ((Op == TEXT("MOVIE:") || Op == TEXT("CREDITS:")) && OutMoviesAfter.Num() > 0 && OutMoviesAfter.Last().IsEmpty())
		{
			OutMoviesAfter.Last() = FPaths::GetBaseFilename(T[1]);
		}
	}
	return OutLevels.Num() > 0;
}

void AOutlawsLevelActor::RandoLevelOrder(TArray<FString>& Out) const
{
	TArray<FString> Movies;
	RandoStory(Out, Movies);
	if (!RandoRun.bSet || RandoRun.Order <= 0 || Out.Num() < 3) return;
	// fixed for the run (rule 1: the order never re-rolls): the run's seed alone
	OLRando::FRng R = OLRando::Rng(RandoRun.Seed, TEXT("outlaws.rcs"), 0, OLRando::OrderSalt);
	const int32 First = RandoRun.Order == 1 ? 1 : 0;
	const int32 Last = RandoRun.Order == 1 ? Out.Num() - 2 : Out.Num() - 1;
	for (int32 i = Last; i > First; --i) Out.Swap(i, First + R.Below(i - First + 1));
}

bool AOutlawsLevelActor::RandoStoryNext(FString& OutMovie, FString& OutNext) const
{
	if (!RandoOn() || RandoRun.Order <= 0) return false;
	TArray<FString> Levels, Movies, Sd, Order;
	if (!RandoStory(Levels, Movies, &Sd)) return false;
	RandoLevelOrder(Order);
	FString Here = LevelName.ToLower();
	const int32 SdAt = Sd.IndexOfByKey(Here);
	if (SdAt != INDEX_NONE) Here = Levels[SdAt];            // (town for townex)
	const int32 K = Order.IndexOfByKey(Here);
	if (K == INDEX_NONE) return false;
	OutMovie = Movies.IsValidIndex(K) ? Movies[K] : FString();      // (rule 2: the story's cutscene for this place in the run)
	OutNext = Order.IsValidIndex(K + 1) ? Order[K + 1] : FString();
	UE_LOG(LogOutlaws, Log, TEXT("OLRANDO story: %s is level %d of %d in this run: cutscene %s (the story's %d%s), next %s"), *Here, K + 1, Order.Num(),
		OutMovie.IsEmpty() ? TEXT("none") : *OutMovie, K + 1, K == 0 ? TEXT("st") : K == 1 ? TEXT("nd") : K == 2 ? TEXT("rd") : TEXT("th"), OutNext.IsEmpty() ? TEXT("the end") : *OutNext);
	return true;
}

// ---- one level's roll ---------------------------------------------------------------------------------------------------------

bool AOutlawsLevelActor::RollRando(const FOutlawsLevel& L, const FString& Level, const FString& InfText, uint32 DiffBit, const FRandoRun& Run, int32 Attempt, FRandoRoll& Out) const
{
	Out = FRandoRoll();
	if (!AssetsRef) return false;
	const FOutlawsAssets& A = *AssetsRef;
	const FString InfLower = InfText.ToLower();
	// the INF as Build's copy has it: what moves (a door's or a lift's sectors), the triggers' sectors, the locks' items,
	// and whether the level has walls to blow
	FOutlawsInfRuntime R;
	R.Init(L, InfText);
	TSet<int32> Moving, Triggers, Needed;
	for (const FOutlawsElevator& E : R.Elevators)
	{
		if (!E.IsScroll()) Moving.Append(E.Sectors);
	}
	for (const FOutlawsSector& S : L.Sectors)
	{
		if (S.Flags & OutlawsFlags::SectorDoor) Moving.Add(S.Index);
	}
	for (const FOutlawsTrigger& T : R.Triggers)
	{
		if (!T.bLine && T.Sector >= 0) Triggers.Add(T.Sector);       // (a sector stepped on; a wall's switch moves nothing standing by it)
	}
	R.RequiredObjects(Needed);
	bool bExplodes = false;
	for (const FOutlawsInfItem& It : R.Inf.Items)
	{
		for (const FOutlawsInfClass& C : It.Classes) bExplodes |= C.Class == EOutlawsInfClass::Explode;
	}
	struct FItmInfo { FString Anim, Func; TMap<FString, FString> Data; };
	TMap<FString, FItmInfo> ItmOf;
	auto Info = [&](const FString& Name) -> const FItmInfo&
	{
		const FString Key = Name.ToLower();
		if (const FItmInfo* Have = ItmOf.Find(Key)) return *Have;
		FItmInfo& I = ItmOf.Add(Key);
		FString Itm;
		I.Anim = OutlawsFormats::ResolveAnim(A, Name, &Itm);
		OutlawsFormats::ParseItm(Itm, I.Func, I.Data);
		I.Func = I.Func.ToLower();
		return I;
	};
	TSet<int32> PinnedRows;
	auto Rename = [&Out](int32 Row, const FString& Name) { Out.Rename.Add(Row, Name.ToUpper()); };
	// what a pickup gives, as one kind of thing ("weap", "ammo", "heal", "armr", "gold", "fuel"), or "" when it is not one
	// that may move (a key, a puzzle piece, the idol, the badge, anything of another class)
	auto KindOf = [&](const FString& Name, const FItmInfo& I, TArray<FString>* OutClasses, bool* OutLock)
	{
		const bool bGround = I.Func == TEXT("inv_groundobject"), bInstant = I.Func == TEXT("inv_instantgroundobject");
		const bool bDynamite = I.Func == TEXT("shot_projectiledispatch");
		if (!bGround && !bInstant && !bDynamite) return FString();
		TArray<FString> Classes;
		if (bDynamite) Classes.Add(TEXT("weap"));
		else if (bInstant) Classes.Add(I.Data.FindRef(TEXT("ITEM_CLASS")).ToLower());
		else
		{
			const int32 N = I.Data.Contains(TEXT("PICKUP_COUNT")) ? FMath::Max(1, FCString::Atoi(*I.Data[TEXT("PICKUP_COUNT")])) : 1;
			for (int32 e = 0; e < N; ++e)
			{
				const FString Sfx = e == 0 ? FString() : FString::FromInt(e);
				const FString* Item = I.Data.Find(TEXT("PICKUP") + Sfx);
				if (!Item) continue;
				const FString Inv = FPaths::GetBaseFilename(*Item).ToLower();
				const FItemDef* D = ItemDefs.Find(Inv);
				FString Class = I.Data.FindRef(TEXT("ITEM_CLASS") + Sfx).ToLower();
				if (Class.IsEmpty() && D) Class = D->Class.ToLower();
				Classes.Add(Class);
				if (OutLock && D && D->UseId >= 0 && Needed.Contains(D->UseId)) *OutLock = true;
				if (OutLock && OLRando::HasWord(InfLower, Inv)) *OutLock = true;
			}
		}
		if (OutClasses) *OutClasses = Classes;
		if (Classes.Num() == 0 || Name.Equals(TEXT("gidol"), ESearchCase::IgnoreCase)) return FString();
		for (const FString& C : Classes)
		{
			bool bOk = false;
			for (const TCHAR* M : OLRando::MovableClasses) bOk |= C == M;
			if (!bOk) return FString();
		}
		if (Classes.Contains(TEXT("weap"))) return FString(TEXT("weap"));
		return Classes[0] == TEXT("gbar") || Classes[0] == TEXT("gsak") ? FString(TEXT("gold")) : Classes[0];
	};
	// (user 2026-10-04, BUG-RANDOPOOL / REQ-RANDOBOSS) the whole game's bad guys, named outlaws and pickups, gathered once from
	// every story level and historical mission: Anything draws bad guys from all of them, Re-roll draws pickups from all of
	// them, and "Bosses in the mix" its outlaws
	static const TSet<FString> BossFuncs = { TEXT("sanchez_new"), TEXT("twofeathers_new"), TEXT("bloodeye_new"), TEXT("bob_new"), TEXT("buckshot_new"),
		TEXT("drdeath_new"), TEXT("dynamite_new"), TEXT("farmer_new"), TEXT("mary_new"), TEXT("simms_new"), TEXT("slim_new"),
		TEXT("shbill_new"), TEXT("shblood_new"), TEXT("shchez_new"), TEXT("shdick_new"), TEXT("shmary_new") };
	// (REQ-RANDOCRITTERS) Placed: how often the game's levels place each bad guy kind (every difficulty), his weight in the draw;
	// Special: the kinds that are not plain gunmen (no BGY<n> picture of their own, or a shot that is no gun's: the poodle, the
	// squirrel, the townsmen, the thrower), left out by Critters Off
	struct FGamePool { bool bBuilt = false; TArray<FString> Chars, Bosses; TMap<FString, FString> BossFunc; TMap<FString, TArray<FString>> ItemsByKind; TMap<FString, int32> Placed; TSet<FString> Special; };
	static FGamePool GP;
	if (!GP.bBuilt)
	{
		GP.bBuilt = true;
		TArray<FString> Lvs, Mvs;
		RandoStory(Lvs, Mvs);
		for (const TCHAR* H : { TEXT("shfort"), TEXT("shgranry"), TEXT("shgulch"), TEXT("shsimms"), TEXT("shwildns") }) Lvs.AddUnique(H);
		for (const FString& Lv : Lvs)
		{
			FString Obt;
			if (!A.GetText(Lv + TEXT(".obt"), Obt)) continue;
			FOutlawsLevel Lx;
			Lx.ParseObt(Obt);
			for (const FOutlawsObject& O : Lx.Objects)
			{
				if (O.Name == TEXT("PLAYER")) continue;
				const FItmInfo& I = Info(O.Name);
				const bool bNwx = I.Anim.EndsWith(TEXT(".nwx"), ESearchCase::IgnoreCase) && A.Has(I.Anim);
				const double Height = I.Data.Contains(TEXT("HEIGHT")) ? FCString::Atod(*I.Data[TEXT("HEIGHT")]) : 6.0;
				const double Radius = I.Data.Contains(TEXT("RADIUS")) ? FCString::Atod(*I.Data[TEXT("RADIUS")]) : 1.0;
				if (I.Func == TEXT("bgy_new"))
				{
					if (bNwx && Height >= 4.5 && Radius > 0.0)
					{
						GP.Chars.AddUnique(O.Name.ToUpper());
						++GP.Placed.FindOrAdd(O.Name.ToUpper());
					}
				}
				else if (BossFuncs.Contains(I.Func))
				{
					if (bNwx && Radius > 0.0 && !GP.Bosses.Contains(O.Name.ToUpper()))
					{
						GP.Bosses.Add(O.Name.ToUpper());
						GP.BossFunc.Add(O.Name.ToUpper(), I.Func);
					}
				}
				else
				{
					const FString K = KindOf(O.Name, I, nullptr, nullptr);
					if (!K.IsEmpty()) GP.ItemsByKind.FindOrAdd(K).AddUnique(O.Name.ToLower());
				}
			}
		}
		GP.Chars.Sort();
		GP.Bosses.Sort();
		{
			TArray<FString> Said;
			int32 All = 0, Spec = 0;
			for (const FString& C : GP.Chars)
			{
				const FItmInfo& I = Info(C);
				const FString St = FPaths::GetBaseFilename(I.Anim).ToLower();
				const FString* Snd = I.Data.Find(TEXT("SHOOT_SOUND"));
				const bool bBgy = St.StartsWith(TEXT("bgy")) && St.Len() >= 4 && FChar::IsDigit(St[3]);
				const bool bGun = IsGunSound(Snd ? *Snd : FString(TEXT("enmyshot")));
				const int32 N = GP.Placed.FindRef(C);
				All += N;
				if (!bBgy || !bGun)
				{
					GP.Special.Add(C);
					Spec += N;
				}
				Said.Add(FString::Printf(TEXT("%s %d%s (%s, %s)"), *C, N, GP.Special.Contains(C) ? TEXT(" SPECIAL") : TEXT(""), *St, Snd ? **Snd : TEXT("enmyshot")));
			}
			OLRando::GSpecialShare = All ? 100.0 * Spec / All : 0.0;
			UE_LOG(LogOutlaws, Log, TEXT("OLRANDO the bad guys' kinds, placed in the game's levels (picture, shot): %s; %d special kinds of %d (%.1f %% of the kinds), %d of %d placements (%.1f %%)"),
				*FString::Join(Said, TEXT("; ")), GP.Special.Num(), GP.Chars.Num(), GP.Chars.Num() ? 100.0 * GP.Special.Num() / GP.Chars.Num() : 0.0, Spec, All, All ? 100.0 * Spec / All : 0.0);
		}
		for (TPair<FString, TArray<FString>>& Kv : GP.ItemsByKind) Kv.Value.Sort();
		GP.ItemsByKind.KeySort([](const FString& X, const FString& Y) { return X < Y; });
		TArray<FString> Parts;
		for (const TPair<FString, TArray<FString>>& Kv : GP.ItemsByKind) Parts.Add(FString::Printf(TEXT("%s %d (%s)"), *Kv.Key, Kv.Value.Num(), *FString::Join(Kv.Value, TEXT(" "))));
		UE_LOG(LogOutlaws, Log, TEXT("OLRANDO the game's pool from %d levels: %d bad guys (%s), %d named outlaws (%s); pickups: %s"), Lvs.Num(), GP.Chars.Num(), *FString::Join(GP.Chars, TEXT(" ")),
			GP.Bosses.Num(), *FString::Join(GP.Bosses, TEXT(" ")), *FString::Join(Parts, TEXT("; ")));
	}
	TArray<int32> BossCandidates;                  // (the bad guys that may become a named outlaw, in row order)

	// ---- the bad guys (rule 3) ----
	if (Run.Enemies > 0)
	{
		TArray<FString> Pool;
		for (const TCHAR* K : OLRando::KindPool)
		{
			const FItmInfo& I = Info(K);
			if (I.Func == TEXT("bgy_new") && I.Anim.EndsWith(TEXT(".nwx"), ESearchCase::IgnoreCase) && A.Has(I.Anim)) Pool.Add(FString(K).ToUpper());
		}
		for (const FString& C : GP.Chars) Pool.AddUnique(C);         // (every level's own kinds and characters too)
		// a kind's strength by its picture (BGY<n>): one with no such picture is drawn by Anything only
		auto TierOf = [&](const FString& K)
		{
			const FString St = FPaths::GetBaseFilename(Info(K).Anim).ToLower();
			return St.StartsWith(TEXT("bgy")) && St.Len() >= 4 && FChar::IsDigit(St[3]) ? OLRando::Tier(St[3]) : -1;
		};
		OLRando::FRng Rg = OLRando::Rng(Run.Seed, Level, Attempt, OLRando::EnemySalt);
		TMap<FString, int32> Made;
		for (int32 Row = 0; Row < L.Objects.Num(); ++Row)
		{
			const FOutlawsObject& O = L.Objects[Row];
			if (!O.OnDifficulty(DiffBit) || O.Name == TEXT("PLAYER")) continue;
			if (!L.SectorById(O.SectorId) && !L.SectorAt(O.Pos.X, O.Pos.Y, O.Pos.Z)) continue;      // (off the map: never built)
			const FItmInfo& I = Info(O.Name);
			if (I.Func != TEXT("bgy_new")) continue;
			const double Height = I.Data.Contains(TEXT("HEIGHT")) ? FCString::Atod(*I.Data[TEXT("HEIGHT")]) : 6.0;
			const FString Stem = FPaths::GetBaseFilename(I.Anim).ToLower();
			if (Height < 4.5 || !I.Anim.EndsWith(TEXT(".nwx"), ESearchCase::IgnoreCase) || !Stem.StartsWith(TEXT("bgy")) || Stem.Len() < 4 || !FChar::IsDigit(Stem[3])) continue;
			if (OLRando::HasWord(InfLower, O.Name.ToLower()))
			{
				PinnedRows.Add(Row);
				Out.Pinned.Add(O.Name.ToLower() + TEXT(" (a bad guy the INF names)"));
				continue;
			}
			++Out.Eligible;
			BossCandidates.Add(Row);
			// (REQ-RANDOCRITTERS) Critters Off: never a critter or a special enemy in his place.  Each kind weighed by how often
			// the game's levels place it (at least once), so the one-off kinds turn up as rarely as they do in the game; it
			// was one in the pool's 58 each: the 6 special kinds (1.6 % of the game's placements) took about one place in ten
			TArray<FString> From;
			TArray<int32> Weight;
			int32 Total = 0;
			for (const FString& K : Pool)
			{
				if (Run.Enemies != 2 && TierOf(K) != OLRando::Tier(Stem[3])) continue;
				if (Run.Critters <= 0 && GP.Special.Contains(K)) continue;
				From.Add(K);
				Weight.Add(OLRando::GOldKindWeights ? 1 : FMath::Max(1, GP.Placed.FindRef(K)));
				Total += Weight.Last();
			}
			if (From.Num() == 0) continue;
			int32 At = Rg.Below(Total), Pi = 0;
			while (Pi < From.Num() - 1 && At >= Weight[Pi]) At -= Weight[Pi++];
			const FString Pick = From[Pi];
			++Made.FindOrAdd(Pick.ToLower());
			if (GP.Special.Contains(Pick)) ++Out.SpecialPicks;
			if (Pick.Equals(O.Name, ESearchCase::IgnoreCase)) continue;
			Rename(Row, Pick);
			TMap<FString, FString>& Keep = Out.Keep.Add(Row);
			for (const TCHAR* Key : { TEXT("BASE_MODE"), TEXT("WATCH_CHOR"), TEXT("DROP_ITEM") }) Keep.Add(Key, I.Data.FindRef(Key));
			++Out.Swapped;
		}
		// (tests) the critters and special enemies the level places itself, not in the draw: they stay as they are
		for (int32 Row = 0; Row < L.Objects.Num(); ++Row)
		{
			const FOutlawsObject& O = L.Objects[Row];
			if (!O.OnDifficulty(DiffBit) || O.Name == TEXT("PLAYER") || BossCandidates.Contains(Row)) continue;
			if (!L.SectorById(O.SectorId) && !L.SectorAt(O.Pos.X, O.Pos.Y, O.Pos.Z)) continue;
			const FItmInfo& I = Info(O.Name);
			if (I.Func != TEXT("bgy_new")) continue;
			const double Height = I.Data.Contains(TEXT("HEIGHT")) ? FCString::Atod(*I.Data[TEXT("HEIGHT")]) : 6.0;
			if (Height < 4.5 || GP.Special.Contains(O.Name.ToUpper())) ++Out.OwnSpecials;
		}
		// (REQ-RANDOBOSS) "Bosses in the mix": now and then one of the game's named outlaws in a
		// bad guy's place, at most one a level (two where there are 60 or more), never within 25 m of the player's start, never
		// the level's own; he ends nothing (Build makes him a bad guy: RandomBoss)
		if (Run.Bosses > 0 && GP.Bosses.Num() > 0)
		{
			TSet<FString> OwnFuncs;
			FVector Start = FVector::ZeroVector;
			bool bStart = false;
			for (const FOutlawsObject& O : L.Objects)
			{
				if (O.Name == TEXT("PLAYER")) { if (!bStart) Start = O.Pos; bStart = true; continue; }
				const FString F = Info(O.Name).Func;
				if (BossFuncs.Contains(F)) OwnFuncs.Add(F);
			}
			TArray<FString> From;
			for (const FString& B : GP.Bosses) if (!OwnFuncs.Contains(GP.BossFunc.FindRef(B))) From.Add(B);
			// a level gets one 35 times in 100, a second (60 bad guys or more) 15 in 100 of those; his place one of the bad guys'
			// at least 25 m from the start
			OLRando::FRng Rb = OLRando::Rng(Run.Seed, Level, Attempt, OLRando::BossSalt);
			TArray<int32> Far;
			for (const int32 Row : BossCandidates)
			{
				const FOutlawsObject& O = L.Objects[Row];
				const double DistCm = FVector2D::Distance(FVector2D(O.Pos.X, O.Pos.Z), FVector2D(Start.X, Start.Z)) * FOutlawsGeometry::UnitsToCm;
				if (!bStart || DistCm >= 2500.0) Far.Add(Row);
			}
			int32 Want = Far.Num() > 0 && Rb.Below(100) < 35 ? 1 : 0;
			if (Want == 1 && Out.Eligible >= 60 && Rb.Below(100) < 15) Want = 2;
			for (int32 k = 0; k < Want && Far.Num() > 0 && From.Num() > 0; ++k)
			{
				const int32 Row = Far[Rb.Below(Far.Num())];
				Far.Remove(Row);
				const FOutlawsObject& O = L.Objects[Row];
				const FString Pick = From[Rb.Below(From.Num())];
				From.Remove(Pick);
				if (!Out.Keep.Contains(Row))
				{
					TMap<FString, FString>& Keep = Out.Keep.Add(Row);
					const FItmInfo& I = Info(O.Name);
					for (const TCHAR* Key : { TEXT("BASE_MODE"), TEXT("WATCH_CHOR"), TEXT("DROP_ITEM") }) Keep.Add(Key, I.Data.FindRef(Key));
				}
				const FString Drawn = Out.Rename.Contains(Row) ? Out.Rename[Row] : O.Name.ToUpper();
				if (int32* N = Made.Find(Drawn.ToLower())) --*N;
				if (GP.Special.Contains(Drawn)) --Out.SpecialPicks;
				if (!Out.Rename.Contains(Row)) ++Out.Swapped;
				Rename(Row, Pick);
				Out.RandomBoss.Add(Row);
				++Made.FindOrAdd(TEXT("boss ") + Pick.ToLower());
			}
		}
		Made.KeySort([](const FString& X, const FString& Y) { return X < Y; });
		TArray<FString> Parts;
		for (const TPair<FString, int32>& Kv : Made) Parts.Add(FString::Printf(TEXT("%s %d"), *Kv.Key, Kv.Value));
		Out.Kinds = FString::Join(Parts, TEXT(", "));
	}

	// ---- the pickups (rule 4) ----
	if (Run.Items > 0)
	{
		struct FSpot { int32 Row = -1; FString Name; bool bWeapon = false; };
		TArray<FSpot> Spots;
		for (int32 Row = 0; Row < L.Objects.Num(); ++Row)
		{
			const FOutlawsObject& O = L.Objects[Row];
			if (!O.OnDifficulty(DiffBit) || O.Name == TEXT("PLAYER")) continue;
			const FOutlawsSector* Sec = L.SectorById(O.SectorId);
			if (!Sec) Sec = L.SectorAt(O.Pos.X, O.Pos.Y, O.Pos.Z);
			if (!Sec) continue;
			const FItmInfo& I = Info(O.Name);
			const bool bGround = I.Func == TEXT("inv_groundobject"), bInstant = I.Func == TEXT("inv_instantgroundobject");
			const bool bDynamite = I.Func == TEXT("shot_projectiledispatch");
			const FString Name = O.Name.ToLower();
			if (I.Func == TEXT("inv_gsheriffstar"))
			{
				PinnedRows.Add(Row);
				Out.Pinned.Add(Name + TEXT(" (the badge)"));
				continue;
			}
			if (!bGround && !bInstant && !bDynamite) continue;
			// what it gives: each entry's class (ITEM_CLASS, else the inventory item's own) and the INF's use for it
			TArray<FString> Classes;
			bool bLock = false;
			if (bDynamite) Classes.Add(TEXT("weap"));
			else if (bInstant) Classes.Add(I.Data.FindRef(TEXT("ITEM_CLASS")).ToLower());
			else
			{
				const int32 N = I.Data.Contains(TEXT("PICKUP_COUNT")) ? FMath::Max(1, FCString::Atoi(*I.Data[TEXT("PICKUP_COUNT")])) : 1;
				for (int32 e = 0; e < N; ++e)
				{
					const FString Sfx = e == 0 ? FString() : FString::FromInt(e);
					const FString* Item = I.Data.Find(TEXT("PICKUP") + Sfx);
					if (!Item) continue;
					const FString Inv = FPaths::GetBaseFilename(*Item).ToLower();
					const FItemDef* D = ItemDefs.Find(Inv);
					FString Class = I.Data.FindRef(TEXT("ITEM_CLASS") + Sfx).ToLower();
					if (Class.IsEmpty() && D) Class = D->Class.ToLower();
					Classes.Add(Class);
					if (D && D->UseId >= 0 && Needed.Contains(D->UseId)) bLock = true;
					if (OLRando::HasWord(InfLower, Inv)) bLock = true;
				}
			}
			const TCHAR* Why = nullptr;
			FString Odd;
			for (const FString& C : Classes)
			{
				if (C.Contains(TEXT("key"))) Why = TEXT("a key");
				else if (C.Contains(TEXT("puzz"))) Why = TEXT("a puzzle piece");
				else if (!Why)
				{
					bool bOk = false;
					for (const TCHAR* M : OLRando::MovableClasses) bOk |= C == M;
					if (!bOk) Odd = C.IsEmpty() ? FString(TEXT("no class")) : C;
				}
			}
			FString WhyText = Why ? FString(Why) : !Odd.IsEmpty() ? FString::Printf(TEXT("class %s"), *Odd) : FString();
			if (WhyText.IsEmpty() && Name == TEXT("gidol")) WhyText = TEXT("the idol");
			if (WhyText.IsEmpty() && bDynamite && bExplodes) WhyText = TEXT("dynamite where walls are to be blown");
			if (WhyText.IsEmpty() && bLock) WhyText = TEXT("the INF's locks ask for it");
			if (WhyText.IsEmpty() && OLRando::HasWord(InfLower, Name)) WhyText = TEXT("the INF names it");
			if (WhyText.IsEmpty())
			{
				for (const FString& C : Classes) if (OLRando::HasWord(InfLower, C)) WhyText = TEXT("the INF names its class");
			}
			if (WhyText.IsEmpty() && Moving.Contains(Sec->Index)) WhyText = TEXT("on a door or a lift");
			if (WhyText.IsEmpty() && Triggers.Contains(Sec->Index)) WhyText = TEXT("on a trigger's sector");
			if (!WhyText.IsEmpty())
			{
				PinnedRows.Add(Row);
				Out.Pinned.Add(FString::Printf(TEXT("%s (%s)"), *Name, *WhyText));
				continue;
			}
			FSpot Sp;
			Sp.Row = Row;
			Sp.Name = Name;
			Sp.bWeapon = Classes.Contains(TEXT("weap"));
			Spots.Add(Sp);
		}
		Out.Movable = Spots.Num();
		OLRando::FRng Ri = OLRando::Rng(Run.Seed, Level, Attempt, OLRando::ItemSalt);
		TArray<FString> Was, Now;
		for (const FSpot& Sp : Spots) Was.Add(Sp.Name);
		Now = Was;
		if (Run.Items == 1)
		{
			for (int32 i = Now.Num() - 1; i > 0; --i) Now.Swap(i, Ri.Below(i + 1));
		}
		else if (!OLRando::GOldItemPool && GP.ItemsByKind.Num() > 0)
		{
			// (BUG-RANDOPOOL, user 2026-10-04: canteens far too often, and only the level's own things: each spot was drawn from
			// the level's own mix by its counts, and the canteens are what the levels place most) each spot from the whole game's
			// pickups: a kind of thing first (weapon, ammunition, health, armour, gold, lamp oil), each kind as likely, then one
			// of its items, in the order the level lists the spots
			TArray<FString> Kinds;
			GP.ItemsByKind.GetKeys(Kinds);
			for (int32 i = 0; i < Now.Num(); ++i)
			{
				const TArray<FString>& Its = GP.ItemsByKind[Kinds[Ri.Below(Kinds.Num())]];
				Now[i] = Its[Ri.Below(Its.Num())];
			}
		}
		else
		{
			// each spot drawn from the level's own mix (its counts as weights), in the order the level lists them
			TArray<FString> Names;
			TArray<int32> Weights;
			for (const FString& N : Was)
			{
				const int32 At = Names.IndexOfByKey(N);
				if (At == INDEX_NONE) { Names.Add(N); Weights.Add(1); }
				else ++Weights[At];
			}
			for (int32 i = 0; i < Now.Num(); ++i)
			{
				int32 Pick = Ri.Below(Was.Num());
				int32 k = 0;
				while (Pick >= Weights[k]) Pick -= Weights[k++];
				Now[i] = Names[k];
			}
		}
		if (Run.Items != 1)
		{
			// every weapon the level places still placed once (never none in a level with no start kit to fall back on), each
			// put on a spot whose item is not the last of its kind
			TArray<FString> Weapons;
			for (const FSpot& Sp : Spots) if (Sp.bWeapon) Weapons.AddUnique(Sp.Name);
			for (const FString& W : Weapons)
			{
				if (Now.Contains(W)) continue;
				TArray<int32> Spare;
				for (int32 i = 0; i < Now.Num(); ++i)
				{
					int32 Same = 0;
					for (const FString& N : Now) Same += N == Now[i] ? 1 : 0;
					if (Same > 1) Spare.Add(i);
				}
				// (the game's pool gives fewer repeats: else any spot not holding one of the level's own weapons)
				if (Spare.Num() == 0) for (int32 i = 0; i < Now.Num(); ++i) if (!Weapons.Contains(Now[i])) Spare.Add(i);
				if (Spare.Num() > 0) Now[Spare[Ri.Below(Spare.Num())]] = W;
			}
			for (const FString& W : Weapons) Out.bWeaponsKept &= Now.Contains(W);
		}
		for (const FString& N : Now) ++Out.ItemKinds.FindOrAdd(N);
		for (int32 i = 0; i < Spots.Num(); ++i)
		{
			if (Now[i] == Was[i]) continue;
			Rename(Spots[i].Row, Now[i]);
			++Out.Changed;
		}
		TArray<FString> A1 = Was, A2 = Now;
		A1.Sort();
		A2.Sort();
		Out.bCountsKept = A1 == A2;
	}
	for (const int32 Row : PinnedRows) Out.PinnedMoved += Out.Rename.Contains(Row) ? 1 : 0;
	// the checksum: every row the roll changes, in row order, with the name it is built as
	TArray<int32> Rows;
	Out.Rename.GetKeys(Rows);
	Rows.Sort();
	uint32 Sum = 0;
	for (const int32 Row : Rows)
	{
		Sum = FCrc::MemCrc32(&Row, sizeof(Row), Sum);
		Sum = FCrc::StrCrc32(*Out.Rename[Row], Sum);
	}
	Out.Sum = Sum;
	return true;
}

void AOutlawsLevelActor::PrepareRando(FOutlawsLevel& L, const FString& InfText, uint32 DiffBit, FRandoRoll& Out)
{
	if (bHom && LevelName.Equals(TEXT("office"), ESearchCase::IgnoreCase)) return;     // (the Marshal's office: no bad guys, its posters)
	if (!RandoRun.bSet) BeginRandoRun(TEXT("the first level built"));
	// the attempt (rule 1): a load's own; the same level again (Restart Level, a death restart, a skip to it) the next one;
	// another level its first
	const FString Here = LevelName.ToLower();
	RandoAttempt = NextRandoAttempt(Here);
	RandoPendingAttempt = -1;
	RandoAttemptLevel = Here;
	if (!RandoOn() || (RandoRun.Enemies <= 0 && RandoRun.Items <= 0)) return;
	const double T0 = FPlatformTime::Seconds();
	if (!RollRando(L, Here, InfText, DiffBit, RandoRun, RandoAttempt, Out)) return;
	for (const TPair<int32, FString>& Kv : Out.Rename)
	{
		if (L.Objects.IsValidIndex(Kv.Key)) L.Objects[Kv.Key].Name = Kv.Value;
	}
	RandoSum = Out.Sum;
	UE_LOG(LogOutlaws, Log, TEXT("OLRANDO roll %s attempt %d (seed %u): %d of %d bad guys another kind (%s), %d critters or special enemies drawn (critters %s), %d of %d movable pickups %s, %d pinned, checksum %08x (%.1f ms)"),
		*Here, RandoAttempt, RandoRun.Seed, Out.Swapped, Out.Eligible, Out.Kinds.IsEmpty() ? TEXT("none") : *Out.Kinds, Out.SpecialPicks, RandoRun.Critters ? TEXT("on") : TEXT("off"), Out.Changed, Out.Movable,
		RandoRun.Items == 1 ? TEXT("shuffled") : RandoRun.Items == 2 ? TEXT("re-rolled") : TEXT("left"), Out.Pinned.Num(), Out.Sum, (FPlatformTime::Seconds() - T0) * 1000.0);
	if (Out.PinnedMoved > 0 || (RandoRun.Items == 1 && !Out.bCountsKept) || !Out.bWeaponsKept)
	{
		UE_LOG(LogOutlaws, Warning, TEXT("OLRANDO roll %s: %d pinned changed, counts %s, weapons %s"), *Here, Out.PinnedMoved, Out.bCountsKept ? TEXT("kept") : TEXT("CHANGED"),
			Out.bWeaponsKept ? TEXT("kept") : TEXT("LOST"));
	}
}

// ---- -OLRandoTest ---------------------------------------------------------------------------------------------------------------
// Run as: -OLLevel=hideout -OLNoFrontEnd -OLRando=2,1,1 -OLRandoSeed=<n> -OLRandoTest.  The level built as the run's first:
// the rolls worked out again for the hideout, the town and the canyon (the same seed and attempt the same checksum, another
// attempt another; nothing pinned changes; Shuffle keeps the counts; Re-roll keeps every weapon); the pinned items of every
// story level and historical mission listed (the audit); a save and a load keep the roll, a restart rolls again (and a
// load after it keeps that one); then three levels of the run's order, each finished as the story finishes it, the cutscene
// by position; a save from a game that never randomized, loaded with the page's options on, plays (and restarts) as it
// was.  Each check "OLRANDO test PASS|FAIL", then a count and the exit.

#if WITH_OUTLAWS_TESTS
void AOutlawsLevelActor::TickRandoTest(float Dt)
{
	static int32 Step = 0, Pass = 0, Fail = 0, RunStep = 0;
	static float Ready = 0.f, Total = 0.f;
	static uint32 Sum0 = 0, Sum1 = 0;
	static TArray<FString> RunOrder, Story, Movies;
	static FString Expect;
	auto Check = [](bool bOk, const FString& What)
	{
		(bOk ? Pass : Fail)++;
		UE_LOG(LogOutlaws, Display, TEXT("OLRANDO test %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
	};
	auto Finish = []()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLRANDO test done: %d passed, %d failed"), Pass, Fail);
		FPlatformMisc::RequestExit(false);
	};
	Total += Dt;
	if (Total > 300.f && Step < 99)
	{
		Check(false, FString::Printf(TEXT("timed out at step %d"), Step));
		Step = 99;
		Finish();
		return;
	}
	bInvincible = true;                       // (the bad guys may be about: the test is of the build, not the fight)
	bEnemiesActive = false;
	const bool bReady = !bRebuildPending && !bNeedPlace && Inf && !PendingLoad.IsValid() && !Movie && LevelEndTimer < 0.f && !bFrontEnd;
	Ready = bReady ? Ready + Dt : 0.f;
	if (Ready < 2.f || Step >= 99) return;
	Ready = 0.f;
	const uint32 DiffBit = OutlawsDifficulty::ObjectBit(SpawnDifficulty);
	auto Roll = [this, DiffBit](const FString& Lv, const FRandoRun& Run, int32 Attempt, FRandoRoll& Out)
	{
		FString Lvt, Obt, InfT;
		if (!AssetsRef || !AssetsRef->GetText(Lv + TEXT(".lvt"), Lvt)) return false;
		AssetsRef->GetText(Lv + TEXT(".obt"), Obt);
		AssetsRef->GetText(Lv + TEXT(".inf"), InfT);
		FOutlawsLevel Lx;
		Lx.ParseLvt(Lvt);
		Lx.ParseObt(Obt);
		return RollRando(Lx, Lv, InfT, DiffBit, Run, Attempt, Out);
	};
	switch (Step)
	{
	case 0:
	{
		Check(RandoOn(), FString::Printf(TEXT("the run is on: enemies %d, items %d, order %d, seed %u"), RandoRun.Enemies, RandoRun.Items, RandoRun.Order, RandoRun.Seed));
		Sum0 = RandoSum;
		Check(RandoAttempt == 0 && Sum0 != 0, FString::Printf(TEXT("%s built at attempt %d with a roll, checksum %08x"), *LevelName, RandoAttempt, Sum0));
		{
			FRandoRoll Again;
			Roll(LevelName.ToLower(), RandoRun, 0, Again);
			Check(Again.Sum == Sum0, FString::Printf(TEXT("the build's roll is the roll worked out again (%08x, %08x)"), Sum0, Again.Sum));
		}
		// the rolls of three levels, each way, worked out twice and at another attempt
		for (const TCHAR* Lv : { TEXT("hideout"), TEXT("townex"), TEXT("canyon") })
		{
			for (int32 Way = 0; Way < 2; ++Way)
			{
				FRandoRun Run = RandoRun;
				Run.Enemies = Way == 0 ? 2 : 1;
				Run.Items = Way == 0 ? 1 : 2;
				FRandoRoll R0, R0b, R1;
				const bool bOk = Roll(Lv, Run, 0, R0) && Roll(Lv, Run, 0, R0b) && Roll(Lv, Run, 1, R1);
				const FString Tag = FString::Printf(TEXT("%s, enemies %s, items %s"), Lv, *OLRando::ModeText(0, Run.Enemies), *OLRando::ModeText(1, Run.Items));
				Check(bOk && R0.Sum == R0b.Sum, FString::Printf(TEXT("%s: the same seed and attempt, the same roll (%08x, %08x): %d of %d bad guys (%s), %d of %d pickups changed"), *Tag,
					R0.Sum, R0b.Sum, R0.Swapped, R0.Eligible, *R0.Kinds, R0.Changed, R0.Movable));
				Check(bOk && R1.Sum != R0.Sum, FString::Printf(TEXT("%s: attempt 1 rolls again (%08x after %08x)"), *Tag, R1.Sum, R0.Sum));
				Check(bOk && R0.PinnedMoved == 0 && R1.PinnedMoved == 0, FString::Printf(TEXT("%s: none of the %d pinned changed"), *Tag, R0.Pinned.Num()));
				if (Run.Items == 1) Check(bOk && R0.bCountsKept && R1.bCountsKept, FString::Printf(TEXT("%s: the same items before and after (%d spots)"), *Tag, R0.Movable));
				else Check(bOk && R0.bWeaponsKept && R1.bWeaponsKept, FString::Printf(TEXT("%s: every weapon the level places still placed"), *Tag));
			}
		}
		// the audit: what stays where it is, in each story level and historical mission
		for (const TCHAR* Lv : { TEXT("hideout"), TEXT("townex"), TEXT("train"), TEXT("canyon"), TEXT("mill"), TEXT("simms"), TEXT("miner"), TEXT("cliff"), TEXT("ranch"),
			TEXT("shfort"), TEXT("shgranry"), TEXT("shgulch"), TEXT("shsimms"), TEXT("shwildns") })
		{
			FRandoRun Run = RandoRun;
			Run.Enemies = 2;
			Run.Items = 1;
			FRandoRoll R;
			if (!Roll(Lv, Run, 0, R))
			{
				Check(false, FString::Printf(TEXT("%s: no roll"), Lv));
				continue;
			}
			TMap<FString, int32> Tally;
			for (const FString& P : R.Pinned) ++Tally.FindOrAdd(P);
			Tally.KeySort([](const FString& X, const FString& Y) { return X < Y; });
			TArray<FString> Parts;
			for (const TPair<FString, int32>& Kv : Tally) Parts.Add(Kv.Value > 1 ? FString::Printf(TEXT("%s x%d"), *Kv.Key, Kv.Value) : Kv.Key);
			UE_LOG(LogOutlaws, Display, TEXT("OLRANDO pinned %s: %d bad guys may change, %d pickups may move, %d pinned: %s"), Lv, R.Eligible, R.Movable, R.Pinned.Num(), *FString::Join(Parts, TEXT("; ")));
			Check(R.PinnedMoved == 0 && R.bCountsKept, FString::Printf(TEXT("%s: Shuffle keeps the pinned and the counts"), Lv));
		}
		// the mission order: fixed for the run, the first and the last kept by Shuffle middle
		RandoStory(Story, Movies);
		RandoLevelOrder(RunOrder);
		TArray<FString> Again;
		RandoLevelOrder(Again);
		TArray<FString> S1 = Story, S2 = RunOrder;
		S1.Sort();
		S2.Sort();
		UE_LOG(LogOutlaws, Display, TEXT("OLRANDO story order: %s; cutscenes after each: %s"), *FString::Join(Story, TEXT(", ")), *FString::Join(Movies, TEXT(", ")));
		UE_LOG(LogOutlaws, Display, TEXT("OLRANDO run order (seed %u): %s"), RandoRun.Seed, *FString::Join(RunOrder, TEXT(", ")));
		Check(RunOrder == Again && S1 == S2 && RunOrder != Story, TEXT("the run's order is the story's levels, shuffled, the same each time asked"));
		if (RandoRun.Order == 1) Check(RunOrder.Num() > 2 && RunOrder[0] == Story[0] && RunOrder.Last() == Story.Last(), TEXT("Shuffle middle: the first and the last in place"));
		Check(SaveGame(-1), TEXT("saved (the autosave)"));
		Check(LoadGame(-1), TEXT("loading it"));
		Step = 1;
		return;
	}
	case 1:
		Check(RandoAttempt == 0 && RandoSum == Sum0, FString::Printf(TEXT("the load builds the saved roll (attempt %d, %08x, saved %08x)"), RandoAttempt, RandoSum, Sum0));
		RestartLevel();
		Step = 2;
		return;
	case 2:
	{
		Sum1 = RandoSum;
		Check(RandoAttempt == 0 && Sum1 == Sum0, FString::Printf(TEXT("Restart Level keeps the roll (attempt %d, %08x after %08x)"), RandoAttempt, Sum1, Sum0));
		TArray<FString> Now;
		RandoLevelOrder(Now);
		Check(Now == RunOrder, TEXT("the mission order is the run's still after the restart"));
		Check(SaveGame(-1) && LoadGame(-1), TEXT("saved and loading again"));
		Step = 3;
		return;
	}
	case 3:
		Check(RandoAttempt == 0 && RandoSum == Sum1, FString::Printf(TEXT("a load after a restart builds that restart's roll (attempt %d, %08x)"), RandoAttempt, RandoSum));
		RestartLevel();
		Step = 4;
		return;
	case 4:
		Check(RandoAttempt == 0 && RandoSum == Sum1, FString::Printf(TEXT("Restart Level after the load keeps the roll (attempt %d, %08x)"), RandoAttempt, RandoSum));
		RunStep = 0;
		Step = 5;
		return;
	case 5:
	{
		// the level is won, as CompleteLevel asks the story (the movie itself not played: the test is of the order)
		const int32 K = RunOrder.IndexOfByKey(LevelName.ToLower());
		FString Mv;
		const FString Next = StoryNext(Mv);
		UE_LOG(LogOutlaws, Display, TEXT("OLRANDO run: level %d (%s) finished: cutscene %s, next %s"), K + 1, *LevelName, *Mv, *Next);
		Check(K >= 0 && Movies.IsValidIndex(K) && Mv == Movies[K], FString::Printf(TEXT("the %d%s level finished shows the story's cutscene %d (%s), whichever level it was (%s; the story's own after it: %s)"),
			K + 1, K == 0 ? TEXT("st") : K == 1 ? TEXT("nd") : K == 2 ? TEXT("rd") : TEXT("th"), K + 1, Movies.IsValidIndex(K) ? *Movies[K] : TEXT("?"), *LevelName,
			Story.IndexOfByKey(LevelName.ToLower()) != INDEX_NONE ? *Movies[Story.IndexOfByKey(LevelName.ToLower())] : TEXT("?")));
		Check(RunOrder.IsValidIndex(K + 1) && Next == RunOrder[K + 1], FString::Printf(TEXT("the next level is the run's (%s)"), *Next));
		Expect = Next;
		NextLevelName = Next;                  // (what the level-end save names: the run's next level)
		NextMovie.Reset();
		StartNextLevel();
		++RunStep;
		Step = 6;
		return;
	}
	case 6:
		Check(LevelName.Equals(Expect, ESearchCase::IgnoreCase) && RandoAttempt == 0, FString::Printf(TEXT("%s built as the run's level %d at attempt %d (checksum %08x)"), *LevelName,
			RunOrder.IndexOfByKey(LevelName.ToLower()) + 1, RandoAttempt, RandoSum));
		Step = RunStep < 3 ? 5 : 7;
		return;
	case 7:
		// a game that never randomized (its save has no "rando" block), loaded while the page has every option on: it plays as
		// it was, and stays so across a restart (the page is taken only at New Game)
		RandoEnemies = 2;
		RandoItems = 2;
		RandoOrder = 2;
		RandoRun = FRandoRun();
		Check(SaveGame(-1), TEXT("saved a game with no randomizer (the page's options all on)"));
		Check(LoadGame(-1), TEXT("loading it"));
		Step = 8;
		return;
	case 8:
		Check(!RandoOn() && RandoSum == 0, FString::Printf(TEXT("a save from an un-randomized game loads un-randomized (on %d, checksum %08x)"), RandoOn() ? 1 : 0, RandoSum));
		RestartLevel();
		Step = 9;
		return;
	case 9:
	{
		Check(!RandoOn() && RandoSum == 0, FString::Printf(TEXT("and stays un-randomized after Restart Level (on %d, checksum %08x)"), RandoOn() ? 1 : 0, RandoSum));
		FString Mv;
		const FString Next = StoryNext(Mv);
		TArray<FString> Lv, Mvs;
		RandoStory(Lv, Mvs);
		const int32 K = Lv.IndexOfByKey(LevelName.ToLower());
		Check(K >= 0 && Next.Equals(Lv.IsValidIndex(K + 1) ? Lv[K + 1] : FString(), ESearchCase::IgnoreCase), FString::Printf(TEXT("its next level is the story's own (%s after %s)"), *Next, *LevelName));
		Step = 99;
		Finish();
		return;
	}
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS

// ---- -OLRandoBossTest (BUG-RANDOPOOL, REQ-RANDOBOSS) ----------------------------------------------------------------------------
// Run as: -OLLevel=hideout -OLNoFrontEnd -OLRando=2,2,0,1 -OLRandoSeed=1 -OLRandoBossTest.  The pickups of the hideout and the
// town over seeds 1 to 20, re-rolled from the level's own mix (as it was) and from the game's pool (now), tallied by item; the
// random bosses of the hideout, the town and the canyon over the same seeds; then in each of the three a roll with a random boss
// built, he is killed and the level goes on, then the level's own boss is killed and the level ends.

#if WITH_OUTLAWS_TESTS
void AOutlawsLevelActor::TickRandoBossTest(float Dt)
{
	static int32 Step = 0, Pass = 0, Fail = 0, LvAt = 0;
	static float Wait = 0.f, Total = 0.f;
	static const TCHAR* const Lvs[] = { TEXT("hideout"), TEXT("townex"), TEXT("canyon") };
	auto Check = [](bool bOk, const FString& What)
	{
		(bOk ? Pass : Fail)++;
		UE_LOG(LogOutlaws, Display, TEXT("OLRANDO boss test %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
	};
	Total += Dt;
	if (Step >= 99) return;
	if (Total > 400.f)
	{
		Check(false, FString::Printf(TEXT("timed out at step %d"), Step));
		UE_LOG(LogOutlaws, Display, TEXT("OLRANDO boss test done: %d passed, %d failed"), Pass, Fail);
		Step = 99;
		FPlatformMisc::RequestExit(false);
		return;
	}
	bInvincible = true;
	bEnemiesActive = false;
	Wait += Dt;
	// (step 3, the level's end: its movie may be up, the level cleared away under it)
	if ((Step != 3 && (bRebuildPending || bNeedPlace || !Inf || PendingLoad.IsValid() || bFrontEnd)) || Wait < (Step == 3 ? 0.6f : 2.f)) return;     // (the end: a bad guy ticks every quarter second or so when not near)
	Wait = 0.f;
	const uint32 DiffBit = OutlawsDifficulty::ObjectBit(SpawnDifficulty);
	auto Roll = [this, DiffBit](const FString& Lv, const FRandoRun& Run, int32 Attempt, FRandoRoll& Out)
	{
		FString Lvt, Obt, InfT;
		if (!AssetsRef || !AssetsRef->GetText(Lv + TEXT(".lvt"), Lvt)) return false;
		AssetsRef->GetText(Lv + TEXT(".obt"), Obt);
		AssetsRef->GetText(Lv + TEXT(".inf"), InfT);
		FOutlawsLevel Lx;
		Lx.ParseLvt(Lvt);
		Lx.ParseObt(Obt);
		return RollRando(Lx, Lv, InfT, DiffBit, Run, Attempt, Out);
	};
	// a roll of this level (bad guys anything, bosses in the mix) with a random boss in it: the attempt for its build
	auto BossAttempt = [&](const FString& Lv)
	{
		FRandoRun Run = RandoRun;
		Run.Enemies = 2;
		Run.Bosses = 1;
		for (int32 k = 0; k < 60; ++k)
		{
			FRandoRoll R;
			if (Roll(Lv, Run, k, R) && R.RandomBoss.Num() > 0) return k;
		}
		return -1;
	};
	auto Build = [&](const FString& Lv)
	{
		RandoRun.Enemies = 2;
		RandoRun.Bosses = 1;
		const int32 K = BossAttempt(Lv);
		Check(K >= 0, FString::Printf(TEXT("%s: a roll with a random boss at attempt %d"), *Lv, K));
		RandoAttemptLevel = Lv;
		RandoPendingAttempt = FMath::Max(0, K);
		if (LevelName.Equals(Lv, ESearchCase::IgnoreCase)) RestartLevel();
		else JumpToLevel(Lv);
	};
	switch (Step)
	{
	case 0:
	{
		// the pickups, by item, over 20 seeds: as it was (the level's own mix) and now (the game's pool)
		for (const TCHAR* Lv : { TEXT("hideout"), TEXT("townex") })
		{
			for (int32 Way = 0; Way < 2; ++Way)
			{
				OLRando::GOldItemPool = Way == 0;
				TMap<FString, int32> Tally;
				int32 Spots = 0, PinnedMoved = 0, WeaponsLost = 0;
				for (uint32 Seed = 1; Seed <= 20; ++Seed)
				{
					FRandoRun Run = RandoRun;
					Run.Seed = Seed;
					Run.Enemies = 0;
					Run.Items = 2;
					Run.Bosses = 0;
					FRandoRoll R;
					if (!Roll(Lv, Run, 0, R)) continue;
					for (const TPair<FString, int32>& Kv : R.ItemKinds) { Tally.FindOrAdd(Kv.Key) += Kv.Value; Spots += Kv.Value; }
					PinnedMoved += R.PinnedMoved;
					WeaponsLost += R.bWeaponsKept ? 0 : 1;
				}
				Tally.ValueSort([](int32 X, int32 Y) { return X > Y; });
				TArray<FString> Parts;
				for (const TPair<FString, int32>& Kv : Tally) Parts.Add(FString::Printf(TEXT("%s %d"), *Kv.Key, Kv.Value));
				int32 Top = 0;
				for (const TPair<FString, int32>& Kv : Tally) { Top = Kv.Value; break; }
				UE_LOG(LogOutlaws, Display, TEXT("OLRANDO pool %s %s: %d spots over 20 seeds, %d kinds, the commonest %.0f %%: %s"), Lv, Way == 0 ? TEXT("BEFORE (the level's mix)") : TEXT("AFTER (the game's pool)"),
					Spots, Tally.Num(), Spots ? 100.0 * Top / Spots : 0.0, *FString::Join(Parts, TEXT(", ")));
				Check(PinnedMoved == 0 && WeaponsLost == 0, FString::Printf(TEXT("%s %s: no pinned item moved (%d), every weapon the level places still placed (%d rolls lost one)"), Lv,
					Way == 0 ? TEXT("before") : TEXT("after"), PinnedMoved, WeaponsLost));
			}
		}
		OLRando::GOldItemPool = false;
		// the random bosses over 20 seeds
		for (const TCHAR* Lv : Lvs)
		{
			int32 NBoss = 0, With = 0, Most = 0, Pinned = 0;
			bool bSame = true;
			TMap<FString, int32> Who;
			for (uint32 Seed = 1; Seed <= 20; ++Seed)
			{
				FRandoRun Run = RandoRun;
				Run.Seed = Seed;
				Run.Enemies = 2;
				Run.Items = 2;
				Run.Bosses = 1;
				FRandoRoll R, R2;
				if (!Roll(Lv, Run, 0, R) || !Roll(Lv, Run, 0, R2)) continue;
				bSame &= R.Sum == R2.Sum;
				NBoss += R.RandomBoss.Num();
				With += R.RandomBoss.Num() > 0 ? 1 : 0;
				Most = FMath::Max(Most, R.RandomBoss.Num());
				Pinned += R.PinnedMoved;
				for (const int32 Row : R.RandomBoss) ++Who.FindOrAdd(R.Rename[Row]);
			}
			TArray<FString> Parts;
			for (const TPair<FString, int32>& Kv : Who) Parts.Add(FString::Printf(TEXT("%s %d"), *Kv.Key, Kv.Value));
			UE_LOG(LogOutlaws, Display, TEXT("OLRANDO bosses %s over 20 seeds: %d random bosses, %d rolls with one or more, at most %d in a roll: %s"), Lv, NBoss, With, Most, *FString::Join(Parts, TEXT(", ")));
			Check(bSame && Pinned == 0 && Most <= 2, FString::Printf(TEXT("%s: the same seed the same roll, nothing pinned changed, at most two"), Lv));
		}
		{
			FRandoRun Off = RandoRun;
			Off.Seed = 7;
			Off.Enemies = 2;
			Off.Items = 2;
			Off.Bosses = 0;
			FRandoRoll R;
			Roll(TEXT("hideout"), Off, 0, R);
			Check(R.RandomBoss.Num() == 0, TEXT("Bosses in the mix off: no random boss"));
		}
		LvAt = 0;
		Build(Lvs[0]);
		Step = 1;
		return;
	}
	case 1:
	{
		Check(LevelName.Equals(Lvs[LvAt], ESearchCase::IgnoreCase) && RandomBossActors.Num() > 0, FString::Printf(TEXT("%s built at attempt %d with %d random boss(es)"), *LevelName, RandoAttempt, RandomBossActors.Num()));
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : RandomBossActors)
		{
			AOutlawsEnemy* E = Wk.Get();
			if (!E) continue;
			if (E->IsDormant()) E->SetDormant(false);
			const FVector At = E->GetActorLocation();
			for (int32 k = 0; k < 80 && !E->IsDead(); ++k) E->TakeHit(50.f, At, FVector(0.0, 0.0, -1.0), At + FVector(0.0, 0.0, 200.0));
			UE_LOG(LogOutlaws, Display, TEXT("OLRANDO boss test: random boss %s killed (dead %d, a boss %d)"), *E->GetOutlawsName(), E->IsDead() ? 1 : 0, E->bBoss ? 1 : 0);
		}
		Step = 2;
		return;
	}
	case 2:
	{
		Check(LevelEndTimer < 0.f && !bBossDefeated, FString::Printf(TEXT("%s: the random boss(es) dead, the level goes on (end timer %.2f, won %d)"), *LevelName, LevelEndTimer, bBossDefeated ? 1 : 0));
		// the level's own named outlaws, killed
		for (const FBossSlot& S : BossSlots)
		{
			AOutlawsEnemy* B = S.Actor.Get();
			if (!B || B->IsDead() || B->bVoiceOnly) continue;
			if (B->IsDormant() && S.Spawns.Num() > 0) B->Appear(S.Spawns[0]);
			// (by the player: a far one is frozen and would not report his fall till he thawed)
			if (const APawn* PP = GetWorld()->GetFirstPlayerController() ? GetWorld()->GetFirstPlayerController()->GetPawn() : nullptr)
			{
				B->SetActorLocation(PP->GetActorLocation() + PP->GetActorForwardVector() * 250.0, false, nullptr, ETeleportType::TeleportPhysics);
			}
			const FVector At = B->GetActorLocation();
			for (int32 k = 0; k < 80 && !B->IsDead(); ++k) B->TakeHit(50.f, At, FVector(0.0, 0.0, -1.0), At + FVector(0.0, 0.0, 200.0));
			UE_LOG(LogOutlaws, Display, TEXT("OLRANDO boss test: the level's own %s killed (dead %d)"), *S.Name, B->IsDead() ? 1 : 0);
		}
		Step = 3;
		return;
	}
	case 3:
		Check(LevelEndTimer >= 0.f || bBossDefeated || Movie != nullptr || !LevelName.Equals(Lvs[LvAt], ESearchCase::IgnoreCase), FString::Printf(TEXT("%s: the level's own boss dead, the level ends as it should (end timer %.2f, won %d)"), *LevelName, LevelEndTimer, bBossDefeated ? 1 : 0));
		LevelEndTimer = -1.f;
		if (++LvAt < 3)
		{
			Build(Lvs[LvAt]);
			Step = 1;
			return;
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLRANDO boss test done: %d passed, %d failed"), Pass, Fail);
		Step = 99;
		FPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS

// ---- -OLRandoCritterTest (REQ-RANDOCRITTERS) ------------------------------------------------------------------------------------
// Run as: -OLLevel=hideout -OLNoFrontEnd -OLRando=2,0,0 -OLRandoCritterTest.  The town's bad guys rolled at seeds 1, 2 and 3 (and
// over seeds 1 to 20 for the shares), Enemies Anything and Same strength, three ways: as it was (every kind drawn as often as
// any other, critters in), Critters On and Critters Off.  Off: no critter or special enemy drawn, the level's own kept; the
// same seed and settings the same roll; On: their share near the game's own (by its placements), and no more than before.

#if WITH_OUTLAWS_TESTS
void AOutlawsLevelActor::TickRandoCritterTest(float Dt)
{
	static bool bDone = false;
	static float Ready = 0.f;
	if (bDone) return;
	const bool bReady = !bRebuildPending && !bNeedPlace && Inf && !PendingLoad.IsValid() && !bFrontEnd;
	Ready = bReady ? Ready + Dt : 0.f;
	if (Ready < 2.f) return;
	bDone = true;
	bInvincible = true;
	int32 Pass = 0, Fail = 0;
	auto Check = [&Pass, &Fail](bool bOk, const FString& What)
	{
		(bOk ? Pass : Fail)++;
		UE_LOG(LogOutlaws, Display, TEXT("OLRANDO critter test %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
	};
	const uint32 DiffBit = OutlawsDifficulty::ObjectBit(SpawnDifficulty);
	auto Roll = [this, DiffBit](const FString& Lv, const FRandoRun& Run, int32 Attempt, FRandoRoll& Out)
	{
		FString Lvt, Obt, InfT;
		if (!AssetsRef || !AssetsRef->GetText(Lv + TEXT(".lvt"), Lvt)) return false;
		AssetsRef->GetText(Lv + TEXT(".obt"), Obt);
		AssetsRef->GetText(Lv + TEXT(".inf"), InfT);
		FOutlawsLevel Lx;
		Lx.ParseLvt(Lvt);
		Lx.ParseObt(Obt);
		return RollRando(Lx, Lv, InfT, DiffBit, Run, Attempt, Out);
	};
	const TCHAR* const Lv = TEXT("townex");
	const TCHAR* const WayName[3] = { TEXT("BEFORE (as it was)"), TEXT("AFTER, Critters On"), TEXT("AFTER, Critters Off") };
	for (int32 Mode = 2; Mode >= 1; --Mode)
	{
		double Share[3] = { 0.0, 0.0, 0.0 };
		int32 Own[3] = { -1, -1, -1 };
		for (int32 Way = 0; Way < 3; ++Way)
		{
			OLRando::GOldKindWeights = Way == 0;
			FRandoRun Run = RandoRun;
			Run.Enemies = Mode;
			Run.Items = 0;
			Run.Bosses = 0;
			Run.Critters = Way == 2 ? 0 : 1;
			TArray<FString> Said;
			int32 Picks = 0, Eligible = 0;
			bool bSame = true;
			for (uint32 Seed = 1; Seed <= 20; ++Seed)
			{
				Run.Seed = Seed;
				FRandoRoll R, R2;
				if (!Roll(Lv, Run, 0, R) || !Roll(Lv, Run, 0, R2)) { Check(false, FString::Printf(TEXT("%s seed %u: no roll"), Lv, Seed)); continue; }
				bSame &= R.Sum == R2.Sum;
				Picks += R.SpecialPicks;
				Eligible += R.Eligible;
				Own[Way] = R.OwnSpecials;
				if (Seed <= 3) Said.Add(FString::Printf(TEXT("seed %u: %d critters or specials drawn of %d bad guys, %d the level's own (%s)"), Seed, R.SpecialPicks, R.Eligible, R.OwnSpecials, *R.Kinds));
				if (Way == 2 && R.SpecialPicks != 0) Check(false, FString::Printf(TEXT("%s, %s, seed %u: %d critters or specials drawn with Critters Off"), Lv, *OLRando::ModeText(0, Mode), Seed, R.SpecialPicks));
			}
			Share[Way] = Eligible ? 100.0 * Picks / Eligible : 0.0;
			UE_LOG(LogOutlaws, Display, TEXT("OLRANDO critters %s, %s, %s: %s; over 20 seeds %d of %d (%.1f %%), the game's own share %.1f %%"), Lv, *OLRando::ModeText(0, Mode), WayName[Way],
				*FString::Join(Said, TEXT("; ")), Picks, Eligible, Share[Way], OLRando::GSpecialShare);
			Check(bSame, FString::Printf(TEXT("%s, %s, %s: the same seed and settings, the same roll"), Lv, *OLRando::ModeText(0, Mode), WayName[Way]));
			if (Way == 2) Check(Picks == 0, FString::Printf(TEXT("%s, %s, Critters Off: no critter or special enemy drawn over 20 seeds (%d)"), Lv, *OLRando::ModeText(0, Mode), Picks));
		}
		OLRando::GOldKindWeights = false;
		Check(Own[2] == Own[1] && Own[1] >= 0, FString::Printf(TEXT("%s, %s: the level's own critters and specials stay with Critters Off (%d, %d with it On)"), Lv, *OLRando::ModeText(0, Mode), Own[2], Own[1]));
		Check(Share[1] <= Share[0] + 0.01 && Share[1] <= 2.0 * OLRando::GSpecialShare + 1.0, FString::Printf(TEXT("%s, %s, Critters On: %.1f %% drawn, no more than before (%.1f %%) and near the game's own %.1f %%"), Lv,
			*OLRando::ModeText(0, Mode), Share[1], Share[0], OLRando::GSpecialShare));
	}
	// Critters Off is another roll than On (the same seed): the setting is part of the roll
	{
		FRandoRun On = RandoRun, Off = RandoRun;
		On.Enemies = Off.Enemies = 2;
		On.Items = Off.Items = 0;
		On.Seed = Off.Seed = 1;
		On.Critters = 1;
		Off.Critters = 0;
		FRandoRoll R1, R0;
		const bool bOk = Roll(Lv, On, 0, R1) && Roll(Lv, Off, 0, R0);
		Check(bOk && (R1.SpecialPicks == 0 || R1.Sum != R0.Sum), FString::Printf(TEXT("%s seed 1: On and Off rolls (%08x, %08x)"), Lv, R1.Sum, R0.Sum));
	}
	Check(RandoRun.bSet, FString::Printf(TEXT("this run's own: enemies %d, critters %d (the page's default Off unless the test says)"), RandoRun.Enemies, RandoRun.Critters));
	UE_LOG(LogOutlaws, Display, TEXT("OLRANDO critter test done: %d passed, %d failed"), Pass, Fail);
	FPlatformMisc::RequestExit(false);
}
#endif // WITH_OUTLAWS_TESTS
