// M1's gaps (PLANS.md "M1. Skeleton" item 4): in a match, the blows other than a gun's pellets reach the other players as the
// pellets do (OutlawsCoopMatch.cpp, ShotAtPlayers): the striking machine finds the other player's body drawn there, reports
// the hit to the host (ServerPlayerHit: the host checks it and deals it through the co-op hurt, the kill credited to the
// striker), and it is never dealt twice.
// - Melee: a knife's jab, a fist, a bottle or a pot smashed over a head (StrikeAtPlayers: a point and a reach against the
//   body, or in the player's picture as drawn), a shovel or a crowbar (StrikeAlongAtPlayers: the tool's striking length so);
//   each striker's blow lands on one player once in 0.4 s.
// - Thrown: a knife, a bottle, a pot (SweepAtPlayers: the flight's step against the body).
// - Dynamite: the blast's fall-off (full within a quarter of the radius, then with the square) on each other player in its
//   reach and in sight, from the machine whose stick went off (BlastAtPlayers); a blast told from another machine no longer
//   hurts this machine's own player in a match (the host deals it), only throws it.
// - The characters' DAMAGE_MULTIPLIER (cidsanch 1.15 ... cidmary .80): applied to what a player takes in a match (INFERRED:
//   the ITMs do not say which way; the heavier hitters' builds take more), on the host as it deals the hit (MatchDamageFor).
#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsDeathmatch.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsRemotePlayer.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsMpPolish.h"
#include "TimerManager.h"
#include "OutlawsMpHost.h"
#include "Camera/CameraComponent.h"
#include "Engine/World.h"
#include "Misc/Paths.h"
#include "HAL/IConsoleManager.h"

// the switches (RC1: a feature not green ships off)
TAutoConsoleVariable<int32> CVarOLMatchStrikes(TEXT("ol.MatchStrikes"), 1, TEXT("Matches: melee, thrown things and dynamite hurt other players (1), or only gun pellets (0)."));
TAutoConsoleVariable<int32> CVarOLMatchItems(TEXT("ol.MatchItems"), 1, TEXT("Matches: the map's pickup generators put their items out and back on their timers (1), or none (0)."));

namespace
{
	constexpr double StrikeCoolSeconds = 0.4;
	double GStrikeAt[UOutlawsCoop::MaxPlayers] = {};
	int32 GStrikesSent = 0, GBlastsSent = 0;
	float GLastStrikeDamage = 0.f;

	/** The point on segment A..B nearest the body's upright axis, and how far from it (the body: Mid, Half up and down). */
	double SegmentToBody(const FVector& A, const FVector& B, const FVector& Mid, double Half, FVector& OutAt, double& OutT)
	{
		const FVector Seg = B - A;
		const double Len = Seg.Size();
		const FVector Dir = Len > 1e-3 ? Seg / Len : FVector::ForwardVector;
		const FVector W0 = A - Mid;
		const double Bz = Dir.Z, Dd = FVector::DotProduct(Dir, W0), E = W0.Z;
		const double Den = 1.0 - Bz * Bz;
		double T = Den > 1e-6 ? (Bz * E - Dd) / Den : -Dd;
		T = FMath::Clamp(T, 0.0, Len);
		OutAt = A + Dir * T;
		OutT = T;
		const FVector OnAxis(Mid.X, Mid.Y, Mid.Z + FMath::Clamp(double(OutAt.Z - Mid.Z), -Half, Half));
		return FVector::Dist(OutAt, OnAxis);
	}
}

