// M2 over the session (tools/olue/PLANS.md "M2. Modes and scoring"): the host runs FOutlawsMatchModes (OutlawsMatchModes.h)
// beside M1's Death Match rules: who carries what is decided on the host from the poses it already has (no new message a
// client could forge), and the items, the team scores and IT ride on the session for every machine to draw and to put in its
// frag table.  Teams: each player's own on its link (the plates over teammates' heads, the table's colours); the pause menu
// changes it.
#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsLevel.h"
#include "OutlawsPlayer.h"
#include "OutlawsRemotePlayer.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsFragTable.h"
#include "OutlawsTextures.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"

namespace
{
	FString HeldClock(int32 Seconds)
	{
		return FString::Printf(TEXT("%d:%02d"), FMath::Max(0, Seconds) / 60, FMath::Max(0, Seconds) % 60);
	}
	constexpr int32 ItMarkerKey = 100;
}

// ---- the link -----------------------------------------------------------------------------------------------------------------

void AOutlawsCoopLink::ServerMatchTeam_Implementation(uint8 InTeam)
{
	if (!Admit(EOLGate::Team, InTeam == 1 || InTeam == 2)) return;
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnMatchTeam(PlayerIndex, InTeam);
}

// ---- what the rest of the game asks ---------------------------------------------------------------------------------------------

bool UOutlawsCoop::IsTeamMatch(const UObject* Context)
{
	const UOutlawsCoop* C = Get(Context);
	const AOutlawsCoopSession* S = C && C->IsMatch() ? C->GetSession() : nullptr;
	return S && S->bTeams;
}

int32 UOutlawsCoop::MyTeam(const UObject* Context)
{
	const UOutlawsCoop* C = Get(Context);
	const AOutlawsCoopLink* K = C ? C->GetLink(C->MyIndex()) : nullptr;
	return K ? K->Team : 0;
}

bool UOutlawsCoop::PlateHiddenFor(const UObject* Context, int32 Who)
{
	// (2026-10-03 decision) nameplates off in a free-for-all, teammates' only in a team class
	const UOutlawsCoop* C = Get(Context);
	if (!C || !C->IsMatch()) return false;
	if (!IsTeamMatch(Context)) return true;
	const AOutlawsCoopLink* K = C->GetLink(Who);
	return !K || K->Team == 0 || K->Team != MyTeam(Context);
}

// ---- the host -------------------------------------------------------------------------------------------------------------------

void UOutlawsCoop::SetUpModes(AOutlawsLevelActor* L)
{
	const FOutlawsLevel* Lv = L ? L->LiveLevel() : nullptr;
	AOutlawsCoopSession* S = GetSession();
	if (!Lv || !S) return;
	Modes = FOutlawsMatchModes();
	Modes.Class = MatchOptions.GameClass;
	Modes.bTeamDocument = MatchOptions.bTeams;
	Modes.bFriendlyFire = MatchOptions.bFriendlyFire;
	Modes.ScoreLimit = MatchOptions.ScoreLimit;
	// (guess) the timed classes read the Kills/Captures row as minutes held; a test gives seconds
	Modes.SecondsLimit = LaunchScoreSeconds > 0.f ? LaunchScoreSeconds : MatchOptions.ScoreLimit * 60.f;
	TWeakObjectPtr<UOutlawsCoop> Weak(this);
	Modes.OnSay = [Weak](const FString& Text, float Seconds) { if (UOutlawsCoop* C = Weak.Get()) C->SayToAll(Text, Seconds); };
	Modes.OnSayTo = [Weak](int32 Who, const FString& Text, float Seconds)
	{
		UOutlawsCoop* C = Weak.Get();
		if (!C) return;
		if (Who == 0) C->OnMatchMessage(Text, Seconds);
		else if (AOutlawsCoopLink* K = C->GetLink(Who)) K->ClientMatchMessage(Text, Seconds);
	};
	Modes.NameOf = [Weak](int32 Who)
	{
		const UOutlawsCoop* C = Weak.Get();
		const FOutlawsDmPlayer* P = C ? const_cast<UOutlawsCoop*>(C)->Dm.Find(Who) : nullptr;
		return P ? P->Name : FString::Printf(TEXT("Player %d"), Who + 1);
	};
	Modes.OnWin = [Weak](const FString& Winner, const FString& Why) { if (UOutlawsCoop* C = Weak.Get()) C->ModeWon(Winner, Why); };
	Modes.ReadLevel(*Lv);
	S->bTeams = Modes.IsTeamClass();
	S->bCheatsOff = IsMatch() && !MatchOptions.bAllowCheats;           // (BUG-ALLOWCHEATS)
	S->ForceNetUpdate();
}

