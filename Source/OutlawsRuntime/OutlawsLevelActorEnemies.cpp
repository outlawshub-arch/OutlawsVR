// AOutlawsLevelActor, enemies half: bad guys spawned from the OBT, the sprites they and their drops
// use, and the noises that wake them.
#include "OutlawsLevelActor.h"
#include "OutlawsFlags.h"
#include "OutlawsShipSmoke.h"
#include "OutlawsPolygon.h"
#include "OutlawsUnlocks.h"
#include "OutlawsAchievements.h"
#include "OutlawsPerfTest.h"
#include "Algo/Reverse.h"
#include "Components/CapsuleComponent.h"
#include "UObject/UObjectIterator.h"
#include "Misc/CommandLine.h"
#include "HAL/IConsoleManager.h"
#include "OutlawsPerf.h"
#include "OutlawsRuntime.h"
#include "OutlawsLab.h"
#include "OutlawsLevel.h"
#include "OutlawsFormats.h"
#include "Misc/FileHelper.h"
#include "Async/Async.h"
#include "Engine/Engine.h"
#include "OutlawsGeometry.h"
#include "OutlawsTextures.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsEnemy.h"
#include "OutlawsInf.h"
#include "OutlawsInfRuntime.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "Engine/Texture2D.h"
#include "Misc/Paths.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "Components/AudioComponent.h"
#include "Kismet/GameplayStatics.h"
#include "OutlawsPcmWave.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "EngineUtils.h"
#include "TimerManager.h"
#include "GameFramework/Info.h"
#include "OutlawsFormats.h"
#include "OutlawsMovieScreen.h"
#include "ProceduralMeshComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "OutlawsPlayer.h"
#include "OutlawsCoop.h"
#include "OutlawsDeviceProfile.h"
#include "Camera/CameraComponent.h"
#include "OutlawsDecalClip.h"
#if PLATFORM_WINDOWS
#include "Async/Async.h"
#include "Engine/Engine.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"
#include "SoundFileIO/SoundFileIO.h"

// %LOCALAPPDATA%/OutlawsVR/media: the cutscenes and music converted from the player's install (olvr_install.py's cache)
static FString OutlawsMediaDir()
{
	return FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("OutlawsVR"), TEXT("media"));
}

// the music on a PC: the install keeps it as flac (music/TRKnnX.flac), which the game does not play; each track is
// decoded once on a worker (the engine's libsndfile) into the media cache as 16-bit wav, the cue waiting meanwhile
namespace OutlawsMusicDecode
{
	FCriticalSection MusicLock;
	TSet<FString> MusicBusy, MusicFailed;

	// true while the wav is on its way (started now or before); false with no flac, or one that would not decode
	bool Start(const FString& Flac, const FString& Wav)
	{
		{
			FScopeLock Guard(&MusicLock);
			if (MusicFailed.Contains(Wav) || !FPaths::FileExists(Flac)) return false;
			if (MusicBusy.Contains(Wav)) return true;
			MusicBusy.Add(Wav);
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLRT music: decoding %s"), *Flac);
		Async(EAsyncExecution::ThreadPool, [Flac, Wav]()
		{
			static const bool bSndFile = Audio::InitSoundFileIOManager();    // (the editor's audio module loads it; a game must)
			const FString Part = Wav + TEXT(".part");
			TArray<uint8> In, Out;
			const bool bOk = bSndFile && FFileHelper::LoadFileToArray(In, *Flac) && Audio::ConvertAudioToWav(In, Out) && Out.Num() > 44
				&& FFileHelper::SaveArrayToFile(Out, *Part) && IFileManager::Get().Move(*Wav, *Part);
			if (bOk)
			{
				UE_LOG(LogOutlaws, Log, TEXT("OLRT music: %s ready (%d KB)"), *Wav, Out.Num() / 1024);
			}
			else
			{
				UE_LOG(LogOutlaws, Warning, TEXT("OLRT music: could not decode %s (libsndfile %s)"), *Flac, bSndFile ? TEXT("loaded") : TEXT("missing"));
			}
			FScopeLock Guard(&MusicLock);
			MusicBusy.Remove(Wav);
			if (!bOk) MusicFailed.Add(Wav);
		});
		return true;
	}
}
#endif

TSharedPtr<OutlawsTextures::FSpriteAtlas> AOutlawsLevelActor::GetAtlas(const FString& Anim, bool bAllChors)
{
	// one atlas per NWX; characters carry every choreography, props only the first
	const FString Key = (bAllChors ? TEXT("all:") : TEXT("")) + Anim.ToLower();
	if (const TSharedPtr<OutlawsTextures::FSpriteAtlas>* Found = AtlasCache.Find(Key))
	{
		return *Found;
	}
	TSharedPtr<OutlawsTextures::FSpriteAtlas> Atlas;
	if (AssetsRef)
	{
		OL_PERF_SCOPE("Sprites: atlas build");
		Atlas = OutlawsTextures::BuildSpriteAtlas(*AssetsRef, Anim, SpritePalette, true, bAllChors);
	}
	if (Atlas && Atlas->Texture)
	{
		Textures.Add(TEXT("SPR:") + Key, Atlas->Texture);   // keeps the atlas referenced
	}
	AtlasCache.Add(Key, Atlas);
	return Atlas;
}

AOutlawsSpriteActor* AOutlawsLevelActor::MakeSprite(const TSharedPtr<OutlawsTextures::FSpriteAtlas>& Atlas, const FString& Name, const FVector& FeetLoc, float Yaw, int32 Light)
{
	UWorld* World = GetWorld();
	if (!World || !Atlas || !Atlas->Texture || !SpriteQuad || !SpriteMaterial)
	{
		return nullptr;
	}
	const double Wcm = Atlas->CellW / 16.0 * FOutlawsGeometry::UnitsToCm;
	const double Hcm = Atlas->CellH / 16.0 * FOutlawsGeometry::UnitsToCm;
	const FTransform Xf(FRotator::ZeroRotator, FeetLoc + FVector(0.0, 0.0, 0.5), FVector(1.0, Wcm, Hcm));
	FActorSpawnParameters P;
	P.Owner = this;
	P.ObjectFlags |= RF_Transient;
	P.bDeferConstruction = true;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AOutlawsSpriteActor* S = World->SpawnActor<AOutlawsSpriteActor>(AOutlawsSpriteActor::StaticClass(), Xf, P);
	if (!S)
	{
		return nullptr;
	}
	S->OutlawsName = Name;
	S->OutlawsYaw = Yaw;
	S->LightLevel = Light;
	S->AtlasCols = Atlas->Cols;
	S->AtlasRows = Atlas->Rows;
	S->Directions = Atlas->Dirs;
	S->FrameCount = Atlas->Frames;
	S->FPS = Atlas->FPS;
	S->Grid = Atlas->Grid;
	S->SpriteTexture = Atlas->Texture;
	S->bFullBright = IsFullBrightArt(*Atlas);       // (REQ-FIREBRIGHT: a fire that is a bad guy, firex, burns full bright too)
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
	S->Quad->SetStaticMesh(SpriteQuad);
	S->Quad->SetMaterial(0, SpriteMaterial);
	S->FinishSpawning(Xf);
	if (bHDTextures)
	{
		S->SetLook(SpriteHdFor(Atlas));
	}
#if WITH_EDITOR
	S->SetFolderPath(TEXT("OutlawsRuntime"));
#endif
	return S;
}

bool AOutlawsLevelActor::SpawnEnemy(const FString& Name, const TMap<FString, FString>& ItmData, const FString& Anim, const FVector& FeetLoc, float Yaw, int32 Light)
{
	OL_PERF_SCOPE("Bad guys: spawn");      // (TEXLOAD: most of a level build's objects)
	UWorld* World = GetWorld();
	const TSharedPtr<OutlawsTextures::FSpriteAtlas> Atlas = GetAtlas(Anim, true);
	if (!World || !Atlas || Atlas->Chors.Num() == 0)
	{
		return false;
	}
	const float Height = ItmData.Contains(TEXT("HEIGHT")) ? FCString::Atof(*ItmData[TEXT("HEIGHT")]) : 6.f;
	const double Half = FMath::Max(40.0, Height * FOutlawsGeometry::UnitsToCm * 0.5);
	FActorSpawnParameters P;
	P.Owner = this;
	P.ObjectFlags |= RF_Transient;
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
	AOutlawsEnemy* E = World->SpawnActor<AOutlawsEnemy>(AOutlawsEnemy::StaticClass(), FeetLoc + FVector(0.0, 0.0, Half + 2.0), FRotator::ZeroRotator, P);
	if (!E)
	{
		return false;
	}
	AOutlawsSpriteActor* S = MakeSprite(Atlas, Name, FeetLoc, Yaw, Light);
	if (S) { S->SetFeet(Atlas->BelowPx, Atlas->FootPx, Atlas->CellH); S->SetFeetAcross(Atlas->SidePx2, Atlas->CellW); }       // (on the frames' insertion points, user 2026-10-02)
	if (OLFlags::Param(TEXT("OLChorLog")))
	{
		// every chor of this sprite: directions, frames, rate, first-row length, tallest frame, opcodes (tests)
		for (int32 c = 0; c < Atlas->Chors.Num(); ++c)
		{
			const OutlawsTextures::FSpriteAtlas::FChor& Ch = Atlas->Chors[c];
			FString Ev;
			for (const FIntVector4& V : Ch.Events) Ev += FString::Printf(TEXT(" [%d:op%d %d %d]"), V.X, V.Y, V.Z, V.W);
			UE_LOG(LogOutlaws, Log, TEXT("OLRT chor %s %s #%d: %d dirs x %d frames, %.1f fps, shows %d, %d px%s"), *Name, *Anim, c, Ch.Dirs, Ch.Frames, Ch.FPS, Ch.Length, Ch.HeightPx, *Ev);
		}
	}
	// the enemy weapon's pellet scatter (SPREAD_1) from its ITM: WEAPON shotgun -> bgsgun, fist ->
	// bgfist, weaponN -> weaponN, anything else the pistol [EXE 0x140068010]
	TMap<FString, FString> Data = ItmData;
	const FString Weapon = ItmData.FindRef(TEXT("WEAPON")).ToLower();
	const FString WeaponItm = Weapon == TEXT("shotgun") ? TEXT("bgsgun.itm") : Weapon == TEXT("fist") ? TEXT("bgfist.itm")
		: Weapon.StartsWith(TEXT("weapon")) ? Weapon + TEXT(".itm") : TEXT("ipistol.itm");
	FString WeaponText;
	if (AssetsRef && AssetsRef->GetText(WeaponItm, WeaponText))
	{
		FString Func;
		TMap<FString, FString> WeaponData;
		OutlawsFormats::ParseItm(WeaponText, Func, WeaponData);
		for (const TCHAR* Key : { TEXT("SPREAD_1"), TEXT("EFFRANGE_1") })
		{
			if (const FString* V = WeaponData.Find(Key))
			{
				Data.Add(Key, *V);
			}
		}
	}
	E->Init(this, Name, Data, S, Yaw);
	E->SetAtlas(Atlas);
#if WITH_EDITOR
	E->SetActorLabel(FString::Printf(TEXT("%s_enemy"), *Name));
	E->SetFolderPath(TEXT("OutlawsRuntime"));
#endif
	SpawnedActors.Add(E);
	Enemies.Add(E);
	EnemySpawns.Add(FeetLoc);
	return true;
}

namespace
{
	// -OLLevel=ranch -OLNoFrontEnd -OLBossTest (user 2026-10-03: Bob Graham at the ranch's boss door): the key given, the
	// door pushed by script, the movie skipped after 3 s, Bob's state each second, his peeks, then a burst of small hits
	// as a Gatling's would land, then quit
	struct FBossTest { double T = 0.0; int32 Step = 0; double NextLog = 0.0; int32 Hits = 0; };
}

#if WITH_OUTLAWS_TESTS
void AOutlawsLevelActor::TickBossTest(float Dt)
{
	static FBossTest B;
	UWorld* World = GetWorld();
	AOutlawsPlayer* P = World && World->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(World->GetFirstPlayerController()->GetPawn()) : nullptr;
	if (!P || bNeedPlace || bFrontEnd || !Inf) return;
	B.T += Dt;
	AOutlawsEnemy* Bob = nullptr;
	for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : Enemies) if (Wk.IsValid() && Wk->bPeekShooter) Bob = Wk.Get();
	auto Say = [&](const TCHAR* What)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT boss test %.1f s: %s; Bob: %s; movie %d; level sound x%.2f; sight: %s; player at %s"), B.T, What, Bob ? *Bob->DebugBossState() : TEXT("none"), IsMoviePlaying() ? 1 : 0,
			LevelSoundDuck, Bob ? *Bob->DebugSight() : TEXT("-"), *P->GetActorLocation().ToCompactString());
	};
	switch (B.Step)
	{
	case 0:
		if (B.T < 3.0) return;
		bInvincible = true;
		// BUG-PISTOLCAP: alone, the all-weapons cheat and another revolver leave the pair
		GiveAllWeapons();
		{
			const int32 AfterCheat = GetCount(TEXT("ipistol"));
			AddCount(TEXT("ipistol"), 1);
			UE_LOG(LogOutlaws, Log, TEXT("OLRT boss test: revolvers after the all-weapons cheat %d, after one more picked up %d (co-op %d; the cap alone 2)"), AfterCheat,
				GetCount(TEXT("ipistol")), UOutlawsCoop::IsActive(this) ? 1 : 0);
		}
		// BUG-BOBFROZEN: before his door, a shot heard, a neighbour's word, an alert and a hit: none of it wakes him
		if (Bob)
		{
			const FVector Fwd = FRotator(0.0, Bob->GetHeading(), 0.0).Vector();
			Say(TEXT("before the door"));
			Bob->HearShot(P->GetActorLocation());
			Bob->ShareTarget();
			Bob->Wake(true);
			Bob->TakeHit(0.5f, Bob->GetActorLocation(), -Fwd, Bob->GetActorLocation() + Fwd * 600.0);
			Say(TEXT("a shot heard, a neighbour's word, an alert and a hit, before the door"));
		}
		B.Step = 10;
		B.NextLog = B.T + 1.0;
		return;
	case 10:
		// five seconds before the door: still, unaware, no shots
		if (B.T < B.NextLog) return;
		B.NextLog = B.T + 1.0;
		Say(TEXT("waiting before the door"));
		if (B.T < 8.0) return;
		GiveItem(101);
		for (int32 i = 0; i < Inf->Elevators.Num(); ++i)
		{
			if (!Inf->Elevators[i].Name.Equals(TEXT("BOBDOOR"), ESearchCase::IgnoreCase) || Inf->Elevators[i].Sectors.Num() == 0) continue;
			const bool bTaken = PushDoorSector(Inf->Elevators[i].Sectors[0]);
			Say(bTaken ? TEXT("BOBDOOR pushed with key 101: taken") : TEXT("BOBDOOR pushed: NOT taken"));
		}
		B.Step = 1;
		B.NextLog = B.T + 3.0;
		return;
	case 1:
		if (B.T < B.NextLog) return;
		Say(TEXT("three seconds after the push"));
		if (IsMoviePlaying()) SkipMovie();
		B.Step = 2;
		B.NextLog = B.T + 1.0;
		return;
	case 2:
	{
		if (IsMoviePlaying()) return;
		// the player into the room, 3.5 m in front of Bob, facing him
		if (Bob)
		{
			const FVector Fwd = FRotator(0.0, Bob->GetHeading(), 0.0).Vector();     // (his facing, not the actor's)
			FVector At = Bob->GetActorLocation() + Fwd * 350.0;
			P->TeleportTo(At, (Bob->GetActorLocation() - At).Rotation(), false, true);
			if (AController* C = P->GetController()) C->SetControlRotation((Bob->GetActorLocation() - At).Rotation());
		}
		Say(TEXT("the level goes on; the player 3.5 m in front of Bob"));
		B.Step = 3;
		B.NextLog = B.T + 1.0;
		return;
	}
	case 3:
		if (B.T >= B.NextLog)
		{
			B.NextLog = B.T + 1.0;
			Say(TEXT("each second"));
		}
		if (B.T > 40.0)
		{
			B.Step = 4;
			B.NextLog = B.T;
		}
		return;
	case 4:
		// a Gatling's worth of small hits, five a second for three seconds
		if (B.T < B.NextLog) return;
		B.NextLog = B.T + 0.2;
		if (Bob && !Bob->IsDead() && B.Hits < 15)
		{
			++B.Hits;
			const FVector From = P->GetActorLocation();
			Bob->TakeHit(0.2f, Bob->GetActorLocation(), (Bob->GetActorLocation() - From).GetSafeNormal(), From);
			Say(*FString::Printf(TEXT("a Gatling hit %d"), B.Hits));
			return;
		}
		B.Step = 5;
		B.NextLog = B.T + 3.0;
		return;
	case 5:
		if (B.T < B.NextLog) return;
		Say(TEXT("after the Gatling's hits"));
		// BUG-BOBDEATH: Bob killed; the ranch ends on its cutscene, so no death of his shown
		UE_LOG(LogOutlaws, Log, TEXT("OLRT boss test: the end movie ready before the kill %d"), EndMovieReady() ? 1 : 0);
		for (int32 k = 0; Bob && k < 60 && !Bob->IsDead(); ++k) Bob->TakeHit(50.f, Bob->GetActorLocation(), FVector(-1.0, 0.0, 0.0), P->GetActorLocation());
		Say(TEXT("Bob shot dead"));
		B.Step = 6;
		B.NextLog = B.T + 0.2;
		return;
	case 6:
		if (B.T < B.NextLog) return;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT boss test: level end timer %.2f, movie %s, end movie ready %d"), LevelEndTimer, *NextMovie, EndMovieReady() ? 1 : 0);
		Say(TEXT("0.2 s after the kill"));
		B.Step = 7;
		B.NextLog = B.T + 2.0;
		return;
	case 7:
		if (B.T < B.NextLog) return;
		Say(TEXT("done"));
		B.Step = 8;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

void AOutlawsLevelActor::TickBossTauntTest(float Dt)
{
	// -OLLevel=townex -OLNoFrontEnd -OLBossTauntTest (R12): bad guys killed by script to 51, 65 and 79 % of them, two taunts
	// heard at each (their distance falling, their volume rising, the bearing drifting), then past 80 %: the boss appears
	static double T = 0.0, StageAt = 0.0;
	static int32 Stage = 0, TauntsAtStage = 0;
	UWorld* World = GetWorld();
	AOutlawsPlayer* P = World && World->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(World->GetFirstPlayerController()->GetPawn()) : nullptr;
	if (!P || bNeedPlace || bFrontEnd) return;
	T += Dt;
	if (T < 3.0) return;
	bInvincible = true;
	static const float Targets[] = { 0.51f, 0.65f, 0.79f, 0.81f };
	auto KillTo = [&](float Frac)
	{
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : Enemies)
		{
			if (float(Kills) >= Frac * float(TotalBadGuys)) break;
			AOutlawsEnemy* E = Wk.Get();
			if (!E || E->IsDead() || E->bBoss || E->IsDormant()) continue;
			for (int32 k = 0; k < 60 && !E->IsDead(); ++k) E->TakeHit(50.f, E->GetActorLocation(), FVector(0.0, 0.0, -1.0), E->GetActorLocation() + FVector(0.0, 0.0, 200.0));
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLRT boss taunt test: killed to %d of %d (%.0f %%)"), Kills, TotalBadGuys, 100.f * Kills / FMath::Max(1, TotalBadGuys));
	};
	if (Stage < 4 && (StageAt == 0.0 || (Stage < 3 && BossTauntsMade - TauntsAtStage >= 2) || T - StageAt > 45.0))
	{
		KillTo(Targets[Stage]);
		StageAt = T;
		TauntsAtStage = BossTauntsMade;
		++Stage;
		return;
	}
	if (Stage == 4 && T - StageAt > 6.0)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT boss taunt test: done (%d taunts)"), BossTauntsMade);
		Stage = 5;
		FGenericPlatformMisc::RequestExit(false);
	}
}

// -OLLevel=ranch -OLNoFrontEnd -OLRanchDoorTest (user 2026-10-12, BUG-RANCHDOOR): the key given and BOBDOOR pushed by script;
// at once the fade to the door's movie must be on and the door held (not moving); each frame until the movie covers the level
// the door must not have moved, nor ever move while the level shows; 3 s into the movie it is skipped; after it the door must
// be open and Bob awake.  One OLSHOT line, then quit
void AOutlawsLevelActor::TickRanchDoorTest(float Dt)
{
	static double T = 0.0, Mark = 0.0, Start = 0.0, Before = 0.0, LastValue = 0.0;
	static int32 Step = 0, Door = -1, VisibleMoves = 0, MovedFrames = 0;
	static bool bAtOnce = false, bHeldAtOnce = false, bStillAtOnce = false;
	UWorld* World = GetWorld();
	const APawn* P = World && World->GetFirstPlayerController() ? World->GetFirstPlayerController()->GetPawn() : nullptr;
	if (!P || bNeedPlace || bFrontEnd || !Inf || Step > 4) return;
	T += Dt;
	AOutlawsEnemy* Bob = nullptr;
	for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : Enemies) if (Wk.IsValid() && Wk->bPeekShooter) Bob = Wk.Get();
	const FOutlawsElevator* E = Inf->Elevators.IsValidIndex(Door) ? &Inf->Elevators[Door] : nullptr;
	if (E && Step >= 1 && Step <= 3)
	{
		if (MidMovieShown == 0) Before = FMath::Max(Before, FMath::Abs(E->Current - Start));
		if (FMath::Abs(E->Current - LastValue) > 1e-6)
		{
			++MovedFrames;
			if (MidMovieHidden.Num() == 0) ++VisibleMoves;
		}
		LastValue = E->Current;
	}
	switch (Step)
	{
	case 0:
	{
		if (T < 4.0) return;
		bInvincible = true;
		for (int32 i = 0; i < Inf->Elevators.Num(); ++i) if (Inf->Elevators[i].Name.Equals(TEXT("BOBDOOR"), ESearchCase::IgnoreCase) && Inf->Elevators[i].Sectors.Num() > 0) Door = i;
		if (Door < 0)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT ranchdoor %s: no BOBDOOR here: FAIL"), *LevelName);
			Step = 9;
			FGenericPlatformMisc::RequestExit(false);
			return;
		}
		E = &Inf->Elevators[Door];
		Start = LastValue = E->Current;
		GiveItem(101);
		const bool bTaken = PushDoorSector(E->Sectors[0]);
		// at once, in the use's own frame: the fade to the movie begun, the door held shut with its move waiting
		bAtOnce = bMidMovieOn;
		bHeldAtOnce = Inf->GetHeldStarts().Contains(Door);
		bStillAtOnce = E->Target < 0 && FMath::Abs(E->Current - Start) < 1e-6;
		UE_LOG(LogOutlaws, Display, TEXT("OLRT ranch door test: BOBDOOR pushed with key 101: taken %d; at once: the movie's fade on %d, the door's move held %d, the door still %d (at %.1f); Bob behind his door %d"),
			bTaken ? 1 : 0, bAtOnce ? 1 : 0, bHeldAtOnce ? 1 : 0, bStillAtOnce ? 1 : 0, E->Current, Bob && Bob->bBehindDoor ? 1 : 0);
		Mark = T;
		Step = 1;
		return;
	}
	case 1:
		// the movie up over the level (the world hidden)
		if (MidMovieShown == 0 && T - Mark < 10.0) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLRT ranch door test: the movie covers the level %.2f s after the use (shown %d); the door had moved %.2f before"), T - Mark, MidMovieShown, Before);
		Mark = T;
		Step = 2;
		return;
	case 2:
		if (T - Mark < 3.0) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLRT ranch door test: 3 s into the movie: the door at %.1f (stop %d, moving %d), playing %d"), E ? E->Current : -1.0, E ? E->Stop : -1, E ? E->Target : -2, IsMoviePlaying() ? 1 : 0);
		if (IsMoviePlaying()) SkipMovie();
		Mark = T;
		Step = 3;
		return;
	case 3:
	{
		if (bMidMovieOn && T - Mark < 10.0) return;
		if (T - Mark < 2.0) return;
		const bool bOpen = E && E->Target < 0 && E->Stop == 1 && FMath::Abs(E->Current - Inf->StopValue(*E, 1)) < 0.01;
		const bool bAwake = Bob && !Bob->bBehindDoor;
		const bool bOk = bAtOnce && bHeldAtOnce && bStillAtOnce && MidMovieShown == 1 && Before < 0.01 && MidMovieDoorAtShow < 0.01 && VisibleMoves == 0 && MovedFrames > 0
			&& !bMidMovieOn && bOpen && bAwake;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT ranchdoor %s: the movie's fade on at the use %d, the door held at the use %d and still %d, the movie shown %d, the door moved %.2f before it (%.2f when let go), %d of %d frames of its move with the level showing (expect 0), open after %d (at %.1f), Bob awake %d: %s"),
			*LevelName, bAtOnce ? 1 : 0, bHeldAtOnce ? 1 : 0, bStillAtOnce ? 1 : 0, MidMovieShown, Before, MidMovieDoorAtShow, VisibleMoves, MovedFrames, bOpen ? 1 : 0, E ? E->Current : -1.0,
			bAwake ? 1 : 0, bOk ? TEXT("PASS") : TEXT("FAIL"));
		Step = 9;
		FGenericPlatformMisc::RequestExit(false);
		return;
	}
	default:
		return;
	}
}

