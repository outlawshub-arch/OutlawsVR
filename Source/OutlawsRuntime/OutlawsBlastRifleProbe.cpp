// BUG-BLASTMISS + REQ-RIFLEMP's probe (the user's five answers of 4 Nov).  Three ways in:
//   a match, two machines (the REQ-MPLETHAL probe's test run):
//     host:   -OLMpHost=town_2p -OLMpClass=dm -OLMpBots=3 -OLMpBotChars=Jam,Jam,Jam -OLMpChar=Jam -OLMpHostRespawn=2 -OLMpMinutes=30
//             -OLMpFrags=0 -OLBlastRifleProbe [-OLOldBotPush] [-OLOldRifleMp] [-OLOldHitLocation] [-OLOldBlastPlayer]
//     client: -OLCoopJoin=<host>:<port> -OLMpChar=Jam -OLBlastRifleProbe
//     The bots hold their fire.  1. The hits to kill, measured through the game's own calls: rifle, revolver, shotgun (all its
//     pellets) at head/chest, mid and edge, the host on a bot (12 kills a cell; 1010 health and "dead" at 1000, so no respawn
//     between them) and on the client (a real kill a cell from full health).  2. The client's own shots, decided on the host: on a
//     bot and on the host, the rifle with no rifle in its hands not doubled, and real trigger pulls through FireGun (the client's and
//     the host's, rifle and revolver at the chest: the other jobs start at ShotAtPlayers).  3. REQ-MPLETHAL's scoped head shot kills, the host's
//     and the client's.  4. A blast: the throw on the host's player, the same on a bot from the host's stick and from the
//     client's (the client watches the bot's body it draws); a blast on a player stays 12.
//   co-op's story, two machines: -OLCoopAuto -OLCoopHost -OLCoopLevel=<lv> -OLBlastRifleProbe [-OLOldBlastPlayer] and -OLCoopAuto
//     -OLCoopJoin=.. -OLBlastRifleProbe: a blast between the two takes half (6) off each, no shot strikes a player, no bots.
//   a story level, one machine: -OLShots -OLNoFrontEnd -OLLevel=<lv> -OLBlastRifleProbe=story [-OLOldBlastPlayer]: the health a blast
//     takes point blank (own stick, a bad guy's), by distance and through a plate, and the level's throwers.
// Lines "OLSHOT blastrifle check PASS|FAIL: ...", "OLSHOT blastrifle table: ..." and "OLSHOT blastrifle <host|client|story> RESULT".

