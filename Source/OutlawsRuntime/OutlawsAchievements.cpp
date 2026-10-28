// REQ-EXTRAS: the achievements, their store and the hooks that earn them (OutlawsAchievements.h has the layout).
#include "OutlawsAchievements.h"
#include "OutlawsUnlocks.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsCoop.h"
#include "OutlawsPerfTest.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/Paths.h"

namespace
{
	// (the remaster's where we can tell they are done, and ours; the order the page lists them)
	const TArray<OutlawsAchievements::FDef>& Defs()
	{
		static const TArray<OutlawsAchievements::FDef> D = {
			{ TEXT("story"), TEXT("out_il_buono"), TEXT("The end of the trail"), TEXT("Finish the main story on any difficulty"), 3, false },
			{ TEXT("story_bad"), TEXT("out_il_bruto"), TEXT("Bad to the end"), TEXT("Finish the main story on Bad or Ugly"), 12, false },
			{ TEXT("story_ugly"), TEXT("out_il_cattivo"), TEXT("Ugly to the end"), TEXT("Finish the main story on Ugly"), 20, false },
			{ TEXT("story_coop"), nullptr, TEXT("Riding together"), TEXT("Finish the main story in co-op"), 0, false },
			{ TEXT("boss_first"), nullptr, TEXT("First outlaw down"), TEXT("Beat a named outlaw in the main story"), 0, false },
			// (user 20 Oct: one for each named outlaw of the story, in the order they are met; BossOf has whose is whose)
			{ TEXT("boss_slim"), nullptr, TEXT("Slim pickings"), TEXT("Beat Slim at the hideout"), 0, false },
			{ TEXT("boss_sanchez"), nullptr, TEXT("Locked up for good"), TEXT("Beat Jack Sanchez in the town"), 0, false },
			{ TEXT("boss_bloodeye"), nullptr, TEXT("End of the line"), TEXT("Beat Bloodeye Tim on the train"), 0, false },
			{ TEXT("boss_dick"), nullptr, TEXT("Short fuse"), TEXT("Beat Dick Farmer in the canyon"), 0, false },
			{ TEXT("boss_henry"), nullptr, TEXT("Sawdust"), TEXT("Beat Henry at the sawmill"), 0, false },
			{ TEXT("boss_simms"), nullptr, TEXT("The fort falls"), TEXT("Beat Simms at Simm's Fort"), 0, false },
			{ TEXT("boss_mary"), nullptr, TEXT("Last orders"), TEXT("Beat Bloody Mary in the mine"), 0, false },
			{ TEXT("boss_drdeath"), TEXT("out_quack"), TEXT("Doctor's orders"), TEXT("Beat Dr. Death"), 2, false },
			{ TEXT("boss_twofeathers"), nullptr, TEXT("Cliffhanger"), TEXT("Beat Two-Feathers at the cliff dwellings"), 0, false },
			{ TEXT("boss_bill"), nullptr, TEXT("Buckshot spent"), TEXT("Beat Bill Morgan at the ranch"), 0, false },
			{ TEXT("boss_bob"), TEXT("out_too_ambitious"), TEXT("The last of the gang"), TEXT("Beat Bob Graham"), 26, true },
			{ TEXT("true_grit"), TEXT("out_true_grit"), TEXT("Hanging on"), TEXT("Finish a level with less than 1 health"), 8, false },
			{ TEXT("secret"), TEXT("out_lookee_here"), TEXT("Off the trail"), TEXT("Find a secret"), 1, false },
			{ TEXT("secrets_all"), TEXT("out_searcher"), TEXT("The searcher"), TEXT("Find every secret in the main story"), 23, false },
			{ TEXT("par"), nullptr, TEXT("Quick on the draw"), TEXT("Finish a main story level under its par time (no cheats, Randomizer or Bad guys multiplier)"), 0, false },
			// (user 20 Oct: earned by using the gun; the remaster's line says getting it, so the line here is ours)
			{ TEXT("gatling"), TEXT("out_fully_loaded"), TEXT("Fully loaded"), TEXT("Fire the gatling gun"), 18, false, true },
			{ TEXT("dynamite3"), TEXT("out_three_coffins"), TEXT("Three coffins"), TEXT("Kill three bad guys with one stick of dynamite"), 19, false },
			{ TEXT("mt_deputy"), TEXT("out_u_favourite"), TEXT("Deputy"), TEXT("Reach Deputy in Marshal Training"), 9, false },
			{ TEXT("mt_sheriff"), TEXT("out_honest"), TEXT("Sheriff"), TEXT("Reach Sheriff in Marshal Training"), 13, false },
			{ TEXT("mt_marshal"), TEXT("out_wear_badge"), TEXT("Marshal"), TEXT("Reach Marshal in Marshal Training"), 24, false },
			{ TEXT("mt_killed"), TEXT("out_dead_or_alive"), TEXT("Dead"), TEXT("Kill all five outlaws in Marshal Training"), 10, false },
			{ TEXT("mt_captured"), TEXT("out_good_cop"), TEXT("Alive"), TEXT("Capture all five outlaws in Marshal Training"), 21, false },
			{ TEXT("handful_civlwar1"), TEXT("out_west_won"), TEXT("Civil War"), TEXT("Finish Civil War"), 4, false },
			{ TEXT("handful_wharf"), TEXT("out_shades_jw"), TEXT("Wharf Town"), TEXT("Finish Wharf Town"), 5, false },
			{ TEXT("handful_villa"), TEXT("out_el_mariscal"), TEXT("Villa"), TEXT("Finish Villa"), 6, false },
			{ TEXT("handful_icecaves"), TEXT("out_slip_slide"), TEXT("Ice Caves"), TEXT("Finish Ice Caves"), 7, false },
			{ TEXT("boxes_first"), nullptr, TEXT("Something in the corner"), TEXT("Find a hidden box"), 0, false },
			{ TEXT("boxes_half"), nullptr, TEXT("Collector"), TEXT("Find half of the hidden boxes"), 0, false },
			{ TEXT("boxes_all"), nullptr, TEXT("The whole set"), TEXT("Find every hidden box"), 0, true },
			{ TEXT("mp_win"), nullptr, TEXT("Top gun"), TEXT("Win a multiplayer match against other players"), 0, false },
			{ TEXT("bots_win"), nullptr, TEXT("Target practice"), TEXT("Win a match against bots"), 0, false },
			{ TEXT("tutorial"), nullptr, TEXT("Greenhorn"), TEXT("Finish the tutorial"), 0, false } };
		return D;
	}

