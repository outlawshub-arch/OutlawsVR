// AOutlawsPlayer, weapons half: the VR weapon wheel and the placeholder guns held in the hands.
#include "OutlawsBeacon.h"
#include "OutlawsFlags.h"
#include "OutlawsTutorialCard.h"
#include "OutlawsPlayer.h"
#include "OutlawsDevChecks.h"
#include "OutlawsViewCone.h"
#include "OutlawsDeviceProfile.h"
#include "TimerManager.h"
#include "OutlawsFeel.h"
#include "OutlawsInk.h"
#include "OutlawsPerf.h"
#include "OutlawsLab.h"
#include "OutlawsDeathmatch.h"
#include "OutlawsFormats.h"
#include "OutlawsAchievements.h"
#include "Engine/CanvasRenderTarget2D.h"
#include "Engine/Canvas.h"
#include "Engine/Font.h"
#include "CanvasItem.h"
#include "OutlawsRuntime.h"
#include "OutlawsBoxHand.h"
#include "OutlawsObjMesh.h"
#include "Camera/CameraComponent.h"
#include "MotionControllerComponent.h"
#include "ProceduralMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Kismet/GameplayStatics.h"
#include "OutlawsLevelActor.h"
#include "OutlawsEnemy.h"
#include "OutlawsCoop.h"
#include "OutlawsSpriteActor.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Engine/World.h"
#include "CollisionQueryParams.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Misc/CommandLine.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Components/CapsuleComponent.h"
#include "ImageUtils.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsTextures.h"
#include "Components/AudioComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "OutlawsGunBuilder.h"
#include "OutlawsRemotePlayer.h"
#include "OutlawsShotTests.h"


namespace
{
	/**
	 * Placeholder guns (private VE models, see PLAN.md).  Offset = where the model's origin (the
	 * GTA right-hand bone) sits in the right hand's frame so its grip lands in the fist (fingers
	 * +X, thumb +Z, palm facing -Y); BoreFix levels the built-in bore tilt.  The left hand mirrors.
	 */
	struct FWeaponDef
	{
		const TCHAR* Label;
		const TCHAR* Obj;
		FVector Offset;
		FRotator BoreFix;
		// Outlaws ijpistol / ijrifle (research/outlaws_weapons_items.md 1.2, 8)
		const TCHAR* AmmoItem;  // the inventory item a round comes from (ibullit, icart)
		int32 Capacity;
		float Cycle;            // seconds between aimed shots
		float FanCycle;         // revolver fan fire; 0 = none
		float Variance;         // degrees
		float RangeCm;
		float ReloadEach;       // seconds per round loaded by hand
		const TCHAR* FireSound;
		const TCHAR* EmptySound;
		const TCHAR* ReloadSound;
		// model points (cm, the right-hand model's space)
		FVector Muzzle;
		FVector Hammer;         // fan fire: the free hand sweeps past here
		FVector LoadPoint;      // reload: the free hand held here (cylinder / magazine)
		bool bDualWield;        // the original's pair (revolvers, sawed-offs); a hand each is now by the count carried (Equip)
		// spent cases: where they leave the gun and which way (model space), and when: a rifle throws
		// one out per shot, a revolver keeps them until reloading pushes them out one per round
		FVector EjectPort;
		FVector EjectDir;
		bool bEjectOnShot;
		float Kick;             // cm the gun slides back along the barrel per shot
		float KickPitch;        // degrees the muzzle rises per shot
		float Damage;           // health points per hit (ITM DAMAGE_1; bad guys have 2..4)
		// the shotguns, gatling and throwables (research/outlaws_weapons_items.md 1.2, appendix A)
		int32 Pellets;          // SHOT_MULTIPLES (0 = 1)
		float Spread;           // SPREAD_1: each pellet scatters up to this many degrees
		int32 RoundsPerShot;    // the double guns fire both barrels (0 = 1)
		bool bAuto;             // held trigger keeps firing; the belt reloads itself (AUTO_RELOAD)
		int32 Throw;            // 1 dynamite (light, throw, fuse), 2 knife (throw, jab)
		const TCHAR* Model3do;  // the game's own model instead of a placeholder OBJ
		float ModelLength;      // cm the 3DO is scaled to, along its longest side
		float HoldPitch;        // degrees the gun sits nose-down in the hand beyond levelling its bore
		bool bFlipModel;        // the 3DO's front is its -X end
		float MountLift;        // (set on load) the mounted gun's height from its middle to its feet
		bool bTwoHand;          // a long gun: the free hand takes the forend (one hand: half the accuracy)
		bool bShoulder;         // ... and has a stock (the virtual stock lays every two-handed gun on the eye)
		float ItemScale = 1.f;  // (set on load) the held model to the level item's own size (a thrown stick grows to it)
	};
	FWeaponDef GWeapons[] = {        // not const: 3DO weapons learn their muzzle on load
		{ TEXT("Revolver"), TEXT("python/python_frame.obj"), FVector(-6.6, 2.7, 0.5), FRotator(-3.9, -0.9, 0.0),
			TEXT("ibullit"), 6, 0.5f, 0.2f, 1.5f, 9000.f, 0.23f, TEXT("pistol2"), TEXT("pistout"), TEXT("preload"),
			FVector(33.8, -3.4, 10.3), FVector(10.5, -3.4, 12.0), FVector(15.5, -3.4, 8.2), true,
			FVector(16.5, -1.8, 9.0), FVector(0.3, 0.7, -0.4), true, 10.5f, 27.f, 2.f },     // cases out on the shot (user 2026-09-28)   // kick x3 (user 2026-09-26),
		{ TEXT("Rifle"), TEXT("ruger/ruger.obj"), FVector(-4.0, 2.7, 5.0), FRotator(-1.9, -6.5, 0.0),
			TEXT("icart"), 12, 0.5f, 0.f, 0.2f, 27000.f, 0.32f, TEXT("rifle"), TEXT("rifleout"), TEXT("rreload"),
			FVector(77.0, -1.3, 10.0), FVector::ZeroVector, FVector(33.0, -1.3, -3.0), false,
			FVector(28.0, -0.3, 11.0), FVector(-0.1, 0.8, 0.6), true, 9.f, 5.f, 2.f,
			1, 0.f, 1, false, 0, nullptr, 0.f, 0.f, false, 0.f, true, true },
		// ijsgun: 1 shell, 6 pellets +-4 deg, 1.5 each, range 200 u (placeholder: the VC Winchester 1912)
		{ TEXT("Shotgun"), TEXT("chromegun/chromegun.obj"), FVector(-4.0, 2.7, 5.0), FRotator(-10.1, -6.4, 0.0),
			TEXT("ishells"), 1, 0.5f, 0.f, 1.0f, 6000.f, 0.9f, TEXT("single"), TEXT("shotout"), TEXT("sreload"),
			FVector(71.8, 4.1, 16.0), FVector::ZeroVector, FVector(20.0, 1.0, 8.0), false,
			FVector(18.0, -2.0, 10.0), FVector(-0.2, 0.8, 0.5), false, 10.f, 18.f, 1.5f,
			6, 4.f, 1, false, 0, nullptr, 0.f, -6.f, false, 0.f, true, true },
		// ijdbsgun: 2 shells, both barrels at once (10 pellets +-2 deg), 1.0 each, range 150 u
		// (placeholder: the Winchester again; no side-by-side model yet)
		{ TEXT("Double Barrel"), TEXT("chromegun/chromegun.obj"), FVector(-4.0, 2.7, 5.0), FRotator(-10.1, -6.4, 0.0),
			TEXT("ishells"), 2, 0.5f, 0.f, 1.0f, 4500.f, 0.45f, TEXT("double"), TEXT("shotout"), TEXT("sreload"),
			FVector(71.8, 4.1, 16.0), FVector::ZeroVector, FVector(20.0, 1.0, 8.0), false,
			FVector(18.0, -2.0, 10.0), FVector(-0.2, 0.8, 0.5), false, 12.f, 22.f, 1.0f,
			5, 2.f, 2, false, 0, nullptr, 0.f, -6.f, false, 0.f, true, true },
		// ijsawgun: 2 shells, both barrels (30 pellets +-18 deg), 2.5 each, range 30 u (placeholder: VC shorty)
		{ TEXT("Sawed-Off"), TEXT("buddyshot/buddyshot.obj"), FVector(-6.6, 2.7, 0.5), FRotator(-9.7, -8.4, 0.0),
			TEXT("ishells"), 2, 0.5f, 0.f, 2.0f, 900.f, 0.45f, TEXT("sawed"), TEXT("shotout"), TEXT("sreload"),
			FVector(49.6, 3.6, 14.5), FVector::ZeroVector, FVector(15.0, 1.0, 9.0), true,
			FVector(14.0, -2.0, 10.0), FVector(-0.2, 0.8, 0.5), false, 12.f, 24.f, 2.5f,
			15, 18.f, 2, false, 0, nullptr, 0.f, -6.f, false, 0.f, true, false },
		// igatgun: a 100-round belt of iclip, 10 rounds a second while held, reloads itself; the game's model
		{ TEXT("Gatling"), TEXT(""), FVector(6.0, 0.0, 4.0), FRotator::ZeroRotator,
			TEXT("iclip"), 100, 0.1f, 0.f, 1.0f, 9000.f, 0.f, TEXT("gatlshot"), TEXT("gatLTAIL"), TEXT("weapgrab"),
			FVector::ZeroVector, FVector::ZeroVector, FVector::ZeroVector, false,
			FVector::ZeroVector, FVector(0.0, 1.0, 0.3), true, 2.f, 2.f, 2.0f,
			1, 0.f, 1, true, 0, TEXT("ggatgun.3do"), 110.f, 0.f, true },
		// ijdynam: pull the trigger to light (a 4.5 s fuse), let go to throw; blast 12 within 50 u
		{ TEXT("Dynamite"), TEXT(""), FVector(2.0, 0.0, 2.0), FRotator::ZeroRotator,
			TEXT("idynam"), 1, 0.6f, 0.f, 0.f, 1500.f, 0.f, TEXT("throw"), TEXT(""), TEXT(""),
			FVector::ZeroVector, FVector::ZeroVector, FVector::ZeroVector, false,
			FVector::ZeroVector, FVector::ZeroVector, false, 0.f, 0.f, 0.f,
			1, 0.f, 1, false, 1, TEXT("gdynam.3do"), 22.f },     // a hand-sized bundle, ~19 cm of sticks (user 2026-09-28: 44 was too big)
		// ijknife: squeeze and let go to throw (4 damage, lands as a gknife pickup); a fast swing jabs
		{ TEXT("Knife"), TEXT(""), FVector(4.0, 0.0, 1.0), FRotator::ZeroRotator,
			TEXT("iknife"), 1, 0.4f, 0.f, 0.f, 900.f, 0.f, TEXT("throw"), TEXT(""), TEXT(""),
			FVector::ZeroVector, FVector::ZeroVector, FVector::ZeroVector, false,
			FVector::ZeroVector, FVector::ZeroVector, false, 0.f, 0.f, 4.0f,
			1, 0.f, 1, false, 2, TEXT("gknife.3do"), 30.f },
	};
	constexpr int32 NumWeapons = UE_ARRAY_COUNT(GWeapons);
	// the revolver's cylinder and its six rounds, split out of the model (islands inside the cylinder's
	// box) so it can swing out on its crane: model space (right-hand gun), cm
	struct FCylinderDef { const TCHAR* Obj; FVector Centre; FVector Pivot; FVector OutDir; float Chamber; };
	const FCylinderDef GCylinder = { TEXT("python/python_cyl.obj"), FVector(15.05, -3.4, 8.05), FVector(15.05, -4.6, 5.65),
		FVector(0.0, -1.0, -0.12), 60.f };      // swings to the gun's left (-Y), a little below level; six chambers
	// the sprite-built guns (tools/olue/item3d: modelled from the item sprites, user 2026-09-28) replace the VE
	// placeholders when their OBJ is there; -OLVEGuns keeps the VE ones.  Their hand frame: grip at the origin,
	// bore along +X, +Z up; <name>.json beside each holds the gun's points (muzzle, sights, hammer, load, eject)
	FString SpriteHeldDir() { return FPaths::Combine(FOutlawsAssets::PlaceholderDir(), TEXT("items3d/held")); }
	const TCHAR* SpriteGunName(int32 Wi)
	{
		static const TCHAR* Names[] = { TEXT("GPISTOL"), TEXT("GRIFLE"), TEXT("GSGUN"), TEXT("GDBSGUN"), TEXT("GSAWGUN") };
		return Wi >= 0 && Wi < UE_ARRAY_COUNT(Names) ? Names[Wi] : nullptr;
	}
	// where each of the five comes from (user 2026-10-02: the meshes ship, the sprite's pixels never do):
	//   Dev      the full placeholder folder (items3d/held on this PC, or the headset's Placeholders), as before
	//   Shipped  the pak's Content/HeldGuns: only our recipes and points (no meshes, no images); the gun's meshes and
	//            texture built at runtime from the player's own HD pickup sprite (OutlawsGunBuilder, REQ-RUNTIMEGUNS)
	//   Card     no HD sprite (a non-remaster install) or no build: the same points, a flat card of the low-res pickup
	//   None     nothing to hold: the VE stand-in on this PC only, else an empty hand
	// -OLShippedGuns forces Shipped and -OLCardGuns Card (tests); -OLVEGuns keeps the VE stand-ins
	enum class EHeldGunSource : uint8 { Unknown, None, Dev, Shipped, Card };
	EHeldGunSource GHeldGunSources[8] = {};
	FString ShippedHeldDir() { return FPaths::Combine(FPaths::ProjectContentDir(), TEXT("HeldGuns")); }
	// the old shipped OBJs and templates (make_held_ship.py), on this PC only: the parity reference, never staged
	FString ParityHeldDir() { return FPaths::Combine(FOutlawsAssets::PlaceholderDir(), TEXT("items3d/held_ship")); }
	bool HeldGunsOnDevPc() { return FOutlawsAssets::HasDevItemDir(); }
	// -OLShippedGuns holds the parity OBJs instead of the runtime build, -OLRuntimeGuns forces the runtime build on this PC
	bool RuntimeGunsOn() { return !OLFlags::Param(TEXT("OLShippedGuns")); }
	const TCHAR* HeldGunSourceName(EHeldGunSource S)
	{
		return S == EHeldGunSource::Dev ? TEXT("dev") : S == EHeldGunSource::Shipped ? (RuntimeGunsOn() ? TEXT("runtime") : TEXT("shipped"))
			: S == EHeldGunSource::Card ? TEXT("card") : TEXT("none");
	}
	EHeldGunSource HeldGunSource(int32 Wi)
	{
		if (Wi < 0 || Wi >= 8) return EHeldGunSource::None;
		EHeldGunSource& Src = GHeldGunSources[Wi];
		if (Src == EHeldGunSource::Unknown)
		{
			const TCHAR* N = SpriteGunName(Wi);
			// the points and the texture recipe make a card; the build recipe (or the parity OBJ) a gun
			const auto Has = [N](const FString& Dir, const TCHAR* Suffix) { return FPaths::FileExists(FPaths::Combine(Dir, FString(N) + Suffix)); };
			const bool bPoints = N && Has(ShippedHeldDir(), TEXT(".json")) && Has(ShippedHeldDir(), TEXT("_recipe.json"));
			const bool bParity = bPoints && Has(ParityHeldDir(), TEXT(".obj"));
			const bool bShipped = RuntimeGunsOn() ? bPoints && (Has(ShippedHeldDir(), TEXT("_build.json")) || bParity) : bParity;
			const EHeldGunSource Packed = bShipped ? EHeldGunSource::Shipped : bPoints ? EHeldGunSource::Card : EHeldGunSource::None;
			if (!N || OLFlags::Param(TEXT("OLVEGuns"))) Src = EHeldGunSource::None;
			else if (OLFlags::Param(TEXT("OLCardGuns"))) Src = bPoints ? EHeldGunSource::Card : EHeldGunSource::None;
			else if (OLFlags::Param(TEXT("OLShippedGuns")) || OLFlags::Param(TEXT("OLRuntimeGuns"))) Src = Packed;
			else if (FPaths::FileExists(FPaths::Combine(SpriteHeldDir(), FString(N) + TEXT(".obj")))) Src = EHeldGunSource::Dev;
			else Src = Packed;
			if (N) UE_LOG(LogOutlaws, Log, TEXT("OLRT held gun %s: %s"), N, HeldGunSourceName(Src));
		}
		return Src;
	}
	bool UseSpriteGun(int32 Wi)
	{
		const EHeldGunSource Src = HeldGunSource(Wi);
		return Src == EHeldGunSource::Dev || Src == EHeldGunSource::Shipped;
	}
	FString HeldGunDir(int32 Wi) { return HeldGunSource(Wi) == EHeldGunSource::Dev ? SpriteHeldDir() : ShippedHeldDir(); }

	TSharedPtr<FJsonObject> LoadHeldGunRecipe(const TCHAR* Name)
	{
		FString Text;
		TSharedPtr<FJsonObject> J;
		if (!FFileHelper::LoadFileToString(Text, *FPaths::Combine(ShippedHeldDir(), FString(Name) + TEXT("_recipe.json")))
			|| !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), J) || !J.IsValid())
		{
			return nullptr;
		}
		return J;
	}

	/** A PNG from disk or the pak, as BGRA8 (sRGB). */
	bool LoadHeldGunImage(const FString& Path, FImage& Out)
	{
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path) || !FImageUtils::DecompressImage(Bytes.GetData(), Bytes.Num(), Out)) return false;
		Out.ChangeFormat(ERawImageFormat::BGRA8, EGammaSpace::sRGB);
		return true;
	}

	bool HeldRecipeInts(const TSharedPtr<FJsonObject>& J, const TCHAR* Field, const TCHAR* Key, int32& A, int32& B)
	{
		const TSharedPtr<FJsonObject>* Obj = nullptr;
		const TSharedPtr<FJsonObject>& From = Field ? (J->TryGetObjectField(Field, Obj) ? *Obj : J) : J;
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if ((Field && !Obj) || !From->TryGetArrayField(Key, Arr) || Arr->Num() != 2) return false;
		A = static_cast<int32>((*Arr)[0]->AsNumber());
		B = static_cast<int32>((*Arr)[1]->AsNumber());
		return true;
	}

	/** Every texel at or under alpha 127 in rows 0..FillRows-1 takes the RGB of its nearest texel over 127 there
	 *  (Euclidean; write_held's scipy fill): per column the nearest such row, then per row the lower envelope of
	 *  those distances' parabolas (Felzenszwalb and Huttenlocher), ties to the upper and the left. */
	void FillFromNearestOpaque(TArray<FColor>& Px, int32 W, int32 FillRows)
	{
		const int32 NumTexels = W * FillRows;
		TArray<int32> NearRow;
		NearRow.Init(-1, NumTexels);
		for (int32 x = 0; x < W; ++x)
		{
			int32 Last = -1;
			for (int32 y = 0; y < FillRows; ++y)
			{
				if (Px[y * W + x].A > 127) Last = y;
				NearRow[y * W + x] = Last;
			}
			Last = -1;
			for (int32 y = FillRows - 1; y >= 0; --y)
			{
				if (Px[y * W + x].A > 127) Last = y;
				const int32 Up = NearRow[y * W + x];
				const int64 DUp = Up >= 0 ? y - Up : MAX_int32, DDown = Last >= 0 ? Last - y : MAX_int32;
				NearRow[y * W + x] = DUp <= DDown ? Up : Last;
			}
		}
		TArray<int32> Env;
		TArray<double> EnvFrom, EnvF;
		Env.SetNumUninitialized(W);
		EnvFrom.SetNumUninitialized(W);
		EnvF.SetNumUninitialized(W);
		for (int32 y = 0; y < FillRows; ++y)
		{
			int32 K = -1;
			for (int32 q = 0; q < W; ++q)
			{
				const int32 R = NearRow[y * W + q];
				if (R < 0) continue;
				const double Fq = double(y - R) * double(y - R);
				double Cross = 0.0;
				while (K >= 0)
				{
					const int32 P = Env[K];
					Cross = ((Fq + double(q) * q) - (EnvF[K] + double(P) * P)) / (2.0 * (q - P));
					if (Cross <= EnvFrom[K]) --K;
					else break;
				}
				++K;
				Env[K] = q;
				EnvF[K] = Fq;
				EnvFrom[K] = K == 0 ? -1e30 : Cross;
			}
			if (K < 0) continue;
			int32 J = 0;
			for (int32 x = 0; x < W; ++x)
			{
				while (J + 1 <= K && EnvFrom[J + 1] < x) ++J;
				FColor& C = Px[y * W + x];
				if (C.A > 127) continue;
				const int32 Q = Env[J];
				const FColor& From = Px[NearRow[y * W + Q] * W + Q];
				C.R = From.R;
				C.G = From.G;
				C.B = From.B;
			}
		}
	}

	TMap<FString, TWeakObjectPtr<UTexture2D>> GShippedGunTextures;      // by the texture path the shipped .mtl names
	TMap<FString, TSharedPtr<OutlawsGunBuilder::FBuiltGun>> GRuntimeGuns; // the guns built from the player's sprite, by name

	/** A held gun's OBJ.  A Shipped gun's path (Content/HeldGuns/<NAME>.obj, <NAME>_<group>.obj: no such files ship)
	 *  is its runtime-built group, else the parity OBJ of that name on this PC, its texture keyed as the runtime's. */
	bool LoadHeldObj(const FString& Path, double Scale, bool bRightHanded, OutlawsObj::FObjMesh& Out)
	{
		if (FPaths::IsSamePath(FPaths::GetPath(Path), ShippedHeldDir()))
		{
			FString Name = FPaths::GetBaseFilename(Path), Group;
			Name.Split(TEXT("_"), &Name, &Group);
			if (const TSharedPtr<OutlawsGunBuilder::FBuiltGun>* G = GRuntimeGuns.Find(Name))
			{
				const OutlawsObj::FObjMesh* M = (*G)->Groups.Find(Group);
				if (!M) return false;
				Out = *M;
				return true;
			}
			const FString Ref = FPaths::Combine(ParityHeldDir(), FPaths::GetCleanFilename(Path));
			if (!FPaths::FileExists(Ref) || !OutlawsObj::Load(Ref, Scale, bRightHanded, Out)) return false;
			for (TPair<FString, FOutlawsMeshSection>& Kv : Out.Sections)
			{
				if (!Kv.Value.Texture.IsEmpty()) Kv.Value.Texture = FPaths::Combine(ShippedHeldDir(), FPaths::GetCleanFilename(Kv.Value.Texture));
			}
			return true;
		}
		return OutlawsObj::Load(Path, Scale, bRightHanded, Out);
	}

	/** -> the runtime gun against the shipped OBJs and texture, one OLRT line */
	void LogRuntimeGunParity(const OutlawsGunBuilder::FBuiltGun& G, const TArray<FColor>& Rt, const TArray<FColor>& Shipped, int32 SpriteRows)
	{
		int32 TrisRt = 0, TrisSh = 0;
		FBox BRt(ForceInit), BSh(ForceInit);
		double SocketDiff = 0.0;
		for (const TPair<FString, OutlawsObj::FObjMesh>& Kv : G.Groups)
		{
			OutlawsObj::FObjMesh Sh;
			const FString P = FPaths::Combine(ParityHeldDir(), G.Name + (Kv.Key.IsEmpty() ? FString() : TEXT("_") + Kv.Key) + TEXT(".obj"));
			const bool bSh = OutlawsObj::Load(P, 1.0, true, Sh);
			for (const TPair<FString, FOutlawsMeshSection>& S : Kv.Value.Sections) TrisRt += S.Value.Triangles.Num() / 3;
			for (const TPair<FString, FOutlawsMeshSection>& S : Sh.Sections) TrisSh += S.Value.Triangles.Num() / 3;
			if (Kv.Key.IsEmpty()) { BRt = Kv.Value.Bounds; BSh = Sh.Bounds; }
			else if (bSh) SocketDiff = FMath::Max(SocketDiff, (Kv.Value.Bounds.GetCenter() - Sh.Bounds.GetCenter()).Size());
			else SocketDiff = 1e9;
		}
		const double BoundsDiff = BSh.IsValid ? FMath::Max((BRt.Min - BSh.Min).GetAbsMax(), (BRt.Max - BSh.Max).GetAbsMax()) : -1.0;
		double Sum = 0.0;
		int32 N = 0, MaxBelow = 0;
		for (int32 i = 0; i < FMath::Min(Rt.Num(), Shipped.Num()); ++i)
		{
			const FColor& A = Rt[i];
			const FColor& B = Shipped[i];
			if (B.A == 0) continue;
			const int32 D = FMath::Max3(FMath::Abs(A.R - B.R), FMath::Abs(A.G - B.G), FMath::Abs(A.B - B.B));
			Sum += (FMath::Abs(A.R - B.R) + FMath::Abs(A.G - B.G) + FMath::Abs(A.B - B.B)) / 3.0;
			++N;
			if (i / FMath::Max(1, G.TexW) >= SpriteRows) MaxBelow = FMath::Max(MaxBelow, D);
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLRT gun %s: runtime tris %d vs shipped %d, bounds diff %.3f cm, grip/socket diff %.3f cm, colour diff %.3f mean RGB (blocks+swatches max %d), build ms %.1f, cache %s"),
			*G.Name, TrisRt, TrisSh, BoundsDiff, SocketDiff, N ? Sum / N : 0.0, MaxBelow, G.BuildMs, G.bCacheHit ? TEXT("hit") : TEXT("miss"));
	}

	/** The shipped gun's texture (made once while anything uses it), with its meshes built beside it from the player's
	 *  HD pickup sprite by its recipes.  bOutNoSprite: it is to be held as a card (no usable HD sprite, a non-remaster
	 *  install, or neither a runtime build nor the parity OBJs). */
	UTexture2D* BuildShippedGunTexture(AOutlawsLevelActor* L, int32 Wi, bool& bOutNoSprite)
	{
		bOutNoSprite = false;
		const TCHAR* Name = SpriteGunName(Wi);
		const TSharedPtr<FJsonObject> J = Name ? LoadHeldGunRecipe(Name) : nullptr;
		if (!J.IsValid())
		{
			UE_LOG(LogOutlaws, Warning, TEXT("OLRT held gun %s: no recipe in %s"), Name ? Name : TEXT("?"), *ShippedHeldDir());
			return nullptr;
		}
		const FString Key = FPaths::Combine(ShippedHeldDir(), J->GetStringField(TEXT("texture")));
		if (const TWeakObjectPtr<UTexture2D>* Hit = GShippedGunTextures.Find(Key))
		{
			if (Hit->IsValid()) return Hit->Get();
		}
		const FOutlawsAssets* Labs = L ? L->GetLabAssets() : nullptr;
		if (!Labs) return nullptr;
		int32 W = 0, H = 0, SpriteW = 0, SpriteH = 0, AtX = 0, AtY = 0, Fill0 = 0, FillRows = 0;
		const TSharedPtr<FJsonObject>* Sprite = nullptr;
		if (!HeldRecipeInts(J, nullptr, TEXT("size"), W, H) || !HeldRecipeInts(J, TEXT("sprite"), TEXT("size"), SpriteW, SpriteH)
			|| !HeldRecipeInts(J, TEXT("sprite"), TEXT("at"), AtX, AtY) || !HeldRecipeInts(J, TEXT("fill"), TEXT("rows"), Fill0, FillRows)
			|| !J->TryGetObjectField(TEXT("sprite"), Sprite) || W <= 0 || H <= 0 || AtX < 0 || AtY < 0 || AtX + SpriteW > W || AtY + SpriteH > H
			|| Fill0 != 0 || FillRows > H)
		{
			UE_LOG(LogOutlaws, Warning, TEXT("OLRT held gun %s: a bad recipe"), Name);
			return nullptr;
		}
		// the player's own sprite, as the pickups are drawn: its NWX cells' SPX frames at 4x
		const FString Nwx = (*Sprite)->GetStringField(TEXT("nwx"));
		const TSharedPtr<OutlawsTextures::FSpriteAtlas> Atlas = L->GetPickupAtlas(Nwx);
		TArray<FColor> Hd;
		int32 HdW = 0, HdH = 0;
		if (!Atlas || !OutlawsTextures::DecodeSpriteAtlasHdPixels(*Labs, *Atlas, Hd, HdW, HdH) || HdW != SpriteW || HdH != SpriteH)
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLRT held gun %s: no %dx%d HD sprite for %s (got %dx%d): held as a card"), Name, SpriteW, SpriteH, *Nwx, HdW, HdH);
			bOutNoSprite = true;
			return nullptr;
		}
		// the parity reference (this PC only, make_held_ship.py's folder): the old template with the sprite pasted and
		// filled; it is what -OLShippedGuns holds, and the runtime build is measured against it when it is there
		TArray<FColor> Px;
		FImage Tpl;
		const FString TplPath = FPaths::Combine(ParityHeldDir(), J->GetStringField(TEXT("template")));
		const bool bRef = FPaths::FileExists(TplPath) && LoadHeldGunImage(TplPath, Tpl) && Tpl.SizeX == W && Tpl.SizeY == H
			&& FPaths::FileExists(FPaths::Combine(ParityHeldDir(), FString(Name) + TEXT(".obj")));
		if (bRef)
		{
			const TArrayView64<FColor> TplPx = Tpl.AsBGRA8();
			Px = TArray<FColor>(TplPx.GetData(), int32(TplPx.Num()));
			for (int32 y = 0; y < SpriteH; ++y)
			{
				FMemory::Memcpy(Px.GetData() + (AtY + y) * W + AtX, Hd.GetData() + y * SpriteW, SpriteW * sizeof(FColor));
			}
			FillFromNearestOpaque(Px, W, FillRows);
		}
		// the runtime build: the gun's meshes and texture from the player's sprite by its recipe
		bool bBuilt = false;
		if (RuntimeGunsOn())
		{
			const FString RecipePath = FPaths::Combine(ShippedHeldDir(), FString(Name) + TEXT("_build.json"));
			TSharedPtr<OutlawsGunBuilder::FBuiltGun> G = MakeShared<OutlawsGunBuilder::FBuiltGun>();
			FString Err;
			if (!FPaths::FileExists(RecipePath))
			{
				UE_LOG(LogOutlaws, Warning, TEXT("OLRT gun %s: no runtime recipe at %s"), Name, *RecipePath);
			}
			else if (!OutlawsGunBuilder::BuildOrLoad(RecipePath, Hd, HdW, HdH, Key, *G, Err) || G->TexW != W || G->TexH != H)
			{
				UE_LOG(LogOutlaws, Warning, TEXT("OLRT gun %s: the runtime build failed (%s, %dx%d)"), Name, *Err, G->TexW, G->TexH);
			}
			else
			{
				TArray<FColor> Rt = G->Tex;
				FillFromNearestOpaque(Rt, W, G->ImageRows);
				if (bRef) LogRuntimeGunParity(*G, Rt, Px, SpriteH);
				else UE_LOG(LogOutlaws, Display, TEXT("OLRT gun %s: runtime build ms %.1f, cache %s"), Name, G->BuildMs, G->bCacheHit ? TEXT("hit") : TEXT("miss"));
				GRuntimeGuns.Add(Name, G);
				Px = MoveTemp(Rt);
				bBuilt = true;
			}
		}
		if (!bBuilt && !bRef)
		{
			// nothing to make the gun from (no meshes ship): held as the card of the low-res pickup, never empty
			UE_LOG(LogOutlaws, Error, TEXT("OLRT gun %s: no runtime build and no parity OBJs: held as a card"), Name);
			bOutNoSprite = true;
			return nullptr;
		}
		if (!bBuilt) UE_LOG(LogOutlaws, Log, TEXT("OLRT gun %s: the parity OBJs from %s"), Name, *ParityHeldDir());
		UTexture2D* T = OutlawsTextures::MakeTextureBGRA(W, H, Px, false, TF_Nearest, TA_Wrap);      // as the loaded PNG was
		if (!T) return nullptr;
		GShippedGunTextures.Add(Key, T);
		// this PC: against the dev texture the sprite was traced in (texels drawn, more than 2 apart in a channel)
		FImage Dev;
		const FString DevPath = FPaths::Combine(SpriteHeldDir(), J->GetStringField(TEXT("texture")));
		if (FPaths::FileExists(DevPath) && LoadHeldGunImage(DevPath, Dev))
		{
			const TArrayView64<FColor> DevPx = Dev.AsBGRA8();
			int32 Drawn = 0, Differ = 0, DifferAll = 0;
			if (Dev.SizeX == W && Dev.SizeY == H)
			{
				for (int32 i = 0; i < Px.Num(); ++i)
				{
					const FColor& A = Px[i];
					const FColor& B = DevPx[i];
					const bool bOff = FMath::Abs(A.R - B.R) > 2 || FMath::Abs(A.G - B.G) > 2 || FMath::Abs(A.B - B.B) > 2 || FMath::Abs(A.A - B.A) > 2;
					DifferAll += bOff ? 1 : 0;
					if (B.A == 0) continue;
					++Drawn;
					Differ += bOff ? 1 : 0;
				}
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLRT held gun %s: built %dx%d from %s, against the dev texture %dx%d: %d of %d drawn texels differ by more than 2 (%d of %d in all)"),
				Name, W, H, *Nwx, Dev.SizeX, Dev.SizeY, Differ, Drawn, DifferAll, Px.Num());
		}
		else
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLRT held gun %s: built %dx%d from the player's %s"), Name, W, H, *Nwx);
		}
		return T;
	}

	UTexture2D* HeldGunTexture(const FString& Path)
	{
		if (const TWeakObjectPtr<UTexture2D>* Hit = GShippedGunTextures.Find(Path))
		{
			if (Hit->IsValid()) return Hit->Get();
		}
		return OutlawsObj::LoadTextureFile(Path);
	}

	/** The card a gun is held as without its HD sprite: one quad in the gun's plane where the sprite lies in its hand
	 *  frame (the recipe's card), its material key "card:<NAME>". */
	bool MakeHeldGunCard(int32 Wi, OutlawsObj::FObjMesh& Out)
	{
		const TCHAR* Name = SpriteGunName(Wi);
		const TSharedPtr<FJsonObject> J = Name ? LoadHeldGunRecipe(Name) : nullptr;
		const TSharedPtr<FJsonObject>* Card = nullptr;
		if (!J.IsValid() || !J->TryGetObjectField(TEXT("card"), Card)) return false;
		const double X0 = (*Card)->GetNumberField(TEXT("x_left")), X1 = (*Card)->GetNumberField(TEXT("x_right"));
		const double Z0 = (*Card)->GetNumberField(TEXT("z_top")), Z1 = (*Card)->GetNumberField(TEXT("z_bottom"));
		FOutlawsMeshSection S;
		S.Texture = FString(TEXT("card:")) + Name;
		S.Positions = { FVector(X0, 0.0, Z0), FVector(X1, 0.0, Z0), FVector(X1, 0.0, Z1), FVector(X0, 0.0, Z1) };
		S.UVs = { FVector2D(0.0, 0.0), FVector2D(1.0, 0.0), FVector2D(1.0, 1.0), FVector2D(0.0, 1.0) };
		S.Normals.Init(FVector(0.0, -1.0, 0.0), 4);
		S.Colors.Init(FLinearColor::White, 4);
		S.Triangles = { 0, 1, 2, 0, 2, 3 };       // (one face: the card's material is two-sided)
		Out = OutlawsObj::FObjMesh();
		for (const FVector& P : S.Positions) Out.Bounds += P;
		Out.Sections.Add(TEXT("card"), S);
		return true;
	}

	/** The card's look: the low-res pickup atlas's first cell on the held sprite material. */
	UTexture2D* HeldGunCardLook(AOutlawsLevelActor* L, const FString& Key, UMaterialInstanceDynamic* Mid)
	{
		const FString Name = Key.RightChop(5);           // (after "card:")
		const TSharedPtr<OutlawsTextures::FSpriteAtlas> Atlas = L ? L->GetPickupAtlas(Name + TEXT(".NWX")) : nullptr;
		if (!Atlas || !Atlas->Texture) return nullptr;
		Mid->SetVectorParameterValue(TEXT("UVScale"), FLinearColor(1.f / FMath::Max(1, Atlas->Cols), 1.f / FMath::Max(1, Atlas->Rows), 0.f, 0.f));
		Mid->SetVectorParameterValue(TEXT("UVOffset"), FLinearColor(0.f, 0.f, 0.f, 0.f));
		Mid->SetScalarParameterValue(TEXT("Opacity"), 1.f);
		return Atlas->Texture;
	}
	struct FSpriteSights { bool bOk = false; FVector Rear = FVector::ZeroVector; FVector Front = FVector::ZeroVector;
		bool bHammer = false; FVector HammerPivot = FVector::ZeroVector; bool bTrigger = false; FVector TriggerPivot = FVector::ZeroVector;
		bool bLever = false; FVector LeverPivot = FVector::ZeroVector;
		bool bSelector = false; FVector Selector = FVector::ZeroVector; };      // Selector: x, side distance, z
	FSpriteSights GSpriteSights[8];
	// the sprite revolver's cylinder (its own OBJ, <GPISTOL>_cyl.obj): centre and crane from GPISTOL.json
	FCylinderDef GSpriteCylinder = { TEXT("GPISTOL_cyl.obj"), FVector::ZeroVector, FVector::ZeroVector, FVector(0.0, -1.0, -0.12), 60.f };
	double GSpriteCylLength = 7.25, GSpriteChamberR = 1.06, GSpriteHoleR = 0.46;      // cm (GPISTOL.json)
	const FCylinderDef& Cylinder() { return UseSpriteGun(0) ? GSpriteCylinder : GCylinder; }
	bool HasCylinder(int32 Wi) { return Wi == 0; }
	constexpr float LaserMax = 400.f;
	constexpr float WheelScale = 3.3f;           // the wheel's drawn size (its parts are laid out in cm at 1): two thirds of the first 5 (user 2026-09-27)
	constexpr float WheelItemsZ = -22.5f;        // the first item row, under the horseshoe and the bottom weapon's round count
	constexpr float WheelItemSize = 4.2f, WheelItemStep = 5.0f, WheelRowStep = 4.7f, WheelColStep = 27.f;
	constexpr float WheelRowWidth = 35.5f;       // the horseshoe's width (its 14.5 cm radius and a 6.5 cm icon each side)

	TMap<FString, TSharedPtr<OutlawsObj::FObjMesh>> GObjCache;
	// the dynamite's wick in the held model's space: the bundle's top cap (where it burns down to) and its tip
	double GFusePivotZ = 0.0, GFuseTipZ = 0.0;
}

FString AOutlawsPlayer::HeldGunTestName(int32 Wi) const
{
	const TCHAR* Name = SpriteGunName(Wi);
	return FString(Name ? Name : TEXT("weapon")).ToLower() + TEXT("_") + HeldGunSourceName(HeldGunSource(Wi));
}

UTexture2D* AOutlawsPlayer::KpfTexture(const FString& Path)
{
	if (const TObjectPtr<UTexture2D>* Found = KpfTextures.Find(Path))
	{
		return *Found;
	}
	UTexture2D* T = nullptr;
	TArray<uint8> Bytes;
	if (LoadMenuSource() && MenuSource->ReadFile(Path, Bytes) && Bytes.Num() > 0)
	{
		T = FImageUtils::ImportBufferAsTexture2D(Bytes);
		if (T)
		{
			T->Filter = TF_Trilinear;
			T->SRGB = true;
			T->UpdateResource();
		}
	}
	KpfTextures.Add(Path, T);
	return T;
}

UStaticMeshComponent* AOutlawsPlayer::WheelPlane(UMaterialInstanceDynamic*& OutMid)
{
	// the engine plane (100 cm, facing +Z) turned to face the eyes (the wheel's X), picture upright
	UStaticMeshComponent* P = NewObject<UStaticMeshComponent>(this);
	P->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane")));
	P->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	P->SetCastShadow(false);
	P->SetupAttachment(WheelRoot);
	P->RegisterComponent();
	OutMid = nullptr;
	// the overlay material: no depth test, so nothing in the world hides the wheel; on the headset its own depth-tested
	// copy pulled to 18 cm (BUG-WHEELOCCLUDE: the 50 cm of the other overlays let a train door hide it)
	UMaterialInterface* Base = WheelOverlayMaterial(false);
	if (!Base) Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_WorldGlass.M_OLR_WorldGlass"));
	if (Base)
	{
		OutMid = UMaterialInstanceDynamic::Create(Base, this);
		P->SetMaterial(0, OutMid);
		KeepAlive.Add(OutMid);
	}
	return P;
}

void AOutlawsPlayer::SetPlaneTexture(UStaticMeshComponent* Plane, UTexture2D* Tex)
{
	if (UMaterialInstanceDynamic* M = Plane ? Cast<UMaterialInstanceDynamic>(Plane->GetMaterial(0)) : nullptr)
	{
		if (Tex) M->SetTextureParameterValue(TEXT("Tex"), Tex);
	}
}

UMaterialInterface* AOutlawsPlayer::LaserMaterial()
{
	UMaterialInterface* Mat = HeldSolidMaterial(FColor(255, 40, 30));
	if (UMaterialInstanceDynamic* Mid = Cast<UMaterialInstanceDynamic>(Mat))
	{
		HeldMids.Remove(Mid);                                 // (not dimmed to the room's light)
		KeepAlive.Add(Mid);
		Mid->SetScalarParameterValue(TEXT("Brightness"), 1.f);
		// at its own depth on the PC (the overlays there cut themselves where it is nearer); on the headset the overlays'
		// "_Q" copies are depth tested at arm's length, so the beam beyond it was hidden behind the menu (user
		// 2026-10-01): there pulled near as the held gear is, over the menu and the wheel
		const UWorld* W = GetWorld();
		const bool bMobile = W && W->FeatureLevel <= ERHIFeatureLevel::ES3_1;
		Mid->SetScalarParameterValue(TEXT("NearPull"), bMobile ? 1.f : 0.f);
		return Mid;
	}
	return Mat ? Mat : SolidMaterial(FColor(255, 40, 30));
}

int32 AOutlawsPlayer::MakeTextPlate(USceneComponent* Parent, float HeightCm, bool bLeft)
{
	// a 4:1 canvas the text is drawn on, shown on a plane four times as wide as it is tall
	UCanvasRenderTarget2D* RT = NewObject<UCanvasRenderTarget2D>(this);
	RT->RenderTargetFormat = RTF_RGBA8;
	RT->TargetGamma = 1.f;
	RT->ClearColor = FLinearColor::Black;
	RT->InitAutoFormat(256, 64);
	RT->OnCanvasRenderTargetUpdate.AddDynamic(this, &AOutlawsPlayer::DrawTextPlate);
	USceneComponent* Holder = NewObject<USceneComponent>(this);
	Holder->SetupAttachment(Parent);
	Holder->RegisterComponent();
	UStaticMeshComponent* P = NewObject<UStaticMeshComponent>(this);
	P->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane")));
	P->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	P->SetCastShadow(false);
	P->SetupAttachment(Holder);
	P->RegisterComponent();
	FOutlawsViewCone::WidenHandBounds(P);
	P->SetRelativeRotation(FRotationMatrix::MakeFromXY(FVector(0.0, -1.0, 0.0), FVector(0.0, 0.0, -1.0)).Rotator());
	P->SetRelativeScale3D(FVector(HeightCm * 4.f / 100.0, HeightCm / 100.0, 1.0));
	P->SetTranslucentSortPriority(6);
	UMaterialInterface* Base = LoadOverlayMaterial(TEXT("M_OLR_TextPlate"));
	if (!Base) Base = LoadOverlayMaterial(TEXT("M_OLR_UIOverlay"));
	if (Base)
	{
		UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, this);
		Mid->SetTextureParameterValue(TEXT("Tex"), RT);
		P->SetMaterial(0, Mid);
		KeepAlive.Add(Mid);
	}
	PlateRTs.Add(RT);
	PlateHolders.Add(Holder);
	PlatePlanes.Add(P);
	PlateShown.Add(FString());
	PlateLeftAligned.Add(bLeft);
	PlateLow.Add(false);
	return PlateRTs.Num() - 1;
}

void AOutlawsPlayer::CoopNamePlate(USceneComponent* Parent, const FVector& RelLoc)
{
	if (!Parent) return;
	if (CoopNamePlateIdx < 0) CoopNamePlateIdx = MakeTextPlate(Parent, 15.f);
	if (!PlateHolders.IsValidIndex(CoopNamePlateIdx)) return;
	USceneComponent* H = PlateHolders[CoopNamePlateIdx];
	if (H->GetAttachParent() != Parent) H->AttachToComponent(Parent, FAttachmentTransformRules::KeepRelativeTransform);
	H->SetRelativeLocationAndRotation(RelLoc, FRotator::ZeroRotator);
	H->SetRelativeScale3D(FVector::OneVector);
}

void AOutlawsPlayer::CoopNamePlateText(const FString& Text, bool bShow)
{
	if (!PlatePlanes.IsValidIndex(CoopNamePlateIdx)) return;
	if (bShow) SetTextPlate(CoopNamePlateIdx, Text);
	PlatePlanes[CoopNamePlateIdx]->SetVisibility(bShow && !Text.IsEmpty());
}

void AOutlawsPlayer::SetTextPlate(int32 Index, const FString& Text, bool bLow)
{
	if (!PlateRTs.IsValidIndex(Index) || (PlateShown[Index] == Text && PlateLow[Index] == bLow)) return;
	PlateShown[Index] = Text;
	PlateLow[Index] = bLow;
	PlateDrawing = Index;
	PlateRTs[Index]->UpdateResource();
	PlateDrawing = -1;
}

void AOutlawsPlayer::DrawTextPlate(UCanvas* C, int32 Width, int32 Height)
{
	if (!C || !PlateShown.IsValidIndex(PlateDrawing)) return;
	if (!PlateFont) PlateFont = KpfFont(TEXT("fonts/MontaguSlab_24pt-Bold.ttf"), 44);
	const FString& Str = PlateShown[PlateDrawing];
	if (!PlateFont || Str.IsEmpty()) return;
	float XL = 0.f, YL = 0.f;
	C->TextSize(PlateFont, Str, XL, YL);
	// outlined (user 2026-10-01): the text and its outline in green (M_OLR_TextPlate's coverage), the text spread round
	// itself 3.5 px (half the first 7: user 2026-10-01); then the letters in red over it (its cream; with blue, the warning
	// red); the material colours them.  Scaled so the outline fits the canvas both ways (it was cropped at the ends)
	constexpr float Ring = 3.5f, Margin = 3.f;
	const float FitW = (Width - 2.f * (Ring + Margin)) / FMath::Max(1.f, XL);
	const float FitH = (Height - 2.f * (Ring + Margin)) / FMath::Max(1.f, YL);
	const float Scale = FMath::Min3(1.f, FitW, FitH);
	const bool bLeft = PlateLeftAligned.IsValidIndex(PlateDrawing) && PlateLeftAligned[PlateDrawing];
	const bool bLow = PlateLow.IsValidIndex(PlateDrawing) && PlateLow[PlateDrawing];
	auto Draw = [&](const FVector2D& Off, const FLinearColor& Colour)
	{
		const FVector2D At = bLeft ? FVector2D(Ring + Margin, Height * 0.5f) : FVector2D(Width * 0.5f, Height * 0.5f);
		FCanvasTextItem T(At + Off, FText::FromString(Str), PlateFont, Colour);
		T.Scale = FVector2D(Scale, Scale);
		T.bCentreX = !bLeft;
		T.bCentreY = true;
		T.BlendMode = SE_BLEND_Translucent;
		C->DrawItem(T);
	};
	for (const float Rad : { Ring * 0.5f, Ring })
	{
		for (int32 k = 0; k < 16; ++k)
		{
			const float A = UE_TWO_PI * k / 16.f;
			Draw(FVector2D(FMath::Cos(A), FMath::Sin(A)) * Rad, FLinearColor(0.f, 1.f, 0.f, 1.f));
		}
	}
	Draw(FVector2D::ZeroVector, FLinearColor(1.f, 1.f, bLow ? 1.f : 0.f, 1.f));
}

namespace
{
	// the plane's picture: U to the viewer's right (-Y), V down (-Z), face toward the eyes (+X)
	const FQuat GPlaneFacing = FRotationMatrix::MakeFromXY(FVector(0.0, -1.0, 0.0), FVector(0.0, 0.0, -1.0)).ToQuat();
	// the remaster's wheel icons per wheel weapon (-1 = bare hands), Outlaws slot order round the ring
	// each placeholder long gun's sights (model space, cm; measured from the OBJ): the rear point the eye looks through
	// (the rifle's peep ring; on the shotguns the top of the receiver the eye sights along) and the front point (post
	// or bead).  With the stock up, the line through both runs through the aiming eye (user 2026-09-28)
	bool SightPoints(int32 Wi, FVector& Rear, FVector& Front)
	{
		if (Wi > 0 && Wi < 8 && GSpriteSights[Wi].bOk)
		{
			Rear = GSpriteSights[Wi].Rear;
			Front = GSpriteSights[Wi].Front;
			return true;
		}
		switch (Wi)
		{
		case 1: Rear = FVector(15.8, -2.39, 12.85); Front = FVector(61.9, 2.86, 14.6); return true;       // ruger: peep ring, post
		case 2:
		case 3: Rear = FVector(19.79, -1.87, 12.56); Front = FVector(68.93, 3.73, 20.67); return true;    // chromegun: receiver, bead
		case 4: Rear = FVector(26.51, 0.36, 14.68); Front = FVector(49.13, 3.75, 18.13); return true;     // buddyshot: rib, bead
		default: return false;
		}
	}

	// the inventory item that carries each weapon (GWeapons order); revolvers can be two
	const TCHAR* WeaponItem(int32 Wi)
	{
		static const TCHAR* Items[] = { TEXT("ipistol"), TEXT("irifle"), TEXT("isgun"), TEXT("idbsgun"), TEXT("isawgun"), TEXT("igatgun"), TEXT("idynam"), TEXT("iknife") };
		return Wi >= 0 && Wi < UE_ARRAY_COUNT(Items) ? Items[Wi] : nullptr;
	}

	const TCHAR* WeaponIcon(int32 Wi)
	{
		switch (Wi)
		{
		case -1: return TEXT("w1fist");
		case 0: return TEXT("w2pistolj");
		case 1: return TEXT("w3rifle");
		case 2: return TEXT("w4shotg");
		case 3: return TEXT("w5shotgdouble");
		case 4: return TEXT("w6shotgsaw");
		case 5: return TEXT("w9minigun");
		case 6: return TEXT("w7dynam");
		case 7: return TEXT("w8knife");
		default: return TEXT("wempty");
		}
	}
}

// ---- BUG-CHARLOADOUT: a character's own guns ---------------------------------------------------------------------------------

namespace
{
	/** The five guns as built (James's own: ijpistol, ijrifle...), put back for single player and James. */
	struct FGunBase { const TCHAR* Sound; float Damage, Spread, Variance, RangeCm; int32 Pellets; };
	FGunBase GGunBase[5];
	bool bGunBase = false;
	FString GGunSound[5];                     // (the character's sounds: FireSound points here)
	FString GWeaponsWorn = TEXT("-");         // whose guns these are now (empty: James's)
	FString GLastGunSound;                    // (-OLCharLoadoutTest: the sound of this player's last shot)
	constexpr double ItmUnitCm = FOutlawsGeometry::UnitsToCm;        // RANGE_1 in Outlaws units

	/** A weapon's SLOT, from its inventory ITM (ipistol 2 ... iknife 8); the number START_WEAPON names. */
	int32 SlotOfWeapon(const FOutlawsAssets* A, int32 Wi)
	{
		static int32 Slots[NumWeapons] = {};
		static bool bRead = false;
		if (!bRead && A)
		{
			bRead = true;
			for (int32 i = 0; i < NumWeapons; ++i)
			{
				FString Text, Func;
				TMap<FString, FString> D;
				if (A->GetText(FString(WeaponItem(i)) + TEXT(".itm"), Text)) OutlawsFormats::ParseItm(Text, Func, D);
				Slots[i] = D.Contains(TEXT("SLOT")) ? FCString::Atoi(*D[TEXT("SLOT")]) : 0;
			}
		}
		return Wi >= 0 && Wi < NumWeapons ? Slots[Wi] : 0;
	}
}

bool AOutlawsPlayer::StockGun(int32 Weapon, FStockGun& Out)
{
	if (Weapon < 0 || Weapon >= 5) return false;
	const FWeaponDef& W = GWeapons[Weapon];
	Out.Label = W.Label;
	Out.AmmoItem = W.AmmoItem;
	Out.Rounds = FMath::Max(1, W.RoundsPerShot);
	Out.Item = WeaponItem(Weapon);
	// what a character's own ITM changes: kept aside as built once a character's guns were first worn (SetWeaponCharacter)
	if (bGunBase)
	{
		const FGunBase& B = GGunBase[Weapon];
		Out.Sound = B.Sound;
		Out.Damage = B.Damage;
		Out.Spread = B.Spread;
		Out.Variance = B.Variance;
		Out.RangeCm = B.RangeCm;
		Out.Pellets = FMath::Max(1, B.Pellets);
	}
	else
	{
		Out.Sound = W.FireSound;
		Out.Damage = W.Damage;
		Out.Spread = W.Spread;
		Out.Variance = W.Variance;
		Out.RangeCm = W.RangeCm;
		Out.Pellets = FMath::Max(1, W.Pellets);
	}
	return true;
}

void AOutlawsPlayer::SetWeaponCharacter(const FOutlawsAssets* A, const FString& Short)
{
	if (!bGunBase)
	{
		for (int32 Wi = 0; Wi < 5; ++Wi)
		{
			const FWeaponDef& W = GWeapons[Wi];
			GGunBase[Wi] = { W.FireSound, W.Damage, W.Spread, W.Variance, W.RangeCm, W.Pellets };
		}
		bGunBase = true;
	}
	const FOutlawsMpCharacter* Ch = Short.IsEmpty() ? nullptr : FOutlawsMpCharacter::Find(Short);
	// James's guns are the ones built (his ij* ITMs, hand tuned since); single player is always his
	const FString Key = Ch && !FString(Ch->Short).Equals(TEXT("Jam")) ? FString(Ch->Short) : FString();
	if (Key == GWeaponsWorn) return;
	GWeaponsWorn = Key;
	for (int32 Wi = 0; Wi < 5; ++Wi)
	{
		FWeaponDef& W = GWeapons[Wi];
		const FGunBase& B = GGunBase[Wi];
		W.FireSound = B.Sound;
		W.Damage = B.Damage;
		W.Spread = B.Spread;
		W.Variance = B.Variance;
		W.RangeCm = B.RangeCm;
		W.Pellets = B.Pellets;
	}
	if (Key.IsEmpty() || !A)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT guns: James Anderson's (as built)"));
		return;
	}
	const FOutlawsCharKit& Kit = FOutlawsCharKit::Of(A, Key);
	FString Line;
	for (int32 Wi = 0; Wi < 5; ++Wi)
	{
		const FString Item = WeaponItem(Wi);
		const FString Own = Kit.WeaponVariant(A, Item);
		FString Text, Func;
		TMap<FString, FString> D;
		if (Own == Item || !A->GetText(Own + TEXT(".itm"), Text)) continue;     // (none of its own: James's)
		OutlawsFormats::ParseItm(Text, Func, D);
		FWeaponDef& W = GWeapons[Wi];
		auto F = [&D](const TCHAR* K, float& V, double Scale = 1.0) { if (const FString* S = D.Find(K)) V = float(FCString::Atod(**S) * Scale); };
		if (const FString* S = D.Find(TEXT("FIRE_SOUND_1")); S && !S->IsEmpty() && !S->Equals(TEXT("null"), ESearchCase::IgnoreCase))
		{
			GGunSound[Wi] = S->ToLower();
			W.FireSound = *GGunSound[Wi];
		}
		F(TEXT("DAMAGE_1"), W.Damage);
		F(TEXT("SPREAD_1"), W.Spread);
		F(TEXT("VARIANCE_1"), W.Variance);
		F(TEXT("RANGE_1"), W.RangeCm, ItmUnitCm);
		if (const FString* S = D.Find(TEXT("SHOT_MULTIPLES"))) W.Pellets = FMath::Max(1, FCString::Atoi(**S));
		Line += FString::Printf(TEXT("%s%s %s (%s, %.2f x %d, spread %.1f, variance %.1f, %.0f m)"), Line.IsEmpty() ? TEXT("") : TEXT("; "), W.Label, *Own,
			W.FireSound, W.Damage, W.Pellets, W.Spread, W.Variance, W.RangeCm / 100.f);
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT guns: %s's own: %s"), Ch->Name, Line.IsEmpty() ? TEXT("(none: James's)") : *Line);
}

bool AOutlawsPlayer::EquipStartWeapon()
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L || L->StartWeaponSlot < 0) return false;
	int32 Wi = -1;
	for (int32 i = 0; i < NumWeapons && Wi < 0; ++i) if (SlotOfWeapon(L->GetLabAssets(), i) == L->StartWeaponSlot) Wi = i;
	const int32 Hand = LeftDominant() ? 0 : 1;
	if (Wi < 0 || WeaponsOwned(Wi) <= 0)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT first weapon: slot %d is %s: the hands bare"), L->StartWeaponSlot, Wi < 0 ? TEXT("no gun of ours") : TEXT("not carried"));
		return false;
	}
	Equip(Hand, Wi);
	// REQ-SANCHEZDUAL: two of a gun that pairs (Spit'n Jack's revolvers) are drawn as the pair, the second in the other hand,
	// as a second one picked up is (UpdateDualPickup)
	const bool bPair = WeaponsOwned(Wi) >= 2 && GWeapons[Wi].Throw <= 0 && !GWeapons[Wi].bAuto && Equipped[1 - Hand] < 0;
	if (bPair)
	{
		Equip(1 - Hand, Wi);
		PrevOwned.Add(Wi, WeaponsOwned(Wi));        // (both drawn: no second pickup to draw)
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT first weapon: the %s in the %s hand%s (START_WEAPON %d)"), GWeapons[Wi].Label, Hand == 0 ? TEXT("left") : TEXT("right"),
		bPair ? (Equipped[1 - Hand] == Wi ? TEXT(", its pair in the other") : TEXT(", its pair NOT drawn")) : TEXT(""), L->StartWeaponSlot);
	return Equipped[Hand] == Wi;
}

int32 AOutlawsPlayer::WeaponsOwned(int32 Wi) const
{
	if (Wi < 0) return 1;                                  // fists
	const AOutlawsLevelActor* L = Level.Get();
	const TCHAR* Item = WeaponItem(Wi);
	return L && Item ? L->GetCount(Item) : 0;
}

FString AOutlawsPlayer::BugSnapAmmoLine() const
{
	// (REQ-BUGSNAP) each weapon carried: rounds in the hands' guns or saved in its magazine, and the reserve item's count
	FString S;
	const AOutlawsLevelActor* L = Level.Get();
	for (int32 Wi = 0; Wi < NumWeapons; ++Wi)
	{
		if (WeaponsOwned(Wi) <= 0) continue;
		const FWeaponDef& W = GWeapons[Wi];
		const int32* Saved = SavedMag.Find(Wi) ? SavedMag.Find(Wi) : SavedMag.Find(16 + Wi);
		const int32 In = Equipped[0] == Wi ? Mag[0] : Equipped[1] == Wi ? Mag[1] : Saved ? *Saved : W.Capacity;
		S += FString::Printf(TEXT("%s x%d (in %d, %s %d); "), W.Label, WeaponsOwned(Wi), In, W.AmmoItem ? W.AmmoItem : TEXT("-"),
			L && W.AmmoItem ? L->GetCount(W.AmmoItem) : 0);
	}
	return S.IsEmpty() ? FString(TEXT("no weapons")) : S + (bUnlimitedAmmo ? TEXT("unlimited ammo cheat on") : TEXT(""));
}

bool AOutlawsPlayer::WeaponHasAmmo(int32 Wi) const
{
	if (Wi < 0 || bUnlimitedAmmo) return true;
	const FWeaponDef& W = GWeapons[Wi];
	const AOutlawsLevelActor* L = Level.Get();
	if (L && W.AmmoItem && L->GetCount(W.AmmoItem) > 0) return true;
	if (W.Throw != 0) return false;                        // a throwable is its own ammunition
	for (int32 H = 0; H < 2; ++H)
	{
		const int32* Saved = SavedMag.Find(H * 16 + Wi);
		if ((Equipped[H] == Wi ? Mag[H] : Saved ? *Saved : W.Capacity) > 0) return true;   // (never drawn: it comes full)
	}
	return false;
}

UStaticMeshComponent* AOutlawsPlayer::WheelSlotComponent(int32 Index, UTextRenderComponent*& OutLabel)
{
	// slots are pooled: the item ring grows with what the player carries
	while (WheelSlots.Num() <= Index)
	{
		UMaterialInterface* TextMat = WheelOverlayMaterial(true);
		UMaterialInstanceDynamic* Mid = nullptr;
		UStaticMeshComponent* S = WheelPlane(Mid);
		S->SetTranslucentSortPriority(3);
		WheelSlots.Add(S);
		WheelSlotMids.Add(Mid);
		UTextRenderComponent* T = NewObject<UTextRenderComponent>(this);
		T->SetHorizontalAlignment(EHTA_Center);
		T->SetVerticalAlignment(EVRTA_TextCenter);
		T->SetWorldSize(1.4f);
		if (TextMat) T->SetTextMaterial(TextMat);
		T->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		T->SetCastShadow(false);
		T->SetupAttachment(WheelRoot);
		T->RegisterComponent();
		WheelLabels.Add(T);
	}
	OutLabel = WheelLabels[Index];
	return WheelSlots[Index];
}

void AOutlawsPlayer::OpenWheel(int32 Hand)
{
	if (AOutlawsLevelActor* LW = Level.Get(); LW && (LW->IsFrontEnd() || LW->IsMoviePlaying())) return;   // no wheel on the title screens or in a movie
	UWorld* World = GetWorld();
	// (REQ-CLIMBWHEEL, user 2026-10-14: "I can't use the weapon wheel while I'm holding or grabbing onto a ledge") the wheel
	// opens while a hand holds a ledge: the holding hand keeps its grip, and a pick goes into the hand whose button opened
	// it (the holding hand's own pick stays put away until it lets go, as a gun in a hand that takes a ledge does); only the
	// quarter second of a climb onto the top still refuses it
	if (!World || MantleLeft > 0.f)
	{
		return;
	}
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (!WheelRoot)
	{
		SlotMat = SolidMaterial(FColor(48, 36, 26));
		SlotLitMat = SolidMaterial(FColor(200, 150, 60));
		LaserMat = LaserMaterial();
		WheelRoot = NewObject<USceneComponent>(this, TEXT("WeaponWheel"));
		WheelRoot->SetupAttachment(VROrigin);
		WheelRoot->RegisterComponent();
	}
	if (!Lasers[Hand])
	{
		Lasers[Hand] = NewObject<UStaticMeshComponent>(this);
		Lasers[Hand]->SetStaticMesh(Cube);
		Lasers[Hand]->SetMaterial(0, LaserMat);
		Lasers[Hand]->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Lasers[Hand]->SetCastShadow(false);
		Lasers[Hand]->SetupAttachment(AimOf(Hand));
		Lasers[Hand]->RegisterComponent();
	}
	// a second button while the wheel is up joins it: one wheel, a laser per hand
	bWheelHeld[Hand] = true;
	WheelPick[Hand] = -1;
	WheelDropPick[Hand] = -1;
	WheelHeldSince[Hand] = FPlatformTime::Seconds();
	Lasers[Hand]->SetVisibility(true);
	Pulse(Hand, 0.3f);
	if (bWheelOpen)
	{
		return;
	}
	bWheelOpen = true;
	// the wheel hangs where the player is looking, facing the eyes; it moves with the play space but
	// not with the head
	const FVector Eye = Camera->GetComponentLocation();
	// 2.4 m out (six to ten feet) at five times the drawn size (its parts are laid out in cm at scale 1),
	// drawn over everything
	WheelDir = Camera->GetForwardVector();
	WheelYawVel = WheelPitchVel = 0.f;
	bWheelYawFollow = bWheelPitchFollow = false;
	const FVector Centre = Eye + WheelDir * 240.0;
	WheelRoot->SetWorldLocationAndRotation(Centre, (Eye - Centre).Rotation());   // X toward the eyes
	WheelRoot->SetWorldScale3D(FVector(WheelScale));
	// the remaster's horseshoe (36 cm across): the weapons round the ring in Outlaws' slot order, the
	// items in a row over the banner at the top, the name of what is pointed at in the banner
	WheelWeapon.Reset();
	WheelItem.Reset();
	WheelIconOn.Reset();
	WheelIconOff.Reset();
	WheelSlotAngle.Reset();
	TArray<FString> Labels;
	static const int32 SlotOrder[] = { -1, 0, 1, 2, 3, 4, 6, 7, 5 };     // fist .. knife, then the gatling (slot 9)
	for (const int32 Wi : SlotOrder)
	{
		if (Wi >= NumWeapons) continue;
		WheelWeapon.Add(Wi);
		WheelItem.Add(FString());
		Labels.Add(Wi < 0 ? TEXT("Hands") : GWeapons[Wi].Label);
		WheelIconOn.Add(FString::Printf(TEXT("gfx/wheel/weapons/on/%s.png"), WeaponIcon(Wi)));
		WheelIconOff.Add(FString::Printf(TEXT("gfx/wheel/weapons/off/%soff.png"), WeaponIcon(Wi)));
	}
	const int32 NumWheelWeapons = WheelWeapon.Num();
	if (!WheelBase)
	{
		UMaterialInstanceDynamic* Mid = nullptr;
		WheelBase = WheelPlane(Mid);
		SetPlaneTexture(WheelBase, KpfTexture(TEXT("gfx/wheel/wheelbase.png")));
		WheelBase->SetRelativeLocationAndRotation(FVector(-0.5, 0.0, 0.0), GPlaneFacing);
		WheelBase->SetRelativeScale3D(FVector(0.36, 0.36, 1.0));
		WheelBase->SetTranslucentSortPriority(1);
		WheelDial = WheelPlane(Mid);
		SetPlaneTexture(WheelDial, KpfTexture(TEXT("gfx/wheel/dialweapon.png")));
		WheelDial->SetTranslucentSortPriority(2);
		WheelItemDial = WheelPlane(Mid);
		SetPlaneTexture(WheelItemDial, KpfTexture(TEXT("gfx/wheel/dialitem.png")));
		WheelItemDial->SetTranslucentSortPriority(2);
		WheelBanner = NewObject<UTextRenderComponent>(this);
		WheelBanner->SetHorizontalAlignment(EHTA_Center);
		WheelBanner->SetVerticalAlignment(EVRTA_TextCenter);
		WheelBanner->SetWorldSize(1.7f);
		WheelBanner->SetTextRenderColor(FColor(250, 225, 150));
		if (UMaterialInterface* TextMat = WheelOverlayMaterial(true)) WheelBanner->SetTextMaterial(TextMat);
		WheelBanner->SetTranslucentSortPriority(5);
		WheelBanner->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		WheelBanner->SetupAttachment(WheelRoot);
		WheelBanner->RegisterComponent();
		WheelBanner->SetRelativeLocation(FVector(0.6, 0.0, 15.7));
	}
	WheelBase->SetVisibility(true);
	WheelDial->SetVisibility(false);
	WheelItemDial->SetVisibility(false);
	WheelBanner->SetText(FText::GetEmpty());
	WheelBanner->SetVisibility(true);
	AllWheelItems.Reset();
	AllWheelItemLabels.Reset();
	if (AOutlawsLevelActor* L = Level.Get())
	{
		L->GetWheelItems(AllWheelItems, AllWheelItemLabels);
	}
	WheelWeaponCount = NumWheelWeapons;
	WheelSlotLabels = Labels;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT wheel items: %s"), *FString::Join(AllWheelItemLabels, TEXT(" | ")));
	WheelItemStart = FMath::Clamp(WheelItemStart, 0, FMath::Max(0, AllWheelItems.Num() - WheelItemsShown));
	RebuildWheelSlots();
	WheelRoot->SetVisibility(true, false);
	ShowWheelMap();                      // (REQ-WHEELMAP: the map in the horseshoe's middle)
	KeepWheelOverWorld();
	// the world slows to half speed and dims behind the wheel (R8, decided 2026-10-03: not in a co-op session, where the
	// host's clock is both players' bad guys and doors; single player as before)
	if (!UOutlawsCoop::IsActive(this)) UGameplayStatics::SetGlobalTimeDilation(this, 0.5f);
}

void AOutlawsPlayer::RebuildWheelSlots()
{
	// the weapons stay; the items are this page of what is carried
	const int32 NumWheelWeapons = WheelWeaponCount;
	WheelWeapon.SetNum(NumWheelWeapons);
	WheelItem.SetNum(NumWheelWeapons);
	WheelIconOn.SetNum(NumWheelWeapons);
	WheelIconOff.SetNum(NumWheelWeapons);
	WheelSlotAngle.Reset();
	TArray<FString> Labels = WheelSlotLabels;
	// every item, grouped by type (user 2026-09-27), in one row under the horseshoe, centred, growing as more is carried;
	// a second row only past the horseshoe's width (BUG-WHEELINV, planner 2026-10-03)
	const int32 First = WheelItemStart;
	const int32 NumItems = FMath::Clamp(AllWheelItems.Num() - First, 0, WheelItemsShown);
	// the medical bag and the lantern (its oil) have places of their own either side of the horseshoe, always there
	// (the bag greyed with none carried), so they never move with what else is carried (user 2026-09-29)
	struct FItemPlace { int32 Row = 0, Col = 0, InRow = 0, RowCount = 1; bool bFixed = false; FVector Fixed = FVector::ZeroVector; };
	TArray<FItemPlace> Places;
	const AOutlawsLevelActor* LW = Level.Get();
	const int32 Bags = LW ? LW->GetCount(TEXT("idocbag")) : 0;
	const int32 Oil = LW ? LW->GetCount(TEXT("ioil")) : 0;
	const int32 PlateLeft = LW ? LW->GetArmour() : 0;
	auto AddFixed = [&](const TCHAR* Item, const FString& Label, const FVector& At)
	{
		WheelWeapon.Add(-2);
		WheelItem.Add(Item);
		Labels.Add(Label);
		WheelIconOn.Add(FString::Printf(TEXT("gfx/wheel/items/on/%s.png"), Item));
		WheelIconOff.Add(FString::Printf(TEXT("gfx/wheel/items/off/%s.png"), Item));
		FItemPlace P;
		P.bFixed = true;
		P.Fixed = At;
		Places.Add(P);
	};
	{
		const int32 Lamp = AllWheelItems.IndexOfByKey(FString(TEXT("ilantern")));
		AddFixed(TEXT("idocbag"), Bags > 0 ? FString::Printf(TEXT("Medical Bag %d"), Bags) : FString(TEXT("Medical Bag (none)")), FVector(0.3, 24.0, -7.0));  // the viewer's left
		AddFixed(TEXT("ilantern"), Lamp != INDEX_NONE ? AllWheelItemLabels[Lamp] : FString(TEXT("Lantern")), FVector(0.3, -24.0, -7.0));                 // and right
		// under them the boiler plate (greyed with none) (user 2026-10-01); the map is no item now (REQ-WHEELMAP, user
		// 2026-10-15): it lies in the horseshoe's middle whenever the wheel is open (OutlawsWheelMap.cpp)
		AddFixed(TEXT("ibplate"), PlateLeft > 0 ? FString::Printf(TEXT("Boiler Plate %d%%"), PlateLeft) : FString(TEXT("Boiler Plate (none)")), FVector(0.3, 24.0, -17.0));
		// a dev tool: the grip editor (user 2026-10-01), over the lantern; (REQ-WHEELDEVONLY, user 15 Oct) only with the dev flag
		// (Dev checks: -OLDevChecks or the pushed flag file; never in Shipping), never on a player build's wheel
		static const bool bDevWheel = OutlawsDevChecks::Enabled();          // (once: the flag file is looked for at the first wheel)
		if (bDevWheel)
		{
			AddFixed(TEXT("idev_grip"), bGripEditor ? FString(TEXT("Grip Editor: on")) : FString(TEXT("Grip Editor: off")), FVector(0.3, -24.0, 3.0));
			AddFixed(TEXT("idev_gripreset"), FString(TEXT("Reset grips")), FVector(0.3, -24.0, 13.0));      // (BUG-GRIPSAVE: back to the baked grips)
		}
	}
	TArray<FString> Stems;
	TArray<TArray<int32>> Rows;
	for (int32 k = 0; k < NumItems; ++k)
	{
		if (AllWheelItems[First + k] == TEXT("idocbag") || AllWheelItems[First + k] == TEXT("ilantern") || AllWheelItems[First + k] == TEXT("ibplate")
			|| AllWheelItems[First + k] == TEXT("imap")) continue;   // (their own places)
		const FString Stem = ItemStem(AllWheelItems[First + k]);
		int32 r = Stems.IndexOfByKey(Stem);
		if (r == INDEX_NONE) { r = Stems.Add(Stem); Rows.AddDefaulted(); }
		Rows[r].Add(First + k);
	}
	const int32 NumCols = 1;
	TArray<int32> Flat;
	for (const TArray<int32>& Row : Rows) Flat.Append(Row);
	const int32 PerRow = FMath::Max(1, FMath::FloorToInt((WheelRowWidth - WheelItemSize) / WheelItemStep) + 1);
	for (int32 k = 0; k < Flat.Num(); ++k)
	{
		const FString& Item = AllWheelItems[Flat[k]];
		WheelWeapon.Add(-2);
		WheelItem.Add(Item);
		Labels.Add(AllWheelItemLabels[Flat[k]]);
		WheelIconOn.Add(FString::Printf(TEXT("gfx/wheel/items/on/%s.png"), *Item));
		WheelIconOff.Add(FString::Printf(TEXT("gfx/wheel/items/off/%s.png"), *Item));
		FItemPlace P;
		P.Row = k / PerRow;
		P.InRow = k % PerRow;
		P.RowCount = FMath::Min(PerRow, Flat.Num() - P.Row * PerRow);
		Places.Add(P);
	}
	if (OLFlags::Param(TEXT("OLShots"))) UE_LOG(LogOutlaws, Display, TEXT("OLWHEEL items (dev flag %s): %s"), OutlawsDevChecks::Enabled() ? TEXT("on") : TEXT("off"), *FString::Join(WheelItem, TEXT(", ")));
	for (int32 i = 0; i < FMath::Max(WheelSlots.Num(), Labels.Num()); ++i)
	{
		UTextRenderComponent* T = nullptr;
		UStaticMeshComponent* S = WheelSlotComponent(i, T);
		if (i >= Labels.Num())
		{
			S->SetVisibility(false);
			T->SetVisibility(false);
			if (WheelCountPlates.IsValidIndex(i) && WheelCountPlates[i] >= 0) PlatePlanes[WheelCountPlates[i]]->SetVisibility(false);
			if (WheelShades.IsValidIndex(i)) WheelShades[i]->SetVisibility(false);
			if (WheelPairPlates.IsValidIndex(i) && WheelPairPlates[i] >= 0) PlatePlanes[WheelPairPlates[i]]->SetVisibility(false);
			continue;
		}
		// weapons round the horseshoe from the upper-left arm (-40 degrees from the top) down the left,
		// across the bottom and up to the upper-right arm (+40): the gap at the top holds the banner;
		// items in a row over it
		const bool bItem = i >= NumWheelWeapons;
		FVector Local;
		float Size;
		if (bItem)
		{
			const FItemPlace& P = Places[i - NumWheelWeapons];
			const double ColX = (P.Col - (NumCols - 1) * 0.5) * WheelColStep;          // the columns centred as a whole
			Local = P.bFixed ? P.Fixed : FVector(0.3, -(ColX + (P.InRow - (P.RowCount - 1) * 0.5) * WheelItemStep), WheelItemsZ - P.Row * WheelRowStep);
			Size = P.bFixed ? 5.5f : WheelItemSize;
			WheelSlotAngle.Add(0.f);
		}
		else
		{
			const float Deg = NumWheelWeapons > 1 ? -40.f - 280.f * i / (NumWheelWeapons - 1) : 180.f;
			const float A = FMath::DegreesToRadians(Deg);
			Local = FVector(0.3, -14.5 * FMath::Sin(A), 14.5 * FMath::Cos(A));
			Size = 6.5f;
			WheelSlotAngle.Add(Deg);
		}
		S->SetRelativeLocationAndRotation(Local, GPlaneFacing);
		S->SetRelativeScale3D(FVector(Size / 100.0, Size / 100.0, 1.0));
		// the weapon's rounds (loaded and carried) under its icon, in the remaster's font (user 2026-09-27)
		while (WheelCountPlates.Num() <= i) WheelCountPlates.Add(-1);
		if (WheelCountPlates[i] < 0) WheelCountPlates[i] = MakeTextPlate(WheelRoot, 2.f);
		{
			const int32 Wi = WheelWeapon[i];
			FString Count;
			if (!bItem && Wi >= 0 && Wi < NumWeapons)
			{
				const FWeaponDef& WD = GWeapons[Wi];
				int32 Loaded = 0;
				for (int32 Hd = 0; Hd < 2; ++Hd)
				{
					const int32* Saved = SavedMag.Find(Hd * 16 + Wi);
					Loaded = FMath::Max(Loaded, Equipped[Hd] == Wi ? Mag[Hd] : Saved ? *Saved : WD.Capacity);   // never drawn: it comes full
				}
				const int32 Reserve = Level.IsValid() && WD.AmmoItem ? Level->GetCount(WD.AmmoItem) : 0;
				Count = WD.Throw != 0 ? FString::FromInt(Reserve) : FString::Printf(TEXT("%d | %d"), Loaded, Reserve);
			}
			if (!bItem && Wi >= 0 && WeaponsOwned(Wi) <= 0) Count.Reset();      // not carried: no count
			// the bag's count and the lantern's oil under their own places
			if (bItem && WheelItem[i] == TEXT("idocbag")) Count = FString::FromInt(Bags);
			if (bItem && WheelItem[i] == TEXT("ilantern")) Count = FString::Printf(TEXT("oil %d"), Oil);
			if (bItem && WheelItem[i] == TEXT("ibplate") && PlateLeft > 0) Count = FString::Printf(TEXT("%d%%"), PlateLeft);
			PlateHolders[WheelCountPlates[i]]->SetRelativeLocationAndRotation(Local + FVector(0.4, 0.0, -Size * 0.5 - 1.3), FRotator::ZeroRotator);
			SetTextPlate(WheelCountPlates[i], Count);
			PlatePlanes[WheelCountPlates[i]]->SetVisibility(!Count.IsEmpty());
		}
		// two of a pair-up weapon carried (revolvers, sawed-offs): "x2" at the icon's top-right corner (user 2026-09-28)
		{
			const int32 Wp = WheelWeapon[i];
			const bool bPair = !bItem && Wp >= 0 && Wp < NumWeapons && GWeapons[Wp].Throw <= 0 && WeaponsOwned(Wp) >= 2;
			while (WheelPairPlates.Num() <= i) WheelPairPlates.Add(-1);
			if (WheelPairPlates[i] < 0) WheelPairPlates[i] = MakeTextPlate(WheelRoot, 1.7f);
			PlateHolders[WheelPairPlates[i]]->SetRelativeLocationAndRotation(Local + FVector(0.5, -Size * 0.42, Size * 0.36), FRotator::ZeroRotator);
			// (user 2026-10-02: revolvers past the pair are kept as spares: "x3", "x4")
			SetTextPlate(WheelPairPlates[i], bPair ? FString::Printf(TEXT("x%d"), WeaponsOwned(Wp)) : FString());
			PlatePlanes[WheelPairPlates[i]]->SetVisibility(bPair);
		}
		// a weapon not carried, or with nothing left to fire, is blacked out and cannot be picked (user 2026-09-27)
		while (WheelUsable.Num() <= i) WheelUsable.Add(true);
		WheelUsable[i] = bItem ? !((WheelItem[i] == TEXT("idocbag") && Bags <= 0) || (WheelItem[i] == TEXT("ibplate") && PlateLeft <= 0)) : WeaponUsable(WheelWeapon[i]);
		while (WheelShades.Num() <= i)
		{
			UMaterialInstanceDynamic* ShadeMid = nullptr;
			UStaticMeshComponent* Sh = WheelPlane(ShadeMid);
			if (!WheelShadeTex)
			{
				TArray<FColor> Px;
				Px.Init(FColor(0, 0, 0, 205), 16);
				WheelShadeTex = OutlawsTextures::MakeTextureBGRA(4, 4, Px, false, TF_Bilinear, TA_Clamp);
				KeepAlive.Add(WheelShadeTex);
			}
			if (!WheelShadeDiscTex)
			{
				// (BUG-WHEELGREY, user 15 Oct: the darkening was a square box round the round slot) a disc as wide as the slot's
				// circle, its edge a pixel and a half soft, clear outside it
				constexpr int32 N = 128;
				constexpr float R = N * 0.48f;
				TArray<FColor> Px;
				Px.SetNumUninitialized(N * N);
				for (int32 y = 0; y < N; ++y)
					for (int32 x = 0; x < N; ++x)
					{
						const float D = FMath::Sqrt(FMath::Square(x + 0.5f - N * 0.5f) + FMath::Square(y + 0.5f - N * 0.5f));
						const float A = FMath::Clamp((R - D) / 1.5f + 0.5f, 0.f, 1.f);
						Px[y * N + x] = FColor(0, 0, 0, uint8(FMath::RoundToInt(205.f * A)));
					}
				WheelShadeDiscTex = OutlawsTextures::MakeTextureBGRA(N, N, Px, false, TF_Bilinear, TA_Clamp);
				KeepAlive.Add(WheelShadeDiscTex);
			}
			Sh->SetTranslucentSortPriority(4);            // over the icon (3)
			WheelShades.Add(Sh);
		}
		// a weapon's slot is round: a disc over it; an item's card is square: the square
		SetPlaneTexture(WheelShades[i], bItem ? WheelShadeTex : WheelShadeDiscTex);
		WheelShades[i]->SetRelativeLocationAndRotation(Local + FVector(0.15, 0.0, 0.0), GPlaneFacing);
		WheelShades[i]->SetRelativeScale3D(FVector(Size / 100.0, Size / 100.0, 1.0));
		WheelShades[i]->SetVisibility(!WheelUsable[i]);
		UTexture2D* Icon = bItem ? ItemCard(WheelItem[i], false) : KpfTexture(WheelIconOff[i]);
		if (!Icon) Icon = bItem ? ItemCard(WheelItem[i], true) : KpfTexture(WheelIconOn[i]);
		if (!Icon) Icon = KpfTexture(bItem ? TEXT("gfx/wheel/dialitem.png") : TEXT("gfx/wheel/weapons/off/wemptyoff.png"));
		SetPlaneTexture(S, Icon);
		S->SetVisibility(true);
		// the label only for an item without art (the banner names whatever is pointed at)
		T->SetText(FText::FromString(Labels[i]));
		T->SetRelativeLocationAndRotation(Local + FVector(0.5, 0.0, -3.5), FRotator::ZeroRotator);
		T->SetTextRenderColor(FColor(235, 220, 180));
		T->SetVisibility(bItem && !ItemCard(WheelItem[i], true));
	}
	UpdateWheelArrows();
}

void AOutlawsPlayer::UpdateWheelArrows()
{
	// the remaster's arrows at the ends of the item row, there only when the row can page that way:
	// gold, or orange with a laser on them
	auto Make = [this](const TCHAR* Off) -> UStaticMeshComponent*
	{
		UMaterialInstanceDynamic* Mid = nullptr;
		UStaticMeshComponent* A = WheelPlane(Mid);
		SetPlaneTexture(A, KpfTexture(Off));
		A->SetTranslucentSortPriority(3);
		return A;
	};
	if (!WheelPrev) WheelPrev = Make(TEXT("gfx/wheel/prevoff.png"));
	if (!WheelNext) WheelNext = Make(TEXT("gfx/wheel/nextoff.png"));
	const int32 Shown = FMath::Clamp(AllWheelItems.Num() - WheelItemStart, 0, WheelItemsShown);
	const double Edge = (Shown - 1) * 0.5 * 6.0 + 5.0;
	const bool bPrev = false, bNext = false;      // every item shows: no paging (user 2026-09-27)
	(void)Shown;
	const bool bHotPrev = WheelArrowHover[0] < 0 || WheelArrowHover[1] < 0;
	const bool bHotNext = WheelArrowHover[0] > 0 || WheelArrowHover[1] > 0;
	WheelPrev->SetRelativeLocationAndRotation(FVector(0.3, Edge, WheelItemsZ), GPlaneFacing);     // +Y: the viewer's left
	WheelNext->SetRelativeLocationAndRotation(FVector(0.3, -Edge, WheelItemsZ), GPlaneFacing);
	WheelPrev->SetRelativeScale3D(FVector(0.024, 0.046, 1.0));
	WheelNext->SetRelativeScale3D(FVector(0.024, 0.046, 1.0));
	SetPlaneTexture(WheelPrev, KpfTexture(bHotPrev ? TEXT("gfx/wheel/prev.png") : TEXT("gfx/wheel/prevoff.png")));
	SetPlaneTexture(WheelNext, KpfTexture(bHotNext ? TEXT("gfx/wheel/next.png") : TEXT("gfx/wheel/nextoff.png")));
	WheelPrev->SetVisibility(bWheelOpen && bPrev);
	WheelNext->SetVisibility(bWheelOpen && bNext);
	KeepWheelOverWorld();                // (BUG-WHEELOCCLUDE: the new item planes too)
}

UMaterialInterface* AOutlawsPlayer::WheelOverlayMaterial(bool bText) const
{
	// (BUG-WHEELOCCLUDE) the PC's overlay (no depth test); on the headset the wheel's own "_Q" copies, depth tested and
	// pulled to 18 cm (17.5 for the banner): behind the hands and the held gear (12 cm + 4 %), the count plates (10 cm +
	// 3 %), in front of any wall (ue_olue_wheel_overlay_quest.py). -OLWheelTopTest=oldmat: the 50 cm copies, as before
	static const bool bOldMat = OLFlags::ValueIs(TEXT("OLWheelTopTest"), TEXT("oldmat"));
	const UWorld* World = GetWorld();
	if (!bOldMat && World && World->FeatureLevel <= ERHIFeatureLevel::ES3_1)
	{
		if (UMaterialInterface* M = LoadObject<UMaterialInterface>(nullptr, bText ? TEXT("/Game/Outlaws/Runtime/M_OLR_WheelText_Q.M_OLR_WheelText_Q")
			: TEXT("/Game/Outlaws/Runtime/M_OLR_WheelOverlay_Q.M_OLR_WheelOverlay_Q"))) return M;
	}
	return LoadOverlayMaterial(bText ? TEXT("M_OLR_TextOverlay") : TEXT("M_OLR_UIOverlay"));
}

int32 AOutlawsPlayer::KeepWheelOverWorld()
{
	// (BUG-WHEELOCCLUDE, user 2026-10-12: on the train the wheel went behind a door and its items could not be seen) the
	// wheel hangs 2.4 m out; a wall or door nearer than that hid it, not by the depth test (the PC overlay has none, the
	// headset's "_Q" copy pulls it to 50 cm) but by occlusion culling: the planes' own small bounds sat behind the door,
	// so the engine dropped them whole (the headset's depth-tested copy can be occluded). Each part's bounds now reach
	// past the eyes, so nothing in the world can cover them (the held gear and the text plates do the same,
	// FOutlawsViewCone::WidenHandBounds). The number of parts widened is returned (the test reads it)
	if (!WheelRoot || !Camera) return 0;
	static const bool bTestOld = OLFlags::ValueIs(TEXT("OLWheelTopTest"), TEXT("nowiden"));   // (the test's control run)
	if (bTestOld) return 0;
	const FVector Eye = Camera->GetComponentLocation();
	TArray<USceneComponent*> Parts;
	WheelRoot->GetChildrenComponents(true, Parts);
	int32 Widened = 0;
	for (USceneComponent* C : Parts)
	{
		UPrimitiveComponent* P = Cast<UPrimitiveComponent>(C);
		if (!P) continue;
		const float Radius = P->Bounds.SphereRadius / FMath::Max(P->BoundsScale, 0.01f);
		if (Radius < 0.01f) continue;
		const float Need = (float(FVector::Dist(Eye, P->Bounds.Origin)) + 60.f) / Radius;
		if (P->BoundsScale < Need)
		{
			P->SetBoundsScale(FMath::Min(Need * 1.25f, 4000.f));
			++Widened;
		}
	}
	return Widened;
}

void AOutlawsPlayer::UpdateWheel()
{
	OL_PERF_SCOPE("Player: wheel");
	if (!bWheelOpen || !WheelRoot)
	{
		return;
	}
	// it follows the head as the menu does: lazily, once the head has turned well away (real time: the
	// world runs at half speed behind the wheel)
	{
		// on the pause menu's damped springs, across and up and down (user 2026-10-02: it stopped dead at the edge and set
		// off in jerks): once 20 degrees off either way it sets off, eases back to the middle of the view and settles
		const FVector Eye = Camera->GetComponentLocation();
		if (bVR)
		{
			const FRotator Head = Camera->GetComponentRotation();
			const FRotator Hung = WheelDir.Rotation();
			const float Step = FMath::Min(float(FApp::GetDeltaTime()), 0.05f);
			auto WheelFollow = [Step](float Angle, float& Vel, bool& bFollow, float Err)
			{
				if (FMath::Abs(Err) > 20.f) bFollow = true;
				if (bFollow && FMath::Abs(Err) < 1.5f && FMath::Abs(Vel) < 6.f) bFollow = false;
				constexpr float Spring = 4.5f;                       // (critically damped: no overshoot)
				Vel += ((bFollow ? Spring * Spring * Err : 0.f) - 2.f * Spring * Vel) * Step;
				return Angle + Vel * Step;
			};
			const float Yaw = WheelFollow(float(Hung.Yaw), WheelYawVel, bWheelYawFollow, float(FRotator::NormalizeAxis(Head.Yaw - Hung.Yaw)));
			const float Pitch = WheelFollow(float(Hung.Pitch), WheelPitchVel, bWheelPitchFollow, float(FMath::Clamp(Head.Pitch, -70.0, 70.0) - Hung.Pitch));
			WheelDir = FRotator(FMath::Clamp(Pitch, -80.f, 80.f), Yaw, 0.f).Vector();
		}
		const FVector Centre = Eye + WheelDir * 240.0;
		WheelRoot->SetWorldLocationAndRotation(Centre, (Eye - Centre).Rotation());
	}
	const FVector C = WheelRoot->GetComponentLocation();
	const FVector N = WheelRoot->GetForwardVector();
	bool bChanged = false;
	for (int32 H = 0; H < 2; ++H)
	{
		if (!bWheelHeld[H] || !Lasers[H])
		{
			continue;
		}
		if (!bVR)
		{
			bChanged = true;       // (desktop tests set the pick by hand)
			continue;
		}
		// the laser: from the hand along its pointing direction to the wheel's plane
		UMotionControllerComponent* Aim = AimOf(H);
		const FVector O = Aim->GetComponentLocation();
		const FVector D = Aim->GetForwardVector();
		float Length = LaserMax;
		int32 Pick = -1, AnyPick = -1;
		const double Den = FVector::DotProduct(D, N);
		if (Den < -1e-3)
		{
			const double T = FVector::DotProduct(C - O, N) / Den;
			if (T > 0.0 && T < LaserMax)
			{
				Length = float(T);
				const FVector P = O + D * T;
				// an arrow first: the row pages with a trigger pull while the laser is on it
				int32 Arrow = 0;
				if (WheelPrev && WheelPrev->IsVisible() && FVector::Dist(P, WheelPrev->GetComponentLocation()) < 3.0 * WheelScale) Arrow = -1;
				else if (WheelNext && WheelNext->IsVisible() && FVector::Dist(P, WheelNext->GetComponentLocation()) < 3.0 * WheelScale) Arrow = 1;
				if (Arrow != WheelArrowHover[H])
				{
					WheelArrowHover[H] = Arrow;
					UpdateWheelArrows();
					if (Arrow != 0) Pulse(H, 0.15f);
				}
				const float Trig = Axis(H == 0 ? TEXT("OLVR_LeftTrigger") : TEXT("OLVR_RightTrigger"));
				if (Trig < 0.3f)
				{
					bWheelTriggerArmed[H] = true;
				}
				else if (Trig > 0.7f && bWheelTriggerArmed[H] && Arrow != 0)
				{
					bWheelTriggerArmed[H] = false;
					WheelItemStart = FMath::Clamp(WheelItemStart + Arrow, 0, FMath::Max(0, AllWheelItems.Num() - WheelItemsShown));
					WheelPick[0] = WheelPick[1] = -1;
					RebuildWheelSlots();
					bChanged = true;
					Pulse(H, 0.35f);
				}
				double Best = 4.5 * WheelScale;          // round an icon's middle
				double BestAny = Best;
				for (int32 i = 0; i < WheelWeapon.Num(); ++i)
				{
					const double Dist = FVector::Dist(P, WheelSlots[i]->GetComponentLocation());
					if (Dist < BestAny)
					{
						BestAny = Dist;
						AnyPick = i;                     // (the grip's drop: a carried gun with no rounds left is greyed, user 2026-10-02)
					}
					if (WheelUsable.IsValidIndex(i) && !WheelUsable[i]) continue;      // blacked out
					if (Dist < Best)
					{
						Best = Dist;
						Pick = i;
					}
				}
			}
		}
		Lasers[H]->SetRelativeLocationAndRotation(FVector(Length * 0.5, 0.0, 0.0), FRotator::ZeroRotator);
		Lasers[H]->SetRelativeScale3D(FVector(Length / 100.0, 0.004, 0.004));
		WheelDropPick[H] = AnyPick;
		if (Pick != WheelPick[H])
		{
			WheelPick[H] = Pick;
			bChanged = true;
			if (Pick >= 0)
			{
				Pulse(H, 0.15f);
			}
		}
	}
	if (bChanged)
	{
		// the lit art for what is pointed at (and what the hands hold), the pick frame behind it, its name
		// in the banner
		int32 Shown = -1;
		for (int32 i = 0; i < WheelWeapon.Num(); ++i)
		{
			const bool bLit = (bWheelHeld[0] && WheelPick[0] == i) || (bWheelHeld[1] && WheelPick[1] == i);
			const bool bHeld = WheelWeapon[i] >= -1 && (Equipped[0] == WheelWeapon[i] || Equipped[1] == WheelWeapon[i]);
			if (bLit) Shown = i;
			if (WheelIconOn.IsValidIndex(i))
			{
				UTexture2D* Icon = KpfTexture((bLit || bHeld) ? WheelIconOn[i] : WheelIconOff[i]);
				if (Icon) SetPlaneTexture(WheelSlots[i], Icon);
			}
			WheelLabels[i]->SetTextRenderColor(bLit ? FColor(255, 255, 255) : FColor(235, 220, 180));
		}
		if (WheelBanner) WheelBanner->SetText(FText::FromString(Shown >= 0 ? WheelLabels[Shown]->Text.ToString() : FString()));
		const bool bItemPick = Shown >= 0 && WheelWeapon[Shown] == -2;
		if (WheelDial)
		{
			WheelDial->SetVisibility(Shown >= 0 && !bItemPick);
			if (Shown >= 0 && !bItemPick && WheelSlotAngle.IsValidIndex(Shown))
			{
				// the trapezoid behind the icon, its wide end outward
				const FQuat Round(FVector::XAxisVector, FMath::DegreesToRadians(WheelSlotAngle[Shown]));
				WheelDial->SetRelativeLocationAndRotation(WheelSlots[Shown]->GetRelativeLocation() - FVector(0.2, 0.0, 0.0), Round * GPlaneFacing);
				WheelDial->SetRelativeScale3D(FVector(0.072, 0.084, 1.0));
			}
		}
		if (WheelItemDial)
		{
			WheelItemDial->SetVisibility(bItemPick);
			if (bItemPick)
			{
				WheelItemDial->SetRelativeLocationAndRotation(WheelSlots[Shown]->GetRelativeLocation() - FVector(0.2, 0.0, 0.0), GPlaneFacing);
				WheelItemDial->SetRelativeScale3D(FVector(0.055, 0.071, 1.0));
			}
		}
	}
	// (REQ-WHEELMAP, BUG-MAPZOOMTRIGGER) a laser on the map, and on no icon or arrow, darkens it with a prompt; that hand's
	// trigger brings it closer (OutlawsWheelMap.cpp)
	UpdateWheelMapPointer();
	UpdateWheelMap(FMath::Min(float(FApp::GetDeltaTime()), 0.05f));
}

void AOutlawsPlayer::ReleaseWheel(int32 Hand)
{
	if (!bWheelOpen || !bWheelHeld[Hand])
	{
		return;
	}
	// this hand takes what its laser points at; the wheel stays while the other button is held
	const int32 Pick = WheelPick[Hand];
	// Classic layout: a tap (let go within a third of a second, nothing picked) reloads that hand's gun,
	// the hold-to-body reload being gone (user 2026-09-27)
	if ((!bFaceReload || OneHand() == Hand) && !WheelWeapon.IsValidIndex(Pick) && FPlatformTime::Seconds() - WheelHeldSince[Hand] < 0.35 && Equipped[Hand] >= 0)
	{
		bButtonReload[Hand] = true;
	}
	bWheelHeld[Hand] = false;
	WheelPick[Hand] = -1;
	if (Lasers[Hand]) Lasers[Hand]->SetVisibility(false);
	if (WheelWeapon.IsValidIndex(Pick))
	{
		if (WheelWeapon[Pick] >= -1)
		{
			const int32 Wi = WheelWeapon[Pick];
			if (Wi >= 0 && WheelUsable.IsValidIndex(Pick) && !WheelUsable[Pick])
			{
				Refuse(Hand);                          // blacked out: not carried, or nothing left to fire
			}
			else
			{
				// one revolver is one revolver: drawn into the other hand it changes hands, rounds and all; only a
				// second one carried (a second gpistol found) makes the pair (user 2026-09-27)
				const int32 O = 1 - Hand;
				if (Wi >= 0 && Equipped[O] == Wi && GWeapons[Wi].Throw <= 0 && WeaponsOwned(Wi) < 2)
				{
					Equip(O, -1);
					SavedMag.Add(Hand * 16 + Wi, SavedMag.FindRef(O * 16 + Wi));
				}
				Equip(Hand, Wi);
				++TutNotes.WheelTakes[Hand];           // (REQ-TUTORIAL)
			}
		}
		else if (AOutlawsLevelActor* L = Level.Get())
		{
			if (WheelItem[Pick] == TEXT("idev_grip"))
			{
				bGripEditor = !bGripEditor;
				if (!bGripEditor && EditHand >= 0) FinishGripEdit();
				L->Notify(bGripEditor ? TEXT("Grip editor on: squeeze a free fist on what the other hand holds") : TEXT("Grip editor off"), 3.f);
				Pulse(Hand, 0.4f);
			}
			else if (WheelItem[Pick] == TEXT("idev_gripreset"))
			{
				if (EditHand >= 0) FinishGripEdit();
				ResetGrips();
				L->Notify(TEXT("Grips reset"), 2.5f);
				Pulse(Hand, 0.4f);
			}
			else if (IsGear(WheelItem[Pick]))
			{
				ToggleGear(Hand, WheelItem[Pick]);       // the lantern, the map, the plate: into this hand, or put away
			}
			else if (IsTool(WheelItem[Pick]))
			{
				EquipTool(Hand, WheelItem[Pick]);
			}
			else if (WheelItem[Pick].Equals(TEXT("ilantern"), ESearchCase::IgnoreCase))
			{
				// the lantern: picked, it is lit or put out (user 2026-09-27)
				if (L->ToggleLamp()) Pulse(Hand, 0.4f); else Refuse(Hand);
			}
			else if (WheelItem[Pick].Equals(TEXT("idocbag"), ESearchCase::IgnoreCase))
			{
				// the medical bag: picked is used (its SND_USE_S, COOL.wav) (user 2026-09-27)
				if (L->UseItem(WheelItem[Pick])) { Pulse(Hand, 0.5f); ++TutNotes.BagsUsed; } else Refuse(Hand);
			}
			else if (L->UseItem(WheelItem[Pick]))
			{
				Pulse(Hand, 0.5f);
			}
		}
	}
	if (!bWheelHeld[0] && !bWheelHeld[1])
	{
		CloseWheel();
	}
	else
	{
		UpdateWheel();
	}
}

void AOutlawsPlayer::CloseWheel()
{
	// put the wheel away without taking anything
	for (int32 H = 0; H < 2; ++H)
	{
		bWheelHeld[H] = false;
		WheelPick[H] = -1;
		WheelDropPick[H] = -1;
		if (Lasers[H]) Lasers[H]->SetVisibility(false);
	}
	if (!bWheelOpen)
	{
		return;
	}
	bWheelOpen = false;
	WheelArrowHover[0] = WheelArrowHover[1] = 0;
	bWheelTriggerArmed[0] = bWheelTriggerArmed[1] = false;
	bWheelMapZoom = false;               // (BUG-MAPZOOMTRIGGER: closing the wheel sends the map back)
	bWheelMapHover = false;
	WheelMapPointHand = -1;
	if (WheelRoot) WheelRoot->SetVisibility(false, true);
	UGameplayStatics::SetGlobalTimeDilation(this, 1.f);
}

void AOutlawsPlayer::DrawStartPistol()
{
	if (WeaponsOwned(0) <= 0) return;
	const int32 Hand = LeftDominant() ? 0 : 1;
	if (Equipped[Hand] == 0) return;
	Equip(Hand, 0);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT the revolver drawn for the level's start"));
}

void AOutlawsPlayer::SwapWeaponHands(int32 From, int32 To)
{
	const int32 Wi = Equipped[From];
	if (Wi < 0 || Equipped[To] >= 0) return;
	const int32 Rounds = Mag[From];
	Equip(From, -1);
	SavedMag.Add(To * 16 + Wi, Rounds);                 // the same gun: its rounds come with it
	Equip(To, Wi);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT %s passed from the %s hand to the %s (%d rounds)"), GWeapons[Wi].Label, From == 0 ? TEXT("left") : TEXT("right"), To == 0 ? TEXT("left") : TEXT("right"), Rounds);
}

bool AOutlawsPlayer::HoldsGatling(int32 Hand) const
{
	return Hand >= 0 && Hand < 2 && Equipped[Hand] >= 0 && GWeapons[Equipped[Hand]].bAuto;
}

UMaterialInterface* AOutlawsPlayer::HeldSectionMaterial(const FString& Key, const FString& Texture)
{
	UMaterialInterface* Base = HeldBase();
	if (!Base) return nullptr;
	if (Key.StartsWith(TEXT("fuse:")))
	{
		// (BUG-TNT) the wick and its cord on a colour of their own, lit as the rest of the held things; (user build 32,
		// BUG-TNTWHITE: "the fuse is yellow instead of black") a tarred black cord, as the game's own wick
		if (!FuseMaterial) { FuseMaterial = HeldSolidMaterial(FColor(30, 27, 24)); KeepAlive.Add(FuseMaterial); }
		return FuseMaterial;
	}
	// one material per texture for the whole session (swapping guns reloads nothing)
	UMaterialInterface* Mat = GunMaterials.FindRef(Texture);
	if (!Mat)
	{
		// (a card: the pickup sprite on the held sprite material, as the lantern's glass)
		const bool bCard = Texture.StartsWith(TEXT("card:"));
		UMaterialInterface* CardBase = bCard ? LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_HeldSprite.M_OLR_HeldSprite")) : nullptr;
		UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(CardBase ? CardBase : Base, this);
		if (UTexture2D* Tex = bCard ? HeldGunCardLook(Level.Get(), Texture, Mid) : HeldGunTexture(Texture))
		{
			Mid->SetTextureParameterValue(TEXT("Tex"), Tex);
			KeepAlive.Add(Tex);
		}
		KeepAlive.Add(Mid);
		HeldMids.Add(Mid);
		if (HeldLight >= 0.f) Mid->SetScalarParameterValue(TEXT("Brightness"), HeldLight);
		GunMaterials.Add(Texture, Mid);
		Mat = Mid;
	}
	return Mat;
}

TSharedPtr<OutlawsObj::FObjMesh> AOutlawsPlayer::HeldModelFor(int32 Wi, bool& bOutCard, FString& OutPath)
{
	// the held model of weapon Wi (built once a session, GObjCache): Equip's, and a thrown one seen in co-op (BuildThrownLook)
	FWeaponDef& W = GWeapons[Wi];
	UMaterialInterface* WorldBase = HeldBase();      // lit by the sector around the player
	// a shipped gun is built from the player's HD pickup sprite before its mesh is taken; without that sprite (a
	// non-remaster install) or a failed build the gun is held as a card of the low-res one (user 2026-10-02)
	if (!W.Model3do && HeldGunSource(Wi) == EHeldGunSource::Shipped)
	{
		bool bNoSprite = false;
		if (UTexture2D* Painted = BuildShippedGunTexture(Level.Get(), Wi, bNoSprite)) KeepAlive.AddUnique(Painted);
		else if (bNoSprite) GHeldGunSources[Wi] = EHeldGunSource::Card;
	}
	const bool bCardGun = !W.Model3do && HeldGunSource(Wi) == EHeldGunSource::Card;
	const bool bSpriteGun = !W.Model3do && UseSpriteGun(Wi);
	// the gatling: the modelled one (item3d rework.py GGATGUN, user 2026-09-29) where it has been made, else the game's
	const FString GatPath = FPaths::Combine(SpriteHeldDir(), TEXT("GGATGUN_gun.obj"));
	const bool bGatObj = W.bAuto && FPaths::FileExists(GatPath) && !OLFlags::Param(TEXT("OLGameGatling"));
	// (the VE stand-ins are private: never looked for off this PC)
	const FString Path = bGatObj ? GatPath : W.Model3do ? FString(TEXT("3do:")) + W.Model3do
		: bCardGun ? FString(TEXT("card:")) + SpriteGunName(Wi)
		: bSpriteGun ? FPaths::Combine(HeldGunDir(Wi), FString(SpriteGunName(Wi)) + TEXT(".obj"))
		: HeldGunsOnDevPc() ? FPaths::Combine(PlaceholderWeaponDir, W.Obj) : FString();
	TSharedPtr<OutlawsObj::FObjMesh>* Cached = GObjCache.Find(Path);
	if (!Cached)
	{
		TSharedPtr<OutlawsObj::FObjMesh> Obj = MakeShared<OutlawsObj::FObjMesh>();
		bool bOk = false;
		if (bGatObj)
		{
			// the gun piece (rework.py splits it off its tripod): cm about its pivot, the barrels along the model's +Y
			// (its front): turned to fire along +X and made half as big again, so the pivot comes to the waist
			bOk = OutlawsObj::Load(Path, 1.0, true, *Obj);
			const double Sc = 1.5;
			const FQuat Q = FQuat::FindBetweenNormals(FVector::RightVector, FVector::ForwardVector);
			FBox Turned(ForceInit);
			for (TPair<FString, FOutlawsMeshSection>& Kv : Obj->Sections)
			{
				for (FVector& P : Kv.Value.Positions) { P = Q.RotateVector(P) * Sc; Turned += P; }
				for (FVector& N : Kv.Value.Normals) N = Q.RotateVector(N);
			}
			Obj->Bounds = Turned;
			W.Muzzle = FVector(32.0 * Sc, 0.0, 0.0);
			W.MountLift = 0.f;
			W.LoadPoint = FVector(-7.0 * Sc, 0.0, 9.0 * Sc);
			W.ItemScale = float(1.0 / Sc);
			UE_LOG(LogOutlaws, Log, TEXT("OLRT VR: modelled gatling %s, bounds %s, muzzle %s"), *Path, *Turned.ToString(), *W.Muzzle.ToString());
		}
		else if (W.Model3do)
		{
			// the game's own model (a ground item): its longest side along the barrel (+X), scaled to
			// the hand, centred on the grip; textures from the level, one material each
			TMap<FString, FOutlawsMeshSection> Src;
			TMap<FString, UTexture2D*> Tex;
			AOutlawsLevelActor* L = Level.Get();
			// the knife and the bundle in detail (item3d weapons_extra.py, user 2026-10-01) only when asked for
			// (-OLDetailedThrowables): the game's own 3DOs look better and truer to the original, and the detailed ones drew
			// black on the headset (user 2026-10-02).  (FACETURN) They are single-faced and most of their faces were wound
			// inward (the knife 37 % facing out, the bundle 17 %), most likely why they drew black there: turned to face out
			// (-OLOldScopeFaces: as the files have them)
			bool bHave = false;
			if (W.Throw > 0 && OLFlags::Param(TEXT("OLDetailedThrowables")))
			{
				const FString Alt = FPaths::Combine(SpriteHeldDir(), W.Throw == 1 ? TEXT("GDYNAM3D.obj") : TEXT("GKNIFE3D.obj"));
				OutlawsObj::FObjMesh AltObj;
				if (FPaths::FileExists(Alt) && OutlawsObj::Load(Alt, 1.0, true, AltObj) && AltObj.Sections.Num() > 0)
				{
					OutlawsObj::FaceItemModelOutward(AltObj, FString::Printf(TEXT("OLRT held %s"), *FPaths::GetBaseFilename(Alt)));
					for (TPair<FString, FOutlawsMeshSection>& Kv : AltObj.Sections)
					{
						if (!Tex.Contains(Kv.Value.Texture)) if (UTexture2D* T = OutlawsObj::LoadTextureFile(Kv.Value.Texture)) { Tex.Add(Kv.Value.Texture, T); KeepAlive.Add(T); }
						Src.Add(Kv.Key, Kv.Value);
					}
					bHave = true;
				}
			}
			if (bHave || (L && L->BuildItemModel(W.Model3do, Src, Tex)))
			{
				FBox B(ForceInit);
				for (const TPair<FString, FOutlawsMeshSection>& Kv : Src) for (const FVector& P : Kv.Value.Positions) B += P;
				const FVector E = B.GetExtent();
				const FVector Long = E.X >= E.Y && E.X >= E.Z ? FVector::ForwardVector : (E.Y >= E.Z ? FVector::RightVector : FVector::UpVector);
				FQuat Turn = FQuat(FVector::UpVector, W.bFlipModel ? PI : 0.0) * FQuat::FindBetweenNormals(Long, FVector::ForwardVector);
				// knife and dynamite stand up in the fist, blade tip and fuse to the sky, the knife held by its handle
				// (user 2026-09-28: pointing forward they stabbed through the fingers)
				if (W.Throw > 0) Turn = FQuat::FindBetweenNormals(FVector::ForwardVector, FVector::UpVector) * Turn;
				const FVector Grip = FVector(0.0, 0.0, W.Throw == 2 ? W.ModelLength * 0.3 : 0.0);
				const double Scale = W.ModelLength / FMath::Max(1.0, 2.0 * FMath::Max3(E.X, E.Y, E.Z));
				W.ItemScale = float(1.0 / FMath::Max(1e-3, Scale));
				for (const TPair<FString, FOutlawsMeshSection>& Kv : Src)
				{
					FOutlawsMeshSection S = Kv.Value;
					for (FVector& P : S.Positions) P = Turn.RotateVector(P - B.GetCenter()) * Scale + Grip;
					for (FVector& N : S.Normals) N = Turn.RotateVector(N);
					const FString Key = TEXT("3do:") + Kv.Value.Texture;
					S.Texture = Key;
					if (!GunMaterials.Contains(Key) && WorldBase)
					{
						UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(WorldBase, this);
						if (UTexture2D* const* T = Tex.Find(Kv.Value.Texture)) Mid->SetTextureParameterValue(TEXT("Tex"), *T);
						KeepAlive.Add(Mid);
						HeldMids.Add(Mid);
						if (HeldLight >= 0.f) Mid->SetScalarParameterValue(TEXT("Brightness"), HeldLight);
						GunMaterials.Add(Key, Mid);
					}
					for (const FVector& P : S.Positions) Obj->Bounds += P;
					Obj->Sections.Add(Key, S);
				}
				W.Muzzle = W.Throw > 0 ? FVector(0.0, 0.0, W.ModelLength * 0.5) + Grip : FVector(W.ModelLength * 0.5, 0.0, 0.0);
				FBox Turned(ForceInit);
				for (const TPair<FString, FOutlawsMeshSection>& Kv : Obj->Sections) for (const FVector& P : Kv.Value.Positions) Turned += P;
				if (W.Throw == 1)
				{
					// the wick: the thin faces above the bundle's top cap (gdynam.3do: the sticks end at 88% of the length,
					// the wick runs on to the end) into their own sections, so it can burn down
					const double Z0 = Turned.Min.Z, Span = Turned.Max.Z - Turned.Min.Z;
					GFusePivotZ = Z0 + 0.884 * Span;
					GFuseTipZ = Turned.Max.Z;
					TArray<FString> Keys;
					Obj->Sections.GetKeys(Keys);
					for (const FString& K : Keys)
					{
						FOutlawsMeshSection& S = Obj->Sections[K];
						FOutlawsMeshSection Wick;
						Wick.Texture = S.Texture;
						TArray<int32> Keep;
						for (int32 t = 0; t + 2 < S.Triangles.Num(); t += 3)
						{
							const int32 A = S.Triangles[t], Bi = S.Triangles[t + 1], C = S.Triangles[t + 2];
							if ((S.Positions[A].Z + S.Positions[Bi].Z + S.Positions[C].Z) / 3.0 > Z0 + 0.889 * Span)
							{
								for (int32 V : { A, Bi, C })
								{
									Wick.Triangles.Add(Wick.Positions.Num());
									Wick.Positions.Add(S.Positions[V]);
									Wick.Normals.Add(S.Normals.IsValidIndex(V) ? S.Normals[V] : FVector::UpVector);
									Wick.UVs.Add(S.UVs.IsValidIndex(V) ? S.UVs[V] : FVector2D::ZeroVector);
									Wick.Colors.Add(S.Colors.IsValidIndex(V) ? S.Colors[V] : FLinearColor::White);
								}
							}
							else
							{
								Keep.Append({ A, Bi, C });
							}
						}
						if (Wick.Triangles.Num() > 0)
						{
							S.Triangles = Keep;
							Obj->Sections.Add(TEXT("fuse:") + K, Wick);
						}
					}
					// (user 2026-10-03, BUG-TNT: "still has no fuse") the 3DO's wick is a few hair-thin faces that vanish in the
					// headset: a cord added to the wick's section so it burns down with it.  (Build 19: still unseen, tinted by the
					// wick's dark texel and too thin) 1.2 cm across, from the sticks' end to 4 cm past the old tip, the section
					// on a solid hemp colour of its own (the material loop below)
					{
						FString WickKey, BodyKey;
						for (const TPair<FString, FOutlawsMeshSection>& Kv : Obj->Sections)
						{
							if (Kv.Key.StartsWith(TEXT("fuse:"))) { WickKey = Kv.Key; break; }
							if (BodyKey.IsEmpty()) BodyKey = Kv.Key;
						}
						const bool bNew = WickKey.IsEmpty();
						if (bNew && !BodyKey.IsEmpty())
						{
							WickKey = TEXT("fuse:") + BodyKey;
							FOutlawsMeshSection Fresh;
							Fresh.Texture = Obj->Sections[BodyKey].Texture;
							Obj->Sections.Add(WickKey, Fresh);
						}
						if (FOutlawsMeshSection* Wk = Obj->Sections.Find(WickKey))
						{
							FVector2D TopUV = FVector2D(0.5, 0.05);
							FVector2D Mid = FVector2D::ZeroVector;
							double TopZ = -1e9;
							for (int32 v = 0; v < Wk->Positions.Num(); ++v)
							{
								Mid += FVector2D(Wk->Positions[v].X, Wk->Positions[v].Y);
								if (Wk->Positions[v].Z > TopZ) { TopZ = Wk->Positions[v].Z; if (Wk->UVs.IsValidIndex(v)) TopUV = Wk->UVs[v]; }
							}
							Mid = Wk->Positions.Num() > 0 ? Mid / Wk->Positions.Num() : FVector2D::ZeroVector;
							const double C0 = GFusePivotZ - 0.3, Z1 = GFuseTipZ + 4.0, Rc = 0.6;
							constexpr int32 Sides = 6;
							for (int32 sdx = 0; sdx < Sides; ++sdx)
							{
								const double A0 = 2.0 * PI * sdx / Sides, A1 = 2.0 * PI * (sdx + 1) / Sides;
								// (a slight lean, as a cord stands, and a little curl at the top)
								auto At = [&](double A, double Z)
								{
									const double F = (Z - C0) / FMath::Max(0.1, Z1 - C0);
									return FVector(Mid.X + Rc * FMath::Cos(A) + 1.0 * F * F, Mid.Y + Rc * FMath::Sin(A), Z);
								};
								const FVector Q[4] = { At(A0, C0), At(A1, C0), At(A1, Z1), At(A0, Z1) };
								const FVector Nn = FVector(FMath::Cos((A0 + A1) * 0.5), FMath::Sin((A0 + A1) * 0.5), 0.0);
								for (int32 Tri : { 0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2 })
								{
									Wk->Triangles.Add(Wk->Positions.Num());
									Wk->Positions.Add(Q[Tri]);
									Wk->Normals.Add(Nn);
									Wk->UVs.Add(TopUV);
									Wk->Colors.Add(FLinearColor(0.85f, 0.8f, 0.7f));
								}
							}
							GFuseTipZ = Z1;
							UE_LOG(LogOutlaws, Log, TEXT("OLRT dynamite fuse: a cord %.1f cm long added to %s (%s)"), Z1 - C0, *WickKey, bNew ? TEXT("the 3DO had no wick") : TEXT("over the 3DO's wick"));
						}
					}
				}
				W.MountLift = static_cast<float>(-Turned.Min.Z);
				W.LoadPoint = FVector(W.ModelLength * 0.1, 0.0, 0.0);
				bOk = Obj->Sections.Num() > 0;
			}
		}
		else if (bSpriteGun || bCardGun)
		{
			// cm, grip at the origin, bore level: no mount offset or bore fix; its own points from the sidecar (a card
			// lies where the sprite does in the same frame and keeps the shipped gun's points)
			bOk = bCardGun ? MakeHeldGunCard(Wi, *Obj) : LoadHeldObj(Path, 1.0, true, *Obj);
			const FString PointsPath = bCardGun ? FPaths::Combine(ShippedHeldDir(), FString(SpriteGunName(Wi)) + TEXT(".json"))
				: FPaths::ChangeExtension(Path, TEXT("json"));
			FString Text;
			TSharedPtr<FJsonObject> J;
			if (bOk && FFileHelper::LoadFileToString(Text, *PointsPath)
				&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), J) && J.IsValid())
			{
				auto Vec = [&J](const TCHAR* K, FVector& Out)
				{
					const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
					if (J->TryGetArrayField(K, A) && A->Num() == 3) Out = FVector((*A)[0]->AsNumber(), (*A)[1]->AsNumber(), (*A)[2]->AsNumber());
				};
				Vec(TEXT("muzzle"), W.Muzzle);
				Vec(TEXT("hammer"), W.Hammer);
				Vec(TEXT("load"), W.LoadPoint);
				Vec(TEXT("eject"), W.EjectPort);
				FSpriteSights& Si = GSpriteSights[Wi];
				Vec(TEXT("sight_rear"), Si.Rear);
				Vec(TEXT("sight_front"), Si.Front);
				Si.bHammer = J->HasField(TEXT("hammer_pivot"));
				Si.bTrigger = J->HasField(TEXT("trigger_pivot"));
				Vec(TEXT("hammer_pivot"), Si.HammerPivot);
				Vec(TEXT("trigger_pivot"), Si.TriggerPivot);
				Si.bLever = J->HasField(TEXT("lever_pivot"));
				Vec(TEXT("lever_pivot"), Si.LeverPivot);
				Si.bSelector = J->HasField(TEXT("selector"));
				Vec(TEXT("selector"), Si.Selector);
				if (Wi == 0)
				{
					Vec(TEXT("cyl_centre"), GSpriteCylinder.Centre);
					Vec(TEXT("cyl_pivot"), GSpriteCylinder.Pivot);
					J->TryGetNumberField(TEXT("cyl_length"), GSpriteCylLength);
					J->TryGetNumberField(TEXT("chamber_r"), GSpriteChamberR);
					J->TryGetNumberField(TEXT("hole_r"), GSpriteHoleR);
				}
				Si.bOk = !Si.Front.IsZero();
			}
			// seated in the new hands (user 2026-09-28): the trigger's blade (1 cm below its hinge) where the index finger
			// crosses the gun's plane at its resting curl, about 8.4 cm out and 3 cm up; the grip rises half as far
			// into the fist; a little toward the palm
			{
				const FSpriteSights& Fit = GSpriteSights[Wi];
				W.Offset = Fit.bTrigger
					? FVector(8.4 - Fit.TriggerPivot.X, 0.3, FMath::Clamp((3.0 - (Fit.TriggerPivot.Z - 1.0)) * 0.5, -1.0, 2.5))
					: FVector::ZeroVector;
			}
			W.BoreFix = FRotator::ZeroRotator;
			UE_LOG(LogOutlaws, Log, TEXT("OLRT VR: sprite gun %s, muzzle %s"), *Path, *W.Muzzle.ToString());
		}
		else if (!Path.IsEmpty())
		{
			bOk = OutlawsObj::Load(Path, 100.0, true, *Obj);   // RenderWare: metres, right-handed
		}
		if (!bOk)
		{
			Obj.Reset();
		}
		Cached = &GObjCache.Add(Path, Obj);
	}
	bOutCard = bCardGun;
	OutPath = Path;
	return *Cached;
}

int32 AOutlawsPlayer::WeaponCapacity(int32 Weapon)
{
	return Weapon >= 0 && Weapon < NumWeapons ? GWeapons[Weapon].Capacity : 0;
}

namespace
{
	// a gun loaded from the carried ammunition (not a throwable: a stick or a knife is its own)
	bool LoadsFromCarried(int32 Wi)
	{
		return Wi >= 0 && Wi < NumWeapons && GWeapons[Wi].Throw <= 0 && GWeapons[Wi].AmmoItem && *GWeapons[Wi].AmmoItem;
	}
}

int32 AOutlawsPlayer::LoadFromCarried(int32 Wi, int32 Rounds)
{
	const FWeaponDef& W = GWeapons[Wi];
	AOutlawsLevelActor* L = Level.Get();
	Rounds = FMath::Clamp(Rounds, 0, W.Capacity);
	const int32 Want = W.Capacity - Rounds;
	int32 Load = bUnlimitedAmmo ? Want : FMath::Min(Want, L ? L->GetCount(W.AmmoItem) : 0);
	if (Load > 0 && !bUnlimitedAmmo && !(L && L->TakeCount(W.AmmoItem, Load))) Load = 0;
	return Rounds + Load;
}

void AOutlawsPlayer::NoteMag(TMap<int32, int32>& M, int32 Hand, int32 Wi, int32 Rounds) const
{
	M.Add(Hand * 16 + Wi, Rounds);
	// (BUG-HOLSTERRELOAD) one gun carried keeps one count: drawn into the other hand it came out full before
	if (Wi >= 0 && Wi < NumWeapons && GWeapons[Wi].Throw <= 0 && WeaponsOwned(Wi) < 2) M.Add((1 - Hand) * 16 + Wi, Rounds);
}

void AOutlawsPlayer::TopUpForLevelStart()
{
	// (BUG-LEVELCARRY, user 2026-10-17: a level won tired, or with guns part fired, started the next one so) rested, and each
	// gun in hand loaded to full from the carried rounds as the reload button loads: the reserve goes down by what goes in,
	// and a short reserve loads what it has.  The guns came back full for nothing before (ResetForLevel's SavedMag reset).
	// (BUG-HOLSTERRELOAD, user 2026-10-18) the holstered guns too, after the hands: no gun is filled for free at a level start
	Stamina = 100.f;
	AOutlawsLevelActor* L = Level.Get();
	if (bCarrySaved)
	{
		// (ResetForLevel cleared them; only the guns still carried: one found later at Bob's ranch comes full, as any found)
		for (const TPair<int32, int32>& Kv : CarrySavedMag) if (WeaponsOwned(Kv.Key % 16) > 0) SavedMag.Add(Kv.Key, Kv.Value);
		CarrySavedMag.Reset();
		bCarrySaved = false;
	}
	FString Line;
	for (int32 H = 0; H < 2; ++H)
	{
		const int32 Wi = Equipped[H];
		const int32 Had = CarryMag[H];
		CarryMag[H] = -1;
		if (!LoadsFromCarried(Wi)) continue;                           // (a stick or a knife: the hand restocks itself)
		const FWeaponDef& W = GWeapons[Wi];
		if (Had >= 0) Mag[H] = FMath::Clamp(Had, 0, W.Capacity);       // the rounds it was carried in with
		const int32 Before = Mag[H];
		Mag[H] = LoadFromCarried(Wi, Mag[H]);
		Spent[H] = W.bEjectOnShot ? 0 : W.Capacity - Mag[H];
		Line += FString::Printf(TEXT("; %s %s %d -> %d (%d %s left)"), H == 0 ? TEXT("left") : TEXT("right"), W.Label, Before, Mag[H],
			L ? L->GetCount(W.AmmoItem) : 0, W.AmmoItem);
	}
	TopUpHolstered(Line);
	SnapEntryMags();
	UE_LOG(LogOutlaws, Log, TEXT("OLRT level start: stamina full%s"), *Line);
}

void AOutlawsPlayer::TopUpHolstered(FString& Line)
{
	// each gun carried and not in a hand, in the table's order: its rounds as put away (never drawn: it is full), loaded from
	// the carried ammunition.  One carried is one gun (both hands' counts the same after); a pair is two, each its own
	const AOutlawsLevelActor* L = Level.Get();
	for (int32 Wi = 0; Wi < NumWeapons; ++Wi)
	{
		if (!LoadsFromCarried(Wi)) continue;
		const int32 Owned = WeaponsOwned(Wi);
		if (Owned <= 0) continue;
		const FWeaponDef& W = GWeapons[Wi];
		const int32 InHand = (Equipped[0] == Wi ? 1 : 0) + (Equipped[1] == Wi ? 1 : 0);
		if (FMath::Min(Owned, 2) - InHand <= 0) continue;
		for (int32 H = 0; H < 2; ++H)
		{
			if (Owned < 2 && H == 1) break;                               // (one gun: done with the first hand)
			if (Equipped[H] == Wi) continue;                               // (a pair, one in this hand: the other is the holstered one)
			const int32* A = SavedMag.Find(H * 16 + Wi);
			const int32* B = Owned < 2 ? SavedMag.Find((1 - H) * 16 + Wi) : nullptr;
			if (!A && !B) continue;                                        // never drawn: full
			const int32 Before = FMath::Clamp(FMath::Min(A ? *A : W.Capacity, B ? *B : W.Capacity), 0, W.Capacity);
			const int32 After = LoadFromCarried(Wi, Before);
			NoteMag(SavedMag, H, Wi, After);
			Line += FString::Printf(TEXT("; holstered %s%s %d -> %d (%d %s left)"), W.Label, Owned < 2 ? TEXT("") : H == 0 ? TEXT(" (left)") : TEXT(" (right)"),
				Before, After, L ? L->GetCount(W.AmmoItem) : 0, W.AmmoItem);
		}
	}
}

void AOutlawsPlayer::SnapEntryMags()
{
	EntryMags = SavedMag;
	for (int32 H = 0; H < 2; ++H) if (Equipped[H] >= 0) NoteMag(EntryMags, H, Equipped[H], Mag[H]);
}

void AOutlawsPlayer::RestoreEntryMags()
{
	for (const TPair<int32, int32>& Kv : EntryMags) SavedMag.Add(Kv.Key, Kv.Value);
	for (int32 H = 0; H < 2; ++H)
	{
		const int32 Wi = Equipped[H];
		const int32* Was = Wi >= 0 ? EntryMags.Find(H * 16 + Wi) : nullptr;
		if (!Was) continue;
		Mag[H] = FMath::Clamp(*Was, 0, GWeapons[Wi].Capacity);
		Spent[H] = GWeapons[Wi].bEjectOnShot ? 0 : GWeapons[Wi].Capacity - Mag[H];
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT restart: the guns hold what they held as the level was entered (%d counts)"), EntryMags.Num());
}

void AOutlawsPlayer::WriteCarryMags(FJsonObject& O, bool bRestart) const
{
	TMap<int32, int32> M;
	if (bRestart)
	{
		M = EntryMags;
	}
	else
	{
		// between levels (the cutscene: the guns as they are) or after the level was won (stashed, the hands cleared since)
		M = bCarrySaved ? CarrySavedMag : SavedMag;
		for (int32 H = 0; H < 2; ++H)
		{
			const int32 Wi = bCarryHands ? CarryHands[H] : Equipped[H];
			const int32 Rounds = bCarryHands ? CarryMag[H] : Mag[H];
			if (Wi >= 0 && Rounds >= 0) NoteMag(M, H, Wi, Rounds);
		}
	}
	const TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
	for (const TPair<int32, int32>& Kv : M) J->SetNumberField(FString::FromInt(Kv.Key), Kv.Value);
	O.SetObjectField(TEXT("carry_mags"), J);
	O.SetBoolField(TEXT("carry_topup"), !bRestart);
}

void AOutlawsPlayer::ReadCarryMags(const FJsonObject& O)
{
	// a level's opening read (a save of one, or a map load's rebuild): the guns hold what they were carried in with, loaded
	// from the carried ammunition for the next level; an opening without them (older saves) has them full as before
	const TSharedPtr<FJsonObject>* M = nullptr;
	if (O.TryGetObjectField(TEXT("carry_mags"), M) && M && M->IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Kv : (*M)->Values)
		{
			const int32 Key = FCString::Atoi(*Kv.Key);
			if (WeaponsOwned(Key % 16) > 0) SavedMag.Add(Key, int32(Kv.Value->AsNumber()));     // (only the guns still carried)
		}
		for (int32 H = 0; H < 2; ++H)
		{
			const int32 Wi = Equipped[H];
			if (const int32* Was = Wi >= 0 ? SavedMag.Find(H * 16 + Wi) : nullptr) Mag[H] = FMath::Clamp(*Was, 0, GWeapons[Wi].Capacity);
		}
		bool bTopUp = false;
		O.TryGetBoolField(TEXT("carry_topup"), bTopUp);
		FString Line;
		if (bTopUp)
		{
			for (int32 H = 0; H < 2; ++H)
			{
				if (!LoadsFromCarried(Equipped[H])) continue;
				Mag[H] = LoadFromCarried(Equipped[H], Mag[H]);
				Line += FString::Printf(TEXT("; %s %s %d"), H == 0 ? TEXT("left") : TEXT("right"), GWeapons[Equipped[H]].Label, Mag[H]);
			}
			TopUpHolstered(Line);
		}
		for (int32 H = 0; H < 2; ++H)
		{
			const int32 Wi = Equipped[H];
			if (Wi >= 0) Spent[H] = GWeapons[Wi].bEjectOnShot ? 0 : GWeapons[Wi].Capacity - Mag[H];
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLRT opening: %d gun counts carried in%s%s"), (*M)->Values.Num(),
			bTopUp ? TEXT(", loaded from the carried ammunition") : TEXT(" (the entry's)"), *Line);
	}
	SnapEntryMags();
}

void AOutlawsPlayer::Equip(int32 Hand, int32 Weapon)
{
	// the gatling's crank hand takes no other weapon while it is held (user 2026-10-01: that hand is for the crank)
	if (Weapon >= 0 && Weapon < NumWeapons && !GWeapons[Weapon].bAuto && HoldsGatling(1 - Hand))
	{
		Refuse(Hand);
		UE_LOG(LogOutlaws, Log, TEXT("OLRT %s not drawn: the gatling's crank hand"), GWeapons[Weapon].Label);
		return;
	}
	DropTool(Hand);
	// a throw in hand is put away: no arc, and a lit fuse goes out
	ShowThrowArc(Hand, false);
	if (bFuseLit[Hand] && FuseSound[Hand].IsValid()) FuseSound[Hand]->Stop();
	bFuseLit[Hand] = false;
	bKnifeReady[Hand] = false;
	bTntGrip[Hand] = false;
	if (Equipped[Hand] >= 0)
	{
		NoteMag(SavedMag, Hand, Equipped[Hand], Mag[Hand]);   // the gun put away keeps its rounds
	}
	// (REQ-FLICKRELOAD) a gun comes to hand shut; one put away open goes shut as it was (nothing loaded)
	bBreakOpen[Hand] = bBreakCycle[Hand] = false;
	BreakShown[Hand] = BreakCycleTimer[Hand] = 0.f;
	bFlickPrev[Hand] = false;
	bRegrip[0] = bRegrip[1] = false;
	SetFrontHidden(1 - Hand, false);             // (REQ-RELOADHIDEHAND: no reload under way now)
	Equipped[Hand] =Weapon >= 0 && Weapon < NumWeapons ? Weapon : -1;
	if (Equipped[Hand] >= 0)
	{
		const int32* Saved = SavedMag.Find(Hand * 16 + Equipped[Hand]);
		Mag[Hand] = Saved ? *Saved : GWeapons[Equipped[Hand]].Capacity;
		FireCooldown[Hand] = 0.f;
		RecoilPitch[Hand] = 0.f;
		RecoilKick[Hand] = 0.f;
		ReloadTimer[Hand] = 0.f;
		// an emptied revolver still carries its fired cases
		Spent[Hand] = GWeapons[Equipped[Hand]].bEjectOnShot ? 0 : GWeapons[Equipped[Hand]].Capacity - Mag[Hand];
		// each hand holds a gun the player carries (user 2026-10-01): any two weapons mix (a long gun in one hand pays for
		// it, 4 x its spread and kick), and the same one is in both hands only when two are carried (a second revolver or
		// sawed-off picked up); the throwing weapons are a stack, a stick or a knife in each hand.  (The counts are not
		// there yet in the test harnesses: none carried, no rule)
		const int32 O = 1 - Hand;
		const FWeaponDef& Wd = GWeapons[Equipped[Hand]];
		const int32 Carried = WeaponsOwned(Equipped[Hand]);
		if (Equipped[O] == Equipped[Hand] && Wd.Throw <= 0 && Carried > 0 && Carried < 2 && ShotTime < 0.f && SmokeTime < 0.f)
		{
			Equip(O, -1);
		}
		// the gatling: the other hand is its crank's (user 2026-10-01), emptied as it is drawn (nothing is drawn into it
		// after: the top of Equip, ToggleGear)
		else if (Wd.bAuto && (Equipped[O] >= 0 || !HeldTool[O].IsEmpty()))
		{
			Equip(O, -1);
		}
	}
	FanHide[Hand] = 0.f;
	// the revolver's cylinder is its own part on the gun: hidden unless the new weapon is a revolver
	// (hiding the gun alone left it on the bare wrist)
	if (CylMesh[Hand] && !HasCylinder(Equipped[Hand])) CylMesh[Hand]->SetVisibility(false);
	if (Equipped[Hand] < 0)
	{
		if (WeaponMesh[Hand]) WeaponMesh[Hand]->SetVisibility(false);
		if (ScopeMesh[Hand]) ScopeMesh[Hand]->SetVisibility(false);      // (fastened to the gun: hidden on its own)
		if (AmmoText[Hand]) AmmoText[Hand]->SetVisibility(false);
		if (AmmoPlate[Hand] >= 0) PlatePlanes[AmmoPlate[Hand]]->SetVisibility(false);
		if (HammerMesh[Hand]) HammerMesh[Hand]->SetVisibility(false);
		if (TriggerMesh[Hand]) TriggerMesh[Hand]->SetVisibility(false);
		if (LeverMesh[Hand]) LeverMesh[Hand]->SetVisibility(false);
		if (SelPivot[Hand]) SelPivot[Hand]->SetVisibility(false, true);
		if (BarrelMesh[Hand]) BarrelMesh[Hand]->SetVisibility(false);
		// the revolver's cylinder and its rounds too (user 2026-09-28: a long gun in the other hand emptied this one and
		// the rounds stayed in the air where the cylinder had been)
		if (CylMesh[Hand]) CylMesh[Hand]->SetVisibility(false);
		ShowRounds(Hand, false);
		UE_LOG(LogOutlaws, Log, TEXT("OLRT VR %s hand: bare"), Hand == 0 ? TEXT("left") : TEXT("right"));
		return;
	}
	FWeaponDef& W = GWeapons[Equipped[Hand]];
	bool bCardGun = false;
	FString Path;
	const TSharedPtr<OutlawsObj::FObjMesh> Model = HeldModelFor(Equipped[Hand], bCardGun, Path);
	if (!Model.IsValid())
	{
		UE_LOG(LogOutlaws, Warning, TEXT("OLRT VR: no model for %s (%s): the hand left empty"), W.Label, Path.IsEmpty() ? TEXT("no held gun") : *Path);
		Equipped[Hand] = -1;
		if (WeaponMesh[Hand]) WeaponMesh[Hand]->SetVisibility(false);
		if (AmmoText[Hand]) AmmoText[Hand]->SetVisibility(false);
		if (AmmoPlate[Hand] >= 0) PlatePlanes[AmmoPlate[Hand]]->SetVisibility(false);
		return;
	}
	if (!WeaponMesh[Hand])
	{
		WeaponMesh[Hand] = NewObject<UProceduralMeshComponent>(this);
		WeaponMesh[Hand]->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		WeaponMesh[Hand]->SetCastShadow(false);
		WeaponMesh[Hand]->SetupAttachment(FrameOf(Hand));
		WeaponMesh[Hand]->RegisterComponent();
	}
	UProceduralMeshComponent* Gun = WeaponMesh[Hand];
	GunBounds[Hand] = Model->Bounds;
	Gun->ClearAllMeshSections(); OutlawsInk::Forget(Gun);
	UMaterialInterface* Base = HeldBase();
	int32 Index = 0;
	FuseSection[Hand] = -1;
	FuseShown[Hand] = 1.f;
	for (const TPair<FString, FOutlawsMeshSection>& Kv : Model->Sections)
	{
		const FOutlawsMeshSection& S = Kv.Value;
		Gun->CreateMeshSection_LinearColor(Index, S.Positions, S.Triangles, S.Normals, S.UVs, S.Colors, TArray<FProcMeshTangent>(), false);
		if (Kv.Key.StartsWith(TEXT("fuse:")))
		{
			FuseSection[Hand] = Index;
			FuseRestPos[Hand] = S.Positions;
			FuseRestNrm[Hand] = S.Normals;
			FuseRestUV[Hand] = S.UVs;
			FuseRestCol[Hand] = S.Colors;
		}
		if (Base)
		{
			if (UMaterialInterface* Mat = HeldSectionMaterial(Kv.Key, S.Texture)) Gun->SetMaterial(Index, Mat);
		}
		++Index;
	}
	BuildBreakBarrels(Hand, Equipped[Hand], bCardGun);      // (REQ-FLICKRELOAD: before the ink shell, which each part gets of its own)
	// the right hand holds it as modelled; the left hand holds a mirror image
	const bool bLeft = Hand == 0;
	GunBaseLoc[Hand] = DefaultGunLoc(Equipped[Hand], bLeft);      // (the baked grip, else the table's: BUG-GRIPSAVE)
	Gun->SetRelativeLocation(GunBaseLoc[Hand]);
	GunBaseRot[Hand] = (bLeft ? FRotator(W.BoreFix.Pitch, -W.BoreFix.Yaw, -W.BoreFix.Roll) : W.BoreFix) + FRotator(W.HoldPitch, 0.0, 0.0);
	Gun->SetRelativeRotation(GunBaseRot[Hand]);
	// (placed by the grip editor this session: that, user 2026-10-01)
	if (const FTransform* Adj = GripAdjust.Find(FString::Printf(TEXT("weapon:%s@%s"), W.Label, bLeft ? TEXT("L") : TEXT("R"))))
	{
		GunBaseLoc[Hand] = Adj->GetLocation();         // (the place only: its turn is the aim's, user 2026-10-01)
		Gun->SetRelativeLocation(GunBaseLoc[Hand]);
	}
	Gun->SetRelativeScale3D(FVector(1.0, bLeft ? -1.0 : 1.0, 1.0));
	Gun->SetVisibility(true);
	// (the headset's ink shell leaves the dynamite's wick out, user 2026-10-01: its black copy, a wick's width round so
	// thin a thing and never burning down with it, hid the wick there; AddShell passes hidden sections by)
	if (FuseSection[Hand] >= 0) Gun->SetMeshSectionVisible(FuseSection[Hand], false);
	if (bCardGun) Gun->SetMeshSectionVisible(0, false);      // (nor round a card: a black slab behind it)
	KeepDrawnOverWorld(Gun);
	if (bCardGun) Gun->SetMeshSectionVisible(0, true);
	if (FuseSection[Hand] >= 0) Gun->SetMeshSectionVisible(FuseSection[Hand], true);
	// the cylinder, centred on its own axis, under a pivot at the crane (a card has none)
	if (HasCylinder(Equipped[Hand]) && !bCardGun)
	{
		const FCylinderDef& CylDef = Cylinder();
		const bool bSpriteCyl = UseSpriteGun(0);
		const FString CylPath = FPaths::Combine(bSpriteCyl ? HeldGunDir(0) : PlaceholderWeaponDir, CylDef.Obj);
		TSharedPtr<OutlawsObj::FObjMesh>* Cyl = GObjCache.Find(CylPath);
		if (!Cyl)
		{
			TSharedPtr<OutlawsObj::FObjMesh> Obj = MakeShared<OutlawsObj::FObjMesh>();
			if (LoadHeldObj(CylPath, bSpriteCyl ? 1.0 : 100.0, true, *Obj))
			{
				for (TPair<FString, FOutlawsMeshSection>& Kv : Obj->Sections)
				{
					for (FVector& P : Kv.Value.Positions) P -= CylDef.Centre;
				}
			}
			else
			{
				Obj.Reset();
			}
			Cyl = &GObjCache.Add(CylPath, Obj);
		}
		if (Cyl->IsValid())
		{
			if (!CylPivot[Hand])
			{
				CylPivot[Hand] = NewObject<USceneComponent>(this);
				CylPivot[Hand]->SetupAttachment(Gun);
				CylPivot[Hand]->RegisterComponent();
				CylMesh[Hand] = NewObject<UProceduralMeshComponent>(this);
				CylMesh[Hand]->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				CylMesh[Hand]->SetCastShadow(false);
				CylMesh[Hand]->SetupAttachment(CylPivot[Hand]);
				CylMesh[Hand]->RegisterComponent();
			}
			CylPivot[Hand]->SetRelativeLocationAndRotation(CylDef.Pivot, FQuat::Identity);
			CylMesh[Hand]->SetRelativeLocationAndRotation(CylDef.Centre - CylDef.Pivot, FQuat::Identity);
			CylMesh[Hand]->ClearAllMeshSections(); OutlawsInk::Forget(CylMesh[Hand]);
			int32 Ci = 0;
			for (const TPair<FString, FOutlawsMeshSection>& Kv : (*Cyl)->Sections)
			{
				const FOutlawsMeshSection& S = Kv.Value;
				CylMesh[Hand]->CreateMeshSection_LinearColor(Ci, S.Positions, S.Triangles, S.Normals, S.UVs, S.Colors, TArray<FProcMeshTangent>(), false);
				if (UMaterialInterface* Mat = GunMaterials.FindRef(S.Texture)) CylMesh[Hand]->SetMaterial(Ci, Mat);
				++Ci;
			}
			CylOut[Hand] = 0.f;
			CylSpin[Hand] = CylSpinTarget[Hand] = 0.f;
			if (bSpriteCyl) BuildRounds(Hand);
			KeepDrawnOverWorld(CylMesh[Hand]);
		}
	}
	else if (CylMesh[Hand])
	{
		CylMesh[Hand]->SetVisibility(false);
	}
	if (!HasCylinder(Equipped[Hand]) || !UseSpriteGun(0)) ShowRounds(Hand, false);
	BuildGunParts(Hand, Equipped[Hand]);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT VR %s hand: %s (%d sections)"), bLeft ? TEXT("left") : TEXT("right"), W.Label, Index);
}

// ---- firing ---------------------------------------------------------------------------------------

namespace
{
	UStaticMeshComponent* MakeFx(AActor* Owner, USceneComponent* Parent, UMaterialInterface* Mat, const TCHAR* Mesh = TEXT("/Engine/BasicShapes/Cube.Cube"))
	{
		UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(Owner);
		C->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, Mesh));
		C->SetMaterial(0, Mat);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCastShadow(false);
		C->SetupAttachment(Parent);
		C->SetUsingAbsoluteLocation(true);
		C->SetUsingAbsoluteRotation(true);
		C->SetUsingAbsoluteScale(true);
		C->RegisterComponent();
		C->SetVisibility(false);
		return C;
	}
}

void AOutlawsPlayer::UpdateGuns(float Dt)
{
	OL_PERF_SCOPE("Player: guns");
	if (!bVR)
	{
		return;
	}
	for (int32 H = 0; H < 2; ++H)
	{
		FireCooldown[H] = FMath::Max(0.f, FireCooldown[H] - Dt);
		// effects fade
		if (FxLeft[H] > 0.f && !(Level.IsValid() && Level->bFlash3DHold))      // (REQ-MUZZLEFLASH3D: a test holds the flash for its picture)
		{
			FxLeft[H] -= Dt;
			if (FxLeft[H] <= 0.f)
			{
				if (MuzzleFx[H]) MuzzleFx[H]->SetVisibility(false);
				if (FlashStar[H]) FlashStar[H]->SetVisibility(false);
				if (TracerFx[H]) TracerFx[H]->SetVisibility(false);
				if (SparkFx[H]) SparkFx[H]->SetVisibility(false);
			}
		}
		const int32 Wi = Equipped[H];
		if (Wi < 0 || !WeaponMesh[H])
		{
			// nothing in this hand: nothing of a revolver may linger in it, however the hand came to be empty
			if (CylMesh[H] && CylMesh[H]->IsVisible()) CylMesh[H]->SetVisibility(false);
			ShowRounds(H, false);
			SetFrontHidden(1 - H, false);       // (REQ-RELOADHIDEHAND: no gun here, no reload hiding the other hand)
			continue;
		}
		const FWeaponDef& W = GWeapons[Wi];
		UProceduralMeshComponent* Gun = WeaponMesh[H];
		// a hand busy fanning the other revolver has its own gun put away until the fanning stops
		FanHide[H] = FMath::Max(0.f, FanHide[H] - Dt);
		// put away (hand only) while that hand fans the other revolver, holds a ledge, the shovel's shaft or the other
		// hand's long gun by its forend (user 2026-10-01: a weapon in the hand no bar to the two-handed hold)
		// (and while it is off the forend only for the other gun's button reload: it takes the forend again on the shut)
		const bool bPutAway = FanHide[H] > 0.f || GrabHand == H || bToolSupport[H] || bSupport[H] || bRegrip[H];
		if (Gun->IsVisible() == bPutAway)
		{
			Gun->SetVisibility(!bPutAway, false);     // the ammo readout is set on its own below
		}
		// recoil: the gun snaps back along the barrel and up, then settles into the fist
		RecoilPitch[H] *= FMath::Exp(-11.f * Dt);
		RecoilKick[H] *= FMath::Exp(-9.f * Dt);            // the jolt back eases home a little slower than the muzzle settles
		if (W.bAuto && FPaths::FileExists(FPaths::Combine(SpriteHeldDir(), TEXT("GGATGUN_tripod.obj"))) && !OLFlags::Param(TEXT("OLGameGatling")))
		{
			// (user 2026-09-29) carried in the hand by the handle bar behind its breech, free to point and move anywhere;
			// the tripod hangs under the gun's pivot, its legs always straight down to the floor (level, turned with the
			// gun), not planted; the other hand can wind the crank on its left side to fire as well as the trigger
			constexpr double Sc = 1.5, PivotZ = 66.0 * Sc;
			EnsureGatTripod();
			// left and right where the head looks, up and down by the hand (user 2026-10-01: held in front of the body,
			// the hand's heading is not where they mean to shoot, its height is), eased so the head's small moves do not
			// shake it; never rolled; in the left hand it is mirrored, its crank on the right, in the right hand on the left
			const FTransform HandXf = FrameOf(H)->GetComponentTransform();
			const FVector HandFwd = HandXf.GetRotation().GetForwardVector();
			const FRotator Head = Camera->GetComponentRotation();
			const FRotator WantAim(FMath::Clamp(float(HandFwd.Rotation().Pitch), -60.f, 60.f), float(Head.Yaw), 0.f);
			GatAim = GFrameCounter - GatShownFrame > 2 ? WantAim : FMath::RInterpTo(GatAim, WantAim, Dt, 12.f);
			const float Yaw = float(GatAim.Yaw);
			const FQuat R = FRotator(GatAim.Pitch, GatAim.Yaw, 0.f).Quaternion();
			const FVector Fwd = R.GetForwardVector();
			const bool bLeftHeld = H == 0;
			const double Side = bLeftHeld ? -1.0 : 1.0;      // the crank on the far side from the holding hand: model -Y is left
			const FVector Pivot = HandXf.GetLocation() + R.RotateVector(FVector(24.0 * Sc, 0.0, 1.0 * Sc)) - Fwd * RecoilKick[H] * 0.3;
			Gun->SetWorldLocationAndRotation(Pivot, R);
			Gun->SetWorldScale3D(FVector(1.0, bLeftHeld ? -1.0 : 1.0, 1.0));
			if (GatSpin)
			{
				// the barrel drum turns about the bore with the crank, a sixth of a turn a round (wound by hand or by the
				// trigger's shots), eased so a trigger shot's sixth swings round rather than jumps (user 2026-10-01)
				GatSpinShown = FMath::FInterpTo(GatSpinShown, GatCrankAngle, Dt, 18.f);
				const FQuat Turn(R.GetForwardVector(), FMath::DegreesToRadians(GatSpinShown * float(Side)));
				GatSpin->SetWorldLocationAndRotation(Pivot, Turn * R);
				GatSpin->SetWorldScale3D(FVector(1.0, bLeftHeld ? -1.0 : 1.0, 1.0));
				GatSpin->SetVisibility(!bPutAway);
			}
			if (GatTripod)
			{
				GatTripod->SetWorldLocationAndRotation(Pivot - FVector(0.0, 0.0, PivotZ), FRotator(0.0, Yaw, 0.0));
				GatTripod->SetVisibility(!bPutAway);
				GatShownFrame = GFrameCounter;
			}
			if (GatCrank)
			{
				// the crank: its axle out of the breech's side (the model's (10, 0, -9) from the pivot, turned: -9, -10
				// here), turning about the model's -Y; the handle 7 cm out from the axle, 1.75 along it
				const FTransform GunXf(R, Pivot);
				const FVector AxisL(0.0, -Side, 0.0);
				const FVector PivW = GunXf.TransformPosition(FVector(-9.0, -10.0 * Side, 0.0) * Sc);
				const FVector AxW = GunXf.TransformVectorNoScale(AxisL);
				const FQuat Spin(AxW, FMath::DegreesToRadians(GatCrankAngle));
				GatCrank->SetWorldLocationAndRotation(PivW, Spin * R);
				GatCrank->SetWorldScale3D(FVector(1.0, Side, 1.0));
				GatCrank->SetVisibility(!bPutAway);
				const FVector A0 = GunXf.TransformVectorNoScale(FVector(0.0, 0.0, -1.0));
				const FVector B0 = FVector::CrossProduct(AxW, A0);
				const FVector HandleW = PivW + Spin.RotateVector(A0 * 7.0 * Sc + AxW * 1.75 * Sc);
				const int32 O = 1 - H;
				const FVector HO = GripOf(O)->GetComponentLocation();
				const float GO = Axis(O == 0 ? TEXT("OLVR_LeftGrip") : TEXT("OLVR_RightGrip"));
				const bool bFree = Equipped[O] < 0 && !HeldItem[O].IsValid() && !HeldBottle[O] && HeldTool[O].IsEmpty();
				const bool bWas = bCranking;
				bCranking = bFree && !bPutAway && GO > 0.5f && (bCranking || FVector::Dist(HO, HandleW) < 14.0);
				// the crank glows white as the free hand nears it, as any thing to grab does (UpdateGrabGlow)
				if (bFree && !bPutAway && !bCranking && FVector::Dist(HO, HandleW) < 30.0) CrankGlowFrame = GFrameCounter;
				if (bCranking)
				{
					// the cranking hand drawn on the handle, going round with it (user 2026-10-01: locked to it, so it
					// shows it is held), its fist round the handle (the handle through the fist's up)
					if (UOutlawsBoxHand* Box = (O == 0 ? LeftHand.Get() : RightHand.Get()))
					{
						Box->SetWorldLocationAndRotation(HandleW, FRotationMatrix::MakeFromZX(AxW, Fwd).ToQuat());
						ForeSnapFrame[O] = GFrameCounter;
					}
					const FVector V = HO - PivW;
					const float Ang = FMath::RadiansToDegrees(FMath::Atan2(FVector::DotProduct(V, B0), FVector::DotProduct(V, A0)));
					if (bWas)
					{
						const float Dlt = FRotator::NormalizeAxis(Ang - GatCrankLast);
						GatCrankAngle += Dlt;
						GatCrankFire = FMath::Min(GatCrankFire + FMath::Abs(Dlt), 120.f);
						if (GatCrankFire >= 60.f)
						{
							// a round every sixth of a turn, as the barrels come round (one shot a frame at most)
							GatCrankFire -= 60.f;
							FireCooldown[H] = 0.f;
							bCrankShot = true;
							FireGun(H, false);
							bCrankShot = false;
						}
					}
					GatCrankLast = Ang;
				}
			}
		}
		else if (W.bAuto)
		{
			// the gatling stands on its carriage in front of the player, turned (slowly) to face where
			// the head faces, a little up or down with the look
			const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
			const FRotator Look = Camera->GetComponentRotation();
			MountYaw = FMath::RInterpTo(FRotator(0.0, MountYaw, 0.0), FRotator(0.0, Look.Yaw, 0.0), Dt, 6.f).Yaw;
			const FRotator Face(FMath::Clamp(Look.Pitch * 0.6f, -20.f, 25.f), MountYaw, 0.0);
			const FVector Ahead = FRotator(0.0, MountYaw, 0.0).Vector();
			Gun->SetWorldLocationAndRotation(Feet + Ahead * 75.0 + FVector(0.0, 0.0, W.MountLift) - Face.Vector() * RecoilKick[H] * 0.3, Face);
			Gun->SetWorldScale3D(FVector::OneVector);
		}
		else if (W.bTwoHand && bSupport[1 - H])
		{
			// three contact points: the rear hand holds the grip, the other hand the forend, and the stock
			// settles into a shoulder set off the head (as in Half-Life: Alyx); the barrel runs from the
			// rear hand (or the shoulder) through the front hand, rolled with the rear hand
			// anchored to both hands: the rear hand stays on the grip exactly as it holds the gun alone, and
			// the spot the front hand closed on stays in the front hand (or, shouldered, the butt stays at
			// the shoulder and that spot in the front hand); the roll is the rear hand's
			const bool bLeft = H == 0;
			const FVector Mirror(1.0, bLeft ? -1.0 : 1.0, 1.0);
			const FTransform HandXf = FrameOf(H)->GetComponentTransform();
			const FTransform Held(GunBaseRot[H].Quaternion(), GunBaseLoc[H], Mirror);   // the gun in the rear hand, one-handed
			const FVector HandModel = Held.InverseTransformPosition(FVector::ZeroVector);  // the rear hand, in the gun's model space
			const FVector ForeModel = SupportAnchor[H];
			const FVector Rear = HandXf.GetLocation();
			const FVector Front = GripOf(1 - H)->GetComponentLocation();
			const FVector Up = (HandXf.GetRotation() * GunBaseRot[H].Quaternion()).RotateVector(FVector::UpVector);
			// the rotation taking the model's (from -> front-hand spot) line onto the world's (from -> front hand)
			auto Solve = [&](const FVector& FromModel, const FVector& FromWorld, FQuat& OutQ, FVector& OutPos)
			{
				const FVector Lm = (ForeModel - FromModel) * Mirror;         // in the gun's (mirrored) local space
				const FQuat InModel = FRotationMatrix::MakeFromXZ(Lm.GetSafeNormal(), FVector::UpVector).ToQuat();
				const FQuat InWorld = FRotationMatrix::MakeFromXZ((Front - FromWorld).GetSafeNormal(), Up).ToQuat();
				OutQ = InWorld * InModel.Inverse();
				OutPos = FromWorld - OutQ.RotateVector(FromModel * Mirror);
			};
			FQuat Q;
			FVector Pos;
			Solve(HandModel, Rear, Q, Pos);
			// the virtual stock (as Pavlov's): raised to the face, the gun lies along the line from the aiming
			// eye (the gun hand's side) through the front hand, so the sights sit on that eye. It blends in
			// by how near the sight line already runs to the eye, so there is no snap on or off
			const FVector BoreModel = W.BoreFix.Quaternion().Inverse().RotateVector(FVector::ForwardVector);  // the barrel's way (model space)
			// the sight line: through the gun's own rear and front sights where they are known, else along the bore over
			// the gun's top (a guess that ran 5.9 cm to the side of the rifle's real sights)
			FVector SightRear, SightFront;
			FVector SightDir = BoreModel, SightRef;
			if (SightPoints(Equipped[H], SightRear, SightFront))
			{
				SightDir = (SightFront - SightRear).GetSafeNormal();
				SightRef = SightRear;
			}
			else
			{
				const double SightH = FMath::Clamp(double(GunBounds[H].Max.Z) - W.Muzzle.Z, 1.5, 9.0);     // the sights ride the gun's top
				SightRef = W.Muzzle + FVector(0.0, 0.0, SightH);
			}
			const FVector SightFore = SightRef + SightDir * FVector::DotProduct(ForeModel - SightRef, SightDir);  // the sight line over the front hand
			const FVector ForeOff = ForeModel - SightFore;                                                   // the front hand, off that line
			const FVector AimEye = Camera->GetComponentLocation() + Camera->GetRightVector() * (bLeft ? -3.2 : 3.2);
			{
				// by the gun hand's nearness to the eye (user 2026-09-27): up at the face the sights go
				// onto the eye; held lower, at the chest, the hands alone aim it
				const FVector Dh = Q.RotateVector(BoreModel * Mirror).GetSafeNormal();
				const double HandToEye = FVector::Dist(Rear, AimEye);
				const bool bFacing = FVector::DotProduct(Camera->GetForwardVector(), Dh) > 0.5;
				const float Target = bFacing ? 1.f - FMath::SmoothStep(24.f, 38.f, float(HandToEye)) : 0.f;
				StockBlend[H] = FMath::FInterpTo(StockBlend[H], Target, Dt, 10.f);
			}
			if (StockBlend[H] > 0.001f)
			{
				const FVector GunUp = Q.RotateVector(FVector::UpVector);        // the roll stays the rear hand's
				const FQuat InModel = FRotationMatrix::MakeFromXZ(SightDir * Mirror, FVector::UpVector).ToQuat();
				// the sight line runs through the aiming eye itself: rear sight, front sight and eye on one straight
				// line (user 2026-09-28; the hands' own left-right offset, kept before, put it beside the eye)
				const FVector Pivot = AimEye;
				FQuat QA = Q;
				FVector SightWorld = Front;
				for (int32 It = 0; It < 3; ++It)
				{
					SightWorld = Front - QA.RotateVector(ForeOff * Mirror);         // the sight line's point over the front hand
					QA = FRotationMatrix::MakeFromXZ((SightWorld - Pivot).GetSafeNormal(), GunUp).ToQuat() * InModel.Inverse();
				}
				const FVector PosA = SightWorld - QA.RotateVector(SightFore * Mirror);
				Q = FQuat::Slerp(Q, QA, StockBlend[H]);
				Pos = FMath::Lerp(Pos, PosA, double(StockBlend[H]));
				// proof: how far the aiming eye sits off the rear-front sight line once fully shouldered (0 is the aim)
				if (StockBlend[H] > 0.99f && SightPoints(Equipped[H], SightRear, SightFront))
				{
					static double LastSightLog = 0.0;
					const double NowS = FPlatformTime::Seconds();
					if (NowS - LastSightLog > 1.0)
					{
						LastSightLog = NowS;
						const FVector Rw = Pos + Q.RotateVector(SightRear * Mirror), Fw = Pos + Q.RotateVector(SightFront * Mirror);
						const double Off = FMath::PointDistToLine(AimEye, (Fw - Rw).GetSafeNormal(), Rw);
						UE_LOG(LogOutlaws, Log, TEXT("OLRT sights: %s shouldered, the aiming eye %.2f cm off the rear-front sight line (rear %.0f cm from the eye)"), W.Label, Off, FVector::Dist(AimEye, Rw));
					}
				}
			}
			bShouldered[H] = bShouldered[H] ? StockBlend[H] > 0.35f : StockBlend[H] > 0.65f;
			const FVector Dir = Q.RotateVector(W.BoreFix.Quaternion().Inverse().RotateVector(FVector::ForwardVector) * Mirror).GetSafeNormal();
			// two hands and a shoulder take the kick: a little slide back, a little lift
			Q = Q * FRotator(RecoilPitch[H] * 0.3f, 0.0, 0.0).Quaternion();
			Gun->SetWorldLocationAndRotation(Pos - Dir * RecoilKick[H] * 0.4f, Q);
			// the front hand drawn on the forend: wrapped under the barrel, knuckles along it
			if (UOutlawsBoxHand* Box = (H == 0 ? RightHand.Get() : LeftHand.Get()))
			{
				const FTransform GunXf = Gun->GetComponentTransform();
				const FVector GunUp = GunXf.GetUnitAxis(EAxis::Z);
				const FVector Spot = GunXf.TransformPosition(ForeModel) - GunUp * 1.5;
				Box->SetWorldLocationAndRotation(Spot, FRotationMatrix::MakeFromXZ(Dir, GunUp).ToQuat());
				ForeSnapFrame[1 - H] = GFrameCounter;
			}
			if (bShouldered[H] != bWasShouldered[H])
			{
				bWasShouldered[H] = bShouldered[H];
				UE_LOG(LogOutlaws, Log, TEXT("OLRT %s %s the cheek (virtual stock)"), W.Label, bShouldered[H] ? TEXT("to") : TEXT("off"));
				Pulse(H, bShouldered[H] ? 0.25f : 0.1f);
			}
		}
		else
		{
			bShouldered[H] = false;
			StockBlend[H] = 0.f;
			Gun->SetRelativeRotation(GunBaseRot[H] + FRotator(RecoilPitch[H], 0.0, 0.0));
			Gun->SetRelativeLocation(GunBaseLoc[H] - FVector(RecoilKick[H], 0.0, 0.0));
			// the hand goes with the kick (user 2026-09-28: the gun slid in a still hand): the gun's move from its rest
			// pose, applied to the hand
			if (UOutlawsBoxHand* Box = (H == 0 ? LeftHand.Get() : RightHand.Get()); Box && (RecoilKick[H] > 0.02f || RecoilPitch[H] > 0.05f))
			{
				const FTransform Frame = FrameOf(H)->GetComponentTransform();
				const FTransform Rest = FTransform(GunBaseRot[H].Quaternion(), GunBaseLoc[H]) * Frame;
				const FTransform Now = FTransform((GunBaseRot[H] + FRotator(RecoilPitch[H], 0.0, 0.0)).Quaternion(), GunBaseLoc[H] - FVector(RecoilKick[H], 0.0, 0.0)) * Frame;
				if (USceneComponent* BoxParent = Box->GetAttachParent())
				{
					Box->SetWorldTransform(BoxParent->GetComponentTransform() * (Rest.Inverse() * Now));
					ForeSnapFrame[H] = GFrameCounter;
				}
			}
		}
		// (fanned shots find their man by the view, the gun itself stays where the hand holds it)
		FanActive[H] = FMath::Max(0.f, FanActive[H] - Dt);
		// trigger: one shot per pull (pull past 0.7, re-armed under 0.3)
		const float T = Axis(H == 0 ? TEXT("OLVR_LeftTrigger") : TEXT("OLVR_RightTrigger"));
		if (T < 0.3f)
		{
			bTriggerArmed[H] = true;
		}
		if (W.Throw > 0)
		{
			if (!bPutAway)
			{
				UpdateThrowable(H, T, Dt);
				Gun->SetVisibility(Mag[H] > 0, false);    // the next stick or knife comes to hand
			}
			else
			{
				ShowThrowArc(H, false);                     // nothing thrown from a hand that is busy
			}
		}
		else if (T > 0.7f && (bTriggerArmed[H] || W.bAuto) && !bWheelHeld[H] && !bPutAway)
		{
			bTriggerArmed[H] = false;
			FireGun(H, false);
		}
		UpdateFan(H, bPutAway);
		// reload: the reload button loads a round at a time from the reserve (the hold-to-body reload is
		// gone: a button only, user 2026-09-27)
		AOutlawsLevelActor* L = Level.Get();
		const int32 Reserve = L ? L->GetCount(W.AmmoItem) : 0;
		// the gatling's belt goes in by itself when it runs dry (AUTO_RELOAD)
		if (W.bAuto && Mag[H] <= 0 && Reserve > 0 && FireCooldown[H] <= 0.f && L)
		{
			const int32 N = FMath::Min(W.Capacity, Reserve);
			if (L->TakeCount(W.AmmoItem, N))
			{
				Mag[H] = N;
				FireCooldown[H] = 1.0f;
				L->PlaySoundAt(W.ReloadSound, Gun->GetComponentLocation());
				Pulse(H, 0.4f);
			}
		}
		// (REQ-FLICKRELOAD) the shotguns break open by a flick or the reload button, and load as they shut: their own cycle
		TickBreak(H, Dt, bWheelHeld[H] || bPutAway);
		const bool bHandLoads = !W.bAuto && W.Throw == 0 && !IsBreakGun(Wi);
		if (!bHandLoads) bButtonReload[H] = false;
		if (bButtonReload[H] && (Mag[H] >= W.Capacity || Reserve <= 0))
		{
			bButtonReload[H] = false;      // full, or nothing left to load
		}
		if (bHandLoads && !bWheelHeld[H] && !bPutAway && bButtonReload[H] && Mag[H] < W.Capacity && Reserve > 0)
		{
			ReloadTimer[H] += Dt;
			if (ReloadTimer[H] >= W.ReloadEach && L->TakeCount(W.AmmoItem, 1))
			{
				ReloadTimer[H] = 0.f;
				++Mag[H];
				CylSpinTarget[H] -= Cylinder().Chamber;     // back one: the empty chamber behind the top comes to the gate
				// a revolver pushes one fired case out for each round that goes in
				if (Spent[H] > 0)
				{
					--Spent[H];
					EjectCase(H);
				}
				L->PlaySoundAt(W.ReloadSound, Gun->GetComponentTransform().TransformPosition(W.LoadPoint));
				Pulse(H, 0.25f);
			}
		}
		else
		{
			ReloadTimer[H] = 0.f;
		}
		bReloadingNow[H] = bHandLoads && !bWheelHeld[H] && !bPutAway && bButtonReload[H] && Mag[H] < W.Capacity && Reserve > 0;
		UpdateCylinder(H, Dt);
		UpdateGunParts(H, Dt);
		UpdateSelector(H, Dt);
		// ammo readout, just above the gun, turned to the eyes
		if (!AmmoText[H])
		{
			AmmoText[H] = NewObject<UTextRenderComponent>(this);
			AmmoText[H]->SetHorizontalAlignment(EHTA_Center);
			AmmoText[H]->SetVerticalAlignment(EVRTA_TextCenter);
			AmmoText[H]->SetWorldSize(1.6f);
			AmmoText[H]->SetTextRenderColor(FColor(235, 220, 180));
			if (UMaterialInterface* TextMat = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_Text.M_OLR_Text")))
			{
				AmmoText[H]->SetTextMaterial(TextMat);
			}
			AmmoText[H]->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			AmmoText[H]->SetupAttachment(Gun);
			AmmoText[H]->RegisterComponent();
		}
		const FVector Above = Gun->GetComponentTransform().TransformPosition(FVector(W.LoadPoint.X, W.LoadPoint.Y, W.Muzzle.Z + 7.0));
		AmmoText[H]->SetVisibility(false);          // (the plate below took its place: the remaster's font)
		// (under the actor, not the gun: the left-hand gun is mirrored, which a world rotation under it cannot undo)
		if (AmmoPlate[H] < 0)
		{
			AmmoPlate[H] = MakeTextPlate(GetRootComponent(), 2.2f);
			// stencil 3: the ink line round the gun stops at the count (user 2026-09-28: it drew over the numbers)
			PlatePlanes[AmmoPlate[H]]->SetRenderCustomDepth(true);
			PlatePlanes[AmmoPlate[H]]->SetCustomDepthStencilValue(3);
		}
		PlateHolders[AmmoPlate[H]]->SetWorldLocationAndRotation(Above, FRotationMatrix::MakeFromXZ(Camera->GetComponentLocation() - Above, Camera->GetUpVector()).Rotator());
		PlateHolders[AmmoPlate[H]]->SetWorldScale3D(FVector::OneVector);
		SetTextPlate(AmmoPlate[H], FString::Printf(TEXT("%d | %d"), Mag[H], Reserve));
		PlatePlanes[AmmoPlate[H]]->SetVisibility(!bPutAway && !bMenuPistol && !AimingDownSights(H));   // (out of the sight picture)
	}
}

bool AOutlawsPlayer::AimingDownSights(int32 Hand) const
{
	if (!bVR || Equipped[Hand] < 0 || !WeaponMesh[Hand] || GWeapons[Equipped[Hand]].Throw > 0) return false;
	if (bShouldered[Hand] || bScoped[Hand]) return true;
	const FTransform X = WeaponMesh[Hand]->GetComponentTransform();
	const FVector Gun = X.GetLocation();
	const FVector Bore = X.TransformVectorNoScale(FVector::ForwardVector).GetSafeNormal();
	const FVector Eye = Camera->GetComponentLocation();
	const FVector ToEye = Eye - Gun;
	if (ToEye.Size() > 55.0 || FVector::DotProduct(Bore, Camera->GetForwardVector()) < 0.95) return false;
	const FVector Off = ToEye - Bore * FVector::DotProduct(ToEye, Bore);    // the eye's distance from the bore's line
	return Off.Size() < 10.0;
}

// a hand's blow (a fist, the knife, a tool, carried gear, a held bad guy) lands only where the eyes could see it land: no
// wall, floor or door of the level between them and the spot (user 2026-10-02: the lantern and the plate hit through
// walls; a hand reaches through a wall in VR, the body does not)
static bool MeleeInSight(const AActor* Self, const AOutlawsLevelActor* L, const FVector& Eye, const FVector& At)
{
	FHitResult Hit;
	FCollisionObjectQueryParams Obj;
	Obj.AddObjectTypesToQuery(ECC_WorldStatic);
	Obj.AddObjectTypesToQuery(ECC_WorldDynamic);
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsMeleeSight), false, Self);
	return !(Self->GetWorld()->LineTraceSingleByObjectType(Hit, Eye, At, Obj, Q) && Hit.GetActor() == L);
}

static bool MeleeTouches(const AOutlawsEnemy* E, const FVector& At, double Reach);

void AOutlawsPlayer::UpdateThrowable(int32 Hand, float Trigger, float Dt)
{
	// dynamite: pull the trigger to light the fuse (4.5 s, pdynam LIFE), let it go to throw with the
	// hand's own swing; knife: squeeze and let go to throw, or swing it fast into a bad guy to jab
	AOutlawsLevelActor* L = Level.Get();
	const FWeaponDef& W = GWeapons[Equipped[Hand]];
	UProceduralMeshComponent* Gun = WeaponMesh[Hand];
	if (!L || !Gun)
	{
		return;
	}
	const FVector Tip = Gun->GetComponentTransform().TransformPosition(W.Muzzle);
	if (W.Throw == 1) UpdateFuseLook(Hand);
	if (Mag[Hand] <= 0)
	{
		// the next one from the pouch (no arc until it is in hand)
		ShowThrowArc(Hand, false);
		ThrowRestock[Hand] -= Dt;
		if (ThrowRestock[Hand] <= 0.f && (bUnlimitedAmmo || L->TakeCount(W.AmmoItem, 1)))
		{
			Mag[Hand] = 1;
		}
		return;
	}
	// the knife's arc shows only while the trigger is squeezed to throw, so it stays out of the way in a fight (user 2026-09-28)
	// (BUG-ARCSTUCK: not with the wheel open, as the bottle's and a held man's); the dynamite's below, once the fuse is lit
	if (W.Throw != 1) ShowThrowArc(Hand, !bWheelOpen && (bKnifeReady[Hand] || Trigger > 0.3f));
	if (W.Throw == 1)
	{
		float WetZ = 0.f;
		if (!bFuseLit[Hand] && Trigger > 0.7f && bTriggerArmed[Hand] && L->WaterAt(Tip, WetZ))
		{
			// no lighting a fuse under water (user 2026-09-29); one lit above it burns on when it goes in
			bTriggerArmed[Hand] = false;
			UE_LOG(LogOutlaws, Log, TEXT("OLRT dynamite: no lighting it under water"));
		}
		if (!bFuseLit[Hand] && Trigger > 0.7f && bTriggerArmed[Hand])
		{
			bTriggerArmed[Hand] = false;
			bFuseLit[Hand] = true;
			Fuse[Hand] = 4.5f;
			FuseSound[Hand] = L->PlayLoopAt(TEXT("fuse"), Tip);
			Pulse(Hand, 0.3f);
		}
		// as the knife's: the arc only while the trigger holds the lit fuse, from lighting it to the throw, never on a stick
		// just held (BUG-TNTARCTRIGGER, user 2026-10-15; was all the while one was in hand, user 2026-09-27)
		ShowThrowArc(Hand, !bWheelOpen && bFuseLit[Hand] && Trigger > 0.3f);
		// the grip closed and opened on an unlit stick (user 2026-09-29): let go unlit, flung along the arc with a swing
		// of the hand, else dropped where the hand is (not when the grip took hold of something, a door or a bad guy)
		{
			const float GripNow = Axis(Hand == 0 ? TEXT("OLVR_LeftGrip") : TEXT("OLVR_RightGrip"));
			if (GripNow > 0.6f && !bFuseLit[Hand] && !bTntGrip[Hand])
			{
				bTntGrip[Hand] = true;
			}
			else if (bTntGrip[Hand] && GripNow < 0.35f)
			{
				bTntGrip[Hand] = false;
				const bool bUsed = bGripUsed[Hand];
				bGripUsed[Hand] = false;
				if (!bFuseLit[Hand] && !bUsed)
				{
					ThrowHeld(Hand, true, HandVelocity[Hand].Size() < 150.f);     // (the hand against the body: walking is no swing)
					return;
				}
			}
		}
		if (bFuseLit[Hand])
		{
			Fuse[Hand] -= Dt;
			if (FuseSound[Hand].IsValid()) FuseSound[Hand]->SetWorldLocation(Tip);
			if (Fuse[Hand] <= 0.f)
			{
				// held too long
				ShowThrowArc(Hand, false);
				bFuseLit[Hand] = false;
				if (FuseSound[Hand].IsValid()) FuseSound[Hand]->Stop();
				Mag[Hand] = 0;
				ThrowRestock[Hand] = 0.6f;
				L->Explode(Tip);
				// in the hand there is no surviving it: the blast's own falloff and the armour do not apply (user 2026-09-28)
				L->SetHurtSource(Tip);
				L->HurtCause = uint8(EOutlawsHurtCause::OwnBlast);      // (REQ-KILLFEED)
				L->DamagePlayer(100.f, 1.f, true);
			}
			else if (Trigger < 0.3f)
			{
				ThrowHeld(Hand);
			}
		}
		return;
	}
	// knife
	if (Trigger > 0.7f && bTriggerArmed[Hand])
	{
		bTriggerArmed[Hand] = false;
		bKnifeReady[Hand] = true;
	}
	if (bKnifeReady[Hand] && Trigger < 0.3f)
	{
		bKnifeReady[Hand] = false;
		ThrowHeld(Hand);
		return;
	}
	KnifeCool[Hand] = FMath::Max(0.f, KnifeCool[Hand] - Dt);
	if (HandVelocity[Hand].Size() > OutlawsFeel::HitSpeedHard) L->ShatterAlong(Tip - HandVelocity[Hand].GetSafeNormal() * 30.0, Tip);   // (glass: no grip needed)
	// (user 2026-10-04, REQ-KNIFESTAB: no grip needed to stab; was the grip held, user 2026-09-29) the fist's own swing test
	if (KnifeCool[Hand] <= 0.f && HandVelocity[Hand].Size() > OutlawsFeel::HitSpeedHard)
	{
		// a stab into dynamite sets it off; a swing through a window or a bottle breaks it (user 2026-09-28)
		if (MeleeDynamite(Tip))
		{
			KnifeCool[Hand] = 0.3f;
			return;
		}
		FVector SwingEnd = Tip;
		if (L->ShootAlong(Tip - HandVelocity[Hand].GetSafeNormal() * 30.0, SwingEnd))
		{
			L->PlaySoundAt(TEXT("jab"), SwingEnd);
			PulseImpact(Hand, float(HandVelocity[Hand].Size()));
			KnifeCool[Hand] = 0.3f;
			return;
		}
		if (!MeleeInSight(this, L, Camera->GetComponentLocation(), Tip)) return;
		if (UOutlawsCoop::StrikeAtPlayers(L, Tip, 12.f, W.Damage * FMath::FRandRange(0.9f, 1.1f) * L->PlayerDamageScale(), TEXT("a knife's jab")))
		{
			L->PlaySoundAt(TEXT("jab"), Tip);       // (M1's gaps: another player in a match)
			Pulse(Hand, 0.8f);
			KnifeCool[Hand] = 0.4f;
			return;
		}
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
		{
			AOutlawsEnemy* E = Wk.Get();
			if (!E || E->IsDead() || E->IsDormant()) continue;
			const FVector C = E->GetActorLocation();
			const float Half = E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			// the blade's tip in him, or the fist that holds it landing as a punch would (MeleeTouches, a fist's 22 cm)
			const FVector Fist = GripOf(Hand)->GetComponentLocation() + HandVelocity[Hand].GetSafeNormal() * 5.0;
			if ((FVector::Dist2D(C, Tip) < E->GetCapsuleComponent()->GetScaledCapsuleRadius() + 12.f && FMath::Abs(Tip.Z - C.Z) < Half) || MeleeTouches(E, Tip, 22.0)
				|| MeleeTouches(E, Fist, 22.0))
			{
				// the jab (FIRE 2: range 8 u, 4 damage)
				AOutlawsEnemy::GNextHitMelee = true;          // (HM1: a knife's stab is melee)
				E->TakeHit(W.Damage * FMath::FRandRange(0.9f, 1.1f) * L->PlayerDamageScale(), Tip, HandWorldVel[Hand].GetSafeNormal(), Tip - HandWorldVel[Hand].GetSafeNormal() * 30.f);
				UE_LOG(LogOutlaws, Log, TEXT("OLRT %s knife stab into %s at %.0f cm/s (grip %.2f): %.1f"), Hand == 0 ? TEXT("left") : TEXT("right"), *E->GetOutlawsName(), HandVelocity[Hand].Size(),
					Axis(Hand == 0 ? TEXT("OLVR_LeftGrip") : TEXT("OLVR_RightGrip")), W.Damage);
				L->PlaySoundAt(TEXT("jab"), Tip);
				Pulse(Hand, 0.8f);
				KnifeCool[Hand] = 0.4f;
				break;
			}
		}
	}
}

// how far a point or a stretch (A to B) comes from a bad guy's body: his capsule's upright middle line, less its
// radius (under 0 inside him); OutAt the nearest point of A..B
static double BodyGap(const AOutlawsEnemy* E, const FVector& A, const FVector& B, FVector* OutAt = nullptr)
{
	const UCapsuleComponent* Cap = E->GetCapsuleComponent();
	const FVector C = E->GetActorLocation();
	const FVector Z(0.0, 0.0, FMath::Max(0.f, Cap->GetScaledCapsuleHalfHeight() - Cap->GetScaledCapsuleRadius()));
	FVector P, Q;
	FMath::SegmentDistToSegmentSafe(A, B, C - Z, C + Z, P, Q);
	if (OutAt) *OutAt = P;
	return FVector::Dist(P, Q) - Cap->GetScaledCapsuleRadius();
}

// a blow's point on a bad guy, as the fist lands (user 2026-09-29: what is drawn is what is hit): inside his picture (its
// quad, turned to the eye, a little inside its edges) and within Reach of it; a held one on his body (his picture is
// mirrored in the left hand and stopped on the stun's frame)
static bool MeleeTouches(const AOutlawsEnemy* E, const FVector& At, double Reach)
{
	const AOutlawsSpriteActor* Spr = E->GetSprite();
	if (E->bHeldByPlayer) return BodyGap(E, At, At) <= 10.0;
	if (Spr)
	{
		const FVector Sc = Spr->GetActorScale3D();                 // (1, width cm, height cm), pivot at the bottom middle
		const FVector Rel = At - Spr->GetActorLocation();
		const double Across = FMath::Abs(FVector::DotProduct(Rel, Spr->GetActorRightVector()));
		const double Up = FVector::DotProduct(Rel, Spr->GetActorUpVector());
		const double Depth = FMath::Abs(FVector::DotProduct(Rel, Spr->GetActorForwardVector()));
		return Across <= FMath::Abs(Sc.Y) * 0.42 && Up >= 0.0 && Up <= Sc.Z * 0.97 && Depth <= Reach;
	}
	const FVector C = E->GetActorLocation();
	const float Half = E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	return FVector::Dist2D(C, At) <= E->GetCapsuleComponent()->GetScaledCapsuleRadius() + 15.f && FMath::Abs(At.Z - C.Z) <= Half;
}

// (BUG-SHOVELWHACK) a shovel's or a crowbar's blow lands from this far out of the fist to the tool's end (the crowbar's
// outer half, near enough, as before; the shovel's shaft too)
static constexpr double ToolStrikeFromCm = 25.0;

// (BUG-SHOVELWHACK) where a tool's striking length A..B lands on a bad guy: within 6 cm of his body's capsule (as before),
// else in his picture as drawn, within a fist's 22 cm of it, a point every 5 cm from the end in; a held one on his body
// only (his picture is in the other hand).  OutAt the point
static bool ToolMeets(const AOutlawsEnemy* E, const FVector& A, const FVector& B, FVector& OutAt)
{
	if (BodyGap(E, A, B, &OutAt) <= 6.0) return true;
	const AOutlawsSpriteActor* Spr = E->GetSprite();
	if (!Spr || E->bHeldByPlayer) return false;
	const int32 N = FMath::Clamp(FMath::CeilToInt(FVector::Dist(A, B) / 5.0), 1, 40);
	for (int32 i = 0; i <= N; ++i)
	{
		const FVector P = FMath::Lerp(B, A, double(i) / N);
		if (Spr->DrawnTouches(P, 22.0))
		{
			OutAt = P;
			return true;
		}
	}
	return false;
}

void AOutlawsPlayer::UpdatePunches(float Dt)
{
	OL_PERF_SCOPE("Player: punches");
	AOutlawsLevelActor* L = Level.Get();
	if (!L || !bVR || bMenuOpen || bWheelOpen || bDeadView) return;
	for (int32 H = 0; H < 2; ++H)
	{
		PunchCool[H] = FMath::Max(0.f, PunchCool[H] - Dt);
		const int32 Wi = Equipped[H];
		const bool bKnife = Wi >= 0 && GWeapons[Wi].Throw == 2;
		const bool bMounted = Wi >= 0 && GWeapons[Wi].bAuto;
		// a bottle, a prop, the plate or the map swung fast through a window breaks it, along the thing's own length (user
		// 2026-10-01: held things did not)
		{
			UPrimitiveComponent* Thing = HeldBottle[H] ? static_cast<UPrimitiveComponent*>(HeldBottle[H].Get())
				: (IsGear(HeldTool[H]) && GearMesh[H]) ? static_cast<UPrimitiveComponent*>(GearMesh[H].Get()) : nullptr;
			// (the lantern: its body, on the swinging hang)
			if (!Thing && HeldTool[H] == TEXT("ilantern") && LanternHang[H])
			{
				TArray<USceneComponent*> Parts;
				LanternHang[H]->GetChildrenComponents(false, Parts);
				for (USceneComponent* C : Parts) if (UProceduralMeshComponent* Pm = Cast<UProceduralMeshComponent>(C)) { Thing = Pm; break; }
			}
			if (Thing && Thing->IsVisible() && HandVelocity[H].Size() > OutlawsFeel::HitSpeedHard)
			{
				const FVector Vd = HandVelocity[H].GetSafeNormal();
				const FBox Bb = Thing->CalcBounds(Thing->GetComponentTransform()).GetBox();
				const double R = Bb.GetExtent().GetMax();
				L->ShatterAlong(Bb.GetCenter() - Vd * (R + 12.0), Bb.GetCenter() + Vd * R);
			}
		}
		// carried gear swung into a bad guy hurts him, held in the other hand or not (user 2026-10-01: fast hands hit):
		// harder the faster the swing; the lantern a little over half of a blow, the map a shove only (it is paper).  (The
		// plate is worn now, REQ-CHESTPLATE: it is never swung)
		if (IsGear(HeldTool[H]) && PunchCool[H] <= 0.f && HandVelocity[H].Size() > OutlawsFeel::HitSpeedHard)
		{
			const bool bLantern = HeldTool[H] == TEXT("ilantern");
			// the thing's own body only (the lantern's case on its hang, not its glow or anything else hung there), and the
			// blow lands where its leading face is, as a fist's knuckles do (user 2026-10-02: the whole box round the lantern
			// and the bad guy's whole box met, through walls, from a long way off)
			UPrimitiveComponent* Body3D = nullptr;
			if (bLantern && LanternHang[H])
			{
				TArray<USceneComponent*> Parts;
				LanternHang[H]->GetChildrenComponents(false, Parts);
				for (USceneComponent* C : Parts) if (UProceduralMeshComponent* Pm = Cast<UProceduralMeshComponent>(C); Pm && Pm->IsVisible()) { Body3D = Pm; break; }
			}
			else if (GearMesh[H] && GearMesh[H]->IsVisible())
			{
				Body3D = GearMesh[H];
			}
			if (Body3D)
			{
				const float Speed = float(HandVelocity[H].Size());
				const FVector Vd = HandVelocity[H].GetSafeNormal();
				const FBox Gb = Body3D->CalcBounds(Body3D->GetComponentTransform()).GetBox();
				const FVector At = Gb.GetClosestPointTo(Gb.GetCenter() + Vd * 1000.0);
				const float Share = bLantern ? 0.6f : 0.f;
				for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
				{
					AOutlawsEnemy* E = Wk.Get();
					if (!E || E->IsDead() || E->IsDormant() || E == GrabbedEnemy[H].Get()) continue;
					if (!MeleeTouches(E, At, 22.0) || !MeleeInSight(this, L, Camera->GetComponentLocation(), At)) continue;
					if (Share > 0.f)
					{
						const float Damage = Share * FMath::FRandRange(0.9f, 1.1f) * FMath::Clamp(Speed / 400.f, 0.6f, 1.5f);
						E->TakeHit(Damage * L->PlayerDamageScale(), At, Vd, At - Vd * 30.f, false, 1.2f);
						L->PlaySoundAt(TEXT("connect"), At);
						UE_LOG(LogOutlaws, Log, TEXT("OLRT %s swung into %s at %.0f cm/s: %.2f"), *HeldTool[H], *E->GetName(), Speed, Damage);
					}
					else if (!E->bHeldByPlayer)
					{
						E->Knock(Vd, 0.6f, false);                   // (the map: a shove)
						UE_LOG(LogOutlaws, Log, TEXT("OLRT the map shoves %s"), *E->GetName());
					}
					PulseImpact(H, Speed);
					PunchCool[H] = 0.5f;
					break;
				}
			}
		}
		// (a hand on the shovel's shaft, or holding the shovel or the crowbar, swings the tool, not a fist: UpdateTools)
		// (the hand holding a bad guy does no harm with it: user 2026-09-29)
		if (PunchCool[H] > 0.f || bKnife || bMounted || GrabHand == H || HeldItem[H].IsValid() || HeldBottle[H] || bWheelHeld[H] || bToolSupport[H]
			|| GrabbedEnemy[H].IsValid() || HeldTool[H] == TEXT("ishovel") || HeldTool[H] == TEXT("icrowbar") || IsGear(HeldTool[H])) continue;
		const FVector V = HandVelocity[H];     // the arm's own swing (walking into someone is not a punch)
		if (V.Size() < OutlawsFeel::HitSpeedHard) continue;
		// a window gives to any fast hand, grip or not (user 2026-09-29); everything else only to a closed fist: the grip
		// held on that hand (user 2026-09-29, now that the grip is free)
		L->ShatterAlong(GripOf(H)->GetComponentLocation() - V.GetSafeNormal() * 12.0, GripOf(H)->GetComponentLocation() + V.GetSafeNormal() * 8.0);
		if (Axis(H == 0 ? TEXT("OLVR_LeftGrip") : TEXT("OLVR_RightGrip")) < 0.5f) continue;
		// the knuckles: the fist itself is the only thing that lands, a gun in it or not (user 2026-09-29: no pistol
		// whipping with the gun's length); a little ahead of the grip along the push
		const FVector Fist = GripOf(H)->GetComponentLocation() + V.GetSafeNormal() * 5.0;
		// a punch into dynamite sets it off; through a window or a bottle, it breaks (user 2026-09-28)
		if (MeleeDynamite(Fist))
		{
			PunchCool[H] = 0.45f;
			continue;
		}
		FVector SwingEnd = Fist;
		if (L->ShootAlong(GripOf(H)->GetComponentLocation() - V.GetSafeNormal() * 12.0, SwingEnd))
		{
			L->PlaySoundAt(TEXT("connect"), SwingEnd);
			PulseImpact(H, float(V.Size()));
			PunchCool[H] = 0.45f;
			continue;
		}
		// (a hand pushed through a wall hits nobody behind it)
		const bool bFistSeen = MeleeInSight(this, L, Camera->GetComponentLocation(), Fist);
		if (bFistSeen && UOutlawsCoop::StrikeAtPlayers(L, Fist, 22.f, 0.8f * FMath::FRandRange(0.9f, 1.1f) * L->PlayerDamageScale(), TEXT("a fist")))
		{
			L->PlaySoundAt(TEXT("connect"), Fist);  // (M1's gaps: another player in a match)
			PulseImpact(H, float(V.Size()));
			PunchCool[H] = 0.45f;
			continue;
		}
		// the one held in the other hand (user 2026-10-03: "determined by where I'm holding the controller, and a sensible
		// radius around that, extending a bit lower so I can punch them in the guts"): a fist within 25 cm of the line from
		// the holding grip down 50 cm lands, seen or not (he is at arm's length, in front of the face)
		const int32 O = 1 - H;
		AOutlawsEnemy* Held = GrabbedEnemy[O].Get();
		const FVector HoldAt = GripOf(O)->GetComponentLocation();
		const bool bHeldHit = Held && !Held->IsDead()
			&& FMath::PointDistToSegment(Fist, HoldAt + FVector(0.0, 0.0, 8.0), HoldAt - FVector(0.0, 0.0, 50.0)) < 25.0;
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
		{
			AOutlawsEnemy* E = Wk.Get();
			if (!E || E->IsDead() || E->IsDormant()) continue;
			if (E == Held ? !bHeldHit : (!bFistSeen || !MeleeTouches(E, Fist, 22.0))) continue;
			// what is drawn is what is hit (user 2026-09-29); held in the other hand he can be punched, as the smash hurts
			// him (user 2026-10-01)
			const bool bBare = true;                                  // (a fist is a fist, a gun in it or not)
			const float Damage = 0.8f * FMath::FRandRange(0.9f, 1.1f);
			const FVector Dir = V.GetSafeNormal();
			AOutlawsEnemy::GNextHitMelee = true;              // (HM1: a fist is melee)
			if (E == Held) ++TutNotes.HeldPunches;            // (REQ-TUTORIAL)
			E->TakeHit(Damage * L->PlayerDamageScale(), Fist, Dir, Fist - Dir * 30.f);
			L->PlaySoundAt(TEXT("connect"), Fist);         // the fist's FIRE_SOUND
			PulseImpact(H, float(V.Size()));
			PunchCool[H] = 0.45f;                            // the punch's 0.5 s cycle
			UE_LOG(LogOutlaws, Log, TEXT("OLRT %s punch (%s%s): %.2f"), H == 0 ? TEXT("left") : TEXT("right"), bBare ? TEXT("fist") : TEXT("with a weapon, a quarter"),
				E->bHeldByPlayer ? TEXT(", held") : TEXT(""), Damage);
			break;
		}
		if (PunchCool[H] > 0.f) continue;
		// a punch into a wall (or a floor, or a door): the thump, a jolt in the hand and cracks where it landed
		// (user 2026-09-28); only a fist driven INTO the surface, not one brushing along it
		FHitResult WH;
		FCollisionObjectQueryParams Obj;
		Obj.AddObjectTypesToQuery(ECC_WorldStatic);
		Obj.AddObjectTypesToQuery(ECC_WorldDynamic);
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsPunchWall), false, this);
		const FVector Along = V.GetSafeNormal();
		if (GetWorld()->LineTraceSingleByObjectType(WH, GripOf(H)->GetComponentLocation() - Along * 12.0, Fist + Along * 3.0, Obj, Q)
			&& WH.GetActor() == L && FVector::DotProduct(V, FVector(WH.ImpactNormal)) < -180.0)
		{
			L->PlaySoundAt(TEXT("connect"), WH.ImpactPoint);
			L->AddPunchMark(WH);
			PulseImpact(H, float(V.Size()));
			PunchCool[H] = 0.45f;
			UE_LOG(LogOutlaws, Log, TEXT("OLRT %s punch into the wall at %s"), H == 0 ? TEXT("left") : TEXT("right"), *WH.ImpactPoint.ToString());
		}
	}
}

void AOutlawsPlayer::EnsureScopeRig(AOutlawsLevelActor* L)
{
	// the circle subtends 52 degrees (radius 0.07 of a 7 m sheet 1 m out) and shows 26: twice the size
	constexpr float CircleRadius = 0.07f, SheetCm = 700.f;
	const float ScopeFov = 2.f * FMath::RadiansToDegrees(FMath::Atan(CircleRadius * SheetCm / 100.f)) * 0.5f;
	if (!ScopeCapture)
	{
		ScopeTarget = NewObject<UTextureRenderTarget2D>(this);
		ScopeTarget->RenderTargetFormat = RTF_RGBA16f;
		ScopeTarget->ClearColor = FLinearColor::Black;
		ScopeTarget->InitAutoFormat(1024, 1024);
		ScopeCapture = NewObject<USceneCaptureComponent2D>(this);
		ScopeCapture->SetupAttachment(RootComponent);
		ScopeCapture->SetUsingAbsoluteLocation(true);
		ScopeCapture->SetUsingAbsoluteRotation(true);
		ScopeCapture->TextureTarget = ScopeTarget;
		ScopeCapture->CaptureSource = ESceneCaptureSource::SCS_SceneColorHDR;
		ScopeCapture->FOVAngle = ScopeFov;
		ScopeCapture->bCaptureOnMovement = false;
		ScopeCapture->bCaptureEveryFrame = false;
		ScopeCapture->RegisterComponent();
		ScopeView = NewObject<UStaticMeshComponent>(this);
		ScopeView->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane")));
		ScopeView->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		ScopeView->SetCastShadow(false);
		ScopeView->SetupAttachment(Camera);
		ScopeView->RegisterComponent();
		// the plane (facing +Z) turned to face the eyes, its U to the right and V down, like the picture
		ScopeView->SetRelativeLocationAndRotation(FVector(100.0, 0.0, 0.0), FRotationMatrix::MakeFromXY(FVector(0.0, 1.0, 0.0), FVector(0.0, 0.0, -1.0)).Rotator());
		ScopeView->SetRelativeScale3D(FVector(SheetCm / 100.f, SheetCm / 100.f, 1.f));
		ScopeView->SetTranslucentSortPriority(50);
		if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_Scope.M_OLR_Scope")))
		{
			UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, this);
			Mid->SetTextureParameterValue(TEXT("Tex"), ScopeTarget);
			Mid->SetScalarParameterValue(TEXT("Radius"), CircleRadius);
			// under the blackout, the game's own scope: the rifle sprite's scope frame (ijrifle.nwx frame 6, the
			// SCOPE_REST_CHOR 20; the remaster's cell 6, 1544 x 1292), its ring's opening stretched over the circle
			// with the crosshair in the middle (user 2026-09-28). The opening, in that cell's pixels: the crosshair
			// at 761.5, 321.5, the glass 199.5 across and 195 down from it
			FIntPoint RetSize;
			if (UTexture2D* Ret = L ? L->SpxCell(TEXT("ijrifle.spx"), 6, RetSize) : nullptr)
			{
				Mid->SetTextureParameterValue(TEXT("Reticle"), Ret);
				Mid->SetVectorParameterValue(TEXT("ReticleRect"), FLinearColor(761.5f / 1544.f, 321.5f / 1292.f, 199.5f / 1544.f, 195.f / 1292.f));
				Mid->SetScalarParameterValue(TEXT("ReticleOn"), 1.f);
			}
			ScopeView->SetMaterial(0, Mid);
		}
		ScopeView->SetVisibility(false);
		// one picture now, behind the load's dark: the target and the capture pass made ready on the render thread
		const double T0 = FPlatformTime::Seconds();
		ScopeCapture->HideActorComponents(this);
		ScopeCapture->SetWorldLocationAndRotation(Camera->GetComponentLocation(), Camera->GetComponentRotation());
		ScopeCapture->CaptureScene();
		UE_LOG(LogOutlaws, Log, TEXT("OLRT prebuild scope rig and one capture: %.1f ms"), (FPlatformTime::Seconds() - T0) * 1000.0);
	}
}

#if WITH_OUTLAWS_TESTS
void AOutlawsPlayer::CheatScopeTestRaise()
{
	if (AOutlawsLevelActor* L = Level.Get()) L->AddCount(TEXT("irifle"), 1);      // (the rifle alone: the scope is the cheat's to give)
	Equip(1, 1);
	bScopeTestNeedsItem = true;
	bScopeTestForce = true;
}

bool AOutlawsPlayer::CheatScopeTestUp() const
{
	return bScoped[1] && ScopeView && ScopeView->IsVisible();
}
#endif // WITH_OUTLAWS_TESTS

void AOutlawsPlayer::UpdateScope()
{
	OL_PERF_SCOPE("Player: scope");
	// which hand looks through the scope: the rifle, the scope carried, the rifle's grip up at that
	// side's eye with the barrel along the view (on under 18 cm, off past 26)
	constexpr int32 RifleIndex = 1;
	AOutlawsLevelActor* L = Level.Get();
	const bool bCarried = L && L->GetCount(TEXT("iscope")) > 0;
	const bool bCan = bCarried && !bMenuOpen && !bWheelOpen && !bDeadView;
	int32 Look = -1;
	FVector LookDir = Camera->GetForwardVector();
	for (int32 H = 0; H < 2; ++H)
	{
		UProceduralMeshComponent* Gun = WeaponMesh[H];
		const bool bRifle = Equipped[H] == RifleIndex && Gun && Gun->IsVisible();
		// the scope on the rifle, every frame for both hands (user 2026-09-30: it stayed on a hand the rifle had left, on
		// both hands at once: a gun hidden leaves what is fastened to it showing, and the gun parts' own check runs only
		// for a hand holding a gun)
		if (ScopeMesh[H]) ScopeMesh[H]->SetVisibility(bRifle && bCarried && ScopeMesh[H]->GetNumSections() > 0);
		if (bScopeTestForce && Equipped[H] == RifleIndex && (!bScopeTestNeedsItem || bCarried))
		{
			bScoped[H] = true;
		}
		else if (!bCan || !bRifle || !bVR)
		{
			bScoped[H] = false;
			if (bRifle && bVR && !bCarried && StockBlend[H] > 0.6f && !bScopeNoItemLogged)
			{
				bScopeNoItemLogged = true;
				UE_LOG(LogOutlaws, Log, TEXT("OLRT scope: the rifle is at the eye but no scope (iscope) is carried"));
			}
		}
		else
		{
			const FVector Bore = Gun->GetComponentTransform().TransformVector(GWeapons[RifleIndex].BoreFix.Quaternion().Inverse().RotateVector(FVector::ForwardVector)).GetSafeNormal();
			const FVector Eye = Camera->GetComponentLocation() + Camera->GetRightVector() * (H == 0 ? -3.2 : 3.2);
			const double D = FVector::Dist(FrameOf(H)->GetComponentLocation(), Eye);
			// up at the eye as the virtual stock takes it (the sights on the eye), or the grip close to it
			const bool bUp = StockBlend[H] > (bScoped[H] ? 0.4f : 0.7f) || (bScoped[H] ? D < 36.0 : D < 28.0);
			bScoped[H] = FVector::DotProduct(Bore, Camera->GetForwardVector()) > 0.7 && bUp;
			if (bScoped[H]) LookDir = Bore;
		}
		if (bScoped[H]) Look = H;
	}
	if (Look < 0)
	{
		if (ScopeView && ScopeView->IsVisible())
		{
			ScopeView->SetVisibility(false);
			ScopeCapture->bCaptureEveryFrame = false;
			for (const TWeakObjectPtr<UPrimitiveComponent>& P : ScopeUninked) if (P.IsValid()) P->SetRenderCustomDepth(true);
			ScopeUninked.Reset();
			UE_LOG(LogOutlaws, Log, TEXT("OLRT scope down"));
		}
		bScopeAimSet = false;
		// (BUG-STUTTER2) made with the level, not on the first raise
		if (!ScopeCapture && L && !L->IsFrontEnd() && !L->IsRebuildPending() && !OLFlags::Param(TEXT("OLNoPrebuild"))) EnsureScopeRig(L);
		return;
	}
	EnsureScopeRig(L);
	if (!ScopeView) return;
	if (!ScopeView->IsVisible())
	{
		// the player's own body, hands and guns stay out of the picture; the view keeps the camera's look
		ScopeCapture->HiddenComponents.Reset();
		ScopeCapture->HideActorComponents(this);
		ScopeCapture->PostProcessSettings = Camera->PostProcessSettings;
		ScopeCapture->bCaptureEveryFrame = true;
		ScopeView->SetVisibility(true);
		// the held gear's ink (custom stencil 1) off while the scope is up: the outline pass drew it over the picture
		TInlineComponentArray<UPrimitiveComponent*> Own(this);
		for (UPrimitiveComponent* P : Own)
		{
			if (P && P->bRenderCustomDepth && P->CustomDepthStencilValue == 1)
			{
				P->SetRenderCustomDepth(false);
				ScopeUninked.Add(P);
			}
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLRT scope up (%s hand), %.1f degree view"), Look == 0 ? TEXT("left") : TEXT("right"), ScopeCapture->FOVAngle);
	}
	// only the rifle hand's eye looks through it (user 2026-09-27); the other eye keeps the plain view
	if (UMaterialInstanceDynamic* Mid = Cast<UMaterialInstanceDynamic>(ScopeView->GetMaterial(0)))
	{
		Mid->SetScalarParameterValue(TEXT("Eye"), bVR ? float(Look) : -1.f);
	}
	const FVector Eye = Camera->GetComponentLocation() + Camera->GetRightVector() * (Look == 0 ? -3.2 : 3.2);
	// steadied (user 2026-09-28: a shaky hand made the picture jitter): a small wobble of where it looks is followed
	// slowly, a real swing of the rifle (5+ degrees off) almost at once.  Only the direction is steadied: the picture's
	// roll is the head's own every frame (tilting the head, a steadied roll swung the scope's horizon behind the world's)
	const FVector WantDir = LookDir.GetSafeNormal();
	if (!bScopeAimSet)
	{
		ScopeAimDir = WantDir;
		bScopeAimSet = true;
	}
	else
	{
		const float Off = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(float(FVector::DotProduct(ScopeAimDir, WantDir)), -1.f, 1.f)));
		const float Rate = FMath::Lerp(5.f, 40.f, FMath::Clamp(Off / 5.f, 0.f, 1.f));
		ScopeAimDir = FMath::Lerp(ScopeAimDir, WantDir, 1.f - FMath::Exp(-Rate * GetWorld()->GetDeltaSeconds())).GetSafeNormal();
	}
	ScopeAim = FRotationMatrix::MakeFromXZ(ScopeAimDir, Camera->GetUpVector()).ToQuat();
	ScopeCapture->SetWorldLocationAndRotation(Eye, ScopeAim.Rotator());
	// the rifle itself along the steadied look (user 2026-10-01: the other eye saw every tremor of the wrist the scope's
	// picture leaves out): turned about the fist by the small difference, so the hand stays on its grip; a real swing is
	// followed at once as the picture's is.  (Placed from the hand afresh every frame before this: down, it is the hand's)
	if (UProceduralMeshComponent* Gun = WeaponMesh[Look])
	{
		const FVector Bore = Gun->GetComponentTransform().TransformVector(GWeapons[RifleIndex].BoreFix.Quaternion().Inverse().RotateVector(FVector::ForwardVector)).GetSafeNormal();
		const FQuat Delta = FQuat::FindBetweenNormals(Bore, ScopeAimDir);
		const FVector Pivot = GripOf(Look)->GetComponentLocation();
		Gun->SetWorldLocationAndRotation(Pivot + Delta.RotateVector(Gun->GetComponentLocation() - Pivot), Delta * Gun->GetComponentQuat());
	}
}

void AOutlawsPlayer::EquipTool(int32 Hand, const FString& Item)
{
	if (Item.Equals(TEXT("ibplate"), ESearchCase::IgnoreCase))
	{
		// (REQ-CHESTPLATE) the plate is worn from its pickup, never held: the hand is left as it is
		UE_LOG(LogOutlaws, Log, TEXT("OLRT the boiler plate is worn, not held: the %s hand keeps what it has"), Hand == 0 ? TEXT("left") : TEXT("right"));
		return;
	}
	Equip(Hand, -1);                         // the gun in that hand is put away
	const int32 O = 1 - Hand;
	if (HeldTool[O].Equals(Item, ESearchCase::IgnoreCase)) DropTool(O);
	HeldTool[Hand] = Item.ToLower();
	const bool bShovel = HeldTool[Hand] == TEXT("ishovel");
	if (!ToolRoot[Hand] || ToolBuilt[Hand] != HeldTool[Hand])
	{
		if (IsGear(HeldTool[Hand]) && !ToolRoot[Hand])
		{
			ToolRoot[Hand] = NewObject<USceneComponent>(this);
			ToolRoot[Hand]->SetupAttachment(FrameOf(Hand));
			ToolRoot[Hand]->RegisterComponent();
		}
		if (IsGear(HeldTool[Hand])) BuildGearModel(Hand);
		else BuildToolModel(Hand, bShovel);
	}
	ToolRoot[Hand]->SetRelativeTransform(ToolRest(Hand));
	ToolRoot[Hand]->SetVisibility(true, true);
	bToolTipValid[Hand] = false;
	if (IsGear(HeldTool[Hand]))
	{
		// the lantern or the map (OutlawsPlayerGear.cpp): no lock opens with it
		if (HeldTool[Hand] == TEXT("imap")) BuildMapPage(Hand);         // (the layer underfoot now)
		Pulse(Hand, 0.3f);
		UE_LOG(LogOutlaws, Log, TEXT("OLRT %s hand takes the %s"), Hand == 0 ? TEXT("left") : TEXT("right"), *HeldTool[Hand]);
		return;
	}
	const bool bPen = HeldTool[Hand] == TEXT("iink");
	if (AOutlawsLevelActor* L = Level.Get())
	{
		L->SetToolHeld(L->UseIdOf(Item), true);
		L->Notify(bPen ? TEXT("Pen: touch it to the guest book") : bShovel ? TEXT("Shovel: drive the spade into soft ground") : TEXT("Crowbar: jab it into a stuck door"), 2.5f);
	}
	Pulse(Hand, 0.3f);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT %s hand takes the %s"), Hand == 0 ? TEXT("left") : TEXT("right"), bPen ? TEXT("pen") : bShovel ? TEXT("shovel") : TEXT("crowbar"));
}

void AOutlawsPlayer::DropTool(int32 Hand)
{
	if (HeldTool[Hand].IsEmpty()) return;
	if (AOutlawsLevelActor* L = Level.Get()) L->SetToolHeld(L->UseIdOf(HeldTool[Hand]), false);
	HeldTool[Hand].Reset();
	if (ToolRoot[Hand]) ToolRoot[Hand]->SetVisibility(false, true);
}

void AOutlawsPlayer::BuildToolModel(int32 Hand, bool bShovel)
{
	// in the hand frame, +X along the pointing hand: gripped a little behind the middle, the business end
	// forward (a spade to drive down, a crowbar's chisel end to jab)
	if (ToolRoot[Hand])
	{
		TArray<USceneComponent*> Kids;
		ToolRoot[Hand]->GetChildrenComponents(true, Kids);       // (all of it: a lantern's parts hang on its pivot)
		for (USceneComponent* K : Kids) if (K) K->DestroyComponent();
	}
	else
	{
		ToolRoot[Hand] = NewObject<USceneComponent>(this);
		ToolRoot[Hand]->SetupAttachment(FrameOf(Hand));
		ToolRoot[Hand]->RegisterComponent();
	}
	const bool bPen = HeldTool[Hand] == TEXT("iink");
	ToolBuilt[Hand] = bPen ? TEXT("iink") : bShovel ? TEXT("ishovel") : TEXT("icrowbar");
	ToolTipX[Hand] = bPen ? 16.5 : bShovel ? 78.0 : 55.0;
	// the modelled tools (backlog 7, user 2026-10-01: shovel, crowbar and pen; item3d/weapons_extra.py builds them in this
	// frame, the ends where the boxes had them), else the sprite-built shovel (blade widened and dished), held by the end
	// of its shaft with the blade forward, the other hand free to take the shaft (user 2026-09-28); -OLGameTools keeps the
	// sprite shovel and the boxes, -OLVEGuns the boxes for all three
	const bool bGameTools = OLFlags::Param(TEXT("OLGameTools"));
	FString ModelName = bPen ? TEXT("GINK3D.obj") : bShovel ? TEXT("GSHOVEL3D.obj") : TEXT("GCROWBAR3D.obj");
	if (bGameTools || !FPaths::FileExists(FPaths::Combine(SpriteHeldDir(), ModelName))) ModelName = bShovel ? TEXT("GSHOVEL.obj") : FString();
	if (!ModelName.IsEmpty() && !OLFlags::Param(TEXT("OLVEGuns")))
	{
		const FString P = FPaths::Combine(SpriteHeldDir(), ModelName);
		TSharedPtr<OutlawsObj::FObjMesh>* Cached = GObjCache.Find(P);
		if (!Cached)
		{
			TSharedPtr<OutlawsObj::FObjMesh> Obj = MakeShared<OutlawsObj::FObjMesh>();
			if (!FPaths::FileExists(P) || !OutlawsObj::Load(P, 1.0, true, *Obj)) Obj.Reset();
			Cached = &GObjCache.Add(P, Obj);
		}
		UMaterialInterface* Base = HeldBase();
		if (Cached->IsValid() && Base)
		{
			UProceduralMeshComponent* C = NewObject<UProceduralMeshComponent>(this);
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->SetCastShadow(false);
			C->SetupAttachment(ToolRoot[Hand]);
			C->RegisterComponent();
			int32 Index = 0;
			for (const TPair<FString, FOutlawsMeshSection>& Kv : (*Cached)->Sections)
			{
				const FOutlawsMeshSection& S = Kv.Value;
				C->CreateMeshSection_LinearColor(Index, S.Positions, S.Triangles, S.Normals, S.UVs, S.Colors, TArray<FProcMeshTangent>(), false);
				UMaterialInterface* Mat = GunMaterials.FindRef(S.Texture);
				if (!Mat)
				{
					UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, this);
					if (UTexture2D* Tex = OutlawsObj::LoadTextureFile(S.Texture))
					{
						Mid->SetTextureParameterValue(TEXT("Tex"), Tex);
						KeepAlive.Add(Tex);
					}
					KeepAlive.Add(Mid);
					HeldMids.Add(Mid);
					GunMaterials.Add(S.Texture, Mid);
					Mat = Mid;
				}
				C->SetMaterial(Index, Mat);
				++Index;
			}
			KeepDrawnOverWorld(C);
			UE_LOG(LogOutlaws, Log, TEXT("OLRT held %s: %s"), *ToolBuilt[Hand], *ModelName);
			HeldLight = -1.f;                      // the new parts take the current sector light
			if (!bShovel) return;                  // (the crowbar's and the pen's ends stay where they were)
			ToolTipX[Hand] = 111.0;
			FString Text;
			TSharedPtr<FJsonObject> J;
			double Tip = 0.0;
			if (FFileHelper::LoadFileToString(Text, *FPaths::ChangeExtension(P, TEXT("json")))
				&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), J) && J.IsValid() && J->TryGetNumberField(TEXT("tip"), Tip))
			{
				ToolTipX[Hand] = Tip;
			}
			HeldLight = -1.f;                      // the new parts take the current sector light
			return;
		}
	}
	UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
	UStaticMesh* Cyl = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	auto Part = [&](UStaticMesh* Shape, const FVector& At, const FRotator& Rot, const FVector& Scale, UMaterialInterface* Mat)
	{
		UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this);
		C->SetStaticMesh(Shape);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCastShadow(false);
		C->SetupAttachment(ToolRoot[Hand]);
		C->RegisterComponent();
		C->SetRelativeLocationAndRotation(At, Rot);
		C->SetRelativeScale3D(Scale);
		C->SetMaterial(0, Mat);
		C->SetBoundsScale(40.f);
		C->SetRenderCustomDepth(true);
		C->SetCustomDepthStencilValue(1);
	};
	const FRotator AlongX(-90.0, 0.0, 0.0);   // a cylinder's height (Z) laid along X
	if (bPen)
	{
		// a quill held as a pen: the nib forward past the fingers, the feather back over the wrist
		UMaterialInterface* Quill = HeldSolidMaterial(FColor(222, 212, 186));
		UMaterialInterface* Feather = HeldSolidMaterial(FColor(236, 232, 222));
		UMaterialInterface* Nib = HeldSolidMaterial(FColor(34, 32, 38));
		Part(Cyl, FVector(6.0, 0.0, 0.0), AlongX, FVector(0.008, 0.008, 0.2), Quill);       // the barrel, -4..16
		Part(Cube, FVector(16.3, 0.0, 0.0), FRotator::ZeroRotator, FVector(0.012, 0.004, 0.004), Nib);   // the nib, tip at 16.9
		Part(Cube, FVector(-9.0, 0.0, 0.6), FRotator(4.0, 0.0, 0.0), FVector(0.15, 0.035, 0.003), Feather);   // the vane
	}
	else if (bShovel)
	{
		UMaterialInterface* Wood = HeldSolidMaterial(FColor(122, 86, 52));
		UMaterialInterface* Iron = HeldSolidMaterial(FColor(78, 78, 82));
		Part(Cyl, FVector(4.0, 0.0, 0.0), AlongX, FVector(0.034, 0.034, 0.95), Wood);         // the shaft, -43..51
		Part(Cube, FVector(-46.0, 0.0, 0.0), FRotator::ZeroRotator, FVector(0.05, 0.13, 0.03), Wood);   // the D-grip
		Part(Cyl, FVector(52.0, 0.0, 0.0), AlongX, FVector(0.05, 0.05, 0.07), Iron);         // the socket
		Part(Cube, FVector(66.0, 0.0, 0.0), FRotator::ZeroRotator, FVector(0.24, 0.21, 0.012), Iron);   // the spade, tip at 78
	}
	else
	{
		UMaterialInterface* Bar = HeldSolidMaterial(FColor(96, 30, 24));
		UMaterialInterface* Steel = HeldSolidMaterial(FColor(70, 72, 78));
		Part(Cyl, FVector(20.0, 0.0, 0.0), AlongX, FVector(0.022, 0.022, 0.62), Bar);         // the bar, -11..51
		Part(Cube, FVector(53.0, 0.0, -0.4), FRotator(-8.0, 0.0, 0.0), FVector(0.05, 0.032, 0.008), Steel);   // the chisel end, tip at 55
		Part(Cube, FVector(-13.5, 0.0, 2.2), FRotator(-40.0, 0.0, 0.0), FVector(0.07, 0.03, 0.018), Steel);   // the claw
	}
	if (AOutlawsLevelActor* L = Level.Get()) { (void)L; }
	HeldLight = -1.f;                          // the new parts take the current sector light
}

FVector AOutlawsPlayer::ToolTip(int32 Hand) const
{
	const double Tip = ToolTipX[Hand];
	return ToolRoot[Hand] ? ToolRoot[Hand]->GetComponentTransform().TransformPosition(FVector(Tip, 0.0, 0.0)) : FVector::ZeroVector;
}

void AOutlawsPlayer::UpdateTools(float Dt)
{
	OL_PERF_SCOPE("Player: tools");
	AOutlawsLevelActor* L = Level.Get();
	UWorld* World = GetWorld();
	UpdateGearCommon(Dt);
	for (int32 H = 0; H < 2; ++H)
	{
		if (HeldTool[H].IsEmpty() || !ToolRoot[H] || !L || !World)
		{
			bToolTipValid[H] = false;
			if (bToolTwoHanded[H] && ToolRoot[H]) ToolRoot[H]->SetRelativeTransform(ToolRest(H));
			bToolTwoHanded[H] = false;
			continue;
		}
		if (IsGear(HeldTool[H]))
		{
			UpdateGear(H, Dt);          // (no stab, no second hand on it)
			continue;
		}
		// both hands on the shovel or the crowbar: it runs from the rear fist toward the front hand, rolled with the rear
		// hand, the front hand drawn round the shaft at ONE place on it (user 2026-09-28: not an extension of one hand;
		// it slid up and down the shaft with the hands' distance)
		{
			const int32 O = 1 - H;
			const FVector Rear = FrameOf(H)->GetComponentLocation();
			const FVector Front = GripOf(O)->GetComponentLocation();
			if (bToolSupport[O] && FVector::Dist(Rear, Front) > 12.0)
			{
				const FVector Up = FrameOf(H)->GetComponentQuat().GetUpVector();
				const FQuat Q = FRotationMatrix::MakeFromXZ((Front - Rear).GetSafeNormal(), Up).ToQuat();
				ToolRoot[H]->SetWorldLocationAndRotation(Rear, Q);
				if (UOutlawsBoxHand* Box = (O == 0 ? LeftHand.Get() : RightHand.Get()))
				{
					const double Lock = HeldTool[H] == TEXT("ishovel") ? 48.0 : 26.0;
					Box->SetWorldLocationAndRotation(Rear + Q.GetForwardVector() * Lock - Q.GetUpVector() * 1.5, Q);
					ForeSnapFrame[O] = GFrameCounter;
				}
				bToolTwoHanded[H] = true;
			}
			else if (bToolTwoHanded[H])
			{
				ToolRoot[H]->SetRelativeTransform(ToolRest(H));
				bToolTwoHanded[H] = false;
			}
		}
		L->SetToolHeld(L->UseIdOf(HeldTool[H]), true);     // (a new level's INF starts without it)
		ToolCooldown[H] = FMath::Max(0.f, ToolCooldown[H] - Dt);
		if (HeldTool[H] == TEXT("ishovel") && !bToolTwoHanded[H]) ToolRoot[H]->SetRelativeTransform(ToolRest(H));    // (last frame's stop undone)
		const FVector Tip = ToolTip(H);
		// the spade meets the ground (user 2026-10-01): it goes in about half its width (10 cm) and no further, the
		// ground clipping through the blade as it digs; driven deeper, the shovel slides back along its shaft in the
		// hands.  The dig itself still goes by the hands' own push (Tip, before the stop)
		if (HeldTool[H] == TEXT("ishovel"))
		{
			constexpr double Sink = 10.0;
			const FVector Back = ToolRoot[H]->GetComponentTransform().TransformPosition(FVector(FMath::Max(0.0, ToolTipX[H] - 60.0), 0.0, 0.0));
			FCollisionQueryParams GQ(SCENE_QUERY_STAT(OutlawsSpadeGround), true, this);
			L->IgnoreHiddenOverlaps(GQ);
			L->IgnorePassedOverlaps(GQ);
			FHitResult Gr;
			if (World->LineTraceSingleByChannel(Gr, Back, Tip, ECC_Visibility, GQ) && Gr.ImpactNormal.Z > 0.5)
			{
				const double In = FVector::Dist(FVector(Gr.ImpactPoint), Tip);
				if (In > Sink) ToolRoot[H]->AddWorldOffset(-(Tip - Back).GetSafeNormal() * (In - Sink));
			}
		}
		if (!bToolTipValid[H])
		{
			ToolTipPrev[H] = Tip;
			bToolTipValid[H] = true;
			continue;
		}
		const FVector Move = Tip - ToolTipPrev[H];
		ToolTipPrev[H] = Tip;
		if (HeldTool[H] == TEXT("iink"))
		{
			// the pen: its nib (or the hand holding it) brought to the guest book signs it, no stab needed (user
			// 2026-09-28); looked for five times a second, then a rest after it took
			if (ToolCooldown[H] <= 0.f && !bMenuOpen && !bWheelOpen && !bDeadView)
			{
				const FVector Eye = Camera->GetComponentLocation();
				if (L->UseByHand(Eye, Tip, false) || L->UseByHand(Eye, GripOf(H)->GetComponentLocation(), false))
				{
					Pulse(H, 0.5f);
					ToolCooldown[H] = 2.f;
				}
				else
				{
					ToolCooldown[H] = 0.2f;
				}
			}
			continue;
		}
		const float Speed = Move.Size() / FMath::Max(Dt, 1e-3f);
		const float Swing = float((Move / FMath::Max(Dt, 1e-3f) - GetVelocity()).Size());     // (the swing, not the walk)
		if (ToolCooldown[H] > 0.f || Speed < 60.f || bMenuOpen || bWheelOpen || bDeadView)
		{
			continue;
		}
		// the tip's path this frame meeting the world: a stab
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsToolStab), false, this);
		FHitResult Hit;
		const FVector Dir = Move.GetSafeNormal();
		// a swing through a window or a bottle breaks it (user 2026-09-28); a blow only with the grip held (2026-09-29)
		if (Swing > OutlawsFeel::HitSpeedHard) L->ShatterAlong(Tip - Move, Tip);    // (glass: no grip needed)
		if (Swing > OutlawsFeel::HitSpeedHard && Axis(H == 0 ? TEXT("OLVR_LeftGrip") : TEXT("OLVR_RightGrip")) >= 0.5f)
		{
			if (MeleeDynamite(Tip))
			{
				ToolCooldown[H] = 0.25f;
				continue;
			}
			FVector SwingEnd = Tip;
			if (L->ShootAlong(Tip - Move, SwingEnd))
			{
				Pulse(H, 0.5f);
				ToolCooldown[H] = 0.25f;
				continue;
			}
			// the shovel or the crowbar swung into a bad guy lands as a club (user 2026-10-01), harder than a fist (the fist
			// 0.8); a held one too, though not by the hand holding the tool.  What lands is the tool from a hand's width past
			// the fist to its end, on his body or on his picture as drawn, as a fist lands (BUG-SHOVELWHACK, user 2026-10-27:
			// a shovel swung at a bad guy's head did nothing: only its outer half within 6 cm of his body's capsule counted,
			// the capsule 180 cm tall and the bad guys drawn up to 220, and face to face the outer half of the 111 cm shovel
			// was past him).  Only a swing (the tip faster than the walk by HitSpeedHard, above): a tool resting on him or
			// carried into him as the player walks is no blow
			if (HeldTool[H] == TEXT("ishovel") || HeldTool[H] == TEXT("icrowbar"))
			{
				const bool bShovel = HeldTool[H] == TEXT("ishovel");
				const FVector Eye = Camera->GetComponentLocation();
				const FVector Butt = ToolRoot[H]->GetComponentTransform().TransformPosition(FVector(FMath::Min(ToolStrikeFromCm, ToolTipX[H] * 0.45), 0.0, 0.0));
				AOutlawsEnemy* Struck = nullptr;
				FVector At = Tip;
				if (UOutlawsCoop::StrikeAlongAtPlayers(L, Butt, Tip, 10.f, (bShovel ? 1.4f : 1.2f) * FMath::FRandRange(0.9f, 1.1f) * L->PlayerDamageScale(), bShovel ? TEXT("a shovel") : TEXT("a crowbar"),
					[&](const FVector& P) { return MeleeInSight(this, L, Eye, P); }))
				{
					L->PlaySoundAt(TEXT("connect"), Tip);   // (M1's gaps: another player in a match)
					PulseImpact(H, Swing);
				}
				for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
				{
					AOutlawsEnemy* E = Wk.Get();
					if (!E || E->IsDead() || E->IsDormant() || E == GrabbedEnemy[H].Get()) continue;
					if (ToolMeets(E, Butt, Tip, At) && MeleeInSight(this, L, Eye, At))
					{
						Struck = E;
						break;
					}
				}
				if (Struck)
				{
					const float Damage = (bShovel ? 1.4f : 1.2f) * FMath::FRandRange(0.9f, 1.1f);
					Struck->TakeHit(Damage * L->PlayerDamageScale(), At, Dir, At - Dir * 30.f);
					L->PlaySoundAt(TEXT("connect"), At);
					PulseImpact(H, Swing);
					if (bToolSupport[1 - H]) PulseImpact(1 - H, Swing * 0.8f);
					ToolCooldown[H] = 0.5f;
					UE_LOG(LogOutlaws, Log, TEXT("OLRT %s whacks %s at %.0f cm/s: %.2f"), bShovel ? TEXT("shovel") : TEXT("crowbar"), *Struck->GetName(), Speed, Damage);
					continue;
				}
			}
		}
		if (!World->LineTraceSingleByChannel(Hit, Tip - Move, Tip + Dir * 2.0, ECC_Visibility, Q))
		{
			continue;
		}
		if (HeldTool[H] == TEXT("ishovel"))
		{
			// the spade driven down into the ground digs where the ground can be dug
			const bool bDown = Hit.ImpactNormal.Z > 0.6 && Move.Z < 0.0;
			const bool bDug = bDown && L->ToolStabFloor(Hit.ImpactPoint);
			// earth flung out round the spade head each time it goes in (user 2026-09-28), more where it digs
			if (bDown) L->SpawnDirt(Hit.ImpactPoint, Hit.ImpactNormal, bDug ? 10 : 6);
			Pulse(H, bDug ? 0.7f : 0.2f);
			ToolCooldown[H] = bDug ? 0.45f : 0.25f;
		}
		else
		{
			// the crowbar's end jabbed into a door (or a switch, a wall onto a door): as a hand pushing it
			const bool bUsed = L->UseByHand(Camera->GetComponentLocation(), Hit.ImpactPoint + Dir * 3.0, true);
			Pulse(H, bUsed ? 0.6f : 0.2f);
			ToolCooldown[H] = bUsed ? 0.6f : 0.25f;
		}
	}
}

FVector AOutlawsPlayer::ForendSpot(int32 Wi) const
{
	if (Wi < 0)
	{
		return FVector::ZeroVector;
	}
	const FWeaponDef& W = GWeapons[Wi];
	const FVector Bore = W.BoreFix.Quaternion().Inverse().RotateVector(FVector::ForwardVector);
	const double Len = FMath::Max(10.0, W.Muzzle.X);
	return W.Muzzle - Bore * 0.38 * Len - FVector(0.0, 0.0, 3.0);
}

bool AOutlawsPlayer::NearToolShaft(int32 Hand) const
{
	// the other hand's shovel: anywhere along the shaft from a hand's width past the rear fist to the blade; a hand
	// holding a weapon may take it too, its weapon put away while it does (user 2026-09-28)
	const int32 O = 1 - Hand;
	if (HeldTool[O].IsEmpty() || HeldTool[O] == TEXT("iink") || IsGear(HeldTool[O]) || !ToolRoot[O] || HeldItem[Hand].IsValid() || HeldBottle[Hand] || !HeldTool[Hand].IsEmpty())
	{
		return false;
	}
	const FTransform Xf = ToolRoot[O]->GetComponentTransform();
	const FVector A = Xf.TransformPosition(FVector(14.0, 0.0, 0.0));
	const FVector B = Xf.TransformPosition(FVector(FMath::Max(30.0, ToolTipX[O] - (HeldTool[O] == TEXT("ishovel") ? 30.0 : 10.0)), 0.0, 0.0));
	return FMath::PointDistToSegment(GripOf(Hand)->GetComponentLocation(), A, B) < 14.0;
}

bool AOutlawsPlayer::NearForegrip(int32 Hand) const
{
	// the other hand's long gun: a stretch under the barrel, 30 to 62 percent of the way back from the
	// muzzle (the forend, the pump, the handguard), within a palm's reach
	const int32 O = 1 - Hand;
	const int32 Wi = Equipped[O];
	// (a weapon in this hand too: it is put away while the hand holds the forend, user 2026-10-01; the lantern too: it
	// hangs under the fist that holds the forend, user 2026-10-02)
	if (Wi < 0 || !GWeapons[Wi].bTwoHand || !WeaponMesh[O] || bSupport[O] || HeldItem[Hand].IsValid() || HeldBottle[Hand]
		|| (!HeldTool[Hand].IsEmpty() && HeldTool[Hand] != TEXT("ilantern")))
	{
		return false;
	}
	const FWeaponDef& W = GWeapons[Wi];
	const FVector Bore = W.BoreFix.Quaternion().Inverse().RotateVector(FVector::ForwardVector);
	const double Len = FMath::Max(10.0, W.Muzzle.X);
	const FTransform Xf = WeaponMesh[O]->GetComponentTransform();
	const FVector A = Xf.TransformPosition(W.Muzzle - Bore * 0.05 * Len - FVector(0.0, 0.0, 3.0));
	const FVector B = Xf.TransformPosition(W.Muzzle - Bore * 0.70 * Len - FVector(0.0, 0.0, 3.0));
	return FMath::PointDistToSegment(GripOf(Hand)->GetComponentLocation(), A, B) < 16.0;
}

bool AOutlawsPlayer::IsMountedGun() const
{
	for (int32 H = 0; H < 2; ++H)
	{
		if (Equipped[H] >= 0 && GWeapons[Equipped[H]].bAuto) return true;
	}
	return false;
}

bool AOutlawsPlayer::CarryingMan() const
{
	for (int32 H = 0; H < 2; ++H)
	{
		const AOutlawsEnemy* E = GrabbedEnemy[H].Get();
		if (E && !E->IsCritter()) return true;
	}
	return false;
}

float AOutlawsPlayer::ThrowPower(int32 Hand) const
{
	// the hand over the tracking floor, scaled as the recenter scales the eyes (165 cm standing), so a tall player and a
	// short one throw alike and the head's own height never enters
	const double Up = GripOf(Hand)->GetComponentLocation().Z - VROrigin->GetComponentLocation().Z + (bVR ? HeightOffset : 0.f);
	return FMath::Clamp(static_cast<float>((Up - 87.0) / 120.0), 0.f, 1.f);
}

FVector AOutlawsPlayer::ThrowHeading(int32 Hand) const
{
	// (user 2026-10-06, BUG-THROWAIM: the throw followed the left stick) the way from the face out through the hand, along
	// the ground: the head's position only, never its look; hand and head both ride the tracking space, so walking, a snap
	// turn or a smooth one never steers it, and the wrist's roll and pitch never enter
	FVector Out = GripOf(Hand)->GetComponentLocation() - Camera->GetComponentLocation();
	Out.Z = 0.0;
	if (Out.Size() > 4.0) return Out.GetSafeNormal();
	// the hand right under the eyes: the way it points
	const FVector F(AimOf(Hand)->GetForwardVector().X, AimOf(Hand)->GetForwardVector().Y, 0.0);
	if (F.SizeSquared() > 1e-4) return F.GetSafeNormal();
	return FVector(GetActorForwardVector().X, GetActorForwardVector().Y, 0.0).GetSafeNormal();
}

FVector AOutlawsPlayer::ThrowVelocity(int32 Hand) const
{
	// the first throw (user 2026-10-06): the hand's height alone the power, waist a lob, overhead the longest (ThrowPower);
	// the weapon's own angle, the stick 35 and the rest 12 degrees; the way from the face through the hand (ThrowHeading).
	// No hand path, no running carried in: what the arc shows is where it goes.  A man flies ManThrowScale of the speed
	const int32 Wi = Equipped[Hand];
	const bool bDynamite = Wi >= 0 && GWeapons[Wi].Throw == 1;
	const AOutlawsEnemy* Held = GrabbedEnemy[Hand].Get();
	// (a man: as far and as high as the palm-up push, ManAimThrowScale at ManAimThrowPitch; the heading as the rest)
	const bool bMan = Held && !Held->IsCritter();
	const float Share = bMan ? ManAimThrowScale : 1.f;
	const float Power = ThrowPower(Hand);
	const float Speed = (bDynamite ? FMath::Lerp(350.f, 2000.f, Power) : FMath::Lerp(500.f, 2600.f, Power)) * Share;
	const float Rad = FMath::DegreesToRadians(bDynamite ? 35.f : bMan ? ManAimThrowPitch : 12.f);
	return (ThrowHeading(Hand) * FMath::Cos(Rad) + FVector(0.0, 0.0, FMath::Sin(Rad))) * Speed;
}

float AOutlawsPlayer::ThrowGravity(const FVector& At) const
{
	// the player's own fall there (UpdateGravity): BaseGravityScale g times the sector's GRAVITY over townex's -60
	const AOutlawsLevelActor* L = Level.Get();
	const float Factor = L ? FMath::Clamp(-L->GravityAt(At) / 60.f, 0.f, 4.f) : 1.f;
	return FMath::Max(1.f, 980.f * BaseGravityScale * Factor);
}

FVector AOutlawsPlayer::TossVelocity(int32 Hand, float Share) const
{
	// the hand's own swing against the body (HandVelocity; the world one drops anything over 4 m/s as a recentre, which
	// held every toss under 5 m/s); a snap turn swings the hand round the head faster than any arm for a frame or two:
	// then the world one
	FVector Swing = HandVelocity[Hand];
	if (Swing.Size() > 1500.f) Swing = FVector(HandWorldVel[Hand]) - BodyCarry();
	// no faster than the hardest aimed throw for its weight: a flick is a throw, not a shot
	const FVector V = (Swing * (TossGain * Share)).GetClampedToMaxSize(2600.f * Share);
	return V + BodyCarry();
}

void AOutlawsPlayer::ShowThrowArc(int32 Hand, bool bShow)
{
	const int32 Per = ArcDotCount + 1;
	UWorld* World = GetWorld();
	if (!bShow || !World || (!WeaponMesh[Hand] && !HeldBottle[Hand] && !GrabbedEnemy[Hand].IsValid()))
	{
		// (BUG-ARCSTUCK: nothing to throw from the hand hides it too, never leaves the last one standing)
		for (int32 i = Hand * Per; i < (Hand + 1) * Per && i < ArcDots.Num(); ++i) if (ArcDots[i]) ArcDots[i]->SetVisibility(false);
		bArcUp[Hand] = false;
		return;
	}
	bArcUp[Hand] = true;
	ArcDrawnFrame[Hand] = GFrameCounter;
	if (ArcDots.Num() < 2 * Per)
	{
		UMaterialInterface* GlowBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_FxGlow.M_OLR_FxGlow"));
		UMaterialInterface* Mat = nullptr;
		if (GlowBase)
		{
			UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(GlowBase, this);
			M->SetVectorParameterValue(TEXT("Color"), FLinearColor(1.f, 0.85f, 0.4f));
			M->SetScalarParameterValue(TEXT("Glow"), 1.5f);
			KeepAlive.Add(M);
			Mat = M;
		}
		else
		{
			Mat = SolidMaterial(FColor(255, 215, 100));
		}
		while (ArcDots.Num() < 2 * Per) ArcDots.Add(MakeFx(this, VROrigin, Mat, TEXT("/Engine/BasicShapes/Sphere.Sphere")));
	}
	// the flight, step by step under the same gravity the throw will have, to the first thing it meets
	FVector P = GrabbedEnemy[Hand].IsValid() ? GrabbedEnemy[Hand]->GrabChest()
		: HeldBottle[Hand] ? HeldBottle[Hand]->Bounds.Origin : WeaponMesh[Hand]->GetComponentLocation();
	FCollisionQueryParams QA(SCENE_QUERY_STAT(OutlawsThrowArcHeld), true, this);
	if (GrabbedEnemy[Hand].IsValid()) QA.AddIgnoredActor(GrabbedEnemy[Hand].Get());
	FVector V = ThrowVelocity(Hand);
	const float G = ThrowGravity(P);     // (the flight's own)
	FCollisionQueryParams& Q = QA;
	bool bLanded = false;
	for (int32 i = 0; i < ArcDotCount; ++i)
	{
		UStaticMeshComponent* Dot = ArcDots[Hand * Per + i];
		if (bLanded)
		{
			Dot->SetVisibility(false);
			continue;
		}
		// a tenth of a second a dot (the flights are twice as long as at the old pace: the dots stand where they did)
		const FVector Next = P + V * 0.1f;
		V.Z -= G * 0.1f;
		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, P, Next, ECC_Visibility, Q))
		{
			bLanded = true;
			UStaticMeshComponent* Mark = ArcDots[Hand * Per + ArcDotCount];
			Mark->SetWorldLocationAndRotation(Hit.ImpactPoint + Hit.ImpactNormal * 1.0, FRotationMatrix::MakeFromZ(Hit.ImpactNormal).Rotator());
			const float MarkFar = static_cast<float>(FMath::Max(1.0, FVector::Dist(Hit.ImpactPoint, Camera->GetComponentLocation()) / 600.0));
			Mark->SetWorldScale3D(FVector(FMath::Min(0.35f * MarkFar, 1.2f), FMath::Min(0.35f * MarkFar, 1.2f), 0.02f));
			Mark->SetVisibility(true);
			Dot->SetVisibility(false);
			continue;
		}
		P = Next;
		Dot->SetWorldLocation(P);
		// bigger the further off, so the far end reads in the headset (user 2026-09-28): 2.5 cm near, up to 14 cm
		Dot->SetWorldScale3D(FVector(FMath::Clamp(0.025 * FVector::Dist(P, Camera->GetComponentLocation()) / 150.0, 0.025, 0.14)));
		Dot->SetVisibility(i > 0);           // (the first sits in the hand)
	}
	if (!bLanded) ArcDots[Hand * Per + ArcDotCount]->SetVisibility(false);
}

void AOutlawsPlayer::SweepThrowArcs()
{
	// (BUG-ARCSTUCK, user 15 Oct: a held man punched dead by the other hand left his arc standing in the world) whatever
	// shows an arc draws it every frame; one this frame did not draw is of a thing no longer held to throw
	for (int32 H = 0; H < 2; ++H)
	{
		if (!bArcUp[H] || ArcDrawnFrame[H] == GFrameCounter) continue;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT throw arc of the %s hand not drawn this frame: hidden"), H == 0 ? TEXT("left") : TEXT("right"));
		ShowThrowArc(H, false);
	}
}

bool AOutlawsPlayer::ArcDotsVisible(int32 Hand) const
{
	const int32 Per = ArcDotCount + 1;
	for (int32 i = Hand * Per; i < (Hand + 1) * Per && i < ArcDots.Num(); ++i) if (ArcDots[i] && ArcDots[i]->IsVisible()) return true;
	return false;
}

void AOutlawsPlayer::ShowTeleportArc(bool bShow)
{
	if (!bShow)
	{
		for (UStaticMeshComponent* D : TeleportDots) if (D) D->SetVisibility(false);
		return;
	}
	if (TeleportDots.Num() < ArcDotCount + 1)
	{
		// yellow: the teleport's own, apart from the gold of a throw (user 2026-09-28)
		UMaterialInterface* GlowBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_FxGlow.M_OLR_FxGlow"));
		UMaterialInterface* Mat = nullptr;
		if (GlowBase)
		{
			UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(GlowBase, this);
			M->SetVectorParameterValue(TEXT("Color"), FLinearColor(1.f, 0.95f, 0.05f));
			M->SetScalarParameterValue(TEXT("Glow"), 1.8f);
			KeepAlive.Add(M);
			Mat = M;
		}
		else
		{
			Mat = SolidMaterial(FColor(255, 240, 20));
		}
		while (TeleportDots.Num() < ArcDotCount + 1) TeleportDots.Add(MakeFx(this, VROrigin, Mat, TEXT("/Engine/BasicShapes/Sphere.Sphere")));
	}
}

void AOutlawsPlayer::UpdateTeleport(float Dt)
{
	OL_PERF_SCOPE("Player: teleport");
	AOutlawsLevelActor* L = Level.Get();
	UWorld* World = GetWorld();
	if (L) L->bTeleportPace = bVR && bTeleportMove;
	const bool bCan = bVR && bTeleportMove && L && World && !bSwimming && !IsClimbing() && MantleLeft <= 0.f && !IsMountedGun()
		&& !bMenuOpen && !bWheelOpen && !bDeadView && !(L->IsFrontEnd());
	const FVector2D S = MoveStick();
	if (!bCan)
	{
		if (bTeleportAim) ShowTeleportArc(false);
		bTeleportAim = false;
		return;
	}
	if (!bTeleportAim)
	{
		// a flick sideways snap-turns, as the turn stick does (user 2026-09-29); once per flick
		if (!bTeleportTurnLatched && FMath::Abs(S.X) > 0.7f && FMath::Abs(S.Y) < 0.5f)
		{
			TurnBy(FMath::Sign(S.X) * SnapTurnDegrees);
			bTeleportTurnLatched = true;
			return;
		}
		if (FMath::Abs(S.X) < 0.3f) bTeleportTurnLatched = false;
		if (S.Y > 0.6f && !bTeleportTurnLatched) bTeleportAim = true;    // the move stick pushed forward: aiming
		else return;
	}
	if (S.Size() < 0.3f)
	{
		// let go: there, if the spot took it; a blink of black (no sliding view, for comfort)
		bTeleportAim = false;
		ShowTeleportArc(false);
		if (!bTeleportValid) return;
		UCharacterMovementComponent* M = GetCharacterMovement();
		const float Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		SetActorLocation(TeleportTarget + FVector(0.0, 0.0, Half + 1.0), false, nullptr, ETeleportType::TeleportPhysics);
		if (M)
		{
			M->Velocity = FVector::ZeroVector;
			M->SetMovementMode(MOVE_Walking);
		}
		if (APlayerController* PC = Cast<APlayerController>(GetController()))
		{
			if (PC->PlayerCameraManager) PC->PlayerCameraManager->StartCameraFade(1.f, 0.f, 0.15f, FLinearColor::Black, false, false);
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLRT teleported to %s"), *TeleportTarget.ToString());
		return;
	}
	// the arc from the stick's hand along its pointing, under ordinary gravity (the game's own is 4.5 times it: its
	// arcs would fall at the feet), 9 m/s: up to about 8 m away
	ShowTeleportArc(true);
	UMotionControllerComponent* From = OffAim();
	FVector P = From->GetComponentLocation();
	FVector V = From->GetForwardVector() * 900.0;
	const float G = 980.f;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsTeleportArc), true, this);
	FCollisionResponseParams IgnorePawns;           // (bad guys and bodies do not stop the arc; the ground does)
	IgnorePawns.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
	const float Feet = GetActorLocation().Z - GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	bool bLanded = false;
	bTeleportValid = false;
	UStaticMeshComponent* Ring = TeleportDots[ArcDotCount];
	for (int32 i = 0; i < ArcDotCount; ++i)
	{
		UStaticMeshComponent* Dot = TeleportDots[i];
		if (bLanded)
		{
			Dot->SetVisibility(false);
			continue;
		}
		const FVector Next = P + V * 0.05f;
		V.Z -= G * 0.05f;
		FHitResult Hit;
		FVector Wet;
		const bool bWater = L->WaterSurfaceHit(P, Next, Wet);
		if (bWater || World->LineTraceSingleByChannel(Hit, P, Next, ECC_Visibility, Q, IgnorePawns))
		{
			bLanded = true;
			Dot->SetVisibility(false);
			// only ground: a floor (not a wall or a ceiling), dry, no more than a step and a bit above the feet, room
			// for the body there
			bool bOk = !bWater && Hit.ImpactNormal.Z > 0.7 && Hit.ImpactPoint.Z - Feet < 120.0;
			if (bOk)
			{
				UCapsuleComponent* Cap = GetCapsuleComponent();
				FCollisionQueryParams CQ(SCENE_QUERY_STAT(OutlawsTeleportFit), false, this);
				FCollisionResponseParams RP;
				Cap->InitSweepCollisionParams(CQ, RP);
				const float Half = Cap->GetScaledCapsuleHalfHeight();
				bOk = !World->OverlapBlockingTestByChannel(FVector(Hit.ImpactPoint) + FVector(0.0, 0.0, Half + 2.0), FQuat::Identity,
					Cap->GetCollisionObjectType(), FCollisionShape::MakeCapsule(Cap->GetScaledCapsuleRadius(), Half), CQ, RP);
			}
			bTeleportValid = bOk;
			TeleportTarget = Hit.ImpactPoint;
			if (bOk)
			{
				Ring->SetWorldLocationAndRotation(FVector(Hit.ImpactPoint) + FVector(0.0, 0.0, 1.0), FRotator::ZeroRotator);
				Ring->SetWorldScale3D(FVector(0.6, 0.6, 0.02));      // a flat yellow disc the body's width where it will stand
				Ring->SetVisibility(true);
			}
			continue;
		}
		P = Next;
		Dot->SetWorldLocation(P);
		Dot->SetWorldScale3D(FVector(FMath::Clamp(0.025 * FVector::Dist(P, Camera->GetComponentLocation()) / 150.0, 0.025, 0.14)));
		Dot->SetVisibility(i > 0);           // (the first sits in the hand)
	}
	if (!bTeleportValid) Ring->SetVisibility(false);
}

void AOutlawsPlayer::ThrowHeld(int32 Hand, bool bUnlit, bool bDrop)
{
	// a copy of what the hand holds flies off with the hand's swing (a weak toss becomes a lob ahead)
	AOutlawsLevelActor* L = Level.Get();
	UProceduralMeshComponent* Gun = WeaponMesh[Hand];
	UWorld* World = GetWorld();
	if (!L || !Gun || !World)
	{
		return;
	}
	const FWeaponDef& W = GWeapons[Equipped[Hand]];
	const FVector Vel = bDrop ? FVector(HandWorldVel[Hand]) : ThrowVelocity(Hand);
	ShowThrowArc(Hand, false);
	FActorSpawnParameters SP;
	SP.ObjectFlags |= RF_Transient;
	AActor* A = World->SpawnActor<AActor>(AActor::StaticClass(), Gun->GetComponentTransform(), SP);
	if (!A)
	{
		return;
	}
	UProceduralMeshComponent* M = NewObject<UProceduralMeshComponent>(A);
	M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	M->SetCastShadow(false);
	A->SetRootComponent(M);
	M->RegisterComponent();
	for (int32 i = 0; i < Gun->GetNumSections(); ++i)
	{
		if (FProcMeshSection* S = Gun->GetProcMeshSection(i))
		{
			// a stick of dynamite can be shot where it lies or flies (user 2026-09-28): its mesh answers shot traces
			FProcMeshSection Copy = *S;
			Copy.bEnableCollision = W.Throw == 1;
			M->SetProcMeshSection(i, Copy);
		}
		// its own instance with the held material's pull off: out of the hand it is part of the world (behind walls,
		// and its flame in front of it; user 2026-09-28)
		UMaterialInterface* Src = Gun->GetMaterial(i);
		UMaterialInterface* Base = HeldBase();
		if (Base && Src)
		{
			UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, A);
			if (UMaterialInstance* SrcMi = Cast<UMaterialInstance>(Src)) Mid->CopyParameterOverrides(SrcMi);
			Mid->SetScalarParameterValue(TEXT("NearPull"), 0.f);
			M->SetMaterial(i, Mid);
		}
		else
		{
			M->SetMaterial(i, Src);
		}
	}
	A->SetActorTransform(Gun->GetComponentTransform());
	if (W.Throw == 1)
	{
		M->bUseComplexAsSimpleCollision = true;
		M->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
		M->SetCollisionResponseToAllChannels(ECR_Ignore);
		M->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
	}
	FThrown T;
	T.Actor = A;
	T.Pos = Gun->GetComponentLocation();
	T.Vel = Vel;
	T.Pull = ThrowGravity(T.Pos);
	T.Type = W.Throw;
	T.Fuse = W.Throw == 1 && !bUnlit ? Fuse[Hand] : -1.f;
	T.bDud = W.Throw == 1 && bUnlit;
	T.Sound = FuseSound[Hand];
	T.Scale0 = A->GetActorScale3D();
	T.GrowTo = W.Throw == 1 || W.Throw == 2 ? W.ItemScale : 1.f;     // (the stick and the knife: the level item's size)
	T.Damage = W.Damage;
	// end over end toward where it flies (Pitch = degrees a second; user 2026-09-28: front flips, no random tumble)
	T.Spin = FRotator((W.Throw == 2 ? FMath::FRandRange(560.f, 700.f) : FMath::FRandRange(380.f, 480.f)) * (bDrop ? 0.2f : 1.f), 0.f, 0.f);
	if (W.Throw == 2)
	{
		T.Homing = KnifeTargetNear(T.Pos, Vel);
		if (AOutlawsEnemy* Tg = T.Homing.Get()) UE_LOG(LogOutlaws, Log, TEXT("OLRT knife homing on %s"), *Tg->GetName());
	}
	if (W.Throw == 1 && bFuseLit[Hand])
	{
		// still burning: its wick and a flame of its own (user 2026-09-28)
		T.FuseSection = FuseSection[Hand];
		T.WickPos = FuseRestPos[Hand];
		T.WickNrm = FuseRestNrm[Hand];
		T.WickUV = FuseRestUV[Hand];
		T.WickCol = FuseRestCol[Hand];
		UStaticMeshComponent* Fl = nullptr;
		UStaticMeshComponent* Co = nullptr;
		MakeFuseFlame(M, Fl, Co, false);       // (thrown: at its own depth, as the stick's own copy is)
		T.Flame = Fl;
		T.Core = Co;
	}
	Thrown.Add(T);
	if (!bDrop)
	{
		++TutNotes.Throws;                           // (REQ-TUTORIAL)
		if (T.Fuse > 0.f) ++TutNotes.LitThrows;
	}
	if (UOutlawsCoop::IsActive(L)) UOutlawsCoop::NoteThrow(L, T.Type, T.Pos, T.Vel, T.Pull / 980.f, T.Fuse);       // (co-op: seen flying there, at its fall)
	FuseSound[Hand] = nullptr;
	bFuseLit[Hand] = false;
	Mag[Hand] = 0;
	ThrowRestock[Hand] = 0.6f;
	if (!bDrop) L->PlaySoundAt(W.FireSound, T.Pos);
	Pulse(Hand, bDrop ? 0.25f : 0.5f);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT threw %s at %.0f cm/s"), W.Label, Vel.Size());
}

bool AOutlawsPlayer::BuildThrownLook(AActor* OnActor, int32 Type, bool bLit, FOutlawsThrownLook& Out)
{
	// (BUG-REMOTETNT) another player's stick or knife was a red cylinder or a grey box here: the thrower's own copy is the
	// held model (ThrowHeld), which this machine has only once its own hand has held one.  The same model built here (the
	// session's cache, the level's 3DO and textures), each section on the held material with the pull off, the wick in its
	// own section and a lit stick's flame (MakeFuseFlame, at its own depth as thrown)
	Out = FOutlawsThrownLook();
	if (!OnActor || !OnActor->GetRootComponent() || (Type != 1 && Type != 2)) return false;
	int32 Wi = -1;
	for (int32 i = 0; i < NumWeapons && Wi < 0; ++i) if (GWeapons[i].Throw == Type) Wi = i;
	UMaterialInterface* Base = HeldBase();
	if (Wi < 0 || !Base) return false;
	bool bCard = false;
	FString Path;
	const TSharedPtr<OutlawsObj::FObjMesh> Model = HeldModelFor(Wi, bCard, Path);
	if (!Model.IsValid() || Model->Sections.Num() == 0) return false;
	UProceduralMeshComponent* M = NewObject<UProceduralMeshComponent>(OnActor);
	M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	M->SetCastShadow(false);
	M->SetupAttachment(OnActor->GetRootComponent());
	M->SetUsingAbsoluteLocation(true);
	M->SetUsingAbsoluteRotation(true);
	M->SetUsingAbsoluteScale(true);
	M->RegisterComponent();
	int32 Index = 0;
	for (const TPair<FString, FOutlawsMeshSection>& Kv : Model->Sections)
	{
		const FOutlawsMeshSection& S = Kv.Value;
		M->CreateMeshSection_LinearColor(Index, S.Positions, S.Triangles, S.Normals, S.UVs, S.Colors, TArray<FProcMeshTangent>(), false);
		if (Kv.Key.StartsWith(TEXT("fuse:")))
		{
			Out.FuseSection = Index;
			Out.WickPos = S.Positions;
			Out.WickNrm = S.Normals;
			Out.WickUV = S.UVs;
			Out.WickCol = S.Colors;
		}
		UMaterialInterface* Src = HeldSectionMaterial(Kv.Key, S.Texture);
		UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, OnActor);
		if (UMaterialInstance* SrcMi = Cast<UMaterialInstance>(Src)) Mid->CopyParameterOverrides(SrcMi);
		Mid->SetScalarParameterValue(TEXT("NearPull"), 0.f);
		M->SetMaterial(Index, Mid);
		++Index;
	}
	Out.Mesh = M;
	Out.GrowTo = GWeapons[Wi].ItemScale;
	if (Type == 1 && bLit && Out.FuseSection >= 0)
	{
		if (FireCells.Num() == 0)
		{
			if (AOutlawsLevelActor* L = Level.Get()) { if (UMaterialInstanceDynamic* F = L->MakeFireLook(this, FireCells, FireFps, FireAspect)) KeepAlive.Add(F); }
		}
		UStaticMeshComponent* Fl = nullptr;
		UStaticMeshComponent* Co = nullptr;
		MakeFuseFlame(M, Fl, Co, false);
		Out.Flame = Fl;
		Out.Core = Co;
	}
	return true;
}

void AOutlawsPlayer::PlaceThrownFuse(const FOutlawsThrownLook& Look, float FuseLeft)
{
	UProceduralMeshComponent* M = Look.Mesh.Get();
	if (!M || Look.FuseSection < 0) return;
	PlaceFuse(M, Look.FuseSection, Look.WickPos, Look.WickNrm, Look.WickUV, Look.WickCol, FMath::Clamp(FuseLeft / 4.5f, 0.04f, 1.f), true,
		Look.Flame.Get(), Look.Core.Get(), 1.8f);         // (as UpdateThrown places this player's own)
}

#if WITH_OUTLAWS_TESTS
void AOutlawsPlayer::TestClearThrown()
{
	for (FThrown& T : Thrown)
	{
		if (T.Sound.IsValid()) T.Sound->Stop();
		if (AActor* A = T.Actor.Get()) A->Destroy();
	}
	Thrown.Reset();
}
#endif // WITH_OUTLAWS_TESTS

void AOutlawsPlayer::UpdateThrown(float Dt)
{
	OL_PERF_SCOPE("Player: thrown");
	AOutlawsLevelActor* L = Level.Get();
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	const float G = 980.f * BaseGravityScale;
	// a thing come to rest stood on its own up on a floor at FloorZ, to be taken up again
	auto RestOn = [](AActor* A, FThrown& T, double FloorZ)
	{
		A->SetActorRotation(FQuat::FindBetweenNormals(A->GetActorQuat().RotateVector(T.Axis), FVector::UpVector) * A->GetActorQuat());
		UPrimitiveComponent* Root = Cast<UPrimitiveComponent>(A->GetRootComponent());
		if (Root) Root->UpdateBounds();
		const double Bottom = Root ? Root->Bounds.GetBox().Min.Z : A->GetActorLocation().Z;
		T.Pos = A->GetActorLocation() + FVector(0.0, 0.0, FloorZ + 0.2 - Bottom);
		T.Vel = FVector::ZeroVector;
		T.bResting = true;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT thrown prop came to rest after %.1f s"), T.Age);
	};
	for (int32 i = Thrown.Num() - 1; i >= 0; --i)
	{
		FThrown& T = Thrown[i];
		AActor* A = T.Actor.Get();
		T.Age += Dt;
		// a pot, a pan, a skull never goes: still rattling about after four seconds (caught on a ledge or a wall), it is
		// set on the floor under it (user 2026-09-29: thrown things that do not break land to be picked up again)
		if (A && T.Type == 4 && !T.bResting && T.Age > 4.f)
		{
			FCollisionQueryParams FQ(SCENE_QUERY_STAT(OutlawsPotSettle), true, this);
			FQ.AddIgnoredActor(A);
			FCollisionResponseParams FRP;
			FRP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
			FHitResult Floor;
			if (World->LineTraceSingleByChannel(Floor, T.Pos, T.Pos - FVector(0.0, 0.0, 3000.0), ECC_Visibility, FQ, FRP))
			{
				RestOn(A, T, Floor.ImpactPoint.Z);
				A->SetActorLocation(T.Pos);
			}
		}
		bool bGone = !A || (T.Age > 12.f && !((T.Type == 3 || T.Type == 4) && T.bResting));    // (a bottle or a pot set down stays)
		if (bGone && A && T.Type == 4) UE_LOG(LogOutlaws, Warning, TEXT("OLRT thrown prop LOST unsettled at %s"), *T.Pos.ToString());
		// thrown past a bad guy within a metre: he flinches (backlog 1d; no dodge: where it lands is unchanged)
		if (!bGone && !T.bResting && L && T.Vel.SizeSquared() > FMath::Square(300.0))
		{
			for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
			{
				AOutlawsEnemy* E = Wk.Get();
				if (!E || E->IsDead() || E->bHeldByPlayer || T.Flinched.Contains(E) || FVector::DistSquared(E->GetActorLocation(), T.Pos) > FMath::Square(100.0)) continue;
				T.Flinched.Add(E);
				E->Flinch();
			}
		}
		if (!bGone && !T.bResting)
		{
			// the knife's aim assist: its way bent toward the chest of the bad guy it was thrown at, no more than 110
			// degrees a second (a near miss of a metre or so closes over the flight; the arc hardly changes); only once
			// it is past the top of its arc and coming down (user 2026-09-28: the rise looked like a homing missile)
			if ((T.Type == 2 || T.Type == 3 || T.Type == 4) && T.Homing.IsValid() && T.Vel.Z <= 0.0)
			{
				AOutlawsEnemy* Tg = T.Homing.Get();
				const float Speed = static_cast<float>(T.Vel.Size());
				if (Tg && !Tg->IsDead() && Speed > 1.f)
				{
					const FVector Aim = Tg->GetActorLocation() + FVector(0.0, 0.0, Tg->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() * 0.15);
					const FVector Cur = T.Vel / Speed;
					const FVector Want = (Aim - T.Pos).GetSafeNormal();
					const float Ang = FMath::Acos(FMath::Clamp(static_cast<float>(FVector::DotProduct(Cur, Want)), -1.f, 1.f));
					const float MaxTurn = FMath::DegreesToRadians(200.f) * FMath::Sqrt(T.Grav) * Dt;     // (paced with the flight; 200: up to a 4 m miss closes)
					if (Ang > 1e-4f)
					{
						T.Vel = FMath::Lerp(Cur, Want, FMath::Min(1.f, MaxTurn / Ang)).GetSafeNormal() * Speed;
					}
				}
			}
			T.Vel.Z -= (T.Pull > 0.f ? T.Pull : G) * T.Grav * Dt;
			const FVector Next = T.Pos + T.Vel * Dt;
			// falling into water: the small splash where it goes in (dynamite, a knife, a bottle)
			FVector InWater;
			if (L && Next.Z < T.Pos.Z && L->WaterSurfaceHit(T.Pos, Next, InWater)) L->SpawnSplash(InWater, false);
			FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsThrown), true, this);
			Q.AddIgnoredActor(A);
			FHitResult Hit;
			// a bottle meets the world by its lowest point (its middle is half its height up: let go just over a shelf it
			// would drop that much further, and burst, before its middle met the shelf)
			FVector Low = FVector::ZeroVector;
			if ((T.Type == 3 || T.Type == 4) && A)
			{
				if (const UPrimitiveComponent* Root = Cast<UPrimitiveComponent>(A->GetRootComponent()))
				{
					Low.Z = -FMath::Max(0.0, T.Pos.Z - Root->Bounds.GetBox().Min.Z);
				}
			}
			// through a window it goes, the pane breaking (user 2026-09-29: knives, bottles, anything thrown); a broken pane
			// has no collision left, so the trace below passes on through it
			if (L)
			{
				L->ShatterAlong(T.Pos, Next);
				if (!Low.IsZero()) L->ShatterAlong(T.Pos + Low, Next + Low);
			}
			// from its middle to its lowest point ahead (user 2026-09-30: thrown pans and rocks fell through the floor): the
			// lowest point's own line began under the floor once the spin swung it down, or once a bounce left the middle
			// on the floor, and met nothing
			// (M1's gaps) through another player's body in a match: a knife or a bottle is spent on it, a pot drops off it
			FVector StruckAt;
			if (L && (T.Type == 2 || T.Type == 3 || T.Type == 4) && !T.bResting && T.Vel.SizeSquared() > FMath::Square(300.0)
				&& UOutlawsCoop::SweepAtPlayers(L, T.Pos, Next, T.Damage * FMath::FRandRange(0.9f, 1.1f) * L->PlayerDamageScale(),
					T.Type == 2 ? TEXT("a thrown knife") : T.Type == 3 ? TEXT("a thrown bottle") : TEXT("a thrown pot"), StruckAt))
			{
				if (T.Type == 2) { L->SpawnGroundItem(TEXT("gknife"), StruckAt - FVector(0.0, 0.0, 60.0)); bGone = true; }
				else if (T.Type == 3) { if (A) L->ShatterGlass(Cast<UProceduralMeshComponent>(A->GetRootComponent()), T.Vel.GetSafeNormal(), T.BreakWav); bGone = true; }
				else { L->PlaySoundAt(T.BreakWav.IsEmpty() ? FString(TEXT("connect")) : T.BreakWav, StruckAt); T.Vel = -T.Vel.GetSafeNormal2D() * 80.0; }
			}
			else if (World->LineTraceSingleByChannel(Hit, T.Pos, Next + Low, ECC_Visibility, Q))
			{
				AOutlawsEnemy* E = Cast<AOutlawsEnemy>(Hit.GetActor());
				if (T.Type == 2)
				{
					// the knife sticks: a bad guy takes it, anything else and it drops as a gknife pickup
					// no knock-back from a thrown knife: the wound alone does the work (user 2026-09-28)
					if (E) { AOutlawsEnemy::GNextHitMelee = true; E->TakeHit(T.Damage * FMath::FRandRange(0.9f, 1.1f) * (L ? L->PlayerDamageScale() : 1.f), Hit.ImpactPoint, T.Vel.GetSafeNormal(), T.Pos, false, 0.f); }
					// the knife drops to the floor below where it struck (user 2026-09-28: it hung in the air where the bad
					// guy had been); the trace ignores the bad guy, so it falls through his body to the ground
					if (L)
					{
						const FVector Struck = FVector(Hit.ImpactPoint) + Hit.ImpactNormal * 2.0;
						FCollisionQueryParams FloorQ(SCENE_QUERY_STAT(OutlawsKnifeFloor), true, this);
						FloorQ.AddIgnoredActor(A);
						if (E) FloorQ.AddIgnoredActor(E);
						FHitResult Floor;
						const FVector Down = World->LineTraceSingleByChannel(Floor, Struck, Struck - FVector(0.0, 0.0, 2000.0), ECC_Visibility, FloorQ)
							? FVector(Floor.ImpactPoint) : Struck - FVector(0.0, 0.0, 10.0);
						L->SpawnGroundItem(TEXT("gknife"), Down);
					}
					bGone = true;
				}
				else if (T.Type == 3)
				{
					// a bottle (user 2026-09-28): into a bad guy it wounds him by his own hit places (the head x2: three to
					// the head fell a common one); struck hard on anything it bursts; set down gently it stands there
					if (E) E->TakeHit(T.Damage * FMath::FRandRange(0.9f, 1.1f) * (L ? L->PlayerDamageScale() : 1.f), Hit.ImpactPoint, T.Vel.GetSafeNormal(), T.Pos, false, 0.5f);
					// gently: no faster than a fall of 15 cm under the game's own gravity (150 units/s2, about 4.5 times
					// the real: a fixed 170 cm/s was an 11 cm drop there, and a bottle let go over its shelf burst)
					const double Gentle = FMath::Sqrt(2.0 * G * 15.0);
					if (E || T.Vel.Size() > Gentle || Hit.ImpactNormal.Z < 0.6)
					{
						if (L && A) L->ShatterGlass(Cast<UProceduralMeshComponent>(A->GetRootComponent()), E ? T.Vel.GetSafeNormal() : FVector(Hit.ImpactNormal), T.BreakWav);
						bGone = true;
					}
					else if (A)
					{
						A->SetActorRotation(FQuat::FindBetweenNormals(A->GetActorQuat().RotateVector(T.Axis), FVector::UpVector) * A->GetActorQuat());
						UPrimitiveComponent* Root = Cast<UPrimitiveComponent>(A->GetRootComponent());
						if (Root) Root->UpdateBounds();
						const double Bottom = Root ? Root->Bounds.GetBox().Min.Z : A->GetActorLocation().Z;
						T.Pos = A->GetActorLocation() + FVector(0.0, 0.0, Hit.ImpactPoint.Z + 0.2 - Bottom);
						T.Vel = FVector::ZeroVector;
						T.bResting = true;
					}
				}
				else if (T.Type == 4)
				{
					// a pot, a pan, the skull (user 2026-09-29): it hurts a bad guy it strikes, rings off what it hits
					// (its own sound), bounces and comes to rest standing
					if (E) E->TakeHit(T.Damage * FMath::FRandRange(0.9f, 1.1f) * (L ? L->PlayerDamageScale() : 1.f), Hit.ImpactPoint, T.Vel.GetSafeNormal(), T.Pos, false, 0.5f);
					// every knock more than a nudge, and a bad guy always, sounds as it does shot (user 2026-09-29)
					// (struck into, not skidding along: a skid rings once, not every frame)
					if (L && (E || FMath::Abs(FVector::DotProduct(T.Vel, FVector(Hit.ImpactNormal))) / FMath::Sqrt(T.Grav) > 110.0)) L->PlaySoundAt(T.BreakWav.IsEmpty() ? FString(TEXT("connect")) : T.BreakWav, Hit.ImpactPoint);
					// bounced as a hard thing does (user 2026-09-29): into the surface it keeps a third, rebounding (a steep throw hops
					// back up), along it seven tenths (a flat one skids on and hops low), at the game's own pace out of the aimed flight's
					// quicker one; a bad guy just knocks it back down
					const FVector N = E ? FVector(-T.Vel.GetSafeNormal2D()) : FVector(Hit.ImpactNormal);
					const FVector V0 = T.Vel / FMath::Sqrt(T.Grav);
					const double Into = FVector::DotProduct(V0, N);
					T.Vel = (V0 - N * Into) * (E ? 0.3f : 0.7f) - N * Into * (E ? 0.2f : 0.35f);
					T.Grav = 1.f;
					// its middle where it was when it struck, off the surface, its lowest point clear of a floor
					T.Pos = FMath::Lerp(T.Pos, Next, double(Hit.Time)) + FVector(Hit.ImpactNormal) * 2.0;
					if (!E && Hit.ImpactNormal.Z > 0.6f) T.Pos.Z = FMath::Max(T.Pos.Z, Hit.ImpactPoint.Z - Low.Z + 2.0);
					T.Spin.Pitch *= 0.6f;
					// on a floor, once the hop left is a few centimetres (a rebound under 110 cm/s in the game's 4.5 g), it settles there
					if (!E && Hit.ImpactNormal.Z > 0.6f && FMath::Abs(Into) * 0.35 < 110.0 && (V0 - N * Into).Size() * 0.7 < 250.0 && A)
					{
						RestOn(A, T, Hit.ImpactPoint.Z);
					}
				}
				else
				{
					// dynamite bounces (ALLOW_BOUNCE), losing most of its speed, and comes to rest
					T.Vel = FMath::GetReflectionVector(T.Vel, Hit.ImpactNormal) * 0.35f / FMath::Sqrt(T.Grav);
					T.Grav = 1.f;
					T.Pos = FVector(Hit.Location) + Hit.ImpactNormal * 3.0;
					if (Hit.ImpactNormal.Z > 0.6f && T.Vel.Size() < 80.f) T.bResting = true;
				}
			}
			else
			{
				T.Pos = Next;
			}
			if (A && !bGone)
			{
				A->SetActorLocation(T.Pos);
				if (!T.bResting)
				{
					// a front flip: turning about the flight's right (Up x Ahead), the top going forward and down
					FVector Ahead(T.Vel.X, T.Vel.Y, 0.0);
					if (!Ahead.Normalize()) Ahead = FVector(A->GetActorForwardVector().X, A->GetActorForwardVector().Y, 0.0).GetSafeNormal();
					const FVector Axis = FVector::CrossProduct(FVector::UpVector, Ahead).GetSafeNormal();
					if (!Axis.IsNearlyZero()) A->AddActorWorldRotation(FQuat(Axis, FMath::DegreesToRadians(T.Spin.Pitch) * Dt));
				}
			}
		}
		if (!bGone && A && T.GrowTo != 1.f)
		{
			// the item's size, grown into smoothly over a second as it leaves the hand (user 2026-09-29: thrown sticks should
			// be the size of the sticks lying in the levels; 2026-10-01: knives, rocks and pots too, over one second); come to
			// rest it grows up off the floor, its lowest point kept where it lies
			const FVector Want = T.Scale0 * FMath::InterpEaseInOut(1.f, T.GrowTo, FMath::Clamp(T.Age / 1.f, 0.f, 1.f), 2.f);
			if (!A->GetActorScale3D().Equals(Want, 1e-4))
			{
				UPrimitiveComponent* Root = Cast<UPrimitiveComponent>(A->GetRootComponent());
				const double Before = Root ? Root->Bounds.GetBox().Min.Z : 0.0;
				A->SetActorScale3D(Want);
				if (T.bResting && Root)
				{
					T.Pos.Z += Before - Root->Bounds.GetBox().Min.Z;
					A->SetActorLocation(T.Pos);
				}
			}
		}
		if (!bGone && T.Type == 1 && A)
		{
			if (T.bResting && !T.bLaid)
			{
				// at rest it lies along the floor, on its side (it stood up in the fist: its length is its up)
				T.bLaid = true;
				FVector Along = A->GetActorUpVector();
				Along.Z = 0.0;
				if (!Along.Normalize()) Along = FVector(A->GetActorForwardVector().X, A->GetActorForwardVector().Y, 0.0).GetSafeNormal();
				if (!Along.IsNearlyZero()) A->SetActorRotation(FQuat::FindBetweenNormals(A->GetActorUpVector(), Along) * A->GetActorQuat());
				const FBox B = A->GetComponentsBoundingBox(true);
				T.Pos += FVector(0.0, 0.0, FMath::Max(0.0, T.Pos.Z - B.Min.Z) + 0.5 - 3.0);
				A->SetActorLocation(T.Pos);
			}
		}
		if (!bGone && T.Type == 1 && T.bDud)
		{
			// unlit: at rest it is a stick on the floor again (gdynam), walked over or taken by hand (user 2026-09-29)
			if (T.bLaid && L)
			{
				FCollisionQueryParams FQ(SCENE_QUERY_STAT(OutlawsDudFloor), true, this);
				if (A) FQ.AddIgnoredActor(A);
				FHitResult Floor;
				const FVector Down = World->LineTraceSingleByChannel(Floor, T.Pos + FVector(0.0, 0.0, 20.0), T.Pos - FVector(0.0, 0.0, 200.0), ECC_Visibility, FQ)
					? FVector(Floor.ImpactPoint) : T.Pos;
				if (AActor* Stick = L->SpawnGroundItem(TEXT("gdynam"), Down))
				{
					L->HoldPickupTillClear(Stick);
					L->AddLooseDynamite(Stick);       // (a blast or a shot sets it off as the level's own sticks)
				}
				UE_LOG(LogOutlaws, Log, TEXT("OLRT an unlit stick comes to rest: back on the ground"));
				bGone = true;
			}
		}
		else if (!bGone && T.Type == 1)
		{
			if (T.Sound.IsValid()) T.Sound->SetWorldLocation(T.Pos);
			T.Fuse -= Dt;
			if (A && T.Flame.IsValid())
			{
				PlaceFuse(Cast<UProceduralMeshComponent>(A->GetRootComponent()), T.FuseSection, T.WickPos, T.WickNrm, T.WickUV, T.WickCol,
					FMath::Clamp(T.Fuse / 4.5f, 0.04f, 1.f), true, T.Flame.Get(), T.Core.Get(), 1.8f);     // bigger: seen from metres off
			}
			if (T.Fuse <= 0.f)
			{
				if (T.Sound.IsValid()) T.Sound->Stop();
				if (L) L->Explode(T.Pos);
				bGone = true;
			}
		}
		if (bGone)
		{
			if (T.Sound.IsValid()) T.Sound->Stop();
			if (A) A->Destroy();
			Thrown.RemoveAt(i);
		}
	}
}

// ---- bottles (user 2026-09-28) --------------------------------------------------------------------------------------
// A hand closing on a bottle takes it: a copy of its model at the real size (the level's stand as big as their sprites),
// shrunk about where the hand closed so it stays in the fist, the prop gone from the shelf.  Let go it flies with the
// hand; set down gently it stands where it came to rest; on anything harder (a bad guy, a wall, a floor from a height)
// it bursts.  Swung into a bad guy it lands as a melee blow and bursts over him.

bool AOutlawsPlayer::TryHoldBottle(int32 Hand, int32 PickThrown, AOutlawsSpriteActor* PickProp)
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L || HeldBottle[Hand])
	{
		return false;
	}
	const FVector At = GripOf(Hand)->GetComponentLocation();
	UProceduralMeshComponent* Src = nullptr;
	AActor* Old = nullptr;
	FString Wav;
	FString HoldMode;                          // its "hold" trait (traits.py), empty for the shape to decide
	float Scale = 1.f;
	FVector Axis = FVector::UpVector;          // the bottle's own up, world
	// one set down earlier first (a thrown bottle at rest: already real size), the nearest, else one of the level's
	// (or the one the reach picked)
	if (PickThrown < 0 && !PickProp)
	{
		double BestD = BottleReach;
		for (int32 i = 0; i < Thrown.Num(); ++i)
		{
			const FThrown& T = Thrown[i];
			const AActor* A = T.Actor.Get();
			const UPrimitiveComponent* M = A ? Cast<UPrimitiveComponent>(A->GetRootComponent()) : nullptr;
			if ((T.Type != 3 && T.Type != 4) || !T.bResting || !M) continue;
			const double D = FVector::Dist(M->Bounds.GetBox().GetClosestPointTo(At), At);
			if (D < BestD)
			{
				BestD = D;
				PickThrown = i;
			}
		}
	}
	for (int32 i = 0; i < Thrown.Num(); ++i)
	{
		FThrown& T = Thrown[i];
		AActor* A = T.Actor.Get();
		UProceduralMeshComponent* M = A ? Cast<UProceduralMeshComponent>(A->GetRootComponent()) : nullptr;
		if (i != PickThrown || !M) continue;
		Src = M;
		Old = A;
		Wav = T.BreakWav;
		HoldMode = T.Hold;
		Axis = A->GetActorQuat().RotateVector(T.Axis);
		bHeldBreaks[Hand] = T.Type == 3;
		// grown back to its level size (or part way): shrunk to the hand's again, to grow back once it goes
		Scale = float(A->GetActorScale3D().X / FMath::Max(1e-3, T.Scale0.X));
		BottleGrow[Hand] = T.GrowTo;
		Thrown.RemoveAt(i);
		break;
	}
	if (!Src && PickThrown < 0)
	{
		bHeldBreaks[Hand] = true;
		bool bBreaks = true;
		AOutlawsSpriteActor* S = PickProp ? L->TakeBottle(PickProp, Wav, Scale, bBreaks) : L->TakeBottleNear(At, BottleReach, Wav, Scale, bBreaks);
		HoldMode = S ? AOutlawsLevelActor::HoldModeFor(S->ModelStem) : FString();
		Src = S ? Cast<UProceduralMeshComponent>(S->Model) : nullptr;
		Old = S;
		bHeldBreaks[Hand] = bBreaks;
		BottleGrow[Hand] = FMath::Max(0.2f, Scale);      // (the copy below is 1 / that of its level size)
	}
	if (!Src)
	{
		if (Old) Old->Destroy();
		return false;
	}
	StowForBottle(Hand);          // a weapon or a tool in this hand steps aside for it
	// the copy in the bottle's own frame: its middle at the origin, its up along +Z, the real size (the shelf's stand as
	// big as their sprites)
	const FTransform SrcX = Src->GetComponentTransform();
	const FVector Centre = Src->Bounds.Origin;
	const FQuat ToUp = FQuat::FindBetweenNormals(Axis.GetSafeNormal(), FVector::UpVector);
	const double K = 1.0 / FMath::Max(0.2f, Scale);
	UProceduralMeshComponent* M = NewObject<UProceduralMeshComponent>(this);
	M->bUseAsyncCooking = false;
	M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	M->SetCastShadow(false);
	M->SetupAttachment(FrameOf(Hand));
	M->RegisterComponent();
	for (int32 i = 0; i < Src->GetNumSections(); ++i)
	{
		const FProcMeshSection* S = Src->GetProcMeshSection(i);
		if (!S) continue;
		FProcMeshSection Copy = *S;
		Copy.bEnableCollision = false;
		Copy.SectionLocalBox = FBox(ForceInit);
		for (FProcMeshVertex& V : Copy.ProcVertexBuffer)
		{
			V.Position = ToUp.RotateVector(SrcX.TransformPosition(V.Position) - Centre) * K;
			V.Normal = ToUp.RotateVector(SrcX.TransformVectorNoScale(V.Normal));
			Copy.SectionLocalBox += V.Position;
		}
		M->SetProcMeshSection(i, Copy);
		M->SetMaterial(i, Src->GetMaterial(i));
	}
	// it starts where it stood, as big as it stood, and flies into the fist (UpdateBottles; user 2026-09-28: a smooth
	// but quick zoom to the hand, held as the dynamite is)
	M->SetWorldTransform(FTransform(ToUp.Inverse(), Centre, FVector(1.0 / K)));
	BottleZoomFrom[Hand] = M->GetRelativeTransform();
	// held as it would be (user 2026-10-01, after 2026-09-29's "where the hand closed on it"): a bottle by its neck, a pan
	// by its handle, a stone in the palm (HoldPoseFor)
	BottleZoomTo[Hand] = HoldPoseFor(Hand, M, HoldMode);
	BottleHold[Hand] = HoldMode;
	BottleZoom[Hand] = 0.f;
	HeldBottle[Hand] = M;
	BottleSound[Hand] = Wav;
	BottleAxis[Hand] = FVector::UpVector;      // (in its own frame)
	PrevBottlePos[Hand] = M->Bounds.Origin;
	BottleClear = L->GetClearCount();
	bBottleReady[Hand] = false;
	if (Old) Old->Destroy();
	Pulse(Hand, 0.4f);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT %s hand took a bottle"), Hand == 0 ? TEXT("left") : TEXT("right"));
	return true;
}

const TCHAR* AOutlawsPlayer::HoldModeOf(const TArray<FVector>& P, double* OutRmax, double* OutRmed, double* OutH)
{
	if (P.Num() < 8) return TEXT("palm");
	FBox B(ForceInit);
	FVector2D C(0.0, 0.0);
	for (const FVector& V : P)
	{
		B += V;
		C += FVector2D(V.X, V.Y);
	}
	C /= double(P.Num());
	TArray<double> R;
	R.Reserve(P.Num());
	double Rmax = 0.0;
	for (const FVector& V : P)
	{
		R.Add(FVector2D::Distance(FVector2D(V.X, V.Y), C));
		Rmax = FMath::Max(Rmax, R.Last());
	}
	R.Sort();
	const double Rmed = FMath::Max(0.1, R[R.Num() / 2]);
	const double H = FMath::Max(0.1, B.Max.Z - B.Min.Z);
	if (OutRmax) *OutRmax = Rmax;
	if (OutRmed) *OutRmed = Rmed;
	if (OutH) *OutH = H;
	if (Rmax > 1.8 * Rmed && Rmax > 0.6 * H) return TEXT("handle");
	if (H > 2.6 * Rmed) return TEXT("neck");
	return TEXT("palm");
}

FTransform AOutlawsPlayer::HoldPoseFor(int32 Hand, const UProceduralMeshComponent* M, const FString& Mode) const
{
	const FTransform Frame = FrameOf(Hand)->GetComponentTransform();
	const UOutlawsBoxHand* Box = Hand == 0 ? LeftHand.Get() : RightHand.Get();
	TArray<FVector> P;
	for (int32 i = 0; M && i < M->GetNumSections(); ++i)
	{
		if (const FProcMeshSection* S = const_cast<UProceduralMeshComponent*>(M)->GetProcMeshSection(i)) for (const FProcMeshVertex& V : S->ProcVertexBuffer) P.Add(V.Position);
	}
	if (!Box || P.Num() < 8) return FTransform::Identity;
	// the shape: its middle in plan (the mean of its corners: a handle has few), how far its corners reach from it
	FBox B(ForceInit);
	FVector2D C(0.0, 0.0);
	for (const FVector& V : P)
	{
		B += V;
		C += FVector2D(V.X, V.Y);
	}
	C /= double(P.Num());
	TArray<double> R;
	R.Reserve(P.Num());
	int32 Far = 0;
	for (int32 i = 0; i < P.Num(); ++i)
	{
		R.Add(FVector2D::Distance(FVector2D(P[i].X, P[i].Y), C));
		if (R[i] > R[Far]) Far = i;
	}
	TArray<double> Sorted = R;
	Sorted.Sort();
	const double Rmed = FMath::Max(0.1, Sorted[Sorted.Num() / 2]), Rmax = R[Far];
	const double H = FMath::Max(0.1, B.Max.Z - B.Min.Z);
	const FString Shape = HoldModeOf(P);
	const FString HoldAs = Mode.IsEmpty() ? Shape : Mode;              // (the json's word over the shape's)
	// in the hand's own frame (the box hand's: X along the fingers, Z up through the fist, the palm facing S x Y)
	const double S = Hand == 1 ? -1.0 : 1.0;
	const FVector Palm(0.0, S, 0.0);
	const FVector Fist(2.0, Hand == 0 ? -0.5 : 0.5, 0.0);
	FVector G(C.X, C.Y, (B.Min.Z + B.Max.Z) * 0.5);
	FQuat Q = FQuat::Identity;
	const TCHAR* How = TEXT("in the palm");
	if (HoldAs == TEXT("handle"))
	{
		// a handle (the pan's, a pot's, the skull's horn): taken 40 to 80 percent of the way out along it; the handle runs
		// up through the fist into the body above it, the inside facing the other palm
		How = TEXT("by the handle");
		const FVector D = FVector(P[Far].X - C.X, P[Far].Y - C.Y, 0.0).GetSafeNormal();
		const double Rb = Rmed * 1.15, Ln = FMath::Max(0.1, Rmax - Rb);
		FVector Sum = FVector::ZeroVector;
		int32 N = 0;
		for (const FVector& V : P)
		{
			const double Along = FVector::DotProduct(V - FVector(C.X, C.Y, 0.0), D);
			if (Along > Rb + 0.4 * Ln && Along < Rb + 0.8 * Ln) { Sum += V; ++N; }
		}
		G = N > 0 ? Sum / N : FVector(C.X, C.Y, B.Min.Z) + D * (Rb + 0.6 * Ln);
		const FVector A = -D, U = FVector::UpVector, X3 = FVector::CrossProduct(A, U);
		const FVector TA = FVector::UpVector, TB = Palm, TC = FVector::CrossProduct(TA, TB);
		const FMatrix Src(FPlane(A, 0.0), FPlane(U, 0.0), FPlane(X3, 0.0), FPlane(0.0, 0.0, 0.0, 1.0));
		const FMatrix Tgt(FPlane(TA, 0.0), FPlane(TB, 0.0), FPlane(TC, 0.0), FPlane(0.0, 0.0, 0.0, 1.0));
		Q = FQuat(Src.GetTransposed() * Tgt);
	}
	else if (HoldAs == TEXT("neck"))
	{
		// tall (a bottle, a vase, a jug): by the neck, the narrowest it is in its upper part, upright with its body under
		// the fist
		How = TEXT("by the neck");
		constexpr int32 Bins = 24;
		double Wide[Bins];
		for (double& Wd : Wide) Wd = 0.0;
		for (int32 i = 0; i < P.Num(); ++i)
		{
			const int32 Bn = FMath::Clamp(int32((P[i].Z - B.Min.Z) / H * Bins), 0, Bins - 1);
			Wide[Bn] = FMath::Max(Wide[Bn], R[i]);
		}
		int32 Neck = int32(Bins * 0.75);
		for (int32 Bn = int32(Bins * 0.55); Bn <= int32(Bins * 0.92); ++Bn)
		{
			if (Wide[Bn] > 0.0 && (Wide[Neck] <= 0.0 || Wide[Bn] < Wide[Neck])) Neck = Bn;
		}
		G = FVector(C.X, C.Y, B.Min.Z + (Neck + 0.5) * H / Bins);
	}
	const FTransform InBox(Q, Fist - Q.RotateVector(G));
	UE_LOG(LogOutlaws, Log, TEXT("OLRT held %s (reach %.1f / %.1f cm, %.1f tall%s)"), How, Rmax, Rmed, H, Mode.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", its trait %s, the shape's %s"), *Mode, *Shape));
	// (at the hand frame's own scale: the box hand's is not one, and a held copy taken at it was baked a little bigger into
	// the thrown prop each throw, user 2026-10-01: the kettle grew every time)
	FTransform Rel = (InBox * Box->GetComponentTransform()).GetRelativeTransform(Frame);
	Rel.SetScale3D(FVector::OneVector);
	return Rel;
}

void AOutlawsPlayer::SetBottleGhost(int32 Hand, bool bOn)
{
	UProceduralMeshComponent* B = HeldBottle[Hand];
	TArray<TObjectPtr<UMaterialInterface>>& Saved = Hand == 0 ? GhostSaved0 : GhostSaved1;
	if (bBottleGhost[Hand] && GhostOf[Hand].Get() != B)
	{
		bBottleGhost[Hand] = false;        // (that one broke or went with the level: nothing to put back)
		Saved.Reset();
	}
	if (!B || bBottleGhost[Hand] == bOn) return;
	GhostOf[Hand] = B;
	if (bOn)
	{
		UMaterialInterface* Ghost = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_HeldGhost.M_OLR_HeldGhost"));
		if (!Ghost) return;
		Saved.Reset();
		for (int32 i = 0; i < B->GetNumMaterials(); ++i)
		{
			UMaterialInterface* Own = B->GetMaterial(i);
			Saved.Add(Own);
			UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(Ghost, this);
			UTexture* T = nullptr;
			if (Own && Own->GetTextureParameterValue(FHashedMaterialParameterInfo(TEXT("Tex")), T) && T) M->SetTextureParameterValue(TEXT("Tex"), T);
			B->SetMaterial(i, M);
		}
	}
	else
	{
		for (int32 i = 0; i < Saved.Num() && i < B->GetNumMaterials(); ++i) B->SetMaterial(i, Saved[i]);
		Saved.Reset();
	}
	bBottleGhost[Hand] = bOn;
}

void AOutlawsPlayer::ReleaseBottle(int32 Hand, bool bThrow)
{
	SetBottleGhost(Hand, false);           // (its own look back before it leaves the hand)
	UProceduralMeshComponent* Held = HeldBottle[Hand];
	HeldBottle[Hand] = nullptr;
	bBottleReady[Hand] = false;
	ShowThrowArc(Hand, false);
	UWorld* World = GetWorld();
	if (!Held)
	{
		return;
	}
	if (!World)
	{
		Held->DestroyComponent();
		return;
	}
	// its own actor, its origin at the bottle's middle (it turns about that in flight)
	const FVector Mid = Held->Bounds.Origin;
	const FTransform HeldX = Held->GetComponentTransform();
	const FTransform AX(HeldX.GetRotation(), Mid);
	FActorSpawnParameters SP;
	SP.ObjectFlags |= RF_Transient;
	AActor* A = World->SpawnActor<AActor>(AActor::StaticClass(), AX, SP);
	if (!A)
	{
		Held->DestroyComponent();
		return;
	}
	UProceduralMeshComponent* M = NewObject<UProceduralMeshComponent>(A);
	M->bUseAsyncCooking = false;
	M->SetCastShadow(false);
	A->SetRootComponent(M);
	M->RegisterComponent();
	A->SetActorTransform(AX);
	for (int32 i = 0; i < Held->GetNumSections(); ++i)
	{
		const FProcMeshSection* S = Held->GetProcMeshSection(i);
		if (!S) continue;
		FProcMeshSection Copy = *S;
		Copy.SectionLocalBox = FBox(ForceInit);
		for (FProcMeshVertex& V : Copy.ProcVertexBuffer)
		{
			V.Position = AX.InverseTransformPosition(HeldX.TransformPosition(V.Position));
			Copy.SectionLocalBox += V.Position;
		}
		// a shot finds it, in flight or set down (the shots' channel only)
		Copy.bEnableCollision = true;
		M->SetProcMeshSection(i, Copy);
		M->SetMaterial(i, Held->GetMaterial(i));
	}
	M->bUseComplexAsSimpleCollision = true;
	M->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	M->SetCollisionResponseToAllChannels(ECR_Ignore);
	M->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
	FThrown T;
	T.Actor = A;
	T.Pos = Mid;
	// thrown: as the knife goes (the hand's height sets the power, the wrist steers a little; the arc showed where),
	// and bent onto the bad guy it was thrown at once it comes down; let go: a toss, the hand's own swing (TossVelocity)
	T.Vel = bThrow ? ThrowVelocity(Hand) : TossVelocity(Hand, 1.f);
	T.Pull = ThrowGravity(Mid);
	if (bThrow)
	{
		T.Homing = KnifeTargetNear(Mid, T.Vel);
		if (AOutlawsLevelActor* L = Level.Get()) L->PlaySoundAt(TEXT("throw"), Mid);
		Pulse(Hand, 0.5f);
	}
	else if (T.Vel.Size() > 250.0)
	{
		// tossed hard with the grip alone it homes as the knife does too (user 2026-09-30): the target found along its
		// own flight (KnifeTargetNear steps the same fall)
		T.Homing = KnifeTargetNear(Mid, T.Vel);
	}
	if (AOutlawsEnemy* Tg = T.Homing.Get()) UE_LOG(LogOutlaws, Log, TEXT("OLRT thrown prop homing on %s"), *Tg->GetName());
	T.Type = bHeldBreaks[Hand] ? 3 : 4;
	// a pot, a pan, a rock, the skull grows back to the size it had in the level over a second (user 2026-10-01)
	T.Scale0 = FVector::OneVector;
	T.GrowTo = T.Type == 4 ? BottleGrow[Hand] : 1.f;
	T.Damage = BottleThrowDamage;
	T.BreakWav = BottleSound[Hand];
	T.Hold = BottleHold[Hand];
	T.Axis = BottleAxis[Hand];               // (the actor turns as the hand did: the same frame)
	// thrown (or tossed hard) it turns end over end as fast as the knife does; let go it just falls (user 2026-09-28)
	T.Spin = FRotator(bThrow || T.Vel.Size() > 250.0 ? FMath::FRandRange(560.f, 700.f) : 0.f, 0.f, 0.f);
	Thrown.Add(T);
	if (AOutlawsLevelActor* CoopL = Level.Get(); CoopL && UOutlawsCoop::IsActive(CoopL)) UOutlawsCoop::NoteThrow(CoopL, T.Type, T.Pos, T.Vel, T.Pull / 980.f, T.Fuse);
	Held->DestroyComponent();
	UE_LOG(LogOutlaws, Log, TEXT("OLRT let go of a bottle at %.0f cm/s"), T.Vel.Size());
	RestoreStowed(Hand);
}

void AOutlawsPlayer::UpdateHandWater(float Dt)
{
	OL_PERF_SCOPE("Player: hand water");
	AOutlawsLevelActor* L = Level.Get();
	UWorld* World = GetWorld();
	if (!L || !World || !bVR) return;
	const FVector Eye = Camera->GetComponentLocation();
	bool bAcross = false;
	float SurfaceZ = 0.f;
	for (int32 H = 0; H < 2; ++H)
	{
		const FVector Hand = GripOf(H)->GetComponentLocation();
		HandSplashCool[H] = FMath::Max(0.f, HandSplashCool[H] - Dt);
		// a quick slap or push through the surface, either way: the small splash, and the hand feels it
		FVector At;
		if (!PrevHandWater[H].IsZero() && HandSplashCool[H] <= 0.f && FMath::Abs(HandWorldVel[H].Z) > 110.f && HandWorldVel[H].Size() > 150.f
			&& L->WaterSurfaceHit(PrevHandWater[H], Hand, At))
		{
			L->SpawnSplash(At, false, true, 0.5f, 0.1f);
			Pulse(H, 0.5f);
			HandSplashCool[H] = 0.35f;
		}
		PrevHandWater[H] = Hand;
		// (BUG-WATERHANDS) the hand itself under the surface, the eyes over it: it dims (UpdateHeldLight)
		FVector Under;
		HandWaterWant[H] = L->WaterSurfaceHit(Eye, Hand, Under) && Hand.Z < Under.Z ? 0.55f : 1.f;
		// the eyes on one side of a water surface, the hand (or the far end of what it holds) on the other
		for (const FVector& P : { Hand, Hand + FrameOf(H)->GetForwardVector() * 70.0 })
		{
			if (!bAcross && L->WaterSurfaceHit(Eye, P, At))
			{
				bAcross = true;
				SurfaceZ = float(At.Z);
			}
		}
	}
	SetHandWater(bAcross, SurfaceZ);
}

void AOutlawsPlayer::SetHandWater(bool bAcross, float SurfaceZ)
{
	// the held gear's material (M_OLR_Held's OLNearPull): a part on the far side of the surface keeps its true depth
	UWorld* World = GetWorld();
	if (!World) return;
	if (!ToneMpc) ToneMpc = LoadObject<UMaterialParameterCollection>(nullptr, TEXT("/Game/Outlaws/Runtime/MPC_OLR_Lights.MPC_OLR_Lights"));
	if (UMaterialParameterCollectionInstance* Mpc = ToneMpc ? World->GetParameterCollectionInstance(ToneMpc) : nullptr)
	{
		Mpc->SetScalarParameterValue(TEXT("WaterOn"), bAcross ? 1.f : 0.f);
		if (bAcross) Mpc->SetScalarParameterValue(TEXT("WaterZ"), SurfaceZ);
	}
}

void AOutlawsPlayer::StowForBottle(int32 Hand)
{
	StowWeapon[Hand] = -1;
	StowTool[Hand].Reset();
	StowMovedTo[Hand] = -1;
	if (Equipped[Hand] < 0 && HeldTool[Hand].IsEmpty()) return;
	const int32 O = 1 - Hand;
	const bool bOtherFree = Equipped[O] < 0 && HeldTool[O].IsEmpty() && !HeldBottle[O] && !HeldItem[O].IsValid() && GrabHand != O;
	StowWeapon[Hand] = Equipped[Hand];
	StowTool[Hand] = HeldTool[Hand];
	StowMovedTo[Hand] = bOtherFree ? O : -1;
	if (StowWeapon[Hand] >= 0)
	{
		if (bOtherFree) SwapWeaponHands(Hand, O);       // rounds and all
		else Equip(Hand, -1);                           // put away (it keeps its rounds)
	}
	else
	{
		const FString T = StowTool[Hand];
		if (bOtherFree) EquipTool(O, T);                // (takes it out of this hand)
		else DropTool(Hand);
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT the %s hand takes a bottle: its %s %s"), Hand == 0 ? TEXT("left") : TEXT("right"),
		StowWeapon[Hand] >= 0 ? GWeapons[StowWeapon[Hand]].Label : *StowTool[Hand], bOtherFree ? TEXT("goes to the other hand") : TEXT("is put away"));
}

void AOutlawsPlayer::RestoreStowed(int32 Hand)
{
	const int32 W = StowWeapon[Hand];
	const FString T = StowTool[Hand];
	const int32 To = StowMovedTo[Hand];
	StowWeapon[Hand] = -1;
	StowTool[Hand].Reset();
	StowMovedTo[Hand] = -1;
	if (W < 0 && T.IsEmpty()) return;
	// only into a hand left empty (something drawn into it meanwhile, a ledge held, death: it stays as it is)
	if (Equipped[Hand] >= 0 || !HeldTool[Hand].IsEmpty() || HeldBottle[Hand] || GrabHand == Hand || bDeadView) return;
	if (W >= 0)
	{
		if (To < 0) Equip(Hand, W);
		else if (Equipped[To] == W) SwapWeaponHands(To, Hand);       // back from the other hand, which is free again
	}
	else if (To < 0 || HeldTool[To] == T)
	{
		EquipTool(Hand, T);                                          // (out of the other hand, if it went there)
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT the bottle gone: the %s hand has its %s back"), Hand == 0 ? TEXT("left") : TEXT("right"), W >= 0 ? GWeapons[W].Label : *T);
}

bool AOutlawsPlayer::PointedNear(int32 Hand, const FVector& A, const FVector& B, double Slack, double& OutScore, FVector& OutOn) const
{
	// loosely (user 2026-09-29: it wanted a precise aim): within 45 cm (plus the thing's own half size, Slack) and a
	// quarter of the way out of the line, along where the open palm faces (user 2026-09-30: the palm, not the pointing,
	// and 2 m at most from the fist: the wrist's angle had it reaching 4 m)
	constexpr double MaxReach = 200.0;
	const FVector Fist = GripOf(Hand)->GetComponentLocation();
	const FVector Point = PalmDir(Hand);
	FVector OnRay;
	FMath::SegmentDistToSegmentSafe(Fist, Fist + Point * MaxReach, A, B, OnRay, OutOn);
	const double Along = FVector::Dist(Fist, OnRay);
	const double Miss = FMath::Max(0.0, FVector::Dist(OnRay, OutOn) - Slack);
	OutScore = 60.0 + Along + Miss * 3.0;
	return Along > 1.0 && Along < MaxReach && Miss < 45.0 + 0.25 * Along && FVector::Dist(Fist, OutOn) - Slack <= MaxReach;
}

AOutlawsEnemy* AOutlawsPlayer::FindGrabEnemy(int32 Hand, bool bRemote, double& OutScore) const
{
	AOutlawsLevelActor* L = Level.Get();
	OutScore = TNumericLimits<double>::Max();
	if (!L) return nullptr;
	const FVector Fist = GripOf(Hand)->GetComponentLocation();
	AOutlawsEnemy* Best = nullptr;
	UWorld* World = GetWorld();
	for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
	{
		AOutlawsEnemy* E = Wk.Get();
		if (!E || !E->CanBeGrabbed()) continue;
		// loosely (user 2026-09-29: it wanted a precise aim at his chest): anywhere about his body, his whole height from
		// the feet to the head, not one point; the fist within 60 cm of it, or the open palm facing him up to 2 m off
		// (PointedNear) with nothing between (user 2026-09-29: grabbed from a couple of metres, snapped to the hand)
		const double Half = E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		const FVector Mid = E->GetActorLocation();
		const FVector Foot = Mid - FVector(0.0, 0.0, Half * 0.85), Head = Mid + FVector(0.0, 0.0, Half);
		const double D = FVector::Dist(Fist, FMath::ClosestPointOnSegment(Fist, Foot, Head));
		const bool bNear = !bRemote && D < 60.0;
		double ReachScore = 0.0;
		FVector OnBody;
		const bool bReach = bRemote && PointedNear(Hand, Foot, Head, 0.0, ReachScore, OnBody);     // (2 m: user 2026-09-30)
		if (!bNear && !bReach) continue;
		if (!bNear && World)
		{
			// seen from the fist: the nearest of his body to the pointing, or his chest
			FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsGrabReach), false, this);
			Q.AddIgnoredActor(E);
			FCollisionResponseParams RP;
			RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
			if (World->LineTraceTestByChannel(Fist, OnBody, ECC_Visibility, Q, RP)
				&& World->LineTraceTestByChannel(Fist, E->GrabChest(), ECC_Visibility, Q, RP)) continue;
		}
		const double Score = bNear ? D : ReachScore;
		if (Score < OutScore)
		{
			OutScore = Score;
			Best = E;
		}
	}
	return Best;
}

bool AOutlawsPlayer::TryReachGrab(int32 Hand)
{
	// the best pointed of the bad guys, the bottles and props: one pick for them all (user 2026-09-29)
	if (!Level.IsValid() || !GetWorld() || HeldBottle[Hand] || GrabbedEnemy[Hand].IsValid()) return false;
	// (a hand holding a stick: its grip lets the stick go, user 2026-09-29; no reaching past it)
	if (Equipped[Hand] >= 0 && GWeapons[Equipped[Hand]].Throw == 1) return false;
	AOutlawsEnemy* E = nullptr;
	int32 PickThrown = -1;
	AOutlawsSpriteActor* PickProp = nullptr;
	FindReachGrab(Hand, E, PickThrown, PickProp);
	if (PickProp || PickThrown >= 0) return TryHoldBottle(Hand, PickThrown, PickProp);
	return E && TryGrabEnemy(Hand, E);
}

void AOutlawsPlayer::FindReachGrab(int32 Hand, AOutlawsEnemy*& OutEnemy, int32& OutThrown, AOutlawsSpriteActor*& OutProp) const
{
	OutEnemy = nullptr;
	OutThrown = -1;
	OutProp = nullptr;
	AOutlawsLevelActor* L = Level.Get();
	UWorld* World = GetWorld();
	if (!L || !World) return;
	double EnemyScore = TNumericLimits<double>::Max();
	AOutlawsEnemy* E = FindGrabEnemy(Hand, true, EnemyScore);
	const FVector Fist = GripOf(Hand)->GetComponentLocation();
	double BestScore = EnemyScore;
	int32 PickThrown = -1;
	AOutlawsSpriteActor* PickProp = nullptr;
	// in sight of the fist: nothing between but the thing itself (its own box a little grown)
	auto InSight = [&](const FBox& Box, const FVector& On, const AActor* Self)
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsReachProp), true, this);
		FCollisionResponseParams RP;
		RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
		FHitResult Hit;
		if (!World->LineTraceSingleByChannel(Hit, Fist, On, ECC_Visibility, Q, RP)) return true;
		return Hit.GetActor() == Self || Box.ExpandBy(8.0).IsInside(Hit.ImpactPoint);
	};
	for (int32 i = 0; i < Thrown.Num(); ++i)
	{
		const FThrown& T = Thrown[i];
		const AActor* A = T.Actor.Get();
		const UPrimitiveComponent* M = A ? Cast<UPrimitiveComponent>(A->GetRootComponent()) : nullptr;
		if ((T.Type != 3 && T.Type != 4) || !T.bResting || !M) continue;
		const FBox Box = M->Bounds.GetBox();
		double S = 0.0;
		FVector On;
		if (PointedNear(Hand, Box.GetCenter(), Box.GetCenter(), Box.GetExtent().Size() * 0.7, S, On) && S < BestScore && InSight(Box, On, A))
		{
			BestScore = S;
			PickThrown = i;
		}
	}
	TArray<AOutlawsSpriteActor*> Props;
	L->GrabbableProps(Props);
	for (AOutlawsSpriteActor* P : Props)
	{
		const FBox Box = P->Model->Bounds.GetBox();
		double S = 0.0;
		FVector On;
		if (PointedNear(Hand, Box.GetCenter(), Box.GetCenter(), Box.GetExtent().Size() * 0.7, S, On) && S < BestScore && InSight(Box, On, P))
		{
			BestScore = S;
			PickThrown = -1;
			PickProp = P;
		}
	}
	OutThrown = PickThrown;
	OutProp = PickProp;
	OutEnemy = PickProp || PickThrown >= 0 ? nullptr : E;
}

bool AOutlawsPlayer::PreviewGrab(int32 Hand, FGrabPick& Out)
{
	Out = FGrabPick();
	AOutlawsLevelActor* L = Level.Get();
	if (!L || !Camera || HeldItem[Hand].IsValid() || HeldBottle[Hand] || GrabbedEnemy[Hand].IsValid() || GrabHand == Hand || bWheelHeld[Hand])
	{
		return false;
	}
	// a sprite actor glows round its 3D model when it shows one, else round its sprite; a thrown bottle is its mesh
	auto FromActor = [&Out](AActor* A) -> bool
	{
		if (AOutlawsSpriteActor* S = Cast<AOutlawsSpriteActor>(A))
		{
			UProceduralMeshComponent* M = Cast<UProceduralMeshComponent>(S->Model);
			if (M && M->IsVisible()) Out.Model = M;
			else Out.Sprite = S;
			return true;
		}
		if (UProceduralMeshComponent* M = A ? Cast<UProceduralMeshComponent>(A->GetRootComponent()) : nullptr)
		{
			Out.Model = M;
			return true;
		}
		return false;
	};
	auto FromEnemy = [&Out](const AOutlawsEnemy* E) -> bool
	{
		Out.Sprite = E ? E->GetSprite() : nullptr;
		return Out.Sprite.IsValid();
	};
	const FVector Eye = Camera->GetComponentLocation();
	const FVector At = GripOf(Hand)->GetComponentLocation();
	// the grip's own order (UpdateHands), the same for any hand: an item at the fist, a bottle or prop within reach, a bad
	// guy touched, the pointed reach, then an item pulled from afar
	if (AActor* Item = L->FindPickupNear(Eye, At, 16.f)) return FromActor(Item);      // (any hand: user 2026-10-01)
	{
		double BestD = BottleReach;
		int32 Pick = -1;
		for (int32 i = 0; i < Thrown.Num(); ++i)
		{
			const FThrown& T = Thrown[i];
			const AActor* A = T.Actor.Get();
			const UPrimitiveComponent* M = A ? Cast<UPrimitiveComponent>(A->GetRootComponent()) : nullptr;
			if ((T.Type != 3 && T.Type != 4) || !T.bResting || !M) continue;
			const double D = FVector::Dist(M->Bounds.GetBox().GetClosestPointTo(At), At);
			if (D < BestD)
			{
				BestD = D;
				Pick = i;
			}
		}
		if (Pick >= 0) return FromActor(Thrown[Pick].Actor.Get());
		if (AOutlawsSpriteActor* P = L->FindBottleNear(At, BottleReach)) return FromActor(P);
	}
	double Score = 0.0;
	if (const AOutlawsEnemy* E = FindGrabEnemy(Hand, false, Score)) return FromEnemy(E);
	if (!(Equipped[Hand] >= 0 && GWeapons[Equipped[Hand]].Throw == 1))
	{
		AOutlawsEnemy* E = nullptr;
		int32 PickThrown = -1;
		AOutlawsSpriteActor* PickProp = nullptr;
		FindReachGrab(Hand, E, PickThrown, PickProp);
		if (PickProp) return FromActor(PickProp);
		if (Thrown.IsValidIndex(PickThrown)) return FromActor(Thrown[PickThrown].Actor.Get());
		if (E) return FromEnemy(E);
	}
	if (AActor* Item = L->FindPickupAlong(Eye, At, PullRange, PullAngle)) return FromActor(Item);
	return false;
}

void AOutlawsPlayer::UpdateGrabGlow(float Dt)
{
	OL_PERF_SCOPE("Player: grab glow");
	GlowTime += Dt;
	GlowLook -= Dt;
	// (not while both hands are on a long gun: user 2026-09-30)
	// (REQ-HALOOPT: the Graphics page's Grab Glow Off draws none; the grabs themselves never read this)
	const bool bOn = bVR && !bMenuOpen && !bDeadView && !bSupport[0] && !bSupport[1] && OutlawsDeviceProfile::GrabGlow();
	if (GlowLook <= 0.f || !bOn)
	{
		GlowLook = 0.05f;
		for (int32 H = 0; H < 2; ++H)
		{
			if (!bOn || !PreviewGrab(H, GlowPick[H])) GlowPick[H] = FGrabPick();
		}
	}
	// ledges, doors and switches in reach (user 2026-10-01: if it glows you can grab it), refreshed with the look
	if (GlowLook == 0.05f || !bOn)
	{
		for (int32 H = 0; H < 2; ++H)
		{
			// (not a hand already on a ledge, on the wheel, on a long gun's forend or the shovel's shaft; one with nothing else to glow)
			const bool bFree = bOn && GrabHand != H && !bWheelHeld[H] && !bSupport[H] && !bToolSupport[H] && !GlowPick[H].Model.IsValid() && !GlowPick[H].Sprite.IsValid();
			UpdateSurfaceGlow(H, bFree);
		}
	}
	TSet<UPrimitiveComponent*> Shown;
	for (int32 H = 0; H < 2; ++H)
	{
		const FGrabPick& P = GlowPick[H];
		UProceduralMeshComponent* Model = P.Model.Get();
		AOutlawsSpriteActor* Spr = Model ? nullptr : P.Sprite.Get();
		UPrimitiveComponent* Target = Model ? static_cast<UPrimitiveComponent*>(Model) : Spr ? static_cast<UPrimitiveComponent*>(Spr->Quad.Get()) : nullptr;
		if (!Target || Shown.Contains(Target)) continue;
		UPrimitiveComponent* Halo = GlowHalos.FindRef(Target).Get();
		if (!Halo)
		{
			if (Model)
			{
				// a white shell a little out from the thing (outside the headset's ink line), at the world's depth
				if (!GlowShellMat)
				{
					UMaterialInstanceDynamic* W = HeldBase() ? UMaterialInstanceDynamic::Create(HeldBase(), this) : nullptr;
					TArray<FColor> Px;
					Px.Init(FColor(255, 255, 250), 4);
					if (W)
					{
						if (UTexture2D* Tx = OutlawsTextures::MakeTextureBGRA(2, 2, Px, false, TF_Nearest, TA_Clamp)) W->SetTextureParameterValue(TEXT("Tex"), Tx);
						W->SetScalarParameterValue(TEXT("NearPull"), 0.f);
					}
					GlowShellMat = W;
				}
				Halo = OutlawsInk::MakeHalo(Model, 0.9f, GlowShellMat);
			}
			else if (UStaticMeshComponent* Q = Spr->Quad; Q && Q->GetStaticMesh())
			{
				// the sprite's card again, grown about its middle and a little behind it (SM_SpriteQuad faces +X, the
				// actor's +X toward the eye): the sprite hides all of it but a rim
				UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_GrabHalo.M_OLR_GrabHalo"));
				if (Base)
				{
					UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(Spr, NAME_None, RF_Transient);
					C->SetStaticMesh(Q->GetStaticMesh());
					C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
					C->SetCastShadow(false);
					C->SetupAttachment(Q);
					C->RegisterComponent();
					const FBoxSphereBounds B = Q->GetStaticMesh()->GetBounds();
					constexpr double K = 1.07;         // (1.14 was too thick: user 2026-09-30; the material blurs the edge)
					FVector Loc = B.Origin * (1.0 - K);
					Loc.X = -1.5 / FMath::Max(0.01, FMath::Abs(Q->GetComponentScale().X));
					C->SetRelativeLocation(Loc);
					C->SetRelativeScale3D(FVector(1.0, K, K));
					UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, C);
					C->SetMaterial(0, Mid);
					GlowHaloMids.Add(Mid);
					Halo = C;
				}
			}
			if (!Halo) continue;
			GlowHalos.Add(Target, Halo);
		}
		if (!Halo->IsVisible()) Halo->SetVisibility(true);
		if (Spr)
		{
			if (UMaterialInstanceDynamic* Mid = Cast<UMaterialInstanceDynamic>(Halo->GetMaterial(0)))
			{
				Spr->CopyLookTo(Mid);                                  // the frame the sprite shows now
				Mid->SetScalarParameterValue(TEXT("Opacity"), 0.55f + 0.15f * FMath::Sin(GlowTime * 6.f));   // a slow breath
			}
		}
		Shown.Add(Target);
	}
	// the gatling's crank, its free hand near it (user 2026-10-01): the white shell at the held gear's depth
	if (bOn && GatCrank && GFrameCounter - CrankGlowFrame <= 1)
	{
		UPrimitiveComponent* Target = GatCrank;
		UPrimitiveComponent* Halo = GlowHalos.FindRef(Target).Get();
		if (!Halo)
		{
			if (!GlowShellHeldMat)
			{
				UMaterialInstanceDynamic* Wm = HeldBase() ? UMaterialInstanceDynamic::Create(HeldBase(), this) : nullptr;
				TArray<FColor> Px;
				Px.Init(FColor(255, 255, 250), 4);
				if (Wm)
				{
					if (UTexture2D* Tx = OutlawsTextures::MakeTextureBGRA(2, 2, Px, false, TF_Nearest, TA_Clamp)) Wm->SetTextureParameterValue(TEXT("Tex"), Tx);
				}
				GlowShellHeldMat = Wm;
			}
			Halo = OutlawsInk::MakeHalo(GatCrank, 0.6f, GlowShellHeldMat);
			if (Halo) GlowHalos.Add(Target, Halo);
		}
		if (Halo)
		{
			if (!Halo->IsVisible()) Halo->SetVisibility(true);
			Shown.Add(Target);
		}
	}
	for (auto It = GlowHalos.CreateIterator(); It; ++It)
	{
		UPrimitiveComponent* T = It.Key().Get();
		UPrimitiveComponent* Halo = It.Value().Get();
		if (!T || !Halo)
		{
			if (Halo) Halo->DestroyComponent();
			It.RemoveCurrent();
			continue;
		}
		if (!Shown.Contains(T) && Halo->IsVisible()) Halo->SetVisibility(false);
	}
}

void AOutlawsPlayer::UpdateSurfaceGlow(int32 Hand, bool bOn)
{
	UProceduralMeshComponent* G = SurfaceGlow[Hand];
	AOutlawsLevelActor* L = Level.Get();
	TArray<FVector> Pos;
	auto Strip = [&Pos](const FVector& A, const FVector& B, const FVector& Across, double Half)
	{
		Pos.Add(A - Across * Half); Pos.Add(B - Across * Half); Pos.Add(B + Across * Half);
		Pos.Add(A - Across * Half); Pos.Add(B + Across * Half); Pos.Add(A + Across * Half);
	};
	if (bOn && L)
	{
		const FVector Eye = Camera->GetComponentLocation();
		const FVector HandP = GripOf(Hand)->GetComponentLocation();
		FVector Gp, In, TopAt, At, N;
		if (FindLedge(Hand, Gp, In, TopAt, false))
		{
			// a band along the lip, on the top just past the face: as long as an arm's span of hand-holds (40 cm)
			const FVector Side = FVector::CrossProduct(FVector::UpVector, In).GetSafeNormal();
			const FVector C(Gp.X, Gp.Y, TopAt.Z + 0.4);
			const FVector Mid = C + In * 4.0;
			Strip(Mid - Side * 20.0, Mid + Side * 20.0, In, 1.4);
		}
		else if (L->HandUseSpot(Eye, HandP, At, N))
		{
			// a thin frame where the hand would push, a hair off the face
			FVector Rt = FVector::CrossProduct(FVector::UpVector, N);
			if (!Rt.Normalize()) Rt = FVector::RightVector;
			const FVector Up = FVector::CrossProduct(N, Rt).GetSafeNormal();
			const FVector C = At + N * 0.6;
			const double W = 12.0, Hh = 18.0, T = 0.9;
			Strip(C - Rt * W - Up * Hh, C - Rt * W + Up * Hh, Rt, T);
			Strip(C + Rt * W - Up * Hh, C + Rt * W + Up * Hh, Rt, T);
			Strip(C - Rt * W + Up * Hh, C + Rt * W + Up * Hh, Up, T);
			Strip(C - Rt * W - Up * Hh, C + Rt * W - Up * Hh, Up, T);
		}
	}
	const int32 Kind = Pos.Num() == 0 ? 0 : Pos.Num() == 6 ? 1 : 2;
	if (Kind != SurfaceGlowKind[Hand])
	{
		SurfaceGlowKind[Hand] = Kind;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT %s hand glow: %s"), Hand == 0 ? TEXT("left") : TEXT("right"), Kind == 1 ? TEXT("a ledge") : Kind == 2 ? TEXT("a door or switch") : TEXT("none"));
	}
	if (Pos.Num() == 0)
	{
		if (G && G->IsVisible()) G->SetVisibility(false);
		return;
	}
	if (!G)
	{
		if (!GlowShellMat)
		{
			UMaterialInstanceDynamic* Wm = HeldBase() ? UMaterialInstanceDynamic::Create(HeldBase(), this) : nullptr;
			TArray<FColor> Px;
			Px.Init(FColor(255, 255, 250), 4);
			if (Wm)
			{
				if (UTexture2D* Tx = OutlawsTextures::MakeTextureBGRA(2, 2, Px, false, TF_Nearest, TA_Clamp)) Wm->SetTextureParameterValue(TEXT("Tex"), Tx);
				Wm->SetScalarParameterValue(TEXT("NearPull"), 0.f);
			}
			GlowShellMat = Wm;
		}
		G = NewObject<UProceduralMeshComponent>(this);
		G->bUseAsyncCooking = false;
		G->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		G->SetCastShadow(false);
		G->SetUsingAbsoluteLocation(true);
		G->SetUsingAbsoluteRotation(true);
		G->SetUsingAbsoluteScale(true);
		G->RegisterComponent();
		G->SetWorldTransform(FTransform::Identity);
		SurfaceGlow[Hand] = G;
	}
	// both faces of each strip; the vertex count only changes between a band and a frame
	TArray<int32> Tri;
	for (int32 i = 0; i < Pos.Num(); i += 3) { Tri.Append({ i, i + 1, i + 2, i, i + 2, i + 1 }); }
	TArray<FVector> Nrm;
	Nrm.Init(FVector::UpVector, Pos.Num());
	TArray<FVector2D> UV;
	UV.Init(FVector2D(0.5, 0.5), Pos.Num());
	TArray<FLinearColor> Col;
	Col.Init(FLinearColor::White, Pos.Num());
	const FProcMeshSection* S = G->GetProcMeshSection(0);
	if (S && S->ProcVertexBuffer.Num() == Pos.Num()) G->UpdateMeshSection_LinearColor(0, Pos, Nrm, UV, Col, TArray<FProcMeshTangent>());
	else G->CreateMeshSection_LinearColor(0, Pos, Tri, Nrm, UV, Col, TArray<FProcMeshTangent>(), false);
	if (GlowShellMat && G->GetMaterial(0) != GlowShellMat) G->SetMaterial(0, GlowShellMat);
	if (!G->IsVisible()) G->SetVisibility(true);
}

bool AOutlawsPlayer::BakedGrip(const FString& What, FVector& Out)
{
	// BUG-GRIPSAVE: the user's own grip editor placements, right hand (cm in the hand's frame: X along the pointing hand, Y
	// right, Z up), pulled from the Quest 2026-10-04 (grips_merged.json, newest wins)
	static const TMap<FString, FVector> Baked = {
		{ TEXT("weapon:Revolver"), FVector(1.88, 0.56, 1.94) },
		{ TEXT("weapon:Rifle"), FVector(0.10, 1.08, 5.78) },
		{ TEXT("weapon:Knife"), FVector(1.38, 1.25, -2.51) },
		{ TEXT("weapon:Dynamite"), FVector(1.68, 1.60, 0.66) },
		{ TEXT("weapon:Shotgun"), FVector(-2.57, 1.51, 6.75) },
		{ TEXT("weapon:Double Barrel"), FVector(0.81, 1.96, 6.64) },
		{ TEXT("weapon:Sawed-Off"), FVector(1.51, 1.44, 5.49) },
		{ TEXT("tool:iink"), FVector(1.83, 0.77, 2.33) } };
	const FVector* V = Baked.Find(What);
	if (!V) return false;
	Out = *V;
	return true;
}

FVector AOutlawsPlayer::DefaultGunLoc(int32 Weapon, bool bLeft)
{
	if (Weapon < 0 || Weapon >= int32(UE_ARRAY_COUNT(GWeapons))) return FVector::ZeroVector;
	const FWeaponDef& W = GWeapons[Weapon];
	FVector At = W.Offset;
	BakedGrip(FString(TEXT("weapon:")) + W.Label, At);
	return FVector(At.X, bLeft ? -At.Y : At.Y, At.Z);
}

FString AOutlawsPlayer::WeaponLabel(int32 Weapon)
{
	return Weapon >= 0 && Weapon < int32(UE_ARRAY_COUNT(GWeapons)) ? FString(GWeapons[Weapon].Label) : FString();
}

FString AOutlawsPlayer::GripSavePath()
{
	return FPaths::ProjectSavedDir() / TEXT("GripEditor") / TEXT("grips.json");
}

void AOutlawsPlayer::LoadGrips()
{
	GripAdjust.Reset();
	GripPlaced.Reset();
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *GripSavePath())) return;
	TSharedPtr<FJsonObject> J;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), J) || !J.IsValid()) return;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Kv : J->Values)
	{
		const TSharedPtr<FJsonObject>* E = nullptr;
		const TSharedPtr<FJsonObject>* Now = nullptr;
		if (!Kv.Value.IsValid() || !Kv.Value->TryGetObject(E) || !(*E)->TryGetObjectField(TEXT("now"), Now)) continue;
		const TArray<TSharedPtr<FJsonValue>>* Loc = nullptr;
		const TArray<TSharedPtr<FJsonValue>>* Rot = nullptr;
		if (!(*Now)->TryGetArrayField(TEXT("loc"), Loc) || Loc->Num() != 3) continue;
		FRotator R = FRotator::ZeroRotator;
		if ((*Now)->TryGetArrayField(TEXT("rot"), Rot) && Rot->Num() == 3) R = FRotator((*Rot)[0]->AsNumber(), (*Rot)[1]->AsNumber(), (*Rot)[2]->AsNumber());
		GripAdjust.Add(Kv.Key, FTransform(R, FVector((*Loc)[0]->AsNumber(), (*Loc)[1]->AsNumber(), (*Loc)[2]->AsNumber())));
		bool bPlaced = true;
		(*E)->TryGetBoolField(TEXT("placed"), bPlaced);
		if (bPlaced) GripPlaced.Add(Kv.Key);
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT grip editor: %d placements loaded from %s (%d placed by hand)"), GripAdjust.Num(), *GripSavePath(), GripPlaced.Num());
}

void AOutlawsPlayer::SaveGrips() const
{
	// each placement (cm, degrees, in its holding hand's frame), what it was before, and whether a hand put it there (a
	// mirrored left is not)
	auto Xf = [](const FTransform& T)
	{
		const FVector L = T.GetLocation();
		const FRotator Ro = T.Rotator();
		return FString::Printf(TEXT("{ \"loc\": [%.2f, %.2f, %.2f], \"rot\": [%.2f, %.2f, %.2f] }"), L.X, L.Y, L.Z, Ro.Pitch, Ro.Yaw, Ro.Roll);
	};
	FString Out = TEXT("{\n");
	int32 N = 0;
	for (const TPair<FString, FTransform>& Kv : GripAdjust)
	{
		const FTransform* D = GripDefault.Find(Kv.Key);
		Out += FString::Printf(TEXT("%s  \"%s\": { \"now\": %s, \"was\": %s, \"placed\": %s }"), N++ ? TEXT(",\n") : TEXT(""), *Kv.Key, *Xf(Kv.Value), D ? *Xf(*D) : TEXT("null"),
			GripPlaced.Contains(Kv.Key) ? TEXT("true") : TEXT("false"));
	}
	Out += TEXT("\n}\n");
	const bool bSaved = FFileHelper::SaveStringToFile(Out, *GripSavePath());
	UE_LOG(LogOutlaws, Log, TEXT("OLRT grip editor: %d placements %s %s"), GripAdjust.Num(), bSaved ? TEXT("written to") : TEXT("NOT written to"), *GripSavePath());
}

void AOutlawsPlayer::ResetGrips()
{
	IFileManager::Get().Delete(*GripSavePath(), false, true, true);
	GripAdjust.Reset();
	GripPlaced.Reset();
	// the guns in hand back to their baked places now (a tool takes its rest when next held)
	for (int32 H = 0; H < 2; ++H)
	{
		if (Equipped[H] < 0 || !WeaponMesh[H]) continue;
		GunBaseLoc[H] = DefaultGunLoc(Equipped[H], H == 0);
		WeaponMesh[H]->SetRelativeLocation(GunBaseLoc[H]);
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT grip editor: reset, %s deleted, the baked grips back"), *GripSavePath());
}

USceneComponent* AOutlawsPlayer::GripEditTarget(int32 Hand, FString& OutKey) const
{
	const TCHAR* Side = Hand == 0 ? TEXT("L") : TEXT("R");
	if (Equipped[Hand] >= 0 && WeaponMesh[Hand] && WeaponMesh[Hand]->IsVisible())
	{
		OutKey = FString::Printf(TEXT("weapon:%s@%s"), GWeapons[Equipped[Hand]].Label, Side);
		return WeaponMesh[Hand];
	}
	if (!HeldTool[Hand].IsEmpty() && ToolRoot[Hand] && ToolRoot[Hand]->IsVisible())
	{
		OutKey = FString::Printf(TEXT("tool:%s@%s"), *HeldTool[Hand], Side);
		return ToolRoot[Hand];
	}
	return nullptr;
}

void AOutlawsPlayer::LitDynamite(TArray<FVector>& Out) const
{
	for (int32 H = 0; H < 2; ++H)
	{
		if (bFuseLit[H] && Equipped[H] >= 0 && GWeapons[Equipped[H]].Throw == 1 && WeaponMesh[H]) Out.Add(WeaponMesh[H]->GetComponentLocation());
	}
	for (const FThrown& T : Thrown)
	{
		if (T.Type == 1 && T.Fuse >= 0.f && !T.bDud) Out.Add(T.Pos);
	}
}

bool AOutlawsPlayer::TryGrabEnemy(int32 Hand, AOutlawsEnemy* Pick)
{
	// a bad guy by the chest (user 2026-09-29): the fist near him, or (TryReachGrab) pointed near him; he snaps to it
	AOutlawsLevelActor* L = Level.Get();
	if (!L || GrabbedEnemy[Hand].IsValid()) return false;     // (one in each hand is fine: user 2026-09-29)
	double Score = 0.0;
	AOutlawsEnemy* Best = Pick ? Pick : FindGrabEnemy(Hand, false, Score);
	if (!Best) return false;
	// the first taken remembers what both hands held; this hand's weapon goes to the other hand if that one is quite
	// free, else it is holstered (a tool is set aside as for a bottle); all given back when the last is let go
	if (!bGrabSession)
	{
		bGrabSession = true;
		SessionWeapon[0] = Equipped[0];
		SessionWeapon[1] = Equipped[1];
	}
	{
		const int32 O = 1 - Hand;
		if (!HeldTool[Hand].IsEmpty())
		{
			StowForBottle(Hand);
		}
		else if (Equipped[Hand] >= 0)
		{
			const bool bOtherFree = Equipped[O] < 0 && HeldTool[O].IsEmpty() && !HeldBottle[O] && !HeldItem[O].IsValid()
				&& !GrabbedEnemy[O].IsValid() && GrabHand != O;
			if (bOtherFree) SwapWeaponHands(Hand, O);
			else Equip(Hand, -1);
		}
	}
	{
		// how far the grab reached (user 2026-09-30: to settle its range): fist to the nearest of his body
		const double Half = Best->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		const FVector Mid = Best->GetActorLocation();
		const FVector Fist = GripOf(Hand)->GetComponentLocation();
		const double Reach = FVector::Dist(Fist, FMath::ClosestPointOnSegment(Fist, Mid - FVector(0.0, 0.0, Half * 0.85), Mid + FVector(0.0, 0.0, Half)));
		UE_LOG(LogOutlaws, Log, TEXT("OLRT grab %s: %.0f cm from the %s fist (%s)"), *Best->GetName(), Reach, Hand == 0 ? TEXT("left") : TEXT("right"),
			Pick ? TEXT("pointed") : TEXT("touched"));
	}
	GrabbedEnemy[Hand] = Best;
	PushPeak[Hand] = 0.f;                // (the reach's own pull toward the fist is no push)
	GrabOffset[Hand] = Best->GetActorLocation() - Best->GrabChest();    // his chest comes to the fist (user 2026-09-29)
	GrabFrom[Hand] = Best->GetActorLocation();
	GrabGlide[Hand] = 0.f;
	PrevCarry[Hand][0] = PrevCarry[Hand][1] = FVector::ZeroVector;
	if (const AOutlawsSpriteActor* Spr = Best->GetSprite())
	{
		const FVector To = Camera->GetComponentLocation() - Spr->GetActorLocation();
		const float ToYaw = FMath::RadiansToDegrees(FMath::Atan2(To.Y, To.X));
		bGrabBack[Hand] = FMath::Abs(FRotator::NormalizeAxis(ToYaw - Spr->GetFacingYaw())) > 90.f;
	}
	bLaunchArmed[Hand] = false;
	Best->BeginHeld(this);
	if (UOutlawsCoop::IsActive(this)) UOutlawsCoop::NoteEnemyGrab(this, Best, Hand);      // (co-op: the other machine holds its copy)
	GetCapsuleComponent()->IgnoreActorWhenMoving(Best, true);
	Pulse(Hand, 0.6f);
	return true;
}

void AOutlawsPlayer::EndGrabSession()
{
	// the last bad guy let go: each hand has back what it held before the first was taken (a hand busy with a bottle, an
	// item or a tool keeps that)
	if (!bGrabSession) return;
	bGrabSession = false;
	if (bDeadView) return;
	const int32 T[2] = { SessionWeapon[0], SessionWeapon[1] };
	if (Equipped[0] == T[0] && Equipped[1] == T[1]) return;
	if (Equipped[0] == T[1] && Equipped[1] == T[0] && T[0] != T[1])
	{
		SwapWeaponHands(0, 1);
		return;
	}
	auto Busy = [this](int32 h) { return HeldBottle[h] || HeldItem[h].IsValid() || !HeldTool[h].IsEmpty() || GrabHand == h; };
	for (int32 h = 0; h < 2; ++h) if (!Busy(h) && Equipped[h] != T[h] && Equipped[h] >= 0) Equip(h, -1);
	for (int32 h = 0; h < 2; ++h) if (!Busy(h) && T[h] >= 0 && Equipped[h] != T[h]) Equip(h, T[h]);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT the hands have their weapons back: left %d, right %d"), Equipped[0], Equipped[1]);
}

bool AOutlawsPlayer::TryTakeEnemyFromOther(int32 Hand)
{
	// the other fist holds a bad guy and this one squeezes by it: he changes hands (user 2026-09-29), and whatever this
	// hand held goes to the hand that let go of him
	const int32 O = 1 - Hand;
	AOutlawsEnemy* E = GrabbedEnemy[O].Get();
	if (!E || GrabbedEnemy[Hand].IsValid() || HeldBottle[Hand] || HeldItem[Hand].IsValid()) return false;
	if (FVector::Dist(GripOf(Hand)->GetComponentLocation(), GripOf(O)->GetComponentLocation()) > 22.0) return false;
	StowWeapon[O] = -1;
	StowTool[O].Reset();
	StowMovedTo[O] = -1;
	StowWeapon[Hand] = -1;
	StowTool[Hand].Reset();
	StowMovedTo[Hand] = -1;
	if (Equipped[Hand] >= 0) SwapWeaponHands(Hand, O);            // (this hand's gun to the hand he leaves)
	GrabbedEnemy[Hand] = E;
	GrabbedEnemy[O] = nullptr;
	GrabOffset[Hand] = E->GetActorLocation() - E->GrabChest();
	GrabFrom[Hand] = E->GetActorLocation();
	GrabGlide[Hand] = 0.f;
	bGrabBack[Hand] = bGrabBack[O];
	bLaunchArmed[Hand] = false;
	if (bGrabAiming[O]) { bGrabAiming[O] = false; ShowThrowArc(O, false); }
	Pulse(Hand, 0.4f);
	Pulse(O, 0.3f);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT %s changes hands"), *E->GetName());
	return true;
}

FVector AOutlawsPlayer::HeldPushVel(int32 Hand) const
{
	if (GetVelocity().Size2D() < 150.f) return HandWorldVel[Hand];
	const FVector Rel = HandVelocity[Hand];
	return Rel.Size() > 600.f ? FVector::ZeroVector : Rel;      // (a snap turn swings the hand round the head)
}

void AOutlawsPlayer::ThrowEnemyAimed(int32 H)
{
	AOutlawsEnemy* E = GrabbedEnemy[H].Get();
	if (!E) return;
	const FVector Vel = ThrowVelocity(H);         // (his fall the one throw's: LetGoEnemy)
	if (AOutlawsLevelActor* LL = Level.Get()) LL->PlaySoundAt(TEXT("throw"), E->GetActorLocation());
	UE_LOG(LogOutlaws, Log, TEXT("OLRT threw %s along the arc at %.0f cm/s"), *E->GetName(), Vel.Size());
	LetGoEnemy(H, true, &Vel);
	Pulse(H, 0.6f);
}

void AOutlawsPlayer::LetGoEnemy(int32 Hand, bool bThrow, const FVector* Launch)
{
	AOutlawsEnemy* E = GrabbedEnemy[Hand].Get();
	GrabbedEnemy[Hand] = nullptr;
	// (BUG-ARCSTUCK) a throw lined up ends with him, however he left the hand: the next one taken is not thrown on its own
	bGrabAiming[Hand] = false;
	ShowThrowArc(Hand, false);
	if (E && bHeldFaded[Hand])
	{
		bHeldFaded[Hand] = false;
		if (AOutlawsSpriteActor* Spr = E->GetSprite()) Spr->SetFade(1.f);
	}
	if (E)
	{
		// on the move he keeps the player's own pace too, so a push or a drop running is not run into (user 2026-10-01)
		const FVector Run(GetVelocity().X, GetVelocity().Y, 0.0);
		const bool bOnTheRun = Run.Size() >= 150.f;
		if (E->bHeldByPlayer)
		{
			// thrown, the one throw (user 2026-10-02): along the arc or the palm's (Launch) the running is in it already, a
			// toss is the hand's swing (TossVelocity, a man's share of it); and he falls as the player does there, not at his
			// own 1 g (a body dropped too)
			const FVector Fling = Launch ? *Launch : bThrow ? TossVelocity(Hand, E->IsCritter() ? 1.f : ManThrowScale) : (bOnTheRun ? Run : FVector::ZeroVector);
			const float Gz = FMath::Max(1.f, FMath::Abs(E->GetCharacterMovement()->GetGravityZ()));
			E->FlightGravity = ThrowGravity(E->GetActorLocation()) / Gz;
			// (BUG-GRABFALL) let go while the far grab still pulls him down off his roof: his fall counts from the fist's
			// height, not from where the pull had got to
			E->bPullCap = GrabGlide[Hand] < 1.f;
			if (E->bPullCap) E->PullCapZ = float((E->GetActorLocation() + (GripOf(Hand)->GetComponentLocation() - E->GrabChest())).Z);
			if (UOutlawsCoop::IsActive(this)) UOutlawsCoop::NoteEnemyRelease(this, E, Fling);     // (co-op: thrown there the same way)
			E->EndHeld(Fling);
		}
		// walked through till he lands, a second at least, and till the two are clear of each other (user 2026-10-01:
		// pushed while running he stuck in the player)
		const int32 Fi = FlungEnemies.AddUnique(E);
		if (FlungUntil.Num() < FlungEnemies.Num()) FlungUntil.SetNumZeroed(FlungEnemies.Num());
		FlungUntil[Fi] = GetWorld()->GetTimeSeconds() + 1.0;
		GetCapsuleComponent()->IgnoreActorWhenMoving(E, true);
		E->GetCapsuleComponent()->IgnoreActorWhenMoving(this, true);
	}
	RestoreStowed(Hand);                  // (a tool set aside)
	if (!GrabbedEnemy[0].IsValid() && !GrabbedEnemy[1].IsValid()) EndGrabSession();
}

void AOutlawsPlayer::UpdateEnemyGrab(float Dt)
{
	OL_PERF_SCOPE("Player: enemy grab");
	AOutlawsLevelActor* LG = Level.Get();
	SmashCool = FMath::Max(0.f, SmashCool - Dt);
	for (int32 h = 0; h < 2; ++h) ShoveCool[h] = FMath::Max(0.f, ShoveCool[h] - Dt);
	// (HM1, the planner's addition) an outlaw knocked out, not killed, groans now and then while held, so the player knows
	// which they carry: his own hurt cry, lower and softer (the game has no groan of its own)
	GroanWait = FMath::Max(0.f, GroanWait - Dt);
	if (LG && GroanWait <= 0.f)
	{
		for (int32 h = 0; h < 2; ++h)
		{
			AOutlawsEnemy* E = GrabbedEnemy[h].Get();
			if (!E || !E->IsDead() || !E->bTakenAlive) continue;
			if (UAudioComponent* A = LG->PlaySoundAt(E->Def.HitSound, E->GrabChest()))
			{
				A->SetPitchMultiplier(0.78f);
				A->SetVolumeMultiplier(A->VolumeMultiplier * 0.55f);
			}
			GroanWait = FMath::FRandRange(2.5f, 4.5f);
			break;
		}
	}
	if (LG)
	{
		// two held bad guys smashed together: both hurt, by how hard (user 2026-09-29).  REQ-HELDCLASH: on their first contact
		// as drawn (UpdateHeldClash, once the hands have placed them, below); -OLOldSmash: when the chests come within 55 cm
		if (OldSmash())
		{
			AOutlawsEnemy* A = GrabbedEnemy[0].Get();
			AOutlawsEnemy* B = GrabbedEnemy[1].Get();
			const FVector Rel = HandVelocity[0] - HandVelocity[1];
			if (A && B && A->bHeldByPlayer && B->bHeldByPlayer && SmashCool <= 0.f && Rel.Size() > OutlawsFeel::HitSpeedHard
				&& FVector::Dist(A->GrabChest(), B->GrabChest()) < 55.0)
			{
				SmashHeld(A, B, Rel, TEXT("the chests within 55 cm, -OLOldSmash"));
			}
		}
		// one rammed into another bad guy with force: the struck one hurt and knocked, the held one half as much
		for (int32 h = 0; h < 2; ++h)
		{
			AOutlawsEnemy* E = GrabbedEnemy[h].Get();
			const FVector V = HandVelocity[h];
			// (a held one is a club only with another held in the other hand: user 2026-09-29)
			if (!E || !E->bHeldByPlayer || !GrabbedEnemy[1 - h].IsValid() || ShoveCool[h] > 0.f || V.Size() < OutlawsFeel::HitSpeedHard) continue;
			// the held body's middle, where the hand has him, against the other's body: a body's half-width off it at most, and
			// in sight (user 2026-10-02: the two whole capsules met through walls, from a long way off)
			const FVector Club = E->GrabChest();
			if (!MeleeInSight(this, LG, Camera->GetComponentLocation(), Club)) continue;
			for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : LG->GetEnemies())
			{
				AOutlawsEnemy* O2 = Wk.Get();
				if (!O2 || O2 == E || O2->bHeldByPlayer || O2->IsDead() || O2->IsDormant()) continue;
				if (BodyGap(O2, Club, Club) > 20.0) continue;
				const float Dmg = FMath::Clamp(float(V.Size()) / 400.f, 0.5f, 2.f) * LG->PlayerDamageScale();
				O2->TakeHit(Dmg, O2->GetActorLocation(), V.GetSafeNormal(), E->GetActorLocation(), false, 1.5f);
				E->TakeHit(Dmg * 0.5f, E->GrabChest(), -V.GetSafeNormal(), O2->GetActorLocation());
				LG->PlaySoundAt(TEXT("connect"), E->GrabChest());
				PulseImpact(h, float(V.Size()));
				ShoveCool[h] = 0.5f;
				UE_LOG(LogOutlaws, Log, TEXT("OLRT held %s rammed into %s at %.0f cm/s"), *E->GetName(), *O2->GetName(), V.Size());
				break;
			}
		}
	}
	if (FlungUntil.Num() < FlungEnemies.Num()) FlungUntil.SetNumZeroed(FlungEnemies.Num());
	const double NowT = GetWorld()->GetTimeSeconds();
	for (int32 i = FlungEnemies.Num() - 1; i >= 0; --i)
	{
		AOutlawsEnemy* E = FlungEnemies[i].Get();
		bool bDone = !E;
		if (E && !E->bThrown && !E->bHeldByPlayer)
		{
			// landed: the two collide again a second after the let-go at the soonest, once clear of each other (4 s at
			// the most: the movement pushes them apart then)
			const FVector D = E->GetActorLocation() - GetActorLocation();
			const double Clear = GetCapsuleComponent()->GetScaledCapsuleRadius() + E->GetCapsuleComponent()->GetScaledCapsuleRadius() + 5.0;
			bDone = (NowT >= FlungUntil[i] && FVector2D(D.X, D.Y).Size() > Clear) || NowT >= FlungUntil[i] + 3.0;
		}
		if (bDone)
		{
			if (E)
			{
				GetCapsuleComponent()->IgnoreActorWhenMoving(E, false);
				E->GetCapsuleComponent()->IgnoreActorWhenMoving(this, false);
			}
			FlungEnemies.RemoveAt(i);
			FlungUntil.RemoveAt(i);
		}
	}
	for (int32 H = 0; H < 2; ++H)
	{
		AOutlawsEnemy* E = GrabbedEnemy[H].Get();
		if (!E) continue;
		if (!E->bHeldByPlayer || bDeadView || GrabHand == H)
		{
			// shot dead in the hands (it let go itself), or the hand went to a ledge; a body taken up is held (user 2026-10-01)
			LetGoEnemy(H, false);
			continue;
		}
		const int32 O = 1 - H;
		const float G = Axis(H == 0 ? TEXT("OLVR_LeftGrip") : TEXT("OLVR_RightGrip"));
		// the hand's push toward where its palm faces, the fastest of the last third of a second
		{
			const FVector Palm = PalmDir(H);
			const float Push = float(FVector::DotProduct(HeldPushVel(H), Palm));
			const double Now = FPlatformTime::Seconds();
			if (Push > PushPeak[H] || Now - PushAt[H] > 0.35)
			{
				PushPeak[H] = FMath::Max(0.f, Push);
				PushAt[H] = Now;
				PushDir[H] = Palm;
			}
		}
		// let go of the grip while a throw is lined up (the trigger pulled, the arc out): thrown along the arc all the same,
		// the grip let slip with the trigger no drop (user 2026-09-29)
		const bool bThrowNow = G < 0.35f && bGrabAiming[H];
		if (G < 0.35f && !bThrowNow)
		{
			// pushed toward where the palm faces and let go: launched that way (user 2026-09-30), the second throw after the
			// trigger's arc, as strong as it (user 2026-09-30; three times the push was 6 to 8 m/s: half a metre): the
			// push a power as the hand's height is the trigger's, a gentle 1.5 m/s about a third, 6.5 m/s the whole; the
			// trigger's speed for it (a man's halved range too), at the palm's pitch, 12 to 45 degrees, under the one
			// throw's fall with the running in it (as ThrowVelocity)
			if (PushPeak[H] > 150.f && FPlatformTime::Seconds() - PushAt[H] <= 0.35)
			{
				FVector Flat = FVector(PushDir[H].X, PushDir[H].Y, 0.0);
				if (Flat.SizeSquared() < 0.04) Flat = FVector(HeldPushVel(H).X, HeldPushVel(H).Y, 0.0);     // (the palm up or down: the push's own way, not the look)
				if (Flat.SizeSquared() < 1e-4) Flat = FVector(AimOf(H)->GetForwardVector().X, AimOf(H)->GetForwardVector().Y, 0.0);
				Flat = Flat.GetSafeNormal();
				const float PalmPitch = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(float(PushDir[H].Z), -1.f, 1.f)));
				const float Pitch = FMath::Clamp(PalmPitch + 8.f, 12.f, 45.f);
				const float Power = FMath::Lerp(0.3f, 1.f, FMath::Clamp((PushPeak[H] - 150.f) / 500.f, 0.f, 1.f));
				const float Ga = ThrowGravity(E->GetActorLocation());
				const float Speed = FMath::Lerp(500.f, 2600.f, Power) * (E->IsCritter() ? 1.f : ManThrowScale);
				const float Range = FMath::Square(Speed) * FMath::Sin(FMath::DegreesToRadians(2.f * Pitch)) / Ga;   // (on the flat, for the log)
				const float Rad = FMath::DegreesToRadians(Pitch);
				const FVector Dir = Flat * FMath::Cos(Rad) + FVector(0.0, 0.0, FMath::Sin(Rad));
				const FVector Vel = Dir * Speed + BodyCarry();
				if (AOutlawsLevelActor* LL = Level.Get()) LL->PlaySoundAt(TEXT("throw"), E->GetActorLocation());
				UE_LOG(LogOutlaws, Log, TEXT("OLRT palm launch %s: push %.0f cm/s -> %.0f cm at %.0f deg, %.0f cm/s"), *E->GetName(), PushPeak[H], Range, Pitch, Vel.Size());
				PushPeak[H] = 0.f;
				LetGoEnemy(H, true, &Vel);
				Pulse(H, 0.6f);
				continue;
			}
			// let go: pushed away it flies (into someone, or up), else it drops where it is
			LetGoEnemy(H, HeldPushVel(H).Size() > 150.f);
			continue;
		}
		// thrown as the knife is (user 2026-09-29): the holding hand's trigger held shows the arc, let go he flies along
		// it, man, chicken or tarantula alike (his speed matched to his own gravity, so he follows the arc drawn)
		const float Tr = Axis(H == 0 ? TEXT("OLVR_LeftTrigger") : TEXT("OLVR_RightTrigger"));
		if (Tr < 0.3f && !bGrabAiming[H]) bLaunchArmed[H] = true;
		if (Tr > 0.7f && bLaunchArmed[H] && !bMenuOpen && !bWheelOpen)
		{
			bLaunchArmed[H] = false;
			bGrabAiming[H] = true;
		}
		{
			// lining up a throw, he goes half see-through so the arc shows through him (user 2026-09-29)
			const bool bFade = bGrabAiming[H] && Tr > 0.3f;
			if (bFade != bHeldFaded[H])
			{
				bHeldFaded[H] = bFade;
				if (AOutlawsSpriteActor* Spr = E->GetSprite()) Spr->SetFade(bFade ? 0.5f : 1.f);
			}
		}
		if (bGrabAiming[H])
		{
			if (Tr > 0.3f && !bMenuOpen && !bWheelOpen && !bThrowNow)
			{
				ShowThrowArc(H, true);
			}
			else
			{
				bGrabAiming[H] = false;
				ShowThrowArc(H, false);
				ThrowEnemyAimed(H);
				continue;
			}
		}
		// held in the left hand a man is drawn mirrored, the hold on him with it (user 2026-09-29): his quad's width
		// negated (GrabChest reads the sign, so the fist keeps the mirrored point); put back when he lands (EndCarry)
		if (!E->IsCritter())
		{
			if (AOutlawsSpriteActor* Spr = E->GetSprite())
			{
				const FVector Sc = Spr->GetActorScale3D();
				const double WantY = FMath::Abs(Sc.Y) * (H == 0 ? -1.0 : 1.0);
				if (Sc.Y != WantY) Spr->SetActorScale3D(FVector(Sc.X, WantY, Sc.Z));
			}
		}
		// locked to the fist, through walls if need be (user 2026-09-29: they caught on walls); the side taken stays
		// toward the eyes (front or back) however the player turns
		{
			const FVector Want = E->IsCritter() ? GripOf(H)->GetComponentLocation() + GrabOffset[H]
				: E->GetActorLocation() + (GripOf(H)->GetComponentLocation() - E->GrabChest());   // his neck in the fist
			GrabGlide[H] = FMath::Min(1.f, GrabGlide[H] + Dt / 0.2f);          // a fifth of a second, quick then settling
			const bool bSettled = GrabGlide[H] >= 1.f;
			// (REQ-HELDCLASH) pushed off the other held man: his offset from the hand kept
			E->SetActorLocation((bSettled ? Want : FMath::Lerp(GrabFrom[H], Want, FMath::InterpEaseOut(0.f, 1.f, GrabGlide[H], 3.f))) + HeldClashOffset(H, E), false);
			// pushed through a window, it gives (user 2026-09-29): his chest and his middle sweep the panes
			const FVector Now[2] = { E->GrabChest(), E->GetActorLocation() };
			if (bSettled && LG)
			{
				for (int32 k = 0; k < 2; ++k)
				{
					if (!PrevCarry[H][k].IsZero() && FVector::DistSquared(PrevCarry[H][k], Now[k]) > 1.0) LG->ShatterAlong(PrevCarry[H][k], Now[k]);
				}
			}
			PrevCarry[H][0] = Now[0];
			PrevCarry[H][1] = Now[1];
		}
		if (E->IsCritter())
		{
			// a chicken by the neck, a tarantula by the back, turning every which way with the hand (user 2026-09-29)
			E->PoseHeldCritter(GripOf(H)->GetComponentTransform(), H == 0, Camera->GetComponentLocation());
		}
		else if (AOutlawsSpriteActor* Spr = E->GetSprite())
		{
			// he faces the eye; pulled in close to the chest the eye is a hand's width off his place, nearly over it, and the
			// angle to it swung with every small move of the hand or head (user 2026-10-02: his rotation flickered and
			// jittered).  Within 45 cm across, the view's own heading takes over (all of it inside 15 cm), and the sprite
			// is told to be seen from straight along that facing (HoldView), so its view frame holds still
			const FVector To = Camera->GetComponentLocation() - Spr->GetActorLocation();
			const double Across = FVector2D(To.X, To.Y).Size();
			const FVector2D Heading = -FVector2D(FRotator(0.0, Camera->GetComponentRotation().Yaw, 0.0).Vector());
			const double Far = FMath::SmoothStep(15.0, 45.0, Across);
			const FVector2D FaceDir = (Across > 1.0 ? FVector2D(To.X, To.Y) / Across : Heading) * Far + Heading * (1.0 - Far);
			const float ToYaw = float(FMath::RadiansToDegrees(FMath::Atan2(FaceDir.Y, FaceDir.X)));
			Spr->OutlawsYaw = FRotator::NormalizeAxis(ToYaw + (bGrabBack[H] ? 180.f : 0.f) - Spr->FacingYawOffset);
			Spr->HoldView(ToYaw);
		}
	}
	// (REQ-HELDCLASH) both placed: they collide where they are drawn, the smash on the first contact
	if (!OldSmash()) UpdateHeldClash(Dt);
}

void AOutlawsPlayer::PassBottle(int32 From, int32 To)
{
	UProceduralMeshComponent* B = HeldBottle[From];
	if (!B || HeldBottle[To])
	{
		return;
	}
	// it leaves from where it is and flies into the other fist, held there as in the first
	B->AttachToComponent(FrameOf(To), FAttachmentTransformRules::KeepWorldTransform);
	BottleZoomFrom[To] = B->GetRelativeTransform();
	BottleZoomTo[To] = HoldPoseFor(To, B, BottleHold[From]);             // (held as this hand holds it: the left mirrors the right)
	BottleHold[To] = BottleHold[From];
	BottleZoom[To] = 0.f;
	BottleZoom[From] = -1.f;
	HeldBottle[To] = B;
	HeldBottle[From] = nullptr;
	BottleSound[To] = BottleSound[From];
	BottleAxis[To] = BottleAxis[From];
	PrevBottlePos[To] = B->Bounds.Origin;
	bBottleReady[From] = bBottleReady[To] = false;
	ShowThrowArc(From, false);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT the bottle changes hands: %s to %s"), From == 0 ? TEXT("left") : TEXT("right"), To == 0 ? TEXT("left") : TEXT("right"));
	if (Equipped[To] >= 0 || StowMovedTo[From] == To)
	{
		// traded for the weapon in the other hand (it comes into this one next): the hands are as the player put them
		StowWeapon[From] = -1;
		StowTool[From].Reset();
		StowMovedTo[From] = -1;
	}
	else
	{
		RestoreStowed(From);
	}
}

void AOutlawsPlayer::BreakHeldBottle(int32 Hand, const FVector& Dir)
{
	UProceduralMeshComponent* B = HeldBottle[Hand];
	HeldBottle[Hand] = nullptr;
	if (!B)
	{
		return;
	}
	if (AOutlawsLevelActor* L = Level.Get())
	{
		L->ShatterGlass(B, Dir, BottleSound[Hand]);
	}
	B->DestroyComponent();
	Pulse(Hand, 0.9f);
	RestoreStowed(Hand);
}

void AOutlawsPlayer::UpdateDualPickup(float Dt)
{
	OL_PERF_SCOPE("Player: dual pickup");
	// picking up a second of a gun (a revolver, a sawed-off: whatever the player can carry two of) with the first in one
	// hand: the other hand draws it at once (user 2026-09-29; asked twice: the first try looked only on the pickup's own
	// frame, when the hand could still be busy with it); tried for two seconds, the hand free of anything but a bare fist
	for (int32 Wi = 0; Wi < NumWeapons; ++Wi)
	{
		if (GWeapons[Wi].Throw > 0 || GWeapons[Wi].bAuto) continue;
		const int32 Now = WeaponsOwned(Wi);
		const int32* Was = PrevOwned.Find(Wi);
		if (Was && *Was < 2 && Now >= 2)
		{
			DualPending.Add(Wi, 2.f);
			UE_LOG(LogOutlaws, Log, TEXT("OLRT second %s picked up (left %d, right %d)"), GWeapons[Wi].Label, Equipped[0], Equipped[1]);
		}
		PrevOwned.Add(Wi, Now);
		float* Left = DualPending.Find(Wi);
		if (!Left) continue;
		*Left -= Dt;
		if (*Left <= 0.f || Now < 2 || bDeadView)
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLRT second %s not drawn (left %d, right %d, grab %d, session %d, items %d %d, bottles %d %d, tools %d %d)"),
				GWeapons[Wi].Label, Equipped[0], Equipped[1], GrabHand, bGrabSession ? 1 : 0, HeldItem[0].IsValid() ? 1 : 0, HeldItem[1].IsValid() ? 1 : 0,
				HeldBottle[0] ? 1 : 0, HeldBottle[1] ? 1 : 0, HeldTool[0].IsEmpty() ? 0 : 1, HeldTool[1].IsEmpty() ? 0 : 1);
			DualPending.Remove(Wi);
			continue;
		}
		if (Equipped[0] == Wi && Equipped[1] == Wi)
		{
			DualPending.Remove(Wi);          // (both drawn already)
			continue;
		}
		for (int32 H = 0; H < 2; ++H)
		{
			const int32 O = 1 - H;
			if (Equipped[H] != Wi) continue;
			const bool bOtherFree = Equipped[O] < 0 && HeldTool[O].IsEmpty() && !HeldBottle[O] && !HeldItem[O].IsValid()
				&& !GrabbedEnemy[O].IsValid() && GrabHand != O;
			if (!bOtherFree) break;
			Equip(O, Wi);
			Pulse(O, 0.4f);
			DualPending.Remove(Wi);
			UE_LOG(LogOutlaws, Log, TEXT("OLRT second %s: drawn in the %s hand"), GWeapons[Wi].Label, O == 0 ? TEXT("left") : TEXT("right"));
			break;
		}
	}
}

void AOutlawsPlayer::UpdateBottles(float Dt)
{
	OL_PERF_SCOPE("Player: bottles");
	AOutlawsLevelActor* L = Level.Get();
	// a new level (or a restart) takes what was held or set down of the last one
	if (L && BottleClear != L->GetClearCount())
	{
		BottleClear = L->GetClearCount();
		for (int32 H = 0; H < 2; ++H)
		{
			if (HeldBottle[H]) HeldBottle[H]->DestroyComponent();
			HeldBottle[H] = nullptr;
		}
		for (int32 i = Thrown.Num() - 1; i >= 0; --i)
		{
			if (Thrown[i].Type != 3 && Thrown[i].Type != 4) continue;
			if (AActor* A = Thrown[i].Actor.Get()) A->Destroy();
			Thrown.RemoveAt(i);
		}
	}
	for (int32 H = 0; H < 2; ++H)
	{
		UProceduralMeshComponent* B = HeldBottle[H];
		if (!B) continue;
		// a weapon or a tool drawn into that hand, a ledge taken, or death: it drops
		if (!L || bDeadView || Equipped[H] >= 0 || !HeldTool[H].IsEmpty() || GrabHand == H)
		{
			ReleaseBottle(H);
			continue;
		}
		// on its way into the fist: a fifth of a second, fast then settling, to the dynamite's hold (upright, its middle
		// 2 cm ahead of and above the fist's)
		if (BottleZoom[H] >= 0.f)
		{
			BottleZoom[H] = FMath::Min(1.f, BottleZoom[H] + Dt / 0.18f);
			FTransform X;
			X.Blend(BottleZoomFrom[H], BottleZoomTo[H], FMath::InterpEaseOut(0.f, 1.f, BottleZoom[H], 3.f));
			B->SetRelativeTransform(X);
			if (BottleZoom[H] >= 1.f) BottleZoom[H] = -1.f;
		}
		SwingCool[H] = FMath::Max(0.f, SwingCool[H] - Dt);
		// the trigger lines up a throw as the knife's does (user 2026-09-29): held, the arc shows and the thing goes half
		// see-through; let go, away it flies along the arc (letting go of the grip still tosses it with the hand)
		if (!bMenuOpen && !bWheelOpen)
		{
			const float Trigger = Axis(H == 0 ? TEXT("OLVR_LeftTrigger") : TEXT("OLVR_RightTrigger"));
			if (Trigger > 0.7f && BottleZoom[H] < 0.f) bBottleReady[H] = true;
			if (bBottleReady[H])
			{
				if (Trigger > 0.3f)
				{
					ShowThrowArc(H, true);
					SetBottleGhost(H, true);
				}
				else
				{
					ReleaseBottle(H, true);
					continue;
				}
			}
		}
		const FVector Now = B->Bounds.Origin;
		const FVector Was = PrevBottlePos[H];
		PrevBottlePos[H] = Now;
		const FVector V = HandVelocity[H];       // the arm's own swing
		if (V.Size() < 250.f || bMenuOpen || bWheelOpen) continue;
		const FVector Dir = V.GetSafeNormal();
		const double Reach = B->Bounds.SphereRadius * 0.5;
		if (SwingCool[H] <= 0.f && UOutlawsCoop::StrikeAtPlayers(L, Now, float(Reach), BottleSmashDamage * FMath::FRandRange(0.9f, 1.1f) * L->PlayerDamageScale(), TEXT("a smash over the head")))
		{
			L->PlaySoundAt(TEXT("connect"), Now);   // (M1's gaps: another player in a match)
			SwingCool[H] = 0.45f;
		}
		// over a head (or into any part of a bad guy): a melee blow, and it bursts on him
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
		{
			AOutlawsEnemy* E = Wk.Get();
			if (!E || E->IsDead() || E->IsDormant()) continue;
			const FVector C = E->GetActorLocation();
			const double Half = E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
			if (FVector::Dist2D(C, Now) > E->GetCapsuleComponent()->GetScaledCapsuleRadius() + Reach || FMath::Abs(Now.Z - C.Z) > Half + Reach) continue;
			// one held in the other hand can be beaten with it too (user 2026-09-29): the swing then counted against that
			// hand's (both hands carried along together is no blow)
			const bool bOtherHolds = E->bHeldByPlayer && GrabbedEnemy[1 - H].Get() == E;
			if ((E->bHeldByPlayer && !bOtherHolds) || SwingCool[H] > 0.f) continue;
			if (bOtherHolds && (V - HandVelocity[1 - H]).Size() < 250.f) continue;
			const float Damage = BottleSmashDamage * FMath::FRandRange(0.9f, 1.1f);
			E->TakeHit(Damage * L->PlayerDamageScale(), Now, Dir, Now - Dir * 30.0);
			L->PlaySoundAt(TEXT("connect"), Now);
			if (!bHeldBreaks[H] && !BottleSound[H].IsEmpty()) L->PlaySoundAt(BottleSound[H], Now);     // (a pot rings as it does shot)
			UE_LOG(LogOutlaws, Log, TEXT("OLRT bottle smashed over %s: %.2f"), *E->GetName(), Damage);
			SwingCool[H] = 0.45f;
			if (bHeldBreaks[H]) BreakHeldBottle(H, Dir);      // (a pot or a pan rings off him and stays in the hand)
			break;
		}
		if (!HeldBottle[H]) continue;
		// swung into another bottle, a lamp, a window: both go
		FVector End = Now + Dir * Reach;
		if (bHeldBreaks[H] && L->ShootAlong(Was, End))
		{
			BreakHeldBottle(H, Dir);
			continue;
		}
		// driven into a wall, a floor or a door: it bursts there
		FHitResult WH;
		FCollisionObjectQueryParams Obj;
		Obj.AddObjectTypesToQuery(ECC_WorldStatic);
		Obj.AddObjectTypesToQuery(ECC_WorldDynamic);
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsBottleSwing), false, this);
		if (bHeldBreaks[H] && GetWorld()->LineTraceSingleByObjectType(WH, Was, Now + Dir * Reach, Obj, Q) && WH.GetActor() == L
			&& FVector::DotProduct(V, FVector(WH.ImpactNormal)) < -180.0)
		{
			BreakHeldBottle(H, FVector(WH.ImpactNormal));
		}
	}
}

void AOutlawsPlayer::UpdateCylinder(int32 Hand, float Dt)
{
	UProceduralMeshComponent* Cyl = CylMesh[Hand];
	UProceduralMeshComponent* Gun = WeaponMesh[Hand];
	if (!Cyl || !Gun || !HasCylinder(Equipped[Hand]))
	{
		ShowRounds(Hand, false);
		return;
	}
	Cyl->SetVisibility(Gun->IsVisible());
	// out while rounds go in (about a fifth of a second either way), turned a chamber per round
	const float Want = bReloadingNow[Hand] || bDebugCylOut || (CylOut[Hand] > 0.f && ReloadTimer[Hand] > 0.f) ? 1.f : 0.f;
	CylOut[Hand] = FMath::FInterpConstantTo(CylOut[Hand], Want, Dt, 5.f);
	CylSpin[Hand] = FMath::FInterpTo(CylSpin[Hand], CylSpinTarget[Hand], Dt, 18.f);
	const FCylinderDef& CylDef = Cylinder();
	const FVector From = (CylDef.Centre - CylDef.Pivot) * FVector(0.0, 1.0, 1.0);
	const FQuat Swing = FQuat::FindBetweenNormals(From.GetSafeNormal(), CylDef.OutDir.GetSafeNormal());
	CylPivot[Hand]->SetRelativeRotation(FQuat::Slerp(FQuat::Identity, Swing, FMath::InterpEaseInOut(0.f, 1.f, CylOut[Hand], 2.f)));
	Cyl->SetRelativeRotation(FRotator(0.0, 0.0, FMath::Fmod(CylSpin[Hand], 360.f)));
	// the rounds: roll s turns chamber k (at 90 + 60k degrees) to 90 + 60k - s, so the top one is k = s / 60; the Mag
	// chambers from the top in firing order are loaded (a shot empties the top one and turns the next up; a round
	// loaded turns the cylinder back one, the empty chamber behind the top coming to the gate)
	if (UseSpriteGun(0) && RoundParts.Num() >= (Hand + 1) * 12)
	{
		const int32 Top = ((FMath::RoundToInt(CylSpinTarget[Hand] / 60.f) % 6) + 6) % 6;
		for (int32 k = 0; k < 6; ++k)
		{
			const bool bLoaded = ((k - Top + 6) % 6) < Mag[Hand] && Cyl->IsVisible();
			for (int32 j = 0; j < 2; ++j)
			{
				if (UStaticMeshComponent* R = RoundParts[Hand * 12 + k * 2 + j]) R->SetVisibility(bLoaded);
			}
		}
	}
}

void AOutlawsPlayer::BuildRounds(int32 Hand)
{
	if (RoundParts.Num() < 24) RoundParts.SetNum(24);
	if (RoundParts[Hand * 12] || !CylMesh[Hand]) return;
	UStaticMesh* Shape = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	UMaterialInterface* Brass = HeldSolidMaterial(FColor(178, 140, 64));
	UMaterialInterface* Lead = HeldSolidMaterial(FColor(128, 124, 118));
	const double Half = GSpriteCylLength * 0.5;
	const double Hole = GSpriteHoleR;
	for (int32 k = 0; k < 6; ++k)
	{
		const double A = FMath::DegreesToRadians(90.0 + 60.0 * k);
		const FVector Ring(0.0, GSpriteChamberR * FMath::Cos(A), GSpriteChamberR * FMath::Sin(A));
		for (int32 j = 0; j < 2; ++j)
		{
			// the case head just proud of the rear face; the bullet's nose just proud of the front
			const bool bHead = j == 0;
			const double Len = bHead ? 0.25 : 0.35;
			const double Rad = bHead ? Hole * 1.05 : Hole * 0.9;
			UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this);
			C->SetStaticMesh(Shape);
			C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			C->SetCastShadow(false);
			C->SetupAttachment(CylMesh[Hand]);
			C->RegisterComponent();
			C->SetRelativeLocationAndRotation(Ring + FVector(bHead ? -Half - Len * 0.3 : Half + Len * 0.3, 0.0, 0.0), FRotator(-90.0, 0.0, 0.0));
			C->SetRelativeScale3D(FVector(Rad * 2.0 / 100.0, Rad * 2.0 / 100.0, Len / 100.0));
			C->SetMaterial(0, bHead ? Brass : Lead);
			KeepDrawnOverWorld(C);
			RoundParts[Hand * 12 + k * 2 + j] = C;
		}
	}
	HeldLight = -1.f;                      // the new parts take the current sector light
}

void AOutlawsPlayer::ShowRounds(int32 Hand, bool bShow)
{
	for (int32 i = Hand * 12; i < FMath::Min(RoundParts.Num(), Hand * 12 + 12); ++i)
	{
		if (RoundParts[i]) RoundParts[i]->SetVisibility(bShow);
	}
}

TSharedPtr<OutlawsObj::FObjMesh>* AOutlawsPlayer::GunPartModel(int32 Wi, const TCHAR* Suffix, const FVector& Pivot)
{
	// a sprite gun's part (<NAME>_hammer.obj and the like) about its pivot, read once a session (GObjCache)
	const FString Path = FPaths::Combine(HeldGunDir(Wi), FString(SpriteGunName(Wi)) + Suffix);
	TSharedPtr<OutlawsObj::FObjMesh>* Cached = GObjCache.Find(Path);
	if (!Cached)
	{
		TSharedPtr<OutlawsObj::FObjMesh> Obj = MakeShared<OutlawsObj::FObjMesh>();
		if ((FPaths::FileExists(Path) || FPaths::IsSamePath(FPaths::GetPath(Path), ShippedHeldDir())) && LoadHeldObj(Path, 1.0, true, *Obj))
		{
			for (TPair<FString, FOutlawsMeshSection>& Kv : Obj->Sections) for (FVector& P : Kv.Value.Positions) P -= Pivot;
		}
		else
		{
			Obj.Reset();
		}
		Cached = &GObjCache.Add(Path, Obj);
	}
	return Cached;
}

void AOutlawsPlayer::BuildGunParts(int32 Hand, int32 Wi)
{
	// the sprite gun's hammer(s) and trigger: own meshes (<NAME>_hammer.obj, _trigger.obj) on pivots under the gun,
	// sharing its texture; the VE guns and bare hands have none
	UProceduralMeshComponent* Gun = WeaponMesh[Hand];
	const bool bSprite = Wi >= 0 && Gun && !GWeapons[Wi].Model3do && UseSpriteGun(Wi);
	auto Part = [&](const TCHAR* Suffix, bool bHas, const FVector& Pivot, TObjectPtr<USceneComponent>& PivotC, TObjectPtr<UProceduralMeshComponent>& MeshC)
	{
		if (!bSprite || !bHas)
		{
			// the last gun's part cleared, not only hidden: the update shows any part that still has sections
			// (user 2026-09-28: the rifle's lever stayed on the revolver, the knife and the dynamite)
			if (MeshC) { MeshC->ClearAllMeshSections(); OutlawsInk::Forget(MeshC); MeshC->SetVisibility(false); }
			return;
		}
		TSharedPtr<OutlawsObj::FObjMesh>* Cached = GunPartModel(Wi, Suffix, Pivot);
		if (!Cached->IsValid())
		{
			if (MeshC) { MeshC->ClearAllMeshSections(); OutlawsInk::Forget(MeshC); MeshC->SetVisibility(false); }
			return;
		}
		if (!PivotC)
		{
			PivotC = NewObject<USceneComponent>(this);
			PivotC->SetupAttachment(Gun);
			PivotC->RegisterComponent();
			MeshC = NewObject<UProceduralMeshComponent>(this);
			MeshC->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			MeshC->SetCastShadow(false);
			MeshC->SetupAttachment(PivotC);
			MeshC->RegisterComponent();
		}
		PivotC->SetRelativeLocationAndRotation(Pivot, FQuat::Identity);
		MeshC->ClearAllMeshSections(); OutlawsInk::Forget(MeshC);
		int32 Ci = 0;
		for (const TPair<FString, FOutlawsMeshSection>& Kv : (*Cached)->Sections)
		{
			const FOutlawsMeshSection& S = Kv.Value;
			MeshC->CreateMeshSection_LinearColor(Ci, S.Positions, S.Triangles, S.Normals, S.UVs, S.Colors, TArray<FProcMeshTangent>(), false);
			if (UMaterialInterface* Mat = GunMaterials.FindRef(S.Texture)) MeshC->SetMaterial(Ci, Mat);
			++Ci;
		}
		MeshC->SetVisibility(Gun->IsVisible());
		KeepDrawnOverWorld(MeshC);
	};
	const FSpriteSights& Si = GSpriteSights[FMath::Clamp(Wi, 0, 7)];
	Part(TEXT("_hammer.obj"), Si.bHammer, Si.HammerPivot, HammerPivot[Hand], HammerMesh[Hand]);
	Part(TEXT("_trigger.obj"), Si.bTrigger, Si.TriggerPivot, TriggerPivot[Hand], TriggerMesh[Hand]);
	Part(TEXT("_lever.obj"), Si.bLever, Si.LeverPivot, LeverPivot[Hand], LeverMesh[Hand]);
	// the telescope on the rifle (user 2026-09-29: the scope item's model, fitted where a scope goes): its rings on the
	// receiver's flat top (4.9 cm over the grip line from 8 to 30 cm ahead of it), centred 24 cm ahead so the eyepiece
	// clears the hammer; shown while the scope is carried (UpdateGunParts).  (Placeholder art: only with the placeholder guns)
	if (bSprite && Wi == 1 && HeldGunSource(Wi) == EHeldGunSource::Dev)
	{
		const FString ScopePath = FPaths::Combine(FPaths::GetPath(SpriteHeldDir()), TEXT("GSCOPE.obj"));
		TSharedPtr<OutlawsObj::FObjMesh>* Cached = GObjCache.Find(ScopePath);
		if (!Cached)
		{
			TSharedPtr<OutlawsObj::FObjMesh> Obj = MakeShared<OutlawsObj::FObjMesh>();
			if (FPaths::FileExists(ScopePath) && OutlawsObj::Load(ScopePath, 1.0, true, *Obj))
			{
				for (TPair<FString, FOutlawsMeshSection>& Kv : Obj->Sections) for (FVector& P : Kv.Value.Positions) P += FVector(24.0, 0.0, 4.9);
				// (SCOPEFACES, user 27 Oct: the tube looked inside-out) the pickup's tube walls are wound inward and the held
				// material is one-sided: turned to face out.  -OLOldScopeFaces: as the file has them
				OutlawsObj::FaceItemModelOutward(*Obj, TEXT("OLRT held scope"));
			}
			else
			{
				Obj.Reset();
			}
			Cached = &GObjCache.Add(ScopePath, Obj);
		}
		if (Cached->IsValid())
		{
			if (!ScopeMesh[Hand])
			{
				ScopeMesh[Hand] = NewObject<UProceduralMeshComponent>(this);
				ScopeMesh[Hand]->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				ScopeMesh[Hand]->SetCastShadow(false);
				ScopeMesh[Hand]->SetupAttachment(Gun);
				ScopeMesh[Hand]->RegisterComponent();
			}
			UProceduralMeshComponent* SM = ScopeMesh[Hand];
			SM->ClearAllMeshSections(); OutlawsInk::Forget(SM);
			UMaterialInterface* Base = HeldBase();
			int32 Ci = 0;
			for (const TPair<FString, FOutlawsMeshSection>& Kv : (*Cached)->Sections)
			{
				const FOutlawsMeshSection& S = Kv.Value;
				SM->CreateMeshSection_LinearColor(Ci, S.Positions, S.Triangles, S.Normals, S.UVs, S.Colors, TArray<FProcMeshTangent>(), false);
				UMaterialInterface* Mat = GunMaterials.FindRef(S.Texture);
				if (!Mat && Base)
				{
					UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, this);
					if (UTexture2D* Tex = OutlawsObj::LoadTextureFile(S.Texture))
					{
						Mid->SetTextureParameterValue(TEXT("Tex"), Tex);
						KeepAlive.Add(Tex);
					}
					KeepAlive.Add(Mid);
					HeldMids.Add(Mid);
					if (HeldLight >= 0.f) Mid->SetScalarParameterValue(TEXT("Brightness"), HeldLight);
					GunMaterials.Add(S.Texture, Mid);
					Mat = Mid;
				}
				if (Mat) SM->SetMaterial(Ci, Mat);
				++Ci;
			}
			KeepDrawnOverWorld(SM);
		}
	}
	else if (ScopeMesh[Hand])
	{
		ScopeMesh[Hand]->ClearAllMeshSections(); OutlawsInk::Forget(ScopeMesh[Hand]);
		ScopeMesh[Hand]->SetVisibility(false);
	}
	// the barrel selector: a steel hub and a lever, on the gun's left as held (the left-hand gun is mirrored, so its
	// left is the model's +Y)
	if (bSprite && Si.bSelector)
	{
		if (!SelPivot[Hand])
		{
			UStaticMesh* Cube = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube"));
			UStaticMesh* Cyl = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
			UMaterialInterface* Steel = HeldSolidMaterial(FColor(58, 58, 62));
			UMaterialInterface* Bright = HeldSolidMaterial(FColor(150, 146, 136));
			SelPivot[Hand] = NewObject<USceneComponent>(this);
			SelPivot[Hand]->SetupAttachment(Gun);
			SelPivot[Hand]->RegisterComponent();
			auto Box = [&](UStaticMesh* Shape, const FVector& At, const FRotator& Rot, const FVector& Scale, UMaterialInterface* Mat)
			{
				UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this);
				C->SetStaticMesh(Shape);
				C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
				C->SetCastShadow(false);
				C->SetupAttachment(SelPivot[Hand]);
				C->RegisterComponent();
				C->SetRelativeLocationAndRotation(At, Rot);
				C->SetRelativeScale3D(Scale);
				C->SetMaterial(0, Mat);
				C->SetBoundsScale(60.f);
				C->SetRenderCustomDepth(true);
				C->SetCustomDepthStencilValue(1);
			};
			Box(Cyl, FVector::ZeroVector, FRotator(0.0, 0.0, 90.0), FVector(0.012, 0.012, 0.004), Steel);        // the hub (axis across the gun)
			Box(Cube, FVector(0.0, 0.0, 1.1), FRotator::ZeroRotator, FVector(0.005, 0.0035, 0.022), Steel);      // the lever
			Box(Cube, FVector(0.0, 0.0, 2.2), FRotator::ZeroRotator, FVector(0.009, 0.005, 0.006), Bright);      // its thumb tab
		}
		const bool bLeftHand = Hand == 0;
		SelPivot[Hand]->SetRelativeLocation(FVector(Si.Selector.X, bLeftHand ? Si.Selector.Y : -Si.Selector.Y, Si.Selector.Z));
		SelPivot[Hand]->SetVisibility(Gun->IsVisible(), true);
		SelPrevDist[Hand] = 99.f;
	}
	else if (SelPivot[Hand])
	{
		SelPivot[Hand]->SetVisibility(false, true);
	}
	HammerCock[Hand] = Mag[Hand] > 0 ? 1.f : 0.f;
	TriggerPull[Hand] = 0.f;
}

bool AOutlawsPlayer::GatlingOut() const
{
	for (int32 H = 0; H < 2; ++H)
	{
		if (Equipped[H] >= 0 && GWeapons[Equipped[H]].bAuto) return true;
	}
	return false;
}

void AOutlawsPlayer::EnsureGatTripod()
{
	if (GatTripod) return;
	auto Build = [this](const TCHAR* File, TObjectPtr<UProceduralMeshComponent>& Out)
	{
	OutlawsObj::FObjMesh Obj;
	const FString Path = FPaths::Combine(SpriteHeldDir(), File);
	if (!FPaths::FileExists(Path) || !OutlawsObj::Load(Path, 1.0, true, Obj)) return;
	Out = NewObject<UProceduralMeshComponent>(this);
	UProceduralMeshComponent* Part = Out;
	Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Part->SetCastShadow(false);
	Part->SetUsingAbsoluteLocation(true);
	Part->SetUsingAbsoluteRotation(true);
	Part->SetupAttachment(GetRootComponent());
	Part->RegisterComponent();
	const double Sc = 1.5;
	const FQuat Q = FQuat::FindBetweenNormals(FVector::RightVector, FVector::ForwardVector);
	UMaterialInterface* Base = HeldBase();
	int32 Ci = 0;
	for (TPair<FString, FOutlawsMeshSection>& Kv : Obj.Sections)
	{
		FOutlawsMeshSection& S = Kv.Value;
		for (FVector& P : S.Positions) P = Q.RotateVector(P) * Sc;
		for (FVector& N : S.Normals) N = Q.RotateVector(N);
		Part->CreateMeshSection_LinearColor(Ci, S.Positions, S.Triangles, S.Normals, S.UVs, S.Colors, TArray<FProcMeshTangent>(), false);
		UMaterialInterface* Mat = GunMaterials.FindRef(S.Texture);
		if (!Mat && Base)
		{
			UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, this);
			if (UTexture2D* Tex = OutlawsObj::LoadTextureFile(S.Texture))
			{
				Mid->SetTextureParameterValue(TEXT("Tex"), Tex);
				KeepAlive.Add(Tex);
			}
			KeepAlive.Add(Mid);
			HeldMids.Add(Mid);
			if (HeldLight >= 0.f) Mid->SetScalarParameterValue(TEXT("Brightness"), HeldLight);
			GunMaterials.Add(S.Texture, Mid);
			Mat = Mid;
		}
		if (Mat) Part->SetMaterial(Ci, Mat);
		++Ci;
	}
	KeepDrawnOverWorld(Part);
	Part->SetVisibility(false);
	};
	Build(TEXT("GGATGUN_tripod.obj"), GatTripod);
	Build(TEXT("GGATGUN_crank.obj"), GatCrank);
	Build(TEXT("GGATGUN_spin.obj"), GatSpin);
}

void AOutlawsPlayer::UpdateGunParts(int32 Hand, float Dt)
{
	// the trigger follows the finger on the controller's trigger; the hammer falls with each shot and comes back
	// cocked (thumb, fanning palm or lever) once the gun is ready again, and stays down while the gun is empty
	UProceduralMeshComponent* Gun = WeaponMesh[Hand];
	const bool bShow = Gun && Gun->IsVisible();
	const int32 Wi = Equipped[Hand];
	if (GatTripod && GatTripod->IsVisible() && GatShownFrame + 1 < GFrameCounter)
	{
		GatTripod->SetVisibility(false);
		if (GatCrank) GatCrank->SetVisibility(false);
		if (GatSpin) GatSpin->SetVisibility(false);
		bCranking = false;
	}
	if (ScopeMesh[Hand] && ScopeMesh[Hand]->GetNumSections() > 0)
	{
		const AOutlawsLevelActor* LS = Level.Get();
		ScopeMesh[Hand]->SetVisibility(bShow && Wi == 1 && LS && LS->GetCount(TEXT("iscope")) > 0);
	}
	if (TriggerMesh[Hand] && TriggerPivot[Hand] && TriggerMesh[Hand]->GetNumSections() > 0)
	{
		TriggerMesh[Hand]->SetVisibility(bShow);
		const float Want = (bMenuOpen || bWheelOpen) ? 0.f : Axis(Hand == 0 ? TEXT("OLVR_LeftTrigger") : TEXT("OLVR_RightTrigger"));
		TriggerPull[Hand] = FMath::FInterpTo(TriggerPull[Hand], Want, Dt, 30.f);
		TriggerPivot[Hand]->SetRelativeRotation(FRotator(-20.0 * TriggerPull[Hand], 0.0, 0.0));   // the blade swings back
	}
	if (HammerMesh[Hand] && HammerPivot[Hand] && HammerMesh[Hand]->GetNumSections() > 0 && Wi >= 0)
	{
		HammerMesh[Hand]->SetVisibility(bShow);
		const FWeaponDef& W = GWeapons[Wi];
		const float Cycle = FMath::Max(0.05f, W.FanCycle > 0.f ? FMath::Min(W.FanCycle, W.Cycle) : W.Cycle);
		const bool bReady = Mag[Hand] > 0 && FireCooldown[Hand] < Cycle * 0.6f;
		HammerCock[Hand] = bReady ? FMath::FInterpConstantTo(HammerCock[Hand], 1.f, Dt, 9.f) : FMath::Min(HammerCock[Hand], Mag[Hand] > 0 ? 1.f : 0.f);
		HammerPivot[Hand]->SetRelativeRotation(FRotator(-32.0 * (1.0 - HammerCock[Hand]), 0.0, 0.0));   // fallen = the spur forward
	}
	if (LeverMesh[Hand] && LeverPivot[Hand] && LeverMesh[Hand]->GetNumSections() > 0 && Wi >= 0)
	{
		// the rifle's lever worked after each shot (user 2026-09-28): down and forward through the first 45% of the
		// fire cycle, back up by 85%; the hammer re-cocks as it closes
		LeverMesh[Hand]->SetVisibility(bShow);
		const float Cycle = FMath::Max(0.05f, GWeapons[Wi].Cycle);
		const float T = FireCooldown[Hand] > 0.f ? FMath::Clamp(1.f - FireCooldown[Hand] / Cycle, 0.f, 1.f) : 1.f;
		const float Open = T < 0.45f ? FMath::InterpEaseOut(0.f, 1.f, T / 0.45f, 2.f) : FMath::InterpEaseInOut(1.f, 0.f, FMath::Clamp((T - 0.45f) / 0.4f, 0.f, 1.f), 2.f);
		LeverPivot[Hand]->SetRelativeRotation(FRotator(48.0 * Open, 0.0, 0.0));   // the loop's rear swings down
	}
}

void AOutlawsPlayer::MakeFuseFlame(USceneComponent* Parent, UStaticMeshComponent*& OutFlame, UStaticMeshComponent*& OutCore, bool bHeld)
{
	// held: pulled to the stick's depth (user 2026-10-01: on the Quest the held stick's flame did not show)
	UMaterialInterface* GlowBase = bHeld ? LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_FxGlowHeld.M_OLR_FxGlowHeld")) : nullptr;
	if (!GlowBase) GlowBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_FxGlow.M_OLR_FxGlow"));
	auto Glow = [this, GlowBase](const FLinearColor& C, float Strength) -> UMaterialInterface*
	{
		if (!GlowBase) return SolidMaterial(C.ToFColor(true));
		UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(GlowBase, this);
		M->SetVectorParameterValue(TEXT("Color"), C);
		M->SetScalarParameterValue(TEXT("Glow"), Strength);
		KeepAlive.Add(M);
		return M;
	};
	// the flame: the game's own animated fire (fire01, as the lamps' embers burn) on a quad, small (user 2026-10-01: the
	// fuse burning down must show past it); the core a bead of light on the wick's end.  Glowing spheres if missing
	AOutlawsLevelActor* L = Level.Get();
	// (user 2026-10-03, BUG-TNT: "the flame, when I light it, is also behind the stick") held, the level's fire card would
	// sit at its true depth behind the stick (pulled to the eye with the hands, M_OLR_Held).  (BUG-FUSEMATCH, user build 51:
	// the held flame was not the thrown one) held, the same fire card, frames and size on the held sprite material
	// (M_OLR_HeldSprite, as the held lantern's flame), pulled to the eye with the stick; thrown, the fire card as before
	UMaterialInstanceDynamic* Fire = L ? L->MakeFireLook(this, FireCells, FireFps, FireAspect) : nullptr;
	if (Fire && bHeld)
	{
		UMaterialInterface* HeldSprite = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_HeldSprite.M_OLR_HeldSprite"));
		UTexture* FireTex = nullptr;
		FLinearColor CellScale(1.f, 1.f, 0.f, 0.f);
		Fire->GetTextureParameterValue(FMaterialParameterInfo(TEXT("Tex")), FireTex);
		Fire->GetVectorParameterValue(FMaterialParameterInfo(TEXT("UVScale")), CellScale);
		UMaterialInstanceDynamic* Hf = HeldSprite && FireTex && FireCells.Num() > 0 ? UMaterialInstanceDynamic::Create(HeldSprite, this) : nullptr;
		if (Hf)
		{
			Hf->SetTextureParameterValue(TEXT("Tex"), FireTex);
			Hf->SetVectorParameterValue(TEXT("UVScale"), CellScale);
			Hf->SetVectorParameterValue(TEXT("UVOffset"), FireCells[0]);
			Hf->SetScalarParameterValue(TEXT("Brightness"), 1.f);         // (a flame: full bright, as the thrown one)
			Hf->SetScalarParameterValue(TEXT("Opacity"), 1.f);
		}
		Fire = Hf;
	}
	if (Fire && L->GetSpriteQuad())
	{
		KeepAlive.Add(Fire);
		OutFlame = MakeFx(Parent->GetOwner(), Parent, Fire, *L->GetSpriteQuad()->GetPathName());
	}
	else
	{
		OutFlame = MakeFx(Parent->GetOwner(), Parent, Glow(FLinearColor(1.f, 0.55f, 0.15f), 4.f), TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	}
	OutCore = MakeFx(Parent->GetOwner(), Parent, Glow(FLinearColor(1.f, 0.92f, 0.6f), 6.f), TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	for (UStaticMeshComponent* C : { OutFlame, OutCore })
	{
		// riding on the stick (MakeFx's parts are placed in world space); upright, set each frame.  BUG-FUSEENSURE: the
		// rotation relative too: a held stick is under the hand's motion controller, whose late update does not take a
		// part in world space (an ensure and a 0.2 s hitch as the stick came into the hand); PlaceFuse sets the world
		// rotation each frame all the same
		C->SetUsingAbsoluteLocation(false);
		C->SetUsingAbsoluteRotation(false);
		C->SetUsingAbsoluteScale(false);
	}
}

void AOutlawsPlayer::PlaceFuse(UProceduralMeshComponent* StickMesh, int32 Section, const TArray<FVector>& Pos, const TArray<FVector>& Nrm,
	const TArray<FVector2D>& UV, const TArray<FLinearColor>& Col, float F, bool bWick, UStaticMeshComponent* Flame, UStaticMeshComponent* Core, float Size)
{
	if (!StickMesh) return;
	if (bWick && Section >= 0 && Section < StickMesh->GetNumSections())
	{
		TArray<FVector> P = Pos;
		for (FVector& V : P) if (V.Z > GFusePivotZ) V.Z = GFusePivotZ + (V.Z - GFusePivotZ) * F;
		StickMesh->UpdateMeshSection_LinearColor(Section, P, Nrm, UV, Col, TArray<FProcMeshTangent>());
	}
	if (!Flame || !Core) return;
	// on the wick's end, a tongue of flame that flickers and always rises (the stick may tumble)
	const double Top = GFusePivotZ + (GFuseTipZ - GFusePivotZ) * F;
	const FTransform Xf = StickMesh->GetComponentTransform();
	const FVector End = Xf.TransformPosition(FVector(0.0, 0.0, Top));
	const float T = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	// (BUG-TNTWHITE) the fire card only on the sprite quad: the held flame is a glow sphere whose material is a dynamic
	// instance too, and once FireCells was filled (a lantern, a thrown stick) it took the card's scale, 1 x 2.6 x 2.6 on
	// a 100 cm sphere, a glowing shell metres across round the eyes: the screen went white
	const AOutlawsLevelActor* Lv = Level.Get();
	const bool bCard = Lv && Lv->GetSpriteQuad() && Flame->GetStaticMesh() == Lv->GetSpriteQuad();
	UMaterialInstanceDynamic* Fire = bCard ? Cast<UMaterialInstanceDynamic>(Flame->GetMaterial(0)) : nullptr;
	if (Fire && FireCells.Num() > 0)
	{
		// the fire sprite: 2.6 cm tall times Size (1.8 in the hand and thrown: BUG-FUSEMATCH), its foot just under the wick's end
		// so the wick shows burning down below it; turned to the eyes; its frames at the sprite's own rate
		const float Hh = 2.6f * Size * (0.94f + 0.08f * FMath::Sin(T * 29.f));
		const FVector To = Camera->GetComponentLocation() - End;
		Flame->SetWorldLocationAndRotation(End - FVector(0.0, 0.0, 0.2 * Hh), FRotator(0.0, FMath::RadiansToDegrees(FMath::Atan2(To.Y, To.X)), 0.0));
		Flame->SetWorldScale3D(FVector(1.0, Hh * FireAspect, Hh));
		Fire->SetVectorParameterValue(TEXT("UVOffset"), FireCells[int32(FMath::FloorToFloat(T * FireFps)) % FireCells.Num()]);
	}
	else
	{
		const float Flick = (0.8f + 0.25f * FMath::Sin(T * 37.f) + 0.15f * FMath::Sin(T * 23.f + 1.3f) + FMath::FRandRange(-0.08f, 0.08f)) * Size;
		Flame->SetWorldLocationAndRotation(End + FVector(0.0, 0.0, 0.9 * Flick), FRotator::ZeroRotator);
		Flame->SetWorldScale3D(FVector(0.014, 0.014, 0.026) * Flick);
	}
	// the burning end: a bead of light on the wick, and sparks spat from it
	Core->SetWorldLocationAndRotation(End, FRotator::ZeroRotator);
	Core->SetWorldScale3D(FVector(0.0045 * Size));
	Flame->SetVisibility(true);
	Core->SetVisibility(true);
	EmitFuseSparks(End, GetWorld() ? GetWorld()->GetDeltaSeconds() : 0.016f, Size);
}

void AOutlawsPlayer::EmitFuseSparks(const FVector& At, float Dt, float Size)
{
	// about 30 a second from each burning fuse, thrown up and out 50..150 cm/s, living a quarter to half a second
	SparkDue += Dt * 30.f;
	if (SparkBeads.Num() == 0)
	{
		UMaterialInterface* GlowBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_FxGlow.M_OLR_FxGlow"));
		UMaterialInterface* Mat = nullptr;
		if (GlowBase)
		{
			UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(GlowBase, this);
			M->SetVectorParameterValue(TEXT("Color"), FLinearColor(1.f, 0.78f, 0.35f));
			M->SetScalarParameterValue(TEXT("Glow"), 5.f);
			KeepAlive.Add(M);
			Mat = M;
		}
		else
		{
			Mat = SolidMaterial(FColor(255, 200, 90));
		}
		for (int32 i = 0; i < 48; ++i) SparkBeads.Add(MakeFx(this, RootComponent, Mat, TEXT("/Engine/BasicShapes/Sphere.Sphere")));
	}
	while (SparkDue >= 1.f)
	{
		SparkDue -= 1.f;
		int32 Free = -1;
		for (int32 b = 0; b < SparkBeads.Num(); ++b)
		{
			if (!FuseSparks.ContainsByPredicate([b](const FFuseSpark& S) { return S.Bead == b; })) { Free = b; break; }
		}
		if (Free < 0) return;
		FFuseSpark S;
		S.Bead = Free;
		S.P = At;
		const FVector Out = FMath::VRand() * FVector(1.0, 1.0, 0.0);
		S.V = (Out * FMath::FRandRange(20.f, 70.f) + FVector(0.0, 0.0, FMath::FRandRange(40.f, 120.f))) * FMath::Sqrt(Size);
		S.Life = S.Life0 = FMath::FRandRange(0.25f, 0.5f);
		FuseSparks.Add(S);
	}
}

void AOutlawsPlayer::UpdateFuseSparks(float Dt)
{
	for (int32 i = FuseSparks.Num() - 1; i >= 0; --i)
	{
		FFuseSpark& S = FuseSparks[i];
		UStaticMeshComponent* B = SparkBeads.IsValidIndex(S.Bead) ? SparkBeads[S.Bead].Get() : nullptr;
		S.Life -= Dt;
		if (!B || S.Life <= 0.f)
		{
			if (B) B->SetVisibility(false);
			FuseSparks.RemoveAt(i);
			continue;
		}
		S.V.Z -= 980.f * Dt;
		S.V *= FMath::Exp(-1.5f * Dt);
		S.P += S.V * Dt;
		B->SetWorldLocation(S.P);
		B->SetWorldScale3D(FVector(0.0035 * FMath::Clamp(S.Life / S.Life0, 0.2f, 1.f)));
		B->SetVisibility(true);
	}
}

void AOutlawsPlayer::UpdateFuseLook(int32 Hand)
{
	// lit: the wick's height over the cap in step with the fuse left (4.5 s), and a flickering flame on its end
	UProceduralMeshComponent* Gun = WeaponMesh[Hand];
	const bool bLit = bFuseLit[Hand] && Mag[Hand] > 0 && Gun && Gun->IsVisible();
	const float F = bLit ? FMath::Clamp(Fuse[Hand] / 4.5f, 0.04f, 1.f) : 1.f;
	if (!FuseFlame[Hand] && Gun)
	{
		UStaticMeshComponent* Fl = nullptr;
		UStaticMeshComponent* Co = nullptr;
		MakeFuseFlame(Gun, Fl, Co);
		FuseFlame[Hand] = Fl;
		FuseCore[Hand] = Co;
	}
	const bool bWick = FMath::Abs(F - FuseShown[Hand]) > 0.005f;
	if (bWick) FuseShown[Hand] = F;
	if (!bLit)
	{
		if (Gun && bWick) PlaceFuse(Gun, FuseSection[Hand], FuseRestPos[Hand], FuseRestNrm[Hand], FuseRestUV[Hand], FuseRestCol[Hand], F, true, nullptr, nullptr);
		if (FuseFlame[Hand]) FuseFlame[Hand]->SetVisibility(false);
		if (FuseCore[Hand]) FuseCore[Hand]->SetVisibility(false);
		return;
	}
	// (BUG-FUSEMATCH) the thrown stick's flame size in the hand too
	PlaceFuse(Gun, FuseSection[Hand], FuseRestPos[Hand], FuseRestNrm[Hand], FuseRestUV[Hand], FuseRestCol[Hand], F, bWick, FuseFlame[Hand], FuseCore[Hand], 1.8f);
}

AOutlawsEnemy* AOutlawsPlayer::KnifeTargetNear(const FVector& From, const FVector& Vel) const
{
	// where the throw lands: the flight stepped under its gravity to the first thing it meets (as the arc shows)
	const AOutlawsLevelActor* L = Level.Get();
	UWorld* World = GetWorld();
	if (!L || !World) return nullptr;
	FVector P = From, V = Vel;
	const float G = ThrowGravity(From);     // (every throw's own)
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsKnifeAim), true, this);
	for (int32 i = 0; i < 80; ++i)
	{
		const FVector Next = P + V * 0.05f;
		V.Z -= G * 0.05f;
		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, P, Next, ECC_Visibility, Q))
		{
			P = Hit.ImpactPoint;
			break;
		}
		P = Next;
	}
	// the nearest live bad guy within 4 m of it (user 2026-09-29; it was 150 cm), measured to his body's axis
	AOutlawsEnemy* Best = nullptr;
	float BestD = 400.f;
	for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
	{
		AOutlawsEnemy* E = Wk.Get();
		if (!E || E->IsDead() || E->IsDormant()) continue;
		const FVector C = E->GetActorLocation();
		const float Half = E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		const FVector Near = FMath::ClosestPointOnSegment(P, C - FVector(0.0, 0.0, Half), C + FVector(0.0, 0.0, Half));
		const float D = static_cast<float>(FVector::Dist(Near, P));
		if (D < BestD) { BestD = D; Best = E; }
	}
	return Best;
}

bool AOutlawsPlayer::MeleeDynamite(const FVector& At)
{
	// what the blow lands on, within a fist's reach: the level's dynamite (its crates and sticks answer shot traces) or
	// a thrown stick, lit or not (user 2026-09-28)
	AOutlawsLevelActor* L = Level.Get();
	UWorld* World = GetWorld();
	if (!L || !World) return false;
	TArray<FOverlapResult> Found;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsMeleeDynamite), true, this);
	if (!World->OverlapMultiByChannel(Found, At, FQuat::Identity, ECC_Visibility, FCollisionShape::MakeSphere(16.f), Q)) return false;
	for (const FOverlapResult& O : Found)
	{
		if (L->HitDynamite(O.GetComponent()) || ShootThrown(O.GetActor())) return true;
	}
	return false;
}

void AOutlawsPlayer::BlastNear(const FVector& Where, float R)
{
	for (FThrown& T : Thrown)
	{
		if (T.Type != 1 || !T.Actor.IsValid()) continue;
		const float D = float(FVector::Dist(T.Pos, Where));
		if (D >= R || D < 1.f) continue;            // (not the stick that just went: it is at the blast's own spot)
		const float After = 0.12f + FMath::FRandRange(0.f, 0.08f) + 0.1f * D / R;
		if (T.Fuse < 0.f || T.Fuse > After) T.Fuse = After;
	}
}

void AOutlawsPlayer::BlastThrow(const FVector& From, float Dealt)
{
	// the blast throws the body up and away as it throws a bad guy (AOutlawsEnemy::Knock: 360 cm/s a point of damage,
	// 0.7 up, 5200 at most; user 2026-09-29: TNT should blast the player across the map too)
	if (Dealt <= 0.f || MantleLeft > 0.f || bDeadView) return;
	if (IsClimbing()) ReleaseGrab(false);
	const FVector Dir = ((GetActorLocation() - From).GetSafeNormal2D() + FVector(0.0, 0.0, 0.7)).GetSafeNormal();
	const FVector V = (Dir * Dealt * 360.f).GetClampedToMaxSize(5200.f);
	UCharacterMovementComponent* M = GetCharacterMovement();
	if (bSwimming && M->MovementMode == MOVE_Flying)
	{
		M->Velocity += V * 0.5;                  // the water takes half of it
	}
	else
	{
		bJumpBoost = false;                // (a blast in a long jump of the lower speeds: the throw under the world's own gravity)
		JumpStretch = 1.f;
		LaunchCharacter(V, true, true);
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT blast throws the player at %.0f cm/s (up %.0f)"), V.Size(), V.Z);
}

bool AOutlawsPlayer::ShootThrown(AActor* A)
{
	// a thrown stick of dynamite hit by a shot goes off, lit or not (user 2026-09-28)
	if (!A) return false;
	for (int32 i = 0; i < Thrown.Num(); ++i)
	{
		FThrown& T = Thrown[i];
		if (T.Type == 3 && T.Actor.Get() == A)
		{
			// a bottle, flying or set down, bursts
			if (AOutlawsLevelActor* L = Level.Get()) L->ShatterGlass(Cast<UProceduralMeshComponent>(A->GetRootComponent()), FVector::ZeroVector, T.BreakWav);
			Thrown.RemoveAt(i);
			A->Destroy();
			return true;
		}
		if (T.Type != 1 || T.Actor.Get() != A) continue;
		const FVector Where = T.Pos;
		if (T.Sound.IsValid()) T.Sound->Stop();
		Thrown.RemoveAt(i);
		A->Destroy();
		if (AOutlawsLevelActor* L = Level.Get()) L->Explode(Where);
		return true;
	}
	return false;
}

bool AOutlawsPlayer::TradeWeapons()
{
	// two different weapons trade hands (user 2026-09-28; any two since 2026-10-01), each with its rounds and barrel setting
	const int32 A = Equipped[0], B = Equipped[1];
	if (A == B || A < 0 || B < 0) return false;
	const int32 MagA = Mag[0], MagB = Mag[1];
	const bool bOneA = bOneBarrel[0], bOneB = bOneBarrel[1];
	Equip(0, -1);
	Equip(1, -1);
	SavedMag.Add(1 * 16 + A, MagA);
	SavedMag.Add(0 * 16 + B, MagB);
	Equip(1, A);
	Equip(0, B);
	bOneBarrel[1] = bOneA;
	bOneBarrel[0] = bOneB;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT traded: %s to the right hand, %s to the left"), GWeapons[A].Label, GWeapons[B].Label);
	return true;
}

bool AOutlawsPlayer::ToggleBarrels()
{
	int32 First = -1;
	for (int32 H = 0; H < 2; ++H) if (First < 0 && Equipped[H] >= 0 && GWeapons[Equipped[H]].RoundsPerShot > 1) First = H;
	if (First < 0) return false;
	const bool bOne = !bOneBarrel[First];
	for (int32 H = 0; H < 2; ++H)
	{
		if (Equipped[H] >= 0 && GWeapons[Equipped[H]].RoundsPerShot > 1)
		{
			bOneBarrel[H] = bOne;
			Pulse(H, 0.3f);
		}
	}
	if (AOutlawsLevelActor* L = Level.Get(); L && WeaponMesh[First]) L->PlaySoundAt(TEXT("weapgrab"), WeaponMesh[First]->GetComponentLocation());
	ShowMessage(bOne ? TEXT("One barrel") : TEXT("Both barrels"), 1.2f);
	return true;
}

void AOutlawsPlayer::UpdateSelector(int32 Hand, float Dt)
{
	// the other hand (empty, or holding its own gun) moving through the lever flicks it; it must leave again before
	// the next flick, and a slow drift does nothing (two sawed-offs held side by side must not keep switching)
	USceneComponent* Sel = SelPivot[Hand];
	UProceduralMeshComponent* Gun = WeaponMesh[Hand];
	const int32 Wi = Equipped[Hand];
	if (!Sel || !Gun || Wi < 0 || GWeapons[Wi].RoundsPerShot < 2)
	{
		if (Sel) Sel->SetVisibility(false, true);
		return;
	}
	Sel->SetVisibility(Gun->IsVisible(), true);
	const int32 O = 1 - Hand;
	const FVector Tab = Sel->GetComponentTransform().TransformPosition(FVector(0.0, 0.0, 2.0));
	const FVector A = GripOf(O)->GetComponentLocation();
	const FVector B = A + FrameOf(O)->GetForwardVector() * 9.0;       // the fist to the fingertips (or its gun's frame)
	const FVector Near = FMath::ClosestPointOnSegment(Tab, A, B);
	const FVector Rel = Near - Tab;
	const float D = static_cast<float>(Rel.Size());
	const float Speed = static_cast<float>((Rel - SelPrevRel[Hand]).Size()) / FMath::Max(Dt, 1e-3f);
	if (D < 4.5f && SelPrevDist[Hand] >= 4.5f && Speed > 25.f && !bMenuOpen && !bWheelOpen && GrabHand != O)
	{
		bOneBarrel[Hand] = !bOneBarrel[Hand];
		Pulse(Hand, 0.35f);
		Pulse(O, 0.2f);
		if (AOutlawsLevelActor* L = Level.Get()) L->PlaySoundAt(TEXT("weapgrab"), Tab);
		UE_LOG(LogOutlaws, Log, TEXT("OLRT %s hand: %s, %s"), Hand == 0 ? TEXT("left") : TEXT("right"), GWeapons[Wi].Label,
			bOneBarrel[Hand] ? TEXT("one barrel") : TEXT("both barrels"));
	}
	SelPrevDist[Hand] = D;
	SelPrevRel[Hand] = Rel;
	// forward = both barrels, back = one (the lever's tip toward the stock)
	SelAngle[Hand] = FMath::FInterpTo(SelAngle[Hand], bOneBarrel[Hand] ? 35.f : -35.f, Dt, 22.f);
	Sel->SetRelativeRotation(FRotator(SelAngle[Hand], 0.0, 0.0));
}

float AOutlawsPlayer::TriggerFingerCurl(int32 Hand) const
{
	const int32 Wi = Equipped[Hand];
	if (Wi < 0 || GWeapons[Wi].Throw > 0 || GWeapons[Wi].bAuto || FanHide[Hand] > 0.f || bToolSupport[Hand] || bSupport[Hand] || bMenuPistol)
	{
		return -1.f;
	}
	// resting on the trigger (the curl the gun is seated for), pulled back with the controller's trigger
	return 0.28f + 0.34f * FMath::Clamp(Axis(Hand == 0 ? TEXT("OLVR_LeftTrigger") : TEXT("OLVR_RightTrigger")), 0.f, 1.f);
}

void AOutlawsPlayer::KeepDrawnOverWorld(UPrimitiveComponent* C)
{
	if (!C) return;
	C->SetRenderCustomDepth(true);
	C->SetCustomDepthStencilValue(1);        // held gear: the ink line post-process outlines stencil 1
	// the headset has no post process: a shell instead, as the hands have (0.15 cm, theirs 0.14)
	if (OutlawsInk::Wanted())
	{
		AOutlawsPlayer* Self = Cast<AOutlawsPlayer>(C->GetOwner());     // (static: the gear's owner is the player)
		UProceduralMeshComponent* P = Cast<UProceduralMeshComponent>(C);
		if (Self && P)
		{
			if (!Self->GearInk) Self->GearInk = Self->HeldSolidMaterial(FColor(10, 8, 7));
			OutlawsInk::AddShell(P, 0.15f, Self->GearInk);
		}
	}
	C->UpdateBounds();
	const float R = C->Bounds.SphereRadius / FMath::Max(0.01f, C->BoundsScale);
	C->SetBoundsScale(FMath::Max(1.f, 200.f / FMath::Max(R, 0.5f)));
}

void AOutlawsPlayer::HoldMenuPistol()
{
	// the title screens' pointer (user 2026-10-01: not a revolver, the dominant hand's pointing finger, and its laser
	// alone): both hands bare, the dominant one pointing (UpdateHands), the menu's laser out of its fingertip (UpdateMenu)
	ResetForLevel();
	bMenuPistol = true;
	Equip(0, -1);
	Equip(1, -1);
	// (nothing carried over from the game just left: stood up, the breath's and the badge's veils gone)
	SetCrouchHeld(false);
	UpdateBreathVeil(0.f);
	if (BadgeVeil) BadgeVeil->SetVisibility(false);
}

bool AOutlawsPlayer::UpdateFan(int32 H, bool bPutAway)
{
	// fan fire: the other hand, bare (a fist: nothing in it; two revolvers do not fan, user 2026-09-26), sweeps fast past the
	// hammer
	if (Equipped[H] < 0 || !WeaponMesh[H]) return false;
	const FWeaponDef& W = GWeapons[Equipped[H]];
	UProceduralMeshComponent* Gun = WeaponMesh[H];
	const int32 O = 1 - H;
	// (user 2026-10-04, REQ-FANPALM: hands brought together, one resting on the other, fanned now and then) the free hand's
	// palm facing down may fan; facing up (cupped under the gun, holding it up) never starts one.  Up from 0.35 of the way
	// to straight up, down again under 0.15: the band between keeps it from chattering
	const double PalmUp = FVector::DotProduct(PalmDir(O), FVector::UpVector);
	if (PalmUp > 0.35) bFanPalmUp[O] = true;
	else if (PalmUp < 0.15) bFanPalmUp[O] = false;
	// (never with a bad guy held in either hand: passing him to the other hand fanned the gun into him, user 2026-10-01)
	if (W.FanCycle > 0.f && !bPutAway && Equipped[O] < 0 && !HeldItem[O].IsValid() && !HeldBottle[O] && HeldTool[O].IsEmpty() && GrabHand != O && !bWheelHeld[O]
		&& !GrabbedEnemy[0].IsValid() && !GrabbedEnemy[1].IsValid() && !bFanPalmUp[O])
	{
		// the palm anywhere along the back and top of the gun, from just past the hammer down to
		// behind the grip, sweeping back (toward the shooter) or down, the way a hammer is raked; the
		// return stroke forward or up does not fire (user 2026-09-27)
		const FTransform GunXf = Gun->GetComponentTransform();
		const FVector ZoneA = GunXf.TransformPosition(W.Hammer + FVector(5.0, 0.0, 4.0));
		const FVector ZoneB = GunXf.TransformPosition(W.Hammer + FVector(-14.0, 0.0, -8.0));
		const FVector Palm = GripOf(O)->GetComponentLocation();
		const FVector Rel = HandVelocity[O] - HandVelocity[H];
		const float Rake = FMath::Max(FVector::DotProduct(Rel, -GunXf.GetUnitAxis(EAxis::X)), FVector::DotProduct(Rel, -GunXf.GetUnitAxis(EAxis::Z)));
		const bool bRake = Rake > 80.f && Rake > 0.6f * Rel.Size();      // mostly back or down, not a sideways brush
		if (FMath::PointDistToSegment(Palm, ZoneA, ZoneB) < 13.0 && bRake && FireCooldown[H] <= 0.f)
		{
			FanActive[H] = 0.6f;
			FireGun(H, true);
			if (Equipped[O] >= 0) FanHide[O] = 0.6f;
			return true;
		}
	}
	return false;
}

AOutlawsPlayer::FWeaponDef_Public AOutlawsPlayer::WeaponFanZone(int32 Weapon)
{
	FWeaponDef_Public Out;
	if (Weapon >= 0 && Weapon < int32(UE_ARRAY_COUNT(GWeapons))) Out.Hammer = GWeapons[Weapon].Hammer;
	return Out;
}

void AOutlawsPlayer::EnsureShotFx(int32 Hand)
{
	// the hand's muzzle flash (a glowing plume along the barrel and a star across it), its faint tracer and the spark where
	// it hit: made on the hand's first shot, or behind the load (FIRSTHITCH)
	if (MuzzleFx[Hand]) return;
	UMaterialInterface* GlowBase = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_FxGlow.M_OLR_FxGlow"));
	auto Glow = [this, GlowBase](const FLinearColor& C, float Strength) -> UMaterialInterface*
	{
		if (!GlowBase) return SolidMaterial(C.ToFColor(true));
		UMaterialInstanceDynamic* M = UMaterialInstanceDynamic::Create(GlowBase, this);
		M->SetVectorParameterValue(TEXT("Color"), C);
		M->SetScalarParameterValue(TEXT("Glow"), Strength);
		KeepAlive.Add(M);
		return M;
	};
	const TCHAR* Sphere = TEXT("/Engine/BasicShapes/Sphere.Sphere");
	MuzzleFx[Hand] = MakeFx(this, VROrigin, Glow(FLinearColor(1.f, 0.62f, 0.22f), 4.f), Sphere);
	FlashStar[Hand] = MakeFx(this, VROrigin, Glow(FLinearColor(1.f, 0.85f, 0.5f), 3.f), Sphere);
	TracerFx[Hand] = MakeFx(this, VROrigin, Glow(FLinearColor(1.f, 0.9f, 0.6f), 0.6f));
	SparkFx[Hand] = MakeFx(this, VROrigin, Glow(FLinearColor(1.f, 0.7f, 0.3f), 3.f), Sphere);
}

void AOutlawsPlayer::FireGun(int32 Hand, bool bFan)
{
	++GunFireCalls;                  // (REQ-BUGSNAP's test: no trigger reached the gun while the chord formed)
	if (!bCrankShot && UOutlawsBeacon::HandOnBubble(this, Hand)) return;      // (R11: the trigger is the join bubble's)
	if (!bCrankShot && AOutlawsTutorialCard::HandOnCard(Hand)) return;       // (REQ-TUTORIAL2: the trigger is the tutorial card's)
	// on the title screens (and under a menu or a movie) the trigger picks, it does not shoot
	if (bMenuPistol || bMenuOpen) return;
	if (AOutlawsLevelActor* LF = Level.Get(); LF && (LF->IsFrontEnd() || LF->IsMoviePlaying())) return;
	const int32 Wi = Equipped[Hand];
	UWorld* World = GetWorld();
	AOutlawsLevelActor* L = Level.Get();
	if (Wi < 0 || !World || !WeaponMesh[Hand] || FireCooldown[Hand] > 0.f)
	{
		return;
	}
	const FWeaponDef& W = GWeapons[Wi];
	UProceduralMeshComponent* Gun = WeaponMesh[Hand];
	const FVector Muzzle = Gun->GetComponentTransform().TransformPosition(W.Muzzle);
	if (bBreakOpen[Hand])
	{
		// (REQ-FLICKRELOAD) broken open: the hammers fall on nothing, nothing fires
		Pulse(Hand, 0.1f);
		FireCooldown[Hand] = 0.25f;
		return;
	}
	bButtonReload[Hand] = false;        // firing stops a button reload
	// no gun fires underwater: the hammer just clicks, as on an empty chamber (user 2026-09-27)
	FLinearColor WaterAtMuzzle;
	const bool bWet = L && L->UnderwaterTintAt(Muzzle, WaterAtMuzzle);
	if (Mag[Hand] <= 0 || bWet)
	{
		if (L) L->PlaySoundAt(W.EmptySound, Muzzle);
		Pulse(Hand, 0.2f);
		FireCooldown[Hand] = 0.25f;
		return;
	}
	// the double guns let both barrels go (as many as are loaded), or one with the selector back (the original's
	// secondary fire)
	const bool bOne = bOneBarrel[Hand] && W.RoundsPerShot > 1;
	const int32 Rounds = bOne ? 1 : FMath::Clamp(W.RoundsPerShot > 0 ? W.RoundsPerShot : 1, 1, Mag[Hand]);
	if (!bUnlimitedAmmo)
	{
		Mag[Hand] -= Rounds;
	}
	++TutNotes.Shots;                                // (REQ-TUTORIAL)
	if (GrabbedEnemy[1 - Hand].IsValid()) ++TutNotes.ShotsHolding;
	CylSpinTarget[Hand] += Cylinder().Chamber;       // cocking turns the cylinder a chamber
	if (W.bAuto && !bCrankShot) GatCrankAngle += 60.f;   // the gatling's crank comes round a sixth with each shot
	if (W.bAuto) OutlawsAchievements::NoteGatlingFired(L);   // (REQ-EXTRAS)
	FireCooldown[Hand] = bFan ? W.FanCycle : W.Cycle;
	HammerCock[Hand] = 0.f;                          // the hammer falls
	// the shot: along the barrel, with the weapon's aim variance (fanning is wilder)
	float V = bFan ? W.Variance + 1.0f : W.Variance;
	const bool bOneHanded = W.bTwoHand && !bSupport[1 - Hand] && OneHand() < 0;    // (one controller: no penalty, REQ-ONECTRL)
	if (bOneHanded)
	{
		V = FMath::Max(V * 4.f, 2.f);      // a long gun in one hand: a quarter of the accuracy (at least 2 degrees off; user 2026-09-28)
	}
	V += (100.f - Stamina) * 0.05f;       // a tired aim [EXE 0x1400de9d1]
	// the revolver steadied by the other hand close to it (resting on it, or both hands round the grip), whatever that
	// hand holds (another revolver too): a quarter better aimed, half the kick (user 2026-09-28); not while fanning,
	// nor with that hand on a ledge
	const int32 Other = 1 - Hand;
	const bool bSteadied = Wi == 0 && !bFan && GrabHand != Other
		&& FVector::Dist(GripOf(Hand)->GetComponentLocation(), GripOf(Other)->GetComponentLocation()) < 16.0;
	if (bSteadied)
	{
		V /= 1.25f;
	}
	if (bScoped[Hand])
	{
		V = 0.f;                           // scoped, the original skips the aim jitter (and its tiredness)
	}
	// and a tired shot carries less far (INFERRED: to 70 percent spent; scoped shots keep theirs)
	const float Reach = bScoped[Hand] ? W.RangeCm : W.RangeCm * FMath::Lerp(0.7f, 1.f, StaminaFrac());
	// the barrel: the model's own bore direction (BoreFix levels it in the hand) carried into the world,
	// so the shot goes where the barrel points, not along the tilted mounting axis
	const FVector ModelBore = W.BoreFix.Quaternion().Inverse().RotateVector(FVector::ForwardVector);
	FVector ShotDir = Gun->GetComponentTransform().TransformVector(ModelBore).GetSafeNormal();
	// scoped, the scope's middle is where the shot goes, above all else (user 2026-09-28): from the scope's own eye point
	// along its steadied look, no scatter, no aim assist
	const bool bScopedShot = bScoped[Hand] && bScopeAimSet && ScopeCapture;
	const FVector From = bScopedShot ? ScopeCapture->GetComponentLocation() : Muzzle;
	if (bScopedShot)
	{
		ShotDir = ScopeAim.GetForwardVector();
	}
	if (!bScopedShot && bFan)
	{
		// fanning's reward: the shot flies at the bad guy nearest the middle of the view
		if (AOutlawsEnemy* E = FindFanTarget(Hand))
		{
			ShotDir = (E->GetActorLocation() + FVector(0.0, 0.0, 25.0) - Muzzle).GetSafeNormal();
		}
	}
	else if (AOutlawsEnemy* E = bScopedShot ? nullptr : FindViewTarget(AimAssistViewCone, W.RangeCm, Muzzle))
	{
		// aim assist: the bad guy being looked at takes a shot aimed near enough to him
		const FVector To = (E->GetActorLocation() + FVector(0.0, 0.0, 25.0) - Muzzle).GetSafeNormal();
		if (FVector::DotProduct(To, ShotDir) >= FMath::Cos(FMath::DegreesToRadians(AimAssistBoreCone)))
		{
			ShotDir = To;
		}
	}
	// the barrel at one's own head (user 2026-10-02): the bore's line through the head, the muzzle within arm's reach of
	// it: death at once, once the shot is done (next tick: the gun is still in use below)
	if (!bScopedShot && L)
	{
		const FVector Head = Camera->GetComponentLocation() - Camera->GetForwardVector() * 8.0;
		const double Along = FVector::DotProduct(Head - Muzzle, ShotDir);
		if (Along > 0.0 && Along < 90.0 && FVector::DistSquared(Muzzle + ShotDir * Along, Head) < FMath::Square(12.0))
		{
			TWeakObjectPtr<AOutlawsLevelActor> WeakL = L;
			GetWorldTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(this, [WeakL]() { if (AOutlawsLevelActor* LK = WeakL.Get()) LK->ShotOwnHead(); }));
		}
	}
	if (L) UOutlawsStats::NoteShotFired(L);   // (the level's end panels: a shot, its pellets' hits below on target)
	// every pellet (a shotgun's SHOT_MULTIPLES, both barrels' worth) scatters within SPREAD_1 about the
	// aim; the first one is the one the flash, tracer and smoke follow
	const int32 Pellets = FMath::Max(1, W.Pellets) * Rounds;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsShot), true, this);
	if (const AOutlawsLevelActor* LQ = Level.Get()) LQ->IgnoreHiddenOverlaps(Q);     // the hidden side of overlapping rooms
	FHitResult Hit;
	bool bHit = false, bProp = false;
	FVector Impact = Muzzle;
	int32 Splashes = 0;                  // a shot into water splashes where it goes in (a shotgun's first few pellets)
	for (int32 p = 0; p < Pellets; ++p)
	{
		const float S = bScopedShot ? 0.f : (bOne && Wi == 4 ? 10.f : W.Spread) * (bOneHanded ? 4.f : 1.f);   // the sawed-off's one barrel: SPREAD_2 10 (both: 18); one hand on a long gun: 4 x
		const FRotator Aim = ShotDir.Rotation() + FRotator(FMath::FRandRange(-V, V) + FMath::FRandRange(-S, S), FMath::FRandRange(-V, V) + FMath::FRandRange(-S, S), 0.0);
		const FVector End = From + Aim.Vector() * Reach;
		FHitResult PHit;
		const bool bPHit = World->LineTraceSingleByChannel(PHit, From, End, ECC_Visibility, Q);
		FVector PImpact = bPHit ? FVector(PHit.ImpactPoint) : End;
		FVector Splash;
		if (L && Splashes < 4 && L->WaterSurfaceHit(From, PImpact, Splash))
		{
			L->SpawnSplash(Splash, false, Splashes == 0);
			++Splashes;
		}
		// the hanging lamps along the shot's way: swung, a chandelier's shade broken (user 2026-10-01)
		if (L) L->ShotNearHanging(From, PImpact);
		// a breakable held in a hand (a bottle, a vase), the shot through it before anything else: it bursts and the
		// pellet stops in it (user 2026-10-01).  (Not when the muzzle is inside its box: the gun held against it)
		bool bInHeld = false;
		for (int32 Hb = 0; Hb < 2 && !bInHeld; ++Hb)
		{
			UProceduralMeshComponent* B = HeldBottle[Hb];
			if (!B || !bHeldBreaks[Hb]) continue;
			const FBox Box = B->Bounds.GetBox();
			FVector HitAt, HitN;
			float Time = 0.f;
			if (!Box.IsInside(From) && FMath::LineExtentBoxIntersection(Box, From, PImpact, FVector::ZeroVector, HitAt, HitN, Time))
			{
				BreakHeldBottle(Hb, Aim.Vector());
				UE_LOG(LogOutlaws, Log, TEXT("OLRT shot the bottle in the %s hand"), Hb == 0 ? TEXT("left") : TEXT("right"));
				bInHeld = true;
			}
		}
		if (bInHeld) continue;
		// the player's own head (user 2026-09-28: "I should be able to shoot myself in the head"): a pellet whose way
		// passes within 11 cm of the headset, ahead of the muzzle and before whatever else it strikes, stops there and
		// hurts: the game's upper-middle x1.5, past the chest plate.  Never along the sights (the eye is behind the
		// muzzle) and never scoped (that shot leaves from the eye)
		if (!bScopedShot && L)
		{
			const FVector Head = Camera->GetComponentLocation();
			const FVector Dv = Aim.Vector();
			const double Tc = FVector::DotProduct(Head - From, Dv);
			if (Tc > 0.0 && Tc < (bPHit ? double(PHit.Distance) : double(Reach)) && FVector::Dist(From + Dv * Tc, Head) < 11.0)
			{
				L->SetHurtSource(Muzzle);
				L->HurtCause = uint8(EOutlawsHurtCause::OwnShot);       // (REQ-KILLFEED)
				L->DamagePlayer(W.Damage * FMath::FRandRange(0.9f, 1.1f), 1.5f, true);
				UE_LOG(LogOutlaws, Log, TEXT("OLRT the player shot their own head: %s pellet %d"), W.Label, p);
				if (p == 0)
				{
					bHit = false;
					Impact = From + Dv * Tc;
				}
				continue;
			}
		}
		// a body lying on the way still bleeds and is pushed (user 2026-09-29: the dead stop blocking shots, so the
		// traces pass them): the nearest corpse whose lying body (a flat cylinder at its feet, 55 cm round, 30 high)
		// the pellet passes through before what it struck takes it there
		if (L)
		{
			AOutlawsEnemy* Corpse = nullptr;
			double BestT = 1.0;
			FVector At = PImpact;
			const FVector Seg = PImpact - From;
			const double SegLen = Seg.Size();
			for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
			{
				AOutlawsEnemy* E = Wk.Get();
				if (!E || !E->IsDead() || SegLen < 1.0) continue;
				const UCapsuleComponent* Cap = E->GetCapsuleComponent();
				const FVector Foot = E->GetActorLocation() - FVector(0.0, 0.0, Cap->GetScaledCapsuleHalfHeight());
				const FVector C = Foot + FVector(0.0, 0.0, 15.0);
				// the closest approach, with height counted double (the body is flat): along the pellet's way
				const FVector Sq(1.0, 1.0, 2.0);
				const FVector A0 = (From - C) * Sq, D0 = Seg * Sq;
				const double T = FMath::Clamp(-FVector::DotProduct(A0, D0) / FMath::Max(1e-6, D0.SizeSquared()), 0.0, 1.0);
				if ((A0 + D0 * T).Size() < 55.0 && T < BestT)
				{
					BestT = T;
					Corpse = E;
					At = From + Seg * T;
				}
			}
			if (Corpse)
			{
				const float KnockScale = FCString::Stricmp(W.Label, TEXT("Revolver")) == 0 ? 1.f / 3.f : 1.f;
				Corpse->TakeHit(W.Damage * (L ? L->PlayerDamageScale() : 1.f), At, Aim.Vector(), Muzzle, false, KnockScale);
				if (p == 0)
				{
					bHit = false;             // (no wall puff: the tracer ends in the body)
					Impact = At;
				}
				continue;
			}
		}
		// (M1) another player's body on the way, in a match: the hit is reported to the host and the pellet stops in it
		if (L && UOutlawsCoop::IsMatchActive(L))
		{
			FVector PAt;
			if (UOutlawsCoop::ShotAtPlayers(L, From, PImpact, W.Damage * FMath::FRandRange(0.9f, 1.1f) * L->PlayerDamageScale(), PAt))
			{
				if (p == 0)
				{
					bHit = false;
					Impact = PAt;
				}
				continue;
			}
		}
		// glass on the way breaks (the bullet goes on); a prop before the wall takes the shot
		const bool bPProp = L && L->ShootAlong(From, PImpact);
		if (bPHit && !bPProp && L && L->HitDynamite(PHit.GetComponent()))
		{
		}
		else if (bPHit && !bPProp && ShootThrown(PHit.GetActor()))
		{
		}
		else if (bPHit && !bPProp)
		{
			// a bad guy takes the damage (ITM DAMAGE with the engine's 0.9..1.1 spread); anything else may
			// be a shootable INF surface
			if (AOutlawsEnemy* E = Cast<AOutlawsEnemy>(PHit.GetActor()))
			{
				// the revolver pushes a third as hard as its damage would (user 2026-09-27); the badge doubles both
				const float KnockScale = FCString::Stricmp(W.Label, TEXT("Revolver")) == 0 ? 1.f / 3.f : 1.f;
				E->TakeHit(W.Damage * FMath::FRandRange(0.9f, 1.1f) * (L ? L->PlayerDamageScale() : 1.f), PHit.ImpactPoint, Aim.Vector(), Muzzle, false, KnockScale);
			}
			else if (L)
			{
				L->ShootAt(PHit);
				L->AddBulletHole(PHit);       // a hole where it struck (user 2026-09-28)
			}
		}
		if (p == 0)
		{
			Hit = PHit;
			bHit = bPHit;
			bProp = bPProp;
			Impact = PImpact;
		}
		else if (bPHit && p < 8)
		{
			SpawnPuff(PImpact + PHit.ImpactNormal * 3.0, FVector(PHit.ImpactNormal) * 20.f, 1.5f, 8.f, 0.6f, 0.2f);
		}
	}
	if (L)
	{
		L->AlertEnemies(Muzzle, L->GunshotNoiseCm);
	}
	if (L) L->PlaySoundAt(bOne ? (Wi == 4 ? TEXT("double") : TEXT("single")) : W.FireSound, Muzzle);
	GLastGunSound = bOne ? (Wi == 4 ? TEXT("double") : TEXT("single")) : W.FireSound;
	if (L) L->EchoOf(bOne ? (Wi == 4 ? TEXT("double") : TEXT("single")) : W.FireSound, Muzzle);       // (R13)
	if (L) L->MuzzleFlash(Muzzle);          // the room lights up for a split second (user 2026-09-27)
	if (L && UOutlawsCoop::IsActive(L)) UOutlawsCoop::NoteShot(L, Muzzle, bOne ? (Wi == 4 ? TEXT("double") : TEXT("single")) : W.FireSound);   // (co-op: heard there too)
	Pulse(Hand, 0.9f);
	// the whole gun jolts back toward the shooter; one hand on a long gun: four times the kick, to show it
	const float KickMul = bOneHanded ? 4.f : bSteadied ? 0.5f : 1.f;
	RecoilPitch[Hand] = FMath::Min(RecoilPitch[Hand] + W.KickPitch * KickMul, 20.f * KickMul);
	RecoilKick[Hand] = FMath::Min(RecoilKick[Hand] + W.Kick * KickMul, 18.f * KickMul);
	// muzzle flash (a glowing plume along the barrel and a star across it), a faint tracer and a
	// spark where it hit
	EnsureShotFx(Hand);
	const FVector Bore = ShotDir;      // the flash follows the shot (a fanned one leaves toward its target)
	const FRotator Dir = (Impact - Muzzle).Rotation();
	const float Big = FMath::FRandRange(0.8f, 1.25f) * (bFan ? 1.15f : 1.f);
	MuzzleFx[Hand]->SetWorldLocationAndRotation(Muzzle + Bore * 5.0 * Big, Bore.Rotation());
	MuzzleFx[Hand]->SetWorldScale3D(FVector(0.12, 0.05, 0.05) * Big);
	FlashStar[Hand]->SetWorldLocationAndRotation(Muzzle + Bore * 1.5, Bore.Rotation() + FRotator(0.0, 0.0, FMath::FRandRange(0.f, 180.f)));
	FlashStar[Hand]->SetWorldScale3D(FVector(0.015, 0.13 * Big, 0.07 * Big));
	const double Len = FVector::Dist(Muzzle, Impact);
	TracerFx[Hand]->SetWorldLocationAndRotation((Muzzle + Impact) * 0.5, Dir);
	TracerFx[Hand]->SetWorldScale3D(FVector(Len / 100.0, 0.002, 0.002));
	SparkFx[Hand]->SetWorldLocationAndRotation(Impact, Dir);
	SparkFx[Hand]->SetWorldScale3D(FVector(0.06, 0.06, 0.06));
	// (REQ-MUZZLEFLASH3D) the flash cut from the shooters' sprites for this gun's kind; today's glow where there is none
	const int32 FlashKind = AOutlawsLevelActor::FlashKindOfSound(W.FireSound);
	const bool bFlash3D = L && L->MuzzleFlash3D(Muzzle, Bore, FlashKind, bFan ? 1.15f : 1.f);
	if (L && L->bFlash3DHold)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLFLASH3D shot: %s, kind %s, %s"), W.Label, AOutlawsLevelActor::FlashKindName(FlashKind), bFlash3D ? TEXT("3D") : TEXT("today's flash"));
	}
	MuzzleFx[Hand]->SetVisibility(!bFlash3D);
	FlashStar[Hand]->SetVisibility(!bFlash3D);
	TracerFx[Hand]->SetVisibility(true);
	SparkFx[Hand]->SetVisibility(bHit || bProp);
	FxLeft[Hand] = 0.05f;
	// black powder smoke: a burst out of the muzzle that drifts, swells and fades, and a wisp at the
	// impact
	for (int32 i = 0; i < 4; ++i)
	{
		const FVector Out = Bore * FMath::FRandRange(40.f, 110.f) + FMath::VRand() * 15.f + FVector(0.0, 0.0, 8.0);
		SpawnPuff(Muzzle + Bore * FMath::FRandRange(1.f, 6.f), Out, FMath::FRandRange(2.f, 3.5f), FMath::FRandRange(14.f, 22.f), FMath::FRandRange(1.1f, 1.8f), 0.08f);   // a quarter of 0.32 (user 2026-09-28)
	}
	if (bHit)
	{
		SpawnPuff(Impact + Hit.ImpactNormal * 3.0, FVector(Hit.ImpactNormal) * 25.f, 2.f, 10.f, 0.7f, 0.25f);
	}
	if (W.bEjectOnShot)
	{
		EjectCase(Hand);
	}
	else
	{
		Spent[Hand] = FMath::Min(Spent[Hand] + Rounds, W.Capacity);
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT %s %s: %s, %d left"), Hand == 0 ? TEXT("left") : TEXT("right"), W.Label, bFan ? TEXT("fan shot") : TEXT("shot"), Mag[Hand]);
}

// ---- headset audio --------------------------------------------------------------------------------

void AOutlawsPlayer::RouteAudioToHeadset()
{
	// 5.1's OpenXR does not name the headset's audio device, so the mixer opened Windows' default
	// output; move this game's output (only) to the headset
	FOnAudioOutputDevicesObtained Listed;
	Listed.BindUFunction(this, TEXT("OnAudioDevicesListed"));
	UAudioMixerBlueprintLibrary::GetAvailableAudioOutputDevices(this, Listed);
}

void AOutlawsPlayer::OnAudioDevicesListed(const TArray<FAudioOutputDeviceInfo>& Devices)
{
	for (const FAudioOutputDeviceInfo& D : Devices)
	{
		if (!HeadsetAudioDevice.IsEmpty() && D.Name.Contains(HeadsetAudioDevice))
		{
			FOnCompletedDeviceSwap Done;
			Done.BindUFunction(this, TEXT("OnAudioDeviceSwapped"));
			UE_LOG(LogOutlaws, Log, TEXT("OLRT VR audio: switching to %s"), *D.Name);
			UAudioMixerBlueprintLibrary::SwapAudioOutputDevice(this, D.DeviceId, Done);
			return;
		}
	}
	TArray<FString> Names;
	for (const FAudioOutputDeviceInfo& D : Devices) Names.Add(D.Name);
	UE_LOG(LogOutlaws, Warning, TEXT("OLRT VR audio: no device matching '%s' among: %s"), *HeadsetAudioDevice, *FString::Join(Names, TEXT(", ")));
}

void AOutlawsPlayer::OnAudioDeviceSwapped(const FSwapAudioOutputResult& Result)
{
	UE_LOG(LogOutlaws, Log, TEXT("OLRT VR audio: swap result %d, now %s"), int32(Result.Result), *Result.CurrentDeviceId);
}

// ---- gunfire effects ---------------------------------------------------------------------------

int32 AOutlawsPlayer::AddPuffSlot()
{
	// one more smoke puff in the pool, free (its age at its life)
	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_FxSmoke.M_OLR_FxSmoke"));
	UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this);
	C->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere")));
	UMaterialInstanceDynamic* M = Base ? UMaterialInstanceDynamic::Create(Base, this) : nullptr;
	if (M)
	{
		M->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.78f, 0.76f, 0.72f));
		C->SetMaterial(0, M);
	}
	C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	C->SetCastShadow(false);
	C->SetupAttachment(VROrigin);
	C->SetUsingAbsoluteLocation(true);
	C->SetUsingAbsoluteRotation(true);
	C->SetUsingAbsoluteScale(true);
	C->RegisterComponent();
	PuffFx.Add(C);
	PuffMat.Add(M);
	return Puffs.Add(FFxBit());
}

void AOutlawsPlayer::SpawnPuff(const FVector& At, const FVector& Vel, float Size0, float Size1, float Life, float Alpha)
{
	constexpr int32 MaxPuffs = 32;
	int32 Slot = INDEX_NONE;
	for (int32 i = 0; i < Puffs.Num(); ++i)
	{
		if (Puffs[i].Age >= Puffs[i].Life) { Slot = i; break; }
	}
	if (Slot == INDEX_NONE && Puffs.Num() < MaxPuffs)
	{
		Slot = AddPuffSlot();
	}
	if (Slot == INDEX_NONE)
	{
		// all busy: take the oldest
		Slot = 0;
		for (int32 i = 1; i < Puffs.Num(); ++i)
		{
			if (Puffs[i].Age / Puffs[i].Life > Puffs[Slot].Age / Puffs[Slot].Life) Slot = i;
		}
	}
	FFxBit& P = Puffs[Slot];
	P.Pos = At;
	P.Vel = Vel;
	P.Age = 0.f;
	P.Life = Life;
	P.Size0 = Size0;
	P.Size1 = Size1;
	P.Alpha = Alpha;
	PuffFx[Slot]->SetWorldLocation(At);
	PuffFx[Slot]->SetWorldScale3D(FVector(Size0 / 100.0));
	PuffFx[Slot]->SetVisibility(true);
}

void AOutlawsPlayer::EjectCase(int32 Hand)
{
	const int32 Wi = Equipped[Hand];
	if (Wi < 0 || !WeaponMesh[Hand])
	{
		return;
	}
	const FWeaponDef& W = GWeapons[Wi];
	const int32 Slot = TakeCaseSlot();
	// out of the port (the model's right side; the left hand's mirrored gun throws left), tumbling,
	// then it falls: through the floor for now, gone after a second and a half
	const FTransform Xf = WeaponMesh[Hand]->GetComponentTransform();
	const FVector Side = Xf.TransformVector(W.EjectDir).GetSafeNormal();
	const bool bFlung = W.bEjectOnShot && !HasCylinder(Equipped[Hand]);        // the revolver's drop out of the cylinder
	const float Speed = bFlung ? FMath::FRandRange(180.f, 260.f) : FMath::FRandRange(50.f, 90.f);
	FFxBit& P = Cases[Slot];
	P.Pos = Xf.TransformPosition(W.EjectPort);
	P.Vel = (Side + FMath::VRand() * 0.25f).GetSafeNormal() * Speed + HandVelocity[Hand] * 0.5f + GetVelocity();
	P.Rot = WeaponMesh[Hand]->GetComponentRotation() + FRotator(90.0, 0.0, 0.0);
	P.Spin = FRotator(FMath::FRandRange(-900.f, 900.f), FMath::FRandRange(-900.f, 900.f), FMath::FRandRange(-900.f, 900.f));
	P.Age = 0.f;
	P.Life = 1.5f;
	P.bBounce = P.bRest = false;
	if (CaseFx[Slot]->GetMaterial(0) != CaseMat) CaseFx[Slot]->SetMaterial(0, CaseMat);     // (the slot may have been a red shell)
	CaseFx[Slot]->SetWorldLocationAndRotation(P.Pos, P.Rot);
	CaseFx[Slot]->SetWorldScale3D(bFlung ? FVector(0.012, 0.012, 0.045) : FVector(0.012, 0.012, 0.033));
	CaseFx[Slot]->SetVisibility(true);
}

int32 AOutlawsPlayer::TakeCaseSlot()
{
	constexpr int32 MaxCases = 16;
	int32 Slot = INDEX_NONE;
	for (int32 i = 0; i < Cases.Num(); ++i)
	{
		if (Cases[i].Age >= Cases[i].Life) { Slot = i; break; }
	}
	if (Slot == INDEX_NONE && Cases.Num() < MaxCases)
	{
		UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(this);
		C->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cylinder.Cylinder")));
		if (!CaseMat) CaseMat = SolidMaterial(FColor(196, 150, 62));      // brass
		C->SetMaterial(0, CaseMat);
		C->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		C->SetCastShadow(false);
		C->SetupAttachment(VROrigin);
		C->SetUsingAbsoluteLocation(true);
		C->SetUsingAbsoluteRotation(true);
		C->SetUsingAbsoluteScale(true);
		C->RegisterComponent();
		CaseFx.Add(C);
		Slot = Cases.Add(FFxBit());
	}
	if (Slot == INDEX_NONE)
	{
		Slot = 0;
		for (int32 i = 1; i < Cases.Num(); ++i)
		{
			if (Cases[i].Age > Cases[Slot].Age) Slot = i;
		}
	}
	return Slot;
}

// ---- REQ-FLICKRELOAD: the double guns break open ------------------------------------------------------------------------

namespace
{
	// the flick: a run of frames turning the gun about its hinge's axis at FlickHoldRate or more, one way, that reaches
	// FlickPeakRate and FlickAngle in all, with little turn about the other two axes (FlickSideShare of it), the hand moving across
	// the barrel under FlickAcrossSpeed (a melee swing or a chop goes faster) and under FlickHandSpeed in all (a throw); one flick,
	// then FlickCooldown. (BUG-FLICKFEEL, user 2026-10-14, Quest build 47: 25 of 104 flicks taken. It was 300 / 600 deg/s, 25
	// degrees, 0.6 off its axis, any hand motion past 200 cm/s (taken ones ran up to 198: a natural flick carries the hand) and
	// 0.4 s between flicks)
	constexpr float FlickHoldRate = 250.f;       // deg/s
	constexpr float FlickPeakRate = 500.f;       // deg/s (a fast aim down peaks at about 450)
	constexpr float FlickAngle = 20.f;           // deg
	constexpr float FlickShutPeakRate = 340.f;   // deg/s, the flick back on an open gun (BUG-FLICKUP)
	constexpr float FlickShutAngle = 16.f;       // deg, the same
	constexpr float FlickSideShare = 0.8f;
	constexpr float FlickAcrossSpeed = 350.f;    // cm/s
	constexpr float FlickHandSpeed = 600.f;      // cm/s
	constexpr float FlickCooldown = 0.15f;       // s
	constexpr float FlickShutAfter = 0.2f;       // s open before a flick back shuts it (the opening flick's own rebound never does)
	constexpr float FlickLogAngle = 15.f;        // deg: a run this long that is not taken is logged with why
	constexpr float BreakAngle = 40.f;           // deg the barrels drop
	constexpr float BreakSnap = 0.08f;           // s to swing open or shut
}

void AOutlawsPlayer::WarmFirstUse(AOutlawsLevelActor* L)
{
	if (!L) return;
	double T0 = FPlatformTime::Seconds();
	int32 Models = 0, Sections = 0;
	for (int32 Wi = 0; Wi < NumWeapons; ++Wi)
	{
		bool bCard = false;
		FString Path;
		const TSharedPtr<OutlawsObj::FObjMesh> Model = HeldModelFor(Wi, bCard, Path);
		if (!Model.IsValid()) continue;
		++Models;
		for (const TPair<FString, FOutlawsMeshSection>& Kv : Model->Sections)
		{
			HeldSectionMaterial(Kv.Key, Kv.Value.Texture);
			++Sections;
		}
	}
	// the sprite guns' hammers, triggers and levers (BuildGunParts: a first rifle drew its lever 3.7 ms)
	int32 Parts = 0;
	for (int32 Wi = 0; Wi < NumWeapons; ++Wi)
	{
		if (GWeapons[Wi].Model3do || !UseSpriteGun(Wi)) continue;
		const FSpriteSights& Si = GSpriteSights[FMath::Clamp(Wi, 0, 7)];
		if (Si.bHammer && GunPartModel(Wi, TEXT("_hammer.obj"), Si.HammerPivot)->IsValid()) ++Parts;
		if (Si.bTrigger && GunPartModel(Wi, TEXT("_trigger.obj"), Si.TriggerPivot)->IsValid()) ++Parts;
		if (Si.bLever && GunPartModel(Wi, TEXT("_lever.obj"), Si.LeverPivot)->IsValid()) ++Parts;
	}
	// each hand's flash, tracer and spark, and a first shot's smoke (a shot puffs five; a hand's first shot made them)
	EnsureShotFx(0);
	EnsureShotFx(1);
	int32 NewPuffs = 0;
	while (Puffs.Num() < 8)
	{
		const int32 Slot = AddPuffSlot();
		if (PuffFx.IsValidIndex(Slot) && PuffFx[Slot]) PuffFx[Slot]->SetVisibility(false);
		++NewPuffs;
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT prebuild held guns: %d models, %d sections, %d parts; shot effects for both hands, %d smoke puffs: %.1f ms"),
		Models, Sections, Parts, NewPuffs, (FPlatformTime::Seconds() - T0) * 1000.0);
	T0 = FPlatformTime::Seconds();
	TSet<FString> Wavs = { TEXT("single"), TEXT("double"), TEXT("fuse"), L->GetBlastSound() };
	for (int32 Wi = 0; Wi < NumWeapons; ++Wi)
	{
		if (GWeapons[Wi].FireSound && *GWeapons[Wi].FireSound) Wavs.Add(GWeapons[Wi].FireSound);
		if (GWeapons[Wi].EmptySound && *GWeapons[Wi].EmptySound) Wavs.Add(GWeapons[Wi].EmptySound);
	}
	for (const FString& W : Wavs) L->WarmWav(W);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT prebuild gun sounds: %d, %.1f ms"), Wavs.Num(), (FPlatformTime::Seconds() - T0) * 1000.0);
}

void AOutlawsPlayer::BuildBreakBarrels(int32 Hand, int32 Wi, bool bCard)
{
	UProceduralMeshComponent* Gun = WeaponMesh[Hand];
	if (!Gun || !IsBreakGun(Wi) || bCard)
	{
		if (BarrelMesh[Hand]) { BarrelMesh[Hand]->ClearAllMeshSections(); OutlawsInk::Forget(BarrelMesh[Hand]); BarrelMesh[Hand]->SetVisibility(false); }
		return;
	}
	const FWeaponDef& W = GWeapons[Wi];
	// the hinge: just ahead of the breech (the gun's load point), under the barrels (2.5 cm below the bore)
	const FVector Hinge(W.LoadPoint.X + 0.8, 0.0, W.Muzzle.Z - 2.5);
	BreakHinge[Hand] = Hinge;
	if (!BreakPivot[Hand])
	{
		BreakPivot[Hand] = NewObject<USceneComponent>(this);
		BreakPivot[Hand]->SetupAttachment(Gun);
		BreakPivot[Hand]->RegisterComponent();
		BarrelMesh[Hand] = NewObject<UProceduralMeshComponent>(this);
		BarrelMesh[Hand]->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		BarrelMesh[Hand]->SetCastShadow(false);
		BarrelMesh[Hand]->SetupAttachment(BreakPivot[Hand]);
		BarrelMesh[Hand]->RegisterComponent();
	}
	BreakPivot[Hand]->SetRelativeLocationAndRotation(Hinge, FQuat::Identity);
	UProceduralMeshComponent* B = BarrelMesh[Hand];
	B->ClearAllMeshSections(); OutlawsInk::Forget(B);
	// each of the gun's sections in two by its triangles' middles: behind the hinge stays on the gun, ahead goes on the pivot
	int32 Front = 0, Rear = 0;
	for (int32 s = 0; s < Gun->GetNumSections(); ++s)
	{
		const FProcMeshSection* Src = Gun->GetProcMeshSection(s);
		if (!Src) continue;
		FProcMeshSection Keep, Move;
		TArray<int32> KeepMap, MoveMap;
		KeepMap.Init(-1, Src->ProcVertexBuffer.Num());
		MoveMap.Init(-1, Src->ProcVertexBuffer.Num());
		for (int32 t = 0; t + 2 < Src->ProcIndexBuffer.Num(); t += 3)
		{
			const uint32 Ia = Src->ProcIndexBuffer[t], Ib = Src->ProcIndexBuffer[t + 1], Ic = Src->ProcIndexBuffer[t + 2];
			const double MidX = (Src->ProcVertexBuffer[Ia].Position.X + Src->ProcVertexBuffer[Ib].Position.X + Src->ProcVertexBuffer[Ic].Position.X) / 3.0;
			const bool bAhead = MidX > Hinge.X;
			FProcMeshSection& Dst = bAhead ? Move : Keep;
			TArray<int32>& Map = bAhead ? MoveMap : KeepMap;
			for (const uint32 U : { Ia, Ib, Ic })
			{
				const int32 I = int32(U);
				if (Map[I] < 0)
				{
					FProcMeshVertex V = Src->ProcVertexBuffer[I];
					if (bAhead) V.Position -= Hinge;
					Map[I] = Dst.ProcVertexBuffer.Add(V);
					Dst.SectionLocalBox += V.Position;
				}
				Dst.ProcIndexBuffer.Add(uint32(Map[I]));
			}
			(bAhead ? Front : Rear) += 1;
		}
		Keep.bEnableCollision = Move.bEnableCollision = false;
		Keep.bSectionVisible = Src->bSectionVisible;
		Move.bSectionVisible = Src->bSectionVisible;
		UMaterialInterface* Mat = Gun->GetMaterial(s);
		Gun->SetProcMeshSection(s, Keep);
		B->SetProcMeshSection(s, Move);
		if (Mat) B->SetMaterial(s, Mat);
	}
	B->SetRelativeLocationAndRotation(FVector::ZeroVector, FQuat::Identity);
	B->SetVisibility(true);
	KeepDrawnOverWorld(B);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT %s hand %s: the barrels on their hinge at %s (%d triangles ahead of it, %d behind)"), Hand == 0 ? TEXT("left") : TEXT("right"), W.Label,
		*Hinge.ToCompactString(), Front, Rear);
}

int32 AOutlawsPlayer::FlickStep(int32 Hand, float Dt)
{
	USceneComponent* F = FrameOf(Hand);
	FlickCool[Hand] = FMath::Max(0.f, FlickCool[Hand] - Dt);
	if (!F || Dt <= 0.f) return 0;
	// (BUG-FLICKFEEL) read in the gun's frame as the hand holds it (the hand's frame turned by the gun's mount, GunBaseRot: its
	// barrel along X, the hinge across Y), not the hand's own: the mount's tilt of the gun in the hand (mirrored for the left,
	// REQ-GRIPMIRROR) put part of a real flick on the other axes. The mount, not the gun's drawn pose: the recoil's kick and the
	// two-handed hold turn that, and must never read as a flick
	const FQuat Mount = WeaponMesh[Hand] ? GunBaseRot[Hand].Quaternion() : FQuat::Identity;
	const FQuat Held = F->GetComponentQuat() * Mount;
	// its turn in the play space (the body's own turning and the snap turn left out), and this frame's turn in its own frame:
	// the muzzle's rise (+) or drop (-), its swing to the side and its roll about the barrel
	const FQuat Q = (VROrigin ? VROrigin->GetComponentQuat().Inverse() : FQuat::Identity) * Held;
	// (a gun just drawn, a menu or the wheel between: start the reading again, never one long jump read as a fast turn)
	if (GFrameCounter > FlickFrame[Hand] + 1) bFlickPrev[Hand] = false;
	FlickFrame[Hand] = GFrameCounter;
	if (!bFlickPrev[Hand])
	{
		FlickPrevQ[Hand] = Q;
		bFlickPrev[Hand] = true;
		return 0;
	}
	// (BUG-FLICKUP, user 2026-10-15: the flick up to shut failed by the wrist's angle) the barrel's own rise or drop, whatever the
	// wrist's roll about it: the change in the barrel's height angle in the play space, the swing across it the rest of the barrel's
	// turn, the roll about the barrel not counted. It was the turn about the gun's own cross axis and half the roll against it, so a
	// muzzle raised with the wrist canted 60 degrees read as half a flick and a swing to the side (open and shut alike; the flick up,
	// wrist and forearm together, is canted more often). With the gun upright it reads as before
	const FVector B0 = FlickPrevQ[Hand].GetForwardVector(), B1 = Q.GetForwardVector();
	FlickPrevQ[Hand] = Q;
	const float Pitch = float(FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(B1.Z, -1.0, 1.0)) - FMath::Asin(FMath::Clamp(B0.Z, -1.0, 1.0))));
	const float Turn = float(FMath::RadiansToDegrees(FMath::Atan2(FVector::CrossProduct(B0, B1).Size(), FVector::DotProduct(B0, B1))));
	const float Swing = FMath::Sqrt(FMath::Max(0.f, Turn * Turn - Pitch * Pitch));
	// (a turn of more than 60 degrees in one frame is a tracking jump or a recentre, not a wrist)
	const float Rate = Turn > 60.f ? 0.f : Pitch / Dt;
	const bool bSameWay = FlickPitch[Hand] == 0.f || (Rate > 0.f) == (FlickPitch[Hand] > 0.f);
	if (FMath::Abs(Rate) < FlickHoldRate || !bSameWay)
	{
		// the run ends: one that turned far enough to be meant and was not taken is said with why (the thresholds' tuning)
		if (!bFlickTaken[Hand] && FMath::Abs(FlickPitch[Hand]) >= FlickLogAngle && FlickLogCool <= 0.f)
		{
			const TCHAR* Why = FlickWhy[Hand] ? FlickWhy[Hand]
				: FlickSawCool[Hand] ? TEXT("the cooldown after the last flick")
				: FMath::Abs(FlickPitch[Hand]) < (FlickPitch[Hand] > 0.f && bBreakOpen[Hand] ? FlickShutAngle : FlickAngle) ? TEXT("too small a turn")
				: TEXT("too slow a turn");
			UE_LOG(LogOutlaws, Log, TEXT("OLRT flick not taken (%s hand): %s; %.0f deg at up to %.0f deg/s, %.0f deg off its axis, the hand at up to %.0f cm/s (%.0f across the barrel)"),
				Hand == 0 ? TEXT("left") : TEXT("right"), Why, FlickPitch[Hand], FlickPeak[Hand], FlickSide[Hand], FlickHandMax[Hand], FlickAcrossMax[Hand]);
			FlickLogCool = 0.25f;
		}
		FlickPitch[Hand] = FlickSide[Hand] = FlickPeak[Hand] = FlickHandMax[Hand] = FlickAcrossMax[Hand] = 0.f;
		bFlickVoid[Hand] = bFlickTaken[Hand] = FlickSawCool[Hand] = false;
		FlickWhy[Hand] = nullptr;
		if (FMath::Abs(Rate) < FlickHoldRate) return 0;
	}
	FlickPitch[Hand] += Pitch;
	// (the forearm's own roll rides along with a wrist flick and no longer counts; all of a swing to the side does)
	FlickSide[Hand] += Swing;
	FlickPeak[Hand] = FMath::Max(FlickPeak[Hand], FMath::Abs(Rate));
	// the hand's own motion: along the barrel a flick may carry it (a natural flick pushes the gun forward, or pulls it back);
	// across the barrel fast (a melee swing, a chop) or anything as fast as a throw spoils the whole run, not only its fast frames
	const FVector Hv = HandVelocity[Hand];
	const float HandSpeed = float(Hv.Size());
	const float Along = FMath::Abs(float(FVector::DotProduct(Hv, Held.GetForwardVector())));
	const float Across = FMath::Sqrt(FMath::Max(0.f, HandSpeed * HandSpeed - Along * Along));
	FlickHandMax[Hand] = FMath::Max(FlickHandMax[Hand], HandSpeed);
	FlickAcrossMax[Hand] = FMath::Max(FlickAcrossMax[Hand], Across);
	if (!bFlickVoid[Hand] && (HandSpeed > FlickHandSpeed || Across > FlickAcrossSpeed))
	{
		bFlickVoid[Hand] = true;
		FlickWhy[Hand] = HandSpeed > FlickHandSpeed ? TEXT("the hand moving as fast as a throw") : TEXT("the hand swinging across the barrel (a melee swing)");
	}
	if (bFlickVoid[Hand]) return 0;
	if (FlickCool[Hand] > 0.f) { FlickSawCool[Hand] = true; return 0; }
	const float Turned = FMath::Abs(FlickPitch[Hand]);
	// (BUG-FLICKUP, the user's Quest log of 15 Oct: shut flicks turned away as too slow ran 21 to 49 degrees at 345 to 496 deg/s,
	// as too small 16 to 19 degrees) the flick back on an open gun has a lower floor: an open gun neither aims nor fires, so a
	// quick raise of it is a shut; the forward flick keeps its floor (a fast aim down peaks at about 450)
	const bool bShutWay = FlickPitch[Hand] > 0.f && bBreakOpen[Hand];
	if (Turned < (bShutWay ? FlickShutAngle : FlickAngle) || FlickPeak[Hand] < (bShutWay ? FlickShutPeakRate : FlickPeakRate)) return 0;
	if (FlickSide[Hand] > FlickSideShare * Turned)
	{
		bFlickVoid[Hand] = true;                 // (an aim sweep: mostly a turn to the side)
		FlickWhy[Hand] = TEXT("too far off its axis (an aim sweep)");
		return 0;
	}
	const int32 Dir = FlickPitch[Hand] < 0.f ? 1 : -1;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT flick %s (%s hand): %.0f deg at up to %.0f deg/s, %.0f deg off its axis, the hand at %.0f cm/s (%.0f across the barrel)"), Dir > 0 ? TEXT("forward") : TEXT("back"),
		Hand == 0 ? TEXT("left") : TEXT("right"), FlickPitch[Hand], FlickPeak[Hand], FlickSide[Hand], HandSpeed, Across);
	FlickCool[Hand] = FlickCooldown;
	bFlickVoid[Hand] = bFlickTaken[Hand] = true;   // (the rest of this run is the same flick)
	return Dir;
}

void AOutlawsPlayer::EjectShell(int32 Hand)
{
	const int32 Wi = Equipped[Hand];
	if (Wi < 0 || !WeaponMesh[Hand]) return;
	const FWeaponDef& W = GWeapons[Wi];
	const int32 Slot = TakeCaseSlot();
	if (!ShellMat) ShellMat = SolidMaterial(FColor(150, 32, 26));        // a red paper hull
	// out of the open breech, back over the hand and up (the extractor's kick), a little to either side
	const FTransform Xf = WeaponMesh[Hand]->GetComponentTransform();
	const FVector Out = Xf.TransformVector(FVector(-1.0, FMath::FRandRange(-0.3, 0.3), 0.9)).GetSafeNormal();
	FFxBit& P = Cases[Slot];
	P.Pos = Xf.TransformPosition(W.LoadPoint + FVector(1.0, 0.0, 0.0));
	P.Vel = Out * FMath::FRandRange(150.f, 220.f) + HandVelocity[Hand] * 0.5f + GetVelocity();
	P.Rot = WeaponMesh[Hand]->GetComponentRotation() + FRotator(90.0, 0.0, 0.0);
	P.Spin = FRotator(FMath::FRandRange(-700.f, 700.f), FMath::FRandRange(-200.f, 200.f), FMath::FRandRange(-700.f, 700.f));
	P.Age = 0.f;
	P.Life = 2.0f;
	P.bBounce = true;
	P.bRest = false;
	if (CaseFx[Slot]->GetMaterial(0) != ShellMat) CaseFx[Slot]->SetMaterial(0, ShellMat);
	CaseFx[Slot]->SetWorldLocationAndRotation(P.Pos, P.Rot);
	CaseFx[Slot]->SetWorldScale3D(FVector(0.021, 0.021, 0.065));      // a 12 bore shell, 2.1 x 6.5 cm
	CaseFx[Slot]->SetVisibility(!bMovieHidden);
}

bool AOutlawsPlayer::OpenBreak(int32 Hand, const TCHAR* Why)
{
	const int32 Wi = Equipped[Hand];
	if (!IsBreakGun(Wi) || bBreakOpen[Hand] || !WeaponMesh[Hand]) return false;
	const FWeaponDef& W = GWeapons[Wi];
	AOutlawsLevelActor* L = Level.Get();
	bBreakOpen[Hand] = true;
	BreakCycleTimer[Hand] = 0.f;
	// the fired shells come out; a live one stays in its barrel
	const int32 N = FMath::Clamp(Spent[Hand], 0, W.Capacity);
	Spent[Hand] = 0;
	for (int32 i = 0; i < N; ++i) EjectShell(Hand);
	ShellsOut += N;
	++FlickOpens;
	const FTransform Xf = WeaponMesh[Hand]->GetComponentTransform();
	const FVector Breech = Xf.TransformPosition(W.LoadPoint);
	// (REQ-RELOADSLICES) the reload wave's open slice, then a shell slice for each shell that will go in (the reserve allowing);
	// no empty-chamber click (BUG-RELOADCLINK: that is the dry fire's alone)
	const int32 Want = FMath::Max(0, W.Capacity - Mag[Hand]);
	const int32 Loads = bUnlimitedAmmo ? Want : FMath::Min(Want, L ? L->GetCount(W.AmmoItem) : 0);
	BreakLoads[Hand] = Loads;
	BreakCycleIn[Hand] = 0;
	if (L) L->PlaySoundRun(Hand, AOutlawsLevelActor::ReloadOpenRun(W.ReloadSound, Loads), Breech);
	Pulse(Hand, 0.35f);
	if (L && UOutlawsCoop::IsActive(L))
	{
		const FVector Vel = Xf.TransformVector(FVector(-1.0, 0.0, 0.9)).GetSafeNormal() * 185.0 + GetVelocity();
		UOutlawsCoop::NoteBreak(L, Hand, true, N, Breech, Vel, Loads);      // (co-op: the barrels open there too, the shells fly, the same slices)
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT %s %s broken open (%s): %d shell%s out, %d loaded, %d carried"), Hand == 0 ? TEXT("left") : TEXT("right"), W.Label, Why, N,
		N == 1 ? TEXT("") : TEXT("s"), Mag[Hand], L ? L->GetCount(W.AmmoItem) : 0);
	return true;
}

bool AOutlawsPlayer::CloseBreak(int32 Hand, const TCHAR* Why)
{
	const int32 Wi = Equipped[Hand];
	if (!IsBreakGun(Wi) || !bBreakOpen[Hand]) return false;
	const FWeaponDef& W = GWeapons[Wi];
	AOutlawsLevelActor* L = Level.Get();
	// loaded from the reserve as the reload button loads: as many as it has, up to the gun's two (one left: one goes in)
	const int32 Want = FMath::Max(0, W.Capacity - Mag[Hand]);
	int32 Load = bUnlimitedAmmo ? Want : FMath::Min(Want, L ? L->GetCount(W.AmmoItem) : 0);
	if (Load > 0 && !bUnlimitedAmmo && !(L && L->TakeCount(W.AmmoItem, Load))) Load = 0;
	Mag[Hand] += Load;
	bBreakOpen[Hand] = false;
	bBreakCycle[Hand] = false;
	BreakCycleTimer[Hand] = 0.f;
	++FlickCloses;
	const FVector Breech = WeaponMesh[Hand] ? WeaponMesh[Hand]->GetComponentTransform().TransformPosition(W.LoadPoint) : GetActorLocation();
	if (L) L->PlaySoundRun(Hand, { AOutlawsLevelActor::ReloadShutSlice(W.ReloadSound) }, Breech);      // (an early shut cuts the shells off)
	BreakShutClock[Hand] = L ? L->SoundRunClock : 0.0;
	Pulse(Hand, 0.45f);
	if (L && UOutlawsCoop::IsActive(L)) UOutlawsCoop::NoteBreak(L, Hand, false, 0, Breech, FVector::ZeroVector);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT %s %s snapped shut (%s): %d put in, %d loaded, %d carried"), Hand == 0 ? TEXT("left") : TEXT("right"), W.Label, Why, Load, Mag[Hand],
		L ? L->GetCount(W.AmmoItem) : 0);
	RegripAfterBreak(Hand);
	return true;
}

void AOutlawsPlayer::RegripAfterBreak(int32 Hand)
{
	const int32 O = 1 - Hand;
	if (!bRegrip[O]) return;
	bRegrip[O] = false;
	const float G = Axis(O == 0 ? TEXT("OLVR_LeftGrip") : TEXT("OLVR_RightGrip"));
	const TCHAR* Which = O == 0 ? TEXT("left") : TEXT("right");
	if (bSupport[O]) return;                     // (it took the forend again by itself during the cycle)
	if (G > 0.6f && NearForegrip(O))
	{
		bSupport[O] = true;
		bGripArmed[O] = false;
		SupportAnchor[Hand] = ForendSpot(Equipped[Hand]);
		Pulse(O, 0.3f);
		UE_LOG(LogOutlaws, Log, TEXT("OLRT %s hand takes the forend again after the reload"), Which);
	}
	else
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT %s hand stays free after the reload (%s)"), Which, G > 0.6f ? TEXT("not near the forend") : TEXT("its grip let go"));
	}
}

void AOutlawsPlayer::SetFrontHidden(int32 Hand, bool bHide)
{
	if (Hand < 0 || Hand > 1 || bFrontHidden[Hand] == bHide) return;
	bFrontHidden[Hand] = bHide;
	// hidden in game, apart from its visibility (VR or desktop, a movie's put-away): the two never undo each other
	if (UOutlawsBoxHand* Box = Hand == 0 ? LeftHand.Get() : RightHand.Get()) Box->SetHiddenInGame(bHide, true);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT %s hand %s"), Hand == 0 ? TEXT("left") : TEXT("right"),
		bHide ? TEXT("hidden for the other gun's button reload") : bSupport[Hand] ? TEXT("drawn again, on the forend") : TEXT("drawn again, free"));
}

#if WITH_OUTLAWS_TESTS
bool AOutlawsPlayer::TestHiddenHandReload(int32 Wi, FString& Out)
{
	constexpr float Dt = 1.f / 90.f;
	TestAxes.Add(TEXT("OLVR_LeftGrip"), 1.f);
	TestBreakArm(1, Wi, 0, 10);
	TestSetTwoHand(1, true);
	TestFrontHandAt(1, 0.f);
	TestHandStill(1, 0.1f);
	const bool bHeld0 = bSupport[0], bHid0 = bFrontHidden[0];
	bButtonReload[1] = true;
	int32 OpenFrames = 0, HiddenOpen = 0, Frames = 0;
	bool bWasOpen = false, bHidAtShut = true;
	for (; Frames < 270; ++Frames)
	{
		TestHandStill(1, Dt);
		if (bBreakOpen[1])
		{
			bWasOpen = true;
			++OpenFrames;
			HiddenOpen += bFrontHidden[0] ? 1 : 0;
		}
		else if (bWasOpen)
		{
			bHidAtShut = bFrontHidden[0];
			break;
		}
	}
	TestHandStill(1, 0.1f);
	const bool bHeld1 = bSupport[0], bHid1 = bFrontHidden[0];
	TestAxes.Remove(TEXT("OLVR_LeftGrip"));
	TestSetTwoHand(1, false);
	Out = FString::Printf(TEXT("before: left hand %s, %s; open %d frames (%.2f s), hidden %d of them; at the shut %s; after: %s, %s"),
		bHeld0 ? TEXT("on the forend") : TEXT("free"), bHid0 ? TEXT("hidden") : TEXT("drawn"), OpenFrames, OpenFrames * Dt, HiddenOpen,
		bHidAtShut ? TEXT("hidden") : TEXT("drawn"), bHeld1 ? TEXT("on the forend") : TEXT("free"), bHid1 ? TEXT("hidden") : TEXT("drawn"));
	return bWasOpen && bHeld0 && !bHid0 && OpenFrames > 0 && HiddenOpen == OpenFrames && !bHidAtShut && bHeld1 && !bHid1;
}
#endif // WITH_OUTLAWS_TESTS

void AOutlawsPlayer::TickBreak(int32 Hand, float Dt, bool bBlocked)
{
	FlickLogCool = FMath::Max(0.f, FlickLogCool - Dt * 0.5f);      // (two hands a frame)
	const int32 Flick = FlickStep(Hand, Dt);          // (read every frame: the turn is since the last one)
	const int32 Wi = Equipped[Hand];
	UProceduralMeshComponent* Gun = WeaponMesh[Hand];
	if (!IsBreakGun(Wi) || !Gun)
	{
		bBreakOpen[Hand] = bBreakCycle[Hand] = false;
		bRegrip[1 - Hand] = false;
		SetFrontHidden(1 - Hand, false);
		BreakShown[Hand] = 0.f;
		if (BarrelMesh[Hand] && BarrelMesh[Hand]->IsVisible()) BarrelMesh[Hand]->SetVisibility(false);
		return;
	}
	const FWeaponDef& W = GWeapons[Wi];
	AOutlawsLevelActor* L = Level.Get();
	const int32 Reserve = L ? L->GetCount(W.AmmoItem) : 0;
	if (!bBlocked)
	{
		// (REQ-FLICKRULES) no flick while the other hand holds the forend (it would throw the front hand around; the reload
		// button still works), and no flick open on a fully loaded gun (nothing to take out or put in)
		const bool bTwoHands = bSupport[1 - Hand];
		const bool bFull = Mag[Hand] >= W.Capacity;
		if (Flick != 0 && bTwoHands)
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLRT flick not taken (%s hand): two hands on the gun (a flick %s)"), Hand == 0 ? TEXT("left") : TEXT("right"),
				Flick > 0 ? TEXT("forward") : TEXT("back"));
			++FlickRefusedTwoHands;
		}
		else if (Flick > 0 && !bBreakOpen[Hand] && bFull)
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLRT flick not taken (%s hand): the gun is fully loaded (%d of %d)"), Hand == 0 ? TEXT("left") : TEXT("right"), Mag[Hand], W.Capacity);
			++FlickRefusedFull;
		}
		else if (Flick > 0 && !bBreakOpen[Hand]) OpenBreak(Hand, TEXT("a forward flick"));
		else if (Flick < 0 && bBreakOpen[Hand] && BreakOpenFor[Hand] >= FlickShutAfter) CloseBreak(Hand, TEXT("a flick back"));
		else if (Flick != 0)
		{
			// (BUG-FLICKFEEL) a flick the gun's state does not take, said with why
			UE_LOG(LogOutlaws, Log, TEXT("OLRT flick not taken (%s hand): %s"), Hand == 0 ? TEXT("left") : TEXT("right"),
				Flick > 0 ? TEXT("a flick forward with the gun already open")
				: !bBreakOpen[Hand] ? TEXT("a flick back with the gun already shut")
				: *FString::Printf(TEXT("a flick back %.2f s after it opened (the opening flick's rebound; %.2f s wanted)"), BreakOpenFor[Hand], FlickShutAfter));
		}
		// the reload button: the same break, a shell at a time from the reserve (the reload's own pace), then shut; pressed with
		// it already open: shut, loaded
		if (bButtonReload[Hand])
		{
			bButtonReload[Hand] = false;
			if (bBreakOpen[Hand]) CloseBreak(Hand, TEXT("the reload button"));
			else if (Mag[Hand] < W.Capacity && (Reserve > 0 || bUnlimitedAmmo) && OpenBreak(Hand, TEXT("the reload button")))
			{
				bBreakCycle[Hand] = true;
				// (REQ-FLICKRULES) held two-handed: the front hand lets go for the cycle, clear of the barrels' swing, and takes
				// the gun back when it shuts (RegripAfterBreak)
				if (bTwoHands)
				{
					bSupport[1 - Hand] = false;
					bRegrip[1 - Hand] = true;
					UE_LOG(LogOutlaws, Log, TEXT("OLRT %s hand lets go of the forend for the reload"), Hand == 0 ? TEXT("right") : TEXT("left"));
				}
			}
		}
		if (bBreakCycle[Hand] && bBreakOpen[Hand])
		{
			// (BUG-RELOADPACE) kept to the opening's sound run: a shell goes in as its slice starts, and the barrels shut as the last
			// slice ends, on the frame the shut slice starts (it was a shell each ReloadEach, then the last slice heard out, gaps between)
			BreakCycleTimer[Hand] += Dt;
			constexpr float Open = AOutlawsLevelActor::ReloadOpenLen, Each = AOutlawsLevelActor::ReloadShellLen;
			while (BreakCycleIn[Hand] < BreakLoads[Hand] && BreakCycleTimer[Hand] >= Open + Each * BreakCycleIn[Hand])
			{
				++BreakCycleIn[Hand];
				if (Mag[Hand] < W.Capacity && (bUnlimitedAmmo || (L && L->TakeCount(W.AmmoItem, 1))))
				{
					++Mag[Hand];
					Pulse(Hand, 0.25f);       // (REQ-RELOADSLICES: its sound is the shell slice the opening started)
				}
			}
			// the last slice heard out (no sound level: its time), never more than a moment past it
			const float End = Open + Each * BreakLoads[Hand];
			if (BreakCycleIn[Hand] >= BreakLoads[Hand] && ((L ? !L->SoundRunBusy(Hand) : BreakCycleTimer[Hand] >= End) || BreakCycleTimer[Hand] >= End + 0.25f))
				CloseBreak(Hand, TEXT("the reload button's cycle"));
		}
	}
	if (!bBreakOpen[Hand]) bBreakCycle[Hand] = false;
	if (!bBreakOpen[Hand]) bRegrip[1 - Hand] = false;     // (shut some other way: a put-away, the wheel)
	// (REQ-RELOADHIDEHAND, user 2026-10-15) the front hand that let go for the button's cycle is not drawn through it (it would hang
	// in the air where the forend was); drawn again as the gun shuts (on the forend, RegripAfterBreak) or as its grip lets go
	SetFrontHidden(1 - Hand, bBreakOpen[Hand] && bBreakCycle[Hand] && bRegrip[1 - Hand]);
	BreakOpenFor[Hand] =bBreakOpen[Hand] ? BreakOpenFor[Hand] + Dt : 0.f;
	// the barrels' swing: a snap either way, a little quicker shut
	BreakShown[Hand] = FMath::FInterpConstantTo(BreakShown[Hand], bBreakOpen[Hand] ? 1.f : 0.f, Dt, 1.f / (bBreakOpen[Hand] ? BreakSnap : BreakSnap * 0.75f));
	if (BreakPivot[Hand]) BreakPivot[Hand]->SetRelativeRotation(FRotator(-BreakAngle * BreakShown[Hand], 0.0, 0.0));
	if (UProceduralMeshComponent* B = BarrelMesh[Hand])
	{
		const bool bShow = Gun->IsVisible() && B->GetNumSections() > 0;
		if (B->IsVisible() != bShow) B->SetVisibility(bShow);
	}
}

#if WITH_OUTLAWS_TESTS
void AOutlawsPlayer::TestBreakArm(int32 Hand, int32 Wi, int32 Loaded, int32 Reserve)
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L || Hand < 0 || Hand > 1) return;
	L->GiveAllWeapons();
	Equip(1 - Hand, -1);
	Equip(Hand, Wi);
	if (Equipped[Hand] != Wi) return;
	const FWeaponDef& W = GWeapons[Wi];
	Mag[Hand] = FMath::Clamp(Loaded, 0, W.Capacity);
	Spent[Hand] = W.bEjectOnShot ? 0 : W.Capacity - Mag[Hand];
	if (W.AmmoItem && *W.AmmoItem)
	{
		L->TakeCount(W.AmmoItem, L->GetCount(W.AmmoItem));
		if (Reserve > 0) L->AddCount(W.AmmoItem, Reserve);
	}
	bButtonReload[Hand] = false;
	bSupport[0] = bSupport[1] = bRegrip[0] = bRegrip[1] = false;
	FireCooldown[Hand] = 0.f;
	FlickCool[Hand] = 0.f;
	bFlickPrev[Hand] = false;
	FlickPitch[Hand] = FlickSide[Hand] = FlickPeak[Hand] = FlickHandMax[Hand] = FlickAcrossMax[Hand] = 0.f;
	bFlickTaken[Hand] = FlickSawCool[Hand] = false;
	FlickWhy[Hand] = nullptr;
	bFlickVoid[Hand] = false;
	HandVelocity[Hand] = FVector::ZeroVector;
	if (USceneComponent* F = FrameOf(Hand)) F->SetRelativeRotation(FRotator::ZeroRotator);
	TestWristRoll = 0.f;
}

void AOutlawsPlayer::TestRollWrist(int32 Hand, float Deg)
{
	USceneComponent* F = FrameOf(Hand);
	if (!F) return;
	const FQuat Gq = F->GetComponentQuat() * (WeaponMesh[Hand] ? GunBaseRot[Hand].Quaternion() : FQuat::Identity);
	F->SetWorldRotation((Gq * FQuat(FVector::ForwardVector, FMath::DegreesToRadians(Deg)) * Gq.Inverse()) * F->GetComponentQuat());
	TestWristRoll += Deg;
	bFlickPrev[Hand] = false;                    // (the roll is a pose, not a turn read as one)
}

void AOutlawsPlayer::TestHandMotion(int32 Hand, float PitchPeak, float YawPeak, float RollPeak, float Seconds, float HandSpeed)
{
	USceneComponent* F = FrameOf(Hand);
	if (!F || Seconds <= 0.f) return;
	constexpr float Dt = 1.f / 90.f;
	const int32 Steps = FMath::Max(1, FMath::RoundToInt(Seconds / Dt));
	for (int32 i = 0; i < Steps; ++i)
	{
		// each rate a half sine over the motion (its peak in the middle), turned in the held gun's frame (BUG-FLICKFEEL: the hand's
		// frame turned by the gun's mount, where FlickStep reads it; the hand's own with no gun), the hand carried along the barrel
		// (TestHandAcross: across it, a chop)
		const float S = FMath::Sin(PI * (float(i) + 0.5f) / float(Steps));
		const FRotator Step(PitchPeak * S * Dt, YawPeak * S * Dt, RollPeak * S * Dt);
		// (BUG-FLICKUP) with the wrist rolled, turned about the gun's unrolled axes (the muzzle raised or dropped, the gun canted)
		const FQuat Gq = F->GetComponentQuat() * (WeaponMesh[Hand] ? GunBaseRot[Hand].Quaternion() : FQuat::Identity)
			* FQuat(FVector::ForwardVector, FMath::DegreesToRadians(-TestWristRoll));
		F->SetWorldRotation((Gq * Step.Quaternion() * Gq.Inverse()) * F->GetComponentQuat());
		HandVelocity[Hand] = (TestHandAcross ? Gq.GetUpVector() : Gq.GetForwardVector()) * HandSpeed * S;
		TickBreak(Hand, Dt, false);
		if (AOutlawsLevelActor* L = Level.Get()) L->TickSoundRuns(Dt);
	}
	HandVelocity[Hand] = FVector::ZeroVector;
}

void AOutlawsPlayer::TestHandStill(int32 Hand, float Seconds)
{
	constexpr float Dt = 1.f / 90.f;
	HandVelocity[Hand] = FVector::ZeroVector;
	AOutlawsLevelActor* L = Level.Get();
	for (int32 i = 0, N = FMath::Max(1, FMath::RoundToInt(Seconds / Dt)); i < N; ++i)
	{
		TickBreak(Hand, Dt, false);
		if (L) L->TickSoundRuns(Dt);          // (the reload's slices keep time with the simulated hand)
	}
}

void AOutlawsPlayer::TestFrontHandAt(int32 Hand, float Away)
{
	const int32 O = 1 - Hand;
	if (Hand < 0 || Hand > 1 || !WeaponMesh[Hand] || Equipped[Hand] < 0 || !GripOf(O)) return;
	const FTransform Xf = WeaponMesh[Hand]->GetComponentTransform();
	GripOf(O)->SetWorldLocation(Xf.TransformPosition(ForendSpot(Equipped[Hand])) - Xf.GetUnitAxis(EAxis::Z) * Away);
}

int32 AOutlawsPlayer::TestShellsFlying(int32& OutResting) const
{
	int32 N = 0;
	OutResting = 0;
	for (const FFxBit& P : Cases)
	{
		if (!P.bBounce || P.Age >= P.Life) continue;
		++N;
		OutResting += P.bRest ? 1 : 0;
	}
	return N;
}
#endif // WITH_OUTLAWS_TESTS

void AOutlawsPlayer::UpdateFx(float Dt)
{
	OL_PERF_SCOPE("Player: fx");
	for (int32 i = 0; i < Puffs.Num(); ++i)
	{
		FFxBit& P = Puffs[i];
		if (P.Age >= P.Life)
		{
			continue;
		}
		P.Age += Dt;
		if (P.Age >= P.Life)
		{
			PuffFx[i]->SetVisibility(false);
			continue;
		}
		// drag slows the burst, warm smoke rises, and it swells as it thins out
		const float T = P.Age / P.Life;
		P.Vel *= FMath::Exp(-2.8f * Dt);
		P.Vel.Z += 14.f * Dt;
		P.Pos += P.Vel * Dt;
		const float Size = FMath::Lerp(P.Size0, P.Size1, 1.f - FMath::Square(1.f - T));
		PuffFx[i]->SetWorldLocation(P.Pos);
		PuffFx[i]->SetWorldScale3D(FVector(Size / 100.0));
		if (PuffMat[i])
		{
			PuffMat[i]->SetScalarParameterValue(TEXT("Opacity"), P.Alpha * FMath::Pow(1.f - T, 1.5f) * FMath::Min(1.f, T * 12.f));
		}
	}
	for (int32 i = 0; i < Cases.Num(); ++i)
	{
		FFxBit& P = Cases[i];
		if (P.Age >= P.Life)
		{
			continue;
		}
		P.Age += Dt;
		if (P.Age >= P.Life)
		{
			CaseFx[i]->SetVisibility(false);
			continue;
		}
		if (P.bRest)
		{
			continue;
		}
		P.Vel.Z -= 980.f * Dt;
		FVector To = P.Pos + P.Vel * Dt;
		// (REQ-FLICKRELOAD) a shotgun shell meets the level: it hops off at a third of its speed and soon lies still
		FHitResult Hit;
		if (P.bBounce && GetWorld() && GetWorld()->LineTraceSingleByChannel(Hit, P.Pos, To, ECC_Visibility, FCollisionQueryParams(SCENE_QUERY_STAT(OutlawsShell), false, this)))
		{
			To = Hit.ImpactPoint + Hit.ImpactNormal * 1.0;
			P.Vel = P.Vel.MirrorByVector(FVector(Hit.ImpactNormal)) * 0.35f;
			P.Spin *= 0.5f;
			if (P.Vel.Size() < 40.f && Hit.ImpactNormal.Z > 0.6f)
			{
				P.bRest = true;
				P.Rot = FRotator(0.0, P.Rot.Yaw, 90.0);      // on its side, until its two seconds are up
			}
		}
		P.Pos = To;
		if (!P.bRest) P.Rot += P.Spin * Dt;
		CaseFx[i]->SetWorldLocationAndRotation(P.Pos, P.Rot);
	}
}

AOutlawsEnemy* AOutlawsPlayer::FindFanTarget(int32 Hand) const
{
	const int32 Wi = Equipped[Hand];
	if (Wi < 0 || !WeaponMesh[Hand])
	{
		return nullptr;
	}
	return FindViewTarget(FanViewCone, GWeapons[Wi].RangeCm, WeaponMesh[Hand]->GetComponentLocation());
}

AOutlawsEnemy* AOutlawsPlayer::FindViewTarget(float ConeDeg, float RangeCm, const FVector& From) const
{
	// the living bad guy nearest the middle of the view (least angle off the eyes' forward), within
	// the cone, in range of the gun and in plain sight of the eyes
	const AOutlawsLevelActor* L = Level.Get();
	if (!L || !GetWorld())
	{
		return nullptr;
	}
	const FVector Eye = Camera->GetComponentLocation();
	const FVector Look = Camera->GetForwardVector();
	double BestDot = FMath::Cos(FMath::DegreesToRadians(ConeDeg));
	AOutlawsEnemy* Best = nullptr;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsViewAim), true, this);
	for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
	{
		AOutlawsEnemy* E = Wk.Get();
		if (!E || E->IsDead() || E->IsDormant())
		{
			continue;
		}
		const FVector Chest = E->GetActorLocation() + FVector(0.0, 0.0, 25.0);
		if (FVector::Dist(Chest, From) > RangeCm)
		{
			continue;
		}
		const double Dot = FVector::DotProduct((Chest - Eye).GetSafeNormal(), Look);
		if (Dot <= BestDot)
		{
			continue;
		}
		FHitResult Hit;
		if (GetWorld()->LineTraceSingleByChannel(Hit, Eye, Chest, ECC_Visibility, Q) && Hit.GetActor() != E)
		{
			continue;
		}
		Best = E;
		BestDot = Dot;
	}
	return Best;
}

// ---- -OLShots -OLGrabFallTest --------------------------------------------------------------------

#if WITH_OUTLAWS_TESTS
void AOutlawsPlayer::GrabFallTestTick()
{
	// (BUG-GRABFALL) a far grab off a roof: the nearest living man put 8 m over the floor 3 m ahead and taken by the right hand
	// that frame (as a pointed grab takes him: the glide from there to the fist), then let go: once dropped after a second's
	// hold, once dropped in the glide. Each landing logged with his health before and after: no fall from the hand, no harm
	AOutlawsLevelActor* L = Level.Get();
	UWorld* W = GetWorld();
	if (!L || !W) return;
	static TWeakObjectPtr<AOutlawsEnemy> Man;
	static int32 Round = 0;
	static float At = 0.f, Hp = 0.f;
	static bool bDown = false;
	const FVector Fl = FVector(Camera->GetForwardVector().X, Camera->GetForwardVector().Y, 0.0).GetSafeNormal();
	GripOf(1)->SetWorldLocationAndRotation(Camera->GetComponentLocation() + Fl * 50.0 + Camera->GetRightVector() * 15.0 - FVector(0.0, 0.0, 15.0), Fl.Rotation());
	HandVelocity[1] = HandWorldVel[1] = FVector::ZeroVector;
	if (ShotStep == 0 && ShotTime > 3.f)
	{
		double Best = 1e30;
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
		{
			AOutlawsEnemy* E = Wk.Get();
			if (!E || E->IsDead() || E->IsDormant() || E->IsCritter()) continue;
			const double D = FVector::DistSquared(E->GetActorLocation(), GetActorLocation());
			if (D < Best) { Best = D; Man = E; }
		}
		if (!Man.IsValid())
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT grabfall: FAIL no man to take"));
			FPlatformMisc::RequestExit(false);
			return;
		}
		ShotStep = 1;
		At = ShotTime;
	}
	AOutlawsEnemy* E = Man.Get();
	if (ShotStep == 0) return;
	if (!E)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT grabfall: FAIL the man is gone"));
		FPlatformMisc::RequestExit(false);
		return;
	}
	if (ShotStep == 1 && ShotTime - At > 0.5f)
	{
		// up on his "roof": 8 m over the floor 3 m ahead, taken the same frame
		const FVector Ahead = GetActorLocation() + Fl * 300.0;
		FHitResult Hit;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsGrabFallTest), false, this);
		Q.AddIgnoredActor(E);
		const double FloorZ = W->LineTraceSingleByChannel(Hit, Ahead + FVector(0.0, 0.0, 100.0), Ahead - FVector(0.0, 0.0, 2000.0), ECC_Visibility, Q) ? Hit.ImpactPoint.Z : GetActorLocation().Z - 90.0;
		const double Half = E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		E->SetActorLocation(FVector(Ahead.X, Ahead.Y, FloorZ + 800.0 + Half), false, nullptr, ETeleportType::TeleportPhysics);
		Hp = E->GetHealth();
		const bool bOk = TryGrabEnemy(1, E);
		TestAxes.Add(TEXT("OLVR_RightGrip"), 1.f);
		bGripArmed[1] = false;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT grabfall round %d: %s taken from %.0f cm over the floor (%s), health %.2f"), Round + 1, *E->GetName(), E->GetActorLocation().Z - Half - FloorZ, bOk ? TEXT("held") : TEXT("NOT held"), Hp);
		if (!bOk)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT grabfall: FAIL the grab"));
			FPlatformMisc::RequestExit(false);
			return;
		}
		ShotStep = 2;
		At = ShotTime;
		bDown = false;
	}
	else if (ShotStep == 2 && ShotTime - At > (Round == 0 ? 1.f : 0.05f))
	{
		TestAxes.Remove(TEXT("OLVR_RightGrip"));
		LetGoEnemy(1, false, nullptr);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT grabfall round %d: let go %s at %.0f cm"), Round + 1, Round == 0 ? TEXT("after a second") : TEXT("in the glide"), E->GetActorLocation().Z);
		ShotStep = 3;
		At = ShotTime;
	}
	else if (ShotStep == 3 && (ShotTime - At > 6.f || (ShotTime - At > 0.5f && !E->bThrown && !E->bHeldByPlayer)))
	{
		const bool bOk = !E->IsDead() && E->GetHealth() >= Hp - 0.001f;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT grabfall round %d: %s, health %.2f -> %.2f%s"), Round + 1, bOk ? TEXT("PASS") : TEXT("FAIL"), Hp, E->GetHealth(),
			E->IsDead() ? TEXT(" (dead)") : TEXT(""));
		if (++Round < 2)
		{
			ShotStep = 1;
			At = ShotTime;
		}
		else
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT grabfall done"));
			FPlatformMisc::RequestExit(false);
		}
	}
}

OL_SHOT_MATCH(OLGrabFallTest) { return OLFlags::Param(TEXT("OLGrabFallTest")); }
OL_SHOT_TEST(OLGrabFallTest, 50)
{
	GrabFallTestTick();              // (OutlawsPlayerWeapons.cpp)
}
#endif // WITH_OUTLAWS_TESTS

// ---- -OLShots -OLThrowTest -----------------------------------------------------------------------

#if WITH_OUTLAWS_TESTS
void AOutlawsPlayer::ThrowTestTick()
{
	// the two throws side by side (user 2026-10-02: a held bad guy flew too slow, a rock too fast): a rock and a held man let
	// go with the same swing of the hand, 400 cm/s ahead and 30 degrees up, then the two thrown along the arc at half power,
	// then the man killed and his body let go with the same swing.  Each flight logged: its speed leaving the hand, how high
	// it rose, how far off and how long after it came back down to the hand's height, and where it came to rest.  At the
	// headset's own fall (BaseGravityScale 1.5; the desktop's is 4.59)
	AOutlawsLevelActor* L = Level.Get();
	UWorld* W = GetWorld();
	if (!L || !W) return;
	static TWeakObjectPtr<AOutlawsEnemy> Man;
	static TWeakObjectPtr<AActor> Rock;
	static FString What;
	static FVector From, Prev;
	static double T0 = 0.0, PrevT = 0.0;
	static float Peak = 0.f, Speed0 = 0.f, Yaw = 0.f, NextAt = 0.f;
	static bool bFlying = false, bCrossed = false;
	static int32 Flick = 0, ManThrow = 0;
	static float FlickHandYaw = 0.f, CrossDist = 0.f, Preview = -1.f;
	static FVector LaunchV = FVector::ZeroVector;
	const double Now = W->GetTimeSeconds();
	const FVector Fwd = FRotator(0.f, Yaw, 0.f).Vector();
	// the right hand held out before the eyes (no tracked hand on the desktop); a man held up over the head, his feet off
	// the floor (held lower they are in it, and he slides off along it)
	if (ShotStep > 0)
	{
		const FVector Fl = FVector(Camera->GetForwardVector().X, Camera->GetForwardVector().Y, 0.0).GetSafeNormal();
		const double Up = ShotStep >= 3 ? 110.0 : -15.0;
		GripOf(1)->SetWorldLocationAndRotation(Camera->GetComponentLocation() + Fl * 50.0 + Camera->GetRightVector() * 15.0 + FVector(0.0, 0.0, Up), Fl.Rotation());
	}
	const FVector Swing = (Fwd * FMath::Cos(FMath::DegreesToRadians(30.f)) + FVector(0.0, 0.0, FMath::Sin(FMath::DegreesToRadians(30.f)))) * 400.0;
	auto Begin = [&](const TCHAR* Name, AActor* A)
	{
		What = Name;
		From = Prev = A->GetActorLocation();
		T0 = PrevT = Now;
		Peak = 0.f;
		bFlying = true;
		bCrossed = false;
		CrossDist = 0.f;
		LaunchV = FVector::ZeroVector;
	};
	auto HandOut = [&]()
	{
		HandVelocity[1] = Swing;
		HandWorldVel[1] = Swing;
	};
	auto MakeRock = [&]()
	{
		UProceduralMeshComponent* M = NewObject<UProceduralMeshComponent>(this);
		M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		M->RegisterComponent();
		M->SetWorldLocation(GripOf(1)->GetComponentLocation());
		TArray<FVector> V = { FVector(-3.0, 0.0, 0.0), FVector(3.0, 0.0, 0.0), FVector(0.0, 0.0, 4.0) };
		TArray<int32> Tri = { 0, 1, 2 };
		M->CreateMeshSection(0, V, Tri, TArray<FVector>(), TArray<FVector2D>(), TArray<FColor>(), TArray<FProcMeshTangent>(), false);
		M->UpdateBounds();
		HeldBottle[1] = M;
		bHeldBreaks[1] = false;
		BottleGrow[1] = 1.f;
		BottleSound[1].Reset();
		BottleHold[1].Reset();
		BottleAxis[1] = FVector::UpVector;
	};
	auto LastRock = [&]() -> AActor*
	{
		if (Thrown.Num() == 0) return nullptr;
		Thrown.Last().Homing = nullptr;          // (no bending onto a bad guy: the bare flight)
		Speed0 = float(Thrown.Last().Vel.Size());
		return Thrown.Last().Actor.Get();
	};
	auto GrabMan = [&]() -> bool
	{
		AOutlawsEnemy* E = Man.Get();
		if (!E) return false;
		E->SetActorLocation(GripOf(1)->GetComponentLocation() + Fwd * 40.0, false, nullptr, ETeleportType::TeleportPhysics);
		const bool bOk = TryGrabEnemy(1, E);
		TestAxes.Add(TEXT("OLVR_RightGrip"), 1.f);     // (held: no hand on the desktop squeezes)
		bGripArmed[1] = false;
		return bOk;
	};
	// (BUG-THROWAIM) the hand set at a height over the floor (100 cm a lob, 150 the middle, 207 the longest), 50 cm out
	// from the face toward Yaw + Side, the wrist as given and the hand at rest; the head stays where it is
	auto PlaceHand = [&](double Height, float Side, const FRotator& Wrist)
	{
		FVector P = Camera->GetComponentLocation() + FRotator(0.f, Yaw + Side, 0.f).Vector() * 50.0;
		P.Z = VROrigin->GetComponentLocation().Z + Height;
		GripOf(1)->SetWorldLocationAndRotation(P, Wrist);
		AimOf(1)->SetWorldRotation(Wrist);
		HandVelocity[1] = HandWorldVel[1] = FVector::ZeroVector;
		FlickHandYaw = Yaw + Side;
	};
	// where the arc shown in the hand ends, as far along the ground from the hand
	auto PreviewOff = [&]() -> float
	{
		ShowThrowArc(1, true);
		const int32 Per = ArcDotCount + 1;
		UStaticMeshComponent* Mark = ArcDots.IsValidIndex(Per + ArcDotCount) ? ArcDots[Per + ArcDotCount] : nullptr;
		const float Off = Mark && Mark->IsVisible() ? float(FVector::Dist2D(Mark->GetComponentLocation(), GripOf(1)->GetComponentLocation())) : -1.f;
		ShowThrowArc(1, false);
		return Off;
	};
	auto WeaponOf = [&](int32 Kind) -> int32
	{
		for (int32 i = 0; i < NumWeapons; ++i) if (GWeapons[i].Throw == Kind) return i;
		return -1;
	};
	auto LogFlight = [&](const FString& Name, const FVector& V, float Back, float Apex, float Rest)
	{
		const float G = ThrowGravity(From);
		const float Vxy = float(FVector(V.X, V.Y, 0.0).Size());
		const float Ang = FMath::RadiansToDegrees(FMath::Atan2(float(V.Z), Vxy));
		UE_LOG(LogOutlaws, Display, TEXT("OLRT throw: %s speed %.0f cm/s angle %.0f deg, distance %.0f cm (flat calc %.0f), apex %.0f cm (calc %.0f), came to rest %.0f cm off"),
			*Name, V.Size(), Ang, Back, 2.f * float(V.Z) * Vxy / G, Apex, FMath::Square(float(FMath::Max(0.0, V.Z))) / (2.f * G), Rest);
	};
	if (bFlying)
	{
		// the flight followed: the rock by its actor, the man by his
		AActor* A = Rock.IsValid() ? Rock.Get() : static_cast<AActor*>(Man.Get());
		bool bDone = !A;
		if (A)
		{
			const FVector P = A->GetActorLocation();
			if (Speed0 <= 0.f && !Rock.IsValid()) Speed0 = float(Man->GetCharacterMovement()->Velocity.Size());
			Peak = FMath::Max(Peak, float(P.Z - From.Z));
			if (!bCrossed && Now - T0 > 0.05 && P.Z < From.Z && Prev.Z >= From.Z)
			{
				const double K = (Prev.Z - From.Z) / FMath::Max(0.001, Prev.Z - P.Z);
				const FVector X = FMath::Lerp(Prev, P, K);
				const double Tx = FMath::Lerp(PrevT, Now, K) - T0;
				bCrossed = true;
				CrossDist = float(FVector::Dist2D(X, From));
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT throw %s: left at %.0f cm/s, rose %.0f cm, back at the hand's height %.0f cm off after %.2f s"),
					*What, Speed0, Peak, FVector::Dist2D(X, From), Tx);
			}
			bool bLanded = false;
			if (Rock.IsValid())
			{
				for (const FThrown& T : Thrown) if (T.Actor.Get() == A) bLanded = T.bResting;
			}
			else
			{
				bLanded = !Man->bThrown && !Man->bHeldByPlayer;
			}
			if (bLanded || Now - T0 > 10.0)
			{
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT throw %s: came to rest %.0f cm off, %.0f cm below, %.2f s after%s"),
					*What, FVector::Dist2D(P, From), From.Z - P.Z, Now - T0, bLanded ? TEXT("") : TEXT(" (timed out)"));
				if (!LaunchV.IsZero()) LogFlight(What, LaunchV, bCrossed ? CrossDist : -1.f, Peak, float(FVector::Dist2D(P, From)));
				{
					// the aimed man throw beside the palm-up push (user 2026-10-06: a thrown man as far as the push)
					static TMap<FString, FVector2D> ManFlights;
					ManFlights.Add(What, FVector2D(FVector::Dist2D(P, From), Peak));
					if (What == TEXT("man push release palm up"))
					{
						FString Line;
						for (const TCHAR* K : { TEXT("man hand 100 cm up"), TEXT("man hand 150 cm up"), TEXT("man hand 207 cm up"), TEXT("man push release palm up") })
						{
							const FVector2D* F = ManFlights.Find(K);
							Line += FString::Printf(TEXT("%s%s: %.0f cm, apex %.0f"), Line.IsEmpty() ? TEXT("") : TEXT("; "), K, F ? F->X : -1.0, F ? F->Y : -1.0);
						}
						UE_LOG(LogOutlaws, Display, TEXT("OLSHOT throw compare: %s"), *Line);
					}
				}
				if (Preview >= 0.f) UE_LOG(LogOutlaws, Display, TEXT("OLRT throw preview: %s arc mark %.0f cm off, came to rest %.0f cm off"), *What, Preview, FVector::Dist2D(P, From));
				Preview = -1.f;
				if (What.StartsWith(TEXT("wrist")))
				{
					const float HeadYaw = float(Camera->GetComponentRotation().Yaw);
					const FVector HeadRight = FRotator(0.f, HeadYaw, 0.f).Quaternion().GetRightVector();
					const FVector Off = P - From;
					const float Side = float(FVector::DotProduct(Off, HeadRight));
					const float LandYaw = float(FVector(Off.X, Off.Y, 0.0).Rotation().Yaw);
					UE_LOG(LogOutlaws, Display, TEXT("OLRT throw: %s landed at %.0f,%.0f,%.0f, hand dir %.0f, head dir %.0f, landed toward %.0f (%+.0f off the hand), %+.0f cm to the head's right"),
						*What, P.X, P.Y, P.Z, FRotator::NormalizeAxis(FlickHandYaw), FRotator::NormalizeAxis(HeadYaw), LandYaw,
						FMath::FindDeltaAngleDegrees(FlickHandYaw, LandYaw), Side);
				}
				bDone = true;
			}
			Prev = P;
			PrevT = Now;
		}
		if (bDone)
		{
			bFlying = false;
			Rock.Reset();
			NextAt = ShotTime + 1.5f;
		}
		return;
	}
	if (ShotStep == 0 && ShotTime > 3.f)
	{
		BaseGravityScale = 1.5f;              // (the headset's)
		L->GameSpeed = 0.5f;                  // (the default, shown 100 %; not saved)
		// the longest clear way from here at chest height (15 m looked along 24 ways)
		const FVector Eye = GetActorLocation() + FVector(0.0, 0.0, 40.0);
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsThrowTestWay), false, this);
		float Best = -1.f;
		for (int32 i = 0; i < 24; ++i)
		{
			const float Yw = i * 15.f;
			FHitResult Hit;
			const FVector To = Eye + FRotator(0.f, Yw, 0.f).Vector() * 1500.0;
			const float D = W->LineTraceSingleByChannel(Hit, Eye, To, ECC_Visibility, Q) ? float(Hit.Distance) : 1500.f;
			if (D > Best) { Best = D; Yaw = Yw; }
		}
		if (Controller) Controller->SetControlRotation(FRotator(0.f, Yaw, 0.f));
		AOutlawsEnemy* Pick = nullptr;
		double BestD = TNumericLimits<double>::Max();
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
		{
			AOutlawsEnemy* E = Wk.Get();
			if (!E || E->IsDead() || E->bBoss || E->IsCritter()) continue;
			const double D = FVector::Dist(E->GetActorLocation(), GetActorLocation());
			if (D < BestD) { BestD = D; Pick = E; }
		}
		Man = Pick;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT throw test: clear way %.0f cm at yaw %.0f, man %s, speed setting %.2f (enemy pace %.2f), gravity scale %.2f"),
			Best, Yaw, Pick ? *Pick->GetName() : TEXT("none"), L->GameSpeed, L->EnemyPace(), BaseGravityScale);
		NextAt = ShotTime + 1.f;
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > NextAt)
	{
		MakeRock();
		HandOut();
		ReleaseBottle(1, false);
		Rock = LastRock();
		if (Rock.IsValid()) Begin(TEXT("rock let go"), Rock.Get());
		++ShotStep;
	}
	else if (ShotStep == 2 && ShotTime > NextAt)
	{
		// (BUG-THROWAIM, user 2026-10-06) the trigger throws, the head held still: dynamite, the knife and a prop with the
		// hand low, middling and high; then a prop with the hand 45 degrees left and right of the face, the wrist rolled 45
		// and pitched down, the body strafing at 400 cm/s, a 45 degree snap turn in the hold; then a prop set down gently
		// with the grip (the drop).  Each to land along the face-to-hand line, as far as its arc showed
		static const TCHAR* ItemName[3] = { TEXT("dynamite"), TEXT("knife"), TEXT("prop") };
		static const double Height[3] = { 100.0, 150.0, 207.0 };
		const int32 Was = Equipped[1];
		FVector V;
		FString Name;
		int32 Item = 2;
		if (Flick < 9)
		{
			Item = Flick / 3;
			PlaceHand(Height[Flick % 3], 0.f, FRotator(0.f, Yaw, 0.f));
			Name = FString::Printf(TEXT("%s hand %.0f cm up"), ItemName[Item], Height[Flick % 3]);
		}
		else if (Flick < 11)
		{
			const float Side = Flick == 9 ? -45.f : 45.f;
			PlaceHand(207.0, Side, FRotator(0.f, Yaw, 0.f));
			Name = FString::Printf(TEXT("wrist prop hand %+.0f of the face"), Side);
		}
		else if (Flick == 11)
		{
			PlaceHand(207.0, 45.f, FRotator(-40.f, Yaw - 30.f, 45.f));
			Name = TEXT("wrist prop hand +45, wrist rolled 45 pitched -40 pointing -30");
		}
		else if (Flick == 12)
		{
			PlaceHand(207.0, 0.f, FRotator(0.f, Yaw, 0.f));
			Name = TEXT("wrist prop strafing right 400 cm/s");
		}
		else if (Flick == 13)
		{
			PlaceHand(207.0, 0.f, FRotator(0.f, Yaw, 0.f));
			Name = TEXT("wrist prop snap turn 45 in the hold");
		}
		if (Flick < 14)
		{
			MakeRock();
			Equipped[1] = Item == 0 ? WeaponOf(1) : Item == 1 ? WeaponOf(2) : -1;
			FVector Carry = FVector::ZeroVector;
			if (Flick == 12)
			{
				// the stick held: the body moving sideways (the old throw added it, BodyCarry)
				Carry = FRotator(0.f, Yaw + 90.f, 0.f).Vector() * 400.0;
				GetCharacterMovement()->Velocity = Carry;
			}
			if (Flick == 13) AddActorWorldRotation(FRotator(0.f, 45.f, 0.f));
			// the hand's way from the face as it now stands (turned, the hand went round with the body)
			const FVector Rel = GripOf(1)->GetComponentLocation() - Camera->GetComponentLocation();
			FlickHandYaw = float(FVector(Rel.X, Rel.Y, 0.0).Rotation().Yaw);
			V = ThrowVelocity(1);
			Preview = PreviewOff();
			if (Flick == 12)
			{
				const float OldOff = FMath::FindDeltaAngleDegrees(float(V.Rotation().Yaw), float((V + Carry).Rotation().Yaw));
				UE_LOG(LogOutlaws, Display, TEXT("OLRT throw stick: strafing 400 cm/s the old throw (body carried) would land %+.1f deg off the hand, the new %+.1f"),
					OldOff, FMath::FindDeltaAngleDegrees(FlickHandYaw, float(V.Rotation().Yaw)));
				GetCharacterMovement()->Velocity = FVector::ZeroVector;
			}
			if (Flick == 13) AddActorWorldRotation(FRotator(0.f, -45.f, 0.f));
			Equipped[1] = Was;
			ReleaseBottle(1, true);
			Rock = LastRock();
			if (Rock.IsValid())
			{
				Thrown.Last().Vel = V;
				Speed0 = float(V.Size());
				Begin(*Name, Rock.Get());
				LaunchV = V;
			}
		}
		else
		{
			// set down: the grip opened with the hand at rest at the chest
			PlaceHand(130.0, 0.f, FRotator(0.f, Yaw, 0.f));
			MakeRock();
			ReleaseBottle(1, false);
			Rock = LastRock();
			if (Rock.IsValid()) Begin(TEXT("prop set down gently"), Rock.Get());
		}
		if (++Flick >= 15) ++ShotStep;
	}
	else if (ShotStep == 3 && ShotTime > NextAt)
	{
		if (ManThrow == 7 && Man.IsValid())
		{
			for (int32 i = 0; i < 60 && !Man->IsDead(); ++i) Man->TakeHit(50.f, Man->GetActorLocation(), Fwd, Man->GetActorLocation() - Fwd * 100.0);
		}
		const bool bOk = GrabMan();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT throw test: man taken up %d"), bOk ? 1 : 0);
		NextAt = ShotTime + 1.5f;
		++ShotStep;
	}
	else if (ShotStep == 4 && ShotTime > NextAt)
	{
		// a held man: the hand high, middling, low, high 45 degrees right of the face and high strafing; then the push
		// release (the palm ahead, pushed at the speed that gives power 0.727: 514 cm/s; then the palm 40 degrees up, the
		// launch's top 45), then killed and his body let go
		static const TCHAR* ManName[8] = { TEXT("man hand 207 cm up"), TEXT("man hand 150 cm up"), TEXT("man hand 100 cm up"), TEXT("wrist man hand +45 of the face"),
			TEXT("wrist man strafing right 400 cm/s"), TEXT("man push release"), TEXT("man push release palm up"), TEXT("body let go") };
		AOutlawsEnemy* E = Man.Get();
		if (E && GrabbedEnemy[1].Get() == E)
		{
			Begin(ManName[ManThrow], E);
			Speed0 = 0.f;
			if (ManThrow < 5)
			{
				static const double ManHeight[5] = { 207.0, 150.0, 100.0, 207.0, 207.0 };
				PlaceHand(ManHeight[ManThrow], ManThrow == 3 ? 45.f : 0.f, FRotator(0.f, Yaw, 0.f));
				if (ManThrow == 4) GetCharacterMovement()->Velocity = FRotator(0.f, Yaw + 90.f, 0.f).Vector() * 400.0;
				const FVector V = ThrowVelocity(1);
				Preview = PreviewOff();
				TestAxes.Remove(TEXT("OLVR_RightGrip"));
				LetGoEnemy(1, true, &V);
				LaunchV = V;
			}
			else if (ManThrow < 7)
			{
				const float PalmUp = ManThrow == 5 ? 0.f : 40.f;
				PushPeak[1] = 150.f + 0.727f * 500.f;
				PushAt[1] = FPlatformTime::Seconds();
				PushDir[1] = FRotator(PalmUp, Yaw, 0.f).Vector();
				HandVelocity[1] = HandWorldVel[1] = FVector::ZeroVector;
				TestAxes.Remove(TEXT("OLVR_RightGrip"));       // (let go: the palm launch next frame)
				LaunchV = FRotator(FMath::Clamp(PalmUp + 8.f, 12.f, 45.f), Yaw, 0.f).Vector() * (FMath::Lerp(500.f, 2600.f, 0.727f) * ManThrowScale);
			}
			else
			{
				TestAxes.Remove(TEXT("OLVR_RightGrip"));
				HandOut();
				LetGoEnemy(1, true);
			}
		}
		else
		{
			TestAxes.Remove(TEXT("OLVR_RightGrip"));
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT throw test: the man is not in the hand (throw %d)"), ManThrow);
			NextAt = ShotTime + 0.5f;
		}
		ShotStep = ++ManThrow >= 8 ? 5 : 3;
	}
	else if (ShotStep == 5 && ShotTime > NextAt)
	{
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

OL_SHOT_MATCH(OLThrowTest) { return OLFlags::Param(TEXT("OLThrowTest")); }
OL_SHOT_TEST(OLThrowTest, 230)
{
	ThrowTestTick();                 // (OutlawsPlayerWeapons.cpp)
}
#endif // WITH_OUTLAWS_TESTS

// ---- -OLShots -OLCharLoadoutTest (BUG-CHARLOADOUT, REQ-MARYNAILS) ------------------------------------------------------------

#if WITH_OUTLAWS_TESTS
void AOutlawsPlayer::CharLoadoutTestTick()
{
	// each multiplayer character's kit made as a match's spawn makes it (the kit, the hands emptied, START_WEAPON drawn), its
	// revolver fired once; every row checked against the character's ITMs read here on their own.  Mary's hands held up to
	// the eyes for the nails (ol_marynails), then single player's James back: his kit, his guns, the hands' own nails
	AOutlawsLevelActor* L = Level.Get();
	const FOutlawsAssets* A = L ? L->GetLabAssets() : nullptr;
	if (!L || !A) return;
	static int32 Fails = 0, Rows = 0;
	static float NextAt = 3.f;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT charloadout check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (!bOk) ++Fails;
	};
	auto Itm = [A](const FString& Name, TMap<FString, FString>& D)
	{
		FString Text, Func;
		D.Reset();
		return A->GetText(Name, Text) && (OutlawsFormats::ParseItm(Text, Func, D), true);
	};
	int32 NumChars = 0;
	const FOutlawsMpCharacter* Chars = FOutlawsMpCharacter::All(NumChars);
	const int32 Hand = LeftDominant() ? 0 : 1;
	auto HoldHands = [this](double Ahead)
	{
		const FVector Eye = Camera->GetComponentLocation();
		const FVector F = Camera->GetForwardVector(), R = Camera->GetRightVector();
		const FVector Fl = FVector(F.X, F.Y, 0.0).GetSafeNormal();
		for (int32 H = 0; H < 2; ++H)
		{
			UOutlawsBoxHand* Box = H == 0 ? LeftHand.Get() : RightHand.Get();
			if (!Box) continue;
			const double Side = H == 0 ? -1.0 : 1.0;
			const FVector Pos = Eye + Fl * Ahead + R * Side * 8.0 - FVector::UpVector * 16.0;
			const FQuat Rot = FRotationMatrix::MakeFromXZ((Fl - R * Side * 0.25).GetSafeNormal(), FVector::UpVector).ToQuat();
			Box->SetVisibility(true, true);
			FrameOf(H)->SetWorldLocationAndRotation(Pos, Rot);
			Box->SetCurl(0.f, 0.f);
		}
	};
	if (ShotTime < NextAt) { if (ShotStep >= NumChars && ShotStep < NumChars + 3) HoldHands(26.0); return; }
	if (ShotStep < NumChars)
	{
		const FOutlawsMpCharacter& Ch = Chars[ShotStep];
		// what the ITMs say, read here apart from the game's own reading
		TMap<FString, FString> C;
		Itm(Ch.Itm, C);
		TMap<FString, int32> Want;
		for (int32 i = 1; i < 16; ++i)
			if (const FString* It = C.Find(FString::Printf(TEXT("START_ITEM_%d"), i)))
				Want.FindOrAdd(It->TrimStartAndEnd().ToLower()) += C.Contains(FString::Printf(TEXT("START_COUNT_%d"), i)) ? FCString::Atoi(*C[FString::Printf(TEXT("START_COUNT_%d"), i)]) : 1;
		// (REQ-SANCHEZDUAL: the table's revolvers at least; Spit'n Jack's ITM gives one, the table two)
		if (Ch.StartPistols > 0) Want.FindOrAdd(TEXT("ipistol")) = FMath::Max(Want.FindRef(TEXT("ipistol")), int32(Ch.StartPistols));
		const int32 WantSlot = C.Contains(TEXT("START_WEAPON")) ? FCString::Atoi(*C[TEXT("START_WEAPON")]) : -1;
		// (BUG-MPDEATHSOUND: the death cry, the ITM's SND_DIE without its .wav)
		const FString WantDie = FPaths::GetBaseFilename(C.FindRef(TEXT("SND_DIE")).TrimStartAndEnd()).ToLower();
		const FString Base = FPaths::GetBaseFilename(Ch.Itm).ToLower();
		const FString Own = FString::Printf(TEXT("i%c%s"), Base.Len() > 3 ? Base[3] : TCHAR('?'), TEXT("pistol"));
		TMap<FString, FString> Pd;
		const bool bOwn = Itm(Own + TEXT(".itm"), Pd);
		const bool bJames = FString(Ch.Short) == TEXT("Jam");
		const FString WantSound = bJames || !bOwn ? FString(TEXT("pistol2")) : Pd.FindRef(TEXT("FIRE_SOUND_1")).ToLower();
		const float WantDamage = bJames || !bOwn ? 2.f : FCString::Atof(*Pd.FindRef(TEXT("DAMAGE_1")));
		// BUG-CHAROIL: as the story's spawn (campaign, Historical Missions): James's basics, each at least his count (the
		// oil among them), and the character's own on top
		{
			TMap<FString, FString> J;
			Itm(TEXT("cidjames.itm"), J);
			TMap<FString, int32> Basics;
			for (int32 i = 1; i < 16; ++i)
				if (const FString* It = J.Find(FString::Printf(TEXT("START_ITEM_%d"), i)))
					Basics.FindOrAdd(It->TrimStartAndEnd().ToLower()) += J.Contains(FString::Printf(TEXT("START_COUNT_%d"), i)) ? FCString::Atoi(*J[FString::Printf(TEXT("START_COUNT_%d"), i)]) : 1;
			L->ApplyStartKit(Ch.Short, 0);
			ResetForLevel();
			EquipStartWeapon();
			bool bBasics = Basics.Contains(TEXT("ioil")), bOwnOnTop = true;
			FString StoryLine;
			for (const TPair<FString, int32>& W : Basics)
			{
				StoryLine += FString::Printf(TEXT("%s%s %d/%d"), StoryLine.IsEmpty() ? TEXT("") : TEXT(", "), *W.Key, L->GetCount(W.Key), W.Value);
				if (L->GetCount(W.Key) < W.Value) bBasics = false;
			}
			for (const TPair<FString, int32>& W : Want)
			{
				if (!Basics.Contains(W.Key)) StoryLine += FString::Printf(TEXT(", own %s %d/%d"), *W.Key, L->GetCount(W.Key), W.Value);
				if (L->GetCount(W.Key) < W.Value) bOwnOnTop = false;
			}
			const int32 StoryFirst = Equipped[Hand];
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT charloadout story %s: [%s] (got/at least); first %s (slot %d); match ITM oil %d"), Ch.Name, *StoryLine,
				StoryFirst >= 0 ? GWeapons[StoryFirst].Label : TEXT("bare hands"), StoryFirst >= 0 ? SlotOfWeapon(A, StoryFirst) : 1, Want.FindRef(TEXT("ioil")));
			Check(bBasics && L->GetCount(TEXT("ioil")) > 0, FString::Printf(TEXT("single player: %s has James's basics, lantern oil %d"), Ch.Name, L->GetCount(TEXT("ioil"))));
			Check(bOwnOnTop && StoryFirst >= 0 && SlotOfWeapon(A, StoryFirst) == WantSlot, FString::Printf(TEXT("single player: %s has its own kit on top and draws START_WEAPON %d"), Ch.Name, WantSlot));
		}
		// as a match's spawn: the kit, the hands emptied, the first weapon drawn; then the revolver fired once
		L->ApplyStartKit(Ch.Short, 1);
		ResetForLevel();
		EquipStartWeapon();
		const int32 First = Equipped[Hand];
		const bool bPairDrawn = First >= 0 && Equipped[1 - Hand] == First;       // (REQ-SANCHEZDUAL)
		const FString Dies = L->DieSound;
		const FString BotDies = FOutlawsCharKit::DieSoundOf(A, Ch.Short);
		FString Got, WantLine;
		bool bCounts = Want.Num() > 0;
		for (const TPair<FString, int32>& W : Want)
		{
			WantLine += FString::Printf(TEXT("%s%s %d"), WantLine.IsEmpty() ? TEXT("") : TEXT(", "), *W.Key, W.Value);
			Got += FString::Printf(TEXT("%s%s %d"), Got.IsEmpty() ? TEXT("") : TEXT(", "), *W.Key, L->GetCount(W.Key));
			if (L->GetCount(W.Key) != W.Value) bCounts = false;
		}
		const FString PistolSound = GWeapons[0].FireSound;
		const float PistolDamage = GWeapons[0].Damage;
		Equip(1 - Hand, 0);
		GLastGunSound.Reset();
		FireGun(1 - Hand, false);
		const FString Fired = GLastGunSound;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT charloadout %s (%s): ITM start [%s], START_WEAPON %d, own revolver %s (%s, %.1f) | now: first %s (slot %d), counts [%s], revolver %s %.1f, fired '%s'"),
			Ch.Name, Ch.Itm, *WantLine, WantSlot, bOwn ? *Own : TEXT("(none)"), *WantSound, WantDamage,
			First >= 0 ? GWeapons[First].Label : TEXT("bare hands"), First >= 0 ? SlotOfWeapon(A, First) : 1, *Got, *PistolSound, PistolDamage, *Fired);
		Check(First >= 0 && SlotOfWeapon(A, First) == WantSlot, FString::Printf(TEXT("%s draws START_WEAPON %d first"), Ch.Name, WantSlot));
		Check(bCounts, FString::Printf(TEXT("%s carries the ITM's start items"), Ch.Name));
		Check(PistolSound.Equals(WantSound, ESearchCase::IgnoreCase) && Fired.Equals(WantSound, ESearchCase::IgnoreCase), FString::Printf(TEXT("%s's revolver sounds %s"), Ch.Name, *WantSound));
		Check(FMath::IsNearlyEqual(PistolDamage, WantDamage, 0.01f), FString::Printf(TEXT("%s's revolver does %.1f"), Ch.Name, WantDamage));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT charloadout deathsound %s: ITM SND_DIE '%s', player dies '%s', bot dies '%s'; hands %s"), Ch.Name, *WantDie, *Dies, *BotDies,
			bPairDrawn ? TEXT("a pair of the first weapon") : TEXT("one first weapon"));
		Check(!WantDie.IsEmpty() && Dies == WantDie && BotDies == WantDie, FString::Printf(TEXT("%s dies with %s (player and bot)"), Ch.Name, *WantDie));
		Check(bPairDrawn == (Ch.StartPistols >= 2) && (Ch.StartPistols < 2 || (First == 0 && WeaponsOwned(0) == int32(Ch.StartPistols))),
			FString::Printf(TEXT("%s %s"), Ch.Name, Ch.StartPistols >= 2 ? TEXT("starts with two revolvers, one in each hand") : TEXT("starts with no pair drawn")));
		++Rows;
		NextAt = ShotTime + 1.5f;
		++ShotStep;
		return;
	}
	const int32 Mary = FOutlawsMpCharacter::IndexOf(TEXT("Mar"));
	if (ShotStep == NumChars)
	{
		// REQ-MARYNAILS: Mary's hands up before the eyes, open
		GEngine->Exec(GetWorld(), TEXT("r.MotionBlurQuality 0"));
		L->ApplyStartKit(Chars[Mary].Short, 1);
		ResetForLevel();
		if (Controller) Controller->SetControlRotation(FRotator(-30.0, GetActorRotation().Yaw, 0.0));
		Check(NailsWorn == (Chars[Mary].NailRGB | 0x1000000u), TEXT("Mary's hands wear her nail colour"));
		NextAt = ShotTime + 2.5f;
		++ShotStep;
		return;
	}
	if (ShotStep == NumChars + 1)
	{
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_marynails.png"), false, false);
		NextAt = ShotTime + 1.f;
		++ShotStep;
		return;
	}
	if (ShotStep == NumChars + 2)
	{
		// single player again: James's kit (cidjames.itm), his guns as built, the hands' own nails
		L->ApplyStartKit();
		ResetForLevel();
		Check(L->WornCharacter.IsEmpty() && FString(GWeapons[0].FireSound) == TEXT("pistol2") && FMath::IsNearlyEqual(GWeapons[0].Damage, 2.f)
			&& L->GetCount(TEXT("irifle")) == 1 && L->GetCount(TEXT("ipistol")) == 1 && L->GetCount(TEXT("isgun")) == 0 && Equipped[0] < 0 && Equipped[1] < 0,
			FString::Printf(TEXT("single player: James's kit and guns (revolver %s %.1f, rifle %d, shotgun %d), hands bare"), GWeapons[0].FireSound, GWeapons[0].Damage,
				L->GetCount(TEXT("irifle")), L->GetCount(TEXT("isgun"))));
		Check(NailsWorn == 0x1000000u, TEXT("single player: the hands' own nails"));
		Check(L->DieSound == TEXT("jadie"), FString::Printf(TEXT("single player: James dies with %s"), *L->DieSound));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT charloadout RESULT %s (%d rows, %d failed)"), Fails == 0 && Rows == NumChars ? TEXT("ALL PASS") : TEXT("FAIL"), Rows, Fails);
		++ShotStep;
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

OL_SHOT_MATCH(OLCharLoadoutTest) { return OLFlags::Param(TEXT("OLCharLoadoutTest")); }
OL_SHOT_TEST(OLCharLoadoutTest, 70)
{
	CharLoadoutTestTick();           // (BUG-CHARLOADOUT: OutlawsPlayerWeapons.cpp)
}
#endif // WITH_OUTLAWS_TESTS

// ---- tests: BUG-REMOTEGUNSOUND (-OLRemoteFxTest) and BUG-TNTDEATHFREEZE (-OLTntDeathTest, -OLRemoteFxTest) --------------------

#if WITH_OUTLAWS_TESTS
FString AOutlawsPlayer::TestFireRevolver()
{
	// the character's revolver, drawn and fired once as the trigger does (the sound it made: also what UOutlawsCoop::NoteShot sent)
	const int32 Hand = LeftDominant() ? 0 : 1;
	Equip(1 - Hand, 0);
	GLastGunSound.Reset();
	FireGun(1 - Hand, false);
	return GLastGunSound;
}

void AOutlawsPlayer::TutorialTestAimGun(int32 Hand, const FVector& Target)
{
	// (REQ-TUTORIAL's test) the grip turned by the bore's miss, twice (the gun hangs off the grip at its own angle)
	const int32 H = Hand & 1;
	UProceduralMeshComponent* Gun = WeaponMesh[H];
	if (!Gun || Equipped[H] < 0) return;
	const FWeaponDef& W = GWeapons[Equipped[H]];
	const FVector ModelBore = W.BoreFix.Quaternion().Inverse().RotateVector(FVector::ForwardVector);
	for (int32 i = 0; i < 3; ++i)
	{
		const FVector Muzzle = Gun->GetComponentTransform().TransformPosition(W.Muzzle);
		const FVector Bore = Gun->GetComponentTransform().TransformVector(ModelBore).GetSafeNormal();
		const FQuat Fix = FQuat::FindBetweenNormals(Bore, (Target - Muzzle).GetSafeNormal());
		GripOf(H)->SetWorldRotation(Fix * GripOf(H)->GetComponentQuat());
	}
}

bool AOutlawsPlayer::TestThrowTnt(const FVector& At, const FVector& Vel, float FuseSec)
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L) return false;
	L->GiveAllWeapons();
	Equip(1, 6);
	Mag[1] = 1;
	bFuseLit[1] = true;
	Fuse[1] = FuseSec;
	const int32 Before = Thrown.Num();
	ThrowHeld(1);
	if (Thrown.Num() <= Before) return false;
	FThrown& T = Thrown.Last();
	T.Pos = At;
	T.Vel = Vel;
	if (AActor* A = T.Actor.Get()) A->SetActorLocation(At);
	return true;
}

bool AOutlawsPlayer::TestTntState(FVector& OutPos, bool& bOutResting) const
{
	for (int32 i = Thrown.Num() - 1; i >= 0; --i)
	{
		if (Thrown[i].Type != 1) continue;
		OutPos = Thrown[i].Pos;
		bOutResting = Thrown[i].bResting;
		return true;
	}
	return false;
}
#endif // WITH_OUTLAWS_TESTS
