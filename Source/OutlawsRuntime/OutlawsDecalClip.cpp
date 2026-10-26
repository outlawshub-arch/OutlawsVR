// BUG-DECALEDGE: see OutlawsDecalClip.h.

#include "OutlawsDecalClip.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInterface.h"
#include "OutlawsRuntime.h"
#include "HAL/PlatformTime.h"

namespace OutlawsDecalClip
{
	namespace
	{
		constexpr double CellCm = 500.0;           // the index's cells across (a decal is 0.2 to 5.6 m)
		constexpr int32 MaxCells = 49;             // a triangle over more cells than this is kept in a list looked at every time
		constexpr int32 IndexAbove = 3000;         // a component with fewer triangles is scanned whole
		constexpr double PlaneTol = 1.0;           // cm: a triangle this close to the decal's plane lies in it

		struct FGrid
		{
			int32 Sig = -1;
			TMap<uint64, TArray<uint64>> Cells;    // (cell x, cell y) -> triangle ids (section << 32 | first index of the triangle)
			TArray<uint64> Large;
			TArray<uint8> SectionOk;               // 1: a solid section (opaque or masked), where marks go
		};
		TMap<TWeakObjectPtr<UProceduralMeshComponent>, FGrid> Grids;
		int32 NClipped = 0, NPieces = 0, NDropped = 0, NWhole = 0;
		double SecTotal = 0.0, SecWorst = 0.0;

		int32 SignatureOf(const UProceduralMeshComponent* C)
		{
			int32 Sig = C->GetNumSections();
			for (int32 s = 0; s < C->GetNumSections(); ++s)
			{
				if (const FProcMeshSection* Sec = const_cast<UProceduralMeshComponent*>(C)->GetProcMeshSection(s)) Sig = Sig * 31 + Sec->ProcIndexBuffer.Num() * 7 + Sec->ProcVertexBuffer.Num();
			}
			return Sig;
		}

		int32 TriCount(const UProceduralMeshComponent* C)
		{
			int32 N = 0;
			for (int32 s = 0; s < C->GetNumSections(); ++s)
			{
				if (const FProcMeshSection* Sec = const_cast<UProceduralMeshComponent*>(C)->GetProcMeshSection(s)) N += Sec->ProcIndexBuffer.Num() / 3;
			}
			return N;
		}

		bool SectionIsSolid(UProceduralMeshComponent* C, int32 s)
		{
			const UMaterialInterface* M = C->GetMaterial(s);
			if (!M) return true;
			const EBlendMode B = M->GetBlendMode();
			return B == BLEND_Opaque || B == BLEND_Masked;
		}

		uint64 CellKey(int32 X, int32 Y) { return (uint64(uint32(X)) << 32) | uint64(uint32(Y)); }

