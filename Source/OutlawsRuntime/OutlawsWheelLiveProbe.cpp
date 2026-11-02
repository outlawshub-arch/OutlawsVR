// BUG-WHEELLIVE (user 2 Nov: the wheel kept what it showed at the open, a pickup made while it was up did not show): -OLShots
// -OLNoFrontEnd -OLLevel=hideout -OLWheelLiveProbe [-OLOldWheelLive], flat.  With the wheel open: (a) bullets added, the revolver's
// count plate shows them; (b) the rifle and its cartridges given, its slot no longer darkened and its count plate shown; (c) a
// medical bag added, its count and its label (the banner's name) one more; (d) a key given, its card appears in the item row.
// (a)-(c) make no part (the slots, plates and shades as many as before).  -OLOldWheelLive: the snapshot as before, the cases FAIL
// (the control).  Lines "OLSHOT wheellive check PASS|FAIL: ..." and "OLSHOT wheellive RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLWheelLiveProbe) { return OLFlags::Param(TEXT("OLWheelLiveProbe")); }

OL_SHOT_TEST(OLWheelLiveProbe, 1610)
{
	static int32 Passes = 0, Fails = 0, SlotsWas = 0, PlatesWas = 0, ShadesWas = 0, BagsWas = 0, RebuildsWas = 0;
	static float At = 0.f;
	if (!L || ShotStep < 0) return;
	const bool bOld = OldWheelLive();
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wheellive check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	auto SlotOf = [this](int32 Weapon, const TCHAR* Item) -> int32
	{
		for (int32 i = 0; i < WheelWeapon.Num(); ++i) if (Item ? WheelItem[i] == Item : WheelWeapon[i] == Weapon) return i;
		return -1;
	};
	auto CountText = [this](int32 i) { return WheelCountPlates.IsValidIndex(i) && PlateShown.IsValidIndex(WheelCountPlates[i]) ? PlateShown[WheelCountPlates[i]] : FString(TEXT("(none)")); };
	auto CountShown = [this](int32 i) { return WheelCountPlates.IsValidIndex(i) && PlatePlanes.IsValidIndex(WheelCountPlates[i]) && PlatePlanes[WheelCountPlates[i]]->IsVisible(); };
	auto Parts = [this]() { return FString::Printf(TEXT("%d slots, %d plates, %d shades"), WheelSlots.Num(), PlateRTs.Num(), WheelShades.Num()); };
	auto NoneMade = [&]() { return WheelSlots.Num() == SlotsWas && PlateRTs.Num() == PlatesWas && WheelShades.Num() == ShadesWas; };
	const double Wait = 0.4;      // (the wheel reads its numbers a tenth of a second apart)
	switch (ShotStep)
	{
	case 0:
		if (ShotTime < 4.f || !bUp || WarmFrames <= 3) return;
		OpenWheel(1);
		SlotsWas = WheelSlots.Num();
		PlatesWas = PlateRTs.Num();
		ShadesWas = WheelShades.Num();
		RebuildsWas = WheelLiveRebuilds;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wheellive %s: the wheel open with %d slots shown (%s); the revolver's count \"%s\""), bOld ? TEXT("old (the snapshot)") : TEXT("live"),
			WheelWeapon.Num(), *Parts(), *CountText(SlotOf(0, nullptr)));
		At = ShotTime;
		ShotStep = 1;
		return;
	case 1:
	{
		if (ShotTime < At + 0.5f) return;
		// (a) bullets
		const int32 i = SlotOf(0, nullptr);
		const FString Was = CountText(i);
		const int32 Reserve = L->GetCount(TEXT("ibullit"));
		L->AddCount(TEXT("ibullit"), 10);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wheellive (a) 10 bullets added (%d -> %d); the revolver's plate was \"%s\""), Reserve, L->GetCount(TEXT("ibullit")), *Was);
		At = ShotTime;
		ShotStep = 2;
		return;
	}
	case 2:
	{
		if (ShotTime < At + Wait) return;
		const int32 i = SlotOf(0, nullptr);
		const FString Now = CountText(i);
		const FString Want = FString::Printf(TEXT("| %d"), L->GetCount(TEXT("ibullit")));
		Check(Now.EndsWith(Want) && CountShown(i), FString::Printf(TEXT("(a) the revolver's plate shows the bullets added: \"%s\" (wanted \"...%s\")"), *Now, *Want));
		Check(NoneMade(), FString::Printf(TEXT("(a) nothing made for a count: %s"), *Parts()));
		// (b) the rifle and its cartridges
		const int32 r = SlotOf(1, nullptr);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wheellive (b) the rifle given: before, darkened %d, its plate shown %d \"%s\""), WheelShades.IsValidIndex(r) && WheelShades[r]->IsVisible() ? 1 : 0,
			CountShown(r) ? 1 : 0, *CountText(r));
		L->AddCount(TEXT("irifle"), 1);
		L->AddCount(TEXT("icart"), 20);
		At = ShotTime;
		ShotStep = 3;
		return;
	}
	case 3:
	{
		if (ShotTime < At + Wait) return;
		const int32 r = SlotOf(1, nullptr);
		const bool bShaded = WheelShades.IsValidIndex(r) && WheelShades[r]->IsVisible();
		Check(r >= 0 && !bShaded && WheelUsable.IsValidIndex(r) && WheelUsable[r] && CountShown(r) && CountText(r).EndsWith(FString::Printf(TEXT("| %d"), L->GetCount(TEXT("icart")))),
			FString::Printf(TEXT("(b) the rifle's slot lit (darkened %d, usable %d) and its plate shown (%d) \"%s\""), bShaded ? 1 : 0, WheelUsable.IsValidIndex(r) && WheelUsable[r] ? 1 : 0, CountShown(r) ? 1 : 0, *CountText(r)));
		Check(NoneMade(), FString::Printf(TEXT("(b) nothing made for a weapon gained: %s"), *Parts()));
		// (c) a medical bag
		BagsWas = L->GetCount(TEXT("idocbag"));
		L->AddCount(TEXT("idocbag"), 1);
		At = ShotTime;
		ShotStep = 4;
		return;
	}
	case 4:
	{
		if (ShotTime < At + Wait) return;
		const int32 b = SlotOf(-2, TEXT("idocbag"));
		const FString Label = WheelLabels.IsValidIndex(b) ? WheelLabels[b]->Text.ToString() : FString();
		const FString Want = FString::Printf(TEXT("Medical Bag %d"), BagsWas + 1);
		Check(b >= 0 && CountText(b) == FString::FromInt(BagsWas + 1) && Label == Want && WheelUsable.IsValidIndex(b) && WheelUsable[b],
			FString::Printf(TEXT("(c) the bag's count \"%s\" and label \"%s\" (wanted %d, \"%s\")"), *CountText(b), *Label, BagsWas + 1, *Want));
		Check(NoneMade(), FString::Printf(TEXT("(c) nothing made for a count: %s"), *Parts()));
		// (d) a key
		L->DebugGiveListed(TEXT("ibrsskey"), TEXT("Brass Key"));
		At = ShotTime;
		ShotStep = 5;
		return;
	}
	case 5:
	{
		if (ShotTime < At + Wait) return;
		const int32 k = SlotOf(-2, TEXT("ibrsskey"));
		Check(k >= 0 && WheelSlots.IsValidIndex(k) && WheelSlots[k]->IsVisible(), FString::Printf(TEXT("(d) the key's card in the item row (slot %d of %d, laid out again %d times)"), k, WheelWeapon.Num(),
			WheelLiveRebuilds - RebuildsWas));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wheellive parts at the end: %s (at the open %d slots, %d plates, %d shades); counts redrawn %d times"), *Parts(), SlotsWas, PlatesWas, ShadesWas, WheelLiveRefreshes);
		CloseWheel();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wheellive RESULT %s (%d passed, %d failed)%s"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			bOld ? TEXT(" (-OLOldWheelLive: the snapshot, FAIL expected)") : TEXT(""));
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	}
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
