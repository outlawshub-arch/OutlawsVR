#include "OutlawsLevelActor.h"
#include "OutlawsFlags.h"
#include "OutlawsShipSmoke.h"
#include "Misc/ScopeExit.h"
#include "OutlawsMpPolish.h"
#include "OutlawsXRRefresh.h"
#include "OutlawsPcmWave.h"
#include "UObject/UObjectIterator.h"
#include "RenderingThread.h"
#include "UObject/UObjectGlobals.h"
#include "OutlawsPerf.h"
#include "OutlawsDeviceProfile.h"
#include "OutlawsRuntime.h"
#include "OutlawsLab.h"
#include "OutlawsLevel.h"
#include "OutlawsFormats.h"
#include "OutlawsGeometry.h"
#include "OutlawsRuntimeLibrary.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsTextures.h"
#include "Async/ParallelFor.h"
#include "ProceduralMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Engine/CollisionProfile.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/Pawn.h"
#include "Misc/Paths.h"
#include "Containers/Ticker.h"
#include "OutlawsInf.h"
#include "OutlawsInfRuntime.h"
#include "Components/InputComponent.h"
#include "InputCoreTypes.h"
#include "Components/AudioComponent.h"
#include "Components/TextRenderComponent.h"
#include "OutlawsPlayer.h"
#include "OutlawsMovieScreen.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Character.h"
#include "Components/CapsuleComponent.h"
#include "OutlawsEnemy.h"
#include "OutlawsCoop.h"
#include "OutlawsRemotePlayer.h"
#include "HeadMountedDisplayFunctionLibrary.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Camera/PlayerCameraManager.h"
#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<int32> CVarOLCull(TEXT("ol.cull"), 1,
	TEXT("Portal culling of sprites, items, enemies and doors outside the eye's portal flood (0 = off)"));
// (BUG-POPIN) the flood's slack round every opening (radians) and how long a sector stays drawn after the flood leaves it
static TAutoConsoleVariable<float> CVarOLVisSlack(TEXT("ol.VisSlack"), 0.f, TEXT("Portal flood: extra angle round every opening, radians (0: the build 22 rule)."));
static TAutoConsoleVariable<float> CVarOLVisHold(TEXT("ol.VisHold"), 1.0f, TEXT("Seconds a sector stays drawn after the flood leaves it (0.4: build 22)."));
// (QSPEED-3) the eye's flood kept until something it read could change it (FloodStillHolds)
static TAutoConsoleVariable<int32> CVarOLVisCache(TEXT("ol.VisCache"), 1, TEXT("Keep the eye's portal flood while nothing it read has changed (1), or flood every tick (0)."));
// (CPU1) the world's solid sections drawn as a part per texture, each culled by its own bounds (-OLNoWorldParts: 0)
static TAutoConsoleVariable<int32> CVarOLWorldParts(TEXT("ol.WorldParts"), 1,
	TEXT("Draw the static world as a mesh per texture, culled by its own bounds (1), or as the one world mesh (0). Read when a level builds."));

using namespace OutlawsFormats;

AOutlawsLevelActor::AOutlawsLevelActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}

void AOutlawsLevelActor::LoadDefaultAssets()
{
	auto LoadIfNull = [](auto& Prop, const TCHAR* Path)
	{
		using T = typename TRemovePointer<decltype(Prop.Get())>::Type;
		if (!Prop)
		{
			Prop = LoadObject<T>(nullptr, Path);
		}
	};
	LoadIfNull(WorldMaterial, TEXT("/Game/Outlaws/Runtime/M_OLR_World.M_OLR_World"));
	LoadIfNull(WorldMaskedMaterial, TEXT("/Game/Outlaws/Runtime/M_OLR_WorldMasked.M_OLR_WorldMasked"));
	LoadIfNull(WorldGlassMaterial, TEXT("/Game/Outlaws/Runtime/M_OLR_WorldGlass.M_OLR_WorldGlass"));
	LoadIfNull(WorldMaskedSolidMaterial, TEXT("/Game/Outlaws/Runtime/M_OLR_WorldMaskedSolid.M_OLR_WorldMaskedSolid"));
	LoadIfNull(WaterMaterial, TEXT("/Game/Outlaws/Runtime/M_OLR_Water.M_OLR_Water"));
	LoadIfNull(SkyMaterial, TEXT("/Game/Outlaws/Core/M_OL_Sky.M_OL_Sky"));
	LoadIfNull(SpriteMaterial, TEXT("/Game/Outlaws/Core/M_OL_Sprite.M_OL_Sprite"));
	LoadIfNull(SpriteFireMaterial, TEXT("/Game/Outlaws/Runtime/M_OLR_SpriteFire.M_OLR_SpriteFire"));      // (REQ-FIREBRIGHT2)
	LoadIfNull(SpriteQuad, TEXT("/Game/Outlaws/Core/SM_SpriteQuad.SM_SpriteQuad"));
}

void AOutlawsLevelActor::Clear()
{
	++ClearCount;
	GlassBursts.Reset();       // (their meshes go with ObjectMeshes)
	for (AActor* A : SpawnedActors)
	{
		if (IsValid(A))
		{
			A->Destroy();
		}
	}
	SpawnedActors.Reset();
	FloorStandingSprites.Reset();
	for (UProceduralMeshComponent* M : ObjectMeshes)
	{
		if (IsValid(M)) M->DestroyComponent();
	}
	ObjectMeshes.Reset();
	for (UProceduralMeshComponent* M : { WorldMesh.Get(), WaterMesh.Get(), SkyMesh.Get(), SkySurfaceMesh.Get() })
	{
		if (IsValid(M)) M->DestroyComponent();
	}
	WorldMesh = WaterMesh = SkyMesh = SkySurfaceMesh = nullptr;
	SkySurfaceMid = nullptr;
	for (UProceduralMeshComponent* M : MoverMeshes)
	{
		if (IsValid(M)) M->DestroyComponent();
	}
	MoverMeshes.Reset();
	MoverBatches.Reset();                   // (their meshes went with MoverMeshes)
	ScrollMeshes.Reset();                   // (their meshes went with ObjectMeshes)
	LightFaces.Reset();                     // (REQ-SECTORLIGHT; their meshes too)
	LightThings.Reset();
	LightGroups.Reset();
	bLightThingsFound = false;
	ExplodeWalls.Reset();
	OverlapPairs.Reset();                   // (their meshes too)
	SeenFromEye.Reset();                    // portal culling: off until the next level's first tick
	SeenDepth.Reset();
	FloodCache = FFloodCache();             // (QSPEED-3) a new level floods afresh
	++FloodGeomGen;
	VisTicks = VisFloods = VisTracked = VisChecked = VisDiffers = VisPops = 0;
	for (int64& R : VisRadii) R = 0;
	for (int64& R : VisSites) R = 0;
	for (int64& R : VisWhyNot) R = 0;
	VisibleSectors.Empty();
	CullBoxes.Reset();
	CullNear.Reset();
	++CullGen;
	GroupCullSectors.Reset();
	GroupCulled.Empty();
	AOutlawsSpriteActor::SetCulling(false, nullptr, FVector::ZeroVector, nullptr);
	bCullFnSent = false;
	NextCullLog = 0.0;
	OverlapParts.Reset();
	OverlapHidden.Reset();
	OverlapIgnored.Reset();
	OverlapFrom = -2;
	ZoneGates.Reset();
	ZoneNow.Reset();
	ZoneSkip.Empty();
	bZoneSkipOn = false;
	bZoneHavePrev = false;
	FloorHurt = 0.f;
	HurtSector = -1;
	Pickups.Reset();
	PickupSerials = 0;
	PlacedGroundModels = 0;
	ModelSerialsFrom = 0;
	Inventory.Reset();
	InventoryLabels.Reset();
	Groups.Reset();
	ComponentGroup.Reset();
	CrossWalls.Reset();
	Inf.Reset();
	for (TPair<int32, TWeakObjectPtr<UAudioComponent>>& L : LoopSounds)
	{
		if (L.Value.IsValid()) L.Value->Stop();
	}
	LoopSounds.Reset();
	PlayerSector = -1;
	bHavePlayerPrev = false;
	Materials.Reset();
	AtxLoops.Reset();
	AtxWalls.Reset();
	GlassTintMid = nullptr;
	Textures.Reset();
	AtlasCache.Reset();         // its textures lived in Textures
	Props.Reset();
	Panes.Reset();
	Dynamite.Reset();
	Sparks.Reset();
	LogFarSummary();                        // (the level's far-freeze and far-sound line, user 2026-10-03)
	Enemies.Reset();
	EnemySpawns.Reset();
	LowestFloor = 0.f;
	Kills = 0;
	TexturesLo.Reset();
	TexturesHd.Reset();
	SpriteAtlasesHd.Reset();
	Boss.Reset();
	BossSpawns.Reset();
	BossSlots.Reset();
	bBackdrop = false;
	TotalBadGuys = 0;
	BossTimer = 2.f;
	BossTauntWait = 15.f;
	BossTauntBearing = -1.f;
	bBossDefeated = false;
	bHomBossDown = false;
	bMidMovieDone = false;                      // (the ranch's door movie: once per build of the level)
	LevelSoundDuck = 1.f;                       // (a level left under its door movie: its sound not kept down)
	RandomBossActors.Reset();
	CellGens.Reset();
	DuckedSounds.Reset();
	bMidMovieOn = false;
	LevelEndTimer = -1.f;
	for (FSoundGen& G : SoundGens)
	{
		for (TWeakObjectPtr<UAudioComponent>& A : G.Playing) if (A.IsValid()) A->Stop();
	}
	SoundGens.Reset();
	ItemGens.Reset();
	Carries.Reset();
	Bubbles.Reset();                 // (their actors went with SpawnedActors)
	StopMusic();
	MusicCues.Reset();
	bHavePlayerStart = false;
}

void AOutlawsLevelActor::SetUpCloudLayer(bool bTiles, float Aspect)
{
	if (!SkySurfaceMid) return;
	// a dome a third as high as it is wide, the picture about twice across it; a horizon sky folds its rows into its top
	// band (its mountains would hang overhead)
	SkySurfaceMid->SetScalarParameterValue(TEXT("CloudOpacity"), 1.f);
	SkySurfaceMid->SetScalarParameterValue(TEXT("CloudH"), 0.35f);
	SkySurfaceMid->SetScalarParameterValue(TEXT("CloudTiles"), 4.f);
	SkySurfaceMid->SetScalarParameterValue(TEXT("CloudMirror"), bTiles ? 0.f : 1.f);
	SkySurfaceMid->SetScalarParameterValue(TEXT("CloudVOff"), 0.02f);
	SkySurfaceMid->SetScalarParameterValue(TEXT("CloudVScale"), 0.3f);
	SkySurfaceMid->SetScalarParameterValue(TEXT("CloudAspect"), Aspect);     // square texels on a wide picture
	CloudDrift = FVector2D(FMath::FRand(), FMath::FRand());
	SkySurfaceMid->SetVectorParameterValue(TEXT("CloudWind"), FLinearColor(float(CloudDrift.X), float(CloudDrift.Y), 0.f, 0.f));
	CloudSpin = FMath::FRandRange(0.f, 2.f * PI);
	SkySurfaceMid->SetScalarParameterValue(TEXT("CloudSpin"), CloudSpin);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT cloud layer on (%s)"), bTiles ? TEXT("the sky tiling") : TEXT("the horizon sky's top band"));
}

FString AOutlawsLevelActor::CloudReport() const
{
	if (!SkySurfaceMid) return TEXT("no sky");
	float Op = -1.f, H = -1.f, Tiles = -1.f, Spin = -1.f;
	SkySurfaceMid->GetScalarParameterValue(FName(TEXT("CloudOpacity")), Op);
	SkySurfaceMid->GetScalarParameterValue(FName(TEXT("CloudH")), H);
	SkySurfaceMid->GetScalarParameterValue(FName(TEXT("CloudTiles")), Tiles);
	SkySurfaceMid->GetScalarParameterValue(FName(TEXT("CloudSpin")), Spin);
	return FString::Printf(TEXT("cloud layer: opacity %.2f, height %.2f, tiles %.1f, spin %.2f rad; sky material %s"), Op, H, Tiles, Spin,
		SkySurfaceMid->Parent ? *SkySurfaceMid->Parent->GetName() : TEXT("-"));
}

AActor* AOutlawsLevelActor::MakeDynamiteActor(UProceduralMeshComponent* From, UProceduralMeshComponent*& OutMesh, const TCHAR* MeshName)
{
	// a copy of a level mesh on an actor of its own, where the level put it (riding a lift if it did): shot traces
	// meet it, bodies pass (a placed knife's too, BUG-KNIFEPICKUP, then made without collision by its caller)
	OutMesh = nullptr;
	UWorld* World = GetWorld();
	if (!World || !From) return nullptr;
	FActorSpawnParameters SP;
	SP.ObjectFlags |= RF_Transient;
	AActor* A = World->SpawnActor<AActor>(AActor::StaticClass(), From->GetComponentTransform(), SP);
	if (!A) return nullptr;
	UProceduralMeshComponent* M = NewObject<UProceduralMeshComponent>(A, MeshName);
	M->bUseComplexAsSimpleCollision = true;
	M->bUseAsyncCooking = false;
	M->SetCastShadow(false);
	A->SetRootComponent(M);
	M->RegisterComponent();
	for (int32 i = 0; i < From->GetNumSections(); ++i)
	{
		if (FProcMeshSection* S = From->GetProcMeshSection(i)) M->SetProcMeshSection(i, *S);
		M->SetMaterial(i, From->GetMaterial(i));
	}
	M->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	M->SetCollisionResponseToAllChannels(ECR_Ignore);
	M->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
	A->SetActorTransform(From->GetComponentTransform());
	if (USceneComponent* Parent = From->GetAttachParent())
	{
		if (Parent != RootComponent) A->AttachToComponent(Parent, FAttachmentTransformRules::KeepWorldTransform);
	}
	SpawnedActors.Add(A);
	OutMesh = M;
	return A;
}

void AOutlawsLevelActor::AddLooseDynamite(AActor* A)
{
	if (!A) return;
	UProceduralMeshComponent* M = Cast<UProceduralMeshComponent>(A->GetRootComponent());
	if (M)
	{
		// shot traces meet it, bodies pass, as the level's own sticks (MakeDynamiteActor)
		M->bUseComplexAsSimpleCollision = true;
		M->bUseAsyncCooking = false;
		for (int32 i = 0; i < M->GetNumSections(); ++i)
		{
			if (FProcMeshSection* S = M->GetProcMeshSection(i))
			{
				FProcMeshSection Copy = *S;
				Copy.bEnableCollision = true;
				M->SetProcMeshSection(i, Copy);
			}
		}
		M->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		M->SetCollisionResponseToAllChannels(ECR_Ignore);
		M->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
	}
	else if (AOutlawsSpriteActor* S = Cast<AOutlawsSpriteActor>(A))
	{
		M = Cast<UProceduralMeshComponent>(S->Model);       // (set off by blasts; its sprite takes shots as a sprite does)
	}
	if (!M) return;
	FDynamite D;
	D.Mesh = M;
	D.Where = M->GetComponentLocation();
	Dynamite.Add(D);
}

bool AOutlawsLevelActor::IsDynamitePickup(const AActor* A) const
{
	for (const FDynamite& D : Dynamite) if (!D.bGone && D.Mesh.IsValid() && D.Mesh->GetOwner() == A) return true;
	return false;
}

int32 AOutlawsLevelActor::ClampThinOverlaps(FOutlawsLevel& L, const TSet<int32>& Moving)
{
	// a flat ceiling poking up half a unit or less into the flat floor of a sector above it that it does not adjoin (the
	// mill's river culverts RIVER11/12, ceilings at 8.25, under the building's floors at 8): the Jedi renderer and its
	// collision never meet the two, one merged world does (a lip underfoot over the whole building's river side): the
	// ceiling comes down to that floor (user 2026-09-29).  Sectors a lift moves are left as they are
	const int32 N = L.Sectors.Num();
	TArray<FBox2D> Box;
	Box.Init(FBox2D(ForceInit), N);
	for (int32 i = 0; i < N; ++i) for (const FVector2D& V : L.Sectors[i].Verts) Box[i] += V;
	auto Adjoined = [&](int32 a, int32 b)
	{
		for (const FOutlawsWall& W : L.Sectors[a].Walls) if (W.Adjoin == b || W.DAdjoin == b) return true;
		for (const FOutlawsWall& W : L.Sectors[b].Walls) if (W.Adjoin == a || W.DAdjoin == a) return true;
		return L.Sectors[a].VAdjoin == b || L.Sectors[b].VAdjoin == a;
	};
	auto Meet = [&](int32 a, int32 b)
	{
		const FBox2D C(FVector2D(FMath::Max(Box[a].Min.X, Box[b].Min.X), FMath::Max(Box[a].Min.Y, Box[b].Min.Y)),
					   FVector2D(FMath::Min(Box[a].Max.X, Box[b].Max.X), FMath::Min(Box[a].Max.Y, Box[b].Max.Y)));
		const FVector2D Sz = C.GetSize();
		if (Sz.X <= 0.0 || Sz.Y <= 0.0) return false;
		const double Step = FMath::Clamp(FMath::Min(Sz.X, Sz.Y) / 8.0, 0.25, 2.0);
		for (double y = C.Min.Y + Step * 0.5; y < C.Max.Y; y += Step)
			for (double x = C.Min.X + Step * 0.5; x < C.Max.X; x += Step)
				if (L.ContainsXZ(L.Sectors[a], x, y) && L.ContainsXZ(L.Sectors[b], x, y)) return true;
		return false;
	};
	int32 Changed = 0;
	for (int32 a = 0; a < N; ++a)
	{
		FOutlawsSector& A = L.Sectors[a];
		if (A.NoWalls() || A.Verts.Num() < 3 || Moving.Contains(a) || (A.Flags & OutlawsFlags::SlopedCeiling) || !Box[a].bIsValid) continue;
		double NewCeil = A.Ceil.Y;
		for (int32 b = 0; b < N; ++b)
		{
			if (b == a) continue;
			const FOutlawsSector& B = L.Sectors[b];
			if (B.NoWalls() || B.Verts.Num() < 3 || (B.Flags & OutlawsFlags::SlopedFloor) || !Box[b].bIsValid || !Box[a].Intersect(Box[b])) continue;
			const double Poke = A.Ceil.Y - B.Floor.Y;
			if (Poke <= 0.001 || Poke > 0.5 || B.Floor.Y <= A.Floor.Y + 0.5 || Adjoined(a, b) || !Meet(a, b)) continue;
			NewCeil = FMath::Min(NewCeil, B.Floor.Y);
		}
		if (NewCeil < A.Ceil.Y)
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLRT sector %d %s: ceiling %.2f poked into a floor above it, brought down to %.2f"), a, *A.Name, A.Ceil.Y, NewCeil);
			A.Ceil.Y = NewCeil;
			++Changed;
		}
	}
	return Changed;
}

