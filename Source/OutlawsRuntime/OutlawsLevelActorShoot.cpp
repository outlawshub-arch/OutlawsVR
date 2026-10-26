// AOutlawsLevelActor, shootables half: props that react to bullets (FUNC Inv_Object: bottles, vases,
// lamps, barrels, sacks, pots, bells, rope), glass panes (SHATTER walls), and ground dynamite that
// blows up when shot.  research/outlaws_shootables_spec.md holds the evidence.
#include "OutlawsLevelActor.h"
#include "OutlawsPerf.h"
#include "OutlawsRuntime.h"
#include "OutlawsLab.h"
#include "OutlawsLevel.h"
#include "OutlawsFormats.h"
#include "OutlawsGeometry.h"
#include "OutlawsTextures.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsEnemy.h"
#include "OutlawsPlayer.h"
#include "OutlawsInf.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsCoop.h"
#include "OutlawsDecalClip.h"
#include "OutlawsAchievements.h"
#include "ProceduralMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/CapsuleComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"

namespace
{
	constexpr double ShootU = FOutlawsGeometry::UnitsToCm;
	constexpr uint32 EvObject = 16384;       // a prop's chor event into its sector [EXE 0x14007bec2]
	constexpr uint32 EvBlastNear = 0x40;     // explosion within WALL_RADIUS
	constexpr uint32 EvBlastFar = 0x8000;    // explosion within RADIUS
}

void AOutlawsLevelActor::LoadShootTables()
{
	if (!AssetsRef)
	{
		return;
	}
	// outlaws.lab sounds.lst: "NUM SOUNDS: 14" then "index L|M|H name"
	if (ObjectSounds.Num() == 0)
	{
		FString Text;
		if (AssetsRef->GetText(TEXT("sounds.lst"), Text))
		{
			TArray<FString> Lines;
			Text.ParseIntoArrayLines(Lines);
			for (const FString& Line : Lines)
			{
				TArray<FString> T;
				Line.ParseIntoArrayWS(T);
				if (T.Num() >= 3 && FChar::IsDigit(T[0][0]))
				{
					const int32 N = FCString::Atoi(*T[0]);
					if (ObjectSounds.Num() <= N) ObjectSounds.SetNum(N + 1);
					ObjectSounds[N] = FPaths::GetBaseFilename(T[2]);
				}
			}
		}
	}
	// the dynamite blast (pdynam.itm)
	FString Pd;
	if (AssetsRef->GetText(TEXT("pdynam.itm"), Pd))
	{
		FString Func;
		TMap<FString, FString> D;
		OutlawsFormats::ParseItm(Pd, Func, D);
		if (D.Contains(TEXT("RADIUS"))) BlastRadius = FCString::Atof(*D[TEXT("RADIUS")]);
		if (D.Contains(TEXT("WALL_RADIUS"))) BlastWallRadius = FCString::Atof(*D[TEXT("WALL_RADIUS")]);
		if (D.Contains(TEXT("DAMAGE"))) BlastDamage = FCString::Atof(*D[TEXT("DAMAGE")]);
		if (D.Contains(TEXT("EXPLOSION"))) BlastSprite = D[TEXT("EXPLOSION")];
		if (D.Contains(TEXT("EXPLODE_SOUND"))) BlastSound = D[TEXT("EXPLODE_SOUND")];
	}
}

// ---- props -----------------------------------------------------------------------------------------

void AOutlawsLevelActor::AddProp(AOutlawsSpriteActor* S, const FString& Name, const TMap<FString, FString>& Data, int32 Sector, bool bHang)
{
	if (!S)
	{
		return;
	}
	FProp P;
	P.Sprite = S;
	P.Name = Name;
	P.Sector = Sector;
	// TYPE SHOOT reacts to bullets; any other TYPE is used by hand (NUDGE) [EXE 0x14007bceb]
	const FString Type = Data.FindRef(TEXT("TYPE"));
	P.bShoot = Type.Equals(TEXT("SHOOT"), ESearchCase::IgnoreCase);
	P.bNudge = !Type.IsEmpty() && !P.bShoot;
	// the hit cylinder: RADIUS / HEIGHT from the ITM (RADIUS -1 behaves as 1), else the sprite frame
	const FVector Scale = S->GetActorScale3D();      // (1, cell width cm, cell height cm)
	P.Radius = Data.Contains(TEXT("RADIUS")) ? FMath::Abs(FCString::Atof(*Data[TEXT("RADIUS")])) * ShootU : Scale.Y * 0.5;
	P.Height = Data.Contains(TEXT("HEIGHT")) ? FMath::Abs(FCString::Atof(*Data[TEXT("HEIGHT")])) * ShootU : Scale.Z;
	const bool bHangs = bHang || (Data.Contains(TEXT("HEIGHT")) && FCString::Atof(*Data[TEXT("HEIGHT")]) < 0.f);
	const FVector Loc = S->GetActorLocation();
	// a hanging object is anchored at the sprite top: the cylinder hangs under it
	P.Base = bHangs ? Loc + FVector(0.0, 0.0, Scale.Z - P.Height) : Loc;
	P.Offset = P.Base - Loc;
	Props.Add(P);
}

void AOutlawsLevelActor::PropReact(int32 Index, bool bNudge)
{
	if (!Props.IsValidIndex(Index))
	{
		return;
	}
	FProp& P = Props[Index];
	AOutlawsSpriteActor* S = P.Sprite.Get();
	if (!S || (bNudge ? !P.bNudge : !P.bShoot))
	{
		return;
	}
	// a hit plays the next choreography; none left (or one already running past its end): nothing
	if (S->Chors.IsValidIndex(P.Current + 1) && !P.bActive)
	{
		PropEnter(Index, P.Current + 1);
		if (UOutlawsCoop::IsActive(this))
		{
			Props[Index].bCoopRemote = false;          // (co-op: this machine's own reaction, its INF events its own)
			UOutlawsCoop::NoteWorldBit(this, UOutlawsCoop::BitProp, Props[Index].CoopId, Props[Index].Current, PropHitDir, bNudge ? 1.f : 0.f);
		}
	}
}

void AOutlawsLevelActor::PropEnter(int32 Index, int32 Chor)
{
	FProp& P = Props[Index];
	AOutlawsSpriteActor* S = P.Sprite.Get();
	if (!S || !S->Chors.IsValidIndex(Chor))
	{
		return;
	}
	P.Current = Chor;
	P.NextEvent = 0;
	P.bActive = Chor != 0;
	if (Chor != 0 && S->Model && S->ModelStem.StartsWith(TEXT("BOTTLE")))
	{
		// a 3D bottle bursts: its own glass flies (user 2026-09-28); the break plays on unseen for its sound and events
		ShatterGlass(Cast<UProceduralMeshComponent>(S->Model), PropHitDir, FString());
		S->SetActorHiddenInGame(true);
	}
	else if (Chor != 0 && S->Model)
	{
		// a reaction that comes back to rest (-4) without shattering (-8) is a knock: barrels, sacks, pots and pans
		// wobble in their frames; the model does it itself, leaning away from the shot and back.  Anything else (a
		// skull knocked over, a vase breaking) plays as the game drew it
		bool bReturns = false, bBreaks = false;
		for (const FIntVector4& E : S->Chors[Chor].Events)
		{
			bReturns |= E.Y == -4;
			bBreaks |= E.Y == -8;
		}
		UProceduralMeshComponent* M = Cast<UProceduralMeshComponent>(S->Model);
		FOutlawsKeyAnim KA;
		if (M && ItemKeyAnim(S->ModelStem, Chor, KA))
		{
			// its own keyframes from the sprite's frames (2026-10-01): the bell's swing, a pot's hop, the lantern kicked on
			// its hook with its flame and glass gone (it stays so, as the sprite holds its last frame)
			bool bMoves = KA.Lift.Num() > 0;
			for (const float Dg : KA.Deg) bMoves |= FMath::Abs(Dg) > 0.01f;
			if (bMoves) StartPropKeys(M, PropHitDir.IsNearlyZero() ? FVector::ForwardVector : PropHitDir, KA);
			if (KA.Hide.Num()) HideItemSections(M, S->ModelStem, KA.Hide);
		}
		else if (bReturns && !bBreaks && M)
		{
			if (!PropHitDir.IsNearlyZero()) StartPropWobble(M, PropHitDir, P.Radius);
		}
		else
		{
			S->ShowSprite();
		}
	}
	bool bLoop = false;
	for (const FIntVector4& E : S->Chors[Chor].Events)
	{
		bLoop |= E.Y == -2;
	}
	S->PlayChor(Chor, bLoop);
	SetActorTickEnabled(true);
}

void AOutlawsLevelActor::StartPropWobble(UProceduralMeshComponent* M, const FVector& Dir, float Radius)
{
	FVector D(Dir.X, Dir.Y, 0.0);
	if (!D.Normalize()) D = FVector::ForwardVector;
	for (FPropWobble& W : PropWobbles)
	{
		if (W.Mesh.Get() == M)
		{
			W.Dir = D;               // hit again while rocking: the new knock starts from where it stands
			W.T = 0.f;
			return;
		}
	}
	FPropWobble W;
	W.Mesh = M;
	W.Foot = M->GetComponentLocation();
	W.Rest = M->GetComponentQuat();
	W.Dir = D;
	W.Radius = FMath::Clamp(Radius, 5.f, 60.f);
	W.Peak = 8.f;
	PropWobbles.Add(W);
	SetActorTickEnabled(true);
}

void AOutlawsLevelActor::StartPropKeys(UProceduralMeshComponent* M, const FVector& Dir, const FOutlawsKeyAnim& KA)
{
	const TArray<float>& Keys = KA.Deg;
	const float Fps = KA.Fps;
	FVector D(Dir.X, Dir.Y, 0.0);
	if (!D.Normalize()) D = FVector::ForwardVector;
	FPropWobble* W = PropWobbles.FindByPredicate([M](const FPropWobble& X) { return X.Mesh.Get() == M; });
	if (!W)
	{
		W = &PropWobbles.AddDefaulted_GetRef();
		W->Mesh = M;
		W->Foot = M->GetComponentLocation();
		W->Rest = M->GetComponentQuat();
		const FBox B = M->Bounds.GetBox();
		W->Top = FVector(B.GetCenter().X, B.GetCenter().Y, B.Max.Z);       // it hangs from its top
	}
	W->Dir = D;
	W->Keys = Keys;
	W->Lift = KA.Lift;
	W->bTop = KA.bTop;
	W->Fps = Fps;
	W->T = 0.f;
	SetActorTickEnabled(true);
}

void AOutlawsLevelActor::UpdatePropWobbles(float Dt)
{
	// a quick knock over 0.09 s, then a damped rock back through upright and settle (0.7 s in all).  Leaning away it
	// pivots on the bottom edge on the far side, rocking back past upright on the near edge, so the foot never sinks
	for (int32 i = PropWobbles.Num() - 1; i >= 0; --i)
	{
		FPropWobble& W = PropWobbles[i];
		UProceduralMeshComponent* M = W.Mesh.Get();
		if (!M)
		{
			PropWobbles.RemoveAt(i);
			continue;
		}
		W.T += Dt;
		if (W.Keys.Num() > 0)
		{
			// keyframed: the step's angle (and lift) eased to the next's (the sprite showed each for a tick), about the
			// hanging point (a bell, a lantern) or the foot (a pot); + tips the top toward the shot's way
			const int32 N = FMath::Max(W.Keys.Num(), W.Lift.Num());
			auto At = [N](const TArray<float>& A, int32 k) { return A.IsValidIndex(k) ? A[k] : 0.f; };
			const float Step = W.T * W.Fps;
			const int32 K = FMath::FloorToInt(Step);
			const bool bEnd = K >= N;
			const float E = 0.5f - 0.5f * FMath::Cos(PI * (Step - K));
			const float S0 = bEnd ? At(W.Keys, N - 1) : FMath::Lerp(At(W.Keys, K), At(W.Keys, K + 1 < N ? K + 1 : N - 1), E);
			const float L0 = bEnd ? 0.f : FMath::Lerp(At(W.Lift, K), At(W.Lift, K + 1 < N ? K + 1 : N - 1), E);
			const FQuat Q(FVector::CrossProduct(FVector::UpVector, W.Dir), FMath::DegreesToRadians(S0));
			const FVector Pivot = W.bTop ? W.Top : W.Foot;
			M->SetWorldLocationAndRotation(Pivot + Q.RotateVector(W.Foot - Pivot) + FVector::UpVector * L0, Q * W.Rest);
			if (bEnd) PropWobbles.RemoveAt(i);
			continue;
		}
		constexpr float Knock = 0.09f, Settle = 0.62f;
		float A = 0.f;
		if (W.T < Knock)
		{
			A = W.Peak * FMath::Sin(HALF_PI * W.T / Knock);
		}
		else if (W.T < Knock + Settle)
		{
			const float u = W.T - Knock;
			A = W.Peak * FMath::Exp(-6.f * u) * FMath::Cos(11.f * u);
		}
		const bool bDone = W.T >= Knock + Settle;
		if (bDone) A = 0.f;
		const FVector Axis = FVector::CrossProduct(FVector::UpVector, W.Dir);   // tips the top toward Dir
		const FQuat Q(Axis, FMath::DegreesToRadians(A));
		const FVector Pivot = W.Foot + W.Dir * (A >= 0.f ? W.Radius : -W.Radius);
		M->SetWorldLocationAndRotation(Pivot + Q.RotateVector(W.Foot - Pivot), Q * W.Rest);
		if (bDone) PropWobbles.RemoveAt(i);
	}
}

