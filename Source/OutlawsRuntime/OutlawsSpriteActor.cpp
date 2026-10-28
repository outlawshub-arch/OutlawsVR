#include "OutlawsSpriteActor.h"
#include "OutlawsFlags.h"
#include "OutlawsGeometry.h"
#include "OutlawsPerf.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/Texture.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Tickable.h"
#include "OutlawsRuntime.h"
#include "EngineUtils.h"
#include "UnrealClient.h"
#include "Misc/Paths.h"
#include "Engine/Engine.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "HAL/IConsoleManager.h"
#include "OutlawsDeviceProfile.h"
#if WITH_EDITOR
#include "Editor.h"
#include "LevelEditorViewport.h"
#endif

namespace
{
	TArray<TWeakObjectPtr<AOutlawsSpriteActor>> GDriven;

	/** Ticks the game world's sprites once a frame (after the actors, so the camera has moved). */
	class FSpriteDriver : public FTickableGameObject
	{
	public:
		TWeakObjectPtr<UWorld> World;
		virtual void Tick(float DeltaTime) override { AOutlawsSpriteActor::TickAll(World.Get(), DeltaTime); }
		virtual TStatId GetStatId() const override { RETURN_QUICK_DECLARE_CYCLE_STAT(FOutlawsSpriteDriver, STATGROUP_Tickables); }
		virtual ETickableTickType GetTickableTickType() const override { return ETickableTickType::Always; }
		virtual UWorld* GetTickableGameObjectWorld() const override { return World.Get(); }
	};
	FSpriteDriver* GDriver = nullptr;          // made on first use and never freed: tickables unregister at exit

	// portal culling, published by the level each frame
	bool GCullOn = false;
	const TBitArray<>* GCullVisible = nullptr;
	FVector GCullEye = FVector::ZeroVector;
	TFunction<int32(int32, const FVector&, const FVector&)> GCullTrack;
	// (V1: was 3 m, which let a shared-space room's things show from the sector beside them) only what is in arm's reach
	constexpr double CullSafeCm = 60.0;
}

int32 AOutlawsSpriteActor::NumCulled = 0;
int32 AOutlawsSpriteActor::NumLodSprites = 0;
int32 AOutlawsSpriteActor::NumLodGone = 0;
float AOutlawsSpriteActor::LodMaxStep = 0.f;

static TAutoConsoleVariable<int32> CVarOLObjectLod(TEXT("ol.ObjectLod"), 1,
	TEXT("CPU1 object LOD: the 3D items and props drawn as their sprite further off and faded out far away (1), or the model at every distance (0)."));
static TAutoConsoleVariable<float> CVarOLObjectLodScale(TEXT("ol.ObjectLodScale"), 1.f,
	TEXT("CPU1 object LOD: every LOD distance times this (tuning; the Graphics page's Object Detail scales them too)."));

void AOutlawsSpriteActor::SetCulling(bool bOn, const TBitArray<>* Visible, const FVector& Eye, TFunction<int32(int32, const FVector&, const FVector&)> Track)
{
	GCullOn = bOn && Visible != nullptr;
	GCullVisible = Visible;
	GCullEye = Eye;
	if (Track || !bOn) GCullTrack = MoveTemp(Track);
}

bool AOutlawsSpriteActor::UpdateCull(float DeltaSeconds)
{
	if (!GCullOn)
	{
		FlushHeldMove();
		if (bCulled)
		{
			bCulled = false;
			--NumCulled;
			if (RootComponent) RootComponent->SetHiddenInGame(false, true);
		}
		return false;
	}
	const FVector P = GetActorLocation() + HeldMove;       // (where it is, a move kept back while culled counted)
	// (V1) the sector is the thing's own: its OB's at first, then followed across the walls it is carried over, checked
	// once it has moved 30 cm (scenery never does)
	if (CullSector == -2)
	{
		CullSector = HomeSector >= 0 ? HomeSector : (GCullTrack ? GCullTrack(-1, P, P) : -1);
		CullPos = P;
	}
	else if (FVector::DistSquared(P, CullPos) > 900.0)
	{
		CullSector = GCullTrack ? GCullTrack(CullSector, CullPos, P) : -1;
		CullPos = P;
	}
	const bool bSee = CullSector < 0 || !GCullVisible->IsValidIndex(CullSector) || (*GCullVisible)[CullSector]
		|| FVector::DistSquared(P, GCullEye) < FMath::Square(CullSafeCm);
	CullHold = bSee ? 0.25f : CullHold - DeltaSeconds;
	const bool bHide = !bSee && CullHold <= 0.f;
	if (bHide != bCulled)
	{
		bCulled = bHide;
		NumCulled += bHide ? 1 : -1;
		if (RootComponent) RootComponent->SetHiddenInGame(bHide, true);
	}
	if (!bCulled) FlushHeldMove();                          // (drawn: in its place first)
	return bCulled;
}

void AOutlawsSpriteActor::AdvanceClock(float DeltaSeconds)
{
	// a culled sprite's animation keeps its time (a prop's chor events and a one-shot's end are read from it)
	if (ForcedCell < 0 && !bClockHeld && (Chors.IsValidIndex(PlayingChor) || (FPS > 0.f && FrameCount > 1)))
	{
		AnimTime += DeltaSeconds;
	}
}

AOutlawsSpriteActor::AOutlawsSpriteActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;

	// the root is the object's foot point; the quad under it moves off it to stand on the frame's insertion point
	// (SetFeet, user 2026-10-02)
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	RootComponent = Root;
	Quad = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Quad"));
	Quad->SetupAttachment(Root);
	Quad->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Quad->SetCastShadow(false);
	Quad->SetMobility(EComponentMobility::Movable);
	Quad->BoundsScale = 3.f;
}

