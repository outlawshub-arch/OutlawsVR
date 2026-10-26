// REQ-CREDITS (user 2026-10-05, board card): the end credits roll at the end of the final cutscene, as James rides off into
// the sunset: first the Outlaws VR credits (Content/Credits/vr_credits.txt), then the original game's own (credits.txt from
// the player's LABs), both in the original's layout ("~" a heading, a plain line a name, an empty line a line of space), in
// the crawl's look (REQ-HOMCRAWL: mf3s.laf, white with a black drop shadow, rising flat up the full picture).
#include "OutlawsMovieScreen.h"
#include "OutlawsRuntime.h"
#include "OutlawsFormats.h"
#include "OutlawsPlayer.h"
#include "MediaPlayer.h"
#include "MediaTexture.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/Canvas.h"
#include "Engine/CanvasRenderTarget2D.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "CanvasItem.h"
#include "Kismet/GameplayStatics.h"

namespace
{
	// the layout in the crawl's measure (the picture 1080 high).  The original drew its credits in credits.laf, 18 px a line
	// on its 480-line screen (3.75% of the picture); mf3s.laf's 71 px line at 0.6 is the same share (3.9%).  Headings and
	// names one size, as the original's one font; the headings in the menus' gold, the names in the crawl's white.
	constexpr double CreditsLayoutH = 1080.0;
	constexpr double CreditsScale = 0.6;
	constexpr double CreditsShadow = 5.0 * CreditsScale;
	constexpr double CreditsPxPerSecond = 66.0;          // about a line and a half a second
}

void AOutlawsMovieScreen::SetUpCreditsLook(const OutlawsFormats::FLafFont* Font)
{
	CrawlGlyphs.Reset();
	CrawlAtlas = nullptr;
	CrawlFont = nullptr;
	if (Font) BuildCrawlAtlas(*Font);
	if (!CrawlAtlas)
	{
		AOutlawsPlayer* Pl = Cast<AOutlawsPlayer>(UGameplayStatics::GetPlayerPawn(this, 0));
		if (Pl) CrawlFont = Pl->KpfFont(TEXT("fonts/MontaguSlab_24pt-Bold.ttf"), 52);
		if (!CrawlFont && GEngine) CrawlFont = GEngine->GetLargeFont();
		CrawlLineH = 71.f;
		CrawlAscent = 60.f;
	}
}

void AOutlawsMovieScreen::ArmCredits(const TArray<FOutlawsCreditLine>& Lines, const OutlawsFormats::FLafFont* Font, double StartAt, double HoldAt)
{
	CreditLines = Lines;
	while (CreditLines.Num() > 0 && CreditLines.Last().Text.IsEmpty()) CreditLines.Pop();     // (no wait after the last name)
	if (CreditLines.Num() == 0) return;
	SetUpCreditsLook(Font);
	CreditsStartAt = StartAt;
	CreditsHoldAt = FMath::Max(HoldAt, StartAt);
	bCredits = true;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT credits armed: %d lines (Outlaws VR %d, the original %d), the roll from %.1f s of the movie, its picture held at %.1f s, %.0f s long"),
		CreditLines.Num(), CreditsLineCount(0), CreditsLineCount(1), CreditsStartAt, CreditsHoldAt, CreditsSeconds());
}

void AOutlawsMovieScreen::PlayCredits(const TArray<FOutlawsCreditLine>& Lines, const OutlawsFormats::FLafFont* Font, const FVector& Eye, const FVector& Forward)
{
	// as a crawl stands: in front of the viewer at eye height, level, facing them
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
	Age = 0.f;
	ShownHoldSteps = -1;
	Prompt->SetText(FText::GetEmpty());
	ArmCredits(Lines, Font, 0.0, 0.0);
	if (!bCredits)
	{
		Finish();                         // (nothing to roll: at once)
		return;
	}
	StartCreditsRoll();
	HoldCreditsPicture(TEXT("no movie: the black"));
}