UMaterialInstanceDynamic* AOutlawsLevelActor::MakeFireLook(UObject* Outer, TArray<FLinearColor>& OutCells, float& OutFps, float& OutAspect)
{
	const TSharedPtr<OutlawsTextures::FSpriteAtlas> Atlas = GetAtlas(TEXT("fire01.nwx"), false);
	if (!Atlas || !Atlas->Texture || !SpriteMaterial) return nullptr;
	UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(SpriteMaterial, Outer);
	const int32 Cols = FMath::Max(1, Atlas->Cols), Rows = FMath::Max(1, Atlas->Rows);
	const float SX = 1.f / Cols, SY = 1.f / Rows;
	M->SetTextureParameterValue(TEXT("Tex"), Atlas->Texture);
	M->SetVectorParameterValue(TEXT("UVScale"), FLinearColor(SX, SY, 0.f, 0.f));
	M->SetScalarParameterValue(TEXT("Brightness"), 1.f);              // (a flame: full bright whatever the light)
	OutCells.Reset();
	const int32 Frames = FMath::Max(1, Atlas->Frames);
	for (int32 f = 0; f < Frames; ++f)
	{
		const int32 C = Atlas->Grid.IsValidIndex(f) ? Atlas->Grid[f] : f;       // (view row 0: the frames in order)
		OutCells.Add(FLinearColor((C % Cols) * SX, (C / Cols) * SY, 0.f, 0.f));
	}
	M->SetVectorParameterValue(TEXT("UVOffset"), OutCells[0]);
	OutFps = Atlas->FPS > 0.f ? Atlas->FPS : 10.f;
	OutAspect = Atlas->CellH > 0 ? float(Atlas->CellW) / Atlas->CellH : 0.66f;
	return M;
}

void AOutlawsLevelActor::SpawnSparks(const FVector& Where, int32 Count)
{
	// lamp embers: small fire01 sprites thrown out, falling and fading [EXE 0x14006d750]
	const TSharedPtr<OutlawsTextures::FSpriteAtlas> Atlas = GetAtlas(TEXT("fire01.nwx"), false);
	for (int32 i = 0; i < Count; ++i)
	{
		AOutlawsSpriteActor* S = MakeSprite(Atlas, TEXT("EMBER"), Where, 0.f, 31);
		if (!S) break;
		SpawnedActors.Add(S);
		FSpark K;
		K.Scale0 = S->GetActorScale3D();         // fire01's own size (1, width, height cm)
		S->SetActorScale3D(S->GetActorScale3D() * FMath::FRandRange(0.25f, 0.6f));
		K.Sprite = S;
		K.Vel = FVector(FMath::FRandRange(-3.5f, 3.5f), FMath::FRandRange(-3.5f, 3.5f), FMath::FRandRange(0.f, 21.f)) * ShootU * 0.5;
		// it falls onto the floor below and burns there a while, each flame its own size (user 2026-09-28: they fell
		// through the ground); up to 3 s to come down
		K.Life = 3.f;
		K.bFire = true;
		Sparks.Add(K);
	}
}

void AOutlawsLevelActor::SpawnDirt(const FVector& Where, const FVector& Up, int32 Count)
{
	if (!DirtAtlas)
	{
		// four clods of earth, 32 px each: a lumpy blob, lit on top and dark underneath, with a few crumbs beside it
		constexpr int32 C = 32, N = 4;
		TArray<FColor> Px;
		Px.Init(FColor(96, 70, 44, 0), C * N * C);
		FRandomStream R(0xD127);
		for (int32 k = 0; k < N; ++k)
		{
			auto Blob = [&](float Cx, float Cy, float Rad)
			{
				float Edge[10];
				for (float& E : Edge) E = R.FRandRange(0.7f, 1.f);
				for (int32 y = 0; y < C; ++y)
				{
					for (int32 x = 0; x < C; ++x)
					{
						const float Dx = (x + 0.5f - Cx) / Rad, Dy = (y + 0.5f - Cy) / Rad;
						const float A = (FMath::Atan2(Dy, Dx) / (2.f * PI) + 0.5f) * 10.f;
						const int32 I = FMath::FloorToInt(A) % 10;
						const float Rim = FMath::Lerp(Edge[I], Edge[(I + 1) % 10], A - FMath::FloorToFloat(A));
						if (Dx * Dx + Dy * Dy > Rim * Rim) continue;
						const float Lit = FMath::Clamp(0.75f - 0.45f * Dy + R.FRandRange(-0.12f, 0.12f), 0.35f, 1.1f);
						Px[y * C * N + k * C + x] = FColor(uint8(FMath::Min(255.f, 118 * Lit)), uint8(FMath::Min(255.f, 86 * Lit)), uint8(FMath::Min(255.f, 54 * Lit)), 255);
					}
				}
			};
			Blob(C * 0.5f + R.FRandRange(-2.f, 2.f), C * 0.55f, R.FRandRange(7.f, 10.f));
			for (int32 b = 0; b < 3; ++b) Blob(R.FRandRange(5.f, C - 5.f), R.FRandRange(5.f, C - 5.f), R.FRandRange(1.5f, 3.f));
		}
		DirtTexture = OutlawsTextures::MakeTextureBGRA(C * N, C, Px, false, TF_Nearest, TA_Clamp);
		DirtAtlas = MakeShared<OutlawsTextures::FSpriteAtlas>();
		DirtAtlas->Texture = DirtTexture;
		DirtAtlas->CellW = DirtAtlas->CellH = 16;          // 30 cm a cell before each clod's own scale
		DirtAtlas->Cols = N;
		DirtAtlas->Rows = 1;
		DirtAtlas->Cells = N;
		DirtAtlas->Frames = N;
		DirtAtlas->FPS = 8.f;                              // tumbling through its four looks
		DirtAtlas->Grid = { 0, 1, 2, 3 };
		DirtAtlas->Nwx = TEXT("(dirt)");
	}
	const FVector Nrm = Up.GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
	const int32 Light = LightAt(Where + Nrm * 20.0);
	for (int32 i = 0; i < Count; ++i)
	{
		AOutlawsSpriteActor* S = MakeSprite(DirtAtlas, TEXT("DIRT"), Where + Nrm * 3.0, 0.f, Light);
		if (!S) break;
		SpawnedActors.Add(S);
		S->SetActorScale3D(S->GetActorScale3D() * FMath::FRandRange(0.22f, 0.5f));
		// out all round the spade and up, like earth flung off it
		const FVector Out = FVector::VectorPlaneProject(FMath::VRand(), Nrm).GetSafeNormal();
		FSpark K;
		K.Sprite = S;
		K.bDirt = true;
		K.Vel = (Nrm * FMath::FRandRange(0.7f, 1.3f) + Out * FMath::FRandRange(0.5f, 1.1f)).GetSafeNormal() * FMath::FRandRange(200.f, 380.f);
		K.Life = FMath::FRandRange(0.7f, 1.1f);
		K.bFade = true;
		K.Life0 = K.Life;
		Sparks.Add(K);
	}
	// and a puff of the game's own dust where it went in
	if (AOutlawsSpriteActor* D = MakeSprite(GetAtlas(TEXT("dust.nwx"), false), TEXT("DUST"), Where, 0.f, Light))
	{
		SpawnedActors.Add(D);
		D->PlayChor(0, false);
		FSpark K;
		K.Sprite = D;
		K.bOneShot = true;
		K.Life = 2.f;
		Sparks.Add(K);
	}
}

void AOutlawsLevelActor::SpawnBlood(const FVector& Where, const FVector& Dir, int32 Count)
{
	// gore (user 2026-09-28): small blood.nwx spatters (its three frames looping) thrown out of the wound, mostly on
	// along the shot with some spray, falling and gone within the second
	const TSharedPtr<OutlawsTextures::FSpriteAtlas> Atlas = GetAtlas(TEXT("blood.nwx"), false);
	const FVector On = Dir.GetSafeNormal();
	// lit by where it flies, as the bad guy bleeding is (user 2026-09-29: it stayed bright in the dark)
	const int32 Lit = FMath::Clamp(LightAt(Where), 0, 31);
	for (int32 i = 0; i < Count; ++i)
	{
		AOutlawsSpriteActor* S = MakeSprite(Atlas, TEXT("BLOOD"), Where + On * 6.0, 0.f, Lit);
		if (!S) break;
		SpawnedActors.Add(S);
		S->SetActorScale3D(S->GetActorScale3D() * FMath::FRandRange(0.65f, 1.1f));     // twice the first size (user 2026-09-28)
		S->PlayChor(0, true);
		FSpark K;
		K.Sprite = S;
		K.bBlood = true;
		K.Vel = (On * FMath::FRandRange(0.6f, 1.f) + FMath::VRand() * 0.45f + FVector(0.0, 0.0, 0.35)).GetSafeNormal() * FMath::FRandRange(180.f, 320.f);
		K.Life = FMath::FRandRange(0.5f, 0.85f);
		K.bFade = true;
		K.Life0 = K.Life;
		Sparks.Add(K);
	}
}

// ---- glass -----------------------------------------------------------------------------------------

void AOutlawsLevelActor::CollectPanes(const FOutlawsLevel& L, FOutlawsBuildFilter& StaticFilter)
{
	// a pane is an opening whose wall (either side) carries SHATTER and a see-through mid
	Panes.Reset();
	TSet<uint64> Done;
	for (const FOutlawsSector& S : L.Sectors)
	{
		for (int32 w = 0; w < S.Walls.Num(); ++w)
		{
			const FOutlawsWall& W = S.Walls[w];
			if (!(W.Flags1 & OutlawsFlags::WallShatter) || W.Adjoin < 0 || !L.Sectors.IsValidIndex(W.Adjoin) || Done.Contains(FOutlawsBuildFilter::Key(S.Index, w)))
			{
				continue;
			}
			FPane P;
			P.S = S.Index;
			P.W = w;
			P.MS = W.Adjoin;
			P.MW = W.Mirror;
			Done.Add(FOutlawsBuildFilter::Key(P.S, P.W));
			StaticFilter.SkipMid.Add(FOutlawsBuildFilter::Key(P.S, P.W));
			if (L.Sectors[P.MS].Walls.IsValidIndex(P.MW))
			{
				Done.Add(FOutlawsBuildFilter::Key(P.MS, P.MW));
				StaticFilter.SkipMid.Add(FOutlawsBuildFilter::Key(P.MS, P.MW));
			}
			if (!S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) continue;
			P.A = S.Verts[W.V1];
			P.B = S.Verts[W.V2];
			const FOutlawsSector& M = L.Sectors[P.MS];
			P.Bottom = FMath::Max(S.Floor.Y, M.Floor.Y);
			P.Top = FMath::Min(S.Ceil.Y, M.Ceil.Y);
			Panes.Add(P);
		}
	}
}

