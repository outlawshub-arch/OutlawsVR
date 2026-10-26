// Room sound (R13, user 2026-10-03: "realistic delay and reverberation to enemies who taunt in an open space compared to a
// closed space. The mine level is prime for this. Don't overdo it."): each sector is classed OPEN, ROOM or CAVE once per
// level; one reverb bus (a submix with the engine's reverb) takes the preset of the class the listener stands in,
// cross-faded over half a second; every positioned game sound sends to it by the class it comes from, drier when near;
// gunshots, taunts and blasts in the open get one soft, muffled echo off a wall or cliff face behind them.  ol.ReverbScale.
#include "OutlawsLevelActor.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevel.h"
#include "OutlawsGeometry.h"
#include "Components/AudioComponent.h"
#include "Sound/SoundSubmix.h"
#include "SubmixEffects/AudioMixerSubmixEffectReverb.h"
#include "AudioDevice.h"
#include "AudioThread.h"
#include "Misc/CommandLine.h"
#include "UObject/UObjectHash.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Camera/PlayerCameraManager.h"
#include "TimerManager.h"
#include "OutlawsCoop.h"
#include "OutlawsMpHost.h"
#include "OutlawsPlayer.h"
#include "EngineUtils.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"

namespace
{
	TAutoConsoleVariable<float> CVarOLReverbScale(TEXT("ol.ReverbScale"), 1.f,
		TEXT("Room sound (R13): the reverb's and the open-air echo's amount, 0 (off) to 2 (twice); 1 as built."));

	enum : uint8 { ClassOpen = 0, ClassRoom = 1, ClassCave = 2 };
	const TCHAR* ClassName(uint8 C) { return C == ClassOpen ? TEXT("open") : C == ClassRoom ? TEXT("room") : TEXT("cave"); }

	// the brief's starting points, for the user's ear: decay (s), wet (linear), pre-delay (s), the tail's high-frequency gain
	struct FPreset { float Decay, Wet, PreDelay, GainHF; };
	// (user 2026-10-03, BUG-REVERB: "not strong enough for me to be able to tell") about 8 dB wetter and longer than the
	// brief's starting points
	const FPreset Presets[3] = {
		{ 0.6f, 0.15f, 0.005f, 0.90f },       // open: a little air (-16 dB)
		{ 1.3f, 0.50f, 0.012f, 0.80f },       // room: -6 dB
		{ 3.0f, 0.80f, 0.050f, 0.45f },       // cave: -2 dB, a pre-delay, the tail low-passed
	};
	const float SendByClass[3] = { 0.2f, 0.8f, 1.0f };

	FSubmixEffectReverbSettings SettingsFor(const FPreset& P, float Scale)
	{
		FSubmixEffectReverbSettings S;
		S.DecayTime = P.Decay;
		S.ReflectionsDelay = P.PreDelay;
		S.LateDelay = FMath::Min(0.1f, P.PreDelay + 0.01f);
		S.GainHF = P.GainHF;
		S.DecayHFRatio = FMath::Clamp(P.GainHF, 0.1f, 2.f);
		S.Density = 0.85f;
		S.Diffusion = 0.85f;
		S.WetLevel = FMath::Clamp(P.Wet * Scale, 0.f, 10.f);
		S.DryLevel = 0.f;              // (the bus hears only the sends: the dry sound goes straight to the master)
		return S;
	}
}

