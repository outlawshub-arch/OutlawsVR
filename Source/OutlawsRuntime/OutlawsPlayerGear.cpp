// Held gear from the items wheel (user 2026-10-01): the lantern, the map and the boiler plate, each taken into the hand
// that picked it as the tools are (HeldTool, ToolRoot) and put away the same way (a weapon drawn into that hand, the
// same block picked again).
#include "OutlawsPlayer.h"
#include "OutlawsFlags.h"
#include "OutlawsInk.h"
#include "OutlawsBoxHand.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsLevel.h"
#include "OutlawsGeometry.h"
#include "ProceduralMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Camera/CameraComponent.h"
#include "MotionControllerComponent.h"
#include "Engine/CanvasRenderTarget2D.h"
#include "Engine/Canvas.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "CanvasItem.h"
#include "RenderUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Math/RandomStream.h"
#include "Components/TextRenderComponent.h"
#include "OutlawsCoop.h"
#include "Misc/CommandLine.h"
#include "OutlawsTextures.h"
#include "OutlawsObjMesh.h"
#include "OutlawsLab.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "OutlawsFeel.h"
#include "UnrealClient.h"
#include "OutlawsDeviceProfile.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Parse.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "RenderCore.h"
#include "RHI.h"
#include "ShaderCompiler.h"
#include "Materials/Material.h"
#include "MaterialShared.h"
#include "Components/SceneCaptureComponent2D.h"
#include "OutlawsShotTests.h"

namespace
{
	// the plate's size by the one picked up (gbplate3 50, gbplate2 100, gbplate1 150): across, up, in cm
	FVector2D PlateSizeFor(int32 Max)
	{
		return Max > 100 ? FVector2D(44.0, 36.0) : Max > 50 ? FVector2D(36.0, 29.0) : FVector2D(28.0, 22.0);
	}
	constexpr double PlateThick = 0.7;
	constexpr int32 PlateWearDents = 10;    // a saved game's worn plate: one dent for each tenth of it worn away
	constexpr int32 PlateMaxDents = 48;     // past this many, a hit deepens a dent it already has
	// the worn plate (REQ-CHESTPLATE): its middle under the neck and ahead of it along the body, its face tipped up toward
	// the eyes so that looking down shows its front, not its rim
	constexpr double ChestDrop = 22.0;
	constexpr double ChestAhead = 17.0;
	constexpr double ChestTilt = 35.0;      // (on another player, OutlawsRemotePlayer.cpp: 12, nearly upright)
	constexpr double LanternDrop = 12.0;    // the fist to the lantern's swing centre (its weight)

	void AddTri(TArray<FVector>& P, TArray<int32>& T, const FVector& A, const FVector& B, const FVector& C, bool bBoth)
	{
		const int32 I = P.Num();
		P.Add(A); P.Add(B); P.Add(C);
		T.Add(I); T.Add(I + 1); T.Add(I + 2);
		if (bBoth) { T.Add(I); T.Add(I + 2); T.Add(I + 1); }
	}

	// the pickup's own boiler plate (item3d pickups.py build_plate: the sprite's riveted shard, inflated and bowed like a
	// breastplate; user 2026-10-01: the held plate should be the one picked up), turned into the held plate's frame: X
	// out from the holder (the way it bulges), Y across, Z up, centred; cm as modelled.  Loaded once (the
	// drawn grid plate when the model is missing)
	struct FPlateShape
	{
		bool bOk = false;
		TArray<FVector> Pos, Nrm;
		TArray<FVector2D> UV;
		TArray<FLinearColor> Col;
		TArray<int32> Tri;
		FString Texture;
		FVector2D Size = FVector2D(1.0, 1.0);   // across, up
	};
	const FPlateShape& PlateShape()
	{
		static FPlateShape S;
		static bool bTried = false;
		if (bTried) return S;
		bTried = true;
		OutlawsObj::FObjMesh Obj;
		const FString Path = FPaths::Combine(FOutlawsAssets::PlaceholderDir(), TEXT("items3d/GBPLATE.obj"));
		if (!FPaths::FileExists(Path) || !OutlawsObj::Load(Path, 1.0, true, Obj) || !Obj.Bounds.IsValid) return S;
		// (PLATEFACES) turned to face out as the floor's copy is (FACETURN): the file has 4 % of its area facing out, so on the
		// one-sided held material the worn plate showed its inside and the headset's ink shell sat inside it.  The mirror below
		// moves the corners and turns each face back with them, so what faces out here still faces out.  -OLOldScopeFaces: as the
		// file has it
		OutlawsObj::FaceItemModelOutward(Obj, TEXT("OLRT worn plate GBPLATE"));
		// its axes by its extents: the thinnest through it, the longest up it
		const FVector E = Obj.Bounds.GetSize();
		int32 Thin = 0, Tall = 0;
		for (int32 a = 1; a < 3; ++a)
		{
			if (E[a] < E[Thin]) Thin = a;
			if (E[a] > E[Tall]) Tall = a;
		}
		if (Thin == Tall) return S;
		const int32 Across = 3 - Thin - Tall;
		const FVector C = Obj.Bounds.GetCenter();
		// which way it bows: its middle column stands out past its edges
		double Mid = 0.0, Edge = 0.0;
		int32 NM = 0, NE = 0;
		for (const TPair<FString, FOutlawsMeshSection>& Kv : Obj.Sections)
		{
			for (const FVector& P : Kv.Value.Positions)
			{
				const double A = FMath::Abs(P[Across] - C[Across]) / FMath::Max(1.0, E[Across]);
				if (A < 0.15) { Mid += P[Thin]; ++NM; }
				else if (A > 0.35) { Edge += P[Thin]; ++NE; }
			}
		}
		const double Sgn = (NM > 0 && NE > 0 && Mid / NM < Edge / NE) ? -1.0 : 1.0;
		auto Unit = [](int32 a) { FVector V = FVector::ZeroVector; V[a] = 1.0; return V; };
		const bool bFlip = Sgn * FVector::DotProduct(Unit(Thin), FVector::CrossProduct(Unit(Across), Unit(Tall))) < 0.0;   // (a mirror turns the faces)
		for (const TPair<FString, FOutlawsMeshSection>& Kv : Obj.Sections)
		{
			const FOutlawsMeshSection& Sec = Kv.Value;
			const int32 Base = S.Pos.Num();
			for (int32 i = 0; i < Sec.Positions.Num(); ++i)
			{
				const FVector P = Sec.Positions[i] - C;
				S.Pos.Add(FVector(Sgn * P[Thin], P[Across], P[Tall]));
				const FVector N = Sec.Normals.IsValidIndex(i) ? Sec.Normals[i] : FVector::ForwardVector;
				S.Nrm.Add(FVector(Sgn * N[Thin], N[Across], N[Tall]));
				S.UV.Add(Sec.UVs.IsValidIndex(i) ? Sec.UVs[i] : FVector2D(0.5, 0.5));
				S.Col.Add(Sec.Colors.IsValidIndex(i) ? Sec.Colors[i] : FLinearColor::White);
			}
			for (int32 t = 0; t + 2 < Sec.Triangles.Num(); t += 3)
			{
				S.Tri.Add(Base + Sec.Triangles[t]);
				S.Tri.Add(Base + Sec.Triangles[t + (bFlip ? 2 : 1)]);
				S.Tri.Add(Base + Sec.Triangles[t + (bFlip ? 1 : 2)]);
			}
			if (S.Texture.IsEmpty()) S.Texture = Sec.Texture;
		}
		S.Size = FVector2D(E[Across], E[Tall]);
		S.bOk = S.Tri.Num() > 0;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT held plate: the pickup's model, %d triangles, %.0f x %.0f cm as modelled"), S.Tri.Num() / 3, S.Size.X, S.Size.Y);
		return S;
	}
	// held, the plate keeps the area the drawn plates had (44 x 36, 36 x 29, 28 x 22 by the one picked up)
	double PlateScale(int32 Max)
	{
		const FPlateShape& S = PlateShape();
		const FVector2D D = PlateSizeFor(Max);
		return S.bOk ? FMath::Sqrt(D.X * D.Y / FMath::Max(1.0, S.Size.X * S.Size.Y)) : 1.0;
	}
	FVector2D PlateDimsFor(int32 Max)
	{
		const FPlateShape& S = PlateShape();
		return S.bOk ? S.Size * PlateScale(Max) : PlateSizeFor(Max);
	}
	// (across, up) on the plate itself, the shard and not its box: inside any of its faces seen square on
	bool PlateCovers(const FVector2D& At, int32 Max)
	{
		const FPlateShape& S = PlateShape();
		if (!S.bOk) return true;
		const FVector2D Q = At / PlateScale(Max);
		for (int32 t = 0; t + 2 < S.Tri.Num(); t += 3)
		{
			const FVector& A3 = S.Pos[S.Tri[t]];
			const FVector& B3 = S.Pos[S.Tri[t + 1]];
			const FVector& C3 = S.Pos[S.Tri[t + 2]];
			const FVector2D A(A3.Y, A3.Z), B(B3.Y, B3.Z), C(C3.Y, C3.Z);
			const double D1 = FVector2D::CrossProduct(B - A, Q - A), D2 = FVector2D::CrossProduct(C - B, Q - B), D3 = FVector2D::CrossProduct(A - C, Q - C);
			const bool bNeg = D1 < 0.0 || D2 < 0.0 || D3 < 0.0, bPos = D1 > 0.0 || D2 > 0.0 || D3 > 0.0;
			if (!(bNeg && bPos)) return true;
		}
		return false;
	}
}

void AOutlawsPlayer::ToggleGear(int32 Hand, const FString& Item)
{
	AOutlawsLevelActor* L = Level.Get();
	const FString Key = Item.ToLower();
	const int32 O = 1 - Hand;
	if (HeldTool[Hand] == Key || HeldTool[O] == Key)
	{
		DropTool(HeldTool[Hand] == Key ? Hand : O);     // the same block again: put away, from whichever hand has it
		Pulse(Hand, 0.2f);
		return;
	}
	if (Key == TEXT("ibplate"))
	{
		// (REQ-CHESTPLATE) worn from its pickup, never taken into a hand: its place on the wheel only shows what is left
		if (!L || L->GetArmour() <= 0) Refuse(Hand);
		else Pulse(Hand, 0.2f);
		return;
	}
	if (HoldsGatling(O))
	{
		Refuse(Hand);                       // (the gatling's crank hand: user 2026-10-01)
		return;
	}
	// picked by a hand holding a weapon while the other hand is quite free: into the free hand, the weapon kept (user
	// 2026-10-01: the gun hand picking the lantern put the gun away with the other hand empty)
	int32 To = Hand;
	if (Equipped[Hand] >= 0 && !HoldsGatling(Hand) && Equipped[O] < 0 && HeldTool[O].IsEmpty() && !HeldBottle[O] && !HeldItem[O].IsValid()
		&& !GrabbedEnemy[O].IsValid() && GrabHand != O)
	{
		To = O;
		bSupport[O] = false;                // (off a long gun's forend)
	}
	EquipTool(To, Key);
}

void AOutlawsPlayer::BuildGearModel(int32 Hand)
{
	TArray<USceneComponent*> Kids;
	ToolRoot[Hand]->GetChildrenComponents(true, Kids);       // (all of it: the arrow rides on the page, the lantern's parts on its pivot)
	for (USceneComponent* K : Kids) if (K) K->DestroyComponent();
	GearMesh[Hand] = nullptr;
	MapArrow[Hand] = nullptr;
	for (int32 k = 0; k < MapMarks; ++k)
	{
		MapPartner[Hand * MapMarks + k] = nullptr;
		MapPartnerName[Hand * MapMarks + k] = nullptr;
	}
	LanternHang[Hand] = nullptr;
	LanternFlame[Hand] = nullptr;
	LanternGlassMid[Hand] = nullptr;
	LanternBody[Hand] = nullptr;
	LanternFlameH[Hand] = 0.f;
	bLanternBob[Hand] = false;
	ToolBuilt[Hand] = HeldTool[Hand];
	ToolTipX[Hand] = 0.0;
	const FString& Item = HeldTool[Hand];
	if (Item == TEXT("imap"))
	{
		BuildMapPage(Hand);
	}
	else if (Item == TEXT("ilantern"))
	{
		// a hurricane lantern hung from the fist by its bail: a pivot kept hanging under its weight (UpdateGear), the bail,
		// the cap and chimney, the glass (glowing while lit), its guard wires and the fount
		USceneComponent* Hang = NewObject<USceneComponent>(this);
		Hang->SetupAttachment(ToolRoot[Hand]);
		Hang->RegisterComponent();
		LanternHang[Hand] = Hang;
		UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
		UStaticMesh* Cyl = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
		UStaticMesh* Ball = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
		auto Part = [&](UStaticMesh* Shape, const FVector& At, const FRotator& Rot, const FVector& Sc, UMaterialInterface* Mat) -> UStaticMeshComponent*
		{
			UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this);
			C->SetStaticMesh(Shape);
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->SetCastShadow(false);
			C->SetupAttachment(Hang);
			C->RegisterComponent();
			C->SetRelativeLocationAndRotation(At, Rot);
			C->SetRelativeScale3D(Sc);
			C->SetMaterial(0, Mat);
			C->SetBoundsScale(40.f);
			C->SetRenderCustomDepth(true);
			C->SetCustomDepthStencilValue(1);
			return C;
		};
		UMaterialInterface* Iron = HeldSolidMaterial(FColor(58, 54, 50));
		UMaterialInterface* Tin = HeldSolidMaterial(FColor(104, 92, 70));
		UMaterialInterface* GlowBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_FxGlowHeld.M_OLR_FxGlowHeld"));     // (at the held depth)
		if (!GlowBase) GlowBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_FxGlow.M_OLR_FxGlow"));
		UMaterialInstanceDynamic* Glass = GlowBase ? UMaterialInstanceDynamic::Create(GlowBase, this) : nullptr;
		UMaterialInstanceDynamic* Fire = GlowBase ? UMaterialInstanceDynamic::Create(GlowBase, this) : nullptr;
		if (Glass) { KeepAlive.Add(Glass); Glass->SetVectorParameterValue(TEXT("Color"), FLinearColor(1.f, 0.6f, 0.26f)); }
		if (Fire) { KeepAlive.Add(Fire); Fire->SetVectorParameterValue(TEXT("Color"), FLinearColor(1.f, 0.85f, 0.5f)); Fire->SetScalarParameterValue(TEXT("Glow"), 6.f); }
		LanternGlassMid[Hand] = Glass;
		// the game's own lantern (user 2026-10-01: the two lanterns met halfway): the pickup's model turned from the
		// player's GLANTERN sprite (item3d/pickups.py: brass globe, guards and bail in the sprite's own paint), at 30 cm
		// (between its 43 and the first held one's 22), hung from the top of its bail in the fist, cel-shaded and inked
		// as the rest of the held gear; lit, a glowing sleeve over its glass brightens the painted glass and flame
		if (!OLFlags::Param(TEXT("OLVEGuns")))
		{
			static TSharedPtr<OutlawsObj::FObjMesh> LanternObj;
			static bool bLanternTried = false;
			if (!bLanternTried)
			{
				bLanternTried = true;
				const FString Path = FPaths::Combine(FOutlawsAssets::PlaceholderDir(), TEXT("items3d/GLANTERN.obj"));
				LanternObj = MakeShared<OutlawsObj::FObjMesh>();
				if (!FPaths::FileExists(Path) || !OutlawsObj::Load(Path, 1.0, true, *LanternObj)) LanternObj.Reset();
				else OutlawsObj::FaceItemModelOutward(*LanternObj, TEXT("OLRT held lantern"));      // (FACETURN: as the floor's; it has none to turn)
			}
			UMaterialInterface* Base = HeldBase();
			if (LanternObj.IsValid() && Base && LanternObj->Bounds.IsValid)
			{
				const FBox B = LanternObj->Bounds;
				const double K = 30.0 / FMath::Max(1.0, B.Max.Z - B.Min.Z);
				const FVector Top(B.GetCenter().X, B.GetCenter().Y, B.Max.Z);
				UProceduralMeshComponent* Body = NewObject<UProceduralMeshComponent>(this);
				Body->bUseAsyncCooking = false;
				Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				Body->SetCastShadow(false);
				Body->SetupAttachment(Hang);
				Body->RegisterComponent();
				// turned a quarter round in the fist (user 2026-10-01: it looked gripped the wrong way)
				Body->SetRelativeRotation(FRotator(0.0, 90.0, 0.0));
				LanternBody[Hand] = Body;
				// the glass on its own, see-through at the held depth (no ink shell behind it), lit by the flame; the painted
				// flame card left out: the game's own fire burns in its place (user 2026-10-01: as on the dynamite's wick)
				UMaterialInterface* SpriteBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_HeldSprite.M_OLR_HeldSprite"));
				UProceduralMeshComponent* GlassPm = nullptr;
				FBox FlameBox(ForceInit);
				int32 Index = 0;
				for (const TPair<FString, FOutlawsMeshSection>& Kv : LanternObj->Sections)
				{
					const FOutlawsMeshSection& S = Kv.Value;
					TArray<FVector> Pos;
					Pos.Reserve(S.Positions.Num());
					for (const FVector& P : S.Positions) Pos.Add((P - Top) * K + FVector(0.0, 0.0, 0.6));     // (the bail's top in the fist)
					if (SpriteBase && Kv.Key.Equals(TEXT("flame"), ESearchCase::IgnoreCase))
					{
						for (const FVector& P : Pos) FlameBox += P;
						continue;
					}
					if (SpriteBase && Kv.Key.Equals(TEXT("glass"), ESearchCase::IgnoreCase))
					{
						if (!GlassPm)
						{
							GlassPm = NewObject<UProceduralMeshComponent>(this);
							GlassPm->bUseAsyncCooking = false;
							GlassPm->SetCollisionEnabled(ECollisionEnabled::NoCollision);
							GlassPm->SetCastShadow(false);
							GlassPm->SetupAttachment(Body);
							GlassPm->RegisterComponent();
							GlassPm->SetBoundsScale(40.f);
							GlassPm->SetTranslucentSortPriority(1);
						}
						const int32 Gi = GlassPm->GetNumSections();
						GlassPm->CreateMeshSection_LinearColor(Gi, Pos, S.Triangles, S.Normals, S.UVs, S.Colors, TArray<FProcMeshTangent>(), false);
						UMaterialInstanceDynamic* Gm = UMaterialInstanceDynamic::Create(SpriteBase, this);
						if (UTexture2D* Tex = OutlawsObj::LoadTextureFile(S.Texture))
						{
							Gm->SetTextureParameterValue(TEXT("Tex"), Tex);
							KeepAlive.Add(Tex);
						}
						Gm->SetScalarParameterValue(TEXT("Opacity"), 0.35f);
						KeepAlive.Add(Gm);
						GlassPm->SetMaterial(Gi, Gm);
						LanternGlassMid[Hand] = Gm;
						continue;
					}
					Body->CreateMeshSection_LinearColor(Index, Pos, S.Triangles, S.Normals, S.UVs, S.Colors, TArray<FProcMeshTangent>(), false);
					UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, this);
					if (UTexture2D* Tex = OutlawsObj::LoadTextureFile(S.Texture))
					{
						Mid->SetTextureParameterValue(TEXT("Tex"), Tex);
						KeepAlive.Add(Tex);
					}
					KeepAlive.Add(Mid);
					HeldMids.Add(Mid);
					Body->SetMaterial(Index, Mid);
					++Index;
				}
				KeepDrawnOverWorld(Body);
				// (no held sprite material: the glowing sleeve over the painted glass, as before)
				if (!GlassPm && Glass) Part(Cyl, FVector(0.0, 0.0, -15.5), FRotator::ZeroRotator, FVector(0.088, 0.088, 0.15), Glass);
				// the flame: fire01 on a card turned to the eyes (UpdateGear), standing where the painted one stood
				AOutlawsLevelActor* Lv = Level.Get();
				TArray<FLinearColor> Cells;
				float Fps = 10.f, Aspect = 0.66f;
				UMaterialInstanceDynamic* WorldFire = Lv && SpriteBase && FlameBox.IsValid ? Lv->MakeFireLook(this, Cells, Fps, Aspect) : nullptr;
				UTexture* FireTex = nullptr;
				FLinearColor CellScale(1.f, 1.f, 0.f, 0.f);
				if (WorldFire)
				{
					WorldFire->GetTextureParameterValue(FMaterialParameterInfo(TEXT("Tex")), FireTex);
					WorldFire->GetVectorParameterValue(FMaterialParameterInfo(TEXT("UVScale")), CellScale);
				}
				if (FireTex && Cells.Num() > 0 && Lv->GetSpriteQuad())
				{
					FireCells = Cells;
					FireFps = Fps;
					FireAspect = Aspect;
					UMaterialInstanceDynamic* Fm = UMaterialInstanceDynamic::Create(SpriteBase, this);
					Fm->SetTextureParameterValue(TEXT("Tex"), FireTex);
					Fm->SetVectorParameterValue(TEXT("UVScale"), CellScale);
					Fm->SetVectorParameterValue(TEXT("UVOffset"), Cells[0]);
					Fm->SetScalarParameterValue(TEXT("Brightness"), 1.2f);
					KeepAlive.Add(Fm);
					UStaticMeshComponent* Fl = Part(Lv->GetSpriteQuad(), FVector::ZeroVector, FRotator::ZeroRotator, FVector::OneVector, Fm);
					Fl->SetRenderCustomDepth(false);
					Fl->SetTranslucentSortPriority(2);          // (over the glass it burns behind)
					LanternFlame[Hand] = Fl;
					LanternFlameAt[Hand] = FVector(FlameBox.GetCenter().X, FlameBox.GetCenter().Y, FlameBox.Min.Z);
					LanternFlameH[Hand] = float(FlameBox.Max.Z - FlameBox.Min.Z);
				}
				else if (Fire)
				{
					LanternFlame[Hand] = Part(Ball, FVector(0.0, 0.0, -16.5), FRotator::ZeroRotator, FVector(0.03, 0.03, 0.05), Fire);
				}
				HeldLight = -1.f;
				return;
			}
		}
		Part(Cyl, FVector::ZeroVector, FRotator(0.0, 0.0, 90.0), FVector(0.012, 0.012, 0.07), Iron);       // the bail's grip, across the fist
		Part(Cube, FVector(0.0, 0.0, -3.2), FRotator::ZeroRotator, FVector(0.004, 0.004, 0.064), Iron);   // the bail down to the cap
		Part(Cyl, FVector(0.0, 0.0, -6.6), FRotator::ZeroRotator, FVector(0.04, 0.04, 0.024), Tin);       // the chimney
		Part(Cyl, FVector(0.0, 0.0, -8.4), FRotator::ZeroRotator, FVector(0.086, 0.086, 0.016), Tin);     // the cap
		if (Glass) Part(Cyl, FVector(0.0, 0.0, -13.2), FRotator::ZeroRotator, FVector(0.072, 0.072, 0.086), Glass);   // the glass
		for (int32 k = 0; k < 4; ++k)
		{
			const double A = UE_DOUBLE_HALF_PI * k + UE_DOUBLE_PI / 4.0;
			Part(Cube, FVector(4.0 * FMath::Cos(A), 4.0 * FMath::Sin(A), -13.2), FRotator::ZeroRotator, FVector(0.003, 0.003, 0.088), Iron);   // the guard
		}
		Part(Cyl, FVector(0.0, 0.0, -19.0), FRotator::ZeroRotator, FVector(0.094, 0.094, 0.032), Tin);    // the fount
		if (Fire) LanternFlame[Hand] = Part(Ball, FVector(0.0, 0.0, -14.0), FRotator::ZeroRotator, FVector(0.012, 0.012, 0.024), Fire);
	}
	HeldLight = -1.f;                      // the new parts take the current sector light
}

