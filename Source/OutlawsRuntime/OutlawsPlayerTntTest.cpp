// -OLShots -OLLevel=hideout -OLNoFrontEnd -OLTntDeathTest (BUG-TNTDEATHFREEZE, single player): a lit stick of dynamite is thrown over
// a bad guy and the player dies while it is in the air.  The stick must keep flying (it froze mid-air: the dead view did not tick
// the thrown things), go off at its fuse, and the blast must still count as this player's dynamite kill.  Prints
// "OLSHOT tnt check PASS|FAIL: ..." and "OLSHOT tnt RESULT".  The two-machine half is in OutlawsCoopRemoteFxTest.cpp (-OLRemoteFxTest).

#include "OutlawsPlayer.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsEnemy.h"
#include "OutlawsStats.h"
#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Components/StaticMeshComponent.h"
#include "Camera/CameraComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "OutlawsBoxHand.h"
#include "ProceduralMeshComponent.h"
#include "UnrealClient.h"
#include "OutlawsShotTests.h"

#if WITH_OUTLAWS_TESTS

void AOutlawsPlayer::TntDeathTestTick(float Dt)
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L || !GetWorld()) return;
	static int32 Fails = 0, Blasts0 = 0, Kills0 = 0;
	static float At = 0.f;
	static FVector PosDead = FVector::ZeroVector;
	static TWeakObjectPtr<AOutlawsEnemy> Man;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT tnt check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (!bOk) ++Fails;
	};
	auto Finish = [this]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT tnt RESULT %s (%d failed)"), Fails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Fails);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	};
	if (ShotStep == 0)
	{
		if (ShotTime < 6.f) return;
		// (a man standing on the floor the stick will lie on: the same ground a metre and a half away as under his feet)
		AOutlawsEnemy* Best = nullptr;
		FVector Over = FVector::ZeroVector;
		auto GroundZ = [this](const FVector& From, double& OutZ)
		{
			FHitResult Gr;
			FCollisionQueryParams GQ(SCENE_QUERY_STAT(OutlawsTntTest), true, this);
			if (!GetWorld()->LineTraceSingleByChannel(Gr, From, From - FVector(0.0, 0.0, 600.0), ECC_Visibility, GQ)) return false;
			OutZ = Gr.ImpactPoint.Z;
			return true;
		};
		for (const TWeakObjectPtr<AOutlawsEnemy>& WE : L->GetEnemies())
		{
			AOutlawsEnemy* E = WE.Get();
			if (!E || E->IsDead() || E->bVoiceOnly || E->bNotBadGuy || E->bBoss || E->IsDormant()) continue;
			const FVector Pos = E->GetActorLocation();
			for (const FVector& Off : { FVector(150.0, 0.0, 0.0), FVector(-150.0, 0.0, 0.0), FVector(0.0, 150.0, 0.0), FVector(0.0, -150.0, 0.0) })
			{
				double Under = 0.0, Beside = 0.0;
				if (!GroundZ(Pos, Under) || !GroundZ(Pos + Off + FVector(0.0, 0.0, 100.0), Beside)) continue;
				if (FMath::Abs(Under - Beside) > 20.0 || Pos.Z - Under > 130.0) continue;
				if (!Best || FVector::DistSquared(Pos, GetActorLocation()) < FVector::DistSquared(Best->GetActorLocation(), GetActorLocation())) { Best = E; Over = Pos + Off + FVector(0.0, 0.0, 160.0); }
				break;
			}
		}
		Check(Best != nullptr, TEXT("a bad guy to blow up, on level ground"));
		if (!Best) { Finish(); return; }
		Man = Best;
		Blasts0 = L->BlastsSeen;
		Kills0 = UOutlawsStats::MyDynamiteKills(L);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT tnt: the bad guy at %s, the stick thrown from %s"), *Best->GetActorLocation().ToCompactString(), *Over.ToCompactString());
		// (beside him, rising a little first: in the air for about a second, the fuse two)
		const bool bThrown = TestThrowTnt(Over, FVector(0.0, 0.0, 300.0), 2.0f);
		Check(bThrown, TEXT("a lit stick thrown"));
		if (!bThrown) { Finish(); return; }
		At = ShotTime;
		ShotStep = 1;
		return;
	}
	if (ShotStep == 1)
	{
		if (ShotTime - At < 0.15f) return;
		L->bInvincible = false;                  // (ShotTest makes the player invincible every frame)
		if (!OLFlags::Param(TEXT("OLTntAlive"))) L->DamagePlayer(1e6f, 1.f, true);       // (-OLTntAlive: the same throw, the player living: the control run)
		bool bRest = false;
		const bool bAir = TestTntState(PosDead, bRest);
		Check(L->IsPlayerDead() && IsDeadView(), TEXT("the player died with the stick in the air"));
		Check(bAir && !bRest, TEXT("the stick was still in the air at the death"));
		At = ShotTime;
		ShotStep = 2;
		return;
	}
	if (ShotStep == 2)
	{
		if (ShotTime - At < 0.4f) return;
		FVector Now = FVector::ZeroVector;
		bool bRest = false;
		const bool bHave = TestTntState(Now, bRest);
		Check(bHave && !bRest && FVector::Dist(Now, PosDead) > 5.0, FString::Printf(TEXT("0.4 s after the death the stick has flown on (moved %.0f cm, %s)"),
			FVector::Dist(Now, PosDead), bHave ? (bRest ? TEXT("at rest") : TEXT("in the air")) : TEXT("gone")));
		At = ShotTime;
		ShotStep = 3;
		return;
	}
	if (ShotStep == 3)
	{
		FVector Now;
		bool bRest;
		const bool bHave = TestTntState(Now, bRest);
		if (bHave && ShotTime - At < 6.f) return;
		Check(L->BlastsSeen > Blasts0, FString::Printf(TEXT("the stick went off (%d blasts)"), L->BlastsSeen - Blasts0));
		Check(!bHave, TEXT("no stick left in flight"));
		const AOutlawsEnemy* E = Man.Get();
		Check(E && E->IsDead(), FString::Printf(TEXT("the bad guy under it died (health %.1f of %.1f, now at %s)"), E ? E->GetHealth() : -1.f, E ? E->GetMaxHealth() : -1.f, E ? *E->GetActorLocation().ToCompactString() : TEXT("?")));
		Check(UOutlawsStats::MyDynamiteKills(L) > Kills0, FString::Printf(TEXT("the dead player is credited the dynamite kill (%d -> %d)"), Kills0, UOutlawsStats::MyDynamiteKills(L)));
		Finish();
	}
}

