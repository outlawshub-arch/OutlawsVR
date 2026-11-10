// -OLShots -OLHQSoundProbe (REQ-HQSOUND, the user 8 and 9 Nov: the HQ Sound Pack by T4H_Dingle and Dxierzan for every player,
// a release asset decoded once into the media cache; a Sound row HQ Sound Pack, On by default; -OLOldSoundPack: today's sounds).
// The run names the pack (-OLHQSoundPackFile=<zip>, in the run's sandbox) and the media folder (-OLTestMediaDir) and says what
// it expects: -OLHQSoundExpect=on|off|fail, and with on -OLHQSoundExpectDecoded=<n> (the first start decodes every sound, the
// second none).
//  - on: the pack ready, every sound it lists resolves to its decoded wav with Remastered Sounds on and off (the names it
//    shares with olsfxhd/oltaunthd and the others, counted), GetSound gives the wav's bytes, the level reads its length from
//    it, the row Off gives today's resolution exactly and On the pack again; the decode count as expected.
//  - off (the row Off, or -OLOldSoundPack) and fail (a corrupt or missing pack): nothing decoded, the pack not used, every
//    name it would hold resolves and reads exactly as with no pack, the game still running.
// Both: the row's settings key and the credits line.  "OLSHOT hqsound check <what>: PASS|FAIL", "OLSHOT hqsound RESULT <n>
// PASS, <m> FAIL".  Runs muted (-nosound): nothing is listened to.

#include "ShotTestCommon.h"
#include "OutlawsHQSound.h"
#include "OutlawsUpdate.h"

#if WITH_OUTLAWS_TESTS

namespace
{
	// the sounds the pack's zip lists ("name.wav", lower case), read from the zip itself (when the pack is off or broken
	// the game holds none)
	TArray<FString> PackNamesFromZip(const FString& Zip)
	{
		TArray<FString> Out;
		TArray<OLUpdate::FZipEntry> Index;
		if (!OLUpdate::ReadZipIndex(Zip, Index)) return Out;
		for (const OLUpdate::FZipEntry& E : Index) if (E.Name.EndsWith(TEXT(".ogg"))) Out.Add(FPaths::GetBaseFilename(E.Name).ToLower() + TEXT(".wav"));
		Out.Sort();
		return Out;
	}

	double RiffSeconds(const TArray<uint8>& D)
	{
		if (D.Num() < 44 || FMemory::Memcmp(D.GetData(), "RIFF", 4) != 0) return -1.0;
		uint16 Ch = 0;
		uint32 Rate = 0, Bytes = 0;
		FMemory::Memcpy(&Ch, D.GetData() + 22, 2);
		FMemory::Memcpy(&Rate, D.GetData() + 24, 4);
		FMemory::Memcpy(&Bytes, D.GetData() + 40, 4);
		return Ch && Rate ? double(Bytes) / double(2 * Ch * Rate) : -1.0;
	}
}

OL_SHOT_MATCH(OLHQSoundProbe) { return OLFlags::Param(TEXT("OLHQSoundProbe")); }