uint8 AOutlawsLevelActor::RoomClassOf(int32 Sector)
{
	const FOutlawsLevel* Lv = LiveLevel();
	if (!Lv) return ClassOpen;
	if (RoomClasses.Num() != Lv->Sectors.Num() || RoomClassLevel != LevelName)
	{
		// once per level: the sky overhead (or a ceiling over 12 m) is OPEN; enclosed, under ~150 m2 or a ceiling under 5 m a
		// ROOM; enclosed and large a CAVE (the mine's caverns, a hall)
		RoomClassLevel = LevelName;
		RoomClasses.SetNum(Lv->Sectors.Num());
		int32 N[3] = { 0, 0, 0 };
		// (the per-level override the brief allows: the mine is rock tunnels and caverns, "prime for this")
		const bool bRockLevel = LevelName.Equals(TEXT("miner"), ESearchCase::IgnoreCase);
		const double U = FOutlawsGeometry::UnitsToCm / 100.0;      // metres a unit
		for (int32 i = 0; i < Lv->Sectors.Num(); ++i)
		{
			const FOutlawsSector& S = Lv->Sectors[i];
			double Area = 0.0;
			for (int32 k = 0; k < S.Verts.Num(); ++k)
			{
				const FVector2D& A = S.Verts[k];
				const FVector2D& B = S.Verts[(k + 1) % S.Verts.Num()];
				Area += A.X * B.Y - B.X * A.Y;
			}
			const double AreaM2 = FMath::Abs(Area) * 0.5 * U * U;
			const double HeightM = (S.Ceil.Y - S.Floor.Y) * U;
			uint8 C = ClassCave;
			if ((S.Flags & OutlawsFlags::ExtCeiling) != 0 || HeightM > 12.0 || S.NoWalls()) C = ClassOpen;
			else if (bRockLevel) C = ClassCave;                     // (the mine's tunnels are rock however narrow)
			else if (AreaM2 < 150.0 || HeightM < 5.0) C = ClassRoom;
			RoomClasses[i] = C;
			++N[C];
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLRT room sound for %s: %d open, %d room, %d cave sectors"), *LevelName, N[ClassOpen], N[ClassRoom], N[ClassCave]);
	}
	return RoomClasses.IsValidIndex(Sector) ? RoomClasses[Sector] : uint8(ClassOpen);
}

bool AOutlawsLevelActor::EnsureRoomSound()
{
	if (RoomReverb && RoomSubmix) return true;
	UWorld* W = GetWorld();
	FAudioDeviceHandle AD = W ? W->GetAudioDevice() : FAudioDeviceHandle();
	if (!AD) return false;
	RoomReverb = NewObject<USubmixEffectReverbPreset>(this);
	RoomSubmix = NewObject<USoundSubmix>(this);
	RoomSubmix->SubmixEffectChain.Add(RoomReverb);
	AD->RegisterSoundSubmix(RoomSubmix, true);
	RoomClassNow = 255;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT room sound: the reverb bus made"));
	return true;
}

void AOutlawsLevelActor::ReleaseRoomSound()
{
	// (RC1, the Quest's host crash: LoadMap asserted "!IsRooted()" tearing down the world.  The mixer's LoadSoundSubmix
	// roots the bus when it is registered and its unload never unroots it, so after any level with room sound the bus,
	// outered to this actor, stayed rooted until the next map load found it)
	if (!RoomSubmix) return;
	UWorld* W = GetWorld();
	if (FAudioDeviceHandle AD = W ? W->GetAudioDevice() : FAudioDeviceHandle()) AD->UnregisterSoundSubmix(RoomSubmix);
	// (the register and the unregister run on the audio thread: both done before the root comes off)
	FAudioCommandFence Fence;
	Fence.BeginFence();
	Fence.Wait();
	const bool bWasRooted = RoomSubmix->IsRooted();
	RoomSubmix->RemoveFromRoot();
	if (RoomReverb) RoomReverb->RemoveFromRoot();
	UE_LOG(LogOutlaws, Log, TEXT("OLRT room sound: the reverb bus let go (it was %srooted)"), bWasRooted ? TEXT("") : TEXT("not "));
	RoomSubmix = nullptr;
	RoomReverb = nullptr;
	RoomClassNow = 255;
}

void AOutlawsLevelActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ReleaseRoomSound();
	DisarmMusic();                // (a next track made ahead and never started)
	Super::EndPlay(EndPlayReason);
	// (whatever is still rooted inside the world now asserts in the map load that follows: named, and let go, RC1's net
	// for anything else the engine roots while in use; the map load runs every end play before it marks the world)
	if (UWorld* W = GetWorld()) LogRootedWorldObjects(W, TEXT("level actor end play"), true);
}