void AOutlawsLevelActor::BuildPanes(const FOutlawsLevel& L, const TMap<FString, FOutlawsTexInfo>& TexInfo, const TArray<FColor>& Palette)
{
	if (!AssetsRef || !WorldMaskedMaterial)
	{
		return;
	}
	for (FPane& P : Panes)
	{
		// its glass on its own mesh: bodies stop at it, bullets and eyes pass through
		FOutlawsBuildFilter F;
		F.bOnly = true;
		F.bMidOnly = true;
		F.OnlyWalls.Add(FOutlawsBuildFilter::Key(P.S, P.W));
		if (P.MW >= 0) F.OnlyWalls.Add(FOutlawsBuildFilter::Key(P.MS, P.MW));
		FOutlawsGeometry G;
		G.Build(L, TexInfo, bReverseWinding, &F);
		UProceduralMeshComponent* M = NewMeshComponent(TEXT("OutlawsGlass"), true);
		M->SetCollisionObjectType(ECC_WorldStatic);
		M->SetCollisionResponseToAllChannels(ECR_Ignore);
		M->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
		// every pane gets its own materials (straight from the world ones, texture set here), so its
		// break frames change on this window only
		TArray<FString> Keys;
		G.World.GetKeys(Keys);
		Keys.Sort();
		int32 i = 0;
		TArray<int32> Collide;
		for (const FString& K : Keys)
		{
			const FOutlawsMeshSection& S = G.World[K];
			if (S.Triangles.Num() == 0) continue;
			M->CreateMeshSection_LinearColor(i, S.Positions, S.Triangles, S.Normals, S.UVs, S.Colors, TArray<FProcMeshTangent>(), false);
			Collide.Add(i);
			UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(S.bMasked ? (WorldGlassMaterial ? WorldGlassMaterial.Get() : WorldMaskedMaterial.Get()) : WorldMaterial.Get(), this);
			if (const TObjectPtr<UTexture2D>* Tex = Textures.Find(S.Texture))
			{
				Mid->SetTextureParameterValue(TEXT("Tex"), *Tex);
			}
			M->SetMaterial(i, Mid);
			P.Mids.Add(Mid);
			P.Stems.Add(S.Texture);
			Materials.Add(FString::Printf(TEXT("GLASS#%d#%d#%d"), P.S, P.W, i), Mid);   // kept referenced (no '|': not a level texture key)
			++i;
		}
		CookCollisionOnce(M, Collide);
		if (bTintedGlass && G.Tint.Triangles.Num() > 0)
		{
			M->CreateMeshSection_LinearColor(i, G.Tint.Positions, G.Tint.Triangles, G.Tint.Normals, G.Tint.UVs, G.Tint.Colors, TArray<FProcMeshTangent>(), false);
			M->SetMaterial(i, GlassTintMaterial());
			P.TintSection = i;
		}
		P.Mesh = M;
		// the ATX: frames after the first STOP are the break, START_SOUND its sound [ATX oltex.lab]
		const FOutlawsWall& W = L.Sectors[P.S].Walls[P.W];
		const FString Atx = L.Textures.IsValidIndex(W.Mid) ? L.Textures[W.Mid] : FString();
		FString Text;
		if (Atx.EndsWith(TEXT(".atx"), ESearchCase::IgnoreCase) && AssetsRef->GetText(Atx, Text))
		{
			TArray<FString> Lines;
			Text.ParseIntoArrayLines(Lines);
			bool bAfterStop = false;
			for (const FString& Line : Lines)
			{
				TArray<FString> T;
				Line.ParseIntoArrayWS(T);
				if (T.Num() == 0) continue;
				const FString Op = T[0].ToUpper();
				if (Op == TEXT("STOP")) { if (bAfterStop && P.Frames.Num() > 0) break; bAfterStop = true; }
				else if (Op == TEXT("RATE") && T.Num() > 1) P.Rate = FMath::Max(1.f, FCString::Atof(*T[1]));
				else if (Op == TEXT("START_SOUND") && T.Num() > 2) P.Sound = FPaths::GetBaseFilename(T[2]);
				else if (Op == TEXT("TEXTURE") && T.Num() > 1 && bAfterStop)
				{
					// one texture per frame, shared by every window using it: each pane loading its own under the
					// same key dropped the earlier panes' copies to the garbage collector, and breaking such a
					// window later handed its material a freed texture (a crash on a render worker)
					const FString Frame = T[1].Contains(TEXT(".")) ? T[1] : T[1] + TEXT(".pcx");
					const FString Key = TEXT("GLASS:") + Frame.ToUpper();
					UTexture2D* Tex = nullptr;
					if (const TObjectPtr<UTexture2D>* Have = Textures.Find(Key))
					{
						Tex = *Have;
					}
					else
					{
						FOutlawsTexInfo Info;
						bool bWasHd = false;
						Tex = OutlawsTextures::LoadLevelTexture(*AssetsRef, Frame, bHDTextures, Palette, false, Info, bWasHd);
						if (Tex) Textures.Add(Key, Tex);
					}
					if (Tex) P.Frames.Add(Tex);
				}
			}
		}
	}
}

UMaterialInterface* AOutlawsLevelActor::GlassTintMaterial()
{
	if (!GlassTintMid && WaterMaterial)
	{
		// a pale grey-blue sheet at a third opacity, lit by the sector like everything else
		GlassTintMid = UMaterialInstanceDynamic::Create(WaterMaterial, this);
		TArray<FColor> Px = { FColor(170, 190, 200, 255) };
		if (UTexture2D* T = OutlawsTextures::MakeTextureBGRA(1, 1, Px, false, TF_Nearest, TA_Wrap))
		{
			Textures.Add(TEXT("GLASSTINT"), T);
			GlassTintMid->SetTextureParameterValue(TEXT("Tex"), T);
		}
		GlassTintMid->SetScalarParameterValue(TEXT("Opacity"), 0.3f);
	}
	return GlassTintMid ? static_cast<UMaterialInterface*>(GlassTintMid) : WaterMaterial.Get();
}

void AOutlawsLevelActor::ShatterPane(int32 Index)
{
	FPane& P = Panes[Index];
	if (P.bBroken)
	{
		return;
	}
	// SHATTR1, the break frames at the ATX rate, the last one held; the opening becomes walkable
	// [EXE 0x1402df830]
	P.bBroken = true;
	P.T = 0.f;
	P.Shown = -1;
	if (P.Mesh && P.TintSection >= 0) P.Mesh->SetMeshSectionVisible(P.TintSection, false);   // the glass is gone
	if (P.Mesh) P.Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	const FVector2D C = (P.A + P.B) * 0.5;
	PlayWav(P.Sound, FOutlawsGeometry::ToUE(C, (P.Bottom + P.Top) * 0.5));
	SetActorTickEnabled(true);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT glass shattered: sector %d wall %d, %d frames"), P.S, P.W, P.Frames.Num());
	if (!bRestoring && UOutlawsCoop::IsActive(this)) UOutlawsCoop::NoteWorldBit(this, UOutlawsCoop::BitPane, Index, 0, FVector::ZeroVector, 0.f);
}

void AOutlawsLevelActor::ShatterAlong(const FVector& From, const FVector& To)
{
	const FVector2D A = FOutlawsGeometry::ToOutlawsPlan(From), B = FOutlawsGeometry::ToOutlawsPlan(To);
	const FVector2D D = B - A;
	for (int32 i = 0; i < Panes.Num(); ++i)
	{
		const FPane& P = Panes[i];
		if (P.bBroken) continue;
		// 2D crossing of the shot with the wall line, then the height there inside the opening
		const FVector2D E = P.B - P.A;
		const double Den = D.X * E.Y - D.Y * E.X;
		if (FMath::Abs(Den) < 1e-9) continue;
		const FVector2D AP = P.A - A;
		const double T = (AP.X * E.Y - AP.Y * E.X) / Den;
		const double S = (AP.X * D.Y - AP.Y * D.X) / Den;
		if (T < 0.0 || T > 1.0 || S < 0.0 || S > 1.0) continue;
		const double Y = (From.Z + (To.Z - From.Z) * T) / ShootU;
		if (Y >= P.Bottom && Y <= P.Top)
		{
			ShatterPane(i);
		}
	}
}

void AOutlawsLevelActor::RefreshPropBases()
{
	for (FProp& P : Props)
	{
		if (const AOutlawsSpriteActor* S = P.Sprite.Get()) P.Base = S->GetActorLocation() + P.Offset;
	}
}

bool AOutlawsLevelActor::ShootAlong(const FVector& From, FVector& InOutEnd)
{
	RefreshPropBases();
	// the nearest prop cylinder the shot meets before the world stopped it
	const FVector Seg = InOutEnd - From;
	double BestT = 1.0;
	int32 Best = -1;
	for (int32 i = 0; i < Props.Num(); ++i)
	{
		const FProp& P = Props[i];
		if (!P.Sprite.IsValid() || P.bNoCollide || P.bGrabOnly) continue;
		// ray against the vertical circle in 2D, then the height at the entry
		const FVector2D O(From.X - P.Base.X, From.Y - P.Base.Y), Dd(Seg.X, Seg.Y);
		const double a = Dd.SizeSquared(), b = 2.0 * FVector2D::DotProduct(O, Dd), c = O.SizeSquared() - P.Radius * P.Radius;
		if (a < 1e-6) continue;
		const double Disc = b * b - 4.0 * a * c;
		if (Disc < 0.0) continue;
		const double Sq = FMath::Sqrt(Disc);
		double T = (-b - Sq) / (2.0 * a);
		// both crossings behind the start: the prop lies back along the line, not in the way (user 2026-09-28: a swing
		// of the arms kept hitting props behind the fist; a shot could hit one behind the shooter)
		if ((-b + Sq) / (2.0 * a) < 0.0) continue;
		if (T < 0.0) T = 0.0;                 // starting inside it
		if (T >= BestT) continue;
		const double Z = From.Z + Seg.Z * T;
		if (Z < P.Base.Z || Z > P.Base.Z + P.Height) continue;
		BestT = T;
		Best = i;
	}
	// glass before the stopping point breaks (the bullet goes on)
	const FVector Stop = From + Seg * BestT;
	ShatterAlong(From, Stop);
	if (Best < 0)
	{
		return false;
	}
	InOutEnd = Stop;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT shot a %s"), *Props[Best].Name);
	PropHitDir = Seg.GetSafeNormal();
	PropReact(Best, false);
	return true;
}

// ---- bottles ---------------------------------------------------------------------------------------

FString AOutlawsLevelActor::BreakSoundOf(const AOutlawsSpriteActor* S) const
{
	// the first SND of its reactions (bottles: sounds.lst 0, bottle.wav)
	for (int32 c = 1; S && c < S->Chors.Num(); ++c)
	{
		for (const FIntVector4& E : S->Chors[c].Events)
		{
			if (E.Y == -3 && ObjectSounds.IsValidIndex(E.W) && !ObjectSounds[E.W].IsEmpty()) return ObjectSounds[E.W];
		}
	}
	return TEXT("bottle");
}

namespace
{
	// bottles, and the small props a hand can take (user 2026-09-29): pots, pans, the cow skull, vases and pottery, each
	// at the size it has in the hand (the level's stand as big as their sprites) and breakable or not
	// (BUG-PANHOLD: the pan and the saucepan by their long handles; their shapes read as "palm", which took them by the
	// middle)
	struct FGrab { float InHand; bool bBreaks; const TCHAR* Hold = nullptr; };
	const TMap<FString, FGrab> Grabbable = {
		{ TEXT("POT1"), { 0.40f, false } }, { TEXT("POT2"), { 0.28f, false, TEXT("handle") } }, { TEXT("PAN1"), { 0.32f, false, TEXT("handle") } },
		{ TEXT("COWSKULL"), { 0.85f, false } }, { TEXT("VASE01"), { 0.45f, true } }, { TEXT("VASE02"), { 0.40f, true } },
		{ TEXT("VASE03"), { 0.55f, true } }, { TEXT("VASE04"), { 0.55f, true } }, { TEXT("POTTERY1"), { 0.40f, true } },
		{ TEXT("POTTERY2"), { 0.38f, true } },
		// the rocks (user 2026-09-30): a metre across in the level, a 25 to 30 cm stone in the hand
		{ TEXT("ROCK1"), { 0.30f, false } }, { TEXT("ROCK2"), { 0.28f, false } }, { TEXT("ROCK3"), { 0.26f, false } },
		{ TEXT("ROCK4"), { 0.30f, false } }, { TEXT("ROCK5"), { 0.26f, false } }, { TEXT("ROCK6"), { 0.26f, false } } };
}

FString AOutlawsLevelActor::HoldModeFor(const FString& Stem)
{
	const FString& Json = ItemTraits(Stem).HoldMode;
	if (!Json.IsEmpty()) return Json;
	const FGrab* G = Grabbable.Find(Stem);
	return G && G->Hold ? FString(G->Hold) : FString();
}

void AOutlawsLevelActor::GrabbableProps(TArray<AOutlawsSpriteActor*>& Out)
{
	RefreshPropBases();
	for (const FProp& P : Props)
	{
		AOutlawsSpriteActor* S = P.Sprite.Get();
		if (!S || !S->Model || P.bActive || P.bNoCollide || P.Current != 0 || S->IsHidden()) continue;
		if (!S->ModelStem.StartsWith(TEXT("BOTTLE")) && !Grabbable.Contains(S->ModelStem) && ItemTraits(S->ModelStem).InHand <= 0.f) continue;
		Out.Add(S);
	}
}

AOutlawsSpriteActor* AOutlawsLevelActor::TakeBottleNear(const FVector& Hand, double Reach, FString& OutSound, float& OutScale, bool& bOutBreaks)
{
	AOutlawsSpriteActor* Best = FindBottleNear(Hand, Reach);
	return Best ? TakeBottle(Best, OutSound, OutScale, bOutBreaks) : nullptr;
}

AOutlawsSpriteActor* AOutlawsLevelActor::FindBottleNear(const FVector& Hand, double Reach)
{
	TArray<AOutlawsSpriteActor*> Cands;
	GrabbableProps(Cands);
	AOutlawsSpriteActor* Best = nullptr;
	double BestD = Reach;
	for (AOutlawsSpriteActor* S : Cands)
	{
		const double D = FVector::Dist(S->Model->Bounds.GetBox().GetClosestPointTo(Hand), Hand);
		if (D < BestD)
		{
			BestD = D;
			Best = S;
		}
	}
	return Best;
}

