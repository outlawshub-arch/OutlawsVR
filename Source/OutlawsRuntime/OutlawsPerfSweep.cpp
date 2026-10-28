// REQ-PERFSWEEP (user 28 Oct, via the planner): New Game > Settings sweep (see OutlawsPerfTest.h): The Town at a busy spot,
// one Graphics row at a time (A B A B), each phase measured by the game itself (OutlawsPerf::BeginWindow), the table in
// Saved/Perf/sweep_<stamp>.txt, every row put back at its end, nothing written.  The run's start, its capture and its
// no-save rules are the Performance test's (OutlawsPerfTest.cpp); this is the sweep's own part.
#include "OutlawsPerfTest.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsEnemy.h"
#include "OutlawsPerf.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsBuildInfo.h"
#include "OutlawsDeviceProfile.h"
#include "OutlawsDynRes.h"
#include "OutlawsXRRefresh.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	// The spot: where the user's run 1 (28 Oct, townex, x4 bad guys) had its worst seconds, (13007, 249), (2897, 402) and
	// (4940, 1489) as perf_timeline.csv logs them (UE cm, the body's middle).  The desktop survey (-OLPerfSweepProbe=spots,
	// x4, two passes of eight headings) had this one with the widest view, 252 sectors visible against 174 and 99, and at yaw
	// 0 the most drawn (466 and 482 draws, 70 000 triangles), with 347 and 362 of 363 bad guys awake (the others 326 to
	// 362); the feet are found under it.
	const FVector SweepSpot(4940.0, 1489.0, 89.0);
	constexpr float SweepYaw = 0.f;
	constexpr double SweepPhaseSec = 15.0;     // a phase (-OLPerfSweepPhaseSec=<s> for a test)
	constexpr double SweepSettleSec = 3.0;     // the first seconds after a switch, not measured (the eye buffer re-made)
	constexpr double SweepLeadSec = 6.0;       // the card before the first phase
	constexpr double SweepCardSec = 12.0;      // the top-3 card at the end, before the main menu
	constexpr double SweepMarkerCm = 500.0;    // the marker this far ahead, at eye height

	enum class EStage : uint8 { Off, Lead, Settle, Measure, Card, Left };

	// what the sweep holds besides the phases: the player's own (put back), the timers, the marker
	struct FSweepRun
	{
		TWeakObjectPtr<AOutlawsLevelActor> Level;
		TWeakObjectPtr<AActor> Marker;
		TMap<FString, FString> Player;            // the player's Graphics rows (OutlawsDeviceProfile::GatherSettings)
		bool bHD = true, bHDChosen = false, bStochastic = true, bTinting = true, bGod = false;
		bool bRestored = true;
		int32 TopHz = 0;                          // the highest refresh the headset offers (0: none offered, the player's kept)
		double LeadSec = SweepLeadSec, CardSec = SweepCardSec;
		EStage Stage = EStage::Off;
		double At = 0.0;                          // real seconds: when this stage began
		double CardUntil = 0.0;
		bool bHeld = false;                       // the pause menu up: the phase measured again from its close
		double AwakeSum = 0.0, AwakeAt = 0.0;
		int32 AwakeN = 0;
		FString Card;                             // the phase card shown now
		FString Dump;                             // the player's settings as the run began (PerfTestSettings)
		FString Capture;
		FString Started;
	};
	FSweepRun GRun;
	OutlawsPerfTest::FSweep GSweep;

	// a row's saving at its lowest: A less B over each pair (an A and the B after it), their mean and their spread
	struct FRowSaving
	{
		FString Row;
		TArray<double> Work, Gpu, Fps;
		double Mean(const TArray<double>& V) const { double S = 0.0; for (const double X : V) S += X; return V.Num() ? S / V.Num() : 0.0; }
		double Spread(const TArray<double>& V) const { return V.Num() > 1 ? FMath::Max(V[0], V[1]) - FMath::Min(V[0], V[1]) : 0.0; }
		double Score() const { return Mean(Work) + Mean(Gpu); }
	};

	TArray<FRowSaving> SweepRank(const OutlawsPerfTest::FSweep& S, FRowSaving* OutAll)
	{
		TArray<FRowSaving> Rows;
		for (int32 k = 0; k + 1 < S.Phases.Num(); ++k)
		{
			const OutlawsPerfTest::FSweepPhase& A = S.Phases[k];
			const OutlawsPerfTest::FSweepPhase& B = S.Phases[k + 1];
			if (A.bB || !B.bB || B.bStep || A.Row != B.Row || !A.bMeasured || !B.bMeasured) continue;
			FRowSaving* R = A.Row == TEXT("All") ? OutAll : Rows.FindByPredicate([&A](const FRowSaving& X) { return X.Row == A.Row; });
			if (!R && A.Row != TEXT("All")) { R = &Rows.AddDefaulted_GetRef(); R->Row = A.Row; }
			if (!R) continue;
			R->Row = A.Row;
			R->Work.Add(A.M.GameWorkMs - B.M.GameWorkMs);
			R->Gpu.Add(A.M.GpuMs - B.M.GpuMs);
			R->Fps.Add(B.M.Fps - A.M.Fps);
		}
		Rows.Sort([](const FRowSaving& X, const FRowSaving& Y) { return X.Score() > Y.Score(); });
		return Rows;
	}

	FString SweepCard(const OutlawsPerfTest::FSweepPhase& Ph, int32 i, int32 n)
	{
		if (Ph.Row == TEXT("All")) return FString::Printf(TEXT("%d of %d: everything at %s"), i, n, Ph.bB ? TEXT("lowest") : TEXT("top"));
		if (Ph.bStep) return FString::Printf(TEXT("%d of %d: %s at %s %%"), i, n, *Ph.Row, *Ph.Value);
		return FString::Printf(TEXT("%d of %d: %s at %s"), i, n, *Ph.Row, Ph.bB ? TEXT("lowest") : TEXT("top"));
	}

	AOutlawsPlayer* SweepPlayer(const AOutlawsLevelActor* L)
	{
		UWorld* World = L ? L->GetWorld() : nullptr;
		return World && World->GetFirstPlayerController() ? Cast<AOutlawsPlayer>(World->GetFirstPlayerController()->GetPawn()) : nullptr;
	}
}

