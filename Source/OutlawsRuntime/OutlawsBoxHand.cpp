#include "OutlawsBoxHand.h"
#include "ProceduralMeshComponent.h"

namespace
{
	// the cutscene's hands: flat tan with a darker shade tan, lighter nails, black ink
	const FColor SkinLit(214, 152, 94), SkinShade(158, 102, 60), NailColour(236, 196, 160);
	const FColor InkColour(10, 8, 7);
	constexpr double Ink = 0.14;          // the outline shell's thickness, cm
	enum { SecLit, SecShade, SecNail, SecInk };

	/** Triangles being built for one section, in one joint's space. */
	struct FMeshOut
	{
		TArray<FVector> P;
		void Tri(const FVector& A, const FVector& B, const FVector& C, const FVector& Outward)
		{
			// outward = (C - A) x (B - A) is the front face (as FOutlawsMeshSection::AddFace)
			const bool bFlip = FVector::DotProduct(FVector::CrossProduct(C - A, B - A), Outward) < 0.0;
			P.Add(A); P.Add(bFlip ? C : B); P.Add(bFlip ? B : C);
		}
	};
}

int32 UOutlawsBoxHand::AddJoint(int32 Parent, const FVector& At, const FRotator& Rest, int32 Digit, int32 Seg, bool bIndex, bool bThumb)
{
	FJoint J;
	J.Parent = Parent;
	J.At = At;
	J.Rest = Rest;
	J.Digit = Digit;
	J.Seg = Seg;
	J.bIndex = bIndex;
	J.bThumb = bThumb;
	return Joints.Add(J);
}

int32 UOutlawsBoxHand::AddVert(const FVector& RestPos, int32 A, int32 B, float W)
{
	FSkinVert V;
	V.A = A;
	V.B = B;
	V.W = W;
	V.Rest = RestPos;
	V.LA = Bind.IsValidIndex(A) ? Bind[A].InverseTransformPosition(RestPos) : RestPos;
	V.LB = Bind.IsValidIndex(B) ? Bind[B].InverseTransformPosition(RestPos) : RestPos;
	return Skin.Add(V);
}

