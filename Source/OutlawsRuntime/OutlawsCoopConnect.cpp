// Story co-op (tools/olue/COOP_DESIGN.md), R8 (user 2026-10-03: internet readiness): what a player sees while joining and
// once in: the join page's Recent addresses (with the host's name when it was learned) and its failures said plainly, the
// host's internet address in its lobby (asked of a what-is-my-ip service, host only).  (The pause menu's Players page, with
// the pings, both builds and a mute row per player, is in OutlawsCoopMenuFit.cpp since REQ-MPLOBBY c.)
//
// None of this runs in single player: the join page and the lobby are co-op pages, the pause menu's Co-op row is only there
// in a session.
#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsBuildInfo.h"
#include "OutlawsFlags.h"
#include "OutlawsLab.h"
#include "OutlawsRemotePlayer.h"
#include "Engine/World.h"
#include "Engine/NetDriver.h"
#include "Engine/NetConnection.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Kismet/GameplayStatics.h"

namespace
{
	constexpr int32 RecentMax = 5;

	FString RecentFile()
	{
		return FPaths::GetPath(AOutlawsLevelActor::SavePath(-1)) / TEXT("coop_recent.txt");
	}

	bool LooksLikeAddress(const FString& S)
	{
		if (S.IsEmpty() || S.Len() > 45) return false;
		for (const TCHAR C : S) if (!(FChar::IsDigit(C) || C == '.' || C == ':' || (C >= 'a' && C <= 'f') || (C >= 'A' && C <= 'F'))) return false;
		return true;
	}
}

// ---- the link's message ---------------------------------------------------------------------------------------------------

void AOutlawsCoopLink::ServerBuildLine_Implementation(const FString& Line)
{
	if (!Admit(EOLGate::BuildLine, Line.Len() <= 200)) return;
	BuildLine = Line.Left(80);
}

// ---- the Recent list (the join page) ----------------------------------------------------------------------------------------

void UOutlawsCoop::ReadRecent()
{
	if (bRecentRead) return;
	bRecentRead = true;
	RecentAddrs.Reset();
	RecentNames.Reset();
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *RecentFile())) return;
	for (const FString& Ln : Lines)
	{
		FString A, N;
		if (!Ln.Split(TEXT("|"), &A, &N)) A = Ln;
		A.TrimStartAndEndInline();
		if (A.IsEmpty() || RecentAddrs.Contains(A) || RecentAddrs.Num() >= RecentMax) continue;
		RecentAddrs.Add(A);
		RecentNames.Add(N.TrimStartAndEnd().Left(16));
	}
}

void UOutlawsCoop::NoteRecent(const FString& Address, const FString& Name)
{
	if (Address.IsEmpty()) return;
	ReadRecent();
	const int32 At = RecentAddrs.IndexOfByKey(Address);
	FString Keep = Name;
	if (At != INDEX_NONE)
	{
		if (Keep.IsEmpty()) Keep = RecentNames[At];
		RecentAddrs.RemoveAt(At);
		RecentNames.RemoveAt(At);
	}
	RecentAddrs.Insert(Address, 0);
	RecentNames.Insert(Keep, 0);
	while (RecentAddrs.Num() > RecentMax)
	{
		RecentAddrs.Pop();
		RecentNames.Pop();
	}
	TArray<FString> Lines;
	for (int32 i = 0; i < RecentAddrs.Num(); ++i) Lines.Add(RecentAddrs[i] + TEXT("|") + RecentNames[i]);
	FFileHelper::SaveStringArrayToFile(Lines, *RecentFile());
	++Version;
}

FString UOutlawsCoop::RecentLabel(int32 i) const
{
	if (!RecentAddrs.IsValidIndex(i)) return FString();
	const FString& N = RecentNames.IsValidIndex(i) ? RecentNames[i] : FString();
	return N.IsEmpty() ? RecentAddrs[i] : FString::Printf(TEXT("%s  %s"), *RecentAddrs[i], *N);
}

bool UOutlawsCoop::CloseRecentList(AOutlawsPlayer* P)
{
	UOutlawsCoop* C = Get(P);
	if (!C || !P || !C->bRecentOpen) return false;
	C->bRecentOpen = false;
	P->CoopShowPage(TEXT("#coopjoin"), false);
	return true;
}

// ---- M1: the optional game password -----------------------------------------------------------------------------------------

FString UOutlawsCoop::CleanPassword(const FString& In)
{
	FString Out;
	for (const TCHAR Ch : In)
	{
		if (FChar::IsAlnum(Ch) || Ch == '.' || Ch == '-' || Ch == '_') Out.AppendChar(Ch);
		if (Out.Len() >= 16) break;
	}
	return Out;
}