// ---- the boiler plate -----------------------------------------------------------------------------------------------

void AOutlawsPlayer::EnsurePlateMarks()
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L) return;
	if (PlateSerialSeen != L->PlateSerial)
	{
		PlateSerialSeen = L->PlateSerial;
		PlateMarks.Reset();                 // a new plate: unmarked
	}
	// a worn plate with no marks (a saved game's has lost them): as many dents as its wear calls for, scattered
	if (PlateMarks.Num() > 0) return;
	const int32 Max = FMath::Max(1, FMath::Max(L->ArmourMax, L->GetArmour()));
	const int32 Want = FMath::Clamp(int32((1.0 - double(L->GetArmour()) / Max) * PlateWearDents), 0, PlateWearDents);
	const FVector2D Dims = PlateDimsFor(Max);
	FRandomStream Rng(1847 + PlateMarks.Num());
	while (PlateMarks.Num() < Want)
	{
		FPlateMark M;
		M.Radius = Rng.FRandRange(1.8f, 2.6f);
		M.Depth = Rng.FRandRange(0.5f, 0.9f);
		for (int32 Try = 0; Try < 40; ++Try)
		{
			M.At = FVector2D(Rng.FRandRange(-0.38f, 0.38f) * Dims.X, Rng.FRandRange(-0.38f, 0.38f) * Dims.Y);
			if (PlateCovers(M.At, Max)) break;       // (on the shard, not beside it)
		}
		PlateMarks.Add(M);
	}
}

void AOutlawsPlayer::BuildPlate()
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L || !ChestRoot) return;
	EnsurePlateMarks();
	UProceduralMeshComponent* Pm = ChestMesh;
	if (!IsValid(Pm))
	{
		Pm = NewObject<UProceduralMeshComponent>(this);
		Pm->bUseAsyncCooking = false;
		Pm->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Pm->SetCastShadow(false);
		Pm->SetupAttachment(ChestRoot);
		Pm->RegisterComponent();
		Pm->SetBoundsScale(40.f);
		Pm->SetRenderCustomDepth(true);
		Pm->SetCustomDepthStencilValue(1);
		ChestMesh = Pm;
		UMaterialInterface* Base = HeldBase();
		if (PlateShape().bOk && Base)
		{
			UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, this);
			if (UTexture2D* Tex = OutlawsObj::LoadTextureFile(PlateShape().Texture))
			{
				Mid->SetTextureParameterValue(TEXT("Tex"), Tex);
				KeepAlive.Add(Tex);
			}
			KeepAlive.Add(Mid);
			HeldMids.Add(Mid);
			HeldLight = -1.f;
			Pm->SetMaterial(0, Mid);                                      // the pickup's own picture
		}
		else
		{
			Pm->SetMaterial(0, HeldSolidMaterial(FColor(92, 88, 84)));        // the plate
			Pm->SetMaterial(1, HeldSolidMaterial(FColor(134, 128, 120)));     // facing the light
			Pm->SetMaterial(2, HeldSolidMaterial(FColor(56, 53, 51)));        // turned from it
			Pm->SetMaterial(3, HeldSolidMaterial(FColor(186, 176, 158)));     // bare metal: the rivets
		}
	}
	const int32 PlateMax = FMath::Max(L->ArmourMax, L->GetArmour());
	PlateDims = PlateDimsFor(PlateMax);
	FillPlateMesh(Pm, PlateMax, PlateMarks);
	Pm->SetVisibility(ChestRoot->IsVisible(), true);
}

bool AOutlawsPlayer::BuildPlateLook(AActor* OnActor, int32 PlateMax, UProceduralMeshComponent*& Out)
{
	// (REQ-CHESTPLATE) the plate another player wears, drawn here as BuildThrownLook draws their thrown things: the same
	// model on the held material, the near pull off (it stands in the world, not before these eyes)
	Out = nullptr;
	UMaterialInterface* Base = HeldBase();
	if (!OnActor || !OnActor->GetRootComponent() || !Base || PlateMax <= 0) return false;
	UProceduralMeshComponent* Pm = NewObject<UProceduralMeshComponent>(OnActor);
	Pm->bUseAsyncCooking = false;
	Pm->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Pm->SetCastShadow(false);
	Pm->SetupAttachment(OnActor->GetRootComponent());
	Pm->SetUsingAbsoluteLocation(true);
	Pm->SetUsingAbsoluteRotation(true);
	Pm->SetUsingAbsoluteScale(true);
	Pm->RegisterComponent();
	FillPlateMesh(Pm, PlateMax, TArray<FPlateMark>());
	auto Mat = [&](const FString& TexPath, const FColor& Solid)
	{
		UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, OnActor);
		UTexture2D* Tex = TexPath.IsEmpty() ? nullptr : OutlawsObj::LoadTextureFile(TexPath);
		if (!Tex)
		{
			TArray<FColor> Px;
			Px.Init(Solid, 4);
			Tex = OutlawsTextures::MakeTextureBGRA(2, 2, Px, false, TF_Nearest, TA_Clamp);
		}
		if (Tex) Mid->SetTextureParameterValue(TEXT("Tex"), Tex);
		Mid->SetScalarParameterValue(TEXT("NearPull"), 0.f);
		return Mid;
	};
	if (PlateShape().bOk)
	{
		Pm->SetMaterial(0, Mat(PlateShape().Texture, FColor(92, 88, 84)));
	}
	else
	{
		Pm->SetMaterial(0, Mat(FString(), FColor(92, 88, 84)));
		Pm->SetMaterial(1, Mat(FString(), FColor(134, 128, 120)));
		Pm->SetMaterial(2, Mat(FString(), FColor(56, 53, 51)));
		Pm->SetMaterial(3, Mat(FString(), FColor(186, 176, 158)));
	}
	Out = Pm;
	return true;
}

void AOutlawsPlayer::FillPlateMesh(UProceduralMeshComponent* Pm, int32 PlateMax, const TArray<FPlateMark>& Marks)
{
	if (!Pm) return;
	const FVector2D Dims = PlateDimsFor(PlateMax);
	// a grid a centimetre square, front (+X, away from the wearer) and back; dents bow it toward the wearer
	const int32 NY = FMath::Max(2, FMath::CeilToInt(Dims.X)), NZ = FMath::Max(2, FMath::CeilToInt(Dims.Y));
	auto Grid = [&](int32 i, int32 j) { return FVector2D(-Dims.X * 0.5 + Dims.X * i / NY, -Dims.Y * 0.5 + Dims.Y * j / NZ); };
	auto Bow = [&](const FVector2D& P)
	{
		double X = 0.0;
		for (const FPlateMark& M : Marks)
		{
			if (M.Depth <= 0.f) continue;
			X -= M.Depth * FMath::Exp(-(P - M.At).SizeSquared() / FMath::Square(M.Radius * 1.05));
		}
		return X;
	};
	if (PlateShape().bOk)
	{
		// the pickup's model, dented as the drawn plate was (user 2026-10-01; REQ-CHESTPLATE: no holes, they were broken):
		// the faces near a dent split finer until they are a third of its size (so the model's big faces bow round it), the
		// steel bowed toward the wearer round each dent and darker where it slopes
		const FPlateShape& S = PlateShape();
		const double K = PlateScale(PlateMax);
		TArray<FVector> V0, N0;
		TArray<FVector2D> UV0;
		TArray<FLinearColor> C0;
		V0.Reserve(S.Pos.Num());
		for (int32 i = 0; i < S.Pos.Num(); ++i)
		{
			V0.Add(S.Pos[i] * K);
			N0.Add(S.Nrm[i]);
			UV0.Add(S.UV[i]);
			C0.Add(S.Col[i]);
		}
		auto Q2 = [&](int32 i) { return FVector2D(V0[i].Y, V0[i].Z); };
		auto NearDent = [&](int32 A, int32 B, int32 C)
		{
			const FVector2D Pa = Q2(A), Pb = Q2(B), Pc = Q2(C);
			const FVector2D Mid = (Pa + Pb + Pc) / 3.0;
			const double Reach = FMath::Max3((Pa - Mid).Size(), (Pb - Mid).Size(), (Pc - Mid).Size());
			for (const FPlateMark& M : Marks)
			{
				if (M.Depth > 0.f && Reach > M.Radius * 0.35 && (Mid - M.At).Size() < M.Radius * 2.2 + Reach) return true;
			}
			return false;
		};
		// a corner halfway along an edge, made once for both faces that share the edge
		TMap<uint64, int32> MidOf;
		auto Half = [&](int32 A, int32 B)
		{
			const uint64 Key = A < B ? (uint64(A) << 32) | uint32(B) : (uint64(B) << 32) | uint32(A);
			if (const int32* Have = MidOf.Find(Key)) return *Have;
			const int32 I = V0.Add((V0[A] + V0[B]) * 0.5);
			N0.Add((N0[A] + N0[B]).GetSafeNormal());
			UV0.Add((UV0[A] + UV0[B]) * 0.5);
			C0.Add((C0[A] + C0[B]) * 0.5f);
			MidOf.Add(Key, I);
			return I;
		};
		TArray<int32> T0;
		TFunction<void(int32, int32, int32, int32)> Emit = [&](int32 A, int32 B, int32 C, int32 Depth)
		{
			if (Depth < 5 && NearDent(A, B, C))
			{
				const int32 Ab = Half(A, B), Bc = Half(B, C), Ca = Half(C, A);
				Emit(A, Ab, Ca, Depth + 1);
				Emit(Ab, B, Bc, Depth + 1);
				Emit(Ca, Bc, C, Depth + 1);
				Emit(Ab, Bc, Ca, Depth + 1);
				return;
			}
			T0.Append({ A, B, C });
		};
		for (int32 t = 0; t + 2 < S.Tri.Num(); t += 3) Emit(S.Tri[t], S.Tri[t + 1], S.Tri[t + 2], 0);
		// the dents' bow and shade on every corner
		for (int32 i = 0; i < V0.Num(); ++i)
		{
			const FVector2D Q = Q2(i);
			V0[i].X += Bow(Q);
			const double Gy = Bow(Q + FVector2D(0.5, 0.0)) - Bow(Q - FVector2D(0.5, 0.0));
			const double Gz = Bow(Q + FVector2D(0.0, 0.5)) - Bow(Q - FVector2D(0.0, 0.5));
			const float Shade = 1.f - 0.45f * FMath::Clamp(float(FMath::Sqrt(Gy * Gy + Gz * Gz)) * 1.5f, 0.f, 1.f);
			C0[i].R *= Shade;
			C0[i].G *= Shade;
			C0[i].B *= Shade;
		}
		Pm->ClearAllMeshSections(); OutlawsInk::Forget(Pm);
		Pm->CreateMeshSection_LinearColor(0, V0, T0, N0, UV0, C0, TArray<FProcMeshTangent>(), false);
		return;
	}
	TArray<FVector> Pos[4];
	TArray<int32> Tri[4];
	// a facet's shade by its turn to a light from up and to the left, on the side it faces (the holder sees the back)
	const FVector LightFront = FVector(0.55, -0.35, 0.75).GetSafeNormal(), LightBack = FVector(-0.55, -0.35, 0.75).GetSafeNormal();
	auto Facet = [&](const FVector& A, const FVector& B, const FVector& C, bool bFront)
	{
		FVector N = FVector::CrossProduct(B - A, C - A).GetSafeNormal();
		if ((N.X > 0.0) != bFront) N = -N;
		const double Lit = FVector::DotProduct(N, bFront ? LightFront : LightBack);
		const double Flat = FVector::DotProduct(bFront ? FVector::ForwardVector : FVector::BackwardVector, bFront ? LightFront : LightBack);
		const int32 S = Lit > Flat + 0.06 ? 1 : Lit < Flat - 0.06 ? 2 : 0;
		AddTri(Pos[S], Tri[S], A, B, C, true);
	};
	for (int32 i = 0; i < NY; ++i)
	{
		for (int32 j = 0; j < NZ; ++j)
		{
			const FVector2D Q[4] = { Grid(i, j), Grid(i + 1, j), Grid(i + 1, j + 1), Grid(i, j + 1) };
			for (const bool bFront : { true, false })
			{
				const double Face = bFront ? PlateThick * 0.5 : -PlateThick * 0.5;
				FVector V[4];
				for (int32 k = 0; k < 4; ++k) V[k] = FVector(Face + Bow(Q[k]), Q[k].X, Q[k].Y);
				Facet(V[0], V[1], V[2], bFront);
				Facet(V[0], V[2], V[3], bFront);
			}
		}
	}
	// the rim: the plate's thickness round its edge
	auto Edge = [&](const FVector2D& A2, const FVector2D& B2)
	{
		const FVector A0(PlateThick * 0.5 + Bow(A2), A2.X, A2.Y), A1(-PlateThick * 0.5 + Bow(A2), A2.X, A2.Y);
		const FVector B0(PlateThick * 0.5 + Bow(B2), B2.X, B2.Y), B1(-PlateThick * 0.5 + Bow(B2), B2.X, B2.Y);
		AddTri(Pos[2], Tri[2], A0, B0, B1, true);
		AddTri(Pos[2], Tri[2], A0, B1, A1, true);
	};
	for (int32 i = 0; i < NY; ++i) { Edge(Grid(i, 0), Grid(i + 1, 0)); Edge(Grid(i, NZ), Grid(i + 1, NZ)); }
	for (int32 j = 0; j < NZ; ++j) { Edge(Grid(0, j), Grid(0, j + 1)); Edge(Grid(NY, j), Grid(NY, j + 1)); }
	// rivets round the edge, both faces: small raised squares
	{
		const double In = 1.7, Step = 5.0, Half = 0.45, Rise = 0.35;
		TArray<FVector2D> Spots;
		for (double y = -Dims.X * 0.5 + In; y <= Dims.X * 0.5 - In + 0.01; y += (Dims.X - 2.0 * In) / FMath::Max(1, FMath::RoundToInt((Dims.X - 2.0 * In) / Step)))
		{
			Spots.Add(FVector2D(y, -Dims.Y * 0.5 + In));
			Spots.Add(FVector2D(y, Dims.Y * 0.5 - In));
		}
		for (double z = -Dims.Y * 0.5 + In + Step; z < Dims.Y * 0.5 - In - 0.5; z += Step)
		{
			Spots.Add(FVector2D(-Dims.X * 0.5 + In, z));
			Spots.Add(FVector2D(Dims.X * 0.5 - In, z));
		}
		for (const FVector2D& S2 : Spots)
		{
			for (const double Sign : { 1.0, -1.0 })
			{
				const double Base = Sign * PlateThick * 0.5 + Bow(S2), Top = Base + Sign * Rise;
				FVector B4[4], T4[4];
				const FVector2D Off[4] = { FVector2D(-Half, -Half), FVector2D(Half, -Half), FVector2D(Half, Half), FVector2D(-Half, Half) };
				for (int32 k = 0; k < 4; ++k) { B4[k] = FVector(Base, S2.X + Off[k].X, S2.Y + Off[k].Y); T4[k] = FVector(Top, S2.X + Off[k].X, S2.Y + Off[k].Y); }
				AddTri(Pos[3], Tri[3], T4[0], T4[1], T4[2], true);
				AddTri(Pos[3], Tri[3], T4[0], T4[2], T4[3], true);
				for (int32 k = 0; k < 4; ++k)
				{
					AddTri(Pos[2], Tri[2], B4[k], B4[(k + 1) % 4], T4[(k + 1) % 4], true);
					AddTri(Pos[2], Tri[2], B4[k], T4[(k + 1) % 4], T4[k], true);
				}
			}
		}
	}
	Pm->ClearAllMeshSections(); OutlawsInk::Forget(Pm);
	for (int32 S = 0; S < 4; ++S)
	{
		if (Pos[S].Num() == 0) continue;
		TArray<FVector> Nrm;
		Nrm.Init(FVector::ForwardVector, Pos[S].Num());
		TArray<FVector2D> UV;
		UV.Init(FVector2D(0.5, 0.5), Pos[S].Num());
		TArray<FLinearColor> Col;
		Col.Init(FLinearColor::White, Pos[S].Num());
		Pm->CreateMeshSection_LinearColor(S, Pos[S], Tri[S], Nrm, UV, Col, TArray<FProcMeshTangent>(), false);
	}
}

