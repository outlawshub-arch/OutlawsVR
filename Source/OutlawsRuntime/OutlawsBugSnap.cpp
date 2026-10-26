// REQ-BUGSNAP: a bug snapshot.  Both triggers and both stick clicks held together for 0.75 s (or F9 on the PC) save
// Saved/BugSnaps/<date_time>/shot.png (what the player sees, a capture from the view camera, the same on PC VR, the PC
// window and the Quest, where the backbuffer and the mirror are not to be trusted) and info.txt (the build, the place, the
// sector, the hands, the nearest actors, anything thrown, the frame rate, the settings, the session and the log's last 300
// lines).  While the chord forms the triggers read 0 and the stick clicks do nothing (no shot, run, barrels, taunt,
// recenter, perf mark or one-controller pause).  A short pulse on both hands and "Bug snapshot saved"; 3 s before the next.
// The info is gathered on the game thread (one frame); the pixels come back on the render thread and the PNG, the info
// file and the log tail are written on a pool thread.

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsCoop.h"
#include "OutlawsGeometry.h"
#include "OutlawsBuildInfo.h"
#include "Camera/CameraComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameUserSettings.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Materials/MaterialInstance.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/FileHelper.h"
#include "Misc/OutputDeviceFile.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Async/Async.h"
#include "RenderingThread.h"
#include "TextureResource.h"
#include "HAL/PlatformOutputDevices.h"
#include <atomic>

extern ENGINE_API float GAverageFPS;     // (UnrealEngine.cpp: the engine's own smoothed frame rate)
extern ENGINE_API float GAverageMS;

namespace
{
	constexpr float BugChordHold = 0.75f;
	constexpr double BugSnapCooldown = 3.0;
	constexpr int32 BugShotSize = 1440;
	// the last snapshot's picture: 0 waiting, 1 written, -1 failed; and the share of pixels that are not black (per mille)
	std::atomic<int32> GBugShotState{ 0 };
	std::atomic<int32> GBugShotLit{ 0 };
	std::atomic<int32> GBugInfoState{ 0 };

	FString MatLine(const UPrimitiveComponent* P)
	{
		FString S;
		for (int32 i = 0; P && i < P->GetNumMaterials(); ++i)
		{
			const UMaterialInterface* M = P->GetMaterial(i);
			const UMaterialInstance* Mi = Cast<UMaterialInstance>(M);
			float Bright = -1.f;
			FLinearColor Tint(-1.f, -1.f, -1.f, -1.f);
			if (M)
			{
				M->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("Brightness")), Bright);
				M->GetVectorParameterValue(FHashedMaterialParameterInfo(TEXT("Tint")), Tint);
			}
			S += FString::Printf(TEXT("[%d %s parent %s Brightness %.2f Tint %s] "), i, M ? *M->GetName() : TEXT("null"),
				Mi && Mi->Parent ? *Mi->Parent->GetName() : TEXT("-"), Bright, Tint.R < 0.f ? TEXT("none") : *Tint.ToString());
		}
		return S;
	}
}

float AOutlawsPlayer::RawAxis(const TCHAR* Name) const
{
	if (TestAxes.Num() > 0) if (const float* V = TestAxes.Find(Name)) return *V;
	return InputComponent ? GetInputAxisValue(FName(Name)) : 0.f;
}

