// REQ-LEGACYKPF (user 2026-10-31, option 1): the VR menus read the user's edited menu theme from MenuOverlay.kpf before the
// player's Common.kpf (pages, fonts, the parchment; loc_english stays on Common.kpf).  -OLShots -OLMenuOverlayProbe from the
// title menu, the overlay given by -OLMenuOverlay=<file> (or found next to the game); -OLMenuOverlayProbe=off with
// -OLNoMenuOverlay checks the stock reads.  Cases: the overlay open (or not), each overlay entry served from it, the
// placeholders and the strings not, every page parsed, the parchment and the menu art decoded from the overlay's own bytes,
// the fonts cached apart (Saved\KpfCache\overlay) with Noto behind them for punctuation; then a shot of the main menu,
// Options, Options > Display, the difficulty page and the pause page (ol_legacykpf_<page>.png in Saved\OLShots).  Lines "OLSHOT legacykpf
// check PASS|FAIL: ..." and "OLSHOT legacykpf RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "OutlawsMenuDoc.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLMenuOverlayProbe) { return OLFlags::Has(TEXT("OLMenuOverlayProbe")); }

OL_SHOT_TEST(OLMenuOverlayProbe, 1310)
{
	static int32 Passes = 0, Fails = 0;
	if (ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT legacykpf check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	auto Shot = [](const FString& Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false); };
	// the pages shot, each one level under the main menu as the player reaches it
	// (and the pause page last, shown from the title: its rows as main_game.rml lists them, End Game under Restart Level)
	static const TCHAR* const Pages[] = { TEXT("#front"), TEXT("options.rml"), TEXT("options_display.rml"), TEXT("#difficulty:story"), TEXT("main_game.rml") };
	static const TCHAR* const Names[] = { TEXT("main"), TEXT("options"), TEXT("options_display"), TEXT("difficulty"), TEXT("pause") };
	if (ShotStep == 0)
	{
		if (ShotTime < 4.f || !bMenuOpen) return;
		const bool bWant = !OLFlags::ValueIs(TEXT("OLMenuOverlayProbe"), TEXT("off"));
		if (!LoadMenuSource())
		{
			Check(false, TEXT("the menu source loads (Common.kpf)"));
		}
		else
		{
			const OutlawsMenu::FSource& S = *MenuSource;
			Check(S.HasOverlay() == bWant, FString::Printf(TEXT("the overlay is %s (want %s)"), S.HasOverlay() ? TEXT("open") : TEXT("not open"), bWant ? TEXT("open") : TEXT("not open")));
			// the 38 entries build_menu_overlay.py writes (main_game_multi and progress are the user's too, kept the stock date;
			// nothing here reads them yet, so they are served but not parsed)
			static const TCHAR* const Rml[] = { TEXT("campaign"), TEXT("dialog_legal"), TEXT("difficulty"), TEXT("extras"), TEXT("lobby_main"), TEXT("main"),
				TEXT("main_game"), TEXT("multiplayer_driver"), TEXT("multiplayer_host_start"), TEXT("multiplayer_main"), TEXT("options"), TEXT("options_display"),
				TEXT("options_gameplay"), TEXT("options_hud"), TEXT("options_input"), TEXT("options_input_acceleration"), TEXT("options_input_deadzone"),
				TEXT("options_input_gamepad"), TEXT("options_input_motion_sensor"), TEXT("options_input_mouse"), TEXT("options_input_sensitivity"),
				TEXT("options_input_vibration"), TEXT("options_language"), TEXT("options_multiplayer"), TEXT("options_sound"), TEXT("options_video"),
				TEXT("select_rcp") };
			static const TCHAR* const Other[] = { TEXT("menu/common.rcss"), TEXT("menu/dialog.rcss"), TEXT("menu/fragtable.rcss"), TEXT("menu/menu.rcss"),
				TEXT("fonts/Rye-Regular.ttf"), TEXT("fonts/MontaguSlab-VariableFont_opsz,wght.ttf"), TEXT("fonts/MontaguSlab_24pt-Bold.ttf"),
				TEXT("fonts/Ultra-Regular.ttf"), TEXT("menu/assets/Main_Base.dds"), TEXT("menu/main_game_multi.rml"), TEXT("menu/progress.rml") };
			int32 Served = 0, Parsed = 0;
			FString NotServed, NotParsed;
			for (const TCHAR* R : Rml)
			{
				const FString File = FString(R) + TEXT(".rml");
				if (S.FromOverlay(TEXT("menu/") + File)) ++Served; else NotServed += TEXT(" ") + File;
				OutlawsMenu::FPage Pg;
				// (extras.rml is the user's own page: stock Common.kpf has none)
				if (S.GetPage(File, Pg)) ++Parsed; else if (bWant || File != TEXT("extras.rml")) NotParsed += TEXT(" ") + File;
			}
			for (const TCHAR* O : Other) { if (S.FromOverlay(O)) ++Served; else NotServed += FString(TEXT(" ")) + O; }
			const int32 All = int32(UE_ARRAY_COUNT(Rml) + UE_ARRAY_COUNT(Other));
			Check(bWant ? Served == All : Served == 0, FString::Printf(TEXT("%d of %d overlay entries served from the overlay (want %d)%s"), Served, All, bWant ? All : 0,
				bWant && !NotServed.IsEmpty() ? *(TEXT(": not") + NotServed) : TEXT("")));
			Check(NotParsed.IsEmpty(), FString::Printf(TEXT("%d of %d pages parsed%s"), Parsed, int32(UE_ARRAY_COUNT(Rml)), NotParsed.IsEmpty() ? TEXT("") : *(TEXT(": not") + NotParsed)));
			Check(!S.FromOverlay(TEXT("menu/assets/Main_Background.dds")) && !S.FromOverlay(TEXT("localization/loc_english.txt")),
				TEXT("Main_Background (the magenta placeholder) and loc_english not in the overlay"));
			const FString Resume = S.Loc(TEXT("$m_local_6050"));
			Check(!Resume.IsEmpty() && !Resume.StartsWith(TEXT("$")), FString::Printf(TEXT("the strings read from Common.kpf: $m_local_6050 = \"%s\""), *Resume));
			// the parchment: the overlay's is an uncompressed 32-bit DDS (UI.kpf's is BC7), decoded from the bytes ReadArt gave
			TArray<uint8> D;
			uint32 Bits = 0;
			int32 W = 0, H = 0;
			const bool bArt = S.ReadArt(TEXT("menu/assets/Main_Base.dds"), D) && D.Num() > 128;
			if (bArt) { FMemory::Memcpy(&H, D.GetData() + 12, 4); FMemory::Memcpy(&W, D.GetData() + 16, 4); FMemory::Memcpy(&Bits, D.GetData() + 88, 4); }
			const bool bDx10 = bArt && FMemory::Memcmp(D.GetData() + 84, "DX10", 4) == 0;
			if (bWant) Check(bArt && !bDx10 && Bits == 32, FString::Printf(TEXT("Main_Base read as menu art: %d x %d, %u-bit, %s"), W, H, Bits, bDx10 ? TEXT("BC7 (UI.kpf's)") : TEXT("uncompressed (the overlay's)")));
			UTexture2D* Art = KpfArt(TEXT("Main_Base"));
			Check(Art && Art->GetSizeX() == W && Art->GetSizeY() == H, FString::Printf(TEXT("KpfArt(Main_Base) decoded %d x %d"), Art ? Art->GetSizeX() : 0, Art ? Art->GetSizeY() : 0));
			if (bWant) Check(MenuParchment && MenuParchment->GetSizeX() == W, FString::Printf(TEXT("the sheet's parchment (KpfDds) is the overlay's %d px wide one"), MenuParchment ? MenuParchment->GetSizeX() : 0));
			// the fonts: copied out to their own folder, Noto behind each for punctuation
			TArray<uint8> Rye;
			S.ReadFile(TEXT("fonts/Rye-Regular.ttf"), Rye);
			const FString Cached = FPaths::ProjectSavedDir() / TEXT("KpfCache") / (bWant ? TEXT("overlay/Rye-Regular.ttf") : TEXT("Rye-Regular.ttf"));
			Check(Rye.Num() > 0 && IFileManager::Get().FileSize(*Cached) == Rye.Num(), FString::Printf(TEXT("Rye cached at %s (%lld bytes, read %d)"), *Cached, IFileManager::Get().FileSize(*Cached), Rye.Num()));
			for (const UFont* F : { MenuTitleFont.Get(), MenuRowFont.Get() })
			{
				const bool bNoto = F && F->CompositeFont.SubTypefaces.Num() > 0 && F->CompositeFont.SubTypefaces[0].Typeface.Fonts.Num() > 0
					&& F->CompositeFont.SubTypefaces[0].Typeface.Fonts[0].Font.GetFontFilename().Contains(TEXT("NotoSans"));
				Check(bNoto, FString::Printf(TEXT("%s: %s, punctuation from Noto %s"), F == MenuTitleFont.Get() ? TEXT("title font") : TEXT("row font"),
					F ? *F->CompositeFont.DefaultTypeface.Fonts[0].Font.GetFontFilename() : TEXT("none"), bNoto ? TEXT("yes") : TEXT("no")));
			}
		}
		ShotStep = 1; ShotTime = 0.f;
		return;
	}
	const int32 Pi = (ShotStep - 1) / 2;
	if (Pi >= int32(UE_ARRAY_COUNT(Pages)))
	{
		if (ShotTime < 1.f) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT legacykpf RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	}
	if ((ShotStep - 1) % 2 == 0 && ShotTime > 1.f)
	{
		MenuHover[1] = -1;
		MenuStack.Reset();
		if (Pi > 0 && Pi < 4) MenuStack.Add(TEXT("#front"));
		if (Pi == 2) MenuStack.Add(TEXT("options.rml"));
		ShowMenuPage(Pages[Pi], false);
		++ShotStep; ShotTime = 0.f;
	}
	else if ((ShotStep - 1) % 2 == 1 && ShotTime > 1.5f)
	{
		Shot(FString(TEXT("ol_legacykpf_")) + Names[Pi]);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT legacykpf shot ol_legacykpf_%s: page %s, %d rows"), Names[Pi], MenuPage ? *MenuPage->File : TEXT("none"), MenuPage ? MenuPage->Items.Num() : 0);
		++ShotStep; ShotTime = 0.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
