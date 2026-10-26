// REQ-MPBOTS, the first step (Death Match): bots in a match.  A bot is a player the host runs: it takes a player slot (a
// link of its own, AOutlawsCoopLink::bBot, made by the host with no connection behind it), so the rules count it as any
// player (it joins the frag table, spawns at the match's spawn points, dies, is credited with kills and respawns), and every
// machine draws it from its link's pose and stats as it draws a remote player (the puppet, its character's sprite, the name
// and health over its head).  Its body and mind on the host are a bad guy's (AOutlawsEnemy: the movement, the hunting through
// the level's openings, the choosing among targets, the combat hook and its shots), unseen there (the puppet is what shows),
// with its capsule out of the bullets' way so every hit on it goes through the match as a hit on a player does.
//
// - The six characters' own stats, read from their ITMs in the player's game files (cidsanch, cidjames, cidfeath, ciddeath,
//   cidbob, cidmary): AIR_VEL_MAX (its run), AIR_ACCEL_X (its acceleration), AIR_YAWRATE (a slower turner aims worse on the
//   turn), DAMAGE_MULTIPLIER (what it takes, as for a player: MatchDamageFor), the START items (its kit), the choreographies.
//   James' ITM has no physics lines: the middle of the others' (INFERRED).  Health is a player's (10): no ITM gives one.
// - Its guns are the player's (damage, pellets, spread, range, sound: OutlawsPlayerWeapons.cpp GWeapons), chosen by the
//   character's preference among those it carries with ammunition (BotPrefs); while it sees someone, the one for the
//   distance (BOTSRANGE: the shotguns inside 6 m, the rifle past 12 m, the revolver between, TickBots).  Dynamite and knives (Spit'n Jack's and
//   Two-Feathers' kits, and pickups) are thrown (BotThrow): a stick lobbed at someone it sees 8 to 22 m off, a knife at 2.5 to
//   10 m, now and then by its skill; the flight drawn on every machine as a player's throw is, the host's own copy flying it
//   (TickBotThrown): the stick's blast is the level's (Explode), its harm dealt through the match and the bot credited
//   (UOutlawsCoop::BlastShooter).  The revolver never runs dry (a bot with no
//   ammunition would stand still: the one advantage it has over a player).
// - Beatable: aim worse than the bad guys' best (their 3 degrees at the Bad difficulty; a bot 1.2 to 2 times that and the gun's
//   own variance), slower to react (LOS_TIMER 0.35 to 0.6 s against 0.25), a little slower than a player on its feet.
// - Pickups: the map's items it walks over, when it has a use for them (a gun it lacks, ammunition for a gun it has, health
//   when hurt), claimed as a player's are (OnPickupClaim: gone on every machine).
// - Nobody in sight, it goes somewhere (BotDrive): to a gun it likes and lacks or health within 20 m, else after the nearest
//   opponent (BotHuntTarget), else to a far spawn point.  The way there is the level's own: the shortest way through its
//   sectors' openings (AOutlawsLevelActor::PathRoute, the openings a bad guy's PathStepToward takes; no navmesh), its corners
//   cut where its body walks straight on, a body's feel for posts and barrels in the way; stuck 4 s, a step aside and the way
//   made afresh (three times running: a far place for a few seconds).  The map's way points (spawn points and pickup places)
//   only where the sectors give no way.  Fallen under 40 % health (BOTSHURT), in a fight or not: health it has a way to
//   within 25 m, else a place out of its opponent's sight, for 10 s or until healed; the way walked by its body before it sets out and a place given up after a 2 s stall (BOTSTUCK).  Its brain is not one of the level's bad guys: the enemy throttle never tiers,
//   freezes or counts it (and IsThrottleExempt says so as for a boss).
// - Team Play: the rules put it on the smaller team (FOutlawsMatchModes::AddPlayer); it never hunts, aims at or hurts a
//   teammate (BotFoe).
// - Slots: bots take the highest player numbers; a human joining a full match takes the last bot's slot (BotsMakeRoom), and
//   when a human leaves a bot comes back, up to the host's count.
#include "OutlawsCoop.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsDeathmatch.h"
#include "OutlawsEnemy.h"
#include "OutlawsFormats.h"
#include "OutlawsFragTable.h"
#include "OutlawsGeometry.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsLevel.h"
#include "OutlawsLab.h"
#include "OutlawsLevelActor.h"
#include "OutlawsMpHost.h"
#include "OutlawsPlayer.h"
#include "OutlawsRemotePlayer.h"
#include "OutlawsSpriteActor.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"

namespace
{
	constexpr float BotHealthMax = 10.f;             // a player's full health (ApplyStartKit)
	constexpr double UnitCmB = FOutlawsGeometry::UnitsToCm;

	/** The player's guns as a bot fires them: the player's weapon table as built (AOutlawsPlayer::StockGun: damage, pellets
	 *  times barrels, spread, range, sound), plus the AI's time between bursts. */
	struct FBotGun
	{
		const TCHAR* Label;
		const TCHAR* Sound;
		float Damage;
		int32 Pellets;           // all the shot's pellets (a barrel's times the barrels fired)
		float Spread;
		float RangeCm;
		int32 AmmoType;          // 0 ibullit, 1 icart, 2 ishells
		int32 Rounds;            // ammunition a shot takes
		float Variance;          // the gun's own spread of the aim (degrees)
		float ShootTimer;        // seconds between the AI's bursts
		const TCHAR* Item;       // its inventory item
	};
	const float GBotShootTimer[5] = { 1.4f, 1.6f, 1.8f, 2.0f, 2.0f };      // (the bots' own: the player's guns have none)
	const TCHAR* const GAmmoItems[3] = { TEXT("ibullit"), TEXT("icart"), TEXT("ishells") };
	/** Gun g (0 revolver, 1 rifle, 2 shotgun, 3 double barrel, 4 sawed-off) as a bot fires it. */
	FBotGun BotGun(int32 g)
	{
		g = FMath::Clamp(g, 0, 4);
		AOutlawsPlayer::FStockGun W;
		AOutlawsPlayer::StockGun(g, W);
		int32 Ammo = 0;
		for (int32 a = 0; a < 3; ++a) if (W.AmmoItem && FCString::Stricmp(W.AmmoItem, GAmmoItems[a]) == 0) Ammo = a;
		return { W.Label, W.Sound, W.Damage, W.Pellets * W.Rounds, W.Spread, W.RangeCm, Ammo, W.Rounds, W.Variance, GBotShootTimer[g], W.Item };
	}
	constexpr int32 MaxAmmo = 100;
	constexpr int32 MaxSticks = 10, MaxKnives = 10;           // (dynamite and knives a bot carries: thrown, BotThrow)
	bool IsSticks(const FString& Item) { return Item.Contains(TEXT("dynam"), ESearchCase::IgnoreCase); }
	bool IsKnives(const FString& Item) { return Item.Contains(TEXT("knife"), ESearchCase::IgnoreCase); }

	/** Each character's liking for each gun (revolver, rifle, shotgun, double barrel, sawed-off), and its nerve (AGGRESSIVENESS). */
	struct FBotPref { const TCHAR* Short; int32 W[5]; int32 Aggr; };
	const FBotPref GBotPrefs[] = {
		{ TEXT("Mar"), { 25, 5, 60, 10, 0 }, 5 },        // Bloody Mary: mostly the shotgun, also pistols, rarely rifle or double barrel
		{ TEXT("Jam"), { 25, 75, 0, 0, 0 }, 4 },         // James Anderson: mostly the rifle, sometimes the pistol
		{ TEXT("Doc"), { 25, 75, 0, 0, 0 }, 4 },         // Dr. Death: as James
		{ TEXT("San"), { 55, 0, 10, 10, 25 }, 6 },       // Spit'n Jack: the quickest and the frailest (1.15): in close, revolver and sawed-off
		{ TEXT("Two"), { 35, 50, 15, 0, 0 }, 3 },        // Two-Feathers: a hunter, sturdier (0.90): rifle and revolver at range
		{ TEXT("Bob"), { 15, 0, 25, 60, 0 }, 5 } };      // Bob Graham: the slowest, starts with the double barrel: shotguns

	const FBotPref& PrefOf(const FString& Short)
	{
		for (const FBotPref& P : GBotPrefs) if (Short.Equals(P.Short, ESearchCase::IgnoreCase)) return P;
		return GBotPrefs[1];
	}

	/** A character's stats as its ITM gives them (read once from the player's files). */
	struct FBotChar
	{
		float VelMax = 30.f, AccelX = 194.f, YawRate = 206.f, DamageMult = 1.f;
		TArray<TPair<FString, int32>> Start;
		int32 Stand = 0, Walk = 1, Run = 1, Shoot = 10, Crouch = 3, Hit = 20, Die = 21;
		bool bPhysics = false;
		FString From;
	};

	const FBotChar& ReadBotChar(const AOutlawsLevelActor* L, const FOutlawsMpCharacter* Ch)
	{
		static TMap<FString, FBotChar> Cache;
		if (const FBotChar* C = Cache.Find(Ch->Short)) return *C;
		FBotChar& Out = Cache.Add(Ch->Short);
		FString Text, Func;
		TMap<FString, FString> D;
		const FOutlawsAssets* A = L ? L->GetLabAssets() : nullptr;
		if (!A || !A->GetText(Ch->Itm, Text))
		{
			UE_LOG(LogOutlawsAI, Warning, TEXT("OLBOT %s: %s not in the game's files: the middle stats"), Ch->Name, Ch->Itm);
			Out.DamageMult = Ch->DamageMult;
			return Out;
		}
		OutlawsFormats::ParseItm(Text, Func, D);
		auto F = [&D](const TCHAR* K, float& V) { if (const FString* S = D.Find(K)) { V = FCString::Atof(**S); return true; } return false; };
		auto I = [&D](const TCHAR* K, int32& V) { if (const FString* S = D.Find(K)) V = FCString::Atoi(**S); };
		Out.bPhysics = F(TEXT("AIR_VEL_MAX"), Out.VelMax);
		F(TEXT("AIR_ACCEL_X"), Out.AccelX);
		F(TEXT("AIR_YAWRATE"), Out.YawRate);
		if (!F(TEXT("DAMAGE_MULTIPLIER"), Out.DamageMult)) Out.DamageMult = 1.f;
		I(TEXT("STAND_CHOR"), Out.Stand);
		I(TEXT("WALK_CHOR"), Out.Walk);
		I(TEXT("RUN_CHOR"), Out.Run);
		I(TEXT("SHOOT_CHOR"), Out.Shoot);
		I(TEXT("CROUCHSTAND_CHOR"), Out.Crouch);
		I(TEXT("HIT_CHOR"), Out.Hit);
		I(TEXT("DIE_CHOR"), Out.Die);
		for (int32 i = 1; i <= 8; ++i)
		{
			const FString* It = D.Find(FString::Printf(TEXT("START_ITEM_%d"), i));
			if (!It) continue;
			int32 N = 1;
			I(*FString::Printf(TEXT("START_COUNT_%d"), i), N);
			Out.Start.Add({ It->TrimStartAndEnd().ToLower(), N });
		}
		FString Kit;
		for (const TPair<FString, int32>& S : Out.Start) Kit += FString::Printf(TEXT("%s%s %d"), Kit.IsEmpty() ? TEXT("") : TEXT(", "), *S.Key, S.Value);
		UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s from %s: AIR_VEL_MAX %.0f, AIR_ACCEL_X %.0f, AIR_YAWRATE %.0f%s, DAMAGE_MULTIPLIER %.2f; start %s; chors stand %d walk %d shoot %d crouch %d hit %d die %d"),
			Ch->Name, Ch->Itm, Out.VelMax, Out.AccelX, Out.YawRate, Out.bPhysics ? TEXT("") : TEXT(" (no physics lines: the others' middle)"), Out.DamageMult, *Kit,
			Out.Stand, Out.Walk, Out.Shoot, Out.Crouch, Out.Hit, Out.Die);
		return Out;
	}

	int32 GunOfItem(const FString& Item)
	{
		for (int32 g = 0; g < 5; ++g) if (Item.Equals(BotGun(g).Item, ESearchCase::IgnoreCase)) return g;
		if (Item.Contains(TEXT("saw"))) return 4;
		return -1;
	}

	int32 AmmoOfItem(const FString& Item)
	{
		for (int32 a = 0; a < 3; ++a) if (Item.Equals(GAmmoItems[a], ESearchCase::IgnoreCase)) return a;
		return -1;
	}

	float SkillMult(int32 Skill) { return Skill <= 0 ? 2.f : Skill >= 2 ? 1.2f : 1.5f; }

	// ---- BOTSRANGE: the gun for the distance while it sees someone (TickBots) ----
	// The bands: the shotguns inside 6 m, the rifle past 12 m, the revolver between (and wherever the band's gun is not carried).
	int32 RangeBand(float M) { return M < 6.f ? 0 : M > 12.f ? 2 : 1; }
	bool GunFits(int32 g, int32 Band) { return Band == 0 ? g >= 2 : Band == 2 ? g == 1 : g == 0; }
	template <typename TBot> bool GunUsable(const TBot& B, int32 g) { return B.bHas[g] && (g == 0 || B.Ammo[BotGun(g).AmmoType] >= BotGun(g).Rounds); }
	struct FRangeHold { int32 Band = -1; double SwapAt = -100.0; };
	TMap<int32, FRangeHold> GRangeHold;                      // each bot's band and its last swap by range (by player number)
	// (the test: shots by band, with the band's gun, with the band's gun carried; shots and distance by gun; swaps)
	int32 GRangeShots[3] = {}, GRangeRight[3] = {}, GRangeCould[3] = {}, GRangeRightCould[3] = {};
	int32 GGunShots[5] = {};
	double GGunShotM[5] = {};
	int32 GRangeSwaps = 0, GLikingSwaps = 0;

	// ---- BOTSHURT: under 40 % health a bot breaks off (BotHurt marks it, BotDrive's plan) ----
	// Health it has a way to within 25 m (by the sectors), else a place the one it fights cannot see; the hunt again when
	// healed or after 10 s.  -OLBotOldHurt: as before (the test's tallies still kept, for the A/B).
	constexpr float HurtFrac = 0.4f;
	constexpr double HurtReachCm = 2500.0, HurtFor = 10.0, HurtHealWithin = 15.0;
	struct FHurtRun
	{
		bool bPending = false, bActive = false, bReach = false, bHeaded = false, bHealed = false, bCover = false, bCounted = false;
		double Since = 0.0, LastEnd = -100.0, SeenAtCover = 0.0, NextShotAt = 0.0;
		int32 Pick = -1;
		FVector Dest = FVector::ZeroVector;
		FString What;
		// (BOTSTUCK) the stall clock on its way: the least left by the way, when and where it last came nearer or moved
		double ProgAt = 0.0, BestLeft = 1e12, LastTick = 0.0;
		FVector ProgFeet = FVector::ZeroVector, ClockDest = FVector::ZeroVector;
		int32 GiveUps = 0;
		TArray<FVector> BadSpots;                            // cover places given up on this break-off
	};
	TMap<int32, FHurtRun> GHurtRun;                          // each bot's break-off (by player number)
	// (the test: falls under 40 %, with health in reach at the fall, of those alive a second on (a chance to act) with health
	// in reach, headed for it, healed within 15 s, to cover, died hurt; health taken)
	int32 GHurtFalls = 0, GHurtReachAll = 0, GHurtReach = 0, GHurtHeaded = 0, GHurtHealed = 0, GHurtCover = 0, GHurtDied = 0, GHealthTaken = 0;
	bool OldHurt() { static const bool b = OLFlags::Param(TEXT("OLBotOldHurt")); return b; }

	// ---- BOTSTUCK: a hurt bot's way walked by its body before it sets out; a 2 s stall on it gives the place up ----
	// (BOTSHURT took stucks from 88 to 144 over 20 runs, most on the way to canteens and cover: ways the sectors pass and a
	// body does not, town_2p's canteen past the bars at sector 149, drygulch's corners round sector 182.)  -OLBotOldStuck: as
	// BOTSHURT left it.
	bool OldStuck() { static const bool b = OLFlags::Param(TEXT("OLBotOldStuck")); return b; }
	constexpr double HurtStallFor = 2.0;
	TMap<int64, double> GHurtNoWay;                          // a health pickup given up from a sector: no bot heads for it from there until then
	int64 NoWayKey(int32 Pick, int32 Sector) { return (int64(Pick) << 32) | int64(uint32(Sector)); }
	int32 GHurtGiveUps = 0, GHurtBlocked = 0, GHurtEnded = 0;

	// ---- the way points: the map's spawn points and pickup places, joined where a player's body walks straight between ----
	struct FBotNav
	{
		int32 Serial = -1;
		TArray<FVector> Nodes;                               // feet
		TArray<TArray<TPair<int32, float>>> Links;
	};
	FBotNav GNav;
	TMap<int32, FVector> GWaypoint;                          // each bot's next way point (by player number)
	TMap<int32, double> GDoorTryAt;                          // when each bot may next push at a door in its way
	struct FTrapWatch { double Since = 0.0; FVector Feet = FVector::ZeroVector; int32 Stucks = 0; };
	TMap<int32, FTrapWatch> GTrapWatch;                      // each bot's stucks within 6 m of one place: shut in
	TMap<int32, double> GNoWaySince;                         // since when each bot has had no way through the sectors
	int32 GBotsFreed = 0;

	int32 GBotTeamRefused = 0, GBotTeamHits = 0;             // (the test, a team class: a bot's shot at a teammate turned away; a teammate's hit on a bot)
	int32 GWalkFail[4] = {};                                 // (the way points' log: a step too high, a drop, something in the way, no ground)

	/** A player's body walks straight from one place to the other, over the ground as it rises and falls: no step up over
	 *  70 cm, no drop over its MAX_DROP, nothing in the way of its body, ground all along. */
	bool WalkClear(UWorld* W, const FVector& FromFeet, const FVector& ToFeet, const AActor* Ignore)
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsBotWay), false, Ignore);
		FCollisionResponseParams Resp;
		Resp.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
		Resp.CollisionResponse.SetResponse(ECC_GameTraceChannel2, ECR_Ignore);
		const FCollisionShape Body = FCollisionShape::MakeCapsule(25.f, 45.f);
		const FVector Up(0.0, 0.0, 95.0);
		const double Len = FVector::Dist2D(FromFeet, ToFeet);
		const int32 N = FMath::Clamp(FMath::CeilToInt(Len / 150.0), 1, 60);
		FVector Prev = FromFeet;
		for (int32 i = 1; i <= N; ++i)
		{
			FVector P = FMath::Lerp(FromFeet, ToFeet, double(i) / N);
			FHitResult Hit;
			const double Top = FMath::Max(Prev.Z, P.Z) + 150.0;
			if (!W->LineTraceSingleByChannel(Hit, FVector(P.X, P.Y, Top), FVector(P.X, P.Y, Top - 900.0), ECC_Visibility, Q)) { ++GWalkFail[3]; return false; }
			const FVector G = Hit.ImpactPoint;
			if (G.Z - Prev.Z > 70.0) { ++GWalkFail[0]; return false; }
			if (Prev.Z - G.Z > 450.0) { ++GWalkFail[1]; return false; }
			if (W->SweepTestByChannel(Prev + Up, G + Up, FQuat::Identity, ECC_Pawn, Body, Q, Resp)) { ++GWalkFail[2]; return false; }
			Prev = G;
		}
		return FMath::Abs(Prev.Z - ToFeet.Z) < 120.0;
	}

	/** (BOTSTUCK) a body fits along from one place to the other over the ground as it rises and falls: only what stands in
	 *  its way counts (bars, a fence, a corner the sectors cut), not the steps or drops the sector way already judged; a
	 *  door is no stop (it opens), nor a wall the sweep begins inside.  A corner grazed is no stop either: the body slides
	 *  60 cm off it, as a bot's steering does along a wall (up to three times a leg); a wall across the way stays one. */
	FString GBodyWhy;                                        // (what stopped the last BodyClear, for the log)
	bool BodyClear(UWorld* W, const FVector& FromFeet, const FVector& ToFeet, const AActor* Ignore)
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsBotBody), false, Ignore);
		FCollisionResponseParams Resp;
		Resp.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
		const FCollisionShape Body = FCollisionShape::MakeCapsule(22.f, 40.f);
		const FVector Up(0.0, 0.0, 110.0);                    // (its lowest at 70 cm: over any step it climbs)
		const int32 N = FMath::Clamp(FMath::CeilToInt(FVector::Dist2D(FromFeet, ToFeet) / 120.0), 1, 40);
		FVector Prev = FromFeet, Off = FVector::ZeroVector;
		int32 Slides = 0;
		for (int32 i = 1; i <= N; ++i)
		{
			FVector P = FMath::Lerp(FromFeet, ToFeet, double(i) / N) + Off;
			FHitResult Hit;
			const double Top = FMath::Max(Prev.Z, P.Z) + 150.0;
			if (W->LineTraceSingleByChannel(Hit, FVector(P.X, P.Y, Top), FVector(P.X, P.Y, Top - 900.0), ECC_Visibility, Q)) P = Hit.ImpactPoint;
			else P.Z = Prev.Z;
			if (W->SweepSingleByChannel(Hit, Prev + Up, P + Up, FQuat::Identity, ECC_Pawn, Body, Q, Resp))
			{
				const UPrimitiveComponent* C = Hit.GetComponent();
				// (begun inside a wall: a way point set on a corner it rounds, not a stop)
				const bool bDoor = Hit.bStartPenetrating || (C && C->GetName().StartsWith(TEXT("OutlawsDoor")));
				const FVector Away = FVector(Hit.Normal.X, Hit.Normal.Y, 0.0).GetSafeNormal() * 60.0;
				if (!bDoor && Slides < 3 && !Away.IsNearlyZero() && !W->SweepTestByChannel(Prev + Up, Prev + Away + Up, FQuat::Identity, ECC_Pawn, Body, Q, Resp)
					&& !W->SweepTestByChannel(Prev + Away + Up, P + Away + Up, FQuat::Identity, ECC_Pawn, Body, Q, Resp))
				{
					++Slides;
					Off += Away;
					P += Away;
				}
				else if (!bDoor)
				{
					GBodyWhy = FString::Printf(TEXT("%s / %s at %s%s, %.0f m along"), Hit.GetActor() ? *Hit.GetActor()->GetName() : TEXT("?"), C ? *C->GetName() : TEXT("?"),
						*FVector(Hit.ImpactPoint).ToCompactString(), Hit.bStartPenetrating ? TEXT(" (inside at the start)") : TEXT(""), FVector::Dist2D(FromFeet, Prev) / 100.0);
					return false;
				}
			}
			Prev = P;
		}
		return true;
	}

	/** (BOTSTUCK) BodyClear along a sector way from Feet: each leg in turn. */
	bool RouteBodyClear(UWorld* W, const FVector& Feet, const TArray<FVector>& Route, const AActor* Ignore)
	{
		FVector From = Feet;
		for (const FVector& P : Route)
		{
			if (FVector::Dist2D(From, P) > 10.0 && !BodyClear(W, From, P, Ignore)) { GBodyWhy = FString::Printf(TEXT("leg %d of %d: "), int32(&P - Route.GetData()), Route.Num()) + GBodyWhy; return false; }
			From = P;
		}
		return true;
	}

	void BuildNav(UWorld* W, FOutlawsDeathmatch& Dm, int32 Serial)
	{
		GNav = FBotNav();
		GNav.Serial = Serial;
		GWaypoint.Reset();
		auto Add = [](const FVector& P) { for (const FVector& O : GNav.Nodes) if (FVector::Dist(O, P) < 150.0) return; GNav.Nodes.Add(P); };
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsBotNav), false);
		for (const FOutlawsDmSpawn& S : Dm.GetSpawns())
		{
			FHitResult Hit;
			Add(W->LineTraceSingleByChannel(Hit, S.Location + FVector(0.0, 0.0, 50.0), S.Location - FVector(0.0, 0.0, 400.0), ECC_Visibility, Q) ? FVector(Hit.ImpactPoint) : S.Location - FVector(0.0, 0.0, 88.0));
		}
		for (const FOutlawsDmItemGen& G : Dm.GetItems()) Add(G.Location);
		GNav.Links.SetNum(GNav.Nodes.Num());
		int32 Links = 0;
		for (int32 a = 0; a < GNav.Nodes.Num(); ++a)
		{
			for (int32 b = a + 1; b < GNav.Nodes.Num(); ++b)
			{
				const float D = float(FVector::Dist(GNav.Nodes[a], GNav.Nodes[b]));
				if (D > 6000.f || !WalkClear(W, GNav.Nodes[a], GNav.Nodes[b], nullptr)) continue;
				GNav.Links[a].Add({ b, D });
				GNav.Links[b].Add({ a, D });
				++Links;
			}
		}
		int32 Lone = 0;
		for (const TArray<TPair<int32, float>>& L : GNav.Links) if (L.Num() == 0) ++Lone;
		UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT the map's way points: %d (its spawn points and pickup places), %d walkable links, %d with none (not joined: %d a step too high, %d a drop, %d something in the way, %d no ground)"),
			GNav.Nodes.Num(), Links, Lone, GWalkFail[0], GWalkFail[1], GWalkFail[2], GWalkFail[3]);
	}

	/** The next place on the way from Feet to Final: Final itself when straight there is clear, else the first way point of
	 *  the shortest way over the points; false when the points do not join them. */
	bool NextWaypoint(UWorld* W, const FVector& Feet, const FVector& Final, const AActor* Ignore, FVector& Out)
	{
		if (WalkClear(W, Feet, Final, Ignore)) { Out = Final; return true; }
		const int32 N = GNav.Nodes.Num();
		if (N == 0) return false;
		TArray<float> Dist;
		TArray<int32> Prev;
		TArray<bool> Done;
		Dist.Init(TNumericLimits<float>::Max(), N);
		Prev.Init(-2, N);
		Done.Init(false, N);
		// (the nearest few in reach of it, and of the end: a handful of looks a plan, not every point's)
		TArray<int32> ByNear;
		for (int32 i = 0; i < N; ++i) ByNear.Add(i);
		ByNear.Sort([&](int32 A, int32 B) { return FVector::DistSquared(Feet, GNav.Nodes[A]) < FVector::DistSquared(Feet, GNav.Nodes[B]); });
		int32 Seen = 0;
		for (int32 k = 0; k < ByNear.Num() && Seen < 6; ++k)
		{
			const int32 i = ByNear[k];
			const float D = float(FVector::Dist(Feet, GNav.Nodes[i]));
			if (D > 3000.f) break;
			if (WalkClear(W, Feet, GNav.Nodes[i], Ignore)) { Dist[i] = D; Prev[i] = -1; ++Seen; }
		}
		for (int32 k = 0; k < N; ++k)
		{
			int32 U = -1;
			for (int32 i = 0; i < N; ++i) if (!Done[i] && Prev[i] != -2 && (U < 0 || Dist[i] < Dist[U])) U = i;
			if (U < 0) break;
			Done[U] = true;
			for (const TPair<int32, float>& E : GNav.Links[U])
			{
				if (Dist[U] + E.Value < Dist[E.Key]) { Dist[E.Key] = Dist[U] + E.Value; Prev[E.Key] = U; }
			}
		}
		int32 Best = -1;
		float BestCost = TNumericLimits<float>::Max();
		ByNear.Sort([&](int32 A, int32 B) { return FVector::DistSquared(Final, GNav.Nodes[A]) < FVector::DistSquared(Final, GNav.Nodes[B]); });
		int32 Tried = 0;
		for (int32 k = 0; k < ByNear.Num() && Tried < 6; ++k)
		{
			const int32 i = ByNear[k];
			if (Prev[i] == -2) continue;
			const float Tail = float(FVector::Dist(GNav.Nodes[i], Final));
			if (Tail > 3000.f) break;
			++Tried;
			if (Dist[i] + Tail >= BestCost || !WalkClear(W, GNav.Nodes[i], Final, Ignore)) continue;
			BestCost = Dist[i] + Tail;
			Best = i;
		}
		if (Best < 0) return false;
		while (Prev[Best] >= 0) Best = Prev[Best];
		Out = GNav.Nodes[Best];
		return true;
	}
}

