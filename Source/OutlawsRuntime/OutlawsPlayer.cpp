#include "OutlawsPlayer.h"
#include "OutlawsFlags.h"
#include "Misc/FileHelper.h"
#include "OutlawsUnlocks.h"
#include "OutlawsAchievements.h"
#include "OutlawsCoopVoicePipe.h"
#include "OutlawsDeathmatch.h"
#include "OutlawsMpPolish.h"
#include "OutlawsFeel.h"
#include "ImageUtils.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "OutlawsPerf.h"
#include "OutlawsDynRes.h"
#include "OutlawsDeviceProfile.h"
#include "OutlawsXRRefresh.h"
#include "RenderCore.h"
#include "DynamicRHI.h"
#include "Misc/App.h"
#include "OutlawsLab.h"
#include "Components/StaticMeshComponent.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/WorldSettings.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Camera/PlayerCameraManager.h"
#include "OutlawsEnemy.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsRuntime.h"
#include "OutlawsBoxHand.h"
#include "OutlawsLevelActor.h"
#include "OutlawsGrabUpscale.h"
#include "OutlawsCoop.h"
#include "OutlawsTextures.h"
#include "Camera/CameraComponent.h"
#include "MotionControllerComponent.h"
#include "LateUpdateManager.h"
#include "HeadMountedDisplayFunctionLibrary.h"
#include "Components/CapsuleComponent.h"
#include "Components/InputComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/CanvasRenderTarget2D.h"
#include "TextureResource.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "TimerManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "ProceduralMeshComponent.h"
#include "UnrealClient.h"
#include "IXRTrackingSystem.h"
#include "IHeadMountedDisplay.h"
#include "OutlawsViewCone.h"
#include "HeadMountedDisplayBase.h"
#include "DefaultSpectatorScreenController.h"
#include "OutlawsShotTests.h"

namespace
{
	constexpr float StandHalfHeight = 87.f;     // 5.8 units tall (Dark Forces PLAYER_HEIGHT)
	constexpr float CrouchHalfHeight = 42.f;    // 2.8 units: through 3-unit crawl spaces
	// (user 2026-10-03, BUG-BOXGAP: slipped past townex's SHOVE_ME box, a 2.5-unit / 75 cm gap at the scope room's door)
	// 80 cm across: closes that gap and stays well under the Jedi engine's own player (PLAYER_WIDTH 1.8 units, 54 cm), so
	// every way the original's body fitted through still fits
	constexpr float BodyRadius = 40.f;
	constexpr float StandEye = 160.f;           // desktop eye above the feet
	constexpr float CrouchEye = 70.f;
}

namespace
{
	// hands, drops and ledges look at the world only: a bad guy is never a ledge or a shelf
	FCollisionResponseParams PlayerNoPawns()
	{
		FCollisionResponseParams RP;
		RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
		return RP;
	}
}

AOutlawsPlayer::AOutlawsPlayer(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	PrimaryActorTick.bCanEverTick = true;
	bUseControllerRotationPitch = false;
	bUseControllerRotationRoll = false;
	bUseControllerRotationYaw = false;
	GetCapsuleComponent()->InitCapsuleSize(BodyRadius, StandHalfHeight);
	HandVelocity[0] = HandVelocity[1] = FVector::ZeroVector;
	PrevHandPos[0] = PrevHandPos[1] = FVector::ZeroVector;
	HandWorldVel[0] = HandWorldVel[1] = FVector::ZeroVector;
	PrevHandWorld[0] = PrevHandWorld[1] = FVector::ZeroVector;

	UCharacterMovementComponent* M = GetCharacterMovement();
	M->bOrientRotationToMovement = false;
	M->NavAgentProps.bCanCrouch = true;
	M->SetCrouchedHalfHeight(CrouchHalfHeight);
	M->MaxStepHeight = StepHeight;
	M->SetWalkableFloorAngle(75.f);          // every Outlaws sloped floor is walkable (steepest authored: 72 deg, ranch; the hideout barn ramp 62)
	M->BrakingDecelerationWalking = 1600.f;
	M->BrakingDecelerationFlying = 600.f;
	M->AirControl = 0.5f;

	// the play-space floor sits at the bottom of the capsule (PlaceOrigin keeps it there)
	VROrigin = CreateDefaultSubobject<USceneComponent>(TEXT("VROrigin"));
	VROrigin->SetupAttachment(GetCapsuleComponent());
	VROrigin->SetRelativeLocation(FVector(0.0, 0.0, -StandHalfHeight));

	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(VROrigin);
	Camera->bLockToHmd = true;
	Camera->SetRelativeLocation(FVector(0.0, 0.0, StandEye));

	auto MakeController = [this](const TCHAR* Name, const TCHAR* Source)
	{
		UMotionControllerComponent* C = CreateDefaultSubobject<UMotionControllerComponent>(Name);
		C->SetupAttachment(VROrigin);
		C->MotionSource = FName(Source);
		return C;
	};
	LeftGrip = MakeController(TEXT("LeftGrip"), TEXT("LeftGrip"));
	RightGrip = MakeController(TEXT("RightGrip"), TEXT("RightGrip"));
	LeftAim = MakeController(TEXT("LeftAim"), TEXT("LeftAim"));
	RightAim = MakeController(TEXT("RightAim"), TEXT("RightAim"));

	LeftHandFrame = CreateDefaultSubobject<USceneComponent>(TEXT("LeftHandFrame"));
	LeftHandFrame->SetupAttachment(LeftGrip);
	RightHandFrame = CreateDefaultSubobject<USceneComponent>(TEXT("RightHandFrame"));
	RightHandFrame->SetupAttachment(RightGrip);
	LeftHand = CreateDefaultSubobject<UOutlawsBoxHand>(TEXT("LeftHand"));
	LeftHand->SetupAttachment(LeftHandFrame);
	LeftHand->bLeft = true;
	RightHand = CreateDefaultSubobject<UOutlawsBoxHand>(TEXT("RightHand"));
	RightHand->SetupAttachment(RightHandFrame);

	WristText = CreateDefaultSubobject<UTextRenderComponent>(TEXT("WristText"));
	WristText->SetupAttachment(VROrigin);
	WristText->SetHorizontalAlignment(EHTA_Center);
	WristText->SetVerticalAlignment(EVRTA_TextCenter);
	WristText->SetWorldSize(1.4f);
	WristText->SetTextRenderColor(FColor(235, 220, 180));
	WristText->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	WristText->SetCastShadow(false);
	WristText->SetVisibility(false);
}

void AOutlawsPlayer::BeginPlay()
{
	Super::BeginPlay();
	// unset or missing: under the local item folder (a dev PC's, or the headset's copy under the game's files folder)
	if (PlaceholderWeaponDir.IsEmpty() || !FPaths::DirectoryExists(PlaceholderWeaponDir))
	{
		PlaceholderWeaponDir = FPaths::Combine(FOutlawsAssets::PlaceholderDir(), TEXT("ve_weapons/hd_xint"));
	}
	FallGraceUntil = 5.0;                // the level start drops the player onto the floor
	bVR = UHeadMountedDisplayFunctionLibrary::IsHeadMountedDisplayEnabled();
	{
		// (BUG-FOVCLIP) the near plane and the eyes' cone, once the headset has its views and the wrist its parts
		FTimerHandle ClipLog;
		GetWorldTimerManager().SetTimer(ClipLog, FTimerDelegate::CreateLambda([]() { FOutlawsViewCone::LogClipDiagnostics(); }), 5.f, false);
	}
	if (bVR)
	{
		UHeadMountedDisplayFunctionLibrary::SetTrackingOrigin(EHMDTrackingOrigin::Floor);
	}
	ApplyModeSettings();
	FindLevel();
	LoadGrips();                         // (BUG-GRIPSAVE: the grip editor's placements, over the baked grips)
	if (bVR)
	{
		BuildHands();
		RouteAudioToHeadset();
	}
	else if (OLFlags::Param(TEXT("OLHandTest")) || OLFlags::Param(TEXT("OLGearTest"))
		|| OLFlags::Param(TEXT("OLHeldGunTest")) || OLFlags::Param(TEXT("OLGripTest")) || OLFlags::Param(TEXT("OLFlash3DTest"))
		|| OLFlags::Param(TEXT("OLKnifeTest")) || OLFlags::Param(TEXT("OLFanTest"))
		|| OLFlags::Param(TEXT("OLTntTest")) || OLFlags::Param(TEXT("OLTntDeathTest")) || OLFlags::Param(TEXT("OLArcStuckTest")) || OLFlags::Param(TEXT("OLRemoteFxTest")) || OLFlags::Param(TEXT("OLLanternSwimTest"))
		|| OLFlags::Param(TEXT("OLFlickTest")) || OLFlags::Param(TEXT("OLFlickNetTest"))
		|| OLFlags::Param(TEXT("OLCharLoadoutTest")) || (OLFlags::Has(TEXT("OLTreadTest"))) || OLFlags::Has(TEXT("OLVb2Test"))
		|| OLFlags::Has(TEXT("OLWaterHandsTest")) || OLFlags::Has(TEXT("OLWheelTopTest"))
		|| OLFlags::Param(TEXT("OLLevelCarryTest")) || OLFlags::ValueIs(TEXT("OLCoopTest"), TEXT("levelcarry")))
	{
		BuildHands();                    // the desktop hand pictures
	}
	LeftHand->SetVisibility(bVR, true);
	RightHand->SetVisibility(bVR, true);
	// the engine's text materials are lit and this world has no lights: use our unlit one
	if (UMaterialInterface* TextMat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_Text.M_OLR_Text")))
	{
		WristText->SetTextMaterial(TextMat);
	}
	if (OLFlags::Param(TEXT("OLSmokeTest")))
	{
		SmokeTime = 0.f;
	}
	if (OLFlags::Param(TEXT("OLShots")))
	{
		ShotTime = 0.f;
	}
	LoadPlayerSettings();
	UE_LOG(LogOutlaws, Log, TEXT("OLRT player ready: %s, level %s"), bVR ? TEXT("VR") : TEXT("desktop"), Level.IsValid() ? TEXT("found") : TEXT("missing"));
	FOutlawsViewCone::Get(true);     // (BUG-FOVCLIP) the view cone our own in-view tests use, and where it came from
}

void AOutlawsPlayer::ApplyModeSettings()
{
	UCharacterMovementComponent* M = GetCharacterMovement();
	M->MaxStepHeight = StepHeight;
	M->MaxFlySpeed = SwimSpeedFraction * (bVR ? VRWalkSpeed : DesktopWalkSpeed);
	M->MaxAcceleration = bVR ? VRAcceleration : Acceleration;
	M->BrakingDecelerationWalking = 3000.f;
	if (bVR)
	{
		M->GravityScale = BaseGravityScale = 1.5f;
		M->JumpZVelocity = VRJumpVelocity;
		M->MaxWalkSpeedCrouched = VRCrouchSpeed;
		bUseControllerRotationYaw = false;
		Camera->bUsePawnControlRotation = false;
	}
	else
	{
		M->GravityScale = BaseGravityScale = DesktopGravityScale;
		M->JumpZVelocity = DesktopJumpVelocity;
		M->MaxWalkSpeedCrouched = DesktopCrouchSpeed;
		bUseControllerRotationYaw = true;
		Camera->bUsePawnControlRotation = true;
	}
}

void AOutlawsPlayer::FindLevel()
{
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AOutlawsLevelActor> It(World); It; ++It)
		{
			Level = *It;
			return;
		}
	}
}

UMaterialInterface* AOutlawsPlayer::SolidMaterial(const FColor& Colour)
{
	// the level's unlit world material with a flat colour texture: the scene has no lights
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_World.M_OLR_World"));
	if (!Base)
	{
		return nullptr;
	}
	TArray<FColor> Pixels;
	Pixels.Init(Colour, 4);
	UTexture2D* T = OutlawsTextures::MakeTextureBGRA(2, 2, Pixels, false, TF_Nearest, TA_Clamp);
	UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(Base, this);
	if (T)
	{
		M->SetTextureParameterValue(TEXT("Tex"), T);
		KeepAlive.Add(T);
	}
	KeepAlive.Add(M);
	return M;
}

float AOutlawsPlayer::RegenRate() const
{
	// REQ-REGEN2X: in co-op and multiplayer the host's rate (the session carries it), alone this build's
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this))
	{
		if (C->IsClient())
		{
			if (const AOutlawsCoopSession* S = C->GetSession())
			{
				if (S->RegenRate >= 0.f) return S->RegenRate;
			}
		}
	}
	return OutlawsFeel::RegenPerSecond;
}

void AOutlawsPlayer::UpdateStamina(float Dt)
{
	OL_PERF_SCOPE("Player: stamina");
	UCharacterMovementComponent* M = GetCharacterMovement();
	const bool bMovingNow = GetVelocity().Size2D() > 60.f;
	const bool bCrouched = GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() < 60.f;
	if (!bDeadView && !bMenuOpen)
	{
		// recovery at half the exe's rates (user 2026-09-27): E_REST 11, walking half that, E_CROUCH 2 more
		float Rate = 5.5f;
		// running drains half by the second and half by the ground covered (user 2026-10-01: turned slow it winded too soon,
		// 30 % a drag): the speed setting against 50 %, where it felt right and drains as before (E_MOVE 10 at a quarter)
		const float SpeedMix = 0.5f + 0.5f * (Level.IsValid() ? Level->Pace() : 0.5f) / 0.5f;
		if (bRunning && bMovingNow && M->IsMovingOnGround()) Rate = -2.5f * SpeedMix * (CarryingMan() ? CarryDrain : 1.f);
		else if (bMovingNow) Rate = 2.75f;
		if (bCrouched && Rate > 0.f) Rate += 1.f;
		Stamina = FMath::Clamp(Stamina + Rate * Dt, 0.f, 100.f);
	}
	if (bDeadView) Stamina = 100.f;
	// below half health it comes back up to half: standing still 2 % a second (REQ-REGEN2X, user 2026-10-11: was 1 %),
	// walking a quarter of that, running none; crouched, twice as fast (user 2026-10-01)
	if (AOutlawsLevelActor* LH = Level.Get())
	{
		const bool bStill = GetVelocity().Size() < 20.f;
		const bool bWalking = !bStill && !(bRunning && bMovingNow) && M->IsMovingOnGround();
		const float Pace = (bStill ? 1.f : bWalking ? 0.25f : 0.f) * (bCrouched ? 2.f : 1.f);
		if (!bDeadView && !bMenuOpen && Pace > 0.f && !LH->IsPlayerDead() && LH->GetHealth() < OutlawsFeel::RegenCap)
		{
			LH->HealPlayer(FMath::Min(RegenRate() * Pace * Dt, OutlawsFeel::RegenCap - LH->GetHealth()));
		}
	}
	// tired.wav from the first point spent: faint at 99, full at 0 (user 2026-09-27)
	AOutlawsLevelActor* L = Level.Get();
	const float Want = Stamina < 99.f && !bDeadView && !bMenuOpen ? FMath::Pow((99.f - Stamina) / 99.f, 1.5f) : 0.f;
	// a breath every 2 seconds (user 2026-09-27: the loop ran them together)
	if (Want > 0.f && L && !L->IsFrontEnd())
	{
		TiredWait -= Dt;
		if (TiredWait <= 0.f)
		{
			TiredWait = 2.f;
			TiredSound = L->PlayOnce2D(TEXT("tired"));
			if (TiredSound.IsValid()) TiredSound->SetVolumeMultiplier(FMath::Max(0.04f, Want) * L->SoundVolume * L->MasterVolume);
		}
	}
	else
	{
		TiredWait = 0.f;
	}
}

void AOutlawsPlayer::UpdateBreath(float Dt)
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L || bDeadView || bMenuOpen || L->IsMoviePlaying() || L->IsPlayerDead())
	{
		if (bDeadView) { Air = 1.f; DrownWait = 0.f; bHeadUnder = false; }
		UpdateBreathVeil(bMenuOpen && !bDeadView ? -1.f : 0.f);      // (held as it was under the menu)
		return;
	}
	FLinearColor Water;
	const bool bWasUnder = bHeadUnder;
	bHeadUnder = L->UnderwaterTintAt(Camera->GetComponentLocation(), Water);
	// 30 s at the slider's 100 % (Pace 0.5); slower, longer in step with it, so a breath swims as far (user 2026-10-01)
	const float Hold = 30.f * 0.5f / FMath::Max(0.05f, L->Pace());
	// up for air with 10 s of it left or less, or drowning (user 2026-10-03, GULP): the game's own gasp.wav, the breath
	// caught (the user may pick another later)
	if (bWasUnder && !bHeadUnder && Air * Hold <= 10.f)
	{
		L->PlayOnce2D(TEXT("gasp"));
		UE_LOG(LogOutlaws, Log, TEXT("OLRT surfaced with %.1f s of air left: a gasp"), Air * Hold);
	}
	if (!bHeadUnder)
	{
		Air = FMath::Min(1.f, Air + Dt / 4.f);           // (back up in 4 s of air)
		DrownWait = 0.f;
	}
	else
	{
		Air = FMath::Max(0.f, Air - Dt / Hold);
	}
	// the dark closing in as the air goes (user 2026-10-01): faint at first, pulsing quicker and deeper toward the end; out of
	// air it stays near its darkest, darker still as health goes
	{
		const float Need = 1.f - Air;
		BreathPhase += Dt * UE_TWO_PI * FMath::Lerp(0.5f, 1.6f, Need);
		const float Beat = 0.5f + 0.5f * FMath::Sin(BreathPhase);
		const float Base = Air > 0.f ? 0.8f * FMath::Pow(Need, 1.6f) : FMath::Lerp(0.8f, 0.97f, FMath::Clamp(1.f - L->GetHealth() / 10.f, 0.f, 1.f));
		UpdateBreathVeil(Base * FMath::Lerp(0.65f, 1.f, Beat));
	}
	if (!bHeadUnder || Air > 0.f) return;
	// out of air: a tenth of health now and every 5 s after, choking, the red over the water's blue
	DrownWait -= Dt;
	if (DrownWait <= 0.f)
	{
		DrownWait = 5.f;
		L->DrownPlayer();
	}
}

void AOutlawsPlayer::Landed(const FHitResult& Hit)
{
	Super::Landed(Hit);
	// how hard: the landing speed as the height it would take to reach it under this game's normal
	// gravity (a low-gravity sector lands softer). Up to 8 m costs nothing; past it the damage builds to
	// the whole of health (10) at about 24 m. Falls skip the armour, as environmental damage did in Dark
	// Forces. Sounds are the character's own (cid*.itm SND_LANDLT / SND_LAND / SND_SPLAT / SND_CRUNCH;
	// which one goes with which landing is INFERRED from their names)
	AOutlawsLevelActor* L = Level.Get();
	const float Down = FMath::Max(0.f, float(-GetCharacterMovement()->Velocity.Z));
	const float G = FMath::Max(1.f, FMath::Abs(GetWorld()->GetGravityZ()) * BaseGravityScale);
	const float Drop = Down * Down / (2.f * G);                 // cm
	if (!L || bDeadView || Drop < 10.f)
	{
		return;
	}
	const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	constexpr float SafeCm = 800.f, LethalCm = 2400.f;   // twice as far before it hurts, half the damage a metre after (user 2026-09-27)
	const bool bGrace = GetWorld()->GetTimeSeconds() < FallGraceUntil;
	const float Damage = bGrace || Drop <= SafeCm ? 0.f : 10.f * (Drop - SafeCm) / (LethalCm - SafeCm);
	if (Damage <= 0.f)
	{
		L->PlaySoundAt(Drop < 500.f ? TEXT("landlite") : TEXT("land"), Feet);   // SND_LANDLT after a jump, SND_LAND from over 5 m (user 2026-09-27)
		return;
	}
	const bool bLethal = Damage >= L->GetHealth() && !L->bInvincible;
	L->PlaySoundAt(TEXT("crunch"), Feet);        // any fall that hurts (user 2026-09-27)
	(void)bLethal;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT fall: %.0f cm/s down (a %.1f m drop): %.1f damage"), Down, Drop / 100.f, Damage);
	L->HurtCause = uint8(EOutlawsHurtCause::Fall);       // (REQ-KILLFEED)
	L->DamagePlayer(Damage, 1.f, true);
	Pulse(0, 0.6f);
	Pulse(1, 0.6f);
}

UMaterialInterface* AOutlawsPlayer::HeldBase() const
{
	UMaterialInterface* Held = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_Held.M_OLR_Held"));
	return Held ? Held : LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_World.M_OLR_World"));
}

UMaterialInterface* AOutlawsPlayer::HeldSolidMaterial(const FColor& Colour)
{
	UMaterialInterface* Base = HeldBase();
	if (!Base)
	{
		return nullptr;
	}
	TArray<FColor> Pixels;
	Pixels.Init(Colour, 4);
	UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(Base, this);
	if (UTexture2D* T = OutlawsTextures::MakeTextureBGRA(2, 2, Pixels, false, TF_Nearest, TA_Clamp))
	{
		M->SetTextureParameterValue(TEXT("Tex"), T);
		KeepAlive.Add(T);
	}
	HeldMids.Add(M);
	return M;
}

void AOutlawsPlayer::UpdateHeldLight(float Dt)
{
	OL_PERF_SCOPE("Player: held light");
	// the light where the head is (sectors are big, the hands never far from it), on the enemies'
	// curve (light / 31) ^ 2.2, eased over a quarter second so a doorway is not a pop
	const AOutlawsLevelActor* L = Level.Get();
	if (!L || HeldMids.Num() == 0)
	{
		return;
	}
	const float Want = FMath::Max(0.04f, FMath::Pow(FMath::Clamp(L->LightAt(Camera->GetComponentLocation()), 0, 31) / 31.f, 2.2f));
	const float Next = HeldLight < 0.f ? Want : FMath::FInterpTo(HeldLight, Want, float(FApp::GetDeltaTime()), 8.f);
	// (BUG-WATERHANDS) a hand under the water with the eyes over it: that hand and what it holds dim as the water dims what
	// is under it (the surface draws over them too: SetHandWater); eased over a tenth of a second
	bool bDimMoved = false;
	for (int32 H = 0; H < 2; ++H)
	{
		const float D = FMath::FInterpTo(HandWaterDim[H], HandWaterWant[H], float(FApp::GetDeltaTime()), 12.f);
		const float Dn = FMath::Abs(D - HandWaterWant[H]) < 0.01f ? HandWaterWant[H] : D;
		if (FMath::Abs(Dn - HandWaterDim[H]) > 0.0005f) { HandWaterDim[H] = Dn; bDimMoved = true; }
	}
	if (FMath::Abs(Next - HeldLight) < 0.002f && !bDimMoved)
	{
		return;
	}
	HeldLight = Next;
	for (UMaterialInstanceDynamic* M : HeldMids)
	{
		if (M) M->SetScalarParameterValue(TEXT("Brightness"), HeldLight);
	}
	for (int32 H = 0; H < 2; ++H)
	{
		if (HandWaterDim[H] > 0.999f || !GripOf(H)) continue;
		TArray<USceneComponent*> Parts;
		GripOf(H)->GetChildrenComponents(true, Parts);
		for (USceneComponent* C : Parts)
		{
			UPrimitiveComponent* P = Cast<UPrimitiveComponent>(C);
			if (!P) continue;
			for (int32 i = 0; i < P->GetNumMaterials(); ++i)
			{
				UMaterialInstanceDynamic* M = Cast<UMaterialInstanceDynamic>(P->GetMaterial(i));
				if (M && HeldMids.Contains(M)) M->SetScalarParameterValue(TEXT("Brightness"), HeldLight * HandWaterDim[H]);
			}
		}
	}
}

void AOutlawsPlayer::BuildHands()
{
	// the cutscene-style hands (user 2026-09-28): flat colours on the held material, lit by the sector like the guns
	auto Make = [this](const FColor& C)
	{
		UMaterialInterface* M = HeldSolidMaterial(C);
		if (UMaterialInstanceDynamic* Mid = Cast<UMaterialInstanceDynamic>(M)) Mid->SetScalarParameterValue(TEXT("Cel"), 0.f);   // lit / shade are in the colours
		return M;
	};
	LeftHand->Build(Make);
	RightHand->Build(Make);
	NailsWorn = 0;
	if (const AOutlawsLevelActor* L = Level.Get()) WearNails(L->WornCharacter);     // (REQ-MARYNAILS: the kit came first)
}

void AOutlawsPlayer::WearNails(const FString& Short)
{
	// REQ-MARYNAILS: the nails are their own section of the hand mesh with their own flat colour, so a character's polish is
	// that section's colour and nothing more (no texture, no new material: the held one, a 2x2 colour as every hand part)
	const FOutlawsMpCharacter* Ch = Short.IsEmpty() ? nullptr : FOutlawsMpCharacter::Find(Short);
	const uint32 Rgb = Ch && Ch->NailRGB ? Ch->NailRGB : 0u;
	const uint32 Key = Rgb | 0x1000000u;                 // (0 is "not yet")
	if (Key == NailsWorn || !LeftHand || !RightHand || !LeftHand->HasMesh()) return;
	NailsWorn = Key;
	const FColor C = Rgb ? FColor(uint8(Rgb >> 16), uint8(Rgb >> 8), uint8(Rgb)) : UOutlawsBoxHand::NaturalNail();
	UMaterialInterface* M = HeldSolidMaterial(C);
	if (UMaterialInstanceDynamic* Mid = Cast<UMaterialInstanceDynamic>(M))
	{
		Mid->SetScalarParameterValue(TEXT("Cel"), 0.f);
		if (HeldLight >= 0.f) Mid->SetScalarParameterValue(TEXT("Brightness"), HeldLight);
	}
	LeftHand->SetNailMaterial(M);
	RightHand->SetNailMaterial(M);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT nails: %s (%s)"), Rgb ? *FString::Printf(TEXT("#%06X"), Rgb) : TEXT("the hands' own"), Ch ? Ch->Name : TEXT("no character"));
}

// ---- input ---------------------------------------------------------------------------------------

void AOutlawsPlayer::SetupPlayerInputComponent(UInputComponent* PIC)
{
	Super::SetupPlayerInputComponent(PIC);
	// legacy axis/action mappings (Config/DefaultInput.ini): OpenXR in 5.1 turns these into its
	// actions when no Enhanced Input config is set for XR
	for (const TCHAR* A : { TEXT("OLVR_LeftX"), TEXT("OLVR_LeftY"), TEXT("OLVR_RightX"), TEXT("OLVR_RightY"),
		TEXT("OLVR_LeftTrigger"), TEXT("OLVR_RightTrigger"), TEXT("OLVR_LeftGrip"), TEXT("OLVR_RightGrip") })
	{
		PIC->BindAxis(FName(A));
	}
	PIC->BindAction(TEXT("OLVR_A"), IE_Pressed, this, &AOutlawsPlayer::PressA);
	PIC->BindAction(TEXT("OLVR_A"), IE_Released, this, &AOutlawsPlayer::PressAReleased);
	PIC->BindAction(TEXT("OLVR_X"), IE_Released, this, &AOutlawsPlayer::PressXReleased);
	PIC->BindAction(TEXT("OLVR_B"), IE_Pressed, this, &AOutlawsPlayer::PressB);
	PIC->BindAction(TEXT("OLVR_X"), IE_Pressed, this, &AOutlawsPlayer::PressX);
	PIC->BindAction(TEXT("OLVR_Y"), IE_Pressed, this, &AOutlawsPlayer::PressY);
	PIC->BindAction(TEXT("OLVR_B"), IE_Released, this, &AOutlawsPlayer::PressBReleased);
	PIC->BindAction(TEXT("OLVR_Y"), IE_Released, this, &AOutlawsPlayer::PressYReleased);
	PIC->BindAction(TEXT("OLVR_Menu"), IE_Pressed, this, &AOutlawsPlayer::PressMenu);
	PIC->BindAction(TEXT("OLVR_LeftStick"), IE_Pressed, this, &AOutlawsPlayer::PressLeftStick);
	PIC->BindAction(TEXT("OLVR_RightStick"), IE_Pressed, this, &AOutlawsPlayer::PressRightStick);
	PIC->BindAction(TEXT("OLVR_LeftStick"), IE_Released, this, &AOutlawsPlayer::ReleaseLeftStick);
	PIC->BindAction(TEXT("OLVR_RightStick"), IE_Released, this, &AOutlawsPlayer::ReleaseRightStick);
	// desktop
	PIC->BindAxis(TEXT("OLD_Forward"), this, &AOutlawsPlayer::DesktopForward);
	PIC->BindAxis(TEXT("OLD_Right"), this, &AOutlawsPlayer::DesktopRight);
	PIC->BindAxis(TEXT("OLD_Turn"), this, &AOutlawsPlayer::DesktopTurn);
	PIC->BindAxis(TEXT("OLD_LookUp"), this, &AOutlawsPlayer::DesktopLookUp);
	PIC->BindAction(TEXT("OLD_Jump"), IE_Pressed, this, &AOutlawsPlayer::DoJump);
	PIC->BindAction(TEXT("OLD_Crouch"), IE_Pressed, this, &AOutlawsPlayer::CrouchPressed);
	PIC->BindAction(TEXT("OLD_Crouch"), IE_Released, this, &AOutlawsPlayer::CrouchReleased);
	PIC->BindAction(TEXT("OLD_Run"), IE_Pressed, this, &AOutlawsPlayer::RunPressed);
	PIC->BindAction(TEXT("OLD_Run"), IE_Released, this, &AOutlawsPlayer::RunReleased);
	// (REQ-BUGSNAP) F9: a bug snapshot, as the headset's both triggers and both stick clicks (F9 was the ninth quick taunt)
	PIC->BindKey(EKeys::F9, IE_Pressed, this, &AOutlawsPlayer::BugSnapKey);
#if !PLATFORM_ANDROID
	// (REQ-SPECCAM) the PC keyboard's V cycles the PC window's view (Headset, Steady); never a controller button
	PIC->BindKey(EKeys::V, IE_Pressed, this, &AOutlawsPlayer::CycleSpecView);
	// (REQ-TRIGGERSTEP) a desktop menu: the left button steps a value row up, the right down (nothing outside a menu)
	PIC->BindKey(EKeys::LeftMouseButton, IE_Pressed, this, &AOutlawsPlayer::MenuMouseLeft);
	PIC->BindKey(EKeys::RightMouseButton, IE_Pressed, this, &AOutlawsPlayer::MenuMouseRight);
#endif
}

float AOutlawsPlayer::Axis(const TCHAR* Name) const
{
	// (REQ-BUGSNAP) the bug snapshot's chord forming: the triggers read let go to everything (guns, throws, menus, skips)
	if (bBugChord && (FCString::Strcmp(Name, TEXT("OLVR_LeftTrigger")) == 0 || FCString::Strcmp(Name, TEXT("OLVR_RightTrigger")) == 0)) return 0.f;
	return RawAxis(Name);
}

FVector2D AOutlawsPlayer::MoveStick() const
{
	if (HeldStickLeft > 0.f)
	{
		return HeldStick;
	}
	if (!bVR)
	{
		return FVector2D::ZeroVector;
	}
	if (OneHand() >= 0)
	{
		// one controller (REQ-ONECTRL): its stick walks and strafes, except a hard sideways push, which turns (TurnStick)
		const FVector2D S = OneStick();
		return OneStickTurning(S) ? FVector2D::ZeroVector : S;
	}
	return LeftDominant() ? FVector2D(Axis(TEXT("OLVR_RightX")), Axis(TEXT("OLVR_RightY")))
		: FVector2D(Axis(TEXT("OLVR_LeftX")), Axis(TEXT("OLVR_LeftY")));
}

FVector2D AOutlawsPlayer::OneStick() const
{
	return OneHand() == 0 ? FVector2D(Axis(TEXT("OLVR_LeftX")), Axis(TEXT("OLVR_LeftY"))) : FVector2D(Axis(TEXT("OLVR_RightX")), Axis(TEXT("OLVR_RightY")));
}

bool AOutlawsPlayer::OneStickTurning(const FVector2D& S)
{
	// pushed hard to the side (three quarters out, and well more sideways than forward): a turn, not a strafe
	return FMath::Abs(S.X) > 0.75f && FMath::Abs(S.X) > 1.5f * FMath::Abs(S.Y);
}

FVector2D AOutlawsPlayer::TurnStick() const
{
	if (OneHand() >= 0)
	{
		// one controller: only the hard sideways push turns; no crouch or jump on the stick (the face button jumps, the
		// body ducks for real)
		const FVector2D S = OneStick();
		return OneStickTurning(S) ? FVector2D(S.X, 0.f) : FVector2D::ZeroVector;
	}
	return LeftDominant() ? FVector2D(Axis(TEXT("OLVR_LeftX")), Axis(TEXT("OLVR_LeftY")))
		: FVector2D(Axis(TEXT("OLVR_RightX")), Axis(TEXT("OLVR_RightY")));
}

