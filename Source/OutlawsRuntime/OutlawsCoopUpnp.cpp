// REQ-UPNP (v1.1): a host asks its router to forward the game's UDP port (OutlawsUpnp.cpp speaks the protocol).
//   - When a host starts listening (co-op or a match), a worker thread finds the router, asks its external address and maps
//     the listen port (UDP, outside and in the same) to this machine with a lease (an hour; renewed at half of it while
//     hosting).  3 s at most; the game thread only reads the answer.
//   - Leave, End Game and every other session end remove the mapping on a worker; quitting removes it before the game goes
//     (3 s at most).  A crash leaves it to the lease.
//   - The lobby's Internet line says what happened; a router whose own external address is a shared one (or differs from the
//     ipify answer, which is only asked once the host pressed Show internet address) means port forwarding can't work.
//   - Options > Multiplayer "Open my router's port automatically" (MpUpnp, on by default) switches it off.
// Tests: -OLUpnpSelfTest (the parsing), -OLUpnpProbe (read-only: discovery and the external address, never a mapping),
// -OLUpnpSsdp=<ip:port> (a mock router on this machine), -OLUpnpLease=<s>, -OLUpnpTest=leave|quit -OLUpnpTestAfter=<s>,
// -OLUpnpQuit (quit once the self test or the probe is done).  An -unattended run without the mock never asks for a mapping.
#include "OutlawsCoop.h"
#include "OutlawsFlags.h"
#include "OutlawsUpnp.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "Async/Async.h"
#include "Misc/ScopeLock.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"

struct FOutlawsUpnpJob
{
	enum class EKind : uint8 { Probe, Open, Renew, Close };
	EKind Kind = EKind::Probe;
	OutlawsUpnp::FGateway Gateway;               // in for Renew and Close, found for Probe and Open
	int32 Port = 0;
	int32 Lease = 0;
	TAtomic<bool> bDone { false };
	TAtomic<bool> bCancel { false };             // the session ended while it ran: the worker removes what it mapped
	bool bFound = false, bOk = false, bMapped = false, bUndone = false;
	FString External, ExternalWhy, Why;
	double Took = 0.0;
};

struct FOutlawsUpnpState
{
	OutlawsUpnp::FGateway Gateway;
	int32 MappedPort = 0;
	int32 Lease = 3600;
	double RenewAt = 0.0;
	TSharedPtr<FOutlawsUpnpJob, ESPMode::ThreadSafe> Job;
	TArray<TSharedPtr<FOutlawsUpnpJob, ESPMode::ThreadSafe>> Closing;     // removals (and cancelled jobs) still on their way
};

namespace
{
	FCriticalSection GUpnpLock;                  // (one exchange with the router at a time, in the order asked)
	constexpr double UpnpLimit = 3.0;

	// the tests' state (one game instance per process)
	FString GTestMode;
	double GTestAfter = 15.0, GTestAt = 0.0;
	int32 GTestPhase = 0;
	bool GQuitWhenDone = false, GQuitPending = false;

	using FJobPtr = TSharedPtr<FOutlawsUpnpJob, ESPMode::ThreadSafe>;

	void RunJob(FOutlawsUpnpJob& J)
	{
		FScopeLock Lock(&GUpnpLock);
		const double T0 = FPlatformTime::Seconds(), Deadline = T0 + UpnpLimit;
		const FString Desc = FString::Printf(TEXT("Outlaws VR (UDP %d)"), J.Port);
		switch (J.Kind)
		{
		case FOutlawsUpnpJob::EKind::Probe:
		case FOutlawsUpnpJob::EKind::Open:
			if (!OutlawsUpnp::Find(Deadline, J.Gateway, J.Why)) break;
			J.bFound = true;
			if (!OutlawsUpnp::ExternalIp(J.Gateway, Deadline, J.External, J.ExternalWhy)) J.External.Reset();
			if (J.Kind == FOutlawsUpnpJob::EKind::Probe) { J.bOk = true; break; }
			J.bOk = J.bMapped = !J.bCancel && OutlawsUpnp::AddUdp(J.Gateway, J.Port, J.Lease, Desc, Deadline, J.Why);
			break;
		case FOutlawsUpnpJob::EKind::Renew:
			J.bOk = J.bMapped = !J.bCancel && OutlawsUpnp::AddUdp(J.Gateway, J.Port, J.Lease, Desc, Deadline, J.Why);
			break;
		case FOutlawsUpnpJob::EKind::Close:
			J.bOk = OutlawsUpnp::DeleteUdp(J.Gateway, J.Port, Deadline, J.Why);
			break;
		}
		if (J.bMapped && J.bCancel)
		{
			FString W;
			J.bUndone = OutlawsUpnp::DeleteUdp(J.Gateway, J.Port, FPlatformTime::Seconds() + UpnpLimit, W);
			J.bMapped = false;
		}
		J.Took = FPlatformTime::Seconds() - T0;
		J.bDone = true;
	}