// ---- who is a bot --------------------------------------------------------------------------------------------------------

UOutlawsCoop::FBot* UOutlawsCoop::FindBot(int32 Who)
{
	return Bots.FindByPredicate([Who](const FBot& B) { return B.Who == Who; });
}

bool UOutlawsCoop::IsBot(int32 Who) const
{
	return Bots.ContainsByPredicate([Who](const FBot& B) { return B.Who == Who; });
}

int32 UOutlawsCoop::HumanCount() const
{
	TArray<AOutlawsCoopLink*> Links;
	GetLinks(Links);
	int32 N = 0;
	for (const AOutlawsCoopLink* K : Links) if (!K->bBot) ++N;
	return FMath::Max(1, N);
}

FString UOutlawsCoop::BotLine(const FBot& B) const
{
	FString Has;
	for (int32 g = 0; g < 5; ++g) if (B.bHas[g]) Has += FString::Printf(TEXT("%s%s"), Has.IsEmpty() ? TEXT("") : TEXT("/"), BotGun(g).Label);
	return FString::Printf(TEXT("%s (player %d): %s, health %.1f, holding %s [%s], ammo %d/%d/%d, shots %d, pickups %d"), *B.Name, B.Who, B.bDead ? TEXT("dead") : TEXT("alive"), B.Health,
		BotGun(B.Weapon).Label, *Has, B.Ammo[0], B.Ammo[1], B.Ammo[2], B.ShotsFired, B.Pickups);
}

// ---- in and out ------------------------------------------------------------------------------------------------------------

void UOutlawsCoop::AddBot(AOutlawsLevelActor* L)
{
	UWorld* W = CurrentWorld();
	if (!W || !L) return;
	// the highest free player number (humans take the lowest)
	TArray<AOutlawsCoopLink*> Links;
	GetLinks(Links);
	int32 Who = -1;
	for (int32 k = MaxPlayers - 1; k >= 1; --k)
	{
		if (!Links.ContainsByPredicate([k](const AOutlawsCoopLink* K) { return K->PlayerIndex == k; })) { Who = k; break; }
	}
	if (Who < 0) return;
	// a character none of the others has, while there are some (then any)
	int32 N = 0;
	const FOutlawsMpCharacter* All = FOutlawsMpCharacter::All(N);
	TArray<int32> Free;
	for (int32 i = 0; i < N; ++i) if (!Bots.ContainsByPredicate([&](const FBot& B) { return B.Short == All[i].Short; })) Free.Add(i);
	int32 Pick = Free.Num() > 0 ? Free[FMath::RandHelper(Free.Num())] : FMath::RandHelper(N);
	// (tests: -OLMpBotChars=San,Two picks the characters, in order, while free)
	FString Wanted;
	if (OLFlags::Value(TEXT("OLMpBotChars"), Wanted))
	{
		TArray<FString> Shorts;
		Wanted.ParseIntoArray(Shorts, TEXT(","));
		for (const FString& Want : Shorts)
		{
			const int32 i = Free.IndexOfByPredicate([&](int32 f) { return Want.Equals(All[f].Short, ESearchCase::IgnoreCase); });
			if (i != INDEX_NONE) { Pick = Free[i]; break; }
		}
	}
	const FOutlawsMpCharacter& Ch = All[Pick];
	FString Name = Ch.Name;
	for (int32 n = 2; Bots.ContainsByPredicate([&](const FBot& B) { return B.Name == Name; }); ++n) Name = FString::Printf(TEXT("%s %d"), Ch.Name, n);
	FActorSpawnParameters SP;
	SP.ObjectFlags |= RF_Transient;
	AOutlawsCoopLink* K = W->SpawnActor<AOutlawsCoopLink>(AOutlawsCoopLink::StaticClass(), FTransform::Identity, SP);
	if (!K) return;
	K->bBot = true;
	K->PlayerIndex = Who;
	K->PlayerName = Name;
	K->Character = Ch.Short;
	K->BuiltSerial = StartedSerial;
	K->ForceNetUpdate();
	FBot& B = Bots.AddDefaulted_GetRef();
	B.Who = Who;
	B.Short = Ch.Short;
	B.Name = Name;
	B.Link = K;
	B.bDead = true;                  // (until the rules spawn it: BotSpawn)
	const FBotChar& St = ReadBotChar(L, &Ch);
	const FBotPref& Pf = PrefOf(Ch.Short);
	UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s joins as player %d (%s, %s): speed %.0f, turn %.0f, takes x%.2f; likes revolver %d, rifle %d, shotgun %d, double barrel %d, sawed-off %d (%d humans, %d bots, max %d)"),
		*Name, Who, Ch.Short, OutlawsMpHost::BotSkillLabel(MatchOptions.BotSkill), St.VelMax, St.YawRate, St.DamageMult, Pf.W[0], Pf.W[1], Pf.W[2], Pf.W[3], Pf.W[4], HumanCount(), Bots.Num(), MaxPlayersSetting());
}

void UOutlawsCoop::RemoveBot(int32 Who, const TCHAR* Why)
{
	const int32 i = Bots.IndexOfByPredicate([Who](const FBot& B) { return B.Who == Who; });
	if (i < 0) return;
	FBot& B = Bots[i];
	UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s (player %d) leaves: %s"), *B.Name, B.Who, Why);
	if (AOutlawsEnemy* E = B.Brain.Get()) E->Destroy();
	if (AOutlawsCoopLink* K = B.Link.Get()) K->Destroy();
	Bots.RemoveAt(i);
}

void UOutlawsCoop::RemoveAllBots(const TCHAR* Why)
{
	while (Bots.Num() > 0) RemoveBot(Bots.Last().Who, Why);
}

void UOutlawsCoop::BotsMakeRoom()
{
	if (!IsHost() || Bots.Num() == 0) return;
	if (HumanCount() + 1 + Bots.Num() > MaxPlayersSetting()) RemoveBot(Bots.Last().Who, TEXT("a human joins and needs the slot"));
}

// ---- the brain -----------------------------------------------------------------------------------------------------------

void UOutlawsCoop::BotSpawn(int32 Who, const FVector& At, float Yaw)
{
	FBot* B = FindBot(Who);
	AOutlawsLevelActor* L = FindLevel();
	UWorld* W = CurrentWorld();
	const FOutlawsMpCharacter* Ch = B ? FOutlawsMpCharacter::Find(B->Short) : nullptr;
	if (!B || !L || !W || !Ch) return;
	if (AOutlawsEnemy* Old = B->Brain.Get()) Old->Destroy();
	B->Brain.Reset();
	const bool bWasDead = B->bDead && B->DiedAt > 0.0;
	const FBotChar& St = ReadBotChar(L, Ch);
	const FBotPref& Pf = PrefOf(B->Short);
	const int32 Skill = FMath::Clamp(MatchOptions.BotSkill, 0, 2);
	const float Sk = SkillMult(Skill);
	// its feet on the floor under the spawn point
	FVector Feet = At - FVector(0.0, 0.0, 88.0);
	{
		FHitResult Hit;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsBotFloor), false);
		if (W->LineTraceSingleByChannel(Hit, At + FVector(0.0, 0.0, 50.0), At - FVector(0.0, 0.0, 400.0), ECC_Visibility, Q)) Feet = Hit.ImpactPoint;
	}
	// the bad guy's own ITM fields, as the character's (BGY_New's defaults under them)
	TMap<FString, FString> D;
	auto Put = [&D](const TCHAR* K, const FString& V) { D.Add(K, V); };
	Put(TEXT("HEIGHT"), TEXT("6.0"));
	Put(TEXT("RADIUS"), TEXT("1.0"));
	Put(TEXT("HEALTH"), FString::SanitizeFloat(BotHealthMax));
	Put(TEXT("AGGRESSIVENESS"), FString::FromInt(FMath::Clamp(Pf.Aggr + Skill - 1, 1, 8)));
	Put(TEXT("WATCH_CHOR"), FString::FromInt(St.Stand));
	Put(TEXT("WALK_CHOR"), FString::FromInt(St.Walk));
	Put(TEXT("RUN_CHOR"), FString::FromInt(St.Run));
	Put(TEXT("DUCK_CHOR"), FString::FromInt(St.Crouch));
	Put(TEXT("SHOOT_CHOR"), FString::FromInt(St.Shoot));
	Put(TEXT("WALK_SHOOT_CHOR"), FString::FromInt(St.Shoot));
	Put(TEXT("RUN_SHOOT_CHOR"), FString::FromInt(St.Shoot));
	Put(TEXT("HIT_CHOR"), FString::FromInt(St.Hit));
	Put(TEXT("CHEST_DIE_CHOR"), FString::FromInt(St.Die));
	Put(TEXT("LEG_DIE_CHOR"), FString::FromInt(St.Die));
	Put(TEXT("HIT_SOUND"), TEXT(""));
	Put(TEXT("DIE_SOUND"), TEXT(""));
	FActorSpawnParameters SP;
	SP.ObjectFlags |= RF_Transient;
	SP.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
	AOutlawsEnemy* E = W->SpawnActor<AOutlawsEnemy>(AOutlawsEnemy::StaticClass(), Feet + FVector(0.0, 0.0, 92.0), FRotator(0.0, Yaw, 0.0), SP);
	if (!E)
	{
		UE_LOG(LogOutlawsAI, Warning, TEXT("OLBOT %s: no room for its body at %s"), *B->Name, *At.ToCompactString());
		return;
	}
	// its sprite: the character's, every choreography timed as the bad guys' are (unseen: the puppet is what shows)
	AOutlawsSpriteActor* Spr = MakePuppetSprite(L, Feet, Yaw, Ch->Nwx);
	if (Spr) Spr->SetActorHiddenInGame(true);
	E->Init(L, B->Name, D, Spr, Yaw);
	E->CoopSetup(this, -1, true);
	E->CoopSelf = Who;
	E->bHunter = true;
	E->SetActorHiddenInGame(true);
	// the bullets pass its capsule (a hit is the match's, on its puppet); it still stands in the way of bodies
	E->GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_Visibility, ECR_Ignore);
	E->GetCapsuleComponent()->SetCollisionResponseToChannel(ECC_GameTraceChannel2, ECR_Ignore);     // (the FENCE walls hold bad guys, not players)
	// the character's feet and nerve, a little under a player's; the aim worse than the bad guys' best
	const float RunU = St.VelMax * 0.8f * (Skill == 0 ? 0.9f : 1.f);
	E->Def.RunSpeed = RunU;
	E->Def.WalkSpeed = RunU * 0.8f;
	E->Def.Health = BotHealthMax;
	E->Health = BotHealthMax;
	E->Def.LosTimer = Skill == 0 ? 0.6f : Skill == 2 ? 0.35f : 0.45f;
	E->Def.Confidence = Skill == 0 ? 0.6f : Skill == 2 ? 0.9f : 0.75f;
	E->Def.MoveInaccuracy = 15.f * Sk * FMath::Clamp(206.f / FMath::Max(100.f, St.YawRate), 0.8f, 1.3f);
	E->Def.PanicInaccuracy = 7.f;
	E->Def.FovDist = 300.f;
	E->Def.Aggressiveness = FMath::Clamp(Pf.Aggr + Skill - 1, 1, 8);        // (after the level's difficulty pass, which scaled it)
	E->Def.TauntTimer = 1000.f;
	E->Def.ShootSound.Reset();
	E->Def.LosTaunts.Reset();
	E->Def.NoLosTaunts.Reset();
	E->Def.AlertTaunts.Reset();
	E->Def.DropItem.Reset();
	if (UCharacterMovementComponent* M = E->GetCharacterMovement())
	{
		M->MaxWalkSpeed = E->Def.WalkSpeed * UnitCmB;
		M->MaxAcceleration = 1500.f * FMath::Clamp(St.AccelX / 205.f, 0.6f, 1.2f);
	}
	B->Brain = E;
	// the start kit (the ITM's), full health
	B->Health = BotHealthMax;
	B->bDead = false;
	B->DiedAt = 0.0;
	FMemory::Memzero(B->bHas);
	FMemory::Memzero(B->Ammo);
	B->Sticks = 0;
	B->Knives = 0;
	B->bHas[0] = true;
	for (const TPair<FString, int32>& S : St.Start)
	{
		const int32 G = GunOfItem(S.Key), A = AmmoOfItem(S.Key);
		if (G >= 0) B->bHas[G] = true;
		if (A >= 0) B->Ammo[A] = FMath::Min(MaxAmmo, B->Ammo[A] + S.Value);
		if (IsSticks(S.Key)) B->Sticks = FMath::Min(MaxSticks, B->Sticks + S.Value);
		if (IsKnives(S.Key)) B->Knives = FMath::Min(MaxKnives, B->Knives + S.Value);
	}
	BotChooseWeapon(*B, TEXT("spawn"));
	// BUG-CHARLOADOUT: the first gun is the ITM's START_WEAPON when it is one it fires (Mary's shotgun, Bob's double barrel)
	const FOutlawsCharKit& Kit = FOutlawsCharKit::Of(L->GetLabAssets(), B->Short);
	for (int32 g = 0; g < 5 && Kit.StartSlot >= 2; ++g)
	{
		FString Text, Func;
		TMap<FString, FString> GunItm;
		if (!B->bHas[g] || !L->GetLabAssets()->GetText(FString(BotGun(g).Item) + TEXT(".itm"), Text)) continue;
		OutlawsFormats::ParseItm(Text, Func, GunItm);
		if (FCString::Atoi(*GunItm.FindRef(TEXT("SLOT"))) != Kit.StartSlot) continue;
		if (g != B->Weapon && (g == 0 || B->Ammo[BotGun(g).AmmoType] >= BotGun(g).Rounds))
		{
			B->Weapon = g;
			BotApplyWeapon(*B);
			UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s takes the %s first (START_WEAPON %d)"), *B->Name, BotGun(g).Label, Kit.StartSlot);
		}
		break;
	}
	if (const AOutlawsEnemy* Body = B->Brain.Get()) UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s fires the %s: %s, %.2f x %d"), *B->Name, BotGun(B->Weapon).Label, *Body->Def.ShootSound, Body->Def.Damage, Body->Def.ShotMultiples);
	if (bWasDead) ++BotRespawns;
	BotPublish(*B, FPlatformTime::Seconds());
	UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s %s at %s as %s: %s"), *B->Name, bWasDead ? TEXT("respawns") : TEXT("spawns"), *Feet.ToCompactString(), Ch->Name, *BotLine(*B));
}