	// the five Marshal Training missions (the posters' levels)
	constexpr int32 MtMissions = 5;

	TArray<FString> GToastQueue;
	TArray<FString> GToastsShown;

	TArray<FString> ProgressList(const TSharedPtr<FJsonObject>& P, const FString& Key)
	{
		TArray<FString> Out;
		const TSharedPtr<FJsonObject>* Pr = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
		if (P.IsValid() && P->TryGetObjectField(TEXT("progress"), Pr) && Pr && Pr->IsValid() && (*Pr)->TryGetArrayField(Key, A))
		{
			for (const TSharedPtr<FJsonValue>& V : *A) if (V.IsValid() && V->Type == EJson::String) Out.AddUnique(V->AsString().ToLower());
		}
		return Out;
	}

	// Value added to the profile's progress list Key (once); the count after
	int32 AddProgress(AOutlawsLevelActor* L, const FString& Key, const FString& Value)
	{
		int32 Count = 0;
		OutlawsUnlocks::ProfileChange(L, [&](const TSharedPtr<FJsonObject>& P)
		{
			if (!P.IsValid()) return;
			const TSharedPtr<FJsonObject>* Pr = nullptr;
			if (!P->TryGetObjectField(TEXT("progress"), Pr) || !Pr || !Pr->IsValid())
			{
				P->SetObjectField(TEXT("progress"), MakeShared<FJsonObject>());
				P->TryGetObjectField(TEXT("progress"), Pr);
			}
			TArray<FString> Have = ProgressList(P, Key);
			Have.AddUnique(Value.ToLower());
			TArray<TSharedPtr<FJsonValue>> A;
			for (const FString& S : Have) A.Add(MakeShared<FJsonValueString>(S));
			(*Pr)->SetArrayField(Key, A);
			Count = Have.Num();
		});
		return Count;
	}

