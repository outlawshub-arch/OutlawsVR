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
