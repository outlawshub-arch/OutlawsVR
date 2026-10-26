// Which headset this is, the Quest 2 profile and the Graphics page's performance rows: see OutlawsDeviceProfile.h.
#include "OutlawsDeviceProfile.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsXRRefresh.h"
#include "OutlawsDynRes.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "EngineUtils.h"
#include "IXRTrackingSystem.h"
#include "IHeadMountedDisplay.h"
#include "UnrealClient.h"
#include "Containers/Ticker.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/CoreDelegates.h"
#if PLATFORM_ANDROID
#include "Android/AndroidPlatformMisc.h"
#include <sys/system_properties.h>
#include <stdio.h>
#endif

namespace
{
	TAutoConsoleVariable<int32> CVarOldpQuest2Profile(TEXT("ol.Quest2Profile"), -1,
		TEXT("The Quest 2 performance defaults: -1 by the headset model (default), 0 off, 1 on (applies at once)"));

	const int32 OldpResSteps[] = { 70, 80, 85, 90, 100, 110, 120 };
	constexpr int32 OldpQuest2Res = 85;        // the Quest 2's render resolution, percent of the recommended eye buffer
	constexpr int32 OldpQuest2Lamps = 1;       // (Q2PROFILE: dotard's Quest 2 ran all 4) one lamp lights the room
	constexpr int32 OldpQuest2PerfLevel = 3;   // (Q2PROFILE) the CPU and GPU at sustained high (XR_EXT_performance_settings)
	constexpr int32 OldpFullLamps = 4;

	// (REQ-TRIGGERSTEP) a number row's next value among ordered Steps, the way the press steps (OutlawsMenu::StepPress): Now
	// found within Tol, or (off the grid) the next grid value that way; the ends stop or go round as the press says
	template <typename T>
	T OldpStepOrdered(const T* Steps, int32 Num, T Now, T Tol)
	{
		if (Num <= 0) return Now;
		const bool bUp = OutlawsMenu::StepPress().Dir >= 0;
		int32 k = INDEX_NONE;
		for (int32 i = 0; i < Num; ++i) if (FMath::Abs(Steps[i] - Now) <= Tol) k = i;
		if (k == INDEX_NONE)
		{
			if (bUp) { for (int32 i = 0; i < Num; ++i) if (Steps[i] > Now) return Steps[i]; k = Num - 1; }
			else { for (int32 i = Num - 1; i >= 0; --i) if (Steps[i] < Now) return Steps[i]; k = 0; }
		}
		return Steps[OutlawsMenu::StepIndex(k, Num, true)];
	}

	FString GOldpModel;
	FString GOldpHardware, GOldpGpu, GOldpSignal;   // (Q2PROFILE) ro.hardware, the GPU, the signal that decided
	OutlawsDeviceProfile::EHeadset GOldpHeadset = OutlawsDeviceProfile::EHeadset::Unknown;
	bool bOldpModelIsQuest2 = false;
	bool bOldpModelIsQuest3 = false;          // (REQ-REFRESH) the Quest 3 and 3S: 120 Hz offered, 90 by default
	bool bOldpQuest2 = false;                 // the profile in effect (model or override)
	bool bOldpQuest2Mode = false;             // (QSPEED-5) the Graphics page's Quest 2 Mode: the Quest 2 profile on any headset

	// the player's choices (-1: not chosen, the headset's default)
	int32 OldpChosenRes = -1;
	float OldpChosenHz = -1.f;
	int32 OldpChosenLamps = -1;
	bool bOldpPerfHud = false;
	bool bOldpGrabGlow = true;           // (REQ-HALOOPT) the grab glow drawn
	int32 OldpPcView = 0;                // (REQ-SPECCAM) the PC window: 0 Headset (the mirror), 1 Steady
	const TCHAR* const OldpPcViewName[] = { TEXT("Headset"), TEXT("Steady") };
	int32 OldpChosenDetail = -1;         // (CPU1) Object Detail: 0 Near, 1 Normal, 2 Far
	const float OldpDetailScale[] = { 0.6f, 1.f, 1.6f };
	const TCHAR* const OldpDetailName[] = { TEXT("Near"), TEXT("Normal"), TEXT("Far") };
	constexpr float OldpQuest2Detail = 0.8f;
	// (REQ-GAMMA) Brightness: the picture's gamma, 1.0 the look as made; 0.7 up to 1.6 in tenths
	float OldpGamma = 1.f;
	constexpr float OldpGammaLo = 0.7f, OldpGammaHi = 1.6f, OldpGammaStep = 0.1f;

	// what we changed, to put back when a row returns to a default that does not touch it
	bool bOldpPdTouched = false;
	float OldpPdOriginal = 1.f;
	float OldpHzAsked = 0.f;
	bool bOldpFpsTouched = false;
	float OldpFpsOriginal = 72.f;

	FString OldpEyeText;
	FTSTicker::FDelegateHandle OldpEyeTick;

	// Q2PROFILE step 2: the app resume (the render and clock state put back)
	int32 GOldpResumes = 0;
	double OldpLastResume = -100.0;
	int32 OldpFovLevel0 = 0, OldpFovDynamic0 = 0;     // the foveation the device profile left at startup
	FTSTicker::FDelegateHandle OldpResumeTick;
	FTSTicker::FDelegateHandle OldpTestTick;
	FDelegateHandle OldpFgHandle, OldpReactHandle;

	int32 OldpCvarInt(const TCHAR* Name, int32 Def = -1)
	{
		IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(Name);
		return V ? V->GetInt() : Def;
	}

