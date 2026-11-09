// -OLShots -OLGroundToolsShot (BUG-HELDVSGROUND, 9 Nov; the user: "in all cases, I prefer the ones in my hand"; the pen: the floor's own): the crowbar, the shovel
// and the pen dropped on the floor in a row 1.5 m ahead, the view from standing height looking down at them, Saved\OLShots\
// ol_groundtools.png.  A line per tool: "OLSHOT groundtools <stem>: model <made|SPRITE ONLY>, <size> cm" (the drawn model's
// bounds), then "OLSHOT groundtools RESULT".  With -OLOldGroundItems: the floor's own models (the before).

#include "ShotTestCommon.h"
#include "OutlawsSpriteActor.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLGroundToolsShot) { return OLFlags::Param(TEXT("OLGroundToolsShot")); }

OL_SHOT_TEST(OLGroundToolsShot, 2584)
{
	static int32 Step = 0, GtPass = 0, GtFail = 0;
	static float Yaw0 = -1000.f;
	static TArray<TWeakObjectPtr<AActor>> Items;
	if (Step < 0 || !L || ShotTime < 5.f) return;
	if (Yaw0 < -999.f) Yaw0 = static_cast<float>(GetActorRotation().Yaw);
	const TCHAR* Stems[] = { TEXT("GCROWBAR"), TEXT("GSHOVEL"), TEXT("GINK1") };
	if (Step == 0)
	{
		// the first clear way from the player, the three 1.5 m out across it, 60 cm apart
		const double Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, Half);
		FCollisionQueryParams Q(TEXT("OLGroundTools"), false, this);
		FVector Dir = FRotator(0.0, Yaw0, 0.0).Vector();
		for (int32 d = 0; d < 8; ++d)
		{
			const FVector Try = FRotator(0.0, Yaw0 + 45.0 * d, 0.0).Vector();
			if (!GetWorld()->LineTraceTestByChannel(Feet + FVector(0.0, 0.0, 60.0), Feet + FVector(0.0, 0.0, 60.0) + Try * 300.0, ECC_Visibility, Q)) { Dir = Try; break; }
		}
		const FVector Side = FVector::CrossProduct(FVector::UpVector, Dir);
		FMath::RandInit(2584);
		for (int32 i = 0; i < 3; ++i) Items.Add(L->SpawnGroundItem(Stems[i], Feet + Dir * 150.0 + Side * (60.0 * (i - 1))));
		if (Controller) Controller->SetControlRotation(FRotator(-40.0, Dir.Rotation().Yaw, 0.0));
		Step = 1;
		return;
	}
	if (Step == 1 && ShotTime > 8.f)
	{
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_groundtools.png"), false, false);
		// the floor's size: the sprite's in the world (the floor json's sprite_world_cm), as every pickup is drawn (the user, 9 Nov)
		const double HandLen[] = { 87.7, 239.5, 0.0 };
		for (int32 i = 0; i < 3; ++i)
		{
			const AOutlawsSpriteActor* S = Cast<AOutlawsSpriteActor>(Items.IsValidIndex(i) ? Items[i].Get() : nullptr);
			const bool bModel = S && S->Model;
			const FVector Sz = bModel ? S->Model->Bounds.GetBox().GetSize() : FVector::ZeroVector;
			const double Len = FVector2D(Sz.X, Sz.Y).Size();      // (laid at any turn: the plan's diagonal bounds the length)
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT groundtools %s: model %s, %s cm"), Stems[i], bModel ? TEXT("made") : TEXT("SPRITE ONLY"),
				bModel ? *Sz.ToString() : TEXT("-"));
			if (i == 2)
			{
				// the pen stays the floor's own inkwell and quill (the user, 9 Nov), standing as it always has
				const bool bOk = bModel && Sz.Z > 20.0;
				(bOk ? GtPass : GtFail)++;
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT groundtools check %s: %s is the floor's own inkwell and quill (%.1f cm high, over 20)"), bOk ? TEXT("PASS") : TEXT("FAIL"), Stems[i], Sz.Z);
				continue;
			}
			const double Long = FMath::Max(Sz.X, Sz.Y);
			// the held shape, not the floor's own model: its triangles (held GCROWBAR3D 972, GSHOVEL3D 5320; the floor's 228, 948)
			int32 Tris = 0;
			if (UProceduralMeshComponent* Pm = bModel ? Cast<UProceduralMeshComponent>(S->Model.Get()) : nullptr)
				for (int32 k = 0; k < Pm->GetNumSections(); ++k) if (const FProcMeshSection* Sec = Pm->GetProcMeshSection(k)) Tris += Sec->ProcIndexBuffer.Num() / 3;
			const int32 HeldTris[] = { 972, 5320 };
			const bool bOk = bModel && Tris >= HeldTris[i] && Sz.Z < 0.15 * Long && Long <= HandLen[i] * 1.1 && Len >= HandLen[i] * 0.9;
			(bOk ? GtPass : GtFail)++;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT groundtools check %s: %s is the held shape (%d triangles drawn), flat (%.1f cm high, under 15%% of its length) at the sprite's size (%.1f cm long, the sprite's %.1f)"),
				bOk ? TEXT("PASS") : TEXT("FAIL"), Stems[i], Tris, Sz.Z, Long, HandLen[i]);
		}
		Step = 2;
		return;
	}
	if (Step == 2 && ShotTime > 10.f)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT groundtools RESULT %d PASS, %d FAIL"), GtPass, GtFail);
		Step = -1;
		FGenericPlatformMisc::RequestExit(false);
	}
}

#endif // WITH_OUTLAWS_TESTS