void AOutlawsPlayer::HoldMoveStick(FVector2D Stick, float Seconds)
{
	HeldStick = Stick;
	HeldStickLeft = Seconds;
}

void AOutlawsPlayer::SetCrouchHeld(bool bHeld)
{
	bCrouchHeld = bHeld;
}

void AOutlawsPlayer::DesktopForward(float V)
{
	if (bVR || V == 0.f || IsClimbing() || MantleLeft > 0.f) return;
	bDesktopMoving = true;
	FRotator R = GetControlRotation();
	R.Roll = 0.0;
	if (!bSwimming || bSurfaceLocked) R.Pitch = 0.0;   // in water, swim where you look (along the surface while it holds the body)
	AddMovementInput(R.Vector(), V);
}

void AOutlawsPlayer::DesktopRight(float V)
{
	if (bVR || V == 0.f || IsClimbing() || MantleLeft > 0.f) return;
	bDesktopMoving = true;
	AddMovementInput(FRotator(0.0, GetControlRotation().Yaw + 90.0, 0.0).Vector(), V);
}

void AOutlawsPlayer::DesktopTurn(float V)
{
	if (!bVR && V != 0.f) AddControllerYawInput(V);
}

void AOutlawsPlayer::DesktopLookUp(float V)
{
	if (!bVR && V != 0.f) AddControllerPitchInput(V);
}

void AOutlawsPlayer::CrouchPressed()
{
	if (bSwimming) { SwimKick(-1.f); return; }      // under water crouch dives
	bCrouchHeld = true;
	CrouchAmount = 1.f;
}

void AOutlawsPlayer::SwimKick(float Dir)
{
	// under water a jump kicks the body up, a crouch down (user 2026-09-27); the water slows it (UpdateGravity)
	UCharacterMovementComponent* M = GetCharacterMovement();
	M->Velocity.Z = Dir > 0.f ? FMath::Max(float(M->Velocity.Z), 0.f) + 380.f : FMath::Min(float(M->Velocity.Z), 0.f) - 380.f;
	M->Velocity.Z = FMath::Clamp(float(M->Velocity.Z), -700.f, 700.f);
	if (Dir < 0.f) DiveUntil = GetWorld()->GetTimeSeconds() + 1.0;     // a kick down from the surface: it lets go (UpdateGravity)
	SwimLog(Dir > 0.f ? TEXT("a kick up") : TEXT("a kick down"));
}
void AOutlawsPlayer::CrouchReleased() { bCrouchHeld = false; }
void AOutlawsPlayer::RunPressed() { bRunning = true; }
void AOutlawsPlayer::RunReleased() { bRunning = false; }

// X (left) and A (right): hold for that hand's weapon wheel, let go to take the weapon pointed at
// (trial layout, bFaceReload: Y and B reload that hand's gun; swapped from X / A, user 2026-09-27)
void AOutlawsPlayer::PressA()
{
	FaceHeld |= 1;
	if (bDeadView) { RequestRespawn(); return; }
	if (bMenuOpen && MenuPage && MenuPage->File == TEXT("#unlocked")) { UnlockPageContinue(); return; }     // (REQ-UNLOCKS: any button)
	if (bVR && !bMenuOpen) OpenWheel(1);
}
void AOutlawsPlayer::PressX()
{
	FaceHeld |= 4;
	if (bDeadView) { RequestRespawn(); return; }
	if (bMenuOpen && MenuPage && MenuPage->File == TEXT("#unlocked")) { UnlockPageContinue(); return; }     // (REQ-UNLOCKS: any button)
	if (bVR && !bMenuOpen) OpenWheel(0);
}
void AOutlawsPlayer::PressAReleased() { FaceHeld &= ~1; ReleaseWheel(1); }
void AOutlawsPlayer::PressXReleased() { FaceHeld &= ~4; ReleaseWheel(0); }

void AOutlawsPlayer::PressBReleased() { FaceHeld &= ~2; }
void AOutlawsPlayer::PressYReleased() { FaceHeld &= ~8; }

void AOutlawsPlayer::LoadPlayerSettings()
{
	const TMap<FString, FString> S = AOutlawsLevelActor::ReadSettings();
	if (const FString* V = S.Find(TEXT("MovementFollows"))) MoveDirection = *V == TEXT("DominantHand") ? EOutlawsMoveDirection::DominantHand : *V == TEXT("OffHand") ? EOutlawsMoveDirection::OffHand : EOutlawsMoveDirection::Head;
	if (const FString* V = S.Find(TEXT("SnapTurn"))) bSnapTurn = V->ToBool();
	if (const FString* V = S.Find(TEXT("Locomotion"))) bTeleportMove = *V == TEXT("Teleport");
	if (const FString* V = S.Find(TEXT("DominantHand"))) DominantHand = *V == TEXT("Left") ? EOutlawsHand::Left : EOutlawsHand::Right;
	if (const FString* V = S.Find(TEXT("ButtonLayout"))) bFaceReload = *V != TEXT("Classic");
	if (const FString* V = S.Find(TEXT("OneController"))) OneController = *V == TEXT("Left") ? 1 : *V == TEXT("Right") ? 2 : 0;
	// (the tests: -OLOneController=Left|Right|Off for this run, not saved unless a menu row is changed)
	FString OneArg;
	if (OLFlags::Value(TEXT("OLOneController"), OneArg)) OneController = OneArg == TEXT("Left") ? 1 : OneArg == TEXT("Right") ? 2 : 0;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT one controller: %s"), OneController == 1 ? TEXT("Left only") : OneController == 2 ? TEXT("Right only") : TEXT("Off"));
}

void AOutlawsPlayer::SaveSettings()
{
	TMap<FString, FString> S = AOutlawsLevelActor::ReadSettings();
	if (const AOutlawsLevelActor* L = Level.Get()) L->GatherSettings(S);
	S.Add(TEXT("MovementFollows"), MoveDirection == EOutlawsMoveDirection::DominantHand ? TEXT("DominantHand") : MoveDirection == EOutlawsMoveDirection::OffHand ? TEXT("OffHand") : TEXT("Head"));
	S.Add(TEXT("SnapTurn"), bSnapTurn ? TEXT("true") : TEXT("false"));
	S.Add(TEXT("Locomotion"), bTeleportMove ? TEXT("Teleport") : TEXT("Smooth"));
	S.Add(TEXT("DominantHand"), LeftDominant() ? TEXT("Left") : TEXT("Right"));
	S.Add(TEXT("ButtonLayout"), bFaceReload ? TEXT("Trial") : TEXT("Classic"));
	S.Add(TEXT("OneController"), OneController == 1 ? TEXT("Left") : OneController == 2 ? TEXT("Right") : TEXT("Off"));
	AOutlawsLevelActor::WriteSettings(S);
}

void AOutlawsPlayer::GripOnNothing(int32 Hand)
{
	// (user 2026-09-29: the grip on nothing does nothing: the jump is the right stick up, the medical bag and the other
	// items are used from the wheel)
}

void AOutlawsPlayer::DetectBodyJump(float Dt)
{
	// the head (height above the play-space floor) rising faster than 1.5 m/s from about standing
	// height is a jump; standing up from a crouch starts well below it and slows before it gets there
	const float HeadZ = Camera->GetRelativeLocation().Z;
	JumpCooldown = FMath::Max(0.f, JumpCooldown - Dt);
	if (PrevHeadZ < 0.f || Dt <= 0.f)
	{
		PrevHeadZ = StandEyeZ = HeadZ;
		return;
	}
	const float Vz = (HeadZ - PrevHeadZ) / Dt;
	PrevHeadZ = HeadZ;
	// standing height: follows the head up at once, settles down slowly (a crouch does not lower it much)
	StandEyeZ = HeadZ > StandEyeZ ? HeadZ : StandEyeZ - 3.f * Dt;
	// a jump needs its wind-up (user 2026-09-28: a quick lift of the head jumped): the head dips 8+ cm below standing,
	// then comes back up fast to standing within 0.8 s of the dip starting. A dip past 30 cm is a crouch: no jump until
	// the head is back at standing height and dips again; none at all while the stick crouch is on
	const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	const float Below = StandEyeZ - HeadZ;
	if (Below > 8.f && JumpDipStart < 0.0) JumpDipStart = Now;
	if (Below > 30.f) bJumpDipDeep = true;
	if (Below < 3.f && JumpDipStart >= 0.0 && (bJumpDipDeep || Now - JumpDipStart > 0.8))
	{
		JumpDipStart = -1.0;                 // back at standing after a crouch or a slow dip: start over
		bJumpDipDeep = false;
	}
	const bool bWoundUp = JumpDipStart >= 0.0 && !bJumpDipDeep && Now - JumpDipStart <= 0.8;
	if (bWoundUp && Vz > 150.f && Below < 6.f && JumpCooldown <= 0.f && !bCrouchHeld && GetCharacterMovement()->IsMovingOnGround())
	{
		JumpCooldown = 0.8f;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT body jump (head %.0f cm/s after a %.0f ms wind-up)"), Vz, float((Now - JumpDipStart) * 1000.0));
		JumpDipStart = -1.0;
		DoJump();
	}
}

void AOutlawsPlayer::PressB()
{
	FaceHeld |= 2;
	if (bDeadView) { RequestRespawn(true); return; }
	if (bMenuOpen) { MenuBack(); return; }
	if (OneHand() == 1) { DoJump(); return; }    // one controller: its upper button jumps (REQ-ONECTRL)
	if (bFaceReload) { if (Equipped[1] >= 0) bButtonReload[1] = true; return; }
	if (!LeftDominant()) DoJump();       // dominant top button
	else UseMedicalBag();
}

void AOutlawsPlayer::PressY()
{
	FaceHeld |= 8;
	if (bDeadView) { RequestRespawn(true); return; }
	if (bMenuOpen) { MenuBack(); return; }
	if (OneHand() == 0) { DoJump(); return; }
	if (bFaceReload) { if (Equipped[0] >= 0) bButtonReload[0] = true; return; }
	if (LeftDominant()) DoJump();
	else UseMedicalBag();                // off-hand top button
}

void AOutlawsPlayer::UseMedicalBag()
{
	AOutlawsLevelActor* L = Level.Get();
	if (L && L->UseMedicalBag())
	{
		++TutNotes.BagsUsed;                 // (REQ-TUTORIAL)
		Pulse(LeftDominant() ? 1 : 0, 0.5f);
	}
}

void AOutlawsPlayer::ResetForLevel()
{
	CloseWheel();
	PrevHeadZ = -1.f;
	Pulls.Reset();
	for (int32 H = 0; H < 2; ++H)
	{
		Equip(H, -1);
		HeldItem[H] = nullptr;
		PushDoor[H] = -1;
	}
	SavedMag.Reset();                    // (a new level: TopUpForLevelStart loads them from the carried ammunition; a restart: RestoreEntryMags)
	Stamina = 100.f;                     // (BUG-LEVELCARRY: rested at every level start; a load's ReadSave puts the saved stamina back)
	// a death on a ledge or mid-mantle left the body flying: back to normal movement
	bSwimming = false;
	bVaulting = false;
	GetCharacterMovement()->SetMovementMode(MOVE_Falling);
	bMenuPistol = false;
	// back on your feet: the view up, colours and edges clear
	bDeadView = false;
	DeathTime = 0.f;
	DeathDrop = 0.f;
	DeathBlend = 0.f;
	HurtFlash = 0.f;
	FallGraceUntil = GetWorld()->GetTimeSeconds() + 3.0;
	VROrigin->SetRelativeRotation(FRotator::ZeroRotator);
	if (RespawnText) RespawnText->SetVisibility(false);
	if (DeathPlate) DeathPlate->SetVisibility(false);
	for (int32 H = 0; H < 2; ++H)
	{
		FanActive[H] = 0.f;
	}
	GrabHand = -1;
	MantleLeft = 0.f;
	NudgedDoor = -1;
	WristLast.Reset();
	UE_LOG(LogOutlaws, Log, TEXT("OLRT one controller: %s"), OneController == 1 ? TEXT("Left only") : OneController == 2 ? TEXT("Right only") : TEXT("Off"));
}

void AOutlawsPlayer::OnHurt(float Damage, const FVector* From)
{
	// the bigger the hit, the redder; a hit the plate stopped only a little. From a shooter or a blast the red
	// comes from the rim facing it (in front or right behind: all round); twice the old strength (user 2026-09-27)
	const float Strength = FMath::Clamp(2.f * (Damage > 0.f ? 0.3f + 0.15f * Damage : 0.15f), 0.f, 1.f);
	if (From)
	{
		ChestHurtFrom = *From;               // (REQ-CHESTPLATE: the worn plate's dent goes where this hit came from)
		ChestHurtAt = GetWorld() ? GetWorld()->GetTimeSeconds() : -1.0;
		const FVector Local = Camera->GetComponentTransform().InverseTransformVectorNoScale(*From - Camera->GetComponentLocation()).GetSafeNormal();
		const FVector2D Screen(Local.Y, Local.Z);
		HurtDir = Screen.Size() < 0.15f ? FVector2D::ZeroVector : Screen.GetSafeNormal();
		HurtDirAmount = FMath::Max(HurtDirAmount, Strength);
	}
	else
	{
		HurtFlash = FMath::Clamp(FMath::Max(HurtFlash, Strength), 0.f, 0.95f);
	}
	Pulse(0, FMath::Clamp(0.3f + 0.2f * Damage, 0.3f, 1.f));
	Pulse(1, FMath::Clamp(0.3f + 0.2f * Damage, 0.3f, 1.f));
}

// Stick clicks: both sticks clicked together (let go within 1.5 s) = recenter; both held 1.5 s = a perf mark and nothing
// else (REQ-PERFMARK, 2026-10-05: the recenter used to fire at the press, so every mark also re-seated the held gear, a
// hitch of its own).  A single click acts on release, and only when the other stick was not pressed meanwhile, so the
// chord never also toggles run or snap turning.
void AOutlawsPlayer::PressLeftStick() { StickPressed(0); }
void AOutlawsPlayer::PressRightStick() { StickPressed(1); }
void AOutlawsPlayer::ReleaseLeftStick() { StickReleased(0); }
void AOutlawsPlayer::ReleaseRightStick() { StickReleased(1); }

void AOutlawsPlayer::StickPressed(int32 Stick)
{
	bStickHeld[Stick] = true;
	StickDownAt[Stick] = FPlatformTime::Seconds();
	if (bStickHeld[1 - Stick] && !bStickChord)
	{
		bStickChord = true;             // (the recenter waits for the release: held on, it is a perf mark instead)
	}
}

void AOutlawsPlayer::StickReleased(int32 Stick)
{
	if (bBugChord)
	{
		// (REQ-BUGSNAP) part of the bug snapshot's chord: no recenter, run, barrels, taunt or one-controller pause
		bStickHeld[Stick] = false;
		return;
	}
	if (bStickChord && bStickHeld[0] && bStickHeld[1])
	{
		// the chord's first stick let go: a click recenters, a hold already saved its perf mark
		UE_LOG(LogOutlaws, Log, TEXT("OLRT stick chord let go after %.2f s: %s"), StickChordHeld, bPerfMarked ? TEXT("perf mark only, no recenter") : TEXT("recenter"));
		if (!bPerfMarked) Recenter();
	}
	bStickHeld[Stick] = false;
	if (OneHand() == Stick && !bStickChord)
	{
		// one controller (REQ-ONECTRL; the right controller has no menu button): held a second, the pause menu opens or
		// closes; a click toggles run
		if (FPlatformTime::Seconds() - StickDownAt[Stick] >= 1.0) PressMenu();
		else if (!bMenuOpen) bRunning = !bRunning;
		return;
	}
	if (!bStickChord && !bMenuOpen)
	{
		// the move stick (off hand) toggles run; the turn stick's click switches a double gun's barrels (user 2026-09-29:
		// swapped with the crouch, which is the turn stick held down now)
		const bool bMoveStick = (Stick == 0) != LeftDominant();
		if (bMoveStick && OutlawsMpPolish::VrTauntPress(Level.Get(), FPlatformTime::Seconds() - StickDownAt[Stick]))
		{
			// (M4) held a moment: the next taunt slot said, the run left as it was
		}
		else if (bMoveStick)
		{
			bRunning = !bRunning;
		}
		else if (!bWheelOpen)
		{
			ToggleBarrels();
		}
	}
	if (!bStickHeld[0] && !bStickHeld[1])
	{
		bStickChord = false;
	}
}

void AOutlawsPlayer::UpdateRightEyeMirror(float Dt)
{
	static const bool bWanted = OLFlags::Param(TEXT("OLRightEye"));
	if (!bWanted || !bVR || SpecCapture) return;      // (REQ-SPECCAM's Steady view owns the window while it is on)
	RightEyeMirrorWait -= Dt;
	if (RightEyeMirrorWait > 0.f) return;
	RightEyeMirrorWait = 3.f;
	if (!GEngine || !GEngine->XRSystem.IsValid()) return;
	IHeadMountedDisplay* Hmd = GEngine->XRSystem->GetHMDDevice();
	ISpectatorScreenController* Ctrl = Hmd ? Hmd->GetSpectatorScreenController() : nullptr;
	if (!Ctrl) return;
	// the OpenXR HMD (the only XR plugin here) is an FHeadMountedDisplayBase with the default controller
	Ctrl->SetSpectatorScreenMode(ESpectatorScreenMode::SingleEyeCroppedToFill);
	FDefaultSpectatorScreenController* Def = static_cast<FDefaultSpectatorScreenController*>(Ctrl);
	FHeadMountedDisplayBase* Base = static_cast<FHeadMountedDisplayBase*>(Hmd);
	ENQUEUE_RENDER_COMMAND(OutlawsRightEyeMirror)([Def, Base](FRHICommandListImmediate&)
	{
		FSpectatorScreenRenderDelegate* D = Def->GetSpectatorScreenRenderDelegate_RenderThread();
		if (!D) return;
		D->BindLambda([Base](FRHICommandListImmediate& Cmd, FTexture2DRHIRef Target, FTexture2DRHIRef Eye, FTexture2DRHIRef, FVector2D Window)
		{
			if (!Eye.IsValid() || !Target.IsValid()) return;
			// the left eye's flat rect (OpenXR: 0.05..0.45 across the double-wide eye texture), moved over half the
			// width to the right eye, then cropped about its middle to the window's shape
			const FIntRect L = Base->GetFullFlatEyeRect_RenderThread(Eye);
			const int32 Half = int32(Eye->GetSizeX() / 2);
			FIntRect Src(L.Min.X + Half, L.Min.Y, L.Max.X + Half, L.Max.Y);
			const float WinAspect = Window.Y > 0.f ? float(Window.X / Window.Y) : float(Target->GetSizeX()) / FMath::Max(1u, Target->GetSizeY());
			const float SrcAspect = float(Src.Width()) / FMath::Max(1, Src.Height());
			if (SrcAspect > WinAspect)
			{
				const int32 W = int32(Src.Height() * WinAspect);
				const int32 C = (Src.Min.X + Src.Max.X) / 2;
				Src.Min.X = C - W / 2;
				Src.Max.X = Src.Min.X + W;
			}
			else
			{
				const int32 H = int32(Src.Width() / WinAspect);
				const int32 C = (Src.Min.Y + Src.Max.Y) / 2;
				Src.Min.Y = C - H / 2;
				Src.Max.Y = Src.Min.Y + H;
			}
			Base->CopyTexture_RenderThread(Cmd, Eye, Src, Target, FIntRect(0, 0, Target->GetSizeX(), Target->GetSizeY()), false, true);
			static bool bSaid = false;
			if (!bSaid)
			{
				bSaid = true;
				UE_LOG(LogOutlaws, Log, TEXT("OLRT mirror: the right eye (%d,%d..%d,%d of %dx%d) to the %dx%d window"),
					Src.Min.X, Src.Min.Y, Src.Max.X, Src.Max.Y, Eye->GetSizeX(), Eye->GetSizeY(), Target->GetSizeX(), Target->GetSizeY());
			}
		});
	});
}

void AOutlawsPlayer::Recenter(bool bQuiet)
{
	if (!bVR)
	{
		return;
	}
	// facing and position: the headset's current pose becomes "forward, centred"; the height is
	// set on the next frame once the new pose is in (the current head counts as standing height)
	UHeadMountedDisplayFunctionLibrary::ResetOrientationAndPosition(0.f, EOrientPositionSelector::OrientationAndPosition);
	VROrigin->SetRelativeLocation(FVector(0.0, 0.0, VROrigin->GetRelativeLocation().Z));
	bPendingHeight = true;
	// the guns back in line with the controllers too (user 2026-09-28)
	ResetHeldGear();
	if (!bQuiet) ShowMessage(TEXT("Recentered"), 2.f);
}

void AOutlawsPlayer::ResetHeldGear()
{
	const int32 W[2] = { Equipped[0], Equipped[1] };
	const bool bOne[2] = { bOneBarrel[0], bOneBarrel[1] };
	for (int32 H = 0; H < 2; ++H)
	{
		// the hand frame's lie along the controller is measured again (AlignHandFrames)
		bFrameAligned[H] = false;
		FrameSteady[H] = 0;
		bSupport[H] = false;
		bToolSupport[H] = false;
		RecoilKick[H] = 0.f;
		RecoilPitch[H] = 0.f;
		StockBlend[H] = 0.f;
		if (UOutlawsBoxHand* Box = H == 0 ? LeftHand.Get() : RightHand.Get()) Box->SetRelativeTransform(FTransform::Identity);
		if (ToolRoot[H])
		{
			ToolRoot[H]->SetRelativeTransform(ToolRest(H));
			bToolTwoHanded[H] = false;
		}
	}
	for (int32 H = 0; H < 2; ++H)
	{
		if (W[H] >= 0)
		{
			Equip(H, W[H]);                     // rebuilt on its seat, its rounds kept (SavedMag)
		}
		else if (HeldTool[H].IsEmpty())
		{
			Equip(H, -1);                       // an empty hand shows nothing (a movie's restore may have shown a gun)
		}
		else if (WeaponMesh[H])
		{
			WeaponMesh[H]->SetVisibility(false);   // a tool in hand: no gun beside it
		}
		// a shovel or crowbar let go of (it stays with its level) is not shown either
		if (HeldTool[H].IsEmpty() && ToolRoot[H]) ToolRoot[H]->SetVisibility(false, true);
	}
	bOneBarrel[0] = bOne[0];
	bOneBarrel[1] = bOne[1];
	UE_LOG(LogOutlaws, Log, TEXT("OLRT held gear re-seated (left %d, right %d)"), Equipped[0], Equipped[1]);
}

void AOutlawsPlayer::RestoreCarriedHands()
{
	if (!bCarryHands) return;
	bCarryHands = false;
	for (int32 H = 0; H < 2; ++H)
	{
		const int32 Wi = CarryHands[H];
		CarryHands[H] = -1;
		if (Wi < 0 || WeaponsOwned(Wi) <= 0 || (H == 1 && Wi == Equipped[0] && WeaponsOwned(Wi) < 2))
		{
			CarryMag[H] = -1;                // not carried any more (Bob's ranch takes them), or one of them already in the other hand
			continue;
		}
		Equip(H, Wi);                        // (its rounds: TopUpForLevelStart, from CarryMag)
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT hands carried over: left %d, right %d"), Equipped[0], Equipped[1]);
}

float AOutlawsPlayer::HeadHeight() const
{
	// the head above the play-space floor, after the recenter / auto height correction
	return float(Camera->GetRelativeLocation().Z) + HeightOffset;
}

void AOutlawsPlayer::CheckHeight(float Dt)
{
	if (!bVR)
	{
		return;
	}
	const float Raw = float(Camera->GetRelativeLocation().Z);
	auto Origin = []()
	{
		switch (UHeadMountedDisplayFunctionLibrary::GetTrackingOrigin())
		{
		case EHMDTrackingOrigin::Floor: return TEXT("floor");
		case EHMDTrackingOrigin::Stage: return TEXT("stage");
		default: return TEXT("eye");
		}
	};
	if (bPendingHeight)
	{
		bPendingHeight = false;
		HeightOffset = VRStandingEyeHeight - Raw;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT VR recenter: raw head %.0f cm (tracking origin %s) -> offset %.0f cm, head now %.0f"), Raw, Origin(), HeightOffset, HeadHeight());
		return;
	}
	if (!bHeightChecked)
	{
		// the first moments after spawn: a head under 60 cm above the tracking floor means the
		// runtime handed out an eye-level space (or no floor); stand the view up
		HeightCheckDelay -= Dt;
		if (HeightCheckDelay <= 0.f)
		{
			bHeightChecked = true;
			if (Raw < 60.f)
			{
				HeightOffset = VRStandingEyeHeight - Raw;
			}
			UE_LOG(LogOutlaws, Log, TEXT("OLRT VR height at spawn: raw head %.0f cm (tracking origin %s), offset %.0f cm"), Raw, Origin(), HeightOffset);
		}
	}
}

void AOutlawsPlayer::DoJump()
{
	if (bDeadView)
	{
		RequestRespawn();
		return;
	}
	if (IsMountedGun())
	{
		return;
	}
	if (IsClimbing() || MantleLeft > 0.f)
	{
		return;
	}
	AOutlawsLevelActor* L = Level.Get();
	float Surface = 0.f;
	// at the surface (swimming, or bobbing just out of the swim at the waterline): jump out. Against a wall
	// (a bank, a pool's edge) the jump goes higher and over it (user 2026-09-27)
	const float FeetZ = float(GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	const FVector Middle(GetActorLocation().X, GetActorLocation().Y, FeetZ + StandHalfHeight);
	if (L && L->WaterAt(Middle - FVector(0.0, 0.0, 40.0), Surface) && Middle.Z > Surface - 80.f && (bSwimming || GetVelocity().Z < 200.f))
	{
		// only over an edge (a bank, a pool's side) ahead within reach: a wall in front, and ground on top of it no higher
		// than a jump climbs (user 2026-09-29: jumping again and again out in the open stood the body on the water)
		const FVector Forward = FRotator(0.0, Camera->GetComponentRotation().Yaw, 0.0).Vector();
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsWaterExit), false, this);
		FHitResult Hit, Top;
		bool bEdge = GetWorld()->SweepSingleByChannel(Hit, Middle, Middle + Forward * 70.0, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeSphere(20.f), Q);
		if (bEdge)
		{
			const FVector Over = FVector(Hit.ImpactPoint) + Forward * 30.0;
			bEdge = GetWorld()->LineTraceSingleByChannel(Top, FVector(Over.X, Over.Y, Surface + 170.0), FVector(Over.X, Over.Y, Surface - 30.0), ECC_Pawn, Q)
				&& Top.ImpactNormal.Z > 0.6 && !Top.bStartPenetrating;
		}
		if (!bEdge)
		{
			return;                                      // treading water at the surface: nothing to climb out onto
		}
		bSwimming = false;
		GetCharacterMovement()->SetMovementMode(MOVE_Falling);
		LaunchCharacter(Forward * 220.0 + FVector(0.0, 0.0, 640.0), true, true);
		SwimExitUntil = GetWorld()->GetTimeSeconds() + 0.6;     // no falling back into the swim on the way up
		UE_LOG(LogOutlaws, Log, TEXT("OLRT out of the water over the edge"));
		return;
	}
	if (bSwimming)
	{
		SwimKick(1.f);                                   // deeper down: a kick up
		return;
	}
	// a spent body jumps half as high (height goes with the square of the launch speed)
	GetCharacterMovement()->JumpZVelocity = (bVR ? VRJumpVelocity : DesktopJumpVelocity) * FMath::Sqrt(FMath::Lerp(0.5f, 1.f, StaminaFrac()));
	if (CanJump())
	{
		Stamina = FMath::Max(0.f, Stamina - 1.25f * (CarryingMan() ? CarryDrain : 1.f));    // E_JUMP 5, at a quarter (user 2026-09-27)
	}
	if (CanJump() && L)
	{
		L->PlaySoundAt(TEXT("jajump"), GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight()));   // SND_JUMP
	}
	Jump();
}

