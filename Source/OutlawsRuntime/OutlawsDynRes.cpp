// Q2 step 3: the render resolution follows the GPU: see OutlawsDynRes.h.
#include "OutlawsDynRes.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsDeviceProfile.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPerf.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "IXRTrackingSystem.h"
#include "Containers/Ticker.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "RenderCore.h"
#include "RenderGraphBuilder.h"
#include "RHI.h"
#include "DynamicRHI.h"
#include "SceneViewExtension.h"
#include <atomic>

namespace
{
	TAutoConsoleVariable<int32> CVarOldrEnable(TEXT("ol.dynres.Enable"), 0,
		TEXT("Dynamic resolution (vr.PixelDensity follows the GPU): 0 off (the default everywhere: the Resolution row holds), 1 on, -1 by the platform (on with a headset on Android)"));
	TAutoConsoleVariable<float> CVarOldrMin(TEXT("ol.dynres.Min"), 0.65f,
		TEXT("Dynamic resolution's floor (vr.PixelDensity); the ceiling is the Resolution row's (0.85 on the Quest 2)"));
	TAutoConsoleVariable<float> CVarOldrFakeGpu(TEXT("ol.dynres.FakeGpuMs"), 0.f,
		TEXT("Tests: the GPU's frame ms at the ceiling, scaled with the pixels at the scale in use (0: the real GPU time)"));
	TAutoConsoleVariable<float> CVarOldrFakeCpu(TEXT("ol.dynres.FakeCpuMs"), 0.f,
		TEXT("Tests: the busier CPU thread's ms (0: the real game thread less the XR wait, or the render thread)"));
	TAutoConsoleVariable<float> CVarOldrDownAt(TEXT("ol.dynres.DownAt"), 0.90f,
		TEXT("Dynamic resolution steps down when the smoothed GPU time stays over this share of the frame for 0.5 s (and the GPU is the long pole)"));
	TAutoConsoleVariable<float> CVarOldrUpAt(TEXT("ol.dynres.UpAt"), 0.70f,
		TEXT("Dynamic resolution steps up when the smoothed GPU time stays under this share of the frame for 3 s"));
	TAutoConsoleVariable<float> CVarOldrGpuOverCpu(TEXT("ol.dynres.GpuOverCpu"), 1.0f,
		TEXT("Dynamic resolution steps down only while the GPU's time is at least the busier CPU thread's times this"));
	TAutoConsoleVariable<float> CVarOldrStep(TEXT("ol.dynres.Step"), 0.05f,
		TEXT("Dynamic resolution's step (vr.PixelDensity)"));
	TAutoConsoleVariable<float> CVarOldrLogEvery(TEXT("ol.dynres.LogEvery"),
#if UE_BUILD_SHIPPING
		10.f,
#else
		1.f,
#endif
		TEXT("Dynamic resolution's status line every this many seconds while on (0: none)"));

	constexpr double OldrDownHold = 0.5, OldrDownGap = 1.0, OldrUpHold = 3.0, OldrUpGap = 3.0;
	constexpr double OldrAim = 0.80, OldrUpRoom = 0.85, OldrResumeHold = 2.0, OldrLongFrame = 0.25;
	// (the frame cycles include the CPU's waits on Vulkan: on them only a frame really missed counts as over)
	constexpr double OldrFallbackDownAt = 1.05, OldrSampleCap = 4.0, OldrStaleSec = 1.0;

	// ---- the decision rules (no engine state: the unit test drives them with synthetic timings) ----

	struct FOldrTuning
	{
		double DownAt = 0.90, UpAt = 0.70, GpuOverCpu = 1.0;
		float Step = 0.05f;
	};

	struct FOldrSample
	{
		double Dt = 0.0;        // s since the last tick
		double GpuMs = 0.0;     // the GPU's time for the frame (<= 0: none)
		double CpuMs = 0.0;     // the busier CPU thread's work (game less the XR wait, or render)
		double BudgetMs = 13.9;
		bool bRealGpu = true;   // false: the frame cycles (they include the CPU's waits)
	};

	enum class EOldrSays : uint8 { Steady, Wait, GpuOver, CpuPole, Under, UpNoRoom, Down, Up };

	const TCHAR* OldrSaysText(EOldrSays S)
	{
		switch (S)
		{
		case EOldrSays::Wait: return TEXT("wait (hitch, load or resume)");
		case EOldrSays::GpuOver: return TEXT("GPU over the line, the long pole (down pending)");
		case EOldrSays::CpuPole: return TEXT("hold: GPU over the line but a CPU thread is the long pole");
		case EOldrSays::Under: return TEXT("GPU under the up line (up pending)");
		case EOldrSays::UpNoRoom: return TEXT("hold: the next step up would not fit");
		case EOldrSays::Down: return TEXT("stepped down");
		case EOldrSays::Up: return TEXT("stepped up");
		default: return TEXT("steady");
		}
	}

	struct FOldrCore
	{
		float Scale = 1.f, Ceiling = 1.f, FloorAsked = 0.65f;
		double Smooth = 0.0, CpuSmooth = 0.0;
		double OverFor = 0.0, UnderFor = 0.0, LastChange = -100.0, HoldUntil = 0.0;
		// (each step re-makes the eye buffer's swapchain: a down soon after an up doubles the wait before the next up, to 30 s;
		// 30 s without a down brings it back to 3)
		double UpHoldNow = OldrUpHold, LastUp = -100.0;
		int32 Downs = 0, Ups = 0;
		EOldrSays Last = EOldrSays::Steady;

		float Lo() const { return FMath::Min(FMath::Max(0.3f, FloorAsked), Ceiling); }

		/** A new ceiling (or on): start from it, readings forgotten. */
		void Restart(double Now)
		{
			Scale = Ceiling;
			UpHoldNow = OldrUpHold;
			Smooth = CpuSmooth = 0.0;
			OverFor = UnderFor = 0.0;
			LastChange = Now;
		}

		/** A resume: the scale kept, readings forgotten, nothing read for the hold. */
		void Hold(double Now, double For)
		{
			HoldUntil = Now + For;
			Smooth = CpuSmooth = 0.0;
			OverFor = UnderFor = 0.0;
		}

		void StepTo(float To, bool bDown, double Now)
		{
			Scale = To;
			(bDown ? Downs : Ups)++;
			if (bDown && Now - LastUp < 10.0) UpHoldNow = FMath::Min(30.0, UpHoldNow * 2.0);
			if (!bDown) LastUp = Now;
			LastChange = Now;
			OverFor = UnderFor = 0.0;
		}

