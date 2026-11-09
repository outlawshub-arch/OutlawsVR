// -OLShots -OLToolSwapProbe (BUG-TOOLSWAP, 9 Nov): one of the ShotTest tests (OutlawsShotTests.h).  The hand-to-hand squeeze
// matrix: each tool and piece of gear in the right hand against the left hand empty, holding the revolver, a held prop, a tool
// or gear; the left hand squeezing with the fists together, the right hand squeezing, and (the shovel) the left hand on the
// shaft.  The user's rule (9 Nov): the crowbar and the pen change hands like a gun against anything; the shovel too with the
// fists together at its grip, and a hand on its shaft holds it two-handed.  A line per case:
//   "OLSHOT toolswap <item> vs <other> (<who squeezes>): R <before> L <before> -> R <after> L <after>: <got>, want <want> PASS|FAIL"
// then "OLSHOT toolswap RESULT <n> PASS, <m> FAIL".  The squeezes are HandSwapSqueezes, the hand update's own.

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLToolSwapProbe) { return OLFlags::Param(TEXT("OLToolSwapProbe")); }

namespace
{
	struct FSwapCase { FString Item, Other, Who, Want; };
}

OL_SHOT_TEST(OLToolSwapProbe, 2575)
{
	static TArray<FSwapCase> SwapCases;
	static int32 Next = 0, Pass = 0, Fail = 0;
	static bool bDone = false;
	if (bDone) return;
	if (SwapCases.Num() == 0)
	{
		const TCHAR* Rows[] = { TEXT("icrowbar"), TEXT("ishovel"), TEXT("iink"), TEXT("ilantern"), TEXT("imap") };
		for (const TCHAR* R : Rows)
		{
			const FString Item = R;
			const FString Tool = Item == TEXT("icrowbar") ? TEXT("iink") : TEXT("icrowbar");
			const FString Gear = Item == TEXT("ilantern") ? TEXT("imap") : TEXT("ilantern");
			const FString Others[] = { TEXT("empty"), TEXT("revolver"), TEXT("prop"), Tool, Gear };
			for (const FString& O : Others)
			{
				// the left hand squeezing: it takes or trades (a held prop is held by its squeeze already: not a case)
				if (O != TEXT("prop")) SwapCases.Add({ Item, O, TEXT("left squeezes"), TEXT("swap") });
				// the right hand (the one holding the item) squeezing: a trade; into an empty hand nothing, as with a gun
				SwapCases.Add({ Item, O, TEXT("right squeezes"), O == TEXT("empty") ? TEXT("none") : TEXT("swap") });
			}
		}
		SwapCases.Add({ TEXT("ishovel"), TEXT("empty"), TEXT("left on the shaft"), TEXT("two-handed") });
		SwapCases.Add({ TEXT("ishovel"), TEXT("revolver"), TEXT("left on the shaft"), TEXT("two-handed") });
	}
	if (ShotTime < 3.f || !L) return;
	int32 Revolver = -1;
	for (int32 i = 0; i < 32; ++i) if (WeaponLabel(i).Contains(TEXT("revolver"))) { Revolver = i; break; }
	auto Clear = [this](int32 H)
	{
		if (HeldBottle[H]) { HeldBottle[H]->DestroyComponent(); HeldBottle[H] = nullptr; }
		Equip(H, -1);
		DropTool(H);
		bSupport[H] = bToolSupport[H] = false;
	};
	auto GiveProp = [this](int32 H)
	{
		UProceduralMeshComponent* M = NewObject<UProceduralMeshComponent>(this);
		M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		M->RegisterComponent();
		M->SetWorldLocation(GripOf(H)->GetComponentLocation());
		TArray<FVector> V = { FVector(-3.0, 0.0, 0.0), FVector(3.0, 0.0, 0.0), FVector(0.0, 0.0, 4.0) };
		TArray<int32> Tri = { 0, 1, 2 };
		M->CreateMeshSection(0, V, Tri, TArray<FVector>(), TArray<FVector2D>(), TArray<FColor>(), TArray<FProcMeshTangent>(), false);
		M->UpdateBounds();
		HeldBottle[H] = M;
		bHeldBreaks[H] = false;
		BottleGrow[H] = 1.f;
		BottleSound[H].Reset();
		BottleHold[H].Reset();
		BottleAxis[H] = FVector::UpVector;
	};
	auto Desc = [this](int32 H) -> FString
	{
		if (Equipped[H] >= 0) return WeaponLabel(Equipped[H]).ToLower();
		if (!HeldTool[H].IsEmpty()) return HeldTool[H];
		if (HeldBottle[H]) return TEXT("prop");
		return TEXT("empty");
	};
	auto Place = [this](int32 H, const FVector& Pos, const FVector& Fl)
	{
		// (the grip, the tracked controller: the hand's frame and what it holds hang under it, and the squeezes measure it)
		GripOf(H)->SetWorldLocationAndRotation(Pos, FRotationMatrix::MakeFromXZ(Fl, FVector::UpVector).ToQuat());
	};
	if (Next >= SwapCases.Num())
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT toolswap RESULT %d PASS, %d FAIL"), Pass, Fail);
		Clear(0); Clear(1);
		bDone = true;
		FGenericPlatformMisc::RequestExit(false);
		return;
	}
	const FSwapCase& C = SwapCases[Next++];
	Clear(0); Clear(1);
	L->AddCount(C.Item, 1);
	EquipTool(1, C.Item);
	if (C.Other == TEXT("revolver")) Equip(0, Revolver);
	else if (C.Other == TEXT("prop")) GiveProp(0);
	else if (C.Other != TEXT("empty")) { L->AddCount(C.Other, 1); EquipTool(0, C.Other); }
	// the hands: the right one out in front, the left one's fist beside it (4 cm), or on the shovel's shaft
	const FVector Eye = Camera->GetComponentLocation();
	const FVector F = Camera->GetForwardVector();
	const FVector Fl = FVector(F.X, F.Y, 0.0).GetSafeNormal(), Rt = FVector::CrossProduct(FVector::UpVector, Fl);
	Place(1, Eye + Fl * 45.0 - FVector::UpVector * 20.0, Fl);
	UpdateTools(FMath::Max(0.001f, Dt));
	const bool bLeft = C.Who != TEXT("right squeezes");
	FVector Want0 = GripOf(1)->GetComponentLocation() - Rt * 4.0;
	if (C.Who == TEXT("left on the shaft") && ToolRoot[1])
	{
		const double Mid = 0.5 * (14.0 + FMath::Max(30.0, ToolTipX[1] - 30.0));
		Want0 = ToolRoot[1]->GetComponentTransform().TransformPosition(FVector(Mid, 0.0, 0.0));
	}
	Place(0, Eye + Fl * 45.0 - FVector::UpVector * 20.0 - Rt * 10.0, Fl);
	GripOf(0)->AddWorldOffset(Want0 - GripOf(0)->GetComponentLocation());
	const double Apart = FVector::Dist(GripOf(0)->GetComponentLocation(), GripOf(1)->GetComponentLocation());
	const FString B1 = Desc(1), B0 = Desc(0);
	const int32 Sq = bLeft ? 0 : 1;
	bGripArmed[0] = bGripArmed[1] = false;
	bGripArmed[Sq] = true;
	HandSwapSqueezes(Sq, 1.f);
	const FString A1 = Desc(1), A0 = Desc(0);
	FString Got = TEXT("none");
	if (bToolSupport[0] && A1 == B1 && A0 == B0) Got = TEXT("two-handed");
	else if (A0 == B1 && A1 == B0) Got = TEXT("swap");
	else if (A1 != B1 || A0 != B0) Got = TEXT("other");
	const bool bOk = Got == C.Want;
	(bOk ? Pass : Fail)++;
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT toolswap %s vs %s (%s): R %s L %s -> R %s L %s: %s, want %s %s (fists %.1f cm apart, reach %.1f)"),
		*C.Item, *C.Other, *C.Who, *B1, *B0, *A1, *A0, *Got, *C.Want, bOk ? TEXT("PASS") : TEXT("FAIL"), Apart, SwapReach());
	bGripArmed[0] = bGripArmed[1] = false;
}

