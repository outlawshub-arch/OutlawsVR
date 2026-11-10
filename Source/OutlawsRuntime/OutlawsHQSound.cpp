// REQ-HQSOUND: the HQ Sound Pack, found, checked and decoded once into the media cache; see OutlawsHQSound.h.
#include "OutlawsHQSound.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsUpdate.h"
#include "OutlawsLevelActor.h"
#include "Async/Async.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CoreMisc.h"
#include "CoreGlobals.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "VorbisAudioInfo.h"
#include "AudioDecompress.h"
// (as OutlawsLevelActorInf.cpp: FSoundQualityInfo lives in a Developer header a packaged game has no access to; its exact
// UE 5.1 layout, which the engine's Vorbis decoder fills)
#if !defined(OUTLAWS_SOUND_QUALITY_INFO)
#define OUTLAWS_SOUND_QUALITY_INFO
struct FSoundQualityInfo
{
	int32 Quality;
	uint32 NumChannels;
	uint32 SampleRate;
	uint32 SampleDataSize;
	float Duration;
	bool bStreaming;
	FString DebugName;
};
#endif
#include <atomic>

namespace
{
	enum class EHq : uint8 { None, Off, Decoding, Ready, Failed };

	FCriticalSection HqLock;
	EHq HqState = EHq::None;
	FString HqRoot, HqPack, HqCache, HqWhy;
	TMap<FString, FString> HqWavs;             // "name.wav" (lower) -> the decoded wav
	std::atomic<bool> bHqChosen{ true };
	std::atomic<bool> bHqStarted{ false };
	std::atomic<bool> bHqRootSet{ false };      // Start has run (the root kept for a row turned On later)
	std::atomic<uint32> HqSerial{ 1 };
	std::atomic<int32> HqDecoded{ 0 };
	double HqSeconds = 0.0;

	constexpr int32 PackFormat = 1;
	constexpr uint32 MaxSoundBytes = 64u * 1024 * 1024;

	uint32 Le32(const uint8* P) { return uint32(P[0]) | uint32(P[1]) << 8 | uint32(P[2]) << 16 | uint32(P[3]) << 24; }
	uint16 Le16(const uint8* P) { return uint16(P[0] | P[1] << 8); }

	void SetState(EHq S, const FString& Why = FString())
	{
		FScopeLock Guard(&HqLock);
		HqState = S;
		HqWhy = Why;
	}

	// one stored entry of the pack read whole, its CRC checked (the pack is written STORED: Ogg does not shrink)
	bool ReadStored(FArchive& Zip, const OLUpdate::FZipEntry& E, TArray<uint8>& Out)
	{
		if (E.Method != 0 || E.CompSize != E.Size || E.Size > MaxSoundBytes) return false;
		if (int64(E.LocalOffset) + 30 > Zip.TotalSize()) return false;
		uint8 Local[30];
		Zip.Seek(E.LocalOffset);
		Zip.Serialize(Local, 30);
		if (Zip.IsError() || Le32(Local) != 0x04034b50) return false;
		const int64 At = int64(E.LocalOffset) + 30 + Le16(Local + 26) + Le16(Local + 28);
		if (At + int64(E.Size) > Zip.TotalSize()) return false;
		Zip.Seek(At);
		Out.SetNumUninitialized(E.Size);
		if (E.Size > 0) Zip.Serialize(Out.GetData(), E.Size);
		return !Zip.IsError() && FCrc::MemCrc32(Out.GetData(), Out.Num()) == E.Crc;
	}

	FString ShaOf(const TArray<uint8>& D)
	{
		OLUpdate::FSha256 S;
		S.Update(D.GetData(), D.Num());
		return S.Finish();
	}

	struct FPackSound
	{
		FString Ogg;          // "name.ogg" as the zip holds it
		FString Sha;
		int64 Bytes = 0;
		const OLUpdate::FZipEntry* Entry = nullptr;
	};

