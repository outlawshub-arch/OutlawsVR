// BUG-REMOTEGUNSOUND and BUG-TNTDEATHFREEZE, the two-machine test (-OLRemoteFxTest on a host and its client; the host with
// -OLMpHost=<map> -OLMpBots=1 -OLMpChar=Mar -OLMpBotChars=San, the client with -OLMpChar=San; the bots hold their fire).  The host starts
// the match, then:
//   1. the client (Spit'n Jack) fires its revolver: the host must be told, and play, his sound (BADGUN), not James's (pistol2);
//   2. the host (Bloody Mary) fires: the client must be told her sound (pistol3);
//   3. the host's bot (Spit'n Jack) fires through BotFired: the client must be told the bot's sound, not the default table's;
//   4. the client throws a lit stick over the host, dies with it in the air, and the stick must go off at its fuse and be dealt to
//      the host through the match, the dead thrower credited ("blew up", ServerPlayerHit bBlast).
//   (before 4, BUG-REMOTETNT) the host throws a lit stick and a knife, then the client does: each must be drawn on the other
//      machine with the thrower's own look, no stand-in shape (-OLRemoteFlyerShots: each held before the eyes and shot).
// Every machine prints "OLSHOT rf check PASS|FAIL: ..." and "OLSHOT rf <host|client> RESULT".
#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsEnemy.h"
#include "OutlawsMpHost.h"
#include "OutlawsFormats.h"
#include "OutlawsLab.h"
#include "OutlawsRemotePlayer.h"
#include "Engine/World.h"

#if WITH_OUTLAWS_TESTS

bool UOutlawsCoop::RemoteFxCheck(bool bOk, const FString& What)
{
	UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT rf check %s: %s [%s]"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What, IsHost() ? TEXT("host") : WhoWord(MyIndex()));
	if (!bOk) ++RfFails;
	return bOk;
}

FString UOutlawsCoop::RemoteFxExpectedPistol(const FString& Short) const
{
	const AOutlawsLevelActor* L = FindLevel();
	const FOutlawsAssets* A = L ? L->GetLabAssets() : nullptr;
	if (Short.IsEmpty() || Short.Equals(TEXT("Jam")) || !A) return TEXT("pistol2");
	const FOutlawsCharKit& Kit = FOutlawsCharKit::Of(A, Short);
	const FString Own = Kit.WeaponVariant(A, TEXT("ipistol"));
	FString Text, Func;
	TMap<FString, FString> D;
	if (Own == TEXT("ipistol") || !A->GetText(Own + TEXT(".itm"), Text)) return TEXT("pistol2");
	OutlawsFormats::ParseItm(Text, Func, D);
	const FString Snd = D.FindRef(TEXT("FIRE_SOUND_1")).ToLower();
	return Snd.IsEmpty() || Snd == TEXT("null") ? FString(TEXT("pistol2")) : Snd;
}

