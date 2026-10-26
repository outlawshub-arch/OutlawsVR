// The headset's display refresh rate (see OutlawsXRRefresh.h): an OpenXR extension plugin that enables
// XR_FB_display_refresh_rate when the runtime has it, reads the offered rates when the session starts and asks for a
// rate when told to.
#include "OutlawsXRRefresh.h"
#include "Modules/ModuleManager.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "IOpenXRExtensionPlugin.h"
#include "OpenXRCore.h"

DEFINE_LOG_CATEGORY_STATIC(LogOutlawsXR, Log, All);

namespace
{
	struct FOlxrRefreshState
	{
		FCriticalSection Lock;
		XrInstance Instance = XR_NULL_HANDLE;
		XrSession Session = XR_NULL_HANDLE;
		PFN_xrEnumerateDisplayRefreshRatesFB Enumerate = nullptr;
		PFN_xrGetDisplayRefreshRateFB Get = nullptr;
		PFN_xrRequestDisplayRefreshRateFB Ask = nullptr;
		TArray<float> Rates;
		float Initial = 0.f;
		float Current = 0.f;
		float Wanted = 0.f;            // our last request (asked again on a new session)
		double LastAsk = 0.0;          // (Maintain: when it last asked again)
		int32 Retries = 0;
		// Q2PROFILE: XR_EXT_performance_settings (the CPU and GPU clock levels)
		PFN_xrPerfSettingsSetPerformanceLevelEXT SetPerf = nullptr;
		int32 PerfWanted = 0;          // 0 none, 1..4 power savings .. boost (OutlawsXRPerf::Request)
		int32 PerfResultCpu = 1, PerfResultGpu = 1;    // (1: not asked yet; XR results are 0 or negative)
		int32 PerfAsks = 0;
		int32 PerfNotif[2][3] = { { 0, 0, 0 }, { 0, 0, 0 } };   // the runtime's last notice: [cpu, gpu][compositing, rendering, thermal]
	};

	FOlxrRefreshState& OlxrState()
	{
		static FOlxrRefreshState S;
		return S;
	}

	const TCHAR* const OlxrPerfNames[] = { TEXT("none"), TEXT("power savings"), TEXT("sustained low"), TEXT("sustained high"), TEXT("boost") };
	const XrPerfSettingsLevelEXT OlxrPerfLevels[] = { XR_PERF_SETTINGS_LEVEL_POWER_SAVINGS_EXT, XR_PERF_SETTINGS_LEVEL_POWER_SAVINGS_EXT,
		XR_PERF_SETTINGS_LEVEL_SUSTAINED_LOW_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT, XR_PERF_SETTINGS_LEVEL_BOOST_EXT };

	const TCHAR* OlxrNotifName(int32 L) { return L >= 75 ? TEXT("impaired") : L >= 25 ? TEXT("warning") : TEXT("normal"); }

	// (the lock held) ask for the kept CPU and GPU level; false when there is nothing to ask or nowhere to ask it
	bool OlxrAskPerf(FOlxrRefreshState& S, const TCHAR* Why)
	{
		if (S.PerfWanted <= 0 || !S.SetPerf || S.Session == XR_NULL_HANDLE) return false;
		const XrPerfSettingsLevelEXT L = OlxrPerfLevels[FMath::Clamp(S.PerfWanted, 1, 4)];
		S.PerfResultCpu = int32(S.SetPerf(S.Session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, L));
		S.PerfResultGpu = int32(S.SetPerf(S.Session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, L));
		++S.PerfAsks;
		UE_LOG(LogOutlawsXR, Log, TEXT("OLRT xr perf: cpu and gpu %s asked (%s; result cpu %d gpu %d, ask %d)"),
			OlxrPerfNames[FMath::Clamp(S.PerfWanted, 0, 4)], Why, S.PerfResultCpu, S.PerfResultGpu, S.PerfAsks);
		return S.PerfResultCpu >= 0 && S.PerfResultGpu >= 0;
	}

	bool OlxrOffered(const FOlxrRefreshState& S, float Hz)
	{
		for (const float R : S.Rates) if (FMath::Abs(R - Hz) < 0.5f) return true;
		return false;
	}

	// (REQ-REFRESH) the one line for a rate set or changed: "OLRT refresh: requested <Hz> Hz, actual <Hz> Hz (<why>)"
	void OlxrLogRate(const FOlxrRefreshState& S, const TCHAR* Why)
	{
		UE_LOG(LogOutlawsXR, Log, TEXT("OLRT refresh: requested %.0f Hz, actual %.0f Hz (%s)"), S.Wanted, S.Current, Why);
	}