void AOutlawsLevelActor::BuildExplodeWalls(const FOutlawsLevel& L, const TMap<FString, FOutlawsTexInfo>& TexInfo)
{
	if (ExplodeWalls.Num() == 0) return;
	// the faces drawn as filled openings (the MIDTEX a see-through wall has), with their signs, on meshes of their own
	FOutlawsLevel Solid = L;
	for (const FExplodeWall& X : ExplodeWalls)
	{
		for (const uint64 K : X.Walls) Solid.Sectors[int32(K >> 32)].Walls[int32(K & 0xffffffffu)].Flags1 |= OutlawsFlags::WallMidTex;
	}
	for (int32 i = 0; i < ExplodeWalls.Num(); ++i)
	{
		FExplodeWall& X = ExplodeWalls[i];
		FOutlawsBuildFilter F;
		F.bOnly = true;
		for (const uint64 K : X.Walls)
		{
			F.OnlyWalls.Add(K);
			F.WallBands.Add(K, 2 | 8);
		}
		// (BUG-OUTHOUSEWALL, user 28 Oct) a face of the plug that is a double adjoin (the outhouse's back wall: below to the
		// explodable sliver, above to the room with the hole) is filled toward the plug's own sector only, its sign on its
		// solid part, as the original draws it before the blast.  -OLOldOuthouseWall: both openings filled, as before
		static const bool bOldOuthouse = OLFlags::Param(TEXT("OLOldOuthouseWall"));
		for (const uint64 K : bOldOuthouse ? TArray<uint64>() : X.Walls)
		{
			const FOutlawsWall& W = L.Sectors[int32(K >> 32)].Walls[int32(K & 0xffffffffu)];
			if (W.Adjoin < 0 || W.DAdjoin < 0) continue;
			auto OwnSide = [&X](int32 Sec) { return X.Walls.ContainsByPredicate([Sec](uint64 J) { return int32(J >> 32) == Sec; }); };
			if (OwnSide(W.DAdjoin)) F.FillToward.Add(K, W.DAdjoin);
			else if (OwnSide(W.Adjoin)) F.FillToward.Add(K, W.Adjoin);
			UE_LOG(LogOutlaws, Log, TEXT("OLRT explodable wall %d: sector %d wall %d (adjoins %d and %d) filled toward %d only"), i, int32(K >> 32), int32(K & 0xffffffffu),
				W.Adjoin, W.DAdjoin, F.FillToward.Contains(K) ? F.FillToward[K] : -1);
		}
		FOutlawsGeometry Gs;
		Gs.Build(Solid, TexInfo, bReverseWinding, &F);
		int32 Tris = 0;
		if (Gs.World.Num() > 0)
		{
			UProceduralMeshComponent* M = NewMeshComponent(*FString::Printf(TEXT("OutlawsExplodeWall%d"), i), false);
			AddSections(M, Gs.World, false, WorldMaterial, WorldMaskedMaterial, Tris);
			ObjectMeshes.Add(M);
			X.Parts.Add(M);
		}
		if (UProceduralMeshComponent* B = MakeBlocker(TEXT("ALL"), X.Blocker))
		{
			ObjectMeshes.Add(B);
			X.Parts.Add(B);
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLRT explodable wall %d (sector %d): %d faces, %d triangles drawn"), i, X.Sector, X.Walls.Num(), Tris);
	}
}

void AOutlawsLevelActor::BlowExplodeWall(FExplodeWall& X, bool bQuiet, const FVector& From)
{
	X.bBlown = true;
	if (!bQuiet && UOutlawsCoop::IsActive(this)) UOutlawsCoop::NoteWorldBit(this, UOutlawsCoop::BitWall, int32(&X - ExplodeWalls.GetData()), 0, From, 0.f);
	for (const TWeakObjectPtr<UPrimitiveComponent>& P : X.Parts)
	{
		if (UPrimitiveComponent* C = P.Get())
		{
			C->SetVisibility(false);
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		}
	}
	OverlapFrom = -2;                   // the way through is open: which overlapping rooms show is worked out again
	if (!bQuiet && X.Box.IsValid)
	{
		// the wall comes down in a spray of earth and stone toward the blast
		FVector Out = From - X.Box.GetCenter();
		Out.Z = 0.0;
		Out = Out.GetSafeNormal();
		if (Out.IsNearlyZero()) Out = FVector::UpVector;
		for (int32 k = 0; k < 3; ++k) SpawnDirt(X.Box.GetCenter() + FVector(0.0, 0.0, FMath::FRandRange(-40.f, 40.f)), Out, 10);
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT explodable wall of sector %d blown%s"), X.Sector, bQuiet ? TEXT(" (restored from a save)") : TEXT(""));
}

int32 AOutlawsLevelActor::DebugExplodeWallCount() const
{
	return ExplodeWalls.Num();
}

bool AOutlawsLevelActor::DebugExplodeWallDrawn(int32 Index, FBox& OutDrawn, FBox& OutSector, int32& OutSectorIndex) const
{
	// (-OLOuthouseWallProbe) the plug as drawn (its own mesh's bounds) against its sector's floor to ceiling
	OutDrawn = FBox(ForceInit);
	if (!ExplodeWalls.IsValidIndex(Index)) return false;
	const FExplodeWall& X = ExplodeWalls[Index];
	OutSector = X.Box;
	OutSectorIndex = X.Sector;
	for (const TWeakObjectPtr<UPrimitiveComponent>& P : X.Parts)
	{
		const UProceduralMeshComponent* M = Cast<UProceduralMeshComponent>(P.Get());
		if (M && M->GetName().StartsWith(TEXT("OutlawsExplodeWall")) && M->IsVisible()) OutDrawn += M->Bounds.GetBox();
	}
	return OutDrawn.IsValid != 0;
}

void AOutlawsLevelActor::DebugBlowExplodeWall(int32 Index)
{
	if (ExplodeWalls.IsValidIndex(Index) && !ExplodeWalls[Index].bBlown) BlowExplodeWall(ExplodeWalls[Index], false, ExplodeWalls[Index].Box.GetCenter() + FVector(0.0, 0.0, 100.0));
}

void AOutlawsLevelActor::BuildSkyCeilingCollision(const FOutlawsLevel& L)
{
	// a room open to the sky stops a body at its sky ceiling as at any other (bad guys blasted up through it vanished
	// behind the sky; user 2026-09-29), but only a real room's: not a sector with no walls (the sky volumes wrapped round
	// a level: the mill's 607, its sky floor across the ELLE lift's shaft, squashed the rider through the car; user
	// 2026-09-29), not a sky floor (a pit is fallen into), not a ceiling a lift carries.  Invisible faces, bodies only
	TSet<int32> Moving, Paired;
	if (Inf) for (const TPair<int32, TArray<int32>>& Kv : Inf->ElevatorsBySector) Moving.Add(Kv.Key);
	for (const TPair<int32, int32>& P : OverlapPairs) { Paired.Add(P.Key); Paired.Add(P.Value); }
	FOutlawsMeshSection All;
	TArray<int32> Tris;
	int32 Rooms = 0;
	for (const FOutlawsSector& S : L.Sectors)
	{
		if (!(S.Flags & OutlawsFlags::ExtCeiling) || S.NoWalls() || S.Verts.Num() < 3 || Moving.Contains(S.Index)) continue;
		Tris.Reset();
		if (!FOutlawsGeometry::TriangulateSector(S, Tris)) continue;
		FOutlawsMeshSection Own;
		FOutlawsMeshSection& Into = Paired.Contains(S.Index) ? Own : All;
		for (int32 t = 0; t + 2 < Tris.Num(); t += 3)
		{
			FVector Q[3];
			for (int32 k = 0; k < 3; ++k)
			{
				const FVector2D V = S.Verts[Tris[t + k]];
				Q[k] = FOutlawsGeometry::ToUE(V.X, L.SurfaceY(S, V, false), V.Y);
			}
			const FVector2D UV[3];
			Into.AddFace(Q, UV, 3, 1.f, false);
			Into.AddFace(Q, UV, 3, 1.f, true);
		}
		++Rooms;
		if (Paired.Contains(S.Index) && Own.Triangles.Num() > 0)
		{
			// an overlapping room's own: it goes (for the body) with that room (ApplyOverlapChoice)
			UProceduralMeshComponent* M = NewMeshComponent(*FString::Printf(TEXT("OutlawsSkyCeiling%d"), S.Index), true);
			M->CreateMeshSection_LinearColor(0, Own.Positions, Own.Triangles, Own.Normals, Own.UVs, Own.Colors, TArray<FProcMeshTangent>(), true);
			SkyCollision(M);
			M->SetVisibility(false);
			ObjectMeshes.Add(M);
			OverlapParts.FindOrAdd(S.Index).Add({ M, false });
		}
	}
	if (All.Triangles.Num() > 0)
	{
		UProceduralMeshComponent* M = NewMeshComponent(TEXT("OutlawsSkyCeilings"), true);
		M->CreateMeshSection_LinearColor(0, All.Positions, All.Triangles, All.Normals, All.UVs, All.Colors, TArray<FProcMeshTangent>(), true);
		SkyCollision(M);
		M->SetVisibility(false);
		ObjectMeshes.Add(M);
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT sky ceilings that stop bodies: %d rooms"), Rooms);
}

float AOutlawsLevelActor::WallFacingYaw(const FVector& Where, int32 SectorIndex, float Fallback) const
{
	// a 3D pickup turned away from the nearest solid wall of its sector or the ones beside it (the levels never set
	// their yaw: sprites had none; user 2026-09-29).  The models face +Y (the sprite's face); a flat one lies with its
	// top edge to the wall.  None within 4 m: the level's own yaw
	const FOutlawsLevel* L = Inf ? &Inf->Live : LevelData.Get();
	if (!L || !L->Sectors.IsValidIndex(SectorIndex)) return Fallback;
	const double U = FOutlawsGeometry::UnitsToCm;
	const FVector2D P = FOutlawsGeometry::ToOutlawsPlan(Where);
	TArray<int32> Secs = { SectorIndex };
	for (const FOutlawsWall& W : L->Sectors[SectorIndex].Walls) if (L->Sectors.IsValidIndex(W.Adjoin)) Secs.AddUnique(W.Adjoin);
	const double Reach = 400.0 / U;
	struct FAwayFrom { double D; FVector2D Away; };
	TArray<FAwayFrom, TInlineAllocator<24>> Walls;
	for (const int32 s : Secs)
	{
		const FOutlawsSector& S = L->Sectors[s];
		for (const FOutlawsWall& W : S.Walls)
		{
			if (W.Adjoin >= 0 || !S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) continue;
			const FVector2D C = FMath::ClosestPointOnSegment2D(P, S.Verts[W.V1], S.Verts[W.V2]);
			const double D = FVector2D::Distance(P, C);
			if (D < Reach && D > 1e-3) Walls.Add({ D, (P - C) / D });
		}
	}
	if (Walls.Num() == 0) return Fallback;
	Walls.StableSort([](const FAwayFrom& A, const FAwayFrom& B) { return A.D < B.D; });
	FVector2D Away = Walls[0].Away;
	// (BUG-STATUEYAW, user 2026-10-28, snapshots 1-4: townex's statues faced along the path, "rotate them 90 degrees
	// clockwise") the booths are 8 units square and the statues stand 2.7 to 3 units off a side wall and 5.5 off the back,
	// so each faced the other side wall 5 units away.  When the nearest wall leaves less than 2 m in front, the thing
	// turns away from the wall within 4 m whose way has the most room (a ray through the open sides, up to 4 m), if that
	// is at least 2 m and twice as much: out of a booth, an alcove or a nook.  231 of the 5022 placed models in the story
	// levels turn.  -OLOldStatueYaw: the nearest wall always
	static const bool bOldYaw = OLFlags::Param(TEXT("OLOldStatueYaw"));
	auto Room = [L, Reach](int32 Sec, const FVector2D& From, const FVector2D& Dir)
	{
		double T0 = 0.0;
		for (int32 Step = 0; Step < 48 && L->Sectors.IsValidIndex(Sec); ++Step)
		{
			const FOutlawsSector& S = L->Sectors[Sec];
			double Hit = TNumericLimits<double>::Max();
			int32 Next = -2;
			for (const FOutlawsWall& W : S.Walls)
			{
				if (!S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) continue;
				const FVector2D A = S.Verts[W.V1], E = S.Verts[W.V2] - S.Verts[W.V1], Wv = A - From;
				const double Den = FVector2D::CrossProduct(Dir, E);
				if (FMath::Abs(Den) < 1e-12) continue;
				const double T = FVector2D::CrossProduct(Wv, E) / Den, At = FVector2D::CrossProduct(Wv, Dir) / Den;
				if (T > T0 + 1e-6 && At >= -1e-9 && At <= 1.0 + 1e-9 && T < Hit) { Hit = T; Next = W.Adjoin; }
			}
			if (Next == -2 || Hit >= Reach) return Reach;
			if (!L->Sectors.IsValidIndex(Next)) return Hit;      // a solid wall
			Sec = Next;
			T0 = Hit;
		}
		return Reach;
	};
	const double Short = 200.0 / U, Room0 = bOldYaw ? Reach : Room(SectorIndex, P, Away);
	if (Room0 < Short)
	{
		double Most = Room0;
		FVector2D Open = Away;
		for (const FAwayFrom& W : Walls)      // nearest first: a tie stays with the nearer wall
		{
			const double R = Room(SectorIndex, P, W.Away);
			if (R > Most + 1e-3) { Most = R; Open = W.Away; }
		}
		if (Most >= Short && Most >= 2.0 * Room0) Away = Open;
	}
	// (Outlaws x, z) -> Unreal (Y, X); +Y turned by the yaw onto that way
	return float(FMath::RadiansToDegrees(FMath::Atan2(Away.X, Away.Y))) - 90.f;
}

void AOutlawsLevelActor::SkyCollision(UProceduralMeshComponent* M)
{
	// the sky is a ceiling all the same (the Jedi engine stops a body at an EXT ceiling as at any other; user 2026-09-29:
	// bad guys blasted up through it vanished behind the sky faces): bodies stop at it, shots and looks pass through
	M->SetCollisionResponseToAllChannels(ECR_Ignore);
	M->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
}

UProceduralMeshComponent* AOutlawsLevelActor::NewMeshComponent(const TCHAR* Name, bool bCollision)
{
	UProceduralMeshComponent* M = NewObject<UProceduralMeshComponent>(this, MakeUniqueObjectName(this, UProceduralMeshComponent::StaticClass(), Name), RF_Transient);
	M->bUseComplexAsSimpleCollision = true;
	M->bUseAsyncCooking = false;
	M->SetCastShadow(false);
	M->SetupAttachment(RootComponent);
	// geometry is in world space whatever the actor's transform
	M->SetUsingAbsoluteLocation(true);
	M->SetUsingAbsoluteRotation(true);
	M->SetUsingAbsoluteScale(true);
	if (bCollision)
	{
		M->SetCollisionProfileName(UCollisionProfile::BlockAll_ProfileName);
	}
	else
	{
		M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	M->RegisterComponent();
	M->SetWorldTransform(FTransform::Identity);
	return M;
}

UMaterialInstanceDynamic* AOutlawsLevelActor::MaterialFor(UMaterialInterface* Parent, const FString& TexKey, int32 WaterChain, int64 AtxWall)
{
	if (!Parent)
	{
		return nullptr;
	}
	// key "<parent>|<stem>", under water "<parent>~w<chain>|<stem>", a wall's own ATX "<parent>~x<key>|<stem>" (the
	// texture toggles read the stem after the bar)
	const FString Key = Parent->GetName() + (WaterChain >= 0 ? FString::Printf(TEXT("~w%d"), WaterChain) : FString())
		+ (AtxWall >= 0 ? FString::Printf(TEXT("~x%lld"), AtxWall) : FString()) + TEXT("|") + TexKey;
	if (TObjectPtr<UMaterialInstanceDynamic>* Found = Materials.Find(Key))
	{
		return *Found;
	}
	UMaterialInstanceDynamic* MID = UMaterialInstanceDynamic::Create(Parent, this);
	if (TObjectPtr<UTexture2D>* Tex = Textures.Find(TexKey))
	{
		MID->SetTextureParameterValue(TEXT("Tex"), *Tex);
	}
	if (WaterChain >= 0 && WaterChainHue.IsValidIndex(WaterChain))
	{
		MID->SetVectorParameterValue(TEXT("WaterTint"), WaterChainHue[WaterChain]);
		MID->SetScalarParameterValue(TEXT("WaterSurface"), float(WaterChainSurface[WaterChain]));
	}
	// an animated texture: a looping one shares its frames with every material of its stem; a stopping one on a wall
	// gets this instance to itself (the builder keyed the wall's section alone)
	if (const FOutlawsTexInfo* TI = TexInfoCache.Find(TexKey))
	{
		if (TI->bAtxLoop)
		{
			FAtxState& A = AtxLoops.FindOrAdd(TexKey);
			if (A.Prog.Num() == 0) InitAtx(A, TexKey, -1, -1);
			A.Mids.Add(MID);
			if (!A.Frame.IsEmpty()) if (UTexture2D* T = AtxFrame(A.Frame)) MID->SetTextureParameterValue(TEXT("Tex"), T);
		}
		else if (TI->bAtxStops && AtxWall >= 0)
		{
			FAtxState& A = AtxWalls.FindOrAdd(AtxWall);
			if (A.Prog.Num() == 0) InitAtx(A, TexKey, int32(AtxWall >> 32), int32(AtxWall & 0xffffffff));
			A.Mids.Add(MID);
			if (!A.Frame.IsEmpty()) if (UTexture2D* T = AtxFrame(A.Frame)) MID->SetTextureParameterValue(TEXT("Tex"), T);
		}
	}
	Materials.Add(Key, MID);
	return MID;
}

void AOutlawsLevelActor::InitAtx(FAtxState& A, const FString& Stem, int32 Sector, int32 Wall)
{
	A.Stem = Stem;
	A.Sector = Sector;
	A.Wall = Wall;
	A.Pc = 0;
	A.Wait = 0.f;
	A.bHalted = false;
	bool bStops = false;
	if (AssetsRef) OutlawsFormats::ParseAtx(*AssetsRef, Stem + TEXT(".atx"), A.Prog, bStops);
	RunAtx(A, 0.f);                    // to its first frame (or its first STOP)
	SetActorTickEnabled(true);
}

void AOutlawsLevelActor::RunAtx(FAtxState& A, float Dt)
{
	using EKind = OutlawsFormats::FOutlawsAtxInstr::EKind;
	if (A.Prog.Num() == 0 || A.bHalted) return;
	A.Wait -= Dt;
	int32 Guard = 0;
	while (!A.bHalted && A.Wait <= 0.f && Guard++ < 64)
	{
		if (!A.Prog.IsValidIndex(A.Pc))
		{
			A.bHalted = true;          // off the end: it holds its last frame
			break;
		}
		const OutlawsFormats::FOutlawsAtxInstr& I = A.Prog[A.Pc];
		switch (I.Kind)
		{
		case EKind::Rate: A.Rate = I.Value; ++A.Pc; break;
		case EKind::Texture: ApplyAtxFrame(A, I.Name); A.Wait += 1.f / FMath::Max(1.f, A.Rate); ++A.Pc; break;
		case EKind::Stop: A.bHalted = true; break;       // (Pc stays on the STOP: NEXT_STOP steps past it)
		case EKind::Goto: A.Pc = int32(I.Value); break;
		case EKind::Sound:
			if (A.Sector >= 0) PlayWav(FPaths::GetBaseFilename(I.Name), WallMidpoint(A.Sector, A.Wall));
			++A.Pc;
			break;
		}
	}
	if (A.Wait < -1.f) A.Wait = 0.f;   // (a long hitch: no burst of frames to catch up)
}

void AOutlawsLevelActor::ApplyAtxFrame(FAtxState& A, const FString& File)
{
	A.Frame = File;
	UTexture2D* T = AtxFrame(File);
	if (!T) return;
	for (const TWeakObjectPtr<UMaterialInstanceDynamic>& M : A.Mids)
	{
		if (M.IsValid()) M->SetTextureParameterValue(TEXT("Tex"), T);
	}
}

UTexture2D* AOutlawsLevelActor::AtxFrame(const FString& File)
{
	const FString Key = TEXT("ATX:") + File.ToUpper();
	if (const TObjectPtr<UTexture2D>* Have = Textures.Find(Key)) return *Have;
	if (!AssetsRef) return nullptr;
	FOutlawsTexInfo Info;
	bool bWasHd = false;
	UTexture2D* T = OutlawsTextures::LoadLevelTexture(*AssetsRef, File, bHDTextures, SpritePalette, false, Info, bWasHd);
	if (T) Textures.Add(Key, T);
	return T;
}

void AOutlawsLevelActor::RefreshAtxFrames()
{
	// after a texture-look change (the frames are loaded in the look of the moment): every program shows its frame again
	TArray<FString> Stale;
	for (const TPair<FString, TObjectPtr<UTexture2D>>& Kv : Textures) if (Kv.Key.StartsWith(TEXT("ATX:"))) Stale.Add(Kv.Key);
	for (const FString& K : Stale) Textures.Remove(K);
	for (TPair<FString, FAtxState>& Kv : AtxLoops) if (!Kv.Value.Frame.IsEmpty()) ApplyAtxFrame(Kv.Value, Kv.Value.Frame);
	for (TPair<int64, FAtxState>& Kv : AtxWalls) if (!Kv.Value.Frame.IsEmpty()) ApplyAtxFrame(Kv.Value, Kv.Value.Frame);
}

void AOutlawsLevelActor::TickAtx(float Dt)
{
	OL_PERF_SCOPE("Level: animated textures");
	for (TPair<FString, FAtxState>& Kv : AtxLoops) RunAtx(Kv.Value, Dt);
	for (TPair<int64, FAtxState>& Kv : AtxWalls) RunAtx(Kv.Value, Dt);
}

void AOutlawsLevelActor::AtxWallMessage(int32 Sector, int32 Wall, const FOutlawsInfMessage& Msg)
{
	FAtxState* A = AtxWalls.Find(int64(FOutlawsBuildFilter::Key(Sector, Wall)));
	if (!A || A->Prog.Num() == 0) return;
	if (Msg.Type == EOutlawsInfMsg::NextStop)
	{
		// still running between STOPs (a second push before the first's frames ran): on to that STOP at once, so the
		// wall keeps step with the INF counter it shows (the cliff's bar switches)
		if (!A->bHalted) RunAtx(*A, 1000.f);
		if (A->bHalted) ++A->Pc;           // past the STOP it holds on
	}
	else if (Msg.Type == EOutlawsInfMsg::GotoStop)
	{
		A->Pc = Msg.Arg;
	}
	else if (Msg.Type == EOutlawsInfMsg::PrevStop)
	{
		// back to the STOP before the one it holds on (the last one from the first): a bar switch slid the other way
		// (user 2026-09-29).  The frame there is the last TEXTURE before that STOP, shown at once
		using EKind = OutlawsFormats::FOutlawsAtxInstr::EKind;
		const int32 N = A->Prog.Num();
		int32 Prev = -1;
		for (int32 i = A->Pc - 1; i >= 0 && Prev < 0; --i) if (A->Prog[i].Kind == EKind::Stop) Prev = i;
		for (int32 i = N - 1; i > A->Pc && Prev < 0; --i) if (A->Prog[i].Kind == EKind::Stop) Prev = i;
		if (Prev < 0) return;
		int32 Tex = -1;
		for (int32 i = Prev - 1; i >= 0 && Tex < 0; --i) if (A->Prog[i].Kind == EKind::Texture) Tex = i;
		for (int32 i = N - 1; i > Prev && Tex < 0; --i) if (A->Prog[i].Kind == EKind::Texture) Tex = i;
		if (Tex >= 0) ApplyAtxFrame(*A, A->Prog[Tex].Name);
		A->Pc = Prev;
		A->bHalted = true;
		A->Wait = 0.f;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT ATX %s wall %d/%d: PREV_STOP -> instruction %d, frame %s"), *A->Stem, Sector, Wall, A->Pc, *A->Frame);
		return;
	}
	else
	{
		return;
	}
	A->bHalted = false;
	A->Wait = 0.f;
	RunAtx(*A, 0.f);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT ATX %s wall %d/%d: %s -> instruction %d, frame %s"), *A->Stem, Sector, Wall,
		Msg.Type == EOutlawsInfMsg::NextStop ? TEXT("NEXT_STOP") : TEXT("GOTO_STOP"), A->Pc, *A->Frame);
}

FVector AOutlawsLevelActor::WallMidpoint(int32 Sector, int32 Wall) const
{
	const FOutlawsLevel* Lv = Inf ? &Inf->Live : LevelData.Get();
	if (!Lv || !Lv->Sectors.IsValidIndex(Sector) || !Lv->Sectors[Sector].Walls.IsValidIndex(Wall)) return FVector::ZeroVector;
	const FOutlawsSector& S = Lv->Sectors[Sector];
	const FOutlawsWall& W = S.Walls[Wall];
	if (!S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) return FVector::ZeroVector;
	const FVector2D M = (S.Verts[W.V1] + S.Verts[W.V2]) * 0.5;
	return FOutlawsGeometry::ToUE(M.X, (S.Floor.Y + S.Ceil.Y) * 0.5, M.Y);
}

int32 AOutlawsLevelActor::AddSections(UProceduralMeshComponent* Mesh, const TMap<FString, FOutlawsMeshSection>& Sections, bool bCollision,
	UMaterialInterface* Opaque, UMaterialInterface* Masked, int32& OutTris, int32 FirstSection, TArray<int32>* OutCollide)
{
	TArray<FString> Keys;
	Sections.GetKeys(Keys);
	Keys.Sort();
	int32 Index = FirstSection;
	TArray<int32> Collide;
	static const bool bOldWaterMidTint = OLFlags::Param(TEXT("OLOldWaterMidTint"));    // (-OLOldWaterMidTint: as before, for shots)
	// (REQ-SECTORLIGHT) a lit sector's section of the static world: kept, to be drawn at its light (SplitWorldDraws moves
	// its solid ones to parts of their own)
	auto NoteLight = [this, Mesh](const FOutlawsMeshSection& S, int32 At)
	{
		if (S.LightElev < 0 || Mesh != WorldMesh) return;
		FLightFace& F = LightFaces.AddDefaulted_GetRef();
		F.Mesh = Mesh;
		F.Section = At;
		F.Elev = S.LightElev;
		F.Off = S.LightOff;
		F.Wall = S.LightWall;
		F.Pos = S.Positions;
		F.Col = S.Colors;
		F.Shown = S.Colors.Num() > 0 ? S.Colors[0].R : -1.f;
		F.Box = FBox(S.Positions);
	};
	for (const FString& K : Keys)
	{
		const FOutlawsMeshSection& All = Sections[K];
		if (All.Triangles.Num() == 0)
		{
			continue;
		}
		{
			const FOutlawsMeshSection& S = All;
			// '~' sections (see-through mid textures) never collide: the opening's flags decide (Blockers)
			Mesh->CreateMeshSection_LinearColor(Index, S.Positions, S.Triangles, S.Normals, S.UVs, S.Colors, TArray<FProcMeshTangent>(), false);
			NoteLight(S, Index);
			if (bCollision && !K.StartsWith(TEXT("~"))) Collide.Add(Index);
			// see-through mid textures ('~') take the glass material: their alpha is their opacity
			UMaterialInterface* Parent = S.bMasked ? Masked : Opaque;
			if (S.bMasked && K.StartsWith(TEXT("~")) && WorldGlassMaterial && Masked == WorldMaskedMaterial) Parent = WorldGlassMaterial;
			// under water (the builder grouped the faces its underwater sectors own): that water's tinted instance; the
			// see-through mids too (BUG-WATERMIDTINT: bars under the water kept their own colours among tinted walls;
			// M_OLR_WorldGlass has the world's OLWaterTint node since, tools\olue\ue_olue_glass_watertint.py)
			const bool bNoTint = Opaque != WorldMaterial || (bOldWaterMidTint && Parent == WorldGlassMaterial);
			Mesh->SetMaterial(Index, MaterialFor(Parent, S.Texture, bNoTint ? -1 : S.WaterGroup, S.AtxWall));
			OutTris += S.NumTriangles();
			++Index;
		}
	}
	// the see-through mids drawn a second time, after every other section (their numbers unchanged): their solid parts
	// (bars, slats, frames) masked at 0.95, writing depth, so a near fence is never drawn behind a far one (user
	// 2026-10-01: the glass alone is translucent and layered in the order the level was built); the glass over them
	// keeps the HD panes half see-through
	if (WorldMaskedSolidMaterial && WorldGlassMaterial && Masked == WorldMaskedMaterial)
	{
		for (const FString& K : Keys)
		{
			const FOutlawsMeshSection& S = Sections[K];
			if (S.Triangles.Num() == 0 || !S.bMasked || !K.StartsWith(TEXT("~"))) continue;
			Mesh->CreateMeshSection_LinearColor(Index, S.Positions, S.Triangles, S.Normals, S.UVs, S.Colors, TArray<FProcMeshTangent>(), false);
			NoteLight(S, Index);
			Mesh->SetMaterial(Index, MaterialFor(WorldMaskedSolidMaterial, S.Texture, bOldWaterMidTint || Opaque != WorldMaterial ? -1 : S.WaterGroup, S.AtxWall));
			OutTris += S.NumTriangles();
			++Index;
		}
	}
	if (OutCollide) OutCollide->Append(Collide);     // (the caller adds more sections, then cooks)
	else CookCollisionOnce(Mesh, Collide);
	return Index;
}

void AOutlawsLevelActor::CookCollisionOnce(UProceduralMeshComponent* Mesh, const TArray<int32>& Collide)
{
	// (TEXLOAD) each section made with collision cooks the whole mesh's collision again (UProceduralMeshComponent::
	// UpdateCollision, synchronous): n sections, n cooks of a growing mesh. The sections are made without it, marked,
	// and the mesh cooked once by setting the last marked section again.
	if (!Mesh || Collide.Num() == 0) return;
	for (const int32 i : Collide)
	{
		if (FProcMeshSection* S = Mesh->GetProcMeshSection(i)) S->bEnableCollision = true;
	}
	if (const FProcMeshSection* Last = Mesh->GetProcMeshSection(Collide.Last()))
	{
		const FProcMeshSection Copy = *Last;
		Mesh->SetProcMeshSection(Collide.Last(), Copy);
	}
}

void AOutlawsLevelActor::SplitWorldDraws()
{
	// (CPU1, the draw census 2026-10-04) the one world mesh draws every section whenever any of it is in view: 47 to 96
	// draws, most of the frame's, at every census spot, while sprites and items were already culled one by one. Each solid
	// section is drawn instead by a mesh of its own (no collision: the world mesh keeps every face for that) which the
	// renderer culls by its own bounds; the world mesh keeps drawing only its see-through sections (glass, tint), so their
	// sorting is as before. The faces, materials and order are the same, so the picture is too.
	// (REQ-SECTORLIGHT) a lit sector's solid sections are drawn by parts of their own whatever the setting: the world mesh
	// collides, and a section changed there would cook its collision again
	const bool bAllParts = WorldMesh && CVarOLWorldParts.GetValueOnGameThread() != 0 && !OLFlags::Param(TEXT("OLNoWorldParts"));
	if (!WorldMesh || (!bAllParts && LightFaces.Num() == 0)) return;
	TMap<int32, UProceduralMeshComponent*> PartOf;
	for (const FLightFace& F : LightFaces) if (F.Mesh.Get() == WorldMesh) PartOf.Add(F.Section, nullptr);
	// (REQ-SECTORLIGHT, cheaper: at first 1.87 ms a frame in cliff on the PC, 3.49 ms on the Quest's settings, most of it
	// vertex colours sent again at each change of level) a light's solid sections go in one part for the light, each as
	// a copy at every level the light reaches, only the copy at its present level shown: a change of level shows another
	// copy and sends no vertex (TickSectorLights).  -OLOldLightRedraw: a part a section, its colours sent at each change
	static const bool bOldRedraw = OLFlags::Param(TEXT("OLOldLightRedraw"));
	TMap<int32, int32> FaceOf;                 // a lit world section -> its LightFaces index
	for (int32 i = 0; i < LightFaces.Num(); ++i) if (LightFaces[i].Mesh.Get() == WorldMesh) FaceOf.Add(LightFaces[i].Section, i);
	TMap<int32, TArray<int32>> LitBy;          // a light (its elevator) -> its solid world sections
	int32 Parts = 0;
	for (int32 s = 0; s < WorldMesh->GetNumSections(); ++s)
	{
		const FProcMeshSection* Sec = WorldMesh->GetProcMeshSection(s);
		UMaterialInterface* Mat = WorldMesh->GetMaterial(s);
		if (!Sec || !Mat || !Sec->bSectionVisible || Sec->ProcIndexBuffer.Num() == 0) continue;
		if (!bAllParts && !PartOf.Contains(s)) continue;
		const EBlendMode Blend = Mat->GetBlendMode();
		if (Blend != BLEND_Opaque && Blend != BLEND_Masked) continue;
		const int32* Fi = bOldRedraw || !Inf ? nullptr : FaceOf.Find(s);
		if (Fi && Inf->Elevators.IsValidIndex(LightFaces[*Fi].Elev))
		{
			LitBy.FindOrAdd(LightFaces[*Fi].Elev).Add(s);
			continue;
		}
		FProcMeshSection Copy = *Sec;
		Copy.bEnableCollision = false;
		UProceduralMeshComponent* P = NewMeshComponent(TEXT("OutlawsWorldPart"), false);
		P->SetProcMeshSection(0, Copy);
		P->SetMaterial(0, Mat);
		ObjectMeshes.Add(P);           // (it goes with the level: Clear)
		WorldMesh->SetMeshSectionVisible(s, false);
		if (UProceduralMeshComponent** Was = PartOf.Find(s)) *Was = P;
		++Parts;
	}
	for (FLightFace& F : LightFaces)
	{
		UProceduralMeshComponent* const* P = F.Mesh.Get() == WorldMesh ? PartOf.Find(F.Section) : nullptr;
		if (P && *P) { F.Mesh = *P; F.Section = 0; }
	}
	int32 LitSections = 0, Copies = 0;
	for (const TPair<int32, TArray<int32>>& Kv : LitBy)
	{
		// the light's whole levels: its stops, and every level between its lowest and highest as it moves (one that jumps,
		// SPEED 0 or less, shows its stops alone); a level none of them holds (a message off the stops) is drawn in a hidden
		// copy when it comes (TickSectorLights)
		const FOutlawsElevator& E = Inf->Elevators[Kv.Key];
		const int32 NowLv = FMath::FloorToInt(E.Current);
		TArray<int32> Lvs = { NowLv };
		int32 Lo = NowLv, Hi = NowLv;
		for (int32 k = 0; k < Inf->Inf.Items[E.Item].Classes[E.ClassIndex].Stops.Num(); ++k)
		{
			const int32 V = FMath::FloorToInt(Inf->StopValue(E, k));
			Lvs.AddUnique(V);
			Lo = FMath::Min(Lo, V);
			Hi = FMath::Max(Hi, V);
		}
		for (int32 v = Lo; E.Speed > 0.0 && v <= Hi; ++v) Lvs.AddUnique(v);
		UProceduralMeshComponent* P = NewMeshComponent(TEXT("OutlawsWorldPart"), false);
		ObjectMeshes.Add(P);           // (it goes with the level: Clear)
		int32 n = 0;
		for (const int32 s : Kv.Value)
		{
			FLightFace& F = LightFaces[FaceOf[s]];
			const FProcMeshSection* Sec = WorldMesh->GetProcMeshSection(s);
			UMaterialInterface* Mat = WorldMesh->GetMaterial(s);
			// the levels this face is drawn at (its sector's offset and its wall's light: some meet at the ends), 16 at most
			const int32 Now = FOutlawsLevel::LightLevel(NowLv + F.Off, F.Wall);
			F.CopyLv = { Now };
			for (const int32 v : Lvs) if (F.CopyLv.Num() < 16) F.CopyLv.AddUnique(FOutlawsLevel::LightLevel(v + F.Off, F.Wall));
			F.Mesh = P;
			F.Section0 = n;
			F.ShownCopy = 0;
			F.Section = n;
			F.Shown = float(Now) / 31.f;
			for (int32 k = 0; k < F.CopyLv.Num(); ++k)
			{
				FProcMeshSection Copy = *Sec;
				Copy.bEnableCollision = false;
				Copy.bSectionVisible = k == 0;
				// (the vertex colours as the builder makes them, CreateMeshSection_LinearColor's: no sRGB; alpha is the swirl's mark)
				const uint8 B = FLinearColor(float(F.CopyLv[k]) / 31.f, 0.f, 0.f, 1.f).ToFColor(false).R;
				for (FProcMeshVertex& V : Copy.ProcVertexBuffer) V.Color.R = V.Color.G = V.Color.B = B;
				P->SetProcMeshSection(n + k, Copy);
				P->SetMaterial(n + k, Mat);
			}
			n += F.CopyLv.Num();
			WorldMesh->SetMeshSectionVisible(s, false);
			++LitSections;
		}
		Copies += n;
		++Parts;
	}
	if (LitBy.Num() > 0)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT sector lights drawn by %d parts: %d sections, %d copies at their levels"), LitBy.Num(), LitSections, Copies);
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT world drawn as %d parts by texture (%d sections left in the world mesh)"), Parts, WorldMesh->GetNumSections() - (Parts - LitBy.Num() + LitSections));
}

uint32 GetOlGcCount();
#if WITH_OUTLAWS_TESTS
bool OlExitAt(const TCHAR* Point);      // (OutlawsLevelActorInf.cpp, -OLExitAt)
#endif

void AOutlawsLevelActor::Build()
{
	ON_SCOPE_EXIT { OutlawsShipSmoke::NoteBuilt(this); };     // (REQ-SHIPLOG: -OLShipSmoke only; every way out)
	OutlawsPerf::FLoading Loading;
	UOutlawsStats::OnLevelBuild(this);        // (the level's end panels: a new level's counts start afresh)
	Clear();
	LoadDefaultAssets();
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	// stray sprites from an earlier build of this actor (e.g. duplicated into a PIE world)
	for (TActorIterator<AOutlawsSpriteActor> It(World); It; ++It)
	{
		if (It->GetOwner() == this)
		{
			It->Destroy();
		}
	}
	for (TActorIterator<AOutlawsEnemy> It(World); It; ++It)
	{
		if (It->GetOwner() == this)
		{
			It->Destroy();
		}
	}

	const double T0 = FPlatformTime::Seconds();
	OutlawsTextures::ResetLoadStats();
	TArray<FString> Report;
	const FString Root = FOutlawsAssets::FindInstallRoot(InstallPath);
	if (Root.IsEmpty())
	{
		LastBuildReport = TEXT("ERROR: Outlaws Remastered install not found (set InstallPath)");
		UE_LOG(LogOutlaws, Error, TEXT("%s"), *LastBuildReport);
		return;
	}
	TSharedPtr<FOutlawsAssets> Assets = FOutlawsAssets::GetShared(Root);
	if (!Assets)
	{
		LastBuildReport = FString::Printf(TEXT("ERROR: no LAB archives under %s"), *Root);
		UE_LOG(LogOutlaws, Error, TEXT("%s"), *LastBuildReport);
		return;
	}
	const FOutlawsAssets& A = *Assets;
	AssetsRef = Assets;
	const double T1 = FPlatformTime::Seconds();

	// ---- level text ----------------------------------------------------------------------------
	FString Lvt, Obt;
	if (!A.GetText(LevelName + TEXT(".lvt"), Lvt))
	{
		LastBuildReport = FString::Printf(TEXT("ERROR: %s.lvt not found"), *LevelName);
		UE_LOG(LogOutlaws, Error, TEXT("%s"), *LastBuildReport);
		return;
	}
	A.GetText(LevelName + TEXT(".obt"), Obt);
	FOutlawsLevel L;
	L.ParseLvt(Lvt);
	L.ParseObt(Obt);
	LevelData = MakeShared<FOutlawsLevel>(L);
	if (LevelData)
	{
		LowestFloor = 1e9f;
		for (const FOutlawsSector& S : LevelData->Sectors) LowestFloor = FMath::Min(LowestFloor, float(S.Floor.Y * FOutlawsGeometry::UnitsToCm));
		if (LowestFloor > 1e8f) LowestFloor = 0.f;
	}
	const double T2 = FPlatformTime::Seconds();

	// ---- textures ------------------------------------------------------------------------------
	TArray<FColor> Palette;
	if (!FindSpritePalette(A, Palette))
	{
		Palette.SetNum(256);
		for (int32 i = 0; i < 256; ++i) Palette[i] = FColor(i, i, i, 255);
		Report.Add(TEXT("WARNING: sprite palette not found, using greyscale"));
	}
	SpritePalette = Palette;
	BuildPaletteGrades(A, L);
	BuildFullBright(A, L);                 // (REQ-FIREBRIGHT: before the objects are made)
	const FString SkyStem = FOutlawsGeometry::PickSkyTexture(L);
	TMap<FString, FOutlawsTexInfo> TexInfo;
	int32 NumPcx = 0, NumHd = 0, NumMissing = 0;
	{
		OL_PERF_SCOPE("Load: level textures");
		// (TEXLOAD) both looks of each texture read, decoded and mipped across the cores in batches (one read of its
		// files for both), then made textures here in the level's order; a batch at a time bounds the memory held
		struct FTexJob
		{
			FString Name, Stem;
			bool bSky = false;
			bool bSeeThrough = false;      // (BUG-GLASS8BIT) a see-through mid somewhere: its PCX look takes the level's SHADES
			FOutlawsTexInfo Info;
			OutlawsTextures::FPreparedTex Lo, Hd;
		};
		// BUG-GLASS8BIT (user 28 Oct, snapshot 2: townex's broken windows grey with Remastered Graphics off): a broken window's
		// shards are palette entries 2 to 4, which the original draws as the level's SHADES (white at 25, 50 and 75 %) over
		// the room behind; the PCX look had them solid grey.  The see-through mids' PCX looks take the SHADES (of every masked
		// mid in the story, only the broken windows' frames use entries 1 to 15)
		static const bool bOldGlass8Bit = OLFlags::Param(TEXT("OLOldGlass8Bit"));     // (-OLOldGlass8Bit: solid as before, for shots)
		TSet<FString> SeeThrough;
		for (const FOutlawsSector& S : L.Sectors)
		{
			for (const FOutlawsWall& W : S.Walls)
			{
				if ((W.Flags1 & OutlawsFlags::WallMidTex) && W.Adjoin >= 0 && L.Textures.IsValidIndex(W.Mid)) SeeThrough.Add(FPaths::GetBaseFilename(L.Textures[W.Mid]).ToUpper());
			}
		}
		TArray<FTexJob> Jobs;
		TSet<FString> Seen;
		for (const FString& Name : L.Textures)
		{
			const FString Stem = FPaths::GetBaseFilename(Name).ToUpper();
			if (Textures.Contains(Stem) || Seen.Contains(Stem)) continue;
			Seen.Add(Stem);
			FTexJob& J = Jobs.AddDefaulted_GetRef();
			J.Name = Name;
			J.Stem = Stem;
			J.bSky = Stem == SkyStem;
			J.bSeeThrough = SeeThrough.Contains(Stem) && !bOldGlass8Bit;
		}
		const bool bSerial = OLFlags::Param(TEXT("OLTexSerial"));
		const int32 Batch = bSerial ? 1 : FMath::Clamp(FTaskGraphInterface::Get().GetNumWorkerThreads() * 2, 4, 32);
		for (int32 First = 0; First < Jobs.Num(); First += Batch)
		{
			const int32 N = FMath::Min(Batch, Jobs.Num() - First);
			ParallelFor(N, [&](int32 i)
			{
				FTexJob& J = Jobs[First + i];
				OutlawsTextures::PrepareLevelTexture(A, J.Name, true, true, Palette, J.bSky, J.Info, J.Lo, J.Hd, J.bSeeThrough ? &L.Shades : nullptr);
			}, bSerial);
			for (int32 i = 0; i < N; ++i)
			{
				FTexJob& J = Jobs[First + i];
				// both looks, so the Remastered Graphics switch swaps them live (the UV maths use the PCX size
				// either way)
				UTexture2D* T = OutlawsTextures::CreatePrepared(J.Lo);
				UTexture2D* H = OutlawsTextures::CreatePrepared(J.Hd);
				if (!T && H) T = H;     // the PCX failed: the HD look and its size stand in
				if (!T)
				{
					++NumMissing;
					UE_LOG(LogOutlaws, Log, TEXT("OLRT texture missing: %s%s"), *J.Name, J.bSky ? TEXT(" (the sky)") : TEXT(""));
					continue;
				}
				TexturesLo.Add(J.Stem, T);
				if (H) TexturesHd.Add(J.Stem, H);
				Textures.Add(J.Stem, bHDTextures && H ? H : T);
				TexInfo.Add(J.Stem, J.Info);
				(H ? NumHd : NumPcx)++;
			}
		}
	}
	const double T3 = FPlatformTime::Seconds();

	TexInfoCache = TexInfo;
	BuildWaterTints(L, A, Palette);

	// ---- INF: doors, lifts, moving floors and ceilings are split out of the static world ---
	FOutlawsBuildFilter StaticFilter;
	FString InfText;
	LoadGameTables();
	LoadShootTables();
	ApplyStartKit();
	// the level ITM's TAUNT_TYPE picks the bad guys' taunt recordings (TOWN -> c105t.wav)
	TauntSuffix.Reset();
	{
		FString LevelItm;
		if (A.GetText(LevelName + TEXT(".itm"), LevelItm))
		{
			FString Func;
			TMap<FString, FString> Data;
			OutlawsFormats::ParseItm(LevelItm, Func, Data);
			const FString Type = Data.FindRef(TEXT("TAUNT_TYPE")).ToUpper();
			TauntSuffix = Type == TEXT("TOWN") ? TEXT("t") : Type == TEXT("MINE") ? TEXT("m") : Type == TEXT("CLIFF") ? TEXT("c") : TEXT("");
		}
	}
	bInfEndsLevel = false;
	FOutlawsGeometry::SectorLight.Reset();     // (REQ-SECTORLIGHT: BuildInf names this level's lit sectors)
	if (bRunInf && A.GetText(LevelName + TEXT(".inf"), InfText))
	{
		BuildInf(L, InfText, StaticFilter);
		TArray<FString> InfLines;
		InfText.ParseIntoArrayLines(InfLines);
		for (const FString& Line : InfLines)
		{
			const FString U = Line.TrimStartAndEnd().ToUpper();
			if (U.StartsWith(TEXT("MESSAGE:")) && (U.Contains(TEXT("END_LEVEL")) || U.Contains(TEXT("COMPLETE")))) { bInfEndsLevel = true; break; }
		}
	}
	if (Inf)
	{
		TSet<int32> Moving;
		for (const TPair<int32, TArray<int32>>& Kv : Inf->ElevatorsBySector) Moving.Add(Kv.Key);
		ClampThinOverlaps(Inf->Live, Moving);
		FloodCache.bValid = false;          // (QSPEED-3: ceilings changed outside the INF's own moves)
	}
	const FOutlawsLevel& Built = Inf ? Inf->Live : L;
	CollectPanes(Built, StaticFilter);       // glass panes leave the world mesh for their own
	// sectors sharing space with others they do not adjoin leave the world mesh for their own (UpdateOverlaps)
	TSet<int32> OverlapSet;
	FindOverlaps(Built, StaticFilter, OverlapSet);
	BuildZoneGates(Built);                  // (BUG-MILLCLIMB: the sides of a level that shares space across a gate)
	for (const int32 Os : OverlapSet)
	{
		StaticFilter.SkipFloor.Add(Os);
		StaticFilter.SkipCeil.Add(Os);
		for (int32 w = 0; w < Built.Sectors[Os].Walls.Num(); ++w) StaticFilter.SkipWalls.Add(FOutlawsBuildFilter::Key(Os, w));
	}

	// ---- geometry ------------------------------------------------------------------------------
	FOutlawsGeometry G;
	G.Build(Built, TexInfo, bReverseWinding, &StaticFilter);
	const double T4 = FPlatformTime::Seconds();

	int32 WorldTris = 0, WaterTris = 0, SkyTris = 0;
	WorldMesh = NewMeshComponent(TEXT("OutlawsWorld"), true);
	int32 TintIndex = 0;
	TArray<int32> WorldCollide;
	{
		OL_PERF_SCOPE("Load: world mesh + collision");
		TintIndex = AddSections(WorldMesh, G.World, true, WorldMaterial, WorldMaskedMaterial, WorldTris, 0, &WorldCollide);
	}
	if (bTintedGlass && G.Tint.Triangles.Num() > 0)
	{
		// the glass of TINT walls (the ones that do not break: those carry their own, below)
		WorldMesh->CreateMeshSection_LinearColor(TintIndex, G.Tint.Positions, G.Tint.Triangles, G.Tint.Normals, G.Tint.UVs, G.Tint.Colors, TArray<FProcMeshTangent>(), false);
		WorldMesh->SetMaterial(TintIndex, GlassTintMaterial());
	}
	{
		OL_PERF_SCOPE("Load: world mesh + collision");
		CookCollisionOnce(WorldMesh, WorldCollide);
	}
	{
		OL_PERF_SCOPE("Load: world parts");
		SplitWorldDraws();
	}
	// invisible walls from the opening flags: NOPASS stops walkers (and shots, unless PROJECTILE_OK),
	// a mid texture without PROJECTILE_OK stops shots, FENCE stops bad guys only (their capsules block
	// the EnemyBlocker channel, the player's ignores it: Config/DefaultEngine.ini)
	for (const TPair<FString, FOutlawsMeshSection>& Kv : G.Blockers)
	{
		if (UProceduralMeshComponent* M = MakeBlocker(Kv.Key, Kv.Value)) ObjectMeshes.Add(M);
	}
	{
		OL_PERF_SCOPE("Load: panes");
		BuildPanes(Built, TexInfo, Palette);
	}
	BuildScrolls(Built, TexInfo);
	for (const FPane& Pn : Panes)
	{
		if (Pn.Mesh) ObjectMeshes.Add(Pn.Mesh);
	}
	if (G.Water.Num() > 0)
	{
		WaterMesh = NewMeshComponent(TEXT("OutlawsWater"), false);
		AddSections(WaterMesh, G.Water, false, WaterMaterial, WaterMaterial, WaterTris);
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT sky: %s (%s)"), *SkyStem, Textures.Contains(SkyStem) ? *Textures[SkyStem]->GetName() : TEXT("not loaded"));
	if (!SkyStem.IsEmpty() && Textures.Contains(SkyStem))
	{
		TMap<FString, FOutlawsMeshSection> Sky;
		FOutlawsMeshSection& S = Sky.Add(SkyStem);
		S.Texture = SkyStem;
		FOutlawsGeometry::BuildSky(L, TexInfo.FindRef(SkyStem), G.Bounds, bReverseWinding, S);
		// the dome about its own centre, so it can turn in place (the clouds drift)
		const FVector SkyCentre(G.Bounds.IsValid ? G.Bounds.GetCenter().X : 0.0, G.Bounds.IsValid ? G.Bounds.GetCenter().Y : 0.0, 0.0);
		for (FVector& P : S.Positions) P -= SkyCentre;
		SkyMesh = NewMeshComponent(TEXT("OutlawsSky"), false);
		SkyMesh->SetWorldLocation(SkyCentre);
		AddSections(SkyMesh, Sky, false, SkyMaterial, SkyMaterial, SkyTris);
		// the sky's surfaces show the same sky by the direction looked in (the dome's mapping), so the dome and
		// they meet seamlessly, and they hide what lies beyond them as the original's sky does
		UMaterialInterface* SurfBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_SkySurface.M_OLR_SkySurface"));
		// (a flat, view-scrolled sky as the original drew it was tried on 2026-09-27 and looked worse in the
		// headset than the dome: the dome stays, and the picture is fitted to it by what it is)
		if (SurfBase)
		{
			SkySurfaceMid = MaterialFor(SurfBase, SkyStem);
			const FOutlawsTexInfo SkyInfo = TexInfo.FindRef(SkyStem);
			// round the dome: twice the original's texels a turn (once stretched it too far)
			SkySurfaceMid->SetScalarParameterValue(TEXT("Repeats"), 2.0f * FMath::Max(1.f, float(L.Parallax.X) / FMath::Max(1, SkyInfo.W)));
			// up the dome, by what the picture is (user 2026-09-27): a tiling sky (clouds all over) goes round
			// twice from horizon to zenith; a horizon sky (a horizon along its bottom, plain above) shows once,
			// stretched to the zenith when tall, or over the lower half with its top colour above, so no second
			// horizon hangs at 45 degrees
			OutlawsTextures::FOutlawsSkyLook Look;
			FString SkyName;
			for (const FString& N : L.Textures) if (FPaths::GetBaseFilename(N).ToUpper() == SkyStem) { SkyName = N; break; }
			const bool bLooked = !SkyName.IsEmpty() && OutlawsTextures::SkyLook(A, SkyName, Palette, Look);
			const float RepV = Look.bTiles || SkyInfo.H < 512 ? 2.f : 1.f;
			SkySurfaceMid->SetScalarParameterValue(TEXT("RepV"), RepV);
			SkySurfaceMid->SetScalarParameterValue(TEXT("Wrap"), Look.bTiles ? 1.f : 0.f);
			SkySurfaceMid->SetVectorParameterValue(TEXT("FadeColour"), Look.Top);
			// the dome shows the same picture the same way, so it and the sky surfaces meet seamlessly
			if (SkyMesh) SkyMesh->SetMaterial(0, SkySurfaceMid);
			UE_LOG(LogOutlaws, Log, TEXT("OLRT sky %s %dx%d: %s (band %.1f, seam %.1f, rows %.1f), %.0f up the dome"), *SkyStem, SkyInfo.W, SkyInfo.H,
				!bLooked ? TEXT("not read, taken as tiling") : Look.bTiles ? TEXT("tiling") : RepV < 1.5f ? TEXT("a horizon sky, stretched to the zenith") : TEXT("a horizon sky, its top colour above"),
				Look.Band, Look.Seam, Look.Adj, RepV);
			// (BUG-TRAINSKY, user 2026-10-12) the train's sky picture looks broken through the dome and cloud layer: this level alone
			// gets a plain gradient dome, sky blue from the top to halfway down, then to a lighter, whiter blue at the horizon
			bGradientSky = LevelName.Equals(TEXT("train"), ESearchCase::IgnoreCase);
			if (bGradientSky)
			{
				constexpr int32 GW = 4, GH = 512;
				FColor Zenith(52, 102, 158, 255), Horizon(134, 150, 170, 255);      // (what the world's unlit pass lifts to about 70 159 229 and 212 230 243)
				FString Over;
				TArray<FString> Num;
				if (OLFlags::Value(TEXT("OLGradSky"), Over, false) && Over.ParseIntoArray(Num, TEXT(",")) == 6)      // (tuning: top r,g,b then horizon r,g,b)
				{
					Zenith = FColor(uint8(FCString::Atoi(*Num[0])), uint8(FCString::Atoi(*Num[1])), uint8(FCString::Atoi(*Num[2])), 255);
					Horizon = FColor(uint8(FCString::Atoi(*Num[3])), uint8(FCString::Atoi(*Num[4])), uint8(FCString::Atoi(*Num[5])), 255);
				}
				TArray<FColor> Rows;
				Rows.SetNum(GW * GH);
				for (int32 y = 0; y < GH; ++y)
				{
					// row 0 is the zenith, the last row the horizon; flat to halfway, then eased to the horizon colour
					const float T = FMath::Clamp((float(y) / float(GH - 1) - 0.5f) * 2.f, 0.f, 1.f);
					const float K = T * T * (3.f - 2.f * T);
					const FColor C(uint8(FMath::RoundToInt(FMath::Lerp(float(Zenith.R), float(Horizon.R), K))), uint8(FMath::RoundToInt(FMath::Lerp(float(Zenith.G), float(Horizon.G), K))),
						uint8(FMath::RoundToInt(FMath::Lerp(float(Zenith.B), float(Horizon.B), K))), 255);
					for (int32 x = 0; x < GW; ++x) Rows[y * GW + x] = C;
				}
				UTexture2D* Grad = OutlawsTextures::MakeTextureBGRA(GW, GH, Rows, false, TF_Bilinear, TA_Clamp);
				Textures.Add(TEXT("_GRADSKY"), Grad);
				// (an animated sky stem hands its frames to every material of the stem each tick: this one is the gradient's now)
				int32 Freed = 0;
				if (FAtxState* Ax = AtxLoops.Find(SkyStem)) Freed = Ax->Mids.RemoveAll([this](const TWeakObjectPtr<UMaterialInstanceDynamic>& M) { return M.Get() == SkySurfaceMid; });
				UE_LOG(LogOutlaws, Log, TEXT("OLRT sky %s: taken off its texture animation: %d"), *SkyStem, Freed);
				SkySurfaceMid->SetScalarParameterValue(TEXT("Repeats"), 1.f);
				SkySurfaceMid->SetScalarParameterValue(TEXT("RepV"), 1.f);
				SkySurfaceMid->SetScalarParameterValue(TEXT("Wrap"), 0.f);
				GradSkyTop = FLinearColor(Zenith);
				UE_LOG(LogOutlaws, Log, TEXT("OLRT sky %s: the gradient dome (BUG-TRAINSKY): sky blue to halfway down, then to a whiter blue at the horizon"), *SkyStem);
				// BUG-TRAINSKY8BIT (user 28 Oct, snapshot 3: with Remastered Graphics off a pale gradient over a bright blue band):
				// the 8-bit look's sky is one flat blue, the mean of the top row of the picture on the level's outer walls (the
				// walls with no neighbour round the open sky's sectors: the train's GCTBGRND, its own sky GWTRNSKY the same blue),
				// so the walls' sky meets it; read here, not written in.  The gradient stays for the remastered look
				{
					TMap<FString, int32> Count;
					for (const FOutlawsSector& Sec : L.Sectors)
					{
						if (!(Sec.Flags & OutlawsFlags::ExtCeiling)) continue;
						for (const FOutlawsWall& W : Sec.Walls) if (W.Adjoin < 0 && L.Textures.IsValidIndex(W.Mid)) Count.FindOrAdd(L.Textures[W.Mid])++;
					}
					int32 Best = 0;
					FlatSkyFrom = SkyName;
					for (const TPair<FString, int32>& Kv : Count) if (Kv.Value > Best) { Best = Kv.Value; FlatSkyFrom = Kv.Key; }
					OutlawsTextures::FOutlawsSkyLook Back;
					FlatSkyPx = OutlawsTextures::SkyLook(A, FlatSkyFrom, Palette, Back) ? Back.TopRow : Zenith;
					FlatSkyColour = FLinearColor(FlatSkyPx);
					TArray<FColor> Flat;
					Flat.Init(FlatSkyPx, 16);
					Textures.Add(TEXT("_FLATSKY"), OutlawsTextures::MakeTextureBGRA(4, 4, Flat, false, TF_Bilinear, TA_Clamp));
					UE_LOG(LogOutlaws, Log, TEXT("OLRT sky %s: the 8-bit look's flat sky (BUG-TRAINSKY8BIT): %d %d %d, the top row of %s (on %d outer walls)"), *SkyStem,
						FlatSkyPx.R, FlatSkyPx.G, FlatSkyPx.B, *FlatSkyFrom, Best);
				}
				ApplyGradientSkyLook();
			}
			if (GetWorld() && GetWorld()->IsGameWorld() && bCloudLayer && !bGradientSky && !OLFlags::Param(TEXT("OLNoCloudLayer")))
			{
				SetUpCloudLayer(!bLooked || Look.bTiles, SkyInfo.H > 0 ? float(SkyInfo.W) / float(SkyInfo.H) : 1.f);
			}
			if (G.SkyFaces.Triangles.Num() > 0)
			{
				SkySurfaceMesh = NewMeshComponent(TEXT("OutlawsSkySurfaces"), false);
				SkySurfaceMesh->CreateMeshSection_LinearColor(0, G.SkyFaces.Positions, G.SkyFaces.Triangles, G.SkyFaces.Normals, G.SkyFaces.UVs, G.SkyFaces.Colors, TArray<FProcMeshTangent>(), false);
				SkySurfaceMesh->SetMaterial(0, SkySurfaceMid);
				UE_LOG(LogOutlaws, Log, TEXT("OLRT sky surfaces: %d triangles"), G.SkyFaces.Triangles.Num() / 3);
			}
		}
	}
	{
		OL_PERF_SCOPE("Load: overlap parts + explode walls");
		BuildOverlapParts(Built, TexInfo, StaticFilter);
		BuildExplodeWalls(Built, TexInfo);
	}
	BuildSkyCeilingCollision(Built);
	if (Inf)
	{
		CreateMoverMeshes();
	}
	const double T5 = FPlatformTime::Seconds();

	// ---- objects -------------------------------------------------------------------------------
	TMap<FString, TSharedPtr<F3doModel>> Models;
	int32 NumEnemies = 0, NumGenerators = 0;
	const bool bGameWorld = World->IsGameWorld();
	int32 NumSprites = 0, NumMeshes = 0, NumNone = 0, NumOutside = 0, NumRiding = 0, NumPickups = 0, NumOtherDifficulty = 0;
	TArray<int32> ModelPickups;            // (BUG-KNIFEPICKUP) the placed 3D ground items' pickups, numbered after the loop
	NumItemModels = 0;
	SpinningModels.Reset();
	WavingModels.Reset();
	WavingStems.Reset();
	FlickerModels.Reset();
	LampFlames.Reset();
	HangModels.Reset();
	BendModels.Reset();
	PropWobbles.Reset();
	// player start: a PLAYER object on the map (miner keeps a second one in its off-map staging
	// area), preferably placed for this difficulty; ties go to the last one
	SpawnDifficulty = PendingSpawnDifficulty >= 0 ? static_cast<EOutlawsDifficulty>(FMath::Clamp(PendingSpawnDifficulty, 0, 2)) : Difficulty;
	PendingSpawnDifficulty = -1;
	const uint32 DiffBit = OutlawsDifficulty::ObjectBit(SpawnDifficulty);
	const FOutlawsObject* Start = nullptr;
	int32 StartScore = -1;
	for (const FOutlawsObject& O : L.Objects)
	{
		if (O.Name != TEXT("PLAYER"))
		{
			continue;
		}
		const bool bOnMap = L.SectorById(O.SectorId) || L.SectorAt(O.Pos.X, O.Pos.Y, O.Pos.Z);
		const int32 Score = (bOnMap ? 2 : 0) + (O.OnDifficulty(DiffBit) ? 1 : 0);
		if (Score >= StartScore)
		{
			Start = &O;
			StartScore = Score;
		}
	}
	if (Start)
	{
		PlayerStartLocation = FOutlawsGeometry::ToUE(Start->Pos.X, Start->Pos.Y, Start->Pos.Z) + FVector(0, 0, 110.0);
		PlayerStartYaw = Start->Yaw;     // Outlaws yaw (north = 0, clockwise) == UE yaw
		bHavePlayerStart = true;
	}
	// one boss per boss function (user 2026-09-27): a level may place an easy variant beside the full one for the same
	// outlaw (townex ESANCHEZ on Good only, one SANCHEZ row on every difficulty; miner EMARY/MARY, EDRDEATH/DRDEATH),
	// and keyed by name both were spawned, so killing Sanchez left the level waiting on the other.  The slot's actor
	// is the name placed most specifically for this difficulty; every variant's rows are places he may appear
	TMap<FString, FString> BossNameFor;          // boss function (lower case) -> the name that becomes its actor
	{
		TMap<FString, FString> FuncOf;
		TMap<FString, int32> Score;             // "func|name" -> rows (x1000 when placed for this difficulty alone)
		for (const FOutlawsObject& O : L.Objects)
		{
			if (!O.OnDifficulty(DiffBit)) continue;
			const FString Key = O.Name.ToLower();
			if (!FuncOf.Contains(Key))
			{
				FString BItm, BFunc;
				TMap<FString, FString> BData;
				ResolveAnim(A, O.Name, &BItm);
				OutlawsFormats::ParseItm(BItm, BFunc, BData);
				FuncOf.Add(Key, BFunc.ToLower());
			}
			const uint32 D = O.Flags2 & 0xF0000000u;
			Score.FindOrAdd(FuncOf[Key] + TEXT("|") + Key) += (D != 0 && D != 0xF0000000u) ? 1000 : 1;
		}
		TMap<FString, int32> Best;
		for (const TPair<FString, int32>& Kv : Score)
		{
			FString Func, Name;
			Kv.Key.Split(TEXT("|"), &Func, &Name);
			if (Kv.Value > Best.FindRef(Func)) { Best.Add(Func, Kv.Value); BossNameFor.Add(Func, Name); }
		}
	}
	// co-op (OutlawsCoopExtras.cpp, user 2026-10-02): the host's extra bad guys, each beside one of the level's own (whose
	// light and starting ways he takes; his kind is his own), made once the level's own are all made (none in single player:
	// the list is empty there)
	// (REQ-SPMULT, user 2026-10-05: single player's own multiplier, Settings > Gameplay, makes the same list here)
	const bool bSoloExtras = bGameWorld && bSpawnEnemies && !bFrontEnd && !UOutlawsCoop::IsActive(this) && PrepareSoloExtras();
	const bool bCoopExtras = bGameWorld && bSpawnEnemies && CoopExtras.Num() > 0 && (UOutlawsCoop::IsActive(this) || bSoloExtras);
	TMap<int32, FCoopCopySource> CopySources;
	// R10 (OutlawsLevelActorRando.cpp): the randomizer's roll for this start puts other names on the rows it changes (the
	// ordinary bad guys' kinds, the movable pickups' items); a bad guy keeps his own starting ways and drop (Roll.Keep)
	FRandoRoll Roll;
	RandoSum = 0;
	if (bGameWorld && !bFrontEnd) PrepareRando(L, InfText, DiffBit, Roll);
	for (const FOutlawsObject& O : L.Objects)
	{
		if (!O.OnDifficulty(DiffBit))
		{
			++NumOtherDifficulty;        // placed for another difficulty
			continue;
		}
		const FOutlawsSector* Sec = L.SectorById(O.SectorId);
		if (!Sec)
		{
			// unknown sector id (miner writes 0): use the sector the object stands in
			Sec = L.SectorAt(O.Pos.X, O.Pos.Y, O.Pos.Z);
		}
		const int32 Light = Sec ? FOutlawsLevel::LightLevel(Sec->Ambient) : 31;
		const FVector Pos = FOutlawsGeometry::ToUE(O.Pos.X, O.Pos.Y, O.Pos.Z);
		if (!Sec)
		{
			++NumOutside;   // off-map staging for scripted scenes; the engine never draws it
			continue;
		}
		FString Itm;
		FString Anim = ResolveAnim(A, O.Name, &Itm);
		bool bCellGen = false;             // (BUG-HOMCELL) a generator of one still figure, shown when the INF turns it on
		// bad guys: live enemies in a game world (the editor shows them as still sprites); enemy
		// generators (Gen_New) spawn them over time in the original and are left out for now
		FString ItmFunc;
		TMap<FString, FString> ItmData;
		OutlawsFormats::ParseItm(Itm, ItmFunc, ItmData);
		if (const TMap<FString, FString>* Keep = Roll.Keep.Find(int32(&O - L.Objects.GetData())))
		{
			for (const TPair<FString, FString>& Kv : *Keep)
			{
				if (Kv.Value.IsEmpty()) ItmData.Remove(Kv.Key);
				else ItmData.Add(Kv.Key, Kv.Value);
			}
		}
		// (REQ-RANDOBOSS) a named outlaw the randomizer put in a bad guy's place: built as a bad guy with the outlaw's own
		// picture, health, weapon and taunts on the boss template; none of the boss's level-end hooks (no boss slot, no
		// late arrival, his death ends nothing)
		const bool bRandomBoss = Roll.RandomBoss.Contains(int32(&O - L.Objects.GetData()));
		if (bRandomBoss)
		{
			TMap<FString, FString> BossData;
			BossData.Add(TEXT("FOV"), TEXT("179"));
			BossData.Add(TEXT("TAUNT_TIMER_TIME"), TEXT("15"));
			BossData.Add(TEXT("HEALTH"), TEXT("6"));
			BossData.Add(TEXT("MOVE_INACCURACY"), TEXT("5"));
			BossData.Add(TEXT("WEAPON_INACCURACY"), TEXT("1"));
			BossData.Append(ItmData);
			ItmData = MoveTemp(BossData);
			UE_LOG(LogOutlaws, Log, TEXT("OLRANDO random boss %s (%s) at row %d: a bad guy that ends nothing"), *O.Name, *ItmFunc, int32(&O - L.Objects.GetData()));
			ItmFunc = TEXT("BGY_New");
		}
		if (ItmFunc.Equals(TEXT("Gen_New"), ESearchCase::IgnoreCase) && bGameWorld)
		{
			// a sound generator (SWIND1A: TYPE AMB_SOUND, GENERATE WIND1.WAV, FREQ -7, MAX_GENERATEES 2)
			// plays its wav now and then, at most MAX_GENERATEES at once; AMB_SOUND is heard everywhere,
			// POINT_SOUND from where it stands
			const FString Type = ItmData.FindRef(TEXT("TYPE")).ToUpper();
			if (Type == TEXT("AMB_SOUND") || Type == TEXT("POINT_SOUND"))
			{
				FSoundGen Gen;
				Gen.Wav = FPaths::GetBaseFilename(ItmData.FindRef(TEXT("GENERATE")));
				Gen.Freq = ItmData.Contains(TEXT("FREQ")) ? FCString::Atof(*ItmData[TEXT("FREQ")]) : 5.f;
				Gen.Pos = Pos;
				Gen.bPoint = Type == TEXT("POINT_SOUND");
				Gen.Max = FMath::Max(1, FCString::Atoi(*ItmData.FindRef(TEXT("MAX_GENERATEES"))));
				// the first one: soon for the continuous sounds, some while in for the occasional ones
				Gen.Wait = Gen.Freq < 0.f ? FMath::FRandRange(0.2f, FMath::Max(0.3f, -Gen.Freq)) : FMath::FRandRange(3.f, 60.f / FMath::Max(0.1f, Gen.Freq));
				// an ambient sound is heard everywhere, so a second generator of the same one only doubled how often it
				// came (the mill's two swolves2: a 6.85 s howl every 6 s, never quiet; user 2026-09-28): one each
				const bool bDup = !Gen.bPoint && SoundGens.ContainsByPredicate([&](const FSoundGen& G) { return !G.bPoint && G.Wav.Equals(Gen.Wav, ESearchCase::IgnoreCase); });
				if (!Gen.Wav.IsEmpty() && !bDup) SoundGens.Add(Gen);
				continue;
			}
			if (Type == TEXT("ITEM"))
			{
				// FREQ / RAND / DELAY in seconds (negative: a fixed number; INFERRED as for the sound ones)
				FItemGen Gen;
				Gen.Name = FPaths::GetBaseFilename(ItmData.FindRef(TEXT("GENERATE")));
				Gen.Anim = OutlawsFormats::ResolveAnim(A, Gen.Name);
				Gen.Pos = Pos;
				Gen.Yaw = O.Yaw;
				Gen.Light = Light;
				Gen.Freq = FMath::Max(1.f, FMath::Abs(FCString::Atof(*ItmData.FindRef(TEXT("FREQ")))));
				Gen.Rand = FMath::Abs(FCString::Atof(*ItmData.FindRef(TEXT("RAND"))));
				Gen.Wait = FMath::Abs(FCString::Atof(*ItmData.FindRef(TEXT("DELAY")))) + FMath::FRandRange(0.f, Gen.Rand);
				Gen.Max = FMath::Max(1, FCString::Atoi(*ItmData.FindRef(TEXT("MAX_GENERATEES"))));
				if (Gen.Anim.EndsWith(TEXT(".nwx"), ESearchCase::IgnoreCase)) ItemGens.Add(Gen);
				continue;
			}
			// (user 2026-10-04, BUG-HOMCELL: the office's cells stayed empty) a generator of a picture (GENERATE <x>.NWX, MASTER
			// OFF: the office's na<X> standing outlaws and nd<X> bodies in their cells, turned on by the porch stops' MASTER_ON):
			// its figure built where it stands, hidden till the INF turns it on (OnObjectMessage), once
			const FString GenFile = ItmData.FindRef(TEXT("GENERATE"));
			if (Type.IsEmpty() && GenFile.EndsWith(TEXT(".nwx"), ESearchCase::IgnoreCase) && ItmData.FindRef(TEXT("MASTER")).Equals(TEXT("OFF"), ESearchCase::IgnoreCase) && !O.Id.IsEmpty())
			{
				Anim = A.Has(GenFile) ? GenFile : OutlawsFormats::ResolveAnim(A, FPaths::GetBaseFilename(GenFile));
				ItmFunc = TEXT("Gen_Figure");
				bCellGen = true;
			}
			else if (bSpawnEnemies)
			{
				++NumGenerators;
				continue;
			}
		}
		// the boss (Sanchez, Two Feathers) [EXE 0x14006ae20]: every row is a place he may appear; the
		// first becomes the one real actor, hidden and still until most of the bad guys are dead
		// the named outlaws all run the boss logic (user 2026-09-27: only Sanchez and Two Feathers did)
		static const TSet<FString> BossFuncs = { TEXT("sanchez_new"), TEXT("twofeathers_new"), TEXT("bloodeye_new"), TEXT("bob_new"), TEXT("buckshot_new"),
			TEXT("drdeath_new"), TEXT("dynamite_new"), TEXT("farmer_new"), TEXT("mary_new"), TEXT("simms_new"), TEXT("slim_new"),
			TEXT("shbill_new"), TEXT("shblood_new"), TEXT("shchez_new"), TEXT("shdick_new"), TEXT("shmary_new") };
		const bool bBossRow = BossFuncs.Contains(ItmFunc.ToLower());
		// an outlaw's voice alone (RADIUS 0: the ranch's BOBVCE, an invisible 999-health DrDeath_New that only taunts
		// for Bob): no boss slot (it cannot be shot, so the ranch could never be won), no body, no shots, not a kill
		if (bBossRow && bGameWorld && bSpawnEnemies && ItmData.Contains(TEXT("RADIUS")) && FCString::Atof(*ItmData[TEXT("RADIUS")]) <= 0.f)
		{
			if (Anim.EndsWith(TEXT(".nwx"), ESearchCase::IgnoreCase) && SpawnEnemy(O.Name, ItmData, Anim, Pos, O.Yaw, Light))
			{
				if (AOutlawsEnemy* E = Enemies.Last().Get())
				{
					E->bVoiceOnly = true;
					E->bStationary = true;
					E->SetActorEnableCollision(false);
					E->GetCharacterMovement()->DisableMovement();
					UE_LOG(LogOutlaws, Log, TEXT("OLRT %s is a voice only (radius 0)"), *O.Name);
				}
			}
			continue;
		}
		if (bBossRow && bGameWorld && bSpawnEnemies)
		{
			double BossDz = 0.0;
			RideFor(Sec->Index, O.Pos.Y, false, BossDz);
			const FVector BossFeet = Pos + FVector(0.0, 0.0, BossDz);
			const FString Func = ItmFunc.ToLower();
			const FString* Chosen = BossNameFor.Find(Func);
			FBossSlot* Slot = BossSlots.FindByPredicate([&](const FBossSlot& S) { return S.Func == Func; });
			if (!Slot) { Slot = &BossSlots.AddDefaulted_GetRef(); Slot->Func = Func; Slot->Name = Chosen ? Chosen->ToUpper() : O.Name; }
			Slot->Spawns.Add(BossFeet);
			if (!Boss.IsValid()) BossSpawns.Add(BossFeet);
			if (!Slot->Actor.IsValid() && (!Chosen || O.Name.Equals(*Chosen, ESearchCase::IgnoreCase)) && Anim.EndsWith(TEXT(".nwx"), ESearchCase::IgnoreCase))
			{
				// SanchezStatic [EXE 0x1408fe860]: the BGY template but for these, then the ITM over it
				TMap<FString, FString> BossData;
				BossData.Add(TEXT("FOV"), TEXT("179"));
				BossData.Add(TEXT("TAUNT_TIMER_TIME"), TEXT("15"));
				BossData.Add(TEXT("HEALTH"), TEXT("6"));
				BossData.Add(TEXT("MOVE_INACCURACY"), TEXT("5"));
				BossData.Add(TEXT("WEAPON_INACCURACY"), TEXT("1"));
				BossData.Append(ItmData);
				if (SpawnEnemy(O.Name, BossData, Anim, BossFeet, O.Yaw, Light))
				{
					if (AOutlawsEnemy* E = Enemies.Last().Get())
					{
						E->bBoss = true;
						E->bStationary = Func == TEXT("slim_new") || Func == TEXT("drdeath_new") || Func == TEXT("bob_new");
						E->bPeekShooter = Func == TEXT("bob_new");
						E->bBehindDoor = E->bPeekShooter;      // (frozen till his door opens: WakeCurtainBoss)
						E->bFallInCutscene = BossFallsInCutscene(LevelName, Func);
						E->bHunter = !E->bStationary;
						// only Sanchez (the town) and Two Feathers (the cliff) hide until most of the bad guys are dead and
						// then turn up at one of their many places; every other named outlaw has a single place and is
						// there from the start (user 2026-09-28: the train's Tim Bloodworth never showed)
						const bool bLateArrival = Func == TEXT("sanchez_new") || Func == TEXT("twofeathers_new");
						E->SetDormant(bLateArrival);
						E->bHuntOnceAlerted = !bLateArrival;
						Slot->Actor = E;
						if (!Boss.IsValid()) Boss = E;
						++TotalBadGuys;
					}
				}
			}
			continue;
		}
		double EnemyRideDz = 0.0;
		// the townsfolk (Civilian_New: man1, woman1) behave as the chickens do (user 2026-09-27)
		const bool bChicken = ItmFunc.Equals(TEXT("Chicken_New"), ESearchCase::IgnoreCase) || ItmFunc.Equals(TEXT("Civilian_New"), ESearchCase::IgnoreCase);
		if (bChicken)
		{
			// ChickenStatic [EXE 0x1408fe260]: the enemy logic with no shooting and no drop: it clucks and
			// follows the player about
			ItmData.FindOrAdd(TEXT("SHOOT_CHOR")) = TEXT("-1");
			ItmData.FindOrAdd(TEXT("DROP_ITEM")) = TEXT("null");
		}
		const bool bEnemyLogic = ItmFunc.Equals(TEXT("BGY_New"), ESearchCase::IgnoreCase) || bChicken;
		if (bEnemyLogic)
		{
			RideFor(Sec->Index, O.Pos.Y, false, EnemyRideDz);
		}
		if (bEnemyLogic && bGameWorld && bSpawnEnemies &&
			Anim.EndsWith(TEXT(".nwx"), ESearchCase::IgnoreCase) && SpawnEnemy(O.Name, ItmData, Anim, Pos + FVector(0.0, 0.0, EnemyRideDz), O.Yaw, Light))
		{
			++NumEnemies;
			if (bRandomBoss) RandomBossActors.Add(Enemies.Last());
			// the boss comes at 80 % of the bad guys: chickens are not counted
			if (bChicken)
			{
				if (AOutlawsEnemy* E = Enemies.Last().Get()) E->bNotBadGuy = true;
			}
			else
			{
				++TotalBadGuys;
				if (bCoopExtras)
				{
					FCoopCopySource& Src = CopySources.Add(int32(&O - L.Objects.GetData()));
					Src.Enemy = Enemies.Num() - 1;
					Src.Name = O.Name;
					Src.Data = ItmData;
					Src.Anim = Anim;
					Src.Light = Light;
				}
			}
			continue;
		}
		if (Anim.EndsWith(TEXT(".nwx"), ESearchCase::IgnoreCase))
		{
			// props that react to shots carry every choreography (chor 1 is the reaction)
			const bool bProp = ItmFunc.Equals(TEXT("Inv_Object"), ESearchCase::IgnoreCase);
			const TSharedPtr<OutlawsTextures::FSpriteAtlas> Atlas = GetAtlas(Anim, bProp);
			if (!Atlas || !Atlas->Texture || !SpriteQuad || !SpriteMaterial)
			{
				++NumNone;
				continue;
			}
			const double Wcm = Atlas->CellW / 16.0 * FOutlawsGeometry::UnitsToCm;
			const double Hcm = Atlas->CellH / 16.0 * FOutlawsGeometry::UnitsToCm;
			const bool bNegHeight = ItmData.Contains(TEXT("HEIGHT")) && FCString::Atof(*ItmData[TEXT("HEIGHT")]) < 0.f;
			// (BUG-GLANTERN) a pickup is never hung by its name: the ground lantern (miner, FUNC Inv_GroundObject, no HEIGHT) took
			// "LANTERN" for a hanging lamp and was drawn its cell's height under its point, sunk 77 cm into the floor.  The Jedi
			// engine draws it on its insertion point like every sprite; on the floor like any other pickup.  -OLOldGlantern: as
			// it was.  (Of the sprites hung by name alone in every level, it is the only pickup.)
			static const bool bOldGlantern = OLFlags::Param(TEXT("OLOldGlantern"));
			const bool bPickup = !bOldGlantern && ItmData.Contains(TEXT("PICKUP"));
			const bool bHang = (IsHanging(O.Name, Itm) && !bPickup) || bNegHeight;
			FVector Loc = Pos;
			// BUG-SCONCELOW (user 2026-10-15: wall sconces too low): the Jedi engine draws every sprite on its frame's
			// insertion point (sprite_drawFrame); a negative HEIGHT only hangs its collision under the point.  The sconce
			// is drawn 48 px above its point and 7 below, so its top on the point put it 3 units low.  The lamps, the
			// church bell and the broken rope have their points at or near their tops, but the old rule hung the atlas
			// cell (as tall as the tallest frame of every choreography): they rise 17 to 21 cm onto their points.  A
			// hanging sprite without a negative HEIGHT (by name only: BOBCHAND, BOBLAMP, LANTERN1) keeps its top on it.
			// -OLOldHangTop: the old rule, for before shots
			static const bool bOldHangTop = OLFlags::Param(TEXT("OLOldHangTop"));
			const int32 Cell0 = Atlas->Grid.Num() > 0 ? Atlas->Grid[0] : 0;
			if (bNegHeight && !bOldHangTop && Atlas->CellH > 0 && Atlas->InsBottomPx.IsValidIndex(Cell0))
			{
				Loc.Z -= Atlas->InsBottomPx[Cell0] * Hcm / Atlas->CellH;
				UE_LOG(LogOutlaws, Verbose, TEXT("OLRT %s hangs on its insertion point, %.1f cm over the old top-on-point place"), *O.Name, Loc.Z - (Pos.Z - Hcm));
			}
			else if (bHang) Loc.Z -= Hcm;
			if (!bHang)
			{
				// placed a little under its floor (ranch and miner canteens and ammo, a unit down): the Jedi engine draws a
				// sprite over the floor of the sector it is in, so the original showed it whole; here the floor would hide
				// it.  Up onto the floor; one deeper than 1.5 units is somewhere else on purpose (user 2026-10-02)
				const double FloorY = L.SurfaceY(*Sec, FVector2D(O.Pos.X, O.Pos.Z), true);
				if (FloorY > O.Pos.Y && FloorY - O.Pos.Y <= 1.5)
				{
					Loc.Z += (FloorY - O.Pos.Y) * FOutlawsGeometry::UnitsToCm;
				}
			}
			Loc.Z += 0.5;
			const FTransform Xf(FRotator::ZeroRotator, Loc, FVector(1.0, Wcm, Hcm));
			FActorSpawnParameters P;
			P.Owner = this;
			P.ObjectFlags |= RF_Transient;
			P.bDeferConstruction = true;
			P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			AOutlawsSpriteActor* S = World->SpawnActor<AOutlawsSpriteActor>(AOutlawsSpriteActor::StaticClass(), Xf, P);
			if (!S)
			{
				continue;
			}
			S->OutlawsName = O.Name;
			S->OutlawsYaw = O.Yaw;
			S->LightLevel = Light;
			S->HomeSector = Sec->Index;            // (V1: the OB's own sector)
			S->AtlasCols = Atlas->Cols;
			S->AtlasRows = Atlas->Rows;
			S->Directions = Atlas->Dirs;
			S->FrameCount = Atlas->Frames;
			S->FPS = Atlas->FPS;
			// (BUG-CWEXPLOOP) a one-shot's script ends it (-OLOldExpLoop: every prop loops, as before)
			static const bool bOldExpLoop = OLFlags::Param(TEXT("OLOldExpLoop"));
			S->EndOp = bOldExpLoop ? 0 : Atlas->EndOp;
			S->Grid = Atlas->Grid;
			S->SpriteTexture = Atlas->Texture;
			float FullShare = 0.f;
			S->bFullBright = IsFullBrightArt(*Atlas, &FullShare);      // (REQ-FIREBRIGHT: a flame whatever the sector's light)
			if (S->bFullBright) UE_LOG(LogOutlaws, Log, TEXT("OLRT full bright: %s (%s, %.0f %% of its art), sector light %d"), *O.Name, *Anim, FullShare * 100.f, Light);
			for (const OutlawsTextures::FSpriteAtlas::FChor& C : Atlas->Chors)
			{
				FOutlawsSpriteChor& D = S->Chors.AddDefaulted_GetRef();
				D.Dirs = C.Dirs;
				D.Frames = C.Frames;
				D.FPS = C.FPS;
				D.GridStart = C.GridStart;
				D.Length = C.Length;
				D.HeightPx = C.HeightPx;
				D.Events = C.Events;
			}
			S->ChorGrid = Atlas->ChorGrid;
			if (!bHang)
			{
				S->SetFeet(Atlas->BelowPx, Atlas->FootPx, Atlas->CellH); S->SetFeetAcross(Atlas->SidePx2, Atlas->CellW);   // (on the frames' insertion points, user 2026-10-02)
			}
			S->Quad->SetStaticMesh(SpriteQuad);
			S->Quad->SetMaterial(0, SpriteMaterialFor(S, *Atlas));     // (REQ-FIREBRIGHT2: a flame's mask, per pixel)
			S->FinishSpawning(Xf);
			if (bCellGen)
			{
				S->SetActorHiddenInGame(true);
				CellGens.Add(O.Id.ToUpper(), S);
				UE_LOG(LogOutlaws, Log, TEXT("OLRT %s (%s): a figure generator, %s, hidden till turned on"), *O.Name, *O.Id, *Anim);
			}
			// on a lift floor or in a door that carries objects: ride it (the mesh was built at the
			// authored position, so the authored transform is the offset from the moving piece)
			double RideDz = 0.0;
			if (USceneComponent* Ride = RideFor(Sec->Index, O.Pos.Y, bHang, RideDz))
			{
				S->AddActorWorldOffset(FVector(0.0, 0.0, RideDz));
				S->AttachToComponent(Ride, FAttachmentTransformRules::KeepRelativeTransform);
				++NumRiding;
			}
#if WITH_EDITOR
			S->SetActorLabel(FString::Printf(TEXT("%s_%s"), *O.Name, *O.Id));
			S->SetFolderPath(TEXT("OutlawsRuntime"));
#endif
			SpawnedActors.Add(S);
			if (bGameWorld)
			{
				// a 3D stand-in where one has been made (items, barrels; hanging lamps, the chandelier, stalactites too: user
				// 2026-10-01; a hanging sprite stands at its foot, Loc.Z lowered by its height, and its model is made that tall)
				UseItemModel(S, Anim, WallFacingYaw(Pos, Sec ? Sec->Index : -1, O.Yaw));
			}
			if (bProp && bGameWorld)
			{
				AddProp(S, O.Name, ItmData, Sec->Index, bHang);
			}
			else if (bGameWorld && !bHang && S->Model && S->ModelStem.StartsWith(TEXT("ROCK")))
			{
				// the rocks are scenery (FUNC NULL), but a hand takes them up as a pot (user 2026-09-30)
				AddProp(S, O.Name, ItmData, Sec->Index, false);
				Props.Last().bGrabOnly = true;
			}
			if (RegisterPickup(S, O.Name, Itm))
			{
				Pickups.Last().Serial = PickupSerials++;
				++NumPickups;
			}
			if (!bHang)
			{
				FloorStandingSprites.Add(S);
			}
			++NumSprites;
		}
		else if (Anim.EndsWith(TEXT(".3do"), ESearchCase::IgnoreCase))
		{
			const FString Key = Anim.ToLower();
			TSharedPtr<F3doModel>* Found = Models.Find(Key);
			if (!Found)
			{
				TSharedPtr<F3doModel> M = MakeShared<F3doModel>();
				FString Text;
				if (!A.GetText(Anim, Text) || !Parse3do(Text, *M))
				{
					M.Reset();
				}
				Found = &Models.Add(Key, M);
			}
			if (!*Found)
			{
				++NumNone;
				continue;
			}
			TMap<FString, FOutlawsMeshSection> Sections;
			OutlawsTextures::Build3doSections(A, **Found, Light, bHDTextures, Palette, bReverseWinding, Textures, Sections);
			UProceduralMeshComponent* M = NewMeshComponent(TEXT("Outlaws3DO"), true);
			int32 Tris = 0;
			AddSections(M, Sections, true, WorldMaterial, WorldMaskedMaterial, Tris);
			double RideDz = 0.0;
			if (USceneComponent* Ride = RideFor(Sec->Index, O.Pos.Y, false, RideDz))
			{
				M->SetUsingAbsoluteLocation(false);
				M->SetUsingAbsoluteRotation(false);
				M->AttachToComponent(Ride, FAttachmentTransformRules::KeepRelativeTransform);
				M->SetRelativeLocationAndRotation(Pos + FVector(0.0, 0.0, RideDz), FRotator(0.0, O.Yaw, 0.0));
				++NumRiding;
			}
			else
			{
				M->SetWorldLocationAndRotation(Pos, FRotator(0.0, O.Yaw, 0.0));
			}
			ObjectMeshes.Add(M);
			if (ItmFunc.Equals(TEXT("shot_ProjectileDispatch"), ESearchCase::IgnoreCase) && bGameWorld)
			{
				// a stick of dynamite on the ground (gdynam.itm: PICKUP idynam, COUNT 1): shot, it goes off; walked over or
				// taken in the hand, it is one more stick (user 2026-09-29: they could not be picked up).  Its own actor,
				// so the pickups can hold and remove it
				ObjectMeshes.Remove(M);
				UProceduralMeshComponent* DM = nullptr;
				AActor* DA = MakeDynamiteActor(M, DM);
				M->DestroyComponent();
				M = DM;
				if (DA && DM)
				{
					FDynamite D;
					D.Mesh = DM;
					D.Where = DM->GetComponentLocation();
					Dynamite.Add(D);
					FPickup P;
					P.Actor = DA;
					P.Ground = O.Name.ToLower();
					FPickup::FEntry E;
					E.Item = FPaths::GetBaseFilename(ItmData.FindRef(TEXT("PICKUP"))).ToLower();
					if (E.Item.IsEmpty()) E.Item = TEXT("idynam");
					E.Count = ItmData.Contains(TEXT("COUNT")) ? FMath::Max(1, FCString::Atoi(*ItmData[TEXT("COUNT")])) : 1;
					E.Message = ItmData.Contains(TEXT("PICKUP_MSG")) ? FCString::Atoi(*ItmData[TEXT("PICKUP_MSG")]) : -1;
					E.Class = ItmData.FindRef(TEXT("ITEM_CLASS")).ToLower();
					P.Item = E.Item;
					P.Message = E.Message;
					P.Entries.Add(E);
					P.Serial = -1;                   // (a save keeps it by the dynamite list)
					Pickups.Add(P);
					++NumPickups;
				}
			}
			else if (bGameWorld && (ItmFunc.Equals(TEXT("Inv_GroundObject"), ESearchCase::IgnoreCase) || ItmFunc.Equals(TEXT("Inv_InstantGroundObject"), ESearchCase::IgnoreCase)))
			{
				// (BUG-KNIFEPICKUP, user 27 Oct: "the ones that spawn with the level can't be picked up") a ground item drawn
				// as a 3D model (gknife.itm: FUNC Inv_GroundObject, ANIM gknife.3do, PICKUP iknife, COUNT 1; the only one the
				// game has) was built as a piece of the level and never made a pickup, so only a knife thrown or dropped in
				// play (SpawnGroundItem) could be taken.  Its own actor, without collision as a dropped knife has, and the
				// pickup its ITM gives (the count and the carry rules as for any pickup).  Numbered after the sprites, below,
				// so a save made before names them as it did.  -OLOldKnifePickup: level scenery, as it was
				static const bool bOldKnifePickup = OLFlags::Param(TEXT("OLOldKnifePickup"));
				++PlacedGroundModels;
				if (bOldKnifePickup)
				{
					UE_LOG(LogOutlaws, Log, TEXT("OLRT placed %s at %s: level scenery, no pickup (-OLOldKnifePickup)"), *O.Name, *Pos.ToString());
				}
				else
				{
					ObjectMeshes.Remove(M);
					UProceduralMeshComponent* KM = nullptr;
					AActor* KA = MakeDynamiteActor(M, KM, TEXT("OutlawsGroundModel"));
					M->DestroyComponent();
					M = KM;
					if (KA && KM)
					{
						KM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
						if (RegisterPickup(KA, O.Name, Itm))
						{
							ModelPickups.Add(Pickups.Num() - 1);
							++NumPickups;
						}
						else
						{
							UE_LOG(LogOutlaws, Log, TEXT("OLRT placed %s at %s is not a pickup"), *O.Name, *Pos.ToString());
						}
					}
				}
			}
			if (!M) continue;
			++NumMeshes;
		}
		else
		{
			++NumNone;
		}
	}
	ModelSerialsFrom = PickupSerials;
	for (const int32 Pi : ModelPickups)
	{
		if (Pickups.IsValidIndex(Pi)) Pickups[Pi].Serial = PickupSerials++;
	}
	if (PlacedGroundModels > 0)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT placed 3D ground items: %d, %d of them pickups (serials from %d)"), PlacedGroundModels, ModelPickups.Num(), ModelSerialsFrom);
	}
	CoopExtrasFirst = -1;
	CoopExtrasMade = 0;
	CoopExtrasSum = 0;
	if (bCoopExtras)
	{
		NumEnemies += SpawnCoopExtras(Built, CopySources);
	}
	{
		OL_PERF_SCOPE("Sprites: HD atlas flush");
		OutlawsTextures::ReleaseAtlasCells();
	}
	const double T6 = FPlatformTime::Seconds();

	// ---- report ----------------------------------------------------------------------------------
	FString StatText;
	TArray<FString> StatKeys;
	G.Stats.GetKeys(StatKeys);
	StatKeys.Sort();
	for (const FString& K : StatKeys) StatText += FString::Printf(TEXT("%s=%d "), *K, G.Stats[K]);
	Report.Insert(FString::Printf(TEXT("%s from %s (%s): %d sectors, %d textures (%d PCX, %d HD, %d missing), %d objects"),
		*LevelName, *Root, *A.Where(LevelName + TEXT(".lvt")), L.Sectors.Num(), L.Textures.Num(), NumPcx, NumHd, NumMissing, L.Objects.Num()), 0);
	Report.Add(FString::Printf(TEXT("geometry: world %d sections %d tris, water %d tris, sky %d tris; %s"),
		G.World.Num(), WorldTris, WaterTris, SkyTris, *StatText));
	{
		// every level mesh this actor holds (a count that grows level after level is geometry left behind)
		TInlineComponentArray<UProceduralMeshComponent*> Meshes(this);
		Report.Add(FString::Printf(TEXT("level meshes held: %d"), Meshes.Num()));
	}
	Report.Add(FString::Printf(TEXT("triangulation: fallbacks %d, area error %.4f, suspect sectors %d"),
		G.TriangulationFallbacks, G.AreaError, G.SuspectSectors.Num()));
	if (GetWorld() && GetWorld()->IsGameWorld()) SettleItemModels();     // (the 3D stand-ins out of walls and each other)
	// each named outlaw: where he may stand, whether he is there or waits (user 2026-10-01: the canyon's never came)
	for (const FBossSlot& S : BossSlots)
	{
		const AOutlawsEnemy* SA = S.Actor.Get();
		Report.Add(FString::Printf(TEXT("boss slot %s (%s): %d places, %s"), *S.Name, *S.Func, S.Spawns.Num(),
			!SA ? TEXT("NO ACTOR (no row with his name and an .nwx)") : SA->IsDormant() ? TEXT("waits for 80 % of the kills") : *FString::Printf(TEXT("there from the start at %s"), *SA->GetActorLocation().ToString())));
	}
	Report.Add(FString::Printf(TEXT("objects: %d sprites (%d atlases), %d 3DO, %d without visual, %d off-map, %d riding movers, %d pickups, %d enemies (%d generators left out); difficulty %s, %d objects placed for other difficulties"),
		NumSprites, AtlasCache.Num(), NumMeshes, NumNone, NumOutside, NumRiding, NumPickups, NumEnemies, NumGenerators, OutlawsDifficulty::Name(SpawnDifficulty), NumOtherDifficulty));
	if (bGameWorld && !bFrontEnd) Report.Add(PlaceCollectBox(Built));     // (REQ-COLLECTBOX, OutlawsCollectBox.cpp)
	if (bGameWorld) BuildHorsePlan();             // (REQ-COOPHORSES: the other players' horses' places, OutlawsCoopHorses.cpp)
	Report.Add(TEXT("shootables: ") + ShootablesReport());
	Report.Add(FString::Printf(TEXT("3D item models: %d sprites stood in for"), NumItemModels));
	if (bHDTextures && bGameWorld)
	{
		SetHDTextures(true);      // the HD sprite atlases and every sprite's look
	}
	if (bGameWorld)
	{
		SetStochastic(bStochastic && !OLFlags::Param(TEXT("OLNoStochastic")));
	}
	if (bGameWorld)
	{
		if (!bFrontEnd) StartMusic(OutlawsMpPolish::LevelMusic(this, L.Music));          // (behind the main menu: the menu's music alone)
		if (!bFrontEnd) PrebuildForPlay();
		if (!bFrontEnd)
		{
			BuildFlash3D(A);           // (REQ-MUZZLEFLASH3D: the flashes cut from the shooters' sprites)
			Report.Add(Flash3DReport);
		}
	}
	if (Inf)
	{
		Report.Add(Inf->Summary() + FString::Printf(TEXT(", %d mover groups, %d message strings"), Groups.Num(), UserMessages.Num()));
		for (int32 i = 0; i < FMath::Min(5, Inf->Warnings.Num()); ++i) Report.Add(TEXT("INF warning: ") + Inf->Warnings[i]);
	}
	Report.Add(FString::Printf(TEXT("times ms: labs %.0f, parse %.0f, textures %.0f, geometry %.0f, meshes %.0f, objects %.0f, total %.0f"),
		(T1 - T0) * 1000, (T2 - T1) * 1000, (T3 - T2) * 1000, (T4 - T3) * 1000, (T5 - T4) * 1000, (T6 - T5) * 1000, (T6 - T0) * 1000));
	{
		// (TEXLOAD) the build's parts in the perf buckets too (the Quest's logcat loses the line), and one line for the load
		auto AddPhase = [](const TCHAR* Bucket, double Sec) { OutlawsPerf::Add(OutlawsPerf::Bucket(Bucket), uint64(Sec / FPlatformTime::GetSecondsPerCycle64())); };
		AddPhase(TEXT("Load: labs + parse"), T2 - T0);
		AddPhase(TEXT("Load: geometry"), T4 - T3);
		AddPhase(TEXT("Load: meshes"), T5 - T4);
		AddPhase(TEXT("Load: objects"), T6 - T5);
		const OutlawsTextures::FLoadStats TS = OutlawsTextures::GetLoadStats();
		Report.Add(FString::Printf(TEXT("level load: %s %.0f ms (textures built %d, from cache %d, decode ms %.0f, mips ms %.0f, upload ms %.0f, read ms %.0f, cache io ms %.0f; %s%s)"),
			*LevelName, (T6 - T0) * 1000, TS.Built, TS.FromCache, TS.DecodeMs, TS.MipsMs, TS.UploadMs, TS.ReadMs, TS.CacheIoMs,
			OutlawsTextures::CpuBC7() ? TEXT("BC7 on the CPU") : TEXT("BC7 on the GPU"), OLFlags::Param(TEXT("OLTexSerial")) ? TEXT(", serial") : TEXT("")));
	}
	LastBuildReport = FString::Join(Report, TEXT("\n"));
	for (const FString& Line : Report)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT %s"), *Line);
	}
}

#if WITH_EDITOR
void AOutlawsLevelActor::PostLoad()
{
	Super::PostLoad();
	if (HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject) || !bBuildInEditor)
	{
		return;
	}
	// build once the editor world is up (spawning actors inside PostLoad is not allowed)
	TWeakObjectPtr<AOutlawsLevelActor> Weak(this);
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak](float)
	{
		AOutlawsLevelActor* Self = Weak.Get();
		if (Self && Self->bBuildInEditor && Self->GetWorld() && Self->GetWorld()->WorldType == EWorldType::Editor)
		{
			Self->Build();
		}
		return false;   // run once
	}));
}

