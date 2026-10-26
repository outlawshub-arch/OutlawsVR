// REQ-SPECCAM: the PC window's Steady view (see AOutlawsPlayer::UpdateSpecCam in OutlawsPlayer.h) and its test.
#include "OutlawsPlayer.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsDeviceProfile.h"
#include "Camera/CameraComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Controller.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "ImageUtils.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "RenderCore.h"
#include "DynamicRHI.h"
#include "UnrealClient.h"
#include "UObject/UObjectIterator.h"
#include "IXRTrackingSystem.h"
#include "IHeadMountedDisplay.h"
#include "HeadMountedDisplayBase.h"
#include "DefaultSpectatorScreenController.h"
#include "OutlawsShotTests.h"

namespace
{
	TAutoConsoleVariable<float> CVarSpecFov(TEXT("ol.speccam.Fov"), 90.f,
		TEXT("REQ-SPECCAM: the Steady view's width in degrees (across the 16:9 frame)."));
	TAutoConsoleVariable<int32> CVarSpecHeight(TEXT("ol.speccam.Height"), 1080,
		TEXT("REQ-SPECCAM: the Steady view's height in pixels (the width is 16:9 of it); a change remakes the target."));
	TAutoConsoleVariable<int32> CVarSpecRate(TEXT("ol.speccam.Rate"), 1,
		TEXT("REQ-SPECCAM: draw the Steady view every Nth frame (1 every frame, 2 half the headset's rate)."));
	TAutoConsoleVariable<float> CVarSpecMinCut(TEXT("ol.speccam.MinCutoff"), 0.4f,
		TEXT("REQ-SPECCAM: the turn smoothing's cutoff (Hz) when the head is still: lower is steadier."));
	TAutoConsoleVariable<float> CVarSpecBeta(TEXT("ol.speccam.Beta"), 0.02f,
		TEXT("REQ-SPECCAM: how much faster the smoothing follows per degree a second of turn."));
	TAutoConsoleVariable<float> CVarSpecPitch(TEXT("ol.speccam.PitchScale"), 0.5f,
		TEXT("REQ-SPECCAM: the head's up and down look, times this (the view stays nearer level)."));

	constexpr float SpecPitchMax = 40.f;
	constexpr float SpecRateCutoff = 1.f;      // (the one-euro filter's speed estimate, Hz)

#if !PLATFORM_ANDROID
	// (the PC's only: the Quest build never makes the view, so these would be unused there)
	float SpecAlpha(float CutoffHz, float Dt)
	{
		const float Tau = 1.f / (2.f * PI * FMath::Max(CutoffHz, 0.01f));
		return 1.f / (1.f + Tau / FMath::Max(Dt, 1e-4f));
	}

	// one step of a one-euro filter on an angle (degrees): little movement is held still, a real turn followed quickly
	void SpecFilter(float& Value, float& Rate, float& PrevRaw, float Raw, float Dt)
	{
		const float RawRate = FMath::FindDeltaAngleDegrees(PrevRaw, Raw) / FMath::Max(Dt, 1e-4f);
		PrevRaw = Raw;
		Rate += SpecAlpha(SpecRateCutoff, Dt) * (RawRate - Rate);
		const float Cut = CVarSpecMinCut.GetValueOnGameThread() + CVarSpecBeta.GetValueOnGameThread() * FMath::Abs(Rate);
		Value = FRotator::NormalizeAxis(Value + SpecAlpha(Cut, Dt) * FMath::FindDeltaAngleDegrees(Value, Raw));
	}
#endif