	FString OlxrRatesText(const TArray<float>& Rates)
	{
		TArray<FString> Parts;
		for (const float R : Rates) Parts.Add(FString::Printf(TEXT("%.0f"), R));
		return FString::Join(Parts, TEXT(" "));
	}

	class FOutlawsXRExtension : public IOpenXRExtensionPlugin
	{
	public:
		virtual FString GetDisplayName() override { return TEXT("OutlawsXR display refresh rate"); }

		virtual bool GetOptionalExtensions(TArray<const ANSICHAR*>& OutExtensions) override
		{
			OutExtensions.Add(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
			OutExtensions.Add(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);      // (Q2PROFILE: the clock levels)
			return true;
		}

		virtual void PostCreateInstance(XrInstance InInstance) override
		{
			FOlxrRefreshState& S = OlxrState();
			FScopeLock L(&S.Lock);
			S.Instance = InInstance;
			S.Enumerate = nullptr;
			S.Get = nullptr;
			S.Ask = nullptr;
			// (each fails, leaving a null pointer, when the runtime did not enable the extension)
			xrGetInstanceProcAddr(InInstance, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction*)&S.Enumerate);
			xrGetInstanceProcAddr(InInstance, "xrGetDisplayRefreshRateFB", (PFN_xrVoidFunction*)&S.Get);
			xrGetInstanceProcAddr(InInstance, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction*)&S.Ask);
			if (!S.Enumerate || !S.Get || !S.Ask)
			{
				S.Enumerate = nullptr;
				S.Get = nullptr;
				S.Ask = nullptr;
				UE_LOG(LogOutlawsXR, Log, TEXT("OLRT display refresh: XR_FB_display_refresh_rate not offered by this OpenXR runtime"));
			}
			S.SetPerf = nullptr;
			xrGetInstanceProcAddr(InInstance, "xrPerfSettingsSetPerformanceLevelEXT", (PFN_xrVoidFunction*)&S.SetPerf);
			UE_LOG(LogOutlawsXR, Log, TEXT("OLRT xr perf: XR_EXT_performance_settings %s"), S.SetPerf ? TEXT("offered (the CPU and GPU levels can be asked)") : TEXT("not offered by this OpenXR runtime"));
		}

		virtual void PostCreateSession(XrSession InSession) override
		{
			FOlxrRefreshState& S = OlxrState();
			FScopeLock L(&S.Lock);
			S.Session = InSession;
			S.Rates.Reset();
			S.Initial = S.Current = 0.f;
			if (!S.Enumerate) return;
			uint32_t Count = 0;
			if (XR_SUCCEEDED(S.Enumerate(InSession, 0, &Count, nullptr)) && Count > 0)
			{
				S.Rates.SetNumZeroed(int32(Count));
				if (!XR_SUCCEEDED(S.Enumerate(InSession, Count, &Count, S.Rates.GetData()))) Count = 0;
				S.Rates.SetNum(int32(Count));
				S.Rates.Sort();
			}
			float Now = 0.f;
			if (XR_SUCCEEDED(S.Get(InSession, &Now))) S.Initial = S.Current = Now;
			UE_LOG(LogOutlawsXR, Log, TEXT("OLRT display refresh: offered %s Hz, now %.0f Hz"), *OlxrRatesText(S.Rates), S.Current);
			if (S.Wanted > 0.f && OlxrOffered(S, S.Wanted))
			{
				const XrResult R = S.Ask(InSession, S.Wanted);
				S.Retries = 0;
				S.LastAsk = FPlatformTime::Seconds();
				OlxrLogRate(S, *FString::Printf(TEXT("session start, result %d"), int32(R)));
			}
			else if (S.Wanted > 0.f)
			{
				OlxrLogRate(S, TEXT("session start, not offered by this headset: not asked"));
			}
			OlxrAskPerf(S, TEXT("session start"));
		}

		virtual void OnDestroySession(XrSession InSession) override
		{
			FOlxrRefreshState& S = OlxrState();
			FScopeLock L(&S.Lock);
			if (S.Session == InSession) S.Session = XR_NULL_HANDLE;
		}

