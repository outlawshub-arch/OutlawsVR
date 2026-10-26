// REQ-CREDITS (user 2026-10-05, board card): the end credits at the story's end, as James rides off into the sunset (the
// ride-off movie outlaws.rcs names on its CREDITS: line, rae.san).  First the Outlaws VR credits, a plain text file shipped
// with the game (Content/Credits/vr_credits.txt) the planner adds to, then the original game's own credits, read at runtime
// from the player's LABs (credits.txt: outlaws.lab, olpatch3's longer one with the Handful of Missions where the player has
// it), never shipped.  Both in the original's layout: "~" a heading, a plain line a name, an empty line a line of space.
// A missing or unreadable credits.txt leaves that part out, with nothing waiting in its place.  The usual skip ends the roll;
// it then goes on as the ending always did (StartNextLevel: single player to the main menu; co-op as before, below).
//
// Co-op: each machine rolls its own credits on its own screen after its own copy of the movie; nothing new goes over the
// network.  A skip is the movies' skip (either player's skip ends it for both, UOutlawsCoop::NoteMovieSkip), and when the
// host's roll ends the host ends the session as it did when its movie ended ("the game is won"), both to their menus.
//
// Tests: -OLLevel=ranch -OLNoFrontEnd -OLCreditsTest[=roll|noorig|natural] [-OLNoOrigCredits] [-OLCreditsShots]
#include "OutlawsLevelActor.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsPlayer.h"
#include "OutlawsEnemy.h"
#include "OutlawsMovieScreen.h"
#include "OutlawsLab.h"
#include "OutlawsFormats.h"
#include "OutlawsInfRuntime.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "HAL/PlatformTime.h"
#include "UnrealClient.h"

namespace
{
	// the ride-off movie's times (rae.mp4, 128 s): the cut to James riding into the sunset at 76 s; the night held at 125 s,
	// before the last fade to black.  Another movie on that line: the roll from its 76th second (or on the black after it, if
	// it is shorter), its picture held 1.5 s before its end.
	constexpr double CreditsRollFrom = 76.0;
	constexpr double CreditsHoldFrom = 125.0;
	constexpr int32 CreditsPartGap = 6;              // empty lines between the two parts

	// the original's credits.txt layout, a line at a time: "~" a heading, a plain line a name, an empty line a line of space
	// (tabs and spaces at either end dropped); '#' starts a comment line in our own file only
	int32 AddCreditsText(TArray<FOutlawsCreditLine>& Out, const FString& Text, int32 Part, bool bComments)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, false);
		const int32 Was = Out.Num();
		for (const FString& Raw : Lines)
		{
			const FString L = Raw.TrimStartAndEnd();
			if (bComments && L.StartsWith(TEXT("#"))) continue;
			FOutlawsCreditLine C;
			C.Part = Part;
			if (L.StartsWith(TEXT("~")))
			{
				C.Text = L.Mid(1).TrimStartAndEnd();
				C.bHeading = !C.Text.IsEmpty();
			}
			else
			{
				C.Text = L;
			}
			if (C.Text.IsEmpty() && Out.Num() == Was) continue;       // (no space before a part's first line)
			Out.Add(C);
		}
		while (Out.Num() > Was && Out.Last().Text.IsEmpty()) Out.Pop();
		return Out.Num() - Was;
	}

	FString CreditsVrFile()
	{
		return FPaths::ProjectContentDir() / TEXT("Credits") / TEXT("vr_credits.txt");
	}
}

bool AOutlawsLevelActor::IsStoryEndMovie(const FString& MovieName) const
{
	// the story's end: no level after this one, and the movie it ends on is the one outlaws.rcs gives its CREDITS: line
	if (MovieName.IsEmpty() || bHom || bFrontEnd || bMovieToMenu || !NextLevelName.IsEmpty() || !AssetsRef) return false;
	FString Text;
	if (!AssetsRef->GetText(TEXT("outlaws.rcs"), Text)) return false;
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	for (const FString& Line : Lines)
	{
		TArray<FString> T;
		Line.ParseIntoArrayWS(T);
		if (T.Num() >= 2 && T[0].Equals(TEXT("CREDITS:"), ESearchCase::IgnoreCase)) return FPaths::GetBaseFilename(T[1]).Equals(MovieName, ESearchCase::IgnoreCase);
	}
	return false;
}