void AOutlawsLevelActor::PostEditChangeProperty(FPropertyChangedEvent& Event)
{
	Super::PostEditChangeProperty(Event);
	const FName N = Event.GetPropertyName();
	if (bBuildInEditor && GetWorld() && GetWorld()->WorldType == EWorldType::Editor &&
		(N == GET_MEMBER_NAME_CHECKED(AOutlawsLevelActor, LevelName) || N == GET_MEMBER_NAME_CHECKED(AOutlawsLevelActor, bHDTextures) ||
		 N == GET_MEMBER_NAME_CHECKED(AOutlawsLevelActor, Difficulty) ||
		 N == GET_MEMBER_NAME_CHECKED(AOutlawsLevelActor, InstallPath)))
	{
		Build();
	}
}
#endif

void AOutlawsLevelActor::BeginPlay()
{
	Super::BeginPlay();
	if (OLFlags::Param(TEXT("OLAdjoinScan")))
	{
		// (BUG-ADJOINDEFAULT) every level's walls built before and after the sign rule, logged; nothing else runs
		UOutlawsRuntimeLibrary::RunAdjoinScan(InstallPath);
		FGenericPlatformMisc::RequestExit(false);
		return;
	}
	if (OLFlags::Param(TEXT("OLWallFacingScan")))
	{
		// (BUG-WALLSFLIPPED) every level's walls facing out of their sector, before and after the winding fix; nothing else runs
		UOutlawsRuntimeLibrary::RunWallFacingScan(InstallPath);
		FGenericPlatformMisc::RequestExit(false);
		return;
	}
	// (-OLOldSkygap: BUG-SKYGAP's before-picture) door notches left out of the room's floor and ceiling
	FOutlawsGeometry::bOldSkygap = OLFlags::Param(TEXT("OLOldSkygap"));
	if (OLFlags::Param(TEXT("OLSmokeTest")))
	{
		bEnemiesActive = false;    // the smoke test wakes one when it gets to enemies
	}
	LoadSettings();                    // the menu settings the player saved last time
	// the game starts at the hideout, the story's first level (user 2026-09-27); test runs keep the map's own level
	if (!OLFlags::Param(TEXT("OLShots")) && !OLFlags::Param(TEXT("OLSmokeTest")))
	{
		LevelName = TEXT("hideout");
	}
	OLFlags::Value(TEXT("OLLevel"), LevelName);     // start in another level (tests)
	{
		FString D;       // -OLDifficulty=good|bad|ugly (tests)
		if (OLFlags::Value(TEXT("OLDifficulty"), D))
		{
			Difficulty = D.Equals(TEXT("good"), ESearchCase::IgnoreCase) ? EOutlawsDifficulty::Good : D.Equals(TEXT("ugly"), ESearchCase::IgnoreCase) ? EOutlawsDifficulty::Ugly : EOutlawsDifficulty::Bad;
		}
	}
	if (OLFlags::Param(TEXT("OLHD")))
	{
		bHDTextures = true;        // start with the remastered look
	}
	UOutlawsCoop::NotifyLevelBeginPlay(this);     // (co-op only: its launch flags, its lobby; nothing in single player)
	// a test run builds its level at once; the game proper opens on the front end (the opening movie, the main
	// menu), and Main Game brings the hideout (user 2026-09-27)
	const bool bTestRun = (OLFlags::Param(TEXT("OLShots")) || OLFlags::Param(TEXT("OLSmokeTest"))
		|| OLFlags::Param(TEXT("OLNoFrontEnd"))) && !OLFlags::Param(TEXT("OLFrontShot"))
		&& !OLFlags::Param(TEXT("OLStoryTest")) && !OLFlags::Param(TEXT("OLMpMenuShot"))
		&& !OLFlags::Param(TEXT("OLMpHostShot"))
		&& !OLFlags::Param(TEXT("OLExtrasTest"))          // (REQ-EXTRAS: from the title menu)
		&& !OLFlags::Has(TEXT("OLMenuOverlayProbe"))      // (REQ-LEGACYKPF: the title pages shot)
		&& !OLFlags::ValueIs(TEXT("OLBackClickTest"), TEXT("extras"))      // (REQ-NOUNLOCKABLES: Back on the Extras pages, from the title)
		&& !OLFlags::HasValue(TEXT("OLTitleLoadTest"));     // (REQ-TITLEMENU: the title menu tested)
	UWorld* GameWorld = GetWorld();
	if (bBuildOnBeginPlay && GameWorld && GameWorld->IsGameWorld() && FOutlawsAssets::CanAskForInstall() && FOutlawsAssets::FindInstallRoot(InstallPath).IsEmpty())
	{
		// a PC with no Outlaws where the game looks: a note in view, then (a second on, so the headset shows it) the
		// folder window on the desktop, which holds the game thread until it closes; then the start as below
		UTextRenderComponent* Note = NewObject<UTextRenderComponent>(this);
		Note->SetHorizontalAlignment(EHTA_Center);
		Note->SetVerticalAlignment(EVRTA_TextCenter);
		Note->SetWorldSize(6.f);
		Note->SetTextRenderColor(FColor(240, 210, 150));
		Note->SetText(FText::FromString(TEXT("Outlaws VR could not find your Outlaws.\nA window on the PC's desktop asks where it is installed.")));
		Note->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Note->SetCastShadow(false);
		Note->RegisterComponent();
		TWeakObjectPtr<AOutlawsLevelActor> Weak(this);
		TWeakObjectPtr<UTextRenderComponent> WeakNote(Note);
		const double AskAt = FPlatformTime::Seconds() + 1.0;
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Weak, WeakNote, AskAt, bTestRun](float)
		{
			AOutlawsLevelActor* Self = Weak.Get();
			UWorld* W = Self ? Self->GetWorld() : nullptr;
			if (!W)
			{
				return false;
			}
			APlayerController* NotePC = W->GetFirstPlayerController();
			if (WeakNote.IsValid() && NotePC && NotePC->PlayerCameraManager)
			{
				// 150 cm ahead of the eyes, facing them
				const FRotator Yaw(0.f, NotePC->PlayerCameraManager->GetCameraRotation().Yaw, 0.f);
				WeakNote->SetWorldLocationAndRotation(NotePC->PlayerCameraManager->GetCameraLocation() + Yaw.Vector() * 150.f, Yaw + FRotator(0.f, 180.f, 0.f));
			}
			if (FPlatformTime::Seconds() < AskAt)
			{
				return true;
			}
			if (FOutlawsAssets::AskForInstallRoot().IsEmpty())
			{
				if (WeakNote.IsValid()) WeakNote->SetText(FText::FromString(TEXT("Outlaws was not found.\nQuit, then start Outlaws VR again to choose its folder.")));
				return false;
			}
			if (WeakNote.IsValid()) WeakNote->DestroyComponent();
			if (bTestRun)
			{
				Self->Build();
				Self->StartSaveDelay = 1.5f;
				Self->SetActorTickEnabled(true);
			}
			else
			{
				Self->StartFrontEnd(true);
			}
			return false;
		}));
	}
	else if (bBuildOnBeginPlay && TakeMapLoadRebuild())
	{
		// (BUG-RESPAWNCRASH) the last world's rebuild, built in this fresh one
	}
	else if (bBuildOnBeginPlay && bTestRun)
	{
		Build();
		StartSaveDelay = 1.5f;             // the level-start autosave
	}
	else if (bBuildOnBeginPlay)
	{
		StartFrontEnd(true);               // straight to the menu (the opening movie plays from Main Game's story)
	}
	PlacePlayerTries = 0;
	bNeedPlace = bPlacePlayer && bHavePlayerStart && !TryPlacePlayer();
	UWorld* World = GetWorld();
	const bool bGame = World && World->IsGameWorld();
	if (Inf && bGame)
	{
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			EnableInput(PC);
			if (InputComponent && !bInputBound)
			{
				InputComponent->BindKey(EKeys::E, IE_Pressed, this, &AOutlawsLevelActor::OnUsePressed);
				InputComponent->BindKey(EKeys::Gamepad_FaceButton_Left, IE_Pressed, this, &AOutlawsLevelActor::OnUsePressed);
				// (M4) the remaster's quick taunts, F1..F10 but F9 (REQ-BUGSNAP: the bug snapshot, the player's own binding)
				for (const FKey& K : { EKeys::F1, EKeys::F2, EKeys::F3, EKeys::F4, EKeys::F5, EKeys::F6, EKeys::F7, EKeys::F8, EKeys::F10 })
				{
					InputComponent->BindKey(K, IE_Pressed, this, &AOutlawsLevelActor::OnTauntPressed);
				}
				// (BUG-SCOREBOARD) a match's frag table, Tab up and Tab away (the PC; the headset's is the pause menu's row)
				InputComponent->BindKey(EKeys::Tab, IE_Pressed, this, &AOutlawsLevelActor::OnScoreboardPressed);
				bInputBound = true;
			}
		}
	}
	// (the front end has no level: its flow runs from the tick; user 2026-09-27: it sat black until a shot's flash woke it)
	SetActorTickEnabled(bNeedPlace || bFrontEnd || bRebuildPending || (bGame && (Inf || Pickups.Num() > 0)));   // (a rebuild waiting: the menu's autosave)
}

