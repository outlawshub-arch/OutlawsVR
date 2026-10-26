// -OLShots -OLTreadTest[=sector] (BUG-SURFACEJOLT, user 2026-10-12: treading at the surface, a sudden jolt a couple of feet
// under it and a swim back up, most when trying to swim up; seen on the cliff level in sector 350 while firing the right
// revolver). The player is put at the surface of a water sector (the one named, else the level's deepest) and held there
// through these phases, each a few seconds:
//   idle      nothing pressed;
//   swimup    strokes with the head up, the jump (the turn stick up) every half second;
//   strokes   both arms swept hard (a swim-up push) while looking down at the water (the head 55 degrees down);
//   fire      the right revolver fired every 0.4 s with the hand swung after each shot, the head 40 degrees down;
//   wander    the stick forward, out and back each way 1.5 s a leg (across the sector and its seams);
//   flick     (BUG-SUBMERGE) the head 45 degrees down, one hand pulled back for a tenth of a second every 0.8 s (a short
//             swing, no stroke): held;
//   dive      (the control) the stick forward with the head 45 degrees down: this one must still let go and go under;
//   strokedive (BUG-SUBMERGE, user 2026-10-14) both arms stroking forward (the hands pulled back past the body) with the head
//             45 degrees down, no stick: it must let go and go under, as before BUG-SURFACEJOLT.
// Every frame the body's middle is measured against the float depth under the surface (read from the top of the water
// stack there); a phase fails if the middle goes more than 12 cm under it or the hold lets go. The water sector itself is
// logged first ("OLSHOT tread sector": its stack and currents, and where the old reading missed the surface).

#include "OutlawsPlayer.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "MotionControllerComponent.h"
#include "UnrealClient.h"
#include "Materials/Material.h"
#include "MaterialShared.h"
#include "Engine/Engine.h"
#include "StereoRendering.h"
#include "Components/TextRenderComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "ProceduralMeshComponent.h"
#include "OutlawsShotTests.h"

#if WITH_OUTLAWS_TESTS

extern bool GSwimStrokeTest;

namespace { constexpr float TreadStandHalf = 87.f; }   // (OutlawsPlayer.cpp's: a standing body's half height, the swim's "middle")