void UOutlawsCoop::ModeWon(const FString& Winner, const FString& Why)
{
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLMODE the match is won: %s (%s)"), *Winner, *Why);
	OutlawsMpHost::Flow().EndNow(Winner, Why);
}

void UOutlawsCoop::OnMatchTeam(int32 Who, int32 Team)
{
	if (!IsHost() || Who < 0 || Who >= MaxPlayers) return;
	if (!MatchInRules[Who] && (Team == 1 || Team == 2))
	{
		// (REQ-MPLOBBY: Join Red / Join Blue in the lobby: put on the player once the match's rules have it)
		LobbyTeam[Who] = uint8(Team);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMODE the %s picked the %s team in the lobby"), WhoWord(Who), FOutlawsMatchModes::TeamName(Team));
		return;
	}
	if (!bMatchSetUp) return;
	const AOutlawsCoopLink* K = GetLink(Who);
	const FVector At = Who == 0 && LocalPlayer() ? LocalPlayer()->GetActorLocation() : K ? FVector(K->Pose.Body) : FVector::ZeroVector;
	if (Modes.SetTeam(Who, Team, At, FPlatformTime::Seconds())) UE_LOG(LogOutlawsCoop, Log, TEXT("OLMODE the %s is now on the %s team"), WhoWord(Who), FOutlawsMatchModes::TeamName(Team));
}

void UOutlawsCoop::TickModesHost(double Now, float Dt)
{
	// where each living player stands (its feet): the host's own, the others' from their poses
	TArray<AOutlawsCoopLink*> Links;
	GetLinks(Links);
	AOutlawsPlayer* P = LocalPlayer();
	auto Where = [&](int32 Who, FVector& OutFeet) -> bool
	{
		if (Who < 0 || Who >= MaxPlayers || MatchDead[Who] || !MatchInRules[Who]) return false;
		if (Who == 0)
		{
			if (!P) return false;
			OutFeet = P->GetActorLocation() - FVector(0.0, 0.0, P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight());
			return true;
		}
		const AOutlawsCoopLink* K = GetLink(Who);
		if (!K || K->Pose.Seq <= 0) return false;
		OutFeet = FVector(K->Pose.Body) - FVector(0.0, 0.0, K->Pose.HalfHeight);
		return true;
	};
	for (int32 Who = 0; Who < MaxPlayers; ++Who) if (MatchInRules[Who]) Modes.NoteAlive(Who, !MatchDead[Who]);
	Modes.Tick(Now, Dt, Where);
	// each player's team on its link (the plates and the colours on every machine)
	for (AOutlawsCoopLink* K : Links)
	{
		const uint8 T = uint8(Modes.TeamOf(K->PlayerIndex));
		if (K->Team != T)
		{
			K->Team = T;
			K->ForceNetUpdate();
		}
	}
	PublishModes();
}

