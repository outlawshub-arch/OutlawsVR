// REQ-EXTRAS (user 13 Oct, v1.1): the main menu's Extras and its pages, modelled on the remaster's own (extras.rml,
// achievements.rml, menu.rcss):
//   #extras         the title pages' parchment: Achievements, Collectibles, Credits (REQ-NOUNLOCKABLES, user 27 Oct: no
//                   Unlockables page; the Randomizer and the Bad guys multiplier are open from the start, the shelf has the boxes)
//   #extras_achiev  the remaster's achievements page on the save page's wide sheet: its parchment (Achiev_Base), the capsules in a
//                   scroll list (AchievM_capsule*, the earned one's icon Achiev_IconNN), "???" for a hidden one not yet earned,
//                   the focused one's description, progress and date in the panel under it, the frame (Achiev_Frame) over them
//   #extras_shelf   the collectible boxes' shelf (REQ-COLLECTBOX fills it later): an outline for each story level's box
// The art is the player's own (UI.kpf), read at runtime as every other page's; without it the plain colours stand in.
// Credits plays the end credits roll (OutlawsLevelActorCredits.cpp) on the black, then comes back here.
#include "OutlawsPlayer.h"
#include "OutlawsAchievements.h"
#include "OutlawsUnlocks.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsLevelActor.h"
#include "Engine/Canvas.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "CanvasItem.h"
#include "Components/TextRenderComponent.h"

namespace
{
	// the achievements page's layout in the 1920 x 1080 view (menu.rcss: the frame at 953, 238; the list inside it 12 in; the
	// description panel 58 up from the foot), all lifted 44 so the prompts fit under the frame at the sheet's bottom right
	constexpr float AchLift = 44.f;
	// BUG-ACHIEVBOX (user 3 Nov, the user's snapshot #5: the frame's top covered the lower flourish under the title and the
	// "N of M earned" line sat on the title's foot): the frame and all in it 0.92 the size about its bottom-right corner (1793,
	// 990), its top from 194 down to 258 under the flourish (which ends about 217 in the drawn page; 203 measured from the
	// snapshot left the count line on its foot), the count line down to 236 between them; -OLOldExtrasLayout: as it was
	constexpr float AchScale = 0.92f, AchPinX = 1793.f, AchPinY = 990.f;
	struct FAchLayout
	{
		float FrameX = 953.f, FrameY = 238.f - AchLift, FrameW = 840.f, FrameH = 796.f;
		float ListX = 965.f, ListY = 250.f - AchLift, ListW = 816.f, ListH = 622.f;
		float DescX = 965.f, DescY = 1080.f - 58.f - 148.f - AchLift, DescW = 816.f, DescH = 148.f;
		float CapW = 748.f, CapH = 144.f, CapStep = 147.f, CapInX = 16.f, CapInY = 8.f + 3.f;
		float IconX = 20.f, IconY = 19.f, Icon = 102.f, TitleX = 132.f + 14.f, TitleMax = 560.f;
		float ThumbW = 48.f, ThumbH = 132.f, ThumbIn = 44.f, DescIn = 28.f;
		float CountY = 176.f, ToastY = 212.f;
		float CapX() const { return ListX + CapInX; }
		float CapY() const { return ListY + CapInY; }
	};

	const FAchLayout& Ach()
	{
		static const FAchLayout L = []()
		{
			FAchLayout A;
			if (OLFlags::Param(TEXT("OLOldExtrasLayout"))) return A;
			auto X = [](float V) { return AchPinX - (AchPinX - V) * AchScale; };
			auto Y = [](float V) { return AchPinY - (AchPinY - V) * AchScale; };
			A.FrameX = X(A.FrameX); A.FrameY = Y(A.FrameY); A.ListX = X(A.ListX); A.ListY = Y(A.ListY); A.DescX = X(A.DescX); A.DescY = Y(A.DescY);
			for (float* V : { &A.FrameW, &A.FrameH, &A.ListW, &A.ListH, &A.DescW, &A.DescH, &A.CapW, &A.CapH, &A.CapStep, &A.CapInX, &A.CapInY,
				&A.IconX, &A.IconY, &A.Icon, &A.TitleX, &A.TitleMax, &A.ThumbW, &A.ThumbH, &A.ThumbIn, &A.DescIn }) *V *= AchScale;
			A.CountY = 236.f;
			A.ToastY = 236.f;           // (a toast takes the count's line while it shows: under the title it fell on the count)
			return A;
		}();
		return L;
	}
}

