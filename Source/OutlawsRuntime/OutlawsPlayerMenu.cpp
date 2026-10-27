// AOutlawsPlayer, menu half: the remaster's in-game menu (Common.kpf, the menu .rml files) on a panel floating in
// front of the player.  The left controller's menu button opens and closes it; either hand points a
// laser and the trigger picks; B or Y goes back a page.  Controls change what they show and nothing
// else yet.
#include "OutlawsDeathmatch.h"
#include "OutlawsFlags.h"
#include "OutlawsBeacon.h"
#include "OutlawsPlayer.h"
#include "OutlawsStats.h"
#include "OutlawsBoxHand.h"
#include "OutlawsPerf.h"
#include "OutlawsRuntime.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsLab.h"
#include "OutlawsLevelActor.h"
#include "OutlawsUnlocks.h"
#include "OutlawsAchievements.h"
#include "OutlawsCoop.h"
#include "OutlawsFragTable.h"
#include "OutlawsBuildInfo.h"
#include "OutlawsDeviceProfile.h"
#include "OutlawsMpHost.h"
#include "OutlawsDevChecks.h"
#include "HAL/IConsoleManager.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "MotionControllerComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/StaticMesh.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/CanvasRenderTarget2D.h"
#include "Engine/Canvas.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "CanvasItem.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "OutlawsTextures.h"
#include "Engine/TextureRenderTarget2D.h"
#include "RenderUtils.h"
#include "UnrealClient.h"
#include "HAL/PlatformMisc.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "OutlawsShotTests.h"

namespace
{
	constexpr int32 MenuRowsShown = 9;
	// (REQ-MENUCENTRE: the pause page's sheet 96 cm tall, from 80: the sheet is only the canvas the text is drawn on, and nine
	// rows with the status line tight under them now sit on it)
	constexpr float PanelW = 100.f, PanelH = 96.f;
	constexpr float RowTop = 8.8f, RowStep = 5.15f;     // the rows sit under the parchment's OUTLAWS plate
	// REQ-SUBMENUSPACE (user 14 Oct): a titled page (Options and the pages under it) on a taller sheet, half as tall again, with
	// its centre where it was; its title, rows and lines lifted, the rows a little further apart (halfway to the pause page's 8)
	// and the help / "more below" line well inside the sheet's foot.  The pause page keeps PanelH and its own layout
	constexpr int32 TallRowsShown = 12;                // (at most: SheetRowsFit keeps those that sit on the sheet)
	constexpr float TallPanelH = 120.f;
	constexpr float TallLift = 14.f;                   // the title, the first row and "more above", higher by this
	constexpr float TallRowStep = 6.6f;
	// REQ-MENUCENTRE (user 14 Oct): the help, status and "more below" line one small gap under the last row shown, the same on
	// every page (it was pinned near the sheet's foot: far under a short page's rows), centre to centre
	constexpr float MenuFootGap = 6.f;
	// BUG-SPEEDWARN: the VR page's sickness warning row shows over this speed (the old 30 %, shown as 120 %)
	constexpr float SpeedWarnOver = 0.605f;
	constexpr float SpeedWarnLine = 3.2f;              // its two lines, centre to centre, in the row's place
	// BUG-GFXTALL (user 15 Oct: the Graphics page with "more above" over its title had him look up): every sheet page under the
	// pause page keeps its block, from the top of its top line ("more above" or the title) to the foot of its prompt line, within
	// this many cm of the sheet, and that top line within SheetTopMaxCm over the rows' middle (the eye's height).  The tallest
	// page that shows all its rows at once (Voice Chat, seven rows, in VR: 74.6 and 34.7 cm) sets both; a longer page pages its
	// rows, and paged down its title makes way for "more above"
	constexpr float SheetBlockMaxCm = 75.f;
	constexpr float SheetTopMaxCm = 35.f;
	constexpr int32 CanvasW = 1024;
	int32 CanvasH(float SheetHeight) { return FMath::CeilToInt(SheetHeight * float(CanvasW) / PanelW); }

	/** (REQ-SUBMENUSPACE) the menu sheet's bounds reach past the eyes, as the scoreboard's and the wheel's (BUG-SCOREOCCLUDE,
	 *  BUG-WHEELOCCLUDE): its own bounds behind a near wall let occlusion culling drop the whole sheet. */
	void KeepSheetOverWorld(UPrimitiveComponent* P, const FVector& Eye)
	{
		if (!P) return;
		const float Radius = P->Bounds.SphereRadius / FMath::Max(P->BoundsScale, 0.01f);
		if (Radius < 0.01f) return;
		const float Need = (float(FVector::Dist(Eye, P->Bounds.Origin)) + 60.f) / Radius;
		if (P->BoundsScale < Need) P->SetBoundsScale(FMath::Min(Need * 1.25f, 4000.f));
	}
	constexpr float MenuDistance = 240.f;              // out where the weapon wheel is (six to ten feet)
	constexpr float MenuScale = 2.5f;                  // its parts are laid out in cm at scale 1

	/** Break a long line at word boundaries (text renders do not wrap). */
	FString Wrap(const FString& Text, int32 Width)
	{
		TArray<FString> Words;
		Text.ParseIntoArrayWS(Words);
		FString Out, Line;
		for (const FString& W : Words)
		{
			if (!Line.IsEmpty() && Line.Len() + 1 + W.Len() > Width)
			{
				Out += Line + TEXT("\n");
				Line.Reset();
			}
			Line += (Line.IsEmpty() ? TEXT("") : TEXT(" ")) + W;
		}
		return Out + Line;
	}
}

bool AOutlawsPlayer::LoadMenuSource()
{
	if (MenuSource && MenuSource->IsLoaded())
	{
		return true;
	}
	MenuSource = MakeShared<OutlawsMenu::FSource>();
	const AOutlawsLevelActor* L = Level.Get();
	const FString Root = FOutlawsAssets::FindInstallRoot(L ? L->InstallPath : FString());
	return !Root.IsEmpty() && MenuSource->Load(Root);
}

void AOutlawsPlayer::PressMenu()
{
	if (bFrontMenu)
	{
		return;                    // the main menu stays until a game starts
	}
	if (!bMenuOpen && Level.IsValid() && (Level->IsFrontEnd() || Level->IsRebuildPending()))
	{
		return;                    // (a level on its way: no pause menu before it, its fade-in would never end)
	}
	if (bMenuOpen)
	{
		CloseMenu();
	}
	else if (UOutlawsCoop::CloseScoreboard(this, TEXT("the Menu button")))
	{
		// (BUG-SCOREBOARD) a match's frag table up: the Menu button that brought it (through the pause menu) puts it away
	}
	else
	{
		OpenMenu();
	}
}

void AOutlawsPlayer::OpenMenu(bool bFront)
{
	AOutlawsLevelActor* LM = Level.Get();
	if (bMenuOpen || bDeadView || !LoadMenuSource())
	{
		return;
	}
	// over a movie (user 2026-10-01: pause a cutscene with the same button): it holds where it is, death-toned behind the
	// menu as play is; the hands and the menu's laser come back to point with (Tick's movie branch)
	if (LM && LM->IsMoviePlaying())
	{
		LM->PauseMovie(true, 1.f);
		LM->SetMovieSkipHold(0.f, 3.f, 0);
		MovieHold = 0.f;
		if (bMovieHidden) HideForMovie(false);
	}
	bFrontMenu = bFront;
	if (bWheelOpen)
	{
		CloseWheel();
	}
	auto Text = [this](float Size, const FColor& Colour) -> UTextRenderComponent*
	{
		UTextRenderComponent* T = NewObject<UTextRenderComponent>(this);
		T->SetHorizontalAlignment(EHTA_Center);
		T->SetVerticalAlignment(EVRTA_TextCenter);
		T->SetWorldSize(Size);
		T->SetTextRenderColor(Colour);
		if (UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_Text.M_OLR_Text"))) T->SetTextMaterial(M);
		T->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		T->SetCastShadow(false);
		T->SetupAttachment(MenuRoot);
		T->RegisterComponent();
		return T;
	};
	if (!MenuRoot)
	{
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		MenuRoot = NewObject<USceneComponent>(this, TEXT("GameMenu"));
		MenuRoot->SetupAttachment(VROrigin);
		MenuRoot->RegisterComponent();
		auto Box = [this, Cube](const FColor& Colour, USceneComponent* Parent) -> UStaticMeshComponent*
		{
			UStaticMeshComponent* B = NewObject<UStaticMeshComponent>(this);
			B->SetStaticMesh(Cube);
			B->SetMaterial(0, SolidMaterial(Colour));
			B->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			B->SetCastShadow(false);
			B->SetupAttachment(Parent);
			B->RegisterComponent();
			return B;
		};
		// dark wood panel, the title, a message line for dialogs, the rows, scroll marks and a toast
		MenuPanel = Box(FColor(34, 24, 16), MenuRoot);
		MenuPanel->SetRelativeLocation(FVector(-1.0, 0.0, 0.0));
		MenuPanel->SetRelativeScale3D(FVector(0.01, PanelW / 100.0, PanelH / 100.0));
		for (int32 H = 0; H < 2; ++H)
		{
			MenuHoverBox[H] = Box(H == 0 ? FColor(120, 88, 40) : FColor(170, 120, 50), MenuRoot);
			MenuHoverBox[H]->SetRelativeScale3D(FVector(0.004, (PanelW - 8.f) / 100.0, (PageRowStep() - 0.6f) / 100.0));
			MenuHoverBox[H]->SetVisibility(false);
			MenuLaser[H] = Box(FColor(255, 40, 30), AimOf(H));
			MenuLaser[H]->SetMaterial(0, LaserMaterial());     // (spared the menu's tone on the headset: it hid them)
			MenuLaser[H]->SetVisibility(false);
		}
		MenuTitle = Text(4.2f, FColor(240, 210, 150));
		MenuTitle->SetRelativeLocation(FVector(0.5, 0.0, 33.0));
		MenuMessage = Text(2.3f, FColor(230, 220, 200));
		MenuMessage->SetRelativeLocation(FVector(0.5, 0.0, 12.0));
		MenuMoreUp = Text(2.f, FColor(180, 160, 120));
		MenuMoreUp->SetRelativeLocation(FVector(0.5, 0.0, PageRowTop() + PageRowStep() * 0.8f));
		MenuMoreDown = Text(2.f, FColor(180, 160, 120));
		MenuMoreDown->SetRelativeLocation(FVector(0.5, 0.0, PageRowTop() - PageRowStep() * (MenuRowsShown - 0.2f)));
		MenuToast = Text(2.2f, FColor(255, 200, 120));
		MenuToast->SetRelativeLocation(FVector(0.5, 0.0, -PanelH * 0.5f + 3.f));
		for (int32 i = 0; i < TallRowsShown; ++i)
		{
			UTextRenderComponent* T = Text(2.6f, FColor(235, 220, 180));
			T->SetRelativeLocation(FVector(0.5, 0.0, PageRowTop() - PageRowStep() * i));
			MenuRows.Add(T);
		}
		// the parchment sheet the page is drawn on
		MenuTitleFont = KpfFont(TEXT("fonts/Rye-Regular.ttf"), 64);
		MenuRowFont = KpfFont(TEXT("fonts/MontaguSlab_24pt-Bold.ttf"), 36);
		MenuSmallFont = KpfFont(TEXT("fonts/MontaguSlab_24pt-Bold.ttf"), 24);
		MenuParchment = KpfDds(TEXT("menu/assets/Main_Base.dds"));
		MenuCanvas = NewObject<UCanvasRenderTarget2D>(this);
		MenuCanvas->RenderTargetFormat = RTF_RGBA8_SRGB;
		MenuCanvas->TargetGamma = 1.f;          // the sRGB target encodes on write
		MenuCanvas->ClearColor = FLinearColor::Transparent;
		MenuCanvas->InitAutoFormat(CanvasW, CanvasH(PanelH));
		MenuCanvas->OnCanvasRenderTargetUpdate.AddDynamic(this, &AOutlawsPlayer::DrawMenuCanvas);
		MenuMask = NewObject<UCanvasRenderTarget2D>(this);
		MenuMask->RenderTargetFormat = RTF_RGBA8;           // linear: coverage as drawn
		MenuMask->TargetGamma = 1.f;
		MenuMask->ClearColor = FLinearColor::Black;
		MenuMask->InitAutoFormat(CanvasW, CanvasH(PanelH));
		MenuMask->OnCanvasRenderTargetUpdate.AddDynamic(this, &AOutlawsPlayer::DrawMenuMask);
		MenuSheet = NewObject<UOutlawsOnTopPlaneComponent>(this);      // (REQ-SUBMENUSPACE: cube bounds, so the eyes can be inside them)
		MenuSheet->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane")));
		MenuSheet->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		MenuSheet->SetCastShadow(false);
		MenuSheet->SetupAttachment(MenuRoot);
		MenuSheet->RegisterComponent();
		// the plane (facing +Z) turned to face the eyes (the menu's X), picture upright
		MenuSheet->SetRelativeLocationAndRotation(FVector(-0.5, 0.0, 0.0), FRotationMatrix::MakeFromXY(FVector(0.0, -1.0, 0.0), FVector(0.0, 0.0, -1.0)).Rotator());
		MenuSheet->SetRelativeScale3D(FVector(PanelW / 100.0, PanelH / 100.0, 1.0));
		// drawn over everything: the colour sheet, its opacity from the coverage sheet
		if (UMaterialInterface* Base = LoadOverlayMaterial(TEXT("M_OLR_MenuOverlay")))
		{
			UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, this);
			Mid->SetTextureParameterValue(TEXT("Tex"), MenuCanvas);
			Mid->SetTextureParameterValue(TEXT("Mask"), MenuMask);
			MenuSheet->SetMaterial(0, Mid);
			KeepAlive.Add(Mid);
		}
	}
	MenuStack.Reset();
	if (!ShowMenuPage(bFront ? UOutlawsCoop::FrontPage(this) : FString(TEXT("main_game.rml")), false))      // ("#front"; a co-op session's lobby)
	{
		UE_LOG(LogOutlaws, Warning, TEXT("OLRT menu: main_game.rml not found in Common.kpf"));
		MenuRoot->SetVisibility(false, true);
		for (int32 H = 0; H < 2; ++H)
		{
			if (MenuLaser[H]) MenuLaser[H]->SetVisibility(false);
		}
		return;
	}
	bMenuOpen = true;
	// facing the way the head faces, up or down as it looks
	MenuYaw = Camera->GetComponentRotation().Yaw;
	MenuYawVel = MenuPitchVel = 0.f;
	bMenuYawFollow = bMenuPitchFollow = false;
	CheatSpin = 0.f;
	CheatSpinLastYaw = MenuYaw;
	MenuPitch = bFrontMenu ? 0.f : FMath::Clamp(float(Camera->GetComponentRotation().Pitch), -70.f, 70.f);   // the main menu stays level
	MenuRoot->SetVisibility(true, true);
	MenuHoverBox[0]->SetVisibility(false);
	MenuHoverBox[1]->SetVisibility(false);
	MenuToast->SetText(FText::GetEmpty());
	if (MenuSheet)
	{
		// the sheet shows the page; the text renders and the wood panel only hold it
		for (USceneComponent* C : { static_cast<USceneComponent*>(MenuPanel), static_cast<USceneComponent*>(MenuTitle), static_cast<USceneComponent*>(MenuMessage),
			static_cast<USceneComponent*>(MenuMoreUp), static_cast<USceneComponent*>(MenuMoreDown), static_cast<USceneComponent*>(MenuToast) }) if (C) C->SetVisibility(false);
		for (UTextRenderComponent* T : MenuRows) T->SetVisibility(false);
		RedrawMenu();
	}
	for (int32 H = 0; H < 2; ++H)
	{
		MenuLaser[H]->SetVisibility(bVR);
		bMenuTriggerArmed[H] = false;     // the press that opened it does not also pick
	}
	// the world stops: everything but the player runs at a ten-thousandth of real time, and the
	// player's own body holds still too (a fall or a slide resumes when the menu closes)
	// (not in co-op: the session keeps one clock for both machines and the network; the menu there pauses nothing, and
	// since 2026-10-02 not the other player either: the game goes on for both, this player out of harm, OutlawsCoopRules.cpp;
	// R7: a host playing alone in its session stops the world as single player does, until someone joins)
	if (!UOutlawsCoop::IsActive(this) || UOutlawsCoop::IsHostAlone(this))
	{
		UGameplayStatics::SetGlobalTimeDilation(this, 0.0001f);
		CustomTimeDilation = 10000.f;
	}
	MenuSavedMode = GetCharacterMovement()->MovementMode;
	MenuSavedVelocity = GetCharacterMovement()->Velocity;
	GetCharacterMovement()->DisableMovement();
	WristText->SetVisibility(false);
	WristLast.Reset();
	Pulse(0, 0.3f);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT menu opened"));
}