int32 AOutlawsLevelActor::LogRootedWorldObjects(UWorld* World, const TCHAR* When, bool bUnroot)
{
	if (!World) return 0;
	static const bool bDiag = OLFlags::Param(TEXT("OLRootDiag"));
	TArray<UObject*> Rooted;
	int32 Seen = 0;
	ForEachObjectWithOuter(World, [&Rooted, &Seen](UObject* Object)
	{
		++Seen;
		if (Object && Object->IsRooted()) Rooted.Add(Object);
	}, true, RF_NoFlags, EInternalObjectFlags::Garbage);
	if (bDiag || Rooted.Num() > 0)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLROOT %s: %d rooted world objects (of %d inside %s)"), When, Rooted.Num(), Seen, *World->GetName());
		for (UObject* O : Rooted)
		{
			UE_LOG(LogOutlaws, Warning, TEXT("OLROOT   rooted: %s%s"), *O->GetFullName(), bUnroot ? TEXT(" (taken off the root)") : TEXT(""));
			if (bUnroot) O->RemoveFromRoot();
		}
	}
	return Rooted.Num();
}

void AOutlawsLevelActor::UpdateRoomSound(float Dt)
{
	if (bFrontEnd || !LiveLevel()) return;
	const float Scale = FMath::Clamp(CVarOLReverbScale.GetValueOnGameThread(), 0.f, 2.f);
	const uint8 Want = RoomClassOf(PlayerSector);
	if (Want != RoomClassLogged)
	{
		RoomClassLogged = Want;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT room sound: the listener is in the %s now (sector %d)"), ClassName(Want), PlayerSector);
	}
	if (!EnsureRoomSound()) return;
	if (Want != RoomClassNow || !FMath::IsNearlyEqual(Scale, RoomScaleNow))
	{
		// a new space (or a new amount): from wherever the fade has got to, toward the new preset over half a second
		const float A = RoomClassNow < 3 ? FMath::Clamp(RoomFade, 0.f, 1.f) : 1.f;
		RoomFrom = RoomClassNow < 3 ? FVector4f(FMath::Lerp(RoomFrom.X, Presets[RoomClassNow].Decay, A), FMath::Lerp(RoomFrom.Y, Presets[RoomClassNow].Wet * RoomScaleNow, A),
			FMath::Lerp(RoomFrom.Z, Presets[RoomClassNow].PreDelay, A), FMath::Lerp(RoomFrom.W, Presets[RoomClassNow].GainHF, A))
			: FVector4f(Presets[Want].Decay, Presets[Want].Wet * Scale, Presets[Want].PreDelay, Presets[Want].GainHF);
		RoomClassNow = Want;
		RoomScaleNow = Scale;
		RoomFade = 0.f;
	}
	if (RoomFade >= 1.f) return;
	RoomFade = FMath::Min(1.f, RoomFade + Dt / 0.5f);
	const FPreset& To = Presets[RoomClassNow];
	FPreset Now;
	Now.Decay = FMath::Lerp(RoomFrom.X, To.Decay, RoomFade);
	Now.Wet = FMath::Lerp(RoomFrom.Y, To.Wet * Scale, RoomFade) / FMath::Max(Scale, 0.0001f);
	Now.PreDelay = FMath::Lerp(RoomFrom.Z, To.PreDelay, RoomFade);
	Now.GainHF = FMath::Lerp(RoomFrom.W, To.GainHF, RoomFade);
	RoomReverb->SetSettings(SettingsFor(Now, Scale));
}

void AOutlawsLevelActor::RoomSend(UAudioComponent* AC, const FVector& Where)
{
	if (bFrontEnd || (!bRoomTestLog && (!AC || !RoomSubmix))) return;       // (the test logs what it would send, muted)
	const float Scale = FMath::Clamp(CVarOLReverbScale.GetValueOnGameThread(), 0.f, 2.f);
	const uint8 C = RoomClassOf(CullSectorOf(Where + FVector(0.0, 0.0, 20.0)));
	float Dist = 800.f;
	if (const APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr)
	{
		if (PC->PlayerCameraManager) Dist = float(FVector::Dist(PC->PlayerCameraManager->GetCameraLocation(), Where));
	}
	// nearer is drier, as in life
	const float Send = SendByClass[C] * FMath::Clamp(Dist / 800.f, 0.6f, 1.f) * FMath::Min(Scale, 1.f);
	if (AC && RoomSubmix) AC->SetSubmixSend(RoomSubmix, Send);
	if (bRoomTestLog) UE_LOG(LogOutlaws, Log, TEXT("OLRT room sound send: %.2f from the %s %.0f cm off"), Send, ClassName(C), Dist);
}

