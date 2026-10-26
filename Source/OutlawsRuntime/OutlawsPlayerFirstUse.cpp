// FIRSTHITCH: the first scope, shot and throw after a level loads.  -OLFirstUseProbe drives them on the desktop (any build,
// Shipping too, as -OLShipSmoke: through OLFlags, not under WITH_OUTLAWS_TESTS) and logs each one's frames against the
// level's median frame; see OutlawsPlayer.h (TickFirstUseProbe).

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsLevelActor.h"
#include "Camera/CameraComponent.h"
#include "Components/AudioComponent.h"
#include "ProceduralMeshComponent.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "RenderCore.h"
#include "RHI.h"
#include "DynamicRHI.h"

namespace
{
	// one action each, in this order, every 0.5 s; the whole list twice (the first use, then the second)
	enum class EFirstUse : uint8 { ScopeUp, ScopeDown, Equip, Fire, TntEquip, TntLight, TntThrow, KnifeEquip, KnifeThrow };
	struct FFirstUseStep { EFirstUse What; int32 Weapon; const TCHAR* Label; };
	// (-OLFirstUseScopeFirst: the rifle scoped and fired before any other shot)
	const FFirstUseStep ScopeFirstSteps[] = {
		{ EFirstUse::Equip, 1, TEXT("equip rifle") },
		{ EFirstUse::ScopeUp, 1, TEXT("scope up") },
		{ EFirstUse::Fire, 1, TEXT("fire rifle scoped") },
		{ EFirstUse::ScopeDown, 1, TEXT("scope down") },
		{ EFirstUse::Fire, 1, TEXT("fire rifle") },
		{ EFirstUse::Equip, 0, TEXT("equip revolver") },
		{ EFirstUse::Fire, 0, TEXT("fire revolver") },
	};
	const FFirstUseStep FirstUseSteps[] = {
		{ EFirstUse::Equip, 0, TEXT("equip revolver") },
		{ EFirstUse::Fire, 0, TEXT("fire revolver") },
		{ EFirstUse::Equip, 1, TEXT("equip rifle") },
		{ EFirstUse::Fire, 1, TEXT("fire rifle") },
		{ EFirstUse::ScopeUp, 1, TEXT("scope up") },
		{ EFirstUse::Fire, 1, TEXT("fire rifle scoped") },
		{ EFirstUse::ScopeDown, 1, TEXT("scope down") },
		{ EFirstUse::Equip, 2, TEXT("equip shotgun") },
		{ EFirstUse::Fire, 2, TEXT("fire shotgun") },
		{ EFirstUse::Equip, 3, TEXT("equip double") },
		{ EFirstUse::Fire, 3, TEXT("fire double") },
		{ EFirstUse::Equip, 4, TEXT("equip sawed-off") },
		{ EFirstUse::Fire, 4, TEXT("fire sawed-off") },
		{ EFirstUse::Equip, 5, TEXT("equip gatling") },
		{ EFirstUse::Fire, 5, TEXT("fire gatling") },
		{ EFirstUse::KnifeEquip, 7, TEXT("equip knife") },
		{ EFirstUse::KnifeThrow, 7, TEXT("throw knife") },
		{ EFirstUse::TntEquip, 6, TEXT("equip dynamite") },
		{ EFirstUse::TntLight, 6, TEXT("light dynamite") },
		{ EFirstUse::TntThrow, 6, TEXT("throw dynamite") },
	};
	constexpr int32 NumFirstUseSteps = UE_ARRAY_COUNT(FirstUseSteps);
	constexpr float FirstUseLead = 3.f;        // s of play after the build before the first step (the fade in done)
	constexpr float FirstUseGap = 0.5f;        // s between steps
	constexpr float FirstUseBlastWait = 6.f;   // s after the dynamite's throw for its blast
	constexpr int32 FirstUseFrames = 5;        // frames counted after each step (game, render and GPU work land in these)