void UOutlawsCoop::BotApplyWeapon(FBot& B)
{
	AOutlawsEnemy* E = B.Brain.Get();
	if (!E) return;
	const FBotGun& G = BotGun(B.Weapon);
	const float Sk = SkillMult(MatchOptions.BotSkill);
	E->Def.Damage = G.Damage;
	E->Def.ShotMultiples = G.Pellets;
	E->Def.Spread = G.Spread;
	E->Def.Range = G.RangeCm / UnitCmB;
	E->Def.ShootTimer = G.ShootTimer * (MatchOptions.BotSkill <= 0 ? 1.4f : MatchOptions.BotSkill >= 2 ? 1.f : 1.15f);
	E->Def.WeaponInaccuracy = 3.f * Sk + G.Variance;
	E->Def.ShootSound = G.Sound;
	// BUG-CHARLOADOUT: the character's own copy of the gun (Sanchez's revolver BADGUN and 2.5, Mary's pistol3, Bob's double
	// barrel 0.75 a pellet): its sound, damage, pellets and spread, heard so on every machine (the shot's sound travels with it)
	const AOutlawsLevelActor* L = FindLevel();
	const FOutlawsAssets* A = L ? L->GetLabAssets() : nullptr;
	const FOutlawsCharKit& Kit = FOutlawsCharKit::Of(A, B.Short);
	const FString Own = Kit.bRead ? Kit.WeaponVariant(A, G.Item) : FString(G.Item);
	FString Text, Func;
	TMap<FString, FString> D;
	if (!FString(B.Short).Equals(TEXT("Jam")) && Own != G.Item && A && A->GetText(Own + TEXT(".itm"), Text))
	{
		OutlawsFormats::ParseItm(Text, Func, D);
		const FString Snd = D.FindRef(TEXT("FIRE_SOUND_1")).ToLower();
		if (!Snd.IsEmpty() && Snd != TEXT("null")) E->Def.ShootSound = Snd;
		if (const FString* V = D.Find(TEXT("DAMAGE_1"))) E->Def.Damage = FCString::Atof(**V);
		if (const FString* V = D.Find(TEXT("SHOT_MULTIPLES"))) E->Def.ShotMultiples = FMath::Max(1, FCString::Atoi(**V)) * G.Rounds;
		if (const FString* V = D.Find(TEXT("SPREAD_1"))) E->Def.Spread = FCString::Atof(**V);
	}
}

void UOutlawsCoop::BotChooseWeapon(FBot& B, const TCHAR* Why)
{
	const FBotPref& Pf = PrefOf(B.Short);
	auto Usable = [&B](int32 g) { return B.bHas[g] && (g == 0 || B.Ammo[BotGun(g).AmmoType] >= BotGun(g).Rounds); };
	int32 Total = 0;
	for (int32 g = 0; g < 5; ++g) if (Usable(g)) Total += Pf.W[g];
	int32 Pick = 0;
	if (Total > 0)
	{
		int32 R = FMath::RandHelper(Total);
		for (int32 g = 0; g < 5; ++g)
		{
			if (!Usable(g)) continue;
			if (R < Pf.W[g]) { Pick = g; break; }
			R -= Pf.W[g];
		}
	}
	const int32 Was = B.Weapon;
	B.Weapon = Pick;
	BotApplyWeapon(B);
	if (Was != Pick) UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s takes the %s (%s; was the %s)"), *B.Name, BotGun(Pick).Label, Why, BotGun(Was).Label);
}

void UOutlawsCoop::BotPublish(FBot& B, double Now)
{
	AOutlawsCoopLink* K = B.Link.Get();
	if (!K) return;
	K->BuiltSerial = StartedSerial;
	if (AOutlawsEnemy* E = B.Brain.Get(); E && !B.bDead)
	{
		const UCapsuleComponent* Cap = E->GetCapsuleComponent();
		const float Half = Cap->GetScaledCapsuleHalfHeight();
		const FVector Body = E->GetActorLocation();
		const FRotator Face(0.0, E->GetHeading(), 0.0);
		FOutlawsCoopPose& P = K->Pose;
		P.Body = Body;
		P.BodyYaw = Face.Yaw;
		P.HalfHeight = Half;
		P.Head = Body + FVector(0.0, 0.0, Half * 0.8);
		P.HeadRot = Face;
		P.RightHand = Body + Face.Vector() * 30.0 + FVector(0.0, 0.0, Half * 0.3);
		P.RightRot = Face;
		P.LeftHand = Body + Face.Vector() * 20.0 + FVector(0.0, 0.0, Half * 0.1);
		P.LeftRot = Face;
		P.RightWeapon = B.Weapon;
		P.LeftWeapon = -1;
		P.bVR = false;
		P.GroundSpeed = float(E->GetVelocity().Size2D());
		static const double ClockBase = FPlatformTime::Seconds();
		P.SentAt = float(Now - ClockBase);
		P.Seq = ++B.PoseSeq;
	}
	FOutlawsCoopStats S = K->Stats;
	S.Health = B.Health;
	S.MaxHealth = BotHealthMax;
	S.Armour = 0;
	S.bDead = B.bDead;
	S.bInMenu = false;
	// (BUG-MPDEATHSOUND: dead, its character's SND_DIE, which every machine plays from its body)
	const AOutlawsLevelActor* Lv = B.bDead ? FindLevel() : nullptr;
	S.DieSound = B.bDead ? FOutlawsCharKit::DieSoundOf(Lv ? Lv->GetLabAssets() : nullptr, B.Short) : FString();
	if (S.Health != K->Stats.Health || S.bDead != K->Stats.bDead || K->Stats.Seq == 0)
	{
		S.Seq = ++B.StatsSeq;
		K->Stats = S;
		K->ForceNetUpdate();
	}
}

// ---- shots, hits, deaths -------------------------------------------------------------------------------------------------

void UOutlawsCoop::BotFired(AOutlawsEnemy* E, const FVector& Muzzle)
{
	FBot* B = E ? FindBot(E->CoopSelf) : nullptr;
	if (!B || !IsHost()) return;
	const FBotGun& G = BotGun(B->Weapon);
	if (B->Weapon != 0) B->Ammo[G.AmmoType] = FMath::Max(0, B->Ammo[G.AmmoType] - G.Rounds);
	else B->Ammo[0] = FMath::Max(0, B->Ammo[0] - 1);
	++B->ShotsFired;
	{
		// (BOTSRANGE's measure: the distance to whoever it shot at, and whether the gun was the band's)
		FVector TF;
		if (E->CoopTarget >= 0 && E->CoopTarget != B->Who && PlayerFeetOf(E->CoopTarget, TF))
		{
			const float M = float(FVector::Dist(E->GetActorLocation() - FVector(0.0, 0.0, E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()), TF) / 100.0);
			const int32 Band = RangeBand(M);
			bool bCould = false;
			for (int32 g = 0; g < 5; ++g) bCould = bCould || (GunFits(g, Band) && (g == B->Weapon || GunUsable(*B, g)));
			const bool bRight = GunFits(B->Weapon, Band);
			++GRangeShots[Band];
			GRangeRight[Band] += bRight;
			GRangeCould[Band] += bCould;
			GRangeRightCould[Band] += bCould && bRight;
			++GGunShots[B->Weapon];
			GGunShotM[B->Weapon] += M;
		}
	}
	// a shotgun is reloaded shell by shell: one shot a burst
	if (B->Weapon >= 2) E->BurstLeft = FMath::Min(E->BurstLeft, 1);
	if (bMatchSetUp) Dm.NoteShot(B->Who, FPlatformTime::Seconds());
	const FString Snd = E->Def.ShootSound.IsEmpty() ? FString(G.Sound) : E->Def.ShootSound;     // (BUG-REMOTEGUNSOUND: the character's own gun, as BotApplyWeapon set it; not the default table)
	ForClients([&](AOutlawsCoopLink& CL) { CL.ClientShotFx(B->Who, Muzzle, Snd); }, B->Who);
	if (AOutlawsRemotePlayer* R = PuppetOf(B->Who)) R->NoteShot();
	// (REQ-MUZZLEFLASH3D) the bot's 3D flash here, the way it faces (the clients draw theirs in OnShotFx)
	if (AOutlawsLevelActor* L = FindLevel())
	{
		if (AOutlawsLevelActor::IsGunSound(Snd) && L->MuzzleFlash3D(Muzzle, FRotator(0.0, E->GetHeading(), 0.0).Vector(), AOutlawsLevelActor::FlashKindOfSound(Snd))
			&& TestOnly == TEXT("remotefx"))
		{
			RfFlash3D.Add(B->Who, AOutlawsLevelActor::FlashKindName(AOutlawsLevelActor::FlashKindOfSound(Snd)));
		}
	}
	if (B->ShotsFired <= 3) UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s fires the %s at the %s"), *B->Name, G.Label, WhoWord(E->CoopTarget));
	// out of ammunition: the next gun once this shot is done (TickBots)
	if (B->Weapon != 0 && B->Ammo[G.AmmoType] < G.Rounds) B->NextChooseAt = 0.0;
}

void UOutlawsCoop::BotPellet(AOutlawsEnemy* E, float Damage, const FVector& At, const FVector& From)
{
	if (!E || !IsHost() || E->CoopSelf < 0) return;
	if (!BotFoe(E->CoopSelf, E->CoopTarget))
	{
		if (E->CoopTarget != E->CoopSelf) ++GBotTeamRefused;       // (never itself, nor a teammate)
		return;
	}
	OnMatchHit(E->CoopSelf, LevelEpoch(), E->CoopTarget, Damage, At, From);
}

void UOutlawsCoop::BotHurt(int32 Who, float Damage, const FVector& At, const FVector& From, int32 Shooter)
{
	FBot* B = FindBot(Who);
	if (!B || B->bDead) return;
	if (Shooter >= 0 && Shooter != Who && !BotFoe(Who, Shooter) && Damage > 0.f) ++GBotTeamHits;
	B->Health = FMath::Max(0.f, B->Health - Damage);
	AOutlawsEnemy* E = B->Brain.Get();
	const double Now = FPlatformTime::Seconds();
	FHurtRun& Hr = GHurtRun.FindOrAdd(Who);
	if (B->Health > 0.f)
	{
		// (BOTSHURT) under 40 %: a break-off, planned on its next drive (once it ended, again 5 s on with a fresh hit)
		if (Damage > 0.f && B->Health < BotHealthMax * HurtFrac && !Hr.bActive && !Hr.bPending && Now - Hr.LastEnd > 5.0) Hr.bPending = true;
		if (E)
		{
			// the flinch and the turn to whoever struck it (the bad guys' own reaction; the health is the match's)
			E->TakeHit(0.f, At, (At - From).GetSafeNormal(), From);
			if (Shooter >= 0 && Shooter != Who && !MatchDead[Shooter]) E->CoopTarget = Shooter;
		}
		BotPublish(*B, Now);
		return;
	}
	if (Hr.Since > 0.0 && Now - Hr.Since <= HurtHealWithin && !Hr.bHealed) ++GHurtDied;
	GHurtRun.Remove(Who);
	B->bDead = true;
	B->DiedAt = Now;
	B->RespawnAt = Now + 3.0 + FMath::FRandRange(0.0, 2.0);
	if (E) E->Destroy();
	B->Brain.Reset();
	BotPublish(*B, Now);
	UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s dies (struck by the %s%s)"), *B->Name, Shooter >= 0 ? WhoWord(Shooter) : TEXT("world"),
		IsBot(Shooter) ? *FString::Printf(TEXT(", %s"), *FindBot(Shooter)->Name) : TEXT(""));
}

// ---- pickups ------------------------------------------------------------------------------------------------------------

void UOutlawsCoop::BotLookForPickups(FBot& B, AOutlawsLevelActor* L, double Now)
{
	AOutlawsEnemy* E = B.Brain.Get();
	if (!E || !L) return;
	const float Half = E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const FVector Feet = E->GetActorLocation() - FVector(0.0, 0.0, Half);
	for (int32 i = 0; i < L->Pickups.Num(); ++i)
	{
		const AOutlawsLevelActor::FPickup& P = L->Pickups[i];
		const AActor* A = P.Actor.Get();
		if (!A || A->IsHidden() || P.bHeld || P.CoopId < 0) continue;
		// what it gives, and whether this bot has a use for it
		TArray<AOutlawsLevelActor::FPickup::FEntry> Gives = P.Entries;
		if (Gives.Num() == 0 && !P.Item.IsEmpty()) { AOutlawsLevelActor::FPickup::FEntry X; X.Item = P.Item; Gives.Add(X); }
		bool bUse = P.bInstant && P.HealthB > 0 && B.Health < BotHealthMax;
		for (const AOutlawsLevelActor::FPickup::FEntry& X : Gives)
		{
			const int32 G = GunOfItem(X.Item), Am = AmmoOfItem(X.Item);
			if (G >= 0 && !B.bHas[G]) bUse = true;
			bool bFits = Am == 0;
			for (int32 g = 1; g < 5 && Am > 0; ++g) bFits = bFits || (B.bHas[g] && BotGun(g).AmmoType == Am);
			if (Am >= 0 && B.Ammo[Am] < MaxAmmo && bFits) bUse = true;
			if ((IsSticks(X.Item) && B.Sticks < MaxSticks) || (IsKnives(X.Item) && B.Knives < MaxKnives)) bUse = true;
		}
		if (!bUse) continue;
		const FVector Where = A->GetActorLocation();
		const double D2 = FVector::Dist2D(Where, Feet);
		if (D2 > 100.0 || FMath::Abs(Where.Z - Feet.Z) > 150.0) continue;
		// walked over: claimed as a player's claim is (gone on every machine; its generator's clock starts)
		// (BUG-BOTPICKUP2: what it heals read now; the claim takes it out of the list, and P is then the next one's)
		const int32 Id = P.CoopId;
		const FString Ground = P.Ground;
		const int32 HealthB = P.bInstant ? P.HealthB : 0;
		OnPickupClaim(B.Who, Id, LevelEpoch(), 1);
		const int32* By = PickupGone.Find(Id);
		if (!By || *By != B.Who) continue;
		FString Got;
		for (const AOutlawsLevelActor::FPickup::FEntry& X : Gives)
		{
			const int32 G = GunOfItem(X.Item), Am = AmmoOfItem(X.Item);
			if (G >= 0) B.bHas[G] = true;
			if (Am >= 0) B.Ammo[Am] = FMath::Min(MaxAmmo, B.Ammo[Am] + FMath::Max(1, X.Count));
			if (IsSticks(X.Item)) B.Sticks = FMath::Min(MaxSticks, B.Sticks + FMath::Max(1, X.Count));
			if (IsKnives(X.Item)) B.Knives = FMath::Min(MaxKnives, B.Knives + FMath::Max(1, X.Count));
			Got += FString::Printf(TEXT("%s%s %d"), Got.IsEmpty() ? TEXT("") : TEXT(", "), *X.Item, X.Count);
		}
		if (HealthB > 0)
		{
			B.Health = FMath::Min(BotHealthMax, B.Health + HealthB / 10.f);           // (as the player's: HEALTH_B / 10)
			Got += FString::Printf(TEXT("%shealth +%.1f"), Got.IsEmpty() ? TEXT("") : TEXT(", "), HealthB / 10.f);
			++GHealthTaken;
			// (BOTSHURT) healed while it broke off: back to the hunt once over 40 %
			if (FHurtRun* Hr = GHurtRun.Find(B.Who); Hr && Hr->Since > 0.0)
			{
				if (Hr->bReach && !Hr->bHealed && Now - Hr->Since <= HurtHealWithin) { Hr->bHealed = true; GHurtHealed += Hr->bCounted; }
				if (Hr->bActive && B.Health >= BotHealthMax * HurtFrac)
				{
					Hr->bActive = false;
					Hr->LastEnd = Now;
					B.DestPickup = -1;
					B.NextPlanAt = 0.0;
					UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s is healed (%.1f) %.1f s after it broke off: back to the hunt"), *B.Name, B.Health, Now - Hr->Since);
				}
			}
		}
		++B.Pickups;
		++BotPickups;
		UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s picks up %s (%s)"), *B.Name, *Ground, *Got);
		BotChooseWeapon(B, TEXT("a pickup"));
		return;                                  // (the list changed)
	}
}

bool UOutlawsCoop::PlayerFeetOf(int32 Who, FVector& OutFeet) const
{
	if (Who < 0 || Who >= MaxPlayers || MatchDead[Who] || PlayerInLobby(Who)) return false;      // (REQ-MPLOBBY: not in the level yet)
	if (Who == 0)
	{
		const AOutlawsLevelActor* L = FindLevel();
		const UWorld* W = CurrentWorld();
		const APlayerController* PC = W ? W->GetFirstPlayerController() : nullptr;
		const APawn* P = PC ? PC->GetPawn() : nullptr;
		if (!P || !L || L->IsPlayerDead()) return false;
		float R = 34.f, H = 87.f;
		P->GetSimpleCollisionCylinder(R, H);
		OutFeet = P->GetActorLocation() - FVector(0.0, 0.0, H);
		return true;
	}
	const FOutlawsCoopRemoteView& V = RemoteViews[Who];
	if (!V.bValid || V.bDead || V.bAway) return false;
	OutFeet = V.Feet;
	return true;
}

