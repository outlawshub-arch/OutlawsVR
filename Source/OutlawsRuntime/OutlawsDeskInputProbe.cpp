// BUG-DESKSAVE's and BUG-DESKSKIP's probe, the desktop's keys in a window with no headset: -OLShots -OLNoFrontEnd -OLLevel=hideout
// -OLDeskInputProbe.  Every key goes in as a real one does, through the game viewport to the player's bindings (a control
// first: Space reaches the jump).
// (1) Escape in play opens the pause menu; the view's middle (the desktop's pointer) on Save Game and a left click opens its
// page; a click on the first empty slot writes slotN.json, and the page lists it; Escape goes back a page; Load Game's page
// lists the save too; Escape twice: back to the pause page, then the menu shut.
// (2) The level won, its cutscene: Escape holds it behind the pause menu and Escape again lets it go on; a left button already
// down as it began counts for nothing; the left button held: no bar for the first half second, then the bar, and the movie
// ends about 3 s into the hold, well before its own end.
// Lines "OLSHOT deskinput check PASS|FAIL: ..." and "OLSHOT deskinput RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsMovieScreen.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Camera/CameraComponent.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "InputCoreTypes.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsDeskInputProbeLocal
{
	int32 Stage = 0, Passes = 0, Fails = 0, Slot = -1;
	float StageT = 0.f, HeldFor = 0.f, AgeAtPause = 0.f, LastAge = 0.f, LastTime = 0.f;
	bool bEarlyChecked = false, bBarChecked = false;
	FString SlotFile;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT deskinput check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}

	/** A key or mouse button down or up as the window gives it: the game viewport, then the player controller's bindings. */
	void Key(AOutlawsPlayer& P, const FKey& K, EInputEvent E)
	{
		UWorld* W = P.GetWorld();
		UGameViewportClient* GV = W ? W->GetGameViewport() : nullptr;
		const float Amount = E == IE_Released ? 0.f : 1.f;
		if (GV) GV->InputKey(FInputKeyEventArgs(GV->Viewport, 0, K, E, Amount, false));
		else if (APlayerController* PC = Cast<APlayerController>(P.GetController())) PC->InputKey(FInputKeyParams(K, E, double(Amount), false));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT deskinput key %s %s (%s)"), *K.ToString(), E == IE_Released ? TEXT("up") : TEXT("down"), GV ? TEXT("the game viewport") : TEXT("the player controller"));
	}

	void Finished(const TCHAR* Why)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT deskinput RESULT %s (%d passed, %d failed%s%s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			*Why ? TEXT(", ") : TEXT(""), Why);
		Stage = -1;
		FGenericPlatformMisc::RequestExit(false);
	}
}

OL_SHOT_MATCH(OLDeskInputProbe) { return OLFlags::Param(TEXT("OLDeskInputProbe")); }

