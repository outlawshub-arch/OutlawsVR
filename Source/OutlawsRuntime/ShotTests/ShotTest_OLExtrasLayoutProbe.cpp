// BUG-ACHIEVBOX + BUG-COLLECTTEXT (user 3 Nov, the user's snapshots #5 and #6): -OLShots -OLExtrasLayoutProbe[=vr]
// [-OLOldExtrasLayout], one of the ShotTest tests (OutlawsShotTests.h).  From the title menu: the achievements page drawn with no
// toast and with one (as an achievement earned on it shows), then the Collectibles shelf with its "Found in X" toast; on each
// the title's, the count's, the toast's and the frame's (the shelf: the boxes' labels, the note under them, the help line)
// boxes as drawn, none over another (a shared edge is not an overlap; the rule checked on two boxes each run); =vr the
// headset's layout (the prompts on a plate, not on the canvas).  Shots
// ol_extraslayout_[vr_]achiev / _achiev_toast / _shelf_toast[_old].png in Saved\OLShots.  Lines "OLSHOT extraslayout check
// PASS|FAIL: ..." and "OLSHOT extraslayout RESULT".

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLExtrasLayoutProbe) { return OLFlags::Has(TEXT("OLExtrasLayoutProbe")); }

OL_SHOT_TEST(OLExtrasLayoutProbe, 1680)
{
	static int32 Passes = 0, Fails = 0;
	static float NextAt = 0.f;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT extraslayout check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	auto Page = [this]() { return bMenuOpen && MenuPage ? MenuPage->File : FString(); };
	const bool bTestVR = OLFlags::ValueIs(TEXT("OLExtrasLayoutProbe"), TEXT("vr"));
	const bool bOld = OLFlags::Param(TEXT("OLOldExtrasLayout"));
	const FString Pre = FString(bTestVR ? TEXT("ol_extraslayout_vr_") : TEXT("ol_extraslayout_"));
	const FString Post = bOld ? TEXT("_old") : TEXT("");
	auto Shot = [&](const TCHAR* Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / (Pre + Name + Post) + TEXT(".png"), false, false); };
	auto Str = [](const FBox2D& B) { return B.bIsValid ? FString::Printf(TEXT("(%.0f,%.0f)-(%.0f,%.0f)"), B.Min.X, B.Min.Y, B.Max.X, B.Max.Y) : FString(TEXT("none")); };
	// two boxes are over each other when they share more than a hairline both ways (half a unit of the 1920 x 1080 view, a
	// third of a pixel at 1280 x 720); boxes that only touch, one's edge on the other's (the count's top on the flourish's
	// foot at y 217, the probe sweep of 7 Nov), are apart.  FBox2D::Intersect counts a shared edge
	auto Depth = [](const FBox2D& A, const FBox2D& B)
	{
		return FVector2D(FMath::Min(A.Max.X, B.Max.X) - FMath::Max(A.Min.X, B.Min.X), FMath::Min(A.Max.Y, B.Max.Y) - FMath::Max(A.Min.Y, B.Min.Y));
	};
	// every pair of named boxes apart (a box not drawn is skipped); Want > 0: the rule's own check, that many overlaps wanted
	auto Apart = [&](const TCHAR* Where, const TArray<TPair<FString, FBox2D>>& Boxes, int32 Want = 0)
	{
		int32 Over = 0;
		FString Said;
		for (int32 a = 0; a < Boxes.Num(); ++a)
		{
			for (int32 b = a + 1; b < Boxes.Num(); ++b)
			{
				const FBox2D& A = Boxes[a].Value;
				const FBox2D& B = Boxes[b].Value;
				if (!A.bIsValid || !B.bIsValid) continue;
				const FVector2D D = Depth(A, B);
				if (D.X <= 0.5 || D.Y <= 0.5) continue;
				++Over;
				Said += FString::Printf(TEXT(" %s %s over %s %s (by %.2f x %.2f);"), *Boxes[a].Key, *Str(A), *Boxes[b].Key, *Str(B), D.X, D.Y);
			}
		}
		FString All;
		for (const TPair<FString, FBox2D>& Kv : Boxes) All += FString::Printf(TEXT(" %s %s"), *Kv.Key, *Str(Kv.Value));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT extraslayout %s boxes:%s"), Where, *All);
		// (the old layout: the overlaps logged, as the snapshots showed them)
		if (Want > 0) Check(Over == Want, FString::Printf(TEXT("%s: %d overlaps counted, %d wanted%s"), Where, Over, Want, *Said));
		else if (bOld) Check(true, FString::Printf(TEXT("old layout, %s: %d overlaps%s"), Where, Over, *Said));
		else Check(Over == 0, FString::Printf(TEXT("%s: nothing over anything else (%d overlaps)%s"), Where, Over, *Said));
		return Over;
	};
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT extraslayout RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	};
	if (ShotTime < NextAt) return;
	NextAt = ShotTime + 1.f;
	float Fx = 0.f, Fy = 0.f, Fw = 0.f, Fh = 0.f;
	AchievFrameBox(Fx, Fy, Fw, Fh);
	const FBox2D Frame(FVector2D(Fx, Fy), FVector2D(Fx + Fw, Fy + Fh));
	switch (ShotStep)
	{
	case 0:
		if (Page() != TEXT("#front") || ShotTime < 4.f)
		{
			if (ShotTime > 120.f) { Check(false, TEXT("the title menu came up within 120 s")); Finish(); }
			NextAt = 0.f;
			return;
		}
		if (bTestVR)
		{
			bVR = true;
			ShowMenuPage(TEXT("#front"), false);
		}
		Check(TestPressLink(TEXT("#extras")) && TestPressLink(TEXT("#extras_achiev")) && IsAchievPage(), FString::Printf(TEXT("Achievements opens (%s)"), *Page()));
		Check(FMath::IsNearlyEqual(Fx, bOld ? 953.f : 1020.2f, 1.f) && FMath::IsNearlyEqual(Fy, bOld ? 194.f : 257.7f, 1.f),
			FString::Printf(TEXT("the frame at %.1f, %.1f, %.1f x %.1f (%s)"), Fx, Fy, Fw, Fh, bOld ? TEXT("old: 953, 194, 840 x 796") : TEXT("0.92 about 1793, 990: 1020, 258, 773 x 732")));
		RedrawMenu();
		break;
	case 1:
	{
		// the lower flourish under the title ends about y 217 over x 788..1151 (measured on this probe's first shot): the count and the frame
		// clear of it (the title's own box reaches into it: the flourish is drawn round the title's foot on the parchment)
		Apart(TEXT("achievements, no toast"), { { TEXT("title"), AchievTitleInk }, { TEXT("count"), AchievCountInk }, { TEXT("frame"), Frame } });
		Apart(TEXT("achievements, the flourish"), { { TEXT("count"), AchievCountInk }, { TEXT("frame"), Frame }, { TEXT("flourish"), FBox2D(FVector2D(788.f, 185.f), FVector2D(1151.f, 217.f)) } });
		{
			// the rule itself, on the boxes the sweep saw: the count on the flourish's foot is apart, one unit up into it is over it
			const FBox2D Flourish(FVector2D(788.f, 185.f), FVector2D(1151.f, 217.f));
			Apart(TEXT("the rule, a shared edge"), { { TEXT("count"), FBox2D(FVector2D(884.f, 217.f), FVector2D(1036.f, 255.f)) }, { TEXT("flourish"), Flourish } });
			Apart(TEXT("the rule, one unit into it"), { { TEXT("count"), FBox2D(FVector2D(884.f, 216.f), FVector2D(1036.f, 254.f)) }, { TEXT("flourish"), Flourish } }, 1);
		}
		if (!bOld) Check(AchievCountInk.bIsValid && AchievCountInk.Min.Y > AchievTitleInk.Max.Y - 2.f, FString::Printf(TEXT("the count line under the title's foot (count %s, title %s)"), *Str(AchievCountInk), *Str(AchievTitleInk)));
		Shot(TEXT("achiev"));
		break;
	}
	case 2:
		MenuToastText(TEXT("Achievement: Greenhorn"));      // (a step after the shot: the shot is taken at its frame's end)
		RedrawMenu();
		break;
	case 3:
		Apart(TEXT("achievements, a toast"), { { TEXT("title"), AchievTitleInk }, { TEXT("count"), AchievCountInk }, { TEXT("toast"), AchievToastInk }, { TEXT("frame"), Frame } });
		Check(AchievToastInk.bIsValid, FString::Printf(TEXT("the toast drawn (%s)"), *Str(AchievToastInk)));
		Shot(TEXT("achiev_toast"));
		break;
	case 4:
		MenuBack();
		Check(TestPressLink(TEXT("#extras_shelf")) && Page() == TEXT("#extras_shelf"), FString::Printf(TEXT("Collectibles opens (%s)"), *Page()));
		MenuToastText(TEXT("Found in Bandit Hideout"));
		RedrawMenu();
		break;
	case 5:
	{
		TArray<TPair<FString, FBox2D>> Boxes = { { TEXT("title"), ShelfTitleInk }, { TEXT("count"), ShelfCountInk }, { TEXT("note"), ShelfNoteInk },
			{ TEXT("help"), WideHelpInk } };
		for (const FBox2D& T : SheetToastInk) Boxes.Add({ TEXT("toast"), T });
		for (int32 i = 0; i < FrontItemInk.Num(); ++i) Boxes.Add({ FString::Printf(TEXT("label%d"), i), FrontItemInk[i] });
		Apart(TEXT("collectibles, a toast"), Boxes);
		if (!bOld) Check(SheetToastInk.Num() > 0 && SheetToastInk[0].bIsValid && !ShelfNoteInk.bIsValid && FMath::Abs(SheetToastInk[0].GetCenter().Y - 970.0) < 10.0,
			FString::Printf(TEXT("the toast in the note's line, the note hidden while it shows (toast %s, note %s)"), SheetToastInk.Num() > 0 ? *Str(SheetToastInk[0]) : TEXT("none"), *Str(ShelfNoteInk)));
		Shot(TEXT("shelf_toast"));
		break;
	}
	case 6:
		Finish();
		return;
	default:
		return;
	}
	++ShotStep;
}

#endif // WITH_OUTLAWS_TESTS