void UOutlawsBoxHand::Build(TFunctionRef<UMaterialInterface*(const FColor&)> MakeMaterial)
{
	if (Mesh)
	{
		Mesh->DestroyComponent();
		Mesh = nullptr;
	}
	Joints.Reset();
	Bind.Reset();
	Skin.Reset();
	SkinPosed.Reset();
	for (FSection& Sec : Sections) Sec = FSection();
	PoseKey = -1;
	if (!GetOwner())
	{
		return;
	}
	Sections[SecLit].Material = MakeMaterial(SkinLit);
	Sections[SecShade].Material = MakeMaterial(SkinShade);
	Sections[SecNail].Material = MakeMaterial(NailColour);
	Sections[SecInk].Material = MakeMaterial(InkColour);
	// right hand: the palm sits outboard (+Y) of the controller handle and faces it (-Y); the left hand is the mirror
	const double S = bLeft ? -1.0 : 1.0;
	// ---- the joints: long thin fingers of three segments; at rest a relaxed cascade (the little finger most curled and
	// splayed) and the index fingertip tipped back, the cutscene's odd angles
	struct FSpec { double Z, L[3], W, Splay, Rest[3], Roll; };
	const FSpec Fingers[4] = {
		{ 2.55, { 4.6, 3.1, 2.5 }, 1.38, 5.0, { 4.0, 6.0, -8.0 }, 0.0 },
		{ 0.85, { 5.1, 3.4, 2.7 }, 1.42, 1.0, { 9.0, 10.0, 6.0 }, 0.0 },
		{ -0.85, { 4.8, 3.2, 2.6 }, 1.36, -3.0, { 14.0, 14.0, 8.0 }, 3.0 },
		{ -2.45, { 3.9, 2.6, 2.2 }, 1.18, -9.0, { 20.0, 18.0, 10.0 }, 8.0 } };
	const double Taper[3] = { 0.88, 0.86, 0.8 };
	int32 Chain[5][3];
	double Lens[5][3], Wide[5][3];
	for (int32 i = 0; i < 4; ++i)
	{
		const FSpec& F = Fingers[i];
		int32 Parent = -1;
		FVector At(3.5, S * 3.2, F.Z);
		double W = F.W;
		for (int32 s = 0; s < 3; ++s)
		{
			const FRotator Rest(s == 0 ? F.Splay : 0.0, -S * F.Rest[s], s == 0 ? S * F.Roll : 0.0);
			const int32 J = AddJoint(Parent, At, Rest, i, s, i == 0, false);
			Joints[J].Len = F.L[s];
			Chain[i][s] = J;
			Lens[i][s] = F.L[s];
			Wide[i][s] = W;
			Parent = J;
			At = FVector(F.L[s], 0.0, 0.0);
			W *= Taper[s];
		}
	}
	// the thumb, as round a pistol grip (user 2026-09-28: it was tucked against the palm): from the palm's upper inner
	// edge up and across the top of the grip (the web of the hand), then forward along the grip's far side above the
	// fingers, the nail out and up. Each segment's way and nail side in hand space; the rests are the turns between them
	{
		const double L[3] = { 3.6, 2.9, 2.3 };
		int32 Parent = -1;
		FVector At(-1.0, S * 2.3, 2.2);
		double W = 1.5;
		const FVector Way[3] = { FVector(0.6, -S * 0.7, 0.35), FVector(0.85, -S * 0.45, 0.1), FVector(0.95, -S * 0.2, -0.1) };
		const FVector Nail(0.0, -S * 0.6, 0.8);
		FQuat Prev = FQuat::Identity;
		for (int32 s = 0; s < 3; ++s)
		{
			// the segment's nail side is its local S*Y
			const FQuat Q = FRotationMatrix::MakeFromXY(Way[s].GetSafeNormal(), Nail * S).ToQuat();
			const FRotator Rest = (Prev.Inverse() * Q).Rotator();
			Prev = Q;
			const int32 J = AddJoint(Parent, At, Rest, 4, s, false, true);
			Joints[J].Len = L[s];
			Chain[4][s] = J;
			Lens[4][s] = L[s];
			Wide[4][s] = W;
			Parent = J;
			At = FVector(L[s], 0.0, 0.0);
			W *= Taper[s];
		}
	}
	// the rest pose: each joint in hand space with no curl (the skin is bound to it)
	Bind.SetNum(Joints.Num());
	for (int32 j = 0; j < Joints.Num(); ++j)
	{
		const FTransform Local(Joints[j].Rest, Joints[j].At);
		Bind[j] = Joints[j].Parent >= 0 ? Local * Bind[Joints[j].Parent] : Local;
	}

	// ---- the skin (user 2026-10-01: welded, rounder, more of it): rings of skin vertices bridged into one surface
	struct FRing { TArray<int32> V; FVector Centre = FVector::ZeroVector; };
	TArray<FIntVector> Surf;
	auto Tri = [&](int32 A, int32 B, int32 C, const FVector& Out)
	{
		const FVector PA = Skin[A].Rest, PB = Skin[B].Rest, PC = Skin[C].Rest;
		// outward = (C - A) x (B - A) is the front face (as FOutlawsMeshSection::AddFace)
		const bool bFlip = FVector::DotProduct(FVector::CrossProduct(PC - PA, PB - PA), Out) < 0.0;
		Surf.Add(bFlip ? FIntVector(A, C, B) : FIntVector(A, B, C));
	};
	auto Bridge = [&](const FRing& R0, const FRing& R1)
	{
		const int32 N = R0.V.Num();
		const FVector Axis = (R0.Centre + R1.Centre) * 0.5;
		for (int32 k = 0; k < N; ++k)
		{
			const int32 j = (k + 1) % N;
			const FVector Mid = (Skin[R0.V[k]].Rest + Skin[R0.V[j]].Rest + Skin[R1.V[k]].Rest + Skin[R1.V[j]].Rest) * 0.25;
			Tri(R0.V[k], R0.V[j], R1.V[j], Mid - Axis);
			Tri(R0.V[k], R1.V[j], R1.V[k], Mid - Axis);
		}
	};
	auto Fan = [&](const FRing& R, int32 Tip)
	{
		const FVector Inner = R.Centre - (Skin[Tip].Rest - R.Centre);
		const int32 N = R.V.Num();
		for (int32 k = 0; k < N; ++k)
		{
			const int32 j = (k + 1) % N;
			Tri(R.V[k], R.V[j], Tip, (Skin[R.V[k]].Rest + Skin[R.V[j]].Rest + Skin[Tip].Rest) / 3.0 - Inner);
		}
	};
	// the palm: one rounded body from the wrist (x -4.2) to the knuckles (3.5) round Y = S x 3.2, Z = -0.2, a rounded
	// square in section (the back of the hand +S x Y), wider and a little deeper at the knuckles; on the palm side the
	// thumb's mound (thenar) where the thumb sinks in and the heel's (hypothenar), on the back a low ridge of knuckles
	{
		constexpr int32 NP = 20;
		const double X0 = -4.2, X1 = 3.5;
		auto Ring = [&](double X, double T, double Shrink) -> FRing
		{
			FRing R;
			R.Centre = FVector(X, S * 3.2, -0.2);
			const double Hy = (1.05 + 0.27 * T) * Shrink, Hz = 2.8 * (1.0 + 0.26 * T) * Shrink;
			for (int32 k = 0; k < NP; ++k)
			{
				const double A = 2.0 * PI * k / NP, C = FMath::Cos(A), Sn = FMath::Sin(A);
				double Y = Hy * FMath::Sign(C) * FMath::Pow(FMath::Abs(C), 2.0 / 3.0);
				const double Z = Hz * FMath::Sign(Sn) * FMath::Pow(FMath::Abs(Sn), 2.0 / 3.0);
				const double ZW = Z - 0.2;                                       // (hand-space Z)
				const double Palmward = FMath::Max(0.0, -C), Backward = FMath::Max(0.0, C);
				const double Thenar = 1.0 * FMath::Exp(-(FMath::Square(X + 1.5) / (2.0 * 1.7 * 1.7) + FMath::Square(ZW - 1.6) / (2.0 * 1.25 * 1.25)));
				const double Heel = 0.45 * FMath::Exp(-(FMath::Square(X + 2.3) / (2.0 * 1.6 * 1.6) + FMath::Square(ZW + 2.3) / (2.0 * 1.0 * 1.0)));
				const double Ridge = 0.22 * FMath::Exp(-FMath::Square(X - 3.0) / (2.0 * 0.6 * 0.6));
				Y += -(Thenar + Heel) * Palmward * Shrink + Ridge * Backward * Shrink;
				R.V.Add(AddVert(R.Centre + FVector(0.0, S * Y, Z), -1, -1, 1.f));
			}
			return R;
		};
		TArray<FRing> Rings;
		Rings.Add(Ring(X0 - 0.55, 0.0, 0.62));
		Rings.Add(Ring(X0 - 0.25, 0.0, 0.9));
		for (const double T : { 0.0, 0.13, 0.26, 0.39, 0.52, 0.64, 0.76, 0.87, 0.95, 1.0 }) Rings.Add(Ring(X0 + (X1 - X0) * T, T, 1.0));
		Rings.Add(Ring(X1 + 0.3, 1.0, 0.9));
		Rings.Add(Ring(X1 + 0.55, 1.0, 0.6));
		for (int32 r = 0; r + 1 < Rings.Num(); ++r) Bridge(Rings[r], Rings[r + 1]);
		Fan(Rings[0], AddVert(FVector(X0 - 0.7, S * 3.2, -0.2), -1, -1, 1.f));
		Fan(Rings.Last(), AddVert(FVector(X1 + 0.7, S * 3.2, -0.2), -1, -1, 1.f));
	}
	// each finger and the thumb: one tube from inside the palm (the hand's) through rings shared at the joints (half each
	// side's), knuckles a little proud on the back, the pads a little full, to a domed tip
	TArray<int32> TipJoint;
	TArray<double> TipLen, TipDepth;
	for (int32 d = 0; d < 5; ++d)
	{
		constexpr int32 N = 10;
		const bool bThumb = d == 4;
		struct FSt { int32 Seg; double X; int32 A, B; float W; double Wd; int32 Kind; };    // kind 0 plain, 1 knuckle, 2 pad
		auto WidthAt = [&](int32 s, double X) { return Wide[d][s] * FMath::Lerp(1.0, Taper[s], FMath::Clamp(X / Lens[d][s], 0.0, 1.0)); };
		TArray<FSt> St;
		St.Add({ 0, bThumb ? -1.3 : -0.9, -1, -1, 1.f, WidthAt(0, 0.0) * 1.02, 0 });
		St.Add({ 0, 0.0, Chain[d][0], -1, 0.5f, WidthAt(0, 0.0) * 1.03, 1 });
		for (int32 s = 0; s < 3; ++s)
		{
			if (s > 0) St.Add({ s, 0.0, Chain[d][s], Chain[d][s - 1], 0.5f, Wide[d][s] * 1.02, 1 });
			St.Add({ s, 0.35 * Lens[d][s], Chain[d][s], Chain[d][s], 1.f, WidthAt(s, 0.35 * Lens[d][s]), 2 });
			St.Add({ s, 0.7 * Lens[d][s], Chain[d][s], Chain[d][s], 1.f, WidthAt(s, 0.7 * Lens[d][s]), 0 });
		}
		St.Add({ 2, Lens[d][2], Chain[d][2], Chain[d][2], 1.f, WidthAt(2, Lens[d][2]), 0 });
		const double DepthK = bThumb ? 0.86 : 0.84;
		TArray<FRing> Rings;
		for (const FSt& T : St)
		{
			const FTransform& Fr = Bind[Chain[d][T.Seg]];
			FRing R;
			R.Centre = Fr.TransformPosition(FVector(T.X, 0.0, 0.0));
			for (int32 k = 0; k < N; ++k)
			{
				const double A = 2.0 * PI * k / N + PI / N, C = FMath::Cos(A), Sn = FMath::Sin(A);
				double Ry = 0.5 * T.Wd * DepthK;
				const double Rz = 0.5 * T.Wd;
				if (T.Kind == 1 && C > 0.0) Ry *= 1.0 + 0.1 * C;        // the knuckle, on the back (+S x Y)
				if (T.Kind == 2 && C < 0.0) Ry *= 1.0 - 0.07 * C;       // the pad, under it
				R.V.Add(AddVert(Fr.TransformPosition(FVector(T.X, S * Ry * C, Rz * Sn)), T.A, T.B, T.W));
			}
			Rings.Add(R);
		}
		// the domed tip, a little toward the pad
		const int32 JT = Chain[d][2];
		const double L2 = Lens[d][2], WT = WidthAt(2, L2);
		for (const FVector2D& Dome : { FVector2D(0.17, 0.82), FVector2D(0.31, 0.52) })
		{
			FRing R;
			R.Centre = Bind[JT].TransformPosition(FVector(L2 + Dome.X * WT, -S * 0.04 * WT, 0.0));
			for (int32 k = 0; k < N; ++k)
			{
				const double A = 2.0 * PI * k / N + PI / N;
				const FVector P(L2 + Dome.X * WT, S * 0.5 * WT * DepthK * Dome.Y * FMath::Cos(A) - S * 0.04 * WT, 0.5 * WT * Dome.Y * FMath::Sin(A));
				R.V.Add(AddVert(Bind[JT].TransformPosition(P), JT, JT, 1.f));
			}
			Rings.Add(R);
		}
		for (int32 r = 0; r + 1 < Rings.Num(); ++r) Bridge(Rings[r], Rings[r + 1]);
		Fan(Rings.Last(), AddVert(Bind[JT].TransformPosition(FVector(L2 + 0.42 * WT, -S * 0.07 * WT, 0.0)), JT, JT, 1.f));
		TipJoint.Add(JT);
		TipLen.Add(L2);
		TipDepth.Add(WT * DepthK);
	}
	// ---- smooth normals (each skin vertex's faces, by area) and the ink shell: every surface vertex pushed out along its
	// normal, the same weights, the faces turned inward
	const int32 NumSurf = Skin.Num();
	TArray<FVector> Nrm;
	Nrm.Init(FVector::ZeroVector, NumSurf);
	for (const FIntVector& F : Surf)
	{
		const FVector PA = Skin[F.X].Rest, PB = Skin[F.Y].Rest, PC = Skin[F.Z].Rest;
		const FVector Fn = FVector::CrossProduct(PC - PA, PB - PA);
		Nrm[F.X] += Fn;
		Nrm[F.Y] += Fn;
		Nrm[F.Z] += Fn;
	}
	TArray<int32> InkOf;
	InkOf.SetNum(NumSurf);
	for (int32 i = 0; i < NumSurf; ++i)
	{
		const FSkinVert V = Skin[i];
		InkOf[i] = AddVert(V.Rest + Nrm[i].GetSafeNormal() * Ink, V.A, V.B, V.W);
	}
	// ---- the sections: lit or shade by each face's turn to a light fixed to the hand (from above and the back), the ink,
	// and the nails (lighter plates on the backs of the tips, riding the last joint)
	const FVector Light = FVector(0.2, S * 0.55, 0.8).GetSafeNormal();
	TMap<int32, int32> Local[4];
	auto Put = [&](int32 Sec, int32 A, int32 B, int32 C)
	{
		for (const int32 V : { A, B, C })
		{
			int32* Have = Local[Sec].Find(V);
			if (!Have)
			{
				Have = &Local[Sec].Add(V, Sections[Sec].Pool.Num());
				Sections[Sec].Pool.Add(V);
			}
			Sections[Sec].Tris.Add(*Have);
		}
	};
	for (const FIntVector& F : Surf)
	{
		const FVector Fn = FVector::CrossProduct(Skin[F.Z].Rest - Skin[F.X].Rest, Skin[F.Y].Rest - Skin[F.X].Rest).GetSafeNormal();
		Put(FVector::DotProduct(Fn, Light) > 0.12 ? SecLit : SecShade, F.X, F.Y, F.Z);
		Put(SecInk, InkOf[F.X], InkOf[F.Z], InkOf[F.Y]);
	}
	for (int32 d = 0; d < TipJoint.Num(); ++d)
	{
		const int32 JT = TipJoint[d];
		const double L = TipLen[d], Y = S * (0.5 * TipDepth[d] + 0.04), Zh = 0.3 * TipDepth[d] / 0.84;
		const int32 A = AddVert(Bind[JT].TransformPosition(FVector(L * 0.42, Y, -Zh)), JT, JT, 1.f);
		const int32 B = AddVert(Bind[JT].TransformPosition(FVector(L * 0.42, Y, Zh)), JT, JT, 1.f);
		const int32 C = AddVert(Bind[JT].TransformPosition(FVector(L + 0.05, Y * 0.9, Zh * 0.8)), JT, JT, 1.f);
		const int32 D = AddVert(Bind[JT].TransformPosition(FVector(L + 0.05, Y * 0.9, -Zh * 0.8)), JT, JT, 1.f);
		const FVector Out = Bind[JT].TransformVectorNoScale(FVector(0.0, S, 0.0));
		auto NailTri = [&](int32 P0, int32 P1, int32 P2)
		{
			const bool bFlip = FVector::DotProduct(FVector::CrossProduct(Skin[P2].Rest - Skin[P0].Rest, Skin[P1].Rest - Skin[P0].Rest), Out) < 0.0;
			if (bFlip) Put(SecNail, P0, P2, P1);
			else Put(SecNail, P0, P1, P2);
		};
		NailTri(A, B, C);
		NailTri(A, C, D);
	}
	Mesh = NewObject<UProceduralMeshComponent>(GetOwner(), NAME_None, RF_Transient);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(false);
	Mesh->SetupAttachment(this);
	Mesh->RegisterComponent();
	Pose(0.f, 0.f);
	Upload(true);
	Mesh->SetBoundsScale(60.f);
	Mesh->SetRenderCustomDepth(true);          // stencil 2: the hands (their own shell outline; the gear's ink line runs over them)
	Mesh->SetCustomDepthStencilValue(2);       // drawn over the level: never occlusion-culled behind a wall (bounds reach the eye)
	Mesh->SetVisibility(IsVisible());
	UE_LOG(LogTemp, Log, TEXT("OLRT hand mesh (%s): %d skin vertices, %d faces (and as many ink)"), bLeft ? TEXT("left") : TEXT("right"), Skin.Num(), Surf.Num());
}