	FJobPtr Launch(FOutlawsUpnpJob::EKind Kind, const OutlawsUpnp::FGateway& G, int32 Port, int32 Lease)
	{
		FJobPtr J = MakeShared<FOutlawsUpnpJob, ESPMode::ThreadSafe>();
		J->Kind = Kind;
		J->Gateway = G;
		J->Port = Port;
		J->Lease = Lease;
		Async(EAsyncExecution::Thread, [J]() { RunJob(*J); });
		return J;
	}

	bool LooksLikeV4(const FString& S)
	{
		int32 Dots = 0;
		for (const TCHAR C : S)
		{
			if (C == '.') ++Dots;
			else if (!FChar::IsDigit(C)) return false;
		}
		return Dots == 3;
	}

	int32 Ms(double S) { return FMath::RoundToInt(S * 1000.0); }
}

bool UOutlawsCoop::UpnpSetting()
{
	const TMap<FString, FString> S = AOutlawsLevelActor::ReadSettings();
	const FString* X = S.Find(TEXT("MpUpnp"));
	return !X || X->ToBool();
}

void UOutlawsCoop::ToggleUpnp()
{
	TMap<FString, FString> S = AOutlawsLevelActor::ReadSettings();
	const bool bOn = !UpnpSetting();
	S.Add(TEXT("MpUpnp"), bOn ? TEXT("true") : TEXT("false"));
	AOutlawsLevelActor::WriteSettings(S);
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLUPNP Open my router's port automatically: %s (from the next hosting)"), bOn ? TEXT("on") : TEXT("off"));
}

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::InitUpnpTests()
{
	OLFlags::Value(TEXT("OLUpnpTest"), GTestMode);
	GTestMode = GTestMode.ToLower();
	OLFlags::Value(TEXT("OLUpnpTestAfter"), GTestAfter);
	GQuitWhenDone = OLFlags::Param(TEXT("OLUpnpQuit"));
	if (OLFlags::Param(TEXT("OLUpnpSelfTest")))
	{
		OutlawsUpnp::SelfTest();
		GQuitPending = GQuitWhenDone;
	}
	if (OLFlags::Param(TEXT("OLUpnpProbe")))
	{
		// (read-only: who answers the search, its service, this machine's address toward it and the router's external address)
		if (!Upnp) Upnp = MakeShared<FOutlawsUpnpState, ESPMode::ThreadSafe>();
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLUPNP probe: searching %s (read-only: discovery and the external address)"),
			OutlawsUpnp::SsdpTarget().IsEmpty() ? TEXT("the home network") : *OutlawsUpnp::SsdpTarget());
		Upnp->Job = Launch(FOutlawsUpnpJob::EKind::Probe, OutlawsUpnp::FGateway(), 0, 0);
		GQuitPending = false;
	}
}
#endif // WITH_OUTLAWS_TESTS

bool UOutlawsCoop::UpnpBusy() const
{
	return GQuitPending || (!GTestMode.IsEmpty() && GTestPhase > 0 && GTestPhase < 3) || (Upnp && (Upnp->Job.IsValid() || Upnp->Closing.Num() > 0));
}