	struct FPack
	{
		FString ManifestSha;
		TArray<OLUpdate::FZipEntry> Index;
		TArray<FPackSound> Sounds;
	};

	// the zip's directory and manifest.txt, every sound the manifest lists present in it (and nothing else in it)
	bool ReadPack(const FString& Path, FPack& Out, FString& Why)
	{
		if (!OLUpdate::ReadZipIndex(Path, Out.Index) || Out.Index.Num() == 0) { Why = TEXT("not a zip"); return false; }
		const OLUpdate::FZipEntry* Man = Out.Index.FindByPredicate([](const OLUpdate::FZipEntry& E) { return E.Name == TEXT("manifest.txt"); });
		if (!Man) { Why = TEXT("no manifest.txt"); return false; }
		TUniquePtr<FArchive> R(IFileManager::Get().CreateFileReader(*Path));
		TArray<uint8> Bytes;
		if (!R || !ReadStored(*R, *Man, Bytes) || Bytes.Num() == 0) { Why = TEXT("manifest.txt unreadable"); return false; }
		Out.ManifestSha = ShaOf(Bytes);
		const FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		TArray<FString> Lines;
		FString(Text.Length(), Text.Get()).ParseIntoArrayLines(Lines, true);
		TMap<FString, int32> ByName;
		for (int32 i = 0; i < Out.Index.Num(); ++i) ByName.Add(Out.Index[i].Name, i);
		for (const FString& Line : Lines)
		{
			if (Line.StartsWith(TEXT("#")))
			{
				if (Line.StartsWith(TEXT("# format ")) && FCString::Atoi(*Line.Mid(9)) != PackFormat) { Why = TEXT("another pack format: ") + Line.Mid(9); return false; }
				continue;
			}
			TArray<FString> Parts;
			Line.TrimStartAndEnd().ParseIntoArrayWS(Parts);
			if (Parts.Num() != 3 || Parts[0].Len() != 64 || !Parts[2].EndsWith(TEXT(".ogg")) || Parts[2].Contains(TEXT("/")) || !OLUpdate::SafeEntryName(Parts[2]))
			{
				Why = TEXT("a manifest line not understood: ") + Line.Left(80);
				return false;
			}
			const int32* At = ByName.Find(Parts[2]);
			if (!At) { Why = TEXT("not in the zip: ") + Parts[2]; return false; }
			FPackSound& S = Out.Sounds.AddDefaulted_GetRef();
			S.Ogg = Parts[2];
			S.Sha = Parts[0].ToLower();
			S.Bytes = FCString::Atoi64(*Parts[1]);
			S.Entry = &Out.Index[*At];
			if (S.Bytes != int64(S.Entry->Size)) { Why = TEXT("size not as the manifest has it: ") + S.Ogg; return false; }
		}
		if (Out.Sounds.Num() == 0 || Out.Sounds.Num() + 1 != Out.Index.Num()) { Why = FString::Printf(TEXT("%d sounds listed, %d files in the zip"), Out.Sounds.Num(), Out.Index.Num()); return false; }
		return true;
	}

	FString WavName(const FString& Ogg) { return FPaths::GetBaseFilename(Ogg).ToLower() + TEXT(".wav"); }

	FString StampPath() { return FPaths::Combine(HqCache, TEXT("stamp.txt")); }

	// stamp.txt, written after the last wav: the manifest's hash, then "<bytes> <name>.wav" a sound
	bool CacheMatches(const FPack& P, TMap<FString, FString>& OutWavs)
	{
		FString Stamp;
		if (!FFileHelper::LoadFileToString(Stamp, *StampPath())) return false;
		TArray<FString> Lines;
		Stamp.ParseIntoArrayLines(Lines, true);
		if (Lines.Num() != P.Sounds.Num() + 2 || Lines[0] != FString::Printf(TEXT("hqsound %d"), PackFormat) || Lines[1] != P.ManifestSha) return false;
		TMap<FString, FString> Wavs;
		for (int32 i = 2; i < Lines.Num(); ++i)
		{
			FString Size, Name;
			if (!Lines[i].Split(TEXT(" "), &Size, &Name)) return false;
			const FString File = FPaths::Combine(HqCache, Name);
			if (IFileManager::Get().FileSize(*File) != FCString::Atoi64(*Size)) return false;
			Wavs.Add(Name, File);
		}
		for (const FPackSound& S : P.Sounds) if (!Wavs.Contains(WavName(S.Ogg))) return false;
		OutWavs = MoveTemp(Wavs);
		return true;
	}