const OutlawsPerfTest::FSweep& OutlawsPerfTest::Sweep() { return GSweep; }

void AOutlawsLevelActor::PerfSweepBegin(const FString& CapturePath)
{
	using namespace OutlawsPerfTest;
	FSweepRun& R = GRun;
	FSweep& S = GSweep;
	R = FSweepRun();
	S = FSweep();
	R.Level = this;
	R.Capture = CapturePath.IsEmpty() ? FString(TEXT("(none: -OLNoPerfFiles)")) : CapturePath;
	R.Started = FDateTime::Now().ToString(TEXT("%Y-%m-%d %H:%M:%S"));
	double Sec = SweepPhaseSec;
	OLFlags::Value(TEXT("OLPerfSweepPhaseSec"), Sec);
	S.PhaseSec = FMath::Clamp(Sec, 1.0, 120.0);
	S.SettleSec = FMath::Min(SweepSettleSec, S.PhaseSec / 3.0);
	R.LeadSec = FMath::Min(SweepLeadSec, S.PhaseSec / 2.0);
	R.CardSec = FMath::Min(SweepCardSec, S.PhaseSec * 2.0);
	// the player's own, put back at the end (the device profile's rows, the level's switches, god mode)
	R.Dump = PerfTestSettings(R.Capture);
	OutlawsDeviceProfile::GatherSettings(R.Player);
	R.bHD = bHDTextures;
	R.bHDChosen = bHDChosen;
	R.bStochastic = bStochastic;
	R.bTinting = bPaletteGrades;
	R.bGod = bInvincible;
	R.bRestored = false;
	bHDChosen = true;          // (the Quest 2 profile's HD default never flips the look in Quest 2 Mode's phases)
	bInvincible = true;        // god mode for the sweep; the bad guys keep their normal AI
	const TArray<float> Rates = OutlawsDeviceProfile::RefreshRates();
	R.TopHz = Rates.Num() > 0 ? FMath::RoundToInt(Rates.Last()) : 0;

	// the phases: a row that cannot change is skipped (and held at its top in the all pair)
	IConsoleVariable* Q2Force = IConsoleManager::Get().FindConsoleVariable(TEXT("ol.Quest2Profile"));
	const bool bLamps = bLampSlots != 0;
	const bool bHd = TexturesHd.Num() > 0;
	const bool bQ2 = OutlawsDeviceProfile::ShowQuest2ModeRow() && !(Q2Force && Q2Force->GetInt() >= 0);
	if (!bLamps) S.Skipped.Add(TEXT("Lamp Lights (no lamp light slots in this build's light collection)"));
	if (!bHd) S.Skipped.Add(TEXT("Remastered Graphics (no HD textures in this level)"));
	if (!bQ2) S.Skipped.Add(OutlawsDeviceProfile::ShowQuest2ModeRow() ? TEXT("Quest 2 Mode (the Quest 2 profile is set from the command line)")
		: TEXT("Quest 2 Mode (a Quest 2: its profile is always on)"));
	auto Add = [&S](const TCHAR* Row, const FString& Value, bool bB, const FSweepSet& Set, bool bStep = false)
	{
		FSweepPhase& Ph = S.Phases.AddDefaulted_GetRef();
		Ph.Row = Row;
		Ph.Value = Value;
		Ph.bB = bB;
		Ph.bStep = bStep;
		Ph.Set = Set;
	};
	auto Pairs = [&Add](const TCHAR* Row, const TCHAR* Top, const TCHAR* Low, TFunctionRef<void(FSweepSet&)> Lower)
	{
		FSweepSet B;
		Lower(B);
		for (int32 k = 0; k < 2; ++k)
		{
			Add(Row, Top, false, FSweepSet());
			Add(Row, Low, true, B);
		}
	};
	Pairs(TEXT("Resolution"), TEXT("120"), TEXT("70"), [](FSweepSet& B) { B.Resolution = 70; });
	// (a live change, so the steps between: a phase each, B-tagged, in no pair; the table's resolution curve)
	for (const int32 Step : { 100, 85 })
	{
		FSweepSet B;
		B.Resolution = Step;
		Add(TEXT("Resolution"), FString::FromInt(Step), true, B, true);
	}
	if (bLamps) Pairs(TEXT("Lamp Lights"), TEXT("4"), TEXT("0"), [](FSweepSet& B) { B.Lamps = 0; });
	if (bHd) Pairs(TEXT("Remastered Graphics"), TEXT("on"), TEXT("off"), [](FSweepSet& B) { B.bRemastered = false; });
	Pairs(TEXT("Object Detail"), TEXT("Far"), TEXT("Near"), [](FSweepSet& B) { B.Detail = 0; });
	Pairs(TEXT("Stochastic Textures"), TEXT("on"), TEXT("off"), [](FSweepSet& B) { B.bStochastic = false; });
	Pairs(TEXT("Tinting"), TEXT("on"), TEXT("off"), [](FSweepSet& B) { B.bTinting = false; });
	if (bQ2) Pairs(TEXT("Quest 2 Mode"), TEXT("off"), TEXT("on"), [](FSweepSet& B) { B.bQuest2 = true; });
	FSweepSet Low;
	Low.Resolution = 70;
	Low.Lamps = bLamps ? 0 : 4;
	Low.bRemastered = !bHd;
	Low.Detail = 0;
	Low.bStochastic = false;
	Low.bTinting = false;
	Low.bQuest2 = bQ2;
	Add(TEXT("All"), TEXT("top"), false, FSweepSet());
	Add(TEXT("All"), TEXT("lowest"), true, Low);

	// the spot, facing the marker
	FVector Feet = SweepSpot;
	PerfSweepPlace(SweepSpot, SweepYaw, true, &Feet);
	AOutlawsPlayer* P = SweepPlayer(this);
	S.Spot = P ? P->GetActorLocation() : SweepSpot;
	S.Yaw = SweepYaw;
	{
		const FVector Fwd = FRotator(0.f, SweepYaw, 0.f).Vector();
		const FVector Eye = Feet + FVector(0.0, 0.0, 150.0);
		FVector At = Eye + Fwd * SweepMarkerCm;
		FHitResult Hit;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsSweepMarker), false, P);
		if (GetWorld() && GetWorld()->LineTraceSingleByChannel(Hit, Eye, At, ECC_Visibility, Q)) At = Eye + Fwd * FMath::Max(100.0, double(Hit.Distance) - 30.0);
		UStaticMesh* Ball = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
		UMaterialInterface* Glow = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_FxGlow.M_OLR_FxGlow"));
		FActorSpawnParameters SP;
		SP.Owner = this;
		SP.ObjectFlags |= RF_Transient;
		AActor* A = Ball && GetWorld() ? GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform(At), SP) : nullptr;
		if (A)
		{
			UStaticMeshComponent* M = NewObject<UStaticMeshComponent>(A);
			M->SetStaticMesh(Ball);
			if (Glow)
			{
				UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Glow, A);
				Mid->SetVectorParameterValue(TEXT("Color"), FLinearColor(1.f, 0.8f, 0.25f));
				Mid->SetScalarParameterValue(TEXT("Glow"), 4.f);
				M->SetMaterial(0, Mid);
			}
			M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			M->SetCastShadow(false);
			A->SetRootComponent(M);
			M->RegisterComponent();
			M->SetWorldLocation(At);
			M->SetWorldScale3D(FVector(0.25));        // (25 cm across: the sphere is a metre)
			SpawnedActors.Add(A);
			R.Marker = A;
			S.Marker = At;
		}
	}

	// every row at its top for the lead-in, then the start line
	PerfSweepApply(0);
	if (!CapturePath.IsEmpty())
	{
		FString Stamp = FPaths::GetBaseFilename(CapturePath);
		Stamp.RemoveFromStart(TEXT("capture_"));
		S.Path = FPaths::Combine(FPaths::GetPath(CapturePath), FString::Printf(TEXT("sweep_%s.txt"), *Stamp));
		FFileHelper::SaveStringToFile(PerfSweepTable(TEXT("running")), *S.Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		OutlawsPerf::MakeReadable(S.Path);       // (BUG-PERFFLAG2: adb's shell reads it)
	}
	else
	{
		S.Path = TEXT("(none: -OLNoPerfFiles)");
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLPERF sweep start: level %s, phases %d, spot (%.0f, %.0f, %.0f), sweep %s, capture %s"), *LevelName, S.Phases.Num(), S.Spot.X, S.Spot.Y, S.Spot.Z,
		*S.Path, *R.Capture);
	for (const FString& Why : S.Skipped) UE_LOG(LogOutlaws, Log, TEXT("OLPERF sweep skipped: %s"), *Why);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT settings sweep: %d phases of %.0f s (the first %.1f s of each not measured), the refresh %s, at (%.0f, %.0f, %.0f) yaw %.0f; god mode on, the settings file held"),
		S.Phases.Num(), S.PhaseSec, S.SettleSec, R.TopHz > 0 ? *FString::Printf(TEXT("%d Hz"), R.TopHz) : TEXT("the player's (no rates offered)"), S.Spot.X, S.Spot.Y, S.Spot.Z, S.Yaw);
	if (P)
	{
		P->ShowMessage(TEXT("Settings sweep: stand still and face the light"), float(R.LeadSec + 1.0));
		P->ShowMessage(FString::Printf(TEXT("About %d minutes. End Game stops it."), FMath::Max(1, FMath::RoundToInt(S.Phases.Num() * S.PhaseSec / 60.0))), float(R.LeadSec + 1.0));
	}
	R.Stage = EStage::Lead;
	R.At = FPlatformTime::Seconds();
}