void AOutlawsPlayer::UpdateBugSnap(float Dt)
{
	if (BugSnapTarget && GBugShotState.load() != 0) BugSnapTarget = nullptr;      // (the picture is out: the target let go)
	const float LT = RawAxis(TEXT("OLVR_LeftTrigger")), RT = RawAxis(TEXT("OLVR_RightTrigger"));
	const bool bBothTriggers = LT > 0.5f && RT > 0.5f;
	const bool bAnyTrigger = LT > 0.5f || RT > 0.5f;
	const bool bBothSticks = bStickHeld[0] && bStickHeld[1];
	const bool bAnyStick = bStickHeld[0] || bStickHeld[1];
	if (!bBugChord && OneHand() < 0 && ((bBothTriggers && bAnyStick) || (bBothSticks && bAnyTrigger)))
	{
		bBugChord = true;
		bBugChordFired = false;
		BugChordHeld = 0.f;
		UE_LOG(LogOutlaws, Log, TEXT("OLBUGSNAP chord forming: the triggers and the stick clicks do nothing until all four are let go"));
	}
	if (!bBugChord) return;
	if (bBothTriggers && bBothSticks)
	{
		BugChordHeld += Dt;
		if (!bBugChordFired && BugChordHeld >= BugChordHold)
		{
			bBugChordFired = true;
			TakeBugSnap(TEXT("both triggers and both stick clicks"));
		}
	}
	else
	{
		BugChordHeld = 0.f;
	}
	if (!bAnyStick && LT < 0.3f && RT < 0.3f)
	{
		bBugChord = false;
		bStickChord = false;
		UE_LOG(LogOutlaws, Log, TEXT("OLBUGSNAP chord let go"));
	}
}

void AOutlawsPlayer::BugSnapKey()
{
	TakeBugSnap(TEXT("F9"));
}

