#pragma once

#include "CoreMinimal.h"
#include "Components/StaticMeshComponent.h"
#include "OutlawsGlyphs.generated.h"

class AOutlawsPlayer;
class UCanvas;
class UFont;
class UTextureRenderTarget2D;
class UMaterialInstanceDynamic;
struct FKey;

/**
 * REQ-BUTTONGLYPHS: a VR prompt shows where its button sits instead of naming it ("A / X: restart level" made the player
 * look at their hands).  A Touch controller seen from the top is drawn by the game from plain shapes (the ring, the face
 * with its lettered buttons, the stick and the menu button, the trigger peeking out at the top and the grip tab at the inner
 * bottom edge; no artwork), the control to use filled red (a button ringed too; the trigger and the grip are shapes of their
 * own that turn red).  A prompt either hand can answer shows the main hand's controller only (REQ-ONEGLYPH); a pushed
 * stick gets arrows, a stick click a dot.
 *
 * The marks come from the keys bound to the prompt's input actions right now (Input.ini remaps included), less the hand
 * not held in one-controller mode, so the ring is always on a control that answers.  Gameplay prompts whose button depends
 * on the left-handed swap or the Classic layout name the action the game's own handler reads for that setting.
 */
namespace OutlawsGlyphs
{
	enum class ECtl : uint8 { Lower, Upper, Stick, StickClick, Menu, Trigger, Grip };

	struct FMark
	{
		int8 Hand = 1;                 // 0 left, 1 right
		ECtl Ctl = ECtl::Lower;
		bool operator==(const FMark& O) const { return Hand == O.Hand && Ctl == O.Ctl; }
	};

	struct FGlyph
	{
		TArray<FMark> Marks;           // at most one per hand; the left hand's first
		int8 Main = 1;                 // REQ-ONEGLYPH: the hand drawn when both answer (the dominant one)
		bool IsEmpty() const { return Marks.Num() == 0; }
		bool operator==(const FGlyph& O) const { return Marks == O.Marks && Main == O.Main; }
		/** REQ-ONEGLYPH: the marks drawn.  A prompt either hand answers shows the main hand's controller only (two read as
		 *  "press both"); the other hand's control still answers. */
		TArray<FMark, TInlineAllocator<2>> Drawn() const;
		/** For the logs: "R lower + L lower". */
		FString Describe() const;
		void Add(const FMark& M);
	};

	/** The control a key is on (Oculus Touch and generic motion controller keys); false for anything else. */
	bool KeyToMark(const FKey& Key, FMark& Out);
	/** The key that sits on a control, for the tests' presses (Oculus Touch names). */
	FKey MarkToKey(const FMark& M);
	/** The controls bound to these input actions now (the player's input first, then the project's mappings). */
	FGlyph ForActions(const AOutlawsPlayer* P, std::initializer_list<const TCHAR*> Actions);
	/** A control by its kind on one hand (Hand 0 / 1) or on either (-1: both hands, or the one controller's). */
	FGlyph ForControl(const AOutlawsPlayer* P, ECtl Ctl, int32 Hand);

	/** (user 15 Oct) every drawing the size the death screen's are seen at: its height over its distance from the eyes (the
	 *  plate's 150 px of 1024 across 110 cm, 140 cm away). */
	constexpr float ViewTan = 0.115f;
	/** A stacked prompt's drawing's top this far under its words, in drawing heights (the trigger's top stands 0.09 over the
	 *  drawing's box). */
	constexpr float StackGap = 0.14f;

	/** One prompt: its label and the controls that answer it. */
	struct FPart
	{
		FGlyph Glyph;
		FString Label;
		bool bHot = false;             // (REQ-BACKCLICK) pointed at: drawn as a pointed-at menu row is (HotInk, outlined black)
		bool bRight = false;           // (REQ-BACKRIGHT) the drawing to the right of the label, on its line, the two centred as one
	};
	/** A pointed-at menu row's colour (the remaster's #e9931c). */
	FLinearColor HotInk();

	/** The drawing's width for a height in pixels. */
	float Width(const FGlyph& G, float Height);
	/** Draws the glyph centred on Centre, Height pixels tall; bMask: all white (the coverage sheets). Returns its box. */
	FBox2D Draw(UCanvas* C, const FGlyph& G, const FVector2D& Centre, float Height, bool bMask);
	/** The glyph then its label beside it, centred together on Centre; text as the menus draw it (outlined, translucent).  For
	 *  instruction lines (the tutorial's cards), where the words tell the player what to do. */
	FBox2D DrawWithLabel(UCanvas* C, const FGlyph& G, const FString& Label, UFont* Font, const FVector2D& Centre, float GlyphH,
		const FLinearColor& Ink, const FLinearColor& Outline, bool bMask, float TextScale = 1.f);
	/** (user 15 Oct) every other prompt: each part's label centred on the line at Centre.Y, its drawing centred under the label;
	 *  two or more parts side by side, the whole centred on Centre.X (a bRight part's drawing beside its label instead).
	 *  Returns the box of everything drawn. */
	FBox2D DrawStacked(UCanvas* C, const TArray<FPart>& Parts, UFont* Font, const FVector2D& Centre, float GlyphH,
		const FLinearColor& Ink, const FLinearColor& Outline, bool bMask, float TextScale = 1.f, TArray<FBox2D>* OutPartBoxes = nullptr);
	/** DrawStacked's width, and how far under the line's middle its drawings reach. */
	float StackedWidth(UCanvas* C, const TArray<FPart>& Parts, UFont* Font, float GlyphH, float TextScale = 1.f);
	float StackedBelow(UCanvas* C, UFont* Font, float GlyphH, float TextScale = 1.f);