void AOutlawsPlayer::TreadTestTick(float Dt)
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L) return;
	struct FPhase { const TCHAR* Name; float Seconds; float Pitch; };
	static const FPhase Phases[] = {
		{ TEXT("idle"), 3.f, 0.f }, { TEXT("swimup"), 3.f, 40.f }, { TEXT("strokes"), 3.f, -55.f },
		{ TEXT("fire"), 4.f, -40.f }, { TEXT("wander"), 6.f, -10.f }, { TEXT("flick"), 3.f, -45.f }, { TEXT("dive"), 1.5f, -45.f },
		{ TEXT("strokedive"), 1.5f, -45.f } };
	static int32 Phase = 0, Fails = 0, Sec = -1, LetGo = 0, Exits = 0, Shots = 0;
	static float At = 0.f, MinUnder = 0.f, NextAct = 0.f, Yaw0 = 0.f;
	static FVector Start = FVector::ZeroVector;
	static bool bWasHeld = false, bWasSwimming = false;
	UCharacterMovementComponent* M = GetCharacterMovement();
	auto Middle = [this]() { return float(GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight()) + TreadStandHalf; };
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT tread check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (!bOk) ++Fails;
	};
	auto Finish = [this]()
	{
		GSwimStrokeTest = false;
		HandVelocity[0] = HandVelocity[1] = FVector::ZeroVector;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT tread RESULT %s (%d failed)"), Fails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Fails);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	};
	auto PlaceAtSurface = [&]()
	{
		FVector Feet;
		if (!L->DebugPointInSector(Sec, Feet)) return false;
		float Surface = 0.f;
		int32 Top = -1;
		if (!L->WaterSurfaceAt(Feet - FVector(0.0, 0.0, 10.0), Surface, Top)) return false;
		// the middle 50 cm under the surface (the swim takes it from 10 under), the hold eases it to its float depth
		const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		Start = FVector(Feet.X, Feet.Y, Surface - 50.f - TreadStandHalf + Half);
		TeleportTo(Start, GetActorRotation(), false, true);
		M->Velocity = FVector::ZeroVector;
		return true;
	};
	if (ShotStep == 0)
	{
		if (ShotTime < 3.f) return;
		FString Arg;
		Sec = OLFlags::Value(TEXT("OLTreadTest"), Arg) && Arg.IsNumeric() ? FCString::Atoi(*Arg) : L->DebugWaterSector();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT tread sector: %s"), *L->DebugWaterSectorInfo(Sec));
		FString Also;
		if (OLFlags::Value(TEXT("OLTreadDump"), Also, false))
		{
			TArray<FString> Ids;
			Also.ParseIntoArray(Ids, TEXT(","));
			for (const FString& Id : Ids) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT tread sector: %s"), *L->DebugWaterSectorInfo(FCString::Atoi(*Id)));
		}
		if (!PlaceAtSurface())
		{
			Check(false, FString::Printf(TEXT("a place at the surface of water sector %d"), Sec));
			Finish();
			return;
		}
		Equip(1, 0);                                    // the revolver in the right hand (the fire phase)
		bUnlimitedAmmo = true;
		GSwimStrokeTest = true;                         // (the strokes run without a headset)
		Yaw0 = float(GetActorRotation().Yaw);
		At = ShotTime;
		ShotStep = 1;
	}
	else if (ShotStep == 1)
	{
		// settle: in the swim and held
		if (ShotTime < At + 2.f) return;
		float Surface = 0.f;
		int32 Top = -1;
		L->WaterSurfaceAt(FVector(GetActorLocation().X, GetActorLocation().Y, Middle()), Surface, Top);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT tread settled: swimming %d, held %d, middle %.0f cm under the surface z %.0f (top water sector %d; float depth %.0f)"),
			bSwimming ? 1 : 0, IsSurfaceHeld() ? 1 : 0, Surface - Middle(), Surface, Top, SwimFloatDepth);
		Check(bSwimming && IsSurfaceHeld(), TEXT("settled in the swim, held at the surface"));
		Phase = 0;
		ShotStep = 2;
		At = ShotTime;
		MinUnder = 0.f; LetGo = Exits = Shots = 0; NextAct = 0.f;
		bWasHeld = true; bWasSwimming = true;
	}
	else if (ShotStep == 2)
	{
		const FPhase& Ph = Phases[Phase];
		const float T = ShotTime - At;
		const bool bDive = FCString::Strcmp(Ph.Name, TEXT("dive")) == 0;
		const bool bStrokeDive = FCString::Strcmp(Ph.Name, TEXT("strokedive")) == 0;
		float Yaw = Yaw0;
		static const float Legs[4] = { 0.f, 180.f, 90.f, 270.f };              // out and back each way: it stays near the start
		if (FCString::Strcmp(Ph.Name, TEXT("wander")) == 0) Yaw = Yaw0 + Legs[int32(T / 1.5f) % 4];
		if (Controller) Controller->SetControlRotation(FRotator(Ph.Pitch, Yaw, 0.0));
		// the phase's input
		HandVelocity[0] = HandVelocity[1] = FVector::ZeroVector;
		if (FCString::Strcmp(Ph.Name, TEXT("swimup")) == 0)
		{
			HandVelocity[0] = HandVelocity[1] = FVector(0.0, 0.0, -220.0);     // strokes with the head up
			if (T >= NextAct) { DoJump(); NextAct = T + 0.5f; }
		}
		else if (FCString::Strcmp(Ph.Name, TEXT("strokes")) == 0)
		{
			HandVelocity[0] = HandVelocity[1] = FVector(0.0, 0.0, -260.0);     // pushing the water down: a swim-up stroke
		}
		else if (FCString::Strcmp(Ph.Name, TEXT("fire")) == 0)
		{
			if (T >= NextAct) { FireGun(1, false); ++Shots; NextAct = T + 0.4f; }
			if (T < NextAct - 0.25f) HandVelocity[1] = GetActorForwardVector() * 300.0;   // the swing after each shot
		}
		else if (FCString::Strcmp(Ph.Name, TEXT("flick")) == 0)
		{
			if (FMath::Fmod(T, 0.8f) < 0.1f) HandVelocity[0] = -FRotator(0.0, Yaw, 0.0).Vector() * 250.0;   // a short pull back
		}
		else if (bStrokeDive)
		{
			HandVelocity[0] = HandVelocity[1] = -FRotator(0.0, Yaw, 0.0).Vector() * 250.0;                  // pulled back: a forward stroke
		}
		else if (FCString::Strcmp(Ph.Name, TEXT("wander")) == 0 || bDive)
		{
			HoldMoveStick(FVector2D(0.0, 1.0), 0.2f);
		}
		// the measure
		float Surface = 0.f;
		int32 Top = -1;
		if (L->WaterSurfaceAt(FVector(GetActorLocation().X, GetActorLocation().Y, Middle()), Surface, Top))
		{
			const float Under = (Surface - SwimFloatDepth) - Middle();            // how far under its float depth
			if (T > 0.1f) MinUnder = FMath::Max(MinUnder, Under);
		}
		if (bWasHeld && !IsSurfaceHeld() && bSwimming) ++LetGo;          // (out over a bank's edge by the jump is no let-go)
		if (bWasSwimming && !bSwimming) ++Exits;
		bWasHeld = IsSurfaceHeld();
		bWasSwimming = bSwimming;
		if (T < Ph.Seconds) return;
		HoldMoveStick(FVector2D::ZeroVector, 0.f);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT tread phase %s: %.1f s, the middle at most %.1f cm under its float depth, the hold let go %d times, out of the swim %d times%s, held at the end %d"),
			Ph.Name, Ph.Seconds, MinUnder, LetGo, Exits, Shots > 0 ? *FString::Printf(TEXT(", %d shots"), Shots) : TEXT(""), IsSurfaceHeld() ? 1 : 0);
		if (bDive || bStrokeDive)
		{
			Check(LetGo > 0 && MinUnder > 25.f, FString::Printf(TEXT("%s: %s with the head down lets go and goes under (%.0f cm under its float depth)"),
				Ph.Name, bDive ? TEXT("the stick") : TEXT("a forward stroke"), MinUnder));
			if (bStrokeDive)
			{
				Finish();
				return;
			}
		}
		// (swimup: a kick up from the surface is the jump's; out of the swim for a moment is not a fail, a fall back in is)
		else Check(LetGo == 0 && MinUnder <= 12.f, FString::Printf(TEXT("%s: no snap under the surface (%.1f cm under its float depth at most, let go %d times)"), Ph.Name, MinUnder, LetGo));
		// back to the start for the next phase
		++Phase;
		if (FCString::Strcmp(Phases[Phase].Name, TEXT("idle")) != 0) TeleportTo(Start, FRotator(0.0, Yaw0, 0.0), false, true);
		M->Velocity = FVector::ZeroVector;
		At = ShotTime + 1.0f;                           // a second to settle (not measured)
		ShotStep = 3;
	}
	else if (ShotStep == 3)
	{
		if (Controller) Controller->SetControlRotation(FRotator(0.0, Yaw0, 0.0));
		HandVelocity[0] = HandVelocity[1] = FVector::ZeroVector;
		if (ShotTime < At) return;
		At = ShotTime;
		MinUnder = 0.f; LetGo = Exits = Shots = 0; NextAct = 0.f;
		bWasHeld = IsSurfaceHeld(); bWasSwimming = bSwimming;
		if (!bWasHeld) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT tread: not held before phase %s (swimming %d)"), Phases[Phase].Name, bSwimming ? 1 : 0);
		ShotStep = 2;
	}
}

