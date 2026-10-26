// REQ-UPNP (v1.1): the UPnP IGD client (see OutlawsUpnp.h).  The protocol's parts, in the order they run:
//   1. SSDP: "M-SEARCH * HTTP/1.1" to 239.255.255.250:1900 over UDP, for the gateway device and its WAN connection services;
//      each router that speaks UPnP answers with a LOCATION header, its device description's URL.
//   2. The description (an HTTP GET): the XML lists the device's services; the one we want is WANIPConnection (or
//      WANPPPConnection on a DSL modem that dials PPPoE), with its controlURL.
//   3. SOAP (HTTP POSTs to that control URL): GetExternalIPAddress, AddPortMapping, DeletePortMapping.
// Everything blocks with a deadline; OutlawsCoopUpnp.cpp runs it on a worker thread.
#include "OutlawsUpnp.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "SocketSubsystem.h"
#include "Sockets.h"
#include "IPAddress.h"
#include "AddressInfoTypes.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"

namespace OutlawsUpnp
{
namespace
{
	ISocketSubsystem* Sockets() { return ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM); }

	double Left(double Deadline) { return Deadline - FPlatformTime::Seconds(); }

	FString BytesToString(const uint8* Data, int32 Num)
	{
		if (Num <= 0) return FString();
		const FUTF8ToTCHAR Conv(reinterpret_cast<const ANSICHAR*>(Data), Num);
		return FString(Conv.Length(), Conv.Get());
	}

	TArray<uint8> StringToBytes(const FString& S)
	{
		const FTCHARToUTF8 Conv(*S);
		TArray<uint8> Out;
		Out.Append(reinterpret_cast<const uint8*>(Conv.Get()), Conv.Length());
		return Out;
	}

	/** A chunked HTTP body made plain (sizes in hex, each chunk's data, a 0-size chunk at the end). */
	TArray<uint8> DechunkBytes(const TArray<uint8>& In)
	{
		TArray<uint8> Out;
		int32 i = 0;
		while (i < In.Num())
		{
			int32 Size = 0, Digits = 0;
			while (i < In.Num() && FChar::IsHexDigit(TCHAR(In[i])))
			{
				Size = Size * 16 + FParse::HexDigit(TCHAR(In[i]));
				++i;
				++Digits;
			}
			while (i < In.Num() && In[i] != '\n') ++i;          // (a chunk extension, then the line's end)
			++i;
			if (Digits == 0 || Size <= 0) break;
			const int32 Take = FMath::Min(Size, In.Num() - i);
			if (Take > 0) Out.Append(In.GetData() + i, Take);
			i += Size;
			while (i < In.Num() && In[i] != '\n') ++i;          // (the chunk's own line end)
			++i;
		}
		return Out;
	}

	TSharedPtr<FInternetAddr> Resolve(const FString& Host, int32 Port)
	{
		ISocketSubsystem* SS = Sockets();
		if (!SS) return nullptr;
		TSharedRef<FInternetAddr> A = SS->CreateInternetAddr();
		bool bValid = false;
		A->SetIp(*Host, bValid);
		if (!bValid)
		{
			// (a router names itself by address in practice; a name is looked up all the same)
			const FAddressInfoResult R = SS->GetAddressInfo(*Host, nullptr, EAddressInfoFlags::Default, FNetworkProtocolTypes::IPv4, SOCKTYPE_Streaming);
			if (R.ReturnCode != SE_NO_ERROR || R.Results.Num() == 0) return nullptr;
			TSharedPtr<FInternetAddr> Got = R.Results[0].Address->Clone();
			Got->SetPort(Port);
			return Got;
		}
		A->SetPort(Port);
		return A;
	}

