// The other co-op player, drawn on this machine (co-op milestone 2; James Anderson's sprite since 2026-10-02).
#include "OutlawsRemotePlayer.h"
#include "OutlawsFlags.h"
#include "OutlawsPerf.h"
#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsTextures.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsLevelActor.h"
#include "OutlawsCoop.h"
#include "OutlawsLab.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/World.h"
#include "Components/TextRenderComponent.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "ProceduralMeshComponent.h"
#include "UnrealClient.h"
#include "Misc/Paths.h"

namespace
{
	// James Anderson's choreographies (JAMES.NWX; cidjames.itm names stand 0, walk 1, shoot 10, crouch 3, hit 20, die 21;
	// the run 2 and the crouch shuffle 4 are his own art the original's player file never named; 15 is a jab, shown for a throw)
	constexpr int32 ChorStand = 0, ChorWalk = 1, ChorRun = 2, ChorCrouch = 3, ChorCrouchWalk = 4, ChorShoot = 10, ChorThrow = 15,
		ChorHit = 20, ChorDie = 21;
	constexpr float MoveOn = 70.f, MoveOff = 40.f;          // cm/s across the ground: moving (the player's own test is 60)
	constexpr float RunOn = 1.25f, RunOff = 1.1f;           // times the walking speed at that player's pace: running
	constexpr float CrouchOn = 60.f, CrouchOff = 66.f;      // the capsule's half height: crouched (the player's own test is 60)
	constexpr double ShotAgainAfter = 0.12;                 // a shot this soon after the last start leaves the flash on show
	constexpr float HitSeconds = 0.35f;
	// R8: the pose buffer (a real internet link: late and lost packets): drawn this far behind the newest pose, on past it
	// this far when the next is late, and a jump of more than this between two poses is a cut (no gliding across)
	constexpr double PoseBufferSeconds = 0.10, PoseExtrapolateSeconds = 0.20;
	constexpr double PoseCutCm = 200.0;
	// the plate over the head (user 2026-10-02), in cm at scale 1: the bar, its black border, the name's size, the speaker
	constexpr float PlateBarW = 44.f, PlateBarH = 5.5f, PlateBorder = 0.9f, PlateNameSize = 10.f, PlateIcon = 9.f;
	constexpr double PlateTalkHold = 0.3;          // the speaker stays this long after a test's talk stops (no flicker)
	// R8: the plate's size, adjustable live on a headset (questperf cmd "ol.PlateScale 1.3")
	TAutoConsoleVariable<float> CVarOLPlateScale(TEXT("ol.PlateScale"), 1.f, TEXT("Co-op: the size of the name, health bar and speaker over the other player's head (1 = as built; 0.25 to 4)."));
	// a plane's picture faces +X (toward the eyes), its U to the viewer's right (-Y), its V down
	FRotator PlateFacing() { return FRotationMatrix::MakeFromXY(FVector(0.0, -1.0, 0.0), FVector(0.0, 0.0, -1.0)).Rotator(); }

	/** A small speaker with two sound waves, cream with a black edge, drawn here (no art from the game files). */
	TArray<FColor> PlateSpeakerPixels(int32 N)
	{
		TArray<FColor> Px;
		Px.Init(FColor(0, 0, 0, 0), N * N);
		const float K = N / 64.f;
		auto Inside = [K](float X, float Y)
		{
			X /= K;
			Y /= K;
			if (X >= 8.f && X <= 21.f && Y >= 24.f && Y <= 40.f) return true;                     // the box
			if (X >= 20.f && X <= 34.f && FMath::Abs(Y - 32.f) <= 8.f + (X - 20.f) * 12.f / 14.f) return true;   // the horn
			const float Dx = X - 34.f, Dy = Y - 32.f;
			const float R = FMath::Sqrt(Dx * Dx + Dy * Dy);
			const bool bAhead = Dx > 0.f && FMath::Abs(FMath::Atan2(Dy, Dx)) < FMath::DegreesToRadians(50.f);
			return bAhead && (FMath::Abs(R - 11.f) <= 2.5f || FMath::Abs(R - 21.f) <= 2.5f);     // two waves
		};
		const float Edge = 2.f * K;
		for (int32 y = 0; y < N; ++y)
		{
			for (int32 x = 0; x < N; ++x)
			{
				const float Cx = x + 0.5f, Cy = y + 0.5f;
				if (Inside(Cx, Cy))
				{
					Px[y * N + x] = FColor(245, 230, 190, 255);
					continue;
				}
				bool bNear = false;
				for (int32 k = 0; k < 8 && !bNear; ++k)
				{
					const float A = UE_TWO_PI * k / 8.f;
					bNear = Inside(Cx + FMath::Cos(A) * Edge, Cy + FMath::Sin(A) * Edge);
				}
				if (bNear) Px[y * N + x] = FColor(0, 0, 0, 255);
			}
		}
		return Px;
	}
}

AOutlawsRemotePlayer::AOutlawsRemotePlayer()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = false;
	SetCanBeDamaged(false);
	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
}

void AOutlawsRemotePlayer::Destroyed()
{
	// (the name's plate is the local player's: hidden, it waits for the next puppet)
	if (AOutlawsPlayer* Me = GetWorld() && GetWorld()->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(GetWorld()->GetFirstPlayerController()->GetPawn()) : nullptr)
	{
		Me->CoopNamePlateText(FString(), false);
	}
	// the sprite is an actor of its own (the level's): an attached or placed actor is not destroyed with this one
	if (AOutlawsSpriteActor* S = Sprite.Get()) S->Destroy();
	Sprite.Reset();
	for (const FLeftBody& Lb : LeftBodies) if (AOutlawsSpriteActor* S = Lb.S.Get()) S->Destroy();
	LeftBodies.Reset();
	Super::Destroyed();
}

UMaterialInterface* AOutlawsRemotePlayer::Flat(const FColor& Colour)
{
	// the level's unlit world material with a flat colour (the scene has no lights), as the player's own SolidMaterial
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_World.M_OLR_World"));
	if (!Base) return nullptr;
	TArray<FColor> Pixels;
	Pixels.Init(Colour, 4);
	UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(Base, this);
	if (UTexture2D* T = OutlawsTextures::MakeTextureBGRA(2, 2, Pixels, false, TF_Nearest, TA_Clamp))
	{
		M->SetTextureParameterValue(TEXT("Tex"), T);
		KeepAlive.Add(T);
	}
	KeepAlive.Add(M);
	return M;
}

UStaticMeshComponent* AOutlawsRemotePlayer::Part(const TCHAR* Shape, const FColor& Colour, const TCHAR* Name)
{
	UStaticMeshComponent* C = Name ? NewObject<UStaticMeshComponent>(this, Name) : NewObject<UStaticMeshComponent>(this);
	C->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, Shape));
	if (UMaterialInterface* M = Flat(Colour)) C->SetMaterial(0, M);
	C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	C->SetGenerateOverlapEvents(false);
	C->SetCastShadow(false);
	C->SetupAttachment(Root);
	C->SetUsingAbsoluteLocation(true);
	C->SetUsingAbsoluteRotation(true);
	C->SetUsingAbsoluteScale(true);
	C->RegisterComponent();
	return C;
}