void AOutlawsSpriteActor::OnConstruction(const FTransform& Transform)
{
	Super::OnConstruction(Transform);
	CurrentCell = -1;
	if (EnsureMID())
	{
		ApplyCell(Grid.Num() > 0 ? Grid[0] : 0);
	}
}

void AOutlawsSpriteActor::BeginPlay()
{
	Super::BeginPlay();
	CurrentCell = -1;
	EnsureMID();
	UWorld* World = GetWorld();
	if (World && World->IsGameWorld())
	{
		if (!GDriver) GDriver = new FSpriteDriver();
		GDriver->World = World;
		GDriven.Add(this);
		bDriven = true;
		SetActorTickEnabled(false);
	}
}

void AOutlawsSpriteActor::CopyLookTo(UMaterialInstanceDynamic* M) const
{
	if (!M) return;
	const int32 Cols = FMath::Max(1, AtlasCols);
	const int32 Rows = FMath::Max(1, AtlasRows);
	const int32 C = FMath::Clamp(CurrentCell, 0, Cols * Rows - 1);
	const float SX = 1.f / Cols;
	const float SY = 1.f / Rows;
	if (UTexture* Held = HeldLookShown())
	{
		// (REQ-GRABUPSCALE) the held look: its own picture over the whole quad
		M->SetVectorParameterValue(TEXT("UVScale"), FLinearColor(1.f, 1.f, 0.f, 0.f));
		M->SetVectorParameterValue(TEXT("UVOffset"), FLinearColor(0.f, 0.f, 0.f, 0.f));
		M->SetTextureParameterValue(TEXT("Tex"), Held);
		return;
	}
	M->SetVectorParameterValue(TEXT("UVScale"), FLinearColor(SX, SY, 0.f, 0.f));
	M->SetVectorParameterValue(TEXT("UVOffset"), FLinearColor((C % Cols) * SX, (C / Cols) * SY, 0.f, 0.f));
	if (SpriteTexture) M->SetTextureParameterValue(TEXT("Tex"), SpriteTexture);
}

void AOutlawsSpriteActor::SetHeldLook(int32 Cell, UTexture* Tex)
{
	if (Tex) HeldLook.Add(Cell, Tex);
	else HeldLook.Remove(Cell);
	CurrentCell = -1;                     // (shown from the next update)
}

void AOutlawsSpriteActor::ClearHeldLook()
{
	if (HeldLook.Num() == 0 && !bHeldLookShown) return;
	HeldLook.Reset();
	if (MID && bHeldLookShown)
	{
		// back to the atlas at once (the let-go's frame shows the atlas, not a picture stretched over another cell)
		bHeldLookShown = false;
		if (SpriteTexture) MID->SetTextureParameterValue(TEXT("Tex"), SpriteTexture);
		const int32 Shown = CurrentCell;
		CurrentCell = -1;
		if (Shown >= 0) ApplyCell(Shown);
	}
	bHeldLookShown = false;
}

UTexture* AOutlawsSpriteActor::DebugQuadTexture() const
{
	UTexture* T = nullptr;
	if (MID) MID->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Tex")), T);
	return T;
}

void AOutlawsSpriteActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (bDriven)
	{
		GDriven.RemoveAllSwap([this](const TWeakObjectPtr<AOutlawsSpriteActor>& P) { return !P.IsValid() || P.Get() == this; });
		bDriven = false;
		if (bCulled)
		{
			bCulled = false;
			--NumCulled;
		}
	}
	Super::EndPlay(EndPlayReason);
}

void AOutlawsSpriteActor::TickAll(UWorld* World, float DeltaSeconds)
{
	OL_PERF_SCOPE("Sprites (all)");
	static const bool bAnchorShots = OLFlags::Param(TEXT("OLAnchorShots")) && OLFlags::Param(TEXT("OLShots"));
	if (bAnchorShots)
	{
		TickAnchorShots(World, DeltaSeconds);
	}
	FVector Viewer;
	int32 ViewerState = 0;                     // 0 not looked up yet, 1 found, 2 none
	// (CPU1 object LOD) one scale for every distance this frame; 0 = off (each model back to the model)
	static const bool bNoLod = OLFlags::Param(TEXT("OLNoObjectLod"));
	const float LodScale = bNoLod || CVarOLObjectLod.GetValueOnGameThread() == 0 ? 0.f
		: FMath::Max(0.05f, CVarOLObjectLodScale.GetValueOnGameThread()) * OutlawsDeviceProfile::ObjectDetailScale();
	NumLodSprites = NumLodGone = 0;
	for (int32 i = GDriven.Num() - 1; i >= 0; --i)
	{
		AOutlawsSpriteActor* S = GDriven[i].Get();
		if (!S)
		{
			GDriven.RemoveAtSwap(i);
			continue;
		}
		if (S->GetWorld() != World || S->IsActorBeingDestroyed())
		{
			continue;
		}
		if (ViewerState == 0)
		{
			ViewerState = S->GetViewerLocation(Viewer) ? 1 : 2;
		}
		// an actor tick's time: the world's, scaled by the actor's own dilation (the bad guys' pace slider)
		const float Dt = DeltaSeconds * S->CustomTimeDilation;
		if (S->UpdateCull(Dt))
		{
			S->AdvanceClock(Dt);           // out of the eye's portals: no turning, no frame to pick
			continue;
		}
		S->UpdateSprite(Dt, ViewerState == 1 ? &Viewer : nullptr);
		if (S->Model && (S->LodSpriteAt > 0.f || S->bLodActive))
		{
			S->UpdateLod(ViewerState == 1 ? Viewer : S->GetActorLocation(), S->LodSpriteAt > 0.f ? LodScale : 0.f);
		}
	}
}

