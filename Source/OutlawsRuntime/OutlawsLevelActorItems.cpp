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

// The item models (tools/olue item3d: the sprite-to-3D models whose UVs read the item sprites) in the level: loaded,
// placed, lit, animated and settled.  The -OLItemTest layout of all of them is OutlawsLevelActorItemTest.cpp (it shares
// OlItemModel's three helpers).

namespace OlItemModel
{
	/** Face normals from each triangle as emitted (outward = (Z - X) x (Y - X), as FOutlawsMeshSection::AddFace),
	 *  then smoothed across faces meeting at a point within 55 degrees: rounded shapes shade round, box edges stay hard. */
	void SmoothItemNormals(FOutlawsMeshSection& S)
	{
		TArray<FVector> Face;
		Face.Init(FVector::ZeroVector, S.Positions.Num());
		for (int32 t = 0; t + 2 < S.Triangles.Num(); t += 3)
		{
			const FVector& X = S.Positions[S.Triangles[t]];
			const FVector& Y = S.Positions[S.Triangles[t + 1]];
			const FVector& Z = S.Positions[S.Triangles[t + 2]];
			const FVector N = FVector::CrossProduct(Z - X, Y - X).GetSafeNormal();
			for (int32 k = 0; k < 3; ++k) Face[S.Triangles[t + k]] = N;
		}
		TMap<FIntVector, TArray<int32>> At;
		for (int32 i = 0; i < S.Positions.Num(); ++i)
		{
			const FVector& P = S.Positions[i];
			At.FindOrAdd(FIntVector(FMath::RoundToInt(P.X * 50.0), FMath::RoundToInt(P.Y * 50.0), FMath::RoundToInt(P.Z * 50.0))).Add(i);
		}
		const double CosLimit = FMath::Cos(FMath::DegreesToRadians(55.0));
		S.Normals.SetNum(S.Positions.Num());
		for (const TPair<FIntVector, TArray<int32>>& Kv : At)
		{
			for (int32 i : Kv.Value)
			{
				FVector Sum = FVector::ZeroVector;
				for (int32 j : Kv.Value) if (FVector::DotProduct(Face[i], Face[j]) > CosLimit) Sum += Face[j];
				S.Normals[i] = Sum.GetSafeNormal(UE_SMALL_NUMBER, Face[i]);
			}
		}
	}

	double ItemWorldScale(const FString& JsonPath)
	{
		FString Text;
		TSharedPtr<FJsonObject> J;
		double Scale = 1.0;
		if (FFileHelper::LoadFileToString(Text, *JsonPath) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), J) && J.IsValid())
		{
			J->TryGetNumberField(TEXT("world_scale"), Scale);
		}
		return Scale;
	}

	FString ItemModelDir() { return FPaths::Combine(FOutlawsAssets::PlaceholderDir(), TEXT("items3d")); }
}

namespace
{
	using OlItemModel::SmoothItemNormals;
	using OlItemModel::ItemWorldScale;
	using OlItemModel::ItemModelDir;

	/** One item model, loaded once: its sections at the sprite's world size with smoothed normals, and whether it is
	 *  a flat thing that lies on its face (badge, guns, keys). */
	struct FItemModelData
	{
		TMap<FString, FOutlawsMeshSection> Sections;
		bool bFlat = false;
		float Spin = 0.f;        // degrees a second about the vertical (the sheriff's star twirls)
		float Hover = 0.f;       // cm off the floor
		float WorldScale = 1.f;  // the json's world_scale: the sprite's size over the real thing's
		bool bAnyYaw = false;    // "any_yaw": turned by where it stands, not by a wall (trees, shrubs: no two alike)
		bool bKeepOrigin = false; // "keep_origin": stood on the model's own origin (a tree's trunk), not its box's middle
		// the sprite's animation as keyframes (anim_catalogue, 2026-10-01): a swing played for a chor; a waving cloth
		TMap<int32, AOutlawsLevelActor::FOutlawsKeyAnim> Anims;     // by chor
		bool bFlicker = false;
		float FlickFps = 10.f;
		TArray<float> FlickScale, FlickBright;
		FString FlickSection;
		TArray<FString> NoInk;
		bool bFlames = false, bHang = false, bBulbs = false;     // lit (fire in its glass), hanging (swings), shades break alone
		AOutlawsLevelActor::FItemTraits Traits;                    // "hold", "breaks" (traits.py)
		bool bBend = false;
		float BendFps = 10.f, BendFlutter = 0.5f;
		TArray<float> BendTop;
		bool bWave = false;
		float WaveFps = 10.f, WaveRipple = 9.f, PoleR = 1.f;
		TArray<float> WaveX;
		TArray<TArray<float>> WaveDrop;
	};
	TMap<FString, TSharedPtr<FItemModelData>> GItemModels;      // by sprite stem; null = no model

	const FItemModelData* ItemModelFor(const FString& Stem);
}

const AOutlawsLevelActor::FItemTraits& AOutlawsLevelActor::ItemTraits(const FString& Stem)
{
	static const FItemTraits None;
	const FItemModelData* D = ItemModelFor(Stem);
	return D ? D->Traits : None;
}