// -OLLevel=<a story level> -OLNoFrontEnd -OLThrottleBossTest (2026-10-06): the enemy throttle held at its tightest step
// (the Quest 2 caps at x0.5, no budget easing), the named outlaws woken and summoned and watched for 6 s (each must stay
// FULL: worst tier 0), then shot down; 3 s on, the level must be ending (where an outlaw's death ends it: not the train's);
// one OLSHOT line, then quit
void AOutlawsLevelActor::TickThrottleBossTest(float Dt)
{
	static double T = 0.0, Mark = 0.0;
	static int32 Step = 0, Worst = 0, Samples = 0, Dead = 0, Total = 0;
	static bool bEnded = false, bMovieSeen = false, bMovieDue = false;
	static FString Names;
	struct FFallen { TWeakObjectPtr<AOutlawsEnemy> E; FString Name; int32 DieChors = 0, HeldFrames = 0, BadFrames = 0, FromChor = -1, ShowChor = -1; bool bHeld = false; };
	static TArray<FFallen> Fallen;
	UWorld* World = GetWorld();
	const APawn* P = World && World->GetFirstPlayerController() ? World->GetFirstPlayerController()->GetPawn() : nullptr;
	if (!P || bNeedPlace || bFrontEnd || Step > 3) return;
	T += Dt;
	TArray<AOutlawsEnemy*, TInlineAllocator<8>> Named;      // (Step 3: -OLBossKillHow's killing blow, 0.15 s after the first hurt)
	for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : Enemies) if (Wk.IsValid() && Wk->bBoss) Named.Add(Wk.Get());
	if (Step == 0)
	{
		if (T < 3.0) return;
		auto SetCVar = [](const TCHAR* Name, int32 V) { if (IConsoleVariable* C = IConsoleManager::Get().FindConsoleVariable(Name)) C->Set(V, ECVF_SetByCode); };
		SetCVar(TEXT("ol.throttle.Enable"), 1);
		SetCVar(TEXT("ol.throttle.Profile"), 2);
		SetCVar(TEXT("ol.throttle.ForceLevel"), 3);
		bInvincible = true;
		WakeCurtainBoss(TEXT("throttle boss test"));
		DebugSummonBoss();
		UE_LOG(LogOutlaws, Display, TEXT("OLRT throttle boss test (%s): %d named outlaws, %d bad guys; the throttle forced to Quest 2 at step 3"), *LevelName, Named.Num(), Enemies.Num());
		Mark = T;
		Step = 1;
		return;
	}
	if (Step == 1)
	{
		for (const AOutlawsEnemy* E : Named) if (!E->IsDead()) { Worst = FMath::Max(Worst, int32(E->GetThrottleTier())); ++Samples; }
		if (T - Mark < 6.0) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLRT throttle boss test (%s): %s"), *LevelName, *ThrottleSummary());
		// -OLBossKillHow=shot (default: one burst from point blank, the first shot kills) | hurt (a shot from afar that
		// does not kill, then the killing shots 0.15 s on, inside the stun, as in play) | pellets (hurt, then a shotgun's
		// ten pellets in one frame) | blast (hurt, then dynamite at his feet) | melee (hurt, then fists or a thrown knife)
		FString How = TEXT("shot");
		OLFlags::Value(TEXT("OLBossKillHow"), How);
		if (How != TEXT("shot"))
		{
			for (AOutlawsEnemy* E : Named)
			{
				const FVector Far = E->GetActorLocation() - FVector(2000.0, 0.0, 0.0);
				E->TakeHit(FMath::Max(0.2f, E->GetHealth() * 0.3f), E->GetActorLocation(), FVector(1.0, 0.0, 0.0), Far);
				UE_LOG(LogOutlaws, Display, TEXT("OLRT boss kill test (%s): %s hurt first, health %.2f, chor on show %d"), *How, *E->GetOutlawsName(), E->GetHealth(),
					E->GetSprite() ? E->GetSprite()->GetPlayingChor() : -1);
			}
			Mark = T;
			Step = 3;
			return;
		}
		for (AOutlawsEnemy* E : Named)
		{
			for (int32 i = 0; i < 40 && !E->IsDead(); ++i) E->TakeHit(4.f, E->GetActorLocation(), FVector(1.0, 0.0, 0.0), E->GetActorLocation() - FVector(100.0, 0.0, 0.0));
			Dead += E->IsDead() ? 1 : 0;
			Names += (Names.IsEmpty() ? TEXT("") : TEXT(" ")) + E->GetOutlawsName();
		}
		Total = Named.Num();
		Mark = T;
		Step = 2;
		for (AOutlawsEnemy* E : Named) Fallen.Add({ E, E->GetOutlawsName() });
		return;
	}
	if (Step == 3)
	{
		if (T - Mark < 0.15) return;
		FString How = TEXT("hurt");
		OLFlags::Value(TEXT("OLBossKillHow"), How);
		for (AOutlawsEnemy* E : Named)
		{
			const FVector At = E->GetActorLocation();
			const FVector Near = At - FVector(300.0, 0.0, 0.0);
			UE_LOG(LogOutlaws, Display, TEXT("OLRT boss kill test (%s): %s killed with chor %d on show"), *How, *E->GetOutlawsName(), E->GetSprite() ? E->GetSprite()->GetPlayingChor() : -1);
			if (How == TEXT("blast"))
			{
				for (int32 i = 0; i < 8 && !E->IsDead(); ++i) Explode(E->GetActorLocation());
			}
			else if (How == TEXT("melee"))
			{
				for (int32 i = 0; i < 40 && !E->IsDead(); ++i) { AOutlawsEnemy::GNextHitMelee = true; E->TakeHit(3.f, At, FVector(1.0, 0.0, 0.0), Near); }
			}
			else if (How == TEXT("pellets"))
			{
				for (int32 v = 0; v < 6 && !E->IsDead(); ++v)
					for (int32 i = 0; i < 10; ++i) E->TakeHit(1.f, At + FVector(0.0, 10.0 * (i - 5), 20.0 * (i % 3) - 20.0), FVector(1.0, 0.0, 0.0), Near, false, 1.f);
			}
			else
			{
				for (int32 i = 0; i < 40 && !E->IsDead(); ++i) E->TakeHit(4.f, At, FVector(1.0, 0.0, 0.0), Near);
			}
			Dead += E->IsDead() ? 1 : 0;
			Names += (Names.IsEmpty() ? TEXT("") : TEXT(" ")) + E->GetOutlawsName();
		}
		Total = Named.Num();
		Mark = T;
		Step = 2;
		for (AOutlawsEnemy* E : Named) Fallen.Add({ E, E->GetOutlawsName() });
		return;
	}
	// (the level may end and move on within these 3 s: the outcome is kept as it comes)
	bEnded = bEnded || DebugLevelEnding() || IsMoviePlaying();
	bMovieSeen = bMovieSeen || Movie != nullptr;
	// (the train ends at its own trigger, the door behind Bloodworth, not on his death: no cutscene is due until the player walks through)
	bMovieDue = bMovieDue || (bBossDefeated && !bInfEndsLevel) || LevelEndTimer >= 0.f || IsMoviePlaying();
	// BUG-DRDEATHANIM: each named outlaw's death as it is shown, frame by frame, until the level is cleared for the movie
	for (FFallen& F : Fallen)
	{
		const AOutlawsEnemy* E = F.E.Get();
		if (!E || Movie) continue;
		F.DieChors = E->DieChorsPlayed;
		F.bHeld |= E->bHeldForCutscene;
		if (E->bHeldForCutscene)
		{
			++F.HeldFrames;
			const AOutlawsSpriteActor* S = E->GetSprite();
			if (E->IsHidden() || !S || S->IsHidden() || !S->IsClockHeld() || !E->IsLivePoseChor(S->GetPlayingChor())) ++F.BadFrames;
			F.FromChor = E->HeldFromChor;
			F.ShowChor = S ? S->GetPlayingChor() : -1;
		}
	}
	if (Step == 2 && T - Mark > 3.0)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT throttle boss test %s: named [%s] worst tier %d over %d looks (expect 0), dead %d of %d (expect all), level ended %d"),
			*LevelName, *Names, Worst, Samples, Dead, Total, bEnded ? 1 : 0);
		// the mine's Dr. Death and the ranch's Bob: no death played, the live frame held (seen, its clock stopped); every other
		// named outlaw: his death played; a level ended on its boss: its cutscene started
		bool bOk = true;
		FString Each;
		for (const FFallen& F : Fallen)
		{
			const bool bPair = (LevelName.Equals(TEXT("miner"), ESearchCase::IgnoreCase) && F.Name.Contains(TEXT("DRDEATH")))
				|| (LevelName.Equals(TEXT("ranch"), ESearchCase::IgnoreCase) && F.Name.Contains(TEXT("BOB")))
				|| (LevelName.Equals(TEXT("hideout"), ESearchCase::IgnoreCase) && F.Name.Contains(TEXT("SLIM")));
			const bool bThis = bPair ? (F.bHeld && F.DieChors == 0 && F.HeldFrames > 0 && F.BadFrames == 0) : (F.DieChors >= 1 && !F.bHeld);
			bOk &= bThis;
			Each += FString::Printf(TEXT("%s%s death chors %d held %d (%d frames, %d hidden, clock running or not a live pose; chor %d held, %d on show at the kill) expect %s %s"),
				Each.IsEmpty() ? TEXT("") : TEXT("; "), *F.Name, F.DieChors, F.bHeld ? 1 : 0, F.HeldFrames, F.BadFrames, F.ShowChor, F.FromChor,
				bPair ? TEXT("held, none played") : TEXT("played"), bThis ? TEXT("ok") : TEXT("BAD"));
		}
		bOk &= !bMovieDue || bMovieSeen;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT bossdeath %s: %s; level ended %d (%s), cutscene started %d: %s"), *LevelName, *Each, bEnded ? 1 : 0,
			bMovieDue ? TEXT("cutscene due") : TEXT("its own trigger ends it"), bMovieSeen ? 1 : 0, bOk ? TEXT("PASS") : TEXT("FAIL"));
		Step = 4;
		FGenericPlatformMisc::RequestExit(false);
	}
}
#endif // WITH_OUTLAWS_TESTS

void AOutlawsLevelActor::UpdateBoss(float Dt)
{
	OL_PERF_SCOPE("Level: boss");
#if WITH_OUTLAWS_TESTS
	static const bool bBossTest = OLFlags::Param(TEXT("OLBossTest"));
	if (bBossTest) TickBossTest(Dt);
	static const bool bBossTauntTest = OLFlags::Param(TEXT("OLBossTauntTest"));
	if (bBossTauntTest) TickBossTauntTest(Dt);
	static const bool bThrottleBossTest = OLFlags::Param(TEXT("OLThrottleBossTest"));
	if (bThrottleBossTest) TickThrottleBossTest(Dt);
	static const bool bRanchDoorTest = OLFlags::Param(TEXT("OLRanchDoorTest"));
	if (bRanchDoorTest) TickRanchDoorTest(Dt);
#endif
	if (LevelEndTimer >= 0.f)
	{
		LevelEndTimer -= Dt;
		if (LevelEndTimer < 0.f)
		{
			// the movie between (the level cleared away: the screen alone in the dark), then the next level
			UWorld* World = GetWorld();
			AOutlawsPlayer* P = World && World->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(World->GetFirstPlayerController()->GetPawn()) : nullptr;
			// the player's movies\<name>.webm converted to .mp4 (tools/olue/convert_movies.py): the engine's WebM
			// player crashes on them, Media Foundation plays H.264 well
			const FString File = NextMovie.IsEmpty() ? FString() : DataFile(TEXT("Movies"), NextMovie + TEXT(".mp4"));
			if (!File.IsEmpty() && !FPaths::FileExists(File))
			{
				UE_LOG(LogOutlaws, Log, TEXT("OLRT movie %s not converted yet (tools/olue/convert_movies.py): skipped"), *File);
			}
			if (World && P && !File.IsEmpty() && FPaths::FileExists(File) && PlayMovie(NextMovie, bCutToMovie))
			{
				UOutlawsStats::ShowOn(this, Movie);    // (user 2026-10-02: the players' counts beside the picture)
				return;
			}
			if (World && P && UOutlawsStats::HoldBetweenLevels(this)) return;     // (no cutscene: the counts on the black a few seconds)
			StartNextLevel();
		}
		return;
	}
	if (OLFlags::Param(TEXT("OLBossTrack")))
	{
		// where each named outlaw goes while the player stands at the start (user 2026-10-01: the canyon's was not at his
		// place at the end), every 10 s for 3 minutes
		static float TrackT = 0.f, TrackNext = 0.f;
		TrackT += Dt;
		if (TrackT >= TrackNext)
		{
			TrackNext += 10.f;
			const APawn* Pp = GetWorld() && GetWorld()->GetFirstPlayerController() ? GetWorld()->GetFirstPlayerController()->GetPawn() : nullptr;
			for (const FBossSlot& S : BossSlots)
			{
				const AOutlawsEnemy* SA = S.Actor.Get();
				if (!SA) continue;
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT boss track %.0f s: %s at %s (%.0f m from his place, %.0f m from the player)%s%s"), TrackT, *S.Name,
					*SA->GetActorLocation().ToString(), FVector::Dist(SA->GetActorLocation(), S.Spawns[0]) / 100.0,
					Pp ? FVector::Dist(SA->GetActorLocation(), Pp->GetActorLocation()) / 100.0 : -1.0, SA->IsDead() ? TEXT(" DEAD") : TEXT(""), SA->IsDormant() ? TEXT(" dormant") : TEXT(""));
			}
		}
		if (TrackT > 181.f) FGenericPlatformMisc::RequestExit(false);
	}
	AOutlawsEnemy* B = Boss.Get();
	bool bAnyDormant = false;
	for (const FBossSlot& S : BossSlots) bAnyDormant |= S.Actor.IsValid() && S.Actor->IsDormant() && S.Spawns.Num() > 0;
	if (!B || !bAnyDormant || bDead)
	{
		return;
	}
	// the exe looks every 2 s [EXE 0x14006ae20 message 118]
	BossTimer -= Dt;
	if (BossTimer > 0.f)
	{
		return;
	}
	BossTimer = 2.f;
	UWorld* World = GetWorld();
	APawn* Pawn = World && World->GetFirstPlayerController() ? World->GetFirstPlayerController()->GetPawn() : nullptr;
	if (!Pawn)
	{
		return;
	}
	const FVector PlayerLoc = Pawn->GetActorLocation();
	FVector Eye;
	FRotator EyeRot;
	Pawn->GetActorEyesViewPoint(Eye, EyeRot);
	UE_LOG(LogOutlaws, Verbose, TEXT("OLRT boss: total %d, kills %d"), TotalBadGuys, Kills);
	if (Kills > 0.8f * TotalBadGuys)
	{
		// each boss at the nearest of its places the player cannot see (octagonal distance), else the nearest.
		// BUG-BOSSIDLEHOST: a co-op host's boss by the active players (the nearest to any of them, unseen by all of them),
		// not by an idle host at the level's start (UOutlawsCoop::BossAnchors); single player as ever
		TArray<FVector> AnchorFeet, AnchorEyes;
		FString AnchorWho;
		if (!UOutlawsCoop::BossAnchors(this, AnchorFeet, AnchorEyes, AnchorWho))
		{
			AnchorFeet = { PlayerLoc };
			AnchorEyes = { Eye };
		}
		for (const FBossSlot& S : BossSlots)
		{
			AOutlawsEnemy* SB = S.Actor.Get();
			if (!SB || !SB->IsDormant() || S.Spawns.Num() == 0) continue;
			int32 Best = -1, BestUnseen = -1;
			double BestD = 1e30, BestUnseenD = 1e30;
			for (int32 i = 0; i < S.Spawns.Num(); ++i)
			{
				double Oct = 1e30;
				for (const FVector& At : AnchorFeet)
				{
					const FVector D = S.Spawns[i] - At;
					const double Ax = FMath::Abs(D.X), Ay = FMath::Abs(D.Y);
					Oct = FMath::Min(Oct, FMath::Max(Ax, Ay) + 0.5 * FMath::Min(Ax, Ay));
				}
				if (Oct < BestD) { BestD = Oct; Best = i; }
				if (Oct < BestUnseenD)
				{
					bool bSeenBy = false;
					for (const FVector& AE : AnchorEyes) bSeenBy = bSeenBy || CanSee(S.Spawns[i] + FVector(0.0, 0.0, 150.0), AE, Pawn);
					if (!bSeenBy)
					{
						BestUnseenD = Oct;
						BestUnseen = i;
					}
				}
			}
			const int32 Pick = BestUnseen >= 0 ? BestUnseen : Best;
			UE_LOG(LogOutlaws, Log, TEXT("OLRT boss %s appears at %s (%d kills of %d)%s"), *S.Name, *S.Spawns[Pick].ToString(), Kills, TotalBadGuys,
				AnchorWho.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", by %s"), *AnchorWho));
			SB->Appear(S.Spawns[Pick]);
		}
	}
	else if (Kills > 0.5f * TotalBadGuys)
	{
		// he is about: a taunt now and then [EXE 0x14006ae20: from somewhere 15..45 m off, a fresh bearing each time].  R12
		// (user 2026-10-03: "as you kill more and more people the taunts get louder and louder as if the boss is getting
		// closer and closer: start off sounding fifty metres away, and end up sounding five metres away, right before they
		// spawn"): from 50 % of the kills to 80 % (his coming) he closes in from 50 m to 5 m (geometric, so each step sounds
		// alike), calls more often, and from one bearing that drifts 20 to 50 degrees a time, as if circling in.  The sound
		// stands at most 25 m off (inside its attenuation and the far-sound cull); the rest of the distance is its volume and
		// a low-pass
		BossTauntWait -= 2.f;
		if (BossTauntWait <= 0.f)
		{
			const float P = FMath::Clamp((float(Kills) / float(FMath::Max(1, TotalBadGuys)) - 0.5f) / 0.3f, 0.f, 1.f);
			const float DistM = 50.f * FMath::Pow(0.1f, P);
			BossTauntWait = FMath::Lerp(30.f, 16.f, P) * FMath::FRandRange(0.7f, 1.3f);   // (BUG-TAUNTS: twice the time between; was 15 to 8 s)
			if (BossTauntBearing < 0.f) BossTauntBearing = FMath::FRandRange(0.f, 360.f);
			else BossTauntBearing = FMath::Fmod(BossTauntBearing + (FMath::RandBool() ? 1.f : -1.f) * FMath::FRandRange(20.f, 50.f) + 360.f, 360.f);
			const float PlaceM = FMath::Min(DistM, 25.f);
			const float Volume = FMath::Clamp(25.f / DistM, 0.12f, 1.f);
			// the low-pass: 1.5 kHz at 50 m, open (20 kHz) at 5 m, log-spaced between
			const float Open = 1.f - FMath::Loge(DistM / 5.f) / FMath::Loge(10.f);
			const float Cutoff = 1500.f * FMath::Pow(20000.f / 1500.f, FMath::Clamp(Open, 0.f, 1.f));
			UAudioComponent* A = B->ForcedTaunt(PlayerLoc + FRotator(0.0, BossTauntBearing, 0.0).Vector() * PlaceM * 100.0);
			if (A)
			{
				A->SetVolumeMultiplier(A->VolumeMultiplier * Volume);
				A->SetLowPassFilterEnabled(Cutoff < 19000.f);
				A->SetLowPassFilterFrequency(Cutoff);
			}
			++BossTauntsMade;
			UE_LOG(LogOutlaws, Log, TEXT("OLRT boss taunt: p %.2f, %.0f m (placed at %.0f m, volume x%.2f, low-pass %.0f Hz), bearing %.0f, next in %.0f s, %d of %d kills%s"),
				P, DistM, PlaceM, Volume, Cutoff, BossTauntBearing, BossTauntWait, Kills, TotalBadGuys, A ? TEXT("") : TEXT(" (no sound)"));
		}
	}
}

void AOutlawsLevelActor::GetLevelChoices(TArray<FString>& OutNames, TArray<FString>& OutLabels) const
{
	// the story first, in play order, with the remaster's names (loc_english.txt)
	static const TMap<FString, FString> StoryNames = {
		{ TEXT("hideout"), TEXT("The Hideout") }, { TEXT("townex"), TEXT("The Town") }, { TEXT("town"), TEXT("The Town") },
		{ TEXT("train"), TEXT("The Train") }, { TEXT("canyon"), TEXT("The Canyon") }, { TEXT("mill"), TEXT("The Sawmill") },
		{ TEXT("simms"), TEXT("Simm's Fort") }, { TEXT("miner"), TEXT("The Mine") }, { TEXT("cliff"), TEXT("The Cliff Dwellings") },
		{ TEXT("ranch"), TEXT("Bob's Ranch") } };
	FString Text;
	TSet<FString> Seen;
	if (AssetsRef && AssetsRef->GetText(TEXT("outlaws.rcs"), Text))
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			TArray<FString> T;
			Line.ParseIntoArrayWS(T);
			FString Name;
			if (T.Num() >= 2 && T[0].Equals(TEXT("LEVEL:"), ESearchCase::IgnoreCase)) Name = T[1].ToLower();
			else if (T.Num() >= 3 && T[0].Equals(TEXT("LEVELHD:"), ESearchCase::IgnoreCase)) { Name = T[2].ToLower(); Seen.Add(T[1].ToLower()); }
			if (Name.IsEmpty() || Seen.Contains(Name)) continue;
			Seen.Add(Name);
			OutNames.Add(Name);
			const FString* Label = StoryNames.Find(Name);
			OutLabels.Add(FString::Printf(TEXT("%d. %s"), OutNames.Num(), Label ? **Label : *Name));
		}
	}
	// then every other level in the archives (the multiplayer maps)
	TArray<FString> Lvts;
	if (AssetsRef) AssetsRef->ListByExtension(TEXT(".lvt"), Lvts);
	for (const FString& F : Lvts)
	{
		const FString Name = FPaths::GetBaseFilename(F).ToLower();
		if (Seen.Contains(Name)) continue;
		Seen.Add(Name);
		OutNames.Add(Name);
		OutLabels.Add(Name.ToUpper());
	}
}

void AOutlawsLevelActor::DebugDumpCarry(const TCHAR* When) const
{
	FString C;
	for (const TPair<FString, int32>& Kv : Counts) C += FString::Printf(TEXT(" %s=%d"), *Kv.Key, Kv.Value);
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT carry %s in %s: counts%s | wrist [%s] | gold %d armour %d"), When, *LevelName, *C, *FString::Join(Inventory, TEXT(", ")), Gold, Armour);
}

void AOutlawsLevelActor::JumpToLevel(const FString& Name, bool bCarry)
{
	if (Movie)
	{
		Movie->Destroy();
		Movie = nullptr;
	}
	LevelEndTimer = -1.f;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT level skip (cheat): %s%s"), *Name, bCarry ? TEXT(" (carrying the pockets)") : TEXT(""));
	UOutlawsStats::NoteJump(this);             // (REQ-LEVELTIMER: no best time for the level skipped to)
	if (bCarry && !Name.IsEmpty())
	{
		// (BUG-SKIPLEVELINV) the cheat's skip goes the way a won level does: guns, ammunition, medicine, gold and armour carried
		// into the chosen level (keys and puzzle pieces left behind, health full, the ranch fists only)
		NextLevelName = Name;
		StartNextLevelThen(nullptr);
		return;
	}
	EntryLevel.Reset();                        // a skip starts from the start kit
	LevelName = Name;
	RestartLevel();
}

FString AOutlawsLevelActor::CarrySignature() const
{
	TArray<FString> Keys;
	Counts.GetKeys(Keys);
	Keys.Sort();
	FString S;
	for (const FString& K : Keys) S += FString::Printf(TEXT("%s=%d "), *K, Counts[K]);
	return S + FString::Printf(TEXT("| gold %d armour %d"), Gold, Armour);
}

bool AOutlawsLevelActor::IsMoviePlaying() const
{
	return Movie && Movie->IsPlaying();
}

void AOutlawsLevelActor::SkipMovie()
{
	if (UOutlawsCoop::IsActive(this)) UOutlawsCoop::NoteMovieSkip(this);     // (co-op: it ends for both)
	if (Movie) Movie->Skip();
}

void AOutlawsLevelActor::PauseMovie(bool bPause, float Tone)
{
	if (!Movie) return;
	Movie->SetPaused(bPause);
	// the PC's post process tones the world, the screen with it; the headset has none: the picture tones itself
	const UWorld* W = GetWorld();
	Movie->SetTone(W && W->FeatureLevel <= ERHIFeatureLevel::ES3_1 ? Tone : 0.f);
}

bool AOutlawsLevelActor::CanSkipMovie() const
{
	return Movie && Movie->IsPlaying() && Movie->CanSkip();
}

void AOutlawsLevelActor::SetMovieSkipHold(float Held, float Need, int32 Buttons)
{
	if (Movie) Movie->SetSkipHold(Held, Need, Buttons);
}

bool AOutlawsLevelActor::PlayMovie(const FString& Name, bool bHardCut)
{
	UWorld* World = GetWorld();
	AOutlawsPlayer* P = World && World->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(World->GetFirstPlayerController()->GetPawn()) : nullptr;
	const FString File = Name.IsEmpty() ? FString() : DataFile(TEXT("Movies"), Name + TEXT(".mp4"));
	if (!World || !P || File.IsEmpty() || !FPaths::FileExists(File))
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT movie %s not converted (tools/olue/convert_movies.py): skipped"), *Name);
		return false;
	}
	Clear();
	P->GetCharacterMovement()->DisableMovement();
	P->GetCharacterMovement()->Velocity = FVector::ZeroVector;
	FActorSpawnParameters SP;
	SP.ObjectFlags |= RF_Transient;
	Movie = World->SpawnActor<AOutlawsMovieScreen>(AOutlawsMovieScreen::StaticClass(), FTransform::Identity, SP);
	if (!Movie)
	{
		return false;
	}
	Movie->OnFinished.AddUObject(this, &AOutlawsLevelActor::StartNextLevel);
	ArmEndCredits(Name);                       // (REQ-CREDITS: the story's last movie rolls the credits as James rides off)
	FadeCamera(bHardCut ? 0.f : 1.f, 0.f, bHardCut ? 0.f : 1.5f, false);   // the cinema fades in over 1.5 s (a hard cut: at once)
	const UCameraComponent* Cam = P->FindComponentByClass<UCameraComponent>();
	// (the volume first: a movie that cannot play finishes inside Play, and the screen goes with it)
	Movie->SetVolume(MovieVolume * MasterVolume);
	Movie->Play(File, Cam ? Cam->GetComponentLocation() : P->GetActorLocation(), Cam ? Cam->GetForwardVector() : P->GetActorForwardVector());
	SetActorTickEnabled(true);
	return true;
}