void AOutlawsPlayer::OnJumped_Implementation()
{
	Super::OnJumped_Implementation();
	++TutNotes.Jumps;                    // (REQ-TUTORIAL)
	const AOutlawsLevelActor* L = Level.Get();
	UCharacterMovementComponent* M = GetCharacterMovement();
	const float S = FMath::Clamp((L ? L->Pace() : 0.5f) / 0.5f, 0.2f, 1.f);      // the movement speed setting, 0.2..1
	if (!M || S >= 0.999f) return;
	// the share of the full setting's reach: a half at 20 %, linear to all of it at 100 %; at the walk's own speed that takes
	// a jump Reach / S times as long
	const float Reach = 0.5f + 0.5f * (S - 0.2f) / 0.8f;
	const float K = FMath::Max(1.f, Reach / S);
	M->Velocity.Z /= K;
	M->GravityScale /= K * K;                 // (now; UpdateGravity keeps it so)
	JumpStretch = K;
	JumpOffZ = float(GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	bJumpBoost = true;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT jump at speed %.0f %%: %.2f times as long (reach %.0f %% of the full speed's)"), S * 100.f, K, Reach * 100.f);
}

// ---- per frame -----------------------------------------------------------------------------------

void AOutlawsPlayer::Tick(float Dt)
{
	OL_PERF_SCOPE("Player tick (total)");
	// (BUG-PAUSEZ) never more time than the frame really took.  The pause (menu, host pause) sets this body's 10000x at once,
	// the world's 1/10000 only from the next frame on: the frame the Menu button opens it on (input, before this tick) came
	// here 10000 times its length, a 14 ms frame as 140 s, and the view's crouch ride ran on at 200 cm/s through it: the eye
	// 280 m above or below the feet, riding back at 200 cm/s after
#if WITH_OUTLAWS_TESTS
	static const bool bOldPauseZ = OLFlags::Param(TEXT("OLOldPauseZ"));     // (-OLPauseZTest's before: the frame as it came)
	if (!bOldPauseZ)
#endif
	Dt = FMath::Min(Dt, float(FApp::GetDeltaTime()));
	Super::Tick(Dt);
	// (BUG-ARCSTUCK) on every way out of Tick, the early returns too: a throw arc not drawn this frame is hidden
	struct FArcSweep { AOutlawsPlayer* P; ~FArcSweep() { P->SweepThrowArcs(); } } ArcSweep{ this };
	UpdateFadeVeil();                      // (first: the movie, menu and front-end branches below return early)
	UpdatePlayWarm(Level.Get());
	if (FirstUseProbeOn()) TickFirstUseProbe(Dt);     // (FIRSTHITCH: -OLFirstUseProbe)
	if (ClimbProbeOn()) TickClimbProbe(Dt);           // (BUG-SLOPECLIMB: -OLClimbProbe)
	OutlawsAchievements::Tick(this, Level.Get(), Dt);     // (REQ-EXTRAS: an achievement's toast once there is somewhere to show it)
	// both sticks kept in for 1.5 s: mark this moment for the perf report (and no recenter at the release)
#if WITH_OUTLAWS_TESTS
	if (OLFlags::Param(TEXT("OLTestStickChord"))) TickStickChordTest();
	static const bool bPauseTidyTest = OLFlags::HasValue(TEXT("OLPauseTidyMode"));
	if (bPauseTidyTest) UOutlawsCoop::PauseTidyTestTick(this, Dt);      // (REQ-PAUSETIDY: OutlawsPlayerMenu.cpp)
	static const bool bDevChecksTest = OLFlags::Has(TEXT("OLDevChecksTest"));
	if (bDevChecksTest) DevChecksTestTick(Dt);                           // (REQ-DEVCHECKS: OutlawsDevChecks.cpp)
	static const bool bUnlockTest = OLFlags::HasValue(TEXT("OLUnlockTest"));
	if (bUnlockTest) OutlawsUnlocks::TickTest(Level.Get(), Dt);         // (REQ-UNLOCKS: OutlawsUnlocksTest.cpp)
	static const bool bGlyphTest = OLFlags::HasValue(TEXT("OLGlyphTest"));
	if (bGlyphTest) GlyphTestTick(Dt);                                   // (REQ-BUTTONGLYPHS: OutlawsPlayerGlyphs.cpp)
	static const bool bVoiceSoak = OLFlags::Has(TEXT("OLVoiceSoak"));
	if (bVoiceSoak)
	{
		// (BUG-MICSTATIC: -OLVoiceSoak[=legacy] [-OLVoiceSoakSeconds=360], the long-run voice test, OutlawsCoopVoiceSoak.cpp)
		static bool bSoakDone = false;
		if (!bSoakDone)
		{
			bSoakDone = true;
			FString SoakMode;
			OLFlags::Value(TEXT("OLVoiceSoak"), SoakMode);
			float SoakSeconds = 360.f;
			OLFlags::Value(TEXT("OLVoiceSoakSeconds"), SoakSeconds);
			OLVoicePipe::RunVoiceSoak(SoakMode == TEXT("legacy"), SoakSeconds);
			FGenericPlatformMisc::RequestExit(false);
		}
	}
#endif
	// (REQ-BUGSNAP: OutlawsBugSnap.cpp) both triggers and both stick clicks: a bug snapshot, in play, menus, movies and death
#if WITH_OUTLAWS_TESTS
	static const bool bBugSnapTest = OLFlags::Param(TEXT("OLBugSnapTest"));
	if (bBugSnapTest) BugSnapTestTick(Dt);
#endif
	UpdateBugSnap(Dt);
	if (bStickHeld[0] && bStickHeld[1] && !bBugChord)
	{
		StickChordHeld += Dt;
		if (StickChordHeld >= 1.5f && !bPerfMarked)
		{
			bPerfMarked = true;
			OutlawsPerf::Mark(TEXT("both sticks held"));
			UE_LOG(LogOutlaws, Log, TEXT("OLRT perf mark saved: both sticks held %.2f s (haptics, toast; no recenter, no gear re-seat)"), StickChordHeld);
			Pulse(0, 1.f);
			Pulse(1, 1.f);
			ShowMessage(TEXT("Perf mark saved"), 1.5f);
		}
	}
	else
	{
		StickChordHeld = 0.f;
		bPerfMarked = false;
	}
	if (!Level.IsValid())
	{
		FindLevel();
	}
	HeldStickLeft = FMath::Max(0.f, HeldStickLeft - Dt);
	UpdateHud(Dt);
	UpdateHudGlyph(Dt);                    // (REQ-BUTTONGLYPHS: the tutorial card's control under the notices)
	UpdateRefuse(Dt);
	UpdateRightEyeMirror(Dt);          // (before the menu's, the movie's and death's early returns: recorded in all of them)
	UpdateSpecCam(Dt);                 // (REQ-SPECCAM, the same: OutlawsPlayerSpecCam.cpp)
	if (AOutlawsLevelActor* LM = Level.Get(); LM && LM->IsMoviePlaying() && bMenuOpen)
	{
		// the movie held behind the pause menu (user 2026-10-01): the menu works as in play, the screen in the death tone.
		// The body stays put (BUG-PAUSELIFT): no room-scale follow, so nothing is left to correct when the movie goes on
		CheckHeight(Dt);
		AlignHandFrames();
		PlaceOrigin(Dt);
		UpdateHands(Dt);
		UpdateMenu(Dt);
		UpdateScreenFx(Dt);
		LM->PauseMovie(true, BackdropTone);
#if WITH_OUTLAWS_TESTS
		if (ShotTime >= 0.f) ShotTest(Dt);
#endif
		return;
	}
	if (AOutlawsLevelActor* LM = Level.Get(); LM && LM->IsMoviePlaying())
	{
		if (!bMovieHidden) HideForMovie(true);
		// watching the story movie: nothing else. Any button or trigger held for 3 s skips it, never in its
		// first second; the screen says to keep holding, with a bar (user 2026-09-27); each more button held fills it
		// faster (two: twice as fast; user 2026-09-28)
		const int32 NumHeld = (Axis(TEXT("OLVR_LeftTrigger")) > 0.5f ? 1 : 0) + (Axis(TEXT("OLVR_RightTrigger")) > 0.5f ? 1 : 0)
			+ (Axis(TEXT("OLVR_LeftGrip")) > 0.5f ? 1 : 0) + (Axis(TEXT("OLVR_RightGrip")) > 0.5f ? 1 : 0) + FMath::CountBits(uint64(FaceHeld & 15));
		constexpr float Need = 3.f;
		MovieHold = NumHeld > 0 && LM->CanSkipMovie() ? MovieHold + Dt * NumHeld : 0.f;
		LM->SetMovieSkipHold(MovieHold, Need, NumHeld);
		if (MovieHold >= Need)
		{
			MovieHold = 0.f;
			LM->SkipMovie();
		}
		// the movie shows clean: the screen effects are not updated while it plays, so the title menu's death
		// tone and edges stayed on the story's first movie (user 2026-09-27)
		ClearScreenFx();
#if WITH_OUTLAWS_TESTS
		if (ShotTime >= 0.f) ShotTest(Dt);
#endif
		return;
	}
	MovieHold = 0.f;
	if (bMovieHidden)
	{
		HideForMovie(false);
		// what was shown before the movie may not be held any more (a level won in the meantime): the hands as they are
		ResetHeldGear();
	}
	if (bDeadView)
	{
		UpdateDeathView(Dt);
		UpdateThrown(Dt);                  // (BUG-TNTDEATHFREEZE: what this player threw before it died flies on and goes off, in the air as on the ground)
#if WITH_OUTLAWS_TESTS
		if (SmokeTime >= 0.f)
		{
			SmokeTest(Dt);
		}
		if (ShotTime >= 0.f)
		{
			ShotTest(Dt);
		}
#endif
		return;
	}
	if (bMenuOpen)
	{
		// the world holds still behind the menu: only the head and the hands keep up.  The body does not (BUG-PAUSELIFT):
		// with movement off nothing holds it to the floor, so a room-scale slide pushed out of a wall or step, or a body
		// resize, left it raised until the menu closed and the floor snapped it back.  It catches up with the head after
		CheckHeight(Dt);
		AlignHandFrames();
		PlaceOrigin(Dt);
		UpdateHands(Dt);
		UpdateGrabGlow(Dt);                // (menu open: hides any glow)
		UpdateMenu(Dt);
		UpdateScreenFx(Dt);
#if WITH_OUTLAWS_TESTS
		if (ShotTime >= 0.f) ShotTest(Dt);
#endif
		return;
	}
	if (bHostPauseFrozen)
	{
		// BUG-HOSTPAUSE: the host's app paused, the world held still here as behind the menu (no walking, no guns, the body put)
		CheckHeight(Dt);
		AlignHandFrames();
		PlaceOrigin(Dt);
		UpdateHands(Dt);
		UpdateScreenFx(Dt);
#if WITH_OUTLAWS_TESTS
		if (ShotTime >= 0.f) ShotTest(Dt);
#endif
		return;
	}
	CheckHeight(Dt);
	AlignHandFrames();
	FollowHead();
	Swimming();
	SwimStrokes(Dt);
	UpdateGravity(Dt);
	// out of the map: a fall that goes on for three seconds (or well under the lowest floor) ends back on the
	// ground near where it began, at a bad guy's empty spawn point, else the level start (user 2026-09-27)
	if (AOutlawsLevelActor* LF = Level.Get(); LF && !LF->IsFrontEnd() && !bDeadView && !bSwimming && !IsClimbing())
	{
		UCharacterMovementComponent* MF = GetCharacterMovement();
		const bool bFalling = MF->MovementMode == MOVE_Falling && MF->Velocity.Z < -100.f;
		const double Now = GetWorld()->GetTimeSeconds();
		if (!bFalling) FallSince = -1.0;
		else if (FallSince < 0.0) { FallSince = Now; FallFrom = GetActorLocation(); }
		// (twice the fall it took before, user 2026-10-02: the canyon's Dead Man's Leap set it off: 4.25 s, the drop of a
		// steady fall growing with the time squared, and 30 m under the lowest floor, from 3 s and 15 m)
		const bool bDeepUnder = GetActorLocation().Z < LF->LowestFloorZ() - 3000.f;
		if (bFalling && (Now - FallSince > 4.25 || (bDeepUnder && Now - FallSince > 1.0)))
		{
			FVector Spot;
			if (LF->FindGroundNear(FallFrom, GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), Spot))
			{
				TeleportTo(Spot, GetActorRotation(), false, true);
				MF->StopMovementImmediately();
				GrantFallGrace(Now + 3.0);
				UE_LOG(LogOutlaws, Log, TEXT("OLRT fell out of the map (from %s): back on the ground at %s"), *FallFrom.ToString(), *Spot.ToString());
			}
			FallSince = -1.0;
		}
	}
	UpdateUnstuck(Dt);                 // (BUG-STUCKGAP: out of every sector, or in a wall: back to safe ground)
	Climbing(Dt);
	UpdateCrouch();
	PlaceOrigin(Dt);
	bool bMoving = false, bTurning = false;
	Locomotion(Dt, bMoving);
	UpdateTeleport(Dt);
	Turning(Dt, bTurning);
	UpdateHands(Dt);
	UpdateGrabGlow(Dt);
	UpdateGuns(Dt);
	UpdateFx(Dt);
	UpdateVignette(Dt, bMoving || bTurning);
	UpdateScreenFx(Dt);
	UpdateFootsteps(Dt);
	UpdatePulls(Dt);
	UpdateThrown(Dt);
	UpdateFuseSparks(Dt);
	UpdateWrist();
	if (bVR)
	{
		DetectBodyJump(Dt);
	}
	UpdateWheel();
	UpdateScope();
	UpdateStamina(Dt);
	UpdateBreath(Dt);
	UpdateTools(Dt);
	UpdateBadgeVignette(Dt);
	UpdatePunches(Dt);
	UpdateEnemyGrab(Dt);
	UpdateBottles(Dt);
	UpdateDualPickup(Dt);
	UpdateHandWater(Dt);
	UpdateHeldLight(Dt);
#if WITH_OUTLAWS_TESTS
	if (SmokeTime >= 0.f)
	{
		SmokeTest(Dt);
	}
	if (ShotTime >= 0.f)
	{
		ShotTest(Dt);
	}
#endif
	bDesktopMoving = false;       // input callbacks run before the next Tick and set it again
}

void AOutlawsPlayer::FollowHead()
{
	// room scale: slide the capsule under the head (with collision), then shift the play space back
	// by what it actually moved so the view does not jump
	if (!bVR || MantleLeft > 0.f)
	{
		return;
	}
	FVector Delta = Camera->GetComponentLocation() - GetActorLocation();
	Delta.Z = 0.0;
	if (Delta.SizeSquared() < 4.0)
	{
		return;
	}
	const FVector Before = GetActorLocation();
	FHitResult Hit;
	GetCharacterMovement()->SafeMoveUpdatedComponent(Delta, GetActorQuat(), true, Hit);
	FVector Moved = GetActorLocation() - Before;
	Moved.Z = 0.0;
	VROrigin->AddWorldOffset(-Moved);
}

void AOutlawsPlayer::UpdateCrouch()
{
	OL_PERF_SCOPE("Player: crouch");
	if (IsClimbing() || MantleLeft > 0.f)
	{
		return;
	}
	if (bVR)
	{
		UpdateBodyHeight();
		return;
	}
	if (bSwimming)
	{
		return;
	}
	const bool bWant = bCrouchHeld;
	if (bWant && !bIsCrouched)
	{
		Crouch();     // in mid-air this shrinks around the middle: the feet come up (crouch-jump)
	}
	else if (!bWant && bIsCrouched)
	{
		UnCrouch();   // refused while a low ceiling is overhead; tried again every frame
	}
}

void AOutlawsPlayer::OnStartCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust)
{
	Super::OnStartCrouch(HalfHeightAdjust, ScaledHalfHeightAdjust);
	PlaceOrigin(0.f);
}

void AOutlawsPlayer::OnEndCrouch(float HalfHeightAdjust, float ScaledHalfHeightAdjust)
{
	Super::OnEndCrouch(HalfHeightAdjust, ScaledHalfHeightAdjust);
	PlaceOrigin(0.f);
}

float AOutlawsPlayer::ViewDrop()
{
	if (bCrouchHeld)
	{
		// (eased off part way under something low, the body cannot grow yet: the eye lower by what it still lacks of the
		// height it wants, UpdateBodyHeight's, till it has grown)
		const UCapsuleComponent* Cap = GetCapsuleComponent();
		const float MinHalf = Cap->GetUnscaledCapsuleRadius() + 2.f;
		const float WantHalf = FMath::Min(FMath::Clamp((HeadHeight() - CrouchDrop + 12.f) * 0.5f, MinHalf, StandHalfHeight), FMath::Lerp(StandHalfHeight, CrouchHalfHeight, CrouchAmount));
		return CrouchDrop + 2.f * FMath::Max(0.f, WantHalf - Cap->GetUnscaledCapsuleHalfHeight());
	}
	if (bStandPending)
	{
		// standing back up under something low: keep the eye inside the body until it has grown
		const float Top = 2.f * GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() - 12.f;
		const float Drop = FMath::Max(0.f, HeadHeight() - Top);
		if (Drop <= 0.5f)
		{
			bStandPending = false;
		}
		return Drop;
	}
	return 0.f;
}

void AOutlawsPlayer::PlaceOrigin(float Dt)
{
	// the tracking floor stays at the feet whatever the capsule height; a VR crouch toggled by the
	// stick (seated play) drops the view to crouch height instead of the real head
	const float Half = GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
	// the stick crouch lowers (and raises) the view as a smooth ride, never a snap: its speed eases in, holds and eases
	// out on arrival (user 2026-09-28: the snap down was sickening).  Its pace goes with the menu's movement speed, the
	// player's own measure of what their VR legs take: 3.6 m/s at 55 percent (twice the first try), 6.5 at full
	float Drop = 0.f;
	if (bVR)
	{
		const float Want = ViewDrop();
		if (MantleDrop0 >= 0.f && MantleLeft > 0.f)
		{
			// a climb's own crouch rides down on the climb's ease: the rise onto the ledge and the drop are one move
			const float A = FMath::InterpEaseInOut(0.f, 1.f, 1.f - MantleLeft / 0.25f, 2.f);
			ShownDrop = FMath::Lerp(MantleDrop0, Want, A);
			ShownDropVel = 0.f;
		}
		else if (Dt > 0.f)
		{
			MantleDrop0 = -1.f;
			const AOutlawsLevelActor* LP = Level.Get();
			const float K = (LP ? LP->Pace() : 0.55f) / 0.55f;
			const float MaxV = 360.f * K;
			const float WantVel = FMath::Clamp((Want - ShownDrop) * 10.f * K, -MaxV, MaxV);
			ShownDropVel = FMath::FInterpTo(ShownDropVel, WantVel, Dt, FMath::Max(6.f, 16.f * K));
			const float Step = ShownDropVel * Dt;
			bool bPast = (Want - ShownDrop) * Step > 0.f && FMath::Abs(Step) >= FMath::Abs(Want - ShownDrop);
#if WITH_OUTLAWS_TESTS
			static const bool bOldPauseZ = OLFlags::Param(TEXT("OLOldPauseZ"));
			bPast = bPast && !bOldPauseZ;
#endif
			if (bPast) { ShownDrop = Want; ShownDropVel = 0.f; }   // (never past it, BUG-PAUSEZ)
			else ShownDrop += Step;
			if (FMath::Abs(Want - ShownDrop) < 0.2f && FMath::Abs(ShownDropVel) < 2.f) { ShownDrop = Want; ShownDropVel = 0.f; }
		}
		Drop = ShownDrop;    // a re-seat with no time passed (the capsule just resized: Dt 0) keeps the ride where it is
	}
	// the train rocks: the original's exe bobs the view on the train level (hardcoded there, no level
	// data); here far slower and smaller for VR, two slow swells blended so it never ticks like a
	// machine, a couple of centimetres at most, eased in and out (user 2026-09-27)
	const AOutlawsLevelActor* LB = Level.Get();
	const bool bRock = LB && LB->LevelName.Equals(TEXT("train"), ESearchCase::IgnoreCase) && !bDeadView && !bMenuOpen && !bSwimming;
	TrainRock = FMath::FInterpConstantTo(TrainRock, bRock ? 1.f : 0.f, float(FApp::GetDeltaTime()), 0.5f);
	float Rock = 0.f;
	if (TrainRock > 0.f)
	{
		const double T = FPlatformTime::Seconds();
		Rock = TrainRock * float(1.4 * FMath::Sin(UE_DOUBLE_TWO_PI * 0.45 * T) + 0.6 * FMath::Sin(UE_DOUBLE_TWO_PI * 0.17 * T + 1.1));
	}
	if (Dt > 0.f) UpdateStepEase(Dt);
	VROrigin->SetRelativeLocation(FVector(VROrigin->GetRelativeLocation().X, VROrigin->GetRelativeLocation().Y, -Half - Drop + (bVR ? HeightOffset : 0.f) - (bVR ? DeathDrop : 0.f) + Rock + StepEase));
	if (!bVR)
	{
		const float Target = bIsCrouched ? CrouchEye : StandEye;
		CameraEye = Dt > 0.f ? FMath::FInterpTo(CameraEye, Target, Dt, 12.f) : Target;
		Camera->SetRelativeLocation(FVector(0.0, 0.0, CameraEye - DeathDrop));
	}
}

void AOutlawsPlayer::UpdateStepEase(float Dt)
{
	// the body steps up and down stairs and small ledges (up to StepHeight) in one frame, the floor snapping it there: the
	// view (and the hands with it) keeps its height and settles onto the new one over about a third of a second, critically
	// damped, no bob.  Only a step taken on foot on still ground: a fall, a jump, a climb, swimming or a lift's ride are
	// never held back.  A step is a rise or drop far steeper than the walk across it (the steepest walkable slope, 75 deg,
	// is only met at a crawl: a ramp at speed is followed as it is)
	const UCharacterMovementComponent* M = GetCharacterMovement();
	const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	const bool bGround = M && M->IsMovingOnGround();
	const UPrimitiveComponent* Base = GetMovementBase();
	const FVector BaseAt = Base ? Base->GetComponentLocation() : FVector::ZeroVector;
	const bool bStillBase = !Base || Base != StepPrevBaseComp.Get() || FVector::DistSquared(BaseAt, StepPrevBase) < 0.01;   // (onto or off a lift at rest: a step)
	if (bGround && bStepPrevGround && bStillBase && !IsClimbing() && !bSwimming && !bDeadView && MantleLeft <= 0.f)
	{
		const double Dz = Feet.Z - StepPrevAt.Z;
		const double Across = FVector2D(Feet.X - StepPrevAt.X, Feet.Y - StepPrevAt.Y).Size();
		if (FMath::Abs(Dz) > StepHeight + 25.f)
		{
			StepEase = StepEaseVel = 0.f;          // (put somewhere: no ease)
		}
		else if (FMath::Abs(Dz) > 1.0 + 0.75 * Across)
		{
			StepEase = FMath::Clamp(StepEase - float(Dz), -2.f * StepHeight, 2.f * StepHeight);   // (a flight's steps run on into each other)
		}
	}
	StepPrevAt = Feet;
	StepPrevBase = BaseAt;
	StepPrevBaseComp = Base;
	bStepPrevGround = bGround;
	// x(t) = (x0 + (v0 + w x0) t) e^-wt: settled (95 %) in about a third of a second (user 2026-10-03: "slightly
	// smoother"; 0.2 s showed each step of a flight, this lets a flight's steps run together into a slope)
	constexpr float Om = 15.f;
	const float E = FMath::Exp(-Om * Dt);
	const float C = StepEaseVel + Om * StepEase;
	StepEase = (StepEase + C * Dt) * E;
	StepEaseVel = (StepEaseVel - Om * C * Dt) * E;
	if (FMath::Abs(StepEase) < 0.05f && FMath::Abs(StepEaseVel) < 1.f) StepEase = StepEaseVel = 0.f;
}

namespace
{
	TAutoConsoleVariable<float> CVarMoveSmoothing(TEXT("ol.vr.MoveSmoothing"), 0.07f,
		TEXT("VR: seconds the move stick's push and walk / run speed changes are eased over (time constant; 0 = instant)."));

	/** A step toward Target, the part a time constant of Tau covers in Dt (Tau 0: all of it). */
	float EaseAlpha(float Dt, float Tau)
	{
		return Tau > 1e-4f ? 1.f - FMath::Exp(-Dt / Tau) : 1.f;
	}
}

void AOutlawsPlayer::Locomotion(float Dt, bool& bOutMoving)
{
	UCharacterMovementComponent* M = GetCharacterMovement();
	const float Ease = bVR ? EaseAlpha(Dt, CVarMoveSmoothing.GetValueOnGameThread()) : 1.f;
	// running at the stamina's pace: spent, it is no faster than walking
	const float WalkSpeed = bVR ? VRWalkSpeed : DesktopWalkSpeed;
	const float RunNow = FMath::Lerp(WalkSpeed, bVR ? VRRunSpeed : DesktopRunSpeed, StaminaFrac());
	M->MaxWalkSpeed = bVR && GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight() < 60.f ? VRCrouchSpeed : (bRunning ? RunNow : WalkSpeed);
	// swimming: half the land speed, running included
	M->MaxFlySpeed = SwimSpeedFraction * (bRunning ? RunNow : WalkSpeed);
	// the VR menu's movement speed (a quarter up to the full speed)
	if (const AOutlawsLevelActor* LS = Level.Get())
	{
		const float Pace = LS->Pace();
		M->MaxWalkSpeed *= Pace;
		M->MaxFlySpeed *= Pace;
		// the long jump of the lower speeds (OnJumped) is over at the landing, or once it falls past where it took off (a drop
		// beyond that is a fall like any other)
		if (bJumpBoost && (!M->IsFalling() || (M->Velocity.Z < 0.0 && GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight() < JumpOffZ - 10.0)))
		{
			bJumpBoost = false;
			JumpStretch = 1.f;
		}
	}
	// a man in the hand slows the step to three quarters (user 2026-09-30); a chicken or a tarantula does not
	if (CarryingMan())
	{
		M->MaxWalkSpeed *= CarrySpeedScale;
		M->MaxFlySpeed *= CarrySpeedScale;
	}
	// VR: walk, run and crouch speeds meet in the same short ease as the stick, not in one frame
	if (bVR)
	{
		SmoothSpeed = SmoothSpeed > 0.f ? FMath::Lerp(SmoothSpeed, M->MaxWalkSpeed, Ease) : M->MaxWalkSpeed;
		M->MaxWalkSpeed = SmoothSpeed;
	}
	// the floor's own grip (the level's FRICTION per sector): a slope slides the player down it at gravity
	// times its steepness times (1 - friction), unless it is NOSLIP; low friction also skates on the flat
	// (ice). Friction 1, nearly every floor, grips: no slide (user 2026-09-27)
	if (M->IsMovingOnGround() && !bSwimming && !IsClimbing())
	{
		float Friction = 1.f;
		bool bSlides = false;
		FVector Down = FVector::ZeroVector;
		const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
		if (const AOutlawsLevelActor* LF = Level.Get())
		{
			if (!LF->FloorUnder(Feet, Friction, bSlides, Down)) Friction = 1.f;
		}
		const float Grip = FMath::Max(0.03f, Friction);
		// VR on a gripping floor: speeds switch on and off at once (comfort; user 2026-09-29); ice and mud keep their slide
		const bool bInstant = bVR && Grip >= 0.999f;
		M->GroundFriction = 8.f * Grip;
		M->BrakingDecelerationWalking = bInstant ? VRInstantAcceleration : 3000.f * Grip;
		M->MaxAcceleration = bInstant ? VRInstantAcceleration : (bVR ? VRAcceleration : Acceleration) * Grip;   // no traction to push off on ice or mud
		const double Tan = Down.Size();
		if (bSlides && Friction < 0.999f && Tan > 1e-3)
		{
			const double SinA = Tan / FMath::Sqrt(1.0 + Tan * Tan);
			const float Slide = float(FMath::Abs(M->GetGravityZ()) * SinA * (1.0 - Friction));
			M->AddImpulse(Down / Tan * Slide * Dt, true);
		}
	}
	else if (bVR)
	{
		M->MaxAcceleration = VRAcceleration;   // off the ground: the instant value times the 0.5 air control would be full steering in the air and in water
	}
	if (IsClimbing() || MantleLeft > 0.f || IsMountedGun())
	{
		return;                          // (behind the gatling the player stays put)
	}
	if (bVR && bTeleportMove && !bSwimming && WalkTestLeft <= 0.f)
	{
		bRunning = false;
		return;                          // (the move stick aims the teleport instead: UpdateTeleport)
	}
	FVector2D S = MoveStick();
	if (WalkTestLeft > 0.f)
	{
		WalkTestLeft -= Dt;
		S = FVector2D(0.0, 1.0);             // (the walk test: straight ahead)
	}
	const float StickLen = S.Size();
	if (GFrameCounter > SmoothMoveFrame + 1) SmoothMove = FVector::ZeroVector;
	SmoothMoveFrame = GFrameCounter;
	if (StickLen < StickDeadZone)
	{
		if (bVR) bRunning = false;       // letting go of the stick ends a VR run
		// VR: the last push eases out over a few frames (a brief stop, not a wall)
		if (bVR && !SmoothMove.IsNearlyZero(0.02))
		{
			SmoothMove *= 1.f - Ease;
			AddMovementInput(SmoothMove.GetSafeNormal(), float(SmoothMove.Size()));
			bOutMoving = true;
		}
		else
		{
			SmoothMove = FVector::ZeroVector;
		}
		return;
	}
	// rescale past the dead zone so the slowest walk starts at zero
	S *= FMath::GetMappedRangeValueClamped(FVector2f(StickDeadZone, 1.f), FVector2f(0.f, 1.f), FMath::Min(StickLen, 1.f)) / StickLen;
	if (GatlingOut()) return;          // standing at the gatling (user 2026-09-29): the stick turns, it does not walk
	FRotator From = !bVR ? GetControlRotation()
		: MoveDirection == EOutlawsMoveDirection::OffHand ? OffAim()->GetComponentRotation()      // in water too: the VR option decides (user 2026-09-27)
		: MoveDirection == EOutlawsMoveDirection::DominantHand ? DominantAim()->GetComponentRotation()
		: Camera->GetComponentRotation();
	From.Roll = 0.0;
	if (!bSwimming || bSurfaceLocked)
	{
		From.Pitch = 0.0;               // on land the stick moves across the ground; in water it keeps the head's (or hand's) pitch, unless the surface holds the body
	}
	const FVector Want = From.Vector() * S.Y + FRotator(0.0, From.Yaw + 90.0, 0.0).Vector() * S.X;
	if (bVR)
	{
		// eased toward the stick: its wobble and the head's sway (the direction follows the head) no longer jerk the
		// body; a real change still lands within a fifth of a second at the default 0.07 s
		SmoothMove = FMath::Lerp(SmoothMove, Want, Ease);
		AddMovementInput(SmoothMove.GetSafeNormal(), float(SmoothMove.Size()));
	}
	else
	{
		AddMovementInput(From.Vector(), S.Y);
		AddMovementInput(FRotator(0.0, From.Yaw + 90.0, 0.0).Vector(), S.X);
	}
	bOutMoving = true;
}

void AOutlawsPlayer::Turning(float Dt, bool& bOutTurning)
{
	if (!bVR)
	{
		return;
	}
	const FVector2D T = TurnStick();
	// the turn stick (the right one, right-handed) held down crouches, let go stands (user 2026-09-29: held, not a
	// toggle; swapped with the barrel switch, now the click), as deep as the stick is held (user 2026-10-01: half down is
	// half a crouch, not two locked steps); begun only with the stick more down than sideways (not on a turn), then kept
	// while it is held down at all; under water a firm push down dives, once per push
	const float Down = float(-T.Y);
	const double StickNow = GetWorld()->GetTimeSeconds();
	if (T.Y > 0.5f) StickUpAt = StickNow;
	if (bSwimming)
	{
		// (BUG-SURFACEJOLT) not within a quarter second of a push up (a swim up): a stick let go springs back past the middle
		const bool bDive = bBarrelLatched ? Down > 0.3f : (Down > 0.7f && FMath::Abs(T.X) < 0.5f && !bMenuOpen && !bWheelOpen && StickNow - StickUpAt > 0.25);
		if (bDive != bBarrelLatched)
		{
			bBarrelLatched = bDive;
			if (bDive) SwimKick(-1.f);
			else if (bCrouchHeld && !bAutoCrouched)
			{
				// let go in the water: a crouch begun on land ends here too (it used to stay on, the view held down)
				bCrouchHeld = false;
				bStandPending = true;
			}
		}
	}
	else
	{
		const bool bDownNow = bBarrelLatched ? Down > 0.1f : (Down > 0.2f && FMath::Abs(T.X) < Down && !bMenuOpen && !bWheelOpen);
		if (bDownNow != bBarrelLatched)
		{
			bBarrelLatched = bDownNow;
			if (bDownNow && !IsClimbing() && MantleLeft <= 0.f)
			{
				bCrouchHeld = true;
				bAutoCrouched = false;
				CrouchFull = FMath::Max(0.f, HeadHeight() - CrouchEye);    // the full drop to crouch eye height, once; then the head leads
				bStandPending = false;
			}
			else if (!bDownNow && bCrouchHeld)
			{
				bCrouchHeld = false;
				bStandPending = true;    // the body grows back as room allows; the eye stays under its top
			}
		}
		if (bBarrelLatched && bCrouchHeld && !bAutoCrouched)
		{
			// from just past the start (a tenth) to most of the way down (0.85) is none to all of the crouch; the view's ride
			// (PlaceOrigin) smooths the stick's own jitter
			CrouchAmount = FMath::Clamp((Down - 0.1f) / 0.75f, 0.f, 1.f);
			CrouchDrop = CrouchFull * CrouchAmount;
		}
	}
	// ... pushed up it jumps (user 2026-09-29: the jump came off the grip), once per push
	if (!bJumpLatched && T.Y > 0.7f && FMath::Abs(T.X) < 0.5f && !bMenuOpen && !bWheelOpen)
	{
		bJumpLatched = true;
		DoJump();
	}
	else if (T.Y < 0.3f)
	{
		bJumpLatched = false;
	}
	if (bSnapTurn)
	{
		if (!bTurnLatched && FMath::Abs(T.X) > 0.7f)
		{
			TurnBy(FMath::Sign(T.X) * SnapTurnDegrees);
			bTurnLatched = true;
		}
		else if (FMath::Abs(T.X) < 0.3f)
		{
			bTurnLatched = false;
		}
	}
	else if (FMath::Abs(T.X) > StickDeadZone)
	{
		TurnBy(T.X * SmoothTurnSpeed * Dt);
		bOutTurning = true;
	}
}

void AOutlawsPlayer::TurnBy(float Degrees)
{
	// rotate about the head, not the capsule centre: shift the play space so the head stays put
	const FVector Head = Camera->GetComponentLocation();
	AddActorWorldRotation(FRotator(0.0, Degrees, 0.0));
	FVector Fix = Head - Camera->GetComponentLocation();
	Fix.Z = 0.0;
	VROrigin->AddWorldOffset(Fix);
}