	/** One HTTP/1.1 exchange ("Connection: close"), read to its end or the deadline. */
	bool Http(const FString& Url, const FString& Method, const TArray<FString>& Headers, const FString& Body, double Deadline,
		int32& Status, FString& RespBody, FString* LocalIp, FString& Why)
	{
		Status = 0;
		RespBody.Reset();
		FString Host, Path;
		int32 Port = 80;
		if (!ParseUrl(Url, Host, Port, Path)) { Why = FString::Printf(TEXT("a URL we cannot read (%s)"), *Url); return false; }
		ISocketSubsystem* SS = Sockets();
		const TSharedPtr<FInternetAddr> To = Resolve(Host, Port);
		if (!SS || !To.IsValid()) { Why = FString::Printf(TEXT("no address for %s"), *Host); return false; }
		FSocket* S = SS->CreateSocket(NAME_Stream, TEXT("OLUPNP http"), FNetworkProtocolTypes::IPv4);
		if (!S) { Why = TEXT("no socket"); return false; }
		ON_SCOPE_EXIT { S->Close(); SS->DestroySocket(S); };
		S->SetNonBlocking(true);
		S->Connect(*To);
		// (a non-blocking connect: wait until the socket is writable, or failed, or the time is up)
		for (;;)
		{
			const ESocketConnectionState St = S->GetConnectionState();
			if (St == SCS_Connected) break;
			if (St == SCS_ConnectionError) { Why = FString::Printf(TEXT("%s:%d refused the connection"), *Host, Port); return false; }
			if (Left(Deadline) <= 0.0) { Why = FString::Printf(TEXT("%s:%d did not answer in time"), *Host, Port); return false; }
			FPlatformProcess::Sleep(0.01f);
		}
		if (LocalIp)
		{
			TSharedRef<FInternetAddr> Me = SS->CreateInternetAddr();
			S->GetAddress(*Me);
			*LocalIp = Me->ToString(false);
		}
		const TArray<uint8> BodyBytes = StringToBytes(Body);
		FString Head = FString::Printf(TEXT("%s %s HTTP/1.1\r\nHost: %s:%d\r\nConnection: close\r\n"), *Method, *Path, *Host, Port);
		for (const FString& H : Headers) Head += H + TEXT("\r\n");
		if (Method == TEXT("POST")) Head += FString::Printf(TEXT("Content-Length: %d\r\n"), BodyBytes.Num());
		Head += TEXT("\r\n");
		TArray<uint8> Out = StringToBytes(Head);
		Out.Append(BodyBytes);
		int32 Sent = 0;
		while (Sent < Out.Num())
		{
			int32 N = 0;
			if (!S->Send(Out.GetData() + Sent, Out.Num() - Sent, N)) { Why = TEXT("the request could not be sent"); return false; }
			Sent += N;
			if (N == 0)
			{
				if (Left(Deadline) <= 0.0) { Why = TEXT("the request could not be sent in time"); return false; }
				S->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromMilliseconds(20));
			}
		}
		// the answer: headers, then the body by Content-Length, chunks, or until the router closes
		TArray<uint8> In;
		int32 HeadEnd = INDEX_NONE, Length = -1;
		bool bChunked = false;
		uint8 Buf[4096];
		for (;;)
		{
			int32 N = 0;
			const bool bOk = S->Recv(Buf, sizeof(Buf), N);
			if (N > 0) In.Append(Buf, N);
			if (HeadEnd == INDEX_NONE)
			{
				for (int32 i = 0; i + 3 < In.Num(); ++i)
				{
					if (In[i] == '\r' && In[i + 1] == '\n' && In[i + 2] == '\r' && In[i + 3] == '\n') { HeadEnd = i + 4; break; }
				}
				if (HeadEnd != INDEX_NONE)
				{
					const FString H = BytesToString(In.GetData(), HeadEnd);
					const FString Cl = HeaderValue(H, TEXT("Content-Length"));
					if (!Cl.IsEmpty()) Length = FCString::Atoi(*Cl);
					bChunked = HeaderValue(H, TEXT("Transfer-Encoding")).Contains(TEXT("chunked"));
				}
			}
			if (HeadEnd != INDEX_NONE)
			{
				const int32 Have = In.Num() - HeadEnd;
				if (!bChunked && Length >= 0 && Have >= Length) break;
				if (bChunked && Have >= 5 && FMemory::Memcmp(In.GetData() + In.Num() - 5, "0\r\n\r\n", 5) == 0) break;
			}
			if (!bOk) break;                                     // (closed by the router: the answer is all here)
			if (N == 0)
			{
				if (Left(Deadline) <= 0.0) { Why = TEXT("the router's answer did not come in time"); return false; }
				S->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(20));
			}
		}
		if (HeadEnd == INDEX_NONE) { Why = TEXT("no answer from the router"); return false; }
		const FString H = BytesToString(In.GetData(), HeadEnd);
		FString Line, Rest;
		H.Split(TEXT("\r\n"), &Line, &Rest);
		TArray<FString> Parts;
		Line.ParseIntoArrayWS(Parts);
		Status = Parts.Num() >= 2 ? FCString::Atoi(*Parts[1]) : 0;
		TArray<uint8> BodyIn;
		BodyIn.Append(In.GetData() + HeadEnd, In.Num() - HeadEnd);
		if (bChunked) BodyIn = DechunkBytes(BodyIn);
		RespBody = BytesToString(BodyIn.GetData(), BodyIn.Num());
		return true;
	}

	bool Soap(const FGateway& G, const FString& Action, const TArray<TPair<FString, FString>>& Args, double Deadline, FString& Resp, FString& Why, int32* ErrorCode = nullptr)
	{
		if (ErrorCode) *ErrorCode = 0;
		const TArray<FString> Headers = {
			TEXT("Content-Type: text/xml; charset=\"utf-8\""),
			FString::Printf(TEXT("SOAPAction: \"%s#%s\""), *G.ServiceType, *Action) };
		int32 Status = 0;
		if (!Http(G.ControlUrl, TEXT("POST"), Headers, SoapEnvelope(G.ServiceType, Action, Args), Deadline, Status, Resp, nullptr, Why)) return false;
		if (Status == 200) return true;
		const FString Code = XmlValue(Resp, TEXT("errorCode")), Desc = XmlValue(Resp, TEXT("errorDescription"));
		if (ErrorCode) *ErrorCode = FCString::Atoi(*Code);
		Why = FString::Printf(TEXT("the router said no to %s (HTTP %d%s%s)"), *Action, Status,
			Code.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(", UPnP error %s"), *Code), Desc.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" %s"), *Desc));
		return false;
	}

	bool ParseV4(const FString& Ip, uint8 (&O)[4])
	{
		TArray<FString> P;
		Ip.TrimStartAndEnd().ParseIntoArray(P, TEXT("."));
		if (P.Num() != 4) return false;
		for (int32 i = 0; i < 4; ++i)
		{
			if (P[i].IsEmpty() || !P[i].IsNumeric()) return false;
			const int32 V = FCString::Atoi(*P[i]);
			if (V < 0 || V > 255) return false;
			O[i] = uint8(V);
		}
		return true;
	}
}