	ISpectatorScreenController* SpecScreen()
	{
		if (!GEngine || !GEngine->XRSystem.IsValid()) return nullptr;
		IHeadMountedDisplay* Hmd = GEngine->XRSystem->GetHMDDevice();
		return Hmd ? Hmd->GetSpectatorScreenController() : nullptr;
	}

#if !PLATFORM_ANDROID
	// the spectator screen shows our target, cropped about its middle to the window's shape (the stock Texture mode stretches)
	void SpecShowOnWindow(ISpectatorScreenController* Ctrl)
	{
		IHeadMountedDisplay* Hmd = GEngine->XRSystem->GetHMDDevice();
		Ctrl->SetSpectatorScreenMode(ESpectatorScreenMode::Texture);
		// the OpenXR HMD (the only XR plugin here) is an FHeadMountedDisplayBase with the default controller (as -OLRightEye)
		FDefaultSpectatorScreenController* Def = static_cast<FDefaultSpectatorScreenController*>(Ctrl);
		FHeadMountedDisplayBase* Base = static_cast<FHeadMountedDisplayBase*>(Hmd);
		ENQUEUE_RENDER_COMMAND(OutlawsSpecCamWindow)([Def, Base](FRHICommandListImmediate&)
		{
			FSpectatorScreenRenderDelegate* D = Def->GetSpectatorScreenRenderDelegate_RenderThread();
			if (!D) return;
			D->BindLambda([Base](FRHICommandListImmediate& Cmd, FTexture2DRHIRef Target, FTexture2DRHIRef, FTexture2DRHIRef Spec, FVector2D Window)
			{
				if (!Spec.IsValid() || !Target.IsValid()) return;
				FIntRect Src(0, 0, Spec->GetSizeX(), Spec->GetSizeY());
				const float WinAspect = Window.Y > 0.f ? float(Window.X / Window.Y) : float(Target->GetSizeX()) / FMath::Max(1u, Target->GetSizeY());
				const float SrcAspect = float(Src.Width()) / FMath::Max(1, Src.Height());
				if (SrcAspect > WinAspect)
				{
					const int32 W = int32(Src.Height() * WinAspect);
					Src.Min.X = (Src.Width() - W) / 2;
					Src.Max.X = Src.Min.X + W;
				}
				else
				{
					const int32 H = int32(Src.Width() / WinAspect);
					Src.Min.Y = (Src.Height() - H) / 2;
					Src.Max.Y = Src.Min.Y + H;
				}
				Base->CopyTexture_RenderThread(Cmd, Spec, Src, Target, FIntRect(0, 0, Target->GetSizeX(), Target->GetSizeY()), false, true);
			});
		});
	}
#endif
}

bool AOutlawsPlayer::SpecCamDesktop()
{
	static const bool bDesk = OLFlags::Has(TEXT("OLSpecCamTest")) || OLFlags::Param(TEXT("OLSpecCamDesk"));
	return bDesk;
}

void AOutlawsPlayer::CycleSpecView()
{
#if !PLATFORM_ANDROID
	if (!bVR && !SpecCamDesktop()) return;           // (the desktop game: its window is already its own view)
	const int32 Next = (OutlawsDeviceProfile::PcWindowView() + 1) % OutlawsDeviceProfile::PcWindowViewCount;
	OutlawsDeviceProfile::SetPcWindowView(Next);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT speccam: PC window view %s (key)"), OutlawsDeviceProfile::PcWindowViewName(Next));
	ShowMessage(FString::Printf(TEXT("PC window: %s view"), OutlawsDeviceProfile::PcWindowViewName(Next)), 2.f);
	if (bMenuOpen) RefreshMenuRows();
	SaveSettings();
#endif
}

void AOutlawsPlayer::DropSpecCam(const TCHAR* Why)
{
	if (ISpectatorScreenController* Ctrl = SpecScreen())
	{
		// back to the stock mirror (SingleEyeCroppedToFill rebinds the stock copy); -OLRightEye's hook comes back at once
		Ctrl->SetSpectatorScreenTexture(nullptr);
		Ctrl->SetSpectatorScreenMode(ESpectatorScreenMode::SingleEyeCroppedToFill);
	}
	RightEyeMirrorWait = 0.f;
	if (SpecCapture)
	{
		SpecCapture->TextureTarget = nullptr;
		SpecCapture->DestroyComponent();
		SpecCapture = nullptr;
	}
	if (SpecTarget)
	{
		SpecTarget->ReleaseResource();
		SpecTarget->MarkAsGarbage();
		SpecTarget = nullptr;
	}
	bSpecInit = false;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT speccam: Steady view off (%s): capture and target destroyed"), Why);
}