void AOutlawsPlayer::TakeBugSnap(const TCHAR* Why)
{
	const double Now = FPlatformTime::Seconds();
	if (Now < BugSnapReadyAt)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLBUGSNAP %s: cooling down (%.1f s left), no snapshot"), Why, BugSnapReadyAt - Now);
		return;
	}
	BugSnapReadyAt = Now + BugSnapCooldown;
	UWorld* W = GetWorld();
	if (!W || !Camera) return;
	const double T0 = FPlatformTime::Seconds();
	const FDateTime Stamp = FDateTime::Now();
	const FString Dir = OLShipLog::ReportSavedDir() / TEXT("BugSnaps") / Stamp.ToString(TEXT("%Y-%m-%d_%H-%M-%S"));
	IFileManager::Get().MakeDirectory(*Dir, true);
	LastBugSnapDir = Dir;
	++BugSnapsTaken;
	GBugShotState = 0;
	GBugShotLit = 0;
	GBugInfoState = 0;

	// ---- the picture: a capture from the view camera, read back on the render thread, written on a pool thread ----
	const FVector Eye = Camera->GetComponentLocation();
	const FRotator Look = Camera->GetComponentRotation();
	if (!BugSnapTarget)
	{
		BugSnapTarget = NewObject<UTextureRenderTarget2D>(this, MakeUniqueObjectName(this, UTextureRenderTarget2D::StaticClass(), TEXT("BugSnapTarget")));
		BugSnapTarget->RenderTargetFormat = RTF_RGBA8;
		BugSnapTarget->ClearColor = FLinearColor::Black;
		BugSnapTarget->bAutoGenerateMips = false;
		BugSnapTarget->TargetGamma = 2.2f;
		BugSnapTarget->InitCustomFormat(BugShotSize, BugShotSize, PF_B8G8R8A8, true);
		BugSnapTarget->UpdateResourceImmediate(true);
	}
	USceneCaptureComponent2D* Cap = NewObject<USceneCaptureComponent2D>(this, MakeUniqueObjectName(this, USceneCaptureComponent2D::StaticClass(), TEXT("BugSnapCapture")));
	Cap->SetUsingAbsoluteLocation(true);
	Cap->SetUsingAbsoluteRotation(true);
	Cap->SetUsingAbsoluteScale(true);
	Cap->TextureTarget = BugSnapTarget;
	Cap->CaptureSource = ESceneCaptureSource::SCS_FinalColorLDR;
	Cap->bCaptureEveryFrame = false;
	Cap->bCaptureOnMovement = false;
	Cap->ShowFlags.SetMotionBlur(false);
	Cap->FOVAngle = bVR ? 100.f : FMath::Clamp(Camera->FieldOfView, 60.f, 120.f);
	Cap->PostProcessSettings = Camera->PostProcessSettings;
	Cap->PostProcessBlendWeight = Camera->PostProcessBlendWeight;
	Cap->RegisterComponentWithWorld(W);
	Cap->SetWorldLocationAndRotation(Eye, Look);
	Cap->CaptureScene();
	Cap->DestroyComponent();
	FTextureRenderTargetResource* Res = BugSnapTarget->GameThread_GetRenderTargetResource();
	const FString ShotPath = Dir / TEXT("shot.png");
	ENQUEUE_RENDER_COMMAND(OutlawsBugSnapRead)([Res, ShotPath](FRHICommandListImmediate& Cmd)
	{
		TArray<FColor> Px;
		if (Res && Res->GetRenderTargetTexture())
		{
			Cmd.ReadSurfaceData(Res->GetRenderTargetTexture(), FIntRect(0, 0, BugShotSize, BugShotSize), Px, FReadSurfaceDataFlags(RCM_UNorm));
		}
		Async(EAsyncExecution::ThreadPool, [Px = MoveTemp(Px), ShotPath]() mutable
		{
			if (Px.Num() != BugShotSize * BugShotSize)
			{
				GBugShotState = -1;
				UE_LOG(LogOutlaws, Warning, TEXT("OLBUGSNAP shot: no pixels read back"));
				return;
			}
			int64 Lit = 0;
			for (FColor& C : Px)
			{
				C.A = 255;
				if (C.R + C.G + C.B > 24) ++Lit;
			}
			IImageWrapperModule& Mod = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
			TSharedPtr<IImageWrapper> Png = Mod.CreateImageWrapper(EImageFormat::PNG);
			bool bOk = Png.IsValid() && Png->SetRaw(Px.GetData(), Px.Num() * sizeof(FColor), BugShotSize, BugShotSize, ERGBFormat::BGRA, 8);
			if (bOk)
			{
				const TArray64<uint8>& Bytes = Png->GetCompressed();
				bOk = Bytes.Num() > 0 && FFileHelper::SaveArrayToFile(TArrayView64<const uint8>(Bytes.GetData(), Bytes.Num()), *ShotPath);
			}
			GBugShotLit = int32(Lit * 1000 / Px.Num());
			GBugShotState = bOk ? 1 : -1;
			UE_LOG(LogOutlaws, Log, TEXT("OLBUGSNAP shot %s: %dx%d, %.1f%% of pixels not black"), bOk ? TEXT("written") : TEXT("FAILED"), BugShotSize, BugShotSize, GBugShotLit.load() / 10.f);
		});
	});

	// ---- the info (gathered here, one frame) ----
	FString S;
	auto Line = [&S](const FString& L) { S += L; S += LINE_TERMINATOR; };
	AOutlawsLevelActor* L = Level.Get();
	Line(FString::Printf(TEXT("Outlaws VR bug snapshot (%s)"), Why));
	Line(TEXT("Build: ") + OutlawsBuildInfo::Line());
	Line(TEXT("Date: ") + Stamp.ToString(TEXT("%Y-%m-%d %H:%M:%S")));
	Line(FString::Printf(TEXT("Platform: %s, %s, feature level %s"), ANSI_TO_TCHAR(FPlatformProperties::IniPlatformName()), bVR ? TEXT("VR") : TEXT("desktop"),
		W->FeatureLevel == ERHIFeatureLevel::ES3_1 ? TEXT("ES3_1") : TEXT("SM5")));
	Line(FString::Printf(TEXT("Level: %s (map %s)"), L ? *L->LevelName : TEXT("none"), *W->GetMapName()));
	const FVector Loc = GetActorLocation();
	const FVector OlLoc = FOutlawsGeometry::ToOutlaws(Loc), OlEye = FOutlawsGeometry::ToOutlaws(Eye);
	Line(FString::Printf(TEXT("Player: UE location %s, rotation %s; eye %s, look %s"), *Loc.ToString(), *GetActorRotation().ToString(), *Eye.ToString(), *Look.ToString()));
	Line(FString::Printf(TEXT("Player in game units: X %.2f Y %.2f Z %.2f (eye X %.2f Y %.2f Z %.2f), heading %.1f"), OlLoc.X, OlLoc.Y, OlLoc.Z, OlEye.X, OlEye.Y, OlEye.Z,
		FRotator::ClampAxis(90.f - float(Look.Yaw))));
	if (L)
	{
		FLinearColor Rows[3], Tint;
		const bool bGrade = L->PaletteGradeAt(Eye, Rows);
		const bool bUnder = L->UnderwaterTintAt(Eye, Tint);
		Line(FString::Printf(TEXT("Sector at the feet: %s"), *L->DebugSectorPaletteAt(Loc)));
		Line(FString::Printf(TEXT("Sector at the eyes: %s; grade %s; underwater tint %s; Tinting %d"), *L->DebugSectorPaletteAt(Eye),
			bGrade ? *FString::Printf(TEXT("on (%s / %s / %s)"), *Rows[0].ToString(), *Rows[1].ToString(), *Rows[2].ToString()) : TEXT("off"),
			bUnder ? *Tint.ToString() : TEXT("none"), L->bPaletteGrades ? 1 : 0));
		Line(FString::Printf(TEXT("Water: swimming %d, head under %d"), bSwimming ? 1 : 0, bHeadUnder ? 1 : 0));
		Line(FString::Printf(TEXT("Health %.1f, armour %d"), L->GetHealth(), L->GetArmour()));
	}
	Line(TEXT("Ammo: ") + BugSnapAmmoLine());
	for (int32 H = 0; H < 2; ++H)
	{
		Line(FString::Printf(TEXT("%s hand: %s; tool %s; item %s; rounds in %d; fuse lit %d; break open %d; reloading %d; two-handed tool %d"),
			H == 0 ? TEXT("Left") : TEXT("Right"), Equipped[H] >= 0 ? *HeldGunTestName(Equipped[H]) : TEXT("bare hand"),
			HeldTool[H].IsEmpty() ? TEXT("none") : *HeldTool[H], HeldItem[H].IsValid() ? *HeldItem[H]->GetName() : TEXT("none"),
			Mag[H], bFuseLit[H] ? 1 : 0, bBreakOpen[H] ? 1 : 0, bReloadingNow[H] ? 1 : 0, bToolTwoHanded[H] ? 1 : 0));
	}
	Line(FString::Printf(TEXT("Menu open %d, wheel open %d, dead %d"), bMenuOpen ? 1 : 0, bWheelOpen ? 1 : 0, bDeadView ? 1 : 0));
	// the nearest 15 actors
	struct FNear { AActor* A; double D; };
	TArray<FNear> Near;
	for (TActorIterator<AActor> It(W); It; ++It)
	{
		AActor* A = *It;
		if (A == this || !IsValid(A) || A->IsA<AInfo>()) continue;
		const double D = FVector::Dist(A->GetActorLocation(), Loc);
		if (D < 1.0 && A->GetRootComponent() == nullptr) continue;
		Near.Add({ A, D });
	}
	Near.Sort([](const FNear& X, const FNear& Y) { return X.D < Y.D; });
	Line(FString::Printf(TEXT("Nearest actors (%d of %d):"), FMath::Min(15, Near.Num()), Near.Num()));
	for (int32 i = 0; i < Near.Num() && i < 15; ++i)
	{
		const AActor* A = Near[i].A;
		Line(FString::Printf(TEXT("  %s %s %.0f cm, %s%s"), *A->GetClass()->GetName(), *A->GetName(), Near[i].D,
			A->IsHidden() ? TEXT("hidden") : A->WasRecentlyRendered(0.2f) ? TEXT("rendered") : TEXT("not rendered"),
			*FString::Printf(TEXT(" at %s"), *A->GetActorLocation().ToString())));
	}
	// thrown things in flight
	int32 NumFlying = 0;
	for (const FThrown& T : Thrown)
	{
		if (T.bResting) continue;
		++NumFlying;
		static const TCHAR* const Types[] = { TEXT("?"), TEXT("dynamite"), TEXT("knife"), TEXT("bottle"), TEXT("prop") };
		Line(FString::Printf(TEXT("Thrown in flight: %s at %s, fuse %.1f, age %.1f s"), Types[FMath::Clamp(T.Type, 0, 4)], *T.Pos.ToString(), T.Fuse, T.Age));
		if (L)
		{
			FLinearColor Rows[3], Tint;
			const bool bGrade = L->PaletteGradeAt(T.Pos, Rows);
			const bool bUnder = L->UnderwaterTintAt(T.Pos, Tint);
			Line(FString::Printf(TEXT("  there: %s, grade %d, underwater tint %s"), *L->DebugSectorPaletteAt(T.Pos), bGrade ? 1 : 0, bUnder ? *Tint.ToString() : TEXT("none")));
		}
		if (const AActor* A = T.Actor.Get())
		{
			TInlineComponentArray<UPrimitiveComponent*> Prims(A);
			for (const UPrimitiveComponent* P : Prims) Line(FString::Printf(TEXT("  %s: %s"), *P->GetName(), *MatLine(P)));
		}
	}
	if (NumFlying == 0) Line(TEXT("Thrown in flight: none"));
	Line(FString::Printf(TEXT("Frame: %.1f fps, %.2f ms (this frame %.2f ms)"), GAverageFPS, GAverageMS, FApp::GetDeltaTime() * 1000.0));
	// settings
	if (L) Line(FString::Printf(TEXT("Game: speed %.2f, autosave every %.0f min"), L->GameSpeed, L->AutosaveMinutes));
	Line(FString::Printf(TEXT("VR: left-handed %d, one controller %d, snap turn %d, movement follows %d, Y / B reload %d"),
		LeftDominant() ? 1 : 0, OneController, bSnapTurn ? 1 : 0, int32(MoveDirection), bFaceReload ? 1 : 0));
	if (const UGameUserSettings* G = GEngine ? GEngine->GetGameUserSettings() : nullptr)
	{
		Line(FString::Printf(TEXT("Graphics: overall %d, resolution %s, window mode %d, vsync %d, frame limit %.0f"), G->GetOverallScalabilityLevel(),
			*G->GetScreenResolution().ToString(), int32(G->GetFullscreenMode()), G->IsVSyncEnabled() ? 1 : 0, G->GetFrameRateLimit()));
	}
	{
		FString Vars;
		for (const TCHAR* N : { TEXT("r.ScreenPercentage"), TEXT("r.MobileContentScaleFactor"), TEXT("vr.PixelDensity"), TEXT("r.Mobile.TonemapperFilm"), TEXT("t.MaxFPS") })
		{
			if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(N)) Vars += FString::Printf(TEXT("%s=%s "), N, *V->GetString());
		}
		IConsoleManager::Get().ForEachConsoleObjectThatStartsWith(FConsoleObjectVisitor::CreateLambda([&Vars](const TCHAR* Name, IConsoleObject* O)
		{
			if (IConsoleVariable* V = O ? O->AsVariable() : nullptr) Vars += FString::Printf(TEXT("%s=%s "), Name, *V->GetString());
		}), TEXT("ol."));
		Line(TEXT("Settings (console variables): ") + Vars);
	}
	FString Session = TEXT("solo");
	if (const UOutlawsCoop* C = UOutlawsCoop::Get(this))
	{
		if (C->IsHost()) Session = TEXT("co-op host");
		else if (C->IsClient()) Session = TEXT("co-op client");
		if (C->IsMatch()) Session += TEXT(", match");
	}
	Line(TEXT("Session: ") + Session);
	const double GatherMs = (FPlatformTime::Seconds() - T0) * 1000.0;
	Line(FString::Printf(TEXT("(gathered in %.2f ms)"), GatherMs));

	// the info file and the log's last 300 lines, on a pool thread
	if (GLog) GLog->Flush();
	const FString LogFile = OLShipLog::FileName();      // (REQ-SHIPLOG: the game's own in Shipping)
	const FString InfoPath = Dir / TEXT("info.txt");
	Async(EAsyncExecution::ThreadPool, [S = MoveTemp(S), LogFile, InfoPath]() mutable
	{
		FString Log;
		S += LINE_TERMINATOR;
		if (FFileHelper::LoadFileToString(Log, *LogFile, FFileHelper::EHashOptions::None, FILEREAD_AllowWrite))
		{
			TArray<FString> Lines;
			Log.ParseIntoArrayLines(Lines, false);
			const int32 From = FMath::Max(0, Lines.Num() - 300);
			S += FString::Printf(TEXT("---- the last %d lines of %s ----"), Lines.Num() - From, *LogFile) + LINE_TERMINATOR;
			for (int32 i = From; i < Lines.Num(); ++i) { S += Lines[i]; S += LINE_TERMINATOR; }
		}
		else
		{
			S += FString::Printf(TEXT("---- the log (%s) could not be read ----"), *LogFile) + LINE_TERMINATOR;
		}
		const bool bOk = FFileHelper::SaveStringToFile(S, *InfoPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		GBugInfoState = bOk ? 1 : -1;
		UE_LOG(LogOutlaws, Log, TEXT("OLBUGSNAP info %s (%d characters)"), bOk ? TEXT("written") : TEXT("FAILED"), S.Len());
	});

	Pulse(0, 0.6f);
	Pulse(1, 0.6f);
	ShowMessage(TEXT("Bug snapshot saved"), 2.f);
	UE_LOG(LogOutlaws, Log, TEXT("OLBUGSNAP saved (%s) to %s (full path %s); info gathered in %.2f ms"), Why, *Dir, *FPaths::ConvertRelativePathToFull(Dir), GatherMs);
}

