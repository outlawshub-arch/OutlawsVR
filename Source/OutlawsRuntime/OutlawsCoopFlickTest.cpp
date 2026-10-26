// REQ-FLICKRELOAD, the two-machine test (-OLFlickNetTest on a host and its client; the host with -OLMpHost=<map> -OLMpBots=0).  The host
// starts the match, then:
//   1. the host flicks its double barrel open (both fired): the client must see the host's right gun open (the pose's Break, the host's
//      word) and two shells fly from the host's puppet; the host flicks it shut: the client sees it shut;
//   2. the client flicks its sawed-off open (one fired): the host must take it (its own BreakAccepted, the pose it passes on) and see one
//      shell fly from the client's puppet; the client flicks it shut: the host sees it shut;
//   1b. (REQ-FLICKRULES) the host's single-barrel Shotgun, fired: open with one shell, then shut, as the client sees it; the host's
//      flicks refused (the Shotgun loaded, the double barrel held two-handed) reach the client as nothing;
//   3. the client sends a break out of bounds (seven shells): the host drops it, nothing opens, no shells;
//   4. the client's Shotgun, fired: the host takes it open with one shell, then shut; the client's refused flicks reach no one.
//   (REQ-RELOADSLICES) each machine hears the other's reload as its own: the open slice and a shell slice per shell going in, then
//   the shut slice, never the empty-chamber click (letters O, S, C; X the click); 1c. the host's double barrel shut early: the
//   client's run is cut off (O, maybe S, then C).
// Every machine prints "OLSHOT fl check PASS|FAIL: ..." and "OLSHOT fl <host|client> RESULT".
#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsRemotePlayer.h"
#include "OutlawsMpHost.h"
#include "Engine/World.h"

#if WITH_OUTLAWS_TESTS

FString UOutlawsCoop::FlHeard(bool bReset)
{
	static TArray<FString> Spy;
	AOutlawsLevelActor::WavSpy = &Spy;
	const FString Got = AOutlawsLevelActor::ReloadCodes(Spy);
	if (bReset) Spy.Reset();
	return Got;
}

bool UOutlawsCoop::FlickNetCheck(bool bOk, const FString& What)
{
	UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT fl check %s: %s [%s]"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What, IsHost() ? TEXT("host") : WhoWord(MyIndex()));
	if (!bOk) ++FlFails;
	return bOk;
}

