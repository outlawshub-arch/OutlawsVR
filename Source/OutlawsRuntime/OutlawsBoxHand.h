#pragma once

#include "CoreMinimal.h"
#include "Components/SceneComponent.h"
#include "OutlawsBoxHand.generated.h"

class UProceduralMeshComponent;
class UMaterialInterface;

/**
 * The player's VR hand, drawn after the intro cutscene's hands (user 2026-09-28): long thin tapering fingers, a
 * relaxed cascade of odd angles at rest, flat cel shading (a lit and a shade tan), lighter nails and a black ink outline
 * (an inside-out shell a little larger); bare wrists, no sleeve.  Lives in motion-controller grip space (X forward,
 * Y right, Z up); the palm faces the other hand.  SetCurl bends the index finger with the trigger and the middle, ring
 * and little fingers with the grip (the thumb stays put).
 *
 * One welded skin (user 2026-10-01: it was a block a segment, blocky): the palm one rounded body with the thumb's
 * mound and the heel's, each finger and the thumb one tube from inside the palm to a domed tip, rings shared where the
 * segments meet and weighted half to each joint there, so a bend stays whole and smooth.  One mesh of four sections
 * (lit, shade, nail, ink), its vertices skinned on the CPU when the curl changes (one draw a section: 31 components a
 * hand was about 200 draws for the pair, Quest capture 2026-10-01).
 */
UCLASS(ClassGroup = (Outlaws), meta = (BlueprintSpawnableComponent))
class OUTLAWSRUNTIME_API UOutlawsBoxHand : public USceneComponent
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hand")
	bool bLeft = false;

	/** Degrees the knuckle / middle / end joints bend at full curl (added to the rest pose). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hand")
	float KnuckleCurl = 72.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hand")
	float JointCurl = 92.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Hand")
	float TipCurl = 55.f;

	/** Creates the mesh (call once the owner is spawned); MakeMaterial gives a flat-colour held material. */
	void Build(TFunctionRef<UMaterialInterface*(const FColor&)> MakeMaterial);
	/** REQ-MARYNAILS: the nails' section in another flat-colour material (a character's polish). */
	void SetNailMaterial(UMaterialInterface* M);
	bool HasMesh() const { return Mesh != nullptr; }
	static FColor NaturalNail();

	/** 0 = open, 1 = closed. */
	UFUNCTION(BlueprintCallable, Category = "Hand")
	void SetCurl(float Trigger, float Grip);

	/** The most the middle, ring and little fingers close (1 = a full fist): round a gun's grip they stop at its
	 *  surface, the lower ones sooner (the grip slants back toward the palm). */
	void SetGripCaps(float Middle, float Ring, float Little) { GripCap[0] = Middle; GripCap[1] = Ring; GripCap[2] = Little; }

	/** The index finger as last posed, in this component's space: its tip, and the way it points (knuckle to tip). */
	FVector IndexTip() const { return IndexTipPosed; }
	FVector IndexWay() const { return IndexWayPosed; }

private:
	/** A finger or thumb joint: its parent joint (-1 = the hand), place and rest turn in the parent's space. */
	struct FJoint
	{
		int32 Parent = -1;
		FVector At = FVector::ZeroVector;
		FRotator Rest = FRotator::ZeroRotator;
		int32 Digit = 0;
		int32 Seg = 0;
		bool bIndex = false;
		bool bThumb = false;
		double Len = 0.0;                   // the segment's length along its +X
	};
	TArray<FJoint> Joints;
	TArray<FTransform> Bind;                // each joint in hand space at rest (no curl)

	/** A skin vertex: blended between two joints (-1 = the hand), its place in each one's rest frame. */
	struct FSkinVert
	{
		int32 A = -1, B = -1;
		float W = 1.f;                      // A's share
		FVector LA = FVector::ZeroVector, LB = FVector::ZeroVector;
		FVector Rest = FVector::ZeroVector;
	};
	TArray<FSkinVert> Skin;
	TArray<FVector> SkinPosed;

	/** One material's triangles: the skin vertex of each corner, the triangles, the posed corners. */
	struct FSection
	{
		TArray<int32> Pool;
		TArray<int32> Tris;
		TArray<FVector> Posed;
		UMaterialInterface* Material = nullptr;
	};
	FSection Sections[4];                   // lit, shade, nail, ink
	float GripCap[3] = { 1.f, 1.f, 1.f };
	int64 PoseKey = -1;                     // the quantized curl last posed (analog jitter does not re-pose)
	FVector IndexTipPosed = FVector(12.0, 0.0, 2.5);
	FVector IndexWayPosed = FVector::ForwardVector;

	UPROPERTY(Transient)
	TObjectPtr<UProceduralMeshComponent> Mesh;

	struct FLook
	{
		UMaterialInterface* Lit = nullptr;
		UMaterialInterface* Shade = nullptr;
		UMaterialInterface* Nail = nullptr;
		UMaterialInterface* Ink = nullptr;
	};

	int32 AddJoint(int32 Parent, const FVector& At, const FRotator& Rest, int32 Digit, int32 Seg, bool bIndex, bool bThumb);
	/** A skin vertex at RestPos (hand space), A's share W, the rest B's. */
	int32 AddVert(const FVector& RestPos, int32 A, int32 B, float W);
	/** The joints turned by the curl, every corner placed; Upload sends them to the mesh. */
	void Pose(float Trigger, float Grip);
	void Upload(bool bCreate);
};