AOutlawsSpriteActor* AOutlawsLevelActor::TakeBottle(AOutlawsSpriteActor* S, FString& OutSound, float& OutScale, bool& bOutBreaks)
{
	const int32 Best = Props.IndexOfByPredicate([S](const FProp& P) { return P.Sprite.Get() == S; });
	if (!S || Best < 0)
	{
		return nullptr;
	}
	OutSound = S->ModelStem.StartsWith(TEXT("ROCK")) ? FString(TEXT("steprock")) : BreakSoundOf(S);   // (a rock knocks, no bottle's ring)
	OutScale = S->ModelScale;
	bOutBreaks = true;
	if (const FGrab* G = Grabbable.Find(S->ModelStem))
	{
		OutScale = 1.f / G->InHand;
		bOutBreaks = G->bBreaks;
	}
	const FItemTraits& Tr = ItemTraits(S->ModelStem);           // (the json's word over the table's)
	if (Tr.InHand > 0.f) OutScale = 1.f / Tr.InHand;
	if (Tr.Breaks >= 0) bOutBreaks = Tr.Breaks > 0;
	if (!Tr.BreakSound.IsEmpty()) OutSound = Tr.BreakSound;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT took a %s off its shelf"), *Props[Best].Name);
	if (UOutlawsCoop::IsActive(this)) UOutlawsCoop::NoteWorldBit(this, UOutlawsCoop::BitPropTaken, Props[Best].CoopId, 0, FVector::ZeroVector, 0.f);
	Props.RemoveAt(Best);
	return S;
}

bool AOutlawsLevelActor::DebugBottleSpot(const FVector& Near, FVector& Out) const
{
	double Best = TNumericLimits<double>::Max();
	for (const FProp& P : Props)
	{
		const AOutlawsSpriteActor* S = P.Sprite.Get();
		if (!S || !S->Model || P.bActive || S->IsHidden() || !S->ModelStem.StartsWith(TEXT("BOTTLE"))) continue;
		const FVector C = S->Model->Bounds.Origin;
		if (FVector::DistSquared(C, Near) < Best)
		{
			Best = FVector::DistSquared(C, Near);
			Out = C;
		}
	}
	return Best < TNumericLimits<double>::Max();
}

void AOutlawsLevelActor::ShatterGlass(UProceduralMeshComponent* Src, const FVector& Dir, const FString& Sound)
{
	FProcMeshSection* Sec = Src ? Src->GetProcMeshSection(0) : nullptr;
	if (!Sec || Sec->ProcIndexBuffer.Num() < 6)
	{
		return;
	}
	const FTransform X = Src->GetComponentTransform();
	const FVector Mid = Src->Bounds.Origin;
	const FVector Along = Dir.GetSafeNormal();
	// the models hold both faces of every triangle, front then back in turn (UseItemModel): a front every 6 indices.
	// Two neighbours (a turned body's quad) make one piece, at most 48 of them
	const TArray<uint32>& Ix = Sec->ProcIndexBuffer;
	const int32 Fronts = Ix.Num() / 6;
	const int32 Stride = FMath::Max(2, 2 * FMath::DivideAndRoundUp(Fronts, 96));
	FGlassBurst G;
	TArray<int32> Tri;
	for (int32 k = 0; k < Fronts && G.Shards.Num() < 48; k += Stride)
	{
		TArray<FVector, TInlineAllocator<6>> P;
		TArray<FVector2D, TInlineAllocator<6>> Uv;
		TArray<FLinearColor, TInlineAllocator<6>> Cl;
		for (int32 f = k; f < FMath::Min(k + 2, Fronts); ++f)
		{
			for (int32 j = 0; j < 3; ++j)
			{
				const int32 Vi = int32(Ix[f * 6 + j]);
				if (!Sec->ProcVertexBuffer.IsValidIndex(Vi)) continue;
				const FProcMeshVertex& V = Sec->ProcVertexBuffer[Vi];
				P.Add(X.TransformPosition(V.Position));
				Uv.Add(V.UV0);
				Cl.Add(FLinearColor(V.Color));
			}
		}
		if (P.Num() < 3 || P.Num() % 3) continue;
		const FVector N = FVector::CrossProduct(P[1] - P[0], P[2] - P[0]).GetSafeNormal();
		if (N.IsNearlyZero()) continue;
		FVector C = FVector::ZeroVector;
		for (const FVector& Q : P) C += Q;
		C /= double(P.Num());
		FShard Sh;
		Sh.First = G.Local.Num();
		Sh.Num = P.Num() * 2;
		Sh.N = N;
		Sh.C = C;
		for (int32 Side = 0; Side < 2; ++Side)
		{
			for (int32 j = 0; j < P.Num(); ++j)
			{
				G.Local.Add((P[j] - C) * 0.9);            // a gap between the pieces
				G.LocalN.Add(Side ? -N : N);
				G.UV.Add(Uv[j]);
				G.Col.Add(Side ? Cl[j] * 0.8f : Cl[j]);
			}
		}
		for (int32 t = 0; t < P.Num(); t += 3)
		{
			const int32 A = Sh.First + t, B = Sh.First + P.Num() + t;
			Tri.Append({ A, A + 1, A + 2, B, B + 2, B + 1 });
		}
		// out from the bottle's middle, on along the blow, and up
		const FVector Out = FVector(C.X - Mid.X, C.Y - Mid.Y, 0.0).GetSafeNormal();
		Sh.V = Out * FMath::FRandRange(90.f, 260.f) + Along * FMath::FRandRange(60.f, 220.f) + FVector(0.0, 0.0, FMath::FRandRange(40.f, 240.f));
		Sh.W = FMath::VRand() * FMath::FRandRange(5.f, 15.f);
		G.Shards.Add(Sh);
	}
	if (G.Shards.Num() == 0)
	{
		return;
	}
	TArray<FVector> Pos;
	Pos.Reserve(G.Local.Num());
	for (const FShard& Sh : G.Shards)
	{
		for (int32 v = 0; v < Sh.Num; ++v) Pos.Add(Sh.C + G.Local[Sh.First + v]);
	}
	UProceduralMeshComponent* M = NewMeshComponent(TEXT("OutlawsShards"), false);
	M->CreateMeshSection_LinearColor(0, Pos, Tri, G.LocalN, G.UV, G.Col, TArray<FProcMeshTangent>(), false);
	M->SetMaterial(0, Src->GetMaterial(0));
	ObjectMeshes.Add(M);
	G.Mesh = M;
	const int32 Pieces = G.Shards.Num();
	GlassBursts.Add(MoveTemp(G));
	SpawnGlass(Mid, Along, 14);
	if (!Sound.IsEmpty()) PlayWav(Sound, Mid);
	SetActorTickEnabled(true);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT a bottle bursts: %d pieces at %s"), Pieces, *Mid.ToString());
}

void AOutlawsLevelActor::UpdateGlass(float Dt)
{
	UWorld* World = GetWorld();
	if (!World || GlassBursts.Num() == 0)
	{
		return;
	}
	FCollisionObjectQueryParams Obj;
	Obj.AddObjectTypesToQuery(ECC_WorldStatic);
	Obj.AddObjectTypesToQuery(ECC_WorldDynamic);
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsShards), false);
	for (int32 b = GlassBursts.Num() - 1; b >= 0; --b)
	{
		FGlassBurst& G = GlassBursts[b];
		UProceduralMeshComponent* M = G.Mesh.Get();
		G.Age += Dt;
		if (!M || G.Age > 4.f)
		{
			if (M)
			{
				ObjectMeshes.Remove(M);
				M->DestroyComponent();
			}
			GlassBursts.RemoveAt(b);
			continue;
		}
		// the pieces lie where they fell, then shrink away over the last second
		const double Size = FMath::Clamp(4.f - G.Age, 0.f, 1.f);
		TArray<FVector> Pos, Nrm;
		Pos.SetNumUninitialized(G.Local.Num());
		Nrm.SetNumUninitialized(G.Local.Num());
		for (FShard& Sh : G.Shards)
		{
			if (!Sh.bRest)
			{
				Sh.V.Z -= 980.0 * Dt;
				const FVector To = Sh.C + Sh.V * Dt;
				FHitResult Hit;
				if (World->LineTraceSingleByObjectType(Hit, Sh.C, To, Obj, Q))
				{
					const FVector Hn = Hit.ImpactNormal;
					if (Hn.Z > 0.6 && Sh.V.Size() < 260.0)
					{
						// down: it lies flat there
						Sh.C = FVector(Hit.ImpactPoint) + Hn * 0.4;
						const FVector Face = Sh.R.RotateVector(Sh.N);
						Sh.R = FQuat::FindBetweenNormals(FVector::DotProduct(Face, Hn) >= 0.0 ? Face : -Face, Hn) * Sh.R;
						Sh.bRest = true;
					}
					else
					{
						// a wall, or too fast: it skips off, slower
						Sh.V = FMath::GetReflectionVector(Sh.V, Hn) * 0.3;
						Sh.C = FVector(Hit.Location) + Hn * 0.5;
						Sh.W *= 0.6;
					}
				}
				else
				{
					Sh.C = To;
				}
				const double Turn = Sh.W.Size();
				if (!Sh.bRest && Turn > 1e-3) Sh.R = FQuat(Sh.W / Turn, Turn * Dt) * Sh.R;
			}
			for (int32 v = Sh.First; v < Sh.First + Sh.Num; ++v)
			{
				Pos[v] = Sh.C + Sh.R.RotateVector(G.Local[v]) * Size;
				Nrm[v] = Sh.R.RotateVector(G.LocalN[v]);
			}
		}
		if (!OutlawsPerf::FreezeMeshUpdates()) M->UpdateMeshSection_LinearColor(0, Pos, Nrm, G.UV, G.Col, TArray<FProcMeshTangent>());
	}
}

void AOutlawsLevelActor::SpawnGlass(const FVector& Where, const FVector& Dir, int32 Count)
{
	if (!GlassAtlas)
	{
		// four frames of glass, 32 px each: a few thin slivers, clear-ish with a bright glint along one edge
		constexpr int32 C = 32, N = 4;
		TArray<FColor> Px;
		Px.Init(FColor(200, 226, 214, 0), C * N * C);
		FRandomStream R(0x61A55);
		for (int32 k = 0; k < N; ++k)
		{
			for (int32 s = 0; s < 3; ++s)
			{
				// a sliver: a thin triangle tapering to a point
				const FVector2D A(R.FRandRange(6.f, 26.f), R.FRandRange(6.f, 26.f));
				const float Ang = R.FRandRange(0.f, 2.f * PI);
				const FVector2D D(FMath::Cos(Ang), FMath::Sin(Ang));
				const float Len = R.FRandRange(5.f, s == 0 ? 12.f : 8.f), Wid = R.FRandRange(1.2f, 2.6f);
				for (int32 y = 0; y < C; ++y)
				{
					for (int32 x = 0; x < C; ++x)
					{
						const FVector2D Q = FVector2D(x + 0.5f, y + 0.5f) - A;
						const float T = float(FVector2D::DotProduct(Q, D)), Off = float(FVector2D::CrossProduct(D, Q));
						if (T < -Len * 0.5f || T > Len * 0.5f) continue;
						const float Half = Wid * (0.5f - T / Len);
						if (Half <= 0.f || FMath::Abs(Off) > Half) continue;
						const bool bGlint = Off > Half - 0.9f;
						Px[y * C * N + k * C + x] = bGlint ? FColor(255, 255, 255, 255) : FColor(206, 232, 222, 150);
					}
				}
			}
		}
		GlassTexture = OutlawsTextures::MakeTextureBGRA(C * N, C, Px, false, TF_Nearest, TA_Clamp);
		GlassAtlas = MakeShared<OutlawsTextures::FSpriteAtlas>();
		GlassAtlas->Texture = GlassTexture;
		GlassAtlas->CellW = GlassAtlas->CellH = 16;
		GlassAtlas->Cols = N;
		GlassAtlas->Rows = 1;
		GlassAtlas->Cells = N;
		GlassAtlas->Frames = N;
		GlassAtlas->FPS = 12.f;                              // turning over: the glints flicker
		GlassAtlas->Grid = { 0, 1, 2, 3 };
		GlassAtlas->Nwx = TEXT("(glass)");
	}
	const int32 Light = FMath::Min(31, LightAt(Where) + 6);
	const FVector Along = Dir.GetSafeNormal();
	for (int32 i = 0; i < Count; ++i)
	{
		AOutlawsSpriteActor* S = MakeSprite(GlassAtlas, TEXT("GLASS"), Where + FMath::VRand() * 4.0, 0.f, Light);
		if (!S) break;
		SpawnedActors.Add(S);
		S->SetActorScale3D(S->GetActorScale3D() * FMath::FRandRange(0.18f, 0.4f));
		FSpark K;
		K.Sprite = S;
		K.bGlass = true;
		K.Vel = (FMath::VRand() + Along * 0.6 + FVector(0.0, 0.0, 0.5)).GetSafeNormal() * FMath::FRandRange(150.f, 380.f);
		K.Life = FMath::FRandRange(0.5f, 0.9f);
		K.bFade = true;
		K.Life0 = K.Life;
		Sparks.Add(K);
	}
}

// ---- dynamite --------------------------------------------------------------------------------------

bool AOutlawsLevelActor::HitDynamite(const UPrimitiveComponent* Comp)
{
	for (int32 i = 0; i < Dynamite.Num(); ++i)
	{
		if (!Dynamite[i].bGone && Dynamite[i].Mesh == Comp)
		{
			Detonate(i);
			return true;
		}
	}
	return false;
}

