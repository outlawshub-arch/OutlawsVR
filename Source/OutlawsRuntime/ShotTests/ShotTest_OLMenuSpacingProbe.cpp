// -OLShots -OLMenuSpacingProbe (REQ-MENUSPACING: the title menu's rows as far apart, for their size, as the remaster's).  From
// the title menu every page under it is shown, drawn and walked (MenuFitWalk: each page at its top and scrolled to its bottom,
// each checked to sit on its panel), then the co-op level and host option pages.  On each page drawn as a list of rows: the
// rows' pitch over their font's size at least the remaster's (OutlawsMenuSpacing::RowsApart, 1.64; the Options sheets
// SheetRowsApart, 1.40), less 0.05; no row's letters over the next one's or over the "more below" line; every row's letters on
// the parchment (x 100 to 1530, under the title and the page's own lines, above the help line at 1015); each row drawn can be
// pointed at; and a list longer than its page reaches its last row by its scroll.  The pause page and the pages with a layout
// of their own (the title page, the difficulty page, the join pad, the keyboard, the lobby, the character page) are listed,
// not checked.  English (the walk's pages as built).  -OLMenuSpacingVR: the headset's layout (bVR).  Lines
// "OLSHOT menuspacing page <file> ...", "OLSHOT menuspacing check PASS|FAIL: ..." and "OLSHOT menuspacing RESULT <n> PASS, <m>
// FAIL".  -OLOldMenuSpacing (the old pitches) must FAIL.

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS
#include "OutlawsMenuSpacing.h"
#include "Engine/Font.h"

OL_SHOT_MATCH(OLMenuSpacingProbe) { return OLFlags::Param(TEXT("OLMenuSpacingProbe")); }

