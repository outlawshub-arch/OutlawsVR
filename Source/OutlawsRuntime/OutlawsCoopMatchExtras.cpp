// M4's remainder (PLANS.md "M4. Polish"): spectating while dead, and the taunts over the link.
// - Spectating (off by default since BUG-MPDEATHCAM, user 2026-10-08: dead in a match, the view stays where this player fell,
//   the death drop and then still, until the respawn, as the original game's): ol.Spectate 1 brings back the follow, dead
//   two seconds, a camera behind a living player (3 m behind, 1.5 m up, looking at them, on a spring), the head still turning
//   it in a headset (the camera locked to the HMD); a grip (or SpectateNext) moves to the next living player; the respawn
//   puts the view back on this player's own eyes.  Co-op never spectates (IsMatch).
// - Taunts: a taunt said here (F1..F10, the VR stick's long press) goes to the host (ServerTaunt) and on to every other
//   machine (ClientTaunt), heard there from where its player stands.  Only a wav the LABs have is played; two a second at
//   most (the link's gate).  ol.TauntLink 0 keeps them on this machine.
#include "OutlawsCoop.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsLab.h"
#include "OutlawsLevelActor.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsFragTable.h"
#include "OutlawsMpHost.h"
#include "OutlawsPlayer.h"
#include "OutlawsRemotePlayer.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/App.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/Engine.h"

TAutoConsoleVariable<int32> CVarOLSpectate(TEXT("ol.Spectate"), 0, TEXT("Matches: dead, follow a living player's view (1), or the death view alone, held where this player fell (0)."));
TAutoConsoleVariable<int32> CVarOLTauntLink(TEXT("ol.TauntLink"), 1, TEXT("Co-op and matches: taunts heard by the other players (1), or on this machine alone (0)."));
extern TAutoConsoleVariable<int32> CVarOLMatchStrikes;
extern TAutoConsoleVariable<int32> CVarOLScoreboardOnTop;

namespace
{
	constexpr double SpectateAfter = 2.0;
	constexpr double SpectateBack = 300.0, SpectateUp = 150.0;

	bool CleanTaunt(const FString& In, FString& Out)
	{
		Out = In.Left(16).ToLower();
		if (Out.IsEmpty()) return false;
		for (const TCHAR Ch : Out) if (!FChar::IsAlnum(Ch) && Ch != TEXT('_')) return false;
		return true;
	}
}

bool UOutlawsCoop::BlastsDealtByHost(const UObject* Context)
{
	return IsMatchActive(Context) && CVarOLMatchStrikes.GetValueOnGameThread() != 0;
}

// ---- spectating -------------------------------------------------------------------------------------------------------------

void UOutlawsCoop::StopSpectating(const TCHAR* Why)
{
	AOutlawsPlayer* P = LocalPlayer();
	if (ACameraActor* Cam = SpectateCam.Get())
	{
		if (APlayerController* PC = P ? Cast<APlayerController>(P->GetController()) : nullptr) PC->SetViewTargetWithBlend(P, 0.3f);
		Cam->Destroy();
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH spectating ends (%s)"), Why);
	}
	SpectateCam.Reset();
	SpectateWho = -1;
}

void UOutlawsCoop::SpectateNext(const TCHAR* Why)
{
	// the next living player after the one followed (in player order, round)
	int32 Pick = -1;
	for (int32 k = 1; k <= MaxPlayers && Pick < 0; ++k)
	{
		const int32 Who = (FMath::Max(SpectateWho, 0) + k) % MaxPlayers;
		if (Who == MyIndex()) continue;
		const AOutlawsRemotePlayer* R = PuppetOf(Who);
		if (R && !R->IsHidden() && !R->PlateDead()) Pick = Who;
	}
	if (Pick < 0 || Pick == SpectateWho) return;
	SpectateWho = Pick;
	const AOutlawsCoopLink* K = GetLink(Pick);
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH spectating the %s (%s) from %.0f m behind (%s)"), WhoWord(Pick), K ? *K->PlayerName : TEXT("?"), SpectateBack / 100.0, Why);
	if (AOutlawsLevelActor* L = FindLevel()) L->Notify(FString::Printf(TEXT("Watching %s"), K && !K->PlayerName.IsEmpty() ? *K->PlayerName : TEXT("another player")), 2.f);
}