void UOutlawsCoop::TickHostFlickNetTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	FOutlawsMatchFlow& F = OutlawsMpHost::Flow();
	if (TestStep < 1100) { TestStep = 1100; TestAt = 0.0; }
	auto Seen = [this](int32 Who) { const FIntVector* S = BreakSeen.Find(Who); return S ? *S : FIntVector(0, 0, 0); };
	auto Shown = [this](int32 Who) { const AOutlawsRemotePlayer* R = PuppetOf(Who); return R ? R->ShellsShown : -1; };
	auto Heard = [this](const TCHAR* Want, const TCHAR* What)
	{
		const FString Got = FlHeard(true);
		FlickNetCheck(Got == Want, FString::Printf(TEXT("the host heard the client's %s as '%s' (want '%s')"), What, *Got, Want));
	};
	switch (TestStep)
	{
	case 1100:
	{
		TArray<AOutlawsCoopLink*> Links;
		GetLinks(Links);
		for (const AOutlawsCoopLink* K : Links)
		{
			if (!K->bBot && K->PlayerIndex > 0 && FlClient < 0) FlClient = K->PlayerIndex;
		}
		if (FlClient < 0 || !bMatchSetUp) return;
		if (TestAt == 0.0) { TestAt = Now + 6.0; return; }
		if (Now < TestAt) return;
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT fl: the host and the client (%s): Start"), WhoWord(FlClient));
		F.HostStart();
		TestStep = 1101;
		return;
	}
	case 1101:
		if (F.GetState() != FOutlawsMatchFlow::EState::Playing) return;
		TestSayTo(FlClient, TEXT("fl:listen"), 0);     // (REQ-RELOADSLICES: both machines' ears on)
		FlHeard(true);
		TestStep = 1110;
		TestAt = Now + 6.0;                      // (past the spawn protection and the kit's draw)
		return;

	// 1. the host's double barrel, both barrels fired: open, then shut
	case 1110:
	{
		if (Now < TestAt) return;
		P->TestBreakArm(1, 3, 0, 10);
		const int32 S0 = P->ShellsOut;
		P->TestHandMotion(1, -800.f, 40.f, 30.f, 0.09f, 60.f);
		FlickNetCheck(P->IsBreakOpen(1) && P->ShellsOut - S0 == 2, FString::Printf(TEXT("the host's flick opens its double barrel, %d shells out"), P->ShellsOut - S0));
		TestStep = 1111;
		TestAt = Now + 1.5;
		return;
	}
	case 1111:
		if (Now < TestAt) return;
		TestSayTo(FlClient, TEXT("fl:expect:0:1:2:OSS"), 0);
		TestStep = 1120;
		TestAt = Now + 1.0;
		return;
	case 1120:
		if (Now < TestAt) return;
		P->TestHandStill(1, 0.5f);
		P->TestHandMotion(1, 800.f, -40.f, -30.f, 0.09f, 60.f);
		FlickNetCheck(!P->IsBreakOpen(1) && P->TestMag(1) == 2, FString::Printf(TEXT("the host's flick back shuts it, %d loaded"), P->TestMag(1)));
		TestStep = 1121;
		TestAt = Now + 1.5;
		return;
	case 1121:
		if (Now < TestAt) return;
		TestSayTo(FlClient, TEXT("fl:expect:0:0:2:C"), 0);
		TestStep = 1122;
		TestAt = Now + 1.0;
		return;

	// 1b. (REQ-FLICKRULES) the host's single-barrel Shotgun, fired: one shell out, the client sees it open, then shut
	case 1122:
	{
		if (Now < TestAt) return;
		P->TestBreakArm(1, 2, 0, 10);
		const int32 S0 = P->ShellsOut;
		P->TestHandMotion(1, -800.f, 40.f, 30.f, 0.09f, 60.f);
		FlickNetCheck(P->IsBreakOpen(1) && P->ShellsOut - S0 == 1, FString::Printf(TEXT("the host's flick opens its Shotgun, %d shell out (want 1)"), P->ShellsOut - S0));
		TestStep = 1123;
		TestAt = Now + 1.5;
		return;
	}
	case 1123:
		if (Now < TestAt) return;
		TestSayTo(FlClient, TEXT("fl:expect:0:1:3:OS"), 0);
		TestStep = 1124;
		TestAt = Now + 1.0;
		return;
	case 1124:
		if (Now < TestAt) return;
		P->TestHandStill(1, 0.5f);
		P->TestHandMotion(1, 800.f, -40.f, -30.f, 0.09f, 60.f);
		FlickNetCheck(!P->IsBreakOpen(1) && P->TestMag(1) == 1, FString::Printf(TEXT("the host's flick back shuts its Shotgun, %d loaded (want 1)"), P->TestMag(1)));
		TestStep = 1125;
		TestAt = Now + 1.5;
		return;
	case 1125:
	{
		if (Now < TestAt) return;
		// refused flicks: the loaded Shotgun, then the double barrel held two-handed; neither reaches the client
		P->TestHandStill(1, 0.3f);
		const int32 Full0 = P->FlickRefusedFull, Two0 = P->FlickRefusedTwoHands;
		P->TestHandMotion(1, -800.f, 40.f, 30.f, 0.09f, 60.f);
		FlickNetCheck(!P->IsBreakOpen(1) && P->FlickRefusedFull == Full0 + 1, TEXT("the host's flick on its loaded Shotgun is refused"));
		P->TestBreakArm(1, 3, 0, 10);
		P->TestSetTwoHand(1, true);
		P->TestHandMotion(1, -800.f, 40.f, 30.f, 0.09f, 60.f);
		FlickNetCheck(!P->IsBreakOpen(1) && P->FlickRefusedTwoHands == Two0 + 1, TEXT("the host's flick on its double barrel held two-handed is refused"));
		P->TestSetTwoHand(1, false);
		TestStep = 1126;
		TestAt = Now + 1.5;
		return;
	}
	case 1126:
		if (Now < TestAt) return;
		TestSayTo(FlClient, TEXT("fl:expect:0:0:3:C"), 0);
		TestStep = 1127;
		TestAt = Now + 1.0;
		return;

	// 1c. (REQ-RELOADSLICES) the host's double barrel, both fired, shut 0.3 s after it opened: the client's shells are cut off
	case 1127:
		if (Now < TestAt) return;
		P->TestBreakArm(1, 3, 0, 10);
		P->TestHandMotion(1, -800.f, 40.f, 30.f, 0.09f, 60.f);
		P->TestHandStill(1, 0.3f);
		P->TestHandMotion(1, 800.f, -40.f, -30.f, 0.09f, 60.f);
		FlickNetCheck(!P->IsBreakOpen(1) && P->TestMag(1) == 2, FString::Printf(TEXT("the host's double barrel opened and shut early, %d loaded"), P->TestMag(1)));
		TestStep = 1128;
		TestAt = Now + 1.5;
		return;
	case 1128:
		if (Now < TestAt) return;
		TestSayTo(FlClient, TEXT("fl:expect:0:0:5:early"), 0);
		TestStep = 1129;
		TestAt = Now + 1.0;
		return;

	// 1d. (REQ-RELOADHIDEHAND) the host's own button reloads held two-handed: its front hand hidden open to shut, back on the forend
	case 1129:
		if (Now < TestAt) return;
		for (const int32 Wi : { 2, 3, 4 })
		{
			FString Drawn;
			const bool bOk = P->TestHiddenHandReload(Wi, Drawn);
			FlickNetCheck(bOk, FString::Printf(TEXT("hidehand: the host's gun %d, its button reload held two-handed: %s"), Wi, *Drawn));
		}
		TestStep = 1130;
		TestAt = Now + 2.0;
		return;

	// 2. the client's sawed-off, one barrel fired: the host's word on it
	case 1130:
		if (Now < TestAt) return;
		FlHeard(true);                           // (the host's own breaks' sounds out of the way)
		TestSayTo(FlClient, TEXT("fl:open"), 0);
		TestStep = 1131;
		TestAt = Now + 2.0;
		return;
	case 1131:
	{
		if (Now < TestAt) return;
		const AOutlawsCoopLink* K = GetLink(FlClient);
		const FIntVector S = Seen(FlClient);
		FlickNetCheck(K && (K->BreakAccepted & 2) && (K->Pose.Break & 2), FString::Printf(TEXT("the host takes the client's right gun as open (accepted %d, its pose %d)"),
			K ? K->BreakAccepted : -1, K ? K->Pose.Break : -1));
		FlickNetCheck(S.X >= 1 && S.Z == 1 && Shown(FlClient) == 1, FString::Printf(TEXT("the host saw the client's break: %d open, %d shells told, %d drawn from its puppet (want 1)"),
			S.X, S.Z, Shown(FlClient)));
		Heard(TEXT("OS"), TEXT("sawed-off opening (one going in)"));
		TestSayTo(FlClient, TEXT("fl:shut"), 0);
		TestStep = 1140;
		TestAt = Now + 2.0;
		return;
	}
	case 1140:
	{
		if (Now < TestAt) return;
		const AOutlawsCoopLink* K = GetLink(FlClient);
		const FIntVector S = Seen(FlClient);
		FlickNetCheck(K && !(K->BreakAccepted & 2) && !(K->Pose.Break & 2) && S.Y >= 1, FString::Printf(TEXT("the host saw the client's gun shut (accepted %d, pose %d, %d shuts)"),
			K ? K->BreakAccepted : -1, K ? K->Pose.Break : -1, S.Y));
		Heard(TEXT("C"), TEXT("sawed-off shutting"));
		TestSayTo(FlClient, TEXT("fl:bogus"), 0);
		TestStep = 1150;
		TestAt = Now + 2.0;
		return;
	}

	// 3. a break out of bounds from the client: dropped
	case 1150:
	{
		if (Now < TestAt) return;
		const AOutlawsCoopLink* K = GetLink(FlClient);
		const FIntVector S = Seen(FlClient);
		FlickNetCheck(K && K->BreakAccepted == 0 && S.Z == 1 && Shown(FlClient) == 1, FString::Printf(TEXT("a break with seven shells from the client is dropped (accepted %d, %d shells told)"),
			K ? K->BreakAccepted : -1, S.Z));
		Heard(TEXT(""), TEXT("dropped break"));
		TestSayTo(FlClient, TEXT("fl:sgopen"), 0);
		TestStep = 1151;
		TestAt = Now + 2.0;
		return;
	}

	// 4. (REQ-FLICKRULES) the client's single-barrel Shotgun, fired: the host takes it open with one shell, then shut; then the
	// client's refused flicks (loaded, two-handed), which reach no one
	case 1151:
	{
		if (Now < TestAt) return;
		const AOutlawsCoopLink* K = GetLink(FlClient);
		const FIntVector S = Seen(FlClient);
		FlickNetCheck(K && (K->BreakAccepted & 2) && (K->Pose.Break & 2) && S.Z == 2 && Shown(FlClient) == 2, FString::Printf(TEXT("the host takes the client's Shotgun as open (accepted %d, pose %d, %d shells told, %d drawn; want 2)"),
			K ? K->BreakAccepted : -1, K ? K->Pose.Break : -1, S.Z, Shown(FlClient)));
		Heard(TEXT("OS"), TEXT("Shotgun opening"));
		TestSayTo(FlClient, TEXT("fl:sgshut"), 0);
		TestStep = 1152;
		TestAt = Now + 2.0;
		return;
	}
	case 1152:
	{
		if (Now < TestAt) return;
		const AOutlawsCoopLink* K = GetLink(FlClient);
		const FIntVector S = Seen(FlClient);
		FlickNetCheck(K && !(K->BreakAccepted & 2) && !(K->Pose.Break & 2) && S.Y >= 2, FString::Printf(TEXT("the host saw the client's Shotgun shut (accepted %d, pose %d, %d shuts)"),
			K ? K->BreakAccepted : -1, K ? K->Pose.Break : -1, S.Y));
		Heard(TEXT("C"), TEXT("Shotgun shutting"));
		TestSayTo(FlClient, TEXT("fl:rules"), 0);
		TestStep = 1153;
		TestAt = Now + 2.0;
		return;
	}
	case 1153:
	{
		if (Now < TestAt) return;
		const AOutlawsCoopLink* K = GetLink(FlClient);
		const FIntVector S = Seen(FlClient);
		FlickNetCheck(K && !(K->Pose.Break & 2) && S.Z == 2, FString::Printf(TEXT("the client's refused flicks reached the host as nothing (pose %d, %d shells told; want 2)"),
			K ? K->Pose.Break : -1, S.Z));
		Heard(TEXT(""), TEXT("refused flicks"));
		TestSayTo(FlClient, TEXT("fl:hidehand"), 0);
		TestSayTo(FlClient, TEXT("fl:report"), 0);
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT fl host RESULT %s (%d failed)"), FlFails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), FlFails);
		TestStep = 1160;
		TestAt = Now + 3.0;
		return;
	}
	case 1160:
		if (Now < TestAt) return;
		TestSay(TEXT("quit"), 0);
		TestStep = 1161;
		TestAt = Now + 3.0;
		return;
	case 1161:
		if (Now < TestAt) return;
		Summary(TEXT("done"));
		QuitForTest();
		TestStep = 1162;
		return;
	default:
		return;
	}
}