void UOutlawsCoop::StartUpnp()
{
	if (!IsHost() || UpnpState != EUpnp::Idle) return;
	if (!UpnpSetting())
	{
		UpnpState = EUpnp::Off;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLUPNP not asked: Options > Multiplayer > Open my router's port automatically is off"));
		++Version;
		return;
	}
	const bool bMock = !OutlawsUpnp::SsdpTarget().IsEmpty();
	if (!bMock && (FParse::Param(FCommandLine::Get(), TEXT("unattended")) || IsTestRun() || bLaunchHost))
	{
		// (a test's host never asks the real router for a mapping; a test does that against the mock, -OLUpnpSsdp=)
		UpnpState = EUpnp::Skipped;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLUPNP not asked: a test run without the mock router"));
		return;
	}
	if (!Upnp) Upnp = MakeShared<FOutlawsUpnpState, ESPMode::ThreadSafe>();
	FOutlawsUpnpState& U = *Upnp;
	U.Lease = 3600;
	OLFlags::Value(TEXT("OLUpnpLease"), U.Lease);
	U.Lease = FMath::Max(0, U.Lease);
	if (U.Job.IsValid())
	{
		U.Job->bCancel = true;                       // (a probe still running: let it finish on its own)
		U.Closing.Add(U.Job);
	}
	UpnpState = EUpnp::Asking;
	UpnpExternal.Reset();
	UpnpWhy.Reset();
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLUPNP asking the router to forward UDP %d to this machine (lease %d s, %.0f s at most)"), ListenPort, U.Lease, UpnpLimit);
	U.Job = Launch(FOutlawsUpnpJob::EKind::Open, OutlawsUpnp::FGateway(), ListenPort, U.Lease);
	++Version;
}

void UOutlawsCoop::StopUpnp(bool bWait)
{
	const bool bWas = UpnpState != EUpnp::Idle;
	UpnpState = EUpnp::Idle;
	UpnpExternal.Reset();
	UpnpWhy.Reset();
	if (!Upnp) return;
	FOutlawsUpnpState& U = *Upnp;
	if (U.Job.IsValid() && U.Job->Kind != FOutlawsUpnpJob::EKind::Probe)
	{
		U.Job->bCancel = true;                       // (the worker removes a mapping it makes after this)
		U.Closing.Add(U.Job);
		U.Job.Reset();
	}
	if (U.MappedPort > 0)
	{
		if (bWait)
		{
			// quitting: here and now, the game waits (3 s at most)
			FOutlawsUpnpJob J;
			J.Kind = FOutlawsUpnpJob::EKind::Close;
			J.Gateway = U.Gateway;
			J.Port = U.MappedPort;
			RunJob(J);
			if (J.bOk) { UE_LOG(LogOutlawsCoop, Log, TEXT("OLUPNP mapping removed on quit (UDP %d) in %d ms"), J.Port, Ms(J.Took)); }
			else { UE_LOG(LogOutlawsCoop, Warning, TEXT("OLUPNP removing the mapping on quit failed: %s (the lease ends it)"), *J.Why); }
		}
		else
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLUPNP removing the mapping (UDP %d): the session ended"), U.MappedPort);
			U.Closing.Add(Launch(FOutlawsUpnpJob::EKind::Close, U.Gateway, U.MappedPort, 0));
		}
		U.MappedPort = 0;
	}
	else if (bWas) { UE_LOG(LogOutlawsCoop, Log, TEXT("OLUPNP session ended: no mapping to remove")); }
	if (bWait)
	{
		const double Until = FPlatformTime::Seconds() + UpnpLimit + 0.5;
		for (;;)
		{
			bool bAll = !U.Job.IsValid() || U.Job->bDone;
			for (const FJobPtr& J : U.Closing) bAll &= J->bDone;
			if (bAll || FPlatformTime::Seconds() > Until) break;
			FPlatformProcess::Sleep(0.02f);
		}
		for (const FJobPtr& J : U.Closing)
		{
			if (!J->bDone) { UE_LOG(LogOutlawsCoop, Warning, TEXT("OLUPNP quitting with a router exchange still on its way (the lease ends it)")); }
			else if (J->Kind == FOutlawsUpnpJob::EKind::Close) { UE_LOG(LogOutlawsCoop, Log, TEXT("OLUPNP mapping removed (UDP %d) %s"), J->Port, J->bOk ? TEXT("before quitting") : *(TEXT("failed: ") + J->Why)); }
			else if (J->bUndone) { UE_LOG(LogOutlawsCoop, Log, TEXT("OLUPNP the mapping made as the session ended was removed (UDP %d)"), J->Port); }
		}
		U.Closing.Reset();
	}
	if (bWas) ++Version;
}