bool UOutlawsCoop::BotFoe(int32 Bot, int32 Other) const
{
	if (Other == Bot || Other < 0) return false;
	if (TestOnly == TEXT("killfeed") || TestOnly == TEXT("remotefx") || TestOnly == TEXT("flicknet")) return false;     // (REQ-KILLFEED's test: the bots hold their fire; the test deals every death)
	if (!Modes.IsTeamClass()) return true;
	const int32 Mine = Modes.TeamOf(Bot);
	return Mine == 0 || Modes.TeamOf(Other) != Mine;
}

int32 UOutlawsCoop::BotHuntTarget(FBot& B, const FVector& Feet, FVector& OutFeet, AOutlawsLevelActor* L)
{
	// the nearest living opponent (a floor apart counts double) it has a way to through the sectors, the one it hunts kept
	// unless another is a third nearer.  (BUG-BOTTEAMPLAY: one on a roof or a ledge no body walks up to was hunted to no
	// end, round the wall below it; the nearest of the rest instead.  None it has a way to: the nearest, as before.)
	struct FFoe { int32 K; double D; FVector F; };
	TArray<FFoe> Foes;
	for (int32 k = 0; k < MaxPlayers; ++k)
	{
		FVector F;
		if (!BotFoe(B.Who, k) || !PlayerFeetOf(k, F)) continue;
		Foes.Add({ k, FVector::Dist2D(F, Feet) + FMath::Abs(F.Z - Feet.Z) * 2.0, F });
	}
	if (Foes.Num() == 0)
	{
		B.HuntWho = -1;
		return -1;
	}
	Foes.Sort([](const FFoe& A, const FFoe& C) { return A.D < C.D; });
	AOutlawsEnemy* E = B.Brain.Get();
	auto Way = [&](const FVector& F)
	{
		if (!E || !L) return true;
		const float Half = E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		TArray<FVector> R;
		double Len = 0.0;
		return L->PathRoute(Feet, F, E->Def.MaxStep, 2.0 * Half / UnitCmB, 1.5, R, Len, E->Def.MaxDrop);
	};
	int32 Best = 0;
	for (int32 i = 0; i < Foes.Num() && i < 3; ++i)
	{
		if (Way(Foes[i].F)) { Best = i; break; }
	}
	for (const FFoe& Kept : Foes)
	{
		if (Kept.K != B.HuntWho) continue;
		if (Kept.K != Foes[Best].K && Foes[Best].D > Kept.D * 0.67 && Way(Kept.F))
		{
			OutFeet = Kept.F;
			return B.HuntWho;
		}
	}
	B.HuntWho = Foes[Best].K;
	OutFeet = Foes[Best].F;
	return B.HuntWho;
}

bool UOutlawsCoop::BotRouteStep(FBot& B, AOutlawsLevelActor* L, const FVector& Feet, double Now, FVector& OutStep)
{
	AOutlawsEnemy* E = B.Brain.Get();
	UWorld* W = E ? E->GetWorld() : nullptr;
	if (!E || !W || !L) return false;
	// the way through the sectors: made afresh every 2 s, or once the place it goes moves 2 m (one it hunts walks on)
	const bool bMoved = FVector::Dist(B.RouteTo, B.Dest) > 200.0;
	if (B.Route.Num() == 0 || bMoved || Now - B.RouteMadeAt > 2.0)
	{
		if (B.Route.Num() == 0 && !bMoved && Now - B.RouteMadeAt < 1.0) return false;     // (no way a moment ago: not asked again yet)
		const float Half = E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		TArray<FVector> R;
		double Len = 0.0;
		B.RouteMadeAt = Now;
		B.RouteTo = B.Dest;
		if (!L->PathRoute(Feet, B.Dest, E->Def.MaxStep, 2.0 * Half / UnitCmB, 1.5, R, Len, E->Def.MaxDrop))
		{
			if (B.RouteFails++ % 20 == 0)
			{
				UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s: no way through the sectors from %s (sector %d) to %s at %s (sector %d)"), *B.Name, *Feet.ToCompactString(), L->CullSectorOf(Feet + FVector(0.0, 0.0, 10.0)),
					*B.DestWhat, *B.Dest.ToCompactString(), L->CullSectorOf(B.Dest + FVector(0.0, 0.0, 10.0)));
				UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT why: %s"), *L->PathRouteWhy(Feet, B.Dest, E->Def.MaxStep, 2.0 * Half / UnitCmB, E->Def.MaxDrop));
			}
			B.Route.Reset();
			return false;
		}
		++B.Routes;
		B.Route = MoveTemp(R);
		B.RouteIdx = 0;
		B.PullAt = 0.0;
	}
	// a point reached: on to the next
	while (B.RouteIdx < B.Route.Num() - 1 && FVector::Dist2D(Feet, B.Route[B.RouteIdx]) < 100.0 && FMath::Abs(Feet.Z - B.Route[B.RouteIdx].Z) < 150.0) ++B.RouteIdx;
	// the corners cut: the farthest of the next three points it walks straight to (twice a second)
	if (Now >= B.PullAt)
	{
		B.PullAt = Now + 0.5;
		for (int32 k = FMath::Min(B.RouteIdx + 3, B.Route.Num() - 1); k > B.RouteIdx; --k)
		{
			if (FVector::Dist(Feet, B.Route[k]) > 2500.0) continue;
			if (WalkClear(W, Feet, B.Route[k], E)) { B.RouteIdx = k; break; }
		}
	}
	double Left = FVector::Dist(Feet, B.Route[B.RouteIdx]);
	for (int32 k = B.RouteIdx; k < B.Route.Num() - 1; ++k) Left += FVector::Dist(B.Route[k], B.Route[k + 1]);
	B.RouteLen = Left;
	OutStep = B.Route[B.RouteIdx];
	return true;
}