void UOutlawsCoop::PublishModes()
{
	AOutlawsCoopSession* S = GetSession();
	if (!S) return;
	TArray<FOutlawsModeItemRep> Items;
	for (const FOutlawsModeItem& I : Modes.Items)
	{
		FOutlawsModeItemRep& R = Items.AddDefaulted_GetRef();
		R.Kind = uint8(I.Kind);
		R.State = uint8(I.State);
		R.Carrier = int8(I.Carrier);
		R.At = I.At;
	}
	const bool bChanged = Items != S->ModeItems || S->RedScore != Modes.TeamScore[1] || S->BlueScore != Modes.TeamScore[2] || S->ItPlayer != Modes.It;
	if (!bChanged) return;
	const bool bStates = Items.Num() != S->ModeItems.Num() || [&]() { for (int32 i = 0; i < Items.Num(); ++i) if (Items[i].State != S->ModeItems[i].State || Items[i].Carrier != S->ModeItems[i].Carrier) return true; return false; }();
	S->ModeItems = MoveTemp(Items);
	if (bStates || S->RedScore != Modes.TeamScore[1] || S->BlueScore != Modes.TeamScore[2] || S->ItPlayer != Modes.It)
	{
		FString Line;
		for (const FOutlawsModeItem& I : Modes.Items) Line += (Line.IsEmpty() ? TEXT("") : TEXT("; ")) + I.Describe();
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMODE state: %s; red %d, blue %d%s"), Line.IsEmpty() ? TEXT("no items") : *Line, Modes.TeamScore[1], Modes.TeamScore[2],
			Modes.It >= 0 ? *FString::Printf(TEXT("; IT the %s"), WhoWord(Modes.It)) : TEXT(""));
	}
	S->RedScore = Modes.TeamScore[1];
	S->BlueScore = Modes.TeamScore[2];
	S->ItPlayer = Modes.It;
	S->ForceNetUpdate();
}

// ---- every machine: the items drawn -------------------------------------------------------------------------------------------

void UOutlawsCoop::TickModeSprites(double Now)
{
	AOutlawsLevelActor* L = FindLevel();
	const AOutlawsCoopSession* S = GetSession();
	if (!L || !S || !bLevelBuilt || L->bNeedPlace) return;
	TMap<int32, TPair<FString, FVector>> Want;
	for (int32 i = 0; i < S->ModeItems.Num(); ++i)
	{
		const FOutlawsModeItemRep& R = S->ModeItems[i];
		FOutlawsModeItem Probe;
		Probe.Kind = EOutlawsModeItemKind(R.Kind);
		if (R.State == uint8(EOutlawsModeItemState::Carried))
		{
			// over the carrier's head, the carrier marked for all; one's own carried item is not drawn
			const AOutlawsRemotePlayer* Rp = R.Carrier != MyIndex() ? PuppetOf(R.Carrier) : nullptr;
			if (Rp) Want.Add(i, TPair<FString, FVector>(Probe.Nwx(), Rp->ShownHead() + FVector(0.0, 0.0, 30.0)));
			continue;
		}
		Want.Add(i, TPair<FString, FVector>(Probe.Nwx(), FVector(R.At)));
	}
	// Tag: IT marked for all (the neutral flag over its head: guess)
	if (S->ItPlayer >= 0 && S->ItPlayer != MyIndex())
	{
		if (const AOutlawsRemotePlayer* Rp = PuppetOf(S->ItPlayer)) Want.Add(ItMarkerKey, TPair<FString, FVector>(TEXT("unflag.nwx"), Rp->ShownHead() + FVector(0.0, 0.0, 30.0)));
	}
	// gone: the sprites no longer wanted (or wanted as another sprite)
	for (auto It = ModeSprites.CreateIterator(); It; ++It)
	{
		const TPair<FString, FVector>* W = Want.Find(It.Key());
		AOutlawsSpriteActor* A = It.Value().Get();
		if (!W || !A || ModeSpriteNwx.FindRef(It.Key()) != W->Key)
		{
			if (A) A->Destroy();
			ModeSpriteNwx.Remove(It.Key());
			It.RemoveCurrent();
		}
	}
	for (const TPair<int32, TPair<FString, FVector>>& Kv : Want)
	{
		const FVector At = Kv.Value.Value;
		if (AOutlawsSpriteActor* A = ModeSprites.FindRef(Kv.Key).Get())
		{
			if (!A->GetActorLocation().Equals(At, 1.0)) A->SetActorLocation(At);
			continue;
		}
		const TSharedPtr<OutlawsTextures::FSpriteAtlas> Atlas = L->GetPickupAtlas(Kv.Value.Key);
		AOutlawsSpriteActor* A = Atlas.IsValid() ? L->MakeSprite(Atlas, FPaths::GetBaseFilename(Kv.Value.Key).ToUpper(), At, 0.f, L->LightAt(At + FVector(0.0, 0.0, 5.0))) : nullptr;
		if (!A)
		{
			if (Now >= NextModeLogAt)
			{
				NextModeLogAt = Now + 5.0;
				UE_LOG(LogOutlawsCoop, Warning, TEXT("OLMODE the sprite %s could not be made here"), *Kv.Value.Key);
			}
			continue;
		}
		ModeSprites.Add(Kv.Key, A);
		ModeSpriteNwx.Add(Kv.Key, Kv.Value.Key);
	}
	if (ModeSprites.Num() != LoggedModeItems)
	{
		LoggedModeItems = ModeSprites.Num();
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMODE drawn here (%s): %d mode sprites"), IsHost() ? TEXT("host") : WhoWord(MyIndex()), LoggedModeItems);
	}
}