void AOutlawsLevelActor::EchoOf(const FString& Wav, const FVector& At)
{
	// one soft tap off a face behind the sound (away from the listener), in the open only, at most one each 0.15 s
	UWorld* W = GetWorld();
	const float Scale = FMath::Clamp(CVarOLReverbScale.GetValueOnGameThread(), 0.f, 2.f);
	if (!W || Wav.IsEmpty() || bFrontEnd || Scale <= 0.f) return;
	const double T = FPlatformTime::Seconds();
	if (T - LastEchoAt < 0.15) return;
	if (RoomClassOf(PlayerSector) != ClassOpen || RoomClassOf(CullSectorOf(At + FVector(0.0, 0.0, 20.0))) != ClassOpen) return;
	const APlayerController* PC = W->GetFirstPlayerController();
	if (!PC || !PC->PlayerCameraManager) return;
	const FVector Ear = PC->PlayerCameraManager->GetCameraLocation();
	FVector Away = (At - Ear).GetSafeNormal2D();
	if (Away.IsNearlyZero()) Away = FVector(1.0, 0.0, 0.0);
	FHitResult Hit;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsEcho), false);
	if (!W->LineTraceSingleByChannel(Hit, At + FVector(0.0, 0.0, 50.0), At + FVector(0.0, 0.0, 50.0) + Away * 6000.0, ECC_Visibility, Q)) return;
	// a face nearer than 30 m makes no echo of its own (it is part of the shot); 30 to 60 m: the way there and back at the speed
	// of sound, 175 to 350 ms
	if (Hit.Distance < 3000.0) return;
	LastEchoAt = T;
	const float Delay = float(2.0 * Hit.Distance / 34300.0);
	const FVector From = Hit.ImpactPoint - Away * 30.0;
	TWeakObjectPtr<AOutlawsLevelActor> WeakThis(this);
	FTimerHandle H;
	W->GetTimerManager().SetTimer(H, [WeakThis, Wav, From, Scale]()
	{
		AOutlawsLevelActor* L = WeakThis.Get();
		if (!L) return;
		if (UAudioComponent* A = L->PlayWav(Wav, From))
		{
			A->SetVolumeMultiplier(A->VolumeMultiplier * 0.16f * FMath::Min(Scale, 1.f));      // (-16 dB)
			A->SetLowPassFilterEnabled(true);
			A->SetLowPassFilterFrequency(2500.f);
		}
	}, Delay, false);
	if (bRoomTestLog) UE_LOG(LogOutlaws, Log, TEXT("OLRT room sound echo: %s off a face %.0f m behind it, in %.0f ms"), *Wav, Hit.Distance / 100.0, Delay * 1000.f);
}