OL_SHOT_MATCH(OLTreadTest) { return (OLFlags::Has(TEXT("OLTreadTest"))); }
OL_SHOT_TEST(OLTreadTest, 170)
{
	TreadTestTick(Dt);               // (BUG-SURFACEJOLT: OutlawsPlayerTreadTest.cpp)
}

// ---- -OLShots -OLWaterHandsTest[=sector] (BUG-WATERHANDS) ----------------------------------------------------------------
// The held gear is pulled toward the eyes so walls never cut it (M_OLR_Held's OLNearPull); a part on the far side of a water
// surface keeps its true depth so the surface draws over it, when UpdateHandWater finds a surface between the eyes and the
// hand (MPC_OLR_Lights WaterOn / WaterZ). 1: every water surface of the level, the eyes 30 cm over it and a hand 40 cm under
// it: found (and how many the old rule found); 2: at the surface of a water sector, looking down, the revolver held 35 cm
// under the surface: the exception on (the shot "ol_waterhands_on"), then forced off as before the fix ("_off"), then the
// hand over the water ("_dry")

void AOutlawsPlayer::WaterHandsTestTick(float Dt)
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L) return;
	static int32 Fails = 0, Sec = -1, Round = 0, Tries = 0;
	static float At = 0.f, Surface = 0.f, Quiet = -1.f;
	static FVector Spot = FVector::ZeroVector;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT waterhands check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (!bOk) ++Fails;
	};
	auto Finish = [this]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT waterhands RESULT %s (%d failed)"), Fails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Fails);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	};
	const TCHAR* Fl = GetWorld()->FeatureLevel <= ERHIFeatureLevel::ES3_1 ? TEXT("es31") : TEXT("sm5");
	if (ShotStep == 0)
	{
		// (the headset's path compiles its shaders first: a hitch while the body is put in the water dropped it in at speed)
		if (ShotTime < 3.f || !TestShadersReady(Quiet, 1.f)) return;
		int32 N = 0, Found = 0, Old = 0;
		const FString Missed = L->DebugWaterSurfaceCheck(N, Found, Old);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT waterhands surfaces on %s: %d water surfaces, the surface found between the eyes and a hand under it on %d (the old rule: %d); %s"),
			*L->LevelName, N, Found, Old, *Missed);
		Check(N == 0 || Found == N, FString::Printf(TEXT("every water surface found between the eyes and a hand under it (%d of %d)"), Found, N));
		FString Arg;
		Sec = OLFlags::Value(TEXT("OLWaterHandsTest"), Arg) && Arg.IsNumeric() ? FCString::Atoi(*Arg) : L->DebugWaterSector();
		FVector Feet;
		int32 Top = -1;
		if (Sec < 0 || !L->DebugPointInSector(Sec, Feet) || !L->WaterSurfaceAt(Feet - FVector(0.0, 0.0, 10.0), Surface, Top))
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT waterhands: no water sector to stand in (%d)"), Sec);
			Finish();
			return;
		}
		const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		Spot = FVector(Feet.X, Feet.Y, Surface - 50.f - TreadStandHalf + Half);
		TeleportTo(Spot, GetActorRotation(), false, true);
		GetCharacterMovement()->Velocity = FVector::ZeroVector;
		Equip(1, 0);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT waterhands: in water sector %d (top %d), surface z %.0f"), Sec, Top, Surface);
		At = ShotTime;
		ShotStep = 1;
		return;
	}
	if (ShotStep == 1 && ShotTime < At + 2.5f) return;
	if (ShotStep == 1 && !IsSurfaceHeld() && ++Tries <= 3)
	{
		// not held at the surface yet (a frame's hitch as it went in): put back and wait again
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT waterhands: not held at the surface after the wait (swimming %d): put back (try %d)"), bSwimming ? 1 : 0, Tries);
		TeleportTo(Spot, GetActorRotation(), false, true);
		GetCharacterMovement()->Velocity = FVector::ZeroVector;
		At = ShotTime;
		return;
	}
	// rounds: 0 the hand under the water, the exception as the runtime finds it; 1 the same, forced off (before the fix);
	// 2 the hand over the water
	if (Controller) Controller->SetControlRotation(FRotator(-55.0, GetActorRotation().Yaw, 0.0));
	const FVector Eye = Camera->GetComponentLocation();
	FVector Ahead = Camera->GetForwardVector();
	Ahead.Z = 0.0;
	Ahead = Ahead.GetSafeNormal();
	const float HandZ = Round == 2 ? FMath::Max(Surface + 15.f, float(Eye.Z) - 30.f) : Surface - 35.f;
	const FVector HandAt(Eye.X + Ahead.X * 45.0, Eye.Y + Ahead.Y * 45.0, HandZ);
	if (UMotionControllerComponent* G = GripOf(1)) G->SetWorldLocation(HandAt);
	FVector Hit = FVector::ZeroVector;
	const bool bAcross = L->WaterSurfaceHit(Eye, HandAt, Hit);
	SetHandWater(Round == 1 ? false : bAcross, bAcross ? float(Hit.Z) : 0.f);
	HandWaterWant[1] = Round != 1 && bAcross && HandAt.Z < Hit.Z ? 0.55f : 1.f;        // (as UpdateHandWater sets it in the headset)
	if (ShotStep == 1)
	{
		At = ShotTime;
		ShotStep = 2;
		return;
	}
	if (ShotStep == 2 && ShotTime > At + 0.6f)
	{
		static const TCHAR* Names[3] = { TEXT("on"), TEXT("off"), TEXT("dry") };
		float On = -1.f, Z = 0.f;
		if (UMaterialParameterCollectionInstance* Mpc = ToneMpc ? GetWorld()->GetParameterCollectionInstance(ToneMpc) : nullptr)
		{
			Mpc->GetScalarParameterValue(TEXT("WaterOn"), On);
			Mpc->GetScalarParameterValue(TEXT("WaterZ"), Z);
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT waterhands %s (%s): eyes %.0f cm over the surface, the hand %.0f cm %s it; surface between them found %d at z %.0f; WaterOn %.0f WaterZ %.0f"),
			Names[Round], Fl, Eye.Z - Surface, FMath::Abs(HandZ - Surface), HandZ < Surface ? TEXT("under") : TEXT("over"), bAcross ? 1 : 0, bAcross ? Hit.Z : 0.0, On, Z);
		if (Round == 0) Check(bAcross && On > 0.5f && FMath::Abs(Z - Surface) < 1.f, TEXT("the hand under the water: the surface found, the held gear left at its true depth (WaterOn 1, WaterZ the surface)"));
		float GunBright = -1.f;
		if (UMaterialInstanceDynamic* GM = WeaponMesh[1] ? Cast<UMaterialInstanceDynamic>(WeaponMesh[1]->GetMaterial(0)) : nullptr) GM->GetScalarParameterValue(FName(TEXT("Brightness")), GunBright);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT waterhands %s: the gun's brightness %.3f, the held light %.3f (x%.2f)"), Names[Round], GunBright, HeldLight, HeldLight > 0.f ? GunBright / HeldLight : 0.f);
		if (Round == 0) Check(HeldLight > 0.f && FMath::Abs(GunBright - HeldLight * 0.55f) < 0.02f, TEXT("the gun under the water dimmed as the water dims (x0.55)"));
		if (Round == 2) Check(HeldLight > 0.f && FMath::Abs(GunBright - HeldLight) < 0.02f, TEXT("the gun over the water at the held light"));
		if (Round == 2) Check(!bAcross && On < 0.5f, TEXT("the hand over the water: the held gear pulled as everywhere (WaterOn 0)"));
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("ol_waterhands_%s_%s.png"), Names[Round], Fl), false, false);
		At = ShotTime;
		ShotStep = 3;
		return;
	}
	if (ShotStep == 3 && ShotTime > At + 0.6f)
	{
		if (++Round >= 3) { Finish(); return; }
		At = ShotTime;
		ShotStep = 2;
	}
}