void AOutlawsLevelActor::Detonate(int32 Index)
{
	FDynamite& D = Dynamite[Index];
	if (D.bGone)
	{
		return;
	}
	D.bGone = true;
	if (UProceduralMeshComponent* M = D.Mesh.Get())
	{
		const FVector At = M->GetComponentLocation();
		if (UOutlawsCoop::IsActive(this)) CoopBlastStick = UOutlawsCoop::StickIdOf(this, M->GetOwner());    // (co-op: which stick went)
		if (AActor* Own = M->GetOwner(); Own && Own != this) Own->Destroy();     // (a pickup of its own: gone with it)
		else M->DestroyComponent();
		D.Where = At;                        // (where it lies now: a lift may have carried it)
	}
	D.Mesh = nullptr;
	Explode(D.Where);
}

void AOutlawsLevelActor::Explode(const FVector& Where)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const double R = BlastRadius * ShootU;
	++BlastsSeen;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT explosion at %s"), *Where.ToString());
	{
		// (BUG-BOTTNT: a match body it kills is thrown from it, on every machine that sees it go off)
		const double NowS = FPlatformTime::Seconds();
		RecentBlasts.RemoveAll([NowS](const FRecentBlast& B) { return NowS - B.Time > 3.0; });
		RecentBlasts.Add({ Where + FVector(0.0, 0.0, 20.0), NowS, float(R), BlastDamage });
	}
	// co-op: a blast here is told to the other machine; one told from there (bCoopRemote) is its sight, its sound and this
	// machine's own player's hurt only (the bad guys, props, glass, the next sticks and the INF come as events of their own)
	const bool bCoopRemote = bCoopRemoteBlast;
	if (!bCoopRemote && UOutlawsCoop::IsActive(this)) UOutlawsCoop::NoteBlast(this, Where, CoopBlastStick);
	CoopBlastStick = -1;
	PlayWav(BlastSound, Where);
	EchoOf(BlastSound, Where);              // (R13)
	// (REQ-TNTFLASH, user 2026-10-12) the area lit as a shot lights it, 5 times the reach and the brightness; the blast told
	// from the other machine (bCoopRemote) comes through here too, so every player sees it
	MuzzleFlash(Where + FVector(0.0, 0.0, 30.0), BlastFlashScale, 1300.f * BlastFlashScale);
	if (!bCoopRemote) AlertEnemies(Where, GunshotNoiseCm);
	// under water [EXE 0x1400ceee0]: no fire and no soot, a boil of bubbles instead of the blast sprite, and the big
	// splash on the surface above (user 2026-09-28)
	float WaterTop = 0.f;
	const bool bUnderWater = WaterAt(Where, WaterTop);
	if (bUnderWater)
	{
		for (int32 b = 0; b < 12; ++b) BlowBubbles(Where + FMath::VRand() * FMath::FRandRange(0.f, 60.f));
		FVector Top;
		if (WaterSurfaceHit(Where, Where + FVector(0.0, 0.0, 5000.0), Top)) SpawnSplash(Top, true);
	}
	else
	{
		AddScorchMarks(Where);
	}
	// the blast sprite, played once
	if (AOutlawsSpriteActor* S = bUnderWater ? nullptr : MakeSprite(GetAtlas(BlastSprite, true), TEXT("BLAST"), Where, 0.f, 31))
	{
		SpawnedActors.Add(S);
		S->PlayChor(0, false);
		FSpark K;
		K.Sprite = S;
		K.bOneShot = true;
		K.Life = 3.f;
		Sparks.Add(K);
	}
	// damage: full within a quarter of the radius, then falling off with the square; needs a clear
	// line [EXE 0x1400d14d0]
	auto Falloff = [R](double D) { return D <= R * 0.25 ? 1.0 : FMath::Square(FMath::Max(0.0, (R - D) / (0.75 * R))); };
	const FVector From = Where + FVector(0.0, 0.0, 20.0);
	if (APlayerController* PC = World->GetFirstPlayerController())
	{
		if (APawn* Pawn = PC->GetPawn())
		{
			const FVector Mid = Pawn->GetActorLocation();
			const double D = FVector::Dist(From, Mid);
			if (D < R && CanSee(From, Mid, Pawn))
			{
				SetHurtSource(From);
				const float Dealt = float(BlastDamage * Falloff(D));
				// (a match: another machine's blast is dealt to this player by the host, as that machine reports it)
				// (REQ-MPBOTS: a bot's stick: the match deals it, the bot credited)
				if (UOutlawsCoop::BlastShooter >= 0) UOutlawsCoop::BlastHostDealt = Dealt;
				else if (!(bCoopRemote && UOutlawsCoop::BlastsDealtByHost(this)))
				{
					if (!bCoopRemote) HurtCause = uint8(EOutlawsHurtCause::OwnBlast);     // (REQ-KILLFEED: this machine's own stick)
					DamagePlayer(Dealt);
				}
				if (AOutlawsPlayer* PP = Cast<AOutlawsPlayer>(Pawn)) PP->BlastThrow(From, Dealt);
			}
		}
	}
	if (!bCoopRemote) UOutlawsCoop::BlastAtPlayers(this, From, float(R), BlastDamage * PlayerDamageScale());     // (a match: the other players in reach)
	FOutlawsStatsScope BlastBy(EOutlawsStatsBy::Me, true);      // (the level's end panels: a blast's kills are dynamite kills)
	int32 BlastKills = 0;                                      // (REQ-EXTRAS dynamite3: the bad guys this one blast killed)
	for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : Enemies)
	{
		AOutlawsEnemy* E = Wk.Get();
		if (!E || E->IsDead() || bCoopRemote) continue;
		const double D = FVector::Dist(From, E->GetActorLocation());
		if (D < R && CanSee(From, E->GetActorLocation(), E))
		{
			E->TakeHit(BlastDamage * Falloff(D) * PlayerDamageScale(), E->GetActorLocation(), (E->GetActorLocation() - Where).GetSafeNormal(), Where, true);
			BlastKills += E->IsDead() && !E->bVoiceOnly && !E->bNotBadGuy ? 1 : 0;
		}
	}
	if (BlastKills > 0) OutlawsAchievements::NoteBlastKills(this, BlastKills);
	// every prop in range reacts as if shot; glass whose wall middle is in range shatters
	RefreshPropBases();
	for (int32 i = 0; i < Props.Num() && !bCoopRemote; ++i)
	{
		if (Props[i].Sprite.IsValid() && FVector::Dist(Props[i].Base, Where) < R && CanSee(From, Props[i].Base + FVector(0.0, 0.0, Props[i].Height * 0.5), Props[i].Sprite.Get()))
		{
			PropHitDir = (Props[i].Base - Where).GetSafeNormal();
			PropReact(i, false);
		}
	}
	for (int32 i = 0; i < Panes.Num() && !bCoopRemote; ++i)
	{
		const FVector2D C = (Panes[i].A + Panes[i].B) * 0.5;
		const FVector Mid = FOutlawsGeometry::ToUE(C, (Panes[i].Bottom + Panes[i].Top) * 0.5);
		if (FVector::Dist(Mid, Where) < R) ShatterPane(i);
	}
	// other dynamite in the blast goes off a split second later, the nearer first: one after another like dominoes, each
	// blast setting off the next (user 2026-09-28)
	// (where each lies now, carried or thrown there; one close by goes whatever is between: in a pile the sticks hid
	// each other from the blast)
	for (FDynamite& D : Dynamite)
	{
		if (D.bGone || D.bArmed || bCoopRemote) continue;
		if (const UProceduralMeshComponent* DM = D.Mesh.Get()) D.Where = DM->GetComponentLocation();
		const double Dist = FVector::Dist(D.Where, Where);
		if (Dist < R && (Dist < R * 0.35 || CanSee(From, D.Where + FVector(0.0, 0.0, 10.0), D.Mesh.IsValid() ? D.Mesh->GetOwner() : nullptr)))
		{
			D.bArmed = true;
			D.Delay = 0.12f + FMath::FRandRange(0.f, 0.08f) + 0.1f * float(Dist / R);
		}
	}
	// ... and the player's own sticks, thrown and flying or lying where they came down
	if (AOutlawsPlayer* P = Cast<AOutlawsPlayer>(World->GetFirstPlayerController() ? World->GetFirstPlayerController()->GetPawn() : nullptr))
	{
		P->BlastNear(Where, float(R));
	}
	// INF: blast events to the sector it went off in (explosion triggers such as KABLAM), and the near-blast event to
	// every other blast-minded sector within WALL_RADIUS (pdynam.itm 15 u) of it [research: outlaws_shootables_spec]
	if (Inf && LevelData && !bCoopRemote)
	{
		const FOutlawsSector* Own = LevelData->SectorAt(FOutlawsGeometry::ToOutlaws(Where, 0.5));
		if (Own)
		{
			Inf->SectorEvent(Own->Index, EvBlastNear | EvBlastFar);
		}
		const double WallR = BlastWallRadius * ShootU;
		TSet<int32> Minded;
		for (const TPair<int32, TArray<int32>>& Kv : Inf->TriggersBySector) Minded.Add(Kv.Key);
		for (const TPair<int32, TArray<int32>>& Kv : Inf->ElevatorsBySector) Minded.Add(Kv.Key);
		for (const int32 s : Minded)
		{
			if ((Own && s == Own->Index) || !Inf->Live.Sectors.IsValidIndex(s)) continue;
			const FOutlawsSector& S = Inf->Live.Sectors[s];
			FBox B(ForceInit);
			for (const FVector2D& V : S.Verts)
			{
				B += FOutlawsGeometry::ToUE(V.X, S.Floor.Y, V.Y);
				B += FOutlawsGeometry::ToUE(V.X, S.Ceil.Y, V.Y);
			}
			if (B.IsValid && B.ComputeSquaredDistanceToPoint(Where) <= WallR * WallR) Inf->SectorEvent(s, EvBlastNear);
		}
		for (FExplodeWall& X : ExplodeWalls)
		{
			if (!X.bBlown && X.Box.IsValid && X.Box.ComputeSquaredDistanceToPoint(Where) <= WallR * WallR) BlowExplodeWall(X, false, Where);
		}
	}
}

// ---- per frame -------------------------------------------------------------------------------------

