#include "OutlawsMovieScreen.h"
#include "OutlawsFlags.h"
#include "OutlawsPerf.h"
#include "OutlawsRuntime.h"
#include "MediaPlayer.h"
#include "MediaTexture.h"
#include "FileMediaSource.h"
#include "MediaSoundComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MaterialShared.h"
#include "Kismet/GameplayStatics.h"
#include "Camera/PlayerCameraManager.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/App.h"
#include "UObject/ConstructorHelpers.h"
#include "OutlawsTextures.h"
#include "OutlawsPlayer.h"
#include "Engine/Canvas.h"
#include "Engine/CanvasRenderTarget2D.h"
#include "Engine/Font.h"
#include "Engine/Engine.h"
#include "CanvasItem.h"
#include "ImageUtils.h"
#include "PixelFormat.h"
#include "OutlawsMenuDoc.h"
#include "HAL/FileManager.h"
#include "OutlawsFormats.h"
#include "Engine/Texture2D.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Camera/CameraComponent.h"

namespace
{
	constexpr float ScreenW = 320.f;     // 16:9, as the movies are (2560x1440)
	constexpr float ScreenH = 180.f;
	constexpr float BarW = 120.f, BarH = 3.2f, BarZ = ScreenH * 0.5f + 5.f;     // the skip bar, over the picture
	// the counts' panels (user 2026-10-02): 2:3, most of the picture's height, their canvases 512 x 768
	constexpr float StatsW = 76.f, StatsH = 114.f;
	// (BUG-SCORESHEETSIZE, user 15 Oct: "way too small to read") the sheets on the black 1.5x (114 x 171 cm at 3 m: the top
	// 16 degrees over the eye, inside the menus' 20), the gap between them 1.5x too.  The canvases stay 512 x 768.  (The
	// black ES3_1 preview shots were the screen material's shaders still compiling: SheetShadersPending)
	constexpr int32 StatsPxW = 512, StatsPxH = 768;
	constexpr float StatsBaseW = 512.f;        // DrawStatsPanel lays the sheet out in the old 512-wide units
	constexpr float StatsHoldScale = 1.5f;
	constexpr float StatsBesideDeg = 37.f;     // (no longer used beside a picture: the row over it, PlaceStats)
	constexpr float StatsHoldDeg = 14.3f;      // with no picture: the two either side of the middle (was 9.5 at 1x)
}

AOutlawsMovieScreen::AOutlawsMovieScreen()
{
	PrimaryActorTick.bCanEverTick = true;
	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
	Screen = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Screen"));
	Screen->SetupAttachment(Root);
	Screen->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Screen->SetCastShadow(false);
	// the engine plane lies in XY facing +Z (100 cm square), the picture's U along its X and V along its Y:
	// X to the viewer's right (+Y), Y down (-Z), so its face (+Z) turns to the viewer at the root's back (-X)
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Plane(TEXT("/Engine/BasicShapes/Plane.Plane"));
	if (Plane.Succeeded()) Screen->SetStaticMesh(Plane.Object);
	Screen->SetRelativeRotation(FRotationMatrix::MakeFromXY(FVector(0.0, 1.0, 0.0), FVector(0.0, 0.0, -1.0)).Rotator());
	Screen->SetRelativeScale3D(FVector(ScreenW / 100.f, ScreenH / 100.f, 1.f));
	Sound = CreateDefaultSubobject<UMediaSoundComponent>(TEXT("Sound"));
	Sound->SetupAttachment(Root);
	Sound->bAllowSpatialization = false;
	Subtitle = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Subtitle"));
	Subtitle->SetupAttachment(Root);
	Subtitle->SetHorizontalAlignment(EHTA_Center);
	Subtitle->SetVerticalAlignment(EVRTA_TextTop);
	Subtitle->SetWorldSize(9.f);
	Subtitle->SetTextRenderColor(FColor(245, 230, 190));
	Subtitle->SetRelativeLocationAndRotation(FVector(-2.0, 0.0, -ScreenH * 0.5f - 8.f), FRotator(0.0, 180.0, 0.0));
	Subtitle->SetText(FText::GetEmpty());
	Subtitle->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Prompt = CreateDefaultSubobject<UTextRenderComponent>(TEXT("SkipPrompt"));
	Prompt->SetupAttachment(Root);
	Prompt->SetHorizontalAlignment(EHTA_Center);
	Prompt->SetVerticalAlignment(EVRTA_TextBottom);
	Prompt->SetWorldSize(8.f);
	Prompt->SetTextRenderColor(FColor(255, 225, 140));
	Prompt->SetRelativeLocationAndRotation(FVector(-2.0, 0.0, BarZ + BarH + 2.f), FRotator(0.0, 180.0, 0.0));
	Prompt->SetText(FText::GetEmpty());
	Prompt->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	for (TObjectPtr<UStaticMeshComponent>* Piece : { &BarFrame, &BarTrack, &BarFill, &BarEdge })
	{
		const FName PieceName = Piece == &BarFrame ? TEXT("SkipBarFrame") : Piece == &BarTrack ? TEXT("SkipBarTrack") : Piece == &BarFill ? TEXT("SkipBarFill") : TEXT("SkipBarEdge");
		*Piece = CreateDefaultSubobject<UStaticMeshComponent>(PieceName);
		(*Piece)->SetupAttachment(Root);
		(*Piece)->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		(*Piece)->SetCastShadow(false);
		if (Plane.Succeeded()) (*Piece)->SetStaticMesh(Plane.Object);
		(*Piece)->SetVisibility(false);
	}
}

void AOutlawsMovieScreen::MakeBarLooks()
{
	// one flat colour each, in the world material (as the picture is): a 1 x 1 texture apiece
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_World.M_OLR_World"));
	if (!Base) return;
	const TPair<UStaticMeshComponent*, FColor> Looks[] = { { BarFrame, FColor(150, 118, 60) }, { BarTrack, FColor(22, 18, 14) },
		{ BarFill, FColor(226, 176, 78) }, { BarEdge, FColor(255, 240, 200) } };
	for (const TPair<UStaticMeshComponent*, FColor>& L : Looks)
	{
		if (!L.Key) continue;
		UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(Base, this);
		if (UTexture2D* T = OutlawsTextures::MakeTextureBGRA(1, 1, TArray<FColor>{ L.Value }, false, TF_Nearest, TA_Clamp)) M->SetTextureParameterValue(TEXT("Tex"), T);
		L.Key->SetMaterial(0, M);
	}
}

void AOutlawsMovieScreen::PlaceBarPiece(UStaticMeshComponent* C, float Left, float Width, float Height, float Depth)
{
	// laid as the picture is (the plane's X to the viewer's right, its Y down), Left cm from the bar's left end
	C->SetRelativeLocationAndRotation(FVector(-Depth, -BarW * 0.5f + Left + Width * 0.5f, BarZ + BarH * 0.5f),
		FRotationMatrix::MakeFromXY(FVector(0.0, 1.0, 0.0), FVector(0.0, 0.0, -1.0)).Rotator());
	C->SetRelativeScale3D(FVector(FMath::Max(0.01f, Width) / 100.f, Height / 100.f, 1.f));
}