// ---- the plate worn (REQ-CHESTPLATE) ------------------------------------------------------------------------------

float AOutlawsPlayer::ChestYaw() const
{
	static const bool bTest = OLFlags::Param(TEXT("OLChestTest"));
	// (the desktop: the view's yaw, as the other machines face this player's sprite; the actor's can lag behind it)
	if (!(bVR || bTest) || !bChestYawSet || !VROrigin) return Camera ? float(Camera->GetComponentRotation().Yaw) : float(GetActorRotation().Yaw);
	return float(FRotator::NormalizeAxis(VROrigin->GetComponentRotation().Yaw + ChestYawRel));
}

int32 AOutlawsPlayer::ChestPlateSize() const
{
	const AOutlawsLevelActor* L = Level.Get();
	if (!L || L->GetArmour() <= 0) return 0;
	return FMath::Max(L->ArmourMax, L->GetArmour());
}

bool AOutlawsPlayer::ChestPlateShown(FVector& OutAt, FRotator& OutRot) const
{
	if (!ChestRoot || !ChestMesh || !ChestMesh->IsVisible()) return false;
	OutAt = ChestRoot->GetComponentLocation();
	OutRot = ChestRoot->GetComponentRotation();
	return true;
}

void AOutlawsPlayer::DentPlate(int32 Lost)
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L || Lost <= 0) return;
	const int32 PlateMax = FMath::Max(1, FMath::Max(L->ArmourMax, L->GetArmour()));
	const FVector2D Dims = PlateDimsFor(PlateMax);
	const float Hit = FMath::Min(1.f, Lost / 15.f);      // (15 points: a shot of a point and a half, a big one)
	if (PlateMarks.Num() >= PlateMaxDents)
	{
		FPlateMark& M = PlateMarks[FMath::RandRange(0, PlateMarks.Num() - 1)];
		M.Depth = FMath::Min(1.6f, M.Depth + 0.25f * (0.5f + Hit));
		return;
	}
	// where a hit from its source would strike (the last hurt's, if it is this one's), pulled a little toward the middle
	// and scattered, on the shard itself
	FVector2D Toward = FVector2D::ZeroVector;
	if (ChestRoot && ChestHurtAt >= 0.0 && GetWorld() && GetWorld()->GetTimeSeconds() - ChestHurtAt < 0.25)
	{
		const FVector Local = ChestRoot->GetComponentTransform().InverseTransformPositionNoScale(ChestHurtFrom);
		Toward = FVector2D(Local.Y, Local.Z).GetSafeNormal() * 0.18;
	}
	FPlateMark Mk;
	for (int32 Try = 0; Try < 40; ++Try)
	{
		Mk.At = FVector2D((Toward.X + FMath::FRandRange(-0.3f, 0.3f)) * Dims.X, (Toward.Y + FMath::FRandRange(-0.3f, 0.3f)) * Dims.Y);
		if (PlateCovers(Mk.At, PlateMax)) break;
	}
	Mk.Radius = 1.8f + 1.0f * Hit;
	Mk.Depth = 0.7f + 0.8f * Hit;
	PlateMarks.Add(Mk);
}

void AOutlawsPlayer::UpdateChestPlate(float Dt)
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L || !Camera || !VROrigin) return;
	static const bool bTest = OLFlags::Param(TEXT("OLChestTest"));
	const bool bLocalShown = bVR || bTest || bTestPlatePickup;
	// the body's yaw in the play space: the head's, never more than 35 degrees off it, and eased toward it (held while
	// looking well down, where the head's yaw says little of the body's)
	const FRotator Cam = Camera->GetComponentRotation();
	const float HeadRel = float(FRotator::NormalizeAxis(Cam.Yaw - VROrigin->GetComponentRotation().Yaw));
	if (!bChestYawSet)
	{
		ChestYawRel = HeadRel;
		bChestYawSet = true;
	}
	else if (Cam.Pitch > -60.f)
	{
		const float D = FRotator::NormalizeAxis(HeadRel - ChestYawRel);
		if (D > 35.f) ChestYawRel += D - 35.f;
		else if (D < -35.f) ChestYawRel += D + 35.f;
		if (Cam.Pitch > -35.f) ChestYawRel += FRotator::NormalizeAxis(HeadRel - ChestYawRel) * FMath::Min(1.f, Dt * 1.2f);
		ChestYawRel = FRotator::NormalizeAxis(ChestYawRel);
	}
	// (-OLChestNetTest: a plate to be seen wearing on the other machine)
	static const bool bNetTest = OLFlags::Param(TEXT("OLChestNetTest"));
	if (bNetTest && UOutlawsCoop::IsActive(this) && !bDeadView && L->GetArmour() <= 0 && ChestNetGiven < 6)
	{
		ChestNetWait += Dt;
		if (ChestNetWait > 5.f)
		{
			ChestNetWait = 0.f;
			++ChestNetGiven;
			L->AddCount(TEXT("ibplate"), 150);
			UE_LOG(LogOutlaws, Log, TEXT("OLCOOP chest plate test: a heavy plate given to this player (armour %d)"), L->GetArmour());
		}
	}
	// a new level: its count starts again, nothing picked up; a new plate: unmarked
	if (PlateLevelSeen.Get() != L)
	{
		PlateLevelSeen = L;
		PlateSerialSeen = L->PlateSerial;
		ChestArmourSeen = L->GetArmour();
	}
	const int32 Armour = L->GetArmour();
	bool bRebuild = false;
	if (PlateSerialSeen != L->PlateSerial)
	{
		EnsurePlateMarks();
		bRebuild = true;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT the boiler plate is worn: armour %d of %d"), Armour, L->ArmourMax);
	}
	else if (ChestArmourSeen > 0 && Armour < ChestArmourSeen)
	{
		// armour lost (the original's rule: it takes its share of every hit): a dent; none left, it breaks and goes
		if (Armour > 0)
		{
			DentPlate(ChestArmourSeen - Armour);
			bRebuild = true;
			UE_LOG(LogOutlaws, Log, TEXT("OLRT the boiler plate is dented: armour %d of %d, %d dents"), Armour, L->ArmourMax, PlateMarks.Num());
		}
		else if (L->ArmourMax > 0)
		{
			L->Notify(TEXT("The boiler plate breaks apart"), 2.f);
			UE_LOG(LogOutlaws, Log, TEXT("OLRT the boiler plate breaks apart"));
		}
	}
	ChestArmourSeen = Armour;
	const bool bShow = bLocalShown && Armour > 0 && !bDeadView;
	if (!bShow)
	{
		if (ChestRoot) ChestRoot->SetVisibility(false, true);
		return;
	}
	if (!ChestRoot)
	{
		ChestRoot = NewObject<USceneComponent>(this, TEXT("ChestPlate"));
		ChestRoot->SetupAttachment(VROrigin);
		ChestRoot->RegisterComponent();
		bRebuild = true;
	}
	// under the head: the neck behind and below the eyes (they turn about it), the chest under that, ahead along the body
	const FQuat Cq = Camera->GetComponentQuat();
	const FVector Neck = Camera->GetComponentLocation() - Cq.GetForwardVector() * 9.0 - Cq.GetUpVector() * 10.0;
	const float Yaw = ChestYaw();
	const FVector Ahead = FRotator(0.0, Yaw, 0.0).Vector();
	ChestRoot->SetWorldLocationAndRotation(Neck - FVector(0.0, 0.0, ChestDrop) + Ahead * ChestAhead, FRotator(ChestTilt, Yaw, 0.0));
	if (!ChestRoot->IsVisible()) ChestRoot->SetVisibility(true, true);
	if (bRebuild || !IsValid(ChestMesh)) BuildPlate();
}

// ---- the map --------------------------------------------------------------------------------------------------------

bool AOutlawsPlayer::BuildMapDrawing()
{
	AOutlawsLevelActor* L = Level.Get();
	const FOutlawsLevel* Lv = L ? L->LiveLevel() : nullptr;
	if (!Lv) return false;
	const FOutlawsSector* Here = Lv->Sector(L->PlayerSectorIndex());
	MapLayer = Here ? Here->Layer : 0;
	// the level as far as it joins up with where the player stands (every layer; rooms off on their own, a cutscene's
	// set far out, are left off: they shrank the walls to a speck), in Unreal's plan: north (+X) up the page, east (+Y)
	// to the right
	MapSectors.Reset();
	{
		TArray<int32> Todo;
		const int32 Start = Here ? L->PlayerSectorIndex() : 0;
		if (Lv->Sectors.IsValidIndex(Start)) { Todo.Add(Start); MapSectors.Add(Start); }
		while (Todo.Num() > 0)
		{
			const FOutlawsSector* S = Lv->Sector(Todo.Pop(false));
			if (!S) continue;
			for (const FOutlawsWall& Wl : S->Walls)
			{
				for (const int32 To : { Wl.Adjoin, Wl.DAdjoin })
				{
					if (Lv->Sectors.IsValidIndex(To) && !MapSectors.Contains(To)) { MapSectors.Add(To); Todo.Add(To); }
				}
			}
			if (Lv->Sectors.IsValidIndex(S->VAdjoin) && !MapSectors.Contains(S->VAdjoin)) { MapSectors.Add(S->VAdjoin); Todo.Add(S->VAdjoin); }
		}
	}
	// (BUG-TRAINMAP, user 16 Oct: the train's map "seems to show too many sectors"; the train's length should fill the map's
	// height "and nothing more") a level carried along by its ground (VELFLOORONLY floors with a VELOCITY: the train's
	// ground, the gaps between its cars and the scenery strips rush past at 62.5 u/s) is mapped by what stays put: the cars,
	// their roofs and couplings.  The moving ground is left off the drawing, as is the sky round it (below).  Only where
	// every joined sector with a VELOCITY moves alike, one ground carrying the level along: the canyon's currents and winds
	// each move their own way and its map stays whole (the train's 13 LOADED..DUMMY sectors, joined to nothing far off,
	// move at 60)
	bool bCarried = false;
	if (!OLFlags::Param(TEXT("OLOldTrainMap")))
	{
		FVector Along = FVector::ZeroVector;
		bool bMixed = false;
		for (const int32 Si : MapSectors)
		{
			const FOutlawsSector& S = Lv->Sectors[Si];
			if (S.Velocity.IsNearlyZero()) continue;
			if (!Along.IsNearlyZero() && !S.Velocity.Equals(Along, 1e-3)) { bMixed = true; break; }
			Along = S.Velocity;
			if (S.Flags & OutlawsFlags::VelFloorOnly) bCarried = true;
		}
		if (bMixed) bCarried = false;
	}
	int32 Ground = 0;
	if (bCarried)
	{
		for (auto It = MapSectors.CreateIterator(); It; ++It)
		{
			if (!Lv->Sectors[*It].Velocity.IsNearlyZero()) { It.RemoveCurrent(); ++Ground; }
		}
	}
	// sized to the places, not the sky: a sector many times bigger than nearly all the rest (the hideout's kilometre-wide
	// outer ground) is left out of the page's size (its walls run off the paper)
	TArray<double> Sizes;
	TMap<int32, double> SizeOf;
	for (const int32 Si : MapSectors)
	{
		FBox2D B(ForceInit);
		for (const FVector2D& V : Lv->Sectors[Si].Verts) B += V;
		const double Sz = B.bIsValid ? B.GetSize().GetMax() : 0.0;
		Sizes.Add(Sz);
		SizeOf.Add(Si, Sz);
	}
	Sizes.Sort();
	// (the train's: ten times, its many small door sectors would otherwise leave out its roofs and flat cars, 96 units long
	// against a 95th size of about 31; its sky round the scenery is 6912)
	const double Big = Sizes.Num() > 0 ? (bCarried ? 10.0 : 3.0) * Sizes[FMath::Min(Sizes.Num() - 1, int32(Sizes.Num() * 0.95))] : 0.0;
	FBox2D Box(ForceInit);
	int32 LeftOut = 0;
	for (auto It = MapSectors.CreateIterator(); It; ++It)
	{
		if (Sizes.Num() > 8 && SizeOf.FindRef(*It) > Big)
		{
			++LeftOut;
			if (bCarried) It.RemoveCurrent();       // (the train's sky round the scenery: not drawn either)
			continue;
		}
		for (const FVector2D& V : Lv->Sectors[*It].Verts) Box += FVector2D(V.Y * FOutlawsGeometry::UnitsToCm, V.X * FOutlawsGeometry::UnitsToCm);
	}
	if (!Box.bIsValid) return false;
	const FVector2D Span = Box.GetSize();     // (north-south, east-west)
	// the train (north to south on the page as it lies in the level) fills the page's height with a narrower strip of
	// paper beside it: its 900 units long and 38 across would otherwise sit in a wide empty page
	const double Aspect = FMath::Clamp(Span.Y / FMath::Max(1.0, Span.X), bCarried ? 0.45 : 0.72, 1.4);   // across over up
	const double Fill = bCarried ? 0.88 : 0.84;      // (the train's ends inside the arrow's 0.9 hold at the page's edge)
	MapPageSize = Aspect >= 1.0 ? FVector2D(28.0, 28.0 / Aspect) : FVector2D(28.0 * Aspect, 28.0);
	MapScale = FMath::Min(MapPageSize.X * Fill / FMath::Max(1.0, Span.Y), MapPageSize.Y * Fill / FMath::Max(1.0, Span.X));
	MapCentre = Box.GetCenter();
	// the drawing: walls in black on the paper (DrawMapCanvas)
	// (the train's narrower page: the picture narrower too, its height held at 1424, so a page cm is as many pixels each way)
	const int32 RH0 = FMath::RoundToInt(1024.0 * MapPageSize.Y / MapPageSize.X);
	const int32 RW = RH0 > 1424 ? FMath::RoundToInt(1424.0 * MapPageSize.X / MapPageSize.Y) : 1024;
	const int32 RH = FMath::Clamp(RH0, 256, 1424);
	if (!MapCanvas)
	{
		MapCanvas = NewObject<UCanvasRenderTarget2D>(this);
		MapCanvas->RenderTargetFormat = RTF_RGBA8_SRGB;
		MapCanvas->TargetGamma = 1.f;
		MapCanvas->ClearColor = FLinearColor(0.78f, 0.7f, 0.55f);
		MapCanvas->InitAutoFormat(RW, RH);
		MapCanvas->OnCanvasRenderTargetUpdate.AddDynamic(this, &AOutlawsPlayer::DrawMapCanvas);
	}
	else if (MapCanvas->SizeX != RW || MapCanvas->SizeY != RH)
	{
		MapCanvas->ResizeTarget(RW, RH);
	}
	if (!MapPaper)
	{
		// a soft round spot (white, its alpha falling off to the rim) for the paper's stains
		TArray<FColor> Px;
		Px.SetNum(64 * 64);
		for (int32 y = 0; y < 64; ++y)
		{
			for (int32 x = 0; x < 64; ++x)
			{
				const double R = FVector2D(x - 31.5, y - 31.5).Size() / 31.5;
				Px[y * 64 + x] = FColor(255, 255, 255, uint8(FMath::Clamp(255.0 * FMath::Square(FMath::Max(0.0, 1.0 - R)), 0.0, 255.0)));
			}
		}
		MapPaper = OutlawsTextures::MakeTextureBGRA(64, 64, Px, false, TF_Bilinear, TA_Clamp);
	}
	MapCanvas->UpdateResource();
	MapDrawnFor = L;
	MapDrawnLevel = Lv;
	MapDrawnSectorCount = Lv->Sectors.Num();
	UE_LOG(LogOutlaws, Log, TEXT("OLRT map drawing: layer %d, %d of %d sectors joined (%d too big to size it by), %.0f x %.0f m, page %.0f x %.0f cm"), MapLayer, MapSectors.Num(), Lv->Sectors.Num(), LeftOut,
		Span.Y / 100.0, Span.X / 100.0, MapPageSize.X, MapPageSize.Y);
	if (bCarried)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT map drawing: carried by its ground: %d moving sectors and %d of the sky left off, the cars %.1f m long fill %.0f%% of the page's %.1f cm height (scale %.5f; too big from %.0f units)"), Ground, LeftOut,
			Span.X / 100.0, Span.X * MapScale / MapPageSize.Y * 100.0, MapPageSize.Y, MapScale, Big);
	}
	return true;
}