void UOutlawsCoop::TickSpectate(double Now)
{
	AOutlawsLevelActor* L = FindLevel();
	AOutlawsPlayer* P = LocalPlayer();
	UWorld* W = CurrentWorld();
	const bool bDead = L && L->bDead && IsMatch();
	APlayerController* ViewPC = P ? Cast<APlayerController>(P->GetController()) : nullptr;
	const UCameraComponent* EyeCam = P ? P->Camera.Get() : nullptr;
	const FVector Eye = EyeCam ? EyeCam->GetComponentLocation() : (P ? P->GetActorLocation() : FVector::ZeroVector);
	if (!bDead && DeadSince != 0.0)
	{
		// (BUG-MPDEATHCAM) back on this player's feet: how the death view held, and the table put away
		const bool bHeld = !bDeathViewLeft && DeathViewDrift <= 120.0;
		++(bHeld ? DeathViewsHeld : DeathViewsMoved);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH death view: %.1f s at the death spot, the eye went %.0f cm from it at most, %s (%s)"), Now - DeadSince, DeathViewDrift,
			bDeathViewLeft ? TEXT("the view left this player") : TEXT("the view on this player throughout"), bHeld ? TEXT("held") : TEXT("MOVED"));
		CloseScoreboard(P, TEXT("the respawn"));
	}
	if (!bDead) DeadSince = 0.0;
	else if (DeadSince == 0.0 && P)
	{
		DeadSince = Now;
		DeathViewAt = P->GetActorLocation();
		DeathViewDrift = 0.0;
		bDeathViewLeft = false;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH death view begins at %s (spectate %d)"), *DeathViewAt.ToCompactString(), CVarOLSpectate.GetValueOnGameThread());
	}
	else if (P && CVarOLSpectate.GetValueOnGameThread() == 0)
	{
		// the body held where it fell (a blast or a slope carried the dead view 10 to 25 m, user 2026-10-08), falling still
		// allowed; held, it is never far off, so a body farther than 3 m is the respawn's placing, left alone and not counted
		const FVector Body = P->GetActorLocation();
		if (FVector::Dist2D(Body, DeathViewAt) <= 300.0)
		{
			if (FVector::Dist2D(Body, DeathViewAt) > 2.0) P->SetActorLocation(FVector(DeathViewAt.X, DeathViewAt.Y, Body.Z), false, nullptr, ETeleportType::TeleportPhysics);
			if (UCharacterMovementComponent* Move = P->GetCharacterMovement()) { Move->Velocity.X = 0.0; Move->Velocity.Y = 0.0; }
			DeathViewDrift = FMath::Max(DeathViewDrift, FVector::Dist2D(Eye, DeathViewAt));
			bDeathViewLeft |= ViewPC && ViewPC->GetViewTarget() != P;
		}
	}
	if (!bDead || !P || !W || CVarOLSpectate.GetValueOnGameThread() == 0 || Now - DeadSince < SpectateAfter)
	{
		if (SpectateCam.IsValid()) StopSpectating(bDead ? TEXT("switched off") : TEXT("alive again"));
		return;
	}
	// the followed one gone or dead: the next
	const AOutlawsRemotePlayer* R = SpectateWho >= 0 ? PuppetOf(SpectateWho) : nullptr;
	if (!R || R->IsHidden() || R->PlateDead())
	{
		SpectateNext(SpectateWho < 0 ? TEXT("dead two seconds") : TEXT("the one followed is gone"));
		R = SpectateWho >= 0 ? PuppetOf(SpectateWho) : nullptr;
		if (!R) return;
	}
	// a grip pressed (either hand): the next player
	const bool bGrip = P->bVR && (P->Axis(TEXT("OLVR_LeftGrip")) > 0.7f || P->Axis(TEXT("OLVR_RightGrip")) > 0.7f);
	if (bGrip && !bSpectateGrip) SpectateNext(TEXT("a grip"));
	bSpectateGrip = bGrip;
	R = PuppetOf(SpectateWho);
	if (!R) return;
	ACameraActor* Cam = SpectateCam.Get();
	const FVector Body = R->ShownBody();
	const FVector Back = -R->GetActorForwardVector().GetSafeNormal2D();
	FVector Want = Body + (Back.IsNearlyZero() ? FVector(-1.0, 0.0, 0.0) : Back) * SpectateBack + FVector(0.0, 0.0, SpectateUp);
	{
		// not through a wall or a roof: pulled in to what lies between the head and the spot behind
		const FVector Head = Body + FVector(0.0, 0.0, 60.0);
		FHitResult Hit;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsSpectate), false, P);
		if (W->SweepSingleByChannel(Hit, Head, Want, FQuat::Identity, ECC_Visibility, FCollisionShape::MakeSphere(15.f), Q)) Want = Hit.Location;
	}
	if (!Cam)
	{
		FActorSpawnParameters SP;
		SP.ObjectFlags |= RF_Transient;
		Cam = W->SpawnActor<ACameraActor>(ACameraActor::StaticClass(), FTransform(Want), SP);
		if (!Cam) return;
		if (UCameraComponent* CC = Cam->GetCameraComponent()) { CC->bLockToHmd = P->bVR; CC->bConstrainAspectRatio = false; }
		SpectateCam = Cam;
		if (APlayerController* PC = Cast<APlayerController>(P->GetController())) PC->SetViewTargetWithBlend(Cam, 0.4f);
	}
	const FVector At = FMath::VInterpTo(Cam->GetActorLocation(), Want, float(FMath::Min(0.1, FApp::GetDeltaTime())), 4.f);
	const FRotator Look = P->bVR ? FRotator(0.0, (Body - At).Rotation().Yaw, 0.0) : (Body + FVector(0.0, 0.0, 40.0) - At).Rotation();
	Cam->SetActorLocationAndRotation(At, Look);
}