FString AOutlawsLevelActor::InfMidMovieFile() const
{
	if (!Inf) return FString();
	for (const FOutlawsInfItem& It : Inf->Inf.Items)
	{
		for (const FOutlawsInfClass& C : It.Classes)
		{
			for (const FOutlawsInfMessage& M : C.TriggerMessages) if (M.Type == EOutlawsInfMsg::Movie) return M.Target;
			for (const FOutlawsInfMessage& M : C.ClientMsg) if (M.Type == EOutlawsInfMsg::Movie) return M.Target;
		}
	}
	return FString();
}

bool AOutlawsLevelActor::BossFallsInCutscene(const FString& Level, const FString& Func)
{
	// (user 2026-10-12, BUG-DRDEATHANIM: these alone; every other boss keeps his death; 2026-10-14: the hideout's Slim too)
	return (Level.Equals(TEXT("miner"), ESearchCase::IgnoreCase) && Func.Equals(TEXT("drdeath_new"), ESearchCase::IgnoreCase))
		|| (Level.Equals(TEXT("ranch"), ESearchCase::IgnoreCase) && Func.Equals(TEXT("bob_new"), ESearchCase::IgnoreCase))
		|| (Level.Equals(TEXT("hideout"), ESearchCase::IgnoreCase) && Func.Equals(TEXT("slim_new"), ESearchCase::IgnoreCase));
}

void AOutlawsLevelActor::TriggerMidLevelMovie(const FString& File, bool bFromHost)
{
	if (bMidMovieDone || bFrontEnd) return;
	bMidMovieDone = true;
	const FString Base = FPaths::GetBaseFilename(File).ToLower();       // (ram.san: the converted RAM.mp4 / ram.mp4)
	UWorld* World = GetWorld();
	AOutlawsPlayer* P = World && World->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(World->GetFirstPlayerController()->GetPawn()) : nullptr;
	FString Path = DataFile(TEXT("Movies"), Base + TEXT(".mp4"));
	if (Path.IsEmpty() || !FPaths::FileExists(Path)) Path = DataFile(TEXT("Movies"), Base.ToUpper() + TEXT(".mp4"));
	if (!World || !P || Movie || Path.IsEmpty() || !FPaths::FileExists(Path))
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT mid-level movie %s: not converted (tools/olue/convert_movies.py) or not now: skipped"), *Base);
		WakeCurtainBoss(TEXT("its movie skipped"));
		return;
	}
	// the world held as it is (the bad guys still, the player too) while it plays over the level; the door that set it off
	// stays shut until the movie covers the level (user 2026-10-12, BUG-RANCHDOOR: it opened in view during the fade)
	const bool bHeld = Inf && Inf->HoldEventStarts();
	if (UOutlawsCoop::IsActive(this) && !bFromHost) UOutlawsCoop::NoteMidLevelMovie(this, Base);     // (the host's: its clients play it too)
	bMidMovieOn = true;
	bMidMovieEnemiesWere = bEnemiesActive;
	bEnemiesActive = false;
	P->GetCharacterMovement()->DisableMovement();
	P->GetCharacterMovement()->Velocity = FVector::ZeroVector;
	// (user 2026-10-03, BUG-BOBCUT: "go to the black cinema. Kind of like at a level end. But just keep the level in memory
	// in the background ... the usual quick fade out. And in.") a quick fade to black (as a level's end, 0.375 s), then the
	// level hidden, not unloaded, and the movie on the black
	MidMoviePath = Path;
	FadeCamera(0.f, 1.f, 0.375f, true);
	FTimerHandle Th;
	World->GetTimerManager().SetTimer(Th, FTimerDelegate::CreateUObject(this, &AOutlawsLevelActor::StartMidMovieNow), 0.4f, false);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT mid-level movie %s: fading to the black cinema (%s)%s%s"), *Base, *Path, bHeld ? TEXT("; the doors this use opens wait shut for it") : TEXT(""),
		bFromHost ? TEXT(" (the host's word)") : TEXT(""));
}

void AOutlawsLevelActor::ReleaseMidMovieDoors(const TCHAR* Why)
{
	if (!Inf || Inf->GetHeldStarts().Num() == 0) return;
	// (tests: how far a held door had gone from its stop by now: nothing)
	double Moved = 0.0;
	FString Names;
	for (const int32 e : Inf->GetHeldStarts())
	{
		if (!Inf->Elevators.IsValidIndex(e)) continue;
		const FOutlawsElevator& E = Inf->Elevators[e];
		Moved = FMath::Max(Moved, FMath::Abs(E.Current - Inf->StopValue(E, E.Stop)));
		Names += (Names.IsEmpty() ? TEXT("") : TEXT(" ")) + E.Name;
	}
	MidMovieDoorAtShow = Moved;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT mid-level movie: the held doors [%s] start now (%s), moved %.2f from their stop before"), *Names, Why, Moved);
	Inf->ReleaseHeld();
}

void AOutlawsLevelActor::StartMidMovieNow()
{
	UWorld* World = GetWorld();
	AOutlawsPlayer* P = World && World->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(World->GetFirstPlayerController()->GetPawn()) : nullptr;
	if (!bMidMovieOn || !World || !P) return;
	FActorSpawnParameters SP;
	SP.ObjectFlags |= RF_Transient;
	Movie = World->SpawnActor<AOutlawsMovieScreen>(AOutlawsMovieScreen::StaticClass(), FTransform::Identity, SP);
	if (!Movie)
	{
		OnMidLevelMovieFinished();
		return;
	}
	HideWorldForMovie(true);
	DuckLevelSound(true);
	++MidMovieShown;
	// the held door opens behind the movie: a second on (a co-op client's own movie comes up a little after the host's; the
	// client's door moves on the host's word), or as the movie ends if that is sooner
	FTimerHandle Rh;
	World->GetTimerManager().SetTimer(Rh, FTimerDelegate::CreateWeakLambda(this, [this]() { ReleaseMidMovieDoors(TEXT("behind the movie")); }), 1.f, false);
	Movie->OnFinished.AddUObject(this, &AOutlawsLevelActor::OnMidLevelMovieFinished);
	FadeCamera(1.f, 0.f, 1.f, false);
	const UCameraComponent* Cam = P->FindComponentByClass<UCameraComponent>();
	Movie->SetVolume(MovieVolume * MasterVolume);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT mid-level movie playing in the black cinema, %d of the level's actors hidden (%s)"), MidMovieHidden.Num(), *MidMoviePath);
	Movie->Play(MidMoviePath, Cam ? Cam->GetComponentLocation() : P->GetActorLocation(), Cam ? Cam->GetForwardVector() : P->GetActorForwardVector());
}

void AOutlawsLevelActor::HideWorldForMovie(bool bHide)
{
	UWorld* World = GetWorld();
	if (!World) return;
	if (!bHide)
	{
		for (const TWeakObjectPtr<AActor>& A : MidMovieHidden) if (A.IsValid()) A->SetActorHiddenInGame(false);
		MidMovieHidden.Reset();
		return;
	}
	// every actor the level shows (its own meshes, sprites, bad guys, pickups, the sky), not the player and what he
	// carries, nor the screen itself
	const APlayerController* PC = World->GetFirstPlayerController();
	const APawn* Pawn = PC ? PC->GetPawn() : nullptr;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		if (!A || A->IsHidden() || A == Pawn || A == PC || A == Movie || (PC && A == PC->PlayerCameraManager) || A->IsA<AInfo>()) continue;
		if (Pawn && (A->GetOwner() == Pawn || A->GetAttachParentActor() == Pawn)) continue;
		if (A->IsA<AOutlawsMovieScreen>()) continue;
		A->SetActorHiddenInGame(true);
		MidMovieHidden.Add(A);
	}
}

void AOutlawsLevelActor::DuckLevelSound(bool bOn)
{
	UWorld* World = GetWorld();
	if (!World) return;
	const float Duck = 1.f / 3.f;
	if (bOn)
	{
		if (LevelSoundDuck < 1.f) return;
		LevelSoundDuck = Duck;
		for (TObjectIterator<UAudioComponent> It; It; ++It)
		{
			UAudioComponent* C = *It;
			if (!C || C->GetWorld() != World || !C->IsPlaying() || (Movie && C->GetOwner() == Movie)) continue;
			C->SetVolumeMultiplier(C->VolumeMultiplier * Duck);
			DuckedSounds.AddUnique(C);
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLRT mid-level movie: the level's sound at x%.2f (%d sounds playing turned down; the music and new sounds too)"), LevelSoundDuck, DuckedSounds.Num());
		return;
	}
	if (LevelSoundDuck >= 1.f) return;
	int32 Back = 0;
	for (const TWeakObjectPtr<UAudioComponent>& Wk : DuckedSounds)
	{
		if (UAudioComponent* C = Wk.Get())
		{
			C->SetVolumeMultiplier(C->VolumeMultiplier / LevelSoundDuck);
			++Back;
		}
	}
	DuckedSounds.Reset();
	LevelSoundDuck = 1.f;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT mid-level movie over: the level's sound back at x%.2f (%d sounds turned up)"), LevelSoundDuck, Back);
}

bool AOutlawsLevelActor::EndMovieReady() const
{
	// (ending now: its movie; not yet (the boss falling this moment): the one it will end on)
	FString Mv = NextMovie;
	if (LevelEndTimer < 0.f) StoryNext(Mv);
	if (Mv.IsEmpty() || bFrontEnd) return false;
	const FString File = DataFile(TEXT("Movies"), Mv + TEXT(".mp4"));
	return !File.IsEmpty() && FPaths::FileExists(File);
}

void AOutlawsLevelActor::OnMidLevelMovieFinished()
{
	if (!bMidMovieOn) return;
	bMidMovieOn = false;
	ReleaseMidMovieDoors(TEXT("the movie over before the second, or not up"));
	if (Movie)
	{
		Movie->OnFinished.RemoveAll(this);
		Movie->Destroy();
		Movie = nullptr;
	}
	bEnemiesActive = bMidMovieEnemiesWere;
	HideWorldForMovie(false);
	DuckLevelSound(false);
	UWorld* World = GetWorld();
	if (AOutlawsPlayer* P = World && World->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(World->GetFirstPlayerController()->GetPawn()) : nullptr)
	{
		P->GetCharacterMovement()->SetMovementMode(MOVE_Walking);
	}
	FadeCamera(1.f, 0.f, 0.375f, false);          // (back to the level: the quick fade in)
	UE_LOG(LogOutlaws, Log, TEXT("OLRT mid-level movie over: the level goes on"));
	WakeCurtainBoss(TEXT("after the door's movie"));
}

void AOutlawsLevelActor::WakeCurtainBoss(const TCHAR* Why)
{
	UWorld* World = GetWorld();
	if (!World) return;
	for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : Enemies)
	{
		AOutlawsEnemy* E = Wk.Get();
		if (!E || !E->bPeekShooter || E->IsDead()) continue;
		// the cloth in front of him: the faces of the level's hidden shot-and-sight blocker (a see-through wall the player
		// walks through; one mesh for the whole level) within 2.5 m of him are taken out of it, so he sees the player and is
		// seen and hit when he leans out; every other such wall stays as it was
		int32 Opened = 0;
		const FVector At = E->GetActorLocation();
		for (const TObjectPtr<UProceduralMeshComponent>& C : ObjectMeshes)
		{
			UProceduralMeshComponent* B = C.Get();
			if (!B || !B->GetName().StartsWith(TEXT("OutlawsBlockSHOT"))) continue;
			const FProcMeshSection* Sec = B->GetProcMeshSection(0);
			if (!Sec) continue;
			TArray<FVector> Pos, Nrm;
			TArray<FVector2D> UV;
			TArray<FLinearColor> Col;
			for (const FProcMeshVertex& V : Sec->ProcVertexBuffer)
			{
				Pos.Add(V.Position);
				Nrm.Add(V.Normal);
				UV.Add(V.UV0);
				Col.Add(FLinearColor(V.Color));
			}
			TArray<int32> Tris;
			const FTransform Xf = B->GetComponentTransform();
			for (int32 i = 0; i + 2 < Sec->ProcIndexBuffer.Num(); i += 3)
			{
				const uint32 A = Sec->ProcIndexBuffer[i], Bv = Sec->ProcIndexBuffer[i + 1], Cv = Sec->ProcIndexBuffer[i + 2];
				const FVector Mid = Xf.TransformPosition((Pos[A] + Pos[Bv] + Pos[Cv]) / 3.0);
				if (FVector::DistSquared2D(Mid, At) < FMath::Square(250.0) && FMath::Abs(Mid.Z - At.Z) < 300.0)
				{
					++Opened;
					continue;
				}
				Tris.Add(int32(A));
				Tris.Add(int32(Bv));
				Tris.Add(int32(Cv));
			}
			if (Opened > 0) B->CreateMeshSection_LinearColor(0, Pos, Tris, Nrm, UV, Col, TArray<FProcMeshTangent>(), true);
		}
		E->bBehindDoor = false;
		E->Wake(false);
		UE_LOG(LogOutlaws, Log, TEXT("OLRT %s knows the player is here (%s): alerted, %d curtain faces taken out of the shot blocker"), *E->GetOutlawsName(), Why, Opened);
	}
}

bool AOutlawsLevelActor::PickBackdropLevel(FString& Out) const
{
	// the story's levels and the historical missions this install has (each has bad guys to stand in for); not the one
	// shown last time (kept beside the saves, so two starts running differ too)
	static const TCHAR* Pool[] = { TEXT("hideout"), TEXT("townex"), TEXT("train"), TEXT("canyon"), TEXT("mill"), TEXT("simms"), TEXT("miner"), TEXT("cliff"),
		TEXT("ranch"), TEXT("shfort"), TEXT("shgranry"), TEXT("shgulch"), TEXT("shsimms"), TEXT("shwildns") };
	if (!AssetsRef) return false;
	const FString LastFile = FPaths::GetPath(SavePath(-1)) / TEXT("menu_level.txt");
	FString Last;
	FFileHelper::LoadFileToString(Last, *LastFile);
	Last = Last.TrimStartAndEnd().ToLower();
	FString Forced;                              // -OLMenuLevel=<name> (tests)
	OLFlags::Value(TEXT("OLMenuLevel"), Forced);
	TArray<FString> Have;
	for (const TCHAR* N : Pool)
	{
		if (AssetsRef->Has(FString(N) + TEXT(".lvt"))) Have.Add(N);
	}
	if (!Forced.IsEmpty() && AssetsRef->Has(Forced.ToLower() + TEXT(".lvt"))) { Out = Forced.ToLower(); return true; }
	if (Have.Num() == 0) return false;
	if (Have.Num() > 1) Have.Remove(Last);
	FRandomStream Rng(int32(FDateTime::UtcNow().GetTicks() ^ (FPlatformTime::Cycles64() * 2654435761ull)));
	Out = Have[Rng.RandHelper(Have.Num())];
	if (!OutlawsPerfTest::IsOn()) FFileHelper::SaveStringToFile(Out, *LastFile);      // (REQ-PERFTESTMODE: back from a run, nothing written)
	return true;
}

bool AOutlawsLevelActor::SpotInSectors(const FVector& Feet, float Yaw) const
{
	if (!LevelData) return true;
	const FOutlawsLevel& L = *LevelData;
	const double U = FOutlawsGeometry::UnitsToCm;
	auto Fits = [&](const FVector& P, double Room, const TCHAR* What)
	{
		// (the level's own plan, every sector: the culling boxes are not made yet when the menu's place is chosen) any
		// sector there whose floor is a step from the feet and whose ceiling leaves the room
		const FVector2D Pt = FOutlawsGeometry::ToOutlawsPlan(P);
		int32 Si = -1;
		bool bOk = false;
		for (int32 i = 0; i < L.Sectors.Num() && !bOk; ++i)
		{
			const FOutlawsSector& S = L.Sectors[i];
			if (!L.ContainsXZ(S, Pt.X, Pt.Y)) continue;
			Si = i;
			const double FloorZ = L.SurfaceY(S, Pt, true) * U, CeilZ = L.SurfaceY(S, Pt, false) * U;
			bOk = FMath::Abs(FloorZ - Feet.Z) < 45.0 && CeilZ - FMath::Max(FloorZ, Feet.Z) > Room;
		}
		if (!bOk) UE_LOG(LogOutlaws, Log, TEXT("OLRT main menu place at %s refused: the %s at %s is outside a sector or without room (sector %d)"), *Feet.ToString(), What, *P.ToString(), Si);
		return bOk;
	};
	if (!Fits(Feet, 190.0, TEXT("feet"))) return false;
	for (int32 d = 0; d < 8; ++d)
	{
		if (!Fits(Feet + FRotator(0.0, Yaw + 45.0 * d, 0.0).Vector() * 55.0, 170.0, TEXT("arm's reach"))) return false;
	}
	// (the menu stands about 1.2 m ahead at the eyes)
	return Fits(Feet + FRotator(0.0, Yaw, 0.0).Vector() * 120.0, 170.0, TEXT("menu's place"));
}

void AOutlawsLevelActor::SetUpBackdrop()
{
	// the level is built (FinishRestart, this same call: no bad guy has ticked, no sector has been entered).  Never saved:
	StartSaveDelay = -1.f;
	AutosaveWait = 0.f;
	UWorld* World = GetWorld();
	const bool bBuilt = World && WorldMesh && bHavePlayerStart;
	bBackdrop = bBuilt;
	// where its bad guys were placed (not the chickens, the townsfolk or a voice alone), then every one of them out of
	// sight: the view is from where one stood, nobody in it
	TArray<int32> Spots;
	for (int32 i = 0; i < Enemies.Num(); ++i)
	{
		AOutlawsEnemy* E = Enemies[i].Get();
		if (!E) continue;
		if (EnemySpawns.IsValidIndex(i) && !E->bVoiceOnly && !E->bNotBadGuy && !E->IsCritter()) Spots.Add(i);
		E->SetDormant(true);
	}
	FRandomStream Rng(int32(FDateTime::UtcNow().GetTicks() ^ (FPlatformTime::Cycles64() * 40503ull)));
	int32 SpotSeed = 0;                          // -OLMenuSeed=<n> (tests): the same spot every run
	if (OLFlags::Value(TEXT("OLMenuSeed"), SpotSeed)) Rng.Initialize(SpotSeed);
	for (int32 i = Spots.Num() - 1; i > 0; --i) Spots.Swap(i, Rng.RandHelper(i + 1));
	int32 ForcedSpot = -1;                       // -OLMenuSpot=<bad guy place> (tests): that place tried first
	if (OLFlags::Value(TEXT("OLMenuSpot"), ForcedSpot) && Spots.Contains(ForcedSpot))
	{
		Spots.Remove(ForcedSpot);
		Spots.Insert(ForcedSpot, 0);
	}
	APawn* Pawn = World && World->GetFirstPlayerController() ? World->GetFirstPlayerController()->GetPawn() : nullptr;
	// the body as tall as it is now (the headset's: it follows the head), 87 if there is none yet
	const ACharacter* Body = Cast<ACharacter>(Pawn);
	const float Half = Body && Body->IsA<AOutlawsPlayer>() ? Body->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 87.f, Radius = 34.f;
	int32 Picked = -1;
	float Open = 0.f, BackdropWallAt = 3000.f;
	if (bBuilt)
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsBackdrop), false, Pawn);
		for (int32 k = 0; k < FMath::Min(24, Spots.Num()) && Picked < 0; ++k)
		{
			const FVector Feet = EnemySpawns[Spots[k]];
			const FVector Centre = Feet + FVector(0.0, 0.0, Half + 2.0);
			// room for the body (a shade slimmer: it hangs still, it need not walk), and not under water
			if (World->OverlapBlockingTestByChannel(Centre, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(Radius - 6.f, Half - 4.f), Q)) continue;
			float Surface = 0.f;
			if (WaterAt(Centre, Surface)) continue;
			// (nothing lying at the feet: its outline drew over the menu)
			if (Pickups.ContainsByPredicate([&Feet](const FPickup& Pk) { return Pk.Actor.IsValid() && FVector::DistSquared(Pk.Actor->GetActorLocation(), Feet) < FMath::Square(160.0); })) continue;
			// the back to the closest wall (user 2026-10-02): looked for all round at the waist and the eyes, the level's own
			// walls only (not a prop, not a slope or a stair); faced square off it, into the room
			const FVector Eye = Feet + FVector(0.0, 0.0, 150.0);
			auto Clear = [&](float Yaw)
			{
				FHitResult Hit;
				const FVector To = Eye + FRotator(0.0, Yaw, 0.0).Vector() * 3000.0;
				return World->LineTraceSingleByChannel(Hit, Eye, To, ECC_Visibility, Q) ? float(Hit.Distance) : 3000.f;
			};
			float WallAt = 3000.f;
			FVector WallNormal = FVector::ZeroVector;
			for (const double Up : { 100.0, 150.0 })
			{
				for (int32 d = 0; d < 36; ++d)
				{
					FHitResult Hit;
					const FVector From = Feet + FVector(0.0, 0.0, Up), Dir = FRotator(0.0, 10.0 * d, 0.0).Vector();
					if (!World->LineTraceSingleByChannel(Hit, From, From + Dir * 3000.0, ECC_Visibility, Q)) continue;
					if (Hit.GetComponent() != WorldMesh || FMath::Abs(Hit.ImpactNormal.Z) > 0.7 || Hit.Distance >= WallAt) continue;
					WallAt = float(Hit.Distance);
					WallNormal = FVector(Hit.ImpactNormal.X, Hit.ImpactNormal.Y, 0.0).GetSafeNormal();
					if (FVector::DotProduct(WallNormal, Dir) > 0.0) WallNormal = -WallNormal;   // (a two-sided face: off it, toward the spot)
				}
			}
			// (no wall in 30 m: the way the bad guy faced when that is open for 3 m, else the most open of twelve ways round)
			float Yaw = !WallNormal.IsNearlyZero() ? float(WallNormal.Rotation().Yaw) : Enemies[Spots[k]].IsValid() ? Enemies[Spots[k]]->GetHeading() : 0.f;
			Open = Clear(Yaw);
			if (WallNormal.IsNearlyZero() && Open < 300.f)
			{
				for (int32 d = 1; d < 12; ++d)
				{
					const float Y2 = Yaw + 30.f * d, O2 = Clear(Y2);
					if (O2 > Open) { Open = O2; Yaw = Y2; }
				}
			}
			if (Open < 150.f && k + 1 < FMath::Min(24, Spots.Num())) continue;          // (a cupboard: another spot)
			// (user 2026-10-03, BUG-SECTOR: "coordinates must be within walls and ceiling/floor of a sector") the feet, a
			// ring round them at arm's length and the menu's place ahead must each stand in a sector of the level as
			// authored, on its floor (a step either way) and under its ceiling with a head's room; an opening between
			// two sectors is room, a wall or a sector's outside is not
			if (!SpotInSectors(Feet, Yaw)) continue;                                 // (none fits: the level's own start)
			PlayerStartLocation = Centre;
			PlayerStartYaw = FRotator::NormalizeAxis(Yaw);
			Picked = Spots[k];
			BackdropWallAt = WallAt;
		}
	}
	else
	{
		// it would not build: the dark, as the menu was before (and no map of an earlier level to judge the place by)
		LevelData.Reset();
		PlayerStartLocation = FVector(0.0, 0.0, 120.0);
		PlayerStartYaw = 0.f;
	}
	if (bBuilt && Picked < 0)
	{
		// (no bad guy's place would do: the level's own start, stood on its floor; in play the body drops the last of it)
		FHitResult Floor;
		FCollisionQueryParams FQ(SCENE_QUERY_STAT(OutlawsBackdropFloor), false, Pawn);
		if (World->LineTraceSingleByChannel(Floor, PlayerStartLocation, PlayerStartLocation - FVector(0.0, 0.0, 300.0), ECC_Visibility, FQ))
		{
			PlayerStartLocation.Z = Floor.Location.Z + Half + 2.0;
		}
	}
	bHavePlayerStart = true;
	PlacePlayerTries = 0;
	bNeedPlace = !TryPlacePlayer();
	UE_LOG(LogOutlaws, Log, TEXT("OLRT main menu level %s: %s, %d bad guy places, %s (open %.0f cm, %s)"), *LevelName, bBuilt ? TEXT("built") : TEXT("NOT built (the dark instead)"), Spots.Num(),
		Picked >= 0 ? *FString::Printf(TEXT("the view from place %d at %s yaw %.0f"), Picked, *PlayerStartLocation.ToString(), PlayerStartYaw) : TEXT("the level's own start"), Open,
		BackdropWallAt < 3000.f ? *FString::Printf(TEXT("back to a wall %.0f cm behind"), BackdropWallAt) : TEXT("no wall within 30 m"));
	AOutlawsPlayer* P = World && World->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(World->GetFirstPlayerController()->GetPawn()) : nullptr;
	if (!bNeedPlace && P)
	{
		P->GetCharacterMovement()->DisableMovement();
		P->GetCharacterMovement()->Velocity = FVector::ZeroVector;
		P->Recenter(true);                 // (the headset's forward is the view's)
		bFrontPending = false;
		ShowFrontMenu();
	}
	else
	{
		bFrontPending = true;              // (the tick opens the menu once the player is there)
	}
}

