// REQ-MPSPEED's probe (user 4 Nov: on Host Game the host picks ONE movement speed for the match, every player is held to it, the
// menus' row greyed with the host's value; matches only, co-op as it was).  The settings file says 150 %.  Machines:
//   host:   -OLMpHost=town_2p -OLMpClass=dm -OLMpBots=0 -OLMpChar=Doc -OLMpMinutes=30 -OLMpFrags=0 -OLMpMoveSpeed=70 -OLMpSpeedProbe -OLMpSpeedLate
//   client: -OLCoopJoin=<host>:<port> -OLMpChar=Mar -OLMpSpeedOwn=150 -OLMpSpeedProbe
//   late:   the same a minute on, -OLCoopSpeed=0.6 -OLMpSpeedOwn=120, joining the match under way
// Each client: held to the host's 70 % (the pace, and the run speed measured beside the host's), its own dial untouched, the row
// "Movement speed 70% (set by host)" greyed and a press on it moving nothing, a save during the match leaving the file's dial as it
// was, a client's claim of the top pace counting for nothing on the host.  The first then leaves: its dial 150 % again, read from
// the file by the level it goes back to.
// -OLOldMpSpeed on all (the host -OLCoopSpeed=0.35, no late joiner): each at its own, the row live, the Host Game row hidden.
// -OLMpSpeedProbe=coop, a co-op pair (-OLCoopHost -OLCoopLevel=hideout -OLCoopSpeed=0.35): nothing held, each at its own, the row live.
// Lines "OLSHOT mpspeed check PASS|FAIL (host|client): ..." and "OLSHOT mpspeed <host|client> RESULT".

#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsDeathmatch.h"
#include "OutlawsEnemy.h"
#include "OutlawsFlags.h"
#include "OutlawsGeometry.h"
#include "OutlawsLevelActor.h"
#include "OutlawsMpHost.h"
#include "OutlawsPlayer.h"
#include "OutlawsShotTests.h"
#include "Containers/Ticker.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsMpSpeedProbeLocal
{
	constexpr float WalkFor = 1.6f;
	constexpr float HostOwnDial = 0.35f;      // (-OLOldMpSpeed and co-op: the host's own 70 %)
	int32 Passes = 0, Fails = 0, Round = 0, First = -1, Target = -1;
	float WalkTop = 0.f, IniAtStart = -2.f;
	double WalkUntil = 0.0, LeftAt = 0.0;
	FVector Home = FVector::ZeroVector;

	void Check(bool bOk, const TCHAR* Who, const FString& What)
	{
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT mpspeed check %s (%s): %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), Who, *What);
		if (bOk) ++Passes; else ++Fails;
	}
	void Result(const TCHAR* Who)
	{
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT mpspeed %s RESULT %s (%d passed, %d failed)"), Who, Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
	}
	/** The dial OutlawsSettings.ini holds (-1: none). */
	float IniDial()
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *(FPaths::ProjectSavedDir() / TEXT("OutlawsSettings.ini")))) return -1.f;
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines);
		for (const FString& Ln : Lines) if (Ln.StartsWith(TEXT("MovementSpeed="))) return FCString::Atof(*Ln.Mid(14));
		return -1.f;
	}
	bool Own() { return OLFlags::Param(TEXT("OLOldMpSpeed")) || OLFlags::ValueIs(TEXT("OLMpSpeedProbe"), TEXT("coop")); }
	float PaceOf(float Dial) { return AOutlawsLevelActor::SpeedFromDial(Dial) * 0.5f; }
	void SampleWalk(AOutlawsPlayer* P) { WalkTop = FMath::Max(WalkTop, float(P->GetVelocity().Size2D())); }

	/** The pause menu's row, Want the host's value as shown (0: nothing held, the player's own, live). */
	void CheckRow(AOutlawsLevelActor* L, AOutlawsPlayer* P, const TCHAR* Who, int32 Want)
	{
		FString Row;
		bool bGrey = false;
		const float Dial = L->GameSpeed;
		const bool bHave = P->MenuPageTestRow(TEXT("#vr"), TEXT("vr_speed"), Row, bGrey);
		if (Want > 0)
		{
			Check(bHave && bGrey && Row == FString::Printf(TEXT("Movement speed %d%% (set by host)"), Want), Who, FString::Printf(TEXT("the row reads '%s', %s"), *Row, bGrey ? TEXT("greyed") : TEXT("live")));
			P->VrSpeedMenuTestPress();
			Check(L->GameSpeed == Dial && L->HeldGameSpeed > 0.f, Who, FString::Printf(TEXT("a press on it moves nothing (own dial %.2f, held %.2f)"), L->GameSpeed, L->HeldGameSpeed));
		}
		else
		{
			Check(bHave && !bGrey && Row.Contains(TEXT(":")) && !Row.Contains(TEXT("host")), Who, FString::Printf(TEXT("the row reads '%s', %s"), *Row, bGrey ? TEXT("greyed") : TEXT("live")));
			P->VrSpeedMenuTestPress();
			Check(L->GameSpeed != Dial, Who, FString::Printf(TEXT("a press steps it (%.2f -> %.2f)"), Dial, L->GameSpeed));
			L->GameSpeed = Dial;
			P->CoopSaveSettings();
		}
		P->CheatsMenuTestClose();
	}
}

