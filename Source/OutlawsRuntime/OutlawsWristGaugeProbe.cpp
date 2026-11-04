// REQ-WRISTGAUGE (the user after his Quest test, 4 Nov 2026: the HUD's thermometer on the left wrist under health and armour,
// tube only): -OLShots -OLLevel=cliff -OLNoFrontEnd -OLWristGaugeProbe[=<tag>] [-OLOldWristGauge].  The wrist held in front of
// the camera (as -OLWristTest).  With the remastered sprites off, then on: stamina full, half, empty; the head really under
// the level's deepest water with the air full, half, low; out again.  Each case: the head where it should be, the tube's two
// cards (red or blue, and the cream), the level drawn, each card's picture the level's own, the two parts meeting, under the
// last row of cards; each photographed (ol_wristgauge_<tag>_<sd|hd>_<case>.png).  Then, once: the tube following stamina as it
// comes back by itself, the plate worn (the tube a row lower), then BUG-WRISTCRASH's recipe: a level restart with a GC (the
// cards as they stand hold live pictures: a card keeps the atlas it shows alive, the heart's too), the remastered sprites
// off and on with a GC between (each time the level's own picture, rebuilt from the level as it is now).  -OLOldWristGauge: no
// tube at all, the rows as they were.  Lines "OLSHOT wristgauge check PASS|FAIL: ..." and "OLSHOT wristgauge RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/Texture.h"
#include "Engine/Texture2D.h"
#include "Engine/Engine.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UObject/UObjectIterator.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLWristGaugeProbe) { return OLFlags::Has(TEXT("OLWristGaugeProbe")); }