FString SsdpTarget()
{
	FString T;
	OLFlags::Value(TEXT("OLUpnpSsdp"), T);
	return T;
}

bool MappingAllowed(const FGateway& G)
{
	if (!FParse::Param(FCommandLine::Get(), TEXT("unattended"))) return true;
	FString Host, Path;
	int32 Port = 0;
	return ParseUrl(G.ControlUrl, Host, Port, Path) && Host.StartsWith(TEXT("127."));
}

bool IsSharedAddress(const FString& Ip)
{
	uint8 O[4];
	if (!ParseV4(Ip, O)) return false;
	return O[0] == 10 || (O[0] == 172 && (O[1] & 0xF0) == 16) || (O[0] == 192 && O[1] == 168) || (O[0] == 100 && (O[1] & 0xC0) == 64);
}

bool Find(double Deadline, FGateway& Out, FString& Why)
{
	Out = FGateway();
	ISocketSubsystem* SS = Sockets();
	if (!SS) { Why = TEXT("no network"); return false; }
	FString Target = SsdpTarget();
	FString TargetHost = TEXT("239.255.255.250");
	int32 TargetPort = 1900;
	if (!Target.IsEmpty())
	{
		FString P;
		if (Target.Split(TEXT(":"), &TargetHost, &P)) TargetPort = FCString::Atoi(*P);
		else TargetHost = Target;
	}
	TSharedRef<FInternetAddr> To = SS->CreateInternetAddr();
	bool bValid = false;
	To->SetIp(*TargetHost, bValid);
	To->SetPort(TargetPort);
	if (!bValid) { Why = FString::Printf(TEXT("a discovery address we cannot read (%s)"), *TargetHost); return false; }
	FSocket* S = SS->CreateSocket(NAME_DGram, TEXT("OLUPNP ssdp"), FNetworkProtocolTypes::IPv4);
	if (!S) { Why = TEXT("no socket"); return false; }
	ON_SCOPE_EXIT { S->Close(); SS->DestroySocket(S); };
	S->SetNonBlocking(true);
	const TCHAR* Kinds[] = {
		TEXT("urn:schemas-upnp-org:device:InternetGatewayDevice:1"),
		TEXT("urn:schemas-upnp-org:device:InternetGatewayDevice:2"),
		TEXT("urn:schemas-upnp-org:service:WANIPConnection:1"),
		TEXT("urn:schemas-upnp-org:service:WANPPPConnection:1") };
	auto Search = [&]()
	{
		for (const TCHAR* St : Kinds)
		{
			const TArray<uint8> Msg = StringToBytes(FString::Printf(
				TEXT("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: 1\r\nST: %s\r\n\r\n"), St));
			int32 Sent = 0;
			S->SendTo(Msg.GetData(), Msg.Num(), Sent, *To);
		}
	};
	Search();
	const double Start = FPlatformTime::Seconds();
	// (the answers: listened for until the first one and a moment after it, with a second search at half a second; room is
	// kept for the description and the SOAP calls inside the deadline)
	const double ListenUntil = Deadline - 0.9;
	double FirstAt = 0.0;
	bool bResent = false;
	TArray<FString> Locations;
	TSharedRef<FInternetAddr> From = SS->CreateInternetAddr();
	uint8 Buf[2048];
	while (FPlatformTime::Seconds() < ListenUntil && (FirstAt == 0.0 || FPlatformTime::Seconds() < FirstAt + 0.2))
	{
		int32 N = 0;
		if (S->RecvFrom(Buf, sizeof(Buf), N, *From) && N > 0)
		{
			const FString Ans = BytesToString(Buf, N);
			const FString Loc = HeaderValue(Ans, TEXT("LOCATION"));
			if (!Loc.IsEmpty() && !Locations.Contains(Loc))
			{
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLUPNP discovery: %s answered (%s), its description at %s"), *From->ToString(false), *HeaderValue(Ans, TEXT("ST")), *Loc);
				Locations.Add(Loc);
				if (FirstAt == 0.0) FirstAt = FPlatformTime::Seconds();
			}
			continue;
		}
		if (!bResent && FPlatformTime::Seconds() > Start + 0.5)
		{
			bResent = true;
			Search();
		}
		S->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(30));
	}
	if (Locations.Num() == 0) { Why = TEXT("no router answered the UPnP search"); return false; }
	FString LastWhy;
	for (const FString& Loc : Locations)
	{
		int32 Status = 0;
		FString Xml, LocalIp, W;
		if (!Http(Loc, TEXT("GET"), {}, FString(), Deadline, Status, Xml, &LocalIp, W)) { LastWhy = W; continue; }
		if (Status != 200) { LastWhy = FString::Printf(TEXT("its description gave HTTP %d"), Status); continue; }
		FGateway G;
		G.Location = Loc;
		G.LocalIp = LocalIp;
		if (!ParseDescription(Xml, Loc, G.ControlUrl, G.ServiceType)) { LastWhy = TEXT("the device has no WAN connection service (not a router, or UPnP port mapping is off)"); continue; }
		Out = G;
		return true;
	}
	Why = LastWhy;
	return false;
}

