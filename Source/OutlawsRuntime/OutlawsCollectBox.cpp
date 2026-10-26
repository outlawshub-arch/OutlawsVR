// REQ-COLLECTBOX (user 13 Oct; planner 21 Oct: build it with our own spots, the box art and the rewards later): one hidden box in
// each of the nine main story levels.  The box is a plain placeholder (a cardboard box with a label, drawn here: nothing of the
// game's or anyone's art), standing upright a little over the floor, turning slowly, with a glint every couple of seconds so it
// can be spotted.  This machine's player takes it by walking into it, or on the headset by closing a hand on it, as other
// pickups are taken; it flies to the waist as they do.  Found, it is kept on this profile (OutlawsAchievements::NoteBoxFound:
// the box achievements and the Extras shelf) and never placed again on that profile, whatever is saved, loaded or restarted.
// Co-op: each machine places its own player's box and keeps its own profile; a box one player takes stays for the other until
// they take it themselves.  Not in matches, the tutorial, Historical Missions, or a changed level (its sector count differs).
// The Randomizer moves pickups, not the boxes.  -OLNoCollectBox: none placed.
//
// The spots (GBoxSpots, chosen from the levels' own data by an offline scan of their sectors and objects): each a side room cut off from the rest of its level by
// one opening (no way through it), holding the original's own pickups (a player is meant to get there), off the way from the
// start to the level's outlaw, out of any sector that shares space with another (the pick by the feet is that sector alone) and
// out of the mill's zone gate.  The test: -OLShots -OLNoFrontEnd -OLCollectBoxTest[=vr] (ShotTests/ShotTest_OLCollectBoxTest.cpp).

#include "OutlawsLevelActor.h"
#include "OutlawsLevel.h"
#include "OutlawsAchievements.h"
#include "OutlawsCoop.h"
#include "OutlawsFlags.h"
#include "OutlawsGeometry.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsTextures.h"
#include "ProceduralMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"

namespace
{
	struct FBoxSpot
	{
		const TCHAR* Level;
		int32 NumSectors;        // the level as shipped (a changed level gets no box)
		int32 Sector;
		double X, Y, Z;          // the box's feet, Outlaws units (Y the floor)
		double FX, FY, FZ;       // the walk test's start, just outside the side room
		const TCHAR* Why;
	};
	const FBoxSpot GBoxSpots[] =
	{
		{ TEXT("hideout"), 435, 340, 617.38, -2.00, 204.38, 634.00, -2.00, 230.00,
			TEXT("the far end of the cow room off the east yard (one way in), past its oil, canteen and ammunition") },
		{ TEXT("townex"), 635, 488, -31.38, 0.25, 223.12, -49.00, 4.25, 240.00,
			TEXT("the far corner of the apothecary's front room by its counter, a step down off the west street (one way in)") },
		{ TEXT("train"), 689, 399, 244.12, 3.00, 453.88, 247.50, 0.00, 457.75,
			TEXT("up on a seat bay off a car's aisle, beside its elixir, out of the way along the train") },
		{ TEXT("canyon"), 191, 132, 229.88, -46.00, 636.88, 178.39, -47.00, 677.96,
			TEXT("the far end of the mine tunnels behind the canyon's secret passage (a shotgun, ammunition and oil along the way)") },
		{ TEXT("mill"), 611, 517, 409.62, -3.00, -12.62, 383.00, 5.00, 24.50,
			TEXT("the far end of a barrel store (one way in), past its ammunition and canteens; away from the zone gate's sectors") },
		{ TEXT("simms"), 332, 208, 88.12, -269.00, -80.38, 72.67, -270.44, -74.65,
			TEXT("the end of a winding passage under the fort, past its elixir and oil") },
		{ TEXT("miner"), 504, 329, 603.62, -105.00, 369.88, 593.04, -100.00, 347.76,
			TEXT("the back of a side chamber off the east tunnels (one way in), behind its bad guys' ammunition, elixir and documents") },
		{ TEXT("cliff"), 370, 233, 91.06, 71.00, -121.11, 78.05, 71.00, -96.01,
			TEXT("the back of a small dwelling up on the cliff (one way in), past its ammunition and canteens") },
		{ TEXT("ranch"), 684, 519, 415.62, -14.00, 357.38, 394.50, -14.00, 362.00,
			TEXT("the back of a cellar store under the ranch (one way in), behind its barrels and canteens") },
	};