void UOutlawsCoop::TickHostRemoteFxTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	FOutlawsMatchFlow& F = OutlawsMpHost::Flow();
	TArray<AOutlawsCoopLink*> Links;
	GetLinks(Links);
	if (TestStep < 1000) { TestStep = 1000; TestAt = 0.0; }
	auto NameOf = [this](int32 Who) { const FOutlawsDmPlayer* D = Dm.Find(Who); return D ? D->Name : FString(TEXT("?")); };
	auto CharOf = [this](int32 Who) { const FOutlawsDmPlayer* D = Dm.Find(Who); return D ? D->Character : FString(); };
	auto Alive = [this](int32 Who) { return Who >= 0 && MatchInRules[Who] && !MatchDead[Who]; };
	switch (TestStep)
	{
	case 1000:
		for (const AOutlawsCoopLink* K : Links)
		{
			const int32 Who = K->PlayerIndex;
			if (Who <= 0 || Who >= MaxPlayers || !MatchInRules[Who]) continue;
			if (K->bBot && RfBot < 0) RfBot = Who;
			if (!K->bBot && RfClient < 0) RfClient = Who;
		}
		if (RfBot < 0 || RfClient < 0 || !bMatchSetUp) return;
		if (TestAt == 0.0) { TestAt = Now + 6.0; return; }
		if (Now < TestAt) return;
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT rf: the host %s (%s), the client %s (%s, %s), the bot %s (%s): Start"), *NameOf(0), *CharOf(0), *NameOf(RfClient), WhoWord(RfClient), *CharOf(RfClient),
			*NameOf(RfBot), *CharOf(RfBot));
		F.HostStart();
		TestStep = 1001;
		return;
	case 1001:
		if (F.GetState() != FOutlawsMatchFlow::EState::Playing) return;
		TestStep = 1010;
		TestAt = Now + 6.0;                      // (past the spawn protection and the kit's draw)
		return;

	// 1. the client fires: the sound this host is told is the client's character's
	case 1010:
		if (Now < TestAt) return;
		if (!Alive(RfClient) || !Alive(0)) { TestAt = Now + 0.5; return; }
		RfHeard.Remove(RfClient);
		TestSayTo(RfClient, TEXT("rf:fire"), 0);
		TestStep = 1011;
		TestAt = Now + 2.0;
		return;
	case 1011:
	{
		if (Now < TestAt) return;
		const FString Want = RemoteFxExpectedPistol(CharOf(RfClient));
		const FString Got = RfHeard.FindRef(RfClient);
		RemoteFxCheck(!Want.Equals(TEXT("pistol2")), FString::Printf(TEXT("the client's character (%s) has a revolver of its own (%s)"), *CharOf(RfClient), *Want));
		RemoteFxCheck(!Got.IsEmpty() && Got.Equals(Want, ESearchCase::IgnoreCase), FString::Printf(TEXT("the host is told the client's shot as '%s' (want '%s')"), *Got, *Want));
		{
			// (REQ-MUZZLEFLASH3D) its 3D flash shown here, the pistol's (none with -OLNoFlash3D)
			const bool bOn = L->HasFlash3D(0);
			const FString Fl = RfFlash3D.FindRef(RfClient);
			RemoteFxCheck(bOn ? Fl == TEXT("pistol") : Fl.IsEmpty(), FString::Printf(TEXT("the client's shot showed %s here (shown: '%s'; %s)"), bOn ? TEXT("a pistol's 3D flash") : TEXT("no 3D flash"), *Fl, *L->Flash3DReport));
		}
		TestStep = 1020;
		return;
	}

	// 2. the host fires: the sound the client is told is the host's character's
	case 1020:
	{
		const FString Want = RemoteFxExpectedPistol(CharOf(0));
		const FString Fired = P->TestFireRevolver();
		RemoteFxCheck(!Want.Equals(TEXT("pistol2")) && Fired.Equals(Want, ESearchCase::IgnoreCase), FString::Printf(TEXT("the host (%s) fires '%s' (want '%s', not James's)"), *CharOf(0), *Fired, *Want));
		TestSayTo(RfClient, FString::Printf(TEXT("rf:expect:0:%s"), *Want), 0);
		TestStep = 1030;
		TestAt = Now + 2.5;
		return;
	}

	// 3. the host's bot fires: the sound the client is told is the bot's character's
	case 1030:
	{
		if (Now < TestAt) return;
		FBot* B = FindBot(RfBot);
		AOutlawsEnemy* E = B ? B->Brain.Get() : nullptr;
		if (!B || !E || B->bDead || !MatchInRules[RfBot]) { TestAt = Now + 0.5; return; }
		B->Weapon = 0;
		BotApplyWeapon(*B);
		const FString Want = RemoteFxExpectedPistol(B->Short);
		const FString Snd = E->Def.ShootSound;
		RemoteFxCheck(!Want.Equals(TEXT("pistol2")) && Snd.Equals(Want, ESearchCase::IgnoreCase), FString::Printf(TEXT("the bot (%s) holds the sound '%s' (want '%s')"), *B->Short, *Snd, *Want));
		RfHeard.Remove(RfBot);
		RfFlash3D.Remove(RfBot);
		BotFired(E, E->GetActorLocation() + FVector(0.0, 0.0, 60.0));
		{
			// (REQ-MUZZLEFLASH3D) the bot's 3D flash on the host too
			const bool bOn = L->HasFlash3D(0);
			const FString Fl = RfFlash3D.FindRef(RfBot);
			RemoteFxCheck(bOn ? Fl == TEXT("pistol") : Fl.IsEmpty(), FString::Printf(TEXT("the bot's shot showed %s on the host (shown: '%s')"), bOn ? TEXT("a pistol's 3D flash") : TEXT("no 3D flash"), *Fl));
		}
		TestSayTo(RfClient, FString::Printf(TEXT("rf:expect:%d:%s"), RfBot, *Want), 0);
		TestStep = 1032;
		TestAt = Now + 2.5;
		return;
	}

	// 3b. (BUG-REMOTETNT) a lit stick and a knife each way: the other machine draws them with the thrower's own look
	case 1032:
		if (Now < TestAt) return;
		RemoteFxCheck(P->TestThrowTnt(P->GetActorLocation() + FVector(0.0, 0.0, 300.0), FVector::ZeroVector, 30.f), TEXT("the host threw a lit stick"));
		TestStep = 1033;
		TestAt = Now + 2.5;
		return;
	case 1033:
		if (Now < TestAt) return;
		TestSayTo(RfClient, TEXT("rf:look:1"), 0);
		P->TestClearThrown();
		L->GiveAllWeapons();
		P->CoopTestThrow(7);
		TestStep = 1034;
		TestAt = Now + 2.5;
		return;
	case 1034:
		if (Now < TestAt) return;
		TestSayTo(RfClient, TEXT("rf:look:2"), 0);
		TestSayTo(RfClient, TEXT("rf:tntup"), 0);
		TestStep = 1035;
		TestAt = Now + 2.5;
		return;
	case 1035:
	{
		if (Now < TestAt) return;
		const AOutlawsRemotePlayer* R = PuppetOf(RfClient);
		RemoteFxCheck(R && R->LooksShown >= 1 && R->StandInsShown == 0, FString::Printf(TEXT("the client's lit stick drawn here with its own look (%d looks, %d stand-ins)"),
			R ? R->LooksShown : -1, R ? R->StandInsShown : -1));
		TestSayTo(RfClient, TEXT("rf:clear"), 0);
		TestSayTo(RfClient, TEXT("rf:knife"), 0);
		TestStep = 1036;
		TestAt = Now + 2.5;
		return;
	}
	case 1036:
	{
		if (Now < TestAt) return;
		const AOutlawsRemotePlayer* R = PuppetOf(RfClient);
		RemoteFxCheck(R && R->LooksShown >= 2 && R->StandInsShown == 0, FString::Printf(TEXT("the client's knife drawn here with its own look (%d looks, %d stand-ins)"),
			R ? R->LooksShown : -1, R ? R->StandInsShown : -1));
		TestStep = 1040;
		TestAt = Now + 1.0;
		return;
	}

	// 4. the client's stick over the host, the client dying with it in the air
	case 1040:
		if (Now < TestAt) return;
		if (!Alive(RfClient) || !Alive(0) || L->bDead) { TestAt = Now + 0.5; return; }
		L->DamagePlayer(1.f, 1.f, true);         // (a point off the host: the stick's 9.6 then kills it, for the kill line)
		RfHits0 = MatchHitsDealt;
		RfBlasts0 = L->BlastsSeen;
		{
			const FVector At = P->GetActorLocation();
			TestSayTo(RfClient, FString::Printf(TEXT("rf:tnt:%.0f,%.0f,%.0f"), At.X, At.Y, At.Z), 0);
		}
		TestStep = 1041;
		TestAt = Now + 0.5;
		return;
	case 1041:
		if (Now < TestAt) return;
		TestSayTo(RfClient, TEXT("rf:die"), 0);
		TestStep = 1042;
		TestAt = Now + 0.8;
		return;
	case 1042:
		if (Now < TestAt) return;
		RemoteFxCheck(MatchDead[RfClient], TEXT("the client is dead while its stick is still in the air"));
		TestStep = 1043;
		TestAt = Now + 4.0;                      // (the fuse is 3 s from the throw; 1.3 s gone)
		return;
	case 1043:
	{
		if (Now < TestAt) return;
		RemoteFxCheck(L->BlastsSeen > RfBlasts0, FString::Printf(TEXT("the dead client's stick went off here (%d blasts)"), L->BlastsSeen - RfBlasts0));
		RemoteFxCheck(MatchHitsDealt > RfHits0, FString::Printf(TEXT("its blast was dealt to the host through the match, the dead thrower credited (%d hits)"), MatchHitsDealt - RfHits0));
		if (MatchDead[0])
		{
			const FString Line = FString::Printf(TEXT("%s blew you up!"), *NameOf(RfClient));
			RemoteFxCheck(KillLinesSeen.Contains(Line), FString::Printf(TEXT("the host died of it and the feed says '%s' (shown: %s)"), *Line, *FString::Join(KillLinesSeen, TEXT(" | "))));
		}
		else
		{
			UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT rf: the host survived the blast (health %.1f): no kill line to check"), L->GetHealth());
		}
		TestSayTo(RfClient, TEXT("rf:report"), 0);
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT rf host RESULT %s (%d failed)"), RfFails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), RfFails);
		TestStep = 1050;
		TestAt = Now + 3.0;
		return;
	}
	case 1050:
		if (Now < TestAt) return;
		TestSay(TEXT("quit"), 0);
		TestStep = 1051;
		TestAt = Now + 3.0;
		return;
	case 1051:
		if (Now < TestAt) return;
		Summary(TEXT("done"));
		QuitForTest();
		TestStep = 1052;
		return;
	default:
		return;
	}
}

