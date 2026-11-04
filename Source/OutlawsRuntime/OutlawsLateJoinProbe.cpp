// BUG-LATEJOIN's probe (the test runs of 3 Nov: "joins a game under way: in the lobby until it enters", and it never did; that was
// the harness, -OLShots on the joiner: AOutlawsLevelActor::BeginPlay's bTestRun).  A second machine joins a game already under way
// and does what a player does, on the real rows:
//   a match's host: -OLShots -OLMpHost=<map> -OLMpClass=dm -OLMpBots=1 -OLMpStartWhen=0 -OLMpMinutes=10 -OLMpFrags=0 -OLLateJoinProbe
//   a co-op host: -OLShots -OLCoopHost -OLCoopLevel=<level> -OLCoopReady -OLLateJoinProbe (it starts alone 2 s after its page is up)
//   the joiner: -OLShots -OLCoopAuto -OLLobbyEnterAfter=999 -OLCoopJoin=<host>:<port> [-OLMenuWalk=comp|coop] -OLLateJoinProbe (the
//   walk: Join pressed on the title menu's pages, a player's way in; without it, the launch's join, a test's only.  -OLCoopAuto:
//   co-op's tick before a session is a test run's; -OLLobbyEnterAfter holds its own Enter off, this probe presses the row)
// The joiner lands on the lobby page, nothing built yet, with the host's game running.  A ray from the eyes (the desktop's pointer)
// and one from a right hand's place (the headset's laser) each find ENTER GAME, and it is pressed as the click and the trigger
// press it (MenuActivate).  Then the host's level is built here, the menu is gone, the player stands in the level and the host is
// drawn.  The host sees the joiner in the game, drawn, and waits for it to leave.  Lines "OLSHOT latejoin check PASS|FAIL: ..." and
// "OLSHOT latejoin RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsCoop.h"
#include "OutlawsRemotePlayer.h"
#include "OutlawsFlags.h"
#include "OutlawsMenuDoc.h"
#include "OutlawsShotTests.h"
#include "Camera/CameraComponent.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsLateJoinProbeLocal
{
	int32 Stage = 0, Passes = 0, Fails = 0, Joiner = 0;
	float StageT = 0.f, PageT = 0.f;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT latejoin check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}
}

OL_SHOT_MATCH(OLLateJoinProbe) { return OLFlags::Param(TEXT("OLLateJoinProbe")); }

