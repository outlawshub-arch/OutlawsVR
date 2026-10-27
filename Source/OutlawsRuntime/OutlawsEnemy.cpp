// Outlaws bad guys: see OutlawsEnemy.h.  Addresses [EXE ...] are in the remaster exe (OutlawsAHOM.exe),
// as read for research/outlaws_enemy_exe.md and its disassembly listings (research/exe_scratch/dis_*.txt).
#include "OutlawsEnemy.h"
#include "OutlawsFlags.h"
#include "OutlawsStats.h"
#include "OutlawsPerf.h"
#include "OutlawsLab.h"
#include "OutlawsTextures.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/FileHelper.h"
#include "OutlawsRuntime.h"

namespace
{
	// the grab points marked by hand on the sprites (user 2026-09-29, tools/olue/placeholders/grab_points.json): per
	// sprite, per frame (known by its biggest piece, its NWX cell, and whether that is mirrored), the hold from the
	// drawing's bottom middle in 1x px (x right, y up)
	struct FGrabPt { int32 Cell = -1; bool bFlip = false; float Ox = 0.f, Oy = 0.f; };
	TMap<FString, TArray<FGrabPt>> GGrabPts;
	bool bGrabPtsLoaded = false;

	void LoadGrabPts()
	{
		if (bGrabPtsLoaded) return;
		bGrabPtsLoaded = true;
		FString Text;
		TSharedPtr<FJsonObject> J;
		if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(FOutlawsAssets::PlaceholderDir(), TEXT("grab_points.json")))
			|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), J) || !J.IsValid()) return;
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Kv : J->Values)
		{
			TArray<FGrabPt>& List = GGrabPts.FindOrAdd(Kv.Key.ToLower());
			for (const TSharedPtr<FJsonValue>& V : Kv.Value->AsArray())
			{
				const TSharedPtr<FJsonObject> O = V->AsObject();
				if (!O) continue;
				FGrabPt P;
				P.Cell = int32(O->GetNumberField(TEXT("cell")));
				P.bFlip = O->GetBoolField(TEXT("flip"));
				P.Ox = float(O->GetNumberField(TEXT("ox")));
				P.Oy = float(O->GetNumberField(TEXT("oy")));
				List.Add(P);
			}
		}
		UE_LOG(LogOutlawsAI, Log, TEXT("OLRT grab points for %d sprites"), GGrabPts.Num());
	}
}
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsCoop.h"
#include "ProceduralMeshComponent.h"
#include "OutlawsSpriteActor.h"
#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Engine/World.h"
#include "CollisionQueryParams.h"
#include "Misc/Paths.h"
#include "HAL/IConsoleManager.h"

// far and unseen (user 2026-10-03): the bad guys this far from every player, in no sector the eye's portals reach, hold still
// (AOutlawsEnemy::UpdateFarFreeze).  ol.FarFreezeEnable 0 turns it off (A/B on the headset); -OLFarFreeze=0|1 sets it for a run.
static TAutoConsoleVariable<int32> CVarOLFarFreezeEnable(TEXT("ol.FarFreezeEnable"), 1,
	TEXT("Far-off bad guys out of sight hold still (1, the default) or think and walk as the near ones do (0)."));
static TAutoConsoleVariable<float> CVarOLFarFreezeDistance(TEXT("ol.FarFreezeDistance"), 10000.f,
	TEXT("How far (cm) from the nearest player an unseen bad guy freezes; he thaws a tenth inside it."));

namespace
{
	constexpr double UnitCm = FOutlawsGeometry::UnitsToCm;
	constexpr float TurnRate = 540.f;          // degrees per second: twice the exe's 270 [EXE 0x14008783c] (user 2026-09-27)
	constexpr float ShareRadiusCm = float(50.0 * FOutlawsGeometry::UnitsToCm);

	// the aggressiveness x confidence decision matrix [EXE 0x1408ff4e0]: 4 retreat, 2 advance, 1 take cover
	const uint8 GDecide[8][8] = {
		{ 4, 4, 4, 4, 4, 4, 1, 1 },
		{ 4, 4, 4, 4, 4, 1, 1, 1 },
		{ 4, 4, 4, 4, 1, 1, 1, 2 },
		{ 4, 4, 4, 1, 1, 1, 2, 2 },
		{ 4, 4, 1, 1, 1, 2, 2, 2 },
		{ 4, 1, 1, 1, 2, 2, 2, 2 },
		{ 1, 1, 1, 2, 2, 2, 2, 2 },
		{ 1, 1, 2, 2, 2, 2, 2, 2 },
	};

	TArray<FString> SoundList(const FString& Text)
	{
		TArray<FString> Parts, Out;
		Text.ParseIntoArray(Parts, TEXT(","));
		for (FString P : Parts)
		{
			P.TrimStartAndEndInline();
			if (!P.IsEmpty()) Out.Add(P);
		}
		return Out;
	}

	FCollisionResponseParams WorldOnly()
	{
		// walls, doors and props; not bodies
		FCollisionResponseParams RP;
		RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
		return RP;
	}

	/** The octagonal distance the enemy code uses everywhere: max + half the min, in units. */
	double OctDist(const FVector& A, const FVector& B)
	{
		const double DX = FMath::Abs(A.X - B.X), DY = FMath::Abs(A.Y - B.Y);
		return (FMath::Max(DX, DY) + 0.5 * FMath::Min(DX, DY)) / UnitCm;
	}

	bool IsMoveSub(int32 S) { return S == 3 || S == 4 || S == 8; }
}

void FOutlawsEnemyDef::Read(const TMap<FString, FString>& Data)
{
	auto F = [&Data](const TCHAR* Key, float& V) { if (const FString* S = Data.Find(Key)) V = FCString::Atof(**S); };
	auto I = [&Data](const TCHAR* Key, int32& V) { if (const FString* S = Data.Find(Key)) V = FCString::Atoi(**S); };
	auto S = [&Data](const TCHAR* Key, FString& V) { if (const FString* T = Data.Find(Key)) V = *T; };
	F(TEXT("HEIGHT"), Height);
	F(TEXT("RADIUS"), Radius);
	F(TEXT("HEALTH"), Health);
	I(TEXT("AGGRESSIVENESS"), Aggressiveness);
	I(TEXT("AGRESSIVENESS"), Aggressiveness);    // the loader maps the misspelling to the same field
	I(TEXT("BASE_MODE"), BaseMode);
	F(TEXT("WALK_SPEED"), WalkSpeed);
	F(TEXT("RUN_SPEED"), RunSpeed);
	F(TEXT("FOV_DIST"), FovDist);
	F(TEXT("FOV"), Fov);                          // degrees in the ITM, half-angle
	// timers: seconds in the ITM (the loader stores milliseconds) [EXE 0x14008fc81]
	F(TEXT("LOS_TIMER_TIME"), LosTimer);
	F(TEXT("SHOOT_TIMER_TIME"), ShootTimer);
	F(TEXT("TAUNT_TIMER_TIME"), TauntTimer);
	F(TEXT("CONFIDENCE"), Confidence);
	F(TEXT("PANIC_RATE"), PanicRate);
	F(TEXT("MOVE_INACCURACY"), MoveInaccuracy);
	F(TEXT("WEAPON_INACCURACY"), WeaponInaccuracy);
	F(TEXT("PANIC_INACCURACY"), PanicInaccuracy);
	F(TEXT("SPREAD_1"), Spread);                 // from the enemy weapon ITM (the level actor adds these two)
	F(TEXT("EFFRANGE_1"), Range);
	F(TEXT("MAX_STEP"), MaxStep);
	F(TEXT("MAX_DROP"), MaxDrop);
	F(TEXT("DAMAGE"), Damage);
	I(TEXT("SHOT_MULTIPLES"), ShotMultiples);
	F(TEXT("THROW_MAX"), ThrowMax);
	F(TEXT("THROW_MIN"), ThrowMin);
	I(TEXT("WATCH_CHOR"), WatchChor);
	I(TEXT("WALK_CHOR"), WalkChor);
	I(TEXT("RUN_CHOR"), RunChor);
	I(TEXT("DUCK_CHOR"), DuckChor);
	I(TEXT("SHOOT_CHOR"), ShootChor);
	I(TEXT("HIT_CHOR"), HitChor);
	I(TEXT("CHEST_DIE_CHOR"), ChestDieChor);
	I(TEXT("LEG_DIE_CHOR"), LegDieChor);
	I(TEXT("WALK_SHOOT_CHOR"), WalkShootChor);
	I(TEXT("RUN_SHOOT_CHOR"), RunShootChor);
	S(TEXT("SHOOT_SOUND"), ShootSound);
	S(TEXT("HIT_SOUND"), HitSound);
	S(TEXT("DIE_SOUND"), DieSound);
	if (const FString* T = Data.Find(TEXT("DROP_ITEM")))
	{
		DropItem = FPaths::GetBaseFilename(*T).ToLower();
		if (DropItem == TEXT("null")) DropItem.Reset();
	}
	if (const FString* T = Data.Find(TEXT("LOS_TAUNTS"))) LosTaunts = SoundList(*T);
	if (const FString* T = Data.Find(TEXT("NO_LOS_TAUNTS"))) NoLosTaunts = SoundList(*T);
	if (const FString* T = Data.Find(TEXT("ALERT_TAUNTS"))) AlertTaunts = SoundList(*T);
	ShotMultiples = FMath::Max(1, ShotMultiples);
}

void FOutlawsEnemyDef::ScaleForDifficulty(EOutlawsDifficulty Difficulty)
{
	// [EXE 0x1400901fc..0x1400903c2]: Good / Bad (and anything else) / Ugly
	float Speed = 1.f, Timer = 2.f, Hp = 0.75f, Conf = 1.f, Dmg = 2.5f, Inacc = 0.75f, Aggr = 0.75f;
	if (Difficulty == EOutlawsDifficulty::Good)
	{
		Speed = 0.8f; Timer = 2.f; Hp = 1.f; Conf = 0.3f; Dmg = 0.8f; Inacc = 2.f; Aggr = 0.3f;
	}
	else if (Difficulty == EOutlawsDifficulty::Ugly)
	{
		Speed = 1.4f; Timer = 1.5f; Hp = 1.f; Conf = 1.6f; Dmg = 4.f; Inacc = 1.f; Aggr = 1.3f;
	}
	WalkSpeed *= Speed;
	RunSpeed *= Speed;
	ShootTimer *= Timer;
	Health *= Hp;
	Confidence *= Conf;
	Damage *= Dmg;
	MoveInaccuracy *= Inacc;
	WeaponInaccuracy *= Inacc;
	Aggressiveness = FMath::Clamp(static_cast<int32>(Aggressiveness * Aggr), 0, 8);
}

void AOutlawsEnemy::Rescale(EOutlawsDifficulty To)
{
	const float Share = Def.Health > 0.f ? Health / Def.Health : 1.f;
	const float WasWalk = Def.WalkSpeed;
	Def.WalkSpeed = Unscaled.WalkSpeed;
	Def.RunSpeed = Unscaled.RunSpeed;
	Def.ShootTimer = Unscaled.ShootTimer;
	Def.Health = Unscaled.Health;
	Def.Confidence = Unscaled.Confidence;
	Def.Damage = Unscaled.Damage;
	Def.MoveInaccuracy = Unscaled.MoveInaccuracy;
	Def.WeaponInaccuracy = Unscaled.WeaponInaccuracy;
	Def.Aggressiveness = Unscaled.Aggressiveness;
	Def.ScaleForDifficulty(To);
	if (!IsDead()) Health = FMath::Max(0.01f, Share * Def.Health);       // (hurt as much as it was)
	// (the step it is taking now too: the walk's cap is otherwise set only at its next move)
	if (WasWalk > 0.f) GetCharacterMovement()->MaxWalkSpeed *= Def.WalkSpeed / WasWalk;
}

void UOutlawsEnemyMovement::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	OL_PERF_SCOPE("Enemies: movement (CMC)");
	CountMove();
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
}

AOutlawsEnemy::AOutlawsEnemy(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer.SetDefaultSubobjectClass<UOutlawsEnemyMovement>(ACharacter::CharacterMovementComponentName))
{
	PrimaryActorTick.bCanEverTick = true;
	AutoPossessAI = EAutoPossessAI::Disabled;
	AIControllerClass = nullptr;
	bUseControllerRotationYaw = false;
	UCapsuleComponent* C = GetCapsuleComponent();
	C->InitCapsuleSize(30.f, 90.f);
	// a Pawn the player's bullets (Visibility traces) can hit; cameras pass through
	C->SetCollisionObjectType(ECC_Pawn);
	C->SetCollisionResponseToAllChannels(ECR_Block);
	C->SetCollisionResponseToChannel(ECC_Camera, ECR_Ignore);
	if (USkeletalMeshComponent* M = GetMesh())
	{
		M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		M->SetVisibility(false);
		// drawn as sprites: the empty skeletal mesh was still ticked every frame (98 a frame on the Quest)
		M->PrimaryComponentTick.bCanEverTick = false;
		M->SetComponentTickEnabled(false);
	}
	UCharacterMovementComponent* Move = GetCharacterMovement();
	Move->bRunPhysicsWithNoController = true;    // moved by its own Tick, no AI controller
	Move->bOrientRotationToMovement = false;
	Move->MaxStepHeight = 90.f;
	Move->MaxWalkSpeed = 240.f;
	Move->MaxAcceleration = 1500.f;
	Move->BrakingDecelerationWalking = 1600.f;
	Move->bCanWalkOffLedges = true;
	Move->SetWalkableFloorAngle(75.f);           // the Outlaws slopes the player walks too (all of them, up to 72 deg)
}

void AOutlawsEnemy::Init(AOutlawsLevelActor* InLevel, const FString& InName, const TMap<FString, FString>& ItmData, AOutlawsSpriteActor* InSprite, float Yaw)
{
	Level = InLevel;
	OutlawsName = InName;
	ApplyLossless();
	Def.Read(ItmData);
	Unscaled = { Def.WalkSpeed, Def.RunSpeed, Def.ShootTimer, Def.Health, Def.Confidence, Def.Damage, Def.MoveInaccuracy, Def.WeaponInaccuracy, Def.Aggressiveness };
	Def.ScaleForDifficulty(InLevel ? InLevel->Difficulty : EOutlawsDifficulty::Good);
	Health = Def.Health;
	Mode = Def.BaseMode;
	Heading = Yaw;
	PrevShotYaw = Yaw;
	StandHalf = FMath::Max(40.f, Def.Height * float(UnitCm) * 0.5f);
	const float Rad = FMath::Clamp(Def.Radius * float(UnitCm), 22.f, 60.f);
	GetCapsuleComponent()->SetCapsuleSize(Rad, StandHalf);
	// the chickens and tarantulas (HEIGHT 3 and 2; people stand 6) are walked through as if not there, and still shot:
	// they pass pawns, the bullets' Visibility traces still stop on them (user 2026-09-29)
	if (Def.Height < 4.5f) GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
	UCharacterMovementComponent* Move = GetCharacterMovement();
	Move->MaxStepHeight = FMath::Clamp(Def.MaxStep * float(UnitCm), 30.f, 120.f);
	Move->MaxWalkSpeed = Def.WalkSpeed * UnitCm;
	Sprite = InSprite;
	if (Sprite)
	{
		// the sprite stands on the capsule's bottom and shows the heading
		Sprite->SetActorLocation(GetActorLocation() - FVector(0.0, 0.0, StandHalf - 0.5));
		Sprite->AttachToComponent(GetCapsuleComponent(), FAttachmentTransformRules::KeepWorldTransform);
		Sprite->OutlawsYaw = Heading;
	}
	LosWait = FMath::FRandRange(0.f, 0.5f);         // sight checks spread over frames
	TauntWait = 2.f * (Def.TauntTimer + FMath::FRandRange(-4.096f, 4.095f));      // (BUG-TAUNTS: twice the original's)
	SetState(EOutlawsEnemyState::Watch);
}

void AOutlawsEnemy::Destroyed()
{
	if (Sprite)
	{
		Sprite->Destroy();
		Sprite = nullptr;
	}
	Super::Destroyed();
}

double AOutlawsEnemy::Now() const
{
	const UWorld* World = GetWorld();
	return World ? World->GetTimeSeconds() : 0.0;
}

EOutlawsEnemyState AOutlawsEnemy::GetState() const
{
	if (State == EOutlawsEnemyState::Chase && (Sub == SubShoot || Sub == SubShootMove))
	{
		return EOutlawsEnemyState::Shoot;
	}
	return State;
}

void AOutlawsEnemy::SetBodyHalf(float NewHalf)
{
	UCapsuleComponent* C = GetCapsuleComponent();
	const float Cur = C->GetUnscaledCapsuleHalfHeight();
	NewHalf = FMath::Max(NewHalf, C->GetUnscaledCapsuleRadius() + 1.f);
	if (FMath::Abs(NewHalf - Cur) < 1.f)
	{
		return;
	}
	// the feet stay where they are; the sprite stays on them
	C->SetCapsuleHalfHeight(NewHalf);
	AddActorWorldOffset(FVector(0.0, 0.0, NewHalf - Cur));
	if (Sprite && Sprite->GetRootComponent())
	{
		Sprite->GetRootComponent()->SetRelativeLocation(FVector(0.0, 0.0, -(NewHalf - 0.5)));
	}
}