void AOutlawsMovieScreen::SetSkipHold(float Held, float Need, int32 Buttons)
{
	// nothing while nothing is held; then the words over the bar, which fills smoothly
	const float F = FMath::Clamp(Held / FMath::Max(0.1f, Need), 0.f, 1.f);
	const bool bShow = Held > 0.f;
	for (UStaticMeshComponent* C : { BarFrame.Get(), BarTrack.Get(), BarFill.Get(), BarEdge.Get() })
	{
		if (C && C->IsVisible() != bShow) C->SetVisibility(bShow);
	}
	const int32 Label = bShow ? FMath::Clamp(Buttons, 1, 9) : 0;
	if (Label != ShownHoldSteps)
	{
		ShownHoldSteps = Label;
		Prompt->SetText(Label == 0 ? FText::GetEmpty()
			: FText::FromString(Label > 1 ? FString::Printf(TEXT("Hold to skip  x%d"), Label) : FString(TEXT("Hold to skip"))));
	}
	if (!bShow) return;
	PlaceBarPiece(BarFrame, -0.6f, BarW + 1.2f, BarH + 1.2f, 1.6f);
	PlaceBarPiece(BarTrack, 0.f, BarW, BarH, 1.8f);
	PlaceBarPiece(BarFill, 0.f, BarW * F, BarH, 2.0f);
	PlaceBarPiece(BarEdge, FMath::Max(0.f, BarW * F - 0.8f), 0.8f, BarH, 2.1f);
}

bool AOutlawsMovieScreen::Play(const FString& File, const FVector& Eye, const FVector& Forward, float Distance)
{
	// world-locked in front of the viewer, facing them, at eye height
	FVector Fwd = Forward;
	Fwd.Z = 0.0;
	Fwd = Fwd.GetSafeNormal();
	if (Fwd.IsNearlyZero()) Fwd = FVector::ForwardVector;
	ScreenDistance = Distance;
	ScreenYaw = Fwd.Rotation().Yaw;
	ScreenPitch = 0.f;                       // level, at eye height: a cutscene screen follows the head's turn only (user 2026-09-29)
	const FVector Ahead = FRotator(ScreenPitch, ScreenYaw, 0.0).Vector();
	SetActorLocationAndRotation(Eye + Ahead * Distance, Ahead.Rotation());
	Screen->SetVisibility(true);
	// the screen's own material where it has been made (M_OLR_MovieScreen: the picture, toned by "Tone"), else the world's
	UMaterialInterface* ScreenBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_MovieScreen.M_OLR_MovieScreen"));
	if (!ScreenBase) ScreenBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_World.M_OLR_World"));
	if (UMaterialInterface* Base = ScreenBase)
	{
		Mid = UMaterialInstanceDynamic::Create(Base, this);
		Screen->SetMaterial(0, Mid);
	}
	if (UMaterialInterface* TextMat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_Text.M_OLR_Text")))
	{
		Subtitle->SetTextMaterial(TextMat);
		Prompt->SetTextMaterial(TextMat);
	}
	MakeBarLooks();
	Age = 0.f;
	ShownHoldSteps = -1;
	Prompt->SetText(FText::GetEmpty());
	if (!FApp::CanEverRender())
	{
		// nothing renders (a -nullrhi test): the media framework asserts without a video sink
		UE_LOG(LogOutlaws, Log, TEXT("OLRT movie %s skipped: no renderer"), *File);
		Finish();
		return false;
	}
	if (!FPaths::FileExists(File))
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT movie %s missing: skipped"), *File);
		Finish();
		return false;
	}
	LoadSubtitles(FPaths::ChangeExtension(File, TEXT("srt")));
	Player = NewObject<UMediaPlayer>(this);
	Player->PlayOnOpen = true;
	Player->SetLooping(false);
	Player->OnEndReached.AddDynamic(this, &AOutlawsMovieScreen::HandleEnd);
	Player->OnMediaOpenFailed.AddDynamic(this, &AOutlawsMovieScreen::HandleOpenFailed);
	Texture = NewObject<UMediaTexture>(this);
	Texture->NewStyleOutput = true;       // a plain texture, so the world material's sampler takes it
	Texture->SetMediaPlayer(Player);
	Texture->UpdateResource();
	if (Mid) Mid->SetTextureParameterValue(TEXT("Tex"), Texture);
	Sound->SetMediaPlayer(Player);
	Source = NewObject<UFileMediaSource>(this);
	Source->SetFilePath(File);
	if (!Player->OpenSource(Source))
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT movie %s will not open: skipped"), *File);
		Finish();
		return false;
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT movie %s playing (%d subtitle cues)"), *File, Cues.Num());
	return true;
}

void AOutlawsMovieScreen::SetVolume(float Volume)
{
	if (Sound) Sound->SetVolumeMultiplier(Volume);
}

float AOutlawsMovieScreen::GetTime() const
{
	return Player ? static_cast<float>(Player->GetTime().GetTotalSeconds()) : 0.f;
}

void AOutlawsMovieScreen::SetPaused(bool bPause)
{
	if (bPause == bPaused || bFinished || (!Player && !bHold && !bCrawl && !bCredits)) return;
	bPaused = bPause;
	if (Player)
	{
		if (bPause) Player->Pause();
		else if (!bCreditsHeld) Player->Play();      // (REQ-CREDITS: a held picture stays held)
	}
	if (Subtitle) Subtitle->SetVisibility(!bPause);      // (its line under the menu read through it)
	UE_LOG(LogOutlaws, Log, TEXT("OLRT movie %s at %.1f s"), bPause ? TEXT("paused") : TEXT("resumed"), GetTime());
}

void AOutlawsMovieScreen::SetTone(float Tone)
{
	if (Mid) Mid->SetScalarParameterValue(TEXT("Tone"), Tone);
	for (UMaterialInstanceDynamic* M : StatsMids) if (M) M->SetScalarParameterValue(TEXT("Tone"), Tone);
}

void AOutlawsMovieScreen::Skip()
{
	UE_LOG(LogOutlaws, Log, TEXT("OLRT movie skipped at %.1f s"), GetTime());
	if (bCredits) bCreditsSkipped = true;          // (REQ-CREDITS: the roll with it)
	Finish();
}