void AOutlawsRemotePlayer::AddFlyer(int32 Kind, const FVector& Pos, const FVector& Vel, float Grav, float Fuse)
{
	FFlyer F;
	USceneComponent* M = nullptr;
	// (BUG-REMOTETNT) a stick or a knife: the thrower's own look, built by this machine's player as its own thrown copy
	// (the held model, its textures, the held material, a lit stick's wick and fire); -OLRemoteFlyerOld keeps the stand-in
	// (tests: the before shots)
	AOutlawsPlayer* Me = GetWorld() && GetWorld()->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(GetWorld()->GetFirstPlayerController()->GetPawn()) : nullptr;
	if ((Kind == 1 || Kind == 2) && Me && !OLFlags::Param(TEXT("OLRemoteFlyerOld"))
		&& Me->BuildThrownLook(this, Kind, Fuse > 0.f, F.Look))
	{
		M = F.Look.Mesh.Get();
	}
	if (M)
	{
		M->SetWorldLocation(Pos);
		M->SetWorldScale3D(FVector::OneVector);
		if (LooksShown++ < 8)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP thrown look: a %s drawn as the thrower's own (%d sections, wick %d, flame %s, grows to %.2f)"), Kind == 1 ? TEXT("stick") : TEXT("knife"),
				F.Look.Mesh.IsValid() ? F.Look.Mesh->GetNumSections() : 0, F.Look.FuseSection, F.Look.Flame.IsValid() ? TEXT("yes") : TEXT("no"), F.Look.GrowTo);
		}
	}
	else
	{
		// a green bottle, a brown lump (and a red stick, a grey blade when the look cannot be built): the shape of what flies
		const TCHAR* Cylinder = TEXT("/Engine/BasicShapes/Cylinder.Cylinder");
		const TCHAR* Sphere = TEXT("/Engine/BasicShapes/Sphere.Sphere");
		const TCHAR* Cube = TEXT("/Engine/BasicShapes/Cube.Cube");
		UStaticMeshComponent* S = nullptr;
		FVector Scale(0.1);
		switch (Kind)
		{
		case 1: S = Part(Cylinder, FColor(176, 40, 30), nullptr); Scale = FVector(0.04, 0.04, 0.2); break;
		case 2: S = Part(Cube, FColor(170, 170, 180), nullptr); Scale = FVector(0.25, 0.02, 0.05); break;
		case 3: S = Part(Sphere, FColor(60, 120, 70), nullptr); Scale = FVector(0.08, 0.08, 0.2); break;
		case 5: S = Part(Cylinder, FColor(150, 32, 26), nullptr); Scale = FVector(0.021, 0.021, 0.065); ++ShellsShown; break;     // (REQ-FLICKRELOAD: a fired shell)
		default: S = Part(Cube, FColor(110, 80, 50), nullptr); Scale = FVector(0.2); break;
		}
		if (!S) return;
		S->SetWorldLocation(Pos);
		S->SetWorldScale3D(Scale);
		M = S;
		if (Kind == 1 || Kind == 2)
		{
			++StandInsShown;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP thrown look: a %s drawn as a stand-in shape"), Kind == 1 ? TEXT("stick") : TEXT("knife"));
		}
	}
	F.Fuse = Fuse;
	F.Mesh = M;
	F.Pos = Pos;
	F.Vel = Vel;
	F.Grav = Grav;
	F.Kind = Kind;
	F.Life = Kind == 1 && Fuse > 0.f ? Fuse + 1.f : Kind == 5 ? 2.f : 4.f;
	Flyers.Add(F);
}

void AOutlawsRemotePlayer::EndFlyersNear(const FVector& At, double Radius)
{
	for (FFlyer& F : Flyers)
	{
		if (F.Kind == 1 && FVector::DistSquared(F.Pos, At) < Radius * Radius) F.Life = 0.f;
	}
}

void AOutlawsRemotePlayer::TickFlyers(float Dt)
{
	UWorld* W = GetWorld();
	APlayerController* PC = W ? W->GetFirstPlayerController() : nullptr;
	AOutlawsPlayer* Me = PC ? Cast<AOutlawsPlayer>(PC->GetPawn()) : nullptr;
	static const bool bShotMode = OLFlags::Param(TEXT("OLRemoteFlyerShots"));
	for (int32 i = Flyers.Num() - 1; i >= 0; --i)
	{
		FFlyer& F = Flyers[i];
		USceneComponent* M = F.Mesh.Get();
		F.Life -= Dt;
		F.Age += Dt;
		if (!M || F.Life <= 0.f)
		{
			if (M) M->DestroyComponent();
			if (UStaticMeshComponent* Fl = F.Look.Flame.Get()) Fl->DestroyComponent();
			if (UStaticMeshComponent* Co = F.Look.Core.Get()) Co->DestroyComponent();
			Flyers.RemoveAt(i);
			continue;
		}
		// (BUG-REMOTETNT) the thrower's own look: it grows to the level item's size over a second, as the thrower's copy does,
		// and a lit stick's wick burns down under its flame
		const bool bLook = F.Look.Mesh.IsValid();
		if (bLook && F.Look.GrowTo != 1.f)
		{
			M->SetWorldScale3D(FVector(FMath::InterpEaseInOut(1.f, F.Look.GrowTo, FMath::Clamp(F.Age, 0.f, 1.f), 2.f)));
		}
		// (-OLRemoteFlyerShots, tests) held up before this machine's eyes a moment and shot, side on, then let fly on
		static TSet<int32> KindsShot;          // (the first of each kind only)
		if (bShotMode && !F.bShot && !KindsShot.Contains(F.Kind) && F.Age >= 0.15f && PC && PC->PlayerCameraManager)
		{
			const FVector Eye = PC->PlayerCameraManager->GetCameraLocation();
			const FRotator CamRot = PC->PlayerCameraManager->GetCameraRotation();
			const FVector Ahead = FRotator(0.0, CamRot.Yaw, 0.0).Vector();
			M->SetWorldLocationAndRotation(Eye + Ahead * 70.0 + FRotator(0.0, CamRot.Yaw, 0.0).Quaternion().GetRightVector() * 4.0 - FVector(0.0, 0.0, 14.0),
				FRotator(-25.0, CamRot.Yaw + 90.0, 0.0));
			M->SetWorldScale3D(FVector::OneVector);      // (at the hand's size for the shot: the whole stick and its flame in view)
			if (F.Kind == 1 && F.Fuse > 0.f) F.Fuse = FMath::Max(F.Fuse, 3.f);
			F.Life = FMath::Max(F.Life, 2.f);
			if (bLook && Me && F.Look.Flame.IsValid()) Me->PlaceThrownFuse(F.Look, F.Fuse);
			if (F.Age >= 1.3f)
			{
				F.bShot = true;
				KindsShot.Add(F.Kind);
				const UOutlawsCoop* C = UOutlawsCoop::Get(this);
				const FString Name = FString::Printf(TEXT("ol_remote_%s_%s"), F.Kind == 1 ? TEXT("stick") : F.Kind == 2 ? TEXT("knife") : TEXT("thing"),
					C && C->IsHost() ? TEXT("host") : TEXT("client"));
				FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
				FString Mats;
				if (const UPrimitiveComponent* P = Cast<UPrimitiveComponent>(M))
				{
					for (int32 k = 0; k < P->GetNumMaterials(); ++k)
					{
						const UMaterialInterface* Mi = P->GetMaterial(k);
						const UMaterialInstance* Inst = Cast<UMaterialInstance>(Mi);
						UTexture* Tex = nullptr;
						if (Mi) Mi->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Tex")), Tex);
						Mats += FString::Printf(TEXT("[%d parent %s tex %s] "), k, Inst && Inst->Parent ? *Inst->Parent->GetName() : TEXT("-"), Tex ? *Tex->GetName() : TEXT("none"));
					}
				}
				const UStaticMeshComponent* Fm = F.Look.Flame.Get();
				UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT remote flyer %s: %s, %s; flame %s"), *Name, bLook ? TEXT("the thrower's look") : TEXT("a stand-in shape"), *Mats,
					Fm && Fm->GetStaticMesh() ? *Fm->GetStaticMesh()->GetName() : TEXT("none"));
			}
			continue;
		}
		if (bLook && F.Kind == 1 && F.Fuse > 0.f)
		{
			F.Fuse = FMath::Max(0.f, F.Fuse - Dt);
			if (Me) Me->PlaceThrownFuse(F.Look, F.Fuse);
		}
		if (F.bRest) continue;
		// under gravity until it meets the level; there it lies (a knife or a stick that lands becomes the other machine's
		// ground item, which comes by itself: the stand-in goes soon after)
		F.Vel.Z -= 980.0 * F.Grav * Dt;
		const FVector To = F.Pos + F.Vel * Dt;
		FHitResult Hit;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsCoopFlyer), false, this);
		if (W && W->LineTraceSingleByChannel(Hit, F.Pos, To, ECC_Visibility, Q))
		{
			F.Pos = Hit.ImpactPoint + Hit.ImpactNormal * 2.0;
			// a stick that meets a wall or a beam falls off it (as the host's copy of a bot's stick does, TickBotThrown)
			if (F.Kind == 1 && Hit.ImpactNormal.Z < 0.6f)
			{
				F.Vel = F.Vel.MirrorByVector(FVector(Hit.ImpactNormal)) * 0.3;
				M->SetWorldLocation(F.Pos);
				continue;
			}
			F.bRest = true;
			F.Life = FMath::Min(F.Life, F.Kind == 1 ? F.Life : 1.0f);
		}
		else
		{
			F.Pos = To;
		}
		F.Spin += Dt * 600.f;
		M->SetWorldLocationAndRotation(F.Pos, FRotator(F.Spin, F.Vel.Rotation().Yaw, 0.f));
	}
}

