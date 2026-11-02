// BUG-WRISTCRASH (the user's Quest crash 2 Nov 2026 15:32, build 809f200: a level restart, the pause menu's remastered graphics
// off and on, the menu closed, and a second later, as the wrist came into view, a SIGSEGV in SetTextureParameterValue from
// LayoutWrist's Icon): -OLShots -OLLevel=hideout -OLNoFrontEnd -OLWristCrashProbe[=vr] [-OLOldWristCache].  The wrist held
// in front of the camera (as -OLWristTest), the plate worn so both cards show; three rounds of: the remastered sprites off
// and on (both pictures of each card cached), the cache noted (its entries, weakly), the level restarted, a GC forced; then
// (1) the noted entries whose texture is gone (the atlases the restart's Clear() dropped; read from the weak side only);
// (2) the player's own cache after the restart: nothing stale left in it; (3) the noted entries put back (a cache no
// restart cleared) and each of them asked for again: the new code rebuilds each from the level as it is now and hands out
// only live textures; -OLOldWristCache hands the freed one out (the pointer is only compared, never used); (4) the
// remastered sprites flipped and a hit taken so the wrist lays out again: every card's material holds a live texture (one in
// the object array, not garbage), the heart's is the level's own now, the heart and plate cards shown.  The first round's
// wrist photographed (ol_wristcrash_<new|old>_<flat|vr>.png).  Lines "OLSHOT wristcrash check PASS|FAIL: ..." and
// "OLSHOT wristcrash RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/Texture.h"
#include "Engine/Engine.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UObject/UObjectIterator.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

namespace
{
	int32 WcGcCount = 0;
}

OL_SHOT_MATCH(OLWristCrashProbe) { return OLFlags::Has(TEXT("OLWristCrashProbe")); }