float UOutlawsCoop::MatchDamageFor(int32 Victim, float Damage) const
{
	const FOutlawsDmPlayer* P = Dm.GetPlayers().FindByPredicate([Victim](const FOutlawsDmPlayer& X) { return X.Id == Victim; });
	const FOutlawsMpCharacter* Ch = P ? FOutlawsMpCharacter::Find(P->Character) : nullptr;
	// (BUG-CHARLOADOUT: the character's ITM as the player's files have it; the table's copy where they lack it)
	const AOutlawsLevelActor* L = Ch ? FindLevel() : nullptr;
	const FOutlawsCharKit& Kit = FOutlawsCharKit::Of(L ? L->GetLabAssets() : nullptr, Ch ? FString(Ch->Short) : FString());
	const float Mult = Ch ? (Kit.bRead ? Kit.DamageMult : Ch->DamageMult) : 1.f;
	if (!FMath::IsNearlyEqual(Mult, 1.f))
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH damage on the %s as %s: %.2f x %.2f = %.2f"), WhoWord(Victim), Ch->Name, Damage, Mult, Damage * Mult);
	}
	return Damage * Mult;
}

bool UOutlawsCoop::ReportStrike(AOutlawsLevelActor* L, int32 Victim, float Damage, const FVector& At, const TCHAR* Kind)
{
	// (the striker's own place as the hit's source: the host's check wants it near where it has the striker)
	AOutlawsPlayer* P = LocalPlayer();
	FVector From = P ? P->GetActorLocation() : At;
	// (REQ-MPBOTS: a bot's stick or knife on the host: the bot's, from where the bot stands)
	const int32 Striker = IsHost() && BlastShooter >= 0 ? BlastShooter : 0;
	if (Striker > 0)
	{
		if (const AOutlawsCoopLink* K = GetLink(Striker)) From = K->Pose.Body;
	}
	++MatchHitsSent;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH %s hits the %s for %.2f (%s)"), Kind, WhoWord(Victim), Damage, IsHost() ? TEXT("the host deals it") : TEXT("reported to the host"));
	L->SpawnBlood(At, (At - From).GetSafeNormal(), 2);
	const bool bBlast = FCString::Strcmp(Kind, TEXT("dynamite")) == 0;      // (REQ-KILLFEED: "blew up")
	if (IsHost()) OnMatchHit(Striker, LevelEpoch(), Victim, Damage, At, From, bBlast);
	else if (AOutlawsCoopLink* Mine = GetLink(MyIndex())) Mine->ServerPlayerHit(LevelEpoch(), Victim, Damage, At, From, bBlast);
	return true;
}

int32 UOutlawsCoop::PlayerStruckAlong(const FVector& A, const FVector& B, float Reach, FVector& OutAt) const
{
	// a point every 5 cm from B back to A (one point when A is B): within Reach of the body as before, or in the player's
	// picture as it is drawn here (BUG-SHOVELWHACK: a blow at the drawn head counts, as on a bad guy)
	const int32 N = FMath::Clamp(FMath::CeilToInt(FVector::Dist(A, B) / 5.0), 0, 40);
	for (int32 Who = 0; Who < MaxPlayers; ++Who)
	{
		if (Who == MyIndex()) continue;
		const AOutlawsRemotePlayer* R = PuppetOf(Who);
		if (!R || R->IsHidden() || R->PlateDead()) continue;
		const FVector Mid = R->ShownBody();
		const double Half = FMath::Max(40.0, double(R->ShownHalf()));
		const AOutlawsSpriteActor* Spr = R->ShownSprite();
		for (int32 i = 0; i <= N; ++i)
		{
			const FVector P = N > 0 ? FMath::Lerp(B, A, double(i) / N) : B;
			const bool bBody = FVector::Dist2D(Mid, P) <= 32.0 + Reach && FMath::Abs(P.Z - Mid.Z) <= Half + Reach;
			if (!bBody && !(Spr && Spr->DrawnTouches(P, FMath::Max(22.0, double(Reach))))) continue;
			OutAt = P;
			return Who;
		}
	}
	return -1;
}

bool UOutlawsCoop::StrikePlayer(AOutlawsLevelActor* L, int32 Who, float Damage, const FVector& At, const TCHAR* Kind)
{
	const double Now = FPlatformTime::Seconds();
	if (Now - GStrikeAt[Who] < StrikeCoolSeconds) return true;         // (the same blow, still going through: struck once)
	GStrikeAt[Who] = Now;
	++GStrikesSent;
	GLastStrikeDamage = Damage;
	return ReportStrike(L, Who, Damage, At, Kind);
}