// ---- the sprite ----------------------------------------------------------------------------------------------------------

const TCHAR* AOutlawsRemotePlayer::AnimName(EAnim A)
{
	switch (A)
	{
	case EAnim::Stand: return TEXT("stand");
	case EAnim::Walk: return TEXT("walk");
	case EAnim::Run: return TEXT("run");
	case EAnim::Crouch: return TEXT("crouch");
	case EAnim::CrouchWalk: return TEXT("crouch-walk");
	case EAnim::Shoot: return TEXT("shoot");
	case EAnim::Throw: return TEXT("throw");
	case EAnim::Hit: return TEXT("hit");
	case EAnim::Die: return TEXT("die");
	default: return TEXT("none");
	}
}

int32 AOutlawsRemotePlayer::AnimChor(EAnim A)
{
	switch (A)
	{
	case EAnim::Walk: return ChorWalk;
	case EAnim::Run: return ChorRun;
	case EAnim::Crouch: return ChorCrouch;
	case EAnim::CrouchWalk: return ChorCrouchWalk;
	case EAnim::Shoot: return ChorShoot;
	case EAnim::Throw: return ChorThrow;
	case EAnim::Hit: return ChorHit;
	case EAnim::Die: return ChorDie;
	default: return ChorStand;
	}
}

const TCHAR* AOutlawsRemotePlayer::ShownState() const
{
	return AnimName(Shown);
}

int32 AOutlawsRemotePlayer::ShownChor() const
{
	const AOutlawsSpriteActor* S = Sprite.Get();
	return S ? S->GetPlayingChor() : -1;
}

FString AOutlawsRemotePlayer::ChorsSeen() const
{
	return FString::Printf(TEXT("%s (%d changes logged)"), *FString::Join(Seen, TEXT(", ")), LogLines);
}

void AOutlawsRemotePlayer::SetSprite(AOutlawsSpriteActor* InSprite, AOutlawsLevelActor* InLevel)
{
	// (BUG-TNTVANISH: a thrown body still in the air goes on as a body of its own, not with the sprite it had)
	bool bGave = false;
	if (AOutlawsSpriteActor* Old = Sprite.Get(); Old && Old != InSprite)
	{
		bGave = LeaveBody(TEXT("its sprite changed"));
		if (!bGave) Old->Destroy();
	}
	Sprite = InSprite;
	Level = InLevel;
	bSpriteGiven = bGave && !InSprite;
	if (!InSprite) return;
	// out of sight till this actor's next tick places it; the chor wanted then is shown at once (a body already lying)
	InSprite->SetActorHiddenInGame(true);
	InSprite->bFaceViewer = true;
	bFreshSprite = true;
	Shown = EAnim::None;
	LightCheck = 0.f;
}

float AOutlawsRemotePlayer::ChorSeconds(EAnim A, float Fallback) const
{
	// a one-shot's length as the sprite plays it: the frames its first row shows, at its own rate
	const AOutlawsSpriteActor* S = Sprite.Get();
	const int32 C = AnimChor(A);
	if (!S || !S->Chors.IsValidIndex(C) || S->Chors[C].FPS <= 0.f) return Fallback;
	return FMath::Max(1, S->Chors[C].Length) / S->Chors[C].FPS;
}

void AOutlawsRemotePlayer::NoteShot()
{
	const AOutlawsCoopLink* L = Link.Get();
	if (L && L->Stats.Seq > 0 && L->Stats.bDead) return;
	// each shot from the shoot chor's start (its first frame is the flash), unless the last start is still that fresh (the
	// gatling's ten a second keep the flash up); the chor shows for its length (0.56 s), then the movement's again
	bActionRestart = Action == EAnim::Shoot && Clock < ActionUntil && Clock - ShownAt >= ShotAgainAfter;
	ActionWhy = Action == EAnim::Shoot && Clock < ActionUntil ? TEXT("another shot") : TEXT("a shot seen");
	Action = EAnim::Shoot;
	ActionUntil = Clock + ChorSeconds(EAnim::Shoot, 0.5f);
}

void AOutlawsRemotePlayer::NoteThrow(int32 Kind)
{
	const AOutlawsCoopLink* L = Link.Get();
	if (L && L->Stats.Seq > 0 && L->Stats.bDead) return;
	bActionRestart = Action == EAnim::Throw && Clock < ActionUntil;
	ActionWhy = Kind == 1 ? TEXT("a stick of dynamite thrown") : Kind == 2 ? TEXT("a knife thrown") : Kind == 3 ? TEXT("a bottle thrown") : TEXT("a prop thrown");
	Action = EAnim::Throw;
	ActionUntil = Clock + ChorSeconds(EAnim::Throw, 0.3f) + 0.15f;     // (the jab, then its guard a moment)
}

void AOutlawsRemotePlayer::LogChor(EAnim A, const FString& Why)
{
	if (!Seen.Contains(AnimName(A))) Seen.Add(AnimName(A));
	// a few lines a second at most: the changes beyond that are counted and said with the next line
	if (LogTokens < 1.f)
	{
		++LogSkipped;
		return;
	}
	LogTokens -= 1.f;
	++LogLines;
	const FString Skipped = LogSkipped > 0 ? FString::Printf(TEXT("; %d changes before not logged"), LogSkipped) : FString();
	LogSkipped = 0;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP puppet chor: %s (chor %d; %s%s)"), AnimName(A), AnimChor(A), *Why, *Skipped);
}

void AOutlawsRemotePlayer::TickSprite(float Dt, const FOutlawsCoopPose& P, const FOutlawsCoopStats& St, float Pace)
{
	AOutlawsSpriteActor* S = Sprite.Get();
	if (!S) return;
	LogTokens = FMath::Min(4.f, LogTokens + Dt * 3.f);
	const FVector Feet(Body.X, Body.Y, Body.Z - Half);
	// on the feet, facing where the player looks (the head's yaw: in VR the body's own yaw turns only with the stick)
	S->SetActorLocation(Feet + FVector(0.0, 0.0, 0.5));
	const bool bDead = St.Seq > 0 && St.bDead;
	if (!bDead) FacingYaw = float(HeadRot.Yaw);
	S->OutlawsYaw = FacingYaw;
	// the sector light where the feet are, as a bad guy's (five times a second)
	LightCheck -= Dt;
	if (LightCheck <= 0.f)
	{
		LightCheck = 0.2f;
		if (const AOutlawsLevelActor* L = Level.Get()) S->SetLightLevel(L->LightAt(Feet + FVector(0.0, 0.0, 5.0)));
	}
	// what the player does, from what arrives: the speed across the ground (the pose's), a crouch from the capsule's half
	// height, death and hurts from the stats, shots and throws as their events come
	const float WalkSpeed = 960.f * (Pace > 0.f ? Pace : 0.5f);
	if (!bMoving && Speed > MoveOn) bMoving = true;
	else if (bMoving && Speed < MoveOff) bMoving = false;
	if (!bMoving) bRunning = false;
	else if (!bRunning && Speed > WalkSpeed * RunOn) bRunning = true;
	else if (bRunning && Speed < WalkSpeed * RunOff) bRunning = false;
	if (!bCrouched && P.HalfHeight < CrouchOn) bCrouched = true;
	else if (bCrouched && P.HalfHeight > CrouchOff) bCrouched = false;
	FString HitWhy;
	if (St.Seq > 0 && St.Seq != LastStatsSeq)
	{
		LastStatsSeq = St.Seq;
		if (bSeenStats && !St.bDead && !bWasDead && St.Health < LastHealth - 0.01f)
		{
			HitWhy = FString::Printf(TEXT("health %.1f -> %.1f"), LastHealth, St.Health);
		}
		bSeenStats = true;
		LastHealth = St.Health;
	}
	if (!HitWhy.IsEmpty())
	{
		Action = EAnim::Hit;
		ActionUntil = Clock + HitSeconds;
		ActionWhy = HitWhy;
		bActionRestart = Shown == EAnim::Hit;
	}
	// the choreography wanted: dead above all; a shot, throw or hurt for its length (not crouched: he has no crouched art for
	// those, and a standing frame would show him above his cover); else crouched, moving, running or standing
	EAnim Want = EAnim::Stand;
	FString Why;
	const int32 Cm = FMath::RoundToInt(Speed);
	bool bFinish = false;
	if (bDead)
	{
		Want = EAnim::Die;
		bFinish = Shown == EAnim::None;              // (first seen dead, or a new sprite for a body: already lying)
		Why = bFinish ? TEXT("dead when first seen: lying") : TEXT("dead: falls, then lies");
		Action = EAnim::None;
	}
	else if (Action != EAnim::None && Clock < ActionUntil && !bCrouched)
	{
		Want = Action;
		Why = ActionWhy;
	}
	else
	{
		if (Action != EAnim::None && (Clock >= ActionUntil || bCrouched)) Action = EAnim::None;
		if (bCrouched)
		{
			Want = bMoving ? EAnim::CrouchWalk : EAnim::Crouch;
			Why = bMoving ? FString::Printf(TEXT("crouched and moving at %d cm/s"), Cm) : FString::Printf(TEXT("crouched: body half height %.0f"), P.HalfHeight);
		}
		else if (bMoving)
		{
			Want = bRunning ? EAnim::Run : EAnim::Walk;
			Why = FString::Printf(TEXT("moving at %d cm/s%s"), Cm, bRunning ? TEXT(", a run") : TEXT(""));
		}
		else
		{
			Why = bWasDead ? TEXT("up again") : (Shown == EAnim::Shoot || Shown == EAnim::Throw || Shown == EAnim::Hit) ? TEXT("done, standing") : TEXT("standing still");
		}
	}
	const bool bRestart = bActionRestart && Want == Action;
	bActionRestart = false;
	if (Want != Shown || bRestart || bFreshSprite)
	{
		const bool bLoop = Want != EAnim::Shoot && Want != EAnim::Throw && Want != EAnim::Hit && Want != EAnim::Die;
		S->PlayChor(AnimChor(Want), bLoop);
		if (bFinish) S->FinishChor();
		if (Want != Shown || bRestart) LogChor(Want, Why);
		Shown = Want;
		ShownAt = Clock;
	}
	bFreshSprite = false;
	bWasDead = bDead;
	// the legs at the art's own pace when the player goes at the game's own speed, slower when slower (a VR step, a stick
	// half pushed); one-shots and standing at their own rate
	float Gait = 1.f;
	if (Shown == EAnim::Walk) Gait = Speed / WalkSpeed;
	else if (Shown == EAnim::Run) Gait = Speed / (WalkSpeed * 2.f);
	S->CustomTimeDilation = Shown == EAnim::CrouchWalk ? CrouchWalkLegs(Speed, Pace) : FMath::Clamp(Gait, 0.5f, 1.5f);
}

