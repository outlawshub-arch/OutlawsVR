// The ink line on the headset: see OutlawsInk.h.
#include "OutlawsInk.h"
#include "OutlawsObjMesh.h"
#include "Materials/MaterialInterface.h"
#include "ProceduralMeshComponent.h"
#include "RHIDefinitions.h"
#include "RHI.h"

namespace
{
	// how many of a component's sections are its own: the rest are the shell (rebuilt when shelled again)
	// a mesh's own sections (before its shells) and how many it had with them: a count that no longer matches means the
	// mesh was rebuilt since (a held gun's mesh is one per hand, cleared for each weapon), and its own count is read again
	TMap<TWeakObjectPtr<UProceduralMeshComponent>, int32> GOwnSections;
	TMap<TWeakObjectPtr<UProceduralMeshComponent>, int32> GShelledSections;

	/** A place on the mesh: vertices a millimetre apart are one. */
	FIntVector PlaceOf(const FVector& P)
	{
		return FIntVector(FMath::RoundToInt(P.X * 10.0), FMath::RoundToInt(P.Y * 10.0), FMath::RoundToInt(P.Z * 10.0));
	}

	/** A triangle by its three places, in order: its twin (the same corners wound the other way) has the same key. */
	struct FTriKey
	{
		FIntVector K[3];
		explicit FTriKey(FIntVector A, FIntVector B, FIntVector C)
		{
			auto Less = [](const FIntVector& X, const FIntVector& Y) { return X.X != Y.X ? X.X < Y.X : X.Y != Y.Y ? X.Y < Y.Y : X.Z < Y.Z; };
			if (Less(B, A)) Swap(A, B);
			if (Less(C, B)) Swap(B, C);
			if (Less(B, A)) Swap(A, B);
			K[0] = A; K[1] = B; K[2] = C;
		}
		bool operator==(const FTriKey& O) const { return K[0] == O.K[0] && K[1] == O.K[1] && K[2] == O.K[2]; }
		friend uint32 GetTypeHash(const FTriKey& T) { return HashCombine(HashCombine(GetTypeHash(T.K[0]), GetTypeHash(T.K[1])), GetTypeHash(T.K[2])); }
	};
}

namespace
{
	/** The section's texture's solid texels, when it is one read from a file (the item models, the held gear). */
	const OutlawsObj::FSolidMask* SolidOf(UMaterialInterface* Mat)
	{
		UTexture* T = nullptr;
		return Mat && Mat->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Tex")), T) && T ? OutlawsObj::SolidMaskOf(T) : nullptr;
	}

	/** A face that shows its texture's see-through texels (more than a few of ~45 points over it, kept off its edges):
	 *  a leaf card's cut-out.  Its shell would be seen through them as a black card (BUG-LEAFINK, the Quest: SHRUB01);
	 *  a solid thing's own far side hides its shell, but a card has none. */
	bool SeeThrough(const FProcMeshSection& Sec, int32 t, const OutlawsObj::FSolidMask& Mask)
	{
		const FVector2D A = Sec.ProcVertexBuffer[Sec.ProcIndexBuffer[t]].UV0;
		const FVector2D B = Sec.ProcVertexBuffer[Sec.ProcIndexBuffer[t + 1]].UV0;
		const FVector2D C = Sec.ProcVertexBuffer[Sec.ProcIndexBuffer[t + 2]].UV0;
		const FVector2D Mid = (A + B + C) / 3.0;
		constexpr int32 N = 8;
		int32 Clear = 0;
		for (int32 i = 0; i <= N; ++i)
		{
			for (int32 j = 0; i + j <= N; ++j)
			{
				const FVector2D P = Mid + ((A * i + B * j + C * (N - i - j)) / double(N) - Mid) * 0.9;
				if (!Mask.At(P.X, P.Y)) ++Clear;
			}
		}
		return Clear > 2;
	}