namespace
{
	const FItemModelData* ItemModelFor(const FString& Stem)
	{
		if (const TSharedPtr<FItemModelData>* Hit = GItemModels.Find(Stem))
		{
			return Hit->Get();
		}
		TSharedPtr<FItemModelData> D;
		const FString Path = FPaths::Combine(ItemModelDir(), Stem + TEXT(".obj"));
		OutlawsObj::FObjMesh Obj;
		if (FPaths::FileExists(Path) && OutlawsObj::Load(Path, 1.0, true, Obj))
		{
			// (SCOPEFACES, FACETURN) item3d's generators wound some models' tubes and shells inward (the scope, lever, crowbar and
			// crank pickups; the cacti, TREE01, the cow skull, the flags, the gatling...): every model turned to face out before
			// the normals are worked out from the winding, so the headset's ink shell goes round it, not inside it (the floor
			// draws both faces, so on the PC they looked right before).  -OLOldScopeFaces: as the files have them
			OutlawsObj::FaceItemModelOutward(Obj, FString::Printf(TEXT("OLRT item model %s"), *Stem));
			D = MakeShared<FItemModelData>();
			D->Sections = MoveTemp(Obj.Sections);
			const double Scale = ItemWorldScale(FPaths::Combine(ItemModelDir(), Stem + TEXT(".json")));
			D->WorldScale = float(Scale);
			const FVector E = Obj.Bounds.GetExtent();
			D->bFlat = E.Y < 0.25 * FMath::Max(E.X, E.Z);
			// the json's "lay" ("flat" / "up") where the shape's proportions guess wrong (keys: their bows stand them up)
			FString Text, Lay;
			TSharedPtr<FJsonObject> J;
			if (FFileHelper::LoadFileToString(Text, *FPaths::Combine(ItemModelDir(), Stem + TEXT(".json"))) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), J)
				&& J.IsValid())
			{
				if (J->TryGetStringField(TEXT("lay"), Lay)) D->bFlat = Lay.Equals(TEXT("flat"), ESearchCase::IgnoreCase);
				double V = 0.0;
				if (J->TryGetNumberField(TEXT("spin"), V)) D->Spin = float(V);
				if (J->TryGetNumberField(TEXT("hover_cm"), V)) D->Hover = float(V);
				bool B = false;
				if (J->TryGetBoolField(TEXT("any_yaw"), B)) D->bAnyYaw = B;
				if (J->TryGetBoolField(TEXT("keep_origin"), B)) D->bKeepOrigin = B;
				if (J->TryGetBoolField(TEXT("flames"), B)) D->bFlames = B;
				if (J->TryGetBoolField(TEXT("hang"), B)) D->bHang = B;
				if (J->TryGetBoolField(TEXT("bulbs"), B)) D->bBulbs = B;
				if (J->TryGetNumberField(TEXT("pole_r"), V)) D->PoleR = float(V);
				const TSharedPtr<FJsonObject>* Ho = nullptr;
				if (J->TryGetObjectField(TEXT("hold"), Ho) && Ho)
				{
					(*Ho)->TryGetStringField(TEXT("mode"), D->Traits.HoldMode);
					if ((*Ho)->TryGetNumberField(TEXT("in_hand"), V)) D->Traits.InHand = float(V);
				}
				const TSharedPtr<FJsonObject>* Br = nullptr;
				if (J->TryGetObjectField(TEXT("breaks"), Br) && Br)
				{
					if ((*Br)->TryGetBoolField(TEXT("on"), B)) D->Traits.Breaks = B ? 1 : 0;
					(*Br)->TryGetStringField(TEXT("sound"), D->Traits.BreakSound);
				}
				auto Floats = [](const TSharedPtr<FJsonObject>& O, const TCHAR* Key, TArray<float>& Out)
				{
					const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
					if (O->TryGetArrayField(Key, Arr)) for (const TSharedPtr<FJsonValue>& Vv : *Arr) Out.Add(float(Vv->AsNumber()));
				};
				auto Names = [](const TSharedPtr<FJsonObject>& O, const TCHAR* Key, TArray<FString>& Out)
				{
					const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
					if (O->TryGetArrayField(Key, Arr)) for (const TSharedPtr<FJsonValue>& Vv : *Arr) Out.Add(Vv->AsString());
				};
				// "swing" (the first bell json): {chor, fps, deg}, about the top
				const TSharedPtr<FJsonObject>* Sw = nullptr;
				if (J->TryGetObjectField(TEXT("swing"), Sw) && Sw)
				{
					AOutlawsLevelActor::FOutlawsKeyAnim KA;
					double N = 1.0;
					(*Sw)->TryGetNumberField(TEXT("fps"), N);
					KA.Fps = float(N);
					KA.bTop = true;
					Floats(*Sw, TEXT("deg"), KA.Deg);
					double Ch = 1.0;
					(*Sw)->TryGetNumberField(TEXT("chor"), Ch);
					D->Anims.Add(int32(Ch), KA);
				}
				// "anims": {"<chor>": {fps, pivot "top"|"foot", deg[], lift[], hide[]}}
				const TSharedPtr<FJsonObject>* An = nullptr;
				if (J->TryGetObjectField(TEXT("anims"), An) && An)
				{
					for (const TPair<FString, TSharedPtr<FJsonValue>>& Kv : (*An)->Values)
					{
						const TSharedPtr<FJsonObject> O = Kv.Value->AsObject();
						if (!O) continue;
						AOutlawsLevelActor::FOutlawsKeyAnim KA;
						double N = 10.0;
						O->TryGetNumberField(TEXT("fps"), N);
						KA.Fps = float(N);
						FString Pv;
						KA.bTop = O->TryGetStringField(TEXT("pivot"), Pv) && Pv.Equals(TEXT("top"), ESearchCase::IgnoreCase);
						Floats(O, TEXT("deg"), KA.Deg);
						Floats(O, TEXT("lift"), KA.Lift);
						Names(O, TEXT("hide"), KA.Hide);
						D->Anims.Add(FCString::Atoi(*Kv.Key), KA);
					}
				}
				// "flicker": {section, fps, scale[], bright[]} (a flame card's sprite drawings); "noink": parts left out
				const TSharedPtr<FJsonObject>* Fl = nullptr;
				if (J->TryGetObjectField(TEXT("flicker"), Fl) && Fl)
				{
					double N = 10.0;
					(*Fl)->TryGetNumberField(TEXT("fps"), N);
					D->FlickFps = float(N);
					(*Fl)->TryGetStringField(TEXT("section"), D->FlickSection);
					Floats(*Fl, TEXT("scale"), D->FlickScale);
					Floats(*Fl, TEXT("bright"), D->FlickBright);
					D->bFlicker = D->FlickScale.Num() > 0 && !D->FlickSection.IsEmpty();
				}
				Names(J, TEXT("noink"), D->NoInk);
				// "bend": {fps, top[] (cm a step), flutter (cm)}: the plants' sway / gust
				const TSharedPtr<FJsonObject>* Bn = nullptr;
				if (J->TryGetObjectField(TEXT("bend"), Bn) && Bn)
				{
					double N = 10.0;
					(*Bn)->TryGetNumberField(TEXT("fps"), N);
					D->BendFps = float(N);
					N = 0.5;
					(*Bn)->TryGetNumberField(TEXT("flutter"), N);
					D->BendFlutter = float(N);
					Floats(*Bn, TEXT("top"), D->BendTop);
					D->bBend = D->BendTop.Num() > 1;
				}
				const TSharedPtr<FJsonObject>* Wv = nullptr;
				if (J->TryGetObjectField(TEXT("wave"), Wv) && Wv)
				{
					double N = 0.0;
					if ((*Wv)->TryGetNumberField(TEXT("fps"), N)) D->WaveFps = float(N);
					if ((*Wv)->TryGetNumberField(TEXT("ripple"), N)) D->WaveRipple = float(N);
					const TArray<TSharedPtr<FJsonValue>>* Xs = nullptr;
					if ((*Wv)->TryGetArrayField(TEXT("x"), Xs)) for (const TSharedPtr<FJsonValue>& Vv : *Xs) D->WaveX.Add(float(Vv->AsNumber()));
					const TArray<TSharedPtr<FJsonValue>>* Dr = nullptr;
					if ((*Wv)->TryGetArrayField(TEXT("drop"), Dr))
					{
						for (const TSharedPtr<FJsonValue>& Row : *Dr)
						{
							TArray<float>& Out = D->WaveDrop.AddDefaulted_GetRef();
							for (const TSharedPtr<FJsonValue>& Vv : Row->AsArray()) Out.Add(float(Vv->AsNumber()));
						}
					}
					D->bWave = D->WaveX.Num() > 1 && D->WaveDrop.Num() > 0;
				}
			}
			for (TPair<FString, FOutlawsMeshSection>& Kv : D->Sections)
			{
				for (FVector& P : Kv.Value.Positions) P *= Scale;
				SmoothItemNormals(Kv.Value);
			}
			UE_LOG(LogOutlaws, Log, TEXT("OLRT item model %s: x%.2f%s"), *Stem, Scale, D->bFlat ? TEXT(", lies flat") : TEXT(""));
		}
		GItemModels.Add(Stem, D);
		return D.Get();
	}
}

// (STUTTER) set by SpawnGroundItem round its UseItemModel: a dropped thing turned by its component, not in its vertices,
// so every drop of a model at a light level is a copy of the first one's sections (the build and the ink shell were
// 18-20 ms a dropped gun on the Quest)
bool GOlItemModelByTurn = false;

namespace
{
	struct FItemTemplate
	{
		TArray<FProcMeshSection> Sections;
		TArray<TWeakObjectPtr<UMaterialInterface>> Mats;
		int32 Own = -1;           // the model's own sections when an ink shell follows them
		bool bNoLod = false;
	};
	TMap<FString, FItemTemplate> GItemTemplates;

	// (CPU2 e) a model's template at another light, its own sections' shade scaled to this light (the shade is the light's
	// share times the cel tone, so the scale is exact; the ink shell's sections stay black): the first drop of a gun at any
	// light is a copy, given one at any light (PrebuildForPlay makes one a gun at light 31)
	const FItemTemplate* RelitTemplate(const FString& ModelName, int32 Light)
	{
		for (int32 From = 31; From > 0; --From)
		{
			if (From == Light) continue;
			const FItemTemplate* Src = GItemTemplates.Find(FString::Printf(TEXT("%s#%d"), *ModelName, From));
			if (!Src) continue;
			FItemTemplate T = *Src;
			const float Scale = float(Light) / float(From);
			for (int32 i = 0; i < T.Sections.Num() && (T.Own < 0 || i < T.Own); ++i)
			{
				for (FProcMeshVertex& V : T.Sections[i].ProcVertexBuffer)
				{
					V.Color.R = uint8(FMath::Clamp(FMath::RoundToInt(V.Color.R * Scale), 0, 255));
					V.Color.G = uint8(FMath::Clamp(FMath::RoundToInt(V.Color.G * Scale), 0, 255));
					V.Color.B = uint8(FMath::Clamp(FMath::RoundToInt(V.Color.B * Scale), 0, 255));
				}
			}
			return &GItemTemplates.Add(FString::Printf(TEXT("%s#%d"), *ModelName, Light), MoveTemp(T));
		}
		return nullptr;
	}
}