bool UOutlawsCoop::StrikeAtPlayers(AOutlawsLevelActor* L, const FVector& At, float Reach, float Damage, const TCHAR* Kind)
{
	if (CVarOLMatchStrikes.GetValueOnGameThread() == 0) return false;
	UOutlawsCoop* C = L ? Get(L) : nullptr;
	if (!C || !C->IsMatch() || !C->bLevelBuilt || Damage <= 0.f) return false;
	FVector Hit = At;
	const int32 Who = C->PlayerStruckAlong(At, At, Reach, Hit);
	return Who >= 0 && C->StrikePlayer(L, Who, Damage, Hit, Kind);
}

bool UOutlawsCoop::StrikeAlongAtPlayers(AOutlawsLevelActor* L, const FVector& A, const FVector& B, float Reach, float Damage, const TCHAR* Kind,
	TFunctionRef<bool(const FVector&)> Seen)
{
	if (CVarOLMatchStrikes.GetValueOnGameThread() == 0) return false;
	UOutlawsCoop* C = L ? Get(L) : nullptr;
	if (!C || !C->IsMatch() || !C->bLevelBuilt || Damage <= 0.f) return false;
	FVector Hit = B;
	const int32 Who = C->PlayerStruckAlong(A, B, Reach, Hit);
	return Who >= 0 && Seen(Hit) && C->StrikePlayer(L, Who, Damage, Hit, Kind);
}

int32 UOutlawsCoop::MatchStrikesSent()
{
	return GStrikesSent;
}

float UOutlawsCoop::LastMatchStrikeDamage()
{
	return GLastStrikeDamage;
}

bool UOutlawsCoop::SweepAtPlayers(AOutlawsLevelActor* L, const FVector& A, const FVector& B, float Damage, const TCHAR* Kind, FVector& OutAt)
{
	if (CVarOLMatchStrikes.GetValueOnGameThread() == 0) return false;
	UOutlawsCoop* C = L ? Get(L) : nullptr;
	if (!C || !C->IsMatch() || !C->bLevelBuilt || Damage <= 0.f) return false;
	int32 Best = -1;
	double BestT = TNumericLimits<double>::Max();
	for (int32 Who = 0; Who < MaxPlayers; ++Who)
	{
		if (Who == C->MyIndex()) continue;
		const AOutlawsRemotePlayer* R = C->PuppetOf(Who);
		if (!R || R->IsHidden() || R->PlateDead()) continue;
		FVector At;
		double T = 0.0;
		const double Half = FMath::Max(40.0, double(R->ShownHalf()));
		const FVector Mid = R->ShownBody();
		if (SegmentToBody(A, B, Mid, Half, At, T) > 32.0 || FMath::Abs(At.Z - Mid.Z) > Half) continue;
		if (T < BestT) { BestT = T; Best = Who; OutAt = At; }
	}
	if (Best < 0) return false;
	++GStrikesSent;
	return C->ReportStrike(L, Best, Damage, OutAt, Kind);
}

int32 UOutlawsCoop::BlastAtPlayers(AOutlawsLevelActor* L, const FVector& Where, float RadiusCm, float Damage)
{
	if (CVarOLMatchStrikes.GetValueOnGameThread() == 0) return 0;
	UOutlawsCoop* C = L ? Get(L) : nullptr;
	if (!C || !C->IsMatch() || !C->bLevelBuilt || Damage <= 0.f || RadiusCm <= 0.f) return 0;
	const double R = RadiusCm;
	auto Falloff = [R](double D) { return D <= R * 0.25 ? 1.0 : FMath::Square(FMath::Max(0.0, (R - D) / (0.75 * R))); };
	int32 Struck = 0;
	for (int32 Who = 0; Who < MaxPlayers; ++Who)
	{
		if (Who == C->MyIndex()) continue;
		const AOutlawsRemotePlayer* Rp = C->PuppetOf(Who);
		if (!Rp || Rp->IsHidden() || Rp->PlateDead()) continue;
		const FVector Mid = Rp->ShownBody();
		const double D = FVector::Dist(Where, Mid);
		if (D >= R || !L->CanSee(Where, Mid, Rp)) continue;
		const float Dealt = float(Damage * Falloff(D));
		if (Dealt < 0.05f) continue;
		++GBlastsSent;
		++Struck;
		if (BlastShooter >= 0) BlastStruckMask |= 1u << Who;
		C->ReportStrike(L, Who, Dealt, Mid, TEXT("dynamite"));
	}
	return Struck;
}