void AOutlawsEnemy::Play(int32 Chor, bool bLoop)
{
	if (!Sprite)
	{
		return;
	}
	if (bHeldForCutscene && !bHoldLetGo)
	{
		// (BUG-DRDEATHANIM) dead into the level's cutscene: he stays as he stood; a body's landing, a co-op snapshot's
		// "played again" or any other late choreography does not start his fall (only FallbackFromHold lets go)
		if (HeldPlaysRefused++ == 0) UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s held for the cutscene: chor %d not played"), *OutlawsName, Chor);
		return;
	}
	if (CoopRole == 1) ++CoopPlays;               // (co-op: the client's copy starts it too)
	if (Chor >= 0 && (Chor == Def.ChestDieChor || Chor == Def.LegDieChor)) ++DieChorsPlayed;
	bHeldForCutscene = false;                     // (a held frame lets go with any new choreography)
	Sprite->PlayChor(Chor, bLoop);
	// the body is as tall as the frame on show [EXE 0x140088aad]: a crouch art frame makes it short,
	// a stand-in duck frame (the families without crouch art) keeps it standing
	if (State != EOutlawsEnemyState::Dead && Sprite->Chors.IsValidIndex(Chor))
	{
		const float Cm = Sprite->Chors[Chor].HeightPx / 16.f * float(UnitCm);
		SetBodyHalf(Cm > 0.f && Cm < StandHalf * 2.f * 0.8f ? FMath::Max(35.f, Cm * 0.5f) : StandHalf);
	}
}

bool AOutlawsEnemy::IsDucking() const
{
	// damage is halved while the duck choreography plays [EXE 0x140088150]
	return Sprite && Def.DuckChor >= 0 && Sprite->GetPlayingChor() == Def.DuckChor;
}

bool AOutlawsEnemy::DuckForProbe(float CrouchHalf)
{
	if (IsDead()) return false;
	const bool bChor = Sprite && Def.DuckChor >= 0 && Sprite->Chors.IsValidIndex(Def.DuckChor);
	if (bChor) Play(Def.DuckChor, true);
	if (CrouchHalf > 0.f)
	{
		SetBodyHalf(CrouchHalf);       // (a bot's own picture is hidden: its puppet goes by the body's height)
		return true;
	}
	return bChor && IsDucking();
}

void AOutlawsEnemy::StandForProbe()
{
	if (!IsDead()) SetBodyHalf(StandHalf);
}

void AOutlawsEnemy::SetState(EOutlawsEnemyState NewState)
{
	State = NewState;
	StateTime = 0.f;
	switch (NewState)
	{
	case EOutlawsEnemyState::Watch:
		ConfidenceMeter = 7.f;                  // watch entry [EXE 0x14008ac63]
		Play(Def.WatchChor, true);
		break;
	case EOutlawsEnemyState::Hit:
		Play(Def.HitChor, false);
		break;
	default:
		break;
	}
}

void AOutlawsEnemy::SetSub(int32 NewSub)
{
	if (bStationary && IsMoveSub(NewSub)) NewSub = SubFace;      // holds its ground: every move is a stand and face
	Sub = NewSub;
	SubTime = 0.f;
	StuckTime = 0.f;
	bDoorTried = false;
	switch (NewSub)
	{
	case SubFace:
	case SubDecide:
		Play(Def.WatchChor, true);
		break;
	case SubAdvance:
	case SubRetreat:
	case SubCover:
		Play(bRunning ? Def.RunChor : Def.WalkChor, true);
		GetCharacterMovement()->MaxWalkSpeed = (bRunning ? Def.RunSpeed : Def.WalkSpeed) * UnitCm;
		break;
	case SubDuck:
		// [EXE 0x14008c6a3]: 7.9 s + up to 8.2 s, a second less per point of aggressiveness; the
		// ambush modes stay down until their condition
		DuckLeft = 7.904f + FMath::FRandRange(0.f, 8.191f) - Def.Aggressiveness;
		GetCharacterMovement()->StopMovementImmediately();
		Play(Def.DuckChor, false);
		break;
	default:
		break;
	}
}

bool AOutlawsEnemy::PlayerView(FVector& OutEye, FVector& OutFeet, float& OutRadius, float& OutHalf) const
{
	if (CoopTarget != 0) return CoopTargetView(OutEye, OutFeet, OutRadius, OutHalf);     // (co-op host: after the other player)
	const UWorld* World = GetWorld();
	const APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	if (!Pawn)
	{
		return false;
	}
	float R = 34.f, H = 87.f;
	Pawn->GetSimpleCollisionCylinder(R, H);
	OutRadius = R;
	OutHalf = H;
	OutFeet = Pawn->GetActorLocation() - FVector(0.0, 0.0, H);
	OutEye = PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : Pawn->GetActorLocation() + FVector(0.0, 0.0, H * 0.8);
	return true;
}

float AOutlawsEnemy::HeightCm() const
{
	return GetCapsuleComponent()->GetScaledCapsuleHalfHeight() * 2.f;
}

FVector AOutlawsEnemy::EyePoint() const
{
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	return GetActorLocation() + FVector(0.0, 0.0, Half * 0.8);
}

FString AOutlawsEnemy::DebugBossState() const
{
	static const TCHAR* States[] = { TEXT("watch"), TEXT("chase"), TEXT("shoot"), TEXT("hit"), TEXT("dead") };
	const int32 St = int32(State);
	return FString::Printf(TEXT("state %s sub %d peek %d health %.1f sees %d shot damage %.1f x%d behind door %d shots %d hidden %d"), St >= 0 && St < 5 ? States[St] : TEXT("?"), Sub, PeekStage, Health, bSeen ? 1 : 0,
		Def.Damage, Def.ShotMultiples, bBehindDoor ? 1 : 0, ShotsTotal, IsHidden() ? 1 : 0);
}

FString AOutlawsEnemy::DebugSight() const
{
	FVector Eye, Feet;
	float R, H;
	const AOutlawsLevelActor* L = Level.Get();
	if (!L || !PlayerView(Eye, Feet, R, H)) return TEXT("no player view");
	const FVector MyEye = EyePoint();
	const double D = OctDist(MyEye, Eye);
	const float Off = FMath::Abs(FRotator::NormalizeAxis((Eye - MyEye).Rotation().Yaw - Heading));
	const int32 TargetLight = L->LightAt(Feet + FVector(0.0, 0.0, 10.0));
	const int32 MyLight = L->LightAt(GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() - 10.0));
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsEnemySightDebug), true, this);
	FHitResult Hit;
	const bool bBlocked = GetWorld()->LineTraceSingleByChannel(Hit, MyEye, Eye, ECC_Visibility, Q, WorldOnly());
	return FString::Printf(TEXT("dist %.0f (fov dist %.0f), off %.0f deg (fov %.0f, heading %.0f), light me %d them %d, trace %s"), D, Def.FovDist, Off, Def.Fov, Heading,
		MyLight, TargetLight, bBlocked ? *FString::Printf(TEXT("BLOCKED by %s at %.0f cm"), Hit.GetComponent() ? *Hit.GetComponent()->GetName() : TEXT("?"), Hit.Distance) : TEXT("clear"));
}

bool AOutlawsEnemy::CanSeeFrom(const FVector& From, const FVector& To) const
{
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsEnemySight), true, this);
	FHitResult Hit;
	return !GetWorld()->LineTraceSingleByChannel(Hit, From, To, ECC_Visibility, Q, WorldOnly());
}

bool AOutlawsEnemy::SightCheck() const
{
	// [EXE 0x140088b80]: inside FOV_DIST; inside the FOV half-angle unless seen in the last 10 s (a
	// fresh look narrows toward the cone's edge); the range shrinks for a short (crouching) target and
	// a dark one, and halves when this bad guy stands in the brighter place; then line of sight
	FVector Eye, Feet;
	float R, H;
	const AOutlawsLevelActor* L = Level.Get();
	if (!L || !PlayerView(Eye, Feet, R, H))
	{
		return false;
	}
	const FVector MyEye = EyePoint();
	const double D = OctDist(MyEye, Eye);
	if (D >= Def.FovDist)
	{
		return false;
	}
	const bool bRecent = Now() - LastSeenTime < 10.0;
	const float Off = FMath::Abs(FRotator::NormalizeAxis((Eye - MyEye).Rotation().Yaw - Heading));
	double Range = Def.FovDist;
	if (!bRecent)
	{
		if (Off >= Def.Fov)
		{
			return false;
		}
		Range *= (Def.Fov - Off) / Def.Fov;
	}
	// (BUG-BOSSPASSIVE) a hunting named outlaw who has had the player in his sights in the last 10 s keeps them over the
	// whole FOV_DIST: in the canyon's shade (light 7) the dark cut his sight to 22 m, so 20 m off in plain view he lost the
	// player, walked at them and did not shoot
	if (bRecent && IsFightingBoss())
	{
		const FVector Chest0 = Feet + FVector(0.0, 0.0, FMath::Max(40.0, (Eye.Z - Feet.Z) * 0.6));
		return CanSeeFrom(MyEye, Eye) || CanSeeFrom(MyEye, Chest0);
	}
	const double TargetH = (Eye.Z - Feet.Z + 15.0) / UnitCm;
	Range *= FMath::Min(1.2, TargetH / 6.5);
	const int32 TargetLight = L->LightAt(Feet + FVector(0.0, 0.0, 10.0));
	const int32 MyLight = L->LightAt(GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() - 10.0));
	double Lit = TargetLight / 23.0;
	if (TargetLight < 10) Lit *= 0.8;
	if (MyLight >= TargetLight + 5) Lit *= 0.5;
	if (L->bLampOn) Lit = 1.0;                // a lit lantern gives the player away in the dark (INFERRED)
	Range *= FMath::Clamp(Lit, 0.03, 1.0);
	if (D >= Range)
	{
		return false;
	}
	const FVector Chest = Feet + FVector(0.0, 0.0, FMath::Max(40.0, (Eye.Z - Feet.Z) * 0.6));
	return CanSeeFrom(MyEye, Eye) || CanSeeFrom(MyEye, Chest);
}

void AOutlawsEnemy::LookForPlayer()
{
	if (CoopRole == 1) CoopChooseTarget();        // (co-op host: the nearer or the seen of the two players)
	FVector Eye, Feet;
	float R, H;
	const AOutlawsLevelActor* L = Level.Get();
	if (!L || TargetDead(L) || !PlayerView(Eye, Feet, R, H))
	{
		bSeen = false;
		bLegsHidden = true;
		LosWait = 1.f;
		return;
	}
	bSeen = SightCheck();
	const double D = OctDist(GetActorLocation(), Feet);
	if (bSeen)
	{
		const double T = Now();
		const float YawTo = (Feet - GetActorLocation()).Rotation().Yaw;
		// sighted again after a while: confidence drops by how long it was lost, more if it had to
		// turn far to find the player again [EXE 0x14008a975..0x14008aab6]
		const double Since = T - LastSeenTime;
		if (State != EOutlawsEnemyState::Watch && Since > 3.0)
		{
			const float K = Since > 20.0 ? 4.f : Since > 10.0 ? 3.f : Since > 5.0 ? 2.f : 1.f;
			const float Turn = FMath::Min(180.f, FMath::Abs(FRotator::NormalizeAxis(YawTo - Heading))) / 180.f;
			ConfidenceMeter = FMath::Clamp(ConfidenceMeter - K * (1.f + Turn), 0.f, 7.f);
		}
		// acquiring after 1.5 s without: the next shot comes within a second [EXE 0x140088960]
		if (T - LastAcquireTime > 1.5)
		{
			ShootWait = FMath::Max(0.f, FMath::FRandRange(0.f, 1.023f) - 0.012f);
		}
		LastAcquireTime = T;
		TargetPos = Feet;
		LastSeenTime = T;
		// "legs hidden": no line to the player's knees (it ducks when you have cover too)
		bLegsHidden = !CanSeeFrom(EyePoint(), Feet + FVector(0.0, 0.0, (Eye.Z - Feet.Z) * 0.25));
	}
	else
	{
		bLegsHidden = true;
	}
	// rechecked every LOS_TIMER_TIME per 25 units of distance, at most every 5 s [EXE 0x14008a915]
	LosWait = FMath::Min(5.f, Def.LosTimer * FMath::Max(1.f, FMath::FloorToFloat(D / 25.0)));
}

void AOutlawsEnemy::Alert(bool bBroadcast)
{
	WakeTick();
	if (State == EOutlawsEnemyState::Dead || bBehindDoor)
	{
		return;
	}
	const bool bWasWatching = State == EOutlawsEnemyState::Watch;
	State = EOutlawsEnemyState::Chase;
	StateTime = 0.f;
	// straight into the combat hook; only the first shot waits a moment [EXE 0x14008acca]
	if (bWasWatching)
	{
		ShootWait = FMath::FRandRange(0.f, 1.523f);
		UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s alerted (mode %d, aggressiveness %d)"), *OutlawsName, Mode, Def.Aggressiveness);
		if (CoopRole == 1) CoopChooseTarget(true);    // (co-op host: which player, said in the log)
	}
	SetSub(SubFace);
	// "share target" to the bad guys within 50 units
	if (bBroadcast)
	{
		if (AOutlawsLevelActor* L = Level.Get())
		{
			L->ShareTargetNear(this, GetActorLocation(), ShareRadiusCm);
		}
	}
}

void AOutlawsEnemy::Wake(bool bSpread)
{
	if (CoopRole == 2) return;                    // (co-op: the client's copy follows the host's)
	if (CoopRole == 1) CoopChooseTarget();        // (BUG-BOSSIDLEHOST: the player it goes for chosen first, so it heads there)
	FVector Eye, Feet;
	float R, H;
	if (PlayerView(Eye, Feet, R, H))
	{
		TargetPos = Feet;
		LastSeenTime = LastAcquireTime = Now();
	}
	Alert(bSpread);
	LookForPlayer();
}

void AOutlawsEnemy::ShareTarget()
{
	if (bDormant || CoopRole == 2) return;
	// only a normal (mode 0) watcher takes a neighbour's target [EXE 0x140089d5e]
	if (State == EOutlawsEnemyState::Watch && Mode == 0)
	{
		Wake(false);
	}
}

void AOutlawsEnemy::AdjustMeters(float Confidence, float Panic)
{
	ConfidenceMeter = FMath::Clamp(ConfidenceMeter + Confidence, 0.f, 7.f);
	PanicMeter = FMath::Clamp(PanicMeter + Panic, 0.f, 7.f);
}

void AOutlawsEnemy::HearShot(const FVector& Where)
{
	if (bDormant || CoopRole == 2) return;
	// [EXE 0x140089dd5]: only a watcher reacts, to a shot it can see the shooter from, or now and
	// then without (26 / 256; 14 / 256 on Ugly); it alerts, tells its neighbours, loses some nerve,
	// and looks again in 2 s
	if (State != EOutlawsEnemyState::Watch)
	{
		return;
	}
	FVector Eye, Feet;
	float R, H;
	const AOutlawsLevelActor* L = Level.Get();
	const int32 Chance = L && L->Difficulty == EOutlawsDifficulty::Ugly ? 14 : 26;
	if (PlayerView(Eye, Feet, R, H) && (CanSeeFrom(EyePoint(), Eye) || FMath::RandHelper(256) < Chance))
	{
		Wake(true);
		AdjustMeters(-1.f, 4.f);
		LosWait = 2.f;
	}
}

int32 AOutlawsEnemy::CrowdAround(const FVector& P, double Radius, bool bTheirGoals, FVector& OutCentre) const
{
	OutCentre = FVector::ZeroVector;
	const AOutlawsLevelActor* L = Level.Get();
	if (!L) return 0;
	int32 N = 0;
	for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
	{
		const AOutlawsEnemy* E = Wk.Get();
		if (!E || E == this || E->IsDead() || E->IsDormant() || E->bHeldByPlayer || E->bThrown) continue;
		const FVector At = E->GetActorLocation();
		const bool bThere = FVector::DistSquared2D(At, P) < Radius * Radius && FMath::Abs(At.Z - P.Z) < 200.0;
		const bool bGoing = bTheirGoals && IsMoveSub(E->Sub) && FVector::DistSquared2D(E->MoveTarget, P) < Radius * Radius;
		if (bThere || bGoing)
		{
			OutCentre += At;
			++N;
		}
	}
	if (N > 0) OutCentre /= N;
	return N;
}

void AOutlawsEnemy::Decide()
{
	// (BUG-BOSSPASSIVE, user 2026-10-15: the canyon's Dick Farmer "would just run around aimlessly") a hunting named outlaw
	// with the player in plain view stands and fights inside his gun's reach, and only closes in from beyond it; the matrix's
	// retreats and cover (his low confidence and aggressiveness on Good) walked him about in view and off out of it
	if (IsFightingBoss() && bSeen)
	{
		if (OctDist(GetActorLocation(), TargetPos) <= Def.Range)
		{
			SetSub(SubFace);
			return;
		}
		bAdvanceNext = true;
		if (DecideMove(true)) return;
		bAdvanceNext = true;
		if (DecideMove(false)) return;
		SetSub(SubFace);
		return;
	}
	// a place clear of the others first (user 2026-10-01: following the player they all bunched in one corner of a room);
	// the original's choice anywhere when there is none
	if (DecideMove(true) || DecideMove(false)) return;
	SetSub(SubFace);   // nowhere to go: face the player and think again
}