void AOutlawsPlayer::BuildMapPage(int32 Hand)
{
	if (!ToolRoot[Hand] || !BuildMapDrawing()) return;
	// the page: torn all round (a ragged edge, two bites out of it), held up out of the fist by its bottom edge, its top
	// leaning away a little; the front (toward the holder, -X) shows the drawing, the back plain paper
	UProceduralMeshComponent* Pg = GearMesh[Hand];
	if (!IsValid(Pg))
	{
		Pg = NewObject<UProceduralMeshComponent>(this);
		Pg->bUseAsyncCooking = false;
		Pg->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Pg->SetCastShadow(false);
		Pg->SetupAttachment(ToolRoot[Hand]);
		Pg->RegisterComponent();
		Pg->SetBoundsScale(40.f);
		Pg->SetRenderCustomDepth(true);
		Pg->SetCustomDepthStencilValue(1);
		GearMesh[Hand] = Pg;
	}
	// pinched by its bottom corner between the thumb and the forefinger on the top of the fist (the right hand its bottom
	// right, the left its bottom left; user 2026-10-01), standing up, its drawing turned halfway between the holder's face
	// and the other palm (user 2026-10-01: with the palms held facing each other it faced straight back at the face; laid
	// flat in the palm it was too far over): the page reaching up, forward and in toward the other hand.  In the hand
	// frame the holder is back (-X), the other palm out S x Y (PalmDir); the drawing faces the page's -X
	{
		// (user 2026-10-03: "held at a weird angle") turned a quarter of the way to the other palm, not half, and its top
		// leaning back from the face a little, as a map is read
		const double S = Hand == 1 ? -1.0 : 1.0;
		const double In = FMath::Tan(FMath::DegreesToRadians(25.0));
		const FVector Facing = FVector(-1.0, S * In, 0.0).GetSafeNormal();    // the drawing's way
		const FVector Along = FVector(In, S, 0.0).GetSafeNormal();            // the page from the pinch, across
		const FQuat Upright = FRotationMatrix::MakeFromXZ(-Facing, FVector::UpVector).ToQuat();
		const FQuat R = Upright * FQuat(FVector::RightVector, FMath::DegreesToRadians(15.0));
		const FVector Pinch(7.0, S * 1.0, 4.0);
		const FVector Centre = Pinch + Along * (MapPageSize.X * 0.45) + FVector::UpVector * (MapPageSize.Y * 0.45);
		Pg->SetRelativeLocationAndRotation(Centre, R.Rotator());
		MapPinchHand[Hand] = Pinch;
		MapPinchPage[Hand] = R.Inverse().RotateVector(Pinch - Centre);
		MapAnchorRel[Hand] = R;
		bMapTwoHand[Hand] = false;
	}
	if (UMaterialInterface* Base = HeldBase())
	{
		UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, this);
		Mid->SetTextureParameterValue(TEXT("Tex"), MapCanvas);
		KeepAlive.Add(Mid);
		HeldMids.Add(Mid);                    // (lit as the hands are: dim in the dark, the lantern lights it)
		HeldLight = -1.f;
		Pg->SetMaterial(0, Mid);
	}
	Pg->SetMaterial(1, HeldSolidMaterial(FColor(196, 178, 142)));
	const double W = MapPageSize.X, Hh = MapPageSize.Y;
	FRandomStream Rng(1997);
	TArray<FVector2D> Ring;
	{
		const double Step = 0.9;
		const FVector2D Corners[4] = { FVector2D(-W * 0.5, -Hh * 0.5), FVector2D(W * 0.5, -Hh * 0.5), FVector2D(W * 0.5, Hh * 0.5), FVector2D(-W * 0.5, Hh * 0.5) };
		// (torn a little more: user 2026-10-01, the menu's parchment look)
		const int32 BiteSide[3] = { Rng.RandRange(0, 3), Rng.RandRange(0, 3), Rng.RandRange(0, 3) };
		const double BiteAt[3] = { Rng.FRandRange(0.2f, 0.8f), Rng.FRandRange(0.2f, 0.8f), Rng.FRandRange(0.15f, 0.85f) };
		for (int32 e = 0; e < 4; ++e)
		{
			const FVector2D A2 = Corners[e], B2 = Corners[(e + 1) % 4];
			const FVector2D In = FVector2D(-(B2 - A2).Y, (B2 - A2).X).GetSafeNormal();     // (inward: the ring runs anticlockwise)
			const int32 N = FMath::Max(2, FMath::RoundToInt((B2 - A2).Size() / Step));
			for (int32 k = 0; k < N; ++k)
			{
				const double F = double(k) / N;
				double Inset = Rng.FRandRange(0.f, 0.85f);
				for (int32 b = 0; b < 3; ++b)
				{
					if (BiteSide[b] != e) continue;
					const double D = FMath::Abs(F - BiteAt[b]) * (B2 - A2).Size();
					if (D < 3.6) Inset += (1.0 - D / 3.6) * 2.6 + Rng.FRandRange(0.f, 0.5f);
				}
				Ring.Add(FMath::Lerp(A2, B2, F) + In * Inset);
			}
		}
	}
	TArray<FVector> P0, P1;
	TArray<int32> T0, T1;
	TArray<FVector2D> UV0;
	auto UVOf = [&](const FVector2D& Q) { return FVector2D(Q.X / W + 0.5, 0.5 - Q.Y / Hh); };
	for (int32 k = 0; k < Ring.Num(); ++k)
	{
		const FVector2D& A2 = Ring[k];
		const FVector2D& B2 = Ring[(k + 1) % Ring.Num()];
		AddTri(P0, T0, FVector(-0.03, 0.0, 0.0), FVector(-0.03, A2.X, A2.Y), FVector(-0.03, B2.X, B2.Y), true);
		UV0.Add(UVOf(FVector2D::ZeroVector)); UV0.Add(UVOf(A2)); UV0.Add(UVOf(B2));
		AddTri(P1, T1, FVector(0.03, 0.0, 0.0), FVector(0.03, A2.X, A2.Y), FVector(0.03, B2.X, B2.Y), true);
	}
	TArray<FVector> N0, N1;
	N0.Init(FVector::BackwardVector, P0.Num());
	N1.Init(FVector::ForwardVector, P1.Num());
	TArray<FVector2D> UV1;
	UV1.Init(FVector2D(0.5, 0.5), P1.Num());
	TArray<FLinearColor> C0, C1;
	C0.Init(FLinearColor::White, P0.Num());
	C1.Init(FLinearColor::White, P1.Num());
	Pg->ClearAllMeshSections(); OutlawsInk::Forget(Pg);
	Pg->CreateMeshSection_LinearColor(0, P0, T0, N0, UV0, C0, TArray<FProcMeshTangent>(), false);
	Pg->CreateMeshSection_LinearColor(1, P1, T1, N1, UV1, C1, TArray<FProcMeshTangent>(), false);
	// the player's arrow, just off the page on the drawing's side
	UProceduralMeshComponent* Ar = MapArrow[Hand];
	if (!IsValid(Ar))
	{
		Ar = NewObject<UProceduralMeshComponent>(this);
		Ar->bUseAsyncCooking = false;
		Ar->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Ar->SetCastShadow(false);
		Ar->SetupAttachment(Pg);
		Ar->RegisterComponent();
		Ar->SetBoundsScale(40.f);
		Ar->SetMaterial(0, HeldSolidMaterial(FColor(150, 24, 18)));
		MapArrow[Hand] = Ar;
		TArray<FVector> AP = { FVector(-MapMarkLift, 0.0, 0.8), FVector(-MapMarkLift, -0.5, -0.5), FVector(-MapMarkLift, 0.5, -0.5) };
		TArray<int32> AT = { 0, 1, 2, 0, 2, 1 };
		TArray<FVector> AN;
		AN.Init(FVector::BackwardVector, 3);
		TArray<FVector2D> AU;
		AU.Init(FVector2D(0.5, 0.5), 3);
		TArray<FLinearColor> AC;
		AC.Init(FLinearColor::White, 3);
		Ar->CreateMeshSection_LinearColor(0, AP, AT, AN, AU, AC, TArray<FProcMeshTangent>(), false);
	}
	PlaceMapArrow(Hand);
	// the co-op partner's marker (user 2026-10-03): a diamond, made once; placed with the arrow (N1: one for each other player)
	for (int32 k = 0; k < MapMarks; ++k)
	{
		if (IsValid(MapPartner[Hand * MapMarks + k])) continue;
		UProceduralMeshComponent* Pm = NewObject<UProceduralMeshComponent>(this);
		Pm->bUseAsyncCooking = false;
		Pm->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Pm->SetCastShadow(false);
		Pm->SetupAttachment(Pg);
		Pm->RegisterComponent();
		Pm->SetBoundsScale(40.f);
		Pm->SetMaterial(0, HeldSolidMaterial(FColor(36, 70, 160)));
		TArray<FVector> PP = { FVector(-MapMarkLift, 0.0, 1.0), FVector(-MapMarkLift, 1.0, 0.0), FVector(-MapMarkLift, 0.0, -1.0), FVector(-MapMarkLift, -1.0, 0.0) };
		TArray<int32> PT = { 0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2 };
		TArray<FVector> PN;
		PN.Init(FVector::BackwardVector, 4);
		TArray<FVector2D> PU;
		PU.Init(FVector2D(0.5, 0.5), 4);
		TArray<FLinearColor> PC;
		PC.Init(FLinearColor::White, 4);
		Pm->CreateMeshSection_LinearColor(0, PP, PT, PN, PU, PC, TArray<FProcMeshTangent>(), true);
		MapPartner[Hand * MapMarks + k] = Pm;
		UTextRenderComponent* Tx = NewObject<UTextRenderComponent>(this);
		Tx->SetHorizontalAlignment(EHTA_Left);
		Tx->SetVerticalAlignment(EVRTA_TextCenter);
		Tx->SetWorldSize(1.6f);
		Tx->SetTextRenderColor(FColor(20, 14, 8));
		if (UMaterialInterface* TM = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_Text.M_OLR_Text"))) Tx->SetTextMaterial(TM);
		Tx->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Tx->SetCastShadow(false);
		Tx->SetupAttachment(Pg);
		Tx->SetRelativeRotation(FRotator(0.0, 180.0, 0.0));        // (the drawing faces the page's -X)
		Tx->RegisterComponent();
		MapPartnerName[Hand * MapMarks + k] = Tx;
	}
	PlaceMapPartner(Hand);
	Pg->SetVisibility(ToolRoot[Hand]->IsVisible(), true);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT map page in the %s hand"), Hand == 0 ? TEXT("left") : TEXT("right"));
}

void AOutlawsPlayer::PlaceMapPartner(int32 Hand)
{
	for (int32 k = 0; k < MapMarks; ++k) PlaceMapPartnerSlot(Hand, k);
}

void AOutlawsPlayer::PlaceMapPartnerSlot(int32 Hand, int32 Slot)
{
	UProceduralMeshComponent* Pm = MapPartner[Hand * MapMarks + Slot];
	UTextRenderComponent* Tx = MapPartnerName[Hand * MapMarks + Slot];
	if (!IsValid(Pm) || !IsValid(Tx)) return;
	AOutlawsLevelActor* L = Level.Get();
	const FOutlawsLevel* Lv = L ? L->LiveLevel() : nullptr;
	FVector Feet;
	FString Name;
	if (!Lv || !UOutlawsCoop::PartnerForMap(this, Feet, Name, Slot))
	{
		Pm->SetVisibility(false);
		Tx->SetVisibility(false);
		return;
	}
	// where on the page (as the arrow: north up, east across), and whether that is on the drawing: a sector of the page's,
	// on its layer
	const FVector2D Q((Feet.Y - MapCentre.Y) * MapScale, (Feet.X - MapCentre.X) * MapScale);
	const int32 Si = L->CullSectorOf(Feet + FVector(0.0, 0.0, 10.0));
	const FOutlawsSector* S = Lv->Sector(Si);
	const FVector2D Half = MapPageSize * 0.5 * 0.9;
	const bool bOn = S && MapSectors.Contains(Si) && S->Layer == MapLayer && FMath::Abs(Q.X) <= Half.X && FMath::Abs(Q.Y) <= Half.Y;
	FVector2D At = Q, Dir(0.0, 1.0);
	if (!bOn)
	{
		// off the drawing: at the page's edge on the way to them, pointing there
		const FVector2D Away = Q.IsNearlyZero() ? FVector2D(0.0, 1.0) : Q.GetSafeNormal();
		const double T = FMath::Min(Half.X / FMath::Max(1e-6, FMath::Abs(Away.X)), Half.Y / FMath::Max(1e-6, FMath::Abs(Away.Y)));
		At = Away * T;
		Dir = Away;
	}
	TArray<FVector> PP;
	if (bOn)
	{
		const double R = 1.0;
		PP = { FVector(-MapMarkLift, At.X, At.Y + R), FVector(-MapMarkLift, At.X + R, At.Y), FVector(-MapMarkLift, At.X, At.Y - R), FVector(-MapMarkLift, At.X - R, At.Y) };
	}
	else
	{
		const FVector2D Sd(Dir.Y, -Dir.X);
		const FVector2D Tip = At, B1 = At - Dir * 1.8 + Sd * 0.9, B2 = At - Dir * 1.8 - Sd * 0.9;
		PP = { FVector(-MapMarkLift, Tip.X, Tip.Y), FVector(-MapMarkLift, B1.X, B1.Y), FVector(-MapMarkLift, At.X - Dir.X * 1.2, At.Y - Dir.Y * 1.2), FVector(-MapMarkLift, B2.X, B2.Y) };
	}
	TArray<FVector> PN;
	PN.Init(FVector::BackwardVector, 4);
	TArray<FVector2D> PU;
	PU.Init(FVector2D(0.5, 0.5), 4);
	TArray<FLinearColor> PC;
	PC.Init(FLinearColor::White, 4);
	Pm->UpdateMeshSection_LinearColor(0, PP, PN, PU, PC, TArray<FProcMeshTangent>());
	Pm->SetVisibility(true);
	Tx->SetText(FText::FromString(Name));
	Tx->SetRelativeLocation(FVector(-MapMarkLift - 0.05, At.X + (bOn ? 1.5 : -Dir.X * 3.0), At.Y + (bOn ? 0.0 : -Dir.Y * 3.0)));
	Tx->SetHorizontalAlignment(bOn ? EHTA_Left : EHTA_Center);
	Tx->SetVisibility(!Name.IsEmpty());
	const FString State = bOn ? TEXT("on the page") : TEXT("off the drawing: an arrow at the edge");
	if (Slot == 0) MapPartnerLogged = State;
	if (State != MapOthersLogged[Slot])
	{
		MapOthersLogged[Slot] = State;
		if (Slot == 0)
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLRT map: the partner %s (page %.1f, %.1f; sector %d)"), *State, Q.X, Q.Y, Si);
		}
		else
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLRT map: mark %d of the others ('%s') %s (page %.1f, %.1f; sector %d)"), Slot + 1, *Name, *State, Q.X, Q.Y, Si);
		}
	}
}

void AOutlawsPlayer::PlaceMapArrow(int32 Hand)
{
	UProceduralMeshComponent* Ar = MapArrow[Hand];
	if (!IsValid(Ar)) return;
	const FVector Eye = Camera->GetComponentLocation();
	// the heading: the view's forward flattened, helped by its up when looking down at the page (the rotator's yaw spins
	// there: user 2026-10-03, the arrow "rotated weirdly")
	const FVector Fw = Camera->GetForwardVector(), Up = Camera->GetUpVector();
	FVector2D H(Fw.X - Up.X * Fw.Z, Fw.Y - Up.Y * Fw.Z);
	H = H.IsNearlyZero() ? FVector2D(1.0, 0.0) : H.GetSafeNormal();
	FVector2D Q((Eye.Y - MapCentre.Y) * MapScale, (Eye.X - MapCentre.X) * MapScale);
	// (off the drawing: held at the page's edge rather than hanging in the air beside it)
	const FVector2D HalfPg = MapPageSize * 0.5 * 0.9;
	Q = FVector2D(FMath::Clamp(Q.X, -HalfPg.X, HalfPg.X), FMath::Clamp(Q.Y, -HalfPg.Y, HalfPg.Y));
	// the triangle's tip along the heading: north up the page (+Z), east across it (+Y)
	const FVector2D D(H.Y, H.X), S(D.Y, -D.X);
	const double Len = 2.6, Back = 1.2, Wide = 1.3;
	TArray<FVector> AP = {
		FVector(-MapMarkLift, Q.X + D.X * Len, Q.Y + D.Y * Len),
		FVector(-MapMarkLift, Q.X - D.X * Back + S.X * Wide, Q.Y - D.Y * Back + S.Y * Wide),
		FVector(-MapMarkLift, Q.X - D.X * Back - S.X * Wide, Q.Y - D.Y * Back - S.Y * Wide) };
	TArray<FVector> AN;
	AN.Init(FVector::BackwardVector, 3);
	TArray<FVector2D> AU;
	AU.Init(FVector2D(0.5, 0.5), 3);
	TArray<FLinearColor> AC;
	AC.Init(FLinearColor::White, 3);
	Ar->UpdateMeshSection_LinearColor(0, AP, AN, AU, AC, TArray<FProcMeshTangent>());
	static double NextArrowLog = 0.0;
	if (OLFlags::Param(TEXT("OLGearTest")) && FPlatformTime::Seconds() > NextArrowLog)
	{
		NextArrowLog = FPlatformTime::Seconds() + 1.0;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT map arrow at page (%.2f, %.2f) of %.0f x %.0f; eye %s, centre %s; visible %d, extent %s, material %s"), Q.X, Q.Y, MapPageSize.X, MapPageSize.Y,
			*Eye.ToString(), *MapCentre.ToString(), Ar->IsVisible() ? 1 : 0, *Ar->Bounds.BoxExtent.ToString(), Ar->GetMaterial(0) ? *Ar->GetMaterial(0)->GetName() : TEXT("none"));
	}
}