float AOutlawsRemotePlayer::CrouchWalkLegs(float Speed, float Pace)
{
	// REQ-CROUCHRUN (user 2026-10-28): a crouched run goes twice the crouch walk (the run's share over the walk), and the art
	// has no crouched run of its own: the crouch walk's legs go up to twice its pace, so they keep up with the ground
	static const bool bOld = OLFlags::Param(TEXT("OLOldCrouchRun"));
	const float WalkSpeed = 960.f * (Pace > 0.f ? Pace : 0.5f);
	return FMath::Clamp(Speed / (WalkSpeed * 0.5f), 0.5f, bOld ? 1.5f : 2.f);
}

bool AOutlawsRemotePlayer::SamplePose(double Now, FPoseSample& Out, bool& bOutCut)
{
	bOutCut = false;
	LastLateBy = 0.0;
	LastNewestGap = Samples.Num() >= 2 ? Samples.Last().T - Samples[Samples.Num() - 2].T : 0.0;
	if (Samples.Num() == 0) return false;
	if (Samples.Num() == 1)
	{
		Out = Samples[0];
		bOutCut = true;
		return true;
	}
	const double T = Now - ClockOffset - PoseBufferSeconds;
	const FPoseSample& First = Samples[0];
	const FPoseSample& Last = Samples.Last();
	if (T <= First.T)
	{
		Out = First;
		return true;
	}
	if (T >= Last.T)
	{
		// late: on along the newest pair's way, a fifth of a second at most, then held
		const FPoseSample& Prev = Samples[Samples.Num() - 2];
		const double Span = FMath::Max(Last.T - Prev.T, 0.001);
		const double Ahead = FMath::Min(T - Last.T, PoseExtrapolateSeconds);
		Out = Last;
		Out.Body = Last.Body + (Last.Body - Prev.Body) * (Ahead / Span);
		Out.Head = Last.Head + (Last.Head - Prev.Head) * (Ahead / Span);
		if (Ahead > 0.06) ++Extrapolated;
		LastLateBy = T - Last.T;
		return true;
	}
	int32 i = Samples.Num() - 2;
	while (i > 0 && Samples[i].T > T) --i;
	const FPoseSample& A = Samples[i];
	const FPoseSample& B = Samples[i + 1];
	const float Alpha = float(FMath::Clamp((T - A.T) / FMath::Max(B.T - A.T, 0.001), 0.0, 1.0));
	Out.T = T;
	Out.Body = FMath::Lerp(A.Body, B.Body, Alpha);
	Out.Head = FMath::Lerp(A.Head, B.Head, Alpha);
	Out.HeadRot = FQuat::Slerp(A.HeadRot, B.HeadRot, Alpha);
	Out.Half = FMath::Lerp(A.Half, B.Half, Alpha);
	Out.Speed = FMath::Lerp(A.Speed, B.Speed, Alpha);
	return true;
}

void AOutlawsRemotePlayer::Tick(float DeltaSeconds)
{
	OL_PERF_SCOPE("Other ours");
	Super::Tick(DeltaSeconds);
	Clock += DeltaSeconds;
	if (Flyers.Num() > 0) TickFlyers(FMath::Min(DeltaSeconds, 0.1f));
	if (LeftBodies.Num() > 0) TickLeftBodies(FMath::Min(DeltaSeconds, 0.1f));
	const AOutlawsCoopLink* L = Link.Get();
	AOutlawsSpriteActor* S = Sprite.Get();
	const AOutlawsLevelActor* Lv = Level.Get();
	bLanternLit = false;
	if (!L || L->Pose.Seq <= 0)
	{
		SetActorHiddenInGame(true);
		if (S) S->SetActorHiddenInGame(true);
		return;
	}
	// REQ-MPLOBBY: a player still in the lobby is not in the level (its pose is its pawn at the level's start): nothing of it
	// drawn here, no plate, no lantern; drawn from its own place once it enters (no easing from the start)
	const bool bLobby = UOutlawsCoop::InLobbyFor(this, PlayerIndex);
	if (bLobby != bLobbyHidden)
	{
		bLobbyHidden = bLobby;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby body: the %s %s"), UOutlawsCoop::WhoWord(PlayerIndex),
			bLobby ? TEXT("is in the lobby: its body is not drawn here") : TEXT("entered: its body is drawn here"));
	}
	if (bLobby)
	{
		SetActorHiddenInGame(true);
		if (S) S->SetActorHiddenInGame(true);
		TickPlate(*L, true);
		Samples.Reset();
		bPlaced = false;
		return;
	}
	const FOutlawsCoopPose& P = L->Pose;
	const float Dt = FMath::Min(DeltaSeconds, 0.1f);
	const double Now = FPlatformTime::Seconds();
	// R8: each new pose into the buffer by the sender's time; the place drawn is a tenth of a second behind the newest,
	// between the two around that moment (twenty a second arrive; a late or lost one on a real link does not show)
	if (P.Seq != LastSeq)
	{
		LastSeq = P.Seq;
		const double Sent = P.SentAt > 0.f ? double(P.SentAt) : Now;
		const double Lag = Now - Sent;
		if (!bHaveOffset || Lag < ClockOffset) ClockOffset = Lag;
		else ClockOffset += (Lag - ClockOffset) * 0.02;        // (follows a drift up, slowly; a quicker arrival sets it at once)
		bHaveOffset = true;
		if (Samples.Num() > 0 && (Sent <= Samples.Last().T || FVector::DistSquared(Samples.Last().Body, FVector(P.Body)) > FMath::Square(PoseCutCm)))
		{
			Samples.Reset();                  // (a jump: a respawn, a new level, a teleport; or the sender started again)
			++BufferCuts;
		}
		FPoseSample Sm;
		Sm.T = Sent;
		Sm.Body = P.Body;
		Sm.Head = P.Head;
		Sm.HeadRot = P.HeadRot.Quaternion();
		Sm.Half = P.HalfHeight;
		Sm.Speed = P.GroundSpeed;
		Samples.Add(Sm);
		if (Samples.Num() > 32) Samples.RemoveAt(0, Samples.Num() - 32);
	}
	FPoseSample Want;
	bool bCut = false;
	if (!SamplePose(Now, Want, bCut)) return;
	if (!bPlaced) bCut = true;
	bPlaced = true;
	Body = Want.Body;
	Head = Want.Head;
	HeadRot = Want.HeadRot.Rotator();
	Half = Want.Half;
	// the speed across the ground as the player's own movement has it (the pose's; lightly smoothed between its arrivals)
	Speed = bCut ? Want.Speed : FMath::FInterpTo(Speed, Want.Speed, Dt, 12.f);
	TickDeath(Dt, L->Stats);                   // (BUG-MPDEATHSOUND, BUG-BOTTNT: the cry; a blast's throw moves the body drawn)
	SetActorHiddenInGame(false);
	SetActorLocation(Body);
	// BUG-LANTERNNET: a lit lantern's light where its holder has it, kept on the body as drawn here (the level lights it)
	if (P.Lamp == 2 && !L->Stats.bDead && !(Lv && Lv->IsMoviePlaying()))
	{
		bLanternLit = true;
		LanternAt = Body + (FVector(P.LampAt) - FVector(P.Body));
		LanternOil = P.LampOil / 255.f;
	}
	TickChestPlate(Dt, P, L->Stats, Lv && Lv->IsMoviePlaying());
	if (!S) return;
	// the sprite: hidden with this actor, and while this machine shows the movie between two levels (the other player's
	// place there means nothing); never by its root component's flag, which the portal culling owns
	// (BUG-TNTVANISH: none while this death's body flies or lies on as a body of its own: the comeback is drawn once alive)
	S->SetActorHiddenInGame((Lv && Lv->IsMoviePlaying()) || (bBodyLeft && L->Stats.bDead));
	TickSprite(Dt, P, L->Stats, L->Pace);