void AOutlawsPlayer::AchievFrameBox(float& OutX, float& OutY, float& OutW, float& OutH)
{
	const FAchLayout& A = Ach();
	OutX = A.FrameX; OutY = A.FrameY; OutW = A.FrameW; OutH = A.FrameH;
}

FString AOutlawsPlayer::MenuLoc(const FString& Key)
{
	if (!LoadMenuSource() || !MenuSource) return FString();
	const FString K = Key.StartsWith(TEXT("$")) ? Key : TEXT("$") + Key;
	const FString V = MenuSource->Loc(K);
	return V.IsEmpty() || V.StartsWith(TEXT("$")) ? FString() : V;
}

void AOutlawsPlayer::ShowExtrasPage()
{
	// (after Extras > Credits: the main menu is up again; its unlock pages, if any, first)
	if (!bMenuOpen || !MenuPage || MenuPage->File != TEXT("#front")) return;
	ShowMenuPage(TEXT("#extras"), true);
}

bool AOutlawsPlayer::IsAchievPage() const
{
	return MenuPage && MenuPage->File == TEXT("#extras_achiev");
}

void AOutlawsPlayer::AchievListBox(float& OutX, float& OutY, float& OutW, float& OutH)
{
	const FAchLayout& A = Ach();
	OutX = A.ListX; OutY = A.ListY; OutW = A.ListW; OutH = A.ListH;
}

bool AOutlawsPlayer::BuildExtrasPage(const FString& File, OutlawsMenu::FPage& Page)
{
	using namespace OutlawsMenu;
	Page.File = File;
	auto Loc = [this](const TCHAR* Key, const TCHAR* Or) { const FString S = MenuLoc(Key); return S.IsEmpty() ? FString(Or) : S; };
	auto Add = [&Page](EItemKind Kind, const FString& Label, const FString& Name, const FString& Target, const TCHAR* Classes = TEXT(""))
	{
		FItem I;
		I.Kind = Kind;
		I.Label = Label;
		I.Name = Name;
		I.Target = Target;
		I.Classes = Classes;
		Page.Items.Add(I);
	};
	const AOutlawsLevelActor* L = Level.Get();
	if (File == TEXT("#extras"))
	{
		Page.Title = TEXT("EXTRAS");
		Add(EItemKind::Link, Loc(TEXT("m_achievements"), TEXT("Achievements")), TEXT(""), TEXT("#extras_achiev"));
		Add(EItemKind::Link, TEXT("Collectibles"), TEXT(""), TEXT("#extras_shelf"));
		Add(EItemKind::Action, Loc(TEXT("m_credits"), TEXT("Credits")), TEXT("extras_credits"), TEXT(""));
		return true;
	}
	if (File == TEXT("#extras_achiev"))
	{
		Page.Title = Loc(TEXT("m_achievements_UC"), TEXT("ACHIEVEMENTS"));
		for (const OutlawsAchievements::FDef& D : OutlawsAchievements::All())
		{
			const bool bEarned = OutlawsAchievements::IsEarned(L, D.Id);
			Add(EItemKind::Action, !bEarned && D.bHidden ? FString(TEXT("???")) : OutlawsAchievements::TitleOf(D, this), FString(TEXT("ach:")) + D.Id, TEXT(""), bEarned ? TEXT("earned") : TEXT(""));
		}
		AchievFocus = 0;
		UE_LOG(LogOutlaws, Log, TEXT("OLEXTRAS achievements page: %d of %d earned (profile %s)"), OutlawsAchievements::EarnedCount(L), Page.Items.Num(), *OutlawsUnlocks::ProfileOf(L));
		return true;
	}
	if (File == TEXT("#extras_shelf"))
	{
		// one box a story level (OutlawsAchievements::BoxLevels): its level's name once found, an outline until then
		Page.Title = TEXT("COLLECTIBLES");
		TArray<FString> Levels, Labels;
		OutlawsAchievements::BoxLevels(Levels, Labels);
		for (int32 i = 0; i < Levels.Num(); ++i)
		{
			const bool bFound = OutlawsAchievements::BoxFound(L, Levels[i]);
			Add(EItemKind::Action, bFound ? Labels[i] : FString(TEXT("?")), TEXT("box:") + Levels[i], TEXT(""), bFound ? TEXT("") : TEXT("disabled"));
		}
		return true;
	}
	return false;
}