#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsDeathmatch.h"
#include "OutlawsEnemy.h"
#include "OutlawsFlags.h"
#include "OutlawsLevelActor.h"
#include "OutlawsMpHost.h"
#include "OutlawsPlayer.h"
#include "OutlawsRemotePlayer.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsShotTests.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "HAL/PlatformMisc.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsBlastRifleProbeLocal
{
	struct FGun { const TCHAR* Name; float Base; int32 Pellets; bool bRifle; };
	struct FSpotAim { const TCHAR* Name; double Across; double Up; float Mult; };
	const FGun Guns[] = { { TEXT("rifle"), 2.f, 1, true }, { TEXT("revolver"), 2.f, 1, false }, { TEXT("shotgun"), 1.5f, 6, false } };
	const FSpotAim Spots[] = { { TEXT("head/chest"), 0.1, 20.0, 1.5f }, { TEXT("mid"), 0.5, 20.0, 1.f }, { TEXT("edge"), 0.85, 20.0, 0.5f } };
	constexpr int32 NumCells = 9, TrialsACell = 12;
	constexpr double BodyRadius = 32.0;

	int32 Passes = 0, Fails = 0, Cell = 0, Trial = 0, Count = 0, Lo = 99, Hi = 0, Sum = 0, Phase = 0, Client = -1, Job = 0, Spent = 0;
	double PhaseAt = 0.0, NextAt = 0.0, SecondAt = 0.0;
	TArray<int32> BotIds;
	TArray<FString> Table;
	float Before = 0.f;

	/** What a blast did to a body, sampled a tick. */
	struct FWatch
	{
		FVector Start = FVector::ZeroVector;
		double PeakSpeed = 0.0, Rise = 0.0, Carry = 0.0, LastAt = -1.0, LastVz = 0.0;
		TArray<double> Pulls;
		void Begin(const FVector& At) { Start = At; PeakSpeed = Rise = Carry = 0.0; LastAt = -1.0; Pulls.Reset(); }
		void Note(const FVector& At, double Speed)
		{
			PeakSpeed = FMath::Max(PeakSpeed, Speed);
			Rise = FMath::Max(Rise, double(At.Z - Start.Z));
			Carry = FMath::Max(Carry, FVector::Dist2D(At, Start));
		}
		/** The upward speed a tick on the way up: what it loses a second is the gravity it flies under. */
		void NoteUp(double At, double Vz)
		{
			if (LastAt >= 0.0 && At > LastAt && LastVz > 0.0 && Vz > 0.0) Pulls.Add((LastVz - Vz) / (At - LastAt) / 980.0);
			LastAt = At;
			LastVz = Vz;
		}
		/** The middle of those, in g (a roof's stop is one tick's wild one). */
		double Gs() const
		{
			if (Pulls.Num() == 0) return 0.0;
			TArray<double> Sorted = Pulls;
			Sorted.Sort();
			return Sorted[Sorted.Num() / 2];
		}
		FString Say() const { return FString::Printf(TEXT("%.0f cm/s at most, %.0f cm up, %.0f cm along"), PeakSpeed, Rise, Carry); }
	};
	FWatch WatchPlayer, WatchBot, WatchSeen;
	FVector BlastSpot = FVector::ZeroVector, BlastStick = FVector::ZeroVector, BotLaunch = FVector::ZeroVector;
	double BlastRoom = 0.0;
	int32 SeenWho = -1;
	bool bSeenOld = false, bSeenOpen = false;
	double SeenUntil = 0.0, SeenRoom = 0.0;
	/** A point-blank stick's throw under the headset's 1.5 g: 2477 cm/s up, so this high with nothing over it. */
	constexpr double OpenRise = 2087.0;

	void Check(bool bOk, const FString& What)
	{
		(bOk ? Passes : Fails)++;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT blastrifle check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
	}
	void Result(const TCHAR* Who, const TCHAR* Note = TEXT(""))
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT blastrifle %s RESULT %s (%d passed, %d failed%s)"), Who, Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails, Note);
	}
	bool Old(const TCHAR* Flag) { return OLFlags::Param(Flag); }
	/** A gun's number on a spot as the switches have it. */
	float WantEach(const FGun& G, const FSpotAim& S)
	{
		return (G.bRifle && !Old(TEXT("OLOldRifleMp")) ? 2.f : 1.f) * (Old(TEXT("OLOldHitLocation")) ? 1.f : S.Mult);
	}
	void WantHits(const FGun& G, const FSpotAim& S, int32& OutLo, int32& OutHi)
	{
		const float Each = G.Base * G.Pellets * WantEach(G, S);
		OutLo = FMath::CeilToInt(10.f / (Each * 1.1f));
		OutHi = FMath::CeilToInt(10.f / (Each * 0.9f));
	}
	/** One trigger pull at Who's body as drawn here: every pellet at the spot, each with the game's own spread of damage. */
	int32 Fire(const UOutlawsCoop* C, AOutlawsLevelActor* L, const FVector& From, int32 Who, const FGun& G, const FSpotAim& S)
	{
		const AOutlawsRemotePlayer* R = C->PuppetOf(Who);
		if (!R) return 0;
		const FVector Mid = R->ShownBody();
		const FVector Flat = (Mid - From).GetSafeNormal2D();
		const FVector Side = FVector::CrossProduct(FVector::UpVector, Flat);
		const FVector To = Mid + Side * S.Across * BodyRadius + FVector(0.0, 0.0, S.Up);
		const FVector Dir = (To - From).GetSafeNormal();
		int32 Hits = 0;
		for (int32 p = 0; p < G.Pellets; ++p)
		{
			FVector At;
			Hits += UOutlawsCoop::ShotAtPlayers(L, From, To + Dir * 300.0, G.Base * FMath::FRandRange(0.9f, 1.1f), At, false, G.bRifle) ? 1 : 0;
		}
		return Hits;
	}
	/** The top of the figure drawn for Who (REQ-MPLETHAL's head). */
	FVector HeadOf(const UOutlawsCoop* C, int32 Who)
	{
		const AOutlawsRemotePlayer* R = C->PuppetOf(Who);
		if (!R) return FVector::ZeroVector;
		const double Half = FMath::Max(40.0, double(R->ShownHalf()));
		const FVector Feet = R->ShownBody() - FVector(0.0, 0.0, Half);
		const AOutlawsSpriteActor* Spr = R->ShownSprite();
		return Feet + FVector(0.0, 0.0, (Spr && Spr->DrawnHeightCm() > 50.0 ? Spr->DrawnHeightCm() : 2.0 * Half) * 0.94);
	}
	/** A spot for a stick Off cm from Mid at its feet with a clear line to it, on the side with the longest clear run behind the body. */
	FVector StickBeside(const AOutlawsLevelActor* L, const FVector& Mid, double Half, double Off, const AActor* Target)
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsBlastRifleStick), true, Target);
		FVector Best = Mid + FVector(0.0, 0.0, 10.0);
		double BestRun = -1.0;
		for (int32 a = 0; a < 8; ++a)
		{
			const FVector Dir = FRotator(0.0, 45.0 * a, 0.0).Vector();
			const FVector Spot = Mid + Dir * Off - FVector(0.0, 0.0, Half - 10.0);
			if (L->GetWorld()->LineTraceTestByChannel(Spot + FVector(0.0, 0.0, 20.0), Mid, ECC_Visibility, Q)) continue;
			FHitResult H;
			const double Run = L->GetWorld()->LineTraceSingleByChannel(H, Mid, Mid - Dir * 2000.0, ECC_Visibility, Q) ? double(H.Distance) : 2000.0;
			if (Run > BestRun) { BestRun = Run; Best = Spot; }
		}
		return Best;
	}
	/** How high that throw goes from Mid, the stick at Stick: walls are slid along (the rise keeps), a roof ends it. */
	double ThrowRoom(const UWorld* W, const FVector& Mid, const FVector& Stick, double Radius, double Half, const AActor* A, const AActor* B)
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsBlastRifleRoom), false, A);
		Q.AddIgnoredActor(B);
		FVector At = Mid;
		FVector Vel = ((Mid - Stick).GetSafeNormal2D() + FVector(0.0, 0.0, 0.7)).GetSafeNormal() * 4320.0;
		const double Dt = 1.0 / 30.0;
		for (int32 i = 0; i < 90 && Vel.Z > 0.0; ++i)
		{
			Vel.Z -= 1470.0 * Dt;
			FHitResult H;
			if (!W->SweepSingleByChannel(H, At, At + Vel * Dt, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(Radius, Half), Q))
			{
				At += Vel * Dt;
				continue;
			}
			if (H.bStartPenetrating || H.ImpactNormal.Z < -0.1) break;
			At = H.Location;
			Vel = FVector::VectorPlaneProject(Vel, H.ImpactNormal);
		}
		return At.Z - Mid.Z;
	}
}