bool AOutlawsEnemy::DecideMove(bool bAvoidCrowd)
{
	// [EXE 0x14008c001]: row = confidence, column = aggressiveness
	const int32 Row = static_cast<int32>(ConfidenceMeter) & 7;
	const int32 Col = Def.Aggressiveness & 7;
	const uint8 Choice = bAdvanceNext ? 2 : GDecide[Row][Col];     // (a friend grabbed: come on, backlog 1a)
	bAdvanceNext = false;
	const FVector Here = GetActorLocation();
	const float ToTarget = (TargetPos - Here).Rotation().Yaw;
	const bool bNear = OctDist(Here, TargetPos) <= 50.0;
	FVector P;
	auto Go = [this, &Here](const FVector& Where, int32 NewSub)
	{
		MoveTarget = Where;
		bRunning = FVector::Dist2D(Where, Here) > 10.0 * UnitCm;   // run when it is more than 10 units off
		SetSub(NewSub);
	};
	if (FlankYawNext != 0.f)
	{
		// the plate held up to it (backlog 1c): round the side instead of straight on, 50 to 70 degrees off
		const float Off = FlankYawNext;
		FlankYawNext = 0.f;
		for (int32 i = 0; i < 8; ++i)
		{
			if (PickPoint(5.f, 15.f, ToTarget + Off, 10.f + 10.f * i, 1, false, P, bAvoidCrowd)) { Go(P, SubAdvance); return true; }
		}
	}
	if (Choice == 2)
	{
		// advance: 5..15 units toward the player, widening the search 15 degrees a try
		for (int32 i = 0; i < 10; ++i)
		{
			if (PickPoint(5.f, 15.f, ToTarget, 20.f + 15.f * i, 1, false, P, bAvoidCrowd)) { Go(P, SubAdvance); return true; }
		}
	}
	else if (Choice == 4)
	{
		// retreat: 5..25 units straight away, unless the player is already far off
		for (int32 i = 0; bNear && i < 10; ++i)
		{
			if (PickPoint(5.f, 25.f, ToTarget + 180.f, 20.f + 15.f * i, 1, false, P, bAvoidCrowd)) { Go(P, SubRetreat); return true; }
		}
	}
	else
	{
		// take cover: up to 36 reachable places 5..25 units off that the player cannot see; failing
		// that, a retreat (which again only happens with the player within 50 units)
		if (PickPoint(5.f, 25.f, 0.f, 180.f, 36, true, P, bAvoidCrowd)) { Go(P, SubCover); return true; }
		for (int32 i = 0; bNear && i < 6; ++i)
		{
			if (PickPoint(5.f, 25.f, ToTarget + 180.f, 20.f + 15.f * i, 1, false, P, bAvoidCrowd)) { Go(P, SubRetreat); return true; }
		}
	}
	return false;
}

bool AOutlawsEnemy::FloorAt(const FVector& Probe, float Below, FVector& OutFloor) const
{
	// solid ground within reach, and not the bed of a pool (water surfaces have no collision)
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsEnemyFloor), true, this);
	FHitResult Floor;
	if (!GetWorld()->LineTraceSingleByChannel(Floor, Probe, Probe - FVector(0.0, 0.0, Below), ECC_Visibility, Q, WorldOnly()))
	{
		return false;
	}
	float Surface = 0.f;
	const AOutlawsLevelActor* L = Level.Get();
	if (L && L->WaterAt(Floor.ImpactPoint + FVector(0.0, 0.0, 10.0), Surface))
	{
		return false;
	}
	OutFloor = Floor.ImpactPoint;
	return true;
}

bool AOutlawsEnemy::PickPoint(float MinU, float MaxU, float CentreYaw, float SpreadDeg, int32 Tries, bool bNeedCover, FVector& Out, bool bAvoidCrowd) const
{
	UWorld* World = GetWorld();
	const FVector Here = GetActorLocation();
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const float Rad = GetCapsuleComponent()->GetScaledCapsuleRadius();
	FVector Eye, Feet;
	float R, H;
	const bool bPlayer = PlayerView(Eye, Feet, R, H);
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsEnemyPath), true, this);
	for (int32 i = 0; i < Tries; ++i)
	{
		const float Yaw = CentreYaw + FMath::FRandRange(-SpreadDeg, SpreadDeg);
		const FVector Dir = FRotator(0.0, Yaw, 0.0).Vector();
		double Dist = FMath::FRandRange(MinU, MaxU) * UnitCm;
		// the body has to fit the whole way (a path probe with MAX_STEP / MAX_DROP in the original)
		FHitResult Hit;
		const FCollisionShape Body = FCollisionShape::MakeCapsule(Rad * 0.9f, FMath::Max(Half * 0.9f - Def.MaxStep * 10.f, Rad));
		const FVector Start = Here + FVector(0.0, 0.0, Def.MaxStep * 10.0);
		if (World->SweepSingleByChannel(Hit, Start, Start + Dir * Dist, FQuat::Identity, ECC_Pawn, Body, Q, WorldOnly()))
		{
			Dist = Hit.Distance - 10.0;
		}
		if (Dist < MinU * UnitCm * 0.6)
		{
			continue;
		}
		FVector P = Here + Dir * Dist;
		FVector Floor;
		if (FloorAt(P + FVector(0.0, 0.0, Def.MaxStep * UnitCm), Def.MaxStep * UnitCm + Half + Def.MaxDrop * UnitCm, Floor))
		{
			P.Z = Floor.Z + Half;
		}
		else if (bFloating && SwimAt(P))
		{
			P.Z = Here.Z;                // (REQ-ENEMYFLOAT: afloat, a place on the water is somewhere to go)
		}
		else
		{
			continue;
		}
		if (bNeedCover && bPlayer && CanSeeFrom(Eye, P))
		{
			continue;
		}
		// (clear of the others, and of where they are headed: 1.6 m)
		FVector Crowd;
		if (bAvoidCrowd && CrowdAround(P, 160.0, true, Crowd) > 0)
		{
			continue;
		}
		Out = P;
		return true;
	}
	return false;
}

void AOutlawsEnemy::MoveAlong(float Dt)
{
	const FVector Here = GetActorLocation();
	FVector To = MoveTarget - Here;
	To.Z = 0.0;
	if (To.Size() < 50.0 || SubTime > 8.f)
	{
		SetSub(SubFace);
		return;
	}
	FVector Dir = To.GetSafeNormal();
	// a little apart from any of the others within 90 cm on the way (user 2026-10-01: they piled into one corner)
	if (const AOutlawsLevelActor* LS = Level.Get())
	{
		FVector Apart = FVector::ZeroVector;
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : LS->GetEnemies())
		{
			const AOutlawsEnemy* E = Wk.Get();
			if (!E || E == this || E->IsDead() || E->IsDormant() || E->bHeldByPlayer) continue;
			FVector D = Here - E->GetActorLocation();
			D.Z = 0.0;
			const double Dist = D.Size();
			if (Dist < 90.0 && Dist > 1.0) Apart += D / Dist * (1.0 - Dist / 90.0);
		}
		if (!Apart.IsNearlyZero()) Dir = (Dir + Apart * 0.8).GetSafeNormal2D();
		if (Dir.IsNearlyZero()) Dir = To.GetSafeNormal();
	}
	// never walk off a drop deeper than MAX_DROP, nor into water
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const float Rad = GetCapsuleComponent()->GetScaledCapsuleRadius();
	FVector Floor;
	// (REQ-ENEMYFLOAT) afloat, water ahead is somewhere to wade
	const bool bFloor = FloorAt(Here + Dir * (Rad + 30.0), Half + Def.MaxDrop * UnitCm, Floor) || (bFloating && SwimAt(Here + Dir * (Rad + 30.0)));
	if (bFloor)
	{
		AddMovementInput(Dir, 1.f);
	}
	FaceYaw(GetVelocity().Size2D() > 20.f ? GetVelocity().Rotation().Yaw : Dir.Rotation().Yaw, Dt);
	const bool bSlow = GetVelocity().Size2D() < GetCharacterMovement()->MaxWalkSpeed * 0.25f;
	StuckTime = (bSlow || !bFloor) && SubTime > 0.3f ? StuckTime + Dt : 0.f;
	if (StuckTime > 0.35f && !bDoorTried)
	{
		// a door in the way opens for it (Jedi smart object) unless locked; one try per move
		bDoorTried = true;
		AOutlawsLevelActor* L = Level.Get();
		const FVector MyEye = EyePoint();
		if (L && L->EnemyOpenDoor(MyEye, MyEye + Dir * (Rad + 5.0)))
		{
			StuckTime = 0.f;
			SubTime = FMath::Min(SubTime, 5.f);
			return;
		}
	}
	if (StuckTime > 1.2f)
	{
		SetSub(SubFace);
	}
}

void AOutlawsEnemy::BeginShotWindup()
{
	// each shoot choreography starts facing the target: the aim error measures how far the target
	// moves during the wind-up [EXE 0x140089850]
	const float YawTo = (TargetPos - GetActorLocation()).Rotation().Yaw;
	Heading = YawTo;
	PrevShotYaw = YawTo;
	ShotsFired = 0;
}

void AOutlawsEnemy::StartShooting(int32 FromSub)
{
	// bursts: 1..4 shots normally (the exe's MIN rolls twice), 1 for a sniper, none while an
	// ambusher waits [EXE 0x14008c4e8..0x14008c53c]
	if (Mode == 0)
	{
		const int32 First = FMath::RandRange(1, 4);
		BurstLeft = First == 4 ? 3 : FMath::RandRange(1, 4);
	}
	else
	{
		BurstLeft = Mode == 1 ? 1 : 0;
	}
	if (BurstLeft <= 0 || Def.ShootChor < 0)
	{
		return;
	}
	// Bob behind his curtain peeks out "a few times every 10 or 15 seconds" (user 2026-10-03, the original as remembered)
	if (bPeekShooter && LastPeekAt >= 0.0 && Now() - LastPeekAt < NextPeekGap)
	{
		return;
	}
	ResumeSub = IsMoveSub(FromSub) ? FromSub : SubFace;
	GetCharacterMovement()->StopMovementImmediately();
	Sub = SubShoot;
	SubTime = 0.f;
	if (bPeekShooter && Sprite && Sprite->Chors.IsValidIndex(35))
	{
		UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s peeks out from the curtain (%s)"), *OutlawsName,
			LastPeekAt < 0.0 ? TEXT("the first") : *FString::Printf(TEXT("%.1f s since the last"), Now() - LastPeekAt));
		LastPeekAt = Now();
		// (user 2026-10-03, BUG-BOBSHOOT: "constantly taking shots at me. Sometimes one, sometimes three, in short bursts and
		// clusters. At random times") out again after 1.5 to 4.5 s, now and then at once; one shot, two or three
		NextPeekGap = FMath::RandRange(0, 3) == 0 ? FMath::FRandRange(0.4f, 1.f) : FMath::FRandRange(1.5f, 4.5f);
		PeekStage = 0;                 // out from the curtain first; the shots come in the next chor
		BurstLeft = FMath::RandRange(0, 2) == 0 ? 1 : FMath::RandRange(2, 3);
		Play(33, false);
		return;
	}
	Play(Def.ShootChor, false);
	BeginShotWindup();
}

void AOutlawsEnemy::StartShootMove()
{
	ResumeSub = Sub;
	Sub = SubShootMove;
	SubTime = 0.f;
	Play(bRunning && Def.RunShootChor >= 0 ? Def.RunShootChor : Def.WalkShootChor, false);
	BeginShotWindup();
}

void AOutlawsEnemy::FireAtPlayer()
{
	AOutlawsLevelActor* L = Level.Get();
	FVector Eye, Feet;
	float R, H;
	if (!L || !PlayerView(Eye, Feet, R, H) || !L->TryEnemyShot())
	{
		return;
	}
	++ShotsTotal;
	// a man the player holds between them (backlog 1a, user 2026-10-01: hesitate about 1.5 s, then shoot; the held man
	// takes what hits him): the line from the eye to the player's chest through a held body
	{
		const FVector From = EyePoint(), To = FVector(Feet.X, Feet.Y, Feet.Z + (Eye.Z - Feet.Z) * 0.75);
		bool bHostage = false;
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
		{
			const AOutlawsEnemy* S = Wk.Get();
			if (!S || S == this || !S->bHeldByPlayer) continue;
			const UCapsuleComponent* SC = S->GetCapsuleComponent();
			const FVector C = S->GetActorLocation(), Hh(0.0, 0.0, SC->GetScaledCapsuleHalfHeight());
			FVector OnS, OnB;
			FMath::SegmentDistToSegmentSafe(From, To, C - Hh, C + Hh, OnS, OnB);
			if (FVector::Dist(OnS, OnB) < SC->GetScaledCapsuleRadius()) { bHostage = true; break; }
		}
		if (bHostage)
		{
			const double T = GetWorld()->GetTimeSeconds();
			if (HostageSince < 0.0 || T - HostageLast > 2.0)
			{
				HostageSince = T;
				UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s holds fire: a friend in the way"), *OutlawsName);
			}
			HostageLast = T;
			if (T - HostageSince < 1.5) return;
		}
	}
	// [EXE 0x140088e50]: from 3/4 of its height; yaw at where it last saw the player, pitch at the
	// live player at 3/4 of their height; errors from turning (MOVE_INACCURACY), the weapon, and panic
	const float MyH = HeightCm();
	const FVector Gun = GetActorLocation() - FVector(0.0, 0.0, MyH * 0.5) + FVector(0.0, 0.0, MyH * 0.75)
		+ FRotator(0.0, Heading, 0.0).Vector() * GetCapsuleComponent()->GetScaledCapsuleRadius();
	const FVector Aim = FVector(TargetPos.X, TargetPos.Y, Feet.Z + (Eye.Z - Feet.Z + 15.0) * 0.75);
	const FRotator Base = (Aim - Gun).Rotation();
	const float Turn = FMath::Min(45.f, FMath::Abs(FRotator::NormalizeAxis(Base.Yaw - PrevShotYaw)));
	const float E = Turn / 45.f * Def.MoveInaccuracy;
	const float PanicErr = FMath::Clamp(PanicMeter / 7.f, 0.f, 1.f) * Def.PanicInaccuracy;
	FRotator Shot = Base;
	Shot.Yaw += FMath::FRandRange(-E, E) + FMath::FRandRange(-Def.WeaponInaccuracy, Def.WeaponInaccuracy) + FMath::FRandRange(-PanicErr, PanicErr);
	Shot.Pitch += FMath::FRandRange(-Def.WeaponInaccuracy, Def.WeaponInaccuracy) + FMath::FRandRange(-PanicErr, PanicErr);
	Heading = Base.Yaw;
	L->PlaySoundAt(Def.ShootSound, Gun);
	L->EchoOf(Def.ShootSound, Gun);          // (R13: in the open, a slap off a wall behind)
	if (AOutlawsLevelActor::IsGunSound(Def.ShootSound)) L->MuzzleFlash(Gun, 0.8f);   // guns only: no flash for the tarantulas (user 2026-09-27)
	if (CoopRole == 1) ++CoopShots;               // (co-op: the client's copy plays the shot too)
	if (CoopSelf >= 0)
	{
		if (UOutlawsCoop* C = Coop.Get()) C->BotFired(this, Gun);     // (REQ-MPBOTS: a bot's shot, seen on every machine)
	}
	const FVector Bottom = Feet + FVector(0.0, 0.0, R);
	const FVector Top = FVector(Feet.X, Feet.Y, FMath::Max(Eye.Z + 10.0, Feet.Z + 2.0 * R));
	const double Range = Def.Range * UnitCm;       // the weapon's EFFRANGE_1: pistol 120 u, shotgun 80 u
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsEnemyShot), true, this);
	int32 Hits = 0;
	for (int32 i = 0; i < Def.ShotMultiples; ++i)
	{
		// the first pellet flies true to the aim, the rest scatter by the weapon's SPREAD_1
		FRotator P = Shot;
		if (i > 0)
		{
			P.Yaw += FMath::FRandRange(-Def.Spread, Def.Spread);
			P.Pitch += FMath::FRandRange(-Def.Spread, Def.Spread);
		}
		FVector End = Gun + P.Vector() * Range;
		FHitResult Hit;
		if (GetWorld()->LineTraceSingleByChannel(Hit, Gun, End, ECC_Visibility, Q, WorldOnly()))
		{
			End = Hit.ImpactPoint;
		}
		// a bad guy the player holds up as a shield takes the pellet (user 2026-09-29)
		bool bShield = false;
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
		{
			AOutlawsEnemy* S = Wk.Get();
			if (!S || S == this || !S->bHeldByPlayer) continue;
			const UCapsuleComponent* SC = S->GetCapsuleComponent();
			const FVector C = S->GetActorLocation(), Hh(0.0, 0.0, SC->GetScaledCapsuleHalfHeight());
			FVector OnS, OnB;
			FMath::SegmentDistToSegmentSafe(Gun, End, C - Hh, C + Hh, OnS, OnB);
			if (FVector::Dist(OnS, OnB) < SC->GetScaledCapsuleRadius())
			{
				End = OnS;
				FOutlawsStatsScope NobodysHit(EOutlawsStatsBy::Nobody);     // (a bad guy's own shot: nobody's kill)
				S->TakeHit(Def.Damage, OnS, P.Vector(), Gun);
				bShield = true;
				break;
			}
		}
		// the boiler plate held up in a hand stops what strikes it (user 2026-10-01); worn through it breaks and the shot
		// goes on with what is left of it
		float ShotDamage = Def.Damage;
		bool bPlated = false;
		if (!bShield && CoopTarget == 0)
		{
			APlayerController* ShotPC = GetWorld()->GetFirstPlayerController();
			if (AOutlawsPlayer* ShotP = ShotPC ? Cast<AOutlawsPlayer>(ShotPC->GetPawn()) : nullptr) bPlated = ShotP->PlateStops(Gun, End, ShotDamage);
		}
		L->ShatterAlong(Gun, End);
		if (bShield || bPlated) continue;
		FVector Splash;
		if (L->WaterSurfaceHit(Gun, End, Splash)) L->SpawnSplash(Splash, false, i == 0);
		// the player's body: closest approach of the pellet to its axis
		FVector OnShot, OnBody;
		FMath::SegmentDistToSegmentSafe(Gun, End, Bottom, Top, OnShot, OnBody);
		const double Miss = FVector::Dist(OnShot, OnBody);
		if (Miss > R)
		{
			continue;
		}
		// Outlaws player hit location: x1.5 in the middle third above the waist, x1 in the middle two
		// thirds, x0.5 at the edges, applied after the plate has taken its share (DamagePlayer)
		const double Across = Miss / R;
		const bool bHigh = OnBody.Z > Feet.Z + (Top.Z - Feet.Z) * 0.5;
		const float Mult = Across < 1.0 / 3.0 ? (bHigh ? 1.5f : 1.f) : Across < 2.0 / 3.0 ? 1.f : 0.5f;
		if (CoopSelf >= 0)
		{
			if (UOutlawsCoop* C = Coop.Get()) C->BotPellet(this, ShotDamage * Mult, OnBody, Gun);     // (REQ-MPBOTS: a match hit, the bot's kill)
		}
		else if (CoopTarget != 0)
		{
			if (UOutlawsCoop* C = Coop.Get()) C->HurtFromEnemy(this, ShotDamage, Mult);     // (co-op: on the client's machine)
		}
		else
		{
			if (UOutlawsCoop* C = CoopRole == 1 ? Coop.Get() : nullptr) C->NotePlayerActive(0, TEXT("hit by a bad guy"));     // (BUG-BOSSIDLEHOST)
			L->SetHurtSource(GetActorLocation());
			L->bHurtByBadGuy = true;
			L->DamagePlayer(ShotDamage, Mult);
		}
		++Hits;
	}
	UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s fires: %d of %d pellets hit (panic %.1f, confidence %.1f)"), *OutlawsName, Hits, Def.ShotMultiples, PanicMeter, ConfidenceMeter);
	if (Hits > 0)
	{
		// "your target was hit": nerve up; the neighbours hear of it too [EXE events 9, 10]
		AdjustMeters(4.f, -3.f);
		L->BroadcastMeters(this, GetActorLocation(), ShareRadiusCm, 2.f, 2.f);
	}
}

