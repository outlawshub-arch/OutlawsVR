#include "OutlawsDevChecks.h"
#include "OutlawsFlags.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsBuildInfo.h"
#include "OutlawsLevelActor.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/DateTime.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "UnrealClient.h"
#include "GameFramework/Controller.h"

namespace
{
	struct FAnswer
	{
		int32 State = 0;
		FString What;
		FString At;
	};

	TArray<OutlawsDevChecks::FCheck> GChecks;
	TMap<FString, FAnswer> GAnswers;
	FString GSource = TEXT("none");
	int32 GPage = 0;
	constexpr int32 RowsOnPage = 9;          // the menu's rows (OutlawsPlayerMenu.cpp MenuRowsShown)
	constexpr int32 ChecksPerPage = 8;       // with a page row under them once the list is longer than a page

	const TCHAR* StateKey(int32 S) { return S == 1 ? TEXT("fixed") : S == 2 ? TEXT("broken") : TEXT("unchecked"); }
	int32 StateFromKey(const FString& K) { return K == TEXT("fixed") ? 1 : K == TEXT("broken") ? 2 : 0; }

	bool ReadList(const FString& Path, TArray<OutlawsDevChecks::FCheck>& Out)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path)) return false;
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
		{
			UE_LOG(LogOutlaws, Warning, TEXT("OLRT devchecks: %s is not valid JSON"), *Path);
			return false;
		}
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Root->TryGetArrayField(TEXT("checks"), Arr)) return false;
		Out.Reset();
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject>* O = nullptr;
			if (!V.IsValid() || !V->TryGetObject(O)) continue;
			OutlawsDevChecks::FCheck C;
			(*O)->TryGetStringField(TEXT("id"), C.Id);
			(*O)->TryGetStringField(TEXT("what"), C.What);
			(*O)->TryGetStringField(TEXT("detail"), C.Detail);
			if (!C.Id.IsEmpty()) Out.Add(C);
		}
		return Out.Num() > 0;
	}

	void ReadAnswers()
	{
		GAnswers.Reset();
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *OutlawsDevChecks::AnswersPath())) return;
		TSharedPtr<FJsonObject> Root;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid()) return;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Root->TryGetArrayField(TEXT("answers"), Arr)) return;
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			const TSharedPtr<FJsonObject>* O = nullptr;
			if (!V.IsValid() || !V->TryGetObject(O)) continue;
			FString Id, State;
			FAnswer A;
			(*O)->TryGetStringField(TEXT("id"), Id);
			(*O)->TryGetStringField(TEXT("state"), State);
			(*O)->TryGetStringField(TEXT("what"), A.What);
			(*O)->TryGetStringField(TEXT("at"), A.At);
			A.State = StateFromKey(State);
			if (!Id.IsEmpty()) GAnswers.Add(Id, A);
		}
	}

	void WriteAnswers()
	{
		// the list's order first, then answers to checks no longer listed (kept, never dropped)
		TArray<TSharedPtr<FJsonValue>> Arr;
		TSet<FString> Done;
		auto Add = [&Arr](const FString& Id, const FAnswer& A)
		{
			TSharedPtr<FJsonObject> O = MakeShared<FJsonObject>();
			O->SetStringField(TEXT("id"), Id);
			O->SetStringField(TEXT("state"), StateKey(A.State));
			O->SetStringField(TEXT("what"), A.What);
			O->SetStringField(TEXT("at"), A.At);
			Arr.Add(MakeShared<FJsonValueObject>(O));
		};
		for (const OutlawsDevChecks::FCheck& C : GChecks)
		{
			if (const FAnswer* A = GAnswers.Find(C.Id)) { Add(C.Id, *A); Done.Add(C.Id); }
		}
		for (const TPair<FString, FAnswer>& P : GAnswers)
		{
			if (!Done.Contains(P.Key)) Add(P.Key, P.Value);
		}
		TSharedPtr<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("build"), OutlawsBuildInfo::Line());
		Root->SetStringField(TEXT("list"), GSource);
		Root->SetStringField(TEXT("updated"), FDateTime::UtcNow().ToIso8601());
		Root->SetArrayField(TEXT("answers"), Arr);
		FString Text;
		const TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Text);
		FJsonSerializer::Serialize(Root.ToSharedRef(), W);
		IFileManager::Get().MakeDirectory(*OutlawsDevChecks::Dir(), true);
		const bool bOk = FFileHelper::SaveStringToFile(Text, *OutlawsDevChecks::AnswersPath(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		UE_LOG(LogOutlaws, Log, TEXT("OLRT devchecks: answers %s (%d) to %s"), bOk ? TEXT("saved") : TEXT("NOT saved"), Arr.Num(), *OutlawsDevChecks::AnswersPath());
	}
}