void UOutlawsCoop::TickHostBlastRifleTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	using namespace OutlawsBlastRifleProbeLocal;
	FOutlawsMatchFlow& F = OutlawsMpHost::Flow();
	const double Wall = FPlatformTime::Seconds();
	L->bEnemiesActive = false;          // (the bots' brains stand where they spawned, apart)
	const bool bOldPush = Old(TEXT("OLOldBotPush")), bOldBlast = Old(TEXT("OLOldBlastPlayer"));
	auto ClientHealth = [this]() { const AOutlawsCoopLink* K = GetLink(Client); return K ? K->Stats.Health : -1.f; };
	auto ClientDead = [this]() { const AOutlawsCoopLink* K = GetLink(Client); return !K || (K->Stats.Seq > 0 && K->Stats.bDead) || K->Stats.Health <= 0.f; };
	auto Finish = [&]()
	{
		for (const FString& T : Table) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT blastrifle table: %s"), *T);
		Result(TEXT("host"), *FString::Printf(TEXT("%s%s%s%s"), bOldPush ? TEXT(", -OLOldBotPush") : TEXT(""), Old(TEXT("OLOldRifleMp")) ? TEXT(", -OLOldRifleMp") : TEXT(""),
			Old(TEXT("OLOldHitLocation")) ? TEXT(", -OLOldHitLocation") : TEXT(""), bOldBlast ? TEXT(", -OLOldBlastPlayer") : TEXT("")));
		TestSay(TEXT("quit"));
		TestStep = 2399;
		TestAt = Now + 3.0;
	};
	auto StandBeside = [&](const FVector& Mid)
	{
		// (3 m off the victim: the host's check wants the muzzle near the shooter)
		const FVector Flat = (P->GetActorLocation() - Mid).GetSafeNormal2D();
		const FVector Eye = Mid + (Flat.IsNearlyZero() ? FVector::ForwardVector : Flat) * 300.0;
		P->SetActorLocation(Eye - FVector(0.0, 0.0, 30.0), false, nullptr, ETeleportType::TeleportPhysics);
		return Eye;
	};
	auto StandFar = [&](const FVector& From)
	{
		// (the spawn furthest off: out of a shot or a blast meant for someone else)
		double Best = -1.0;
		FVector Far = P->GetActorLocation();
		for (const FOutlawsDmSpawn& Sp : Dm.GetSpawns())
		{
			const double D = FVector::Dist(Sp.Location, From);
			if (D > Best) { Best = D; Far = Sp.Location; }
		}
		P->SetActorLocation(Far, false, nullptr, ETeleportType::TeleportPhysics);
	};
	if (!IsMatch())
	{
		// ---- co-op's story: the pair stand together, one blast between them; items 2 to 4 have nothing to act on
		if (TestStep < 2390) { TestStep = 2390; TestAt = Now + 8.0; Passes = Fails = 0; }
		TArray<AOutlawsCoopLink*> Links;
		GetLinks(Links);
		for (const AOutlawsCoopLink* K : Links) if (K->PlayerIndex > 0) Client = K->PlayerIndex;
		const float Want = bOldBlast ? 12.f : 6.f;
		switch (TestStep)
		{
		case 2390:
			if (Now < TestAt || Client < 0 || !PuppetOf(Client)) return;
			L->TestSetHealth(100.f);
			TestSayTo(Client, TEXT("ml:brhealth"), 100);
			TestSayTo(Client, TEXT("ml:near"), 0);
			TestStep = 2391;
			TestAt = Now + 2.5;
			return;
		case 2391:
			if (Now < TestAt) return;
			Before = ClientHealth();
			L->Explode(P->GetActorLocation() - FVector(0.0, 0.0, 60.0));
			Check(FMath::IsNearlyEqual(100.f - L->GetHealth(), Want, 0.3f), FString::Printf(TEXT("co-op: the host loses %.2f to a blast at its feet (want %.0f)"), 100.f - L->GetHealth(), Want));
			TestStep = 2392;
			TestAt = Now + 0.7;             // (the blast's alone: its throw's landing, a 12.5 m drop, takes a point of its own after that)
			return;
		case 2392:
		{
			if (Now < TestAt) return;
			const float Lost = Before - ClientHealth();
			Check(Lost > 0.f && Lost <= Want + 0.01f, FString::Printf(TEXT("co-op: the client beside it loses %.2f (health %.1f -> %.1f; %.0f at most)"), Lost, Before, ClientHealth(), Want));
			FVector At;
			const FVector Eye = P->GetActorLocation();
			Check(!UOutlawsCoop::ShotAtPlayers(L, Eye, PuppetOf(Client)->ShownBody(), 2.f, At, false, true), TEXT("co-op: a rifle shot strikes no player"));
			Check(Bots.Num() == 0, TEXT("co-op: no bots"));
			Result(TEXT("host"), bOldBlast ? TEXT(", -OLOldBlastPlayer") : TEXT(""));
			TestSay(TEXT("quit"));
			TestStep = 2399;
			TestAt = Now + 3.0;
			return;
		}
		case 2399:
			if (Now < TestAt) return;
			TestStep = 2400;
			QuitForTest();
			return;
		default:
			return;
		}
	}
	if (TestStep < 2330) { TestStep = 2330; TestAt = 0.0; }
	switch (TestStep)
	{
	case 2330:
	{
		// (the level's own dynamite out of it: a stick by a pickup sets it off, and 12.00 + 2.87 + 2.96 + 1.83 is no reading of one blast)
		for (AOutlawsLevelActor::FDynamite& D : L->Dynamite) D.bGone = true;
		// the three bots and the client in the rules; then Start, and past the spawn protection
		TArray<AOutlawsCoopLink*> Links;
		GetLinks(Links);
		BotIds.Reset();
		Client = -1;
		for (const AOutlawsCoopLink* K : Links)
		{
			const int32 Who = K->PlayerIndex;
			if (Who <= 0 || Who >= MaxPlayers || !MatchInRules[Who]) continue;
			if (K->bBot) BotIds.Add(Who);
			else if (Client < 0) Client = Who;
		}
		if (TestAt == 0.0) TestAt = Now + 240.0;
		if ((BotIds.Num() < 3 || Client < 0 || !bMatchSetUp) && Now < TestAt) return;
		Check(BotIds.Num() >= 3 && Client > 0, FString::Printf(TEXT("the match has %d bots and %s"), BotIds.Num(), Client > 0 ? TEXT("the client") : TEXT("no client")));
		if (BotIds.Num() < 3 || Client < 0) { Finish(); return; }
		F.HostStart();
		TestStep = 2331;
		return;
	}
	case 2331:
		if (F.GetState() != FOutlawsMatchFlow::EState::Playing) return;
		TestStep = 2332;
		TestAt = Now + 4.0;
		return;
	case 2332:
		if (Now < TestAt) return;
		Cell = Trial = Count = Sum = Hi = 0;
		Lo = 99;
		// (the bots are one of each, whatever the list says: the table on the one with no DAMAGE_MULTIPLIER)
		BotIds.Sort([this](int32 X, int32 Y) { return FMath::Abs(MatchDamageFor(X, 1.f) - 1.f) < FMath::Abs(MatchDamageFor(Y, 1.f) - 1.f); });
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT blastrifle live: multiplier on the table's bot %.2f, on the jobs' bot %.2f, on the client %.2f"), MatchDamageFor(BotIds[0], 1.f), MatchDamageFor(BotIds[1], 1.f), MatchDamageFor(Client, 1.f));
		TestStep = 2340;
		return;

	// 1. the table on a bot: 1010 health, "dead" at 1000, up to 18 hits a second (the host's own limit is 40, its second not this one's:
	// two of these can fall in one of its, a refused pellet is a hit too few)
	case 2340:
	{
		const FGun& G = Guns[Cell / 3];
		const FSpotAim& S = Spots[Cell % 3];
		FBot* B = FindBot(BotIds[0]);
		const AOutlawsRemotePlayer* R = PuppetOf(BotIds[0]);
		if (!B || B->bDead || !R) return;
		if (Wall - SecondAt >= 1.0) { SecondAt = Wall; Spent = 0; }
		if (Spent + G.Pellets > 18) return;
		if (Count == 0) B->Health = 1010.f;
		const FVector Eye = StandBeside(R->ShownBody());
		Fire(this, L, Eye, BotIds[0], G, S);
		Spent += G.Pellets;
		++Count;
		if (B->Health > 1000.f && Count < 40) return;
		Lo = FMath::Min(Lo, Count);
		Hi = FMath::Max(Hi, Count);
		Sum += Count;
		Count = 0;
		if (++Trial < TrialsACell) return;
		int32 WLo, WHi;
		WantHits(G, S, WLo, WHi);
		Check(Lo >= WLo && Hi <= WHi, FString::Printf(TEXT("%s, %s, on a bot, %d kills: %d-%d hits (avg %.1f), want %d-%d"), G.Name, S.Name, Trial, Lo, Hi, float(Sum) / Trial, WLo, WHi));
		Table.Add(FString::Printf(TEXT("%s | %s | x%.2f | bot %d-%d (avg %.1f) | want %d-%d"), G.Name, S.Name, WantEach(G, S), Lo, Hi, float(Sum) / Trial, WLo, WHi));
		B->Health = 10.f;
		Trial = Sum = Hi = 0;
		Lo = 99;
		if (++Cell < NumCells) return;
		Cell = Phase = 0;
		PhaseAt = Now;
		TestStep = 2350;
		return;
	}

	// 1b. the client, a real kill a cell from full health
	case 2350:
	{
		const FGun& G = Guns[Cell / 3];
		const FSpotAim& S = Spots[Cell % 3];
		const FOutlawsDmPlayer* D = Dm.Find(Client);
		const AOutlawsRemotePlayer* R = PuppetOf(Client);
		if (Phase == 0)
		{
			const bool bFresh = D && D->bAlive && Wall > D->ProtectedUntil + 0.3 && !MatchDead[Client] && !ClientDead() && ClientHealth() >= 9.99f && R;
			if (!bFresh && Now - PhaseAt < 40.0) return;
			Check(bFresh, FString::Printf(TEXT("%s, %s: the client came back fresh"), G.Name, S.Name));
			if (!bFresh) { Finish(); return; }
			Count = 0;
			NextAt = Now;
			Phase = 1;
			return;
		}
		if (Now < NextAt) return;
		NextAt = Now + 0.8;
		if (!ClientDead() && Count < 16)
		{
			const FVector Eye = StandBeside(R->ShownBody());
			Fire(this, L, Eye, Client, G, S);
			++Count;
			return;
		}
		int32 WLo, WHi;
		WantHits(G, S, WLo, WHi);
		if (WHi >= 8) WHi += 3;         // (a player's own regain, 4.9 to 5.0 under half, over a slow run of blows)
		Check(ClientDead() && Count >= WLo && Count <= WHi, FString::Printf(TEXT("%s, %s, on the client: %d hits to kill, want %d-%d"), G.Name, S.Name, Count, WLo, WHi));
		for (FString& T : Table) if (T.StartsWith(FString::Printf(TEXT("%s | %s |"), G.Name, S.Name))) T += FString::Printf(TEXT(" | client %d"), Count);
		TestSayTo(Client, TEXT("ml:respawn"), 0);
		Phase = 0;
		PhaseAt = Now;
		if (++Cell < NumCells) return;
		Job = 0;
		TestStep = 2360;
		return;
	}

	// 2. the client's own shots, decided here: rifle in the hands, then none
	case 2360:
	{
		// Target: 0 a bot, 1 the host.  Pull: a real trigger pull through FireGun, 1 the client's, 2 the host's (rifle and revolver at the
		// chest: the gun held first, as a hand holds it; no shotgun, its +-4 degrees have no one spot, and the pellets take the same way)
		struct FShotJob { int32 Cell; int32 Target; bool bHold; int32 Pull; };
		static const FShotJob Jobs[] = { { 0, 0, true, 0 }, { 1, 0, true, 0 }, { 2, 0, true, 0 }, { 3, 0, true, 0 }, { 0, 1, true, 0 }, { 2, 1, true, 0 }, { 0, 0, false, 0 },
			{ 0, 0, true, 1 }, { 3, 0, true, 1 }, { 0, 1, true, 1 }, { 3, 1, true, 1 }, { 0, 0, true, 2 }, { 3, 0, true, 2 } };
		if (Job >= int32(UE_ARRAY_COUNT(Jobs))) { TestStep = 2370; Phase = 0; return; }
		const FShotJob& J = Jobs[Job];
		const FGun& G = Guns[J.Cell / 3];
		const FSpotAim& S = Spots[J.Cell % 3];
		const int32 Who = J.Target == 1 ? 0 : BotIds[1];
		FBot* B = FindBot(BotIds[1]);
		if (Phase == 0)
		{
			// (REQ-SPAWNGRACE: the striker past its own protection too, a protected player's shot is refused)
			const FOutlawsDmPlayer* Striker = Dm.Find(J.Pull == 2 ? 0 : Client);
			if (!MatchDead[Client] && !ClientDead() && B && !B->bDead && Striker && Wall > Striker->ProtectedUntil + 0.3) { TestAt = Now; Phase = 1; }
			return;
		}
		if (Phase == 1)
		{
			const FString Hold = FString::Printf(TEXT("hold:%d"), J.Pull != 0 && !G.bRifle ? 0 : J.bHold ? 1 : 0);
			if (J.Pull == 2)
			{
				OnBlastRifleTestStep(TEXT("brback"), Who, L, P);
				OnLethalTestStep(Hold, 0, L, P);
			}
			else
			{
				TestSayTo(Client, J.Pull != 0 ? TEXT("ml:brback") : TEXT("ml:near"), Who);
				TestSayTo(Client, TEXT("ml:") + Hold, Who);
			}
			TestAt = Now + 1.5;
			Phase = 2;
			return;
		}
		if (Now < TestAt) return;
		if (Phase == 2)
		{
			if (J.Target == 1) L->TestSetHealth(100.f);
			else B->Health = 1010.f;
			Before = J.Target == 1 ? 100.f : 1010.f;
			if (J.Pull == 2) OnBlastRifleTestStep(TEXT("brpull"), J.Cell * 16 + Who, L, P);
			else TestSayTo(Client, J.Pull == 1 ? TEXT("ml:brpull") : TEXT("ml:brshot"), J.Cell * 16 + Who);
			TestAt = Now + 1.0;
			Phase = 3;
			return;
		}
		const float Lost = Before - (J.Target == 1 ? L->GetHealth() : B->Health);
		const float Each = G.Base * (J.bHold ? WantEach(G, S) : (Old(TEXT("OLOldHitLocation")) ? 1.f : S.Mult)) * MatchDamageFor(Who, 1.f);
		Check(Lost >= Each * 0.89f && Lost <= Each * 1.11f, FString::Printf(TEXT("the %s's %s%s at %s on the %s%s: %.2f taken, want %.2f +-10%%"), J.Pull == 2 ? TEXT("host") : TEXT("client"), G.Name, J.Pull != 0 ? TEXT(", a real trigger pull,") : TEXT(""),
			S.Name, WhoWord(Who), J.bHold ? TEXT("") : TEXT(", no rifle in its hands"), Lost, Each));
		if (B) B->Health = 10.f;
		L->TestSetHealth(10.f);
		++Job;
		Phase = 0;
		return;
	}

	// 3. REQ-MPLETHAL's scoped head shot alongside: the host's on a bot, the client's on another
	case 2370:
	{
		FBot* B0 = FindBot(BotIds[0]);
		FBot* B1 = FindBot(BotIds[1]);
		if (Phase == 0)
		{
			if (!B0 || !B1 || B0->bDead || B1->bDead || !PuppetOf(BotIds[0])) return;
			B0->Health = B1->Health = 10.f;
			const FVector Head = HeadOf(this, BotIds[0]);
			const FVector Flat = (Head - P->GetActorLocation()).GetSafeNormal2D();
			const FVector Eye = Head - Flat * 300.0;
			P->SetActorLocation(Eye - FVector(0.0, 0.0, 60.0), false, nullptr, ETeleportType::TeleportPhysics);
			FVector At;
			const bool bHit = UOutlawsCoop::ShotAtPlayers(L, Eye, Head + Flat * 300.0, 2.f, At, true, true);
			Check(bHit && B0->bDead, FString::Printf(TEXT("the host's scoped head shot on a bot of 10 health: %s, health %.1f"), bHit ? TEXT("a hit") : TEXT("a miss"), B0->Health));
			if (const AOutlawsRemotePlayer* R1 = PuppetOf(BotIds[1])) StandFar(R1->ShownBody());     // (not in the client's line: the spawns differ a run)
			TestSayTo(Client, TEXT("ml:near"), BotIds[1]);
			TestSayTo(Client, TEXT("ml:hold:1"), BotIds[1]);
			TestAt = Now + 1.5;
			Phase = 1;
			return;
		}
		if (Now < TestAt) return;
		if (Phase == 1)
		{
			TestSayTo(Client, TEXT("ml:brscoped"), BotIds[1]);
			TestAt = Now + 1.0;
			Phase = 2;
			return;
		}
		Check(!B1 || B1->bDead, FString::Printf(TEXT("the client's scoped head shot on a bot of 10 health: health %.1f"), B1 ? B1->Health : -1.f));
		TestStep = 2380;
		Phase = 2;
		return;
	}

	// 4. a blast.  The throw on the host's player; on a bot, from the host's stick; from the client's stick, the client watching
	case 2380:
	{
		FBot* B = FindBot(BotIds[2]);
		FBot* B2 = FindBot(BotIds[0]);
		AOutlawsEnemy* E = B ? B->Brain.Get() : nullptr;
		AOutlawsEnemy* E2 = B2 ? B2->Brain.Get() : nullptr;
		const double Half = P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		switch (Phase)
		{
		case 2:
		{
			if (!E || B->bDead) return;
			B->Health = 1010.f;
			E->Health = 1000.f;                 // (what its landing does is the bots' own rule, not this flight's)
			// (the user, 4 Nov: "match the player's flight": measured where the whole rise has the room: of the spawns out of this
			// blast's reach of everyone else, the tallest throw)
			{
				UWorld* W = L->GetWorld();
				const double BotHalf = E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
				const double Radius = FMath::Max(double(E->GetCapsuleComponent()->GetScaledCapsuleRadius()), double(P->GetCapsuleComponent()->GetScaledCapsuleRadius()));
				FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsBlastRifleOpen), false, E);
				Q.AddIgnoredActor(P);
				auto ClearOfOthers = [&](const FVector& At)
				{
					for (const FBot& O : Bots)
					{
						const AOutlawsEnemy* Other = O.Brain.Get();
						if (Other && Other != E && FVector::Dist(Other->GetActorLocation(), At) < 1700.0) return false;
					}
					const AOutlawsRemotePlayer* R = PuppetOf(Client);
					return !R || FVector::Dist(R->ShownBody(), At) >= 1700.0;
				};
				BlastSpot = E->GetActorLocation();
				BlastStick = StickBeside(L, BlastSpot, BotHalf, 150.0, E);
				BlastRoom = -1.0;
				int32 Tried = 0;
				for (const FOutlawsDmSpawn& Sp : Dm.GetSpawns())
				{
					FHitResult Floor;
					if (!W->LineTraceSingleByChannel(Floor, Sp.Location + FVector(0.0, 0.0, 100.0), Sp.Location - FVector(0.0, 0.0, 600.0), ECC_Visibility, Q)) continue;
					const FVector Mid = FVector(Floor.ImpactPoint) + FVector(0.0, 0.0, BotHalf + 3.0);
					if (!ClearOfOthers(Mid)) continue;
					for (int32 a = 0; a < 8; ++a)
					{
						const FVector Stick = Mid + FRotator(0.0, 45.0 * a, 0.0).Vector() * 150.0 - FVector(0.0, 0.0, BotHalf - 10.0);
						if (W->LineTraceTestByChannel(Stick + FVector(0.0, 0.0, 20.0), Mid, ECC_Visibility, Q)) continue;
						const double Up = ThrowRoom(W, Mid, Stick, Radius, FMath::Max(BotHalf, Half), E, P);
						++Tried;
						if (Up > BlastRoom) { BlastRoom = Up; BlastSpot = Mid; BlastStick = Stick; }
					}
				}
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT blastrifle the open spot: %d throws tried of %d spawns, the tallest has %.0f cm over it, at %s"), Tried, Dm.GetSpawns().Num(), BlastRoom, *BlastSpot.ToCompactString());
				E->SetActorLocation(BlastSpot, false, nullptr, ETeleportType::TeleportPhysics);
				E->GetCharacterMovement()->StopMovementImmediately();
			}
			// (the host well away: this blast is the bot's)
			StandFar(BlastSpot);
			TestAt = Now + 0.6;
			Phase = 10;
			return;
		}
		case 10:
			if (Now < TestAt) return;
			if (!E) return;
			BlastSpot = E->GetActorLocation();                 // (as it settled)
			TestSayTo(Client, TEXT("ml:brwatch"), BotIds[2] + (bOldPush ? 100 : 0) + 1000 * int32(FMath::Max(0.0, BlastRoom)));
			TestAt = Now + 0.3;
			Phase = 11;
			return;
		case 11:
		{
			if (Now < TestAt) return;
			if (!E) return;
			WatchBot.Begin(BlastSpot);
			L->Explode(BlastStick);
			// (the push as BlastThrow sums it for a player: 360 cm/s a point, 12 points inside a quarter of the radius, 0.7 up)
			BotLaunch = E->PendingKnock;
			{
				const float Want = 12.f * MatchDamageFor(BotIds[2], 1.f);
				Check(FMath::IsNearlyEqual(1010.f - B->Health, Want, 0.5f), FString::Printf(TEXT("the host's stick on a bot in a match: %.1f taken (12 x its multiplier: %.1f)"), 1010.f - B->Health, Want));
				const FVector Nominal = ((BlastSpot - (BlastStick + FVector(0.0, 0.0, 20.0))).GetSafeNormal2D() + FVector(0.0, 0.0, 0.7)).GetSafeNormal() * 4320.0;
				Check(bOldPush ? BotLaunch.IsNearlyZero() : FVector::Dist(BotLaunch, Nominal) < 5.0, FString::Printf(TEXT("the bot's launch %.0f cm/s (up %.0f); the player's formula %.0f (up %.0f)%s"),
					BotLaunch.Size(), BotLaunch.Z, Nominal.Size(), Nominal.Z, bOldPush ? TEXT(", -OLOldBotPush: none") : TEXT("")));
			}
			TestAt = Now + 4.5;
			Phase = 3;
			return;
		}
		case 3:
			if (E)
			{
				WatchBot.Note(E->GetActorLocation(), E->GetVelocity().Size());
				WatchBot.NoteUp(L->GetWorld()->GetTimeSeconds(), E->GetVelocity().Z);
			}
			if (Now < TestAt) return;
			if (E)
			{
				Check(E->GetCharacterMovement()->IsMovingOnGround() && E->GetCharacterMovement()->GravityScale == 1.f,
					FString::Printf(TEXT("the bot down again, its own gravity back: x%.2f, on the ground %d"), E->GetCharacterMovement()->GravityScale, E->GetCharacterMovement()->IsMovingOnGround() ? 1 : 0));
			}
			// the player next: the same spot, the same stick, in the headset's gravity (the probe's body is a desktop's, 4.59 g)
			L->bInvincible = true;              // (the throw alone: a dead view is not thrown)
			P->BaseGravityScale = 1.5f;
			P->SetActorLocation(BlastSpot + FVector(0.0, 0.0, Half - (E ? double(E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()) : Half)), false, nullptr, ETeleportType::TeleportPhysics);
			P->GetCharacterMovement()->StopMovementImmediately();
			TestAt = Now + 0.5;
			Phase = 12;
			return;
		case 12:
			// (settled first: UpdateGravity takes the headset's gravity on the body's next tick, and the launch's first frames at 4.59 g cost 50 to 120 cm of the rise)
			if (Now < TestAt) return;
			WatchPlayer.Begin(P->GetActorLocation());
			L->Explode(BlastStick);
			TestAt = Now + 4.5;
			Phase = 9;
			return;
		case 9:
			WatchPlayer.Note(P->GetActorLocation(), P->GetVelocity().Size());
			WatchPlayer.NoteUp(L->GetWorld()->GetTimeSeconds(), P->GetVelocity().Z);
			if (Now < TestAt) return;
			L->bInvincible = false;
			P->BaseGravityScale = P->DesktopGravityScale;
			// (thrown at all, and the measure itself: the headset's 1.5 g, as high as the spot has the room for)
			Check(FMath::Abs(WatchPlayer.Gs() - 1.5) < 0.05 && WatchPlayer.Rise >= FMath::Min(BlastRoom, OpenRise) * 0.9, FString::Printf(TEXT("that stick on the host's player, on the bot's spot, at the headset's gravity: %s, %.2f g (the launch's rise there: %.0f cm)"), *WatchPlayer.Say(), WatchPlayer.Gs(), FMath::Min(BlastRoom, OpenRise)));
			// (the flight: the same rise as the player's, within the bodies' tick of a sample, under the same measured gravity)
			Check(bOldPush ? WatchBot.Rise < 60.0 : FMath::Abs(WatchBot.Rise - WatchPlayer.Rise) <= FMath::Max(40.0, WatchPlayer.Rise * 0.03) && FMath::Abs(WatchBot.Gs() - WatchPlayer.Gs()) < 0.05,
				FString::Printf(TEXT("that stick on a live bot: %s, %.2f g%s (the player's: %s, %.2f g)"), *WatchBot.Say(), WatchBot.Gs(), bOldPush ? TEXT(", -OLOldBotPush") : TEXT(""), *WatchPlayer.Say(), WatchPlayer.Gs()));
			Table.Add(FString::Printf(TEXT("the flight: the bot rises %.0f cm at %.2f g, the player %.0f cm at %.2f g (a point-blank stick: %.0f cm at 1.5 g; %.0f cm of room)%s"),
				WatchBot.Rise, WatchBot.Gs(), WatchPlayer.Rise, WatchPlayer.Gs(), OpenRise, BlastRoom, bOldPush ? TEXT(", -OLOldBotPush") : TEXT("")));
			if (B) B->Health = 10.f;
			Phase = 4;
			return;
		case 4:
			if (!E2 || B2->bDead || !PuppetOf(BotIds[0])) return;     // (the scoped shot's bot, back)
			B2->Health = 1010.f;
			WatchBot.Begin(E2->GetActorLocation());
			TestSayTo(Client, TEXT("ml:near"), BotIds[0]);
			TestAt = Now + 1.5;
			Phase = 5;
			return;
		case 5:
			if (Now < TestAt) return;
			TestSayTo(Client, TEXT("ml:brhealth"), 100);
			WatchBot.Begin(E2 ? E2->GetActorLocation() : FVector::ZeroVector);
			TestSayTo(Client, TEXT("ml:brblast"), BotIds[0] + (bOldPush ? 100 : 0));     // (the switch is the host's: the client is told)
			TestAt = Now + 3.0;
			Phase = 6;
			return;
		case 6:
			if (E2) WatchBot.Note(E2->GetActorLocation(), E2->GetVelocity().Size());
			if (Now < TestAt) return;
			Check(bOldPush ? WatchBot.PeakSpeed < 800.0 : WatchBot.PeakSpeed > WatchPlayer.PeakSpeed * 0.5, FString::Printf(TEXT("the client's stick on a live bot, thrown on the host: %s"), *WatchBot.Say()));
			if (B2) B2->Health = 10.f;
			Phase = 7;
			return;
		case 7:
		{
			// a blast on a player in a match: 12, the client's and the host's
			const AOutlawsRemotePlayer* R = PuppetOf(Client);
			const FOutlawsDmPlayer* D = Dm.Find(Client);
			if (ClientDead())
			{
				// (the stick on a bot reached it, at 10 health: the spawns differ a run; back up first, past the protection)
				if (Now >= TestAt) { TestSayTo(Client, TEXT("ml:respawn"), 0); TestAt = Now + 8.0; }
				return;
			}
			if (!R || !D || !D->bAlive || Wall <= D->ProtectedUntil + 0.3) return;
			if (ClientHealth() < 50.f)
			{
				// (back up on 10: a blast of 12 only shows on more than it takes)
				if (Now >= TestAt) { TestSayTo(Client, TEXT("ml:brhealth"), 100); TestAt = Now + 1.0; }
				return;
			}
			Before = ClientHealth();
			StandBeside(R->ShownBody());
			L->TestSetHealth(100.f);            // (the host stands in this one too)
			L->Explode(R->ShownBody() + FVector(0.0, 0.0, 10.0));
			TestAt = Now + 1.5;
			Phase = 8;
			return;
		}
		case 8:
		{
			if (Now < TestAt) return;
			const float Lost = Before - ClientHealth();
			Check(FMath::IsNearlyEqual(Lost, 12.f, 1.0f), FString::Printf(TEXT("the host's stick on the client in a match: %.1f taken, health %.1f -> %.1f (a match stays at 12)"), Lost, Before, ClientHealth()));
			L->TestSetHealth(100.f);
			L->Explode(StickBeside(L, P->GetActorLocation(), Half, 40.0, P));
			Check(FMath::IsNearlyEqual(100.f - L->GetHealth(), 12.f, 0.5f), FString::Printf(TEXT("the host's own stick in a match: %.1f taken"), 100.f - L->GetHealth()));
			L->TestSetHealth(10.f);
			Finish();
			return;
		}
		default:
			return;
		}
	}
	case 2399:
		if (Now < TestAt) return;
		TestStep = 2400;
		QuitForTest();
		return;
	default:
		return;
	}
}