void AOutlawsLevelActor::Tick(float DeltaSeconds)
{
	OutlawsXRRefresh::Maintain(FPlatformTime::Seconds());       // (BUG-REFRESH: the rate asked for, kept)
	OL_PERF_SCOPE("Level tick (total)");
	Super::Tick(DeltaSeconds);
	if (bMapLoadPending) return;               // (BUG-RESPAWNCRASH: L_Runtime is loading again; nothing of this world goes on)
#if WITH_OUTLAWS_TESTS
	static const bool bTeardownTest = OLFlags::HasValue(TEXT("OLTeardownTest"));
	if (bTeardownTest) StartTeardownTest();
	static const bool bCreditsTest = OLFlags::Has(TEXT("OLCreditsTest"));
	if (bCreditsTest) TickCreditsTest(DeltaSeconds);      // (REQ-CREDITS)
#endif
	if (bRebuildPending)
	{
		// the old level collected twice (the engine's own pass at the end of a frame; some of it only lets go once the first
		// pass has run: on the Quest a second pass freed 700 MB more, 300 MB of it textures), a few frames after each for the
		// GPU's deferred deletes, then the new one (user 2026-10-01: memory limit on a reload)
		++RebuildFrames;
		if (bFrontEnd)
		{
			// (the main menu's level on its way: the body hangs where it is, no fall through the cleared world)
			UWorld* RW = GetWorld();
			if (ACharacter* RC = RW && RW->GetFirstPlayerController() ? Cast<ACharacter>(RW->GetFirstPlayerController()->GetPawn()) : nullptr)
			{
				RC->GetCharacterMovement()->DisableMovement();
				RC->GetCharacterMovement()->Velocity = FVector::ZeroVector;
			}
		}
		if (RebuildStage == 0 && (GetOlGcCount() != RebuildGcMark || RebuildFrames > 30)) { RebuildStage = 1; RebuildFrames = 0; }
		else if (RebuildStage == 1 && RebuildFrames >= 3) { RebuildStage = 2; RebuildFrames = 0; RebuildGcMark = GetOlGcCount(); if (GEngine) GEngine->ForceGarbageCollection(true); }
		else if (RebuildStage == 2 && (GetOlGcCount() != RebuildGcMark || RebuildFrames > 30)) { RebuildStage = 3; RebuildFrames = 0; }
		else if (RebuildStage == 3)
		{
			// (-OLMemSoak) the empty world's memory sampled over a second, its least: the PC's driver lets go of the old
			// level's copies over a few frames, and one sample read that as growth
			static const bool bSoakSample = OLFlags::Has(TEXT("OLMemSoak"));
			if (bSoakSample) SoakEmptyMB = FMath::Min(SoakEmptyMB, ResidentMB());
			if (RebuildFrames >= (bSoakSample ? 60 : 5)) FinishRestart();
		}
		return;
	}
#if WITH_OUTLAWS_TESTS
	static const bool bStutterProbe = OLFlags::Param(TEXT("OLStutterProbe"));
	if (bStutterProbe && !bFrontEnd) TickStutterProbe(DeltaSeconds);
	if (bFrontEnd && !bNeedPlace) OlExitAt(TEXT("title"));    // (BUG-EXITCRASH: the main menu's first frame up)
	static const bool bMusicTest = OLFlags::Param(TEXT("OLMusicTest"));
	if (bMusicTest && !bFrontEnd) TickMusicTest(DeltaSeconds);
	static const bool bRandoTest = OLFlags::Param(TEXT("OLRandoTest"));
	if (bRandoTest) TickRandoTest(DeltaSeconds);
	static const bool bRandoBossTest = OLFlags::Param(TEXT("OLRandoBossTest"));
	if (bRandoBossTest) TickRandoBossTest(DeltaSeconds);
	static const bool bRandoCritterTest = OLFlags::Param(TEXT("OLRandoCritterTest"));
	if (bRandoCritterTest) TickRandoCritterTest(DeltaSeconds);
	static const bool bMemSoak = OLFlags::Has(TEXT("OLMemSoak"));
	if (bMemSoak) TickMemSoak(DeltaSeconds);
	if (OLFlags::Param(TEXT("OLDoorRuleTest")) && Inf && !bNeedPlace && !bRebuildPending && !bFrontEnd)
	{
		// (BUG-BOSSDOOR) -OLLevel=<it> -OLNoFrontEnd -OLDoorRuleTest: the level's door sectors (its moving sectors and automatic
		// doors) and how many of them a bad guy may work, by the original's rule; then quit
		static float DoorT = 0.f;
		DoorT += DeltaSeconds;
		if (DoorT > 3.f && DoorT - DeltaSeconds <= 3.f)
		{
			TSet<int32> Doors;
			for (const TPair<int32, TArray<int32>>& Kv : Inf->ElevatorsBySector) Doors.Add(Kv.Key);
			for (const FOutlawsSector& S : Inf->Live.Sectors) if (S.Flags & OutlawsFlags::SectorDoor) Doors.Add(S.Index);
			int32 Theirs = 0;
			TArray<FString> Names;
			for (const int32 D : Doors)
			{
				if (!EnemyMayWorkDoor(D)) continue;
				++Theirs;
				if (Names.Num() < 12 && Inf->Live.Sectors.IsValidIndex(D)) Names.Add(Inf->Live.Sectors[D].Name.IsEmpty() ? FString::FromInt(D) : Inf->Live.Sectors[D].Name);
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT door rule %s: %d moving or door sectors, %d a bad guy may work (%s)"), *LevelName, Doors.Num(), Theirs, *FString::Join(Names, TEXT(", ")));
			FGenericPlatformMisc::RequestExit(false);
		}
	}
#endif
	if (bNeedPlace && (TryPlacePlayer() || (++PlacePlayerTries > 240 && !UOutlawsCoop::IsActive(this))))   // (co-op: the controller arrives over the network)
	{
		bNeedPlace = false;
	}
	UWorld* World = GetWorld();
	const bool bGame = World && World->IsGameWorld();
	if (bGame)
	{
		StopSpentSounds();
		TickSoundRuns(DeltaSeconds);                  // (REQ-RELOADSLICES)
		UpdateMusic();
		TickLights(DeltaSeconds);
		TickFlash3D(DeltaSeconds);                    // (REQ-MUZZLEFLASH3D)
		{
			OL_PERF_SCOPE("Level: saves");            // (CPU1 step 0a)
			TickSaves(DeltaSeconds);
		}
		TickTutorial(DeltaSeconds);                   // (REQ-TUTORIAL, OutlawsTutorial.cpp)
		TickPerfTest(DeltaSeconds);                   // (REQ-PERFTESTMODE, OutlawsPerfTest.cpp)
#if WITH_OUTLAWS_TESTS
		static const bool bTutorialTest = OLFlags::HasValue(TEXT("OLTutorialTest"));
		if (bTutorialTest) TickTutorialTest(DeltaSeconds);
		static const bool bHmCareerTest = OLFlags::Has(TEXT("OLHmCareerTest"));
		if (bHmCareerTest) TickHmCareerTest(DeltaSeconds);       // (REQ-HMCAREER, OutlawsHom.cpp)
#endif
		if (bFrontEnd)
		{
			// the front end: the player hangs in the dark; once placed, the opening movie (or the menu)
			APawn* Pawn = World->GetFirstPlayerController() ? World->GetFirstPlayerController()->GetPawn() : nullptr;
			if (AOutlawsPlayer* P = Cast<AOutlawsPlayer>(Pawn))
			{
				P->GetCharacterMovement()->DisableMovement();
				P->GetCharacterMovement()->Velocity = FVector::ZeroVector;
				if (bFrontPending && !bNeedPlace)
				{
					bFrontPending = false;
					P->HoldMenuPistol();           // a revolver to point and shoot at the menu with
					if (bFrontSkipIntro || !PlayMovie(TEXT("op_cr"), true)) ShowFrontMenu();
					else bMovieToMenu = true;
				}
				else if (!bFrontPending && !bNeedPlace && !bMovieToMenu && !IsMoviePlaying() && !P->IsMenuOpen() && FPlatformTime::Seconds() >= FrontRetryAt)
				{
					FrontRetryAt = FPlatformTime::Seconds() + 1.0;
					// (the title flow with no menu up and nothing on its way: a load that failed, a pause page shut, a rebuild
					// asked for from somewhere else: the main menu again, whatever led here)
					bDead = false;
					Health = FMath::Max(Health, 1.f);
					UE_LOG(LogOutlaws, Log, TEXT("OLRT front end without its menu: opened again"));
					ShowFrontMenu();
				}
			}
		}
		if (BadgeLeft > 0.f && !bDead)
		{
			BadgeLeft -= DeltaSeconds;
			if (BadgeLeft <= 0.f)
			{
				BadgeLeft = 0.f;       // (REQ-NOBADGEFADEMSG, user 2026-10-12: no message when the power ends)
				UE_LOG(LogOutlaws, Log, TEXT("OLRT badge power over"));
			}
		}
	}
	// twirling pickups (the sheriff's star)
	for (int32 i = SpinningModels.Num() - 1; i >= 0; --i)
	{
		if (UProceduralMeshComponent* M = SpinningModels[i].Key.Get()) M->AddWorldRotation(FRotator(0.0, SpinningModels[i].Value * DeltaSeconds, 0.0));
		else SpinningModels.RemoveAt(i);
	}
	{
		OL_PERF_SCOPE("Level: flags + lamps");   // (CPU1 step 0a)
		UpdateWavingModels(DeltaSeconds);         // the flags' cloth (their sprite's idle loop, as keyframes)
		UpdateLamps(DeltaSeconds);                // the lamps' fire, the hanging ones' swing
	}
	if (bGame && SkyMesh)
	{
		// the clouds drift left: the dome turns slowly about the vertical
		SkyMesh->AddWorldRotation(FRotator(0.0, -SkySpinSpeed * DeltaSeconds, 0.0));
		if (SkySurfaceMid) SkySurfaceMid->SetScalarParameterValue(TEXT("Spin"), FMath::DegreesToRadians(float(SkyMesh->GetComponentRotation().Yaw)));
		// the cloud layer turns the other way, slowly wandering in speed: the wind seems to veer
		if (SkySurfaceMid)
		{
			const float Veer = 0.6f + 0.4f * FMath::Sin(float(GetWorld()->GetTimeSeconds()) * 0.05f);
			CloudSpin = FMath::Fmod(CloudSpin + FMath::DegreesToRadians(CloudSpinSpeed * Veer * DeltaSeconds), 2.f * PI);
			SkySurfaceMid->SetScalarParameterValue(TEXT("CloudSpin"), CloudSpin);
			// and drifts: the offset kept here, wrapped to 0..1 (never large, so it stays smooth at any precision)
			CloudDrift += CloudWindRate * double(DeltaSeconds);
			CloudDrift.X = FMath::Frac(CloudDrift.X);
			CloudDrift.Y = FMath::Frac(CloudDrift.Y);
			SkySurfaceMid->SetVectorParameterValue(TEXT("CloudWind"), FLinearColor(float(CloudDrift.X), float(CloudDrift.Y), 0.f, 0.f));
		}
	}
	if (bDead)
	{
		// the world waits while the player lies dead: a button restarts the level (AOutlawsPlayer)
		DeathTimer += DeltaSeconds;
		if (!UOutlawsCoop::IsActive(this)) return;      // (co-op: it goes on for the other player)
	}
	if (bGame && (Inf || bLampOn || bFrontEnd || SkyMesh || Movie || Pickups.Num() > 0 || CollectBoxActor.IsValid() || Props.Num() > 0 || Panes.Num() > 0 || Sparks.Num() > 0 || GlassBursts.Num() > 0 || SoundGens.Num() > 0 || ItemGens.Num() > 0 || Boss.IsValid() || LevelEndTimer >= 0.f))
	{
		if (Inf) TickInf(DeltaSeconds);
		// (the level behind the main menu is a still picture: nothing picked up, generated, carried or won there)
		// (co-op: a body lying on a pickup takes nothing; single player as before, where a death in this frame's INF still
		// reached here)
		if (!bFrontEnd && !(bDead && UOutlawsCoop::IsActive(this))) UpdatePickups();
		if (!bFrontEnd) UpdateCollectBox(DeltaSeconds);       // (REQ-COLLECTBOX)
		UpdateShootables(DeltaSeconds);
		TickAtx(DeltaSeconds);
		if (!bFrontEnd) UpdateSoundGens(DeltaSeconds);
		if (!bFrontEnd) UpdateItemGens(DeltaSeconds);
		UpdateBubbles(DeltaSeconds);
		UpdateBloodPools(DeltaSeconds);
		if (!bFrontEnd) CarryBySectors(DeltaSeconds);
		if (!bFrontEnd && Enemies.Num() > 0) TickEnemyThrottle(DeltaSeconds);   // (CPU2)
		if (!bFrontEnd && Enemies.Num() > 0) TickTaunts(DeltaSeconds);          // (BUG-TAUNTRATE)
		if (!bFrontEnd && Enemies.Num() > 0)
		{
			// the far-freeze book (user 2026-10-03): how many held still, for how long, and the ticks that spared
			FarFrozenSum += FarFrozen * double(DeltaSeconds);
			FarFrozenTime += DeltaSeconds;
			FarFrozenTicks += FarFrozen * (1.0 - 2.0 * double(DeltaSeconds));
		}
		if (!bFrontEnd || !bBackdrop) UpdateBoss(DeltaSeconds);
		{
			OL_PERF_SCOPE("Level: room sound");   // (CPU1 step 0a)
			UpdateRoomSound(DeltaSeconds);        // (R13: the reverb follows the space the listener stands in)
		}
#if WITH_OUTLAWS_TESTS
		static const bool bReverbTest = OLFlags::Param(TEXT("OLReverbTest"));
		if (bReverbTest) TickReverbTest(DeltaSeconds);
#endif
	}
	else if (!bNeedPlace && !bRebuildPending)          // (a restart from inside this tick: the empty level is not idle)
	{
		SetActorTickEnabled(false);
	}
}

bool AOutlawsLevelActor::TryPlacePlayer()
{
	UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld())
	{
		return true;
	}
	APlayerController* PC = World->GetFirstPlayerController();
	APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	const FRotator Rot(0.f, PlayerStartYaw, 0.f);
	if (!Pawn && PC && bUseOutlawsPlayer && UOutlawsCoop::IsActive(this))
	{
		// co-op: the session's controller comes without a pawn; this machine's own player is made here, as below
		Pawn = UOutlawsCoop::SpawnLocalPlayer(PC, PlayerStartLocation, Rot);
	}
	if (!Pawn)
	{
		return false;
	}
	// our player (VR or desktop) instead of the game mode's stand-in (AOutlawsStandInPawn)
	if (bUseOutlawsPlayer && !Pawn->IsA<AOutlawsPlayer>())
	{
		FActorSpawnParameters P;
		P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
		if (AOutlawsPlayer* NewPawn = World->SpawnActor<AOutlawsPlayer>(AOutlawsPlayer::StaticClass(), PlayerStartLocation, Rot, P))
		{
			APawn* Old = Pawn;
			PC->UnPossess();
			PC->Possess(NewPawn);
			Old->Destroy();
			Pawn = NewPawn;
			UE_LOG(LogOutlaws, Log, TEXT("OLRT Outlaws player spawned"));
		}
	}
	Pawn->TeleportTo(PlayerStartLocation, Rot, false, true);
	if (AOutlawsPlayer* P = Cast<AOutlawsPlayer>(Pawn))
	{
		P->GrantFallGrace(GetWorld()->GetTimeSeconds() + 3.0);     // placed on a level: the settle is not a fall
	}
	PC->SetControlRotation(Rot);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT player placed at %s yaw %.1f"), *PlayerStartLocation.ToString(), PlayerStartYaw);
	// the first level (the hideout; the office for the historical missions) starts with the revolver drawn in the
	// leading hand (user 2026-09-28); a save being put back sets the hands its own way afterwards
	if (AOutlawsPlayer* P = Cast<AOutlawsPlayer>(Pawn); P && !bFrontEnd
		&& (LevelName.Equals(TEXT("hideout"), ESearchCase::IgnoreCase) || LevelName.Equals(TEXT("office"), ESearchCase::IgnoreCase)))
	{
		P->DrawStartPistol();
	}
	// every level start: the view and the held gear set straight again (user 2026-09-28)
	if (AOutlawsPlayer* P = Cast<AOutlawsPlayer>(Pawn); P && !bFrontEnd)
	{
		P->Recenter(true);
	}
#if WITH_OUTLAWS_TESTS
	if (OLFlags::Param(TEXT("OLItemTest")))
	{
		SpawnItemTest();
	}
#endif
	return true;
}