void AOutlawsEnemy::SetDormant(bool bOn)
{
	bDormant = bOn;
	SetThrottleTier(0);
	WakeTick();
	SetActorHiddenInGame(bOn);
	GetCapsuleComponent()->SetCollisionEnabled(bOn ? ECollisionEnabled::NoCollision : BodyCollision());
	if (bOn) GetCharacterMovement()->Deactivate(); else GetCharacterMovement()->Activate();
	if (Sprite) Sprite->SetActorHiddenInGame(bOn);
}

void AOutlawsEnemy::Appear(const FVector& FeetLoc)
{
	if (CoopRole == 2) return;                    // (co-op: the client's copy appears when the host's does)
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	SetActorLocation(FeetLoc + FVector(0.0, 0.0, Half + 2.0), false, nullptr, ETeleportType::TeleportPhysics);
	if (Sprite) Sprite->SetActorLocation(FeetLoc);
	SetDormant(false);
	Wake(false);
	ShootWait = FMath::FRandRange(0.f, 1.023f);
	LosWait = 2.f;
	ForcedTaunt(EyePoint());
}

UAudioComponent* AOutlawsEnemy::ForcedTaunt(const FVector& At)
{
	AOutlawsLevelActor* L = Level.Get();
	const TArray<FString>& List = Def.LosTaunts.Num() > 0 ? Def.LosTaunts : Def.NoLosTaunts;
	if (!L || List.Num() == 0 || L->IsTauntPlaying())
	{
		return nullptr;                  // (BUG-TAUNTS: never over another taunt)
	}
	FString Line = List[FMath::RandHelper(List.Num())];
	for (int32 Try = 0; Try < 4 && List.Num() > 1 && Line == LastTaunt; ++Try)
	{
		Line = List[FMath::RandHelper(List.Num())];
	}
	LastTaunt = Line;
	// (no echo for a voice: off a far face it sounded like a second taunt; BUG-TAUNTS)
	UAudioComponent* A = L->PlaySoundAt(L->TauntWav(Line), At);
	L->TauntPlaying = A;
	AOutlawsLevelActor::VaryPitch(A);
	return A;
}

void AOutlawsEnemy::Taunt(const TArray<FString>& List, const TCHAR* Why)
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L || List.Num() == 0) return;
	if (!AOutlawsLevelActor::OldTauntRate())
	{
		L->EventTaunt(this, List, Why);         // (BUG-TAUNTRATE: one clock per listening player, OutlawsLevelActorTaunts.cpp)
		return;
	}
	// -OLOldTauntRate: [EXE 0x14008eeb0]: player within 200 units, one taunt at a time level-wide after a coin flip,
	// and not the same line twice running (up to 4 redraws)
	FVector Eye, Feet;
	float R, H;
	if (!PlayerView(Eye, Feet, R, H) || OctDist(GetActorLocation(), Feet) > 200.0 || !L->TryTaunt(false))
	{
		return;
	}
	FString Line;
	SayLine(List, Line);
}

UAudioComponent* AOutlawsEnemy::SayLine(const TArray<FString>& List, FString& OutLine)
{
	AOutlawsLevelActor* L = Level.Get();
	OutLine.Reset();
	if (!L || List.Num() == 0) return nullptr;
	FString Line = List[FMath::RandHelper(List.Num())];
	// (BUG-TAUNTRATE: nor the line anyone said last, as the x4 copies share their kind's lines)
	const bool bOld = AOutlawsLevelActor::OldTauntRate();
	for (int32 Try = 0; Try < 4 && List.Num() > 1 && (Line == LastTaunt || (!bOld && Line == L->TauntLastLine)); ++Try)
	{
		Line = List[FMath::RandHelper(List.Num())];
	}
	LastTaunt = Line;
	L->TauntLastLine = Line;
	OutLine = Line;
	// (no echo for a voice: off a far face it sounded like a second taunt; BUG-TAUNTS)
	UAudioComponent* A = L->PlaySoundAt(L->TauntWav(Line), EyePoint());
	L->TauntPlaying = A;
	AOutlawsLevelActor::VaryPitch(A);
	return A;
}

bool AOutlawsEnemy::FindDropEdge(const FVector& Way, FVector& OutDir, float& OutDist) const
{
	// (user 2026-10-03, build 19: nobody went over; "the knock off should happen even if it knocks them towards the player")
	// every way round, the shot's first, at 50, 100 and 150 cm: the first place with no floor within 2.5 m under the feet,
	// the way to it clear at the waist; the nearest wins (the shot's way on a tie)
	UWorld* W = GetWorld();
	if (!W || Way.IsNearlyZero()) return false;
	const double Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, Half);
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsDropEdge), false, this);
	bool bFound = false;
	OutDist = 1e9f;
	for (const float Deg : { 0.f, -45.f, 45.f, -90.f, 90.f, -135.f, 135.f, 180.f })
	{
		const FVector D = FRotator(0.0, Deg, 0.0).RotateVector(Way);
		for (const float R : { 50.f, 100.f, 150.f })
		{
			if (R >= OutDist) break;
			const FVector Waist = Feet + FVector(0.0, 0.0, 60.0);
			FHitResult Hit;
			if (W->LineTraceSingleByChannel(Hit, Waist, Waist + D * R, ECC_Visibility, Q)) break;      // (a wall that way)
			const FVector Over = Feet + D * R + FVector(0.0, 0.0, 30.0);
			if (W->LineTraceSingleByChannel(Hit, Over, Over - FVector(0.0, 0.0, 280.0), ECC_Visibility, Q)) continue;
			OutDir = D;
			OutDist = R;
			bFound = true;
			break;
		}
	}
	return bFound;
}

void AOutlawsEnemy::Knock(const FVector& Dir, float Dealt, bool bBlast)
{
	if (bHeldForCutscene)
	{
		PendingKnock = FVector::ZeroVector;        // (BUG-DRDEATHANIM: he stands as he was for the fade to the cutscene)
		return;
	}
	if (CoopRole == 2)
	{
		// (co-op: the client's copy; the host's takes the shove, and the client's follows it)
		CoopReportHit(Dealt, GetActorLocation(), Dir, GetActorLocation() - Dir * 30.0, uint8(2 | (bBlast ? 1 : 0)), 1.f);
		return;
	}
	WakeTick();
	// the hit carries the body along the shot (upward too: a point-blank shotgun blast from a crouch
	// throws a bad guy some 10 m up); a blast throws it up and away; the living only stagger
	FVector V = Dir.GetSafeNormal();
	if (bBlast)
	{
		V = (V.GetSafeNormal2D() + FVector(0.0, 0.0, 0.7)).GetSafeNormal();
	}
	else if (IsDead())
	{
		// a killing shot lifts the body, the more so from below (a crouched shot runs nearly level)
		V = (V.GetSafeNormal2D() + FVector(0.0, 0.0, 0.4 + 2.0 * FMath::Max(0.0, V.Z))).GetSafeNormal();
	}
	float Speed = Dealt * (bBlast ? 360.f : 85.f) * (IsDead() ? 1.f : 0.25f);
	// (user 2026-10-03, REQ-LEDGEKNOCK: "shooting an enemy ... next to an edge with a big drop ... pushed them towards that
	// edge") the Jedi engine only pushes along the shot (computeDamagePushVelocity, x4 on the killing hit); here a drop
	// within 1.5 m, any way round (back toward the shooter too), turns the push onto it with the speed to go over: a kill
	// always, a living one hit hard
	if (!bBlast && (IsDead() || Dealt >= 2.f))
	{
		FVector Edge;
		float EdgeDist = 0.f;
		if (FindDropEdge(V.GetSafeNormal2D(), Edge, EdgeDist))
		{
			const FVector Over = Edge.GetSafeNormal2D();
			// (enough to carry the body past the lip; user 2026-10-04, build 22: "only about half as strong as it needs to be",
			// twice the 120 + 2.4 per cm it was)
			const float Need = 240.f + 4.8f * EdgeDist;
			V = (Over + FVector(0.0, 0.0, 0.25)).GetSafeNormal();
			Speed = FMath::Max(Speed, IsDead() ? Need : FMath::Min(Need, Dealt * 120.f));
			UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s: a drop %.0f cm off, %s: knocked toward it at %.0f cm/s"), *OutlawsName, EdgeDist, IsDead() ? TEXT("dead") : TEXT("living"), Speed);
		}
	}
	// every pellet adds; the sum goes out on the next tick (LaunchCharacter keeps only its last call)
	PendingKnock = (PendingKnock + V * Speed).GetClampedToMaxSize(bBlast ? 5200.f : 3000.f);
}

bool AOutlawsEnemy::GNextHitMelee = false;

void AOutlawsEnemy::TakeHit(float Damage, const FVector& Point, const FVector& Dir, const FVector& From, bool bBlast, float KnockScale)
{
	const bool bMelee = GNextHitMelee;
	GNextHitMelee = false;
	UOutlawsStats::NoteHit(this, From);       // (the level's end panels: who struck him, a shot on target)
	if (CoopRole == 2)
	{
		// (co-op: the client's copy; the host deals the hit, and its result comes back)
		CoopReportHit(Damage, Point, Dir, From, uint8(bBlast ? 1 : 0), KnockScale);
		return;
	}
	ThrottleHitAt = FPlatformTime::Seconds();
	SetThrottleTier(0);                       // (CPU2: shot, he acts on it this frame, whatever the throttle)
	WakeTick();
	if (bDormant || bBehindDoor)
	{
		return;
	}
	if (State == EOutlawsEnemyState::Dead)
	{
		// the rest of a shotgun's pellets and later blasts still push the body, and it still bleeds
		if (AOutlawsLevelActor* L = Level.Get()) L->SpawnBlood(Point, Dir, 1);
		Knock(Dir, Damage * KnockScale * (FVector::Dist(From, GetActorLocation()) < 7.0 * UnitCm ? 4.f : 1.f), bBlast);
		return;
	}
	// [EXE 0x140088150]: the outer third of the body x0.5, the chest (middle band above mid height)
	// x2, else x1; halved while the duck plays; x4 from point blank (within 7 units)
	const FVector Centre = GetActorLocation();
	const FVector Right = FVector::CrossProduct(FVector::UpVector, Dir.GetSafeNormal2D());
	const double Across = FMath::Abs(FVector::DotProduct(Point - Centre, Right)) / GetCapsuleComponent()->GetScaledCapsuleRadius();
	const bool bHigh = Point.Z > Centre.Z;
	float Mult = Across > 2.0 / 3.0 ? 0.5f : (bHigh && Across < 1.0 / 3.0 ? 2.f : 1.f);
	if (IsDucking()) Mult *= 0.5f;
	if (FVector::Dist(From, Centre) < 7.0 * UnitCm) Mult *= 4.f;
	if (bBlast) Mult = 1.f;          // a blast's falloff already set its damage
	if (bMelee && bBoss)
	{
		if (const AOutlawsLevelActor* LH = Level.Get(); LH && LH->bHom) Mult *= 100.f;      // (HM1: the knockout)
	}
	UOutlawsStats::NoteDamage(this, FMath::Clamp(Damage * Mult, 0.f, FMath::Max(0.f, Health)));      // (REQ-STATS)
	Health -= Damage * Mult;
	UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s hit for %.2f (x%.1f), health %.2f"), *OutlawsName, Damage * Mult, Mult, Health);
	// gore: one to four spurts by the damage taken (user 2026-09-28)
	if (AOutlawsLevelActor* L = Level.Get()) L->SpawnBlood(Point, Dir, FMath::Clamp(FMath::CeilToInt(Damage * Mult), 1, 4));
	if (bHeldByPlayer)
	{
		// held up as a shield: no flinch and no push; dead, the player's grip lets go of it (user 2026-09-29)
		if (Health <= 0.f)
		{
			EndHeld(FVector::ZeroVector);
			bTakenAlive = bMelee && bBoss;
			Die(Point, Dir);
		}
		else if (AOutlawsLevelActor* L = Level.Get())
		{
			L->PlaySoundAt(Def.HitSound, GetActorLocation());
		}
		return;
	}
	if (Health <= 0.f)
	{
		bTakenAlive = bMelee && bBoss;
			Die(Point, Dir);
		PendingKnock *= 4.f;          // this volley's earlier pellets pushed a living body (a quarter): now in full
		Knock(Dir, Damage * Mult * KnockScale, bBlast);
		return;
	}
	Knock(Dir, Damage * Mult * KnockScale, bBlast);
	FullPaceUntil = FPlatformTime::Seconds() + 0.7;
	// event 5: nerve down, panic up; a bold one (aggressiveness 4+) fires back at once; a watcher
	// alerts and tells its neighbours
	AdjustMeters(-3.f, 3.f);
	if (Def.Aggressiveness >= 4)
	{
		ShootWait = 0.f;
	}
	++HitsTaken;
	if (AOutlawsLevelActor* L = Level.Get())
	{
		L->PlaySoundAt(Def.HitSound, GetActorLocation());
	}
	if (State == EOutlawsEnemyState::Watch)
	{
		Wake(true);
	}
	Heading = (-Dir).Rotation().Yaw;
	SetState(EOutlawsEnemyState::Hit);
}

void AOutlawsEnemy::Die(const FVector& Point, const FVector& Dir)
{
	UOutlawsStats::NoteKill(this);            // (the level's end panels: the kill of whoever struck him last)
	WakeTick();
	if (bHeldByPlayer || bThrown)
	{
		GetCharacterMovement()->SetMovementMode(MOVE_Falling);
		EndCarry();
	}
	// below mid height: the leg death, else the chest death (the side deaths are dead code in the exe)
	const int32 Chor = Point.Z < GetActorLocation().Z ? Def.LegDieChor : Def.ChestDieChor;
	SetBodyHalf(StandHalf);
	State = EOutlawsEnemyState::Dead;
	DiedAt = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
	StateTime = 0.f;
	// (user 2026-10-12, BUG-DRDEATHANIM; was BUG-BOBDEATH's hiding) Doc's and Bob's fall is in the cutscene their level ends
	// on: when it is there to play, no death here; the cutscene takes over from him as he stood (bFallInCutscene)
	const AOutlawsLevelActor* LE = Level.Get();
	if (bFallInCutscene && bBoss && CoopRole != 2 && LE && LE->EndMovieReady())
	{
		HoldForCutscene(TEXT("the host's or single player's kill"));
	}
	else
	{
		Heading = (-Dir).Rotation().Yaw;
		Play(Chor, false);
	}
	DeadChor = Chor;
	// the body falls and slides with the shot, but no longer stops bullets or the player
	UCapsuleComponent* C = GetCapsuleComponent();
	C->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
	C->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
	// ... nor do the FENCE walls hold it (they keep the living on their ledges: a body goes over the edge; user
	// 2026-09-28, the canyon's cliff snipers)
	C->SetCollisionResponseToChannel(ECC_GameTraceChannel2, ECR_Ignore);
	if (AOutlawsLevelActor* L = Level.Get())
	{
		L->PlaySoundAt(Def.DieSound, GetActorLocation());
		L->OnEnemyKilled(this);
	}
	UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s dies (chor %d), drops %s"), *OutlawsName, Chor, Def.DropItem.IsEmpty() ? TEXT("nothing") : *Def.DropItem);
	if (CoopRole == 1)
	{
		if (UOutlawsCoop* Co = Coop.Get()) Co->NoteEnemyDied(this, Chor, Dir);     // (co-op: the client's copy dies the same way)
	}
}

void AOutlawsEnemy::HoldForCutscene(const TCHAR* Why)
{
	// the frame on show kept, where he stands and facing as he did; no shove from this shot or later ones
	// (user 2026-10-14: in play the killing shot nearly always lands inside the hit stun of the shot before it, and the
	// stun's art is the first frames of the fall: that frame held looked like his death starting.  A stun, a death or no
	// choreography on show: his stand is held instead)
	HeldFromChor = Sprite ? Sprite->GetPlayingChor() : -1;
	if (Sprite && !IsLivePoseChor(HeldFromChor))
	{
		const int32 Stand = Def.WatchChor >= 0 ? Def.WatchChor : 0;
		Sprite->PlayChor(Stand, true);
	}
	bHeldForCutscene = true;
	HeldPlaysRefused = 0;
	PendingKnock = FVector::ZeroVector;
	GetCharacterMovement()->StopMovementImmediately();
	if (Sprite) Sprite->HoldClock();
	UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s dies into the level's cutscene: no death played, his live frame held (chor %d, was %d on show; hit %d, deaths %d %d; %s)"),
		*OutlawsName, Sprite ? Sprite->GetPlayingChor() : -1, HeldFromChor, Def.HitChor, Def.ChestDieChor, Def.LegDieChor, Why);
}