void UOutlawsCoop::TickUpnp(double Now)
{
	if (GQuitPending && (!Upnp || !Upnp->Job.IsValid()))
	{
		GQuitPending = false;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLUPNP test done: quitting"));
		FPlatformMisc::RequestExit(false);
	}
	if (!Upnp) return;
	FOutlawsUpnpState& U = *Upnp;
	for (int32 i = U.Closing.Num() - 1; i >= 0; --i)
	{
		const FJobPtr J = U.Closing[i];
		if (!J->bDone) continue;
		U.Closing.RemoveAt(i);
		if (J->Kind == FOutlawsUpnpJob::EKind::Close)
		{
			if (J->bOk) { UE_LOG(LogOutlawsCoop, Log, TEXT("OLUPNP mapping removed (UDP %d) in %d ms"), J->Port, Ms(J->Took)); }
			else { UE_LOG(LogOutlawsCoop, Warning, TEXT("OLUPNP removing the mapping failed: %s (the lease ends it)"), *J->Why); }
		}
		else if (J->bUndone) { UE_LOG(LogOutlawsCoop, Log, TEXT("OLUPNP the mapping made as the session ended was removed (UDP %d)"), J->Port); }
	}
	if (U.Job.IsValid() && U.Job->bDone)
	{
		const FJobPtr J = U.Job;
		U.Job.Reset();
		switch (J->Kind)
		{
		case FOutlawsUpnpJob::EKind::Probe:
			if (J->bFound)
			{
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLUPNP probe: router found in %d ms: %s at %s; this machine %s; external address %s%s"), Ms(J->Took), *J->Gateway.ServiceType,
					*J->Gateway.ControlUrl, *J->Gateway.LocalIp, J->External.IsEmpty() ? *(TEXT("not given: ") + J->ExternalWhy) : *J->External,
					J->External.IsEmpty() ? TEXT("") : OutlawsUpnp::IsSharedAddress(J->External) ? TEXT(" (a shared address: port forwarding can't work here)") : TEXT(" (a public address)"));
			}
			else
			{
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLUPNP probe: %s, in %d ms; the lobby would say \"Internet: couldn't open the port automatically (see the guide)\""), *J->Why, Ms(J->Took));
			}
			if (GQuitWhenDone) GQuitPending = true;
			break;
		case FOutlawsUpnpJob::EKind::Open:
			UpnpExternal = J->External;
			if (J->bMapped)
			{
				U.Gateway = J->Gateway;
				U.MappedPort = J->Port;
				U.RenewAt = Now + FMath::Max(5.0, U.Lease > 0 ? U.Lease * 0.5 : 1800.0);
				UpnpState = EUpnp::Opened;
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLUPNP port opened: UDP %d to %s by %s, external address %s, lease %d s, in %d ms"), J->Port, *J->Gateway.LocalIp,
					*J->Gateway.ControlUrl, J->External.IsEmpty() ? TEXT("(not given)") : *J->External, U.Lease, Ms(J->Took));
			}
			else
			{
				UpnpState = EUpnp::Refused;
				UpnpWhy = J->Why;
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLUPNP port not opened: %s (in %d ms)"), *J->Why, Ms(J->Took));
			}
			if (UpnpShared()) UE_LOG(LogOutlawsCoop, Log, TEXT("OLUPNP shared address: the router's external address %s cannot be reached from the internet"), *UpnpExternal);
			++Version;
			break;
		case FOutlawsUpnpJob::EKind::Renew:
			if (J->bMapped)
			{
				U.RenewAt = Now + FMath::Max(5.0, U.Lease > 0 ? U.Lease * 0.5 : 1800.0);
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLUPNP lease renewed (UDP %d, %d s) in %d ms"), J->Port, U.Lease, Ms(J->Took));
			}
			else
			{
				U.RenewAt = Now + 60.0;
				UE_LOG(LogOutlawsCoop, Warning, TEXT("OLUPNP renewing the lease failed: %s (again in a minute)"), *J->Why);
			}
			break;
		case FOutlawsUpnpJob::EKind::Close:
			break;
		}
	}
	if (U.MappedPort > 0 && !U.Job.IsValid() && UpnpState == EUpnp::Opened && Now >= U.RenewAt)
	{
		U.Job = Launch(FOutlawsUpnpJob::EKind::Renew, U.Gateway, U.MappedPort, U.Lease);
	}
	// -OLUpnpTest=leave|quit: once the router has answered, host a while, then Leave (the mapping removed on a worker, then
	// quit) or quit at once (removed as the game goes)
	if (GTestMode.IsEmpty()) return;
	if (GTestPhase == 0 && (UpnpState == EUpnp::Opened || UpnpState == EUpnp::Refused))
	{
		GTestPhase = 1;
		GTestAt = Now + GTestAfter;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLUPNP test: the router answered (%s); %s in %.0f s"), UpnpState == EUpnp::Opened ? TEXT("opened") : TEXT("refused"), *GTestMode, GTestAfter);
	}
	else if (GTestPhase == 1 && Now >= GTestAt)
	{
		if (GTestMode == TEXT("leave"))
		{
			GTestPhase = 2;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLUPNP test: Leave"));
			EndSession(TEXT("the UPnP test's Leave"), true);
		}
		else
		{
			GTestPhase = 3;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLUPNP test: quit"));
			FPlatformMisc::RequestExit(false);
		}
	}
	else if (GTestPhase == 2 && U.Closing.Num() == 0 && !U.Job.IsValid())
	{
		GTestPhase = 3;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLUPNP test: left, nothing left with the router; quitting"));
		FPlatformMisc::RequestExit(false);
	}
}