#if WITH_OUTLAWS_TESTS
	// (tests, BUG-TNTVANISH) half a second after a comeback whose thrown body went on alone: drawn where its own pose says
	// (a metre and a half), apart from that body, with a sprite of its own on show
	if (ComebackCheckAt > 0.0 && Clock >= ComebackCheckAt)
	{
		ComebackCheckAt = 0.0;
		const FLeftBody* Lb = LeftBodies.Num() > 0 ? &LeftBodies.Last() : nullptr;
		const double OffPose = FVector::Dist(Body, FVector(P.Body));
		const double FromBody = Lb ? FVector::Dist(Body - FVector(0.0, 0.0, Half), Lb->Feet) : -1.0;
		const bool bOk = OffPose < 150.0 && FromBody > 100.0 && !S->IsHidden() && !L->Stats.bDead;
		++ComebacksChecked;
		ComebacksAtSpawn += bOk ? 1 : 0;
		LeftLines.Add(FString::Printf(TEXT("%s back %.0f cm off its pose, %.1f m from its body%s"), *LeftWho, OffPose, FromBody / 100.0, bOk ? TEXT("") : TEXT(" (WRONG)")));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH test tntvanish: %s back at %s, %.0f cm off its pose, %.1f m from its thrown body, sprite %s"), *LeftWho, *Body.ToCompactString(),
			OffPose, FromBody / 100.0, S->IsHidden() ? TEXT("hidden") : TEXT("shown"));
	}
#endif
	TickPlate(*L, (Lv && Lv->IsMoviePlaying()) || UOutlawsCoop::PlateHiddenFor(this, PlayerIndex));     // (M1/M2: none in a free-for-all, teammates' in a team class)
}

// ---- REQ-CHESTPLATE (user 2026-10-15): the boiler plate this player wears ---------------------------------------------

void AOutlawsRemotePlayer::TickChestPlate(float Dt, const FOutlawsCoopPose& P, const FOutlawsCoopStats& St, bool bHide)
{
	// on the chest as its wearer's machine has it: under the head (the neck behind and below the eyes), ahead along the
	// body's yaw, nearly upright; the plate picked up, undented (the dents are its wearer's to see)
	const bool bWant = !bHide && P.PlateSize > 0 && St.Armour > 0 && !St.bDead;
	if (!bWant)
	{
		if (ChestPlate && ChestPlate->IsVisible())
		{
			ChestPlate->SetVisibility(false);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP chest plate: the %s's plate is not drawn here (size %d, armour %d%s)"), UOutlawsCoop::WhoWord(PlayerIndex), P.PlateSize, St.Armour,
				St.bDead ? TEXT(", dead") : bHide ? TEXT(", hidden") : TEXT(""));
		}
		return;
	}
	if (!ChestPlate || ChestPlateBuilt != P.PlateSize)
	{
		if (ChestPlate) ChestPlate->DestroyComponent();
		ChestPlate = nullptr;
		UWorld* World = GetWorld();
		APlayerController* Pc = World ? World->GetFirstPlayerController() : nullptr;
		AOutlawsPlayer* Me = Pc ? Cast<AOutlawsPlayer>(Pc->GetPawn()) : nullptr;
		UProceduralMeshComponent* Pm = nullptr;
		if (!Me || !Me->BuildPlateLook(this, P.PlateSize, Pm) || !Pm) return;
		ChestPlate = Pm;
		ChestPlateBuilt = P.PlateSize;
		bChestYawEased = false;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP chest plate: the %s wears a plate of %d, drawn here"), UOutlawsCoop::WhoWord(PlayerIndex), int32(P.PlateSize));
	}
	ChestYawShown = bChestYawEased ? float(ChestYawShown + FRotator::NormalizeAxis(P.ChestYaw - ChestYawShown) * FMath::Min(1.f, Dt * 12.f)) : P.ChestYaw;
	bChestYawEased = true;
	const FQuat Hq = HeadRot.Quaternion();
	const FVector Neck = Head - Hq.GetForwardVector() * 9.0 - Hq.GetUpVector() * 10.0;
	FVector At = Neck - FVector(0.0, 0.0, 22.0) + FRotator(0.0, ChestYawShown, 0.0).Vector() * 17.0;
	// the sprite is drawn nearer the eye than the body stands (its feet, AOutlawsSpriteActor::SetFeet): the plate is scaled
	// about the eye to just before the sprite's picture, so it looks the same and the picture does not hide it; seen from
	// behind it is the far side of the body and is not drawn
	UWorld* W = GetWorld();
	APlayerController* PC = W ? W->GetFirstPlayerController() : nullptr;
	const FVector Eye = PC && PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : At;
	// turned as the sprite's drawing is: its eight views step by 45 degrees round the line to these eyes, so the plate
	// takes the step the drawing shows (front, or a three-quarter view); side on or from behind it is not drawn
	const float ToEye = float((Eye - At).Rotation().Yaw);
	const float Step = 45.f * FMath::RoundToFloat(float(FRotator::NormalizeAxis(ChestYawShown - ToEye)) / 45.f);
	const bool bFront = FMath::Abs(Step) < 90.f;
	const float PlateYaw = float(FRotator::NormalizeAxis(ToEye + Step));
	double Scale = 1.0;
	const AOutlawsSpriteActor* S = Sprite.Get();
	const FVector View = At - Eye;
	const double Dist = View.Size();
	// (the sprite's picture stands well nearer the eye than its quad's place says: 6 cm before it was still hidden, 80 not)
	static const double Before = [] { double Cm = 60.0; OLFlags::Value(TEXT("OLChestBeforeCm"), Cm); return Cm; }();
	if (S && S->Quad && Dist > 30.0)
	{
		const double ToQuad = FVector::DotProduct(S->Quad->GetComponentLocation() - Eye, View / Dist);
		if (ToQuad - Before < Dist && ToQuad > Before + 14.0)
		{
			Scale = (ToQuad - Before) / Dist;
			At = Eye + View * Scale;
		}
	}
	ChestPlate->SetWorldLocationAndRotation(At, FRotator(12.0, PlateYaw, 0.0));
	ChestPlate->SetWorldScale3D(FVector(Scale));
	if (ChestPlate->IsVisible() != bFront) ChestPlate->SetVisibility(bFront);
	static const bool bNetTest = OLFlags::Param(TEXT("OLChestNetTest"));
	if (bNetTest && Clock >= ChestLogAt)
	{
		ChestLogAt = Clock + 3.0;
		const FVector Rel = FRotator(0.0, ChestYawShown, 0.0).UnrotateVector(At - Head);
		const FProcMeshSection* Sec = ChestPlate->GetProcMeshSection(0);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP chest plate: the %s's plate drawn %s: %.1f cm ahead, %.1f right, %.1f up of the head as drawn (scaled %.2f toward these eyes); yaw %.1f (sent %.1f, drawn %.1f, the drawing's step %.0f); %d corners, bounds %.0f cm, material %s, proxy %s, actor hidden %d, seen %.2f s ago"),
			UOutlawsCoop::WhoWord(PlayerIndex), bFront ? TEXT("facing these eyes") : TEXT("NOT (its back to these eyes)"), Rel.X, Rel.Y, Rel.Z, Scale, ChestYawShown, P.ChestYaw, PlateYaw, Step,
			Sec ? Sec->ProcVertexBuffer.Num() : -1, ChestPlate->Bounds.SphereRadius, ChestPlate->GetMaterial(0) ? *ChestPlate->GetMaterial(0)->GetName() : TEXT("none"),
			ChestPlate->SceneProxy ? TEXT("yes") : TEXT("NO"), IsHidden() ? 1 : 0, W ? float(W->GetTimeSeconds() - ChestPlate->GetLastRenderTimeOnScreen()) : -1.f);
	}
	if (!bFront) return;
	// lit as the bad guys are where it hangs (the held material's brightness, as this player's own hands are)
	if (const AOutlawsLevelActor* Lv = Level.Get())
	{
		const float Light = FMath::Max(0.04f, FMath::Pow(FMath::Clamp(Lv->LightAt(At), 0, 31) / 31.f, 2.2f));
		for (int32 i = 0; i < ChestPlate->GetNumMaterials(); ++i)
		{
			if (UMaterialInstanceDynamic* M = Cast<UMaterialInstanceDynamic>(ChestPlate->GetMaterial(i))) M->SetScalarParameterValue(TEXT("Brightness"), Light);
		}
	}
}