bool AOutlawsEnemy::IsLivePoseChor(int32 Chor) const
{
	return Chor >= 0 && Chor != Def.HitChor && Chor != Def.ChestDieChor && Chor != Def.LegDieChor;
}

void AOutlawsEnemy::FallbackFromHold()
{
	// the level did not end on him after all (the player dead at that moment, a co-op word lost): he falls as anyone else
	if (!bHeldForCutscene) return;
	UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s: the level is not ending on his death after %.1f s: his death played after all"), *OutlawsName, StateTime);
	bHoldLetGo = true;
	Play(DeadChor >= 0 ? DeadChor : Def.ChestDieChor, false);
	bHoldLetGo = false;
}

bool AOutlawsEnemy::CanBeGrabbed() const
{
	// about, not a voice: people, chickens and tarantulas alike (user 2026-09-29); the dead too, people only (user
	// 2026-10-01: a chicken's or a tarantula's last frame is bits of it, on the floor)
	// (co-op: the client's copy only while the host's is neither held nor flying)
	// (not one dead into the level's cutscene: he stands as he was for its fade)
	return !bDormant && !bVoiceOnly && !bHeldByPlayer && !bHeldForCutscene && CoopSelf < 0 && (!IsDead() || !IsCritter()) && (CoopRole != 2 || CoopPuppetGrabbable());
}

bool AOutlawsEnemy::NeckOf(int32 Cell, FVector2D& Out)
{
	// where one takes hold of him (user 2026-09-29: just under the head, the neck or the top of the chest): the body's
	// middle line from its hips and legs (raised arms and guns go off to the sides), the head the topmost drawing on
	// that line, and the hold 19% of his height below the top of it (a hat's crown to the neck, measured on the
	// sprites)
	if (const FVector2D* Hit = NeckCache.Find(Cell)) { Out = *Hit; return true; }
	if (!AtlasRef.IsValid() || Cell < 0) return false;
	const OutlawsTextures::FSpriteAtlas& A = *AtlasRef;
	const int32 Cols = FMath::Max(1, A.Cols);
	const int32 Cx0 = (Cell % Cols) * A.CellW, Cy0 = (Cell / Cols) * A.CellH;
	// first the hand-marked point for this frame, if it has one
	LoadGrabPts();
	if (const TArray<FGrabPt>* Pts = GGrabPts.Find(FPaths::GetBaseFilename(A.Nwx).ToLower()))
	{
		float X0 = 1e9f, X1 = -1e9f, Y1 = -1e9f;
		const OutlawsTextures::FSpriteAtlas::FNode* Big = nullptr;
		for (const OutlawsTextures::FSpriteAtlas::FNode& N : A.Nodes)
		{
			const float Mx = N.X + N.W * 0.5f, My = N.Y + N.H * 0.5f;
			if (Mx < Cx0 || Mx >= Cx0 + A.CellW || My < Cy0 || My >= Cy0 + A.CellH) continue;
			X0 = FMath::Min(X0, float(N.X - Cx0));
			X1 = FMath::Max(X1, float(N.X + N.W - Cx0));
			Y1 = FMath::Max(Y1, float(N.Y + N.H - Cy0));
			if (!Big || N.W * N.H > Big->W * Big->H) Big = &N;
		}
		if (Big)
		{
			for (const FGrabPt& P : *Pts)
			{
				if (P.Cell != Big->Cell) continue;
				const float Sx = P.bFlip != Big->bFlipX ? -1.f : 1.f;
				Out = FVector2D((X0 + X1) * 0.5f + Sx * P.Ox, Y1 - P.Oy);
				NeckCache.Add(Cell, Out);
				return true;
			}
		}
	}
	if (A.Alpha.Num() == 0) return false;
	if (Cx0 + A.CellW > A.AtlasW || Cy0 + A.CellH > A.AtlasH) return false;
	auto At = [&](int32 x, int32 y) { return A.Alpha[(Cy0 + y) * A.AtlasW + Cx0 + x] != 0; };
	int32 Top = -1, Bot = -1;
	for (int32 y = 0; y < A.CellH; ++y) for (int32 x = 0; x < A.CellW; ++x) if (At(x, y)) { if (Top < 0) Top = y; Bot = y; break; }
	if (Top < 0) return false;
	const int32 H = Bot - Top + 1;
	TArray<int32> Xs;
	for (int32 y = Top + int32(H * 0.45f); y < Top + int32(H * 0.8f); ++y) for (int32 x = 0; x < A.CellW; ++x) if (At(x, y)) Xs.Add(x);
	Xs.Sort();
	const float Cx = Xs.Num() ? float(Xs[Xs.Num() / 2]) : A.CellW * 0.5f;
	const int32 Win = FMath::Max(3, int32(A.CellW * 0.10f));
	int32 Head = Top;
	for (int32 y = Top; y <= Bot; ++y)
	{
		int32 N = 0;
		for (int32 x = FMath::Max(0, int32(Cx) - Win); x <= FMath::Min(A.CellW - 1, int32(Cx) + Win); ++x) N += At(x, y) ? 1 : 0;
		if (N >= 2) { Head = y; break; }
	}
	Out = FVector2D(Cx + 0.5f, Head + 0.19f * float(Bot + 1 - Head));
	NeckCache.Add(Cell, Out);
	return true;
}

FVector AOutlawsEnemy::GrabChest() const
{
	// the neck on the frame on show, where the sprite stands; else a third of the way up the body from the middle
	AOutlawsEnemy* Self = const_cast<AOutlawsEnemy*>(this);
	FVector2D N;
	if (Sprite && AtlasRef.IsValid() && !IsCritter() && Self->NeckOf(Sprite->CurrentCell, N))
	{
		const OutlawsTextures::FSpriteAtlas& A = *AtlasRef;
		// on the quad as drawn (it stands on the frame's insertion point, under the actor's place: user 2026-10-02)
		const USceneComponent* Q = Sprite->Quad ? static_cast<const USceneComponent*>(Sprite->Quad.Get()) : Sprite->GetRootComponent();
		const FVector Sc = Q->GetComponentScale();
		const float CmX = Sc.Y / FMath::Max(1, A.CellW), CmY = Sc.Z / FMath::Max(1, A.CellH);
		return Q->GetComponentLocation() - Sprite->GetActorRightVector() * ((N.X - A.CellW * 0.5f) * CmX)   // (texture x runs along -Right: the marked points came out mirrored, user 2026-09-29)
			+ Sprite->GetActorUpVector() * ((A.CellH - N.Y) * CmY);
	}
	return GetActorLocation() + FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() * 0.35);
}

void AOutlawsEnemy::BeginHeld(AActor* By)
{
	WakeTick();
	bHeldByPlayer = true;
	bThrown = false;
	HeldBy = By;
	UCharacterMovementComponent* Move = GetCharacterMovement();
	Move->StopMovementImmediately();
	Move->DisableMovement();
	// (BUG-GRABFALL) taken off a roof by a far grab: no fall of his own is going on now, whatever height he was taken from
	bFalling = bFallCried = false;
	FallTopZ = FallStartZ = GetActorLocation().Z;
	bThrowFromSet = false;
	if (By) GetCapsuleComponent()->IgnoreActorWhenMoving(By, true);
	// a body taken up (user 2026-10-01): the pool under it stops where it has got to, the rest bleeds where it is put down;
	// carried, it drips
	if (IsDead())
	{
		if (AOutlawsLevelActor* LB = Level.Get(); LB && BloodPoolMesh.IsValid()) BleedLeft *= 1.f - LB->StopBloodPool(BloodPoolMesh.Get());
		BloodPoolMesh.Reset();
		bBloodPool = false;
		DripWait = FMath::FRandRange(0.6f, 1.2f);
	}
	// held, and thrown after, the walls that keep the living in (FENCE) or keep bodies out (NOPASS, bullets passing) do
	// not hold him (user 2026-10-01: he could not be thrown through them); back on landing (EndCarry)
	GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_GameTraceChannel2, ECR_Ignore);
	GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_GameTraceChannel3, ECR_Ignore);
	// held in the first frame of the hit stun (user 2026-09-29: the death's first frame has the rifle and shotgun men
	// dropping their guns); thrown, the stun plays on as he flies
	SetBodyHalf(StandHalf);
	if (IsCritter())
	{
		// a chicken or a tarantula kicks its legs in the hand: its walk, round and round (user 2026-09-29)
		Play(Def.WalkChor, true);
		if (Sprite) Sprite->CustomTimeDilation = 1.f;
	}
	else
	{
		Play(Def.HitChor, false);
		if (Sprite) Sprite->CustomTimeDilation = 0.f;
	}
	// (not drawn over the level: the near pull flattened him into the HUD and into the camera, user 2026-09-29; he stays a
	// sprite in the world, locked to the fist)
	if (AOutlawsLevelActor* L = Level.Get(); L && !IsDead()) L->PlaySoundAt(Def.HitSound, GetActorLocation());     // (the dead are quiet)
	FriendsSeeGrab();
	UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s %s by the player"), *OutlawsName, IsDead() ? TEXT("(dead) taken up") : TEXT("grabbed"));
}

void AOutlawsEnemy::EndHeld(const FVector& Vel)
{
	UOutlawsStats::NoteRelease(this, Vel);    // (the level's end panels: a throw)
	if (Sprite) Sprite->SetSwingDriven(false); // (REQ-HELDSWAY: let go, his legs straight at once)
	if (bGrabUpOn)
	{
		// (REQ-GRABUPSCALE) the atlas back at the let-go itself; a load still on its way is dropped (GrabUpGen)
		bGrabUpOn = false;
		++GrabUpGen;
		if (Sprite) Sprite->ClearHeldLook();
		UE_LOG(LogOutlawsAI, Log, TEXT("OLRT grabup: %s let go: the atlas frame again"), *OutlawsName);
	}
	if (CoopRole == 2)
	{
		CoopPuppetRelease(Vel);                   // (co-op: the client's copy flies here at once; the host throws its own)
		return;
	}
	WakeTick();
	bHeldByPlayer = false;
	UCharacterMovementComponent* Move = GetCharacterMovement();
	Move->SetMovementMode(MOVE_Falling);
	// (a body flies as the living do, user 2026-10-01: it lands dead, TickCarried)
	// dropped or thrown: it flies (or just falls) till it lands; thrown hard or up it cries as it goes
	bThrown = true;
	ThrowTime = 0.f;
	ThrowPrev = GetActorLocation();
	// (BUG-GRABFALL) his fall starts at the hand: the height he was pulled from is nothing to it (a throw's own speed still is)
	bFalling = bFallCried = false;
	FallTopZ = FallStartZ = GetActorLocation().Z;
	ThrowFromZ = bPullCap ? FMath::Min(float(GetActorLocation().Z), PullCapZ) : float(GetActorLocation().Z);
	ThrowFromVz = Vel.Z;
	bThrowFromSet = true;
	bPullCap = false;
	ThrowHits.Reset();
	RestoreSpritePose();
	if (Sprite) Sprite->SetOnTop(false);
	// (weight does not come into it: a chicken goes as far as a man or a knife, user 2026-09-29); a chicken or a
	// tarantula gives its own death cry when flung, not the man's fall scream; a man screams only thrown up steeper
	// than 45 degrees
	const bool bCritter = Def.Height < 4.5f;
	const FVector Fling = Vel;
	if (FlightGravity != 1.f)
	{
		if (SavedGravityScale < 0.f) SavedGravityScale = Move->GravityScale;
		Move->GravityScale = SavedGravityScale * FlightGravity;
	}
	FlightGravity = 1.f;
	// held through the floor (the fist goes where it likes): stood back on it and, thrown, slid along it with the
	// throw's whole speed rather than let go in the floor (user 2026-09-29)
	{
		const double Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		const FVector Here = GetActorLocation();
		const AActor* By = HeldBy.Get();
		const double Top = FMath::Max(Here.Z + Half, By ? By->GetActorLocation().Z : Here.Z + Half) + 20.0;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsHeldFloor), false, this);
		if (By) Q.AddIgnoredActor(By);
		FCollisionResponseParams RP;
		RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
		FHitResult Floor;
		UWorld* W = GetWorld();
		// his lowest point: the capsule's, or the picture's foot if that is lower; any of it under the floor slides him (user
		// 2026-10-01: a palm push let go with him clipped should slide as the trigger's throw does; it was 2 cm under)
		double Bottom = Here.Z - Half;
		if (Sprite) Bottom = FMath::Min(Bottom, Sprite->GetActorLocation().Z);
		const bool bFloor = W && W->LineTraceSingleByChannel(Floor, FVector(Here.X, Here.Y, Top), FVector(Here.X, Here.Y, Here.Z - Half - 60.0), ECC_Visibility, Q, RP);
		if (bFloor) UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s let go %.1f cm over the floor (normal z %.2f)"), *OutlawsName, Bottom - Floor.ImpactPoint.Z, Floor.ImpactNormal.Z);
		if (bFloor && Floor.ImpactNormal.Z > 0.7 && Bottom < Floor.ImpactPoint.Z - 0.5)
		{
			SetActorLocation(FVector(Here.X, Here.Y, Floor.ImpactPoint.Z + Half + 2.0), false, nullptr, ETeleportType::TeleportPhysics);
			FVector Flat(Fling.X, Fling.Y, 0.0);
			if (Fling.Size() > 150.f && Flat.Normalize())
			{
				// braked evenly to rest where an arc at the knife's 12 degrees would have come down: v^2 / (2 a) =
				// v^2 sin 24 / g, so a = g / (2 sin 24), about 1.2 g (the flight's own g: the one throw's)
				bFloorSlide = true;
				SavedGroundFriction = Move->GroundFriction;
				SavedBrakeDecel = Move->BrakingDecelerationWalking;
				Move->GroundFriction = 0.f;
				Move->BrakingDecelerationWalking = FMath::Abs(Move->GetGravityZ()) * 1.23f;
				Move->SetMovementMode(MOVE_Walking);
				Move->Velocity = Flat * Fling.Size();
				if (Sprite) Sprite->CustomTimeDilation = 1.f;
				UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s thrown from under the floor: slides at %.0f cm/s"), *OutlawsName, Fling.Size());
				return;
			}
		}
	}
	LaunchCharacter(Fling, true, true);
	if (Sprite) Sprite->CustomTimeDilation = 1.f;
	if (AOutlawsLevelActor* L = Level.Get())
	{
		const float Speed = Fling.Size();
		const float Up = Speed > 1.f ? FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(Fling.Z / Speed, -1.f, 1.f))) : 0.f;
		if (IsDead()) {}                                    // (a body makes no cry)
		else if (bCritter && Speed > 300.f) L->PlaySoundAt(Def.DieSound, GetActorLocation());
		else if (!bCritter && Speed > 300.f && Up > 45.f) L->PlaySoundAt(TEXT("bgfall"), GetActorLocation());
	}
	UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s let go at %.0f cm/s"), *OutlawsName, Vel.Size());
}

void AOutlawsEnemy::RestoreSpritePose()
{
	if (!bCritterPosed || !Sprite) return;
	bCritterPosed = false;
	Sprite->bFaceViewer = true;
	Sprite->SetActorRelativeLocation(FVector(0.0, 0.0, -(GetCapsuleComponent()->GetScaledCapsuleHalfHeight() - 0.5)));
	Sprite->SetActorRelativeRotation(FRotator::ZeroRotator);
}

void AOutlawsEnemy::PoseHeldCritter(const FTransform& Grip, bool bLeftHand, const FVector& Eye)
{
	if (!Sprite || !AtlasRef.IsValid()) return;
	bCritterPosed = true;
	Sprite->bFaceViewer = false;
	const OutlawsTextures::FSpriteAtlas& A = *AtlasRef;
	// the frame on show: its drawn pieces' outline in the cell (1x px, from the cell's top-left)
	const int32 Cell = Sprite->CurrentCell;
	const int32 Cols = FMath::Max(1, A.Cols);
	const int32 Cx0 = (FMath::Max(0, Cell) % Cols) * A.CellW, Cy0 = (FMath::Max(0, Cell) / Cols) * A.CellH;
	float X0 = 1e9f, X1 = -1e9f, Y0 = 1e9f, Y1 = -1e9f;
	for (const OutlawsTextures::FSpriteAtlas::FNode& N : A.Nodes)
	{
		const float Mx = N.X + N.W * 0.5f, My = N.Y + N.H * 0.5f;
		if (Mx < Cx0 || Mx >= Cx0 + A.CellW || My < Cy0 || My >= Cy0 + A.CellH) continue;
		X0 = FMath::Min(X0, float(N.X - Cx0)); X1 = FMath::Max(X1, float(N.X + N.W - Cx0));
		Y0 = FMath::Min(Y0, float(N.Y - Cy0)); Y1 = FMath::Max(Y1, float(N.Y + N.H - Cy0));
	}
	if (X1 < X0) { X0 = 0.f; X1 = float(A.CellW); Y0 = 0.f; Y1 = float(A.CellH); }
	// the head side: where the top quarter of the drawing sits across the cell
	float HeadX = 0.f, HeadN = 0.f;
	for (const OutlawsTextures::FSpriteAtlas::FNode& N : A.Nodes)
	{
		const float Mx = N.X + N.W * 0.5f, My = N.Y + N.H * 0.5f;
		if (Mx < Cx0 || Mx >= Cx0 + A.CellW || My < Cy0 || My >= Cy0 + A.CellH) continue;
		if (N.Y - Cy0 > Y0 + (Y1 - Y0) * 0.3f) continue;
		HeadX += (Mx - Cx0) * N.W * N.H;
		HeadN += N.W * N.H;
	}
	HeadX = HeadN > 0.f ? HeadX / HeadN : (X0 + X1) * 0.5f;
	const FVector Sc = Sprite->GetActorScale3D();                 // (1, width cm, height cm): the whole cell
	const float CmX = Sc.Y / FMath::Max(1, A.CellW), CmY = Sc.Z / FMath::Max(1, A.CellH);
	// the anchor in the cell (px): the tarantula's back, the top middle of the drawing; the chicken's neck, a fifth of
	// the way down from its head's top, across at its head
	const bool bChicken = Def.Height >= 2.5f;                      // (chicken 3, tarantula 2)
	const float Ax = bChicken ? HeadX : (X0 + X1) * 0.5f;
	const float Ay = bChicken ? Y0 + (Y1 - Y0) * 0.2f : Y0 + 1.f;
	// up (feet to head/back) turned with the hand: the tarantula's legs away from the palm; the chicken hangs from the
	// fist, its head out the top of it
	const FQuat G = Grip.GetRotation();
	const FVector Palm = bLeftHand ? G.GetRightVector() : -G.GetRightVector();
	const FVector Up = bChicken ? G.GetUpVector() : -Palm;
	FVector Face = Eye - Grip.GetLocation();
	Face -= Up * FVector::DotProduct(Face, Up);
	if (!Face.Normalize()) Face = G.GetForwardVector();
	const FRotator R = FRotationMatrix::MakeFromXZ(Face, Up).Rotator();
	const FVector Right = FRotationMatrix(R).GetUnitAxis(EAxis::Y);
	const FVector Anchor = Grip.GetLocation() + (bChicken ? FVector::ZeroVector : Palm * 3.0);
	const FVector Off = -Right * ((Ax - A.CellW * 0.5f) * CmX) + Up * ((A.CellH - Ay) * CmY);   // (texture x along -Right)
	Sprite->SetActorLocationAndRotation(Anchor - Off, R);
	// the side of it to the eye, however the hand turns
	const FVector To = Eye - Sprite->GetActorLocation();
	Sprite->OutlawsYaw = FRotator::NormalizeAxis(FMath::RadiansToDegrees(FMath::Atan2(To.Y, To.X)) - 90.f - Sprite->FacingYawOffset);
}