FString UOutlawsCoop::CheckJoinPassword(const FString& Options) const
{
	if (HostPassword.IsEmpty()) return FString();
	const FString Given = UGameplayStatics::ParseOption(Options, TEXT("Password"));
	if (Given.IsEmpty()) return TEXT("This game needs a password");
	// (the same length and letters; compared in full, whatever the first difference)
	bool bSame = Given.Len() == HostPassword.Len();
	for (int32 i = 0; i < FMath::Min(Given.Len(), HostPassword.Len()); ++i) bSame &= Given[i] == HostPassword[i];
	return bSame ? FString() : FString(TEXT("Wrong password"));
}

// ---- REQ-NATIVEMAPS (user 3 Nov): the join guard ------------------------------------------------------------------------------
// A joiner without the host's native map (an older build, -OLNoNativeMaps, a broken or other copy) used to get an empty level:
// nothing caught "ERROR: <level>.lvt not found", and the build compare was only the LAN beacon's.  Now the joiner's URL carries
// its build and its native maps (each LAB's stamp), the host's PreLogin turns it away with the reason, and a joiner told to
// build a level it cannot (a later match's map) goes to its main menu saying so, never into an empty level.  The build is
// sent and logged, never refused (the user 3 Nov: "your PC and Quest can always play together", they are often on different
// nightlies): only a missing map or another copy of it turns a joiner away.
// -OLOldJoinGuard: none of it (the password and the player count only).

namespace
{
	TSharedPtr<FOutlawsAssets> GuardAssets(const AOutlawsLevelActor* L)
	{
		const FString Root = FOutlawsAssets::FindInstallRoot(L ? L->InstallPath : FString());
		return Root.IsEmpty() ? nullptr : FOutlawsAssets::GetShared(Root);
	}

	/** The stamp a "level.stamp~..." list gives Level (empty: not in it). */
	FString StampIn(const FString& List, const FString& Level)
	{
		TArray<FString> Items;
		List.ParseIntoArray(Items, TEXT("~"));
		const FString Want = Level.ToLower() + TEXT(".");
		for (const FString& It : Items) if (It.StartsWith(Want)) return It.Mid(Want.Len());
		return FString();
	}
}

bool UOutlawsCoop::OldJoinGuard()
{
	static const bool bOld = OLFlags::Param(TEXT("OLOldJoinGuard"));
	return bOld;
}

FString UOutlawsCoop::NativeStampsHere()
{
	const TSharedPtr<FOutlawsAssets> A = OldJoinGuard() ? nullptr : GuardAssets(nullptr);
	return A ? A->NativeStampList() : FString();
}

bool UOutlawsCoop::OldNetVersion()
{
	static const bool bOld = OLFlags::Param(TEXT("OLOldNetVersion"));
	return bOld;
}

int32 UOutlawsCoop::NetVersionHere()
{
	int32 N = OutlawsBuildInfo::NetVersion;
#if WITH_OUTLAWS_TESTS
	OLFlags::Value(TEXT("OLNetVersionFake"), N);              // (tests: a machine on another number)
#endif
	return N;
}

const TCHAR* UOutlawsCoop::NetVersionRefusal()
{
	return TEXT("This host is on a different version. Please update.");
}

#if WITH_OUTLAWS_TESTS
int32 UOutlawsCoop::NetVersionTurnedAway = 0;
#endif

FString UOutlawsCoop::CheckJoinNetVersion(const FString& Options)
{
	if (OldNetVersion()) return FString();
	// (REQ-NETVERSION: before this number 16 of 16 mixed-build joins fell at the door, NetChecksumMismatch on the session 0.09 s
	// after the lobby; none on the URL is a joiner from before it, 0.95 and the nightlies since)
	const int32 Theirs = UGameplayStatics::GetIntOption(Options, TEXT("OLNetVersion"), 0);
	if (Theirs == NetVersionHere()) return FString();
#if WITH_OUTLAWS_TESTS
	++NetVersionTurnedAway;
#endif
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP net version: joiner on %d, this host on %d, build %s: refused"), Theirs, NetVersionHere(), OutlawsBuildInfo::Commit());
	return NetVersionRefusal();
}

FString UOutlawsCoop::JoinGuardOptions()
{
	const FString Net = OldNetVersion() ? FString() : FString::Printf(TEXT("?OLNetVersion=%d"), NetVersionHere());
	if (OldJoinGuard()) return Net;
	FString Build = OutlawsBuildInfo::Commit();
#if WITH_OUTLAWS_TESTS
	OLFlags::Value(TEXT("OLJoinGuardFakeBuild"), Build);      // (tests: a joiner on another build)
#endif
	return Net + FString::Printf(TEXT("?OLBuild=%s?OLMaps=%s"), *Build, *NativeStampsHere());
}