void UOutlawsCoop::BotDrive(FBot& B, AOutlawsLevelActor* L, double Now)
{
	AOutlawsEnemy* E = B.Brain.Get();
	if (!E || !L) return;
	const float Half = E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const FVector Feet = E->GetActorLocation() - FVector(0.0, 0.0, Half);
	const double Speed = E->GetVelocity().Size2D();
	if (Speed > 60.0) B.StillSince = Now;
	else if (B.StillSince == 0.0) B.StillSince = Now;
	// (the test's measures: the ground it covers on its own drive; each time it comes to see someone)
	if (!B.LastFeet.IsZero())
	{
		const double Moved = FVector::Dist2D(Feet, B.LastFeet);
		if (B.bDriving && Moved < 300.0) B.DriveCm += Moved;
		const bool bHunting = B.bDriving && B.DestPickup < 0 && B.HuntWho >= 0 && B.DestWhat.StartsWith(TEXT("the "));
		if (bHunting && Moved < 300.0) B.IvDriveCm += Moved;
		B.bIvHunt = B.bIvHunt && bHunting;
	}
	B.LastFeet = Feet;
	if (E->bSeen && !B.bSawLast) ++B.Contacts;
	B.bSawLast = E->bSeen;
	// BOTSHURT: fallen under 40 %: health it has a way to within 25 m, else a place the one it fights cannot see; the hunt
	// again when healed (BotLookForPickups) or after 10 s.  In a fight too: it takes the bad guys' combat's moves over (their
	// shots on the move go on).  -OLBotOldHurt: as before; the tallies still kept (what an old bot did with health in reach).
	auto IsHealthPickup = [](const AOutlawsLevelActor::FPickup& P) { return P.bInstant && P.HealthB > 0; };
	auto HealthPickupThere = [&](int32 Id)
	{
		for (const AOutlawsLevelActor::FPickup& P : L->Pickups)
		{
			if (P.CoopId != Id) continue;
			const AActor* A = P.Actor.Get();
			return A && !A->IsHidden() && !P.bHeld && IsHealthPickup(P);
		}
		return false;
	};
	// (the nearest health by the way within 25 m, or the floor beside one it cannot walk onto)
	// (BOTSTUCK: and its body fits the way there; none given up from this sector by any bot in the last minute)
	const bool bCheckWay = !OldHurt() && !OldStuck();
	const int32 HereSec = L->CullSectorOf(Feet + FVector(0.0, 0.0, 10.0));
	auto NoWayFromHere = [&](int32 Id) { const double* Until = GHurtNoWay.Find(NoWayKey(Id, HereSec)); return Until && Now < *Until; };
	auto HealthInReach = [&](int32& OutId, FVector& OutDest, FString& OutWhat)
	{
		const double Tall = 2.0 * Half / UnitCmB;
		double Best = HurtReachCm;
		OutId = -1;
		for (const AOutlawsLevelActor::FPickup& P : L->Pickups)
		{
			const AActor* A = P.Actor.Get();
			if (!A || A->IsHidden() || P.bHeld || P.CoopId < 0 || !IsHealthPickup(P)) continue;
			if (const double* Until = B.AvoidPickup.Find(P.CoopId); Until && Now < *Until) continue;
			if (bCheckWay && NoWayFromHere(P.CoopId)) continue;
			FVector To = A->GetActorLocation();
			if (FVector::Dist(To, Feet) > Best || FMath::Abs(To.Z - Feet.Z) > 250.0) continue;
			TArray<FVector> R;
			double Len = 0.0;
			FString What = P.Ground;
			if (!L->PathRoute(Feet, To, E->Def.MaxStep, Tall, 1.5, R, Len, E->Def.MaxDrop))
			{
				FVector Beside;
				if (!L->PathReachPoint(To, 65.0, 140.0, Tall, Beside) || !L->PathRoute(Feet, Beside, E->Def.MaxStep, Tall, 1.5, R, Len, E->Def.MaxDrop)) continue;
				To = Beside;
				What += TEXT(" (from beside it)");
			}
			if (Len > Best) continue;
			if (bCheckWay && !RouteBodyClear(E->GetWorld(), Feet, R, E))
			{
				++GHurtBlocked;
				UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s passes %s by: the way there through the sectors (%.0f m) is not one its body fits (sector %d; %s)"), *B.Name, *What, Len / 100.0, HereSec, *GBodyWhy);
				continue;
			}
			Best = Len;
			OutId = P.CoopId;
			OutDest = To;
			OutWhat = What;
		}
		return OutId >= 0;
	};
	FHurtRun& Hr = GHurtRun.FindOrAdd(B.Who);
	auto TakeCover = [&]()
	{
		// (away from the one it fights: 1.5 to 7.5 m off, out of their sight; any side; failing both, straight away)
		FVector Eye, TF, P;
		float R = 0.f, H = 0.f;
		const bool bFoe = E->PlayerView(Eye, TF, R, H);
		const float Away = bFoe ? float((Feet - TF).Rotation().Yaw) : E->GetHeading() + 180.f;
		if (bCheckWay)
		{
			// (BOTSTUCK) a place counts only with a way there through the sectors no more than twice the straight line and 3
			// m round, its body fitting the way, not within 2.5 m of one given up this break-off; a few tries each
			auto Fits = [&](const FVector& At)
			{
				const FVector To = At - FVector(0.0, 0.0, Half);
				for (const FVector& Bad : Hr.BadSpots) if (FVector::Dist2D(Bad, To) < 250.0) return false;
				TArray<FVector> Rt;
				double Len = 0.0;
				return L->PathRoute(Feet, To, E->Def.MaxStep, 2.0 * Half / UnitCmB, 1.5, Rt, Len, E->Def.MaxDrop) && Len <= 2.0 * FVector::Dist(Feet, To) + 300.0
					&& RouteBodyClear(E->GetWorld(), Feet, Rt, E);
			};
			auto Try = [&](float MinU, float MaxU, float Yaw, float Spread, int32 Tries, bool bNeedCover)
			{
				for (int32 k = 0; k < 4; ++k) if (E->PickPoint(MinU, MaxU, Yaw, Spread, Tries, bNeedCover, P) && Fits(P)) return true;
				return false;
			};
			Hr.bCover = bFoe && (Try(5.f, 25.f, Away, 80.f, 4, true) || Try(5.f, 25.f, 0.f, 180.f, 6, true));
			if (!Hr.bCover && !Try(8.f, 25.f, Away, 45.f, 2, false) && !Try(5.f, 25.f, 0.f, 180.f, 4, false)) P = Feet + FRotator(0.0, Away, 0.0).Vector() * 400.0 + FVector(0.0, 0.0, Half);
		}
		else
		{
			Hr.bCover = bFoe && (E->PickPoint(5.f, 25.f, Away, 80.f, 16, true, P) || E->PickPoint(5.f, 25.f, 0.f, 180.f, 24, true, P));
			if (!Hr.bCover && !E->PickPoint(8.f, 25.f, Away, 45.f, 8, false, P)) P = Feet + FRotator(0.0, Away, 0.0).Vector() * 400.0 + FVector(0.0, 0.0, Half);
		}
		Hr.Dest = P - FVector(0.0, 0.0, Half);
		Hr.What = Hr.bCover ? TEXT("cover") : TEXT("away from the fight");
		Hr.Pick = -1;
		Hr.SeenAtCover = 0.0;
	};
	if (Hr.bPending)
	{
		Hr = FHurtRun();
		Hr.Since = Now;
		Hr.NextShotAt = Now + 0.5;
		Hr.bReach = HealthInReach(Hr.Pick, Hr.Dest, Hr.What);
		++GHurtFalls;
		GHurtReachAll += Hr.bReach;
		if (!OldHurt())
		{
			Hr.bActive = true;
			// (the stuck clock afresh: it may have stood in the fight for seconds with the same place to go)
			B.ProgressAt = Now;
			B.ProgressDist = 1e12;
			B.ProgressFeet = Feet;
			B.StuckRun = 0;
			if (!Hr.bReach)
			{
				TakeCover();
				GHurtCover += Hr.bCover;
			}
			B.NextPlanAt = 0.0;
			B.NextDriveAt = 0.0;
		}
		UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s is hurt (%.1f), %s the %s: %s%s"), *B.Name, B.Health, E->bSeen ? TEXT("sees") : TEXT("after"), WhoWord(E->CoopTarget),
			Hr.bReach ? *FString::Printf(TEXT("health in reach, %s"), *Hr.What) : TEXT("no health in reach"), OldHurt() ? TEXT(" (-OLBotOldHurt: on as before)") : !Hr.bReach ? *FString::Printf(TEXT(", off to %s"), *Hr.What) : TEXT(""));
	}
	if (Hr.Since > 0.0 && !Hr.bCounted && Now - Hr.Since >= 1.0)
	{
		Hr.bCounted = true;
		GHurtReach += Hr.bReach;
		GHurtHealed += Hr.bReach && Hr.bHealed;
	}
	if (Hr.bCounted && Hr.bReach && !Hr.bHeaded && Now - Hr.Since <= HurtFor && B.DestPickup >= 0 && HealthPickupThere(B.DestPickup)) { Hr.bHeaded = true; ++GHurtHeaded; }
	if (Hr.bActive && Now - Hr.Since >= HurtFor)
	{
		Hr.bActive = false;
		Hr.LastEnd = Now;
		if (Hr.Pick >= 0 && B.DestPickup == Hr.Pick) B.DestPickup = -1;
		B.NextPlanAt = 0.0;
		UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s: 10 s broken off (%.1f health): back to the hunt"), *B.Name, B.Health);
	}
	// (on its way it shoots only on the move, never stopping to: the bad guys' shoot timer held while it goes, its own
	// timer at their pace instead)
	if (Hr.bActive && E->State == EOutlawsEnemyState::Chase && (E->Sub == AOutlawsEnemy::SubAdvance || E->Sub == AOutlawsEnemy::SubRetreat || E->Sub == AOutlawsEnemy::SubCover))
	{
		E->ShootWait = FMath::Max(E->ShootWait, 0.2f);
		if (E->bSeen && Now >= Hr.NextShotAt && E->Def.WalkShootChor >= 0 && E->CoopTarget >= 0 && !MatchDead[E->CoopTarget] && L->TakeAttackToken(E))
		{
			Hr.NextShotAt = Now + FMath::Max(0.3, double(E->Def.ShootTimer) + FMath::FRandRange(-1.0, 1.0));
			E->StartShootMove();
		}
	}
	const bool bHurtMine = Hr.bActive && E->State == EOutlawsEnemyState::Chase && (E->Sub == AOutlawsEnemy::SubFace || E->Sub == AOutlawsEnemy::SubDecide
		|| E->Sub == AOutlawsEnemy::SubAdvance || E->Sub == AOutlawsEnemy::SubRetreat || E->Sub == AOutlawsEnemy::SubCover);
	// (the debug line: what the brain is doing)
	if (Now >= B.NextDebugAt)
	{
		B.NextDebugAt = Now + 10.0;
		UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s: state %d sub %d, %s the %s, speed %.0f, at %s, %d drives, to %s, route %d/%d %.0f m left, %d stuck"), *B.Name, int32(E->State), E->Sub, E->bSeen ? TEXT("sees") : TEXT("after"),
			WhoWord(E->CoopTarget), Speed, *Feet.ToCompactString(), B.Drives, *B.DestWhat, B.RouteIdx, B.Route.Num(), B.RouteLen / 100.0, B.Stucks);
	}
	// on the move already (its own drive): steered again every 0.25 s; it gives way to the bad guys' combat once it sees someone
	const bool bMine = B.bDriving && E->State == EOutlawsEnemyState::Chase && E->Sub == AOutlawsEnemy::SubAdvance && !E->bSeen;
	if (!bMine) B.bDriving = false;
	const bool bIdle = E->State == EOutlawsEnemyState::Watch || (E->State == EOutlawsEnemyState::Chase && !E->bSeen
		&& (E->Sub == AOutlawsEnemy::SubFace || E->Sub == AOutlawsEnemy::SubDecide || ((E->Sub == AOutlawsEnemy::SubAdvance || E->Sub == AOutlawsEnemy::SubCover) && Now - B.StillSince > 1.2)));
	if ((!bIdle && !bMine && !bHurtMine) || Now < B.NextDriveAt) return;
	B.NextDriveAt = Now + 0.25;
	if (E->State == EOutlawsEnemyState::Watch) E->Alert(false);
	if (E->State != EOutlawsEnemyState::Chase) return;
	if (Hr.bActive)
	{
		// (BOTSHURT) the health while it lies there (another took it: the next in reach, else cover); at cover, it stays
		// (facing the fight, its shots as they come) unless seen there a second, then cover afresh
		const double* Shun = Hr.Pick >= 0 ? B.AvoidPickup.Find(Hr.Pick) : nullptr;
		if (Hr.Pick >= 0 && (!HealthPickupThere(Hr.Pick) || (Shun && Now < *Shun)) && !HealthInReach(Hr.Pick, Hr.Dest, Hr.What)) TakeCover();
		if (Hr.Pick < 0 && FVector::Dist2D(Hr.Dest, Feet) < 120.0)
		{
			if (!E->bSeen) Hr.SeenAtCover = 0.0;
			else if (Hr.SeenAtCover == 0.0) Hr.SeenAtCover = Now;
			if (Hr.SeenAtCover > 0.0 && Now - Hr.SeenAtCover > 1.0) TakeCover();
			else
			{
				if (E->Sub != AOutlawsEnemy::SubFace) E->SetSub(AOutlawsEnemy::SubFace);
				E->SubTime = 0.f;              // (no Decide: it holds the place)
				B.bDriving = false;
				return;
			}
		}
		// (BOTSTUCK) a stall on its way: 2 s no nearer by the way and hardly moved (the clock held while the bad guys' combat
		// had it): that place given up (a health pickup by every bot from this sector for a minute), the next health in reach
		// or other cover; a third give-up in one break-off, back to the hunt
		if (bCheckWay)
		{
			// (the time the bad guys' combat had it, standing to shoot or throw, is no stall: the stuck clock held over it as
			// this one is; most stucks on the way to cover came the moment it was back from a fight's shots)
			if (Hr.LastTick > 0.0 && Now - Hr.LastTick > 0.6) B.ProgressAt = FMath::Min(Now, B.ProgressAt + (Now - Hr.LastTick - 0.25));
			if (Hr.ProgAt == 0.0 || FVector::Dist(Hr.ClockDest, Hr.Dest) > 50.0)
			{
				Hr.ProgAt = Now;
				Hr.BestLeft = 1e12;
				Hr.ProgFeet = Feet;
				Hr.ClockDest = Hr.Dest;
			}
			else
			{
				if (Now - Hr.LastTick > 0.6) Hr.ProgAt += Now - Hr.LastTick - 0.25;
				const double LeftNow = B.DestWhat == Hr.What && B.Route.Num() > 0 ? B.RouteLen : FVector::Dist(Feet, Hr.Dest);
				if (LeftNow < Hr.BestLeft - 100.0 || FVector::Dist2D(Feet, Hr.ProgFeet) > 250.0)
				{
					Hr.BestLeft = FMath::Min(Hr.BestLeft, LeftNow);
					Hr.ProgAt = Now;
					Hr.ProgFeet = Feet;
				}
				else if (Now - Hr.ProgAt > HurtStallFor && Hr.Pick < 0 && LeftNow < 250.0)
				{
					// (stalled within 2.5 m of its cover, a wall or another body in the last step: held where it stands)
					Hr.Dest = Feet;
					Hr.ClockDest = Feet;
					Hr.ProgAt = Now;
					UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s holds %s %.1f m short of the place"), *B.Name, *Hr.What, LeftNow / 100.0);
				}
				else if (Now - Hr.ProgAt > HurtStallFor)
				{
					++GHurtGiveUps;
					++Hr.GiveUps;
					UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s gives up on %s at %s (sector %d): %.0f s no nearer, %.0f m left"), *B.Name, *Hr.What, *Feet.ToCompactString(), HereSec, HurtStallFor, LeftNow / 100.0);
					if (Hr.Pick >= 0)
					{
						B.AvoidPickup.Add(Hr.Pick, Now + 30.0);
						GHurtNoWay.Add(NoWayKey(Hr.Pick, HereSec), Now + 60.0);
					}
					else Hr.BadSpots.Add(Hr.Dest);
					B.Route.Reset();
					B.RouteMadeAt = 0.0;
					B.ProgressAt = Now;
					B.ProgressDist = 1e12;
					B.ProgressFeet = Feet;
					B.StuckRun = 0;
					if (Hr.GiveUps >= 3)
					{
						++GHurtEnded;
						Hr.bActive = false;
						Hr.LastEnd = Now;
						if (B.DestPickup >= 0 && B.DestPickup == Hr.Pick) B.DestPickup = -1;
						B.NextPlanAt = 0.0;
						UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s: three places given up (%.1f health): back to the hunt"), *B.Name, B.Health);
					}
					else if (Hr.Pick < 0 || !HealthInReach(Hr.Pick, Hr.Dest, Hr.What)) TakeCover();
					Hr.ProgAt = 0.0;
				}
			}
			Hr.LastTick = Now;
		}
		if (Hr.bActive)
		{
			B.DestPickup = Hr.Pick;
			if (Hr.What != B.DestWhat)
			{
				B.ProgressAt = Now;
				B.ProgressDist = 1e12;
				B.ProgressFeet = Feet;
				B.StuckRun = 0;
				UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s breaks off for %s (%.0f m)"), *B.Name, *Hr.What, FVector::Dist(Hr.Dest, Feet) / 100.0);
			}
			B.Dest = Hr.Dest;
			B.DestWhat = Hr.What;
			B.NextPlanAt = Now + 1.5;
		}
	}
	// where to (looked at again every 1.5 s, and on arriving): a pickup it has a use for within 20 m; else the nearest
	// opponent (BotHuntTarget); else a far spawn point.  Stuck three times on one way: a far place for a few seconds.
	if (!Hr.bActive && (Now >= B.NextPlanAt || FVector::Dist2D(B.Dest, Feet) < 120.0))
	{
		B.NextPlanAt = Now + 1.5;
		FVector Dest = FVector::ZeroVector;
		FString What;
		const FBotPref& Pf = PrefOf(B.Short);
		// (a pickup it set out for is kept for 6 s while it lies there: BUG-BOTTEAMPLAY, drygulch's balcony, where the plan
		// flipped between a pickup below and the one it hunted every 1.5 s and neither came nearer)
		bool bKeepPickup = false;
		if (B.DestPickup >= 0 && Now < B.PickupUntil && Now >= B.CommitUntil)
		{
			for (const AOutlawsLevelActor::FPickup& P : L->Pickups)
			{
				const AActor* A = P.Actor.Get();
				if (P.CoopId == B.DestPickup) { bKeepPickup = A && !A->IsHidden() && !P.bHeld; break; }
			}
		}
		if (bKeepPickup)
		{
			Dest = B.Dest;
			What = B.DestWhat;
		}
		else if (Now < B.CommitUntil)
		{
			Dest = B.Goal;
			What = TEXT("a far place");
		}
		else
		{
			double Best = 2000.0;
			int32 BestPick = -1;
			bool bBestBig = false, bBestHealth = false;
			for (const AOutlawsLevelActor::FPickup& P : L->Pickups)
			{
				const AActor* A = P.Actor.Get();
				if (!A || A->IsHidden() || P.bHeld || P.CoopId < 0) continue;
				if (const double* Until = B.AvoidPickup.Find(P.CoopId); Until && Now < *Until) continue;
				if (bCheckWay && IsHealthPickup(P) && NoWayFromHere(P.CoopId)) continue;          // (BOTSTUCK: given up from here)
				bool bUse = P.bInstant && P.HealthB > 0 && B.Health < BotHealthMax * 0.7f, bBig = bUse;
				// (a gun it likes and lacks; BUG-BOTPICKUP: also, within 8 m (on its way, not a trip of its own), shells for a gun
				// it holds when half spent and sticks or knives when it carries none, as a player stocks up)
				const double D = FVector::Dist(A->GetActorLocation(), Feet);
				TArray<AOutlawsLevelActor::FPickup::FEntry> Gives = P.Entries;
				if (Gives.Num() == 0 && !P.Item.IsEmpty()) { AOutlawsLevelActor::FPickup::FEntry X; X.Item = P.Item; Gives.Add(X); }
				for (const AOutlawsLevelActor::FPickup::FEntry& X : Gives)
				{
					const int32 G = GunOfItem(X.Item), Am = AmmoOfItem(X.Item);
					if (G >= 0 && !B.bHas[G] && Pf.W[G] > 0) bUse = bBig = true;
					if (D > 800.0) continue;
					for (int32 g = 1; g < 5 && Am >= 0; ++g) if (B.bHas[g] && BotGun(g).AmmoType == Am && B.Ammo[Am] < MaxAmmo / 2) bUse = true;
					if ((IsSticks(X.Item) && B.Sticks <= 0) || (IsKnives(X.Item) && B.Knives <= 0)) bUse = true;
				}
				if (bUse && D < Best && FMath::Abs(A->GetActorLocation().Z - Feet.Z) < 250.0) { Best = D; Dest = A->GetActorLocation(); What = P.Ground; BestPick = P.CoopId; bBestBig = bBig; bBestHealth = IsHealthPickup(P); }
			}
			if (BestPick >= 0)
			{
				// (only one it has a way to through the sectors, 50 m at most; one it has none to is left alone for half a minute)
				// (Half: its body, above)
				TArray<FVector> R;
				double Len = 0.0;
				bool bWay = L->PathRoute(Feet, Dest, E->Def.MaxStep, 2.0 * Half / UnitCmB, 1.5, R, Len, E->Def.MaxDrop) && Len <= 5000.0;
				// (BUG-BOTPICKUP: one it cannot walk to, under a bar or on a ledge, reached from just beside it: claimed within
				// 1 m across and 1.5 m up, BotLookForPickups)
				FVector Beside;
				const bool bBeside = !bWay && L->PathReachPoint(Dest, 65.0, 140.0, 2.0 * Half / UnitCmB, Beside);
				if (bBeside && L->PathRoute(Feet, Beside, E->Def.MaxStep, 2.0 * Half / UnitCmB, 1.5, R, Len, E->Def.MaxDrop) && Len <= 5000.0)
				{
					bWay = true;
					Dest = Beside;
					What += TEXT(" (from beside it)");
				}
				// (BUG-BOTTEAMPLAY: only on its way to whoever it hunts: no more than 8 m further round for shells or kit, 25 m for a
				// gun it lacks or health it needs; trips off its way cost the hunt more than the pickup gave)
				if (bWay)
				{
					FVector TF;
					TArray<FVector> R2;
					double Lt = 0.0, Lt2 = 0.0;
					if (BotHuntTarget(B, Feet, TF, L) >= 0 && L->PathRoute(Feet, TF, E->Def.MaxStep, 2.0 * Half / UnitCmB, 1.5, R2, Lt, E->Def.MaxDrop)
						&& L->PathRoute(Dest, TF, E->Def.MaxStep, 2.0 * Half / UnitCmB, 1.5, R2, Lt2, E->Def.MaxDrop) && Len + Lt2 - Lt > (bBestBig ? 2500.0 : 800.0)
						&& !(bBestHealth && !OldHurt() && B.Health < BotHealthMax * HurtFrac && Len <= HurtReachCm))      // (BOTSHURT: under 40 %, health within 25 m whatever the way)
					{
						What.Reset();
						BestPick = -1;
					}
				}
				else
				{
					B.AvoidPickup.Add(BestPick, Now + 30.0);
					UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s leaves %s alone: %s"), *B.Name, *What, Len > 5000.0 ? *FString::Printf(TEXT("%.0f m round by the way"), Len / 100.0) : bBeside ? TEXT("no way to it or beside it through the sectors") : TEXT("no way to it through the sectors, no floor beside it"));
					UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT why: %s"), *L->PathRouteWhy(Feet, Dest, E->Def.MaxStep, 2.0 * Half / UnitCmB, E->Def.MaxDrop));
					What.Reset();
					BestPick = -1;
				}
			}
			if (BestPick >= 0 && BestPick != B.DestPickup) { B.PickupUntil = Now + 6.0; ++BotPickupTrips; }
			B.DestPickup = BestPick;
			if (What.IsEmpty())
			{
				FVector TF;
				const int32 Who = BotHuntTarget(B, Feet, TF, L);
				if (Who >= 0)
				{
					Dest = TF;
					What = FString::Printf(TEXT("the %s"), WhoWord(Who));
					if (!E->bSeen) E->CoopTarget = Who;
				}
			}
			if (What.IsEmpty())
			{
				const TArray<FOutlawsDmSpawn>& Sp = Dm.GetSpawns();
				if ((Now >= B.GoalUntil || FVector::Dist2D(B.Goal, Feet) < 300.0) && Sp.Num() > 0)
				{
					B.Goal = Sp[FMath::RandHelper(Sp.Num())].Location - FVector(0.0, 0.0, 88.0);
					B.GoalUntil = Now + 25.0;
				}
				Dest = B.Goal;
				What = TEXT("a far place");
			}
		}
		if (What != B.DestWhat)
		{
			B.ProgressAt = Now;
			B.ProgressDist = 1e12;
			B.ProgressFeet = Feet;
			B.StuckRun = 0;
			if (B.Drives % 4 == 0) UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s heads for %s (%.0f m)"), *B.Name, *What, FVector::Dist(Dest, Feet) / 100.0);
		}
		B.Dest = Dest;
		B.DestWhat = What;
	}
	// the way there: through the sectors (BotRouteStep); else over the way points; else straight
	UWorld* W = E->GetWorld();
	FVector Step;
	const bool bRouted = BotRouteStep(B, L, Feet, Now, Step);
	if (!bRouted)
	{
		FVector* Wp = GWaypoint.Find(B.Who);
		if (!Wp || FVector::Dist2D(*Wp, Feet) < 150.0 || Now >= B.NextPlanAt - 0.3)
		{
			if (!NextWaypoint(W, Feet, B.Dest, E, Step)) Step = B.Dest;
			Wp = &GWaypoint.Add(B.Who, Step);
		}
		Step = *Wp;
	}
	// no way through the sectors for 8 s together (fallen into a pit it cannot climb out of, shut in a room): freed below
	bool bFree = false;
	{
		double& NoWay = GNoWaySince.FindOrAdd(B.Who);
		if (bRouted) NoWay = 0.0;
		else if (NoWay == 0.0) NoWay = Now;
		else if (Now - NoWay > 8.0)
		{
			NoWay = 0.0;
			// (BUG-BOTTEAMPLAY: no way to where it goes is not shut in: one it hunts on a roof put it at a spawn point every
			// 8 s.  A way to a spawn point: off there for a few seconds instead; none: shut in, freed below)
			const TArray<FOutlawsDmSpawn>& Sp = Dm.GetSpawns();
			bFree = true;
			for (int32 Try = 0; Try < 4 && Sp.Num() > 0 && bFree; ++Try)
			{
				const FVector To = Sp[FMath::RandHelper(Sp.Num())].Location - FVector(0.0, 0.0, 88.0);
				TArray<FVector> R;
				double Len = 0.0;
				if (FVector::Dist2D(To, Feet) > 600.0 && L->PathRoute(Feet, To, E->Def.MaxStep, 2.0 * Half / UnitCmB, 1.5, R, Len, E->Def.MaxDrop))
				{
					bFree = false;
					B.Goal = To;
					B.GoalUntil = Now + 25.0;
					B.CommitUntil = Now + 6.0;
					B.NextPlanAt = 0.0;
					UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s has no way to %s: a spawn point %.0f m off by the way for a while"), *B.Name, *B.DestWhat, Len / 100.0);
				}
			}
		}
	}
	// stuck: no nearer by the way and hardly moved for 4 s: a step aside, the way made afresh; three in a row, a far place
	const double Left = bRouted ? B.RouteLen : FVector::Dist(Feet, B.Dest);
	if (Left < B.ProgressDist - 150.0 || FVector::Dist2D(Feet, B.ProgressFeet) > 400.0)
	{
		B.ProgressDist = FMath::Min(B.ProgressDist, Left);
		B.ProgressAt = Now;
		B.ProgressFeet = Feet;
		B.StuckRun = 0;
	}
	else if (Now - B.ProgressAt > 4.0)
	{
		++B.Stucks;
		++B.StuckRun;
		B.ProgressAt = Now;
		B.ProgressDist = Left;
		B.ProgressFeet = Feet;
		B.Side = -B.Side;
		const FVector Toward = (Step - Feet).GetSafeNormal2D();
		B.UnstickDir = Toward.IsNearlyZero() ? FRotator(0.0, FMath::FRandRange(-180.f, 180.f), 0.0).Vector() : Toward.RotateAngleAxis(100.f * B.Side, FVector::UpVector);
		B.UnstickUntil = Now + 0.8;
		FString Pts;
		for (int32 k = B.RouteIdx; k < B.Route.Num() && k < B.RouteIdx + 3; ++k)
		{
			const int32 Was[4] = { GWalkFail[0], GWalkFail[1], GWalkFail[2], GWalkFail[3] };
			const bool bClear = WalkClear(W, Feet, B.Route[k], E);
			const TCHAR* Why = bClear ? TEXT("clear") : GWalkFail[0] != Was[0] ? TEXT("a step") : GWalkFail[1] != Was[1] ? TEXT("a drop") : GWalkFail[2] != Was[2] ? TEXT("in the way") : GWalkFail[3] != Was[3] ? TEXT("no ground") : TEXT("off at the end");
			Pts += FString::Printf(TEXT(" [%d %s %.0f m %s]"), k, *B.Route[k].ToCompactString(), FVector::Dist(Feet, B.Route[k]) / 100.0, Why);
		}
		if (B.Route.IsValidIndex(B.RouteIdx))
		{
			// (what its body meets on the way to the next point)
			FCollisionQueryParams HitQ(SCENE_QUERY_STAT(OutlawsBotWhy), false, E);
			FCollisionResponseParams HitResp;
			HitResp.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
			HitResp.CollisionResponse.SetResponse(ECC_GameTraceChannel2, ECR_Ignore);
			FHitResult Hit;
			const FVector Up(0.0, 0.0, 95.0);
			if (W->SweepSingleByChannel(Hit, Feet + Up, FVector(B.Route[B.RouteIdx].X, B.Route[B.RouteIdx].Y, Feet.Z) + Up, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(25.f, 45.f), HitQ, HitResp))
			{
				Pts += FString::Printf(TEXT(" | met %s / %s at %s (sector %d)"), Hit.GetActor() ? *Hit.GetActor()->GetName() : TEXT("?"), Hit.GetComponent() ? *Hit.GetComponent()->GetName() : TEXT("?"),
					*FVector(Hit.ImpactPoint).ToCompactString(), L->CullSectorOf(FVector(Hit.ImpactPoint)));
			}
			Pts += FString::Printf(TEXT(" | here sector %d"), L->CullSectorOf(Feet + FVector(0.0, 0.0, 10.0)));
		}
		UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s way points ahead:%s"), *B.Name, Pts.IsEmpty() ? TEXT(" none") : *Pts);
		// (diagnosis, once a sector: its walls as the way-finding reads them)
		static TSet<int32> Dumped;
		const int32 Sec = L->CullSectorOf(Feet + FVector(0.0, 0.0, 10.0));
		if (L->Inf.IsValid() && L->Inf->Live.Sectors.IsValidIndex(Sec) && !Dumped.Contains(Sec) && Dumped.Num() < 6)
		{
			Dumped.Add(Sec);
			const FOutlawsSector& S = L->Inf->Live.Sectors[Sec];
			const double U = FOutlawsGeometry::UnitsToCm;
			FString Walls;
			for (int32 w = 0; w < S.Walls.Num(); ++w)
			{
				const FOutlawsWall& Wl = S.Walls[w];
				if (!S.Verts.IsValidIndex(Wl.V1) || !S.Verts.IsValidIndex(Wl.V2)) continue;
				const FVector2D A = S.Verts[Wl.V1], C = S.Verts[Wl.V2];
				Walls += FString::Printf(TEXT(" w%d (%.0f,%.0f)-(%.0f,%.0f) adj %d/%d flags %x;"), w, A.Y * U, A.X * U, C.Y * U, C.X * U, Wl.Adjoin, Wl.DAdjoin, Wl.Flags1);
			}
			UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT sector %d '%s': floor %.1f ceil %.1f flags %x, %d walls:%s"), Sec, *S.Name, S.Floor.Y * U, S.Ceil.Y * U, S.Flags, S.Walls.Num(), *Walls);
		}
		if (B.DestPickup >= 0 && B.StuckRun >= 2)
		{
			B.AvoidPickup.Add(B.DestPickup, Now + 30.0);
			B.DestPickup = -1;
			B.NextPlanAt = 0.0;
		}
		B.Route.Reset();
		B.RouteMadeAt = 0.0;
		if (B.StuckRun >= 3)
		{
			const TArray<FOutlawsDmSpawn>& Sp = Dm.GetSpawns();
			if (Sp.Num() > 0)
			{
				B.Goal = Sp[FMath::RandHelper(Sp.Num())].Location - FVector(0.0, 0.0, 88.0);
				B.GoalUntil = Now + 25.0;
				B.CommitUntil = Now + 6.0;
				B.NextPlanAt = 0.0;
			}
			B.StuckRun = 0;
		}
		UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s stuck on the way to %s at %s (%.0f m left by the %s): a step aside%s"), *B.Name, *B.DestWhat, *Feet.ToCompactString(), Left / 100.0,
			bRouted ? TEXT("sectors") : TEXT("way points"), Now < B.CommitUntil ? TEXT(", then a far place") : TEXT(""));
		// shut in: stuck five times in 30 s within 6 m of one place (a room whose only ways out no body passes: town_2p's
		// spawn points behind barred windows): put at another spawn point, as the rules' respawn would (no death)
		FTrapWatch& T = GTrapWatch.FindOrAdd(B.Who);
		if (T.Since == 0.0 || Now - T.Since > 30.0 || FVector::Dist2D(T.Feet, Feet) > 600.0) { T.Since = Now; T.Feet = Feet; T.Stucks = 0; }
		if (++T.Stucks >= 5) bFree = true;
	}
	if (bFree)
	{
		GTrapWatch.Remove(B.Who);
		{
			const TArray<FOutlawsDmSpawn>& Sp = Dm.GetSpawns();
			int32 Pick = -1;
			for (int32 Try = 0; Try < 8 && Sp.Num() > 0; ++Try)
			{
				const int32 i = FMath::RandHelper(Sp.Num());
				if (FVector::Dist2D(Sp[i].Location, Feet) > 1500.0) { Pick = i; break; }
			}
			if (Pick >= 0)
			{
				++GBotsFreed;
				UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s is shut in at %s: put at the spawn point at %s"), *B.Name, *Feet.ToCompactString(), *Sp[Pick].Location.ToCompactString());
				// (what it carries and its health go with it)
				const float Health = B.Health;
				const int32 Weapon = B.Weapon, Sticks = B.Sticks, Knives = B.Knives;
				bool Has[5];
				int32 Ammo[3];
				FMemory::Memcpy(Has, B.bHas, sizeof(Has));
				FMemory::Memcpy(Ammo, B.Ammo, sizeof(Ammo));
				const int32 Who = B.Who;
				BotSpawn(Who, Sp[Pick].Location, Sp[Pick].Yaw);
				if (FBot* Again = FindBot(Who))
				{
					Again->Health = Health;
					Again->Weapon = Weapon;
					Again->Sticks = Sticks;
					Again->Knives = Knives;
					FMemory::Memcpy(Again->bHas, Has, sizeof(Has));
					FMemory::Memcpy(Again->Ammo, Ammo, sizeof(Ammo));
					BotApplyWeapon(*Again);
					BotPublish(*Again, Now);
				}
				return;                              // (a fresh brain: driven from the next tick)
			}
		}
	}
	FVector Want = Step - Feet;
	Want.Z = 0.0;
	Want = Want.GetSafeNormal();
	if (Now < B.UnstickUntil) Want = B.UnstickDir;
	if (Want.IsNearlyZero()) Want = FRotator(0.0, FMath::FRandRange(-180.f, 180.f), 0.0).Vector();
	// the body's feel for what is in its way (a post, a barrel, a corner the way's points pass): along it, one way
	const float Rad = E->GetCapsuleComponent()->GetScaledCapsuleRadius();
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsBotFeel), false, E);
	// (raised over what its legs step up: BUG-BOTTEAMPLAY, drygulch's cellar stairs of 60 cm and the 75 cm step out of
	// them were felt as walls, and it went round them into the stairwell's sides)
	const float Lift = E->GetCharacterMovement()->MaxStepHeight + 5.f;
	const float FeelHalf = FMath::Max(Rad * 0.9f + 1.f, (2.f * Half - Lift) * 0.5f);
	const FVector Centre = Feet + FVector(0.0, 0.0, Lift + FeelHalf);
	const FCollisionShape Body = FCollisionShape::MakeCapsule(Rad * 0.9f, FeelHalf);
	FCollisionResponseParams Resp;
	Resp.CollisionResponse.SetResponse(ECC_GameTraceChannel2, ECR_Ignore);
	const double Ahead = FMath::Clamp(FVector::Dist2D(Step, Feet), 60.0, 150.0);
	auto Clear = [&](const FVector& Dir)
	{
		const FVector End = Centre + Dir * Ahead;
		if (W->SweepTestByChannel(Centre, End, FQuat::Identity, ECC_Pawn, Body, Q, Resp)) return false;
		FHitResult Hit;           // (ground ahead within its drop)
		return W->LineTraceSingleByChannel(Hit, End, FVector(End.X, End.Y, Feet.Z - 25.0 - E->Def.MaxDrop * UnitCmB), ECC_Visibility, Q);
	};
	const bool bWantClear = Clear(Want);
	// held up, or something in its way: a shut door there opens for it as for a player's push (no key; tried once a second;
	// GATE-FLAKES from bots11: it slid along a shut door at a run, so a "held up" test alone never pushed it; and the door
	// was the bad guys' rule's, so town_2p's spawn room door was never its to open).  At a few reaches: the push needs the
	// hand at the leaf's face, not through it, and the body may stand off it by anything from nothing to a stride
	if ((Speed < 100.0 || !bWantClear) && Now >= GDoorTryAt.FindRef(B.Who))
	{
		GDoorTryAt.Add(B.Who, Now + 1.0);
		const FVector Eye = E->EyePoint();
		for (const double Reach : { 5.0, 25.0, 45.0, 65.0 })
		{
			if (!L->EnemyOpenDoor(Eye, Eye + Want * (Rad + Reach), true)) continue;
			UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s opens a door on its way to %s"), *B.Name, *B.DestWhat);
			break;
		}
	}
	FVector Go = FVector::ZeroVector;
	if (bWantClear) Go = Want;
	else if (!B.LastGo.IsNearlyZero() && FVector::DotProduct(B.LastGo, Want) > 0.2 && Clear(B.LastGo)) Go = B.LastGo;
	else
	{
		static const float Angles[] = { 30.f, -30.f, 60.f, -60.f, 90.f, -90.f, 125.f, -125.f };
		for (const float A : Angles)
		{
			const FVector Dir = Want.RotateAngleAxis(A * B.Side, FVector::UpVector);
			if (Clear(Dir)) { Go = Dir; break; }
		}
	}
	B.LastGo = Go;
	if (Go.IsNearlyZero()) Go = Want;      // (nothing clear: on toward it; the stuck rule above steps it aside)
	E->MoveTarget = Feet + Go * FMath::Clamp(FVector::Dist2D(Step, Feet), 150.0, 400.0) + FVector(0.0, 0.0, Half);
	E->bRunning = true;
	E->HuntWait = 1.5f;
	if (E->Sub != AOutlawsEnemy::SubAdvance) E->SetSub(AOutlawsEnemy::SubAdvance);
	B.bDriving = true;
	++B.Drives;
}