	// (REQ-EXTRAS secrets_all) the story levels' secret sector counts kept so far: progress.secret_totals { "hideout": 3, ... }
	TMap<FString, int32> SecretTotals(const TSharedPtr<FJsonObject>& P)
	{
		TMap<FString, int32> Out;
		const TSharedPtr<FJsonObject>* Pr = nullptr;
		const TSharedPtr<FJsonObject>* T = nullptr;
		if (P.IsValid() && P->TryGetObjectField(TEXT("progress"), Pr) && Pr && Pr->IsValid() && (*Pr)->TryGetObjectField(TEXT("secret_totals"), T) && T && T->IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Kv : (*T)->Values) if (Kv.Value.IsValid() && Kv.Value->Type == EJson::Number) Out.Add(Kv.Key.ToLower(), int32(Kv.Value->AsNumber()));
		}
		return Out;
	}

	// the story levels whose every secret is found (their count kept and as many "level:sector" entries in progress.secrets)
	int32 LevelsAllSecrets(const TSharedPtr<FJsonObject>& P)
	{
		TArray<FString> Levels, Labels;
		OutlawsAchievements::BoxLevels(Levels, Labels);
		const TMap<FString, int32> Totals = SecretTotals(P);
		const TArray<FString> Found = ProgressList(P, TEXT("secrets"));
		int32 N = 0;
		for (const FString& Lv : Levels)
		{
			const int32* T = Totals.Find(Lv);
			if (!T) continue;
			int32 Here = 0;
			for (const FString& F : Found) Here += F.StartsWith(Lv + TEXT(":")) ? 1 : 0;
			N += Here >= *T ? 1 : 0;
		}
		return N;
	}

	// the story's named outlaws: the level, his FUNC, his achievement (the mill's Henry is a DrDeath_New as the mine's Dr. Death is)
	const TCHAR* BossOf(const FString& Level, const FString& Func)
	{
		static const TCHAR* const Bosses[][3] = { { TEXT("hideout"), TEXT("slim_new"), TEXT("boss_slim") }, { TEXT("townex"), TEXT("sanchez_new"), TEXT("boss_sanchez") },
			{ TEXT("train"), TEXT("bloodeye_new"), TEXT("boss_bloodeye") }, { TEXT("canyon"), TEXT("farmer_new"), TEXT("boss_dick") }, { TEXT("mill"), TEXT("drdeath_new"), TEXT("boss_henry") },
			{ TEXT("simms"), TEXT("simms_new"), TEXT("boss_simms") }, { TEXT("miner"), TEXT("mary_new"), TEXT("boss_mary") }, { TEXT("miner"), TEXT("drdeath_new"), TEXT("boss_drdeath") },
			{ TEXT("cliff"), TEXT("twofeathers_new"), TEXT("boss_twofeathers") }, { TEXT("ranch"), TEXT("buckshot_new"), TEXT("boss_bill") }, { TEXT("ranch"), TEXT("bob_new"), TEXT("boss_bob") } };
		for (const auto& B : Bosses) if (Level.Equals(B[0], ESearchCase::IgnoreCase) && Func.Equals(B[1], ESearchCase::IgnoreCase)) return B[2];
		return nullptr;
	}

	AOutlawsPlayer* PlayerOf(const AOutlawsLevelActor* L)
	{
		UWorld* W = L ? L->GetWorld() : nullptr;
		return W && W->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(W->GetFirstPlayerController()->GetPawn()) : nullptr;
	}

	FString Clock(int32 S) { return FString::Printf(TEXT("%d:%02d"), S / 60, S % 60); }
}

const TArray<OutlawsAchievements::FDef>& OutlawsAchievements::All() { return Defs(); }

const OutlawsAchievements::FDef* OutlawsAchievements::Find(const FString& Id)
{
	return Defs().FindByPredicate([&Id](const FDef& D) { return Id.Equals(D.Id, ESearchCase::IgnoreCase); });
}