		EOldrSays Tick(double Now, const FOldrSample& S, const FOldrTuning& T)
		{
			if (S.GpuMs <= 0.0) return Last = EOldrSays::Wait;
			if (S.Dt > OldrLongFrame || Now < HoldUntil)
			{
				OverFor = UnderFor = 0.0;
				return Last = EOldrSays::Wait;
			}
			const double Budget = S.BudgetMs;
			const double Cap = Budget * OldrSampleCap;
			const double G = FMath::Min(S.GpuMs, Cap), C = FMath::Clamp(S.CpuMs, 0.0, Cap);
			const double A = FMath::Clamp(S.Dt * 4.0, 0.0, 1.0);
			Smooth = Smooth <= 0.0 ? G : FMath::Lerp(Smooth, G, A);
			CpuSmooth = CpuSmooth <= 0.0 ? C : FMath::Lerp(CpuSmooth, C, A);
			const double DownLine = Budget * (S.bRealGpu ? T.DownAt : OldrFallbackDownAt);
			const bool bOver = Smooth > DownLine;
			const bool bGpuPole = Smooth >= CpuSmooth * T.GpuOverCpu;
			EOldrSays Says = EOldrSays::Steady;
			if (bOver && bGpuPole) { OverFor += S.Dt; UnderFor = 0.0; Says = EOldrSays::GpuOver; }
			else if (bOver) { OverFor = UnderFor = 0.0; Says = EOldrSays::CpuPole; }
			else if (Smooth < Budget * T.UpAt) { UnderFor += S.Dt; OverFor = 0.0; Says = EOldrSays::Under; }
			else { OverFor = UnderFor = 0.0; }
			const float Step = FMath::Clamp(T.Step, 0.01f, 0.25f);
			const float L = Lo();
			const double Since = Now - LastChange;
			if (UpHoldNow > OldrUpHold && Since > 30.0) UpHoldNow = OldrUpHold;
			if (OverFor >= OldrDownHold && Since >= OldrDownGap && Scale > L + 0.001f)
			{
				// the scale that brings the GPU to 80 % of the frame (its ms go with the pixels), one to three steps, on the
				// ceiling's grid (0.85, 0.80, ... on the Quest 2)
				float Want = Scale * float(FMath::Sqrt(Budget * OldrAim / Smooth));
				Want = FMath::Clamp(Want, Scale - 3.f * Step, Scale - Step);
				const float Steps = FMath::CeilToFloat((Ceiling - Want) / Step - 0.001f);
				StepTo(FMath::Max(L, Ceiling - Steps * Step), true, Now);
				return Last = EOldrSays::Down;
			}
			if (UnderFor >= UpHoldNow && Since >= OldrUpGap && Scale < Ceiling - 0.001f)
			{
				const float Next = FMath::Min(Ceiling, Scale + Step);
				const double Cost = Smooth * FMath::Square(double(Next) / double(Scale));
				if (Cost < Budget * OldrUpRoom)
				{
					StepTo(Next, false, Now);
					return Last = EOldrSays::Up;
				}
				UnderFor = 0.0;
				Says = EOldrSays::UpNoRoom;
			}
			return Last = Says;
		}
	};

	FOldrTuning OldrTuning()
	{
		FOldrTuning T;
		T.DownAt = CVarOldrDownAt.GetValueOnGameThread();
		T.UpAt = CVarOldrUpAt.GetValueOnGameThread();
		T.GpuOverCpu = CVarOldrGpuOverCpu.GetValueOnGameThread();
		T.Step = CVarOldrStep.GetValueOnGameThread();
		return T;
	}

	// ---- the GPU's own time for the game view: a timestamp pair around each viewport family's render ----

	std::atomic<bool> bOldrTimerOn{false};
	std::atomic<uint32> OldrGpuUs{0};       // the last landed frame's scene work, microseconds
	std::atomic<uint32> OldrGpuSeq{0};      // bumped per landed frame

	class FOldrGpuTimer : public FSceneViewExtensionBase
	{
	public:
		FOldrGpuTimer(const FAutoRegister& R) : FSceneViewExtensionBase(R) {}

		virtual void SetupViewFamily(FSceneViewFamily&) override {}
		virtual void SetupView(FSceneViewFamily&, FSceneView&) override {}
		virtual void BeginRenderViewFamily(FSceneViewFamily&) override {}

		virtual void PreRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily&) override
		{
			if (!Pool.IsValid()) Pool = RHICreateRenderQueryPool(RQT_AbsoluteTime);
			Harvest();
			FPair& P = Open.Emplace_GetRef();
			P.Frame = GFrameCounterRenderThread;
			P.Begin = Pool->AllocateQuery();
			FRHIRenderQuery* Q = P.Begin.GetQuery();
			GraphBuilder.AddPass(RDG_EVENT_NAME("OLDynResGpuBegin"), ERDGPassFlags::NeverCull, [Q](FRHICommandListImmediate& RHICmdList) { RHICmdList.EndRenderQuery(Q); });
		}

		virtual void PostRenderViewFamily_RenderThread(FRDGBuilder& GraphBuilder, FSceneViewFamily&) override
		{
			if (Open.Num() == 0 || !Pool.IsValid()) return;
			FPair P = MoveTemp(Open.Last());
			Open.Pop();
			P.End = Pool->AllocateQuery();
			FRHIRenderQuery* Q = P.End.GetQuery();
			GraphBuilder.AddPass(RDG_EVENT_NAME("OLDynResGpuEnd"), ERDGPassFlags::NeverCull, [Q](FRHICommandListImmediate& RHICmdList) { RHICmdList.EndRenderQuery(Q); });
			InFlight.Add(MoveTemp(P));
			// (results that never land: keep a few frames' worth, drop the oldest)
			while (InFlight.Num() > 8) InFlight.RemoveAt(0);
		}

	protected:
		virtual bool IsActiveThisFrame_Internal(const FSceneViewExtensionContext& Context) const override
		{
			return bOldrTimerOn.load(std::memory_order_relaxed) && Context.Viewport != nullptr;
		}

	private:
		struct FPair
		{
			FRHIPooledRenderQuery Begin, End;
			uint64 Frame = 0;
		};
		FRenderQueryPoolRHIRef Pool;
		TArray<FPair> Open, InFlight;       // (render thread only)
		uint64 PubFrame = 0;
		uint64 PubUs = 0;