void AOutlawsPlayer::UpdateSpecCam(float Dt)
{
#if PLATFORM_ANDROID
	return;                                          // (never on the Quest)
#else
	const bool bWant = OutlawsDeviceProfile::PcWindowView() == 1 && (bVR || SpecCamDesktop()) && Camera && GetWorld();
	if (!bWant)
	{
		if (SpecCapture || SpecTarget) DropSpecCam(TEXT("view Headset"));
		return;
	}
	const int32 H = FMath::Clamp(CVarSpecHeight.GetValueOnGameThread(), 240, 2160);
	const int32 W = (H * 16 + 8) / 9;
	if (SpecTarget && (SpecTarget->SizeX != W || SpecTarget->SizeY != H)) DropSpecCam(TEXT("new size"));
	if (!SpecCapture)
	{
		SpecTarget = NewObject<UTextureRenderTarget2D>(this, MakeUniqueObjectName(this, UTextureRenderTarget2D::StaticClass(), TEXT("SpecCamTarget")));
		// (8-bit, stored as written, display gamma 2.2: the capture's tonemapper writes the window's own values, which the
		// spectator screen copies as they are.  Gamma 1 left them linear (dark, oversaturated); an sRGB target encoded
		// them twice (washed out).)
		SpecTarget->RenderTargetFormat = RTF_RGBA8;
		SpecTarget->ClearColor = FLinearColor::Black;
		SpecTarget->bAutoGenerateMips = false;
		SpecTarget->TargetGamma = 2.2f;
		SpecTarget->InitCustomFormat(W, H, PF_B8G8R8A8, true);
		SpecCapture = NewObject<USceneCaptureComponent2D>(this, MakeUniqueObjectName(this, USceneCaptureComponent2D::StaticClass(), TEXT("SpecCamCapture")));
		SpecCapture->SetupAttachment(RootComponent);
		SpecCapture->SetUsingAbsoluteLocation(true);
		SpecCapture->SetUsingAbsoluteRotation(true);
		SpecCapture->SetUsingAbsoluteScale(true);
		SpecCapture->TextureTarget = SpecTarget;
		SpecCapture->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
		SpecCapture->bCaptureEveryFrame = false;
		SpecCapture->bCaptureOnMovement = false;
		SpecCapture->bAlwaysPersistRenderingState = true;     // (the temporal history kept between our own captures)
		SpecCapture->ShowFlags.SetMotionBlur(false);
		SpecCapture->RegisterComponent();
		SpecMirrorWait = 0.f;
		SpecFrame = 0;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT speccam: Steady view on: %dx%d, %.0f degrees, every %d frame(s)"), W, H,
			CVarSpecFov.GetValueOnGameThread(), FMath::Max(1, CVarSpecRate.GetValueOnGameThread()));
	}
	// the head's pose: the place as it is, the turn smoothed, the horizon level, the up and down look damped
	const FRotator Raw = bSpecTestHead ? SpecTestHead : Camera->GetComponentRotation();
	const float PitchWant = FMath::Clamp(FRotator::NormalizeAxis(Raw.Pitch) * CVarSpecPitch.GetValueOnGameThread(), -SpecPitchMax, SpecPitchMax);
	if (!bSpecInit)
	{
		bSpecInit = true;
		SpecYaw = SpecRawYaw = Raw.Yaw;
		SpecPitch = SpecRawPitch = PitchWant;
		SpecYawRate = SpecPitchRate = 0.f;
	}
	else
	{
		SpecFilter(SpecYaw, SpecYawRate, SpecRawYaw, Raw.Yaw, Dt);
		SpecFilter(SpecPitch, SpecPitchRate, SpecRawPitch, PitchWant, Dt);
	}
	const FRotator Out(SpecPitch, SpecYaw, 0.f);
	SpecLastRaw = Raw;
	SpecLastOut = Out;
	SpecCapture->SetWorldLocationAndRotation(Camera->GetComponentLocation(), Out);
	SpecCapture->FOVAngle = FMath::Clamp(CVarSpecFov.GetValueOnGameThread(), 40.f, 140.f);
	// the camera's own look (the gamma grade, the underwater blur, hurt and death tones) as the headset sees it
	SpecCapture->PostProcessSettings = Camera->PostProcessSettings;
	SpecCapture->PostProcessBlendWeight = Camera->PostProcessBlendWeight;
	if (SpecFrame++ % uint32(FMath::Max(1, CVarSpecRate.GetValueOnGameThread())) == 0) SpecCapture->CaptureSceneDeferred();
	// the window: our target in place of the eye (checked each second: anything that sets the mode puts the stock copy back)
	SpecMirrorWait -= Dt;
	if (SpecMirrorWait <= 0.f)
	{
		SpecMirrorWait = 1.f;
		if (ISpectatorScreenController* Ctrl = SpecScreen())
		{
			if (Ctrl->GetSpectatorScreenMode() != ESpectatorScreenMode::Texture || Ctrl->GetSpectatorScreenTexture() != SpecTarget)
			{
				Ctrl->SetSpectatorScreenTexture(SpecTarget);
				SpecShowOnWindow(Ctrl);
				UE_LOG(LogOutlaws, Log, TEXT("OLRT speccam: the PC window shows the Steady view"));
			}
		}
	}