	// the CPU and GPU clocks as Linux shows them (on the Quest the GPU's file may be closed to apps: "?")
	FString OldpClocksText()
	{
#if PLATFORM_ANDROID
		auto ReadNum = [](const char* Path) -> int64
		{
			FILE* F = fopen(Path, "r");
			if (!F) return -1;
			long long V = -1;
			if (fscanf(F, "%lld", &V) != 1) V = -1;
			fclose(F);
			return int64(V);
		};
		const int64 Little = ReadNum("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
		const int64 Big = ReadNum("/sys/devices/system/cpu/cpu7/cpufreq/scaling_cur_freq");
		const int64 Gpu = ReadNum("/sys/class/kgsl/kgsl-3d0/gpuclk");
		auto Mhz = [](int64 V, int64 Div) { return V > 0 ? FString::Printf(TEXT("%lld MHz"), (long long)(V / Div)) : FString(TEXT("?")); };
		return FString::Printf(TEXT("clocks cpu0 %s cpu7 %s gpu %s"), *Mhz(Little, 1000), *Mhz(Big, 1000), *Mhz(Gpu, 1000000));
#else
		return TEXT("clocks n/a (desktop)");
#endif
	}

	FString OldpStateText()
	{
		IConsoleVariable* Pd = IConsoleManager::Get().FindConsoleVariable(TEXT("vr.PixelDensity"));
		IHeadMountedDisplay* Hmd = GEngine && GEngine->XRSystem.IsValid() ? GEngine->XRSystem->GetHMDDevice() : nullptr;
		const float HzNow = OutlawsXRRefresh::CurrentRate();
		const FString HzText = HzNow > 0.f ? FString::Printf(TEXT("%.0f Hz"), HzNow) : FString(TEXT("n/a"));
		const FString HmdPd = Hmd ? FString::Printf(TEXT("%.2f"), Hmd->GetPixelDenity()) : FString(TEXT("n/a"));
		return FString::Printf(TEXT("refresh %s (asked %.0f), vr.PixelDensity %.2f, headset pixel density %s, foveation level %d dynamic %d, %s, %s"),
			*HzText, OutlawsXRRefresh::Wanted(), Pd ? Pd->GetFloat() : -1.f, *HmdPd,
			OldpCvarInt(TEXT("vr.VRS.HMDFixedFoveationLevel")), OldpCvarInt(TEXT("vr.VRS.HMDFixedFoveationDynamic")),
			*OutlawsXRPerf::StatusText(), *OldpClocksText());
	}

	void OldpOnForeground() { OutlawsDeviceProfile::OnAppResume(TEXT("app resumed")); }
	void OldpStartTest();       // -OLQ2ProfileTest (at the end of the file)

	bool OldpDecide()
	{
		const int32 Force = CVarOldpQuest2Profile.GetValueOnGameThread();
		return Force >= 0 ? Force > 0 : bOldpModelIsQuest2 || bOldpQuest2Mode;
	}

	const TCHAR* OldpProfileName() { return bOldpQuest2 ? TEXT("Quest2") : TEXT("default"); }

	FString OldpSource()
	{
		const int32 Force = CVarOldpQuest2Profile.GetValueOnGameThread();
		if (Force < 0) return bOldpQuest2Mode && !bOldpModelIsQuest2 ? TEXT("Quest 2 Mode on the Graphics page") : TEXT("by model");
		return FString::Printf(TEXT("forced %s (ol.Quest2Profile %d)"), Force > 0 ? TEXT("on") : TEXT("off"), Force);
	}

	void OldpLogEyeBuffer()
	{
		IConsoleVariable* Pd = IConsoleManager::Get().FindConsoleVariable(TEXT("vr.PixelDensity"));
		const float Want = Pd ? Pd->GetFloat() : 1.f;
		IHeadMountedDisplay* Hmd = GEngine && GEngine->XRSystem.IsValid() ? GEngine->XRSystem->GetHMDDevice() : nullptr;
		FViewport* Vp = GEngine && GEngine->GameViewport ? GEngine->GameViewport->Viewport : nullptr;
		if (!Hmd || !GEngine->XRSystem->IsHeadTrackingAllowed())
		{
			OldpEyeText = TEXT("no headset");
			UE_LOG(LogOutlaws, Log, TEXT("OLRT eye buffer: no headset (desktop run), vr.PixelDensity %.2f has nothing to size; window %s"),
				Want, Vp ? *Vp->GetSizeXY().ToString() : TEXT("?"));
			return;
		}
		const FIntPoint Ideal = Hmd->GetIdealRenderTargetSize();
		const FIntPoint Rt = Vp ? Vp->GetRenderTargetTextureSizeXY() : FIntPoint::ZeroValue;
		OldpEyeText = FString::Printf(TEXT("%dx%d"), Rt.X, Rt.Y);
		UE_LOG(LogOutlaws, Log, TEXT("OLRT eye buffer: render target %dx%d (pixel density %.2f, vr.PixelDensity %.2f; the runtime recommends %dx%d at 1.0)"),
			Rt.X, Rt.Y, Hmd->GetPixelDenity(), Want, Ideal.X, Ideal.Y);
	}

	void OldpScheduleEyeLog()
	{
		// a moment later: the cvar sink hands vr.PixelDensity to the headset next frame, and the swapchain follows
		if (OldpEyeTick.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(OldpEyeTick);
		OldpEyeTick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
		{
			OldpEyeTick.Reset();
			OldpLogEyeBuffer();
			return false;
		}), 2.0f);
	}

	void OldpNotifyLevels()
	{
		if (!GEngine) return;
		for (const FWorldContext& C : GEngine->GetWorldContexts())
		{
			UWorld* W = C.World();
			if (!W || !W->IsGameWorld()) continue;
			for (TActorIterator<AOutlawsLevelActor> It(W); It; ++It) It->ApplyHDDefault();
		}
	}

	// the profile decided again (the console variable, the Quest 2 Mode row, the settings file): applied when it changed
	bool OldpRedecide(bool bApply, bool bLogSame = false)
	{
		const bool bWas = bOldpQuest2;
		bOldpQuest2 = OldpDecide();
		if (bLogSame || bWas != bOldpQuest2)
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLRT headset: %s, profile %s (%s)%s"), *GOldpModel, OldpProfileName(), *OldpSource(), bWas == bOldpQuest2 ? TEXT(", unchanged") : TEXT(""));
		}
		if (bWas == bOldpQuest2) return false;
		if (bApply) OutlawsDeviceProfile::Apply(TEXT("profile changed"));
		OldpNotifyLevels();
		return true;
	}

	void OldpOnProfileCvar(IConsoleVariable*) { OldpRedecide(true, true); }

	int32 OldpParseChoice(const TMap<FString, FString>& S, const TCHAR* Key, int32 Lo, int32 Hi)
	{
		const FString* V = S.Find(Key);
		if (!V || V->IsEmpty() || !V->IsNumeric()) return -1;    // ("auto")
		return FMath::Clamp(FCString::Atoi(**V), Lo, Hi);
	}

	// ol.gfx.Set <resolution|refresh|lamps|perfhud> <number|auto>: a row from the console (adb tests), not saved
	void OldpConsoleSet(const TArray<FString>& Args)
	{
		if (Args.Num() < 2)
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLRT graphics: ol.gfx.Set <resolution|refresh|lamps|perfhud> <number|auto>"));
			return;
		}
		const FString Row = Args[0].ToLower();
		const bool bAuto = Args[1].Equals(TEXT("auto"), ESearchCase::IgnoreCase);
		const float V = FCString::Atof(*Args[1]);
		if (Row == TEXT("resolution")) OldpChosenRes = bAuto ? -1 : FMath::Clamp(FMath::RoundToInt(V), 50, 150);
		else if (Row == TEXT("refresh")) OldpChosenHz = bAuto ? -1.f : V;
		else if (Row == TEXT("lamps")) OldpChosenLamps = bAuto ? -1 : FMath::Clamp(FMath::RoundToInt(V), 0, 4);
		else if (Row == TEXT("perfhud")) bOldpPerfHud = !bAuto && V > 0.f;
		OutlawsDeviceProfile::Apply(TEXT("console"));
	}
	FAutoConsoleCommand CmdOldpGfxSet(TEXT("ol.gfx.Set"), TEXT("Set a Graphics page row (resolution, refresh, lamps, perfhud) to a number or auto; not saved"),
		FConsoleCommandWithArgsDelegate::CreateStatic(&OldpConsoleSet));
}

const TCHAR* OutlawsDeviceProfile::HeadsetName(EHeadset H)
{
	switch (H)
	{
	case EHeadset::Quest1: return TEXT("Quest 1");
	case EHeadset::Quest2: return TEXT("Quest 2");
	case EHeadset::Quest3: return TEXT("Quest 3");
	case EHeadset::QuestPro: return TEXT("Quest Pro");
	case EHeadset::PC: return TEXT("PC");
	default: return TEXT("unknown");
	}
}