		void Harvest()
		{
			while (InFlight.Num() > 0)
			{
				FPair& P = InFlight[0];
				uint64 B = 0, E = 0;
				if (!RHIGetRenderQueryResult(P.Begin.GetQuery(), B, false) || !RHIGetRenderQueryResult(P.End.GetQuery(), E, false)) break;
				const uint64 Us = E > B ? E - B : 0;
				// (two viewport families in one frame add up)
				PubUs = P.Frame == PubFrame ? PubUs + Us : Us;
				PubFrame = P.Frame;
				OldrGpuUs.store(uint32(FMath::Min<uint64>(PubUs, 10000000)), std::memory_order_relaxed);
				OldrGpuSeq.fetch_add(1, std::memory_order_release);
				InFlight.RemoveAt(0);
			}
		}
	};

	TSharedPtr<FOldrGpuTimer, ESPMode::ThreadSafe> GOldrTimer;

	// ---- the controller on the game thread ----

	enum class EOldrSource : uint8 { None, Timestamps, FrameCycles, Fake };
	const TCHAR* OldrSourceText(EOldrSource S)
	{
		switch (S)
		{
		case EOldrSource::Timestamps: return TEXT("GPU timestamps");
		case EOldrSource::FrameCycles: return TEXT("RHI frame cycles");
		case EOldrSource::Fake: return TEXT("fake");
		default: return TEXT("none");
		}
	}

	bool bOldrActive = false;
	FOldrCore GCore;
	float OldrLastCeiling = -1.f;
	double OldrRaw = 0.0, OldrCpuRaw = 0.0, OldrGameRaw = 0.0, OldrRenderRaw = 0.0, OldrCyclesRaw = 0.0;
	double OldrNextLog = 0.0, OldrLastLog = 0.0, OldrLogFrameMax = 0.0;
	int32 OldrLogFrames = 0;
	int32 OldrResumeSeen = 0;
	EOldrSource OldrSource = EOldrSource::None, OldrSourceLogged = EOldrSource::None;
	uint32 OldrSeqSeen = 0;
	double OldrLastLanded = 0.0, OldrTimerSince = 0.0, OldrRealMs = 0.0;
	bool bOldrNoGpuLogged = false;
	FTSTicker::FDelegateHandle OldrTick, OldrTestTick;

	float OldrFloor() { GCore.FloorAsked = CVarOldrMin.GetValueOnGameThread(); return GCore.Lo(); }

	double OldrBudgetMs()
	{
		static IConsoleVariable* Fps = IConsoleManager::Get().FindConsoleVariable(TEXT("ol.perf.TargetFps"));
		return 1000.0 / FMath::Max(1.f, Fps ? Fps->GetFloat() : 72.f);
	}

	void OldrSetPd(float V)
	{
		if (IConsoleVariable* Pd = IConsoleManager::Get().FindConsoleVariable(TEXT("vr.PixelDensity"))) Pd->Set(V, ECVF_SetByCode);
	}

	/** The timestamp pair's newest reading while it is landing (ms), else 0. */
	double OldrRealGpu(double Now)
	{
		const uint32 Seq = OldrGpuSeq.load(std::memory_order_acquire);
		if (Seq != OldrSeqSeen)
		{
			OldrSeqSeen = Seq;
			OldrLastLanded = Now;
			OldrRealMs = double(OldrGpuUs.load(std::memory_order_relaxed)) / 1000.0;
		}
		return Now - OldrLastLanded <= OldrStaleSec ? OldrRealMs : 0.0;
	}

	bool OldrTickFn(float Dt)
	{
		const double Now = FPlatformTime::Seconds();
		if (!bOldrActive)
		{
			bOldrTimerOn = false;
			// (on the headset the XR system comes up after the settings are first applied: taken over once it is)
			static double NextLook = 0.0;
			if (Now >= NextLook)
			{
				NextLook = Now + 1.0;
				if (OutlawsDynRes::WantsControl()) OutlawsDeviceProfile::Apply(TEXT("dynamic resolution on (the headset is up)"));
			}
			return true;
		}
		// the timestamp pair runs only while on (nothing is issued on a PC with it off)
		if (!bOldrTimerOn && GSupportsTimestampRenderQueries)
		{
			if (!GOldrTimer.IsValid()) GOldrTimer = FSceneViewExtensions::NewExtension<FOldrGpuTimer>();
			bOldrTimerOn = true;
			OldrTimerSince = Now;
		}
		// (an app resume: the long suspend frame and the clocks coming back say nothing of the load; held 2 s)
		if (OldrResumeSeen != OutlawsDeviceProfile::ResumeCount())
		{
			OldrResumeSeen = OutlawsDeviceProfile::ResumeCount();
			GCore.Hold(Now, OldrResumeHold);
			UE_LOG(LogOutlaws, Log, TEXT("OLRT dynres: app resumed, holding %.2f for %.0f s (floor %.2f, ceiling %.2f)"), GCore.Scale, OldrResumeHold, OldrFloor(), GCore.Ceiling);
		}
		const double Budget = OldrBudgetMs();
		const float FakeG = CVarOldrFakeGpu.GetValueOnGameThread();
		const float FakeC = CVarOldrFakeCpu.GetValueOnGameThread();
		OldrCyclesRaw = FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0));
		const double Real = OldrRealGpu(Now);
		FOldrSample S;
		S.Dt = Dt;
		S.BudgetMs = Budget;
		if (FakeG > 0.f)
		{
			OldrSource = EOldrSource::Fake;
			S.GpuMs = double(FakeG) * FMath::Square(double(GCore.Scale) / FMath::Max(0.01, double(GCore.Ceiling)));
		}
		else if (Real > 0.0)
		{
			OldrSource = EOldrSource::Timestamps;
			S.GpuMs = Real;
		}
		else if (bOldrTimerOn && Now - OldrTimerSince < 2.0)
		{
			// (the first results are a frame or two away: not read on the frame cycles meanwhile)
			OldrSource = EOldrSource::None;
			S.GpuMs = 0.0;
		}
		else
		{
			OldrSource = OldrCyclesRaw > 0.0 ? EOldrSource::FrameCycles : EOldrSource::None;
			S.GpuMs = OldrCyclesRaw;
			S.bRealGpu = false;
		}
		OldrGameRaw = FMath::Max(0.0, FPlatformTime::ToMilliseconds(GGameThreadTime) - OutlawsPerf::XRWaitMs());
		OldrRenderRaw = FPlatformTime::ToMilliseconds(GRenderThreadTime);
		S.CpuMs = FakeC > 0.f ? double(FakeC) : FMath::Max(OldrGameRaw, OldrRenderRaw);
		OldrRaw = S.GpuMs;
		OldrCpuRaw = S.CpuMs;
		if (OldrSource != OldrSourceLogged && OldrSource != EOldrSource::None)
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLRT dynres: reading %s%s (was %s)"), OldrSourceText(OldrSource),
				OldrSource == EOldrSource::FrameCycles ? (GSupportsTimestampRenderQueries ? TEXT(": the timestamps stopped landing; down only on frames really missed")
					: TEXT(": no timestamp queries here (r.Vulkan.SupportsTimestampQueries?); down only on frames really missed")) : TEXT(""),
				OldrSourceText(OldrSourceLogged));
			OldrSourceLogged = OldrSource;
		}
		if (S.GpuMs <= 0.0 && OldrSource == EOldrSource::None && !(bOldrTimerOn && Now - OldrTimerSince < 2.0))
		{
			if (!bOldrNoGpuLogged)
			{
				bOldrNoGpuLogged = true;
				UE_LOG(LogOutlaws, Log, TEXT("OLRT dynres: no GPU timing here: holding %.2f"), GCore.Scale);
			}
		}
		OldrFloor();
		const float From = GCore.Scale;
		const EOldrSays Says = GCore.Tick(Now, S, OldrTuning());
		if (Says == EOldrSays::Down || Says == EOldrSays::Up)
		{
			OldrSetPd(GCore.Scale);
			UE_LOG(LogOutlaws, Log, TEXT("OLRT dynres: %s %.2f -> %.2f (GPU %.1f ms smoothed, %.1f this frame, %s; CPU pole %.1f ms smoothed (game %.1f less the XR wait, render %.1f); budget %.1f ms; floor %.2f, ceiling %.2f; steps down %d up %d; next up after %.0f s under)"),
				Says == EOldrSays::Down ? TEXT("down") : TEXT("up"), From, GCore.Scale, GCore.Smooth, OldrRaw, OldrSourceText(OldrSource), GCore.CpuSmooth, OldrGameRaw, OldrRenderRaw,
				Budget, GCore.Lo(), GCore.Ceiling, GCore.Downs, GCore.Ups, GCore.UpHoldNow);
		}
#if !UE_BUILD_SHIPPING
		UE_LOG(LogOutlaws, Verbose, TEXT("OLRT dynres frame: scale %.2f, GPU %.2f ms (%s; frame cycles %.2f), CPU %.2f (game %.2f, render %.2f), smoothed %.2f / %.2f, %s"),
			GCore.Scale, OldrRaw, OldrSourceText(OldrSource), OldrCyclesRaw, OldrCpuRaw, OldrGameRaw, OldrRenderRaw, GCore.Smooth, GCore.CpuSmooth, OldrSaysText(Says));