bool ExternalIp(const FGateway& G, double Deadline, FString& Ip, FString& Why)
{
	FString Resp;
	if (!Soap(G, TEXT("GetExternalIPAddress"), {}, Deadline, Resp, Why)) return false;
	Ip = XmlValue(Resp, TEXT("NewExternalIPAddress"));
	if (Ip.IsEmpty()) { Why = TEXT("the router gave no external address"); return false; }
	return true;
}

bool AddUdp(const FGateway& G, int32 Port, int32 LeaseSec, const FString& Description, double Deadline, FString& Why)
{
	if (!MappingAllowed(G)) { Why = TEXT("an unattended run never maps a port on a real router"); return false; }
	auto Args = [&](int32 Lease)
	{
		return TArray<TPair<FString, FString>>{
			{ TEXT("NewRemoteHost"), FString() },
			{ TEXT("NewExternalPort"), FString::FromInt(Port) },
			{ TEXT("NewProtocol"), TEXT("UDP") },
			{ TEXT("NewInternalPort"), FString::FromInt(Port) },
			{ TEXT("NewInternalClient"), G.LocalIp },
			{ TEXT("NewEnabled"), TEXT("1") },
			{ TEXT("NewPortMappingDescription"), Description },
			{ TEXT("NewLeaseDuration"), FString::FromInt(Lease) } };
	};
	FString Resp;
	int32 Code = 0;
	if (Soap(G, TEXT("AddPortMapping"), Args(LeaseSec), Deadline, Resp, Why, &Code)) return true;
	if (Code == 725 && LeaseSec != 0)
	{
		// (OnlyPermanentLeasesSupported: such a router keeps it until we remove it, which Leave and quit do)
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLUPNP the router keeps only permanent mappings: asked again with no lease"));
		return Soap(G, TEXT("AddPortMapping"), Args(0), Deadline, Resp, Why, &Code);
	}
	return false;
}

