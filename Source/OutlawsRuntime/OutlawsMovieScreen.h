#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "OutlawsStats.h"
#include "OutlawsMovieScreen.generated.h"

class UMediaPlayer;
class UMediaTexture;
class UFileMediaSource;
class UMediaSoundComponent;
class UStaticMeshComponent;
class UTextRenderComponent;
class UMaterialInstanceDynamic;
class UCanvasRenderTarget2D;
class UCanvas;
class UFont;
namespace OutlawsFormats { struct FLafFont; }

/** (REQ-CREDITS) one line of the end credits: the original's credits.txt layout ("~" a heading, a plain line a name, an empty
 *  line a line of space); Part 0 the Outlaws VR credits, 1 the original game's. */
struct FOutlawsCreditLine
{
	FString Text;
	bool bHeading = false;
	int32 Part = 0;
};

/**
 * A story movie (the remaster's movies\<NAME>.webm converted to .mp4: 16:9, or 4:3 for the original cutscenes) on a
 * screen standing in front of the player, world-locked at eye height, with its sound and its .srt subtitles under it.
 * (REQ-MOVIE43) The screen takes the movie's own shape, read from the media once it is open, at the same height.
 * Plays through Media Foundation; OnFinished fires at the end, on a skip, or at once when the file
 * is missing or will not open.
 */
UCLASS(NotPlaceable)
class OUTLAWSRUNTIME_API AOutlawsMovieScreen : public AActor
{
	GENERATED_BODY()

public:
	AOutlawsMovieScreen();

	/** Start the movie file; the screen faces From (the viewer's eye) from Distance cm along Forward. */
	bool Play(const FString& File, const FVector& Eye, const FVector& Forward, float Distance = 300.f);
	float ScreenDistance = 300.f;
	float ScreenYaw = 0.f;                    // follows the viewer's head lazily, as the pause menu does
	float ScreenYawVel = 0.f;                 // (its spring's speed, degrees a second)
	bool bScreenYawFollow = false;            // (easing back to the middle of the view)
	float ScreenPitch = 0.f;                  // kept level (the pause menu alone tips with the head; user 2026-09-29)
	/** Stop now (the player skipped): OnFinished fires. */
	void Skip();
	/** Held behind the pause menu (user 2026-10-01): picture and sound stop where they are, and go on from there. */
	void SetPaused(bool bPause);
	/** The menu's death tone on the picture itself, 0..1 (the headset has no post process to give it). */
	void SetTone(float Tone);
	bool bPaused = false;
	/** The first second of a movie cannot be skipped (user 2026-09-27). */
	bool CanSkip() const { return Age >= 1.f; }
	/** How long a button has been held toward the skip (0 = none): the screen says to keep holding, with a bar that
	 *  fills (Buttons held: its speed, shown as x2, x3...). */
	void SetSkipHold(float Held, float Need, int32 Buttons = 1);
	bool IsPlaying() const { return !bFinished; }
	float GetTime() const;
	void SetVolume(float Volume);
	/** (REQ-MOVIE43) the picture's width over its height as the media gave it (0 until it is open), and the screen's size (cm). */
	float GetPictureAspect() const { return PictureAspect; }
	FVector2D GetScreenSize() const;

	/** The level's end (user 2026-10-02): the players' counts (UOutlawsStats) on a panel beside the picture, turned to the
	 *  eyes and carried round with the screen: in co-op the host's on the left and the client's on the right, in single
	 *  player one on the left.  While they show, the screen waits for the head to turn further before it follows (50
	 *  degrees, not 25), so a panel can be looked at. */
	void ShowStats();
	/** No cutscene to play: no picture, the panels alone on the black for Seconds (in the middle of the view), then it
	 *  finishes as a movie does (skippable the same way). */
	void HoldStats(const FVector& Eye, const FVector& Forward, float Seconds);