	void Publish(TMap<FString, FString>&& Wavs)
	{
		{
			FScopeLock Guard(&HqLock);
			HqWavs = MoveTemp(Wavs);
			HqState = EHq::Ready;
			HqWhy.Reset();
		}
		++HqSerial;
	}

	void Fail(const FString& Why)
	{
		UE_LOG(LogOutlaws, Warning, TEXT("OLRT hqsound: the pack is not used (%s): today's sounds"), *Why);
		SetState(EHq::Failed, Why);
		++HqSerial;
	}

	// 16-bit PCM as a RIFF wav
	void PutWav(TArray<uint8>& Out, const uint8* Pcm, int32 Bytes, int32 Channels, int32 Rate)
	{
		Out.Reset(44 + Bytes);
		auto U32 = [&Out](uint32 V) { for (int32 i = 0; i < 4; ++i) Out.Add(uint8(V >> (8 * i))); };
		auto U16 = [&Out](uint16 V) { Out.Add(uint8(V)); Out.Add(uint8(V >> 8)); };
		auto Tag = [&Out](const char* T) { Out.Append(reinterpret_cast<const uint8*>(T), 4); };
		Tag("RIFF"); U32(36 + Bytes); Tag("WAVE");
		Tag("fmt "); U32(16); U16(1); U16(uint16(Channels)); U32(uint32(Rate)); U32(uint32(Rate * Channels * 2)); U16(uint16(Channels * 2)); U16(16);
		Tag("data"); U32(Bytes);
		Out.Append(Pcm, Bytes);
	}