void AOutlawsMovieScreen::StartCreditsRoll()
{
	if (bCreditsRolling) return;
	const UWorld* World = GetWorld();
	const bool bMobile = World && World->FeatureLevel <= ERHIFeatureLevel::ES3_1;
	const int32 CanvasW = bMobile ? 1280 : 1920, CanvasH = bMobile ? 720 : 1080;
	CrawlCanvas = NewObject<UCanvasRenderTarget2D>(this);
	CrawlCanvas->RenderTargetFormat = RTF_RGBA8_SRGB;
	CrawlCanvas->TargetGamma = 1.f;
	CrawlCanvas->ClearColor = FLinearColor::Black;
	CrawlCanvas->bAutoGenerateMips = true;           // (the thin strokes held together where the picture shows small, as the crawl's)
	CrawlCanvas->MipsSamplerFilter = TF_Bilinear;
	CrawlCanvas->InitAutoFormat(CanvasW, CanvasH);
	CrawlCanvas->OnCanvasRenderTargetUpdate.AddDynamic(this, &AOutlawsMovieScreen::DrawCredits);
	if (Mid) Mid->SetTextureParameterValue(TEXT("Tex"), CrawlCanvas);
	// the level's counts made way for the roll (they stood beside the picture, or in the hand, since the movie began)
	bStats = false;
	for (UStaticMeshComponent* Pc : StatsPlanes) if (Pc) Pc->SetVisibility(false);
	bHandCard = false;
	if (HandPlane) HandPlane->SetVisibility(false);
	Subtitle->SetText(FText::GetEmpty());
	CreditsAge = 0.f;
	bCreditsRolling = true;
	CrawlCanvas->UpdateResource();
	UE_LOG(LogOutlaws, Log, TEXT("OLRT credits roll starts%s: %d lines in %s, %.0f s"), Player ? *FString::Printf(TEXT(" at %.1f s of the movie"), GetTime()) : TEXT(" on the black"),
		CreditLines.Num(), CrawlAtlas ? TEXT("mf3s.laf") : TEXT("a fallback font"), CreditsSeconds());
}

void AOutlawsMovieScreen::HoldCreditsPicture(const TCHAR* Why)
{
	if (bCreditsHeld) return;
	bCreditsHeld = true;
	if (Player && Player->IsPlaying()) Player->Pause();
	UE_LOG(LogOutlaws, Log, TEXT("OLRT credits: the picture held (%s) at %.1f s, the roll %.0f s in"), Why, GetTime(), CreditsAge);
	OnCreditsHeld.Broadcast();
}

bool AOutlawsMovieScreen::TickCredits(float DeltaSeconds)
{
	if (!bCreditsRolling)
	{
		if (!Player || !bStarted || GetTime() < CreditsStartAt) return false;      // (the movie as ever until James rides off)
		StartCreditsRoll();
	}
	if (!bCreditsHeld && Player)
	{
		// held before the movie's last fade to black: a second and a half short of its end at the latest
		const double Length = Player->GetDuration().GetTotalSeconds();
		const double HoldAt = Length > 1.0 ? FMath::Min(CreditsHoldAt, Length - 1.5) : CreditsHoldAt;
		if (GetTime() >= HoldAt) HoldCreditsPicture(TEXT("its hold point"));
	}
	CreditsAge += DeltaSeconds;
	if (CrawlCanvas) CrawlCanvas->UpdateResource();
	if (CreditsAge >= CreditsSeconds())
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT credits over (%.0f s)"), CreditsAge);
		Finish();
	}
	return true;
}

double AOutlawsMovieScreen::CreditsRunPx() const
{
	// the first line comes up from under the picture's foot; the roll is over when the last has gone over its top
	return CreditsLayoutH + CreditLines.Num() * double(CrawlLineH) * CreditsScale;
}

double AOutlawsMovieScreen::CreditsSpeed() const
{
	return CreditsPxPerSecond;
}

int32 AOutlawsMovieScreen::CreditsLineCount(int32 Part) const
{
	int32 N = 0;
	for (const FOutlawsCreditLine& L : CreditLines) N += L.Part == Part ? 1 : 0;
	return N;
}

int32 AOutlawsMovieScreen::CreditsFirstLine(int32 Part) const
{
	for (int32 i = 0; i < CreditLines.Num(); ++i) if (CreditLines[i].Part == Part && !CreditLines[i].Text.IsEmpty()) return i;
	return INDEX_NONE;
}