OL_SHOT_TEST(OLHQSoundProbe, 2640)
{
	static int32 Step = 0, Pass = 0, Fail = 0;
	static double Since = 0.0;
	if (!L) return;
	const double Now = FPlatformTime::Seconds();
	auto Check = [](bool bOk, const FString& What)
	{
		(bOk ? Pass : Fail)++;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hqsound check %s: %s"), *What, bOk ? TEXT("PASS") : TEXT("FAIL"));
	};
	if (Step == 0)
	{
		if (ShotTime < 3.f) return;
		Step = 1;
		Since = Now;
	}
	if (Step == 1)
	{
		// the decode runs on a worker: wait for it (two minutes at most)
		if (OLHQSound::Busy() && Now - Since < 120.0) return;
		Step = 2;
		FString Expect = TEXT("on");
		OLFlags::Value(TEXT("OLHQSoundExpect"), Expect);
		int32 WantDecoded = -1;
		OLFlags::Value(TEXT("OLHQSoundExpectDecoded"), WantDecoded);
		FString Zip;
		OLFlags::Value(TEXT("OLHQSoundPackFile"), Zip, /*bShouldStopOnSeparator*/ false);
		Zip = FPaths::ConvertRelativePathToFull(Zip.TrimQuotes());
		const FOutlawsAssets* A = L->GetLabAssets();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hqsound start: expect %s, state %s, pack %s, cache %s, decoded this run %d in %.2f s, old flag %d, row %s"),
			*Expect, *OLHQSound::State(), *OLHQSound::PackPath(), *OLHQSound::CacheDir(), OLHQSound::DecodedThisRun(), OLHQSound::DecodeSeconds(),
			OLHQSound::OldFlag() ? 1 : 0, OLHQSound::Chosen() ? TEXT("On") : TEXT("Off"));
		if (!A)
		{
			Check(false, TEXT("the install's LABs are open"));
		}
		else
		{
			const TArray<FString> Zipped = PackNamesFromZip(Zip);
			// (the corrupt pack is still a readable zip; the missing one lists nothing: the stock order is checked on the
			// names a good pack holds, which every run of the check is given beside it as -OLHQSoundNames=<zip>)
			FString NamesZip;
			OLFlags::Value(TEXT("OLHQSoundNames"), NamesZip, false);
			const TArray<FString> Names = Zipped.Num() > 0 ? Zipped : PackNamesFromZip(FPaths::ConvertRelativePathToFull(NamesZip.TrimQuotes()));
			Check(Names.Num() > 0, FString::Printf(TEXT("the pack's names are known (%d)"), Names.Num()));
			auto IsHdSet = [](const FString& Where) { return Where.Equals(TEXT("olsfxhd.lab"), ESearchCase::IgnoreCase) || Where.Equals(TEXT("oltaunthd.lab"), ESearchCase::IgnoreCase); };
			// today's resolution, the pack aside: equal to the game's own when the pack is not in use
			auto SameAsToday = [&](const TCHAR* Why)
			{
				int32 Diff = 0, Read = 0;
				FString First;
				for (const FString& N : Names)
				{
					for (const bool bHd : { true, false })
					{
						TArray<uint8> Game, Stock;
						const bool bG = A->GetSound(N, bHd, Game), bS = A->GetSoundStock(N, bHd, Stock);
						Read += bG ? 1 : 0;
						if (A->WhereSound(N, bHd) != A->WhereSoundStock(N, bHd) || bG != bS || Game != Stock)
						{
							if (Diff++ == 0) First = FString::Printf(TEXT("%s (hd %d: %s, stock %s)"), *N, bHd ? 1 : 0, *A->WhereSound(N, bHd), *A->WhereSoundStock(N, bHd));
						}
					}
				}
				Check(Diff == 0, FString::Printf(TEXT("%s: all %d names resolve and read as today, Remastered Sounds on and off (%d read, %d differ%s%s)"),
					Why, Names.Num(), Read, Diff, Diff ? TEXT(", first ") : TEXT(""), *First));
			};
			if (Expect == TEXT("on"))
			{
				Check(OLHQSound::Active() && OLHQSound::State() == TEXT("ready"), TEXT("the pack is ready and on (") + OLHQSound::State() + TEXT(")"));
				Check(OLHQSound::Names() == Names, FString::Printf(TEXT("the game holds the pack's %d sounds (%d)"), Names.Num(), OLHQSound::Names().Num()));
				if (WantDecoded >= 0)
				{
					Check(OLHQSound::DecodedThisRun() == WantDecoded, FString::Printf(TEXT("decoded this run: %d (want %d; %s)"), OLHQSound::DecodedThisRun(), WantDecoded,
						WantDecoded > 0 ? TEXT("the first start decodes each once") : TEXT("a later start decodes nothing")));
				}
				if (OLHQSound::DecodedThisRun() > 0) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hqsound decode: %d sounds in %.2f s"), OLHQSound::DecodedThisRun(), OLHQSound::DecodeSeconds());
				int32 Shared = 0, Others = 0, Wrong = 0, BadWav = 0, BadLen = 0, New = 0;
				FString First;
				for (const FString& N : Names)
				{
					const FString StockHd = A->WhereSoundStock(N, true);
					if (IsHdSet(StockHd)) ++Shared; else ++Others;
					if (StockHd.IsEmpty() && A->WhereSoundStock(N, false).IsEmpty()) ++New;
					const FString Wav = OLHQSound::WavFor(N);
					TArray<uint8> File, Hd, Classic;
					FFileHelper::LoadFileToArray(File, *Wav);
					const bool bHd = A->GetSound(N, true, Hd), bClassic = A->GetSound(N, false, Classic);
					if (A->WhereSound(N, true) != TEXT("HQSoundPack.zip") || A->WhereSound(N, false) != TEXT("HQSoundPack.zip") || !bHd || !bClassic || Hd != File || Classic != File)
					{
						if (Wrong++ == 0) First = FString::Printf(TEXT("%s: %s / %s"), *N, *A->WhereSound(N, true), *A->WhereSound(N, false));
					}
					const double Sec = RiffSeconds(File);
					if (Sec <= 0.0) ++BadWav;
					else if (FMath::Abs(L->WavSeconds(FPaths::GetBaseFilename(N)) - Sec) > 0.002) ++BadLen;
				}
				Check(Wrong == 0, FString::Printf(TEXT("every pack name resolves to its decoded wav, Remastered Sounds on and off (%d wrong%s%s)"), Wrong, Wrong ? TEXT(", first ") : TEXT(""), *First));
				Check(Shared == 43 && Others == 194, FString::Printf(TEXT("the pack wins the %d names it shares with olsfxhd/oltaunthd and supplies its %d others (want 43 and 194; %d new to the game)"), Shared, Others, New));
				Check(BadWav == 0 && BadLen == 0, FString::Printf(TEXT("each decoded wav is a 16-bit RIFF and the level reads its length from it (%d bad, %d lengths off)"), BadWav, BadLen));
				// the row: Off is today's resolution exactly, On the pack again; the cache's serial moves on each time
				const uint32 S0 = OLHQSound::Serial();
				OLHQSound::SetChosen(false);
				const bool bMoved = OLHQSound::Serial() != S0;
				SameAsToday(TEXT("the row Off"));
				OLHQSound::SetChosen(true);
				Check(bMoved && OLHQSound::Active() && A->WhereSound(Names[0], true) == TEXT("HQSoundPack.zip"), TEXT("the row Off moves the serial, On gives the pack again"));
			}
			else
			{
				Check(!OLHQSound::Active() && OLHQSound::WavFor(Names[0]).IsEmpty(), TEXT("the pack is not in use (") + OLHQSound::State() + TEXT(")"));
				Check(OLHQSound::DecodedThisRun() == 0, FString::Printf(TEXT("nothing decoded this run (%d)"), OLHQSound::DecodedThisRun()));
				if (Expect == TEXT("fail")) Check(!FPaths::FileExists(FPaths::Combine(OLHQSound::CacheDir(), TEXT("stamp.txt"))), TEXT("no stamp in the cache: the next start tries again"));
				if (Expect == TEXT("fail")) Check(OLHQSound::State().StartsWith(TEXT("failed")) || OLHQSound::State().StartsWith(TEXT("none")), TEXT("a corrupt or missing pack is refused (") + OLHQSound::State() + TEXT(")"));
				SameAsToday(OLHQSound::OldFlag() ? TEXT("-OLOldSoundPack") : Expect == TEXT("off") ? TEXT("the row Off") : TEXT("a corrupt or missing pack"));
				if (OLHQSound::OldFlag())
				{
					// -OLOldSoundPack whatever the row says
					OLHQSound::SetChosen(true);
					Check(!OLHQSound::Active() && OLHQSound::DecodedThisRun() == 0 && !OLHQSound::Busy(), TEXT("-OLOldSoundPack: the row On decodes nothing and uses nothing"));
				}
			}
			// the settings key (default On) and the credits line
			TMap<FString, FString> S;
			L->GatherSettings(S);
			Check(S.FindRef(TEXT("HQSoundPack")) == (OLHQSound::Chosen() ? TEXT("true") : TEXT("false")), TEXT("HQSoundPack in the settings: ") + S.FindRef(TEXT("HQSoundPack")));
			FString Credits;
			FFileHelper::LoadFileToString(Credits, *(FPaths::ProjectContentDir() / TEXT("Credits") / TEXT("vr_credits.txt")));
			Credits.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
			const int32 Mods = Credits.Find(TEXT("\n~Mods\n")), Line = Credits.Find(TEXT("\nHQ Sound Pack by T4H_Dingle and Dxierzan\n"));
			Check(Mods != INDEX_NONE && Line > Mods, TEXT("the credits line 'HQ Sound Pack by T4H_Dingle and Dxierzan' under ~Mods"));
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT hqsound RESULT %d PASS, %d FAIL"), Pass, Fail);
		FGenericPlatformMisc::RequestExit(false);
	}
}

#endif // WITH_OUTLAWS_TESTS
