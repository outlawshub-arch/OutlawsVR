// REQ-UPNP (v1.1): a small UPnP IGD client of our own, so a host's router can forward the game's UDP port by itself.
// UE 5.1 has none (its NAT traversal is the online services'), and the job is small: SSDP discovery (an M-SEARCH on the
// home network), the router's device description over HTTP, then SOAP calls on its WANIPConnection or WANPPPConnection
// service (GetExternalIPAddress, AddPortMapping, DeletePortMapping).  Plain sockets, no library.
//
// Every call here blocks and takes a deadline (FPlatformTime::Seconds()); run them off the game thread
// (OutlawsCoopUpnp.cpp does, with a 3 s limit).  The parsing helpers are pure, for the tests.
#pragma once

#include "CoreMinimal.h"

namespace OutlawsUpnp
{
	/** The router's port-mapping service, as discovery found it. */
	struct FGateway
	{
		FString Location;        // the device description's URL (the SSDP answer's LOCATION)
		FString ControlUrl;      // the WAN*Connection service's control URL, absolute
		FString ServiceType;     // urn:schemas-upnp-org:service:WANIPConnection:1 (or :2, or WANPPPConnection:1)
		FString LocalIp;         // this machine's address on the way to the router (the mapping's internal client)
		bool IsValid() const { return !ControlUrl.IsEmpty() && !ServiceType.IsEmpty(); }
	};

	/** -OLUpnpSsdp=<ip:port>: the M-SEARCH goes there (unicast) instead of the multicast group; a test's mock router. */
	FString SsdpTarget();

	/** Finds the router: M-SEARCH, then the first answer whose description has a WAN*Connection service.  Why says what failed. */
	bool Find(double Deadline, FGateway& Out, FString& Why);
	bool ExternalIp(const FGateway& G, double Deadline, FString& Ip, FString& Why);
	/** Maps Port (UDP, outside and in) to G.LocalIp for LeaseSec (a router that keeps only permanent mappings gets 0). */
	bool AddUdp(const FGateway& G, int32 Port, int32 LeaseSec, const FString& Description, double Deadline, FString& Why);
	bool DeleteUdp(const FGateway& G, int32 Port, double Deadline, FString& Why);

	/** A mapping is only ever made or removed on a real router in a run a person started: an -unattended run (every automated
	 *  test) may change only a router on this machine (the mock, 127.x).  Discovery and the external address are read-only. */
	bool MappingAllowed(const FGateway& G);

	/** True for an address port forwarding cannot reach: 10/8, 172.16/12, 192.168/16, 100.64/10 (carrier-grade NAT). */
	bool IsSharedAddress(const FString& Ip);

	// ---- the pure parts (tested by -OLUpnpSelfTest) ----
	FString HeaderValue(const FString& Response, const FString& Name);
	bool ParseUrl(const FString& Url, FString& Host, int32& Port, FString& Path);
	bool ParseDescription(const FString& Xml, const FString& Location, FString& ControlUrl, FString& ServiceType);
	FString SoapEnvelope(const FString& Service, const FString& Action, const TArray<TPair<FString, FString>>& Args);
	FString XmlValue(const FString& Xml, const FString& Tag);
	FString Dechunk(const FString& Body);
#if WITH_OUTLAWS_TESTS
	/** Runs the pure parts' checks; logs "OLUPNP selftest PASS|FAIL ..." per check; returns the failures. */
	int32 SelfTest();
#endif // WITH_OUTLAWS_TESTS
}