// ---- the host -------------------------------------------------------------------------------------------------------------

void UOutlawsCoop::TickHostMpSpeedTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	using namespace OutlawsMpSpeedProbeLocal;
	FOutlawsMatchFlow& F = OutlawsMpHost::Flow();
	const bool bCoop = OLFlags::ValueIs(TEXT("OLMpSpeedProbe"), TEXT("coop"));
	const bool bOld = OLFlags::Param(TEXT("OLOldMpSpeed"));
	const int32 Want = Own() ? 0 : FMath::RoundToInt(LaunchMoveShown);
	const AOutlawsCoopSession* S = GetSession();
	L->bInvincible = true;
	auto Mine = [](bool bOk, const FString& What) { Check(bOk, TEXT("host"), What); };
	auto Find = [this](int32 Not)
	{
		// a human client whose level is up, in the match's rules if a match
		TArray<AOutlawsCoopLink*> Links;
		GetLinks(Links);
		for (const AOutlawsCoopLink* K : Links)
		{
			const int32 Who = K->PlayerIndex;
			if (Who > 0 && Who < MaxPlayers && Who != Not && !K->bBot && K->BuiltSerial == StartedSerial && (!IsMatch() || MatchInRules[Who])) return Who;
		}
		return -1;
	};
	auto Finish = [&]()
	{
		Result(TEXT("host"));
		TestStep = 2208;
		TestAt = Now + 2.0;
	};
	if (TestStep < 2200) { TestStep = 2200; TestAt = 0.0; }
	switch (TestStep)
	{
	case 2200:
		if (TestAt == 0.0) TestAt = Now + 240.0;
		First = Target = Find(-1);
		if ((First < 0 || (!bCoop && !bMatchSetUp)) && Now < TestAt) return;
		Mine(First > 0, FString::Printf(TEXT("the client is in (%s, %s)"), bCoop ? TEXT("a co-op pair") : TEXT("a match"), bOld ? TEXT("-OLOldMpSpeed") : TEXT("new")));
		if (First < 0) { Finish(); return; }
		if (!bCoop) F.HostStart();
		TestStep = 2201;
		return;
	case 2201:
		if (!bCoop && F.GetState() != FOutlawsMatchFlow::EState::Playing) return;
		Home = P->GetActorLocation();
		TestAt = Now + 3.0;
		TestStep = 2202;
		return;
	case 2202:
	{
		if (Now < TestAt) return;
		if (Round == 0)
		{
			Mine(IsMatch() != bCoop, FString::Printf(TEXT("IsMatch %d"), IsMatch() ? 1 : 0));
			Mine(Want > 0 ? FMath::IsNearlyEqual(L->HeldGameSpeed, float(Want) / 200.f, 0.001f) && S && FMath::IsNearlyEqual(S->MatchMoveDial, L->HeldGameSpeed, 0.001f)
				: L->HeldGameSpeed < 0.f && S && S->MatchMoveDial == 0.f, FString::Printf(TEXT("held %.3f, on the session %.3f, own dial %.3f"), L->HeldGameSpeed, S ? S->MatchMoveDial : -1.f, L->GameSpeed));
			Mine(FMath::IsNearlyEqual(L->Pace(), PaceOf(Want > 0 ? float(Want) / 200.f : L->GameSpeed), 0.001f), FString::Printf(TEXT("pace %.3f"), L->Pace()));
			CheckRow(L, P, TEXT("host"), Want);
			FString Row;
			bool bGrey = false;
			const bool bRow = P->MenuPageTestRow(TEXT("#mphost"), TEXT("mphost_speed"), Row, bGrey);
			P->CheatsMenuTestClose();
			if (!bCoop) Mine(bRow == !bOld, FString::Printf(TEXT("Host Game's row: %s"), bRow ? *Row : TEXT("hidden")));
		}
		TestSayTo(Target, TEXT("ms:check"), Want);
		P->CoopTestRun(true);
		TestWalkOpen(P, WalkFor);
		TestSayTo(Target, TEXT("ms:walk"), 0);
		WalkUntil = Now + WalkFor;
		WalkTop = 0.f;
		TestStep = 2203;
		return;
	}
	case 2203:
		SampleWalk(P);
		if (Now < WalkUntil + 0.3) return;
		P->CoopTestRun(false);
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT mpspeed host's run %.0f cm/s (pace %.3f)"), WalkTop, L->Pace());
		TestSayTo(Target, TEXT("ms:peer"), FMath::RoundToInt(WalkTop));
		P->SetActorLocation(Home, false, nullptr, ETeleportType::TeleportPhysics);
		TestAt = Now + 2.5;
		TestStep = 2204;
		return;
	case 2204:
		if (Now < TestAt) return;
		TestSayTo(Target, TEXT("ms:row"), Want);
		TestSayTo(Target, TEXT("ms:claim"), 0);
		TestAt = Now + 2.5;
		TestStep = 2205;
		return;
	case 2205:
	{
		if (Now < TestAt) return;
		if (Want > 0)
		{
			const AOutlawsCoopLink* K = GetLink(Target);
			Mine(K && K->Pace > L->Pace() + 0.01f && FMath::IsNearlyEqual(L->CoopWaitPace, L->Pace(), 0.001f),
				FString::Printf(TEXT("the client claims pace %.3f: the host's stays, doors at %.3f, pace %.3f"), K ? K->Pace : -1.f, L->CoopWaitPace, L->Pace()));
		}
		if (Round == 0 && OLFlags::Param(TEXT("OLMpSpeedLate")))
		{
			Round = 1;
			TestAt = Now + 150.0;
			TestStep = 2206;
			return;
		}
		if (Round == 0) Result(TEXT("host"));        // (a pair: the session ends as the client leaves)
		TestSayTo(First, TEXT("ms:leave"), 0);
		TestAt = Now + 20.0;
		TestStep = 2207;
		return;
	}
	case 2206:
		// the late joiner, into the match under way
		Target = Find(First);
		if (Target < 0 && Now < TestAt) return;
		Mine(Target > 0 && F.GetState() == FOutlawsMatchFlow::EState::Playing, TEXT("a late joiner came in with the match under way"));
		if (Target < 0) { Finish(); return; }
		TestAt = Now + 4.0;
		TestStep = 2202;
		return;
	case 2207:
		if (Now < TestAt) return;
		if (Round == 1) { TestSayTo(Target, TEXT("ms:end"), 0); Finish(); }
		else { TestStep = 2208; TestAt = Now + 2.0; }
		return;
	case 2208:
		if (Now < TestAt) return;
		TestSay(TEXT("quit"));
		TestAt = Now + 3.0;
		TestStep = 2209;
		return;
	case 2209:
		if (Now < TestAt) return;
		TestStep = 2210;
		FPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

// ---- a client ---------------------------------------------------------------------------------------------------------------

void UOutlawsCoop::TickClientMpSpeedTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	using namespace OutlawsMpSpeedProbeLocal;
	if (WalkUntil == 0.0) return;
	SampleWalk(P);
	if (Now < WalkUntil + 0.3) return;
	WalkUntil = 0.0;
	P->CoopTestRun(false);
	UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT mpspeed client's run %.0f cm/s (pace %.3f, own dial %.3f)"), WalkTop, L->Pace(), L->GameSpeed);
}