void AOutlawsEnemy::Landed(const FHitResult& Hit)
{
	Super::Landed(Hit);
	bFalling = false;          // (BUG-FALLSCREAM: the next fall measures from its own start, however the ticks were kept)
	// how far it came down: the landing speed under the gravity it fell with (a thrown one's quickened gravity too:
	// v^2 / 2g is the height either way); up to 3 m nothing, then a share of his full health up to all of it at 12 m
	// (a man falls harder than the player, whose falls hurt from 8 m)
	if (IsDead()) return;
	const float Down = FMath::Max(0.f, float(-GetCharacterMovement()->Velocity.Z));
	const float G = FMath::Max(1.f, FMath::Abs(GetCharacterMovement()->GetGravityZ()));
	float Drop = Down * Down / (2.f * G);
	if (bThrown && bThrowFromSet)
	{
		// (BUG-GRABFALL) let go by a hand: no harder than a fall from where it let him go at the speed it gave him
		// (v^2 = v0^2 + 2 g h), so a far grab's pull off a roof adds nothing; a throw down or up still lands as hard as it is
		const float FromHand = FMath::Square(ThrowFromVz) / (2.f * G) + (ThrowFromZ - float(GetActorLocation().Z)) + 30.f;
		if (Drop > FromHand)
		{
			UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s lands at %.1f m of fall by his speed, %.1f m from the hand's let-go: the let-go's"), *OutlawsName, Drop / 100.f, FMath::Max(0.f, FromHand) / 100.f);
			Drop = FMath::Max(0.f, FromHand);
		}
	}
	constexpr float SafeCm = 300.f, LethalCm = 1200.f;
	if (Drop <= SafeCm) return;
	const float Share = FMath::Min(1.f, (Drop - SafeCm) / (LethalCm - SafeCm));
	const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
	// (BUG-ENEMYCLUNK, user 2026-10-12) no "crunch" here: it is the player's landing sound and a man blown up and down again
	// clunked with it every time; the hit below plays his own hurt cry, or his death cry when it kills him
	UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s falls %.1f m: %.0f%% of his health"), *OutlawsName, Drop / 100.f, Share * 100.f);
	FOutlawsStatsScope FallBy(EOutlawsStatsBy(StatsBy), false, false);      // (the fall's kill: whoever threw or struck him last)
	TakeHit(GetMaxHealth() * Share + 0.01f, Feet, FVector::DownVector, GetActorLocation() + FVector(0.0, 0.0, 100.0), true, 0.f);
}

void AOutlawsEnemy::FloorKill(const FVector& Feet)
{
	// (user 2026-10-05, BUG-TRAINFALL) the sector flags that kill the player kill him too: off the train onto its ground he
	// dies, whatever the fall did. Not while held (the hand has him), not a hidden or voice-only one, and on the host only
	// (co-op: the client's copy dies when the host's does)
	if (IsDead() || bDormant || bVoiceOnly || bHeldByPlayer || CoopRole == 2) return;
	UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s on a deadly floor at %s: dies"), *OutlawsName, *Feet.ToCompactString());
	FOutlawsStatsScope FloorBy(EOutlawsStatsBy(StatsBy), false, false);     // (the kill: whoever threw or struck him last)
	TakeHit(GetMaxHealth() * 100.f + 100.f, Feet, FVector::DownVector, GetActorLocation() + FVector(0.0, 0.0, 100.0), true, 0.f);
}

void AOutlawsEnemy::EndCarry()
{
	// down: the walls hold him again (the dead keep to none of the FENCE ones)
	GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_GameTraceChannel3, ECR_Block);
	if (!IsDead()) GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_GameTraceChannel2, ECR_Block);
	if (Sprite && Sprite->GetActorScale3D().Y < 0.0)
	{
		const FVector Sc = Sprite->GetActorScale3D();
		Sprite->SetActorScale3D(FVector(Sc.X, -Sc.Y, Sc.Z));      // (mirrored in the left hand: himself again)
	}
	if (bFloorSlide)
	{
		bFloorSlide = false;
		GetCharacterMovement()->GroundFriction = SavedGroundFriction;
		GetCharacterMovement()->BrakingDecelerationWalking = SavedBrakeDecel;
	}
	if (SavedGravityScale >= 0.f)
	{
		GetCharacterMovement()->GravityScale = SavedGravityScale;       // (the throw's pace over)
		SavedGravityScale = -1.f;
	}
	RestoreSpritePose();
	bHeldByPlayer = bThrown = false;
	bThrowFromSet = false;
	if (Sprite) Sprite->SetOnTop(false);
	// (the holder's collision comes back from his side, a second after the let-go and once clear of him: the player's
	// UpdateEnemyGrab; landing at his feet it caught him, user 2026-10-01)
	HeldBy.Reset();
	if (Sprite) Sprite->CustomTimeDilation = CustomTimeDilation;
}

void AOutlawsEnemy::TickCarried(float Dt)
{
	if (bHeldByPlayer)
	{
		if (Sprite) Sprite->CustomTimeDilation = IsCritter() ? 1.f : 0.f;     // (the player moves the body)
		// a body drips as it is carried, a drop every second or two (user 2026-10-01)
		if (IsDead() && !IsCritter() && BleedLeft > 0.05f)
		{
			DripWait -= Dt;
			if (DripWait <= 0.f)
			{
				DripWait = FMath::FRandRange(1.f, 2.f);
				AOutlawsLevelActor* LD = Level.Get();
				const bool bDrop = LD && LD->StartBloodPool(GetActorLocation() + FVector(FMath::FRandRange(-8.f, 8.f), FMath::FRandRange(-8.f, 8.f), 50.f), 1.f, true) != nullptr;
				UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s drips%s"), *OutlawsName, bDrop ? TEXT("") : TEXT(" (no floor under him)"));
			}
		}
		return;
	}
	// thrown: the death's second and third frames (falling) till the ground, bowling over any bad guy in the way
	ThrowTime += Dt;
	if (Sprite) Sprite->CustomTimeDilation = Sprite->GetChorFrame() >= 2 ? 0.f : 1.f;
	if (CoopRole == 1)
	{
		// (co-op, user 2026-10-02: the player who did not throw him is solid to him, on both machines alike; OutlawsCoopGrabs.cpp)
		FVector KnockVel = GetCharacterMovement()->Velocity;
		if (CoopKnockBody(ThrowPrev, KnockVel)) GetCharacterMovement()->Velocity = KnockVel;
	}
	const FVector V = GetVelocity();
	AOutlawsLevelActor* L = Level.Get();
	// flung through a window, it breaks (user 2026-09-29): looked for ahead along the flight (a capsule's width and a tenth
	// of a second's travel, at the middle and the chest), so the pane is gone before it can stop him, and behind too
	if (L)
	{
		const FVector Here = GetActorLocation();
		if (!ThrowPrev.IsZero()) L->ShatterAlong(ThrowPrev, Here);
		if (V.Size() > 100.f)
		{
			const FVector Dir = V.GetSafeNormal();
			const float Ahead = GetCapsuleComponent()->GetScaledCapsuleRadius() + 20.f + V.Size() * 0.1f;
			const float Hh = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			for (const float Dz : { 0.f, Hh * 0.5f })
			{
				L->ShatterAlong(Here + FVector(0.0, 0.0, Dz), Here + FVector(0.0, 0.0, Dz) + Dir * Ahead);
			}
		}
		ThrowPrev = Here;
	}
	if (L && V.Size() > 250.f)
	{
		const UCapsuleComponent* Me = GetCapsuleComponent();
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
		{
			AOutlawsEnemy* E = Wk.Get();
			if (!E || E == this || E->IsDead() || E->IsDormant() || ThrowHits.Contains(E)) continue;
			const UCapsuleComponent* C = E->GetCapsuleComponent();
			const FVector D = E->GetActorLocation() - GetActorLocation();
			if (FVector2D(D.X, D.Y).Size() < Me->GetScaledCapsuleRadius() + C->GetScaledCapsuleRadius() + 10.0
				&& FMath::Abs(D.Z) < Me->GetScaledCapsuleHalfHeight() + C->GetScaledCapsuleHalfHeight())
			{
				ThrowHits.Add(E);
				// thrown into another, both go down for good (user 2026-09-29)
				UE_LOG(LogOutlawsAI, Log, TEXT("OLRT thrown %s strikes %s: both fall"), *OutlawsName, *E->OutlawsName);
				// ... with no blast of a knock (the killing blow's knock grew with its 100 points and threw them across the
				// map, user 2026-09-29): a hop apart instead, each landing at most 4.5 m off (10 m between them at most)
				FOutlawsStatsScope ThrowerBy(EOutlawsStatsBy(StatsBy), false, false);     // (both the thrower's kills)
				E->TakeHit(100.f, E->GetActorLocation(), V.GetSafeNormal(), GetActorLocation(), false, 0.f);
				TakeHit(100.f, GetActorLocation(), -V.GetSafeNormal(), E->GetActorLocation(), false, 0.f);
				auto Hop = [](AOutlawsEnemy* X, FVector Dir)
				{
					Dir.Z = 0.0;
					if (!Dir.Normalize()) Dir = FVector::ForwardVector;
					const float G = FMath::Max(1.f, FMath::Abs(X->GetCharacterMovement()->GetGravityZ()));
					const float Vz = 250.f;
					const float Vh = FMath::Min(450.f / (2.f * Vz / G), 900.f);
					X->GetCharacterMovement()->StopMovementImmediately();
					X->LaunchCharacter(Dir * Vh + FVector(0.0, 0.0, Vz), true, true);
				};
				Hop(E, V);
				Hop(this, -V);
				if (CoopRole == 1) UOutlawsCoop::NoteThrownStrike(this);     // (co-op: a client's throw: its own flight there hops the same)
				return;
			}
		}
	}
	// (slid along the floor: till it comes to rest, or runs off an edge and lands)
	if (bFloorSlide && GetCharacterMovement()->IsMovingOnGround() && GetVelocity().Size2D() > 40.0 && ThrowTime < 8.f) return;
	if (ThrowTime > 0.25f && GetCharacterMovement()->IsMovingOnGround())
	{
		EndCarry();
		if (IsDead())
		{
			// a body comes down dead: its death again, to lie as it fell; the rest of its blood starts here (Tick)
			Play(DeadChor >= 0 ? DeadChor : Def.ChestDieChor, false);
			UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s (dead) lands, %.0f%% of its blood left"), *OutlawsName, BleedLeft * 100.f);
			return;
		}
		Play(Def.WalkChor, true);
		SetState(EOutlawsEnemyState::Chase);
		// thrown and up again, rattled (backlog 1a): a step less sure, and a fresh choice
		AdjustMeters(-1.f, 1.f);
		SetSub(SubDecide);
		UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s lands and gets up"), *OutlawsName);
	}
}

void AOutlawsEnemy::FaceYaw(float Yaw, float Dt)
{
	const float Delta = FRotator::NormalizeAxis(Yaw - Heading);
	Heading = FRotator::NormalizeAxis(Heading + FMath::Clamp(Delta, -TurnRate * Dt, TurnRate * Dt));
}

void AOutlawsEnemy::UpdateTickRate()
{
	UCharacterMovementComponent* Move = GetCharacterMovement();
	const bool bStill = Move && Move->IsMovingOnGround() && GetVelocity().SizeSquared() < 25.f && !bKnockFlight
		&& PendingKnock.IsNearlyZero() && !bHeldByPlayer && !bThrown;
	float Interval = 0.f;
	if (bStill)
	{
		if (IsDead())
		{
			if (bDropped && StateTime > 3.f) Interval = 0.5f;          // the death played out and the body lies still
		}
		else if (bDormant)
		{
			Interval = 0.5f;
		}
		else if (bFarFrozen)
		{
			Interval = 0.5f;                                           // far and unseen: his tick looks only whether to thaw
		}
		else if (State == EOutlawsEnemyState::Watch && !bHunter)
		{
			// a watcher more than 25 m off still looks for the player five times a second (LosWait keeps its own time)
			const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
			const APawn* P = PC ? PC->GetPawn() : nullptr;
			if (P && FVector::DistSquared(P->GetActorLocation(), GetActorLocation()) > FMath::Square(2500.0) && !(CoopRole == 1 && CoopRemoteNear(2500.0))) Interval = 0.2f;
		}
	}
	if (ThrottleTier == 2) Interval = 0.1f;                        // (CPU2 WANDER: a slow walk, ten steps a second)
	if (Interval != GetActorTickInterval())
	{
		SetActorTickInterval(Interval);
		if (Move) Move->SetComponentTickInterval(Interval);
	}
}

void AOutlawsEnemy::WakeTick()
{
	ThrottleWakeAt = FPlatformTime::Seconds();   // (CPU2: the throttle promotes him through its wake queue)
	// far and unseen and something reached him (an alert, a hit, a hand, a death): he goes on from where he was, and acts
	// on it a while before he may freeze again (user 2026-10-03)
	if (bFarFrozen)
	{
		FreezeGraceUntil = FPlatformTime::Seconds() + 10.0;
		SetFarFrozen(false, TEXT("woken"));
	}
	if (GetActorTickInterval() > 0.f)
	{
		SetActorTickInterval(0.f);
		if (UCharacterMovementComponent* Move = GetCharacterMovement()) Move->SetComponentTickInterval(0.f);
	}
}

void AOutlawsEnemy::UpdateFarFreeze(float Dt)
{
	AOutlawsLevelActor* L = Level.Get();
	if (CVarOLFarFreezeEnable.GetValueOnGameThread() == 0 || !L || CoopRole == 2)
	{
		if (bFarFrozen) SetFarFrozen(false, TEXT("switched off"));
		return;
	}
	// the frozen look every tick (0.5 s); the others four times a second
	FreezeCheck -= Dt;
	if (!bFarFrozen && FreezeCheck > 0.f) return;
	FreezeCheck = 0.25f;
	const UCharacterMovementComponent* Move = GetCharacterMovement();
	// never a boss (scripted), a voice, the dormant, the dead, one held or flying, one reacting to a hit, one in the air or
	// carried by a moving sector (a lift, a scrolling floor: the INF moves him), nor one just woken
	const bool bMay = !IsDead() && !bDormant && CoopSelf < 0 && !bBoss && !bHunter && !bVoiceOnly && !bHeldByPlayer && !bThrown && !bKnockFlight
		&& PendingKnock.IsNearlyZero() && State != EOutlawsEnemyState::Hit && Move && Move->IsMovingOnGround()
		&& !L->IsCarriedBySector(this) && FPlatformTime::Seconds() >= FreezeGraceUntil;
	bool bFar = false;
	if (bMay)
	{
		// frozen past the distance, thawed a tenth inside it: nobody flickers at the border
		const double Limit = double(CVarOLFarFreezeDistance.GetValueOnGameThread()) * (bFarFrozen ? 0.9 : 1.0);
		const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
		const APawn* P = PC ? PC->GetPawn() : nullptr;
		const FVector Here = GetActorLocation();
		bFar = P && Limit > 0.0 && FVector::DistSquared(P->GetActorLocation(), Here) > Limit * Limit && !(CoopRole == 1 && CoopRemoteNear(Limit));
		if (bFar)
		{
			// his sector, looked up again once he has moved 30 cm (as the sprites do theirs)
			if (FreezeSector == -2 || FVector::DistSquared(Here, FreezeSectorAt) > 900.0)
			{
				FreezeSector = L->CullSectorOf(Here, &CullHint);
				FreezeSectorAt = Here;
			}
			bFar = !L->IsSectorSeen(FreezeSector);
		}
	}
	if (bFar != bFarFrozen) SetFarFrozen(bFar, bFar ? TEXT("far and unseen") : bMay ? TEXT("near or seen") : TEXT("busy"));
}

