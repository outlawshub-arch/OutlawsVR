// -OLShots -OLVb2Test[=gpu] (videobatch2): checks for REQ-NOBADGEFADEMSG, REQ-SHIELDNOEQUIP, BUG-ENEMYCLUNK, BUG-FALLSCREAM and
// REQ-TNTFLASH, in one level run (hideout), printing "OLSHOT vb2 check PASS|FAIL: ..." and "OLSHOT vb2 RESULT".
//   badge    BadgeLeft run out: no "power fades" line on the HUD, the power gone;
//   shield   a plate picked up with a revolver in the right hand: no hand takes the plate, the revolver stays;
//   enemy    one man put through a 4 m drop (hurt, no fall cry), a knock-up that lands on the same floor (no fall cry), a 10 m
//            drop (one fall cry), and a blast beside him: none of these plays "crunch"; a hurt or death cry of his own does;
//            (BUG-ENEMYCLUNK 2, BUG-FALLSCREAM 2) each as this player's doing (his kill if it kills), then a thrown knife's hit
//            and a thrown knife's kill: no "bulhit" (the multiplayer hit tick) on any of them; every sound heard is logged
//            ("OLSHOT vb2 heard"); run with -OLHitConfirmSP the tick is allowed in single player and must still stay out;
//   tnt      the blast lit a flash slot at 5 times the muzzle flash's reach and brightness, that decays out.
// -OLVb2Test=gpu: REQ-TNTFLASH's cost: the GPU frame time with a TNT flash held lit against none, twice each, still, looking the way
// the start faces (run with -featureleveles31 -OLDeviceProfile=Quest2 for the headset's path).

#include "OutlawsPlayer.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsEnemy.h"
#include "OutlawsStats.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "RenderCore.h"
#include "RHI.h"
#include "OutlawsDeviceProfile.h"
#include "UnrealClient.h"
#include "Misc/Paths.h"
#include "OutlawsShotTests.h"

#if WITH_OUTLAWS_TESTS

