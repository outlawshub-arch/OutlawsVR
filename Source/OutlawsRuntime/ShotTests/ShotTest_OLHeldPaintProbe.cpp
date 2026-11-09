// -OLShots -OLHeldPaintProbe (REQ-SPRITEPAINT, 9 Nov; the user: "Yes, original paint"): the held crowbar and shovel, and their copies
// on the floor, are painted from the floor models' own sprite paint (the player's install), not item3d's flat colours.  Each one's
// drawn texture (its first section's "Tex") against OutlawsItem3d::Texture of the floor model's key.  Lines
// "OLSHOT heldpaint check PASS|FAIL: <what>", then "OLSHOT heldpaint RESULT <n> PASS, <m> FAIL".  -OLOldHeldPaint: must FAIL.

#include "ShotTestCommon.h"
#include "OutlawsItem3d.h"
#include "OutlawsSpriteActor.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLHeldPaintProbe) { return OLFlags::Param(TEXT("OLHeldPaintProbe")); }

OL_SHOT_TEST(OLHeldPaintProbe, 2586)
{
	static int32 Step = 0, Pass = 0, Fail = 0;
	static TArray<TWeakObjectPtr<AActor>> Items;
	if (Step < 0 || !L || ShotTime < 6.f) return;
	auto Check = [&](bool bOk, const FString& What)
	{
		(bOk ? Pass : Fail)++;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT heldpaint check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
	};
	auto TexOf = [](UPrimitiveComponent* C) -> UTexture*
	{
		UTexture* T = nullptr;
		if (UMaterialInstanceDynamic* M = C ? Cast<UMaterialInstanceDynamic>(C->GetMaterial(0)) : nullptr) M->GetTextureParameterValue(FName(TEXT("Tex")), T);
		return T;
	};
	auto Want = [&](const TCHAR* Stem) { return OutlawsItem3d::Texture(FPaths::Combine(OutlawsItem3d::ShippedDir(), FString(Stem) + TEXT(".png")), L); };
	auto ToolMesh = [&](int32 H) -> UPrimitiveComponent*
	{
		if (!ToolRoot[H]) return nullptr;
		TArray<USceneComponent*> Kids;
		ToolRoot[H]->GetChildrenComponents(true, Kids);
		for (USceneComponent* K : Kids) if (UProceduralMeshComponent* P = Cast<UProceduralMeshComponent>(K)) return P;
		return nullptr;
	};
	if (Step == 0)
	{
		L->AddCount(TEXT("ishovel"), 1);
		L->AddCount(TEXT("icrowbar"), 1);
		EquipTool(0, TEXT("ishovel"));
		EquipTool(1, TEXT("icrowbar"));
		const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
		Items.Add(L->SpawnGroundItem(TEXT("GSHOVEL"), Feet + GetActorForwardVector() * 150.0));
		Items.Add(L->SpawnGroundItem(TEXT("GCROWBAR"), Feet + GetActorForwardVector() * 150.0 + GetActorRightVector() * 80.0));
		Step = 1;
		return;
	}
	if (Step == 1)
	{
		const TCHAR* Stems[] = { TEXT("GSHOVEL"), TEXT("GCROWBAR") };
		for (int32 i = 0; i < 2; ++i)
		{
			UTexture* W = Want(Stems[i]);
			UTexture* Hand = TexOf(ToolMesh(i));
			const AOutlawsSpriteActor* S = Cast<AOutlawsSpriteActor>(Items.IsValidIndex(i) ? Items[i].Get() : nullptr);
			UTexture* Floor = S && S->Model ? TexOf(S->Model.Get()) : nullptr;
			Check(W && Hand == W, FString::Printf(TEXT("the held %s wears the floor sprite's paint (%s)"), Stems[i], Hand ? *Hand->GetName() : TEXT("none")));
			Check(W && Floor == W, FString::Printf(TEXT("the %s on the floor wears it too (%s)"), Stems[i], Floor ? *Floor->GetName() : TEXT("none")));
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT heldpaint RESULT %d PASS, %d FAIL"), Pass, Fail);
		Step = -1;
		FGenericPlatformMisc::RequestExit(false);
	}
}

#endif // WITH_OUTLAWS_TESTS