bool AOutlawsLevelActor::BuildEndCredits(TArray<FOutlawsCreditLine>& Out) const
{
	Out.Reset();
	// ours first
	FString Vr;
	int32 NVr = 0;
	if (FFileHelper::LoadFileToString(Vr, *CreditsVrFile())) NVr = AddCreditsText(Out, Vr, 0, true);
	// then the original's, from the player's own files (the LAB that holds it last in the patch order: olpatch3's where it is
	// there); missing, unreadable or empty: left out, with nothing waiting in its place
	const bool bNoOrig = OLFlags::Param(TEXT("OLNoOrigCredits"));
	FString Orig;
	int32 NOrig = 0;
	if (!bNoOrig && AssetsRef && AssetsRef->GetText(TEXT("credits.txt"), Orig))
	{
		TArray<FOutlawsCreditLine> Part;
		NOrig = AddCreditsText(Part, Orig, 1, false);
		if (NOrig > 0 && Out.Num() > 0)
		{
			for (int32 i = 0; i < CreditsPartGap; ++i) { FOutlawsCreditLine Gap; Gap.Part = 0; Out.Add(Gap); }
		}
		Out.Append(Part);
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT credits: Outlaws VR %d lines (%s), the original %d lines (%s)"), NVr, *CreditsVrFile(), NOrig,
		bNoOrig ? TEXT("left out: -OLNoOrigCredits") : NOrig > 0 ? *FString::Printf(TEXT("credits.txt from %s"), *FPaths::GetCleanFilename(AssetsRef->Where(TEXT("credits.txt"))))
		: TEXT("none in the player's files: left out"));
	return Out.ContainsByPredicate([](const FOutlawsCreditLine& L) { return !L.Text.IsEmpty(); });
}

void AOutlawsLevelActor::ArmEndCredits(const FString& MovieName)
{
	if (!Movie || !IsStoryEndMovie(MovieName)) return;
	TArray<FOutlawsCreditLine> Lines;
	if (!BuildEndCredits(Lines)) return;
	OutlawsFormats::FLafFont Font;
	TArray<uint8> FontBytes;
	const bool bFont = AssetsRef && AssetsRef->Get(TEXT("mf3s.laf"), FontBytes) && OutlawsFormats::DecodeLaf(FontBytes, Font);
	Movie->ArmCredits(Lines, bFont ? &Font : nullptr, CreditsRollFrom, CreditsHoldFrom);
	// the original's credits music (credits.msc) once the picture is held and the movie's own sound is over
	Movie->OnCreditsHeld.AddWeakLambda(this, [this]() { StartMusic(TEXT("credits.msc")); });
}

bool AOutlawsLevelActor::StartEndCreditsIfDue()
{
	if (!IsStoryEndMovie(NextMovie)) return false;
	if (Movie && Movie->CreditsDone()) return false;           // (rolled over the movie, or skipped with it: the end goes on)
	UWorld* World = GetWorld();
	AOutlawsPlayer* P = World && World->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(World->GetFirstPlayerController()->GetPawn()) : nullptr;
	TArray<FOutlawsCreditLine> Lines;
	if (!World || !P || !BuildEndCredits(Lines)) return false;
	// the movie never rolled them (not converted, would not open, or no cutscene and the counts on the black): on the black
	if (Movie)
	{
		Movie->OnFinished.RemoveAll(this);
		Movie->Destroy();
		Movie = nullptr;
	}
	P->GetCharacterMovement()->DisableMovement();
	P->GetCharacterMovement()->Velocity = FVector::ZeroVector;
	FActorSpawnParameters SP;
	SP.ObjectFlags |= RF_Transient;
	Movie = World->SpawnActor<AOutlawsMovieScreen>(AOutlawsMovieScreen::StaticClass(), FTransform::Identity, SP);
	if (!Movie) return false;
	Movie->OnFinished.AddUObject(this, &AOutlawsLevelActor::StartNextLevel);
	Movie->OnCreditsHeld.AddWeakLambda(this, [this]() { StartMusic(TEXT("credits.msc")); });
	FadeCamera(1.f, 0.f, 1.f, false);
	OutlawsFormats::FLafFont Font;
	TArray<uint8> FontBytes;
	const bool bFont = AssetsRef && AssetsRef->Get(TEXT("mf3s.laf"), FontBytes) && OutlawsFormats::DecodeLaf(FontBytes, Font);
	const UCameraComponent* Cam = P->FindComponentByClass<UCameraComponent>();
	UE_LOG(LogOutlaws, Log, TEXT("OLRT credits on the black (the end movie %s did not roll them)"), *NextMovie);
	Movie->PlayCredits(Lines, bFont ? &Font : nullptr, Cam ? Cam->GetComponentLocation() : P->GetActorLocation(), Cam ? Cam->GetForwardVector() : P->GetActorForwardVector());
	SetActorTickEnabled(true);
	return true;
}