void AOutlawsLevelActor::MovePlayerStart(const FVector& At)
{
	PlayerStartLocation = At;
	if (bNeedPlace) return;                   // (placed there when the player arrives)
	UWorld* World = GetWorld();
	APawn* Pawn = World && World->GetFirstPlayerController() ? World->GetFirstPlayerController()->GetPawn() : nullptr;
	if (Pawn) Pawn->TeleportTo(At, FRotator(0.f, PlayerStartYaw, 0.f), false, true);
}

FString AOutlawsLevelActor::RunFloorCheck()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return TEXT("no world");
	}
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsFloorCheck), true);
	int32 Tested = 0, Misses = 0;
	TArray<FString> Names;
	for (const TWeakObjectPtr<AActor>& W : FloorStandingSprites)
	{
		AActor* S = W.Get();
		if (!S) continue;
		++Tested;
		const FVector Lc = S->GetActorLocation();
		FHitResult Hit;
		if (!World->LineTraceSingleByChannel(Hit, Lc + FVector(0, 0, 60.0), Lc - FVector(0, 0, 400.0), ECC_Visibility, Q))
		{
			++Misses;
			if (Names.Num() < 20)
			{
				const AOutlawsSpriteActor* SA = Cast<AOutlawsSpriteActor>(S);
				Names.Add(SA ? SA->OutlawsName : S->GetName());
			}
		}
	}
	FHitResult Hit;
	const bool bStart = bHavePlayerStart && World->LineTraceSingleByChannel(Hit, PlayerStartLocation, PlayerStartLocation - FVector(0, 0, 400.0), ECC_Visibility, Q);
	return FString::Printf(TEXT("floor check: %d tested, %d misses [%s]; player start floor %s (normal z %.2f)"),
		Tested, Misses, *FString::Join(Names, TEXT(", ")), bStart ? TEXT("HIT") : TEXT("MISS"), bStart ? Hit.ImpactNormal.Z : 0.0);
}

bool AOutlawsLevelActor::IsGunSound(const FString& Wav)
{
	FString Base;
	if (!Wav.Split(TEXT(","), &Base, nullptr)) Base = Wav;
	Base = FPaths::GetBaseFilename(Base.TrimStartAndEnd()).ToLower();
	static const TSet<FString> Guns = { TEXT("enmyshot"), TEXT("rifle"), TEXT("single"), TEXT("double"), TEXT("badgun"), TEXT("pistol"),
		TEXT("pistol3"), TEXT("pistol3r"), TEXT("sawed"), TEXT("gatlshot"), TEXT("shotgun") };
	return Guns.Contains(Base);
}

void AOutlawsLevelActor::MuzzleFlash(const FVector& At, float Strength, float Radius)
{
	// the gatling's run of shots keeps one light alive rather than filling every slot
	int32 Slot = INDEX_NONE;
	for (int32 i = 0; i < UE_ARRAY_COUNT(Flashes); ++i)
	{
		if (Flashes[i].Age < 0.25f && FVector::DistSquared(Flashes[i].Pos, At) < 200.0 * 200.0 && FMath::IsNearlyEqual(Flashes[i].Radius, Radius)) Slot = i;
	}
	if (Slot == INDEX_NONE)
	{
		Slot = NextFlash;
		NextFlash = (NextFlash + 1) % UE_ARRAY_COUNT(Flashes);
	}
	Flashes[Slot].Strength = FMath::Max(Strength, Flashes[Slot].Age < 0.03f ? Flashes[Slot].Strength : 0.f);
	Flashes[Slot].Pos = At;
	Flashes[Slot].Radius = Radius;
	Flashes[Slot].Age = 0.f;
	SetActorTickEnabled(true);
}

bool AOutlawsLevelActor::ToggleLamp()
{
	if (bLampOn)
	{
		bLampOn = false;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT lantern out"));
		return true;
	}
	if (bDead) return false;
	if (GetCount(TEXT("ioil")) <= 0)
	{
		Notify(TEXT("The lantern has no oil"), 2.f);
		return false;
	}
	bLampOn = true;
	SetActorTickEnabled(true);
	PlayWav(TEXT("light"), FVector::ZeroVector, false, true);      // the character's SND_LIGHTLAMP
	UE_LOG(LogOutlaws, Log, TEXT("OLRT lantern lit, oil %d"), GetCount(TEXT("ioil")));
	return true;
}

static void OlCensus();

void AOutlawsLevelActor::LanternLight(const FVector& At, float Oil, float T, FLinearColor& OutPos, FLinearColor& OutCol, float* OutFlicker)
{
	Oil = FMath::Clamp(Oil, 0.f, 1.f);
	const float Glow = FMath::Lerp(0.3f, 1.f, FMath::Sqrt(Oil));
	const float Flicker = 1.f + 0.06f * FMath::Sin(T * 7.3f) + 0.04f * FMath::Sin(T * 17.9f + 1.7f) + 0.08f * FMath::PerlinNoise1D(T * 5.f);
	// (REQ-LANTERNBOOST, user 2026-10-11: twice the reach, 22 m (was 11), and 1.25 times as bright, 0.75 (was 0.6); every
	// lantern, the player's own and the others' in co-op, is lit through here)
	OutPos = FLinearColor(At.X, At.Y, At.Z, LanternReach * FMath::Lerp(0.65f, 1.f, Oil) * (1.f + 0.3f * (Flicker - 1.f)));
	OutCol = FLinearColor(1.f, 0.86f, 0.68f) * (LanternBright * Glow * Flicker);
	if (OutFlicker) *OutFlicker = Flicker;
}

bool AOutlawsLevelActor::LanternShining(FVector& OutAt, float& OutOil) const
{
	if (!bLampOn || bDead || IsMoviePlaying()) return false;
	const UWorld* W = GetWorld();
	const APlayerController* PC = W ? W->GetFirstPlayerController() : nullptr;
	if (!PC || !PC->PlayerCameraManager) return false;
	// (held in a hand, its flame; else at the chest)
	OutAt = FPlatformTime::Seconds() - LampAtTime < 0.25 ? LampAt : PC->PlayerCameraManager->GetCameraLocation() - FVector(0.0, 0.0, 25.0);
	OutOil = FMath::Clamp(GetCount(TEXT("ioil")) / 150.f, 0.f, 1.f);
	return true;
}

static TAutoConsoleVariable<int32> CVarRemoteLanterns(TEXT("ol.RemoteLanterns"), -1,
	TEXT("BUG-LANTERNNET: how many other players' lit lanterns light the room here (the nearest; 0..2); -1 the headset's (2, 1 under the Quest 2 profile)"));

int32 AOutlawsLevelActor::RemoteLanternCap()
{
	const int32 V = CVarRemoteLanterns.GetValueOnGameThread();
	if (V >= 0) return FMath::Min(V, 2);
	return OutlawsDeviceProfile::IsQuest2() ? 1 : 2;      // (-OLDeviceProfile=Quest2 turns the profile on, Q2PROFILE)
}

void AOutlawsLevelActor::TickLights(float Dt)
{
	OL_PERF_SCOPE("Level: lights");
	UWorld* W = GetWorld();
	if (!LightsMpc) LightsMpc = LoadObject<UMaterialParameterCollection>(nullptr, TEXT("/Game/Outlaws/Runtime/MPC_OLR_Lights.MPC_OLR_Lights"));
	UMaterialParameterCollectionInstance* Mpc = LightsMpc && W ? W->GetParameterCollectionInstance(LightsMpc) : nullptr;
	if (!Mpc) return;
	// the lantern: at the player's chest, warm white, flickering like a flame; a unit of oil every 3 s
	// (INFERRED: 30 lasts a minute and a half, a full 150 seven and a half), dimmer as the oil runs low
	FLinearColor LampPos(0.f, 0.f, 0.f, 1.f), LampCol(0.f, 0.f, 0.f, 0.f);
	if (bLampOn && bDead) bLampOn = false;
	// a movie: the lantern neither shines on its screen nor burns (user 2026-09-28: cutscenes looked lit up)
	const bool bMovieOn = Movie && Movie->IsPlaying();
	if (bLampOn && !bMovieOn)
	{
		OilBurn += Dt / 3.f;
		while (OilBurn >= 1.f)
		{
			OilBurn -= 1.f;
			TakeCount(TEXT("ioil"), 1);
		}
		if (GetCount(TEXT("ioil")) <= 0)
		{
			bLampOn = false;
			Notify(TEXT("The lantern is out of oil"), 2.f);
		}
	}
	FVector At;
	float Oil = 0.f;
	if (LanternShining(At, Oil))
	{
		LampTime += Dt;
		LanternLight(At, Oil, LampTime, LampPos, LampCol);
	}
	Mpc->SetVectorParameterValue(TEXT("L0"), LampPos);
	Mpc->SetVectorParameterValue(TEXT("C0"), LampCol);
	// the flashes: full for a frame or two, gone in about a fifth of a second
	for (int32 i = 0; i < UE_ARRAY_COUNT(Flashes); ++i)
	{
		FMuzzleFlash& F = Flashes[i];
		F.Age += Dt;
		const float K = F.Age < 0.03f ? 1.f : FMath::Exp(-(F.Age - 0.03f) * 22.f);
		const bool bOn = F.Age < 0.25f && !bMovieOn;
		Mpc->SetVectorParameterValue(*FString::Printf(TEXT("L%d"), i + 1), FLinearColor(F.Pos.X, F.Pos.Y, F.Pos.Z, F.Radius));
		Mpc->SetVectorParameterValue(*FString::Printf(TEXT("C%d"), i + 1), bOn ? FLinearColor(1.f, 0.8f, 0.55f) * (1.3f * F.Strength * K) : FLinearColor(0.f, 0.f, 0.f, 0.f));
	}
	TickLampLights(Dt, bMovieOn, Mpc);
	if (OLFlags::Param(TEXT("OLCensus")))
	{
		static float CensusT = 0.f;
		const float Was = CensusT;
		CensusT += Dt;
		if (Was < 20.f && CensusT >= 20.f) OlCensus();
		if (Was < 50.f && CensusT >= 50.f) { OlCensus(); FGenericPlatformMisc::RequestExit(false); }
	}
	static int32 ReloadsLeft = -2;
	static float ReloadWait = 15.f;
	if (ReloadsLeft == -2)
	{
		ReloadsLeft = -1;
		OLFlags::Value(TEXT("OLReloadTest"), ReloadsLeft);
	}
	if (ReloadsLeft >= 0)
	{
		ReloadWait -= Dt;
		if (ReloadWait <= 0.f)
		{
			ReloadWait = 15.f;
			const FPlatformMemoryStats M = FPlatformMemory::GetStats();
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT reload test: %d to go, %.0f MB used, peak %.0f MB"), ReloadsLeft, M.UsedPhysical / 1048576.0, M.PeakUsedPhysical / 1048576.0);
			if (ReloadsLeft-- == 0) FGenericPlatformMisc::RequestExit(false);
			else RestartLevel();
		}
	}
}

static FAutoConsoleCommandWithWorld CmdOlFrontEnd(TEXT("ol.FrontEnd"), TEXT("Back to the main menu (a new random level behind it), nothing saved"),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* W)
	{
		for (TActorIterator<AOutlawsLevelActor> It(W); It; ++It)
		{
			if (AOutlawsPlayer* P = W->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(W->GetFirstPlayerController()->GetPawn()) : nullptr) P->CloseGameMenu();
			It->ReturnToFrontEnd();
			break;
		}
	}));

static FAutoConsoleCommandWithWorld CmdOlRestart(TEXT("ol.Restart"), TEXT("Restart the level (memory tests: each restart should end where the last one did)"),
	FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* W)
	{
		for (TActorIterator<AOutlawsLevelActor> It(W); It; ++It) { It->RestartLevel(); break; }
	}));

// every live object counted by class, and which grew since the last call (user 2026-10-01: the Quest's memory climbs 9 MB a
// second in play and drops 500 MB at each collection: something is made every frame)
static void OlCensus()
{
	static TMap<FName, int32> Prev;
	TMap<FName, int32> Now;
	int32 Total = 0;
	for (TObjectIterator<UObject> It; It; ++It)
	{
		++Now.FindOrAdd(It->GetClass()->GetFName());
		++Total;
	}
	TArray<TPair<FName, int32>> Grew;
	for (const TPair<FName, int32>& Kv : Now)
	{
		const int32 D = Kv.Value - Prev.FindRef(Kv.Key);
		if (D != 0) Grew.Add({ Kv.Key, D });
	}
	Grew.Sort([](const TPair<FName, int32>& A, const TPair<FName, int32>& B) { return A.Value > B.Value; });
	const FPlatformMemoryStats M = FPlatformMemory::GetStats();
	UE_LOG(LogOutlaws, Display, TEXT("OLCENSUS %d objects, %.0f MB used, %.1f MB of sound played%s"), Total, M.UsedPhysical / 1048576.0, UOutlawsPcmWave::BytesPlayed.load() / 1048576.0, Prev.Num() ? TEXT(", grew since the last:") : TEXT(" (first count)"));
	for (int32 i = 0; Prev.Num() && i < FMath::Min(20, Grew.Num()) && Grew[i].Value > 0; ++i)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLCENSUS   +%d %s (now %d)"), Grew[i].Value, *Grew[i].Key.ToString(), Now.FindRef(Grew[i].Key));
	}
	Prev = MoveTemp(Now);
}
static FAutoConsoleCommand CmdOlCensus(TEXT("ol.Census"), TEXT("Count live objects by class; the second call lists what grew"), FConsoleCommandDelegate::CreateStatic(&OlCensus));

static TAutoConsoleVariable<int32> CVarLampLights(TEXT("ol.LampLights"), 4,
	TEXT("How many lit lamps light the room around them (0..4; slots 4.. of MPC_OLR_Lights); a cap under the Graphics page's Lamp Lights row"));

bool AOutlawsLevelActor::NearestLampFire(const FVector& From, FVector& Out) const
{
	double Best = TNumericLimits<double>::Max();
	for (const FLampFlame& F : LampFlames)
	{
		const UStaticMeshComponent* Q = F.Quad.Get();
		if (!Q || !Q->IsVisible()) continue;
		const double D = FVector::DistSquared(Q->GetComponentLocation(), From);
		if (D < Best) { Best = D; Out = Q->GetComponentLocation(); }
	}
	return Best < TNumericLimits<double>::Max();
}

void AOutlawsLevelActor::TickLampLights(float Dt, bool bMovieOn, UMaterialParameterCollectionInstance* Mpc)
{
	if (bLampSlots < 0)
	{
		bLampSlots = LightsMpc && LightsMpc->GetParameterId(TEXT("L4")).IsValid() ? 1 : 0;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT lamp light slots: %s"), bLampSlots ? TEXT("4") : TEXT("none (run ue_olue_lights_slots.py)"));
	}
	if (!bLampSlots) return;
	UWorld* W = GetWorld();
	APlayerController* PC = W ? W->GetFirstPlayerController() : nullptr;
	const FVector Eye = PC && PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : FVector::ZeroVector;
	// the Graphics page's Lamp Lights (1 by default under the Quest 2 profile), the cvar as a cap for tests
	const int32 LampRow = FMath::Min(FMath::Clamp(CVarLampLights.GetValueOnGameThread(), 0, 4), OutlawsDeviceProfile::LampLights());
	// BUG-LANTERNNET: the other players' lit lanterns within 41 m, the nearest first, up to the cap: the first in L7, the
	// second in L6; the lamps keep the slots below them, as many as the row less the lanterns (the slots lit stay in the row)
	// (REQ-LANTERNBOOST: was 30 m for the 11 m reach; the 22 m reach keeps the same 19 m between the cut and the lit ground)
	const double RemoteFar = 1900.0 + LanternReach;
	struct FRemoteLit { FVector At; float Oil; float Phase; int32 Who; double D; };
	TArray<FRemoteLit> Remote;
	if (!bMovieOn && W)
	{
		for (TActorIterator<AOutlawsRemotePlayer> It(W); It; ++It)
		{
			FVector RAt;
			float ROil = 0.f;
			if (!It->LanternShown(RAt, ROil)) continue;
			const double D = FVector::Dist(RAt, Eye);
			if (D < RemoteFar) Remote.Add({ RAt, ROil, 37.1f * float(It->PlayerIndex + 1), It->PlayerIndex, D });
		}
		Remote.Sort([](const FRemoteLit& A, const FRemoteLit& B) { return A.D < B.D; });
	}
	const int32 RemoteN = FMath::Min(Remote.Num(), RemoteLanternCap());
	Remote.SetNum(RemoteN);
	RemoteLanternsLit = RemoteN;
	const int32 Want = FMath::Max(0, LampRow - RemoteN);
	// the lit lamps: each lamp model's lit glasses (their fire cards), the light at their middle
	struct FLit { UProceduralMeshComponent* Mesh; FVector Sum; int32 N; double D; };
	TArray<FLit> Lit;
	if (!bMovieOn)
	{
		for (const FLampFlame& F : LampFlames)
		{
			UStaticMeshComponent* Q = F.Quad.Get();
			UProceduralMeshComponent* M = F.Mesh.Get();
			if (!Q || !M || !Q->IsVisible()) continue;
			FLit* E = Lit.FindByPredicate([M](const FLit& X) { return X.Mesh == M; });
			if (!E) E = &Lit.Add_GetRef({ M, FVector::ZeroVector, 0, 0.0 });
			E->Sum += Q->GetComponentLocation();
			++E->N;
		}
	}
	// (user 2026-10-03, BUG-LAMPS: "can't tell the difference", "too subtle or not showing at all", up close too) 20 m out
	// of reach, fading from 14 m; the light 1.5 (was 0.42) over 9 m (was 6): the shader's fall-off is (1 - d/r)^2, so at
	// 3 m about 0.57 of it lands (was 0.07), at 1 m about 1.0, well over the held lantern's 0.32 at 3 m
	const double Far = 2000.0, FadeFrom = 1400.0;
	for (FLit& E : Lit) E.D = FVector::Dist(E.Sum / E.N, Eye);
	Lit.RemoveAll([Far](const FLit& E) { return E.D >= Far; });
	// the nearest first, a lamp already shining a fifth nearer than it is (it holds its slot against a lamp just nearer)
	auto Held = [this](const UProceduralMeshComponent* M) { for (const FLampLight& S : LampLights) if (S.Mesh.Get() == M && S.Fade > 0.f) return true; return false; };
	Lit.Sort([&Held](const FLit& A, const FLit& B) { return A.D * (Held(A.Mesh) ? 0.8 : 1.0) < B.D * (Held(B.Mesh) ? 0.8 : 1.0); });
	if (Lit.Num() > Want) Lit.SetNum(Want);
	// a slot whose lamp went out (shot, or a movie) goes dark now; one no longer wanted fades out; a free one takes a lamp
	// not yet shining
	const float Rate = Dt / 0.4f;
	for (int32 s = 0; s < 4; ++s)
	{
		FLampLight& S = LampLights[s];
		const FLit* E = S.Mesh.IsValid() ? Lit.FindByPredicate([&S](const FLit& X) { return X.Mesh == S.Mesh.Get(); }) : nullptr;
		const bool bStillLit = S.Mesh.IsValid() && LampFlames.ContainsByPredicate([&S](const FLampFlame& F) { return F.Mesh == S.Mesh && F.Quad.IsValid() && F.Quad->IsVisible(); });
		if (!bStillLit || bMovieOn) S.Fade = 0.f;
		else S.Fade = FMath::Clamp(S.Fade + (E && s < Want ? Rate : -Rate), 0.f, 1.f);
		if (S.Fade <= 0.f) S.Mesh.Reset();
	}
	for (const FLit& E : Lit)
	{
		if (LampLights[0].Mesh == E.Mesh || LampLights[1].Mesh == E.Mesh || LampLights[2].Mesh == E.Mesh || LampLights[3].Mesh == E.Mesh) continue;
		for (int32 s = 0; s < Want; ++s)
		{
			if (LampLights[s].Mesh.IsValid()) continue;
			LampLights[s].Mesh = E.Mesh;
			LampLights[s].Fade = FMath::Min(Rate, 1.f);
			LampLights[s].Phase = FMath::FRandRange(0.f, 100.f);
			UE_LOG(LogOutlaws, Log, TEXT("OLRT lamp light %d: %s at %.0f cm"), s, *GetNameSafe(E.Mesh->GetOwner()), E.D);
			break;
		}
	}
	// the light: the fire's warm tone, under the lantern's, 6 m, a gentle flicker, times its slot's fade and the distance's
	const float T = float(W ? W->GetTimeSeconds() : 0.0);
	RemoteLanternsShown.Reset();
	LampSlotsLit = 0;
	for (int32 s = 0; s < 4; ++s)
	{
		FLampLight& S = LampLights[s];
		// a remote lantern's slot (once the lamp that had it has faded out): the lantern's own light, as its holder sees it
		if (s >= 4 - RemoteN && S.Fade <= 0.f)
		{
			const FRemoteLit& R = Remote[3 - s];
			FRemoteLanternShown& Sh = RemoteLanternsShown.AddDefaulted_GetRef();
			Sh.Slot = 4 + s;
			Sh.Who = R.Who;
			Sh.At = R.At;
			Sh.Oil = R.Oil;
			LanternLight(R.At, R.Oil, T + R.Phase, Sh.Pos, Sh.Col, &Sh.Flicker);
			Mpc->SetVectorParameterValue(*FString::Printf(TEXT("L%d"), 4 + s), Sh.Pos);
			Mpc->SetVectorParameterValue(*FString::Printf(TEXT("C%d"), 4 + s), Sh.Col);
			continue;
		}
		UProceduralMeshComponent* M = S.Mesh.Get();
		FVector At = FVector::ZeroVector;
		int32 N = 0;
		if (M)
		{
			for (const FLampFlame& F : LampFlames)
			{
				if (F.Mesh.Get() == M && F.Quad.IsValid() && F.Quad->IsVisible()) { At += F.Quad->GetComponentLocation(); ++N; }
			}
		}
		FLinearColor Col(0.f, 0.f, 0.f, 0.f);
		if (N > 0 && S.Fade > 0.f)
		{
			At /= N;
			const float D = float(FVector::Dist(At, Eye));
			const float U = FMath::Clamp((D - float(FadeFrom)) / float(Far - FadeFrom), 0.f, 1.f);
			const float Near = 1.f - U * U * U * (U * (U * 6.f - 15.f) + 10.f);     // (smootherstep: no kink at either end)
			const float Ph = T + S.Phase;
			const float Flicker = 1.f + 0.05f * FMath::Sin(Ph * 6.1f) + 0.03f * FMath::Sin(Ph * 15.7f + 1.3f) + 0.06f * FMath::PerlinNoise1D(Ph * 4.f);
			const float Many = FMath::Min(1.f, 0.55f + 0.15f * N);
			const float Ease = S.Fade * S.Fade * (3.f - 2.f * S.Fade);
			Col = FLinearColor(1.f, 0.72f, 0.42f) * (1.5f * Many * Flicker * Near * Ease);
			++LampSlotsLit;
		}
		Mpc->SetVectorParameterValue(*FString::Printf(TEXT("L%d"), 4 + s), FLinearColor(At.X, At.Y, At.Z, 900.f));
		Mpc->SetVectorParameterValue(*FString::Printf(TEXT("C%d"), 4 + s), Col);
	}
}

int32 AOutlawsLevelActor::DarkestSector() const
{
	if (!Inf) return -1;
	const FOutlawsLevel& Lv = Inf->Live;
	int32 Best = -1;
	double BestArea = 0.0;
	for (const FOutlawsSector& S : Lv.Sectors)
	{
		if (S.Verts.Num() < 3 || (S.Flags & 0x80) || S.Ceil.Y - S.Floor.Y < 8.0) continue;
		double A = 0.0;
		for (int32 i = 0; i < S.Verts.Num(); ++i)
		{
			const FVector2D& P = S.Verts[i];
			const FVector2D& Q = S.Verts[(i + 1) % S.Verts.Num()];
			A += P.X * Q.Y - Q.X * P.Y;
		}
		A = FMath::Abs(A) * 0.5;
		if (A < 150.0) continue;
		if (Best < 0 || S.Ambient < Lv.Sectors[Best].Ambient || (S.Ambient == Lv.Sectors[Best].Ambient && A > BestArea))
		{
			Best = S.Index;
			BestArea = A;
		}
	}
	if (Best >= 0) UE_LOG(LogOutlaws, Log, TEXT("OLRT darkest sector %d %s ambient %d area %.0f"), Best, *Lv.Sectors[Best].Name, Lv.Sectors[Best].Ambient, BestArea);
	return Best;
}

bool AOutlawsLevelActor::FindGroundNear(const FVector& Near, float HalfHeight, FVector& OutCentre) const
{
	if (!LevelData) return false;
	auto Clear = [this](const FVector& Feet)
	{
		for (const TWeakObjectPtr<AOutlawsEnemy>& W : Enemies)
		{
			const AOutlawsEnemy* E = W.Get();
			if (E && !E->IsDead() && FVector::Dist2D(E->GetActorLocation(), Feet) < 120.0) return false;
		}
		return true;
	};
	double Best = 1e30;
	bool bFound = false;
	const TBitArray<>* Skip = PlayerSkip();
	for (const FVector& Feet : EnemySpawns)
	{
		const double D = FVector::DistSquared(Feet, Near);
		if (Skip && FOutlawsLevel::Skipped(Skip, CullSectorOf(Feet + FVector(0.0, 0.0, 10.0)))) continue;     // (BUG-MILLCLIMB: not a zone gate's far side)
		if (D < Best && Clear(Feet))
		{
			Best = D;
			OutCentre = Feet + FVector(0.0, 0.0, HalfHeight + 2.0);
			bFound = true;
		}
	}
	if (!bFound && bHavePlayerStart)
	{
		OutCentre = PlayerStartLocation;
		bFound = true;
	}
	return bFound;
}

UProceduralMeshComponent* AOutlawsLevelActor::MakeBlocker(const FString& Kind, const FOutlawsMeshSection& S)
{
	if (S.Triangles.Num() == 0) return nullptr;
	UProceduralMeshComponent* M = NewMeshComponent(*(TEXT("OutlawsBlock") + Kind), true);
	M->CreateMeshSection_LinearColor(0, S.Positions, S.Triangles, S.Normals, S.UVs, S.Colors, TArray<FProcMeshTangent>(), true);
	M->SetVisibility(false);
	M->SetHiddenInGame(true);
	M->SetCollisionResponseToAllChannels(ECR_Ignore);
	if (Kind == TEXT("ENEMY"))
	{
		M->SetCollisionObjectType(ECC_GameTraceChannel2);
		M->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	}
	else
	{
		// (a NOPASS wall bullets go through: its own channel, "PawnBlocker", so a bad guy held or thrown passes it, user
		// 2026-10-01; the player and the living keep to it)
		M->SetCollisionObjectType(Kind == TEXT("PAWN") ? ECC_GameTraceChannel3 : ECC_WorldStatic);
		if (Kind != TEXT("SHOT")) M->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
		if (Kind != TEXT("PAWN")) M->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
	}
	return M;
}

void AOutlawsLevelActor::FindOverlaps(const FOutlawsLevel& L, const FOutlawsBuildFilter& Static, TSet<int32>& OutSectors)
{
	// two sectors share space when a point inside one's floor plan lies inside the other's too, with their floor-to-
	// ceiling spans overlapping by over half a unit there, and neither adjoins the other.  The points: each triangle's
	// middle and the points toward its corners, and a grid over the two plans' common box (the middles alone missed 14 of
	// the mill's 22 pairs; user 2026-09-29).  Sectors with no walls (the sky wrapped round a level) are left alone; a
	// sector whose floor, ceiling or walls a lift or a scroll carries counts all the same, its static faces going on its
	// own meshes (the mill's flume channels lie inside its halls: their walls stood in the rooms)
	const int32 N = L.Sectors.Num();
	TArray<FBox2D> Box;
	Box.Init(FBox2D(ForceInit), N);
	for (int32 i = 0; i < N; ++i)
	{
		for (const FVector2D& V : L.Sectors[i].Verts) Box[i] += V;
	}
	TArray<bool> Ok;
	Ok.SetNum(N);
	for (int32 i = 0; i < N; ++i)
	{
		const FOutlawsSector& S = L.Sectors[i];
		Ok[i] = !S.NoWalls() && S.Verts.Num() >= 3 && Box[i].bIsValid;
	}
	auto Adjoined = [&](int32 a, int32 b)
	{
		for (const FOutlawsWall& W : L.Sectors[a].Walls) if (W.Adjoin == b || W.DAdjoin == b) return true;
		for (const FOutlawsWall& W : L.Sectors[b].Walls) if (W.Adjoin == a || W.DAdjoin == a) return true;
		return false;
	};
	auto SharedAt = [&](int32 a, int32 b, const FVector2D& Pt)
	{
		const FOutlawsSector& SA = L.Sectors[a];
		const FOutlawsSector& SB = L.Sectors[b];
		if (!L.ContainsXZ(SA, Pt.X, Pt.Y) || !L.ContainsXZ(SB, Pt.X, Pt.Y)) return false;
		const double Lo = FMath::Max(L.SurfaceY(SA, Pt, true), L.SurfaceY(SB, Pt, true));
		const double Hi = FMath::Min(L.SurfaceY(SA, Pt, false), L.SurfaceY(SB, Pt, false));
		return Hi - Lo > 0.5;
	};
	// each sector's triangle points, made when first wanted
	TArray<TArray<FVector2D>> TriPts;
	TriPts.SetNum(N);
	TArray<bool> TriDone;
	TriDone.Init(false, N);
	TArray<int32> Tris;
	auto PointsOf = [&](int32 s) -> const TArray<FVector2D>&
	{
		if (!TriDone[s])
		{
			TriDone[s] = true;
			Tris.Reset();
			const FOutlawsSector& S = L.Sectors[s];
			if (FOutlawsGeometry::TriangulateSector(S, Tris))
			{
				for (int32 t = 0; t + 2 < Tris.Num(); t += 3)
				{
					const FVector2D P0 = S.Verts[Tris[t]], P1 = S.Verts[Tris[t + 1]], P2 = S.Verts[Tris[t + 2]];
					const FVector2D C = (P0 + P1 + P2) / 3.0;
					TriPts[s].Append({ C, (C + P0) * 0.5, (C + P1) * 0.5, (C + P2) * 0.5 });
				}
			}
		}
		return TriPts[s];
	};
	for (int32 a = 0; a < N; ++a)
	{
		if (!Ok[a]) continue;
		for (int32 b = a + 1; b < N; ++b)
		{
			if (!Ok[b] || !Box[a].Intersect(Box[b]) || Adjoined(a, b)) continue;
			const FBox2D Common(FVector2D(FMath::Max(Box[a].Min.X, Box[b].Min.X), FMath::Max(Box[a].Min.Y, Box[b].Min.Y)),
								FVector2D(FMath::Min(Box[a].Max.X, Box[b].Max.X), FMath::Min(Box[a].Max.Y, Box[b].Max.Y)));
			const FVector2D Size = Common.GetSize();
			if (Size.X <= 0.0 || Size.Y <= 0.0) continue;
			bool bShared = false;
			for (int32 s : { a, b })
			{
				for (const FVector2D& Pt : PointsOf(s))
				{
					if (Common.IsInside(Pt) && SharedAt(a, b, Pt)) { bShared = true; break; }
				}
				if (bShared) break;
			}
			// the common box walked at about a twelfth of its shorter side (half a unit at least, four at most)
			const double Step = FMath::Clamp(FMath::Min(Size.X, Size.Y) / 12.0, 0.5, 4.0);
			for (double y = Common.Min.Y + Step * 0.5; y < Common.Max.Y && !bShared; y += Step)
			{
				for (double x = Common.Min.X + Step * 0.5; x < Common.Max.X; x += Step)
				{
					if (SharedAt(a, b, FVector2D(x, y))) { bShared = true; break; }
				}
			}
			if (!bShared) continue;
			OverlapPairs.Add(TPair<int32, int32>(a, b));
			OutSectors.Add(a);
			OutSectors.Add(b);
		}
	}
	// what a lift or a scroll already carries of these sectors: not for the overlap meshes
	OverlapCarried = FOutlawsBuildFilter();
	for (const int32 Si : OutSectors)
	{
		if (Static.SkipFloor.Contains(Si)) OverlapCarried.SkipFloor.Add(Si);
		if (Static.SkipCeil.Contains(Si)) OverlapCarried.SkipCeil.Add(Si);
		for (int32 w = 0; w < L.Sectors[Si].Walls.Num(); ++w)
		{
			const uint64 Kw = FOutlawsBuildFilter::Key(Si, w);
			if (Static.SkipWalls.Contains(Kw)) OverlapCarried.SkipWalls.Add(Kw);
		}
	}
	if (OverlapPairs.Num() > 0)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT overlapping sectors: %d pairs among %d sectors, each on its own meshes"), OverlapPairs.Num(), OutSectors.Num());
		FString List;
		for (const TPair<int32, int32>& Pr : OverlapPairs) List += FString::Printf(TEXT(" %d/%d"), Pr.Key, Pr.Value);
		UE_LOG(LogOutlaws, Log, TEXT("OLRT overlapping pairs:%s"), *List);
	}
}

void AOutlawsLevelActor::BuildOverlapParts(const FOutlawsLevel& L, const TMap<FString, FOutlawsTexInfo>& TexInfo, const FOutlawsBuildFilter& Static)
{
	TSet<int32> Sectors;
	for (const TPair<int32, int32>& P : OverlapPairs) { Sectors.Add(P.Key); Sectors.Add(P.Value); }
	for (const int32 Si : Sectors)
	{
		FOutlawsBuildFilter F;
		F.bOnly = true;
		if (!OverlapCarried.SkipFloor.Contains(Si)) F.OnlyFloor.Add(Si);
		if (!OverlapCarried.SkipCeil.Contains(Si)) F.OnlyCeil.Add(Si);
		for (int32 w = 0; w < L.Sectors[Si].Walls.Num(); ++w)
		{
			const uint64 Kw = FOutlawsBuildFilter::Key(Si, w);
			if (!OverlapCarried.SkipWalls.Contains(Kw)) F.OnlyWalls.Add(Kw);      // (a lift's or a scroll's faces stay theirs)
		}
		F.SkipMid = Static.SkipMid;                // (glass panes stay on their own meshes)
		F.FloorShift = Static.FloorShift;
		if (!OLFlags::Param(TEXT("OLOldMillvis"))) F.FillHolesOf = Static.FillHolesOf;        // (BUG-MILLVIS: a door leaf's hole filled, as in the world mesh)
		FOutlawsGeometry Gs;
		Gs.Build(L, TexInfo, bReverseWinding, &F);
		TArray<FOverlapPart>& Parts = OverlapParts.Add(Si);
		int32 Tris = 0;
		if (Gs.World.Num() > 0 || Gs.Tint.Triangles.Num() > 0)
		{
			UProceduralMeshComponent* M = NewMeshComponent(*FString::Printf(TEXT("OutlawsSector%d"), Si), true);
			const int32 TintIndex = AddSections(M, Gs.World, true, WorldMaterial, WorldMaskedMaterial, Tris);
			if (bTintedGlass && Gs.Tint.Triangles.Num() > 0)
			{
				M->CreateMeshSection_LinearColor(TintIndex, Gs.Tint.Positions, Gs.Tint.Triangles, Gs.Tint.Normals, Gs.Tint.UVs, Gs.Tint.Colors, TArray<FProcMeshTangent>(), false);
				M->SetMaterial(TintIndex, GlassTintMaterial());
			}
			ObjectMeshes.Add(M);
			Parts.Add({ M, true });
		}
		if (Gs.Water.Num() > 0)
		{
			UProceduralMeshComponent* M = NewMeshComponent(*FString::Printf(TEXT("OutlawsSectorWater%d"), Si), false);
			AddSections(M, Gs.Water, false, WaterMaterial, WaterMaterial, Tris);
			ObjectMeshes.Add(M);
			Parts.Add({ M, true });
		}
		if (SkySurfaceMid && Gs.SkyFaces.Triangles.Num() > 0)
		{
			UProceduralMeshComponent* M = NewMeshComponent(*FString::Printf(TEXT("OutlawsSectorSky%d"), Si), false);
			M->CreateMeshSection_LinearColor(0, Gs.SkyFaces.Positions, Gs.SkyFaces.Triangles, Gs.SkyFaces.Normals, Gs.SkyFaces.UVs, Gs.SkyFaces.Colors, TArray<FProcMeshTangent>(), false);
			M->SetMaterial(0, SkySurfaceMid);
			ObjectMeshes.Add(M);
			Parts.Add({ M, true });
		}
		for (const TPair<FString, FOutlawsMeshSection>& Kv : Gs.Blockers)
		{
			if (UProceduralMeshComponent* M = MakeBlocker(Kv.Key, Kv.Value))
			{
				ObjectMeshes.Add(M);
				Parts.Add({ M, false });
			}
		}
		// a scrolling floor or ceiling of the sector (the flume's running water) goes with it, when its mesh carries
		// nothing of any other sector
		for (const FScrollMesh& SM : ScrollMeshes)
		{
			if (!SM.Mesh) continue;
			TSet<int32> Covered = SM.Filter.OnlyFloor;
			Covered.Append(SM.Filter.OnlyCeil);
			for (const uint64 Kw : SM.Filter.OnlyWalls) Covered.Add(int32(Kw >> 32));
			if (Covered.Num() == 1 && Covered.Contains(Si)) Parts.Add({ SM.Mesh.Get(), true });
		}
	}
}