// ---- the test (-OLCoopTest=dmstrike: the host with -OLMpHost=<map>, two clients joining; -OLCoopAuto on all three) ---------

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TickHostStrikeTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	FOutlawsMatchFlow& F = OutlawsMpHost::Flow();
	TArray<AOutlawsCoopLink*> Links;
	GetLinks(Links);
	TArray<int32> Clients;
	for (const AOutlawsCoopLink* K : Links) if (K->PlayerIndex > 0 && MatchInRules[K->PlayerIndex]) Clients.Add(K->PlayerIndex);
	auto Check = [](bool bOk, const FString& What) { UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH test check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What); };
	static int32 DealtBefore = 0;
	if (TestStep < 800) { TestStep = 800; TestAt = 0.0; }
	switch (TestStep)
	{
	case 800:
		// the warm-up with both clients; the victim to wear Bloody Mary (her multiplier, .80) from the start
		if (Clients.Num() < 2 || !bMatchSetUp) return;
		if (TestAt == 0.0) { TestAt = Now + 6.0; return; }
		if (Now < TestAt) return;
		MatchTestShooter = Clients[0];
		MatchTestVictim = Clients[1];
		Dm.SetPendingCharacter(MatchTestVictim, TEXT("Mar"));
		Check(F.HostStart(), TEXT("the host's Start the game"));
		TestStep = 801;
		return;
	case 801:
		if (F.GetState() != FOutlawsMatchFlow::EState::Playing) return;
		TestStep = 802;
		TestAt = Now + 4.0;                      // (past the spawn protection)
		return;
	case 802:
		if (Now < TestAt) return;
		TestSayTo(MatchTestShooter, TEXT("dm:aim"), MatchTestVictim);
		TestStep = 803;
		TestAt = Now + 1.5;
		return;
	case 803:
	case 804:
	case 805:
		// a fist, a thrown knife, a smash: each reported by the shooter, dealt by the host (the victim's health 10)
		if (Now < TestAt) return;
		DealtBefore = MatchHitsDealt;
		TestSayTo(MatchTestShooter, TestStep == 803 ? TEXT("ds:punch") : TestStep == 804 ? TEXT("ds:throw") : TEXT("ds:smash"), MatchTestVictim);
		TestStep += 10;
		TestAt = Now + 1.0;
		return;
	case 813:
	case 814:
	case 815:
		if (Now < TestAt) return;
		Check(MatchHitsDealt > DealtBefore, FString::Printf(TEXT("%s from the %s dealt on the %s by the host"), TestStep == 813 ? TEXT("a fist") : TestStep == 814 ? TEXT("a thrown knife") : TEXT("a smash"),
			WhoWord(MatchTestShooter), WhoWord(MatchTestVictim)));
		TestStep -= 9;
		TestAt = Now + 0.6;
		return;
	case 806:
		// dynamite by the victim's feet: the kill, the shooter's
		if (Now < TestAt) return;
		TestSayTo(MatchTestShooter, TEXT("ds:blast"), MatchTestVictim);
		TestStep = 807;
		TestAt = Now + 3.0;
		return;
	case 807:
	{
		if (!MatchDead[MatchTestVictim] && Now < TestAt) return;
		const FOutlawsDmPlayer* S = Dm.Find(MatchTestShooter);
		const FOutlawsDmPlayer* V = Dm.Find(MatchTestVictim);
		Check(MatchDead[MatchTestVictim], FString::Printf(TEXT("the %s died to the blows and the blast"), WhoWord(MatchTestVictim)));
		Check(S && S->Frags == 1, FString::Printf(TEXT("the kill is the %s's: %d frags"), WhoWord(MatchTestShooter), S ? S->Frags : -1));
		Check(V && V->Character == TEXT("Mar"), FString::Printf(TEXT("the victim wore %s"), V ? *V->Character : TEXT("?")));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH test: the host's summary: %s"), *MatchSummary());
		// (M4) the shooter's taunts, a key's and the VR stick's long press, heard here; the dead victim follows a player
		DealtBefore = MatchTauntsHeard;
		TestSayTo(MatchTestShooter, TEXT("ds:taunt"), 0);
		TestSayTo(MatchTestVictim, TEXT("ds:spectate"), 0);
		TestStep = 830;
		TestAt = Now + 3.0;
		return;
	}
	case 830:
		if (Now < TestAt) return;
		TestSayTo(MatchTestShooter, TEXT("ds:vrtaunt"), 0);
		TestStep = 831;
		TestAt = Now + 4.0;
		return;
	case 831:
		if (Now < TestAt) return;
		Check(MatchTauntsHeard >= DealtBefore + 2, FString::Printf(TEXT("the %s's two taunts heard by the host (%d)"), WhoWord(MatchTestShooter), MatchTauntsHeard - DealtBefore));
		TestStep = 820;
		return;
	case 820:
	{
		// the map's items: out on the ground; the quickest generator's taken by the host and back after its period
		const TArray<FOutlawsDmItemGen>& Items = Dm.GetItems();
		int32 Out = 0, Quick = -1;
		for (int32 i = 0; i < MatchItemActors.Num(); ++i)
		{
			if (MatchItemActors[i].IsValid()) ++Out;
			if (MatchItemActors[i].IsValid() && (Quick < 0 || Items[i].Period < Items[Quick].Period)) Quick = i;
		}
		Check(Out > Items.Num() / 2, FString::Printf(TEXT("%d of the map's %d generator items on the ground"), Out, Items.Num()));
		const int32 Pi = Quick >= 0 ? PickupIndexOf(L, MatchItemActors[Quick].Get()) : -1;
		Check(Pi >= 0, FString::Printf(TEXT("the quickest generator (%s, %.0f s) has its item out"), Quick >= 0 ? *Items[Quick].Object : TEXT("?"), Quick >= 0 ? Items[Quick].Period : 0.f));
		if (Pi < 0) { TestStep = 822; return; }
		L->CollectPickup(Pi, true);
		MatchTestShots = Quick;
		DealtBefore = MatchItemsBack;
		TestAt = Now + Items[Quick].Period + 3.0;
		TestStep = 821;
		return;
	}
	case 821:
	{
		const TArray<FOutlawsDmItemGen>& Items = Dm.GetItems();
		if (Now < TestAt && !(Items.IsValidIndex(MatchTestShots) && Items[MatchTestShots].bOut))
		{
			static bool bGoneChecked = false;
			if (!bGoneChecked && Items.IsValidIndex(MatchTestShots) && !Items[MatchTestShots].bOut)
			{
				bGoneChecked = true;
				Check(true, FString::Printf(TEXT("%s taken: its generator counts down %.0f s"), *Items[MatchTestShots].Object, Items[MatchTestShots].Period));
			}
			return;
		}
		Check(MatchItemActors.IsValidIndex(MatchTestShots) && MatchItemActors[MatchTestShots].IsValid(),
			FString::Printf(TEXT("%s back on the ground after its period"), Items.IsValidIndex(MatchTestShots) ? *Items[MatchTestShots].Object : TEXT("?")));
		TestStep = 822;
		TestAt = Now + 2.0;
		return;
	}
	case 822:
		if (Now < TestAt) return;
		TestSay(TEXT("quit"));
		TestStep = 100;
		TestAt = Now + 3.0;
		return;
	case 100:
		if (Now < TestAt) return;
		Summary(TEXT("done"));
		QuitForTest();
		TestStep = 101;
		return;
	default:
		return;
	}
}