// ---- -OLBugSnapTest ----------------------------------------------------------------------------------------------------
// -OLLevel=hideout -OLNoFrontEnd -OLBugSnapTest: in play, the chord built up from the test axes and the stick
// presses (the sticks first, then the triggers): no shot, no run toggle, no barrels, no recenter, no perf mark, no pause;
// the snapshot after 0.75 s, none again within the cooldown; then in the pause menu; then F9 (BugSnapKey).  Each folder is
// checked for a non-black shot.png and the info fields.  Prints "OLBUGSNAP check PASS|FAIL: ..." and "OLBUGSNAP RESULT".

#if WITH_OUTLAWS_TESTS
void AOutlawsPlayer::BugSnapTestTick(float Dt)
{
	static int32 Step = 0, Fails = 0, Passes = 0;
	static float At = 0.f;
	static FString Folder;
	static int32 Snaps0 = 0, Shots0 = 0;
	static bool bRun0 = false, bMenu0 = false;
	static int32 Barrel0 = 0;
	AOutlawsLevelActor* L = Level.Get();
	static float Clock = 0.f;
	Clock += Dt;
	if (!L || Clock < 12.f) return;
	At += Dt;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLBUGSNAP check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		(bOk ? Passes : Fails)++;
	};
	auto Next = [](int32 S) { Step = S; At = 0.f; };
	auto Trig = [this](float V) { TestAxes.Add(TEXT("OLVR_LeftTrigger"), V); TestAxes.Add(TEXT("OLVR_RightTrigger"), V); };
	auto CheckFolder = [&Check](const FString& Tag)
	{
		const FString Shot = Folder / TEXT("shot.png"), Info = Folder / TEXT("info.txt");
		Check(IFileManager::Get().DirectoryExists(*Folder), Tag + TEXT(": the folder ") + Folder);
		const int64 Size = IFileManager::Get().FileSize(*Shot);
		Check(GBugShotState.load() == 1 && Size > 1000, FString::Printf(TEXT("%s: shot.png written (%lld bytes)"), *Tag, Size));
		Check(GBugShotLit.load() > 100, FString::Printf(TEXT("%s: shot.png not black (%.1f%% of pixels lit)"), *Tag, GBugShotLit.load() / 10.f));
		FString Text;
		FFileHelper::LoadFileToString(Text, *Info);
		bool bAll = GBugInfoState.load() == 1;
		for (const TCHAR* Field : { TEXT("Build: "), TEXT("Date: "), TEXT("Level: "), TEXT("Player: UE location"), TEXT("Player in game units"), TEXT("Sector at the eyes"),
			TEXT("Water: "), TEXT("Health "), TEXT("Ammo: "), TEXT("Left hand: "), TEXT("Right hand: "), TEXT("Nearest actors"), TEXT("Thrown in flight"), TEXT("Frame: "),
			TEXT("VR: "), TEXT("Settings (console variables)"), TEXT("Session: "), TEXT("---- the last ") })
		{
			if (!Text.Contains(Field)) { bAll = false; UE_LOG(LogOutlaws, Display, TEXT("OLBUGSNAP info lacks \"%s\""), Field); }
		}
		Check(bAll, FString::Printf(TEXT("%s: info.txt has every field (%d characters)"), *Tag, Text.Len()));
	};
	switch (Step)
	{
	case 0:
		Equip(0, 0);          // (a revolver in each hand: a trigger that got through would fire it)
		Equip(1, 0);
		Snaps0 = BugSnapsTaken;
		Shots0 = GunFireCalls;
		bRun0 = bRunning;
		Barrel0 = (bOneBarrel[0] ? 1 : 0) | (bOneBarrel[1] ? 2 : 0);
		Trig(0.f);
		PressLeftStick();
		PressRightStick();
		Next(1);
		break;
	case 1:
		if (At < 0.2f) break;
		Trig(1.f);
		Next(2);
		break;
	case 2:
	{
		if (At < 0.4f) break;
		Check(bBugChord && Axis(TEXT("OLVR_RightTrigger")) == 0.f && Axis(TEXT("OLVR_LeftTrigger")) == 0.f, TEXT("chord forming: the triggers read 0 to the game"));
		Check(BugSnapsTaken == Snaps0, TEXT("no snapshot before 0.75 s"));
		// the guns as in VR (the desktop has none): a trigger pull would reach FireGun
		const bool bWas = bVR;
		bVR = true;
		UpdateGuns(0.f);
		bVR = bWas;
		Check(GunFireCalls == Shots0, FString::Printf(TEXT("the guns' trigger pass during the chord fires nothing (%d)"), GunFireCalls - Shots0));
		Next(3);
		break;
	}
	case 3:
		if (At < 0.6f) break;
		Check(BugSnapsTaken == Snaps0 + 1, TEXT("a snapshot after the chord held 0.75 s"));
		Folder = LastBugSnapDir;
		Next(4);
		break;
	case 4:
		if (At < 0.5f) break;
		ReleaseLeftStick();
		ReleaseRightStick();
		Trig(0.f);
		Next(5);
		break;
	case 5:
		if (At < 0.3f) break;
		if ((GBugShotState.load() == 0 || GBugInfoState.load() == 0) && At < 10.f) break;
		Check(GunFireCalls == Shots0, FString::Printf(TEXT("no shot fired (%d)"), GunFireCalls - Shots0));
		Check(bRunning == bRun0, TEXT("run not toggled"));
		Check(((bOneBarrel[0] ? 1 : 0) | (bOneBarrel[1] ? 2 : 0)) == Barrel0, TEXT("barrels not switched"));
		Check(!bMenuOpen, TEXT("no pause menu"));
		Check(!bPerfMarked, TEXT("no perf mark"));
		Check(!bBugChord && !bStickChord, TEXT("the chord let go"));
		{
			// the control: the same trigger pull with no chord does reach the gun
			Trig(1.f);
			const bool bWas = bVR;
			bVR = true;
			UpdateGuns(0.f);
			bVR = bWas;
			Check(GunFireCalls > Shots0, FString::Printf(TEXT("control: a trigger pull with no chord reaches the gun (%d)"), GunFireCalls - Shots0));
			Shots0 = GunFireCalls;
			Trig(0.f);
			bVR = true;
			UpdateGuns(0.f);      // (re-armed)
			bVR = bWas;
		}
		CheckFolder(TEXT("in play"));
		// the cooldown: the chord again at once does nothing
		PressLeftStick(); PressRightStick(); Trig(1.f);
		Next(6);
		break;
	case 6:
		if (At < 1.2f) break;
		Check(BugSnapsTaken == Snaps0 + 1, TEXT("no second snapshot within the 3 s cooldown"));
		ReleaseLeftStick(); ReleaseRightStick(); Trig(0.f);
		Next(7);
		break;
	case 7:
		if (At < 3.f) break;
		OpenMenu(false);
		Next(8);
		break;
	case 8:
		if (At < 1.f) break;
		bMenu0 = bMenuOpen;
		PressLeftStick(); PressRightStick(); Trig(1.f);
		Next(9);
		break;
	case 9:
		if (At < 1.2f) break;
		Check(BugSnapsTaken == Snaps0 + 2, TEXT("a snapshot from the pause menu"));
		Folder = LastBugSnapDir;
		ReleaseLeftStick(); ReleaseRightStick(); Trig(0.f);
		Next(10);
		break;
	case 10:
		if (At < 0.5f) break;
		Check(bMenu0 && bMenuOpen, TEXT("the pause menu stays open (the chord did not close it)"));
		if ((GBugShotState.load() == 0 || GBugInfoState.load() == 0) && At < 10.f) break;
		CheckFolder(TEXT("pause menu"));
		MenuBack();
		Next(11);
		break;
	case 11:
		if (At < 3.5f) break;
		if (bMenuOpen) MenuBack();
		bVR = false;
		BugSnapKey();
		Check(BugSnapsTaken == Snaps0 + 3, TEXT("F9 takes a snapshot"));
		Folder = LastBugSnapDir;
		Next(12);
		break;
	case 12:
		if ((GBugShotState.load() == 0 || GBugInfoState.load() == 0) && At < 10.f) break;
		CheckFolder(TEXT("F9"));
		Check(GunFireCalls == Shots0, TEXT("still no shot fired after the control"));
		// one controller (the right): its stick click held a second still opens the pause menu, and its trigger with its
		// stick held is no chord (there is no second controller)
		OneController = 2;
		Trig(1.f);
		PressRightStick();
		Next(13);
		break;
	case 13:
		if (At < 1.2f) break;
		Check(!bBugChord, TEXT("one controller: its stick and trigger held are no chord"));
		Trig(0.f);
		ReleaseRightStick();
		Check(bMenuOpen, TEXT("one controller: the stick click held a second still opens the pause menu"));
		if (bMenuOpen) MenuBack();
		OneController = 0;
		UE_LOG(LogOutlaws, Display, TEXT("OLBUGSNAP RESULT %s (%d passed, %d failed)"), Fails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		Next(14);
		FGenericPlatformMisc::RequestExit(false);
		break;
	default:
		break;
	}
}
#endif // WITH_OUTLAWS_TESTS