bool AOutlawsRemotePlayer::ChestPlateShown(FVector& OutAt, FRotator& OutRot) const
{
	if (!ChestPlate || !ChestPlate->IsVisible() || IsHidden()) return false;
	OutAt = ChestPlate->GetComponentLocation();
	OutRot = ChestPlate->GetComponentRotation();
	return true;
}

// ---- the death: its cry and a blast's throw (BUG-MPDEATHSOUND, BUG-BOTTNT, user 2026-10-09) ------------------------------

int32 AOutlawsRemotePlayer::DeathCriesHeard = 0;
int32 AOutlawsRemotePlayer::BodiesFlung = 0;
int32 AOutlawsRemotePlayer::BodiesLanded = 0;
TArray<FString> AOutlawsRemotePlayer::CryLines;
TArray<FString> AOutlawsRemotePlayer::FlingLines;

void AOutlawsRemotePlayer::TickDeath(float Dt, const FOutlawsCoopStats& St)
{
	AOutlawsLevelActor* L = Level.Get();
	UWorld* W = GetWorld();
	const AOutlawsCoopLink* K = Link.Get();
	if (St.Seq <= 0 || !L || !W) return;
	const FString Who = K && !K->PlayerName.IsEmpty() ? K->PlayerName : FString::Printf(TEXT("player %d"), PlayerIndex);
	const double Now = FPlatformTime::Seconds();
	if (!St.bDead)
	{
		// BUG-TNTVANISH: still in the air as the player comes back: that body flies on and lies as a corpse of its own (before,
		// it went with the player to the spawn point and vanished from the air)
		if (bFlingOn && bFlying)
		{
			if (LeaveBody(TEXT("its player respawned")))
			{
#if WITH_OUTLAWS_TESTS
				ComebackCheckAt = Clock + 0.5;
#endif
			}
			else
			{
				++BodiesVanished;
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH body of %s (player %d) gone from the air %.1f s on: its player respawned (-OLOldTntVanish)"), *Who, PlayerIndex, Now - FlingAt);
			}
		}
		bBodyLeft = false;
		if (bFlingOn) UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH body of %s up again (the throw's place let go)"), *Who);
		DeadSince = 0.0;
		bAliveSeen = true;
		bFlingDecided = bFlingOn = bFlying = false;
		return;
	}
	if (DeadSince == 0.0)
	{
		// the death as it comes: the character's own cry (its ITM's SND_DIE, sent with the death) from where the body lies
		DeadSince = Now;
		bFlingDecided = !bAliveSeen;             // (first seen dead: already lying where it fell)
		const FOutlawsAssets* A = L->GetLabAssets();
		if (bAliveSeen && !St.DieSound.IsEmpty() && A && A->Has(St.DieSound + TEXT(".wav")))
		{
			L->PlaySoundAt(St.DieSound, Body);
			++DeathCriesHeard;
			CryLines.Add(FString::Printf(TEXT("%s=%s"), *Who, *St.DieSound));
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH death cry heard from %s (player %d): %s.wav"), *Who, PlayerIndex, *St.DieSound);
		}
		else if (bAliveSeen)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH death of %s (player %d): no cry (%s)"), *Who, PlayerIndex, St.DieSound.IsEmpty() ? TEXT("none sent") : *St.DieSound);
		}
	}
	// a blast that killed it (seen go off here from 1.5 s before the death to 1 s after: the death and the blast come apart
	// over a link) throws the body as AOutlawsEnemy::Knock throws a dead bad guy's: up and away at 360 cm/s a point dealt, 0.7
	// up, 5200 at most.  The dying player's own view stays where it fell (BUG-MPDEATHCAM holds its own body there): the body
	// the others see is this one
	if (!bFlingDecided && UOutlawsCoop::IsMatchActive(this))
	{
		for (const AOutlawsLevelActor::FRecentBlast& B : L->RecentBlasts)
		{
			if (B.Time < DeadSince - 1.5) continue;
			const double R = B.RadiusCm;
			const double D = FVector::Dist(B.At, Body);
			if (D >= R || !L->CanSee(B.At, Body, this)) continue;
			const double Fall = D <= R * 0.25 ? 1.0 : FMath::Square(FMath::Max(0.0, (R - D) / (0.75 * R)));
			const float Dealt = float(B.Damage * Fall);
			if (Dealt < 0.05f) continue;
			FVector Away = (Body - B.At).GetSafeNormal2D();
			if (Away.IsNearlyZero()) Away = FRotator(0.0, FacingYaw + 180.0, 0.0).Vector();
			FlingVel = ((Away + FVector(0.0, 0.0, 0.7)).GetSafeNormal() * Dealt * 360.f).GetClampedToMaxSize(5200.f);
			FlingFeet = FlingStart = Body - FVector(0.0, 0.0, Half);
			FlingAt = Now;
			bFlingDecided = bFlingOn = bFlying = true;
			++BodiesFlung;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH body of %s (player %d) thrown by a blast %.1f m off: %.0f cm/s (up %.0f)"), *Who, PlayerIndex, D / 100.0, FlingVel.Size(), FlingVel.Z);
			break;
		}
		if (!bFlingDecided && Now - DeadSince > 1.0) bFlingDecided = true;
	}
	if (!bFlingOn) return;
	if (bFlying)
	{
		if (!StepFling(FlingFeet, FlingVel, Dt, Sprite.Get())) bFlying = false;
		if (Now - FlingAt > 6.0) bFlying = false;
		if (!bFlying)
		{
			++BodiesLanded;
			const double Far = FVector::Dist2D(FlingStart, FlingFeet) / 100.0;
			FlingLines.Add(FString::Printf(TEXT("%s=%.1f m"), *Who, Far));
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH body of %s (player %d) landed %.1f m from where it fell (%.1f s in the air, %.1f m down)"), *Who, PlayerIndex,
				Far, Now - FlingAt, (FlingStart.Z - FlingFeet.Z) / 100.0);
		}
	}
	Body = FlingFeet + FVector(0.0, 0.0, Half);
}

bool AOutlawsRemotePlayer::StepFling(FVector& Feet, FVector& Vel, float Dt, const AActor* Ignore) const
{
	// under the world's gravity, meeting the level: a floor stops it, a wall turns it back at a third of its speed
	UWorld* W = GetWorld();
	if (!W) return false;
	const float Step = FMath::Min(Dt, 0.05f);
	Vel.Z += W->GetGravityZ() * Step;
	const FVector From = Feet + FVector(0.0, 0.0, 2.0);
	const FVector To = From + Vel * Step;
	FHitResult Hit;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsBodyThrow), false, this);
	if (Ignore) Q.AddIgnoredActor(Ignore);
	if (W->LineTraceSingleByChannel(Hit, From, To, ECC_Visibility, Q))
	{
		if (Hit.ImpactNormal.Z > 0.5)
		{
			Feet = FVector(Hit.ImpactPoint);
			Vel = FVector::ZeroVector;
			return false;
		}
		Feet = FVector(Hit.ImpactPoint) + FVector(Hit.ImpactNormal) * 5.0 - FVector(0.0, 0.0, 2.0);
		Vel = Vel.MirrorByVector(FVector(Hit.ImpactNormal)) * 0.3;
		return true;
	}
	Feet = To - FVector(0.0, 0.0, 2.0);
	return true;
}