#if WITH_OUTLAWS_TESTS
void AOutlawsLevelActor::TickReverbTest(float Dt)
{
	// -OLLevel=miner|canyon -OLNoFrontEnd -OLReverbTest (R13): the player stood in a cave sector, then a room, then the open
	// (the levels that have them), a test shot 8 m ahead in each (its send logged), then shots in the open until an echo
	// is heard or twenty tried; then quit
	static double T = 0.0, Next = 3.0;
	static int32 Step = 0, Shots = 0;
	UWorld* W = GetWorld();
	APawn* P = W && W->GetFirstPlayerController() ? W->GetFirstPlayerController()->GetPawn() : nullptr;
	const FOutlawsLevel* Lv = LiveLevel();
	if (!P || !Lv || bNeedPlace || bFrontEnd) return;
	T += Dt;
	if (T < Next) return;
	bRoomTestLog = true;
	bInvincible = true;
	RoomClassOf(0);
	auto CentreOf = [&](int32 Si)
	{
		const FOutlawsSector& S = Lv->Sectors[Si];
		FVector2D C = FVector2D::ZeroVector;
		for (const FVector2D& V : S.Verts) C += V;
		C /= FMath::Max(1, S.Verts.Num());
		return FOutlawsGeometry::ToUE(C.X, S.Floor.Y + 6.0, C.Y);
	};
	auto Pick = [&](uint8 Class, int32 Nth)
	{
		int32 Seen = 0;
		for (int32 i = 0; i < RoomClasses.Num(); ++i)
		{
			if (RoomClasses[i] != Class || Lv->Sectors[i].Verts.Num() < 3 || Lv->Sectors[i].NoWalls()) continue;
			if (CullSectorOf(CentreOf(i)) != i) continue;          // (a sector whose middle is its own: not a sliver)
			if (Seen++ == Nth) return i;
		}
		return -1;
	};
	if (Step < 3)
	{
		const uint8 Class = Step == 0 ? 2 : Step == 1 ? 1 : 0;
		const int32 Si = Pick(Class, 3);
		if (Si >= 0)
		{
			P->TeleportTo(CentreOf(Si), P->GetActorRotation(), false, true);
			UE_LOG(LogOutlaws, Log, TEXT("OLRT room sound test: the player to sector %d (%s)"), Si, Class == 2 ? TEXT("cave") : Class == 1 ? TEXT("room") : TEXT("open"));
		}
		else UE_LOG(LogOutlaws, Log, TEXT("OLRT room sound test: no %s sector here"), Class == 2 ? TEXT("cave") : Class == 1 ? TEXT("room") : TEXT("open"));
		++Step;
		Next = T + 1.5;
		return;
	}
	if (Step >= 3 && Step < 6)
	{
		// (the shot for the last placement)
		PlayWav(TEXT("single"), P->GetActorLocation() + P->GetActorForwardVector() * 800.0);
		++Step;
		Next = T + 1.0;
		if (Step == 4)
		{
			// the order: cave, room, open placements were made one after the other; shoot after each by re-placing
			Step = 10;
		}
		return;
	}
	if (Step == 10 || Step == 11 || Step == 12)
	{
		const uint8 Class = Step == 10 ? 2 : Step == 11 ? 1 : 0;
		const int32 Si = Pick(Class, 3);
		if (Si >= 0)
		{
			P->TeleportTo(CentreOf(Si), P->GetActorRotation(), false, true);
			Next = T + 1.0;
			Step += 10;           // 20, 21, 22: shoot there next
		}
		else
		{
			++Step;
			if (Step == 13) Step = 30;
		}
		return;
	}
	if (Step >= 20 && Step <= 22)
	{
		PlayWav(TEXT("single"), P->GetActorLocation() + P->GetActorForwardVector() * 800.0);
		Step = Step == 22 ? 30 : Step - 9;          // 20 -> 11, 21 -> 12, 22 -> 30
		Next = T + 1.5;
		return;
	}
	if (Step == 30)
	{
		// echoes: shots from open sectors' middles, toward and away from walls, till one is heard
		const int32 Si = Pick(0, Shots + 4);
		if (Si < 0 || Shots >= 20)
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLRT room sound test: done (%d echo tries)"), Shots);
			Step = 40;
			FGenericPlatformMisc::RequestExit(false);
			return;
		}
		const FVector At = CentreOf(Si);
		P->TeleportTo(At + FVector(0.0, 0.0, 10.0), P->GetActorRotation(), false, true);
		LastEchoAt = 0.0;
		EchoOf(TEXT("single"), At + FVector(500.0, 0.0, 100.0));
		++Shots;
		Next = T + 0.5;
	}
}
#endif // WITH_OUTLAWS_TESTS

// ---- -OLTeardownTest=host | mphost | join (the Quest's "!IsRooted()" crash at a map load, questhost) -----------------------
// -OLLevel=<level> -OLNoFrontEnd: the level played a while (its reverb bus made), a level's end movie played and skipped
// (-OLTeardownMovie=<name>, toe_trb by default), the title menu, then co-op hosted, a match hosted (-OLTeardownMap=<rcm>,
// town_2p.rcm by default) or a host joined (-OLTeardownJoin=<address>); in the new world End Game, back to the title menu,
// then quit.  Each map load is logged with the rooted objects inside the old world (OLROOT).

#if WITH_OUTLAWS_TESTS
void AOutlawsLevelActor::StartTeardownTest()
{
	static bool bStarted = false;
	if (bStarted) return;
	bStarted = true;
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float Dt)
	{
		UWorld* W = nullptr;
		if (GEngine)
		{
			for (const FWorldContext& Ctx : GEngine->GetWorldContexts())
			{
				if (Ctx.WorldType == EWorldType::Game && Ctx.World()) W = Ctx.World();
			}
		}
		if (!W || W->bIsTearingDown) return true;
		for (TActorIterator<AOutlawsLevelActor> It(W); It; ++It)
		{
			It->TickTeardownTest(FMath::Min(Dt, 0.1f));
			break;
		}
		return true;
	}));
}