		virtual void OnEvent(XrSession InSession, const XrEventDataBaseHeader* InHeader) override
		{
			// (a rate asked for before the session has the focus may be dropped: asked again when it gets it)
			if (InHeader && InHeader->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
			{
				const XrEventDataSessionStateChanged* St = reinterpret_cast<const XrEventDataSessionStateChanged*>(InHeader);
				FOlxrRefreshState& S = OlxrState();
				FScopeLock L(&S.Lock);
				if (St->state == XR_SESSION_STATE_FOCUSED && S.Ask && S.Session != XR_NULL_HANDLE && S.Wanted > 0.f)
				{
					const XrResult R = S.Ask(S.Session, S.Wanted);
					UE_LOG(LogOutlawsXR, Log, TEXT("OLRT display refresh: the session has the focus: asked again for %.0f Hz (result %d)"), S.Wanted, int32(R));
				}
				// (Q2PROFILE: after a suspend the clock levels may be back at the runtime's; asked again with the focus)
				if (St->state == XR_SESSION_STATE_FOCUSED) OlxrAskPerf(S, TEXT("the session has the focus"));
				return;
			}
			if (InHeader && InHeader->type == XR_TYPE_EVENT_DATA_PERF_SETTINGS_EXT)
			{
				const XrEventDataPerfSettingsEXT* P = reinterpret_cast<const XrEventDataPerfSettingsEXT*>(InHeader);
				FOlxrRefreshState& S = OlxrState();
				FScopeLock L(&S.Lock);
				const int32 D = P->domain == XR_PERF_SETTINGS_DOMAIN_GPU_EXT ? 1 : 0;
				const int32 Sub = FMath::Clamp(int32(P->subDomain) - 1, 0, 2);
				S.PerfNotif[D][Sub] = int32(P->toLevel);
				static const TCHAR* const SubName[] = { TEXT("compositing"), TEXT("rendering"), TEXT("thermal") };
				UE_LOG(LogOutlawsXR, Log, TEXT("OLRT xr perf: runtime %s %s %s -> %s"), D ? TEXT("gpu") : TEXT("cpu"), SubName[Sub],
					OlxrNotifName(int32(P->fromLevel)), OlxrNotifName(int32(P->toLevel)));
				return;
			}
			if (!InHeader || InHeader->type != XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB) return;
			const XrEventDataDisplayRefreshRateChangedFB* E = reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB*>(InHeader);
			FOlxrRefreshState& S = OlxrState();
			FScopeLock L(&S.Lock);
			S.Current = E->toDisplayRefreshRate;
			OlxrLogRate(S, *FString::Printf(TEXT("changed from %.0f Hz"), E->fromDisplayRefreshRate));
		}
	};

	class FOutlawsXRModule : public IModuleInterface
	{
	public:
		virtual void StartupModule() override { Extension.RegisterOpenXRExtensionModularFeature(); }
		virtual void ShutdownModule() override { Extension.UnregisterOpenXRExtensionModularFeature(); }
	private:
		FOutlawsXRExtension Extension;
	};
}

bool OutlawsXRRefresh::IsAvailable()
{
	FOlxrRefreshState& S = OlxrState();
	FScopeLock L(&S.Lock);
	return S.Ask && S.Session != XR_NULL_HANDLE && S.Rates.Num() > 0;
}

TArray<float> OutlawsXRRefresh::SupportedRates()
{
	FOlxrRefreshState& S = OlxrState();
	FScopeLock L(&S.Lock);
	return S.Session != XR_NULL_HANDLE ? S.Rates : TArray<float>();
}

float OutlawsXRRefresh::CurrentRate()
{
	FOlxrRefreshState& S = OlxrState();
	FScopeLock L(&S.Lock);
	if (S.Get && S.Session != XR_NULL_HANDLE)
	{
		float Now = 0.f;
		if (XR_SUCCEEDED(S.Get(S.Session, &Now))) S.Current = Now;
	}
	return S.Current;
}

float OutlawsXRRefresh::InitialRate()
{
	FOlxrRefreshState& S = OlxrState();
	FScopeLock L(&S.Lock);
	return S.Initial;
}

bool OutlawsXRRefresh::Request(float Hz)
{
	FOlxrRefreshState& S = OlxrState();
	FScopeLock L(&S.Lock);
	if (Hz <= 0.f) return false;
	// (kept even before the session is up, so the session start asks for it; nothing is asked without the extension)
	S.Wanted = Hz;
	if (!S.Ask || S.Session == XR_NULL_HANDLE) return false;
	if (!OlxrOffered(S, Hz))
	{
		OlxrLogRate(S, TEXT("not offered by this headset: not asked"));
		S.Wanted = 0.f;
		return false;
	}
	S.Retries = 0;
	S.LastAsk = FPlatformTime::Seconds();
	const XrResult R = S.Ask(S.Session, Hz);
	float Now = 0.f;
	if (S.Get && XR_SUCCEEDED(S.Get(S.Session, &Now))) S.Current = Now;
	OlxrLogRate(S, *FString::Printf(TEXT("set, result %d"), int32(R)));
	return XR_SUCCEEDED(R);
}