void AOutlawsLevelActor::PerfSweepApply(int32 Phase)
{
	// a phase's rows (0: every row at its top): the device profile's through LoadSettings from the player's own keys (nothing
	// written), the level's switches as the Graphics page's presses make them
	const OutlawsPerfTest::FSweepSet Set = GSweep.Phases.IsValidIndex(Phase - 1) ? GSweep.Phases[Phase - 1].Set : OutlawsPerfTest::FSweepSet();
	TMap<FString, FString> M = GRun.Player;
	M.Add(TEXT("RenderScale"), FString::FromInt(Set.Resolution));
	M.Add(TEXT("LampLights"), FString::FromInt(Set.Lamps));
	M.Add(TEXT("ObjectDetail"), FString::FromInt(Set.Detail));
	M.Add(TEXT("Quest2Mode"), Set.bQuest2 ? TEXT("true") : TEXT("false"));
	if (GRun.TopHz > 0) M.Add(TEXT("RefreshRate"), FString::FromInt(GRun.TopHz));
	OutlawsDeviceProfile::LoadSettings(M, TEXT("settings sweep"));
	if (bHDTextures != Set.bRemastered) SetHDTextures(Set.bRemastered);
	if (bStochastic != Set.bStochastic) SetStochastic(Set.bStochastic);
	bPaletteGrades = Set.bTinting;
}