void AOutlawsLevelActor::UpdateOverlaps(int32 FromSector)
{
	// (the choice is made each tick from the eye and the body: ApplyOverlapChoice)
	OverlapFrom = FromSector;
}

void AOutlawsLevelActor::PortalFlood(const FOutlawsLevel& L, int32 From, const FVector2D& Eye, double EyeZ, TSet<int32>& Out, TMap<int32, int32>* Depth, FFloodBounds* Bounds) const
{
	// what the eye sees, as the original's portal renderer finds it: from the eye's sector through its openings, each
	// further opening seen only within the angles the openings before it leave, round in plan and up and down (user
	// 2026-10-01: in plan alone a crawlway under a room saw the room through openings far over the eye, and sectors
	// sharing space showed together), a lift's or the water's sector above or below with the same view.  An opening's
	// height is what both sides leave open (a shut door none); a sloped side is taken as open its whole height.  Unblown
	// explodable walls are shut
	if (Bounds)
	{
		Bounds->SafeR = 0.0;
		Bounds->Read.Init(false, L.Sectors.Num());
	}
	if (!L.Sectors.IsValidIndex(From)) return;
	// (QSPEED-3, Bounds) each window edge also carries how fast it can turn as the eye moves (radians a game unit, within
	// the radius allowed) and what it is the angle of (an id: the bearing of one vertex, or the up-and-down angle of one
	// height over one distance, with the slack's sign; 0 a constant).  Two edges of one id are the same sum of the eye's
	// position (whole turns apart at most), so they compare alike wherever it goes; every other choice the eye's position
	// enters limits how far the eye may move before it could go the other way.  Inside that radius, with no height it
	// read changed, the flood would come out the same
	struct FItem
	{
		int32 S; double Lo, Hi; int32 Depth; bool bFull; double VLo, VHi;
		double LLo = 0.0, LHi = 0.0, LVLo = 0.0, LVHi = 0.0;
		uint64 ILo = 0, IHi = 0, IVLo = 0, IVHi = 0;
	};
	const bool bTrack = Bounds && DebugFloodMode == 0;
	double SafeR = 1e30;
	auto Mix = [](uint64 H, uint64 V) { H ^= V + 0x9E3779B97F4A7C15ull + (H << 6) + (H >> 2); H ^= H >> 31; H *= 0xBF58476D1CE4E5B9ull; return H ^ (H >> 29); };
	auto Bits = [](double D) { uint64 U; FMemory::Memcpy(&U, &D, sizeof(U)); return U; };
	auto PointId = [&Mix, &Bits](uint64 Tag, const FVector2D& P) { return Mix(Mix(Tag, Bits(P.X)), Bits(P.Y)) | 1ull; };
	// (each Keep and Cap below carries a number of its own, the choice it stands for: the OLVISCACHE line counts which one
	// set each flood's radius)
	int32 MinSite = 0;
	auto Keep = [&SafeR, &MinSite](double Diff, double Rate, int32 Site) { if (Rate > 0.0 && FMath::Abs(Diff) / Rate < SafeR) { SafeR = FMath::Abs(Diff) / Rate; MinSite = Site; } };
	auto Cap = [&SafeR, &MinSite](double MaxR, int32 Site) { if (MaxR < SafeR) { SafeR = MaxR; MinSite = Site; } };
	// a clamp, the larger (or smaller) of X and Y: the winner's id while the other side could not overtake it within
	// RefineMin of the eye (that kept), else an id of its own for "the larger of these two" (the same sum wherever the eye
	// goes, turning no faster than the faster of them: nothing to keep).  The winner's id is the finer one: it matches
	// the same edge reached another way
	const double RefineMin = 0.2;
	auto Pick = [&Keep, &Mix, RefineMin](double X, double LX, uint64 IX, double Y, double LY, uint64 IY, bool bXWins, uint64 Tag, double& OutL, uint64& OutI, int32 Site)
	{
		if (IX == IY) { OutL = FMath::Max(LX, LY); OutI = IX; return; }
		if (LX + LY <= 0.0 || FMath::Abs(X - Y) >= RefineMin * (LX + LY))
		{
			Keep(X - Y, LX + LY, Site);
			OutL = bXWins ? LX : LY;
			OutI = bXWins ? IX : IY;
			return;
		}
		OutL = FMath::Max(LX, LY);
		OutI = Mix(Mix(Tag, FMath::Min(IX, IY)), FMath::Max(IX, IY)) | 1ull;
	};
	// (QSPEED-3) the stack and the windows by sector are kept from one flood to the next (game thread only, never
	// nested): each flood clears only the sectors the last one wrote, instead of making a list for every sector again
	static TArray<FItem> Stack;
	Stack.Reset();
	// the openings of each sector (walls with an adjoin, ends valid, not of no length), in wall order, and each sector's
	// winding, from the plan (static for a level: only heights move); and the eye's sums for each, stamped by flood
	struct FFloodPortal { int32 W; FVector2D A, B; double Len; };
	struct FFloodEyeWall { uint32 Stamp = 0; bool bEnds = false; double Side = 0.0, DClose = 0.0, a = 0.0, b = 0.0, DA = 0.0, DB = 0.0; FVector2D Close = FVector2D::ZeroVector; };
	struct FFloodEyeOpen { uint32 Stamp = 0; bool bShut = false; double Vb = 0.0, Vt = 0.0, Lvb = 0.0, Lvt = 0.0; uint64 Vbid = 0, Vtid = 0; };
	struct FFloodGeo
	{
		const FOutlawsLevel* Level = nullptr;
		int32 NumS = -1;
		uint32 Gen = 0;
		uint32 Stamp = 0;
		TArray<int32> Start;
		TArray<FFloodPortal> Portals;
		TArray<double> Inside;
		TArray<FFloodEyeWall> Eye;
		TArray<FFloodEyeOpen> Open;            // (two an opening: its adjoin and its second one)
	};
	static FFloodGeo Geo;
	if (Geo.Level != &L || Geo.NumS != L.Sectors.Num() || Geo.Gen != FloodGeomGen)
	{
		Geo.Level = &L;
		Geo.NumS = L.Sectors.Num();
		Geo.Gen = FloodGeomGen;
		Geo.Start.Reset();
		Geo.Portals.Reset();
		Geo.Inside.Reset();
		for (const FOutlawsSector& Gs : L.Sectors)
		{
			Geo.Start.Add(Geo.Portals.Num());
			double Area2 = 0.0;
			for (int32 i = 0; i < Gs.Verts.Num(); ++i) { const FVector2D& P = Gs.Verts[i]; const FVector2D& Q = Gs.Verts[(i + 1) % Gs.Verts.Num()]; Area2 += P.X * Q.Y - Q.X * P.Y; }
			Geo.Inside.Add(Area2 < 0.0 ? -1.0 : 1.0);
			if (Gs.Verts.Num() < 3) continue;
			for (int32 w = 0; w < Gs.Walls.Num(); ++w)
			{
				const FOutlawsWall& W = Gs.Walls[w];
				if (W.Adjoin < 0 && W.DAdjoin < 0) continue;
				if (!Gs.Verts.IsValidIndex(W.V1) || !Gs.Verts.IsValidIndex(W.V2)) continue;
				const FVector2D A = Gs.Verts[W.V1], B = Gs.Verts[W.V2];
				const double Len = FVector2D::Distance(A, B);
				if (Len < 1e-4) continue;
				Geo.Portals.Add({ w, A, B, Len });
			}
		}
		Geo.Start.Add(Geo.Portals.Num());
		Geo.Eye.Reset();
		Geo.Eye.SetNum(Geo.Portals.Num());
		Geo.Open.Reset();
		Geo.Open.SetNum(2 * Geo.Portals.Num());
		Geo.Stamp = 0;
	}
	if (++Geo.Stamp == 0)
	{
		for (FFloodEyeWall& Me : Geo.Eye) Me.Stamp = 0;
		for (FFloodEyeOpen& Mo : Geo.Open) Mo.Stamp = 0;
		Geo.Stamp = 1;
	}
	TArray<uint8> Visits;
	TSet<uint64> Shut;
	for (const FExplodeWall& X : ExplodeWalls) if (!X.bBlown) for (const uint64 K : X.Walls) Shut.Add(K);
	auto Wrap = [](double A) { while (A > PI) A -= 2.0 * PI; while (A <= -PI) A += 2.0 * PI; return A; };
	// (BUG-POPIN, build 22: a still player's head sway of a few cm moved a grazing opening's window across the next one's
	// edge, the sector beyond flipping in and out) every opening seen a little wider, round and up and down
	const double Slack = CVarOLVisSlack.GetValueOnGameThread();
	// (a test aid, DebugVisScan: 1 lifts the visit and depth caps, 2 the up-and-down window, 4 the round one, 8 the
	// build-22 rule of a dozen arrivals a sector)
	const bool bNoCap = (DebugFloodMode & 1) != 0, bNoV = (DebugFloodMode & 2) != 0, bNoAng = (DebugFloodMode & 4) != 0, bOld = (DebugFloodMode & 8) != 0;
	if (bNoCap || bOld) Visits.Init(0, L.Sectors.Num());
	Stack.Add({ From, 0.0, 0.0, 0, true, -HALF_PI, HALF_PI });
	// (BUG-POPIN, build 22: a dozen arrivals a sector, whatever their windows, left rooms and yards undrawn in clear view:
	// a sector reached first through many narrow gaps had no arrival left for the wide opening seen later.  The original
	// has no such count, only the depth (MAX_ADJOIN_DEPTH 40), and enters a sector through every opening any part of the
	// view still passes) an arrival is passed on unless an earlier one into the same sector had a window holding all of
	// it, round and up and down; the depth as the original's, and a budget on the whole flood
	struct FWin
	{
		double Lo, Hi, VLo, VHi; bool bFull;
		double LLo, LHi, LVLo, LVHi;
		uint64 ILo, IHi, IVLo, IVHi;
	};
	// (Bounds) the slack's sign in an edge's id: with no slack the first edge of one opening and the last of the next one
	// are the one bearing of the vertex they share
	const uint64 TagLo = Slack != 0.0 ? 11 : 13, TagHi = Slack != 0.0 ? 12 : 13;
	static TArray<TArray<FWin, TInlineAllocator<4>>> Wins;
	static TArray<int32> WinsUsed;
	for (const int32 Wu : WinsUsed) if (Wins.IsValidIndex(Wu)) Wins[Wu].Reset();
	WinsUsed.Reset();
	if (!bOld && !bNoCap && Wins.Num() < L.Sectors.Num()) Wins.SetNum(L.Sectors.Num());
	int32 Budget = 40000;
	DebugFloodPops = 0;
	while (Stack.Num() > 0)
	{
		const FItem It = Stack.Pop(false);
		Out.Add(It.S);
		if (Depth)
		{
			int32& D = Depth->FindOrAdd(It.S, It.Depth);
			D = FMath::Min(D, It.Depth);
		}
		const FOutlawsSector& S = L.Sectors[It.S];
		if (L.Sectors.IsValidIndex(S.VAdjoin)) Out.Add(S.VAdjoin);
		if (bNoCap) { if (Visits[It.S]++ >= 48 || It.Depth >= 160) continue; }
		else if (bOld) { if (It.Depth >= 48 || Visits[It.S]++ >= 12) continue; }
		else
		{
			if (It.Depth >= 40 || --Budget < 0) continue;
			bool bHeld = false;
			for (const FWin& Wn : Wins[It.S])
			{
				const bool bBelow = It.VLo < Wn.VLo - 1e-9;
				if (bTrack && It.IVLo != Wn.IVLo) Keep(It.VLo - Wn.VLo + 1e-9, It.LVLo + Wn.LVLo, 1);
				if (bBelow) continue;
				const bool bAbove = It.VHi > Wn.VHi + 1e-9;
				if (bTrack && It.IVHi != Wn.IVHi) Keep(It.VHi - Wn.VHi - 1e-9, It.LVHi + Wn.LVHi, 2);
				if (bAbove) continue;
				if (Wn.bFull) { bHeld = true; break; }
				if (It.bFull) continue;
				const double D0 = Wrap(It.Lo - Wn.Lo);
				const bool bSameLo = It.ILo == Wn.ILo;
				if (bTrack && !bSameLo)
				{
					Keep(PI - FMath::Abs(D0), It.LLo + Wn.LLo, 3);      // (the wrap's seam)
					Keep(D0 + 1e-9, It.LLo + Wn.LLo, 4);
				}
				if (D0 < -1e-9) continue;
				// (the test below is It.Hi against Wn.Hi, whole turns apart fixed while the seam above holds: one id, nothing to keep)
				if (bTrack && It.IHi != Wn.IHi) Keep(D0 + (It.Hi - It.Lo) - (Wn.Hi - Wn.Lo) - 1e-9, It.LHi + Wn.LHi, 5);
				if (D0 + (It.Hi - It.Lo) <= (Wn.Hi - Wn.Lo) + 1e-9) { bHeld = true; break; }
			}
			if (bHeld) continue;
			if (Wins[It.S].Num() == 0) WinsUsed.Add(It.S);
			Wins[It.S].Add({ It.Lo, It.Hi, It.VLo, It.VHi, It.bFull, It.LLo, It.LHi, It.LVLo, It.LVHi, It.ILo, It.IHi, It.IVLo, It.IVHi });
		}
		++DebugFloodPops;
		// (BUG-POPIN) through a water surface (the sector above or below) the same view goes on: from under the water the
		// world above, its bad guys too (only the sector beyond was drawn, nothing past it)
		if (!bOld && L.Sectors.IsValidIndex(S.VAdjoin))
		{
			FItem Up = It;
			Up.S = S.VAdjoin;
			Up.Depth = It.Depth + 1;
			Stack.Add(Up);
		}
		if (S.Verts.Num() < 3) continue;
		// (QSPEED-3) the sector's openings from the plan list (the winding's side of each and its ends worked out once a
		// level), the eye's own sums for each once a flood however many arrivals the sector has (the same sums: the
		// same results)
		const double Inside = Geo.Inside[It.S];         // the winding: which side of each wall is the sector's inside
		for (int32 k = Geo.Start[It.S]; k < Geo.Start[It.S + 1]; ++k)
		{
			const FFloodPortal& Pt = Geo.Portals[k];
			const int32 w = Pt.W;
			const FOutlawsWall& W = S.Walls[w];
			if (Shut.Num() > 0 && Shut.Contains(FOutlawsBuildFilter::Key(It.S, w))) continue;
			const FVector2D A = Pt.A, B = Pt.B;
			const double Len = Pt.Len;
			FFloodEyeWall& Me = Geo.Eye[k];
			if (Me.Stamp != Geo.Stamp)
			{
				Me.Stamp = Geo.Stamp;
				Me.bEnds = false;
				Me.Side = FVector2D::CrossProduct(B - A, Eye - A) / Len * Inside;     // > 0: the eye on this sector's side
				Me.Close = FMath::ClosestPointOnSegment2D(Eye, A, B);
				Me.DClose = FVector2D::Distance(Eye, Me.Close);
			}
			const double Side = Me.Side;
			const FVector2D Close = Me.Close;
			const double DClose = Me.DClose;
			const bool bAt = DClose < 0.6;                                                 // the eye in the opening itself
			double Lo = 0.0, Hi = 0.0;
			double LLo = 0.0, LHi = 0.0;
			uint64 ILo = 0, IHi = 0;
			bool bFull = false;
			if (bTrack && It.bFull) Keep(DClose - 0.6, 1.0, 6);       // (bAt decides only under a full view)
			if (bAt && It.bFull)
			{
				bFull = true;                                                              // both sides are the eye's own
			}
			else
			{
				if (bTrack) Keep(Side - 1e-3, 1.0, 7);
				if (Side < 1e-3) continue;                                                 // seen from behind
				if (!Me.bEnds)
				{
					Me.bEnds = true;
					Me.a = FMath::Atan2(A.Y - Eye.Y, A.X - Eye.X);
					Me.b = FMath::Atan2(B.Y - Eye.Y, B.X - Eye.X);
					Me.DA = FVector2D::Distance(Eye, A);
					Me.DB = FVector2D::Distance(Eye, B);
				}
				const double a = Me.a, b = Me.b;
				// (Bounds) an end's bearing turns at most 1.05 / its distance a unit while the eye stays within half of it
				double La = 0.0, Lb = 0.0;
				if (bTrack)
				{
					const double DA = Me.DA, DB = Me.DB;
					Cap(DA * 0.5, 8);
					Cap(DB * 0.5, 9);
					La = 1.05 / FMath::Max(DA, 1e-12);
					Lb = 1.05 / FMath::Max(DB, 1e-12);
				}
				if (It.bFull || bNoAng)
				{
					const double D = Wrap(b - a);
					Lo = (D >= 0.0 ? a : b) - Slack;
					Hi = Lo + FMath::Abs(D) + 2.0 * Slack;
					if (bTrack)
					{
						// (which end comes first, and the wrap's seam, hold while Side does: the eye stays off the wall's line)
						Keep(PI - FMath::Abs(D >= 0.0 ? a : b), D >= 0.0 ? La : Lb, 10);     // the bearing's own seam (Lo is that bearing as is)
						LLo = D >= 0.0 ? La : Lb;
						LHi = D >= 0.0 ? Lb : La;
						ILo = PointId(TagLo, D >= 0.0 ? A : B);
						IHi = PointId(TagHi, D >= 0.0 ? B : A);
					}
				}
				else
				{
					double a2, b2;
					double Ls = 0.0, Le = 0.0;           // (Bounds) how fast the opening's first and last edge turn
					uint64 Is = 0, Ie = 0;               // ... and what they are the bearings of
					if (bOld)
					{
						const double M = (It.Lo + It.Hi) * 0.5;
						a2 = M + Wrap(a - M); b2 = M + Wrap(b - M);
						if (a2 > b2) Swap(a2, b2);
						if (b2 - a2 > PI) continue;
					}
					else
					{
						// (BUG-POPIN) the opening's own span (under half a turn, the eye before it), then turned to lie by the
						// window: taken about the window's middle instead, a long wall seen across the turn's seam (the train's
						// trackside, west of the eye) came out the other way round, over half a turn, and was dropped
						const double D = Wrap(b - a);
						const double Turn = Wrap((D >= 0.0 ? a : b) - It.Lo);
						a2 = It.Lo + Turn;
						b2 = a2 + FMath::Abs(D);
						if (bTrack)
						{
							Ls = D >= 0.0 ? La : Lb;
							Le = D >= 0.0 ? Lb : La;
							Is = PointId(TagLo, D >= 0.0 ? A : B);
							Ie = PointId(TagHi, D >= 0.0 ? B : A);
							if (Is != It.ILo) Keep(PI - FMath::Abs(Turn), Ls + It.LLo, 11);     // the turn to the window's seam
						}
					}
					Lo = FMath::Max(It.Lo, a2 - Slack);
					Hi = FMath::Min(It.Hi, b2 + Slack);
					if (bTrack)
					{
						// the clamps: the winner's id while the other side is well clear, else the pair's own (see Pick)
						Pick(It.Lo, It.LLo, It.ILo, a2 - Slack, Ls, Is, It.Lo >= a2 - Slack, 41, LLo, ILo, 12);
						Pick(It.Hi, It.LHi, It.IHi, b2 + Slack, Le, Ie, It.Hi <= b2 + Slack, 42, LHi, IHi, 13);
						if (ILo != IHi) Keep(Hi - Lo - 1e-5, LHi + LLo, 14);
					}
					if (Hi - Lo <= 1e-5) continue;
				}
			}
			for (int32 j = 0; j < 2; ++j)
			{
				const int32 Next = j == 0 ? W.Adjoin : W.DAdjoin;
				if (Next < 0 || !L.Sectors.IsValidIndex(Next)) continue;
				// up and down: the opening's height (what both sides leave open) seen from the eye, nearest and furthest
				double VLo = It.VLo, VHi = It.VHi;
				double LVLo = It.LVLo, LVHi = It.LVHi;
				uint64 IVLo = It.IVLo, IVHi = It.IVHi;
				if (!bFull)
				{
					// (QSPEED-3) the opening's own angles once a flood (the heights do not move within one)
					FFloodEyeOpen& Mo = Geo.Open[2 * k + j];
					if (Mo.Stamp != Geo.Stamp)
					{
						Mo.Stamp = Geo.Stamp;
						const FOutlawsSector& N2 = L.Sectors[Next];
						if (Bounds)
						{
							Bounds->Read[It.S] = true;      // (the heights read: a lift or door moving either one floods again)
							Bounds->Read[Next] = true;
						}
						const bool bSloped = ((S.Flags | N2.Flags) & (OutlawsFlags::SlopedFloor | OutlawsFlags::SlopedCeiling)) != 0;
						const double Bottom = bSloped ? FMath::Min(S.Floor.Y, N2.Floor.Y) : FMath::Max(S.Floor.Y, N2.Floor.Y);
						const double Top = bSloped ? FMath::Max(S.Ceil.Y, N2.Ceil.Y) : FMath::Min(S.Ceil.Y, N2.Ceil.Y);
						Mo.bShut = Top - Bottom <= 0.01;                                       // shut (a closed door)
						if (!Mo.bShut && !bNoV)
						{
							const double DNear = FMath::Max(0.05, DClose);
							const double DFar = FMath::Max(DNear, FMath::Max(Me.DA, Me.DB));
							const double Zb = Bottom - EyeZ, Zt = Top - EyeZ;
							Mo.Vb = FMath::Atan2(Zb, Zb < 0.0 ? DNear : DFar) - Slack;
							Mo.Vt = FMath::Atan2(Zt, Zt > 0.0 ? DNear : DFar) + Slack;
							if (bTrack)
							{
								// (Bounds) the up-and-down angle of an edge r away (height and plan distance, both moving at most as
								// far as the eye) turns at most 2.83 / r a unit while the eye stays within r / 2.83; it does not jump
								// where the height crosses the eye's (both sides read 0 there)
								const double Rb = FMath::Max(1e-12, FMath::Sqrt(Zb * Zb + DNear * DNear)), Rt = FMath::Max(1e-12, FMath::Sqrt(Zt * Zt + DNear * DNear));
								Cap(Rb / 2.83, 15);
								Cap(Rt / 2.83, 16);
								Mo.Lvb = 2.83 / Rb;
								Mo.Lvt = 2.83 / Rt;
								// what the two distances are distances to (ids, as the bearings): the wall as a whole (always right),
								// or, while it stays so within RefineMin, one end of it (the nearest, or for the far one the further
								// end), so that the same end seen past another wall matches
								const FVector2D Seg = B - A;
								const double Dot1 = (Eye - A) | Seg, Dot2 = Seg | Seg;
								const uint64 IdA = PointId(22, A), IdB = PointId(22, B);
								const uint64 SegId = Mix(Mix(23, FMath::Min(IdA, IdB)), FMath::Max(IdA, IdB)) | 1ull;
								uint64 NearId = SegId;
								if (Dot1 <= 0.0 && -Dot1 >= RefineMin * Len) { NearId = IdA; Keep(Dot1 / Len, 1.0, 17); }
								else if (Dot2 <= Dot1 && Dot1 - Dot2 >= RefineMin * Len) { NearId = IdB; Keep((Dot2 - Dot1) / Len, 1.0, 18); }
								const double DAe = Me.DA, DBe = Me.DB;
								uint64 FarId = Mix(24, SegId) | 1ull;
								if (FMath::Abs(DAe - DBe) >= 2.0 * RefineMin && FMath::Max(DAe, DBe) - DNear >= 2.0 * RefineMin)
								{
									FarId = DAe >= DBe ? IdA : IdB;
									Keep(DAe - DBe, 2.0, 21);
									Keep(DNear - FMath::Max(DAe, DBe), 2.0, 22);
								}
								// (which of the two each angle reads, by the height's side of the eye: kept while well clear, else
								// the whole wall's sum, both sides)
								if (FMath::Abs(Zb) >= RefineMin) { Mo.Vbid = Mix(Mix(Slack != 0.0 ? 31 : 33, Bits(Bottom)), Zb < 0.0 ? NearId : FarId) | 1ull; Keep(Zb, 1.0, 23); }
								else Mo.Vbid = Mix(Mix(Slack != 0.0 ? 34 : 36, Bits(Bottom)), SegId) | 1ull;
								if (FMath::Abs(Zt) >= RefineMin) { Mo.Vtid = Mix(Mix(Slack != 0.0 ? 32 : 33, Bits(Top)), Zt > 0.0 ? NearId : FarId) | 1ull; Keep(Zt, 1.0, 24); }
								else Mo.Vtid = Mix(Mix(Slack != 0.0 ? 35 : 36, Bits(Top)), SegId) | 1ull;
							}
						}
					}
					if (Mo.bShut) continue;
					if (!bNoV)
					{
						const double Vb = Mo.Vb, Vt = Mo.Vt;
						VLo = FMath::Max(VLo, Vb);
						VHi = FMath::Min(VHi, Vt);
						if (bTrack)
						{
							Pick(It.VLo, It.LVLo, It.IVLo, Vb, Mo.Lvb, Mo.Vbid, It.VLo >= Vb, 43, LVLo, IVLo, 25);
							Pick(It.VHi, It.LVHi, It.IVHi, Vt, Mo.Lvt, Mo.Vtid, It.VHi <= Vt, 44, LVHi, IVHi, 26);
							if (IVLo != IVHi) Keep(VHi - VLo - 1e-4, LVHi + LVLo, 27);
						}
						if (VHi - VLo <= 1e-4) continue;
					}
				}
				Stack.Add({ Next, Lo, Hi, It.Depth + 1, bFull, VLo, VHi, LLo, LHi, LVLo, LVHi, ILo, IHi, IVLo, IVHi });
			}
		}
	}
	// (Bounds) half the radius found, for the rounding of the sums
	if (Bounds) { Bounds->SafeR = bTrack ? SafeR * 0.5 : 0.0; Bounds->Site = MinSite; }
}

#if WITH_OUTLAWS_TESTS
// (QSPEED-3, -OLVisCacheCheck) the flood as it was before the kept flood and its reused lists (bedceaa, word for word but
// the name): what every tick's flood is checked against
void AOutlawsLevelActor::PortalFloodRef(const FOutlawsLevel& L, int32 From, const FVector2D& Eye, double EyeZ, TSet<int32>& Out, TMap<int32, int32>* Depth) const
{
	// what the eye sees, as the original's portal renderer finds it: from the eye's sector through its openings, each
	// further opening seen only within the angles the openings before it leave, round in plan and up and down (user
	// 2026-10-01: in plan alone a crawlway under a room saw the room through openings far over the eye, and sectors
	// sharing space showed together), a lift's or the water's sector above or below with the same view.  An opening's
	// height is what both sides leave open (a shut door none); a sloped side is taken as open its whole height.  Unblown
	// explodable walls are shut
	if (!L.Sectors.IsValidIndex(From)) return;
	struct FItem { int32 S; double Lo, Hi; int32 Depth; bool bFull; double VLo, VHi; };
	TArray<FItem> Stack;
	TArray<uint8> Visits;
	Visits.Init(0, L.Sectors.Num());
	TSet<uint64> Shut;
	for (const FExplodeWall& X : ExplodeWalls) if (!X.bBlown) for (const uint64 K : X.Walls) Shut.Add(K);
	auto Wrap = [](double A) { while (A > PI) A -= 2.0 * PI; while (A <= -PI) A += 2.0 * PI; return A; };
	// (BUG-POPIN, build 22: a still player's head sway of a few cm moved a grazing opening's window across the next one's
	// edge, the sector beyond flipping in and out) every opening seen a little wider, round and up and down
	const double Slack = CVarOLVisSlack.GetValueOnGameThread();
	// (a test aid, DebugVisScan: 1 lifts the visit and depth caps, 2 the up-and-down window, 4 the round one, 8 the
	// build-22 rule of a dozen arrivals a sector)
	const bool bNoCap = (DebugFloodMode & 1) != 0, bNoV = (DebugFloodMode & 2) != 0, bNoAng = (DebugFloodMode & 4) != 0, bOld = (DebugFloodMode & 8) != 0;
	Stack.Add({ From, 0.0, 0.0, 0, true, -HALF_PI, HALF_PI });
	// (BUG-POPIN, build 22: a dozen arrivals a sector, whatever their windows, left rooms and yards undrawn in clear view:
	// a sector reached first through many narrow gaps had no arrival left for the wide opening seen later.  The original
	// has no such count, only the depth (MAX_ADJOIN_DEPTH 40), and enters a sector through every opening any part of the
	// view still passes) an arrival is passed on unless an earlier one into the same sector had a window holding all of
	// it, round and up and down; the depth as the original's, and a budget on the whole flood
	struct FWin { double Lo, Hi, VLo, VHi; bool bFull; };
	TArray<TArray<FWin, TInlineAllocator<4>>> Wins;
	if (!bOld && !bNoCap) Wins.SetNum(L.Sectors.Num());
	int32 Budget = 40000;
	DebugFloodPops = 0;
	while (Stack.Num() > 0)
	{
		const FItem It = Stack.Pop(false);
		Out.Add(It.S);
		if (Depth)
		{
			int32& D = Depth->FindOrAdd(It.S, It.Depth);
			D = FMath::Min(D, It.Depth);
		}
		const FOutlawsSector& S = L.Sectors[It.S];
		if (L.Sectors.IsValidIndex(S.VAdjoin)) Out.Add(S.VAdjoin);
		if (bNoCap) { if (Visits[It.S]++ >= 48 || It.Depth >= 160) continue; }
		else if (bOld) { if (It.Depth >= 48 || Visits[It.S]++ >= 12) continue; }
		else
		{
			if (It.Depth >= 40 || --Budget < 0) continue;
			bool bHeld = false;
			for (const FWin& Wn : Wins[It.S])
			{
				if (It.VLo < Wn.VLo - 1e-9 || It.VHi > Wn.VHi + 1e-9) continue;
				if (Wn.bFull) { bHeld = true; break; }
				if (It.bFull) continue;
				const double D0 = Wrap(It.Lo - Wn.Lo);
				if (D0 >= -1e-9 && D0 + (It.Hi - It.Lo) <= (Wn.Hi - Wn.Lo) + 1e-9) { bHeld = true; break; }
			}
			if (bHeld) continue;
			Wins[It.S].Add({ It.Lo, It.Hi, It.VLo, It.VHi, It.bFull });
		}
		++DebugFloodPops;
		// (BUG-POPIN) through a water surface (the sector above or below) the same view goes on: from under the water the
		// world above, its bad guys too (only the sector beyond was drawn, nothing past it)
		if (!bOld && L.Sectors.IsValidIndex(S.VAdjoin)) Stack.Add({ S.VAdjoin, It.Lo, It.Hi, It.Depth + 1, It.bFull, It.VLo, It.VHi });
		if (S.Verts.Num() < 3) continue;
		double Area2 = 0.0;             // the winding: which side of each wall is the sector's inside
		for (int32 i = 0; i < S.Verts.Num(); ++i) { const FVector2D& P = S.Verts[i]; const FVector2D& Q = S.Verts[(i + 1) % S.Verts.Num()]; Area2 += P.X * Q.Y - Q.X * P.Y; }
		const double Inside = Area2 < 0.0 ? -1.0 : 1.0;
		for (int32 w = 0; w < S.Walls.Num(); ++w)
		{
			const FOutlawsWall& W = S.Walls[w];
			if ((W.Adjoin < 0 && W.DAdjoin < 0) || Shut.Contains(FOutlawsBuildFilter::Key(It.S, w))) continue;
			if (!S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) continue;
			const FVector2D A = S.Verts[W.V1], B = S.Verts[W.V2];
			const double Len = FVector2D::Distance(A, B);
			if (Len < 1e-4) continue;
			const double Side = FVector2D::CrossProduct(B - A, Eye - A) / Len * Inside;     // > 0: the eye on this sector's side
			const FVector2D Close = FMath::ClosestPointOnSegment2D(Eye, A, B);
			const bool bAt = FVector2D::Distance(Eye, Close) < 0.6;                           // the eye in the opening itself
			double Lo = 0.0, Hi = 0.0;
			bool bFull = false;
			if (bAt && It.bFull)
			{
				bFull = true;                                                              // both sides are the eye's own
			}
			else
			{
				if (Side < 1e-3) continue;                                                 // seen from behind
				const double a = FMath::Atan2(A.Y - Eye.Y, A.X - Eye.X), b = FMath::Atan2(B.Y - Eye.Y, B.X - Eye.X);
				if (It.bFull || bNoAng)
				{
					const double D = Wrap(b - a);
					Lo = (D >= 0.0 ? a : b) - Slack;
					Hi = Lo + FMath::Abs(D) + 2.0 * Slack;
				}
				else
				{
					double a2, b2;
					if (bOld)
					{
						const double M = (It.Lo + It.Hi) * 0.5;
						a2 = M + Wrap(a - M); b2 = M + Wrap(b - M);
						if (a2 > b2) Swap(a2, b2);
						if (b2 - a2 > PI) continue;
					}
					else
					{
						// (BUG-POPIN) the opening's own span (under half a turn, the eye before it), then turned to lie by the
						// window: taken about the window's middle instead, a long wall seen across the turn's seam (the train's
						// trackside, west of the eye) came out the other way round, over half a turn, and was dropped
						const double D = Wrap(b - a);
						a2 = It.Lo + Wrap((D >= 0.0 ? a : b) - It.Lo);
						b2 = a2 + FMath::Abs(D);
					}
					Lo = FMath::Max(It.Lo, a2 - Slack);
					Hi = FMath::Min(It.Hi, b2 + Slack);
					if (Hi - Lo <= 1e-5) continue;
				}
			}
			for (const int32 Next : { W.Adjoin, W.DAdjoin })
			{
				if (Next < 0 || !L.Sectors.IsValidIndex(Next)) continue;
				// up and down: the opening's height (what both sides leave open) seen from the eye, nearest and furthest
				double VLo = It.VLo, VHi = It.VHi;
				if (!bFull)
				{
					const FOutlawsSector& N2 = L.Sectors[Next];
					const bool bSloped = ((S.Flags | N2.Flags) & (OutlawsFlags::SlopedFloor | OutlawsFlags::SlopedCeiling)) != 0;
					const double Bottom = bSloped ? FMath::Min(S.Floor.Y, N2.Floor.Y) : FMath::Max(S.Floor.Y, N2.Floor.Y);
					const double Top = bSloped ? FMath::Max(S.Ceil.Y, N2.Ceil.Y) : FMath::Min(S.Ceil.Y, N2.Ceil.Y);
					if (Top - Bottom <= 0.01) continue;                                    // shut (a closed door)
					const double DNear = FMath::Max(0.05, FVector2D::Distance(Eye, Close));
					const double DFar = FMath::Max(DNear, FMath::Max(FVector2D::Distance(Eye, A), FVector2D::Distance(Eye, B)));
					const double Zb = Bottom - EyeZ, Zt = Top - EyeZ;
					if (!bNoV)
					{
						VLo = FMath::Max(VLo, FMath::Atan2(Zb, Zb < 0.0 ? DNear : DFar) - Slack);
						VHi = FMath::Min(VHi, FMath::Atan2(Zt, Zt > 0.0 ? DNear : DFar) + Slack);
						if (VHi - VLo <= 1e-4) continue;
					}
				}
				Stack.Add({ Next, Lo, Hi, It.Depth + 1, bFull, VLo, VHi });
			}
		}
	}
}
#endif // WITH_OUTLAWS_TESTS