bool OutlawsDevChecks::Enabled()
{
#if UE_BUILD_SHIPPING
	return false;
#else
	if (OLFlags::Param(TEXT("OLNoDevChecks"))) return false;
	if (OLFlags::Param(TEXT("OLDevChecks"))) return true;
	return IFileManager::Get().FileExists(*(Dir() / TEXT("devchecks_on")));
#endif
}

FString OutlawsDevChecks::Dir() { return FPaths::ProjectSavedDir() / TEXT("DevChecks"); }
FString OutlawsDevChecks::ListPath() { return Dir() / TEXT("checks.json"); }
FString OutlawsDevChecks::BundledPath() { return FPaths::ProjectContentDir() / TEXT("DevChecks") / TEXT("checks.json"); }
FString OutlawsDevChecks::AnswersPath() { return Dir() / TEXT("answers.json"); }

FString OutlawsDevChecks::Reload()
{
	GSource = ReadList(ListPath(), GChecks) ? TEXT("pushed") : ReadList(BundledPath(), GChecks) ? TEXT("bundled") : TEXT("none");
	if (GSource == TEXT("none")) GChecks.Reset();
	ReadAnswers();
	GPage = FMath::Clamp(GPage, 0, NumPages() - 1);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT devchecks: %d checks from the %s list, %d answers"), GChecks.Num(), *GSource, GAnswers.Num());
	return GSource;
}

const TArray<OutlawsDevChecks::FCheck>& OutlawsDevChecks::Checks() { return GChecks; }
const FString& OutlawsDevChecks::Source() { return GSource; }

int32 OutlawsDevChecks::State(const FString& Id)
{
	const FAnswer* A = GAnswers.Find(Id);
	return A ? A->State : 0;
}

int32 OutlawsDevChecks::Cycle(const FString& Id)
{
	FAnswer& A = GAnswers.FindOrAdd(Id);
	A.State = (A.State + 1) % 3;
	A.At = FDateTime::UtcNow().ToIso8601();
	for (const FCheck& C : GChecks) if (C.Id == Id) A.What = C.What;
	WriteAnswers();
	return A.State;
}

const TCHAR* OutlawsDevChecks::StateText(int32 S)
{
	return S == 1 ? TEXT("Fixed") : S == 2 ? TEXT("Still broken") : TEXT("Not checked");
}

int32 OutlawsDevChecks::NumPages()
{
	return GChecks.Num() <= RowsOnPage ? 1 : (GChecks.Num() + ChecksPerPage - 1) / ChecksPerPage;
}

int32 OutlawsDevChecks::CurrentPage() { return GPage; }

void OutlawsDevChecks::NextPage() { GPage = (GPage + 1) % FMath::Max(1, NumPages()); }