	/** A text crawl (HM1, user 2026-10-03: the Historical Missions' intro.txt, "a time for lawmen"), as the remaster shows
	 *  it (REQ-HOMCRAWL, user 2026-10-05): the lines in capitals, without punctuation, in the game's own western font
	 *  (Font: mf3s.laf) in white with a black drop shadow, centred, rising flat up the full picture over Backdrop
	 *  (hintro.pcx; the remaster's wide hintro from Common.kpf when bHdBackdrop and the GPU reads BC7; black without
	 *  either), for Seconds; skippable as a movie is.  Drawn on a canvas in the picture's place. */
	void PlayCrawl(const TArray<FString>& Lines, class UTexture* Backdrop, const FVector& Eye, const FVector& Forward, float Seconds,
		const OutlawsFormats::FLafFont* Font, bool bHdBackdrop);
	bool IsCrawl() const { return bCrawl; }
	/** (tests) The crawl moved to where line Line's baseline sits BaselineFrac down the picture; its canvas saved as Png. */
	void SeekCrawl(int32 Line, float BaselineFrac);
	bool SaveCrawlPng(const FString& Png) const;
	/** (tests, BUG-SCORESHEETSIZE) how many of the sheets' materials have shaders still compiling for this feature level; with
	 *  bFinish the game thread waits for them.  The editor compiles the ES3_1 preview's shaders when the sheets first show, and
	 *  until they are in the planes are not drawn (a black shot); a cooked game has them built in and never waits. */
	int32 SheetShadersPending(bool bFinish);

	/** (REQ-CREDITS, OutlawsMovieScreenCredits.cpp) the end credits, rolled flat up the picture in the crawl's look (mf3s.laf,
	 *  black shadow): ArmCredits before Play, and the roll starts over the movie at StartAt s (James riding off into the
	 *  sunset), the picture held at HoldAt s (before the movie's fade) while the roll goes on; PlayCredits rolls them on the
	 *  black, no movie.  The screen finishes when the last line has gone over the top, or on a skip. */
	void ArmCredits(const TArray<FOutlawsCreditLine>& Lines, const OutlawsFormats::FLafFont* Font, double StartAt, double HoldAt);
	void PlayCredits(const TArray<FOutlawsCreditLine>& Lines, const OutlawsFormats::FLafFont* Font, const FVector& Eye, const FVector& Forward);
	bool HasCredits() const { return bCredits; }
	/** The roll has run here (started, or the player skipped the screen it was armed on): it is not to be played again. */
	bool CreditsDone() const { return bCredits && (bCreditsRolling || bCreditsSkipped); }
	bool IsCreditsRolling() const { return bCreditsRolling; }
	bool IsCreditsHeld() const { return bCreditsHeld; }
	float CreditsSeconds() const { return float(CreditsRunPx() / CreditsSpeed()); }
	float CreditsAgeNow() const { return CreditsAge; }
	/** (tests) the roll moved so line Line's baseline sits BaselineFrac down the picture; the movie moved to Seconds. */
	void SeekCredits(int32 Line, float BaselineFrac);
	void SeekMovie(float Seconds);
	int32 CreditsLineCount(int32 Part) const;
	int32 CreditsFirstLine(int32 Part) const;
	/** The picture held (or the roll on the black from the start): the level starts the credits' music. */
	DECLARE_MULTICAST_DELEGATE(FOnCreditsHeld);
	FOnCreditsHeld OnCreditsHeld;

	DECLARE_MULTICAST_DELEGATE(FOnMovieFinished);
	FOnMovieFinished OnFinished;

	virtual void Tick(float DeltaSeconds) override;

private:
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UStaticMeshComponent> Screen;
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UMediaSoundComponent> Sound;
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UTextRenderComponent> Subtitle;
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UTextRenderComponent> Prompt;      // "Hold to skip", over the picture
	int32 ShownHoldSteps = -1;
	// the skip bar over the picture (user 2026-09-28: the text bar looked poor): a gold frame, a dark track, the gold
	// fill with a bright leading edge
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UStaticMeshComponent> BarFrame;
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UStaticMeshComponent> BarTrack;
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UStaticMeshComponent> BarFill;
	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UStaticMeshComponent> BarEdge;
	void MakeBarLooks();
	void PlaceBarPiece(UStaticMeshComponent* C, float Left, float Width, float Height, float Depth);
	UPROPERTY(Transient)
	TObjectPtr<UMediaPlayer> Player;
	UPROPERTY(Transient)
	TObjectPtr<UMediaTexture> Texture;
	UPROPERTY(Transient)
	TObjectPtr<UFileMediaSource> Source;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> Mid;

	struct FCue { double Start = 0.0; double End = 0.0; FString Text; };
	TArray<FCue> Cues;
	int32 ShownCue = -1;
	bool bFinished = false;
	bool bStarted = false;
	float Age = 0.f;
	float StallFor = 0.f;              // (seconds the picture has stood at LastTime while playing)
	double LastTime = -1.0;
	float PictureAspect = 0.f;         // (REQ-MOVIE43) the movie's shape, once read (FitPicture)
	void FitPicture();
	void SetScreenAspect(float Aspect);