// ---- the frag table ---------------------------------------------------------------------------------------------------------------

void UOutlawsCoop::FillModeColumns(FFragTableData& Out) const
{
	// called by FillMatchTable once its rows are in: the class's title, its extra column, the team scores and colours
	const AOutlawsCoopSession* S = GetSession();
	if (!S) return;
	const EOutlawsGameClass Class = EOutlawsGameClass(FMath::Clamp<int32>(S->MatchClass, 1, 6));
	Out.Title = OutlawsMpHost::ClassLabel(Class);
	const bool bCapt = Class == EOutlawsGameClass::CaptureTheFlag || Class == EOutlawsGameClass::SecretDocument;
	const bool bTime = Class == EOutlawsGameClass::Chicken || Class == EOutlawsGameClass::Tag;
	if (bCapt || bTime)
	{
		FFragTableColumn C;
		C.Header = bCapt ? TEXT("CAPTURES") : TEXT("TIME");
		C.Width = 210.f;
		Out.Columns.Insert(C, 1);
	}
	if (S->bTeams)
	{
		Out.HeaderRows.Insert({ TEXT("Red Team"), FString::FromInt(S->RedScore), 1 }, 0);
		Out.HeaderRows.Insert({ TEXT("Blue Team"), FString::FromInt(S->BlueScore), 2 }, 1);
	}
	for (FFragTableRow& Row : Out.Rows)
	{
		const FOutlawsMatchScore* R = S->Scores.FindByPredicate([&Row](const FOutlawsMatchScore& X) { return X.Name == Row.Name; });
		if (!R) continue;
		Row.Team = R->Team;
		if (bCapt) Row.Values.Insert(FString::FromInt(R->Captures), 0);
		if (bTime) Row.Values.Insert(HeldClock(R->HeldSeconds), 0);
		if (bCapt) Row.SortA = R->Captures * 1000 + R->Frags;
		if (bTime) Row.SortA = R->HeldSeconds;
		if (bTime) Row.SortB = R->Deaths;         // (Tag's tie: fewest deaths, guess)
	}
	Out.SortRows();
}