FString OutlawsAchievements::TitleOf(const FDef& D, AOutlawsPlayer* P)
{
	const FString Loc = D.LocKey && P ? P->MenuLoc(FString::Printf(TEXT("%s_name"), D.LocKey)) : FString();
	return Loc.IsEmpty() ? FString(D.Title) : Loc;
}

FString OutlawsAchievements::DescOf(const FDef& D, AOutlawsPlayer* P)
{
	// (ours where the remaster's line says more than this game can tell: its campaign lines say "or higher", as ours do)
	const FString Loc = D.LocKey && P && !D.bOwnDesc ? P->MenuLoc(FString::Printf(TEXT("%s_desc"), D.LocKey)) : FString();
	return Loc.IsEmpty() ? FString(D.Desc) : Loc;
}

bool OutlawsAchievements::IsEarned(const AOutlawsLevelActor* L, const FString& Id)
{
	return OutlawsUnlocks::IsEarned(L, *Id);
}

FString OutlawsAchievements::EarnedLine(const AOutlawsLevelActor* L, const FString& Id)
{
	const TSharedPtr<FJsonObject> P = OutlawsUnlocks::ProfileRead(L);
	const TSharedPtr<FJsonObject>* E = nullptr;
	const TSharedPtr<FJsonObject>* R = nullptr;
	if (!P.IsValid() || !P->TryGetObjectField(TEXT("earned"), E) || !E || !E->IsValid() || !(*E)->TryGetObjectField(Id, R) || !R || !R->IsValid()) return FString();
	FString When;
	FDateTime At;
	if ((*R)->TryGetStringField(TEXT("when"), When) && FDateTime::Parse(When, At)) {
		static const TCHAR* const Months[] = { TEXT("Jan"), TEXT("Feb"), TEXT("Mar"), TEXT("Apr"), TEXT("May"), TEXT("Jun"), TEXT("Jul"), TEXT("Aug"), TEXT("Sep"), TEXT("Oct"), TEXT("Nov"), TEXT("Dec") };
		return FString::Printf(TEXT("Earned %d %s %d"), At.GetDay(), Months[FMath::Clamp(At.GetMonth() - 1, 0, 11)], At.GetYear());
	}
	return TEXT("Earned");
}

FString OutlawsAchievements::Progress(const AOutlawsLevelActor* L, const FString& Id)
{
	const TSharedPtr<FJsonObject> P = OutlawsUnlocks::ProfileRead(L);
	if (Id.StartsWith(TEXT("boxes_")))
	{
		TArray<FString> Levels, Labels;
		BoxLevels(Levels, Labels);
		int32 N = 0;
		for (const FString& Lv : ProgressList(P, TEXT("boxes"))) N += Levels.Contains(Lv) ? 1 : 0;
		return FString::Printf(TEXT("Boxes found: %d of %d"), N, Levels.Num());
	}
	if (Id == TEXT("mt_killed") || Id == TEXT("mt_captured"))
	{
		return FString::Printf(TEXT("%s: %d of %d"), Id == TEXT("mt_killed") ? TEXT("Outlaws killed") : TEXT("Outlaws captured"), FMath::Min(ProgressList(P, Id).Num(), MtMissions), MtMissions);
	}
	if (Id == TEXT("secrets_all"))
	{
		TArray<FString> Levels, Labels;
		BoxLevels(Levels, Labels);
		return FString::Printf(TEXT("Levels with every secret found: %d of %d"), LevelsAllSecrets(P), Levels.Num());
	}
	if (Id == TEXT("par"))
	{
		// the kept finish nearest its level's par (the most under it, else the least over), any story level and difficulty
		TArray<FString> Levels, Labels;
		BoxLevels(Levels, Labels);
		int32 Best = -1, BestPar = -1;
		FString Where;
		const TSharedPtr<FJsonObject>* Times = nullptr;
		if (P.IsValid() && P->TryGetObjectField(TEXT("times"), Times) && Times && Times->IsValid())
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Lv : (*Times)->Values)
			{
				const int32 k = Levels.IndexOfByKey(Lv.Key.ToLower());
				if (k == INDEX_NONE || !Lv.Value.IsValid() || Lv.Value->Type != EJson::Object) continue;
				for (const TPair<FString, TSharedPtr<FJsonValue>>& Df : Lv.Value->AsObject()->Values)
				{
					double S = -1.0;
					const int32 Par = ParSeconds(Levels[k]);
					if (Df.Value.IsValid() && Df.Value->Type == EJson::Object && Df.Value->AsObject()->TryGetNumberField(TEXT("seconds"), S) && S >= 0.0
						&& (Best < 0 || int32(S) - Par < Best - BestPar))
					{
						Best = int32(S);
						BestPar = Par;
						Where = Labels[k];
					}
				}
			}
		}
		return Best < 0 ? FString(TEXT("Your nearest to par: none yet")) : FString::Printf(TEXT("Your nearest to par: %s %s (par %s)"), *Where, *Clock(Best), *Clock(BestPar));
	}
	return FString();
}