OL_SHOT_MATCH(OLTntDeathTest) { return OLFlags::Param(TEXT("OLTntDeathTest")); }
OL_SHOT_TEST(OLTntDeathTest, 110)
{
	TntDeathTestTick(Dt);            // (BUG-TNTDEATHFREEZE: OutlawsPlayerTntTest.cpp)
}

// -OLShots -OLLevel=<map> -OLNoFrontEnd -OLTntBlueTest [-OLTntNoTint | -OLTntTint] [-OLTntDist=<cm>] (BUG-TNTBLUE, BUG-FUSEMATCH): a lit stick held before the eyes
// (ol_tntblue_held), then thrown and kept 1.6 m out (ol_tntblue_thrown), then let fly with its sector and palette logged
// every tenth of a second.  Each stick's materials are logged (parent, texture, brightness) and the held and thrown flames compared.
// -OLTntNoTint / -OLTntTint: the menu's Tinting off / on.  Prints "OLSHOT tntblue check PASS|FAIL: ..." and "OLSHOT tntblue RESULT".
static FString TntMatInfo(const UPrimitiveComponent* P)
{
	FString S;
	if (!P) return TEXT("(none)");
	for (int32 i = 0; i < P->GetNumMaterials(); ++i)
	{
		const UMaterialInterface* M = P->GetMaterial(i);
		const UMaterialInstance* Mi = Cast<UMaterialInstance>(M);
		UTexture* Tex = nullptr;
		float Bright = -1.f, Pull = -1.f;
		if (M)
		{
			M->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Tex")), Tex);
			M->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("Brightness")), Bright);
			M->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("NearPull")), Pull);
		}
		S += FString::Printf(TEXT("[%d %s parent %s tex %s bright %.2f pull %.2f] "), i, M ? *M->GetName() : TEXT("null"),
			Mi && Mi->Parent ? *Mi->Parent->GetName() : TEXT("-"), Tex ? *Tex->GetName() : TEXT("none"), Bright, Pull);
	}
	return S;
}

