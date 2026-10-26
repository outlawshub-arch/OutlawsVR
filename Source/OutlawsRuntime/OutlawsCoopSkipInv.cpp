// BUG-SKIPLEVELINV (user 2026-10-13): the cheat's level skip (Cheats > Skip to level) carries the pockets into the chosen level as a
// won level does, instead of the start kit.  In co-op the host's skip is the session's Jump start and each machine carries its own
// player's pockets (the same StartNextLevelThen).  This is the check, -OLCoopAuto -OLCoopTest=skipinv on a story co-op host and
// its client (hideout): each fills its pockets (every weapon, armour 40), the host skips to the town by the menu's call, and each
// machine's pockets, as one line, are the same in the town.

#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "Engine/World.h"

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TickSkipInvTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	const bool bHost = IsHost();
	const TCHAR* Me = bHost ? TEXT("host") : TEXT("client");
	auto Check = [this, Me](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP skipinv check %s (%s): %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), Me, *What);
		if (bOk) ++SiPasses; else ++SiFails;
	};
	switch (SiStep)
	{
	case 0:
		if (!L->LevelName.Equals(TEXT("hideout"), ESearchCase::IgnoreCase)) return;
		if (bHost)
		{
			const AOutlawsCoopLink* CL = GetLink(PartnerIndex());
			if (!CL || CL->BuiltSerial != StartedSerial || !PartnerPuppet()) return;
		}
		if (SiAt == 0.0) { SiAt = Now + 4.0; return; }
		if (Now < SiAt) return;
		{
			const FString Start = L->CarrySignature();
			L->GiveAllWeapons();
			L->SetArmour(40);
			SiFull = L->CarrySignature();
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP skipinv (%s): start kit %s"), Me, *Start);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP skipinv (%s): full kit  %s"), Me, *SiFull);
			Check(SiFull != Start, TEXT("the cheat gave more than the start kit"));
		}
		SiAt = Now + 4.0;
		SiStep = 1;
		return;
	case 1:
		if (Now < SiAt) return;
		if (bHost)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP skipinv: the host skips to the town by the menu's call"));
			Check(MenuLevelChange(P, EOutlawsCoopStart::Jump, TEXT("town")), TEXT("the host's skip is taken by the session"));
		}
		SiAt = Now + 60.0;
		SiStep = 2;
		return;
	case 2:
		if (!L->LevelName.Equals(TEXT("town"), ESearchCase::IgnoreCase) || L->IsRebuildPending() || !L->HasActorBegunPlay())
		{
			if (Now > SiAt) { Check(false, TEXT("the town was not reached")); SiStep = 3; SiAt = Now; }
			return;
		}
		SiAt = Now + 3.0;
		SiStep = 3;
		return;
	case 3:
		if (Now < SiAt) return;
		if (L->LevelName.Equals(TEXT("town"), ESearchCase::IgnoreCase))
		{
			Check(L->CarrySignature() == SiFull, FString::Printf(TEXT("the pockets in the town: %s"), *L->CarrySignature()));
			Check(L->GetArmour() == 40, FString::Printf(TEXT("armour %d"), L->GetArmour()));
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP skipinv %s RESULT %s (%d passed, %d failed)"), Me, SiFails == 0 && SiPasses > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), SiPasses, SiFails);
		SiAt = Now + (bHost ? 25.0 : 120.0);          // (the host waits for the client's turn, the client for the host's quit)
		SiStep = bHost ? 4 : 20;
		return;
	case 4:
		if (Now < SiAt) return;
		TestSay(TEXT("quit"), 0);
		SiAt = Now + 2.0;
		SiStep = 5;
		return;
	case 5:
		if (Now < SiAt) return;
		Summary(TEXT("done"));
		QuitForTest();
		SiStep = 6;
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