// ---- BUG-TNTVANISH (v0.9 known issue): a thrown body that outlives its player's death --------------------------------------

int32 AOutlawsRemotePlayer::BodiesLeft = 0;
int32 AOutlawsRemotePlayer::BodiesLeftBots = 0;
int32 AOutlawsRemotePlayer::BodiesVanished = 0;
int32 AOutlawsRemotePlayer::BodiesLeftLanded = 0;
int32 AOutlawsRemotePlayer::BodiesLeftUnseen = 0;
int32 AOutlawsRemotePlayer::ComebacksAtSpawn = 0;
int32 AOutlawsRemotePlayer::ComebacksChecked = 0;
TArray<FString> AOutlawsRemotePlayer::LeftLines;

bool AOutlawsRemotePlayer::LeaveBody(const TCHAR* Why)
{
	static const bool bOld = OLFlags::Param(TEXT("OLOldTntVanish"));
	AOutlawsSpriteActor* S = Sprite.Get();
	if (bOld || !S || !bFlingOn || !bFlying) return false;
	// two kept for a player (the oldest goes: a body flies on only when its player came back within its flight)
	while (LeftBodies.Num() >= 2)
	{
		if (AOutlawsSpriteActor* O = LeftBodies[0].S.Get()) O->Destroy();
		LeftBodies.RemoveAt(0);
	}
	FLeftBody& Lb = LeftBodies.AddDefaulted_GetRef();
	Lb.S = S;
	Lb.Feet = FlingFeet;
	Lb.Vel = FlingVel;
	Lb.Start = FlingStart;
	Lb.At = FlingAt;
	S->CustomTimeDilation = 1.f;              // (its fall and lying at the art's own rate)
	Sprite.Reset();
	bSpriteGiven = true;
	bBodyLeft = true;
	bFlingOn = bFlying = false;
	const AOutlawsCoopLink* K = Link.Get();
	LeftWho = K && !K->PlayerName.IsEmpty() ? K->PlayerName : FString::Printf(TEXT("player %d"), PlayerIndex);
	++BodiesLeft;
	BodiesLeftBots += K && K->bBot ? 1 : 0;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH body of %s (player %d) still in the air %.1f s on as %s: it flies on as a body of its own"), *LeftWho, PlayerIndex,
		FPlatformTime::Seconds() - FlingAt, Why);
	return true;
}

void AOutlawsRemotePlayer::TickLeftBodies(float Dt)
{
	const AOutlawsLevelActor* Lv = Level.Get();
	const bool bMovie = Lv && Lv->IsMoviePlaying();
	const double Now = FPlatformTime::Seconds();
	for (int32 i = LeftBodies.Num() - 1; i >= 0; --i)
	{
		FLeftBody& Lb = LeftBodies[i];
		AOutlawsSpriteActor* S = Lb.S.Get();
		if (!S) { LeftBodies.RemoveAt(i); continue; }           // (a build of the level cleared it)
		if (Lb.bFlying)
		{
			if (!StepFling(Lb.Feet, Lb.Vel, Dt, S) || Now - Lb.At > 6.0)
			{
				Lb.bFlying = false;
				++BodiesLanded;
				++BodiesLeftLanded;
				const double Far = FVector::Dist2D(Lb.Start, Lb.Feet) / 100.0;
				FlingLines.Add(FString::Printf(TEXT("%s=%.1f m"), *LeftWho, Far));
				LeftLines.Add(FString::Printf(TEXT("%s landed %.1f m off"), *LeftWho, Far));
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH body of %s (player %d, flown on alone) landed %.1f m from where it fell (%.1f s in the air, %.1f m down)"), *LeftWho, PlayerIndex,
					Far, Now - Lb.At, (Lb.Start.Z - Lb.Feet.Z) / 100.0);
			}
			else if (S->IsHidden() && !bMovie) ++BodiesLeftUnseen;
		}
		S->SetActorLocation(Lb.Feet + FVector(0.0, 0.0, 0.5));
		S->SetActorHiddenInGame(bMovie);
		Lb.LightCheck -= Dt;
		if (Lb.LightCheck <= 0.f && Lv)
		{
			Lb.LightCheck = 0.2f;
			S->SetLightLevel(Lv->LightAt(Lb.Feet + FVector(0.0, 0.0, 5.0)));
		}
	}
}

// ---- the name and health bar over the head (user 2026-10-02) ---------------------------------------------------------------

FString AOutlawsRemotePlayer::PlateLabel(const FString& Name, const FString& Character)
{
	if (Name.IsEmpty()) return Name;
	// the user's initials (2026-10-05): Dr. Death D, 'Bloody' Mary M, Bob Graham B, Two-Feathers T, Spit'n Jack S, James Anderson J
	static const TCHAR* Shorts[][2] = { { TEXT("Doc"), TEXT("D") }, { TEXT("Mar"), TEXT("M") }, { TEXT("Bob"), TEXT("B") },
		{ TEXT("Two"), TEXT("T") }, { TEXT("San"), TEXT("S") }, { TEXT("Jam"), TEXT("J") } };
	for (const auto& E : Shorts) if (Character.Equals(E[0], ESearchCase::IgnoreCase)) return FString::Printf(TEXT("%s (%s)"), *Name, E[1]);
	return Name;
}

int32 AOutlawsRemotePlayer::HealthBand(float Fraction)
{
	// "green for full, orange for mid, red for bad": green over two thirds, red under one third, orange between
	return Fraction > 0.66f ? 0 : Fraction >= 0.33f ? 1 : 2;
}

const TCHAR* AOutlawsRemotePlayer::BandName(int32 Band)
{
	return Band == 0 ? TEXT("green") : Band == 1 ? TEXT("orange") : TEXT("red");
}

bool AOutlawsRemotePlayer::VoiceActiveNow() const
{
	// the voice chat's word (it holds it 0.3 s past the last sound itself: OutlawsCoopVoice.cpp)
	return UOutlawsCoop::IsRemoteTalking(this, PlayerIndex);
}

FString AOutlawsRemotePlayer::PlateState() const
{
	return FString::Printf(TEXT("%s, health bar %d %% (%s%s), speaker %s"), ShownName.IsEmpty() ? TEXT("no name line") : *FString::Printf(TEXT("name '%s'"), *ShownName),
		FMath::RoundToInt(ShownFrac * 100.f), BandName(ShownBand), bShownDead ? TEXT(", dead: empty") : TEXT(""), bTalkShown ? TEXT("shown") : TEXT("hidden"));
}