void UOutlawsCoop::OnMpSpeedTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	using namespace OutlawsMpSpeedProbeLocal;
	auto Mine = [](bool bOk, const FString& What) { Check(bOk, TEXT("client"), What); };
	int32 OwnShown = 0;
	OLFlags::Value(TEXT("OLMpSpeedOwn"), OwnShown);
	if (Step == TEXT("check"))
	{
		// Arg: the host's value as shown (0: nothing held)
		L->bInvincible = true;
		IniAtStart = IniDial();
		Mine(FMath::RoundToInt(L->GameSpeed * 200.f) == OwnShown, FString::Printf(TEXT("own dial %.0f %%, as this client has it (the file %.3f)"), L->GameSpeed * 200.f, IniAtStart));
		Mine(Arg > 0 ? FMath::IsNearlyEqual(L->HeldGameSpeed, float(Arg) / 200.f, 0.001f) : L->HeldGameSpeed < 0.f, FString::Printf(TEXT("held %.3f (the host says %d %%)"), L->HeldGameSpeed, Arg));
		Mine(FMath::IsNearlyEqual(L->Pace(), PaceOf(Arg > 0 ? float(Arg) / 200.f : L->GameSpeed), 0.001f), FString::Printf(TEXT("pace %.3f"), L->Pace()));
	}
	else if (Step == TEXT("walk"))
	{
		P->CoopTestRun(true);
		TestWalkOpen(P, WalkFor);
		WalkUntil = FPlatformTime::Seconds() + WalkFor;
		WalkTop = 0.f;
	}
	else if (Step == TEXT("peer"))
	{
		// Arg: the host's top speed, cm/s
		const float Ratio = WalkTop / FMath::Max(1.f, float(Arg));
		const float WantRatio = Own() ? AOutlawsLevelActor::SpeedFromDial(L->GameSpeed) / AOutlawsLevelActor::SpeedFromDial(HostOwnDial) : 1.f;
		Mine(FMath::Abs(Ratio - WantRatio) < 0.12f * WantRatio, FString::Printf(TEXT("the run: %.0f cm/s here, the host's %d: %.2f x, wanted %.2f x"), WalkTop, Arg, Ratio, WantRatio));
	}
	else if (Step == TEXT("row"))
	{
		CheckRow(L, P, TEXT("client"), Arg);
	}
	else if (Step == TEXT("claim"))
	{
		// a save in the match, and (held) a claim of the top pace
		P->CoopSaveSettings();
		Mine(FMath::IsNearlyEqual(IniDial(), IniAtStart, 0.001f), FString::Printf(TEXT("a save in the match: the file's dial %.3f, as it was (%.3f)"), IniDial(), IniAtStart));
		if (L->HeldGameSpeed >= 0.f)
		{
			if (AOutlawsCoopLink* Link = GetLink(MyIndex())) Link->ServerPace(AOutlawsLevelActor::MaxPace());
		}
	}
	else if (Step == TEXT("leave"))
	{
		LeftAt = FPlatformTime::Seconds();
		FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateWeakLambda(this, [this, OwnShown](float)
		{
			if (FPlatformTime::Seconds() - LeftAt < 12.0) return true;
			AOutlawsLevelActor* Lv = FindLevel();
			Check(Lv && !IsMatch() && Lv->HeldGameSpeed < 0.f && FMath::RoundToInt(Lv->GameSpeed * 200.f) == OwnShown && FMath::IsNearlyEqual(Lv->Pace(), PaceOf(Lv->GameSpeed), 0.001f), TEXT("client"),
				FString::Printf(TEXT("after leaving: own dial %.0f %%, nothing held (%.2f), pace %.3f"), Lv ? Lv->GameSpeed * 200.f : -1.f, Lv ? Lv->HeldGameSpeed : -1.f, Lv ? Lv->Pace() : -1.f));
			Check(FMath::IsNearlyEqual(IniDial(), IniAtStart, 0.001f), TEXT("client"), FString::Printf(TEXT("after leaving: the file's dial %.3f, as it was"), IniDial()));
			Result(TEXT("client"));
			FPlatformMisc::RequestExit(false);
			return false;
		}), 1.f);
		LeaveSession(this, TEXT("REQ-MPSPEED's probe"));
	}
	else if (Step == TEXT("end"))
	{
		Result(TEXT("client"));
	}
}