// ---- dynamite and knives ------------------------------------------------------------------------------------------------

int32 UOutlawsCoop::BlastShooter = -1;
float UOutlawsCoop::BlastHostDealt = -1.f;
uint32 UOutlawsCoop::BlastStruckMask = 0;

namespace
{
	/** Each player's pace over the ground as the host sees it (sampled a few times a second, smoothed): a throw's lead. */
	struct FPaceTrack
	{
		FVector Feet = FVector::ZeroVector, Vel = FVector::ZeroVector;
		double At = 0.0;
		bool bOk = false;
	};
	FPaceTrack GPace[16];

	/** A bot's stick or knife in the air or (a stick) lying lit, on the host. */
	struct FBotFlying
	{
		int32 Who = -1;
		int32 Kind = 1;                      // 1 dynamite, 2 a knife (AOutlawsRemotePlayer::AddFlyer's kinds)
		FVector Pos = FVector::ZeroVector, Vel = FVector::ZeroVector;
		double EndAt = 0.0;                  // a stick's fuse out; a knife's flight given up
		bool bRest = false;
		int32 Epoch = 0;
		int32 Target = -1;                   // whom it was thrown at, whether they were on the move then, and (a knife) how
		bool bMoving = false;                // near it passed them (the test's count)
		double Closest = 1e9;
	};
	TArray<FBotFlying> GBotFlying;
	constexpr float BotThrowGrav = 1.5f;     // (980 cm/s2 times this, as every machine draws the flight: TickFlyers)
	constexpr float BotKnifeDamage = 3.f;   // (the match's thrown knife, OutlawsCoopMatchStrikes.cpp)
	constexpr double BotKnifeSpeed = 2200.0;

	/** The launch velocity of a lob from From to To at Speed under G (the lower arc, or bHigh the upper); false: out of its
	 *  reach. */
	bool LobVelocity(const FVector& From, const FVector& To, double Speed, double G, FVector& Out, bool bHigh = false)
	{
		const FVector D = To - From;
		const FVector Flat(D.X, D.Y, 0.0);
		const double X = Flat.Size();
		if (X < 1.0) return false;
		const double V2 = Speed * Speed;
		const double Disc = V2 * V2 - G * (G * X * X + 2.0 * D.Z * V2);
		if (Disc < 0.0) return false;
		const double Ang = FMath::Atan((V2 + (bHigh ? 1.0 : -1.0) * FMath::Sqrt(Disc)) / (G * X));
		Out = Flat / X * (Speed * FMath::Cos(Ang)) + FVector(0.0, 0.0, Speed * FMath::Sin(Ang));
		return true;
	}

	/** Whether the arc from From at Vel under G for T seconds comes to within Near of To without meeting the level (a wall,
	 *  a beam, a low ceiling) or another body on the way. */
	bool ArcClear(UWorld* W, const AActor* Self, const FVector& From, const FVector& Vel, double G, double T, const FVector& To, double Near)
	{
		if (!W) return false;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsBotThrow), false, Self);
		constexpr int32 Steps = 10;
		FVector P = From;
		for (int32 s = 1; s <= Steps; ++s)
		{
			const double t = T * s / Steps;
			const FVector N = From + Vel * t - FVector(0.0, 0.0, 0.5 * G * t * t);
			FHitResult Hit;
			if (W->LineTraceSingleByChannel(Hit, P, N, ECC_Visibility, Q)) return FVector::Dist(FVector(Hit.ImpactPoint), To) < Near;
			P = N;
		}
		return true;
	}
}

void UOutlawsCoop::BotThrow(FBot& B, AOutlawsLevelActor* L, double Now)
{
	AOutlawsEnemy* E = B.Brain.Get();
	// (the throw check, -OLBotThrowMany: every bot carries both and throws whenever it may, for a count of hits)
	static const bool bMany = OLFlags::Param(TEXT("OLBotThrowMany"));
	if (bMany) { B.Sticks = FMath::Max(B.Sticks, 1); B.Knives = FMath::Max(B.Knives, 1); }
	if (!E || !L || !E->bSeen || Now < B.NextThrowAt || (B.Sticks <= 0 && B.Knives <= 0)) return;
	B.NextThrowAt = Now + 1.0;                   // (looked at once a second while it sees someone)
	const int32 Who = E->CoopTarget;
	FVector TF;
	if (!BotFoe(B.Who, Who) || !PlayerFeetOf(Who, TF)) return;
	const float Half = E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const FVector Feet = E->GetActorLocation() - FVector(0.0, 0.0, Half);
	const double D = FVector::Dist2D(TF, Feet);
	// a stick at 8 to 22 m (outside its own blast's worst; on about the same floor), a knife at 2.5 to 10 m
	int32 Kind = 0;
	if (B.Sticks > 0 && D > 800.0 && D < 2200.0 && FMath::Abs(TF.Z - Feet.Z) < 400.0) Kind = 1;
	else if (B.Knives > 0 && D > 250.0 && D < 1000.0) Kind = 2;
	if (Kind == 0) return;
	const int32 Skill = FMath::Clamp(MatchOptions.BotSkill, 0, 2);
	const float Chance = Kind == 1 ? (Skill == 0 ? 0.12f : Skill == 2 ? 0.3f : 0.2f) : (Skill == 0 ? 0.15f : Skill == 2 ? 0.35f : 0.25f);
	const bool bRoll = bMany || FMath::FRand() <= Chance;
	// (the test, GATE-FLAKES: a chance the roll passed up still has its arc worked out, so the check knows how many clear
	// chances the bots had; a knife carried through a match where nobody came within its 2.5 to 10 m is no fault)
#if WITH_OUTLAWS_TESTS
	const bool bCountClear = TestOnly == TEXT("bots");
#else
	constexpr bool bCountClear = false;
#endif
	if (!bRoll && !bCountClear) return;
	// a team class: no stick where a teammate stands near the target
	if (Kind == 1 && Modes.IsTeamClass())
	{
		for (int32 k = 0; k < MaxPlayers; ++k)
		{
			FVector MF;
			if (k != B.Who && !BotFoe(B.Who, k) && PlayerFeetOf(k, MF) && FVector::Dist(MF, TF) < L->BlastRadiusCm() * 0.6f) return;
		}
	}
	const FVector Toward = (TF - Feet).GetSafeNormal2D();
	const FVector Hand = E->GetActorLocation() + FVector(0.0, 0.0, Half * 0.5) + Toward * 70.0;
	const FVector Rise(0.0, 0.0, Kind == 1 ? 10.0 : 100.0);                  // (a stick at the feet, a knife at the chest)
	const double G = 980.0 * BotThrowGrav;
	const FVector Pace = GPace[Who].bOk ? GPace[Who].Vel : FVector::ZeroVector;
	FVector Aim, Vel;
	// its skill: how far off it may land (either way), how much of the target's pace it leads by (where they will be
	// when the knife arrives or the stick goes off), and how long a stick lies lit before it goes
	const double Spread = Kind == 1 ? (Skill == 0 ? 500.0 : Skill == 2 ? 60.0 : 150.0) : (Skill == 0 ? 70.0 : Skill == 2 ? 12.0 : 28.0);
	const double Lead = Skill == 0 ? 0.0 : Skill == 2 ? 1.0 : 0.5;
	const double Lie = Kind == 1 ? (Skill == 0 ? 1.0 : Skill == 2 ? 0.3 : 0.6) : 0.0;
	const FVector Off = FVector(FMath::FRandRange(-1.f, 1.f), FMath::FRandRange(-1.f, 1.f), 0.f) * Spread;
	// the lower arc, or (a stick) the upper one over what is in the way; its whole flight clear to the mark, a few
	// rounds to settle the lead against the flight's time
	bool bSolved = false;
	double T = 0.0;
	for (int32 Arc = 0; Arc < (Kind == 1 ? 2 : 1) && !bSolved; ++Arc)
	{
		bool bOk = true;
		T = 0.0;
		for (int32 Round = 0; Round < 3 && bOk; ++Round)
		{
			Aim = TF + Rise + Off + Pace * (Lead * (T + Lie));
			const double X = FVector::Dist2D(Aim, Hand);
			const double Speed = Kind == 1 ? FMath::Max(900.0, FMath::Sqrt(G * X) * 1.1) : BotKnifeSpeed;
			bOk = LobVelocity(Hand, Aim, Speed, G, Vel, Arc == 1);
			if (bOk) T = X / FMath::Max(1.0, Vel.Size2D());
		}
		bSolved = bOk && ArcClear(E->GetWorld(), E, Hand, Vel, G, T, Aim, Kind == 1 ? 200.0 : 120.0);
	}
	if (!bSolved) return;
	++BotThrowClear;
	if (!bRoll) return;
	const float Fuse = float(T + Lie);
	FBotFlying F;
	F.Who = B.Who;
	F.Kind = Kind;
	F.Pos = Hand;
	F.Vel = Vel;
	F.EndAt = Now + (Kind == 1 ? Fuse : 3.0);
	F.Epoch = LevelEpoch();
	F.Target = Who;
	F.bMoving = Pace.Size2D() > 100.0;
	GBotFlying.Add(F);
	if (Kind == 1) --B.Sticks;
	else --B.Knives;
	++B.Throws;
	++BotThrowsMade;
	++BotThrowsBy[Kind - 1][F.bMoving ? 1 : 0];
	B.NextThrowAt = Now + (bMany ? 2.0 : Kind == 1 ? 8.0 : 3.0);
	OnThrow(B.Who, LevelEpoch(), uint8(Kind), Hand, Vel, BotThrowGrav, Kind == 1 ? Fuse : -1.f);     // (drawn here and on every client)
	UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s throws %s at the %s (%.0f m, %s %.1f m/s; %d sticks, %d knives left)"), *B.Name, Kind == 1 ? TEXT("a stick of dynamite") : TEXT("a knife"), WhoWord(Who), D / 100.0,
		F.bMoving ? TEXT("moving") : TEXT("standing"), Pace.Size2D() / 100.0, B.Sticks, B.Knives);
}

void UOutlawsCoop::TickBotThrown(AOutlawsLevelActor* L, double Dt)
{
	if (GBotFlying.Num() == 0 || !L) return;
	UWorld* W = L->GetWorld();
	const double Now = FPlatformTime::Seconds();
	const double G = 980.0 * BotThrowGrav;
	const int32 Epoch = LevelEpoch();
	auto BodyOf = [this](int32 Who) { const AOutlawsCoopLink* K = GetLink(Who); return K ? FVector(K->Pose.Body) : FVector::ZeroVector; };
	for (int32 i = GBotFlying.Num() - 1; i >= 0; --i)
	{
		FBotFlying& F = GBotFlying[i];
		if (F.Epoch != Epoch || !W) { GBotFlying.RemoveAt(i); continue; }
		bool bGone = false;
		if (!F.bRest)
		{
			F.Vel.Z -= G * Dt;
			const FVector To = F.Pos + F.Vel * Dt;
			if (F.Kind == 2)
			{
				// a knife: into the first body on its way (the host's own player here; the others as drawn here)
				FVector HF, At, TFt;
				bool bHit = false;
				if (PlayerFeetOf(F.Target, TFt))
				{
					const FVector Mid = TFt + FVector(0.0, 0.0, 90.0);
					F.Closest = FMath::Min(F.Closest, FVector::Dist(FMath::ClosestPointOnSegment(Mid, F.Pos, To), Mid));
				}
				if (BotFoe(F.Who, 0) && PlayerFeetOf(0, HF))
				{
					const FVector Mid = HF + FVector(0.0, 0.0, 90.0);
					const FVector C = FMath::ClosestPointOnSegment(Mid, F.Pos, To);
					if (FVector::Dist2D(C, Mid) < 40.0 && FMath::Abs(C.Z - Mid.Z) < 95.0)
					{
						OnMatchHit(F.Who, Epoch, 0, BotKnifeDamage, C, BodyOf(F.Who));
						bHit = true;
					}
				}
				if (!bHit)
				{
					TGuardValue<int32> By(BlastShooter, F.Who);
					bHit = SweepAtPlayers(L, F.Pos, To, BotKnifeDamage, TEXT("a bot's knife"), At);
				}
				if (bHit)
				{
					++BotThrowHits;
					++BotHitsBy[1][F.bMoving ? 1 : 0];
					UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s's knife strikes home"), *FindBot(F.Who)->Name);
					GBotFlying.RemoveAt(i);
					continue;
				}
			}
			FHitResult Hit;
			FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsBotThrown), false);
			if (W->LineTraceSingleByChannel(Hit, F.Pos, To, ECC_Visibility, Q))
			{
				F.Pos = FVector(Hit.ImpactPoint) + FVector(Hit.ImpactNormal) * 2.0;
				// a stick that meets a wall or a beam falls off it (stuck up there it went off out of sight of the one below)
				if (F.Kind == 1 && Hit.ImpactNormal.Z < 0.6) F.Vel = F.Vel.MirrorByVector(FVector(Hit.ImpactNormal)) * 0.3;
				else F.bRest = true;
				bGone = F.Kind == 2;                 // (a knife that met the level is spent)
			}
			else F.Pos = To;
		}
		if (F.Kind == 1 && Now >= F.EndAt)
		{
			// the stick goes off where it lies: the level's own blast (its sight and sound everywhere, the bodies in reach
			// struck through the match), the bot credited
			FVector HF, TFt;
			const bool bHostThere = BotFoe(F.Who, 0) && PlayerFeetOf(0, HF);
			const double FromMark = PlayerFeetOf(F.Target, TFt) ? FVector::Dist(TFt, F.Pos) : -100.0;
			{
				TGuardValue<int32> By(BlastShooter, F.Who);
				BlastHostDealt = -1.f;
				BlastStruckMask = 0;
				L->Explode(F.Pos);
			}
			const bool bHostHit = bHostThere && BlastHostDealt > 0.f;
			if (bHostHit) OnMatchHit(F.Who, Epoch, 0, BlastHostDealt, HF + FVector(0.0, 0.0, 90.0), BodyOf(F.Who), true);
			// struck: the host's player or any other opponent in its reach (BlastAtPlayers' strikes, through the match)
			int32 Foes = bHostHit ? 1 : 0;
			for (int32 k = 1; k < MaxPlayers; ++k) if ((BlastStruckMask & (1u << k)) && BotFoe(F.Who, k)) ++Foes;
			if (Foes > 0)
			{
				++BotThrowHits;
				++BotHitsBy[0][F.bMoving ? 1 : 0];
			}
			BlastHostDealt = -1.f;
			BlastStruckMask = 0;
			const FBot* B = FindBot(F.Who);
			UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s's stick goes off at %s, %.1f m from the %s: %d struck"), B ? *B->Name : TEXT("a gone bot"), *F.Pos.ToCompactString(), FromMark / 100.0, WhoWord(F.Target), Foes);
			bGone = true;
		}
		else if (F.Kind == 2 && Now >= F.EndAt) bGone = true;
		if (bGone && F.Kind == 2)
		{
			const FBot* B = FindBot(F.Who);
			UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s's knife misses the %s by %.1f m"), B ? *B->Name : TEXT("a gone bot"), WhoWord(F.Target), F.Closest / 100.0);
		}
		if (bGone) GBotFlying.RemoveAt(i);
	}
}