void AOutlawsPlayer::DrawMapCanvas(UCanvas* Canvas, int32 Width, int32 Height)
{
	AOutlawsLevelActor* L = Level.Get();
	const FOutlawsLevel* Lv = L ? L->LiveLevel() : nullptr;
	if (!Canvas) return;
	// the paper: plain tan, aged with soft stains and darker toward its edges (MapPaper: a soft round spot)
	{
		// a bright parchment of its own (user 2026-10-01: the menu's sheet showed its icons and the logo, and it was dark
		// and orange): pale cream, mottled below
		FCanvasTileItem I(FVector2D(0.f, 0.f), GWhiteTexture, FVector2D(Width, Height), FLinearColor(0.92f, 0.86f, 0.70f, 1.f));
		Canvas->DrawItem(I);         // (the canvas's own blend: an opaque tile drew white, its colour dropped)
	}
	if (MapPaper && MapPaper->GetResource())
	{
		FRandomStream Rng(611);
		auto Spot = [&](const FVector2D& At, double Size, const FLinearColor& C)
		{
			FCanvasTileItem I(At - FVector2D(Size, Size) * 0.5, MapPaper->GetResource(), FVector2D(Size, Size), C);
			I.BlendMode = SE_BLEND_Translucent;
			Canvas->DrawItem(I);
		};
		// mottled as old parchment is, lightly: broad faint tan clouds, small darker flecks, paler patches
		for (int32 k = 0; k < 60; ++k)
		{
			Spot(FVector2D(Rng.FRandRange(0.f, Width), Rng.FRandRange(0.f, Height)), Rng.FRandRange(0.1f, 0.4f) * Width,
				Rng.FRandRange(0.f, 1.f) < 0.65f ? FLinearColor(0.62f, 0.5f, 0.32f, Rng.FRandRange(0.03f, 0.08f)) : FLinearColor(1.f, 0.97f, 0.88f, Rng.FRandRange(0.06f, 0.14f)));
		}
		for (int32 k = 0; k < 90; ++k)
		{
			Spot(FVector2D(Rng.FRandRange(0.f, Width), Rng.FRandRange(0.f, Height)), Rng.FRandRange(0.008f, 0.03f) * Width,
				FLinearColor(0.5f, 0.38f, 0.22f, Rng.FRandRange(0.06f, 0.16f)));
		}
		const int32 Around = 36;
		for (int32 k = 0; k < Around; ++k)
		{
			const double F = double(k) / Around;
			const double S = Width * Rng.FRandRange(0.12f, 0.2f);
			const FLinearColor Burn(0.48f, 0.34f, 0.18f, Rng.FRandRange(0.1f, 0.18f));      // (the edges a little browned)
			Spot(FVector2D(F * Width, 0.0), S, Burn);
			Spot(FVector2D(F * Width, Height), S, Burn);
			Spot(FVector2D(0.0, F * Height), S, Burn);
			Spot(FVector2D(Width, F * Height), S, Burn);
		}
	}
	if (!Lv) return;
	const double K = Width / MapPageSize.X;      // pixels a page cm
	// the hint blobs (user 2026-10-01: key hunting): a faint red patch about 10 m across round each item the level's locks
	// still need, lying somewhere in it and not at its middle (off by up to 3 m, the same each time); under the walls
	if (L && MapPaper && MapPaper->GetResource())
	{
		TArray<FVector> Spots;
		L->KeyItemSpots(Spots);
		for (const FVector& W : Spots)
		{
			FRandomStream Rs(int32(GetTypeHash(FIntVector(W / 50.0))));
			const FVector2D Off = FVector2D(Rs.FRandRange(-1.f, 1.f), Rs.FRandRange(-1.f, 1.f)).GetSafeNormal() * Rs.FRandRange(0.f, 300.f);
			const double WX = W.X + Off.X, WY = W.Y + Off.Y;
			const FVector2D At(Width * 0.5 + (WY - MapCentre.Y) * MapScale * K, Height * 0.5 - (WX - MapCentre.X) * MapScale * K);
			const double Rpx = 500.0 * MapScale * K;
			for (const FVector2D& Pass : { FVector2D(2.6, 0.16), FVector2D(1.5, 0.14) })
			{
				const double Sz = FMath::Max(12.0, Rpx * Pass.X);
				FCanvasTileItem I(At - FVector2D(Sz, Sz) * 0.5, MapPaper->GetResource(), FVector2D(Sz, Sz), FLinearColor(0.78f, 0.06f, 0.03f, float(Pass.Y)));
				I.BlendMode = SE_BLEND_Translucent;
				Canvas->DrawItem(I);
			}
		}
	}
	auto ToPx = [&](const FVector2D& V)            // a sector vertex (x, z) in Outlaws units
	{
		const double WX = V.Y * FOutlawsGeometry::UnitsToCm, WY = V.X * FOutlawsGeometry::UnitsToCm;
		return FVector2D(Width * 0.5 + (WY - MapCentre.Y) * MapScale * K, Height * 0.5 - (WX - MapCentre.X) * MapScale * K);
	};
	// the walls: the solid ones, and openings onto a floor of another height (a ledge, a drop); the other layers first,
	// half as dark, then the one underfoot over them (user 2026-10-01)
	for (const bool bHere : { false, true })
	{
		const FLinearColor Solid = bHere ? FLinearColor(0.02f, 0.015f, 0.01f, 1.f) : FLinearColor(0.02f, 0.015f, 0.01f, 0.5f);
		const FLinearColor Step = bHere ? FLinearColor(0.1f, 0.07f, 0.05f, 0.85f) : FLinearColor(0.1f, 0.07f, 0.05f, 0.4f);
		for (const int32 Si : MapSectors)
		{
			const FOutlawsSector& S = Lv->Sectors[Si];
			if ((S.Layer == MapLayer) != bHere) continue;
			for (const FOutlawsWall& Wl : S.Walls)
			{
				if (!S.Verts.IsValidIndex(Wl.V1) || !S.Verts.IsValidIndex(Wl.V2)) continue;
				bool bSolid = Wl.Adjoin < 0;
				if (!bSolid)
				{
					const FOutlawsSector* O = Lv->Sector(Wl.Adjoin);
					if (O && O->Layer == S.Layer && FMath::Abs(O->Floor.Y - S.Floor.Y) <= 2.0) continue;
				}
				Canvas->K2_DrawLine(ToPx(S.Verts[Wl.V1]), ToPx(S.Verts[Wl.V2]), (bSolid ? 4.5f : 2.2f) * float(K / 36.0), bSolid ? Solid : Step);
			}
		}
	}
	DrawMapItems(Canvas, Width, Height, K);        // (REQ-MAPITEMS: the seen items over the walls, OutlawsMapItems.cpp)
}

// ---- every frame ----------------------------------------------------------------------------------------------------

void AOutlawsPlayer::UpdateGearCommon(float Dt)
{
	UpdateChestPlate(Dt);               // (REQ-CHESTPLATE: the plate worn; armour takes its share of every hit, as in the original)
	AOutlawsLevelActor* L = Level.Get();
	// (REQ-MAPITEMS) what this player's eyes find lying about goes on their map
	if (L && Camera && !bDeadView) L->UpdateMapSeen(Camera->GetComponentLocation(), Camera->GetForwardVector(), Dt);
	if (!L || (!bVR && !bTestLampFollowsHand)) return;
	// the lantern burns while one is held and there is oil (picked up later, it lights again by itself)
	const bool bHeld = HeldTool[0] == TEXT("ilantern") || HeldTool[1] == TEXT("ilantern");
	const bool bWant = bHeld && L->GetCount(TEXT("ioil")) > 0 && !bDeadView;
	if (bWant != L->bLampOn)
	{
		if (bWant) L->ToggleLamp();
		else L->bLampOn = false;
	}
}

void AOutlawsPlayer::UpdateGear(int32 Hand, float Dt)
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L || !ToolRoot[Hand]) return;
	const FString& Item = HeldTool[Hand];
	if (Item == TEXT("imap"))
	{
		const FOutlawsLevel* Lv = L->LiveLevel();
		const FOutlawsSector* Here = Lv ? Lv->Sector(L->PlayerSectorIndex()) : nullptr;
		if (Here && Here->Layer != MapLayer) BuildMapPage(Hand);       // (another layer underfoot: drawn again)
		// held at the hand's own angle, pinned at the corner the fingers hold (user 2026-10-03: the eased turn toward the
		// eyes felt floaty; "fix it to the hand orientation again")
		if (UProceduralMeshComponent* Pg = GearMesh[Hand])
		{
			const FTransform Root = ToolRoot[Hand]->GetComponentTransform();
			const FVector PinchW = Root.TransformPosition(MapPinchHand[Hand]);
			FQuat Anchor = Root.GetRotation() * MapAnchorRel[Hand];
			// the other hand on the far bottom corner (user 2026-10-03: "hold the other side with my other hand ... if I use the
			// grip button on that side"): a fresh grip within 18 cm of it takes it, free of anything else; then the page's
			// bottom edge runs from hand to hand and its drawing faces the eyes, until that grip lets go
			const int32 O = 1 - Hand;
			const bool bGripO = Axis(O == 0 ? TEXT("OLVR_LeftGrip") : TEXT("OLVR_RightGrip")) > (bMapTwoHand[Hand] ? 0.3f : 0.5f);
			const bool bOFree = HeldTool[O].IsEmpty() && !GrabbedEnemy[O].IsValid() && !HeldItem[O].IsValid() && !HeldBottle[O];
			const FVector FarPage(MapPinchPage[Hand].X, -MapPinchPage[Hand].Y, MapPinchPage[Hand].Z);
			const FVector FarW = PinchW - Anchor.RotateVector(MapPinchPage[Hand]) + Anchor.RotateVector(FarPage);
			const FVector OW = GripOf(O)->GetComponentLocation();
			if (!bGripO || !bOFree) bMapTwoHand[Hand] = false;
			else if (!bMapOtherGrip[Hand] && FVector::Dist(OW, FarW) < 18.0)
			{
				bMapTwoHand[Hand] = true;
				Pulse(O, 0.3f);
				UE_LOG(LogOutlaws, Log, TEXT("OLRT map: held in both hands"));
			}
			bMapOtherGrip[Hand] = bGripO;
			if (bMapTwoHand[Hand])
			{
				FVector Y = PinchW - OW;                                   // (the page's +Y runs from the far corner to the pinch)
				if (MapPinchPage[Hand].Y < 0.0) Y = -Y;
				const FVector Mid = (PinchW + OW) * 0.5;
				FVector X = Mid - Camera->GetComponentLocation();          // (the drawing, the page's -X, to the eyes)
				if (Y.SizeSquared() > 25.0)
				{
					Y = Y.GetSafeNormal();
					X = (X - Y * FVector::DotProduct(X, Y)).GetSafeNormal();
					if (!X.IsNearlyZero()) Anchor = FRotationMatrix::MakeFromXY(X, Y).ToQuat();
				}
			}
			Pg->SetWorldLocationAndRotation(PinchW - Anchor.RotateVector(MapPinchPage[Hand]), Anchor);
		}
		// a lock item taken (or dropped): its blob off the page; an item seen (REQ-MAPITEMS): on it
		RefreshMapMarks(Dt);
		PlaceMapArrow(Hand);
		PlaceMapPartner(Hand);
		return;
	}
	if (Item == TEXT("ilantern"))
	{
		USceneComponent* Hang = LanternHang[Hand];
		if (!IsValid(Hang)) return;
		// the fist on the other hand's long gun (user 2026-10-02): the lantern hangs from under it, clear of the forend
		if (bSupport[Hand] != bLanternLow[Hand])
		{
			bLanternLow[Hand] = bSupport[Hand];
			if (!bLanternLow[Hand]) ToolRoot[Hand]->SetRelativeLocation(ToolRest(Hand).GetLocation());
		}
		if (bLanternLow[Hand])
		{
			if (const UOutlawsBoxHand* Fist = Hand == 0 ? LeftHand.Get() : RightHand.Get()) ToolRoot[Hand]->SetWorldLocation(Fist->GetComponentLocation() + FVector::DownVector * 7.0);     // (the fist as drawn on the forend)
		}
		// it hangs from the fist and swings: a weight on a 12 cm cord under gravity, worked out in the play space's own
		// frame (user 2026-10-01: a snap turn flung it), damped harder than it was, and never more than 35 degrees off
		// plumb (it flailed walking about).  The walk is felt only as a change of pace, and that eased in over a third of
		// a second, at a third of its strength (user 2026-10-01: setting off still kicked it: a steady walk had been
		// pushing it as hard as starting one)
		const FTransform Space = VROrigin->GetComponentTransform();
		const FVector OriginW = VROrigin->GetComponentLocation();
		const FVector Top = Space.InverseTransformPosition(ToolRoot[Hand]->GetComponentLocation());
		const FVector Plumb = Space.InverseTransformVectorNoScale(FVector::DownVector).GetSafeNormal();
		const float Yaw = float(GetActorRotation().Yaw);
		if (!bLanternBob[Hand] || FVector::Dist(LanternBob[Hand], Top) > LanternDrop * 3.0)
		{
			LanternBob[Hand] = LanternBobPrev[Hand] = Top + Plumb * LanternDrop;
			LanternOriginPrev[Hand] = OriginW;
			LanternMoveVel[Hand] = FVector::ZeroVector;
			LanternYawPrev[Hand] = Yaw;
			bLanternBob[Hand] = true;
		}
		const float Step = FMath::Min(Dt, 0.05f);
		// the space's pace through the world this frame; a teleport's or a snap turn's jump is no pace (the eased one held)
		const FVector MovedW = OriginW - LanternOriginPrev[Hand];
		LanternOriginPrev[Hand] = OriginW;
		const bool bJump = MovedW.Size() > 8.0 || FMath::Abs(FMath::FindDeltaAngleDegrees(LanternYawPrev[Hand], Yaw)) > 5.f;
		LanternYawPrev[Hand] = Yaw;
		const FVector PaceW = bJump || Dt <= 0.f ? LanternMoveVel[Hand] : MovedW / Dt;
		const FVector PaceWas = LanternMoveVel[Hand];
		LanternMoveVel[Hand] = FMath::Lerp(PaceWas, PaceW, 1.f - FMath::Exp(-Dt / 0.33f));
		// its change this frame, a third of it, pushes the weight back (in the space's frame: a step of it a frame)
		const FVector Kick = Space.InverseTransformVectorNoScale(LanternMoveVel[Hand] - PaceWas) * (Step * 0.33);
		const FVector Vel = (LanternBob[Hand] - LanternBobPrev[Hand]).GetClampedToMaxSize(6.0) * FMath::Max(0.f, 1.f - 5.f * Step);
		LanternBobPrev[Hand] = LanternBob[Hand];
		LanternBob[Hand] += Vel - Kick + Plumb * (980.0 * Step * Step);
		FVector Down = (LanternBob[Hand] - Top).GetSafeNormal();
		if (Down.IsNearlyZero()) Down = Plumb;
		constexpr double MaxSwing = 35.0;
		const double Off = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(Down, Plumb), -1.0, 1.0)));
		if (Off > MaxSwing)
		{
			Down = FQuat::Slerp(FQuat::Identity, FQuat::FindBetweenNormals(Plumb, Down), MaxSwing / Off).RotateVector(Plumb);
		}
		LanternBob[Hand] = Top + Down * LanternDrop;
		if (Off > MaxSwing) LanternBobPrev[Hand] = LanternBob[Hand] - (LanternBob[Hand] - LanternBobPrev[Hand]) * 0.3;   // (the stop takes its speed)
		const FVector Fwd = FrameOf(Hand)->GetForwardVector();
		Hang->SetWorldRotation(FRotationMatrix::MakeFromZX(-Space.TransformVectorNoScale(Down), Fwd).ToQuat());
		// lit: the glass glows and the flame flickers; the level's lantern light comes from the flame
		const bool bLit = L->bLampOn;
		const float T = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
		const float Flick = 1.f + 0.07f * FMath::Sin(T * 7.3f) + 0.05f * FMath::Sin(T * 17.9f + 1.7f) + 0.06f * FMath::PerlinNoise1D(T * 5.f);
		const float Oil = FMath::Clamp(L->GetCount(TEXT("ioil")) / 150.f, 0.f, 1.f);
		if (UMaterialInstanceDynamic* G = LanternGlassMid[Hand])
		{
			G->SetScalarParameterValue(TEXT("Glow"), bLit ? FMath::Lerp(0.3f, 0.65f, FMath::Sqrt(Oil)) * Flick : 0.f);       // (the sleeve)
			G->SetScalarParameterValue(TEXT("Brightness"), bLit ? FMath::Lerp(0.85f, 1.25f, FMath::Sqrt(Oil)) * Flick : FMath::Max(0.04f, HeldLight));   // (the glass)
		}
		if (UStaticMeshComponent* F = LanternFlame[Hand])
		{
			F->SetVisibility(bLit && ToolRoot[Hand]->IsVisible());
			UMaterialInstanceDynamic* Fm = Cast<UMaterialInstanceDynamic>(F->GetMaterial(0));
			if (LanternFlameH[Hand] > 0.f && LanternBody[Hand] && Fm && FireCells.Num() > 0)
			{
				// the fire sprite upright and turned to the eyes, its foot where the painted flame's was, its drawings at the
				// sprite's own rate, taller and shorter with the flicker
				const FVector Foot = LanternBody[Hand]->GetComponentTransform().TransformPosition(LanternFlameAt[Hand]);
				const float Hh = LanternFlameH[Hand] * 1.2f * Flick;
				const FVector To = Camera->GetComponentLocation() - Foot;
				F->SetWorldLocationAndRotation(Foot - FVector(0.0, 0.0, 0.1 * Hh), FRotator(0.0, FMath::RadiansToDegrees(FMath::Atan2(To.Y, To.X)), 0.0));
				F->SetWorldScale3D(FVector(1.0, Hh * FireAspect, Hh));
				Fm->SetVectorParameterValue(TEXT("UVOffset"), FireCells[int32(FMath::FloorToFloat(T * FireFps)) % FireCells.Num()]);
			}
			else
			{
				F->SetRelativeScale3D(FVector(0.012, 0.012, 0.024) * Flick);
			}
			if (bLit)
			{
				L->LampAt = F->GetComponentLocation();
				L->LampAtTime = FPlatformTime::Seconds();
			}
		}
		return;
	}
}

// ---- the Sheriff's Badge's gold ---------------------------------------------------------------------------------------

void AOutlawsPlayer::UpdateBadgeVignette(float Dt)
{
	// a gold filter (user 2026-10-01: a colour filter, not solid gold; the whole view a little, the rim most), on a plane
	// 15 cm before the eyes (the headset has no post process): strong and
	// steady for the badge's first 20 seconds; in its last 10 it pulses with the badge's blink (brightest while the
	// badge shows, the second's later half) and each pulse fainter, gone as the badge is (user 2026-10-01)
	AOutlawsLevelActor* L = Level.Get();
	const float Left = L ? L->BadgeLeft : 0.f;
	const bool bOn = Left > 0.f && !bDeadView && !bMovieHidden;
	BadgeVeilIn = FMath::FInterpConstantTo(BadgeVeilIn, bOn ? 1.f : 0.f, Dt, 3.f);
	if (!BadgeVeil && BadgeVeilIn <= 0.f) return;
	if (!BadgeVeil)
	{
		UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_Vignette.M_OLR_Vignette"));
		if (!Base) return;
		BadgeVeil = NewObject<UStaticMeshComponent>(this);
		BadgeVeil->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane")));
		BadgeVeil->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		BadgeVeil->SetCastShadow(false);
		BadgeVeil->SetupAttachment(Camera);
		BadgeVeil->RegisterComponent();
		// a 60 cm square 15 cm out: past the edge of the headset's view for either eye (about 55 degrees each way)
		BadgeVeil->SetRelativeLocationAndRotation(FVector(15.0, 0.0, 0.0), FRotator(90.0, 0.0, 0.0));
		BadgeVeil->SetRelativeScale3D(FVector(0.6, 0.6, 1.0));
		BadgeVeil->SetTranslucentSortPriority(-10);       // (under the menu, the wheel and the text)
		BadgeVeil->SetBoundsScale(20.f);
		BadgeVeilMid = UMaterialInstanceDynamic::Create(Base, this);
		BadgeVeilMid->SetVectorParameterValue(TEXT("Color"), FLinearColor(1.3f, 1.f, 0.45f));     // (what is seen times this)
		BadgeVeil->SetMaterial(0, BadgeVeilMid);
	}
	float Amount = 0.8f;
	if (Left <= 10.f)
	{
		const float Pulse = 0.5f + 0.5f * FMath::Cos(UE_TWO_PI * (FMath::Frac(Left) - 0.75f));
		Amount *= Pulse * FMath::Clamp(Left / 10.f, 0.f, 1.f);
	}
	Amount *= BadgeVeilIn;
	BadgeVeilMid->SetScalarParameterValue(TEXT("Amount"), Amount);
	BadgeVeil->SetVisibility(Amount > 0.001f);
}