void AOutlawsEnemy::SetFarFrozen(bool bOn, const TCHAR* Why)
{
	if (bOn == bFarFrozen) return;
	static const bool bFreezeTest = OLFlags::Param(TEXT("OLFreezeTest"));
	bFarFrozen = bOn;
	if (bOn)
	{
		// mid-stride: stood where he is (MoveAlong gives its input again each tick once he goes on)
		if (UCharacterMovementComponent* Move = GetCharacterMovement()) Move->StopMovementImmediately();
		if (Sprite) Sprite->CustomTimeDilation = 0.f;         // the frame on show held
		if (bFreezeTest) FreezeSnap = DebugFreezeState();
	}
	else
	{
		if (Sprite) Sprite->CustomTimeDilation = CustomTimeDilation;
		FreezeCheck = 0.25f;
		if (bFreezeTest)
		{
			const FString Now = DebugFreezeState();
			UE_LOG(LogOutlawsAI, Display, TEXT("OLSHOT freeze cycle %s (%s): %s [%s] -> [%s]"), *OutlawsName, Why, Now == FreezeSnap ? TEXT("identical") : TEXT("DIFFERENT"), *FreezeSnap, *Now);
		}
	}
	if (AOutlawsLevelActor* L = Level.Get()) L->NoteFarFreeze(bOn ? 1 : -1);
	UpdateTickRate();
}

FString AOutlawsEnemy::DebugFreezeState() const
{
	const FVector P = GetActorLocation();
	return FString::Printf(TEXT("%.0f %.0f %.0f h%.0f st%d sub%d hp%.2f conf%.1f chor%d/%d"), P.X, P.Y, P.Z, Heading, int32(State), Sub, Health, ConfidenceMeter,
		Sprite ? Sprite->GetPlayingChor() : -1, Sprite ? Sprite->GetChorFrame() : -1);
}

void AOutlawsEnemy::Tick(float Dt)
{
	OL_PERF_SCOPE("Enemies (all)");
	CountTick(GetActorTickInterval());
	Super::Tick(Dt);
	if (CoopRole == 2 && TickPuppetCulled(Dt)) return;     // (CPU2 b: the client's copy, far and unseen)
	if (ThrottleTier == 2)
	{
		TickWander(Dt);                       // (CPU2: throttled to a walk; the manager lifts him)
		return;
	}
	// (REQ-HELDSWAY) held in a hand, his legs swing behind it (OutlawsSpriteSwing.cpp); each machine from its own copy's
	// motion: the client's copy of one in the other player's hand too (that player's pose names him), not once he flies
	if (Sprite)
	{
		const UOutlawsCoop* Cp = CoopRole == 2 && bPuppetHostCarried && !bThrown && !bHeldByPlayer ? Coop.Get() : nullptr;
		Sprite->SetSwingDriven(!IsCritter() && !bThrown && (bHeldByPlayer || (Cp && Cp->OtherPlayerHolds(CoopIndex))));
	}
	TickGrabUpscale();                    // (REQ-GRABUPSCALE: the held frames' upscales on, or off at the let-go)
	UpdateFarFreeze(Dt);
	UpdateTickRate();
	// the menu's movement speed slows the living bad guys' whole pace (their sprites animate with them);
	// the dead fly and fall at full speed
	if (Level.IsValid())
	{
		// ... but a hit's reaction and the flight it throws them into play at full speed (user 2026-09-27)
		if (bKnockFlight && GetCharacterMovement()->IsMovingOnGround())
		{
			bKnockFlight = false;
			// landed: the living keep to their FENCE walls again (a hit carried them over one: they stay where it threw them)
			if (!IsDead() && CoopSelf < 0) GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_GameTraceChannel2, ECR_Block);     // (a match bot walks as a player)
		}
		const bool bReacting = FPlatformTime::Seconds() < FullPaceUntil || bKnockFlight;
		// held or thrown, he goes at the player's hand's own speed (user 2026-10-02: a living one thrown flew in slow motion,
		// at the bad guys' pace of 0.3, three times as long as a body or a rock)
		const float Pace = IsDead() || bReacting || bHeldByPlayer || bThrown || CoopSelf >= 0 ? 1.f : Level->EnemyPace();
		if (CustomTimeDilation != Pace)
		{
			CustomTimeDilation = Pace;
			if (Sprite && !bFarFrozen) Sprite->CustomTimeDilation = Pace;      // (frozen, his sprite holds its frame)
		}
	}
	// far and unseen: nothing more till he thaws (user 2026-10-03)
	if (bFarFrozen)
	{
		if (Sprite) Sprite->OutlawsYaw = Heading;
		return;
	}
	// the sprite's insertion point on the floor, not on the capsule's bottom: walking, the capsule rides 2 cm over the
	// floor, and on a slope its round bottom stands higher still over the floor under its middle (user 2026-10-02)
	if (Sprite)
	{
		const UCharacterMovementComponent* FloorMove = GetCharacterMovement();
		float FloorGap = 0.f;
		if (FloorMove->IsMovingOnGround() && FloorMove->CurrentFloor.bBlockingHit)
		{
			const float CapR = GetCapsuleComponent()->GetScaledCapsuleRadius();
			const float SlopeZ = FMath::Max(0.5f, float(FloorMove->CurrentFloor.HitResult.ImpactNormal.Z));
			FloorGap = FloorMove->CurrentFloor.FloorDist + CapR * (1.f / SlopeZ - 1.f);
		}
		Sprite->SetFloorGap(FloorGap);
	}
	// dead and come to rest on the ground: a pool of blood spreads under the body (user 2026-09-29).  People only: the
	// chickens (HEIGHT 3) and the tarantulas (2) run on the bad guys' logic too, the people stand 6 (townsfolk too)
	if (IsDead() && !bBloodPool && !bHeldByPlayer && !bThrown && BleedLeft > 0.15f && Def.Height >= 4.5f && StateTime > 0.8f
		&& GetCharacterMovement()->IsMovingOnGround() && GetVelocity().Size() < 40.f)
	{
		bBloodPool = true;
		if (AOutlawsLevelActor* L = Level.Get()) BloodPoolMesh = L->StartBloodPool(GetActorLocation(), BleedLeft);
	}
	// a chicken's or a tarantula's death ends in bits of it: its last frame laid flat on the floor, a decal (user 2026-10-01)
	if (IsDead() && IsCritter() && !bRemainsLaid && !bHeldByPlayer && !bThrown && Sprite && StateTime > 1.5f && GetCharacterMovement()->IsMovingOnGround()
		&& GetVelocity().Size() < 20.f)
	{
		bRemainsLaid = true;
		Sprite->bFaceViewer = false;
		const double Hh = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		const double Tall = FMath::Abs(Sprite->GetActorScale3D().Z);
		const FVector Way = FRotator(0.0, Heading, 0.0).Vector();
		// picture up (pitched 90: its top runs back along the heading), centred where it fell, just over the floor
		Sprite->SetActorLocationAndRotation(GetActorLocation() - FVector(0.0, 0.0, Hh - 0.4) + Way * (Tall * 0.5), FRotator(90.0, Heading, 0.0));
		UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s's remains laid on the floor"), *OutlawsName);
	}
	// off a cliff (or thrown off one): the bad guy's fall cry once, 6 m below where the fall began and dropping at 10 m/s or
	// more (bgfall.wav, the exe's own enemy fall sound; user 2026-09-27), a body shot off one too (user 2026-09-28)
	// (BUG-FALLSCREAM, user 2026-10-12: it was 2.5 m below the top of the arc at 3 m/s, so a knock, a blast's hop or a short
	// drop cried; the measure is now from where he left the ground, and an arc that comes back to the same floor never does)
	// (BUG-FALLSCREAM, user 2026-10-14: cries with nobody falling, some right as the player came into the level) also only a
	// fall that has lasted FallCryAirSec, and never a fall that began in his first FallCrySettleSec (a man placed over the floor
	// settling at the start, or one dropped through a floor not yet solid on the level's first frames); a placement or a rescue
	// that moves him further in a tick than his own speed could is no fall either: the fall is measured again from there
	if (GetCharacterMovement()->IsFalling())
	{
		const float Z = GetActorLocation().Z;
		const double Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
		if (!bFalling)
		{
			bFalling = true; bFallCried = false; FallTopZ = Z; FallStartZ = Z; FallLastZ = Z;
			FallStartAt = Now;
			bFallSettling = GetGameTimeSinceCreation() < FallCrySettleSec;
		}
		if (FMath::Abs(Z - FallLastZ) > 300.f + FMath::Abs(float(GetVelocity().Z)) * 2.f * FMath::Max(Dt, 0.05f))
		{
			UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s moved %.0f cm in a tick while falling: no fall, measured again from here"), *OutlawsName, Z - FallLastZ);
			FallTopZ = FallStartZ = Z;
			FallStartAt = Now;
		}
		FallLastZ = Z;
		FallTopZ = FMath::Max(FallTopZ, Z);
		const float Fallen = FallStartZ - Z;
		// (user 2026-10-03, BUG-DEADSCREAM) a body dead more than 5 s (picked up and thrown off a ledge) falls silent
		const bool bLongDead = IsDead() && GetWorld() && GetWorld()->GetTimeSeconds() - DiedAt > 5.0;
		if (!bFallCried && !bLongDead && !bFallSettling && Now - FallStartAt >= FallCryAirSec && Fallen > FallCryDropCm && GetVelocity().Z < -FallCrySpeed)
		{
			bFallCried = true;
			UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s fall cry: %.1f m down in %.2f s at %.0f cm/s"), *OutlawsName, Fallen / 100.f, Now - FallStartAt, -GetVelocity().Z);
			if (AOutlawsLevelActor* L = Level.Get()) L->PlaySoundAt(TEXT("bgfall"), GetActorLocation());
		}
	}
	else
	{
		bFalling = false;
	}
	// the sector light where the body now is: the sprite takes it on as it walks (or flies) between sectors
	if (Sprite && Level.IsValid())
	{
		LightCheck -= Dt;
		if (LightCheck <= 0.f)
		{
			LightCheck = 0.2f;
			Sprite->SetLightLevel(Level->LightAt(GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() - 5.0)));
		}
	}
	if (CoopRole == 2)
	{
		TickPuppet();                             // (co-op: the client's copy shows the host's; nothing of its own)
		return;
	}
	if (!PendingKnock.IsNearlyZero())
	{
		UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s thrown at %.0f cm/s (up %.0f)"), *OutlawsName, PendingKnock.Size(), PendingKnock.Z);
		LaunchCharacter(PendingKnock, false, false);
		bKnockFlight = true;
		// a hit's flight carries the living over a FENCE wall too (off the ledge it guards); back on landing
		GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_GameTraceChannel2, ECR_Ignore);
		PendingKnock = FVector::ZeroVector;
	}
	UWorld* World = GetWorld();
	AOutlawsLevelActor* L = Level.Get();
	if (!World || !World->IsGameWorld() || !L)
	{
		return;
	}
	if (bDormant)
	{
		return;
	}
	if (bBehindDoor)
	{
		if (Sprite) Sprite->OutlawsYaw = Heading;    // (still and unaware till his door opens)
		return;
	}
	TickWater(Dt);                            // (REQ-ENEMYFLOAT: afloat in deep water, drowned held under; OutlawsEnemyFloat.cpp)
	if (bHeldByPlayer || bThrown)
	{
		TickCarried(Dt);
		return;
	}
	StateTime += Dt;
	SubTime += Dt;
	if (State == EOutlawsEnemyState::Dead)
	{
		// the boss's death played out (message 99 on chor END): the level is won [EXE 0x14006ae20]
		if (bBoss && !bBossReported)     // at once: the fall plays out in the fade (user 2026-09-27)
		{
			bBossReported = true;
			L->OnBossDefeated();
		}
		if (bHeldForCutscene && StateTime > 1.5f && !L->DebugLevelEnding() && !L->IsMoviePlaying()) FallbackFromHold();
		// the drop comes once the death has played out and the body lies still
		if (!bDropped && (!Sprite || Sprite->IsChorDone() || bHeldForCutscene) && GetVelocity().Size() < 30.f)
		{
			bDropped = true;
			if (!Def.DropItem.IsEmpty())
			{
				L->SpawnGroundItem(Def.DropItem, GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight()));
			}
		}
		if (Sprite) Sprite->OutlawsYaw = Heading;
		return;
	}
	if (!L->AreEnemiesActive())
	{
		if (Sprite) Sprite->OutlawsYaw = Heading;
		return;
	}
	// CPU2 MOVE-ONLY: on his last plan every frame, thinking (with the time saved up) every ol.throttle.ThinkInterval
	if (ThrottleTier == 1)
	{
		ThinkAcc += Dt;
		if (ThinkAcc < L->ThrottleThinkInterval())
		{
			if (IsMoveSub(Sub)) MoveAlong(Dt);
			if (Sprite) Sprite->OutlawsYaw = Heading;
			return;
		}
		Dt = FMath::Min(ThinkAcc, 0.5f);
		ThinkAcc = 0.f;
	}
	// meters: confidence builds, panic fades [EXE 0x140089bc3]
	ConfidenceMeter = FMath::Clamp(ConfidenceMeter + Dt * Def.Confidence, 0.f, 7.f);
	PanicMeter = FMath::Clamp(PanicMeter - Dt * Def.PanicRate, 0.f, 7.f);
	LosWait -= Dt;
	if (LosWait <= 0.f)
	{
		LookForPlayer();
	}
	FVector Eye, Feet;
	float PR = 34.f, PH = 87.f;
	const bool bPlayer = PlayerView(Eye, Feet, PR, PH) && !TargetDead(L);
	// a roaming boss hunts: always after the player, and while it cannot see them it walks (not runs: they have time
	// to hear it coming) to the next opening on the way to where they are, every second and a half
	if (bHunter && bPlayer && !(bHuntOnceAlerted && State == EOutlawsEnemyState::Watch))
	{
		if (State == EOutlawsEnemyState::Watch)
		{
			Alert(false);
		}
		HuntWait -= Dt;
		if (State == EOutlawsEnemyState::Chase && !bSeen && HuntWait <= 0.f
			&& (Sub == SubFace || Sub == SubDecide || Sub == SubAdvance || Sub == SubRetreat || Sub == SubCover))
		{
			HuntWait = 1.5f;
			TargetPos = Feet;
			const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			FVector Step;
			if (!L->PathStepToward(GetActorLocation() - FVector(0.0, 0.0, Half), Feet, Def.MaxStep, 2.0 * Half / UnitCm, Step))
			{
				Step = Feet;           // no way through the openings: straight at them (a door on the way may open)
			}
			MoveTarget = Step + FVector(0.0, 0.0, Half);
			bRunning = false;
			SetSub(SubAdvance);
		}
	}

	switch (State)
	{
	case EOutlawsEnemyState::Watch:
		// a watcher that sees the player alerts at once and tells the others close by
		if (bSeen && bPlayer)
		{
			Alert(true);
		}
		break;

	case EOutlawsEnemyState::Hit:
		if ((!Sprite || Sprite->IsChorDone()) && StateTime > 0.25f)
		{
			State = EOutlawsEnemyState::Chase;
			StateTime = 0.f;
			// five hits without dying make it think again [EXE 0x14008b8cf]
			if (HitsTaken >= 5)
			{
				HitsTaken = 0;
				SetSub(SubDecide);
			}
			else
			{
				SetSub(SubFace);
			}
		}
		break;

	case EOutlawsEnemyState::Chase:
	{
		ReactToVerbs(Dt);
		// the shoot timer [EXE 0x14008b9c6]; with the player in sight: facing or ducking -> stand and
		// shoot; already shooting -> carry on; on the move (advance, retreat, cover) or deciding ->
		// shoot on the move half the time, else stop, shoot, and take the move up again after
		ShootWait -= Dt;
		if (ShootWait <= 0.f)
		{
			ShootWait = Mode == 1 ? 4.f * (Def.ShootTimer + FMath::FRandRange(0.f, 2.047f)) - 4.096f
				: Def.ShootTimer + FMath::FRandRange(-1.024f, 1.023f);
			ShootWait = FMath::Max(ShootWait, 0.05f);
			// Bob behind his curtain (user 2026-10-03, BUG-BOBSHOOT): not by sight but by reach: anywhere within 20 m of him
			// (the finale room and the door) he keeps leaning out and firing where the player is, through the shut door
			// too (the shots stop in it, and he is heard); his own peek gap paces him
			bool bInReach = false;
			if (bPeekShooter)
			{
				FVector PEye, PFeet;
				float BR = 0.f, BH = 0.f;
				if (PlayerView(PEye, PFeet, BR, BH) && FVector::Dist(PFeet, GetActorLocation()) < 2000.0)
				{
					bInReach = true;
					TargetPos = PFeet + FVector(0.0, 0.0, (PEye.Z - PFeet.Z) * 0.75);
				}
				ShootWait = 0.25f;
			}
			// throwers (THROW_MAX) lob dynamite or knives instead: not built yet, so they hold fire
			if ((bSeen || bInReach) && bPlayer && !bVoiceOnly && Def.ShootChor >= 0 && Def.ThrowMax <= 0.f && Sub != SubShoot && Sub != SubShootMove
				&& !L->TakeAttackToken(this))
			{
				ShootWait = FMath::FRandRange(0.3f, 0.8f);      // (CPU2: enough of them firing at that player now; soon again)
			}
			else if ((bSeen || bInReach) && bPlayer && !bVoiceOnly && Def.ShootChor >= 0 && Def.ThrowMax <= 0.f && Sub != SubShoot && Sub != SubShootMove)
			{
				// (BUG-BOSSPASSIVE) a shot on the move only with a shoot-on-the-move chor in his sprite: the named outlaws' NWXs
				// (and the marshals' and the Gatling man's) have 27 chors or fewer, none of 31 / 32, so he walked on with no
				// shot until the 3 s fallback, one at most.  Not a match bot (ours, not the original's): it keeps shooting on
				// the move as before, walking on to the pickups (the stop-and-shoot cut town_2p's bot pickups from 3..6 to 0..1)
				const int32 MoveChor = bRunning && Def.RunShootChor >= 0 ? Def.RunShootChor : Def.WalkShootChor;
				const bool bMoveChor = CoopSelf >= 0 ? Def.WalkShootChor >= 0
					: MoveChor >= 0 && Sprite && Sprite->Chors.IsValidIndex(MoveChor);
				if (Sub == SubFace || Sub == SubDuck)
				{
					StartShooting(SubFace);
				}
				else if (IsFightingBoss() && IsMoveSub(Sub))
				{
					StartShooting(SubFace);       // (a named outlaw stops, shoots and then stands his ground: Decide above)
				}
				else if (IsMoveSub(Sub) && !IsDucking() && bMoveChor && FMath::RandBool())
				{
					StartShootMove();
				}
				else
				{
					StartShooting(Sub);
				}
			}
		}
		// periodic taunts (-OLOldTauntRate only: each bad guy's own timer; now the level's one clock per listening player,
		// AOutlawsLevelActor::TickTaunts, BUG-TAUNTRATE): out of sight, in sight, or now and then the alert lines
		TauntWait -= Dt;
		if (TauntWait <= 0.f && AOutlawsLevelActor::OldTauntRate())
		{
			// (user 2026-10-03, BUG-TAUNTS: the original's own timer "was still too fast ... twice as much time between taunts")
			TauntWait = 2.f * FMath::Max(1.f, Def.TauntTimer + FMath::FRandRange(-4.096f, 4.095f));
			const bool bOld = Now() - LastSeenTime > 10.0;
			Taunt(!bSeen ? Def.NoLosTaunts : (bOld && FMath::RandBool() ? Def.AlertTaunts : Def.LosTaunts));
		}
		// fire on each EVENT 1 of the playing shoot choreography
		auto FireEvents = [this, bPlayer](int32 Chor) -> bool
		{
			int32 Due = 0;
			bool bHasEvent = false;
			if (Sprite && Sprite->Chors.IsValidIndex(Chor))
			{
				const int32 Shown = Sprite->GetChorFrame();
				for (const FIntVector4& Ev : Sprite->Chors[Chor].Events)
				{
					if (Ev.Y == -5 && Ev.W == 1)
					{
						bHasEvent = true;
						if (Ev.X <= Shown) ++Due;
					}
				}
			}
			const bool bDone = !Sprite || Sprite->IsChorDone() || SubTime > 3.f;
			if (!bHasEvent && bDone && ShotsFired == 0) Due = 1;
			while (ShotsFired < Due)
			{
				++ShotsFired;
				if (bPlayer) FireAtPlayer();
			}
			return bDone;
		};
		auto Resume = [this]()
		{
			// back to the move a shot interrupted, or face the player
			if (IsMoveSub(ResumeSub))
			{
				Sub = ResumeSub;
				SubTime = 0.f;
				StuckTime = 0.f;
				Play(bRunning ? Def.RunChor : Def.WalkChor, true);
			}
			else
			{
				SetSub(SubFace);
			}
		};
		switch (Sub)
		{
		case SubFace:
		{
			FaceYaw((TargetPos - GetActorLocation()).Rotation().Yaw, Dt);
			// two or more others within 1.8 m and the player out of sight: walk off away from the bunch, now and then
			// (user 2026-10-01: following the player they all cornered to one side of a room)
			DisperseWait -= Dt;
			FVector Crowd, Away;
			if (!bSeen && DisperseWait <= 0.f)
			{
				DisperseWait = FMath::FRandRange(1.5f, 3.f);
				if (CrowdAround(GetActorLocation(), 180.0, false, Crowd) >= 2)
				{
					FVector From = GetActorLocation() - Crowd;
					From.Z = 0.0;
					const float Yaw = From.IsNearlyZero() ? FMath::FRandRange(-180.f, 180.f) : float(From.Rotation().Yaw);
					if (PickPoint(5.f, 15.f, Yaw, 70.f, 8, false, Away, true))
					{
						MoveTarget = Away;
						bRunning = false;
						SetSub(SubCover);
						break;
					}
				}
			}
			if (bSeen && bLegsHidden && Def.DuckChor >= 0)
			{
				SetSub(SubDuck);
			}
			else if (SubTime > 1.f)
			{
				SetSub(SubDecide);
			}
			break;
		}
		case SubDecide:
			Decide();
			break;
		case SubAdvance:
		case SubRetreat:
		case SubCover:
			if (bSeen && bLegsHidden && Def.DuckChor >= 0)
			{
				SetSub(SubDuck);
				break;
			}
			MoveAlong(Dt);
			break;
		case SubDuck:
			FaceYaw((TargetPos - GetActorLocation()).Rotation().Yaw, Dt);
			// [EXE 0x14008c7a7..0x14008c86d]: an ambusher (mode 3) turns normal once the player is
			// within 15 units; any mode turns normal when the player's legs show, and then stands;
			// a normal bad guy stays down for its duck time only while the legs stay hidden
			if (Mode == 3 && bPlayer && OctDist(GetActorLocation(), Feet) < 15.0)
			{
				Mode = 0;
			}
			if (bSeen && !bLegsHidden)
			{
				Mode = 0;
				SetSub(SubFace);
			}
			else if (Mode == 0)
			{
				DuckLeft -= Dt;
				if (DuckLeft <= 0.f)
				{
					SetSub(SubFace);
				}
			}
			break;
		case SubShoot:
			FaceYaw((TargetPos - GetActorLocation()).Rotation().Yaw, Dt);
			if (PeekStage >= 0)
			{
				// Bob: lean out (33), shoot (34), back behind the curtain (35)
				const bool bChorDone = !Sprite || Sprite->IsChorDone() || SubTime > 3.f;
				if (PeekStage == 0 && bChorDone) { PeekStage = 1; SubTime = 0.f; Play(34, false); BeginShotWindup(); }
				else if (PeekStage == 1 && FireEvents(34))
				{
					// (the burst: the shot chor again while shots are left and he still sees the player)
					if (--BurstLeft > 0) { SubTime = 0.f; Play(34, false); BeginShotWindup(); }      // (by reach, not sight)
					else { PeekStage = 2; SubTime = 0.f; Play(35, false); }
				}
				else if (PeekStage == 2 && bChorDone) { PeekStage = -1; Resume(); }
				break;
			}
			if (FireEvents(Def.ShootChor))
			{
				if (--BurstLeft > 0 && bSeen)
				{
					SubTime = 0.f;
					Play(Def.ShootChor, false);
					BeginShotWindup();
				}
				else
				{
					Resume();
				}
			}
			break;
		case SubShootMove:
		{
			const int32 Chor = Sprite ? Sprite->GetPlayingChor() : Def.WalkShootChor;
			MoveAlong(Dt);
			if (Sub == SubShootMove && FireEvents(Chor))
			{
				Resume();
			}
			break;
		}
		default:
			SetSub(SubFace);
			break;
		}
		break;
	}

	default:
		break;
	}
	if (Sprite)
	{
		Sprite->OutlawsYaw = Heading;
	}
}