void AOutlawsLevelActor::StartFrontEnd(bool bSkipIntro)
{
	NoteDeathSlot(-1);                 // (BUG-DEATHAUTOSAVE: a game from the title goes back to its own saves only)
	UWorld* World = GetWorld();
	// the LABs first: the level pick reads them, and the menu before any level needs them for its hover sounds (user
	// 2026-09-28: the main menu was silent)
	if (!AssetsRef)
	{
		const FString Root = FOutlawsAssets::FindInstallRoot(InstallPath);
		if (!Root.IsEmpty()) AssetsRef = FOutlawsAssets::GetShared(Root);
	}
	bFrontEnd = true;
	bFrontSkipIntro = bSkipIntro;
	bTutorial = false;                 // (REQ-TUTORIAL: the main menu ends it, however it was left)
	TutorialRun.Reset();
	bMovieToMenu = false;
	if (Movie)
	{
		Movie->Destroy();
		Movie = nullptr;
	}
	PendingLoad.Reset();               // (a load still waiting is not put onto the menu's level)
	RandoRun.bSet = false;             // (R10: the next game takes the page's randomizer afresh, or its save's)
	RandoPendingAttempt = -1;
	LevelEndTimer = -1.f;
	bBossDefeated = false;
	bHomBossDown = false;
	bDead = false;
	bLampOn = false;
	// a random level behind the menu (user 2026-10-02), built as any level is (the old one let go of first); the opening
	// movie's path, and an install with none of the levels, keep the dark
	FString Pick;
	// (a co-op lobby keeps the dark: each machine would build its own random level for nothing)
	if (bSkipIntro && World && World->IsGameWorld() && !OLFlags::Param(TEXT("OLDarkMenu")) && !UOutlawsCoop::IsActive(this) && PickBackdropLevel(Pick))
	{
		LevelName = Pick;
		bFrontPending = false;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT front end (menu) over %s"), *Pick);
		RestartLevelThen([this]() { SetUpBackdrop(); });
		SetActorTickEnabled(true);
		return;
	}
	Clear();
	// the player hangs in the dark until the flow places a level
	PlayerStartLocation = FVector(0.0, 0.0, 120.0);
	PlayerStartYaw = 0.f;
	bHavePlayerStart = true;
	PlacePlayerTries = 0;
	bNeedPlace = !TryPlacePlayer();
	bFrontPending = true;
	SetActorTickEnabled(true);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT front end%s"), bSkipIntro ? TEXT(" (menu)") : TEXT(" (opening movie first)"));
}

void AOutlawsLevelActor::ShowFrontMenu()
{
	bFrontEnd = true;
	bMovieToMenu = false;
	if (Movie)
	{
		Movie->Destroy();
		Movie = nullptr;
	}
	UWorld* World = GetWorld();
	AOutlawsPlayer* P = World && World->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(World->GetFirstPlayerController()->GetPawn()) : nullptr;
	FadeCamera(0.f, 0.f, 0.f, false);
	if (P)
	{
		if (P->IsMenuOpen()) P->CloseGameMenu();      // (a pause menu opened while the level was on its way: the main menu, not it)
		P->HoldMenuPistol();               // bare hands, the leading one pointing: no guns on the title screens
		P->OpenFrontMenu();
	}
	StartMenuMusic();
	UE_LOG(LogOutlaws, Log, TEXT("OLRT main menu"));
	OutlawsShipSmoke::NoteMenu(this);      // (REQ-SHIPLOG: -OLShipSmoke only)
}

void AOutlawsLevelActor::ChangeDifficulty(EOutlawsDifficulty To)
{
	if (To == Difficulty) return;
	const EOutlawsDifficulty Was = Difficulty;
	Difficulty = To;
	int32 N = 0;
	for (const TWeakObjectPtr<AOutlawsEnemy>& W : Enemies)
	{
		if (AOutlawsEnemy* E = W.Get()) { E->Rescale(To); ++N; }
	}
	// the autosave follows the choice (a death after this comes back at the new difficulty), its bad guys still the ones
	// its level was built with
	// (only the autosave of the level being played: one left by another game is not touched)
	// (never in co-op: the autosave there is single player's; REQ-PERFTESTMODE: nor in a performance test run, whose level the
	// player's own autosave may be of)
	const bool bSave = !UOutlawsCoop::IsActive(this) && !OutlawsPerfTest::IsOn() && ReadSaveInfo(-1).Level.Equals(LevelName, ESearchCase::IgnoreCase) && PatchSaveDifficulty(-1, To, Was);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT difficulty %s -> %s: %d bad guys rescaled (placed for %s), autosave %s"), OutlawsDifficulty::Name(Was), OutlawsDifficulty::Name(To), N,
		OutlawsDifficulty::Name(SpawnDifficulty), bSave ? TEXT("follows") : TEXT("untouched"));
}

void AOutlawsLevelActor::StartStory(EOutlawsDifficulty InDifficulty)
{
	// Main Game: the story's first movie (OUTLAWS.RCS: in_shb), then the hideout with the start kit
	Difficulty = InDifficulty;
	bHom = false;
	bTutorial = false;                     // (REQ-TUTORIAL)
	TutorialRun.Reset();
	bFrontEnd = false;
	bMovieToMenu = false;
	PlayTime = 0.0;
	Counts.Reset();
	Inventory.Reset();
	InventoryLabels.Reset();
	Gold = 0;
	Armour = 0;
	ArmourMax = 0;                         // (BUG-PLATECARRY: no plate at all, not the last game's size)
	Health = 10.f;
	BeginRandoRun(TEXT("new game"));       // (R10: the run's randomizer and seed, fixed from here)
	NextLevelName = TEXT("hideout");
	if (RandoOn() && RandoRun.Order > 0)
	{
		TArray<FString> Order;
		RandoLevelOrder(Order);
		if (Order.Num() > 0) NextLevelName = Order[0];      // (Shuffle all: the run may not start at the hideout)
	}
	NextMovie = TEXT("in_shb");
	LevelName = NextLevelName;             // (not the main menu's level, should anything restart before the hideout is built)
	EntryLevel.Reset();
	StopMusic();
	bMenuMusic = false;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT new game (difficulty %d)"), int32(InDifficulty));
	if (!PlayMovie(NextMovie, false)) StartNextLevel();
}

void AOutlawsLevelActor::StartMission(const FString& Level, EOutlawsDifficulty InDifficulty, TFunction<void()> Then)
{
	Difficulty = InDifficulty;
	bHom = false;
	bTutorial = false;                     // (REQ-TUTORIAL: StartTutorial sets it again after this)
	TutorialRun.Reset();
	bFrontEnd = false;
	bMovieToMenu = false;
	PlayTime = 0.0;
	Counts.Reset();
	Inventory.Reset();
	InventoryLabels.Reset();
	Gold = 0;
	Armour = 0;
	ArmourMax = 0;                         // (BUG-PLATECARRY: no plate at all, not the last game's size)
	Health = 10.f;
	LevelName = Level;
	EntryLevel.Reset();
	StopMusic();
	bMenuMusic = false;
	BeginRandoRun(TEXT("historical mission"));
	UE_LOG(LogOutlaws, Log, TEXT("OLRT historical mission %s (difficulty %d)"), *Level, int32(InDifficulty));
	RestartLevelThen(MoveTemp(Then));          // (RestartLevel is this with nothing after; a co-op start is told when it is built)
}

void AOutlawsLevelActor::ReturnToFrontEnd()
{
	UE_LOG(LogOutlaws, Log, TEXT("OLRT end game: back to the main menu"));
	StartFrontEnd(true);
}

void AOutlawsLevelActor::GetMissionChoices(TArray<FString>& OutNames, TArray<FString>& OutLabels)
{
	// the sheriff levels (shfort...), the original's historical missions
	static const TCHAR* Missions[][2] = {
		{ TEXT("shfort"), TEXT("The Fort") }, { TEXT("shgranry"), TEXT("The Granary") }, { TEXT("shgulch"), TEXT("Dry Gulch") },
		{ TEXT("shsimms"), TEXT("Simm's Fort") }, { TEXT("shwildns"), TEXT("The Wilderness") } };
	for (const auto& M : Missions)
	{
		OutNames.Add(M[0]);
		OutLabels.Add(M[1]);
	}
}

namespace
{
	// REQ-HMCAREER: the Handful of Missions as the remaster's olpatch4 holds them: each mission's script (STORY: its levels in
	// order), its first level and its name (Common.kpf m_local_26001..26004)
	struct FHandfulMission { const TCHAR* Rca; const TCHAR* First; const TCHAR* Name; };
	const FHandfulMission HandfulMissions[] = {
		{ TEXT("civilwar.rca"), TEXT("civlwar1"), TEXT("Civil War") }, { TEXT("icecaves.rca"), TEXT("icecaves"), TEXT("Ice Caves") },
		{ TEXT("villa.rca"), TEXT("villa"), TEXT("Villa") }, { TEXT("wharf.rca"), TEXT("wharf"), TEXT("Wharf Town") } };
}

bool AOutlawsLevelActor::IsCareerMission(const FString& Level)
{
	TArray<FString> Names, Labels;
	GetMissionChoices(Names, Labels);
	return Names.Contains(FPaths::GetBaseFilename(Level).ToLower());
}

void AOutlawsLevelActor::GetHandfulChoices(TArray<FString>& OutNames, TArray<FString>& OutLabels, const FOutlawsAssets* Assets)
{
	for (const FHandfulMission& M : HandfulMissions)
	{
		if (Assets && !Assets->Has(FString(M.First) + TEXT(".lvt"))) continue;      // (an install without the expansion)
		OutNames.Add(M.First);
		OutLabels.Add(M.Name);
	}
}

bool AOutlawsLevelActor::IsHandfulLevel(const FString& Level)
{
	const FString L = Level.ToLower();
	if (L == TEXT("civlwar2")) return true;
	for (const FHandfulMission& M : HandfulMissions) if (L == M.First) return true;
	return false;
}

void AOutlawsLevelActor::StartNextLevel()
{
	if (StartEndCreditsIfDue()) return;        // (REQ-CREDITS: the story's end with no roll yet: on the black first)
	if (UOutlawsCoop::IsActive(this) && UOutlawsCoop::HoldNextLevel(this))
	{
		return;                    // (co-op: the host starts the next level for both through the session)
	}
	StartNextLevelThen(nullptr);
}

void AOutlawsLevelActor::StartNextLevelThen(TFunction<void()> Then)
{
	if (bFrontEnd && !bMovieToMenu)
	{
		return;                    // (the main menu is up: no level follows it but by its own buttons)
	}
	if (bMovieToMenu)
	{
		// the opening movie is done: the main menu
		ShowFrontMenu();
		return;
	}
	if (PerfTestLeave())
	{
		return;                    // (REQ-PERFTESTMODE: a performance test run's level is the last: the main menu)
	}
	if (Movie)
	{
		Movie->Destroy();
		Movie = nullptr;
	}
	if (NextLevelName.IsEmpty())
	{
		// the credits are done: the title screen (user 2026-09-27); a story finish earned first, so the main menu opens on
		// its unlock pages (REQ-UNLOCKS)
		OutlawsUnlocks::EarnPending(this, TEXT("the ending"));
		ReturnToFrontEnd();
		return;
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT level won: on to %s"), *NextLevelName);
	// the marshal keeps what he carries (weapons, ammunition, medicine, gold, his plate); keys and
	// puzzle pieces belong to the level he leaves; health comes back full
	TMap<FString, int32> Carry;
	for (const TPair<FString, int32>& Kv : Counts)
	{
		const FItemDef* D = ItemDefs.Find(Kv.Key);
		const FString Class = D ? D->Class.ToLower() : FString();
		if (Class.Contains(TEXT("key")) || Class.Contains(TEXT("puzz"))) continue;
		Carry.Add(Kv.Key, Kv.Value);
	}
	const int32 CarryGold = Gold, CarryArmour = Armour;
	// and whatever is in his hands (user 2026-09-28), drawn again once the carried weapons are counted in below
	UWorld* World = GetWorld();
	AOutlawsPlayer* P = World && World->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(World->GetFirstPlayerController()->GetPawn()) : nullptr;
	if (P) P->StashHandsForNextLevel();
	LevelName = NextLevelName;
	TWeakObjectPtr<AOutlawsPlayer> WeakP = P;
	if (!Then)
	{
		// (BUG-RESPAWNCRASH) what a map load needs to start the next level in this rebuild's place
		NextRebuildKind = ERebuildKind::NextLevel;
		MapCarryCounts = Carry;
		MapCarryGold = CarryGold;
		MapCarryArmour = CarryArmour;
	}
	RestartLevelThen([this, Carry, CarryGold, CarryArmour, WeakP, Then = MoveTemp(Then)]() mutable
	{
		const bool bStripped = LevelName.Equals(TEXT("ranch"), ESearchCase::IgnoreCase);   // Bob's Ranch: fists only (ApplyStartKit)
		if (bStripped) Carry.Reset();
		for (const TPair<FString, int32>& Kv : Carry)
		{
			int32& N = Counts.FindOrAdd(Kv.Key);
			N = FMath::Max(N, Kv.Value);
		}
		Gold = FMath::Max(Gold, CarryGold);
		Armour = bStripped ? 0 : FMath::Max(Armour, CarryArmour);
		if (AOutlawsPlayer* Pl = WeakP.Get())
		{
			Pl->RestoreCarriedHands();
			Pl->TopUpForLevelStart();        // (BUG-LEVELCARRY: rested, the guns loaded from what is carried, before the entry kit is kept)
		}
		EntryLevel = LevelName;
		EntryCounts = Counts;
		EntryGold = Gold;
		EntryArmour = Armour;
		EntryArmourMax = Armour > 0 ? FMath::Max(ArmourMax, Armour) : 0;      // (BUG-PLATERESTART)
		UE_LOG(LogOutlaws, Log, TEXT("OLRT carried into %s: %d items, gold %d, armour %d of %d"), *LevelName, Carry.Num(), Gold, Armour, EntryArmourMax);
		if (Then) Then();
	});
}

void AOutlawsLevelActor::DebugSummonBoss()
{
	Kills = FMath::Max(Kills, TotalBadGuys);
	BossTimer = 0.f;
	SetActorTickEnabled(true);
}

void AOutlawsLevelActor::OnBossDefeated()
{
	if ((bDead && !UOutlawsCoop::IsActive(this)) || bBossDefeated)      // (co-op: the other player may have won it)
	{
		return;
	}
	if (bHom)
	{
		// (HM1, REQ-HMBOSSEND) a historical mission ends at the horse, gripped empty-handed; the outlaw's fate is settled as he
		// goes down, as the original's [EXE 0x14008b2e5]: punched or stabbed out captured, anything else killed.  Co-op: the
		// host's call, its word to every client (UOutlawsCoop::HomBossDown)
		const AOutlawsEnemy* B = HomBoss();
		if (!bHomBossDown && B && B->IsDead())
		{
			const int32 Outcome = B->bTakenAlive ? 1 : 2, NameId = HomBossNameId(B);
			if (UOutlawsCoop::HomBossDown(this, Outcome, NameId)) HomBossDown(Outcome, NameId);
		}
		return;
	}
	// [EXE 0x14008f300]: not when the player is dead; otherwise the level ends (RequestEndLevel) when an outlaw whose own
	// handler ends it goes down: Slim, Dr. Death, Bob, Sanchez, Two Feathers, Farmer, Simms... Mary, Bill Morgan
	// (Buckshot), Bloodworth and the historical copies run the plain bad-guy handler and end nothing [EXE handler table
	// 0x140085940: 0x14006a4c0] (user 2026-09-29: the mine ends with Doc, the ranch with Bob, whoever else is standing).
	// A level whose named outlaws all run the plain handler (the train) waits for all of them, then its own trigger
	static const TSet<FString> Plain = { TEXT("bloodeye_new"), TEXT("mary_new"), TEXT("buckshot_new"), TEXT("shbill_new"),
		TEXT("shblood_new"), TEXT("shchez_new"), TEXT("shmary_new") };
	int32 Enders = 0, EndersDown = 0, Left = 0;
	for (const FBossSlot& S : BossSlots)
	{
		if (!S.Actor.IsValid()) continue;
		Left += S.Actor->IsDead() ? 0 : 1;
		if (Plain.Contains(S.Func.ToLower())) continue;
		++Enders;
		EndersDown += S.Actor->IsDead() ? 1 : 0;
	}
	// REQ-EXTRAS: each named outlaw down so far (the first, and each his own)
	for (const FBossSlot& S : BossSlots) if (S.Actor.IsValid() && S.Actor->IsDead()) OutlawsAchievements::NoteBossDown(this, LevelName, S.Func);
	if (Enders > 0 ? EndersDown == 0 : Left > 0)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT a named outlaw is down; the level goes on (%d that end it still standing, %d in all)"), Enders - EndersDown, Left);
		return;
	}
	bBossDefeated = true;
	if (bInfEndsLevel)
	{
		// the level's own trigger ends it (the train: the door behind Bloodworth)
		UE_LOG(LogOutlaws, Log, TEXT("OLRT every boss is down: the level ends at its own trigger (INF END_LEVEL)"));
		return;
	}
	CompleteLevel(TEXT("boss defeated"));
}

void AOutlawsLevelActor::CompleteLevel(const FString& Why)
{
	if ((bDead && !UOutlawsCoop::IsActive(this)) || LevelEndTimer >= 0.f || bFrontEnd)          // (nothing is won behind the main menu; co-op: nor lost by one death)
	{
		return;
	}
	NextLevelName = StoryNext(NextMovie);
	if (OutlawsPerfTest::IsRunning())
	{
		// REQ-PERFTESTMODE: the run's level won: its capture ends at the exit; no cutscene, the counts on the black, then the
		// main menu (PerfTestLeave), no level after it
		OutlawsPerfTest::End(TEXT("level exit"));
		NextMovie.Reset();
	}
	if (bHom && !LevelName.Equals(TEXT("office"), ESearchCase::IgnoreCase)) HomMissionEnd();      // (HM1: back to the office)
	if (UOutlawsCoop::IsActive(this) && !UOutlawsCoop::OnLevelWon(this, Why))
	{
		return;                    // (co-op: the client's level ends on the host's word)
	}
	OutlawsUnlocks::NoteLevelWon(this);      // (REQ-UNLOCKS: the story's last level: earned when its cutscene ends)
	OutlawsAchievements::NoteLevelComplete(this, Health, NextLevelName);     // (REQ-EXTRAS: True Grit, a Handful mission's end)
	UOutlawsStats::NoteLevelEnd(this);       // (REQ-LEVELTIMER: the time settled against the best times when the end card is made)
	UE_LOG(LogOutlaws, Log, TEXT("OLRT level complete (%s): movie %s, next level %s"), *Why, *NextMovie, *NextLevelName);
	bPendingOpeningSave = bAutosave && !bHom && !OutlawsPerfTest::IsOn();   // (once the end timer is set below: the autosave becomes the next level's opening)
	// to black over 1.5 s from the last blow, no words, and the cinema fades in (user 2026-10-02: every level fades out to
	// its cutscene and in from it; 2026-09-27's hard cuts for the hideout, train, sawmill, mine and ranch are gone)
	bCutToMovie = false;
	if (bCutToMovie)
	{
		LevelEndTimer = 0.01f;
	}
	else
	{
		// slowly (1.5 s) only where the level ends on a moment held: the town, the canyon, Simms and the cliffs; every other
		// level's cutscene follows straight on from the play, four times quicker (user 2026-10-02)
		const bool bSlow = LevelName == TEXT("townex") || LevelName == TEXT("canyon") || LevelName == TEXT("simms") || LevelName == TEXT("cliff");
		const float EndFade = bSlow ? 1.5f : 0.375f;
		FadeCamera(0.f, 1.f, EndFade, true);
		LevelEndTimer = EndFade;
	}
	if (bPendingOpeningSave)
	{
		bPendingOpeningSave = false;
		SaveLevelOpening();
	}
	SetActorTickEnabled(true);
}

bool AOutlawsLevelActor::BuildItemModel(const FString& File, TMap<FString, FOutlawsMeshSection>& OutSections, TMap<FString, UTexture2D*>& OutTextures)
{
	FString Text;
	OutlawsFormats::F3doModel M;
	if (!AssetsRef || !AssetsRef->GetText(File, Text) || !OutlawsFormats::Parse3do(Text, M))
	{
		return false;
	}
	OutlawsTextures::Build3doSections(*AssetsRef, M, 31, bHDTextures, SpritePalette, bReverseWinding, Textures, OutSections);
	for (const TPair<FString, FOutlawsMeshSection>& Kv : OutSections)
	{
		if (const TObjectPtr<UTexture2D>* T = Textures.Find(Kv.Value.Texture)) OutTextures.Add(Kv.Value.Texture, *T);
	}
	return OutSections.Num() > 0;
}

FString AOutlawsLevelActor::StoryNext(FString& OutMovie) const
{
	// OUTLAWS.RCS STORY: MOVIE / LEVEL / LEVELHD <sd> <hd> / CREDITS lines, in play order
	OutMovie.Reset();
	{
		FString Next;
		if (RandoStoryNext(OutMovie, Next)) return Next;           // (R10: a shuffled run's order, its cutscenes by position)
	}
	FString Text;
	if (!bHom && IsHandfulLevel(LevelName) && AssetsRef)
	{
		// (REQ-HMCAREER) a Handful mission goes on through its own script's levels (Civil War: civlwar1, then civlwar2); after
		// its last, nothing (the title menu, as a single mission always did)
		for (const FHandfulMission& M : HandfulMissions)
		{
			if (!AssetsRef->GetText(M.Rca, Text)) continue;
			TArray<FString> Lines, Levels;
			Text.ParseIntoArrayLines(Lines);
			for (const FString& Line : Lines)
			{
				TArray<FString> T;
				Line.ParseIntoArrayWS(T);
				if (T.Num() >= 2 && T[0].Equals(TEXT("LEVEL:"), ESearchCase::IgnoreCase)) Levels.Add(T[1].ToLower());
			}
			const int32 At = Levels.IndexOfByKey(LevelName.ToLower());
			if (At != INDEX_NONE) return Levels.IsValidIndex(At + 1) ? Levels[At + 1] : FString();
		}
		Text.Reset();
	}
	if (!AssetsRef || !AssetsRef->GetText(TEXT("outlaws.rcs"), Text))
	{
		return FString();
	}
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	bool bFound = false;
	for (const FString& Line : Lines)
	{
		TArray<FString> T;
		Line.ParseIntoArrayWS(T);
		if (T.Num() < 2) continue;
		const FString Op = T[0].ToUpper();
		if (!bFound)
		{
			if ((Op == TEXT("LEVEL:") && T[1].Equals(LevelName, ESearchCase::IgnoreCase)) ||
				(Op == TEXT("LEVELHD:") && T.Num() > 2 && (T[1].Equals(LevelName, ESearchCase::IgnoreCase) || T[2].Equals(LevelName, ESearchCase::IgnoreCase))))
			{
				bFound = true;
			}
			continue;
		}
		if (Op == TEXT("MOVIE:") || Op == TEXT("CREDITS:"))
		{
			if (OutMovie.IsEmpty()) OutMovie = FPaths::GetBaseFilename(T[1]);
			if (Op == TEXT("CREDITS:")) return FString();
		}
		else if (Op == TEXT("LEVEL:"))
		{
			return T[1].ToLower();
		}
		else if (Op == TEXT("LEVELHD:") && T.Num() > 2)
		{
			return T[2].ToLower();        // the HD build of the level (townex for town)
		}
	}
	return FString();
}

void AOutlawsLevelActor::StartMenuMusic()
{
	if (bMenuMusic && MusicCues.Num() > 0 && MusicCue >= 0)
	{
		return;                           // (already playing: back from a menu page or a movie it kept)
	}
	StopMusic();
	MusicCues.Reset();
	FMusicCmd Track;
	Track.Track = 3;
	Track.Disc = TEXT('A');
	FMusicCmd Again;
	Again.bGoto = true;
	Again.Cue = 0;
	MusicCues.Add({ Track, Again });
	MusicCue = 0;
	MusicStep = -1;
	MusicEndReal = 0.0;
	bMenuMusic = true;
	UpdateMusic();
}

void AOutlawsLevelActor::StartMusic(const FString& Msc)
{
	bMenuMusic = false;
	// MSC 1.0: START_CUE n, CUE n BEGIN ... END with CD_TRACK <disc label> <track> and GOTO_CUE n;
	// the label's last character picks the disc: 1 (or *) -> TRKnnA, 2 -> TRKnnB [EXE 0x14031f0e0]
	StopMusic();
	WarmMusicAudio();
	MusicCues.Reset();
	FString Text;
	if (Msc.IsEmpty() || Msc.Equals(TEXT("NULL"), ESearchCase::IgnoreCase) || !AssetsRef || !AssetsRef->GetText(Msc, Text))
	{
		return;
	}
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	int32 Start = 1, Cur = -1;
	for (const FString& Line : Lines)
	{
		TArray<FString> T;
		Line.ParseIntoArrayWS(T);
		if (T.Num() == 0) continue;
		const FString K = T[0].ToUpper();
		if (K == TEXT("START_CUE:") && T.Num() > 1) Start = FCString::Atoi(*T[1]);
		else if (K == TEXT("CUE") && T.Num() > 1)
		{
			Cur = FCString::Atoi(*T[1]) - 1;
			if (Cur >= 0 && Cur < 64 && MusicCues.Num() <= Cur) MusicCues.SetNum(Cur + 1);
		}
		else if (K == TEXT("CD_TRACK") && T.Num() > 2 && MusicCues.IsValidIndex(Cur))
		{
			FMusicCmd C;
			C.Track = FCString::Atoi(*T[2]);
			C.Disc = T[1].EndsWith(TEXT("2")) ? TEXT('B') : TEXT('A');
			MusicCues[Cur].Add(C);
		}
		else if (K == TEXT("GOTO_CUE") && T.Num() > 1 && MusicCues.IsValidIndex(Cur))
		{
			FMusicCmd C;
			C.bGoto = true;
			C.Cue = FCString::Atoi(*T[1]) - 1;
			MusicCues[Cur].Add(C);
		}
	}
	MusicCue = Start - 1;
	MusicEndReal = 0.0;
	MusicStep = -1;          // UpdateMusic steps onto the cue's first command
	UE_LOG(LogOutlaws, Log, TEXT("OLRT music %s: %d cues, start %d"), *Msc, MusicCues.Num(), Start);
	UpdateMusic();
}