#endif
		const float Every = CVarOldrLogEvery.GetValueOnGameThread();
		++OldrLogFrames;
		OldrLogFrameMax = FMath::Max(OldrLogFrameMax, double(Dt) * 1000.0);
		if (Every > 0.f && Now >= OldrNextLog)
		{
			// (the frames since the last line: their average and the longest, wall clock)
			const double Span = OldrLastLog > 0.0 ? Now - OldrLastLog : 0.0;
			const double FrameMs = Span > 0.0 && OldrLogFrames > 0 ? Span * 1000.0 / OldrLogFrames : 0.0;
			OldrNextLog = Now + Every;
			UE_LOG(LogOutlaws, Log, TEXT("OLRT dynres status: scale %.2f (%.2f..%.2f), GPU %.1f ms smoothed (%.1f this frame, %s; frame cycles %.1f), CPU pole %.1f ms smoothed (game %.1f less the XR wait, render %.1f), frame %.2f ms avg (%.1f fps, longest %.1f) over %d, budget %.1f ms, %s, steps down %d up %d"),
				GCore.Scale, GCore.Lo(), GCore.Ceiling, GCore.Smooth, OldrRaw, OldrSourceText(OldrSource), OldrCyclesRaw, GCore.CpuSmooth, OldrGameRaw, OldrRenderRaw,
				FrameMs, FrameMs > 0.0 ? 1000.0 / FrameMs : 0.0, OldrLogFrameMax, OldrLogFrames, Budget, OldrSaysText(Says), GCore.Downs, GCore.Ups);
			OldrLastLog = Now;
			OldrLogFrames = 0;
			OldrLogFrameMax = 0.0;
		}
		return true;
	}

	void OldrOnEnableCvar(IConsoleVariable*)
	{
		OutlawsDeviceProfile::Apply(OutlawsDynRes::WantsControl() ? TEXT("dynamic resolution on") : TEXT("dynamic resolution off"));
	}
#if WITH_OUTLAWS_TESTS
	void OldrStartTest();
	void OldrStartUnitTest();
	void OldrStartOffTest();
#endif
}

bool OutlawsDynRes::WantsControl()
{
	const int32 E = CVarOldrEnable.GetValueOnGameThread();
	if (E >= 0) return E > 0;
#if PLATFORM_ANDROID
	return GEngine && GEngine->XRSystem.IsValid();
#else
	return false;
#endif
}

void OutlawsDynRes::Startup()
{
	OldrTick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&OldrTickFn), 0.f);
	CVarOldrEnable->SetOnChangedCallback(FConsoleVariableDelegate::CreateStatic(&OldrOnEnableCvar));
	OldrResumeSeen = OutlawsDeviceProfile::ResumeCount();
#if WITH_OUTLAWS_TESTS
	if (OLFlags::Param(TEXT("OLDynResTest"))) OldrStartTest();
	if (OLFlags::Param(TEXT("OLDynResUnitTest"))) OldrStartUnitTest();
	if (OLFlags::Param(TEXT("OLDynResOffTest"))) OldrStartOffTest();
#endif
}

void OutlawsDynRes::Shutdown()
{
	bOldrTimerOn = false;
	GOldrTimer.Reset();
	if (OldrTick.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(OldrTick);
	OldrTick.Reset();
	if (OldrTestTick.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(OldrTestTick);
	OldrTestTick.Reset();
}

float OutlawsDynRes::Claim(float InCeiling, const TCHAR* Why)
{
	const bool bWas = bOldrActive;
	bOldrActive = WantsControl();
	GCore.Ceiling = FMath::Max(0.3f, InCeiling);
	if (!bOldrActive)
	{
		if (bWas) UE_LOG(LogOutlaws, Log, TEXT("OLRT dynres: off (%s): vr.PixelDensity back to %.2f"), Why, GCore.Ceiling);
		GCore.Scale = GCore.Ceiling;
		OldrLastCeiling = -1.f;
		bOldrTimerOn = false;
		return GCore.Ceiling;
	}
	const bool bNewCeiling = !bWas || !FMath::IsNearlyEqual(OldrLastCeiling, GCore.Ceiling, 0.001f);
	if (bNewCeiling) GCore.Restart(FPlatformTime::Seconds());
	else GCore.Scale = FMath::Clamp(GCore.Scale, OldrFloor(), GCore.Ceiling);
	OldrLastCeiling = GCore.Ceiling;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT dynres: %s (%s): vr.PixelDensity %.2f, floor %.2f, ceiling %.2f%s"), bWas ? TEXT("keeps the resolution") : TEXT("on"), Why,
		GCore.Scale, OldrFloor(), GCore.Ceiling, bNewCeiling ? TEXT(" (from the ceiling)") : TEXT(" (the controller's own)"));
	return GCore.Scale;
}

bool OutlawsDynRes::IsActive() { return bOldrActive; }
float OutlawsDynRes::Scale() { return GCore.Scale; }
float OutlawsDynRes::Floor() { return OldrFloor(); }
float OutlawsDynRes::Ceiling() { return GCore.Ceiling; }

FString OutlawsDynRes::ShortText()
{
	return bOldrActive ? FString::Printf(TEXT("dynamic %.2f (%.2f..%.2f)"), GCore.Scale, OldrFloor(), GCore.Ceiling) : FString(TEXT("dynamic off"));
}

double OutlawsDynRes::GpuMs()
{
	if (bOldrActive && OldrSource == EOldrSource::Timestamps && OldrRealMs > 0.0) return OldrRealMs;
	return FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles(0));
}

#if WITH_OUTLAWS_TESTS
namespace
{
	// -OLDynResUnitTest: the rules alone (FOldrCore), fed synthetic timings at 72 Hz with the GPU's ms following the pixels
	// (the square of the scale): GPU-bound, CPU-bound (the real GPU light, and the GPU over the line but under the CPU),
	// mixed phases, noise and spikes, the floor and the ceiling, and the frame-cycles fallback (paced, then missed frames).
	struct FOldrUnit
	{
		int32 Pass = 0, Fail = 0;
		void Check(bool bOk, const FString& What)
		{
			(bOk ? Pass : Fail)++;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dynres unit check: %s %s"), bOk ? TEXT("ok") : TEXT("FAIL"), *What);
		}
	};

	struct FOldrRun
	{
		FOldrCore C;
		FOldrTuning T;
		double Now = 100.0;
		float MinScale = 10.f, MaxScale = 0.f;
		int32 Downs0 = 0, Ups0 = 0;
		FRandomStream Rng{1234};
		FString Trace;

		FOldrRun(float Ceiling, float Floor)
		{
			C.Ceiling = Ceiling;
			C.FloorAsked = Floor;
			C.Restart(Now - 10.0);
		}
		/** Runs Secs at 72 Hz: GpuAtCeiling(t) is the GPU's ms at the ceiling (scaled by the pixels), Cpu(t) the CPU pole. */
		template <typename FG, typename FC>
		void Run(double Secs, FG GpuAtCeiling, FC Cpu, bool bReal = true)
		{
			const double Dt = 1.0 / 72.0;
			const int32 N = int32(Secs * 72.0);
			Downs0 = C.Downs;
			Ups0 = C.Ups;
			MinScale = 10.f;
			MaxScale = 0.f;
			for (int32 i = 0; i < N; ++i)
			{
				const double t = i * Dt;
				FOldrSample S;
				S.Dt = Dt;
				S.BudgetMs = 1000.0 / 72.0;
				S.bRealGpu = bReal;
				S.GpuMs = GpuAtCeiling(t, *this) * (bReal ? FMath::Square(double(C.Scale) / double(C.Ceiling)) : 1.0);
				S.CpuMs = Cpu(t, *this);
				Now += Dt;
				const float From = C.Scale;
				C.Tick(Now, S, T);
				MinScale = FMath::Min(MinScale, C.Scale);
				MaxScale = FMath::Max(MaxScale, C.Scale);
				if (C.Scale != From) Trace += FString::Printf(TEXT(" %.1fs:%.2f"), t, C.Scale);
			}
		}
		int32 Downs() const { return C.Downs - Downs0; }
		int32 Ups() const { return C.Ups - Ups0; }
		double GpuNow(double AtCeiling) const { return AtCeiling * FMath::Square(double(C.Scale) / double(C.Ceiling)); }
	};