void AOutlawsPlayer::Vb2TestTick(float Dt)
{
	AOutlawsLevelActor* L = Level.Get();
	UWorld* W = GetWorld();
	if (!L || !W) return;
	static int32 Fails = 0;
	static float At = 0.f;
	static TArray<FString> Spy;
	static TWeakObjectPtr<AOutlawsEnemy> Man;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT vb2 check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (!bOk) ++Fails;
	};
	auto Finish = [this]()
	{
		AOutlawsLevelActor::WavSpy = nullptr;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT vb2 RESULT %s (%d failed)"), Fails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Fails);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	};
	auto Heard = [](const FString& Wav) { return Spy.Contains(Wav.ToLower()); };
	auto HeardText = []() { return FString::Join(Spy, TEXT(",")); };
	const bool bGpu = OLFlags::ValueIs(TEXT("OLVb2Test"), TEXT("gpu"));

	if (bGpu)
	{
		constexpr float Win = 5.f, Skip = 1.f;
		static float T0 = 0.f, Quiet = -1.f, Yaw = -1000.f;
		static double GpuSum[4] = {};
		static int32 Frames[4] = {};
		if (Yaw < -999.f) Yaw = float(GetActorRotation().Yaw);
		if (Controller) Controller->SetControlRotation(FRotator(0.0, Yaw, 0.0));
		if (ShotStep == 0)
		{
			if (!TestShadersReady(Quiet, 3.f) || ShotTime < 5.f) return;
			T0 = ShotTime;
			int32 Pct = 0;
			if (OLFlags::Value(TEXT("OLCostScreenPct"), Pct) && Pct > 0)
			{
				if (IConsoleVariable* Sp = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage"))) Sp->Set(float(Pct), ECVF_SetByCode);
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT vb2 gpu: start (%s, %s profile)"),
				W->FeatureLevel == ERHIFeatureLevel::ES3_1 ? TEXT("ES3_1, the headset's path") : TEXT("SM5"), OutlawsDeviceProfile::IsQuest2() ? TEXT("Quest2") : TEXT("default"));
			ShotStep = 1;
			return;
		}
		if (ShotStep != 1) return;
		const float In = ShotTime - T0;
		const int32 Wn = FMath::FloorToInt(In / Win);
		if (Wn < 4)
		{
			// flash windows 1 and 3: the blast flash held at its start every frame, 5 m in front of the eyes
			if ((Wn % 2) == 1) L->MuzzleFlash(Camera->GetComponentLocation() + Camera->GetForwardVector() * 500.0, AOutlawsLevelActor::BlastFlashScale, 1300.f * AOutlawsLevelActor::BlastFlashScale);
			if (In - Wn * Win < Skip) return;
			const double Gpu = FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0));
			if (Gpu > 0.0) { GpuSum[Wn] += Gpu; ++Frames[Wn]; }
			return;
		}
		auto Ms = [](int32 k) { return Frames[k] > 0 ? GpuSum[k] / Frames[k] : 0.0; };
		const double None = (Ms(0) + Ms(2)) * 0.5, Lit = (Ms(1) + Ms(3)) * 0.5;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT vb2 gpu: GPU ms none %.3f (%d frames), flash %.3f (%d), none %.3f (%d), flash %.3f (%d)"),
			Ms(0), Frames[0], Ms(1), Frames[1], Ms(2), Frames[2], Ms(3), Frames[3]);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT vb2 gpu RESULT: none %.3f ms, TNT flash lit %.3f ms, flash - none %+.3f ms (%+.1f %%)"), None, Lit, Lit - None, None > 0.0 ? 100.0 * (Lit - None) / None : 0.0);
		Check(Frames[0] > 30 && Frames[1] > 30 && Frames[2] > 30 && Frames[3] > 30, FString::Printf(TEXT("GPU timing in every window (%d %d %d %d frames)"), Frames[0], Frames[1], Frames[2], Frames[3]));
		Finish();
		return;
	}

	if (OLFlags::ValueIs(TEXT("OLVb2Test"), TEXT("sky")))
	{
		// BUG-TRAINSKY: out under the sky (the first open-ceiling sector, unless the start is under it), looking up and across at
		// the horizon: a shot each (ol_sky_<pose>.png) and the colour then read from the shots by vb2_skycheck.py
		struct FPose { const TCHAR* Name; float Pitch; float Yaw; };
		static const FPose Poses[] = { { TEXT("zenith"), 80.f, 0.f }, { TEXT("high"), 55.f, 0.f }, { TEXT("mid"), 30.f, 0.f }, { TEXT("low"), 12.f, 0.f }, { TEXT("horizon"), 2.f, 0.f }, { TEXT("horizon_b"), 2.f, 180.f } };
		static float Quiet = -1.f;
		static int32 Pose = 0;
		if (ShotStep == 0)
		{
			if (!TestShadersReady(Quiet, 3.f) || ShotTime < 6.f) return;
			int32 Pick = INDEX_NONE;
			for (int32 i = 0; i < L->TestSectorCount() && Pick == INDEX_NONE; ++i)
			{
				FVector Pt;
				if ((L->TestSectorFlags(i) & 1u) && L->DebugPointInSector(i, Pt)) Pick = i;
			}
			FVector Pt;
			if (Pick != INDEX_NONE && L->DebugPointInSector(Pick, Pt))
			{
				FHitResult Hit;
				FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsVb2Sky), false, this);
				const double Floor = W->LineTraceSingleByChannel(Hit, Pt - FVector(0.0, 0.0, 20.0), Pt - FVector(0.0, 0.0, 6000.0), ECC_Visibility, Q) ? Hit.ImpactPoint.Z : Pt.Z - 300.0;
				TeleportTo(FVector(Pt.X, Pt.Y, Floor + GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 5.0), GetActorRotation(), false, true);
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT vb2 sky: level %s, open-ceiling sector %d, gradient dome %d, at %s"), *L->LevelName, Pick, L->HasGradientSky() ? 1 : 0, *GetActorLocation().ToString());
			At = ShotTime;
			ShotStep = 1;
			return;
		}
		if (Pose >= UE_ARRAY_COUNT(Poses)) return;
		if (Controller) Controller->SetControlRotation(FRotator(Poses[Pose].Pitch, Poses[Pose].Yaw, 0.0));
		const float In = ShotTime - At;
		if (ShotStep == 1 && In > 1.0f)
		{
			FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("ol_sky_%s.png"), Poses[Pose].Name), false, false);
			ShotStep = 2;
			At = ShotTime;
		}
		else if (ShotStep == 2 && In > 0.5f)
		{
			++Pose;
			ShotStep = 1;
			At = ShotTime;
			if (Pose >= UE_ARRAY_COUNT(Poses))
			{
				Check(L->HasGradientSky(), TEXT("the level uses the gradient dome (the shots are read by vb2_skycheck.py)"));
				Finish();
			}
		}
		return;
	}

	enum { Start, Badge, Shield, ShieldWait, PickMan, Short, ShortWait, Knock, KnockWait, Long, LongWait, Blast, BlastWait, Knife, KnifeWait, KnifeKill, KnifeKillWait, Done };
	auto PlaceMan = [&](float UpCm) -> bool
	{
		// the nearest living man that is not a critter, three metres ahead, UpCm over the floor there
		AOutlawsEnemy* Best = nullptr;
		double Dist = 1e30;
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
		{
			AOutlawsEnemy* E = Wk.Get();
			if (!E || E->IsDead() || E->IsDormant() || E->IsCritter()) continue;
			const double D = FVector::DistSquared(E->GetActorLocation(), GetActorLocation());
			if (D < Dist) { Dist = D; Best = E; }
		}
		if (!Best) return false;
		Man = Best;
		const FVector Fl = FVector(Camera->GetForwardVector().X, Camera->GetForwardVector().Y, 0.0).GetSafeNormal();
		const FVector Ahead = GetActorLocation() + Fl * 300.0;
		FHitResult Hit;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsVb2Test), false, this);
		Q.AddIgnoredActor(Best);
		const double FloorZ = W->LineTraceSingleByChannel(Hit, Ahead + FVector(0.0, 0.0, 100.0), Ahead - FVector(0.0, 0.0, 2000.0), ECC_Visibility, Q) ? Hit.ImpactPoint.Z : GetActorLocation().Z - 90.0;
		const double Half = Best->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		if (UpCm > 100.f)
		{
			FHitResult Up;
			if (W->LineTraceSingleByChannel(Up, FVector(Ahead.X, Ahead.Y, FloorZ + 50.0), FVector(Ahead.X, Ahead.Y, FloorZ + UpCm + 2.0 * Half + 50.0), ECC_Visibility, Q)) return false;      // (a roof over the drop)
		}
		Best->SetActorLocation(FVector(Ahead.X, Ahead.Y, FloorZ + UpCm + Half + 2.0), false, nullptr, ETeleportType::TeleportPhysics);
		Best->WakeForTest();
		Best->StatsBy = uint8(EOutlawsStatsBy::Me);          // (this player's doing: a kill by it ticked "bulhit" before BUG-ENEMYCLUNK 2)
		Best->GetCharacterMovement()->Velocity = FVector::ZeroVector;
		Best->GetCharacterMovement()->SetMovementMode(MOVE_Falling);
		return true;
	};
	// a living man falls at the bad guys' pace, slower than a body: each case waits for his landing (and a moment after it)
	auto Settled = [&]() -> bool
	{
		static float SettleAt = -1.f;
		AOutlawsEnemy* E = Man.Get();
		if (ShotTime - At > 40.f) return true;
		if (E && ShotTime - At > 1.5f && !E->GetCharacterMovement()->IsFalling())
		{
			if (SettleAt < 0.f) SettleAt = ShotTime;
			if (ShotTime - SettleAt > 1.0f) { SettleAt = -1.f; return true; }
		}
		else SettleAt = -1.f;
		return false;
	};
	auto Begin = [&](int32 Next, const char* Why)
	{
		if (Spy.Num() > 0) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT vb2 heard: %s"), *HeardText());
		Spy.Reset();
		AOutlawsLevelActor::WavSpy = &Spy;
		At = ShotTime;
		ShotStep = Next;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT vb2: %s"), UTF8_TO_TCHAR(Why));
	};

	if (ShotStep == Start)
	{
		if (ShotTime < 3.f) return;
		ShotStep = Badge;
		return;
	}
	if (ShotStep == Badge)
	{
		L->bInvincible = true;
		HudLines.Reset();
		L->BadgeLeft = 0.2f;
		At = ShotTime;
		ShotStep = Badge + 1;                      // (the shield step: below, after the badge has run out)
		return;
	}
	if (ShotStep == Shield)
	{
		if (ShotTime - At < 0.6f) return;
		bool bFades = false;
		for (const FString& H : HudLines) bFades |= H.Contains(TEXT("fades"));
		Check(L->BadgeLeft <= 0.f && !bFades, FString::Printf(TEXT("the badge's power ran out (%.2f left) and no 'power fades' line (%d lines up)"), L->BadgeLeft, HudLines.Num()));
		// a revolver in the right hand, then a plate picked up (the hand logic that is the headset's, run on the desktop)
		bTestPlatePickup = true;
		Equip(1, 0);
		bUnlimitedAmmo = true;
		const int32 Gun = Equipped[1];
		L->ArmourMax = 0;
		L->AddCount(TEXT("ibplate"), 150);
		At = ShotTime;
		ShotStep = ShieldWait;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT vb2: plate picked up, right hand weapon %d, armour %d"), Gun, L->GetArmour());
		return;
	}
	if (ShotStep == ShieldWait)
	{
		if (ShotTime - At < 1.0f) return;
		Check(L->GetArmour() == 150, FString::Printf(TEXT("the plate was taken (armour %d)"), L->GetArmour()));
		Check(HeldTool[0] != TEXT("ibplate") && HeldTool[1] != TEXT("ibplate"), FString::Printf(TEXT("the plate is in no hand (held tools '%s' '%s')"), *HeldTool[0], *HeldTool[1]));
		Check(Equipped[1] >= 0, FString::Printf(TEXT("the right hand still holds its revolver (weapon %d)"), Equipped[1]));
		bTestPlatePickup = false;
		ShotStep = PickMan;
		return;
	}
	if (ShotStep == PickMan)
	{
		if (ShotTime < 8.f) return;                // (past the bad guys' first seconds: a fall then never cries, BUG-FALLSCREAM 2)
		Begin(Short, "a 4 m drop for the nearest man");
		if (!PlaceMan(400.f))
		{
			Check(false, TEXT("a man to put through the drops"));
			Finish();
		}
		return;
	}
	if (ShotStep == Short)
	{
		ShotStep = ShortWait;
		return;
	}
	if (ShotStep == ShortWait)
	{
		if (!Settled()) return;
		AOutlawsEnemy* E = Man.Get();
		const bool bSound = E && (Heard(E->Def.HitSound) || Heard(E->Def.DieSound));
		Check(E && !Heard(TEXT("bgfall")), FString::Printf(TEXT("a 4 m drop: no fall cry (heard: %s)"), *HeardText()));
		Check(!Heard(TEXT("crunch")) && bSound, FString::Printf(TEXT("a 4 m drop: his own hurt or death cry (%s / %s), no crunch (heard: %s)"), E ? *E->Def.HitSound : TEXT("-"), E ? *E->Def.DieSound : TEXT("-"), *HeardText()));
		Check(!Heard(TEXT("bulhit")), FString::Printf(TEXT("a 4 m drop: no bulhit tick (heard: %s)"), *HeardText()));
		ShotStep = Knock;
		return;
	}
	if (ShotStep == Knock)
	{
		Begin(KnockWait, "a knock-up that lands on the same floor");
		if (!PlaceMan(0.f))
		{
			Check(false, TEXT("a man for the knock"));
			Finish();
			return;
		}
		Man->LaunchCharacter(FVector(0.0, 0.0, 1100.0), true, true);      // (a 6 m arc: peak above where a 2.5 m rule used to cry)
		return;
	}
	if (ShotStep == KnockWait)
	{
		if (!Settled()) return;
		Check(!Heard(TEXT("bgfall")), FString::Printf(TEXT("a knock up and down on one floor: no fall cry (heard: %s)"), *HeardText()));
		Check(!Heard(TEXT("crunch")), FString::Printf(TEXT("the knock's landing: no crunch (heard: %s)"), *HeardText()));
		ShotStep = Long;
		return;
	}
	if (ShotStep == Long)
	{
		Begin(LongWait, "a 10 m drop");
		if (!PlaceMan(1000.f))
		{
			// (a roof over the column, or none left: the same drop from a lower start would not be the check)
			Check(false, TEXT("a clear 10 m column for a man to fall in"));
			Finish();
		}
		return;
	}
	if (ShotStep == LongWait)
	{
		if (!Settled()) return;
		int32 Cries = 0;
		for (const FString& S : Spy) Cries += S == TEXT("bgfall") ? 1 : 0;
		Check(Cries == 1, FString::Printf(TEXT("a 10 m drop: one fall cry (%d; heard: %s)"), Cries, *HeardText()));
		Check(!Heard(TEXT("crunch")), FString::Printf(TEXT("the 10 m landing: no crunch (heard: %s)"), *HeardText()));
		Check(!Heard(TEXT("bulhit")), FString::Printf(TEXT("the 10 m landing: no bulhit tick (heard: %s, dead %d)"), *HeardText(), Man.IsValid() && Man->IsDead() ? 1 : 0));
		ShotStep = Blast;
		return;
	}
	if (ShotStep == Blast)
	{
		Begin(BlastWait, "a blast beside a man");
		if (!PlaceMan(0.f))
		{
			Check(false, TEXT("a living man for the blast"));
			Finish();
			return;
		}
		Man->SetActorLocation(Man->GetActorLocation() + FVector(0.0, 0.0, 30.0), false, nullptr, ETeleportType::TeleportPhysics);
		L->Explode(Man->GetActorLocation() - Camera->GetForwardVector().GetSafeNormal2D() * 150.0);
		// the flash, at once: a slot lit at 5 times the reach and the brightness
		int32 Lit = INDEX_NONE;
		for (int32 i = 0; i < UE_ARRAY_COUNT(L->Flashes); ++i) if (L->Flashes[i].Age < 0.1f && L->Flashes[i].Strength >= AOutlawsLevelActor::BlastFlashScale - 0.01f) Lit = i;
		Check(Lit != INDEX_NONE && FMath::IsNearlyEqual(L->Flashes[Lit].Radius, 6500.f), FString::Printf(TEXT("the blast lit a flash slot at 5 times the muzzle flash (slot %d, reach %.0f vs 1300, strength %.1f vs 1)"),
			Lit, Lit != INDEX_NONE ? L->Flashes[Lit].Radius : 0.f, Lit != INDEX_NONE ? L->Flashes[Lit].Strength : 0.f));
		return;
	}
	if (ShotStep == BlastWait)
	{
		if (!Settled()) return;
		AOutlawsEnemy* E = Man.Get();
		const bool bOwnCry = E && (Heard(E->Def.HitSound) || Heard(E->Def.DieSound));
		Check(!Heard(TEXT("crunch")), FString::Printf(TEXT("a man hurt by a blast: no crunch (heard: %s)"), *HeardText()));
		Check(bOwnCry, FString::Printf(TEXT("a man hurt by a blast: his own hurt or death cry (%s / %s)"), E ? *E->Def.HitSound : TEXT("-"), E ? *E->Def.DieSound : TEXT("-")));
		Check(!Heard(TEXT("bgfall")), FString::Printf(TEXT("a blast's hop: no fall cry (heard: %s)"), *HeardText()));
		Check(!Heard(TEXT("bulhit")), FString::Printf(TEXT("a blast: no bulhit tick (heard: %s, dead %d)"), *HeardText(), E && E->IsDead() ? 1 : 0));
		bool bGone = true;
		for (int32 i = 0; i < UE_ARRAY_COUNT(L->Flashes); ++i) bGone &= L->Flashes[i].Age >= 0.25f;
		Check(bGone, TEXT("the flash decayed out and freed its slot"));
		ShotStep = Knife;
		return;
	}
	if (ShotStep == Knife || ShotStep == KnifeKill)
	{
		// a thrown knife's hit as the knife deals it (OutlawsPlayerWeapons.cpp: GNextHitMelee, no knock), first a wound, then a kill
		const bool bKill = ShotStep == KnifeKill;
		Begin(bKill ? KnifeKillWait : KnifeWait, bKill ? "a thrown knife's kill" : "a thrown knife's wound");
		if (!PlaceMan(0.f))
		{
			Check(false, TEXT("a living man for the knife"));
			Finish();
			return;
		}
		const FVector From = GetActorLocation();
		AOutlawsEnemy::GNextHitMelee = true;
		Man->TakeHit(bKill ? 500.f : 0.2f, Man->GetActorLocation(), (Man->GetActorLocation() - From).GetSafeNormal(), From, false, 0.f);
		At = ShotTime;
		return;
	}
	if (ShotStep == KnifeWait || ShotStep == KnifeKillWait)
	{
		if (ShotTime - At < 1.0f) return;
		AOutlawsEnemy* E = Man.Get();
		const bool bKill = ShotStep == KnifeKillWait;
		const FString Own = E ? (bKill ? E->Def.DieSound : E->Def.HitSound) : FString();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT vb2 heard: %s"), *HeardText());
		Check(!Heard(TEXT("bulhit")), FString::Printf(TEXT("%s: no bulhit tick (heard: %s, dead %d)"), bKill ? TEXT("a thrown knife's kill") : TEXT("a thrown knife's wound"), *HeardText(), E && E->IsDead() ? 1 : 0));
		Check(!Own.IsEmpty() && Heard(Own), FString::Printf(TEXT("%s: his own %s cry %s"), bKill ? TEXT("a thrown knife's kill") : TEXT("a thrown knife's wound"), bKill ? TEXT("death") : TEXT("hurt"), *Own));
		if (bKill) { ShotStep = Done; Finish(); return; }
		ShotStep = KnifeKill;
	}
}

OL_SHOT_MATCH(OLVb2Test) { return OLFlags::Has(TEXT("OLVb2Test")); }
OL_SHOT_TEST(OLVb2Test, 150)
{
	Vb2TestTick(Dt);                 // (videobatch2: OutlawsPlayerVb2Test.cpp)
}
#endif // WITH_OUTLAWS_TESTS
