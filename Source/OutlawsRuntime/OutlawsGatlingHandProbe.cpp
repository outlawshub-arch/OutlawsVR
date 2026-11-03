// REQ-GATLINGHAND's probe (the user's play notes of 3 Nov, snapshot 2026-11-03_19-45-51: "detach the Gatling gun from my
// head. For the aim... it seems to follow my head and I'd prefer it to follow my hand like any other gun"):
//   -OLShots -OLNoFrontEnd -OLLevel=<level> -OLGatlingHandProbe [-OLOldGatlingAim]
// The headset's rules: the Gatling in the right hand (the left its crank's), the hand frame and the head set by the probe and
// the guns' update run at once (no headset to move them back).  (1) the head turned 40 degrees with the hand still: the
// aim (the gun's forward) and its place do not move; (2) the hand turned 30 degrees: the aim turns with it, along the
// hand's pointing; (3) a shot: its line from the muzzle runs where the hand points (within 2 degrees; the head 40 degrees
// off, so no aim assist from the view).  Also the snapshot's hand line names the Gatling.  -OLOldGatlingAim: the head's
// heading, as before ((1) and (2) then fail, the control).  Lines "OLSHOT gatlinghand check PASS|FAIL: ..." and
// "OLSHOT gatlinghand RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "ProceduralMeshComponent.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsGatlingHandProbeLocal
{
	int32 Stage = 0, Passes = 0, Fails = 0;
	float StageT = 0.f;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT gatlinghand check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}
	double AngleDeg(const FVector& A, const FVector& B)
	{
		return FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(A.GetSafeNormal(), B.GetSafeNormal()), -1.0, 1.0)));
	}
}

OL_SHOT_MATCH(OLGatlingHandProbe) { return OLFlags::Param(TEXT("OLGatlingHandProbe")); }

OL_SHOT_TEST(OLGatlingHandProbe, 2150)
{
	using namespace OutlawsGatlingHandProbeLocal;
	if (!L || ShotTime < 0.f || Stage < 0) return;
	const bool bOld = OLFlags::Param(TEXT("OLOldGatlingAim"));
	constexpr int32 Gatling = 5;
	bVR = true;
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT gatlinghand RESULT %s (%d passed, %d failed; %s, %s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			*L->LevelName, bOld ? TEXT("-OLOldGatlingAim") : TEXT("new"));
		ShotTime = -1.f;
		Stage = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	if (L->IsRebuildPending() || L->IsMoviePlaying() || bMenuOpen) { StageT = 0.f; return; }
	L->StartSaveDelay = -1.f;           // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	L->bInvincible = true;
	StageT += Dt;
	if (Stage == 0)
	{
		if (StageT < 3.f) return;
		L->GiveAllWeapons();
		Equip(1, Gatling);
		Stage = 1;
		StageT = 0.f;
		return;
	}
	if (Stage == 1 && StageT < 1.f) return;
	UProceduralMeshComponent* Gun = WeaponMesh[1];
	if (!Gun || Equipped[1] != Gatling)
	{
		Check(false, FString::Printf(TEXT("the Gatling in the right hand: equipped %d, mesh %s"), Equipped[1], Gun ? TEXT("there") : TEXT("missing")));
		Finish();
		return;
	}
	// the head and the hand set, then the guns' update at once: the gun's place and forward read back
	const double Base = GetActorRotation().Yaw;
	const FVector Ahead = FRotator(0.0, Base, 0.0).Vector(), Right = FRotator(0.0, Base + 90.0, 0.0).Vector();
	const FVector HandAt = Camera->GetComponentLocation() + Ahead * 35.0 + Right * 22.0 - FVector(0.0, 0.0, 30.0);
	auto Pose = [&](double HeadYaw, double HandYaw, FVector& OutPivot, FVector& OutFwd)
	{
		Camera->SetWorldRotation(FRotator(0.0, Base + HeadYaw, 0.0));
		FrameOf(1)->SetWorldTransform(FTransform(FRotator(0.0, Base + HandYaw, 0.0), HandAt));
		Mag[1] = FMath::Max(Mag[1], 20);
		UpdateGuns(1.f / 72.f);
		OutPivot = Gun->GetComponentLocation();
		OutFwd = Gun->GetForwardVector();
	};
	FVector P0, F0, P1, F1, P2, F2;
	Pose(0.0, 0.0, P0, F0);
	Pose(0.0, 0.0, P0, F0);             // (settled: the old rule eases into it)
	Pose(40.0, 0.0, P1, F1);
	const double Turn1 = AngleDeg(F0, F1), Move1 = FVector::Dist(P0, P1);
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT gatlinghand (1) the head 40 deg round, the hand still: the aim turned %.2f deg, the gun moved %.2f cm"), Turn1, Move1);
	if (bOld) Check(Turn1 > 0.2, FString::Printf(TEXT("(1, old rule) the aim follows the head: %.2f deg"), Turn1));
	else Check(Turn1 < 0.5 && Move1 < 1.0, FString::Printf(TEXT("(1) the head 40 deg round with the hand still: the aim turned %.2f deg, the gun moved %.2f cm"), Turn1, Move1));
	Pose(40.0, 30.0, P2, F2);
	const FVector HandFwd = FRotator(0.0, Base + 30.0, 0.0).Vector();
	const double Turn2 = AngleDeg(F0, F2), Off2 = AngleDeg(F2, HandFwd);
	if (bOld) Check(Off2 > 1.0, FString::Printf(TEXT("(2, old rule) the hand turned 30 deg: the aim %.2f deg off the hand's pointing (the head's)"), Off2));
	else Check(Turn2 > 29.0 && Off2 < 1.0, FString::Printf(TEXT("(2) the hand turned 30 deg: the aim turned %.2f deg, %.2f deg off the hand's pointing"), Turn2, Off2));
	// (3) a shot with the hand 30 deg round and the head 40: its line from the muzzle
	FireCooldown[1] = 0.f;
	const int32 Before = Mag[1];
	FireGun(1, false);
	const FVector Impact = SparkFx[1] ? SparkFx[1]->GetComponentLocation() : FVector::ZeroVector;
	const FVector Muzzle = TracerFx[1] ? TracerFx[1]->GetComponentLocation() * 2.0 - Impact : P2;      // (the tracer sits halfway)
	const double Off3 = AngleDeg(Impact - Muzzle, HandFwd);
	if (bOld) Check(Mag[1] < Before, FString::Printf(TEXT("(3, old rule) a shot fired (%d -> %d), its line %.2f deg off the hand's pointing"), Before, Mag[1], Off3));
	else Check(Mag[1] < Before && Off3 < 2.0, FString::Printf(TEXT("(3) a shot (%d -> %d rounds) lands where the hand points: its line %.2f deg off, at %s"), Before, Mag[1], Off3, *Impact.ToCompactString()));
	// the snapshot's hand line (the snapshot info: "Right hand: <name>")
	const FString Name = HeldGunTestName(Equipped[1]);
	Check(Name == TEXT("gatling"), FString::Printf(TEXT("the snapshot names the right hand's gun \"%s\" (was weapon_none)"), *Name));
	Finish();
}

#endif // WITH_OUTLAWS_TESTS
