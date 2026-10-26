// -OLShots -OLExtrasTest [-OLExtrasTestVR=right|left|one|oneleft]: REQ-EXTRAS, one of the ShotTest tests (OutlawsShotTests.h).
//
// From the title menu: an achievements.json in the old shape written in place (REQ-UNLOCKS' story earned, REQ-LEVELTIMER's best
// time) and read back (earned, its date, both modes open, the best time as the par's progress); Extras and each page
// walked and photographed (ol_extras_*); an achievement earned through its hook (the tutorial's) and seen in the file beside the
// old entries, its toast shown, its capsule earned; Credits rolled and skipped, back on the Extras page.  The Saved folder is
// the runner's to put back (out\extras\saved.ps1).

#include "ShotTestCommon.h"
#include "OutlawsAchievements.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLExtrasTest) { return OLFlags::Param(TEXT("OLExtrasTest")); }

OL_SHOT_TEST(OLExtrasTest, 855)
{
	auto Shot = [](const FString& Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false); };
	static int32 Passes = 0, Fails = 0;
	static float NextAt = 0.f;
	auto Check = [](bool bOk, const FString& What)
	{
		bOk ? ++Passes : ++Fails;
		UE_LOG(LogOutlaws, Display, TEXT("OLEXTRAS check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
	};
	auto Rows = [this]()
	{
		FString Out;
		if (MenuPage) for (const OutlawsMenu::FItem& I : MenuPage->Items) Out += (Out.IsEmpty() ? TEXT("") : TEXT(" | ")) + I.Label + (I.Classes.Contains(TEXT("disabled")) ? TEXT(" [greyed]") : TEXT(""));
		return Out;
	};
	auto Page = [this]() { return bMenuOpen && MenuPage ? MenuPage->File : FString(); };
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLEXTRAS RESULT %d passed, %d failed"), Passes, Fails);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	};
	FString Lay;
	const bool bTestVR = OLFlags::Value(TEXT("OLExtrasTestVR"), Lay);
	const FString Pre = bTestVR ? TEXT("ol_extras_vr_") : TEXT("ol_extras_");
	if (ShotTime < NextAt) return;
	NextAt = ShotTime + 1.f;
	const FString Store = OutlawsUnlocks::StorePath();
	switch (ShotStep)
	{
	case 0:
	{
		if (Page() != TEXT("#front") || ShotTime < 4.f)
		{
			if (ShotTime > 120.f) { Check(false, TEXT("the title menu came up within 120 s")); Finish(); }
			NextAt = 0.f;
			return;
		}
		if (bTestVR)
		{
			bVR = true;
			DominantHand = (Lay == TEXT("left") || Lay == TEXT("oneleft")) ? EOutlawsHand::Left : EOutlawsHand::Right;
			OneController = Lay == TEXT("one") ? 2 : Lay == TEXT("oneleft") ? 1 : 0;
			ShowMenuPage(TEXT("#front"), false);
		}
		const FString R = Rows();
		Check(R.Contains(TEXT("New Game | Extras | Options | Exit")), FString::Printf(TEXT("the title's small row has Extras before Options (%s)"), *R));
		// the old shape, as REQ-UNLOCKS and REQ-LEVELTIMER wrote it: nothing of REQ-EXTRAS in it
		const FString Prof = OutlawsUnlocks::ProfileOf(L);
		const FString Old = FString::Printf(TEXT("{ \"version\": 1, \"unlockAllModes\": false, \"profiles\": { \"%s\": { \"earned\": { \"story\": { \"when\": \"2026-10-13 16:02:11\", \"difficulty\": \"Good\", \"coop\": false } }, \"unseen\": [], \"times\": { \"hideout\": { \"Good\": { \"seconds\": 412, \"when\": \"2026-10-13 17:02:11\", \"coop\": false } } } } } }"), *Prof);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Store), true);
		Check(FFileHelper::SaveStringToFile(Old, *Store), FString::Printf(TEXT("an old-shape achievements.json written for profile %s"), *Prof));
		NextAt = ShotTime + 2.f;              // (the store looks at the file again once a second)
		break;
	}
	case 1:
	{
		Check(OutlawsAchievements::IsEarned(L, TEXT("story")), TEXT("old file: story earned"));
		const FString When = OutlawsAchievements::EarnedLine(L, TEXT("story"));
		Check(When == TEXT("Earned 13 Oct 2026"), FString::Printf(TEXT("old file: its date reads (%s)"), *When));
		Check(OutlawsUnlocks::IsUnlocked(L, OutlawsUnlocks::EMode::Randomizer) && OutlawsUnlocks::IsUnlocked(L, OutlawsUnlocks::EMode::BadGuys), TEXT("old file: both modes open (UNLOCKFREE: from the start)"));
		const FString Par = OutlawsAchievements::Progress(L, TEXT("par"));
		Check(Par.Contains(TEXT("6:52")) && Par.Contains(TEXT("The Hideout")), FString::Printf(TEXT("old file: the best time is the par's progress (%s)"), *Par));
		Check(!OutlawsUnlocks::StoreDamaged(), TEXT("old file: not taken as damaged"));
		Check(TestPressLink(TEXT("#extras")) && Page() == TEXT("#extras"), FString::Printf(TEXT("Extras opens its page (%s)"), *Page()));
		Check(Rows() == TEXT("Achievements | Unlockables | Collectibles | Credits"), FString::Printf(TEXT("Extras reads Achievements, Unlockables, Collectibles, Credits (%s)"), *Rows()));
		break;
	}
	case 2: Shot(Pre + TEXT("page")); break;
	case 3:
	{
		Check(TestPressLink(TEXT("#extras_achiev")) && IsAchievPage(), FString::Printf(TEXT("Achievements opens (%s)"), *Page()));
		const int32 N = MenuPage ? MenuPage->Items.Num() : 0;
		Check(N == OutlawsAchievements::All().Num(), FString::Printf(TEXT("a capsule for each of the %d achievements (%d)"), OutlawsAchievements::All().Num(), N));
		const int32 Bob = MenuPage ? MenuPage->Items.IndexOfByPredicate([](const OutlawsMenu::FItem& I) { return I.Name == TEXT("ach:boss_bob"); }) : INDEX_NONE;
		Check(Bob != INDEX_NONE && MenuPage->Items[Bob].Label == TEXT("???"), TEXT("a hidden one not yet earned reads ???"));
		Check(MenuPage && MenuPage->Items.IsValidIndex(0) && MenuPage->Items[0].Classes.Contains(TEXT("earned")), TEXT("the story's capsule is earned"));
		UE_LOG(LogOutlaws, Display, TEXT("OLEXTRAS achievements: %s"), *Rows());
		RedrawMenu();
		break;
	}
	case 4: Shot(Pre + TEXT("achievements")); MenuHover[1] = 2; RedrawMenu(); break;
	case 5:
	{
		Shot(Pre + TEXT("achievements_focus"));
		Check(AchievFocus == 2, FString::Printf(TEXT("pointing at the third capsule describes it (focus %d)"), AchievFocus));
		MenuHover[1] = -1;
		// the tutorial's hook, as its last card does it
		OutlawsAchievements::NoteTutorialDone(L);
		Check(OutlawsAchievements::IsEarned(L, TEXT("tutorial")), TEXT("the tutorial's achievement earned"));
		FString Text;
		FFileHelper::LoadFileToString(Text, *Store);
		Check(Text.Contains(TEXT("\"tutorial\"")) && Text.Contains(TEXT("\"story\"")) && Text.Contains(TEXT("2026-10-13 16:02:11")) && Text.Contains(TEXT("\"times\"")) && Text.Contains(TEXT("412")),
			TEXT("the saved file has the new entry beside the old ones (story, its date, the best time)"));
		break;
	}
	case 6:
	{
		const bool bToast = OutlawsAchievements::ToastsShown().ContainsByPredicate([](const FString& S) { return S.StartsWith(TEXT("Achievement: ")) && S.Contains(TEXT("Greenhorn")); });
		Check(bToast && MenuToast && MenuToast->Text.ToString().Contains(TEXT("Greenhorn")), FString::Printf(TEXT("its toast shown (%s)"), MenuToast ? *MenuToast->Text.ToString() : TEXT("none")));
		ShowMenuPage(TEXT("#extras_achiev"), false);
		const int32 N = MenuPage ? MenuPage->Items.Num() : 0;
		const int32 Tut = MenuPage ? MenuPage->Items.IndexOfByPredicate([](const OutlawsMenu::FItem& I) { return I.Name == TEXT("ach:tutorial"); }) : INDEX_NONE;
		Check(Tut != INDEX_NONE && MenuPage->Items[Tut].Classes.Contains(TEXT("earned")), TEXT("its capsule earned on the page"));
		MenuScroll = FMath::Max(0, N - AchievRowsShown);
		MenuHover[1] = Tut - MenuScroll;
		RedrawMenu();
		break;
	}
	case 7: Shot(Pre + TEXT("achievements_earned")); break;
	case 8:
	{
		// (user 20 Oct) the new ones through their hooks: each named outlaw by his level and FUNC, every secret of the story,
		// the gatling, three with one stick, the par of a level
		MenuHover[1] = -1;
		static const TCHAR* const Bosses[][3] = { { TEXT("hideout"), TEXT("slim_new"), TEXT("boss_slim") }, { TEXT("townex"), TEXT("sanchez_new"), TEXT("boss_sanchez") },
			{ TEXT("train"), TEXT("bloodeye_new"), TEXT("boss_bloodeye") }, { TEXT("canyon"), TEXT("farmer_new"), TEXT("boss_dick") }, { TEXT("mill"), TEXT("drdeath_new"), TEXT("boss_henry") },
			{ TEXT("simms"), TEXT("simms_new"), TEXT("boss_simms") }, { TEXT("miner"), TEXT("mary_new"), TEXT("boss_mary") }, { TEXT("miner"), TEXT("drdeath_new"), TEXT("boss_drdeath") },
			{ TEXT("cliff"), TEXT("twofeathers_new"), TEXT("boss_twofeathers") }, { TEXT("ranch"), TEXT("buckshot_new"), TEXT("boss_bill") }, { TEXT("ranch"), TEXT("bob_new"), TEXT("boss_bob") } };
		OutlawsAchievements::NoteBossDown(L, TEXT("shfort"), TEXT("sanchez_new"));
		Check(!OutlawsAchievements::IsEarned(L, TEXT("boss_first")) && !OutlawsAchievements::IsEarned(L, TEXT("boss_sanchez")), TEXT("an outlaw down outside the story earns nothing"));
		for (const auto& B : Bosses)
		{
			OutlawsAchievements::NoteBossDown(L, B[0], B[1]);
			Check(OutlawsAchievements::IsEarned(L, B[2]), FString::Printf(TEXT("%s down in %s: %s earned"), B[1], B[0], B[2]));
			if (FCString::Strcmp(B[2], TEXT("boss_henry")) == 0) Check(!OutlawsAchievements::IsEarned(L, TEXT("boss_drdeath")), TEXT("the mill's Henry (a DrDeath_New) is not Dr. Death"));
		}
		Check(OutlawsAchievements::IsEarned(L, TEXT("boss_first")), TEXT("boss_first earned with the first"));
		// every secret: two in each story level but the ranch, which has none; the last one found earns it
		TArray<FString> Levels, Labels;
		OutlawsAchievements::BoxLevels(Levels, Labels);
		for (int32 k = 0; k < Levels.Num(); ++k)
		{
			if (Levels[k] == TEXT("ranch")) continue;
			OutlawsAchievements::NoteSecret(L, Levels[k], 10, 2);
			if (k < Levels.Num() - 2) OutlawsAchievements::NoteSecret(L, Levels[k], 11, 2);
		}
		OutlawsAchievements::NoteSecret(L, TEXT("hideout"), 10, 2);      // (the same sector again: still one)
		Check(OutlawsAchievements::IsEarned(L, TEXT("secret")) && !OutlawsAchievements::IsEarned(L, TEXT("secrets_all")), FString::Printf(TEXT("secrets: one found, not every one (%s)"), *OutlawsAchievements::Progress(L, TEXT("secrets_all"))));
		OutlawsAchievements::NoteSecret(L, Levels[Levels.Num() - 2], 11, 2);
		Check(!OutlawsAchievements::IsEarned(L, TEXT("secrets_all")), FString::Printf(TEXT("secrets: every one but the ranch's count unknown (%s)"), *OutlawsAchievements::Progress(L, TEXT("secrets_all"))));
		OutlawsAchievements::NoteSecretTotal(L, TEXT("ranch"), 0);
		Check(OutlawsAchievements::IsEarned(L, TEXT("secrets_all")), FString::Printf(TEXT("secrets: the ranch counted (none): every secret found (%s)"), *OutlawsAchievements::Progress(L, TEXT("secrets_all"))));
		OutlawsAchievements::NoteGatlingFired(L);
		Check(!OutlawsAchievements::IsEarned(L, TEXT("gatling")), TEXT("the gatling on the title screen earns nothing"));
		OutlawsAchievements::NoteBlastKills(L, 3);
		Check(!OutlawsAchievements::IsEarned(L, TEXT("dynamite3")), TEXT("a blast on the title screen earns nothing"));
		const bool bFront = L->bFrontEnd;
		L->bFrontEnd = false;              // (as in a level: the two hooks look at that alone)
		OutlawsAchievements::NoteGatlingFired(L);
		Check(OutlawsAchievements::IsEarned(L, TEXT("gatling")), TEXT("the gatling fired: earned"));
		OutlawsAchievements::NoteBlastKills(L, 2);
		Check(!OutlawsAchievements::IsEarned(L, TEXT("dynamite3")), TEXT("two with one stick: not yet"));
		OutlawsAchievements::NoteBlastKills(L, 3);
		Check(OutlawsAchievements::IsEarned(L, TEXT("dynamite3")), TEXT("three with one stick: earned"));
		L->bFrontEnd = bFront;
		const int32 TrainPar = OutlawsAchievements::ParSeconds(TEXT("train"));
		Check(TrainPar == 240 && OutlawsAchievements::ParSeconds(TEXT("ranch")) == 720 && OutlawsAchievements::ParSeconds(TEXT("shfort")) < 0, FString::Printf(TEXT("par times: train %d s, ranch, none outside the story"), TrainPar));
		OutlawsAchievements::NoteKeptTime(L, TEXT("train"), TrainPar);
		OutlawsAchievements::NoteKeptTime(L, TEXT("shfort"), 10);
		Check(!OutlawsAchievements::IsEarned(L, TEXT("par")), TEXT("par: on the par itself, or outside the story, not earned"));
		OutlawsAchievements::NoteKeptTime(L, TEXT("train"), TrainPar - 1);
		Check(OutlawsAchievements::IsEarned(L, TEXT("par")), TEXT("par: a second under the train's par, earned"));
		Check(!OutlawsAchievements::IsEarned(L, TEXT("boxes_all")), TEXT("boxes_all not earned"));
		// the names: the remaster's where it has them (its icons beside), ours for the gatling's line
		AOutlawsPlayer* Me = this;
		for (const TCHAR* Id : { TEXT("secrets_all"), TEXT("gatling"), TEXT("dynamite3"), TEXT("boss_slim") })
		{
			const OutlawsAchievements::FDef* D = OutlawsAchievements::Find(Id);
			UE_LOG(LogOutlaws, Display, TEXT("OLEXTRAS %s: \"%s\", %s (icon %d)"), Id, D ? *OutlawsAchievements::TitleOf(*D, Me) : TEXT("?"), D ? *OutlawsAchievements::DescOf(*D, Me) : TEXT("?"), D ? D->Icon : -1);
		}
		const OutlawsAchievements::FDef* G = OutlawsAchievements::Find(TEXT("gatling"));
		Check(G && OutlawsAchievements::DescOf(*G, Me) == TEXT("Fire the gatling gun"), TEXT("the gatling's line is ours"));
		FString Text;
		FFileHelper::LoadFileToString(Text, *Store);
		Check(Text.Contains(TEXT("\"secret_totals\"")) && Text.Contains(TEXT("\"hideout:10\"")) && Text.Contains(TEXT("\"tutorial\"")) && Text.Contains(TEXT("2026-10-13 16:02:11")),
			TEXT("the file keeps the secrets' progress beside the old entries"));
		ShowMenuPage(TEXT("#extras_achiev"), false);
		const int32 Sl = MenuPage ? MenuPage->Items.IndexOfByPredicate([](const OutlawsMenu::FItem& I) { return I.Name == TEXT("ach:boss_slim"); }) : INDEX_NONE;
		MenuScroll = FMath::Max(0, Sl - 1);
		MenuHover[1] = Sl - MenuScroll;
		RedrawMenu();
		break;
	}
	case 9: Shot(Pre + TEXT("achievements_bosses")); break;
	case 10:
		MenuHover[1] = -1;
		MenuBack();
		Check(Page() == TEXT("#extras"), FString::Printf(TEXT("Back to Extras (%s)"), *Page()));
		Check(TestPressLink(TEXT("#extras_unlock")) && Page() == TEXT("#extras_unlock"), FString::Printf(TEXT("Unlockables opens (%s)"), *Page()));
		Check(Rows() == TEXT("Randomizer: available from the start | Bad guys multiplier: available from the start | Hidden box rewards: coming soon"), FString::Printf(TEXT("Unlockables: both open, available from the start (%s)"), *Rows()));
		break;
	case 11: Shot(Pre + TEXT("unlockables")); break;
	case 12:
		MenuBack();
		Check(TestPressLink(TEXT("#extras_shelf")) && Page() == TEXT("#extras_shelf"), FString::Printf(TEXT("Collectibles opens (%s)"), *Page()));
		Check(MenuPage && MenuPage->Items.Num() == 9 && !MenuPage->Items.ContainsByPredicate([](const OutlawsMenu::FItem& I) { return !I.Classes.Contains(TEXT("disabled")); }),
			FString::Printf(TEXT("the shelf: nine outlines, none found (%s)"), *Rows()));
		break;
	case 13: Shot(Pre + TEXT("collectibles")); break;
	case 14:
	{
		MenuBack();
		const int32 Cr = MenuPage ? MenuPage->Items.IndexOfByPredicate([](const OutlawsMenu::FItem& I) { return I.Name == TEXT("extras_credits"); }) : INDEX_NONE;
		if (Cr != INDEX_NONE) MenuActivate(1, Cr);
		NextAt = ShotTime + 3.f;
		break;
	}
	case 15:
		Check(L && L->IsMoviePlaying() && !bMenuOpen, TEXT("Credits rolls the end credits"));
		Shot(Pre + TEXT("credits"));
		break;
	case 16: if (L) L->SkipMovie(); NextAt = ShotTime + 2.f; break;
	case 17:
		Check(Page() == TEXT("#extras"), FString::Printf(TEXT("skipped: back on the Extras page (%s)"), *Page()));
		Finish();
		return;
	default: return;
	}
	++ShotStep;
}

#endif // WITH_OUTLAWS_TESTS
