// REQ-AIRCATCH (user 4 Nov: jump + ledge grabs are hard to time; on foot is fine): -OLShots -OLLevel=mill -OLNoFrontEnd
// -OLAirCatchProbe [-OLOldAirCatch].  The mill's walkway 572 at x 308, its lip 240 cm over the hall's floor (the climb probe's
// ledge), jumped at with the body and the grips driven as a hand drives them: an early squeeze held till the ledge comes in
// reach, the same squeeze let go; a step past the ledge longer than the reach is tall, each way; a jump that stops 10 cm short
// of the reach and one under a ceiling shutting the top off (both never caught, the reach not widened); a squeeze on an item
// and on a forend, then the ledge in reach; a let-go on the ledge with the other grip held; on foot; off the walkway on purpose
// with the grip held from the ground, and with a fresh squeeze in the air; a second jump on a grip held since the first; the
// tick; a gun in the other hand and in the same hand.  With -OLOldAirCatch the same cases with the old answers.
// Lines "OLSHOT aircatch <n> PASS|FAIL: ..." and "OLSHOT aircatch RESULT".
#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsGeometry.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Camera/CameraComponent.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "MotionControllerComponent.h"
#include "Engine/World.h"

#if WITH_OUTLAWS_TESTS

namespace
{
	enum class EAirKind : uint8 { Held, Hitch, OnFoot, DropOff, LetGoOnLedge, Tick, Item, Forend, TwoJumps };
	struct FAirCase
	{
		const TCHAR* What = TEXT("");
		EAirKind Kind = EAirKind::Held;
		int32 Hand = 1;             // the hand that goes for the ledge
		double HandUp = 100.0;      // that hand over the feet, 6 cm out from the wall's face
		double Rise = 150.0;        // the jump, cm
		bool bCatch = false;        // caught, as built
		bool bCatchOld = false;     // caught, -OLOldAirCatch
		bool bLetGo = false;        // (Held) the squeeze let go a moment later
		bool bCeiling = false;      // (Held) a ceiling just over the top
		bool bGround = false;       // (DropOff) squeezed on the ground, not in the air
		double Step = 0.0;          // (Hitch) cm, one frame, + up
		int32 Gun = -1;             // (Held) the weapon in the hand it says
		int32 GunHand = 0;
		bool bNeverReach = false;   // the ledge must never have been in reach
	};
	struct FAirProbe
	{
		TArray<FAirCase> Cases;
		int32 CaseI = -1, Step = 0, Stage = 0, Pass = 0, Fail = 0;
		float T = 0.f, At = 0.f, Air = 0.f;
		bool bSqueezed = false, bReachAtSqueeze = false, bReachSeen = false, bEndsClear = false, bTook = false;
		bool bLanded = false;
		int32 Ticks0 = 0, FramesInAir = 0;
		double HighestHand = -1e9;
		FVector Stand = FVector::ZeroVector, Off[2];
		TWeakObjectPtr<AActor> Slab;
		FString Note;
	};
	FAirProbe GAir;

	const TCHAR* GripAxis(int32 H) { return H == 0 ? TEXT("OLVR_LeftGrip") : TEXT("OLVR_RightGrip"); }

