// REQ-HELDSWAY (user 2026-10-04): a held bad guy or body swung about, his legs swing behind.  The quad's top edge stays at
// the grip; its two bottom corners follow on a damped spring, on world X and Y only, never Z, so the sprite's height never
// changes.  The bottom is a weight with its own speed, pulled back under the grip by the spring and slowed by the damping:
// moving, it trails behind by about the hand's speed times 2 x damping / sqrt(stiffness); stopped, it swings past a little
// and settles.  The part of the lag toward or away from the viewer also turns the bottom edge about its middle (a yaw), so
// the feet foreshorten as they swing toward or away.  Off (dropped, thrown, the hold over) the spring is reset at once.
// Drawn as a four-corner procedural quad over the sprite's own (SM_SpriteQuad's corners and UVs, its material), the static
// quad hidden in game while it shows; still, the plain quad is drawn again and nothing is updated.
#include "OutlawsSpriteActor.h"
#include "OutlawsRuntime.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "ProceduralMeshComponent.h"
#include "StaticMeshResources.h"

static TAutoConsoleVariable<int32> CVarHeldSwayEnable(TEXT("ol.heldsway.Enable"), 1,
	TEXT("REQ-HELDSWAY: a held bad guy's legs swing behind the hand (1), or hang straight (0)."), ECVF_Default);
static TAutoConsoleVariable<float> CVarHeldSwayStiffness(TEXT("ol.heldsway.Stiffness"), 56.f,
	TEXT("REQ-HELDSWAY: the spring pulling the legs back under the grip (1/s^2; its square root is the swing's rate in rad/s). Lower is heavier and slower."), ECVF_Default);
static TAutoConsoleVariable<float> CVarHeldSwayDamping(TEXT("ol.heldsway.Damping"), 0.6f,
	TEXT("REQ-HELDSWAY: the damping ratio (1 no overshoot, 0.6 about a tenth past, lower rings longer). It also sets how far the legs trail a moving hand."), ECVF_Default);
static TAutoConsoleVariable<float> CVarHeldSwayMaxLag(TEXT("ol.heldsway.MaxLagCm"), 40.f,
	TEXT("REQ-HELDSWAY: the farthest the bottom edge trails the grip, cm across the floor (also held to a third of the sprite's height)."), ECVF_Default);
static TAutoConsoleVariable<float> CVarHeldSwayMaxYaw(TEXT("ol.heldsway.MaxYawDeg"), 30.f,
	TEXT("REQ-HELDSWAY: the bottom edge's turn at the full lag toward or away from the viewer, degrees."), ECVF_Default);

namespace
{
	/** SM_SpriteQuad's corners in its own space and their UVs: top left, top right, bottom left, bottom right as the viewer
	 *  (on +X) sees them.  From the mesh's bounds (1 wide along Y, 1 tall, pivot at the bottom middle): the texture's right
	 *  toward -Y (PlaceQuad's note), V down.  In the editor's game the mesh's own vertices are checked against these once. */
	struct FQuadCorners
	{
		FVector Pos[4];
		FVector2D UV[4];
	};

	const FQuadCorners& CornersOf(const UStaticMesh* Mesh)
	{
		static FQuadCorners C;
		static const UStaticMesh* Made = nullptr;
		static bool bAny = false;
		if (bAny && Made == Mesh) return C;
		bAny = true;
		Made = Mesh;
		FBox B(FVector(0.0, -0.5, 0.0), FVector(0.0, 0.5, 1.0));
		if (Mesh) B = Mesh->GetBoundingBox();
		const double X = B.GetCenter().X;
		C.Pos[0] = FVector(X, B.Max.Y, B.Max.Z);
		C.Pos[1] = FVector(X, B.Min.Y, B.Max.Z);
		C.Pos[2] = FVector(X, B.Max.Y, B.Min.Z);
		C.Pos[3] = FVector(X, B.Min.Y, B.Min.Z);
		C.UV[0] = FVector2D(0.0, 0.0);
		C.UV[1] = FVector2D(1.0, 0.0);
		C.UV[2] = FVector2D(0.0, 1.0);
		C.UV[3] = FVector2D(1.0, 1.0);
#if WITH_EDITOR
		if (Mesh && Mesh->GetRenderData() && Mesh->GetRenderData()->LODResources.Num() > 0)
		{
			const FStaticMeshLODResources& Lod = Mesh->GetRenderData()->LODResources[0];
			const int32 N = Lod.VertexBuffers.PositionVertexBuffer.GetNumVertices();
			int32 Bad = 0;
			for (int32 i = 0; i < N; ++i)
			{
				const FVector P(Lod.VertexBuffers.PositionVertexBuffer.VertexPosition(i));
				const FVector2D UV(Lod.VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(i, 0));
				const int32 K = (P.Z > B.GetCenter().Z ? 0 : 2) + (P.Y > B.GetCenter().Y ? 0 : 1);
				if (!UV.Equals(C.UV[K], 0.01)) ++Bad;
			}
			UE_LOG(LogTemp, Log, TEXT("OLSWING SM_SpriteQuad: %d vertices, bounds %s, %d UVs off the corners assumed%s"), N, *B.ToString(), Bad,
				Bad ? TEXT(" (CHECK: the swing quad's picture would be mirrored)") : TEXT(""));
		}
#endif
		return C;
	}
}