// ---- the host's tick ------------------------------------------------------------------------------------------------------

void UOutlawsCoop::TickBots(double Now, AOutlawsLevelActor* L)
{
	if (!IsHost() || !L) return;
	if (GNav.Serial != MatchSerial && Bots.Num() > 0 && bMatchSetUp)
	{
		if (UWorld* W = CurrentWorld()) BuildNav(W, Dm, MatchSerial);
	}
	{
		static double LastThrownAt = 0.0;
		const double Dt = LastThrownAt > 0.0 ? FMath::Min(0.1, Now - LastThrownAt) : 0.0;
		LastThrownAt = Now;
		TickBotThrown(L, Dt);                    // (the bots' sticks and knives in the air)
	}
	// everyone's pace, for the throws' lead (a jump of more than 15 m/s is a respawn or a put: no pace)
	static_assert(UE_ARRAY_COUNT(GPace) == MaxPlayers, "a pace for each player");
	for (int32 k = 0; k < MaxPlayers; ++k)
	{
		FPaceTrack& T = GPace[k];
		FVector F;
		if (!PlayerFeetOf(k, F)) { T.bOk = false; continue; }
		if (!T.bOk) { T = FPaceTrack(); T.Feet = F; T.At = Now; T.bOk = true; continue; }
		if (Now - T.At < 0.15) continue;
		FVector V = (F - T.Feet) / (Now - T.At);
		V.Z = 0.0;
		if (V.Size() > 1500.0) V = FVector::ZeroVector;
		T.Vel = FMath::Lerp(T.Vel, V, 0.5);
		T.Feet = F;
		T.At = Now;
	}
	const bool bOn = IsMatch() && MatchOptions.bBots && OutlawsMpHost::BotsAllowed(MatchOptions.GameClass);
	const int32 Want = bOn ? FMath::Clamp(MatchOptions.BotCount, 0, FMath::Max(0, MaxPlayersSetting() - HumanCount())) : 0;
	while (Bots.Num() > Want) RemoveBot(Bots.Last().Who, bOn ? TEXT("a human has its slot") : TEXT("bots are off"));
	for (int32 Guard = 0; Bots.Num() < Want && Guard < MaxPlayers; ++Guard)
	{
		const int32 Was = Bots.Num();
		AddBot(L);
		if (Bots.Num() == Was) break;
	}
	for (FBot& B : Bots)
	{
		AOutlawsEnemy* E = B.Brain.Get();
		// the brain fell to the world (a deadly floor) or was lost with a build of the level
		if (!B.bDead && E && E->IsDead())
		{
			BotHurt(B.Who, B.Health + 1.f, E->GetActorLocation(), E->GetActorLocation(), -1);
			continue;
		}
		if (!B.bDead && !E && B.DiedAt == 0.0 && B.PoseSeq > 0)
		{
			B.DiedAt = Now;
			B.bDead = true;
			B.RespawnAt = Now + 2.5;
		}
		if (B.bDead)
		{
			if (B.DiedAt > 0.0 && Now >= B.RespawnAt && MatchDead[B.Who])
			{
				B.RespawnAt = Now + 1.0;             // (refused for the delay: asked again)
				OnMatchRespawnAsk(B.Who);
			}
			BotPublish(B, Now);
			continue;
		}
		if (Now >= B.NextLookAt)
		{
			B.NextLookAt = Now + 0.2;
			BotLookForPickups(B, L, Now);
		}
		BotDrive(B, L, Now);
		BotThrow(B, L, Now);
		if (E && TestOnly == TEXT("bots"))
		{
			// (the test: the brain never throttled; and once a second, how much nearer by the way it came to whoever it hunts
			// while it saw nobody: wandering comes no nearer)
			if (E->ThrottleTier != 0) ++B.TierSeen;
			if (E->IsFarFrozen()) ++B.FrozenSeen;
			if (Now - B.HuntSampleAt >= 1.0)
			{
				const float Half = E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
				const FVector Feet = E->GetActorLocation() - FVector(0.0, 0.0, Half);
				FVector TF;
				TArray<FVector> R;
				double Len = -1.0;
				const int32 Who = B.HuntWho;
				if (E->bSeen || Who < 0 || !PlayerFeetOf(Who, TF) || !L->PathRoute(Feet, TF, E->Def.MaxStep, 2.0 * Half / UnitCmB, 1.5, R, Len, E->Def.MaxDrop)) Len = -1.0;
				if (Len >= 0.0 && B.HuntLen >= 0.0 && Who == B.HuntSampleWho && Now - B.HuntSampleAt < 1.6)
				{
					B.ApproachCm += B.HuntLen - Len;
					// (its own way: from where it stood a second ago to where they stand now, so their going is not its)
					TArray<FVector> R0;
					double Len0 = -1.0;
					if (L->PathRoute(B.HuntFeet, TF, E->Def.MaxStep, 2.0 * Half / UnitCmB, 1.5, R0, Len0, E->Def.MaxDrop) && Len0 >= 0.0)
					{
						B.OwnApproachCm += Len0 - Len;
						if (B.bIvHunt && B.IvDriveCm > 0.0)
						{
							B.HuntDriveCm += B.IvDriveCm;
							B.HuntOwnCm += Len0 - Len;
							B.HuntNearCm += B.HuntLen - Len;
						}
					}
				}
				B.IvDriveCm = 0.0;
				B.bIvHunt = true;
				B.HuntFeet = Feet;
				B.HuntLen = Len;
				B.HuntSampleWho = Who;
				B.HuntSampleAt = Now;
			}
		}
		// BOTSRANGE: while it sees someone, the gun for the distance (RangeBand), rolled once a band: among the band's guns by
		// its liking, now and then (15 %) its liking alone; the revolver when it carries none of the band's.  A band changes a
		// step (0.75 m) past its edge, a swap at most every 3 s and not in a burst (at once when the gun runs dry).
		// -OLBotOldGuns: its liking alone, every 12 to 20 s, as before.
		static const bool bOldGuns = OLFlags::Param(TEXT("OLBotOldGuns"));
		bool bRangeSeen = false;
		FRangeHold& H = GRangeHold.FindOrAdd(B.Who);
		FVector TF;
		if (E && !bOldGuns && E->bSeen && E->CoopTarget >= 0 && E->CoopTarget != B.Who && PlayerFeetOf(E->CoopTarget, TF))
		{
			bRangeSeen = true;
			const float M = float(FVector::Dist(E->GetActorLocation() - FVector(0.0, 0.0, E->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()), TF) / 100.0);
			int32 Band = RangeBand(M);
			if (H.Band >= 0 && FMath::Abs(Band - H.Band) == 1 && FMath::Abs(M - (FMath::Max(Band, H.Band) == 1 ? 6.f : 12.f)) < 0.75f) Band = H.Band;
			const bool bDry = !GunUsable(B, B.Weapon);
			if ((Band != H.Band || bDry) && (bDry || (Now - H.SwapAt >= 3.0 && E->BurstLeft <= 0)))
			{
				H.Band = Band;
				const FBotPref& Pf = PrefOf(B.Short);
				int32 Pick = 0, Total = 0;
				const bool bLiking = FMath::FRand() < 0.15f;
				for (int32 g = 0; g < 5; ++g) if (GunUsable(B, g) && (bLiking || GunFits(g, Band))) Total += Pf.W[g] + (bLiking ? 0 : 5);
				if (Total > 0)
				{
					int32 R = FMath::RandHelper(Total);
					for (int32 g = 0; g < 5; ++g)
					{
						if (!GunUsable(B, g) || !(bLiking || GunFits(g, Band))) continue;
						const int32 Wt = Pf.W[g] + (bLiking ? 0 : 5);
						if (R < Wt) { Pick = g; break; }
						R -= Wt;
					}
				}
				if (Pick != B.Weapon)
				{
					UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT %s takes the %s (by range: %.1f m%s; was the %s)"), *B.Name, BotGun(Pick).Label, M, bLiking ? TEXT(", its liking") : TEXT(""), BotGun(B.Weapon).Label);
					B.Weapon = Pick;
					BotApplyWeapon(B);
					H.SwapAt = Now;
					++GRangeSwaps;
				}
			}
		}
		else H.Band = -1;
		if (bRangeSeen) B.NextChooseAt = FMath::Max(B.NextChooseAt, Now + 4.0);       // (its liking again only 4 s out of sight)
		if (Now >= B.NextChooseAt && !bRangeSeen)
		{
			B.NextChooseAt = Now + FMath::FRandRange(12.0, 20.0);
			const int32 Was = B.Weapon;
			BotChooseWeapon(B, TEXT("its liking"));
			GLikingSwaps += B.Weapon != Was;
		}
		BotPublish(B, Now);
	}
}

