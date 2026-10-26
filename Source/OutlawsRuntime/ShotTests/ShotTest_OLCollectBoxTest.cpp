// -OLShots -OLLevel=hideout -OLNoFrontEnd -OLCollectBoxTest[=vr]: REQ-COLLECTBOX, one of the ShotTest tests (OutlawsShotTests.h).
//
// This profile's boxes forgotten first; then each of the nine story levels in turn (a level skip, no bad guys, the player not to
// be hurt): its box placed in its spot's sector, that sector its own (the pick by the feet gives it, it shares no space, no zone
// gate); the way from the level's start through the sectors by the player's rules logged (a lift's moving floor or a climb
// stops it: a note, not a check); the way on foot from just outside the box's side room found (player's rules: fences pass,
// doors open), the player photographed 1.6 m from the box and from outside the room (ol_collectbox_<level>_<mode>_near / _door);
// the box taken (desktop: walked to along that way, doors pushed open, a side step round what is in the room; =vr: walked to on
// the even levels, a hand closed on it on the odd ones); NoteBoxFound's count, the shelf's line and the box achievements at 1, 5
// and 9; the level restarted: no box.  The first level also saved before the box was taken and that save loaded after: no box.
// At the end the Extras shelf opened in the pause menu and photographed (ol_collectbox_<mode>_shelf), nine boxes on it.  The
// Saved folder is the runner's to put back.

#include "ShotTestCommon.h"
#include "OutlawsAchievements.h"
#include "OutlawsLevel.h"
#include "OutlawsGeometry.h"
#include "Components/CapsuleComponent.h"

#if WITH_OUTLAWS_TESTS

namespace
{
	struct FCollectBoxTest
	{
		int32 Step = 0;
		double At = 0.0;
		int32 Pass = 0, Fail = 0;
		bool bVRMode = false;
		int32 Index = 0;                    // the level in BoxLevels' order
		TArray<FString> Levels, Labels;
		FVector Feet = FVector::ZeroVector, From = FVector::ZeroVector;
		int32 Sector = -1;
		TArray<FVector> Route;
		int32 RouteIdx = 0;
		double WalkStart = 0.0;
		double SettleAt = 0.0;
		double DoorAt = 0.0;
		FVector LastPos = FVector::ZeroVector;
		double LastAt = 0.0, SideUntil = 0.0;
		double Side = 1.0;
		int32 Clears = 0;
		bool bLoadDone = false;
		FString Mode;
	};
	FCollectBoxTest CB;
	constexpr int32 CollectBoxSlot = 9;
}

OL_SHOT_MATCH(OLCollectBoxTest) { return OLFlags::Has(TEXT("OLCollectBoxTest")); }