	// on a worker: each sound checked against the manifest, decoded and written; the stamp last, so a decode cut short is
	// done again at the next start
	void Decode(const FString& Path, const FString& Cache)
	{
		const double T0 = FPlatformTime::Seconds();
		FPack P;
		FString Why;
		if (!ReadPack(Path, P, Why)) { Fail(Why); return; }
		TUniquePtr<FArchive> R(IFileManager::Get().CreateFileReader(*Path));
		if (!R) { Fail(TEXT("the zip is not readable")); return; }
		IFileManager& F = IFileManager::Get();
		F.Delete(*StampPath(), false, true, true);
		F.MakeDirectory(*Cache, true);
		TMap<FString, FString> Wavs;
		FString Stamp = FString::Printf(TEXT("hqsound %d\n%s\n"), PackFormat, *P.ManifestSha);
		TArray<uint8> Ogg, Pcm, Wav;
		// every sound checked first (its CRC and SHA-256), so a damaged pack writes nothing
		for (const FPackSound& S : P.Sounds)
		{
			if (!ReadStored(*R, *S.Entry, Ogg)) { Fail(TEXT("unreadable: ") + S.Ogg); return; }
			if (ShaOf(Ogg) != S.Sha) { Fail(TEXT("SHA-256 not as the manifest has it: ") + S.Ogg); return; }
		}
		// a decode cut short leaves no wav of this run behind
		auto Undo = [&Wavs, &F]() { for (const TPair<FString, FString>& W : Wavs) F.Delete(*W.Value, false, true, true); };
		for (const FPackSound& S : P.Sounds)
		{
			if (IsEngineExitRequested()) { SetState(EHq::Failed, TEXT("the game closed during the decode")); return; }
			if (!ReadStored(*R, *S.Entry, Ogg) || ShaOf(Ogg) != S.Sha) { Undo(); Fail(TEXT("changed during the decode: ") + S.Ogg); return; }
			FVorbisAudioInfo Vorbis;
			FSoundQualityInfo Q;
			if (Ogg.Num() < 4 || FMemory::Memcmp(Ogg.GetData(), "OggS", 4) != 0 || !Vorbis.ReadCompressedInfo(Ogg.GetData(), Ogg.Num(), &Q)
				|| Q.SampleDataSize == 0 || Q.NumChannels < 1 || Q.NumChannels > 2 || Q.SampleRate == 0)
			{
				Undo();
				Fail(TEXT("does not decode: ") + S.Ogg);
				return;
			}
			Pcm.SetNumZeroed(Q.SampleDataSize);
			Vorbis.ExpandFile(Pcm.GetData(), &Q);
			PutWav(Wav, Pcm.GetData(), Pcm.Num(), Q.NumChannels, int32(Q.SampleRate));
			const FString Name = WavName(S.Ogg);
			const FString File = FPaths::Combine(Cache, Name);
			const FString Part = File + TEXT(".part");
			if (!FFileHelper::SaveArrayToFile(Wav, *Part) || !F.Move(*File, *Part, true, true)) { Undo(); Fail(TEXT("could not write ") + File); return; }
			Stamp += FString::Printf(TEXT("%d %s\n"), Wav.Num(), *Name);
			Wavs.Add(Name, File);
			++HqDecoded;
		}
		if (!FFileHelper::SaveStringToFile(Stamp, *StampPath(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM)) { Undo(); Fail(TEXT("could not write the stamp")); return; }
		HqSeconds = FPlatformTime::Seconds() - T0;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT hqsound: %d sounds decoded once into %s in %.2f s"), HqDecoded.load(), *Cache, HqSeconds);
		Publish(MoveTemp(Wavs));
	}

	FString FindPack(const FString& InstallRoot)
	{
#if WITH_OUTLAWS_TESTS
		// -OLHQSoundPackFile=<zip> (tests only): that file and no other, there or not
		FString Arg;
		if (OLFlags::Value(TEXT("OLHQSoundPackFile"), Arg, /*bShouldStopOnSeparator*/ false)) return FPaths::ConvertRelativePathToFull(Arg.TrimQuotes());
#endif
		TArray<FString> Tries;
		Tries.Add(FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::RootDir(), TEXT("Tools"), TEXT("HQSoundPack.zip"))));
		const FString Updated = OLUpdate::UpdatedDataDir();
		if (!Updated.IsEmpty()) Tries.Add(FPaths::Combine(Updated, TEXT("HQSoundPack.zip")));
		if (!InstallRoot.IsEmpty()) Tries.Add(FPaths::Combine(InstallRoot, TEXT("HQSoundPack.zip")));
		for (const FString& T : Tries) if (FPaths::FileExists(T)) return T;
		return FString();
	}

	FString FindCache()
	{
#if WITH_OUTLAWS_TESTS
		// -OLTestMediaDir=<dir> (tests only): the test's own media folder, as the music's
		FString TestDir;
		if (OLFlags::Value(TEXT("OLTestMediaDir"), TestDir, /*bShouldStopOnSeparator*/ false) && !TestDir.TrimQuotes().IsEmpty()) return FPaths::Combine(TestDir.TrimQuotes(), TEXT("HQSound"));
#endif
#if PLATFORM_ANDROID
		return FPaths::Combine(FPaths::ProjectPersistentDownloadDir(), TEXT("HQSound"));
#else
		return FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("OutlawsVR"), TEXT("media"), TEXT("HQSound"));
