// REQ-NATIVEMAPS (user 3 Nov), the join guard's probe:
//   -OLNativeMaps=<dir> -OLMpHost=3way -OLMpClass=dm -OLMpBots=0 -OLMpStartWhen=0 -OLJoinGuardProbe
// On a launched host whose match is on a native map, PreLogin's check (CheckJoinBuildAndMap) is given the options joiners
// would send: this machine's own (let in), a joiner without the native maps (-OLNoNativeMaps: turned away, the reason naming
// the map), another copy of the map (turned away), another build with the maps and a build with no build on its URL but the
// maps (let in: the user 3 Nov, builds may differ), one with neither (turned away for the map); then the host's level set to
// a stock map: the joiner without the maps let in, on this build or another.  The joiner's own check (ClientCannotBuild):
// the host's native level builds here, a level nowhere does not, another copy of the host's LAB does not.  One line a case
// "OLSHOT joinguard case PASS|FAIL: ...", then "OLSHOT joinguard RESULT".  -OLOldJoinGuard: every refusal case FAILs (the old
// behaviour: anyone with the password let in).  The two-machine joins themselves: out\mapfull\joinguard.ps1 (the lane's).

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsCoop.h"
#include "OutlawsMpHost.h"
#include "OutlawsLab.h"
#include "OutlawsFlags.h"
#include "OutlawsBuildInfo.h"
#include "OutlawsShotTests.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformTime.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsJoinGuardProbeLocal
{
	int32 Stage = 0, Passes = 0, Fails = 0;
	double StageAt = 0.0;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT joinguard case %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}
}

OL_SHOT_MATCH(OLJoinGuardProbe) { return OLFlags::Param(TEXT("OLJoinGuardProbe")); }