		FGrid& GridOf(UProceduralMeshComponent* C)
		{
			FGrid& G = Grids.FindOrAdd(C);
			const int32 Sig = SignatureOf(C);
			if (G.Sig == Sig) return G;
			if (Grids.Num() > 64)
			{
				for (auto It = Grids.CreateIterator(); It; ++It) if (!It.Key().IsValid()) It.RemoveCurrent();
			}
			FGrid& Fresh = Grids.FindOrAdd(C);
			const double T0 = FPlatformTime::Seconds();
			Fresh.Sig = Sig;
			Fresh.Cells.Reset();
			Fresh.Large.Reset();
			Fresh.SectionOk.Reset();
			const FTransform Tr = C->GetComponentTransform();
			for (int32 s = 0; s < C->GetNumSections(); ++s)
			{
				const bool bOk = SectionIsSolid(C, s);
				Fresh.SectionOk.Add(bOk ? 1 : 0);
				const FProcMeshSection* Sec = C->GetProcMeshSection(s);
				if (!bOk || !Sec) continue;
				for (int32 i = 0; i + 2 < Sec->ProcIndexBuffer.Num(); i += 3)
				{
					const FVector A = Tr.TransformPosition(FVector(Sec->ProcVertexBuffer[Sec->ProcIndexBuffer[i]].Position));
					const FVector B = Tr.TransformPosition(FVector(Sec->ProcVertexBuffer[Sec->ProcIndexBuffer[i + 1]].Position));
					const FVector D = Tr.TransformPosition(FVector(Sec->ProcVertexBuffer[Sec->ProcIndexBuffer[i + 2]].Position));
					const int32 X0 = FMath::FloorToInt(FMath::Min3(A.X, B.X, D.X) / CellCm), X1 = FMath::FloorToInt(FMath::Max3(A.X, B.X, D.X) / CellCm);
					const int32 Y0 = FMath::FloorToInt(FMath::Min3(A.Y, B.Y, D.Y) / CellCm), Y1 = FMath::FloorToInt(FMath::Max3(A.Y, B.Y, D.Y) / CellCm);
					const uint64 Id = (uint64(s) << 32) | uint64(i);
					if (int64(X1 - X0 + 1) * int64(Y1 - Y0 + 1) > MaxCells) { Fresh.Large.Add(Id); continue; }
					for (int32 X = X0; X <= X1; ++X) for (int32 Y = Y0; Y <= Y1; ++Y) Fresh.Cells.FindOrAdd(CellKey(X, Y)).Add(Id);
				}
			}
			UE_LOG(LogOutlaws, Log, TEXT("OLRT decal surfaces indexed: %d triangles in %d cells (+%d large) in %.1f ms"), TriCount(C), Fresh.Cells.Num(), Fresh.Large.Num(), (FPlatformTime::Seconds() - T0) * 1000.0);
			return Fresh;
		}

		/** Keep the part of the polygon where A u + B v + C >= 0. */
		void ClipHalf(TArray<FVector2D>& Poly, double A, double B, double C)
		{
			TArray<FVector2D> Out;
			const int32 N = Poly.Num();
			for (int32 i = 0; i < N; ++i)
			{
				const FVector2D& P = Poly[i];
				const FVector2D& Q = Poly[(i + 1) % N];
				const double Dp = A * P.X + B * P.Y + C, Dq = A * Q.X + B * Q.Y + C;
				if (Dp >= 0.0) Out.Add(P);
				if ((Dp >= 0.0) != (Dq >= 0.0))
				{
					const double T = Dp / (Dp - Dq);
					Out.Add(P + (Q - P) * T);
				}
			}
			Poly = MoveTemp(Out);
		}
	}

	void Prepare(UProceduralMeshComponent* Comp)
	{
		if (Comp && TriCount(Comp) >= IndexAbove) GridOf(Comp);
	}

	FString Describe()
	{
		const int32 N = NClipped + NDropped + NWhole;
		return FString::Printf(TEXT("%d decals clipped to their surface into %d pieces, %d dropped (off it), %d left whole; %.3f ms each on average, %.2f ms the worst"), NClipped, NPieces, NDropped, NWhole, N ? SecTotal * 1000.0 / N : 0.0, SecWorst * 1000.0);
	}

