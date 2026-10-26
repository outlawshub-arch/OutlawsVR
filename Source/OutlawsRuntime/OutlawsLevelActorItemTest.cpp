#include "OutlawsLevelActor.h"
#include "OutlawsFlags.h"
#include "OutlawsEnemy.h"
#include "OutlawsCoop.h"
#include "Components/CapsuleComponent.h"
#include "OutlawsFeel.h"
#include "OutlawsInk.h"
#include "OutlawsTextures.h"
#include "OutlawsLab.h"
#include "OutlawsRuntime.h"
#include "OutlawsObjMesh.h"
#include "OutlawsSpriteActor.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_OUTLAWS_TESTS

// ---- -OLItemTest -----------------------------------------------------------------------------------
// The sprite-to-3D item prototypes (tools/olue item3d: extruded, lathed and hand-fitted models whose UVs
// read the item sprites) laid out on the floor before the player start at their real size: small things
// in a near arc, long guns and tools in a far one. The OBJs live outside the project like the VE weapon
// placeholders, so nothing from them is cooked. -OLItemDir=<dir> reads another folder.
// Each item is drawn as big as the original's sprite (<name>.json world_scale; user 2026-09-28: the real
// sizes were too small to spot); -OLItemRealSize shows the real size kept for the hands, -OLItemWorldMul=<f>
// scales the world size.  The item models themselves are OutlawsLevelActorItems.cpp.

namespace OlItemModel
{
	void SmoothItemNormals(FOutlawsMeshSection& S);
	double ItemWorldScale(const FString& JsonPath);
	FString ItemModelDir();
}