void UOutlawsBoxHand::Pose(float Trigger, float Grip)
{
	// fingers curl toward the palm side (-Y for the right hand): negative yaw, on top of the rest pose
	const double S = bLeft ? -1.0 : 1.0;
	const float Curl[3] = { KnuckleCurl, JointCurl, TipCurl };
	TArray<FTransform, TInlineAllocator<16>> Xf;
	Xf.SetNum(Joints.Num());
	for (int32 j = 0; j < Joints.Num(); ++j)
	{
		const FJoint& J = Joints[j];
		FRotator R = J.Rest;
		if (!J.bThumb)       // the thumb rests on the face buttons: the grip does not move it
		{
			const double C = J.bIndex ? FMath::Clamp(Trigger, 0.f, 1.f) : FMath::Clamp(Grip, 0.f, 1.f) * GripCap[FMath::Clamp(J.Digit - 1, 0, 2)];
			R += FRotator(0.0, -S * Curl[J.Seg] * C, 0.0);
		}
		const FTransform Local(R, J.At);
		Xf[j] = J.Parent >= 0 ? Local * Xf[J.Parent] : Local;
	}
	// the index finger's tip and its way, knuckle to tip (the menu's pointer comes out of it)
	{
		int32 Knuckle = -1, Tip = -1;
		for (int32 j = 0; j < Joints.Num(); ++j)
		{
			if (!Joints[j].bIndex) continue;
			if (Joints[j].Seg == 0) Knuckle = j;
			if (Joints[j].Seg == 2) Tip = j;
		}
		if (Knuckle >= 0 && Tip >= 0)
		{
			IndexTipPosed = Xf[Tip].TransformPosition(FVector(Joints[Tip].Len, 0.0, 0.0));
			IndexWayPosed = (IndexTipPosed - Xf[Knuckle].GetLocation()).GetSafeNormal();
		}
	}
	// the skin: each vertex where its two joints carry it, blended
	SkinPosed.SetNumUninitialized(Skin.Num());
	for (int32 i = 0; i < Skin.Num(); ++i)
	{
		const FSkinVert& V = Skin[i];
		const FVector PA = V.A >= 0 ? Xf[V.A].TransformPosition(V.LA) : V.LA;
		if (V.W >= 0.999f)
		{
			SkinPosed[i] = PA;
			continue;
		}
		const FVector PB = V.B >= 0 ? Xf[V.B].TransformPosition(V.LB) : V.LB;
		SkinPosed[i] = PA * V.W + PB * (1.f - V.W);
	}
	for (FSection& Sec : Sections)
	{
		Sec.Posed.SetNumUninitialized(Sec.Pool.Num());
		for (int32 k = 0; k < Sec.Pool.Num(); ++k) Sec.Posed[k] = SkinPosed[Sec.Pool[k]];
	}
}