void AOutlawsLevelActor::UpdateShootables(float Dt)
{
	OL_PERF_SCOPE("Level: shootables");
	// props: run the opcodes of the playing reaction as their frames come up [EXE wax_NextFrame]
	for (int32 i = Props.Num() - 1; i >= 0; --i)
	{
		FProp& P = Props[i];
		AOutlawsSpriteActor* S = P.Sprite.Get();
		if (!S)
		{
			Props.RemoveAt(i);
			continue;
		}
		if (!P.bActive || !S->Chors.IsValidIndex(P.Current))
		{
			continue;
		}
		const TArray<FIntVector4>& Events = S->Chors[P.Current].Events;
		const int32 Shown = S->GetChorFrame();
		bool bRemoved = false;
		while (P.NextEvent < Events.Num() && Events[P.NextEvent].X <= Shown)
		{
			const FIntVector4 E = Events[P.NextEvent++];
			const FVector At = P.Base + FVector(0.0, 0.0, P.Height * 0.5);
			if (E.Y == -3)
			{
				if (ObjectSounds.IsValidIndex(E.W) && !ObjectSounds[E.W].IsEmpty()) PlayWav(ObjectSounds[E.W], At);
			}
			else if (E.Y == -4)
			{
				PropEnter(i, E.W);          // back to rest (barrels, sacks, pots, bells)
				break;
			}
			else if (E.Y == -5)
			{
				UE_LOG(LogOutlaws, Log, TEXT("OLRT prop %s chor %d event %d (sector %d)"), *P.Name, P.Current, E.W, P.Sector);
				if (E.W == 4) P.bNoCollide = true;
				if (E.W == 5)
				{
					SpawnSparks(At, 2 * FMath::RandHelper(4) + 2);
					PlayWav(TEXT("explode"), At);
				}
				if ((E.W == 3 || E.W == 5) && Inf && P.Sector >= 0 && !P.bCoopRemote) Inf->SectorEvent(P.Sector, EvObject);
			}
			else if (E.Y == -8)
			{
				S->Destroy();                // shattered and gone
				Props.RemoveAt(i);
				bRemoved = true;
				break;
			}
		}
		if (!bRemoved && Props.IsValidIndex(i) && Props[i].bActive && S->IsChorDone() && Props[i].NextEvent >= Events.Num())
		{
			Props[i].bActive = false;       // held on its last frame; the next hit plays the next chor
		}
	}
	UpdatePropWobbles(Dt);
	// glass: the break frames at the ATX rate
	for (FPane& P : Panes)
	{
		if (!P.bBroken || P.Frames.Num() == 0) continue;
		P.T += Dt;
		const int32 F = FMath::Min(P.Frames.Num() - 1, static_cast<int32>(P.T * P.Rate));
		if (F != P.Shown)
		{
			P.Shown = F;
			for (UMaterialInstanceDynamic* M : P.Mids) M->SetTextureParameterValue(TEXT("Tex"), P.Frames[F]);
		}
	}
	// embers fall and fade; blast sprites go when played out
	for (int32 i = Sparks.Num() - 1; i >= 0; --i)
	{
		FSpark& K = Sparks[i];
		AOutlawsSpriteActor* S = K.Sprite.Get();
		K.Life -= Dt;
		if (!S || K.Life <= 0.f || (K.bOneShot && S->IsChorDone()))
		{
			if (S) S->Destroy();
			Sparks.RemoveAt(i);
			continue;
		}
		// blood, clods and glass thin out as they spread, gone by the end of their time (user 2026-09-28)
		if (K.bFade && K.Life0 > 0.f)
		{
			S->SetFade(K.Life / (K.Life0 * 0.7f));
		}
		if (K.bLanded)
		{
			// a lamp's fire burning on the floor: shrinks away over its last second; the player standing in it burns
			// as on a damaging floor (1 a second, the cry with it)
			const float Fade = FMath::Clamp(K.Life, 0.f, 1.f);
			const FVector Base = S->GetActorLocation();
			S->SetActorScale3D(FVector(1.0, K.Scale0.Y * K.Size * Fade, K.Scale0.Z * K.Size * Fade));
			const APawn* Pawn = GetWorld() && GetWorld()->GetFirstPlayerController() ? GetWorld()->GetFirstPlayerController()->GetPawn() : nullptr;
			const AOutlawsPlayer* Pl = Cast<AOutlawsPlayer>(Pawn);
			if (Pl && !bDead && Fade > 0.3f)
			{
				const double Feet = Pl->GetActorLocation().Z - Pl->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
				const double Reach = 0.4 * K.Size * K.Scale0.Y + Pl->GetCapsuleComponent()->GetScaledCapsuleRadius() * 0.5;
				if (FVector2D::Distance(FVector2D(Base), FVector2D(Pl->GetActorLocation())) < Reach && FMath::Abs(Feet - Base.Z) < 60.0)
				{
					FireHurt += Dt;
					if (FireHurt >= 0.25f)
					{
						const double Now = FPlatformTime::Seconds();
						if (Now - LastShriek >= 1.2)
						{
							LastShriek = Now;
							PlayWav(TEXT("shriek1"), Pl->GetActorLocation(), false, true);
						}
						bQuietHurt = true;
						DamagePlayer(FireHurt, 1.f, true);
						bQuietHurt = false;
						FireHurt = 0.f;
					}
				}
			}
			continue;
		}
		if (!K.bOneShot)
		{
			K.Vel.Z -= 490.0 * Dt;
			const FVector From = S->GetActorLocation(), To = From + K.Vel * Dt;
			if (K.bFire)
			{
				FHitResult Hit;
				FCollisionObjectQueryParams Obj;
				Obj.AddObjectTypesToQuery(ECC_WorldStatic);
				Obj.AddObjectTypesToQuery(ECC_WorldDynamic);
				FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsLampFire), false);
				Q.AddIgnoredActor(S);
				if (UWorld* World = GetWorld(); World && World->LineTraceSingleByObjectType(Hit, From, To, Obj, Q))
				{
					if (Hit.ImpactNormal.Z > 0.6)
					{
						// down on a floor: it stays and burns 5 to 9 s, a little bigger or smaller than the next
						S->SetActorLocation(Hit.ImpactPoint + FVector(0.0, 0.0, 0.5));
						K.bLanded = true;
						K.Vel = FVector::ZeroVector;
						K.Life = FMath::FRandRange(5.f, 9.f);
						K.Size = FMath::FRandRange(0.4f, 0.75f);
						if (Hit.GetComponent() && ComponentGroup.Contains(Hit.GetComponent()))
						{
							S->AttachToComponent(Hit.GetComponent(), FAttachmentTransformRules::KeepWorldTransform);   // on a lift: rides it
						}
						continue;
					}
					// a wall: it drops down it
					K.Vel.X = K.Vel.Y = 0.0;
					S->AddActorWorldOffset(FVector(0.0, 0.0, To.Z - From.Z));
					continue;
				}
			}
			if (K.bBlood || K.bDirt || K.bGlass)
			{
				// blood meeting the level stops there and stays as a stain, on a floor or a wall (user 2026-09-28); a clod
				// crumbles
				FHitResult Hit;
				FCollisionObjectQueryParams Obj;
				Obj.AddObjectTypesToQuery(ECC_WorldStatic);
				Obj.AddObjectTypesToQuery(ECC_WorldDynamic);
				FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsBlood), false);
				Q.AddIgnoredActor(S);
				UWorld* World = GetWorld();
				bool bHit = World && World->LineTraceSingleByObjectType(Hit, From, To, Obj, Q);
				// only the level stops them: a thing thrown or swung into the bad guy (the bottle, a 3D item) sits right
				// where the blood flies out, and its stain was left in the air when it moved on (user 2026-09-30); the
				// level's invisible blockers are passed too
				for (int32 Try = 0; bHit && !TakesMarks(Hit.GetComponent()) && Try < 4; ++Try)
				{
					Q.AddIgnoredComponent(Hit.GetComponent());
					bHit = World->LineTraceSingleByObjectType(Hit, From, To, Obj, Q);
				}
				if (bHit)
				{
					if (K.bBlood) AddBloodDecal(Hit.ImpactPoint, Hit.ImpactNormal, Hit.GetComponent());
					S->Destroy();
					Sparks.RemoveAt(i);
					continue;
				}
			}
			S->AddActorWorldOffset(To - From);
		}
	}
	UpdateGlass(Dt);
	// armed dynamite goes off when its moment comes
	for (int32 i = 0; i < Dynamite.Num(); ++i)
	{
		if (!Dynamite[i].bArmed || Dynamite[i].bGone) continue;
		Dynamite[i].Delay -= Dt;
		if (Dynamite[i].Delay <= 0.f) Detonate(i);
	}
}

static TAutoConsoleVariable<int32> CVarOLDecalClip(TEXT("ol.DecalClip"), 1, TEXT("Decals (soot, blood, bullet holes, cracks) cut to the surface they lie on (1), or whole rectangles as before (0)."));

UProceduralMeshComponent* AOutlawsLevelActor::AddDecal(const FVector& At, const FVector& Normal, double Width, double Height, float Roll, UMaterialInterface* Mat,
	const FVector2D& Uv0, const FVector2D& UvSize, UPrimitiveComponent* On, TArray<TWeakObjectPtr<UProceduralMeshComponent>>& Ring, int32 Max,
	const FLinearColor& Tint, bool bSectorLight, bool bClip)
{
	const FVector N = Normal.GetSafeNormal();
	const FVector Ax = FVector::CrossProduct(N, FMath::Abs(N.Z) > 0.99 ? FVector::RightVector : FVector::UpVector).GetSafeNormal().RotateAngleAxis(Roll, N);
	const FVector Ay = FVector::CrossProduct(N, Ax);
	const FVector C = At + N * 0.3;                                  // a hair off the surface
	const FVector Hx = Ax * Width * 0.5, Hy = Ay * Height * 0.5;
	const float L = bSectorLight ? FMath::Clamp(LightAt(At + N * 20.0), 0, 31) / 31.f : 1.f;
	const FLinearColor Lc(L * Tint.R, L * Tint.G, L * Tint.B);
	// (BUG-DECALEDGE) cut to the surface it lands on: the level mesh's own triangles in its plane, so it never hangs out over a wall's
	// end, a floor's edge or a doorway; a surface that cannot be read keeps the whole rectangle
	TArray<OutlawsDecalClip::FPiece> Pieces;
	bool bPieces = false;
	if (bClip && On && CVarOLDecalClip.GetValueOnGameThread() != 0 && OutlawsDecalClip::Clip(Cast<UProceduralMeshComponent>(On), C, N, Ax, Ay, Width, Height, Pieces))
	{
		if (Pieces.Num() == 0) return nullptr;          // wholly off the surface it was aimed at
		bPieces = true;
	}
	UProceduralMeshComponent* M = NewMeshComponent(TEXT("OutlawsDecal"), false);
	if (bPieces)
	{
		TArray<FVector> Pos, Nor;
		TArray<int32> Tri;
		TArray<FVector2D> Uv;
		TArray<FLinearColor> Col;
		for (const OutlawsDecalClip::FPiece& Pc : Pieces)
		{
			const int32 B = Pos.Num();
			for (int32 k = 0; k < 3; ++k)
			{
				Pos.Add(C + Ax * Pc.P[k].X + Ay * Pc.P[k].Y);
				Nor.Add(N);
				Col.Add(Lc);
				Uv.Add(FVector2D(Uv0.X + (Pc.P[k].X / Width + 0.5) * UvSize.X, Uv0.Y + UvSize.Y - (Pc.P[k].Y / Height + 0.5) * UvSize.Y));
			}
			Tri.Append({ B, B + 1, B + 2, B, B + 2, B + 1 });
		}
		M->CreateMeshSection_LinearColor(0, Pos, Tri, Nor, Uv, Col, TArray<FProcMeshTangent>(), false);
	}
	else
	M->CreateMeshSection_LinearColor(0, { C - Hx - Hy, C + Hx - Hy, C + Hx + Hy, C - Hx + Hy }, { 0, 2, 1, 0, 3, 2, 0, 1, 2, 0, 2, 3 }, { N, N, N, N },
		{ FVector2D(Uv0.X, Uv0.Y + UvSize.Y), FVector2D(Uv0.X + UvSize.X, Uv0.Y + UvSize.Y), FVector2D(Uv0.X + UvSize.X, Uv0.Y), Uv0 },
		TArray<FLinearColor>{ Lc, Lc, Lc, Lc }, TArray<FProcMeshTangent>(), false);
	M->SetMaterial(0, Mat);
	if (On && ComponentGroup.Contains(On))
	{
		// on a lift floor or a door: it moves with it
		M->SetUsingAbsoluteLocation(false);
		M->SetUsingAbsoluteRotation(false);
		M->SetUsingAbsoluteScale(false);
		M->AttachToComponent(On, FAttachmentTransformRules::KeepWorldTransform);
	}
	ObjectMeshes.Add(M);
	Ring.Add(M);
	while (Ring.Num() > Max)
	{
		if (UProceduralMeshComponent* Old = Ring[0].Get())
		{
			ObjectMeshes.Remove(Old);
			Old->DestroyComponent();
		}
		Ring.RemoveAt(0);
	}
	return M;
}

void AOutlawsLevelActor::DebugClearScorches()
{
	for (TWeakObjectPtr<UProceduralMeshComponent>& W : Scorches)
	{
		if (UProceduralMeshComponent* M = W.Get())
		{
			ObjectMeshes.Remove(M);
			M->DestroyComponent();
		}
	}
	Scorches.Reset();
}

void AOutlawsLevelActor::AddBloodDecal(const FVector& At, const FVector& Normal, UPrimitiveComponent* On)
{
	const TSharedPtr<OutlawsTextures::FSpriteAtlas> Atlas = GetAtlas(TEXT("blood.nwx"), false);
	if (!Atlas || !Atlas->Texture || !WorldMaskedMaterial || Atlas->Grid.Num() == 0)
	{
		return;
	}
	// one of the spatter's frames, turned any which way, on the floor or wall it struck
	const int32 Cell = Atlas->Grid[FMath::RandRange(0, Atlas->Grid.Num() - 1)];
	const FVector2D Uv0(double(Cell % Atlas->Cols) / Atlas->Cols, double(Cell / Atlas->Cols) / Atlas->Rows);
	const FVector2D UvSize(1.0 / Atlas->Cols, 1.0 / Atlas->Rows);
	const double Size = FMath::FRandRange(0.9f, 1.4f) * Atlas->CellW / 16.0 * FOutlawsGeometry::UnitsToCm;
	const double Aspect = double(Atlas->CellH) / FMath::Max(1, Atlas->CellW);
	UTexture* T = bHDTextures ? SpriteHdFor(Atlas) : nullptr;
	if (!BloodMat)
	{
		BloodMat = UMaterialInstanceDynamic::Create(WorldMaskedMaterial, this);
	}
	BloodMat->SetTextureParameterValue(TEXT("Tex"), T ? T : Atlas->Texture);
	// darker once it has soaked in (the spatter frames read pink lying lit on a surface)
	AddDecal(At, Normal, Size, Size * Aspect, FMath::FRandRange(0.f, 360.f), BloodMat, Uv0, UvSize, On, BloodDecals, MaxBloodDecals, FLinearColor(0.62f, 0.3f, 0.3f));
}

bool AOutlawsLevelActor::TakesMarks(const UPrimitiveComponent* C) const
{
	return C && C->GetOwner() == this && C->IsVisible() && !C->bHiddenInGame && C != SkyMesh && C != SkySurfaceMesh && C != WaterMesh;
}