OL_SHOT_TEST(OLLateJoinProbe, 2320)
{
	using namespace OutlawsLateJoinProbeLocal;
	if (!L || ShotTime < 0.f || Stage < 0) return;
	UOutlawsCoop* C = UOutlawsCoop::Get(this);
	AOutlawsCoopSession* S = C ? C->GetSession() : nullptr;
	StageT += Dt;
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT latejoin RESULT %s (%d passed, %d failed; %s, %s, %s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			C && C->IsHost() ? TEXT("the host") : TEXT("the joiner"), C && C->IsMatch() ? TEXT("a match") : TEXT("co-op"), C && !C->MenuWalk.IsEmpty() ? *C->MenuWalk : TEXT("launched"));
		ShotTime = -1.f;
		Stage = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	auto Next = [&](int32 To) { Stage = To; StageT = 0.f; };
	if (!C || !S)
	{
		// (the title menu, the walk, the connection)
		if (StageT > 150.f) { Check(false, TEXT("no session came up")); Finish(); }
		return;
	}
	// ---- the host: its game goes on alone until the joiner is in it ----
	if (C->IsHost())
	{
		if (Stage == 0)
		{
			if (!C->IsMatch() && S->StartSerial == 0)
			{
				if (bMenuOpen && StageT > 2.f) C->StartGame(true);
				return;
			}
			TArray<AOutlawsCoopLink*> Links;
			C->GetLinks(Links);
			for (const AOutlawsCoopLink* K : Links)
			{
				if (K->bBot || K->PlayerIndex <= 0 || !C->IsPlayerEntered(K) || K->BuiltSerial != C->StartedSerial) continue;
				Joiner = K->PlayerIndex;
				Check(true, FString::Printf(TEXT("player %d came in to the game under way (level %s, serial %d)"), Joiner, *S->Level, S->StartSerial));
				Next(1);
				return;
			}
			if (StageT > 240.f) { Check(false, TEXT("no joiner came in")); Finish(); }
			return;
		}
		if (Stage == 1)
		{
			if (const AOutlawsRemotePlayer* R = C->PuppetOf(Joiner))
			{
				Check(true, FString::Printf(TEXT("the joiner is drawn here, %.0f cm from the host"), FVector::Dist(R->GetActorLocation(), GetActorLocation())));
				Next(2);
			}
			else if (StageT > 20.f) { Check(false, TEXT("no puppet of the joiner here in 20 s")); Finish(); }
			return;
		}
		if (!C->GetLink(Joiner) || StageT > 40.f) Finish();     // (the joiner ends first: it reports with the session still up)
		return;
	}
	// ---- the joiner ----
	switch (Stage)
	{
	case 0:
	{
		if (CoopMenuPage() != TEXT("#cooplobby") || S->StartSerial <= 0 || C->MyIndex() <= 0)
		{
			PageT = 0.f;
			if (StageT > 150.f) { Check(false, FString::Printf(TEXT("no lobby page of a game under way in 150 s (page '%s', serial %d, player %d)"), *CoopMenuPage(), S->StartSerial, C->MyIndex())); Finish(); }
			return;
		}
		PageT += Dt;
		if (PageT < 3.f) return;
		Check(C->InLobbyHere() && !C->bLevelBuilt, FString::Printf(TEXT("the lobby page is up over the host's game (%s, serial %d), this machine in the lobby %d, nothing built here %d"), *S->Level, S->StartSerial,
			C->InLobbyHere() ? 1 : 0, C->bLevelBuilt ? 0 : 1));
		const int32 Item = MenuPage ? MenuPage->Items.IndexOfByPredicate([](const OutlawsMenu::FItem& I) { return I.Name == TEXT("coop:enter"); }) : INDEX_NONE;
		const TPair<FBox2D, int32>* Hit = FrontHits.FindByPredicate([Item](const TPair<FBox2D, int32>& H) { return H.Value == Item; });
		if (Item == INDEX_NONE || !Hit || !MenuRoot || !Camera)
		{
			Check(false, FString::Printf(TEXT("the page has ENTER GAME to point at (item %d, %d laser boxes)"), Item, FrontHits.Num()));
			Finish();
			return;
		}
		// (the ray checks of the title's own test, AOutlawsPlayer's menu probe: panel centre, 1920 x 1080 view on the sheet)
		const FVector2D Mid = Hit->Key.GetCenter();
		const FVector At = MenuRoot->GetComponentTransform().TransformPosition(FVector(0.0, (0.5 - Mid.X / 1920.0) * SheetW, SheetH * 0.5 - Mid.Y / 1080.0 * SheetH));
		const FVector Eye = Camera->GetComponentLocation();
		const FVector Hand = Eye + Camera->GetForwardVector() * 25.0 + Camera->GetRightVector() * 20.0 - Camera->GetUpVector() * 35.0;
		float Len = 0.f;
		const int32 ByEye = MenuRowUnderRay(Eye, (At - Eye).GetSafeNormal(), Len);
		const int32 ByHand = MenuRowUnderRay(Hand, (At - Hand).GetSafeNormal(), Len);
		Check(ByEye == Item, FString::Printf(TEXT("the desktop's ray finds %s (item %d, found %d)"), *MenuPage->Items[Item].Label, Item, ByEye));
		Check(ByHand == Item, FString::Printf(TEXT("a hand's laser finds %s (item %d, found %d)"), *MenuPage->Items[Item].Label, Item, ByHand));
		MenuActivate(1, Item);
		Check(C->bEnterPressed, TEXT("the press takes: this player asks for the host's game"));
		Next(1);
		return;
	}
	case 1:
		if (!C->bLevelBuilt || C->StartedSerial != S->StartSerial || L->IsRebuildPending() || L->IsFrontEnd() || bMenuOpen)
		{
			if (StageT > 90.f) { Check(false, FString::Printf(TEXT("not in the game in 90 s (built %d, serial %d of %d, page '%s')"), C->bLevelBuilt ? 1 : 0, C->StartedSerial, S->StartSerial, *CoopMenuPage())); Finish(); }
			return;
		}
		Check(L->LevelName.Equals(S->Level, ESearchCase::IgnoreCase) && !bDeadView, FString::Printf(TEXT("in the game %.1f s after the press: level %s (the host's %s), the menu gone, standing at %s"), StageT, *L->LevelName,
			*S->Level, *GetActorLocation().ToCompactString()));
		Next(2);
		return;
	case 2:
		if (const AOutlawsRemotePlayer* R = C->PuppetOf(0))
		{
			Check(true, FString::Printf(TEXT("the host is drawn here, %.0f cm away"), FVector::Dist(R->GetActorLocation(), GetActorLocation())));
			Next(3);
		}
		else if (StageT > 20.f) { Check(false, TEXT("no puppet of the host here in 20 s")); Finish(); }
		return;
	default:
		if (StageT > 3.f) Finish();
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