void AOutlawsPlayer::TntBlueTestTick(float Dt)
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L || !GetWorld()) return;
	static int32 Fails = 0;
	static float At = 0.f;
	static float HoldYaw = -1000.f;
	static FVector Spot = FVector::ZeroVector;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT tntblue check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (!bOk) ++Fails;
	};
	auto Shot = [](const TCHAR* Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(Name) + TEXT(".png"), false, false); };
	if (HoldYaw < -999.f) HoldYaw = static_cast<float>(GetActorRotation().Yaw);
	if (Controller) Controller->SetControlRotation(FRotator(-12.0, HoldYaw, 0.0));
	const FVector Eye = Camera->GetComponentLocation();
	const FVector F = Camera->GetForwardVector(), R = Camera->GetRightVector();
	const FVector Fl = FVector(F.X, F.Y, 0.0).GetSafeNormal();
	// the stick (held, then thrown) a little under the eyes, 30 cm out; the hand put down out of the shot once it has thrown
	if (UOutlawsBoxHand* Box = RightHand.Get())
	{
		const FVector Pos = ShotStep <= 2 ? Eye + Fl * 30.0 + R * 3.0 - FVector::UpVector * 12.0 : Eye + Fl * 10.0 + R * 40.0 - FVector::UpVector * 90.0;
		Box->SetVisibility(true, true);
		FrameOf(1)->SetWorldLocationAndRotation(Pos, FRotationMatrix::MakeFromXZ((Fl - R * 0.25).GetSafeNormal(), FVector::UpVector).ToQuat());
		Box->SetCurl(1.f, 1.f);
	}
	auto ThrownStick = [this]() -> FThrown* { for (int32 i = Thrown.Num() - 1; i >= 0; --i) if (Thrown[i].Type == 1) return &Thrown[i]; return nullptr; };
	auto Where = [L](const FVector& P)
	{
		FLinearColor Rows[3], Tint;
		const bool bGrade = L->PaletteGradeAt(P, Rows);
		const bool bUnder = L->UnderwaterTintAt(P, Tint);
		return FString::Printf(TEXT("%s, grade %d, underwater tint %s"), *L->DebugSectorPaletteAt(P), bGrade ? 1 : 0, bUnder ? *Tint.ToString() : TEXT("none"));
	};
	if (ShotStep == 0)
	{
		if (ShotTime < 4.f) return;
		GEngine->Exec(GetWorld(), TEXT("r.MotionBlurQuality 0"));
		L->bInvincible = true;
		L->GiveAllWeapons();
		if (OLFlags::Param(TEXT("OLTntNoTint"))) L->bPaletteGrades = false;
		if (OLFlags::Param(TEXT("OLTntTint"))) L->bPaletteGrades = true;
		if (FireCells.Num() == 0) { if (UMaterialInstanceDynamic* M = L->MakeFireLook(this, FireCells, FireFps, FireAspect)) KeepAlive.Add(M); }
		Equip(1, 6);
		Mag[1] = 1;
		bFuseLit[1] = true;
		Fuse[1] = 4.5f;
		UE_LOG(LogOutlaws, Display, TEXT("OLRT tntblue: Tinting %d, the eyes in %s"), L->bPaletteGrades ? 1 : 0, *Where(Eye));
		At = ShotTime;
		ShotStep = 1;
		return;
	}
	if (ShotStep == 1 || ShotStep == 2) { Fuse[1] = FMath::Max(Fuse[1], 3.f); }
	if (ShotStep == 1)
	{
		UpdateFuseLook(1);
		if (ShotTime - At < 1.5f) return;
		Shot(TEXT("ol_tntblue_held"));
		const UStaticMeshComponent* Fm = FuseFlame[1].Get();
		UE_LOG(LogOutlaws, Display, TEXT("OLRT tntblue held: %s; at %s"), *TntMatInfo(WeaponMesh[1]), *Where(WeaponMesh[1] ? WeaponMesh[1]->GetComponentLocation() : Eye));
		UE_LOG(LogOutlaws, Display, TEXT("OLRT tntblue held flame: mesh %s, %s, height %.2f cm"), Fm && Fm->GetStaticMesh() ? *Fm->GetStaticMesh()->GetName() : TEXT("none"),
			*TntMatInfo(Fm), Fm ? Fm->GetComponentScale().Z : 0.0);
		At = ShotTime;
		ShotStep = 2;
		return;
	}
	if (ShotStep == 2)
	{
		UpdateFuseLook(1);
		if (ShotTime - At < 0.3f) return;
		// (out of the hand it grows to the level item's size: seen from further; -OLTntDist=<cm> how far, 160 by default)
		float Dist = 160.f;
		OLFlags::Value(TEXT("OLTntDist"), Dist);
		Spot = Eye + Fl * Dist - FVector::UpVector * (25.0 * Dist / 160.0);
		const int32 Before = Thrown.Num();
		ThrowHeld(1, false, true);
		Check(Thrown.Num() > Before, TEXT("a lit stick thrown"));
		if (FThrown* T = ThrownStick()) T->Fuse = 30.f;
		At = ShotTime;
		ShotStep = 3;
		return;
	}
	FThrown* T = ThrownStick();
	if (ShotStep == 3)
	{
		if (T) { T->Pos = Spot; T->Vel = FVector::ZeroVector; T->Fuse = FMath::Max(T->Fuse, 3.f); if (AActor* A = T->Actor.Get()) A->SetActorLocation(Spot); }
		if (ShotTime - At < 1.0f) return;
		Shot(TEXT("ol_tntblue_thrown"));
		AActor* A = T ? T->Actor.Get() : nullptr;
		const UStaticMeshComponent* Fm = T ? T->Flame.Get() : nullptr;
		UE_LOG(LogOutlaws, Display, TEXT("OLRT tntblue thrown: %s; at %s"), A ? *TntMatInfo(Cast<UPrimitiveComponent>(A->GetRootComponent())) : TEXT("(gone)"), *Where(Spot));
		UE_LOG(LogOutlaws, Display, TEXT("OLRT tntblue thrown flame: mesh %s, %s, height %.2f cm"), Fm && Fm->GetStaticMesh() ? *Fm->GetStaticMesh()->GetName() : TEXT("none"),
			*TntMatInfo(Fm), Fm ? Fm->GetComponentScale().Z : 0.0);
		Check(A != nullptr, TEXT("the thrown stick is there to be seen"));
		if (T) { T->Vel = Fl * 500.0 + FVector(0.0, 0.0, 250.0); T->Fuse = 1.5f; }
		At = ShotTime;
		ShotStep = 4;
		return;
	}
	if (ShotStep == 4)
	{
		static float NextLog = 0.f;
		if (T && ShotTime >= NextLog)
		{
			NextLog = ShotTime + 0.1f;
			UE_LOG(LogOutlaws, Display, TEXT("OLRT tntblue flight +%.1f s: at %s, %s"), ShotTime - At, *T->Pos.ToCompactString(), *Where(T->Pos));
		}
		if (T && ShotTime - At < 3.f) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT tntblue RESULT %s (%d failed)"), Fails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Fails);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

OL_SHOT_MATCH(OLTntBlueTest) { return OLFlags::Param(TEXT("OLTntBlueTest")); }
OL_SHOT_TEST(OLTntBlueTest, 100)
{
	TntBlueTestTick(Dt);             // (BUG-TNTBLUE: OutlawsPlayerTntTest.cpp)
}
#endif // WITH_OUTLAWS_TESTS