void AOutlawsLevelActor::NearFlood(const FOutlawsLevel& L, int32 From, const FVector2D& Body, TSet<int32>& Out) const
{
	// what the body can touch: its sector, and through openings within 2.5 m of it the sectors beyond (three deep), the
	// one above or below too
	if (!L.Sectors.IsValidIndex(From)) return;
	const double Reach = 250.0 / FOutlawsGeometry::UnitsToCm;
	TArray<TPair<int32, int32>> Queue;
	Queue.Emplace(From, 0);
	Out.Add(From);
	for (int32 q = 0; q < Queue.Num(); ++q)
	{
		const FOutlawsSector& S = L.Sectors[Queue[q].Key];
		const int32 Depth = Queue[q].Value;
		if (L.Sectors.IsValidIndex(S.VAdjoin) && !Out.Contains(S.VAdjoin)) { Out.Add(S.VAdjoin); if (Depth < 3) Queue.Emplace(S.VAdjoin, Depth + 1); }
		if (Depth >= 3) continue;
		for (const FOutlawsWall& W : S.Walls)
		{
			if ((W.Adjoin < 0 && W.DAdjoin < 0) || !S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) continue;
			if (FVector2D::Distance(Body, FMath::ClosestPointOnSegment2D(Body, S.Verts[W.V1], S.Verts[W.V2])) > Reach) continue;
			for (const int32 Next : { W.Adjoin, W.DAdjoin })
			{
				if (Next >= 0 && L.Sectors.IsValidIndex(Next) && !Out.Contains(Next)) { Out.Add(Next); Queue.Emplace(Next, Depth + 1); }
			}
		}
	}
}

void AOutlawsLevelActor::ApplyOverlapChoice(int32 BodySector, bool bLog)
{
	// sectors sharing space without an opening between them are separate places (the original draws and collides only
	// what its portals reach; user 2026-09-29: the mill's yard and the flume house inside it, the tunnel under ACCESS1T,
	// the hideout's room dug into its hillside).  Each of them is drawn when the eye's portal flood reaches it, and solid
	// when it is the body's sector or an opening within reach leads into it; the body's own always both
	if (OverlapParts.Num() == 0) return;
	const FOutlawsLevel& L = Inf ? Inf->Live : *LevelData;
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	const ACharacter* Ch = PC ? Cast<ACharacter>(PC->GetPawn()) : nullptr;
	if (!Ch) return;
	const double U = FOutlawsGeometry::UnitsToCm;
	const FVector PL = Ch->GetActorLocation();
	const double Feet = (PL.Z - Ch->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()) / U;
	const FVector EyeW = PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : PL;
	const FVector2D Eye = FOutlawsGeometry::ToOutlawsPlan(EyeW), Body = FOutlawsGeometry::ToOutlawsPlan(PL);
	const TSet<int32>& Seen = SeenFromEye;         // this tick's floods (UpdateVisibility)
	TSet<int32> Near;
	NearFlood(L, BodySector, Body, Near);
	TSet<int32> Hide, Unseen;
	for (const TPair<int32, TArray<FOverlapPart>>& Kv : OverlapParts)
	{
		if (Kv.Key == BodySector) continue;
		if (!Near.Contains(Kv.Key)) Hide.Add(Kv.Key);
		if (!Seen.Contains(Kv.Key) || OverlapLosers.Contains(Kv.Key)) Unseen.Add(Kv.Key);
	}
	ZoneForceOff(Hide, Unseen);         // (BUG-MILLCLIMB) the far side of a zone gate: neither drawn nor solid for this player
	if (Hide.Num() == OverlapIgnored.Num() && Hide.Includes(OverlapIgnored) && Unseen.Num() == OverlapHidden.Num() && Unseen.Includes(OverlapHidden)) return;
	UPrimitiveComponent* Body3 = Ch->GetCapsuleComponent();
	int32 Changed = 0;
	for (TPair<int32, TArray<FOverlapPart>>& Kv : OverlapParts)
	{
		const bool bIgnore = Hide.Contains(Kv.Key), bUnseen = Unseen.Contains(Kv.Key);
		if (bIgnore == OverlapIgnored.Contains(Kv.Key) && bUnseen == OverlapHidden.Contains(Kv.Key)) continue;
		++Changed;
		for (FOverlapPart& Part : Kv.Value)
		{
			UPrimitiveComponent* C = Part.Comp.Get();
			if (!C) continue;
			if (Part.bDrawn) C->SetVisibility(!bUnseen);
			if (Body3) Body3->IgnoreComponentWhenMoving(C, bIgnore);   // the player walks through it; bad guys still stand on it
		}
	}
	OverlapIgnored = MoveTemp(Hide);
	OverlapHidden = MoveTemp(Unseen);
	if (Changed > 0 && (bLog || OLFlags::Param(TEXT("OLShots"))))
	{
		FString Which;
		for (const int32 Hs : OverlapIgnored) Which += FString::Printf(TEXT(" %d%s"), Hs, OverlapHidden.Contains(Hs) ? TEXT("") : TEXT("(seen)"));
		for (const int32 Hs : OverlapHidden) if (!OverlapIgnored.Contains(Hs)) Which += FString::Printf(TEXT(" %d(unseen, solid)"), Hs);
		UE_LOG(LogOutlaws, Log, TEXT("OLRT overlaps from sector %d (feet %.2f): %d passed through:%s"), BodySector, Feet, OverlapIgnored.Num(), *Which);
	}
}

FString AOutlawsLevelActor::DebugOverlapReport(int32 Si) const
{
	// every sector sharing space with Si anywhere over its floor plan (a grid at a unit), with the slopes' true heights:
	// how deep the shared span gets, over how many samples, and whether the two adjoin (a test aid; user 2026-09-29)
	const FOutlawsLevel& L = Inf ? Inf->Live : *LevelData;
	if (!L.Sectors.IsValidIndex(Si)) return TEXT("(no such sector)");
	const FOutlawsSector& A = L.Sectors[Si];
	FBox2D Box(ForceInit);
	for (const FVector2D& V : A.Verts) Box += V;
	struct FHitInfo { double Max = 0.0; int32 N = 0; double FloorA = 0.0, CeilA = 0.0, FloorB = 0.0, CeilB = 0.0; };
	TMap<int32, FHitInfo> Hits;
	for (double y = Box.Min.Y + 0.5; y < Box.Max.Y; y += 1.0)
	{
		for (double x = Box.Min.X + 0.5; x < Box.Max.X; x += 1.0)
		{
			const FVector2D P(x, y);
			if (!L.ContainsXZ(A, P.X, P.Y)) continue;
			const double AF = L.SurfaceY(A, P, true), AC = L.SurfaceY(A, P, false);
			for (const FOutlawsSector& B : L.Sectors)
			{
				if (B.Index == Si || B.NoWalls() || B.Verts.Num() < 3 || !L.ContainsXZ(B, P.X, P.Y)) continue;
				const double BF = L.SurfaceY(B, P, true), BC = L.SurfaceY(B, P, false);
				const double Over = FMath::Min(AC, BC) - FMath::Max(AF, BF);
				if (Over <= 0.001) continue;
				FHitInfo& H = Hits.FindOrAdd(B.Index);
				if (Over > H.Max) { H.Max = Over; H.FloorA = AF; H.CeilA = AC; H.FloorB = BF; H.CeilB = BC; }
				++H.N;
			}
		}
	}
	auto Adjoined = [&](int32 a, int32 b)
	{
		for (const FOutlawsWall& W : L.Sectors[a].Walls) if (W.Adjoin == b || W.DAdjoin == b) return true;
		for (const FOutlawsWall& W : L.Sectors[b].Walls) if (W.Adjoin == a || W.DAdjoin == a) return true;
		return L.Sectors[a].VAdjoin == b || L.Sectors[b].VAdjoin == a;
	};
	FString Out;
	for (const TPair<int32, FHitInfo>& Kv : Hits)
	{
		const FHitInfo& H = Kv.Value;
		Out += FString::Printf(TEXT(" [%d %s: up to %.2f u over %d samples (%.2f..%.2f vs %.2f..%.2f)%s%s]"), Kv.Key, *L.Sectors[Kv.Key].Name, H.Max, H.N,
			H.FloorA, H.CeilA, H.FloorB, H.CeilB, Adjoined(Si, Kv.Key) ? TEXT(" adjoined") : TEXT(""),
			OverlapPairs.Contains(TPair<int32, int32>(FMath::Min(Si, Kv.Key), FMath::Max(Si, Kv.Key))) ? TEXT(" PAIRED") : TEXT(""));
	}
	return Out.IsEmpty() ? TEXT(" none") : Out;
}

void AOutlawsLevelActor::IgnoreHiddenOverlaps(FCollisionQueryParams& Q) const
{
	for (const int32 Si : OverlapHidden)
	{
		if (const TArray<FOverlapPart>* Parts = OverlapParts.Find(Si))
		{
			for (const FOverlapPart& Part : *Parts) if (UPrimitiveComponent* C = Part.Comp.Get()) Q.AddIgnoredComponent(C);
		}
	}
}

void AOutlawsLevelActor::IgnorePassedOverlaps(FCollisionQueryParams& Q) const
{
	for (const int32 Si : OverlapIgnored)
	{
		if (const TArray<FOverlapPart>* Parts = OverlapParts.Find(Si))
		{
			for (const FOverlapPart& Part : *Parts) if (UPrimitiveComponent* C = Part.Comp.Get()) Q.AddIgnoredComponent(C);
		}
	}
}

int32 AOutlawsLevelActor::CullScan(const FVector2D& Pt, double Y, int32 N, const TArray<int32>* Only) const
{
	// the highest floor at or under Y among the sectors holding Pt, else the lowest, as SectorAt picks; first in index
	// order on a tie (Only keeps index order, so a list scan and the full scan pick alike)
	const FOutlawsLevel& L = *LevelData;
	int32 Best = -1, Low = -1;
	double BestY = -1e30, LowY = 1e30;
	const int32 Count = Only ? Only->Num() : N;
	for (int32 k = 0; k < Count; ++k)
	{
		const int32 i = Only ? (*Only)[k] : k;
		if (!CullBoxes[i].IsInside(Pt)) continue;
		const FOutlawsSector& S = L.Sectors[i];
		if (!L.ContainsXZ(S, Pt.X, Pt.Y)) continue;
		const double F = L.SurfaceY(S, Pt, true);
		if (F <= Y + 0.01 && F > BestY) { Best = i; BestY = F; }
		if (F < LowY) { Low = i; LowY = F; }
	}
	return Best >= 0 ? Best : Low;
}

void AOutlawsLevelActor::BuildCullNear()
{
	// boxes swept along x: a pair meets when their spans overlap on both axes (edges touching count)
	const int32 N = CullBoxes.Num();
	CullNear.Reset();
	CullNear.SetNum(N);
	++CullGen;
	TArray<int32> ByMinX;
	ByMinX.Reserve(N);
	for (int32 i = 0; i < N; ++i) ByMinX.Add(i);
	ByMinX.Sort([this](int32 A, int32 B) { return CullBoxes[A].Min.X < CullBoxes[B].Min.X; });
	for (int32 a = 0; a < N; ++a)
	{
		const FBox2D& A = CullBoxes[ByMinX[a]];
		for (int32 b = a; b < N; ++b)
		{
			const FBox2D& B = CullBoxes[ByMinX[b]];
			if (B.Min.X > A.Max.X) break;
			if (B.Min.Y > A.Max.Y || A.Min.Y > B.Max.Y || B.Max.X < A.Min.X) continue;
			CullNear[ByMinX[a]].Add(ByMinX[b]);
			if (b != a) CullNear[ByMinX[b]].Add(ByMinX[a]);
		}
	}
	for (TArray<int32>& Near : CullNear) Near.Sort();
}

#if WITH_OUTLAWS_TESTS
namespace
{
	uint64 GCullCalls[4] = { 0, 0, 0, 0 };         // (-OLSectorCacheCheck) by the hint's sector, by an adjoin, the full search; wrong
}
#endif

int32 AOutlawsLevelActor::CullSectorOf(const FVector& P, FOutlawsSectorHint* Hint) const
{
	// the sector a sprite stands in, on the level as authored (its plan is fixed: a box test first, then the polygon)
	if (!LevelData) return -1;
	const FOutlawsLevel& L = *LevelData;
	const FVector Ol = FOutlawsGeometry::ToOutlaws(P, 0.3);
	const FVector2D Pt(Ol.X, Ol.Z);
	const int32 N = FMath::Min(L.Sectors.Num(), CullBoxes.Num());
	// the hint's sector, else one of its adjoins, whose box holds Pt: every box that holds Pt meets that box, so its
	// CullNear list holds every sector the full search would test (the same pick, to the bit); else the full search
	const TArray<int32>* Only = nullptr;
	[[maybe_unused]] int32 Via = 2;
	if (Hint && Hint->Gen == CullGen && CullNear.Num() == N && Hint->Sector >= 0 && Hint->Sector < N)
	{
		const int32 C = Hint->Sector;
		if (CullBoxes[C].IsInside(Pt)) { Only = &CullNear[C]; Via = 0; }
		else
		{
			for (const FOutlawsWall& W : L.Sectors[C].Walls)
			{
				const int32 A = W.Adjoin >= 0 && W.Adjoin < N && CullBoxes[W.Adjoin].IsInside(Pt) ? W.Adjoin
					: W.DAdjoin >= 0 && W.DAdjoin < N && CullBoxes[W.DAdjoin].IsInside(Pt) ? W.DAdjoin : -1;
				if (A >= 0) { Only = &CullNear[A]; Via = 1; break; }
			}
		}
	}
	const int32 Found = CullScan(Pt, Ol.Y, N, Only);
	if (Hint && Found >= 0) { Hint->Sector = Found; Hint->Gen = CullGen; }
#if WITH_OUTLAWS_TESTS
	if (Hint)
	{
		static const bool bCheck = OLFlags::Param(TEXT("OLSectorCacheCheck"));
		++GCullCalls[Via];
		if (bCheck)
		{
			const int32 Full = Only ? CullScan(Pt, Ol.Y, N, nullptr) : Found;
			if (Full != Found)
			{
				++GCullCalls[3];
				UE_LOG(LogOutlaws, Warning, TEXT("OLSECTORCACHE wrong: %d by the hint, %d by the full search, at %s"), Found, Full, *P.ToCompactString());
			}
			const uint64 Calls = GCullCalls[0] + GCullCalls[1] + GCullCalls[2];
			if (Calls % 5000 == 0)
			{
				UE_LOG(LogOutlaws, Log, TEXT("OLSECTORCACHE %llu lookups: %llu by the last sector, %llu by an adjoin, %llu full; %llu wrong"),
					Calls, GCullCalls[0], GCullCalls[1], GCullCalls[2], GCullCalls[3]);
			}
		}
	}
#endif
	return Found;
}

void AOutlawsLevelActor::UpdateVisibility()
{
	OL_PERF_SCOPE("Level: visibility");
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	const bool bOn = CVarOLCull.GetValueOnGameThread() != 0 && Inf && LevelData && PC && PC->PlayerCameraManager;      // (the main menu's level too)
	const FOutlawsLevel* LP = Inf ? &Inf->Live : nullptr;
	const int32 NumS = LP ? LP->Sectors.Num() : 0;
	FVector EyeW = FVector::ZeroVector;
	int32 From = PlayerSector;
	++VisTicks;
	if (LP && PC && PC->PlayerCameraManager)
	{
		const double U = FOutlawsGeometry::UnitsToCm;
		EyeW = PC->PlayerCameraManager->GetCameraLocation();
		const FVector2D Eye = FOutlawsGeometry::ToOutlawsPlan(EyeW);
		const double EyeZ = EyeW.Z / U;
		// (V1, the original's rule: the camera draws from the player object's sector, renderer_computeCameraTransform(
		// s_playerEye->sector); the eye's height is only a transform) one flood, from the body's tracked sector; the eye's
		// own position sets the windows.  A sector that no opening reaches is not drawn, whatever space it shares
		if (PlayerSector < 0)
		{
			const FOutlawsSector* EyeS = LP->SectorAt(Eye.X, EyeZ, Eye.Y);      // (no body yet: the level's first tick)
			if (EyeS) From = EyeS->Index;
		}
		// BUG-DOORWAYGEO (user 28 Oct, snapshots 3-5): under a raised door slab (the hideout's upstairs doors, sectors 31 and
		// 32: thin door sectors at 23..25, holes in the plans of their rooms 376 and 341) the eye stands in no sector at all:
		// the room's plan has a hole there and the slab is over it.  The body's sector went to the room below whose plan
		// holds the spot (325, 190), and the flood from there drew that room and left the one the player stands in unseen.
		// With no sector round the eye and one right over it, the flood starts from that one's neighbour round the eye's
		// height (the room), and the slab is drawn too.  -OLOldDoorwayGeo: from the body's sector, as before
		static const bool bOldDoorway = OLFlags::Param(TEXT("OLOldDoorwayGeo"));
		int32 Slab = -1;
		if (!bOldDoorway && LP->Sectors.IsValidIndex(From))
		{
			const FOutlawsLevel& Lv = *LP;
			auto Holds = [&Lv, &Eye, EyeZ](const FOutlawsSector& S)
			{
				return S.Verts.Num() >= 3 && EyeZ >= Lv.SurfaceY(S, Eye, true) - 0.05 && EyeZ <= Lv.SurfaceY(S, Eye, false) + 0.05 && Lv.ContainsXZ(S, Eye.X, Eye.Y);
			};
			if (!Holds(Lv.Sectors[From]))
			{
				int32 Room = -1;
				bool bInOne = false;
				for (int32 i = 0; i < NumS && !bInOne; ++i)
				{
					const FOutlawsSector& S = Lv.Sectors[i];
					if ((CullBoxes.IsValidIndex(i) && !CullBoxes[i].IsInside(Eye)) || S.Verts.Num() < 3 || !Lv.ContainsXZ(S, Eye.X, Eye.Y)) continue;
					if (EyeZ >= Lv.SurfaceY(S, Eye, true) - 0.05 && EyeZ <= Lv.SurfaceY(S, Eye, false) + 0.05) { bInOne = true; break; }
					if (EyeZ >= Lv.SurfaceY(S, Eye, true) || Room >= 0) continue;
					for (const FOutlawsWall& W : S.Walls)
					{
						const FOutlawsSector* R = Lv.Sector(W.Adjoin);
						if (R && EyeZ >= R->Floor.Y - 0.05 && EyeZ <= R->Ceil.Y + 0.05) { Room = R->Index; Slab = i; break; }
					}
				}
				if (bInOne || Room < 0) Slab = -1;
				else From = Room;
			}
		}
		DebugFloodMode = 0;
		// (QSPEED-3) the flood again only when something it read may have changed (FloodStillHolds); else the last one
		// stands, the same sectors to the one
		const bool bKeep = PlayerSector >= 0 && FloodStillHolds(*LP, From, Eye, EyeZ);
		// (the bounds cost half as much again as the flood: worked out only while the eye has all but stopped, under 2 mm
		// since the last tick, as a moving eye leaves any radius they give at once; and after a flood whose bounds kept it
		// for under two ticks, not again for 1, 2, 4 .. 64 floods, until one keeps)
		const bool bStill = FloodCache.bHavePrev && FMath::Max(FVector2D::Distance(Eye, FloodCache.PrevEye), FMath::Abs(EyeZ - FloodCache.PrevEyeZ)) < 0.2 / U;
		FloodCache.PrevEye = Eye;
		FloodCache.PrevEyeZ = EyeZ;
		FloodCache.bHavePrev = true;
		if (bKeep) ++FloodCache.KeptSince;
		if (!bKeep)
		{
			OL_PERF_SCOPE("Level: visibility flood");
			SeenFromEye.Reset();
			SeenDepth.Reset();
			if (FloodCache.bLastTracked)
			{
				FloodCache.Backoff = FloodCache.KeptSince >= 2 ? 0 : FMath::Clamp(FloodCache.Backoff * 2, 1, 64);
				FloodCache.WaitLeft = FloodCache.Backoff;
			}
			const bool bTry = bStill && FloodCache.WaitLeft <= 0;
			if (!bTry && FloodCache.WaitLeft > 0) --FloodCache.WaitLeft;
			const bool bCache = CVarOLVisCache.GetValueOnGameThread() != 0 && PlayerSector >= 0 && bTry;
			FloodCache.bLastTracked = bCache;
			FloodCache.KeptSince = 0;
			PortalFlood(*LP, From, Eye, EyeZ, SeenFromEye, &SeenDepth, bCache ? &FloodCache.Bounds : nullptr);
			if (Slab >= 0) SeenFromEye.Add(Slab);           // (BUG-DOORWAYGEO) the slab over the eye
			++VisFloods;
			VisPops += DebugFloodPops;
			if (bCache)
			{
				++VisTracked;
				const double Rcm = FloodCache.Bounds.SafeR * U;
				++VisRadii[Rcm < 0.1 ? 0 : Rcm < 1.0 ? 1 : Rcm < 10.0 ? 2 : 3];
				++VisSites[FMath::Clamp(FloodCache.Bounds.Site, 0, 31)];
			}
			FloodCache.bValid = bCache && FloodCache.Bounds.SafeR > 0.0;
			FloodCache.Level = LP;
			FloodCache.NumS = NumS;
			FloodCache.From = From;
			FloodCache.Eye = Eye;
			FloodCache.EyeZ = EyeZ;
			FloodCache.Slack = CVarOLVisSlack.GetValueOnGameThread();
			FloodCache.Blown = BlownWalls();
			FloodCache.HeightGen = Inf->HeightGen;
		}
#if WITH_OUTLAWS_TESTS
		{
			// (-OLFloodBench) once, 10 s into the level: the flood 300 times as it was, as it is, as it is with its bounds
			static const bool bBench = OLFlags::Param(TEXT("OLFloodBench"));
			static bool bBenched = false;
			if (bBench && !bBenched && World && World->GetTimeSeconds() > 10.0)
			{
				bBenched = true;
				TSet<int32> BenchOut;
				TMap<int32, int32> BenchDepth;
				FFloodBounds BenchBounds;
				double BenchT[3] = { 0.0, 0.0, 0.0 };
				for (int32 r = 0; r < 3; ++r)
				{
					for (int32 Mode = 0; Mode < 3; ++Mode)
					{
						const double T0 = FPlatformTime::Seconds();
						for (int32 i = 0; i < 100; ++i)
						{
							BenchOut.Reset();
							BenchDepth.Reset();
							if (Mode == 0) PortalFloodRef(*LP, From, Eye, EyeZ, BenchOut, &BenchDepth);
							else PortalFlood(*LP, From, Eye, EyeZ, BenchOut, &BenchDepth, Mode == 2 ? &BenchBounds : nullptr);
						}
						BenchT[Mode] += FPlatformTime::Seconds() - T0;
					}
				}
				UE_LOG(LogOutlaws, Display, TEXT("OLFLOODBENCH %s: a flood as it was %.3f ms, as it is %.3f ms, with bounds %.3f ms (%d sectors, %d arrivals)"), *LevelName,
					BenchT[0] * 1000.0 / 300.0, BenchT[1] * 1000.0 / 300.0, BenchT[2] * 1000.0 / 300.0, BenchOut.Num(), DebugFloodPops);
			}
		}
		{
			// (-OLVisCacheCheck) the proof: every tick, kept or flooded anew, the flood as it was before QSPEED-3 (every
			// tick, no lists kept: PortalFloodRef) beside what this tick uses, sector for sector and depth for depth
			static const bool bCheck = OLFlags::Param(TEXT("OLVisCacheCheck"));
			if (bCheck)
			{
				TSet<int32> Fresh;
				TMap<int32, int32> FreshDepth;
				const int32 Pops = DebugFloodPops;
				PortalFloodRef(*LP, From, Eye, EyeZ, Fresh, &FreshDepth);
				DebugFloodPops = Pops;
				++VisChecked;
				bool bSame = Fresh.Num() == SeenFromEye.Num() && FreshDepth.Num() == SeenDepth.Num();
				for (const int32 Si : Fresh) if (bSame && !SeenFromEye.Contains(Si)) bSame = false;
				for (const TPair<int32, int32>& Kv : FreshDepth) { const int32* D = SeenDepth.Find(Kv.Key); if (bSame && (!D || *D != Kv.Value)) bSame = false; }
				if (!bSame)
				{
					++VisDiffers;
					if (VisDiffers <= 20)
					{
						UE_LOG(LogOutlaws, Warning, TEXT("OLVISCACHE differs (%s, %s): %d sectors, the old flood %d; eye moved %.4f of a safe %.4f, from sector %d"), *LevelName, bKeep ? TEXT("kept") : TEXT("flooded"),
							SeenFromEye.Num(), Fresh.Num(), FMath::Max(FVector2D::Distance(Eye, FloodCache.Eye), FMath::Abs(EyeZ - FloodCache.EyeZ)), FloodCache.Bounds.SafeR, From);
					}
				}
			}
		}
#endif
		OverlapLosers.Reset();
	}
	else
	{
		SeenFromEye.Reset();
		SeenDepth.Reset();
		FloodCache.bValid = false;
	}
	if (World && World->GetTimeSeconds() >= VisCacheNextLog)
	{
		// (QSPEED-3) how often the flood was kept, once a minute, as the OLPERF summary
		VisCacheNextLog = World->GetTimeSeconds() + (VisCacheNextLog <= 0.0 ? 10.0 : 60.0);
		UE_LOG(LogOutlaws, Log, TEXT("OLVISCACHE %s"), *DebugVisCacheStats());
	}
	const bool bValid = bOn && SeenFromEye.Num() > 0;
	if (!bValid)
	{
		AOutlawsSpriteActor::SetCulling(false, nullptr, FVector::ZeroVector, nullptr);
		bCullFnSent = false;
		for (int32 g = 0; g < GroupCulled.Num(); ++g)
		{
			if (!GroupCulled[g] || !Groups.IsValidIndex(g)) continue;
			GroupCulled[g] = false;
			ShowMoverGroup(g);
		}
		ShowMoverBatches();
		return;
	}
	if (CullBoxes.Num() != LevelData->Sectors.Num())
	{
		CullBoxes.SetNum(LevelData->Sectors.Num());
		for (int32 i = 0; i < LevelData->Sectors.Num(); ++i)
		{
			FBox2D B(ForceInit);
			for (const FVector2D& V : LevelData->Sectors[i].Verts) B += V;
			CullBoxes[i] = B.bIsValid ? B.ExpandBy(0.05) : B;
		}
		BuildCullNear();
	}
	// (V1) the flooded sectors and nothing more: no adjoin beyond them (a shut door's far room stays out), held 1 s
	// after the flood leaves one (pops only: open-yard sectors at the view's edge flipped in and out as the view swept;
	// BUG-POPIN, build 22: a still player's head sway flipped grazing sectors every half second, 0.4 s was too short)
	VisibleSectors.Init(false, NumS);
	if (SeenAt.Num() != NumS) SeenAt.Init(-1.0, NumS);
	const double VisNow = World ? World->GetTimeSeconds() : 0.0;
	for (const int32 Si : SeenFromEye) if (Si >= 0 && Si < NumS) SeenAt[Si] = VisNow;
	for (int32 s = 0; s < NumS; ++s) if (SeenAt[s] >= 0.0 && VisNow - SeenAt[s] <= CVarOLVisHold.GetValueOnGameThread()) VisibleSectors[s] = true;
	// (the sprites' sector: tracked from where each was, as the player's; handed over once a level)
	AOutlawsSpriteActor::SetCulling(true, &VisibleSectors, EyeW, bCullFnSent ? TFunction<int32(int32, const FVector&, const FVector&)>()
		: TFunction<int32(int32, const FVector&, const FVector&)>([this](int32 Was, const FVector& From, const FVector& P) { return TrackThing(Was, From, P); }));
	bCullFnSent = true;
	// doors and lifts: shown while any of their sectors may be seen
	if (GroupCullSectors.Num() != Groups.Num())
	{
		GroupCullSectors.SetNum(Groups.Num());
		GroupCulled.Init(false, Groups.Num());
		for (int32 g = 0; g < Groups.Num(); ++g)
		{
			TSet<int32> Set;
			const FMoverGroup& G = Groups[g];
			if (Inf->Elevators.IsValidIndex(G.Elevator)) for (const int32 Es : Inf->Elevators[G.Elevator].Sectors) Set.Add(Es);
			for (const FOutlawsBuildFilter* F : { &G.GroupFilter, &G.FloorFilter, &G.CeilFilter })
			{
				for (const int32 Fs : F->OnlyFloor) Set.Add(Fs);
				for (const int32 Fs : F->OnlyCeil) Set.Add(Fs);
				for (const uint64 K : F->OnlyWalls) Set.Add(int32(K >> 32));
			}
			// (BUG-POPIN) a shut door is what the room before it shows, though the flood never enters its sector (no
			// height to pass): shown too while a sector next to one of its own may be seen (nothing beyond is entered)
			TSet<int32> Next;
			for (const int32 Gs : Set)
			{
				if (!LevelData->Sectors.IsValidIndex(Gs)) continue;
				const FOutlawsSector& GS = LevelData->Sectors[Gs];
				if (GS.VAdjoin >= 0) Next.Add(GS.VAdjoin);
				for (const FOutlawsWall& W : GS.Walls) { if (W.Adjoin >= 0) Next.Add(W.Adjoin); if (W.DAdjoin >= 0) Next.Add(W.DAdjoin); }
			}
			Set.Append(Next);
			GroupCullSectors[g] = Set.Array();
		}
	}
	int32 GroupsHidden = 0;
	for (int32 g = 0; g < Groups.Num(); ++g)
	{
		bool bSee = GroupCullSectors[g].Num() == 0;
		for (const int32 Gs : GroupCullSectors[g]) if (Gs >= 0 && Gs < NumS && VisibleSectors[Gs]) { bSee = true; break; }
		if (bSee == !GroupCulled[g]) { GroupsHidden += GroupCulled[g] ? 1 : 0; continue; }
		GroupCulled[g] = !bSee;
		GroupsHidden += bSee ? 0 : 1;
		if (ChurnGroup.Num() != Groups.Num()) ChurnGroup.Init(0, Groups.Num());
		++ChurnGroup[g];
		ShowMoverGroup(g);
	}
	ShowMoverBatches();
	// (BUG-POPIN) the churn: sectors whose may-be-seen flag flipped, counted; every 2 s the ones that flipped 3+ times
	{
		if (ChurnSector.Num() != NumS || ChurnPrevVisible.Num() != NumS)
		{
			ChurnSector.Init(0, NumS);
			ChurnPrevVisible = VisibleSectors;
		}
		else
		{
			for (int32 s = 0; s < NumS; ++s)
			{
				if (VisibleSectors[s] != ChurnPrevVisible[s]) { ++ChurnSector[s]; ++VisFlipsTotal; ChurnPrevVisible[s] = VisibleSectors[s]; }
			}
		}
		const double Tn = World ? World->GetTimeSeconds() : 0.0;
		if (Tn >= ChurnNextLog)
		{
			ChurnNextLog = Tn + 2.0;
			FString Flips;
			int32 NFlip = 0;
			for (int32 s = 0; s < ChurnSector.Num(); ++s)
			{
				if (ChurnSector[s] >= 3 && LP)
				{
					if (NFlip++ < 12) Flips += FString::Printf(TEXT(" s%d%s%s x%d"), s, LP->Sectors[s].Name.IsEmpty() ? TEXT("") : TEXT(":"), *LP->Sectors[s].Name, ChurnSector[s]);
				}
				ChurnSector[s] = 0;
			}
			for (int32 g = 0; g < ChurnGroup.Num(); ++g)
			{
				if (ChurnGroup[g] >= 3 && Inf && Inf->Elevators.IsValidIndex(Groups[g].Elevator))
				{
					if (NFlip++ < 16) Flips += FString::Printf(TEXT(" door/lift %s x%d"), *Inf->Elevators[Groups[g].Elevator].Name, ChurnGroup[g]);
				}
				ChurnGroup[g] = 0;
			}
			if (NFlip > 0)
			{
				const FVector Pp = PC && PC->GetPawn() ? PC->GetPawn()->GetActorLocation() : FVector::ZeroVector;
				UE_LOG(LogOutlaws, Log, TEXT("OLRT vis churn (%s): %d flipping in 2 s; eye sector %d, player sector %d at %s:%s"), *LevelName, NFlip, From, PlayerSector, *Pp.ToString(), *Flips);
			}
		}
	}
	if (World && NextCullLog <= 0.0) NextCullLog = World->GetTimeSeconds() + 3.0;     // the first report once the sprites have settled
	if (World && World->GetTimeSeconds() >= NextCullLog)
	{
		NextCullLog = World->GetTimeSeconds() + 10.0;
		int32 Vis = 0;
		for (TConstSetBitIterator<> It(VisibleSectors); It; ++It) ++Vis;
		UE_LOG(LogOutlaws, Verbose, TEXT("OLRT culling: eye in sector %d, %d of %d sectors may be seen, %d sprites and %d of %d doors/lifts hidden; %d bad guys far and frozen, %d far sounds skipped"),
			From, Vis, NumS, AOutlawsSpriteActor::NumCulled, GroupsHidden, Groups.Num(), FarFrozen, FarSoundsSkipped);
	}
}

int32 AOutlawsLevelActor::DebugNumSectors() const
{
	return LevelData ? LevelData->Sectors.Num() : 0;
}

int32 AOutlawsLevelActor::BlownWalls() const
{
	int32 N = 0;
	for (const FExplodeWall& X : ExplodeWalls) N += X.bBlown ? 1 : 0;
	return N;
}

bool AOutlawsLevelActor::FloodStillHolds(const FOutlawsLevel& L, int32 From, const FVector2D& Eye, double EyeZ)
{
	FFloodCache& C = FloodCache;
	if (!C.bValid || CVarOLVisCache.GetValueOnGameThread() == 0 || !Inf) { ++VisWhyNot[0]; return false; }
	if (C.Level != &L || C.NumS != L.Sectors.Num() || C.From != From || C.Slack != CVarOLVisSlack.GetValueOnGameThread()) { ++VisWhyNot[1]; return false; }
	if (FVector2D::Distance(Eye, C.Eye) >= C.Bounds.SafeR || FMath::Abs(EyeZ - C.EyeZ) >= C.Bounds.SafeR) { ++VisWhyNot[2]; return false; }
	if (C.Blown != BlownWalls()) { ++VisWhyNot[3]; return false; }
	if (Inf->HeightGen != C.HeightGen)
	{
		// something moved: only a height the flood read sends it again (a lift across the level does not)
		for (TConstSetBitIterator<> It(C.Bounds.Read); It; ++It)
		{
			if (Inf->SectorHeightGen.IsValidIndex(It.GetIndex()) && Inf->SectorHeightGen[It.GetIndex()] > C.HeightGen) { ++VisWhyNot[4]; return false; }
		}
		C.HeightGen = Inf->HeightGen;
	}
	return true;
}

FString AOutlawsLevelActor::DebugVisCacheStats() const
{
	FString Sites;
	for (int32 i = 0; i < 32; ++i) if (VisSites[i] > 0) Sites += FString::Printf(TEXT(" s%d=%lld"), i, VisSites[i]);
	return FString::Printf(TEXT("%s: %lld ticks, %lld floods (%.1f%% kept; %lld floods with bounds, %.0f arrivals a flood, %d sectors), safe radius now %.3f cm (floods under 1 mm %lld, 1 cm %lld, 10 cm %lld, more %lld); flooded again: none kept %lld, sector %lld, eye moved %lld, wall blown %lld, height moved %lld; checked %lld, %lld differ"),
		*LevelName, VisTicks, VisFloods, VisTicks > 0 ? 100.0 * double(VisTicks - VisFloods) / double(VisTicks) : 0.0, VisTracked, VisFloods > 0 ? double(VisPops) / double(VisFloods) : 0.0, SeenFromEye.Num(), FloodCache.Bounds.SafeR * FOutlawsGeometry::UnitsToCm,
		VisRadii[0], VisRadii[1], VisRadii[2], VisRadii[3], VisWhyNot[0], VisWhyNot[1], VisWhyNot[2], VisWhyNot[3], VisWhyNot[4], VisChecked, VisDiffers) + Sites;
}