// ---- the red of a hit, on the headset ----------------------------------------------------------------------------------

void AOutlawsPlayer::UpdateHurtVeil()
{
	// the PC draws it as a post process (M_OLR_HurtDir); the headset has none, so there it rides on a plane 15 cm before
	// the eyes as the badge's gold does (user 2026-10-01).  -OLHurtVeil shows the plane on the PC too
	static const bool bForce = OLFlags::Param(TEXT("OLHurtVeil"));
	const UWorld* World = GetWorld();
	if (!World || !(bForce || World->FeatureLevel <= ERHIFeatureLevel::ES3_1)) return;
	const bool bRound = HurtFlash > HurtDirAmount;       // (all round: drowning, a floor; the flash, not a side)
	const float Amount = bMovieHidden ? 0.f : FMath::Max(HurtDirAmount, HurtFlash);
	if (!HurtVeil && Amount <= 0.f) return;
	if (!HurtVeil)
	{
		UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_HurtVeil.M_OLR_HurtVeil"));
		if (!Base) return;
		HurtVeil = NewObject<UStaticMeshComponent>(this);
		HurtVeil->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane")));
		HurtVeil->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		HurtVeil->SetCastShadow(false);
		HurtVeil->SetupAttachment(Camera);
		HurtVeil->RegisterComponent();
		// as the badge's plane: 60 cm square, 15 cm out, its U across the view and V down it
		HurtVeil->SetRelativeLocationAndRotation(FVector(15.0, 0.0, 0.0), FRotationMatrix::MakeFromXY(FVector(0.0, 1.0, 0.0), FVector(0.0, 0.0, -1.0)).Rotator());
		HurtVeil->SetRelativeScale3D(FVector(0.6, 0.6, 1.0));
		HurtVeil->SetTranslucentSortPriority(-9);        // (over the badge's gold, under the menu, the wheel and the text)
		HurtVeil->SetBoundsScale(20.f);
		HurtVeilMid = UMaterialInstanceDynamic::Create(Base, this);
		HurtVeil->SetMaterial(0, HurtVeilMid);
	}
	HurtVeilMid->SetVectorParameterValue(TEXT("Dir"), bRound ? FLinearColor(0.f, 0.f, 0.f, 0.f) : FLinearColor(HurtDir.X, HurtDir.Y, 0.f, 0.f));
	HurtVeilMid->SetScalarParameterValue(TEXT("Amount"), Amount);
	HurtVeil->SetVisibility(Amount > 0.001f);
}

// ---- the dark of a held breath ------------------------------------------------------------------------------------------

void AOutlawsPlayer::UpdateFadeVeil()
{
	// the headset only (the PC's post process draws the camera's fade itself)
	const UWorld* W = GetWorld();
	if (!W || W->FeatureLevel > ERHIFeatureLevel::ES3_1) return;
	const APlayerController* PC = Cast<APlayerController>(GetController());
	const APlayerCameraManager* Cm = PC ? PC->PlayerCameraManager : nullptr;
	const float Amount = Cm && Cm->bEnableFading ? FMath::Clamp(Cm->FadeAmount, 0.f, 1.f) : 0.f;
	if (!FadeVeil && Amount <= 0.001f) return;
	if (!FadeVeil)
	{
		UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_Vignette.M_OLR_Vignette"));
		if (!Base) return;
		FadeVeil = NewObject<UStaticMeshComponent>(this);
		FadeVeil->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane")));
		FadeVeil->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		FadeVeil->SetCastShadow(false);
		FadeVeil->SetupAttachment(Camera);
		FadeVeil->RegisterComponent();
		// the breath veil's place, a hair nearer the eye; the whole view times black by the fade
		FadeVeil->SetRelativeLocationAndRotation(FVector(15.0, 0.0, 0.0), FRotator(90.0, 0.0, 0.0));
		FadeVeil->SetRelativeScale3D(FVector(0.6, 0.6, 1.0));
		FadeVeil->SetTranslucentSortPriority(-8);
		FadeVeil->SetBoundsScale(20.f);
		FadeVeilMid = UMaterialInstanceDynamic::Create(Base, this);
		FadeVeilMid->SetVectorParameterValue(TEXT("Color"), FLinearColor::Black);
		FadeVeilMid->SetScalarParameterValue(TEXT("Whole"), 1.f);
		FadeVeil->SetMaterial(0, FadeVeilMid);
	}
	FadeVeilMid->SetScalarParameterValue(TEXT("Amount"), Amount);
	FadeVeil->SetVisibility(Amount > 0.001f);
}

void AOutlawsPlayer::UpdatePlayWarm(AOutlawsLevelActor* L)
{
	if (WarmFrames > 3) return;
	if (WarmFrames >= 0)
	{
		// (-OLStutterProbe -OLShots: the fade each warm frame, a picture of the second and of the first one after; a test)
#if WITH_OUTLAWS_TESTS
		static const bool bProbe = OLFlags::Param(TEXT("OLStutterProbe")) && OLFlags::Param(TEXT("OLShots"));
#else
		constexpr bool bProbe = false;
#endif
		if (bProbe)
		{
			const APlayerController* PC = Cast<APlayerController>(GetController());
			const APlayerCameraManager* Cm = PC ? PC->PlayerCameraManager : nullptr;
			UE_LOG(LogOutlaws, Log, TEXT("OLRT probe warm frame %d: fade %.3f"), WarmFrames + 1, Cm && Cm->bEnableFading ? Cm->FadeAmount : 0.f);
			if (WarmFrames == 1 || WarmFrames == 3)
			{
				FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / (WarmFrames == 1 ? TEXT("ol_warm_under.png") : TEXT("ol_warm_after.png")), false, false);
			}
		}
		// (FIRSTHITCH) the second frame of the cards: the scope's picture taken of them too.  The first shot through the scope
		// drew the flash, the tracer, the smoke and the case in the scope's own pass for the first time (55 ms on the desktop)
		if (WarmFrames == 1 && !OldFirstUse() && L && !L->IsFrontEnd() && !L->IsRebuildPending()) WarmScopeEffects(L);
		if (++WarmFrames <= 3) return;
		for (UPrimitiveComponent* C : WarmCards) if (C) C->DestroyComponent();
		WarmCards.Reset();
		return;
	}
	static const bool bSkip = OLFlags::Param(TEXT("OLNoPrebuild"));      // (the before of a measure)
	if (bSkip || !L || L->IsFrontEnd() || L->IsRebuildPending() || !IsLocallyControlled() || !FApp::CanEverRender()) return;
	const double T0 = FPlatformTime::Seconds();
	TArray<UMaterialInterface*> Mats;
	static const TCHAR* Paths[] = { TEXT("/Game/Outlaws/Runtime/M_OLR_FxGlow.M_OLR_FxGlow"), TEXT("/Game/Outlaws/Runtime/M_OLR_FxGlowHeld.M_OLR_FxGlowHeld"),
		TEXT("/Game/Outlaws/Runtime/M_OLR_HeldSprite.M_OLR_HeldSprite"), TEXT("/Game/Outlaws/Runtime/M_OLR_Held.M_OLR_Held"),
		TEXT("/Game/Outlaws/Runtime/M_OLR_Scope.M_OLR_Scope"), TEXT("/Game/Outlaws/Runtime/M_OLR_Soot.M_OLR_Soot"),
		TEXT("/Game/Outlaws/Runtime/M_OLR_ItemModel.M_OLR_ItemModel"), TEXT("/Game/Outlaws/Runtime/M_OLR_FxFlash.M_OLR_FxFlash") };     // (REQ-MUZZLEFLASH3D)
	for (const TCHAR* P : Paths) if (UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, P)) Mats.Add(M);
	// (FIRSTHITCH) and the gun smoke, the blood, clods and glass a shot throws (fading sprites) and a dropped item's ink shell,
	// which the list above left to their first time in play
	static const TCHAR* More[] = { TEXT("/Game/Outlaws/Runtime/M_OLR_FxSmoke.M_OLR_FxSmoke"), TEXT("/Game/Outlaws/Runtime/M_OLR_SpriteFade.M_OLR_SpriteFade"),
		TEXT("/Game/Outlaws/Runtime/M_OLR_ItemInk.M_OLR_ItemInk") };
	if (!OldFirstUse()) for (const TCHAR* P : More) if (UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, P)) Mats.Add(M);
	if (L->SpriteMaterial) Mats.Add(L->SpriteMaterial);      // (the blast and the fuse's fire)
	UStaticMesh* Plane = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	const TArray<FVector> Pos = { FVector(0, -1, -1), FVector(0, 1, -1), FVector(0, 1, 1), FVector(0, -1, 1) };
	const TArray<int32> Tri = { 0, 2, 1, 0, 3, 2 };
	const TArray<FVector> Nrm = { FVector(-1, 0, 0), FVector(-1, 0, 0), FVector(-1, 0, 0), FVector(-1, 0, 0) };
	const TArray<FVector2D> Uv = { FVector2D(0, 1), FVector2D(1, 1), FVector2D(1, 0), FVector2D(0, 0) };
	int32 i = 0;
	for (UMaterialInterface* M : Mats)
	{
		// 0.2 mm cards 4 m out (a hundredth of a pixel: drawn, so the PSO is made, but no pixel lit; the desktop ES3.1 preview showed
		// 2 cm ones through its fade), side by side, under the fade
		const FVector At(400.0, -40.0 + 6.0 * i++, -20.0);
		if (Plane)
		{
			UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this);
			C->SetStaticMesh(Plane);
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->SetCastShadow(false);
			C->SetupAttachment(Camera);
			C->RegisterComponent();
			C->SetRelativeLocationAndRotation(At, FRotator(90.0, 0.0, 0.0));
			C->SetRelativeScale3D(FVector(0.0002, 0.0002, 1.0));
			C->SetMaterial(0, M);
			C->SetTranslucentSortPriority(-20);
			C->SetRenderCustomDepth(true);
			C->SetCustomDepthStencilValue(1);
			WarmCards.Add(C);
		}
		UProceduralMeshComponent* Pm = NewObject<UProceduralMeshComponent>(this);
		Pm->bUseAsyncCooking = false;
		Pm->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Pm->SetCastShadow(false);
		Pm->SetupAttachment(Camera);
		Pm->RegisterComponent();
		Pm->CreateMeshSection_LinearColor(0, Pos, Tri, Nrm, Uv, {}, {}, false);
		Pm->SetRelativeLocation(At + FVector(0.0, 3.0, 0.0));
		Pm->SetRelativeScale3D(FVector(0.01));
		Pm->SetMaterial(0, M);
		Pm->SetTranslucentSortPriority(-20);
		Pm->SetRenderCustomDepth(true);
		Pm->SetCustomDepthStencilValue(1);
		WarmCards.Add(Pm);
	}
	WarmFrames = 0;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT prebuild effect materials warmed: %d materials, %d cards, %.1f ms"), Mats.Num(), WarmCards.Num(), (FPlatformTime::Seconds() - T0) * 1000.0);
	if (!OldFirstUse()) WarmFirstUse(L);      // (FIRSTHITCH: the guns' held models and sounds, under the same dark)
}

bool AOutlawsPlayer::OldFirstUse()
{
	static const bool bOld = OLFlags::Param(TEXT("OLOldFirstUse"));      // (FIRSTHITCH's before: the first-use prebuild as it was)
	return bOld;
}

void AOutlawsPlayer::WarmScopeEffects(AOutlawsLevelActor* L)
{
	const double T0 = FPlatformTime::Seconds();
	if (!ScopeCapture) EnsureScopeRig(L);
	if (!ScopeCapture || WarmCards.Num() == 0) return;
	// the player's own parts stay out of the picture, as in play, but for the cards; a wide look at them for this one
	ScopeCapture->HiddenComponents.Reset();
	ScopeCapture->HideActorComponents(this);
	FBox Cards(ForceInit);
	for (UPrimitiveComponent* C : WarmCards)
	{
		if (!C) continue;
		ScopeCapture->HiddenComponents.Remove(C);
		Cards += C->GetComponentLocation();
	}
	const float Fov = ScopeCapture->FOVAngle;
	const FVector Eye = Camera->GetComponentLocation();
	ScopeCapture->FOVAngle = 60.f;
	ScopeCapture->SetWorldLocationAndRotation(Eye, (Cards.GetCenter() - Eye).Rotation());
	ScopeCapture->CaptureScene();
	ScopeCapture->FOVAngle = Fov;
	ScopeCapture->HiddenComponents.Reset();
	ScopeCapture->HideActorComponents(this);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT prebuild scope picture of the effect cards: %d cards, %.1f ms"), WarmCards.Num(), (FPlatformTime::Seconds() - T0) * 1000.0);
}

void AOutlawsPlayer::UpdateBreathVeil(float Amount)
{
	// Amount < 0: left as it is (the menu up)
	if (Amount < 0.f) return;
	if (!BreathVeil && Amount <= 0.001f) return;
	if (!BreathVeil)
	{
		UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_Vignette.M_OLR_Vignette"));
		if (!Base) return;
		BreathVeil = NewObject<UStaticMeshComponent>(this);
		BreathVeil->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane")));
		BreathVeil->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		BreathVeil->SetCastShadow(false);
		BreathVeil->SetupAttachment(Camera);
		BreathVeil->RegisterComponent();
		// as the badge's: 60 cm square, 15 cm out, past the edge of the view
		BreathVeil->SetRelativeLocationAndRotation(FVector(15.2, 0.0, 0.0), FRotator(90.0, 0.0, 0.0));
		BreathVeil->SetRelativeScale3D(FVector(0.6, 0.6, 1.0));
		BreathVeil->SetTranslucentSortPriority(-11);
		BreathVeil->SetBoundsScale(20.f);
		BreathVeilMid = UMaterialInstanceDynamic::Create(Base, this);
		// what is seen times near-black at the rim, the whole view a little dimmer, the middle last to go
		BreathVeilMid->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.02f, 0.02f, 0.035f));
		BreathVeilMid->SetScalarParameterValue(TEXT("Whole"), 0.3f);
		BreathVeilMid->SetScalarParameterValue(TEXT("Inner"), 0.12f);
		BreathVeilMid->SetScalarParameterValue(TEXT("Outer"), 0.7f);
		BreathVeil->SetMaterial(0, BreathVeilMid);
	}
	BreathVeilMid->SetScalarParameterValue(TEXT("Amount"), Amount);
	BreathVeil->SetVisibility(Amount > 0.001f && !bMovieHidden);
}

// ---- the shared feel (the consistency plan, user 2026-10-01) -------------------------------------------------------------

void AOutlawsPlayer::PulseImpact(int32 Hand, float Speed)
{
	Pulse(Hand, OutlawsFeel::ImpactPulse(Speed));
}

void AOutlawsPlayer::Refuse(int32 Hand)
{
	// two short buzzes a moment apart and the rattle of a locked thing (SND_USE_F of eleven of the game's items), the same
	// wherever something cannot be done: a greyed wheel slot, a gun into the crank hand, the shield with no plate left
	Pulse(Hand, 0.35f);
	RefuseSecond[Hand] = 0.12f;
	const double Now = FPlatformTime::Seconds();
	if (Now - RefuseSoundAt > 0.4)
	{
		RefuseSoundAt = Now;
		if (AOutlawsLevelActor* L = Level.Get()) L->PlayOnce2D(TEXT("locked"));
	}
}

void AOutlawsPlayer::UpdateRefuse(float Dt)
{
	for (int32 H = 0; H < 2; ++H)
	{
		if (RefuseSecond[H] < 0.f) continue;
		RefuseSecond[H] -= float(FApp::GetDeltaTime());      // (real time: the wheel slows the world)
		if (RefuseSecond[H] < 0.f) Pulse(H, 0.35f);
	}
}

// ---- the grip editor (a dev tool, user 2026-10-01) ----------------------------------------------------------------------

FTransform AOutlawsPlayer::ToolRest(int32 Hand) const
{
	// a tool's rest in one hand: the crowbar stands up out of the fist (turned as the knife and dynamite were; user
	// 2026-09-28), the shovel lies along the hand; or where the grip editor put it this session
	FTransform Rest = HeldTool[Hand] == TEXT("icrowbar") ? FTransform(FRotator(75.0, 0.0, 0.0)) : FTransform::Identity;
	FVector Baked;
	if (BakedGrip(TEXT("tool:") + HeldTool[Hand], Baked)) Rest.SetLocation(FVector(Baked.X, Hand == 0 ? -Baked.Y : Baked.Y, Baked.Z));     // (BUG-GRIPSAVE)
	if (const FTransform* Adj = GripAdjust.Find(FString::Printf(TEXT("tool:%s@%s"), *HeldTool[Hand], Hand == 0 ? TEXT("L") : TEXT("R")))) Rest.SetLocation(Adj->GetLocation());
	return Rest;
}

bool AOutlawsPlayer::GripEditorInput(int32 Hand, float Grip)
{
	const int32 O = 1 - Hand;
	FString Key;
	USceneComponent* Target = GripEditTarget(O, Key);
	if (EditHand == Hand)
	{
		if (Grip < 0.4f || !Target || Target != EditComp.Get())
		{
			FinishGripEdit();
			return true;
		}
		// the thing slides with the moving fist, never turning (user 2026-10-01: the aim is right; it is where the hand sits
		// on it that wants tuning): its place in the holding hand's frame is what is kept, its turn as it was
		const FVector WantAt = (EditRel * FrameOf(Hand)->GetComponentTransform()).GetLocation();
		const FVector RelAt = FrameOf(O)->GetComponentTransform().InverseTransformPosition(WantAt);
		FTransform Rel = Target->GetRelativeTransform();
		Rel.SetLocation(RelAt);
		if (Target == WeaponMesh[O])
		{
			GunBaseLoc[O] = RelAt;
			Target->SetRelativeLocation(RelAt);
		}
		else
		{
			Target->SetRelativeLocation(RelAt);
		}
		GripAdjust.Add(Key, Rel);
		return true;
	}
	if (EditHand >= 0) return false;
	const bool bFree = Equipped[Hand] < 0 && HeldTool[Hand].IsEmpty() && !HeldBottle[Hand] && !HeldItem[Hand].IsValid() && !GrabbedEnemy[Hand].IsValid()
		&& GrabHand != Hand;
	if (!bFree || !Target || Grip <= 0.6f || !bGripArmed[Hand]) return false;
	// the fist within 12 cm of the thing (all of it: a tool's parts, the lantern on its hang)
	FBox B(ForceInit);
	if (UPrimitiveComponent* Pc = Cast<UPrimitiveComponent>(Target)) B += Pc->Bounds.GetBox();
	TArray<USceneComponent*> Parts;
	Target->GetChildrenComponents(true, Parts);
	for (USceneComponent* C : Parts) if (UPrimitiveComponent* Pc = Cast<UPrimitiveComponent>(C); Pc && Pc->IsVisible()) B += Pc->Bounds.GetBox();
	if (!B.IsValid || B.ComputeSquaredDistanceToPoint(GripOf(Hand)->GetComponentLocation()) > FMath::Square(12.0)) return false;
	bGripArmed[Hand] = false;
	EditHand = Hand;
	EditComp = Target;
	EditKey = Key;
	EditRel = Target->GetComponentTransform().GetRelativeTransform(FrameOf(Hand)->GetComponentTransform());
	if (!GripDefault.Contains(Key)) GripDefault.Add(Key, Target->GetRelativeTransform());
	Pulse(Hand, 0.4f);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT grip editor: %s taken by the %s fist"), *Key, Hand == 0 ? TEXT("left") : TEXT("right"));
	return true;
}