void UOutlawsCoop::OnRemoteFxTestStep(const FString& Step, int32 Arg, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	if (Step == TEXT("fire"))
	{
		const FString Fired = P->TestFireRevolver();
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT rf: the client (%s) fired '%s'"), *L->WornCharacter, *Fired);
	}
	else if (Step.StartsWith(TEXT("expect:")))
	{
		// expect:<who>:<sound>
		FString WhoText, Want;
		Step.Mid(7).Split(TEXT(":"), &WhoText, &Want);
		const int32 Who = FCString::Atoi(*WhoText);
		const FString Got = RfHeard.FindRef(Who);
		RemoteFxCheck(!Got.IsEmpty() && Got.Equals(Want, ESearchCase::IgnoreCase), FString::Printf(TEXT("the client is told the %s's shot as '%s' (want '%s')"), WhoWord(Who), *Got, *Want));
		const bool bOn = L->HasFlash3D(0);
		const FString Fl = RfFlash3D.FindRef(Who);
		RemoteFxCheck(bOn ? Fl == TEXT("pistol") : Fl.IsEmpty(), FString::Printf(TEXT("the %s's shot showed %s on the client (shown: '%s'; %s)"), WhoWord(Who),
			bOn ? TEXT("a pistol's 3D flash") : TEXT("no 3D flash"), *Fl, *L->Flash3DReport));
	}
	else if (Step.StartsWith(TEXT("tnt:")))
	{
		// tnt:x,y,z: a lit stick thrown 1.5 m over that spot, a three second fuse
		TArray<FString> V;
		Step.Mid(4).ParseIntoArray(V, TEXT(","));
		const FVector At = V.Num() == 3 ? FVector(FCString::Atod(*V[0]), FCString::Atod(*V[1]), FCString::Atod(*V[2]) + 150.0) : P->GetActorLocation();
		const bool bOk = P->TestThrowTnt(At, FVector(0.0, 0.0, 100.0), 3.0f);
		RemoteFxCheck(bOk, FString::Printf(TEXT("the client threw a lit stick over %s"), *At.ToCompactString()));
	}
	else if (Step.StartsWith(TEXT("look:")))
	{
		// look:<n>: the host's sticks and knives drawn here so far, n of them, all with the host's own look (BUG-REMOTETNT)
		const int32 Want = FCString::Atoi(*Step.Mid(5));
		const AOutlawsRemotePlayer* R = PuppetOf(0);
		RemoteFxCheck(R && R->LooksShown >= Want && R->StandInsShown == 0, FString::Printf(TEXT("the host's %s drawn here with its own look (%d looks, %d stand-ins)"),
			Want == 1 ? TEXT("lit stick") : TEXT("knife"), R ? R->LooksShown : -1, R ? R->StandInsShown : -1));
	}
	else if (Step == TEXT("tntup"))
	{
		RemoteFxCheck(P->TestThrowTnt(P->GetActorLocation() + FVector(0.0, 0.0, 300.0), FVector::ZeroVector, 30.f), TEXT("the client threw a lit stick"));
	}
	else if (Step == TEXT("clear"))
	{
		P->TestClearThrown();
	}
	else if (Step == TEXT("knife"))
	{
		L->GiveAllWeapons();
		P->CoopTestThrow(7);
	}
	else if (Step == TEXT("die"))
	{
		FVector Pos;
		bool bRest = false;
		const bool bAir = P->TestTntState(Pos, bRest);
		if (!L->bDead) L->DamagePlayer(1e6f, 1.f, true);
		RemoteFxCheck(L->bDead && bAir && !bRest, TEXT("the client died with its stick in the air"));
	}
	else if (Step == TEXT("report"))
	{
		FVector Pos;
		bool bRest = false;
		RemoteFxCheck(L->BlastsSeen > 0 && !P->TestTntState(Pos, bRest), FString::Printf(TEXT("the dead client's own stick went off (%d blasts) and none is left in flight"), L->BlastsSeen));
		UE_LOG(LogOutlawsCoop, Display, TEXT("OLSHOT rf client RESULT %s (%d failed)"), RfFails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), RfFails);
	}
}
#endif // WITH_OUTLAWS_TESTS