OL_SHOT_TEST(OLDeskInputProbe, 2531)
{
	using namespace OutlawsDeskInputProbeLocal;
	if (!L || ShotTime < 0.f || Stage < 0) return;
	AOutlawsMovieScreen* M = L->GetMovieScreen();
	const bool bPlaying = L->IsMoviePlaying();
	const FString Page = bMenuOpen && MenuPage ? MenuPage->File : FString(TEXT("none"));
	auto Next = [&]() { ++Stage; StageT = 0.f; };
	auto Go = [&](int32 To) { Stage = To; StageT = 0.f; };
	// the item of the page shown whose link opens File (INDEX_NONE: none)
	auto ItemTo = [&](const TCHAR* File) -> int32
	{
		if (!bMenuOpen || !MenuPage) return INDEX_NONE;
		return MenuPage->Items.IndexOfByPredicate([File](const OutlawsMenu::FItem& It) { return It.Target.Equals(File, ESearchCase::IgnoreCase); });
	};
	// the row the view's middle is on now, as a left click reads it
	auto RowInView = [&]() -> int32
	{
		float Unused = 0.f;
		return bMenuOpen && Camera ? MenuRowUnderRay(Camera->GetComponentLocation(), Camera->GetForwardVector(), Unused) : -1;
	};
	// the view turned so its middle is on item Item of the page shown (the rows' middle of all the ways that meet it)
	auto AimAt = [&](int32 Item) -> bool
	{
		if (!bMenuOpen || !MenuPage || !Camera || !Controller || Item < 0 || !MenuPage->Message.IsEmpty()) return false;
		const int32 Row = Item - MenuScroll;
		const FVector Eye = Camera->GetComponentLocation();
		double SumPitch = 0.0, SumYaw = 0.0;
		int32 N = 0;
		for (double Pitch = -35.0; Pitch <= 35.0; Pitch += 0.5)
		{
			for (double Yaw = -40.0; Yaw <= 40.0; Yaw += 0.5)
			{
				float Unused = 0.f;
				if (MenuRowUnderRay(Eye, FRotator(Pitch, MenuYaw + Yaw, 0.0).Vector(), Unused) == Row) { SumPitch += Pitch; SumYaw += Yaw; ++N; }
			}
		}
		if (N == 0) return false;
		Controller->SetControlRotation(FRotator(SumPitch / N, MenuYaw + SumYaw / N, 0.0));
		return true;
	};
	StageT += Dt;
	if (Stage > 0 && StageT > 40.f)
	{
		Check(false, FString::Printf(TEXT("stage %d waited 40 s (menu %d, page %s, movie %d)"), Stage, bMenuOpen ? 1 : 0, *Page, bPlaying ? 1 : 0));
		Key(*this, EKeys::LeftMouseButton, IE_Released);
		Finished(TEXT("timed out"));
		return;
	}
	if (M && bPlaying)
	{
		LastAge = M->GetAge();
		LastTime = M->GetTime();
	}
	switch (Stage)
	{
	// ---- (0) a desktop, and a key put in this way reaches the player's bindings ----
	case 0:
		if (L->IsRebuildPending() || bMenuOpen || bPlaying || StageT < 3.f) return;
		L->StartSaveDelay = -1.f;
		L->AutosaveWait = 0.f;
		Check(!bVR, FString::Printf(TEXT("a desktop window, no headset (bVR %d)"), bVR ? 1 : 0));
		Key(*this, EKeys::SpaceBar, IE_Pressed);
		Next();
		return;
	case 1:
		if (StageT < 0.2f) return;
		Check(bJumpKeyDown, FString::Printf(TEXT("control: Space put in through the game viewport reaches the jump (held %d)"), bJumpKeyDown ? 1 : 0));
		Key(*this, EKeys::SpaceBar, IE_Released);
		Next();
		return;
	// ---- (1) the pause menu by Escape, a manual save by the mouse ----
	case 2:
		if (StageT < 1.5f) return;          // (the jump down again)
		Key(*this, EKeys::Escape, IE_Pressed);
		Next();
		return;
	case 3:
		if (StageT < 0.1f) return;
		Key(*this, EKeys::Escape, IE_Released);
		Next();
		return;
	case 4:
	{
		if (StageT < 0.4f) return;
		const bool bPause = bMenuOpen && !bFrontMenu && Page == TEXT("main_game.rml");
		Check(bPause, FString::Printf(TEXT("Escape in play opens the pause menu (open %d, page %s)"), bMenuOpen ? 1 : 0, *Page));
		const int32 Item = ItemTo(TEXT("save_game.rml"));
		if (!bPause || !AimAt(Item))
		{
			Check(false, FString::Printf(TEXT("the save part not run: no pause page with Save Game to point at (item %d)"), Item));
			Go(30);
			return;
		}
		Next();
		return;
	}
	case 5:
	{
		if (StageT < 0.4f) return;
		const int32 Item = ItemTo(TEXT("save_game.rml"));
		Check(Item != INDEX_NONE && RowInView() == Item - MenuScroll, FString::Printf(TEXT("the view's middle on Save Game (row %d, Save Game item %d)"), RowInView(), Item));
		Key(*this, EKeys::LeftMouseButton, IE_Pressed);
		Next();
		return;
	}
	case 6:
		if (StageT < 0.1f) return;
		Key(*this, EKeys::LeftMouseButton, IE_Released);
		Next();
		return;
	case 7:
	{
		if (StageT < 0.4f) return;
		Check(Page == TEXT("save_game.rml"), FString::Printf(TEXT("a left click on Save Game opens its page (page %s)"), *Page));
		int32 Want = INDEX_NONE;
		if (Page == TEXT("save_game.rml")) Want = SlotsShown.IndexOfByPredicate([](const FSlotShown& S) { return S.bEmpty && S.Slot >= 0; });
		Slot = SlotsShown.IsValidIndex(Want) ? SlotsShown[Want].Slot : -1;
		SlotFile = Slot >= 0 ? AOutlawsLevelActor::SavePath(Slot) : FString();
		const bool bFree = Slot >= 0 && !FPaths::FileExists(SlotFile);
		Check(bFree, FString::Printf(TEXT("an empty slot to save to: slot %d, no file %s yet"), Slot, *FPaths::GetCleanFilename(SlotFile)));
		if (!bFree || !AimAt(Want))
		{
			Go(16);
			return;
		}
		Next();
		return;
	}
	case 8:
	{
		if (StageT < 0.4f) return;
		const int32 Want = SlotsShown.IndexOfByPredicate([](const FSlotShown& S) { return S.Slot == Slot; });
		Check(RowInView() == Want - MenuScroll, FString::Printf(TEXT("the view's middle on slot %d's capsule (row %d)"), Slot, RowInView()));
		Key(*this, EKeys::LeftMouseButton, IE_Pressed);
		Next();
		return;
	}
	case 9:
		if (StageT < 0.1f) return;
		Key(*this, EKeys::LeftMouseButton, IE_Released);
		Next();
		return;
	case 10:
	{
		if (StageT < 0.6f) return;
		const int64 Size = FPaths::FileExists(SlotFile) ? IFileManager::Get().FileSize(*SlotFile) : -1;
		const AOutlawsLevelActor::FSaveInfo Info = AOutlawsLevelActor::ReadSaveInfo(Slot);
		Check(Size > 100 && !Info.bEmpty && Info.Level.Equals(L->LevelName, ESearchCase::IgnoreCase),
			FString::Printf(TEXT("the click wrote the manual save: %s, %lld bytes, level %s (this level %s), named \"%s\""), *FPaths::GetCleanFilename(SlotFile), Size, *Info.Level, *L->LevelName, *Info.Name));
		const int32 Shown = SlotsShown.IndexOfByPredicate([](const FSlotShown& S) { return S.Slot == Slot; });
		const bool bListed = Page == TEXT("save_game.rml") && Shown != INDEX_NONE && !SlotsShown[Shown].bEmpty && MenuPage->Items.IsValidIndex(Shown) && MenuPage->Items[Shown].Label != TEXT("Empty");
		Check(bListed, FString::Printf(TEXT("the Save Game page lists it (page %s, slot %d shown as \"%s\")"), *Page, Slot,
			MenuPage && MenuPage->Items.IsValidIndex(Shown) ? *MenuPage->Items[Shown].Label : TEXT("nothing")));
		Key(*this, EKeys::Escape, IE_Pressed);
		Next();
		return;
	}
	case 11:
		if (StageT < 0.1f) return;
		Key(*this, EKeys::Escape, IE_Released);
		Next();
		return;
	case 12:
	{
		if (StageT < 0.4f) return;
		Check(bMenuOpen && Page == TEXT("main_game.rml"), FString::Printf(TEXT("Escape on the Save Game page goes back to the pause page (page %s)"), *Page));
		if (!AimAt(ItemTo(TEXT("load_game.rml"))))
		{
			Check(false, TEXT("no Load Game row to point at"));
			Go(18);
			return;
		}
		Next();
		return;
	}
	case 13:
	{
		if (StageT < 0.4f) return;
		const int32 Item = ItemTo(TEXT("load_game.rml"));
		Check(Item != INDEX_NONE && RowInView() == Item - MenuScroll, FString::Printf(TEXT("the view's middle on Load Game (row %d, Load Game item %d)"), RowInView(), Item));
		Key(*this, EKeys::LeftMouseButton, IE_Pressed);
		Next();
		return;
	}
	case 14:
		if (StageT < 0.1f) return;
		Key(*this, EKeys::LeftMouseButton, IE_Released);
		Next();
		return;
	case 15:
	{
		if (StageT < 0.4f) return;
		const FString Name = FString::Printf(TEXT("load:%d"), Slot);
		const int32 Shown = Page == TEXT("load_game.rml") ? MenuPage->Items.IndexOfByPredicate([&Name](const OutlawsMenu::FItem& It) { return It.Name == Name; }) : INDEX_NONE;
		const bool bListed = Shown != INDEX_NONE && SlotsShown.IsValidIndex(Shown) && !SlotsShown[Shown].bEmpty && MenuPage->Items[Shown].Label != TEXT("Empty");
		Check(bListed, FString::Printf(TEXT("the Load Game page lists the save (page %s, %s shown as \"%s\")"), *Page, *Name, Shown != INDEX_NONE ? *MenuPage->Items[Shown].Label : TEXT("nothing")));
		Next();
		return;
	}
	case 16:
		// Escape twice: back to the pause page, then the menu shut
		Key(*this, EKeys::Escape, IE_Pressed);
		Next();
		return;
	case 17:
		if (StageT < 0.1f) return;
		Key(*this, EKeys::Escape, IE_Released);
		Next();
		return;
	case 18:
		if (StageT < 0.4f) return;
		Check(bMenuOpen && Page == TEXT("main_game.rml"), FString::Printf(TEXT("Escape goes back to the pause page (page %s)"), *Page));
		Key(*this, EKeys::Escape, IE_Pressed);
		Next();
		return;
	case 19:
		if (StageT < 0.1f) return;
		Key(*this, EKeys::Escape, IE_Released);
		Next();
		return;
	case 20:
		if (StageT < 0.4f) return;
		Check(!bMenuOpen, FString::Printf(TEXT("Escape on the pause page shuts the menu (open %d, page %s)"), bMenuOpen ? 1 : 0, *Page));
		Go(30);
		return;
	// ---- (2) the cutscene: Escape pauses it, the left button held skips it ----
	case 30:
		if (StageT < 0.5f) return;
		if (bMenuOpen) CloseMenu();          // (only when the part before failed)
		Key(*this, EKeys::LeftMouseButton, IE_Pressed);          // (down before the movie: it must be let go first)
		L->StartSaveDelay = -1.f;
		L->AutosaveWait = 0.f;
		L->CompleteLevel(TEXT("the desktop input probe"));
		Next();
		return;
	case 31:
		if (!bPlaying || !M || M->GetAge() < 2.f) return;
		Check(MovieHold == 0.f && !M->SkipBarShown(), FString::Printf(TEXT("a left button down since before the movie counts for nothing (%.2f s in, hold %.2f, bar %d)"),
			M->GetAge(), MovieHold, M->SkipBarShown() ? 1 : 0));
		Key(*this, EKeys::LeftMouseButton, IE_Released);
		Next();
		return;
	case 32:
		if (StageT < 0.3f) return;
		Key(*this, EKeys::Escape, IE_Pressed);
		Next();
		return;
	case 33:
		if (StageT < 0.1f) return;
		Key(*this, EKeys::Escape, IE_Released);
		Next();
		return;
	case 34:
		if (StageT < 0.4f) return;
		Check(bMenuOpen && bPlaying && M && M->bPaused, FString::Printf(TEXT("Escape over the movie: the pause menu up, the movie held behind it (open %d, page %s, paused %d)"),
			bMenuOpen ? 1 : 0, *Page, M && M->bPaused ? 1 : 0));
		AgeAtPause = M ? M->GetAge() : 0.f;
		Key(*this, EKeys::Escape, IE_Pressed);
		Next();
		return;
	case 35:
		if (StageT < 0.1f) return;
		Key(*this, EKeys::Escape, IE_Released);
		Next();
		return;
	case 36:
		if (StageT < 0.6f) return;
		Check(!bMenuOpen && bPlaying && M && !M->bPaused && M->GetAge() > AgeAtPause, FString::Printf(TEXT("Escape again: the menu shut, the movie goes on (open %d, %.2f s in, %.2f at the pause)"),
			bMenuOpen ? 1 : 0, M ? M->GetAge() : -1.f, AgeAtPause));
		if (bMenuOpen) CloseMenu();
		if (!bPlaying)
		{
			Finished(TEXT("the movie was over before the hold"));
			return;
		}
		Key(*this, EKeys::LeftMouseButton, IE_Pressed);
		HeldFor = 0.f;
		bEarlyChecked = bBarChecked = false;
		Next();
		return;
	case 37:
		HeldFor += Dt;
		if (!bEarlyChecked && HeldFor >= 0.3f)
		{
			bEarlyChecked = true;
			Check(MovieHold == 0.f && M && !M->SkipBarShown(), FString::Printf(TEXT("0.3 s into the left button's hold: no bar yet (hold %.2f)"), MovieHold));
		}
		if (!bBarChecked && HeldFor >= 1.f && bPlaying)
		{
			bBarChecked = true;
			Check(MovieHold > 0.f && M && M->SkipBarShown(), FString::Printf(TEXT("1 s into the hold: the bar shows (hold %.2f)"), MovieHold));
		}
		if (bPlaying && HeldFor < 6.f) return;
		Key(*this, EKeys::LeftMouseButton, IE_Released);
		Check(bBarChecked && !bPlaying && HeldFor > 2.7f && HeldFor < 3.4f, FString::Printf(TEXT("the left button held skips the movie in about 3 s: %.2f s held, it ended %.1f s in (the picture at %.1f s)%s"),
			HeldFor, LastAge, LastTime, bPlaying ? TEXT(", STILL PLAYING") : TEXT("")));
		if (bPlaying) L->SkipMovie();
		Next();
		return;
	case 38:
		if (StageT < 1.f) return;
		Finished(TEXT(""));
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
