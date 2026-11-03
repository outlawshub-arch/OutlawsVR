// BUG-QUESTNATIVEMAPS (the Quest, 3 Nov: none of the 19 community maps on the Host Game page; on the PC all of them):
// -OLNativeMapsListProbe (1960), for the headset where no editor runs: any build, Shipping too, as -OLShipSmoke (through
// OLFlags, not under WITH_OUTLAWS_TESTS; nothing runs without the flag).  A second into the run, the native maps as
// FOutlawsAssets::ListNativeMaps finds them (the stat listing, else the folder's MANIFEST.txt; its "OLRT native maps: N maps
// (...) from <root> via <how>" line and the listing's diagnosis come first): a line an RCM "OLSHOT nativelist map PASS|FAIL:
// <folder>: LAB <file> <bytes> bytes (<names> names), RCM <file> "<NAME>" modes <letters>, level <level> in its LAB", then
// "OLSHOT nativelist RESULT PASS|FAIL (N maps from <root> via <how>; P passed, F failed)", and the game quits a second later
// (BUG-EXITCRASH; with -OLShipSmoke it plays on).  A map passes when its LAB opens, its RCM reads with a level and that
// level's .lvt is in the LAB; with -OLNoNativeMaps the RESULT passes with none.

#include "OutlawsMpHost.h"
#include "OutlawsLab.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsShipSmoke.h"
#include "CoreGlobals.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

namespace
{
	int32 GNativeListStep = 0;

	void RunNativeListProbe()
	{
		const FString Root = FOutlawsAssets::NativeMapsRoot();
		FString Via;
		const TArray<FOutlawsAssets::FNativeMap> Maps = FOutlawsAssets::ListNativeMaps(Root, &Via);
		IPlatformFile& PF = FPlatformFileManager::Get().GetPlatformFile();
		int32 Passes = 0, Fails = 0;
		auto Line = [&Passes, &Fails](bool bOk, const FString& What)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT nativelist map %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
			if (bOk) ++Passes; else ++Fails;
		};
		for (const FOutlawsAssets::FNativeMap& Map : Maps)
		{
			const FString Dir = Root / Map.Dir;
			if (Map.Rcms.Num() == 0)
			{
				Line(false, FString::Printf(TEXT("%s: no .rcm (LABs: %s)"), *Map.Dir, Map.Labs.Num() ? *FString::Join(Map.Labs, TEXT(", ")) : TEXT("none")));
				continue;
			}
			for (const FString& Rcm : Map.Rcms)
			{
				// its LAB as ListMaps pairs them: the one of the RCM's name
				const FString Base = FPaths::GetBaseFilename(Rcm);
				const FString* LabName = Map.Labs.FindByPredicate([&Base](const FString& L) { return FPaths::GetBaseFilename(L).Equals(Base, ESearchCase::IgnoreCase); });
				FString Text, Why;
				FOutlawsMpMap M;
				const bool bRcm = FFileHelper::LoadFileToString(Text, *(Dir / Rcm)) && OutlawsMpHost::ParseRcm(Text, M, Why);
				FOutlawsLabFile Lab;
				const bool bLab = LabName && Lab.Open(Dir / *LabName);
				const bool bLevel = bLab && bRcm && Lab.GetIndex().Contains(M.Level + TEXT(".lvt"));
				const FString LabText = !LabName ? FString(TEXT("no LAB of its name")) : FString::Printf(TEXT("LAB %s %lld bytes (%s)"), **LabName, PF.FileSize(*(Dir / *LabName)),
					bLab ? *FString::Printf(TEXT("%d names"), Lab.GetIndex().Num()) : TEXT("does not open"));
				const FString RcmText = bRcm ? FString::Printf(TEXT("RCM %s \"%s\" modes %s, level %s %s"), *Rcm, *M.Name, *M.ModesText(), *M.Level, bLevel ? TEXT("in its LAB") : TEXT("NOT in its LAB"))
					: FString::Printf(TEXT("RCM %s does not read (%s)"), *Rcm, Why.IsEmpty() ? TEXT("not readable") : *Why);
				Line(bLab && bRcm && bLevel, FString::Printf(TEXT("%s: %s, %s"), *Map.Dir, *LabText, *RcmText));
			}
		}
		// (-OLNoNativeMaps: none is the pass)
		const bool bNone = OLFlags::Param(TEXT("OLNoNativeMaps"));
		const bool bPass = bNone ? Root.IsEmpty() && Maps.Num() == 0 : Fails == 0 && Passes > 0;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT nativelist RESULT %s (%d maps from %s via %s; %d passed, %d failed%s)"), bPass ? TEXT("PASS") : TEXT("FAIL"),
			Maps.Num(), Root.IsEmpty() ? TEXT("no native maps' folder") : *Root, Via.IsEmpty() ? TEXT("nothing") : *Via, Passes, Fails, bNone ? TEXT("; -OLNoNativeMaps: none wanted") : TEXT(""));
	}

	bool NativeListTick(float)
	{
		if (GIsEditor) return false;
		if (GNativeListStep == 0)
		{
			GNativeListStep = 1;
			RunNativeListProbe();
			return true;                       // (the quit a second later, from this ticker)
		}
		if (!OutlawsShipSmoke::Active()) FPlatformMisc::RequestExit(false);
		return false;
	}
}

void OutlawsMpHost::StartNativeListProbe()
{
	if (!OLFlags::Param(TEXT("OLNativeMapsListProbe")) || GIsEditor || IsRunningCommandlet()) return;
	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&NativeListTick), 1.0f);
}