FString UOutlawsCoop::CheckJoinBuildAndMap(const FString& Options) const
{
	if (OldJoinGuard()) return FString();
	// (another build on the same net version is let in, the user 3 Nov: said once a join, never refused)
	const FString Build = UGameplayStatics::ParseOption(Options, TEXT("OLBuild"));
	if (Build != OutlawsBuildInfo::Commit())
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP join guard: joiner on build %s, host %s (let in: builds may differ)"),
			Build.IsEmpty() ? TEXT("(none on its URL: from before the guard)") : *Build, OutlawsBuildInfo::Commit());
	}
	// (the level the host's game is on, or starts with: a native map's is checked against the joiner's copy)
	const TSharedPtr<FOutlawsAssets> A = GuardAssets(FindLevel());
	const FString Mine = A ? A->NativeStampFor(LevelToStart) : FString();
	if (Mine.IsEmpty()) return FString();
	const FString Title = MatchOptions.MapFile.IsEmpty() ? LevelToStart : OutlawsMpHost::MapTitle(MatchOptions.MapFile);
	const FString Theirs = StampIn(UGameplayStatics::ParseOption(Options, TEXT("OLMaps")), LevelToStart);
	if (Theirs.IsEmpty())
	{
		return FString::Printf(TEXT("This game is on %s, a community map your Outlaws VR does not have"), *Title);
	}
	if (Theirs != Mine)
	{
		return FString::Printf(TEXT("This game is on %s, and your copy of that map is not the host's"), *Title);
	}
	return FString();
}

FString UOutlawsCoop::ClientCannotBuild(const FString& Level) const
{
	if (OldJoinGuard() || Level.IsEmpty()) return FString();
	const AOutlawsCoopSession* S = GetSession();
	const FString Title = S && !S->MapName.IsEmpty() && S->MatchClass != 0 ? OutlawsMpHost::MapTitle(S->MapName) : Level;
	const TSharedPtr<FOutlawsAssets> A = GuardAssets(FindLevel());
	if (!A) return FString();
	if (!A->HasLevel(Level)) return FString::Printf(TEXT("the host's map %s is not on this machine"), *Title);
	const FString Mine = A->NativeStampFor(Level);
	const FString Host = S ? StampIn(S->NativeStamps, Level) : FString();
	if (!Mine.IsEmpty() && !Host.IsEmpty() && Mine != Host) return FString::Printf(TEXT("this machine's copy of the host's map %s is not the host's"), *Title);
	return FString();
}

void UOutlawsCoop::LeaveCannotBuild(const FString& Why)
{
	UE_LOG(LogOutlawsCoop, Warning, TEXT("OLCOOP join guard: %s: this joiner goes to its main menu"), *Why);
#if WITH_OUTLAWS_TESTS
	if (bAuto)
	{
		Summary(Why);
		QuitForTest();
		return;
	}
#endif
	EndSession(Why, true);
	JoinError = FString::Printf(TEXT("Left: %s"), *Why);
}

bool UOutlawsCoop::PasswordTyped(AOutlawsPlayer* P, const FString& Purpose, const FString& Text)
{
	UOutlawsCoop* C = Get(P);
	if (!C || (Purpose != TEXT("coop_password") && Purpose != TEXT("join_password"))) return false;
	const FString Pw = CleanPassword(Text);
	if (Purpose == TEXT("coop_password"))
	{
		C->HostPassword = Pw;
		C->RefreshLobby();
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby: password %s"), Pw.IsEmpty() ? TEXT("cleared (anyone may join)") : TEXT("set"));
	}
	else
	{
		C->JoinPassword = Pw;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP join page: password %s"), Pw.IsEmpty() ? TEXT("cleared") : TEXT("typed"));
	}
	++C->Version;
	if (Purpose == TEXT("coop_password")) P->CoopMenuBack();      // (REQ-COOPLOBBY: back to the lobby's Options page)
	else P->CoopShowPage(TEXT("#coopjoin"), false);
	return true;
}

namespace
{
	FString RecentPasswordFile()
	{
		return FPaths::GetPath(AOutlawsLevelActor::SavePath(-1)) / TEXT("coop_passwords.txt");
	}
}

void UOutlawsCoop::ReadRecentPasswords()
{
	RecentPasswords.Reset();
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *RecentPasswordFile())) return;
	for (const FString& Ln : Lines)
	{
		FString A, Pw;
		if (Ln.Split(TEXT("|"), &A, &Pw) && !A.IsEmpty()) RecentPasswords.Add(A.TrimStartAndEnd(), CleanPassword(Pw));
	}
}