OL_SHOT_MATCH(OLWaterHandsTest) { return OLFlags::Has(TEXT("OLWaterHandsTest")); }
OL_SHOT_TEST(OLWaterHandsTest, 180)
{
	WaterHandsTestTick(Dt);          // (BUG-WATERHANDS: OutlawsPlayerTreadTest.cpp)
}

// ---- -OLShots -OLWheelTopTest[=nowiden|oldmat] (BUG-WHEELOCCLUDE) --------------------------------------------------------
// A door (an opaque 3 x 3 m slab, its face 25 cm before the eyes) and the wheel opened behind it (2.4 m out). The PC's overlay
// has no depth test; the headset's ("_Q") is depth tested at a pulled depth, 50 cm before the fix (the door won), 18 cm now.
// After a second and a half: every shown wheel part drawn (WasRecentlyRendered: not culled) and its bounds holding the eyes;
// a shot with the wheel open ("ol_wheeltop_<mode>_<fl>_open.png") and one after it closes ("_closed"): the wheel must change
// the picture (vb_shotdiff.py). =nowiden: the parts' own bounds; =oldmat: the headset's 50 cm copies (the controls)

void AOutlawsPlayer::WheelTopTestTick(float Dt)
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L) return;
	static int32 Fails = 0;
	static float At = 0.f;
	static UStaticMeshComponent* Door = nullptr;
	const bool bNoWiden = OLFlags::ValueIs(TEXT("OLWheelTopTest"), TEXT("nowiden"));
	const bool bOldMat = OLFlags::ValueIs(TEXT("OLWheelTopTest"), TEXT("oldmat"));
	const TCHAR* Mode = bNoWiden ? TEXT("nowiden") : bOldMat ? TEXT("oldmat") : TEXT("fixed");
	const TCHAR* Fl = GetWorld()->FeatureLevel <= ERHIFeatureLevel::ES3_1 ? TEXT("es31") : TEXT("sm5");
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wheeltop check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (!bOk) ++Fails;
	};
	auto Shot = [&](const TCHAR* Which)
	{
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("ol_wheeltop_%s_%s_%s.png"), Mode, Fl, Which), false, false);
	};
	static float Quiet = -1.f;
	static double OpenedAt = 0.0;
	auto WheelMatsReady = [this]()
	{
#if WITH_EDITOR
		const ERHIFeatureLevel::Type FlNow = GetWorld()->FeatureLevel;
		for (const bool bText : { false, true })
		{
			const UMaterialInterface* Mi = WheelOverlayMaterial(bText);
			const UMaterial* M = Mi ? Mi->GetMaterial() : nullptr;
			const FMaterialResource* R = M ? M->GetMaterialResource(FlNow) : nullptr;
			if (R && !R->IsCompilationFinished()) return false;
		}
#endif
		return true;
	};
	if (ShotStep == 0)
	{
		if (ShotTime < 4.f || !TestShadersReady(Quiet, 2.f)) return;
		if (Controller) Controller->SetControlRotation(FRotator(0.0, GetActorRotation().Yaw, 0.0));
		const FVector Eye = Camera->GetComponentLocation();
		FVector Fwd = Camera->GetForwardVector();
		Fwd.Z = 0.0;
		Fwd = Fwd.GetSafeNormal();
		Door = NewObject<UStaticMeshComponent>(this, TEXT("OLTestDoor"));
		Door->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
		Door->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Door->SetMaterial(0, SolidMaterial(FColor(96, 70, 44)));
		Door->RegisterComponent();
		Door->SetWorldLocationAndRotation(Eye + Fwd * 30.0, Fwd.Rotation());
		Door->SetWorldScale3D(FVector(0.1, 3.0, 3.0));             // 10 cm thick (its face 25 cm out), 3 m wide and tall
		OpenWheel(1);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wheeltop (%s, %s): a door 25 cm before the eyes, the wheel opened %.0f cm out"),
			Mode, Fl, WheelRoot ? FVector::Dist(Eye, WheelRoot->GetComponentLocation()) : -1.0);
		At = ShotTime;
		OpenedAt = FPlatformTime::Seconds();
		ShotStep = 1;
		return;
	}
	if (ShotStep == 1 && !WheelMatsReady() && FPlatformTime::Seconds() - OpenedAt < 240.0)
	{
		At = ShotTime;                                  // (the wheel's materials still compiling for this feature level)
		return;
	}
	if (ShotStep == 1 && ShotTime > At + 1.5f)
	{
		const FVector Eye = Camera->GetComponentLocation();
		TArray<USceneComponent*> Parts;
		if (WheelRoot) WheelRoot->GetChildrenComponents(true, Parts);
		int32 Shown = 0, Drawn = 0, HoldEyes = 0;
		TArray<FString> NotDrawn;
		for (USceneComponent* C : Parts)
		{
			UPrimitiveComponent* P = Cast<UPrimitiveComponent>(C);
			if (!P || !P->IsVisible()) continue;
			if (!P->IsA<UStaticMeshComponent>()) continue;     // (the planes: the labels' text may have nothing to draw)
			++Shown;
			if (P->WasRecentlyRendered(0.25f)) ++Drawn;
			else if (NotDrawn.Num() < 6) NotDrawn.Add(P->GetName());
			if (FVector::Dist(Eye, P->Bounds.Origin) < P->Bounds.SphereRadius) ++HoldEyes;
		}
		const bool bDoorDrawn = Door && Door->WasRecentlyRendered(0.25f);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wheeltop (%s, %s): %d wheel planes shown, %d drawn, %d with bounds holding the eyes; the door drawn %d; not drawn: %s"),
			Mode, Fl, Shown, Drawn, HoldEyes, bDoorDrawn ? 1 : 0, NotDrawn.Num() ? *FString::Join(NotDrawn, TEXT(", ")) : TEXT("none"));
		if (!bNoWiden && !bOldMat)
		{
			Check(Shown > 0 && Drawn == Shown, FString::Printf(TEXT("every shown wheel part drawn behind the door (%d of %d)"), Drawn, Shown));
			Check(HoldEyes == Shown, FString::Printf(TEXT("every shown wheel part's bounds hold the eyes (%d of %d): never culled behind a wall"), HoldEyes, Shown));
		}
		Check(bDoorDrawn, TEXT("the door itself drawn (it is in view)"));
		Shot(TEXT("open"));
		At = ShotTime;
		ShotStep = 2;
		return;
	}
	if (ShotStep == 2 && ShotTime > At + 0.6f)
	{
		CloseWheel();
		At = ShotTime;
		ShotStep = 3;
		return;
	}
	if (ShotStep == 3 && ShotTime > At + 0.6f)
	{
		Shot(TEXT("closed"));
		At = ShotTime;
		ShotStep = 4;
		return;
	}
	if (ShotStep == 4 && ShotTime > At + 0.6f)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wheeltop RESULT %s (%d failed)"), Fails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Fails);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