	bool Clip(UProceduralMeshComponent* Comp, const FVector& Centre, const FVector& N, const FVector& Ax, const FVector& Ay, double Width, double Height, TArray<FPiece>& Out)
	{
		Out.Reset();
		if (!Comp) { ++NWhole; return false; }
		const double T0 = FPlatformTime::Seconds();
		struct FTimer { double T0; ~FTimer() { const double D = FPlatformTime::Seconds() - T0; SecTotal += D; SecWorst = FMath::Max(SecWorst, D); } } Timer{ T0 };
		const double Hw = Width * 0.5, Hh = Height * 0.5;
		const FTransform Tr = Comp->GetComponentTransform();
		const bool bIdentity = Tr.Equals(FTransform::Identity, 1e-4f);
		bool bFound = false;
		TSet<uint64> Seen;
		auto Test = [&](const FProcMeshSection* Sec, int32 i)
		{
			const FProcMeshVertex& Va = Sec->ProcVertexBuffer[Sec->ProcIndexBuffer[i]];
			const FProcMeshVertex& Vb = Sec->ProcVertexBuffer[Sec->ProcIndexBuffer[i + 1]];
			const FProcMeshVertex& Vc = Sec->ProcVertexBuffer[Sec->ProcIndexBuffer[i + 2]];
			FVector P[3] = { FVector(Va.Position), FVector(Vb.Position), FVector(Vc.Position) };
			if (!bIdentity) for (FVector& V : P) V = Tr.TransformPosition(V);
			for (const FVector& V : P) if (FMath::Abs(FVector::DotProduct(V - Centre, N)) > PlaneTol) return;
			// facing the way the decal does (a coplanar face the other way is another surface: a two-sided pane's back)
			const FVector Nv = bIdentity ? FVector(Va.Normal) : Tr.TransformVectorNoScale(FVector(Va.Normal));
			if (Nv.SizeSquared() > 0.25 && FVector::DotProduct(Nv, N) < 0.3) return;
			bFound = true;
			TArray<FVector2D> Poly;
			for (const FVector& V : P) Poly.Add(FVector2D(FVector::DotProduct(V - Centre, Ax), FVector::DotProduct(V - Centre, Ay)));
			ClipHalf(Poly, 1.0, 0.0, Hw);
			if (Poly.Num() >= 3) ClipHalf(Poly, -1.0, 0.0, Hw);
			if (Poly.Num() >= 3) ClipHalf(Poly, 0.0, 1.0, Hh);
			if (Poly.Num() >= 3) ClipHalf(Poly, 0.0, -1.0, Hh);
			for (int32 k = 1; k + 1 < Poly.Num(); ++k)
			{
				FPiece Pc;
				Pc.P[0] = Poly[0];
				Pc.P[1] = Poly[k];
				Pc.P[2] = Poly[k + 1];
				// (a sliver with no area adds nothing)
				if (FMath::Abs(FVector2D::CrossProduct(Pc.P[1] - Pc.P[0], Pc.P[2] - Pc.P[0])) > 1e-3) Out.Add(Pc);
			}
		};
		// the rectangle's reach on the ground plane
		const FVector Corners[4] = { Centre - Ax * Hw - Ay * Hh, Centre + Ax * Hw - Ay * Hh, Centre + Ax * Hw + Ay * Hh, Centre - Ax * Hw + Ay * Hh };
		if (TriCount(Comp) < IndexAbove)
		{
			for (int32 s = 0; s < Comp->GetNumSections(); ++s)
			{
				const FProcMeshSection* Sec = Comp->GetProcMeshSection(s);
				if (!Sec || !SectionIsSolid(Comp, s)) continue;
				for (int32 i = 0; i + 2 < Sec->ProcIndexBuffer.Num(); i += 3) Test(Sec, i);
			}
		}
		else
		{
			FGrid& G = GridOf(Comp);
			double MinX = 1e18, MaxX = -1e18, MinY = 1e18, MaxY = -1e18;
			for (const FVector& C : Corners) { MinX = FMath::Min(MinX, C.X); MaxX = FMath::Max(MaxX, C.X); MinY = FMath::Min(MinY, C.Y); MaxY = FMath::Max(MaxY, C.Y); }
			auto Visit = [&](uint64 Id)
			{
				bool bAlready = false;
				Seen.Add(Id, &bAlready);
				if (bAlready) return;
				const int32 s = int32(Id >> 32), i = int32(Id & 0xffffffffu);
				const FProcMeshSection* Sec = Comp->GetProcMeshSection(s);
				if (Sec && Sec->ProcIndexBuffer.IsValidIndex(i + 2)) Test(Sec, i);
			};
			for (int32 X = FMath::FloorToInt(MinX / CellCm); X <= FMath::FloorToInt(MaxX / CellCm); ++X)
			{
				for (int32 Y = FMath::FloorToInt(MinY / CellCm); Y <= FMath::FloorToInt(MaxY / CellCm); ++Y)
				{
					if (const TArray<uint64>* Ids = G.Cells.Find(CellKey(X, Y))) for (const uint64 Id : *Ids) Visit(Id);
				}
			}
			for (const uint64 Id : G.Large) Visit(Id);
		}
		if (!bFound) { ++NWhole; return false; }
		if (Out.Num() == 0) ++NDropped;
		else { ++NClipped; NPieces += Out.Num(); }
		return true;
	}
}