FString AOutlawsEnemy::DebugBodyState() const
{
	return FString::Printf(TEXT("dead %d held %d thrown %d pool %d (mesh %d) bleed %.2f remains %d"), IsDead() ? 1 : 0, bHeldByPlayer ? 1 : 0, bThrown ? 1 : 0,
		bBloodPool ? 1 : 0, BloodPoolMesh.IsValid() ? 1 : 0, BleedLeft, bRemainsLaid ? 1 : 0);
}

void AOutlawsEnemy::FriendsSeeGrab()
{
	// a man grabbed (or a body taken up) where his friends within 10 m can see it (backlog 1a, user 2026-10-01): they wake,
	// their next move is an advance, the nearest shouts one of his alert lines
	AOutlawsLevelActor* L = Level.Get();
	if (!L) return;
	const FVector At = GetActorLocation();
	AOutlawsEnemy* Nearest = nullptr;
	double Best = TNumericLimits<double>::Max();
	int32 N = 0;
	for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
	{
		AOutlawsEnemy* E = Wk.Get();
		if (!E || E == this || E->IsDead() || E->bDormant || E->bVoiceOnly || E->bHeldByPlayer || E->bThrown) continue;
		const double D = FVector::Dist(E->GetActorLocation(), At);
		if (D > 1000.0 || !E->CanSeeFrom(E->EyePoint(), At)) continue;
		if (E->State == EOutlawsEnemyState::Watch) E->Wake(false);
		E->bAdvanceNext = true;
		++N;
		if (D < Best) { Best = D; Nearest = E; }
	}
	if (Nearest) Nearest->Taunt(Nearest->Def.AlertTaunts.Num() > 0 ? Nearest->Def.AlertTaunts : Nearest->Def.LosTaunts, TEXT("friend taken"));
	if (N > 0) UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s taken: %d friends saw it"), *OutlawsName, N);
}

void AOutlawsEnemy::Flinch()
{
	if (IsDead() || bDormant || CoopRole == 2 || bHeldByPlayer || bThrown || State != EOutlawsEnemyState::Chase || FlinchLeft > 0.f) return;
	if (Sub == SubShoot || Sub == SubShootMove) return;          // (a shot under way goes on)
	SetSub(SubFace);
	SubTime = 0.5f;                                             // (SubFace thinks again at 1 s: half a second here)
	if (Def.DuckChor >= 0) Play(Def.DuckChor, false);
	FlinchLeft = 0.5f;
	UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s flinches"), *OutlawsName);
}

void AOutlawsEnemy::ReactToVerbs(float Dt)
{
	AOutlawsLevelActor* L = Level.Get();
	UWorld* W = GetWorld();
	if (!L || !W) return;
	if (FlinchLeft > 0.f)
	{
		FlinchLeft -= Dt;
		if (FlinchLeft <= 0.f && Sub == SubFace) Play(Def.WatchChor, true);
	}
	AOutlawsPlayer* P = W->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(W->GetFirstPlayerController()->GetPawn()) : nullptr;
	if (!P) return;
	const double T = W->GetTimeSeconds();
	// a lit stick in sight within its blast (backlog 1b): straight away from it, at a run, and a shout
	FleeCheck -= Dt;
	if (FleeCheck <= 0.f && T >= FleeUntil && !bStationary)
	{
		FleeCheck = 0.25f;
		TArray<FVector> Lit;
		P->LitDynamite(Lit);
		const FVector Here = GetActorLocation();
		for (const FVector& S : Lit)
		{
			if (FVector::Dist(S, Here) > L->BlastRadiusCm() || !CanSeeFrom(EyePoint(), S)) continue;
			FVector Away = Here - S;
			Away.Z = 0.0;
			const float Yaw = Away.IsNearlyZero() ? FMath::FRandRange(-180.f, 180.f) : float(Away.Rotation().Yaw);
			FVector To;
			for (int32 i = 0; i < 6; ++i)
			{
				if (PickPoint(8.f, 25.f, Yaw, 20.f + 15.f * i, 1, false, To, false))
				{
					MoveTarget = To;
					bRunning = true;
					SetSub(SubRetreat);
					FleeUntil = T + 2.0;
					Taunt(Def.AlertTaunts.Num() > 0 ? Def.AlertTaunts : Def.LosTaunts, TEXT("lit dynamite"));
					UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s runs from lit dynamite"), *OutlawsName);
					break;
				}
			}
			break;
		}
	}
	// the plate held up to it for a second (backlog 1c): its next move flanks, and it waits half a second once; it never
	// stops shooting.  Not in co-op (user 2026-10-02: "remove that altogether for co-op as enemies will constantly pause")
	PlateCheck -= Dt;
	if (PlateCheck <= 0.f && CoopRole == 0 && !UOutlawsCoop::IsActive(this))
	{
		PlateCheck = 0.2f;
		FVector Eye, Feet;
		float R, H;
		const bool bCovered = PlayerView(Eye, Feet, R, H) && P->PlateShields(EyePoint(), FVector(Feet.X, Feet.Y, Feet.Z + (Eye.Z - Feet.Z) * 0.75));
		PlateSeen = bCovered ? PlateSeen + 0.2f : 0.f;
		if (!bCovered) bPlatePaused = false;
		if (PlateSeen >= 1.f && !bPlatePaused)
		{
			bPlatePaused = true;
			ShootWait += 0.5f;
			FlankYawNext = (FMath::RandBool() ? 1.f : -1.f) * FMath::FRandRange(50.f, 70.f);
			UE_LOG(LogOutlawsAI, Log, TEXT("OLRT %s sees the plate: a flank next"), *OutlawsName);
		}
	}
}

// ---- CPU2 (2026-10-05): the lossless pass and the throttle's per-bad-guy side (the manager: OutlawsEnemyThrottle.cpp) ----

namespace
{
	int32 LosslessMask()
	{
		// bit 1 no overlap events, 2 query-only capsule, 4 movement flags; -OLLossless=<mask> for an A/B (7 by default)
		static const int32 Mask = [] { int32 M = 7; OLFlags::Value(TEXT("OLLossless"), M); return M; }();
		return Mask;
	}
}

ECollisionEnabled::Type AOutlawsEnemy::BodyCollision() const
{
	// nothing simulates physics against a bad guy: the movement sweeps and the traces need only queries
	return (LosslessMask() & 2) ? ECollisionEnabled::QueryOnly : ECollisionEnabled::QueryAndPhysics;
}

void AOutlawsEnemy::ApplyLossless()
{
	const int32 Mask = LosslessMask();
	UCapsuleComponent* C = GetCapsuleComponent();
	UCharacterMovementComponent* Move = GetCharacterMovement();
	if (Mask & 1)
	{
		// nothing listens for a bad guy's overlaps (pickups, triggers and doors go by distance and sector)
		C->SetGenerateOverlapEvents(false);
		if (USkeletalMeshComponent* M = GetMesh()) M->SetGenerateOverlapEvents(false);
	}
	if ((Mask & 2) && C->GetCollisionEnabled() != ECollisionEnabled::NoCollision)
	{
		C->SetCollisionEnabled(BodyCollision());
	}
	if (Mask & 4)
	{
		Move->bEnablePhysicsInteraction = false;     // no simulated bodies to push
		Move->SetAvoidanceEnabled(false);
		Move->bAlwaysCheckFloor = false;             // standing still, the floor is not looked for again every frame
		Move->bEnableScopedMovementUpdates = true;
		C->SetShouldUpdatePhysicsVolume(false);      // no physics volumes in a level (its water is the game's own)
	}
}

void AOutlawsEnemy::SetThrottleTier(uint8 NewTier)
{
	if (NewTier == ThrottleTier) return;
	const uint8 Old = ThrottleTier;
	ThrottleTier = NewTier;
	ThrottleTierSince = FPlatformTime::Seconds();
	ThinkAcc = 0.f;
	UCharacterMovementComponent* Move = GetCharacterMovement();
	if (Old == 3)
	{
		SetActorTickEnabled(true);
		if (Sprite) Sprite->SetActorTickEnabled(true);
	}
	if (Old >= 2 && NewTier < 2 && Move)
	{
		if (Move->IsActive()) Move->SetComponentTickEnabled(true);
		// (it finds its floor again on its next tick; not one in a hand, whose movement BeginHeld turned off: the throttle
		// lifts him to FULL the frame after a far grab, and walking again he stood on the floor under the hand each frame,
		// the host's copy of a client's hold 16 cm under it, BUG-GRABLAG; one thrown keeps his flight)
		if (!bHeldByPlayer && !bThrown) Move->SetMovementMode(MOVE_Walking);
	}
	if (NewTier >= 2 && Old < 2 && Move)
	{
		Move->StopMovementImmediately();
		Move->SetComponentTickEnabled(false);
		WanderLeft = 0.f;
		WanderSector = -2;
	}
	if (NewTier == 3)
	{
		SetActorTickEnabled(false);
		if (Sprite) Sprite->SetActorTickEnabled(false);
	}
	UpdateTickRate();
}

void AOutlawsEnemy::TickWander(float Dt)
{
	// a slow walk that never leaves the sector he stands in (so never over a wall or a ledge, into a door or onto a lift),
	// the floor looked up twice a second
	AOutlawsLevelActor* L = Level.Get();
	if (Sprite) Sprite->OutlawsYaw = Heading;
	if (!L || IsDead()) return;
	const FVector Here = GetActorLocation();
	const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const float Rad = GetCapsuleComponent()->GetScaledCapsuleRadius();
	if (WanderSector == -2)
	{
		WanderSector = L->IsCarriedBySector(this) ? -1 : L->CullSectorOf(Here - FVector(0.0, 0.0, Half - 5.0), &CullHint);
		WanderFloorCheck = 0.f;
	}
	if (WanderSector < 0) return;                     // (on a lift, a door or nowhere known: he stands)
	WanderLeft -= Dt;
	if (WanderLeft <= 0.f)
	{
		WanderLeft = FMath::FRandRange(2.f, 5.f);
		WanderDir = FMath::RandBool() ? FRotator(0.0, FMath::FRandRange(0.f, 360.f), 0.0).Vector() : FVector::ZeroVector;
	}
	if (WanderDir.IsNearlyZero()) return;
	const float Speed = GetCharacterMovement()->MaxWalkSpeed * 0.4f * CustomTimeDilation;
	const FVector To = Here + WanderDir * (Speed * Dt);
	const FVector Feet = To - FVector(0.0, 0.0, Half - 5.0);
	if (L->CullSectorOf(Feet, &CullHint) != WanderSector || L->CullSectorOf(Feet + WanderDir * Rad, &CullHint) != WanderSector)
	{
		WanderDir = -WanderDir;                       // the sector's edge: back the way he came
		WanderLeft = FMath::FRandRange(1.f, 3.f);
		return;
	}
	FVector Next = To;
	WanderFloorCheck -= Dt;
	if (WanderFloorCheck <= 0.f)
	{
		WanderFloorCheck = 0.5f;
		FVector Floor;
		if (!FloorAt(To, Half + 60.f, Floor))
		{
			WanderDir = -WanderDir;
			return;
		}
		Next.Z = Floor.Z + Half + 2.0;
	}
	SetActorLocation(Next, false, nullptr, ETeleportType::TeleportPhysics);
	Heading = float(WanderDir.Rotation().Yaw);
	if (Sprite) Sprite->OutlawsYaw = Heading;
}