	struct FFirstUseFrame { float Frame, Game, Render, Rhi, Gpu; };
	struct FFirstUseMark { FString Label; int32 Round = 0; double CallMs = 0.0; uint64 Frame = 0; TArray<FFirstUseFrame> After; };
	struct FFirstUseProbe
	{
		TWeakObjectPtr<AOutlawsLevelActor> Level;
		FString LevelName;
		float T = 0.f;
		int32 Step = 0;                         // next step (rounds of NumFirstUseSteps)
		float NextAt = 0.f;
		float BlastBy = -1.f;                   // waiting for the thrown stick's blast until then
		TWeakObjectPtr<AActor> Stick;          // the thrown stick (its actor goes with its blast)
		bool bDone = false;
		TArray<float> Quiet;                    // frames with no step in the last FirstUseFrames + 2
		uint64 LastStepFrame = 0;
		TArray<FFirstUseMark> Marks;
	};
	FFirstUseProbe GFirstUse;

	float Median(TArray<float> V)
	{
		if (V.Num() == 0) return 0.f;
		V.Sort();
		return V[V.Num() / 2];
	}
}

bool AOutlawsPlayer::FirstUseProbeOn()
{
	static const bool bOn = OLFlags::Param(TEXT("OLFirstUseProbe"));
	return bOn;
}