bool AOutlawsLevelActor::UseItemModel(AOutlawsSpriteActor* S, const FString& Anim, float Yaw)
{
	static const bool bSprites = OLFlags::Param(TEXT("OLSpriteItems"));
	if (bSprites || !S || !S->GetRootComponent())
	{
		return false;
	}
	const FString ModelName = FPaths::GetBaseFilename(Anim).ToUpper();
	const FItemModelData* D = ItemModelFor(ModelName);
	if (!D)
	{
		return false;
	}
	if (!DebugModelAt.Contains(ModelName)) DebugModelAt.Add(ModelName, S->GetActorLocation());     // (tests: -OLLookAtModel=)
	// flat things lie on their face (the sprite side up), everything turned by the object's yaw, standing on the
	// sprite's foot point.  Scenery (trees, shrubs, cacti) turns by where it stands: every copy a different face; the
	// rocks too (user 2026-10-02: all alike in 3D), their models made without the flag
	// (user 2026-10-03: "some still look a bit uniform", a round rock turned looks the same) each rock is also tipped up
	// to 14 degrees and stretched or squashed by up to about a sixth along each axis, all from where it stands
	FQuat Tip = FQuat::Identity;
	FVector Shape = FVector::OneVector;
	if (D->bAnyYaw || ModelName.StartsWith(TEXT("ROCK")))
	{
		const FVector At = S->GetActorLocation();
		const uint32 H = HashCombine(GetTypeHash(FMath::RoundToInt(At.X)), GetTypeHash(FMath::RoundToInt(At.Y)));
		Yaw = float((H * 2654435761u) % 3600u) * 0.1f;
		if (ModelName.StartsWith(TEXT("ROCK")))
		{
			FRandomStream Rs(int32(H ^ 0x5bd1e995u));
			const FVector Axis = FVector(Rs.FRandRange(-1.f, 1.f), Rs.FRandRange(-1.f, 1.f), 0.0).GetSafeNormal();
			if (!Axis.IsNearlyZero()) Tip = FQuat(Axis, FMath::DegreesToRadians(Rs.FRandRange(3.f, 14.f)));
			Shape = FVector(Rs.FRandRange(0.85f, 1.18f), Rs.FRandRange(0.85f, 1.18f), Rs.FRandRange(0.82f, 1.15f));
		}
	}
	S->ModelYaw = Yaw;
	// (STUTTER) a drop: turned by its component, its sections a copy of the first drop's
	bool bByTurn = GOlItemModelByTurn && !D->bAnyYaw && !ModelName.StartsWith(TEXT("ROCK")) && !D->bFlicker && !D->bWave && !D->bBend && !D->bHang && !D->bFlames;
	for (const TPair<FString, FOutlawsMeshSection>& Kv : D->Sections)
	{
		bByTurn &= !Kv.Key.StartsWith(TEXT("glass"), ESearchCase::IgnoreCase) && !Kv.Key.Equals(TEXT("flame"), ESearchCase::IgnoreCase);
	}
	const float CompYaw = bByTurn ? Yaw : 0.f;
	if (bByTurn) Yaw = 0.f;
	const FString TemplateKey = FString::Printf(TEXT("%s#%d"), *ModelName, FMath::Clamp(S->LightLevel, 0, 31));
	static const bool bNoInkT = OLFlags::Param(TEXT("OLNoItemInk"));
	const FItemTemplate* Found = bByTurn ? GItemTemplates.Find(TemplateKey) : nullptr;
	if (bByTurn && !Found) Found = RelitTemplate(ModelName, FMath::Clamp(S->LightLevel, 0, 31));
	if (const FItemTemplate* T = Found;
		T && !T->Mats.ContainsByPredicate([](const TWeakObjectPtr<UMaterialInterface>& W) { return !W.IsValid(); }))
	{
		UProceduralMeshComponent* M = NewObject<UProceduralMeshComponent>(S, NAME_None, RF_Transient);
		M->bUseAsyncCooking = false;
		M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		M->SetCastShadow(false);
		M->SetUsingAbsoluteRotation(true);
		M->SetUsingAbsoluteScale(true);
		M->SetupAttachment(S->GetRootComponent());
		M->RegisterComponent();
		M->SetRelativeLocation(FVector::ZeroVector);
		M->SetWorldRotation(FRotator(0.0, CompYaw, 0.0));
		for (int32 i = 0; i < T->Sections.Num(); ++i)
		{
			M->SetProcMeshSection(i, T->Sections[i]);
			M->SetMaterial(i, T->Mats[i].Get());
		}
		if (T->Own >= 0) OutlawsInk::NoteShelled(M, T->Own);
		if (!bNoInkT)
		{
			M->SetRenderCustomDepth(true);
			M->SetCustomDepthStencilValue(4);
		}
		S->ShowModel(M);
		S->ModelStem = FPaths::GetBaseFilename(Anim).ToUpper();
		S->ModelScale = D->WorldScale;
		if (!T->bNoLod)
		{
			const FBoxSphereBounds Lb = M->CalcBounds(FTransform::Identity);
			const float At = FMath::Max(400.f, 40.f * 2.f * float(Lb.BoxExtent.GetMax()));
			S->SetLod(At, 3.f * At);
		}
		if (D->Spin != 0.f) SpinningModels.Add(TPair<TWeakObjectPtr<UProceduralMeshComponent>, float>(M, D->Spin));
		++NumItemModels;
		return true;
	}
	const FQuat Lay = D->bFlat ? FQuat(FVector::ForwardVector, HALF_PI) : FQuat::Identity;
	const FQuat Turn = Tip * FQuat(FVector::UpVector, FMath::DegreesToRadians(Yaw)) * Lay;
	double MinZ = TNumericLimits<double>::Max();
	FBox B(ForceInit);
	for (const TPair<FString, FOutlawsMeshSection>& Kv : D->Sections)
	{
		for (const FVector& P : Kv.Value.Positions)
		{
			const FVector R = Turn.RotateVector(P * Shape);
			MinZ = FMath::Min(MinZ, R.Z);
			B += R;
		}
	}
	const FVector Offset(D->bKeepOrigin ? 0.0 : -B.GetCenter().X, D->bKeepOrigin ? 0.0 : -B.GetCenter().Y, -MinZ + D->Hover);
	const FVector LightDir = FVector(0.35, -0.45, 0.82).GetSafeNormal();
	const float Bright = FMath::Clamp(S->LightLevel, 0, 31) / 31.f;
	UProceduralMeshComponent* M = NewObject<UProceduralMeshComponent>(S, NAME_None, RF_Transient);
	M->bUseAsyncCooking = false;
	M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	M->SetCastShadow(false);
	M->SetUsingAbsoluteRotation(true);
	M->SetUsingAbsoluteScale(true);
	M->SetupAttachment(S->GetRootComponent());
	M->RegisterComponent();
	M->SetRelativeLocation(FVector::ZeroVector);
	M->SetWorldRotation(FRotator(0.0, CompYaw, 0.0));
	int32 Index = 0;
	FWavingModel Wave;                        // (a waving cloth keeps its rest vertices)
	bool bNoLod = D->bHang;                   // (CPU1 object LOD: not for hanging lamps, glass or flames: their looks cannot dither)
	for (const TPair<FString, FOutlawsMeshSection>& Kv : D->Sections)
	{
		// both faces of every triangle (the models are not wound consistently), each shaded by its own normal against
		// a fixed key light so the shape reads in the unlit world material, as the item test does
		const FOutlawsMeshSection& Sec = Kv.Value;
		const int32 N = Sec.Positions.Num();
		TArray<FVector> Pos;
		TArray<FVector> Nrm;
		TArray<FVector2D> UV;
		TArray<FLinearColor> Col;
		TArray<int32> Tri;
		Pos.Reserve(N * 2);
		for (int32 Side = 0; Side < 2; ++Side)
		{
			for (int32 i = 0; i < N; ++i)
			{
				const FVector Nw = Turn.RotateVector((Sec.Normals[i] / Shape).GetSafeNormal()) * (Side ? -1.0 : 1.0);
				// cel-shaded as the held gear is (user 2026-09-29): a lit tone and a shade tone with a narrow step
				// between, not a smooth ramp; the ink lines come from the post-process (custom stencil 4 below)
				const float Lit = FMath::SmoothStep(0.12f, 0.24f, float(FVector::DotProduct(Nw, LightDir)));
				const float Shade = Bright * FMath::Lerp(0.66f, 1.f, Lit);
				Pos.Add(Turn.RotateVector(Sec.Positions[i] * Shape) + Offset);
				Nrm.Add(Nw);
				UV.Add(Sec.UVs[i]);
				Col.Add(FLinearColor(Shade, Shade, Shade, 1.f));
			}
		}
		for (int32 t = 0; t + 2 < Sec.Triangles.Num(); t += 3)
		{
			Tri.Append({ Sec.Triangles[t], Sec.Triangles[t + 1], Sec.Triangles[t + 2] });
			Tri.Append({ N + Sec.Triangles[t], N + Sec.Triangles[t + 2], N + Sec.Triangles[t + 1] });
		}
		const bool bFlame = Kv.Key.Equals(TEXT("flame"), ESearchCase::IgnoreCase);
		if (bFlame) for (FLinearColor& C0 : Col) C0 = FLinearColor::White;           // (a flame shines: no shade)
		M->CreateMeshSection_LinearColor(Index, Pos, Tri, Nrm, UV, Col, TArray<FProcMeshTangent>(), false);
		if (D->bFlicker && Kv.Key.Equals(D->FlickSection, ESearchCase::IgnoreCase))
		{
			FFlickerModel& Fm = FlickerModels.AddDefaulted_GetRef();
			Fm.Mesh = M;
			Fm.Section = Index;
			Fm.Base = Pos;
			Fm.Nrm = Nrm;
			Fm.UV = UV;
			Fm.Col = Col;
			FBox Bx(ForceInit);
			for (const FVector& P0 : Pos) Bx += P0;
			Fm.Root = FVector(Bx.GetCenter().X, Bx.GetCenter().Y, Bx.Min.Z);      // it grows from its foot
			Fm.Fps = D->FlickFps;
			Fm.Scale = D->FlickScale;
			Fm.Bright = D->FlickBright;
			const FVector At = S->GetActorLocation();
			Fm.Phase = float((HashCombine(GetTypeHash(FMath::RoundToInt(At.X)), GetTypeHash(FMath::RoundToInt(At.Y))) % 1000u)) / 1000.f;
		}
		if (D->bWave || D->bBend)
		{
			Wave.Base.Add(Pos);
			Wave.Nrm.Add(Nrm);
			Wave.UV.Add(UV);
			Wave.Col.Add(Col);
		}
		// a section whose OBJ material is "glass" is drawn see-through (the man in the bottle, user 2026-10-01): the held
		// ghost's translucent unlit material, its texture the item's, a third opaque
		const bool bGlass = Kv.Key.StartsWith(TEXT("glass"), ESearchCase::IgnoreCase);
		// a "flame" section: the same unlit material, opaque where its texture is (bright whatever the light)
		TObjectPtr<UMaterialInstanceDynamic>& Mat = ItemModelMats.FindOrAdd(bGlass ? TEXT("glass:") + Sec.Texture : bFlame ? TEXT("flame:") + Sec.Texture : Sec.Texture);
		if (!Mat && (bGlass || bFlame))
		{
			if (UMaterialInterface* Ghost = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_HeldGhost.M_OLR_HeldGhost")))
			{
				Mat = UMaterialInstanceDynamic::Create(Ghost, this);
				if (UTexture2D* T = OutlawsObj::LoadTextureFile(Sec.Texture)) Mat->SetTextureParameterValue(TEXT("Tex"), T);
				Mat->SetScalarParameterValue(TEXT("Opacity"), bGlass ? 0.35f : 1.f);
			}
		}
		bNoLod |= bGlass || bFlame;
		if (!Mat && WorldMaskedMaterial)
		{
			// (CPU1 object LOD) M_OLR_ItemModel: the world's masked material with the LOD dither (the same picture undithered)
			static TWeakObjectPtr<UMaterialInterface> ItemBase;
			if (!ItemBase.IsValid()) ItemBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_ItemModel.M_OLR_ItemModel"));
			bNoLod |= !ItemBase.IsValid();
			Mat = UMaterialInstanceDynamic::Create(ItemBase.IsValid() ? ItemBase.Get() : WorldMaskedMaterial.Get(), this);
			if (UTexture2D* T = OutlawsObj::LoadTextureFile(Sec.Texture)) Mat->SetTextureParameterValue(TEXT("Tex"), T);
		}
		M->SetMaterial(Index, Mat);
		if (D->bFlames && bFlame) M->SetMeshSectionVisible(Index, false);      // (the game's own fire burns instead)
		if (D->bFlames && bGlass)
		{
			// the fire in this glass: the fire sprite on a card, its foot a little under the glass's middle, as tall as
			// half the glass (or most of its width), on the model (it swings with it), turned to the eye (UpdateLamps)
			if (!LampFireMid) LampFireMid = MakeFireLook(this, LampFireCells, LampFireFps, LampFireAspect);
			if (LampFireMid && GetSpriteQuad())
			{
				FBox Gb(ForceInit);
				for (const FVector& P0 : Pos) Gb += P0;
				const FVector Ge = Gb.GetSize();
				const float Hf = FMath::Clamp(float(FMath::Min(Ge.Z * 0.5, FMath::Max(Ge.X, Ge.Y) * 0.8)), 3.f, 40.f);
				UStaticMeshComponent* Q = NewObject<UStaticMeshComponent>(S, NAME_None, RF_Transient);
				Q->SetStaticMesh(GetSpriteQuad());
				Q->SetMaterial(0, LampFireMid);
				Q->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				Q->SetCastShadow(false);
				Q->SetupAttachment(M);
				Q->RegisterComponent();
				Q->SetUsingAbsoluteRotation(true);
				Q->SetUsingAbsoluteScale(true);
				Q->SetRelativeLocation(FVector(Gb.GetCenter().X, Gb.GetCenter().Y, Gb.GetCenter().Z - Hf * 0.45));
				Q->SetWorldScale3D(FVector(1.0, Hf * LampFireAspect, Hf));
				Q->SetTranslucentSortPriority(1);
				LampFlames.Add({ Q, M, Index });
			}
		}
		++Index;
	}
	if (D->bHang)
	{
		// hung from its top: the pendulum's pivot and length from the model as placed
		FBox Lb(ForceInit);
		for (const TPair<FString, FOutlawsMeshSection>& Kv : D->Sections) for (const FVector& P0 : Kv.Value.Positions) Lb += Turn.RotateVector(P0 * Shape) + Offset;
		FHangModel& Hm = HangModels.AddDefaulted_GetRef();
		Hm.Mesh = M;
		Hm.Stem = FPaths::GetBaseFilename(Anim).ToUpper();
		Hm.PivotLocal = FVector(Lb.GetCenter().X, Lb.GetCenter().Y, Lb.Max.Z);
		Hm.Len = FMath::Max(15.f, float(Lb.Max.Z - Lb.GetCenter().Z));
		Hm.bBulbs = D->bBulbs;
		int32 Si = 0;
		for (const TPair<FString, FOutlawsMeshSection>& Kv : D->Sections)
		{
			if (Kv.Key.StartsWith(TEXT("glass"), ESearchCase::IgnoreCase)) Hm.Glass.Add(Si);
			++Si;
		}
	}
	static const bool bNoInk = OLFlags::Param(TEXT("OLNoItemInk"));
	if (!bNoInk)
	{
		M->SetRenderCustomDepth(true);          // the ink outline's world-item stencil (M_OLR_InkOutline, stencil 4)
		M->SetCustomDepthStencilValue(4);
		// the headset has no post process: a shell instead (thicker than the held gear's: these are seen from further)
		if (OutlawsInk::Wanted())
		{
			if (!ItemInk)
			{
				// (CPU1 object LOD) M_OLR_ItemInk: M_OLR_Held with the LOD dither, so the shell fades with its model
				UMaterialInterface* Held = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_ItemInk.M_OLR_ItemInk"));
				if (!Held) Held = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_Held.M_OLR_Held"));
				TArray<FColor> Px;
				Px.Init(FColor(10, 8, 7), 4);
				if (Held && (ItemInk = UMaterialInstanceDynamic::Create(Held, this)) != nullptr)
				{
					if (UTexture2D* T = OutlawsTextures::MakeTextureBGRA(2, 2, Px, false, TF_Nearest, TA_Clamp)) ItemInk->SetTextureParameterValue(TEXT("Tex"), T);
					ItemInk->SetScalarParameterValue(TEXT("NearPull"), 0.f);      // at the world's depth, not over it as held gear
				}
			}
			ShellItemModel(M, FPaths::GetBaseFilename(Anim).ToUpper());
		}
	}
	S->ShowModel(M);
	S->ModelStem = FPaths::GetBaseFilename(Anim).ToUpper();
	S->ModelScale = D->WorldScale;
	if (!bNoLod)
	{
		// (CPU1 object LOD, user 2026-10-04) the sprite from 12 m for a 30 cm object, further for a bigger one (by its
		// bounds), at least 4 m; gone at three times that (never for keys and the like: PinPickupLod)
		const FBoxSphereBounds Lb = M->CalcBounds(FTransform::Identity);
		const float Size = 2.f * float(Lb.BoxExtent.GetMax());
		const float At = FMath::Max(400.f, 40.f * Size);
		S->SetLod(At, 3.f * At);
	}
	if (D->Spin != 0.f)
	{
		SpinningModels.Add(TPair<TWeakObjectPtr<UProceduralMeshComponent>, float>(M, D->Spin));
	}
	if (D->bBend)
	{
		FBendModel& Bm = BendModels.AddDefaulted_GetRef();
		Bm.Mesh = M;
		Bm.Stem = FPaths::GetBaseFilename(Anim).ToUpper();
		Bm.Base = Wave.Base;
		Bm.Nrm = Wave.Nrm;
		Bm.UV = Wave.UV;
		Bm.Col = Wave.Col;
		float Hmax = 1.f;
		for (const TArray<FVector>& Sb : Bm.Base) for (const FVector& P0 : Sb) Hmax = FMath::Max(Hmax, float(P0.Z));
		Bm.Height = Hmax;
		Bm.Fps = D->BendFps;
		Bm.Top = D->BendTop;
		Bm.Flutter = D->BendFlutter;
		const FVector At = S->GetActorLocation();
		Bm.Phase = float((HashCombine(GetTypeHash(FMath::RoundToInt(At.X)), GetTypeHash(FMath::RoundToInt(At.Y))) % 1000u)) / 1000.f;
	}
	if (D->bWave)
	{
		// the cloth along the model's X from the pole at its origin, its face across Y (the sprite's plane), turned
		Wave.Mesh = M;
		Wave.Dir = Turn.RotateVector(FVector::ForwardVector);
		Wave.Side = Turn.RotateVector(FVector::RightVector);
		Wave.PoleR = D->PoleR * D->WorldScale;
		Wave.Fps = D->WaveFps;
		Wave.Ripple = D->WaveRipple;
		Wave.X = D->WaveX;
		Wave.Drop = D->WaveDrop;
		const FVector At = S->GetActorLocation();
		Wave.Phase = float((HashCombine(GetTypeHash(FMath::RoundToInt(At.X)), GetTypeHash(FMath::RoundToInt(At.Y))) % 1000u)) / 1000.f;
		WavingModels.Add(MoveTemp(Wave));
		WavingStems.Add(FPaths::GetBaseFilename(Anim).ToUpper());
	}
	if (bByTurn)
	{
		// the first drop of this model at this light: its sections (and ink shell) kept for the next
		FItemTemplate& T = GItemTemplates.Add(TemplateKey);
		T.bNoLod = bNoLod;
		T.Own = M->GetNumSections() > Index ? Index : -1;
		for (int32 i = 0; i < M->GetNumSections(); ++i)
		{
			if (const FProcMeshSection* Sec = M->GetProcMeshSection(i)) T.Sections.Add(*Sec);
			T.Mats.Add(M->GetMaterial(i));
		}
		if (T.Sections.Num() != T.Mats.Num()) GItemTemplates.Remove(TemplateKey);
	}
	++NumItemModels;
	return true;
}

bool AOutlawsLevelActor::ItemKeyAnim(const FString& Stem, int32 Chor, FOutlawsKeyAnim& Out) const
{
	const FItemModelData* D = ItemModelFor(Stem);
	const FOutlawsKeyAnim* KA = D ? D->Anims.Find(Chor) : nullptr;
	if (!KA) return false;
	Out = *KA;
	return true;
}

namespace
{
	int32 ItemSectionIndex(const FItemModelData* D, const FString& Name)
	{
		int32 i = 0;
		for (const TPair<FString, FOutlawsMeshSection>& Kv : D->Sections)
		{
			if (Kv.Key.Equals(Name, ESearchCase::IgnoreCase)) return i;
			++i;
		}
		return -1;
	}
}

void AOutlawsLevelActor::ShellItemModel(UProceduralMeshComponent* M, const FString& Stem)
{
	if (!M || !ItemInk || !OutlawsInk::Wanted()) return;
	const FItemModelData* D = ItemModelFor(Stem);
	// the parts kept out of the ink (a flame) hidden while the shell is made from the rest (a hidden part makes none)
	TArray<int32> Out;
	if (D)
	{
		for (const FString& N : D->NoInk)
		{
			const int32 i = ItemSectionIndex(D, N);
			if (i >= 0 && M->IsMeshSectionVisible(i))
			{
				M->SetMeshSectionVisible(i, false);
				Out.Add(i);
			}
		}
	}
	OutlawsInk::AddShell(M, 0.6f, ItemInk);
	for (int32 i : Out) M->SetMeshSectionVisible(i, true);
}

void AOutlawsLevelActor::HideItemSections(UProceduralMeshComponent* M, const FString& Stem, const TArray<FString>& Names)
{
	const FItemModelData* D = ItemModelFor(Stem);
	if (!M || !D) return;
	for (const FString& N : Names)
	{
		const int32 i = ItemSectionIndex(D, N);
		if (i < 0) continue;
		M->SetMeshSectionVisible(i, false);
		FlickerModels.RemoveAll([M, i](const FFlickerModel& F) { return F.Mesh.Get() == M && F.Section == i; });
	}
	ShellItemModel(M, Stem);
}

void AOutlawsLevelActor::SwingHanging(FHangModel& Hm, const FVector& Dir, float RadPerSec)
{
	FVector2D D(Dir.X, Dir.Y);
	if (!D.Normalize()) return;
	Hm.Vel += D * RadPerSec;
	SetActorTickEnabled(true);
	if (UOutlawsCoop::IsActive(this)) UOutlawsCoop::NoteWorldBit(this, UOutlawsCoop::BitSwing, int32(&Hm - HangModels.GetData()), 0, Dir, RadPerSec);
}

void AOutlawsLevelActor::BreakShade(FHangModel& Hm, int32 Section)
{
	UProceduralMeshComponent* M = Hm.Mesh.Get();
	if (!M || !M->IsMeshSectionVisible(Section)) return;
	const FProcMeshSection* Sec = M->GetProcMeshSection(Section);
	const FVector At = Sec ? M->GetComponentTransform().TransformPosition(Sec->SectionLocalBox.GetCenter()) : M->Bounds.Origin;
	M->SetMeshSectionVisible(Section, false);
	ShellItemModel(M, Hm.Stem);
	PlaySoundAt(TEXT("bottle"), At);
	SpawnSparks(At, 5);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT %s: a shade broken (section %d)"), *Hm.Stem, Section);
	if (UOutlawsCoop::IsActive(this)) UOutlawsCoop::NoteWorldBit(this, UOutlawsCoop::BitShade, int32(&Hm - HangModels.GetData()), Section, FVector::ZeroVector, 0.f);
}

void AOutlawsLevelActor::ShotNearHanging(const FVector& From, const FVector& To)
{
	const FVector Dir = (To - From).GetSafeNormal();
	for (FHangModel& Hm : HangModels)
	{
		UProceduralMeshComponent* M = Hm.Mesh.Get();
		if (!M || !M->IsVisible()) continue;
		const FVector C = M->Bounds.Origin;
		if (FVector::Dist(FMath::ClosestPointOnSegment(C, From, To), C) > M->Bounds.SphereRadius * 0.7f) continue;
		if (Hm.bBulbs)
		{
			// the shade the shot goes through, the nearest to its line
			const FTransform X = M->GetComponentTransform();
			int32 Best = -1;
			double BestD = 0.0;
			for (const int32 i : Hm.Glass)
			{
				const FProcMeshSection* Sec = i < M->GetNumSections() ? M->GetProcMeshSection(i) : nullptr;
				if (!Sec || !M->IsMeshSectionVisible(i)) continue;
				const FVector Gc = X.TransformPosition(Sec->SectionLocalBox.GetCenter());
				const double Dd = FVector::Dist(FMath::ClosestPointOnSegment(Gc, From, To), Gc);
				if (Dd < Sec->SectionLocalBox.GetExtent().GetMax() * 1.1 && (Best < 0 || Dd < BestD))
				{
					Best = i;
					BestD = Dd;
				}
			}
			if (Best >= 0) BreakShade(Hm, Best);
		}
		SwingHanging(Hm, Dir, 1.2f * FMath::Sqrt(50.f / Hm.Len));
	}
}

void AOutlawsLevelActor::HandNearHanging(const FVector& At, const FVector& Vel)
{
	const float Speed = float(Vel.Size());
	for (FHangModel& Hm : HangModels)
	{
		UProceduralMeshComponent* M = Hm.Mesh.Get();
		if (!M || !M->IsVisible() || Hm.Cool > 0.f) continue;
		if (M->Bounds.GetBox().ComputeSquaredDistanceToPoint(At) > FMath::Square(4.0)) continue;
		Hm.Cool = 0.3f;
		if (Hm.bBulbs && Speed > OutlawsFeel::HitSpeedHard)
		{
			const FTransform X = M->GetComponentTransform();
			for (const int32 i : Hm.Glass)
			{
				const FProcMeshSection* Sec = i < M->GetNumSections() ? M->GetProcMeshSection(i) : nullptr;
				if (!Sec || !M->IsMeshSectionVisible(i)) continue;
				if (FVector::Dist(X.TransformPosition(Sec->SectionLocalBox.GetCenter()), At) < Sec->SectionLocalBox.GetExtent().GetMax() + 4.0)
				{
					BreakShade(Hm, i);
					break;
				}
			}
		}
		SwingHanging(Hm, Vel, FMath::Min(2.f, 0.5f * Speed / Hm.Len));
	}
}

void AOutlawsLevelActor::UpdateLamps(float Dt)
{
	if (LampFlames.Num() == 0 && HangModels.Num() == 0) return;
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	const FVector Eye = PC && PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : FVector::ZeroVector;
	const float Now = World ? float(World->GetTimeSeconds()) : 0.f;
	// the hanging lamps: pendulums from their tops, damped, never more than 40 degrees out
	for (int32 i = HangModels.Num() - 1; i >= 0; --i)
	{
		FHangModel& Hm = HangModels[i];
		UProceduralMeshComponent* M = Hm.Mesh.Get();
		USceneComponent* Parent = M ? M->GetAttachParent() : nullptr;
		if (!M || !Parent)
		{
			HangModels.RemoveAt(i);
			continue;
		}
		Hm.Cool = FMath::Max(0.f, Hm.Cool - Dt);
		if (Hm.Tilt.SizeSquared() < 1e-6 && Hm.Vel.SizeSquared() < 1e-5)
		{
			if (Hm.bMoved)
			{
				Hm.Tilt = Hm.Vel = FVector2D::ZeroVector;
				M->SetRelativeLocation(FVector::ZeroVector);
				M->SetWorldRotation(FRotator::ZeroRotator);
				Hm.bMoved = false;
			}
			continue;
		}
		const float W2 = 980.f / Hm.Len, Damp = 2.f * 0.12f * FMath::Sqrt(W2);
		float Left = FMath::Min(Dt, 0.1f);
		while (Left > 0.f)
		{
			const float St = FMath::Min(Left, 1.f / 120.f);
			Hm.Vel += (-W2 * Hm.Tilt - Damp * Hm.Vel) * St;
			Hm.Tilt += Hm.Vel * St;
			Left -= St;
		}
		if (Hm.Tilt.Size() > 0.7f) Hm.Tilt = Hm.Tilt.GetSafeNormal() * 0.7f;
		const float A = float(Hm.Tilt.Size());
		const FVector2D Dd = A > 1e-5f ? Hm.Tilt / A : FVector2D(1.0, 0.0);
		const FVector Down(FMath::Sin(A) * Dd.X, FMath::Sin(A) * Dd.Y, -FMath::Cos(A));
		const FQuat Q = FQuat::FindBetweenNormals(FVector::DownVector, Down);
		const FVector Base = Parent->GetComponentLocation();
		const FVector Pivot = Base + Hm.PivotLocal;
		M->SetWorldLocationAndRotation(Pivot + Q.RotateVector(Base - Pivot), Q);
		Hm.bMoved = true;
		// swung hard into a bad guy (backlog 1e, user 2026-10-01: a light hazard): a little hurt and a knock its way, once a
		// swing
		Hm.HitCool = FMath::Max(0.f, Hm.HitCool - Dt);
		if (A > 0.2f && Hm.HitCool <= 0.f)
		{
			const FBox Lb = M->Bounds.GetBox();
			const FVector Way = FVector(Hm.Vel.X, Hm.Vel.Y, 0.0).GetSafeNormal();
			for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : Enemies)
			{
				AOutlawsEnemy* E = Wk.Get();
				if (!E || E->IsDead() || E->IsDormant() || E->bHeldByPlayer || !Lb.Intersect(E->GetCapsuleComponent()->Bounds.GetBox())) continue;
				E->TakeHit(0.3f, Lb.GetClosestPointTo(E->GetActorLocation()), Way.IsNearlyZero() ? FVector::ForwardVector : Way, Lb.GetCenter(), false, 1.f);
				Hm.HitCool = 1.f;
				UE_LOG(LogOutlaws, Log, TEXT("OLRT a swinging %s strikes %s"), *Hm.Stem, *E->GetName());
				break;
			}
		}
	}
	// the lamps' fire: the sprite's drawings at its own rate, each card turned to the eye, out with its glass
	if (LampFireMid && LampFireCells.Num() > 0)
	{
		LampFireMid->SetVectorParameterValue(TEXT("UVOffset"), LampFireCells[int32(FMath::FloorToFloat(Now * LampFireFps)) % LampFireCells.Num()]);
	}
	for (int32 i = LampFlames.Num() - 1; i >= 0; --i)
	{
		FLampFlame& F = LampFlames[i];
		UStaticMeshComponent* Q = F.Quad.Get();
		UProceduralMeshComponent* M = F.Mesh.Get();
		if (!Q || !M)
		{
			LampFlames.RemoveAt(i);
			continue;
		}
		const bool bOn = M->IsVisible() && F.Section < M->GetNumSections() && M->IsMeshSectionVisible(F.Section);
		if (Q->IsVisible() != bOn) Q->SetVisibility(bOn);
		if (!bOn) continue;
		const FVector To = Eye - Q->GetComponentLocation();
		if (To.SizeSquared() > FMath::Square(4000.0)) continue;
		Q->SetWorldRotation(FRotator(0.0, FMath::RadiansToDegrees(FMath::Atan2(To.Y, To.X)), 0.0));
	}
}