// ---- the bots (user 4 Nov: "with multiplayer, bots should just match the top movement speeds set by the host") --------------
// -OLShots -OLMpHost=<map> -OLMpClass=dm -OLMpBots=7 -OLMpBotSkill=<0..2> -OLMpMinutes=10 -OLMpFrags=0 [-OLMpMoveSpeed=<40..150>]
// -OLMpSpeedBotProbe [-OLMpSpeedBotSeconds=<90>] [-OLOldMpSpeed].  Each bot's top speed a player's full run at the match's speed, its
// walk 0.8 of that, whatever its skill; the row left alone: the host's own dial.  Its fastest on the ground is measured, and how far
// each goes, with the routes' tallies, for the low end (a bot has no jump: its routes are steps and drops).  -OLOldMpSpeed: its own feet.
// Lines "OLSHOT mpspeed check PASS|FAIL (bots): ...", "OLSHOT mpspeed bots map ..." and "OLSHOT mpspeed bots RESULT".

namespace OutlawsMpSpeedProbeLocal
{
	struct FBotWatch { FVector Last = FVector::ZeroVector; double Moved = 0.0; float Top = 0.f; };
	FBotWatch BotWatch[16];
	int32 BotStage = 0, RoutesWas = 0, NoWayWas = 0, StucksWas = 0, FreedWas = 0;
	double BotsFrom = 0.0;