void UOutlawsCoop::OnBlastRifleTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	using namespace OutlawsBlastRifleProbeLocal;
	const UCameraComponent* Cam = P->Camera.Get();
	const FVector Eye = Cam ? Cam->GetComponentLocation() : P->GetActorLocation();
	if (Step == TEXT("brhealth")) { L->TestSetHealth(float(Arg)); return; }
	if (Step == TEXT("brshot"))
	{
		const int32 C = Arg / 16, Who = Arg % 16;
		// (the near step's spot can lie in a wall, and the game's unstuck then warps this body onto the target itself: a line straight
		// down its axis meets it at the waist, the spot low, x1 for x1.5.  The shot goes from 120 cm off then, as the near step has it)
		const AOutlawsRemotePlayer* Aimed = PuppetOf(Who);
		const FVector From = Aimed && FVector::Dist2D(Eye, Aimed->ShownBody()) < 60.0 ? Aimed->ShownBody() + FVector(120.0, 0.0, 60.0) : Eye;
		const int32 Hits = Fire(this, L, From, Who, Guns[C / 3], Spots[C % 3]);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT blastrifle client: its %s at %s on the %s: %d of %d pellets on the body"), Guns[C / 3].Name, Spots[C % 3].Name, WhoWord(Who), Hits, Guns[C / 3].Pellets);
		return;
	}
	if (Step == TEXT("brback"))
	{
		// (the pulls) 2.5 m off the body as drawn here, on a clear side, on its floor, facing it: the hold step's "near" keeps the
		// shooter's own height, and with a bot on another floor a real shot, from the muzzle, stops in the level (the jobs' shots
		// from the eye never ask the level)
		const AOutlawsRemotePlayer* R = PuppetOf(Arg);
		if (!R) return;
		const FVector Mid = R->ShownBody();
		FVector Flat = (P->GetActorLocation() - Mid).GetSafeNormal2D();
		if (Flat.IsNearlyZero()) Flat = FVector::ForwardVector;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsBlastRifleBack), true, P);
		for (int32 i = 0; i < 8; ++i)
		{
			const FVector Try = Flat.RotateAngleAxis(45.0 * i, FVector::UpVector);
			FHitResult Hit;
			if (P->GetWorld()->LineTraceSingleByChannel(Hit, Mid, Mid + Try * 320.0, ECC_Visibility, Q)) continue;
			Flat = Try;
			break;
		}
		P->SetActorLocation(Mid + Flat * 250.0 + FVector(0.0, 0.0, 60.0), false, nullptr, ETeleportType::TeleportPhysics);
		if (AController* Ct = P->GetController()) Ct->SetControlRotation((-Flat).Rotation());
		return;
	}
	if (Step == TEXT("brpull"))
	{
		// a real trigger pull: the hold step's gun held in both hands (the rifle's one-handed scatter of 2 degrees would leave the
		// chest's third at this range), its bore on the spot, and FireGun as a hand's trigger reaches it
		const int32 C = Arg / 16, Who = Arg % 16;
		const FGun& G = Guns[C / 3];
		const FSpotAim& S = Spots[C % 3];
		const AOutlawsRemotePlayer* R = PuppetOf(Who);
		const int32 Hand = P->LeftDominant() ? 0 : 1;
		if (!R) { UE_LOG(LogOutlaws, Display, TEXT("OLSHOT blastrifle %s: the %s is not drawn here"), IsHost() ? TEXT("host") : TEXT("client"), WhoWord(Who)); return; }
		const FVector Mid = R->ShownBody();
		const FVector Flat = (Mid - Eye).GetSafeNormal2D();
		P->Mag[Hand] = 6;
		P->FireCooldown[Hand] = 0.f;
		P->TutorialTestAimGun(Hand, Mid + FVector::CrossProduct(FVector::UpVector, Flat) * S.Across * BodyRadius + FVector(0.0, 0.0, S.Up));
		const int32 Sent = MatchHitsSent;
		P->TestSetTwoHand(Hand, true);
		P->FireGun(Hand, false);
		P->TestSetTwoHand(Hand, false);
		// (where it went: the tracer sits halfway along the shot, the spark at its end)
		const FVector Spark = P->SparkFx[Hand] ? P->SparkFx[Hand]->GetComponentLocation() : FVector::ZeroVector;
		const FVector Muzzle = P->TracerFx[Hand] ? P->TracerFx[Hand]->GetComponentLocation() * 2.0 - Spark : FVector::ZeroVector;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT blastrifle %s: its real pull, weapon %d, at the %s: %d rounds left, %d hits sent; muzzle %.0f cm off the body, the shot's end %.0f cm off it"),
			IsHost() ? TEXT("host") : TEXT("client"), P->Equipped[Hand], WhoWord(Who), P->Mag[Hand], MatchHitsSent - Sent, FVector::Dist(Muzzle, Mid), FVector::Dist(Spark, Mid));
		return;
	}
	if (Step == TEXT("brscoped"))
	{
		const FVector Head = HeadOf(this, Arg);
		const FVector Dir = (Head - Eye).GetSafeNormal();
		FVector At;
		const bool bHit = ShotAtPlayers(L, Eye, Head + Dir * 300.0, 2.f, At, true, true);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT blastrifle client: its scoped shot at the %s's head: %s"), WhoWord(Arg), bHit ? TEXT("a hit") : TEXT("a miss"));
		return;
	}
	if (Step == TEXT("brblast"))
	{
		const AOutlawsRemotePlayer* R = PuppetOf(Arg % 100);
		if (!R) return;
		SeenWho = Arg % 100;
		bSeenOld = Arg >= 100;
		SeenUntil = FPlatformTime::Seconds() + 2.6;
		WatchSeen.Begin(R->ShownBody());
		L->Explode(R->ShownBody() - FVector(0.0, 0.0, 40.0) + (Eye - R->ShownBody()).GetSafeNormal2D() * 100.0);
		return;
	}
	if (Step == TEXT("brwatch"))
	{
		// (the host's stick on a bot in the open: what this machine shows of the flight; Arg: the bot, 100 for -OLOldBotPush, the room in thousands)
		const AOutlawsRemotePlayer* R = PuppetOf(Arg % 100);
		if (!R) return;
		SeenWho = Arg % 100;
		bSeenOld = Arg % 1000 >= 100;
		bSeenOpen = true;
		SeenRoom = double(Arg / 1000);
		SeenUntil = FPlatformTime::Seconds() + 3.5;
		WatchSeen.Begin(R->ShownBody());
		return;
	}
}