OL_SHOT_TEST(OLMenuSpacingProbe, 2690)
{
	static int32 Stage = 0;
	static float Waited = 0.f;
	if (Stage >= 3) return;
	Waited += Dt;
	if (Stage == 0)
	{
		if (Waited < 4.f || !bMenuOpen || !MenuPage) return;
		if (OLFlags::Param(TEXT("OLMenuSpacingVR")))
		{
			bVR = true;
			DominantHand = EOutlawsHand::Right;
			ShowMenuPage(MenuPage->File, false);
		}
		Stage = 1;
		Waited = 0.f;
		return;
	}
	if (Stage == 1)
	{
		if (Waited < 1.f) return;
		Stage = 2;
		int32 Pass = 0, Fail = 0;
		auto Say = [&Pass, &Fail](bool bOk, const FString& What)
		{
			(bOk ? Pass : Fail)++;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT menuspacing check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		};
		// (a text box's letters: its middle, as the front shot's rule: 19 % off its top and its foot)
		auto Letters = [](const FBox2D& B) { const float Cut = float(B.GetSize().Y) * 0.19f; return FBox2D(B.Min + FVector2D(0.f, Cut), B.Max - FVector2D(0.f, Cut)); };
		TSet<FString> Listed;
		OutlawsMenuSpacing::WalkHook() = [&, this](const FString& File, TFunctionRef<void(bool, const FString&)> Check)
		{
			if (!MenuPage) return;
			const int32 N = MenuPage->Items.Num();
			const OutlawsMenuSpacing::FFrontList& FL = OutlawsMenuSpacing::Front();
			if (IsFrontPage())
			{
				if (!(FL.bList && FL.File == File))
				{
					if (!Listed.Contains(File)) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT menuspacing page %s: a layout of its own (%d items), not a list"), *File, N);
					Listed.Add(File);
					return;
				}
				// the rows drawn, their middles and boxes (view units, 1920 x 1080)
				TArray<int32> Rows;
				for (int32 i = FL.From; i < FL.From + FL.Shown; ++i) if (FrontItemInk.IsValidIndex(i) && FrontItemInk[i].bIsValid) Rows.Add(i);
				float Pitch = 0.f, Most = 0.f;
				for (int32 k = 1; k < Rows.Num(); ++k)
				{
					const float D = float(FrontItemInk[Rows[k]].GetCenter().Y - FrontItemInk[Rows[k - 1]].GetCenter().Y);
					Pitch = k == 1 ? D : FMath::Min(Pitch, D);
					Most = FMath::Max(Most, D);
				}
				const float Ratio = FL.Em > 0.f ? Pitch / FL.Em : 0.f;
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT menuspacing page %s scroll %d: front list, %d rows, %d drawn (%d to %d), pitch %.1f (most %.1f), font %.0f, line %.1f: %.2f of the size (the remaster's %.2f)"),
					*File, FL.From, N, Rows.Num(), FL.From, FL.From + FL.Shown - 1, Pitch, Most, FL.Em, Rows.Num() > 0 ? float(FrontItemInk[Rows[0]].GetSize().Y) : 0.f, Ratio, OutlawsMenuSpacing::RowsApart);
				if (Rows.Num() >= 2)
				{
					Check(Ratio >= OutlawsMenuSpacing::RowsApart - 0.05f, FString::Printf(TEXT("menuspacing %s scroll %d: the rows %.1f apart for a %.0f font, %.2f of its size (the remaster's %.2f, less 0.05)"),
						*File, FL.From, Pitch, FL.Em, Ratio, OutlawsMenuSpacing::RowsApart));
				}
				FString Over, Off, Blind;
				const float TopLimit = FMath::Max(250.f, FL.LinesFoot);
				for (int32 k = 0; k < Rows.Num(); ++k)
				{
					const FBox2D L = Letters(FrontItemInk[Rows[k]]);
					if (k + 1 < Rows.Num() && L.Intersect(Letters(FrontItemInk[Rows[k + 1]]))) Over += FString::Printf(TEXT(" %d over %d;"), Rows[k], Rows[k + 1]);
					if (FL.More.bIsValid && L.Intersect(Letters(FL.More))) Over += FString::Printf(TEXT(" %d over the more line;"), Rows[k]);
					if (L.Min.X < 100.f || L.Max.X > 1530.f || L.Min.Y < TopLimit || L.Max.Y > 1015.f)
						Off += FString::Printf(TEXT(" %d (x %.0f..%.0f y %.0f..%.0f);"), Rows[k], L.Min.X, L.Max.X, L.Min.Y, L.Max.Y);
					const bool bOff = MenuPage->Items[Rows[k]].Classes.Contains(TEXT("disabled"));
					if (!bOff && FrontHitAt(FrontItemInk[Rows[k]].GetCenter()) != Rows[k]) Blind += FString::Printf(TEXT(" %d;"), Rows[k]);
				}
				Check(Over.IsEmpty(), FString::Printf(TEXT("menuspacing %s scroll %d: no row's letters over another's%s"), *File, FL.From, *Over));
				Check(Off.IsEmpty(), FString::Printf(TEXT("menuspacing %s scroll %d: every row on the parchment (x 100..1530, y %.0f..1015)%s"), *File, FL.From, TopLimit, *Off));
				Check(Blind.IsEmpty(), FString::Printf(TEXT("menuspacing %s scroll %d: every row drawn can be pointed at%s"), *File, FL.From, *Blind));
				Check(FL.Shown + FL.MaxScroll >= N && (FL.MaxScroll == 0) == (FL.Shown >= N), FString::Printf(TEXT("menuspacing %s: all %d rows reachable (%d shown, scroll to %d)"), *File, N, FL.Shown, FL.MaxScroll));
				if (FL.MaxScroll > 0 && FL.From == FL.MaxScroll)
				{
					Check(FrontItemInk.IsValidIndex(N - 1) && FrontItemInk[N - 1].bIsValid, FString::Printf(TEXT("menuspacing %s scrolled to %d: the last row drawn"), *File, FL.From));
				}
				return;
			}
			if (!IsSheetPage() || SheetCanvasSize.X <= 0) return;
			// a sheet page: its rows as drawn (canvas pixels), the sheet 100 cm wide
			const float S = float(SheetCanvasSize.X) / 100.f;
			const float Em = MenuRowFont ? float(MenuRowFont->LegacyFontSize) * 4.f / 3.f / S : 0.f;
			float Pitch = 0.f;
			FString Over;
			for (int32 k = 1; k < SheetRowInk.Num(); ++k)
			{
				const float D = float(SheetRowInk[k].GetCenter().Y - SheetRowInk[k - 1].GetCenter().Y) / S;
				Pitch = k == 1 ? D : FMath::Min(Pitch, D);
				if (SheetRowInk[k].Intersect(SheetRowInk[k - 1])) Over += FString::Printf(TEXT(" %d over %d;"), k - 1, k);
			}
			const float Ratio = Em > 0.f ? Pitch / Em : 0.f;
			const bool bPause = File.Equals(TEXT("main_game.rml"), ESearchCase::IgnoreCase);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT menuspacing page %s scroll %d: %s, %d rows, %d drawn, pitch %.2f cm, font %.2f cm: %.2f of the size (the reference %.2f)"),
				*File, MenuScroll, bPause ? TEXT("the pause sheet") : TEXT("sheet"), N, SheetRowInk.Num(), Pitch, Em, Ratio, OutlawsMenuSpacing::SheetRowsApart);
			Check(Over.IsEmpty(), FString::Printf(TEXT("menuspacing %s scroll %d: no row over another%s"), *File, MenuScroll, *Over));
			if (!bPause && IsTallSheet() && SheetRowInk.Num() >= 2)
			{
				Check(Ratio >= OutlawsMenuSpacing::SheetRowsApart - 0.05f, FString::Printf(TEXT("menuspacing %s scroll %d: the rows %.2f cm apart for a %.2f cm font, %.2f of its size (the Options reference %.2f, less 0.05)"),
					*File, MenuScroll, Pitch, Em, Ratio, OutlawsMenuSpacing::SheetRowsApart));
			}
		};
		int32 Walked = 0;
		for (const TCHAR* Root : { TEXT("#front"), TEXT("#cooplevels"), TEXT("#coophostopts") })
		{
			MenuStack.Reset();
			if (FCString::Strcmp(Root, TEXT("#front")) != 0) MenuStack.Add(TEXT("#front"));
			Walked += MenuFitWalk(Root, [&Say](bool bOk, const FString& What) { Say(bOk, What); });
		}
		OutlawsMenuSpacing::WalkHook() = nullptr;
		Say(Walked >= 20, FString::Printf(TEXT("menuspacing: %d pages walked"), Walked));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT menuspacing RESULT %d PASS, %d FAIL (%s, %s)"), Pass, Fail, OutlawsMenuSpacing::Old() ? TEXT("-OLOldMenuSpacing") : TEXT("new spacing"), bVR ? TEXT("headset layout") : TEXT("desktop"));
		MenuStack.Reset();
		ShowMenuPage(TEXT("#front"), false);
		Waited = 0.f;
		return;
	}
	if (Waited < 1.f) return;
	Stage = 3;
	FGenericPlatformMisc::RequestExit(false);
}

#endif // WITH_OUTLAWS_TESTS