#endif
	}

	// game thread: the pack found, then the cache taken or the decode started
	void Kick()
	{
		if (bHqStarted.exchange(true)) return;
		const FString Path = FindPack(HqRoot);
		const FString Cache = FindCache();
		{
			FScopeLock Guard(&HqLock);
			HqPack = Path;
			HqCache = Cache;
		}
		if (Path.IsEmpty() || !FPaths::FileExists(Path))
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLRT hqsound: no HQ Sound Pack found%s: today's sounds"), Path.IsEmpty() ? TEXT("") : *(TEXT(" at ") + Path));
			SetState(EHq::None, TEXT("no pack"));
			return;
		}
		FPack P;
		FString Why;
		if (!ReadPack(Path, P, Why)) { Fail(Why); return; }
		TMap<FString, FString> Wavs;
		if (CacheMatches(P, Wavs))
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLRT hqsound: %d sounds from the cache %s (decoded before; nothing to decode)"), Wavs.Num(), *Cache);
			Publish(MoveTemp(Wavs));
			return;
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLRT hqsound: decoding %d sounds from %s into %s (once)"), P.Sounds.Num(), *Path, *Cache);
		SetState(EHq::Decoding);
		LoadVorbisLibraries();       // (on the game thread, before the worker uses the decoder)
		Async(EAsyncExecution::Thread, [Path, Cache]() { Decode(Path, Cache); });
	}
}

bool OLHQSound::Chosen() { return bHqChosen; }

void OLHQSound::SetChosen(bool bOn)
{
	if (bHqChosen.exchange(bOn) == bOn) return;
	++HqSerial;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT hqsound: the HQ Sound Pack row %s"), bOn ? TEXT("On") : TEXT("Off"));
	if (bOn && !OldFlag() && bHqRootSet) Kick();
}

bool OLHQSound::OldFlag()
{
	static const bool bOld = OLFlags::Param(TEXT("OLOldSoundPack"));
	return bOld;
}

void OLHQSound::Start(const FString& InstallRoot)
{
	if (bHqRootSet.exchange(true)) return;
	HqRoot = InstallRoot;
	// the row as the settings file has it (the level's LoadSettings sets it again, the same key)
	const TMap<FString, FString> S = AOutlawsLevelActor::ReadSettings();
	if (const FString* X = S.Find(TEXT("HQSoundPack"))) bHqChosen = X->ToBool();
	if (OldFlag() || !bHqChosen)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLRT hqsound: off (%s): today's sounds"), OldFlag() ? TEXT("-OLOldSoundPack") : TEXT("the row is Off"));
		SetState(EHq::Off);
		return;
	}
	Kick();
}

bool OLHQSound::Active()
{
	if (!bHqChosen || OldFlag()) return false;
	FScopeLock Guard(&HqLock);
	return HqState == EHq::Ready;
}

FString OLHQSound::WavFor(const FString& Name)
{
	if (!bHqChosen || OldFlag()) return FString();
	FScopeLock Guard(&HqLock);
	if (HqState != EHq::Ready) return FString();
	const FString* W = HqWavs.Find(Name.ToLower());
	return W ? *W : FString();
}

uint32 OLHQSound::Serial() { return HqSerial; }

FString OLHQSound::State()
{
	FScopeLock Guard(&HqLock);
	switch (HqState)
	{
	case EHq::Off: return TEXT("off");
	case EHq::Decoding: return TEXT("decoding");
	case EHq::Ready: return TEXT("ready");
	case EHq::Failed: return TEXT("failed (") + HqWhy + TEXT(")");
	default: return HqWhy.IsEmpty() ? FString(TEXT("none")) : TEXT("none (") + HqWhy + TEXT(")");
	}
}

bool OLHQSound::Busy()
{
	FScopeLock Guard(&HqLock);
	return HqState == EHq::Decoding;
}

FString OLHQSound::PackPath() { FScopeLock Guard(&HqLock); return HqPack; }
FString OLHQSound::CacheDir() { FScopeLock Guard(&HqLock); return HqCache; }

TArray<FString> OLHQSound::Names()
{
	TArray<FString> Out;
	FScopeLock Guard(&HqLock);
	HqWavs.GetKeys(Out);
	Out.Sort();
	return Out;
}

int32 OLHQSound::DecodedThisRun() { return HqDecoded; }
double OLHQSound::DecodeSeconds() { return HqSeconds; }