TMap<FString, FString> AOutlawsLevelActor::ReadSettings()
{
	TMap<FString, FString> Out;
	FString Text;
	if (FFileHelper::LoadFileToString(Text, *(FPaths::ProjectSavedDir() / TEXT("OutlawsSettings.ini"))))
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			FString K, V;
			if (Line.Split(TEXT("="), &K, &V)) Out.Add(K.TrimStartAndEnd(), V.TrimStartAndEnd());
		}
	}
	return Out;
}

void AOutlawsLevelActor::WriteSettings(const TMap<FString, FString>& Settings)
{
	if (OutlawsPerfTest::IsSweep())
	{
		// REQ-PERFSWEEP: the Settings sweep changes the rows itself and puts the player's back; nothing reaches the file till
		// the main menu (a row pressed in its pause menu is undone at its next phase and at its end)
		UE_LOG(LogOutlaws, Log, TEXT("OLRT settings sweep: the settings file left as it was (held for the sweep)"));
		return;
	}
	FString Text = TEXT("; Outlaws VR settings (the in-game menu writes this)\n");
	TArray<FString> Keys;
	Settings.GetKeys(Keys);
	Keys.Sort();
	for (const FString& K : Keys) Text += K + TEXT("=") + Settings[K] + TEXT("\n");
	// (UTF-8: the Multiplayer page's player name may be any text; plain ASCII files come out byte for byte as before)
	FFileHelper::SaveStringToFile(Text, *(FPaths::ProjectSavedDir() / TEXT("OutlawsSettings.ini")), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void AOutlawsLevelActor::LoadSettings()
{
	const TMap<FString, FString> S = ReadSettings();
	auto B = [&S](const TCHAR* K, bool& V) { if (const FString* X = S.Find(K)) V = X->ToBool(); };
	auto F = [&S](const TCHAR* K, float& V, float Lo, float Hi) { if (const FString* X = S.Find(K)) V = FMath::Clamp(FCString::Atof(**X), Lo, Hi); };
	B(TEXT("RemasteredGraphics"), bHDTextures);
	// HD textures by the headset (user 2026-10-03): until the player presses Remastered Graphics, the Quest 2 profile
	// starts them off; any other headset keeps the file's value as before
	bHDTexturesInFile = bHDTextures;
	bHDChosen = false;
	B(TEXT("RemasteredGraphicsChosen"), bHDChosen);
	if (!bHDChosen && !OutlawsDeviceProfile::DefaultHDTextures()) bHDTextures = false;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT graphics: HD textures %s (%s)"), bHDTextures ? TEXT("on") : TEXT("off"),
		bHDChosen ? TEXT("the player's choice") : OutlawsDeviceProfile::DefaultHDTextures() ? TEXT("not chosen: the settings file's value") : TEXT("not chosen: off by the Quest 2 profile"));
	OutlawsDeviceProfile::LoadSettings(S);      // the Graphics page's performance rows (resolution, refresh rate, lamps, HUD)
	B(TEXT("RemasteredSounds"), bHDSounds);
	B(TEXT("Tinting"), bPaletteGrades);
	B(TEXT("StochasticTextures"), bStochastic);
	F(TEXT("MovementSpeed"), GameSpeed, 0.1f, 1.f);
	// saved before the slider was halved (user 2026-10-01: its 100 % is the old 50 %): the same speed on the new scale
	if (S.Contains(TEXT("MovementSpeed")) && !S.Contains(TEXT("MovementSpeedScale"))) GameSpeed = FMath::Clamp(GameSpeed * 2.f, 0.2f, 1.f);
	GameSpeed = FMath::Clamp(GameSpeed, 0.2f, 1.f);
	// (user 2026-10-03) a dial saved before the new curve: moved to the dial value that gives the same speed, once (the file
	// is marked "MovementSpeedCurve=2" when it is next saved), so nobody's feel changes
	if (S.Contains(TEXT("MovementSpeed")) && !S.Contains(TEXT("MovementSpeedCurve")))
	{
		const float Was = GameSpeed;
		// (the old default, 100 %, is the new default, 100 %: the slower speed the user asked for; any other choice keeps
		// its speed)
		GameSpeed = FMath::IsNearlyEqual(Was, 0.5f, 0.001f) ? 0.5f : FMath::Clamp(FMath::RoundToFloat(SpeedDialFor(Was) * 200.f) / 200.f, 0.2f, 1.f);
		UE_LOG(LogOutlaws, Log, TEXT("OLRT movement speed: the saved dial %.0f %% is %.0f %% on the new dial (the same speed)"), Was * 200.f, GameSpeed * 200.f);
	}
	if (GameSpeed > MaxGameSpeed)
	{
		// (BUG-SPEEDCAP) the dial tops out at 150 %: a setting above it (the old 200 %) loads as the top
		UE_LOG(LogOutlaws, Log, TEXT("OLRT movement speed: the saved %.0f %% is over the top of the dial: %.0f %%"), GameSpeed * 200.f, MaxGameSpeed * 200.f);
		GameSpeed = MaxGameSpeed;
	}
	static bool bCurveLogged = false;
	if (!bCurveLogged)
	{
		bCurveLogged = true;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT movement speed dial (user 2026-10-03): 40 %% -> the old 40 %%, 100 %% -> the old 90 %%, 200 %% -> the old 200 %%; now %.0f %% (pace %.3f)"),
			GameSpeed * 200.f, SpeedFromDial(GameSpeed) * 0.5f);
	}
	F(TEXT("MasterVolume"), MasterVolume, 0.f, 1.f);
	F(TEXT("SoundVolume"), SoundVolume, 0.f, 1.f);
	F(TEXT("MusicVolume"), MusicVolume, 0.f, 1.f);
	F(TEXT("MovieVolume"), MovieVolume, 0.f, 1.f);
	B(TEXT("Autosave"), bAutosave);
	F(TEXT("AutosaveMinutes"), AutosaveMinutes, 1.f, 30.f);
	// the Multiplayer page (user 2026-10-02)
	B(TEXT("MpMicrophone"), bMpMicrophone);
	if (const FString* X = S.Find(TEXT("MpPlayerName"))) MpPlayerName = CleanMpName(*X);
	if (const FString* X = S.Find(TEXT("MpPushToTalk"))) MpPushToTalk = FMath::Clamp(FCString::Atoi(**X), 0, 3);
	if (const FString* X = S.Find(TEXT("CoopBadGuys"))) CoopBadGuys = X->Equals(TEXT("auto"), ESearchCase::IgnoreCase) ? 0.f : FMath::Clamp(FCString::Atof(**X), 1.f, 4.f);
	if (const FString* X = S.Find(TEXT("SoloBadGuys"))) SoloBadGuys = FMath::RoundToFloat(FMath::Clamp(FCString::Atof(**X), 1.f, 4.f) * 4.f) / 4.f;
	if (const FString* X = S.Find(TEXT("CoopMaxPlayers"))) CoopMaxPlayers = FMath::Clamp(FCString::Atoi(**X), 2, 8);
	if (const FString* X = S.Find(TEXT("MpMicSensitivity"))) MpMicSensitivity = FMath::Clamp(FCString::Atoi(**X), 0, 2);
	if (const FString* X = S.Find(TEXT("MpPartnerVolume"))) MpPartnerVolume = FMath::Clamp(FCString::Atoi(**X), 50, 300);
	B(TEXT("MpVoiceFade"), bMpVoiceFade);
	UOutlawsCoop::ApplyMicSensitivity(MpMicSensitivity);
	// the Randomizer page (R10, user 2026-10-03): taken by a run at its New Game
	if (const FString* X = S.Find(TEXT("RandoEnemies"))) RandoEnemies = FMath::Clamp(FCString::Atoi(**X), 0, 2);
	if (const FString* X = S.Find(TEXT("RandoItems"))) RandoItems = FMath::Clamp(FCString::Atoi(**X), 0, 2);
	if (const FString* X = S.Find(TEXT("RandoOrder"))) RandoOrder = FMath::Clamp(FCString::Atoi(**X), 0, 2);
	if (const FString* X = S.Find(TEXT("RandoBosses"))) RandoBosses = FMath::Clamp(FCString::Atoi(**X), 0, 1);
	if (const FString* X = S.Find(TEXT("RandoCritters"))) RandoCritters = FMath::Clamp(FCString::Atoi(**X), 0, 1);
	if (const FString* X = S.Find(TEXT("RandoSeed"))) RandoSeedText = X->TrimStartAndEnd().Left(RandoSeedMax);
	FString Arg;
	if (OLFlags::Value(TEXT("OLMpName"), Arg))
	{
		bMpNameArg = true;
		MpPlayerNameInFile = MpPlayerName;
		MpPlayerName = CleanMpName(Arg);
	}
}

FString AOutlawsLevelActor::CleanMpName(const FString& In)
{
	FString Out;
	for (const TCHAR Ch : In)
	{
		if (Ch >= 32 && Ch != 127) Out.AppendChar(Ch);
	}
	Out.TrimStartAndEndInline();
	return Out.Left(MpNameMax);
}

void AOutlawsLevelActor::GatherSettings(TMap<FString, FString>& Out) const
{
	auto B = [&Out](const TCHAR* K, bool V) { Out.Add(K, V ? TEXT("true") : TEXT("false")); };
	auto F = [&Out](const TCHAR* K, float V) { Out.Add(K, FString::SanitizeFloat(V)); };
	// (not chosen: the file keeps its own value, so a Quest 2 default or a forced profile in a test never rewrites it)
	B(TEXT("RemasteredGraphics"), bHDChosen ? bHDTextures : bHDTexturesInFile);
	B(TEXT("RemasteredGraphicsChosen"), bHDChosen);      // (written false too: the save merges into the file's old keys)
	OutlawsDeviceProfile::GatherSettings(Out);
	B(TEXT("RemasteredSounds"), bHDSounds);
	B(TEXT("Tinting"), bPaletteGrades);
	B(TEXT("StochasticTextures"), bStochastic);
	F(TEXT("MovementSpeed"), GameSpeedInFile >= 0.f ? GameSpeedInFile : GameSpeed);     // (a co-op test's own speed is never written)
	Out.Add(TEXT("MovementSpeedScale"), TEXT("2"));        // (the halved slider: Pace = speed x 0.5)
	Out.Add(TEXT("MovementSpeedCurve"), TEXT("2"));        // (user 2026-10-03: the dial's 100 % is the old 90 %; SpeedFromDial)
	F(TEXT("MasterVolume"), MasterVolume);
	F(TEXT("SoundVolume"), SoundVolume);
	F(TEXT("MusicVolume"), MusicVolume);
	F(TEXT("MovieVolume"), MovieVolume);
	B(TEXT("Autosave"), bAutosave);
	F(TEXT("AutosaveMinutes"), AutosaveMinutes);
	Out.Add(TEXT("MpMicrophone"), bMpMicrophone ? TEXT("1") : TEXT("0"));
	Out.Add(TEXT("MpPushToTalk"), FString::FromInt(MpPushToTalk));
	Out.Add(TEXT("CoopBadGuys"), CoopBadGuys <= 0.f ? FString(TEXT("auto")) : FString::Printf(TEXT("%.2f"), CoopBadGuys));
	Out.Add(TEXT("SoloBadGuys"), FString::Printf(TEXT("%.2f"), SoloBadGuys));
	Out.Add(TEXT("CoopMaxPlayers"), FString::FromInt(CoopMaxPlayers));
	Out.Add(TEXT("MpMicSensitivity"), FString::FromInt(MpMicSensitivity));
	Out.Add(TEXT("MpPartnerVolume"), FString::FromInt(MpPartnerVolume));
	Out.Add(TEXT("MpVoiceFade"), bMpVoiceFade ? TEXT("1") : TEXT("0"));
	Out.Add(TEXT("MpPlayerName"), bMpNameArg ? MpPlayerNameInFile : MpPlayerName);      // (a test's -OLMpName is never written)
	Out.Add(TEXT("RandoEnemies"), FString::FromInt(RandoEnemies));
	Out.Add(TEXT("RandoItems"), FString::FromInt(RandoItems));
	Out.Add(TEXT("RandoOrder"), FString::FromInt(RandoOrder));
	Out.Add(TEXT("RandoBosses"), FString::FromInt(RandoBosses));
	Out.Add(TEXT("RandoCritters"), FString::FromInt(RandoCritters));
	Out.Add(TEXT("RandoSeed"), RandoSeedText);
}

void AOutlawsLevelActor::ApplyMusicVolume()
{
	// (user 2026-10-03, BUG-MUSIC: at 0 the music stopped and did not come back until the next track) never quite 0:
	// the engine stops a silent voice; the track plays on unheard and is there when the slider comes back up
	if (MusicAudio.IsValid()) MusicAudio->SetVolumeMultiplier(FMath::Max(MusicVolume * MasterVolume * LevelSoundDuck, 0.001f));
}

void AOutlawsLevelActor::StopMusic()
{
	if (MusicAudio.IsValid()) MusicAudio->Stop();
	MusicAudio = nullptr;
	DisarmMusic();
	MusicNextFile.Reset();
	MusicCue = -1;
	MusicEndReal = 0.0;
}

void AOutlawsLevelActor::UpdateMusic()
{
	OL_PERF_SCOPE("Level: music");
	// the next cue once the track has run its length in real time (not on the voice going quiet: a
	// stolen or paused voice would otherwise skip straight on, loading track after track)
	if (!MusicCues.IsValidIndex(MusicCue) || FPlatformTime::Seconds() < MusicEndReal)
	{
		TickMusicAhead();
		return;
	}
	// the track ended (or none yet): the next command; a spent cue falls through to the next one,
	// GOTO_CUE jumps, and after the last cue the music stops
	for (int32 Guard = 0; Guard < 64; ++Guard)
	{
		++MusicStep;
		if (!MusicCues.IsValidIndex(MusicCue))
		{
			MusicCue = -1;
			return;
		}
		if (MusicStep >= MusicCues[MusicCue].Num())
		{
			++MusicCue;
			MusicStep = -1;
			continue;
		}
		const FMusicCmd& C = MusicCues[MusicCue][MusicStep];
		if (C.bGoto)
		{
			MusicCue = C.Cue;
			MusicStep = -1;
			continue;
		}
		const FString File = MusicFileFor(C);
		if (!FPaths::FileExists(File))
		{
#if PLATFORM_WINDOWS
			// (a PC: the install's flac decoded into the media cache, then this same command again)
			const FString Track = FString::Printf(TEXT("TRK%02d%c"), C.Track, C.Disc);
			if (AssetsRef && OutlawsMusicDecode::Start(FPaths::Combine(AssetsRef->GetRoot(), TEXT("music"), Track + TEXT(".flac")),
				FPaths::Combine(OutlawsMediaDir(), TEXT("Music"), Track + TEXT(".wav"))))
			{
				--MusicStep;
				MusicEndReal = FPlatformTime::Seconds() + 1.0;
				return;
			}
#endif
			UE_LOG(LogOutlaws, Log, TEXT("OLRT music %s missing (convert_movies.py): the level stays quiet"), *File);
			MusicCue = -1;
			return;
		}
		// (STUTTER) read on a worker: the same command again next frame till it is in
		const double Ts0 = FPlatformTime::Seconds();
		if (!MusicReady(File))
		{
			--MusicStep;
			return;
		}
		// (BUG-STUTTER2 (5)) its wave and component made on a frame of their own (TickMusicAhead does it well before the
		// cue; a track not made ahead, the level's first, is made here and started next frame): this frame only starts it
		const FString Key = (TEXT("file:") + File).ToLower();
		if (MusicArmedKey != Key && MusicArmFailedKey != Key)
		{
			if (ArmMusic(File))
			{
				--MusicStep;
				return;
			}
			MusicArmFailedKey = Key;
		}
		const double Ts1 = FPlatformTime::Seconds();
		MusicAudio = MusicArmedKey == Key ? StartArmedMusic() : PlayWav(TEXT("file:") + File, FVector::ZeroVector, false, true);
		DisarmMusic();
		MusicArmFailedKey.Reset();
		const double Ts2 = FPlatformTime::Seconds();
		if (MusicAudio.IsValid())
		{
			if (MusicAudio->Sound) MusicAudio->Sound->VirtualizationMode = EVirtualizationMode::PlayWhenSilent;   // (BUG-MUSIC)
			MusicAudio->SetVolumeMultiplier(FMath::Max(MusicVolume * MasterVolume * LevelSoundDuck, 0.001f));
			MusicAudio->bOverridePriority = true;       // never the voice given up when too many play
			MusicAudio->Priority = 100.f;
			MusicEndReal = FPlatformTime::Seconds() + (MusicAudio->Sound ? MusicAudio->Sound->Duration : 0.f);
		}
		else
		{
			MusicEndReal = FPlatformTime::Seconds() + 5.0;   // could not start: try the next in a while, not every frame
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLRT music cue %d: %s"), MusicCue + 1, *FPaths::GetCleanFilename(File));
		MusicNextFile = PeekNextMusicFile();      // the one after, read and made ready while this plays (from a later frame)
		MusicAheadWait = 2;
		// (BUG-STUTTER2: build 32's switch still cost 40 to 54 ms in "Level: music"; where it goes, when it is more than 5 ms)
		const double Ts3 = FPlatformTime::Seconds();
		if (Ts3 - Ts0 > 0.005)
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLRT music switch %.1f ms: ready %.1f, play %.1f, rest %.1f"), (Ts3 - Ts0) * 1000.0,
				(Ts1 - Ts0) * 1000.0, (Ts2 - Ts1) * 1000.0, (Ts3 - Ts2) * 1000.0);
		}
		MusicSwitchMs = (Ts3 - Ts0) * 1000.0;
		++MusicSwitchCount;
		return;
	}
	MusicCue = -1;
}

void AOutlawsLevelActor::TickMusicAhead()
{
	// (BUG-STUTTER2 (5)) while a track plays: the next one read on a worker, then its wave and component made, one step a
	// frame and none on a switch frame, so the cue that follows has only to start it
	if (MusicNextFile.IsEmpty() || !MusicCues.IsValidIndex(MusicCue))
	{
		return;
	}
	if (MusicAheadWait > 0)
	{
		--MusicAheadWait;
		return;
	}
	if (!FPaths::FileExists(MusicNextFile))
	{
		MusicNextFile.Reset();             // (UpdateMusic decodes or skips a missing one when its cue comes)
		return;
	}
	if (!MusicReady(MusicNextFile))
	{
		return;                            // still reading
	}
	if (!ArmMusic(MusicNextFile))
	{
		MusicArmFailedKey = (TEXT("file:") + MusicNextFile).ToLower();
	}
	MusicNextFile.Reset();
}