bool DeleteUdp(const FGateway& G, int32 Port, double Deadline, FString& Why)
{
	if (!MappingAllowed(G)) { Why = TEXT("an unattended run never changes a real router"); return false; }
	FString Resp;
	return Soap(G, TEXT("DeletePortMapping"), {
		{ TEXT("NewRemoteHost"), FString() },
		{ TEXT("NewExternalPort"), FString::FromInt(Port) },
		{ TEXT("NewProtocol"), TEXT("UDP") } }, Deadline, Resp, Why);
}

// ---- the pure parts --------------------------------------------------------------------------------------------------------

FString HeaderValue(const FString& Response, const FString& Name)
{
	TArray<FString> Lines;
	Response.ParseIntoArrayLines(Lines);
	for (const FString& L : Lines)
	{
		FString K, V;
		if (L.Split(TEXT(":"), &K, &V) && K.TrimStartAndEnd().Equals(Name, ESearchCase::IgnoreCase)) return V.TrimStartAndEnd();
	}
	return FString();
}

bool ParseUrl(const FString& Url, FString& Host, int32& Port, FString& Path)
{
	FString Rest = Url.TrimStartAndEnd();
	if (!Rest.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase)) return false;
	Rest.RightChopInline(7);
	int32 Slash = INDEX_NONE;
	FString HostPort = Rest;
	Path = TEXT("/");
	if (Rest.FindChar('/', Slash))
	{
		HostPort = Rest.Left(Slash);
		Path = Rest.Mid(Slash);
	}
	FString P;
	Port = 80;
	if (HostPort.Split(TEXT(":"), &Host, &P)) Port = FCString::Atoi(*P);
	else Host = HostPort;
	return !Host.IsEmpty() && Port > 0 && Port < 65536;
}

FString XmlValue(const FString& Xml, const FString& Tag)
{
	// the text of the first <Tag> or <prefix:Tag> (no attributes; enough for the descriptions and the SOAP answers)
	const FString Key = Tag + TEXT(">");
	int32 From = 0;
	for (;;)
	{
		const int32 At = Xml.Find(Key, ESearchCase::IgnoreCase, ESearchDir::FromStart, From);
		if (At == INDEX_NONE) return FString();
		From = At + Key.Len();
		if (At == 0) continue;
		int32 Lt = At - 1;
		if (Xml[Lt] == ':')
		{
			// (over the prefix; a closing tag's '/' stops it short of the '<')
			--Lt;
			while (Lt >= 0 && (FChar::IsAlnum(Xml[Lt]) || Xml[Lt] == '_' || Xml[Lt] == '-')) --Lt;
		}
		if (Lt < 0 || Xml[Lt] != '<') continue;
		const int32 End = Xml.Find(TEXT("<"), ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
		return (End == INDEX_NONE ? Xml.Mid(From) : Xml.Mid(From, End - From)).TrimStartAndEnd();
	}
}

bool ParseDescription(const FString& Xml, const FString& Location, FString& ControlUrl, FString& ServiceType)
{
	ControlUrl.Reset();
	ServiceType.Reset();
	FString Ppp, PppType;
	int32 From = 0;
	for (;;)
	{
		const int32 At = Xml.Find(TEXT("<service>"), ESearchCase::IgnoreCase, ESearchDir::FromStart, From);
		if (At == INDEX_NONE) break;
		const int32 End = Xml.Find(TEXT("</service>"), ESearchCase::IgnoreCase, ESearchDir::FromStart, At);
		if (End == INDEX_NONE) break;
		const FString Block = Xml.Mid(At, End - At);
		From = End + 10;
		const FString Type = XmlValue(Block, TEXT("serviceType"));
		const FString Ctl = XmlValue(Block, TEXT("controlURL"));
		if (Ctl.IsEmpty()) continue;
		if (Type.Contains(TEXT(":service:WANIPConnection:")) && ControlUrl.IsEmpty()) { ControlUrl = Ctl; ServiceType = Type; }
		else if (Type.Contains(TEXT(":service:WANPPPConnection:")) && Ppp.IsEmpty()) { Ppp = Ctl; PppType = Type; }
	}
	if (ControlUrl.IsEmpty()) { ControlUrl = Ppp; ServiceType = PppType; }
	if (ControlUrl.IsEmpty()) return false;
	if (ControlUrl.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase)) return true;
	// relative: against URLBase when the description has one, else the description's own address
	FString Base = XmlValue(Xml, TEXT("URLBase"));
	if (Base.IsEmpty()) Base = Location;
	FString Host, Path;
	int32 Port = 80;
	if (!ParseUrl(Base, Host, Port, Path)) return false;
	const FString Root = FString::Printf(TEXT("http://%s:%d"), *Host, Port);
	if (ControlUrl.StartsWith(TEXT("/"))) ControlUrl = Root + ControlUrl;
	else
	{
		int32 Slash = INDEX_NONE;
		Path.FindLastChar('/', Slash);
		ControlUrl = Root + Path.Left(Slash + 1) + ControlUrl;
	}
	return true;
}