	/** One section's shell: its corners pushed out Local along their faces' normals, wound the other way; faces that
	 *  show see-through texels of Mask (when given) have none. */
	bool HullOf(const FProcMeshSection& Sec, double Local, TArray<FVector>& Pos, TArray<FVector>& Nrm, TArray<FVector2D>& UV, TArray<int32>& Tris, const OutlawsObj::FSolidMask* Mask)
	{
		const TArray<FProcMeshVertex>& Vb = Sec.ProcVertexBuffer;
		const TArray<uint32>& Ib = Sec.ProcIndexBuffer;
		// one of each twin: two-sided things are drawn as each face and its reverse (the item models); a shell of the
		// inner copy would show black through their see-through texels
		TSet<FTriKey> Seen;
		TArray<int32> Kept;
		TMap<FIntVector, TArray<FVector, TInlineAllocator<8>>> FacesAt;
		for (int32 t = 0; t + 2 < Ib.Num(); t += 3)
		{
			const FVector& A = Vb[Ib[t]].Position;
			const FVector& B = Vb[Ib[t + 1]].Position;
			const FVector& C = Vb[Ib[t + 2]].Position;
			bool bTwin = false;
			Seen.Add(FTriKey(PlaceOf(A), PlaceOf(B), PlaceOf(C)), &bTwin);
			if (bTwin || (Mask && SeeThrough(Sec, t, *Mask))) continue;
			Kept.Add(t);
			const FVector N = FVector::CrossProduct(C - A, B - A);   // the front face (FOutlawsMeshSection::AddFace)
			for (int32 k = 0; k < 3; ++k) FacesAt.FindOrAdd(PlaceOf(Vb[Ib[t + k]].Position)).Add(N);
		}
		// each corner pushed out along its faces' normals at that place, those within ~110 degrees of its own
		// (a box's corner closes; a face's reverse stays out of it)
		TMap<uint32, int32> Remap;
		for (int32 t : Kept)
		{
			int32 Out[3];
			for (int32 k = 0; k < 3; ++k)
			{
				const uint32 V = Ib[t + k];
				if (const int32* Done = Remap.Find(V))
				{
					Out[k] = *Done;
					continue;
				}
				const FVector Own3 = Vb[V].Normal;
				FVector Sum = FVector::ZeroVector;
				if (const auto* Faces = FacesAt.Find(PlaceOf(Vb[V].Position)))
				{
					for (const FVector& F : *Faces)
					{
						if (Own3.IsNearlyZero() || FVector::DotProduct(F.GetSafeNormal(), Own3) > -0.3) Sum += F;
					}
				}
				FVector N = Sum.GetSafeNormal();
				if (N.IsNearlyZero()) N = Own3.GetSafeNormal();
				Out[k] = Pos.Add(Vb[V].Position + N * Local);
				Nrm.Add(-N);
				UV.Add(FVector2D(0.5, 0.5));
				Remap.Add(V, Out[k]);
			}
			// wound the other way: its front faces inward, so only the far side of the shell draws
			Tris.Add(Out[0]);
			Tris.Add(Out[2]);
			Tris.Add(Out[1]);
		}
		return Tris.Num() > 0;
	}
}

bool OutlawsInk::Wanted()
{
	return GMaxRHIFeatureLevel < ERHIFeatureLevel::SM5;
}

void OutlawsInk::Forget(UProceduralMeshComponent* M)
{
	// (BUG-TNT, the Quest: a hand's gun mesh is cleared for each weapon; the revolver's remembered count of its own
	// sections then cut the dynamite's second, its wick and cord, off as an old shell)
	GOwnSections.Remove(M);
	GShelledSections.Remove(M);
}

void OutlawsInk::NoteShelled(UProceduralMeshComponent* M, int32 Own)
{
	if (!M) return;
	GOwnSections.Add(M, Own);
	GShelledSections.Add(M, M->GetNumSections());
}