bool AOutlawsPlayer::ExtrasPress(const OutlawsMenu::FItem& I, int32 Index)
{
	if (I.Name.StartsWith(TEXT("ach:")))
	{
		AchievFocus = Index;                        // (a capsule pressed: its description, as pointing at it shows)
		RedrawMenu();
		return true;
	}
	if (I.Name == TEXT("extras_credits"))
	{
		AOutlawsLevelActor* L = Level.Get();
		if (!L || !L->PlayExtrasCredits()) MenuToastText(TEXT("No credits to roll"));
		return true;
	}
	if (I.Name.StartsWith(TEXT("box:")))
	{
		MenuToastText(FString::Printf(TEXT("Found in %s"), *I.Label));
		return true;
	}
	return false;
}

int32 AOutlawsPlayer::AchievRowAt(double Px, double Py) const
{
	const FAchLayout& A = Ach();
	if (!MenuPage || Px < A.CapX() || Px >= A.CapX() + A.CapW || Py < A.ListY || Py >= A.ListY + A.ListH) return -1;
	const int32 r = int32((Py - A.CapY()) / A.CapStep);
	if (Py < A.CapY() || r < 0 || r >= AchievRowsShown || Py >= A.CapY() + r * A.CapStep + A.CapH) return -1;
	return MenuPage->Items.IsValidIndex(MenuScroll + r) ? r : -1;
}