UMaterialInstanceDynamic* AOutlawsLevelActor::DarkMarkMaterial(bool bCrack)
{
	TObjectPtr<UMaterialInstanceDynamic>& Mid = bCrack ? CrackMat : ScorchMat;
	if (Mid) return Mid;
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_Soot.M_OLR_Soot"));
	if (!Base) return nullptr;
	// drawn once: soft soot (dark in the middle, thinning out to a ragged, speckled edge) or a burst of cracks
	constexpr int32 D = 64;
	TArray<FColor> Px;
	Px.SetNum(D * D);
	FRandomStream R(bCrack ? 0xC4AC : 0x5007);
	if (!bCrack)
	{
		float Edge[16];
		for (float& E : Edge) E = R.FRandRange(0.8f, 1.f);
		for (int32 y = 0; y < D; ++y)
		{
			for (int32 x = 0; x < D; ++x)
			{
				const float Dx = (x + 0.5f - D * 0.5f) / (D * 0.5f), Dy = (y + 0.5f - D * 0.5f) / (D * 0.5f);
				const float A = (FMath::Atan2(Dy, Dx) / (2.f * PI) + 0.5f) * 16.f;
				const int32 K = FMath::FloorToInt(A) % 16;
				const float Rim = FMath::Lerp(Edge[K], Edge[(K + 1) % 16], A - FMath::FloorToFloat(A));
				const float Rr = FMath::Sqrt(Dx * Dx + Dy * Dy) / Rim;
				const float Soft = FMath::Clamp(1.f - Rr, 0.f, 1.f);
				const float Al = FMath::Clamp(Soft * Soft * (3.f - 2.f * Soft) * R.FRandRange(0.8f, 1.1f), 0.f, 1.f);
				const uint8 C = uint8(8 + R.RandRange(0, 10));
				Px[y * D + x] = FColor(C + 4, C + 2, C, uint8(Al * 255.f));
			}
		}
	}
	else
	{
		for (FColor& Q : Px) Q = FColor(40, 34, 30, 0);
		auto Dot = [&](int32 X, int32 Y, uint8 A)
		{
			if (X >= 0 && Y >= 0 && X < D && Y < D) Px[Y * D + X].A = FMath::Max(Px[Y * D + X].A, A);
		};
		// jagged branches out from where the knuckles landed, thinning as they go
		for (int32 b = 0; b < 7; ++b)
		{
			float X = D * 0.5f, Y = D * 0.5f, Ang = R.FRandRange(0.f, 2.f * PI);
			const int32 Len = R.RandRange(14, 28);
			for (int32 i = 0; i < Len; ++i)
			{
				Ang += R.FRandRange(-0.5f, 0.5f);
				X += FMath::Cos(Ang);
				Y += FMath::Sin(Ang);
				const uint8 A = uint8(255 - i * 150 / Len);
				Dot(int32(X), int32(Y), A);
				if (i < Len / 2) Dot(int32(X) + 1, int32(Y), A);
				if (i == Len / 2 && R.FRand() < 0.6f)
				{
					float X2 = X, Y2 = Y, A2 = Ang + R.FRandRange(0.6f, 1.2f) * (R.FRand() < 0.5f ? -1.f : 1.f);
					for (int32 j = 0; j < Len / 2; ++j) { A2 += R.FRandRange(-0.4f, 0.4f); X2 += FMath::Cos(A2); Y2 += FMath::Sin(A2); Dot(int32(X2), int32(Y2), uint8(160 - j * 100 / Len)); }
				}
			}
		}
		for (int32 y = -3; y <= 3; ++y) for (int32 x = -3; x <= 3; ++x) if (x * x + y * y <= 9) Dot(D / 2 + x, D / 2 + y, 200);   // the crushed spot
	}
	UTexture2D* T = OutlawsTextures::MakeTextureBGRA(D, D, Px, true, TF_Trilinear, TA_Clamp);
	Mid = UMaterialInstanceDynamic::Create(Base, this);
	if (T) Mid->SetTextureParameterValue(TEXT("Tex"), T);
	return Mid;
}

void AOutlawsLevelActor::AddScorchMarks(const FVector& Where)
{
	UWorld* World = GetWorld();
	UMaterialInstanceDynamic* Mat = World ? DarkMarkMaterial(false) : nullptr;
	if (!Mat) return;
	// the nearest surfaces all round (floor, walls, ceiling): a mark on each within reach, the nearer the bigger and
	// darker; one mark per patch of surface
	constexpr double Reach = 350.0;
	static const FVector Dirs[] = {
		FVector(0, 0, -1), FVector(0, 0, 1), FVector(1, 0, 0), FVector(-1, 0, 0), FVector(0, 1, 0), FVector(0, -1, 0),
		FVector(1, 1, -0.6), FVector(1, -1, -0.6), FVector(-1, 1, -0.6), FVector(-1, -1, -0.6),
		FVector(1, 1, 0.6), FVector(1, -1, 0.6), FVector(-1, 1, 0.6), FVector(-1, -1, 0.6) };
	FCollisionObjectQueryParams Obj;
	Obj.AddObjectTypesToQuery(ECC_WorldStatic);
	Obj.AddObjectTypesToQuery(ECC_WorldDynamic);
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsScorch), false);
	TArray<FHitResult> Hits;
	// (user 2026-09-29: the soot did not always come.)  The rays start a hand above the blast (a stick lying on the
	// floor goes off at or just under its surface, and a ray from inside the floor never meets it) and look past what
	// is not the level (the stick itself, a sprite, a prop) to the first surface that is
	const FVector From = Where + FVector(0.0, 0.0, 15.0);
	for (const FVector& D0 : Dirs)
	{
		TArray<FHitResult> Along;
		World->LineTraceMultiByObjectType(Along, From, From + D0.GetSafeNormal() * Reach, Obj, Q);
		for (const FHitResult& H : Along)
		{
			if (TakesMarks(H.GetComponent()))
			{
				Hits.Add(H);
				break;
			}
		}
	}
	// one mark per surface (the floor, each wall, the ceiling), where it came nearest
	Hits.Sort([](const FHitResult& A, const FHitResult& B) { return A.Distance < B.Distance; });
	TArray<TPair<FVector, FVector>> Made;
	for (const FHitResult& H : Hits)
	{
		const FVector N(H.ImpactNormal), At(H.ImpactPoint);
		bool bSame = false;
		for (const TPair<FVector, FVector>& M : Made)
		{
			bSame |= FVector::DotProduct(M.Value, N) > 0.95 && FMath::Abs(FVector::DotProduct(At - M.Key, N)) < 20.0;
		}
		if (bSame) continue;
		// the nearest point of that surface to the blast, not where the ray happened to land
		const FVector Foot = Where - N * FVector::DotProduct(Where - At, N);
		const double Dist = FVector::Dist(Where, Foot);
		const double K = FMath::Clamp(Dist / Reach, 0.0, 1.0);
		const double Size = FMath::Lerp(560.0, 180.0, K);     // (twice the first size, user 2026-09-29)
		Made.Emplace(At, N);
		const float Strength = FMath::Lerp(0.95f, 0.3f, float(K));
		AddDecal(Foot, N, Size, Size, FMath::FRandRange(0.f, 360.f), Mat, FVector2D::ZeroVector, FVector2D(1.0, 1.0),
			H.GetComponent(), Scorches, MaxScorches, FLinearColor(Strength, Strength, Strength), false);
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT blast soot: %d marks"), Made.Num());
}

void AOutlawsLevelActor::AddPunchMark(const FHitResult& Hit)
{
	UMaterialInstanceDynamic* Mat = TakesMarks(Hit.GetComponent()) ? DarkMarkMaterial(true) : nullptr;
	if (!Mat) return;
	const double Size = FMath::FRandRange(16.f, 24.f);
	AddDecal(Hit.ImpactPoint, Hit.ImpactNormal, Size, Size, FMath::FRandRange(0.f, 360.f), Mat, FVector2D::ZeroVector, FVector2D(1.0, 1.0),
		Hit.GetComponent(), Cracks, MaxCracks, FLinearColor(0.85f, 0.85f, 0.85f), false);
}

void AOutlawsLevelActor::AddBulletHole(const FHitResult& Hit)
{
	UPrimitiveComponent* C = Hit.GetComponent();
	// the level's own visible surfaces only: not a bad guy, the sky, water or an invisible blocker
	if (!Hit.bBlockingHit || !TakesMarks(C) || !WorldMaskedMaterial)
	{
		return;
	}
	if (!BulletHoleMat)
	{
		// the game has no bullet hole: a black pit in a ragged ring of lighter splinters (it has to show on dark wood),
		// drawn once
		constexpr int32 D = 32;
		TArray<FColor> Px;
		Px.SetNum(D * D);
		FRandomStream R(0x0B01E);
		float Edge[12];
		for (float& E : Edge) E = R.FRandRange(0.72f, 1.f);
		for (int32 y = 0; y < D; ++y)
		{
			for (int32 x = 0; x < D; ++x)
			{
				const float Dx = (x + 0.5f - D * 0.5f) / (D * 0.5f), Dy = (y + 0.5f - D * 0.5f) / (D * 0.5f);
				const float Rr = FMath::Sqrt(Dx * Dx + Dy * Dy);
				const float A = (FMath::Atan2(Dy, Dx) / (2.f * PI) + 0.5f) * 12.f;
				const int32 K = FMath::FloorToInt(A) % 12;
				const float Rim = 0.95f * FMath::Lerp(Edge[K], Edge[(K + 1) % 12], A - FMath::FloorToFloat(A));
				const uint8 Nz = uint8(R.RandRange(0, 10));
				Px[y * D + x] = Rr < 0.34f ? FColor(6 + Nz / 2, 5 + Nz / 2, 4 + Nz / 2, 255)
					: Rr < 0.46f ? FColor(40 + Nz, 34 + Nz, 28 + Nz, 255)
					: Rr < Rim ? FColor(112 + Nz * 3, 97 + Nz * 3, 76 + Nz * 2, 255)
					: FColor(112, 97, 76, 0);
			}
		}
		UTexture2D* T = OutlawsTextures::MakeTextureBGRA(D, D, Px, true, TF_Trilinear, TA_Clamp);
		BulletHoleMat = UMaterialInstanceDynamic::Create(WorldMaskedMaterial, this);
		if (T) BulletHoleMat->SetTextureParameterValue(TEXT("Tex"), T);
	}
	const double Size = FMath::FRandRange(5.f, 7.f);
	AddDecal(Hit.ImpactPoint, Hit.ImpactNormal, Size, Size, FMath::FRandRange(0.f, 360.f), BulletHoleMat, FVector2D::ZeroVector, FVector2D(1.0, 1.0), C, BulletHoles, MaxBulletHoles);
}

UProceduralMeshComponent* AOutlawsLevelActor::StartBloodPool(const FVector& Body, float Share, bool bDrop)
{
	UWorld* World = GetWorld();
	if (!World || !WorldMaskedMaterial) return nullptr;
	float WaterTop = 0.f;
	if (WaterAt(Body, WaterTop)) return nullptr;
	// the floor under the body, past what is not the level (the body itself, sprites, props)
	FCollisionObjectQueryParams Obj;
	Obj.AddObjectTypesToQuery(ECC_WorldStatic);
	Obj.AddObjectTypesToQuery(ECC_WorldDynamic);
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsBloodPool), false);
	TArray<FHitResult> Along;
	World->LineTraceMultiByObjectType(Along, Body + FVector(0.0, 0.0, 30.0), Body - FVector(0.0, 0.0, 400.0), Obj, Q);
	const FHitResult* Floor = nullptr;
	for (const FHitResult& H : Along)
	{
		if (TakesMarks(H.GetComponent())) { Floor = &H; break; }
	}
	if (!Floor || Floor->ImpactNormal.Z < 0.6) return nullptr;
	if (!BloodPoolMat)
	{
		// the pool: deep red, darker and thicker in the middle, a ragged rounded edge with a lobe or two, drawn once
		constexpr int32 D = 64;
		TArray<FColor> Px;
		Px.SetNum(D * D);
		FRandomStream R(0xB100D);
		float Edge[16];
		for (float& E : Edge) E = R.FRandRange(0.74f, 1.f);
		for (int32 y = 0; y < D; ++y)
		{
			for (int32 x = 0; x < D; ++x)
			{
				const float Dx = (x + 0.5f - D * 0.5f) / (D * 0.5f), Dy = (y + 0.5f - D * 0.5f) / (D * 0.5f);
				const float Rr = FMath::Sqrt(Dx * Dx + Dy * Dy);
				const float A = (FMath::Atan2(Dy, Dx) / (2.f * PI) + 0.5f) * 16.f;
				const int32 K = FMath::FloorToInt(A) % 16;
				const float Rim = 0.96f * FMath::Lerp(Edge[K], Edge[(K + 1) % 16], A - FMath::FloorToFloat(A));
				const float In = FMath::Clamp(Rr / Rim, 0.f, 1.f);
				// the spatter's own reds (blood.nwx/.spx: 114,33,33 to 147,57,57; user 2026-09-29: the pool was another
				// shade), deeper in the middle; the same tint as the spatters goes on it below
				const float Nz = float(R.RandRange(0, 8));
				const float Mix = In * In;
				const uint8 Rd = uint8(FMath::Lerp(114.f, 147.f, Mix) + Nz), Gb = uint8(FMath::Lerp(33.f, 57.f, Mix) + Nz * 0.6f);
				Px[y * D + x] = Rr < Rim ? FColor(Rd, Gb, Gb, 255) : FColor(130, 46, 46, 0);
			}
		}
		UTexture2D* T = OutlawsTextures::MakeTextureBGRA(D, D, Px, true, TF_Trilinear, TA_Clamp);
		BloodPoolMat = UMaterialInstanceDynamic::Create(WorldMaskedMaterial, this);
		if (T) BloodPoolMat->SetTextureParameterValue(TEXT("Tex"), T);
	}
	// a whole pool 1.1 to 1.5 m; a body moved on bleeds what it has left (its area: the size by the square root); a drop
	// off a carried body (user 2026-10-01) 5 to 11 cm, there at once
	const double Max = bDrop ? FMath::FRandRange(5.f, 11.f) : FMath::FRandRange(110.f, 150.f) * FMath::Sqrt(FMath::Clamp(Share, 0.05f, 1.f));
	UProceduralMeshComponent* M = AddDecal(Floor->ImpactPoint + Floor->ImpactNormal * (bDrop ? 0.06 : 0.05), Floor->ImpactNormal, Max, Max * FMath::FRandRange(0.8f, 1.f),
		FMath::FRandRange(0.f, 360.f), BloodPoolMat, FVector2D::ZeroVector, FVector2D(1.0, 1.0), Floor->GetComponent(), bDrop ? BloodDropRing : BloodPoolRing,
		bDrop ? 60 : MaxBloodPools, FLinearColor(0.62f, 0.3f, 0.3f), true, false);       // (the spatters' tint, AddBloodDecal; a pool keeps its four corners: it grows)
	if (bDrop) return M;
	const FProcMeshSection* S = M ? M->GetProcMeshSection(0) : nullptr;
	if (!S || S->ProcVertexBuffer.Num() < 4) return nullptr;
	FBloodPool P;
	P.Mesh = M;
	for (int32 i = 0; i < 4; ++i)
	{
		P.Corners[i] = S->ProcVertexBuffer[i].Position;      // (the mesh's own space: it may ride a lift)
		P.Centre += P.Corners[i] * 0.25;
	}
	BloodPools.Add(P);
	UpdateBloodPools(0.f);
	SetActorTickEnabled(true);
	return M;
}