bool AOutlawsLevelActor::PlayExtrasCredits()
{
	// (REQ-EXTRAS) as the story's end rolls them with no movie (StartEndCreditsIfDue), from the main menu
	UWorld* World = GetWorld();
	AOutlawsPlayer* P = World && World->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(World->GetFirstPlayerController()->GetPawn()) : nullptr;
	TArray<FOutlawsCreditLine> Lines;
	if (!World || !P || !bFrontEnd || !BuildEndCredits(Lines)) return false;
	if (Movie)
	{
		Movie->OnFinished.RemoveAll(this);
		Movie->Destroy();
		Movie = nullptr;
	}
	FActorSpawnParameters SP;
	SP.ObjectFlags |= RF_Transient;
	Movie = World->SpawnActor<AOutlawsMovieScreen>(AOutlawsMovieScreen::StaticClass(), FTransform::Identity, SP);
	if (!Movie) return false;
	if (P->IsMenuOpen()) P->CloseGameMenu();
	Movie->OnFinished.AddUObject(this, &AOutlawsLevelActor::ExtrasCreditsDone);
	StopMusic();
	bMenuMusic = false;
	StartMusic(TEXT("credits.msc"));
	OutlawsFormats::FLafFont Font;
	TArray<uint8> FontBytes;
	const bool bFont = AssetsRef && AssetsRef->Get(TEXT("mf3s.laf"), FontBytes) && OutlawsFormats::DecodeLaf(FontBytes, Font);
	const UCameraComponent* Cam = P->FindComponentByClass<UCameraComponent>();
	UE_LOG(LogOutlaws, Log, TEXT("OLRT credits from Extras: %d lines on the black"), Lines.Num());
	Movie->PlayCredits(Lines, bFont ? &Font : nullptr, Cam ? Cam->GetComponentLocation() : P->GetActorLocation(), Cam ? Cam->GetForwardVector() : P->GetActorForwardVector());
	SetActorTickEnabled(true);
	return true;
}

void AOutlawsLevelActor::ExtrasCreditsDone()
{
	UE_LOG(LogOutlaws, Log, TEXT("OLRT credits from Extras: over, back to the Extras page"));
	StopMusic();
	ShowFrontMenu();
	UWorld* World = GetWorld();
	if (AOutlawsPlayer* P = World && World->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(World->GetFirstPlayerController()->GetPawn()) : nullptr) P->ShowExtrasPage();
}

// ---- -OLCreditsTest: the ending, the roll, its parts, the skip -----------------------------------------------------------------

