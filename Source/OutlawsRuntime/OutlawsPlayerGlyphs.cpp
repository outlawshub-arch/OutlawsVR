// REQ-BUTTONGLYPHS: the player's VR prompts drawn as the controller with the button to press marked (OutlawsGlyphs.h), and
// the -OLGlyphTest run that photographs every prompt per settings layout and checks each mark against the bound input.

#include "OutlawsPlayer.h"
#include "OutlawsFlags.h"
#include "OutlawsGlyphs.h"
#include "OutlawsCoop.h"
#include "OutlawsLevelActor.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsRuntime.h"
#include "Camera/CameraComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

using namespace OutlawsGlyphs;

UFont* AOutlawsPlayer::GlyphFont(bool bTitle)
{
	if (bTitle && MenuTitleFont) return MenuTitleFont;
	if (!bTitle && MenuRowFont) return MenuRowFont;
	UFont* F = KpfFont(bTitle ? TEXT("fonts/Rye-Regular.ttf") : TEXT("fonts/MontaguSlab_24pt-Bold.ttf"), bTitle ? 64 : 36);
	return F ? F : (GEngine ? GEngine->GetLargeFont() : nullptr);
}

bool AOutlawsPlayer::UpdateGlyphPrompt(UTextRenderComponent* Over, const FString& InText, bool bShow)
{
	const FString& Text = GlyphTestPrompt.IsEmpty() ? InText : GlyphTestPrompt;
	TArray<FOutlawsGlyphRow> Rows;
	bool bAny = false;
	if (bVR && Over)
	{
		TArray<FString> Lines;
		Text.ParseIntoArray(Lines, TEXT("\n"), true);
		for (int32 i = 0; i < Lines.Num(); ++i)
		{
			FOutlawsGlyphRow R;
			if (SplitPrompt(this, Lines[i], R.Glyph, R.Text)) bAny = true;
			else
			{
				R.Text = Lines[i];
				R.bTitle = i == 0;
			}
			Rows.Add(R);
		}
	}
	if (!bAny)
	{
		if (DeathPlate) DeathPlate->SetVisibility(false);
		return false;
	}
	if (!DeathPlate)
	{
		DeathPlate = NewObject<UOutlawsGlyphPlate>(this);
		DeathPlate->SetupAttachment(VROrigin);
		DeathPlate->RegisterComponent();
		DeathPlate->Init(LoadOverlayMaterial(TEXT("M_OLR_MenuOverlay")), GlyphFont(false), GlyphFont(true), 110.f, 640, ViewTan * 140.f);   // (140 cm away)
		// (REQ-DEATHGAP, user 15 Oct) the heading as far over the first prompt as a sheet's title is over its first row, seen alike
		DeathPlate->TitleGapCm = [this](UCanvas* C) { return SheetTitleGapSeenCm(C, 140.f); };
	}
	if (DeathPlate->SetRows(Rows))
	{
		FString Said;
		for (const FOutlawsGlyphRow& R : Rows) Said += FString::Printf(TEXT(" [%s: %s]"), *R.Glyph.Describe(), *R.Text);
		UE_LOG(LogOutlaws, Log, TEXT("OLGLYPH death prompt:%s; heading to first line %.2f cm at 140 cm"), *Said, DeathPlate->DrawnTitleGapCm());
	}
	DeathPlate->SetVisibility(bShow);
	if (bShow) DeathPlate->PlaceFacing(Over->GetComponentLocation(), Over->GetComponentQuat());
	return true;
}