void AOutlawsLevelActor::UpdateWavingModels(float Dt)
{
	// the flags' cloth as their sprite drew it (CONFLAG/UNFLAG chor 0: 8 frames at 10 fps, one wave a loop hoist to
	// fly): the centre line's measured sag, eased between the frames, less the first frame's (the model is built from
	// it), and a ripple across the face growing toward the fly end, a wave a loop.  20 times a second, near the player
	if (WavingModels.Num() == 0 && FlickerModels.Num() == 0 && BendModels.Num() == 0) return;
	// BUG-SRGBREST (28 Oct, after BUG-SCROLLSRGB): the colours written as the build wrote them, with no sRGB conversion:
	// UpdateMeshSection_LinearColor converts by default where CreateMeshSection_LinearColor does not, so a waving cloth, a
	// bending plant or a flickering flame drew brighter than its sector after its first move.  -OLOldSRGBRest: converted as before
	static const bool bOldSrgb = OLFlags::Param(TEXT("OLOldSRGBRest"));
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	const FVector Eye = PC && PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : FVector::ZeroVector;
	const float Now = World ? float(World->GetTimeSeconds()) : 0.f;
	for (int32 i = WavingModels.Num() - 1; i >= 0; --i)
	{
		FWavingModel& W = WavingModels[i];
		UProceduralMeshComponent* M = W.Mesh.Get();
		if (!M)
		{
			WavingModels.RemoveAt(i);
			if (WavingStems.IsValidIndex(i)) WavingStems.RemoveAt(i);
			continue;
		}
		W.Wait -= Dt;
		if (W.Wait > 0.f || !M->IsVisible() || FVector::DistSquared(M->GetComponentLocation(), Eye) > FMath::Square(6000.0)) continue;
		W.Wait = 0.05f;
		const int32 NF = W.Drop.Num();
		const float Period = NF / FMath::Max(1.f, W.Fps);
		const float Ph = FMath::Frac(Now / Period + W.Phase);
		const float F = Ph * NF;
		const int32 F0 = FMath::FloorToInt(F) % NF, F1 = (F0 + 1) % NF;
		const float Fr = 0.5f - 0.5f * FMath::Cos(PI * (F - FMath::FloorToFloat(F)));
		const float L = W.X.Last();
		auto DropAt = [&W](int32 Fi, float X)
		{
			const TArray<float>& Row = W.Drop[Fi];
			if (X <= W.X[0]) return Row[0] * FMath::Max(0.f, X / W.X[0]);
			for (int32 k = 1; k < W.X.Num(); ++k)
			{
				if (X <= W.X[k]) return FMath::Lerp(Row[k - 1], Row[k], (X - W.X[k - 1]) / FMath::Max(1e-3f, W.X[k] - W.X[k - 1]));
			}
			return Row.Last();
		};
		for (int32 s = 0; s < W.Base.Num(); ++s)
		{
			TArray<FVector> Pos = W.Base[s];
			for (FVector& P : Pos)
			{
				const float Out = FMath::Abs(float(FVector::DotProduct(FVector(P.X, P.Y, 0.0), W.Dir))) - W.PoleR;
				if (Out <= 0.3f) continue;                       // the pole stands still
				const float Sag = FMath::Lerp(DropAt(F0, Out), DropAt(F1, Out), Fr) - DropAt(0, Out);
				const float Rip = W.Ripple * FMath::Clamp(Out / L, 0.f, 1.f) * FMath::Sin(2.f * PI * (Ph - Out / L));
				P += W.Side * Rip - FVector::UpVector * Sag;
			}
			M->UpdateMeshSection_LinearColor(s, Pos, W.Nrm[s], W.UV[s], W.Col[s], TArray<FProcMeshTangent>(), bOldSrgb);
		}
		if (WavingStems.IsValidIndex(i)) ShellItemModel(M, WavingStems[i]);      // (the ink line follows the cloth)
	}
	// plants bending as their sprites swayed: the step's top shift eased to the next's, times (height / top)^2 so the
	// base stays put; the level's one wind way (the sprite's screen x); a small flutter leaf by leaf.  15 times a
	// second within 30 m; the ink shell follows within 12 m
	static const FVector Wind = FVector(0.8, 0.6, 0.0).GetSafeNormal();
	const FVector Across(-Wind.Y, Wind.X, 0.0);
	for (int32 i = BendModels.Num() - 1; i >= 0; --i)
	{
		FBendModel& Bm = BendModels[i];
		UProceduralMeshComponent* M = Bm.Mesh.Get();
		if (!M)
		{
			BendModels.RemoveAt(i);
			continue;
		}
		Bm.Wait -= Dt;
		const double D2 = FVector::DistSquared(M->GetComponentLocation(), Eye);
		if (Bm.Wait > 0.f || !M->IsVisible() || D2 > FMath::Square(3000.0)) continue;
		Bm.Wait = 1.f / 15.f;
		const int32 N = Bm.Top.Num();
		const float Period = N / FMath::Max(1.f, Bm.Fps);
		const float Steps = FMath::Frac(Now / Period + Bm.Phase) * N;
		const int32 K0 = FMath::FloorToInt(Steps) % N, K1 = (K0 + 1) % N;
		const float Ease = 0.5f - 0.5f * FMath::Cos(PI * (Steps - FMath::FloorToFloat(Steps)));
		const float TopShift = FMath::Lerp(Bm.Top[K0], Bm.Top[K1], Ease) - Bm.Top[0];
		for (int32 s = 0; s < Bm.Base.Num(); ++s)
		{
			TArray<FVector> Pos = Bm.Base[s];
			for (FVector& P : Pos)
			{
				const float Hn = FMath::Clamp(float(P.Z) / Bm.Height, 0.f, 1.f);
				const float W = Hn * Hn;
				const float Fl = Bm.Flutter * FMath::Sin(Now * 4.8f + float(P.X + P.Y) * 0.07f);
				P += Wind * (TopShift * W) + Across * (Fl * W);
			}
			M->UpdateMeshSection_LinearColor(s, Pos, Bm.Nrm[s], Bm.UV[s], Bm.Col[s], TArray<FProcMeshTangent>(), bOldSrgb);
		}
		if (D2 < FMath::Square(1200.0)) ShellItemModel(M, Bm.Stem);
	}
	// flames flickering through their sprite's drawings: each frame's size (about the flame's foot) and brightness, held
	// for a frame as the sprite held it
	for (int32 i = FlickerModels.Num() - 1; i >= 0; --i)
	{
		FFlickerModel& Fm = FlickerModels[i];
		UProceduralMeshComponent* M = Fm.Mesh.Get();
		if (!M)
		{
			FlickerModels.RemoveAt(i);
			continue;
		}
		const int32 N = Fm.Scale.Num();
		const int32 Fr = int32(FMath::FloorToFloat((Now + Fm.Phase * N / FMath::Max(1.f, Fm.Fps)) * Fm.Fps)) % N;
		if (Fr == Fm.Shown || !M->IsVisible() || FVector::DistSquared(M->GetComponentLocation(), Eye) > FMath::Square(6000.0)) continue;
		Fm.Shown = Fr;
		const float Sc = Fm.Scale[Fr];
		const float Br = Fm.Bright.IsValidIndex(Fr) ? Fm.Bright[Fr] : 1.f;
		TArray<FVector> Pos = Fm.Base;
		for (FVector& P : Pos) P = Fm.Root + (P - Fm.Root) * Sc;
		TArray<FLinearColor> Col = Fm.Col;
		for (FLinearColor& C0 : Col) C0 = FLinearColor(C0.R * Br, C0.G * Br, C0.B * Br, C0.A);
		M->UpdateMeshSection_LinearColor(Fm.Section, Pos, Fm.Nrm, Fm.UV, Col, TArray<FProcMeshTangent>(), bOldSrgb);
	}
}

