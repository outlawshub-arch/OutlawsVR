// M1 hardening (tools/olue/PLANS.md "M1 addition: WAN and security"): every message a player sends the host goes through
// AOutlawsCoopLink::Admit first: a hard rate for its kind (a burst beyond it is dropped), and the caller's own bounds (sizes,
// indexes, numbers that are not finite).  Anything outside them is dropped and said in the log (at most once a second for each
// kind and player), never acted on and never a crash.  -OLCoopTest=fuzz: a client sends every kind of message out of bounds,
// oversized and in bursts; the host must drop them, say so, and play on.
#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"

namespace
{
	/** Each kind's most a second (a player's normal play stays well under: poses 20, voice 17, a shotgun's pellets ~10). */
	int32 GateRate(EOLGate K)
	{
		switch (K)
		{
		case EOLGate::Pose: return 60;
		case EOLGate::Stats: return 30;
		case EOLGate::EnemyHit: case EOLGate::WorldBit: return 80;
		case EOLGate::PlayerHit: case EOLGate::Voice: return 40;
		case EOLGate::PickupClaim: case EOLGate::PickupDrop: case EOLGate::Noise: case EOLGate::InfEvent: case EOLGate::ShotFx: return 30;
		case EOLGate::GroundItem: case EOLGate::TestNote: return 20;
		case EOLGate::Ready: case EOLGate::LevelStats: case EOLGate::Blast: case EOLGate::Throw: case EOLGate::EnemyGrab: case EOLGate::EnemyRelease: case EOLGate::BreakFx: return 10;
		case EOLGate::Cheat: return 10;
		case EOLGate::JoinHello: case EOLGate::BuildLine: return 2;
		default: return 5;
		}
	}
}

const TCHAR* AOutlawsCoopLink::GateName(EOLGate K)
{
	static const TCHAR* const Names[] = { TEXT("ready"), TEXT("pose"), TEXT("stats"), TEXT("level built"), TEXT("pickup claim"), TEXT("pickup drop"),
		TEXT("ground item"), TEXT("test note"), TEXT("enemy hit"), TEXT("noise"), TEXT("INF event"), TEXT("movie skip"), TEXT("level end"), TEXT("player hit"),
		TEXT("respawn ask"), TEXT("character"), TEXT("revive"), TEXT("pace"), TEXT("name"), TEXT("level stats"), TEXT("blast"), TEXT("world change"),
		TEXT("shot"), TEXT("throw"), TEXT("player state"), TEXT("enemy grab"), TEXT("enemy release"), TEXT("voice packet"), TEXT("join hello"), TEXT("build line"), TEXT("team"), TEXT("taunt"), TEXT("app pause"), TEXT("cheat ask"),
		TEXT("barrel break") };
	static_assert(UE_ARRAY_COUNT(Names) == int32(EOLGate::Count), "a name for every kind");
	return int32(K) < int32(EOLGate::Count) ? Names[int32(K)] : TEXT("?");
}

bool AOutlawsCoopLink::Admit(EOLGate Kind, bool bValid)
{
	const int32 k = int32(Kind);
	if (k < 0 || k >= int32(EOLGate::Count)) return false;
	const double Now = FPlatformTime::Seconds();
	if (Now - GateAt[k] >= 1.0)
	{
		GateAt[k] = Now;
		GateN[k] = 0;
	}
	const bool bRate = ++GateN[k] <= GateRate(Kind);
	if (bValid && bRate) return true;
	++GateDropped[k];
	++DroppedTotal;
	if (Now - GateLogAt[k] >= 1.0)
	{
		GateLogAt[k] = Now;
		UE_LOG(LogOutlawsCoop, Warning, TEXT("OLCOOP dropped from the %s: %s (%s), %d of that kind so far"), UOutlawsCoop::WhoWord(FMath::Clamp(PlayerIndex, 0, UOutlawsCoop::MaxPlayers - 1)),
			GateName(Kind), !bValid ? TEXT("out of bounds") : *FString::Printf(TEXT("more than %d a second"), GateRate(Kind)), GateDropped[k]);
	}
	return false;
}