void AOutlawsPlayer::FinishGripEdit()
{
	if (EditHand < 0) return;
	const int32 H = EditHand;
	EditHand = -1;
	EditComp.Reset();
	Pulse(H, 0.5f);
	PlaceGrip(EditKey);
	if (AOutlawsLevelActor* L = Level.Get()) L->Notify(FString::Printf(TEXT("Grip saved: %s"), *EditKey), 2.5f);
}

void AOutlawsPlayer::PlaceGrip(const FString& Key)
{
	// (BUG-GRIPSAVE) kept for good: placed by hand, and (REQ-GRIPMIRROR) a right hand's placement mirrored onto the left (Y
	// flipped, as a left-hand gun is drawn) unless the left was placed itself
	const FTransform* T = GripAdjust.Find(Key);
	if (!T) return;
	GripPlaced.Add(Key);
	if (Key.EndsWith(TEXT("@R")))
	{
		const FString LeftKey = Key.LeftChop(1) + TEXT("L");
		if (!GripPlaced.Contains(LeftKey))
		{
			const FVector At = T->GetLocation();
			const FRotator R = T->Rotator();
			GripAdjust.Add(LeftKey, FTransform(FRotator(R.Pitch, -R.Yaw, -R.Roll), FVector(At.X, -At.Y, At.Z)));
			if (GripDefault.Contains(Key) && !GripDefault.Contains(LeftKey)) GripDefault.Add(LeftKey, GripDefault[Key]);
		}
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT grip editor: %s placed at %s%s"), *Key, *T->GetLocation().ToString(), Key.EndsWith(TEXT("@R")) && !GripPlaced.Contains(Key.LeftChop(1) + TEXT("L")) ? TEXT(" (mirrored onto the left)") : TEXT(""));
	SaveGrips();
}

// ---- -OLShots -OLLanternSwimTest (REQ-LANTERNSWIM) ------------------------------------------------------------------------

FString AOutlawsPlayer::LanternSwimState() const
{
	const AOutlawsLevelActor* L = Level.Get();
	return FString::Printf(TEXT("hands L '%s' R '%s', shown L %d R %d, guns L %d R %d, lanterns carried %d"), *HeldTool[0], *HeldTool[1],
		ToolRoot[0] && ToolRoot[0]->IsVisible() ? 1 : 0, ToolRoot[1] && ToolRoot[1]->IsVisible() ? 1 : 0, Equipped[0], Equipped[1],
		L ? L->GetCount(TEXT("ilantern")) : -1);
}

#if WITH_OUTLAWS_TESTS
void AOutlawsPlayer::LanternSwimTestTick()
{
	// a lit lantern in the left hand, then in the right with the map in the left: put away once by the wheel's own path (the
	// same block picked again), taken out again, then carried into the water; the swim starts and the lantern goes in the
	// same frame, the hands as the wheel left them; out of the water it stays away
	AOutlawsLevelActor* L = Level.Get();
	if (!L) return;
	static int32 Round = 0, Sec = -1, Fails = 0;
	static float At = 0.f;
	static FString Manual;
	static uint64 InFrame = 0, SwimFrame = 0, AwayFrame = 0;
	static FVector Dry = FVector::ZeroVector;
	const int32 H = Round;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lanternswim check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (!bOk) ++Fails;
	};
	auto Finish = [this]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lanternswim RESULT %s (%d failed)"), Fails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Fails);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	};
	auto Lamp = [L]() { return FString::Printf(TEXT("lamp %s, oil %d"), L->bLampOn ? TEXT("lit") : TEXT("out"), L->GetCount(TEXT("ioil"))); };
	if (ShotStep == 0)
	{
		if (ShotTime < 3.f) return;
		Sec = L->DebugWaterSector();
		if (Sec < 0)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lanternswim: no water sector here"));
			++Fails;
			Finish();
			return;
		}
		Dry = GetActorLocation();
		if (L->GetCount(TEXT("ilantern")) <= 0) L->AddCount(TEXT("ilantern"), 1);
		L->AddCount(TEXT("ioil"), 150);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lanternswim: water sector %d"), Sec);
		At = ShotTime;
		ShotStep = 1;
	}
	else if (ShotStep == 1 && ShotTime > At + 0.5f)
	{
		if (Round == 1) EquipTool(0, TEXT("imap"));
		EquipTool(H, TEXT("ilantern"));
		if (!L->bLampOn) L->ToggleLamp();
		At = ShotTime;
		ShotStep = 2;
	}
	else if (ShotStep == 2 && ShotTime > At + 0.5f)
	{
		ToggleGear(H, TEXT("ilantern"));          // (the wheel's put-away: OutlawsPlayerWeapons.cpp ReleaseWheel)
		Manual = LanternSwimState();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lanternswim round %d (%s hand): put away by the wheel's path: %s; %s"), Round, H == 0 ? TEXT("left") : TEXT("right"), *Manual, *Lamp());
		EquipTool(H, TEXT("ilantern"));
		if (!L->bLampOn) L->ToggleLamp();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lanternswim round %d: taken out again: %s; %s"), Round, *LanternSwimState(), *Lamp());
		At = ShotTime;
		ShotStep = 3;
	}
	else if (ShotStep == 3 && ShotTime > At + 0.5f)
	{
		FVector Feet;
		float SlopeYaw = 0.f;
		if (!L->SlopeFoot(Sec, Feet, SlopeYaw))
		{
			Check(false, TEXT("a place in the water sector"));
			Finish();
			return;
		}
		Check(!bSwimming && HeldTool[H] == TEXT("ilantern"), FString::Printf(TEXT("dry, the lantern held before going in (swimming %d)"), bSwimming ? 1 : 0));
		TeleportTo(Feet + FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0), GetActorRotation(), false, true);
		GetCharacterMovement()->Velocity = FVector::ZeroVector;
		InFrame = GFrameCounter;
		SwimFrame = AwayFrame = 0;
		At = ShotTime;
		ShotStep = 4;
	}
	else if (ShotStep == 4)
	{
		if (SwimFrame == 0 && bSwimming) SwimFrame = GFrameCounter;
		if (AwayFrame == 0 && HeldTool[H] != TEXT("ilantern")) AwayFrame = GFrameCounter;
		if (ShotTime < At + 2.f) return;
		const FString Wet = LanternSwimState();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lanternswim round %d: in the water: %s; %s"), Round, *Wet, *Lamp());
		Check(SwimFrame > 0, FString::Printf(TEXT("round %d: the swim began (%lld frames after the teleport)"), Round, SwimFrame > 0 ? int64(SwimFrame - InFrame) : -1));
		Check(SwimFrame > 0 && AwayFrame >= SwimFrame && AwayFrame - SwimFrame <= 3,
			FString::Printf(TEXT("round %d: the lantern put away %lld frames after the swim began"), Round, SwimFrame > 0 && AwayFrame > 0 ? int64(AwayFrame) - int64(SwimFrame) : -1));
		Check(Wet == Manual, FString::Printf(TEXT("round %d: the hands in the water as the wheel's put-away left them ('%s' vs '%s')"), Round, *Wet, *Manual));
		if (Round == 1) Check(HeldTool[0] == TEXT("imap"), FString::Printf(TEXT("round 1: the map kept in the left hand ('%s')"), *HeldTool[0]));
		TeleportTo(Dry, GetActorRotation(), false, true);
		GetCharacterMovement()->Velocity = FVector::ZeroVector;
		At = ShotTime;
		ShotStep = 5;
	}
	else if (ShotStep == 5 && ShotTime > At + 2.f)
	{
		Check(!bSwimming && HeldTool[0] != TEXT("ilantern") && HeldTool[1] != TEXT("ilantern"),
			FString::Printf(TEXT("round %d: out of the water (swimming %d), the lantern still put away: %s"), Round, bSwimming ? 1 : 0, *LanternSwimState()));
		if (Round == 0)
		{
			Round = 1;
			At = ShotTime;
			ShotStep = 1;
			return;
		}
		Finish();
	}
}

OL_SHOT_MATCH(OLLanternSwimTest) { return OLFlags::Param(TEXT("OLLanternSwimTest")); }
OL_SHOT_TEST(OLLanternSwimTest, 90)
{
	LanternSwimTestTick();           // (REQ-LANTERNSWIM: OutlawsPlayerGear.cpp)
}
#endif // WITH_OUTLAWS_TESTS

// ---- -OLShots -OLLanternCostTest (REQ-LANTERNBOOST) -----------------------------------------------------------------------

#if WITH_OUTLAWS_TESTS
bool AOutlawsPlayer::TestShadersReady(float& Quiet, float Hold)
{
	// the world's own materials compiled for this feature level (a material still compiling draws black; while the local
	// shader cache cannot be written a changed one compiles at every start, and the headset's path on the desktop leaves a
	// tail of engine debug shaders that may never finish), then Hold seconds more; at most 5 minutes
	static const TCHAR* const Paths[] = { TEXT("/Game/Outlaws/Runtime/M_OLR_World.M_OLR_World"), TEXT("/Game/Outlaws/Runtime/M_OLR_WorldMasked.M_OLR_WorldMasked"),
		TEXT("/Game/Outlaws/Core/M_OL_Sprite.M_OL_Sprite"), TEXT("/Game/Outlaws/Core/M_OL_Sky.M_OL_Sky"), TEXT("/Game/Outlaws/Runtime/M_OLR_SkySurface.M_OLR_SkySurface"),
		TEXT("/Game/Outlaws/Runtime/M_OLR_Held.M_OLR_Held"), TEXT("/Game/Outlaws/Runtime/M_OLR_Water.M_OLR_Water") };
	static double Since = FPlatformTime::Seconds();
	const ERHIFeatureLevel::Type Fl = GetWorld()->FeatureLevel;
	int32 Waiting = 0;
#if WITH_EDITOR
	for (const TCHAR* P : Paths)
	{
		const UMaterial* M = LoadObject<UMaterial>(nullptr, P);
		const FMaterialResource* R = M ? M->GetMaterialResource(Fl) : nullptr;
		if (R && !R->IsCompilationFinished()) ++Waiting;
	}
#else
	(void)Fl;                          // (a cooked build: its shaders come with it)
	(void)Paths;
#endif
	const bool bIdle = !GShaderCompilingManager || !GShaderCompilingManager->IsCompiling();
	const bool bLate = FPlatformTime::Seconds() - Since > 300.0;
	if (Waiting > 0 && !bLate) { Quiet = -1.f; return false; }
	if (Quiet < 0.f)
	{
		Quiet = ShotTime;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT shaders ready after %.0f s (%s; %d jobs left%s)"), FPlatformTime::Seconds() - Since, Waiting ? TEXT("given up waiting") : TEXT("the world's materials compiled"),
			GShaderCompilingManager ? GShaderCompilingManager->GetNumRemainingJobs() : 0, bIdle ? TEXT(", idle") : TEXT(""));
	}
	return ShotTime - Quiet >= Hold;
}

void AOutlawsPlayer::LanternCostTestTick()
{
	// standing still, the lantern out, lit, out, lit (6 s each, the last 5 s of each measured): the GPU's frame time each way,
	// and the lit lantern's reach and colour in MPC_OLR_Lights L0 / C0 against REQ-LANTERNBOOST's 22 m and 0.75 (full oil)
	AOutlawsLevelActor* L = Level.Get();
	if (!L) return;
	constexpr float Win = 6.f, Skip = 1.f;
	static float T0 = 0.f;
	static double GpuSum[4] = {}, RtSum[4] = {};
	static int32 Frames[4] = {};
	static double ReachSum = 0.0, RSum = 0.0, GSum = 0.0, BSum = 0.0, DarkMax = 0.0;
	static int32 LitN = 0, DarkN = 0;
	static int32 Fails = 0;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lanterncost check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (!bOk) ++Fails;
	};
	// level, looking the way the start faces (the yard), still
	static float Yaw = -1000.f;
	if (Yaw < -999.f) Yaw = float(GetActorRotation().Yaw);
	if (Controller) Controller->SetControlRotation(FRotator(0.0, Yaw, 0.0));
	// (the shaders first: a material still compiling draws black and costs nothing; the measuring starts 3 s after)
	static float Quiet = -1.f;
	if (ShotStep == 0)
	{
		if (!TestShadersReady(Quiet, 3.f) || ShotTime < 5.f) return;
		if (L->GetCount(TEXT("ilantern")) <= 0) L->AddCount(TEXT("ilantern"), 1);
		if (L->bLampOn) L->ToggleLamp();
		T0 = ShotTime;
		// -OLCostScreenPct=<n>: the pixels of a headset's two eyes on the desktop's viewport (r.ScreenPercentage)
		int32 Pct = 0;
		if (OLFlags::Value(TEXT("OLCostScreenPct"), Pct) && Pct > 0)
		{
			if (IConsoleVariable* Sp = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage"))) Sp->Set(float(Pct), ECVF_SetByCode);
		}
		const IConsoleVariable* Sp = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage"));
		const FIntPoint Size = GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport ? GEngine->GameViewport->Viewport->GetSizeXY() : FIntPoint::ZeroValue;
		const float S = Sp ? Sp->GetFloat() / 100.f : 1.f;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lanterncost: start (%s, %s profile, viewport %dx%d, r.ScreenPercentage %.0f: about %.2f Mpixels)"),
			GetWorld()->FeatureLevel == ERHIFeatureLevel::ES3_1 ? TEXT("ES3_1, the headset's path") : TEXT("SM5"), OutlawsDeviceProfile::IsQuest2() ? TEXT("Quest2") : TEXT("default"),
			Size.X, Size.Y, S * 100.f, Size.X * Size.Y * S * S / 1e6f);
		ShotStep = 1;
		return;
	}
	if (ShotStep != 1) return;
	if (L->GetCount(TEXT("ioil")) < 300) L->AddCount(TEXT("ioil"), 300);       // (full the whole time: the oil's dimming kept out)
	const float In = ShotTime - T0;
	const int32 W = FMath::FloorToInt(In / Win);
	if (W < 4)
	{
		const bool bLit = (W % 2) == 1;
		if (L->bLampOn != bLit) L->ToggleLamp();
		if (In - W * Win < Skip) return;
		const double Gpu = FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0));
		if (Gpu > 0.0)
		{
			GpuSum[W] += Gpu;
			RtSum[W] += FPlatformTime::ToMilliseconds(GRenderThreadTime);
			++Frames[W];
		}
		FLinearColor P(0.f, 0.f, 0.f, 0.f), C(0.f, 0.f, 0.f, 0.f);
		if (UMaterialParameterCollectionInstance* M = L->LightsMpc && GetWorld() ? GetWorld()->GetParameterCollectionInstance(L->LightsMpc) : nullptr)
		{
			M->GetVectorParameterValue(TEXT("L0"), P);
			M->GetVectorParameterValue(TEXT("C0"), C);
		}
		if (bLit) { ReachSum += P.A; RSum += C.R; GSum += C.G; BSum += C.B; ++LitN; }
		else { DarkMax = FMath::Max(DarkMax, double(C.R + C.G + C.B)); ++DarkN; }
		return;
	}
	auto Ms = [](int32 k) { return Frames[k] > 0 ? GpuSum[k] / Frames[k] : 0.0; };
	auto Rt = [](int32 k) { return Frames[k] > 0 ? RtSum[k] / Frames[k] : 0.0; };
	const double Unlit = (Ms(0) + Ms(2)) * 0.5, Lit = (Ms(1) + Ms(3)) * 0.5;
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lanterncost: GPU ms unlit %.3f (%d frames), lit %.3f (%d), unlit %.3f (%d), lit %.3f (%d); render thread %.2f / %.2f / %.2f / %.2f ms"),
		Ms(0), Frames[0], Ms(1), Frames[1], Ms(2), Frames[2], Ms(3), Frames[3], Rt(0), Rt(1), Rt(2), Rt(3));
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lanterncost RESULT: GPU unlit %.3f ms, lit %.3f ms, lit - unlit %+.3f ms (%+.1f %%)"), Unlit, Lit, Lit - Unlit, Unlit > 0.0 ? 100.0 * (Lit - Unlit) / Unlit : 0.0);
	Check(Frames[0] > 30 && Frames[1] > 30 && Frames[2] > 30 && Frames[3] > 30, FString::Printf(TEXT("GPU timing in every window (%d %d %d %d frames)"), Frames[0], Frames[1], Frames[2], Frames[3]));
	const double Reach = LitN ? ReachSum / LitN : 0.0, R = LitN ? RSum / LitN : 0.0, G = LitN ? GSum / LitN : 0.0, B = LitN ? BSum / LitN : 0.0;
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lanterncost lantern: mean reach %.1f cm, mean colour %.4f %.4f %.4f over %d lit frames (REQ-LANTERNBOOST: 2200, 0.75 0.645 0.51; was 1100, 0.6)"), Reach, R, G, B, LitN);
	Check(LitN > 30 && FMath::Abs(Reach - 2200.0) <= 2200.0 * 0.03, FString::Printf(TEXT("the lit lantern's reach about 22 m: %.1f cm"), Reach));
	Check(LitN > 30 && FMath::Abs(R - 0.75) <= 0.75 * 0.06 && FMath::Abs(G / FMath::Max(R, 1e-6) - 0.86) < 0.005 && FMath::Abs(B / FMath::Max(R, 1e-6) - 0.68) < 0.005,
		FString::Printf(TEXT("the lit lantern 1.25 times as bright: colour %.4f %.4f %.4f"), R, G, B));
	Check(DarkN > 30 && DarkMax <= 0.0, FString::Printf(TEXT("dark while out (the brightest C0 sum %.4f over %d frames)"), DarkMax, DarkN));
	if (L->bLampOn) L->ToggleLamp();
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lanterncost RESULT %s (%d failed)"), Fails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Fails);
	ShotStep = 2;
	FGenericPlatformMisc::RequestExit(false);
	ShotTime = -1.f;
}