// ---- its test (-OLMpTest=modes: -OLCoopAuto -OLCoopTest=modes on all three; the host with -OLMpHost=<map> -OLMpClass=<n>) ----------
// The host drives the two clients (A player 1, B player 2) by coordinates it sends (md:x / md:y / md:z then md:go) and by M1's
// dm:aim / dm:shoot / dm:respawn; each class checks its own rules and the match's end on all three.

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TickHostModesTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	FOutlawsMatchFlow& F = OutlawsMpHost::Flow();
	auto Check = [](bool bOk, const FString& What) { UE_LOG(LogOutlawsCoop, Log, TEXT("OLMODE test check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What); };
	auto Goto = [this](int32 Who, const FVector& Feet)
	{
		// (the feet's place, sent in whole cm; the client stands its body there)
		TestSayTo(Who, TEXT("md:x"), FMath::RoundToInt(Feet.X));
		TestSayTo(Who, TEXT("md:y"), FMath::RoundToInt(Feet.Y));
		TestSayTo(Who, TEXT("md:z"), FMath::RoundToInt(Feet.Z));
		TestSayTo(Who, TEXT("md:go"), 0);
	};
	auto Kill = [&](int32 Shooter, int32 Victim)
	{
		// (one shot every call while the victim lives: M1's test steps; the shooter set beside the victim first, and again
		// every fifth try)
		if (ModeTestTries % 5 == 1) TestSayTo(Shooter, TEXT("dm:aim"), Victim);
		TestSayTo(Shooter, TEXT("dm:shoot"), Victim);
	};
	const FOutlawsModeItem* I0 = Modes.Items.IsValidIndex(0) ? &Modes.Items[0] : nullptr;
	const FOutlawsModeItem* I1 = Modes.Items.IsValidIndex(1) ? &Modes.Items[1] : nullptr;
	const int32 A = ModeTestA, B = ModeTestB;
	if (TestStep < 800) { TestStep = 800; TestAt = 0.0; }
	if (TestStep >= 800 && TestStep < 802 && Now < TestAt) return;
	switch (TestStep)
	{
	case 800:
	{
		int32 In = 0;
		for (int32 k = 1; k < MaxPlayers; ++k) In += MatchInRules[k] ? 1 : 0;
		if (In < 2 || !bMatchSetUp) return;
		if (TestAt == 0.0) { TestAt = Now + 5.0; return; }
		ModeTestA = 1;
		ModeTestB = 2;
		Check(F.HostStart(), FString::Printf(TEXT("%s: the host's Start the game"), *OutlawsMpHost::ClassLabel(Modes.Class)));
		TestStep = 801;
		return;
	}
	case 801:
		if (F.GetState() != FOutlawsMatchFlow::EState::Playing) return;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMODE test: %s live; teams: host %d, A %d, B %d; items %d"), *OutlawsMpHost::ClassLabel(Modes.Class), Modes.TeamOf(0), Modes.TeamOf(1), Modes.TeamOf(2), Modes.Items.Num());
		TestStep = 802;
		TestAt = Now + 4.0;                      // (past the spawn protection)
		ModeTestTries = 0;
		return;
	default:
		break;
	}
	if (TestStep >= 900)
	{
		// the end: on every machine
		if (TestStep == 900)
		{
			if (F.GetState() != FOutlawsMatchFlow::EState::Ended && Now < TestAt) return;
			Check(F.GetState() == FOutlawsMatchFlow::EState::Ended, FString::Printf(TEXT("the class's own limit ended the match (%s)"), FOutlawsMatchFlow::StateName(F.GetState())));
			TestStep = 901;
			TestAt = Now + 3.0;
			return;
		}
		if (TestStep == 901 && Now >= TestAt)
		{
			for (int32 k = 1; k <= 2; ++k) TestSayTo(k, TEXT("dm:report"), 0);
			FFragTableData T;
			FillMatchTable(T);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLMODE test: the host's table: %s"), *T.Describe());
			TestStep = 902;
			TestAt = Now + 2.0;
			return;
		}
		if (TestStep == 902 && Now >= TestAt)
		{
			for (int32 k = 1; k <= 2; ++k) TestSayTo(k, TEXT("quit"), 0);
			TestStep = 903;
			TestAt = Now + 3.0;
			return;
		}
		if (TestStep == 903 && Now >= TestAt)
		{
			Summary(TEXT("done"));
			QuitForTest();
			TestStep = 904;
		}
		return;
	}
	if (Now < TestAt) return;
	auto Next = [&](double Wait) { ++TestStep; TestAt = Now + Wait; ModeTestTries = 0; };
	auto Fail = [&](const FString& Why) { Check(false, Why); TestStep = 900; TestAt = Now + 1.0; };
	auto Tries = [&](int32 Most, const TCHAR* What) { if (++ModeTestTries > Most) { Fail(FString::Printf(TEXT("%s never happened"), What)); return false; } return true; };
	switch (Modes.Class)
	{
	case EOutlawsGameClass::TeamPlay:
		// A and B on opposite teams (auto-balance: the host red, A blue, B red); friendly fire off: B's shot at the host (its
		// teammate) is refused; A kills B twice: the blue team reaches the limit (2)
		switch (TestStep)
		{
		case 802:
			Check(Modes.TeamOf(A) != Modes.TeamOf(B) && Modes.TeamOf(0) == Modes.TeamOf(B), FString::Printf(TEXT("teams balanced: host %s, A %s, B %s"),
				FOutlawsMatchModes::TeamName(Modes.TeamOf(0)), FOutlawsMatchModes::TeamName(Modes.TeamOf(A)), FOutlawsMatchModes::TeamName(Modes.TeamOf(B))));
			MatchTestShots = MatchHitsRefused;
			TestSayTo(B, TEXT("dm:aim"), 0);
			Next(1.5);
			return;
		case 803:
			TestSayTo(B, TEXT("dm:shoot"), 0);
			Next(1.0);
			return;
		case 804:
			Check(MatchHitsRefused > MatchTestShots && L && L->GetHealth() >= 10.f, FString::Printf(TEXT("a teammate's shot refused (friendly fire off): refused %d, the host's health %.1f"),
				MatchHitsRefused - MatchTestShots, L ? L->GetHealth() : -1.f));
			Next(0.5);
			return;
		case 805:
		case 807:
			if (MatchDead[B]) { Next(0.5); return; }
			if (!Tries(20, TEXT("A killing B"))) return;
			Kill(A, B);
			TestAt = Now + 0.5;
			return;
		case 806:
			Check(Modes.TeamScore[Modes.TeamOf(A)] == 1, FString::Printf(TEXT("the %s team scores for A's kill: red %d, blue %d"), FOutlawsMatchModes::TeamName(Modes.TeamOf(A)), Modes.TeamScore[1], Modes.TeamScore[2]));
			TestSayTo(B, TEXT("dm:respawn"), 0);
			Next(8.0);
			return;
		case 808:
			TestStep = 900;
			TestAt = Now + 3.0;
			return;
		}
		return;
	case EOutlawsGameClass::CaptureTheFlag:
		// A (blue) takes the red flag and brings it home: a capture; takes it again, B (red) kills A: dropped; B touches it:
		// home again; A takes it once more and captures: the limit (2)
		if (!I0 || !I1) return Fail(TEXT("no flags on this map"));
		{
			const int32 TA = Modes.TeamOf(A);
			const FOutlawsModeItem& Enemy = TA == 1 ? *I1 : *I0;
			const FOutlawsModeItem& Own = TA == 1 ? *I0 : *I1;
			switch (TestStep)
			{
			case 802: case 806: case 812:
				Goto(A, Enemy.At);
				Next(2.0);
				return;
			case 803: case 807: case 813:
				Check(Enemy.State == EOutlawsModeItemState::Carried && Enemy.Carrier == A, FString::Printf(TEXT("A carries the %s flag (%s)"), FOutlawsMatchModes::TeamName(Enemy.Team), *Enemy.Describe()));
				if (TestStep == 807) { Next(0.5); return; }
				Goto(A, Own.Home);
				Next(2.0);
				return;
			case 804: case 814:
				Check(Enemy.State == EOutlawsModeItemState::Home && Modes.TeamScore[TA] == (TestStep == 804 ? 1 : 2), FString::Printf(TEXT("A captured: red %d, blue %d"), Modes.TeamScore[1], Modes.TeamScore[2]));
				if (TestStep == 814) { TestStep = 900; TestAt = Now + 3.0; return; }
				Next(0.5);
				return;
			case 805:
				Next(0.5);
				return;
			case 808:
				if (MatchDead[A]) { Next(1.0); return; }
				if (!Tries(20, TEXT("B killing A"))) return;
				Kill(B, A);
				TestAt = Now + 0.5;
				return;
			case 809:
				Check(Enemy.State == EOutlawsModeItemState::Dropped, FString::Printf(TEXT("the flag dropped where A fell (%s)"), *Enemy.Describe()));
				Goto(B, Enemy.At);
				Next(2.0);
				return;
			case 810:
				Check(Enemy.State == EOutlawsModeItemState::Home, FString::Printf(TEXT("B's touch sent its own flag home (%s)"), *Enemy.Describe()));
				TestSayTo(A, TEXT("dm:respawn"), 0);
				Next(8.0);
				return;
			case 811:
				Next(4.0);
				return;
			}
		}
		return;
	case EOutlawsGameClass::Chicken:
		// A takes the chicken; B kills A: the chicken loose; B takes it and holds it to the limit
		if (!I0) return Fail(TEXT("no chicken"));
		switch (TestStep)
		{
		case 802:
			Goto(A, I0->At);
			Next(2.0);
			return;
		case 803:
			Check(I0->State == EOutlawsModeItemState::Carried && I0->Carrier == A, FString::Printf(TEXT("A carries the chicken (%s)"), *I0->Describe()));
			Next(2.0);
			return;
		case 804:
			if (MatchDead[A]) { Next(1.0); return; }
			if (!Tries(20, TEXT("B killing A"))) return;
			Kill(B, A);
			TestAt = Now + 0.5;
			return;
		case 805:
		{
			const FOutlawsModePlayer* PA = Modes.Find(A);
			Check(I0->State == EOutlawsModeItemState::Dropped && PA && PA->Held > 1.f, FString::Printf(TEXT("the chicken loose where A fell; A held it %.1f s"), PA ? PA->Held : -1.f));
			Goto(B, I0->At);
			Next(2.0);
			return;
		}
		case 806:
			Check(I0->State == EOutlawsModeItemState::Carried && I0->Carrier == B, FString::Printf(TEXT("B carries the chicken (%s)"), *I0->Describe()));
			TestStep = 900;
			TestAt = Now + Modes.SecondsLimit + 5.0;
			return;
		}
		return;
	case EOutlawsGameClass::Tag:
		// whoever is IT: a client kills IT (B kills A, or A kills B, or A kills the host): the killer is IT and holds it to the limit
		switch (TestStep)
		{
		case 802:
			Check(Modes.It >= 0, FString::Printf(TEXT("someone is IT at the start (the %s)"), Modes.It >= 0 ? WhoWord(Modes.It) : TEXT("nobody")));
			ModeTestA = Modes.It == 1 ? 2 : 1;      // (the killer)
			ModeTestB = Modes.It;                   // (IT, the victim)
			Next(0.5);
			return;
		case 803:
			if (MatchDead[B]) { Next(1.0); return; }
			if (!Tries(20, TEXT("killing IT"))) return;
			Kill(A, B);
			TestAt = Now + 0.5;
			return;
		case 804:
			Check(Modes.It == A, FString::Printf(TEXT("the %s killed IT and is IT (IT: the %s)"), WhoWord(A), Modes.It >= 0 ? WhoWord(Modes.It) : TEXT("nobody")));
			if (B != 0) TestSayTo(B, TEXT("dm:respawn"), 0);
			TestStep = 900;
			TestAt = Now + Modes.SecondsLimit + 5.0;
			return;
		}
		return;
	case EOutlawsGameClass::SecretDocument:
		// A takes the document and brings it to a capture point, twice: the limit (2)
		if (!I0) return Fail(TEXT("no document"));
		switch (TestStep)
		{
		case 802: case 805:
			Goto(A, I0->At);
			Next(2.0);
			return;
		case 803: case 806:
		{
			Check(I0->State == EOutlawsModeItemState::Carried && I0->Carrier == A, FString::Printf(TEXT("A carries the document (%s)"), *I0->Describe()));
			const FVector To = Modes.bTeamDocument ? Modes.Bases[FMath::Clamp(Modes.TeamOf(A) - 1, 0, 1)] : (Modes.CapturePoints.Num() > 0 ? Modes.CapturePoints[0] : I0->Home);
			Goto(A, To);
			Next(2.0);
			return;
		}
		case 804:
		{
			const FOutlawsModePlayer* PA = Modes.Find(A);
			Check(I0->State == EOutlawsModeItemState::Home && PA && PA->Captures == 1, FString::Printf(TEXT("A captured the document (%d), it is back in its place"), PA ? PA->Captures : -1));
			Next(0.5);
			return;
		}
		case 807:
			TestStep = 900;
			TestAt = Now + 3.0;
			return;
		}
		return;
	default:
		return Fail(TEXT("the test has no steps for this class"));
	}
}

void UOutlawsCoop::OnModesTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	static FVector Target = FVector::ZeroVector;
	if (Step == TEXT("x")) Target.X = Arg;
	else if (Step == TEXT("y")) Target.Y = Arg;
	else if (Step == TEXT("z")) Target.Z = Arg;
	else if (Step == TEXT("go"))
	{
		const float Half = P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		P->TeleportTo(Target + FVector(0.0, 0.0, Half + 2.0), P->GetActorRotation(), false, true);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMODE test: the %s stands at %s"), WhoWord(MyIndex()), *Target.ToCompactString());
	}
}
#endif // WITH_OUTLAWS_TESTS