void AOutlawsSpriteActor::SetSwingDriven(bool bOn)
{
	bOn = bOn && CVarHeldSwayEnable.GetValueOnGameThread() != 0;
	if (bSwingDrive == bOn) return;
	if (!bOn)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT heldsway %s: let go, straight again (the legs trailed %.1f cm at most)"), *GetName(), SwingPeak);
		EndSwing();                                // (let go, thrown or dropped: straight at once, no stretch)
	}
	SwingPeak = 0.f;
	bSwingDrive = bOn;
}

bool AOutlawsSpriteActor::SwingBottom(FVector& OutL, FVector& OutR, FVector& OutRestL, FVector& OutRestR) const
{
	OutL = SwingDrawn[0];
	OutR = SwingDrawn[1];
	OutRestL = SwingDrawn[2];
	OutRestR = SwingDrawn[3];
	return bSwingShown;
}

void AOutlawsSpriteActor::ShowSwingMesh(bool bOn)
{
	if (bOn == bSwingShown) return;
	bSwingShown = bOn;
	if (SwingMesh) SwingMesh->SetVisibility(bOn);
	// (hidden in game, not the visibility the object LOD and the 3D stand-in switch: those stay theirs)
	if (Quad) Quad->SetHiddenInGame(bOn);
}

void AOutlawsSpriteActor::EndSwing()
{
	SwingOff = SwingVel = FVector::ZeroVector;
	SwingYawDeg = 0.f;
	bSwingHavePrev = false;
	ShowSwingMesh(false);
}