void AOutlawsPlayer::CloseMenu()
{
	if (!bMenuOpen)
	{
		return;
	}
	bMenuOpen = false;
	bFrontMenu = false;
	SetSheetShape(false);
	// a movie held behind the menu goes on (the movie branch puts the hands away again)
	if (AOutlawsLevelActor* LMv = Level.Get(); LMv && LMv->IsMoviePlaying()) LMv->PauseMovie(false, 0.f);
	if (MenuRoot) MenuRoot->SetVisibility(false, true);
	UOutlawsCoop::HidePauseScoreboard(this);      // (REQ-SCOREABOVE)
	for (int32 H = 0; H < 2; ++H)
	{
		if (MenuLaser[H]) MenuLaser[H]->SetVisibility(false);
	}
	UGameplayStatics::SetGlobalTimeDilation(this, 1.f);
	CustomTimeDilation = 1.f;
	GetCharacterMovement()->SetMovementMode(MenuSavedMode == MOVE_None ? MOVE_Falling : MenuSavedMode.GetValue());
	GetCharacterMovement()->Velocity = MenuSavedVelocity;
	// the trigger that picked Resume must be let go before a gun fires; hand motion and grips during
	// the menu are not a swing or a grab
	for (int32 H = 0; H < 2; ++H)
	{
		bTriggerArmed[H] = false;
		PrevHandPos[H] = GripOf(H)->GetComponentLocation() - GetActorLocation();
		PrevHandWorld[H] = GripOf(H)->GetComponentLocation();
		HandVelocity[H] = HandWorldVel[H] = FVector::ZeroVector;
		PrevGrip[H] = Axis(H == 0 ? TEXT("OLVR_LeftGrip") : TEXT("OLVR_RightGrip"));
		bGripArmed[H] = PrevGrip[H] < 0.4f;
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT menu closed"));
}

void AOutlawsPlayer::UnlockPageContinue()
{
	// (REQ-UNLOCKS) any button on an unlock page: never shown again; the next one, or the main menu
	if (!MenuPage || MenuPage->File != TEXT("#unlocked")) return;
	if (!UnlockPageId.IsEmpty()) OutlawsUnlocks::MarkSeen(Level.Get(), UnlockPageId);
	UnlockPageId.Reset();
	ShowMenuPage(TEXT("#front"), false);
}

void AOutlawsPlayer::MenuBack()
{
	if (MenuPage && MenuPage->File == TEXT("#unlocked")) { UnlockPageContinue(); return; }
	if (MenuStack.Num() == 0)
	{
		if (!bFrontMenu) CloseMenu();
		return;
	}
	const FString Prev = MenuStack.Pop();
	const FString Left = MenuPage ? MenuPage->File : FString();
	ShowMenuPage(Prev, false);
	if (Left == TEXT("#voice") && MenuPage && MenuPage->File == TEXT("#mp"))
	{
		// (REQ-VOICEPAGE) back on the Multiplayer page with the Voice Chat row lit, as when it was pressed
		const int32 i = MenuPage->Items.IndexOfByPredicate([](const OutlawsMenu::FItem& It) { return It.Target == TEXT("#voice"); });
		if (i != INDEX_NONE && i >= MenuScroll)
		{
			const int32 First = MenuPage->Message.IsEmpty() ? 0 : 4;
			MenuHover[0] = MenuHover[1] = i - MenuScroll + First;
			bMenuHoverHeld = true;
			RedrawMenu();
		}
	}
}

bool AOutlawsPlayer::ShowMenuPage(const FString& File, bool bPush)
{
	// (REQ-UNLOCKS) the main menu opens on any unlock page this profile has not seen yet (after the ending and the credits)
	if (File == TEXT("#front"))
	{
		FString Id, Title, Where;
		if (OutlawsUnlocks::NextNotice(Level.Get(), Id, Title, Where)) return ShowMenuPage(TEXT("#unlocked"), false);
	}
	if (!MenuPage || MenuPage->File != File) UOutlawsCoop::MenuPageChanged(this);      // (REQ-MPLOBBY c: a player list from its first page)
	TSharedPtr<OutlawsMenu::FPage> Page = MakeShared<OutlawsMenu::FPage>();
	if (File.StartsWith(TEXT("#extras")))
	{
		if (!BuildExtrasPage(File, *Page)) return false;           // (REQ-EXTRAS: OutlawsPlayerExtras.cpp)
	}
	else if (File == TEXT("#devchecks"))
	{
		Page->File = File;
		OutlawsDevChecks::Reload();            // (a list pushed while the game runs shows the next time the page opens)
		OutlawsDevChecks::BuildPage(*Page);
	}
	else if (File == TEXT("#cheats"))
	{
		// our own page: the cheats, working
		using namespace OutlawsMenu;
		Page->File = File;
		Page->Title = TEXT("CHEATS");
		// BUG-ALLOWCHEATS: a client of a match whose host has Allow Cheats off sees one line instead of the rows; a row that only
		// changes this player's own menus (CheatIsLocalOnly) stays
		const bool bHostOff = UOutlawsCoop::CheatsOffHere(this);
		bool bAnyHidden = false;
		auto Add = [&Page, bHostOff, &bAnyHidden](EItemKind Kind, const TCHAR* Label, const TCHAR* Name)
		{
			if (bHostOff && !CheatIsLocalOnly(Name)) { bAnyHidden = true; return; }
			FItem I;
			I.Kind = Kind;
			I.Label = Label;
			I.Name = Name;
			Page->Items.Add(I);
		};
		// (user 2026-10-03: the original's codes, as many as fit; each row names its code)
		Add(EItemKind::Action, TEXT("Jackpot: the level's keys (OLJACKPOT)"), TEXT("cheat_jackpot"));
		Add(EItemKind::Check, TEXT("Invincibility (OLIMYELLA)"), TEXT("cheat_god"));
		Add(EItemKind::Check, TEXT("Unlimited ammo (OLASH)"), TEXT("cheat_ammo"));
		Add(EItemKind::Action, TEXT("All weapons and ammo (OLPOSTAL)"), TEXT("cheat_weapons"));
		Add(EItemKind::Action, TEXT("Full health (OLER)"), TEXT("cheat_health"));
		Add(EItemKind::Action, TEXT("More oil (OLOPEC)"), TEXT("cheat_oil"));
		Add(EItemKind::Check, TEXT("Freeze enemies (OLREDLITE)"), TEXT("cheat_freeze"));
		Add(EItemKind::Action, TEXT("Bring on the boss (OLAPPEAR)"), TEXT("cheat_boss"));
		if (!bHostOff)
		{
			FItem I;
			I.Kind = EItemKind::Link;
			I.Label = TEXT("Skip to level (OLSCORE)");
			I.Target = TEXT("#levels");
			Page->Items.Add(I);
		}
		else
		{
			FItem H;
			H.Kind = EItemKind::Heading;
			H.Label = TEXT("The host has cheats off");
			Page->Items.Insert(H, 0);
		}
		(void)bAnyHidden;
		// (UNLOCKFREE: the Unlock all modes row went with the locks; every mode is open from the start)
	}
	else if (File == TEXT("#unlocked"))
	{
		// REQ-UNLOCKS: on the way back to the main menu after the ending (the credits first), one page per new unlock, once
		using namespace OutlawsMenu;
		Page->File = File;
		Page->Title = TEXT("UNLOCKED");
		FString Title, Where;
		UnlockPageId.Reset();
		OutlawsUnlocks::NextNotice(Level.Get(), UnlockPageId, Title, Where);
		auto Line = [&Page](const FString& Text) { FItem H; H.Kind = EItemKind::Heading; H.Label = Text; Page->Items.Add(H); };
		Line(Title);
		TArray<FString> WhereLines;
		Where.ParseIntoArray(WhereLines, TEXT("|"));
		for (const FString& WL : WhereLines) Line(WL);
		FItem Ok;
		Ok.Kind = EItemKind::Action;
		Ok.Label = TEXT("Continue");
		Ok.Name = TEXT("unlock_ok");
		Page->Items.Add(Ok);
		UE_LOG(LogOutlaws, Log, TEXT("OLUNLOCK page: %s | %s (%s)"), *Title, *Where, *UnlockPageId);
	}
	else if (File == TEXT("#vr"))
	{
		// the VR settings: how the stick moves, how it turns, which hand leads, the button layout
		using namespace OutlawsMenu;
		Page->File = File;
		Page->Title = TEXT("VR");
		auto Add = [&Page](EItemKind Kind, const TCHAR* Label, const TCHAR* Name, TArray<FString> Options)
		{
			FItem I;
			I.Kind = Kind;
			I.Label = Label;
			I.Name = Name;
			I.Options = MoveTemp(Options);
			Page->Items.Add(I);
		};
		Add(EItemKind::Heading, TEXT("Locomotion"), TEXT(""), {});
		Add(EItemKind::Select, TEXT("Movement"), TEXT("vr_locomotion"), { TEXT("Smooth"), TEXT("Teleport") });
		// the smooth walk's own settings; teleporting has no walk to steer, and runs at half pace (user 2026-09-28)
		if (!bTeleportMove)
		{
			Add(EItemKind::Select, TEXT("Movement follows"), TEXT("vr_movefollow"), { TEXT("Head"), TEXT("Dominant hand"), TEXT("Off hand") });
			Add(EItemKind::Range, TEXT("Movement speed (bad guys too)"), TEXT("vr_speed"), {});
			// (BUG-SPEEDWARN, user 15 Oct: the warning was a toast over the prompts) its own row in the small font, under the
			// speed, while the speed is over the old 30 %; the page is built again as it comes or goes
			const AOutlawsLevelActor* LS = Level.Get();
			if (LS && LS->GameSpeed > SpeedWarnOver) Add(EItemKind::Heading, TEXT("Higher movement speeds can induce VR sickness.\nMake sure you start slow, and increase this at your own risk!"), TEXT("vr_speedwarn"), {});
		}
		Add(EItemKind::Select, TEXT("Turning"), TEXT("vr_turn"), { TEXT("Snap"), TEXT("Smooth") });
		Add(EItemKind::Heading, TEXT("Hands"), TEXT(""), {});
		Add(EItemKind::Select, TEXT("Dominant hand"), TEXT("vr_domhand"), { TEXT("Right"), TEXT("Left") });
		Add(EItemKind::Select, TEXT("One controller"), TEXT("vr_onectrl"), { TEXT("Off"), TEXT("Left only"), TEXT("Right only") });
		Add(EItemKind::Check, TEXT("Y / B reload (off: tap X / A)"), TEXT("vr_facereload"), {});
		Add(EItemKind::Action, TEXT("Restore defaults"), TEXT("defaults:vr"), {});
	}
	else if (File == TEXT("#levels"))
	{
		// the level skip: every level, the story first
		using namespace OutlawsMenu;
		Page->File = File;
		Page->Title = TEXT("SKIP TO LEVEL");
		TArray<FString> Names, Labels;
		if (AOutlawsLevelActor* L = Level.Get()) L->GetLevelChoices(Names, Labels);
		for (int32 i = 0; i < Names.Num(); ++i)
		{
			FItem I;
			I.Kind = EItemKind::Action;
			I.Label = Labels[i];
			I.Name = TEXT("level:") + Names[i];
			Page->Items.Add(I);
		}
	}
	else if (File == TEXT("#front"))
	{
		// the remaster's main menu (main.rml): Load Game, New Game; Options, Exit
		using namespace OutlawsMenu;
		Page->File = File;
		auto Link = [&Page](const TCHAR* Label, const TCHAR* Target) { FItem I; I.Kind = EItemKind::Link; I.Label = Label; I.Target = Target; Page->Items.Add(I); };
		// (REQ-TITLEMENU, user 14 Oct) two big buttons: Load Game (the load page: the resume point and the autosaves first, then
		// the player's own saves) where Resume Game was, and New Game (Main Game or Historical Missions on the next page); the
		// small row is Options and Exit alone.  Load Game is greyed out with no save at all, and in a co-op or match session
		// (BUG-MPRESUME, dotard 12 Oct: in a match the title's Resume Game loaded the single-player game)
		{
			FItem Ld;
			Ld.Kind = EItemKind::Link;
			Ld.Label = TEXT("Load Game");
			Ld.Name = TEXT("title_load");
			Ld.Target = TEXT("load_game.rml");
			Ld.Classes = TitleLoadOffered(this) ? TEXT("big") : TEXT("big disabled");
			Page->Items.Add(Ld);
		}
		{
			FItem N;
			N.Kind = EItemKind::Link;
			N.Label = TEXT("New Game");
			N.Target = TEXT("#newgame");
			N.Classes = TEXT("big");
			Page->Items.Add(N);
		}
		// (REQ-EXTRAS, user 13 Oct) Extras in the small row before Options, as the remaster's main menu has it beside Options
		Link(TEXT("Extras"), TEXT("#extras"));
		Link(TEXT("Options"), TEXT("options.rml"));
		FItem Q;
		Q.Kind = EItemKind::Action;
		Q.Label = TEXT("Exit");
		Q.Action = TEXT("ev_quit");
		Q.Classes = TEXT("chicken");
		Page->Items.Add(Q);
	}
	else if (File.StartsWith(TEXT("#difficulty:")))
	{
		// difficulty.rml: Good, Bad, Ugly, then the game named after the colon (story, or a mission level)
		using namespace OutlawsMenu;
		Page->File = File;
		Page->Title = MenuSource ? MenuSource->Loc(TEXT("$m_local_6014_UC")) : FString(TEXT("DIFFICULTY"));
		const FString Target = File.Mid(12);
		const TCHAR* Names[] = { TEXT("Good"), TEXT("Bad"), TEXT("Ugly") };
		for (int32 d = 0; d < 3; ++d)
		{
			FItem I;
			I.Kind = EItemKind::Action;
			I.Label = Names[d];
			I.Name = FString::Printf(TEXT("start:%s:%d"), *Target, d);
			Page->Items.Add(I);
		}
	}
	else if (File == TEXT("#newgame"))
	{
		// New Game's page: the story or a historical mission, each on as before
		using namespace OutlawsMenu;
		Page->File = File;
		Page->Title = TEXT("NEW GAME");
		auto Link = [&Page](const TCHAR* Label, const TCHAR* Target) { FItem I; I.Kind = EItemKind::Link; I.Label = Label; I.Target = Target; Page->Items.Add(I); };
		Link(TEXT("Main Game"), TEXT("#difficulty:story"));
		Link(TEXT("Historical Missions"), TEXT("#missions"));
		// REQ-MPMENU (user 15 Oct): one Multiplayer choice holds Co-op and Competitive, each with its own Host Game and Join
		// Game (OutlawsCoop.cpp builds the pages); the Tutorial last
		Link(TEXT("Multiplayer"), TEXT("#coopmulti"));
		{
			// REQ-TUTORIAL: the controls in three minutes in Sanctuary (OutlawsTutorial.cpp)
			FItem T;
			T.Kind = EItemKind::Action;
			T.Label = TEXT("Tutorial");
			T.Name = TEXT("tutorial_start");
			Page->Items.Add(T);
		}
	}
	else if (File == TEXT("#character"))
	{
		// (M1) the remaster's character page (lobby_character.rml): six tiles, the choice worn from the next spawn
		using namespace OutlawsMenu;
		Page->File = File;
		Page->Title = MenuSource ? MenuSource->Loc(TEXT("$m_character_select")) : FString(TEXT("Character Select"));
		if (Page->Title.IsEmpty() || Page->Title.StartsWith(TEXT("$"))) Page->Title = TEXT("Character Select");
		int32 N = 0;
		const FOutlawsMpCharacter* Cs = FOutlawsMpCharacter::All(N);
		for (int32 i = 0; i < N; ++i)
		{
			FItem I;
			I.Kind = EItemKind::Action;
			I.Label = Cs[i].Name;
			I.Name = FString(TEXT("char:")) + Cs[i].Short;
			Page->Items.Add(I);
		}
	}
	else if (File == TEXT("#homride"))
	{
		// (HM1) an office poster nudged or shot: ride out to its mission, or not yet
		using namespace OutlawsMenu;
		Page->File = File;
		Page->Title = TEXT("WANTED");
		const AOutlawsLevelActor* LH = Level.Get();
		FItem Go;
		Go.Kind = EItemKind::Action;
		Go.Label = FString::Printf(TEXT("Ride out to %s"), LH ? *LH->HomMissionLabel() : TEXT("the mission"));
		Go.Name = TEXT("hom_ride");
		Page->Items.Add(Go);
		FItem No;
		No.Kind = EItemKind::Action;
		No.Label = TEXT("Not yet");
		No.Name = TEXT("hom_stay");
		Page->Items.Add(No);
	}
	else if (File == TEXT("#missions"))
	{
		using namespace OutlawsMenu;
		Page->File = File;
		Page->Title = TEXT("HISTORICAL MISSIONS");
		// REQ-HMCAREER (user 14 Oct): the five original missions only through Marshal Training (its office's posters); the
		// Handful of Missions below, each a row of its own (matched by level file, never by title)
		TArray<FString> Names, Labels;
		const AOutlawsLevelActor* LM = Level.Get();
		AOutlawsLevelActor::GetHandfulChoices(Names, Labels, LM ? LM->GetAssets() : nullptr);
		// (HM1) the original's way in: the crawl, then the Marshal's office and its posters
		if (AOutlawsLevelActor::HasHomSave())
		{
			FItem I;
			I.Kind = EItemKind::Action;
			I.Label = TEXT("Resume Marshal Training");
			I.Name = TEXT("hom_resume");
			Page->Items.Add(I);
		}
		{
			FItem I;
			I.Kind = EItemKind::Link;
			I.Label = TEXT("Marshal Training");
			I.Target = TEXT("#difficulty:hom");
			Page->Items.Add(I);
		}
		for (int32 i = 0; i < Names.Num(); ++i)
		{
			if (AOutlawsLevelActor::IsCareerMission(Names[i])) continue;
			FItem I;
			I.Kind = EItemKind::Link;
			I.Label = Labels[i];
			I.Target = TEXT("#difficulty:") + Names[i];
			Page->Items.Add(I);
		}
	}
	else if (File == TEXT("#mp"))
	{
		// the Multiplayer page (user 2026-10-02): the name the other player sees over this player's head in co-op (the letters
		// keyboard), the voice chat's page (REQ-VOICEPAGE, user 14 Oct: its rows moved to #voice), the home network's finding;
		// each saved at once
		using namespace OutlawsMenu;
		Page->File = File;
		Page->Title = TEXT("MULTIPLAYER");
		const AOutlawsLevelActor* LN = Level.Get();
		FItem N;
		N.Kind = EItemKind::Action;
		N.Label = FString::Printf(TEXT("Player name:  %s"), LN && !LN->MpPlayerName.IsEmpty() ? *LN->MpPlayerName : TEXT("(none)"));
		N.Name = TEXT("mp_name");
		Page->Items.Add(N);
		FItem Vc;
		Vc.Kind = EItemKind::Link;
		Vc.Label = TEXT("Voice Chat");
		Vc.Target = TEXT("#voice");
		Page->Items.Add(Vc);
		// R11: the join bubbles on the home network (OutlawsBeacon.cpp)
		FItem An;
		An.Kind = EItemKind::Check;
		An.Label = TEXT("Let players on my network find my game");
		An.Name = TEXT("mp_announce");
		Page->Items.Add(An);
		FItem Li;
		Li.Kind = EItemKind::Check;
		Li.Label = TEXT("Tell me when someone nearby is playing");
		Li.Name = TEXT("mp_listen");
		Page->Items.Add(Li);
		// REQ-UPNP: the host's router asked to forward the game's port (OutlawsCoopUpnp.cpp)
		FItem Up;
		Up.Kind = EItemKind::Check;
		Up.Label = TEXT("Open my router's port automatically");
		Up.Name = TEXT("mp_upnp");
		Page->Items.Add(Up);
	}
	else if (File == TEXT("#voice"))
	{
		// REQ-VOICEPAGE (user 14 Oct): the voice chat's rows, moved from the Multiplayer page as they were (the same settings,
		// keys and defaults); Back to the Multiplayer page
		using namespace OutlawsMenu;
		Page->File = File;
		Page->Title = TEXT("VOICE CHAT");
		FItem M;
		M.Kind = EItemKind::Check;
		M.Label = TEXT("Microphone");
		M.Name = TEXT("mp_mic");
		Page->Items.Add(M);
		// R8: the level live (the microphone opened for it while this page is up), push to talk, sensitivity
		FItem Mt;
		Mt.Kind = EItemKind::Action;
		Mt.Label = TEXT("Level");
		Mt.Name = TEXT("mp_meter");
		Page->Items.Add(Mt);
		FItem Pt;
		Pt.Kind = EItemKind::Select;
		Pt.Label = TEXT("Push to talk");
		Pt.Name = TEXT("mp_ptt");
		Pt.Options = { TEXT("Off"), TEXT("Left grip"), TEXT("Right grip"), TEXT("Either grip") };
		Page->Items.Add(Pt);
		FItem Sn;
		Sn.Kind = EItemKind::Select;
		Sn.Label = TEXT("Microphone sensitivity");
		Sn.Name = TEXT("mp_sens");
		Sn.Options = { TEXT("Low"), TEXT("Normal"), TEXT("High") };
		Page->Items.Add(Sn);
		// REQ-VOICEVOL (user 2026-10-05): the other players' voices, louder, and whether they fade with distance
		FItem Pv;
		Pv.Kind = EItemKind::Select;
		Pv.Label = TEXT("Partner volume");
		Pv.Name = TEXT("mp_pvol");
		Pv.Options = { TEXT("50%"), TEXT("100%"), TEXT("150%"), TEXT("200%"), TEXT("250%"), TEXT("300%") };
		Page->Items.Add(Pv);
		FItem Vf;
		Vf.Kind = EItemKind::Check;
		Vf.Label = TEXT("Voice fades with distance");
		Vf.Name = TEXT("mp_vfade");
		Page->Items.Add(Vf);
		FItem Bk;
		Bk.Kind = EItemKind::Action;
		Bk.Label = TEXT("Back");
		Bk.Name = TEXT("voice_back");
		Bk.bGoBack = true;
		Page->Items.Add(Bk);
	}
	else if (File == TEXT("#rando"))
	{
		// R10, the Randomizer page (user 2026-10-03): taken by the next New Game; each row saved at once
		using namespace OutlawsMenu;
		Page->File = File;
		Page->Title = TEXT("RANDOMIZER");
		const AOutlawsLevelActor* LR = Level.Get();
		// REQ-COOPRANDO: a co-op client's page shows the host's run (handed over with each start), nothing to change
		const UOutlawsCoop* CR = UOutlawsCoop::Get(this);
		if (CR && CR->IsClient())
		{
			const TCHAR* Modes[3][3] = { { TEXT("Off"), TEXT("Same strength"), TEXT("Anything") }, { TEXT("Off"), TEXT("Shuffle places"), TEXT("Re-roll") },
				{ TEXT("Off"), TEXT("Shuffle middle"), TEXT("Shuffle all") } };
			const bool bRun = LR && LR->RandoRun.bSet;
			auto Line = [&Page](const FString& Text)
			{
				FItem H;
				H.Kind = EItemKind::Heading;
				H.Label = Text;
				Page->Items.Add(H);
			};
			Line(FString::Printf(TEXT("Enemies: %s"), bRun ? Modes[0][FMath::Clamp(LR->RandoRun.Enemies, 0, 2)] : TEXT("Off")));
			Line(FString::Printf(TEXT("Items: %s"), bRun ? Modes[1][FMath::Clamp(LR->RandoRun.Items, 0, 2)] : TEXT("Off")));
			Line(FString::Printf(TEXT("Mission order: %s"), bRun ? Modes[2][FMath::Clamp(LR->RandoRun.Order, 0, 2)] : TEXT("Off")));
			Line(FString::Printf(TEXT("Bosses in the mix: %s"), bRun && LR->RandoRun.Bosses ? TEXT("On") : TEXT("Off")));
			Line(FString::Printf(TEXT("Critters: %s"), bRun && LR->RandoRun.Critters ? TEXT("On") : TEXT("Off")));
			if (LR && LR->RandoOn()) Line(FString::Printf(TEXT("Seed: %u"), LR->RandoRun.Seed));
			Line(TEXT("The host's settings (only the host can change them)"));
		}
		else
		{
			auto Sel = [&Page](const TCHAR* Label, const TCHAR* Name, TArray<FString> Options)
			{
				FItem I;
				I.Kind = EItemKind::Select;
				I.Label = Label;
				I.Name = Name;
				I.Options = MoveTemp(Options);
				Page->Items.Add(I);
			};
			Sel(TEXT("Enemies"), TEXT("rando_enemies"), { TEXT("Off"), TEXT("Same strength"), TEXT("Anything") });
			Sel(TEXT("Items"), TEXT("rando_items"), { TEXT("Off"), TEXT("Shuffle places"), TEXT("Re-roll") });
			Sel(TEXT("Mission order"), TEXT("rando_order"), { TEXT("Off"), TEXT("Shuffle middle"), TEXT("Shuffle all") });
			Sel(TEXT("Bosses in the mix"), TEXT("rando_bosses"), { TEXT("Off"), TEXT("On") });      // (REQ-RANDOBOSS: with Enemies on)
			Sel(TEXT("Critters"), TEXT("rando_critters"), { TEXT("Off"), TEXT("On") });            // (REQ-RANDOCRITTERS: squirrels, poodles, townsmen)
			FItem Sd;
			Sd.Kind = EItemKind::Action;
			Sd.Label = FString::Printf(TEXT("Seed:  %s"), LR && !LR->RandoSeedText.IsEmpty() ? *LR->RandoSeedText : TEXT("Random"));
			Sd.Name = TEXT("rando_seed");
			Page->Items.Add(Sd);
			if (LR && !LR->RandoSeedText.IsEmpty())
			{
				FItem Rn;
				Rn.Kind = EItemKind::Action;
				Rn.Label = TEXT("Use a random seed");
				Rn.Name = TEXT("rando_random");
				Page->Items.Add(Rn);
			}
			// the game being played: its own seed (fixed at its New Game), to note down and type in again
			if (LR && !LR->IsFrontEnd() && LR->RandoOn())
			{
				FItem Now;
				Now.Kind = EItemKind::Heading;
				Now.Label = FString::Printf(TEXT("This game's seed: %u"), LR->RandoRun.Seed);
				Page->Items.Add(Now);
			}
			FItem Note;
			Note.Kind = EItemKind::Heading;
			Note.Label = TEXT("From the next New Game (in co-op, the host's go to every player)");
			Note.Name = TEXT("rando_note");
			Page->Items.Add(Note);
		}
	}
	else if (File.StartsWith(TEXT("#mphost")))
	{
		BuildMpHostPage(File, *Page);      // M1: the Host Game page and its Locations (OutlawsPlayerMpHost.cpp)
	}
	else if (File == TEXT("#kbd"))
	{
		BuildKeyboardPage(*Page);          // the letters keyboard (OpenKeyboard)
	}
	else if (File.StartsWith(TEXT("#coop")))
	{
		UOutlawsCoop::BuildMenuPage(this, File, *Page);       // co-op: Host / Join, the join pad, the lobby (OutlawsCoop.cpp)
	}
	else if (File == TEXT("save_game.rml") || File == TEXT("load_game.rml"))
	{
		using namespace OutlawsMenu;
		const bool bSave = File == TEXT("save_game.rml");
		Page->File = File;
		Page->Title = MenuSource ? MenuSource->Loc(bSave ? TEXT("$m_local_5018_UC") : TEXT("$m_local_5017_UC")) : FString(bSave ? TEXT("SAVE GAME") : TEXT("LOAD GAME"));
		SlotsShown.Reset();
		auto AddSlot = [&](int32 Slot)
		{
			const AOutlawsLevelActor::FSaveInfo S = AOutlawsLevelActor::ReadSaveInfo(Slot);
			FSlotShown Shown;
			Shown.Slot = Slot;
			Shown.bEmpty = S.bEmpty;
			Shown.Level = S.Level;
			// (REQ-TITLEMENU: the autosaves on their own capsule art, named for what they are: the resume point, a co-op one)
			Shown.Name = Slot == -2 ? FString::Printf(TEXT("Co-op: %s"), *S.Name) : Slot == -1 && !bSave ? FString::Printf(TEXT("Resume: %s"), *S.Name) : S.Name;
			Shown.GameTime = S.GameTime;
			Shown.Stamp = S.Stamp;
			SlotsShown.Add(Shown);
			FItem I;
			I.Kind = EItemKind::Action;
			I.Label = S.bEmpty ? TEXT("Empty") : Shown.Name;
			I.Name = FString::Printf(TEXT("%s:%d"), bSave ? TEXT("save") : TEXT("load"), Slot);
			Page->Items.Add(I);
		};
		if (!bSave)
		{
			// REQ-TITLEMENU: the resume point (the last single-player autosave) first, then the other autosaves newest first (a
			// co-op host's own, R7), then the player's own saves
			if (AOutlawsLevelActor::HasAutosave()) AddSlot(-1);
			TArray<TPair<FDateTime, int32>> Autos;
			for (int32 a : { -2 })
			{
				if (!AOutlawsLevelActor::ReadSaveInfo(a).bEmpty) Autos.Emplace(IFileManager::Get().GetTimeStamp(*AOutlawsLevelActor::SavePath(a)), a);
			}
			Autos.StableSort([](const TPair<FDateTime, int32>& A, const TPair<FDateTime, int32>& B) { return A.Key > B.Key; });
			for (const TPair<FDateTime, int32>& A : Autos) AddSlot(A.Value);
		}
		for (int32 s = 0; s < AOutlawsLevelActor::NumSaveSlots; ++s) AddSlot(s);
	}
	else if (!MenuSource || !MenuSource->GetPage(File, *Page))
	{
		MenuToastText(FString::Printf(TEXT("%s is not in this Common.kpf"), *File));
		return false;
	}
	if (!File.StartsWith(TEXT("#")))
	{
		ScrubPage(*Page);
	}
	if (bPush && MenuPage)
	{
		MenuStack.Push(MenuPage->File);
	}
	if (File == TEXT("main_game.rml"))
	{
		// REQ-PAUSETIDY (user 2026-10-08: the pause page in multiplayer scrolled): the remaster's rows in their order, then
		// ours: Start the game under Resume (the host's warm-up), Players > before Options (co-op or a match: the Scoreboard,
		// the character, the team, the session's lines and Mute), Cheats > before Exit once a whole turn has shown it
		// (REQ-NOUNSTUCK, user 2026-10-27: the Unstuck row and the Tools page it shared with Cheats are gone; the rescue
		// itself is automatic, OutlawsPlayerUnstuck.cpp)
		using namespace OutlawsMenu;
		// Save, Load and Restart Level: single player's and the co-op host's (user 2026-10-08: the host restarts, saves and
		// loads for both); a co-op joiner and every match go without
		const UOutlawsCoop* Co = UOutlawsCoop::IsActive(this) ? UOutlawsCoop::Get(this) : nullptr;
		const AOutlawsLevelActor* LT = Level.Get();
		const bool bTutorial = LT && LT->IsTutorial();
		if (FOutlawsMpCharacter::bMatchMenu || (Co && (!Co->IsHost() || Co->IsMatch())) || bTutorial)       // (REQ-TUTORIAL: nothing saved there)
		{
			Page->Items.RemoveAll([](const FItem& It)
			{
				return It.Action == TEXT("ev_restart_level") || It.Target.Equals(TEXT("save_game.rml"), ESearchCase::IgnoreCase) || It.Target.Equals(TEXT("load_game.rml"), ESearchCase::IgnoreCase);
			});
		}
		if (bCheatsShown)
		{
			FItem I;
			I.Kind = EItemKind::Link;
			I.Label = TEXT("Cheats");
			I.Target = TEXT("#cheats");
			Page->Items.Insert(I, FMath::Max(0, Page->Items.Num() - 1));
		}
		if (bCheatsShown && OutlawsDevChecks::Enabled())
		{
			// REQ-DEVCHECKS: the headset checklist, under Cheats, behind the same turn and a dev flag (OutlawsDevChecks.h:
			// never in Shipping, never without -OLDevChecks or Saved/DevChecks/devchecks_on)
			FItem I;
			I.Kind = EItemKind::Link;
			I.Label = TEXT("Dev checks");
			I.Target = TEXT("#devchecks");
			Page->Items.Insert(I, FMath::Max(0, Page->Items.Num() - 1));
		}
		if (UOutlawsCoop::IsActive(this) || FOutlawsMpCharacter::bMatchMenu)
		{
			FItem I;
			I.Kind = EItemKind::Link;
			I.Label = TEXT("Players");
			I.Target = TEXT("#cooppause");
			const int32 Opt = Page->Items.IndexOfByPredicate([](const FItem& It) { return It.Target.Equals(TEXT("options.rml"), ESearchCase::IgnoreCase); });
			Page->Items.Insert(I, Opt != INDEX_NONE ? Opt : FMath::Max(0, Page->Items.Num() - 1));
		}
		if (OutlawsMpHost::Flow().GetState() == FOutlawsMatchFlow::EState::WarmUp)
		{
			// M1 (the host's warm-up): "Start the game", under Resume
			FItem I;
			I.Kind = EItemKind::Action;
			I.Label = TEXT("Start the game");
			I.Name = TEXT("mphost_matchstart");
			Page->Items.Insert(I, FMath::Min(1, Page->Items.Num()));
		}
		if (bTutorial)
		{
			// REQ-TUTORIAL: "Skip tutorial" at the top of the pause page, back to the main menu
			FItem I;
			I.Kind = EItemKind::Action;
			I.Label = TEXT("Skip tutorial");
			I.Name = TEXT("tutorial_skip");
			Page->Items.Insert(I, 0);
		}
		FString Rows;
		for (const FItem& It : Page->Items) Rows += (Rows.IsEmpty() ? TEXT("") : TEXT(" | ")) + It.Label;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT pause page rows (%d, %s): %s"), Page->Items.Num(), Page->Items.Num() > MenuRowsShown ? TEXT("scrolls") : TEXT("no scroll"), *Rows);
	}
	if (File == TEXT("options.rml"))
	{
		// ours: the VR settings page, first in the list
		OutlawsMenu::FItem I;
		I.Kind = OutlawsMenu::EItemKind::Link;
		I.Label = TEXT("VR");
		I.Target = TEXT("#vr");
		Page->Items.Insert(I, 0);
		// ours: the Multiplayer page under it (user 2026-10-02: the player name and the microphone)
		OutlawsMenu::FItem Mp;
		Mp.Kind = OutlawsMenu::EItemKind::Link;
		Mp.Label = TEXT("Multiplayer");
		Mp.Target = TEXT("#mp");
		Page->Items.Insert(Mp, 1);
		// (REQ-RANDOMOVE: the Randomizer row is on the Gameplay page now)
	}
	if (File == TEXT("options_gameplay.rml"))
	{
		// ours: the difficulty, changed in play (user 2026-10-02): the bad guys here take its stats at once, who is placed
		// follows from the next level
		if (const AOutlawsLevelActor* LD = Level.Get(); LD && !LD->IsFrontEnd())
		{
			OutlawsMenu::FItem Df;
			Df.Kind = OutlawsMenu::EItemKind::Select;
			Df.Label = TEXT("Difficulty");
			Df.Name = TEXT("difficulty");
			Df.Options = { TEXT("Good"), TEXT("Bad"), TEXT("Ugly") };
			Page->Items.Add(Df);
		}
		// ours: the autosave (user 2026-09-27): on at the start of each level and every few minutes
		OutlawsMenu::FItem A;
		A.Kind = OutlawsMenu::EItemKind::Check;
		A.Label = MenuSource ? MenuSource->Loc(TEXT("$m_autosave")) : FString(TEXT("Auto Save"));
		A.Name = TEXT("autosave");
		Page->Items.Add(A);
		OutlawsMenu::FItem F;
		F.Kind = OutlawsMenu::EItemKind::Select;
		F.Label = TEXT("Autosave every");
		F.Name = TEXT("autosave_minutes");
		F.Options = { TEXT("1 minute"), TEXT("2 minutes"), TEXT("3 minutes"), TEXT("5 minutes"), TEXT("10 minutes"), TEXT("15 minutes") };
		Page->Items.Add(F);
		// REQ-SPMULT (user 2026-10-05): co-op's bad guys multiplier in single player, the same quarters up to four times
		// (UNLOCKFREE, user decision for 0.95: there from the first start, x1 until the player sets it)
		{
			OutlawsMenu::FItem Bg;
			Bg.Kind = OutlawsMenu::EItemKind::Select;
			Bg.Label = TEXT("Bad guys");
			Bg.Name = TEXT("solo_badguys");
			for (int32 q = 4; q <= 16; ++q) Bg.Options.Add(q == 4 ? FString(TEXT("x1 (off)")) : FString::Printf(TEXT("x%.2f"), q / 4.f));
			Page->Items.Add(Bg);
		}
		OutlawsMenu::FItem D;
		D.Kind = OutlawsMenu::EItemKind::Action;
		D.Label = TEXT("Restore defaults");
		D.Name = TEXT("defaults:gameplay");
		Page->Items.Add(D);
		// ours (R10): the Randomizer page (UNLOCKFREE: there from the first start, everything on it Off until set); REQ-RANDOMOVE
		// (user 14 Oct): here at the bottom (above a Back row, where the page has one) instead of on the Options page; Back from it
		// returns here
		{
			OutlawsMenu::FItem Rd;
			Rd.Kind = OutlawsMenu::EItemKind::Link;
			Rd.Label = TEXT("Randomizer");
			Rd.Target = TEXT("#rando");
			const int32 Bk = Page->Items.IndexOfByPredicate([](const OutlawsMenu::FItem& It) { return It.bGoBack; });
			Page->Items.Insert(Rd, Bk != INDEX_NONE ? Bk : Page->Items.Num());
		}
	}
	if (File == TEXT("options_sound.rml"))
	{
		// ours: the remaster's HD sound effects or the originals (the remaster has no such switch)
		OutlawsMenu::FItem I;
		I.Kind = OutlawsMenu::EItemKind::Check;
		I.Label = TEXT("Remastered Sounds");
		I.Name = TEXT("snd_remastered");
		Page->Items.Add(I);
	}
	// a settings page (display, sound) gets its own Restore defaults at the end
	{
		bool bDisplay = false, bSound = false;
		for (const OutlawsMenu::FItem& It : Page->Items)
		{
			bDisplay |= It.Name == TEXT("r_remasteredmode") || It.Name == TEXT("r_tinting");
			bSound |= It.Name.StartsWith(TEXT("snd_"));
		}
		if (bDisplay && !Page->Items.ContainsByPredicate([](const OutlawsMenu::FItem& It) { return It.Name == TEXT("r_stochastic"); }))
		{
			// ours: stochastic texturing on natural ground and rock (user 2026-09-28)
			OutlawsMenu::FItem S;
			S.Kind = OutlawsMenu::EItemKind::Check;
			S.Label = TEXT("Stochastic Textures");
			S.Name = TEXT("r_stochastic");
			Page->Items.Add(S);
		}
		if (bDisplay && !Page->Items.ContainsByPredicate([](const OutlawsMenu::FItem& It) { return It.Name == TEXT("r_gamma"); }))
		{
			// ours: REQ-GAMMA, a slider like the speed and the volumes (each press a step, live; OutlawsDeviceProfile::Gamma)
			OutlawsMenu::FItem G;
			G.Kind = OutlawsMenu::EItemKind::Range;
			G.Label = TEXT("Brightness");
			G.Name = TEXT("r_gamma");
			G.Min = 0.7f;
			G.Max = 1.6f;
			G.Step = 0.1f;
			Page->Items.Add(G);
		}
		if (bDisplay && !Page->Items.ContainsByPredicate([](const OutlawsMenu::FItem& It) { return It.Name == TEXT("r_resolution"); }))
		{
			// ours: the performance levers for older headsets (user 2026-10-03; OutlawsDeviceProfile): render resolution, the
			// display's refresh rate (only when the headset offers rates), the lamps that light rooms, a frame-rate HUD
			auto Row = [&Page](const TCHAR* Label, const TCHAR* Name, bool bCheck)
			{
				OutlawsMenu::FItem R;
				R.Kind = bCheck ? OutlawsMenu::EItemKind::Check : OutlawsMenu::EItemKind::Select;
				R.Label = Label;
				R.Name = Name;
				Page->Items.Add(R);
			};
			Row(TEXT("Resolution"), TEXT("r_resolution"), false);
			if (OutlawsDeviceProfile::RefreshRates().Num() > 0) Row(TEXT("Refresh Rate"), TEXT("r_refresh"), false);
			Row(TEXT("Lamp Lights"), TEXT("r_lamps"), false);
			Row(TEXT("Object Detail"), TEXT("r_objdetail"), false);
			// (QSPEED-5) the Quest 2 profile on a Quest 3, to see the Quest 2's look and speed (off by default)
			if (OutlawsDeviceProfile::ShowQuest2ModeRow()) Row(TEXT("Quest 2 Mode"), TEXT("r_quest2mode"), true);
			Row(TEXT("Performance HUD"), TEXT("r_perfhud"), true);
			Row(TEXT("Grab Glow"), TEXT("r_grabglow"), true);
#if !PLATFORM_ANDROID
			// (REQ-SPECCAM) the PC window's view, only with a headset on the PC (the desktop game's window is its own view)
			if (bVR || SpecCamDesktop()) Row(TEXT("PC Window View"), TEXT("r_pcview"), false);
#endif
		}
		if (bDisplay || bSound)
		{
			OutlawsMenu::FItem D;
			D.Kind = OutlawsMenu::EItemKind::Action;
			D.Label = TEXT("Restore defaults");
			D.Name = bDisplay ? TEXT("defaults:display") : TEXT("defaults:sound");
			Page->Items.Add(D);
		}
	}
	MenuPage = Page;
	MenuScroll = 0;
	MenuBackHover[0] = MenuBackHover[1] = false;     // (REQ-BACKCLICK: lit again only once a laser finds the new page's Back)
	if (bMenuHoverHeld)
	{
		// (REQ-VOICEPAGE: a row the menu lit belongs to its page)
		bMenuHoverHeld = false;
		MenuHover[0] = MenuHover[1] = -1;
	}
	SetSheetShape(IsSavePage() || IsFrontPage(), !File.Equals(TEXT("main_game.rml"), ESearchCase::IgnoreCase));
	FrontHits.Reset();
	FrontItemInk.Reset();
	MenuTitle->SetText(FText::FromString(Page->Title.IsEmpty() ? File : Page->Title));
	MenuMessage->SetText(FText::FromString(Wrap(Page->Message, 44)));
	RefreshMenuRows();
	return true;
}

const float* AOutlawsPlayer::LiveSetting(const FString& Name) const
{
	if (Name == TEXT("r_gamma")) return OutlawsDeviceProfile::GammaSetting();     // (REQ-GAMMA: the machine's, not the level's)
	AOutlawsLevelActor* L = Level.Get();
	if (!L) return nullptr;
	if (Name == TEXT("vr_speed")) return &L->GameSpeed;
	if (Name == TEXT("snd_mastervolume")) return &L->MasterVolume;
	if (Name == TEXT("snd_volume")) return &L->SoundVolume;
	if (Name == TEXT("snd_musicvolume")) return &L->MusicVolume;
	if (Name == TEXT("snd_movievolume")) return &L->MovieVolume;
	return nullptr;
}

bool AOutlawsPlayer::ItemWorks(const OutlawsMenu::FItem& I, TSet<FString>& Visiting) const
{
	using namespace OutlawsMenu;
	static const TSet<FString> Actions = { TEXT("ev_close_menu"), TEXT("ev_restart_level"), TEXT("ev_end_game"), TEXT("ev_quit"), TEXT("ev_dialog_close"),
		TEXT("ev_restart_level_confirm"), TEXT("ev_quit_confirm"), TEXT("ev_save_game_confirm"), TEXT("ev_load_game_confirm"), TEXT("ev_exit_game_confirm") };
	static const TSet<FString> Settings = { TEXT("r_remasteredmode"), TEXT("r_tinting"), TEXT("r_stochastic"), TEXT("snd_remastered"), TEXT("snd_mastervolume"),
		TEXT("snd_volume"), TEXT("snd_musicvolume"), TEXT("snd_movievolume"), TEXT("r_resolution"), TEXT("r_refresh"), TEXT("r_lamps"), TEXT("r_perfhud"), TEXT("r_grabglow"), TEXT("r_objdetail"), TEXT("r_gamma"), TEXT("r_pcview"), TEXT("r_quest2mode") };
	switch (I.Kind)
	{
	case EItemKind::Link:
	case EItemKind::Dialog:
		if (I.Target.StartsWith(TEXT("#"))) return true;
		return PageWorks(I.Target, Visiting);
	case EItemKind::Action:
	case EItemKind::Button:
		return I.bGoBack || Actions.Contains(I.Action) || I.Name.StartsWith(TEXT("cheat_")) || I.Name.StartsWith(TEXT("level:")) || I.Name.StartsWith(TEXT("defaults:"))
			|| I.Name.StartsWith(TEXT("save:")) || I.Name.StartsWith(TEXT("load:")) || I.Name.StartsWith(TEXT("start:")) || I.Name.StartsWith(TEXT("char:"));
	case EItemKind::Check:
	case EItemKind::Select:
	case EItemKind::Range:
		return Settings.Contains(I.Name) || I.Name.StartsWith(TEXT("vr_")) || I.Name.StartsWith(TEXT("cheat_")) || I.Name.StartsWith(TEXT("autosave"))
			|| I.Name == TEXT("solo_badguys");
	case EItemKind::Heading:
		return true;           // kept only when something under it stays (ScrubPage)
	default:
		return false;          // key bindings and the like: not in the VR build
	}
}

bool AOutlawsPlayer::PageWorks(const FString& File, TSet<FString>& Visiting) const
{
	if (File.StartsWith(TEXT("#"))) return true;
	// ours: the save and load screens, the gameplay page's autosave
	if (File == TEXT("save_game.rml") || File == TEXT("load_game.rml") || File == TEXT("options_gameplay.rml")) return true;
	if (Visiting.Contains(File) || !MenuSource) return false;
	Visiting.Add(File);
	OutlawsMenu::FPage P;
	bool bAny = false;
	if (MenuSource->GetPage(File, P))
	{
		if (File == TEXT("options_sound.rml") || File == TEXT("options.rml")) bAny = true;   // ours are added there
		for (const OutlawsMenu::FItem& I : P.Items)
		{
			if (I.Kind != OutlawsMenu::EItemKind::Heading && !I.bGoBack && I.Action != TEXT("ev_dialog_close") && ItemWorks(I, Visiting))
			{
				bAny = true;
				break;
			}
		}
	}
	Visiting.Remove(File);
	return bAny;
}

void AOutlawsPlayer::ScrubPage(OutlawsMenu::FPage& Page) const
{
	// the options that do nothing in the VR build (yet) are left out, and headings with nothing under them
	TSet<FString> Visiting = { Page.File };
	TArray<OutlawsMenu::FItem> Kept;
	for (const OutlawsMenu::FItem& I : Page.Items)
	{
		if (ItemWorks(I, Visiting)) Kept.Add(I);
	}
	for (int32 i = Kept.Num() - 1; i >= 0; --i)
	{
		if (Kept[i].Kind == OutlawsMenu::EItemKind::Heading && (i == Kept.Num() - 1 || Kept[i + 1].Kind == OutlawsMenu::EItemKind::Heading))
		{
			Kept.RemoveAt(i);
		}
	}
	// End Game straight under Restart Level (user 2026-10-01: back to the main menu, saved first); the pause page gets one if its
	// source has none
	const int32 Restart = Kept.IndexOfByPredicate([](const OutlawsMenu::FItem& It) { return It.Action == TEXT("ev_restart_level"); });
	if (Restart != INDEX_NONE)
	{
		const int32 End = Kept.IndexOfByPredicate([](const OutlawsMenu::FItem& It) { return It.Action == TEXT("ev_end_game"); });
		OutlawsMenu::FItem EndItem;
		if (End != INDEX_NONE)
		{
			EndItem = Kept[End];
			Kept.RemoveAt(End);
		}
		else
		{
			EndItem.Kind = OutlawsMenu::EItemKind::Action;
			EndItem.Label = TEXT("End Game");
			EndItem.Action = TEXT("ev_end_game");
		}
		const int32 After = Kept.IndexOfByPredicate([](const OutlawsMenu::FItem& It) { return It.Action == TEXT("ev_restart_level"); });
		Kept.Insert(EndItem, After + 1);
	}
	Page.Items = MoveTemp(Kept);
}

FString AOutlawsPlayer::MenuRowText(int32 Index) const
{
	using namespace OutlawsMenu;
	const FItem& I = MenuPage->Items[Index];
	const FString Key = MenuPage->File + TEXT(":") + FString::FromInt(Index);
	const int32* V = MenuValues.Find(Key);
	if (I.Name == TEXT("mp_meter"))
	{
		// (R8) twenty bars from about -50 dB to -10 dB, and what the voice chat would do with it
		const AOutlawsLevelActor* LM = Level.Get();
		if (!LM || !LM->bMpMicrophone) return TEXT("Level:  (the microphone is off)");
		const float Lv = UOutlawsCoop::MicMeterLevel(this);
		const float Db = Lv > 0.f ? 20.f * FMath::LogX(10.f, Lv) : -100.f;
		const int32 Bars = FMath::Clamp(FMath::RoundToInt((Db + 50.f) / 2.f), 0, 20);
		const bool bSend = UOutlawsCoop::MicMeterSending(this);
		return FString::Printf(TEXT("Level:  %s%s  %s"), *FString::ChrN(Bars, 'I'), *FString::ChrN(20 - Bars, '-'),
			bSend ? TEXT("sending") : LM->MpPushToTalk > 0 ? TEXT("hold the grip to talk") : TEXT("say something"));
	}
	switch (I.Kind)
	{
	case EItemKind::Link:
	case EItemKind::Dialog:
		return I.Label + TEXT("  >");
	case EItemKind::Heading:
		if (I.Name == TEXT("vr_speedwarn")) return I.Label;          // (BUG-SPEEDWARN: a note, not a section)
		return TEXT("- ") + I.Label + TEXT(" -");
	case EItemKind::Select:
	{
		// the Graphics page's performance rows: their values in effect
		if (OutlawsDeviceProfile::IsRow(I.Name)) return FString::Printf(TEXT("%s:  %s"), *I.Label, *OutlawsDeviceProfile::RowValue(I.Name));
		// the VR page shows the live settings
		int32 Shown = V ? *V : 0;
		if (I.Name == TEXT("vr_movefollow")) Shown = MoveDirection == EOutlawsMoveDirection::DominantHand ? 1 : MoveDirection == EOutlawsMoveDirection::OffHand ? 2 : 0;
		else if (I.Name == TEXT("vr_turn")) Shown = bSnapTurn ? 0 : 1;
		else if (I.Name.StartsWith(TEXT("devcheck:"))) Shown = OutlawsDevChecks::State(I.Name.Mid(9));
		else if (I.Name == TEXT("mp_ptt")) Shown = Level.IsValid() ? Level->MpPushToTalk : 0;
		else if (I.Name == TEXT("mp_sens")) Shown = Level.IsValid() ? Level->MpMicSensitivity : 1;
		else if (I.Name == TEXT("mp_pvol")) Shown = Level.IsValid() ? FMath::Clamp(Level->MpPartnerVolume / 50 - 1, 0, 5) : 3;
		else if (I.Name == TEXT("rando_enemies")) Shown = Level.IsValid() ? Level->RandoEnemies : 0;
		else if (I.Name == TEXT("rando_items")) Shown = Level.IsValid() ? Level->RandoItems : 0;
		else if (I.Name == TEXT("rando_order")) Shown = Level.IsValid() ? Level->RandoOrder : 0;
		else if (I.Name == TEXT("rando_bosses")) Shown = Level.IsValid() ? Level->RandoBosses : 0;
		else if (I.Name == TEXT("rando_critters")) Shown = Level.IsValid() ? Level->RandoCritters : 0;
		else if (I.Name == TEXT("vr_locomotion")) Shown = bTeleportMove ? 1 : 0;
		else if (I.Name == TEXT("vr_domhand")) Shown = LeftDominant() ? 1 : 0;
		else if (I.Name == TEXT("vr_onectrl")) Shown = OneController;
		else if (I.Name == TEXT("difficulty")) Shown = Level.IsValid() ? int32(Level->Difficulty) : 0;
		else if (I.Name == TEXT("solo_badguys")) Shown = Level.IsValid() ? FMath::Clamp(FMath::RoundToInt(Level->SoloBadGuys * 4.f) - 4, 0, 12) : 0;
		else if (I.Name == TEXT("autosave_minutes"))
		{
			static const float Minutes[] = { 1.f, 2.f, 3.f, 5.f, 10.f, 15.f };
			const AOutlawsLevelActor* LA = Level.Get();
			Shown = 1;
			for (int32 k = 0; k < UE_ARRAY_COUNT(Minutes); ++k) if (LA && FMath::IsNearlyEqual(LA->AutosaveMinutes, Minutes[k])) Shown = k;
		}
		return I.Options.Num() > 0 ? FString::Printf(TEXT("%s:  %s"), *I.Label, *I.Options[Shown % I.Options.Num()]) : I.Label;
	}
	case EItemKind::Check:
	{
		// the cheats show their real state
		const AOutlawsLevelActor* L = Level.Get();
		bool bOn = V && *V;
		if (I.Name == TEXT("cheat_god")) bOn = L && L->bInvincible;
		else if (I.Name == TEXT("cheat_ammo")) bOn = bUnlimitedAmmo;
		else if (I.Name == TEXT("cheat_freeze")) bOn = L && !L->bEnemiesActive;
		else if (I.Name == TEXT("r_remasteredmode")) bOn = L && L->bHDTextures;
		else if (I.Name == TEXT("snd_remastered")) bOn = L && L->bHDSounds;
		else if (I.Name == TEXT("r_tinting")) bOn = L && L->bPaletteGrades;
		else if (I.Name == TEXT("r_stochastic")) bOn = L && L->bStochastic;
		else if (I.Name == TEXT("r_perfhud")) bOn = OutlawsDeviceProfile::PerfHud();
		else if (I.Name == TEXT("r_grabglow")) bOn = OutlawsDeviceProfile::GrabGlow();
		else if (I.Name == TEXT("r_quest2mode")) bOn = OutlawsDeviceProfile::IsQuest2();
		else if (I.Name == TEXT("vr_facereload")) bOn = bFaceReload;
		else if (I.Name == TEXT("autosave")) bOn = L && L->bAutosave;
		else if (I.Name == TEXT("mp_announce") || I.Name == TEXT("mp_listen")) bOn = UOutlawsBeacon::Setting(I.Name);
		else if (I.Name == TEXT("mp_upnp")) bOn = UOutlawsCoop::UpnpSetting();
		else if (I.Name == TEXT("mp_vfade")) bOn = L && L->bMpVoiceFade;
		else if (I.Name == TEXT("mp_mic"))
		{
			// on, but the headset's permission refused or no microphone found: said beside it (the voice chat's state)
			bOn = L && L->bMpMicrophone;
			const EOutlawsCoopMic Mic = UOutlawsCoop::MicState(this);
			if (bOn && Mic != EOutlawsCoopMic::On && Mic != EOutlawsCoopMic::Off) return FString::Printf(TEXT("%s:  On (%s)"), *I.Label, UOutlawsCoop::MicStateText(Mic));
		}
		return FString::Printf(TEXT("%s:  %s"), *I.Label, bOn ? TEXT("On") : TEXT("Off"));
	}
	case EItemKind::Radio:
		return FString::Printf(TEXT("%s %s"), (V && *V) ? TEXT("(o)") : TEXT("( )"), *I.Label);
	case EItemKind::Range:
	{
		if (I.Name == TEXT("r_gamma")) return FString::Printf(TEXT("%s:  %s"), *I.Label, *OutlawsDeviceProfile::GammaText());
		if (const float* Live = LiveSetting(I.Name))
		{
			// (the movement speed reads as twice its number: its 0.5 is "100 %", the default; user 2026-10-02)
			return FString::Printf(TEXT("%s:  %d%%"), *I.Label, FMath::RoundToInt(*Live * (I.Name == TEXT("vr_speed") ? 200.f : 100.f)));
		}
		// ten steps across the control's own range; 0..1 controls read as percent, the rest as numbers
		const int32 Step = V ? *V : (I.Name.Contains(TEXT("volume")) ? 10 : 5);
		const float Value = I.Min + (I.Max - I.Min) * Step / 10.f;
		return I.Max <= 1.f ? FString::Printf(TEXT("%s:  %d%%"), *I.Label, FMath::RoundToInt(Value * 100.f))
			: I.Max <= 10.f ? FString::Printf(TEXT("%s:  %.2f"), *I.Label, Value)
			: FString::Printf(TEXT("%s:  %d"), *I.Label, FMath::RoundToInt(Value));
	}
	case EItemKind::Bind:
		return FString::Printf(TEXT("%s:  (VR controls)"), *I.Label);
	default:
		return I.Label;
	}
}

void AOutlawsPlayer::RefreshMenuRows()
{
	if (!MenuPage)
	{
		return;
	}
	const int32 N = MenuPage->Items.Num();
	// dialogs put their message where the first rows would be: their buttons go lower
	const int32 First = MenuPage->Message.IsEmpty() ? 0 : 4;
	const int32 Fit = SheetRowsFit();          // (REQ-MPLOBBY c: a row that would reach off the sheet waits for the scroll)
	for (int32 r = 0; r < MenuRows.Num(); ++r)          // (REQ-SUBMENUSPACE: the rows past this page's count left empty)
	{
		const int32 i = MenuScroll + r - First;
		MenuRows[r]->SetText(FText::FromString(r < Fit && r < PageRowsShown() && r >= First && MenuPage->Items.IsValidIndex(i) ? MenuRowText(i) : FString()));
	}
	MenuMoreUp->SetText(FText::FromString(MenuScroll > 0 ? TEXT("more above") : TEXT("")));
	MenuMoreDown->SetText(FText::FromString(MenuScroll + Fit - First < N ? TEXT("more below") : TEXT("")));
	RedrawMenu();
}

int32 AOutlawsPlayer::MenuPointerHand() const
{
	// (RC1.1, user 2026-10-05: a tester with only a working left controller could not point at the title screens)
	if (OneHand() >= 0) return OneHand();    // one controller: that hand points (REQ-ONECTRL)
	const int32 Dom = LeftDominant() ? 0 : 1;
	const UMotionControllerComponent* DomAim = AimOf(Dom);
	const UMotionControllerComponent* OtherAim = AimOf(1 - Dom);
	return DomAim && !DomAim->IsTracked() && OtherAim && OtherAim->IsTracked() ? 1 - Dom : Dom;
}

void AOutlawsPlayer::RedrawMenu()
{
	if (MenuCanvas) MenuCanvas->UpdateResource();
	if (MenuMask) MenuMask->UpdateResource();
}

void AOutlawsPlayer::DrawMenuMask(UCanvas* C, int32 Width, int32 Height)
{
	bMenuMaskPass = true;
	DrawMenuCanvas(C, Width, Height);
	bMenuMaskPass = false;
}

UFont* AOutlawsPlayer::KpfFont(const FString& KpfPath, int32 Size)
{
	TArray<uint8> Bytes;
	if (!LoadMenuSource() || !MenuSource->ReadFile(KpfPath, Bytes) || Bytes.Num() == 0)
	{
		return nullptr;
	}
	// the player's own font file, copied out of their Common.kpf so the font system can open it
	const FString Disk = FPaths::ProjectSavedDir() / TEXT("KpfCache") / FPaths::GetCleanFilename(KpfPath);
	if (IFileManager::Get().FileSize(*Disk) != Bytes.Num())
	{
		FFileHelper::SaveArrayToFile(Bytes, *Disk);
	}
	UFont* F = NewObject<UFont>(this);
	F->FontCacheType = EFontCacheType::Runtime;
	F->LegacyFontSize = Size;
	FTypefaceEntry& E = F->CompositeFont.DefaultTypeface.Fonts.AddDefaulted_GetRef();
	E.Name = TEXT("Regular");
	E.Font = FFontData(Disk, EFontHinting::Default, EFontLoadingPolicy::LazyLoad);
	// the remaster's display fonts carry only letters and digits (66 glyphs): punctuation and anything
	// else comes from a full font in the same file, as the remaster's own fallback does
	TArray<uint8> Full;
	const FString FullPath = TEXT("fonts/NotoSans-ExtraCondensedMedium.ttf");
	if (MenuSource->ReadFile(FullPath, Full) && Full.Num() > 0)
	{
		const FString FullDisk = FPaths::ProjectSavedDir() / TEXT("KpfCache") / FPaths::GetCleanFilename(FullPath);
		if (IFileManager::Get().FileSize(*FullDisk) != Full.Num()) FFileHelper::SaveArrayToFile(Full, *FullDisk);
		FCompositeSubFont& Sub = F->CompositeFont.SubTypefaces.AddDefaulted_GetRef();
		for (const FIntPoint& R : { FIntPoint(0x21, 0x2F), FIntPoint(0x3A, 0x40), FIntPoint(0x5B, 0x60), FIntPoint(0x7B, 0xFFFF) })
		{
			Sub.CharacterRanges.Add(FInt32Range(FInt32Range::BoundsType::Inclusive(R.X), FInt32Range::BoundsType::Inclusive(R.Y)));
		}
		FTypefaceEntry& FE = Sub.Typeface.Fonts.AddDefaulted_GetRef();
		FE.Name = TEXT("Regular");
		FE.Font = FFontData(FullDisk, EFontHinting::Default, EFontLoadingPolicy::LazyLoad);
	}
	return F;
}

UTexture2D* AOutlawsPlayer::KpfDds(const FString& KpfPath)
{
	// uncompressed 32-bit DDS (the menu art): 128-byte header, BGRA rows
	TArray<uint8> D;
	if (!LoadMenuSource() || !MenuSource->ReadFile(KpfPath, D) || D.Num() < 128 || FMemory::Memcmp(D.GetData(), "DDS ", 4) != 0)
	{
		return nullptr;
	}
	int32 H = 0, W = 0;
	uint32 Bits = 0, RMask = 0;
	FMemory::Memcpy(&H, D.GetData() + 12, 4);
	FMemory::Memcpy(&W, D.GetData() + 16, 4);
	FMemory::Memcpy(&Bits, D.GetData() + 88, 4);
	FMemory::Memcpy(&RMask, D.GetData() + 92, 4);
	if (Bits != 32 || W <= 0 || H <= 0 || D.Num() < 128 + W * H * 4)
	{
		return nullptr;
	}
	TArray<FColor> Px;
	Px.SetNumUninitialized(W * H);
	FMemory::Memcpy(Px.GetData(), D.GetData() + 128, int64(W) * H * 4);
	if (RMask == 0xFF)
	{
		for (FColor& C : Px) Swap(C.R, C.B);     // RGBA in the file: FColor is BGRA
	}
	return OutlawsTextures::MakeTextureBGRA(W, H, Px, true, TF_Trilinear, TA_Clamp, true);     // (BUG-MENUBLACK) menu art keeps its CPU copy
}

void AOutlawsPlayer::DrawMenuCanvas(UCanvas* C, int32 Width, int32 Height)
{
	if (!C || !MenuPage)
	{
		return;
	}
	if (!bMenuMaskPass) { UpdateMenuHelpPlate(FString()); SheetBackInk = FBox2D(ForceInit); }     // (hidden unless the page puts its help line there)
	if (IsSavePage())
	{
		if (IsAchievPage()) DrawAchievCanvas(C, Width, Height);      // (REQ-EXTRAS)
		else DrawSaveCanvas(C, Width, Height);
		return;
	}
	if (IsFrontPage())
	{
		DrawFrontCanvas(C, Width, Height);
		return;
	}
	const float S = Width / PanelW;                              // pixels per cm on the sheet
	const float SheetHalf = PageSheetH() * 0.5f;                  // (REQ-SUBMENUSPACE: a titled page's sheet is taller)
	const float Lift = IsTallSheet() ? TallLift : 0.f;
	auto Y = [&](float Zcm) { return (SheetHalf - Zcm) * S; };
	// text only, floating over the world, in the remaster's pause-menu style (dialog.rcss dialog_link):
	// Montagu Slab, black fill with a dulled-yellow outline (#d8cb56); pointed at: orange (#e9931c).
	// Colours are linear (FLinearColor::FromSRGBColor) into the sRGB target
	const float ColX = 0.5f * Width, ColW = 0.9f * Width;
	const FLinearColor Ink = FLinearColor::FromSRGBColor(FColor(8, 6, 4));
	const FLinearColor Outline = FLinearColor::FromSRGBColor(FColor(0xd8, 0xcb, 0x56));
	const FLinearColor Hot = FLinearColor::FromSRGBColor(FColor(0xe9, 0x93, 0x1c));
	const FLinearColor Red = Hot, Faint = Outline;
	// (REQ-MPLOBBY c: where each row, the title and the help line went, for the fit check; the colour pass only)
	if (!bMenuMaskPass)
	{
		SheetRowInk.Reset();
		SheetTitleInk = FBox2D(ForceInit);
		SheetFootInk.Reset();
		SheetEdgeInk.Reset();
		SheetMoreUpInk = FBox2D(ForceInit);
		SheetCanvasSize = FIntPoint(Width, Height);
		if (MenuSmallFont)
		{
			float XL = 0.f, YL = 0.f;
			C->TextSize(MenuSmallFont, TEXT("Ag"), XL, YL);
			if (YL > 0.f) SheetSmallHalfPx = YL * 0.31f;
		}
	}
	auto Text = [&](UFont* Font, const FString& Str, float X, float Yc, const FLinearColor& Colour, float MaxW) -> FBox2D
	{
		if (!Font || Str.IsEmpty()) return FBox2D(ForceInit);
		FBox2D Glyphs;
		if (DrawGlyphLine(C, Str, Font, FVector2D(X, Yc), Colour, Colour.Equals(Hot) ? FLinearColor::Black : Outline, bMenuMaskPass, Glyphs)) return Glyphs;   // (REQ-BUTTONGLYPHS)
		float XL = 0.f, YL = 0.f;
		C->TextSize(Font, Str, XL, YL);
		const float Scale = XL > MaxW ? MaxW / XL : 1.f;
		// the coverage pass draws everything white
		FCanvasTextItem Item(FVector2D(X, Yc), FText::FromString(Str), Font, bMenuMaskPass ? FLinearColor::White : Colour);
		Item.Scale = FVector2D(Scale, Scale);
		Item.bCentreX = Item.bCentreY = true;
		Item.bOutlined = true;
		Item.OutlineColor = bMenuMaskPass ? FLinearColor::White : Colour.Equals(Hot) ? FLinearColor::Black : Outline;
		Item.BlendMode = SE_BLEND_Translucent;
		C->DrawItem(Item);
		// (the letters' own height: the face's line height is about 1.6 of its size, the letters, capitals to descenders, about
		// 0.62 of the line; the width as drawn)
		return FBox2D(FVector2D(X - XL * Scale * 0.5f, Yc - YL * Scale * 0.31f), FVector2D(X + XL * Scale * 0.5f, Yc + YL * Scale * 0.31f));
	};
	const bool bInk = !bMenuMaskPass;
	// the page's name (the pause page shows none, as the remaster's); (BUG-GFXTALL) a titled page paged down shows "more above"
	// in its place, over the first row, so the block is no taller than at the top
	const bool bTitleMakesWay = IsTallSheet() && MenuScroll > 0;
	if (!MenuPage->File.Equals(TEXT("main_game.rml"), ESearchCase::IgnoreCase) && !bTitleMakesWay)
	{
		const FBox2D T = Text(MenuTitleFont, MenuTitle ? MenuTitle->Text.ToString() : FString(), ColX, Y(17.5f + Lift), Ink, ColW);   // black on the yellow outline, as the rows
		if (bInk) SheetTitleInk = T;
		if (bInk && T.bIsValid) SheetEdgeInk.Add(T);
	}
	// a dialog's message, a line at a time
	if (MenuMessage && !MenuMessage->Text.IsEmpty())
	{
		TArray<FString> Lines;
		MenuMessage->Text.ToString().ParseIntoArrayLines(Lines);
		for (int32 i = 0; i < Lines.Num(); ++i) Text(MenuSmallFont, Lines[i], ColX, Y(PageRowTop() - PageRowStep() * (0.5f + i * 0.8f)), Ink, ColW);
	}
	// the rows: the one pointed at turns orange
	for (int32 r = 0; r < MenuRows.Num(); ++r)
	{
		const bool bHot = MenuHover[0] == r || MenuHover[1] == r;
		const FString RowText = MenuRows[r]->Text.ToString();
		const int32 Item = MenuScroll + r - (MenuPage->Items.Num() > 0 ? 0 : 0);
		// (BUG-SPEEDWARN) the speed warning: its two lines in the small font, in the warning's colour, in its row's place
		if (!RowText.IsEmpty() && MenuPage->Items.IsValidIndex(Item - (MenuPage->Message.IsEmpty() ? 0 : 4)) && MenuPage->Items[Item - (MenuPage->Message.IsEmpty() ? 0 : 4)].Name == TEXT("vr_speedwarn"))
		{
			TArray<FString> Lines;
			RowText.ParseIntoArrayLines(Lines);
			FBox2D Note(ForceInit);
			for (int32 i = 0; i < Lines.Num(); ++i)
			{
				const FBox2D B = Text(MenuSmallFont, Lines[i], ColX, Y(RowZ(r) + SpeedWarnLine * ((Lines.Num() - 1) * 0.5f - i)), Red, ColW * 0.95f);
				if (B.bIsValid) Note += B;
			}
			if (bInk && Note.bIsValid) SheetRowInk.Add(Note);
			continue;
		}
		float Speed = -1.f;
		if (RowText.StartsWith(TEXT("Movement speed")))
		{
			if (AOutlawsLevelActor* LS = Level.Get()) Speed = LS->GameSpeed;
		}
		int32 Colon = INDEX_NONE;
		if (Speed > 0.605f && RowText.FindLastChar(TEXT(':'), Colon))
		{
			// yellow at 0.65, orange at 0.7, red at the top, 0.75 (shown as 130, 140 and 150 %), blended between
			const FLinearColor Yel = FLinearColor::FromSRGBColor(FColor(0xFF, 0xE0, 0x40));
			const FLinearColor Org = FLinearColor::FromSRGBColor(FColor(0xFF, 0x98, 0x1E));
			const FLinearColor RedC = FLinearColor::FromSRGBColor(FColor(0xE8, 0x28, 0x18));
			const FLinearColor Risk = Speed < 0.7f ? FMath::Lerp(Yel, Org, (Speed - 0.61f) / 0.09f) : FMath::Lerp(Org, RedC, FMath::Clamp((Speed - 0.7f) / 0.05f, 0.f, 1.f));
			const FString Head = RowText.Left(Colon + 1) + TEXT("  ");
			const FString Num = RowText.Mid(Colon + 1).TrimStart();
			float HW = 0.f, NW = 0.f, TH = 0.f;
			C->TextSize(MenuRowFont, Head, HW, TH);
			C->TextSize(MenuRowFont, Num, NW, TH);
			const float Fit = HW + NW > ColW ? ColW / (HW + NW) : 1.f;
			const float Left = ColX - (HW + NW) * Fit * 0.5f;
			auto Part = [&](const FString& Str, float X, const FLinearColor& Colour, const FLinearColor& Edge)
			{
				FCanvasTextItem T(FVector2D(X, Y(RowZ(r))), FText::FromString(Str), MenuRowFont, bMenuMaskPass ? FLinearColor::White : Colour);
				T.Scale = FVector2D(Fit, Fit);
				T.bCentreY = true;
				T.bOutlined = true;
				T.OutlineColor = bMenuMaskPass ? FLinearColor::White : Edge;
				T.BlendMode = SE_BLEND_Translucent;
				C->DrawItem(T);
			};
			Part(Head, Left, bHot ? Hot : Ink, bHot ? FLinearColor::Black : Outline);
			Part(Num, Left + HW * Fit, Risk, FLinearColor::Black);
			if (bInk) SheetRowInk.Add(FBox2D(FVector2D(Left, Y(RowZ(r)) - TH * Fit * 0.31f), FVector2D(Left + (HW + NW) * Fit, Y(RowZ(r)) + TH * Fit * 0.31f)));
			(void)Item;
			continue;
		}
		// (REQ-EXTRAS) a locked mode's row greyed, its requirement in its label
		const int32 Ri = MenuScroll + r - (MenuPage->Message.IsEmpty() ? 0 : 4);
		const bool bGrey = MenuPage->Items.IsValidIndex(Ri) && MenuPage->Items[Ri].Classes.Contains(TEXT("disabled"));
		const FBox2D RowBox = Text(MenuRowFont, RowText, ColX, Y(RowZ(r)), bGrey ? FLinearColor::FromSRGBColor(FColor(0x6a, 0x60, 0x56)) : bHot ? Hot : Ink, ColW);
		if (bInk && RowBox.bIsValid) SheetRowInk.Add(RowBox);
	}
	// (REQ-PROMPTCENTRE, user 15 Oct: the prompts sat to the left) "more above" centred: over the title on a titled page (the
	// title and the first row have no room between them), else where the title would be, over the first row (it was at the
	// right, on the title's line)
	if (MenuMoreUp && !MenuMoreUp->Text.IsEmpty())
	{
		const bool bTitled = !MenuPage->File.Equals(TEXT("main_game.rml"), ESearchCase::IgnoreCase) && !bTitleMakesWay;
		const FString Up = MenuMoreUp->Text.ToString();
		float UpY = Y(bTitled ? 17.5f + Lift + 7.f : (RowZ(0) + 17.5f + Lift) * 0.5f);
		// (REQ-BUTTONGLYPHS) in VR the words with the stick drawn under them: the two just over the title (or the first row)
		const FBox2D Over = bTitled ? SheetTitleInk : SheetRowInk.Num() > 0 ? SheetRowInk[0] : FBox2D(ForceInit);
		if (Over.bIsValid && GlyphLineWidth(C, Up, MenuSmallFont) >= 0.f) UpY = float(Over.Min.Y) - 10.f - OutlawsGlyphs::StackedBelow(C, MenuSmallFont, MenuGlyphPx(C));
		const FBox2D B = Text(MenuSmallFont, Up, ColX, UpY, Faint, ColW * 0.3f);
		if (bInk && B.bIsValid) { SheetEdgeInk.Add(B); SheetMoreUpInk = B; }
	}
	// the foot line: a submenu says what takes the player back (the remaster's help line), the pause page the level's time so far
	// (REQ-LEVELTIMER), and "more below" after it; (REQ-PROMPTCENTRE) the line centred as one under the rows, one gap between
	// its parts (the back line was at the left, "more below" at the right)
	{
		FString Lead;
		if (MenuStack.Num() > 0) Lead = bVR ? TEXT("B or Y: back") : TEXT("Esc: back");
		else if (MenuPage->File.Equals(TEXT("main_game.rml"), ESearchCase::IgnoreCase) && !PauseTimeShown.IsEmpty()) Lead = PauseTimeShown;
		const FString More = MenuMoreDown && !MenuMoreDown->Text.IsEmpty() ? MenuMoreDown->Text.ToString() : FString();
		const float MaxW = ColW * 0.3f, Gap = 6.f * S;
		// (REQ-BUTTONGLYPHS) a VR prompt is as wide as its words or its controllers under them, whichever is wider: measured by
		// its words alone, Back and "more below" overlapped
		auto Wide = [&](const FString& Str)
		{
			if (Str.IsEmpty() || !MenuSmallFont) return 0.f;
			const float GW = GlyphLineWidth(C, Str, MenuSmallFont);
			if (GW >= 0.f) return GW;
			float XL = 0.f, YL = 0.f;
			C->TextSize(MenuSmallFont, Str, XL, YL);
			return FMath::Min(XL, MaxW);
		};
		const float WL = Wide(Lead), WR = Wide(More);
		const float Total = WL + WR + (WL > 0.f && WR > 0.f ? Gap : 0.f);
		const float Left = ColX - Total * 0.5f;
		const float FootY = Y(PageFootZ());
		if (bInk) { SheetFootBelow = 16.f; SheetBackInk = FBox2D(ForceInit); }
		if (!Lead.IsEmpty())
		{
			// (REQ-BACKCLICK) Back is pointed at and pulled as a row is: orange when a laser is on it
			const bool bBack = MenuStack.Num() > 0;
			const FBox2D B = Text(MenuSmallFont, Lead, Left + WL * 0.5f, FootY, bBack && IsMenuBackHot() ? Hot : Faint, MaxW);
			if (bInk && B.bIsValid) { SheetFootInk.Add(B); SheetEdgeInk.Add(B); SheetFootBelow = FMath::Max(SheetFootBelow, float(B.Max.Y) - FootY); }
			if (bInk && bBack) SheetBackInk = B;
		}
		if (!More.IsEmpty())
		{
			const FBox2D B = Text(MenuSmallFont, More, Left + Total - WR * 0.5f, FootY, Faint, MaxW);
			if (bInk && B.bIsValid) { SheetFootInk.Add(B); SheetEdgeInk.Add(B); SheetFootBelow = FMath::Max(SheetFootBelow, float(B.Max.Y) - FootY); }
		}
	}
	if (MenuToast && !MenuToast->Text.IsEmpty())
	{
		TArray<FString> Lines;
		MenuToast->Text.ToString().ParseIntoArrayLines(Lines);
		for (int32 i = 0; i < Lines.Num(); ++i)
		{
			Text(MenuSmallFont, Lines[i], ColX, Y(PageFootZ() - 3.2f * i), Red, ColW * 0.95f);      // (REQ-MENUCENTRE: down from the line, clear of the rows)
		}
	}
}

void AOutlawsPlayer::MenuToastText(const FString& Text)
{
	if (MenuToast)
	{
		MenuToast->SetText(FText::FromString(Text));
	}
	MenuToastLeft = 2.5f;
	RedrawMenu();
}

#if WITH_OUTLAWS_TESTS
FString AOutlawsPlayer::CheatsPageTestLabels()
{
	const bool bWas = bMenuOpen;
	if (!bWas) OpenMenu(false);
	FString Out;
	if (bMenuOpen && ShowMenuPage(TEXT("#cheats"), false) && MenuPage)
	{
		for (const OutlawsMenu::FItem& It : MenuPage->Items) Out += (Out.IsEmpty() ? TEXT("") : TEXT("|")) + It.Label;
	}
	if (!bWas && bMenuOpen) CloseMenu();
	return Out;
}

bool AOutlawsPlayer::CheatsMenuTestPress(const FString& Name)
{
	if (!bMenuOpen) OpenMenu(false);
	if (!bMenuOpen || !ShowMenuPage(TEXT("#cheats"), false) || !MenuPage) return false;
	for (int32 i = 0; i < MenuPage->Items.Num(); ++i)
	{
		if (MenuPage->Items[i].Name != Name) continue;
		MenuActivate(0, i);
		return true;
	}
	return false;
}

bool AOutlawsPlayer::VrSpeedMenuTestPress()
{
	if (!bMenuOpen) OpenMenu(false);
	if (!bMenuOpen || !ShowMenuPage(TEXT("#vr"), false) || !MenuPage) return false;
	for (int32 i = 0; i < MenuPage->Items.Num(); ++i)
	{
		if (MenuPage->Items[i].Name != TEXT("vr_speed")) continue;
		MenuActivate(0, i);
		return true;
	}
	return false;
}
#endif // WITH_OUTLAWS_TESTS

bool AOutlawsPlayer::CheatIsLocalOnly(const FString& Name)
{
	// BUG-ALLOWCHEATS: the cheat rows the host's Allow Cheats setting does not govern, because they change nothing but what this
	// player's own menus show (nothing replicated, nothing in the host's lobby for the others).  To exempt a row, add its Name
	// (the row's id on the Cheats page, e.g. "cheat_god") to this set; the page keeps it for a client of a no-cheats
	// host and the host answers a client's ask for it with yes.
	// (UNLOCKFREE: its one row, REQ-UNLOCKS' Unlock all modes, went with the locks; the set stays for the next such row)
	static const TSet<FString> LocalOnly = TSet<FString>();
	return LocalOnly.Contains(Name);
}

void AOutlawsPlayer::RunCheat(const FString& Name, TFunction<void()> Do, bool bForceAsk, TFunction<void(bool)> Result)
{
	auto Finish = [&Result](bool bOk) { if (Result) Result(bOk); };
	// (single player, the host's own and a local-only row: as ever)
	if (CheatIsLocalOnly(Name) || !UOutlawsCoop::IsActive(this) )
	{
		Do();
		Finish(true);
		return;
	}
	if (!bForceAsk && UOutlawsCoop::CheatsOffHere(this))
	{
		MenuToastText(TEXT("The host has cheats off"));
		Finish(false);
		return;
	}
	TWeakObjectPtr<AOutlawsPlayer> Weak(this);
	const bool bSent = UOutlawsCoop::AskHostCheat(this, Name, [Weak, Do, Result](bool bOk)
	{
		if (bOk) Do();
		else if (AOutlawsPlayer* Me = Weak.Get()) Me->MenuToastText(TEXT("The host has cheats off"));
		if (Result) Result(bOk);
	});
	if (!bSent)
	{
		Do();                    // (the host: its own cheat; no one to ask)
		Finish(true);
	}
}

// ---- REQ-TRIGGERSTEP: the menu input's one way into a row ---------------------------------------------------------------------

bool AOutlawsPlayer::IsMenuValueRow(int32 Index) const
{
	using namespace OutlawsMenu;
	if (!MenuPage || !MenuPage->Items.IsValidIndex(Index)) return false;
	const FItem& I = MenuPage->Items[Index];
	if (I.Kind == EItemKind::Select || I.Kind == EItemKind::Range || I.Kind == EItemKind::Check) return true;
	return I.Name == TEXT("coop:badguys") || I.Name == TEXT("coop:maxplayers") || I.Name == TEXT("coop:diff");     // (the lobby's own)
}

void AOutlawsPlayer::MenuPress(int32 Hand, int32 Index, int32 Dir, bool bStopAtEnds)
{
	OutlawsMenu::FStepPress& S = OutlawsMenu::StepPress();
	const OutlawsMenu::FStepPress Was = S;
	S.Dir = Dir < 0 ? -1 : 1;
	S.bStopAtEnds = bStopAtEnds;
	S.bHitEnd = false;
	const FString Name = MenuPage && MenuPage->Items.IsValidIndex(Index) ? MenuPage->Items[Index].Name : FString();
	MenuActivate(Hand, Index);
	bMenuStepHitEnd = S.bHitEnd;
	S = Was;                     // (every other press, from code: one step up, round again)
	if (bMenuStepHitEnd)
	{
		// the row was already at that end: it stays, and the hand that pressed gets a short firmer tick
		Pulse(Hand, 0.9f);
		++MenuStepTicks[FMath::Clamp(Hand, 0, 1)];
		UE_LOG(LogOutlaws, Log, TEXT("OLRT menu step: %s is at its %s, a tick on the %s hand"), *Name, Dir < 0 ? TEXT("lowest") : TEXT("highest"), Hand == 0 ? TEXT("left") : TEXT("right"));
	}
}

void AOutlawsPlayer::MenuTriggerPress(int32 Hand, int32 Index)
{
	// two controllers: the right trigger up, the left down, a number stops at its ends; one controller: up and round again
	const bool bOne = OneHand() >= 0;
	MenuPress(Hand, Index, bOne || Hand == 1 ? 1 : -1, !bOne);
}

void AOutlawsPlayer::MenuMousePress(bool bRight)
{
	if (!bMenuOpen || bVR || !MenuPage || !MenuRoot || !Camera) return;
	bDesktopMenuMouse = true;
	if (MenuPage->File == TEXT("#unlocked"))
	{
		UnlockPageContinue();                      // (REQ-UNLOCKS: any button carries on)
		return;
	}
	float Length = 0.f;
	const int32 Row = MenuRowUnderRay(Camera->GetComponentLocation(), Camera->GetForwardVector(), Length);
	if (Row < 0) return;
	const int32 First = MenuPage->Message.IsEmpty() ? 0 : 4;
	MenuMouseStep(MenuScroll + Row - First, bRight);
}

int32 AOutlawsPlayer::MenuRowUnderRay(const FVector& O, const FVector& D, float& Length)
{
	if (!MenuRoot || !MenuPage) return -1;
	const FTransform Panel = MenuRoot->GetComponentTransform();
	const FVector N = Panel.GetUnitAxis(EAxis::X);
	const int32 First = MenuPage->Message.IsEmpty() ? 0 : 4;
	int32 Row = -1;
	const double Den = FVector::DotProduct(D, N);
	if (Den >= -1e-3) return -1;
	const double T = FVector::DotProduct(Panel.GetLocation() - O, N) / Den;
	if (T <= 0.0 || T >= 600.0) return -1;
	const FVector Local = Panel.InverseTransformPosition(O + D * T);
	if (bWideSheet && FMath::Abs(Local.Y) < SheetW * 0.5 && FMath::Abs(Local.Z) < SheetH * 0.5)
	{
		// the save screen: which capsule (748 x 100 in the 1920 x 1080 layout, the list at 838, 160)
		Length = float(T);
		const double Px = (0.5 - Local.Y / SheetW) * 1920.0, Py = (SheetH * 0.5 - Local.Z) / SheetH * 1080.0;
		if (IsFrontPage())
		{
			Row = FrontHitAt(FVector2D(Px, Py));
		}
		else if (IsAchievPage())
		{
			Row = AchievRowAt(Px, Py);                // (REQ-EXTRAS: which capsule)
		}
		else
		{
			const int32 r = Py >= 160.0 && Py < 925.0 && Px >= 838.0 && Px < 838.0 + 748.0 ? int32((Py - 160.0) / 100.0) : -1;
			if (r >= 0 && MenuPage->Items.IsValidIndex(MenuScroll + r)) Row = r;
		}
	}
	else if (!bWideSheet && FMath::Abs(Local.Y) < PanelW * 0.5 && FMath::Abs(Local.Z) < PageSheetH() * 0.5)      // (REQ-SUBMENUSPACE: the sheet's own height)
	{
		Length = float(T);
		const int32 r = RowAt(float(Local.Z));
		const int32 i = MenuScroll + r - First;
		if (r >= First && r < SheetRowsFit() && MenuPage->Items.IsValidIndex(i) && MenuPage->Items[i].Kind != OutlawsMenu::EItemKind::Heading
			&& !MenuPage->Items[i].Classes.Contains(TEXT("disabled")))         // (REQ-EXTRAS: a greyed row is never pointed at)
		{
			Row = r;
		}
	}
	return Row;
}

void AOutlawsPlayer::MenuActivate(int32 Hand, int32 Index)
{
	using namespace OutlawsMenu;
	if (!MenuPage || !MenuPage->Items.IsValidIndex(Index))
	{
		return;
	}
	const FItem I = MenuPage->Items[Index];
	if (I.Classes.Contains(TEXT("disabled")))
	{
		// (a greyed-out row, the title's Load Game with no save: nothing happens, whichever way it was reached)
		UE_LOG(LogOutlaws, Log, TEXT("OLRT menu: %s is greyed out, not pressed"), *I.Label);
		return;
	}
	const FString Key = MenuPage->File + TEXT(":") + FString::FromInt(Index);
	Pulse(Hand, 0.35f);
	if (ExtrasPress(I, Index)) return;                   // (REQ-EXTRAS: the Extras pages' own rows)
	if (I.Name.StartsWith(TEXT("cheat_")) && !bCheatReplay)
	{
		// BUG-ALLOWCHEATS: a client of a match asks the host first (its answer runs the row again, bCheatReplay); single
		// player, the host and a local-only row (CheatIsLocalOnly) run it at once
		TWeakObjectPtr<AOutlawsPlayer> Weak(this);
		const FString Name = I.Name;
		RunCheat(Name, [Weak, Hand, Index, Name]()
		{
			AOutlawsPlayer* Me = Weak.Get();
			if (!Me || !Me->MenuPage || !Me->MenuPage->Items.IsValidIndex(Index) || Me->MenuPage->Items[Index].Name != Name) return;
			Me->bCheatReplay = true;
			Me->MenuActivate(Hand, Index);
			Me->bCheatReplay = false;
		});
		return;
	}
	if (I.Name.StartsWith(TEXT("cheat_")))
	{
		// (REQ-LEVELTIMER: a cheat that runs here keeps this level's best time from being saved)
		AOutlawsLevelActor* LC = Level.Get();
		UOutlawsStats::NoteCheat(LC, I.Name, LC && LC->bInvincible, bUnlimitedAmmo, !LC || LC->bEnemiesActive);
	}
	if (I.Name.StartsWith(TEXT("mphost")))
	{
		MpHostPress(I.Name);                             // (M1: the Host Game page's rows, OutlawsPlayerMpHost.cpp)
		return;
	}
	switch (I.Kind)
	{
	case EItemKind::Link:
	case EItemKind::Dialog:
		ShowMenuPage(I.Target, true);
		return;
	case EItemKind::Action:
	case EItemKind::Button:
		if (I.Name.StartsWith(TEXT("coop:")))
		{
			UOutlawsCoop::MenuAction(this, I.Name);          // (the co-op pages' rows)
			return;
		}
		if (I.Name == TEXT("devchecks_next"))
		{
			OutlawsDevChecks::NextPage();                     // (REQ-DEVCHECKS: the page row; Back still leads to the pause page)
			ShowMenuPage(TEXT("#devchecks"), false);
			return;
		}
		if (I.Name == TEXT("devchecks_none")) return;
		if (I.Name == TEXT("unlock_ok"))
		{
			UnlockPageContinue();                            // (REQ-UNLOCKS: the unlock page's Continue)
			return;
		}
		// (REQ-NOUNSTUCK: no "unstuck" row any more; RequestUnstuck is only the automatic rescue's, OutlawsPlayerUnstuck.cpp)
		if (I.Name == TEXT("mp_name"))
		{
			const AOutlawsLevelActor* LN = Level.Get();
			OpenKeyboard(TEXT("mp_name"), TEXT("PLAYER NAME"), LN ? LN->MpPlayerName : FString(), AOutlawsLevelActor::MpNameMax);
			return;
		}
		if (I.Name == TEXT("rando_seed"))
		{
			const AOutlawsLevelActor* LS = Level.Get();
			OpenKeyboard(TEXT("rando_seed"), TEXT("RANDOMIZER SEED"), LS ? LS->RandoSeedText : FString(), AOutlawsLevelActor::RandoSeedMax);
			return;
		}
		if (I.Name == TEXT("rando_random"))
		{
			if (AOutlawsLevelActor* LS = Level.Get()) LS->RandoSeedText.Reset();
			SaveSettings();
			ShowMenuPage(TEXT("#rando"), false);
			MenuToastText(TEXT("A new seed at each New Game"));
			return;
		}
		if (I.Name.StartsWith(TEXT("kbd:")))
		{
			KeyboardKey(I.Name.Mid(4));                      // (the letters keyboard's keys)
			return;
		}
		if (I.Name.StartsWith(TEXT("level:")))
		{
			const FString Name = I.Name.Mid(6);
			if (UOutlawsCoop::IsActive(this) && UOutlawsCoop::MenuLevelChange(this, EOutlawsCoopStart::Jump, Name))
			{
				return;                  // (co-op: the host's skip takes both players there; the client's is refused)
			}
			CloseMenu();
			if (AOutlawsLevelActor* L = Level.Get()) L->JumpToLevel(Name, true);       // (BUG-SKIPLEVELINV: with the pockets)
			return;
		}
		if (I.Name.StartsWith(TEXT("char:")))
		{
			// (M1) the character page: saved and handed to the match; worn from the next spawn
			const FOutlawsMpCharacter* Ch = FOutlawsMpCharacter::Find(I.Name.Mid(5));
			if (Ch)
			{
				FOutlawsMpCharacter::SetLocalChoice(Ch->Short);
				MenuToastText(FOutlawsMpCharacter::bMatchMenu ? FString::Printf(TEXT("%s from your next spawn"), Ch->Name) : FString::Printf(TEXT("Character: %s"), Ch->Name));
				RedrawMenu();
			}
			return;
		}
		if (I.Name == TEXT("tutorial_start") || I.Name == TEXT("tutorial_skip"))
		{
			// REQ-TUTORIAL (OutlawsTutorial.cpp): New Game > Tutorial; the pause page's Skip tutorial
			AOutlawsLevelActor* L = Level.Get();
			CloseMenu();
			if (L && I.Name == TEXT("tutorial_start"))
			{
				ClearScreenFx();
				L->StartTutorial();
			}
			else if (L)
			{
				L->EndTutorial(true);
			}
			return;
		}
		if (I.Name.StartsWith(TEXT("start:")))
		{
			// "start:<story | level>:<difficulty>"
			TArray<FString> Parts;
			I.Name.ParseIntoArray(Parts, TEXT(":"));
			AOutlawsLevelActor* L = Level.Get();
			if (L && Parts.Num() >= 3)
			{
				const EOutlawsDifficulty D = static_cast<EOutlawsDifficulty>(FMath::Clamp(FCString::Atoi(*Parts[2]), 0, 2));
				CloseMenu();
				ClearScreenFx();          // the game starts clean, not fading out of the menu's tone (user 2026-09-27)
				if (Parts[1] == TEXT("story")) L->StartStory(D);
				else if (Parts[1] == TEXT("hom")) L->StartHom(D);
				else L->StartMission(Parts[1], D);
			}
			return;
		}
		if (I.Action == TEXT("ev_exit_game_confirm") && UOutlawsCoop::IsActive(this))
		{
			// End Game in co-op leaves the session (nothing saved); both machines go back to their own title menu
			CloseMenu();
			UOutlawsCoop::LeaveSession(this, TEXT("a player ended the game"));
			return;
		}
		if (I.Action == TEXT("ev_exit_game_confirm"))
		{
			// End Game: the autosave first (user 2026-10-01), then the main menu
			// (between two levels, in the fade or the cutscene, there is no level to save: the next level's opening instead,
			// user 2026-10-02)
			if (AOutlawsLevelActor* LS = Level.Get(); LS && !LS->IsFrontEnd() && !LS->IsPlayerDead())
			{
				if (!LS->SaveGame(-1)) LS->SaveLevelOpening();
			}
			CloseMenu();
			if (AOutlawsLevelActor* L = Level.Get()) L->ReturnToFrontEnd();
			return;
		}
		if (I.Name.StartsWith(TEXT("save:")) || I.Name.StartsWith(TEXT("load:")))
		{
			const bool bSave = I.Name.StartsWith(TEXT("save:"));
			const int32 Slot = FCString::Atoi(*I.Name.Mid(5));
			const bool bEmpty = !SlotsShown.IsValidIndex(Index) || SlotsShown[Index].bEmpty;
			PendingSaveSlot = Slot;
			if (bSave && bEmpty)
			{
				AOutlawsLevelActor* L = Level.Get();
				const bool bOk = L && L->SaveGame(Slot);
				ShowMenuPage(MenuPage->File, false);
				MenuToastText(bOk ? (MenuSource ? MenuSource->Loc(TEXT("$m_game_saved")) : FString(TEXT("Game saved!"))) : FString(TEXT("Could not save here")));
			}
			else if (bSave)
			{
				ShowMenuPage(TEXT("dialog_save_overwrite.rml"), true);       // "Are you sure you want to overwrite this saved game?"
			}
			else if (!bEmpty && bFrontMenu)
			{
				// (REQ-TITLEMENU: from the title's Load Game there is no game going on to lose: the save loads at once, as
				// Resume Game did)
				CloseMenu();
				if (AOutlawsLevelActor* L = Level.Get(); L && !L->LoadGame(Slot) && L->IsFrontEnd())
				{
					L->ShowFrontMenu();
					MenuToastText(TEXT("That save could not be loaded"));
				}
			}
			else if (!bEmpty)
			{
				ShowMenuPage(TEXT("dialog_load_game.rml"), true);            // "... Your current progress will be lost."
			}
			return;
		}
		if (I.Action == TEXT("ev_save_game_confirm"))
		{
			AOutlawsLevelActor* L = Level.Get();
			const bool bOk = L && L->SaveGame(PendingSaveSlot);
			MenuBack();
			MenuToastText(bOk ? (MenuSource ? MenuSource->Loc(TEXT("$m_game_saved")) : FString(TEXT("Game saved!"))) : FString(TEXT("Could not save here")));
			return;
		}
		if (I.Action == TEXT("ev_resume_last_save"))
		{
			if (!ResumeGameOffered(this))
			{
				// (BUG-MPRESUME: a co-op or match session never loads the single-player autosave; the row just closes the menu)
				UE_LOG(LogOutlaws, Log, TEXT("OLRT menu: Resume Game refused in a co-op or match session"));
				if (bFrontMenu) MenuBack(); else CloseMenu();
				return;
			}
			// the last autosave, loaded as any save is (through black); should it not load, the main menu again
			CloseMenu();
			if (AOutlawsLevelActor* L = Level.Get(); L && !L->LoadGame(-1))
			{
				L->ShowFrontMenu();
				MenuToastText(TEXT("The last save could not be loaded"));
			}
			return;
		}
		if (I.Action == TEXT("ev_load_game_confirm"))
		{
			CloseMenu();
			if (AOutlawsLevelActor* L = Level.Get(); L && !L->LoadGame(PendingSaveSlot) && L->IsFrontEnd())
			{
				L->ShowFrontMenu();            // (from the main menu, a save that will not load: the main menu again)
				MenuToastText(TEXT("That save could not be loaded"));
			}
			return;
		}
		if (I.Name.StartsWith(TEXT("defaults:")))
		{
			AOutlawsLevelActor* L = Level.Get();
			if (I.Name == TEXT("defaults:gameplay") && L)
			{
				L->bAutosave = true;
				L->AutosaveMinutes = 2.f;
				L->SoloBadGuys = 1.f;
			}
			if (I.Name == TEXT("defaults:vr"))
			{
				MoveDirection = EOutlawsMoveDirection::Head;
				bSnapTurn = true;
				const bool bWasTeleport = bTeleportMove;
				bTeleportMove = false;
				if (L) L->bTeleportPace = false;
				if (bWasTeleport) bMenuRebuild = true;         // (next frame: the page is in use here)
				DominantHand = EOutlawsHand::Right;
				bFaceReload = true;
				OneController = 0;
				if (L && L->GameSpeed > SpeedWarnOver) bMenuRebuild = true;      // (BUG-SPEEDWARN: the warning row goes)
				if (L) L->GameSpeed = 0.5f;             // (shown as 100 %, the default)
			}
			else if (I.Name == TEXT("defaults:sound") && L)
			{
				L->MasterVolume = 1.f;
				L->SoundVolume = 1.f;
				L->MusicVolume = 0.5f;
				L->MovieVolume = 1.f;
				L->bHDSounds = true;
				L->ApplyMusicVolume();
			}
			else if (I.Name == TEXT("defaults:display") && L)
			{
				// HD textures back to the headset's default (on; off under the Quest 2 profile), and no longer the player's choice
				L->bHDChosen = false;
				if (OutlawsDeviceProfile::DefaultHDTextures()) L->bHDTexturesInFile = true;
				if (L->bHDTextures != OutlawsDeviceProfile::DefaultHDTextures()) L->SetHDTextures(OutlawsDeviceProfile::DefaultHDTextures());
				L->bPaletteGrades = true;
				L->SetStochastic(true);
				OutlawsDeviceProfile::RestoreDefaults();
			}
			RefreshMenuRows();
			SaveSettings();
			MenuToastText(TEXT("Defaults restored"));
			return;
		}
		if (I.Name == TEXT("cheat_boss"))
		{
			if (AOutlawsLevelActor* L = Level.Get()) L->DebugSummonBoss();
			MenuToastText(TEXT("The boss is coming"));
			return;
		}
		if (I.Name == TEXT("hom_resume"))
		{
			CloseMenu();
			ClearScreenFx();
			if (AOutlawsLevelActor* L = Level.Get()) L->HomResume();
			return;
		}
		if (I.Name == TEXT("hom_ride") || I.Name == TEXT("hom_stay"))
		{
			CloseMenu();
			if (I.Name == TEXT("hom_ride")) if (AOutlawsLevelActor* L = Level.Get()) { ClearScreenFx(); L->HomRide(); }
			return;
		}
		if (I.Name == TEXT("cheat_jackpot"))
		{
			const int32 N = Level.IsValid() ? Level->CheatJackpot() : 0;
			MenuToastText(N > 0 ? FString::Printf(TEXT("Jackpot: %d key item%s"), N, N == 1 ? TEXT("") : TEXT("s")) : FString(TEXT("Jackpot: nothing more is needed here")));
			return;
		}
		if (I.Name == TEXT("cheat_health"))
		{
			if (AOutlawsLevelActor* L = Level.Get()) L->CheatFullHealth();
			MenuToastText(TEXT("Full health"));
			return;
		}
		if (I.Name == TEXT("cheat_oil"))
		{
			if (AOutlawsLevelActor* L = Level.Get()) L->AddCount(TEXT("ioil"), 100);
			MenuToastText(TEXT("More oil"));
			return;
		}
		if (I.Name == TEXT("cheat_weapons"))
		{
			if (AOutlawsLevelActor* L = Level.Get()) L->GiveAllWeapons();
			MenuToastText(TEXT("All weapons and ammo given"));
			return;
		}
		if (I.Action == TEXT("ev_close_menu"))
		{
			if (bFrontMenu) MenuBack(); else CloseMenu();      // (the main menu stays until a game starts)
		}
		else if (I.Action == TEXT("ev_restart_level") && UOutlawsCoop::IsActive(this) && UOutlawsCoop::MenuLevelChange(this, EOutlawsCoopStart::Restart, FString(), true))
		{
			// (co-op: only the host restarts the level, for both)
		}
		else if (I.Action == TEXT("ev_restart_level") || I.Action == TEXT("ev_end_game") || I.Action == TEXT("ev_quit"))
		{
			// the remaster opens these confirmations from code (the literals sit beside the events)
			ShowMenuPage(I.Action == TEXT("ev_restart_level") ? TEXT("dialog_restart_level.rml")
				: I.Action == TEXT("ev_end_game") ? TEXT("dialog_exit_game.rml") : TEXT("dialog_quit.rml"), true);
		}
		else if (I.Action == TEXT("ev_restart_level_confirm") && UOutlawsCoop::IsActive(this) && UOutlawsCoop::MenuLevelChange(this, EOutlawsCoopStart::Restart, FString()))
		{
			// (co-op: the host's Restart Level restarts both)
		}
		else if (I.Action == TEXT("ev_restart_level_confirm"))
		{
			CloseMenu();
			if (AOutlawsLevelActor* L = Level.Get()) L->RestartLevel();
		}
		else if (I.Action == TEXT("ev_quit_confirm"))
		{
			SaveSettings();
			FGenericPlatformMisc::RequestExit(false);
		}
		else if (I.bGoBack || I.Action == TEXT("ev_dialog_close"))
		{
			MenuBack();
		}
		else
		{
			MenuToastText(FString::Printf(TEXT("%s: not in the VR build yet"), *I.Label));
		}
		return;
	case EItemKind::Select:
		if (I.Name.StartsWith(TEXT("devcheck:")))
		{
			// REQ-DEVCHECKS: Not checked > Fixed > Still broken, saved at once; the check's longer note shows a while
			const FString Id = I.Name.Mid(9);
			const int32 S = OutlawsDevChecks::Cycle(Id);
			RefreshMenuRows();
			FString Detail;
			for (const OutlawsDevChecks::FCheck& C : OutlawsDevChecks::Checks()) if (C.Id == Id) Detail = C.Detail.Replace(TEXT("\n"), TEXT(" "));   // (one line: the toast sits under the rows)
			MenuToastText(Detail.IsEmpty() ? FString::Printf(TEXT("%s: %s"), *Id, OutlawsDevChecks::StateText(S)) : Detail);
			MenuToastLeft = Detail.IsEmpty() ? 2.5f : 8.f;
			return;
		}
		if (I.Name == TEXT("vr_movefollow"))
		{
			static const EOutlawsMoveDirection Ways[] = { EOutlawsMoveDirection::Head, EOutlawsMoveDirection::DominantHand, EOutlawsMoveDirection::OffHand };
			int32 k = 0;
			while (k < UE_ARRAY_COUNT(Ways) && Ways[k] != MoveDirection) ++k;
			MoveDirection = Ways[StepIndex(k, UE_ARRAY_COUNT(Ways), false)];
			RefreshMenuRows();
			SaveSettings();
			return;
		}
		if (I.Name == TEXT("vr_turn"))
		{
			bSnapTurn = !bSnapTurn;
			RefreshMenuRows();
			SaveSettings();
			return;
		}
		if (I.Name.StartsWith(TEXT("rando_")))
		{
			// (R10) the Randomizer page's rows, saved at once; a game going on keeps its own until the next New Game
			if (AOutlawsLevelActor* L = Level.Get())
			{
				const bool bOnOff = I.Name == TEXT("rando_bosses") || I.Name == TEXT("rando_critters");
				int32& V = I.Name == TEXT("rando_enemies") ? L->RandoEnemies : I.Name == TEXT("rando_items") ? L->RandoItems : I.Name == TEXT("rando_bosses") ? L->RandoBosses
					: I.Name == TEXT("rando_critters") ? L->RandoCritters : L->RandoOrder;
				V = StepIndex(V, bOnOff ? 2 : 3, false);
				UE_LOG(LogOutlaws, Log, TEXT("OLRANDO page: enemies %d, items %d, mission order %d, bosses %d, critters %d, seed '%s'"), L->RandoEnemies, L->RandoItems, L->RandoOrder,
					L->RandoBosses, L->RandoCritters, *L->RandoSeedText);
			}
			RefreshMenuRows();
			SaveSettings();
			return;
		}
		if (I.Name == TEXT("mp_pvol"))
		{
			// (REQ-VOICEVOL) 50% .. 300% in halves, round again; the voice chat reads it live
			if (AOutlawsLevelActor* L = Level.Get())
			{
				L->MpPartnerVolume = (StepIndex(FMath::Clamp(L->MpPartnerVolume / 50 - 1, 0, 5), 6, true) + 1) * 50;     // (REQ-TRIGGERSTEP: a number row)
				UE_LOG(LogOutlaws, Log, TEXT("OLRT multiplayer: partner volume %d%%"), L->MpPartnerVolume);
			}
			RefreshMenuRows();
			SaveSettings();
			return;
		}
		if (I.Name == TEXT("mp_ptt") || I.Name == TEXT("mp_sens"))
		{
			// (R8) the Multiplayer page's voice rows, live and saved
			if (AOutlawsLevelActor* L = Level.Get())
			{
				if (I.Name == TEXT("mp_ptt")) L->MpPushToTalk = StepIndex(L->MpPushToTalk, 4, false);
				else
				{
					L->MpMicSensitivity = StepIndex(L->MpMicSensitivity, 3, false);
					UOutlawsCoop::ApplyMicSensitivity(L->MpMicSensitivity);
				}
				UE_LOG(LogOutlaws, Log, TEXT("OLRT multiplayer: push to talk %d, microphone sensitivity %d"), L->MpPushToTalk, L->MpMicSensitivity);
			}
			RefreshMenuRows();
			SaveSettings();
			return;
		}
		if (I.Name == TEXT("vr_locomotion"))
		{
			bTeleportMove = !bTeleportMove;
			ShowTeleportArc(false);
			bTeleportAim = false;
			if (AOutlawsLevelActor* LT = Level.Get()) LT->bTeleportPace = bTeleportMove;
			SaveSettings();
			bMenuRebuild = true;                    // the page again next frame: the smooth walk's rows come or go
			return;
		}
		if (I.Name == TEXT("difficulty"))
		{
			if (AOutlawsLevelActor* L = Level.Get())
			{
				L->ChangeDifficulty(static_cast<EOutlawsDifficulty>(StepIndex(int32(L->Difficulty), 3, false)));
				MenuToastText(TEXT("Enemies here change now. Who appears changes from the next level."));
			}
			RefreshMenuRows();
			return;
		}
		if (I.Name == TEXT("solo_badguys"))
		{
			// (REQ-SPMULT) x1 to x4 in quarters, round again; made from the next level built (a co-op session keeps the host's)
			if (AOutlawsLevelActor* L = Level.Get())
			{
				L->SoloBadGuys = StepNumber(L->SoloBadGuys, 1.f, 4.f, 0.25f);
				UE_LOG(LogOutlaws, Log, TEXT("OLRT single player bad guys: x%.2f"), L->SoloBadGuys);
				if (!L->IsFrontEnd()) MenuToastText(TEXT("The bad guys change from the next level."));
			}
			RefreshMenuRows();
			SaveSettings();
			return;
		}
		if (I.Name == TEXT("autosave_minutes"))
		{
			static const float Minutes[] = { 1.f, 2.f, 3.f, 5.f, 10.f, 15.f };
			if (AOutlawsLevelActor* L = Level.Get())
			{
				int32 k = 0;
				while (k < UE_ARRAY_COUNT(Minutes) && L->AutosaveMinutes > Minutes[k] + 0.01f) ++k;
				L->AutosaveMinutes = Minutes[StepIndex(FMath::Min(k, int32(UE_ARRAY_COUNT(Minutes)) - 1), UE_ARRAY_COUNT(Minutes), true)];
			}
			RefreshMenuRows();
			SaveSettings();
			return;
		}
		if (I.Name == TEXT("vr_onectrl"))
		{
			// (REQ-ONECTRL) Off, Left only, Right only: applied at once
			OneController = StepIndex(OneController, 3, false);
			RefreshMenuRows();
			SaveSettings();
			return;
		}
		if (I.Name == TEXT("vr_domhand"))
		{
			DominantHand = LeftDominant() ? EOutlawsHand::Right : EOutlawsHand::Left;
			RefreshMenuRows();
			SaveSettings();
			return;
		}
		if (OutlawsDeviceProfile::IsRow(I.Name))
		{
			// the Graphics page's performance rows: the next value, applied at once, with a line on what it does
			const FString Hint = OutlawsDeviceProfile::PressRow(I.Name);
			RefreshMenuRows();
			SaveSettings();
			MenuToastText(Hint);
			return;
		}
		if (I.Options.Num() > 1)
		{
			MenuValues.Add(Key, StepIndex(MenuValues.FindRef(Key), I.Options.Num(), false));
		}
		break;
	case EItemKind::Check:
		if (I.Name == TEXT("mp_announce") || I.Name == TEXT("mp_listen"))
		{
			UOutlawsBeacon::Toggle(I.Name);
			RefreshMenuRows();
			return;
		}
		if (I.Name == TEXT("mp_upnp"))
		{
			UOutlawsCoop::ToggleUpnp();
			RefreshMenuRows();
			return;
		}
		if (I.Name == TEXT("mp_vfade"))
		{
			// (REQ-VOICEVOL) the voice chat reads it live
			if (AOutlawsLevelActor* L = Level.Get())
			{
				L->bMpVoiceFade = !L->bMpVoiceFade;
				UE_LOG(LogOutlaws, Log, TEXT("OLRT multiplayer: voice fades with distance %s"), L->bMpVoiceFade ? TEXT("on") : TEXT("off"));
			}
			RefreshMenuRows();
			SaveSettings();
			return;
		}
		if (I.Name == TEXT("mp_mic"))
		{
			// (user 2026-10-02) the voice chat reads it live
			if (AOutlawsLevelActor* L = Level.Get())
			{
				L->bMpMicrophone = !L->bMpMicrophone;
				UOutlawsCoop::NoteMicSetting(this, L->bMpMicrophone);
				UE_LOG(LogOutlaws, Log, TEXT("OLRT multiplayer: microphone %s"), L->bMpMicrophone ? TEXT("on") : TEXT("off"));
			}
			RefreshMenuRows();
			SaveSettings();
			return;
		}
		if (I.Name == TEXT("autosave"))
		{
			if (AOutlawsLevelActor* L = Level.Get()) L->bAutosave = !L->bAutosave;
			RefreshMenuRows();
			SaveSettings();
			return;
		}
		if (I.Name == TEXT("vr_facereload"))
		{
			bFaceReload = !bFaceReload;
			RefreshMenuRows();
			SaveSettings();
			return;
		}
		if (I.Name == TEXT("r_perfhud") || I.Name == TEXT("r_grabglow") || I.Name == TEXT("r_quest2mode"))
		{
			MenuToastText(OutlawsDeviceProfile::PressRow(I.Name));
			RefreshMenuRows();
			SaveSettings();
			return;
		}
		if (I.Name == TEXT("r_remasteredmode") || I.Name == TEXT("snd_remastered") || I.Name == TEXT("r_tinting") || I.Name == TEXT("r_stochastic"))
		{
			// working switches: the level's textures (live), sounds, palette grades and stochastic texturing
			if (AOutlawsLevelActor* L = Level.Get())
			{
				if (I.Name == TEXT("r_remasteredmode"))
				{
					L->bHDChosen = true;           // (from now on the player's own: the Quest 2 profile's default no longer applies)
					L->SetHDTextures(!L->bHDTextures);
				}
				else if (I.Name == TEXT("r_tinting")) L->bPaletteGrades = !L->bPaletteGrades;
				else if (I.Name == TEXT("r_stochastic")) L->SetStochastic(!L->bStochastic);
				else L->SetHDSounds(!L->bHDSounds);
			}
			RefreshMenuRows();
			SaveSettings();
			return;
		}
		if (I.Name.StartsWith(TEXT("cheat_")))
		{
			AOutlawsLevelActor* L = Level.Get();
			if (I.Name == TEXT("cheat_god") && L) L->bInvincible = !L->bInvincible;
			else if (I.Name == TEXT("cheat_ammo")) bUnlimitedAmmo = !bUnlimitedAmmo;
			else if (I.Name == TEXT("cheat_freeze") && L) L->bEnemiesActive = !L->bEnemiesActive;
			RefreshMenuRows();
			MenuToastText(TEXT("Cheat changed"));
			return;
		}
		MenuValues.Add(Key, MenuValues.FindRef(Key) ? 0 : 1);
		break;
	case EItemKind::Radio:
		for (int32 k = 0; k < MenuPage->Items.Num(); ++k)
		{
			if (MenuPage->Items[k].Kind == EItemKind::Radio) MenuValues.Add(MenuPage->File + TEXT(":") + FString::FromInt(k), k == Index ? 1 : 0);
		}
		break;
	case EItemKind::Range:
	{
		if (I.Name == TEXT("r_gamma"))
		{
			// REQ-GAMMA: a step brighter (after 1.6 back to 0.7), shown at once behind the menu (UpdateScreenFx)
			MenuToastText(OutlawsDeviceProfile::PressGamma());
			RefreshMenuRows();
			SaveSettings();
			return;
		}
		if (float* Live = const_cast<float*>(LiveSetting(I.Name)))
		{
			// the speed: 0.2 up to 1 in steps of 0.05, shown as 40 % up to 200 % in tens (user 2026-10-02); volumes 0 to 100 % in tens;
			// then round again
			const bool bSpeed = I.Name == TEXT("vr_speed");
			const float Lo = bSpeed ? AOutlawsLevelActor::MinGameSpeed : 0.f, Step = bSpeed ? 0.05f : 0.1f;
			const float Hi = bSpeed ? AOutlawsLevelActor::MaxGameSpeed : 1.f;          // (BUG-SPEEDCAP: the speed tops out at 150 %)
			const bool bWarnWas = *Live > SpeedWarnOver;
			*Live = StepNumber(*Live, Lo, Hi, Step);   // on the step grid (an old 47.5 % goes to 55); REQ-TRIGGERSTEP: a number row
			// (BUG-SPEEDWARN) over the old 30 %, where sickness comes on, the warning row under this one (it was a toast over the
			// prompts): the page again next frame as it comes or goes
			if (bSpeed && bWarnWas != (*Live > SpeedWarnOver)) bMenuRebuild = true;
			if (AOutlawsLevelActor* L = Level.Get()) L->ApplyMusicVolume();
			RefreshMenuRows();
			SaveSettings();
			return;
		}
		const int32 Now = MenuValues.Contains(Key) ? MenuValues[Key] : (I.Name.Contains(TEXT("volume")) ? 10 : 5);
		MenuValues.Add(Key, StepIndex(FMath::Clamp(Now, 0, 10), 11, true));
		break;
	}
	case EItemKind::Bind:
		MenuToastText(TEXT("The VR controls are fixed for now"));
		return;
	default:
		return;
	}
	RefreshMenuRows();
	MenuToastText(TEXT("Shown only: settings are not applied yet"));
}

void AOutlawsPlayer::UpdateMenu(float Dt)
{
	OL_PERF_SCOPE("Player: menu");
	if (!bMenuOpen || !MenuRoot || !MenuPage)
	{
		return;
	}
	if (bMenuRebuild)
	{
		bMenuRebuild = false;
		ShowMenuPage(MenuPage->File, false);
	}
	// REQ-LEVELTIMER: the pause page's time line, drawn again as the second changes (a session's clock runs on behind the menu;
	// single player's stands still)
	{
		const bool bPausePage = MenuPage->File.Equals(TEXT("main_game.rml"), ESearchCase::IgnoreCase) && !bFrontMenu;
		const FString Line = bPausePage ? UOutlawsStats::PauseLine(this) : FString();
		if (Line != PauseTimeShown)
		{
			PauseTimeShown = Line;
			RedrawMenu();
		}
	}
	// the panel follows the head's turn lazily: once the head has turned well away (25 degrees across, 20 up or down; user
	// 2026-09-28) it sets off, eases along on a damped spring all the way back to the middle and settles there (user
	// 2026-10-01: it stopped dead at the edge and set off again in jerks, rigid)
	const FVector Eye = Camera->GetComponentLocation();
	const float HeadYaw = Camera->GetComponentRotation().Yaw;
	const float HeadPitch = bFrontMenu ? 0.f : FMath::Clamp(float(Camera->GetComponentRotation().Pitch), -70.f, 70.f);
	// a whole turn on the spot (either way; turning back undoes it) shows the Cheats row on the pause page (user 2026-10-01)
	if (!bCheatsShown && !bFrontMenu)
	{
		CheatSpin += FRotator::NormalizeAxis(HeadYaw - CheatSpinLastYaw);
		if (FMath::Abs(CheatSpin) >= 360.f)
		{
			bCheatsShown = true;
			if (MenuPage->File == TEXT("main_game.rml")) bMenuRebuild = true;
			MenuToastText(TEXT("Cheats unlocked"));
			MenuToastLeft = 3.f;
			Pulse(0, 0.5f);
			Pulse(1, 0.5f);
			UE_LOG(LogOutlaws, Log, TEXT("OLRT cheats shown (a whole turn with the menu up)"));
		}
	}
	CheatSpinLastYaw = HeadYaw;
	const float Step = FMath::Min(Dt, 0.05f);
	auto Follow = [Step](float& Angle, float& Vel, bool& bFollow, float Err, float Start)
	{
		if (FMath::Abs(Err) > Start) bFollow = true;
		if (bFollow && FMath::Abs(Err) < 1.5f && FMath::Abs(Vel) < 6.f) bFollow = false;
		constexpr float W = 4.5f;                                    // (a critically damped spring: no overshoot)
		const float Want = bFollow ? W * W * Err : 0.f;
		Vel += (Want - 2.f * W * Vel) * Step;
		Angle += Vel * Step;
	};
	// REQ-SCORESIDE (user, Quest build 47): in a match the live frag table stands to the left of the pause panel (and of the score
	// screen at the match's end), the pause panel to the right, the pair centred on where the menu faces and turning with it.
	// Each panel's middle is a head turn of about 25 degrees off, so the pair only sets off after a wider turn (50)
	const bool bScorePair = UOutlawsCoop::WantsPauseScoreboard(this) && (!bFrontMenu || MenuPage->File == TEXT("#coopscore"));
	constexpr float ScoreSideW = 200.f;                 // the table's width, cm (the pause panel is 250, 320 for a wide page)
	// (on a flat screen the view is narrower than a headset's: the pair stands further out, smaller, so both fit in it; the menu
	// draws over the world, so only its size changes)
	const float PairDist = bScorePair && !bVR ? 340.f : MenuDistance;
	float PauseYawOff = 0.f, ScoreYawOff = 0.f;
	if (bScorePair)
	{
		const float PauseHalf = FMath::RadiansToDegrees(FMath::Atan(SheetW * MenuScale * 0.5f / PairDist));
		const float ScoreHalf = FMath::RadiansToDegrees(FMath::Atan(ScoreSideW * 0.5f / PairDist));
		const float Sep = PauseHalf + ScoreHalf + 3.f;  // (3 degrees between the two)
		PauseYawOff = (Sep + ScoreHalf - PauseHalf) * 0.5f;
		ScoreYawOff = PauseYawOff - Sep;
	}
	Follow(MenuYaw, MenuYawVel, bMenuYawFollow, FRotator::NormalizeAxis(HeadYaw - MenuYaw), bScorePair ? 50.f : 25.f);
	MenuYaw = FRotator::NormalizeAxis(MenuYaw);
	Follow(MenuPitch, MenuPitchVel, bMenuPitchFollow, HeadPitch - MenuPitch, 20.f);
	// always out at the same distance: it draws over the world, so walls in between do not matter; tipped to face
	// the eyes, the rows' middle on the line of sight (REQ-MENUCENTRE)
	const double Dist = PairDist;
	const double Scale = MenuScale;
	PlaceMenuRoot(PauseYawOff, Dist);
	KeepSheetOverWorld(MenuSheet, Eye);          // (REQ-SUBMENUSPACE: the taller sheet too)
	// REQ-SCORESIDE: the table to the left, its middle at the pause panel's height, the same distance out, facing the eyes
	if (bScorePair)
	{
		const FRotator ScoreFace(-MenuPitch, MenuYaw + ScoreYawOff + 180.0, 0.0);
		const FVector ScoreCentre = Eye + FRotator(MenuPitch, MenuYaw + ScoreYawOff, 0.0).Vector() * Dist - FRotationMatrix(ScoreFace).GetUnitAxis(EAxis::Z) * 12.0 * Scale;
		UOutlawsCoop::UpdatePauseScoreboard(this, Eye, ScoreCentre, -FRotationMatrix(ScoreFace).GetUnitAxis(EAxis::Y), ScoreSideW);
	}
	else
	{
		UOutlawsCoop::HidePauseScoreboard(this);
	}

	// scroll with either stick
	const float Stick = FMath::Abs(MoveStick().Y) > FMath::Abs(TurnStick().Y) ? MoveStick().Y : TurnStick().Y;
	MenuScrollWait -= Dt;
	const int32 First = MenuPage->Message.IsEmpty() ? 0 : 4;
	const int32 MaxScroll = IsAchievPage() ? FMath::Max(0, MenuPage->Items.Num() - AchievRowsShown) : bWideSheet ? FMath::Max(0, MenuPage->Items.Num() + First - 7) : SheetMaxScroll();
	if (FMath::Abs(Stick) > 0.6f && MenuScrollWait <= 0.f)
	{
		MenuScroll = FMath::Clamp(MenuScroll + (Stick < 0.f ? 1 : -1), 0, MaxScroll);
		MenuScrollWait = 0.15f;
		RefreshMenuRows();
	}

	// each hand points a laser at the panel; the trigger picks the row it points at
	// (the title screens: the dominant hand's pointing finger alone, the laser out of its tip along it; user 2026-10-01)
	const bool bFingerPointer = bMenuPistol && bVR;
	const int32 Dom = MenuPointerHand();
	for (int32 H = 0; H < 2; ++H)
	{
		UMotionControllerComponent* Aim = AimOf(H);
		FVector O = Aim->GetComponentLocation();
		FVector D = Aim->GetForwardVector();
		const bool bOffHand = bFingerPointer && H != Dom;
		if (bFingerPointer && !bOffHand)
		{
			if (const UOutlawsBoxHand* Box = H == 0 ? LeftHand.Get() : RightHand.Get())
			{
				const FTransform Bx = Box->GetComponentTransform();
				O = Bx.TransformPosition(Box->IndexTip());
				D = Bx.TransformVectorNoScale(Box->IndexWay()).GetSafeNormal();
			}
		}
		float Length = 150.f;
		int32 Row = -1;
		bool bOnBack = false;
		if (bVR && !bOffHand)
		{
			Row = MenuRowUnderRay(O, D, Length);
			// (REQ-BACKCLICK, user 15 Oct) the Back prompt under the rows points and pulls as a row does
			if (Row < 0) bOnBack = MenuBackUnderRay(O, D, Length);
		}
		else if (!bVR && H == 1 && bDesktopMenuMouse && Camera)
		{
			// (REQ-TRIGGERSTEP) the desktop, once its mouse has been used in a menu: the view's middle points (the right hand's hover)
			float Unused = 0.f;
			Row = MenuRowUnderRay(Camera->GetComponentLocation(), Camera->GetForwardVector(), Unused);
		}
		if (MenuLaser[H])
		{
			if (bOffHand)
			{
				MenuLaser[H]->SetVisibility(false);
			}
			else if (bFingerPointer)
			{
				MenuLaser[H]->SetVisibility(true);
				MenuLaser[H]->SetWorldLocationAndRotation(O + D * (Length * 0.5), D.Rotation());
				MenuLaser[H]->SetWorldScale3D(FVector(Length / 100.0, 0.003, 0.003));
			}
			else
			{
				MenuLaser[H]->SetRelativeLocationAndRotation(FVector(Length * 0.5, 0.0, 0.0), FRotator::ZeroRotator);
				MenuLaser[H]->SetRelativeScale3D(FVector(Length / 100.0, 0.004, 0.004));
			}
		}
		if (UOutlawsBeacon::HandOnBubble(this, H)) { Row = -1; bOnBack = false; }      // (R11: the join bubble in front of the menu has this hand)
		if (bOnBack != MenuBackHover[H])
		{
			MenuBackHover[H] = bOnBack;
			if (bOnBack)
			{
				Pulse(H, 0.1f);
				if (AOutlawsLevelActor* L = Level.Get()) L->PlayMenuSound(TEXT("riflout"));
			}
			RedrawMenu();
		}
		if (bMenuHoverHeld && Row >= 0 && Row != MenuHover[H])
		{
			// (REQ-VOICEPAGE: the row the menu lit stays lit until a hand points at another row)
			bMenuHoverHeld = false;
			MenuHover[1 - H] = -1;
		}
		if (Row != MenuHover[H] && !bMenuHoverHeld)
		{
			MenuHover[H] = Row;
			if (Row >= 0)
			{
				Pulse(H, 0.1f);
				// the remaster's hover sounds (its exe names riflout.wav, and chicken1.wav for the entry of
				// class "chicken": Exit)
				const int32 Item = MenuScroll + Row - First;
				const bool bChicken = MenuPage->Items.IsValidIndex(Item) && MenuPage->Items[Item].Classes.Contains(TEXT("chicken"));
				if (AOutlawsLevelActor* L = Level.Get()) L->PlayMenuSound(bChicken ? TEXT("chicken1") : TEXT("riflout"));
			}
			RedrawMenu();
		}
		MenuHoverBox[H]->SetVisibility(Row >= 0 && !MenuSheet);
		if (Row >= 0)
		{
			MenuHoverBox[H]->SetRelativeLocation(FVector(-0.2 - 0.1 * H, 0.0, RowZ(Row)));
		}
		const float Trigger = Axis(H == 0 ? TEXT("OLVR_LeftTrigger") : TEXT("OLVR_RightTrigger"));
		if (Trigger < 0.3f)
		{
			bMenuTriggerArmed[H] = true;
		}
		else if (Trigger > 0.7f && bMenuTriggerArmed[H])
		{
			bMenuTriggerArmed[H] = false;
			if (MenuPage->File == TEXT("#unlocked"))
			{
				UnlockPageContinue();                      // (REQ-UNLOCKS: any button carries on, pointed at a row or not)
				return;
			}
			if (bOnBack)
			{
				MenuBack();                                // (REQ-BACKCLICK) as the B / Y button
				return;
			}
			int32 Pick = Row >= 0 ? MenuScroll + Row - First : INDEX_NONE;
			if (Pick == INDEX_NONE && bVR && OneHand() < 0 && MenuHover[1 - H] >= 0 && !UOutlawsBeacon::HandOnBubble(this, H))
			{
				// REQ-TRIGGERSTEP: this hand points at no row; a value row the other hand points at is stepped by this trigger
				// (the right up, the left down), so one pointer works both ways
				const int32 Other = MenuScroll + MenuHover[1 - H] - First;
				if (IsMenuValueRow(Other)) Pick = Other;
			}
			if (Pick != INDEX_NONE)
			{
				MenuTriggerPress(H, Pick);
				if (!bMenuOpen) return;
			}
		}
	}
	if (IsFrontPage() && MenuPage->File == TEXT("#front"))
	{
		FrontSpin += float(FApp::GetDeltaTime()) * 3.f;      // the lens: a turn every two minutes
		if (FrontSpin >= 360.f) FrontSpin -= 360.f;
		static double LastSpinDraw = 0.0;
		if (FPlatformTime::Seconds() - LastSpinDraw > 1.0 / 20.0) { LastSpinDraw = FPlatformTime::Seconds(); RedrawMenu(); }
	}
	if (MenuPage->File.StartsWith(TEXT("#coop")))
	{
		// a co-op page follows the session: built again when the lobby moves on, its state lines redrawn a few times a second
		const uint32 CoopVersion = UOutlawsCoop::PageVersion(this);
		if (CoopVersion != CoopPageSeen)
		{
			CoopPageSeen = CoopVersion;
			bMenuRebuild = true;
		}
		static double LastCoopDraw = 0.0;
		if (FPlatformTime::Seconds() - LastCoopDraw > 0.25) { LastCoopDraw = FPlatformTime::Seconds(); RedrawMenu(); }
	}
	if (MenuPage->File == TEXT("#voice"))
	{
		// (R8) the microphone meter row, ten times a second (REQ-VOICEPAGE: on the Voice Chat page)
		static double LastMeterDraw = 0.0;
		if (FPlatformTime::Seconds() - LastMeterDraw > 0.1) { LastMeterDraw = FPlatformTime::Seconds(); RefreshMenuRows(); }
	}
	if (MenuToastLeft > 0.f)
	{
		MenuToastLeft -= Dt;
		if (MenuToastLeft <= 0.f)
		{
			MenuToast->SetText(FText::GetEmpty());
			RedrawMenu();
		}
	}
}

bool AOutlawsPlayer::IsSavePage() const
{
	return MenuPage && (MenuPage->File == TEXT("save_game.rml") || MenuPage->File == TEXT("load_game.rml") || IsAchievPage());     // (REQ-EXTRAS: the same wide sheet)
}

void AOutlawsPlayer::SetSheetShape(bool bWide, bool bTall)
{
	// the save screen is the remaster's 16:9 layout: a wider, shorter sheet and a bigger canvas; a titled page's sheet is half as
	// tall again as the pause page's (REQ-SUBMENUSPACE), its canvas with it
	bTall = bTall && !bWide;
	if (!MenuSheet || !MenuCanvas || !MenuMask || (bWide == bWideSheet && bTall == bTallSheet))
	{
		return;
	}
	bWideSheet = bWide;
	bTallSheet = bTall;
	SheetW = bWide ? 128.f : PanelW;
	SheetH = bWide ? 72.f : bTall ? TallPanelH : PanelH;
	const int32 W = bWide ? 1600 : CanvasW, H = bWide ? 900 : CanvasH(SheetH);
	MenuCanvas->ResizeTarget(W, H);
	MenuMask->ResizeTarget(W, H);
	MenuSheet->SetRelativeScale3D(FVector(SheetW / 100.0, SheetH / 100.0, 1.0));
	RedrawMenu();
}

float AOutlawsPlayer::MenuGlyphPx(const UCanvas* C) const
{
	// the sheet is laid out in cm at MenuScale, MenuDistance from the eyes; its canvas spans SheetW
	return C && SheetW > 0.f ? MenuGlyphCm() * float(C->ClipX) / SheetW : 0.f;
}

float AOutlawsPlayer::MenuGlyphCm() const
{
	// (REQ-BACKCLICK, user 15 Oct) the menus' drawings three quarters of the death screen's seen size; the death screen keeps its own
	return MenuGlyphSeenCm() * MenuGlyphShrink;
}

float AOutlawsPlayer::MenuGlyphSeenCm() const
{
	return OutlawsGlyphs::ViewTan * MenuDistance / MenuScale;
}

float AOutlawsPlayer::SheetTitleGapSeenCm(UCanvas* C, float Dist)
{
	// (REQ-DEATHGAP) as DrawMenuCanvas lays a titled (tall) sheet out: the title's middle 17.5 cm up, the first row's at
	// PageRowTop, both lifted alike; each line's letters YL * 0.31 either side of its middle (the drawn boxes' rule)
	UFont* TF = GlyphFont(true);
	UFont* RF = GlyphFont(false);
	if (!C || !TF || !RF) return 0.f;
	float XL = 0.f, TitleYL = 0.f, RowYL = 0.f;
	C->TextSize(TF, TEXT("Options"), XL, TitleYL);
	C->TextSize(RF, TEXT("Multiplayer"), XL, RowYL);
	const float S = float(CanvasW) / PanelW;
	const float Centres = 17.5f - (RowTop - 2.f);
	const float GapSheetCm = Centres - (TitleYL + RowYL) * 0.31f / S;
	return GapSheetCm * Dist / (MenuDistance / MenuScale);
}

float AOutlawsPlayer::SheetTitleGapDrawnSeenCm(float Dist) const
{
	if (!SheetTitleInk.bIsValid || SheetRowInk.Num() == 0 || SheetCanvasSize.X <= 0) return -1.f;
	const float S = float(SheetCanvasSize.X) / PanelW;
	return float(SheetRowInk[0].Min.Y - SheetTitleInk.Max.Y) / S * Dist / (MenuDistance / MenuScale);
}

bool AOutlawsPlayer::MenuBackUnderRay(const FVector& O, const FVector& D, float& Length) const
{
	if (!MenuRoot || !MenuPage || MenuStack.Num() == 0) return false;
	const FTransform Panel = MenuRoot->GetComponentTransform();
	const FVector N = Panel.GetUnitAxis(EAxis::X);
	const double Den = FVector::DotProduct(D, N);
	if (Den >= -1e-3) return false;
	const double T = FVector::DotProduct(Panel.GetLocation() - O, N) / Den;
	if (T <= 0.0 || T >= 600.0) return false;
	const FVector Local = Panel.InverseTransformPosition(O + D * T);
	// the wide sheets in VR: the Back part of the help plate in the sheet's corner
	if (MenuHelpPlate && MenuHelpPlate->IsVisible() && (bWideSheet || IsFrontPage() || IsSavePage()))
	{
		const FVector At = MenuHelpPlate->GetRelativeLocation();
		const float Wc = MenuHelpPlate->WidthCmNow(), Hc = MenuHelpPlate->HeightCm();
		const FVector2D Uv(0.5 - (Local.Y - At.Y) / Wc, 0.5 - (Local.Z - At.Z) / Hc);
		int32 R = -1, P = -1;
		if (Uv.X < 0.0 || Uv.X > 1.0 || Uv.Y < 0.0 || Uv.Y > 1.0 || !MenuHelpPlate->PartAt(Uv, R, P, 14.f)) return false;
		const TArray<FOutlawsGlyphRow>& Rows = MenuHelpPlate->GetRows();
		if (!Rows.IsValidIndex(R) || P < 1 || !Rows[R].More.IsValidIndex(P - 1) || Rows[R].More[P - 1].Label != TEXT("Back")) return false;
		Length = float(T);
		return true;
	}
	// a sheet page: the foot line's Back as last drawn (its words and, in VR, the controllers under them), a little round it
	if (bWideSheet || !SheetBackInk.bIsValid || SheetCanvasSize.X <= 0) return false;
	const double S = SheetCanvasSize.X / PanelW;
	const FVector2D Px((0.5 - Local.Y / PanelW) * SheetCanvasSize.X, (PageSheetH() * 0.5 - Local.Z) * S);
	if (!SheetBackInk.ExpandBy(1.2 * S).IsInside(Px)) return false;
	Length = float(T);
	return true;
}

void AOutlawsPlayer::UpdateMenuHelpPlate(const FString& Line)
{
	TArray<OutlawsGlyphs::FPart> Parts;
	if (Line.IsEmpty() || !bVR || !MenuRoot || !GlyphLineParts(Line, Parts))
	{
		if (MenuHelpPlate) MenuHelpPlate->SetVisibility(false);
		return;
	}
	if (!MenuHelpPlate)
	{
		// as wide as the death screen's plate is seen (so its words and drawings are the same size), in the menu's cm
		const float PlateDist = MenuDistance / MenuScale;
		MenuHelpPlate = NewObject<UOutlawsGlyphPlate>(this);
		MenuHelpPlate->SetupAttachment(MenuRoot);
		MenuHelpPlate->RegisterComponent();
		MenuHelpPlate->Init(LoadOverlayMaterial(TEXT("M_OLR_MenuOverlay")), GlyphFont(false), GlyphFont(true), 110.f * PlateDist / 140.f, 440, MenuGlyphCm());   // (BUG-MENUGLYPH2: tall enough for two rows)
	}
	for (OutlawsGlyphs::FPart& Pt : Parts) Pt.bHot = Pt.Label == TEXT("Back") && IsMenuBackHot();     // (REQ-BACKCLICK) pointed at
	TArray<FOutlawsGlyphRow> Rows;
	if (Parts.Num() == 1)
	{
		FOutlawsGlyphRow& R = Rows.AddDefaulted_GetRef();
		R.Glyph = Parts[0].Glyph;
		R.Text = Parts[0].Label;
		R.bRight = Parts[0].bRight;          // (REQ-BACKRIGHT)
	}
	else
	{
		// (BUG-MENUGLYPH2, user 18 Oct) Select over Back, one to a row, so the two fit the sheet's corner clear of its torn edge
		// (side by side they are 28 cm wide; the art past the torn edge has about 25).  Each prompt is its row's second part
		// after an empty one, so the rows line up alike and Back stays a part the laser can light (MenuBackUnderRay)
		for (const OutlawsGlyphs::FPart& Pt : Parts) Rows.AddDefaulted_GetRef().More.Add(Pt);
	}
	if (MenuHelpPlate->SetRows(Rows))
	{
		FString Said;
		for (const FOutlawsGlyphRow& R : Rows) for (const OutlawsGlyphs::FPart& Pt : R.More.Num() > 0 ? R.More : TArray<OutlawsGlyphs::FPart>{ { R.Glyph, R.Text } })
			Said += FString::Printf(TEXT("%s%s \"%s\""), Said.IsEmpty() ? TEXT("") : TEXT(", "), *Pt.Glyph.Describe(), *Pt.Label);
		UE_LOG(LogOutlaws, Log, TEXT("OLGLYPH menu help plate: %s"), *Said);
	}
	// (BUG-MENUGLYPH, BUG-MENUGLYPH2) every wide page: the prompts inside the sheet at its bottom right, their drawn words and
	// controllers MenuHelpInsetCm in from the right edge and the foot, a little in front of the sheet (the plate draws over it)
	const FBox2D Ink = MenuHelpPlate->InkBox();
	const float CmPerPx = MenuHelpPlate->WidthCmNow() / float(MenuHelpPlate->PixWide());
	const float RightCm = Ink.bIsValid ? (Ink.Max.X - MenuHelpPlate->PixWide() * 0.5f) * CmPerPx : MenuHelpPlate->WidthCmNow() * 0.5f;
	const float FootCm = Ink.bIsValid ? Ink.Max.Y * CmPerPx - MenuHelpPlate->HeightCm() * 0.5f : MenuHelpPlate->HeightCm() * 0.5f;
	// (the save and load pages: 1 cm in from the right edge, so the two rows clear the slot list's frame, which ends 17 cm from it)
	// (REQ-EXTRAS: the achievements page's VR prompts left of its frame, where the remaster puts that page's help, menu.rcss
	// menu_helps.achiev right 990: at the sheet's corner the two rows stood over the frame)
	const float InsetRight = IsAchievPage() ? (1920.f - 953.f + 24.f) * SheetW / 1920.f : IsSavePage() ? 1.f : MenuHelpInsetCm;
	MenuHelpPlate->PlaceFacing(FVector(-0.4, -(SheetW * 0.5f - InsetRight - RightCm), -SheetH * 0.5f + MenuHelpInsetCm + FootCm), FQuat::Identity, true);
	MenuHelpPlate->SetVisibility(bMenuOpen);
}

UTexture2D* AOutlawsPlayer::KpfArt(const FString& Name)
{
	if (const TObjectPtr<UTexture2D>* Hit = MenuArt.Find(Name))
	{
		return *Hit;
	}
	UTexture2D* T = nullptr;
	const FString Path = TEXT("menu/assets/") + Name + TEXT(".dds");
	TArray<uint8> D;
	if (LoadMenuSource() && MenuSource->ReadArt(Path, D) && D.Num() > 148 && FMemory::Memcmp(D.GetData(), "DDS ", 4) == 0)
	{
		int32 H = 0, W = 0;
		FMemory::Memcpy(&H, D.GetData() + 12, 4);
		FMemory::Memcpy(&W, D.GetData() + 16, 4);
		if (FMemory::Memcmp(D.GetData() + 84, "DX10", 4) == 0)
		{
			// the DX10 header: DXGI 98 / 99 is BC7 (UI.kpf's art), one mip
			uint32 Dxgi = 0;
			FMemory::Memcpy(&Dxgi, D.GetData() + 128, 4);
			if (Dxgi == 98 || Dxgi == 99) T = OutlawsTextures::MakeTextureBC7(W, H, 1, D.GetData() + 148, D.Num() - 148, TF_Bilinear, TA_Clamp, true);
		}
		else
		{
			T = KpfDds(Path);
		}
	}
	// (BUG-MENUBLACK) clamped as it is made, one upload: a second UpdateResource here drew the art black or garbage
	// once the CPU copy went after the first (BUG-RESPAWNCRASH step 3)
	if (!T)
	{
		UE_LOG(LogOutlaws, Warning, TEXT("OLRT menu art: no %s"), *Path);
	}
	MenuArt.Add(Name, T);
	return T;
}

UFont* AOutlawsPlayer::SaveFont(int32 Kind)
{
	if (const TObjectPtr<UFont>* Hit = SaveFonts.Find(Kind))
	{
		return *Hit;
	}
	// menu.rcss sizes (px in the 1920 wide layout) at the canvas's 1600: font sizes are points (x 4/3 px)
	const float K = 1600.f / 1920.f * 0.75f;
	static const TCHAR* Rye = TEXT("fonts/Rye-Regular.ttf");
	static const TCHAR* Montagu = TEXT("fonts/MontaguSlab-VariableFont_opsz,wght.ttf");
	static const TCHAR* MontaguBold = TEXT("fonts/MontaguSlab_24pt-Bold.ttf");
	static const struct { const TCHAR* Path; float Px; } Kinds[] = {
		{ Rye, 101.f },          // 0 menu_title: Rye 76pt
		{ TEXT("fonts/Ultra-Regular.ttf"), 38.f },   // 1 menu_save_slot
		{ Montagu, 24.f },       // 2 name, game time
		{ Montagu, 18.f },       // 3 timestamp
		{ Montagu, 43.f },       // 4 "Empty"
		{ MontaguBold, 32.f },   // 5 menu_help
		{ Rye, 103.f },          // 6 main-large
		{ MontaguBold, 103.f },  // 7 main-large, pointed at
		{ Rye, 61.f },           // 8 main-small
		{ MontaguBold, 61.f },   // 9 main-small, pointed at
		{ MontaguBold, 70.f },   // 10 diff_button
		{ Rye, 72.f },           // 11 menu_link.main (the missions)
		{ MontaguBold, 72.f },   // 12 ... pointed at
		{ Rye, 42.f },           // 13 co-op host lobby rows (REQ-COOPLOBBY: smaller than main-small so the rows keep apart)
		{ MontaguBold, 42.f } }; // 14 ... pointed at
	const int32 Use = FMath::Clamp(Kind, 0, int32(UE_ARRAY_COUNT(Kinds)) - 1);
	UFont* F = KpfFont(Kinds[Use].Path, FMath::RoundToInt(Kinds[Use].Px * K));
	SaveFonts.Add(Kind, F);
	return F;
}

void AOutlawsPlayer::DrawSaveCanvas(UCanvas* C, int32 Width, int32 Height)
{
	// load_game.rml / save_game.rml with menu.rcss's layout (1920 x 1080, dp): the parchment (Load_Base, 2560 x
	// 1440 centred), the title over the left half, the level picture in the parchment's frame, the slot list
	// (#401000, capsules 748 x 100) under Load_Frame, the helps along the bottom
	const float K = Width / 1920.f;
	if (bMenuMaskPass)
	{
		// the parchment covers the whole sheet
		FCanvasTileItem All(FVector2D(0.f, 0.f), GWhiteTexture, FVector2D(Width, Height), FLinearColor::White);
		All.BlendMode = SE_BLEND_Opaque;
		C->DrawItem(All);
		return;
	}
	auto Col = [](uint8 R, uint8 G, uint8 B) { return FLinearColor::FromSRGBColor(FColor(R, G, B)); };
	auto Tile = [&](UTexture2D* T, float X, float Y, float W, float H, FVector2D UV0 = FVector2D(0.f, 0.f), FVector2D UV1 = FVector2D(1.f, 1.f))
	{
		if (!T || !T->GetResource()) return;
		FCanvasTileItem I(FVector2D(X * K, Y * K), T->GetResource(), FVector2D(W * K, H * K), UV0, UV1, FLinearColor::White);
		I.BlendMode = SE_BLEND_Translucent;
		C->DrawItem(I);
	};
	auto Fill = [&](float X, float Y, float W, float H, const FLinearColor& Colour)
	{
		FCanvasTileItem I(FVector2D(X * K, Y * K), GWhiteTexture, FVector2D(W * K, H * K), Colour);
		I.BlendMode = SE_BLEND_Opaque;
		C->DrawItem(I);
	};
	// Align: 0 left, 1 centre, 2 right of X; Y is the line's middle
	// (BUG-MENUGLYPH2: returns its box, view coordinates)
	auto Txt = [&](UFont* F, const FString& Str, float X, float Y, int32 Align, const FLinearColor& Colour, float MaxW, bool bShadow = false) -> FBox2D
	{
		if (!F || Str.IsEmpty()) return FBox2D(ForceInit);
		FBox2D Glyphs;
		if (Align == 1 && DrawGlyphLine(C, Str, F, FVector2D(X * K, Y * K), Colour, FLinearColor::Black, bMenuMaskPass, Glyphs)) return FBox2D(Glyphs.Min / K, Glyphs.Max / K);   // (REQ-BUTTONGLYPHS)
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

	if (UTexture2D* Base = KpfArt(TEXT("Load_Base"))) Tile(Base, 0.f, 0.f, 1920.f, 1080.f, FVector2D(320.f / 2560.f, 180.f / 1440.f), FVector2D(2240.f / 2560.f, 1260.f / 1440.f));
	else Fill(0.f, 0.f, 1920.f, 1080.f, Col(0x9a, 0x6a, 0x30));
	// menu_title.load: top 127, 860 wide, centred; Rye, #1d0800
	Txt(SaveFont(0), MenuPage->Title, 430.f, 182.f, 1, Col(0x1d, 0x08, 0x00), 760.f);

	const int32 N = MenuPage->Items.Num();
	const int32 Hot = MenuHover[1] >= 0 ? MenuHover[1] : MenuHover[0];
	// the level picture (ev_level_picture): the slot pointed at, else the first saved one, else this level
	FString PicLevel;
	if (SlotsShown.IsValidIndex(MenuScroll + Hot) && !SlotsShown[MenuScroll + Hot].bEmpty) PicLevel = SlotsShown[MenuScroll + Hot].Level;
	for (int32 i = 0; PicLevel.IsEmpty() && i < SlotsShown.Num(); ++i) if (!SlotsShown[i].bEmpty) PicLevel = SlotsShown[i].Level;
	if (PicLevel.IsEmpty()) if (const AOutlawsLevelActor* L = Level.Get()) PicLevel = L->LevelName;
	Tile(KpfArt(FString::Printf(TEXT("Load_Level%02d"), AOutlawsLevelActor::LevelPicture(PicLevel))), 138.f, 453.f, 562.f, 306.f, FVector2D(0.f, 0.f), FVector2D(562.f / 564.f, 306.f / 308.f));

	// the list: 7 whole capsules and the top of the 8th (765 high)
	Fill(838.f, 160.f, 811.f, 765.f, Col(0x40, 0x10, 0x00));
	const FString Empty = MenuSource ? MenuSource->Loc(TEXT("$m_local_5075")) : FString(TEXT("Empty"));
	for (int32 r = 0; r < 8; ++r)
	{
		const int32 i = MenuScroll + r;
		if (!SlotsShown.IsValidIndex(i) || i >= N) break;
		const FSlotShown& S = SlotsShown[i];
		const bool bHot = r == MenuHover[0] || r == MenuHover[1];
		const float X0 = 838.f, Y0 = 160.f + 100.f * r;
		const float Vis = FMath::Min(100.f, 925.f - Y0);                  // the 8th shows only its top
		const FString Art = FString(S.Slot < 0 ? TEXT("Load_CapsuleAuto") : S.bEmpty ? TEXT("Load_CapsuleEmpty") : TEXT("Load_Capsule")) + (bHot ? TEXT("HOVER") : TEXT("OFF"));
		Tile(KpfArt(Art), X0, Y0, 748.f, Vis, FVector2D(0.f, 0.f), FVector2D(1.f, Vis / 100.f));
		const FLinearColor Ink = bHot ? FLinearColor::White : Col(0x88, 0x88, 0x88);
		if (S.Slot >= 0) Txt(SaveFont(1), FString::FromInt(S.Slot + 1), X0 + 35.f + 34.5f, Y0 + 49.f, 1, Ink, 69.f);
		if (S.bEmpty)
		{
			Txt(SaveFont(4), Empty, X0 + 132.f + 16.f, Y0 + 49.f, 0, bHot ? Col(0xf2, 0xaa, 0x44) : Col(0x88, 0x88, 0x88), 560.f);
			continue;
		}
		Txt(SaveFont(2), S.Name, X0 + 132.f + 7.f, Y0 + 35.5f, 0, Ink, 443.f);
		Txt(SaveFont(2), S.GameTime, X0 + 748.f - 39.f - 7.f, Y0 + 35.5f, 2, Ink, 108.f);
		if (Vis >= 78.f) Txt(SaveFont(3), S.Stamp, X0 + 748.f - 39.f, Y0 + 66.f, 2, bHot ? Col(0xf2, 0xaa, 0x44) : Col(0x60, 0x60, 0x60), 577.f);
	}
	// the scroll thumb (Load_SliderV) down the frame's right-hand groove
	if (N > 7)
	{
		const float Track = 765.f - 132.f;
		const float Y = 160.f + Track * float(MenuScroll) / float(FMath::Max(1, N - 7));
		Tile(KpfArt(TEXT("Load_SliderV")), 1603.f, Y, 48.f, 132.f);
	}
	Tile(KpfArt(TEXT("Load_Frame")), 822.f, 145.f, 840.f, 796.f);

	// menu_helps.load; a toast (saved!) under the list
	// (REQ-BUTTONGLYPHS: in VR on a plate, the controllers under the words; BUG-MENUGLYPH2, user 18 Oct: both inside the sheet at
	// its bottom right, the flat screen's line right-aligned MenuHelpInsetCm in from the edge, as the title pages')
	WideHelpInk = FBox2D(ForceInit);
	if (bVR) UpdateMenuHelpPlate(TEXT("Trigger  Select        B / Y  Back"));
	else WideHelpInk = Txt(SaveFont(5), TEXT("Click  Select        Esc  Back"), WideParchmentRight() - MenuHelpInsetCm * 1920.f / SheetW, 1030.f, 2, Col(0xc0, 0xc0, 0xc0), 746.f, true);
	if (MenuToast && !MenuToast->Text.IsEmpty()) Txt(SaveFont(2), MenuToast->Text.ToString(), 850.f + 373.f, 962.f, 1, Col(0xf2, 0xaa, 0x44), 746.f, true);
}

// the pause page has no title: its eight rows spread down the whole sheet (user 2026-09-27: still squished)
float AOutlawsPlayer::PageRowTop() const
{
	// the pause page has no title; a titled page's rows start lower, clear of it (user 2026-09-27)
	return MenuPage && MenuPage->File == TEXT("main_game.rml") ? 27.f : RowTop - 2.f + (IsTallSheet() ? TallLift : 0.f);
}

bool AOutlawsPlayer::IsTallSheet() const
{
	// (REQ-SUBMENUSPACE) every titled parchment page: Options, its pages and the other pages under the pause page
	return MenuPage && !bWideSheet && !IsFrontPage() && !IsSavePage() && !MenuPage->File.Equals(TEXT("main_game.rml"), ESearchCase::IgnoreCase);
}

float AOutlawsPlayer::PageSheetH() const
{
	return IsTallSheet() ? TallPanelH : PanelH;
}

float AOutlawsPlayer::PageFootZ() const
{
	// (REQ-MENUCENTRE) one gap under the last row shown
	// (REQ-RANDOCRITTERS) a last row drawn shorter than the first (the Randomizer's foot note, shrunk to the sheet's width): the
	// line comes up by the difference, so the gap from its letters is the same as on every other page
	float Short = 0.f;
	if (SheetRowInk.Num() > 1 && CanvasW > 0)
		Short = FMath::Max(0.f, float(SheetRowInk[0].GetSize().Y - SheetRowInk.Last().GetSize().Y) * 0.5f) / (float(CanvasW) / PanelW);
	return RowZ(FMath::Max(1, SheetRowsShownNow()) - 1) - MenuFootGap - BackBesideDrop() + Short;
}

float AOutlawsPlayer::BackBesideDrop() const
{
	// (REQ-BACKRIGHT) VR's Back has its controllers beside the word, centred on the line: they stand half a drawing over it,
	// more than the letters do; the line goes down by the difference
	if (!bVR || MenuStack.Num() == 0 || CanvasW <= 0) return 0.f;
	const float S = float(CanvasW) / PanelW;
	return FMath::Max(0.f, MenuGlyphCm() * 0.5f - SheetSmallHalfPx / S);
}

int32 AOutlawsPlayer::PageRowsShown() const
{
	const int32 N = IsTallSheet() ? TallRowsShown : MenuRowsShown;
	return TestSheetRowCap > 0 ? FMath::Min(N, TestSheetRowCap) : N;      // (-OLPromptCentreTest: a page made to scroll)
}

bool AOutlawsPlayer::IsSheetPage() const
{
	return MenuPage && !bWideSheet && !IsFrontPage() && !IsSavePage();
}

int32 AOutlawsPlayer::SheetRowsShownNow() const
{
	// (as RefreshMenuRows fills them: from the top, up to what fits, as far as the page's rows go at this scroll)
	if (!MenuPage) return 0;
	const int32 First = MenuPage->Message.IsEmpty() ? 0 : 4;
	return FMath::Clamp(MenuPage->Items.Num() + First - MenuScroll, 1, FMath::Min(SheetRowsFit(), PageRowsShown()));
}

float AOutlawsPlayer::SheetRowsMiddleZ() const
{
	// the rows' middle: with an even count, between the two middle rows
	return (RowZ(0) + RowZ(FMath::Max(1, SheetRowsShownNow()) - 1)) * 0.5f;
}

float AOutlawsPlayer::MenuEyeZ() const
{
	// (BUG-MENUGLYPH2) a wide sheet sits at its own height in VR too: its help plate is inside it (REQ-BUTTONGLYPHS had lifted
	// the sheet by the plate's height when the plate hung under it)
	return IsSheetPage() ? SheetRowsMiddleZ() : 12.f;
}

void AOutlawsPlayer::PlaceMenuRoot(double PauseYawOff, double Dist)
{
	if (!MenuRoot || !Camera) return;
	const FVector Eye = Camera->GetComponentLocation();
	const double D = Dist > 0.0 ? Dist : double(MenuDistance);
	const FVector Ahead = FRotator(MenuPitch, MenuYaw + PauseYawOff, 0.0).Vector();
	const FRotator Face(-MenuPitch, MenuYaw + PauseYawOff + 180.0, 0.0);
	// (REQ-MENUCENTRE) the sheet's height MenuEyeZ on the line of sight: on a sheet page the rows' middle (the panel used to sit
	// with its own middle 12 cm under it, every page's rows somewhere else)
	MenuRoot->SetWorldLocationAndRotation(Eye + Ahead * D - FRotationMatrix(Face).GetUnitAxis(EAxis::Z) * double(MenuEyeZ()) * MenuScale, Face);
	MenuRoot->SetWorldScale3D(FVector(MenuScale));
}

bool AOutlawsPlayer::MenuCentreMeasure(float& OutMiddleOffCm, float& OutPromptGapCm) const
{
	OutMiddleOffCm = 0.f;
	OutPromptGapCm = -1.f;
	if (!IsSheetPage() || !MenuRoot || !Camera) return false;
	// where the line of sight (the menu's facing, as it opened) meets the sheet, against the rows' middle; the sheet's up as the
	// height
	const FTransform Pn = MenuRoot->GetComponentTransform();
	const FVector Eye = Camera->GetComponentLocation();
	const FVector Ahead = FRotator(MenuPitch, MenuYaw, 0.0).Vector();
	const FVector Nrm = Pn.GetUnitAxis(EAxis::X);
	const double Den = FVector::DotProduct(Ahead, Nrm);
	if (FMath::Abs(Den) < 1e-3) return false;
	const FVector Hit = Eye + Ahead * (FVector::DotProduct(Pn.GetLocation() - Eye, Nrm) / Den);
	const float EyeZ = float(Pn.InverseTransformPosition(Hit).Z);
	const float K = float(Pn.GetScale3D().Z);
	OutMiddleOffCm = (SheetRowsMiddleZ() - EyeZ) * K;
	// the gap from the last row's letters to the prompt line's, as last drawn (canvas pixels to sheet cm to world cm)
	if (SheetRowInk.Num() > 0 && SheetFootInk.Num() > 0 && SheetCanvasSize.X > 0)
	{
		float RowsBottom = 0.f;
		for (const FBox2D& R : SheetRowInk) RowsBottom = FMath::Max(RowsBottom, R.Max.Y);
		float FootTop = TNumericLimits<float>::Max();
		for (const FBox2D& F : SheetFootInk) FootTop = FMath::Min(FootTop, F.Min.Y);
		OutPromptGapCm = (FootTop - RowsBottom) / (float(SheetCanvasSize.X) / PanelW) * K;
	}
	return true;
}

bool AOutlawsPlayer::PromptFootOffset(float& OutCm, FString& OutText) const
{
	OutCm = 0.f;
	OutText.Reset();
	if (!IsSheetPage() || !MenuRoot || SheetFootInk.Num() == 0 || SheetCanvasSize.X <= 0) return false;
	FBox2D All(ForceInit);
	for (const FBox2D& B : SheetFootInk) All += B;
	// canvas pixels from the sheet's middle, to sheet cm, to world cm
	OutCm = (float(All.GetCenter().X) - SheetCanvasSize.X * 0.5f) / (float(SheetCanvasSize.X) / PanelW) * float(MenuRoot->GetComponentScale().Z);
	if (MenuStack.Num() > 0) OutText = bVR ? TEXT("B or Y: back") : TEXT("Esc: back");
	else if (MenuPage->File.Equals(TEXT("main_game.rml"), ESearchCase::IgnoreCase)) OutText = PauseTimeShown;
	if (MenuMoreDown && !MenuMoreDown->Text.IsEmpty()) OutText += (OutText.IsEmpty() ? TEXT("") : TEXT(" | ")) + MenuMoreDown->Text.ToString();
	return true;
}

bool AOutlawsPlayer::PromptMoreUpOffset(float& OutCm) const
{
	OutCm = 0.f;
	if (!IsSheetPage() || !MenuRoot || !SheetMoreUpInk.bIsValid || SheetCanvasSize.X <= 0) return false;
	OutCm = (float(SheetMoreUpInk.GetCenter().X) - SheetCanvasSize.X * 0.5f) / (float(SheetCanvasSize.X) / PanelW) * float(MenuRoot->GetComponentScale().Z);
	return true;
}

float AOutlawsPlayer::RowZ(int32 r) const
{
	// a gap above every section heading after the first row (the VR page's Locomotion and Hands)
	float Z = PageRowTop() - PageRowStep() * r;
	if (MenuPage)
	{
		const int32 First = MenuPage->Message.IsEmpty() ? 0 : 4;
		for (int32 k = 1; k <= r; ++k)
		{
			const int32 i = MenuScroll + k - First;
			// (BUG-SPEEDWARN: the speed warning is a note under its row, no section; REQ-RANDOCRITTERS: the Randomizer's foot note too,
			// so its seven rows sit on the sheet without a scroll)
			if (k >= First && MenuPage->Items.IsValidIndex(i) && ((MenuPage->Items[i].Kind == OutlawsMenu::EItemKind::Heading && MenuPage->Items[i].Name != TEXT("vr_speedwarn")
				&& MenuPage->Items[i].Name != TEXT("rando_note"))
				|| MenuPage->Items[i].Name.StartsWith(TEXT("defaults:")))) Z -= 1.8f;
		}
	}
	return Z;
}

int32 AOutlawsPlayer::RowAt(float Z) const
{
	for (int32 r = 0; r < PageRowsShown(); ++r)
	{
		if (FMath::Abs(Z - RowZ(r)) <= PageRowStep() * 0.5f) return r;
	}
	return -1;
}

float AOutlawsPlayer::PageRowStep() const
{
	// (REQ-MPLOBBY c, BUG-MENUOFFPANEL: a titled page's rows a little closer, 5.15 from 5.4, so that nine of them sit clear of
	// the help line under them)
	// (REQ-SUBMENUSPACE: on the taller sheet 6.6, about halfway between that and the pause page's 8)
	return MenuPage && MenuPage->File == TEXT("main_game.rml") ? 8.f : IsTallSheet() ? TallRowStep : RowStep;
}

float AOutlawsPlayer::WideParchmentRight() const
{
	// (BUG-MENUGLYPH2) measured on the art at the foot (y 980 to 1080 of the view): the title pages' torn parchment (Main_Base,
	// Diff_Base, StartGame_Base: one shape) darkens into its burn from about x 1365; the character page's blue cloth starts at
	// about 1500 with the parchment shaded from 1440; the save and load parchment fills the sheet
	if (!MenuPage || IsSavePage()) return 1920.f;
	return MenuPage->File == TEXT("#character") ? 1440.f : 1350.f;
}

float AOutlawsPlayer::WideTornRight() const
{
	// (the torn edge's black line and burn reach x 1480 at most over the sheet's lower half; the character page has no torn edge)
	if (!MenuPage || IsSavePage()) return 1920.f;
	return MenuPage->File == TEXT("#character") ? 1440.f : 1500.f;
}

bool AOutlawsPlayer::IsFrontPage() const
{
	return MenuPage && (MenuPage->File == TEXT("#front") || MenuPage->File == TEXT("#newgame") || MenuPage->File == TEXT("#missions") || MenuPage->File.StartsWith(TEXT("#difficulty:"))
		|| MenuPage->File.StartsWith(TEXT("#coop")) || MenuPage->File == TEXT("#kbd") || MenuPage->File == TEXT("#character") || MenuPage->File == TEXT("#extras")
		|| MenuPage->File == TEXT("#extras_shelf"));
}

FString AOutlawsPlayer::CoopMenuPage() const
{
	return bMenuOpen && MenuPage ? MenuPage->File : FString();
}

FString AOutlawsPlayer::CoopMenuParent() const
{
	if (!MenuPage) return FString();
	if (MenuPage->File != TEXT("#coopjoin")) return MenuPage->File;
	return MenuStack.Num() > 0 ? MenuStack.Last() : FString();
}

bool AOutlawsPlayer::CoopPressRow(const FString& Name)
{
	if (!bMenuOpen || !MenuPage) return false;
	const int32 i = MenuPage->Items.IndexOfByPredicate([&Name](const OutlawsMenu::FItem& It) { return It.Name == Name; });
	if (i == INDEX_NONE) return false;
	MenuActivate(1, i);
	return true;
}

#if WITH_OUTLAWS_TESTS
bool AOutlawsPlayer::TestPressLink(const FString& Target)
{
	if (!bMenuOpen || !MenuPage) return false;
	const int32 i = MenuPage->Items.IndexOfByPredicate([&Target](const OutlawsMenu::FItem& It) { return It.Kind == OutlawsMenu::EItemKind::Link && It.Target.Equals(Target, ESearchCase::IgnoreCase); });
	if (i == INDEX_NONE) return false;
	MenuActivate(1, i);
	return true;
}
#endif // WITH_OUTLAWS_TESTS

FString AOutlawsPlayer::CoopRowLabel(const FString& Name) const
{
	if (!bMenuOpen || !MenuPage) return FString();
	const OutlawsMenu::FItem* It = MenuPage->Items.FindByPredicate([&Name](const OutlawsMenu::FItem& I) { return I.Name == Name; });
	return It ? It->Label : FString();
}

int32 AOutlawsPlayer::SheetRowsFit() const
{
	// (REQ-MPLOBBY c) the rows from the top that sit on the parchment sheet: each row's text above the sheet's foot, and above
	// the help / "more below" line when one is drawn; a row's and the line's heights as last drawn (the fonts' usual sizes
	// before the first draw).  The section gaps (RowZ) are what push rows down.
	if (!MenuPage || bWideSheet || IsFrontPage() || IsSavePage()) return MenuRowsShown;
	const float S = float(CanvasW) / PanelW;    // canvas pixels per cm (the sheet: 1024 x 983, a titled page's 1024 x 1229)
	const float Half_H = PageSheetH() * 0.5f;
	const int32 Most = PageRowsShown();
	const float Half = SheetRowInk.Num() > 0 ? SheetRowInk[0].GetSize().Y * 0.5f : 24.f;          // (the letters' height, not the line's)
	float FootHalf = SheetFootInk.Num() > 0 ? SheetFootInk[0].GetSize().Y * 0.5f : 16.f;
	const int32 First = MenuPage->Message.IsEmpty() ? 0 : 4;
	const bool bFoot = MenuStack.Num() > 0 || MenuPage->Items.Num() + First > Most
		|| (MenuPage->File.Equals(TEXT("main_game.rml"), ESearchCase::IgnoreCase) && !PauseTimeShown.IsEmpty());
	// (REQ-BUTTONGLYPHS) VR's Back and "more below" have their controllers under the words: that much room under the line
	// (worked out, not as last drawn: this runs while the page is drawn too, its foot not drawn yet)
	// (REQ-BACKRIGHT) Back's beside its word: half a drawing either side of the line when it is the line's only prompt
	if (bVR && MenuPage->Items.Num() + First > Most)
		FootHalf = FMath::Max(FootHalf, 16.f + MenuGlyphCm() * S * (1.f + OutlawsGlyphs::StackGap));
	else if (bVR && MenuStack.Num() > 0)
		FootHalf = FMath::Max(FootHalf, MenuGlyphCm() * S * 0.5f);
	// (REQ-MENUCENTRE) the line goes one gap under the last row shown: the rows that fit are those whose line, under the last of
	// them, still sits inside the sheet with a margin (12 px, as the fit check asks)
	const float Floor = Half_H * 2.f * S;
	int32 N = Most;
	while (N > 1)
	{
		const float RowBottom = (Half_H - RowZ(N - 1)) * S + Half;
		const float LineBottom = (Half_H - (RowZ(N - 1) - MenuFootGap - BackBesideDrop())) * S + FootHalf + 12.f;
		// (BUG-GFXTALL) and the block, "more above" to the prompts, within the height every titled page keeps
		bool bBlock = true;
		if (IsTallSheet())
		{
			float TopZ = 0.f, FootZ = 0.f;
			SheetBlockZ(N, TopZ, FootZ);
			bBlock = TopZ - FootZ <= SheetBlockMaxCm && TopZ - (RowZ(0) + RowZ(N - 1)) * 0.5f <= SheetTopMaxCm;
		}
		if (RowBottom <= Floor && (!bFoot || LineBottom <= Floor) && bBlock) break;
		--N;
	}
	return N;
}

void AOutlawsPlayer::SheetBlockZ(int32 N, float& OutTopZ, float& OutFootZ) const
{
	// (as DrawMenuCanvas lays them out) the title's middle 17.5 cm up, lifted on the tall sheet; "more above" over it, in VR its
	// stick drawn between them; the prompt line one gap under row N, in VR its controllers under the words
	OutTopZ = OutFootZ = 0.f;
	if (!MenuPage || N < 1) return;
	const float S = float(CanvasW) / PanelW;
	const float Lift = IsTallSheet() ? TallLift : 0.f;
	const float TitleZ = 17.5f + Lift;
	const float TitleHalf = SheetTitleInk.bIsValid ? float(SheetTitleInk.GetSize().Y) * 0.5f / S : 3.f;
	const float Small = SheetSmallHalfPx / S;
	const float Glyph = bVR ? MenuGlyphCm() : 0.f;       // (the drawings' height, sheet cm)
	const float RowHalf = SheetRowInk.Num() > 0 ? float(SheetRowInk[0].GetSize().Y) * 0.5f / S : 24.f / S;
	OutTopZ = TitleZ + TitleHalf;
	if (MenuScroll > 0)
	{
		// (paged down: the title makes way, "more above" over the first row, its stick between)
		OutTopZ = bVR ? RowZ(0) + RowHalf + 10.f / S + Small * 2.f + Glyph * (1.f + OutlawsGlyphs::StackGap)
			: (RowZ(0) + TitleZ) * 0.5f + Small;
	}
	const int32 First = MenuPage->Message.IsEmpty() ? 0 : 4;
	const bool bMoreDown = MenuScroll + N - First < MenuPage->Items.Num();
	const bool bFoot = MenuStack.Num() > 0 || bMoreDown;
	// (REQ-BACKRIGHT) "more below" has its stick under its words; Back alone its controllers beside them, on the line
	const float FootBelow = bMoreDown ? Small + Glyph * (1.f + OutlawsGlyphs::StackGap) : FMath::Max(Small, Glyph * 0.5f);
	OutFootZ = bFoot ? RowZ(N - 1) - MenuFootGap - BackBesideDrop() - FootBelow : RowZ(N - 1) - RowHalf;
}

float AOutlawsPlayer::SheetBlockLimitCm() { return SheetBlockMaxCm; }
float AOutlawsPlayer::SheetTopLimitCm() { return SheetTopMaxCm; }

bool AOutlawsPlayer::SheetBlockMeasure(float& OutBlockCm, float& OutTopCm, float& OutTopDeg) const
{
	OutBlockCm = OutTopCm = OutTopDeg = 0.f;
	if (!IsTallSheet() || !MenuRoot || !Camera || SheetCanvasSize.X <= 0) return false;
	FBox2D All(ForceInit);
	for (const FBox2D& B : SheetRowInk) All += B;
	for (const FBox2D& B : SheetEdgeInk) All += B;
	if (!All.bIsValid) return false;
	const float S = float(SheetCanvasSize.X) / PanelW;
	const float TopZ = PageSheetH() * 0.5f - float(All.Min.Y) / S;
	OutBlockCm = float(All.Max.Y - All.Min.Y) / S;
	OutTopCm = TopZ - SheetRowsMiddleZ();
	// the top's angle over the line of sight the menu was placed on
	const FTransform Pn = MenuRoot->GetComponentTransform();
	const FVector D = Pn.TransformPosition(FVector(0.0, 0.0, double(TopZ))) - Camera->GetComponentLocation();
	const FVector Ahead = FRotator(MenuPitch, MenuYaw, 0.0).Vector();
	OutTopDeg = float(FMath::RadiansToDegrees(FMath::Atan2(FVector::DotProduct(D, Pn.GetUnitAxis(EAxis::Z)), FVector::DotProduct(D, Ahead))));
	return true;
}

int32 AOutlawsPlayer::SheetMaxScroll()
{
	// (the scroll that shows the last row, with each scroll's own row count)
	if (!MenuPage) return 0;
	const int32 First = MenuPage->Message.IsEmpty() ? 0 : 4;
	const int32 Keep = MenuScroll;
	int32 Max = 0;
	for (int32 Sc = 0; Sc <= MenuPage->Items.Num(); ++Sc)
	{
		MenuScroll = Sc;
		Max = Sc;
		if (Sc + SheetRowsFit() - First >= MenuPage->Items.Num()) break;
	}
	MenuScroll = Keep;
	return Max;
}

void AOutlawsPlayer::DrawFrontCanvas(UCanvas* C, int32 Width, int32 Height)
{
	// the remaster's title screens (main.rml, difficulty.rml, select_rcp.rml) laid out as menu.rcss does: a
	// 2560 x 1440 stage seen through the 1920 x 1080 view (offset 320, 180); the art from UI.kpf
	const float K = Width / 1920.f;
	if (bMenuMaskPass)
	{
		FCanvasTileItem All(FVector2D(0.f, 0.f), GWhiteTexture, FVector2D(Width, Height), FLinearColor::White);
		All.BlendMode = SE_BLEND_Opaque;
		C->DrawItem(All);
		return;
	}
	FrontHits.Reset();
	FrontItemInk.Reset();
	FrontInk = FBox2D(ForceInit);
	auto Col = [](uint8 R, uint8 G, uint8 B) { return FLinearColor::FromSRGBColor(FColor(R, G, B)); };
	// a piece of art placed as the page's rules place it: the page content lives inside tpl_menu's "background"
	// element, a 4608 x 1440 strip centred on the window, so "left: L" is view x = L - 1344 and "right: R" is
	// x = 3264 - R - width; tops are y = top - 180
	auto Stage = [&](const TCHAR* Art, float SX, float SY, float W, float H, float Spin = 0.f)
	{
		UTexture2D* T = KpfArt(Art);
		if (!T || !T->GetResource()) return;
		FCanvasTileItem I(FVector2D(SX * K, SY * K), T->GetResource(), FVector2D(W * K, H * K), FLinearColor::White);
		I.BlendMode = SE_BLEND_Translucent;
		if (Spin != 0.f)
		{
			I.Rotation = FRotator(0.f, Spin, 0.f);
			I.PivotPoint = FVector2D(0.5f, 0.5f);
		}
		C->DrawItem(I);
	};
	// text at view coordinates; Align 0 left, 1 centre; returns its box (view coordinates)
	auto Txt = [&](UFont* F, const FString& Str, float X, float Y, int32 Align, const FLinearColor& Colour, bool bShadow, const FLinearColor* Outline = nullptr) -> FBox2D
	{
		if (!F || Str.IsEmpty()) return FBox2D(FVector2D(X, Y), FVector2D(X, Y));
		FBox2D Glyphs;
		if (Align == 1 && DrawGlyphLine(C, Str, F, FVector2D(X * K, Y * K), Colour, Outline ? *Outline : FLinearColor::Black, bMenuMaskPass, Glyphs))
			return FBox2D(Glyphs.Min / K, Glyphs.Max / K);       // (REQ-BUTTONGLYPHS)
		float XL = 0.f, YL = 0.f;
		C->TextSize(F, Str, XL, YL);
		float X0 = X * K;
		if (Align == 1) X0 -= XL * 0.5f;
		FCanvasTextItem T(FVector2D(X0, Y * K), FText::FromString(Str), F, Colour);
		T.bCentreY = true;
		T.BlendMode = SE_BLEND_Translucent;
		if (bShadow) T.EnableShadow(FLinearColor::Black, FVector2D(4.f * K, 2.f * K));
		if (Outline) { T.bOutlined = true; T.OutlineColor = *Outline; }
		C->DrawItem(T);
		return FBox2D(FVector2D(X0 / K, Y - YL * 0.5f / K), FVector2D((X0 + XL) / K, Y + YL * 0.5f / K));
	};
	const int32 Hot = MenuHover[1] >= 0 ? MenuHover[1] : MenuHover[0];
	const FLinearColor Warm = Col(0xe9, 0x93, 0x1c), White = FLinearColor::White, Black = FLinearColor::Black;
	// a main-menu link: hollow Rye in black; pointed at, the solid font in warm under it (menu.rcss DUAL-LAYER)
	// (REQ-MPLOBBY b: a row marked "disabled", the locked lobby's Enter Game, is drawn greyed out and cannot be pointed at)
	auto Link = [&](int32 Index, const FString& Str, float X, float Y, int32 Align, int32 RyeKind, int32 FillKind, float PadX, float PadY)
	{
		const bool bOff = MenuPage->Items.IsValidIndex(Index) && MenuPage->Items[Index].Classes.Contains(TEXT("disabled"));
		FBox2D Box;
		if (Index == Hot && !bOff)
		{
			Txt(SaveFont(FillKind), Str, X, Y, Align, Warm, false, &White);
			Box = Txt(SaveFont(RyeKind), Str, X, Y, Align, Black, false);
		}
		else
		{
			Box = Txt(SaveFont(RyeKind), Str, X, Y, Align, bOff ? Col(0x80, 0x76, 0x6a) : Black, false);
		}
		FrontInk += Box;
		if (Index >= 0)
		{
			while (FrontItemInk.Num() <= Index) FrontItemInk.Add(FBox2D(ForceInit));
			FrontItemInk[Index] = Box;
		}
		Box = Box.ExpandBy(FVector2D(PadX, PadY));
		if (!bOff) FrontHits.Emplace(Box, Index);
	};
	const FString File = MenuPage->File;
	if (File == TEXT("#front"))
	{
		Stage(TEXT("Main_Background"), 3264.f - 2000.f, -180.f, 2000.f, 1440.f);          // right: 0
		Stage(TEXT("Main_Lens"), 3264.f - 1024.f - 1380.f, 64.f - 180.f, 1380.f, 1204.f, FrontSpin);   // right: 1024, top: 64
		Stage(TEXT("Main_Base"), 1024.f - 1344.f, -180.f, 1844.f, 1440.f);                // left: 1024
		// menu_items.root: centred on x 828 from y 340; the large links (those marked "big": Load Game, New Game; one alone sits
		// between the two places), then the small row 50 under them
		int32 Big = 0;
		while (Big < MenuPage->Items.Num() && MenuPage->Items[Big].Classes.Contains(TEXT("big"))) ++Big;
		float Y = 340.f + 25.f + 129.f * 0.5f + (Big == 1 ? (129.f + 25.f + 34.f - 21.f) * 0.5f : 0.f);
		for (int32 i = 0; i < Big; ++i)
		{
			Link(i, MenuPage->Items[i].Label, 828.f, Y, 1, 6, 7, 40.f, 16.f);
			Y += 129.f + 25.f + 34.f - 21.f;
		}
		const float RowY = 340.f + 2.f * (25.f + 129.f + 34.f - 21.f) + 21.f + 50.f + 11.f + 77.f * 0.5f;
		TArray<float> Widths;
		float Total = 0.f;
		for (int32 i = Big; i < MenuPage->Items.Num(); ++i)
		{
			float XL = 0.f, YL = 0.f;
			C->TextSize(SaveFont(8), MenuPage->Items[i].Label, XL, YL);
			Widths.Add(XL / K);
			Total += XL / K + 72.f;
		}
		float X = 828.f - Total * 0.5f + 36.f;
		for (int32 i = Big; i < MenuPage->Items.Num(); ++i)
		{
			Link(i, MenuPage->Items[i].Label, X, RowY, 0, 8, 9, 30.f, 12.f);
			X += Widths[i - Big] + 72.f;
		}
	}
	else if (File.StartsWith(TEXT("#difficulty:")))
	{
		Stage(TEXT("Diff_Background"), 3264.f - 1780.f, -180.f, 1780.f, 1440.f);           // right: 0
		Stage(TEXT("Diff_Lens"), 3264.f - 512.f - 1380.f, 64.f - 180.f, 1380.f, 1204.f, FrontSpin);   // right: 512
		Stage(TEXT("Diff_Base"), 1024.f - 1344.f, -180.f, 1844.f, 1440.f);
		Txt(SaveFont(0), MenuPage->Title, 815.f, 176.f, 1, Col(0x1d, 0x08, 0x00), false);
		// diff_button: Montagu Slab bold 70, grey, yellow when pointed at, left at 565 from 468
		for (int32 i = 0; i < MenuPage->Items.Num(); ++i)
		{
			const float Y = 468.f + 4.f + 39.f + i * 92.f;
			FBox2D Box = Txt(SaveFont(10), MenuPage->Items[i].Label, 565.f, Y, 0, i == Hot ? Col(0xff, 0xef, 0x65) : Col(0x88, 0x88, 0x88), true);
			FrontHits.Emplace(Box.ExpandBy(FVector2D(40.f, 10.f)), i);
		}
		// (REQ-EXTRAS) New Game > Main Game: one line under the difficulties, what there is to earn
		if (File == TEXT("#difficulty:story"))
		{
			Txt(SaveFont(2), TEXT("The Randomizer is in Options > Gameplay. Find the hidden boxes for more."),
				565.f, 468.f + 4.f + 39.f + 3.f * 92.f + 10.f, 0, Col(0xc0, 0xc0, 0xc0), true);
		}
	}
	else if (File.StartsWith(TEXT("#coop")))
	{
		// the co-op pages (plain): the start pages' parchment and title, a few lines of the session's state, then the rows; the
		// join page's rows are a number pad (1 2 3 / 4 5 6 / 7 8 9 / . 0 Delete) with Join under it
		Stage(TEXT("StartGame_Background"), 3264.f - 2000.f, -180.f, 2000.f, 1440.f);
		Stage(TEXT("StartGame_Base"), 1024.f - 1344.f, -180.f, 1844.f, 1440.f);
		const FLinearColor Ink = Col(0x1d, 0x08, 0x00);
		Txt(SaveFont(0), MenuPage->Title, 815.f, 176.f, 1, Ink, false);
		TArray<FString> Lines;
		UOutlawsCoop::StatusLines(this, File, Lines);
		// (the address, the line to read out or type, larger than the rest)
		const bool bBigFirst = File == TEXT("#coophost") || File == TEXT("#coopjoin");
		float Y = 362.f;
		for (int32 k = 0; k < Lines.Num(); ++k)
		{
			const bool bBig = bBigFirst && k == 0;
			FrontInk += Txt(SaveFont(bBig ? 9 : 5), Lines[k], 815.f, bBig ? Y + 6.f : Y, 1, Ink, false);
			Y += bBig ? 66.f : File == TEXT("#coophost") || File == TEXT("#cooplobby") || File == TEXT("#coopclient") || File == TEXT("#coopscore") || File == TEXT("#cooppause") ? 38.f : 44.f;      // (REQ-COOPLOBBY: the host's lines closer, for its rows)
		}
		for (int32 i = 0; i < MenuPage->Items.Num(); ++i)
		{
			if (File == TEXT("#coopjoin"))
			{
				// (REQ-MPMENU: the rows placed by name, not by index: since M1's Password row the Join row had fallen into the
				// Recent column, under a "Recent" heading with no hosts in it)
				const FString& Nm = MenuPage->Items[i].Name;
				if (Nm.StartsWith(TEXT("coop:recent:")))
				{
					// (R8) the Recent hosts, a column to the right of the pad
					const int32 r = FCString::Atoi(*Nm.Mid(12));
					if (r == 0) Txt(SaveFont(5), TEXT("Recent"), 1330.f, 452.f, 1, Ink, false);
					Link(i, MenuPage->Items[i].Label, 1330.f, 500.f + float(r) * 90.f, 1, 11, 12, 32.f, 16.f);
					continue;
				}
				if (Nm == TEXT("coop:joinpassword") || Nm == TEXT("coop:join") || Nm == TEXT("coop:leave"))
				{
					// Password then Join under the pad, centred (Cancel alone while connecting, with no Password row); side by side
					// the Password row ran into the rifle on the parchment
					const bool bPw = MenuPage->Items.ContainsByPredicate([](const OutlawsMenu::FItem& It) { return It.Name == TEXT("coop:joinpassword"); });
					const float RowY = !bPw ? 870.f : Nm == TEXT("coop:joinpassword") ? 862.f : 952.f;
					Link(i, MenuPage->Items[i].Label, 815.f, RowY, 1, 11, 12, 40.f, 12.f);
					continue;
				}
				Link(i, MenuPage->Items[i].Label, 815.f + float(i % 3 - 1) * 180.f, 494.f + float(i / 3) * 92.f, 1, 11, 12, 50.f, 16.f);
			}
			else if (File == TEXT("#coophost") || File == TEXT("#coophostopts") || File == TEXT("#cooplevels") || File == TEXT("#cooplobby") || File == TEXT("#coopclient") || File == TEXT("#coopscore")
				|| File == TEXT("#cooppause"))
			{
				// (REQ-COOPLOBBY) the host's pages in a smaller face, the rows spread over the room left above the help line
				// (at most 72 apart, at least 50: the face is about 47 tall).  REQ-MPLOBBY c: the pause Players page too; the
				// lobby's and its player lists are cut to what fits (UOutlawsCoop::FrontRowRoom), a "More players" row under them
				const int32 N = MenuPage->Items.Num();
				const float Step = FMath::Clamp((975.f - (Y + 12.f)) / float(FMath::Max(N - 1, 1)), 50.f, 72.f);
				Link(i, MenuPage->Items[i].Label, 815.f, Y + 12.f + i * Step, 1, 13, 14, 36.f, 2.f);
			}
			else
			{
				Link(i, MenuPage->Items[i].Label, 815.f, Y + 44.f + i * 80.f, 1, 11, 12, 40.f, 12.f);
			}
		}
	}
	else if (File == TEXT("#character"))
	{
		// lobby_character.rml with menu.rcss: Char_Character_Base over the whole stage, the title (menu_title.char: top 32,
		// 60 dp, centred), the six 404 x 456 tiles at their places, each its picture (270 at 69, 82) over its frame; the
		// frame OFF, HOVER pointed at, INUSE the character worn now
		Stage(TEXT("Char_Character_Base"), -320.f, -180.f, 2560.f, 1440.f);
		Txt(SaveFont(8), MenuPage->Title, 960.f, 32.f + 40.f, 1, Col(0x1d, 0x08, 0x00), false, nullptr);
		static const FVector2D Places[6] = { { 285.f, 163.f }, { 761.f, 163.f }, { 1217.f, 163.f }, { 285.f, 595.f }, { 761.f, 595.f }, { 1217.f, 595.f } };
		const FString Worn = FOutlawsMpCharacter::LocalChoice();
		int32 N = 0;
		const FOutlawsMpCharacter* Cs = FOutlawsMpCharacter::All(N);
		for (int32 i = 0; i < N && i < 6 && i < MenuPage->Items.Num(); ++i)
		{
			const FString Short = Cs[i].Short;
			const TCHAR* State = i == Hot ? TEXT("HOVER") : Short.Equals(Worn, ESearchCase::IgnoreCase) ? TEXT("INUSE") : TEXT("OFF");
			const float X = Places[i].X, Y = Places[i].Y;
			Stage(*FString::Printf(TEXT("Char_%s%s@2x"), *Short, State), X, Y, 404.f, 456.f);
			Stage(*FString::Printf(TEXT("Char_%sPicture@2x"), *Short), X + 69.f, Y + 82.f, 270.f, 270.f);
			FrontHits.Emplace(FBox2D(FVector2D(X, Y), FVector2D(X + 404.f, Y + 456.f)), i);
		}
	}
	else if (File == TEXT("#kbd"))
	{
		// the letters keyboard (user 2026-10-02): the start pages' parchment and title, what is typed so far (larger), a note,
		// then the keys as the join page's number pad draws its: four rows of ten, the long keys under them.  Each key's
		// pointing box is its whole cell, so the laser never falls between two
		Stage(TEXT("StartGame_Background"), 3264.f - 2000.f, -180.f, 2000.f, 1440.f);
		Stage(TEXT("StartGame_Base"), 1024.f - 1344.f, -180.f, 1844.f, 1440.f);
		const FLinearColor Ink = Col(0x1d, 0x08, 0x00);
		Txt(SaveFont(0), MenuPage->Title, 815.f, 176.f, 1, Ink, false);
		Txt(SaveFont(9), (KbdText.IsEmpty() ? FString() : KbdText) + TEXT("_"), 815.f, 330.f, 1, Ink, false);
		Txt(SaveFont(5), FString::Printf(TEXT("%d of %d letters%s"), KbdText.Len(), KbdMax, KbdNote.IsEmpty() ? TEXT("") : *(TEXT("    ") + KbdNote)), 815.f, 398.f, 1, Ink, false);
		constexpr float Step = 92.f, Top = 478.f, RowH = 92.f;
		for (int32 i = 0; i < MenuPage->Items.Num(); ++i)
		{
			const FString& Label = MenuPage->Items[i].Label;
			float X = 815.f, Y = 0.f, Half = 46.f;
			if (i < 40)
			{
				X = 880.f + (float(i % 10) - 4.5f) * Step;        // (right of the rifle down the parchment's left)
				Y = Top + float(i / 10) * RowH;
				Half = Step * 0.5f;
			}
			else
			{
				// the long keys: Shift, Space, Delete, Cancel, OK across the bottom, set right a little and closer (user
				// 2026-10-03: Shift sat under the rifle's lever at the parchment's bottom left)
				const int32 k = i - 40;
				X = 935.f + (float(k) - 2.f) * 170.f;
				Y = Top + 4.f * RowH + 18.f;
				Half = 82.f;
			}
			const bool bHot = i == Hot;
			if (bHot) Txt(SaveFont(9), Label, X, Y, 1, Warm, false, &White);
			Txt(SaveFont(8), Label, X, Y, 1, Black, false);
			FrontHits.Emplace(FBox2D(FVector2D(X - Half, Y - RowH * 0.5f), FVector2D(X + Half, Y + RowH * 0.5f)), i);
		}
	}
	else if (File == TEXT("#extras_shelf"))
	{
		// (REQ-EXTRAS) the collectible boxes' shelf on the start pages' parchment: a box outline a story level, three rows of three;
		// a found one filled in with its level's name under it (REQ-COLLECTBOX places them; until then, the outlines and a note)
		Stage(TEXT("StartGame_Background"), 3264.f - 2000.f, -180.f, 2000.f, 1440.f);
		Stage(TEXT("StartGame_Base"), 1024.f - 1344.f, -180.f, 1844.f, 1440.f);
		const FLinearColor Ink = Col(0x1d, 0x08, 0x00), Faded = Col(0x6a, 0x50, 0x38);
		Txt(SaveFont(0), MenuPage->Title, 815.f, 176.f, 1, Ink, false);
		int32 Found = 0;
		for (const OutlawsMenu::FItem& It : MenuPage->Items) Found += It.Classes.Contains(TEXT("disabled")) ? 0 : 1;
		FrontInk += Txt(SaveFont(5), FString::Printf(TEXT("Boxes found: %d of %d"), Found, MenuPage->Items.Num()), 815.f, 372.f, 1, Ink, false);
		auto Rect = [&](float X, float Y, float W, float H, float Edge, const FLinearColor& Colour)
		{
			for (const FBox2D& B : { FBox2D(FVector2D(X, Y), FVector2D(X + W, Y + Edge)), FBox2D(FVector2D(X, Y + H - Edge), FVector2D(X + W, Y + H)),
				FBox2D(FVector2D(X, Y), FVector2D(X + Edge, Y + H)), FBox2D(FVector2D(X + W - Edge, Y), FVector2D(X + W, Y + H)) })
			{
				FCanvasTileItem T(B.Min * K, GWhiteTexture, (B.Max - B.Min) * K, Colour);
				T.BlendMode = SE_BLEND_Translucent;
				C->DrawItem(T);
			}
		};
		for (int32 i = 0; i < MenuPage->Items.Num(); ++i)
		{
			const float Cx = 815.f + float(i % 3 - 1) * 250.f, Cy = 470.f + float(i / 3) * 165.f;
			const bool bFound = !MenuPage->Items[i].Classes.Contains(TEXT("disabled"));
			const FBox2D Box(FVector2D(Cx - 75.f, Cy - 55.f), FVector2D(Cx + 75.f, Cy + 45.f));
			if (bFound)
			{
				FCanvasTileItem T(Box.Min * K, GWhiteTexture, (Box.Max - Box.Min) * K, Col(0x40, 0x10, 0x00));
				T.BlendMode = SE_BLEND_Translucent;
				C->DrawItem(T);
			}
			Rect(Box.Min.X, Box.Min.Y, 150.f, 100.f, i == Hot && bFound ? 6.f : 3.f, i == Hot && bFound ? Warm : bFound ? Ink : Faded);
			const FBox2D Lb = Txt(SaveFont(bFound ? 5 : 2), MenuPage->Items[i].Label, Cx, Cy + 72.f, 1, bFound ? Ink : Faded, false);
			FrontInk += Box;
			FrontInk += Lb;
			while (FrontItemInk.Num() <= i) FrontItemInk.Add(FBox2D(ForceInit));
			FrontItemInk[i] = Lb;
			if (bFound) FrontHits.Emplace(FBox2D(Box.Min, FVector2D(Box.Max.X, Lb.Max.Y)), i);
		}
		FrontInk += Txt(SaveFont(5), OutlawsAchievements::BoxesPlaced ? TEXT("One hidden in each level of the story") : TEXT("Hidden boxes: coming soon"), 815.f, 970.f, 1, Ink, false);
	}
	else
	{
		Stage(TEXT("StartGame_Background"), 3264.f - 2000.f, -180.f, 2000.f, 1440.f);
		Stage(TEXT("StartGame_Base"), 1024.f - 1344.f, -180.f, 1844.f, 1440.f);
		Txt(SaveFont(0), MenuPage->Title, 815.f, 176.f, 1, Col(0x1d, 0x08, 0x00), false);
		// menu_items.start: menu_link.main links centred on x 815 from y 350; (REQ-HMCAREER) more than five rows (Historical
		// Missions with Resume Marshal Training) closer together, the last row's middle at 930, its letters clear of the help line
		const int32 N = MenuPage->Items.Num();
		const float Step = N > 1 ? FMath::Min(118.f, (930.f - 413.f) / float(N - 1)) : 118.f;
		for (int32 i = 0; i < N; ++i)
		{
			Link(i, MenuPage->Items[i].Label, 815.f, 350.f + 18.f + 45.f + i * Step, 1, 11, 12, 40.f, 14.f);
		}
	}
	// the help line: what selects, what goes back
	const FLinearColor Help = Col(0xc0, 0xc0, 0xc0);
	// centred under the page's buttons (the main menu's column sits on x 828, the start pages' on 815, the
	// difficulty column on about 700; user 2026-09-27)
	const float HelpX = File == TEXT("#front") ? 828.f : File.StartsWith(TEXT("#difficulty:")) ? 700.f : 815.f;
	// (the character page's tiles reach down to y 1051: its help goes under them, its toast under the title)
	const bool bCharPage = MenuPage && MenuPage->File == TEXT("#character");
	const FString HelpLine = MenuStack.Num() > 0 ? TEXT("Point and pull the trigger to select        B / Y  Back") : TEXT("Point and pull the trigger to select");
	// (REQ-BUTTONGLYPHS: in VR on a plate, the controllers under the words; BUG-MENUGLYPH2: inside the sheet at its bottom right)
	WideHelpInk = FBox2D(ForceInit);
	if (bVR) UpdateMenuHelpPlate(HelpLine);
	else
	{
		// (BUG-MENUGLYPH, BUG-MENUGLYPH2, user 18 Oct) the flat screen's line at the bottom right of the clean parchment: its right
		// end MenuHelpInsetCm in from where the parchment ends (the torn edge's burn; it ran over the torn edge at the sheet's
		// right edge); the character page's under its tiles' names
		float XL = 0.f, YL = 0.f;
		if (SaveFont(5)) C->TextSize(SaveFont(5), HelpLine, XL, YL);
		const float Right = WideParchmentRight() - MenuHelpInsetCm * 1920.f / SheetW;
		WideHelpInk = Txt(SaveFont(5), HelpLine, Right - XL / K * 0.5f, bCharPage ? 1050.f : 1030.f, 1, Help, true);
	}
	if (MenuToast && !MenuToast->Text.IsEmpty()) Txt(SaveFont(2), MenuToast->Text.ToString(), HelpX, bCharPage ? 128.f : 990.f, 1, Col(0xf2, 0xaa, 0x44), true);
	// which build this is, small under the help line on the main menu (for the testers' reports)
	// (user 14 Oct: the public release is v0.9, a beta) the main menu says so before its build; Line() itself stays as it was for
	// the co-op build comparison, the beacon and the logs
	// (BUG-MENUGLYPH2: centred under the flat screen's line again, as before it moved; in VR where it was)
	if (File == TEXT("#front")) Txt(SaveFont(3), FString::Printf(TEXT("v0.95  build %s"), OutlawsBuildInfo::Commit()), WideHelpInk.bIsValid ? float(WideHelpInk.GetCenter().X) : HelpX, 1062.f, 1, Help, true);
}

// ---- the letters keyboard (user 2026-10-02) ------------------------------------------------------------------------------

void AOutlawsPlayer::OpenKeyboard(const FString& Purpose, const FString& Title, const FString& Initial, int32 MaxLen)
{
	KbdPurpose = Purpose;
	KbdTitle = Title;
	KbdMax = FMath::Max(1, MaxLen);
	KbdText = Initial.Left(KbdMax);
	KbdNote = Purpose == TEXT("mp_name") ? TEXT("(none typed: no name over your head)") : Purpose == TEXT("rando_seed") ? TEXT("(none typed: a new seed at each New Game)") : TEXT("");
	bKbdLower = false;
	ShowMenuPage(TEXT("#kbd"), true);
}

void AOutlawsPlayer::BuildKeyboardPage(OutlawsMenu::FPage& Page) const
{
	// forty keys in four rows of ten, then the long keys (DrawFrontCanvas lays them out by their place in the list)
	using namespace OutlawsMenu;
	Page.File = TEXT("#kbd");
	Page.Title = KbdTitle.IsEmpty() ? FString(TEXT("TYPE")) : KbdTitle;
	auto Key = [&Page](const FString& Label, const FString& Name)
	{
		FItem I;
		I.Kind = EItemKind::Action;
		I.Label = Label;
		I.Name = TEXT("kbd:") + Name;
		Page.Items.Add(I);
	};
	for (const TCHAR* Row : { TEXT("1234567890"), TEXT("QWERTYUIOP"), TEXT("ASDFGHJKL'"), TEXT("ZXCVBNM.-_") })
	{
		for (const TCHAR* C = Row; *C; ++C)
		{
			const FString Ch(1, C);
			Key(bKbdLower ? Ch.ToLower() : Ch, TEXT("key:") + Ch);
		}
	}
	Key(bKbdLower ? TEXT("ABC") : TEXT("abc"), TEXT("shift"));
	Key(TEXT("Space"), TEXT("space"));
	Key(TEXT("Delete"), TEXT("del"));
	Key(TEXT("Cancel"), TEXT("cancel"));
	Key(TEXT("OK"), TEXT("ok"));
}

void AOutlawsPlayer::KeyboardKey(const FString& Key)
{
	if (Key == TEXT("ok"))
	{
		KeyboardDone();
		return;
	}
	if (Key == TEXT("cancel"))
	{
		MenuBack();
		return;
	}
	if (Key == TEXT("shift"))
	{
		bKbdLower = !bKbdLower;
		ShowMenuPage(TEXT("#kbd"), false);
		return;
	}
	if (Key == TEXT("del"))
	{
		KbdText.LeftChopInline(1);
	}
	else
	{
		const FString Ch = Key == TEXT("space") ? FString(TEXT(" ")) : Key.StartsWith(TEXT("key:")) ? Key.Mid(4, 1) : FString();
		if (Ch.IsEmpty()) return;
		if (KbdText.Len() >= KbdMax)
		{
			MenuToastText(FString::Printf(TEXT("%d letters at most"), KbdMax));
			return;
		}
		KbdText += bKbdLower ? Ch.ToLower() : Ch;
	}
	RedrawMenu();
}

void AOutlawsPlayer::KeyboardDone()
{
	AOutlawsLevelActor* L = Level.Get();
	if (KbdPurpose == TEXT("mp_name") && L)
	{
		// saved at once; a co-op session sends it to the other player by itself (UOutlawsCoop::TickName)
		L->MpPlayerName = AOutlawsLevelActor::CleanMpName(KbdText);
		L->bMpNameArg = false;            // (a name the player typed is theirs to keep, even in a test's run)
		SaveSettings();
		UE_LOG(LogOutlaws, Log, TEXT("OLRT multiplayer: player name set to '%s'"), *L->MpPlayerName);
		MenuBack();
		MenuToastText(L->MpPlayerName.IsEmpty() ? FString(TEXT("No name: the other player sees your health bar alone")) : FString::Printf(TEXT("Name saved: %s"), *L->MpPlayerName));
		return;
	}
	if (KbdPurpose == TEXT("rando_seed") && L)
	{
		// (R10) the seed the next New Game takes: a number as typed, any other text by its hash; none, a new one each game
		L->RandoSeedText = KbdText.TrimStartAndEnd().Left(AOutlawsLevelActor::RandoSeedMax);
		SaveSettings();
		UE_LOG(LogOutlaws, Log, TEXT("OLRANDO page: seed set to '%s' (%u)"), *L->RandoSeedText, L->RandoSeedText.IsEmpty() ? 0u : AOutlawsLevelActor::RandoSeedFromText(L->RandoSeedText));
		MenuBack();
		MenuToastText(L->RandoSeedText.IsEmpty() ? FString(TEXT("A new seed at each New Game")) : FString::Printf(TEXT("Seed saved: %s"), *L->RandoSeedText));
		return;
	}
	if (MpHostKeyboardDone(KbdPurpose, KbdText)) return;              // (M1: the Host Game page's password)
	if (UOutlawsCoop::PasswordTyped(this, KbdPurpose, KbdText)) return;     // (M1: the co-op lobby's and the join page's)
	MenuBack();
}

#if WITH_OUTLAWS_TESTS
void AOutlawsPlayer::MpMenuShotTick()
{
	// the title menu, then Options > Multiplayer (photographed), Voice Chat (REQ-VOICEPAGE; photographed), its voice rows
	// pressed and put back, the microphone switched and the meter watched, Back (the Voice Chat row lit), the name row opening
	// the keyboard, a name typed on its keys (Shift for small letters among them; photographed), OK; what the settings file
	// then holds
	auto Shot = [](const TCHAR* Name)
	{
		const FString Path = FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(Name) + TEXT(".png");
		FScreenshotRequest::RequestScreenshot(Path, false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mp menu: screenshot %s"), *Path);
	};
	auto Press = [this](const FString& Name)
	{
		if (!MenuPage) return false;
		const int32 i = MenuPage->Items.IndexOfByPredicate([&Name](const OutlawsMenu::FItem& It) { return It.Name == Name; });
		if (i == INDEX_NONE)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mp menu: no %s on %s"), *Name, *MenuPage->File);
			return false;
		}
		MenuActivate(1, i);
		return true;
	};
	auto Rows = [this]()
	{
		TArray<FString> Out;
		if (MenuPage) for (int32 i = 0; i < MenuPage->Items.Num(); ++i) Out.Add(MenuRowText(i));
		return FString::Join(Out, TEXT(" | "));
	};
	auto File = []()
	{
		const TMap<FString, FString> S = AOutlawsLevelActor::ReadSettings();
		return FString::Printf(TEXT("MpMicrophone=%s MpPlayerName='%s'"), S.Contains(TEXT("MpMicrophone")) ? *S[TEXT("MpMicrophone")] : TEXT("(absent)"),
			S.Contains(TEXT("MpPlayerName")) ? *S[TEXT("MpPlayerName")] : TEXT("(absent)"));
	};
	if (ShotStep == 0 && ShotTime > 4.f && bMenuOpen && MenuPage)
	{
		ShowMenuPage(TEXT("options.rml"), true);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mp menu: options rows: %s"), *Rows());
		const int32 Mp = MenuPage->Items.IndexOfByPredicate([](const OutlawsMenu::FItem& It) { return It.Target == TEXT("#mp"); });
		if (Mp != INDEX_NONE) MenuActivate(1, Mp);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mp menu: page %s rows: %s; file before: %s"), MenuPage ? *MenuPage->File : TEXT("?"), *Rows(), *File());
		++ShotStep;
		ShotTime = 4.f;                   // (the timeline from here: the menu may have come up late, after the opening movie)
	}
	else if (ShotStep == 1 && ShotTime > 5.5f) { Shot(TEXT("ol_mp_options")); ++ShotStep; }
	else if (ShotStep == 2 && ShotTime > 6.f)
	{
		// (REQ-VOICEPAGE) the Voice Chat row opens its page
		const int32 Vc = MenuPage ? MenuPage->Items.IndexOfByPredicate([](const OutlawsMenu::FItem& It) { return It.Target == TEXT("#voice"); }) : INDEX_NONE;
		if (Vc != INDEX_NONE) MenuActivate(1, Vc);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mp menu: page %s rows: %s; file before: %s"), MenuPage ? *MenuPage->File : TEXT("?"), *Rows(), *File());
		++ShotStep;
	}
	else if (ShotStep == 3 && ShotTime > 6.8f) { Shot(TEXT("ol_mp_voice")); ++ShotStep; }
	else if (ShotStep == 4 && ShotTime > 7.4f)
	{
		{
			// (REQ-VOICEVOL) the voice rows: shown, pressed (saved), then round again to what they were
			auto Voice = []()
			{
				const TMap<FString, FString> S = AOutlawsLevelActor::ReadSettings();
				return FString::Printf(TEXT("MpPartnerVolume=%s MpVoiceFade=%s"), S.Contains(TEXT("MpPartnerVolume")) ? *S[TEXT("MpPartnerVolume")] : TEXT("(absent)"),
					S.Contains(TEXT("MpVoiceFade")) ? *S[TEXT("MpVoiceFade")] : TEXT("(absent)"));
			};
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mp menu: voice rows before: %s; file: %s"), *Rows(), *Voice());
			Press(TEXT("mp_pvol"));
			Press(TEXT("mp_vfade"));
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mp menu: voice rows pressed: %s; file: %s"), *Rows(), *Voice());
			for (int32 k = 0; k < 5; ++k) Press(TEXT("mp_pvol"));
			Press(TEXT("mp_vfade"));
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mp menu: voice rows back: %s; file: %s"), *Rows(), *Voice());
		}
		Press(TEXT("mp_mic"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mp menu: microphone pressed: page %s rows: %s; file: %s"), MenuPage ? *MenuPage->File : TEXT("?"), *Rows(), *File());
		++ShotStep;
	}
	else if (ShotStep == 5 && ShotTime > 8.2f)
	{
		// (R8, REQ-VOICEPAGE) the meter on the Voice Chat page: the microphone open for it, the row redrawn ten times a second
		const int32 Mi = MenuPage ? MenuPage->Items.IndexOfByPredicate([](const OutlawsMenu::FItem& It) { return It.Name == TEXT("mp_meter"); }) : INDEX_NONE;
		const int32 RowAt = Mi - MenuScroll + (MenuPage && !MenuPage->Message.IsEmpty() ? 4 : 0);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mp menu: meter on page %s: microphone open for it %s, row '%s', drawn '%s'"), MenuPage ? *MenuPage->File : TEXT("?"),
			UOutlawsCoop::MicMeterOpen(this) ? TEXT("yes") : TEXT("no"), Mi != INDEX_NONE ? *MenuRowText(Mi) : TEXT("(none)"),
			MenuRows.IsValidIndex(RowAt) && MenuRows[RowAt] ? *MenuRows[RowAt]->Text.ToString() : TEXT("(not drawn)"));
		Shot(TEXT("ol_mp_voice_mic"));
		++ShotStep;
	}
	else if (ShotStep == 6 && ShotTime > 9.f)
	{
		// Back (the page's own Back row) to the Multiplayer page, the Voice Chat row lit; the meter's microphone closed
		Press(TEXT("voice_back"));
		const int32 Vc = MenuPage ? MenuPage->Items.IndexOfByPredicate([](const OutlawsMenu::FItem& It) { return It.Target == TEXT("#voice"); }) : INDEX_NONE;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mp menu: back: page %s, lit row %d (Voice Chat row %d), rows: %s"), MenuPage ? *MenuPage->File : TEXT("?"),
			MenuHover[1], Vc, *Rows());
		++ShotStep;
	}
	else if (ShotStep == 7 && ShotTime > 9.6f)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mp menu: after back: page %s, lit row %d, meter's microphone open %s"), MenuPage ? *MenuPage->File : TEXT("?"), MenuHover[1],
			UOutlawsCoop::MicMeterOpen(this) ? TEXT("yes") : TEXT("no"));
		Shot(TEXT("ol_mp_back"));
		++ShotStep;
	}
	else if (ShotStep == 8 && ShotTime > 10.2f)
	{
		Press(TEXT("mp_name"));
		for (const TCHAR* K : { TEXT("kbd:key:T"), TEXT("kbd:shift"), TEXT("kbd:key:E"), TEXT("kbd:key:X"), TEXT("kbd:space"), TEXT("kbd:shift"), TEXT("kbd:key:J"),
			TEXT("kbd:key:_"), TEXT("kbd:key:7"), TEXT("kbd:key:Q"), TEXT("kbd:del") })
		{
			Press(K);
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mp menu: keyboard page %s, %d keys, typed '%s'"), MenuPage ? *MenuPage->File : TEXT("?"), MenuPage ? MenuPage->Items.Num() : 0, *KbdText);
		++ShotStep;
	}
	else if (ShotStep == 9 && ShotTime > 11.f) { Shot(TEXT("ol_mp_keyboard")); ++ShotStep; }
	else if (ShotStep == 10 && ShotTime > 11.8f)
	{
		Press(TEXT("kbd:ok"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT mp menu: after OK: page %s rows: %s; file: %s"), MenuPage ? *MenuPage->File : TEXT("?"), *Rows(), *File());
		++ShotStep;
	}
	else if (ShotStep == 11 && ShotTime > 12.8f) { FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; }
}

OL_SHOT_MATCH(OLMpMenuShot) { return OLFlags::Param(TEXT("OLMpMenuShot")); }
OL_SHOT_TEST(OLMpMenuShot, 790)
{
	MpMenuShotTick();                // (the Multiplayer page and its keyboard: OutlawsPlayerMenu.cpp)
}

void AOutlawsPlayer::GfxMenuShotTick()
{
	// the title menu, then Options > Graphics (photographed); each performance row pressed once with the knob it drives
	// logged before and after (photographed, the HUD on); Restore defaults (photographed); what the settings file holds
	auto Shot = [](const TCHAR* Name)
	{
		const FString Path = FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(Name) + TEXT(".png");
		FScreenshotRequest::RequestScreenshot(Path, false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT gfx menu: screenshot %s"), *Path);
	};
	auto Rows = [this]()
	{
		TArray<FString> Out;
		if (MenuPage) for (int32 i = 0; i < MenuPage->Items.Num(); ++i) Out.Add(MenuRowText(i));
		return FString::Join(Out, TEXT(" | "));
	};
	auto Knobs = [this]()
	{
		const IConsoleVariable* Pd = IConsoleManager::Get().FindConsoleVariable(TEXT("vr.PixelDensity"));
		const IConsoleVariable* Fps = IConsoleManager::Get().FindConsoleVariable(TEXT("ol.perf.TargetFps"));
		const AOutlawsLevelActor* L = Level.Get();
		return FString::Printf(TEXT("profile %s, vr.PixelDensity %.2f, refresh asked %.0f (ol.perf.TargetFps %.0f), lamp slots %d, perf HUD %s (text %s), HD textures %s (chosen %d), PC window %s (capture %s)"),
			OutlawsDeviceProfile::IsQuest2() ? TEXT("Quest2") : TEXT("default"), Pd ? Pd->GetFloat() : -1.f, OutlawsDeviceProfile::RefreshRate(), Fps ? Fps->GetFloat() : -1.f, OutlawsDeviceProfile::LampLights(),
			OutlawsDeviceProfile::PerfHud() ? TEXT("on") : TEXT("off"), PerfHudText && PerfHudText->IsVisible() ? TEXT("shown") : TEXT("hidden"),
			L && L->bHDTextures ? TEXT("on") : TEXT("off"), L && L->bHDChosen ? 1 : 0,
			OutlawsDeviceProfile::PcWindowViewName(OutlawsDeviceProfile::PcWindowView()), SpecCapture ? TEXT("made") : TEXT("none"));
	};
	auto File = []()
	{
		const TMap<FString, FString> S = AOutlawsLevelActor::ReadSettings();
		TArray<FString> Out;
		for (const TCHAR* K : { TEXT("RenderScale"), TEXT("RefreshRate"), TEXT("LampLights"), TEXT("PerfHud"), TEXT("RemasteredGraphics"), TEXT("RemasteredGraphicsChosen"), TEXT("PcWindowView"), TEXT("Quest2Mode") })
		{
			Out.Add(FString::Printf(TEXT("%s=%s"), K, S.Contains(K) ? *S[K] : TEXT("(absent)")));
		}
		return FString::Join(Out, TEXT(" "));
	};
	auto Press = [this, &Knobs, &Rows](const FString& Name)
	{
		const int32 i = MenuPage ? MenuPage->Items.IndexOfByPredicate([&Name](const OutlawsMenu::FItem& It) { return It.Name == Name; }) : INDEX_NONE;
		if (i == INDEX_NONE)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT gfx menu: no %s on %s"), *Name, MenuPage ? *MenuPage->File : TEXT("?"));
			return;
		}
		const FString Before = Knobs();
		const FString RowBefore = MenuRowText(i);
		MenuActivate(1, i);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT gfx menu: pressed '%s' -> '%s'\n    before: %s\n    after:  %s"), *RowBefore, MenuPage && MenuPage->Items.IsValidIndex(i) ? *MenuRowText(i) : TEXT("?"), *Before, *Knobs());
	};
	if (ShotStep == 0 && ShotTime > 4.f && bMenuOpen && MenuPage)
	{
		ShowMenuPage(TEXT("options.rml"), true);
		const int32 Gfx = MenuPage ? MenuPage->Items.IndexOfByPredicate([](const OutlawsMenu::FItem& It) { return It.Target == TEXT("options_display.rml"); }) : INDEX_NONE;
		if (Gfx != INDEX_NONE) MenuActivate(1, Gfx);
		else ShowMenuPage(TEXT("options_display.rml"), true);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT gfx menu: page %s (%s) rows: %s\n    knobs: %s\n    file: %s"), MenuPage ? *MenuPage->File : TEXT("?"),
			OutlawsDeviceProfile::IsQuest2() ? TEXT("Quest2 profile") : TEXT("default profile"), *Rows(), *Knobs(), *File());
		++ShotStep;
		ShotTime = 4.f;                   // (the timeline from here: the menu may have come up late, after the opening movie)
	}
	else if (ShotStep == 1 && ShotTime > 5.5f) { Shot(TEXT("ol_gfx_page")); ++ShotStep; }
	else if (ShotStep == 2 && ShotTime > 6.5f)
	{
		// (QSPEED-5: Quest 2 Mode first, the profile's values in the knobs before the other rows choose their own)
		for (const TCHAR* Name : { TEXT("r_quest2mode"), TEXT("r_resolution"), TEXT("r_refresh"), TEXT("r_lamps"), TEXT("r_perfhud"), TEXT("r_remasteredmode"), TEXT("r_pcview") }) Press(Name);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT gfx menu: after the presses: rows: %s\n    file: %s"), *Rows(), *File());
		++ShotStep;
	}
	else if (ShotStep == 3 && ShotTime > 9.f) { Shot(TEXT("ol_gfx_page_changed")); ++ShotStep; }
	else if (ShotStep == 4 && ShotTime > 10.f)
	{
		Press(TEXT("defaults:display"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT gfx menu: after Restore defaults: rows: %s\n    file: %s"), *Rows(), *File());
		++ShotStep;
	}
	else if (ShotStep == 5 && ShotTime > 11.5f) { Shot(TEXT("ol_gfx_page_defaults")); ++ShotStep; }
	else if (ShotStep == 6 && ShotTime > 12.5f) { FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; }
}

OL_SHOT_MATCH(OLGfxMenuShot) { return OLFlags::Param(TEXT("OLGfxMenuShot")); }
OL_SHOT_TEST(OLGfxMenuShot, 810)
{
	GfxMenuShotTick();               // (the Graphics page's performance rows: OutlawsPlayerMenu.cpp)
}

void AOutlawsPlayer::GammaTestTick()
{
	// REQ-GAMMA: the view photographed at 1.0 first (the look as before), the Brightness row pressed three times (1.3: the
	// grade, the headset's QHurt.w, the file), the file read back over a 1.0 in memory, Restore defaults (1.0 everywhere);
	// then the view at 0.7, 1.0 and 1.6 with the menu closed (the gate measures their mean luminance)
	static int32 Fails = 0;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT gamma check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (!bOk) ++Fails;
	};
	auto Shot = [](const TCHAR* Name)
	{
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(Name) + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT gamma: screenshot %s at %.1f"), Name, OutlawsDeviceProfile::Gamma());
	};
	auto FileGamma = []()
	{
		const TMap<FString, FString> S = AOutlawsLevelActor::ReadSettings();
		return S.Contains(TEXT("Gamma")) ? S[TEXT("Gamma")] : FString(TEXT("(absent)"));
	};
	auto QW = [this]()
	{
		FLinearColor Q(0.f, 0.f, 0.f, -9.f);
		if (UMaterialParameterCollectionInstance* M = ToneMpc && GetWorld() ? GetWorld()->GetParameterCollectionInstance(ToneMpc) : nullptr) M->GetVectorParameterValue(TEXT("QHurt"), Q);
		return Q.A;
	};
	auto State = [this, &QW, &FileGamma]()
	{
		const FPostProcessSettings& PP = Camera->PostProcessSettings;
		return FString::Printf(TEXT("gamma %.2f, grade ColorGamma override %d w %.2f, headset QHurt.w %.2f, file Gamma=%s"), OutlawsDeviceProfile::Gamma(),
			PP.bOverride_ColorGamma ? 1 : 0, PP.ColorGamma.W, QW(), *FileGamma());
	};
	auto Agree = [this, &QW](float G)
	{
		const FPostProcessSettings& PP = Camera->PostProcessSettings;
		return FMath::IsNearlyEqual(OutlawsDeviceProfile::Gamma(), G, 0.001f) && (PP.bOverride_ColorGamma == (G != 1.f)) && FMath::IsNearlyEqual(float(PP.ColorGamma.W), G, 0.001f)
			&& FMath::IsNearlyEqual(QW(), G - 1.f, 0.001f);
	};
	auto Row = [this]()
	{
		const int32 i = MenuPage ? MenuPage->Items.IndexOfByPredicate([](const OutlawsMenu::FItem& It) { return It.Name == TEXT("r_gamma"); }) : INDEX_NONE;
		return i;
	};
	auto Finish = [this]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT gamma RESULT %s (%d failed)"), Fails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Fails);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	};
	AOutlawsLevelActor* L = Level.Get();
	if (!L) return;
	// level, looking the way the start faces (the held-gun shots' view: the yard, the sky); still, no motion blur
	static float Yaw = -1000.f;
	if (Yaw < -999.f) { Yaw = float(GetActorRotation().Yaw); GEngine->Exec(GetWorld(), TEXT("r.MotionBlurQuality 0")); }
	if (Controller) Controller->SetControlRotation(FRotator(0.0, Yaw, 0.0));
	// (the shaders first: a material still compiling draws black; the timeline starts 2 s after, TestShadersReady)
	static float Quiet = -1.f;
	if (ShotStep == 0)
	{
		if (!TestShadersReady(Quiet, 2.f) || ShotTime < 5.f) return;
		ShotTime = 5.5f;               // (the timeline below from here)
	}
	if (ShotStep == 0 && ShotTime > 5.f)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT gamma: start: %s (%s)"), *State(), GetWorld()->FeatureLevel == ERHIFeatureLevel::ES3_1 ? TEXT("ES3_1, the headset's path") : TEXT("SM5"));
		Check(Agree(1.f), FString::Printf(TEXT("the default is 1.0, the grade untouched: %s"), *State()));
		Shot(TEXT("ol_gamma_base"));
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 6.f)
	{
		OpenMenu(false);
		ShowMenuPage(TEXT("options_display.rml"), true);
		const int32 i = Row();
		Check(i != INDEX_NONE, FString::Printf(TEXT("a Brightness row on %s: '%s'"), MenuPage ? *MenuPage->File : TEXT("?"), i != INDEX_NONE ? *MenuRowText(i) : TEXT("none")));
		if (i == INDEX_NONE) { Finish(); return; }
		Check(MenuRowText(i).EndsWith(TEXT("1.0 (default)")), FString::Printf(TEXT("the row reads '%s'"), *MenuRowText(i)));
		for (int32 k = 0; k < 3; ++k) MenuActivate(1, i);
		++ShotStep;
	}
	else if (ShotStep == 2 && ShotTime > 7.f)
	{
		const int32 i = Row();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT gamma: pressed 3 times: '%s'; %s"), i != INDEX_NONE ? *MenuRowText(i) : TEXT("?"), *State());
		Check(Agree(1.3f) && FileGamma() == TEXT("1.30") && i != INDEX_NONE && MenuRowText(i).EndsWith(TEXT("1.3")), FString::Printf(TEXT("three presses: 1.3 live and saved: %s"), *State()));
		Shot(TEXT("ol_gamma_menu_130"));
		// the round trip: 1.0 in memory, then the settings file read again
		OutlawsDeviceProfile::SetGamma(1.f);
		OutlawsDeviceProfile::LoadSettings(AOutlawsLevelActor::ReadSettings());     // (what AOutlawsLevelActor::LoadSettings does with it at start)
		Check(FMath::IsNearlyEqual(OutlawsDeviceProfile::Gamma(), 1.3f, 0.001f), FString::Printf(TEXT("read back from the file: %.2f"), OutlawsDeviceProfile::Gamma()));
		++ShotStep;
	}
	else if (ShotStep == 3 && ShotTime > 8.f)
	{
		Check(Agree(1.3f), FString::Printf(TEXT("after the read back the grade follows: %s"), *State()));
		const int32 d = MenuPage ? MenuPage->Items.IndexOfByPredicate([](const OutlawsMenu::FItem& It) { return It.Name == TEXT("defaults:display"); }) : INDEX_NONE;
		if (d != INDEX_NONE) MenuActivate(1, d);
		++ShotStep;
	}
	else if (ShotStep == 4 && ShotTime > 9.f)
	{
		const int32 i = Row();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT gamma: after Restore defaults: '%s'; %s"), i != INDEX_NONE ? *MenuRowText(i) : TEXT("?"), *State());
		Check(Agree(1.f) && FileGamma() == TEXT("1.00") && i != INDEX_NONE && MenuRowText(i).EndsWith(TEXT("1.0 (default)")), FString::Printf(TEXT("Restore defaults: 1.0 live and saved: %s"), *State()));
		// a full lap of presses comes back round to 1.0 (1.1 .. 1.6, 0.7 .. 1.0: ten)
		for (int32 k = 0; k < 10; ++k) MenuActivate(1, i);
		Check(FMath::IsNearlyEqual(OutlawsDeviceProfile::Gamma(), 1.f, 0.001f) && MenuRowText(i).EndsWith(TEXT("1.0 (default)")), FString::Printf(TEXT("ten presses go round to '%s'"), *MenuRowText(i)));
		CloseMenu();
		OutlawsDeviceProfile::SetGamma(0.7f);       // (the shots below are not saved)
		++ShotStep;
	}
	else if (ShotStep == 5 && ShotTime > 11.f) { Check(Agree(0.7f), State()); Shot(TEXT("ol_gamma_070")); OutlawsDeviceProfile::SetGamma(1.f); ++ShotStep; }
	else if (ShotStep == 6 && ShotTime > 12.5f) { Check(Agree(1.f), State()); Shot(TEXT("ol_gamma_100")); OutlawsDeviceProfile::SetGamma(1.6f); ++ShotStep; }
	else if (ShotStep == 7 && ShotTime > 14.f) { Check(Agree(1.6f), State()); Shot(TEXT("ol_gamma_160")); OutlawsDeviceProfile::SetGamma(1.f); ++ShotStep; }
	else if (ShotStep == 8 && ShotTime > 15.5f) Finish();
}

OL_SHOT_MATCH(OLGammaTest) { return OLFlags::Param(TEXT("OLGammaTest")); }
OL_SHOT_TEST(OLGammaTest, 820)
{
	GammaTestTick();                 // (REQ-GAMMA: OutlawsPlayerMenu.cpp)
}
#endif // WITH_OUTLAWS_TESTS

// ---- REQ-TITLEMENU: the title's Load Game (-OLTitleLoadTest=saves | slot | none) ------------------------------------------

#if WITH_OUTLAWS_TESTS
void AOutlawsPlayer::TitleLoadTestTick()
{
	// the runner puts saves in place (saves, slot) or none: the title menu's rows, Load Game pressable or greyed out, each row's
	// pointer box over its label for each pointing hand (right-handed, left-handed, one controller left or right), then Load
	// Game pressed: the load page in order (the resume point, the other autosaves newest first, the player's own saves) and the
	// resume point (saves) or the first own save (slot) loaded from it.  With none, Load Game cannot be pointed at or pressed
	static int32 Passes = 0, Fails = 0;
	static float NextAt = 0.f, LoadAt = 0.f;
	static FString WantLevel;
	static int32 PickRow = INDEX_NONE;
	if (ShotTime < 0.f) return;
	FString Mode;
	OLFlags::Value(TEXT("OLTitleLoadTest"), Mode);
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLTITLE check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		(bOk ? Passes : Fails)++;
	};
	auto Shot = [&Mode](const TCHAR* Name)
	{
		const FString File = FString::Printf(TEXT("ol_title_%s_%s"), *Mode, Name);
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / File + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLTITLE screenshot %s"), *File);
	};
	auto Finish = [this, &Mode]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLTITLE test %s RESULT %s (%d passed, %d failed)"), *Mode, Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	};
	auto Rows = [this]()
	{
		FString Out;
		if (MenuPage) for (const OutlawsMenu::FItem& I : MenuPage->Items) Out += (Out.IsEmpty() ? TEXT("") : TEXT(" | ")) + I.Label + (I.Classes.Contains(TEXT("disabled")) ? TEXT(" [greyed]") : TEXT(""));
		return Out;
	};
	const bool bSaves = Mode != TEXT("none");
	if (ShotTime < NextAt) return;
	switch (ShotStep)
	{
	case 0:
		if (ShotTime < 4.f || !bMenuOpen || !MenuPage || MenuPage->File != TEXT("#front"))
		{
			if (ShotTime > 120.f) { Check(false, TEXT("the title menu came up within 120 s")); Finish(); }
			return;
		}
		RedrawMenu();
		NextAt = ShotTime + 1.f;
		++ShotStep;
		return;
	case 1:
	{
		const TArray<OutlawsMenu::FItem>& It = MenuPage->Items;
		UE_LOG(LogOutlaws, Display, TEXT("OLTITLE %s: the title menu: %s"), *Mode, *Rows());
		Check(AnySaveToLoad() == bSaves, FString::Printf(TEXT("the runner's saves: %s (wanted %s)"), AnySaveToLoad() ? TEXT("there") : TEXT("none"), bSaves ? TEXT("there") : TEXT("none")));
		FString Labels;
		for (const OutlawsMenu::FItem& I : It) Labels += (Labels.IsEmpty() ? TEXT("") : TEXT(" | ")) + I.Label;
		Check(Labels == TEXT("Load Game | New Game | Extras | Options | Exit"), FString::Printf(TEXT("the rows are Load Game, New Game, Extras, Options, Exit (%s)"), *Labels));
		const bool bBig = It.Num() == 5 && It[0].Classes.Contains(TEXT("big")) && It[1].Classes.Contains(TEXT("big")) && !It[2].Classes.Contains(TEXT("big")) && !It[3].Classes.Contains(TEXT("big"))
			&& !It[4].Classes.Contains(TEXT("big"));
		Check(bBig, TEXT("Load Game and New Game are the big buttons, Extras, Options and Exit the small row"));
		const bool bOld = It.ContainsByPredicate([](const OutlawsMenu::FItem& I) { return I.Action == TEXT("ev_resume_last_save") || I.Label == TEXT("Resume Game") || I.Label == TEXT("Load"); });
		Check(!bOld, TEXT("no Resume Game and no separate Load row"));
		const bool bGrey = It.Num() > 0 && It[0].Classes.Contains(TEXT("disabled"));
		Check(bGrey == !bSaves, FString::Printf(TEXT("Load Game is %s (%s)"), bGrey ? TEXT("greyed out") : TEXT("pressable"), bSaves ? TEXT("saves there") : TEXT("no saves")));
		Shot(TEXT("title"));
		// where each label drew, and the pointer's boxes over them, for each pointing hand
		const bool bInk = FrontItemInk.Num() == 5 && !FrontItemInk.ContainsByPredicate([](const FBox2D& B) { return !B.bIsValid; });
		Check(bInk, FString::Printf(TEXT("all five labels drawn (%d)"), FrontItemInk.Num()));
		if (bInk)
		{
			const FBox2D* B = FrontItemInk.GetData();
			const bool bStack = B[0].Max.Y <= B[1].Min.Y && B[1].Max.Y <= B[2].Min.Y && FMath::Abs(B[2].GetCenter().Y - B[3].GetCenter().Y) < 2.f && B[2].Max.X <= B[3].Min.X
				&& FMath::Abs(B[3].GetCenter().Y - B[4].GetCenter().Y) < 2.f && B[3].Max.X <= B[4].Min.X;
			Check(bStack, FString::Printf(TEXT("Load Game above New Game, Extras, Options and Exit side by side under them (%s / %s / %s / %s / %s)"), *B[0].ToString(), *B[1].ToString(), *B[2].ToString(), *B[3].ToString(), *B[4].ToString()));
			const EOutlawsHand DomWas = DominantHand;
			const int32 OneWas = OneController;
			struct FCase { const TCHAR* Name; EOutlawsHand Dom; int32 One; int32 Hand; };
			const FCase HandCases[] = { { TEXT("right-handed"), EOutlawsHand::Right, 0, 1 }, { TEXT("left-handed"), EOutlawsHand::Left, 0, 0 },
				{ TEXT("one controller, left"), EOutlawsHand::Right, 1, 0 }, { TEXT("one controller, right"), EOutlawsHand::Left, 2, 1 } };
			for (const FCase& Cs : HandCases)
			{
				DominantHand = Cs.Dom;
				OneController = Cs.One;
				FString Got;
				bool bOk = MenuPointerHand() == Cs.Hand;
				for (int32 i = 0; i < It.Num(); ++i)
				{
					const int32 Want = It[i].Classes.Contains(TEXT("disabled")) ? -1 : i;
					const int32 Hit = FrontHitAt(B[i].GetCenter());
					bOk &= Hit == Want;
					Got += FString::Printf(TEXT("%s%s -> %d"), i ? TEXT(", ") : TEXT(""), *It[i].Label, Hit);
				}
				Check(bOk, FString::Printf(TEXT("%s: the %s hand points; each label's middle picks its own row (%s)"), Cs.Name, MenuPointerHand() == 0 ? TEXT("left") : TEXT("right"), *Got));
			}
			DominantHand = DomWas;
			OneController = OneWas;
			// the desktop's and the lasers' geometry: a ray from the eyes to each label's middle on the panel finds that row
			if (Camera && MenuRoot && bWideSheet)
			{
				const FTransform Panel = MenuRoot->GetComponentTransform();
				const FVector Eye = Camera->GetComponentLocation();
				FString Got;
				bool bOk = true;
				for (int32 i = 0; i < It.Num(); ++i)
				{
					const FVector2D C = B[i].GetCenter();
					const FVector At = Panel.TransformPosition(FVector(0.0, (0.5 - C.X / 1920.0) * SheetW, SheetH * 0.5 - C.Y / 1080.0 * SheetH));
					float Len = 0.f;
					const int32 Hit = MenuRowUnderRay(Eye, (At - Eye).GetSafeNormal(), Len);
					const int32 Want = It[i].Classes.Contains(TEXT("disabled")) ? -1 : i;
					bOk &= Hit == Want;
					Got += FString::Printf(TEXT("%s%s -> %d"), i ? TEXT(", ") : TEXT(""), *It[i].Label, Hit);
				}
				Check(bOk, FString::Printf(TEXT("desktop: the view's ray to each label finds its row (%s)"), *Got));
			}
			else Check(false, TEXT("desktop: the title is on the wide sheet for the ray check"));
		}
		MenuHover[1] = 0;
		RedrawMenu();
		NextAt = ShotTime + 0.6f;
		++ShotStep;
		return;
	}
	case 2:
		MenuHover[1] = -1;
		RedrawMenu();
		NextAt = ShotTime + 0.6f;
		++ShotStep;
		return;
	case 3:
	{
		const int32 Ld = FrontItemInk.IsValidIndex(0) ? FrontHitAt(FrontItemInk[0].GetCenter()) : INDEX_NONE;
		if (!bSaves)
		{
			Check(Ld == -1, FString::Printf(TEXT("no saves: the pointer finds nothing on Load Game (%d)"), Ld));
			MenuActivate(MenuPointerHand(), 0);
			Check(bMenuOpen && MenuPage && MenuPage->File == TEXT("#front"), FString::Printf(TEXT("no saves: pressing Load Game leaves the title as it is (%s)"), MenuPage ? *MenuPage->File : TEXT("none")));
			Finish();
			return;
		}
		Check(Ld == 0, FString::Printf(TEXT("the pointer on Load Game picks it (%d)"), Ld));
		MenuActivate(MenuPointerHand(), FMath::Max(Ld, 0));
		NextAt = ShotTime + 1.f;
		++ShotStep;
		return;
	}
	case 4:
	{
		Check(MenuPage && MenuPage->File == TEXT("load_game.rml"), FString::Printf(TEXT("Load Game opens the load page (%s)"), MenuPage ? *MenuPage->File : TEXT("none")));
		if (!MenuPage || MenuPage->File != TEXT("load_game.rml")) { Finish(); return; }
		UE_LOG(LogOutlaws, Display, TEXT("OLTITLE %s: the load page: %s"), *Mode, *Rows());
		TArray<int32> Slots;
		for (const OutlawsMenu::FItem& I : MenuPage->Items) Slots.Add(I.Name.StartsWith(TEXT("load:")) ? FCString::Atoi(*I.Name.Mid(5)) : -99);
		FString Order;
		for (int32 s : Slots) Order += FString::Printf(TEXT("%s%d"), Order.IsEmpty() ? TEXT("") : TEXT(" "), s);
		const bool bResumeFirst = AOutlawsLevelActor::HasAutosave() && Slots.Num() > 0 && Slots[0] == -1 && MenuPage->Items[0].Label.StartsWith(TEXT("Resume: "));
		Check(bResumeFirst, FString::Printf(TEXT("the resume point is the first row (%s)"), MenuPage->Items.Num() > 0 ? *MenuPage->Items[0].Label : TEXT("none")));
		int32 FirstOwn = Slots.IndexOfByPredicate([](int32 s) { return s >= 0; });
		bool bAutosBefore = FirstOwn != INDEX_NONE, bNewest = true, bOwnInOrder = true;
		int32 Autos = 0;
		for (int32 k = 0; k < Slots.Num(); ++k)
		{
			if (k > FirstOwn && FirstOwn != INDEX_NONE && Slots[k] < 0) bAutosBefore = false;
			if (Slots[k] < 0 && Slots[k] != -1)
			{
				++Autos;
				if (k > 1 && Slots[k - 1] < -1 && IFileManager::Get().GetTimeStamp(*AOutlawsLevelActor::SavePath(Slots[k - 1])) < IFileManager::Get().GetTimeStamp(*AOutlawsLevelActor::SavePath(Slots[k]))) bNewest = false;
			}
			if (FirstOwn != INDEX_NONE && k >= FirstOwn && Slots[k] != k - FirstOwn) bOwnInOrder = false;
		}
		Check(bAutosBefore && bNewest, FString::Printf(TEXT("the autosaves come first, newest first (%d besides the resume point), then the player's own saves (slots %s)"), Autos, *Order));
		Check(bOwnInOrder && FirstOwn != INDEX_NONE && Slots.Num() - FirstOwn == AOutlawsLevelActor::NumSaveSlots, FString::Printf(TEXT("the player's own saves in slot order, all %d slots"), int32(AOutlawsLevelActor::NumSaveSlots)));
		Check(Autos >= 1 || Mode != TEXT("saves"), FString::Printf(TEXT("the runner's co-op autosave is listed (%d)"), Autos));
		Shot(TEXT("loadpage"));
		// what to load: the resume point, or the first of the player's own saves with something in it
		PickRow = INDEX_NONE;
		int32 Slot = -1;
		if (Mode == TEXT("slot"))
		{
			for (int32 k = FMath::Max(FirstOwn, 0); k < Slots.Num() && PickRow == INDEX_NONE; ++k) if (Slots[k] >= 0 && !AOutlawsLevelActor::ReadSaveInfo(Slots[k]).bEmpty) { PickRow = k; Slot = Slots[k]; }
		}
		else PickRow = Slots.IndexOfByKey(-1);
		WantLevel = PickRow != INDEX_NONE ? AOutlawsLevelActor::ReadSaveInfo(Slot).Level : FString();
		Check(PickRow != INDEX_NONE && !WantLevel.IsEmpty(), FString::Printf(TEXT("a %s to load (row %d, slot %d, %s)"), Mode == TEXT("slot") ? TEXT("save of the player's own") : TEXT("resume point"), PickRow, Slot, *WantLevel));
		if (PickRow == INDEX_NONE) { Finish(); return; }
		MenuHover[1] = PickRow;
		RedrawMenu();
		NextAt = ShotTime + 0.6f;
		++ShotStep;
		return;
	}
	case 5:
		MenuHover[1] = -1;
		MenuActivate(MenuPointerHand(), PickRow);
		Check(!bMenuOpen, FString::Printf(TEXT("picked: it loads at once, no \"progress will be lost\" page at the title (%s)"), bMenuOpen && MenuPage ? *MenuPage->File : TEXT("menu closed")));
		LoadAt = ShotTime;
		++ShotStep;
		return;
	case 6:
	{
		const AOutlawsLevelActor* L = Level.Get();
		if (!L || L->IsFrontEnd() || !L->LevelName.Equals(WantLevel, ESearchCase::IgnoreCase))
		{
			if (ShotTime - LoadAt > 90.f) { Check(false, FString::Printf(TEXT("%s loaded within 90 s (%s)"), *WantLevel, L ? *L->LevelName : TEXT("?"))); Finish(); }
			return;
		}
		NextAt = ShotTime + 5.f;
		++ShotStep;
		return;
	}
	case 7:
	{
		const AOutlawsLevelActor* L = Level.Get();
		Check(L && !L->IsFrontEnd() && L->LevelName.Equals(WantLevel, ESearchCase::IgnoreCase) && !bMenuOpen, FString::Printf(TEXT("loaded from the title: %s is up, the menu closed (%.0f s)"), L ? *L->LevelName : TEXT("?"), ShotTime - LoadAt));
		Shot(TEXT("loaded"));
		NextAt = ShotTime + 1.f;
		++ShotStep;
		return;
	}
	default:
		Finish();
		return;
	}
}

OL_SHOT_MATCH(OLTitleLoadTest) { return OLFlags::HasValue(TEXT("OLTitleLoadTest")); }
OL_SHOT_TEST(OLTitleLoadTest, 840)
{
	TitleLoadTestTick();             // (REQ-TITLEMENU: the title's Load Game, OutlawsPlayerMenu.cpp)
}
#endif // WITH_OUTLAWS_TESTS

// ---- REQ-MPLOBBY (c): every page fits its panel (BUG-MENUOFFPANEL), Resume Game only outside a session (BUG-MPRESUME) ------

bool AOutlawsPlayer::ResumeGameOffered(const UObject* Context)
{
	return AOutlawsLevelActor::HasAutosave() && !UOutlawsCoop::IsActive(Context) && !FOutlawsMpCharacter::bMatchMenu;
}

bool AOutlawsPlayer::AnySaveToLoad()
{
	if (AOutlawsLevelActor::HasAutosave() || !AOutlawsLevelActor::ReadSaveInfo(-2).bEmpty) return true;
	for (int32 s = 0; s < AOutlawsLevelActor::NumSaveSlots; ++s)
	{
		if (!AOutlawsLevelActor::ReadSaveInfo(s).bEmpty) return true;
	}
	return false;
}

bool AOutlawsPlayer::TitleLoadOffered(const UObject* Context)
{
	return AnySaveToLoad() && !UOutlawsCoop::IsActive(Context) && !FOutlawsMpCharacter::bMatchMenu;
}

int32 AOutlawsPlayer::FrontHitAt(const FVector2D& At) const
{
	int32 Row = -1;
	for (const TPair<FBox2D, int32>& Hit : FrontHits)
	{
		if (Hit.Key.IsInside(At)) Row = Hit.Value;
	}
	return Row;
}

bool AOutlawsPlayer::MenuPageFits(FString& Said) const
{
	if (!bMenuOpen || !MenuPage)
	{
		Said = TEXT("no page open");
		return false;
	}
	const FString& File = MenuPage->File;
	if (IsSavePage())
	{
		Said = FString::Printf(TEXT("%s: the save screen's own list (7 slots shown, the rest scroll)"), *File);
		return true;
	}
	if (IsFrontPage())
	{
		const FBox2D B = FrontInk;
		if (File.StartsWith(TEXT("#coop")))
		{
			// (the start pages' parchment: the lines and rows inside it, under the title and above the help line, whose letters
			// start at about y 1015; a full page puts its last row's middle at 975 by design; view units, 1920 x 1080)
			const bool bOk = B.bIsValid && B.Min.X >= 100.f && B.Max.X <= 1530.f && B.Min.Y >= 300.f && B.Max.Y <= 1015.f;
			Said = FString::Printf(TEXT("%s: %d rows, lines and rows x %.0f..%.0f, y %.0f..%.0f (the parchment under the title and above the help line: x 100..1530, y 300..1015)"),
				*File, MenuPage->Items.Num(), B.Min.X, B.Max.X, B.Min.Y, B.Max.Y);
			return bOk;
		}
		if (!B.bIsValid)
		{
			Said = FString::Printf(TEXT("%s: the remaster's own art layout"), *File);
			return true;
		}
		const bool bOk = B.Min.X >= 0.f && B.Max.X <= 1920.f && B.Min.Y >= 0.f && B.Max.Y <= 1080.f;
		Said = FString::Printf(TEXT("%s: x %.0f..%.0f, y %.0f..%.0f (the view: 1920 x 1080)"), *File, B.Min.X, B.Max.X, B.Min.Y, B.Max.Y);
		return bOk;
	}
	// the parchment sheet (1024 x 820 canvas pixels): every row drawn inside it, below the title, clear of the help line and of
	// "more below" (each where it is drawn, left and right)
	const float W = float(SheetCanvasSize.X), H = float(SheetCanvasSize.Y);
	if (W <= 0.f || H <= 0.f)
	{
		Said = FString::Printf(TEXT("%s: not drawn"), *File);
		return false;
	}
	FBox2D Rows(ForceInit);
	FString Bad;
	for (int32 k = 0; k < SheetRowInk.Num(); ++k)
	{
		const FBox2D& R = SheetRowInk[k];
		Rows += R;
		if (R.Min.X < 0.f || R.Max.X > W || R.Min.Y < 0.f || R.Max.Y > H) Bad += FString::Printf(TEXT(" row %d off the sheet (y %.0f..%.0f);"), k, R.Min.Y, R.Max.Y);
		if (SheetTitleInk.bIsValid && R.Intersect(SheetTitleInk)) Bad += FString::Printf(TEXT(" row %d under the title;"), k);
		for (const FBox2D& F : SheetFootInk) if (R.Intersect(F)) Bad += FString::Printf(TEXT(" row %d over the help line (row y %.0f..%.0f x %.0f..%.0f, line y %.0f..%.0f x %.0f..%.0f);"),
			k, R.Min.Y, R.Max.Y, R.Min.X, R.Max.X, F.Min.Y, F.Max.Y, F.Min.X, F.Max.X);
	}
	// (REQ-SUBMENUSPACE) on the taller sheet the title, "more above", the help line and "more below" sit wholly inside it, a
	// margin from every edge (on the pause page's sheet the status line's margin is only reported)
	constexpr float Margin = 12.f;
	float Least = H;
	for (const FBox2D& E : SheetEdgeInk)
	{
		Least = FMath::Min(Least, FMath::Min(FMath::Min(E.Min.X, W - E.Max.X), FMath::Min(E.Min.Y, H - E.Max.Y)));
		if (IsTallSheet() && (E.Min.X < Margin || E.Max.X > W - Margin || E.Min.Y < Margin || E.Max.Y > H - Margin))
			Bad += FString::Printf(TEXT(" a line not inside the sheet with a margin (y %.0f..%.0f x %.0f..%.0f);"), E.Min.Y, E.Max.Y, E.Min.X, E.Max.X);
	}
	const float FootTop = SheetFootInk.Num() > 0 ? SheetFootInk[0].Min.Y : H;
	Said = FString::Printf(TEXT("%s scroll %d: %d of %d rows drawn, y %.0f..%.0f x %.0f..%.0f (sheet %.0f x %.0f, title to %.0f, help line from %.0f; %d lines, least margin %.0f px)%s"), *File, MenuScroll,
		SheetRowInk.Num(), MenuPage->Items.Num(), Rows.Min.Y, Rows.Max.Y, Rows.Min.X, Rows.Max.X, W, H, SheetTitleInk.bIsValid ? SheetTitleInk.Max.Y : 0.f, FootTop, SheetEdgeInk.Num(), Least, *Bad);
	return Bad.IsEmpty() && (SheetRowInk.Num() > 0 || MenuPage->Items.Num() == 0);
}

int32 AOutlawsPlayer::MenuFitWalk(const FString& Root, TFunctionRef<void(bool, const FString&)> Check)
{
	// every page under Root: shown (not pushed), drawn, checked at the top and scrolled to the bottom; its links followed.  The
	// pages are only looked at: nothing on them is pressed
	TArray<FString> Queue = { Root };
	TSet<FString> Seen = { Root };
	TArray<FString> Walked;
	const TArray<FString> Stack = MenuStack;
	while (Queue.Num() > 0)
	{
		const FString File = Queue[0];
		Queue.RemoveAt(0);
		if (!ShowMenuPage(File, false) || !MenuPage)
		{
			Check(false, FString::Printf(TEXT("%s opens"), *File));
			continue;
		}
		// (the help line shows on every page under the root, as when the player goes there)
		MenuStack = Stack;
		if (File != Root) MenuStack.Add(Root);
		Walked.Add(File);
		const bool bSheet = !IsFrontPage() && !IsSavePage();
		const int32 MaxScroll = bSheet ? SheetMaxScroll() : 0;
		for (int32 Sc : { 0, MaxScroll })
		{
			MenuScroll = Sc;
			RefreshMenuRows();               // (redraws the sheet: the ink measured here)
			if (!bSheet) RedrawMenu();
			FString Said;
			const bool bFits = MenuPageFits(Said);
			Check(bFits, FString::Printf(TEXT("%s fits the panel (%s)"), *File, *Said));
			if (bSheet && MenuRoot)
			{
				// (REQ-SUBMENUSPACE) the pointer's hit areas follow the rows: a ray straight at the first and the last row shown that
				// can be pressed finds that row
				const FTransform Pn = MenuRoot->GetComponentTransform();
				const FVector Nrm = Pn.GetUnitAxis(EAxis::X);
				const int32 First = MenuPage->Message.IsEmpty() ? 0 : 4;
				const int32 Fit = SheetRowsFit();
				int32 Lo = -1, Hi = -1;
				for (int32 r = First; r < Fit; ++r)
				{
					const int32 i = MenuScroll + r - First;
					if (!MenuPage->Items.IsValidIndex(i) || MenuPage->Items[i].Kind == OutlawsMenu::EItemKind::Heading) continue;
					if (MenuPage->Items[i].Classes.Contains(TEXT("disabled"))) continue;      // (REQ-EXTRAS: a greyed row is never pointed at)
					if (Lo < 0) Lo = r;
					Hi = r;
				}
				for (const int32 r : { Lo, Hi })
				{
					if (r < 0) continue;
					const FVector At = Pn.TransformPosition(FVector(0.0, 0.0, RowZ(r)));
					float Len = 0.f;
					const int32 Got = MenuRowUnderRay(At + Nrm * 100.0, -Nrm, Len);
					Check(Got == r, FString::Printf(TEXT("%s scroll %d: the pointer at row %d (%s, z %.1f cm) finds row %d"), *File, MenuScroll, r,
						*MenuPage->Items[MenuScroll + r - First].Label, RowZ(r), Got));
				}
			}
			if (MaxScroll == 0) break;
		}
		if (MenuSheet && Camera && MenuRoot && IsTallSheet())
		{
			// (REQ-SUBMENUSPACE) the taller sheet's bounds hold its corners and reach past the eyes, so occlusion culling never drops
			// it behind a near wall (as the scoreboard's, BUG-SCOREOCCLUDE)
			KeepSheetOverWorld(MenuSheet, Camera->GetComponentLocation());
			const FBox B = MenuSheet->Bounds.GetBox();
			const FTransform Pn = MenuRoot->GetComponentTransform();
			bool bCorners = true;
			for (const float Y : { -0.5f, 0.5f }) for (const float Z : { -0.5f, 0.5f }) bCorners &= B.IsInside(Pn.TransformPosition(FVector(0.0, Y * PanelW, Z * PageSheetH())));
			Check(bCorners && B.IsInside(Camera->GetComponentLocation()), FString::Printf(TEXT("%s: the %.0f cm sheet's bounds hold its corners (%d) and the eyes (%d), bounds scale %.1f"),
				*File, PageSheetH(), bCorners ? 1 : 0, B.IsInside(Camera->GetComponentLocation()) ? 1 : 0, MenuSheet->BoundsScale));
		}
		MenuScroll = 0;
		for (const OutlawsMenu::FItem& I : MenuPage->Items)
		{
			if ((I.Kind == OutlawsMenu::EItemKind::Link || I.Kind == OutlawsMenu::EItemKind::Dialog) && !I.Target.IsEmpty() && !Seen.Contains(I.Target))
			{
				Seen.Add(I.Target);
				Queue.Add(I.Target);
			}
		}
	}
	MenuStack = Stack;
	ShowMenuPage(Root, false);
	UE_LOG(LogOutlaws, Log, TEXT("OLFIT walked %d pages from %s: %s"), Walked.Num(), *Root, *FString::Join(Walked, TEXT(", ")));
	return Walked.Num();
}

// ---- -OLPauseTidyMode=<mode> (REQ-PAUSETIDY) ------------------------------------------------------------------------------

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::PauseTidyTestTick(AOutlawsPlayer* P, float Dt)
{
	static float T = 0.f;
	static int32 Step = 0;
	static FString Mode;
	if (!P || Step >= 30) return;
	AOutlawsLevelActor* L = P->Level.Get();
	if (!L || L->IsFrontEnd() || L->IsRebuildPending() || P->bFrontMenu) { T = 0.f; return; }
	if (Mode.IsEmpty() && !OLFlags::Value(TEXT("OLPauseTidyMode"), Mode)) Mode = TEXT("sp");
	T += Dt;
	float At = 20.f;
	OLFlags::Value(TEXT("OLPauseTidyAt"), At);
	auto Rows = [P](const TCHAR* What)
	{
		FString R;
		const int32 N = P->MenuPage ? P->MenuPage->Items.Num() : 0;
		for (int32 i = 0; i < N; ++i) R += (R.IsEmpty() ? TEXT("") : TEXT(" | ")) + P->MenuPage->Items[i].Label;
		const int32 First = P->MenuPage && !P->MenuPage->Message.IsEmpty() ? 4 : 0;
		const bool bScroll = N + First > MenuRowsShown;
		UE_LOG(LogOutlaws, Log, TEXT("OLPAUSE %s %s (%s): %d rows: %s"), *Mode, What, P->MenuPage ? *P->MenuPage->File : TEXT("none"), N, *R);
		UE_LOG(LogOutlaws, Log, TEXT("OLPAUSE check %s: %s %s no scroll (%d of %d rows shown)"), bScroll ? TEXT("FAIL") : TEXT("PASS"), *Mode, What, FMath::Min(N + First, MenuRowsShown), N + First);
	};
	auto Has = [P](const TCHAR* Target) { return P->MenuPage && P->MenuPage->Items.ContainsByPredicate([Target](const OutlawsMenu::FItem& I) { return I.Target == Target; }); };
	auto Has2 = [P](const TCHAR* Name) { return P->MenuPage && P->MenuPage->Items.ContainsByPredicate([Name](const OutlawsMenu::FItem& I) { return I.Name == Name; }); };
	// (REQ-MPLOBBY c's checks: "OLFIT check PASS|FAIL")
	static int32 FitPass = 0, FitFail = 0, MuteWho = -1;
	static FString MoreBefore, FitTag;
	if (FitTag.IsEmpty() && OLFlags::Value(TEXT("OLFitTag"), FitTag)) FitTag += TEXT("_");      // (-OLFitTag=<run>: the pictures' names)
	auto FitCheck = [](bool bOk, const FString& What)
	{
		(bOk ? FitPass : FitFail)++;
		UE_LOG(LogOutlaws, Log, TEXT("OLFIT check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
	};
	static float NextAt = 0.f;
	// (REQ-NOUNSTUCK) the pause page's own rows: no Tools or Unstuck row; Cheats just before Exit once shown, never before;
	// Dev checks just after it with the dev flag only (OutlawsDevChecks::Enabled), never without
	static int32 Turns = 0;
	auto OwnRows = [P](const FString& What)
	{
		const TArray<OutlawsMenu::FItem>* It = P->MenuPage && P->MenuPage->File == TEXT("main_game.rml") ? &P->MenuPage->Items : nullptr;
		auto Find = [It](TFunctionRef<bool(const OutlawsMenu::FItem&)> F) { return It ? It->IndexOfByPredicate(F) : INDEX_NONE; };
		const int32 Tools = Find([](const OutlawsMenu::FItem& I) { return I.Target == TEXT("#tools") || I.Label == TEXT("Tools"); });
		const int32 Unstuck = Find([](const OutlawsMenu::FItem& I) { return I.Name == TEXT("unstuck") || I.Label == TEXT("Unstuck"); });
		const int32 Cheats = Find([](const OutlawsMenu::FItem& I) { return I.Target == TEXT("#cheats"); });
		const int32 Dev = Find([](const OutlawsMenu::FItem& I) { return I.Target == TEXT("#devchecks"); });
		const int32 Last = It ? It->Num() - 1 : INDEX_NONE;
		const bool bExitLast = It && Last >= 0 && (*It)[Last].Label == TEXT("Exit");
		const bool bDevWanted = P->bCheatsShown && OutlawsDevChecks::Enabled();
		auto Say = [&What](bool bOk, const FString& Line) { UE_LOG(LogOutlaws, Log, TEXT("OLPAUSE check %s: %s %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What, *Line); };
		Say(It != nullptr, FString::Printf(TEXT("the pause page is up (%s)"), P->MenuPage ? *P->MenuPage->File : TEXT("none")));
		Say(Tools == INDEX_NONE && Unstuck == INDEX_NONE, FString::Printf(TEXT("no Tools row (%d) and no Unstuck row (%d)"), Tools, Unstuck));
		Say(P->bCheatsShown ? (Cheats != INDEX_NONE && bExitLast && Cheats == Last - (bDevWanted ? 2 : 1)) : Cheats == INDEX_NONE,
			FString::Printf(TEXT("Cheats row %s (at %d of %d rows, Exit last %d; cheats %s)"), Cheats == INDEX_NONE ? TEXT("absent") : TEXT("there"), Cheats, Last + 1, bExitLast ? 1 : 0,
				P->bCheatsShown ? TEXT("shown: wanted just before Exit") : TEXT("hidden: wanted absent")));
		Say(bDevWanted ? (Dev != INDEX_NONE && Dev == Cheats + 1 && Dev == Last - 1) : Dev == INDEX_NONE,
			FString::Printf(TEXT("Dev checks row %s (at %d; dev flag %d, cheats %d: wanted %s)"), Dev == INDEX_NONE ? TEXT("absent") : TEXT("there"), Dev, OutlawsDevChecks::Enabled() ? 1 : 0,
				P->bCheatsShown ? 1 : 0, bDevWanted ? TEXT("just after Cheats") : TEXT("absent")));
	};
	if (Step == 0)
	{
		if (T < At) return;
		// a session's host waits for its joiner to have built the level (60 s at most): one joining later takes a stand-in's
		// place in the player list between the mute row's press and its check
		UOutlawsCoop* C = Mode == TEXT("sp") ? nullptr : UOutlawsCoop::Get(P);
		if (C && C->IsHost() && T < At + 60.f)
		{
			TArray<AOutlawsCoopLink*> Links;
			C->GetLinks(Links);
			if (!Links.ContainsByPredicate([](const AOutlawsCoopLink* K) { return K && !K->bBot && K->PlayerIndex > 0 && K->BuiltSerial > 0; })) return;
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLPAUSE %s: the checks start %.1f s in"), *Mode, T);
		NextAt = T;
	}
	if (T < NextAt) return;
	switch (Step)
	{
	case 0: P->OpenMenu(); NextAt = T + 1.0f; ++Step; return;
	case 1:
	{
		Rows(TEXT("pause page"));
		{
			// REQ-SCOREABOVE: the live table above the pause panel in a match, nothing new in single player or the story co-op
			const UOutlawsCoop* Cm = UOutlawsCoop::Get(P);
			const bool bMatchNow = Cm && Cm->IsMatch(), bUp = Cm && Cm->PausePanel.IsValid();
			UE_LOG(LogOutlaws, Log, TEXT("OLPAUSE check %s: %s the scoreboard panel above the pause panel is %s (%s)"), bUp == bMatchNow ? TEXT("PASS") : TEXT("FAIL"), *Mode, bUp ? TEXT("up") : TEXT("absent"), bMatchNow ? TEXT("a match: wanted up") : TEXT("no match: wanted absent"));
		}
		Shot(FString::Printf(TEXT("ol_pause_%s"), *Mode));
		// Save, Load and Restart Level: single player and the co-op host have them, a co-op joiner and a match not
		const bool bWant = Mode == TEXT("sp") || Mode == TEXT("cohost");
		auto HasI = [P](const TCHAR* Target) { return P->MenuPage && P->MenuPage->Items.ContainsByPredicate([Target](const OutlawsMenu::FItem& I) { return I.Target.Equals(Target, ESearchCase::IgnoreCase); }); };
		const bool bSave = HasI(TEXT("save_game.rml")), bLoad = HasI(TEXT("load_game.rml"));
		const bool bRestart = P->MenuPage && P->MenuPage->Items.ContainsByPredicate([](const OutlawsMenu::FItem& I) { return I.Action == TEXT("ev_restart_level"); });
		UE_LOG(LogOutlaws, Log, TEXT("OLPAUSE check %s: %s save %d load %d restart %d (want %s)"), bSave == bWant && bLoad == bWant && bRestart == bWant ? TEXT("PASS") : TEXT("FAIL"),
			*Mode, bSave ? 1 : 0, bLoad ? 1 : 0, bRestart ? 1 : 0, bWant ? TEXT("all three") : TEXT("none"));
		NextAt = T + 0.8f; ++Step; return;
	}
	case 2:
		if (Has(TEXT("#cooppause"))) { P->ShowMenuPage(TEXT("#cooppause"), true); NextAt = T + 1.0f; ++Step; }
		else { UE_LOG(LogOutlaws, Log, TEXT("OLPAUSE %s: no Players row"), *Mode); Step = 5; }
		return;
	case 3: Rows(TEXT("Players page")); Shot(FString::Printf(TEXT("ol_pause_%s_players"), *Mode)); NextAt = T + 0.8f; ++Step; return;
	case 4:
		P->MenuBack();
		UE_LOG(LogOutlaws, Log, TEXT("OLPAUSE check %s: %s Back from Players to the pause page (%s)"), P->MenuPage && P->MenuPage->File == TEXT("main_game.rml") ? TEXT("PASS") : TEXT("FAIL"), *Mode, P->MenuPage ? *P->MenuPage->File : TEXT("none"));
		NextAt = T + 0.5f; ++Step; return;
	case 5:
		// (REQ-NOUNSTUCK: was the Tools page, steps 5 to 7) the pause page's own rows, the cheats still hidden
		OwnRows(Mode + TEXT(" pause page, cheats hidden:"));
		Step = 8;
		NextAt = T + 0.3f;
		return;
	case 8:
	{
		// REQ-MPLOBBY c (BUG-MPRESUME): no Resume Game in a co-op or match session, the pause page's nor the title menu's (it
		// would load the single-player game).  REQ-TITLEMENU: the title has no Resume Game at all; its Load Game is pressable
		// in single player with a save to load, greyed out in a session
		const bool bSp = Mode == TEXT("sp");
		const bool bPauseResume = P->MenuPage && P->MenuPage->Items.ContainsByPredicate([](const OutlawsMenu::FItem& I) { return I.Action == TEXT("ev_resume_last_save") || I.Label == TEXT("Resume Game"); });
		FitCheck(!bPauseResume, FString::Printf(TEXT("%s: the pause page has no Resume Game row"), *Mode));
		P->ShowMenuPage(TEXT("#front"), false);
		const bool bFrontResume = P->MenuPage && P->MenuPage->Items.ContainsByPredicate([](const OutlawsMenu::FItem& I) { return I.Action == TEXT("ev_resume_last_save") || I.Label == TEXT("Resume Game"); });
		const OutlawsMenu::FItem* Ld = P->MenuPage ? P->MenuPage->Items.FindByPredicate([](const OutlawsMenu::FItem& I) { return I.Name == TEXT("title_load"); }) : nullptr;
		const bool bLoadOn = Ld && !Ld->Classes.Contains(TEXT("disabled"));
		const bool bWant = bSp && AOutlawsPlayer::AnySaveToLoad();
		FitCheck(!bFrontResume, FString::Printf(TEXT("%s: the title menu has no Resume Game row"), *Mode));
		FitCheck(Ld && bLoadOn == bWant, FString::Printf(TEXT("%s: the title menu's Load Game is %s (%s; a save %s)"), *Mode, !Ld ? TEXT("missing") : bLoadOn ? TEXT("pressable") : TEXT("greyed out"),
			bSp ? TEXT("single player: pressable with a save") : TEXT("a session: never pressable"), AOutlawsPlayer::AnySaveToLoad() ? TEXT("there") : TEXT("absent")));
		P->ShowMenuPage(TEXT("main_game.rml"), false);
		NextAt = T + 0.5f; ++Step; return;
	}
	case 9:
	{
		// BUG-MENUOFFPANEL: every page under pause shown, scrolled to its top and its bottom, each checked to sit on its panel
		const int32 N = P->MenuFitWalk(TEXT("main_game.rml"), [&FitCheck](bool bOk, const FString& What) { FitCheck(bOk, Mode + TEXT(": ") + What); });
		FitCheck(N >= 5, FString::Printf(TEXT("%s: %d pages walked under pause"), *Mode, N));
		NextAt = T + 0.5f;
		Step = Mode == TEXT("sp") ? 16 : 10;
		return;
	}
	case 10:
		if (Has(TEXT("#cooppause"))) { P->ShowMenuPage(TEXT("#cooppause"), true); NextAt = T + 1.0f; ++Step; }
		else { FitCheck(false, FString::Printf(TEXT("%s: the pause page has Players"), *Mode)); Step = 16; }
		return;
	case 11:
	{
		// the Players page (8 or 16 players with -OLFakePlayers): photographed, it fits; a mute row for each other player with a
		// microphone, as many as the page holds, "More players" for the rest
		UOutlawsCoop* C = UOutlawsCoop::Get(P);
		Rows(TEXT("Players page (fit)"));
		FString Fit;
		const bool bFits = P->MenuPageFits(Fit);
		FitCheck(bFits, FString::Printf(TEXT("%s: the Players page fits the panel (%s)"), *Mode, *Fit));
		Shot(FString::Printf(TEXT("ol_fit_%s%s_players"), *FitTag, *Mode));
		TArray<UOutlawsCoop::FPlayerRow> Listed;
		if (C) C->PlayerRows(Listed);
		int32 WantMute = 0, Shown = 0, MuteRows = 0;
		for (const OutlawsMenu::FItem& I : P->MenuPage->Items)
		{
			if (I.Name.StartsWith(TEXT("coop:mute:")) || I.Name.StartsWith(TEXT("coop:player:")))
			{
				++Shown;
				const int32 Who = FCString::Atoi(*I.Name.Mid(I.Name.StartsWith(TEXT("coop:mute:")) ? 10 : 12));
				const UOutlawsCoop::FPlayerRow* R = Listed.FindByPredicate([Who](const UOutlawsCoop::FPlayerRow& X) { return X.Who == Who; });
				WantMute += R && R->bMic ? 1 : 0;
				MuteRows += I.Name.StartsWith(TEXT("coop:mute:")) ? 1 : 0;
			}
		}
		const bool bMore = Has2(TEXT("coop:moreplayers"));
		const bool bOld = Has2(TEXT("coop:mute"));
		FitCheck(MuteRows == WantMute && MuteRows > 0 && !bOld, FString::Printf(TEXT("%s: one mute row per player with a microphone (%d of %d players shown, %d mute rows, %d wanted; no Mute partner row %d)"),
			*Mode, Shown, FMath::Max(0, Listed.Num() - 1), MuteRows, WantMute, bOld ? 0 : 1));
		FitCheck(bMore == (Shown < Listed.Num() - 1), FString::Printf(TEXT("%s: More players %s (%d of %d others shown)"), *Mode, bMore ? TEXT("shown") : TEXT("not needed"), Shown, Listed.Num() - 1));
		// press the first mute row
		MuteWho = -1;
		for (const OutlawsMenu::FItem& I : P->MenuPage->Items) if (I.Name.StartsWith(TEXT("coop:mute:"))) { MuteWho = FCString::Atoi(*I.Name.Mid(10)); break; }
		if (MuteWho >= 0) P->CoopPressRow(FString::Printf(TEXT("coop:mute:%d"), MuteWho));
		MoreBefore = P->CoopRowLabel(TEXT("coop:moreplayers"));
		NextAt = T + 1.0f; ++Step; return;
	}
	case 12:
	{
		UOutlawsCoop* C = UOutlawsCoop::Get(P);
		P->ShowMenuPage(TEXT("#cooppause"), false);
		const FString Label = P->CoopRowLabel(FString::Printf(TEXT("coop:mute:%d"), MuteWho));
		FitCheck(C && MuteWho >= 0 && C->IsVoiceMuted(MuteWho) && C->MutedBits == (1u << MuteWho) && Label.EndsWith(TEXT("Muted")),
			FString::Printf(TEXT("%s: the mute row mutes that player only (player index %d, bits 0x%x, row \"%s\")"), *Mode, MuteWho, C ? C->MutedBits : 0u, *Label));
		if (MuteWho >= 0) P->CoopPressRow(FString::Printf(TEXT("coop:mute:%d"), MuteWho));
		NextAt = T + 1.0f; ++Step; return;
	}
	case 13:
	{
		UOutlawsCoop* C = UOutlawsCoop::Get(P);
		P->ShowMenuPage(TEXT("#cooppause"), false);
		const FString Label = P->CoopRowLabel(FString::Printf(TEXT("coop:mute:%d"), MuteWho));
		FitCheck(C && MuteWho >= 0 && !C->IsVoiceMuted(MuteWho) && C->MutedBits == 0u && Label.EndsWith(TEXT("Mute")),
			FString::Printf(TEXT("%s: pressed again, heard again (bits 0x%x, row \"%s\")"), *Mode, C ? C->MutedBits : 0u, *Label));
		if (Has2(TEXT("coop:moreplayers")))
		{
			P->CoopPressRow(TEXT("coop:moreplayers"));
			NextAt = T + 1.0f; ++Step; return;
		}
		Step = 15; NextAt = T + 0.5f; return;
	}
	case 14:
	{
		P->ShowMenuPage(TEXT("#cooppause"), false);
		P->RedrawMenu();
		Rows(TEXT("Players page, More players pressed"));
		FString Fit;
		const FString More = P->CoopRowLabel(TEXT("coop:moreplayers"));
		const bool bFits = P->MenuPageFits(Fit);
		FitCheck(bFits && !More.IsEmpty() && More != MoreBefore && MoreBefore.Contains(TEXT("(1-")), FString::Printf(TEXT("%s: More players shows the next page (%s, from %s), it fits (%s)"), *Mode, *More, *MoreBefore, *Fit));
		Shot(FString::Printf(TEXT("ol_fit_%s%s_players_more"), *FitTag, *Mode));
		if (UOutlawsCoop* C = UOutlawsCoop::Get(P)) C->PlayerListPage = 0;
		NextAt = T + 1.0f; ++Step; return;
	}
	case 15:
		P->MenuBack();
		NextAt = T + 0.5f; ++Step; return;
	case 16:
		// (REQ-NOUNSTUCK) then the cheats shown the player's way (a whole turn with the pause page up) and the page again
		P->MenuStack.Reset();
		P->ShowMenuPage(TEXT("main_game.rml"), false);
		Turns = 0;
		NextAt = T + 0.5f; ++Step; return;
	case 17:
		if (!P->bCheatsShown && Turns < 40)
		{
			if (AController* Cn = P->GetController()) { FRotator R = Cn->GetControlRotation(); R.Yaw += 20.f; Cn->SetControlRotation(R); }
			++Turns;
			NextAt = T + 0.05f;
			return;
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLPAUSE check %s: %s the turn on the spot showed the cheats (%d steps of 20 degrees)"), P->bCheatsShown ? TEXT("PASS") : TEXT("FAIL"), *Mode, Turns);
		if (AController* Cn = P->GetController()) Cn->SetControlRotation(FRotator(0.0, P->MenuYaw, 0.0));     // (back to face the page)
		NextAt = T + 1.0f; ++Step; return;
	case 18:
		// the page as the turn left it (the turn rebuilds it), not shown again by the test
		Rows(TEXT("pause page, cheats shown"));
		OwnRows(Mode + TEXT(" pause page, cheats shown:"));
		Shot(FString::Printf(TEXT("ol_pause_%s_cheats"), *Mode));
		NextAt = T + 0.8f; ++Step; return;
	case 19:
		if (Has(TEXT("#cheats"))) { P->ShowMenuPage(TEXT("#cheats"), true); NextAt = T + 1.0f; ++Step; }
		else { Step = 21; NextAt = T + 0.3f; }
		return;
	case 20:
	{
		// the Cheats page from its pause row, as before (a client of a host with cheats off: its one line), then Back
		FString R;
		for (const OutlawsMenu::FItem& I : P->MenuPage ? P->MenuPage->Items : TArray<OutlawsMenu::FItem>()) R += (R.IsEmpty() ? TEXT("") : TEXT(" | ")) + I.Label;
		UE_LOG(LogOutlaws, Log, TEXT("OLPAUSE %s Cheats page (%s): %d rows: %s"), *Mode, P->MenuPage ? *P->MenuPage->File : TEXT("none"), P->MenuPage ? P->MenuPage->Items.Num() : 0, *R.Left(400));
		UE_LOG(LogOutlaws, Log, TEXT("OLPAUSE check %s: %s the Cheats row opens the Cheats page (%s)"), P->MenuPage && P->MenuPage->File == TEXT("#cheats") ? TEXT("PASS") : TEXT("FAIL"), *Mode, P->MenuPage ? *P->MenuPage->File : TEXT("none"));
		P->MenuBack();
		UE_LOG(LogOutlaws, Log, TEXT("OLPAUSE check %s: %s Back from Cheats to the pause page (%s)"), P->MenuPage && P->MenuPage->File == TEXT("main_game.rml") ? TEXT("PASS") : TEXT("FAIL"), *Mode, P->MenuPage ? *P->MenuPage->File : TEXT("none"));
		NextAt = T + 0.5f; ++Step; return;
	}
	case 21:
		P->CloseMenu();
		UE_LOG(LogOutlaws, Log, TEXT("OLFIT %s done: %d passed, %d failed"), *Mode, FitPass, FitFail);
		UE_LOG(LogOutlaws, Log, TEXT("OLPAUSE %s done"), *Mode);
		Step = 30;
		if (OLFlags::Param(TEXT("OLPauseTidyQuit"))) FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS

// ---- -OLShots -OLTriggerStepTest (REQ-TRIGGERSTEP) ---------------------------------------------------------------------------
// From the pause menu (in a level), in three modes (two controllers, one controller, the desktop's mouse): Options' link to the VR page (an
// action row), Movement speed (a number row: stops at both ends with a tick on the hand that pressed; one controller goes
// round), Movement follows (a named list: round both ways), Y / B reload (on/off: either trigger flips it), Gameplay's Autosave
// every (a number row of choices) and the Host Game page's Minutes (a number row on another page).  The desktop's ray from the
// view finds the row it points at.  What it changed is put back.  Each check "OLSTEP check PASS|FAIL", then RESULT and the exit.

#if WITH_OUTLAWS_TESTS
void AOutlawsPlayer::TriggerStepTestTick()
{
	static bool bDone = false;
	static int32 Passes = 0, Fails = 0;
	AOutlawsLevelActor* L = Level.Get();
	if (bDone || !L || ShotTime < 6.f) return;
	static float OpenedAt = -1.f;
	if (!bMenuOpen) { OpenMenu(false); OpenedAt = ShotTime; return; }
	if (!MenuPage || ShotTime < OpenedAt + 1.f) return;       // (a second for the panel to be placed in front of the eyes)
	bDone = true;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSTEP check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		(bOk ? Passes : Fails)++;
	};
	auto Find = [this](const FString& Name)
	{
		return MenuPage ? MenuPage->Items.IndexOfByPredicate([&Name](const OutlawsMenu::FItem& It) { return It.Name == Name || It.Target == Name; }) : INDEX_NONE;
	};
	auto Show = [this](const FString& File)
	{
		ShowMenuPage(File, true);
		return MenuPage && MenuPage->File == File;
	};
	const float Speed0 = L->GameSpeed, Auto0 = L->AutosaveMinutes;
	const EOutlawsMoveDirection Move0 = MoveDirection;
	const bool bFace0 = bFaceReload;
	const int32 One0 = OneController;
	const float Lo = AOutlawsLevelActor::MinGameSpeed, Hi = AOutlawsLevelActor::MaxGameSpeed;
	static const TCHAR* const ModeName[] = { TEXT("two controllers"), TEXT("one controller"), TEXT("desktop") };
	for (int32 Mode = 0; Mode < 3; ++Mode)
	{
		OneController = Mode == 1 ? 2 : 0;           // (one controller: the right one)
		const FString M = ModeName[Mode];
		// up: the right trigger (one controller: its trigger) or the left button; down: the left trigger or the right button
		auto Up = [this, Mode](int32 i) { if (Mode == 2) MenuMouseStep(i, false); else MenuTriggerPress(1, i); };
		auto Down = [this, Mode](int32 i) { if (Mode == 2) MenuMouseStep(i, true); else MenuTriggerPress(Mode == 1 ? 1 : 0, i); };
		const int32 DownHand = Mode == 0 ? 0 : 1;
		MenuStack.Reset();
		ShowMenuPage(TEXT("main_game.rml"), false);

		// an action row: Options' link to the VR page, pressed with the down button (either trigger presses an action row)
		Check(Show(TEXT("options.rml")), M + TEXT(": Options shown"));
		const int32 Link = Find(TEXT("#vr"));
		if (Link != INDEX_NONE) Down(Link);
		Check(Link != INDEX_NONE && MenuPage && MenuPage->File == TEXT("#vr"), M + FString::Printf(TEXT(": the action row (link to the VR page) pressed with the %s opens it (%s)"),
			Mode == 2 ? TEXT("right button") : Mode == 1 ? TEXT("one trigger") : TEXT("left trigger"), MenuPage ? *MenuPage->File : TEXT("none")));
		if (!MenuPage || MenuPage->File != TEXT("#vr")) Show(TEXT("#vr"));

		// the desktop's ray: from the eyes to Movement speed's row, the row found
		const int32 Sp = Find(TEXT("vr_speed"));
		if (Mode == 2 && Sp != INDEX_NONE && Camera && MenuRoot)
		{
			const int32 First = MenuPage->Message.IsEmpty() ? 0 : 4;
			const int32 R = Sp - MenuScroll + First;
			const FVector At = MenuRoot->GetComponentTransform().TransformPosition(FVector(0.0, 0.0, RowZ(R)));
			const FVector Eye = Camera->GetComponentLocation();
			float Len = 0.f;
			const int32 Hit = MenuRowUnderRay(Eye, (At - Eye).GetSafeNormal(), Len);
			Check(Hit == R, FString::Printf(TEXT("desktop: the view's ray to Movement speed's row finds it (row %d, found %d)"), R, Hit));
		}

		// a number row: Movement speed
		Check(Sp != INDEX_NONE, M + TEXT(": Movement speed row there"));
		if (Sp != INDEX_NONE)
		{
			if (Mode == 1)
			{
				// one controller: up and round again, no tick (as before)
				L->GameSpeed = Hi - 0.05f;
				const int32 T0 = MenuStepTicks[0] + MenuStepTicks[1];
				Up(Sp);
				const float AtTop = L->GameSpeed;
				Up(Sp);
				Check(FMath::IsNearlyEqual(AtTop, Hi, 0.001f) && FMath::IsNearlyEqual(L->GameSpeed, Lo, 0.001f) && MenuStepTicks[0] + MenuStepTicks[1] == T0,
					FString::Printf(TEXT("one controller: Movement speed steps up to the top (%.2f) then round to the bottom (%.2f), no tick"), AtTop, L->GameSpeed));
			}
			else
			{
				L->GameSpeed = 0.5f;
				bool bMonotonic = true;
				float Was = L->GameSpeed;
				const int32 TD0 = MenuStepTicks[DownHand];
				for (int32 k = 0; k < 20; ++k) { Down(Sp); bMonotonic &= L->GameSpeed <= Was + 0.0001f; Was = L->GameSpeed; }
				Check(bMonotonic && FMath::IsNearlyEqual(L->GameSpeed, Lo, 0.001f), M + FString::Printf(TEXT(": Movement speed stepped down 20 times stops at the bottom (%.2f, never went round)"), L->GameSpeed));
				Check(MenuStepTicks[DownHand] > TD0 && bMenuStepHitEnd, M + FString::Printf(TEXT(": a tick on the %s hand at the bottom (%d ticks)"), DownHand == 0 ? TEXT("left") : TEXT("right"), MenuStepTicks[DownHand] - TD0));
				const int32 TU0 = MenuStepTicks[1];
				bMonotonic = true;
				for (int32 k = 0; k < 20; ++k) { Up(Sp); bMonotonic &= L->GameSpeed >= Was - 0.0001f; Was = L->GameSpeed; }
				Check(bMonotonic && FMath::IsNearlyEqual(L->GameSpeed, Hi, 0.001f), M + FString::Printf(TEXT(": Movement speed stepped up 20 times stops at the top (%.2f, never went round)"), L->GameSpeed));
				Check(MenuStepTicks[1] > TU0 && bMenuStepHitEnd, M + FString::Printf(TEXT(": a tick on the right hand at the top (%d ticks)"), MenuStepTicks[1] - TU0));
				Down(Sp);
				Check(!bMenuStepHitEnd && L->GameSpeed < Hi - 0.01f, M + FString::Printf(TEXT(": one step down from the top moves it, no tick (%.2f)"), L->GameSpeed));
			}
		}

		// a named list: Movement follows (Head, Dominant hand, Off hand), round both ways
		const int32 Mf = Find(TEXT("vr_movefollow"));
		Check(Mf != INDEX_NONE, M + TEXT(": Movement follows row there"));
		if (Mf != INDEX_NONE)
		{
			MoveDirection = EOutlawsMoveDirection::Head;
			const int32 T0 = MenuStepTicks[0] + MenuStepTicks[1];
			if (Mode == 1)
			{
				Up(Mf);
				const bool bDom = MoveDirection == EOutlawsMoveDirection::DominantHand;
				Up(Mf); Up(Mf);
				Check(bDom && MoveDirection == EOutlawsMoveDirection::Head, TEXT("one controller: Movement follows steps forward and round past its end to Head"));
			}
			else
			{
				Down(Mf);
				const bool bWrapDown = MoveDirection == EOutlawsMoveDirection::OffHand;
				Up(Mf);
				const bool bBack = MoveDirection == EOutlawsMoveDirection::Head;
				Up(Mf); Up(Mf); Up(Mf);
				Check(bWrapDown, M + TEXT(": Movement follows stepped down from Head goes round to Off hand"));
				Check(bBack && MoveDirection == EOutlawsMoveDirection::Head, M + TEXT(": up from Off hand is Head again, and three more up go round to Head"));
			}
			Check(MenuStepTicks[0] + MenuStepTicks[1] == T0, M + TEXT(": a named list gives no end tick"));
		}

		// an on/off row: Y / B reload, flipped by either trigger (button)
		const int32 Fr = Find(TEXT("vr_facereload"));
		Check(Fr != INDEX_NONE, M + TEXT(": Y / B reload row there"));
		if (Fr != INDEX_NONE)
		{
			const bool B0 = bFaceReload;
			Up(Fr);
			const bool B1 = bFaceReload;
			Down(Fr);
			Check(B1 == !B0 && bFaceReload == B0, M + FString::Printf(TEXT(": Y / B reload flipped by the up press (%d -> %d) and back by the down press (%d)"), B0, B1, bFaceReload));
		}

		// a number row of choices on another page: Autosave every (1 .. 15 minutes)
		if (Show(TEXT("options_gameplay.rml")))
		{
			const int32 Am = Find(TEXT("autosave_minutes"));
			Check(Am != INDEX_NONE, M + TEXT(": Autosave every row there"));
			if (Am != INDEX_NONE && Mode != 1)
			{
				for (int32 k = 0; k < 8; ++k) Down(Am);
				const float Bottom = L->AutosaveMinutes;
				const bool bTickB = bMenuStepHitEnd;
				for (int32 k = 0; k < 8; ++k) Up(Am);
				Check(FMath::IsNearlyEqual(Bottom, 1.f) && bTickB && FMath::IsNearlyEqual(L->AutosaveMinutes, 15.f) && bMenuStepHitEnd,
					M + FString::Printf(TEXT(": Autosave every stops at 1 going down and at 15 going up (%.0f, %.0f), a tick at each"), Bottom, L->AutosaveMinutes));
			}
			else if (Am != INDEX_NONE)
			{
				L->AutosaveMinutes = 15.f;
				Up(Am);
				Check(FMath::IsNearlyEqual(L->AutosaveMinutes, 1.f), FString::Printf(TEXT("one controller: Autosave every goes round from 15 to %.0f"), L->AutosaveMinutes));
			}
		}

		// the Host Game page's Minutes (Unlimited, 5 .. 120): stops at both ends
		if (Show(TEXT("#mphost")))
		{
			const int32 Tm = Find(TEXT("mphost_time"));
			Check(Tm != INDEX_NONE, M + TEXT(": Host Game's Minutes row there"));
			if (Tm != INDEX_NONE && Mode != 1)
			{
				for (int32 k = 0; k < 12; ++k) Down(Tm);
				const FString Bottom = MenuRowText(Tm);
				const bool bTickB = bMenuStepHitEnd;
				for (int32 k = 0; k < 12; ++k) Up(Tm);
				const FString Top = MenuRowText(Tm);
				Check(Bottom.Contains(TEXT("Unlimited")) && bTickB && Top.Contains(TEXT("120")) && bMenuStepHitEnd,
					M + FString::Printf(TEXT(": Host Game's Minutes stops at '%s' going down and '%s' going up, a tick at each"), *Bottom, *Top));
			}
			else if (Tm != INDEX_NONE)
			{
				bool bSawTop = false, bRound = false;
				for (int32 k = 0; k < 12; ++k)
				{
					Up(Tm);
					const FString Now = MenuRowText(Tm);
					if (Now.Contains(TEXT("120"))) bSawTop = true;
					else if (bSawTop && Now.Contains(TEXT("Unlimited"))) bRound = true;
				}
				Check(bSawTop && bRound, TEXT("one controller: Host Game's Minutes goes up to 120 and round to Unlimited"));
			}
		}
	}
	// what the test changed, put back
	OneController = One0;
	L->GameSpeed = Speed0;
	L->AutosaveMinutes = Auto0;
	MoveDirection = Move0;
	bFaceReload = bFace0;
	SaveSettings();
	MenuStack.Reset();
	CloseMenu();
	UE_LOG(LogOutlaws, Display, TEXT("OLSTEP RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
	FGenericPlatformMisc::RequestExit(false);
	ShotTime = -1.f;
}

OL_SHOT_MATCH(OLTriggerStepTest) { return OLFlags::Param(TEXT("OLTriggerStepTest")); }
OL_SHOT_TEST(OLTriggerStepTest, 830)
{
	TriggerStepTestTick();           // (REQ-TRIGGERSTEP: OutlawsPlayerMenu.cpp)
}
#endif // WITH_OUTLAWS_TESTS