OutlawsDeviceProfile::EHeadset OutlawsDeviceProfile::ClassifyHeadset(const FString& InModel, const FString& InHardware, const FString& InGpu, FString* OutSignal)
{
	// Q2PROFILE (tester dotard, 10 Oct): his Quest 2 reports Build.MODEL "Quest", not "Quest 2", so the old model test
	// missed it. First the board's codename (ro.hardware: hollywood Quest 2, eureka Quest 3, panther Quest 3S, seacliff
	// Pro, monterey Quest 1), then the model, then the GPU (Adreno 650 the Quest 2's XR2, 740 the Quest 3's XR2 Gen 2).
	// UE 5.1's device profile is no help: the Quest 3 gets Oculus_Quest2 too.
	auto Say = [OutSignal](const FString& T) { if (OutSignal) *OutSignal = T; };
	const FString Hw = InHardware.TrimStartAndEnd().ToLower();
	const FString Model = InModel.TrimStartAndEnd();
	const FString Gpu = InGpu.TrimStartAndEnd();
	struct FCode { const TCHAR* Code; EHeadset H; };
	static const FCode Codes[] = { { TEXT("hollywood"), EHeadset::Quest2 }, { TEXT("eureka"), EHeadset::Quest3 }, { TEXT("panther"), EHeadset::Quest3 },
		{ TEXT("seacliff"), EHeadset::QuestPro }, { TEXT("monterey"), EHeadset::Quest1 } };
	for (const FCode& C : Codes)
	{
		if (Hw == C.Code) { Say(FString::Printf(TEXT("ro.hardware %s"), C.Code)); return C.H; }
	}
	if (Model.Contains(TEXT("Quest 2"))) { Say(TEXT("model 'Quest 2'")); return EHeadset::Quest2; }
	if (Model.Contains(TEXT("Quest 3"))) { Say(FString::Printf(TEXT("model '%s'"), *Model)); return EHeadset::Quest3; }
	if (Model.Contains(TEXT("Quest Pro"))) { Say(TEXT("model 'Quest Pro'")); return EHeadset::QuestPro; }
	const bool bAdreno650 = Gpu.Contains(TEXT("650")), bAdreno740 = Gpu.Contains(TEXT("740")), bAdreno540 = Gpu.Contains(TEXT("540"));
	if (bAdreno650) { Say(FString::Printf(TEXT("GPU %s"), *Gpu)); return EHeadset::Quest2; }
	if (bAdreno740) { Say(FString::Printf(TEXT("GPU %s"), *Gpu)); return EHeadset::Quest3; }
	if (Model.Equals(TEXT("Quest"), ESearchCase::IgnoreCase) || Model.Equals(TEXT("Oculus Quest"), ESearchCase::IgnoreCase))
	{
		if (bAdreno540) { Say(FString::Printf(TEXT("model '%s', GPU %s"), *Model, *Gpu)); return EHeadset::Quest1; }
		// the Quest 2 reports plain "Quest" (and the first Quest is past its support): the lighter profile
		Say(FString::Printf(TEXT("model '%s' (plain: the Quest 2 reports it)"), *Model));
		return EHeadset::Quest2;
	}
	if (Model.Equals(TEXT("PC"))) { Say(TEXT("not Android")); return EHeadset::PC; }
	Say(FString::Printf(TEXT("nothing known (model '%s', ro.hardware '%s', GPU '%s')"), *Model, *Hw, *Gpu));
	return EHeadset::Unknown;
}

void OutlawsDeviceProfile::Startup()
{
#if PLATFORM_ANDROID
	GOldpModel = FAndroidMisc::GetDeviceModel().TrimStartAndEnd();
	const FString Make = FAndroidMisc::GetDeviceMake().TrimStartAndEnd();
	if (const FString* Hw = FAndroidMisc::GetConfigRulesVariable(TEXT("ro.hardware"))) GOldpHardware = *Hw;
	if (GOldpHardware.IsEmpty())
	{
		char Buf[PROP_VALUE_MAX] = { 0 };
		if (__system_property_get("ro.hardware", Buf) > 0) GOldpHardware = UTF8_TO_TCHAR(Buf);
	}
	GOldpGpu = FAndroidMisc::GetGPUFamily();
#else
	GOldpModel = TEXT("PC");
	const FString Make = FPlatformProperties::IniPlatformName();
#endif
	// (desktop tests: stand-ins for the headset's own strings)
	OLFlags::Value(TEXT("OLHeadsetModel"), GOldpModel);
	OLFlags::Value(TEXT("OLHeadsetHardware"), GOldpHardware);
	OLFlags::Value(TEXT("OLHeadsetGpu"), GOldpGpu);
	GOldpHeadset = ClassifyHeadset(GOldpModel, GOldpHardware, GOldpGpu, &GOldpSignal);
	// Quest 2 only: the Quest 3 and 3S (Quest 3 class chip), the Pro, the first Quest and anything unknown keep the defaults
	bOldpModelIsQuest2 = GOldpHeadset == EHeadset::Quest2;
	bOldpModelIsQuest3 = GOldpHeadset == EHeadset::Quest3;
	// -OLDeviceProfile=Quest2 turns the Quest 2 profile on (Quest3 or PC: off), as -OLQuest2Profile does
	FString Dp;
	const bool bDp = OLFlags::Value(TEXT("OLDeviceProfile"), Dp);
	if (OLFlags::Param(TEXT("OLQuest2Profile")) || (bDp && Dp.Equals(TEXT("Quest2"), ESearchCase::IgnoreCase))) CVarOldpQuest2Profile->Set(1, ECVF_SetByCommandline);
	else if (OLFlags::Param(TEXT("OLNoQuest2Profile")) || bDp) CVarOldpQuest2Profile->Set(0, ECVF_SetByCommandline);
	bOldpQuest2 = OldpDecide();
	UE_LOG(LogOutlaws, Log, TEXT("OLRT headset: %s, profile %s (%s; make %s)"), *GOldpModel, OldpProfileName(), *OldpSource(), *Make);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT headset detect: %s by %s (model '%s', ro.hardware '%s', GPU '%s'%s)"), HeadsetName(GOldpHeadset), *GOldpSignal,
		*GOldpModel, *GOldpHardware, *GOldpGpu, bDp ? *FString::Printf(TEXT(", -OLDeviceProfile=%s"), *Dp) : TEXT(""));
	CVarOldpQuest2Profile->SetOnChangedCallback(FConsoleVariableDelegate::CreateStatic(&OldpOnProfileCvar));
	OutlawsXRPerf::Request(PerfLevel(), TEXT("startup"));
	OldpFovLevel0 = OldpCvarInt(TEXT("vr.VRS.HMDFixedFoveationLevel"), 0);
	OldpFovDynamic0 = OldpCvarInt(TEXT("vr.VRS.HMDFixedFoveationDynamic"), 0);
	OldpFgHandle = FCoreDelegates::ApplicationHasEnteredForegroundDelegate.AddStatic(&OldpOnForeground);
	OldpReactHandle = FCoreDelegates::ApplicationHasReactivatedDelegate.AddStatic(&OldpOnForeground);
	OutlawsDynRes::Startup();
	FString TestMode;
	if (OLFlags::Param(TEXT("OLQ2ProfileTest")) || OLFlags::Value(TEXT("OLQ2ProfileTest"), TestMode)) OldpStartTest();
}