void AOutlawsLevelActor::WarmMusicAudio()
{
	// (BUG-STUTTER2 (5)) the first 2D sound component a run makes loads what the audio device wants for it (25 ms on a cold
	// desktop start): one made and let go here in the level's load, so the first cue's own making is cheap
	UWorld* World = GetWorld();
	if (bMusicAudioWarm || !bPlaySounds || !World || !World->IsGameWorld())
	{
		return;
	}
	bMusicAudioWarm = true;
	const double T0 = FPlatformTime::Seconds();
	UOutlawsPcmWave* S = NewObject<UOutlawsPcmWave>(this);
	S->SetSampleRate(22050);
	S->NumChannels = 1;
	S->Duration = 0.1f;
	S->SoundGroup = SOUNDGROUP_Default;
	S->bLooping = false;
	if (UAudioComponent* AC = UGameplayStatics::CreateSound2D(this, S, 0.001f, 1.f, 0.f, nullptr, false, true))
	{
		AC->DestroyComponent();
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT music audio warmed in the load: %.1f ms"), (FPlatformTime::Seconds() - T0) * 1000.0);
}

bool AOutlawsLevelActor::ArmMusic(const FString& File)
{
	DisarmMusic();
	UWorld* World = GetWorld();
	if (bRestoring || !bPlaySounds || !World || !World->IsGameWorld())
	{
		return false;
	}
	const FString Key = (TEXT("file:") + File).ToLower();
	const int32 At = MusicWavCache.IndexOfByPredicate([&Key](const TPair<FString, TSharedPtr<FWav>>& P) { return P.Key == Key; });
	const TSharedPtr<FWav> Wav = At != INDEX_NONE ? MusicWavCache[At].Value : nullptr;
	if (!Wav || Wav->Pcm.Num() == 0)
	{
		return false;
	}
	const double T0 = FPlatformTime::Seconds();
	// as PlayWav makes a 2D one-shot (the samples shared, no copy; whole samples, trimmed on the worker), not started
	UOutlawsPcmWave* S = NewObject<UOutlawsPcmWave>(this);
	S->SetSampleRate(Wav->Rate);
	S->NumChannels = Wav->Channels;
	S->Duration = float(Wav->Pcm.Num()) / float(2 * FMath::Max(1, Wav->Channels) * FMath::Max(1, Wav->Rate));
	S->SoundGroup = SOUNDGROUP_Default;
	S->bLooping = false;
	S->Pcm = TSharedPtr<const TArray<uint8>>(Wav, &Wav->Pcm);
	S->VirtualizationMode = EVirtualizationMode::PlayWhenSilent;      // (BUG-MUSIC)
	UAudioComponent* AC = UGameplayStatics::CreateSound2D(this, S, FMath::Max(MusicVolume * MasterVolume * LevelSoundDuck, 0.001f), 1.f, 0.f, nullptr, false, true);
	if (!AC)
	{
		return false;                      // (no audio device: -nosound)
	}
	AC->bOverridePriority = true;
	AC->Priority = 100.f;
	MusicArmed = AC;
	MusicArmedKey = Key;
	MusicPlayMs[0] = (FPlatformTime::Seconds() - T0) * 1000.0;
	return true;
}

void AOutlawsLevelActor::DisarmMusic()
{
	if (MusicArmed)
	{
		MusicArmed->DestroyComponent();
	}
	MusicArmed = nullptr;
	MusicArmedKey.Reset();
}

UAudioComponent* AOutlawsLevelActor::StartArmedMusic()
{
	// PlayWav's bookkeeping for a 2D one-shot, on the component made ahead
	UAudioComponent* AC = MusicArmed;
	MusicArmed = nullptr;                  // (playing: the audio device keeps it, and MusicAudio watches it)
	if (!AC || !IsValid(AC))
	{
		return nullptr;
	}
	const double T0 = FPlatformTime::Seconds();
	// the cache keeps its newest last: this one is in use again
	const int32 At = MusicWavCache.IndexOfByPredicate([this](const TPair<FString, TSharedPtr<FWav>>& P) { return P.Key == MusicArmedKey; });
	if (At != INDEX_NONE && At != MusicWavCache.Num() - 1)
	{
		TPair<FString, TSharedPtr<FWav>> E = MoveTemp(MusicWavCache[At]);
		MusicWavCache.RemoveAt(At);
		MusicWavCache.Add(MoveTemp(E));
	}
	MusicArmedKey.Reset();
	AC->SetVolumeMultiplier(FMath::Max(MusicVolume * MasterVolume * LevelSoundDuck, 0.001f));
	if (bMuffled)
	{
		AC->SetLowPassFilterFrequency(MuffleHz);
		AC->SetLowPassFilterEnabled(true);
	}
	AC->Play();
	if (LevelSoundDuck < 1.f) DuckedSounds.Add(AC);
	TimedSounds.Emplace(AC, FPlatformTime::Seconds() + (AC->Sound ? AC->Sound->Duration : 0.f) + 0.1);
	LiveSounds.Emplace(AC, false);
	MusicPlayMs[1] = (FPlatformTime::Seconds() - T0) * 1000.0;
	MusicPlayMs[2] = 0.0;
	return AC;
}

FString AOutlawsLevelActor::MusicFileFor(const FMusicCmd& C) const
{
	return DataFile(TEXT("Music"), FString::Printf(TEXT("TRK%02d%c.wav"), C.Track, C.Disc));
}

FString AOutlawsLevelActor::PeekNextMusicFile() const
{
	// UpdateMusic's walk from where it stands, nothing changed: the track it will load next (empty: none)
	int32 Cue = MusicCue, Step = MusicStep;
	for (int32 Guard = 0; Guard < 64; ++Guard)
	{
		++Step;
		if (!MusicCues.IsValidIndex(Cue)) return FString();
		if (Step >= MusicCues[Cue].Num())
		{
			++Cue;
			Step = -1;
			continue;
		}
		const FMusicCmd& C = MusicCues[Cue][Step];
		if (C.bGoto)
		{
			Cue = C.Cue;
			Step = -1;
			continue;
		}
		return MusicFileFor(C);
	}
	return FString();
}

void AOutlawsLevelActor::PrefetchMusic(const FString& File)
{
	if (File.IsEmpty() || !FPaths::FileExists(File)) return;
	const FString Key = (TEXT("file:") + File).ToLower();
	if (MusicPrefetch.Contains(Key) || MusicWavCache.ContainsByPredicate([&Key](const TPair<FString, TSharedPtr<FWav>>& P) { return P.Key == Key; })) return;
	MusicPrefetch.Add(Key, Async(EAsyncExecution::ThreadPool, [File]()
	{
		TSharedPtr<FWav> Wav = MakeShared<FWav>();
		TArray<uint8> D;
		if (FFileHelper::LoadFileToArray(D, *File) && D.Num() > 12 && FMemory::Memcmp(D.GetData(), "RIFF", 4) == 0) ParseRiffWav(D, *Wav);
		// (BUG-STUTTER2) whole samples here, not in PlayWav: a stray byte trimmed there shrank the track's 40 MB on the game thread
		const int32 Block = 2 * FMath::Max(1, Wav->Channels);
		if (Wav->Pcm.Num() % Block) Wav->Pcm.SetNum(Wav->Pcm.Num() - Wav->Pcm.Num() % Block);
		return Wav;
	}));
}

bool AOutlawsLevelActor::MusicReady(const FString& File)
{
	const FString Key = (TEXT("file:") + File).ToLower();
	if (MusicWavCache.ContainsByPredicate([&Key](const TPair<FString, TSharedPtr<FWav>>& P) { return P.Key == Key; })) return true;
	TFuture<TSharedPtr<FWav>>* F = MusicPrefetch.Find(Key);
	if (!F)
	{
		PrefetchMusic(File);
		return !MusicPrefetch.Contains(Key);     // (could not start one: PlayWav reads it as before)
	}
	if (!F->IsReady()) return false;
	const TSharedPtr<FWav> Wav = F->Get();
	MusicPrefetch.Remove(Key);
	if (!Wav || Wav->Pcm.Num() == 0) return true;      // (PlayWav tries it the old way and logs what it finds)
	MusicWavCache.Emplace(Key, Wav);
	if (MusicWavCache.Num() > 2)
	{
		// (BUG-STUTTER2) the oldest track's samples (tens of MB) let go on a worker, not freed on the game thread; two kept,
		// the playing one and the next (a 3 to 4 minute track is about 40 MB: a third held was 40 MB more on the Quest)
		TSharedPtr<FWav> Old = MoveTemp(MusicWavCache[0].Value);
		MusicWavCache.RemoveAt(0);
		Async(EAsyncExecution::ThreadPool, [Old = MoveTemp(Old)]() mutable { Old.Reset(); });
	}
	return true;
}

void AOutlawsLevelActor::VaryPitch(UAudioComponent* A)
{
	if (A) A->SetPitchMultiplier(FMath::FRandRange(0.875f, 1.125f));
}

void AOutlawsLevelActor::UpdateSoundGens(float Dt)
{
	OL_PERF_SCOPE("Level: sound generators");
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	const FVector Listener = PC && PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : FVector::ZeroVector;
	for (FSoundGen& G : SoundGens)
	{
		G.Playing.RemoveAll([](const TWeakObjectPtr<UAudioComponent>& A) { return !A.IsValid() || !A->IsPlaying(); });
		// an occasional sound (positive FREQ) waits while its last one still plays: the gap runs from its end, and it
		// never lies over itself (the continuous ones, negative FREQ, loop on their own period as before)
		if (G.Freq > 0.f && G.Playing.Num() > 0)
		{
			continue;
		}
		G.Wait -= Dt;
		if (G.Wait > 0.f)
		{
			continue;
		}
		// FREQ (INFERRED from the data, all 44 sound ITMs): negative = a fixed repeat period in seconds,
		// the continuous sounds (fire -0.8, water -3.2: their sample lengths, wind -7, hawk -160);
		// positive = times a minute at random, the occasional ones (crow 1, rooster 3, dog 3, thunder 2)
		if (G.Freq < 0.f)
		{
			G.Wait = FMath::Max(0.3f, -G.Freq);
		}
		else
		{
			const float Mean = 60.f / FMath::Max(0.1f, G.Freq);
			G.Wait = Mean * FMath::FRandRange(0.4f, 1.6f);
			// the wolves howl about once a minute (their FREQ 5 had them near constant; user 2026-09-28)
			if (G.Wav.StartsWith(TEXT("wolves"), ESearchCase::IgnoreCase)) G.Wait = 60.f * FMath::FRandRange(0.8f, 1.2f);
		}
		if (G.Playing.Num() >= G.Max)
		{
			continue;
		}
		if (UAudioComponent* A = PlayWav(G.Wav, G.bPoint ? G.Pos : Listener, false, !G.bPoint))
		{
			// the fires' crackle (SFIRE generators, fire.wav: the hideout's and the ranch's) at a quarter (user 2026-09-27)
			if (G.Wav.Equals(TEXT("fire"), ESearchCase::IgnoreCase)) A->SetVolumeMultiplier(A->VolumeMultiplier * 0.25f);
			VaryPitch(A);
			G.Playing.Add(A);
		}
	}
}

UTexture* AOutlawsLevelActor::SpriteHdFor(const TSharedPtr<OutlawsTextures::FSpriteAtlas>& Atlas)
{
	if (!Atlas || !AssetsRef)
	{
		return nullptr;
	}
	if (!Atlas->bHdTried && GetWorld() && GetWorld()->IsGameWorld())
	{
		OL_PERF_SCOPE("Sprites: HD atlas build");
		if (UTexture* T = OutlawsTextures::BuildSpriteAtlasHD(this, *AssetsRef, *Atlas))
		{
			SpriteAtlasesHd.Add(T);
		}
	}
	return Atlas->TextureHd;
}

bool AOutlawsLevelActor::IsNaturalSurface(const FString& Stem)
{
	// by the names the levels use: GF/GW + what it is (checked by eye over all 670 level textures, 2026-09-28)
	static const TCHAR* Natural[] = { TEXT("DIRT"), TEXT("MUD"), TEXT("ROK"), TEXT("ROCK"), TEXT("SND"), TEXT("SAND"), TEXT("CANYON"), TEXT("CNRK"),
		TEXT("CLF"), TEXT("CLIF"), TEXT("GRAV"), TEXT("GRVL"), TEXT("GRASS"), TEXT("DSRT"), TEXT("DESERT"), TEXT("HILL"), TEXT("TRAIL"), TEXT("GRND"),
		TEXT("GROUND"), TEXT("EARTH"), TEXT("DUST"), TEXT("CLAY"), TEXT("GFBRN") };
	static const TCHAR* Built[] = { TEXT("PLANK"), TEXT("WOOD"), TEXT("BOARD"), TEXT("BRICK"), TEXT("ADOBE"), TEXT("SHACK"), TEXT("BEAM"), TEXT("POST"),
		TEXT("DOOR"), TEXT("ROOF"), TEXT("TILE"), TEXT("BOX"), TEXT("CRATE"), TEXT("FENCE"), TEXT("LOG"), TEXT("STAIR"), TEXT("RAIL"), TEXT("SIGN"),
		TEXT("PAPER"), TEXT("METAL"), TEXT("IRON"), TEXT("GLASS"), TEXT("CURTAIN"), TEXT("RUG"), TEXT("CARPET"), TEXT("BARN"), TEXT("TELEG"),
		TEXT("CTBG") /* a backdrop picture (GCTBGRND) */ };
	const FString S = Stem.ToUpper();
	for (const TCHAR* B : Built) if (S.Contains(B)) return false;
	for (const TCHAR* N : Natural) if (S.Contains(N)) return true;
	return false;
}

void AOutlawsLevelActor::SetStochastic(bool bOn)
{
	bStochastic = bOn;
	// the world materials are keyed "<parent>|<stem>" (MaterialFor)
	int32 Natural = 0;
	for (const TPair<FString, TObjectPtr<UMaterialInstanceDynamic>>& Kv : Materials)
	{
		if (!Kv.Value) continue;
		FString Parent, Stem;
		if (!Kv.Key.Split(TEXT("|"), &Parent, &Stem)) continue;
		Parent.Split(TEXT("~"), &Parent, nullptr);          // (under water: "<parent>~w<chain>")
		if (!(Parent == TEXT("M_OLR_World") || Parent == TEXT("M_OLR_WorldMasked"))) continue;
		const bool bNat = IsNaturalSurface(Stem);
		Natural += bNat ? 1 : 0;
		Kv.Value->SetScalarParameterValue(TEXT("Stochastic"), bOn && bNat ? 1.f : 0.f);
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT stochastic textures %s: %d natural surfaces"), bOn ? TEXT("on") : TEXT("off"), Natural);
}

void AOutlawsLevelActor::ApplyHDDefault()
{
	if (bHDChosen) return;           // (the player's own choice stands)
	const bool bWant = OutlawsDeviceProfile::DefaultHDTextures() && bHDTexturesInFile;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT graphics: HD textures %s -> %s (not chosen: %s)"), bHDTextures ? TEXT("on") : TEXT("off"), bWant ? TEXT("on") : TEXT("off"),
		OutlawsDeviceProfile::DefaultHDTextures() ? TEXT("the settings file's value") : TEXT("off by the Quest 2 profile"));
	if (bWant != bHDTextures) SetHDTextures(bWant);
}

void AOutlawsLevelActor::SetHDTextures(bool bOn)
{
	bHDTextures = bOn;
	// sprites: every atlas's HD look (built on first use), matched to the sprites by their original
	TMap<UTexture*, UTexture*> SpriteHd;
	int32 NumHdAtlases = 0;
	for (const TPair<FString, TSharedPtr<OutlawsTextures::FSpriteAtlas>>& Kv : AtlasCache)
	{
		if (!Kv.Value || !Kv.Value->Texture) continue;
		UTexture* Hd = bOn ? SpriteHdFor(Kv.Value) : nullptr;
		SpriteHd.Add(Kv.Value->Texture, Hd);
		NumHdAtlases += Hd ? 1 : 0;
	}
	if (UWorld* World = GetWorld())
	{
		for (TActorIterator<AOutlawsSpriteActor> It(World); It; ++It)
		{
			if (It->GetOwner() != this) continue;
			UTexture* Lo = It->SpriteTextureLo ? It->SpriteTextureLo.Get() : It->SpriteTexture.Get();
			UTexture* const* Hd = SpriteHd.Find(Lo);
			It->SetLook(bOn && Hd ? *Hd : nullptr);
		}
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT remastered sprites: %d of %d atlases"), NumHdAtlases, SpriteHd.Num());
	for (const TPair<FString, TObjectPtr<UTexture2D>>& Kv : TexturesLo)
	{
		const TObjectPtr<UTexture2D>* Hd = TexturesHd.Find(Kv.Key);
		Textures.Add(Kv.Key, bOn && Hd ? *Hd : Kv.Value);
	}
	// every material showing a level texture (key "<parent>|<stem>") takes the other look
	for (const TPair<FString, TObjectPtr<UMaterialInstanceDynamic>>& Kv : Materials)
	{
		int32 Bar;
		if (!Kv.Value || !Kv.Key.FindChar(TEXT('|'), Bar)) continue;
		if (bGradientSky && Kv.Value == SkySurfaceMid) continue;      // (BUG-TRAINSKY: the gradient dome keeps its own picture)
		const FString Stem = Kv.Key.Mid(Bar + 1);
		if (const TObjectPtr<UTexture2D>* T = TexturesLo.Contains(Stem) ? Textures.Find(Stem) : nullptr)
		{
			Kv.Value->SetTextureParameterValue(TEXT("Tex"), *T);
		}
	}
	RefreshAtxFrames();
	// whole windows show the level texture too
	for (FPane& P : Panes)
	{
		if (P.bBroken) continue;
		for (int32 i = 0; i < P.Mids.Num() && i < P.Stems.Num(); ++i)
		{
			if (const TObjectPtr<UTexture2D>* T = Textures.Find(P.Stems[i])) P.Mids[i]->SetTextureParameterValue(TEXT("Tex"), *T);
		}
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT remastered graphics %s (%d of %d textures have an HD version)"), bOn ? TEXT("on") : TEXT("off"), TexturesHd.Num(), TexturesLo.Num());
}

void AOutlawsLevelActor::SetHDSounds(bool bOn)
{
	bHDSounds = bOn;
	WavCache.Reset();
	UE_LOG(LogOutlaws, Log, TEXT("OLRT remastered sounds %s"), bOn ? TEXT("on") : TEXT("off"));
}

int32 AOutlawsLevelActor::LightAt(const FVector& UE) const
{
	if (!LevelData)
	{
		return 31;
	}
	// Outlaws x = UE.Y, y = UE.Z, z = UE.X (units)
	const FOutlawsSector* S = LevelData->SectorAt(FOutlawsGeometry::ToOutlaws(UE, 0.5));
	return S ? FOutlawsLevel::LightLevel(S->Ambient) : 31;
}

extern bool GOlItemModelByTurn;     // (OutlawsLevelActorItemTest.cpp)

AActor* AOutlawsLevelActor::SpawnGroundItem(const FString& Name, const FVector& InFeetPos)
{
	if (!AssetsRef || Name.IsEmpty())
	{
		return nullptr;
	}
	OL_PERF_SCOPE("Level: drops");
	// (BUG-BELTCEIL) a drop rests on the floor under where it is made: a bad guy's drop comes when his body lies still, and
	// a body held in mid-air (frozen far off, or stood on another's head) left it hanging there; the town's belt hung 10 m
	// up over the two bodies it came from.  Down onto the first surface below (bodies and players are passed through)
	FVector FeetPos = InFeetPos;
	if (UWorld* WorldF = GetWorld())
	{
		FCollisionQueryParams QF(SCENE_QUERY_STAT(OutlawsDropFloor), true);
		FCollisionResponseParams RF;
		RF.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
		FHitResult Floor;
		if (WorldF->LineTraceSingleByChannel(Floor, InFeetPos + FVector(0.0, 0.0, 30.0), InFeetPos - FVector(0.0, 0.0, 5000.0), ECC_Visibility, QF, RF))
		{
			FeetPos.Z = Floor.ImpactPoint.Z;
			if (InFeetPos.Z - FeetPos.Z > 60.0)
			{
				UE_LOG(LogOutlaws, Log, TEXT("OLRT drop %s settled %.0f cm down onto the floor"), *Name, InFeetPos.Z - FeetPos.Z);
			}
		}
	}
	FString Itm;
	const FString Anim = OutlawsFormats::ResolveAnim(*AssetsRef, Name, &Itm);
	if (Anim.EndsWith(TEXT(".3do"), ESearchCase::IgnoreCase))
	{
		// a 3D ground item (a thrown knife comes down as gknife): its model, lying where it fell (built once a file, STUTTER)
		UWorld* World = GetWorld();
		const TMap<FString, FOutlawsMeshSection>* Cached = World ? GroundModels.Find(Anim.ToLower()) : nullptr;
		if (World && !Cached)
		{
			TMap<FString, FOutlawsMeshSection> Built;
			TMap<FString, UTexture2D*> Tex;
			if (BuildItemModel(Anim, Built, Tex)) Cached = &GroundModels.Add(Anim.ToLower(), MoveTemp(Built));
		}
		if (!Cached)
		{
			return nullptr;
		}
		const TMap<FString, FOutlawsMeshSection>& Sections = *Cached;
		FActorSpawnParameters SP;
		SP.Owner = this;
		SP.ObjectFlags |= RF_Transient;
		AActor* A = World->SpawnActor<AActor>(AActor::StaticClass(), FTransform(FeetPos), SP);
		if (!A)
		{
			return nullptr;
		}
		UProceduralMeshComponent* M = NewObject<UProceduralMeshComponent>(A);
		M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		M->SetCastShadow(false);
		A->SetRootComponent(M);
		M->RegisterComponent();
		int32 Tris = 0;
		AddSections(M, Sections, false, WorldMaterial, WorldMaskedMaterial, Tris);
		A->SetActorLocationAndRotation(FeetPos + FVector(0.0, 0.0, 2.0), FRotator(0.0, FMath::FRandRange(0.f, 360.f), 0.0));
		SpawnedActors.Add(A);
		if (!RegisterPickup(A, Name, Itm) && !IsStickGround(Name))       // (a stick is no ground pickup: its callers make it loose dynamite)
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLRT drop %s is not a pickup"), *Name);
		}
		SetActorTickEnabled(true);
		if (UOutlawsCoop::IsActive(this)) UOutlawsCoop::NoteGroundItem(this, A, Name, FeetPos);    // (co-op: numbered, made there too)
		return A;
	}
	if (!Anim.EndsWith(TEXT(".nwx"), ESearchCase::IgnoreCase))
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT drop %s: no sprite (%s)"), *Name, *Anim);
		return nullptr;
	}
	// (STUTTER -OLStutterProbe: where a drop's time goes; a test, never in Shipping)
#if WITH_OUTLAWS_TESTS
	static const bool bProbe = OLFlags::Param(TEXT("OLStutterProbe"));
#else
	constexpr bool bProbe = false;
#endif
	double Tp[5] = { bProbe ? FPlatformTime::Seconds() : 0.0 };
	const TSharedPtr<OutlawsTextures::FSpriteAtlas> DropAtlas = GetAtlas(Anim, false);
	if (bProbe) Tp[1] = FPlatformTime::Seconds();
	AOutlawsSpriteActor* S = MakeSprite(DropAtlas, Name, FeetPos, 0.f, LightAt(FeetPos));
	if (!S)
	{
		return nullptr;
	}
	S->SetFeet(DropAtlas->BelowPx, DropAtlas->FootPx, DropAtlas->CellH); S->SetFeetAcross(DropAtlas->SidePx2, DropAtlas->CellW);     // (on the frame's insertion point, user 2026-10-02)
	SpawnedActors.Add(S);
	if (bProbe) Tp[2] = FPlatformTime::Seconds();
	GOlItemModelByTurn = true;      // (STUTTER: a copy of the first drop's model, turned)
	UseItemModel(S, Anim, FMath::FRandRange(0.f, 360.f));      // dropped any which way
	GOlItemModelByTurn = false;
	if (bProbe) Tp[3] = FPlatformTime::Seconds();
	// dropped on a lift floor or a carrying door: it rides along
	if (UWorld* World = GetWorld())
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsDropRide), true);
		Q.AddIgnoredActor(S);
		FCollisionResponseParams RP;
		RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
		FHitResult Hit;
		if (World->LineTraceSingleByChannel(Hit, FeetPos + FVector(0.0, 0.0, 30.0), FeetPos - FVector(0.0, 0.0, 60.0), ECC_Visibility, Q, RP)
			&& ComponentGroup.Contains(Hit.GetComponent()))
		{
			S->AttachToComponent(Hit.GetComponent(), FAttachmentTransformRules::KeepWorldTransform);
		}
	}
	if (!RegisterPickup(S, Name, Itm) && !IsStickGround(Name))
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT drop %s is not a pickup"), *Name);
	}
	SetActorTickEnabled(true);
	if (UOutlawsCoop::IsActive(this)) UOutlawsCoop::NoteGroundItem(this, S, Name, FeetPos);    // (co-op: numbered, made there too)
	if (bProbe)
	{
		Tp[4] = FPlatformTime::Seconds();
		UE_LOG(LogOutlaws, Log, TEXT("OLRT probe drop %s (%s): atlas %.2f, sprite %.2f, model %.2f, rest %.2f ms"), *Name, *Anim,
			(Tp[1] - Tp[0]) * 1000.0, (Tp[2] - Tp[1]) * 1000.0, (Tp[3] - Tp[2]) * 1000.0, (Tp[4] - Tp[3]) * 1000.0);
	}
	return S;
}

void AOutlawsLevelActor::PrebuildForPlay()
{
	static const bool bSkip = OLFlags::Param(TEXT("OLNoPrebuild"));      // (the before of a measure)
	if (bSkip || !AssetsRef || !FApp::CanEverRender())
	{
		return;
	}
	auto Timed = [](const FString& What, TFunctionRef<void()> Fn)
	{
		const double T0 = FPlatformTime::Seconds();
		Fn();
		UE_LOG(LogOutlaws, Log, TEXT("OLRT prebuild %s: %.1f ms"), *What, (FPlatformTime::Seconds() - T0) * 1000.0);
	};
	// (BUG-DECALEDGE) the world mesh's triangles indexed, for cutting a decal to its surface
	Timed(TEXT("decal surfaces"), [this]() { OutlawsDecalClip::Prepare(WorldMesh); });
	// the bad guys' drops: each one's sprite atlas and 3D model, or its .3do ground model
	TSet<FString> Drops;
	for (const TWeakObjectPtr<AOutlawsEnemy>& W : Enemies)
	{
		if (const AOutlawsEnemy* E = W.Get(); E && !E->Def.DropItem.IsEmpty()) Drops.Add(E->Def.DropItem.ToLower());
	}
	// (CPU2 e) and the guns a player can put down (the wheel drop, a dead player's guns, the knife and the box of
	// dynamite): each weapon ITM's GROUND_ITEM.  A co-op client built the host's first dropped revolver in the frame
	// it came: 16.6 ms on the desktop
	static const TCHAR* const Guns[] = { TEXT("ipistol"), TEXT("irifle"), TEXT("isgun"), TEXT("idbsgun"), TEXT("isawgun"),
		TEXT("igatgun"), TEXT("iknife") };
	for (const TCHAR* Gun : Guns)
	{
		FString Text, Func;
		TMap<FString, FString> Data;
		if (!AssetsRef->GetText(FString(Gun) + TEXT(".itm"), Text)) continue;
		OutlawsFormats::ParseItm(Text, Func, Data);
		const FString Ground = FPaths::GetBaseFilename(Data.FindRef(TEXT("GROUND_ITEM"))).ToLower();
		if (!Ground.IsEmpty()) Drops.Add(Ground);
	}
	Drops.Add(TEXT("gamboxt"));
	for (const FString& Name : Drops)
	{
		Timed(TEXT("drop ") + Name, [this, &Name]()
		{
			const FString Anim = OutlawsFormats::ResolveAnim(*AssetsRef, Name);
			if (Anim.EndsWith(TEXT(".3do"), ESearchCase::IgnoreCase) && !GroundModels.Contains(Anim.ToLower()))
			{
				TMap<FString, FOutlawsMeshSection> Built;
				TMap<FString, UTexture2D*> Tex;
				if (BuildItemModel(Anim, Built, Tex)) GroundModels.Add(Anim.ToLower(), MoveTemp(Built));
			}
			else if (Anim.EndsWith(TEXT(".nwx"), ESearchCase::IgnoreCase))
			{
				const TSharedPtr<OutlawsTextures::FSpriteAtlas> Atlas = GetAtlas(Anim, false);
				ItemTraits(FPaths::GetBaseFilename(Anim).ToUpper());      // (its 3D model read in)
				// (CPU2 e) and its dropped look made once, out of sight, so the first real drop (at any light) is a copy
				// of it (UseItemModel's templates): the build and the ink shell were the 13 to 17 ms of a first drop
				if (AOutlawsSpriteActor* S = Atlas ? MakeSprite(Atlas, Name, GetActorLocation() - FVector(0.0, 0.0, 100000.0), 0.f, 31) : nullptr)
				{
					const FString Stem = FPaths::GetBaseFilename(Anim).ToUpper();
					const bool bHadDebugAt = DebugModelAt.Contains(Stem);
					GOlItemModelByTurn = true;
					if (UseItemModel(S, Anim, 0.f)) --NumItemModels;       // (not one of the level's)
					GOlItemModelByTurn = false;
					if (!bHadDebugAt) DebugModelAt.Remove(Stem);           // (tests: where a real one stands)
					S->Destroy();
				}
			}
		});
	}
	// the first dynamite blast: its sprite (every choreography) and the soot and crack marks
	Timed(TEXT("blast ") + BlastSprite, [this]() { GetAtlas(BlastSprite, true); DarkMarkMaterial(false); DarkMarkMaterial(true); });
	// (BUG-STUTTER2) and its HD atlas, with the fire and dust a blast leaves (the first blast on the Quest drew it: 125 ms)
	if (bHDTextures)
	{
		Timed(TEXT("blast HD atlases"), [this]()
		{
			SpriteHdFor(GetAtlas(BlastSprite, true));
			SpriteHdFor(GetAtlas(TEXT("fire01.nwx"), false));
			SpriteHdFor(GetAtlas(TEXT("dust.nwx"), false));
		});
	}
	// the rifle scope's picture (the first raise built a 772x646 texture)
	Timed(TEXT("scope ijrifle.spx cell 6"), [this]() { FIntPoint Size; SpxCell(TEXT("ijrifle.spx"), 6, Size); });
	// what the load let go of, collected now behind the fade (the first periodic GC after a load was 40 to 87 ms, then
	// seconds of purge): a full pass at the end of this frame
	if (GEngine) GEngine->ForceGarbageCollection(true);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT prebuild: a full GC at the end of this frame"));
}

#if WITH_OUTLAWS_TESTS
void AOutlawsLevelActor::TickMusicTest(float Dt)
{
	// (-OLMusicTest -OLLevel=<it> -OLNoFrontEnd: BUG-STUTTER2 (5)) the playing track stopped, then a quarter second on the
	// next cue started as if it had run out; ten of them, each switch frame's time and its time in UpdateMusic, then quit.
	// This runs before UpdateMusic in the level's tick, so a switch counted here was the last frame, whose length is Dt
	MusicTestT += Dt;
	if (MusicSwitchCount != MusicTestSeen)
	{
		MusicTestSeen = MusicSwitchCount;
		if (MusicTestSwitches == 0)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT music first cue: frame %.1f ms, music %.2f ms (made %.2f, start %.2f)"),
				Dt * 1000.f, MusicSwitchMs, MusicPlayMs[0], MusicPlayMs[1]);
		}
		else
		{
			MusicTestFrameMs.Add(Dt * 1000.f);
			MusicTestMusicMs.Add(float(MusicSwitchMs));
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT music switch %d: frame %.1f ms, music %.2f ms (wave %.2f, spawn %.2f, rest %.2f)"),
				MusicTestSwitches, Dt * 1000.f, MusicSwitchMs, MusicPlayMs[0], MusicPlayMs[1], MusicPlayMs[2]);
		}
	}
	else if (MusicTestT > 4.f)
	{
		MusicTestOtherMs.Add(Dt * 1000.f);
	}
	if (MusicTestSwitches >= 10 || MusicTestT > 150.f)
	{
		auto Stat = [](TArray<float> V, float& Med, float& Max)
		{
			Med = Max = 0.f;
			if (V.Num() == 0) return;
			V.Sort();
			Med = V[V.Num() / 2];
			Max = V.Last();
		};
		float FMed, FMax, MMed, MMax, OMed, OMax;
		Stat(MusicTestFrameMs, FMed, FMax);
		Stat(MusicTestMusicMs, MMed, MMax);
		Stat(MusicTestOtherMs, OMed, OMax);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT music test %s: %d switches; switch frame median %.1f max %.1f ms; in music median %.2f max %.2f ms; other frames median %.1f ms; %s"),
			*LevelName, MusicTestFrameMs.Num(), FMed, FMax, MMed, MMax, OMed, MusicTestFrameMs.Num() >= 10 && MMax < 3.f ? TEXT("PASS") : TEXT("FAIL"));
		FGenericPlatformMisc::RequestExit(false);
		MusicTestSwitches = -1000;
		return;
	}
	if (MusicTestSwitches < 0 || MusicTestT < MusicTestNextAt || !MusicCues.IsValidIndex(MusicCue)) return;
	if (MusicAudio.IsValid())
	{
		MusicAudio->Stop();           // (the track run out: its voice ends a frame or more before the next cue)
		MusicAudio = nullptr;
		MusicTestNextAt = MusicTestT + 0.25f;
		return;
	}
	MusicEndReal = 0.0;
	++MusicTestSwitches;
	MusicTestNextAt = MusicTestT + 4.f;
}