void AOutlawsSpriteActor::SetLod(float SpriteAtCm, float GoneAtCm)
{
	LodSpriteAt = FMath::Max(0.f, SpriteAtCm);
	LodGoneAt = FMath::Max(0.f, GoneAtCm);
	if (LodSpriteAt <= 0.f && bLodActive) ApplyLod(0.f, 0.f);
}

void AOutlawsSpriteActor::UpdateLod(const FVector& Viewer, float Scale)
{
	// in a hand (HoldView) or switched off: the model as before
	const bool bHeld = HeldViewFrame > 0 && GFrameCounter - HeldViewFrame <= 2;
	float Fm = 0.f, Sa = 0.f;
	if (!bHeld && Scale > 0.f)
	{
		const double D = FVector::Dist(Viewer, GetActorLocation());
		// model to sprite over a band of a quarter of the distance (1.5 to 5 m) centred on it
		const double D1 = LodSpriteAt * Scale;
		const double B1 = FMath::Clamp(D1 * 0.25, 150.0, 500.0);
		const float S = FMath::SmoothStep(float(D1 - B1 * 0.5), float(D1 + B1 * 0.5), float(D));
		// sprite to nothing over a fifth of the far distance (3 to 8 m)
		float G = 1.f;
		if (LodGoneAt > 0.f && !bLodKeep)
		{
			const double D2 = FMath::Max(double(LodGoneAt) * Scale, D1 + B1);
			const double B2 = FMath::Clamp(D2 * 0.2, 300.0, 800.0);
			G = 1.f - FMath::SmoothStep(float(D2 - B2 * 0.5), float(D2 + B2 * 0.5), float(D));
		}
		// steps of 1/16 and 1/32: the model's proxy is rebuilt for each new fade, so only so many in a band
		Fm = FMath::RoundToFloat(S * 16.f) / 16.f;
		Sa = FMath::RoundToFloat(S * G * 32.f) / 32.f;
	}
	ApplyLod(Fm, Sa);
	NumLodSprites += Fm >= 1.f && Sa > 0.f ? 1 : 0;
	NumLodGone += Fm >= 1.f && Sa <= 0.f ? 1 : 0;
}

void AOutlawsSpriteActor::ApplyLod(float ModelFade, float SpriteAlpha)
{
	// (the sweep's pop meter: a step between two frames in a row, of a model or sprite drawn last frame; a sprite coming
	// back from portal culling was out of sight)
	const bool bSeen = (Model && Model->WasRecentlyRendered(0.2f)) || (Quad && Quad->WasRecentlyRendered(0.2f));
	if (LodFrame + 1 == GFrameCounter && bSeen)
	{
		LodMaxStep = FMath::Max(LodMaxStep, FMath::Max(FMath::Abs(ModelFade - LodFApplied), FMath::Abs(SpriteAlpha - LodSpriteAlpha)));
	}
	LodFrame = GFrameCounter;
	LodFApplied = ModelFade;
	if (Model)
	{
		const bool bShow = ModelFade < 1.f;
		if (bShow && ModelFade != LodModelFade)
		{
			Model->SetCustomPrimitiveDataFloat(0, ModelFade);    // (M_OLR_ItemModel / M_OLR_ItemInk: the dither)
			LodModelFade = ModelFade;
		}
		if (Model->GetVisibleFlag() != bShow) Model->SetVisibility(bShow);
	}
	if (Quad)
	{
		const bool bShow = SpriteAlpha > 0.f;
		if (bShow)
		{
			const bool bSwitched = LodQuadLook(SpriteAlpha < 1.f);
			if (SpriteAlpha < 1.f && MID && (bSwitched || SpriteAlpha != LodSpriteAlpha)) MID->SetScalarParameterValue(TEXT("Fade"), SpriteAlpha);
		}
		if (Quad->GetVisibleFlag() != bShow) Quad->SetVisibility(bShow);
		LodSpriteAlpha = SpriteAlpha;
	}
	bLodActive = ModelFade > 0.f || SpriteAlpha > 0.f;
}

bool AOutlawsSpriteActor::LodQuadLook(bool bFade)
{
	if (bFade == bLodQuadFade || !Quad || !EnsureMID()) return false;
	const int32 Shown = CurrentCell;
	if (!bLodQuadFade) LodSolidMID = MID;          // its look as drawn without LOD
	if (bFade)
	{
		if (!LodFadeMID)
		{
			static TWeakObjectPtr<UMaterialInterface> FadeBase;
			if (!FadeBase.IsValid()) FadeBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_SpriteFade.M_OLR_SpriteFade"));
			if (!FadeBase.IsValid()) return false;
			LodFadeMID = UMaterialInstanceDynamic::Create(FadeBase.Get(), Quad);
			if (!LodFadeMID) return false;
		}
		Quad->SetMaterial(0, LodFadeMID);
		MID = LodFadeMID;
	}
	else
	{
		if (!LodSolidMID) return false;
		Quad->SetMaterial(0, LodSolidMID);
		MID = LodSolidMID;
	}
	bLodQuadFade = bFade;
	if (SpriteTexture) MID->SetTextureParameterValue(TEXT("Tex"), SpriteTexture);
	MID->SetScalarParameterValue(TEXT("Brightness"), LitBrightness());
	CurrentCell = -1;
	if (Shown >= 0) ApplyCell(Shown);
	return true;
}