OL_SHOT_TEST(OLWristCrashProbe, 1420)
{
	static int32 Passes = 0, Fails = 0, Round = 0, GcMark = 0;
	static float Wait = 0.f;
	static bool bVRMode = false, bHooked = false;
	static TMap<FString, FWristIconHeld> Noted;
	constexpr int32 Rounds = 3;
	if (!L || ShotStep < 0) return;
	const bool bOld = OLFlags::Param(TEXT("OLOldWristCache"));
	const TCHAR* const Tag = bOld ? TEXT("old") : TEXT("new");
	if (bVRMode) bVR = true;            // (the headset's rules, this run only)
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wristcrash check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wristcrash RESULT %s (%d passed, %d failed, %s, %s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			bOld ? TEXT("-OLOldWristCache") : TEXT("new"), bVRMode ? TEXT("vr") : TEXT("flat"));
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	// every texture alive now (not garbage): a pointer is looked up in it, never followed, so a freed one is safe to test
	auto LiveTextures = []()
	{
		TSet<const UTexture*> Live;
		for (TObjectIterator<UTexture> It; It; ++It) if (IsValid(*It)) Live.Add(*It);
		return Live;
	};
	auto Hit = [&](float Damage) { L->bInvincible = false; L->DamagePlayer(Damage, 1.f, true); L->bInvincible = true; };
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 4.f || !bUp) return;
		if (!bHooked)
		{
			bHooked = true;
			FCoreUObjectDelegates::GetPostGarbageCollect().AddLambda([]() { ++WcGcCount; });
		}
		FString Mode;
		OLFlags::Value(TEXT("OLWristCrashProbe"), Mode);
		bVRMode = Mode.Equals(TEXT("vr"), ESearchCase::IgnoreCase);
		L->bInvincible = true;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wristcrash start: %s, %s, %s, remastered sprites %s"), *L->LevelName, Tag, bVRMode ? TEXT("vr") : TEXT("flat"), L->bHDTextures ? TEXT("on") : TEXT("off"));
		Round = 0;
		ShotStep = 1;
		return;
	}
	case 1:
	{
		// the plate worn (both cards), the remastered sprites flipped and back so each card's two pictures are cached
		if (L->GetArmour() <= 0) L->AddCount(TEXT("ibplate"), 50);
		L->SetHDTextures(!L->bHDTextures);
		WristLast.Reset();
		Wait = ShotTime + 0.5f;
		ShotStep = 2;
		return;
	}
	case 2:
	{
		if (ShotTime < Wait) return;
		L->SetHDTextures(!L->bHDTextures);
		WristLast.Reset();
		Wait = ShotTime + 0.5f;
		ShotStep = 3;
		return;
	}
	case 3:
	{
		if (ShotTime < Wait) return;
		Noted = WristIconCache;
		FString Keys;
		for (const TPair<FString, FWristIconHeld>& Kv : Noted) Keys += TEXT(" ") + Kv.Key;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wristcrash round %d: %d cached before the restart:%s (shown '%s')"), Round + 1, Noted.Num(), *Keys, *WristShownKeys);
		// (the raw cache keeps them through the restart; it is emptied here so nothing in the run hands a freed one to a card:
		// the noted copy shows what it would have held)
		if (bOld) WristIconCache.Reset();
		L->RestartLevel();
		Wait = ShotTime + 1.f;
		ShotStep = 4;
		return;
	}
	case 4:
	{
		if (ShotTime < Wait || !bUp) return;
		GcMark = WcGcCount;
		if (GEngine) GEngine->ForceGarbageCollection(true);
		Wait = ShotTime + 0.3f;
		ShotStep = 5;
		return;
	}
	case 5:
	{
		if (ShotTime < Wait || WcGcCount == GcMark) return;
		const int32 R = Round + 1;
		// (1) what the restart freed, from the weak side
		int32 Gone = 0;
		FString GoneKeys;
		for (const TPair<FString, FWristIconHeld>& Kv : Noted)
		{
			if (!Kv.Value.Src.Tex || Kv.Value.Live.IsValid()) continue;
			++Gone;
			GoneKeys += TEXT(" ") + Kv.Key;
		}
		Check(Gone > 0, FString::Printf(TEXT("round %d (1): the restart and a GC freed %d of the %d textures cached before it:%s"), R, Gone, Noted.Num(), *GoneKeys));
		// (2) the player's own cache now
		FString StaleKeys;
		const int32 Stale = WristIconCacheStale(&StaleKeys);
		if (bOld) { UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wristcrash round %d (2) -OLOldWristCache: the cache (emptied by the probe) holds %d stale"), R, Stale); }
		else Check(Stale == 0, FString::Printf(TEXT("round %d (2): nothing stale in the wrist's cache after the restart (%d of %d: %s)"), R, Stale, WristIconCache.Num(), *StaleKeys));
		// (3) the noted entries put back and asked for again, as a relayout would
		WristIconCache = Noted;
		const FString Suffix = L->bHDTextures ? TEXT("#hd") : TEXT("#sd");
		const int32 RebuiltWas = WristIconRebuilt;
		int32 Asked = 0, Freed = 0, LiveOut = 0;
		FString Lines;
		TArray<TPair<FString, const UTexture*>> Out;          // (a rebuild may make the HD atlas: alive is read after them all)
		for (const TPair<FString, FWristIconHeld>& Kv : Noted)
		{
			if (!Kv.Key.EndsWith(Suffix) || !Kv.Value.Src.Tex || Kv.Value.Live.IsValid()) continue;
			FWristIconSrc Src;
			WristIcon(Kv.Key.LeftChop(3), Src);
			++Asked;
			const bool bFreed = Src.Tex && Src.Tex == Kv.Value.Src.Tex;          // (the dead one itself: compared, not used)
			Freed += bFreed ? 1 : 0;
			Out.Emplace(Kv.Key, bFreed ? nullptr : Src.Tex);
			if (bFreed) Lines += FString::Printf(TEXT(" %s: the freed texture handed out;"), *Kv.Key);
		}
		const TSet<const UTexture*> Live = LiveTextures();
		for (const TPair<FString, const UTexture*>& O : Out)
		{
			if (!O.Value) continue;
			const bool bLive = Live.Contains(O.Value);
			LiveOut += bLive ? 1 : 0;
			Lines += FString::Printf(TEXT(" %s: %s;"), *O.Key, bLive ? TEXT("rebuilt, a live texture") : TEXT("a texture not alive"));
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wristcrash round %d (3) the stale entries asked for again (%d, %d rebuilt):%s"), R, Asked, WristIconRebuilt - RebuiltWas, *Lines);
		if (bOld)
		{
			Check(Asked > 0 && Freed == Asked, FString::Printf(TEXT("round %d (3) -OLOldWristCache: as before, the raw cache hands out the freed texture, %d of %d (the crash)"), R, Freed, Asked));
			WristIconCache.Reset();          // (never laid out: the run would use them)
		}
		else Check(Asked > 0 && LiveOut == Asked && WristIconRebuilt - RebuiltWas == Asked,
			FString::Printf(TEXT("round %d (3): every stale entry rebuilt, a live texture handed out, none freed: %d of %d live, %d freed"), R, LiveOut, Asked, Freed));
		// (4) a relayout: the remastered sprites flipped (the menu's toggle) and a hit
		if (L->GetArmour() <= 0) L->AddCount(TEXT("ibplate"), 50);
		L->SetHDTextures(!L->bHDTextures);
		Hit(0.5f);
		WristLast.Reset();
		Wait = ShotTime + 0.6f;
		ShotStep = 6;
		return;
	}
	case 6:
	{
		if (ShotTime < Wait) return;
		const int32 R = Round + 1;
		const TSet<const UTexture*> Live = LiveTextures();
		int32 Cards = 0, LiveCards = 0, Shown = 0;
		UTexture* HeartTex = nullptr;
		for (int32 i = 0; i < WristIconsUsed && i < WristIcons.Num(); ++i)
		{
			UStaticMeshComponent* P = WristIcons[i];
			Shown += P && P->IsVisible() ? 1 : 0;
			UMaterialInstanceDynamic* M = P ? Cast<UMaterialInstanceDynamic>(P->GetMaterial(0)) : nullptr;
			UTexture* T = nullptr;
			if (M) M->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Tex")), T);
			++Cards;
			LiveCards += T && Live.Contains(T) ? 1 : 0;
			if (i == 0) HeartTex = T;
		}
		UTexture* Want = nullptr;
		FVector2D Sc, Of;
		float Asp = 1.f;
		L->SpriteIcon(TEXT("intheart.nwx"), Want, Sc, Of, Asp);
		Check(Cards > 0 && LiveCards == Cards, FString::Printf(TEXT("round %d (4): after the relayout every card's texture is a live object: %d of %d (remastered sprites %s, health %.0f%%, armour %d)"),
			R, LiveCards, Cards, L->bHDTextures ? TEXT("on") : TEXT("off"), L->GetHealth() * 10.f, L->GetArmour()));
		Check(HeartTex && HeartTex == Want, FString::Printf(TEXT("round %d (4): the heart's card shows the level's own picture now (%s)"), R, HeartTex && HeartTex == Want ? *HeartTex->GetName() : TEXT("another")));
		Check(WristShownKeys == TEXT("nwx:intheart nwx:ibplate") && Shown == 2 && WristRoot && WristRoot->IsVisible(),
			FString::Printf(TEXT("round %d (4): the heart and plate cards on the wrist ('%s', %d shown): %s"), R, *WristShownKeys, Shown, *WristRowsLog));
		if (Round == 0)
		{
			const FString Name = FString::Printf(TEXT("ol_wristcrash_%s_%s"), Tag, bVRMode ? TEXT("vr") : TEXT("flat"));
			FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wristcrash shot %s.png"), *Name);
		}
		Wait = ShotTime + 0.5f;
		if (++Round >= Rounds) { ShotStep = 7; return; }
		ShotStep = 1;
		return;
	}
	case 7:
	{
		if (ShotTime < Wait) return;
		Finish();
		return;
	}
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