OL_SHOT_TEST(OLWristGaugeProbe, 2400)
{
	struct FCase { const TCHAR* Name; bool bUnder; float Breath; float AirLeft; };
	// (the one not shown set far from the one shown, so the wrong one reads wrong)
	static const FCase GaugeCases[] =
	{
		{ TEXT("stamina_full"), false, 100.f, 0.2f },
		{ TEXT("stamina_half"), false, 50.f, 0.2f },
		{ TEXT("stamina_empty"), false, 0.f, 1.f },
		{ TEXT("under_air_full"), true, 30.f, 1.f },
		{ TEXT("under_air_half"), true, 100.f, 0.5f },
		{ TEXT("under_air_low"), true, 100.f, 0.12f },
		{ TEXT("out_again"), false, 40.f, 0.9f },
	};
	constexpr int32 NumCases = UE_ARRAY_COUNT(GaugeCases);
	constexpr float Neck = 0.13f, TubeW = 8.f, IconH = 2.6f;      // (LayoutWrist's)
	static int32 Passes = 0, Fails = 0, Sec = -1, Pass = 0, Case = 0;
	static float Wait = 0.f;
	static FVector DryAt = FVector::ZeroVector;
	static FString Tag;
	static bool bHolding = false;
	if (!L || ShotStep < 0) return;
	const bool bOld = OLFlags::Param(TEXT("OLOldWristGauge"));
	L->bInvincible = true;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wristgauge check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wristgauge RESULT %s (%d passed, %d failed, %s, %s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			bOld ? TEXT("-OLOldWristGauge") : TEXT("new"), *Tag);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	auto Shoot = [&](const FString& What)
	{
		const FString Name = FString::Printf(TEXT("ol_wristgauge_%s_%s_%s"), *Tag, L->bHDTextures ? TEXT("hd") : TEXT("sd"), *What);
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wristgauge shot %s.png"), *Name);
	};
	auto TexOf = [&](int32 Part) -> UTexture*
	{
		UTexture* T = nullptr;
		UStaticMeshComponent* P = WristGauge.IsValidIndex(Part) ? WristGauge[Part].Get() : nullptr;
		UMaterialInstanceDynamic* M = P ? Cast<UMaterialInstanceDynamic>(P->GetMaterial(0)) : nullptr;
		if (M) M->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Tex")), T);
		return T;
	};
	// a part's left and right edge, in cm right of the panel's middle, and its height in cm
	auto Span = [&](int32 Part, float& Left, float& Right, float& Up)
	{
		const UStaticMeshComponent* P = WristGauge[Part];
		const FVector At = P->GetRelativeLocation();
		const float W = P->GetRelativeScale3D().X * 100.f;
		Left = -At.Y - W * 0.5f; Right = -At.Y + W * 0.5f; Up = At.Z;
	};
	// the checks of a case: the tube as laid out against what the player's breath and air say now
	auto Judge = [&](const TCHAR* Name, bool bBlue, float Frac, bool bLevelsOwn = true)
	{
		const FString Say = FString::Printf(TEXT("%s, %s"), Name, L->bHDTextures ? TEXT("hd") : TEXT("sd"));
		if (bOld)
		{
			Check(WristGauge.Num() == 0 && WristGaugeKeys.IsEmpty() && WristGaugeLevel < 0.f, FString::Printf(TEXT("%s: no tube with -OLOldWristGauge (%d parts, '%s')"), *Say, WristGauge.Num(), *WristGaugeKeys));
			return;
		}
		const FString WantKeys = bBlue ? TEXT("tube:3 tube:2") : TEXT("tube:1 tube:2");
		const float WantLevel = Neck + (1.f - Neck) * Frac;
		Check(WristGaugeKeys == WantKeys, FString::Printf(TEXT("%s: the cards '%s' (want '%s')"), *Say, *WristGaugeKeys, *WantKeys));
		Check(FMath::Abs(WristGaugeLevel - WantLevel) <= 1.5f / WristGaugeSteps, FString::Printf(TEXT("%s: the tube drawn to %.3f of its length (want %.3f)"), *Say, WristGaugeLevel, WantLevel));
		if (WristGauge.Num() < 2)
		{
			Check(false, FString::Printf(TEXT("%s: the tube's two parts made (%d)"), *Say, WristGauge.Num()));
			return;
		}
		// each part's picture: the level's own, the atlas of the chosen look
		const int32 Chors[2] = { bBlue ? 3 : 1, 2 };
		for (int32 i = 0; i < 2; ++i)
		{
			UTexture* Want = nullptr;
			FVector2D Sc, Of;
			float Asp = 1.f;
			const bool bArt = L->SpriteIcon(TEXT("thermom.nwx"), Want, Sc, Of, Asp, Chors[i]);
			UTexture* Got = TexOf(i);
			Check(bArt && Got && (Got == Want || !bLevelsOwn) && FMath::IsNearlyEqual(Asp, 92.f / 14.f, 0.01f) && (Cast<UTexture2D>(Got) != nullptr) == !L->bHDTextures,
				FString::Printf(TEXT("%s: part %d's picture is chor %d of the level's atlas (%s, the level's %s, cell at %.2f %.2f, aspect %.2f)"), *Say, i, Chors[i], Got ? *Got->GetName() : TEXT("none"), Want ? *Want->GetName() : TEXT("none"), Of.X, Of.Y, Asp));
			Check(WristGauge[i]->IsVisible(), FString::Printf(TEXT("%s: part %d shown"), *Say, i));
		}
		float L0, R0, U0, L1, R1, U1;
		Span(0, L0, R0, U0);
		Span(1, L1, R1, U1);
		const float Full = FMath::Max(R0, R1) - L0;
		Check(FMath::IsNearlyEqual(Full, TubeW, 0.02f) && L1 <= R0 + 0.01f && R0 - L1 < 0.06f && FMath::IsNearlyEqual(U0, U1, 0.01f),
			FString::Printf(TEXT("%s: the parts meet (colour %.2f..%.2f, cream %.2f..%.2f, %.2f cm long)"), *Say, L0, R0, L1, R1, Full));
		const float TubeTop = U0 + 0.5f * TubeW / (92.f / 14.f);
		const float RowBottom = WristIconsUsed > 0 ? WristIcons[WristIconsUsed - 1]->GetRelativeLocation().Z - IconH * 0.5f : 1e6f;
		Check(TubeTop < RowBottom - 0.2f, FString::Printf(TEXT("%s: the tube under the rows (its top %.2f, the last row's bottom %.2f; rows: %s)"), *Say, TubeTop, RowBottom, *WristRowsLog));
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	// the values of the case, held against the breath's own running up and down, laid out again at once
	if (bHolding)
	{
		const FCase& C = GaugeCases[Case];
		Stamina = C.Breath;
		Air = C.AirLeft;
		LayoutWrist();
	}
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 4.f || !bUp) return;
		OLFlags::Value(TEXT("OLWristGaugeProbe"), Tag);
		if (Tag.IsEmpty()) Tag = bOld ? TEXT("old") : TEXT("pc");
		Sec = L->DebugWaterSector();
		DryAt = GetActorLocation();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wristgauge start: %s, %s, water sector %d, remastered sprites %s"), *L->LevelName, *Tag, Sec, L->bHDTextures ? TEXT("on") : TEXT("off"));
		Check(Sec >= 0, FString::Printf(TEXT("the level %s has water (sector %d)"), *L->LevelName, Sec));
		L->SetHDTextures(false);
		WristLast.Reset();
		Pass = 0;
		Case = 0;
		ShotStep = 1;
		return;
	}
	case 1:
	{
		// a case begins: the head where the case wants it
		const FCase& C = GaugeCases[Case];
		FVector Feet;
		float SlopeYaw = 0.f;
		if (C.bUnder && !bHeadUnder && Sec >= 0 && L->SlopeFoot(Sec, Feet, SlopeYaw)) TeleportTo(Feet + FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0), GetActorRotation(), false, true);
		if (!C.bUnder && bHeadUnder) TeleportTo(DryAt, GetActorRotation(), false, true);
		bHolding = true;
		Wait = ShotTime + 0.8f;
		ShotStep = 2;
		return;
	}
	case 2:
	{
		if (ShotTime < Wait) return;
		const FCase& C = GaugeCases[Case];
		Check(bHeadUnder == C.bUnder, FString::Printf(TEXT("%s, %s: the head %s (eyes at %.0f %.0f %.0f)"), C.Name, L->bHDTextures ? TEXT("hd") : TEXT("sd"), bHeadUnder ? TEXT("under") : TEXT("out"), Camera->GetComponentLocation().X, Camera->GetComponentLocation().Y, Camera->GetComponentLocation().Z));
		Judge(C.Name, C.bUnder, C.bUnder ? C.AirLeft : C.Breath / 100.f);
		Shoot(C.Name);
		Wait = ShotTime + 0.3f;
		ShotStep = 3;
		return;
	}
	case 3:
	{
		if (ShotTime < Wait) return;
		if (++Case < NumCases) { ShotStep = 1; return; }
		bHolding = false;
		Case = 0;
		if (!bOld && ++Pass < 2)
		{
			L->SetHDTextures(true);
			WristLast.Reset();
			ShotStep = 1;
			return;
		}
		// the tube following by itself, nothing held
		Stamina = 30.f;
		Wait = ShotTime + 1.5f;
		ShotStep = 4;
		return;
	}
	case 4:
	{
		if (ShotTime < Wait) return;
		Judge(TEXT("following"), false, StaminaFrac());
		Check(Stamina > 30.f, FString::Printf(TEXT("following: stamina came back by itself to %.1f"), Stamina));
		if (L->GetArmour() <= 0) L->AddCount(TEXT("ibplate"), 50);
		Wait = ShotTime + 0.5f;
		ShotStep = 5;
		return;
	}
	case 5:
	{
		if (ShotTime < Wait) return;
		Judge(TEXT("plate_worn"), false, StaminaFrac());
		Check(WristShownKeys == TEXT("nwx:intheart nwx:ibplate"), FString::Printf(TEXT("the rows' cards are the heart and the plate ('%s')"), *WristShownKeys));
		Shoot(TEXT("plate_worn"));
		Wait = ShotTime + 0.3f;
		ShotStep = 6;
		return;
	}
	case 6:
	{
		if (ShotTime < Wait) return;
		L->RestartLevel();
		Wait = ShotTime + 1.f;
		ShotStep = 7;
		return;
	}
	case 7:
	{
		if (ShotTime < Wait || !bUp) return;
		GEngine->ForceGarbageCollection(true);
		// (a hit, so the wrist lays out again from whatever the restart left, as BUG-WRISTCRASH's probe does)
		L->bInvincible = false; L->DamagePlayer(1.f, 1.f, true); L->bInvincible = true;
		Wait = ShotTime + 1.f;
		ShotStep = 8;
		return;
	}
	case 8:
	{
		if (ShotTime < Wait) return;
		Judge(TEXT("after_restart"), false, StaminaFrac(), false);
		if (bOld) { Finish(); return; }
		TSet<const UTexture*> Live;
		for (TObjectIterator<UTexture> It; It; ++It) if (IsValid(*It)) Live.Add(*It);
		Check(Live.Contains(TexOf(0)) && Live.Contains(TexOf(1)), TEXT("after_restart: both parts' pictures are live objects"));
		// (what the heart's card, the older one, does here, for the log)
		UTexture* HeartHas = nullptr, *HeartOwn = nullptr;
		FVector2D Sc, Of;
		float Asp = 1.f;
		UMaterialInstanceDynamic* M = WristIcons.Num() > 0 ? Cast<UMaterialInstanceDynamic>(WristIcons[0]->GetMaterial(0)) : nullptr;
		if (M) M->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Tex")), HeartHas);
		L->SpriteIcon(TEXT("intheart.nwx"), HeartOwn, Sc, Of, Asp);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wristgauge after_restart: the heart's card holds %s, the level's is %s"), HeartHas ? *HeartHas->GetName() : TEXT("none"), HeartOwn ? *HeartOwn->GetName() : TEXT("none"));
		L->SetHDTextures(false);
		WristLast.Reset();
		Wait = ShotTime + 1.f;
		ShotStep = 9;
		return;
	}
	case 9:
	{
		if (ShotTime < Wait) return;
		Judge(TEXT("after_restart_off"), false, StaminaFrac());
		GEngine->ForceGarbageCollection(true);
		Wait = ShotTime + 1.f;
		ShotStep = 10;
		return;
	}
	case 10:
	{
		if (ShotTime < Wait) return;
		L->SetHDTextures(true);
		WristLast.Reset();
		Wait = ShotTime + 1.f;
		ShotStep = 11;
		return;
	}
	case 11:
	{
		if (ShotTime < Wait) return;
		Judge(TEXT("after_restart_on"), false, StaminaFrac());
		Finish();
		return;
	}
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