// ---- taunts over the link ---------------------------------------------------------------------------------------------------

void AOutlawsCoopLink::ServerTaunt_Implementation(const FString& Wav)
{
	if (!Admit(EOLGate::Taunt, Wav.Len() <= 16)) return;
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnTaunt(PlayerIndex, Wav);
}

void AOutlawsCoopLink::ClientTaunt_Implementation(int32 From, const FString& Wav)
{
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnTaunt(FMath::Clamp(From, 0, UOutlawsCoop::MaxPlayers - 1), Wav.Left(16));
}

void UOutlawsCoop::NoteTaunt(AOutlawsLevelActor* L, const FString& Wav)
{
	UOutlawsCoop* C = L ? Get(L) : nullptr;
	FString Clean;
	if (!C || !IsActive(L) || !C->bLevelBuilt || CVarOLTauntLink.GetValueOnGameThread() == 0 || !CleanTaunt(Wav, Clean)) return;
	if (C->IsHost()) C->ForClients([&](AOutlawsCoopLink& K) { K.ClientTaunt(0, Clean); });
	else if (AOutlawsCoopLink* Mine = C->GetLink(C->MyIndex())) Mine->ServerTaunt(Clean);
}

void UOutlawsCoop::OnTaunt(int32 Who, const FString& Wav)
{
	AOutlawsLevelActor* L = FindLevel();
	FString Clean;
	if (!L || !bLevelBuilt || !CleanTaunt(Wav, Clean)) return;
	const FOutlawsAssets* A = L->GetLabAssets();
	if (!A || !A->Has(Clean + TEXT(".wav")))
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH taunt from the %s refused: no %s.wav"), WhoWord(Who), *Clean);
		return;
	}
	if (IsHost()) ForClients([&](AOutlawsCoopLink& K) { K.ClientTaunt(Who, Clean); }, Who);     // (on to the others)
	if (Who == MyIndex()) return;
	const AOutlawsRemotePlayer* R = PuppetOf(Who);
	const FVector At = R ? R->ShownBody() + FVector(0.0, 0.0, 60.0) : L->GetActorLocation();
	L->PlaySoundAt(Clean, At);
	++MatchTauntsHeard;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH taunt heard from the %s: %s.wav at %s"), WhoWord(Who), *Clean, R ? TEXT("its body") : TEXT("the level (no body drawn)"));
}

