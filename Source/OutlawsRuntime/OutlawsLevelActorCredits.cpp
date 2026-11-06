// REQ-CREDITS (user 2026-10-05, board card): the end credits at the story's end, as James rides off into the sunset (the
// ride-off movie outlaws.rcs names on its CREDITS: line, rae.san).  First the Outlaws VR credits, a plain text file shipped
// with the game (Content/Credits/vr_credits.txt) the planner adds to, then the original game's own credits, read at runtime
// from the player's LABs (credits.txt: outlaws.lab, olpatch3's longer one with the Handful of Missions where the player has
// it), never shipped.  Both in the original's layout: "~" a heading, a plain line a name, an empty line a line of space.
// A missing or unreadable credits.txt leaves that part out, with nothing waiting in its place.  The usual skip ends the roll;
// it then goes on as the ending always did (StartNextLevel: single player to the main menu; co-op as before, below).
// REQ-CREDITSEND (the user 4 Nov): over the movie the words run off the top, the picture fades to black on its last frame
// (the movie plays on to it if the roll ended first), and only then the main menu.
// REQ-CREDITORDER (the user 5 Nov): the VR credits, then Night Dive's, then the original's: the remaster shows Night Dive's
// before LucasArts'.  Night Dive's are read at runtime from the player's Common.kpf (credits/credits_nds.txt, the same layout),
// never shipped, and left out as credits.txt is when they are not there.  "Thanks for playing" is gone from the end of the VR
// part, as the credits carry on after it.  -OLOldCreditOrder: ours then the original's, with that line.
// REQ-CREDITBLOCKS (the user 6 Nov): every block in the remaster's Common.kpf is on the roll, in the remaster's own order (its
// exe's list, CreditsBlocks below), all read at runtime and none shipped; a block the player lacks is left out with no gap.
// credits.txt is those blocks run together (the original's, the silly credits, the legal lines, The End), so the blocks replace
// it rather than follow it, which would show them twice; with no credits_og.txt credits.txt stands in, the end of it included.
// -OLOldCreditBlocks: the VR credits, Night Dive's, credits.txt.
// (the planner, 7 Nov) credits.msc's tracks end about 328 s before this roll does, so they go round again (StartCreditsMusic);
// ol.CreditsMusicLoop 0 or -OLOldCreditBlocks: once through and quiet, as before.
//
// Co-op: each machine rolls its own credits on its own screen after its own copy of the movie; nothing new goes over the
// network.  A skip is the movies' skip (either player's skip ends it for both, UOutlawsCoop::NoteMovieSkip), and when the
// host's roll ends the host ends the session as it did when its movie ended ("the game is won"), both to their menus.
//
// Tests: -OLLevel=ranch -OLNoFrontEnd -OLCreditsTest[=roll|noorig|natural|black] [-OLNoOrigCredits] [-OLCreditsShots]
// (black: the roll with no movie, BUG-CREDITSBG: its backdrop, its length, the widest line and canvas shots at 1920 and 1280 wide)
// -OLLevel=ranch -OLNoFrontEnd -OLCreditOrderProbe [-OLOldCreditOrder] [-OLOldCreditsEnd] [-OLNoNightDiveCredits]: the parts, line
// for line against their files, the joins' canvas shots, and the start, the hold and the fade over the longer roll
// -OLCreditBlocksProbe [-OLOldCreditBlocks] [-OLNoCreditBlock=name+name]: the same with every block (REQ-CREDITBLOCKS): each join
// shot, each block once, every letter inked, the last line and the fade shot; name is a block's in CreditsBlocks
#include "OutlawsLevelActor.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsPlayer.h"
#include "OutlawsEnemy.h"
#include "OutlawsMovieScreen.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsCoop.h"
#include "OutlawsLab.h"
#include "OutlawsFormats.h"
#include "OutlawsTextures.h"
#include "OutlawsInfRuntime.h"
#include "Engine/Texture2D.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "UnrealClient.h"

TAutoConsoleVariable<int32> CVarOLCreditsMusicLoop(TEXT("ol.CreditsMusicLoop"), 1,
	TEXT("The end credits: credits.msc goes round again until the roll is over (1), or plays once and stops (0)."));