int32 OutlawsAchievements::EarnedCount(const AOutlawsLevelActor* L)
{
	int32 N = 0;
	for (const FDef& D : Defs()) N += IsEarned(L, D.Id) ? 1 : 0;
	return N;
}

TArray<FString> OutlawsAchievements::Earn(AOutlawsLevelActor* L, const TArray<FString>& Ids, const TCHAR* Where)
{
	TArray<FString> New;
	for (const FString& Id : Ids) if (Find(Id) && !IsEarned(L, Id)) New.AddUnique(Id);
	if (New.Num() == 0) return New;
	if (OutlawsPerfTest::IsOn())
	{
		// (REQ-PERFTESTMODE: nothing earned in a performance test run, nor toasted)
		UE_LOG(LogOutlaws, Log, TEXT("OLACH not earned in a performance test run: %s (%s)"), *FString::Join(New, TEXT(", ")), Where);
		return TArray<FString>();
	}
	const bool bCoop = L && UOutlawsCoop::IsActive(L);
	const FString Diff = L ? OutlawsDifficulty::Name(L->Difficulty) : TEXT("Good");
	OutlawsUnlocks::ProfileChange(L, [&](const TSharedPtr<FJsonObject>& P)
	{
		if (!P.IsValid()) return;
		const TSharedPtr<FJsonObject>* E = nullptr;
		if (!P->TryGetObjectField(TEXT("earned"), E) || !E || !E->IsValid())
		{
			P->SetObjectField(TEXT("earned"), MakeShared<FJsonObject>());
			P->TryGetObjectField(TEXT("earned"), E);
		}
		for (const FString& Id : New)
		{
			if ((*E)->HasField(Id)) continue;
			const TSharedRef<FJsonObject> Rec = MakeShared<FJsonObject>();
			Rec->SetStringField(TEXT("when"), FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S")));
			Rec->SetStringField(TEXT("difficulty"), Diff);
			Rec->SetBoolField(TEXT("coop"), bCoop);
			(*E)->SetObjectField(Id, Rec);
		}
	});
	AOutlawsPlayer* P = PlayerOf(L);
	for (const FString& Id : New)
	{
		const FString Title = TitleOf(*Find(Id), P);
		GToastQueue.Add(TEXT("Achievement: ") + Title);
		UE_LOG(LogOutlaws, Log, TEXT("OLACH earned %s (\"%s\") on profile %s (%s, %s%s)"), *Id, *Title, *OutlawsUnlocks::ProfileOf(L), Where, *Diff, bCoop ? TEXT(", co-op") : TEXT(""));
	}
	return New;
}

void OutlawsAchievements::QueueToast(AOutlawsLevelActor* L, const FString& Id)
{
	if (const FDef* D = Find(Id)) GToastQueue.Add(TEXT("Achievement: ") + TitleOf(*D, PlayerOf(L)));
}

void OutlawsAchievements::NoteStoryFinish(AOutlawsLevelActor* L, int32 Difficulty, bool bCoop)
{
	TArray<FString> Ids = { TEXT("story") };
	if (Difficulty >= 1) Ids.Add(TEXT("story_bad"));
	if (Difficulty >= 2) Ids.Add(TEXT("story_ugly"));
	if (bCoop) Ids.Add(TEXT("story_coop"));
	Earn(L, Ids, TEXT("the ending"));
}

void OutlawsAchievements::NoteBossDown(AOutlawsLevelActor* L, const FString& Level, const FString& Func)
{
	// (only the main story's levels: a Handful mission's or Marshal Training's outlaw is none of these)
	const TCHAR* Own = BossOf(Level, Func);
	if (!L || L->bHom || !Own) return;
	Earn(L, { TEXT("boss_first"), Own }, *FString::Printf(TEXT("%s down in %s"), *Func.ToLower(), *Level.ToLower()));
}

void OutlawsAchievements::NoteLevelComplete(AOutlawsLevelActor* L, float Health, const FString& NextLevel)
{
	if (!L || L->IsFrontEnd() || L->IsTutorial()) return;
	TArray<FString> Ids;
	// (the original's 100 to a full life is our 10: under 1 of its is under 0.1 of ours)
	if (Health > 0.f && Health < 0.1f) Ids.Add(TEXT("true_grit"));
	// a Handful of Missions mission's last level (no level after it): its own achievement, by its first level
	if (!L->bHom && NextLevel.IsEmpty() && AOutlawsLevelActor::IsHandfulLevel(L->LevelName))
	{
		const FString Lv = L->LevelName.ToLower();
		Ids.Add(TEXT("handful_") + (Lv == TEXT("civlwar2") ? FString(TEXT("civlwar1")) : Lv));
	}
	if (Ids.Num() > 0) Earn(L, Ids, *FString::Printf(TEXT("%s won"), *L->LevelName));
	NoteSecretTotal(L, L->LevelName, L->SecretSectorCount());      // (secrets_all: a story level played through has its count kept)
}

void OutlawsAchievements::NoteHomEnd(AOutlawsLevelActor* L, const FString& Mission, int32 Outcome, int32 GameScore)
{
	if (!L || Outcome == 0 || !AOutlawsLevelActor::IsCareerMission(Mission)) return;
	TArray<FString> Ids;
	if (GameScore >= 9300) Ids.Add(TEXT("mt_deputy"));       // (the ranks' scores: OutlawsHom.cpp HomRanks)
	if (GameScore >= 12400) Ids.Add(TEXT("mt_sheriff"));
	if (GameScore >= 15500) Ids.Add(TEXT("mt_marshal"));
	const FString Key = Outcome == 1 ? TEXT("mt_captured") : TEXT("mt_killed");
	if (AddProgress(L, Key, FPaths::GetBaseFilename(Mission)) >= MtMissions) Ids.Add(Key);
	Earn(L, Ids, *FString::Printf(TEXT("Marshal Training: %s %s"), *Mission, Outcome == 1 ? TEXT("captured") : TEXT("killed")));
}

void OutlawsAchievements::NoteSecret(AOutlawsLevelActor* L, const FString& Level, int32 Sector, int32 Total)
{
	if (!L || L->IsTutorial()) return;
	TArray<FString> Ids = { TEXT("secret") };
	if (ParSeconds(Level) >= 0)
	{
		// (each story level's secrets by sector, kept across careers as the other progress is; its count kept with it)
		NoteSecretTotal(L, Level, Total);
		AddProgress(L, TEXT("secrets"), FString::Printf(TEXT("%s:%d"), *Level.ToLower(), Sector));
		TArray<FString> Levels, Labels;
		BoxLevels(Levels, Labels);
		const int32 Done = LevelsAllSecrets(OutlawsUnlocks::ProfileRead(L));
		UE_LOG(LogOutlaws, Log, TEXT("OLACH secret %s:%d (%d in the level); levels with every secret found %d of %d"), *Level.ToLower(), Sector, Total, Done, Levels.Num());
		if (Done >= Levels.Num()) Ids.Add(TEXT("secrets_all"));
	}
	Earn(L, Ids, *FString::Printf(TEXT("a secret in %s"), *Level.ToLower()));
}

void OutlawsAchievements::NoteSecretTotal(AOutlawsLevelActor* L, const FString& Level, int32 Total)
{
	if (!L || Total < 0 || ParSeconds(Level) < 0) return;
	const FString Lv = Level.ToLower();
	const int32* Had = SecretTotals(OutlawsUnlocks::ProfileRead(L)).Find(Lv);
	if (Had && *Had == Total) return;           // (written only when it is new or changed)
	OutlawsUnlocks::ProfileChange(L, [&](const TSharedPtr<FJsonObject>& P)
	{
		if (!P.IsValid()) return;
		const TSharedPtr<FJsonObject>* Pr = nullptr;
		if (!P->TryGetObjectField(TEXT("progress"), Pr) || !Pr || !Pr->IsValid())
		{
			P->SetObjectField(TEXT("progress"), MakeShared<FJsonObject>());
			P->TryGetObjectField(TEXT("progress"), Pr);
		}
		const TSharedPtr<FJsonObject>* T = nullptr;
		if (!(*Pr)->TryGetObjectField(TEXT("secret_totals"), T) || !T || !T->IsValid())
		{
			(*Pr)->SetObjectField(TEXT("secret_totals"), MakeShared<FJsonObject>());
			(*Pr)->TryGetObjectField(TEXT("secret_totals"), T);
		}
		(*T)->SetNumberField(Lv, Total);
	});
	// (a level with no secrets at all is done once its count is known: the last one known may finish the set)
	TArray<FString> Levels, Labels;
	BoxLevels(Levels, Labels);
	if (LevelsAllSecrets(OutlawsUnlocks::ProfileRead(L)) >= Levels.Num()) Earn(L, { TEXT("secrets_all") }, *FString::Printf(TEXT("%s's secrets counted"), *Lv));
}

void OutlawsAchievements::NoteGatlingFired(AOutlawsLevelActor* L)
{
	if (L && !L->IsFrontEnd() && !IsEarned(L, TEXT("gatling"))) Earn(L, { TEXT("gatling") }, TEXT("the gatling fired"));
}

void OutlawsAchievements::NoteBlastKills(AOutlawsLevelActor* L, int32 Kills)
{
	if (!L || L->IsFrontEnd() || Kills <= 0) return;
	UE_LOG(LogOutlaws, Log, TEXT("OLACH one blast killed %d"), Kills);
	if (Kills >= 3) Earn(L, { TEXT("dynamite3") }, *FString::Printf(TEXT("%d with one stick"), Kills));
}

int32 OutlawsAchievements::ParSeconds(const FString& Level)
{
	// (user 20 Oct: one par per story level, a good run with some room, not a perfect one; from the user's own Quest finishes
	// of 13 to 20 Oct, the Randomizer's and the multiplier's included, and the levels' size where there were none)
	static const TCHAR* const Lv[] = { TEXT("hideout"), TEXT("townex"), TEXT("train"), TEXT("canyon"), TEXT("mill"), TEXT("simms"), TEXT("miner"), TEXT("cliff"), TEXT("ranch") };
	static const int32 Par[] = { 9 * 60, 10 * 60, 4 * 60, 12 * 60, 12 * 60, 8 * 60, 10 * 60, 10 * 60, 12 * 60 };
	for (int32 i = 0; i < UE_ARRAY_COUNT(Lv); ++i) if (Level.Equals(Lv[i], ESearchCase::IgnoreCase)) return Par[i];
	return -1;
}

void OutlawsAchievements::NoteKeptTime(AOutlawsLevelActor* L, const FString& LevelKey, int32 Seconds)
{
	const int32 Par = ParSeconds(LevelKey);
	if (L && Seconds >= 0 && Par >= 0 && Seconds < Par) Earn(L, { TEXT("par") }, *FString::Printf(TEXT("%s in %s, par %s"), *LevelKey, *Clock(Seconds), *Clock(Par)));
}

void OutlawsAchievements::NoteMatchEnd(AOutlawsLevelActor* L, bool bWon, bool bOnlyBots)
{
	UE_LOG(LogOutlaws, Log, TEXT("OLACH match ended here: %s, %s"), bWon ? TEXT("won") : TEXT("not won"), bOnlyBots ? TEXT("against bots only") : TEXT("against players"));
	if (bWon) Earn(L, { bOnlyBots ? TEXT("bots_win") : TEXT("mp_win") }, TEXT("a match won"));
}

void OutlawsAchievements::NoteTutorialDone(AOutlawsLevelActor* L)
{
	Earn(L, { TEXT("tutorial") }, TEXT("the tutorial"));
}

void OutlawsAchievements::BoxLevels(TArray<FString>& OutLevels, TArray<FString>& OutLabels)
{
	static const TCHAR* const Story[][2] = { { TEXT("hideout"), TEXT("The Hideout") }, { TEXT("townex"), TEXT("The Town") }, { TEXT("train"), TEXT("The Train") },
		{ TEXT("canyon"), TEXT("The Canyon") }, { TEXT("mill"), TEXT("The Sawmill") }, { TEXT("simms"), TEXT("Simm's Fort") }, { TEXT("miner"), TEXT("The Mine") },
		{ TEXT("cliff"), TEXT("The Cliff Dwellings") }, { TEXT("ranch"), TEXT("Bob's Ranch") } };
	OutLevels.Reset();
	OutLabels.Reset();
	for (const auto& S : Story) { OutLevels.Add(S[0]); OutLabels.Add(S[1]); }
}

bool OutlawsAchievements::BoxFound(const AOutlawsLevelActor* L, const FString& Level)
{
	return ProgressList(OutlawsUnlocks::ProfileRead(L), TEXT("boxes")).Contains(Level.ToLower());
}

void OutlawsAchievements::NoteBoxFound(AOutlawsLevelActor* L, const FString& Level)
{
	TArray<FString> Levels, Labels;
	BoxLevels(Levels, Labels);
	if (!Levels.Contains(Level.ToLower())) return;
	AddProgress(L, TEXT("boxes"), Level);
	int32 N = 0;
	for (const FString& Lv : Levels) N += BoxFound(L, Lv) ? 1 : 0;
	TArray<FString> Ids = { TEXT("boxes_first") };
	if (N * 2 >= Levels.Num()) Ids.Add(TEXT("boxes_half"));
	if (N >= Levels.Num()) Ids.Add(TEXT("boxes_all"));
	Earn(L, Ids, *FString::Printf(TEXT("box %d of %d (%s)"), N, Levels.Num(), *Level));
}

#if WITH_OUTLAWS_TESTS
void OutlawsAchievements::DebugForgetBoxes(AOutlawsLevelActor* L)
{
	OutlawsUnlocks::ProfileChange(L, [](const TSharedPtr<FJsonObject>& P)
	{
		if (!P.IsValid()) return;
		const TSharedPtr<FJsonObject>* Pr = nullptr;
		if (P->TryGetObjectField(TEXT("progress"), Pr) && Pr && Pr->IsValid()) (*Pr)->RemoveField(TEXT("boxes"));
		const TSharedPtr<FJsonObject>* E = nullptr;
		if (P->TryGetObjectField(TEXT("earned"), E) && E && E->IsValid())
		{
			for (const TCHAR* Id : { TEXT("boxes_first"), TEXT("boxes_half"), TEXT("boxes_all") }) (*E)->RemoveField(Id);
		}
	});
}
#endif

void OutlawsAchievements::Tick(AOutlawsPlayer* P, AOutlawsLevelActor* L, float Dt)
{
	if (GToastQueue.Num() == 0 || !P || !L) return;
	// (several earned at once, a boss and First outlaw down: each its own moment, not one over the other)
	static double NextToastAt = 0.0;
	const double Now = FPlatformTime::Seconds();
	if (Now < NextToastAt) return;
	// a menu up: its toast line (the main menu once it opens after the ending); the game going on: the level's notice line
	const FString Text = GToastQueue[0];
	if (P->IsMenuOpen()) P->CoopToast(Text);
	else if (!L->IsFrontEnd() && !L->IsMoviePlaying()) L->Notify(Text, 4.f);
	else return;
	GToastQueue.RemoveAt(0);
	NextToastAt = Now + 2.5;
	GToastsShown.Add(Text);
	UE_LOG(LogOutlaws, Log, TEXT("OLACH toast: %s (%s)"), *Text, P->IsMenuOpen() ? TEXT("the menu") : TEXT("in play"));
}

const TArray<FString>& OutlawsAchievements::ToastsShown() { return GToastsShown; }