void AOutlawsSpriteActor::SetLook(UTexture* Hd)
{
	if (!SpriteTextureLo) SpriteTextureLo = SpriteTexture;
	SpriteTexture = Hd ? Hd : SpriteTextureLo.Get();
	if (MID && SpriteTexture) MID->SetTextureParameterValue(TEXT("Tex"), SpriteTexture);
	if (bHeldLookShown)
	{
		bHeldLookShown = false;           // (REQ-GRABUPSCALE) the held look put back on by the next update
		CurrentCell = -1;
	}
}

void AOutlawsSpriteActor::SetLightLevel(int32 Level)
{
	if (Level == LightLevel)
	{
		return;
	}
	LightLevel = Level;
	if (MID) MID->SetScalarParameterValue(TEXT("Brightness"), LitBrightness());
}

void AOutlawsSpriteActor::ShowModel(UPrimitiveComponent* InModel)
{
	Model = InModel;
	if (Model) Model->SetVisibility(true);
	if (Quad) Quad->SetVisibility(false);
}

void AOutlawsSpriteActor::ShowSprite()
{
	// (CPU1 object LOD) the sprite for good: no more LOD, its solid look back
	LodSpriteAt = 0.f;
	if (bLodActive)
	{
		LodQuadLook(false);
		if (Model) Model->SetCustomPrimitiveDataFloat(0, 0.f);
		LodModelFade = LodSpriteAlpha = 0.f;
		bLodActive = false;
	}
	if (Model) Model->SetVisibility(false);
	if (Quad) Quad->SetVisibility(true);
}

void AOutlawsSpriteActor::SetFade(float Alpha)
{
	if (!Quad)
	{
		return;
	}
	if (!bFadeMaterial)
	{
		UMaterialInterface* Fade = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_SpriteFade.M_OLR_SpriteFade"));
		if (!Fade)
		{
			return;                  // (not made yet: stays solid)
		}
		bFadeMaterial = true;
		const int32 Shown = CurrentCell;
		Quad->SetMaterial(0, Fade);
		MID = nullptr;
		if (EnsureMID() && Shown >= 0)
		{
			ApplyCell(Shown);
		}
	}
	if (MID)
	{
		MID->SetScalarParameterValue(TEXT("Fade"), FMath::Clamp(Alpha, 0.f, 1.f));
	}
}

bool AOutlawsSpriteActor::EnsureMID()
{
	if (!Quad)
	{
		return false;
	}
	if (MID && Quad->GetMaterial(0) == MID)
	{
		return true;
	}
	UMaterialInterface* Base = Quad->GetMaterial(0);
	if (!Base)
	{
		return false;
	}
	if (UMaterialInstanceDynamic* Existing = Cast<UMaterialInstanceDynamic>(Base))
	{
		if (Existing->GetOuter() == Quad || Existing->GetOuter() == this)
		{
			MID = Existing;          // ours (saved with the map, or created earlier)
		}
		else
		{
			Base = Existing->Parent; // never chain onto another actor's instance
		}
	}
	if (!MID || Quad->GetMaterial(0) != MID)
	{
		MID = UMaterialInstanceDynamic::Create(Base, Quad);
		if (!MID)
		{
			return false;
		}
		Quad->SetMaterial(0, MID);
	}
	if (SpriteTexture)
	{
		MID->SetTextureParameterValue(TEXT("Tex"), SpriteTexture);
	}
	bHeldLookShown = false;
	// sector light in display space, as the Jedi colormaps apply it (the world materials do the same); a flame full bright
	MID->SetScalarParameterValue(TEXT("Brightness"), LitBrightness());
	CurrentCell = -1;
	return true;
}

void AOutlawsSpriteActor::SetOnTop(bool bOn)
{
	if (!Quad) return;
	UMaterialInterface* OnTop = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_SpriteOnTop.M_OLR_SpriteOnTop"));
	if (!OnTop) return;
	UMaterialInterface* Cur = MID ? MID->Parent.Get() : Quad->GetMaterial(0);
	if (bOn)
	{
		if (Cur == OnTop) return;
		OffTopBase = Cur;
		Quad->SetMaterial(0, OnTop);
	}
	else
	{
		if (Cur != OnTop || !OffTopBase) return;
		Quad->SetMaterial(0, OffTopBase);
	}
	const int32 Shown = CurrentCell;
	MID = nullptr;
	if (EnsureMID() && Shown >= 0) ApplyCell(Shown);
	// its bounds reach the eye, so occlusion never drops it for standing behind a wall while it is drawn over it
	// (BUG-FOVCLIP: off again it went to 1, below the constructor's 3 that the camera-facing quad needs, so a body
	// carried and dropped could vanish at the edge of view)
	Quad->SetBoundsScale(bOn ? 4.f : 3.f);
}

void AOutlawsSpriteActor::ApplyCell(int32 Cell)
{
	if (!MID || Cell == CurrentCell)
	{
		return;
	}
	const int32 Cols = FMath::Max(1, AtlasCols);
	const int32 Rows = FMath::Max(1, AtlasRows);
	const int32 C = FMath::Clamp(Cell, 0, Cols * Rows - 1);
	const float SX = 1.f / Cols;
	const float SY = 1.f / Rows;
	// (REQ-GRABUPSCALE) a held bad guy's cell with a picture of its own: that picture over the whole quad
	if (UTexture* Held = HeldLook.Num() > 0 ? HeldLook.FindRef(C).Get() : nullptr)
	{
		MID->SetTextureParameterValue(TEXT("Tex"), Held);
		MID->SetVectorParameterValue(TEXT("UVScale"), FLinearColor(1.f, 1.f, 0.f, 0.f));
		MID->SetVectorParameterValue(TEXT("UVOffset"), FLinearColor(0.f, 0.f, 0.f, 0.f));
		bHeldLookShown = true;
		CurrentCell = C;
		return;
	}
	if (bHeldLookShown)
	{
		if (SpriteTexture) MID->SetTextureParameterValue(TEXT("Tex"), SpriteTexture);
		bHeldLookShown = false;
	}
	MID->SetVectorParameterValue(TEXT("UVScale"), FLinearColor(SX, SY, 0.f, 0.f));
	MID->SetVectorParameterValue(TEXT("UVOffset"), FLinearColor((C % Cols) * SX, (C / Cols) * SY, 0.f, 0.f));
	CurrentCell = C;
}