void AOutlawsLevelActor::PerfSweepRestore(bool bWorld)
{
	FSweepRun& R = GRun;
	if (R.bRestored) return;
	R.bRestored = true;
	// (bHDChosen still the sweep's: a profile changing back does not touch the look before it is put back below)
	OutlawsDeviceProfile::LoadSettings(R.Player, TEXT("settings sweep over: the player's own"));
	if (bWorld)
	{
		if (bHDTextures != R.bHD) SetHDTextures(R.bHD);
		if (bStochastic != R.bStochastic) SetStochastic(R.bStochastic);
	}
	else
	{
		bHDTextures = R.bHD;                 // (the game closing: the flags alone, no world to redraw)
		bStochastic = R.bStochastic;
	}
	bPaletteGrades = R.bTinting;
	bHDChosen = R.bHDChosen;
	TMap<FString, FString> Now;
	OutlawsDeviceProfile::GatherSettings(Now);
	bool bSame = Now.Num() == R.Player.Num();
	for (const TPair<FString, FString>& Kv : R.Player) bSame &= Now.FindRef(Kv.Key) == Kv.Value;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT settings sweep: the player's settings put back (rows %s; resolution %s, refresh %s, lamp lights %s, object detail %s, Quest 2 Mode %s, remastered %s, stochastic %s, tinting %s)"),
		bSame ? TEXT("identical") : TEXT("DIFFER"), *OutlawsDeviceProfile::RowValue(TEXT("r_resolution")), *OutlawsDeviceProfile::RowValue(TEXT("r_refresh")),
		*OutlawsDeviceProfile::RowValue(TEXT("r_lamps")), *OutlawsDeviceProfile::RowValue(TEXT("r_objdetail")), *OutlawsDeviceProfile::RowValue(TEXT("r_quest2mode")),
		bHDTextures ? TEXT("on") : TEXT("off"), bStochastic ? TEXT("on") : TEXT("off"), bPaletteGrades ? TEXT("on") : TEXT("off"));
}