void AOutlawsPlayer::TickFirstUseProbe(float Dt)
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L || L->IsFrontEnd() || L->IsRebuildPending() || L->IsMoviePlaying() || bMenuOpen) return;
	FFirstUseProbe& P = GFirstUse;
	if (P.Level.Get() != L || P.LevelName != L->LevelName)
	{
		P = FFirstUseProbe();
		P.Level = L;
		P.LevelName = L->LevelName;
		UE_LOG(LogOutlaws, Display, TEXT("OLFIRSTUSE %s: probe armed (%s)"), *P.LevelName,
			OLFlags::Param(TEXT("OLOldFirstUse")) ? TEXT("old first use") : OLFlags::Param(TEXT("OLNoPrebuild")) ? TEXT("no prebuild") : TEXT("warmed"));
	}
	if (P.bDone) return;
	const float FrameMs = float(FApp::GetDeltaTime() * 1000.0);     // the frame just ended
	const FFirstUseFrame Fr{ FrameMs, float(FPlatformTime::ToMilliseconds(GGameThreadTime)), float(FPlatformTime::ToMilliseconds(GRenderThreadTime)),
		float(FPlatformTime::ToMilliseconds(GRHIThreadTime)), float(FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0))) };
	const uint64 Now = GFrameCounter;
	P.T += float(FApp::GetDeltaTime());
	for (FFirstUseMark& M : P.Marks) if (M.After.Num() < FirstUseFrames && Now > M.Frame) M.After.Add(Fr);
	if (P.Step > 0 && Now > P.LastStepFrame + FirstUseFrames + 2 && P.BlastBy < 0.f) P.Quiet.Add(FrameMs);
	else if (P.Step == 0 && P.T > FirstUseLead - 1.f) P.Quiet.Add(FrameMs);
	L->bInvincible = true;
	auto Mark = [&](const FString& Label, TFunctionRef<void()> Fn, int32 Round)
	{
		const double T0 = FPlatformTime::Seconds();
		Fn();
		FFirstUseMark M;
		M.Label = Label;
		M.Round = Round;
		M.CallMs = (FPlatformTime::Seconds() - T0) * 1000.0;
		M.Frame = Now;
		P.Marks.Add(MoveTemp(M));
		P.LastStepFrame = Now;
	};
	// the thrown stick's blast: the frame its flight ends
	if (P.BlastBy >= 0.f)
	{
		bool bFlying = false;
		for (const FThrown& Th : Thrown) if (Th.Actor.Get() == P.Stick.Get() && P.Stick.IsValid()) bFlying = true;
		if (!bFlying || P.T > P.BlastBy)
		{
			Mark(!bFlying ? TEXT("dynamite blast") : TEXT("dynamite blast (not seen)"), []() {}, (P.Step - 1) / NumFirstUseSteps + 1);
			P.Marks.Last().After.Add(Fr);      // (the blast was the last frame's flight: this frame's length holds it)
			P.BlastBy = -1.f;
			P.NextAt = P.T + FirstUseGap * 2.f;
		}
		return;
	}
	if (P.T < FMath::Max(FirstUseLead, P.NextAt)) return;
	if (P.Step >= NumFirstUseSteps * 2)
	{
		if (P.Marks.Num() > 0 && P.Marks.Last().After.Num() < FirstUseFrames) return;
		P.bDone = true;
		bScopeTestForce = false;
		Equip(1, -1);
		L->bInvincible = false;
		const float Med = Median(P.Quiet);
		UE_LOG(LogOutlaws, Display, TEXT("OLFIRSTUSE %s: median frame %.2f ms over %d quiet frames"), *P.LevelName, Med, P.Quiet.Num());
		for (const FFirstUseMark& M : P.Marks)
		{
			FFirstUseFrame Worst{ 0.f, 0.f, 0.f, 0.f, 0.f };
			FString Frs;
			for (const FFirstUseFrame& F : M.After) { if (F.Frame > Worst.Frame) Worst = F; Frs += FString::Printf(TEXT(" %.1f"), F.Frame); }
			UE_LOG(LogOutlaws, Display, TEXT("OLFIRSTUSE %s r%d %s: worst %.1f ms (%.2fx median, +%.1f ms; game %.1f render %.1f RHI %.1f GPU %.1f), call %.1f ms, frames%s"),
				*P.LevelName, M.Round, *M.Label, Worst.Frame, Med > 0.f ? Worst.Frame / Med : 0.f, Worst.Frame - Med, Worst.Game, Worst.Render, Worst.Rhi, Worst.Gpu,
				M.CallMs, *Frs);
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLFIRSTUSE %s: done"), *P.LevelName);
		return;
	}
	static const bool bScopeFirst = OLFlags::Param(TEXT("OLFirstUseScopeFirst"));
	const int32 Si = P.Step % NumFirstUseSteps;
	const FFirstUseStep& S = bScopeFirst && Si < UE_ARRAY_COUNT(ScopeFirstSteps) ? ScopeFirstSteps[Si] : FirstUseSteps[Si];
	const int32 Round = P.Step / NumFirstUseSteps + 1;
	if (P.Step == 0) L->GiveAllWeapons();
	++P.Step;
	P.NextAt = P.T + FirstUseGap;
	switch (S.What)
	{
	case EFirstUse::Equip:
	case EFirstUse::KnifeEquip:
	case EFirstUse::TntEquip:
		Mark(S.Label, [&]() { Equip(1, S.Weapon); }, Round);
		break;
	case EFirstUse::ScopeUp:
		Mark(S.Label, [&]() { if (Equipped[1] != S.Weapon) Equip(1, S.Weapon); bScopeTestForce = true; }, Round);
		break;
	case EFirstUse::ScopeDown:
		Mark(S.Label, [&]() { bScopeTestForce = false; }, Round);
		break;
	case EFirstUse::Fire:
		Mark(S.Label, [&]()
		{
			if (Equipped[1] != S.Weapon) Equip(1, S.Weapon);
			if (Mag[1] <= 0) Mag[1] = 2;
			FireCooldown[1] = 0.f;
			FireGun(1, false);
		}, Round);
		break;
	case EFirstUse::TntLight:
		Mark(S.Label, [&]()
		{
			Mag[1] = 1;
			bFuseLit[1] = true;
			Fuse[1] = 4.5f;
			if (WeaponMesh[1]) FuseSound[1] = L->PlayLoopAt(TEXT("fuse"), WeaponMesh[1]->GetComponentLocation());
			UpdateFuseLook(1);
		}, Round);
		break;
	case EFirstUse::KnifeThrow:
	case EFirstUse::TntThrow:
		Mark(S.Label, [&]()
		{
			const int32 Before = Thrown.Num();
			if (S.What == EFirstUse::KnifeThrow) bKnifeReady[1] = true;
			Mag[1] = 1;
			ThrowHeld(1);
			if (Thrown.Num() > Before)
			{
				// away along the view, not the desktop hand's still swing
				Thrown.Last().Vel = Camera->GetForwardVector() * 1400.0 + FVector(0.0, 0.0, 350.0);
				if (S.What == EFirstUse::TntThrow) { P.Stick = Thrown.Last().Actor; P.BlastBy = P.T + FirstUseBlastWait; }
			}
		}, Round);
		break;
	}
}