void UOutlawsCoop::OnStrikeTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	if (Step == TEXT("taunt") || Step == TEXT("vrtaunt") || Step == TEXT("spectate")) { if (L && P) OnStrikeExtraStep(Step, L, P); return; }
	const AOutlawsRemotePlayer* R = PuppetOf(Arg);
	if (!R || !L || !P) { UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH test check FAIL: the %s is not drawn here for %s"), WhoWord(Arg), *Step); return; }
	const FVector Chest = R->ShownBody() + FVector(0.0, 0.0, 20.0);
	const FVector Eye = P->Camera ? P->Camera->GetComponentLocation() : P->GetActorLocation();
	bool bHit = false;
	if (Step == TEXT("punch"))
	{
		// the fist at the chest, as the punch's own check finds it
		bHit = StrikeAtPlayers(L, Chest + (Eye - Chest).GetSafeNormal() * 20.0, 22.f, 0.8f, TEXT("a fist"));
	}
	else if (Step == TEXT("throw"))
	{
		// a knife's flight step through the body
		FVector At;
		const FVector Dir = (Chest - Eye).GetSafeNormal();
		bHit = SweepAtPlayers(L, Chest - Dir * 60.0, Chest + Dir * 60.0, 3.f, TEXT("a thrown knife"), At);
	}
	else if (Step == TEXT("smash"))
	{
		bHit = StrikeAtPlayers(L, Chest + FVector(0.0, 0.0, 40.0), 15.f, 1.2f, TEXT("a smash over the head"));
	}
	else if (Step == TEXT("blast"))
	{
		bHit = BlastAtPlayers(L, R->ShownBody() + FVector(0.0, 0.0, -60.0) + (Eye - Chest).GetSafeNormal2D() * 50.0, L->BlastRadiusCm(), 12.f) > 0;
	}
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH test: the %s's %s at the %s: %s"), WhoWord(MyIndex()), *Step, WhoWord(Arg), bHit ? TEXT("a hit") : TEXT("a miss"));
}
#endif // WITH_OUTLAWS_TESTS