OL_SHOT_TEST(OLCollectBoxTest, 287)
{
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (!L || !PC || !Camera || CB.Step < 0) return;
	const double Now = FPlatformTime::Seconds();
	auto Check = [](bool bOk, const FString& What)
	{
		(bOk ? CB.Pass : CB.Fail)++;
		UE_LOG(LogOutlaws, Display, TEXT("OLBOXTEST check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
	};
	const FString ModeTag = CB.bVRMode ? TEXT("vr") : TEXT("desktop");
	auto Shot = [&](const FString& Name)
	{
		const FString Path = FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("ol_collectbox_%s.png"), *Name);
		FScreenshotRequest::RequestScreenshot(Path, false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLBOXTEST shot %s"), *Path);
	};
	const double Half = GetCapsuleComponent() ? GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 90.0;
	auto StandAt = [&](const FVector& Floor, const FVector& Look)
	{
		TeleportTo(Floor + FVector(0.0, 0.0, Half + 2.0), GetActorRotation(), false, true);
		const FVector Eye = Floor + FVector(0.0, 0.0, 160.0);
		PC->SetControlRotation((Look - Eye).Rotation());
	};
	auto BoxMid = [&]() { return CB.Feet + FVector(0.0, 0.0, 22.0); };
	auto FoundCount = [&]()
	{
		int32 N = 0;
		for (const FString& Lv : CB.Levels) N += OutlawsAchievements::BoxFound(L, Lv) ? 1 : 0;
		return N;
	};
	auto Jump = [&](int32 Step)
	{
		CB.Clears = L->GetClearCount();
		L->JumpToLevel(CB.Levels[CB.Index]);
		CB.Step = Step;
		CB.At = Now + 2.0;
	};
	// a level built again since CB.Clears, and settled
	auto Rebuilt = [&]()
	{
		const bool bBuilt = L->GetClearCount() != CB.Clears && !L->PendingLoad.IsValid() && !L->IsRebuildPending() && L->LevelName.Equals(CB.Levels[CB.Index], ESearchCase::IgnoreCase);
		if (!bBuilt) { CB.SettleAt = 0.0; return false; }
		if (CB.SettleAt == 0.0) CB.SettleAt = Now + 1.5;     // (a moment for the player to be put at the start)
		if (Now < CB.SettleAt) return false;
		CB.SettleAt = 0.0;
		return true;
	};
	if (CB.bVRMode) bVR = true;            // (the headset's rules, this run only)
	if (Now < CB.At) return;
	const FString Lv = CB.Levels.IsValidIndex(CB.Index) ? CB.Levels[CB.Index] : FString();
	switch (CB.Step)
	{
	case 0:
	{
		if (CB.At == 0.0) { CB.At = Now + 5.0; return; }
		if (L->IsFrontEnd() || L->PendingLoad.IsValid()) return;
		OLFlags::Value(TEXT("OLCollectBoxTest"), CB.Mode);
		CB.bVRMode = CB.Mode == TEXT("vr");
		OutlawsAchievements::BoxLevels(CB.Levels, CB.Labels);
		L->bInvincible = true;
		L->bSpawnEnemies = false;           // (the walks undisturbed; the boxes do not care)
		OutlawsAchievements::DebugForgetBoxes(L);
		Check(FoundCount() == 0, FString::Printf(TEXT("the profile's boxes forgotten (%d found)"), FoundCount()));
		Check(OutlawsAchievements::BoxesPlaced, TEXT("BoxesPlaced on (the shelf's note)"));
		CB.Index = 0;
		Jump(1);
		return;
	}
	case 1:
	{
		if (!Rebuilt()) { CB.At = Now + 0.5; return; }
		L->bInvincible = true;
		FString Why;
		const bool bSpot = AOutlawsLevelActor::CollectBoxSpot(Lv, CB.Feet, CB.From, CB.Sector, &Why);
		Check(bSpot, FString::Printf(TEXT("%s has a spot: %s"), *Lv, *Why));
		AActor* Box = L->GetCollectBox();
		Check(Box != nullptr, FString::Printf(TEXT("%s: the box placed"), *Lv));
		if (Box) CB.Feet = FVector(CB.Feet.X, CB.Feet.Y, Box->GetActorLocation().Z);      // (the floor there as the level has it)
		const FString Safe = L->CollectBoxSafety();
		Check(Safe.StartsWith(FString::Printf(TEXT("sector %d, the feet pick %d,"), CB.Sector, CB.Sector)) && Safe.Contains(TEXT("its own space")) && Safe.Contains(TEXT("no zone gate")),
			FString::Printf(TEXT("%s: the spot's sector is its own (%s)"), *Lv, *Safe));
		const int32 Cull = L->CullSectorOf(CB.Feet + FVector(0.0, 0.0, 10.0));
		Check(Cull == CB.Sector, FString::Printf(TEXT("%s: the box's place is in sector %d (%d)"), *Lv, CB.Sector, Cull));
		// the way from the level's start by the player's rules (every door opens, fences pass; steps to 3.5 u, then 4.5 with a
		// jump; drops to 20 u): logged, not checked (a lift's moving floor or a climb ends it; the side rooms hold the original's
		// own pickups, the designers' word that a player gets there)
		TArray<FVector> R;
		double Len = 0.0;
		const FVector Start = L->GetPlayerStartLocation() - FVector(0.0, 0.0, 110.0);
		L->bRoutePlayer = true;
		double StartStep = 3.5;
		bool bFromStart = L->PathRoute(Start, CB.Feet, StartStep, 5.8, 1.5, R, Len, 20.0);
		if (!bFromStart) { StartStep = 4.5; bFromStart = L->PathRoute(Start, CB.Feet, StartStep, 5.8, 1.5, R, Len, 20.0); }
		UE_LOG(LogOutlaws, Display, TEXT("OLBOXTEST %s from the level's start, player's rules: %s"), *Lv, bFromStart
			? *FString::Printf(TEXT("a way of %.0f m over %d points, steps to %.1f u"), Len / 100.0, R.Num(), StartStep) : *(TEXT("none through the sectors: ") + L->PathRouteWhy(Start, CB.Feet, 4.5, 5.8, 20.0)));
		CB.Route.Reset();
		Len = 0.0;
		const bool bWay = L->PathRoute(CB.From, CB.Feet, 3.5, 5.8, 1.5, CB.Route, Len, 12.0);
		Check(bWay && CB.Route.Num() > 0, FString::Printf(TEXT("%s: a way on foot from outside the side room to the box (%.1f m, %d points)%s"), *Lv, Len / 100.0, CB.Route.Num(),
			bWay ? TEXT("") : *(TEXT(": ") + L->PathRouteWhy(CB.From, CB.Feet, 3.5, 5.8, 12.0))));
		L->bRoutePlayer = false;
		// 1.6 m back along the way from the box: the near shot first (the first frames after a level's build blur)
		FVector Near = CB.From;
		double Back = 0.0;
		FVector Prev = CB.Feet;
		for (int32 k = CB.Route.Num() - 1; k >= 0; --k)
		{
			const FVector P = CB.Route[k];
			const double D = FVector::Dist(P, Prev);
			if (Back + D >= 160.0 && D > 1.0) { Near = Prev + (P - Prev) * ((160.0 - Back) / D); break; }
			Back += D;
			Prev = P;
			Near = P;
		}
		StandAt(Near, BoxMid());
		CB.Step = 2;
		CB.At = Now + 5.0;
		return;
	}
	case 2:
	{
		Shot(FString::Printf(TEXT("%s_%s_near"), *Lv, *ModeTag));
		CB.Step = 20;                // (the shot is taken at this frame's end: the move waits a moment)
		CB.At = Now + 0.4;
		return;
	}
	case 20:
	{
		StandAt(CB.From, BoxMid());
		CB.Step = 3;
		CB.At = Now + 2.0;
		return;
	}
	case 3:
	{
		Shot(FString::Printf(TEXT("%s_%s_door"), *Lv, *ModeTag));
		if (CB.Index == 0) Check(L->SaveGame(CollectBoxSlot), TEXT("saved to slot 9 before the box was taken"));
		CB.Step = 4;
		CB.At = Now + 0.6;
		return;
	}
	case 4:
	{
		if (CB.bVRMode && CB.Index % 2 == 1)
		{
			// a hand closed on it (the grip's own call, TryHold): the player beside it, the hand on it
			StandAt(CB.Route.Num() > 1 ? CB.Route[CB.Route.Num() - 2] : CB.From, BoxMid());
			const bool bGot = L->TakeCollectBoxByHand(Camera->GetComponentLocation(), BoxMid() + FVector(-8.0, 0.0, 0.0), 16.f);
			Check(bGot && !L->GetCollectBox(), FString::Printf(TEXT("%s: a hand on the box takes it"), *Lv));
			CB.Step = 6;
			CB.At = Now + 1.5;
			return;
		}
		CB.RouteIdx = 0;
		CB.WalkStart = Now;
		CB.LastPos = GetActorLocation();
		CB.LastAt = Now;
		CB.SideUntil = 0.0;
		CB.Step = 5;
		CB.At = Now + 0.3;
		return;
	}
	case 5:
	{
		if (!L->GetCollectBox())
		{
			Check(true, FString::Printf(TEXT("%s: walked into the box and it was taken (%.1f s from the doorway)"), *Lv, Now - CB.WalkStart));
			CB.Step = 6;
			CB.At = Now + 1.5;
			return;
		}
		if (Now - CB.WalkStart > 40.0)
		{
			FString Pts;
			for (const FVector& P0 : CB.Route) Pts += P0.ToCompactString() + TEXT(" ");
			Check(false, FString::Printf(TEXT("%s: walked to the box within 40 s (stuck at %s, point %d of %d: %s; box %s)"), *Lv, *GetActorLocation().ToCompactString(), CB.RouteIdx, CB.Route.Num(), *Pts, *CB.Feet.ToCompactString()));
			L->TakeCollectBoxByHand(BoxMid(), BoxMid(), 100.f);     // (taken all the same, so the rest is checked)
			CB.Step = 6;
			CB.At = Now + 1.5;
			return;
		}
		const FVector Me = GetActorLocation();
		while (CB.RouteIdx < CB.Route.Num() - 1 && FVector::Dist2D(Me, CB.Route[CB.RouteIdx]) < 20.0) ++CB.RouteIdx;     // (through each opening, not across its jamb)
		const FVector To = CB.Route.IsValidIndex(CB.RouteIdx) ? CB.Route[CB.RouteIdx] : CB.Feet;
		FVector Dir = (To - Me).GetSafeNormal2D();
		// held up by something in the room (a crate, a barrel, a stall): a step to one side, then the other the next time
		if (Now - CB.LastAt > 1.0)
		{
			if (FVector::Dist2D(Me, CB.LastPos) < 15.0) { CB.Side = -CB.Side; CB.SideUntil = Now + 0.8; }
			CB.LastPos = Me;
			CB.LastAt = Now;
		}
		if (Now < CB.SideUntil) Dir = (Dir + FVector(-Dir.Y, Dir.X, 0.0) * (1.5 * CB.Side)).GetSafeNormal2D();
		PC->SetControlRotation(FRotator(0.0, Dir.Rotation().Yaw, 0.0));
		AddMovementInput(Dir, 1.f);
		// a door ahead opened as the player opens one (the use button)
		const FVector Eye = Me + FVector(0.0, 0.0, 60.0);
		if (Now >= CB.DoorAt) { CB.DoorAt = Now + 1.0; L->Use(Eye, Dir); }
		return;              // (every frame)
	}
	case 6:
	{
		const int32 N = FoundCount();
		Check(OutlawsAchievements::BoxFound(L, Lv) && N == CB.Index + 1, FString::Printf(TEXT("%s: found on the profile (%d of %d)"), *Lv, N, CB.Levels.Num()));
		const FString Shelf = OutlawsAchievements::Progress(L, TEXT("boxes_all"));
		Check(Shelf.Contains(FString::Printf(TEXT("%d of %d"), N, CB.Levels.Num())), FString::Printf(TEXT("the shelf's count reads %d (%s)"), N, *Shelf));
		Check(OutlawsAchievements::IsEarned(L, TEXT("boxes_first")), TEXT("boxes_first earned"));
		Check(OutlawsAchievements::IsEarned(L, TEXT("boxes_half")) == (N * 2 >= CB.Levels.Num()), FString::Printf(TEXT("boxes_half %s at %d"), N * 2 >= CB.Levels.Num() ? TEXT("earned") : TEXT("not yet"), N));
		Check(OutlawsAchievements::IsEarned(L, TEXT("boxes_all")) == (N == CB.Levels.Num()), FString::Printf(TEXT("boxes_all %s at %d"), N == CB.Levels.Num() ? TEXT("earned") : TEXT("not yet"), N));
		if (CB.Index == 0 && !CB.bLoadDone)
		{
			CB.bLoadDone = true;
			CB.Clears = L->GetClearCount();
			Check(L->LoadGame(CollectBoxSlot), TEXT("slot 9 (saved before the box was taken) loading"));
			CB.Step = 7;
			CB.At = Now + 2.0;
			return;
		}
		CB.Clears = L->GetClearCount();
		L->RestartLevel();
		CB.Step = 8;
		CB.At = Now + 2.0;
		return;
	}
	case 7:
	{
		if (!Rebuilt()) { CB.At = Now + 0.5; return; }
		Check(!L->GetCollectBox(), FString::Printf(TEXT("%s: the save from before loaded, the box does not come back"), *Lv));
		CB.Clears = L->GetClearCount();
		L->RestartLevel();
		CB.Step = 8;
		CB.At = Now + 2.0;
		return;
	}
	case 8:
	{
		if (!Rebuilt()) { CB.At = Now + 0.5; return; }
		Check(!L->GetCollectBox(), FString::Printf(TEXT("%s: restarted, the box does not come back"), *Lv));
		if (++CB.Index < CB.Levels.Num())
		{
			Jump(1);
			return;
		}
		Check(FoundCount() == CB.Levels.Num() && OutlawsAchievements::IsEarned(L, TEXT("boxes_all")), FString::Printf(TEXT("all %d found, boxes_all earned"), CB.Levels.Num()));
		OpenMenu();
		const bool bPage = bMenuOpen && ShowMenuPage(TEXT("#extras_shelf"), false);
		int32 Shown = 0, Named = 0;
		if (bPage && MenuPage)
		{
			for (int32 i = 0; i < MenuPage->Items.Num(); ++i)
			{
				const OutlawsMenu::FItem& I = MenuPage->Items[i];
				Shown += I.Classes.Contains(TEXT("disabled")) ? 0 : 1;
				Named += CB.Labels.IsValidIndex(i) && I.Label == CB.Labels[i] ? 1 : 0;
			}
		}
		Check(bPage && Shown == CB.Levels.Num() && Named == CB.Levels.Num(), FString::Printf(TEXT("the Extras shelf: %d of %d boxes shown, %d with their level's name"), Shown, CB.Levels.Num(), Named));
		RedrawMenu();
		CB.Step = 9;
		CB.At = Now + 1.0;
		return;
	}
	case 9:
		Shot(FString::Printf(TEXT("%s_shelf"), *ModeTag));
		CB.Step = 10;
		CB.At = Now + 1.0;
		return;
	default:
		UE_LOG(LogOutlaws, Display, TEXT("OLBOXTEST RESULT %d passed, %d failed (%s)"), CB.Pass, CB.Fail, *ModeTag);
		CB.Step = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