void AOutlawsMovieScreen::Tick(float DeltaSeconds)
{
	OL_PERF_SCOPE("Movie screen");
	Super::Tick(DeltaSeconds);
	if (bFinished || (!Player && !bHold && !bCrawl && !bCredits))
	{
		return;
	}
	if (bPaused) return;                // (held behind the pause menu: no age, no turning after the head)
	Age += DeltaSeconds;
	if (bStats)
	{
		// (co-op: the other machine's last counts may still be on their way: looked at twice a second)
		StatsCheck -= DeltaSeconds;
		if (StatsCheck <= 0.f)
		{
			StatsCheck = 0.5f;
			RefreshStats();
		}
	}
	if (bHandCard) PlaceHandCard();
	if (Player && Player->IsPlaying()) bStarted = true;
	// the screen follows the viewer round, as the pause menu and the wheel do: once the head has turned
	// well away (25 degrees), easing after it; at eye height, facing them (user 2026-09-27)
	if (const APlayerCameraManager* Cam = UGameplayStatics::GetPlayerCameraManager(this, 0))
	{
		const FVector Eye = Cam->GetCameraLocation();
		const float HeadYaw = Cam->GetCameraRotation().Yaw;
		// left and right only, on the pause menu's damped spring (user 2026-10-02: it stopped dead at the edge and set off
		// in jerks): once 25 degrees off it sets off, eases all the way back to the middle and settles there
		const float Delta = FRotator::NormalizeAxis(HeadYaw - ScreenYaw);
		const float Step = FMath::Min(float(FApp::GetDeltaTime()), 0.05f);
		if (FMath::Abs(Delta) > 25.f) bScreenYawFollow = true;            // (the cards sit over the picture now: no wider turn)
		if (bScreenYawFollow && FMath::Abs(Delta) < 1.5f && FMath::Abs(ScreenYawVel) < 6.f) bScreenYawFollow = false;
		constexpr float Spring = 4.5f;                               // (critically damped: no overshoot)
		ScreenYawVel += ((bScreenYawFollow ? Spring * Spring * Delta : 0.f) - 2.f * Spring * ScreenYawVel) * Step;
		ScreenYaw = FRotator::NormalizeAxis(ScreenYaw + ScreenYawVel * Step);
		const FVector Ahead = FRotator(ScreenPitch, ScreenYaw, 0.0).Vector();
		SetActorLocationAndRotation(Eye + Ahead * ScreenDistance, Ahead.Rotation());
	}
	if (bCredits && TickCredits(DeltaSeconds)) return;      // (REQ-CREDITS: the roll has the frame)
	if (bCrawl)
	{
		if (CrawlCanvas) CrawlCanvas->UpdateResource();
		if (Age >= CrawlSeconds)
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLRT crawl over (%.0f s)"), CrawlSeconds);
			Finish();
		}
		return;
	}
	if (bHold)
	{
		HoldLeft -= DeltaSeconds;
		if (HoldLeft <= 0.f)
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLRT the counts' hold is over"));
			Finish();
		}
		return;
	}
	// the cue on show
	const double T = GetTime();
	int32 Cue = -1;
	for (int32 i = 0; i < Cues.Num(); ++i)
	{
		if (T >= Cues[i].Start && T < Cues[i].End) { Cue = i; break; }
	}
	if (Cue != ShownCue)
	{
		ShownCue = Cue;
		Subtitle->SetText(FText::FromString(Cue >= 0 ? Cues[Cue].Text : FString()));
	}
	// a player that never gets going (no decoder) must not hold the game
	if (!bStarted && Age > 8.f)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT movie never started: skipped"));
		Finish();
	}
	// ... nor one whose end event never comes (seen 2026-10-06 on the PC: the picture stood at 60.6 s of 60.6 for good,
	// no OnEndReached): half a second at the same time at its end, 10 s anywhere else (a starved decoder), or twice its
	// length and 15 s more, and it is over
	if (bStarted && !bFinished)
	{
		StallFor = FMath::IsNearlyEqual(T, LastTime, 0.001) ? StallFor + DeltaSeconds : 0.f;
		LastTime = T;
		const double Length = Player ? Player->GetDuration().GetTotalSeconds() : 0.0;
		const bool bAtEnd = Length > 1.0 && T >= Length - 0.25;
		if (StallFor > (bAtEnd ? 0.5f : 10.f) || (Length > 1.0 && Age > Length * 2.0 + 15.0))
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLRT movie stalled at %.1f s of %.1f (%.0f s on): ended"), T, Length, Age);
			Finish();
		}
	}
}

void AOutlawsMovieScreen::LoadSubtitles(const FString& SrtFile)
{
	// SRT: number, "00:00:01,000 --> 00:00:04,000", text lines, blank
	Cues.Reset();
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *SrtFile))
	{
		return;
	}
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, false);
	auto Seconds = [](const FString& S) -> double
	{
		int32 H = 0, M = 0, Sec = 0, Ms = 0;
		FString Clean = S.TrimStartAndEnd().Replace(TEXT(","), TEXT(":")).Replace(TEXT("."), TEXT(":"));
		TArray<FString> P;
		Clean.ParseIntoArray(P, TEXT(":"));
		if (P.Num() >= 4) { H = FCString::Atoi(*P[0]); M = FCString::Atoi(*P[1]); Sec = FCString::Atoi(*P[2]); Ms = FCString::Atoi(*P[3]); }
		return H * 3600.0 + M * 60.0 + Sec + Ms / 1000.0;
	};
	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		FString L, R;
		if (!Lines[i].Split(TEXT("-->"), &L, &R)) continue;
		FCue C;
		C.Start = Seconds(L);
		C.End = Seconds(R);
		for (int32 j = i + 1; j < Lines.Num() && !Lines[j].TrimStartAndEnd().IsEmpty(); ++j)
		{
			if (!C.Text.IsEmpty()) C.Text += TEXT("\n");
			C.Text += Lines[j].TrimStartAndEnd();
			i = j;
		}
		Cues.Add(C);
	}
}

void AOutlawsMovieScreen::Finish()
{
	if (bFinished)
	{
		return;
	}
	bFinished = true;
	if (Player) Player->Close();
	Subtitle->SetText(FText::GetEmpty());
	SetSkipHold(0.f, 1.f);
	OnFinished.Broadcast();
}

void AOutlawsMovieScreen::HandleEnd()
{
	UE_LOG(LogOutlaws, Log, TEXT("OLRT movie ended"));
	if (bCreditsRolling) { HoldCreditsPicture(TEXT("the movie ended")); return; }     // (REQ-CREDITS: the roll goes on)
	Finish();
}

void AOutlawsMovieScreen::HandleOpenFailed(FString Url)
{
	UE_LOG(LogOutlaws, Log, TEXT("OLRT movie open failed: %s"), *Url);
	Finish();
}

// ---- a text crawl (HM1: the Historical Missions' intro) -----------------------------------------------------------------

namespace
{
	// the crawl's layout in the remaster's measure (REQ-HOMCRAWL, from its own screenshots): the picture 1080 high, the font
	// at its own size, one line height (71) a line and a blank line between paragraphs, the shadow 5 down and 5 right, black
	constexpr double CrawlLayoutH = 1080.0;
	constexpr double CrawlShadow = 5.0;
	const TCHAR* CrawlHdBackdrop = TEXT("gfx/hintro-4K-32_10.dds");

	// the remaster prints the words in capitals and leaves out what is not a letter or a digit (the settlers' work.
	// shows as THE SETTLERS WORK)
	FString CrawlText(const FString& Line)
	{
		FString Out;
		for (const TCHAR Ch : Line)
		{
			if (FChar::IsAlnum(Ch)) Out.AppendChar(FChar::ToUpper(Ch));
			else if (FChar::IsWhitespace(Ch) && !Out.IsEmpty() && !Out.EndsWith(TEXT(" "))) Out.AppendChar(TEXT(' '));
		}
		Out.TrimEndInline();
		return Out;
	}
}