void UOutlawsCoop::TickClientBlastRifleTest(AOutlawsLevelActor* L)
{
	using namespace OutlawsBlastRifleProbeLocal;
	if (SeenWho < 0) return;
	if (const AOutlawsRemotePlayer* R = PuppetOf(SeenWho)) WatchSeen.Note(R->ShownBody(), 0.0);
	if (FPlatformTime::Seconds() < SeenUntil) return;
	if (bSeenOpen)
	{
		const double Want = FMath::Min(SeenRoom, OpenRise);
		Check(bSeenOld ? WatchSeen.Rise < 60.0 : WatchSeen.Rise >= Want * 0.85 && WatchSeen.Rise <= OpenRise * 1.1,
			FString::Printf(TEXT("the client sees the host's stick throw a bot: %s%s (the launch's rise there: %.0f cm)"), *WatchSeen.Say(), bSeenOld ? TEXT(" (-OLOldBotPush on the host)") : TEXT(""), Want));
		bSeenOpen = false;
		SeenWho = -1;
		return;
	}
	const bool bThrown = WatchSeen.Rise >= 30.0 || WatchSeen.Carry >= 60.0;
	Check(bSeenOld ? !bThrown : bThrown, FString::Printf(TEXT("the client sees the bot's body after its stick: %s%s"), *WatchSeen.Say(), bSeenOld ? TEXT(" (-OLOldBotPush on the host)") : TEXT("")));
	Result(TEXT("client"));
	SeenWho = -1;
}