void UOutlawsCoop::OnStrikeExtraStep(const FString& Step, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	auto Check = [](bool bOk, const FString& What) { UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH test check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What); };
	if (Step == TEXT("taunt"))
	{
		// F3 (the key's own path)
		Check(OutlawsMpPolish::Taunt(L, 3), FString::Printf(TEXT("the %s says taunt slot 3"), WhoWord(MyIndex())));
	}
	else if (Step == TEXT("vrtaunt"))
	{
		// the move stick held 0.8 s and let go, as a headset's would be
		const int32 Move = P->LeftDominant() ? 1 : 0;
		P->StickPressed(Move);
		P->StickDownAt[Move] -= 0.8;
		const bool bRun = P->bRunning;
		P->StickReleased(Move);
		Check(P->bRunning == bRun, FString::Printf(TEXT("the %s's long press said a taunt, the run left as it was"), WhoWord(MyIndex())));
	}
	else if (Step == TEXT("spectate"))
	{
		TWeakObjectPtr<UOutlawsCoop> Weak(this);
		FTimerHandle H;
		P->GetWorldTimerManager().SetTimer(H, FTimerDelegate::CreateWeakLambda(this, [Weak]()
		{
			UOutlawsCoop* C = Weak.Get();
			if (!C) return;
			// (BUG-MPDEATHCAM) the dead one's view stays where it fell: no camera on another player, the eye where it died
			const bool bHeld = !C->SpectateCam.IsValid() && C->SpectateWho < 0 && !C->bDeathViewLeft && C->DeathViewDrift <= 120.0;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH test check %s: the dead %s's view holds at the death spot (%.0f cm off it, %s)"), bHeld ? TEXT("PASS") : TEXT("FAIL"), WhoWord(C->MyIndex()),
				C->DeathViewDrift, C->SpectateCam.IsValid() || C->bDeathViewLeft ? TEXT("on another player") : TEXT("on no other player"));
			Shot(TEXT("ol_spectate"));
		}), 3.f, false);
	}
}