void AOutlawsMovieScreen::PlayCrawl(const TArray<FString>& Lines, UTexture* Backdrop, const FVector& Eye, const FVector& Forward, float Seconds,
	const OutlawsFormats::FLafFont* Font, bool bHdBackdrop)
{
	FVector Fwd = Forward;
	Fwd.Z = 0.0;
	Fwd = Fwd.GetSafeNormal();
	if (Fwd.IsNearlyZero()) Fwd = FVector::ForwardVector;
	ScreenYaw = Fwd.Rotation().Yaw;
	ScreenPitch = 0.f;
	const FVector Ahead = FRotator(ScreenPitch, ScreenYaw, 0.0).Vector();
	SetActorLocationAndRotation(Eye + Ahead * ScreenDistance, Ahead.Rotation());
	Screen->SetVisibility(true);
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_MovieScreen.M_OLR_MovieScreen"));
	if (!Base) Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_World.M_OLR_World"));
	if (Base)
	{
		Mid = UMaterialInstanceDynamic::Create(Base, this);
		Screen->SetMaterial(0, Mid);
	}
	if (UMaterialInterface* TextMat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_Text.M_OLR_Text")))
	{
		Subtitle->SetTextMaterial(TextMat);
		Prompt->SetTextMaterial(TextMat);
	}
	MakeBarLooks();
	AOutlawsPlayer* Pl = Cast<AOutlawsPlayer>(UGameplayStatics::GetPlayerPawn(this, 0));
	CrawlGlyphs.Reset();
	CrawlAtlas = nullptr;
	CrawlFont = nullptr;
	if (Font) BuildCrawlAtlas(*Font);
	if (!CrawlAtlas)
	{
		if (Pl) CrawlFont = Pl->KpfFont(TEXT("fonts/MontaguSlab_24pt-Bold.ttf"), 52);
		if (!CrawlFont && GEngine) CrawlFont = GEngine->GetLargeFont();
		CrawlLineH = 71.f;
		CrawlAscent = 60.f;
	}
	// the picture: the remaster's wide one (6912 x 2160, BC7) where the GPU reads BC7 and Remastered Graphics is on, else
	// the game's 640 x 480; either cropped to the canvas's 16:9 about its middle, never stretched
	const UWorld* World = GetWorld();
	const bool bMobile = World && World->FeatureLevel <= ERHIFeatureLevel::ES3_1;
	CrawlBack = Backdrop;
	if (bHdBackdrop && Pl && GPixelFormats[PF_BC7].Supported && Pl->LoadMenuSource())
	{
		TArray<uint8> D;
		if (Pl->MenuSource->ReadFile(CrawlHdBackdrop, D) && D.Num() > 148 && FMemory::Memcmp(D.GetData(), "DDS ", 4) == 0
			&& FMemory::Memcmp(D.GetData() + 84, "DX10", 4) == 0)
		{
			int32 PicH = 0, PicW = 0;
			uint32 Dxgi = 0;
			FMemory::Memcpy(&PicH, D.GetData() + 12, 4);
			FMemory::Memcpy(&PicW, D.GetData() + 16, 4);
			FMemory::Memcpy(&Dxgi, D.GetData() + 128, 4);
			if (Dxgi == 98 || Dxgi == 99)
			{
				// (BUG-MENUBLACK) clamped as it is made (set afterwards it never reached the GPU's sampler), CPU copy kept
				if (UTexture2D* Hd = OutlawsTextures::MakeTextureBC7(PicW, PicH, 1, D.GetData() + 148, D.Num() - 148, TF_Bilinear, TA_Clamp, true))
				{
					CrawlBack = Hd;
				}
			}
		}
	}
	const int32 CanvasW = bMobile ? 1280 : 1920, CanvasH = bMobile ? 720 : 1080;
	CrawlBackUV0 = FVector2D::ZeroVector;
	CrawlBackUV1 = FVector2D(1.0, 1.0);
	if (CrawlBack && CrawlBack->GetSurfaceWidth() > 0 && CrawlBack->GetSurfaceHeight() > 0)
	{
		const double Pic = double(CrawlBack->GetSurfaceWidth()) / CrawlBack->GetSurfaceHeight(), Can = double(CanvasW) / CanvasH;
		if (Pic > Can) { const double U = Can / Pic; CrawlBackUV0.X = 0.5 - U * 0.5; CrawlBackUV1.X = 0.5 + U * 0.5; }
		else { const double V = Pic / Can; CrawlBackUV0.Y = 0.5 - V * 0.5; CrawlBackUV1.Y = 0.5 + V * 0.5; }
	}
	CrawlLines.Reset();
	for (const FString& L : Lines) CrawlLines.Add(CrawlText(L));
	while (CrawlLines.Num() > 0 && CrawlLines.Last().IsEmpty()) CrawlLines.Pop();
	CrawlSeconds = FMath::Max(Seconds, 3.f);
	CrawlCanvas = NewObject<UCanvasRenderTarget2D>(this);
	CrawlCanvas->RenderTargetFormat = RTF_RGBA8_SRGB;
	CrawlCanvas->TargetGamma = 1.f;
	CrawlCanvas->ClearColor = FLinearColor::Black;
	// mips, so the thin strokes of the letters do not break up where the picture shows smaller than the canvas (the flat
	// screen's view of it, and its far side in the headset)
	CrawlCanvas->bAutoGenerateMips = true;
	CrawlCanvas->MipsSamplerFilter = TF_Bilinear;
	CrawlCanvas->InitAutoFormat(CanvasW, CanvasH);
	CrawlCanvas->OnCanvasRenderTargetUpdate.AddDynamic(this, &AOutlawsMovieScreen::DrawCrawl);
	if (Mid) Mid->SetTextureParameterValue(TEXT("Tex"), CrawlCanvas);
	Age = 0.f;
	ShownHoldSteps = -1;
	Prompt->SetText(FText::GetEmpty());
	bCrawl = true;
	CrawlCanvas->UpdateResource();
	UE_LOG(LogOutlaws, Log, TEXT("OLRT crawl: %d lines over %s (%dx%d) for %.0f s in %s"), CrawlLines.Num(), CrawlBack ? *CrawlBack->GetName() : TEXT("black"),
		CrawlBack ? int32(CrawlBack->GetSurfaceWidth()) : 0, CrawlBack ? int32(CrawlBack->GetSurfaceHeight()) : 0, CrawlSeconds,
		CrawlAtlas ? TEXT("the game's font") : TEXT("a fallback font"));
}

void AOutlawsMovieScreen::BuildCrawlAtlas(const OutlawsFormats::FLafFont& Font)
{
	// every letter the font has, packed in rows on a sheet 1024 wide (2 px apart so the filtering never bleeds)
	constexpr int32 SheetW = 1024, Pad = 2;
	struct FPlace { TCHAR Ch; const OutlawsFormats::FLafGlyph* G; int32 X, Y; };
	TArray<FPlace> Places;
	int32 X = Pad, Y = Pad, RowH = 0;
	for (int32 c = 32; c < 256; ++c)
	{
		const OutlawsFormats::FLafGlyph* G = Font.Find(TCHAR(c));
		if (!G) continue;
		const int32 Gw = G->W * G->H > 1 ? G->W : 0, Gh = Gw > 0 ? G->H : 0;
		if (X + Gw + Pad > SheetW) { X = Pad; Y += RowH + Pad; RowH = 0; }
		Places.Add({ TCHAR(c), G, X, Y });
		X += Gw + Pad;
		RowH = FMath::Max(RowH, Gh);
	}
	if (Places.Num() == 0) return;
	const int32 SheetH = int32(FMath::RoundUpToPowerOfTwo(uint32(FMath::Max(Y + RowH + Pad, 4))));
	TArray<FColor> Px;
	Px.Init(FColor(255, 255, 255, 0), SheetW * SheetH);
	for (const FPlace& P : Places)
	{
		const OutlawsFormats::FLafGlyph& G = *P.G;
		FCrawlGlyph& Out = CrawlGlyphs.Add(P.Ch);
		Out.Advance = float(G.Advance);
		Out.OffX = float(G.OffX);
		Out.OffY = float(G.OffY);
		if (G.W * G.H <= 1) continue;                   // (a space: the step alone)
		Out.W = float(G.W);
		Out.H = float(G.H);
		Out.UV0 = FVector2D(double(P.X) / SheetW, double(P.Y) / SheetH);
		Out.UV1 = FVector2D(double(P.X + G.W) / SheetW, double(P.Y + G.H) / SheetH);
		for (int32 r = 0; r < G.H; ++r)
		{
			for (int32 q = 0; q < G.W; ++q)
			{
				if (Font.Data[G.DataOffset + r * G.W + q]) Px[(P.Y + r) * SheetW + P.X + q].A = 255;
			}
		}
	}
	CrawlAtlas = OutlawsTextures::MakeTextureBGRA(SheetW, SheetH, Px, false, TF_Bilinear, TA_Clamp);
	CrawlLineH = float(Font.Height);
	CrawlAscent = float(Font.Ascent);
}

double AOutlawsMovieScreen::CrawlSpeed() const
{
	// the first line comes up from under the picture's foot and the last has gone over its top when the time is up
	const double Run = CrawlLayoutH + CrawlLines.Num() * double(CrawlLineH);
	return Run / FMath::Max(1.0, double(CrawlSeconds) - 0.5);
}

void AOutlawsMovieScreen::SeekCrawl(int32 Line, float BaselineFrac)
{
	if (!bCrawl) return;
	const double Start = CrawlLayoutH + CrawlAscent + Line * double(CrawlLineH);
	Age = float((Start - BaselineFrac * CrawlLayoutH) / CrawlSpeed());
	if (CrawlCanvas) CrawlCanvas->UpdateResource();
}

int32 AOutlawsMovieScreen::SheetShadersPending(bool bFinish)
{
	int32 Pending = 0;
#if WITH_EDITOR
	UWorld* W = GetWorld();
	if (!W) return 0;
	for (UMaterialInstanceDynamic* M : StatsMids)
	{
		FMaterialResource* R = M ? M->GetMaterialResource(W->FeatureLevel) : nullptr;
		if (!R || R->IsCompilationFinished()) continue;
		++Pending;
		if (bFinish) R->FinishCompilation();
	}
#endif
	return Pending;
}

bool AOutlawsMovieScreen::SaveCrawlPng(const FString& Png) const
{
	if (!CrawlCanvas) return false;
	TUniquePtr<FArchive> Ar(IFileManager::Get().CreateFileWriter(*Png));
	return Ar && FImageUtils::ExportRenderTarget2DAsPNG(CrawlCanvas, *Ar);
}

void AOutlawsMovieScreen::DrawCrawl(UCanvas* C, int32 Width, int32 Height)
{
	if (!C) return;
	// the picture as it is (the remaster neither dims nor tints it under the words)
	if (CrawlBack)
	{
		FCanvasTileItem Bg(FVector2D::ZeroVector, CrawlBack->GetResource(), FVector2D(Width, Height), CrawlBackUV0, CrawlBackUV1, FLinearColor::White);
		Bg.BlendMode = SE_BLEND_Opaque;
		C->DrawItem(Bg);
	}
	if (!CrawlAtlas && !CrawlFont) return;
	// the lines rise flat up the picture at a steady speed, each centred, cut off at the picture's edges (no fade)
	const double S = Height / CrawlLayoutH;
	const double Across = CrawlLayoutH * Width / Height;                       // the canvas's width in layout px
	const double Top = CrawlLayoutH + CrawlAscent - Age * CrawlSpeed();        // the first line's baseline, in layout px
	const FLinearColor Ink = FLinearColor::FromSRGBColor(FColor(238, 236, 232));
	const FLinearColor Shade = FLinearColor::Black;
	for (int32 Pass = 0; Pass < 2; ++Pass)                                    // the shadows first, so none falls over a letter
	{
		const double Off = Pass == 0 ? CrawlShadow : 0.0;
		const FLinearColor& Col = Pass == 0 ? Shade : Ink;
		for (int32 i = 0; i < CrawlLines.Num(); ++i)
		{
			const FString& Line = CrawlLines[i];
			if (Line.IsEmpty()) continue;
			const double Base = Top + i * double(CrawlLineH);
			if (Base + CrawlLineH < 0.0 || Base - CrawlLineH > CrawlLayoutH) continue;
			if (!CrawlAtlas)
			{
				float Tw = 0.f, Th = 0.f;
				C->TextSize(CrawlFont, Line, Tw, Th, 1.f, 1.f);
				FCanvasTextItem T(FVector2D((Across - Tw) * 0.5 * S + Off * S, (Base - CrawlAscent + Off) * S), FText::FromString(Line), CrawlFont, Col);
				T.Scale = FVector2D(S, S);
				T.BlendMode = SE_BLEND_Translucent;
				C->DrawItem(T);
				continue;
			}
			double Run = 0.0;
			for (const TCHAR Ch : Line)
			{
				if (const FCrawlGlyph* G = CrawlGlyphs.Find(Ch)) Run += G->Advance;
			}
			double Pen = (Across - Run) * 0.5;
			for (const TCHAR Ch : Line)
			{
				const FCrawlGlyph* G = CrawlGlyphs.Find(Ch);
				if (!G) continue;
				if (G->W > 0.f)
				{
					FCanvasTileItem T(FVector2D((Pen + G->OffX + Off) * S, (Base + G->OffY + Off) * S), CrawlAtlas->GetResource(),
						FVector2D(G->W * S, G->H * S), G->UV0, G->UV1, Col);
					T.BlendMode = SE_BLEND_Translucent;
					C->DrawItem(T);
				}
				Pen += G->Advance;
			}
		}
	}
}

// ---- the counts' panels at a level's end (user 2026-10-02) ----------------------------------------------------------------

void AOutlawsMovieScreen::ShowStats()
{
	bStats = true;
	StatsCheck = 0.5f;
	RefreshStats();
}

void AOutlawsMovieScreen::HoldStats(const FVector& Eye, const FVector& Forward, float Seconds)
{
	// placed as Play places the picture, which is not shown: the panels alone, in the middle of the view
	FVector Fwd = Forward;
	Fwd.Z = 0.0;
	Fwd = Fwd.GetSafeNormal();
	if (Fwd.IsNearlyZero()) Fwd = FVector::ForwardVector;
	ScreenYaw = Fwd.Rotation().Yaw;
	ScreenPitch = 0.f;
	const FVector Ahead = FRotator(ScreenPitch, ScreenYaw, 0.0).Vector();
	SetActorLocationAndRotation(Eye + Ahead * ScreenDistance, Ahead.Rotation());
	Screen->SetVisibility(false);
	if (UMaterialInterface* TextMat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_Text.M_OLR_Text")))
	{
		Subtitle->SetTextMaterial(TextMat);
		Prompt->SetTextMaterial(TextMat);
	}
	MakeBarLooks();
	Age = 0.f;
	ShownHoldSteps = -1;
	Prompt->SetText(FText::GetEmpty());
	bHold = true;
	HoldLeft = Seconds;
	ShowStats();
}

void AOutlawsMovieScreen::MakeStatsPanels()
{
	if (StatsPlanes.Num() > 0) return;
	// the menus' own fonts, from the player's Common.kpf (Rye for the name, Montagu Slab for the rows); the sheet a plain
	// parchment tone (the menus' own Main_Base sheet, with its title art and pistols, fought the words)
	if (AOutlawsPlayer* Pl = Cast<AOutlawsPlayer>(UGameplayStatics::GetPlayerPawn(this, 0)))
	{
		StatsTitleFont = Pl->KpfFont(TEXT("fonts/Rye-Regular.ttf"), 60);
		StatsRowFont = Pl->KpfFont(TEXT("fonts/MontaguSlab_24pt-Bold.ttf"), 40);
		StatsSmallFont = Pl->KpfFont(TEXT("fonts/MontaguSlab_24pt-Bold.ttf"), 30);
	}
	if (GEngine)
	{
		if (!StatsTitleFont) StatsTitleFont = GEngine->GetLargeFont();
		if (!StatsRowFont) StatsRowFont = GEngine->GetMediumFont();
		if (!StatsSmallFont) StatsSmallFont = GEngine->GetMediumFont();
	}
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_MovieScreen.M_OLR_MovieScreen"));
	if (!Base) Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_World.M_OLR_World"));
	UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	for (int32 i = 0; i < 2; ++i)
	{
		UCanvasRenderTarget2D* RT = NewObject<UCanvasRenderTarget2D>(this);
		RT->RenderTargetFormat = RTF_RGBA8_SRGB;
		RT->TargetGamma = 1.f;                 // the sRGB target encodes on write (as the menu's)
		RT->ClearColor = FLinearColor(0.015f, 0.01f, 0.006f, 1.f);
		RT->InitAutoFormat(StatsPxW, StatsPxH);
		if (i == 0) RT->OnCanvasRenderTargetUpdate.AddDynamic(this, &AOutlawsMovieScreen::DrawStats0);
		else RT->OnCanvasRenderTargetUpdate.AddDynamic(this, &AOutlawsMovieScreen::DrawStats1);
		UStaticMeshComponent* Pc = NewObject<UStaticMeshComponent>(this);
		Pc->SetStaticMesh(PlaneMesh);
		Pc->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Pc->SetCastShadow(false);
		Pc->SetupAttachment(GetRootComponent());
		Pc->RegisterComponent();
		Pc->SetVisibility(false);
		if (Base)
		{
			UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(Base, this);
			M->SetTextureParameterValue(TEXT("Tex"), RT);
			Pc->SetMaterial(0, M);
			StatsMids.Add(M);
		}
		StatsCanvas.Add(RT);
		StatsPlanes.Add(Pc);
	}
}

void AOutlawsMovieScreen::PlaceStats()
{
	// on the circle round the eye at the screen's distance, each turned to face it: beside the picture (32 to 48 degrees
	// out), or with no picture either side of the middle (one alone: in it); the left is the host's or the one player's
	const float D = ScreenDistance;
	if (!bHold)
	{
		// (user 2026-10-03, BUG-STATSLAYOUT: "above the cinema screen rather than on the left and right ... centered based on
		// how many player cards there are") one row over the picture (and its skip bar), the cards at 0.6 size, side by side
		const int32 Count = FMath::Min(StatsPlanes.Num(), StatsShown.Num());
		constexpr float S = 0.6f, Gap = 6.f;
		const float W = StatsW * S, Hc = StatsH * S;
		const float Z = BarZ + BarH + 14.f + Hc * 0.5f;
		for (int32 i = 0; i < Count; ++i)
		{
			const float Y = (float(i) - 0.5f * float(Count - 1)) * (W + Gap);
			StatsPlanes[i]->SetRelativeLocationAndRotation(FVector(-1.0, Y, Z), FRotationMatrix::MakeFromXY(FVector(0.0, 1.0, 0.0), FVector(0.0, 0.0, -1.0)).Rotator());
			StatsPlanes[i]->SetRelativeScale3D(FVector(W / 100.f, Hc / 100.f, 1.f));
		}
		return;
	}
	for (int32 i = 0; i < StatsPlanes.Num() && i < StatsShown.Num(); ++i)
	{
		const float Side = bStatsTwo && i == 1 ? 1.f : -1.f;
		const float Deg = bHold ? (bStatsTwo ? Side * StatsHoldDeg : 0.f) : Side * StatsBesideDeg;
		const float Rad = FMath::DegreesToRadians(Deg);
		const FVector Centre(D * FMath::Cos(Rad) - D, D * FMath::Sin(Rad), 0.0);
		const FVector ToEye(-FMath::Cos(Rad), -FMath::Sin(Rad), 0.0);
		const FVector Right(ToEye.Y, -ToEye.X, 0.0);
		// (the plane's face is its +Z: X to the panel's right, Y down, as the picture's)
		StatsPlanes[i]->SetRelativeLocationAndRotation(Centre, FRotationMatrix::MakeFromXY(Right, FVector(0.0, 0.0, -1.0)).Rotator());
		StatsPlanes[i]->SetRelativeScale3D(FVector(StatsW * StatsHoldScale / 100.f, StatsH * StatsHoldScale / 100.f, 1.f));
	}
}

void AOutlawsMovieScreen::RefreshStats()
{
	TArray<FOutlawsStatsPanel> Now;
	bool bTwo = false;
	if (UseHandCard())
	{
		RefreshHandCard();
		return;
	}
	if (!UOutlawsStats::Panels(this, Now, bTwo)) return;
	MakeStatsPanels();
	const bool bLayout = bTwo != bStatsTwo || Now.Num() != StatsShown.Num();
	TArray<int32> Changed;
	for (int32 i = 0; i < Now.Num() && i < StatsCanvas.Num(); ++i)
	{
		if (!StatsShown.IsValidIndex(i) || StatsShown[i] != Now[i]) Changed.Add(i);
	}
	bStatsTwo = bTwo;
	StatsShown = Now;
	for (int32 i = 0; i < StatsPlanes.Num(); ++i)
	{
		const bool bShow = StatsShown.IsValidIndex(i);
		if (StatsPlanes[i] && StatsPlanes[i]->IsVisible() != bShow) StatsPlanes[i]->SetVisibility(bShow);
	}
	if (bLayout) PlaceStats();
	for (int32 i : Changed)
	{
		StatsCanvas[i]->UpdateResource();
		FString Rows;
		for (const TPair<FString, FString>& R : StatsShown[i].Rows) Rows += FString::Printf(TEXT("%s%s %s"), Rows.IsEmpty() ? TEXT("") : TEXT(", "), *R.Key, *R.Value);
		UE_LOG(LogOutlaws, Log, TEXT("OLSTATS panel %d drawn (%s): [%s] %s"), i, i == 0 ? TEXT("left") : TEXT("right"), *StatsShown[i].Name, *Rows);
	}
}

void AOutlawsMovieScreen::DrawStats0(UCanvas* C, int32 Width, int32 Height)
{
	DrawStatsPanel(0, C, Width, Height);
}

void AOutlawsMovieScreen::DrawStats1(UCanvas* C, int32 Width, int32 Height)
{
	DrawStatsPanel(1, C, Width, Height);
}

void AOutlawsMovieScreen::DrawStatsPanel(int32 Index, UCanvas* C, int32 Width, int32 Height)
{
	if (!C || !StatsShown.IsValidIndex(Index)) return;
	const FOutlawsStatsPanel& P = StatsShown[Index];
	// colours are linear into the sRGB target (FLinearColor::FromSRGBColor), as the menu's
	auto Lin = [](uint8 R, uint8 G, uint8 B) { return FLinearColor::FromSRGBColor(FColor(R, G, B)); };
	auto Box = [C](float X, float Y, float W, float H, const FLinearColor& Colour)
	{
		FCanvasTileItem T(FVector2D(X, Y), FVector2D(W, H), Colour);
		T.BlendMode = SE_BLEND_Opaque;
		C->DrawItem(T);
	};
	// Align: 0 from X, 1 centred on X, 2 ending at X; shrunk to MaxW
	auto Text = [C](UFont* Font, const FString& Str, float X, float Yc, int32 Align, const FLinearColor& Colour, float MaxW, float MaxH = 0.f)
	{
		if (!Font || Str.IsEmpty()) return;
		float XL = 0.f, YL = 0.f;
		C->TextSize(Font, Str, XL, YL);
		float Scale = XL > MaxW ? MaxW / XL : 1.f;
		if (MaxH > 0.f && YL * Scale > MaxH) Scale = MaxH / YL;      // (REQ-STATS: more rows, each line kept inside its band)
		const float Left = Align == 0 ? X : Align == 1 ? X - XL * Scale * 0.5f : X - XL * Scale;
		FCanvasTextItem Item(FVector2D(Left, Yc), FText::FromString(Str), Font, Colour);
		Item.Scale = FVector2D(Scale, Scale);
		Item.bCentreY = true;
		Item.BlendMode = SE_BLEND_Translucent;
		C->DrawItem(Item);
	};
	const float W = float(Width), H = float(Height);
	const float U = W / StatsBaseW;           // (the layout in the old 512-wide units)
	// dark wood round a brass edge, the parchment inside (toned down: the picture beside it is bright, and the screen's
	// material shows the sheet as drawn, unlit)
	Box(0.f, 0.f, W, H, Lin(34, 24, 16));
	Box(8.f * U, 8.f * U, W - 16.f * U, H - 16.f * U, Lin(140, 108, 54));
	Box(14.f * U, 14.f * U, W - 28.f * U, H - 28.f * U, Lin(152, 130, 90));
	const FLinearColor Ink = Lin(24, 14, 6), Faded = Lin(78, 46, 18), Brass = Lin(120, 88, 40);
	// (BUG-SCORESHEETSIZE) a long sheet (the player's counts, 14 to 17 rows) gets a shorter head so its rows get the room
	const int32 N = FMath::Max(1, P.Rows.Num());
	const bool bLong = N > 9;
	const float TitleY = bLong ? 62.f : 74.f, SubY = bLong ? 124.f : 136.f, RuleY = bLong ? 148.f : 166.f;
	Text(StatsTitleFont, P.Name, W * 0.5f, TitleY * U, 1, Ink, W - 70.f * U, (bLong ? 96.f : 100.f) * U);
	Text(StatsSmallFont, P.Sub, W * 0.5f, SubY * U, 1, Faded, W - 70.f * U, (bLong ? 40.f : 36.f) * U);
	Box(40.f * U, RuleY * U, W - 80.f * U, 3.f * U, Brass);
	// the rows: the label from the left, the number to the right, every other row on a faint band
	const float Top = (RuleY + (bLong ? 10.f : 16.f)) * U, Bottom = H - (bLong ? 26.f : 34.f) * U;
	const float Step = FMath::Min(76.f * U, (Bottom - Top) / float(N));
	for (int32 r = 0; r < P.Rows.Num(); ++r)
	{
		const float Yc = Top + Step * (float(r) + 0.5f);
		if (r % 2 == 1) Box(30.f * U, Yc - Step * 0.5f, W - 60.f * U, Step, Lin(136, 114, 78));
		Text(StatsRowFont, P.Rows[r].Key, 42.f * U, Yc, 0, Faded, W * 0.58f, Step * 0.92f);
		Text(StatsRowFont, P.Rows[r].Value, W - 42.f * U, Yc, 2, Ink, W * 0.3f, Step * 0.92f);
	}
}

// ---- (REQ-HANDSCORE) the card in the hand ---------------------------------------------------------------------------------

namespace
{
	constexpr float HandCardW = 36.f;          // cm across
	constexpr float HandCardHead = 7.f;        // cm: the name, the level, the rule under them
	constexpr float HandCardRow = 2.3f;        // cm a row (the text 0.8 of it: a cap height near 1 cm, about 1.3 degrees at 45 cm)
	constexpr float HandCardPxPerCm = 24.f;
}

bool AOutlawsMovieScreen::UseHandCard() const
{
	if (bHold) return false;
	static const bool bForce = OLFlags::Param(TEXT("OLHandCard"));
	const AOutlawsPlayer* P = Cast<AOutlawsPlayer>(UGameplayStatics::GetPlayerPawn(this, 0));
	return bForce || (P && P->IsVR());
}

void AOutlawsMovieScreen::RefreshHandCard()
{
	FOutlawsStatsCard Card;
	if (!UOutlawsStats::HandCard(this, Card)) return;
	if (bHandCard && Card == HandShown) return;
	AOutlawsPlayer* Pl = Cast<AOutlawsPlayer>(UGameplayStatics::GetPlayerPawn(this, 0));
	if (!StatsTitleFont || !StatsRowFont || !StatsSmallFont)
	{
		if (Pl)
		{
			StatsTitleFont = Pl->KpfFont(TEXT("fonts/Rye-Regular.ttf"), 60);
			StatsRowFont = Pl->KpfFont(TEXT("fonts/MontaguSlab_24pt-Bold.ttf"), 40);
			StatsSmallFont = Pl->KpfFont(TEXT("fonts/MontaguSlab_24pt-Bold.ttf"), 30);
		}
		if (GEngine)
		{
			if (!StatsTitleFont) StatsTitleFont = GEngine->GetLargeFont();
			if (!StatsRowFont) StatsRowFont = GEngine->GetMediumFont();
			if (!StatsSmallFont) StatsSmallFont = GEngine->GetMediumFont();
		}
	}
	// single player: the rows in two columns; co-op: a head row and a row for each player
	const int32 Lines = Card.bTable ? Card.Cells.Num() + 1 : (Card.Rows.Num() + 1) / 2;
	const float Step = FMath::Min(HandCardRow, 28.f / float(FMath::Max(1, Lines)));
	HandSize = FVector2D(HandCardW, HandCardHead + Step * float(Lines) + 1.f);
	const int32 PxW = FMath::RoundToInt(HandSize.X * HandCardPxPerCm), PxH = FMath::RoundToInt(HandSize.Y * HandCardPxPerCm);
	if (!HandCanvas)
	{
		HandCanvas = NewObject<UCanvasRenderTarget2D>(this);
		HandCanvas->RenderTargetFormat = RTF_RGBA8_SRGB;
		HandCanvas->TargetGamma = 1.f;
		HandCanvas->ClearColor = FLinearColor(0.015f, 0.01f, 0.006f, 1.f);
		HandCanvas->InitAutoFormat(PxW, PxH);
		HandCanvas->OnCanvasRenderTargetUpdate.AddDynamic(this, &AOutlawsMovieScreen::DrawHandCard);
	}
	else if (HandCanvas->SizeX != PxW || HandCanvas->SizeY != PxH)
	{
		HandCanvas->ResizeTarget(PxW, PxH);
	}
	if (!HandPlane)
	{
		HandPlane = NewObject<UStaticMeshComponent>(this);
		HandPlane->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane")));
		HandPlane->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		HandPlane->SetCastShadow(false);
		HandPlane->SetupAttachment(GetRootComponent());
		HandPlane->SetUsingAbsoluteLocation(true);
		HandPlane->SetUsingAbsoluteRotation(true);
		HandPlane->SetUsingAbsoluteScale(true);
		HandPlane->RegisterComponent();
		UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_MovieScreen.M_OLR_MovieScreen"));
		if (!Base) Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_World.M_OLR_World"));
		if (Base)
		{
			UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(Base, this);
			M->SetTextureParameterValue(TEXT("Tex"), HandCanvas);
			HandPlane->SetMaterial(0, M);
			StatsMids.Add(M);
		}
	}
	HandShown = Card;
	bHandCard = true;
	HandPlane->SetVisibility(true);
	HandCanvas->UpdateResource();
	PlaceHandCard();
	UE_LOG(LogOutlaws, Log, TEXT("OLRT hand card (%s hand, %s, %.0f x %.0f cm, rows %.1f cm): %s"), Pl && Pl->ScoreCardHand() == 0 ? TEXT("left") : TEXT("right"),
		Pl && Pl->IsVR() ? TEXT("headset") : TEXT("forced, before the eye"), HandSize.X, HandSize.Y, Step, *Card.Lines());
}

void AOutlawsMovieScreen::PlaceHandCard()
{
	AOutlawsPlayer* P = Cast<AOutlawsPlayer>(UGameplayStatics::GetPlayerPawn(this, 0));
	if (!P || !HandPlane) return;
	const FVector Eye = P->Camera ? P->Camera->GetComponentLocation() : P->GetActorLocation();
	FVector Centre;
	if (P->IsVR())
	{
		// held up out of the fist by its bottom edge, as the map is, a little ahead of the hand
		const USceneComponent* F = P->ScoreCardFrame();
		FVector Fw = F->GetForwardVector();
		Fw.Z = 0.0;
		Centre = F->GetComponentLocation() + Fw.GetSafeNormal() * 4.0 + FVector::UpVector * (HandSize.Y * 0.5 + 3.0);
	}
	else
	{
		// (-OLHandCard on the flat screen, the tests: where a held card would be, before the eye)
		FVector Fw = P->Camera ? P->Camera->GetForwardVector() : P->GetActorForwardVector();
		Fw.Z = 0.0;
		Centre = Eye + Fw.GetSafeNormal() * 48.0 - FVector::UpVector * 2.0;
	}
	// turned to the eyes (the plane's face is its +Z, X to the card's right, Y down)
	const FVector ToEye = (Eye - Centre).GetSafeNormal();
	FVector Right = FVector::CrossProduct(ToEye, FVector::UpVector);
	if (Right.IsNearlyZero()) Right = P->GetActorRightVector();
	HandPlane->SetWorldLocationAndRotation(Centre, FRotationMatrix::MakeFromZX(ToEye, Right.GetSafeNormal()).Rotator());
	HandPlane->SetWorldScale3D(FVector(HandSize.X / 100.f, HandSize.Y / 100.f, 1.f));
}

void AOutlawsMovieScreen::DrawHandCard(UCanvas* C, int32 Width, int32 Height)
{
	if (!C || HandSize.X <= 0.f) return;
	const FOutlawsStatsCard& P = HandShown;
	const float W = float(Width), H = float(Height), D = W / HandSize.X;     // px a cm
	auto Lin = [](uint8 R, uint8 G, uint8 B) { return FLinearColor::FromSRGBColor(FColor(R, G, B)); };
	auto Box = [C](float X, float Y, float Bw, float Bh, const FLinearColor& Colour)
	{
		FCanvasTileItem T(FVector2D(X, Y), FVector2D(Bw, Bh), Colour);
		T.BlendMode = SE_BLEND_Opaque;
		C->DrawItem(T);
	};
	auto Text = [C](UFont* Font, const FString& Str, float X, float Yc, int32 Align, const FLinearColor& Colour, float MaxW, float MaxH)
	{
		if (!Font || Str.IsEmpty()) return;
		float XL = 0.f, YL = 0.f;
		C->TextSize(Font, Str, XL, YL);
		if (XL <= 0.f || YL <= 0.f) return;
		const float Scale = FMath::Min(MaxW / XL, MaxH / YL);
		const float Left = Align == 0 ? X : Align == 1 ? X - XL * Scale * 0.5f : X - XL * Scale;
		FCanvasTextItem Item(FVector2D(Left, Yc), FText::FromString(Str), Font, Colour);
		Item.Scale = FVector2D(Scale, Scale);
		Item.bCentreY = true;
		Item.BlendMode = SE_BLEND_Translucent;
		C->DrawItem(Item);
	};
	Box(0.f, 0.f, W, H, Lin(34, 24, 16));
	Box(8.f, 8.f, W - 16.f, H - 16.f, Lin(140, 108, 54));
	Box(14.f, 14.f, W - 28.f, H - 28.f, Lin(160, 138, 98));
	const FLinearColor Ink = Lin(20, 12, 4), Faded = Lin(70, 40, 14), Brass = Lin(120, 88, 40), Band = Lin(144, 122, 86), Mine = Lin(208, 176, 104);
	Text(StatsTitleFont, P.Title, W * 0.5f, 2.3f * D, 1, Ink, W - 3.f * D, 3.0f * D);
	Text(StatsSmallFont, P.Sub, W * 0.5f, 4.9f * D, 1, Faded, W - 3.f * D, 1.7f * D);
	Box(1.5f * D, 6.3f * D, W - 3.f * D, 3.f, Brass);
	const float Top = HandCardHead * D, Inner = W - 2.f * D, X0 = 1.f * D;
	const int32 Lines = P.bTable ? P.Cells.Num() + 1 : (P.Rows.Num() + 1) / 2;
	const float Step = (H - Top - 1.f * D) / float(FMath::Max(1, Lines));
	const float TextH = Step * 0.8f;
	if (!P.bTable)
	{
		// two columns, the label from the left, the number to the right; every other row on a faint band
		for (int32 r = 0; r < Lines; ++r)
		{
			if (r % 2 == 1) Box(X0, Top + Step * r, Inner, Step, Band);
		}
		Box(W * 0.5f - 1.5f, Top, 3.f, Step * Lines, Brass);
		const float ColW = Inner * 0.5f;
		for (int32 i = 0; i < P.Rows.Num(); ++i)
		{
			const int32 Col = i / Lines, Row = i % Lines;
			const float Cx = X0 + ColW * Col, Yc = Top + Step * (Row + 0.5f);
			Text(StatsRowFont, P.Rows[i].Key, Cx + 0.4f * D, Yc, 0, Faded, ColW * 0.66f, TextH);
			Text(StatsRowFont, P.Rows[i].Value, Cx + ColW - 0.5f * D, Yc, 2, Ink, ColW * 0.27f, TextH);
		}
		return;
	}
	// co-op: the players' column, then one for each count; the holder's row first, picked out
	const int32 NC = FMath::Max(1, P.Cols.Num());
	const float NameW = Inner * 0.32f, CountW = (Inner - NameW) / float(FMath::Max(1, NC - 1));
	auto ColX = [&](int32 c) { return c == 0 ? X0 + 0.4f * D : X0 + NameW + CountW * (c - 0.5f); };
	for (int32 c = 0; c < P.Cols.Num(); ++c)
	{
		Text(StatsSmallFont, P.Cols[c], ColX(c), Top + Step * 0.5f, c == 0 ? 0 : 1, Faded, c == 0 ? NameW - 0.6f * D : CountW - 0.4f * D, TextH * 0.85f);
	}
	Box(X0, Top + Step - 1.5f, Inner, 3.f, Brass);
	for (int32 r = 0; r < P.Cells.Num(); ++r)
	{
		const float Y = Top + Step * (r + 1);
		if (r == 0) Box(X0, Y + 2.f, Inner, Step - 2.f, Mine);
		else if (r % 2 == 0) Box(X0, Y, Inner, Step, Band);
		const TArray<FString>& Row = P.Cells[r];
		for (int32 c = 0; c < Row.Num() && c < NC; ++c)
		{
			Text(StatsRowFont, Row[c], ColX(c), Y + Step * 0.5f, c == 0 ? 0 : 1, Ink, c == 0 ? NameW - 0.6f * D : CountW - 0.4f * D, TextH);
		}
	}
}