// ---- -OLScoreboardTest (BUG-MPDEATHCAM, BUG-SCOREBOARD; the host of a bots match, run_bots.ps1 -HostExtra) ---------------
// Forty seconds into the match: the host turned to the nearest wall, the table toggled up in the old material (a shot), down
// by the same call, up in the overlay (a shot), down and up again; then the host dies where it stands, its view watched for
// four seconds (no other player's, the eye where it fell), respawns, and the table is gone with the respawn.

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TickScoreboardTest(double Now)
{
	AOutlawsLevelActor* L = FindLevel();
	AOutlawsPlayer* P = LocalPlayer();
	const AOutlawsCoopSession* S = GetSession();
	UWorld* W = CurrentWorld();
	if (!L || !P || !S || !W || SbTestStep >= 9) return;
	auto Check = [](bool bOk, const FString& What) { UE_LOG(LogOutlawsCoop, Log, TEXT("OLBOT test check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What); };
	IConsoleVariable* OnTop = IConsoleManager::Get().FindConsoleVariable(TEXT("ol.ScoreboardOnTop"));
	const bool bUp = MatchPanel.IsValid();
	switch (SbTestStep)
	{
	case 0:
		if (S->MatchState != uint8(FOutlawsMatchFlow::EState::Playing)) return;
		SbTestAt = Now + 40.0;
		SbTestStep = 1;
		return;
	case 1:
	{
		if (Now < SbTestAt || L->bDead) return;
		// the nearest wall round the eye, 0.6 to 2.8 m off (the table stands 3 m out: behind it in the old material)
		const UCameraComponent* Cam = P->Camera.Get();
		const FVector Eye = Cam ? Cam->GetComponentLocation() : P->GetActorLocation();
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsSbTest), false, P);
		double Best = 1e9, BestYaw = P->GetActorRotation().Yaw;
		for (int32 k = 0; k < 24; ++k)
		{
			const double Yaw = 15.0 * k;
			FHitResult Hit;
			if (W->LineTraceSingleByChannel(Hit, Eye, Eye + FRotator(0.0, Yaw, 0.0).Vector() * 280.0, ECC_Visibility, Q) && Hit.Distance >= 60.0 && Hit.Distance < Best) { Best = Hit.Distance; BestYaw = Yaw; }
		}
		P->SetActorRotation(FRotator(0.0, BestYaw, 0.0));
		if (AController* Ctl = P->GetController()) Ctl->SetControlRotation(FRotator(0.0, BestYaw, 0.0));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH sbtest: the host faces %s (yaw %.0f)"), Best < 1e8 ? *FString::Printf(TEXT("a wall %.0f cm off"), Best) : TEXT("no wall within 2.8 m"), BestYaw);
		SbTestAt = Now + 0.6;
		SbTestStep = 2;
		return;
	}
	case 2:
		if (Now < SbTestAt) return;
		if (OnTop) OnTop->Set(0, ECVF_SetByCode);
		ToggleScoreboard(P, TEXT("the test (the old material)"));
		SbTestAt = Now + 1.2;
		SbTestStep = 3;
		return;
	case 3:
		if (Now < SbTestAt) return;
		Shot(TEXT("ol_scoreboard_before"));
		Check(bUp, TEXT("the table up in the old material (the before shot)"));
		SbTestAt = Now + 0.6;
		SbTestStep = 4;
		return;
	case 4:
		if (Now < SbTestAt) return;
		ToggleScoreboard(P, TEXT("the test's same input"));
		Check(!MatchPanel.IsValid(), TEXT("the same input put the table away"));
		if (OnTop) OnTop->Set(1, ECVF_SetByCode);
		ToggleScoreboard(P, TEXT("the test (the overlay)"));
		SbTestAt = Now + 1.2;
		SbTestStep = 5;
		return;
	case 5:
	{
		if (Now < SbTestAt) return;
		Shot(TEXT("ol_scoreboard_after"));
		AOutlawsFragTablePanel* Panel = MatchPanel.Get();
		const FString Mat = Panel ? Panel->DescribeMaterial() : FString(TEXT("no table"));
		Check(Panel && Panel->bOnTop && Mat.Contains(TEXT("M_OLR_UIOverlay")), FString::Printf(TEXT("the table drawn with no depth test: %s"), *Mat));
		bool bHint = false;
		if (Panel) for (const FFragTableHeaderRow& R : Panel->GetData().HeaderRows) bHint |= R.Label == ScoreboardCloseHint(P);
		Check(bHint, FString::Printf(TEXT("the table says \"%s\""), *ScoreboardCloseHint(P)));
		SbTestAt = Now + 0.6;
		SbTestStep = 6;
		return;
	}
	case 6:
		if (Now < SbTestAt) return;
		ToggleScoreboard(P, TEXT("the test's same input"));
		Check(!MatchPanel.IsValid(), TEXT("the same input put the overlay table away"));
		ToggleScoreboard(P, TEXT("the test (up for the respawn)"));
		Check(MatchPanel.IsValid() && bMatchPanelHeld, TEXT("the same input brought it back, up until closed"));
		if (!L->bDead) L->DamagePlayer(10000.f, 1.f, true);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH sbtest: the host killed where it stands (dead %d)"), L->bDead ? 1 : 0);
		SbTestAt = Now + 1.8;                  // (a match puts the dead back after 3 s on its own)
		SbTestStep = 7;
		return;
	case 7:
		if (Now < SbTestAt) return;
		Check(L->bDead && !SpectateCam.IsValid() && SpectateWho < 0 && !bDeathViewLeft && DeathViewDrift <= 120.0,
			FString::Printf(TEXT("dead %.1f s, the view holds at the death spot (%.0f cm off it level, %s, spectate cam %s)"), DeadSince > 0.0 ? Now - DeadSince : -1.0, DeathViewDrift,
				bDeathViewLeft ? TEXT("left this player") : TEXT("on this player"), SpectateCam.IsValid() ? TEXT("on") : TEXT("off")));
		Shot(TEXT("ol_deathview_host"));
		P->RequestRespawn(false);
		SbTestAt = Now + 4.0;
		SbTestStep = 8;
		return;
	case 8:
		if (Now < SbTestAt) return;
		Check(!L->bDead, TEXT("the host respawned"));
		Check(!MatchPanel.IsValid() && !bMatchPanelHeld, TEXT("the table closed on its own with the respawn"));
		Check(DeathViewsHeld >= 1 && DeathViewsMoved == 0, FString::Printf(TEXT("the death views held %d, moved %d"), DeathViewsHeld, DeathViewsMoved));
		SbTestStep = 9;
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS

// ---- -OLScoreAboveTest (REQ-SCOREABOVE, REQ-SCORESIDE, BUG-SCOREOCCLUDE; a bots match, any machine: run_bots with -HostExtra/-ClientExtra)
// Twenty seconds into the match: no panel before; the pause menu opened and the live table stands to the left of the pause panel
// at its height (measured from the eyes, a shot level and a shot looking up); a wall 55 cm before the eyes and the table still
// drawn (its bounds hold the eyes); a body turn and the pair keeps its place round the head; the table's rows change while the
// menu stays open; a submenu keeps it; closing the menu removes it.

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TickScoreAboveTest(double Now)
{
	AOutlawsLevelActor* L = FindLevel();
	AOutlawsPlayer* P = LocalPlayer();
	const AOutlawsCoopSession* S = GetSession();
	if (!L || !P || !S || SaTestStep == 20) return;
	const TCHAR* Who = IsHost() ? TEXT("host") : WhoWord(MyIndex());
	auto Check = [Who](bool bOk, const FString& What) { UE_LOG(LogOutlawsCoop, Log, TEXT("OLSCOREABOVE check %s: (%s) %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), Who, *What); };
	AOutlawsFragTablePanel* Panel = PausePanel.Get();
	auto RowsKey = [](const AOutlawsFragTablePanel* Pn)
	{
		FString K;
		if (Pn) for (const FFragTableRow& R : Pn->GetData().Rows) K += R.Name + TEXT(":") + FString::Join(R.Values, TEXT(",")) + TEXT(";");
		return K;
	};
	auto Yaw = [](const FVector& From, const FVector& To) { return FMath::RadiansToDegrees(FMath::Atan2(To.Y - From.Y, To.X - From.X)); };
	const UCameraComponent* Cam = P->Camera.Get();
	const FVector Eye = Cam ? Cam->GetComponentLocation() : P->GetActorLocation();
	switch (SaTestStep)
	{
	case 0:
		if (S->MatchState != uint8(FOutlawsMatchFlow::EState::Playing) || L->bDead) return;
		if (GEngine) GEngine->Exec(P->GetWorld(), TEXT("r.MotionBlurQuality 0"));      // (sharp pictures, as the other shot tests)
		// (GATE-FLAKES: out of harm, as the other match tests are: a bot's stick killed the host with its menu open, which
		// closed the menu before the table's checks; the bots still fight each other, so the table's rows still change)
		L->bInvincible = true;
		SaTestAt = Now + 20.0;
		SaTestStep = 1;
		return;
	case 1:
		if (Now < SaTestAt || L->bDead) return;
		Check(!PausePanel.IsValid(), TEXT("no pause scoreboard before the menu opens"));
		// (the head level first, a moment before the menu opens where the eyes then look)
		if (AController* Ctl = P->GetController()) Ctl->SetControlRotation(FRotator(0.0, P->GetActorRotation().Yaw, 0.0));
		SaTestAt = Now + 0.5;
		SaTestStep = 31;
		return;
	case 31:
		if (Now < SaTestAt) return;
		P->OpenMenu();
		SaTestAt = Now + 2.0;
		SaTestStep = 2;
		return;
	case 2:
	{
		if (Now < SaTestAt) return;
		Check(P->bMenuOpen && Panel != nullptr, FString::Printf(TEXT("the menu is open and the table is up (menu %d)"), P->bMenuOpen ? 1 : 0));
		if (!Panel || !P->MenuRoot) { SaTestStep = 20; return; }
		const FTransform Pause = P->MenuRoot->GetComponentTransform();
		// (REQ-SCORESIDE) to the left of the pause panel as the eyes see them, the two middles at one height, the pair round the head
		const double PanelYaw = Yaw(Eye, Panel->GetActorLocation()), PauseYaw = Yaw(Eye, Pause.GetLocation());
		const double Apart = FRotator::NormalizeAxis(PanelYaw - PauseYaw);
		const double Mid = FRotator::NormalizeAxis(PauseYaw + Apart * 0.5 - P->GetControlRotation().Yaw);
		const double Rise = Panel->GetActorLocation().Z - Pause.GetLocation().Z;
		Check(Apart < -35.0 && Apart > -75.0 && FMath::Abs(Rise) < 10.0 && FMath::Abs(Mid) < 12.0,
			FString::Printf(TEXT("the table stands to the left of the pause panel: %.1f deg apart (left is less than 0), %.0f cm higher, the pair's middle %.1f deg from the head"), Apart, Rise, Mid));
		const FString Mat = Panel->DescribeMaterial();
		const bool bQuestMat = P->GetWorld() && P->GetWorld()->FeatureLevel <= ERHIFeatureLevel::ES3_1;
		Check(Panel->bOnTop && Mat.Contains(bQuestMat ? TEXT("M_OLR_WheelOverlay_Q") : TEXT("M_OLR_UIOverlay")) && !Panel->IsHidden(),
			FString::Printf(TEXT("drawn over the world: %s (%s)"), *Mat, bQuestMat ? TEXT("mobile: the 18 cm copy wanted") : TEXT("PC: no depth test wanted")));
		// (the ES3_1 preview showed the wood black: its frame is linear colour shown without the sRGB encode the headset's eye
		// buffer does, so every dark tone there looks near black; the canvas itself holds the same wood on both renderers)
		FColor Wood(0, 0, 0, 0);
		const bool bWood = Panel->ReadWood(Wood);
		Check(bWood && int32(Wood.R) + Wood.G + Wood.B >= 30 && Wood.R >= Wood.B,
			FString::Printf(TEXT("the table's wood is in its canvas, not black: sRGB %d %d %d (%s)"), Wood.R, Wood.G, Wood.B, bQuestMat ? TEXT("mobile") : TEXT("PC")));
		bool bLocal = false;
		for (const FFragTableRow& R : Panel->GetData().Rows) bLocal |= R.bLocal;
		Check(Panel->GetData().Rows.Num() >= 2 && bLocal, FString::Printf(TEXT("%d rows, the local player's marked (%s)"), Panel->GetData().Rows.Num(), *Panel->GetData().Describe()));
		SaTestFirst = RowsKey(Panel);
		SaTestAt = Now + 1.0;
		SaTestStep = 3;
		return;
	}
	case 3:
	{
		if (Now < SaTestAt) return;
		Shot(FString(TEXT("ol_scoreabove_level")) + (IsHost() ? TEXT("") : TEXT("_client")));
		SaTestAt = Now + 0.4;
		SaTestStep = 33;
		return;
	}
	case 33:
	{
		if (Now < SaTestAt) return;
		// (BUG-SCOREOCCLUDE) a wall 55 cm before the eyes, 3 m wide and tall, between the eyes and both panels
		{
			FVector Fwd = Cam ? Cam->GetForwardVector() : P->GetActorForwardVector();
			Fwd.Z = 0.0;
			Fwd = Fwd.GetSafeNormal();
			UStaticMeshComponent* Wall = NewObject<UStaticMeshComponent>(L, TEXT("OLScoreTestWall"));      // (the level's, not the player's: the menu lets the player's own parts show in front)
			Wall->SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Cube.Cube")));
			Wall->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Wall->SetMaterial(0, P->SolidMaterial(FColor(96, 70, 44)));      // (a door's brown, as the wheel's test)
			Wall->RegisterComponent();
			Wall->SetWorldLocationAndRotation(Eye + Fwd * 60.0, Fwd.Rotation());
			Wall->SetWorldScale3D(FVector(0.1, 3.0, 3.0));
			SaTestWall = Wall;
		}
		SaTestAt = Now + 1.5;
		SaTestStep = 32;
		return;
	}
	case 32:
	{
		if (Now < SaTestAt) return;
		UStaticMeshComponent* Wall = SaTestWall.Get();
		const bool bWallDrawn = Wall && Wall->WasRecentlyRendered(0.25f);
		const bool bDrawn = Panel && Panel->WasRecentlyRendered(0.25f);
		const USceneComponent* Root = Panel ? Panel->GetRootComponent() : nullptr;
		const bool bHolds = Root && FVector::Dist(Eye, Root->Bounds.Origin) < Root->Bounds.SphereRadius && Root->Bounds.GetBox().IsInside(Eye);      // (the box too: occlusion tests the box)
		Check(bDrawn && bHolds && bWallDrawn, FString::Printf(TEXT("a wall 55 cm before the eyes: the table still drawn %d, its bounds (sphere and box) hold the eyes %d (scale %.0f), the wall drawn %d"),
			bDrawn ? 1 : 0, bHolds ? 1 : 0, Panel ? Panel->GetBoundsScale() : 0.f, bWallDrawn ? 1 : 0));
		Shot(FString(TEXT("ol_scoreabove_wall")) + (IsHost() ? TEXT("") : TEXT("_client")));
		SaTestAt = Now + 0.5;
		SaTestStep = 30;
		return;
	}
	case 30:
		if (Now < SaTestAt) return;
		if (UStaticMeshComponent* Wall = SaTestWall.Get()) Wall->DestroyComponent();      // (after its picture is taken)
		SaTestWall.Reset();
		// looking up: the head tips well up (35 degrees), the pause panel and the table follow it
		if (AController* Ctl = P->GetController()) Ctl->SetControlRotation(FRotator(35.0, P->GetActorRotation().Yaw, 0.0));
		SaTestAt = Now + 2.5;
		SaTestStep = 4;
		return;
	case 4:
		if (Now < SaTestAt) return;
		Shot(FString(TEXT("ol_scoreabove_up")) + (IsHost() ? TEXT("") : TEXT("_client")));
		SaTestAt = Now + 1.0;
		SaTestStep = 5;
		return;
	case 5:
		if (Now < SaTestAt) return;
		// a body turn of 120 degrees: both panels turn round with the head
		if (AController* Ctl = P->GetController()) Ctl->SetControlRotation(FRotator(10.0, P->GetActorRotation().Yaw + 120.0, 0.0));
		SaTestAt = Now + 4.0;
		SaTestStep = 6;
		return;
	case 6:
	{
		if (Now < SaTestAt) return;
		if (!Panel || !P->MenuRoot) { Check(false, TEXT("the table is still up after the turn")); SaTestStep = 20; return; }
		const FVector PauseNow = P->MenuRoot->GetComponentLocation();
		const double HeadYaw = P->GetControlRotation().Yaw;
		const double PanelYaw = Yaw(Eye, Panel->GetActorLocation()), PauseYaw = Yaw(Eye, PauseNow);
		const double Apart = FRotator::NormalizeAxis(PanelYaw - PauseYaw);
		const double D2 = FMath::Abs(FRotator::NormalizeAxis(PauseYaw + Apart * 0.5 - HeadYaw));
		Check(Apart < -35.0 && Apart > -75.0 && D2 < 15.0, FString::Printf(TEXT("after a 120 degree body turn the table stays to the left of the pause panel (%.1f deg apart, the pair's middle %.1f deg from the head)"), Apart, D2));
		Shot(FString(TEXT("ol_scoreabove_turned")) + (IsHost() ? TEXT("") : TEXT("_client")));
		SaTestAt = Now + 45.0;
		SaTestStep = 7;
		return;
	}
	case 7:
		// live: the rows (frags, deaths, shots) change with the menu still open
		if (Panel && RowsKey(Panel) != SaTestFirst)
		{
			Check(true, FString::Printf(TEXT("scores changed live while paused: %s"), *Panel->GetData().Describe()));
			SaTestAt = Now + 0.2;
			SaTestStep = 8;
		}
		else if (Now >= SaTestAt)
		{
			Check(false, TEXT("no score row changed in 45 s with the menu open"));
			SaTestStep = 8;
		}
		return;
	case 8:
		if (Now < SaTestAt) return;
		// a submenu: the table stays and the page's rows still pick
		P->ShowMenuPage(TEXT("#cooppause"), true);
		SaTestAt = Now + 1.0;
		SaTestStep = 9;
		return;
	case 9:
		if (Now < SaTestAt) return;
		Check(PausePanel.IsValid() && P->MenuPage && P->MenuPage->File == TEXT("#cooppause"), TEXT("the Players page opens and the table stays up"));
		P->MenuBack();
		SaTestAt = Now + 0.6;
		SaTestStep = 10;
		return;
	case 10:
		if (Now < SaTestAt) return;
		Check(PausePanel.IsValid() && P->MenuPage && P->MenuPage->File == TEXT("main_game.rml"), TEXT("Back returns to the pause page, the table still up"));
		P->CloseMenu();
		SaTestAt = Now + 0.5;
		SaTestStep = 11;
		return;
	case 11:
		if (Now < SaTestAt) return;
		Check(!PausePanel.IsValid() && !P->bMenuOpen, TEXT("the table is gone with the menu closed"));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLSCOREABOVE (%s) done"), Who);
		// (the host stays a while for a client still in its own steps: its session would end with the host)
		SaTestAt = Now + (IsHost() ? 45.0 : 0.0);
		SaTestStep = 12;
		return;
	case 12:
		if (Now < SaTestAt) return;
		SaTestStep = 20;
		if (OLFlags::Param(TEXT("OLScoreAboveQuit"))) FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