void AOutlawsPlayer::UpdateHudGlyph(float Dt)
{
	// (REQ-TUTORIAL2: the card panel's control under the panel; the glyph test's cards still stand as notice lines)
	const bool bOnCard = bVR && Camera && !bMenuOpen && !bDeadView && bTutCardUp && !TutGlyph.Glyph.IsEmpty();
	const bool bOn = bOnCard || (bVR && HudText && Camera && !bMenuOpen && !bDeadView && !TutGlyphLine.IsEmpty() && !TutGlyph.Glyph.IsEmpty() && HudLines.Contains(TutGlyphLine));
	if (!bOn)
	{
		if (HudPlate) HudPlate->SetVisibility(false);
		return;
	}
	const float WidthCm = 44.f;
	if (!HudPlate)
	{
		HudPlate = NewObject<UOutlawsGlyphPlate>(this);
		HudPlate->SetupAttachment(Camera);
		HudPlate->RegisterComponent();
		HudPlate->SetDepthPriorityGroup(SDPG_Foreground);
		HudPlate->Init(LoadOverlayMaterial(TEXT("M_OLR_MenuOverlay")), GlyphFont(false), GlyphFont(true), WidthCm, 240, ViewTan * 80.f);   // (laid out 80 cm away, scaled with the notices)
	}
	if (HudPlate->SetRows({ TutGlyph })) UE_LOG(LogOutlaws, Log, TEXT("OLGLYPH tutorial card: %s, \"%s\""), *TutGlyph.Glyph.Describe(), *TutGlyph.Text);
	// just under the notices (their lines stand on 10 degrees over the middle of the view and grow upward), as near as they are
	const float Along = bOnCard ? TutCardDist : HudDist;
	const float K = Along / 80.f;
	const float Hcm = HudPlate->HeightCm();
	const double Tan10 = FMath::Tan(FMath::DegreesToRadians(10.0));
	const double Under = bOnCard ? double(TutCardBottomZ) : HudDist * Tan10;
	HudPlate->PlaceFacing(FVector(Along, 0.0, Under - (Hcm * 0.5 + 0.6) * K), FRotator(0.0, 180.0, 0.0).Quaternion(), true);
	HudPlate->SetRelativeScale3D(FVector(WidthCm / 100.0 * K, Hcm / 100.0 * K, 1.0));
	HudPlate->SetVisibility(true);
}

FOutlawsGlyphRow AOutlawsPlayer::TutorialGlyph(const FString& CardId) const
{
	// the control each card asks for, by the same rules the game's handlers use (PressA / PressX open that hand's wheel; Y / B
	// reload in the default layout, a tap of the wheel button in Classic and with one controller; the one controller's upper
	// button jumps; the turning stick's push up jumps)
	const int32 One = OneHand();
	const int32 Dom = One >= 0 ? One : (LeftDominant() ? 0 : 1);
	const int32 Off = 1 - Dom;
	auto Lower = [this](int32 H) { return ForActions(this, { H == 0 ? TEXT("OLVR_X") : TEXT("OLVR_A") }); };
	auto Upper = [this](int32 H) { return ForActions(this, { H == 0 ? TEXT("OLVR_Y") : TEXT("OLVR_B") }); };
	const int32 MoveHand = One >= 0 ? One : (LeftDominant() ? 1 : 0), TurnHand = One >= 0 ? One : (LeftDominant() ? 0 : 1);
	FOutlawsGlyphRow R;
	R.bBeside = true;          // (an instruction line: the card's words say what to do, the drawing and its short label beside each other)
	if (CardId == TEXT("take_revolver")) { R.Glyph = Lower(Dom); R.Text = TEXT("Weapon wheel"); }
	else if (CardId == TEXT("fire")) { R.Glyph = ForControl(this, ECtl::Trigger, Dom); R.Text = TEXT("Fire"); }
	else if (CardId == TEXT("reload"))
	{
		if (bFaceReload && One < 0) { R.Glyph = Upper(Dom); R.Text = TEXT("Reload"); }
		else { R.Glyph = Lower(Dom); R.Text = TEXT("Tap to reload"); }
	}
	else if (CardId == TEXT("take_knife")) { R.Glyph = Lower(One >= 0 ? Dom : Off); R.Text = TEXT("Weapon wheel"); }
	else if (CardId == TEXT("throw_knife") || CardId == TEXT("throw_dynamite")) { R.Glyph = ForControl(this, ECtl::Trigger, Dom); R.Text = TEXT("Hold, aim, let go"); }
	// (REQ-TUTORIAL2: the hand named on every card: the dynamite and the lantern from the other hand's wheel, the bag from the
	// main hand's)
	else if (CardId == TEXT("take_dynamite") || CardId == TEXT("lantern")) { R.Glyph = Lower(One >= 0 ? Dom : Off); R.Text = TEXT("Weapon wheel"); }
	else if (CardId == TEXT("heal")) { R.Glyph = Lower(Dom); R.Text = TEXT("Weapon wheel"); }
	else if (CardId == TEXT("pickup")) { R.Glyph = ForControl(this, ECtl::Grip, One >= 0 ? Dom : Off); R.Text = TEXT("Grip"); }
	else if (CardId == TEXT("pause")) { R.Glyph = ForActions(this, { TEXT("OLVR_Menu") }); R.Text = TEXT("Menu"); }
	else if (CardId == TEXT("walk")) { R.Glyph = ForControl(this, ECtl::Stick, MoveHand); R.Text = TEXT("Walk"); }
	else if (CardId == TEXT("turn")) { R.Glyph = ForControl(this, ECtl::Stick, TurnHand); R.Text = TEXT("Turn"); }
	else if (CardId == TEXT("jump"))
	{
		if (One >= 0) { R.Glyph = Upper(One); R.Text = TEXT("Jump"); }
		else { R.Glyph = ForControl(this, ECtl::Stick, TurnHand); R.Text = TEXT("Down: crouch. Up: jump"); }
	}
	else if (CardId == TEXT("climb") || CardId == TEXT("grab") || CardId == TEXT("horse")) { R.Glyph = ForControl(this, ECtl::Grip, -1); R.Text = TEXT("Grip"); }
	else if (CardId == TEXT("punch"))
	{
		// (REQ-PUNCHGRIP) the grip of the punching hand: the one not holding him, else the main hand (the left for left-handed
		// players); one controller has no punch card
		const int32 Fist = GrabbedEnemy[Dom].IsValid() && !GrabbedEnemy[Off].IsValid() ? Off : Dom;
		R.Glyph = ForControl(this, ECtl::Grip, Fist);
		R.Text = TEXT("Hold for a fist");
	}
	else if (CardId == TEXT("fight") || CardId == TEXT("grab_shoot")) { R.Glyph = ForControl(this, ECtl::Trigger, -1); R.Text = TEXT("Fire"); }
	return R;
}