void AOutlawsPlayer::Swimming()
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L || IsClimbing() || MantleLeft > 0.f)
	{
		return;
	}
	float Surface = 0.f;
	const float Feet = GetActorLocation().Z - GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
	const FVector C(GetActorLocation().X, GetActorLocation().Y, Feet + StandHalfHeight);   // a standing body's middle
	int32 TopSector = -1, InSector = -1;
	bool bAir = false;
	bool bWater = L->WaterSurfaceAt(C, Surface, TopSector, &InSector, &bAir) && C.Z < Surface;
	UCharacterMovementComponent* M = GetCharacterMovement();
	const double Now = GetWorld()->GetTimeSeconds();
	// (BUG-SURFACEJOLT) a drop of the body between frames while swimming that none of the swim's own steps made (they log
	// themselves): logged with what moved it, for the next campaign log
	if (bSwimming && SwimLastMiddle > -1e8f && SwimLastMiddle - C.Z > 20.f)
	{
		SwimLog(FString::Printf(TEXT("the body dropped %.0f cm since the last frame outside the swim's own steps (sector %d, mode %d, %.0f cm/s across, %.0f up)"),
			SwimLastMiddle - C.Z, InSector, int32(M->MovementMode), M->Velocity.Size2D(), M->Velocity.Z));
	}
	// (BUG-SURFACEJOLT) swimming, a moment with no water found under the middle (a seam between sectors, a gap in a floor
	// plan) while it is still well under the surface it last had is not the way out: that dropped the body into a fall and
	// back into the water with the fall's speed, a plunge under the surface. Out only once it lasts a quarter second
	if (bSwimming && !bWater && C.Z < SwimSurface - 20.f)
	{
		if (SwimLostSince < 0.0) { SwimLostSince = Now; SwimLog(TEXT("no water found under the middle: held in the swim for now")); }
		if (Now - SwimLostSince < 0.25) return;
	}
	else
	{
		SwimLostSince = -1.0;
	}
	if (bWater)
	{
		if (bSwimming && FMath::Abs(Surface - SwimSurface) > 10.f)
		{
			SwimLog(FString::Printf(TEXT("the surface moved %.0f -> %.0f cm (sector %d, top water sector %d)"), SwimSurface, Surface, InSector, TopSector));
		}
		if (bSwimming && bAir != bSwimSurfaceAir) SwimLog(bAir ? TEXT("under open water again (air over the water here)") : TEXT("into a flooded passage (a roof over the water here, no surface to hold the body)"));
		SwimSurface = Surface;
		SwimSector = TopSector;
		bSwimSurfaceAir = bAir;
	}
	// enter a little below the surface, leave a little above it (no flicker at the waterline)
	if (!bSwimming && bWater && C.Z < Surface - 10.f && GetWorld()->GetTimeSeconds() >= SwimExitUntil)
	{
		// a fall into the water: smsplash.wav at the surface (user 2026-09-27)
		if (M->Velocity.Z < -250.f) L->SpawnSplash(FVector(C.X, C.Y, Surface), M->Velocity.Z < -700.f);   // (seen as well as heard; a hard fall the big one)
		bSwimming = true;
		// a held lantern is put away going in, from either hand, as the wheel puts it away; it stays away on the way out
		// (REQ-LANTERNSWIM, user 2026-10-08)
		for (int32 H = 0; H < 2; ++H)
		{
			if (HeldTool[H] != TEXT("ilantern")) continue;
			ToggleGear(H, TEXT("ilantern"));
			UE_LOG(LogOutlaws, Log, TEXT("OLRT lantern put away going into the water (%s hand)"), H == 0 ? TEXT("left") : TEXT("right"));
			break;
		}
		if (bIsCrouched) UnCrouch();
		// the stick's crouch ends in the water: it kept the view down by the crouch's drop, the eyes under the surface
		// with the body floating at it (user 2026-09-30: stuck crouched after going down, hard to come up)
		if (bCrouchHeld && !bAutoCrouched)
		{
			bCrouchHeld = false;
			bStandPending = true;
		}
		// the fall carries on under the surface and the water slows it (UpdateGravity): a big fall goes deep
		const FVector In = M->Velocity;
		M->SetMovementMode(MOVE_Flying);
		M->Velocity = FVector(In.X * 0.5, In.Y * 0.5, In.Z);
		SwimLog(FString::Printf(TEXT("into the water at %.0f cm/s down (sector %d)"), -In.Z, InSector));
	}
	else if (bSwimming && (!bWater || C.Z > Surface + 15.f))
	{
		SwimLog(FString::Printf(TEXT("out of the water: %s (%.0f cm/s up)"), bWater ? TEXT("above the surface") : C.Z >= SwimSurface - 20.f ? TEXT("up past the surface") : TEXT("no water found for a quarter second"), M->Velocity.Z));
		// jumped (or kicked) up out of it: the same splash, where the feet leave the water (user 2026-09-28); wading
		// out onto a bank stays quiet
		if (M->Velocity.Z > 200.f) L->SpawnSplash(FVector(C.X, C.Y, bWater ? Surface : Feet), false);
		bSwimming = false;
		M->SetMovementMode(MOVE_Falling);
	}
}

bool GSwimStrokeTest = false;             // -OLSwimSpeedTest: the strokes run without a headset

void AOutlawsPlayer::SwimStrokes(float Dt)
{
	UCharacterMovementComponent* M = GetCharacterMovement();
	if ((!bVR && !GSwimStrokeTest) || !bSwimming || !M || M->MovementMode != MOVE_Flying)
	{
		SwimCarry = FVector::ZeroVector;          // (out of the water: nothing left owed)
		StrokeDiveCharge = 0.f;
		if (M) M->BrakingDecelerationFlying = 600.f;
		return;
	}
	if (Dt <= 0.f || IsClimbing() || bMenuOpen || bWheelOpen || bDeadView) return;
	const double Now = GetWorld()->GetTimeSeconds();
	// (user 2026-10-03: the push "ends too quick"): the water slows a gliding body less than one at rest, and what is
	// left of the last strokes' push keeps coming over the next half second or so
	M->BrakingDecelerationFlying = Now < StrokeGlideUntil ? 250.f : 600.f;
	const FVector Due = SwimCarry * (1.0 - FMath::Exp(-Dt / 0.4));
	SwimCarry -= Due;
	// (user 2026-10-05, BUG-SWIMSPEED: arm swimming "far too fast") the strokes reach the land speed and no more: the
	// walk (or run) at the Movement Speed dial's pace, so the dial governs swimming too. It was a flat 7 m/s floor,
	// three times the dial's 100 % walk and seven times its 40 %
	const float Cap = SwimSpeedFraction > 0.f ? M->MaxFlySpeed / SwimSpeedFraction : M->MaxFlySpeed;
	if (!Due.IsNearlyZero())
	{
		const FVector D = bSurfaceLocked ? FVector(Due.X, Due.Y, 0.0) : Due;
		const FVector Dn = D.GetSafeNormal();
		const double Room = Cap - FVector::DotProduct(M->Velocity, Dn);
		const float Before = float(M->Velocity.Size());
		if (Room > 0.0) M->Velocity += Dn * FMath::Min(D.Size(), Room);
		M->Velocity = M->Velocity.GetClampedToMaxSize(FMath::Max(Cap, Before));
	}
	// each hand's motion against the body past 70 cm/s is a stroke (a breaststroke's sweep runs 150-250 cm/s): 4.5 cm/s
	// of speed per cm/s over that, each second, per hand; a full two-handed stroke (about 0.4 s at 200 cm/s) reaches
	// about 5 m/s, glided down over a couple of metres
	// (user 2026-10-03, BUG-SWIM: the push by the hands' own direction "still feels weird ... a swinging arm motion
	// trigger, and it will just follow whatever direction your head is facing") an arm swung past a stroke's speed, any
	// way, pushes the body along the view, as hard as the swing is fast: look down to dive, up to rise
	FVector Push = FVector::ZeroVector;
	float Swing = 0.f;
	for (int32 H = 0; H < 2; ++H)
	{
		const float Sweep = FMath::Min(float(HandVelocity[H].Size()), 400.f);     // (a snap turn's jump is not a stroke)
		if (Sweep > 70.f) Swing += 4.5f * (Sweep - 70.f);
	}
	if (Swing > 0.f) Push = Camera->GetForwardVector() * Swing;
	// (BUG-SUBMERGE, user 2026-10-14: looking down and swimming forward no longer went under at the surface; BUG-SURFACEJOLT
	// had taken every arm swing out of the surface's dive) a forward stroke dives again: a hand pulled back past the body
	// (most of its sweep against the view's level way) for a fifth of a second of pull lately. A swim-up stroke pushes the
	// water down, a shot's swing or an aim goes forward or up, and a hand that has just fired is left out: none of them dive
	{
		const FVector Fl = Camera->GetForwardVector().GetSafeNormal2D();
		bool bPull = false;
		for (int32 H = 0; H < 2; ++H)
		{
			const float Sweep = FMath::Min(float(HandVelocity[H].Size()), 400.f);
			if (Sweep <= 70.f || FireCooldown[H] > 0.f || Fl.IsNearlyZero()) continue;
			if (float(FVector::DotProduct(HandVelocity[H], -Fl)) > 0.6f * float(HandVelocity[H].Size())) bPull = true;
		}
		StrokeDiveCharge = FMath::Clamp(StrokeDiveCharge + (bPull ? Dt : -0.5f * Dt), 0.f, 0.6f);
		if (bPull && StrokeDiveCharge >= 0.2f) StrokeDiveUntil = Now + 0.3;
	}
	// (the surface holds the body, UpdateGravity: the stroke runs along it)
	if (bSurfaceLocked) Push.Z = 0.0;
	if (Push.IsNearlyZero()) return;
	const FVector Way = Push.GetSafeNormal();
	StrokeUntil = Now + 0.3;
	StrokeGlideUntil = Now + 1.5;                 // (the glide after it: UpdateGravity's cap on a fast rise leaves it be)
	// half the push now, half carried into the glide
	FVector V = M->Velocity + Push * (0.5 * Dt);
	SwimCarry += Push * (0.5 * Dt);
	// no faster than 7 m/s along the stroke's way (Cap), so strokes in a rhythm build speed (the water's friction glides
	// it down after)
	const float CarryAlong = static_cast<float>(FVector::DotProduct(SwimCarry, Way));
	if (CarryAlong > Cap) SwimCarry -= Way * (CarryAlong - Cap);
	const float Along = static_cast<float>(FVector::DotProduct(V, Way));
	if (Along > Cap) V -= Way * (Along - Cap);
	// (and no faster overall: a stroke across the stick's swim, or a current, adds nothing past it)
	V = V.GetClampedToMaxSize(FMath::Max(Cap, float(M->Velocity.Size())));
	M->Velocity = V;
}

void AOutlawsPlayer::UpdateGravity(float Dt)
{
	OL_PERF_SCOPE("Player: gravity");
	// the sector's GRAVITY: the land feel is tuned for townex's -60 (the Jedi engine's player ignored
	// sector gravity, a flat 150 u/s^2; Outlaws gives each sector its own, -10 in water), so every sector
	// pulls in proportion.  In water (flying movement has no gravity of its own) the body sinks slowly
	AOutlawsLevelActor* L = Level.Get();
	UCharacterMovementComponent* M = GetCharacterMovement();
	if (!L || IsClimbing() || MantleLeft > 0.f)
	{
		return;
	}
	const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	const float Factor = FMath::Clamp(-L->GravityAt(Feet) / 60.f, 0.f, 4.f);
	M->GravityScale = BaseGravityScale * Factor / (bJumpBoost ? JumpStretch * JumpStretch : 1.f);     // (a long jump: OnJumped)
	if (!bSwimming) { bSurfaceLocked = false; bDived = false; SwimLastMiddle = -1e9f; }
	// (BUG-SURFACEJOLT) no stepping in the water: the fly mode steps over what the swim runs into as the walk does, up the
	// step height (105 cm), across, and back down as far again to whatever is under it. Treading over the cliff's shallow
	// sloped water (sector 350) the step's way down dropped the body a metre under the surface in one frame. A swimmer
	// slides along what it meets (a slope lifts it); out of the water the walk's steps are back
	const float WantStep = bSwimming && M->MovementMode == MOVE_Flying ? 0.f : StepHeight;
	if (M->MaxStepHeight != WantStep) M->MaxStepHeight = WantStep;
	if (bSwimming && M->MovementMode == MOVE_Flying)
	{
		const float Pull = 980.f * BaseGravityScale * Factor;
		// the sink keeps pace with the swim: the teleport pace (and the speed slider) halve the swim speed, and a full
		// sink against it made surfacing a struggle (the smoke test's swim up failed with teleport moves saved)
		const float SinkCap = SwimSinkSpeed * (L ? L->Pace() : 1.f);
		const double Now = GetWorld()->GetTimeSeconds();
		float Middle = float(Feet.Z) + StandHalfHeight;                // a standing body's middle, as Swimming() measures it
		const float FloatZ = SwimSurface - SwimFloatDepth;
		// at the surface the water holds the body up (user 2026-09-29): no sink, and the stick and the strokes run
		// along the surface (Locomotion, SwimStrokes), unless the head is tilted down past DivePitch while moving, a
		// dive; a kick down (crouch) dives too. A body still plunging from a fall is not caught (a big fall goes deep)
		// (BUG-SURFACEJOLT, user 2026-10-12: at the surface, swimming up or firing the revolver, a jolt a couple of feet
		// down and a swim back up) held at the surface, only the stick with the head down, or a kick down, lets go. An
		// arm's swing (a stroke, an aim, a fan of the hammer) counted as moving, so looking down at the water while the
		// arms pushed (a swim-up stroke, a shot's swing) was a dive: the hold let go and the stroke drove the body under
		// along the view. Under the surface the strokes steer as before (look down to dive, up to rise)
		const bool bStick = !GetLastMovementInputVector().IsNearlyZero();
		const bool bMoving = bStick || Now < StrokeUntil;
		const bool bHeadDown = float(Camera->GetComponentRotation().Pitch) < DivePitch;
		const bool bKick = Now < DiveUntil;
		// (BUG-SUBMERGE) and a forward stroke with the head down (SwimStrokes: the hands pulled back past the body)
		const bool bStrokeDive = Now < StrokeDiveUntil;
		const bool bDiving = bKick || (bHeadDown && (bSurfaceLocked ? (bStick || bStrokeDive) : bMoving));
		const bool bWasLocked = bSurfaceLocked;
		// (BUG-SURFACEJOLT, the cliff's sector 350: water 90 cm deep on a sloped floor beside the deep LIFT2) a body held at
		// the surface found well under its float depth with no dive and no plunge was moved there by something else
		// between frames (the fly mode's move across a shallow sector's sloped floor dropped it a metre in one frame): it
		// goes back up to the float depth at once (swept: under a roof it stays) and the hold keeps it
		if (bSurfaceLocked && bSwimSurfaceAir && !bDiving && Middle < FloatZ - 25.f && M->Velocity.Z >= -SinkCap)
		{
			const float Was = Middle;
			AddActorWorldOffset(FVector(0.0, 0.0, FloatZ - Middle), true);
			Middle = float(GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight()) + StandHalfHeight;
			SwimLog(FString::Printf(TEXT("held at the surface but %.0f cm under its float depth with no dive: put back up %.0f cm"), FloatZ - Was, Middle - Was));
		}
		if (bDiving && bSurfaceLocked) bDived = true;
		// a slow, short dive stopped a hand's breadth down (the movement speed at a quarter) was caught again at once
		// (BUG-SURFACEJOLT, the town's flooded passages 580 and 524: water under a roof, no air over it) only a real surface
		// (air over the water) holds the body; a passage's roof is no surface. It was taken for one: the body was held with
		// its head in the rock at the roof of one passage and let go a metre on where the next passage's roof stood 6 m higher
		bSurfaceLocked = bSwimSurfaceAir && !bDiving && M->Velocity.Z >= -SinkCap && (bDived ? Middle >= FloatZ - 5.f : Middle > FloatZ - 40.f);
		if (bWasLocked != bSurfaceLocked)
		{
			SwimLog(bSurfaceLocked ? TEXT("the surface holds the body") : FString::Printf(TEXT("the surface lets go: %s"),
				!bSwimSurfaceAir ? TEXT("no surface here (a flooded passage's roof)") : bKick ? TEXT("a kick down") : bDiving && bStrokeDive && !bStick ? TEXT("a forward stroke with the head down (a dive)") : bDiving ? TEXT("the stick with the head down (a dive)") : M->Velocity.Z < -SinkCap ? TEXT("sinking faster than the water lets a body sink")
				: TEXT("too far under the float depth")));
		}
		if (bSurfaceLocked)
		{
			bDived = false;
			// the swim's vertical part goes; the body eases to its float depth (about a quarter second)
			SinkVel = 0.f;
			M->Velocity.Z = 0.0;
			const float Ease = (FloatZ - Middle) * FMath::Min(1.f, 5.f * Dt);
			if (Ease < -3.f) SwimLog(FString::Printf(TEXT("the hold eases the body down %.1f cm this frame (%.0f cm over its float depth)"), -Ease, Middle - FloatZ));
			AddActorWorldOffset(FVector(0.0, 0.0, Ease), true);
		}
		else if (M->Velocity.Z < -SinkCap)
		{
			// still plunging from a fall: the water's drag bleeds the speed off (stopping distance ~ speed / 8:
			// about 1.2 m from a 5 m jump, several metres at terminal velocity; user 2026-09-27)
			M->Velocity.Z = -SinkCap + (M->Velocity.Z + SinkCap) * FMath::Exp(-8.f * Dt);
			SinkVel = SinkCap;
		}
		else
		{
			// the sector's gravity pulls the body down, up to the water's sink speed (user 2026-09-29: gravity should
			// still apply under water, the sectors' -10 is for that), as a drift of the body rather than through the
			// fly mode's velocity: that mode's braking (600 cm/s2 with no stick) undid a pull of a sixth of g each
			// frame, so the body hung, and a clamp on the velocity instead undid the swim up (user 2026-09-29: could
			// not resurface). A swim up beats the drift (WATER.PHY MAX_VEL 15 u/s: 450 cm/s, against 90); what is
			// still sinking in the velocity itself counts toward the drift
			SinkVel = FMath::Min(SinkVel + Pull * Dt, SinkCap);
			const float Drift = FMath::Max(0.f, SinkVel + FMath::Min(0.f, float(M->Velocity.Z)));
			AddActorWorldOffset(FVector(0.0, 0.0, -Drift * Dt), true);
		}
		// a kick up faster than swimming goes: the water takes the extra off (about a metre's worth); a stroke's own glide
		// keeps its speed (user 2026-10-02)
		if (M->Velocity.Z > M->MaxFlySpeed && Now >= StrokeGlideUntil)
		{
			M->Velocity.Z = M->MaxFlySpeed + (M->Velocity.Z - M->MaxFlySpeed) * FMath::Exp(-4.f * Dt);
		}
		SwimLastMiddle = float(GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight()) + StandHalfHeight;
		// (BUG-SURFACEJOLT) the swim's state, every 2 s while swimming (the campaign logs had none)
		if (Now >= SwimLogNext)
		{
			SwimLogNext = Now + 2.0;
			UE_LOG(LogOutlaws, Log, TEXT("OLRT swim: hold %s, middle %.0f cm under the surface (float depth %.0f), %.0f cm/s up, surface z %.0f (sector %d), head pitch %.0f, stick %d, stroke %d"),
				bSurfaceLocked ? TEXT("on") : bDived ? TEXT("off (dived)") : TEXT("off"), SwimSurface - Middle, SwimFloatDepth, M->Velocity.Z, SwimSurface, SwimSector,
				Camera->GetComponentRotation().Pitch, bStick ? 1 : 0, Now < StrokeUntil ? 1 : 0);
		}
	}
}

void AOutlawsPlayer::SwimLog(const FString& What)
{
	// (BUG-SURFACEJOLT) a change in the swim, logged with its cause; at most 8 a second (a flicker would flood the log)
	const double Now = FPlatformTime::Seconds();
	if (Now - SwimLogWindow > 1.0) { SwimLogWindow = Now; SwimLogCount = 0; }
	if (++SwimLogCount > 8) return;
	const UCharacterMovementComponent* M = GetCharacterMovement();
	const float Middle = float(GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight()) + StandHalfHeight;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT swim: %s (middle %.0f cm under the surface z %.0f, %.0f cm/s up)"), *What, SwimSurface - Middle, SwimSurface, M ? M->Velocity.Z : 0.0);
}

// ---- climbing ------------------------------------------------------------------------------------

void AOutlawsPlayer::Climbing(float Dt)
{
	UCharacterMovementComponent* M = GetCharacterMovement();
	// a mantle in progress: ease the body onto the ledge
	if (MantleLeft > 0.f)
	{
		MantleLeft = FMath::Max(0.f, MantleLeft - Dt);
		const float A = FMath::InterpEaseInOut(0.f, 1.f, 1.f - MantleLeft / 0.25f, 2.f);
		SetActorLocation(FMath::Lerp(MantleFrom, MantleTo, A), false, nullptr, ETeleportType::TeleportPhysics);
		M->Velocity = FVector::ZeroVector;
		if (MantleLeft <= 0.f)
		{
			if (bVaulting)
			{
				M->SetMovementMode(MOVE_Falling);
				M->Velocity = LedgeIn * 250.0 + FVector(0.0, 0.0, 50.0);
				bVaulting = false;
			}
			else
			{
				M->SetMovementMode(MOVE_Walking);
			}
		}
		return;
	}
	if (!bVR || Dt <= 0.f)
	{
		return;
	}
	for (int32 H = 0; H < 2; ++H)
	{
		// hand motion relative to the body: what a release flings the player with
		const FVector Rel = GripOf(H)->GetComponentLocation() - GetActorLocation();
		HandVelocity[H] = FMath::Lerp(HandVelocity[H], (Rel - PrevHandPos[H]) / Dt, 0.5f);
		PrevHandPos[H] = Rel;
		// and through the world, which a room-scale step carries too (the capsule follows the head
		// without a velocity); a jump of over 4 m/s is a recenter or mantle, not a hand
		const FVector WorldP = GripOf(H)->GetComponentLocation();
		FVector WorldV = PrevHandWorld[H].IsZero() ? FVector::ZeroVector : (WorldP - PrevHandWorld[H]) / Dt;
		if (WorldV.Size() > 400.f) WorldV = FVector::ZeroVector;
		HandWorldVel[H] = FMath::Lerp(HandWorldVel[H], WorldV, 0.5f);
		PrevHandWorld[H] = WorldP;
		// a hanging lamp struck by the hand swings (user 2026-10-01)
		if (HandVelocity[H].Size() > OutlawsFeel::HitSpeedLight) if (AOutlawsLevelActor* LH = Level.Get()) LH->HandNearHanging(WorldP, HandVelocity[H]);
		// a push: the hand driven into a wall at a press's speed presses a switch there, no grip needed (user
		// 2026-09-28); the fingertips lead (6 cm along the hand's pointing way)
		PushCool[H] = FMath::Max(0.f, PushCool[H] - Dt);
		if (bVR && PushCool[H] <= 0.f && GrabHand != H && HandWorldVel[H].Size() > 45.f)
		{
			const FVector Way = HandWorldVel[H].GetSafeNormal();
			const FVector Tip = WorldP + AimOf(H)->GetForwardVector() * 6.0;
			FCollisionQueryParams PQ(SCENE_QUERY_STAT(OutlawsPush), true, this);
			if (AOutlawsLevelActor* LP = Level.Get()) LP->IgnoreHiddenOverlaps(PQ);
			FHitResult PHit;
			if (GetWorld()->LineTraceSingleByChannel(PHit, Tip - Way * 4.0, Tip + Way * 3.0, ECC_Visibility, PQ)
				&& FVector::DotProduct(Way, -PHit.ImpactNormal) > 0.5)
			{
				AOutlawsLevelActor* LP = Level.Get();
				if (LP && LP->PushByHand(PHit.ImpactPoint + Way * 1.0))
				{
					PushCool[H] = 0.8f;
					Pulse(H, 0.5f);
				}
			}
		}
		const float G = Axis(H == 0 ? TEXT("OLVR_LeftGrip") : TEXT("OLVR_RightGrip"));
		if (G < 0.4f)
		{
			bGripArmed[H] = true;            // one action per real squeeze (no re-fire around 0.6)
			bSupport[H] = false;
			bToolSupport[H] = false;
			bRegrip[H] = false;              // (let go during a button reload: it stays free when the gun shuts)
		}
		// the wheel up on this hand's button: a squeeze drops one of what its laser points at, the trigger still picks
		// (user 2026-10-02: "this would allow co op players to pass items or weapons between each other")
		if (bWheelOpen && bWheelHeld[H] && G > 0.6f && bGripArmed[H] && !HeldItem[H].IsValid() && GrabHand != H)
		{
			bGripArmed[H] = false;
			WheelDrop(H);
			PrevGrip[H] = G;
			continue;
		}
		// the grip editor (dev): a free fist on what the other hand holds takes it to be placed, nothing else happens
		if (bGripEditor && GripEditorInput(H, G))
		{
			PrevGrip[H] = G;
			continue;
		}
		// the shovel let go of (or put away) by the rear hand, or this hand taking a tool: off the shaft (a weapon in this
		// hand stays, put away, while it holds the shaft)
		if (bToolSupport[H] && (HeldTool[1 - H].IsEmpty() || !HeldTool[H].IsEmpty()))
		{
			bToolSupport[H] = false;
		}
		// a hand squeezing on the other hand's shovel shaft takes it two-handed (user 2026-09-28), empty or not
		if (!bSupport[H] && !bToolSupport[H] && G > 0.6f && bGripArmed[H] && NearToolShaft(H))
		{
			bToolSupport[H] = true;
			bGripArmed[H] = false;
			Pulse(H, 0.3f);
		}
		// an empty hand squeezing on the other hand's long gun takes its forend (and nothing else)
		if (!bSupport[H] && G > 0.6f && bGripArmed[H] && NearForegrip(H))
		{
			bSupport[H] = true;
			bGripArmed[H] = false;
			// the hand snaps to the forend's spot, wherever along it the hand closed (user 2026-09-27)
			SupportAnchor[1 - H] = ForendSpot(Equipped[1 - H]);
			Pulse(H, 0.3f);
		}
		// a bottle in one hand and a weapon in the other, squeezed with the hands together: they trade hands (user
		// 2026-09-28), the bottle flying into the other fist and the weapon, rounds and all, into this one
		if (G > 0.6f && bGripArmed[H] && GrabHand != H && GrabHand != 1 - H && !bWheelHeld[H] && !bWheelHeld[1 - H]
			&& ((HeldBottle[H] && Equipped[1 - H] >= 0) || (Equipped[H] >= 0 && HeldBottle[1 - H]))
			&& FVector::Dist(GripOf(H)->GetComponentLocation(), GripOf(1 - H)->GetComponentLocation()) < SwapReach())
		{
			bGripArmed[H] = false;
			const int32 Bh = HeldBottle[H] ? H : 1 - H;
			PassBottle(Bh, 1 - Bh);
			SwapWeaponHands(1 - Bh, Bh);
			Pulse(H, 0.4f);
			Pulse(1 - H, 0.4f);
		}
		// a fist squeezed against the other hand while that one holds a weapon: the weapon changes hands, as if
		// passed from one to the other (user 2026-09-28); the forend grip above comes first for a long gun
		if (!bSupport[H] && G > 0.6f && bGripArmed[H] && Equipped[H] < 0 && Equipped[1 - H] >= 0 && !HeldItem[H].IsValid() && !HeldBottle[H]
			&& HeldTool[H].IsEmpty() && GrabHand != H && GrabHand != 1 - H && !bWheelHeld[H] && !bWheelHeld[1 - H]
			&& FVector::Dist(GripOf(H)->GetComponentLocation(), GripOf(1 - H)->GetComponentLocation()) < SwapReach())
		{
			bGripArmed[H] = false;
			SwapWeaponHands(1 - H, H);
			Pulse(H, 0.4f);
			Pulse(1 - H, 0.2f);
		}
		// ... and a bottle the same way (user 2026-09-28)
		if (!bSupport[H] && G > 0.6f && bGripArmed[H] && Equipped[H] < 0 && !HeldBottle[H] && HeldBottle[1 - H] && !HeldItem[H].IsValid()
			&& HeldTool[H].IsEmpty() && GrabHand != H && GrabHand != 1 - H && !bWheelHeld[H]
			&& FVector::Dist(GripOf(H)->GetComponentLocation(), GripOf(1 - H)->GetComponentLocation()) < SwapReach())
		{
			bGripArmed[H] = false;
			PassBottle(1 - H, H);
			Pulse(H, 0.4f);
			Pulse(1 - H, 0.2f);
		}
		// two different one-handed weapons (a revolver and dynamite...): the same squeeze with the hands together trades
		// them (user 2026-09-28)
		if (!bSupport[H] && G > 0.6f && bGripArmed[H] && Equipped[H] >= 0 && Equipped[1 - H] >= 0 && Equipped[H] != Equipped[1 - H]
			&& GrabHand != H && GrabHand != 1 - H && !bWheelHeld[H] && !bWheelHeld[1 - H]
			&& FVector::Dist(GripOf(H)->GetComponentLocation(), GripOf(1 - H)->GetComponentLocation()) < SwapReach() && TradeWeapons())
		{
			bGripArmed[H] = false;
			Pulse(H, 0.4f);
			Pulse(1 - H, 0.4f);
		}
		// the held gear (the lantern, the plate, the map) changes hands as a weapon does (user 2026-10-01): a free fist
		// squeezed against the hand holding it takes it; with a weapon in the other hand the two trade
		if (!bSupport[H] && G > 0.6f && bGripArmed[H] && GrabHand != H && GrabHand != 1 - H && !bWheelHeld[H] && !bWheelHeld[1 - H]
			&& !HeldItem[H].IsValid() && !HeldBottle[H] && !HeldItem[1 - H].IsValid()
			&& FVector::Dist(GripOf(H)->GetComponentLocation(), GripOf(1 - H)->GetComponentLocation()) < SwapReach())
		{
			const bool bTake = IsGear(HeldTool[1 - H]) && HeldTool[H].IsEmpty() && Equipped[H] < 0 && !HeldBottle[1 - H];
			const bool bGiveHere = IsGear(HeldTool[H]) && Equipped[1 - H] >= 0;          // gear here, a weapon there
			// a weapon here, gear there (not while that hand holds this weapon's forend, the lantern hanging under it)
			const bool bTakeThere = IsGear(HeldTool[1 - H]) && Equipped[H] >= 0 && !bSupport[1 - H];
			// gear here and a bottle or a prop there (held by its grip: this hand squeezes): they trade (user 2026-10-01: the
			// shield would not change hands with other things)
			const bool bForBottle = IsGear(HeldTool[H]) && HeldBottle[1 - H] != nullptr;
			// gear and a tool or other gear, one in each hand: they trade
			const bool bToolTrade = !HeldTool[H].IsEmpty() && !HeldTool[1 - H].IsEmpty() && HeldTool[H] != HeldTool[1 - H]
				&& (IsGear(HeldTool[H]) || IsGear(HeldTool[1 - H]));
			if (bForBottle || bToolTrade)
			{
				bGripArmed[H] = false;
				const FString Here = HeldTool[H], There = HeldTool[1 - H];
				DropTool(H);
				if (bForBottle)
				{
					PassBottle(1 - H, H);
				}
				else
				{
					DropTool(1 - H);
					EquipTool(H, There);
				}
				EquipTool(1 - H, Here);
				Pulse(H, 0.4f);
				Pulse(1 - H, 0.4f);
				UE_LOG(LogOutlaws, Log, TEXT("OLRT the %s traded for the %s"), *Here, bForBottle ? TEXT("held thing") : *There);
			}
			else if (bTake || bGiveHere || bTakeThere)
			{
				bGripArmed[H] = false;
				const int32 Gh = bGiveHere ? H : 1 - H;       // the hand with the gear
				const FString Item = HeldTool[Gh];
				DropTool(Gh);
				if (!bTake) SwapWeaponHands(1 - Gh, Gh);
				EquipTool(1 - Gh, Item);
				Pulse(H, 0.4f);
				Pulse(1 - H, 0.3f);
				UE_LOG(LogOutlaws, Log, TEXT("OLRT the %s changes hands%s"), *Item, bTake ? TEXT("") : TEXT(" (traded for the weapon)"));
			}
		}
		if (G > 0.6f && bGripArmed[H] && Equipped[H] >= 0 && GrabHand != H && !bWheelHeld[H])
		{
			// a hand holding a gun takes whatever glows for it as an empty hand does (user 2026-10-01): the gun set aside
			// while it holds the thing (to the other hand if that is free) and back when it is let go; or it pushes a door
			// or a switch, or takes a ledge
			bGripArmed[H] = false;
			NoteDoorUse(H);
			AOutlawsLevelActor* L = Level.Get();
			if ((UOutlawsCoop::IsActive(this) && UOutlawsCoop::TryRevive(this, GripOf(H)->GetComponentLocation()))      // (co-op: the other player's body)
				|| TryTakeEnemyFromOther(H) || TryHold(H) || TryHoldBottle(H) || TryGrabEnemy(H) || TryHandUse(H) || TryGrab(H)
				|| (L && L->UseByHand(Camera->GetComponentLocation(), GripOf(H)->GetComponentLocation(), true))
				|| TryReachGrab(H) || TryPull(H))
			{
				Pulse(H, 0.5f);
				bGripUsed[H] = true;
			}
			else
			{
				bGripUsed[H] = false;
				GripOnNothing(H);
			}
		}
		else if (HeldBottle[H] && G < 0.4f)
		{
			// held only while the grip is (user 2026-09-29): let go, it goes with the hand's motion, any way at all; but with
			// a throw lined up (the trigger pulled, the arc out) along the arc all the same (user 2026-09-29: the grip let
			// slip with the trigger dropped it)
			ReleaseBottle(H, bBottleReady[H]);
		}
		else if (G > 0.6f && bGripArmed[H] && GrabHand != H && !HeldItem[H].IsValid() && !HeldBottle[H] && Equipped[H] < 0 && !bWheelHeld[H])
		{
			bGripArmed[H] = false;
			NoteDoorUse(H);
			// the first thing that applies: take an item or a bottle, use a door or switch, hold a ledge, and
			// only then work a lift or platform the hand is on
			const bool bDone = (UOutlawsCoop::IsActive(this) && UOutlawsCoop::TryRevive(this, GripOf(H)->GetComponentLocation()))     // (co-op: the other player's body)
				|| TryTakeEnemyFromOther(H) || TryHold(H) || TryHoldBottle(H) || TryGrabEnemy(H) || TryHandUse(H) || TryGrab(H)
				|| TryReachGrab(H) || TryPull(H);       // (the reach: a bad guy, a bottle or a prop pointed near, user 2026-09-29)
			if (!bDone)
			{
				AOutlawsLevelActor* L = Level.Get();
				if (L && L->UseByHand(Camera->GetComponentLocation(), GripOf(H)->GetComponentLocation(), true))
				{
					Pulse(H, 0.5f);
				}
				else
				{
					GripOnNothing(H);
				}
			}
		}
		else if (G < 0.4f && PrevGrip[H] >= 0.4f)
		{
			if (HeldItem[H].IsValid())
			{
				ReleaseHeld(H);
			}
			else if (GrabHand == H)
			{
				ReleaseGrab(true);
			}
		}
		PrevGrip[H] = G;
		PushDoors(H);
	}
	if (GrabHand < 0)
	{
		return;
	}
	// keep the holding hand where it grabbed: pulling the hand down lifts the body
	M->Velocity = FVector::ZeroVector;
	const FVector Delta = GrabPoint - GripOf(GrabHand)->GetComponentLocation();
	FHitResult Hit;
	M->SafeMoveUpdatedComponent(Delta, GetActorQuat(), true, Hit);
	const float Feet = GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	if (Feet > LedgeTopZ - 25.f)
	{
		StartMantle();
	}
}