void AOutlawsLevelActor::TickStutterProbe(float Dt)
{
	const float Before = StutterProbeT;
	StutterProbeT += Dt;
	if (!(Before < 8.f && StutterProbeT >= 8.f)) return;
	UWorld* World = GetWorld();
	const APawn* Pawn = World && World->GetFirstPlayerController() ? World->GetFirstPlayerController()->GetPawn() : nullptr;
	if (!Pawn || !AssetsRef) return;
	OutlawsPerf::Mark(TEXT("stutter probe"));      // (its frame and the next ones in hitches.csv after this row)
	const FVector Feet = Pawn->GetActorLocation() - FVector(0.0, 0.0, 90.0);
	auto Timed = [](const FString& What, TFunctionRef<void()> Fn)
	{
		const double T0 = FPlatformTime::Seconds();
		Fn();
		UE_LOG(LogOutlaws, Log, TEXT("OLRT probe %s: %.2f ms"), *What, (FPlatformTime::Seconds() - T0) * 1000.0);
	};
	TSet<FString> Drops;
	for (const TWeakObjectPtr<AOutlawsEnemy>& W : Enemies)
	{
		if (const AOutlawsEnemy* E = W.Get(); E && !E->Def.DropItem.IsEmpty()) Drops.Add(E->Def.DropItem.ToLower());
	}
	int32 k = 0;
	for (const FString& Name : Drops)
	{
		for (int32 n = 0; n < 3; ++n, ++k)
		{
			Timed(FString::Printf(TEXT("drop %s #%d"), *Name, n + 1), [&]() { SpawnGroundItem(Name, Feet + FVector(150.0 + 40.0 * k, 0.0, 0.0)); });
		}
	}
	Timed(TEXT("blast"), [&]() { Explode(Feet + FVector(2500.0, 0.0, 60.0)); });
	Timed(TEXT("scope"), [&]() { FIntPoint Size; SpxCell(TEXT("ijrifle.spx"), 6, Size); });
	if (AOutlawsPlayer* PP = Cast<AOutlawsPlayer>(const_cast<APawn*>(Pawn))) Timed(TEXT("scope rig (made already: 0)"), [&]() { PP->EnsureScopeRig(this); });
	const FString Next = MusicCues.Num() > 0 ? PeekNextMusicFile() : FString();
	if (!Next.IsEmpty() && FPaths::FileExists(Next))
	{
		Timed(TEXT("music read on the game thread ") + FPaths::GetCleanFilename(Next), [&]()
		{
			TArray<uint8> D;
			FWav W;
			if (FFileHelper::LoadFileToArray(D, *Next) && D.Num() > 12) ParseRiffWav(D, W);
		});
		Timed(TEXT("music start (async) ") + FPaths::GetCleanFilename(Next), [&]() { PrefetchMusic(Next); MusicReady(Next); });
	}
}
#endif // WITH_OUTLAWS_TESTS

void AOutlawsLevelActor::AlertEnemies(const FVector& Where, float RadiusCm)
{
	if (UOutlawsCoop::IsActive(this))
	{
		UOutlawsCoop::NoteNoise(this, Where, RadiusCm);      // (co-op: the host's bad guys hear both players' shots)
		return;
	}
	for (const TWeakObjectPtr<AOutlawsEnemy>& W : Enemies)
	{
		AOutlawsEnemy* E = W.Get();
		if (E && FVector::Dist(E->GetActorLocation(), Where) < RadiusCm)
		{
			E->HearShot(Where);
		}
	}
}

void AOutlawsLevelActor::OnEnemyKilled(AOutlawsEnemy* Enemy)
{
	if (Enemy && (Enemy->bNotBadGuy || Enemy->bBoss))
	{
		return;
	}
	++Kills;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT kills: %d"), Kills);
}

bool AOutlawsLevelActor::EnemyMayWorkDoor(int32 Door) const
{
	if (!Inf) return false;
	// bad guys carry no keys: a door with OBJECT: n stays shut for them.  (user 2026-10-04, BUG-BOSSDOOR: the train's boss
	// opened its doors) and, as the Jedi engine has it (a bad guy is a "smart object": infElevatorMessageInternal and the INF
	// links' entity mask, the player alone by default), a bad guy works only an automatic door (SECFLAG1_DOOR) or an INF item
	// whose ENTITY_MASK lets anyone (*: the cliff's, the mill's, the ranch's and others); the train's car doors are neither
	bool bTheirs = Inf->Live.Sectors.IsValidIndex(Door) && (Inf->Live.Sectors[Door].Flags & OutlawsFlags::SectorDoor) != 0;
	if (const TArray<int32>* Es = Inf->ElevatorsBySector.Find(Door))
	{
		for (int32 e : *Es)
		{
			const FOutlawsElevator& E = Inf->Elevators[e];
			if (Inf->Inf.Items[E.Item].Classes[E.ClassIndex].KeyObject >= 0) return false;
			for (const FOutlawsInfClass& C : Inf->Inf.Items[E.Item].Classes) bTheirs |= C.bAnyEntity;
		}
	}
	return bTheirs;
}

bool AOutlawsLevelActor::EnemyOpenDoor(const FVector& Eye, const FVector& Ahead, bool bPlayerRules)
{
	// (bPlayerRules, GATE-FLAKES from bots11: a multiplayer bot, a player the host runs: any door a player's push opens, but
	// none that wants a key, as it carries none.  town_2p's spawn room behind sector 40's door held a bot in many matches)
	// the bad guy's own sector is the near side of the door
	int32 From = -1;
	if (LevelData)
	{
		if (const FOutlawsSector* S = LevelData->SectorAt(FOutlawsGeometry::ToOutlaws(Eye, -4.0))) From = S->Index;
	}
	bool bClosed = false;
	const int32 Door = DoorAtHand(Eye, Ahead, bClosed, From);
	if (Door < 0 || !bClosed || !Inf)
	{
		return false;
	}
	bool bKeyed = false;
	if (const TArray<int32>* Es = bPlayerRules ? Inf->ElevatorsBySector.Find(Door) : nullptr)
	{
		for (int32 e : *Es) bKeyed |= Inf->Inf.Items[Inf->Elevators[e].Item].Classes[Inf->Elevators[e].ClassIndex].KeyObject >= 0;
	}
	if (bPlayerRules ? bKeyed : !EnemyMayWorkDoor(Door))
	{
		static TSet<int32> Said;
		if (!Said.Contains(Door)) { Said.Add(Door); UE_LOG(LogOutlaws, Log, TEXT("OLRT a bad guy at the door of sector %d: not his to open"), Door); }
		return false;
	}
	// (co-op: the host's INF is told the push is an enemy's, not a player's)
	const bool bTaken = UOutlawsCoop::IsActive(this) ? UOutlawsCoop::EnemyDoorEvent(this, Door, OutlawsInf::EvNudgeBack) : Inf->SectorEvent(Door, OutlawsInf::EvNudgeBack);
	if (bTaken)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT an enemy opens the door of sector %d"), Door);
	}
	return bTaken;
}

bool AOutlawsLevelActor::TryEnemyShot()
{
	const UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	if (Now < NextEnemyShot)
	{
		return false;
	}
	NextEnemyShot = Now + 0.05;
	LastDangerTime = Now;                   // (shot at: the autosave waits)
	return true;
}

bool AOutlawsLevelActor::IsTauntPlaying() const
{
	return TauntPlaying.IsValid() && TauntPlaying->IsPlaying();
}

bool AOutlawsLevelActor::TryTaunt(bool bForce)
{
	const UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	if (IsTauntPlaying() || (!bForce && (Now < NextTaunt || FMath::RandBool())))
	{
		return false;
	}
	NextTaunt = Now + 1.904 + FMath::FRandRange(0.f, 8.191f);
	return true;
}

FString AOutlawsLevelActor::TauntWav(const FString& Name) const
{
	const FString Stem = FPaths::GetBaseFilename(Name);
	if (!TauntSuffix.IsEmpty() && AssetsRef && AssetsRef->Has(Stem + TauntSuffix + TEXT(".wav")))
	{
		return Stem + TauntSuffix;
	}
	return Stem;
}

void AOutlawsLevelActor::ShareTargetNear(AOutlawsEnemy* Source, const FVector& Where, float RadiusCm)
{
	for (const TWeakObjectPtr<AOutlawsEnemy>& W : Enemies)
	{
		AOutlawsEnemy* E = W.Get();
		if (E && E != Source && FVector::Dist(E->GetActorLocation(), Where) < RadiusCm)
		{
			// (co-op host: a watcher it wakes goes after the same player)
			if (Source && Source->CoopRole == 1 && E->GetState() == EOutlawsEnemyState::Watch) E->CoopTarget = Source->CoopTarget;
			E->ShareTarget();
		}
	}
}

void AOutlawsLevelActor::BroadcastMeters(AOutlawsEnemy* Source, const FVector& Where, float RadiusCm, float Confidence, float Panic)
{
	for (const TWeakObjectPtr<AOutlawsEnemy>& W : Enemies)
	{
		AOutlawsEnemy* E = W.Get();
		if (E && E != Source && !E->IsDead() && FVector::Dist(E->GetActorLocation(), Where) < RadiusCm)
		{
			E->AdjustMeters(Confidence, Panic);
		}
	}
}

AOutlawsSpriteActor* AOutlawsLevelActor::MakeLooseSprite(const FString& Anim, const FVector& At)
{
	AOutlawsSpriteActor* S = MakeSprite(GetAtlas(Anim, false), TEXT("LOOSE"), At, 0.f, 31);
	if (S)
	{
		S->SetActorEnableCollision(false);
		SpawnedActors.Add(S);
	}
	return S;
}

FString AOutlawsLevelActor::DataFile(const TCHAR* Folder, const FString& File) const
{
	if (AssetsRef)
	{
		const FString Beside = FPaths::ConvertRelativePathToFull(FPaths::Combine(AssetsRef->GetRoot(), Folder, File));
		if (FPaths::FileExists(Beside)) return Beside;
	}
#if PLATFORM_WINDOWS
	// a PC: the converter's cache (olvr_install.py --pc, "Convert cutscenes.cmd" in the PC zip), where the music
	// decoded from the install's flac is kept too
	const FString Converted = FPaths::ConvertRelativePathToFull(FPaths::Combine(OutlawsMediaDir(), Folder, File));
	if (FPaths::FileExists(Converted)) return Converted;
#endif
	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectContentDir(), Folder, File));
}

bool AOutlawsLevelActor::PathStepToward(const FVector& FromFeet, const FVector& ToFeet, double MaxStepU, double HeightU, FVector& OutStep,
	FIntPoint* OutVia, const TArray<FIntPoint>* Avoid) const
{
	if (OutVia) *OutVia = FIntPoint(-1, -1);
	if (!Inf) return false;
	const FOutlawsLevel& Lv = Inf->Live;
	auto At = [&](const FVector& P) { return Lv.SectorAt(FOutlawsGeometry::ToOutlaws(P, 0.2)); };
	const FOutlawsSector* A = At(FromFeet);
	const FOutlawsSector* B = At(ToFeet);
	if (!A || !B) return false;
	if (A->Index == B->Index)
	{
		OutStep = ToFeet;
		return true;
	}
	// breadth first over the openings (the live level: doors as they stand now)
	const int32 N = Lv.Sectors.Num();
	TArray<int32> Prev, Via;
	Prev.Init(-1, N);
	Via.Init(-1, N);
	TArray<int32> Queue;
	Queue.Add(A->Index);
	Prev[A->Index] = A->Index;
	for (int32 q = 0; q < Queue.Num() && Prev[B->Index] < 0; ++q)
	{
		const FOutlawsSector& S = Lv.Sectors[Queue[q]];
		for (int32 w = 0; w < S.Walls.Num(); ++w)
		{
			const FOutlawsWall& W = S.Walls[w];
			const int32 Nx = W.Adjoin;
			if (!Lv.Sectors.IsValidIndex(Nx) || Prev[Nx] >= 0) continue;
			if (W.Flags1 & (OutlawsFlags::WallNoPass | OutlawsFlags::WallFence)) continue;
			if (Avoid && Avoid->Contains(FIntPoint(S.Index, w))) continue;        // (BUG-BOSSWALL: he could not get through it)
			const FOutlawsSector& T = Lv.Sectors[Nx];
			const double Lo = FMath::Max(S.Floor.Y, T.Floor.Y), Hi = FMath::Min(S.Ceil.Y, T.Ceil.Y);
			if (Hi - Lo < HeightU * 0.9 || T.Floor.Y - S.Floor.Y > MaxStepU) continue;
			Prev[Nx] = S.Index;
			Via[Nx] = w;
			Queue.Add(Nx);
		}
	}
	if (Prev[B->Index] < 0) return false;
	int32 Cur = B->Index;
	while (Prev[Cur] != A->Index) Cur = Prev[Cur];
	const FOutlawsWall& W = A->Walls[Via[Cur]];
	if (!A->Verts.IsValidIndex(W.V1) || !A->Verts.IsValidIndex(W.V2)) return false;
	if (OutVia) *OutVia = FIntPoint(A->Index, Via[Cur]);
	const FVector2D Mid = (A->Verts[W.V1] + A->Verts[W.V2]) * 0.5;
	// through the opening, up to two units on toward the next sector's middle
	const FOutlawsSector& T = Lv.Sectors[Cur];
	FVector2D C(0.0, 0.0);
	for (const FVector2D& V : T.Verts) C += V;
	C /= FMath::Max(1, T.Verts.Num());
	const FVector2D P = Mid + (C - Mid).GetSafeNormal() * FMath::Min(2.0, (C - Mid).Size() * 0.5);
	OutStep = FOutlawsGeometry::ToUE(P, T.Floor.Y);
	return true;
}

FString AOutlawsLevelActor::DebugSectorWalls(const FVector& P, double RadiusU) const
{
	if (!Inf) return TEXT("no level");
	const FOutlawsLevel& Lv = Inf->Live;
	const FOutlawsSector* S = Lv.SectorAt(FOutlawsGeometry::ToOutlaws(P, 0.2));
	if (!S) return TEXT("no sector");
	const FVector2D Q = FOutlawsGeometry::ToOutlawsPlan(P);
	FString Out = FString::Printf(TEXT("sector %d (floor %.2f ceil %.2f flags %u, %d walls) at %.2f %.2f:"), S->Index, S->Floor.Y, S->Ceil.Y, S->Flags, S->Walls.Num(), Q.X, Q.Y);
	for (int32 w = 0; w < S->Walls.Num(); ++w)
	{
		const FOutlawsWall& W = S->Walls[w];
		if (!S->Verts.IsValidIndex(W.V1) || !S->Verts.IsValidIndex(W.V2)) continue;
		const FVector2D A = S->Verts[W.V1], B = S->Verts[W.V2];
		const FVector2D AB = B - A;
		const double T = FMath::Clamp(FVector2D::DotProduct(Q - A, AB) / FMath::Max(AB.SizeSquared(), 1e-9), 0.0, 1.0);
		const double D = FVector2D::Distance(Q, A + AB * T);
		if (D > RadiusU) continue;
		FString Next;
		if (Lv.Sectors.IsValidIndex(W.Adjoin))
		{
			const FOutlawsSector& N = Lv.Sectors[W.Adjoin];
			Next = FString::Printf(TEXT(" to %d (floor %.2f ceil %.2f)"), N.Index, N.Floor.Y, N.Ceil.Y);
		}
		Out += FString::Printf(TEXT(" [w%d %.2f %.2f > %.2f %.2f, %.2f u off%s flags %u]"), w, A.X, A.Y, B.X, B.Y, D, *Next, W.Flags1);
	}
	return Out;
}

namespace
{
	/** REQ-MPBOTS: a sector of several separate floor plans (outer rings, each with its holes) is several places: each wall's
	 *  part (the outer ring it lies on, or for a hole's wall the smallest outer ring round the hole), worked out once a level. */
	struct FRouteSectorParts
	{
		TArray<int32> WallPart;          // per wall: its part (an outer ring's index), -1 a ring of fewer than three walls
		TArray<TArray<int32>> Rings;     // vertex rings (FOutlawsLevel::LoopsFromWallsEx)
		TArray<int32> RingPart;          // per ring: its part
		TArray<double> RingArea;
	};

	bool RouteInRing(const FOutlawsSector& S, const TArray<int32>& Ring, const FVector2D& P)
	{
		return OutlawsPolygon::InRing(S.Verts, Ring, P.X, P.Y, true);      // (edges Ring[i] -> Ring[i - 1], as it always ran)
	}

	const FRouteSectorParts& RouteSectorParts(const FOutlawsLevel& Lv, const FOutlawsSector& S)
	{
		static const FOutlawsLevel* For = nullptr;
		static int32 ForCount = -1;
		static TMap<int32, FRouteSectorParts> Cache;
		if (For != &Lv || ForCount != Lv.Sectors.Num()) { For = &Lv; ForCount = Lv.Sectors.Num(); Cache.Reset(); }
		if (const FRouteSectorParts* Got = Cache.Find(S.Index)) return *Got;
		FRouteSectorParts& P = Cache.Add(S.Index);
		TArray<TArray<int32>> RingWalls;
		FOutlawsLevel::LoopsFromWallsEx(S, P.Rings, RingWalls);
		P.WallPart.Init(-1, S.Walls.Num());
		P.RingPart.Init(-1, P.Rings.Num());
		P.RingArea.Init(0.0, P.Rings.Num());
		for (int32 r = 0; r < P.Rings.Num(); ++r) P.RingArea[r] = FMath::Abs(FOutlawsLevel::RingArea(S, P.Rings[r]));
		TArray<bool> bOuter;
		bOuter.Init(true, P.Rings.Num());
		for (int32 r = 0; r < P.Rings.Num(); ++r)
		{
			for (int32 o = 0; o < P.Rings.Num() && P.Rings.Num() > 1; ++o)
			{
				if (o != r && P.RingArea[o] > P.RingArea[r] && RouteInRing(S, P.Rings[o], S.Verts[P.Rings[r][0]])) { bOuter[r] = false; break; }
			}
		}
		for (int32 r = 0; r < P.Rings.Num(); ++r)
		{
			if (bOuter[r]) { P.RingPart[r] = r; continue; }
			double Best = TNumericLimits<double>::Max();
			for (int32 o = 0; o < P.Rings.Num(); ++o)
			{
				if (bOuter[o] && P.RingArea[o] < Best && RouteInRing(S, P.Rings[o], S.Verts[P.Rings[r][0]])) { Best = P.RingArea[o]; P.RingPart[r] = o; }
			}
		}
		for (int32 r = 0; r < RingWalls.Num(); ++r) for (const int32 w : RingWalls[r]) if (P.WallPart.IsValidIndex(w)) P.WallPart[w] = P.RingPart[r];
		return P;
	}

	/** GATE-FLAKES (from bots11): a wall of S that opens onto another sector but no body walks through: the floor beyond rises
	 *  over a body's step (WalkClear's 70 cm; a building's upper-floor windows onto the open ground, drygulch's, whose legs ran
	 *  straight through the building and turned a bot leaving its upper floor by the window back into the room), or the gap
	 *  there is under a body's height (a door's sector excepted: it opens). */
	bool RouteWallTooHigh(const FOutlawsLevel* Lv, const FOutlawsSector& S, const FOutlawsWall& W)
	{
		if (!Lv || !Lv->Sectors.IsValidIndex(W.Adjoin) || !S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) return false;
		const FOutlawsSector& T = Lv->Sectors[W.Adjoin];
		if (T.Flags & OutlawsFlags::SectorDoor) return false;
		const FVector2D M = (S.Verts[W.V1] + S.Verts[W.V2]) * 0.5;
		const double FS = Lv->SurfaceY(S, M, true), FT = Lv->SurfaceY(T, M, true);
		const double Gap = FMath::Min(Lv->SurfaceY(S, M, false), Lv->SurfaceY(T, M, false)) - FMath::Max(FS, FT);
		return FT - FS > 2.3 || Gap < 5.0;
	}

	/** The straight way from P to X inside the sector crosses one of its walls no body passes (not the wall Through it leaves
	 *  by): the way round inside it is longer than it looks (a room's partition, an L-shaped sector's corner).  bOut: any of
	 *  its walls, openings too (the leg leaves the sector there and runs on over sectors the way never looked at). */
	bool RouteLegBlocked(const FOutlawsSector& S, const FVector2D& P, const FVector2D& X, int32 Through, const FOutlawsLevel* Lv = nullptr, bool bOut = false)
	{
		const FVector2D D = X - P;
		for (int32 w = 0; w < S.Walls.Num(); ++w)
		{
			if (w == Through) continue;
			const FOutlawsWall& W = S.Walls[w];
			if (!bOut && W.Adjoin >= 0 && !(W.Flags1 & (OutlawsFlags::WallNoPass | OutlawsFlags::WallFence)) && !RouteWallTooHigh(Lv, S, W)) continue;
			if (!S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) continue;
			const FVector2D A = S.Verts[W.V1], E = S.Verts[W.V2] - A;
			const double Den = D.X * E.Y - D.Y * E.X;
			if (FMath::Abs(Den) < 1e-9) continue;
			const FVector2D AP = A - P;
			const double t = (AP.X * E.Y - AP.Y * E.X) / Den;
			const double u = (AP.X * D.Y - AP.Y * D.X) / Den;
			if (t > 0.01 && t < 0.99 && u > 0.001 && u < 0.999) return true;
		}
		return false;
	}

	/** An opening no body walks through: no-pass, a fence (bad guys and bots), a glass pane (its own mesh stops bodies, BuildPanes). */
	constexpr uint32 RouteShut = OutlawsFlags::WallNoPass | OutlawsFlags::WallFence | OutlawsFlags::WallShatter;
	/** (REQ-COLLECTBOX) the player's walk (bRoutePlayer): a fence stops only bad guys and bots. */
	constexpr uint32 RouteShutPlayer = OutlawsFlags::WallNoPass | OutlawsFlags::WallShatter;

	/** BUG-BOTPICKUP: an opening from S into T a body passes, and where: the floors and ceilings as they stand at the
	 *  crossing (a sloped floor or ceiling read where the way crosses, not at its hinge: town_2p's ramps up to its
	 *  porches and the ditch round its jail, drygulch's slopes), no step up over MaxStepU, tall enough unless bDoor.
	 *  Tried at the point nearest HerePos first, then along the opening (a ramp's side meets its neighbour at every
	 *  height, the right height somewhere along it).  OutLo: the higher floor there.  MaxDropU above 0: no deeper drop (a
	 *  roof's edge). */
	bool RouteOpening(const FOutlawsLevel& Lv, const FOutlawsSector& S, const FOutlawsSector& T, const FOutlawsWall& W, const FVector2D& HerePos,
		double MarginU, double MaxStepU, double HeightU, bool bDoor, FVector2D& OutX, double& OutLo, double MaxDropU = 0.0)
	{
		const FVector2D V1 = S.Verts[W.V1], V2 = S.Verts[W.V2];
		const double Len = (V2 - V1).Size();
		if (Len < 0.01) return false;
		const FVector2D Dir = (V2 - V1) / Len;
		const double In = FMath::Min(MarginU, Len * 0.5);
		const double Near = FMath::Clamp(FVector2D::DotProduct(HerePos - V1, Dir), In, Len - In);
		double Ts[8];
		int32 N = 0;
		Ts[N++] = Near;
		for (int32 k = 0; k <= 4; ++k) Ts[N++] = FMath::Lerp(In, Len - In, k / 4.0);
		double BestOff = TNumericLimits<double>::Max();
		bool bAny = false;
		for (int32 k = 0; k < N; ++k)
		{
			const FVector2D X = V1 + Dir * Ts[k];
			const double FS = Lv.SurfaceY(S, X, true), FT = Lv.SurfaceY(T, X, true);
			const double Lo = FMath::Max(FS, FT), Hi = FMath::Min(Lv.SurfaceY(S, X, false), Lv.SurfaceY(T, X, false));
			if (FT - FS > MaxStepU) continue;
			if (MaxDropU > 0.0 && FS - FT > MaxDropU) continue;          // (a drop the body will not walk off: MoveAlong's MAX_DROP)
			if (Hi - Lo < HeightU * 0.9 && !bDoor) continue;
			const double Off = FMath::Abs(Ts[k] - Near);
			if (Off < BestOff) { BestOff = Off; OutX = X; OutLo = Lo; bAny = true; }
			if (k == 0) break;
		}
		return bAny;
	}

