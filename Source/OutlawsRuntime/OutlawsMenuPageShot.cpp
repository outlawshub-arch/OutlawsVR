// -OLMenuPageShot=<page> (8 Nov, the sign-off shots: the user signs off a visible change from before / after pictures): the
// title menu opens, the page is shown as the player reaches it (Options, then the page), and Saved\OLShots\ol_menupage_<page>.png
// is taken; then the run ends.  <page> is a menu file (options_display.rml) or one of ours (#mp, #voice, #vr).  The line
// "OLSHOT menupage shot <file>: page <page>, <n> rows: <labels>".  -OLMenuPageSample: the accent sample's two lines added
// under the rows (REQ-LANGUAGE).
#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "OutlawsMenuDoc.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLMenuPageShot) { return OLFlags::Has(TEXT("OLMenuPageShot")); }

OL_SHOT_TEST(OLMenuPageShot, 2560)
{
	static int32 Stage = 0;
	static float Waited = 0.f;
	if (Stage >= 3) return;
	Waited += Dt;
	FString Page;
	if (!OLFlags::Value(TEXT("OLMenuPageShot"), Page) || Page.IsEmpty()) Page = TEXT("#mp");
	const FString Clean = Page.Replace(TEXT("#"), TEXT("")).Replace(TEXT(".rml"), TEXT(""));
	if (Stage == 0)
	{
		if (Waited < 4.f || !bMenuOpen) return;
		MenuHover[0] = MenuHover[1] = -1;
		MenuStack.Reset();
		MenuStack.Add(TEXT("#front"));
		MenuStack.Add(TEXT("options.rml"));
		ShowMenuPage(Page, false);
		// (-OLMenuPageScroll=<n>: a long page scrolled down n rows first, for a row under the fold)
		FString Sc;
		if (OLFlags::Value(TEXT("OLMenuPageScroll"), Sc) && Sc.IsNumeric()) { MenuScroll = FCString::Atoi(*Sc); RefreshMenuRows(); }
		// (REQ-LANGUAGE, -OLMenuPageSample: the accent sample's two lines added under the page's rows, for the accents sign-off)
		if (OLFlags::Has(TEXT("OLMenuPageSample")) && MenuPage)
		{
			for (const TCHAR* Line : { TEXT("Fran\u00E7ais, Espa\u00F1ol, Portugu\u00EAs, Activ\u00E9"), TEXT("\u00DCberpr\u00FCfen, A\u00E7\u00E3o, Perch\u00E9") })
			{
				OutlawsMenu::FItem H;
				H.Kind = OutlawsMenu::EItemKind::Heading;
				H.Label = Line;
				MenuPage->Items.Add(H);
			}
			RefreshMenuRows();
		}
		Stage = 1; Waited = 0.f;
		return;
	}
	if (Stage == 1)
	{
		if (Waited < 1.5f) return;
		const FString Name = TEXT("ol_menupage_") + Clean;
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		FString Rows;
		if (MenuPage) for (int32 i = 0; i < MenuPage->Items.Num(); ++i) Rows += (Rows.IsEmpty() ? TEXT("") : TEXT(" | ")) + MenuRowText(i);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT menupage shot %s: page %s, %d rows: %s"), *Name, MenuPage ? *MenuPage->File : TEXT("none"),
			MenuPage ? MenuPage->Items.Num() : 0, *Rows);
		Stage = 2; Waited = 0.f;
		return;
	}
	if (Waited < 2.f) return;
	Stage = 3;
	FGenericPlatformMisc::RequestExit(false);
}

#endif // WITH_OUTLAWS_TESTS