// -OLShots -OLToolSwapShot (the sign-off picture): the crowbar in the right hand and the revolver in the left, the hands apart
// (ol_toolswap_a), brought together for the left hand's squeeze, then apart again (ol_toolswap_b): with the fix the two have
// traded, with -OLOldToolSwap they have not.  "OLSHOT toolswapshot R <..> L <..>" after the squeeze.
OL_SHOT_MATCH(OLToolSwapShot) { return OLFlags::Param(TEXT("OLToolSwapShot")); }

OL_SHOT_TEST(OLToolSwapShot, 2576)
{
	static int32 Step = 0;
	static float Yaw = -1000.f;
	if (Step >= 5 || !L) return;
	if (Step == 0) GEngine->Exec(GetWorld(), TEXT("r.MotionBlurQuality 0"));
	if (Yaw < -999.f) Yaw = static_cast<float>(GetActorRotation().Yaw);
	if (Controller) Controller->SetControlRotation(FRotator(-15.0, Yaw, 0.0));
	const FVector Eye = Camera->GetComponentLocation();
	const FVector F = Camera->GetForwardVector();
	const FVector Fl = FVector(F.X, F.Y, 0.0).GetSafeNormal(), Rt = FVector::CrossProduct(FVector::UpVector, Fl);
	const bool bTogether = Step == 2;
	for (int32 H = 0; H < 2; ++H)
	{
		const double Side = (H == 0 ? -1.0 : 1.0) * (bTogether ? 2.0 : 14.0);
		GripOf(H)->SetWorldLocationAndRotation(Eye + Fl * 42.0 + Rt * Side - FVector::UpVector * 16.0, FRotationMatrix::MakeFromXZ(Fl, FVector::UpVector).ToQuat());
		if (UOutlawsBoxHand* Box = H == 0 ? LeftHand.Get() : RightHand.Get()) { Box->SetVisibility(true, true); Box->SetCurl(1.f, 1.f); }
	}
	UpdateTools(FMath::Max(0.001f, Dt));
	auto Shot = [](const TCHAR* Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(Name) + TEXT(".png"), false, false); };
	auto Desc = [this](int32 H) -> FString
	{
		if (Equipped[H] >= 0) return WeaponLabel(Equipped[H]).ToLower();
		return HeldTool[H].IsEmpty() ? FString(TEXT("empty")) : HeldTool[H];
	};
	if (Step == 0 && ShotTime > 3.f)
	{
		int32 Revolver = -1;
		for (int32 i = 0; i < 32; ++i) if (WeaponLabel(i).Contains(TEXT("revolver"))) { Revolver = i; break; }
		L->AddCount(TEXT("icrowbar"), 1);
		EquipTool(1, TEXT("icrowbar"));
		Equip(0, Revolver);
		++Step;
	}
	else if (Step == 1 && ShotTime > 5.f) { Shot(TEXT("ol_toolswap_a")); ++Step; }
	else if (Step == 2 && ShotTime > 6.f)
	{
		bGripArmed[1] = false;
		bGripArmed[0] = true;
		HandSwapSqueezes(0, 1.f);
		bGripArmed[0] = false;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT toolswapshot R %s L %s"), *Desc(1), *Desc(0));
		++Step;
	}
	else if (Step == 3 && ShotTime > 8.f) { Shot(TEXT("ol_toolswap_b")); ++Step; }
	else if (Step == 4 && ShotTime > 9.f) { ++Step; FGenericPlatformMisc::RequestExit(false); }
}

#endif // WITH_OUTLAWS_TESTS