	/** BUG-BOTPICKUP: the way between two points inside one sector round the walls inside it (town_2p's jail, one sector
	 *  of cells and a corridor, its partitions holes in it): over its corners, each a little off the corner on the open
	 *  side, shortest first.  The corners and which of them see each other worked out once a sector (a level's load). */
	struct FInSectorCorners
	{
		TArray<FVector2D> Pts;
		TArray<TArray<bool>> Sees;
	};

	const FInSectorCorners& InSectorCorners(const FOutlawsLevel& Lv, const FOutlawsSector& S, double OffU)
	{
		static const FOutlawsLevel* For = nullptr;
		static int32 ForCount = -1;
		static TMap<int32, FInSectorCorners> Cache;
		if (For != &Lv || ForCount != Lv.Sectors.Num()) { For = &Lv; ForCount = Lv.Sectors.Num(); Cache.Reset(); }
		if (const FInSectorCorners* Got = Cache.Find(S.Index)) return *Got;
		FInSectorCorners& C = Cache.Add(S.Index);
		// each corner of the walls no body passes: off it against the walls that meet there (a corner they run straight
		// through is no corner); kept when that place is inside the sector
		TArray<FVector2D> Away;
		Away.Init(FVector2D::ZeroVector, S.Verts.Num());
		TArray<int32> Meets;
		Meets.Init(0, S.Verts.Num());
		for (const FOutlawsWall& W : S.Walls)
		{
			if (W.Adjoin >= 0 && !(W.Flags1 & (OutlawsFlags::WallNoPass | OutlawsFlags::WallFence | OutlawsFlags::WallShatter)) && !RouteWallTooHigh(&Lv, S, W)) continue;
			if (!S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) continue;
			const FVector2D E = (S.Verts[W.V2] - S.Verts[W.V1]).GetSafeNormal();
			Away[W.V1] -= E;
			Away[W.V2] += E;
			++Meets[W.V1];
			++Meets[W.V2];
		}
		for (int32 v = 0; v < S.Verts.Num() && C.Pts.Num() < 160; ++v)
		{
			if (Meets[v] == 0 || Away[v].Size() < 0.3) continue;
			const FVector2D P = S.Verts[v] + Away[v].GetSafeNormal() * OffU;
			if (!Lv.ContainsXZ(S, P.X, P.Y)) continue;
			C.Pts.Add(P);
		}
		C.Sees.SetNum(C.Pts.Num());
		for (int32 a = 0; a < C.Pts.Num(); ++a) C.Sees[a].Init(false, C.Pts.Num());
		for (int32 a = 0; a < C.Pts.Num(); ++a)
		{
			for (int32 b = a + 1; b < C.Pts.Num(); ++b)
			{
				C.Sees[a][b] = C.Sees[b][a] = !RouteLegBlocked(S, C.Pts[a], C.Pts[b], -1, &Lv);
			}
		}
		return C;
	}

	/** The corners between From and To inside S (none when the straight way is clear, or no way round is found). */
	void InSectorWay(const FOutlawsLevel& Lv, const FOutlawsSector& S, const FVector2D& From, const FVector2D& To, double OffU, TArray<FVector2D>& Out)
	{
		Out.Reset();
		if (!RouteLegBlocked(S, From, To, -1, &Lv)) return;
		const FInSectorCorners& C = InSectorCorners(Lv, S, OffU);
		const int32 N = C.Pts.Num();
		if (N == 0) return;
		// nodes: the corners, then From (N), then To (N + 1)
		TArray<bool> FromSees, ToSees;
		FromSees.SetNum(N);
		ToSees.SetNum(N);
		for (int32 i = 0; i < N; ++i)
		{
			FromSees[i] = !RouteLegBlocked(S, From, C.Pts[i], -1, &Lv);
			ToSees[i] = !RouteLegBlocked(S, C.Pts[i], To, -1, &Lv);
		}
		auto Pt = [&](int32 i) { return i < N ? C.Pts[i] : i == N ? From : To; };
		auto Sees = [&](int32 a, int32 b)
		{
			if (a > b) Swap(a, b);
			if (b < N) return C.Sees[a][b];
			if (a == N) return b == N + 1 ? false : FromSees[b];
			return a < N ? (b == N ? FromSees[a] : ToSees[a]) : false;
		};
		TArray<double> Dist;
		TArray<int32> Prev;
		TArray<bool> Done;
		Dist.Init(TNumericLimits<double>::Max(), N + 2);
		Prev.Init(-1, N + 2);
		Done.Init(false, N + 2);
		Dist[N] = 0.0;
		for (int32 Step = 0; Step < N + 2; ++Step)
		{
			int32 U = -1;
			for (int32 i = 0; i < N + 2; ++i) if (!Done[i] && Dist[i] < TNumericLimits<double>::Max() && (U < 0 || Dist[i] < Dist[U])) U = i;
			if (U < 0 || U == N + 1) break;
			Done[U] = true;
			for (int32 i = 0; i < N + 2; ++i)
			{
				if (Done[i] || i == U || !Sees(U, i)) continue;
				const double D = Dist[U] + (Pt(U) - Pt(i)).Size();
				if (D < Dist[i]) { Dist[i] = D; Prev[i] = U; }
			}
		}
		if (Prev[N + 1] < 0) return;
		for (int32 Cur = Prev[N + 1]; Cur >= 0 && Cur != N; Cur = Prev[Cur]) Out.Add(C.Pts[Cur]);
		Algo::Reverse(Out);
	}

	/** The part of the sector a point stands in (the smallest outer ring round it); -1 when the sector is one part, or none holds it. */
	int32 RouteSectorPartAt(const FOutlawsLevel& Lv, const FOutlawsSector& S, const FVector2D& Pt)
	{
		const FRouteSectorParts& P = RouteSectorParts(Lv, S);
		int32 Outers = 0, Best = -1;
		double BestArea = TNumericLimits<double>::Max();
		for (int32 r = 0; r < P.Rings.Num(); ++r)
		{
			if (P.RingPart[r] != r) continue;
			++Outers;
			if (P.RingArea[r] < BestArea && RouteInRing(S, P.Rings[r], Pt)) { BestArea = P.RingArea[r]; Best = r; }
		}
		return Outers > 1 ? Best : -1;
	}
}

bool AOutlawsLevelActor::PathRoute(const FVector& FromFeet, const FVector& ToFeet, double MaxStepU, double HeightU, double MarginU, TArray<FVector>& OutPoints, double& OutLenCm, double MaxDropU) const
{
	// REQ-MPBOTS: the whole way through the level's sectors, shortest first (Dijkstra over their openings: an adjoin or a
	// double adjoin, not no-pass or fence, tall enough for the body unless it is a door the body may work, no step up over
	// MaxStepU).  Each sector entered at the point of its opening nearest where the way came from, kept MarginU in from the
	// opening's ends so a body passes; a sector of several separate floor plans is left only by the part it was entered in.
	// The points are those crossings, then ToFeet.
	OutPoints.Reset();
	OutLenCm = 0.0;
	if (!Inf) return false;
	const FOutlawsLevel& Lv = Inf->Live;
	const double U = FOutlawsGeometry::UnitsToCm;
	auto At = [&](const FVector& P) { return Lv.SectorAt(FOutlawsGeometry::ToOutlaws(P, 0.2)); };
	const FOutlawsSector* A = At(FromFeet);
	const FOutlawsSector* B = At(ToFeet);
	if (!A || !B) return false;
	const FVector2D From2 = FOutlawsGeometry::ToOutlawsPlan(FromFeet), To2 = FOutlawsGeometry::ToOutlawsPlan(ToFeet);
	const int32 FromPart = RouteSectorPartAt(Lv, *A, From2);
	const int32 ToPart = RouteSectorPartAt(Lv, *B, To2);
	// (each leg inside one sector: round the walls inside it, InSectorWay)
	FVector Last = FromFeet;
	TArray<FVector2D> Round;
	auto Leg = [&](const FOutlawsSector& In, const FVector& P)
	{
		// (GATE-FLAKES, from bots11: the corners 90 cm off at least: a wall's own body is thicker than its line, and a leg on
		// from a corner 54 cm off it caught the wall's end and held the bot sliding along it)
		InSectorWay(Lv, In, FOutlawsGeometry::ToOutlawsPlan(Last), FOutlawsGeometry::ToOutlawsPlan(P), FMath::Max(MarginU * 1.2, 3.0), Round);
		for (const FVector2D& C : Round)
		{
			const FVector Q = FOutlawsGeometry::ToUE(C, Lv.SurfaceY(In, C, true));
			OutLenCm += FVector::Dist(Last, Q);
			OutPoints.Add(Q);
			Last = Q;
		}
		OutLenCm += FVector::Dist(Last, P);
		OutPoints.Add(P);
		Last = P;
	};
	if (A->Index == B->Index && FromPart == ToPart)
	{
		Leg(*A, ToFeet);
		return true;
	}
	// (a node: a sector and the part of it the way is in; most sectors are one part)
	struct FNode { int32 S; int32 Part; };
	TArray<FNode> Nodes;
	TMap<uint64, int32> NodeOf;
	auto NodeFor = [&](int32 S, int32 Part) -> int32
	{
		const uint64 K = (uint64(uint32(S)) << 32) | uint64(uint32(Part + 1));
		if (const int32* Got = NodeOf.Find(K)) return *Got;
		const int32 i = Nodes.Add({ S, Part });
		NodeOf.Add(K, i);
		return i;
	};
	TArray<double> Dist;
	TArray<int32> Prev;
	TArray<FVector2D> Pos;
	TArray<double> PosZ;
	auto Grow = [&]() { while (Dist.Num() < Nodes.Num()) { Dist.Add(TNumericLimits<double>::Max()); Prev.Add(-1); Pos.Add(FVector2D::ZeroVector); PosZ.Add(0.0); } };
	struct FQ { double D; int32 N; bool operator<(const FQ& O) const { return D < O.D; } };
	TArray<FQ> Heap;
	const int32 Start = NodeFor(A->Index, FromPart);
	Grow();
	Dist[Start] = 0.0;
	Pos[Start] = From2;
	PosZ[Start] = FromFeet.Z / U;
	Heap.HeapPush({ 0.0, Start });
	int32 Goal = -1, Popped = 0;
	while (Heap.Num() > 0)
	{
		FQ Q;
		Heap.HeapPop(Q);
		if (Q.D > Dist[Q.N]) continue;
		const FNode Here = Nodes[Q.N];
		if (Here.S == B->Index && (ToPart < 0 || Here.Part < 0 || Here.Part == ToPart)) { Goal = Q.N; break; }
		if (++Popped > 40000) break;
		const FOutlawsSector& S = Lv.Sectors[Here.S];
		const FRouteSectorParts& SP = RouteSectorParts(Lv, S);
		const FVector2D HerePos = Pos[Q.N];
		const double HereZ = PosZ[Q.N];
		for (int32 w = 0; w < S.Walls.Num(); ++w)
		{
			const FOutlawsWall& W = S.Walls[w];
			if (W.Flags1 & (bRoutePlayer ? RouteShutPlayer : RouteShut)) continue;
			if (Here.Part >= 0 && SP.WallPart.IsValidIndex(w) && SP.WallPart[w] != Here.Part) continue;      // (another part of this sector)
			if (!S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) continue;
			for (int32 k = 0; k < 2; ++k)
			{
				const int32 Nx = k == 0 ? W.Adjoin : W.DAdjoin;
				const int32 Mw = k == 0 ? W.Mirror : W.DMirror;
				if (!Lv.Sectors.IsValidIndex(Nx) || Nx == Here.S) continue;
				const FOutlawsSector& T = Lv.Sectors[Nx];
				// the crossing: the opening's point nearest where this sector was entered, MarginU in from either end, where the
				// body passes (the floors and ceilings there; a door shut now is opened on the way)
				FVector2D X;
				double Lo = 0.0;
				if (T.Walls.IsValidIndex(Mw) && (T.Walls[Mw].Flags1 & (bRoutePlayer ? RouteShutPlayer : RouteShut))) continue;          // (shut from the far side)
				if (!RouteOpening(Lv, S, T, W, HerePos, MarginU, MaxStepU, HeightU, (bRoutePlayer ? RoutePlayerDoor(Nx) || RoutePlayerDoor(Here.S) : EnemyMayWorkDoor(Nx) || EnemyMayWorkDoor(Here.S)), X, Lo, MaxDropU)) continue;
				const FRouteSectorParts& TP = RouteSectorParts(Lv, T);
				const int32 TPart = TP.Rings.Num() > 1 && TP.WallPart.IsValidIndex(Mw) ? TP.WallPart[Mw] : -1;
				const int32 Nn = NodeFor(Nx, TPart);
				Grow();
				// (a leg through a wall of this sector: dear, so a way with clear legs is taken where there is one.  BUG-BOTPICKUP2:
				// out through another of its openings too: town_2p's L-shaped street sector 151, whose leg from its east end to
				// its north side cut over sector 150 and held the bots on a wall there for a minute and more)
				const double D = Q.D + (X - HerePos).Size() + FMath::Abs(Lo - HereZ) * 0.5 + (RouteLegBlocked(S, HerePos, X, w, &Lv, true) ? 100.0 : 0.0);
				if (D < Dist[Nn])
				{
					Dist[Nn] = D;
					Prev[Nn] = Q.N;
					Pos[Nn] = X;
					PosZ[Nn] = Lo;
					Heap.HeapPush({ D, Nn });
				}
			}
		}
	}
	if (Goal < 0) return false;
	TArray<int32> Chain;
	for (int32 Cur = Goal; Cur != Start && Cur >= 0 && Chain.Num() <= Nodes.Num(); Cur = Prev[Cur]) Chain.Add(Cur);
	Algo::Reverse(Chain);
	int32 In = A->Index;
	for (const int32 Nn : Chain)
	{
		Leg(Lv.Sectors[In], FOutlawsGeometry::ToUE(Pos[Nn], PosZ[Nn]));
		In = Nodes[Nn].S;
	}
	Leg(*B, ToFeet);
	return true;
}

bool AOutlawsLevelActor::PathReachPoint(const FVector& Thing, double ReachCm, double ReachUpCm, double HeightU, FVector& OutFeet) const
{
	if (!Inf) return false;
	const FOutlawsLevel& Lv = Inf->Live;
	const double U = FOutlawsGeometry::UnitsToCm;
	const FOutlawsSector* B = Lv.SectorAt(FOutlawsGeometry::ToOutlaws(Thing, 0.2));
	if (!B) return false;
	const FVector2D P = FOutlawsGeometry::ToOutlawsPlan(Thing);
	const double ThingZ = Thing.Z / U;
	double Best = TNumericLimits<double>::Max();
	for (const FOutlawsWall& W : B->Walls)
	{
		if (W.Flags1 & RouteShut) continue;
		if (!B->Verts.IsValidIndex(W.V1) || !B->Verts.IsValidIndex(W.V2)) continue;
		for (const int32 Nx : { W.Adjoin, W.DAdjoin })
		{
			if (!Lv.Sectors.IsValidIndex(Nx) || Nx == B->Index) continue;
			const FVector2D V1 = B->Verts[W.V1], V2 = B->Verts[W.V2];
			const double Len = (V2 - V1).Size();
			if (Len < 0.01) continue;
			const FVector2D Dir = (V2 - V1) / Len;
			const FVector2D X = V1 + Dir * FMath::Clamp(FVector2D::DotProduct(P - V1, Dir), 0.0, Len);
			const double Across = (X - P).Size();
			if (Across * U > ReachCm) continue;
			// a step into the neighbour, square off the opening (away from the thing's side)
			FVector2D Out(-Dir.Y, Dir.X);
			if (FVector2D::DotProduct(Out, P - X) > 0.0) Out = -Out;
			const FVector2D Stand = X + Out * 1.0;
			// (the floor there at or below the thing's: the neighbour, or one past it when the neighbour is a narrow step)
			const FOutlawsSector* On = Lv.SectorAt(Stand.X, ThingZ + 0.2, Stand.Y);
			if (!On || On == B) continue;
			const double F = Lv.SurfaceY(*On, Stand, true);
			if (Lv.SurfaceY(*On, Stand, false) - F < HeightU * 0.9) continue;
			if (FMath::Abs(ThingZ - F) * U > ReachUpCm) continue;
			if (Across < Best)
			{
				Best = Across;
				OutFeet = FOutlawsGeometry::ToUE(Stand, F);
			}
		}
	}
	return Best < TNumericLimits<double>::Max();
}

FString AOutlawsLevelActor::PathRouteWhy(const FVector& FromFeet, const FVector& ToFeet, double MaxStepU, double HeightU, double MaxDropU) const
{
	if (!Inf) return TEXT("no level");
	const FOutlawsLevel& Lv = Inf->Live;
	const double U = FOutlawsGeometry::UnitsToCm;
	auto At = [&](const FVector& P) { return Lv.SectorAt(FOutlawsGeometry::ToOutlaws(P, 0.2)); };
	const FOutlawsSector* A = At(FromFeet);
	const FOutlawsSector* B = At(ToFeet);
	FString Out = FString::Printf(TEXT("from sector %d (cull %d, floor %.1f) to sector %d (cull %d, floor %.1f), feet z %.1f / %.1f u"),
		A ? A->Index : -1, CullSectorOf(FromFeet + FVector(0.0, 0.0, 10.0)), A ? A->Floor.Y : 0.0, B ? B->Index : -1, CullSectorOf(ToFeet + FVector(0.0, 0.0, 10.0)), B ? B->Floor.Y : 0.0, FromFeet.Z / U, ToFeet.Z / U);
	if (!A || !B) return Out;
	TArray<bool> Seen;
	Seen.Init(false, Lv.Sectors.Num());
	TArray<int32> Queue;
	Queue.Add(A->Index);
	Seen[A->Index] = true;
	for (int32 q = 0; q < Queue.Num(); ++q)
	{
		const FOutlawsSector& S = Lv.Sectors[Queue[q]];
		for (const FOutlawsWall& W : S.Walls)
		{
			if (W.Flags1 & (bRoutePlayer ? RouteShutPlayer : RouteShut)) continue;
			for (const int32 Nx : { W.Adjoin, W.DAdjoin })
			{
				if (!Lv.Sectors.IsValidIndex(Nx) || Seen[Nx]) continue;
				const FOutlawsSector& T = Lv.Sectors[Nx];
				if (!S.Verts.IsValidIndex(W.V1) || !S.Verts.IsValidIndex(W.V2)) continue;
				FVector2D X;
				double Lo = 0.0;
				if (!RouteOpening(Lv, S, T, W, S.Verts[W.V1], 1.5, MaxStepU, HeightU, (bRoutePlayer ? RoutePlayerDoor(Nx) || RoutePlayerDoor(S.Index) : EnemyMayWorkDoor(Nx) || EnemyMayWorkDoor(S.Index)), X, Lo, MaxDropU)) continue;
				Seen[Nx] = true;
				Queue.Add(Nx);
			}
		}
	}
	Out += FString::Printf(TEXT("; flood reached %d of %d sectors (goal %s)"), Queue.Num(), Lv.Sectors.Num(), Seen[B->Index] ? TEXT("reached") : TEXT("not reached"));
	// each opening into the goal sector, seen from the far side
	int32 Shown = 0;
	for (const FOutlawsWall& W : B->Walls)
	{
		for (const int32 Nx : { W.Adjoin, W.DAdjoin })
		{
			if (!Lv.Sectors.IsValidIndex(Nx) || ++Shown > 10) continue;
			const FOutlawsSector& T = Lv.Sectors[Nx];
			if (!B->Verts.IsValidIndex(W.V1) || !B->Verts.IsValidIndex(W.V2)) continue;
			// (at the opening's middle, slopes read there)
			const FVector2D M = (B->Verts[W.V1] + B->Verts[W.V2]) * 0.5;
			const double FB = Lv.SurfaceY(*B, M, true), FT = Lv.SurfaceY(T, M, true);
			const double Lo = FMath::Max(FB, FT), Hi = FMath::Min(Lv.SurfaceY(*B, M, false), Lv.SurfaceY(T, M, false));
			Out += FString::Printf(TEXT(" | from %d '%s'%s: floor %.1f (%.1f here) ceil %.1f, step up %.1f, gap %.1f, flags %x / sector %x%s%s"), Nx, *T.Name, Seen[Nx] ? TEXT(" (reached)") : TEXT(""), T.Floor.Y, FT, T.Ceil.Y,
				FB - FT, Hi - Lo, W.Flags1, T.Flags, EnemyMayWorkDoor(Nx) ? TEXT(" door-near") : TEXT(""), EnemyMayWorkDoor(B->Index) ? TEXT(" door-goal") : TEXT(""));
		}
	}
	Out += FString::Printf(TEXT(" | goal '%s' floor %.1f ceil %.1f flags %x, max step %.1f, height %.1f"), *B->Name, B->Floor.Y, B->Ceil.Y, B->Flags, MaxStepU, HeightU);
	return Out;
}

UTexture2D* AOutlawsLevelActor::SpxCell(const FString& Spx, int32 Cell, FIntPoint& OutSize)
{
	const FString Key = FString::Printf(TEXT("%s#%d"), *Spx.ToLower(), Cell);
	if (TObjectPtr<UTexture2D>* Hit = SpxCells.Find(Key))
	{
		if (*Hit) OutSize = SpxCellSizes.FindRef(Key);
		return *Hit;
	}
	UTexture2D* T = nullptr;
	TArray<uint8> Data;
	TArray<OutlawsFormats::FSprxFrame> Frames;
	if (FApp::CanEverRender() && AssetsRef && AssetsRef->Get(Spx, Data) && OutlawsFormats::ParseSprx(Data, Frames) && Frames.IsValidIndex(Cell))
	{
		const OutlawsFormats::FSprxFrame& F = Frames[Cell];
		const int32 NumLayers = 1 + ((F.Flags & 1) ? 1 : 0) + ((F.Flags & 2) ? 1 : 0);
		const int32 PerLayer = (F.End - F.Offset) / NumLayers;
		if (F.Format == OutlawsFormats::SprxBC7 && F.W > 0 && F.H > 0 && F.Offset >= 0 && F.Offset + PerLayer <= Data.Num())
		{
			T = OutlawsTextures::MakeTextureBC7(F.W, F.H, F.Mips, Data.GetData() + F.Offset, PerLayer, TF_Trilinear);
			SpxCellSizes.Add(Key, FIntPoint(F.W, F.H));
		}
	}
	SpxCells.Add(Key, T);
	if (T) OutSize = SpxCellSizes.FindRef(Key);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT %s cell %d: %s"), *Spx, Cell, T ? *FString::Printf(TEXT("%dx%d"), T->GetSizeX(), T->GetSizeY()) : TEXT("not there"));
	return T;
}

bool AOutlawsLevelActor::SpriteIcon(const FString& Nwx, UTexture*& OutTex, FVector2D& OutScale, FVector2D& OutOffset, float& OutAspect)
{
	const TSharedPtr<OutlawsTextures::FSpriteAtlas> A = GetAtlas(Nwx, false);
	if (!A || !A->Texture || A->CellW <= 0 || A->CellH <= 0)
	{
		return false;
	}
	UTexture* Hd = bHDTextures ? SpriteHdFor(A) : nullptr;
	OutTex = Hd ? Hd : A->Texture;
	const int32 Cols = FMath::Max(1, A->Cols), Rows = FMath::Max(1, A->Rows);
	const int32 Cell = A->Grid.Num() > 0 ? FMath::Clamp(A->Grid[0], 0, Cols * Rows - 1) : 0;
	OutScale = FVector2D(1.0 / Cols, 1.0 / Rows);
	OutOffset = FVector2D(double(Cell % Cols) / Cols, double(Cell / Cols) / Rows);
	OutAspect = float(A->CellW) / float(A->CellH);
	return true;
}

// ---- far and unseen (user 2026-10-03): the level's part of AOutlawsEnemy::UpdateFarFreeze ----

bool AOutlawsLevelActor::IsSectorSeen(int32 Sector) const
{
	// no flood yet (the level's first ticks, culling off): everything counts as seen, so nobody freezes by mistake
	if (Sector < 0 || Sector >= VisibleSectors.Num() || SeenFromEye.Num() == 0) return true;
	return VisibleSectors[Sector];
}

bool AOutlawsLevelActor::IsCarriedBySector(const AActor* A) const
{
	const FCarry* C = Carries.Find(const_cast<AActor*>(A));
	return C && !C->Vel.IsNearlyZero();
}

void AOutlawsLevelActor::NoteFarFreeze(int32 Delta)
{
	FarFrozen = FMath::Max(0, FarFrozen + Delta);
	FarFrozenPeak = FMath::Max(FarFrozenPeak, FarFrozen);
	// a line once the count has moved by a fifth of the living (five at least) since the last one
	int32 Living = 0;
	for (const TWeakObjectPtr<AOutlawsEnemy>& W : Enemies) if (const AOutlawsEnemy* E = W.Get(); E && !E->IsDead() && !E->IsDormant()) ++Living;
	if (FMath::Abs(FarFrozen - FarFrozenLogged) >= FMath::Max(5, Living / 5))
	{
		FarFrozenLogged = FarFrozen;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT far freeze (%s): %d of %d living bad guys hold still"), *LevelName, FarFrozen, Living);
	}
}

void AOutlawsLevelActor::LogFarSummary()
{
	if (FarFrozenTime > 1.0 && Enemies.Num() > 0)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT far freeze summary (%s): %.1f of %d bad guys frozen on average over %.0f s (peak %d), about %.0f enemy ticks spared; %d far sounds skipped"),
			*LevelName, FarFrozenSum / FarFrozenTime, Enemies.Num(), FarFrozenTime, FarFrozenPeak, FarFrozenTicks, FarSoundsSkipped);
	}
	FarFrozen = FarFrozenLogged = FarFrozenPeak = 0;
	FarFrozenSum = FarFrozenTicks = FarFrozenTime = 0.0;
	FarSoundsSkipped = 0;
}