namespace
{
	// the ride-off movie's times (rae.mp4, 128 s): REQ-CREDITSEND (the user 4 Nov, their frame at 1:19): the roll starts at 79 s,
	// James riding away with the sun going down behind his head and the sky open (the frame's sun matches rae.mp4 best at 79.0,
	// after the cut at 76 s); the picture held at 127.5 s, its last frames: the movie has no fade to black, it ends on the
	// starry night, still from 126.9 s.  Another movie on that line: the roll from its 79th second (or on the black after it,
	// if it is shorter), its picture held half a second before its end.  -OLOldCreditsEnd: from the cut at 76 s, held at 125 s
	constexpr double CreditsRollFrom = 79.0, CreditsRollFromOld = 76.0;
	constexpr double CreditsHoldFrom = 127.5, CreditsHoldFromOld = 125.0;
	constexpr int32 CreditsPartGap = 6;              // empty lines between two parts
	constexpr int32 CreditsPartOriginal = 1;
	constexpr int32 CreditsPartNightDive = 2;        // (FOutlawsCreditLine::Part: it rolls between 0 and 1)
	// REQ-CREDITBLOCKS: the remaster's blocks in its Common.kpf, in the order its exe lists them.  bOriginal: part of what
	// credits.txt holds (the localised versions' lines aside), so on the roll only when credits_og.txt stands for that file
	struct FCreditsBlock
	{
		const TCHAR* Name;
		const TCHAR* File;
		int32 Part;
		bool bOriginal;
	};
	constexpr FCreditsBlock CreditsBlocks[] =
	{
		{ TEXT("nds"), TEXT("credits/credits_nds.txt"), CreditsPartNightDive, false },
		{ TEXT("quantic"), TEXT("credits/credits_quantic.txt"), 3, false },
		{ TEXT("disney"), TEXT("credits/credits_disney.txt"), 4, false },
		{ TEXT("og"), TEXT("credits/credits_og.txt"), CreditsPartOriginal, true },
		{ TEXT("ogloc"), TEXT("credits/credits_ogloc.txt"), 5, true },
		{ TEXT("ogsilly"), TEXT("credits/credits_ogsilly.txt"), 6, true },
		{ TEXT("vst"), TEXT("credits/credits_vst.txt"), 7, true },
		{ TEXT("legal"), TEXT("credits/credits_legal.txt"), 8, true },
		{ TEXT("theend"), TEXT("credits/credits_theend.txt"), 9, true },
	};
	// -OLOldCreditOrder: the line vr_credits.txt ended on before REQ-CREDITORDER, three empty lines under the one before it
	constexpr const TCHAR* CreditsOldVrEnd = TEXT("\n\n\n~Thanks for playing\n");

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

	int32 CreditBlockOfPart(int32 Part)
	{
		for (int32 i = 0; i < UE_ARRAY_COUNT(CreditsBlocks); ++i) if (CreditsBlocks[i].Part == Part) return i;
		return INDEX_NONE;
	}

	// (tests) -OLNoCreditBlock=name+name: as if the player's Common.kpf lacked them ('+': a comma ends a flag's value)
	bool CreditBlockLeftOut(const TCHAR* Name)
	{
		FString Names;
		TArray<FString> Left;
		if (OLFlags::Value(TEXT("OLNoCreditBlock"), Names)) Names.ParseIntoArray(Left, TEXT("+"));
		return Left.Contains(Name);
	}