// ---- -OLCoopTest=fuzz (two players): the client sends every kind out of bounds, oversized and in bursts --------------------------

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::OnFuzzTestStep(const FString& Step, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	AOutlawsCoopLink* Mine = GetLink(MyIndex());
	if (Step != TEXT("go") || !Mine) return;
	const float NaN = FMath::Sqrt(-1.f);
	const FVector Bad(NaN, 1.0e30, -1.0e30);
	const int32 E = LevelEpoch();
	FRandomStream R(7);
	// (plain ASCII just past each of our caps: the engine itself closes a connection over a reliable message of about 64 KB,
	// or one that queues too many at once; those are its limits, not ours to test)
	auto Junk = [&R](int32 Len) { FString S; S.Reserve(Len); for (int32 i = 0; i < Len; ++i) S.AppendChar(TCHAR(R.RandRange(33, 126))); return S; };
	TArray<uint8> Big;
	Big.SetNumUninitialized(3000);
	for (uint8& B : Big) B = uint8(R.RandRange(0, 255));
	TArray<int32> ManyItems;
	ManyItems.Init(-7, 200);
	int32 Sent = 0;
	// out of bounds, once each
	FOutlawsCoopPose BadPose = Mine->Pose;
	BadPose.Body = Bad;
	BadPose.Seq = Mine->Pose.Seq + 1;
	BadPose.LeftTool = Junk(500);
	Mine->ServerPose(BadPose); ++Sent;
	FOutlawsCoopStats BadStats;
	BadStats.Health = NaN;
	BadStats.Armour = -5;
	Mine->ServerStats(BadStats); ++Sent;
	Mine->ServerLevelBuilt(-3); ++Sent;
	Mine->ServerPickupClaim(-1, E, 255); ++Sent;
	Mine->ServerPickupDrop(INT32_MIN, E, Bad); ++Sent;
	Mine->ServerGroundItem(Junk(500), Bad, -1, E); ++Sent;
	Mine->ServerTestNote(Junk(2000)); ++Sent;
	Mine->ServerEnemyHit(E, INT32_MAX, 1.0e9f, Bad, FVector(NaN), Bad, 255, NaN); ++Sent;
	Mine->ServerNoise(E, Bad, -1.f); ++Sent;
	Mine->ServerInfEvent(E, INT32_MAX, -9, 3, ManyItems); ++Sent;
	Mine->ServerLevelEndDone(-1); ++Sent;
	Mine->ServerPlayerHit(E, 99, 1.0e6f, Bad, Bad, true); ++Sent;
	Mine->ServerMatchCharacter(Junk(100)); ++Sent;
	Mine->ServerRevive(E, 99, 1000); ++Sent;
	Mine->ServerPace(NaN); ++Sent;
	Mine->ServerPlayerName(Junk(2000)); ++Sent;
	Mine->ServerBlast(E, Bad, -5); ++Sent;
	Mine->ServerWorldBit(E, 255, -1, 0, FVector(NaN), NaN); ++Sent;
	Mine->ServerShotFx(Bad, Junk(500)); ++Sent;
	Mine->ServerThrow(E, 255, Bad, FVector(1.0e9), NaN, NaN); ++Sent;
	Mine->ServerPlayer2(Junk(2000));               // (within the cap: junk JSON, which the host's saves must survive) ++Sent;
	Mine->ServerEnemyGrab(E, -1, 9); ++Sent;
	Mine->ServerEnemyRelease(E, -1, Bad, Bad, NaN); ++Sent;
	Mine->ServerVoice(Big); ++Sent;
	Mine->ServerBuildLine(Junk(2000)); ++Sent;
	// in bursts: well past each kind's rate in one go (valid-looking, so only the rate stops them)
	TArray<uint8> Packet;
	Packet.Init(0x55, 60);
	for (int32 i = 0; i < 200; ++i) { Mine->ServerVoice(Packet); ++Sent; }
	for (int32 i = 0; i < 40; ++i) { Mine->ServerNoise(E, P->GetActorLocation(), 100.f); ++Sent; }
	for (int32 i = 0; i < 5; ++i) { Mine->ServerJoinHello(); ++Sent; }
	for (int32 i = 0; i < 10; ++i) { Mine->ServerPace(0.5f); ++Sent; }
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP fuzz: the client sent %d messages out of bounds, oversized or in bursts"), Sent);
	Mine->ServerTestNote(TEXT("fuzz sent"));
}

void UOutlawsCoop::TickHostFuzzTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	AOutlawsCoopLink* CL = GetLink(PartnerIndex());
	switch (TestStep)
	{
	case 0:
		if (!CL || CL->BuiltSerial != StartedSerial) return;
		if (TestAt == 0.0) { TestAt = Now + 3.0; return; }
		if (Now < TestAt) return;
		FuzzPosesBefore = PosesReceived;
		TestSay(TEXT("fz:go"));
		TestStep = 1;
		TestAt = Now + 5.0;
		return;
	case 1:
	{
		if (Now < TestAt || !CL) return;
		// every kind the client sent out of bounds is dropped here; the bursts too; and the game goes on (its poses still come)
		FString Kinds;
		int32 KindsDropped = 0;
		for (int32 k = 0; k < int32(EOLGate::Count); ++k)
		{
			if (CL->GateDropped[k] == 0) continue;
			++KindsDropped;
			Kinds += FString::Printf(TEXT("%s%s %d"), Kinds.IsEmpty() ? TEXT("") : TEXT(", "), AOutlawsCoopLink::GateName(EOLGate(k)), CL->GateDropped[k]);
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP fuzz: the host dropped %d messages of %d kinds: %s"), CL->DroppedTotal, KindsDropped, *Kinds);
		const bool bPoses = PosesReceived > FuzzPosesBefore;
		// (22 or more: the reliable kinds sent bad; voice and shots go unreliable, and the engine drops an oversized one or most of
		// a burst before it reaches the gate)
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP fuzz check %s: %d kinds dropped (22 or more wanted)"), KindsDropped >= 22 ? TEXT("PASS") : TEXT("FAIL"), KindsDropped);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP fuzz check %s: the client is still connected"), CL && !CL->IsActorBeingDestroyed() ? TEXT("PASS") : TEXT("FAIL"));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP fuzz check %s: the client's poses still come (%d since)"), bPoses ? TEXT("PASS") : TEXT("FAIL"), PosesReceived - FuzzPosesBefore);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP fuzz check %s: the host's own player is unharmed (health %.1f, dead %d)"), L && !L->bDead && L->GetHealth() > 0.f ? TEXT("PASS") : TEXT("FAIL"),
			L ? L->GetHealth() : -1.f, L && L->bDead ? 1 : 0);
		TestSay(TEXT("quit"));
		TestStep = 100;
		TestAt = Now + 2.0;
		return;
	}
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
#endif // WITH_OUTLAWS_TESTS