// ---- -OLCoopTest=bots ---------------------------------------------------------------------------------------------------
// The host: -OLMpHost=<map> -OLMpBots=<n> (-OLCoopMaxPlayers=<n>): the bots in, the match started, two minutes of it, the
// host respawned when dead, a look at a bot photographed, the scores; then the checks and the end.  A client (-OLCoopJoin):
// respawns when dead, reports the bots it draws and photographs one.

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TickBotTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	auto Check = [](bool bOk, const FString& What) { UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT test check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What); };
	// either machine: back from the dead after 3 s (the death screen's button)
	if (L->bDead && IsMatch())
	{
		if (BotTestDeadAt == 0.0) BotTestDeadAt = Now;
		else if (Now - BotTestDeadAt > (OLFlags::Param(TEXT("OLTntVanishTest")) ? 0.4 : 3.0))     // (BUG-TNTVANISH: back while the body flies)
		{
			BotTestDeadAt = Now;
			MatchRespawn(P);
		}
	}
	else BotTestDeadAt = 0.0;
	// (BUG-MPDEATHSOUND, BUG-BOTTNT) the deaths this machine showed: the cries heard from the bodies, the bodies blasts threw
	auto DeathTallies = [this]()
	{
		UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT test deaths here (%s): cries heard %d [%s]; bodies thrown %d, landed %d [%s]"), IsHost() ? TEXT("host") : WhoWord(MyIndex()),
			AOutlawsRemotePlayer::DeathCriesHeard, *FString::Join(AOutlawsRemotePlayer::CryLines, TEXT(", ")), AOutlawsRemotePlayer::BodiesFlung,
			AOutlawsRemotePlayer::BodiesLanded, *FString::Join(AOutlawsRemotePlayer::FlingLines, TEXT(", ")));
	};
	// (-OLTntVanishTest, BUG-TNTVANISH) the bodies still in the air when their player came back: each drawn on alone till it
	// landed, and the comeback drawn at its own place apart from it; a bot's body here, and the host also a remote player's
	auto TntVanishChecks = [this, &Check]()
	{
		typedef AOutlawsRemotePlayer RP;
		const FString Lines = FString::Join(RP::LeftLines, TEXT(", "));
		UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT test tntvanish (%s): left in the air %d (bots %d), landed %d, frames not drawn %d, gone from the air %d, comebacks at their place %d of %d [%s]"),
			IsHost() ? TEXT("host") : WhoWord(MyIndex()), RP::BodiesLeft, RP::BodiesLeftBots, RP::BodiesLeftLanded, RP::BodiesLeftUnseen, RP::BodiesVanished, RP::ComebacksAtSpawn, RP::ComebacksChecked, *Lines);
		Check(RP::BodiesLeftBots > 0, FString::Printf(TEXT("a bot's thrown body flew on after it respawned here (%d)"), RP::BodiesLeftBots));
		if (IsHost()) Check(RP::BodiesLeft - RP::BodiesLeftBots > 0, FString::Printf(TEXT("a remote player's thrown body flew on after they respawned (%d)"), RP::BodiesLeft - RP::BodiesLeftBots));
		Check(RP::BodiesVanished == 0, FString::Printf(TEXT("no thrown body gone from the air at a respawn (%d)"), RP::BodiesVanished));
		Check(RP::BodiesLeftLanded == RP::BodiesLeft && RP::BodiesLeftUnseen == 0, FString::Printf(TEXT("each drawn till it landed: %d of %d landed, %d frames not drawn"),
			RP::BodiesLeftLanded, RP::BodiesLeft, RP::BodiesLeftUnseen));
		Check(RP::ComebacksChecked > 0 && RP::ComebacksAtSpawn == RP::ComebacksChecked, FString::Printf(TEXT("the comebacks drawn at their own place: %d of %d"), RP::ComebacksAtSpawn, RP::ComebacksChecked));
	};
	static const bool bTntVanish = OLFlags::Param(TEXT("OLTntVanishTest"));
	// the nearest bot drawn here, and a place 4 m from it facing it
	auto LookAtBot = [this, L, P](const TCHAR* ShotName) -> bool
	{
		// (-OLScoreAboveTest rides on this test's bots: moving the player for this picture in the middle of its wall step spoiled
		// that step's check and picture, so with it on the player stays put and this picture is not taken)
		static const bool bScoreAbove = OLFlags::Param(TEXT("OLScoreAboveTest"));
		if (bScoreAbove) return false;
		TArray<AOutlawsCoopLink*> Links;
		GetLinks(Links);
		const AOutlawsRemotePlayer* Best = nullptr;
		FString BestName;
		double BestD = 1e12;
		for (const AOutlawsCoopLink* K : Links)
		{
			if (!K->bBot) continue;
			const AOutlawsRemotePlayer* R = PuppetOf(K->PlayerIndex);
			if (!R || R->PlateDead()) continue;
			const double D = FVector::Dist(R->ShownBody(), P->GetActorLocation());
			if (D < BestD) { BestD = D; Best = R; BestName = K->PlayerName; }
		}
		if (!Best) return false;
		UWorld* W = P->GetWorld();
		const FVector Mid = Best->ShownBody();
		const float Half = P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(), Rad = P->GetCapsuleComponent()->GetScaledCapsuleRadius();
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsBotTestLook), false, P);
		for (int32 k = 0; k < 8; ++k)
		{
			const FVector At = Mid + FRotator(0.0, 45.0 * k, 0.0).Vector() * 400.0;
			FHitResult Hit;
			if (W->LineTraceSingleByChannel(Hit, Mid, At, ECC_Visibility, Q)) continue;
			if (W->OverlapBlockingTestByChannel(At, FQuat::Identity, ECC_Pawn, FCollisionShape::MakeCapsule(Rad, Half), Q)) continue;
			const FRotator Face = (Mid - At).Rotation();
			P->TeleportTo(At, FRotator(0.0, Face.Yaw, 0.0), false, true);
			if (AController* Ctl = P->GetController()) Ctl->SetControlRotation(FRotator(FMath::Clamp(Face.Pitch, -30.0, 30.0), Face.Yaw, 0.0));
			UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT test: the %s stands 4 m from %s (%s, %s) for %s"), IsHost() ? TEXT("host") : WhoWord(MyIndex()), *BestName, Best->ShownState(), *Best->PlateState(), ShotName);
			BotTestAt = FPlatformTime::Seconds() + 0.4;
			return true;
		}
		return false;
	};
	auto Report = [this](const TCHAR* Who)
	{
		TArray<AOutlawsCoopLink*> Links;
		GetLinks(Links);
		FString Seen;
		int32 N = 0;
		for (const AOutlawsCoopLink* K : Links)
		{
			if (!K->bBot) continue;
			const AOutlawsRemotePlayer* R = PuppetOf(K->PlayerIndex);
			++N;
			Seen += FString::Printf(TEXT("%s%s as %s: %s"), Seen.IsEmpty() ? TEXT("") : TEXT("; "), *K->PlayerName, *K->Character,
				R ? *FString::Printf(TEXT("drawn %s, %s, %s"), *R->SpriteNwx, R->ShownState(), *R->PlateState()) : TEXT("not drawn"));
		}
		UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT test: the %s sees %d bots: %s"), Who, N, *Seen);
		return N;
	};
	if (!IsHost())
	{
		// the client: a look and a photograph 30 s in, the bots it draws every 15 s, and how far and how smoothly each moves
		// here (a step of over 1.5 m in one frame while it lives, not just after a respawn, is a jump)
		if (BotTestStep == 0) { BotTestStep = 1; BotTestLiveAt = Now; BotTestNextReport = Now + 10.0; }
		static double PrevFrameAt = 0.0;
		const double FrameDt = PrevFrameAt > 0.0 ? Now - PrevFrameAt : 0.0;
		PrevFrameAt = Now;
		{
			TArray<AOutlawsCoopLink*> Links;
			GetLinks(Links);
			for (const AOutlawsCoopLink* K : Links)
			{
				if (!K->bBot) continue;
				const AOutlawsRemotePlayer* R = PuppetOf(K->PlayerIndex);
				if (!R) continue;
				FBotSeen& S = BotSeen.FindOrAdd(K->PlayerIndex);
				const bool bDead = R->PlateDead() || K->Stats.bDead;
				const FVector At = R->ShownBody();
				if (bDead) S.DeadAt = Now;
				if (S.bWasDead && !bDead) ++S.Respawns;
				if (S.bHave && !bDead && !S.bWasDead && Now - S.DeadAt > 1.0)
				{
					const double Step = FVector::Dist(At, S.Last);
					// (8 m and more in a frame: put somewhere, a spawn at the match's start; under it, a jerk in the drawing:
					// over 1.5 m at more than twice a running player's speed for this frame's length, so a slow frame here
					// that carries it on as far as it ran is not one)
					if (Step >= 800.0) ++S.Teleports;
					else
					{
						if (Step > 150.0 && Step > FrameDt * 1500.0)
						{
							++S.Jumps;
							// (GATE-FLAKES: each jump told, with how the drawing stood: run on past the newest pose, the sender's gap)
							UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT test jump: the %s draws %s step %.0f cm in a %.3f s frame; run on past the newest pose %.2f s, the newest two %.2f s apart, cuts %d"),
								WhoWord(MyIndex()), *K->PlayerName, Step, FrameDt, R->PoseLateBy(), R->PoseNewestGap(), R->PoseCuts());
						}
						S.MovedCm += Step;
						S.MaxStep = FMath::Max(S.MaxStep, Step);
					}
				}
				S.Last = At;
				S.bHave = true;
				S.bWasDead = bDead;
			}
		}
		const bool bLast = BotTestStep < 8 && Now - BotTestLiveAt > 110.0;     // (a report as the host's two minutes end; its quit ends this one too)
		if (Now >= BotTestNextReport || bLast)
		{
			BotTestNextReport = Now + 15.0;
			Report(WhoWord(MyIndex()));
			DeathTallies();
			TArray<AOutlawsCoopLink*> Links;
			GetLinks(Links);
			for (const AOutlawsCoopLink* K : Links)
			{
				const FBotSeen* S = K->bBot ? BotSeen.Find(K->PlayerIndex) : nullptr;
				const AOutlawsRemotePlayer* R = K->bBot ? PuppetOf(K->PlayerIndex) : nullptr;
				if (!S || !R) continue;
				UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT test: the %s draws %s (player %d): moved %.1f m, jumps %d, largest step %.0f cm, put %d, respawns %d, pose cuts %d, late frames %d"), WhoWord(MyIndex()),
					*K->PlayerName, K->PlayerIndex, S->MovedCm / 100.0, S->Jumps, S->MaxStep, S->Teleports, S->Respawns, R->PoseCuts(), R->PoseExtrapolated());
			}
		}
		if (BotTestStep == 1 && Now - BotTestLiveAt > 30.0 && !L->bDead && LookAtBot(TEXT("ol_bots_client"))) BotTestStep = 2;
		else if (BotTestStep == 2 && Now >= BotTestAt) { Shot(TEXT("ol_bots_client")); BotTestStep = 3; }
		if (bLast && bTntVanish) TntVanishChecks();
		if (bLast) BotTestStep = 8;
		return;
	}
	FOutlawsMatchFlow& F = OutlawsMpHost::Flow();
	const int32 Expect = FMath::Max(0, LaunchBots);
	switch (BotTestStep)
	{
	case 0:
		if (!bMatchSetUp || Bots.Num() == 0 || Bots.Num() < FMath::Min(Expect, MaxPlayersSetting() - HumanCount())) return;
		{
			FString Names;
			for (const FBot& B : Bots) Names += FString::Printf(TEXT("%s%s"), Names.IsEmpty() ? TEXT("") : TEXT(", "), *B.Name);
			Check(Bots.Num() == FMath::Min(Expect, MaxPlayersSetting() - HumanCount()), FString::Printf(TEXT("%d bots in as named characters: %s"), Bots.Num(), *Names));
		}
		BotTestAt = Now + 6.0;
		BotTestStep = 1;
		return;
	case 1:
		if (Now < BotTestAt) return;
		Check(F.HostStart(), FString::Printf(TEXT("the host's Start the game with bots (%d players)"), F.GetPlayers()));
		BotTestStep = 2;
		return;
	case 2:
		if (F.GetState() != FOutlawsMatchFlow::EState::Playing) return;
		BotTestLiveAt = Now;
		BotTestNextReport = Now + 20.0;
		BotTestStep = 3;
		return;
	case 3:
	case 4:
	case 5:
	{
		if (Now >= BotTestNextReport)
		{
			BotTestNextReport = Now + 20.0;
			Report(TEXT("host"));
			for (const FBot& B : Bots)
			{
				UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT test: %s"), *BotLine(B));
				UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT test way: %s drove %.1f m, came %.1f m nearer to whoever it hunted (%.1f m by its own way), met someone %d times, routes %d (no way %d), stuck %d; throttled %d, far frozen %d; on the hunt %.1f m driven, %.1f m nearer by its own way"),
					*B.Name, B.DriveCm / 100.0, B.ApproachCm / 100.0, B.OwnApproachCm / 100.0, B.Contacts, B.Routes, B.RouteFails, B.Stucks, B.TierSeen, B.FrozenSeen, B.HuntDriveCm / 100.0, B.HuntOwnCm / 100.0);
			}
			UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT test scores %.0f s in: %s"), Now - BotTestLiveAt, *MatchSummary());
			DeathTallies();
		}
		// -OLBotTntTest (BUG-BOTTNT): a stick goes off by a living bot every 12 s from 40 s in (the host well away): it dies
		// and its body flies, here and on the client
		static const bool bTnt = OLFlags::Param(TEXT("OLBotTntTest")) || bTntVanish;
		static double TntNextAt = 0.0;
		static int32 TntSet = 0;
		// (-OLTntVanishTest, BUG-TNTVANISH) the rules' least time dead cut to 0.3 s and each dead bot back 0.4 s after its death,
		// so it comes back while its thrown body is still in the air; the client comes back 0.4 s after its own
		if (bTntVanish)
		{
			Dm.Config.RespawnDelay = FMath::Min(Dm.Config.RespawnDelay, 0.3f);
			for (FBot& B : Bots) if (B.bDead && B.DiedAt > 0.0 && B.RespawnAt > Now && Now - B.DiedAt > 0.4) B.RespawnAt = Now;
		}
		if (bTnt && Now - BotTestLiveAt > 40.0 && Now - BotTestLiveAt < 110.0 && Now >= TntNextAt)
		{
			TntNextAt = Now + 12.0;
			const double Reach = L->BlastRadiusCm() * 1.2;
			TArray<AOutlawsCoopLink*> Links;
			GetLinks(Links);
			for (const AOutlawsCoopLink* K : Links)
			{
				const AOutlawsRemotePlayer* R = K->bBot ? PuppetOf(K->PlayerIndex) : nullptr;
				if (!R || R->IsHidden() || R->PlateDead() || K->Stats.bDead) continue;
				const FVector Feet = R->ShownBody() - FVector(0.0, 0.0, R->ShownHalf());
				if (P && FVector::Dist(P->GetActorLocation(), R->ShownBody()) < Reach) continue;
				const FVector At = Feet + FRotator(0.0, double(TntSet) * 97.0, 0.0).Vector() * 60.0 + FVector(0.0, 0.0, 10.0);
				++TntSet;
				UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT test tnt %d: a stick goes off by %s at %s"), TntSet, *K->PlayerName, *At.ToCompactString());
				L->Explode(At);
				break;
			}
			// (-OLTntVanishTest) and one by the client, a remote player here (the host still well away)
			if (bTntVanish) for (const AOutlawsCoopLink* K : Links)
			{
				const AOutlawsRemotePlayer* R = !K->bBot && K->PlayerIndex != MyIndex() ? PuppetOf(K->PlayerIndex) : nullptr;
				if (!R || R->IsHidden() || R->PlateDead() || K->Stats.bDead) continue;
				if (P && FVector::Dist(P->GetActorLocation(), R->ShownBody()) < Reach) continue;
				const FVector At = R->ShownBody() - FVector(0.0, 0.0, R->ShownHalf()) + FRotator(0.0, double(TntSet) * 97.0, 0.0).Vector() * 60.0 + FVector(0.0, 0.0, 10.0);
				++TntSet;
				UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT test tnt %d: a stick goes off by the %s at %s"), TntSet, WhoWord(K->PlayerIndex), *At.ToCompactString());
				L->Explode(At);
				break;
			}
		}
		if (BotTestStep == 3 && Now - BotTestLiveAt > 25.0 && !L->bDead && LookAtBot(TEXT("ol_bots_host"))) BotTestStep = 4;
		else if (BotTestStep == 4 && FPlatformTime::Seconds() >= BotTestAt) { Shot(TEXT("ol_bots_host")); BotTestStep = 5; }
		if (Now - BotTestLiveAt < 120.0) return;
		// the checks: in, picking up, killing, dying, respawning, and the table with their kills
		int32 BotFrags = 0, BotDeaths = 0;
		for (const FOutlawsDmPlayer& Dp : Dm.GetPlayers()) if (IsBot(Dp.Id)) { BotFrags += FMath::Max(0, Dp.Frags); BotDeaths += Dp.Deaths; }
		const int32 Humans = HumanCount();
		Check(Bots.Num() == FMath::Min(Expect, MaxPlayersSetting() - Humans), FString::Printf(TEXT("%d bots for %d humans, max %d players (asked for %d)"), Bots.Num(), Humans, MaxPlayersSetting(), Expect));
		// (BUG-BOTPICKUP2: a pickup is owed once the bots set out for 4; town_2p's dm bots, a gun or two they lack 20 m off and
		// knives for the knifeless within 8 m, fought where they spawned in 1 run of 7 and wanted nothing in reach)
		if (BotPickups > 0 || BotPickupTrips >= 4) Check(BotPickups > 0, FString::Printf(TEXT("the bots picked up %d items (set out for %d)"), BotPickups, BotPickupTrips));
		else UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT test: no pickup to check (set out for %d)"), BotPickupTrips);
		Check(BotFrags > 0, FString::Printf(TEXT("the bots made %d kills"), BotFrags));
		Check(BotDeaths > 0, FString::Printf(TEXT("the bots died %d times"), BotDeaths));
		Check(BotRespawns > 0, FString::Printf(TEXT("the bots respawned %d times"), BotRespawns));
		// the way-finding: each went a real distance on its own; together they came nearer (by the way through the level)
		// to whoever they hunted for a good part of the ground they covered (a wanderer comes no nearer), and met someone
		double Drive = 0.0, Approach = 0.0, LeastDrive = 1e12;
		int32 Contacts = 0, Throttled = 0, Fails = 0, Routes = 0;
		FString Ways;
		for (const FBot& B : Bots)
		{
			Drive += B.DriveCm;
			Approach += B.ApproachCm;
			LeastDrive = FMath::Min(LeastDrive, B.DriveCm);
			Contacts += B.Contacts;
			Throttled += B.TierSeen + B.FrozenSeen;
			Fails += B.RouteFails;
			Routes += B.Routes;
			Ways += FString::Printf(TEXT("%s%s %.0f m / %.0f m nearer (own way %.0f) / met %d / stuck %d"), Ways.IsEmpty() ? TEXT("") : TEXT("; "), *B.Name, B.DriveCm / 100.0, B.ApproachCm / 100.0, B.OwnApproachCm / 100.0, B.Contacts, B.Stucks);
		}
		Check(Bots.Num() > 0 && LeastDrive >= 1500.0, FString::Printf(TEXT("each bot went its own way at least 15 m (%s)"), *Ways));
		// (GATE-FLAKES, bots11's measure: over the seconds each spent all on its own drive after whoever it hunted, by its own
		// way; before, every metre driven counted, to pickups and far places too, against a nearness that also fell in the
		// seconds its quarry ran off, so a sound hunt wobbled about the 20 % line)
		double HuntDrive = 0.0, HuntOwn = 0.0, HuntNear = 0.0;
		for (const FBot& B : Bots) { HuntDrive += B.HuntDriveCm; HuntOwn += B.HuntOwnCm; HuntNear += B.HuntNearCm; }
		UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT test approach (the old measure): the bots came %.0f m nearer to whoever they hunted over %.0f m driven (%.0f %%); routes %d, no way %d"),
			Approach / 100.0, Drive / 100.0, Drive > 0.0 ? 100.0 * Approach / Drive : 0.0, Routes, Fails);
		Check(HuntDrive >= 3000.0 && HuntOwn >= HuntDrive * 0.3, FString::Printf(TEXT("the bots came %.0f m nearer to whoever they hunted (by their own way) over %.0f m driven after them (%.0f %%, want 30 %%; with their quarry's going %.0f m); routes %d, no way %d"),
			HuntOwn / 100.0, HuntDrive / 100.0, HuntDrive > 0.0 ? 100.0 * HuntOwn / HuntDrive : 0.0, HuntNear / 100.0, Routes, Fails));
		Check(Contacts >= Bots.Num(), FString::Printf(TEXT("the bots met an opponent %d times"), Contacts));
		int32 Kit = 0;
		for (const FBot& B : Bots) Kit += B.Sticks + B.Knives + B.Throws;
		// (GATE-FLAKES: a throw is owed once the bots had 16 clear chances, someone at its range and a clear arc: at a fifth to a
		// quarter of them taken, none in 16 is under 3 in 100; a knife carried through a match where nobody came near is no fault)
		if (Kit > 0 && (BotThrowClear >= 16 || BotThrowsMade > 0)) Check(BotThrowsMade > 0, FString::Printf(TEXT("the bots threw %d sticks and knives in %d clear chances (%d struck; sticks at one standing %d of %d, moving %d of %d; knives standing %d of %d, moving %d of %d)"), BotThrowsMade, BotThrowClear, BotThrowHits, BotHitsBy[0][0], BotThrowsBy[0][0], BotHitsBy[0][1], BotThrowsBy[0][1], BotHitsBy[1][0], BotThrowsBy[1][0], BotHitsBy[1][1], BotThrowsBy[1][1]));
		else UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT test: no throw to check (kit %d, %d clear chances)"), Kit, BotThrowClear);
		{
			// (BOTSRANGE) the shots fired with the band's gun for the distance (the shotguns inside 6 m, the rifle past 12 m,
			// the revolver between); of those where it carried the band's gun; each gun's shots and their mean distance
			const int32 N = GRangeShots[0] + GRangeShots[1] + GRangeShots[2], Right = GRangeRight[0] + GRangeRight[1] + GRangeRight[2];
			const int32 Could = GRangeCould[0] + GRangeCould[1] + GRangeCould[2], RightCould = GRangeRightCould[0] + GRangeRightCould[1] + GRangeRightCould[2];
			FString Guns;
			for (int32 g = 0; g < 5; ++g) if (GGunShots[g] > 0) Guns += FString::Printf(TEXT("%s%s %d at %.1f m"), Guns.IsEmpty() ? TEXT("") : TEXT(", "), BotGun(g).Label, GGunShots[g], GGunShotM[g] / GGunShots[g]);
			const double BotMinutes = FMath::Max(1.0, Bots.Num() * (Now - BotTestLiveAt) / 60.0);
			const FString Line = FString::Printf(TEXT("%d shots, %d with the gun for the distance (%.0f %%); where it carried that gun %d, %d with it (%.0f %%); inside 6 m %d/%d, 6 to 12 m %d/%d, past 12 m %d/%d; by gun: %s; swaps by range %d, by liking %d (%.1f a bot-minute)%s"),
				N, Right, N > 0 ? 100.0 * Right / N : 0.0, Could, RightCould, Could > 0 ? 100.0 * RightCould / Could : 0.0, GRangeRight[0], GRangeShots[0], GRangeRight[1], GRangeShots[1], GRangeRight[2], GRangeShots[2],
				Guns.IsEmpty() ? TEXT("none") : *Guns, GRangeSwaps, GLikingSwaps, (GRangeSwaps + GLikingSwaps) / BotMinutes, OLFlags::Param(TEXT("OLBotOldGuns")) ? TEXT(" (-OLBotOldGuns)") : TEXT(""));
			UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT test range: %s"), *Line);
			// (20 runs: 76 to 97 % where it carried the band's gun, 59 % with -OLBotOldGuns)
			if (!OLFlags::Param(TEXT("OLBotOldGuns")) && Could >= 30) Check(RightCould >= Could * 0.6, FString::Printf(TEXT("the bots shot with the gun for the distance %d of the %d times they carried it (%.0f %%, want 60 %%)"), RightCould, Could, 100.0 * RightCould / Could));
		}
		// (BOTSHURT) the falls under 40 %: with health in reach (25 m by the way); of those alive a second on, how many headed for it within 10 s and were
		// healed within 15 s; the rest, to cover; died within 15 s; health taken; the bots' kills
		UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT test hurt: %d falls under 40 %%, %d with health in reach (%d alive 1 s on), %d headed for it (%.0f %%), %d healed within 15 s (%.0f %%); %d to cover; %d died hurt; health taken %d; bot kills %d (%.2f a bot)%s"),
			GHurtFalls, GHurtReachAll, GHurtReach, GHurtHeaded, GHurtReach > 0 ? 100.0 * GHurtHeaded / GHurtReach : 0.0, GHurtHealed, GHurtReach > 0 ? 100.0 * GHurtHealed / GHurtReach : 0.0,
			GHurtCover, GHurtDied, GHealthTaken, BotFrags, Bots.Num() > 0 ? double(BotFrags) / Bots.Num() : 0.0, OldHurt() ? TEXT(" (-OLBotOldHurt)") : TEXT(""));
		// (BOTSTUCK) the hurt bots' ways: health passed by for a way its body does not fit, places given up after a 2 s stall,
		// break-offs ended on a third; the stucks and the shut-in moves of all the bots
		{
			int32 Stucks = 0;
			for (const FBot& Bt : Bots) Stucks += Bt.Stucks;
			UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT test stuck: %d stucks, %d shut in and put at a spawn point; hurt: %d health passed by (no body way), %d places given up, %d break-offs ended%s"),
				Stucks, GBotsFreed, GHurtBlocked, GHurtGiveUps, GHurtEnded, OldStuck() ? TEXT(" (-OLBotOldStuck)") : TEXT(""));
		}
		// (20 runs: 97 % headed, 48 % healed within 15 s; 9 % and 3 % with -OLBotOldHurt)
		if (!OldHurt() && GHurtReach >= 3) Check(GHurtHeaded >= GHurtReach * 0.6, FString::Printf(TEXT("the hurt bots headed for health in reach %d of %d times (%.0f %%, want 60 %%)"), GHurtHeaded, GHurtReach, 100.0 * GHurtHeaded / GHurtReach));
		Check(Throttled == 0, FString::Printf(TEXT("no bot throttled or far frozen (%d frames; %s)"), Throttled, *L->ThrottleSummary()));
		if (Modes.IsTeamClass())
		{
			FString Teams;
			bool bAllOn = true;
			for (int32 k = 0; k < MaxPlayers; ++k)
			{
				if (!GetLink(k)) continue;
				const int32 T = Modes.TeamOf(k);
				bAllOn = bAllOn && (T == 1 || T == 2);
				Teams += FString::Printf(TEXT("%s%s %s"), Teams.IsEmpty() ? TEXT("") : TEXT(", "), IsBot(k) ? *FindBot(k)->Name : WhoWord(k), FOutlawsMatchModes::TeamName(T));
			}
			Check(bAllOn, FString::Printf(TEXT("every player on a team: %s"), *Teams));
			Check(GBotTeamHits == 0, FString::Printf(TEXT("no bot hit by a teammate (%d; shots at a teammate turned away %d)"), GBotTeamHits, GBotTeamRefused));
		}
		FFragTableData T;
		const bool bTable = FillMatchTable(T);
		Check(bTable && T.Rows.Num() == Bots.Num() + Humans, FString::Printf(TEXT("the frag table: %s"), *T.Describe()));
		UE_LOG(LogOutlawsAI, Log, TEXT("OLBOT test final scores: %s"), *MatchSummary());
		DeathTallies();
		if (bTntVanish) TntVanishChecks();
		if (OLFlags::Param(TEXT("OLBotTntTest")))
		{
			Check(AOutlawsRemotePlayer::BodiesFlung > 0 && AOutlawsRemotePlayer::BodiesLanded > 0, FString::Printf(TEXT("bodies thrown by blasts here: %d, landed %d [%s]"),
				AOutlawsRemotePlayer::BodiesFlung, AOutlawsRemotePlayer::BodiesLanded, *FString::Join(AOutlawsRemotePlayer::FlingLines, TEXT(", "))));
			Check(AOutlawsRemotePlayer::DeathCriesHeard > 0, FString::Printf(TEXT("death cries heard here: %d [%s]"), AOutlawsRemotePlayer::DeathCriesHeard,
				*FString::Join(AOutlawsRemotePlayer::CryLines, TEXT(", "))));
		}
		ShowMatchPanel(10.f);
		BotTestAt = Now + 1.0;
		BotTestStep = 6;
		return;
	}
	case 6:
		if (Now < BotTestAt) return;
		Shot(TEXT("ol_bots_scores"));
		BotTestAt = Now + 2.0;
		BotTestStep = 7;
		return;
	case 7:
		if (Now < BotTestAt) return;
		TestSay(TEXT("quit"));
		BotTestAt = Now + 3.0;
		BotTestStep = 8;
		return;
	case 8:
		if (Now < BotTestAt) return;
		Summary(TEXT("done"));
		QuitForTest();
		BotTestStep = 9;
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