	// each of CreditsBlocks' texts, empty where the player has none.  mf3s.laf stops at 255, so it has no long dash and no trade
	// mark, and its copyright slot holds the font's filler block: those three as the 1997 credits.txt wrote them, or the first
	// two would drop out of the lines and the third would draw as a striped box (seen in the shot, 7 Nov)
	void ReadCreditBlocks(const FOutlawsAssets* Assets, TArray<FString>& Out)
	{
		Out.Reset();
		Out.SetNum(UE_ARRAY_COUNT(CreditsBlocks));
		OutlawsMenu::FKpf Kpf;
		if (!Assets || !Kpf.Open(FPaths::Combine(Assets->GetRoot(), TEXT("Common.kpf")))) return;
		const TCHAR LongDash[] = { TCHAR(0x2014), 0 };
		const TCHAR TradeMark[] = { TCHAR(0x2122), 0 };
		const TCHAR Copyright[] = { TCHAR(0x00A9), 0 };
		for (int32 i = 0; i < Out.Num(); ++i)
		{
			if (CreditBlockLeftOut(CreditsBlocks[i].Name) || !Kpf.ReadText(CreditsBlocks[i].File, Out[i]))
			{
				Out[i].Reset();
				continue;
			}
			Out[i].ReplaceInline(LongDash, TEXT("-"));
			Out[i].ReplaceInline(TradeMark, TEXT("(TM)"));
			Out[i].ReplaceInline(Copyright, TEXT("(c)"));
		}
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
	static const bool bOldOrder = OLFlags::Param(TEXT("OLOldCreditOrder"));
	// ours first
	FString Vr;
	int32 NVr = 0;
	if (FFileHelper::LoadFileToString(Vr, *CreditsVrFile())) NVr = AddCreditsText(Out, bOldOrder ? Vr + CreditsOldVrEnd : Vr, 0, true);
	// the gap belongs to the part it follows
	auto AddPart = [&Out](const FString& Text, int32 Part)
	{
		TArray<FOutlawsCreditLine> Lines;
		const int32 N = AddCreditsText(Lines, Text, Part, false);
		for (int32 i = 0; N > 0 && Out.Num() > 0 && i < CreditsPartGap; ++i) { FOutlawsCreditLine Gap; Gap.Part = Out.Last().Part; Out.Add(Gap); }
		Out.Append(Lines);
		return N;
	};
	// then the player's own files, in the remaster's order (REQ-CREDITBLOCKS: Common.kpf; credits.txt standing in for the original's
	// from the LAB that holds it last in the patch order: olpatch3's where it is there); missing, unreadable or empty: left out,
	// with nothing waiting in its place.  -OLNoOrigCredits: nothing of the player's
	const bool bNoOrig = OLFlags::Param(TEXT("OLNoOrigCredits"));
	const bool bNoNightDive = bOldOrder || bNoOrig || OLFlags::Param(TEXT("OLNoNightDiveCredits"));
	const bool bOldBlocks = bOldOrder || OLFlags::Param(TEXT("OLOldCreditBlocks"));
	TArray<FString> Blocks;
	ReadCreditBlocks(AssetsRef.Get(), Blocks);
	const bool bChain = !bOldBlocks && !Blocks[CreditBlockOfPart(CreditsPartOriginal)].IsEmpty();
	int32 NNightDive = 0, NOrig = 0, NMore = 0;
	for (int32 i = 0; !bNoOrig && i < Blocks.Num(); ++i)
	{
		const FCreditsBlock& B = CreditsBlocks[i];
		FString Orig;
		if (B.Part == CreditsPartNightDive) NNightDive = bNoNightDive ? 0 : AddPart(Blocks[i], B.Part);
		else if (B.Part != CreditsPartOriginal) NMore += (B.bOriginal ? bChain : !bOldBlocks) ? AddPart(Blocks[i], B.Part) : 0;
		else if (bChain) NOrig = AddPart(Blocks[i], B.Part);
		else if (AssetsRef && AssetsRef->GetText(TEXT("credits.txt"), Orig)) NOrig = AddPart(Orig, B.Part);
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT credits: Outlaws VR %d lines (%s), Night Dive %d lines, the original %d lines (%s), the remaster's other blocks %d lines"), NVr, *CreditsVrFile(), NNightDive, NOrig,
		bNoOrig ? TEXT("left out: -OLNoOrigCredits") : bChain ? TEXT("credits_og.txt from Common.kpf") : NOrig > 0 ? *FString::Printf(TEXT("credits.txt from %s"), *FPaths::GetCleanFilename(AssetsRef->Where(TEXT("credits.txt"))))
		: TEXT("none in the player's files: left out"), NMore);
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
	// (REQ-CREDITSEND) co-op keeps its ending: the roll's end closes the screen at once and the host ends the session
	static const bool bOldEnd = OLFlags::Param(TEXT("OLOldCreditsEnd"));
	Movie->ArmCredits(Lines, bFont ? &Font : nullptr, bOldEnd ? CreditsRollFromOld : CreditsRollFrom, bOldEnd ? CreditsHoldFromOld : CreditsHoldFrom,
		!bOldEnd && !UOutlawsCoop::IsActive(this));
	// the original's credits music (credits.msc) once the picture is held and the movie's own sound is over
	Movie->OnCreditsHeld.AddWeakLambda(this, [this]() { StartCreditsMusic(); });
}

bool AOutlawsLevelActor::CreditsMusicLoops() const
{
	return CVarOLCreditsMusicLoop.GetValueOnGameThread() != 0 && !OLFlags::Param(TEXT("OLOldCreditBlocks")) && !OLFlags::Param(TEXT("OLOldCreditOrder"));
}

void AOutlawsLevelActor::StartCreditsMusic()
{
	StartMusic(TEXT("credits.msc"));
	// after the last cue the file's music stops: a jump back to the cue it started on, so UpdateMusic and PeekNextMusicFile walk
	// it as they do any GOTO_CUE (no cue playing: no tracks found, the roll stays quiet as it did)
	if (!CreditsMusicLoops() || !MusicCues.IsValidIndex(MusicCue)) return;
	FMusicCmd Back;
	Back.bGoto = true;
	Back.Cue = MusicCue;
	MusicCues.Last().Add(Back);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT music credits: round again from cue %d after cue %d"), MusicCue + 1, MusicCues.Num());
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
	Movie->OnCreditsHeld.AddWeakLambda(this, [this]() { StartCreditsMusic(); });
	FadeCamera(1.f, 0.f, 1.f, false);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT credits with no movie (the end movie %s did not roll them)"), *NextMovie);
	PlayCreditsWithoutMovie(Lines, P);
	SetActorTickEnabled(true);
	return true;
}

UTexture2D* AOutlawsLevelActor::MakeHintroBackdrop() const
{
	TArray<uint8> Bytes;
	OutlawsFormats::FPcxImage Img;
	if (!AssetsRef || !AssetsRef->Get(TEXT("hintro.pcx"), Bytes) || !OutlawsFormats::DecodePcx(Bytes, Img) || Img.Palette.Num() != 256 || Img.Indices.Num() != Img.W * Img.H) return nullptr;
	TArray<FColor> Px;
	Px.SetNumUninitialized(Img.W * Img.H);
	for (int32 i = 0; i < Px.Num(); ++i) { Px[i] = Img.Palette[Img.Indices[i]]; Px[i].A = 255; }
	return OutlawsTextures::MakeTextureBGRA(Img.W, Img.H, Px, false, TF_Bilinear, TA_Clamp);
}

void AOutlawsLevelActor::PlayCreditsWithoutMovie(const TArray<FOutlawsCreditLine>& Lines, AOutlawsPlayer* P)
{
	// (BUG-CREDITSBG) the story's end with no movie and Extras > Credits: over the Marshal Training sunset, as the crawl is
	if (!Movie || !P) return;
	OutlawsFormats::FLafFont Font;
	TArray<uint8> FontBytes;
	const bool bFont = AssetsRef && AssetsRef->Get(TEXT("mf3s.laf"), FontBytes) && OutlawsFormats::DecodeLaf(FontBytes, Font);
	const UCameraComponent* Cam = P->FindComponentByClass<UCameraComponent>();
	Movie->PlayCredits(Lines, bFont ? &Font : nullptr, Cam ? Cam->GetComponentLocation() : P->GetActorLocation(), Cam ? Cam->GetForwardVector() : P->GetActorForwardVector(),
		MakeHintroBackdrop(), bHDTextures);
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
	StartCreditsMusic();
	UE_LOG(LogOutlaws, Log, TEXT("OLRT credits from Extras: %d lines, no movie"), Lines.Num());
	PlayCreditsWithoutMovie(Lines, P);
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
	static double T = 0.0, Mark = 0.0, SkipAt = 0.0, RollAt = 0.0, HeldAt = 0.0, WordsGoneAt = 0.0;
	static bool bFadeSeen = false, bFadeShot = false, bBlackShot = false;
	static TArray<int32> JoinParts;                     // (REQ-CREDITBLOCKS) the parts after the first, in the roll's order
	static int32 JoinAt = 0;
	static int32 MusicCueWas = -1, MusicRounds = 0;      // (REQ-CREDITBLOCKS) the credits' cue last seen, the times it went back
	static FString Mode;
	if (Step < 0) return;
	if (Mode.IsEmpty())
	{
		if (OLFlags::Param(TEXT("OLCreditBlocksProbe"))) Mode = TEXT("blocks");
		else if (OLFlags::Param(TEXT("OLCreditOrderProbe"))) Mode = TEXT("order");
		else if (!OLFlags::Value(TEXT("OLCreditsTest"), Mode)) Mode = TEXT("roll");
		Mode = Mode.ToLower();
	}
	T += Dt;
	if (!bFrontEnd && Movie && Movie->IsCreditsHeld() && MusicCue >= 0)
	{
		if (MusicCue < MusicCueWas)
		{
			++MusicRounds;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT credits music: back to cue %d at roll %.1f s (round %d)"), MusicCue + 1, Movie->CreditsAgeNow(), MusicRounds);
		}
		MusicCueWas = MusicCue;
	}
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
	const bool bBlack = Mode == TEXT("black");
	const bool bOrder = Mode == TEXT("order") || Mode == TEXT("blocks");
	const bool bShots = OLFlags::Param(TEXT("OLCreditsShots")) || bBlack || bOrder;
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
	const bool bOldEnd = OLFlags::Param(TEXT("OLOldCreditsEnd"));
	// (REQ-CREDITSEND) the picture held on the movie's last frames (-OLOldCreditsEnd: at 125 s)
	auto CheckHeldAt = [&]()
	{
		const double Want = bOldEnd ? CreditsHoldFromOld : CreditsHoldFrom;
		Check(FMath::Abs(Movie->GetTime() - Want) < 0.6, FString::Printf(TEXT("the picture held at %.2f s of the movie (%.1f wanted)"), double(Movie->GetTime()), Want));
	};
	// (REQ-CREDITORDER, REQ-CREDITBLOCKS) the parts as the roll has them, each against its file read again here; these lines give
	// counts and CRCs, so no words of the player's files are written anywhere
	auto CheckParts = [&]()
	{
		const bool bOldOrder = OLFlags::Param(TEXT("OLOldCreditOrder"));
		const bool bOldBlocks = bOldOrder || OLFlags::Param(TEXT("OLOldCreditBlocks"));
		const bool bNightDive = !bOldOrder && !OLFlags::Param(TEXT("OLNoNightDiveCredits"));
		TArray<FString> Blocks;
		ReadCreditBlocks(AssetsRef.Get(), Blocks);
		const bool bChain = !bOldBlocks && !Blocks[CreditBlockOfPart(CreditsPartOriginal)].IsEmpty();
		// (written out again here, so a change to the table shows) the exe's order: Night Dive, Quantic, Disney, the original,
		// the localised versions, the silly credits, Very Special Thanks, the legal lines, The End
		TArray<int32> Want = { 0 };
		for (const int32 Part : { CreditsPartNightDive, 3, 4, CreditsPartOriginal, 5, 6, 7, 8, 9 })
		{
			const int32 At = CreditBlockOfPart(Part);
			if (At == INDEX_NONE) { Check(false, FString::Printf(TEXT("part %d is in the blocks' table"), Part)); continue; }
			const bool bHere = !Blocks[At].IsEmpty();
			if (Part == CreditsPartNightDive ? bNightDive && bHere : Part == CreditsPartOriginal ? true : CreditsBlocks[At].bOriginal ? bChain && bHere : !bOldBlocks && bHere) Want.Add(Part);
		}
		TArray<FOutlawsCreditLine> Lines;
		const bool bLines = BuildEndCredits(Lines);
		// the order, and the empty lines at each join
		TArray<int32> Order;
		int32 BadGaps = 0, Prev = INDEX_NONE;
		for (int32 i = 0; i < Lines.Num(); ++i)
		{
			if (Lines[i].Text.IsEmpty()) continue;
			if (Order.Num() == 0 || Order.Last() != Lines[i].Part)
			{
				if (Order.Num() > 0 && i - Prev - 1 != CreditsPartGap) ++BadGaps;
				Order.Add(Lines[i].Part);
			}
			Prev = i;
		}
		Check(bLines && Order == Want, FString::Printf(TEXT("the parts in order: %s (0 Outlaws VR, 2 Night Dive, 3 Quantic, 4 Disney, 1 the original, 5 localised, 6 silly, 7 VST, 8 legal, 9 The End)"), *FString::JoinBy(Order, TEXT(" "), [](int32 P) { return FString::FromInt(P); })));
		Check(BadGaps == 0, FString::Printf(TEXT("%d empty lines at each of the %d joins (%d not)"), CreditsPartGap, FMath::Max(0, Order.Num() - 1), BadGaps));
		auto CheckFile = [&](int32 Part, const TCHAR* Name, const FString& Text, const FString& From)
		{
			TArray<FOutlawsCreditLine> File;
			const int32 N = AddCreditsText(File, Text, Part, false);
			const int32 At = Lines.IndexOfByPredicate([Part](const FOutlawsCreditLine& L) { return L.Part == Part; });
			int32 Same = 0;
			uint32 Crc = 0;
			for (int32 i = 0; i < N; ++i)
			{
				if (At >= 0 && Lines.IsValidIndex(At + i) && Lines[At + i].Text == File[i].Text && Lines[At + i].bHeading == File[i].bHeading) ++Same;
				Crc = FCrc::StrCrc32(*File[i].Text, Crc);
			}
			Check(N > 0 && Same == N, FString::Printf(TEXT("%s: %d of %d lines as %s has them, crc %08x"), Name, Same, N, *From, Crc));
		};
		for (int32 i = 0; i < Blocks.Num(); ++i)
		{
			const FCreditsBlock& B = CreditsBlocks[i];
			if (!Want.Contains(B.Part) || (B.Part == CreditsPartOriginal && !bChain))
			{
				if (B.Part == CreditsPartNightDive) Check(Movie->CreditsLineCount(CreditsPartNightDive) == 0, bOldOrder ? TEXT("old order (-OLOldCreditOrder): no Night Dive part") : TEXT("no Night Dive part (-OLNoNightDiveCredits): the roll closes over the space"));
				else if (B.Part != CreditsPartOriginal) Check(Movie->CreditsLineCount(B.Part) == 0, FString::Printf(TEXT("no %s block: the roll closes over the space"), B.Name));
				continue;
			}
			CheckFile(B.Part, B.Name, Blocks[i], FString::Printf(TEXT("%s in %s"), B.File, *FPaths::Combine(AssetsRef->GetRoot(), TEXT("Common.kpf"))));
		}
		FString Orig;
		if (!bChain)
		{
			if (AssetsRef->GetText(TEXT("credits.txt"), Orig)) CheckFile(CreditsPartOriginal, TEXT("the original"), Orig, AssetsRef->Where(TEXT("credits.txt")));
			else Check(false, TEXT("credits.txt read"));
		}
		// (REQ-CREDITBLOCKS) credits.txt holds these three as well: each block's longest line once on the roll, never twice
		for (int32 i = 0; i < Blocks.Num(); ++i)
		{
			const FCreditsBlock& B = CreditsBlocks[i];
			if (Blocks[i].IsEmpty() || (FCString::Strcmp(B.Name, TEXT("ogsilly")) != 0 && FCString::Strcmp(B.Name, TEXT("legal")) != 0 && FCString::Strcmp(B.Name, TEXT("theend")) != 0)) continue;
			TArray<FOutlawsCreditLine> File;
			AddCreditsText(File, Blocks[i], B.Part, false);
			FString Longest;
			for (const FOutlawsCreditLine& L : File) if (L.Text.Len() > Longest.Len()) Longest = L.Text;
			const int32 Seen = Lines.FilterByPredicate([&Longest](const FOutlawsCreditLine& L) { return L.Text == Longest; }).Num();
			Check(Seen == 1, FString::Printf(TEXT("%s's longest line on the roll %d time(s), crc %08x"), B.Name, Seen, FCrc::StrCrc32(*Longest)));
		}
		// a letter the font lacks drops out of its line, and a line of nothing else is a blank line; a slot with no letter of its
		// own holds the filler block at the font's data offset 0 (only the space sits there by right): every letter a real one
		OutlawsFormats::FLafFont Font;
		TArray<uint8> FontBytes;
		int32 Letters = 0, NoGlyph = 0;
		if (AssetsRef->Get(TEXT("mf3s.laf"), FontBytes) && OutlawsFormats::DecodeLaf(FontBytes, Font))
		{
			for (const FOutlawsCreditLine& L : Lines)
			{
				for (const TCHAR Ch : L.Text)
				{
					const OutlawsFormats::FLafGlyph* G = Font.Find(Ch);
					++Letters;
					NoGlyph += G && (Ch == TEXT(' ') || (G->W * G->H > 1 && G->DataOffset != 0)) ? 0 : 1;
				}
			}
		}
		Check(Letters > 0 && NoGlyph == 0, FString::Printf(TEXT("the roll's %d letters all real ones in mf3s.laf (%d not)"), Letters, NoGlyph));
		const int32 LastAt = Lines.FindLastByPredicate([](const FOutlawsCreditLine& L) { return !L.Text.IsEmpty(); });
		const bool bNoEnd = bChain && CreditBlockLeftOut(TEXT("theend"));
		Check(Lines.IsValidIndex(LastAt) && (bNoEnd || Lines[LastAt].bHeading), FString::Printf(TEXT("the roll's last line is part %d's: The End, or credits.txt's, or with The End left out the block before it"), Lines.IsValidIndex(LastAt) ? Lines[LastAt].Part : -1));
		const int32 Thanks = Lines.IndexOfByPredicate([](const FOutlawsCreditLine& L) { return L.Text.Contains(TEXT("Thanks for playing")); });
		if (bOldOrder) Check(Lines.IsValidIndex(Thanks) && Lines[Thanks].bHeading && Lines[Thanks].Part == 0 && Thanks + 1 + CreditsPartGap == Movie->CreditsFirstLine(1), TEXT("old order: \"Thanks for playing\" ends the VR part"));
		else Check(Thanks == INDEX_NONE, TEXT("no \"Thanks for playing\" anywhere on the roll"));
		const double Frac = Movie->WidestLineFrac(nullptr);
		Check(Frac > 0.0 && Frac <= 0.9, FString::Printf(TEXT("the widest line's ink %.1f%% of the canvas's width, inside a 5%% margin either side"), Frac * 100.0));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT credits order: %d lines, Outlaws VR %d, Night Dive %d, the original %d, the rest %d, the roll %.0f s"), Lines.Num(), Movie->CreditsLineCount(0), Movie->CreditsLineCount(CreditsPartNightDive), Movie->CreditsLineCount(1),
			Lines.Num() - Movie->CreditsLineCount(0) - Movie->CreditsLineCount(CreditsPartNightDive) - Movie->CreditsLineCount(1), Movie->CreditsSeconds());
	};
	auto PartName = [](int32 Part)
	{
		const int32 At = CreditBlockOfPart(Part);
		return FString(At != INDEX_NONE ? CreditsBlocks[At].Name : TEXT("vr"));
	};
	// (REQ-CREDITORDER) the middle of the gap ahead of a part at the picture's middle, the lines either side of it on show
	auto SeekJoin = [&](int32 Part) { Movie->SeekCredits(Movie->CreditsFirstLine(Part) - CreditsPartGap / 2, 0.5f); };
	switch (Step)
	{
	case 0:
	{
		if (!P || bNeedPlace || bFrontEnd || !Inf || bRebuildPending || T < 4.0) return;
		bInvincible = true;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT credits test (%s) in %s: the level is up"), *Mode, *LevelName);
		if (bBlack)
		{
			// (BUG-CREDITSBG, REQ-TEXTSIZE) the roll with no movie, as Extras > Credits and the story's end without one roll it
			TArray<FOutlawsCreditLine> Lines;
			const bool bLines = BuildEndCredits(Lines);
			if (Movie) { Movie->OnFinished.RemoveAll(this); Movie->Destroy(); Movie = nullptr; }
			FActorSpawnParameters SP;
			SP.ObjectFlags |= RF_Transient;
			Movie = bLines ? World->SpawnActor<AOutlawsMovieScreen>(AOutlawsMovieScreen::StaticClass(), FTransform::Identity, SP) : nullptr;
			if (Movie) PlayCreditsWithoutMovie(Lines, P);
			Check(Movie && Movie->IsCreditsRolling(), FString::Printf(TEXT("the credits roll with no movie (%d lines)"), Lines.Num()));
			if (!Movie || !Movie->IsCreditsRolling()) { Done(); return; }
			Mark = T;
			Step = 50;
			return;
		}
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
		Check(true, FString::Printf(TEXT("the end movie %s plays with the credits armed: Outlaws VR %d lines, Night Dive %d, the original %d"), *NextMovie, Movie->CreditsLineCount(0), Movie->CreditsLineCount(CreditsPartNightDive), Movie->CreditsLineCount(1)));
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
		{
			const double Want = bOldEnd ? CreditsRollFromOld : CreditsRollFrom;
			Check(FMath::Abs(Movie->GetTime() - Want) < 0.6, FString::Printf(TEXT("it starts at %.2f s of the movie (%.1f wanted)"), double(Movie->GetTime()), Want));
		}
		Mark = T;
		Step = 12;
		return;
	case 12:
		if (T - Mark < 1.5) return;
		Shot(TEXT("1_start"));
		if (bOrder) CheckParts();
		Mark = T;
		Step = bNatural ? 40 : bOrder ? 60 : 20;
		return;
	case 20:
		if (T - Mark < 0.5) return;                // (the view's shot is taken at the end of its frame: no seek in that one)
		Mark = T;
		if (bNoOrig)
		{
			// the last of our lines just under the top: with nothing after it the roll ends as it goes, ahead of the movie
			// (REQ-CREDITSEND: which plays on to its end; -OLOldCreditsEnd: the menu is up at once)
			Movie->SeekCredits(Movie->CreditsLineCount(0) - 1, 0.1f);
			SkipAt = T;
			Step = Movie->CreditsFadeArmed() ? 31 : 30;
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
		CheckHeldAt();
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
		Movie->SeekCredits(Movie->CreditsFirstLine(1) + Movie->CreditsLineCount(1) / 2, 0.1f);
		Mark = T;
		Step = 18;
		return;
	case 18:
		if (T - Mark < 0.6) return;
		Movie->SeekCredits(Movie->CreditsFirstLine(1) + Movie->CreditsLineCount(1) / 2, 0.1f);
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
	case 31:
		// (REQ-CREDITSEND) the roll ended first: the movie goes on with no words to its last frame, the fade, the menu
		if (!Movie || bFrontEnd) { Check(false, TEXT("the movie plays on after the roll's end")); Done(); return; }
		if (Movie->CreditsAgeNow() < Movie->CreditsSeconds()) return;
		Check(Movie->IsPlaying() && !Movie->IsCreditsHeld(), FString::Printf(TEXT("the roll over at %.1f s of the movie, which plays on (Finish did not cut it)"), double(Movie->GetTime())));
		HeldAt = 0.0;
		Mark = T;
		Step = 32;
		return;
	case 32:
		if (!Movie || bFrontEnd) { Check(false, TEXT("the picture held at its last frame after the roll's end")); Done(); return; }
		if (!Movie->IsCreditsHeld())
		{
			if (T - Mark > 70.0) { Check(false, TEXT("the picture held at its last frame after the roll's end")); Done(); }
			return;
		}
		HeldAt = T;
		CheckHeldAt();
		Shot(TEXT("2_held"));
		Step = 43;
		return;
	case 40:
		// natural: no seeks, no skip: held at its point, then rolled to the end, then the main menu
		if (!bFrontEnd && Movie && Movie->IsCreditsHeld() && HeldAt == 0.0)
		{
			HeldAt = T;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT credits natural: the picture held %.1f s after the roll began"), T - RollAt);
			CheckHeldAt();
			Mark = T;
			Step = 41;
			return;
		}
		if (bFrontEnd) Step = 42;
		return;
	case 41:
		if (T - Mark < 2.0) return;
		Shot(TEXT("2_held"));
		Step = bOldEnd ? 42 : 43;
		return;
	case 43:
		// (REQ-CREDITSEND) the words gone, the held picture going to black, then black, then the menu
		if (bFrontEnd || !Movie) { Step = 42; return; }
		if (Movie->CreditsFadeNow() < 0.45f) return;
		bFadeSeen = true;
		Check(Movie->CreditsAgeNow() >= Movie->CreditsSeconds(), FString::Printf(TEXT("the fade starts after the last line: roll %.1f of %.1f s"), Movie->CreditsAgeNow(), Movie->CreditsSeconds()));
		Shot(TEXT("3_fade"));
		Step = 44;
		return;
	case 44:
		if (bFrontEnd || !Movie) { Step = 42; return; }
		if (Movie->CreditsFadeNow() < 0.93f) return;
		Shot(TEXT("4_black"));
		Step = 42;
		return;
	case 42:
		if (!bFrontEnd) return;
		Check(HeldAt > 0.0, TEXT("the picture was held at its point"));
		Check(bOldEnd ? !bFadeSeen : bFadeSeen, bOldEnd ? TEXT("old ending (-OLOldCreditsEnd): no fade, the roll's end closes the screen") : TEXT("the held picture faded to black before the menu"));
		Check(true, FString::Printf(TEXT("the roll ran to its end (%.0f s from its start) and the main menu followed"), T - RollAt));
		Done();
		return;
	// black: the backdrop, the length, a line well into the original's part at 1920 x 1080 and at the Quest's 1280 x 720
	case 50:
	{
		if (!Movie || T - Mark < 1.5) return;
		const bool bOldBg = OLFlags::Param(TEXT("OLOldCreditsBg")), bOldSize = OLFlags::Param(TEXT("OLOldTextSize"));
		const UTexture* Back = Movie->CrawlBackdrop();
		const int32 Bw = Back ? int32(Back->GetSurfaceWidth()) : 0, Bh = Back ? int32(Back->GetSurfaceHeight()) : 0;
		if (bOldBg) Check(!Back, TEXT("old rule (-OLOldCreditsBg): the roll on the black"));
		else Check(Back && ((Bw == 640 && Bh == 480) || (Bw == 6912 && Bh == 2160)), FString::Printf(TEXT("the roll over the crawl's picture %s (%dx%d; hintro.pcx 640x480 or the wide 6912x2160)"),
			Back ? *Back->GetName() : TEXT("none"), Bw, Bh));
		// the length as it was at 0.6 and 66 px a second (mf3s.laf's 71 px line): within 8 s
		const int32 N = Movie->CreditsLineTotal();
		const double Was = (1080.0 + N * 71.0 * 0.6) / 66.0;
		Check(FMath::Abs(Movie->CreditsSeconds() - Was) < 8.0, FString::Printf(TEXT("%s: the roll %.0f s for %d lines (%.0f s at the old size)"), bOldSize ? TEXT("old size (-OLOldTextSize)") : TEXT("1.5x the letters"),
			Movie->CreditsSeconds(), N, Was));
		Movie->SeekCredits(Movie->CreditsFirstLine(1) + Movie->CreditsLineCount(1) / 3, 0.5f);
		Mark = T;
		Step = 51;
		return;
	}
	case 51:
	case 53:
	{
		if (T - Mark < 0.6) return;
		const bool b720 = Step == 53;
		Movie->SeekCredits(Movie->CreditsFirstLine(1) + Movie->CreditsLineCount(1) / 3, 0.5f);
		FString Widest;
		const double Frac = Movie->WidestLineFrac(&Widest);
		Check(Frac > 0.0 && Frac <= 0.9, FString::Printf(TEXT("%s: the widest line's ink %.1f%% of the canvas's width, inside a 5%% margin either side (\"%s\")"),
			b720 ? TEXT("1280x720") : TEXT("1920x1080"), Frac * 100.0, *Widest));
		Shot(b720 ? TEXT("720") : TEXT("1080"));
		Mark = T;
		Step = b720 ? 54 : 52;
		return;
	}
	case 52:
		if (T - Mark < 0.6) return;
		Check(Movie->SetCanvasSizeForTest(1280, 720), TEXT("the canvas at the Quest's 1280x720"));
		Mark = T;
		Step = 53;
		return;
	case 54:
		if (T - Mark < 1.0) return;
		Check(!Movie->CreditsFadeArmed(), TEXT("no end fade with no movie (the sunset path, Extras > Credits): the roll's end closes the screen as it did"));
		Done();
		return;
	// order (-OLCreditOrderProbe, REQ-CREDITORDER): held as ever, the joins seen, then the last line near the top and the ending
	// as the words go: the seeks wait for the hold (a far seek while the movie plays stalls its decoder, step 13)
	case 60:
		if (!Movie->IsCreditsHeld())
		{
			if (T - Mark > 70.0) { Check(false, TEXT("the picture held near the movie's end")); Done(); }
			return;
		}
		CheckHeldAt();
		Mark = T;
		Step = 61;
		return;
	case 61:
		if (T - Mark < 2.0) return;
		// (REQ-CREDITBLOCKS) every join the roll has
		JoinParts.Reset();
		for (const FCreditsBlock& B : CreditsBlocks) if (Movie->CreditsFirstLine(B.Part) != INDEX_NONE) JoinParts.Add(B.Part);
		JoinParts.Sort([this](int32 A, int32 B) { return Movie->CreditsFirstLine(A) < Movie->CreditsFirstLine(B); });
		JoinAt = 0;
		Mark = T;
		Step = JoinParts.Num() > 0 ? 62 : 65;
		return;
	case 62:
		if (T - Mark < 0.6) return;
		SeekJoin(JoinParts[JoinAt]);
		Mark = T;
		Step = 63;
		return;
	case 63:
		if (T - Mark < 0.6) return;
		SeekJoin(JoinParts[JoinAt]);
		Shot(*FString::Printf(TEXT("2_join_%s_%s"), *PartName(JoinAt > 0 ? JoinParts[JoinAt - 1] : 0), *PartName(JoinParts[JoinAt])));
		++JoinAt;
		Mark = T;
		Step = JoinAt < JoinParts.Num() ? 62 : 65;
		return;
	case 65:
		if (T - Mark < 0.6) return;
		// (a block forced missing leaves fewer joins: the credits music's walk seen before the end, the check at case 67)
		if (!MusicAudio.IsValid() && CreditsMusicLoops() && MusicCue >= 0 && MusicRounds == 0 && T - Mark < 25.0) return;
		Movie->SeekCredits(Movie->CreditsLineTotal() - 1, 0.1f);
		Mark = T;
		Step = 64;
		return;
	case 64:
		if (T - Mark < 0.6) return;
		Movie->SeekCredits(Movie->CreditsLineTotal() - 1, 0.1f);
		Shot(TEXT("3_last_line"));
		Step = 66;
		return;
	case 66:
		if (!bFrontEnd && Movie && Movie->CreditsAgeNow() < Movie->CreditsSeconds()) return;
		WordsGoneAt = T;
		Step = 67;
		return;
	case 67:
		if (!bFrontEnd)
		{
			if (Movie && Movie->CreditsFadeNow() > 0.f) bFadeSeen = true;
			if (Movie && !bFadeShot && Movie->CreditsFadeNow() >= 0.45f) { bFadeShot = true; Shot(TEXT("4_fade")); }
			if (Movie && !bBlackShot && Movie->CreditsFadeNow() >= 0.93f) { bBlackShot = true; Shot(TEXT("5_black")); }
			if (T - WordsGoneAt > 15.0) { Check(false, TEXT("back to the main menu after the last line")); Done(); }
			return;
		}
		Check(bOldEnd ? !bFadeSeen : bFadeSeen, bOldEnd ? TEXT("old ending (-OLOldCreditsEnd): no fade") : TEXT("the held picture faded to black before the menu"));
		Check(bOldEnd ? T - WordsGoneAt < 1.5 : FMath::Abs(T - WordsGoneAt - 3.0) < 0.75, FString::Printf(TEXT("the main menu up %.2f s after the last line (the 3 s fade; old ending: at once)"), T - WordsGoneAt));
		// the tracks can't sound in these runs (-nosound: a cue that can't start gives way after 5 s, so every cue is over well
		// inside this run): the walk past the last cue is what's measured.  A run with sound keeps its first cue here.
		if (!MusicAudio.IsValid()) Check(CreditsMusicLoops() ? MusicRounds > 0 : MusicRounds == 0, FString::Printf(TEXT("the credits music %s: back to its first cue %d time(s)"),
			CreditsMusicLoops() ? TEXT("goes round") : TEXT("plays once (ol.CreditsMusicLoop 0 or an old roll)"), MusicRounds));
		Done();
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