void AOutlawsRemotePlayer::BuildPlate()
{
	// every part drawn with the depth test, as the level is (walls in front hide it): the bar's black ground and its fill are
	// the level's unlit world material in a flat colour, the name the wrist's unlit text material (the HUD's cream, a black
	// shadow behind), the speaker a picture made here on the level's masked material.  All of them draw on the headset's
	// mobile renderer as the level does.
	PlateRoot = NewObject<USceneComponent>(this, TEXT("Nameplate"));
	PlateRoot->SetupAttachment(Root);
	PlateRoot->SetUsingAbsoluteLocation(true);
	PlateRoot->SetUsingAbsoluteRotation(true);
	PlateRoot->SetUsingAbsoluteScale(true);
	PlateRoot->RegisterComponent();
	UStaticMesh* PlaneMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	const FRotator Facing = PlateFacing();
	auto Quad = [this, PlaneMesh, &Facing](UMaterialInterface* M, const TCHAR* Name)
	{
		UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this, Name);
		C->SetStaticMesh(PlaneMesh);
		if (M) C->SetMaterial(0, M);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetGenerateOverlapEvents(false);
		C->SetCastShadow(false);
		C->SetupAttachment(PlateRoot);
		C->SetRelativeRotation(Facing);
		C->RegisterComponent();
		return C;
	};
	BarBack = Quad(Flat(FColor(0, 0, 0)), TEXT("NameplateBarBack"));
	FillMats.Reset();
	FillMats.Add(Flat(FColor(70, 205, 60)));
	FillMats.Add(Flat(FColor(245, 145, 25)));
	FillMats.Add(Flat(FColor(225, 35, 25)));
	BarFill = Quad(FillMats[0], TEXT("NameplateBarFill"));
	UMaterialInterface* MaskBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_WorldMasked.M_OLR_WorldMasked"));
	UMaterialInstanceDynamic* IconMat = MaskBase ? UMaterialInstanceDynamic::Create(MaskBase, this) : nullptr;
	if (IconMat)
	{
		if (UTexture2D* T = OutlawsTextures::MakeTextureBGRA(64, 64, PlateSpeakerPixels(64), false, TF_Bilinear, TA_Clamp))
		{
			IconMat->SetTextureParameterValue(TEXT("Tex"), T);
			KeepAlive.Add(T);
		}
		KeepAlive.Add(IconMat);
	}
	TalkIcon = Quad(IconMat, TEXT("NameplateSpeaker"));
	TalkIcon->SetRelativeScale3D(FVector(PlateIcon / 100.0, PlateIcon / 100.0, 1.0));
	UMaterialInterface* TextMat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_Text.M_OLR_Text"));
	auto Text = [this, TextMat](const TCHAR* Name, const FColor& Colour)
	{
		UTextRenderComponent* T = NewObject<UTextRenderComponent>(this, Name);
		T->SetHorizontalAlignment(EHTA_Center);
		T->SetVerticalAlignment(EVRTA_TextBottom);
		T->SetWorldSize(PlateNameSize);
		T->SetTextRenderColor(Colour);
		if (TextMat) T->SetTextMaterial(TextMat);
		T->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		T->SetCastShadow(false);
		T->SetText(FText::GetEmpty());
		T->SetupAttachment(PlateRoot);
		T->RegisterComponent();
		return T;
	};
	NameShadow = Text(TEXT("NameplateShadow"), FColor(0, 0, 0));
	NameText = Text(TEXT("NameplateName"), FColor(245, 230, 190));
	const float NameZ = PlateBarH * 0.5f + 1.5f;
	NameText->SetRelativeLocation(FVector(0.0, 0.0, NameZ));
	NameShadow->SetRelativeLocation(FVector(-0.3, -0.45, NameZ - 0.45));       // (down and to the viewer's right)
	bPlateFresh = true;
}

void AOutlawsRemotePlayer::TickPlate(const AOutlawsCoopLink& L, bool bHide)
{
	if (!PlateRoot) BuildPlate();
	if (!PlateRoot || !BarBack || !BarFill || !TalkIcon || !NameText || !NameShadow) return;
	const FOutlawsCoopStats& St = L.Stats;
	const bool bDead = St.Seq > 0 && St.bDead;
	const float MaxH = St.MaxHealth > 0.f ? St.MaxHealth : 10.f;
	const float Frac = bDead ? 0.f : FMath::Clamp(St.Health / MaxH, 0.f, 1.f);
	const int32 Band = bDead ? 2 : HealthBand(Frac);
	FString Name = PlateLabel(AOutlawsLevelActor::CleanMpName(L.PlayerName), L.Character);
	if (L.bAppPaused) Name = Name.IsEmpty() ? FString(TEXT("(paused)")) : Name + TEXT(" (paused)");     // (BUG-COOPTIMEOUT: its place held)
	// the speaker: while their voice comes through (the voice chat holds it 0.3 s past the last sound); a test's forced talk
	// is held the same here
	if (bTestTalk) TalkUntil = Clock + PlateTalkHold;
	const bool bTalk = VoiceActiveNow() || Clock < TalkUntil;
	// the parts change only when what they show does
	if (Name != ShownName || bPlateFresh)
	{
		NameText->SetText(FText::FromString(Name));
		NameShadow->SetText(FText::FromString(Name));
	}
	if ((Band != ShownBand || bPlateFresh) && FillMats.IsValidIndex(Band)) BarFill->SetMaterial(0, FillMats[Band]);
	bPlateFresh = false;
	ShownName = Name;
	ShownBand = Band;
	ShownFrac = Frac;
	bTalkShown = bTalk;
	bShownDead = bDead;
	const bool bName = !Name.IsEmpty();
	// (user 2026-10-03) the name in the ammo count's own font, on one of this machine's player's text plates; the text
	// components only measure it now (where the speaker goes)
	NameText->SetVisibility(false);
	NameShadow->SetVisibility(false);
	if (AOutlawsPlayer* Me = GetWorld() && GetWorld()->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(GetWorld()->GetFirstPlayerController()->GetPawn()) : nullptr)
	{
		Me->CoopNamePlate(PlateRoot, FVector(0.0, 0.0, PlateBarH * 0.5 + 1.5 + 6.0));
		// (that plate draws over the world, as the gun's count does: hidden while a wall stands between the eyes and it, as
		// the depth test hides the bar)
		bool bClear = true;
		if (bName && !bHide)
		{
			FVector Eye;
			FRotator Unused;
			if (const APlayerController* PC = GetWorld()->GetFirstPlayerController()) PC->GetPlayerViewPoint(Eye, Unused);
			FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsNameplateSight), false, this);
			Q.AddIgnoredActor(Me);
			if (AOutlawsSpriteActor* Sp = Sprite.Get()) Q.AddIgnoredActor(Sp);
			FHitResult Hit;
			bClear = !GetWorld()->LineTraceSingleByChannel(Hit, Eye, PlateRoot->GetComponentLocation(), ECC_Visibility, Q);
		}
		Me->CoopNamePlateText(Name, !bHide && bName && bClear);
	}
	BarBack->SetVisibility(!bHide);
	BarFill->SetVisibility(!bHide && Frac > 0.005f);
	TalkIcon->SetVisibility(!bHide && bTalk);
	// the bar: a black ground, the fill from its left end (as the viewer sees it) in from the border
	const float InW = PlateBarW - 2.f * PlateBorder, InH = PlateBarH - 2.f * PlateBorder;
	BarBack->SetRelativeScale3D(FVector(PlateBarW / 100.0, PlateBarH / 100.0, 1.0));
	BarFill->SetRelativeScale3D(FVector(FMath::Max(0.01f, InW * Frac) / 100.0, InH / 100.0, 1.0));
	BarFill->SetRelativeLocation(FVector(0.3, InW * 0.5f * (1.f - Frac), 0.0));
	// the speaker beside the name, to the viewer's right (beside the bar when there is no name)
	if (bTalk)
	{
		const FVector Size = bName ? NameText->GetTextLocalSize() : FVector::ZeroVector;
		const double Right = bName ? Size.Y * 0.5 + 1.5 + PlateIcon * 0.5 : PlateBarW * 0.5 + 1.5 + PlateIcon * 0.5;
		const double Up = bName ? PlateBarH * 0.5 + 1.5 + Size.Z * 0.5 : 0.0;
		TalkIcon->SetRelativeLocation(FVector(0.2, -Right, Up));
	}
	// over the hat and the rifle held up past it (the sprite stands about 2 m, the barrel to about 2.3), lower crouched, just over the body lying dead; turned (yaw) to this machine's
	// eyes, a little bigger further off so it stays readable (the same size up to about 4 m, twice it at 13 m)
	const FVector Feet(Body.X, Body.Y, Body.Z - Half);
	const double Top = bDead ? 70.0 : (Shown == EAnim::Crouch || Shown == EAnim::CrouchWalk) ? 165.0 : 238.0;
	FVector Eye = Head;
	if (const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		FRotator Unused;
		PC->GetPlayerViewPoint(Eye, Unused);
	}
	const FVector Base = Feet + FVector(0.0, 0.0, Top);
	const double Dist = FVector::Dist(Eye, Base);
	const double Scale = FMath::Clamp(0.6 + Dist / 900.0, 1.0, 2.2) * FMath::Clamp(CVarOLPlateScale.GetValueOnGameThread(), 0.25f, 4.f);
	const FVector At = Base + FVector(0.0, 0.0, (PlateBarH * 0.5 + 6.0) * Scale);
	const FVector ToEye = Eye - At;
	PlateRoot->SetWorldLocationAndRotation(At, FRotator(0.0, ToEye.IsNearlyZero() ? 0.0 : ToEye.Rotation().Yaw, 0.0));
	PlateRoot->SetWorldScale3D(FVector(Scale));
	// said in the log when it changes (tests read it)
	const FString State = PlateState();
	if (State != LoggedPlate)
	{
		LoggedPlate = State;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP nameplate of the %s: %s (health %.1f of %.0f)"), UOutlawsCoop::WhoWord(PlayerIndex), *State, St.Health, MaxH);
	}
}