void AOutlawsLevelActor::TickTeardownTest(float Dt)
{
	struct FState { int32 Step = 0; double T = 0.0; TWeakObjectPtr<UWorld> From; };
	static FState S;
	FString Kind;
	OLFlags::Value(TEXT("OLTeardownTest"), Kind);
	Kind = Kind.ToLower();
	UWorld* W = GetWorld();
	UOutlawsCoop* C = UOutlawsCoop::Get(this);
	if (!W || !C) return;
	S.T += Dt;
	auto Next = [](const TCHAR* What)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLTEARDOWN step %d done: %s"), S.Step, What);
		++S.Step;
		S.T = 0.0;
	};
	switch (S.Step)
	{
	case 0:      // the level live a while, the reverb bus made
		if (bFrontEnd || bNeedPlace || !LiveLevel() || S.T < 6.0) return;
		UE_LOG(LogOutlaws, Log, TEXT("OLTEARDOWN level %s: reverb bus %s"), *LevelName, RoomSubmix ? TEXT("made") : TEXT("NOT MADE"));
		{
			FString M = TEXT("toe_trb");
			OLFlags::Value(TEXT("OLTeardownMovie"), M);
			const bool bOn = PlayMovie(M, true);
			UE_LOG(LogOutlaws, Log, TEXT("OLTEARDOWN movie %s: %s"), *M, bOn ? TEXT("playing") : TEXT("did not play"));
		}
		Next(TEXT("the movie"));
		return;
	case 1:
		if (S.T < 4.0) return;
		UE_LOG(LogOutlaws, Log, TEXT("OLTEARDOWN movie playing %d at the skip"), IsMoviePlaying() ? 1 : 0);
		SkipMovie();
		Next(TEXT("the movie skipped"));
		return;
	case 2:      // the next level (the movie's end starts it), then the title menu
		if (S.T < 8.0) return;
		if (!bFrontEnd)
		{
			if (AOutlawsPlayer* P = W->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(W->GetFirstPlayerController()->GetPawn()) : nullptr) P->CloseGameMenu();
			ReturnToFrontEnd();
		}
		Next(TEXT("the title menu"));
		return;
	case 3:
		if (!bFrontEnd || S.T < 6.0) return;
		LogRootedWorldObjects(W, TEXT("teardown test, before the map load"));
		S.From = W;
		if (Kind == TEXT("join"))
		{
			FString Addr = TEXT("127.0.0.1:7797");
			OLFlags::Value(TEXT("OLTeardownJoin"), Addr);
			C->Join(Addr, true);
		}
		else if (Kind == TEXT("mphost"))
		{
			FOutlawsMpHostOptions O;
			O.Load();
			O.MapFile = TEXT("town_2p.rcm");
			OLFlags::Value(TEXT("OLTeardownMap"), O.MapFile);
			O.GameClass = EOutlawsGameClass::DeathMatch;
			O.MaxPlayers = OutlawsMpHost::MaxPlayersCap();
			O.StartWhen = 0;
			if (!C->HostMatch(O)) UE_LOG(LogOutlaws, Log, TEXT("OLTEARDOWN the match would not host"));
		}
		else C->Host();
		Next(*FString::Printf(TEXT("%s asked"), *Kind));
		return;
	case 4:      // in the new world a while: End Game
		if (W == S.From.Get() || S.T < 12.0) return;
		UE_LOG(LogOutlaws, Log, TEXT("OLTEARDOWN survived the map load: %s, net mode %d, coop active %d"), *Kind, int32(W->GetNetMode()), UOutlawsCoop::IsActive(this) ? 1 : 0);
		LogRootedWorldObjects(W, TEXT("teardown test, before End Game"));
		S.From = W;
		UOutlawsCoop::LeaveSession(this, TEXT("a player ended the game (teardown test)"));
		Next(TEXT("End Game"));
		return;
	case 5:
		if (W == S.From.Get() || S.T < 8.0) return;
		UE_LOG(LogOutlaws, Log, TEXT("OLTEARDOWN back after End Game: front end %d, net mode %d: PASS"), bFrontEnd ? 1 : 0, int32(W->GetNetMode()));
		Next(TEXT("quit"));
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