	/** A prompt line's leading letter pair ("A / X: restart level", "B or Y: back") read as its glyph and the rest of the
	 *  line, first letter capitalised; false when the line has none. */
	bool SplitPrompt(const AOutlawsPlayer* P, const FString& Line, FGlyph& OutGlyph, FString& OutRest);
}

/** One row of a glyph plate: a glyph (may be empty) and its text, drawn under the text (bBeside: beside it, an instruction
 *  line's); More: further prompts on the same line, side by side. */
struct FOutlawsGlyphRow
{
	OutlawsGlyphs::FGlyph Glyph;
	FString Text;
	bool bTitle = false;
	bool bBeside = false;
	bool bRight = false;               // (REQ-BACKRIGHT) the row's own part drawn as a bRight part
	TArray<OutlawsGlyphs::FPart> More;
};

/**
 * REQ-BUTTONGLYPHS: a small sheet in the world (the death screen, the tutorial's card) with rows of glyph and text drawn on
 * a canvas and shown through the menu's overlay material.  It redraws only when its rows change.
 */
UCLASS()
class UOutlawsGlyphPlate : public UStaticMeshComponent
{
	GENERATED_BODY()
public:
	UOutlawsGlyphPlate();
	/** Sets up the canvases and the material (once); GlyphCm: the drawings' height in the plate's own cm. */
	void Init(UMaterialInterface* Overlay, UFont* InFont, UFont* InTitleFont, float InWidthCm, int32 InPixH, float InGlyphCm);
	/** The rows to show; returns true when they changed (and were drawn). */
	bool SetRows(const TArray<FOutlawsGlyphRow>& InRows);
	/** Faces +X of Frame toward the viewer (as a text render's front), centred on At; bRelative: in the parent's space. */
	void PlaceFacing(const FVector& At, const FQuat& Frame, bool bRelative = false);
	const TArray<FOutlawsGlyphRow>& GetRows() const { return Rows; }
	float HeightCm() const { return HeightCmNow; }
	float WidthCmNow() const { return WidthCm; }
	float GlyphPx() const { return GlyphCm * PixW / WidthCm; }      // (the drawings' height on the plate, pixels)
	int32 PixWide() const { return PixW; }
	/** A stacked part's box as last drawn, plate pixels (invalid when there is none). */
	FBox2D PartBox(int32 Row, int32 Part) const { return PartBoxes.IsValidIndex(Row) && PartBoxes[Row].IsValidIndex(Part) ? PartBoxes[Row][Part] : FBox2D(ForceInit); }
	/** A row's parts' boxes together, plate pixels. */
	FBox2D RowBox(int32 Row) const { FBox2D B(ForceInit); if (PartBoxes.IsValidIndex(Row)) for (const FBox2D& P : PartBoxes[Row]) if (P.bIsValid) B += P; return B; }
	/** (BUG-MENUGLYPH2) every row's boxes together, plate pixels. */
	FBox2D InkBox() const { FBox2D B(ForceInit); for (int32 r = 0; r < PartBoxes.Num(); ++r) { const FBox2D R = RowBox(r); if (R.bIsValid) B += R; } return B; }
	/** (REQ-BACKCLICK) the stacked part (0 the row's own, then its More) drawn under a point given as 0..1 across and down the
	 *  plate, as last drawn, PadPx around each; false when none is there. */
	bool PartAt(const FVector2D& Uv, int32& OutRow, int32& OutPart, float PadPx = 0.f) const;
	/** The middle of a stacked part as last drawn, 0..1 across and down; false when there is none (the tests' aim). */
	bool PartCentreUv(int32 Row, int32 Part, FVector2D& OutUv) const;
	/** (REQ-DEATHGAP) when set, a title row's letters stand this many plate cm over the next row's letters (given the canvas
	 *  the plate draws on, for the fonts' sizes); unset, the title keeps its own row height. */
	TFunction<float(UCanvas*)> TitleGapCm;
	/** The gap from the title's letters to the next row's as last drawn, plate cm; -1 when there is none. */
	float DrawnTitleGapCm() const { return LastTitleGapCm; }

	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> Tex;
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> Mask;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> Mid;
	UPROPERTY(Transient)
	TObjectPtr<UFont> Font;
	UPROPERTY(Transient)
	TObjectPtr<UFont> TitleFont;
private:
	void Redraw();
	TArray<FOutlawsGlyphRow> Rows;
	TArray<TArray<FBox2D>> PartBoxes;     // (each row's stacked parts as last drawn, plate pixels)
	float WidthCm = 60.f;
	float GlyphCm = 10.f;
	float HeightCmNow = 10.f;
	float LastTitleGapCm = -1.f;
	int32 PixW = 1024, PixH = 256;
};