void UOutlawsCoop::OnFlickNetTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	if (Step.StartsWith(TEXT("expect:")))
	{
		// expect:<who>:<open 1|0>:<shells so far>[:<heard>]: that player's right gun as this machine has it, and (REQ-RELOADSLICES)
		// the reload slices heard from it since the last ("early": O, perhaps S, then C, the run cut off)
		TArray<FString> V;
		Step.Mid(7).ParseIntoArray(V, TEXT(":"));
		if (V.Num() != 3 && V.Num() != 4) return;
		if (V.Num() == 4)
		{
			const int32 Cuts = L->SoundRunCuts - FlCuts0;
			FlCuts0 = L->SoundRunCuts;
			const FString Got = FlHeard(true);
			const bool bEarly = V[3] == TEXT("early");
			const bool bOk = bEarly ? (Got == TEXT("OC") || Got == TEXT("OSC")) && Cuts >= 1 : Got == V[3];
			FlickNetCheck(bOk, FString::Printf(TEXT("the client heard the %s's reload as '%s' (want '%s'), %d run%s cut off"), WhoWord(FCString::Atoi(*V[0])), *Got,
				bEarly ? TEXT("OC or OSC, cut off") : *V[3], Cuts, Cuts == 1 ? TEXT("") : TEXT("s")));
		}
		const int32 Who = FCString::Atoi(*V[0]);
		const bool bOpen = FCString::Atoi(*V[1]) != 0;
		const int32 Shells = FCString::Atoi(*V[2]);
		const AOutlawsCoopLink* K = GetLink(Who);
		const FIntVector* S = BreakSeen.Find(Who);
		const AOutlawsRemotePlayer* R = PuppetOf(Who);
		FlickNetCheck(K && ((K->Pose.Break & 2) != 0) == bOpen, FString::Printf(TEXT("the client sees the %s's right gun %s (its pose's Break %d)"), WhoWord(Who),
			bOpen ? TEXT("open") : TEXT("shut"), K ? K->Pose.Break : -1));
		FlickNetCheck(S && (bOpen ? S->X >= 1 : S->Y >= 1) && S->Z == Shells, FString::Printf(TEXT("the client was told the %s's break: %d open, %d shut, %d shells (want %d)"), WhoWord(Who),
			S ? S->X : 0, S ? S->Y : 0, S ? S->Z : 0, Shells));
		FlickNetCheck(R && R->ShellsShown == Shells, FString::Printf(TEXT("the client drew %d shells flying from the %s's puppet (want %d; %d now)"), R ? R->ShellsShown : -1, WhoWord(Who),
			Shells, R ? R->ShellFlyers() : -1));
	}
	else if (Step == TEXT("listen"))
	{
		FlHeard(true);
		FlCuts0 = L->SoundRunCuts;
	}
	else if (Step == TEXT("open"))
	{
		P->TestBreakArm(1, 4, 1, 5);
		const int32 S0 = P->ShellsOut;
		P->TestHandMotion(1, -800.f, 40.f, 30.f, 0.09f, 60.f);
		FlickNetCheck(P->IsBreakOpen(1) && P->ShellsOut - S0 == 1, FString::Printf(TEXT("the client's flick opens its sawed-off, %d shell out (want 1)"), P->ShellsOut - S0));
	}
	else if (Step == TEXT("shut"))
	{
		P->TestHandStill(1, 0.5f);
		P->TestHandMotion(1, 800.f, -40.f, -30.f, 0.09f, 60.f);
		FlickNetCheck(!P->IsBreakOpen(1) && P->TestMag(1) == 2 && L->GetCount(TEXT("ishells")) == 4, FString::Printf(TEXT("the client's flick back shuts it, %d loaded, %d carried (want 2, 4)"),
			P->TestMag(1), L->GetCount(TEXT("ishells"))));
	}
	else if (Step == TEXT("sgopen"))
	{
		P->TestBreakArm(1, 2, 0, 5);
		const int32 S0 = P->ShellsOut;
		P->TestHandMotion(1, -800.f, 40.f, 30.f, 0.09f, 60.f);
		FlickNetCheck(P->IsBreakOpen(1) && P->ShellsOut - S0 == 1, FString::Printf(TEXT("the client's flick opens its Shotgun, %d shell out (want 1)"), P->ShellsOut - S0));
	}
	else if (Step == TEXT("sgshut"))
	{
		P->TestHandStill(1, 0.5f);
		P->TestHandMotion(1, 800.f, -40.f, -30.f, 0.09f, 60.f);
		FlickNetCheck(!P->IsBreakOpen(1) && P->TestMag(1) == 1 && L->GetCount(TEXT("ishells")) == 4, FString::Printf(TEXT("the client's flick back shuts its Shotgun, %d loaded, %d carried (want 1, 4)"),
			P->TestMag(1), L->GetCount(TEXT("ishells"))));
	}
	else if (Step == TEXT("rules"))
	{
		P->TestHandStill(1, 0.3f);
		const int32 Full0 = P->FlickRefusedFull, Two0 = P->FlickRefusedTwoHands;
		P->TestHandMotion(1, -800.f, 40.f, 30.f, 0.09f, 60.f);
		FlickNetCheck(!P->IsBreakOpen(1) && P->FlickRefusedFull == Full0 + 1, TEXT("the client's flick on its loaded Shotgun is refused"));
		P->TestBreakArm(1, 4, 0, 5);
		P->TestSetTwoHand(1, true);
		P->TestHandMotion(1, -800.f, 40.f, 30.f, 0.09f, 60.f);
		FlickNetCheck(!P->IsBreakOpen(1) && P->FlickRefusedTwoHands == Two0 + 1, TEXT("the client's flick on its sawed-off held two-handed is refused"));
		P->TestSetTwoHand(1, false);
	}
	else if (Step == TEXT("bogus"))
	{
		if (AOutlawsCoopLink* Mine = GetLink(MyIndex())) Mine->ServerBreakFx(1, true, 7, P->GetActorLocation(), FVector::ZeroVector);
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT fl: the client sent a break with seven shells"));
	}
	else if (Step == TEXT("hidehand"))
	{
		// (REQ-RELOADHIDEHAND) the client's own button reloads held two-handed
		for (const int32 Wi : { 2, 3, 4 })
		{
			FString Seen;
			const bool bOk = P->TestHiddenHandReload(Wi, Seen);
			FlickNetCheck(bOk, FString::Printf(TEXT("hidehand: the client's gun %d, its button reload held two-handed: %s"), Wi, *Seen));
		}
	}
	else if (Step == TEXT("report"))
	{
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT fl client RESULT %s (%d failed)"), FlFails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), FlFails);
	}
}
#endif // WITH_OUTLAWS_TESTS