void AOutlawsMovieScreen::SeekCredits(int32 Line, float BaselineFrac)
{
	if (!bCreditsRolling) return;
	const double Start = CreditsLayoutH + double(CrawlAscent) * CreditsScale + Line * double(CrawlLineH) * CreditsScale;
	CreditsAge = float(FMath::Max(0.0, (Start - BaselineFrac * CreditsLayoutH) / CreditsSpeed()));
	if (CrawlCanvas) CrawlCanvas->UpdateResource();
}

void AOutlawsMovieScreen::SeekMovie(float Seconds)
{
	if (Player && !bCreditsHeld) Player->Seek(FTimespan::FromSeconds(Seconds));
}

void AOutlawsMovieScreen::DrawCredits(UCanvas* C, int32 Width, int32 Height)
{
	if (!C) return;
	// the movie's picture as it is under the words (playing, then held); the black without one
	if (Player && Texture && Texture->GetResource())
	{
		FCanvasTileItem Bg(FVector2D::ZeroVector, Texture->GetResource(), FVector2D(Width, Height), FVector2D::ZeroVector, FVector2D(1.0, 1.0), FLinearColor::White);
		Bg.BlendMode = SE_BLEND_Opaque;
		C->DrawItem(Bg);
	}
	if (!CrawlAtlas && !CrawlFont) return;
	const double S = Height / CreditsLayoutH;
	const double K = CreditsScale;
	const double LineH = double(CrawlLineH) * K;
	const double Across = CreditsLayoutH * Width / Height;
	const double Top = CreditsLayoutH + double(CrawlAscent) * K - CreditsAge * CreditsSpeed();      // the first line's baseline
	const FLinearColor Ink = FLinearColor::FromSRGBColor(FColor(238, 236, 232));
	const FLinearColor Gold = FLinearColor::FromSRGBColor(FColor(240, 200, 120));
	const FLinearColor Shade = FLinearColor::Black;
	// the lines on show only (the original's credits are some 800 lines)
	const int32 First = FMath::Max(0, FMath::FloorToInt((-Top - LineH) / LineH));
	const int32 Last = FMath::Min(CreditLines.Num() - 1, FMath::CeilToInt((CreditsLayoutH - Top + LineH) / LineH));
	for (int32 Pass = 0; Pass < 2; ++Pass)                                    // the shadows first, so none falls over a letter
	{
		const double Off = Pass == 0 ? CreditsShadow : 0.0;
		for (int32 i = First; i <= Last; ++i)
		{
			const FOutlawsCreditLine& Line = CreditLines[i];
			if (Line.Text.IsEmpty()) continue;
			const FLinearColor& Col = Pass == 0 ? Shade : Line.bHeading ? Gold : Ink;
			const double Base = Top + i * LineH;
			if (!CrawlAtlas)
			{
				float Tw = 0.f, Th = 0.f;
				C->TextSize(CrawlFont, Line.Text, Tw, Th, 1.f, 1.f);
				FCanvasTextItem T(FVector2D((Across - Tw * K) * 0.5 * S + Off * S, (Base - CrawlAscent * K + Off) * S), FText::FromString(Line.Text), CrawlFont, Col);
				T.Scale = FVector2D(S * K, S * K);
				T.BlendMode = SE_BLEND_Translucent;
				C->DrawItem(T);
				continue;
			}
			double Run = 0.0;
			for (const TCHAR Ch : Line.Text)
			{
				if (const FCrawlGlyph* G = CrawlGlyphs.Find(Ch)) Run += G->Advance * K;
			}
			double Pen = (Across - Run) * 0.5;
			for (const TCHAR Ch : Line.Text)
			{
				const FCrawlGlyph* G = CrawlGlyphs.Find(Ch);
				if (!G) continue;
				if (G->W > 0.f)
				{
					FCanvasTileItem T(FVector2D((Pen + G->OffX * K + Off) * S, (Base + G->OffY * K + Off) * S), CrawlAtlas->GetResource(),
						FVector2D(G->W * K * S, G->H * K * S), G->UV0, G->UV1, Col);
					T.BlendMode = SE_BLEND_Translucent;
					C->DrawItem(T);
				}
				Pen += G->Advance * K;
			}
		}
	}
}