float AOutlawsSpriteActor::GetFacingYaw() const
{
	// Outlaws yaw: 0 = north (+Z), increasing clockwise seen from above (LawMaker's
	// DrawObjectTriangle).  With UE.X = map Z (north) and UE.Y = map X (east), UE yaw 0 is north
	// and also increases clockwise, so the angle carries over unchanged.
	return FRotator::NormalizeAxis(OutlawsYaw + FacingYawOffset);
}

int32 AOutlawsSpriteActor::DirectionFor(const FVector& Viewer) const
{
	if (Directions <= 1)
	{
		return 0;
	}
	const FVector Here = GetActorLocation();
	const float ToViewerYaw = FMath::RadiansToDegrees(FMath::Atan2(Viewer.Y - Here.Y, Viewer.X - Here.X));
	float Rel = FRotator::NormalizeAxis(ToViewerYaw - GetFacingYaw());   // -180..180, 0 = viewer in front
	if (!bClockwiseDirections)
	{
		Rel = -Rel;
	}
	if (Rel < 0.f)
	{
		Rel += 360.f;
	}
	const float Step = 360.f / Directions;
	int32 Dir = FMath::RoundToInt(Rel / Step) + DirectionOffset;
	Dir = ((Dir % Directions) + Directions) % Directions;
	return Dir;
}

bool AOutlawsSpriteActor::GetViewerLocation(FVector& OutLocation) const
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	if (World->IsGameWorld())
	{
		if (APlayerController* PC = World->GetFirstPlayerController())
		{
			if (PC->PlayerCameraManager)
			{
				OutLocation = PC->PlayerCameraManager->GetCameraLocation();
				return true;
			}
			if (const APawn* Pawn = PC->GetPawn())
			{
				OutLocation = Pawn->GetActorLocation();
				return true;
			}
		}
		return false;
	}
#if WITH_EDITOR
	if (GCurrentLevelEditingViewportClient)
	{
		OutLocation = GCurrentLevelEditingViewportClient->GetViewLocation();
		return true;
	}
#endif
	return false;
}

void AOutlawsSpriteActor::Tick(float DeltaSeconds)
{
	OL_PERF_SCOPE("Sprites (all)");
	Super::Tick(DeltaSeconds);
	FVector Viewer;
	const bool bHaveViewer = GetViewerLocation(Viewer);
	UpdateSprite(DeltaSeconds, bHaveViewer ? &Viewer : nullptr);
}

void AOutlawsSpriteActor::UpdateSprite(float DeltaSeconds, const FVector* ViewerAt)
{
	const bool bHaveViewer = ViewerAt != nullptr;
	// held in a hand (HoldView): turned and drawn as seen straight along the hold's facing, not from the eye a hand's width
	// off (the quad's place below still goes by the real eye)
	const bool bHeldView = HeldViewFrame > 0 && GFrameCounter - HeldViewFrame <= 1;
	const FVector Viewer = !bHaveViewer ? FVector::ZeroVector
		: bHeldView ? GetActorLocation() + FRotator(0.0, HeldFaceYaw, 0.0).Vector() * 100.0 : *ViewerAt;
	if (bFaceViewer && bHaveViewer)
	{
		const FVector Here = GetActorLocation();
		const FVector2D To(Viewer.X - Here.X, Viewer.Y - Here.Y);
		if (To.SizeSquared() >= 1.f)
		{
			const float Yaw = FMath::RadiansToDegrees(FMath::Atan2(To.Y, To.X));
			// a new facing only past a quarter degree: a head's sway would otherwise send every sprite's transform to
			// the renderer every frame (EndOfFrameUpdates + UpdatePrimitiveTransform, ~2 ms on the Quest)
			if (FMath::Abs(FMath::FindDeltaAngleDegrees(GetActorRotation().Yaw, Yaw)) > 0.25f)
			{
				SetActorRotation(FRotator(0.f, Yaw, 0.f));
			}
		}
	}

	if (!EnsureMID())
	{
		return;
	}
	if (ForcedCell >= 0)
	{
		ApplyCell(ForcedCell);
	}
	else if (Chors.IsValidIndex(PlayingChor))
	{
		// a character's choreography: its own direction count, speed and cells
		const FOutlawsSpriteChor& C = Chors[PlayingChor];
		if (!bClockHeld) AnimTime += DeltaSeconds;
		const int32 Shown = GetChorFrame();
		const int32 Len = FMath::Max(1, C.Length);
		const int32 Frame = bChorLoop ? Shown % Len : FMath::Min(Shown, Len - 1);
		const int32 SavedDirs = Directions;
		Directions = C.Dirs;
		CurrentDirection = bHaveViewer ? DirectionFor(Viewer) : 0;
		Directions = SavedDirs;
		const int32 Index = C.GridStart + CurrentDirection * FMath::Max(1, C.Frames) + FMath::Min(Frame, C.Frames - 1);
		ApplyCell(ChorGrid.IsValidIndex(Index) ? ChorGrid[Index] : 0);
	}
	else
	{
		const int32 Frames = FMath::Max(1, FrameCount);
		int32 Frame = 0;
		if (FPS > 0.f && Frames > 1)
		{
			if (!bClockHeld) AnimTime += DeltaSeconds;
			Frame = static_cast<int32>(AnimTime * FPS) % Frames;
		}
		CurrentDirection = bHaveViewer ? DirectionFor(Viewer) : 0;
		const int32 Index = CurrentDirection * Frames + Frame;
		const int32 Cell = Grid.IsValidIndex(Index) ? Grid[Index] : 0;
		ApplyCell(Cell);
	}
	PlaceQuad(ViewerAt);
	if (bSwingDrive || bSwingShown) UpdateSwing();         // (REQ-HELDSWAY: OutlawsSpriteSwing.cpp)
}