void OutlawsDeviceProfile::Shutdown()
{
	OutlawsDynRes::Shutdown();
	if (OldpEyeTick.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(OldpEyeTick);
	OldpEyeTick.Reset();
	if (OldpResumeTick.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(OldpResumeTick);
	OldpResumeTick.Reset();
	if (OldpTestTick.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(OldpTestTick);
	OldpTestTick.Reset();
	FCoreDelegates::ApplicationHasEnteredForegroundDelegate.Remove(OldpFgHandle);
	FCoreDelegates::ApplicationHasReactivatedDelegate.Remove(OldpReactHandle);
}

OutlawsDeviceProfile::EHeadset OutlawsDeviceProfile::Headset() { return GOldpHeadset; }
int32 OutlawsDeviceProfile::ResumeCount() { return GOldpResumes; }
int32 OutlawsDeviceProfile::PerfLevel() { return bOldpQuest2 ? OldpQuest2PerfLevel : 0; }

void OutlawsDeviceProfile::OnAppResume(const TCHAR* Why)
{
	// Q2PROFILE step 2 (dotard's train, 10 Oct: 36 fps for 5 minutes after a 16 s suspend, 52 to 71 before): the refresh
	// rate, the CPU and GPU levels, the eye buffer's pixel density and the foveation put back.  Android sends both the
	// reactivate and the foreground: one pass for the pair.
	const double Now = FPlatformTime::Seconds();
	if (Now - OldpLastResume < 1.0) return;
	OldpLastResume = Now;
	++GOldpResumes;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT resume %d (%s): before: %s"), GOldpResumes, Why, *OldpStateText());
	// foveation: what the device profile set at startup
	IConsoleManager& Cm = IConsoleManager::Get();
	if (IConsoleVariable* F = Cm.FindConsoleVariable(TEXT("vr.VRS.HMDFixedFoveationLevel"))) F->Set(OldpFovLevel0, ECVF_SetByCode);
	if (IConsoleVariable* F = Cm.FindConsoleVariable(TEXT("vr.VRS.HMDFixedFoveationDynamic"))) F->Set(OldpFovDynamic0, ECVF_SetByCode);
	// the rows (resolution, refresh, lamps) and the clock levels: asked again even when unchanged
	OldpHzAsked = 0.f;
	Apply(Why);
	OutlawsXRRefresh::Reapply(Why);
	OutlawsXRPerf::Reapply(Why);
	// the headset's pixel density straight from the cvar (the cvar sink only passes a change on)
	IHeadMountedDisplay* Hmd = GEngine && GEngine->XRSystem.IsValid() ? GEngine->XRSystem->GetHMDDevice() : nullptr;
	IConsoleVariable* Pd = Cm.FindConsoleVariable(TEXT("vr.PixelDensity"));
	if (Hmd && Pd) Hmd->SetPixelDensity(Pd->GetFloat());
	UE_LOG(LogOutlaws, Log, TEXT("OLRT resume %d: after: %s"), GOldpResumes, *OldpStateText());
	// and again a few seconds on, when the session has the focus back (the XR module asks again on the focus)
	if (OldpResumeTick.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(OldpResumeTick);
	OldpResumeTick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
	{
		OldpResumeTick.Reset();
		UE_LOG(LogOutlaws, Log, TEXT("OLRT resume %d: 5 s on: %s"), GOldpResumes, *OldpStateText());
		return false;
	}), 5.0f);
}

bool OutlawsDeviceProfile::IsQuest2() { return bOldpQuest2; }
const FString& OutlawsDeviceProfile::Model() { return GOldpModel; }
bool OutlawsDeviceProfile::DefaultHDTextures() { return !bOldpQuest2; }

int32 OutlawsDeviceProfile::ResolutionPercent()
{
	return OldpChosenRes > 0 ? OldpChosenRes : bOldpQuest2 ? OldpQuest2Res : 100;
}

int32 OutlawsDeviceProfile::LampLights()
{
	return OldpChosenLamps >= 0 ? OldpChosenLamps : bOldpQuest2 ? OldpQuest2Lamps : OldpFullLamps;
}

bool OutlawsDeviceProfile::PerfHud() { return bOldpPerfHud; }
bool OutlawsDeviceProfile::GrabGlow() { return bOldpGrabGlow; }

int32 OutlawsDeviceProfile::PcWindowView()
{
#if PLATFORM_ANDROID
	return 0;                            // (the headset itself has no PC window)
#else
	return OldpPcView;
#endif
}

void OutlawsDeviceProfile::SetPcWindowView(int32 View)
{
	OldpPcView = FMath::Clamp(View, 0, PcWindowViewCount - 1);
}

const TCHAR* OutlawsDeviceProfile::PcWindowViewName(int32 View)
{
	return OldpPcViewName[FMath::Clamp(View, 0, PcWindowViewCount - 1)];
}

float OutlawsDeviceProfile::ObjectDetailScale()
{
	return OldpDetailScale[OldpChosenDetail >= 0 ? OldpChosenDetail : 1] * (bOldpQuest2 ? OldpQuest2Detail : 1.f);
}

float OutlawsDeviceProfile::RefreshRate()
{
	if (OldpChosenHz > 0.f) return OldpChosenHz;
	// (REQ-REFRESH) the default: 72 on the Quest 2, 90 on the Quest 3; anything else keeps the headset's own
	return bOldpQuest2 ? 72.f : bOldpModelIsQuest3 ? 90.f : 0.f;
}

TArray<float> OutlawsDeviceProfile::RefreshRates()
{
	TArray<float> Device = OutlawsXRRefresh::SupportedRates();
	FString Fake;
	if (Device.Num() == 0 && OLFlags::Value(TEXT("OLRefreshRates"), Fake, false))
	{
		TArray<FString> Parts;
		Fake.ParseIntoArray(Parts, TEXT(","));
		for (const FString& P : Parts) if (FCString::Atof(*P) > 0.f) Device.Add(FCString::Atof(*P));
	}
	// (REQ-REFRESH) 60, 72 and 90, and 120 on the Quest 3, each only when the headset reports it
	TArray<float> Rates;
	for (const float Hz : { 60.f, 72.f, 90.f, 120.f })
	{
		if (Hz > 100.f && !bOldpModelIsQuest3) continue;
		for (const float D : Device) if (FMath::Abs(D - Hz) < 0.5f) { Rates.Add(D); break; }
	}
	return Rates;
}

bool OutlawsDeviceProfile::IsRow(const FString& Name)
{
	return Name == TEXT("r_resolution") || Name == TEXT("r_refresh") || Name == TEXT("r_lamps") || Name == TEXT("r_perfhud") || Name == TEXT("r_grabglow") || Name == TEXT("r_objdetail") || Name == TEXT("r_pcview")
		|| Name == TEXT("r_quest2mode");
}

bool OutlawsDeviceProfile::ShowQuest2ModeRow() { return !bOldpModelIsQuest2; }
bool OutlawsDeviceProfile::Quest2Mode() { return bOldpQuest2Mode; }

FString OutlawsDeviceProfile::RowValue(const FString& Name)
{
	// (BUG-SCREENPCT) under dynamic resolution the row is the ceiling it works under, and says so
	if (Name == TEXT("r_resolution")) return (OutlawsDynRes::IsActive() ? TEXT("up to ") : TEXT("")) + FString::Printf(TEXT("%d %%"), ResolutionPercent());
	if (Name == TEXT("r_refresh"))
	{
		float Hz = RefreshRate();
		if (Hz <= 0.f) Hz = OutlawsXRRefresh::CurrentRate();
		if (Hz <= 0.f) Hz = 72.f;
		return FString::Printf(TEXT("%.0f Hz"), Hz);
	}
	if (Name == TEXT("r_lamps"))
	{
		const int32 N = LampLights();
		return N <= 0 ? FString(TEXT("Off")) : N >= OldpFullLamps ? FString::Printf(TEXT("%d (all)"), N) : FString::FromInt(N);
	}
	if (Name == TEXT("r_perfhud")) return bOldpPerfHud ? TEXT("On") : TEXT("Off");
	if (Name == TEXT("r_grabglow")) return bOldpGrabGlow ? TEXT("On") : TEXT("Off");
	if (Name == TEXT("r_objdetail")) return OldpDetailName[OldpChosenDetail >= 0 ? OldpChosenDetail : 1];
	if (Name == TEXT("r_pcview")) return OldpPcViewName[OldpPcView];
	if (Name == TEXT("r_quest2mode")) return bOldpQuest2 ? TEXT("On") : TEXT("Off");
	return FString();
}

FString OutlawsDeviceProfile::PressRow(const FString& Name)
{
	FString Hint;
	if (Name == TEXT("r_resolution"))
	{
		// the next step (REQ-TRIGGERSTEP: up or down, a number row)
		OldpChosenRes = OldpStepOrdered(OldpResSteps, UE_ARRAY_COUNT(OldpResSteps), ResolutionPercent(), 0);
		Hint = OutlawsDynRes::IsActive() ? TEXT("The most it renders at. Dynamic resolution drops below it when the headset is busy. Lower is faster but softer.")
			: TEXT("Lower resolution is faster but softer.");
	}
	else if (Name == TEXT("r_refresh"))
	{
		const TArray<float> Rates = RefreshRates();
		if (Rates.Num() == 0) return TEXT("This headset does not offer other refresh rates.");
		float Now = RefreshRate();
		if (Now <= 0.f) Now = OutlawsXRRefresh::CurrentRate();
		if (Now <= 0.f) Now = 72.f;
		OldpChosenHz = OldpStepOrdered(Rates.GetData(), Rates.Num(), Now, 0.5f);     // (REQ-TRIGGERSTEP: a number row)
		Hint = TEXT("A steady lower rate feels better than an uneven higher one.");
	}
	else if (Name == TEXT("r_lamps"))
	{
		// off, 1 .. all (REQ-TRIGGERSTEP: a number row, up is more lamps; one controller goes up and round again)
		OldpChosenLamps = OutlawsMenu::StepIndex(FMath::Clamp(LampLights(), 0, OldpFullLamps), OldpFullLamps + 1, true);
		Hint = TEXT("Lamps that light the room around them. Fewer is faster.");
	}
	else if (Name == TEXT("r_perfhud"))
	{
		bOldpPerfHud = !bOldpPerfHud;
		Hint = TEXT("Frame rate and timings, above your view.");
	}
	else if (Name == TEXT("r_grabglow"))
	{
		bOldpGrabGlow = !bOldpGrabGlow;
		Hint = TEXT("The white glow round what your hand can grab. Grabbing works the same either way.");
	}
	else if (Name == TEXT("r_objdetail"))
	{
		OldpChosenDetail = OutlawsMenu::StepIndex(OldpChosenDetail >= 0 ? OldpChosenDetail : 1, 3, false);
		Hint = TEXT("How far items and props stay 3D before their picture takes over. Near is faster.");
	}
	else if (Name == TEXT("r_pcview"))
	{
		OldpPcView = OutlawsMenu::StepIndex(OldpPcView, PcWindowViewCount, false);
		Hint = TEXT("What the PC window shows. Steady is level and smooth, for recording. The V key on the PC switches it too.");
	}
	else if (Name == TEXT("r_quest2mode"))
	{
		// (QSPEED-5) the Quest 2 profile on this headset, to see how the game looks and runs on a Quest 2
		bOldpQuest2Mode = !bOldpQuest2Mode;
		OldpRedecide(false);
		Hint = CVarOldpQuest2Profile.GetValueOnGameThread() >= 0 ? TEXT("Set for this run from the command line.")
			: bOldpQuest2Mode ? TEXT("Plays as a Quest 2 would: lower resolution, 72 Hz, one lamp light, nearer detail.")
			: TEXT("Back to this headset's own settings.");
		UE_LOG(LogOutlaws, Log, TEXT("OLRT graphics: Quest 2 Mode %s (menu), profile %s"), bOldpQuest2Mode ? TEXT("on") : TEXT("off"), OldpProfileName());
	}
	Apply(TEXT("menu"));
	return Hint;
}

void OutlawsDeviceProfile::RestoreDefaults()
{
	OldpChosenRes = -1;
	OldpChosenHz = -1.f;
	OldpChosenLamps = -1;
	bOldpPerfHud = false;
	bOldpGrabGlow = true;
	OldpChosenDetail = -1;
	OldpGamma = 1.f;
	OldpPcView = 0;
	bOldpQuest2Mode = false;
	OldpRedecide(false);
	Apply(TEXT("defaults restored"));
}

float OutlawsDeviceProfile::Gamma()
{
	return OldpGamma;
}

float* OutlawsDeviceProfile::GammaSetting()
{
	return &OldpGamma;
}

void OutlawsDeviceProfile::SetGamma(float G)
{
	OldpGamma = FMath::Clamp(FMath::RoundToFloat(G / OldpGammaStep) * OldpGammaStep, OldpGammaLo, OldpGammaHi);
	if (FMath::IsNearlyEqual(OldpGamma, 1.f, 0.01f)) OldpGamma = 1.f;     // (exactly the look as made)
}

FString OutlawsDeviceProfile::PressGamma()
{
	// one step brighter or darker (REQ-TRIGGERSTEP: a number row, stopping at 0.7 and 1.6 with two controllers)
	SetGamma(OutlawsMenu::StepNumber(OldpGamma, OldpGammaLo, OldpGammaHi, OldpGammaStep));
	UE_LOG(LogOutlaws, Log, TEXT("OLRT gamma: %.1f (menu)"), OldpGamma);
	return TEXT("Higher lifts the shadows. 1.0 is the picture as made.");
}

FString OutlawsDeviceProfile::GammaText()
{
	return FMath::IsNearlyEqual(OldpGamma, 1.f, 0.01f) ? FString(TEXT("1.0 (default)")) : FString::Printf(TEXT("%.1f"), OldpGamma);
}

void OutlawsDeviceProfile::LoadSettings(const TMap<FString, FString>& S)
{
	OldpChosenRes = OldpParseChoice(S, TEXT("RenderScale"), 50, 150);
	OldpChosenLamps = OldpParseChoice(S, TEXT("LampLights"), 0, 4);
	const int32 Hz = OldpParseChoice(S, TEXT("RefreshRate"), 0, 240);
	OldpChosenHz = Hz > 0 ? float(Hz) : -1.f;
	const FString* Hud = S.Find(TEXT("PerfHud"));
	bOldpPerfHud = Hud && Hud->ToBool();
	const FString* Glow = S.Find(TEXT("GrabGlow"));
	bOldpGrabGlow = !Glow || Glow->ToBool();
	OldpChosenDetail = OldpParseChoice(S, TEXT("ObjectDetail"), 0, 2);
	const FString* Gm = S.Find(TEXT("Gamma"));
	SetGamma(Gm && FCString::Atof(**Gm) > 0.f ? FCString::Atof(**Gm) : 1.f);
	const FString* Pv = S.Find(TEXT("PcWindowView"));
	OldpPcView = Pv && Pv->Equals(TEXT("steady"), ESearchCase::IgnoreCase) ? 1 : 0;
	const FString* Q2 = S.Find(TEXT("Quest2Mode"));
	bOldpQuest2Mode = Q2 && Q2->ToBool();
	OldpRedecide(false);
	Apply(TEXT("settings loaded"));
}

void OutlawsDeviceProfile::GatherSettings(TMap<FString, FString>& Out)
{
	Out.Add(TEXT("RenderScale"), OldpChosenRes > 0 ? FString::FromInt(OldpChosenRes) : FString(TEXT("auto")));
	Out.Add(TEXT("RefreshRate"), OldpChosenHz > 0.f ? FString::FromInt(FMath::RoundToInt(OldpChosenHz)) : FString(TEXT("auto")));
	Out.Add(TEXT("LampLights"), OldpChosenLamps >= 0 ? FString::FromInt(OldpChosenLamps) : FString(TEXT("auto")));
	Out.Add(TEXT("PerfHud"), bOldpPerfHud ? TEXT("true") : TEXT("false"));
	Out.Add(TEXT("GrabGlow"), bOldpGrabGlow ? TEXT("true") : TEXT("false"));
	Out.Add(TEXT("ObjectDetail"), OldpChosenDetail >= 0 ? FString::FromInt(OldpChosenDetail) : FString(TEXT("auto")));
	Out.Add(TEXT("Gamma"), FString::Printf(TEXT("%.2f"), OldpGamma));
	Out.Add(TEXT("PcWindowView"), OldpPcView != 0 ? TEXT("steady") : TEXT("headset"));   // (written both ways: the save merges into the file's old keys)
	Out.Add(TEXT("Quest2Mode"), bOldpQuest2Mode ? TEXT("true") : TEXT("false"));
}

void OutlawsDeviceProfile::Apply(const TCHAR* Why)
{
	IConsoleManager& Cm = IConsoleManager::Get();
	// resolution: vr.PixelDensity, set only when a choice or the profile asks; otherwise what was there is put back
	FString ResText;
	if (IConsoleVariable* Pd = Cm.FindConsoleVariable(TEXT("vr.PixelDensity")))
	{
		const bool bWant = OldpChosenRes > 0 || bOldpQuest2;
		// (Q2 step 3) dynamic resolution, when on, owns the value under the row's ceiling (the headset's own 1.0 when no row asks)
		const bool bDyn = OutlawsDynRes::WantsControl();
		if (bWant || bDyn)
		{
			if (!bOldpPdTouched) { OldpPdOriginal = Pd->GetFloat(); bOldpPdTouched = true; }
			const float Ceiling = bWant ? ResolutionPercent() / 100.f : OldpPdOriginal;
			Pd->Set(OutlawsDynRes::Claim(Ceiling, Why), ECVF_SetByCode);
			ResText = bWant ? FString::Printf(TEXT("%d %% (vr.PixelDensity %.2f)"), ResolutionPercent(), Pd->GetFloat())
				: FString::Printf(TEXT("the headset's own (vr.PixelDensity %.2f)"), Pd->GetFloat());
			if (bDyn) ResText += TEXT(", ") + OutlawsDynRes::ShortText();
		}
		else
		{
			OutlawsDynRes::Claim(OldpPdOriginal, Why);      // (off: it lets go)
			if (bOldpPdTouched) { Pd->Set(OldpPdOriginal, ECVF_SetByCode); bOldpPdTouched = false; }
			ResText = FString::Printf(TEXT("untouched (vr.PixelDensity %.2f)"), Pd->GetFloat());
		}
	}
	// refresh rate: asked only for a chosen rate; back to the session's own when the choice goes
	FString HzText;
	IConsoleVariable* Fps = Cm.FindConsoleVariable(TEXT("ol.perf.TargetFps"));
	const float Want = RefreshRate();
	if (Want > 0.f)
	{
		// (the XR module keeps the rate and asks for it when the session starts, only if the headset offers it; with no
		// XR_FB_display_refresh_rate nothing is asked)
		if (Want != OldpHzAsked)
		{
			OutlawsXRRefresh::Request(Want);
			OldpHzAsked = Want;
		}
		if (Fps)
		{
			if (!bOldpFpsTouched) { OldpFpsOriginal = Fps->GetFloat(); bOldpFpsTouched = true; }
			Fps->Set(Want, ECVF_SetByCode);
		}
		HzText = FString::Printf(TEXT("%.0f Hz asked%s%s"), Want, OldpChosenHz > 0.f ? TEXT("") : TEXT(" (default)"),
			OutlawsXRRefresh::IsAvailable() ? TEXT("") : TEXT(" (no XR refresh control here)"));
	}
	else
	{
		if (OldpHzAsked > 0.f)
		{
			const float Back = OutlawsXRRefresh::InitialRate() > 0.f ? OutlawsXRRefresh::InitialRate() : 72.f;
			OutlawsXRRefresh::Request(Back);
			OldpHzAsked = 0.f;
		}
		if (bOldpFpsTouched && Fps) { Fps->Set(OldpFpsOriginal, ECVF_SetByCode); bOldpFpsTouched = false; }
		const float Now = OutlawsXRRefresh::CurrentRate();
		HzText = Now > 0.f ? FString::Printf(TEXT("headset's own (%.0f Hz)"), Now) : FString(TEXT("headset's own"));
	}
	// (Q2PROFILE) the CPU and GPU clock levels: sustained high under the Quest 2 profile, else the runtime's own
	OutlawsXRPerf::Request(PerfLevel(), Why);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT graphics (%s, %s): resolution %s, refresh %s, lamp lights %d%s, performance HUD %s"),
		OldpProfileName(), Why, *ResText, *HzText, LampLights(), OldpChosenLamps >= 0 ? TEXT(" (chosen)") : TEXT(""), bOldpPerfHud ? TEXT("on") : TEXT("off"));
	OldpScheduleEyeLog();
}

FString OutlawsDeviceProfile::EyeBufferText() { return OldpEyeText; }

namespace
{
	// -OLQ2ProfileTest (Q2PROFILE, desktop): the detection by model string, the profile's defaults (with
	// -OLDeviceProfile=Quest2, or the headset stand-ins -OLHeadsetModel / -OLHeadsetHardware / -OLHeadsetGpu), a saved
	// choice winning over them, and a simulated app resume putting the render, refresh and clock state back.
	// -OLQ2ProfileTest=default expects the default profile instead. One "OLSHOT q2profile: PASS|FAIL" line, then quit.
	struct FOldpTest
	{
		int32 Step = 0;
		double T0 = 0.0, StepAt = 0.0;
		int32 Pass = 0, Fail = 0;
		int32 ResumesBefore = 0;
		bool bWantQuest2 = true;
		bool bRowTest = false;            // (QSPEED-5) -OLQ2ProfileTest=row: the Quest 2 Mode row pressed on and off
		void Check(bool bOk, const FString& What)
		{
			(bOk ? Pass : Fail)++;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT q2profile check: %s %s"), bOk ? TEXT("ok") : TEXT("FAIL"), *What);
		}
	};
	FOldpTest GOldpTest;

	AOutlawsLevelActor* OldpTestLevel()
	{
		if (!GEngine) return nullptr;
		for (const FWorldContext& C : GEngine->GetWorldContexts())
		{
			UWorld* W = C.World();
			if (!W || !W->IsGameWorld()) continue;
			for (TActorIterator<AOutlawsLevelActor> It(W); It; ++It) return *It;
		}
		return nullptr;
	}

	void OldpTestDetect(FOldpTest& T)
	{
		using EH = OutlawsDeviceProfile::EHeadset;
		struct FCase { const TCHAR* Model; const TCHAR* Hw; const TCHAR* Gpu; EH Want; };
		const FCase Cases[] = {
			{ TEXT("Quest 2"), TEXT(""), TEXT(""), EH::Quest2 },
			{ TEXT("Quest 3"), TEXT(""), TEXT(""), EH::Quest3 },
			{ TEXT("Quest 3S"), TEXT(""), TEXT(""), EH::Quest3 },
			{ TEXT("Quest Pro"), TEXT(""), TEXT(""), EH::QuestPro },
			{ TEXT("Quest"), TEXT(""), TEXT(""), EH::Quest2 },
			{ TEXT("Quest"), TEXT("hollywood"), TEXT("Adreno (TM) 650"), EH::Quest2 },      // (dotard's log)
			{ TEXT("Quest 3"), TEXT("eureka"), TEXT("Adreno (TM) 740"), EH::Quest3 },
			{ TEXT("Quest 3S"), TEXT("panther"), TEXT(""), EH::Quest3 },
			{ TEXT("Quest"), TEXT("monterey"), TEXT("Adreno (TM) 540"), EH::Quest1 },
			{ TEXT("Quest"), TEXT(""), TEXT("Adreno (TM) 740"), EH::Quest3 },
			{ TEXT("PC"), TEXT(""), TEXT(""), EH::PC },
		};
		for (const FCase& C : Cases)
		{
			FString Signal;
			const EH Got = OutlawsDeviceProfile::ClassifyHeadset(C.Model, C.Hw, C.Gpu, &Signal);
			T.Check(Got == C.Want, FString::Printf(TEXT("detect model '%s' ro.hardware '%s' GPU '%s' -> %s by %s (want %s)"), C.Model, C.Hw, C.Gpu,
				OutlawsDeviceProfile::HeadsetName(Got), *Signal, OutlawsDeviceProfile::HeadsetName(C.Want)));
		}
	}

	bool OldpTestTickFn(float)
	{
		FOldpTest& T = GOldpTest;
		const double Now = FPlatformTime::Seconds();
		AOutlawsLevelActor* L = OldpTestLevel();
		IConsoleManager& Cm = IConsoleManager::Get();
		IConsoleVariable* Pd = Cm.FindConsoleVariable(TEXT("vr.PixelDensity"));
		IConsoleVariable* Fov = Cm.FindConsoleVariable(TEXT("vr.VRS.HMDFixedFoveationLevel"));
		switch (T.Step)
		{
		case 0:
			OldpTestDetect(T);
			T.Step = 1;
			return true;
		case 1:
		{
			// the level up and its settings read (a few seconds after the level actor appears)
			if (!L)
			{
				if (Now - T.T0 > 120.0) { T.Check(false, TEXT("no level in 120 s")); T.Step = 9; }
				return true;
			}
			if (T.StepAt <= 0.0) T.StepAt = Now;
			if (Now - T.StepAt < 6.0) return true;
			const bool bQ2 = T.bWantQuest2;
			T.Check(OutlawsDeviceProfile::IsQuest2() == bQ2, FString::Printf(TEXT("profile %s (want %s)"), OutlawsDeviceProfile::IsQuest2() ? TEXT("Quest2") : TEXT("default"), bQ2 ? TEXT("Quest2") : TEXT("default")));
			T.Check(OutlawsDeviceProfile::ResolutionPercent() == (bQ2 ? 85 : 100), FString::Printf(TEXT("resolution %d %% (want %d)"), OutlawsDeviceProfile::ResolutionPercent(), bQ2 ? 85 : 100));
			T.Check(!bQ2 || (Pd && FMath::IsNearlyEqual(Pd->GetFloat(), 0.85f, 0.001f)), FString::Printf(TEXT("vr.PixelDensity %.2f"), Pd ? Pd->GetFloat() : -1.f));
			T.Check(OutlawsDeviceProfile::LampLights() == (bQ2 ? 1 : 4), FString::Printf(TEXT("lamp lights %d (want %d)"), OutlawsDeviceProfile::LampLights(), bQ2 ? 1 : 4));
			T.Check(AOutlawsLevelActor::RemoteLanternCap() == (bQ2 ? 1 : 2), FString::Printf(TEXT("remote lantern cap %d (want %d)"), AOutlawsLevelActor::RemoteLanternCap(), bQ2 ? 1 : 2));
			T.Check(!bQ2 || OutlawsDeviceProfile::RefreshRate() == 72.f, FString::Printf(TEXT("refresh default %.0f Hz (want 72 on the Quest 2)"), OutlawsDeviceProfile::RefreshRate()));
			T.Check(OutlawsDeviceProfile::DefaultHDTextures() == !bQ2, FString::Printf(TEXT("HD textures default %d"), OutlawsDeviceProfile::DefaultHDTextures() ? 1 : 0));
			T.Check(FMath::IsNearlyEqual(OutlawsDeviceProfile::ObjectDetailScale(), bQ2 ? 0.8f : 1.f, 0.001f), FString::Printf(TEXT("object detail scale %.2f"), OutlawsDeviceProfile::ObjectDetailScale()));
			T.Check(OutlawsXRPerf::Wanted() == (bQ2 ? 3 : 0), FString::Printf(TEXT("cpu/gpu level kept %s (want %s)"), OutlawsXRPerf::LevelName(OutlawsXRPerf::Wanted()), OutlawsXRPerf::LevelName(bQ2 ? 3 : 0)));
			const FString Sum = L->ThrottleSummary();
			T.Check(Sum.StartsWith(bQ2 ? TEXT("throttle Quest2") : TEXT("throttle PC")), FString::Printf(TEXT("throttle: %s"), *Sum.Left(120)));
			T.Check(!bQ2 || L->ThrottleLevel > 0 || (L->ThrottleCaps[0] == 16 && L->ThrottleCaps[1] == 20 && L->ThrottleCaps[2] == 40 && L->ThrottleCaps[3] == 4),
				FString::Printf(TEXT("throttle caps %d/%d/%d/%d at level %d (want 16/20/40/4 at level 0)"), L->ThrottleCaps[0], L->ThrottleCaps[1], L->ThrottleCaps[2], L->ThrottleCaps[3], L->ThrottleLevel));
			// a saved choice wins over the profile's default (applied, nothing written), then the file's back
			TMap<FString, FString> Saved;
			OutlawsDeviceProfile::GatherSettings(Saved);
			TMap<FString, FString> Chosen = Saved;
			Chosen.Add(TEXT("LampLights"), TEXT("3"));
			Chosen.Add(TEXT("RenderScale"), TEXT("110"));
			OutlawsDeviceProfile::LoadSettings(Chosen);
			T.Check(OutlawsDeviceProfile::LampLights() == 3 && OutlawsDeviceProfile::ResolutionPercent() == 110,
				FString::Printf(TEXT("saved choice wins: lamp lights %d resolution %d (want 3, 110)"), OutlawsDeviceProfile::LampLights(), OutlawsDeviceProfile::ResolutionPercent()));
			OutlawsDeviceProfile::LoadSettings(Saved);
			T.Check(OutlawsDeviceProfile::LampLights() == (bQ2 ? 1 : 4), FString::Printf(TEXT("back to the default: lamp lights %d"), OutlawsDeviceProfile::LampLights()));
			if (T.bRowTest)
			{
				// (QSPEED-5) Quest 2 Mode on: the Quest 2 profile's values at once, in the settings, read back; off: the defaults
				const float PdBefore = Pd ? Pd->GetFloat() : -1.f;
				const bool bCmdLine = CVarOldpQuest2Profile.GetValueOnGameThread() >= 0;
				T.Check(OutlawsDeviceProfile::ShowQuest2ModeRow(), TEXT("the Quest 2 Mode row is shown here"));
				T.Check(OutlawsDeviceProfile::RowValue(TEXT("r_quest2mode")) == (bQ2 ? TEXT("On") : TEXT("Off")), FString::Printf(TEXT("row before: %s"), *OutlawsDeviceProfile::RowValue(TEXT("r_quest2mode"))));
				const FString Hint = OutlawsDeviceProfile::PressRow(TEXT("r_quest2mode"));
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT q2profile row: pressed, hint '%s', row %s, %s"), *Hint, *OutlawsDeviceProfile::RowValue(TEXT("r_quest2mode")), *OldpSource());
				const bool bOn = bCmdLine ? bQ2 : true;
				T.Check(OutlawsDeviceProfile::IsQuest2() == bOn, FString::Printf(TEXT("row on: profile %s (want %s)"), OldpProfileName(), bOn ? TEXT("Quest2") : TEXT("default")));
				T.Check(OutlawsDeviceProfile::RowValue(TEXT("r_quest2mode")) == (bOn ? TEXT("On") : TEXT("Off")), TEXT("row on: shows ") + OutlawsDeviceProfile::RowValue(TEXT("r_quest2mode")));
				if (bOn)
				{
					T.Check(OutlawsDeviceProfile::ResolutionPercent() == 85 && Pd && FMath::IsNearlyEqual(Pd->GetFloat(), 0.85f, 0.001f),
						FString::Printf(TEXT("row on: resolution %d %% vr.PixelDensity %.2f (want 85, 0.85)"), OutlawsDeviceProfile::ResolutionPercent(), Pd ? Pd->GetFloat() : -1.f));
					T.Check(OutlawsDeviceProfile::LampLights() == 1, FString::Printf(TEXT("row on: lamp lights %d (want 1)"), OutlawsDeviceProfile::LampLights()));
					T.Check(OutlawsDeviceProfile::RefreshRate() == 72.f, FString::Printf(TEXT("row on: refresh %.0f Hz (want 72)"), OutlawsDeviceProfile::RefreshRate()));
					T.Check(!OutlawsDeviceProfile::DefaultHDTextures(), TEXT("row on: HD textures off by default"));
					T.Check(FMath::IsNearlyEqual(OutlawsDeviceProfile::ObjectDetailScale(), 0.8f, 0.001f), FString::Printf(TEXT("row on: object detail scale %.2f"), OutlawsDeviceProfile::ObjectDetailScale()));
					T.Check(OutlawsXRPerf::Wanted() == 3, FString::Printf(TEXT("row on: cpu/gpu level %s"), OutlawsXRPerf::LevelName(OutlawsXRPerf::Wanted())));
					T.Check(AOutlawsLevelActor::RemoteLanternCap() == 1, FString::Printf(TEXT("row on: remote lantern cap %d"), AOutlawsLevelActor::RemoteLanternCap()));
				}
				TMap<FString, FString> On;
				OutlawsDeviceProfile::GatherSettings(On);
				T.Check(On.FindRef(TEXT("Quest2Mode")) == TEXT("true"), TEXT("row on: settings Quest2Mode=") + On.FindRef(TEXT("Quest2Mode")));
				// read back from the settings (as the next start would): off first, then the file's on
				TMap<FString, FString> Off = On;
				Off.Add(TEXT("Quest2Mode"), TEXT("false"));
				OutlawsDeviceProfile::LoadSettings(Off);
				T.Check(OutlawsDeviceProfile::IsQuest2() == (bCmdLine ? bQ2 : false), FString::Printf(TEXT("settings off: profile %s"), OldpProfileName()));
				OutlawsDeviceProfile::LoadSettings(On);
				T.Check(OutlawsDeviceProfile::IsQuest2() == bOn && OutlawsDeviceProfile::Quest2Mode(), FString::Printf(TEXT("settings on read back: profile %s, mode %d"), OldpProfileName(), OutlawsDeviceProfile::Quest2Mode() ? 1 : 0));
				OutlawsDeviceProfile::PressRow(TEXT("r_quest2mode"));
				T.Check(!OutlawsDeviceProfile::Quest2Mode() && OutlawsDeviceProfile::IsQuest2() == bQ2, FString::Printf(TEXT("row off: profile %s"), OldpProfileName()));
				T.Check(OutlawsDeviceProfile::ResolutionPercent() == (bQ2 ? 85 : 100) && (!Pd || FMath::IsNearlyEqual(Pd->GetFloat(), PdBefore, 0.001f)),
					FString::Printf(TEXT("row off: resolution %d %% vr.PixelDensity %.2f (want %.2f)"), OutlawsDeviceProfile::ResolutionPercent(), Pd ? Pd->GetFloat() : -1.f, PdBefore));
				T.Check(OutlawsDeviceProfile::LampLights() == (bQ2 ? 1 : 4), FString::Printf(TEXT("row off: lamp lights %d"), OutlawsDeviceProfile::LampLights()));
				OutlawsDeviceProfile::PressRow(TEXT("r_quest2mode"));
				OutlawsDeviceProfile::RestoreDefaults();
				T.Check(!OutlawsDeviceProfile::Quest2Mode() && OutlawsDeviceProfile::IsQuest2() == bQ2, FString::Printf(TEXT("Restore defaults: mode off, profile %s"), OldpProfileName()));
				OutlawsDeviceProfile::LoadSettings(Saved);
			}

			// the simulated resume: the state lost (pixel density, foveation), the throttle left high
			const float PdWant = Pd ? Pd->GetFloat() : 1.f;
			const int32 FovWant = Fov ? Fov->GetInt() : 0;
			// (the profile owns vr.PixelDensity only under the Quest 2 profile or a chosen resolution; the default leaves it be)
			if (Pd && bQ2) Pd->Set(PdWant > 0.95f ? 0.7f : 1.0f, ECVF_SetByCode);
			if (Fov) Fov->Set(FovWant == 3 ? 1 : 3, ECVF_SetByCode);
			L->ThrottleLevel = 3;
			L->ThrottleSmoothMs = 40.0;
			T.ResumesBefore = OutlawsDeviceProfile::ResumeCount();
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT q2profile resume: lost state: vr.PixelDensity %.2f foveation %d throttle level 3 (to come back: %.2f, %d, 0)"),
				Pd ? Pd->GetFloat() : -1.f, Fov ? Fov->GetInt() : -1, PdWant, FovWant);
			OutlawsDeviceProfile::OnAppResume(TEXT("simulated resume"));
			T.Check(OutlawsDeviceProfile::ResumeCount() == T.ResumesBefore + 1, TEXT("resume handled once"));
			T.Check(!Pd || FMath::IsNearlyEqual(Pd->GetFloat(), PdWant, 0.001f), FString::Printf(TEXT("resume: vr.PixelDensity %.2f (want %.2f)"), Pd ? Pd->GetFloat() : -1.f, PdWant));
			T.Check(!Fov || Fov->GetInt() == FovWant, FString::Printf(TEXT("resume: foveation level %d (want %d)"), Fov ? Fov->GetInt() : -1, FovWant));
			T.Check(!bQ2 || OutlawsXRRefresh::Wanted() == 72.f, FString::Printf(TEXT("resume: refresh kept to ask %.0f Hz"), OutlawsXRRefresh::Wanted()));
			T.Check(OutlawsXRPerf::Wanted() == (bQ2 ? 3 : 0), FString::Printf(TEXT("resume: %s"), *OutlawsXRPerf::StatusText()));
			OutlawsDeviceProfile::OnAppResume(TEXT("simulated resume, the second delegate"));
			T.Check(OutlawsDeviceProfile::ResumeCount() == T.ResumesBefore + 1, TEXT("the second delegate of the pair skipped"));
			T.Step = 2;
			T.StepAt = Now;
			return true;
		}
		case 2:
			if (Now - T.StepAt < 0.4) return true;
			T.Check(L && L->ThrottleLevel == 0, FString::Printf(TEXT("resume: throttle level %d (want 0)"), L ? L->ThrottleLevel : -1));
			T.Step = 9;
			return true;
		default:
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT q2profile: %s (%d ok, %d failed; headset %s, profile %s)"), T.Fail == 0 ? TEXT("PASS") : TEXT("FAIL"),
				T.Pass, T.Fail, OutlawsDeviceProfile::HeadsetName(OutlawsDeviceProfile::Headset()), OutlawsDeviceProfile::IsQuest2() ? TEXT("Quest2") : TEXT("default"));
			OldpTestTick.Reset();
			FGenericPlatformMisc::RequestExit(false);
			return false;
		}
	}

	void OldpStartTest()
	{
		FString Mode;
		OLFlags::Value(TEXT("OLQ2ProfileTest"), Mode);
		GOldpTest = FOldpTest();
		// (QSPEED-5) =row: the Quest 2 Mode row pressed on the desktop's default profile (=rowq2: under -OLQuest2Profile, which
		// wins); =modefile: the settings file's Quest2Mode=true turns the profile on by itself (the plain checks, want Quest2)
		GOldpTest.bRowTest = Mode.Equals(TEXT("row"), ESearchCase::IgnoreCase) || Mode.Equals(TEXT("rowq2"), ESearchCase::IgnoreCase);
		GOldpTest.bWantQuest2 = GOldpTest.bRowTest ? Mode.Equals(TEXT("rowq2"), ESearchCase::IgnoreCase) || OutlawsDeviceProfile::IsQuest2()
			: !Mode.Equals(TEXT("default"), ESearchCase::IgnoreCase);
		GOldpTest.T0 = FPlatformTime::Seconds();
		OldpTestTick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&OldpTestTickFn), 0.1f);
	}
}