float AOutlawsLevelActor::StopBloodPool(UProceduralMeshComponent* Pool)
{
	// (no longer in the list: it had spread all the way)
	for (int32 i = 0; i < BloodPools.Num(); ++i)
	{
		if (BloodPools[i].Mesh.Get() != Pool) continue;
		const float T = FMath::Clamp(BloodPools[i].Age / 15.f, 0.f, 1.f);
		BloodPools.RemoveAtSwap(i);
		return 1.f - FMath::Pow(1.f - T, 3.f);           // (as far as it had spread: UpdateBloodPools' ease)
	}
	return 1.f;
}


void AOutlawsLevelActor::UpdateBloodPools(float Dt)
{
	OL_PERF_SCOPE("Level: blood pools");
	// spreading out over 15 s from a tenth of its size, quickly at first, slower and slower (an ease-out)
	for (int32 i = BloodPools.Num() - 1; i >= 0; --i)
	{
		FBloodPool& P = BloodPools[i];
		UProceduralMeshComponent* M = P.Mesh.Get();
		if (!M)
		{
			BloodPools.RemoveAtSwap(i);
			continue;
		}
		P.Age += Dt;
		const float T = FMath::Clamp(P.Age / 15.f, 0.f, 1.f);
		const float Sc = 0.1f + 0.9f * (1.f - FMath::Pow(1.f - T, 3.f));
		TArray<FVector> Pos;
		for (const FVector& C : P.Corners) Pos.Add(P.Centre + (C - P.Centre) * Sc);
		if (!OutlawsPerf::FreezeMeshUpdates()) M->UpdateMeshSection_LinearColor(0, Pos, TArray<FVector>(), TArray<FVector2D>(), TArray<FLinearColor>(), TArray<FProcMeshTangent>());
		if (T >= 1.f) BloodPools.RemoveAtSwap(i);
	}
}

FString AOutlawsLevelActor::ShootablesReport() const
{
	FString Glass;
	if (Panes.Num() > 0 && Panes[0].Mesh)
	{
		// the first pane's look: its light and texture, for diagnosis
		const FPane& P = Panes[0];
		const FProcMeshSection* Sec = P.Mesh->GetProcMeshSection(0);
		UTexture* T = nullptr;
		if (P.Mids.Num() > 0) P.Mids[0]->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Tex")), T);
		Glass = FString::Printf(TEXT(" | pane0 sections %d verts %d colour %s tex %s parent %s"), P.Mesh->GetNumSections(),
			Sec ? Sec->ProcVertexBuffer.Num() : -1, Sec && Sec->ProcVertexBuffer.Num() ? *Sec->ProcVertexBuffer[0].Color.ToString() : TEXT("-"),
			T ? *T->GetName() : TEXT("none"), P.Mids.Num() && P.Mids[0]->Parent ? *P.Mids[0]->Parent->GetName() : TEXT("none"));
	}
	return FString::Printf(TEXT("%d shootable props, %d glass panes, %d dynamite, %d object sounds%s"), Props.Num(), Panes.Num(), Dynamite.Num(), ObjectSounds.Num(), *Glass);
}

#if WITH_OUTLAWS_TESTS
FString AOutlawsLevelActor::SelfTestShootables()
{
	// one shot at a bottle, a barrel and a window from 2 m, then a dynamite stick
	TArray<FString> Out;
	// a swing that starts 1 m in front of a prop and heads away from it must not hit it (the "behind the start" case)
	for (const FProp& P : Props)
	{
		if (P.Sprite.IsValid() && !P.bNoCollide && P.Name.StartsWith(TEXT("BOTTLE")))
		{
			const FVector Mid = P.Base + FVector(0.0, 0.0, P.Height * 0.5);
			FVector AwayEnd = Mid + FVector(130.0, 0.0, 0.0);
			const bool bAwayHit = ShootAlong(Mid + FVector(100.0, 0.0, 0.0), AwayEnd);
			Out.Add(FString::Printf(TEXT("swing away from %s hit %d (expect 0)"), *P.Name, bAwayHit ? 1 : 0));
			break;
		}
	}
	auto ShootAt2m = [this](const FVector& Target) -> bool
	{
		// a shot along +X from 2 m short of the target to 2 m past it
		const FVector From = Target - FVector(200.0, 0.0, 0.0);
		FVector End = Target + FVector(200.0, 0.0, 0.0);
		return ShootAlong(From, End);
	};
	for (const TCHAR* Kind : { TEXT("BOTTLE"), TEXT("BARREL"), TEXT("SACK"), TEXT("PAN") })
	{
		for (const FProp& P : Props)
		{
			if (P.Name.StartsWith(Kind) && P.Sprite.IsValid())
			{
				const bool bHit = ShootAt2m(P.Base + FVector(0.0, 0.0, P.Height * 0.5));
				Out.Add(FString::Printf(TEXT("%s hit %d"), *P.Name, bHit ? 1 : 0));
				break;
			}
		}
	}
	if (Panes.Num() > 0)
	{
		const FPane& P = Panes[0];
		const FVector2D C = (P.A + P.B) * 0.5, N(-(P.B - P.A).Y, (P.B - P.A).X);
		const FVector2D N1 = N.GetSafeNormal();
		const double Y = (P.Bottom + P.Top) * 0.5 * ShootU;
		const FVector A((C.Y + N1.Y) * ShootU, (C.X + N1.X) * ShootU, Y), B((C.Y - N1.Y) * ShootU, (C.X - N1.X) * ShootU, Y);
		ShatterAlong(A, B);
		Out.Add(FString::Printf(TEXT("window broken %d"), Panes[0].bBroken ? 1 : 0));
	}
	if (Dynamite.Num() > 0)
	{
		Detonate(0);
		Out.Add(TEXT("dynamite detonated"));
	}
	return FString::Join(Out, TEXT(", "));
}
#endif // WITH_OUTLAWS_TESTS

bool AOutlawsLevelActor::TurnPropByHand(const FVector& Hand)
{
	// the nearest NUDGE prop whose cylinder the hand is at (its radius and 15 cm more across, its height and 15 cm
	// more up and down): the valve wheels have nothing else that works them (user 2026-09-28: the mill's waterways)
	int32 Best = -1;
	double BestD = 1e30;
	for (int32 i = 0; i < Props.Num(); ++i)
	{
		const FProp& P = Props[i];
		if (!P.bNudge || !P.Sprite.IsValid() || P.bActive) continue;
		const double Across = FVector::Dist2D(Hand, P.Base);
		const double Up = Hand.Z - P.Base.Z;
		if (Across > P.Radius + 15.0 || Up < -15.0 || Up > P.Height + 15.0) continue;
		if (Across < BestD) { BestD = Across; Best = i; }
	}
	if (Best < 0) return false;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT hand works %s (sector %d)"), *Props[Best].Name, Props[Best].Sector);
	PropReact(Best, true);
	return true;
}

bool AOutlawsLevelActor::PushByHand(const FVector& Point)
{
	if (!Inf) return false;
	TArray<TPair<int32, int32>> Walls;
	WallsAt(Point, PlayerSector, Walls, true);
	bool bUsed = false;
	for (const TPair<int32, int32>& SW : Walls)
	{
		if (!Inf->TriggersByWall.Contains(FOutlawsInfRuntime::WallKey(SW.Key, SW.Value))) continue;     // switches only
		if (Inf->WallSlides(SW.Key, SW.Value)) continue;     // a bar switch (the cliff's): the grip alone moves it (user 2026-09-29)
		bUsed |= HandEvent(SW.Key, SW.Value, SW.Key == PlayerSector ? OutlawsInf::EvNudgeFront : OutlawsInf::EvNudgeBack);
	}
	if (!bUsed && KeySectors.Num() > 0)
	{
		// (REQ-PIANO) a press onto the piano's keys: the keys sector's nudge, as the use key's on its edge in the original
		const double U = FOutlawsGeometry::UnitsToCm;
		const FOutlawsSector* On = Inf->Live.SectorAt(FOutlawsGeometry::ToOutlaws(Point, 0.1));
		if (On && KeySectors.Contains(On->Index) && FMath::Abs(Point.Z / U - On->Floor.Y) < 0.3)
		{
			const bool bTaken = HandEvent(On->Index, -1, On->Index == PlayerSector ? OutlawsInf::EvNudgeFront : OutlawsInf::EvNudgeBack);
			UE_LOG(LogOutlaws, Log, TEXT("OLRT hand pressed the piano keys (sector %d) at %s: %s"), On->Index, *Point.ToString(), bTaken ? TEXT("taken") : TEXT("not taken"));
			return bTaken;
		}
	}
	if (bUsed)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT hand pushed a switch at %s"), *Point.ToString());
	}
	return bUsed;
}

int32 AOutlawsLevelActor::DebugShootProp(const FString& Name)
{
	int32 N = 0;
	for (int32 i = 0; i < Props.Num(); ++i)
	{
		if (Props[i].Name.Equals(Name, ESearchCase::IgnoreCase) && Props[i].Sprite.IsValid())
		{
			const AOutlawsSpriteActor* S = Props[i].Sprite.Get();
			for (int32 c = 0; c < S->Chors.Num(); ++c)
			{
				FString Ev;
				for (const FIntVector4& E : S->Chors[c].Events) Ev += FString::Printf(TEXT("[%d:%d %d %d] "), E.X, E.Y, E.Z, E.W);
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT prop %s chor %d: %d frames, events %s"), *Props[i].Name, c, S->Chors[c].Frames, *Ev);
			}
			PropHitDir = FVector::ForwardVector;
			PropReact(i, false);
			++N;
		}
	}
	return N;
}

FString AOutlawsLevelActor::DebugKnockModelProp(bool bKnock)
{
	static int32 Which = -1;
	if (bKnock)
	{
		for (int32 i = 0; i < Props.Num() && Which < 0; ++i)
		{
			const AOutlawsSpriteActor* S = Props[i].Sprite.Get();
			if (S && S->Model && Props[i].bShoot && S->Chors.Num() > 1 && !S->ModelStem.StartsWith(TEXT("BOTTLE"))) Which = i;
		}
		if (Which < 0) return TEXT("no model prop");
		PropHitDir = FVector(1.0, 0.0, -0.1);
		PropReact(Which, false);
	}
	if (!Props.IsValidIndex(Which) || !Props[Which].Sprite.IsValid()) return TEXT("gone");
	const AOutlawsSpriteActor* S = Props[Which].Sprite.Get();
	const UPrimitiveComponent* M = S->Model;
	if (!M) return TEXT("no model");
	const FBox B = M->Bounds.GetBox();
	return FString::Printf(TEXT("%s (%s) model %s visible %d sprite-shown %d rot %s loc %s (foot %s) bottom z %.1f top %.1f"),
		*Props[Which].Name, *S->ModelStem, *M->GetName(), M->IsVisible() ? 1 : 0, (S->Quad && S->Quad->IsVisible()) ? 1 : 0,
		*M->GetComponentRotation().ToString(), *M->GetComponentLocation().ToString(), *S->GetActorLocation().ToString(), B.Min.Z, B.Max.Z);
}

int32 AOutlawsLevelActor::DebugNudgeAllProps()
{
	int32 N = 0;
	for (int32 i = 0; i < Props.Num(); ++i)
	{
		if (Props[i].bNudge && Props[i].Sprite.IsValid())
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT nudging %s (sector %d, %d chors)"), *Props[i].Name, Props[i].Sector, Props[i].Sprite->Chors.Num());
			PropReact(i, true);
			++N;
		}
	}
	return N;
}