void AOutlawsSpriteActor::SetFeet(const TArray<int16>& BelowPx, const TArray<int16>& FootPx, int32 CellH)
{
	if (CellH <= 0 || BelowPx.Num() != FootPx.Num())
	{
		return;
	}
	CellBelowPx = BelowPx;
	CellFootPx = FootPx;
	CellPxH = CellH;
}

void AOutlawsSpriteActor::SetFeetAcross(const TArray<int16>& SidePx2, int32 CellW)
{
	if (CellW <= 0 || SidePx2.Num() != CellBelowPx.Num())
	{
		return;
	}
	CellSidePx2 = SidePx2;
	CellPxW = CellW;
}

void AOutlawsSpriteActor::PlaceQuad(const FVector* Viewer)
{
	// (an actor saved in an old editor map has the quad as its root: left as it was)
	if (!Quad || !Root || Quad->GetAttachParent() != Root)
	{
		return;
	}
	// standing: facing the viewer, not held in a hand (HoldView), its quad shown (not a 3D stand-in)
	const bool bHeld = HeldViewFrame > 0 && GFrameCounter - HeldViewFrame <= 1;
	const bool bStand = !bHeld && bFaceViewer && CellPxH > 0 && CellBelowPx.IsValidIndex(CurrentCell) && Quad->IsVisible();
	if (!bStand)
	{
		if (bQuadMoved)
		{
			Quad->SetRelativeLocationAndRotation(FVector::ZeroVector, FRotator::ZeroRotator);
			Quad->SetRelativeScale3D(FVector::OneVector);
			bQuadMoved = false;
		}
		return;
	}
	// cm per atlas pixel up the quad (the actor's Z scale is the cell's height in cm; effects shrink it)
	const FVector A = GetActorLocation() - FVector(0.0, 0.0, FloorGapCm);
	const double CmPx = FMath::Abs(GetActorScale3D().Z) / CellPxH;
	const double Below = CellBelowPx[CurrentCell] * CmPx;
	const double Foot = CellFootPx[CurrentCell] * CmPx;
	// the quad's foot with the insertion point on the actor's place (R4: across too; the quad's width runs along the
	// actor's Y, the texture's right toward -Y as the viewer, on the actor's +X, sees it)
	FVector Want = A - FVector(0.0, 0.0, Below);
	if (CellPxW > 0 && CellSidePx2.IsValidIndex(CurrentCell))
	{
		const double Side = 0.5 * CellSidePx2[CurrentCell] * FMath::Abs(GetActorScale3D().Y) / CellPxW;
		Want += GetActorRightVector() * Side;
	}
	double K = 1.0;
	if (Foot > 0.05)
	{
		// the drawing under the floor would be cut by it: the quad scaled by K about the eye looks the same from
		// there, and K = H / (H + Foot) brings its lowest drawn row up onto the floor plane.  Not past MaxFootPullCm
		// nearer (or half the way to the eye); what is left is lifted.  An eye at or under the floor: lifted only
		double Lift = Foot;
		if (Viewer)
		{
			const double H = Viewer->Z - A.Z;
			const double D = FVector2D(Viewer->X - A.X, Viewer->Y - A.Y).Size();
			if (H > 1.0 && D > 1.0)
			{
				double Pull = FMath::Min(D * Foot / (H + Foot), FMath::Min(MaxFootPullCm, 0.5 * D));
				// (BUG-ENEMYWALLCLIP) never nearer than a wall between it and the eye: a bad guy standing just behind
				// a thin wall was drawn up to 60 cm nearer, through it (user snap 2026-10-19 21:01, the mill's sector
				// 591 seen from 526); what the wall leaves is lifted instead
				static const bool bOldEnemyWallClip = OLFlags::Param(TEXT("OLOldEnemyWallClip"));    // (as before, for shots)
				if (Pull > 2.0 && !bOldEnemyWallClip)
				{
					const FVector2D Dir(Viewer->X - A.X, Viewer->Y - A.Y);
					Pull = FMath::Min(Pull, FMath::Max(0.0, PullClearance(A, Dir / D) - PullWallMarginCm));
				}
				K = 1.0 - Pull / D;
				Lift = FMath::Max(0.0, K * (H + Foot) - H);
				Want = *Viewer + (Want - *Viewer) * K;
			}
		}
		Want.Z += Lift;
	}
	// moved only past 2 mm or a thousandth of scale: a swaying head would otherwise send every standing sprite's
	// transform to the renderer every frame
	if (bQuadMoved && FVector::DistSquared(Quad->GetComponentLocation(), Want) < 0.04 && FMath::Abs(Quad->GetRelativeScale3D().X - K) < 0.001)
	{
		return;
	}
	Quad->SetRelativeScale3D(FVector(K));
	Quad->SetWorldLocation(Want);
	bQuadMoved = true;
}