void OutlawsDevChecks::BuildPage(OutlawsMenu::FPage& Page)
{
	using namespace OutlawsMenu;
	const int32 Pages = NumPages();
	GPage = FMath::Clamp(GPage, 0, Pages - 1);
	Page.Title = Pages > 1 ? FString::Printf(TEXT("DEV CHECKS %d/%d"), GPage + 1, Pages) : FString(TEXT("DEV CHECKS"));
	const int32 First = Pages > 1 ? GPage * ChecksPerPage : 0;
	const int32 Last = Pages > 1 ? FMath::Min(First + ChecksPerPage, GChecks.Num()) : GChecks.Num();
	for (int32 i = First; i < Last; ++i)
	{
		FItem I;
		I.Kind = EItemKind::Select;
		I.Label = GChecks[i].Id + TEXT("  ") + GChecks[i].What;
		I.Name = TEXT("devcheck:") + GChecks[i].Id;
		I.Options = { TEXT("Not checked"), TEXT("Fixed"), TEXT("Still broken") };
		Page.Items.Add(I);
	}
	if (Pages > 1)
	{
		FItem I;
		I.Kind = EItemKind::Action;
		I.Label = FString::Printf(TEXT("Page %d of %d: next page"), GPage + 1, Pages);
		I.Name = TEXT("devchecks_next");
		Page.Items.Add(I);
	}
	if (GChecks.Num() == 0)
	{
		FItem I;
		I.Kind = EItemKind::Action;
		I.Label = TEXT("No checks found");
		I.Name = TEXT("devchecks_none");
		Page.Items.Add(I);
	}
}

// ---- -OLDevChecksTest[=nodev] -----------------------------------------------------------------------------------------
// full: the turn on the spot unlocks Cheats, Tools > Dev checks opens, a row cycles, the answers file is read back, the pushed
// list wins and the bundled one stands in when it is gone, paging, Back. nodev: after the unlock the Tools page has no Dev
// checks row (run without -OLDevChecks and without Saved/DevChecks/devchecks_on). -OLDevChecksShots=<dir> for the shots.