void AOutlawsLevelActor::SpawnItemTest()
{
	using namespace OlItemModel;
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	for (UProceduralMeshComponent* M : ItemTestMeshes)
	{
		if (M)
		{
			ObjectMeshes.Remove(M);
			M->DestroyComponent();
		}
	}
	ItemTestMeshes.Reset();
	FString Dir = ItemModelDir();
	OLFlags::Value(TEXT("OLItemDir"), Dir);
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *FPaths::Combine(Dir, TEXT("*.obj")), true, false);
	Files.Sort();
	if (Files.Num() == 0)
	{
		UE_LOG(LogOutlaws, Warning, TEXT("OLRT item test: no OBJs in %s"), *Dir);
		return;
	}
	bEnemiesActive = false;          // a look round, not a fight
	bInvincible = true;
	GiveAllWeapons();                // the sprite-built guns and shovel to try in the hands
	DebugGiveListed(TEXT("ishovel"), TEXT("Shovel"));

	struct FItem
	{
		FString Name;
		TMap<FString, FOutlawsMeshSection> Sections;
		FQuat Lay = FQuat::Identity;
		double Width = 0.0;
	};
	TArray<FItem> Near, Far;
	for (const FString& F : Files)
	{
		OutlawsObj::FObjMesh Obj;
		if (!OutlawsObj::Load(FPaths::Combine(Dir, F), 1.0, true, Obj))
		{
			continue;
		}
		FItem It;
		It.Name = FPaths::GetBaseFilename(F);
		It.Sections = MoveTemp(Obj.Sections);
		double Scale = 1.0;
		if (!OLFlags::Param(TEXT("OLItemRealSize")))
		{
			double Mul = 1.0;
			OLFlags::Value(TEXT("OLItemWorldMul"), Mul);
			Scale = ItemWorldScale(FPaths::Combine(Dir, It.Name + TEXT(".json"))) * Mul;
		}
		for (TPair<FString, FOutlawsMeshSection>& Kv : It.Sections)
		{
			for (FVector& P : Kv.Value.Positions) P *= Scale;
			SmoothItemNormals(Kv.Value);
		}
		// flat things (badge, guns, keys) lie on a face: their thin Y turned up, the sprite side on top
		const FVector E = Obj.Bounds.GetExtent();
		if (E.Y < 0.25 * FMath::Max(E.X, E.Z))
		{
			It.Lay = FQuat(FVector::ForwardVector, HALF_PI);
		}
		FBox B(ForceInit);
		for (const TPair<FString, FOutlawsMeshSection>& Kv : It.Sections)
		{
			for (const FVector& P : Kv.Value.Positions) B += It.Lay.RotateVector(P);
		}
		It.Width = B.GetSize().X;
		(It.Width > 120.0 ? Far : Near).Add(MoveTemp(It));
	}

	const FVector LightDir = FVector(0.35, -0.45, 0.82).GetSafeNormal();
	FCollisionQueryParams Q(TEXT("OLItemTest"), false);
	if (APlayerController* PC = World->GetFirstPlayerController(); PC && PC->GetPawn()) Q.AddIgnoredActor(PC->GetPawn());
	TMap<FString, UMaterialInstanceDynamic*> Mats;
	auto PlaceRow = [&](TArray<FItem>& Row, double Radius)
	{
		const double Gap = 30.0;
		double Total = -Gap;
		for (const FItem& It : Row) Total += It.Width + Gap;
		Radius = FMath::Max(Radius, Total / (0.75 * PI));     // the row spans at most 135 degrees
		double Along = -Total * 0.5;
		for (FItem& It : Row)
		{
			const double Mid = Along + It.Width * 0.5;
			Along += It.Width + Gap;
			const double Ang = PlayerStartYaw + FMath::RadiansToDegrees(Mid / Radius);
			const FVector Dir2 = FRotator(0.0, Ang, 0.0).Vector();
			// pulled in short of any wall between the start and the spot
			FVector Spot = PlayerStartLocation + Dir2 * Radius;
			FHitResult Wall;
			if (World->LineTraceSingleByChannel(Wall, PlayerStartLocation, Spot, ECC_Visibility, Q))
			{
				Spot = PlayerStartLocation + Dir2 * FMath::Max(40.0, Wall.Distance - 30.0);
			}
			double FloorZ = PlayerStartLocation.Z - 90.0;
			FHitResult Hit;
			if (World->LineTraceSingleByChannel(Hit, FVector(Spot.X, Spot.Y, PlayerStartLocation.Z + 40.0),
				FVector(Spot.X, Spot.Y, PlayerStartLocation.Z - 400.0), ECC_Visibility, Q))
			{
				FloorZ = Hit.ImpactPoint.Z;
			}
			// the sprite side (+Y) faces the player; the long side runs along the arc
			const FQuat Face = FQuat(FVector::UpVector, FMath::DegreesToRadians(Ang + 180.0 - 90.0)) * It.Lay;
			double MinZ = TNumericLimits<double>::Max();
			FVector Centre = FVector::ZeroVector;
			int32 Count = 0;
			for (const TPair<FString, FOutlawsMeshSection>& Kv : It.Sections)
			{
				for (const FVector& P : Kv.Value.Positions)
				{
					const FVector R = Face.RotateVector(P);
					MinZ = FMath::Min(MinZ, R.Z);
					Centre += R;
					++Count;
				}
			}
			Centre /= FMath::Max(1, Count);
			const FVector Offset = FVector(Spot.X - Centre.X, Spot.Y - Centre.Y, FloorZ + 0.3 - MinZ);
			const float Bright = FMath::Clamp(LightAt(FVector(Spot.X, Spot.Y, FloorZ + 30.0)), 0, 31) / 31.f;

			UProceduralMeshComponent* M = NewMeshComponent(*(TEXT("OLItemTest_") + It.Name), false);
			int32 Index = 0;
			for (TPair<FString, FOutlawsMeshSection>& Kv : It.Sections)
			{
				// both faces of every triangle (the prototypes are not wound consistently yet), each shaded by
				// its own normal against a fixed key light so the shapes read in the unlit world material
				FOutlawsMeshSection& S = Kv.Value;
				const int32 N = S.Positions.Num();
				TArray<FVector> Pos;
				TArray<FVector> Nrm;
				TArray<FVector2D> UV;
				TArray<FLinearColor> Col;
				TArray<int32> Tri;
				for (int32 Side = 0; Side < 2; ++Side)
				{
					for (int32 i = 0; i < N; ++i)
					{
						const FVector Nw = Face.RotateVector(S.Normals[i]) * (Side ? -1.0 : 1.0);
						const float Shade = Bright * (0.68f + 0.32f * FMath::Max(0.f, float(FVector::DotProduct(Nw, LightDir))));
						Pos.Add(Face.RotateVector(S.Positions[i]) + Offset);
						Nrm.Add(Nw);
						UV.Add(S.UVs[i]);
						Col.Add(FLinearColor(Shade, Shade, Shade, 1.f));
					}
				}
				for (int32 t = 0; t + 2 < S.Triangles.Num(); t += 3)
				{
					Tri.Append({ S.Triangles[t], S.Triangles[t + 1], S.Triangles[t + 2] });
					Tri.Append({ N + S.Triangles[t], N + S.Triangles[t + 2], N + S.Triangles[t + 1] });
				}
				M->CreateMeshSection_LinearColor(Index, Pos, Tri, Nrm, UV, Col, TArray<FProcMeshTangent>(), false);
				UMaterialInstanceDynamic*& Mat = Mats.FindOrAdd(S.Texture);
				if (!Mat && WorldMaskedMaterial)
				{
					Mat = UMaterialInstanceDynamic::Create(WorldMaskedMaterial, this);
					if (UTexture2D* T = OutlawsObj::LoadTextureFile(S.Texture)) Mat->SetTextureParameterValue(TEXT("Tex"), T);
				}
				M->SetMaterial(Index, Mat);
				++Index;
			}
			ObjectMeshes.Add(M);
			ItemTestMeshes.Add(M);
			UE_LOG(LogOutlaws, Log, TEXT("OLRT item test: %s at %s (width %.0f cm, light %.2f)"), *It.Name, *Spot.ToString(), It.Width, Bright);
		}
	};
	double NearR = 150.0;
	{
		double Total = 0.0;
		for (const FItem& It : Near) Total += It.Width + 30.0;
		NearR = FMath::Max(NearR, Total / (0.75 * PI));
	}
	PlaceRow(Near, NearR);
	PlaceRow(Far, NearR + 220.0);
	UE_LOG(LogOutlaws, Display, TEXT("OLRT item test: %d items from %s"), ItemTestMeshes.Num(), *Dir);
}
#endif // WITH_OUTLAWS_TESTS