OL_SHOT_MATCH(OLWheelTopTest) { return OLFlags::Has(TEXT("OLWheelTopTest")); }
OL_SHOT_TEST(OLWheelTopTest, 210)
{
	WheelTopTestTick(Dt);            // (BUG-WHEELOCCLUDE: OutlawsPlayerTreadTest.cpp)
}


// ---- -OLShots -OLUwEdgeTest[=sector] (BUG-UNDERWATERFLICKER) -------------------------------------------------------------
// The eyes held 90 cm under the surface of a water sector (the one named, else the level's deepest), looking level, and a
// shot of the view in three rounds: the underwater blur's taps clamped to the eye's own rectangle (the fix, "clamp"), the
// old unclamped taps ("noclamp": the control, M_OLR_Underwater's EdgeClamp 0), and clamped again ("clamp2"). Run with
// -emulatestereo for the two eyes side by side as in the headset (uwedge_diff.py compares the shots).

void AOutlawsPlayer::UwEdgeTestTick(float Dt)
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L) return;
	static int32 Fails = 0, Sec = -1, Round = 0;
	static float At = 0.f, Surface = 0.f, Quiet = -1.f;
	static FVector Spot = FVector::ZeroVector;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT uwedge check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (!bOk) ++Fails;
	};
	auto Finish = [this]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT uwedge RESULT %s (%d failed)"), Fails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Fails);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	};
	if (ShotStep == 0)
	{
		if (ShotTime < 3.f || !TestShadersReady(Quiet, 1.f)) return;
		FString Arg;
		Sec = OLFlags::Value(TEXT("OLUwEdgeTest"), Arg) && Arg.IsNumeric() ? FCString::Atoi(*Arg) : L->DebugWaterSector();
		FVector Top;
		int32 TopSec = -1;
		if (Sec < 0 || !L->DebugPointInSector(Sec, Top) || !L->WaterSurfaceAt(Top - FVector(0.0, 0.0, 10.0), Surface, TopSec))
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT uwedge: no water sector (%d)"), Sec);
			Check(false, TEXT("a water sector to dive in"));
			Finish();
			return;
		}
		Spot = FVector(Top.X, Top.Y, Surface);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT uwedge: water sector %d (top %d), surface z %.0f, stereo %d"), Sec, TopSec, Surface,
			GEngine && GEngine->StereoRenderingDevice.IsValid() && GEngine->StereoRenderingDevice->IsStereoEnabled() ? 1 : 0);
		At = ShotTime;
		ShotStep = 1;
	}
	// held each frame with the eyes 90 cm under the surface, looking level
	const double EyeOff = Camera->GetComponentLocation().Z - GetActorLocation().Z;
	SetActorLocation(FVector(Spot.X, Spot.Y, Surface - 90.0 - EyeOff), false, nullptr, ETeleportType::TeleportPhysics);
	GetCharacterMovement()->Velocity = FVector::ZeroVector;
	if (Controller) Controller->SetControlRotation(FRotator(0.0, GetActorRotation().Yaw, 0.0));
	static const TCHAR* Names[3] = { TEXT("clamp"), TEXT("noclamp"), TEXT("clamp2") };
	if (UnderwaterMid) UnderwaterMid->SetScalarParameterValue(TEXT("EdgeClamp"), Round == 1 ? 0.f : 1.f);
	if (ShotStep == 1 && ShotTime > At + 2.5f)
	{
#if WITH_EDITOR
		// (the filter's own shaders compiled: a post process material still compiling is skipped)
		const UMaterial* Uw = LoadObject<UMaterial>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_Underwater.M_OLR_Underwater"));
		const FMaterialResource* R = Uw ? Uw->GetMaterialResource(GetWorld()->FeatureLevel) : nullptr;
		if (R && !R->IsCompilationFinished() && ShotTime < At + 120.f) return;
#endif
		At = ShotTime;
		ShotStep = 2;
		return;
	}
	if (ShotStep == 2 && ShotTime > At + 1.0f)
	{
		const float Deep = Surface - float(Camera->GetComponentLocation().Z);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT uwedge %s: eyes %.0f cm under, head under %d, filter amount %.2f"), Names[Round], Deep, bHeadUnder ? 1 : 0, UnderwaterAmount);
		Check(bHeadUnder && UnderwaterMid && UnderwaterAmount > 0.2f, FString::Printf(TEXT("%s: the eyes under the water, the underwater filter on (%.2f)"), Names[Round], UnderwaterAmount));
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("ol_uwedge_%s_%s.png"), *L->LevelName, Names[Round]), false, false);
		At = ShotTime;
		ShotStep = 3;
		return;
	}
	if (ShotStep == 3 && ShotTime > At + 0.6f)
	{
		if (++Round >= 3) { Finish(); return; }
		At = ShotTime;
		ShotStep = 2;
	}
}

OL_SHOT_MATCH(OLUwEdgeTest) { return OLFlags::Has(TEXT("OLUwEdgeTest")); }
OL_SHOT_TEST(OLUwEdgeTest, 190)
{
	UwEdgeTestTick(Dt);              // (BUG-UNDERWATERFLICKER: OutlawsPlayerTreadTest.cpp)
}
#endif // WITH_OUTLAWS_TESTS