double AOutlawsSpriteActor::DrawnHeightCm() const
{
	const double Cell = FMath::Abs(GetActorScale3D().Z) * 0.97;
	if (Chors.IsValidIndex(PlayingChor) && Chors[PlayingChor].HeightPx > 0)
	{
		return FMath::Min(Cell, Chors[PlayingChor].HeightPx / 16.0 * FOutlawsGeometry::UnitsToCm);
	}
	return Cell;
}

bool AOutlawsSpriteActor::DrawnTouches(const FVector& At, double Reach) const
{
	// (BUG-SHOVELWHACK, user 2026-10-27: a shovel swung at a bad guy's head did nothing; his body's capsule is 180 cm tall,
	// his picture up to 220) the quad's foot and scale as PlaceQuad left them (its place when it stands unmoved), across
	// from the drawing's middle along the actor's Y, the height from the insertion point on the floor
	if (IsHidden() || !Quad || !Quad->IsVisible()) return false;
	const FVector Sc = GetActorScale3D();
	const double K = FMath::Max(0.05, double(Quad->GetRelativeScale3D().X));
	const FVector Rel = At - Quad->GetComponentLocation();
	const double Across = FMath::Abs(FVector::DotProduct(Rel, GetActorRightVector()));
	const double Depth = FMath::Abs(FVector::DotProduct(Rel, GetActorForwardVector()));
	const double Up = At.Z - (GetActorLocation().Z - FloorGapCm);
	return Across <= FMath::Abs(Sc.Y) * K * 0.42 && Depth <= Reach && Up >= -10.0 && Up <= DrawnHeightCm() * K;
}

double AOutlawsSpriteActor::PullClearance(const FVector& At, const FVector2D& Dir)
{
	// the level's walls (world static) toward the eye, at knee height over the floor under the insertion point; the
	// answer kept until the place moves 5 cm or the bearing 2 degrees (a far sprite's bearing hardly moves)
	if (PullClearCm >= 0.0 && FVector::DistSquared(At, PullClearAt) < 25.0 && FVector2D::DotProduct(Dir, PullClearDir) > 0.9994)
	{
		return PullClearCm;
	}
	PullClearAt = At;
	PullClearDir = Dir;
	PullClearCm = MaxFootPullCm + PullWallMarginCm;
	UWorld* W = GetWorld();
	if (!W) return PullClearCm;
	const FVector Start = At + FVector(0.0, 0.0, 40.0);
	const FVector End = Start + FVector(Dir.X, Dir.Y, 0.0) * PullClearCm;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsSpritePull), false, this);
	FHitResult Hit;
	if (W->LineTraceSingleByObjectType(Hit, Start, End, FCollisionObjectQueryParams(ECC_WorldStatic), Q))
	{
		PullClearCm = Hit.Distance;
	}
	return PullClearCm;
}

void AOutlawsSpriteActor::PlayChor(int32 Chor, bool bLoop)
{
	PlayingChor = Chors.IsValidIndex(Chor) ? Chor : -1;
	bChorLoop = bLoop;
	bClockHeld = false;
	AnimTime = 0.f;
	CurrentCell = -1;
}

int32 AOutlawsSpriteActor::GetChorFrame() const
{
	if (!Chors.IsValidIndex(PlayingChor))
	{
		return 0;
	}
	// Outlaws chors run at their own rate; 0 fps shows the first frame only
	const float Rate = Chors[PlayingChor].FPS;
	return Rate > 0.f ? static_cast<int32>(AnimTime * Rate) : 0;
}

bool AOutlawsSpriteActor::IsChorDone() const
{
	return Chors.IsValidIndex(PlayingChor) && !bChorLoop && GetChorFrame() >= FMath::Max(1, Chors[PlayingChor].Length);
}