bool AOutlawsPlayer::FindLedge(int32 Hand, FVector& OutGrabPoint, FVector& OutIn, FVector& OutTop, bool bLog) const
{
	// (TryGrab's probe, on its own: the glow shows the same ledges the grip takes, user 2026-10-01)
	const UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	const FVector P = GripOf(Hand)->GetComponentLocation();
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsGrab), true, this);
	if (const AOutlawsLevelActor* LQ = Level.Get()) LQ->IgnorePassedOverlaps(Q);     // (what the body walks through holds no ledge)
	// a hand touching something solid, or within reach of it (user 2026-09-28: the grab was too precise): the wall is
	// looked for out to 40 cm past the hand along the line from the eyes, and the hand snaps to the lip it finds
	const bool bTouching = World->OverlapBlockingTestByChannel(P, FQuat::Identity, ECC_Visibility, FCollisionShape::MakeSphere(9.f), Q, PlayerNoPawns());
	// which way is "into the ledge": from the eyes through the hand to the wall face
	const FVector Eye = Camera->GetComponentLocation();
	const FVector Dir = (P - Eye).GetSafeNormal();
	FHitResult Wall;
	const bool bWall = World->LineTraceSingleByChannel(Wall, Eye, P + Dir * 40.0, ECC_Visibility, Q, PlayerNoPawns())
		&& FVector::Dist(FVector(Wall.ImpactPoint), P) <= 40.0;
	if (!bTouching && !bWall)
	{
		return false;
	}
	FVector In = bWall ? -FVector(Wall.ImpactNormal.X, Wall.ImpactNormal.Y, 0.0) : FVector(Dir.X, Dir.Y, 0.0);
	if (In.SizeSquared() < 0.01)
	{
		In = FVector(Dir.X, Dir.Y, 0.0);          // palm on top of the ledge
	}
	In = In.GetSafeNormal();
	if (In.IsNearlyZero())
	{
		In = FRotator(0.0, Camera->GetComponentRotation().Yaw, 0.0).Vector();
	}
	// (a top up to 60 degrees counts: the town's sloped roofs, BUG-SLOPES)
	// a ledge: a walkable top just past the wall face, level with the hand (-20 .. +40 cm); probed
	// close to the face first so thin tops (fences, rails) count
	FHitResult Top;
	bool bTop = false;
	for (const double Past : { 4.0, 10.0, 20.0 })
	{
		const FVector Probe = (bWall ? Wall.ImpactPoint : P) + In * Past;
		FHitResult T;
		if (World->LineTraceSingleByChannel(T, Probe + FVector(0.0, 0.0, 45.0), Probe - FVector(0.0, 0.0, 30.0), ECC_Visibility, Q, PlayerNoPawns()) &&
			!T.bStartPenetrating && T.ImpactNormal.Z >= 0.5 && T.ImpactPoint.Z - P.Z <= 45.0 && T.ImpactPoint.Z - P.Z >= -30.0)
		{
			Top = T;
			bTop = true;
			break;
		}
	}
	if (!bTop)
	{
		if (bLog) UE_LOG(LogOutlaws, Log, TEXT("OLRT VR grab (%s hand): no ledge at %s (wall %d, top %d)"), Hand == 0 ? TEXT("left") : TEXT("right"),
			*P.ToString(), bWall ? 1 : 0, bTop ? 1 : 0);
		return false;
	}
	// the top has to be reachable from the hand: open air from the hand's side of the wall straight up to just over the
	// top, then in over it (user 2026-10-01: a hand up by a ceiling found the floor of the room above through it, the
	// search starting 45 cm over the hand, and the mantle put the player up there, through the ceiling)
	{
		const FVector Face = bWall ? FVector(Wall.ImpactPoint) : FVector(Top.ImpactPoint) - In * 4.0;
		const FVector Out = Face - In * 3.0;
		const double Over = FMath::Max(double(Top.ImpactPoint.Z), P.Z) + 12.0;
		const FVector Up(Out.X, Out.Y, Over), Above(Top.ImpactPoint.X, Top.ImpactPoint.Y, Over);
		FHitResult Roof;
		const bool bRoof = World->LineTraceSingleByChannel(Roof, FVector(Out.X, Out.Y, P.Z), Up, ECC_Visibility, Q, PlayerNoPawns())
			|| World->LineTraceSingleByChannel(Roof, Up, Above, ECC_Visibility, Q, PlayerNoPawns());
		if (bRoof)
		{
			if (bLog) UE_LOG(LogOutlaws, Log, TEXT("OLRT VR grab (%s hand): top z %.0f shut off by z %.0f over the hand (z %.0f): no ledge"), Hand == 0 ? TEXT("left") : TEXT("right"),
				Top.ImpactPoint.Z, Roof.ImpactPoint.Z, P.Z);
			return false;
		}
	}
	// the hand snaps to the lip: just outside the wall face (or where the top was found), a little under the top edge,
	// never more than 30 cm above where the hand was
	{
		const FVector Face = bWall ? FVector(Wall.ImpactPoint) : FVector(Top.ImpactPoint) - In * 4.0;
		const FVector Out = Face - In * 3.0;
		OutGrabPoint = FVector(Out.X, Out.Y, FMath::Min(double(Top.ImpactPoint.Z) - 4.0, P.Z + 30.0));
	}
	OutIn = In;
	OutTop = Top.ImpactPoint;
	return true;
}

bool AOutlawsPlayer::TryGrab(int32 Hand)
{
	FVector Gp, In, TopAt;
	if (!GetWorld() || !FindLedge(Hand, Gp, In, TopAt, true))
	{
		return false;
	}
	const FVector P = GripOf(Hand)->GetComponentLocation();
	GrabHand = Hand;
	GrabPoint = Gp;
	LedgeTopZ = float(TopAt.Z);
	LedgeIn = In;
	if (bIsCrouched && !bCrouchHeld) UnCrouch();
	bSwimming = false;
	UCharacterMovementComponent* M = GetCharacterMovement();
	M->SetMovementMode(MOVE_Flying);
	M->Velocity = FVector::ZeroVector;
	Pulse(Hand, 0.6f);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT VR grab (%s hand): ledge top z %.0f, hand z %.0f"), Hand == 0 ? TEXT("left") : TEXT("right"), LedgeTopZ, P.Z);
	return true;
}

void AOutlawsPlayer::ReleaseGrab(bool bFling)
{
	const int32 H = GrabHand;
	// (REQ-CLIMBWHEEL) the ledge let go of with the wheel up: the wheel goes away without taking anything, and the fall or
	// the climb onto the top goes on as before
	if (H >= 0 && bWheelOpen)
	{
		CloseWheel();
		UE_LOG(LogOutlaws, Log, TEXT("OLRT wheel closed: the %s hand let go of the ledge"), H == 0 ? TEXT("left") : TEXT("right"));
	}
	// let go with the hand at chest height or lower (28 cm under the eyes; user 2026-09-28: 45 felt like the waist):
	// the body is up at the lip, so climb onto it (or over it, for a fence or rail)
	const bool bLow = H >= 0 && GripOf(H)->GetComponentLocation().Z <= Camera->GetComponentLocation().Z - 28.0;
	if (bLow && StartMantle())
	{
		return;
	}
	// (BUG-SLOPECLIMB) a hand's let-go that does not climb, and why, one line each (a Log line: Shipping's log has it too)
	if (H >= 0 && bFling)
	{
		const AOutlawsLevelActor* LL = Level.Get();
		UE_LOG(LogOutlaws, Log, TEXT("OLRT let go of the ledge z %.0f (the body in sector %d): no climb, falling: %s"), LedgeTopZ, LL ? LL->PlayerSectorIndex() : -1,
			bLow ? *ClimbRefusal : TEXT("the hand over the chest"));
	}
	GrabHand = -1;
	UCharacterMovementComponent* M = GetCharacterMovement();
	M->SetMovementMode(MOVE_Falling);
	if (bFling && H >= 0)
	{
		// pushing off: the body moves opposite to the hand's last motion (swing up by pulling down fast)
		M->Velocity = (-HandVelocity[H]).GetClampedToMaxSize(450.f);
	}
}

void AOutlawsPlayer::TeleportSucceeded(bool bIsATest)
{
	// (BUG-WHARFPLANK) both before the movement looks for a floor at the new place (OnTeleported, in Super): the floors the
	// body can touch from there, and a body that fits under the roof there
	if (!bIsATest)
	{
		if (AOutlawsLevelActor* L = Level.Get()) L->NotePlayerTeleported();
		if (IsLocallyControlled()) FitUnderRoofAfterTeleport();
	}
	Super::TeleportSucceeded(bIsATest);
}

void AOutlawsPlayer::FitUnderRoofAfterTeleport()
{
	// (BUG-WHARFPLANK) a teleport (a load, a respawn, the unstuck's rescue) to where a standing body does not fit under the
	// roof (the wharf's plank 660 under its sloped roof, 4 units of room at its low end): the head in the roof, the movement
	// found no floor, and pushed out of the roof the body went down through the plank onto the pilings.  Where a crouched
	// body fits, the crouch goes on by itself, as a climb into a low space puts it on, and comes off with room
	// (-OLOldTeleportFit: as before)
	static const bool bOldFit = OLFlags::Param(TEXT("OLOldTeleportFit"));
	UWorld* World = GetWorld();
	if (bOldFit || !World || IsClimbing() || bSwimming) return;
	UCapsuleComponent* Cap = GetCapsuleComponent();
	const float R = Cap->GetScaledCapsuleRadius();
	const float CurHalf = Cap->GetUnscaledCapsuleHalfHeight();
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsTeleportFit), false, this);
	FCollisionResponseParams RP;
	Cap->InitSweepCollisionParams(Q, RP);
	if (const AOutlawsLevelActor* LQ = Level.Get()) LQ->IgnorePassedOverlaps(Q);
	const ECollisionChannel Channel = Cap->GetCollisionObjectType();
	const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, CurHalf + 2.0);      // (the teleports put the body 2 cm up)
	auto Fits = [&](float Half)
	{
		return !World->OverlapBlockingTestByChannel(Feet + FVector(0.0, 0.0, Half + 2.0), FQuat::Identity, Channel, FCollisionShape::MakeCapsule(R, Half), Q, RP);
	};
	if (CurHalf <= CrouchHalfHeight + 0.5f || Fits(CurHalf)) return;
	// the tallest body that fits, down to the crouch's; none, and something other than a roof is in the way: left as it was
	float Half = -1.f;
	for (float Try = CurHalf - 6.f; Try > CrouchHalfHeight; Try -= 6.f)
	{
		if (Fits(Try)) { Half = Try; break; }
	}
	if (Half < 0.f && Fits(CrouchHalfHeight)) Half = CrouchHalfHeight;
	if (Half < 0.f) return;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT teleport: no room for the body (half %.0f) under the roof there, crouched to fit (half %.0f)"), CurHalf, Half);
	if (bVR)
	{
		Cap->SetCapsuleHalfHeight(Half, false);
		SetActorLocation(Feet + FVector(0.0, 0.0, Half + 2.0), false, nullptr, ETeleportType::TeleportPhysics);
		const float StandHalf = FMath::Clamp((HeadHeight() + 12.f) * 0.5f, R + 2.f, StandHalfHeight);
		if (!bCrouchHeld && Half < StandHalf - 1.f)
		{
			bCrouchHeld = true;
			bAutoCrouched = true;
			bStandPending = false;
			CrouchAmount = 1.f;
			CrouchFull = CrouchDrop = FMath::Max(0.f, HeadHeight() - CrouchEye);
			ShowMessage(TEXT("Crouch"), 1.5f);
		}
		return;
	}
	// (flat: the movement's own crouch, which stands up again by itself once there is room, UpdateCrouch)
	UCharacterMovementComponent* M = GetCharacterMovement();
	const bool bWasNone = M->MovementMode == MOVE_None;
	if (bWasNone) M->SetMovementMode(MOVE_Falling);         // (it crouches only walking or falling)
	M->bWantsToCrouch = true;
	M->Crouch(false);
	if (bWasNone) M->DisableMovement();
	SetActorLocation(Feet + FVector(0.0, 0.0, Cap->GetUnscaledCapsuleHalfHeight() + 2.0), false, nullptr, ETeleportType::TeleportPhysics);
}

bool AOutlawsPlayer::StartMantle()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return false;
	}
	UCapsuleComponent* Cap = GetCapsuleComponent();
	const float R = Cap->GetScaledCapsuleRadius();
	const float CurHalf = Cap->GetUnscaledCapsuleHalfHeight();
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsMantle), false, this);
	FCollisionResponseParams RP;
	Cap->InitSweepCollisionParams(Q, RP);
	if (const AOutlawsLevelActor* LQ = Level.Get()) LQ->IgnorePassedOverlaps(Q);
	const ECollisionChannel Channel = Cap->GetCollisionObjectType();
	auto Fits = [&](const FVector& At, float Half)
	{
		return !World->OverlapBlockingTestByChannel(At + FVector(0.0, 0.0, Half + 2.0), FQuat::Identity, Channel, FCollisionShape::MakeCapsule(R, Half), Q, RP);
	};
	// VR keeps its head-sized body; try that, then a crouch, just past the lip, then a little
	// further (a thick rail)
	const float Want = bVR ? CurHalf : StandHalfHeight;
	const FVector Lip = FVector(GrabPoint.X, GrabPoint.Y, LedgeTopZ);
	FVector Base = FVector::ZeroVector;
	float Half = -1.f;
	auto HasFloor = [&](const FVector& At)
	{
		FHitResult F;
		return World->LineTraceSingleByChannel(F, At + FVector(0.0, 0.0, 5.0), At - FVector(0.0, 0.0, 40.0), ECC_Visibility, Q, PlayerNoPawns());
	};
	// the ground at a point past the lip: a top that rises away from the lip (a sloped roof, the town's; user 2026-10-03,
	// BUG-SLOPES: "if I try to climb on them I kind of just drop down") stands the body on itself there, up to 1.2 m over
	// the lip, rather than at the lip's height inside the slope
	// (user 2026-10-04, BUG-CLIMBCEIL, the fort: a box whose top is near the ceiling, a room over it: the search from 2 m up
	// started in the room above and stood the body on its floor) never over the ceiling of the ledge's own sector: the search
	// starts under it, and nothing there or above it is a landing
	const AOutlawsLevelActor* LC = Level.Get();
	int32 LedgeSector = -1;
	double LedgeCeil = LC ? LC->CeilingZAt(FVector(GrabPoint.X, GrabPoint.Y, LedgeTopZ) + LedgeIn * (R + 12.0), &LedgeSector) : 1e9;
	// (user 2026-10-27, BUG-SLOPECLIMB, the town's roofs over a room: "it would throw me up a little and then drop me") a roof
	// rising past the lip is over the point looked at, so the sector found there was the room under the roof, whose ceiling is
	// under the lip (or, where the roof's eave dips into the room's top, a hand over it): no spot was under it, the climb was
	// refused and the hand let go.  A ceiling that leaves no crouch's room over the ledge's top is checked against the sector
	// the top is in, looked for at the lip itself just in from the wall, and that one's is kept when it is higher (a box under
	// a low ceiling has the same one there: still no climb) (-OLOldSlopeClimb: as before)
	static const bool bOldSlopeClimb = OLFlags::Param(TEXT("OLOldSlopeClimb"));
	if (LC && !bOldSlopeClimb && LedgeCeil < LedgeTopZ + 2.0 * CrouchHalfHeight)
	{
		int32 LipSector = -1;
		const double LipCeil = LC->CeilingZAt(FVector(GrabPoint.X, GrabPoint.Y, LedgeTopZ) + LedgeIn * 7.0, &LipSector);
		if (LipCeil > LedgeCeil)
		{
			LedgeCeil = LipCeil;
			LedgeSector = LipSector;
		}
	}
	ClimbRefusal.Reset();
	// (BUG-SLOPECLIMB) a refusal, and why, for the let-go's line (ReleaseGrab); while hanging, once a second here
	auto Refuse = [&](const FString& Why)
	{
		ClimbRefusal = FString::Printf(TEXT("%s (the ledge's sector %d)"), *Why, LedgeSector);
		static double LastRefuseLog = -10.0;      // (asked every tick while hanging: once a second)
		if (FPlatformTime::Seconds() - LastRefuseLog > 1.0)
		{
			LastRefuseLog = FPlatformTime::Seconds();
			UE_LOG(LogOutlaws, Log, TEXT("OLRT climb over the ledge z %.0f refused: %s"), LedgeTopZ, *ClimbRefusal);
		}
		return false;
	};
	auto GroundAt = [&](const FVector& At)
	{
		FHitResult G;
		const double From = FMath::Min(At.Z + 200.0, FMath::Max(At.Z + 5.0, LedgeCeil - 2.0));
		if (World->LineTraceSingleByChannel(G, FVector(At.X, At.Y, From), At - FVector(0.0, 0.0, 5.0), ECC_Visibility, Q, PlayerNoPawns())
			&& !G.bStartPenetrating && G.ImpactNormal.Z >= 0.25)
		{
			return FVector(At.X, At.Y, FMath::Max(At.Z, double(G.ImpactPoint.Z)));
		}
		return At;
	};
	// (BUG-MILLCLIMB, the mill's hall inside its yard: a climb onto a box threw the player out into the yard) never a spot
	// in a sector the player's own does not lead to through openings near the climb
	FString LandWhy;
	int32 LandRefused = 0, CeilRefused = 0;
	auto LandOk = [&](const FVector& At)
	{
		if (!LC || LC->ClimbLandingSector(At, GetActorLocation(), &LandWhy) != -1) return true;
		++LandRefused;
		return false;
	};
	// somewhere to stand past the lip; else on the lip itself (a fence or thin wall: stand on it, the
	// body's round bottom perches on the edge); else anywhere it fits (over a rail and down)
	for (const bool bNeedFloor : { true, false })
	{
		for (const double Past : { R + 12.0, R + 42.0, 6.0 })
		{
			if (!bNeedFloor && Past < R) continue;
			const FVector At0 = bNeedFloor ? GroundAt(Lip + LedgeIn * Past) : Lip + LedgeIn * Past;
			for (const float Try : { Want, FMath::Min(Want, CrouchHalfHeight) })
			{
				// (BUG-SLOPES, the town's roofs: the upright body's round bottom cut into a rising roof, so no spot fitted
				// and the climb was refused) a little higher, resting on the slope, up to 45 cm
				for (const double Lift : { 0.0, 15.0, 30.0, 45.0 })
				{
					const FVector At = At0 + FVector(0.0, 0.0, Lift);
					if (At.Z + 2.0 * Try > LedgeCeil + 1.0) { ++CeilRefused; continue; }          // (BUG-CLIMBCEIL: the body under the ledge's own ceiling)
					if (Half < 0.f && Fits(At, Try) && (!bNeedFloor || HasFloor(At0)) && LandOk(At))
					{
						Half = Try;
						Base = At;
					}
				}
			}
		}
	}
	if (Half < 0.f)
	{
		// no room on or past the ledge: keep hanging.  (BUG-SLOPECLIMB: the ceiling's refusal and the no-room one left no line
		// in the log before)
		return Refuse(LandRefused > 0 ? LandWhy : CeilRefused > 0 ? FString::Printf(TEXT("no spot for the body under the ledge's ceiling z %.0f"), LedgeCeil)
			: FString(TEXT("no room for the body on or past the ledge")));
	}
	// (user 2026-10-04, BUG-CLIMBBARS: barred windows were climbed through) the way over the lip has to be open as a walk
	// would need it: the body (crouched, a little slimmer) swept across from over the hanging spot to the landing; a mid
	// texture that stops the body (bars), a wall, anything solid in between: no climb
	{
		const float SweepHalf = FMath::Min(Half, CrouchHalfHeight) * 0.9f;
		const FVector Land = Base + FVector(0.0, 0.0, SweepHalf + 4.0);
		// (over a rail the landing is lower than the lip: across at the lip's height, then down)
		const double Top = FMath::Max(Land.Z, double(LedgeTopZ) + SweepHalf + 4.0);
		const FVector Over = FVector(GetActorLocation().X, GetActorLocation().Y, Top), Past = FVector(Land.X, Land.Y, Top);
		const FCollisionShape Body = FCollisionShape::MakeCapsule(R * 0.8f, SweepHalf);
		FHitResult Block;
		auto Shut = [&](const FVector& A, const FVector& B)
		{
			return World->SweepSingleByChannel(Block, A, B, FQuat::Identity, Channel, Body, Q, RP) && !Block.bStartPenetrating;
		};
		if (Shut(Over, Past) || (Top > Land.Z + 1.0 && Shut(Past, Land)))
		{
			return Refuse(FString::Printf(TEXT("the way across is shut by %s at %s"), Block.GetComponent() ? *Block.GetComponent()->GetName() : TEXT("?"),
				*FVector(Block.ImpactPoint).ToCompactString()));
		}
	}
	if (Half < CurHalf)
	{
		if (bVR)
		{
			// shrink with the feet kept in place, so the view does not jump up
			Cap->SetCapsuleHalfHeight(Half, false);
			AddActorWorldOffset(FVector(0.0, 0.0, Half - CurHalf), false);
			// only a crouched body fits up there (the kitchen's high key nook, level two): the crouch goes on by itself
			// and the view comes down with the climb, one smooth move, instead of the head up through the ceiling (user
			// 2026-09-28); it stands up again by itself once there is room overhead
			const float StandHalf = FMath::Clamp((HeadHeight() + 12.f) * 0.5f, R + 2.f, StandHalfHeight);
			if (!bCrouchHeld && Half < StandHalf - 1.f)
			{
				bCrouchHeld = true;
				bAutoCrouched = true;
				bStandPending = false;
				CrouchAmount = 1.f;
				CrouchFull = CrouchDrop = FMath::Max(0.f, HeadHeight() - CrouchEye);
				MantleDrop0 = ShownDrop;
				ShowMessage(TEXT("Crouch"), 1.5f);
			}
		}
		else
		{
			GetCharacterMovement()->bWantsToCrouch = true;
			GetCharacterMovement()->Crouch(false);
		}
	}
	// nothing to stand on where the body lands: a fence or rail, so go over it and drop
	FHitResult Floor;
	bVaulting = !World->LineTraceSingleByChannel(Floor, Base + FVector(0.0, 0.0, 5.0), Base - FVector(0.0, 0.0, 40.0), ECC_Visibility, Q, PlayerNoPawns());
	GrabHand = -1;
	MantleFrom = GetActorLocation();
	MantleTo = Base + FVector(0.0, 0.0, Half + 2.0);
	MantleLeft = 0.25f;
	GetCharacterMovement()->SetMovementMode(MOVE_Flying);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT VR %s ledge z %.0f (body half %.0f)"), bVaulting ? TEXT("vault over") : TEXT("mantle onto"), LedgeTopZ, Half);
	return true;
}

// ---- hand grips: items and doors ------------------------------------------------------------------

void AOutlawsPlayer::Pulse(int32 Hand, float Amplitude)
{
	APlayerController* PC = Cast<APlayerController>(GetController());
	UWorld* World = GetWorld();
	if (!PC || !World)
	{
		return;
	}
	const EControllerHand CH = Hand == 0 ? EControllerHand::Left : EControllerHand::Right;
	PC->SetHapticsByValue(0.4f, Amplitude, CH);
	FTimerHandle T;
	TWeakObjectPtr<APlayerController> WeakPC(PC);
	World->GetTimerManager().SetTimer(T, FTimerDelegate::CreateLambda([WeakPC, CH]()
	{
		if (WeakPC.IsValid()) WeakPC->SetHapticsByValue(0.f, 0.f, CH);
	}), 0.08f, false);
}

FVector AOutlawsPlayer::ChestPoint() const
{
	// about 30 cm below the eyes, a little in front of them
	const FVector Forward = FRotator(0.0, Camera->GetComponentRotation().Yaw, 0.0).Vector();
	return Camera->GetComponentLocation() - FVector(0.0, 0.0, 30.0) + Forward * 8.0;
}

bool AOutlawsPlayer::TryPull(int32 Hand)
{
	AOutlawsLevelActor* L = Level.Get();
	AActor* Item = L ? L->PullPickupAlong(Camera->GetComponentLocation(), GripOf(Hand)->GetComponentLocation(), PullRange, PullAngle) : nullptr;
	if (!Item)
	{
		return false;
	}
	FPull P;
	P.Item = Item;
	P.Start = Item->GetActorLocation();
	P.Scale = Item->GetActorScale3D();
	Pulls.Add(P);
	Pulse(Hand, 0.4f);
	return true;
}

void AOutlawsPlayer::UpdatePulls(float Dt)
{
	OL_PERF_SCOPE("Player: pulls");
	// a short hop over to the waist (user 2026-10-02: "from the waist down", not the chest), shrinking to nothing and
	// spinning on the way, then taken.  The waist: 55 % of the eyes' height over the feet, a hand in front of the body
	AOutlawsLevelActor* L = Level.Get();
	// (user 2026-10-03: "twice as fast": 0.35 s before; the hand's pull keeps the same flight, so the two look alike)
	constexpr float Time = 0.175f;
	const FVector EyeAt = Camera->GetComponentLocation();
	const double FeetZ = GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const FVector Ahead = FRotator(0.0, Camera->GetComponentRotation().Yaw, 0.0).Vector();
	const FVector Waist = FVector(EyeAt.X, EyeAt.Y, FeetZ + 0.55 * FMath::Max(60.0, EyeAt.Z - FeetZ)) + Ahead * 12.0;
	for (int32 i = Pulls.Num() - 1; i >= 0; --i)
	{
		FPull& P = Pulls[i];
		AActor* Item = P.Item.Get();
		if (!Item || (!L && !P.bTaken))
		{
			Pulls.RemoveAt(i);
			continue;
		}
		P.T += Dt / Time;
		const float A = FMath::Clamp(P.T, 0.f, 1.f);
		const float Ease = A * A * (3.f - 2.f * A);
		const FVector To = Waist;
		const double Arc = FMath::Sin(A * PI) * FMath::Min(30.0, 0.2 * FVector::Dist(P.Start, To));
		Item->SetActorLocation(FMath::Lerp(P.Start, To, Ease) + FVector(0.0, 0.0, Arc));
		// to nothing by the end (a hair over zero: a zero scale upsets the bounds); quicker at the last
		Item->SetActorScale3D(P.Scale * FMath::Max(0.01f, 1.f - Ease * Ease));
		if (AOutlawsSpriteActor* S = Cast<AOutlawsSpriteActor>(Item))
		{
			S->OutlawsYaw += Dt * 720.f;
		}
		if (P.T >= 1.f && P.bTaken)
		{
			// already counted: its look reached the waist, gone
			UE_LOG(LogOutlaws, Log, TEXT("OLRT collect fly end: %s %.0f cm from the waist"), *Item->GetName(), FVector::Dist(Item->GetActorLocation(), Waist));
			Pulls.RemoveAt(i);
			Item->Destroy();
		}
		else if (P.T >= 1.f)
		{
			Pulls.RemoveAt(i);
			PullLanding = Item;           // (flown already: taken here without a second flight)
			const bool bGot = L->CollectPickupActor(Item, true);
			PullLanding = nullptr;
			if (!bGot)
			{
				// nothing of it fitted: back to full size at the player's feet
				Item->SetActorScale3D(P.Scale);
				L->DropPickupActor(Item, Camera->GetComponentLocation());
			}
		}
	}
}

bool AOutlawsPlayer::FlyCollected(AActor* Item)
{
	// every pickup collected flies to the waist as a hand's pull does, shrinking away and spinning, then goes (user 2026-10-02:
	// "whether they walk over it, reach for it or whatever").  It is counted already: only its look flies, nothing of it
	// touches anything on the way, and it goes within a second whatever happens to this player meanwhile
	if (!Item || PullLanding.Get() == Item || !Camera)
	{
		return false;
	}
	Item->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);     // (a fist's, or a lift's rider)
	Item->SetActorEnableCollision(false);
	Item->SetActorHiddenInGame(false);
	Item->SetLifeSpan(1.f);
	Pulls.RemoveAll([Item](const FPull& Pl) { return Pl.Item.Get() == Item; });
	FPull P;
	P.Item = Item;
	P.Start = Item->GetActorLocation();
	P.Scale = Item->GetActorScale3D();
	P.bTaken = true;
	Pulls.Add(P);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT collect fly start: %s %.0f cm from the chest"), *Item->GetName(), FVector::Dist(P.Start, ChestPoint()));
	return true;
}

bool AOutlawsPlayer::TryHold(int32 Hand)
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L)
	{
		return false;
	}
	UMotionControllerComponent* Grip = GripOf(Hand);
	// (REQ-COLLECTBOX) a hidden box closed on: found, it flies to the waist as a pickup does (nothing stays in the hand)
	if (L->TakeCollectBoxByHand(Camera->GetComponentLocation(), Grip->GetComponentLocation(), 16.f))
	{
		Pulse(Hand, 0.4f);
		return true;
	}
	AActor* Item = L->TakePickupNear(Camera->GetComponentLocation(), Grip->GetComponentLocation(), 16.f);
	if (!Item)
	{
		return false;
	}
	// sit it in the closed fist: the sprite's middle at the palm (sprites anchor at their bottom)
	const UOutlawsBoxHand* Box = Hand == 0 ? LeftHand.Get() : RightHand.Get();
	const FVector Palm = Box->GetComponentTransform().TransformPosition(FVector(2.0, Hand == 0 ? -0.5 : 0.5, 0.0));
	if (L->IsDynamitePickup(Item))
	{
		// a stick of dynamite off the ground: the hand's size (22 cm, as the drawn one), its middle in the fist
		const FBox B = Item->GetComponentsBoundingBox(true);
		const float Len = float(2.0 * B.GetExtent().GetMax());
		if (Len > 1.f) Item->SetActorScale3D(Item->GetActorScale3D() * FMath::Min(1.f, 22.f / Len));
		const FVector Mid = Item->GetComponentsBoundingBox(true).GetCenter();
		Item->SetActorLocation(Item->GetActorLocation() + (Palm - Mid));
		Item->AttachToComponent(Grip, FAttachmentTransformRules::KeepWorldTransform);
		StowForBottle(Hand);              // (a weapon or a tool in the hand steps aside, back when it is let go)
		HeldItem[Hand] = Item;
		Pulse(Hand, 0.4f);
		return true;
	}
	Item->SetActorLocation(Palm - FVector(0.0, 0.0, Item->GetActorScale3D().Z * 0.5));
	Item->AttachToComponent(Grip, FAttachmentTransformRules::KeepWorldTransform);
	StowForBottle(Hand);                  // (a weapon or a tool in the hand steps aside, back when it is let go)
	HeldItem[Hand] = Item;
	Pulse(Hand, 0.4f);
	return true;
}

UMaterialInterface* AOutlawsPlayer::LoadOverlayMaterial(const TCHAR* Name) const
{
	// on the headset the "_Q" copy: depth tested, pulled to arm's length (the mobile renderer has no custom depth for
	// the PC overlay's cut round the hands; user 2026-10-01: hands, laser and guns showed behind the menu there)
	const UWorld* World = GetWorld();
	if (World && World->FeatureLevel <= ERHIFeatureLevel::ES3_1)
	{
		if (UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, *FString::Printf(TEXT("/Game/Outlaws/Runtime/%s_Q.%s_Q"), Name, Name))) return M;
	}
	return LoadObject<UMaterialInterface>(nullptr, *FString::Printf(TEXT("/Game/Outlaws/Runtime/%s.%s"), Name, Name));
}