bool AOutlawsPlayer::GlyphLineParts(const FString& Str, TArray<FPart>& Parts) const
{
	Parts.Reset();
	if (!bVR) return false;
	FGlyph G;
	FString Rest;
	if (Str == TEXT("more below") || Str == TEXT("more above")) Parts.Add({ ScrollGlyph(), Str == TEXT("more below") ? TEXT("More below") : TEXT("More above") });
	else if (Str.StartsWith(TEXT("Trigger  Select")) || Str.StartsWith(TEXT("Point and pull the trigger to select")))
	{
		Parts.Add({ SelectGlyph(), TEXT("Select") });
		if (Str.EndsWith(TEXT("B / Y  Back"))) Parts.Add({ BackGlyph(), TEXT("Back") });
	}
	else if (SplitPrompt(this, Str, G, Rest)) Parts.Add({ G, Rest });
	// (REQ-BACKRIGHT, user 15 Oct) Back's controllers to the right of the word, the two centred as one; the others keep theirs under
	for (FPart& Pt : Parts) Pt.bRight = Pt.Label == TEXT("Back");
	return Parts.Num() > 0;
}

float AOutlawsPlayer::GlyphLineWidth(UCanvas* C, const FString& Str, UFont* F) const
{
	TArray<FPart> Parts;
	if (!C || !F || !GlyphLineParts(Str, Parts)) return -1.f;
	return StackedWidth(C, Parts, F, MenuGlyphPx(C));
}

bool AOutlawsPlayer::DrawGlyphLine(UCanvas* C, const FString& Str, UFont* F, const FVector2D& Centre, const FLinearColor& Ink, const FLinearColor& Outline,
	bool bMask, FBox2D& Out)
{
	// (user 15 Oct) the words centred on the line, the controller under them, the size the death screen's are seen at
	TArray<FPart> Parts;
	if (!C || !F || !GlyphLineParts(Str, Parts)) return false;
	Out = DrawStacked(C, Parts, F, Centre, MenuGlyphPx(C), Ink, Outline, bMask);
	return true;
}

// ---- -OLShots -OLGlyphTest=<layout> --------------------------------------------------------------------------------------
// Each scene is set up, photographed a second later (the shot is taken at the end of that frame, so nothing changes in it),
// and checked: every mark's key must fire an action the prompt's handler reads, on a hand that is held, and every held hand
// that can answer must be drawn.  The menu's Back is also pressed for real on the ringed key.