#if WITH_OUTLAWS_TESTS
void AOutlawsPlayer::DevChecksTestTick(float Dt)
{
	static float T = 0.f, NextAt = 0.f;
	static int32 Step = 0, Turns = 0;
	static FString Mode;
	static int32 Fails = 0;
	if (Step >= 100) return;
	AOutlawsLevelActor* L = Level.Get();
	if (!L || L->IsFrontEnd() || L->IsRebuildPending() || bFrontMenu) { T = 0.f; return; }
	if (Mode.IsEmpty() && !OLFlags::Value(TEXT("OLDevChecksTest"), Mode)) Mode = TEXT("full");
	T += Dt;
	if (Step == 0 && T < 12.f) return;
	if (T < NextAt) return;
	auto Check = [](bool bOk, const FString& What)
	{
		if (!bOk) ++Fails;
		UE_LOG(LogOutlaws, Log, TEXT("OLDEVCHECK check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
	};
	auto Shot = [](const FString& Name)
	{
		FString ShotDir = FPaths::ProjectSavedDir() / TEXT("OLShots");
		OLFlags::Value(TEXT("OLDevChecksShots"), ShotDir);
		const FString Path = ShotDir / Name + TEXT(".png");
		FScreenshotRequest::RequestScreenshot(Path, false, false);
		UE_LOG(LogOutlaws, Log, TEXT("OLDEVCHECK screenshot %s"), *Path);
	};
	auto Rows = [this](const TCHAR* What)
	{
		FString R;
		const int32 N = MenuPage ? MenuPage->Items.Num() : 0;
		for (int32 i = 0; i < N; ++i) R += (R.IsEmpty() ? TEXT("") : TEXT(" | ")) + MenuRowText(i);
		UE_LOG(LogOutlaws, Log, TEXT("OLDEVCHECK %s (%s): %d rows: %s"), What, MenuPage ? *MenuPage->File : TEXT("none"), N, *R);
		return N;
	};
	auto Find = [this](const FString& Target) { return MenuPage ? MenuPage->Items.IndexOfByPredicate([&Target](const OutlawsMenu::FItem& I) { return I.Target == Target || I.Name == Target; }) : INDEX_NONE; };
	auto Finish = [&]()
	{
		CloseMenu();
		UE_LOG(LogOutlaws, Log, TEXT("OLDEVCHECK %s done: %s (%d failed)"), *Mode, Fails ? TEXT("FAIL") : TEXT("PASS"), Fails);
		Step = 100;
		FGenericPlatformMisc::RequestExit(false);
	};
	switch (Step)
	{
	case 0:
		UE_LOG(LogOutlaws, Log, TEXT("OLDEVCHECK %s: enabled %d (devchecks_on %d), list %s"), *Mode, OutlawsDevChecks::Enabled() ? 1 : 0,
			IFileManager::Get().FileExists(*(OutlawsDevChecks::Dir() / TEXT("devchecks_on"))) ? 1 : 0, *OutlawsDevChecks::Dir());
		OpenMenu();
		NextAt = T + 1.f; ++Step; return;
	case 1:
		Check(!bCheatsShown, TEXT("Cheats locked before the turn"));
		if (Find(TEXT("#tools")) != INDEX_NONE) ShowMenuPage(TEXT("#tools"), true);
		Rows(TEXT("Tools page before the turn"));
		Check(Find(TEXT("#devchecks")) == INDEX_NONE, TEXT("no Dev checks row before the unlock"));
		MenuBack();
		NextAt = T + 0.3f; ++Step; return;
	case 2:
		// the gesture: a whole turn of the head (here the view) with the menu up, 20 degrees a step
		if (!bCheatsShown && Turns < 40)
		{
			if (AController* C = GetController()) { FRotator R = C->GetControlRotation(); R.Yaw += 20.f; C->SetControlRotation(R); }
			++Turns;
			NextAt = T + 0.05f;
			return;
		}
		Check(bCheatsShown, FString::Printf(TEXT("the turn on the spot unlocked Cheats (%d steps of 20 degrees)"), Turns));
		NextAt = T + 0.5f; ++Step; return;
	case 3:
	{
		const int32 Tools = Find(TEXT("#tools"));
		if (Tools != INDEX_NONE) MenuActivate(1, Tools);
		Rows(TEXT("Tools page after the turn"));
		const int32 Dev = Find(TEXT("#devchecks"));
		if (Mode == TEXT("nodev"))
		{
			Check(Dev == INDEX_NONE, TEXT("no Dev checks row without the dev flag (the packaged default)"));
			Check(Find(TEXT("#cheats")) != INDEX_NONE, TEXT("the Cheats row is there"));
			Shot(TEXT("devchecks_nodev_tools"));
			NextAt = T + 1.f; Step = 50; return;
		}
		Check(Dev != INDEX_NONE, TEXT("Dev checks row on the Tools page after the unlock"));
		if (Dev == INDEX_NONE) { Finish(); return; }
		Shot(TEXT("devchecks_tools"));
		NextAt = T + 0.8f; ++Step; return;
	}
	case 4:
		MenuActivate(1, Find(TEXT("#devchecks")));
		Rows(TEXT("Dev checks page"));
		Check(MenuPage && MenuPage->File == TEXT("#devchecks"), TEXT("Dev checks page opened"));
		Check(OutlawsDevChecks::Source() == TEXT("bundled"), FString::Printf(TEXT("no pushed list: the bundled list (%d checks) is used (%s)"), OutlawsDevChecks::Checks().Num(), *OutlawsDevChecks::Source()));
		Check(MenuPage && MenuPage->Items.Num() <= RowsOnPage, FString::Printf(TEXT("the page fits without scrolling (%d rows of %d)"), MenuPage ? MenuPage->Items.Num() : 0, RowsOnPage));
		NextAt = T + 0.5f; ++Step; return;
	case 5:
	{
		// cycle the first row twice: Fixed, then Still broken
		const FString Id = OutlawsDevChecks::Checks().Num() ? OutlawsDevChecks::Checks()[0].Id : FString();
		const int32 Row = Find(TEXT("devcheck:") + Id);
		MenuActivate(1, Row);
		Check(OutlawsDevChecks::State(Id) == OutlawsDevChecks::Fixed, FString::Printf(TEXT("%s cycled to Fixed (row: %s)"), *Id, Row != INDEX_NONE ? *MenuRowText(Row) : TEXT("none")));
		Shot(TEXT("devchecks_page"));
		MenuActivate(1, Row);
		FString Text;
		FFileHelper::LoadFileToString(Text, *OutlawsDevChecks::AnswersPath());
		Check(Text.Contains(Id) && Text.Contains(TEXT("\"broken\"")), FString::Printf(TEXT("answers file written (%s): %s"), *OutlawsDevChecks::AnswersPath(), *Text.Left(300).Replace(TEXT("\n"), TEXT(" "))));
		OutlawsDevChecks::Reload();
		Check(OutlawsDevChecks::State(Id) == OutlawsDevChecks::StillBroken, FString::Printf(TEXT("%s reads back as Still broken after a reload"), *Id));
		NextAt = T + 0.8f; ++Step; return;
	}
	case 6:
		// paging: the page row walks the pages and wraps
		if (OutlawsDevChecks::NumPages() > 1)
		{
			MenuActivate(1, Find(TEXT("devchecks_next")));
			Rows(TEXT("Dev checks page 2"));
			Check(OutlawsDevChecks::CurrentPage() == 1 && MenuPage && MenuPage->File == TEXT("#devchecks") && MenuStack.Num() == 2,
				FString::Printf(TEXT("next page shows page 2 of %d, Back still leads to Tools (stack %d)"), OutlawsDevChecks::NumPages(), MenuStack.Num()));
			Shot(TEXT("devchecks_page2"));
			for (int32 k = 1; k < OutlawsDevChecks::NumPages(); ++k) MenuActivate(1, Find(TEXT("devchecks_next")));
			Check(OutlawsDevChecks::CurrentPage() == 0, TEXT("the page row wraps back to page 1"));
		}
		NextAt = T + 0.8f; ++Step; return;
	case 7:
	{
		// a pushed list wins over the bundled one; gone again, the bundled one stands in
		const int32 Bundled = OutlawsDevChecks::Checks().Num();
		const FString Pushed = TEXT("{\"list\":\"test\",\"checks\":[{\"id\":\"TEST-A\",\"what\":\"pushed list row one\"},{\"id\":\"TEST-B\",\"what\":\"pushed list row two\"}]}");
		const bool bHadPushed = IFileManager::Get().FileExists(*OutlawsDevChecks::ListPath());
		FString Old;
		if (bHadPushed) FFileHelper::LoadFileToString(Old, *OutlawsDevChecks::ListPath());
		FFileHelper::SaveStringToFile(Pushed, *OutlawsDevChecks::ListPath());
		ShowMenuPage(TEXT("#devchecks"), false);
		Rows(TEXT("Dev checks page, pushed list"));
		Check(OutlawsDevChecks::Source() == TEXT("pushed") && OutlawsDevChecks::Checks().Num() == 2 && Find(TEXT("devcheck:TEST-B")) != INDEX_NONE,
			FString::Printf(TEXT("the pushed list is read on opening (%s, %d checks)"), *OutlawsDevChecks::Source(), OutlawsDevChecks::Checks().Num()));
		Shot(TEXT("devchecks_pushed"));
		if (bHadPushed) FFileHelper::SaveStringToFile(Old, *OutlawsDevChecks::ListPath()); else IFileManager::Get().Delete(*OutlawsDevChecks::ListPath());
		ShowMenuPage(TEXT("#devchecks"), false);
		Check(OutlawsDevChecks::Source() == (bHadPushed ? TEXT("pushed") : TEXT("bundled")) && OutlawsDevChecks::Checks().Num() == (bHadPushed ? OutlawsDevChecks::Checks().Num() : Bundled),
			FString::Printf(TEXT("without the pushed file the bundled list is back (%s, %d checks)"), *OutlawsDevChecks::Source(), OutlawsDevChecks::Checks().Num()));
		Check(OutlawsDevChecks::State(OutlawsDevChecks::Checks().Num() ? OutlawsDevChecks::Checks()[0].Id : FString()) == OutlawsDevChecks::StillBroken,
			TEXT("the answer survives the list switching"));
		NextAt = T + 0.5f; ++Step; return;
	}
	case 8:
		MenuBack();
		Check(MenuPage && MenuPage->File == TEXT("#tools"), FString::Printf(TEXT("Back returns to Tools (%s)"), MenuPage ? *MenuPage->File : TEXT("none")));
		MenuBack();
		Check(MenuPage && MenuPage->File == TEXT("main_game.rml"), TEXT("Back again returns to the pause page"));
		NextAt = T + 0.5f; Step = 50; return;
	case 50:
		Finish();
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