	void AirCases(TArray<FAirCase>& Out)
	{
		auto Add = [&Out](const TCHAR* What, EAirKind Kind) -> FAirCase& { FAirCase& C = Out.AddDefaulted_GetRef(); C.What = What; C.Kind = Kind; return C; };
		{ FAirCase& C = Add(TEXT("an early squeeze held till the ledge comes in reach"), EAirKind::Held); C.bCatch = true; }
		{ FAirCase& C = Add(TEXT("the same squeeze let go before the ledge comes"), EAirKind::Held); C.bLetGo = true; }
		{ FAirCase& C = Add(TEXT("a hitch's step down past the ledge"), EAirKind::Hitch); C.HandUp = 150.0; C.Step = -100.0; C.bCatch = true; }
		{ FAirCase& C = Add(TEXT("a hitch's step up past the ledge"), EAirKind::Hitch); C.HandUp = 150.0; C.Step = 100.0; C.bCatch = true; }
		{ FAirCase& C = Add(TEXT("a jump that stops 10 cm short of the reach"), EAirKind::Held); C.Rise = 85.0; C.bNeverReach = true; }
		{ FAirCase& C = Add(TEXT("a ceiling shuts the top off (never caught)"), EAirKind::Held); C.bCeiling = true; C.bNeverReach = true; }
		{ FAirCase& C = Add(TEXT("a squeeze on an item, then the ledge in reach"), EAirKind::Item); }
		{ FAirCase& C = Add(TEXT("a squeeze on a forend, then the ledge in reach"), EAirKind::Forend); C.Hand = 0; }
		{ FAirCase& C = Add(TEXT("a let-go on the ledge, the other grip held since the hang"), EAirKind::LetGoOnLedge); C.Hand = 0; }
		{ FAirCase& C = Add(TEXT("on foot: the squeeze held, the hand raised to the ledge"), EAirKind::OnFoot); C.HandUp = 215.0; }
		{ FAirCase& C = Add(TEXT("off the walkway on purpose, the grip held from the ground"), EAirKind::DropOff); C.HandUp = 150.0; C.bGround = true; }
		{ FAirCase& C = Add(TEXT("off the walkway, a fresh squeeze in the air"), EAirKind::DropOff); C.HandUp = 150.0; C.bCatch = true; }
		{ FAirCase& C = Add(TEXT("a second jump on the grip held since the first"), EAirKind::TwoJumps); }
		{ FAirCase& C = Add(TEXT("a tick when the ledge comes in reach, no grip"), EAirKind::Tick); C.Rise = 130.0; }
		{ FAirCase& C = Add(TEXT("the left hand's squeeze, a gun in the right"), EAirKind::Held); C.Hand = 0; C.Gun = 1; C.GunHand = 1; C.bCatch = true; }
		{ FAirCase& C = Add(TEXT("the squeeze of the hand with the gun"), EAirKind::Held); C.Gun = 1; C.GunHand = 1; C.bCatch = true; }
	}
}

OL_SHOT_MATCH(OLAirCatchProbe) { return OLFlags::Param(TEXT("OLAirCatchProbe")); }