	// the counts' panels (ShowStats): a canvas each, drawn in the menus' own fonts, on a plane in the picture's material
	bool bStats = false;
	bool bHold = false;                        // (HoldStats: no picture, only the panels)
	float HoldLeft = 0.f;
	float StatsCheck = 0.f;
	bool bStatsTwo = false;
	TArray<FOutlawsStatsPanel> StatsShown;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UCanvasRenderTarget2D>> StatsCanvas;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UStaticMeshComponent>> StatsPlanes;
	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> StatsMids;
	UPROPERTY(Transient)
	TObjectPtr<UFont> StatsTitleFont;
	UPROPERTY(Transient)
	TObjectPtr<UFont> StatsRowFont;
	UPROPERTY(Transient)
	TObjectPtr<UFont> StatsSmallFont;
	void MakeStatsPanels();
	void RefreshStats();
	void PlaceStats();
	void DrawStatsPanel(int32 Index, UCanvas* C, int32 Width, int32 Height);
	UFUNCTION()
	void DrawStats0(UCanvas* C, int32 Width, int32 Height);
	UFUNCTION()
	void DrawStats1(UCanvas* C, int32 Width, int32 Height);
	// (REQ-HANDSCORE) on the headset the counts are a card held in the hand (the one controller's, else the off hand),
	// turned to the eyes, not panels over the picture (too small to read at the Quest's resolution)
	bool bHandCard = false;
	FOutlawsStatsCard HandShown;
	FVector2D HandSize = FVector2D::ZeroVector;      // cm
	UPROPERTY(Transient)
	TObjectPtr<UCanvasRenderTarget2D> HandCanvas;
	UPROPERTY(Transient)
	TObjectPtr<UStaticMeshComponent> HandPlane;
	bool UseHandCard() const;
	void RefreshHandCard();
	void PlaceHandCard();
	UFUNCTION()
	void DrawHandCard(UCanvas* C, int32 Width, int32 Height);

	bool bCrawl = false;
	float CrawlSeconds = 0.f;
	TArray<FString> CrawlLines;
	UPROPERTY(Transient)
	TObjectPtr<class UTexture> CrawlBack;
	UPROPERTY(Transient)
	TObjectPtr<UCanvasRenderTarget2D> CrawlCanvas;
	UPROPERTY(Transient)
	TObjectPtr<UFont> CrawlFont;                   // (only when the game's font would not load)
	// the game's font as an atlas (white, coverage in alpha) and where each letter is in it, in the font's pixels
	struct FCrawlGlyph { FVector2D UV0 = FVector2D::ZeroVector, UV1 = FVector2D::ZeroVector; float W = 0.f, H = 0.f, OffX = 0.f, OffY = 0.f, Advance = 0.f; };
	TMap<TCHAR, FCrawlGlyph> CrawlGlyphs;
	UPROPERTY(Transient)
	TObjectPtr<class UTexture2D> CrawlAtlas;
	float CrawlLineH = 71.f, CrawlAscent = 66.f;
	FVector2D CrawlBackUV0 = FVector2D::ZeroVector, CrawlBackUV1 = FVector2D(1.0, 1.0);
	void BuildCrawlAtlas(const OutlawsFormats::FLafFont& Font);
	double CrawlSpeed() const;                     // layout px (the picture 1080 high) a second
	UFUNCTION()
	void DrawCrawl(UCanvas* C, int32 Width, int32 Height);

	// (REQ-CREDITS) the end credits' state; drawn on CrawlCanvas with the crawl's glyph atlas
	bool bCredits = false;             // armed (or rolling on the black)
	bool bCreditsRolling = false;
	bool bCreditsHeld = false;         // the movie's picture stopped where it is (or no movie)
	bool bCreditsSkipped = false;
	double CreditsStartAt = 0.0, CreditsHoldAt = 0.0;
	float CreditsAge = 0.f;
	TArray<FOutlawsCreditLine> CreditLines;
	void SetUpCreditsLook(const OutlawsFormats::FLafFont* Font);
	void StartCreditsRoll();
	void HoldCreditsPicture(const TCHAR* Why);
	bool TickCredits(float DeltaSeconds);
	double CreditsRunPx() const;
	double CreditsSpeed() const;
	UFUNCTION()
	void DrawCredits(UCanvas* C, int32 Width, int32 Height);

	void LoadSubtitles(const FString& SrtFile);
	void Finish();

	UFUNCTION()
	void HandleEnd();
	UFUNCTION()
	void HandleOpenFailed(FString Url);
};