// ---- the map's pickup generators (M1's gaps item 2) -----------------------------------------------------------------------
// A multiplayer map's pickups are N* generators (FUNC Gen_New, GENERATE the item, FREQ its period): the level build leaves
// them out, and the rules (FOutlawsDeathmatch::ReadLevel) time them.  The host puts each generator's item on the ground where
// the generator stands (a ground item: numbered and made on every machine, R7's late joiners get it in their snapshot); once
// it is taken (by anyone: the host's world loses it too) the generator's period runs and the item comes back; the match's
// start puts every one back.

void UOutlawsCoop::TickMatchItems(double Now)
{
	AOutlawsLevelActor* L = FindLevel();
	if (!L || !IsHost() || !bMatchSetUp || CVarOLMatchItems.GetValueOnGameThread() == 0) return;
	TArray<FOutlawsDmItemGen>& Items = Dm.GetItems();
	if (MatchItemSerial != MatchSerial)
	{
		// this level's first look: the items out, and the rules' word for when one comes back
		MatchItemSerial = MatchSerial;
		MatchItemActors.Reset();
		MatchItemActors.SetNum(Items.Num());
		MatchItemListed.Reset();
		MatchItemListed.SetNumZeroed(Items.Num());
		TWeakObjectPtr<UOutlawsCoop> Weak(this);
		Dm.OnItemBack = [Weak](const FOutlawsDmItemGen& G, int32 Index)
		{
			UOutlawsCoop* C = Weak.Get();
			AOutlawsLevelActor* Lv = C ? C->FindLevel() : nullptr;
			if (!Lv || !C->MatchItemActors.IsValidIndex(Index) || C->MatchItemActors[Index].IsValid()) return;
			C->MatchItemActors[Index] = Lv->SpawnGroundItem(FPaths::GetBaseFilename(G.Generate).ToLower(), G.Location);
			if (C->MatchItemListed.IsValidIndex(Index)) C->MatchItemListed[Index] = C->MatchItemActors[Index].IsValid() && PickupIndexOf(Lv, C->MatchItemActors[Index].Get()) >= 0;
			++C->MatchItemsBack;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH item back: %s (%s) at %s%s"), *G.Object, *G.Generate, *G.Location.ToCompactString(), C->MatchItemActors[Index].IsValid() ? TEXT("") : TEXT(" (NOT MADE)"));
		};
		int32 Made = 0;
		for (int32 i = 0; i < Items.Num(); ++i)
		{
			MatchItemActors[i] = L->SpawnGroundItem(FPaths::GetBaseFilename(Items[i].Generate).ToLower(), Items[i].Location);
			MatchItemListed[i] = MatchItemActors[i].IsValid() && PickupIndexOf(L, MatchItemActors[i].Get()) >= 0;
			Items[i].bOut = true;
			if (MatchItemActors[i].IsValid()) ++Made;
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLMATCH the map's pickup generators: %d of %d items out"), Made, Items.Num());
		return;
	}
	// taken: the generator's period starts (a taken pickup leaves the host's list, whoever took it)
	for (int32 i = 0; i < Items.Num() && i < MatchItemActors.Num(); ++i)
	{
		if (!Items[i].bOut) continue;
		// (a pickup: gone from the host's list once taken; dynamite lies apart from the list: its actor gone or hidden)
		AActor* A = MatchItemActors[i].Get();
		const bool bListed = MatchItemListed.IsValidIndex(i) && MatchItemListed[i];
		const bool bThere = A && !A->IsHidden() && (!bListed || PickupIndexOf(L, A) >= 0);
		if (bThere) continue;
		MatchItemActors[i].Reset();
		Dm.NoteItemTaken(i, Now);
	}
}