void AOutlawsPlayer::ReleaseHeld(int32 Hand)
{
	AActor* Item = HeldItem[Hand].Get();
	HeldItem[Hand] = nullptr;
	RestoreStowed(Hand);                  // (the weapon or tool it set aside, back in the hand)
	AOutlawsLevelActor* L = Level.Get();
	if (!Item || !L)
	{
		return;
	}
	const float D = FVector::Dist(GripOf(Hand)->GetComponentLocation(), ChestPoint());
	if (L->IsDynamitePickup(Item)) Item->SetActorScale3D(FVector::OneVector);     // (the item's own size again)
	if (D < ChestReach && L->CollectPickupActor(Item))    // tucked away: message, sound, inventory
	{
		Pulse(Hand, 0.8f);
	}
	else
	{
		// let go elsewhere, or nothing of it fitted (full ammo, a worse plate): back on the ground
		L->DropPickupActor(Item, Camera->GetComponentLocation());
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT VR let go %.0f cm from the chest: %s"), D, D < ChestReach ? TEXT("kept") : TEXT("dropped"));
}

double AOutlawsPlayer::SwapReach() const
{
	double Size = 0.0;
	for (int32 H = 0; H < 2; ++H)
	{
		const UPrimitiveComponent* Held = Equipped[H] >= 0 ? static_cast<const UPrimitiveComponent*>(WeaponMesh[H].Get())
			: HeldBottle[H] ? static_cast<const UPrimitiveComponent*>(HeldBottle[H].Get())
			: (IsGear(HeldTool[H]) && GearMesh[H]) ? static_cast<const UPrimitiveComponent*>(GearMesh[H].Get()) : nullptr;
		if (Held && Held->IsVisible()) Size = FMath::Max(Size, 2.0 * Held->CalcBounds(FTransform::Identity).BoxExtent.GetMax());     // (its own size: the held gear's bounds are scaled up)
	}
	return FMath::Min(30.0, 15.0 + 0.2 * Size);
}

void AOutlawsPlayer::PushDoors(int32 Hand)
{
	// a hand moving into a closed door pushes it open, no button needed; the hand has to come
	// off the door before it can push again, and a door on the move or standing open is left
	// alone (a nudge there would be the one that shuts it)
	AOutlawsLevelActor* L = Level.Get();
	if (!L || bWheelHeld[Hand] || GrabHand == Hand)
	{
		return;
	}
	const FVector Eye = Camera->GetComponentLocation();
	const FVector HandP = GripOf(Hand)->GetComponentLocation();
	bool bClosed = false;
	const int32 Door = L->DoorAtHand(Eye, HandP, bClosed);
	if (Door < 0)
	{
		PushDoor[Hand] = -1;
		return;
	}
	if (PushDoor[Hand] == Door || !bClosed)
	{
		return;
	}
	// one nudge per door per moment, whichever hand or button sent it (a locked door rattles
	// once, two hands on one leaf count once)
	const double Now = GetWorld()->GetTimeSeconds();
	if (Door == NudgedDoor && Now - NudgedTime < 0.5)
	{
		return;
	}
	// into the door means away from the eyes
	const FVector Dir = (HandP - Eye).GetSafeNormal();
	if (FVector::DotProduct(HandWorldVel[Hand], Dir) < DoorPushSpeed)
	{
		return;
	}
	PushDoor[Hand] = Door;
	NudgedDoor = Door;
	NudgedTime = Now;
	if (L->PushDoorSector(Door))
	{
		Pulse(Hand, 0.4f);
	}
}

void AOutlawsPlayer::NoteDoorUse(int32 Hand)
{
	// a grip press with a door under the hand is that door's nudge for the moment: the push
	// check that follows in the same frame must not send a second one
	AOutlawsLevelActor* L = Level.Get();
	if (!L)
	{
		return;
	}
	bool bClosed = false;
	const int32 Door = L->DoorAtHand(Camera->GetComponentLocation(), GripOf(Hand)->GetComponentLocation(), bClosed);
	if (Door >= 0)
	{
		PushDoor[Hand] = Door;
		NudgedDoor = Door;
		NudgedTime = GetWorld()->GetTimeSeconds();
	}
}

bool AOutlawsPlayer::TryHandUse(int32 Hand)
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L || !L->UseByHand(Camera->GetComponentLocation(), GripOf(Hand)->GetComponentLocation()))
	{
		return false;
	}
	Pulse(Hand, 0.5f);
	return true;
}

// ---- VR body height ------------------------------------------------------------------------------

void AOutlawsPlayer::UpdateBodyHeight()
{
	// the body is as tall as the head is high (plus the skull above the eyes): duck low enough and
	// any gap the headset fits under can be walked through.  The stick crouch lowers the view too.
	const float MinHalf = GetCapsuleComponent()->GetUnscaledCapsuleRadius() + 2.f;
	float Want = FMath::Clamp((HeadHeight() + 12.f) * 0.5f, MinHalf, StandHalfHeight);
	// crouched by a climb into a low space: up again by itself once the whole head-sized body has room, on a floor
	if (!bCrouchHeld)
	{
		bAutoCrouched = false;             // (the stick stood it up)
	}
	else if (bAutoCrouched && MantleLeft <= 0.f && GetCharacterMovement()->IsMovingOnGround())
	{
		UCapsuleComponent* Cap = GetCapsuleComponent();
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsAutoStand), false, this);
		FCollisionResponseParams RP;
		Cap->InitSweepCollisionParams(Q, RP);
		const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, Cap->GetUnscaledCapsuleHalfHeight());
		if (GetWorld() && !GetWorld()->OverlapBlockingTestByChannel(Feet + FVector(0.0, 0.0, Want + 2.0), FQuat::Identity, Cap->GetCollisionObjectType(),
			FCollisionShape::MakeCapsule(Cap->GetUnscaledCapsuleRadius(), Want), Q, RP))
		{
			bCrouchHeld = false;
			bAutoCrouched = false;
			bStandPending = true;
			ShowMessage(TEXT("Stand"), 1.5f);
		}
	}
	if (bCrouchHeld)
	{
		// (a part crouch is a part shorter body: the crouch's own height only at the full depth)
		Want = FMath::Min(FMath::Clamp((HeadHeight() - CrouchDrop + 12.f) * 0.5f, MinHalf, StandHalfHeight), FMath::Lerp(StandHalfHeight, CrouchHalfHeight, CrouchAmount));
	}
	ResizeBody(Want);
}

void AOutlawsPlayer::ResizeBody(float NewHalf)
{
	UCapsuleComponent* Cap = GetCapsuleComponent();
	UWorld* World = GetWorld();
	const float Old = Cap->GetUnscaledCapsuleHalfHeight();
	if (!World || FMath::Abs(NewHalf - Old) < 0.5f)
	{
		return;
	}
	float Half = NewHalf;
	if (NewHalf > Old)
	{
		// grow only into free space: sweep the top of the capsule up and stop under any ceiling
		const float R = Cap->GetUnscaledCapsuleRadius();
		const FVector TopSphere = GetActorLocation() + FVector(0.0, 0.0, Old - R);
		const float Want = 2.f * (NewHalf - Old);
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsBodyGrow), false, this);
		FCollisionResponseParams RP;
		Cap->InitSweepCollisionParams(Q, RP);
		FHitResult Hit;
		// sweep 2 cm further than needed and keep a real 1 cm gap under any ceiling
		if (World->SweepSingleByChannel(Hit, TopSphere, TopSphere + FVector(0.0, 0.0, Want + 2.0), FQuat::Identity, Cap->GetCollisionObjectType(),
			FCollisionShape::MakeSphere(R - 1.f), Q, RP))
		{
			Half = Old + FMath::Max(0.f, float(Hit.Distance) - 2.f) * 0.5f;
		}
		if (Half < Old + 0.25f)
		{
			return;
		}
	}
	// the feet stay where they are: the centre moves by the change in half height
	Cap->SetCapsuleHalfHeight(Half, false);
	AddActorWorldOffset(FVector(0.0, 0.0, Half - Old), false);
}

void AOutlawsPlayer::AlignHandFrames()
{
	// The fingers must point where the controller points.  The aim pose's forward is that
	// direction by definition; express it in grip space once per hand (the two poses are rigid)
	// and turn the hand frame so its X runs along it, keeping the grip's Y as the palm axis.
	if (!bVR)
	{
		return;
	}
	for (int32 H = 0; H < 2; ++H)
	{
		if (bFrameAligned[H] || !GripOf(H)->IsTracked() || !AimOf(H)->IsTracked())
		{
			continue;
		}
		const FTransform G = GripOf(H)->GetComponentTransform();
		const FVector F = G.InverseTransformVectorNoScale(AimOf(H)->GetForwardVector()).GetSafeNormal();
		if (F.IsNearlyZero())
		{
			continue;
		}
		// the two poses are rigid, so a good reading repeats: take it once it has held within a degree for 10 frames
		// (a controller just waking can give one stale pose, and a gun set off that sat crooked all session)
		if (FrameSteady[H] > 0 && (F | FrameSample[H]) > 0.99985)
		{
			++FrameSteady[H];
		}
		else
		{
			FrameSample[H] = F;
			FrameSteady[H] = 1;
		}
		if (FrameSteady[H] < 10)
		{
			continue;
		}
		const FRotator R = FRotationMatrix::MakeFromXY(F, FVector(0.0, 1.0, 0.0)).Rotator();
		FrameOf(H)->SetRelativeRotation(R);
		bFrameAligned[H] = true;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT VR %s hand: aim forward in grip space %s -> hand frame %s"), H == 0 ? TEXT("left") : TEXT("right"), *F.ToString(), *R.ToString());
	}
}

// ---- visuals -------------------------------------------------------------------------------------

void AOutlawsPlayer::UpdateHands(float Dt)
{
	OL_PERF_SCOPE("Player: hands");
	if (!bVR)
	{
		return;
	}
	// a hand holding a ledge stays fully closed
	for (int32 H = 0; H < 2; ++H)
	{
		if (AActor* Item = HeldItem[H].Get())
		{
			const UOutlawsBoxHand* Box = H == 0 ? LeftHand.Get() : RightHand.Get();
			const FVector Palm = Box->GetComponentTransform().TransformPosition(FVector(2.0, H == 0 ? -0.5 : 0.5, 0.0));
			Item->SetActorLocation(Palm - FVector(0.0, 0.0, Item->GetActorScale3D().Z * 0.5));
		}
	}
	// a gun hand stays closed round its grip, except while that gun is put away for fanning
	const bool bClosed[2] = { GrabHand == 0 || HeldItem[0].IsValid() || HeldBottle[0] != nullptr || bSupport[0] || bToolSupport[0] || !HeldTool[0].IsEmpty() || (Equipped[0] >= 0 && FanHide[0] <= 0.f), GrabHand == 1 || HeldItem[1].IsValid() || HeldBottle[1] != nullptr || bSupport[1] || bToolSupport[1] || !HeldTool[1].IsEmpty() || (Equipped[1] >= 0 && FanHide[1] <= 0.f) };
	// a hand no longer on a forend goes back to its controller
	for (int32 H = 0; H < 2; ++H)
	{
		UOutlawsBoxHand* Box = H == 0 ? LeftHand.Get() : RightHand.Get();
		if (Box && GFrameCounter - ForeSnapFrame[H] > 1 && !Box->GetRelativeTransform().Equals(FTransform::Identity, 1e-3))
		{
			Box->SetRelativeTransform(FTransform::Identity);
		}
	}
	// a gun hand: the index finger on the trigger, pulled with it; the other fingers round the grip
	const float Index[2] = { TriggerFingerCurl(0), TriggerFingerCurl(1) };
	// round a gun's grip the lower fingers stop at its surface (user 2026-09-28: the little finger went into the grip)
	LeftHand->SetGripCaps(Index[0] >= 0.f ? 0.8f : 1.f, Index[0] >= 0.f ? 0.72f : 1.f, Index[0] >= 0.f ? 0.62f : 1.f);
	RightHand->SetGripCaps(Index[1] >= 0.f ? 0.8f : 1.f, Index[1] >= 0.f ? 0.72f : 1.f, Index[1] >= 0.f ? 0.62f : 1.f);
	// on the title screens the dominant hand points: the index straight, the other fingers closed (the menu's pointer)
	const int32 Pointer = bMenuPistol ? MenuPointerHand() : -1;
	// the map pinched by its corner: the forefinger half bent to the thumb, the other fingers closed (user 2026-10-01)
	const bool bPinch[2] = { HeldTool[0] == TEXT("imap"), HeldTool[1] == TEXT("imap") };
	if (Pointer == 0) LeftHand->SetCurl(0.f, 1.f);
	else if (bPinch[0]) LeftHand->SetCurl(0.5f, 1.f);
	else LeftHand->SetCurl(Index[0] >= 0.f ? Index[0] : (bClosed[0] ? 1.f : Axis(TEXT("OLVR_LeftTrigger"))), bClosed[0] ? 1.f : Axis(TEXT("OLVR_LeftGrip")));
	if (Pointer == 1) RightHand->SetCurl(0.f, 1.f);
	else if (bPinch[1]) RightHand->SetCurl(0.5f, 1.f);
	else RightHand->SetCurl(Index[1] >= 0.f ? Index[1] : (bClosed[1] ? 1.f : Axis(TEXT("OLVR_RightTrigger"))), bClosed[1] ? 1.f : Axis(TEXT("OLVR_RightGrip")));
}

void AOutlawsPlayer::UpdateVignette(float Dt, bool bActive)
{
	OL_PERF_SCOPE("Player: vignette");
	const float Target = bVR && bComfortVignette && bActive ? 1.f : 0.f;
	Vignette = FMath::FInterpTo(Vignette, Target, Dt, 6.f);   // applied in UpdateScreenFx
}

void AOutlawsPlayer::HideForMovie(bool bHide)
{
	bMovieHidden = bHide;
	if (bHide)
	{
		// the smoke and spent cases are done with, not paused
		for (int32 i = 0; i < Puffs.Num(); ++i) { Puffs[i].Age = Puffs[i].Life; if (PuffFx.IsValidIndex(i) && PuffFx[i]) PuffFx[i]->SetVisibility(false); }
		for (int32 i = 0; i < Cases.Num(); ++i) { Cases[i].Age = Cases[i].Life; if (CaseFx.IsValidIndex(i) && CaseFx[i]) CaseFx[i]->SetVisibility(false); }
		MovieHiddenParts.Reset();
		TInlineComponentArray<UPrimitiveComponent*> Parts(this);
		for (UPrimitiveComponent* C : Parts)
		{
			if (C && C->IsVisible())
			{
				MovieHiddenParts.Add(C);
				C->SetVisibility(false);
			}
		}
		if (AOutlawsSpriteActor* B = BadgeIcon.Get()) B->SetActorHiddenInGame(true);
		UE_LOG(LogOutlaws, Log, TEXT("OLRT movie: %d player parts put away"), MovieHiddenParts.Num());
	}
	else
	{
		for (const TWeakObjectPtr<UPrimitiveComponent>& C : MovieHiddenParts)
		{
			if (UPrimitiveComponent* P = C.Get()) P->SetVisibility(true);
		}
		MovieHiddenParts.Reset();
	}
}

void AOutlawsPlayer::ClearScreenFx()
{
	HurtFlash = 0.f;
	HurtDirAmount = 0.f;
	BackdropTone = BackdropBlur = BackdropDim = 0.f;
	PaletteAmount = 0.f;
	UnderwaterBlend = 0.f;
	FPostProcessSettings& PP = Camera->PostProcessSettings;
	// the same tone as play (no filmic curve): the opening movie came before any frame of play had set it and looked
	// washed out (user 2026-09-28)
	PP.bOverride_ToneCurveAmount = true;
	PP.ToneCurveAmount = 0.f;
	PP.bOverride_SceneColorTint = false;
	PP.SceneColorTint = FLinearColor::White;
	PP.bOverride_VignetteIntensity = true;
	PP.VignetteIntensity = 0.f;
	ApplyGamma(PP);
	if (PaletteMid) PaletteMid->SetScalarParameterValue(TEXT("Amount"), 0.f);
	if (DeathMid) DeathMid->SetScalarParameterValue(TEXT("Amount"), 0.f);
	if (HurtDirMid) HurtDirMid->SetScalarParameterValue(TEXT("Amount"), 0.f);
	if (BackdropMid)
	{
		BackdropMid->SetScalarParameterValue(TEXT("Blur"), 0.f);
		BackdropMid->SetScalarParameterValue(TEXT("Tone"), 0.f);
		BackdropMid->SetScalarParameterValue(TEXT("Dim"), 0.f);
	}
	for (FWeightedBlendable& B : PP.WeightedBlendables.Array)
	{
		if (B.Object == BackdropMid || B.Object == HurtDirMid) B.Weight = 0.f;
	}
	if (ToneMpc && GetWorld())
	{
		if (UMaterialParameterCollectionInstance* Mpc = GetWorld()->GetParameterCollectionInstance(ToneMpc)) Mpc->SetScalarParameterValue(TEXT("Tone"), 0.f);
	}
}

void AOutlawsPlayer::ApplyGamma(FPostProcessSettings& PP)
{
	// REQ-GAMMA: the Brightness slider in the colour grade the tonemapper already bakes into its LUT (pow(c, 1 / gamma) on
	// linear colour; no pass of its own). 1.0 leaves the override off: the picture exactly as before. (The headset has no
	// post process: its materials take QHurt.w, UpdateScreenFx)
	const float G = OutlawsDeviceProfile::Gamma();
	PP.bOverride_ColorGamma = G != 1.f;
	PP.ColorGamma = FVector4(1.f, 1.f, 1.f, G);
}