void OutlawsInk::AddShell(UProceduralMeshComponent* M, float Thickness, UMaterialInterface* InkMaterial)
{
	if (!M || !InkMaterial)
	{
		return;
	}
	for (auto It = GOwnSections.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid()) It.RemoveCurrent();
	}
	for (auto It = GShelledSections.CreateIterator(); It; ++It)
	{
		if (!It.Key().IsValid()) It.RemoveCurrent();
	}
	int32 Own = M->GetNumSections();
	// (BUG-TNT, the Quest: the revolver's 1 own section was remembered for the hand's mesh, the dynamite rebuilt it with 2,
	// and its second, the wick and cord, was cleared here as an old shell)
	const int32* Shelled = GShelledSections.Find(M);
	if (const int32* Known = Shelled && *Shelled == M->GetNumSections() ? GOwnSections.Find(M) : nullptr)
	{
		Own = FMath::Min(*Known, Own);
		for (int32 s = M->GetNumSections() - 1; s >= Own; --s)
		{
			M->ClearMeshSection(s);
		}
	}
	GOwnSections.Add(M, Own);
	// the thickness in the component's own units
	const double Local = Thickness / FMath::Max(1e-3, M->GetComponentScale().GetAbs().GetMax());
	for (int32 s = 0; s < Own; ++s)
	{
		const FProcMeshSection* Sec = M->GetProcMeshSection(s);
		TArray<FVector> Pos, Nrm;
		TArray<FVector2D> UV;
		TArray<int32> Tris;
		if (!Sec || !Sec->bSectionVisible || Sec->ProcIndexBuffer.Num() < 3 || !HullOf(*Sec, Local, Pos, Nrm, UV, Tris, SolidOf(M->GetMaterial(s))))
		{
			continue;
		}
		const int32 Index = M->GetNumSections();
		M->CreateMeshSection_LinearColor(Index, Pos, Tris, Nrm, UV, TArray<FLinearColor>(), TArray<FProcMeshTangent>(), false);
		M->SetMaterial(Index, InkMaterial);
	}
	GShelledSections.Add(M, M->GetNumSections());
}

UProceduralMeshComponent* OutlawsInk::MakeHalo(UProceduralMeshComponent* Src, float Thickness, UMaterialInterface* Mat)
{
	if (!Src || !Mat)
	{
		return nullptr;
	}
	const int32* Shelled = GShelledSections.Find(Src);
	const int32* Known = Shelled && *Shelled == Src->GetNumSections() ? GOwnSections.Find(Src) : nullptr;
	const int32 Own = Known ? FMath::Min(*Known, Src->GetNumSections()) : Src->GetNumSections();
	UObject* Outer = Src->GetOwner() ? static_cast<UObject*>(Src->GetOwner()) : static_cast<UObject*>(Src);
	UProceduralMeshComponent* H = NewObject<UProceduralMeshComponent>(Outer, NAME_None, RF_Transient);
	H->bUseAsyncCooking = false;
	H->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	H->SetCastShadow(false);
	H->SetupAttachment(Src);
	H->RegisterComponent();
	const double Local = Thickness / FMath::Max(1e-3, Src->GetComponentScale().GetAbs().GetMax());
	int32 Index = 0;
	for (int32 s = 0; s < Own; ++s)
	{
		const FProcMeshSection* Sec = Src->GetProcMeshSection(s);
		TArray<FVector> Pos, Nrm;
		TArray<FVector2D> UV;
		TArray<int32> Tris;
		if (!Sec || !Sec->bSectionVisible || Sec->ProcIndexBuffer.Num() < 3 || !HullOf(*Sec, Local, Pos, Nrm, UV, Tris, SolidOf(Src->GetMaterial(s))))
		{
			continue;
		}
		H->CreateMeshSection_LinearColor(Index, Pos, Tris, Nrm, UV, TArray<FLinearColor>(), TArray<FProcMeshTangent>(), false);
		H->SetMaterial(Index, Mat);
		++Index;
	}
	return H;
}