FString SoapEnvelope(const FString& Service, const FString& Action, const TArray<TPair<FString, FString>>& Args)
{
	FString A;
	for (const TPair<FString, FString>& Kv : Args) A += FString::Printf(TEXT("<%s>%s</%s>"), *Kv.Key, *Kv.Value, *Kv.Key);
	return FString::Printf(TEXT("<?xml version=\"1.0\"?>\r\n<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">")
		TEXT("<s:Body><u:%s xmlns:u=\"%s\">%s</u:%s></s:Body></s:Envelope>\r\n"), *Action, *Service, *A, *Action);
}

FString Dechunk(const FString& Body)
{
	const TArray<uint8> Out = DechunkBytes(StringToBytes(Body));
	return BytesToString(Out.GetData(), Out.Num());
}

#if WITH_OUTLAWS_TESTS
int32 SelfTest()
{
	int32 Fail = 0;
	auto Check = [&Fail](bool bOk, const FString& What)
	{
		Fail += bOk ? 0 : 1;
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLUPNP selftest %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
	};
	const FString Ssdp = TEXT("HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=120\r\nST: urn:schemas-upnp-org:device:InternetGatewayDevice:1\r\nlocation: http://192.168.0.1:5000/rootDesc.xml\r\n\r\n");
	Check(HeaderValue(Ssdp, TEXT("LOCATION")) == TEXT("http://192.168.0.1:5000/rootDesc.xml"), TEXT("the SSDP answer's LOCATION, any case"));
	FString H, P;
	int32 Port = 0;
	Check(ParseUrl(TEXT("http://192.168.0.1:5000/ctl/IPConn"), H, Port, P) && H == TEXT("192.168.0.1") && Port == 5000 && P == TEXT("/ctl/IPConn"), TEXT("a URL with a port"));
	Check(ParseUrl(TEXT("http://10.0.0.138"), H, Port, P) && Port == 80 && P == TEXT("/"), TEXT("a URL without a port or path"));
	const FString Desc = TEXT("<?xml version=\"1.0\"?><root><device><serviceList><service><serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType>")
		TEXT("<controlURL>/ctl/L3F</controlURL></service></serviceList><deviceList><device><deviceList><device><serviceList>")
		TEXT("<service><serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1</serviceType><controlURL>/ctl/PPP</controlURL></service>")
		TEXT("<service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType><serviceId>urn:upnp-org:serviceId:WANIPConn1</serviceId>")
		TEXT("<controlURL>/ctl/IPConn</controlURL></service></serviceList></device></deviceList></device></deviceList></device></root>");
	FString Ctl, Type;
	Check(ParseDescription(Desc, TEXT("http://192.168.0.1:5000/rootDesc.xml"), Ctl, Type) && Ctl == TEXT("http://192.168.0.1:5000/ctl/IPConn")
		&& Type == TEXT("urn:schemas-upnp-org:service:WANIPConnection:1"), FString::Printf(TEXT("the description's WANIPConnection over its PPP one (%s)"), *Ctl));
	const FString PppOnly = TEXT("<root><URLBase>http://10.1.1.1:49000/</URLBase><service><serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1</serviceType>")
		TEXT("<controlURL>upnp/control/WANPPPConn1</controlURL></service></root>");
	Check(ParseDescription(PppOnly, TEXT("http://10.1.1.1:49000/igddesc.xml"), Ctl, Type) && Ctl == TEXT("http://10.1.1.1:49000/upnp/control/WANPPPConn1")
		&& Type.Contains(TEXT("WANPPPConnection")), FString::Printf(TEXT("a PPP-only modem with URLBase and a relative control URL (%s)"), *Ctl));
	Check(!ParseDescription(TEXT("<root><service><serviceType>urn:schemas-upnp-org:service:ContentDirectory:1</serviceType><controlURL>/cd</controlURL></service></root>"),
		TEXT("http://192.168.0.5/d.xml"), Ctl, Type), TEXT("a media server is not a router"));
	const FString Env = SoapEnvelope(TEXT("urn:schemas-upnp-org:service:WANIPConnection:1"), TEXT("DeletePortMapping"), { { TEXT("NewExternalPort"), TEXT("7777") }, { TEXT("NewProtocol"), TEXT("UDP") } });
	Check(Env.Contains(TEXT("<u:DeletePortMapping xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\"><NewExternalPort>7777</NewExternalPort><NewProtocol>UDP</NewProtocol></u:DeletePortMapping>")),
		TEXT("the SOAP envelope"));
	const FString Ans = TEXT("<s:Envelope><s:Body><u:GetExternalIPAddressResponse xmlns:u=\"urn:schemas-upnp-org:service:WANIPConnection:1\"><NewExternalIPAddress>203.0.113.7</NewExternalIPAddress></u:GetExternalIPAddressResponse></s:Body></s:Envelope>");
	Check(XmlValue(Ans, TEXT("NewExternalIPAddress")) == TEXT("203.0.113.7"), TEXT("the external address out of the SOAP answer"));
	const FString Err = TEXT("<s:Fault><detail><UPnPError xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>718</errorCode><errorDescription>ConflictInMappingEntry</errorDescription></UPnPError></detail></s:Fault>");
	Check(XmlValue(Err, TEXT("errorCode")) == TEXT("718") && XmlValue(Err, TEXT("errorDescription")) == TEXT("ConflictInMappingEntry"), TEXT("a SOAP fault's error code"));
	Check(Dechunk(TEXT("5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n")) == TEXT("hello world"), TEXT("a chunked body"));
	Check(IsSharedAddress(TEXT("100.72.1.9")) && IsSharedAddress(TEXT("10.0.0.1")) && IsSharedAddress(TEXT("172.20.3.4")) && IsSharedAddress(TEXT("192.168.1.1"))
		&& !IsSharedAddress(TEXT("172.32.0.1")) && !IsSharedAddress(TEXT("100.128.0.1")) && !IsSharedAddress(TEXT("203.0.113.7")) && !IsSharedAddress(TEXT("")),
		TEXT("shared addresses: 10/8, 172.16/12, 192.168/16, 100.64/10, and not others"));
	FGateway Real;
	Real.ControlUrl = TEXT("http://192.168.0.1:5000/ctl/IPConn");
	FGateway Mock;
	Mock.ControlUrl = TEXT("http://127.0.0.1:19002/ctl/IPConn");
	const bool bUnattended = FParse::Param(FCommandLine::Get(), TEXT("unattended"));
	Check(MappingAllowed(Mock) && MappingAllowed(Real) == !bUnattended, FString::Printf(TEXT("mappings on a real router only in a run a person started (this run %s)"), bUnattended ? TEXT("unattended") : TEXT("by hand")));
	UE_LOG(LogOutlawsCoop, Display, TEXT("OLUPNP selftest done: %d failed"), Fail);
	return Fail;
}
#endif // WITH_OUTLAWS_TESTS
}