void OutlawsXRRefresh::Maintain(double Now)
{
	FOlxrRefreshState& S = OlxrState();
	FScopeLock L(&S.Lock);
	if (!S.Ask || !S.Get || S.Session == XR_NULL_HANDLE || S.Wanted <= 0.f || S.Retries >= 10 || Now - S.LastAsk < 3.0) return;
	float Cur = 0.f;
	if (!XR_SUCCEEDED(S.Get(S.Session, &Cur))) return;
	S.Current = Cur;
	// (there: the count starts again, so a rate pulled back later is asked for again)
	if (FMath::Abs(Cur - S.Wanted) < 0.5f) { S.Retries = 0; S.LastAsk = Now; return; }
	if (!OlxrOffered(S, S.Wanted)) return;
	S.LastAsk = Now;
	++S.Retries;
	const XrResult R = S.Ask(S.Session, S.Wanted);
	UE_LOG(LogOutlawsXR, Log, TEXT("OLRT display refresh: still %.0f Hz, asked again for %.0f Hz (try %d, result %d)"), Cur, S.Wanted, S.Retries, int32(R));
}

bool OutlawsXRRefresh::Reapply(const TCHAR* Why)
{
	FOlxrRefreshState& S = OlxrState();
	FScopeLock L(&S.Lock);
	if (S.Wanted <= 0.f || !S.Ask || S.Session == XR_NULL_HANDLE || !OlxrOffered(S, S.Wanted)) return false;
	S.Retries = 0;
	S.LastAsk = FPlatformTime::Seconds();
	const XrResult R = S.Ask(S.Session, S.Wanted);
	float Now = 0.f;
	if (S.Get && XR_SUCCEEDED(S.Get(S.Session, &Now))) S.Current = Now;
	OlxrLogRate(S, *FString::Printf(TEXT("%s, asked again, result %d"), Why, int32(R)));
	return XR_SUCCEEDED(R);
}

float OutlawsXRRefresh::Wanted()
{
	FOlxrRefreshState& S = OlxrState();
	FScopeLock L(&S.Lock);
	return S.Wanted;
}

bool OutlawsXRPerf::IsAvailable()
{
	FOlxrRefreshState& S = OlxrState();
	FScopeLock L(&S.Lock);
	return S.SetPerf && S.Session != XR_NULL_HANDLE;
}

bool OutlawsXRPerf::Request(int32 Level, const TCHAR* Why)
{
	FOlxrRefreshState& S = OlxrState();
	FScopeLock L(&S.Lock);
	Level = FMath::Clamp(Level, 0, 4);
	if (Level == S.PerfWanted && S.PerfAsks > 0) return S.PerfResultCpu >= 0 && S.PerfResultGpu >= 0;
	S.PerfWanted = Level;
	// (kept before the session is up: the session start asks for it; with 0 nothing more is asked, the runtime keeps the last)
	return OlxrAskPerf(S, Why);
}

bool OutlawsXRPerf::Reapply(const TCHAR* Why)
{
	FOlxrRefreshState& S = OlxrState();
	FScopeLock L(&S.Lock);
	return OlxrAskPerf(S, Why);
}

int32 OutlawsXRPerf::Wanted()
{
	FOlxrRefreshState& S = OlxrState();
	FScopeLock L(&S.Lock);
	return S.PerfWanted;
}

const TCHAR* OutlawsXRPerf::LevelName(int32 Level)
{
	return OlxrPerfNames[FMath::Clamp(Level, 0, 4)];
}

FString OutlawsXRPerf::StatusText()
{
	FOlxrRefreshState& S = OlxrState();
	FScopeLock L(&S.Lock);
	if (!S.SetPerf) return FString::Printf(TEXT("cpu/gpu level %s kept, no XR_EXT_performance_settings here"), OlxrPerfNames[FMath::Clamp(S.PerfWanted, 0, 4)]);
	if (S.Session == XR_NULL_HANDLE) return FString::Printf(TEXT("cpu/gpu level %s kept, no session"), OlxrPerfNames[FMath::Clamp(S.PerfWanted, 0, 4)]);
	return FString::Printf(TEXT("cpu/gpu level %s (last result cpu %d gpu %d, asked %d times), runtime thermal cpu %s gpu %s, rendering cpu %s gpu %s"),
		OlxrPerfNames[FMath::Clamp(S.PerfWanted, 0, 4)], S.PerfResultCpu, S.PerfResultGpu, S.PerfAsks,
		OlxrNotifName(S.PerfNotif[0][2]), OlxrNotifName(S.PerfNotif[1][2]), OlxrNotifName(S.PerfNotif[0][1]), OlxrNotifName(S.PerfNotif[1][1]));
}

IMPLEMENT_MODULE(FOutlawsXRModule, OutlawsXR)