OL_SHOT_TEST(OLAirCatchProbe, 2350)
{
	FAirProbe& P = GAir;
	if (L->IsFrontEnd() || L->IsRebuildPending() || L->IsMoviePlaying() || bMenuOpen) return;
	L->StartSaveDelay = -1.f;           // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	UCharacterMovementComponent* M = GetCharacterMovement();
	UCapsuleComponent* Cap = GetCapsuleComponent();
	const float Half = Cap->GetScaledCapsuleHalfHeight();
	const float R = Cap->GetScaledCapsuleRadius();
	const bool bOld = OldAirCatch();
	// the climb probe's ledge: the walkway 572 at x 308, over the hall's floor 21
	const double WX = 308.0, WZ = 16.0 - (WX - 301.0) * 40.0 / 59.0;
	const FVector Lip = FOutlawsGeometry::ToUE(WX, 29.0, WZ);
	const FVector In = FOutlawsGeometry::ToUE(0.561, 0.0, 0.8276).GetSafeNormal();
	const double FloorZ = FOutlawsGeometry::ToUE(WX, 21.0, WZ).Z;
	P.T += Dt;
	if (P.T < 3.f) return;               // (the level settled, the fade in done)
	if (P.CaseI < 0)
	{
		bVR = true;                      // (the headset's rules and its fall, this run only)
		BaseGravityScale = 1.5f;
		bCrouchHeld = bStandPending = bAutoCrouched = false;
		CrouchAmount = 1.f;
		CrouchDrop = CrouchFull = 0.f;
		ShownDrop = ShownDropVel = 0.f;
		HeightOffset = 0.f;
		Camera->SetRelativeLocation(FVector(0.0, 0.0, 170.0));
		AirCases(P.Cases);
		L->GiveAllWeapons();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT aircatch on: %d cases, %s, lip %s, into it %s, floor z %.0f"), P.Cases.Num(), bOld ? TEXT("-OLOldAirCatch") : TEXT("as built"), *Lip.ToCompactString(), *In.ToCompactString(), FloorZ);
		P.CaseI = 0;
	}
	if (!P.Cases.IsValidIndex(P.CaseI))
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT aircatch RESULT %s (%d passed, %d failed, %s)"), P.Fail == 0 && P.Pass > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), P.Pass, P.Fail, bOld ? TEXT("-OLOldAirCatch") : TEXT("as built"));
		ShotTime = -1.f;
		FGenericPlatformMisc::RequestExit(false);
		return;
	}
	const FAirCase& C = P.Cases[P.CaseI];
	const int32 H = C.Hand, O = 1 - H;
	auto PutHands = [&]() { for (int32 i = 0; i < 2; ++i) GripOf(i)->SetWorldLocation(GetActorLocation() + P.Off[i]); };
	// the hand on the wall's side, Up over the feet; the other one low, behind the body, out of every reach
	auto HandOver = [&](double Up) { return FVector(Lip.X - In.X * 6.0 - P.Stand.X, Lip.Y - In.Y * 6.0 - P.Stand.Y, Up - Half); };
	auto OutOfReach = [&]() { return FVector(-In.X * 30.0, -In.Y * 30.0, 50.0 - Half); };
	auto InReach = [&](int32 Which) { FVector Gp, Li, Tp; return FindLedge(Which, Gp, Li, Tp, false); };
	auto Launch = [&](double Rise)
	{
		M->SetMovementMode(MOVE_Falling);
		M->Velocity = FVector(0.0, 0.0, FMath::Sqrt(2.0 * FMath::Abs(M->GetGravityZ()) * Rise));
		P.Air = 0.f;
		P.FramesInAir = 0;
	};
	auto Squeeze = [&](int32 Which, bool bOn) { TestAxes.Add(GripAxis(Which), bOn ? 1.f : 0.f); };

	if (P.Step == 0)
	{
		// stood on the hall's floor, the ledge in front (the walkway's top for the drops), the hands where the case wants them
		TestAxes.Reset();
		if (IsClimbing()) ClimbWheelTestLetGo(GrabHand);      // (a fall, not the climb onto the top)
		Equip(0, -1);
		Equip(1, -1);
		bSupport[0] = bSupport[1] = false;
		M->SetMovementMode(MOVE_Walking);
		M->Velocity = FVector::ZeroVector;
		P.Stand = FVector(Lip.X - In.X * (R + 6.0), Lip.Y - In.Y * (R + 6.0), FloorZ + Half + 2.0);
		const FVector Start = C.Kind == EAirKind::DropOff ? FVector(Lip.X + In.X * (R + 10.0), Lip.Y + In.Y * (R + 10.0), Lip.Z + Half + 2.0) : P.Stand;
		TeleportTo(Start, FRotator(0.0, In.Rotation().Yaw, 0.0), false, true);
		if (Controller) Controller->SetControlRotation(FRotator(0.0, In.Rotation().Yaw, 0.0));
		P.Off[H] = HandOver(C.HandUp);
		P.Off[O] = OutOfReach();
		if (C.Kind == EAirKind::DropOff) P.Off[H] = FVector(-In.X * (R - 2.0), -In.Y * (R - 2.0), 60.0 - Half);     // (out over the edge, low)
		if (C.Kind == EAirKind::OnFoot) P.Off[H] = HandOver(100.0);       // (under the ledge's reach till the squeeze is made)
		if (C.Gun >= 0) Equip(C.GunHand, C.Gun);
		if (C.Kind == EAirKind::Forend) TestBreakArm(O, 3, 2, 10);
		if (C.bCeiling)
		{
			// a slab 6 cm over the top, reaching out past the face: the hand under it finds the room above's floor through it
			// (the rule of user 2026-10-01); visibility traces only, so the body jumps as it did
			if (AActor* S = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform(FRotator(0.0, In.Rotation().Yaw, 0.0), Lip + FVector(0.0, 0.0, 16.0))))
			{
				UBoxComponent* B = NewObject<UBoxComponent>(S);
				S->SetRootComponent(B);
				B->SetBoxExtent(FVector(60.0, 150.0, 10.0));
				B->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
				B->SetCollisionResponseToAllChannels(ECR_Ignore);
				B->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
				B->RegisterComponent();
				B->SetWorldLocationAndRotation(Lip + FVector(0.0, 0.0, 16.0), FRotator(0.0, In.Rotation().Yaw, 0.0));
				P.Slab = S;
			}
		}
		PutHands();
		P.At = P.T;
		P.bSqueezed = P.bReachAtSqueeze = P.bReachSeen = P.bTook = P.bLanded = false;
		P.bEndsClear = true;
		P.Ticks0 = AirTicks;
		P.HighestHand = -1e9;
		P.Stage = 0;
		P.Note.Reset();
		P.Step = 1;
		return;
	}
	PutHands();
	if (P.Step == 1)
	{
		if (P.T < P.At + 0.5f) return;
		if (C.Kind == EAirKind::DropOff && C.bGround)
		{
			// squeezed where the body stands (on nothing: the chain finds no ledge at the waist), and held
			Squeeze(H, true);
			if (P.T < P.At + 0.7f) return;
			if (GrabHand >= 0) { P.Note = TEXT(" (taken on the ground!)"); }
		}
		if (C.Kind == EAirKind::OnFoot || C.Kind == EAirKind::Hitch)
		{
			if (C.Kind == EAirKind::OnFoot)
			{
				Squeeze(H, true);
				P.bSqueezed = true;
				P.At = P.T;
				P.Step = 2;
				return;
			}
			// up in the air, the hand 50 cm past the top on the step's near side
			const double Feet = Lip.Z - (C.Step < 0.0 ? -50.0 : 50.0) - C.HandUp;
			P.Stand.Z = Feet + Half;
			SetActorLocation(P.Stand, false, nullptr, ETeleportType::TeleportPhysics);
			M->SetMovementMode(MOVE_Falling);
			M->Velocity = FVector::ZeroVector;
			PutHands();
			P.At = P.T;
			P.Step = 2;
			return;
		}
		if (C.Kind == EAirKind::DropOff)
		{
			// off the edge: the body out over the hall, against the wall, falling
			P.Off[H] = HandOver(C.HandUp);
			SetActorLocation(FVector(P.Stand.X, P.Stand.Y, Lip.Z + Half + 1.0), false, nullptr, ETeleportType::TeleportPhysics);
			M->SetMovementMode(MOVE_Falling);
			M->Velocity = FVector::ZeroVector;
			PutHands();
			P.Air = 0.f;
			P.FramesInAir = 0;
		}
		else
		{
			Launch(C.Kind == EAirKind::TwoJumps ? 30.0 : C.Rise);
			if (C.Kind == EAirKind::TwoJumps) P.Off[H] = HandOver(40.0);
		}
		P.At = P.T;
		P.Step = 2;
		return;
	}
	if (P.Step == 2)
	{
		P.Air += Dt;
		++P.FramesInAir;
		P.HighestHand = FMath::Max(P.HighestHand, GripOf(H)->GetComponentLocation().Z);
		P.bTook = P.bTook || (IsClimbing() && GrabHand == H);
		const bool bOnGround = M->IsMovingOnGround();
		bool bEnd = P.T > P.At + 4.f;
		switch (C.Kind)
		{
		case EAirKind::Held:
			if (!P.bSqueezed && P.FramesInAir >= 2)
			{
				P.bReachAtSqueeze = InReach(H);
				Squeeze(H, true);
				P.bSqueezed = true;
				P.At = P.T;
			}
			else if (C.bLetGo && P.bSqueezed && P.T > P.At + 0.03f)
			{
				Squeeze(H, false);
			}
			bEnd = bEnd || IsClimbing() || (bOnGround && P.T > P.At + 0.1f);
			break;
		case EAirKind::Hitch:
			if (P.Stage == 0)
			{
				M->Velocity = FVector::ZeroVector;
				P.bReachAtSqueeze = InReach(H);
				Squeeze(H, true);
				P.bSqueezed = true;
				if (P.T > P.At + 0.15f) P.Stage = 1;
			}
			else if (P.Stage == 1)
			{
				// the step: neither end of it has the ledge in reach, so only the path can find it
				M->Velocity = FVector::ZeroVector;
				const bool bBefore = InReach(H);
				P.Stand.Z += C.Step;
				SetActorLocation(P.Stand, false, nullptr, ETeleportType::TeleportPhysics);
				PutHands();
				const bool bAfter = InReach(H);
				if (bBefore || bAfter) P.Note = FString::Printf(TEXT(" (in reach at an end of the step: before %d, after %d)"), bBefore ? 1 : 0, bAfter ? 1 : 0);
				M->Velocity = FVector(0.0, 0.0, C.Step < 0.0 ? -300.0 : 0.0);
				P.Stage = 2;
				P.At = P.T;
			}
			else
			{
				bEnd = IsClimbing() || P.T > P.At + 1.5f || (bOnGround && P.T > P.At + 0.1f);
			}
			break;
		case EAirKind::OnFoot:
			// a moment on the ground with the squeeze made, then the hand up to the ledge, the grip still held
			if (P.T > P.At + 0.2f) P.Off[H] = HandOver(C.HandUp);
			P.bReachSeen = P.bReachSeen || InReach(H);
			bEnd = IsClimbing() || P.T > P.At + 0.7f;
			break;
		case EAirKind::DropOff:
			if (!C.bGround && !P.bSqueezed && P.FramesInAir >= 2)
			{
				P.bReachAtSqueeze = InReach(H);
				Squeeze(H, true);
				P.bSqueezed = true;
			}
			bEnd = bEnd || IsClimbing() || (bOnGround && P.T > P.At + 0.1f);
			break;
		case EAirKind::TwoJumps:
			if (P.Stage == 0)
			{
				if (!P.bSqueezed && P.FramesInAir >= 2) { Squeeze(H, true); P.bSqueezed = true; P.bEndsClear = bAirCatch[H] || bOld; }
				if (bAirCatch[H]) P.Stage = 1;
				if (P.bSqueezed && bOnGround && P.T > P.At + 0.1f) P.Stage = 2;
			}
			else if (P.Stage == 1)
			{
				if (bOnGround && P.T > P.At + 0.1f) P.Stage = 2;
			}
			else if (P.Stage == 2)
			{
				if (bAirCatch[H]) P.Note = TEXT(" (still armed on the ground)");
				P.Off[H] = HandOver(100.0);
				Launch(150.0);
				P.Stage = 3;
				P.At = P.T;
			}
			else
			{
				bEnd = bEnd || IsClimbing() || (bOnGround && P.T > P.At + 0.1f);
			}
			break;
		case EAirKind::Tick:
			bEnd = bEnd || IsClimbing() || (bOnGround && P.T > P.At + 0.1f);
			break;
		case EAirKind::Item:
			if (!P.bSqueezed && P.FramesInAir >= 2)
			{
				// a pickup brought to the hand as the squeeze closes on it
				TArray<AOutlawsLevelActor::FPickupDebug> Items;
				L->DebugPickups(Items);
				if (Items.Num() > 0 && Items[0].Actor)
				{
					Items[0].Actor->SetActorLocation(GripOf(H)->GetComponentLocation() - FVector(0.0, 0.0, Items[0].Actor->GetActorScale3D().Z * 0.5));
					Squeeze(H, true);
					P.bSqueezed = true;
					P.At = P.T;
				}
				else
				{
					P.Note = TEXT(" (no pickup lying about)");
					bEnd = true;
				}
			}
			else if (P.bSqueezed && P.Stage == 0 && P.T > P.At + 0.03f)
			{
				if (!HeldItem[H].IsValid()) P.Note = TEXT(" (the item was not taken)");
				P.Stage = 1;
			}
			bEnd = bEnd || IsClimbing() || (bOnGround && P.T > P.At + 0.1f);
			break;
		case EAirKind::Forend:
			if (!P.bSqueezed && P.FramesInAir >= 2)
			{
				TestFrontHandAt(O, 0.f);
				P.Off[H] = GripOf(H)->GetComponentLocation() - GetActorLocation();
				Squeeze(H, true);
				P.bSqueezed = true;
				P.At = P.T;
			}
			else if (P.bSqueezed && P.Stage == 0 && P.T > P.At + 0.03f)
			{
				if (!bSupport[H]) P.Note = TEXT(" (the forend was not taken)");
				P.Off[H] = HandOver(C.HandUp);
				P.Stage = 1;
			}
			bEnd = bEnd || IsClimbing() || (bOnGround && P.T > P.At + 0.1f);
			break;
		case EAirKind::LetGoOnLedge:
			// the right hand takes the ledge with a squeeze in reach (either way of building), the left squeezes on nothing while
			// the body hangs, the right lets go and the fall passes the ledge with the left there and still held
			if (P.Stage == 0)
			{
				P.Off[O] = HandOver(C.HandUp);
				P.Off[H] = OutOfReach();
				if (InReach(O)) { Squeeze(O, true); P.Stage = 1; P.At = P.T; }
			}
			else if (P.Stage == 1)
			{
				if (IsClimbing() && GrabHand == O)
				{
					P.Off[O] = HandOver(195.0);     // (over the eyes: a let-go there is a fall, not the climb)
					P.Stage = 2;
					P.At = P.T;
				}
				else if (P.T > P.At + 1.f)
				{
					P.Note = TEXT(" (the ledge was not taken)");
					bEnd = true;
				}
			}
			else if (P.Stage == 2)
			{
				if (P.T > P.At + 0.3f) { Squeeze(H, true); P.Stage = 3; P.At = P.T; }
			}
			else if (P.Stage == 3)
			{
				if (P.T > P.At + 0.1f)
				{
					Squeeze(O, false);
					P.Stage = 4;
					P.At = P.T;
				}
			}
			else if (P.Stage == 4)
			{
				if (!IsClimbing())
				{
					P.Off[H] = HandOver(195.0);
					P.Stage = 5;
					P.At = P.T;
				}
				else if (P.T > P.At + 0.5f)
				{
					P.Note = TEXT(" (the ledge was not let go of)");
					bEnd = true;
				}
			}
			else
			{
				if (bAirCatch[H] || bAirCatch[O]) P.Note = TEXT(" (a catch armed)");
				bEnd = bEnd || IsClimbing() || (bOnGround && P.T > P.At + 0.1f);
			}
			break;
		}
		if (C.Kind != EAirKind::OnFoot) P.bReachSeen = P.bReachSeen || (!IsClimbing() && InReach(H));
		if (!bEnd) return;
		const bool bGot = IsClimbing() && GrabHand == H;
		const bool bWant = bOld ? C.bCatchOld : C.bCatch;
		bool bOk = bGot == bWant && P.Note.IsEmpty();
		// what the case is for: a squeeze that really was early; a ledge that really was (or was not) in reach and not taken
		if (C.Kind == EAirKind::Held && !C.bLetGo && !C.bNeverReach && P.bReachAtSqueeze) { bOk = false; P.Note += TEXT(" (the ledge already in reach at the squeeze)"); }
		if (C.bNeverReach && P.bReachSeen) { bOk = false; P.Note += TEXT(" (the ledge came in reach)"); }
		const bool bNeedReach = !bWant && !C.bNeverReach && C.Kind != EAirKind::Hitch;
		if (bNeedReach && !P.bReachSeen) { bOk = false; P.Note += TEXT(" (the ledge never came in reach: no test)"); }
		if (C.Kind == EAirKind::Tick)
		{
			const int32 Want = bOld ? 0 : 1, Got = AirTicks - P.Ticks0;
			if (Got != Want) { bOk = false; P.Note += FString::Printf(TEXT(" (%d ticks, wanted %d)"), Got, Want); }
		}
		if (C.Gun >= 0 && Equipped[C.GunHand] != C.Gun) { bOk = false; P.Note += TEXT(" (the gun left the hand)"); }
		if (C.Gun >= 0 && C.GunHand == H && bGot && !bGripUsed[H]) { bOk = false; P.Note += TEXT(" (the catch did not count as the grip's take)"); }
		if (bAirCatch[0] || bAirCatch[1])
		{
			if (bGot || bOnGround) { bOk = false; P.Note += TEXT(" (a catch left armed)"); }
		}
		(bOk ? P.Pass : P.Fail) += 1;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT aircatch %d %s: %s: caught %d (want %d), the ledge in reach at any time %d, hand's highest %.0f (top %.0f), ticks %d%s"), P.CaseI, bOk ? TEXT("PASS") : TEXT("FAIL"), C.What,
			bGot ? 1 : 0, bWant ? 1 : 0, P.bReachSeen ? 1 : 0, P.HighestHand, double(Lip.Z), AirTicks - P.Ticks0, *P.Note);
		if (AActor* S = P.Slab.Get()) S->Destroy();
		P.Slab = nullptr;
		++P.CaseI;
		P.Step = 0;
	}
}

#endif // WITH_OUTLAWS_TESTS