bool AOutlawsLevelActor::PerfSweepPlace(const FVector& Spot, float Yaw, bool bTurn, FVector* OutFeet)
{
	// the feet under the spot (a trace down from 60 cm over the body's middle, under any roof), the body on them as a level
	// start places it; bTurn: facing Yaw, the view set straight
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	AOutlawsPlayer* P = PC ? Cast<AOutlawsPlayer>(PC->GetPawn()) : nullptr;
	if (!P) return false;
	const float Half = P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	FVector Feet = Spot - FVector(0.0, 0.0, Half);
	FHitResult Hit;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsSweepSpot), false, P);
	if (World->LineTraceSingleByObjectType(Hit, Spot + FVector(0.0, 0.0, 60.0), Spot - FVector(0.0, 0.0, 400.0), FCollisionObjectQueryParams(ECC_WorldStatic), Q)) Feet = Hit.ImpactPoint;
	const FRotator Rot(0.f, bTurn ? Yaw : float(P->GetActorRotation().Yaw), 0.f);
	P->TeleportTo(Feet + FVector(0.0, 0.0, Half + 2.0), Rot, false, true);
	P->GrantFallGrace(World->GetTimeSeconds() + 3.0);
	if (bTurn)
	{
		PC->SetControlRotation(Rot);
		P->Recenter(true);
	}
	if (OutFeet) *OutFeet = Feet;
	return true;
}

void AOutlawsLevelActor::TickPerfSweep(float Dt)
{
	using namespace OutlawsPerfTest;
	FSweepRun& R = GRun;
	FSweep& S = GSweep;
	const double Now = FPlatformTime::Seconds();
	AOutlawsPlayer* P = SweepPlayer(this);
	if (R.Stage == EStage::Card)
	{
		// the top-3 card read: god mode back and the main menu
		if (Now < R.CardUntil) return;
		R.Stage = EStage::Left;
		bInvincible = R.bGod;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT settings sweep: done, back to the main menu"));
		ReturnToFrontEnd();
		return;
	}
	if (!OutlawsPerfTest::IsRunning() || !P || R.Stage == EStage::Off || R.Stage == EStage::Left) return;
	// the pause menu up (or a movie, a rebuild): the phase waits, and is measured again from the menu's close
	if (P->IsMenuOpen() || P->IsDeadView() || IsMoviePlaying() || IsRebuildPending())
	{
		if (!R.bHeld)
		{
			R.bHeld = true;
			OutlawsPerf::EndWindow();
			UE_LOG(LogOutlaws, Log, TEXT("OLRT settings sweep: phase %d held (the pause menu up); measured again from its close"), S.Now);
		}
		return;
	}
	if (R.bHeld)
	{
		R.bHeld = false;
		if (S.Now > 0)
		{
			PerfSweepApply(S.Now);            // (a row pressed in the pause menu's Options undone)
			R.Stage = EStage::Settle;
		}
		R.At = Now;
		return;
	}
	auto Switch = [&]()
	{
		++S.Now;
		const FSweepPhase& Ph = S.Phases[S.Now - 1];
		// (wandered off the spot: put back on it, facing as the player faces now)
		const double Off = FVector::Dist2D(P->GetActorLocation(), S.Spot);
		if (Off > 100.0)
		{
			PerfSweepPlace(SweepSpot, S.Yaw, false);
			UE_LOG(LogOutlaws, Log, TEXT("OLRT settings sweep: put back on the spot (%.0f cm off it)"), Off);
		}
		PerfSweepApply(S.Now);
		UE_LOG(LogOutlaws, Log, TEXT("OLPERF sweep phase %d of %d: %s %s (%s)"), S.Now, S.Phases.Num(), *Ph.Row, *Ph.Value, Ph.bB ? TEXT("B") : TEXT("A"));
		if (!R.Card.IsEmpty()) P->DropMessage(R.Card);
		R.Card = SweepCard(Ph, S.Now, S.Phases.Num());
		P->ShowMessage(R.Card, float(S.PhaseSec + 2.0));
		R.Stage = EStage::Settle;
		R.At = Now;
	};
	switch (R.Stage)
	{
	case EStage::Lead:
		if (Now - R.At >= R.LeadSec) Switch();
		return;
	case EStage::Settle:
		if (Now - R.At < S.SettleSec) return;
		OutlawsPerf::BeginWindow(OutlawsPerf::BudgetMs());
		R.AwakeSum = 0.0;
		R.AwakeN = 0;
		R.AwakeAt = 0.0;
		R.Stage = EStage::Measure;
		R.At = Now;
		return;
	case EStage::Measure:
	{
		if (Now >= R.AwakeAt)
		{
			// the bad guys awake (not dormant, not frozen far off), once a second
			int32 Awake = 0;
			for (const TWeakObjectPtr<AOutlawsEnemy>& E : Enemies) Awake += E.IsValid() && !E->IsDead() && E->PerfAwake() ? 1 : 0;
			R.AwakeSum += Awake;
			++R.AwakeN;
			R.AwakeAt = Now + 1.0;
		}
		if (Now - R.At < S.PhaseSec - S.SettleSec) return;
		FSweepPhase& Ph = S.Phases[S.Now - 1];
		Ph.M = OutlawsPerf::EndWindow();
		Ph.Awake = R.AwakeN ? R.AwakeSum / R.AwakeN : 0.0;
		Ph.bMeasured = true;
		++S.Done;
		UE_LOG(LogOutlaws, Log, TEXT("OLPERF sweep phase %d measured: %lld frames, game work %.2f, render %.2f, RHI %.2f, GPU %.2f, fps %.1f, over %.1f %%, awake %d"), S.Now, Ph.M.Frames,
			Ph.M.GameWorkMs, Ph.M.RenderMs, Ph.M.RhiMs, Ph.M.GpuMs, Ph.M.Fps, Ph.M.OverPct, FMath::RoundToInt(Ph.Awake));
		if (S.Now >= S.Phases.Num())
		{
			OutlawsPerfTest::End(TEXT("done"));
			return;
		}
		Switch();
		return;
	}
	default:
		return;
	}
}