#endif
}

// ---- -OLShots -OLSpecCamTest=shots|perf (REQ-SPECCAM) -------------------------------------------------------------------
// shots: the head held at the level's start and shaken (a slow turn, a quick small shake in yaw and pitch, a slow roll),
// the PC window's view cycled as the V key does: Headset (a shot of the window), Steady (a shot of the Steady target,
// the smoothing measured over 5 s: frame to frame rotation against the raw head's, roll, pitch), Headset again (nothing
// of ours left after a garbage collection), Steady and back once more.  perf: the head turning 36 degrees a second, no
// shake, 10 s phases off, on, off, on, off, on (the first 1.5 s of each not counted); GPU, frame, game and render thread
// ms per phase, then the cost on minus off.

#if WITH_OUTLAWS_TESTS
void AOutlawsPlayer::SpecCamTestTick(float Dt)
{
#if !PLATFORM_ANDROID
	AOutlawsLevelActor* L = Level.Get();
	if (!L) return;
	static int32 Fails = 0, Step = 0, Phase = 0;
	static float At = 0.f, Quiet = -1.f, BaseYaw = 0.f, T = 0.f;
	static bool bPerf = OLFlags::ValueIs(TEXT("OLSpecCamTest"), TEXT("perf"));
	struct FAcc { int32 N = 0; double Gpu = 0, Frame = 0, Game = 0, Render = 0; TArray<float> Gpus; };
	static FAcc Acc[6];
	static double RawStep = 0, OutStep = 0, RawJerk = 0, OutJerk = 0, MaxRoll = 0, MaxRawPitch = 0, MaxOutPitch = 0;
	static FRotator PrevRaw, PrevOut;
	static double PrevRawD = -1, PrevOutD = -1, RawTurn = 0, OutTurn = 0;
	static int32 Samples = 0;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT speccam check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (!bOk) ++Fails;
	};
	auto Finish = [this]()
	{
		OutlawsDeviceProfile::SetPcWindowView(0);
		UpdateSpecCam(0.f);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT speccam RESULT %s (%d failed)"), Fails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Fails);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	};
	auto Ours = [this](int32& Captures, int32& Targets)
	{
		Captures = Targets = 0;
		for (TObjectIterator<USceneCaptureComponent2D> It; It; ++It) if (It->GetWorld() == GetWorld() && It->GetName().StartsWith(TEXT("SpecCam")) && !It->IsBeingDestroyed() && IsValid(*It)) ++Captures;
		for (TObjectIterator<UTextureRenderTarget2D> It; It; ++It) if (It->GetName().StartsWith(TEXT("SpecCam")) && IsValid(*It)) ++Targets;
	};
	auto EveryFrameCaptures = [this]()
	{
		int32 N = 0;
		for (TObjectIterator<USceneCaptureComponent2D> It; It; ++It) if (It->GetWorld() == GetWorld() && IsValid(*It) && It->IsRegistered()) ++N;
		return N;
	};
	auto Shot = [&L](const TCHAR* Name)
	{
		const FString File = FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("ol_speccam_%s_%s.png"), *L->LevelName, Name);
		FScreenshotRequest::RequestScreenshot(File, false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT speccam: window shot %s"), *File);
	};
	auto SaveTarget = [this, &L](const TCHAR* Name)
	{
		const FString File = FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("ol_speccam_%s_%s.png"), *L->LevelName, Name);
		bool bOk = false;
		if (SpecTarget)
		{
			if (FArchive* Ar = IFileManager::Get().CreateFileWriter(*File))
			{
				bOk = FImageUtils::ExportRenderTarget2DAsPNG(SpecTarget, *Ar);
				delete Ar;
			}
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT speccam: Steady target %s (%s)"), *File, bOk ? TEXT("saved") : TEXT("FAILED"));
		return bOk;
	};
	if (Step == 0)
	{
		if (ShotTime < 3.f || !TestShadersReady(Quiet, 1.f)) return;
		OutlawsDeviceProfile::SetPcWindowView(0);
		BaseYaw = GetActorRotation().Yaw;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT speccam: %s on %s, stereo %d, vr %d"), bPerf ? TEXT("perf") : TEXT("shots"), *L->LevelName,
			GEngine && GEngine->StereoRenderingDevice.IsValid() && GEngine->StereoRenderingDevice->IsStereoEnabled() ? 1 : 0, bVR ? 1 : 0);
		At = ShotTime;
		T = 0.f;
		Step = 1;
	}
	T += Dt;
	// the head: held in place; perf turns it evenly, shots shakes it too
	GetCharacterMovement()->Velocity = FVector::ZeroVector;
	FRotator Head;
	if (bPerf)
	{
		const float PhaseT = ShotTime - At;
		Head = FRotator(0.0, BaseYaw + 36.0 * PhaseT, 0.0);
	}
	else
	{
		Head = FRotator(4.0 + 3.0 * FMath::Sin(T * 2.f * PI * 4.3f) + 1.5 * FMath::Sin(T * 2.f * PI * 7.1f),
			BaseYaw + 15.0 * T + 2.5 * FMath::Sin(T * 2.f * PI * 5.2f) + 1.2 * FMath::Sin(T * 2.f * PI * 8.7f),
			12.0 * FMath::Sin(T * 2.f * PI * 0.4f));
	}
	if (Controller) Controller->SetControlRotation(Head);
	bSpecTestHead = true;
	SpecTestHead = Head;
	int32 Caps = 0, Tgts = 0;
	if (bPerf)
	{
		// phases 0..5: off, on, off, on, off, on; 10 s each, the first 1.5 s settling
		const float PhaseLen = 10.f, Settle = 1.5f;
		const float PhaseT = ShotTime - At;
		if (PhaseT >= Settle)
		{
			FAcc& A = Acc[Phase];
			const float Gpu = FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0));
			++A.N;
			A.Gpu += Gpu;
			A.Gpus.Add(Gpu);
			A.Frame += FApp::GetDeltaTime() * 1000.0;
			A.Game += FPlatformTime::ToMilliseconds(GGameThreadTime);
			A.Render += FPlatformTime::ToMilliseconds(GRenderThreadTime);
		}
		if (PhaseT >= PhaseLen)
		{
			FAcc& A = Acc[Phase];
			A.Gpus.Sort();
			const float P95 = A.Gpus.Num() ? A.Gpus[FMath::Min(A.Gpus.Num() - 1, A.Gpus.Num() * 95 / 100)] : 0.f;
			Ours(Caps, Tgts);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT speccam perf phase %d %s: %d frames, gpu %.2f ms (p95 %.2f), frame %.2f ms, game %.2f, render %.2f; ours: %d capture, %d target"),
				Phase, Phase % 2 ? TEXT("on") : TEXT("off"), A.N, A.N ? A.Gpu / A.N : 0.0, P95, A.N ? A.Frame / A.N : 0.0, A.N ? A.Game / A.N : 0.0, A.N ? A.Render / A.N : 0.0, Caps, Tgts);
			Check((Phase % 2 == 1) == (Caps == 1 && Tgts == 1), FString::Printf(TEXT("phase %d: the Steady view's capture and target exist only while it is on"), Phase));
			if (++Phase >= 6)
			{
				double Off = 0, On = 0, OffF = 0, OnF = 0, OffR = 0, OnR = 0;
				for (int32 k = 0; k < 6; ++k)
				{
					const double G = Acc[k].N ? Acc[k].Gpu / Acc[k].N : 0.0, F = Acc[k].N ? Acc[k].Frame / Acc[k].N : 0.0, R = Acc[k].N ? Acc[k].Render / Acc[k].N : 0.0;
					(k % 2 ? On : Off) += G / 3.0;
					(k % 2 ? OnF : OffF) += F / 3.0;
					(k % 2 ? OnR : OffR) += R / 3.0;
				}
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT speccam perf %s: gpu off %.2f ms, on %.2f ms, cost %+.2f ms; frame off %.2f, on %.2f (%+.2f); render thread off %.2f, on %.2f (%+.2f); height %d, rate %d"),
					*L->LevelName, Off, On, On - Off, OffF, OnF, OnF - OffF, OffR, OnR, OnR - OffR, CVarSpecHeight.GetValueOnGameThread(), CVarSpecRate.GetValueOnGameThread());
				Finish();
				return;
			}
			if (Phase % 2) CycleSpecView();          // off -> on (the key's own path)
			else CycleSpecView();                    // on -> off
			At = ShotTime;
			Check(OutlawsDeviceProfile::PcWindowView() == Phase % 2, FString::Printf(TEXT("phase %d: the view is %s"), Phase, OutlawsDeviceProfile::PcWindowViewName(Phase % 2)));
		}
		return;
	}
	const float S = ShotTime - At;
	switch (Step)
	{
	case 1:                                          // Headset: nothing of ours, a shot of the window
		if (S < 2.f) return;
		Ours(Caps, Tgts);
		Check(!SpecCapture && !SpecTarget && Caps == 0 && Tgts == 0, FString::Printf(TEXT("Headset: no Steady capture or target (%d, %d; scene captures registered in the world: %d)"), Caps, Tgts, EveryFrameCaptures()));
		Check(!FindComponentByClass<USceneCaptureComponent2D>() || FindComponentByClass<USceneCaptureComponent2D>() == ScopeCapture, TEXT("Headset: the player carries no capture but the scope's"));
		Shot(TEXT("1_headset"));
		Step = 2;
		At = ShotTime;
		return;
	case 2:                                          // the key: Steady
		if (S < 1.f) return;
		CycleSpecView();
		Check(OutlawsDeviceProfile::PcWindowView() == 1, TEXT("the key: Steady"));
		Step = 3;
		At = ShotTime;
		Samples = 0;
		RawStep = OutStep = RawJerk = OutJerk = MaxRoll = MaxRawPitch = MaxOutPitch = RawTurn = OutTurn = 0;
		PrevRawD = PrevOutD = -1;
		return;
	case 3:                                          // Steady: measured for 6 s, shots at 3 s
		Ours(Caps, Tgts);
		if (S < 0.5f) return;
		if (S >= 1.f && S < 6.f && SpecCapture)
		{
			if (Samples > 0)
			{
				const double DR = FMath::RadiansToDegrees(PrevRaw.Quaternion().AngularDistance(SpecLastRaw.Quaternion()));
				const double DO = FMath::RadiansToDegrees(PrevOut.Quaternion().AngularDistance(SpecLastOut.Quaternion()));
				RawStep += DR;
				OutStep += DO;
				if (PrevRawD >= 0) { RawJerk += FMath::Abs(DR - PrevRawD); OutJerk += FMath::Abs(DO - PrevOutD); }
				PrevRawD = DR;
				PrevOutD = DO;
				RawTurn += FMath::FindDeltaAngleDegrees(PrevRaw.Yaw, SpecLastRaw.Yaw);
				OutTurn += FMath::FindDeltaAngleDegrees(PrevOut.Yaw, SpecLastOut.Yaw);
			}
			MaxRoll = FMath::Max(MaxRoll, FMath::Abs(double(SpecLastOut.Roll)));
			MaxRawPitch = FMath::Max(MaxRawPitch, FMath::Abs(double(FRotator::NormalizeAxis(SpecLastRaw.Pitch))));
			MaxOutPitch = FMath::Max(MaxOutPitch, FMath::Abs(double(SpecLastOut.Pitch)));
			PrevRaw = SpecLastRaw;
			PrevOut = SpecLastOut;
			++Samples;
		}
		if (Phase == 0 && S >= 3.f)
		{
			Phase = 1;
			Check(SpecCapture && SpecTarget && Caps == 1 && Tgts == 1, FString::Printf(TEXT("Steady: one capture and one target (%d, %d)"), Caps, Tgts));
			Check(SpecTarget && SpecTarget->SizeX * 9 == SpecTarget->SizeY * 16, FString::Printf(TEXT("Steady: the frame is 16:9 (%dx%d)"), SpecTarget ? SpecTarget->SizeX : 0, SpecTarget ? SpecTarget->SizeY : 0));
			Check(SpecCapture && FMath::IsNearlyEqual(SpecCapture->FOVAngle, 90.f, 0.5f), FString::Printf(TEXT("Steady: 90 degrees wide (%.1f)"), SpecCapture ? SpecCapture->FOVAngle : 0.f));
			Check(SaveTarget(TEXT("2_steady")), TEXT("Steady: the target saved"));
			Shot(TEXT("2_steady_rawhead"));
		}
		if (S < 6.f) return;
		{
			const int32 N = FMath::Max(1, Samples - 1);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT speccam smoothing over %d frames: raw head %.3f deg/frame (jerk %.3f), Steady %.3f deg/frame (jerk %.3f); turned raw %.1f, Steady %.1f deg; roll max %.2f; pitch max raw %.1f, Steady %.1f"),
				Samples, RawStep / N, RawJerk / N, OutStep / N, OutJerk / N, RawTurn, OutTurn, MaxRoll, MaxRawPitch, MaxOutPitch);
			Check(Samples > 30, FString::Printf(TEXT("smoothing: frames measured (%d)"), Samples));
			Check(MaxRoll < 0.01, TEXT("smoothing: the horizon level (no roll)"));
			Check(OutJerk < 0.25 * RawJerk, TEXT("smoothing: the frame to frame change of rotation under a quarter of the raw head's"));
			Check(OutStep < 0.6 * RawStep, TEXT("smoothing: less rotation a frame than the raw head"));
			Check(FMath::Abs(OutTurn - RawTurn) < 0.2 * FMath::Abs(RawTurn) + 3.0, TEXT("smoothing: the slow turn followed"));
			Check(MaxOutPitch <= 0.6 * MaxRawPitch + 0.5, TEXT("smoothing: the up and down look damped"));
		}
		Step = 30;
		At = ShotTime;
		return;
	case 30:                                         // the pause menu in the Steady view
		if (!bMenuOpen) PressMenu();
		Step = 31;
		At = ShotTime;
		return;
	case 31:
		if (S < 1.5f) return;
		Check(bMenuOpen, TEXT("Steady: the pause menu open"));
		Check(SaveTarget(TEXT("2b_steady_menu")), TEXT("Steady: the target saved with the menu up"));
		Shot(TEXT("2b_steady_menu_rawhead"));
		Step = 32;
		At = ShotTime;
		return;
	case 32:
		if (S < 0.5f) return;
		if (bMenuOpen) CloseMenu();
		Step = 4;
		At = ShotTime;
		return;
	case 4:                                          // the key: Headset again, then everything of ours gone
		CycleSpecView();
		Check(OutlawsDeviceProfile::PcWindowView() == 0, TEXT("the key again: Headset"));
		Step = 5;
		At = ShotTime;
		return;
	case 5:
		if (S < 0.5f) return;
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		Ours(Caps, Tgts);
		Check(!SpecCapture && !SpecTarget && Caps == 0 && Tgts == 0, FString::Printf(TEXT("Headset again: the capture and the target gone after a collection (%d, %d)"), Caps, Tgts));
		Shot(TEXT("3_headset_again"));
		Step = 6;
		At = ShotTime;
		return;
	case 6:                                          // and once more round: Steady, Headset
		if (S < 1.f) return;
		CycleSpecView();
		Step = 7;
		At = ShotTime;
		return;
	case 7:
		if (S < 1.f) return;
		Ours(Caps, Tgts);
		Check(OutlawsDeviceProfile::PcWindowView() == 1 && Caps == 1 && Tgts == 1, TEXT("second round: Steady made again"));
		CycleSpecView();
		Step = 8;
		At = ShotTime;
		return;
	case 8:
		if (S < 0.5f) return;
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS);
		Ours(Caps, Tgts);
		Check(OutlawsDeviceProfile::PcWindowView() == 0 && Caps == 0 && Tgts == 0, TEXT("second round: Headset, nothing left"));
		bSpecTestHead = false;
		Finish();
		return;
	default:
		return;
	}
#endif
}

OL_SHOT_MATCH(OLSpecCamTest) { return OLFlags::Has(TEXT("OLSpecCamTest")); }
OL_SHOT_TEST(OLSpecCamTest, 200)
{
	SpecCamTestTick(Dt);             // (REQ-SPECCAM: OutlawsPlayerSpecCam.cpp)
}
#endif // WITH_OUTLAWS_TESTS