namespace
{
	struct FGlyphTest
	{
		FString Layout;
		int32 Scene = -1;
		int32 Phase = 0;
		double Next = 0.0;
		int32 Pass = 0, Fail = 0;
		TArray<FString> Shots;
		FString ScrollPage;
		FKey BackKey;
		float SheetGapAt140 = -1.f;     // (REQ-DEATHGAP) the Options sheet's title-to-first-row gap as drawn, seen at 140 cm
	};
	FGlyphTest GT;
	const TCHAR* const TutCards[] = { TEXT("take_revolver"), TEXT("fire"), TEXT("reload"), TEXT("take_knife"), TEXT("walk"), TEXT("turn"), TEXT("jump"),
		TEXT("grab"), TEXT("punch"), TEXT("heal") };
	constexpr int32 NumTut = UE_ARRAY_COUNT(TutCards);
	// the scenes: 0 menu Back, 1 more below, 2 more above, 3 save screen, 4 title-style page, then the cards, then three death screens
	constexpr int32 SceneTut0 = 5, SceneDeath0 = SceneTut0 + NumTut, NumScenes = SceneDeath0 + 3;
}

#if WITH_OUTLAWS_TESTS
void AOutlawsPlayer::GlyphTestTick(float Dt)
{
	if (GT.Scene == -2) return;
	if (GT.Layout.IsEmpty()) OLFlags::Value(TEXT("OLGlyphTest"), GT.Layout);
	AOutlawsLevelActor* L = Level.Get();
	APlayerController* PC = Cast<APlayerController>(GetController());
	const double Now = FPlatformTime::Seconds();
	if (!L || !PC || !PC->PlayerInput || L->IsFrontEnd() || ShotTime < 6.f) return;
	if (bMenuOpen && Controller) Controller->SetControlRotation(FRotator(0.0, MenuYaw, 0.0));
	if (Now < GT.Next) return;
	UPlayerInput* Inp = PC->PlayerInput;
	const FString& Lay = GT.Layout;
	auto Check = [&](bool bOk, const FString& What)
	{
		(bOk ? GT.Pass : GT.Fail)++;
		UE_LOG(LogOutlaws, Display, TEXT("OLGLYPH check %s: %s %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *Lay, *What);
	};
	// the actions a key fires as the input is bound now (the reverse of the glyph's lookup)
	auto Fires = [Inp](const FKey& Key)
	{
		TArray<FString> Out;
		for (const TCHAR* A : { TEXT("OLVR_A"), TEXT("OLVR_B"), TEXT("OLVR_X"), TEXT("OLVR_Y"), TEXT("OLVR_Menu"), TEXT("OLVR_LeftStick"), TEXT("OLVR_RightStick") })
			for (const FInputActionKeyMapping& M : Inp->GetKeysForAction(FName(A))) if (M.Key == Key) Out.AddUnique(A);
		return Out;
	};
	auto Verify = [&](const FString& Scene, const FGlyph& G, const TArray<FString>& Want)
	{
		const int32 One = OneHand();
		bool bOk = !G.IsEmpty();
		FString Said;
		for (const FMark& M : G.Marks)
		{
			const FKey K = MarkToKey(M);
			const TArray<FString> F = Fires(K);
			const bool bHit = F.ContainsByPredicate([&Want](const FString& A) { return Want.Contains(A); });
			const bool bHeld = One < 0 || M.Hand == One;
			bOk &= bHit && bHeld;
			Said += FString::Printf(TEXT(" %s %s %s fires {%s}%s;"), M.Hand == 0 ? TEXT("L") : TEXT("R"), *K.GetFName().ToString(), bHit ? TEXT("ok") : TEXT("WRONG"),
				*FString::Join(F, TEXT(",")), bHeld ? TEXT("") : TEXT(" (hand not held)"));
		}
		for (int32 H = 0; H < 2; ++H)
		{
			if (One >= 0 && H != One) continue;
			bool bBound = false;
			for (const FString& A : Want) for (const FInputActionKeyMapping& M : Inp->GetKeysForAction(FName(*A))) { FMark Mk; bBound |= KeyToMark(M.Key, Mk) && Mk.Hand == H; }
			const bool bMarked = G.Marks.ContainsByPredicate([H](const FMark& M) { return M.Hand == H; });
			if (bBound && !bMarked) { bOk = false; Said += FString::Printf(TEXT(" %s hand answers but is not marked;"), H == 0 ? TEXT("L") : TEXT("R")); }
		}
		// REQ-ONEGLYPH: one controller drawn, the main hand's when it answers (the other hand's control still fires)
		const TArray<FMark, TInlineAllocator<2>> Drawn = G.Drawn();
		const bool bMainMarked = G.Marks.ContainsByPredicate([this](const FMark& M) { return M.Hand == MainHand(); });
		const bool bOneDrawn = Drawn.Num() == 1 && (!bMainMarked || Drawn[0].Hand == MainHand());
		if (!bOneDrawn) { bOk = false; Said += FString::Printf(TEXT(" %d controllers drawn, main hand %s;"), Drawn.Num(), MainHand() == 0 ? TEXT("L") : TEXT("R")); }
		Check(bOk, FString::Printf(TEXT("%s: ring %s, drawn %s, wants {%s}:%s"), *Scene, *G.Describe(),
			Drawn.Num() == 1 ? (Drawn[0].Hand == 0 ? TEXT("L") : TEXT("R")) : TEXT("?"), *FString::Join(Want, TEXT(",")), *Said));
	};
	// an axis or analogue control: on a held hand, and that hand's control is mapped
	auto VerifyAxis = [&](const FString& Scene, const FGlyph& G, const FString& Part)
	{
		const int32 One = OneHand();
		bool bOk = !G.IsEmpty();
		for (const FMark& M : G.Marks)
		{
			bOk &= One < 0 || M.Hand == One;
			bool bMapped = false;
			for (const FInputAxisKeyMapping& A : Inp->AxisMappings) { FMark Mk; bMapped |= KeyToMark(A.Key, Mk) && Mk.Hand == M.Hand && Mk.Ctl == (M.Ctl == ECtl::StickClick ? ECtl::Stick : M.Ctl); }
			bOk &= bMapped;
		}
		const TArray<FMark, TInlineAllocator<2>> Drawn = G.Drawn();
		bOk &= Drawn.Num() == 1 && (G.Marks.Num() == 1 || Drawn[0].Hand == MainHand());
		Check(bOk, FString::Printf(TEXT("%s: %s marked %s, %d drawn (one controller %d)"), *Scene, *Part, *G.Describe(), Drawn.Num(), One));
	};
	auto Lower = [](int32 H) { return FString(H == 0 ? TEXT("OLVR_X") : TEXT("OLVR_A")); };
	auto Upper = [](int32 H) { return FString(H == 0 ? TEXT("OLVR_Y") : TEXT("OLVR_B")); };
	auto ToRoot = [this]() { while (MenuStack.Num() > 0) MenuBack(); };
	const int32 S = GT.Scene;
	auto SceneName = [S]() -> FString
	{
		static const TCHAR* const Menu[] = { TEXT("menu_back"), TEXT("menu_more_below"), TEXT("menu_more_above"), TEXT("menu_save"), TEXT("menu_title_page") };
		if (S < SceneTut0) return Menu[S];
		if (S < SceneDeath0) return FString(TEXT("tut_")) + TutCards[S - SceneTut0];
		return S == SceneDeath0 ? TEXT("death") : S == SceneDeath0 + 1 ? TEXT("death_coop") : TEXT("death_match");
	};

	if (S == -1)
	{
		// the layout, for this run only (nothing saved)
		bVR = true;
		DominantHand = (Lay == TEXT("left") || Lay == TEXT("oneleft")) ? EOutlawsHand::Left : EOutlawsHand::Right;
		bFaceReload = Lay != TEXT("classic");
		OneController = Lay == TEXT("one") ? 2 : Lay == TEXT("oneleft") ? 1 : 0;
		if (Lay == TEXT("remap"))
		{
			// Input.ini's A and B swapped on the right controller: the restart becomes the upper button, Back the lower
			const FKey RA(TEXT("OculusTouch_Right_A_Click")), RB(TEXT("OculusTouch_Right_B_Click"));
			Inp->RemoveActionMapping(FInputActionKeyMapping(TEXT("OLVR_A"), RA));
			Inp->RemoveActionMapping(FInputActionKeyMapping(TEXT("OLVR_B"), RB));
			Inp->AddActionMapping(FInputActionKeyMapping(TEXT("OLVR_A"), RB));
			Inp->AddActionMapping(FInputActionKeyMapping(TEXT("OLVR_B"), RA));
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLGLYPH layout %s: dominant %s, %s layout, one controller %d; OLVR_A on %s, OLVR_B on %s"), *Lay, LeftDominant() ? TEXT("left") : TEXT("right"),
			bFaceReload ? TEXT("default") : TEXT("Classic"), OneController, *ForActions(this, { TEXT("OLVR_A") }).Describe(), *ForActions(this, { TEXT("OLVR_B") }).Describe());
		GT.Scene = 0;
		GT.Phase = 0;
		return;
	}
	if (S >= NumScenes)
	{
		GlyphTestPrompt.Reset();
		if (Lay == TEXT("remap")) Inp->ForceRebuildingKeyMaps(true);       // (the project's own mappings back)
		UE_LOG(LogOutlaws, Display, TEXT("OLGLYPH %s RESULT %s (%d passed, %d failed; %d shots)"), *Lay, GT.Fail == 0 && GT.Pass > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), GT.Pass, GT.Fail, GT.Shots.Num());
		GT.Scene = -2;
		FGenericPlatformMisc::RequestExit(false);
		return;
	}
	const FString Name = SceneName();
	if (GT.Phase == 0)
	{
		// ---- set the scene up ----
		float Wait = 1.0f;
		if (S == 0)
		{
			if (!bMenuOpen) OpenMenu();
			ToRoot();
			ShowMenuPage(TEXT("options.rml"), true);
		}
		else if (S == 1)
		{
			// any page under the pause page whose rows scroll (none may: then the line is not there to photograph)
			ToRoot();
			TArray<FString> Queue = { TEXT("main_game.rml") };
			TSet<FString> Seen = { TEXT("main_game.rml") };
			GT.ScrollPage.Reset();
			while (Queue.Num() > 0 && GT.ScrollPage.IsEmpty())
			{
				const FString File = Queue[0];
				Queue.RemoveAt(0);
				if (!ShowMenuPage(File, false) || !MenuPage) continue;
				RefreshMenuRows();
				if (File != TEXT("main_game.rml") && SheetMaxScroll() > 0) { GT.ScrollPage = File; break; }
				for (const OutlawsMenu::FItem& I : MenuPage->Items)
					if ((I.Kind == OutlawsMenu::EItemKind::Link || I.Kind == OutlawsMenu::EItemKind::Dialog) && !I.Target.IsEmpty() && !Seen.Contains(I.Target)) { Seen.Add(I.Target); Queue.Add(I.Target); }
			}
			ShowMenuPage(TEXT("main_game.rml"), false);
			if (!GT.ScrollPage.IsEmpty()) ShowMenuPage(GT.ScrollPage, true);
			UE_LOG(LogOutlaws, Display, TEXT("OLGLYPH a page that scrolls: %s"), GT.ScrollPage.IsEmpty() ? TEXT("none") : *GT.ScrollPage);
		}
		else if (S == 2)
		{
			MenuScroll = SheetMaxScroll();
			RefreshMenuRows();
		}
		else if (S == 3)
		{
			ToRoot();
			ShowMenuPage(TEXT("save_game.rml"), true);
		}
		else if (S == 4)
		{
			ToRoot();
			ShowMenuPage(TEXT("#coop"), true);
		}
		else if (S < SceneDeath0)
		{
			if (bMenuOpen) { ToRoot(); CloseMenu(); }
			HudLines.Reset();
			HudLeft.Reset();
			bHudDirty = true;
			FString Line, Hint;
			AOutlawsLevelActor::TutorialCardLines(this, TutCards[S - SceneTut0], Line, Hint);
			ShowMessage(Line, 1.0e6f);
			if (!Hint.IsEmpty()) ShowMessage(Hint, 1.0e6f);
			TutGlyph = TutorialGlyph(TutCards[S - SceneTut0]);
			TutGlyphLine = Line;
		}
		else if (S == SceneDeath0)
		{
			HudLines.Reset();
			HudLeft.Reset();
			bHudDirty = true;
			TutGlyphLine.Reset();
			OnDied();
			bDeathAutosave = true;
			GlyphTestPrompt.Reset();
			Wait = 3.f;
		}
		else if (S == SceneDeath0 + 1) GlyphTestPrompt = UOutlawsCoop::DeathPrompt(true);
		else GlyphTestPrompt = UOutlawsCoop::MatchDeathPrompt(true);
		if (bMenuOpen) RedrawMenu();
		GT.Phase = 1;
		GT.Next = Now + Wait;
		return;
	}
	if (GT.Phase == 1)
	{
		// ---- photographed and checked ----
		const bool bSkip = (S == 1 || S == 2) && GT.ScrollPage.IsEmpty();
		if (!bSkip)
		{
			const FString File = FString::Printf(TEXT("glyph_%s_%s"), *Lay, *Name);
			FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / File + TEXT(".png"), false, false);
			GT.Shots.Add(File);
			UE_LOG(LogOutlaws, Display, TEXT("OLGLYPH shot %s.png"), *File);
		}
		const int32 One = OneHand();
		const int32 Dom = One >= 0 ? One : (LeftDominant() ? 0 : 1);
		GT.BackKey = FKey();
		if (S == 0)
		{
			Verify(TEXT("menu Back"), BackGlyph(), { TEXT("OLVR_B"), TEXT("OLVR_Y") });
			VerifyAxis(TEXT("menu select"), SelectGlyph(), TEXT("trigger"));
			// (user 15 Oct) Back centred under the rows, its controllers under the word, all on the sheet
			const FBox2D Foot = SheetFootInk.Num() == 1 ? SheetFootInk[0] : FBox2D(ForceInit);
			const float Off = Foot.bIsValid ? float(Foot.GetCenter().X) - SheetCanvasSize.X * 0.5f : 999.f;
			Check(Foot.bIsValid && FMath::Abs(Off) < 3.f && Foot.Max.Y < SheetCanvasSize.Y - 4 && SheetFootBelow > 40.f,
				FString::Printf(TEXT("menu Back line: centre %.1f px off the middle, %.0f px under its line, bottom %.0f of %d"), Off, SheetFootBelow, Foot.bIsValid ? Foot.Max.Y : 0.0, SheetCanvasSize.Y));
			// (REQ-DEATHGAP) the Options title's gap to its first row as drawn, for the death screens to match
			GT.SheetGapAt140 = SheetTitleGapDrawnSeenCm(140.f);
			UE_LOG(LogOutlaws, Display, TEXT("OLGLYPH deathgap: sheet title to first row %.2f cm seen at 140 cm (%s)"), GT.SheetGapAt140, MenuPage ? *MenuPage->File : TEXT("no page"));
			// and pressed for real: the ringed key goes back to the pause page
			if (!BackGlyph().IsEmpty()) GT.BackKey = MarkToKey(BackGlyph().Marks.Last());
			if (GT.BackKey.IsValid()) PC->InputKey(FInputKeyParams(GT.BackKey, IE_Pressed, 1.0, false));
		}
		else if (S == 1 || S == 2)
		{
			if (!bSkip) VerifyAxis(FString::Printf(TEXT("menu %s (%s)"), *Name, *GT.ScrollPage), ScrollGlyph(), TEXT("stick"));
		}
		else if (S == 3 || S == 4)
		{
			Verify(FString::Printf(TEXT("%s Back"), *Name), BackGlyph(), { TEXT("OLVR_B"), TEXT("OLVR_Y") });
			VerifyAxis(FString::Printf(TEXT("%s select"), *Name), SelectGlyph(), TEXT("trigger"));
			// (BUG-MENUGLYPH2) Select over Back, a row each, on the plate inside the sheet's corner
			TArray<FString> Said;
			if (MenuHelpPlate && MenuHelpPlate->IsVisible()) for (const FOutlawsGlyphRow& Rw : MenuHelpPlate->GetRows()) for (const OutlawsGlyphs::FPart& Pt : Rw.More) Said.Add(Pt.Label);
			const bool bPlate = Said.Num() == 2 && Said[0] == TEXT("Select") && Said[1] == TEXT("Back");
			Check(bPlate, FString::Printf(TEXT("%s: the help line (Select over Back) on the plate inside the wide sheet (%s)"), *Name, *FString::Join(Said, TEXT(", "))));
		}
		else if (S < SceneDeath0)
		{
			const FString Card = TutCards[S - SceneTut0];
			const FGlyph& G = TutGlyph.Glyph;
			const bool bShown = HudPlate && HudPlate->IsVisible();
			Check(bShown, FString::Printf(TEXT("%s: the card's glyph is shown under its line"), *Name));
			if (Card == TEXT("take_revolver")) Verify(Name, G, { Lower(Dom) });
			else if (Card == TEXT("reload")) Verify(Name, G, { bFaceReload && One < 0 ? Upper(Dom) : Lower(Dom) });
			else if (Card == TEXT("take_knife")) Verify(Name, G, { Lower(One >= 0 ? Dom : 1 - Dom) });
			else if (Card == TEXT("heal")) Verify(Name, G, { Lower(Dom) });      // (REQ-TUTORIAL2: the main hand's wheel, named on the card)
			else if (Card == TEXT("jump") && One >= 0) Verify(Name, G, { Upper(One) });
			else VerifyAxis(Name, G, Card);
			if (Card == TEXT("punch"))
			{
				// (REQ-PUNCHGRIP) no man held here: the main hand's grip alone, as the line says to hold it
				const bool bOk = G.Marks.Num() == 1 && G.Marks[0].Ctl == ECtl::Grip && G.Marks[0].Hand == Dom;
				FString Said, SaidHint;
				AOutlawsLevelActor::TutorialCardLines(this, Card, Said, SaidHint);
				Check(bOk && Said.Contains(TEXT("hold the grip to make a fist")), FString::Printf(TEXT("%s: the main hand's grip marked (%s), the line says to hold it"), *Name, *G.Describe()));
			}
		}
		else
		{
			const bool bUp = DeathPlate && DeathPlate->IsVisible();
			Check(bUp, FString::Printf(TEXT("%s: the glyph plate is up where the text was (text hidden %d)"), *Name, RespawnText && !RespawnText->IsVisible() ? 1 : 0));
			if (DeathPlate)
			{
				// (REQ-DEATHGAP) the heading to the first line as the Options title to its first row, seen alike (0.3 cm either way)
				const float Gap = DeathPlate->DrawnTitleGapCm();
				Check(GT.SheetGapAt140 > 0.f && Gap > 0.f && FMath::Abs(Gap - GT.SheetGapAt140) < 0.3f, FString::Printf(TEXT("%s deathgap: heading to first line %.2f cm, sheet title to first row %.2f cm (both at 140 cm)"),
					*Name, Gap, GT.SheetGapAt140));
				for (const FOutlawsGlyphRow& R : DeathPlate->GetRows())
				{
					if (R.Glyph.IsEmpty()) continue;
					const bool bAutosave = R.Text.Contains(TEXT("autosave"));
					Verify(FString::Printf(TEXT("%s \"%s\""), *Name, *R.Text), R.Glyph, bAutosave ? TArray<FString>{ TEXT("OLVR_B"), TEXT("OLVR_Y") } : TArray<FString>{ TEXT("OLVR_A"), TEXT("OLVR_X") });
				}
			}
		}
		GT.Phase = 2;
		GT.Next = Now + 0.4;
		return;
	}
	// ---- after: the real press let go and its effect checked ----
	if (S == 0 && GT.BackKey.IsValid())
	{
		PC->InputKey(FInputKeyParams(GT.BackKey, IE_Released, 0.0, false));
		const FString Page = MenuPage ? MenuPage->File : FString();
		Check(Page == TEXT("main_game.rml"), FString::Printf(TEXT("menu Back pressed on the ringed %s: page now %s (want main_game.rml)"), *GT.BackKey.GetFName().ToString(), *Page));
	}
	++GT.Scene;
	GT.Phase = 0;
	GT.Next = Now + 0.1;
}
#endif // WITH_OUTLAWS_TESTS