void OutlawsPerfTest::SweepFinish(const TCHAR* Why)
{
	// End's sweep part, the capture still on: a phase cut short is not measured; every row put back; the table; the top-3 card
	// at the end of a whole sweep (god mode kept while it shows), else god mode back now
	FSweepRun& R = GRun;
	FSweep& S = GSweep;
	OutlawsPerf::EndWindow();
	AOutlawsLevelActor* L = R.Level.Get();
	const bool bClosed = FCString::Strcmp(Why, TEXT("game closed")) == 0 || IsEngineExitRequested();
	if (L) L->PerfSweepRestore(!bClosed);
	if (L && !S.Path.StartsWith(TEXT("(")))
	{
		if (!FFileHelper::SaveStringToFile(L->PerfSweepTable(Why), *S.Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			UE_LOG(LogOutlaws, Warning, TEXT("OLRT settings sweep: the table %s could not be written"), *S.Path);
		}
		OutlawsPerf::MakeReadable(S.Path);
	}
	AOutlawsPlayer* P = SweepPlayer(L);
	if (P && !R.Card.IsEmpty()) P->DropMessage(R.Card);
	R.Card.Reset();
	S.Top3.Reset();
	const TArray<FRowSaving> Rank = SweepRank(S, nullptr);
	for (int32 i = 0; i < Rank.Num() && i < 3; ++i)
	{
		S.Top3.Add(FString::Printf(TEXT("%s%d. %s: GPU %.2f, game %.2f ms"), i == 0 ? TEXT("Costliest ") : TEXT(""), i + 1, *Rank[i].Row,
			Rank[i].Mean(Rank[i].Gpu), Rank[i].Mean(Rank[i].Work)));
	}
	if (L && P && !bClosed && FCString::Strcmp(Why, TEXT("done")) == 0)
	{
		for (const FString& Line : S.Top3) P->ShowMessage(Line, float(R.CardSec + 1.0));
		R.Stage = EStage::Card;
		R.CardUntil = FPlatformTime::Seconds() + R.CardSec;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT settings sweep: the top-3 card: %s"), *FString::Join(S.Top3, TEXT(" | ")));
		return;
	}
	R.Stage = EStage::Left;
	if (L) L->bInvincible = R.bGod;
}

void OutlawsPerfTest::SweepOver()
{
	// the main menu again (or the run left some other way): god mode back, the marker gone, the rows back if not yet
	FSweepRun& R = GRun;
	if (AOutlawsLevelActor* L = R.Level.Get())
	{
		L->PerfSweepRestore(!IsEngineExitRequested());
		L->bInvincible = R.bGod;
	}
	if (AActor* M = R.Marker.Get()) M->Destroy();
	R.Marker.Reset();
	R.Stage = EStage::Off;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT settings sweep over: god mode %s as before, the settings file as ever from here"), R.bGod ? TEXT("on") : TEXT("off"));
}

FString AOutlawsLevelActor::PerfSweepTable(const FString& Why) const
{
	using namespace OutlawsPerfTest;
	const FSweep& S = GSweep;
	const FSweepRun& R = GRun;
	TArray<FString> L;
	L.Add(FString::Printf(TEXT("Settings sweep, %s (%s)"), *R.Started, *Why));
	L.Add(FString::Printf(TEXT("build: %s"), *OutlawsBuildInfo::Line()));
	L.Add(FString::Printf(TEXT("level: %s (%s), difficulty %s (%s), bad guys x%.2f, randomizer %s, seed %u"), *LevelName, *LevelTitle(LevelName), OutlawsDifficulty::Name(Difficulty),
		*OutlawsPerfTest::DifficultyFrom(), SoloBadGuysWanted(), RandoOn() ? TEXT("on") : TEXT("off"), RandoRun.Seed));
	L.Add(FString::Printf(TEXT("headset: %s, profile %s"), OutlawsDeviceProfile::HeadsetName(OutlawsDeviceProfile::Headset()), OutlawsDeviceProfile::IsQuest2() ? TEXT("Quest 2") : TEXT("default")));
	L.Add(FString::Printf(TEXT("spot: (%.0f, %.0f, %.0f), facing yaw %.0f (the marker %.0f m ahead at eye height)"), S.Spot.X, S.Spot.Y, S.Spot.Z, S.Yaw, SweepMarkerCm / 100.0));
	const float Hz = OutlawsXRRefresh::CurrentRate();
	L.Add(FString::Printf(TEXT("refresh: %s asked, %s now; budget %.2f ms (a frame over 1.05 x it is over budget)"), R.TopHz > 0 ? *FString::Printf(TEXT("%d Hz"), R.TopHz) : TEXT("nothing"),
		Hz > 0.f ? *FString::Printf(TEXT("%.0f Hz"), Hz) : TEXT("no headset rate"), OutlawsPerf::BudgetMs()));
	L.Add(FString::Printf(TEXT("phases: %d of %d measured, %.0f s each, the first %.1f s after a switch not measured"), S.Done, S.Phases.Num(), S.PhaseSec, S.SettleSec));
	L.Add(FString::Printf(TEXT("skipped: %s"), S.Skipped.Num() ? *FString::Join(S.Skipped, TEXT("; ")) : TEXT("none")));
	L.Add(TEXT("live: every row changes live, no reload (Resolution re-makes the eye buffer through vr.PixelDensity, Lamp Lights and Object Detail are read each frame, Remastered "
		"Graphics swaps the level's loaded HD textures, Stochastic Textures a material switch, Tinting the view's grade, Quest 2 Mode the profile with the other rows held at the top)"));
	if (OutlawsDeviceProfile::EyeBufferText() == TEXT("no headset")) L.Add(TEXT("note: no headset (a desktop run): the Resolution row sizes nothing here"));
	if (OutlawsDynRes::IsActive()) L.Add(TEXT("note: dynamic resolution is on: the Resolution row is its ceiling"));
	L.Add(FString::Printf(TEXT("capture: %s"), *R.Capture));
	FRowSaving All;
	const TArray<FRowSaving> Rank = SweepRank(S, &All);
	L.Add(TEXT(""));
	L.Add(TEXT("Each row's saving at its lowest (A less B; the mean of its pairs, the spread between its two pairs in brackets), most saved first (game work + GPU ms):"));
	L.Add(FString::Printf(TEXT("  %-22s %5s %18s %18s %16s"), TEXT("row"), TEXT("pairs"), TEXT("game work ms"), TEXT("GPU ms a frame"), TEXT("fps gained")));
	double SumWork = 0.0, SumGpu = 0.0, SumFps = 0.0;
	auto Cell = [](const FRowSaving& X, const TArray<double>& V, bool bFps)
	{
		const FString Mean = bFps ? FString::Printf(TEXT("%+.1f"), X.Mean(V)) : FString::Printf(TEXT("%.2f"), X.Mean(V));
		const FString Spread = V.Num() < 2 ? FString(TEXT("-")) : bFps ? FString::Printf(TEXT("%.1f"), X.Spread(V)) : FString::Printf(TEXT("%.2f"), X.Spread(V));
		return FString::Printf(TEXT("%s (%s)"), *Mean, *Spread);
	};
	for (const FRowSaving& X : Rank)
	{
		L.Add(FString::Printf(TEXT("  %-22s %5d %18s %18s %16s"), *X.Row, X.Work.Num(), *Cell(X, X.Work, false), *Cell(X, X.Gpu, false), *Cell(X, X.Fps, true)));
		SumWork += X.Mean(X.Work);
		SumGpu += X.Mean(X.Gpu);
		SumFps += X.Mean(X.Fps);
	}
	if (Rank.Num() == 0) L.Add(TEXT("  (no pair measured yet)"));
	L.Add(TEXT(""));
	if (All.Work.Num() > 0)
	{
		L.Add(FString::Printf(TEXT("All top against all lowest: game work %.2f ms, GPU %.2f ms, fps %+.1f; the rows one at a time add up to game work %.2f ms, GPU %.2f ms, fps %+.1f"),
			All.Mean(All.Work), All.Mean(All.Gpu), All.Mean(All.Fps), SumWork, SumGpu, SumFps));
	}
	else
	{
		L.Add(FString::Printf(TEXT("All top against all lowest: not measured; the rows one at a time add up to game work %.2f ms, GPU %.2f ms, fps %+.1f"), SumWork, SumGpu, SumFps));
	}
	// the resolution's steps: its two A phases (120), each step's phase, its two B phases (70)
	{
		double W[4] = {}, G[4] = {}, F[4] = {};
		int32 N[4] = {};
		for (const FSweepPhase& Ph : S.Phases)
		{
			if (Ph.Row != TEXT("Resolution") || !Ph.bMeasured) continue;
			const int32 k = Ph.Value == TEXT("120") ? 0 : Ph.Value == TEXT("100") ? 1 : Ph.Value == TEXT("85") ? 2 : 3;
			W[k] += Ph.M.GameWorkMs; G[k] += Ph.M.GpuMs; F[k] += Ph.M.Fps; ++N[k];
		}
		static const TCHAR* const Names[4] = { TEXT("120"), TEXT("100"), TEXT("85"), TEXT("70") };
		FString Steps;
		for (int32 k = 0; k < 4; ++k)
		{
			if (!N[k]) continue;
			Steps += FString::Printf(TEXT("%s%s %%: game work %.2f, GPU %.2f ms, fps %.1f"), Steps.IsEmpty() ? TEXT("") : TEXT(" | "), Names[k], W[k] / N[k], G[k] / N[k], F[k] / N[k]);
		}
		L.Add(FString::Printf(TEXT("Resolution's steps (the 100 and 85 phases once each, after the pairs): %s"), Steps.IsEmpty() ? TEXT("not measured") : *Steps));
	}
	L.Add(TEXT(""));
	L.Add(TEXT("Every phase (measured from the settle's end to the next switch; the buckets: the costliest parts of the game thread, ms a frame):"));
	L.Add(FString::Printf(TEXT("  %3s %-20s %-6s %2s %6s %7s %7s %7s %7s %7s %6s %6s %6s %8s  %s"), TEXT("#"), TEXT("row"), TEXT("value"), TEXT(""), TEXT("frames"), TEXT("work"), TEXT("render"),
		TEXT("RHI"), TEXT("GPU"), TEXT("fps"), TEXT("over%"), TEXT("awake"), TEXT("draws"), TEXT("tris"), TEXT("top buckets")));
	for (int32 i = 0; i < S.Phases.Num(); ++i)
	{
		const FSweepPhase& Ph = S.Phases[i];
		if (!Ph.bMeasured)
		{
			L.Add(FString::Printf(TEXT("  %3d %-20s %-6s %2s  (not measured)"), i + 1, *Ph.Row, *Ph.Value, Ph.bB ? TEXT("B") : TEXT("A")));
			continue;
		}
		FString Top;
		for (int32 b = 0; b < Ph.M.Buckets.Num() && b < 3; ++b) Top += FString::Printf(TEXT("%s%s %.2f"), b ? TEXT(", ") : TEXT(""), *Ph.M.Buckets[b].Key, Ph.M.Buckets[b].Value);
		L.Add(FString::Printf(TEXT("  %3d %-20s %-6s %2s %6lld %7.2f %7.2f %7.2f %7.2f %7.1f %6.1f %6.1f %6.0f %8.0f  %s"), i + 1, *Ph.Row, *Ph.Value, Ph.bB ? TEXT("B") : TEXT("A"), Ph.M.Frames,
			Ph.M.GameWorkMs, Ph.M.RenderMs, Ph.M.RhiMs, Ph.M.GpuMs, Ph.M.Fps, Ph.M.OverPct, Ph.Awake, Ph.M.Draws, Ph.M.Tris, Top.IsEmpty() ? TEXT("-") : *Top));
	}
	L.Add(TEXT(""));
	L.Add(TEXT("The player's settings as the sweep began (every row put back to these at its end; nothing written):"));
	TArray<FString> Dump;
	R.Dump.ParseIntoArrayLines(Dump);
	for (const FString& D : Dump) L.Add(TEXT("  ") + D);
	return FString::Join(L, TEXT("\n")) + TEXT("\n");
}