void UOutlawsBoxHand::Upload(bool bCreate)
{
	if (!Mesh) return;
	int32 Index = 0;
	for (FSection& Sec : Sections)
	{
		if (Sec.Tris.Num() == 0 || !Sec.Material) continue;
		if (bCreate)
		{
			TArray<FVector> Normals;
			Normals.Init(FVector::UpVector, Sec.Posed.Num());
			TArray<FVector2D> UV;
			UV.Init(FVector2D(0.5, 0.5), Sec.Posed.Num());
			Mesh->CreateMeshSection_LinearColor(Index, Sec.Posed, Sec.Tris, Normals, UV, TArray<FLinearColor>(), TArray<FProcMeshTangent>(), false);
			Mesh->SetMaterial(Index, Sec.Material);
		}
		else
		{
			// positions only: the normals, UVs and colours stay as made
			Mesh->UpdateMeshSection_LinearColor(Index, Sec.Posed, TArray<FVector>(), TArray<FVector2D>(), TArray<FLinearColor>(), TArray<FProcMeshTangent>());
		}
		++Index;
	}
}

FColor UOutlawsBoxHand::NaturalNail()
{
	return NailColour;
}

void UOutlawsBoxHand::SetNailMaterial(UMaterialInterface* M)
{
	if (!M) return;
	Sections[SecNail].Material = M;
	if (!Mesh || Sections[SecNail].Tris.Num() == 0) return;
	int32 Index = 0;                                   // (Upload's numbering: the sections with triangles, in order)
	for (int32 i = 0; i < SecNail; ++i) if (Sections[i].Tris.Num() > 0 && Sections[i].Material) ++Index;
	Mesh->SetMaterial(Index, M);
}

void UOutlawsBoxHand::SetCurl(float Trigger, float Grip)
{
	// re-posed only when the curl moves a 64th or the caps change: an analog trigger's jitter is not a new pose
	auto Q = [](float V) { return int64(FMath::RoundToInt(FMath::Clamp(V, 0.f, 1.f) * 64.f)); };
	const int64 Key = Q(Trigger) | (Q(Grip) << 8) | (Q(GripCap[0]) << 16) | (Q(GripCap[1]) << 24) | (Q(GripCap[2]) << 32);
	if (Key == PoseKey || !Mesh) return;
	PoseKey = Key;
	Pose(Trigger, Grip);
	Upload(false);
}