void AOutlawsSpriteActor::UpdateSwing()
{
	UWorld* W = GetWorld();
	if (!bSwingDrive || !Quad || !W)
	{
		EndSwing();
		return;
	}
	// (the world's own step: a held man's sprite clock is stopped, its time dilation 0)
	const float Dt = FMath::Clamp(W->GetDeltaSeconds(), 0.f, 0.1f);
	const FTransform Qt = Quad->GetComponentTransform();
	const FQuadCorners& C = CornersOf(Quad->GetStaticMesh());
	const FVector Grip = Qt.TransformPosition((C.Pos[0] + C.Pos[1]) * 0.5);
	const FVector Foot = Qt.TransformPosition((C.Pos[2] + C.Pos[3]) * 0.5);
	const float H = FMath::Max(1.f, float(FVector::Dist(Grip, Foot)));
	const float MaxOff = FMath::Max(0.f, FMath::Min(CVarHeldSwayMaxLag.GetValueOnGameThread(), H / 3.f));
	const float K = FMath::Max(1.f, CVarHeldSwayStiffness.GetValueOnGameThread());
	const float Cd = 2.f * FMath::Max(0.f, CVarHeldSwayDamping.GetValueOnGameThread()) * FMath::Sqrt(K);
	FVector D = FVector::ZeroVector;
	if (bSwingHavePrev)
	{
		D = Grip - SwingPrevGrip;
		D.Z = 0.0;
		// a jump no hand makes in a frame (a teleport, a co-op copy put back): no swing from it
		if (D.SizeSquared() > FMath::Square(150.0))
		{
			SwingOff = SwingVel = FVector::ZeroVector;
			D = FVector::ZeroVector;
		}
	}
	SwingPrevGrip = Grip;
	bSwingHavePrev = true;
	// the bottom keeps its own speed (SwingVel, world) as the grip moves: the lag grows by the grip's move, shrinks by the
	// bottom's; the spring pulls the bottom back under the grip, the damping slows it (semi-implicit Euler, short steps)
	const FVector GripVel = Dt > 0.f ? D / Dt : FVector::ZeroVector;
	const int32 Steps = FMath::Clamp(FMath::CeilToInt(Dt * 120.f), 1, 12);
	const float Hs = Dt / Steps;
	for (int32 i = 0; i < Steps; ++i)
	{
		const FVector Acc = -K * SwingOff - Cd * SwingVel;
		SwingVel += Acc * Hs;
		SwingVel.Z = 0.0;
		SwingOff += (SwingVel - GripVel) * Hs;
		SwingOff.Z = 0.0;
	}
	const float Len = float(SwingOff.Size());
	if (Len > MaxOff && Len > 0.f)
	{
		// (held at the farthest lag: the bottom no faster outward than the grip, so it does not ring at the stop)
		const FVector N = SwingOff / Len;
		SwingOff = N * MaxOff;
		const float Out = float(FVector::DotProduct(SwingVel - GripVel, N));
		if (Out > 0.f) SwingVel -= N * Out;
	}
	SwingPeak = FMath::Max(SwingPeak, FMath::Min(Len, MaxOff));
	// still under the grip: the plain quad again (no jitter, nothing sent to the renderer)
	if (SwingOff.SizeSquared() < FMath::Square(0.05) && (SwingVel - GripVel).SizeSquared() < FMath::Square(0.5) && D.SizeSquared() < FMath::Square(0.01))
	{
		SwingOff = SwingVel = FVector::ZeroVector;
		SwingYawDeg = 0.f;
		ShowSwingMesh(false);
		return;
	}
	// a 3D stand-in drawn in the quad's place, or the sprite hidden: nothing to bend
	if (!Quad->GetVisibleFlag() || IsHidden())
	{
		ShowSwingMesh(false);
		return;
	}
	// the lag toward or away from the viewer (along the quad's facing) turns the bottom edge about its middle
	const FVector Toward = FVector(Qt.GetUnitAxis(EAxis::X).X, Qt.GetUnitAxis(EAxis::X).Y, 0.0).GetSafeNormal();
	const float Along = float(FVector::DotProduct(SwingOff, Toward));
	const float MaxYaw = FMath::Clamp(CVarHeldSwayMaxYaw.GetValueOnGameThread(), 0.f, 89.f);
	SwingYawDeg = MaxOff > 0.f ? FMath::Clamp(Along / MaxOff, -1.f, 1.f) * MaxYaw : 0.f;
	const FVector L = Qt.TransformPosition(C.Pos[2]), R = Qt.TransformPosition(C.Pos[3]);
	const FVector Mid = (L + R) * 0.5 + SwingOff;
	FVector Half = (R - L) * 0.5;
	Half.Z = 0.0;
	Half = FRotator(0.f, SwingYawDeg, 0.f).RotateVector(Half);
	const FVector BL(Mid.X - Half.X, Mid.Y - Half.Y, L.Z), BR(Mid.X + Half.X, Mid.Y + Half.Y, R.Z);
	SwingDrawn[0] = BL;
	SwingDrawn[1] = BR;
	SwingDrawn[2] = L;
	SwingDrawn[3] = R;
	if (!SwingMesh)
	{
		SwingMesh = NewObject<UProceduralMeshComponent>(this, TEXT("SwingQuad"), RF_Transient);
		SwingMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		SwingMesh->bUseComplexAsSimpleCollision = false;
		SwingMesh->SetupAttachment(Quad);
		SwingMesh->SetVisibility(false);
		SwingMesh->RegisterComponent();
	}
	// corners in the quad's own frame (the mesh's parent): the top as the static quad has it, the bottom moved
	TArray<FVector> Pos = { C.Pos[0], C.Pos[1], Qt.InverseTransformPosition(BL), Qt.InverseTransformPosition(BR) };
	TArray<FVector2D> UV = { C.UV[0], C.UV[1], C.UV[2], C.UV[3] };
	TArray<FVector> Nrm;
	Nrm.Init(FVector(1.0, 0.0, 0.0), 4);
	TArray<FProcMeshTangent> Tan;
	Tan.Init(FProcMeshTangent(0.f, -1.f, 0.f), 4);
	if (SwingMesh->GetNumSections() == 0)
	{
		// clockwise as the viewer on +X sees it: top left, top right, bottom right; top left, bottom right, bottom left
		const TArray<int32> Tri = { 0, 1, 3, 0, 3, 2 };
		SwingMesh->CreateMeshSection(0, Pos, Tri, Nrm, UV, TArray<FColor>(), Tan, false);
	}
	else
	{
		SwingMesh->UpdateMeshSection(0, Pos, Nrm, UV, TArray<FColor>(), Tan);
	}
	// the static quad's look: its material (a fade or on-top swap included), shadow, bounds and custom depth
	if (SwingMesh->GetMaterial(0) != Quad->GetMaterial(0)) SwingMesh->SetMaterial(0, Quad->GetMaterial(0));
	if (SwingMesh->CastShadow != Quad->CastShadow) SwingMesh->SetCastShadow(Quad->CastShadow);
	if (SwingMesh->BoundsScale != Quad->BoundsScale) SwingMesh->SetBoundsScale(Quad->BoundsScale);
	if (SwingMesh->bRenderCustomDepth != Quad->bRenderCustomDepth) SwingMesh->SetRenderCustomDepth(Quad->bRenderCustomDepth);
	if (SwingMesh->CustomDepthStencilValue != Quad->CustomDepthStencilValue) SwingMesh->SetCustomDepthStencilValue(Quad->CustomDepthStencilValue);
	if (SwingMesh->TranslucencySortPriority != Quad->TranslucencySortPriority) SwingMesh->SetTranslucentSortPriority(Quad->TranslucencySortPriority);
	ShowSwingMesh(true);
}