void AOutlawsSpriteActor::TickAnchorShots(UWorld* World, float DeltaSeconds)
{
	// -OLAnchorShots (user 2026-10-02: do the sprites stand on the floor?): a log line per standing sprite, then a camera
	// 35 cm off the floor beside the horses and a few others, one shot each, then quit
	struct FShot { TWeakObjectPtr<AOutlawsSpriteActor> S; float Around = 0.f; FString Name; };
	static TArray<FShot> Shots;
	static TWeakObjectPtr<ACameraActor> Cam;
	static float Clock = 0.f;
	static float Next = 6.f;
	static int32 Step = -1;
	if (!World || !World->IsGameWorld()) return;
	APlayerController* PC = World->GetFirstPlayerController();
	if (!PC) return;
	Clock += DeltaSeconds;
	FString Tag = TEXT("after");
	OLFlags::Value(TEXT("OLAnchorTag"), Tag);

	auto FloorUnder = [World](const FVector& At, double& OutZ)
	{
		FCollisionObjectQueryParams Obj;
		Obj.AddObjectTypesToQuery(ECC_WorldStatic);
		Obj.AddObjectTypesToQuery(ECC_WorldDynamic);
		FHitResult Hit;
		if (!World->LineTraceSingleByObjectType(Hit, At + FVector(0.0, 0.0, 40.0), At - FVector(0.0, 0.0, 400.0), Obj)) return false;
		OutZ = Hit.ImpactPoint.Z;
		return true;
	};
	auto Report = [&FloorUnder](AOutlawsSpriteActor* S, const FVector& Eye)
	{
		if (!S || S->CellPxH <= 0 || !S->CellBelowPx.IsValidIndex(S->CurrentCell) || !S->Quad) return;
		double Floor = 0.0;
		const FVector A = S->GetActorLocation();
		if (!FloorUnder(A, Floor)) return;
		const int32 C = S->CurrentCell;
		const double CmPx = FMath::Abs(S->GetActorScale3D().Z) / S->CellPxH;
		const double K = FMath::Max(0.01, double(S->Quad->GetComponentScale().Z) / FMath::Max(0.01, FMath::Abs(S->GetActorScale3D().Z)));
		const double Qz = S->Quad->GetComponentLocation().Z;
		const double Pad = (S->CellBelowPx[C] - S->CellFootPx[C]) * CmPx;
		// heights as drawn: in the quad (3D), and seen from the eye on the actor's own upright line (the picture the
		// original draws: the quad scaled back about the eye)
		const double Low3d = Qz + Pad * K;
		const double LowSeen = Eye.Z + (Low3d - Eye.Z) / K;
		const double InsSeen = Eye.Z + (Qz + S->CellBelowPx[C] * CmPx * K - Eye.Z) / K;
		UE_LOG(LogOutlaws, Display, TEXT("OLRT anchor %-9s cell %3d foot %2d px: insertion point %+6.1f cm over the floor, lowest drawn row %+6.1f cm (3D %+6.1f), quad x%.3f, eye %.0f cm off"),
			*S->OutlawsName, C, int32(S->CellFootPx[C]), InsSeen - Floor, LowSeen - Floor, Low3d - Floor, K, FVector::Dist2D(Eye, A));
	};

	if (Step == -1)
	{
		if (Clock < 3.f) return;
		FVector Eye = PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : FVector::ZeroVector;
		int32 N = 0;
		for (TActorIterator<AOutlawsSpriteActor> It(World); It; ++It)
		{
			if (It->CellPxH > 0 && It->Quad && It->Quad->IsVisible()) { Report(*It, Eye); ++N; }
		}
		FString Want = TEXT("HOR1,HOR2,HOR3,COW1,CHCKEN,CAMFIRE1,CAMFIRE2,MAN1,WOMAN1,BGY,SANCHEZ,TARAN,WHEEL,FIRE02");
		OLFlags::Value(TEXT("OLAnchorNames"), Want);
		TArray<FString> Names;
		Want.ParseIntoArray(Names, TEXT(","));
		for (const FString& Wn : Names)
		{
			for (TActorIterator<AOutlawsSpriteActor> It(World); It; ++It)
			{
				if (It->CellPxH <= 0 || !It->Quad || !It->Quad->IsVisible() || !It->OutlawsName.StartsWith(Wn)) continue;
				const FString Base = It->OutlawsName.ToLower();
				// the horse from its side and from behind (its widest and its deepest foot), the others from the front
				if (Wn.StartsWith(TEXT("HOR")))
				{
					Shots.Add({ *It, 90.f, Base + TEXT("_side") });
					Shots.Add({ *It, 180.f, Base + TEXT("_back") });
				}
				else
				{
					Shots.Add({ *It, 20.f, Base });
				}
				break;
			}
			if (Shots.Num() >= 9) break;
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLRT anchor shots: %d standing sprites logged, %d shots (%s)"), N, Shots.Num(), *Tag);
		FActorSpawnParameters P;
		P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		Cam = World->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), FTransform::Identity, P);
		if (Cam.IsValid())
		{
			Cam->GetCameraComponent()->SetFieldOfView(70.f);
			Cam->GetCameraComponent()->bConstrainAspectRatio = false;
			PC->SetViewTarget(Cam.Get());
		}
		GEngine->Exec(World, TEXT("r.MotionBlurQuality 0"));
		Step = 0;
		Next = Clock + 0.5f;
		return;
	}
	if (Clock < Next || !Cam.IsValid()) return;
	const int32 k = Step / 2;
	if (k >= Shots.Num())
	{
		FGenericPlatformMisc::RequestExit(false);
		Step = 1 << 20;
		return;
	}
	AOutlawsSpriteActor* S = Shots[k].S.Get();
	if (!S)
	{
		Step += 2;
		return;
	}
	const FVector A = S->GetActorLocation();
	double Floor = A.Z;
	FloorUnder(A, Floor);
	if (Step % 2 == 0)
	{
		// 35 cm over the floor, a little more than the sprite's width off (1.6 .. 3.2 m), on the first clear side
		const double Off = FMath::Clamp(FMath::Abs(S->GetActorScale3D().Y) * 1.1, 160.0, 320.0);
		FCollisionQueryParams Q(TEXT("OLAnchorShot"), false);
		FVector Spot = A + FRotator(0.0, S->GetFacingYaw() + Shots[k].Around, 0.0).Vector() * Off;
		for (int32 d = 0; d < 12; ++d)
		{
			const float Yaw = S->GetFacingYaw() + Shots[k].Around + (d % 2 ? 1.f : -1.f) * 25.f * ((d + 1) / 2);
			const FVector Try = A + FRotator(0.0, Yaw, 0.0).Vector() * Off;
			if (!World->LineTraceTestByChannel(A + FVector(0.0, 0.0, 30.0), FVector(Try.X, Try.Y, Floor + 35.0), ECC_Visibility, Q)) { Spot = Try; break; }
		}
		const FVector Eye(Spot.X, Spot.Y, Floor + 35.0);
		const FVector Look(A.X, A.Y, Floor + 25.0);
		Cam->SetActorLocationAndRotation(Eye, (Look - Eye).Rotation());
		Next = Clock + 0.7f;
	}
	else
	{
		const FVector Eye = Cam->GetActorLocation();
		Cam->SetActorRotation((FVector(A.X, A.Y, Floor + 25.0) - Eye).Rotation());
		Report(S, Eye);
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("ol_anchor_%s_%s.png"), *Shots[k].Name, *Tag), false, false);
		Next = Clock + 0.35f;
	}
	++Step;
}