void UOutlawsCoop::NoteRecentPassword(const FString& Address, const FString& Password)
{
	ReadRecentPasswords();
	RecentPasswords.Add(Address, CleanPassword(Password));
	TArray<FString> Lines;
	for (const TPair<FString, FString>& Kv : RecentPasswords) if (RecentAddrs.Contains(Kv.Key) || Kv.Key == Address) Lines.Add(Kv.Key + TEXT("|") + Kv.Value);
	FFileHelper::SaveStringArrayToFile(Lines, *RecentPasswordFile());
}

// ---- the host's internet address ----------------------------------------------------------------------------------------------

void UOutlawsCoop::AskInternetAddress()
{
	// (asked once per hosting; a plain https GET answered with the address itself; 5 s, else "unknown")
	if (bInternetAsked || !IsHost()) return;
	bInternetAsked = true;
	InternetAddress = TEXT("asking ...");
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP asking api.ipify.org for this host's internet address"));
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Rq = FHttpModule::Get().CreateRequest();
	Rq->SetURL(TEXT("https://api.ipify.org"));
	Rq->SetVerb(TEXT("GET"));
	Rq->SetTimeout(5.f);
	TWeakObjectPtr<UOutlawsCoop> WeakThis(this);
	Rq->OnProcessRequestComplete().BindLambda([WeakThis](FHttpRequestPtr, FHttpResponsePtr Rs, bool bOk)
	{
		UOutlawsCoop* C = WeakThis.Get();
		if (!C) return;
		const FString Body = bOk && Rs.IsValid() && Rs->GetResponseCode() == 200 ? Rs->GetContentAsString().TrimStartAndEnd() : FString();
		C->InternetAddress = LooksLikeAddress(Body) ? Body : FString(TEXT("unknown"));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP this host's internet address: %s"), *C->InternetAddress);
		++C->Version;
	});
	Rq->ProcessRequest();
}

void UOutlawsCoop::SetShowInternet(bool bOn)
{
	// (the lobby row: on asks now and is remembered on this machine; off hides the line and forgets it; a test writes nothing)
	bShowInternet = bOn;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lobby: show internet address %s"), bOn ? TEXT("on") : TEXT("off"));
	if (!IsTestRun())
	{
		TMap<FString, FString> Set = AOutlawsLevelActor::ReadSettings();
		Set.Add(TEXT("CoopShowInternetAddress"), bOn ? TEXT("1") : TEXT("0"));
		AOutlawsLevelActor::WriteSettings(Set);
	}
	if (bOn) AskInternetAddress();
	++Version;
}

// ---- the pause menu's Co-op page ----------------------------------------------------------------------------------------------

void UOutlawsCoop::TickBuildLine()
{
	if (bBuildSent) return;
	if (AOutlawsCoopLink* Mine = GetLink(MyIndex()))
	{
		bBuildSent = true;
		Mine->ServerBuildLine(OutlawsBuildInfo::Line());       // (the host's own: set here at once)
	}
}

float UOutlawsCoop::PingMs() const
{
	// the round trip as the engine's connection measures it (its average over the last moments)
	const UWorld* W = CurrentWorld();
	const UNetDriver* D = W ? W->GetNetDriver() : nullptr;
	if (!D) return -1.f;
	if (IsClient()) return D->ServerConnection ? D->ServerConnection->AvgLag * 1000.f : -1.f;
	const AOutlawsCoopLink* CL = GetLink(PartnerIndex());
	const APlayerController* PC = CL ? Cast<APlayerController>(CL->GetOwner()) : nullptr;
	return PC && PC->NetConnection ? PC->NetConnection->AvgLag * 1000.f : -1.f;
}

bool UOutlawsCoop::PartnerForMap(const UObject* Context, FVector& OutFeet, FString& OutName, int32 Slot)
{
	// N1: Slot counts the other players in player order (0: the partner of two, the host on a client's map)
	const UOutlawsCoop* C = Get(Context);
	if (!C || C->Mode == EOutlawsCoopMode::None || !C->bLevelBuilt || Slot < 0) return false;
	int32 Who = -1;
	for (int32 i = 0, n = 0; i < MaxPlayers; ++i)
	{
		if (i == C->MyIndex() || !C->Puppets[i].IsValid()) continue;
		if (n++ == Slot)
		{
			Who = i;
			break;
		}
	}
	const AOutlawsRemotePlayer* R = C->PuppetOf(Who);
	if (!R || R->IsHidden()) return false;
	OutFeet = R->ShownBody() - FVector(0.0, 0.0, R->ShownHalf());
	const AOutlawsCoopLink* OL = C->GetLink(Who);
	OutName = OL ? OL->PlayerName : FString();
	return true;
}