void AOutlawsPlayer::DrawAchievCanvas(UCanvas* C, int32 Width, int32 Height)
{
	const float K = Width / 1920.f;
	if (bMenuMaskPass)
	{
		FCanvasTileItem All(FVector2D(0.f, 0.f), GWhiteTexture, FVector2D(Width, Height), FLinearColor::White);
		All.BlendMode = SE_BLEND_Opaque;
		C->DrawItem(All);
		return;
	}
	auto Col = [](uint8 R, uint8 G, uint8 B) { return FLinearColor::FromSRGBColor(FColor(R, G, B)); };
	auto Tile = [&](UTexture2D* T, float X, float Y, float W, float H, FVector2D UV0 = FVector2D(0.f, 0.f), FVector2D UV1 = FVector2D(1.f, 1.f)) -> bool
	{
		if (!T || !T->GetResource()) return false;
		FCanvasTileItem I(FVector2D(X * K, Y * K), T->GetResource(), FVector2D(W * K, H * K), UV0, UV1, FLinearColor::White);
		I.BlendMode = SE_BLEND_Translucent;
		C->DrawItem(I);
		return true;
	};
	auto Fill = [&](float X, float Y, float W, float H, const FLinearColor& Colour)
	{
		FCanvasTileItem I(FVector2D(X * K, Y * K), GWhiteTexture, FVector2D(W * K, H * K), Colour);
		I.BlendMode = SE_BLEND_Opaque;
		C->DrawItem(I);
	};
	auto Txt = [&](UFont* F, const FString& Str, float X, float Y, int32 Align, const FLinearColor& Colour, float MaxW, bool bShadow = false) -> FBox2D
	{
		if (!F || Str.IsEmpty()) return FBox2D(ForceInit);
		FBox2D Glyphs;
		if (Align == 1 && DrawGlyphLine(C, Str, F, FVector2D(X * K, Y * K), Colour, FLinearColor::Black, bMenuMaskPass, Glyphs)) return FBox2D(Glyphs.Min / K, Glyphs.Max / K);
		float XL = 0.f, YL = 0.f;
		C->TextSize(F, Str, XL, YL);
		const float Sc = MaxW > 0.f && XL > MaxW * K ? MaxW * K / XL : 1.f;
		float X0 = X * K;
		if (Align == 1) X0 -= XL * Sc * 0.5f;
		else if (Align == 2) X0 -= XL * Sc;
		FCanvasTextItem T(FVector2D(X0, Y * K), FText::FromString(Str), F, Colour);
		T.Scale = FVector2D(Sc, Sc);
		T.bCentreY = true;
		T.BlendMode = SE_BLEND_Translucent;
		if (bShadow) T.EnableShadow(FLinearColor::Black, FVector2D(3.f * K, 2.f * K));
		C->DrawItem(T);
		return FBox2D(FVector2D(X0 / K, Y - YL * Sc * 0.5f / K), FVector2D((X0 + XL * Sc) / K, Y + YL * Sc * 0.5f / K));
	};
	// a line broken into lines no wider than MaxW (view units)
	auto Wrap = [&](UFont* F, const FString& Str, float MaxW)
	{
		TArray<FString> Words, Out;
		Str.ParseIntoArrayWS(Words);
		FString Line;
		for (const FString& W : Words)
		{
			const FString Try = Line.IsEmpty() ? W : Line + TEXT(" ") + W;
			float XL = 0.f, YL = 0.f;
			if (F) C->TextSize(F, Try, XL, YL);
			if (!Line.IsEmpty() && XL / K > MaxW) { Out.Add(Line); Line = W; }
			else Line = Try;
		}
		if (!Line.IsEmpty()) Out.Add(Line);
		return Out;
	};

	// the parchment (Achiev_Base, 2560 x 1440 centred), else the save page's plain colour
	if (!Tile(KpfArt(TEXT("Achiev_Base")), 0.f, 0.f, 1920.f, 1080.f, FVector2D(320.f / 2560.f, 180.f / 1440.f), FVector2D(2240.f / 2560.f, 1260.f / 1440.f)))
		Fill(0.f, 0.f, 1920.f, 1080.f, Col(0x9a, 0x6a, 0x30));
	const FLinearColor Ink = Col(0x1d, 0x08, 0x00);
	const AOutlawsLevelActor* L = Level.Get();
	const TArray<OutlawsAchievements::FDef>& Defs = OutlawsAchievements::All();
	const int32 N = MenuPage->Items.Num();
	// menu_title.options: centred at the top, between the parchment's two flourishes; how many are earned under it (the toast
	// takes that line while it shows, BUG-ACHIEVBOX)
	const FAchLayout& A = Ach();
	const bool bToast = MenuToast && !MenuToast->Text.IsEmpty();
	const bool bToastOnCount = bToast && A.ToastY == A.CountY;
	AchievTitleInk = Txt(SaveFont(0), MenuPage->Title, 960.f, 116.f, 1, Ink, 1100.f);
	AchievCountInk = FBox2D(ForceInit);
	if (!bToastOnCount) AchievCountInk = Txt(SaveFont(2), FString::Printf(TEXT("%d of %d earned"), OutlawsAchievements::EarnedCount(L), Defs.Num()), 960.f, A.CountY, 1, Ink, 600.f);

	// the list: four whole capsules (menu.rcss: 748 x 144, 3 apart), pointed at HOVER, the one described FOCUS
	const int32 Hot = MenuHover[1] >= 0 ? MenuHover[1] : MenuHover[0];
	if (Hot >= 0 && MenuPage->Items.IsValidIndex(MenuScroll + Hot)) AchievFocus = MenuScroll + Hot;     // (focus follows the pointer, as the remaster's)
	AchievFocus = FMath::Clamp(AchievFocus, 0, FMath::Max(0, N - 1));
	Fill(A.ListX, A.ListY, A.ListW, A.ListH, Col(0x40, 0x10, 0x00));
	for (int32 r = 0; r < AchievRowsShown; ++r)
	{
		const int32 i = MenuScroll + r;
		if (!MenuPage->Items.IsValidIndex(i) || !Defs.IsValidIndex(i)) break;
		const OutlawsAchievements::FDef& D = Defs[i];
		const bool bEarned = MenuPage->Items[i].Classes.Contains(TEXT("earned"));
		const bool bHover = r == MenuHover[0] || r == MenuHover[1];
		const float X0 = A.CapX(), Y0 = A.CapY() + A.CapStep * r;
		const FString Art = FString::Printf(TEXT("AchievM_capsule%s_%s"), bEarned ? TEXT("UNLOCK") : TEXT("LOCK"), bHover ? TEXT("HOVER") : i == AchievFocus ? TEXT("FOCUS") : TEXT("INACTIVE"));
		if (!Tile(KpfArt(Art), X0, Y0, A.CapW, A.CapH))
		{
			// (no remaster: a plain capsule, lighter when earned, its edge orange when pointed at or described)
			Fill(X0, Y0, A.CapW, A.CapH, bHover || i == AchievFocus ? Col(0xe9, 0x93, 0x1c) : Col(0x20, 0x08, 0x00));
			Fill(X0 + 4.f, Y0 + 4.f, A.CapW - 8.f, A.CapH - 8.f, bEarned ? Col(0x6a, 0x40, 0x18) : Col(0x30, 0x14, 0x06));
		}
		// the earned one's picture (menu_achievement_pic: 102 x 102 at 20, 19); ours have none of the remaster's
		if (bEarned && D.Icon > 0) Tile(KpfArt(FString::Printf(TEXT("Achiev_Icon%02d"), D.Icon)), X0 + A.IconX, Y0 + A.IconY, A.Icon, A.Icon);
		else if (bEarned) Tile(KpfArt(TEXT("AchievS_Unlocked@2x")), X0 + A.IconX + A.Icon * 0.108f, Y0 + A.IconY + A.Icon * 0.108f, A.Icon * 0.784f, A.Icon * 0.784f);
		// menu_achievement_title: from 132 (14 padding), white
		Txt(SaveFont(5), MenuPage->Items[i].Label, X0 + A.TitleX, Y0 + A.CapH * 0.5f, 0, bEarned || bHover ? FLinearColor::White : Col(0xa8, 0xa0, 0x96), A.TitleMax);
	}
	// the scroll thumb down the frame's right-hand groove
	if (N > AchievRowsShown)
	{
		const float Track = A.ListH - A.ThumbH;
		Tile(KpfArt(TEXT("Load_SliderV")), A.ListX + A.ListW - A.ThumbIn, A.ListY + Track * float(MenuScroll) / float(FMath::Max(1, N - AchievRowsShown)), A.ThumbW, A.ThumbH);
	}
	// menu_achiev_desc: the focused one's line, its progress and the day it was earned
	Fill(A.DescX, A.DescY, A.DescW, A.DescH, Col(0x40, 0x10, 0x00));
	if (Defs.IsValidIndex(AchievFocus))
	{
		const OutlawsAchievements::FDef& D = Defs[AchievFocus];
		const bool bEarned = OutlawsAchievements::IsEarned(L, D.Id);
		TArray<FString> Lines = Wrap(SaveFont(2), !bEarned && D.bHidden ? FString(TEXT("Hidden until earned")) : OutlawsAchievements::DescOf(D, this), A.DescW - 2.f * A.DescIn);
		TArray<FLinearColor> Colours;
		for (int32 k = 0; k < Lines.Num(); ++k) Colours.Add(FLinearColor::White);
		const FString Prog = OutlawsAchievements::Progress(L, D.Id);
		if (!Prog.IsEmpty() && !bEarned) { Lines.Add(Prog); Colours.Add(Col(0xf2, 0xaa, 0x44)); }
		const FString When = OutlawsAchievements::EarnedLine(L, D.Id);
		if (!When.IsEmpty()) { Lines.Add(When); Colours.Add(Col(0xf2, 0xaa, 0x44)); }
		const float Step = 34.f;
		float Y = A.DescY + A.DescH * 0.5f - Step * (Lines.Num() - 1) * 0.5f;
		for (int32 k = 0; k < Lines.Num() && k < 4; ++k, Y += Step) Txt(SaveFont(2), Lines[k], A.DescX + A.DescIn, Y, 0, Colours[k], A.DescW - 2.f * A.DescIn);
	}
	Tile(KpfArt(TEXT("Achiev_Frame")), A.FrameX, A.FrameY, A.FrameW, A.FrameH);

	// the prompts inside the sheet at its bottom right, as every wide page's (BUG-MENUGLYPH2); a toast under the title
	WideHelpInk = FBox2D(ForceInit);
	if (bVR) UpdateMenuHelpPlate(TEXT("Trigger  Select        B / Y  Back"));
	else WideHelpInk = Txt(SaveFont(5), TEXT("Click  Select        Esc  Back"), WideParchmentRight() - MenuHelpInsetCm * 1920.f / SheetW, 1030.f, 2, Col(0xc0, 0xc0, 0xc0), 746.f, true);
	AchievToastInk = FBox2D(ForceInit);
	if (bToast) AchievToastInk = Txt(SaveFont(2), MenuToast->Text.ToString(), 960.f, A.ToastY, 1, Col(0xf2, 0xaa, 0x44), 900.f, true);
}