	const FBoxSpot* SpotOf(const FString& Level)
	{
		for (const FBoxSpot& S : GBoxSpots) if (Level.Equals(S.Level, ESearchCase::IgnoreCase)) return &S;
		return nullptr;
	}

	// the box: upright, as a game box stands on a shelf (cm)
	constexpr double BoxW = 26.0, BoxH = 32.0, BoxD = 8.0, BoxHover = 6.0;
	constexpr float BoxSpinDeg = 35.f, GlintEvery = 2.4f, GlintFor = 0.35f;

	// its picture: cardboard with a darker edge, a cream label with a five-pointed star on the front; the side strip plain
	UTexture2D* BoxTexture()
	{
		static TWeakObjectPtr<UTexture2D> Made;
		if (Made.IsValid()) return Made.Get();
		constexpr int32 W = 64, H = 64, Front = 48;
		TArray<FColor> Px;
		Px.SetNumUninitialized(W * H);
		const FColor Card(146, 104, 58), Edge(92, 60, 30), Label(226, 208, 168), Star(120, 40, 24);
		const FVector2D C(Front * 0.5, H * 0.48);
		for (int32 y = 0; y < H; ++y)
		{
			for (int32 x = 0; x < W; ++x)
			{
				FColor P = Card;
				const bool bSide = x >= Front;
				const int32 lx = bSide ? x - Front : x, lw = bSide ? W - Front : Front;
				if (lx < 2 || lx >= lw - 2 || y < 2 || y >= H - 2) P = Edge;
				else if (!bSide && x >= 8 && x < Front - 8 && y >= 12 && y < H - 12)
				{
					P = Label;
					// the star: inside the five-pointed outline (13 texels out at a point, 5.2 between two)
					const FVector2D D(x + 0.5 - C.X, y + 0.5 - C.Y);
					const double A = FMath::Fmod(FMath::Atan2(D.X, -D.Y) + 2.0 * PI, 2.0 * PI / 5.0) / (2.0 * PI / 5.0);
					const double Off = FMath::Min(A, 1.0 - A) * 2.0;      // 0 along a point, 1 halfway to the next
					if (D.Size() <= FMath::Lerp(13.0, 5.2, Off)) P = Star;
				}
				else if (bSide && y >= 12 && y < H - 12 && (y / 4) % 2 == 0 && lx >= 5 && lx < lw - 5) P = Label;
				// a little grain so it reads as card, not plastic
				const int32 N = int32((HashCombine(GetTypeHash(x), GetTypeHash(y * 131)) % 13u)) - 6;
				P.R = uint8(FMath::Clamp(int32(P.R) + N, 0, 255));
				P.G = uint8(FMath::Clamp(int32(P.G) + N, 0, 255));
				P.B = uint8(FMath::Clamp(int32(P.B) + N, 0, 255));
				Px[y * W + x] = P;
			}
		}
		UTexture2D* T = OutlawsTextures::MakeTextureBGRA(W, H, Px, false, TF_Nearest, TA_Clamp);
		if (T) T->AddToRoot();       // (one for the run: every level's box shares it)
		Made = T;
		return T;
	}
}

bool AOutlawsLevelActor::RoutePlayerDoor(int32 Sector) const
{
	// the player works any door (an automatic one, or an INF door or lift, a key's with its key)
	if (!Inf || !Inf->Live.Sectors.IsValidIndex(Sector)) return false;
	return (Inf->Live.Sectors[Sector].Flags & OutlawsFlags::SectorDoor) != 0 || Inf->ElevatorsBySector.Contains(Sector);
}

bool AOutlawsLevelActor::CollectBoxSpot(const FString& Level, FVector& OutFeet, FVector& OutWalkFrom, int32& OutSector, FString* OutWhy)
{
	const FBoxSpot* S = SpotOf(Level);
	if (!S) return false;
	OutFeet = FOutlawsGeometry::ToUE(S->X, S->Y, S->Z);
	OutWalkFrom = FOutlawsGeometry::ToUE(S->FX, S->FY, S->FZ);
	OutSector = S->Sector;
	if (OutWhy) *OutWhy = S->Why;
	return true;
}