int32 AOutlawsLevelActor::TrackThing(int32 Was, const FVector& From, const FVector& P) const
{
	const FOutlawsLevel* LP = Inf ? &Inf->Live : nullptr;
	if (!LP) return -1;
	const FVector Ol = FOutlawsGeometry::ToOutlaws(P, 0.2);
	int32 S = Was >= 0 ? LP->TrackMove(Was, FOutlawsGeometry::ToOutlawsPlan(From), FOutlawsGeometry::ToOutlawsPlan(P), Ol.Y) : -2;
	if (S == -2) S = LP->Which3D(Ol);
	return S;
}

FString AOutlawsLevelActor::DebugGroupsHidden() const
{
	int32 N = 0;
	FString Names;
	for (int32 g = 0; g < GroupCulled.Num() && g < Groups.Num(); ++g)
	{
		if (!GroupCulled[g]) continue;
		if (N++ < 40) Names += FString::Printf(TEXT(" %s"), Inf && Inf->Elevators.IsValidIndex(Groups[g].Elevator) ? *Inf->Elevators[Groups[g].Elevator].Name : TEXT("?"));
	}
	return FString::Printf(TEXT("%d of %d doors/lifts hidden:%s"), N, Groups.Num(), *Names);
}

#if WITH_OUTLAWS_TESTS
FString AOutlawsLevelActor::DebugVisScan(const AActor* Viewer, int32& OutFalse, int32& OutPairs) const
{
	// (BUG-POPIN, -OLVisScan) eyes over the whole level (each sector's floor on a grid, at head height): every living bad
	// guy within 40 m the eye has a clear line to while the flood from the eye's sector leaves his sector out, and which
	// rule left it out (the flood again without the caps, the up-and-down window, the round one)
	OutFalse = OutPairs = 0;
	const FOutlawsLevel* LP = Inf ? &Inf->Live : nullptr;
	UWorld* World = GetWorld();
	if (!LP || !World) return TEXT("no level");
	const FOutlawsLevel& L = *LP;
	const double U = FOutlawsGeometry::UnitsToCm;
	struct FE { const AOutlawsEnemy* E; const AOutlawsSpriteActor* Spr; FVector To; int32 S; int32 True; };
	TArray<FE> Es;
	for (TActorIterator<AOutlawsEnemy> It(World); It; ++It)
	{
		const AOutlawsSpriteActor* Spr = It->GetSprite();
		if (!Spr || It->IsDead()) continue;
		const FVector P = Spr->GetActorLocation();
		Es.Add({ *It, Spr, It->GetActorLocation() + FVector(0.0, 0.0, 40.0), Spr->DebugCullSector(), L.Which3D(FOutlawsGeometry::ToOutlaws(P, 0.2)) });
	}
	TMap<FString, int32> Why;
	TSet<FString> Who;
	FString Ex;
	int32 Eyes = 0, NEx = 0;
	const double T0 = FPlatformTime::Seconds();
	double NextLog = 0.0;
	int32 Traces = 0, OldFalse = 0, MaxPops = 0;
	int64 SumPops = 0;
	TSet<FString> OldWho;
	for (const FOutlawsSector& S : L.Sectors)
	{
		if (S.Verts.Num() < 3 || S.NoWalls()) continue;
		FBox2D Box(ForceInit);
		for (const FVector2D& V : S.Verts) Box += V;
		const FVector2D Ext = Box.GetSize();
		const double Step = FMath::Max(150.0 / U, FMath::Max(Ext.X, Ext.Y) / 5.0);
		bool bNear = false;
		for (const FE& E : Es) if (Box.ExpandBy(4000.0 / U).IsInside(FOutlawsGeometry::ToOutlawsPlan(E.To))) { bNear = true; break; }
		if (!bNear || FPlatformTime::Seconds() - T0 > 200.0) continue;
		if (FPlatformTime::Seconds() > NextLog)
		{
			NextLog = FPlatformTime::Seconds() + 5.0;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT vis scan progress: sector %d of %d, %d eyes, %d traces, %d culled in clear view"), S.Index, L.Sectors.Num(), Eyes, Traces, OutFalse);
		}
		for (double y = Box.Min.Y + Step * 0.5; y < Box.Max.Y; y += Step)
		{
			for (double x = Box.Min.X + Step * 0.5; x < Box.Max.X; x += Step)
			{
				const FVector2D P(x, y);
				if (!L.ContainsXZ(S, P.X, P.Y)) continue;
				const double F = L.SurfaceY(S, P, true), C = L.SurfaceY(S, P, false);
				if (C - F < 120.0 / U || L.Which3D(P.X, F + 0.2, P.Y) != S.Index) continue;
				const double EyeY = F + FMath::Min(165.0 / U, (C - F) * 0.8);
				const FVector EyeW = FOutlawsGeometry::ToUE(P, EyeY);
				TSet<int32> Seen, SeenOld;
				DebugFloodMode = 8;
				PortalFlood(L, S.Index, P, EyeY, SeenOld);
				DebugFloodMode = 0;
				PortalFlood(L, S.Index, P, EyeY, Seen);
				MaxPops = FMath::Max(MaxPops, DebugFloodPops);
				SumPops += DebugFloodPops;
				++Eyes;
				for (const FE& E : Es)
				{
					const double D = FVector::Dist(EyeW, E.To);
					if (D > 4000.0 || D < 100.0 || (Seen.Contains(E.S) && SeenOld.Contains(E.S))) continue;
					FHitResult Hb;
					FCollisionQueryParams Qv(SCENE_QUERY_STAT(OutlawsVisScan), false, Viewer);
					Qv.AddIgnoredActor(E.E);
					Qv.AddIgnoredActor(E.Spr);
					++Traces;
					if (World->LineTraceSingleByChannel(Hb, EyeW, E.To, ECC_Visibility, Qv)) continue;
					if (!SeenOld.Contains(E.S)) { ++OldFalse; OldWho.Add(FString::Printf(TEXT("%s@s%d"), *E.E->GetOutlawsName(), E.S)); }
					if (Seen.Contains(E.S)) continue;
					++OutFalse;
					FString C3;
					if (OutFalse > 300 && Who.Contains(FString::Printf(TEXT("%s@s%d"), *E.E->GetOutlawsName(), E.S))) { ++Why.FindOrAdd(TEXT("(unsorted)")); continue; }
					if (E.True != E.S) C3 += FString::Printf(TEXT("sector(true s%d %s)"), E.True, Seen.Contains(E.True) ? TEXT("seen") : TEXT("unseen"));
					for (const int32 Mode : { 2, 4, 6, 1, 3, 5, 7 })
					{
						TSet<int32> S2;
						DebugFloodMode = Mode;
						PortalFlood(L, S.Index, P, EyeY, S2);
						if (S2.Contains(E.S)) { C3 += FString::Printf(TEXT("%s%s%s%s"), C3.IsEmpty() ? TEXT("") : TEXT("+"), (Mode & 1) ? TEXT("cap") : TEXT(""), (Mode & 2) ? TEXT("V") : TEXT(""), (Mode & 4) ? TEXT("ang") : TEXT("")); break; }
						if (Mode == 7) C3 += TEXT("+unreached");
					}
					DebugFloodMode = 0;
					++Why.FindOrAdd(C3);
					const FString Name = FString::Printf(TEXT("%s@s%d"), *E.E->GetOutlawsName(), E.S);
					if (!Who.Contains(Name) && NEx++ < 40)
						Ex += FString::Printf(TEXT("\n   eye s%d (%.0f,%.0f,%.0f) -> %s %.0fm [%s]"), S.Index, EyeW.X, EyeW.Y, EyeW.Z, *Name, D / 100.0, *C3);
					Who.Add(Name);
				}
			}
		}
	}
	OutPairs = Who.Num();
	FString W;
	for (const TPair<FString, int32>& Kv : Why) W += FString::Printf(TEXT(" %s=%d"), *Kv.Key, Kv.Value);
	return FString::Printf(TEXT("%d eyes, %d bad guys, %d traces; build-22 rule: %d culled in clear view (%d bad guys); the flood's arrivals passed on: %d at most, %.0f a flood; still culled, seen when lifted:%s; first of each:%s"),
		Eyes, Es.Num(), Traces, OldFalse, OldWho.Num(), MaxPops, Eyes > 0 ? double(SumPops) / Eyes : 0.0, *W, *Ex);
}

FString AOutlawsLevelActor::DebugEyeScan(const AActor* Viewer, const FVector& EyeW, int32& OutRays, int32& OutMissed, TArray<int32>* OutMissedSectors) const
{
	// (BUG-DOORWAYGEO, -OLDoorwayGeoProbe) the level as the eye sees it against what the culling leaves drawn: rays all
	// round from the eye (8 degrees apart, 40 m) to the level's own faces; a face's sector (the side the eye is on) left
	// out of this tick's flood is geometry missing in clear view.  Each such sector: the rays, and which rule left it out
	OutRays = OutMissed = 0;
	const FOutlawsLevel* LP = Inf ? &Inf->Live : nullptr;
	UWorld* World = GetWorld();
	if (!LP || !World) return TEXT("no level");
	const FOutlawsLevel& L = *LP;
	const double U = FOutlawsGeometry::UnitsToCm;
	TMap<int32, int32> Missed;
	TMap<int32, FString> MissedOn;
	int32 Unplaced = 0;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsEyeScan), true, Viewer);
	for (int32 p = -84; p <= 84; p += 8)
	{
		for (int32 y = 0; y < 360; y += 8)
		{
			const FVector Dir = FRotator(p, y, 0.0).Vector();
			FHitResult H;
			if (!World->LineTraceSingleByChannel(H, EyeW, EyeW + Dir * 4000.0, ECC_Visibility, Q) || H.GetActor() != this) continue;
			++OutRays;
			// the face's sector: one whose plan holds the point just off it and whose floor and ceiling hold its height
			// (Which3D falls back to a sector by plan alone, the room under a stacked one); none, the ray is not counted
			const FVector Po = FOutlawsGeometry::ToOutlaws(H.ImpactPoint - Dir * 2.0);
			const FVector2D Pp(Po.X, Po.Z);
			int32 S = -1;
			for (int32 i = 0; i < L.Sectors.Num() && S < 0; ++i)
			{
				const FOutlawsSector& Si = L.Sectors[i];
				if ((CullBoxes.IsValidIndex(i) && !CullBoxes[i].IsInside(Pp)) || Si.Verts.Num() < 3 || !L.ContainsXZ(Si, Pp.X, Pp.Y)) continue;
				if (Po.Y >= L.SurfaceY(Si, Pp, true) - 0.1 && Po.Y <= L.SurfaceY(Si, Pp, false) + 0.1) S = i;
			}
			if (S < 0) { ++Unplaced; continue; }
			if (SeenFromEye.Contains(S)) continue;
			++OutMissed;
			if (++Missed.FindOrAdd(S) == 1) MissedOn.Add(S, FString::Printf(TEXT("%s at %s"), H.GetComponent() ? *H.GetComponent()->GetName() : TEXT("?"), *H.ImpactPoint.ToCompactString()));
		}
	}
	const FVector2D Eye = FOutlawsGeometry::ToOutlawsPlan(EyeW);
	const double EyeZ = EyeW.Z / U;
	const int32 EyeS = L.Which3D(FOutlawsGeometry::ToOutlaws(EyeW));
	FString Out = FString::Printf(TEXT("body sector %d, eye sector %d, %d sectors drawn; %d of %d rays on a face whose sector is not drawn (%d in no sector)"), PlayerSector, EyeS, SeenFromEye.Num(), OutMissed, OutRays,
		Unplaced);
	if (Missed.Num() == 0) return Out;
	auto Desc = [&L](int32 Si)
	{
		if (!L.Sectors.IsValidIndex(Si)) return FString(TEXT("?"));
		const FOutlawsSector& S = L.Sectors[Si];
		FString A;
		for (const FOutlawsWall& W : S.Walls) if (W.Adjoin >= 0) A += FString::Printf(TEXT(" %d"), W.Adjoin);
		return FString::Printf(TEXT("s%d %.2f..%.2f flags 0x%x adj%s"), Si, S.Floor.Y, S.Ceil.Y, S.Flags, *A);
	};
	Out += FString::Printf(TEXT("\n   eye %.2f, %.2f at %.2f; body %s; eye's %s"), Eye.X, Eye.Y, EyeZ, *Desc(PlayerSector), *Desc(EyeS));
	FString Plan, Drawn;
	for (const FOutlawsSector& S : L.Sectors) if (S.Verts.Num() >= 3 && L.ContainsXZ(S, Eye.X, Eye.Y)) Plan += TEXT("\n     under or over the eye: ") + Desc(S.Index);
	for (const int32 Si : SeenFromEye) Drawn += FString::Printf(TEXT(" %d"), Si);
	Out += Plan + TEXT("\n   drawn:") + Drawn;
	TSet<int32> FromEye;
	if (EyeS >= 0 && EyeS != PlayerSector) PortalFlood(L, EyeS, Eye, EyeZ, FromEye);
	Missed.ValueSort([](int32 A, int32 B) { return A > B; });
	for (const TPair<int32, int32>& Kv : Missed)
	{
		if (OutMissedSectors) OutMissedSectors->Add(Kv.Key);
		FString Why;
		if (FromEye.Contains(Kv.Key)) Why = TEXT("seen from the eye's own sector");
		for (const int32 Mode : { 2, 4, 6, 1, 3, 5, 7 })
		{
			TSet<int32> S2;
			DebugFloodMode = Mode;
			PortalFlood(L, PlayerSector, Eye, EyeZ, S2);
			if (S2.Contains(Kv.Key)) { Why += FString::Printf(TEXT("%sfrom the body's when lifted: %s%s%s"), Why.IsEmpty() ? TEXT("") : TEXT("; "), (Mode & 1) ? TEXT("cap") : TEXT(""), (Mode & 2) ? TEXT("V") : TEXT(""), (Mode & 4) ? TEXT("ang") : TEXT("")); break; }
			if (Mode == 7) Why += FString::Printf(TEXT("%sunreached from the body's"), Why.IsEmpty() ? TEXT("") : TEXT("; "));
		}
		DebugFloodMode = 0;
		Out += FString::Printf(TEXT("\n   s%d %d rays (first on %s) [%s] %s"), Kv.Key, Kv.Value, *MissedOn.FindRef(Kv.Key), *Why, *Desc(Kv.Key));
	}
	return Out;
}
#endif // WITH_OUTLAWS_TESTS

bool AOutlawsLevelActor::DebugCeilBoxSpot(FVector& OutStand, FVector& OutLip, FVector& OutIn, FString& OutWhat, double& OutTopZ, double& OutCeilZ, double& OutAboveZ) const
{
	if (!LevelData) return false;
	const FOutlawsLevel& Lv = *LevelData;
	const double U = FOutlawsGeometry::UnitsToCm;
	struct FPick { double Score = 1e9; } Best;
	bool bFound = false;
	for (const FOutlawsSector& S : Lv.Sectors)
	{
		for (const FOutlawsWall& W : S.Walls)
		{
			if (W.Adjoin < 0 || !Lv.Sectors.IsValidIndex(W.Adjoin) || (W.Flags1 & (OutlawsFlags::WallNoPass | OutlawsFlags::WallFence))) continue;
			const FOutlawsSector& B = Lv.Sectors[W.Adjoin];
			const double Rise = B.Floor.Y - S.Floor.Y, Room = B.Ceil.Y - B.Floor.Y;
			if (Rise < 1.5 || Rise > 6.0 || Room < 3.5 || Room > 8.0 || !S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) continue;
			const FVector2D A = S.Verts[W.V1], C = S.Verts[W.V2];
			if (FVector2D::Distance(A, C) < 2.0) continue;
			const FVector2D Mid = (A + C) * 0.5;
			FVector2D N = FVector2D(-(C - A).Y, (C - A).X).GetSafeNormal();
			if (!Lv.ContainsXZ(B, Mid.X + N.X * 0.5, Mid.Y + N.Y * 0.5)) N = -N;
			if (!Lv.ContainsXZ(B, Mid.X + N.X * 2.0, Mid.Y + N.Y * 2.0) || !Lv.ContainsXZ(S, Mid.X - N.X * 1.5, Mid.Y - N.Y * 1.5)) continue;
			// a room over the box: a sector holding the spot whose floor is the box's ceiling, or just over it
			const FOutlawsSector* Up = nullptr;
			for (const FOutlawsSector& T : Lv.Sectors)
			{
				if (&T == &B || T.Floor.Y < B.Ceil.Y - 0.01 || T.Floor.Y > B.Ceil.Y + 4.0) continue;
				if (Lv.ContainsXZ(T, Mid.X + N.X * 2.0, Mid.Y + N.Y * 2.0)) { Up = &T; break; }
			}
			if (!Up || Room >= Best.Score) continue;
			Best.Score = Room;              // (the lowest ceiling found)
			bFound = true;
			OutStand = FOutlawsGeometry::ToUE(Mid.X - N.X * 1.5, S.Floor.Y, Mid.Y - N.Y * 1.5);
			OutLip = FOutlawsGeometry::ToUE(Mid.X, B.Floor.Y, Mid.Y);
			OutIn = FOutlawsGeometry::ToUE(N.X, 0.0, N.Y).GetSafeNormal();
			OutTopZ = B.Floor.Y * U;
			OutCeilZ = B.Ceil.Y * U;
			OutAboveZ = Up->Floor.Y * U;
			OutWhat = FString::Printf(TEXT("box sector %d (%s), %.0f cm over sector %d, ceiling %.0f cm over its top, the room over it sector %d (%s) at %.0f cm"), B.Index, *B.Name, Rise * U,
				S.Index, Room * U, Up->Index, *Up->Name, Up->Floor.Y * U);
		}
	}
	return bFound;
}

int32 AOutlawsLevelActor::SectorHoldingPoint(const FVector& P, double Slack) const
{
	const FOutlawsLevel* Lv = Inf ? &Inf->Live : LevelData.Get();
	if (!Lv) return -2;
	const double U = FOutlawsGeometry::UnitsToCm;
	const FVector Ol = FOutlawsGeometry::ToOutlaws(P);
	const double X = Ol.X, Y = Ol.Y, Z = Ol.Z, S0 = Slack / U;
	const FVector2D Pt(X, Z);
	const TBitArray<>* Skip = PlayerSkip();          // (BUG-MILLCLIMB: never a zone gate's far side, off for the player)
	// (BUG-JUMPPICK) of the sectors holding the point, the walled one whose floor is nearest it, as the tracking picks: the
	// unstuck's last head sector (whose middle it may warp to) on a catwalk was the cavern under it, the first in the list
	static const bool bOldJumppick = OLFlags::Param(TEXT("OLOldJumppick"));
	int32 Best = -1;
	double BestScore = TNumericLimits<double>::Max();
	for (const FOutlawsSector& S : Lv->Sectors)
	{
		if (S.Verts.Num() < 3 || FOutlawsLevel::Skipped(Skip, S.Index) || !Lv->ContainsXZ(S, X, Z)) continue;
		double Floor = Lv->SurfaceY(S, Pt, true);
		if (Y < Floor - S0 && (S.Flags & OutlawsFlags::SectorDoor) && Inf && Inf->Live.Sectors.IsValidIndex(S.Index))
		{
			// (BUG-WALLSTUCK) an automatic door standing open: its thin sector keeps the shut leaf's top as its floor, so the
			// doorway under it was in no sector and a player stood in it (a hand on the door, the head through) was "between
			// sectors" and sent back by the unstuck (the hideout's doors 31 and 33, the user's two snapshots).  Open, the
			// doorway is held down to the lowest floor beside it
			bool bOpen = false;
			if (const TArray<int32>* Es = Inf->ElevatorsBySector.Find(S.Index))
			{
				for (int32 e : *Es) bOpen |= Inf->Elevators[e].Stop != 0 || Inf->Elevators[e].Target >= 0;
			}
			if (bOpen)
			{
				for (const FOutlawsWall& W : S.Walls)
				{
					if (Lv->Sectors.IsValidIndex(W.Adjoin)) Floor = FMath::Min(Floor, Lv->Sectors[W.Adjoin].Floor.Y);
				}
			}
		}
		if (Y < Floor - S0 || Y > Lv->SurfaceY(S, Pt, false) + S0) continue;
		if (bOldJumppick) return S.Index;
		const double Score = (S.NoWalls() ? 1e6 : 0.0) + FMath::Abs(Y - Floor);
		if (Score < BestScore) { Best = S.Index; BestScore = Score; }
	}
	return Best;
}

int32 AOutlawsLevelActor::NumSectors() const
{
	const FOutlawsLevel* Lv = Inf ? &Inf->Live : LevelData.Get();
	return Lv ? Lv->Sectors.Num() : 0;
}

bool AOutlawsLevelActor::SectorWalkLine(int32 Si, double MinWallCm, TArray<FVector>& Out, double& OutWidthCm, double& OutLengthCm) const
{
	Out.Reset();
	OutWidthCm = 1e18;
	OutLengthCm = 0.0;
	const FOutlawsLevel* Lv = Inf ? &Inf->Live : LevelData.Get();
	const FOutlawsSector* S = Lv ? Lv->Sector(Si) : nullptr;
	if (!S || S->Verts.Num() < 3) return false;
	const double U = FOutlawsGeometry::UnitsToCm, Step = 0.05;
	for (const FOutlawsWall& Wl : S->Walls)
	{
		if (!S->Verts.IsValidIndex(Wl.V1) || !S->Verts.IsValidIndex(Wl.V2)) continue;
		const FVector2D A = S->Verts[Wl.V1], B = S->Verts[Wl.V2];
		const double Len = FVector2D::Distance(A, B);
		if (Len * U < MinWallCm) continue;
		const FVector2D Mid = (A + B) * 0.5, N = FVector2D(-(B - A).Y, (B - A).X) / Len;
		// the side the sector lies on, then across it to its far edge
		const double Side = Lv->ContainsXZ(*S, Mid.X + N.X * Step, Mid.Y + N.Y * Step) ? 1.0 : Lv->ContainsXZ(*S, Mid.X - N.X * Step, Mid.Y - N.Y * Step) ? -1.0 : 0.0;
		if (Side == 0.0) continue;
		double D = Step;
		while (D < 40.0 && Lv->ContainsXZ(*S, Mid.X + N.X * Side * (D + Step), Mid.Y + N.Y * Side * (D + Step))) D += Step;
		OutWidthCm = FMath::Min(OutWidthCm, D * U);
		for (const double F : { -0.35, 0.0, 0.35 })
		{
			// near each end and the middle, so the line keeps to a bending strip's floor
			const FVector2D P = Mid + (B - A) * F + N * Side * (D * 0.5);
			if (!Lv->ContainsXZ(*S, P.X, P.Y)) continue;
			const FVector At = FOutlawsGeometry::ToUE(P, Lv->SurfaceY(*S, P, true));
			if (Out.Num() > 0) OutLengthCm += FVector::Dist2D(Out.Last(), At);
			Out.Add(At);
		}
	}
	return Out.Num() > 0;
}

double AOutlawsLevelActor::SectorHeadroomCm(int32 Si, int32& OutDrops) const
{
	OutDrops = 0;
	const FOutlawsLevel* Lv = Inf ? &Inf->Live : LevelData.Get();
	const FOutlawsSector* S = Lv ? Lv->Sector(Si) : nullptr;
	if (!S) return 0.0;
	for (const FOutlawsWall& Wl : S->Walls)
	{
		const FOutlawsSector* O = Lv->Sector(Wl.Adjoin);
		if (O && O->Floor.Y < S->Floor.Y - 1.0) ++OutDrops;
	}
	return (S->Ceil.Y - S->Floor.Y) * FOutlawsGeometry::UnitsToCm;
}

FString AOutlawsLevelActor::DescribeSector(int32 Si) const
{
	const FOutlawsLevel* Lv = Inf ? &Inf->Live : LevelData.Get();
	const FOutlawsSector* S = Lv ? Lv->Sector(Si) : nullptr;
	if (!S) return TEXT("no such sector");
	FString Adj;
	for (const FOutlawsWall& Wl : S->Walls)
	{
		const FOutlawsSector* O = Lv->Sector(Wl.Adjoin);
		Adj += O ? FString::Printf(TEXT(" %d(floor %.1f ceil %.1f)"), O->Index, O->Floor.Y, O->Ceil.Y) : FString(TEXT(" solid"));
	}
	return FString::Printf(TEXT("sector %d id %s '%s' floor %.1f ceil %.1f flags %u, %d verts, %d walls; adjoins:%s"), S->Index, *S->Id, *S->Name, S->Floor.Y, S->Ceil.Y, S->Flags, S->Verts.Num(),
		S->Walls.Num(), *Adj);
}

bool AOutlawsLevelActor::SectorMiddleSpot(int32 Si, float Radius, float Half, const AActor* Ignore, FVector& OutCentre) const
{
	const FOutlawsLevel* Lv = Inf ? &Inf->Live : LevelData.Get();
	const FOutlawsSector* S = Lv ? Lv->Sector(Si) : nullptr;
	UWorld* W = GetWorld();
	if (!S || !W || S->Verts.Num() < 3) return false;
	const double U = FOutlawsGeometry::UnitsToCm;
	FVector2D Mid(0.0, 0.0), Lo(1e18, 1e18), Hi(-1e18, -1e18);
	for (const FVector2D& V : S->Verts)
	{
		Mid += V;
		Lo = FVector2D(FMath::Min(Lo.X, V.X), FMath::Min(Lo.Y, V.Y));
		Hi = FVector2D(FMath::Max(Hi.X, V.X), FMath::Max(Hi.Y, V.Y));
	}
	Mid /= double(S->Verts.Num());
	// the centroid first, then a 12 x 12 grid over the outline nearest the centroid first
	TArray<FVector2D> Tries;
	Tries.Add(Mid);
	for (int32 i = 0; i <= 12; ++i)
		for (int32 j = 0; j <= 12; ++j) Tries.Add(FVector2D(FMath::Lerp(Lo.X, Hi.X, i / 12.0), FMath::Lerp(Lo.Y, Hi.Y, j / 12.0)));
	Tries.Sort([&Mid](const FVector2D& A, const FVector2D& B) { return FVector2D::DistSquared(A, Mid) < FVector2D::DistSquared(B, Mid); });
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsSectorMiddle), false, Ignore);
	for (const FVector2D& T : Tries)
	{
		if (!Lv->ContainsXZ(*S, T.X, T.Y)) continue;
		const double F = Lv->SurfaceY(*S, T, true), C = Lv->SurfaceY(*S, T, false);
		if ((C - F) * U < 2.0 * Half + 4.0) continue;
		const FVector At = FOutlawsGeometry::ToUE(T.X, F, T.Y) + FVector(0.0, 0.0, Half + 3.0);
		if (W->OverlapBlockingTestByChannel(At, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(Radius + 5.f, Half), Q)) continue;
		OutCentre = At;
		return true;
	}
	return false;
}

bool AOutlawsLevelActor::DebugCeilBoxSpotN(int32 Nth, FVector& OutStand, FVector& OutLip, FVector& OutIn, FString& OutWhat, double& OutTopZ, double& OutCeilZ, double& OutAboveZ) const
{
	if (!LevelData) return false;
	const FOutlawsLevel& Lv = *LevelData;
	const double U = FOutlawsGeometry::UnitsToCm;
	int32 Seen = 0;
	TSet<int32> Boxes;                   // (one wall per box)
	for (const FOutlawsSector& S : Lv.Sectors)
	{
		for (const FOutlawsWall& W : S.Walls)
		{
			if (W.Adjoin < 0 || !Lv.Sectors.IsValidIndex(W.Adjoin) || (W.Flags1 & (OutlawsFlags::WallNoPass | OutlawsFlags::WallFence)) || Boxes.Contains(W.Adjoin)) continue;
			const FOutlawsSector& B = Lv.Sectors[W.Adjoin];
			const double Rise = B.Floor.Y - S.Floor.Y, Room = B.Ceil.Y - B.Floor.Y;
			if (Rise < 1.5 || Rise > 6.0 || Room < 3.5 || Room > 8.0 || !S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) continue;
			const FVector2D A = S.Verts[W.V1], C = S.Verts[W.V2];
			if (FVector2D::Distance(A, C) < 2.0) continue;
			const FVector2D Mid = (A + C) * 0.5;
			FVector2D N = FVector2D(-(C - A).Y, (C - A).X).GetSafeNormal();
			if (!Lv.ContainsXZ(B, Mid.X + N.X * 0.5, Mid.Y + N.Y * 0.5)) N = -N;
			if (!Lv.ContainsXZ(B, Mid.X + N.X * 2.0, Mid.Y + N.Y * 2.0) || !Lv.ContainsXZ(S, Mid.X - N.X * 1.5, Mid.Y - N.Y * 1.5)) continue;
			const FOutlawsSector* Up = nullptr;
			for (const FOutlawsSector& T : Lv.Sectors)
			{
				if (&T == &B || T.Floor.Y < B.Ceil.Y - 0.01 || T.Floor.Y > B.Ceil.Y + 8.0) continue;
				if (Lv.ContainsXZ(T, Mid.X + N.X * 2.0, Mid.Y + N.Y * 2.0)) { Up = &T; break; }
			}
			if (!Up) continue;
			Boxes.Add(W.Adjoin);
			if (Seen++ != Nth) continue;
			OutStand = FOutlawsGeometry::ToUE(Mid.X - N.X * 1.5, S.Floor.Y, Mid.Y - N.Y * 1.5);
			OutLip = FOutlawsGeometry::ToUE(Mid.X, B.Floor.Y, Mid.Y);
			OutIn = FOutlawsGeometry::ToUE(N.X, 0.0, N.Y).GetSafeNormal();
			OutTopZ = B.Floor.Y * U;
			OutCeilZ = B.Ceil.Y * U;
			OutAboveZ = Up->Floor.Y * U;
			OutWhat = FString::Printf(TEXT("box sector %d (%s), %.0f cm over sector %d, ceiling %.0f cm over its top, the room over it sector %d (%s) at %.0f cm (a gap of %.0f cm)"), B.Index, *B.Name,
				Rise * U, S.Index, Room * U, Up->Index, *Up->Name, Up->Floor.Y * U, (Up->Floor.Y - B.Ceil.Y) * U);
			return true;
		}
	}
	return false;
}

double AOutlawsLevelActor::CeilingZAt(const FVector& P, int32* OutSector) const
{
	if (OutSector) *OutSector = -1;
	const FOutlawsLevel* Lv = Inf ? &Inf->Live : LevelData.Get();
	if (!Lv) return 1e9;
	const double U = FOutlawsGeometry::UnitsToCm;
	const FOutlawsSector* S = Lv->SectorAt(FOutlawsGeometry::ToOutlaws(P, 0.2), PlayerSkip());      // (the player's lookups skip a zone gate's far side)
	if (!S) return 1e9;
	if (OutSector) *OutSector = S->Index;
	return Lv->SurfaceY(*S, FOutlawsGeometry::ToOutlawsPlan(P), false) * U;
}

bool AOutlawsLevelActor::DebugRoofLip(int32 Nth, FVector& OutGround, FVector& OutLip, FVector& OutIn, int32& OutSector) const
{
	const FOutlawsLevel* LP = Inf ? &Inf->Live : nullptr;
	if (!LP) return false;
	const double U = FOutlawsGeometry::UnitsToCm;
	int32 Found = 0;
	for (const FOutlawsSector& S : LP->Sectors)
	{
		if (!S.SlopeFloor.bSet || S.Verts.Num() < 3) continue;
		for (const FOutlawsWall& W : S.Walls)
		{
			if (!LP->Sectors.IsValidIndex(W.Adjoin) || !S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) continue;
			const FOutlawsSector& G = LP->Sectors[W.Adjoin];
			const FVector2D A = S.Verts[W.V1], B = S.Verts[W.V2], M = (A + B) * 0.5;
			const double Lip = LP->SurfaceY(S, M, true), Ground = LP->SurfaceY(G, M, true);
			const double Rise = (Lip - Ground) * U;
			if (Rise < 120.0 || Rise > 240.0 || LP->SurfaceY(G, M, false) - Ground < 7.0) continue;
			FVector2D N(-(B - A).Y, (B - A).X);
			N.Normalize();
			// (inward: toward the roof sector's middle)
			FVector2D C = FVector2D::ZeroVector;
			for (const FVector2D& V : S.Verts) C += V;
			C /= S.Verts.Num();
			if (FVector2D::DotProduct(C - M, N) < 0.0) N = -N;
			if (Found++ < Nth) continue;
			OutLip = FOutlawsGeometry::ToUE(M.X, Lip, M.Y);
			OutGround = FOutlawsGeometry::ToUE(M.X - N.X * 1.5, Ground, M.Y - N.Y * 1.5);
			OutIn = FOutlawsGeometry::ToUE(N.X, 0.0, N.Y).GetSafeNormal();
			OutSector = S.Index;
			return true;
		}
	}
	return false;
}

// ---- CPU1 step 0a: the counts behind the perf buckets -----------------------------------------------------------------

int32 AOutlawsLevelActor::PerfVisibleSectors() const
{
	return VisibleSectors.CountSetBits();
}

int32 AOutlawsLevelActor::PerfMoverGroups() const
{
	return Groups.Num();
}

bool AOutlawsLevelActor::DebugBarsSpot(FVector& OutStand, FVector& OutLip, FVector& OutIn, FString& OutWhat) const
{
	if (!LevelData) return false;
	const FOutlawsLevel& Lv = *LevelData;
	for (const FOutlawsSector& S : Lv.Sectors)
	{
		for (const FOutlawsWall& W : S.Walls)
		{
			if (W.Adjoin < 0 || !(W.Flags1 & OutlawsFlags::WallMidTex) || !(W.Flags1 & (OutlawsFlags::WallNoPass | OutlawsFlags::WallFence)) || !Lv.Sectors.IsValidIndex(W.Adjoin)) continue;
			const FOutlawsSector& B = Lv.Sectors[W.Adjoin];
			const double Sill = B.Floor.Y - S.Floor.Y;
			if (Sill < 0.0 || Sill > 6.0 || B.Ceil.Y - B.Floor.Y < 4.0 || !S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) continue;
			const FVector2D A = S.Verts[W.V1], C = S.Verts[W.V2];
			if (FVector2D::Distance(A, C) < 2.0) continue;
			const FVector2D Mid = (A + C) * 0.5;
			FVector2D N = FVector2D(-(C - A).Y, (C - A).X).GetSafeNormal();
			if (!Lv.ContainsXZ(B, Mid.X + N.X * 0.5, Mid.Y + N.Y * 0.5)) N = -N;
			// (the bars are often a thin sector's side: the stand is wherever the floor is, 1.5 units back)
			if (!Lv.ContainsXZ(B, Mid.X + N.X * 0.5, Mid.Y + N.Y * 0.5) || !Lv.SectorAt(Mid.X - N.X * 1.5, S.Floor.Y + 0.5, Mid.Y - N.Y * 1.5)) continue;
			OutStand = FOutlawsGeometry::ToUE(Mid.X - N.X * 1.5, S.Floor.Y, Mid.Y - N.Y * 1.5);
			// (the hands on the bars at chest height (a cell's bars stand on the floor; a window's on its sill): the climb's lip)
			OutLip = FOutlawsGeometry::ToUE(Mid.X, FMath::Min(B.Floor.Y + 3.5, B.Ceil.Y - 0.5), Mid.Y);
			OutIn = (FOutlawsGeometry::ToUE(N.X, 0.0, N.Y)).GetSafeNormal();
			OutWhat = FString::Printf(TEXT("sector %d (%s) wall %s to sector %d (%s), flags 0x%x, mid tex %d, sill %.0f cm"), S.Index, *S.Name, *W.Id, B.Index, *B.Name, W.Flags1, W.Mid,
				Sill * FOutlawsGeometry::UnitsToCm);
			return true;
		}
	}
	return false;
}