	bool OldrUnitTickFn(float)
	{
		FOldrUnit U;
		const double Budget = 1000.0 / 72.0;
		auto Flat = [](double V) { return [V](double, FOldrRun&) { return V; }; };
		// 1: GPU-bound (16 ms at the Quest 3's 1.0, CPU 8): down, settles under the down line, then holds 60 s
		{
			FOldrRun R(1.0f, 0.65f);
			R.Run(10.0, Flat(16.0), Flat(8.0));
			const float S1 = R.C.Scale;
			U.Check(R.Downs() >= 1 && S1 < 1.0f && S1 >= 0.65f - 0.001f && R.GpuNow(16.0) <= Budget * 0.90,
				FString::Printf(TEXT("GPU-bound 16 ms / CPU 8: down to %.2f (GPU %.1f ms, line %.1f), %d steps:%s"), S1, R.GpuNow(16.0), Budget * 0.9, R.Downs(), *R.Trace));
			R.Trace.Reset();
			R.Run(60.0, Flat(16.0), Flat(8.0));
			U.Check(R.Downs() == 0 && R.Ups() == 0 && R.C.Scale == S1, FString::Printf(TEXT("GPU-bound held 60 s: %.2f, %d down %d up (no hunting)"), R.C.Scale, R.Downs(), R.Ups()));
		}
		// 2: CPU-bound, the v0.9 Quest 3 case (real GPU 9.4 at 1.0, game work 13.5): stays at the ceiling 120 s
		{
			FOldrRun R(1.0f, 0.65f);
			R.Run(120.0, Flat(9.4), Flat(13.5));
			U.Check(R.Downs() == 0 && R.MinScale >= 1.0f - 0.001f, FString::Printf(TEXT("CPU-bound (GPU 9.4, CPU 13.5): at the ceiling 120 s, lowest %.2f, %d down"), R.MinScale, R.Downs()));
		}
		// 3: CPU-bound with the GPU over the line but under the CPU (GPU 13.0, CPU 14.5): holds at the ceiling
		{
			FOldrRun R(0.85f, 0.65f);
			R.Run(60.0, Flat(13.0), Flat(14.5));
			U.Check(R.Downs() == 0 && R.MinScale >= 0.85f - 0.001f && R.C.Last == EOldrSays::CpuPole,
				FString::Printf(TEXT("GPU 13.0 over the line, CPU 14.5 longer: holds %.2f (%d down; says '%s')"), R.MinScale, R.Downs(), OldrSaysText(R.C.Last)));
		}
		// 4: the same frame but the GPU the long pole (GPU 13.0, CPU 9): steps down
		{
			FOldrRun R(0.85f, 0.65f);
			R.Run(10.0, Flat(13.0), Flat(9.0));
			U.Check(R.Downs() >= 1 && R.C.Scale < 0.85f, FString::Printf(TEXT("GPU 13.0 the long pole (CPU 9): down to %.2f (%d steps)"), R.C.Scale, R.Downs()));
		}
		// 5: started at the floor (the v0.9 state) with GPU headroom and the CPU the long pole: climbs back to the ceiling
		{
			FOldrRun R(1.0f, 0.65f);
			R.C.Scale = 0.65f;
			R.Run(60.0, Flat(9.4), Flat(13.0));
			U.Check(FMath::IsNearlyEqual(R.C.Scale, 1.0f, 0.001f) && R.Downs() == 0, FString::Printf(TEXT("from the floor (GPU 9.4 at 1.0, CPU 13.0): back to %.2f in 60 s (%d up, %d down):%s"), R.C.Scale, R.Ups(), R.Downs(), *R.Trace));
		}
		// 6: mixed: 15 s GPU-heavy (GPU 17, CPU 9) then 45 s CPU-heavy (GPU 8, CPU 14), twice
		{
			FOldrRun R(0.85f, 0.65f);
			auto G = [](double t, FOldrRun&) { return FMath::Fmod(t, 60.0) < 15.0 ? 17.0 : 8.0; };
			auto Cp = [](double t, FOldrRun&) { return FMath::Fmod(t, 60.0) < 15.0 ? 9.0 : 14.0; };
			R.Run(15.0, G, Cp);
			const float Heavy = R.C.Scale;
			R.Run(105.0, [&G](double t, FOldrRun& X) { return G(t + 15.0, X); }, [&Cp](double t, FOldrRun& X) { return Cp(t + 15.0, X); });
			U.Check(Heavy < 0.85f && R.MinScale >= 0.65f - 0.001f && R.MaxScale <= 0.85f + 0.001f && R.C.Scale >= 0.85f - 0.001f && R.Downs() + R.Ups() <= 12,
				FString::Printf(TEXT("mixed phases: %.2f in the GPU-heavy phase, %.2f..%.2f after, ends %.2f (%d down %d up):%s"), Heavy, R.MinScale, R.MaxScale, R.C.Scale, R.Downs(), R.Ups(), *R.Trace));
		}
		// 7: noisy light (GPU 10 +-4 ms per frame, a 40 ms spike 1 frame in 100, CPU 9): no step at all in 120 s
		{
			FOldrRun R(1.0f, 0.65f);
			R.Run(120.0, [](double, FOldrRun& X) { return X.Rng.FRand() < 0.01f ? 40.0 : 10.0 + X.Rng.FRandRange(-4.f, 4.f); }, Flat(9.0));
			U.Check(R.Downs() == 0 && R.MinScale >= 1.0f - 0.001f, FString::Printf(TEXT("noisy light GPU (10 +-4, spikes 40): %d down, lowest %.2f"), R.Downs(), R.MinScale));
		}
		// 8: noisy heavy (GPU 16 +-4, CPU 8 +-3): settles and stays within one step for the last 60 s
		{
			FOldrRun R(1.0f, 0.65f);
			auto G = [](double, FOldrRun& X) { return 16.0 + X.Rng.FRandRange(-4.f, 4.f); };
			auto Cp = [](double, FOldrRun& X) { return 8.0 + X.Rng.FRandRange(-3.f, 3.f); };
			R.Run(30.0, G, Cp);
			const float S1 = R.C.Scale;
			R.Trace.Reset();
			R.Run(60.0, G, Cp);
			U.Check(S1 < 1.0f && R.MaxScale - R.MinScale <= 0.05f + 0.001f && R.Downs() + R.Ups() <= 2,
				FString::Printf(TEXT("noisy heavy GPU (16 +-4): %.2f after 30 s, %.2f..%.2f the next 60 s (%d down %d up):%s"), S1, R.MinScale, R.MaxScale, R.Downs(), R.Ups(), *R.Trace));
		}
		// 9: very heavy (40 ms): the floor, never under; then light (6 ms): the ceiling, never over
		{
			FOldrRun R(0.85f, 0.65f);
			R.Run(10.0, Flat(40.0), Flat(8.0));
			U.Check(FMath::IsNearlyEqual(R.C.Scale, 0.65f, 0.001f) && R.MinScale >= 0.65f - 0.001f, FString::Printf(TEXT("very heavy 40 ms: at the floor %.2f, lowest %.2f"), R.C.Scale, R.MinScale));
			R.Run(90.0, Flat(6.0), Flat(8.0));
			U.Check(FMath::IsNearlyEqual(R.C.Scale, 0.85f, 0.001f) && R.MaxScale <= 0.85f + 0.001f, FString::Printf(TEXT("then light 6 ms: back to %.2f, highest %.2f (ceiling 0.85)"), R.C.Scale, R.MaxScale));
		}
		// 10: the fallback (frame cycles): paced at the frame (13.9 every frame, CPU 9.7) holds; frames missed (27.8) step down
		{
			FOldrRun R(1.0f, 0.65f);
			R.Run(60.0, Flat(Budget), Flat(9.7), false);
			U.Check(R.Downs() == 0, FString::Printf(TEXT("frame cycles paced at %.1f (CPU 9.7): %d down (v0.9 went to the floor here)"), Budget, R.Downs()));
			R.Run(5.0, Flat(Budget * 2.0), Flat(9.0), false);
			U.Check(R.Downs() >= 1, FString::Printf(TEXT("frame cycles at %.1f (frames missed, CPU 9): %d down, %.2f"), Budget * 2.0, R.Downs(), R.C.Scale));
		}
		// 11: a hitch (a 0.4 s frame) and the hold after a resume read nothing
		{
			FOldrRun R(0.85f, 0.65f);
			FOldrSample S;
			S.Dt = 0.4;
			S.GpuMs = 400.0;
			S.CpuMs = 5.0;
			S.BudgetMs = Budget;
			R.Now += 0.4;
			const EOldrSays A = R.C.Tick(R.Now, S, R.T);
			R.C.Hold(R.Now, 2.0);
			R.Run(1.9, Flat(30.0), Flat(5.0));
			U.Check(A == EOldrSays::Wait && R.Downs() == 0 && R.C.Smooth <= 0.0, FString::Printf(TEXT("a 0.4 s hitch and 1.9 s of a resume hold: %d down (says '%s')"), R.Downs(), OldrSaysText(A)));
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dynres unit: %s (%d ok, %d failed)"), U.Fail == 0 ? TEXT("PASS") : TEXT("FAIL"), U.Pass, U.Fail);
		FGenericPlatformMisc::RequestExit(false);
		return false;
	}

	void OldrStartUnitTest()
	{
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&OldrUnitTickFn), 1.0f);
	}

	// -OLDynResTest (desktop, with -OLDeviceProfile=Quest2): the controller forced on, fake loads (ol.dynres.FakeGpuMs, the
	// ms at the ceiling, scaled with the pixels; ol.dynres.FakeCpuMs, the CPU pole) stepped: a CPU-paced frame (the GPU over
	// the line but under the CPU) holds the ceiling; over budget it steps down and settles under the down line, a heavy load
	// stops at the floor, a light one brings it back to the ceiling while the enemy throttle is held at its tightest
	// (neither moves the other), a resume keeps the controller's value, a Resolution row of 110 % is the ceiling it works
	// under (BUG-SCREENPCT: the row says "up to", the choice is saved and holds over a reload, never passed going up), and off
	// puts the profile's back.
	struct FOldrTest
	{
		int32 Step = 0;
		double T0 = 0.0, StepAt = 0.0, LevelAt = 0.0;
		int32 Pass = 0, Fail = 0;
		int32 Downs0 = 0, Ups0 = 0;
		float Settled = 0.f, MaxScale = 0.f, MinScale = 10.f;
		TMap<FString, FString> Saved0;
		double FirstDownSec = -1.0, RecoverSec = -1.0;
		void Check(bool bOk, const FString& What)
		{
			(bOk ? Pass : Fail)++;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dynres check: %s %s"), bOk ? TEXT("ok") : TEXT("FAIL"), *What);
		}
	};
	FOldrTest GOldrTest;

	AOutlawsLevelActor* OldrTestLevel()
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

	float OldrPd()
	{
		IConsoleVariable* Pd = IConsoleManager::Get().FindConsoleVariable(TEXT("vr.PixelDensity"));
		return Pd ? Pd->GetFloat() : -1.f;
	}

	void OldrSetCvar(const TCHAR* Name, const TCHAR* Value)
	{
		if (IConsoleVariable* V = IConsoleManager::Get().FindConsoleVariable(Name)) V->Set(Value, ECVF_SetByCode);
	}

	bool OldrTestTickFn(float)
	{
		FOldrTest& T = GOldrTest;
		const double Now = FPlatformTime::Seconds();
		AOutlawsLevelActor* L = OldrTestLevel();
		const double In = Now - T.StepAt;
		const double Budget = OldrBudgetMs();
		auto GpuAt = [](float Fake) { return double(Fake) * FMath::Square(double(GCore.Scale) / double(GCore.Ceiling)); };
		switch (T.Step)
		{
		case 0:
			if (!L)
			{
				if (Now - T.T0 > 120.0) { T.Check(false, TEXT("no level in 120 s")); T.Step = 99; }
				return true;
			}
			if (T.LevelAt <= 0.0) T.LevelAt = Now;
			if (Now - T.LevelAt < 6.0) return true;
			OldrSetCvar(TEXT("ol.dynres.Enable"), TEXT("1"));
			T.Check(OutlawsDynRes::IsActive(), TEXT("on (ol.dynres.Enable 1)"));
			T.Check(FMath::IsNearlyEqual(GCore.Ceiling, 0.85f, 0.001f) && FMath::IsNearlyEqual(OldrPd(), 0.85f, 0.001f),
				FString::Printf(TEXT("starts at the Quest 2 ceiling: ceiling %.2f, vr.PixelDensity %.2f (want 0.85)"), GCore.Ceiling, OldrPd()));
			T.Check(FMath::IsNearlyEqual(OldrFloor(), 0.65f, 0.001f), FString::Printf(TEXT("floor %.2f (want 0.65)"), OldrFloor()));
			T.Check(FMath::IsNearlyEqual(float(Budget), 1000.f / 72.f, 0.05f), FString::Printf(TEXT("budget %.2f ms (72 Hz)"), Budget));
			// 0b: CPU-paced (QSPEED-4): the GPU over the down line (13.2 ms) but the CPU longer (14.5): the ceiling holds
			OldrSetCvar(TEXT("ol.dynres.FakeGpuMs"), TEXT("13.2"));
			OldrSetCvar(TEXT("ol.dynres.FakeCpuMs"), TEXT("14.5"));
			T.Downs0 = GCore.Downs;
			T.MinScale = 10.f;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dynres: CPU-paced: GPU 13.2 ms at the ceiling, CPU pole 14.5 (budget %.1f)"), Budget);
			T.Step = 20;
			T.StepAt = Now;
			return true;
		case 20:
			T.MinScale = FMath::Min(T.MinScale, GCore.Scale);
			if (In < 6.0) return true;
			T.Check(GCore.Downs == T.Downs0 && T.MinScale >= 0.85f - 0.001f && FMath::IsNearlyEqual(OldrPd(), 0.85f, 0.001f),
				FString::Printf(TEXT("CPU-paced 6 s: holds the ceiling (lowest %.2f, %d steps down, vr.PixelDensity %.2f; says '%s')"), T.MinScale, GCore.Downs - T.Downs0, OldrPd(), OldrSaysText(GCore.Last)));
			// 1: over budget (18 ms at 0.85), the CPU light (4 ms)
			OldrSetCvar(TEXT("ol.dynres.FakeCpuMs"), TEXT("4"));
			OldrSetCvar(TEXT("ol.dynres.FakeGpuMs"), TEXT("18"));
			T.Downs0 = GCore.Downs;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dynres: load 18 ms at the ceiling, CPU 4 (budget %.1f)"), Budget);
			T.Step = 1;
			T.StepAt = Now;
			return true;
		case 1:
			if (T.FirstDownSec < 0.0 && GCore.Downs > T.Downs0) T.FirstDownSec = In;
			if (In < 6.0) return true;
			T.Check(GCore.Downs > T.Downs0 && T.FirstDownSec >= 0.0 && T.FirstDownSec < 2.0, FString::Printf(TEXT("steps down under load: %d steps, the first after %.2f s"), GCore.Downs - T.Downs0, T.FirstDownSec));
			T.Check(GCore.Scale < 0.85f && GCore.Scale >= 0.65f - 0.001f, FString::Printf(TEXT("scale %.2f (under 0.85, not under 0.65)"), GCore.Scale));
			T.Check(GpuAt(18.f) <= Budget * 0.90, FString::Printf(TEXT("settled under the down line: GPU %.1f ms at %.2f (line %.1f)"), GpuAt(18.f), GCore.Scale, Budget * 0.90));
			T.Check(FMath::IsNearlyEqual(OldrPd(), GCore.Scale, 0.001f), FString::Printf(TEXT("vr.PixelDensity %.2f is the controller's"), OldrPd()));
			T.Settled = GCore.Scale;
			T.Downs0 = GCore.Downs;
			T.Ups0 = GCore.Ups;
			T.Step = 2;
			T.StepAt = Now;
			return true;
		case 2:
			// 2: the same load held: no hunting
			if (In < 5.0) return true;
			T.Check(GCore.Downs == T.Downs0 && GCore.Ups == T.Ups0 && GCore.Scale == T.Settled, FString::Printf(TEXT("steady under the same load 5 s: scale %.2f (was %.2f), steps %d down %d up"),
				GCore.Scale, T.Settled, GCore.Downs - T.Downs0, GCore.Ups - T.Ups0));
			// 3: a heavy load (30 ms at the ceiling): the floor and no further
			OldrSetCvar(TEXT("ol.dynres.FakeGpuMs"), TEXT("30"));
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dynres: load 30 ms at the ceiling"));
			T.Step = 3;
			T.StepAt = Now;
			return true;
		case 3:
			if (In < 6.0) return true;
			T.Check(FMath::IsNearlyEqual(GCore.Scale, 0.65f, 0.001f) && FMath::IsNearlyEqual(OldrPd(), 0.65f, 0.001f), FString::Printf(TEXT("heavy load: at the floor %.2f (vr.PixelDensity %.2f)"), GCore.Scale, OldrPd()));
			// 4: light (8 ms) with the enemy throttle held at its tightest: the resolution comes back, the throttle stays
			OldrSetCvar(TEXT("ol.dynres.FakeGpuMs"), TEXT("8"));
			OldrSetCvar(TEXT("ol.throttle.ForceLevel"), TEXT("3"));
			T.Ups0 = GCore.Ups;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dynres: load 8 ms at the ceiling, the enemy throttle held at level 3"));
			T.Step = 4;
			T.StepAt = Now;
			return true;
		case 4:
			if (T.RecoverSec < 0.0 && GCore.Scale >= 0.85f - 0.001f) T.RecoverSec = In;
			if (In < 20.0 && T.RecoverSec < 0.0) return true;
			if (In < 1.0) return true;
			T.Check(T.RecoverSec >= 0.0, FString::Printf(TEXT("recovers to the ceiling under a light load: %.2f after %.1f s (%d steps up)"), GCore.Scale, T.RecoverSec, GCore.Ups - T.Ups0));
			T.Check(L && L->ThrottleLevel == 3, FString::Printf(TEXT("the enemy throttle left at its level 3 meanwhile (level %d)"), L ? L->ThrottleLevel : -1));
			OldrSetCvar(TEXT("ol.throttle.ForceLevel"), TEXT("-1"));
			// 5: down again, then a resume with vr.PixelDensity lost: the controller's value comes back
			OldrSetCvar(TEXT("ol.dynres.FakeGpuMs"), TEXT("18"));
			T.Step = 5;
			T.StepAt = Now;
			return true;
		case 5:
		{
			if (In < 4.0) return true;
			const float Want = GCore.Scale;
			T.Check(Want < 0.85f, FString::Printf(TEXT("down again before the resume: %.2f"), Want));
			OldrSetCvar(TEXT("vr.PixelDensity"), TEXT("1.0"));
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dynres: resume with vr.PixelDensity lost (1.00; the controller's %.2f)"), Want);
			OutlawsDeviceProfile::OnAppResume(TEXT("simulated resume (dynres test)"));
			T.Check(FMath::IsNearlyEqual(OldrPd(), Want, 0.001f), FString::Printf(TEXT("resume: vr.PixelDensity %.2f (want the controller's %.2f, not the profile's 0.85)"), OldrPd(), Want));
			T.Settled = Want;
			T.Downs0 = GCore.Downs;
			T.Ups0 = GCore.Ups;
			T.Step = 6;
			T.StepAt = Now;
			return true;
		}
		case 6:
		{
			if (In < 1.0) return true;
			T.Check(GCore.Downs == T.Downs0 && GCore.Ups == T.Ups0, TEXT("no step in the hold after the resume"));
			// 6: the Resolution row stepped to 110 %: the ceiling, started from, and the row says so
			T.Saved0.Reset();
			OutlawsDeviceProfile::GatherSettings(T.Saved0);
			int32 Presses = 0;
			while (OutlawsDeviceProfile::ResolutionPercent() != 110 && Presses < 12)
			{
				OutlawsDeviceProfile::PressRow(TEXT("r_resolution"));
				++Presses;
			}
			const FString Row = OutlawsDeviceProfile::RowValue(TEXT("r_resolution"));
			T.Check(OutlawsDeviceProfile::ResolutionPercent() == 110 && FMath::IsNearlyEqual(GCore.Ceiling, 1.10f, 0.001f) && FMath::IsNearlyEqual(OldrPd(), 1.10f, 0.001f),
				FString::Printf(TEXT("110 %% chosen (%d presses): ceiling %.2f, vr.PixelDensity %.2f (want 1.10, 1.10)"), Presses, GCore.Ceiling, OldrPd()));
			T.Check(Row == TEXT("up to 110 %"), FString::Printf(TEXT("the row says '%s' (want 'up to 110 %%')"), *Row));
			OldrSetCvar(TEXT("ol.dynres.FakeGpuMs"), TEXT("18"));
			T.Downs0 = GCore.Downs;
			T.Step = 7;
			T.StepAt = Now;
			return true;
		}
		case 7:
		{
			// 7: under load it steps down under 110 %; the choice is kept, saved as 110 and holds over a settings reload
			if (In < 6.0) return true;
			T.Check(GCore.Downs > T.Downs0 && GCore.Scale < 1.10f && FMath::IsNearlyEqual(GCore.Ceiling, 1.10f, 0.001f) && OutlawsDeviceProfile::ResolutionPercent() == 110,
				FString::Printf(TEXT("110 %% under load: scale %.2f, ceiling %.2f, row %d %% (%d steps down)"), GCore.Scale, GCore.Ceiling, OutlawsDeviceProfile::ResolutionPercent(), GCore.Downs - T.Downs0));
			TMap<FString, FString> S;
			OutlawsDeviceProfile::GatherSettings(S);
			T.Check(S.FindRef(TEXT("RenderScale")) == TEXT("110"), FString::Printf(TEXT("saved RenderScale=%s (want 110)"), *S.FindRef(TEXT("RenderScale"))));
			const float Before = GCore.Scale;
			OutlawsDeviceProfile::LoadSettings(S);
			T.Check(OutlawsDeviceProfile::ResolutionPercent() == 110 && FMath::IsNearlyEqual(GCore.Ceiling, 1.10f, 0.001f) && FMath::IsNearlyEqual(GCore.Scale, Before, 0.001f)
				&& FMath::IsNearlyEqual(OldrPd(), Before, 0.001f), FString::Printf(TEXT("reloaded: row %d %%, ceiling %.2f, scale %.2f, vr.PixelDensity %.2f (want 110, 1.10, %.2f kept)"),
				OutlawsDeviceProfile::ResolutionPercent(), GCore.Ceiling, GCore.Scale, OldrPd(), Before));
			OldrSetCvar(TEXT("ol.dynres.FakeGpuMs"), TEXT("8"));
			T.MaxScale = GCore.Scale;
			T.RecoverSec = -1.0;
			T.Step = 8;
			T.StepAt = Now;
			return true;
		}
		case 8:
			// 8: light again: back up to 110 % and never past it
			T.MaxScale = FMath::Max(T.MaxScale, GCore.Scale);
			if (T.RecoverSec < 0.0 && GCore.Scale >= 1.10f - 0.001f) T.RecoverSec = In;
			// (up to 2 min: the down just after step 4's ups doubles the wait between ups, to 12 s)
			if (In < 120.0 && (T.RecoverSec < 0.0 || In < T.RecoverSec + 6.0)) return true;
			T.Check(T.RecoverSec >= 0.0 && T.MaxScale <= 1.10f + 0.001f, FString::Printf(TEXT("light load: back to %.2f after %.1f s, highest %.2f (want 1.10, never over)"), GCore.Scale, T.RecoverSec, T.MaxScale));
			OutlawsDeviceProfile::LoadSettings(T.Saved0);
			T.Check(OutlawsDeviceProfile::ResolutionPercent() == 85, FString::Printf(TEXT("the choice put back: %d %% (want the Quest 2's 85)"), OutlawsDeviceProfile::ResolutionPercent()));
			T.Step = 9;
			T.StepAt = Now;
			return true;
		case 9:
			// 9: off: the profile's value back
			OldrSetCvar(TEXT("ol.dynres.FakeGpuMs"), TEXT("0"));
			OldrSetCvar(TEXT("ol.dynres.FakeCpuMs"), TEXT("0"));
			OldrSetCvar(TEXT("ol.dynres.Enable"), TEXT("0"));
			T.Check(!OutlawsDynRes::IsActive() && FMath::IsNearlyEqual(OldrPd(), 0.85f, 0.001f), FString::Printf(TEXT("off: vr.PixelDensity %.2f (want the profile's 0.85)"), OldrPd()));
			T.Step = 99;
			return true;
		default:
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dynres: %s (%d ok, %d failed; first step down after %.2f s, back to the ceiling after %.1f s, steps down %d up %d)"),
				T.Fail == 0 ? TEXT("PASS") : TEXT("FAIL"), T.Pass, T.Fail, T.FirstDownSec, T.RecoverSec, GCore.Downs, GCore.Ups);
			OldrTestTick.Reset();
			FGenericPlatformMisc::RequestExit(false);
			return false;
		}
	}

	void OldrStartTest()
	{
		GOldrTest = FOldrTest();
		GOldrTest.T0 = FPlatformTime::Seconds();
		OldrTestTick = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&OldrTestTickFn), 0.1f);
	}

	// -OLDynResOffTest (BUG-DYNRESOFF, desktop, with -OLDeviceProfile=Quest2): the default (off): vr.PixelDensity is the
	// Resolution row's value and never moves over 60 s; a press of the row changes it at once (the row without "up to"),
	// the choice is in the settings the menu saves, and the old choice put back restores it.
	struct FOldrOffTest
	{
		int32 Step = 0, Pass = 0, Fail = 0, Samples = 0;
		double T0 = 0.0, LevelAt = 0.0, StepAt = 0.0;
		float Want = 0.f, Lo = 10.f, Hi = -10.f;
		TMap<FString, FString> Saved0;
		void Check(bool bOk, const FString& What)
		{
			(bOk ? Pass : Fail)++;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dynresoff check: %s %s"), bOk ? TEXT("ok") : TEXT("FAIL"), *What);
		}
	};
	FOldrOffTest GOldrOff;

	bool OldrOffTickFn(float)
	{
		FOldrOffTest& T = GOldrOff;
		const double Now = FPlatformTime::Seconds();
		switch (T.Step)
		{
		case 0:
			if (!OldrTestLevel())
			{
				if (Now - T.T0 > 120.0) { T.Check(false, TEXT("no level in 120 s")); T.Step = 99; }
				return true;
			}
			if (T.LevelAt <= 0.0) T.LevelAt = Now;
			if (Now - T.LevelAt < 6.0) return true;
			T.Want = OutlawsDeviceProfile::ResolutionPercent() / 100.f;
			T.Check(!OutlawsDynRes::WantsControl() && !OutlawsDynRes::IsActive(), FString::Printf(TEXT("off by default (ol.dynres.Enable %d)"), CVarOldrEnable.GetValueOnGameThread()));
			T.Check(FMath::IsNearlyEqual(OldrPd(), T.Want, 0.001f), FString::Printf(TEXT("vr.PixelDensity %.2f is the row's %d %%"), OldrPd(), OutlawsDeviceProfile::ResolutionPercent()));
			T.Step = 1;
			T.StepAt = Now;
			return true;
		case 1:
			T.Lo = FMath::Min(T.Lo, OldrPd());
			T.Hi = FMath::Max(T.Hi, OldrPd());
			++T.Samples;
			if (Now - T.StepAt < 60.0) return true;
			T.Check(FMath::IsNearlyEqual(T.Lo, T.Want, 0.001f) && FMath::IsNearlyEqual(T.Hi, T.Want, 0.001f),
				FString::Printf(TEXT("60 s: vr.PixelDensity %.2f..%.2f over %d samples (want %.2f throughout)"), T.Lo, T.Hi, T.Samples, T.Want));
			T.Step = 2;
			return true;
		case 2:
		{
			T.Saved0.Reset();
			OutlawsDeviceProfile::GatherSettings(T.Saved0);
			const int32 Before = OutlawsDeviceProfile::ResolutionPercent();
			OutlawsDeviceProfile::PressRow(TEXT("r_resolution"));
			const int32 After = OutlawsDeviceProfile::ResolutionPercent();
			const FString Row = OutlawsDeviceProfile::RowValue(TEXT("r_resolution"));
			TMap<FString, FString> S;
			OutlawsDeviceProfile::GatherSettings(S);
			T.Check(After != Before && FMath::IsNearlyEqual(OldrPd(), After / 100.f, 0.001f),
				FString::Printf(TEXT("row pressed: %d %% -> %d %%, vr.PixelDensity %.2f at once"), Before, After, OldrPd()));
			T.Check(Row == FString::Printf(TEXT("%d %%"), After), FString::Printf(TEXT("the row says '%s'"), *Row));
			T.Check(S.FindRef(TEXT("RenderScale")) == FString::FromInt(After), FString::Printf(TEXT("saved as RenderScale=%s"), *S.FindRef(TEXT("RenderScale"))));
			T.Want = After / 100.f;
			T.StepAt = Now;
			T.Step = 3;
			return true;
		}
		case 3:
			if (Now - T.StepAt < 5.0)
			{
				if (!FMath::IsNearlyEqual(OldrPd(), T.Want, 0.001f)) { T.Check(false, FString::Printf(TEXT("moved to %.2f after the press"), OldrPd())); T.Step = 4; }
				return true;
			}
			T.Check(FMath::IsNearlyEqual(OldrPd(), T.Want, 0.001f), FString::Printf(TEXT("holds %.2f for 5 s after the press"), OldrPd()));
			T.Step = 4;
			return true;
		case 4:
			OutlawsDeviceProfile::LoadSettings(T.Saved0);
			T.Check(FMath::IsNearlyEqual(OldrPd(), OutlawsDeviceProfile::ResolutionPercent() / 100.f, 0.001f),
				FString::Printf(TEXT("the old choice put back: %d %%, vr.PixelDensity %.2f"), OutlawsDeviceProfile::ResolutionPercent(), OldrPd()));
			T.Step = 99;
			return true;
		default:
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT dynresoff: %s (%d ok, %d failed)"), T.Fail == 0 ? TEXT("PASS") : TEXT("FAIL"), T.Pass, T.Fail);
			FGenericPlatformMisc::RequestExit(false);
			return false;
		}
	}

	void OldrStartOffTest()
	{
		GOldrOff = FOldrOffTest();
		GOldrOff.T0 = FPlatformTime::Seconds();
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&OldrOffTickFn), 0.1f);
	}
}
#endif