	void BotTallies(const UOutlawsCoop* C, int32& Routes, int32& NoWay, int32& Stucks)
	{
		TArray<UOutlawsCoop::FBotProbeView> Views;
		C->DebugBotViews(Views);
		Routes = NoWay = Stucks = 0;
		for (const UOutlawsCoop::FBotProbeView& V : Views) { Routes += V.Routes; NoWay += V.RouteFails; Stucks += V.Stucks; }
	}
}

OL_SHOT_MATCH(OLMpSpeedBotProbe) { return OLFlags::Param(TEXT("OLMpSpeedBotProbe")); }

OL_SHOT_TEST(OLMpSpeedBotProbe, 2209)
{
	using namespace OutlawsMpSpeedProbeLocal;
	if (!L || BotStage < 0) return;
	L->StartSaveDelay = -1.f;           // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	L->bEnemiesActive = true;           // (ShotTest stills them each tick: the bots' brains are bad guys')
	L->bInvincible = true;
	UOutlawsCoop* C = UOutlawsCoop::Get(this);
	const bool bOld = OLFlags::Param(TEXT("OLOldMpSpeed"));
	const double Now = FPlatformTime::Seconds();
	static double StartAt = Now;
	int32 Seconds = 90;
	OLFlags::Value(TEXT("OLMpSpeedBotSeconds"), Seconds);
	auto Mine = [](bool bOk, const FString& What) { Check(bOk, TEXT("bots"), What); };
	auto Finish = [&]()
	{
		Result(TEXT("bots"));
		BotStage = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	TArray<UOutlawsCoop::FBotProbeView> Views;
	if (C) C->DebugBotViews(Views);
	if (BotStage == 0)
	{
		const bool bUp = C && C->IsHost() && C->IsMatch() && C->bLevelBuilt && !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay();
		FOutlawsMatchFlow& F = OutlawsMpHost::Flow();
		if (bUp && Views.Num() > 0 && F.GetState() == FOutlawsMatchFlow::EState::WarmUp) F.HostStart();     // (the match started, as the user's games)
		if (bUp && Views.Num() > 0 && F.GetState() == FOutlawsMatchFlow::EState::Playing)
		{
			BotTallies(C, RoutesWas, NoWayWas, StucksWas);
			FreedWas = UOutlawsCoop::DebugBotsFreed();
			BotsFrom = Now + 2.0;
			BotStage = 1;
			return;
		}
		if (Now - StartAt < 180.0) return;
		Mine(false, TEXT("needs a launched host with bots: -OLMpHost=<map> -OLMpClass=dm -OLMpBots=7"));
		Finish();
		return;
	}
	if (Now < BotsFrom) return;
	if (BotStage == 1 && Now - BotsFrom < double(Seconds))
	{
		for (const UOutlawsCoop::FBotProbeView& V : Views)
		{
			if (V.Who < 0 || V.Who >= UE_ARRAY_COUNT(BotWatch)) continue;
			FBotWatch& W = BotWatch[V.Who];
			const AOutlawsEnemy* E = V.bDead ? nullptr : V.Brain.Get();
			if (!E) { W.Last = FVector::ZeroVector; continue; }
			const FVector At = E->GetActorLocation();
			if (!W.Last.IsZero() && FVector::Dist2D(At, W.Last) < 400.0) W.Moved += FVector::Dist2D(At, W.Last);
			W.Last = At;
			if (E->GetCharacterMovement()->IsMovingOnGround()) W.Top = FMath::Max(W.Top, float(E->GetVelocity().Size2D()));
		}
		return;
	}
	int32 Shown = 0, Skill = -1;
	OLFlags::Value(TEXT("OLMpMoveSpeed"), Shown);
	OLFlags::Value(TEXT("OLMpBotSkill"), Skill);
	const float Dial = bOld ? -1.f : Shown > 0 ? Shown / 200.f : L->GameSpeed;
	Mine(FMath::IsNearlyEqual(L->HeldGameSpeed, Dial, 0.001f), FString::Printf(TEXT("the match's speed %.0f %% (%s), own dial %.0f %%"), L->HeldGameSpeed * 200.f, Shown > 0 ? TEXT("the row") : TEXT("the row left alone"), L->GameSpeed * 200.f));
	const float Top = VRRunSpeed * L->Pace();
	float Fastest = 0.f, Set = 0.f;
	double Most = 0.0, Least = -1.0;
	bool bTop = Views.Num() > 0, bShare = true;
	for (const UOutlawsCoop::FBotProbeView& V : Views)
	{
		if (V.Who < 0 || V.Who >= UE_ARRAY_COUNT(BotWatch)) continue;
		const FBotWatch& W = BotWatch[V.Who];
		Fastest = FMath::Max(Fastest, W.Top);
		Most = FMath::Max(Most, W.Moved);
		Least = Least < 0.0 ? W.Moved : FMath::Min(Least, W.Moved);
		const AOutlawsEnemy* E = V.Brain.Get();
		if (!E) continue;
		Set = E->Def.RunSpeed * float(FOutlawsGeometry::UnitsToCm);
		bTop &= bOld ? Set > 500.f : FMath::IsNearlyEqual(Set, Top, 1.f);
		bShare &= FMath::IsNearlyEqual(E->Def.WalkSpeed, E->Def.RunSpeed * 0.8f, 0.01f);
	}
	Mine(bTop, bOld ? FString::Printf(TEXT("-OLOldMpSpeed: its own feet, %.0f cm/s"), Set) : FString::Printf(TEXT("each bot's top %.0f cm/s, a player's full run at the match's speed (%.0f), skill %d"), Set, Top, Skill));
	Mine(bShare, TEXT("each bot's walk 0.8 of its top"));
	if (!bOld) Mine(Fastest >= Top * 0.75f && Fastest <= Top * 1.05f, FString::Printf(TEXT("the fastest on the ground %.0f cm/s of %.0f"), Fastest, Top));
	Mine(Least * 100.0 > 0.1 * FMath::Max(Set, Top) * Seconds, FString::Printf(TEXT("each bot gets about: %.0f m the least, %.0f m the most"), Least / 100.0, Most / 100.0));
	int32 Routes = 0, NoWay = 0, Stucks = 0;
	BotTallies(C, Routes, NoWay, Stucks);
	UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT mpspeed bots map %s, %d bots, skill %d, match %.0f %%: top %.0f, fastest %.0f cm/s, moved %.0f..%.0f m in %d s; routes %d, no way %d, stucks %d, shut-in moves %d"),
		*L->LevelName, Views.Num(), Skill, L->HeldGameSpeed * 200.f, Set, Fastest, Least / 100.0, Most / 100.0, Seconds, Routes - RoutesWas, NoWay - NoWayWas, Stucks - StucksWas, UOutlawsCoop::DebugBotsFreed() - FreedWas);
	Finish();
}

#endif // WITH_OUTLAWS_TESTS