#if WITH_OUTLAWS_TESTS
void AOutlawsLevelActor::TickCreditsTest(float Dt)
{
	static int32 Step = 0, Passes = 0, Fails = 0;
	static double T = 0.0, Mark = 0.0, SkipAt = 0.0, RollAt = 0.0, HeldAt = 0.0;
	static FString Mode;
	if (Step < 0) return;
	if (Mode.IsEmpty())
	{
		if (!OLFlags::Value(TEXT("OLCreditsTest"), Mode)) Mode = TEXT("roll");
		Mode = Mode.ToLower();
	}
	T += Dt;
	UWorld* World = GetWorld();
	AOutlawsPlayer* P = World && World->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(World->GetFirstPlayerController()->GetPawn()) : nullptr;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT credits check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	auto Done = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT credits %s RESULT %s (%d passed, %d failed)"), *Mode, Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		Step = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	const bool bShots = OLFlags::Param(TEXT("OLCreditsShots"));
	auto Shot = [&](const TCHAR* Name)
	{
		if (!bShots || !Movie) return;
		const FString Dir = FPaths::ProjectSavedDir() / TEXT("OLShots");
		const FString Base = FString::Printf(TEXT("credits_%s_%s"), *Mode, Name);
		const bool bCanvas = Movie->SaveCrawlPng(Dir / Base + TEXT("_canvas.png"));
		FScreenshotRequest::RequestScreenshot(Dir / Base + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT credits shot %s (canvas %d), roll %.1f of %.0f s, movie held %d"), *Base, bCanvas ? 1 : 0, Movie->CreditsAgeNow(), Movie->CreditsSeconds(),
			Movie->IsCreditsHeld() ? 1 : 0);
	};
	const bool bNatural = Mode == TEXT("natural");
	const bool bNoOrig = Mode == TEXT("noorig");
	switch (Step)
	{
	case 0:
	{
		if (!P || bNeedPlace || bFrontEnd || !Inf || bRebuildPending || T < 4.0) return;
		bInvincible = true;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT credits test (%s) in %s: the level is up"), *Mode, *LevelName);
		if (!bNatural)
		{
			CompleteLevel(TEXT("credits test"));
			Mark = T;
			Step = 10;
			return;
		}
		// the final level's own ending: Bob's door with its key (its movie, skipped), then Bob shot down
		GiveItem(101);
		bool bPushed = false;
		for (int32 i = 0; i < Inf->Elevators.Num(); ++i)
		{
			if (!Inf->Elevators[i].Name.Equals(TEXT("BOBDOOR"), ESearchCase::IgnoreCase) || Inf->Elevators[i].Sectors.Num() == 0) continue;
			bPushed = PushDoorSector(Inf->Elevators[i].Sectors[0]);
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT credits natural: BOBDOOR pushed with key 101: %s"), bPushed ? TEXT("taken") : TEXT("not taken"));
		Mark = T;
		Step = 1;
		return;
	}
	case 1:
		if (T - Mark < 4.0) return;
		if (IsMoviePlaying()) SkipMovie();          // (the door's movie)
		Mark = T;
		Step = 2;
		return;
	case 2:
	{
		if (T - Mark < 8.0 || IsMoviePlaying()) return;
		AOutlawsEnemy* Bob = nullptr;
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : Enemies) if (Wk.IsValid() && Wk->bPeekShooter) Bob = Wk.Get();
		const FVector From = P ? P->GetActorLocation() : FVector::ZeroVector;
		for (int32 k = 0; Bob && k < 60 && !Bob->IsDead(); ++k) Bob->TakeHit(50.f, Bob->GetActorLocation(), (Bob->GetActorLocation() - From).GetSafeNormal(), From);
		Check(Bob && Bob->IsDead(), TEXT("Bob shot down: the level's own ending"));
		if (!Bob || !Bob->IsDead()) CompleteLevel(TEXT("credits test: Bob would not fall"));
		Mark = T;
		Step = 10;
		return;
	}
	case 10:
		// the ride-off movie, with the credits armed on its screen
		if (!(Movie && Movie->HasCredits() && Movie->IsPlaying() && Movie->GetTime() > 0.5f))     // (open and going: a seek before is lost)
		{
			if (T - Mark > 40.0) { Check(false, TEXT("the end movie with the credits on its screen came")); Done(); }
			return;
		}
		Check(true, FString::Printf(TEXT("the end movie %s plays with the credits armed: Outlaws VR %d lines, the original %d"), *NextMovie, Movie->CreditsLineCount(0), Movie->CreditsLineCount(1)));
		Check(Movie->CreditsLineCount(0) > 0, TEXT("the Outlaws VR part is there"));
		Check(bNoOrig ? Movie->CreditsLineCount(1) == 0 : Movie->CreditsLineCount(1) > 0, bNoOrig ? TEXT("the original part left out (-OLNoOrigCredits)") : TEXT("the original part read from the player's files"));
		if (!bNatural) Movie->SeekMovie(72.f);           // (to just before James rides off)
		Mark = T;
		Step = 11;
		return;
	case 11:
		if (!Movie || !Movie->IsCreditsRolling())
		{
			if (!Movie || !Movie->IsPlaying() || T - Mark > (bNatural ? 150.0 : 30.0)) { Check(false, TEXT("the roll starts over the movie")); Done(); }
			return;
		}
		RollAt = T;
		Check(true, TEXT("the roll starts over the movie as James rides off"));
		Mark = T;
		Step = 12;
		return;
	case 12:
		if (T - Mark < 1.5) return;
		Shot(TEXT("1_start"));
		Mark = T;
		Step = bNatural ? 40 : 20;
		return;
	case 20:
		if (T - Mark < 0.5) return;                // (the view's shot is taken at the end of its frame: no seek in that one)
		Mark = T;
		if (bNoOrig)
		{
			// the last of our lines just under the top: with nothing after it the roll ends as it goes, and the menu is up at once
			Movie->SeekCredits(Movie->CreditsLineCount(0) - 1, 0.1f);
			SkipAt = T;
			Step = 30;
			return;
		}
		Movie->SeekCredits(Movie->CreditsFirstLine(0), 0.12f);
		Step = 13;
		return;
	case 13:
		if (T - Mark < 0.6) return;
		Movie->SeekCredits(Movie->CreditsFirstLine(0), 0.12f);
		Shot(TEXT("2_vr"));
		// (no second seek: a far seek while the roll draws stalled the decoder here; the movie plays on to its hold)
		Mark = T;
		Step = 14;
		return;
	case 14:
		if (!Movie->IsCreditsHeld())
		{
			if (FMath::FloorToInt((T - Mark) / 10.0) != FMath::FloorToInt((T - Mark - Dt) / 10.0)) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT credits: waiting for the hold, movie at %.1f s"), Movie->GetTime());
			if (T - Mark > 70.0) { Check(false, TEXT("the picture held near the movie's end")); Done(); }
			return;
		}
		Check(true, TEXT("the picture held near the movie's end, the roll going on"));
		Movie->SeekCredits(Movie->CreditsLineCount(0) - 3, 0.25f);
		Mark = T;
		Step = 15;
		return;
	case 15:
		if (T - Mark < 2.0) return;
		Movie->SeekCredits(Movie->CreditsLineCount(0) - 3, 0.25f);
		Shot(TEXT("3_held_join"));
		Mark = T;
		Step = 21;
		return;
	case 21:
		if (T - Mark < 0.5) return;
		Movie->SeekCredits(Movie->CreditsFirstLine(1), 0.15f);
		Mark = T;
		Step = 16;
		return;
	case 16:
		if (T - Mark < 0.6) return;
		Movie->SeekCredits(Movie->CreditsFirstLine(1), 0.15f);
		Shot(TEXT("4_original"));
		Mark = T;
		Step = 17;
		return;
	case 17:
		if (T - Mark < 0.6) return;
		// further on: halfway through the original's
		Movie->SeekCredits(Movie->CreditsLineCount(0) + Movie->CreditsLineCount(1) / 2, 0.1f);
		Mark = T;
		Step = 18;
		return;
	case 18:
		if (T - Mark < 0.6) return;
		Movie->SeekCredits(Movie->CreditsLineCount(0) + Movie->CreditsLineCount(1) / 2, 0.1f);
		Shot(TEXT("5_original_mid"));
		Mark = T;
		Step = 19;
		return;
	case 19:
		if (T - Mark < 1.0) return;
		// the usual skip: the roll ends, and the ending goes on to the main menu
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT credits: the skip at %.1f of %.0f s"), Movie->CreditsAgeNow(), Movie->CreditsSeconds());
		SkipMovie();
		SkipAt = T;
		Step = 30;
		return;
	case 30:
		if (!bFrontEnd)
		{
			if (T - SkipAt > 20.0) { Check(false, TEXT("back to the main menu")); Done(); }
			return;
		}
		if (bNoOrig)
		{
			// the last line from 10% down to over the top, and no more: (0.1 x 1080 + a line) / 66 px a second, about 2.3 s
			Check(T - SkipAt < 3.5, FString::Printf(TEXT("with no original credits the menu is up as our last line goes (%.2f s after it was 10%% from the top)"), T - SkipAt));
		}
		else
		{
			Check(T - SkipAt < 1.0, FString::Printf(TEXT("the skip ends the roll and the main menu is up (%.2f s)"), T - SkipAt));
		}
		Done();
		return;
	case 40:
		// natural: no seeks, no skip: held at its point, then rolled to the end, then the main menu
		if (!bFrontEnd && Movie && Movie->IsCreditsHeld() && HeldAt == 0.0)
		{
			HeldAt = T;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT credits natural: the picture held %.1f s after the roll began"), T - RollAt);
			Mark = T;
			Step = 41;
			return;
		}
		if (bFrontEnd) Step = 42;
		return;
	case 41:
		if (T - Mark < 2.0) return;
		Shot(TEXT("2_held"));
		Step = 42;
		return;
	case 42:
		if (!bFrontEnd) return;
		Check(HeldAt > 0.0, TEXT("the picture was held at its point"));
		Check(true, FString::Printf(TEXT("the roll ran to its end (%.0f s from its start) and the main menu followed"), T - RollAt));
		Done();
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