// ---- a story level ------------------------------------------------------------------------------------------------------------

OL_SHOT_MATCH(OLBlastRifleProbe) { return OLFlags::ValueIs(TEXT("OLBlastRifleProbe"), TEXT("story")); }

OL_SHOT_TEST(OLBlastRifleProbe, 2330)
{
	using namespace OutlawsBlastRifleProbeLocal;
	if (!L || ShotStep < 0) return;
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	if (ShotStep == 0)
	{
		if (ShotTime < 4.f || !bUp) return;
		L->StartSaveDelay = -1.f;
		L->AutosaveWait = 0.f;
		L->bInvincible = false;             // (-OLShots' own)
		const bool bOld = OLFlags::Param(TEXT("OLOldBlastPlayer"));
		const FString Lv = L->LevelName.ToLower();
		Check(!UOutlawsCoop::IsMatchActive(L), FString::Printf(TEXT("%s: no match"), *Lv));
		FVector At;
		const FVector Mid = GetActorLocation();
		Check(!UOutlawsCoop::ShotAtPlayers(L, Mid, Mid + GetActorForwardVector() * 5000.0, 2.f, At, false, true), TEXT("a rifle shot strikes no player"));
		// the distances in a row, the player put back between them: full within a quarter of the radius, then the square
		const double R = L->BlastRadiusCm();
		struct FCase { const TCHAR* Name; double Off; bool bEnemy; int32 Plate; };
		const FCase Blasts[] = { { TEXT("own stick, point blank"), 30.0, false, 0 }, { TEXT("a bad guy's stick, point blank"), 30.0, true, 0 }, { TEXT("own stick, 3.5 m"), 350.0, false, 0 },
			{ TEXT("own stick, 7 m"), 700.0, false, 0 }, { TEXT("own stick, 11 m"), 1100.0, false, 0 }, { TEXT("own stick, point blank, 50 plate"), 30.0, false, 50 } };
		const double Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		for (const FCase& K : Blasts)
		{
			SetActorLocation(Mid, false, nullptr, ETeleportType::TeleportPhysics);
			GetCharacterMovement()->StopMovementImmediately();
			L->TestSetHealth(100.f);
			L->SetArmour(K.Plate);
			const FVector Stick = StickBeside(L, Mid, Half, K.Off, this);
			const FVector From = Stick + FVector(0.0, 0.0, 20.0);
			const double D = FVector::Dist(From, Mid);
			const double Blast = 12.0 * (D <= R * 0.25 ? 1.0 : FMath::Square(FMath::Max(0.0, (R - D) / (0.75 * R))));
			const float Want = float(FMath::Max(0.0, Blast - K.Plate / 10.0) * (bOld ? 1.0 : 0.5));
			L->bEnemyBlast = K.bEnemy;
			L->Explode(Stick);
			L->bEnemyBlast = false;
			const float Lost = 100.f - L->GetHealth();
			Check(FMath::IsNearlyEqual(Lost, Want, 0.05f), FString::Printf(TEXT("%s: %s, %.0f cm: %.2f health lost, want %.2f%s"), *Lv, K.Name, D, Lost, Want, bOld ? TEXT(" (-OLOldBlastPlayer)") : TEXT("")));
		}
		L->SetArmour(0);
		L->TestSetHealth(10.f);
		FString Throwers;
		int32 Sticks = 0;
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
		{
			const AOutlawsEnemy* E = Wk.Get();
			if (!E || !E->IsThrower()) continue;
			++Sticks;
			if (Throwers.Len() < 120) Throwers += E->GetOutlawsName() + TEXT(" ");
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT blastrifle story %s: %d throwers placed (%s), sticks to kill from 10 health point blank: %d"), *Lv, Sticks, *Throwers, bOld ? 1 : 2);
		Result(TEXT("story"), bOld ? TEXT(", -OLOldBlastPlayer") : TEXT(""));
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
	}
}

#endif // WITH_OUTLAWS_TESTS