OL_SHOT_MATCH(OLLanternCostTest) { return OLFlags::Param(TEXT("OLLanternCostTest")); }
OL_SHOT_TEST(OLLanternCostTest, 850)
{
	LanternCostTestTick();           // (REQ-LANTERNBOOST: OutlawsPlayerGear.cpp)
}
#endif // WITH_OUTLAWS_TESTS

// ---- -OLShots -OLChestTest (REQ-CHESTPLATE) -----------------------------------------------------------------------------

#if WITH_OUTLAWS_TESTS
void AOutlawsPlayer::ChestTestTick(float Dt)
{
	AOutlawsLevelActor* L = Level.Get();
	APlayerController* PC = Cast<APlayerController>(Controller);
	if (!L || !PC || !Camera) return;
	if (ShotStep == 0) GEngine->Exec(GetWorld(), TEXT("r.MotionBlurQuality 0"));
	static const EOutlawsHand DomWas = DominantHand;      // (put back at the end)
	static float DownYaw = 0.f;
	static FString HandsBefore;
	// (the desktop turns the body with the view; a headset's play space stays put while the head turns: held still here)
	bUseControllerRotationYaw = ShotStep >= 20;
	auto Check = [this](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT chest %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (!bOk) ++ChestTestFails;
	};
	auto Shot = [](const TCHAR* Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(Name) + TEXT(".png"), false, false); };
	auto Look = [this, PC](float YawOff, float Pitch) { PC->SetControlRotation(FRotator(Pitch, ChestTestBaseYaw + YawOff, 0.0)); };
	// the plate against the head and the body: forward / right / up of the eyes in the body's frame (cm); its yaw off the
	// body's and the head's
	auto Where = [this](const TCHAR* Tag, FVector& OutRel, float& OutBodyOff, float& OutHeadOff) -> bool
	{
		FVector At;
		FRotator Rot;
		if (!ChestPlateShown(At, Rot))
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT chest %s: no plate shown"), Tag);
			return false;
		}
		const FVector Eye = Camera->GetComponentLocation();
		const FRotator Body(0.0, ChestYaw(), 0.0);
		OutRel = Body.UnrotateVector(At - Eye);
		OutBodyOff = float(FRotator::NormalizeAxis(Rot.Yaw - Body.Yaw));
		OutHeadOff = float(FRotator::NormalizeAxis(Body.Yaw - Camera->GetComponentRotation().Yaw));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT chest %s: plate at %.1f cm ahead, %.1f right, %.1f up of the eyes (body frame); its yaw %.1f, body yaw %.1f (%.1f off the plate, %.1f off the head), head pitch %.1f, tilt %.1f"),
			Tag, OutRel.X, OutRel.Y, OutRel.Z, Rot.Yaw, Body.Yaw, OutBodyOff, OutHeadOff, Camera->GetComponentRotation().Pitch, Rot.Pitch);
		return true;
	};
	auto Verts = [this]() { const FProcMeshSection* S = ChestMesh ? ChestMesh->GetProcMeshSection(0) : nullptr; return S ? S->ProcVertexBuffer.Num() : 0; };
	auto Hands = [this]() { return FString::Printf(TEXT("left: weapon %d, tool '%s'; right: weapon %d, tool '%s'"), Equipped[0], *HeldTool[0], Equipped[1], *HeldTool[1]); };
	// the wrist steps: the off hand held up before the eyes
	if (ShotStep >= 12)
	{
		const int32 Off = LeftDominant() ? 1 : 0;
		const FVector Eye = Camera->GetComponentLocation(), F = Camera->GetForwardVector(), R = Camera->GetRightVector(), Up = Camera->GetUpVector();
		for (int32 H = 0; H < 2; ++H)
		{
			UOutlawsBoxHand* Box = H == 0 ? LeftHand.Get() : RightHand.Get();
			if (!Box) continue;
			const double Side = H == 0 ? -1.0 : 1.0;
			const FVector Pos = H == Off ? Eye + F * 30.0 - Up * 4.0 - R * Side * 2.0 : Eye + F * 40.0 + R * Side * 22.0 - Up * 24.0;
			FrameOf(H)->SetWorldLocationAndRotation(Pos, FRotationMatrix::MakeFromXZ(H == Off ? (F + R * Side * 0.6).GetSafeNormal() : F, Up).ToQuat());
			Box->SetVisibility(true, true);
			Box->SetCurl(0.6f, 0.6f);
		}
	}
	const float T = ShotTime;
	switch (ShotStep)
	{
	case 0:
		if (T < 3.f) return;
		ChestTestBaseYaw = float(GetActorRotation().Yaw);
		Look(0.f, 0.f);
		L->AddCount(TEXT("idocbag"), 3);
		HandsBefore = Hands();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT chest pickup: before, armour %d, %s"), L->GetArmour(), *HandsBefore);
		L->ArmourMax = 0;
		L->AddCount(TEXT("ibplate"), 150);           // (as a pickup takes it: armour set, a new plate)
		++ShotStep;
		return;
	case 1:
	{
		if (T < 3.6f) return;
		Check(L->GetArmour() == 150, FString::Printf(TEXT("the plate picked up is worn: armour %d"), L->GetArmour()));
		Check(Hands() == HandsBefore && HeldTool[0].IsEmpty() && HeldTool[1].IsEmpty(), FString::Printf(TEXT("the plate is in neither hand, the hands as they were (%s)"), *Hands()));
		FVector Rel;
		float BodyOff = 0.f, HeadOff = 0.f;
		const bool bShown = Where(TEXT("picked up"), Rel, BodyOff, HeadOff);
		Check(bShown, TEXT("the plate is shown at once"));
		Check(bShown && Rel.Z < -22.0 && Rel.Z > -50.0 && Rel.X > -2.0 && Rel.X < 25.0 && FMath::Abs(Rel.Y) < 3.0,
			FString::Printf(TEXT("under the head at the chest: %.1f ahead, %.1f right, %.1f up"), Rel.X, Rel.Y, Rel.Z));
		Check(bShown && FMath::Abs(BodyOff) < 0.5f, FString::Printf(TEXT("it faces with the body (%.1f off)"), BodyOff));
		Look(20.f, 0.f);                             // (a head turn inside the body's 35 degrees)
		++ShotStep;
		return;
	}
	case 2:
	{
		if (T < 3.75f) return;
		FVector Rel;
		float BodyOff = 0.f, HeadOff = 0.f;
		Where(TEXT("head turned 20"), Rel, BodyOff, HeadOff);
		Check(FMath::Abs(HeadOff) > 12.f && FMath::Abs(HeadOff) <= 20.5f, FString::Printf(TEXT("a 20 degree head turn leaves the body behind at first (%.1f off the head)"), HeadOff));
		Look(90.f, 0.f);                             // (past it)
		++ShotStep;
		return;
	}
	case 3:
	{
		if (T < 3.9f) return;
		FVector Rel;
		float BodyOff = 0.f, HeadOff = 0.f;
		Where(TEXT("head turned 90"), Rel, BodyOff, HeadOff);
		Check(FMath::Abs(HeadOff) <= 35.5f && FMath::Abs(HeadOff) > 25.f, FString::Printf(TEXT("a 90 degree head turn drags the body to within 35 degrees (%.1f off the head)"), HeadOff));
		Check(FMath::Abs(BodyOff) < 0.5f, FString::Printf(TEXT("the plate turns with the body (%.1f off)"), BodyOff));
		Look(90.f, -75.f);                           // (looking well down: the body holds)
		++ShotStep;
		return;
	}
	case 4:
	{
		if (T < 4.1f) return;
		DownYaw = ChestYaw();
		Look(110.f, -75.f);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT chest looking down: body yaw %.1f"), DownYaw);
		++ShotStep;
		return;
	}
	case 5:
	{
		if (T < 4.6f) return;
		const float Was = DownYaw;
		Check(FMath::Abs(FRotator::NormalizeAxis(ChestYaw() - Was)) < 1.f, FString::Printf(TEXT("looking well down, a 20 degree head turn leaves the body as it was (%.1f, was %.1f)"), ChestYaw(), Was));
		Look(0.f, 0.f);
		++ShotStep;
		return;
	}
	case 6:
	{
		if (T < 8.f) return;
		FVector Rel;
		float BodyOff = 0.f, HeadOff = 0.f;
		Where(TEXT("head back ahead"), Rel, BodyOff, HeadOff);
		Check(FMath::Abs(HeadOff) < 3.f, FString::Printf(TEXT("the body eases back under the head (%.1f off)"), HeadOff));
		Look(0.f, -78.f);                            // (looking down at it)
		++ShotStep;
		return;
	}
	case 7:
	{
		if (T < 9.f) return;
		FVector Rel;
		float BodyOff = 0.f, HeadOff = 0.f;
		Where(TEXT("looking down"), Rel, BodyOff, HeadOff);
		ChestTestFreshVerts = Verts();
		Check(ChestMesh && ChestMesh->GetNumSections() <= 4 && ChestDents() == 0, FString::Printf(TEXT("a fresh plate: %d dents, %d corners, %d sections"), ChestDents(), ChestTestFreshVerts, ChestMesh ? ChestMesh->GetNumSections() : 0));
		Shot(TEXT("chest_fresh"));
		++ShotStep;
		return;
	}
	case 8:
	{
		// six hits of 1.5 from ahead and to the left: the plate takes them all (15 points each)
		if (T < 9.5f) return;
		static int32 Hits = 0;
		static float NextHit = 0.f;
		if (T < NextHit) return;
		NextHit = T + 0.15f;
		const float Health = L->GetHealth();
		L->bInvincible = false;
		L->SetHurtSource(Camera->GetComponentLocation() + FRotator(0.0, ChestYaw() - 25.0, 0.0).Vector() * 800.0);
		L->DamagePlayer(1.5f, 1.f, false);
		L->bInvincible = true;
		++Hits;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT chest hit %d: armour %d, health %.1f (was %.1f)"), Hits, L->GetArmour(), L->GetHealth(), Health);
		if (Hits >= 6) ++ShotStep;
		return;
	}
	case 9:
	{
		if (T < 11.f) return;
		const int32 V = Verts();
		Check(L->GetArmour() == 60, FString::Printf(TEXT("the armour took the six hits (armour %d of 150)"), L->GetArmour()));
		Check(ChestDents() == 6, FString::Printf(TEXT("a dent for each hit (%d)"), ChestDents()));
		Check(ChestMesh && (ChestMesh->GetNumSections() == 1 || ChestMesh->GetNumSections() == 4) && V > ChestTestFreshVerts,
			FString::Printf(TEXT("the plate bows round its dents and has no holes: %d corners (fresh %d), %d sections (no torn-wall section)"), V, ChestTestFreshVerts, ChestMesh ? ChestMesh->GetNumSections() : 0));
		FVector Rel;
		float BodyOff = 0.f, HeadOff = 0.f;
		Check(Where(TEXT("dented"), Rel, BodyOff, HeadOff), TEXT("the dented plate is still worn"));
		Shot(TEXT("chest_dented"));
		++ShotStep;
		return;
	}
	case 10:
	{
		if (T < 11.6f) return;
		const float Health = L->GetHealth();
		L->bInvincible = false;
		L->DamagePlayer(8.f, 1.f, false);           // (more than its 6 points left: it breaks, the rest reaches health)
		L->bInvincible = true;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT chest big hit: armour %d, health %.1f (was %.1f)"), L->GetArmour(), L->GetHealth(), Health);
		++ShotStep;
		return;
	}
	case 11:
	{
		if (T < 12.4f) return;
		FVector At;
		FRotator Rot;
		bool bLine = false;
		for (const FString& H : HudLines) bLine |= H.Contains(TEXT("breaks apart"));
		Check(L->GetArmour() == 0 && !ChestPlateShown(At, Rot), FString::Printf(TEXT("broken, the plate is gone (armour %d)"), L->GetArmour()));
		Check(bLine, FString::Printf(TEXT("'The boiler plate breaks apart' shown (%d lines up)"), HudLines.Num()));
		Shot(TEXT("chest_broken"));
		// the wrist (BUG-WRISTPLATE): no plate, a fresh plate, a dented plate, on each hand; the bags still carried.  The
		// plate's card was laid out before it broke, so a card left showing would sit over the health number
		Look(0.f, -10.f);
		WristLast.Reset();
		++ShotStep;
		return;
	}
	case 12: case 13: case 14:
	case 15: case 16: case 17:
	{
		const int32 k = ShotStep - 12, State = k % 3;          // 0 no plate, 1 fresh, 2 dented
		if (T < 13.6f + k * 1.2f) return;
		static const TCHAR* StateNames[] = { TEXT("noplate"), TEXT("fresh"), TEXT("dented") };
		const bool bLeft = LeftDominant();
		const UOutlawsBoxHand* Off = bLeft ? RightHand.Get() : LeftHand.Get();
		const UOutlawsBoxHand* Main = bLeft ? LeftHand.Get() : RightHand.Get();
		const FVector W = WristRoot ? WristRoot->GetComponentLocation() : FVector::ZeroVector;
		const double DOff = Off ? FVector::Dist(W, Off->GetComponentLocation()) : 1e9, DMain = Main ? FVector::Dist(W, Main->GetComponentLocation()) : 1e9;
		int32 Shown = 0;
		for (const UStaticMeshComponent* P : WristIcons) Shown += P && P->IsVisible() ? 1 : 0;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT chest wrist (%s-handed, %s, armour %d): '%s', %d cards shown, %d numbers, %.1f cm from the off hand, %.1f from the main; bags carried %d"),
			bLeft ? TEXT("left") : TEXT("right"), StateNames[State], L->GetArmour(), *WristShownKeys, Shown, WristPlatesUsed, DOff, DMain, L->GetCount(TEXT("idocbag")));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT chest wrist rows (%s-handed, %s): %s"), bLeft ? TEXT("left") : TEXT("right"), StateNames[State], *WristRowsLog);
		Check(WristRoot && WristRoot->IsVisible() && DOff < 15.0 && DOff < DMain, FString::Printf(TEXT("%s-handed: the wrist on the %s hand"), bLeft ? TEXT("left") : TEXT("right"), bLeft ? TEXT("right") : TEXT("left")));
		if (State == 0)
		{
			Check(WristShownKeys == TEXT("nwx:intheart") && WristPlatesUsed == 1 && Shown == 1,
				FString::Printf(TEXT("no plate: the health row alone, no plate card ('%s', %d cards shown, %d numbers)"), *WristShownKeys, Shown, WristPlatesUsed));
		}
		else
		{
			// two rows: the plate's card and number a row under the heart's and health's, at the same left edge
			const FVector H = WristIcons.Num() > 1 ? WristIcons[0]->GetRelativeLocation() : FVector::ZeroVector;
			const FVector P = WristIcons.Num() > 1 ? WristIcons[1]->GetRelativeLocation() : FVector::ZeroVector;
			const FVector HN = WristPlates.Num() > 1 ? PlateHolders[WristPlates[0]]->GetRelativeLocation() : FVector::ZeroVector;
			const FVector PN = WristPlates.Num() > 1 ? PlateHolders[WristPlates[1]]->GetRelativeLocation() : FVector::ZeroVector;
			Check(WristShownKeys == TEXT("nwx:intheart nwx:ibplate") && WristPlatesUsed == 2 && Shown == 2,
				FString::Printf(TEXT("health and armour only ('%s', %d cards shown, %d numbers), with %d bags carried"), *WristShownKeys, Shown, WristPlatesUsed, L->GetCount(TEXT("idocbag"))));
			Check(H.Z - P.Z > 2.9 && FMath::Abs(H.Y - P.Y) < 0.5 && HN.Z - PN.Z > 2.9 && FMath::Abs(HN.Y - PN.Y) < 0.5,
				FString::Printf(TEXT("the plate a row under health: cards %.1f cm apart down, %.1f across; numbers %.1f down, %.1f across"), H.Z - P.Z, FMath::Abs(H.Y - P.Y), HN.Z - PN.Z, FMath::Abs(HN.Y - PN.Y)));
		}
		Shot(*FString::Printf(TEXT("wrist_%s_%s"), bLeft ? TEXT("left") : TEXT("right"), StateNames[State]));
		// the next state
		if (State == 0) { L->ArmourMax = 0; L->AddCount(TEXT("ibplate"), 100); }     // (a pickup: a fresh plate)
		else if (State == 1) { L->bInvincible = false; L->DamagePlayer(3.f, 1.f, false); L->bInvincible = true; }     // (dented)
		else { L->SetArmour(0); DominantHand = bLeft ? EOutlawsHand::Right : EOutlawsHand::Left; }     // (gone; the other hand)
		WristLast.Reset();
		++ShotStep;
		return;
	}
	case 18:
	{
		// the look the other machines draw on this player, 1.2 m before these eyes and facing them, as a puppet's plate faces
		// a viewer in front of it (chest_look_front)
		if (T < 21.f) return;
		UProceduralMeshComponent* Look3D = nullptr;
		if (BuildPlateLook(this, 150, Look3D) && Look3D)
		{
			const FVector Eye = Camera->GetComponentLocation();
			const FVector Fl = FRotator(0.0, Camera->GetComponentRotation().Yaw, 0.0).Vector();
			PC->SetControlRotation(FRotator(0.0, Camera->GetComponentRotation().Yaw, 0.0));
			Look3D->SetWorldLocationAndRotation(Eye + Fl * 120.0, FRotator(12.0, Fl.Rotation().Yaw + 180.0, 0.0));
			for (int32 i = 0; i < Look3D->GetNumMaterials(); ++i)
			{
				if (UMaterialInstanceDynamic* M = Cast<UMaterialInstanceDynamic>(Look3D->GetMaterial(i))) M->SetScalarParameterValue(TEXT("Brightness"), 1.f);
			}
			const FBox B = Look3D->Bounds.GetBox();
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT chest the other machines' look: box %.1f x %.1f x %.1f cm (world)"), B.GetSize().X, B.GetSize().Y, B.GetSize().Z);
		}
		++ShotStep;
		return;
	}
	case 19:
		if (T < 22.f) return;
		Shot(TEXT("chest_look_front"));
		++ShotStep;
		return;
	case 20:
		if (T < 22.6f) return;
		DominantHand = DomWas;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT chest RESULT %s (%d failed)"), ChestTestFails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), ChestTestFails);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
		return;
	default:
		return;
	}
}

OL_SHOT_MATCH(OLChestTest) { return OLFlags::Param(TEXT("OLChestTest")); }
OL_SHOT_TEST(OLChestTest, 1080)
{
	ChestTestTick(Dt);                   // (REQ-CHESTPLATE: OutlawsPlayerGear.cpp)
}
#endif // WITH_OUTLAWS_TESTS