bool UOutlawsCoop::UpnpShared() const
{
	if (UpnpExternal.IsEmpty()) return false;
	if (OutlawsUpnp::IsSharedAddress(UpnpExternal)) return true;
	// (double NAT: the router's own address is public but is not the one the internet sees; only known once the host pressed
	// Show internet address, the one time ipify is asked)
	return bShowInternet && LooksLikeV4(InternetAddress) && InternetAddress != UpnpExternal;
}

FString UOutlawsCoop::ShownInternetAddress() const
{
	if (!UpnpExternal.IsEmpty() && !UpnpShared()) return UpnpExternal;
	return InternetAddress;
}

void UOutlawsCoop::UpnpLines(TArray<FString>& Out) const
{
	switch (UpnpState)
	{
	case EUpnp::Idle:
	case EUpnp::Skipped:
		return;
	case EUpnp::Off:
		Out.Add(TEXT("Internet: automatic port opening is off (Options > Multiplayer)"));
		return;
	case EUpnp::Asking:
		Out.Add(FString::Printf(TEXT("Internet: asking the router to open port %d ..."), ListenPort));
		return;
	default:
		break;
	}
	if (UpnpShared())
	{
		Out.Add(TEXT("Internet: this connection's address is shared, so port forwarding can't work"));
		Out.Add(TEXT("(friends outside can join through a VPN app; see the guide)"));
		return;
	}
	if (UpnpState == EUpnp::Opened)
	{
		// (the address only once the host has pressed Show internet address, as before: a host on a stream keeps it private)
		const FString A = ShownInternetAddress();
		if (bShowInternet && LooksLikeV4(A)) Out.Add(FString::Printf(TEXT("Internet: port opened automatically (address %s:%d)"), *A, ListenPort));
		else Out.Add(TEXT("Internet: port opened automatically"));
	}
	else Out.Add(TEXT("Internet: couldn't open the port automatically (see the guide)"));
}