FString AOutlawsLevelActor::PlaceCollectBox(const FOutlawsLevel& Lv)
{
	if (AActor* Old = CollectBoxActor.Get()) Old->Destroy();
	CollectBoxActor.Reset();
	CollectBoxGlint.Reset();
	CollectBoxSafe.Reset();
	UWorld* World = GetWorld();
	const FBoxSpot* S = SpotOf(LevelName);
	if (!World || !World->IsGameWorld() || bFrontEnd) return TEXT("collectible box: none (no game)");
	if (!S) return TEXT("collectible box: none (not a story level)");
	if (bTutorial || bHom || UOutlawsCoop::IsMatchActive(this)) return TEXT("collectible box: none (the tutorial, Historical Missions or a match)");
	if (OLFlags::Param(TEXT("OLNoCollectBox"))) return TEXT("collectible box: none (-OLNoCollectBox)");
	if (Lv.Sectors.Num() != S->NumSectors || !Lv.Sectors.IsValidIndex(S->Sector))
	{
		return FString::Printf(TEXT("collectible box: none (a changed %s: %d sectors, %d as shipped)"), S->Level, Lv.Sectors.Num(), S->NumSectors);
	}
	if (OutlawsAchievements::BoxFound(this, LevelName)) return FString::Printf(TEXT("collectible box: %s's found already on this profile, not placed"), S->Level);
	const FOutlawsSector& Sec = Lv.Sectors[S->Sector];
	const FVector2D Plan(S->X, S->Z);
	const double Floor = Lv.SurfaceY(Sec, Plan, true);
	const FVector Feet = FOutlawsGeometry::ToUE(Plan, Floor);
	// the sector the feet pick (a load, a restart or a jump puts the player by that pick), shared space, the zone gate
	const int32 Picked = Lv.Which3D(S->X, Floor + 0.2, S->Z);
	bool bGate = false;
	for (const FZoneGate& G : ZoneGates) bGate |= G.Gate == S->Sector || G.Off[0].Contains(S->Sector) || G.Off[1].Contains(S->Sector);
	const bool bShared = OverlapParts.Contains(S->Sector);
	CollectBoxSafe = FString::Printf(TEXT("sector %d, the feet pick %d, %s, %s"), S->Sector, Picked, bShared ? TEXT("SHARES SPACE") : TEXT("its own space"),
		bGate ? TEXT("IN A ZONE GATE") : TEXT("no zone gate"));

	FActorSpawnParameters Sp;
	Sp.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* A = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(Feet), Sp);
	if (!A) return TEXT("collectible box: none (could not spawn)");
	USceneComponent* Root = NewObject<USceneComponent>(A, TEXT("Root"), RF_Transient);
	A->SetRootComponent(Root);
	Root->RegisterComponent();
	A->SetActorLocation(Feet);
	SpawnedActors.Add(A);

	// the box: six faces, each shaded by its own normal against a fixed key light (as the item models are, in the unlit world
	// material), brighter than its sector a little so a dark room still shows it
	const FVector LightDir = FVector(0.35, -0.45, 0.82).GetSafeNormal();
	const float Bright = FMath::Max(0.5f, FMath::Clamp(FOutlawsLevel::LightLevel(Sec.Ambient), 0, 31) / 31.f);
	TArray<FVector> Pos, Nrm;
	TArray<FVector2D> UV;
	TArray<FLinearColor> Col;
	TArray<int32> Tri;
	const FVector E(BoxD * 0.5, BoxW * 0.5, BoxH * 0.5);
	const FVector Mid(0.0, 0.0, BoxHover + BoxH * 0.5);
	auto Face = [&](const FVector& N, const FVector& U, const FVector& V, bool bFront)
	{
		// corners at Mid + N*e + (+-)U*e + (+-)V*e; UVs: the front art (u 0..0.75) or the side strip (0.75..1)
		const double En = FMath::Abs(FVector::DotProduct(N, E)), Eu = FMath::Abs(FVector::DotProduct(U, E)), Ev = FMath::Abs(FVector::DotProduct(V, E));
		const int32 B0 = Pos.Num();
		const float U0 = bFront ? 0.f : 0.75f, U1 = bFront ? 0.75f : 1.f;
		const float Lit = FMath::SmoothStep(0.12f, 0.24f, float(FVector::DotProduct(N, LightDir)));
		const float Shade = Bright * FMath::Lerp(0.66f, 1.f, Lit);
		const FVector2D Q[4] = { FVector2D(-1, -1), FVector2D(1, -1), FVector2D(1, 1), FVector2D(-1, 1) };
		for (const FVector2D& K : Q)
		{
			Pos.Add(Mid + N * En + U * (Eu * K.X) + V * (Ev * K.Y));
			Nrm.Add(N);
			UV.Add(FVector2D(FMath::Lerp(U0, U1, (K.X + 1.f) * 0.5f), 1.f - (K.Y + 1.f) * 0.5f));
			Col.Add(FLinearColor(Shade, Shade, Shade, 1.f));
		}
		// wound so the face looks out along N (the world material draws one side)
		const bool bFlip = FVector::DotProduct(FVector::CrossProduct(U, V), N) > 0.0;
		if (bFlip) Tri.Append({ B0, B0 + 2, B0 + 1, B0, B0 + 3, B0 + 2 });
		else Tri.Append({ B0, B0 + 1, B0 + 2, B0, B0 + 2, B0 + 3 });
	};
	Face(FVector(1, 0, 0), FVector(0, -1, 0), FVector(0, 0, 1), true);     // the front
	Face(FVector(-1, 0, 0), FVector(0, 1, 0), FVector(0, 0, 1), true);     // the back
	Face(FVector(0, 1, 0), FVector(1, 0, 0), FVector(0, 0, 1), false);
	Face(FVector(0, -1, 0), FVector(-1, 0, 0), FVector(0, 0, 1), false);
	Face(FVector(0, 0, 1), FVector(0, 1, 0), FVector(1, 0, 0), false);
	Face(FVector(0, 0, -1), FVector(0, 1, 0), FVector(-1, 0, 0), false);
	UProceduralMeshComponent* M = NewObject<UProceduralMeshComponent>(A, TEXT("BoxMesh"), RF_Transient);
	M->bUseAsyncCooking = false;
	M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	M->SetCastShadow(false);
	M->SetupAttachment(Root);
	M->RegisterComponent();
	M->CreateMeshSection_LinearColor(0, Pos, Tri, Nrm, UV, Col, TArray<FProcMeshTangent>(), false);
	static TWeakObjectPtr<UMaterialInterface> ItemBase;
	if (!ItemBase.IsValid()) ItemBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_ItemModel.M_OLR_ItemModel"));
	UMaterialInterface* Base = ItemBase.IsValid() ? ItemBase.Get() : WorldMaskedMaterial.Get();
	if (Base)
	{
		UMaterialInstanceDynamic* Mat = UMaterialInstanceDynamic::Create(Base, A);
		if (UTexture2D* T = BoxTexture()) Mat->SetTextureParameterValue(TEXT("Tex"), T);
		M->SetMaterial(0, Mat);
	}
	M->SetRenderCustomDepth(true);          // the ink outline's world-item stencil (as the item models)
	M->SetCustomDepthStencilValue(4);

	// the glint: a small warm spark at a top corner, now and then
	if (UStaticMesh* Ball = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere")))
	{
		UStaticMeshComponent* G = NewObject<UStaticMeshComponent>(A, TEXT("Glint"), RF_Transient);
		G->SetStaticMesh(Ball);
		G->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		G->SetCastShadow(false);
		G->SetupAttachment(M);
		G->RegisterComponent();
		G->SetRelativeLocation(Mid + FVector(BoxD * 0.5 + 0.5, BoxW * 0.5 - 2.0, BoxH * 0.5 - 2.0));
		G->SetRelativeScale3D(FVector(0.001));
		if (UMaterialInterface* GlowBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_FxGlow.M_OLR_FxGlow")))
		{
			UMaterialInstanceDynamic* Gm = UMaterialInstanceDynamic::Create(GlowBase, A);
			Gm->SetVectorParameterValue(TEXT("Color"), FLinearColor(1.f, 0.93f, 0.62f));
			Gm->SetScalarParameterValue(TEXT("Glow"), 4.f);
			G->SetMaterial(0, Gm);
		}
		CollectBoxGlint = G;
	}
	CollectBoxActor = A;
	CollectBoxRest = Feet;
	CollectBoxTime = float(FMath::Frac(S->X * 0.37));         // (each level's glint and turn start at their own phase)
	UE_LOG(LogOutlaws, Log, TEXT("OLBOX placed in %s at %s (%s): %s"), S->Level, *Feet.ToCompactString(), *CollectBoxSafe, S->Why);
	return FString::Printf(TEXT("collectible box: placed at %s (%s)"), *Feet.ToCompactString(), *CollectBoxSafe);
}

void AOutlawsLevelActor::UpdateCollectBox(float Dt)
{
	AActor* A = CollectBoxActor.Get();
	if (!A) return;
	CollectBoxTime += Dt;
	// a slow turn and a gentle bob; the glint a quick swell and fade every GlintEvery seconds
	A->SetActorLocationAndRotation(CollectBoxRest + FVector(0.0, 0.0, 2.0 * FMath::Sin(CollectBoxTime * 2.1)), FRotator(0.0, FMath::Fmod(CollectBoxTime * BoxSpinDeg, 360.f), 0.0));
	if (UStaticMeshComponent* G = CollectBoxGlint.Get())
	{
		const float Ph = FMath::Fmod(CollectBoxTime, GlintEvery);
		const float K = Ph < GlintFor ? FMath::Sin(PI * Ph / GlintFor) : 0.f;
		G->SetRelativeScale3D(FVector(FMath::Max(0.001f, 0.045f * K)));
	}
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	AOutlawsPlayer* P = PC ? Cast<AOutlawsPlayer>(PC->GetPawn()) : nullptr;
	if (!P || bDead) return;
	const UCapsuleComponent* Cap = P->GetCapsuleComponent();
	const double R = Cap ? Cap->GetScaledCapsuleRadius() : 40.0, Half = Cap ? Cap->GetScaledCapsuleHalfHeight() : 90.0;
	const FVector Body = P->GetActorLocation();
	const double Feet = Body.Z - Half;
	// walked into: across within the body's radius and a little of the box's width (the VR feet rule's reach, 45 cm at least),
	// the box between a step under the feet and the chest; a raised one (a seat, a shelf) with nothing between the chest and it
	const double Reach = FMath::Max(R, 1.5 * FOutlawsGeometry::UnitsToCm) + BoxW * 0.5;
	if (FVector2D::DistSquared(FVector2D(Body), FVector2D(CollectBoxRest)) > Reach * Reach) return;
	const double Z = CollectBoxRest.Z;
	if (Z < Feet - 40.0 || Z > Feet + 170.0) return;
	if (Z > Feet + 40.0)
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsCollectBox), false, A);
		Q.AddIgnoredActor(P);
		FHitResult Hit;
		const FVector Chest = Body + FVector(0.0, 0.0, Half * 0.4);
		if (World->LineTraceSingleByChannel(Hit, Chest, CollectBoxRest + FVector(0.0, 0.0, BoxHover + BoxH * 0.5), ECC_Visibility, Q)) return;
	}
	CollectBoxTaken(TEXT("walked into"));
}