OL_SHOT_TEST(OLJoinGuardProbe, 1780)
{
	using namespace OutlawsJoinGuardProbeLocal;
	if (!L || ShotTime < 0.f || Stage < 0) return;
	const double Now = FPlatformTime::Seconds();
	L->StartSaveDelay = -1.f;           // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	UOutlawsCoop* C = UOutlawsCoop::Get(this);
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT joinguard RESULT %s (%d passed, %d failed%s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			UOutlawsCoop::OldJoinGuard() ? TEXT(", -OLOldJoinGuard") : TEXT(""));
		ShotTime = -1.f;
		Stage = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	if (Stage == 0)
	{
		StageAt = Now;
		Stage = 1;
		return;
	}
	AOutlawsCoopSession* S = C ? C->GetSession() : nullptr;
	const FOutlawsAssets* A = L->GetLabAssets();
	if (!C || !S || !A || !C->IsHost() || !C->IsMatch() || C->LevelToStart.IsEmpty() || !C->bLevelBuilt)
	{
		if (Now - StageAt < 120.0) return;
		Check(false, TEXT("needs a launched host on a native map: -OLMpHost=3way -OLMpClass=dm -OLMpBots=0 -OLMpStartWhen=0"));
		Finish();
		return;
	}
	const FString Lv = C->LevelToStart;
	const FString Stamp = A->NativeStampFor(Lv);
	const FString Title = OutlawsMpHost::MapTitle(C->MatchOptions.MapFile);
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT joinguard: the host's level %s (%s), its LAB's stamp %s; this build %s; the session's native list %d chars"),
		*Lv, *Title, Stamp.IsEmpty() ? TEXT("none") : *Stamp, OutlawsBuildInfo::Commit(), S->NativeStamps.Len());
	if (Stamp.IsEmpty())
	{
		Check(false, FString::Printf(TEXT("the host's level %s is not a native map's (launch with -OLMpHost=3way and -OLNativeMaps)"), *Lv));
		Finish();
		return;
	}
	const FString Build = FString::Printf(TEXT("?OLBuild=%s"), OutlawsBuildInfo::Commit());
	const FString Own = UOutlawsCoop::JoinGuardOptions();
	auto Ask = [&](const FString& Options) { return C->CheckJoinBuildAndMap(Options); };
	// (1) this machine's own options: let in
	{
		const FString Why = Ask(Own);
		Check(Why.IsEmpty() && Own.Contains(Lv + TEXT(".") + Stamp), FString::Printf(TEXT("a joiner with the maps (this machine's options, %d chars) is let in%s"), Own.Len(),
			Why.IsEmpty() ? TEXT("") : *(TEXT(": ") + Why)));
	}
	// (2) a joiner without the native maps (-OLNoNativeMaps: an empty list)
	{
		const FString Why = Ask(Build + TEXT("?OLMaps="));
		Check(!Why.IsEmpty() && Why.Contains(Title), FString::Printf(TEXT("a joiner without the native maps is turned away, saying which map: \"%s\""), *Why));
	}
	// (3) another copy of the map: the same level, another stamp
	{
		const FString Other = Own.Replace(*(Lv + TEXT(".") + Stamp), *(Lv + TEXT(".") + (Stamp == TEXT("00000000") ? TEXT("00000001") : TEXT("00000000"))));
		const FString Why = Ask(Other);
		Check(!Why.IsEmpty() && Why.Contains(TEXT("not the host's")), FString::Printf(TEXT("a joiner with another copy of %s is turned away: \"%s\""), *Lv, *Why));
	}
	// (4) another build with the maps, (5) no build on its URL (from before the guard): let in on the build (the user 3 Nov),
	// the map decides
	{
		const FString Why = Ask(Own.Replace(*Build, TEXT("?OLBuild=0000000")));
		Check(Why.IsEmpty(), FString::Printf(TEXT("another build with the maps is let in%s"), Why.IsEmpty() ? TEXT("") : *(TEXT(": ") + Why)));
		const FString Why2 = Ask(Own.Replace(*Build, TEXT("")));
		Check(Why2.IsEmpty(), FString::Printf(TEXT("no OLBuild on its URL but the maps: let in%s"), Why2.IsEmpty() ? TEXT("") : *(TEXT(": ") + Why2)));
		const FString Why3 = Ask(TEXT("?Name=Old"));
		Check(!Why3.IsEmpty() && Why3.Contains(Title), FString::Printf(TEXT("no OLBuild and no maps on its URL: turned away for the map, not the build: \"%s\""), *Why3));
	}
	// (6) the host's game on a stock map: the joiner without the maps let in, as before, on this build or another
	{
		TGuardValue<FString> KeepLevel(C->LevelToStart, TEXT("drygulch"));
		TGuardValue<FString> KeepFile(C->MatchOptions.MapFile, TEXT("drygulch.rcm"));
		const FString Why = Ask(Build + TEXT("?OLMaps="));
		Check(Why.IsEmpty(), FString::Printf(TEXT("a stock-map game lets the joiner without the native maps in%s"), Why.IsEmpty() ? TEXT("") : *(TEXT(": ") + Why)));
		const FString Why2 = Ask(TEXT("?OLBuild=0000000?OLMaps="));
		Check(Why2.IsEmpty(), FString::Printf(TEXT("a stock-map game lets another build without the native maps in%s"), Why2.IsEmpty() ? TEXT("") : *(TEXT(": ") + Why2)));
	}
	// (7) the joiner's own check before it builds the host's level
	{
		const FString Here = C->ClientCannotBuild(Lv);
		Check(Here.IsEmpty(), FString::Printf(TEXT("the joiner's check: the host's %s builds here%s"), *Lv, Here.IsEmpty() ? TEXT("") : *(TEXT(": ") + Here)));
		const FString Nowhere = C->ClientCannotBuild(TEXT("nosuchmap"));
		Check(!Nowhere.IsEmpty(), FString::Printf(TEXT("the joiner's check: a level in no LAB is refused: \"%s\""), *Nowhere));
		const FString Keep = S->NativeStamps;
		S->NativeStamps = Keep.Replace(*(Lv + TEXT(".") + Stamp), *(Lv + TEXT(".00000000")));
		const FString Copy = C->ClientCannotBuild(Lv);
		S->NativeStamps = Keep;
		Check(!Copy.IsEmpty(), FString::Printf(TEXT("the joiner's check: another copy than the host's is refused: \"%s\""), *Copy));
		const FString Stock = C->ClientCannotBuild(TEXT("drygulch"));
		Check(Stock.IsEmpty(), FString::Printf(TEXT("the joiner's check: a stock level builds%s"), Stock.IsEmpty() ? TEXT("") : *(TEXT(": ") + Stock)));
	}
	Finish();
}

#endif // WITH_OUTLAWS_TESTS