void AOutlawsPlayer::UpdateScreenFx(float Dt)
{
	OL_PERF_SCOPE("Player: screen fx");
	// one place for everything the camera's post process shows: the wheel's dimming, the red flash
	// of a hit, the dark edges of low health, and the sepia of death
	const AOutlawsLevelActor* L = Level.Get();
	HurtFlash = FMath::Max(0.f, HurtFlash - Dt * 1.6f);
	DeathBlend = bDeadView ? FMath::Min(1.f, DeathBlend + Dt / 1.2f) : 0.f;
	FLinearColor Tint = FLinearColor::White;
	if (bWheelOpen)
	{
		Tint *= FLinearColor(0.55f, 0.55f, 0.6f);
	}
	if (HurtFlash > 0.f)
	{
		// toward the blood's own red (blood.nwx/.spx averages 131,46,46: G and B 0.355 of R), a little darkened
		// (user 2026-09-29: the flash was a purer red than the blood)
		Tint *= FMath::Lerp(FLinearColor::White, FLinearColor(0.86f, 0.305f, 0.305f), HurtFlash);
	}
	// underwater: the view takes the colour of the water (its surface textures' average), as a filter of its
	// hue (brightest channel 1) most of the way, and a little darker (user 2026-09-27)
	{
		FLinearColor Water;
		const bool bUnder = L && L->UnderwaterTintAt(Camera->GetComponentLocation(), Water);
		if (AOutlawsLevelActor* LM = Level.Get()) LM->SetMuffled(bUnder && !bDeadView && !bMenuOpen);      // the ears under water too (user 2026-09-30); the menu clear
		if (bUnder)
		{
			const float Top = FMath::Max3(Water.R, Water.G, Water.B);
			const FLinearColor Hue = Top > 1e-3f ? Water / Top : FLinearColor::White;
			// the view's filter in the same colour the water gives the surfaces in it
			if (!L->WaterHueAt(Camera->GetComponentLocation(), UnderwaterHue)) UnderwaterHue = FLinearColor(Hue.R, Hue.G, Hue.B, 1.f);
			UnderwaterFilter = FMath::Lerp(FLinearColor::White, Hue, 0.7f) * 0.8f;
			UnderwaterFilter.A = 1.f;
		}
		UnderwaterBlend = FMath::FInterpConstantTo(UnderwaterBlend, bUnder ? 1.f : 0.f, float(FApp::GetDeltaTime()), 4.f);
		// a breath of bubbles every few seconds, in front of the face (user 2026-09-27)
		if (bUnder && !bDeadView && !bMenuOpen)
		{
			BubbleWait -= float(FApp::GetDeltaTime());
			if (BubbleWait <= 0.f)
			{
				BubbleWait = FMath::FRandRange(2.5f, 4.5f);
				const FVector Face = Camera->GetComponentLocation() + Camera->GetForwardVector() * 18.0 - Camera->GetUpVector() * 8.0;
				if (AOutlawsLevelActor* LB = Level.Get()) LB->BlowBubbles(Face);
			}
		}
		// the water's colour as a plain filter over the view (M_OLR_Underwater): the picture multiplied by the colour of
		// the water's own surface texture, no fog (user 2026-09-28: the murk with distance was not liked)
		if (!UnderwaterMid)
		{
			if (UMaterialInterface* Uw = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_Underwater.M_OLR_Underwater")))
			{
				UnderwaterMid = UMaterialInstanceDynamic::Create(Uw, this);
				Camera->PostProcessSettings.WeightedBlendables.Array.Add(FWeightedBlendable(0.f, UnderwaterMid));
			}
		}
		if (UnderwaterMid)
		{
			UnderwaterMid->SetVectorParameterValue(TEXT("Hue"), UnderwaterHue);
			// stronger the deeper the eye (user 2026-09-28): a quarter just under the surface, 85 percent by 3 m down
			float Surface = 0.f;
			const float Deep = L && L->WaterAt(Camera->GetComponentLocation(), Surface) ? Surface - float(Camera->GetComponentLocation().Z) : 0.f;
			UnderwaterAmount = UnderwaterBlend * FMath::Clamp(0.25f + Deep / 500.f, 0.25f, 0.85f);
			UnderwaterMid->SetScalarParameterValue(TEXT("Amount"), UnderwaterAmount);
			for (FWeightedBlendable& B : Camera->PostProcessSettings.WeightedBlendables.Array)
			{
				if (B.Object == UnderwaterMid) B.Weight = UnderwaterBlend > 0.f ? 1.f : 0.f;   // off costs nothing
			}
		}
		else if (UnderwaterBlend > 0.f)
		{
			Tint *= FMath::Lerp(FLinearColor::White, UnderwaterFilter, UnderwaterBlend);
		}
	}
	// as health runs low the whole view greys and dims (below 60 percent, most of the way at 10)
	const float HealthFrac = L ? FMath::Clamp(L->GetHealth() / 10.f, 0.f, 1.f) : 1.f;
	// dying carries low health's tone, darkening and edges straight into death's look (they used to drop to
	// full colour and fade back up): the tone never dips, the darkening and heavy edges ease off as death's
	// bright gold comes in (user 2026-09-27)
	const float AliveWeak = FMath::Clamp((0.6f - HealthFrac) / 0.5f, 0.f, 1.f);
	if (bDeadView && !bFxDead)
	{
		WeakAtDeath = LastWeak;
		LowAtDeath = LastLow;
	}
	bFxDead = bDeadView;
	if (!bDeadView) LastWeak = AliveWeak;
	const float Weak = bDeadView ? WeakAtDeath * (1.f - DeathBlend) : AliveWeak;
	Tint *= FLinearColor(1.f - 0.7f * Weak, 1.f - 0.7f * Weak, 1.f - 0.7f * Weak);   // low health darkens the view (up to 70%)
	FPostProcessSettings& PP = Camera->PostProcessSettings;
	// the palette of the sector the eyes are in (water and the like), as a colour grade
	if (!PaletteMid)
	{
		if (UMaterialInterface* Grade = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_PaletteGrade.M_OLR_PaletteGrade")))
		{
			PaletteMid = UMaterialInstanceDynamic::Create(Grade, this);
			PP.WeightedBlendables.Array.Add(FWeightedBlendable(1.f, PaletteMid));
		}
	}
	if (PaletteMid)
	{
		FLinearColor Rows[3];
		const bool bGrade = L && L->PaletteGradeAt(Camera->GetComponentLocation(), Rows);
		if (bGrade)
		{
			PaletteMid->SetVectorParameterValue(TEXT("RowR"), Rows[0]);
			PaletteMid->SetVectorParameterValue(TEXT("RowG"), Rows[1]);
			PaletteMid->SetVectorParameterValue(TEXT("RowB"), Rows[2]);
			for (int32 r = 0; r < 3; ++r) QuestPalRows[r] = Rows[r];
		}
		if (bGrade != bPaletteGraded)
		{
			bPaletteGraded = bGrade;
			UE_LOG(LogOutlaws, Log, TEXT("OLRT palette grade %s at %s"), bGrade ? TEXT("on") : TEXT("off"), *Camera->GetComponentLocation().ToString());
		}
		PaletteAmount = FMath::FInterpConstantTo(PaletteAmount, bGrade ? 1.f : 0.f, Dt, 2.5f);
		PaletteMid->SetScalarParameterValue(TEXT("Amount"), PaletteAmount);
	}
	// death: the remaster's gold-sepia duotone, every colour onto one ramp from dark olive to pale
	// cream with its brightness kept (fitted from the remaster's death screen, linear light:
	// R 1.12 Y, G 1.02 Y, B 0.46 Y; 0.2126 x 1.12 + 0.7152 x 1.02 + 0.0722 x 0.46 = 1.0)
	if (!DeathMid && PaletteMid)
	{
		DeathMid = UMaterialInstanceDynamic::Create(PaletteMid->Parent, this);
		const FVector Y(0.2126, 0.7152, 0.0722);
		const float Gain[3] = { 1.12f, 1.02f, 0.46f };
		const TCHAR* Rows[3] = { TEXT("RowR"), TEXT("RowG"), TEXT("RowB") };
		for (int32 c = 0; c < 3; ++c)
		{
			DeathMid->SetVectorParameterValue(Rows[c], FLinearColor(Gain[c] * Y.X, Gain[c] * Y.Y, Gain[c] * Y.Z, c == 0 ? 0.01f : 0.f));
		}
		PP.WeightedBlendables.Array.Add(FWeightedBlendable(1.f, DeathMid));
	}
	if (DeathMid)
	{
		// low health fades toward the same tone, all the way at death
		DeathMid->SetScalarParameterValue(TEXT("Amount"), FMath::Max(DeathBlend, bDeadView ? WeakAtDeath : Weak));
	}
	HurtDirAmount = FMath::Max(0.f, HurtDirAmount - Dt * 1.6f);
	UpdateHurtVeil();                       // (the headset: no post process, a plane before the eyes)
	if (!HurtDirMid)
	{
		if (UMaterialInterface* HurtBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_HurtDir.M_OLR_HurtDir")))
		{
			HurtDirMid = UMaterialInstanceDynamic::Create(HurtBase, this);
			PP.WeightedBlendables.Array.Add(FWeightedBlendable(0.f, HurtDirMid));
		}
	}
	if (HurtDirMid)
	{
		HurtDirMid->SetVectorParameterValue(TEXT("Dir"), FLinearColor(HurtDir.X, HurtDir.Y, 0.f, 0.f));
		HurtDirMid->SetScalarParameterValue(TEXT("Amount"), HurtDirAmount);
		for (FWeightedBlendable& B : PP.WeightedBlendables.Array)
		{
			if (B.Object == HurtDirMid) B.Weight = HurtDirAmount > 0.f ? 1.f : 0.f;
		}
	}
	// the ink line round held gear (custom stencil 1), the intro's cartoon outline (user 2026-09-28)
	if (!InkMid)
	{
		if (UMaterialInterface* Ink = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_InkOutline.M_OLR_InkOutline")))
		{
			InkMid = UMaterialInstanceDynamic::Create(Ink, this);
			PP.WeightedBlendables.Array.Add(FWeightedBlendable(1.f, InkMid));
		}
	}
	// off behind a menu or the wheel: the pass draws after the scene, so items lying beyond the panel had their black
	// outlines drawn over it (user 2026-10-02: shapes over the main menu)
	static const bool bInkOverMenu = OLFlags::Param(TEXT("OLInkOverMenu"));   // (tests: the old way)
	if (InkMid && !bInkOverMenu)
	{
		for (FWeightedBlendable& B : PP.WeightedBlendables.Array)
		{
			if (B.Object == InkMid) B.Weight = (bMenuOpen || bWheelOpen) ? 0.f : 1.f;
		}
	}
	// behind the pause menu the world blurs and takes the death tone; behind the wheel it blurs
	// (the menu and wheel draw after it, sharp). Real time: the world is frozen under the menu
	if (!BackdropMid)
	{
		if (UMaterialInterface* Back = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_Backdrop.M_OLR_Backdrop")))
		{
			BackdropMid = UMaterialInstanceDynamic::Create(Back, this);
			PP.WeightedBlendables.Array.Add(FWeightedBlendable(0.f, BackdropMid));
		}
	}
	if (BackdropMid)
	{
		const float RealDt = float(FApp::GetDeltaTime());
		// the menu: tone only, no blur (user 2026-09-27); the wheel: no blur either, the world half as bright round it
		// (user 2026-09-28)
		BackdropBlur = 0.f;
		BackdropDim = FMath::FInterpConstantTo(BackdropDim, bWheelOpen && !bMenuOpen ? 1.f : 0.f, RealDt, 5.f);
		BackdropTone = FMath::FInterpConstantTo(BackdropTone, bMenuOpen ? 1.f : 0.f, RealDt, 4.f);
		BackdropMid->SetScalarParameterValue(TEXT("Blur"), BackdropBlur);
		BackdropMid->SetScalarParameterValue(TEXT("Tone"), BackdropTone);
		BackdropMid->SetScalarParameterValue(TEXT("Dim"), BackdropDim);
		// translucent surfaces (glass, water, smoke) draw after this pass: they take the tone themselves
		if (!ToneMpc) ToneMpc = LoadObject<UMaterialParameterCollection>(nullptr, TEXT("/Game/Outlaws/Runtime/MPC_OLR_Lights.MPC_OLR_Lights"));
		if (UMaterialParameterCollectionInstance* Mpc = ToneMpc ? GetWorld()->GetParameterCollectionInstance(ToneMpc) : nullptr)
		{
			const bool bSet = Mpc->SetScalarParameterValue(TEXT("Tone"), BackdropTone);
			static bool bWarned = false;
			if (!bSet && !bWarned) { bWarned = true; UE_LOG(LogOutlaws, Warning, TEXT("OLRT MPC_OLR_Lights has no Tone scalar")); }
		}
		for (FWeightedBlendable& B : PP.WeightedBlendables.Array)
		{
			if (B.Object == BackdropMid) B.Weight = (BackdropBlur > 0.f || BackdropTone > 0.f || BackdropDim > 0.f) ? 1.f : 0.f;   // off costs nothing
		}
		// the player's own hands, guns and lasers go into custom depth: the backdrop leaves them sharp and the
		// menu and wheel overlays let them show in front (their materials' OwnCut)
		if (BackdropBlur > 0.f || bMenuOpen || bWheelOpen)
		{
			TInlineComponentArray<UPrimitiveComponent*> Own(this);
			for (UPrimitiveComponent* P : Own)
			{
				if (P && !P->bRenderCustomDepth) P->SetRenderCustomDepth(true);
			}
		}
	}
	// no filmic curve: the palette colours and the baked light show as the original drew them
	PP.bOverride_ToneCurveAmount = true;
	PP.ToneCurveAmount = 0.f;
	PP.bOverride_SceneColorTint = !Tint.Equals(FLinearColor::White, 1e-3f);
	PP.SceneColorTint = Tint;
	PP.bOverride_ColorSaturation = false;
	const float Sat = 1.f;
	PP.ColorSaturation = FVector4(Sat, Sat, Sat, 1.f);
	ApplyGamma(PP);
	// low health closes in from the edges below 60 percent, and throbs below 30 percent
	const float Frac = L ? FMath::Clamp(L->GetHealth() / 10.f, 0.f, 1.f) : 1.f;
	// dead: the edges stay closed in, all the way (no health left) as the death tone comes in (user 2026-09-27)
	float Low = bDeadView ? FMath::Lerp(LowAtDeath, 1.f, DeathBlend) : FMath::Clamp((0.6f - Frac) / 0.5f, 0.f, 1.f);
	if (!bDeadView) LastLow = Low;
	if (Frac < 0.3f && !bDeadView)
	{
		Low += 0.15f * FMath::Sin(GetWorld()->GetRealTimeSeconds() * 5.f);
	}
	PP.bOverride_VignetteIntensity = true;
	// death keeps its gold tone bright, as the original; the pause menu takes death's look (its tone is the
	// backdrop's, drawn under the menu so the menu keeps its colours), edges included
	PP.VignetteIntensity = FMath::Max(1.2f * Vignette, 1.6f * Low + 0.35f * FMath::Max(DeathBlend, BackdropTone));
	// the headset draws no post process: its materials do these same sums on their own colours (ue_olue_quest_look.py),
	// from the values the PC's blendables and scene settings take above
	if (!ToneMpc) ToneMpc = LoadObject<UMaterialParameterCollection>(nullptr, TEXT("/Game/Outlaws/Runtime/MPC_OLR_Lights.MPC_OLR_Lights"));
	if (UMaterialParameterCollectionInstance* Q = ToneMpc && GetWorld() ? GetWorld()->GetParameterCollectionInstance(ToneMpc) : nullptr)
	{
		float Death = DeathMid ? FMath::Max(DeathBlend, bDeadView ? WeakAtDeath : Weak) : 0.f;
		float Under = UnderwaterMid && UnderwaterBlend > 0.f ? UnderwaterAmount : 0.f;
		FLinearColor Hue = UnderwaterHue;
		FVector2D QDir = HurtDir;
		float QHurtAmount = HurtDirMid ? HurtDirAmount : 0.f, QTone = BackdropTone, QDim = BackdropDim;
#if WITH_OUTLAWS_TESTS
		// (QSPEED-5) -OLPoseFx=hurt|death|menu|dim|under|all: the headset's screen effects held at fixed amounts for the pose
		// shots (the materials' before and after compared on the same picture)
		static const FString PoseFx = [] { FString V; OLFlags::Value(TEXT("OLPoseFx"), V, false); return V.ToLower(); }();
		if (!PoseFx.IsEmpty())
		{
			const bool bAll = PoseFx == TEXT("all");
			if (bAll || PoseFx == TEXT("hurt")) { QDir = FVector2D(0.6f, 0.8f); QHurtAmount = 0.8f; }
			if (bAll || PoseFx == TEXT("death")) Death = 1.f;
			if (bAll || PoseFx == TEXT("menu")) { QTone = 1.f; Q->SetScalarParameterValue(TEXT("Tone"), 1.f); }   // (the glass, glow, smoke and water too)
			if (bAll || PoseFx == TEXT("dim")) QDim = 1.f;
			if (bAll || PoseFx == TEXT("under")) { Under = 0.6f; Hue = FLinearColor(0.55f, 0.8f, 0.9f, 1.f); }
		}
#endif
		Q->SetVectorParameterValue(TEXT("QPalR"), QuestPalRows[0]);
		Q->SetVectorParameterValue(TEXT("QPalG"), QuestPalRows[1]);
		Q->SetVectorParameterValue(TEXT("QPalB"), QuestPalRows[2]);
		Q->SetVectorParameterValue(TEXT("QTint"), FLinearColor(Tint.R, Tint.G, Tint.B, PaletteMid ? PaletteAmount : 0.f));
		Q->SetVectorParameterValue(TEXT("QUnder"), FLinearColor(Hue.R * 0.9f, Hue.G * 0.9f, Hue.B * 0.9f, Under));
		// (w: REQ-GAMMA's Brightness less one, 0 the look as made: the node skips its pow)
		Q->SetVectorParameterValue(TEXT("QHurt"), FLinearColor(QDir.X, QDir.Y, QHurtAmount, OutlawsDeviceProfile::Gamma() - 1.f));
		Q->SetVectorParameterValue(TEXT("QMisc"), FLinearColor(Death, QTone, QDim, PP.VignetteIntensity));
	}
}

void AOutlawsPlayer::UpdateFootsteps(float Dt)
{
	OL_PERF_SCOPE("Player: footsteps");
	// a step sound every stride on the ground, the floor's own (the sector's FLOOR SOUND); walking
	// in the room counts as much as the stick
	AOutlawsLevelActor* L = Level.Get();
	const UCharacterMovementComponent* M = GetCharacterMovement();
	const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	const double Moved = StepPrev.IsZero() ? 0.0 : FVector::Dist2D(Feet, StepPrev);
	StepPrev = Feet;
	if (!L || !M->IsMovingOnGround() || Moved > 200.0)
	{
		return;
	}
	// the cadence of the original, from the user's recording (2026-09-27): a walking step every 0.500 s, a
	// running step every 0.340 s. Those hold at the walk and run speeds the movement slider sets; between
	// and beyond them the cadence follows the feet's speed (a half-pushed stick, the ramp up to a run)
	const float Pace = L->Pace();
	const float WalkV = (bVR ? VRWalkSpeed : DesktopWalkSpeed) * Pace;
	const float RunV = (bVR ? VRRunSpeed : DesktopRunSpeed) * Pace;
	StepSpeed = FMath::FInterpTo(StepSpeed, float(Moved / FMath::Max(Dt, 1e-4f)), Dt, 8.f);
	const float Alpha = (StepSpeed - WalkV) / FMath::Max(1.f, RunV - WalkV);
	const float StepsPerSecond = FMath::Clamp(FMath::Lerp(1.f / 0.5f, 1.f / 0.34f, Alpha), 0.8f, 3.6f);
	const float StepPeriod = 1.f / StepsPerSecond;
	if (Moved < 30.0 * Dt)
	{
		StepTimer = StepPeriod * 0.5f;      // standing: the first step comes half a stride after setting off
		return;
	}
	StepTimer += Dt;
	if (StepTimer >= StepPeriod)
	{
		StepTimer -= StepPeriod;
		const FString Sound = L->FloorSoundAt(Feet);
		if (!Sound.IsEmpty())
		{
			// straight under the head, at the feet's height (user 2026-10-03, BUG-STEPPAN: "coming out of my right ear more
			// than my left": in the headset the body's middle can sit off to one side of the head)
			const FVector Head = Camera->GetComponentLocation();
			// a third as loud as the level's own (user 2026-10-03, BUG-STEPVOL: "about a third of what they are now")
			if (UAudioComponent* A = L->PlaySoundAt(Sound, FVector(Head.X, Head.Y, Feet.Z))) A->SetVolumeMultiplier(A->VolumeMultiplier / 3.f);
			// (where the body's feet were against the head, head's right and forward: the first eight steps a level, and
			// any with the body over 15 cm aside)
			static FString StepLogLevel;
			static int32 StepLogs = 0;
			if (StepLogLevel != L->LevelName) { StepLogLevel = L->LevelName; StepLogs = 0; }
			const FVector Off = Feet - FVector(Head.X, Head.Y, Feet.Z);
			const float Right = float(FVector::DotProduct(Off, Camera->GetRightVector().GetSafeNormal2D()));
			const float Fwd = float(FVector::DotProduct(Off, Camera->GetForwardVector().GetSafeNormal2D()));
			if (StepLogs < 8 || (FMath::Abs(Right) > 15.f && StepLogs < 40))
			{
				++StepLogs;
				UE_LOG(LogOutlaws, Log, TEXT("OLRT step: the body's feet %.0f cm right, %.0f cm ahead of the head (played under the head)"), Right, Fwd);
			}
		}
	}
}

void AOutlawsPlayer::ShowMessage(const FString& Text, float Seconds)
{
	if (Text.IsEmpty())
	{
		return;
	}
	// up to three lines; the oldest goes first
	if (HudLines.Num() >= 3)
	{
		HudLines.RemoveAt(0);
		HudLeft.RemoveAt(0);
	}
	HudLines.Add(Text);
	HudLeft.Add(Seconds);
	bHudDirty = true;
}

void AOutlawsPlayer::DropMessage(const FString& Text)
{
	for (int32 i = HudLines.Num() - 1; i >= 0; --i)
	{
		if (HudLines[i] != Text) continue;
		HudLines.RemoveAt(i);
		HudLeft.RemoveAt(i);
		bHudDirty = true;
	}
}

void AOutlawsPlayer::CoopHostPauseFreeze(bool bOn)
{
	UCharacterMovementComponent* M = GetCharacterMovement();
	if (bOn)
	{
		// (everything but this body at a ten-thousandth of real time, as the single-player menu does: the bad guys, the
		// other players' puppets, the sprites and the timers wait for the host)
		if (UGameplayStatics::GetGlobalTimeDilation(this) > 0.001f) UGameplayStatics::SetGlobalTimeDilation(this, 0.0001f);
		CustomTimeDilation = 10000.f;
		if (!bHostPauseFrozen)
		{
			HostPauseSavedMode = M->MovementMode;
			bHostPauseFrozen = true;
		}
		if (M->MovementMode != MOVE_None)
		{
			M->StopMovementImmediately();
			M->DisableMovement();
		}
		return;
	}
	if (!bHostPauseFrozen) return;
	bHostPauseFrozen = false;
	UGameplayStatics::SetGlobalTimeDilation(this, 1.f);       // (co-op only: the menu there stops nothing)
	CustomTimeDilation = 1.f;
	if (bMenuOpen) MenuSavedMode = HostPauseSavedMode == MOVE_None ? MOVE_Falling : HostPauseSavedMode.GetValue();
	else M->SetMovementMode(HostPauseSavedMode == MOVE_None ? MOVE_Falling : HostPauseSavedMode.GetValue());
}

void AOutlawsPlayer::UpdateHud(float Dt)
{
	OL_PERF_SCOPE("Player: hud");
	if (!HudText)
	{
		// fixed to the view, about 20 degrees above centre (halfway to the top of a Quest's view),
		// close enough not to sink into walls and drawn over the world
		HudText = NewObject<UTextRenderComponent>(this);
		HudText->SetHorizontalAlignment(EHTA_Center);
		HudText->SetVerticalAlignment(EVRTA_TextBottom);
		HudText->SetWorldSize(2.9f);     // a size up from 2.4 with thinner strokes (M_OLR_TextTop "Weight"): the notices read bold (user 2026-09-28)
		HudText->SetTextRenderColor(FColor(245, 230, 190));
		// the pickup and notice lines draw over the level (user 2026-09-28): the translucent, depth-test-off text.
		// REQ-HUDOUTLINE: M_OLR_TextTop's letters with a 2 px black outline round them, so the lines read against a bright
		// sky or clouds (user 2026-10-15; tools\olue\ue_olue_hud_outline.py). Only these notice lines take it
		UMaterialInterface* Top = LoadOverlayMaterial(TEXT("M_OLR_TextOutline"));
		if (!Top) Top = LoadOverlayMaterial(TEXT("M_OLR_TextTop"));
		if (!Top) Top = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_Text.M_OLR_Text"));
		if (Top) HudText->SetTextMaterial(Top);
		HudText->SetTranslucentSortPriority(8);
		HudText->SetRenderCustomDepth(true);          // stencil 3: no ink line over the notices
		HudText->SetCustomDepthStencilValue(3);
		HudText->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		HudText->SetCastShadow(false);
		HudText->SetDepthPriorityGroup(SDPG_Foreground);
		HudText->SetupAttachment(Camera);
		HudText->SetText(FText::GetEmpty());     // not the component's default "Text"
		HudText->RegisterComponent();
		HudDist = 80.f;
		HudText->SetRelativeLocationAndRotation(FVector(HudDist, 0.0, HudDist * FMath::Tan(FMath::DegreesToRadians(10.0))), FRotator(0.0, 180.0, 0.0));   // 10 degrees above the middle of the view
	}
	if (HudLines.Num() > 0 && Camera)
	{
		// BUG-MSGCLIP: on the headset M_OLR_TextTop_Q keeps the depth test (pulled to 50 cm), so a door or wall nearer
		// than that hid "you need a key" and the pickup lines. The lines come in front of the first surface along
		// their line of sight (5 cm short of it, 20 cm at the nearest), scaled so they read the same size.
		const double Full = 80.0;
		const double Tan10 = FMath::Tan(FMath::DegreesToRadians(10.0));
		const FVector Eye = Camera->GetComponentLocation();
		const FVector Way = Camera->GetComponentTransform().TransformVectorNoScale(FVector(1.0, 0.0, Tan10)).GetSafeNormal();
		const double Along = Full * FMath::Sqrt(1.0 + Tan10 * Tan10);
		double Want = Full;
		FHitResult Hit;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsHudClear), false, this);
		for (const auto& H : HeldItem) if (H.IsValid()) Q.AddIgnoredActor(H.Get());     // (not the gear in the hands)
		if (GetWorld() && GetWorld()->LineTraceSingleByChannel(Hit, Eye, Eye + Way * (Along + 10.0), ECC_Visibility, Q, PlayerNoPawns()))
		{
			Want = FMath::Clamp((Hit.Distance - 5.0) / FMath::Sqrt(1.0 + Tan10 * Tan10), 20.0, Full);
		}
		// nearer at once, back out gently (no pumping as the head moves along a wall)
		HudDist = Want < HudDist ? float(Want) : FMath::FInterpTo(HudDist, float(Want), Dt, 4.f);
		// (REQ-TUTORIAL2) the tutorial's card stands where the lines stand: they go over its top edge while it is up
		const double Lift = bTutCardUp && TutCardDist > 1.f ? FMath::Max(0.0, double(TutCardTopZ) / TutCardDist + 0.01 - Tan10) : 0.0;
		HudText->SetRelativeLocation(FVector(HudDist, 0.0, HudDist * (Tan10 + Lift)));
		HudText->SetRelativeScale3D(FVector(HudDist / Full));
		if (!bHudDistLogged && HudDist < Full - 1.f)
		{
			bHudDistLogged = true;
			UE_LOG(LogOutlaws, Log, TEXT("OLRT hud notice pulled to %.0f cm (wall %.0f cm along the line), material %s"), HudDist, Hit.Distance,
				HudText->GetMaterial(0) ? *HudText->GetMaterial(0)->GetName() : TEXT("none"));
		}
	}
	for (int32 i = HudLeft.Num() - 1; i >= 0; --i)
	{
		HudLeft[i] -= Dt;
		if (HudLeft[i] <= 0.f)
		{
			HudLines.RemoveAt(i);
			HudLeft.RemoveAt(i);
			bHudDirty = true;
		}
	}
	if (bHudDirty)
	{
		bHudDirty = false;
		HudText->SetText(FText::FromString(FString::Join(HudLines, TEXT("\n"))));
		HudText->SetVisibility(HudLines.Num() > 0);
	}
	UpdateKillFeed(Dt);                  // (REQ-KILLFEED: OutlawsPlayerKillFeed.cpp)
	UpdatePerfHud();
}

void AOutlawsPlayer::UpdatePerfHud()
{
	if (!OutlawsDeviceProfile::PerfHud())
	{
		if (PerfHudText && PerfHudText->IsVisible()) PerfHudText->SetVisibility(false);
		PerfHudTime = 0.f;
		PerfHudFrames = 0;
		return;
	}
	if (!PerfHudText)
	{
		// as the notices (drawn over the level), smaller, about 22 degrees above the middle of the view (over the notices
		// and the menu's title)
		PerfHudText = NewObject<UTextRenderComponent>(this);
		PerfHudText->SetHorizontalAlignment(EHTA_Center);
		PerfHudText->SetVerticalAlignment(EVRTA_TextBottom);
		PerfHudText->SetWorldSize(2.0f);
		PerfHudText->SetTextRenderColor(FColor(190, 245, 170));
		UMaterialInterface* Top = LoadOverlayMaterial(TEXT("M_OLR_TextTop"));
		if (!Top) Top = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_Text.M_OLR_Text"));
		if (Top) PerfHudText->SetTextMaterial(Top);
		PerfHudText->SetTranslucentSortPriority(8);
		PerfHudText->SetRenderCustomDepth(true);
		PerfHudText->SetCustomDepthStencilValue(3);
		PerfHudText->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		PerfHudText->SetCastShadow(false);
		PerfHudText->SetDepthPriorityGroup(SDPG_Foreground);
		PerfHudText->SetupAttachment(Camera);
		PerfHudText->SetText(FText::FromString(TEXT("...")));
		PerfHudText->RegisterComponent();
		const double Dist = 80.0;
		PerfHudText->SetRelativeLocationAndRotation(FVector(Dist, 0.0, Dist * FMath::Tan(FMath::DegreesToRadians(22.0))), FRotator(0.0, 180.0, 0.0));
	}
	if (!PerfHudText->IsVisible()) PerfHudText->SetVisibility(true);
	// real time (not the game's slowed clock), refreshed twice a second
	PerfHudTime += float(FApp::GetDeltaTime());
	++PerfHudFrames;
	if (PerfHudTime < 0.5f) return;
	const float Fps = PerfHudFrames / PerfHudTime;
	const FString Eye = OutlawsDeviceProfile::EyeBufferText();
	// (REQ-REFRESH) the display rate it runs at, when the headset reports one
	const float Hz = OutlawsXRRefresh::CurrentRate();
	const FString HzText = Hz > 0.f ? FString::Printf(TEXT("  %.0f Hz"), Hz) : FString();
	PerfHudText->SetText(FText::FromString(FString::Printf(TEXT("%.0f fps%s  %.1f ms\ngame %.1f  draw %.1f  gpu %.1f\n%d %%%s%s"),
		Fps, *HzText, 1000.f * PerfHudTime / PerfHudFrames, FPlatformTime::ToMilliseconds(GGameThreadTime), FPlatformTime::ToMilliseconds(GRenderThreadTime),
		OutlawsDynRes::GpuMs(), OutlawsDynRes::IsActive() ? FMath::RoundToInt(OutlawsDynRes::Scale() * 100.f) : OutlawsDeviceProfile::ResolutionPercent(),
		Eye.IsEmpty() ? TEXT("") : TEXT("  eye "), *Eye)));
	PerfHudTime = 0.f;
	PerfHudFrames = 0;
}

void AOutlawsPlayer::OnDied()
{
	if (bDeadView)
	{
		return;
	}
	bDeadView = true;
	DeathTime = 0.f;
	DeathDrop = 0.f;
	DeathEye = FMath::Max(40.f, float(Camera->GetComponentLocation().Z - (GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight())));
	CloseMenu();
	CloseWheel();
	if (GrabHand >= 0)
	{
		ReleaseGrab(false);
	}
	GetCharacterMovement()->StopMovementImmediately();
	// (BUG-DEATHAUTOSAVE: the save loaded when no autosave has come since, and never an autosave of another level)
	const AOutlawsLevelActor* DL = Level.Get();
	bDeathAutosave = DL && DL->DeathSave(DeathSaveSlot);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT player died: the view falls from %.0f cm"), DeathEye);
}

void AOutlawsPlayer::RequestRespawn(bool bFromAutosave)
{
	AOutlawsLevelActor* L = Level.Get();
	if (!bDeadView || DeathTime < 1.f || !L)
	{
		return;
	}
	if (UOutlawsCoop::IsActive(this))
	{
		if (UOutlawsCoop::MatchRespawn(this)) return;     // (M1: a match: a spawn point, the host's to give)
		UOutlawsCoop::RespawnAtStart(this);  // (co-op: back at the level's start with the start kit; the other player plays on)
		return;
	}
	if (bFromAutosave)
	{
		// B / Y: back to the autosave (user 2026-09-27), when there is one; the save loaded, when no autosave came after it
		if (!bDeathAutosave) return;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT respawn from %s"), DeathSaveSlot >= 0 ? *FString::Printf(TEXT("slot %d (loaded again)"), DeathSaveSlot) : TEXT("the autosave"));
		if (!L->LoadGame(DeathSaveSlot >= 0 ? DeathSaveSlot : -1)) L->RestartLevel();
		return;
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT respawn"));
	L->RestartLevel();
}

void AOutlawsPlayer::UpdateDeathView(float Dt)
{
	OL_PERF_SCOPE("Player: death view");
	// the head drops to the floor like a body falling, tips over, and stays there until a button
	DeathTime += Dt;
	const float Fall = FMath::Min(1.f, DeathTime / 0.7f);
	// down to about 70 cm off the floor, level (no tip over: user 2026-09-27)
	DeathDrop = FMath::Max(0.f, DeathEye - 70.f) * Fall * Fall;
	VROrigin->SetRelativeRotation(FRotator::ZeroRotator);
	AlignHandFrames();
	FollowHead();
	PlaceOrigin(Dt);
	// and the face never goes into the floor, however far the head leans down
	{
		const float FeetZ = float(GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
		const float Under = FeetZ + 30.f - float(Camera->GetComponentLocation().Z);
		if (Under > 0.f) VROrigin->AddWorldOffset(FVector(0.0, 0.0, Under));
	}
	UpdateHands(Dt);
	UpdateScreenFx(Dt);
	if (!RespawnText)
	{
		RespawnText = NewObject<UTextRenderComponent>(this);
		RespawnText->SetHorizontalAlignment(EHTA_Center);
		RespawnText->SetVerticalAlignment(EVRTA_TextCenter);
		RespawnText->SetWorldSize(4.f);
		RespawnText->SetTextRenderColor(FColor(240, 220, 180));
		UMaterialInterface* Top = LoadOverlayMaterial(TEXT("M_OLR_TextTop"));
		if (!Top) Top = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_Text.M_OLR_Text"));
		if (Top) RespawnText->SetTextMaterial(Top);
		RespawnText->SetTranslucentSortPriority(8);
		RespawnText->SetRenderCustomDepth(true);
		RespawnText->SetCustomDepthStencilValue(3);
		RespawnText->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		RespawnText->SetCastShadow(false);
		RespawnText->SetupAttachment(VROrigin);
		RespawnText->RegisterComponent();
	}
	const bool bShow = DeathTime > 1.5f;
	{
		// restart the level, or (with an autosave) go back to it
		const FString Want = UOutlawsCoop::IsMatchActive(this) ? UOutlawsCoop::MatchDeathPrompt(bVR)     // (M1: a match: a respawn)
			: UOutlawsCoop::IsActive(this) ? UOutlawsCoop::DeathPrompt(bVR)        // (co-op: a revive, or the level's start)
			: bVR ? (!bDeathAutosave ? TEXT("You are dead\nA / X: restart level") : DeathSaveSlot >= 0 ? TEXT("You are dead\nA / X: restart level\nB / Y: load the save again")
				: TEXT("You are dead\nA / X: restart level\nB / Y: restart from autosave"))
			: TEXT("You are dead\nSpace: restart level");
		if (RespawnText->Text.ToString() != Want) RespawnText->SetText(FText::FromString(Want));
	}
	RespawnText->SetVisibility(bShow);
	if (bShow)
	{
		const FVector Eye = Camera->GetComponentLocation();
		const FVector Ahead = FRotator(0.0, Camera->GetComponentRotation().Yaw, 0.0).Vector();
		const FVector At = Eye + Ahead * 140.0 + FVector(0.0, 0.0, 10.0);
		RespawnText->SetWorldLocationAndRotation(At, (Eye - At).Rotation());
	}
	if (UpdateGlyphPrompt(RespawnText, RespawnText->Text.ToString(), bShow)) RespawnText->SetVisibility(false);     // (REQ-BUTTONGLYPHS)
	// a trigger pulled once the prompt shows respawns too (not in co-op: a stray pull there would throw away a revive)
	for (int32 H = 0; H < 2 && bShow && bVR && !UOutlawsCoop::IsActive(this); ++H)
	{
		if (Axis(H == 0 ? TEXT("OLVR_LeftTrigger") : TEXT("OLVR_RightTrigger")) > 0.7f && DeathTime > 2.f)
		{
			RequestRespawn();
			return;
		}
	}
}

void AOutlawsPlayer::UpdateBadgeIcon()
{
	AOutlawsLevelActor* L = Level.Get();
	const bool bOn = L && L->HasBadgePower() && !bDeadView && !bMenuOpen;
	AOutlawsSpriteActor* S = BadgeIcon.Get();
	if (!bOn)
	{
		if (S) S->SetActorHiddenInGame(true);
		return;
	}
	// on the HUD, just above the pickup lines (they grow upward from 10 degrees over the middle of the view), the
	// spinning star of the pickup itself (gbadge.nwx; ibadge.nwx is the inventory icon, which reads as a key)
	// (user 2026-09-28)
	const double Dist = 80.0;
	const double TextZ = Dist * FMath::Tan(FMath::DegreesToRadians(10.0));
	const double Lines = HudText && HudText->IsVisible() ? double(HudLines.Num()) : 0.0;
	const FVector Above = Camera->GetComponentTransform().TransformPosition(FVector(Dist, 0.0, TextZ + Lines * 3.26 + 3.2));
	if (!S)
	{
		S = L->MakeLooseSprite(TEXT("gbadge.nwx"), Above);
		if (!S) return;
		// about 9 cm tall at 80 cm, whatever its cells' size (twice the first 4.5: hard to see; user 2026-09-28)
		const float H = float(S->GetActorScale3D().Z);          // the quad is a unit tall: its Z scale is its height in cm
		if (H > 0.1f) S->SetActorScale3D(S->GetActorScale3D() * (9.f / H));
		TInlineComponentArray<UPrimitiveComponent*> Parts(S);
		for (UPrimitiveComponent* P : Parts) P->SetDepthPriorityGroup(SDPG_Foreground);     // over the world, as the text
		BadgeIcon = S;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT badge icon on the HUD (%.0f cm art)"), H);
	}
	// the last 10 seconds: on and off once a second, a warning it is running out (user 2026-09-27)
	const bool bBlinkOff = L->BadgeLeft <= 10.f && FMath::Frac(L->BadgeLeft) < 0.5f;
	S->SetActorHiddenInGame(bBlinkOff);
	S->SetActorLocation(Above);
}

FString AOutlawsPlayer::ItemStem(const FString& Item)
{
	// trailing digits go (igear1, igear2...); a trailing letter after "key" too (the stone keys isqrkeyA..E)
	FString S = Item.ToLower();
	while (S.Len() > 1 && FChar::IsDigit(S[S.Len() - 1])) S.RemoveAt(S.Len() - 1);
	if (S.Len() > 4 && S.Mid(S.Len() - 4, 3) == TEXT("key") && FChar::IsAlpha(S[S.Len() - 1])) S.RemoveAt(S.Len() - 1);
	return S;
}

UTexture2D* AOutlawsPlayer::ItemCard(const FString& Item, bool bOn)
{
	const TCHAR* Dir = bOn ? TEXT("on") : TEXT("off");
	UTexture2D* T = KpfTexture(FString::Printf(TEXT("gfx/wheel/items/%s/%s.png"), Dir, *Item));
	if (!T)
	{
		// (BUG-WHEELINV) the remaster's card names differ from a few ITMs: isteekey's card is isteelkey, ielixer's ielix, and
		// the stone keys' letter is upper case (isqrkeyA); a key with no card of its own is listed by name
		FString Card = Item.ToLower();
		if (Card == TEXT("isteekey")) Card = TEXT("isteelkey");
		else if (Card == TEXT("ielixer")) Card = TEXT("ielix");
		else if (Card.Len() > 4 && Card.Mid(Card.Len() - 4, 3) == TEXT("key") && FChar::IsAlpha(Card[Card.Len() - 1])) Card[Card.Len() - 1] = FChar::ToUpper(Card[Card.Len() - 1]);
		if (Card != Item) T = KpfTexture(FString::Printf(TEXT("gfx/wheel/items/%s/%s.png"), Dir, *Card));
	}
	if (!T) T = KpfTexture(FString::Printf(TEXT("gfx/wheel/items/%s/%s.png"), Dir, *ItemStem(Item)));
	return T;
}

bool AOutlawsPlayer::WristIcon(const FString& Key, FWristIconSrc& Out)
{
	AOutlawsLevelActor* L = Level.Get();
	const FString CacheKey = Key + (L && L->bHDTextures ? TEXT("#hd") : TEXT("#sd"));
	if (const FWristIconSrc* Hit = WristIconCache.Find(CacheKey))
	{
		Out = *Hit;
		return Out.Tex != nullptr;
	}
	FWristIconSrc Src;
	if (Key.StartsWith(TEXT("nwx:")))
	{
		if (L) L->SpriteIcon(Key.Mid(4) + TEXT(".nwx"), Src.Tex, Src.Scale, Src.Offset, Src.Aspect);
	}
	else if (Key.StartsWith(TEXT("kpf:")))
	{
		// the remaster's wheel cards (152 x 216), by the item's name or its type
		Src.Tex = ItemCard(Key.Mid(4), true);
		Src.Aspect = 152.f / 216.f;
	}
	WristIconCache.Add(CacheKey, Src);
	Out = Src;
	return Src.Tex != nullptr;
}

UStaticMeshComponent* AOutlawsPlayer::WristIconPlane(int32 Index)
{
	while (WristIcons.Num() <= Index)
	{
		UStaticMeshComponent* P = NewObject<UStaticMeshComponent>(this);
		P->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane")));
		P->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		P->SetCastShadow(false);
		P->SetupAttachment(WristRoot);
		P->RegisterComponent();
		FOutlawsViewCone::WidenHandBounds(P);
		UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_WristIcon.M_OLR_WristIcon"));
		if (!Base) Base = LoadOverlayMaterial(TEXT("M_OLR_UIOverlay"));
		if (Base)
		{
			UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, this);
			P->SetMaterial(0, Mid);
			KeepAlive.Add(Mid);
		}
		WristIcons.Add(P);
	}
	return WristIcons[Index];
}

void AOutlawsPlayer::LayoutWrist()
{
	// the panel, in cm: x to the viewer's right (the root's -Y), y down (-Z), the root's X toward the eyes.
	// One row: the heart and health, the plate and armour.  Each a share of full health (user 2026-09-29: the keys and
	// gold went stale between levels): health 10 points = 100%, a plate count of n absorbs n/10 points (n%)
	AOutlawsLevelActor* L = Level.Get();
	if (!L) return;
	const int32 Health = FMath::RoundToInt(L->GetHealth() * 10.f);
	const int32 Armour = L->GetArmour();
	const TArray<FString> Items;
	FString Sig = FString::Printf(TEXT("%d|%d|%d|"), Health, Armour, L->bHDTextures ? 1 : 0);
	if (Sig == WristLast) return;
	WristLast = Sig;

	const FQuat Facing = FRotationMatrix::MakeFromXY(FVector(0.0, -1.0, 0.0), FVector(0.0, 0.0, -1.0)).ToQuat();
	constexpr float IconH = 2.6f, Gap = 0.35f, RowStep = 3.1f;
	int32 NIcon = 0, NText = 0;
	float X = 0.f, Y = 0.f, Widest = 0.f;
	auto Icon = [&](const FString& Key) -> float
	{
		FWristIconSrc Src;
		if (!WristIcon(Key, Src)) return 0.f;
		WristShownKeys += (WristShownKeys.IsEmpty() ? TEXT("") : TEXT(" ")) + Key;
		const float W = IconH * Src.Aspect;
		UStaticMeshComponent* P = WristIconPlane(NIcon++);
		if (UMaterialInstanceDynamic* M = Cast<UMaterialInstanceDynamic>(P->GetMaterial(0)))
		{
			M->SetTextureParameterValue(TEXT("Tex"), Src.Tex);
			M->SetVectorParameterValue(TEXT("UVScale"), FLinearColor(Src.Scale.X, Src.Scale.Y, 0.f, 0.f));
			M->SetVectorParameterValue(TEXT("UVOffset"), FLinearColor(Src.Offset.X, Src.Offset.Y, 0.f, 0.f));
		}
		P->SetRelativeLocationAndRotation(FVector(0.0, -(X + W * 0.5f), -(Y + IconH * 0.5f)), Facing);
		P->SetRelativeScale3D(FVector(W / 100.0, IconH / 100.0, 1.0));
		P->SetVisibility(true);
		X += W + Gap;
		return W;
	};
	// the numbers: outlined plates as the ammo counts are (user 2026-10-01), left-aligned on a 4:1 canvas whose left
	// edge is at X; the letters about the old text's size; red when low
	constexpr float PlateH = 2.6f;
	auto Number = [&](const FString& Str, const FColor& Colour, float Size = 1.9f)
	{
		const int32 k = WristPlatesUsed++;
		while (WristPlates.Num() <= k) WristPlates.Add(MakeTextPlate(WristRoot, PlateH, true));
		const int32 Pi = WristPlates[k];
		SetTextPlate(Pi, Str, Colour == FColor(255, 90, 60));
		PlateHolders[Pi]->SetRelativeLocationAndRotation(FVector(0.05, -(X + PlateH * 2.f), -(Y + IconH * 0.5f)), FRotator::ZeroRotator);
		PlateHolders[Pi]->SetRelativeScale3D(FVector::OneVector);
		PlatePlanes[Pi]->SetVisibility(true);
		X += Str.Len() * Size * 0.62f + Gap * 2.f;
	};
	WristPlatesUsed = 0;
	WristShownKeys.Reset();
	for (UTextRenderComponent* T : WristNumbers) if (T) T->SetVisibility(false);     // (the old text numbers)
	const FColor Ink(235, 220, 180), Low(255, 90, 60);
	// row 1: health
	Icon(TEXT("nwx:intheart"));
	Number(FString::Printf(TEXT("%d%%"), Health), Health < 30 ? Low : Ink);
	Widest = FMath::Max(Widest, X);
	// row 2, under it as the medical bag's row was (BUG-WRISTPLATE, user 2026-10-15): the plate worn on the chest
	// (REQ-CHESTPLATE), only while one is worn
	if (Armour > 0)
	{
		X = 0.f; Y += RowStep;
		Icon(TEXT("nwx:ibplate"));
		Number(FString::Printf(TEXT("%d%%"), Armour), Ink);
		Widest = FMath::Max(Widest, X);
	}
	// the keys and papers: cards only, no names, a row for each type (the three stone keys, the gears...),
	// each row centred (user 2026-09-27)
	TArray<FString> Stems;
	TArray<TArray<FString>> Rows;
	for (const FString& It : Items)
	{
		if (It == TEXT("ilantern") || It == TEXT("ioil") || !ItemCard(It, true)) continue;
		const FString Stem = ItemStem(It);
		int32 r = Stems.IndexOfByKey(Stem);
		if (r == INDEX_NONE) { r = Stems.Add(Stem); Rows.AddDefaulted(); }
		Rows[r].Add(It);
	}
	TArray<TPair<int32, float>> RowSpans;      // first icon index and width of every row, to centre them
	RowSpans.Emplace(0, X);
	int32 RowFirst = 0;
	for (const TArray<FString>& Row : Rows)
	{
		X = 0.f; Y += RowStep;
		RowFirst = NIcon;
		for (const FString& It : Row) Icon(TEXT("kpf:") + It);
		RowSpans.Emplace(RowFirst, X);
		Widest = FMath::Max(Widest, X);
	}
	// centre the panel on the wrist, and each card row in it
	const float Height = Y + IconH;
	for (int32 i = 0; i < WristIcons.Num(); ++i)
	{
		if (i >= NIcon) { WristIcons[i]->SetVisibility(false); continue; }
		float RowShift = 0.f;
		for (int32 s = 1; s < RowSpans.Num(); ++s)
		{
			const int32 End = s + 1 < RowSpans.Num() ? RowSpans[s + 1].Key : NIcon;
			if (i >= RowSpans[s].Key && i < End) RowShift = (Widest - RowSpans[s].Value) * 0.5f;
		}
		WristIcons[i]->AddRelativeLocation(FVector(0.0, Widest * 0.5 - RowShift, Height * 0.5));
	}
	for (int32 i = 0; i < WristPlates.Num(); ++i)
	{
		if (i < WristPlatesUsed) PlateHolders[WristPlates[i]]->AddRelativeLocation(FVector(0.0, Widest * 0.5, Height * 0.5));
		else PlatePlanes[WristPlates[i]]->SetVisibility(false);
	}
	WristIconsUsed = NIcon;
	// (tests) where each card and number sits: right and up of the panel's middle, in cm
	TArray<FString> Keys;
	WristShownKeys.ParseIntoArray(Keys, TEXT(" "));
	WristRowsLog.Reset();
	for (int32 i = 0; i < NIcon; ++i)
	{
		const FVector P = WristIcons[i]->GetRelativeLocation();
		WristRowsLog += FString::Printf(TEXT("%s[card %s at %.1f,%.1f]"), WristRowsLog.IsEmpty() ? TEXT("") : TEXT(" "), Keys.IsValidIndex(i) ? *Keys[i] : TEXT("?"), -P.Y, P.Z);
	}
	for (int32 i = 0; i < WristPlatesUsed; ++i)
	{
		const FVector P = PlateHolders[WristPlates[i]]->GetRelativeLocation();
		WristRowsLog += FString::Printf(TEXT(" [number %d at %.1f,%.1f]"), i, -P.Y, P.Z);
	}
}

void AOutlawsPlayer::UpdateWrist()
{
	OL_PERF_SCOPE("Player: wrist");
	// sits just behind the off hand where a watch face would be, turned to the eyes; only shown
	// when that wrist is raised in front of the face (-OLWristTest: held in front of the desktop camera)
	UpdateBadgeIcon();
	WristText->SetVisibility(false);
	const bool bTest = OLFlags::Param(TEXT("OLWristTest"));
	static const bool bHandTest = OLFlags::Param(TEXT("OLChestTest"));     // (REQ-CHESTPLATE: on the off hand, on the desktop)
	if (!bVR && !bTest && !bHandTest)
	{
		if (WristRoot) WristRoot->SetVisibility(false, true);
		return;
	}
	const FVector Eye = Camera->GetComponentLocation();
	FVector Wrist;
	if (bTest)
	{
		Wrist = Eye + Camera->GetForwardVector() * 40.0 - FVector(0.0, 0.0, 2.0);
	}
	else
	{
		UOutlawsBoxHand* Off = LeftDominant() ? RightHand.Get() : LeftHand.Get();
		Wrist = Off->GetComponentTransform().TransformPosition(FVector(-8.0, LeftDominant() ? 3.2 : -3.2, 1.0));
	}
	const FVector ToWrist = Wrist - Eye;
	// (BUG-FOVCLIP: was a 41 degree cone round the view's middle, so a wrist held low, in plain sight at the bottom of the
	// Quest's view, stayed blank until raised) anywhere the eyes can see it, the headset's own view and a margin
	const bool bShow = bTest || bHandTest || (ToWrist.Size() < 60.0
		&& FOutlawsViewCone::Get().Contains(Eye, Camera->GetComponentQuat(), Wrist)
		&& !AimingDownSights(0) && !AimingDownSights(1));     // (not over the sights of a gun brought up to the eye)
	if (!WristRoot)
	{
		WristRoot = NewObject<USceneComponent>(this, TEXT("WristDisplay"));
		WristRoot->SetupAttachment(VROrigin);
		WristRoot->RegisterComponent();
		WristLast.Reset();
	}
	WristRoot->SetVisibility(bShow, true);
	if (!bShow)
	{
		return;
	}
	WristRoot->SetWorldLocationAndRotation(Wrist, (-ToWrist).Rotation());
	LayoutWrist();
	// the layout hid the spare parts: the root's visibility must not have shown them again (BUG-WRISTPLATE: was "hide what
	// is hidden", but the root had just shown every card, so a card no longer laid out, the plate once the plate was gone,
	// came back where it last sat, over the health number)
	for (int32 i = WristIconsUsed; i < WristIcons.Num(); ++i) WristIcons[i]->SetVisibility(false);
	for (int32 i = WristPlatesUsed; i < WristPlates.Num(); ++i) PlatePlanes[WristPlates[i]]->SetVisibility(false);
	for (UTextRenderComponent* T : WristNumbers) if (T) T->SetVisibility(false);
}

// ---- smoke test ----------------------------------------------------------------------------------

#if WITH_OUTLAWS_TESTS
void AOutlawsPlayer::ShotTest(float Dt)
{
	// -OLShots: stand before a few see-through mids (glass, then others) and screenshot each
	ShotTime += Dt;
	AOutlawsLevelActor* L = Level.Get();
	if (!L) return;
	L->bEnemiesActive = false;
	L->bInvincible = true;
	if (OLFlags::HasValue(TEXT("OLGlyphTest"))) return;     // (REQ-BUTTONGLYPHS: GlyphTestTick, from Tick)
	// each test registers itself from its own file with the flags that pick it (OutlawsShotTests.h, ShotTests\*.cpp); the
	// first one, by order, whose flags are set runs (OLShots, the see-through mids, when none is)
	if (const FOutlawsShotTest* T = OutlawsShotTests::Find()) T->Run(*this, Dt, L);
}

void AOutlawsPlayer::SmokeTest(float Dt)
{
	// -OLSmokeTest (no headset): walk, crouch, crouch-jump, turn, swim; log each; quit
	SmokeTime += Dt;
	AOutlawsLevelActor* L = Level.Get();
	const float Half = GetCapsuleComponent()->GetUnscaledCapsuleHalfHeight();
	const float Feet = GetActorLocation().Z - Half;
	auto Report = [&](const FString& What)
	{
		const UCharacterMovementComponent* M = GetCharacterMovement();
		UE_LOG(LogOutlaws, Display, TEXT("OLTEST %s | pos %s yaw %.1f mode %d half %.0f crouched %d swimming %d eye %.0f"), *What, *GetActorLocation().ToString(),
			GetActorRotation().Yaw, int32(M->MovementMode.GetValue()), Half, bIsCrouched ? 1 : 0, bSwimming ? 1 : 0, Camera->GetComponentLocation().Z - Feet);
	};
	if (SmokeStep >= 4 && SmokeStep <= 5)
	{
		SmokeValue = FMath::Max(SmokeValue, Feet);
	}
	switch (SmokeStep)
	{
	case 0:
		if (SmokeTime > 1.5f)
		{
			Report(TEXT("start"));
			// placeholder guns load from their OBJ files (VR hands hold them; here they just attach)
			Equip(1, 0);
			Report(FString::Printf(TEXT("equip revolver: %s"), Equipped[1] == 0 ? TEXT("ok") : TEXT("FAILED")));
			FireGun(1, false);
			Report(FString::Printf(TEXT("fired revolver: %d rounds left (expect 5)"), Mag[1]));
			Equip(1, 1);
			Report(FString::Printf(TEXT("equip rifle: %s"), Equipped[1] == 1 ? TEXT("ok") : TEXT("FAILED")));
			// only revolvers pair up
			Equip(0, 0);
			Equip(1, 0);
			const bool bPair = Equipped[0] == 0 && Equipped[1] == 0;
			Equip(1, 1);
			Report(FString::Printf(TEXT("dual wield: two revolvers %s, rifle keeps the revolver in the other hand %s"), bPair ? TEXT("ok") : TEXT("FAILED"),
				Equipped[0] == 0 && Equipped[1] == 1 ? TEXT("ok") : TEXT("FAILED")));
			// one-handed weapons mix: a revolver and dynamite stay together; a rifle drawn leaves the other hand its weapon
			// (user 2026-10-01: a weapon in the hand no bar to the two-handed hold)
			{
				Equip(0, 6);
				Equip(1, 0);
				const bool bMix = Equipped[0] == 6 && Equipped[1] == 0;
				Equip(0, 7);
				const bool bMix2 = Equipped[0] == 7 && Equipped[1] == 0;
				Mag[1] = 3;
				const bool bTraded = TradeWeapons() && Equipped[0] == 0 && Equipped[1] == 7 && Mag[0] == 3;
				Equip(1, 1);
				Report(FString::Printf(TEXT("mix and match: revolver + dynamite %s, revolver + knife %s, traded hands with its rounds %s, rifle keeps the revolver in the other hand %s"),
					bMix ? TEXT("ok") : TEXT("FAILED"), bMix2 ? TEXT("ok") : TEXT("FAILED"), bTraded ? TEXT("ok") : TEXT("FAILED"),
					Equipped[0] == 0 && Equipped[1] == 1 ? TEXT("ok") : TEXT("FAILED")));
			}
			FireGun(1, false);
			Report(FString::Printf(TEXT("gunfire effects: %d smoke puffs (expect 8+), %d cases (expect 2: the revolver and the rifle both drop one on the shot, spent %d)"),
				Puffs.Num(), Cases.Num(), Spent[0]));
			// the other weapons: shotguns (pellets, both barrels), the gatling (the game's model, a belt),
			// dynamite and a knife thrown (the blast comes later; nobody is hurt by it here)
			if (L)
			{
				L->bInvincible = true;
				L->GiveAllWeapons();
				Equip(1, 2);
				FireGun(1, false);
				Report(FString::Printf(TEXT("shotgun: equipped %d, mag %d (expect 0 of 1)"), Equipped[1], Mag[1]));
				Equip(1, 4);
				FireGun(1, false);
				Report(FString::Printf(TEXT("double barrel: equipped %d, mag %d (expect 0 of 2: both barrels)"), Equipped[1], Mag[1]));
				Equip(1, 5);
				const int32 GatSections = WeaponMesh[1] ? WeaponMesh[1]->GetNumSections() : 0;
				FireGun(1, false);
				Report(FString::Printf(TEXT("gatling: equipped %d, model sections %d (expect 1+), mag %d (expect 99)"), Equipped[1], GatSections, Mag[1]));
				Equip(1, 6);
				Mag[1] = 1;
				bFuseLit[1] = true;
				Fuse[1] = 0.8f;
				ThrowHeld(1);
				Equip(1, 7);
				Mag[1] = 1;
				ThrowHeld(1);
				Report(FString::Printf(TEXT("dynamite and knife thrown: %d in flight (expect 2)"), Thrown.Num()));
				// a thrown stick can be shot: its mesh answers a shot trace, and the shot sets it off
				{
					AActor* Stick = nullptr;
					FVector At = FVector::ZeroVector;
					for (const FThrown& T : Thrown) if (T.Type == 1 && T.Actor.IsValid()) { Stick = T.Actor.Get(); At = T.Pos; }
					bool bTraced = false;
					if (Stick)
					{
						FHitResult TH;
						FCollisionQueryParams TQ(SCENE_QUERY_STAT(OutlawsStickTest), true, this);
						bTraced = GetWorld()->LineTraceSingleByChannel(TH, At + FVector(0.0, 0.0, 60.0), At - FVector(0.0, 0.0, 60.0), ECC_Visibility, TQ) && TH.GetActor() == Stick;
					}
					const bool bShot = Stick && ShootThrown(Stick);
					Report(FString::Printf(TEXT("thrown dynamite shot: a shot trace hits it %s, it went off %s (%d left in flight, expect 1)"),
						bTraced ? TEXT("ok") : TEXT("FAILED"), bShot ? TEXT("ok") : TEXT("FAILED"), Thrown.Num()));
				}
				Equip(1, 1);
			}
			// the game menu: every page reachable from the pause menu, read from Common.kpf
			if (LoadMenuSource())
			{
				TArray<FString> Queue = { TEXT("main_game.rml") };
				TSet<FString> Seen = { TEXT("main_game.rml") };
				int32 Pages = 0, Items = 0, Missing = 0;
				FString Main, Options;
				while (Queue.Num() > 0)
				{
					const FString File = Queue.Pop();
					OutlawsMenu::FPage Page;
					if (!MenuSource->GetPage(File, Page))
					{
						++Missing;
						continue;
					}
					++Pages;
					Items += Page.Items.Num();
					TArray<FString> Labels;
					for (const OutlawsMenu::FItem& I : Page.Items)
					{
						Labels.Add(I.Label);
						if ((I.Kind == OutlawsMenu::EItemKind::Link || I.Kind == OutlawsMenu::EItemKind::Dialog) && !Seen.Contains(I.Target))
						{
							Seen.Add(I.Target);
							Queue.Add(I.Target);
						}
					}
					if (File == TEXT("main_game.rml")) Main = Page.Title + TEXT(": ") + FString::Join(Labels, TEXT(", "));
					if (File == TEXT("options.rml")) Options = Page.Title + TEXT(": ") + FString::Join(Labels, TEXT(", "));
				}
				Report(FString::Printf(TEXT("menu: %d pages, %d items, %d missing | %s | %s"), Pages, Items, Missing, *Main, *Options));
				// what the scrub leaves of each page (only what works), and the settings file round trip
				for (const TCHAR* F : { TEXT("main_game.rml"), TEXT("options.rml"), TEXT("options_display.rml"), TEXT("options_sound.rml") })
				{
					OutlawsMenu::FPage Page;
					if (!MenuSource->GetPage(F, Page)) continue;
					ScrubPage(Page);
					TArray<FString> Labels;
					for (const OutlawsMenu::FItem& I : Page.Items) Labels.Add(I.Label);
					Report(FString::Printf(TEXT("scrubbed %s: %s"), F, *FString::Join(Labels, TEXT(", "))));
				}
				SaveSettings();
				Report(FString::Printf(TEXT("settings file: %d keys"), AOutlawsLevelActor::ReadSettings().Num()));
			}
			else
			{
				Report(TEXT("menu: Common.kpf not readable"));
			}
			Equip(1, -1);
			SmokeMark = GetActorLocation();
			SmokeStart = SmokeMark;
			HoldMoveStick(FVector2D(0.0, 1.0), 2.f);
			SmokeStep = 1;
		}
		break;
	case 1:
		if (SmokeTime > 4.0f)
		{
			Report(FString::Printf(TEXT("walked %.0f cm in 2 s"), FVector::Dist2D(SmokeMark, GetActorLocation())));
			SetCrouchHeld(true);
			SmokeStep = 2;
		}
		break;
	case 2:
		if (SmokeTime > 4.6f) { Report(TEXT("crouch held")); SetCrouchHeld(false); SmokeStep = 3; }
		break;
	case 3:
		if (SmokeTime > 5.2f) { Report(TEXT("crouch released")); SmokeMark = FVector(0.0, 0.0, Feet); SmokeValue = Feet; DoJump(); SmokeStep = 4; }
		break;
	case 4:
		if (SmokeTime > 5.32f) { SetCrouchHeld(true); SmokeStep = 5; }          // crouch just after take-off
		break;
	case 5:
		if (SmokeTime > 6.6f)
		{
			Report(FString::Printf(TEXT("crouch-jump: feet rose %.0f cm (a standing jump alone rises ~%.0f)"), SmokeValue - SmokeMark.Z,
				FMath::Square(GetCharacterMovement()->JumpZVelocity) / (2.0 * 980.0 * GetCharacterMovement()->GravityScale)));
			SetCrouchHeld(false);
			SmokeStep = 6;
		}
		break;
	case 6:
		if (SmokeTime > 7.2f)
		{
			const float Yaw0 = GetActorRotation().Yaw;
			TurnBy(45.f);
			Report(FString::Printf(TEXT("turn %.1f -> %.1f"), Yaw0, GetActorRotation().Yaw));
			FVector W;
			if (L && L->FindWaterPoint(W))
			{
				TeleportTo(W, GetActorRotation(), false, true);
				Report(FString::Printf(TEXT("teleported into water at %s"), *W.ToString()));
			}
			else
			{
				Report(TEXT("no deep water in this level"));
			}
			SmokeStep = 7;
		}
		break;
	case 7:
		if (SmokeTime > 8.4f)
		{
			// the sink: the sector's gravity drifts the body down, at the water's sink speed at most (user 2026-09-29)
			float Surface = 0.f;
			const bool bWater = L && L->WaterAt(GetActorLocation(), Surface);
			Report(FString::Printf(TEXT("after 1.2 s in water: in water %d, surface z %.0f, centre %.0f below it (expect 120-170: sinking from 60)"), bWater ? 1 : 0, Surface, Surface - GetActorLocation().Z));
			// a swim up: the look pitched near straight up, the stick full forward (at the saved pace: teleport moves halve it)
			if (Controller) Controller->SetControlRotation(FRotator(80.0, GetControlRotation().Yaw, 0.0));
			HoldMoveStick(FVector2D(0.0, 1.0), 1.0f);
			Report(FString::Printf(TEXT("swimming up at pace %.2f"), L ? L->Pace() : 1.f));
			SmokeStep = 70;
		}
		break;
	case 70:
		if (SmokeTime > 10.f)
		{
			float Surface = 0.f;
			const bool bWater = L && L->WaterAt(GetActorLocation(), Surface);
			Report(FString::Printf(TEXT("after a swim up: in water %d, centre %.0f below the surface (expect ~%.0f: the surface holds the body), held %d (expect 1)"), bWater ? 1 : 0, Surface - GetActorLocation().Z, SwimFloatDepth, bSurfaceLocked ? 1 : 0));
			// a dive: the head tilted down past DivePitch while moving
			if (Controller) Controller->SetControlRotation(FRotator(-45.0, GetControlRotation().Yaw, 0.0));
			HoldMoveStick(FVector2D(0.0, 0.5), 0.9f);
			SmokeStep = 71;
		}
		break;
	case 71:
		if (SmokeTime > 11.f)
		{
			float Surface = 0.f;
			const bool bWater = L && L->WaterAt(GetActorLocation(), Surface);
			Report(FString::Printf(TEXT("after a dive: in water %d, centre %.0f below the surface (expect 70+: the head down 45 and moving lets go), held %d (expect 0)"), bWater ? 1 : 0, Surface - GetActorLocation().Z, bSurfaceLocked ? 1 : 0));
			if (Controller) Controller->SetControlRotation(FRotator(0.0, GetControlRotation().Yaw, 0.0));
			{
				// the sector palette seen from a point 40 cm under the surface (the water's own palette)
				FLinearColor Rows[3];
				const bool bGrade = L && L->PaletteGradeAt(FVector(GetActorLocation().X, GetActorLocation().Y, Surface - 40.0), Rows);
				Report(FString::Printf(TEXT("palette under the surface: grade %d (expect 0: the water's own tint instead, 2026-09-28), red row %s"), bGrade ? 1 : 0, bGrade ? *Rows[0].ToString() : TEXT("-")));
			}
			SmokeStep = 8;
		}
		break;
	case 8:
		if (SmokeTime > 11.5f && L)
		{
			// stats: armour absorbs first, canteen and medical bag heal, a walked-over ammo box counts
			TeleportTo(SmokeStart, GetActorRotation(), false, true);
			L->bInvincible = false;
			L->DamagePlayer(3.f);
			Report(FString::Printf(TEXT("hurt 3: health %.1f (expect 7.0), armour %d"), L->GetHealth(), L->GetArmour()));
			L->AddCount(TEXT("ibplate"), 50);
			L->DamagePlayer(2.f);
			Report(FString::Printf(TEXT("light plate then hurt 2: armour %d (expect 30), health %.1f (expect 7.0)"), L->GetArmour(), L->GetHealth()));
			L->HealPlayer(1.f);
			L->AddCount(TEXT("idocbag"), 3);
			L->UseMedicalBag();
			Report(FString::Printf(TEXT("canteen then medical bag: health %.1f (expect 10.0), bag left %d (expect 1)"), L->GetHealth(), L->GetCount(TEXT("idocbag"))));
			SmokeValue = L->GetCount(TEXT("ibullit")) + L->GetCount(TEXT("icart")) + L->GetCount(TEXT("ishells"));
			const FVector Ammo = L->PickupLocationByClass(TEXT("ammo"));
			if (!Ammo.IsZero())
			{
				TeleportTo(Ammo + FVector(0.0, 0.0, Half + 2.0), GetActorRotation(), false, true);
				Report(FString::Printf(TEXT("stepped onto an ammo pickup at %s with %.0f rounds in reserve"), *Ammo.ToString(), SmokeValue));
			}
			else
			{
				Report(TEXT("no ammo pickup in this level"));
			}
			SmokeStep = 9;
		}
		break;
	case 9:
		if (SmokeTime > 12.5f && L)
		{
			const int32 Now = L->GetCount(TEXT("ibullit")) + L->GetCount(TEXT("icart")) + L->GetCount(TEXT("ishells"));
			Report(FString::Printf(TEXT("ammo reserve after the pickup: %d (was %.0f); message in view: %s"), Now, SmokeValue, HudLines.Num() ? *HudLines.Last() : TEXT("NONE")));
			L->DamagePlayer(50.f);
			Report(FString::Printf(TEXT("hurt 50: dead %d (expect 1), health %.1f"), L->IsPlayerDead() ? 1 : 0, L->GetHealth()));
			SmokeStep = 10;
		}
		break;
	case 10:
		if (SmokeTime > 14.5f && L)
		{
			Report(FString::Printf(TEXT("dead view: eye %.0f cm above the feet (expect about 25), sepia %.2f"),
				Camera->GetComponentLocation().Z - (GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight()), DeathBlend));
			DoJump();            // desktop respawn button
			SmokeStep = 20;
		}
		break;
	case 20:
		if (SmokeTime > 17.5f && L)
		{
			Report(FString::Printf(TEXT("after death: dead %d (expect 0), health %.1f (expect 10.0), armour %d (expect 0), bullets %d (expect 24), ammo pickups back %d"),
				L->IsPlayerDead() ? 1 : 0, L->GetHealth(), L->GetArmour(), L->GetCount(TEXT("ibullit")), L->PickupLocationByClass(TEXT("ammo")).IsZero() ? 0 : 1));
			// enemies: one bad guy on the open strip the walk test crossed, 7 m ahead of the start
			SmokeEnemy = nullptr;
			const FVector Fwd = FRotator(0.0, GetActorRotation().Yaw, 0.0).Vector();
			for (const TWeakObjectPtr<AOutlawsEnemy>& W : L->GetEnemies())
			{
				if (W.IsValid() && !W->IsDead() && !W->IsDormant() && !W->bBoss && W->Def.ShootChor >= 0 && W->Def.Aggressiveness > 0)
				{
					SmokeEnemy = W.Get();
					break;
				}
			}
			Report(FString::Printf(TEXT("enemies: %d spawned"), L->GetEnemies().Num()));
			if (AOutlawsEnemy* E = SmokeEnemy.Get())
			{
				const float EHalf = E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
				FVector At = GetActorLocation() + Fwd * 700.0;
				FHitResult Floor;
				FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsSmoke), true, this);
				Q.AddIgnoredActor(E);
				if (GetWorld()->LineTraceSingleByChannel(Floor, At + FVector(0.0, 0.0, 100.0), At - FVector(0.0, 0.0, 400.0), ECC_Visibility, Q, PlayerNoPawns()))
				{
					At = Floor.ImpactPoint + FVector(0.0, 0.0, EHalf + 2.0);
				}
				E->SetActorLocation(At, false, nullptr, ETeleportType::TeleportPhysics);
				// a bullet from the eyes toward it stops on its body
				FHitResult Shot;
				const FCollisionQueryParams ShotQ(SCENE_QUERY_STAT(OutlawsSmoke), true, this);
				const bool bShot = GetWorld()->LineTraceSingleByChannel(Shot, Camera->GetComponentLocation(), E->GetActorLocation(), ECC_Visibility, ShotQ);
				Report(FString::Printf(TEXT("enemy %s placed at %s, health %.1f; a bullet trace hits it: %s"), *E->GetOutlawsName(), *At.ToString(), E->GetHealth(),
					bShot && Shot.GetActor() == E ? TEXT("yes") : *FString::Printf(TEXT("NO (%s)"), Shot.GetActor() ? *Shot.GetActor()->GetName() : TEXT("nothing"))));
				// heavy plate first: on Bad a bad guy's chest hit is 3.75, and a death here would restart the level
				L->AddCount(TEXT("ibplate"), 150);
				SmokeValue = L->GetHealth() * 10.f + L->GetArmour();
				L->bEnemiesActive = true;
				E->Wake(false);      // placed facing wherever its OBT yaw pointed: wake it rather than wait to be seen
				SmokeStep = 11;
			}
			else
			{
				Report(TEXT("no fighting enemy in this level"));
				SmokeStep = 13;
			}
		}
		break;
	case 11:
		if (SmokeTime > 22.5f && L)
		{
			AOutlawsEnemy* E = SmokeEnemy.Get();
			Report(FString::Printf(TEXT("after 5 s with the enemy awake: state %d (1 alert, 2 shooting), sub-state %d, player health %.1f armour %d (health x10 + armour was %.0f: expect lower)"),
				E ? int32(E->GetState()) : -1, E ? E->GetSubState() : -1, L->GetHealth(), L->GetArmour(), SmokeValue));
			L->bEnemiesActive = false;
			SmokeValue = L->GetKills();
			if (E)
			{
				const FVector Dir = (E->GetActorLocation() - Camera->GetComponentLocation()).GetSafeNormal();
				// two body shots from where the player stands (x1 middle band: a chest hit would be x2)
				E->TakeHit(2.0f, E->GetActorLocation() - FVector(0.0, 0.0, 10.0), Dir, Camera->GetComponentLocation());
				E->TakeHit(2.0f, E->GetActorLocation() - FVector(0.0, 0.0, 10.0), Dir, Camera->GetComponentLocation());
				// and a point-blank shotgun blast from below into the body: it should fly
				for (int32 i = 0; i < 6; ++i) E->TakeHit(1.5f, E->GetActorLocation(), FVector(0.3, 0.0, 1.0).GetSafeNormal(), E->GetActorLocation() - FVector(0.0, 0.0, 60.0));
			}
			SmokeStep = 12;
		}
		break;
	case 12:
		if (SmokeTime > 24.f && L)
		{
			AOutlawsEnemy* E = SmokeEnemy.Get();
			Report(FString::Printf(TEXT("two revolver hits: dead %d (expect 1), kills %d (was %.0f), drop %s, sprite chor %d"),
				E && E->IsDead() ? 1 : 0, L->GetKills(), SmokeValue, E ? *E->Def.DropItem : TEXT("?"),
				E && E->GetSprite() ? E->GetSprite()->GetPlayingChor() : -1));
			SmokeStep = 13;
		}
		break;
	case 13:
		if (L)
		{
			Report(TEXT("shootables: ") + L->ShootablesReport() + TEXT(" | ") + L->SelfTestShootables());
			// cheats: invincible takes no damage; unlimited ammo keeps the magazine
			const float Before = L->GetHealth();
			L->bInvincible = true;
			L->DamagePlayer(5.f);
			L->bInvincible = false;
			Report(FString::Printf(TEXT("invincible: health %.0f -> %.0f (expect equal); floor sound here '%s'"),
				Before, L->GetHealth(), *L->FloorSoundAt(GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight()))));
			// the boss: summoned (kills = total), he appears on the next 2 s look
			L->bInvincible = true;
			AOutlawsEnemy* B = L->GetBoss();
			Report(FString::Printf(TEXT("boss %s dormant %d, hidden %d"), B ? *B->GetOutlawsName() : TEXT("none"), B && B->IsDormant() ? 1 : 0, B && B->IsHidden() ? 1 : 0));
			L->DebugSummonBoss();
			SmokeValue = SmokeTime;
		}
		SmokeStep = 14;
		break;
	case 14:
		if (L && SmokeTime > SmokeValue + 2.5f)
		{
			AOutlawsEnemy* B = L->GetBoss();
			Report(FString::Printf(TEXT("boss after summon: dormant %d (expect 0), hidden %d (expect 0), at %s"), B && B->IsDormant() ? 1 : 0, B && B->IsHidden() ? 1 : 0,
				B ? *B->GetActorLocation().ToString() : TEXT("-")));
			for (int32 i = 0; B && i < 20 && !B->IsDead(); ++i)
			{
				B->TakeHit(4.f, B->GetActorLocation(), FVector(1.0, 0.0, 0.0), B->GetActorLocation() - FVector(100.0, 0.0, 0.0));
			}
			Report(FString::Printf(TEXT("boss shot: dead %d (expect 1)"), B && B->IsDead() ? 1 : 0));
			SmokeValue = SmokeTime;
			SmokeStep = 15;
		}
		break;
	case 15:
		if (L && SmokeTime > SmokeValue + 2.5f)
		{
			Report(FString::Printf(TEXT("level won %d (expect 1)"), L->IsLevelWon() ? 1 : 0));
			SmokeValue = SmokeTime;
			SmokeStep = 16;
		}
		break;
	case 16:
		// the movie (skipped when it will not play here) and the next story level, carrying the kit
		if (L && L->IsMoviePlaying()) L->SkipMovie();
		if (L && SmokeTime > SmokeValue + 8.f)
		{
			Report(FString::Printf(TEXT("next level: %s (expect train), shells %d, gatling belts %d (carried)"), *L->LevelName,
				L->GetCount(TEXT("ishells")), L->GetCount(TEXT("iclip"))));
			Report(TEXT("done"));
			SmokeStep = 17;
			FGenericPlatformMisc::RequestExit(false);
		}
		break;
	default:
		break;
	}
}

// -OLTestStickChord: both sticks held 1.7 s (a perf mark, no recenter), then both clicked for 0.3 s (a recenter)
void AOutlawsPlayer::TickStickChordTest()
{
	static double Start = 0.0;
	static int32 Step = 0;
	const double Now = FPlatformTime::Seconds();
	if (Start == 0.0) Start = Now;
	const double T = Now - Start;
	if (Step == 0 && T > 8.0) { ++Step; PressLeftStick(); PressRightStick(); UE_LOG(LogOutlaws, Log, TEXT("OLRT stick chord test: both held")); }
	else if (Step == 1 && T > 9.7) { ++Step; ReleaseLeftStick(); ReleaseRightStick(); }
	else if (Step == 2 && T > 12.0) { ++Step; PressLeftStick(); PressRightStick(); UE_LOG(LogOutlaws, Log, TEXT("OLRT stick chord test: both clicked")); }
	else if (Step == 3 && T > 12.3) { ++Step; ReleaseLeftStick(); ReleaseRightStick(); }
	else if (Step == 4 && T > 14.0) { ++Step; UE_LOG(LogOutlaws, Log, TEXT("OLRT stick chord test: done")); FGenericPlatformMisc::RequestExit(false); }
}
#endif // WITH_OUTLAWS_TESTS