bool AOutlawsLevelActor::TakeCollectBoxByHand(const FVector& Eye, const FVector& Hand, float Radius)
{
	AActor* A = CollectBoxActor.Get();
	if (!A || bDead) return false;
	const FVector Centre = A->GetActorLocation() + FVector(0.0, 0.0, BoxHover + BoxH * 0.5);
	// to the box's surface (its longest half-side counted), and in sight of the eyes (not through a wall)
	if (FVector::Dist(Hand, Centre) - BoxH * 0.5 > Radius) return false;
	if (!CanSee(Eye, Centre, A)) return false;
	CollectBoxTaken(TEXT("taken by hand"));
	return true;
}

void AOutlawsLevelActor::CollectBoxTaken(const TCHAR* How)
{
	AActor* A = CollectBoxActor.Get();
	CollectBoxActor.Reset();
	CollectBoxGlint.Reset();
	if (!A) return;
	OutlawsAchievements::NoteBoxFound(this, LevelName);
	TArray<FString> Levels, Labels;
	OutlawsAchievements::BoxLevels(Levels, Labels);
	int32 N = 0;
	for (const FString& Lv : Levels) N += OutlawsAchievements::BoxFound(this, Lv) ? 1 : 0;
	UE_LOG(LogOutlaws, Log, TEXT("OLBOX found in %s (%s): %d of %d on this profile"), *LevelName, How, N, Levels.Num());
	Notify(FString::Printf(TEXT("Hidden box found: %d of %d"), N, Levels.Num()), 3.f);
	PlayWav(TEXT("powerup"), A->GetActorLocation());
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	AOutlawsPlayer* P = PC ? Cast<AOutlawsPlayer>(PC->GetPawn()) : nullptr;
	if (!P || !P->FlyCollected(A)) A->Destroy();      // (to the waist as a pickup goes, then gone)
}