void AOutlawsLevelActor::DebugRestColours(int32 Kind, int32& OutModels, int32& OutMoved, int32& OutVerts, int32& OutAsBuild, int32& OutAsOld, int32& OutLit,
	FVector& OutAt) const
{
	// (-OLSrgbRestProbe) the moving models' vertex colours as last given the GPU, against the colours they were made with,
	// unconverted (the build's rule) and sRGB-converted (as the updates wrote them before BUG-SRGBREST).  Kind 0 the waving
	// cloths, 1 the bending plants, 2 the flickering flames (made colours times the frame's brightness), 3 the bottle shards
	OutModels = OutMoved = OutVerts = OutAsBuild = OutAsOld = OutLit = 0;
	OutAt = FVector::ZeroVector;
	auto Count = [&](const FProcMeshSection* P, const TArray<FLinearColor>& Want, float Br)
	{
		for (int32 k = 0; P && k < P->ProcVertexBuffer.Num() && k < Want.Num(); ++k)
		{
			const FLinearColor C(Want[k].R * Br, Want[k].G * Br, Want[k].B * Br, Want[k].A);
			const FColor Got = P->ProcVertexBuffer[k].Color, Raw = C.ToFColor(false);
			++OutVerts;
			OutAsBuild += Got == Raw ? 1 : 0;
			OutAsOld += Got == C.ToFColor(true) ? 1 : 0;
			OutLit += Raw.R > 0 && Raw.R < 255 ? 1 : 0;
		}
	};
	// a section moved: its drawn corners are off the shape it was made with (an update reached it)
	auto Moved = [](const FProcMeshSection* P, const TArray<FVector>& Base)
	{
		for (int32 k = 0; P && k < P->ProcVertexBuffer.Num() && k < Base.Num(); ++k)
		{
			if (!P->ProcVertexBuffer[k].Position.Equals(Base[k], 0.01)) return true;
		}
		return false;
	};
	auto Note = [&](const UProceduralMeshComponent* M) { if (OutModels++ == 0) OutAt = M->Bounds.Origin; };
	if (Kind == 0 || Kind == 1)
	{
		const int32 N = Kind == 0 ? WavingModels.Num() : BendModels.Num();
		for (int32 i = 0; i < N; ++i)
		{
			UProceduralMeshComponent* M = Kind == 0 ? WavingModels[i].Mesh.Get() : BendModels[i].Mesh.Get();
			if (!M) continue;
			Note(M);
			const TArray<TArray<FVector>>& Base = Kind == 0 ? WavingModels[i].Base : BendModels[i].Base;
			const TArray<TArray<FLinearColor>>& Col = Kind == 0 ? WavingModels[i].Col : BendModels[i].Col;
			bool bMoved = false;
			for (int32 s = 0; s < Base.Num() && s < Col.Num(); ++s) bMoved |= Moved(M->GetProcMeshSection(s), Base[s]);
			if (!bMoved) continue;
			++OutMoved;
			for (int32 s = 0; s < Base.Num() && s < Col.Num(); ++s) Count(M->GetProcMeshSection(s), Col[s], 1.f);
		}
	}
	else if (Kind == 2)
	{
		for (const FFlickerModel& Fm : FlickerModels)
		{
			UProceduralMeshComponent* M = Fm.Mesh.Get();
			if (!M) continue;
			Note(M);
			if (Fm.Shown < 0) continue;
			++OutMoved;
			Count(M->GetProcMeshSection(Fm.Section), Fm.Col, Fm.Bright.IsValidIndex(Fm.Shown) ? Fm.Bright[Fm.Shown] : 1.f);
		}
	}
	else if (Kind == 3)
	{
		for (const FGlassBurst& G : GlassBursts)
		{
			UProceduralMeshComponent* M = G.Mesh.Get();
			if (!M) continue;
			Note(M);
			const FProcMeshSection* P = M->GetProcMeshSection(0);
			if (!P) continue;
			// (the shards fly from where SpawnGlass made them: their corners leave the made ones at once)
			++OutMoved;
			Count(P, G.Col, 1.f);
		}
	}
}

void AOutlawsLevelActor::SettleItemModels()
{
	// the sprites were placed for the original's flat pictures: a 3D model as wide as its picture can stand half in a wall
	// or in the barrel beside it.  Each model's own outline (how far its mesh reaches in 16 ways, from the sprite's foot)
	// against the walls at a third of its height, and against the outlines of its neighbours whose heights overlap, a few
	// rounds of small pushes; kept only where the floor under it stays at its height (bottles stay on their shelf) and
	// never more than half its width or 40 cm.  Hanging lamps, scenery (trees, rocks turned by place), what rides a lift
	// or a door, and what has no model are left as placed
	static const bool bOff = OLFlags::Param(TEXT("OLNoSettle"));
	UWorld* World = GetWorld();
	if (!World) return;
	constexpr int32 Ways = 16;
	struct FItem
	{
		AOutlawsSpriteActor* S = nullptr;
		FVector Home = FVector::ZeroVector;     // where it was placed (the foot)
		FVector At = FVector::ZeroVector;
		double Reach[Ways];                     // its outline: how far it reaches each way
		double Wide = 0.0, Bottom = 0.0, Top = 0.0;
		bool bMoved = false;
	};
	TArray<FItem> Items;
	for (AActor* Spawned : SpawnedActors)
	{
		AOutlawsSpriteActor* S = Cast<AOutlawsSpriteActor>(Spawned);
		UProceduralMeshComponent* M = S ? Cast<UProceduralMeshComponent>(S->Model) : nullptr;
		if (!M || S->ModelStem.IsEmpty() || (S->GetRootComponent() && S->GetRootComponent()->GetAttachParent())) continue;
		const FItemModelData* D = ItemModelFor(S->ModelStem);
		if (!D || D->bHang || D->bAnyYaw || D->bKeepOrigin) continue;
		FItem It;
		It.S = S;
		It.Home = It.At = S->GetActorLocation();
		for (double& R : It.Reach) R = 0.0;
		const FVector C = M->GetComponentLocation();
		double Lo = TNumericLimits<double>::Max(), Hi = -Lo;
		for (int32 s = 0; s < M->GetNumSections(); ++s)
		{
			const FProcMeshSection* Sec = M->GetProcMeshSection(s);
			if (!Sec || !Sec->bSectionVisible) continue;
			for (const FProcMeshVertex& V : Sec->ProcVertexBuffer)
			{
				const FVector P = V.Position + (C - It.At);         // (the mesh rides at the foot, world-aligned)
				Lo = FMath::Min(Lo, P.Z);
				Hi = FMath::Max(Hi, P.Z);
				for (int32 k = 0; k < Ways; ++k)
				{
					const double A = UE_DOUBLE_TWO_PI * k / Ways;
					It.Reach[k] = FMath::Max(It.Reach[k], P.X * FMath::Cos(A) + P.Y * FMath::Sin(A));
				}
			}
		}
		if (Lo > Hi) continue;
		for (const double R : It.Reach) It.Wide = FMath::Max(It.Wide, R);
		It.Bottom = It.At.Z + Lo;
		It.Top = It.At.Z + Hi;
		if (It.Wide < 2.0 || It.Wide > 150.0) continue;          // (a speck, or something the size of a wagon)
		Items.Add(It);
	}
	auto ReachTo = [&](const FItem& It, const FVector2D& Dir)
	{
		// the outline along any heading: the nearer of the two sampled ways round it, scaled to the heading
		double Best = 0.0;
		for (int32 k = 0; k < Ways; ++k)
		{
			const double A = UE_DOUBLE_TWO_PI * k / Ways;
			const double Cos = Dir.X * FMath::Cos(A) + Dir.Y * FMath::Sin(A);
			if (Cos > 0.92) Best = FMath::Max(Best, It.Reach[k] * Cos);
		}
		return Best;
	};
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsSettle), true);
	auto FloorKept = [&](const FItem& It, const FVector& To)
	{
		FHitResult F;
		if (!World->LineTraceSingleByChannel(F, To + FVector(0.0, 0.0, 30.0), To - FVector(0.0, 0.0, 30.0), ECC_Visibility, Q) || F.GetActor() != this) return false;
		return FMath::Abs(F.ImpactPoint.Z - It.Home.Z) < 4.0 && F.ImpactNormal.Z > 0.7;
	};
	auto TryMove = [&](FItem& It, const FVector2D& By)
	{
		FVector To = It.At + FVector(By.X, By.Y, 0.0);
		FVector2D Off(To.X - It.Home.X, To.Y - It.Home.Y);
		const double Cap = FMath::Min(40.0, It.Wide * 0.5);
		if (Off.Size() > Cap) Off = Off.GetSafeNormal() * Cap;
		To = FVector(It.Home.X + Off.X, It.Home.Y + Off.Y, It.Home.Z);
		if ((FVector2D(To) - FVector2D(It.At)).Size() < 0.2 || !FloorKept(It, To)) return;
		It.At = To;
		It.bMoved = true;
	};
	int32 WallPushes = 0, PairPushes = 0;
	for (int32 Round = 0; Round < 6; ++Round)
	{
		// out of the walls: the deepest a wall reaches into its outline, out along that way
		for (FItem& It : Items)
		{
			const double Hz = It.Bottom + FMath::Clamp((It.Top - It.Bottom) * 0.35, 6.0, 60.0);
			FVector2D Push(0.0, 0.0);
			double Deepest = 0.5;
			for (int32 k = 0; k < Ways; ++k)
			{
				const double A = UE_DOUBLE_TWO_PI * k / Ways;
				const FVector2D Dir(FMath::Cos(A), FMath::Sin(A));
				const FVector From(It.At.X, It.At.Y, Hz);
				FHitResult H;
				if (!World->LineTraceSingleByChannel(H, From, From + FVector(Dir.X, Dir.Y, 0.0) * (It.Reach[k] + 1.0), ECC_Visibility, Q)) continue;
				if (H.GetActor() != this || FMath::Abs(H.ImpactNormal.Z) > 0.6 || H.bStartPenetrating) continue;
				const double In = It.Reach[k] - H.Distance;
				if (In > Deepest) { Deepest = In; Push = -Dir * In; }
			}
			if (!Push.IsNearlyZero()) { TryMove(It, Push); ++WallPushes; }
		}
		// apart from each other: two outlines that overlap (their heights too), each half the way back
		for (int32 i = 0; i < Items.Num(); ++i)
		{
			for (int32 j = i + 1; j < Items.Num(); ++j)
			{
				FItem& A = Items[i];
				FItem& B = Items[j];
				if (FMath::Min(A.Top, B.Top) - FMath::Max(A.Bottom, B.Bottom) < 5.0) continue;
				FVector2D D(B.At.X - A.At.X, B.At.Y - A.At.Y);
				const double Dist = D.Size();
				if (Dist > A.Wide + B.Wide) continue;
				const FVector2D Dir = Dist > 0.1 ? D / Dist : FVector2D(1.0, 0.0);
				const double Over = ReachTo(A, Dir) + ReachTo(B, -Dir) - Dist;
				if (Over < 0.5) continue;
				TryMove(A, -Dir * (Over * 0.5));
				TryMove(B, Dir * (Over * 0.5));
				++PairPushes;
			}
		}
	}
	int32 Moved = 0;
	double Most = 0.0;
	FString MostName;
	for (FItem& It : Items)
	{
		if (!It.bMoved) continue;
		const double Dd = FVector::Dist2D(It.At, It.Home);
		if (Dd < 0.5) continue;
		if (!bOff) It.S->SetActorLocation(It.At);       // (-OLNoSettle: worked out, not applied)
		++Moved;
		if (Dd > Most) { Most = Dd; MostName = It.S->ModelStem; SettleLookAt = It.Home; }
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT settled 3D items%s: %d of %d moved (%d wall pushes, %d pair pushes), the most %.0f cm (%s)"),
		bOff ? TEXT(" (not applied: -OLNoSettle)") : TEXT(""), Moved, Items.Num(), WallPushes, PairPushes, Most, *MostName);
}

// (BUG-RESPAWNCRASH) the soak's count of the item models' caches: entries, and the bytes of the templates' vertices
int64 OlItemCacheBytes(int32& OutTemplates, int32& OutModels)
{
	int64 Bytes = 0;
	for (const TPair<FString, FItemTemplate>& Kv : GItemTemplates)
	{
		for (const FProcMeshSection& S : Kv.Value.Sections) Bytes += S.ProcVertexBuffer.GetAllocatedSize() + S.ProcIndexBuffer.GetAllocatedSize();
	}
	OutTemplates = GItemTemplates.Num();
	OutModels = GItemModels.Num();
	return Bytes;
}

// (BUG-RESPAWNCRASH) a level change: the templates are the level's models at its lights (a copy per light level), made again
// as the next level's items drop
void OlEmptyItemTemplates()
{
	GItemTemplates.Empty();
}
