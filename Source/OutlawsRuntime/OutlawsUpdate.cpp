// REQ-AUTOUPDATE: the game's own update check and install, see OutlawsUpdate.h. The check is one GET of latest.json beside
// the newest release; the download is the release zip in Range chunks (so nothing that size sits in memory, and a stopped
// download goes on from where it was at the next Update now); the zip is the same one for both platforms. Probe:
// OutlawsUpdateProbe.cpp. Old behaviour (no check at all): -OLOldUpdater.

#include "OutlawsUpdate.h"
#include "OutlawsBeacon.h"
#include "OutlawsBuildInfo.h"
#include "OutlawsFlags.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "EngineUtils.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/App.h"
#include "Misc/Compression.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if PLATFORM_ANDROID
#include "Android/AndroidApplication.h"
#include "Android/AndroidJNI.h"
#include "Android/AndroidJavaEnv.h"
#endif

#ifndef OL_UPDATE_BUILD
#define OL_UPDATE_BUILD 0
#endif

#if WITH_OUTLAWS_TESTS
void OLUpdateStartProbe(UOutlawsUpdate* U);    // OutlawsUpdateProbe.cpp
#endif

namespace
{
	constexpr int64 ChunkBytes = 8 * 1024 * 1024;
	constexpr int32 MaxRetries = 4;
	constexpr int64 MaxFeedBytes = 64 * 1024;
	constexpr int64 MaxEntryBytes = 1024ll * 1024 * 1024;
	const TCHAR* const SetCheck = TEXT("UpdateCheck");
	const TCHAR* const SetSkip = TEXT("UpdateSkipBuild");

	bool PlainName(const FString& S)
	{
		if (S.IsEmpty() || S.Len() > 128 || S.Contains(TEXT(".."))) return false;
		for (TCHAR C : S)
		{
			if (!FChar::IsAlnum(C) && C != TEXT('.') && C != TEXT('-') && C != TEXT('_')) return false;
		}
		return true;
	}

	bool HexHash(const FString& S)
	{
		if (S.Len() != 64) return false;
		for (TCHAR C : S) if (!FChar::IsHexDigit(C)) return false;
		return true;
	}

	uint32 Le32(const uint8* P) { return uint32(P[0]) | uint32(P[1]) << 8 | uint32(P[2]) << 16 | uint32(P[3]) << 24; }
	uint16 Le16(const uint8* P) { return uint16(P[0] | P[1] << 8); }
	uint32 Ror(uint32 X, int32 N) { return (X >> N) | (X << (32 - N)); }

#if PLATFORM_ANDROID
	bool JavaBool(const char* Name)
	{
		JNIEnv* Env = FAndroidApplication::GetJavaEnv();
		jmethodID M = Env ? FJavaWrapper::FindMethod(Env, FJavaWrapper::GameActivityClassID, Name, "()Z", false) : nullptr;
		return M && FJavaWrapper::CallBooleanMethod(Env, FJavaWrapper::GameActivityThis, M);
	}
	int32 JavaInt(const char* Name)
	{
		JNIEnv* Env = FAndroidApplication::GetJavaEnv();
		jmethodID M = Env ? FJavaWrapper::FindMethod(Env, FJavaWrapper::GameActivityClassID, Name, "()I", false) : nullptr;
		return M ? FJavaWrapper::CallIntMethod(Env, FJavaWrapper::GameActivityThis, M) : -1;
	}
	void JavaVoid(const char* Name)
	{
		JNIEnv* Env = FAndroidApplication::GetJavaEnv();
		jmethodID M = Env ? FJavaWrapper::FindMethod(Env, FJavaWrapper::GameActivityClassID, Name, "()V", false) : nullptr;
		if (M) FJavaWrapper::CallVoidMethod(Env, FJavaWrapper::GameActivityThis, M);
	}
	void JavaInstall(const FString& Apk)
	{
		JNIEnv* Env = FAndroidApplication::GetJavaEnv();
		jmethodID M = Env ? FJavaWrapper::FindMethod(Env, FJavaWrapper::GameActivityClassID, "AndroidThunkJava_OLUpdate_Install", "(Ljava/lang/String;)V", false) : nullptr;
		if (!M) return;
		jstring S = Env->NewStringUTF(TCHAR_TO_UTF8(*Apk));
		FJavaWrapper::CallVoidMethod(Env, FJavaWrapper::GameActivityThis, M, S);
		Env->DeleteLocalRef(S);
	}
#endif
}

// ---- the feed --------------------------------------------------------------------------------------------------------------

FString OLUpdate::FeedUrl()
{
	return TEXT("https://github.com/outlawshub-arch/OutlawsVR/releases/latest/download/latest.json");
}

FString OLUpdate::ZipUrl(const FLatest& L)
{
	return FString::Printf(TEXT("https://github.com/outlawshub-arch/OutlawsVR/releases/download/%s/%s"), *L.Tag, *L.Zip);
}

int32 OLUpdate::MyBuild()
{
	return OL_UPDATE_BUILD;
}

bool OLUpdate::ParseLatest(const FString& Json, FLatest& Out)
{
	TSharedPtr<FJsonObject> O;
	const TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(R, O) || !O.IsValid()) return false;
	int32 Schema = 0;
	double Size = 0.0;
	FLatest L;
	if (!O->TryGetNumberField(TEXT("schema"), Schema) || Schema != 1) return false;
	if (!O->TryGetNumberField(TEXT("build"), L.Build) || L.Build <= 0) return false;
	if (!O->TryGetNumberField(TEXT("zip_size"), Size) || Size <= 0.0) return false;
	L.ZipSize = int64(Size);
	O->TryGetStringField(TEXT("version"), L.Version);
	O->TryGetStringField(TEXT("commit"), L.Commit);
	O->TryGetStringField(TEXT("tag"), L.Tag);
	O->TryGetStringField(TEXT("zip"), L.Zip);
	O->TryGetStringField(TEXT("zip_sha256"), L.ZipSha256);
	O->TryGetStringField(TEXT("apk_sha256"), L.ApkSha256);
	// (the file's words go into a URL and a path, and into the bubble: plain names only, one release of ours only)
	if (!PlainName(L.Tag) || !PlainName(L.Zip) || !PlainName(L.Version) || !L.Zip.EndsWith(TEXT(".zip"))) return false;
	if (!HexHash(L.ZipSha256) || !HexHash(L.ApkSha256)) return false;
	L.ZipSha256.ToLowerInline();
	L.ApkSha256.ToLowerInline();
	Out = L;
	return true;
}

OLUpdate::EDecide OLUpdate::Decide(const FLatest& L, int32 Mine, int32 SkipBuild)
{
	if (L.Build <= Mine) return EDecide::Nothing;
	return L.Build == SkipBuild ? EDecide::Skipped : EDecide::Prompt;
}

// ---- SHA-256 (FIPS 180-4; the engine's Core has MD5 and SHA-1 only) ------------------------------------------------------------

OLUpdate::FSha256::FSha256()
{
	static const uint32 Start[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
	FMemory::Memcpy(H, Start, sizeof(H));
}

void OLUpdate::FSha256::Block(const uint8* P)
{
	static const uint32 K[64] = {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
		0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
		0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
		0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };
	uint32 W[64];
	for (int32 i = 0; i < 16; ++i) W[i] = uint32(P[i * 4]) << 24 | uint32(P[i * 4 + 1]) << 16 | uint32(P[i * 4 + 2]) << 8 | uint32(P[i * 4 + 3]);
	for (int32 i = 16; i < 64; ++i)
	{
		const uint32 S0 = Ror(W[i - 15], 7) ^ Ror(W[i - 15], 18) ^ (W[i - 15] >> 3);
		const uint32 S1 = Ror(W[i - 2], 17) ^ Ror(W[i - 2], 19) ^ (W[i - 2] >> 10);
		W[i] = W[i - 16] + S0 + W[i - 7] + S1;
	}
	uint32 A = H[0], B = H[1], C = H[2], D = H[3], E = H[4], F = H[5], G = H[6], X = H[7];
	for (int32 i = 0; i < 64; ++i)
	{
		const uint32 T1 = X + (Ror(E, 6) ^ Ror(E, 11) ^ Ror(E, 25)) + ((E & F) ^ (~E & G)) + K[i] + W[i];
		const uint32 T2 = (Ror(A, 2) ^ Ror(A, 13) ^ Ror(A, 22)) + ((A & B) ^ (A & C) ^ (B & C));
		X = G; G = F; F = E; E = D + T1; D = C; C = B; B = A; A = T1 + T2;
	}
	H[0] += A; H[1] += B; H[2] += C; H[3] += D; H[4] += E; H[5] += F; H[6] += G; H[7] += X;
}

void OLUpdate::FSha256::Update(const uint8* Data, int64 Len)
{
	Total += uint64(Len);
	while (Len > 0)
	{
		if (Fill == 0 && Len >= 64)
		{
			Block(Data);
			Data += 64;
			Len -= 64;
			continue;
		}
		const int32 Take = int32(FMath::Min<int64>(64 - Fill, Len));
		FMemory::Memcpy(Buf + Fill, Data, Take);
		Fill += Take;
		Data += Take;
		Len -= Take;
		if (Fill == 64) { Block(Buf); Fill = 0; }
	}
}

FString OLUpdate::FSha256::Finish()
{
	const uint64 Bits = Total * 8;
	const uint8 One = 0x80, Nil = 0;
	Update(&One, 1);
	while (Fill != 56) Update(&Nil, 1);
	uint8 Tail[8];
	for (int32 i = 0; i < 8; ++i) Tail[i] = uint8(Bits >> (56 - i * 8));
	Update(Tail, 8);
	FString Out;
	for (int32 i = 0; i < 8; ++i) Out += FString::Printf(TEXT("%08x"), H[i]);
	return Out;
}

FString OLUpdate::Sha256OfFile(const FString& Path)
{
	TUniquePtr<FArchive> R(IFileManager::Get().CreateFileReader(*Path));
	if (!R) return FString();
	FSha256 S;
	TArray<uint8> Buf;
	Buf.SetNumUninitialized(1024 * 1024);
	int64 Left = R->TotalSize();
	while (Left > 0)
	{
		const int64 Take = FMath::Min<int64>(Left, Buf.Num());
		R->Serialize(Buf.GetData(), Take);
		if (R->IsError()) return FString();
		S.Update(Buf.GetData(), Take);
		Left -= Take;
	}
	return S.Finish();
}

// ---- the zip (no zip reader in a Shipping runtime; a release zip is plain deflate, under 4 GB) ----------------------------------

bool OLUpdate::SafeEntryName(const FString& Name)
{
	if (Name.IsEmpty() || Name.StartsWith(TEXT("/")) || Name.Contains(TEXT(":"))) return false;
	TArray<FString> Parts;
	Name.ParseIntoArray(Parts, TEXT("/"), true);
	for (const FString& P : Parts) if (P == TEXT("..") || P == TEXT(".")) return false;
	return true;
}

bool OLUpdate::ReadZipIndex(const FString& ZipPath, TArray<FZipEntry>& Out)
{
	Out.Reset();
	TUniquePtr<FArchive> R(IFileManager::Get().CreateFileReader(*ZipPath));
	if (!R) return false;
	const int64 Size = R->TotalSize();
	const int64 TailLen = FMath::Min<int64>(Size, 66 * 1024);
	if (TailLen < 22) return false;
	TArray<uint8> Tail;
	Tail.SetNumUninitialized(TailLen);
	R->Seek(Size - TailLen);
	R->Serialize(Tail.GetData(), TailLen);
	int64 At = TailLen - 22;
	while (At >= 0 && Le32(&Tail[At]) != 0x06054b50) --At;
	if (At < 0) return false;
	const uint32 DirSize = Le32(&Tail[At + 12]), DirAt = Le32(&Tail[At + 16]);
	const int32 Count = Le16(&Tail[At + 10]);
	if (DirAt == 0xffffffffu || Count == 0xffff || int64(DirAt) + DirSize > Size) return false;
	TArray<uint8> Dir;
	Dir.SetNumUninitialized(DirSize);
	R->Seek(DirAt);
	R->Serialize(Dir.GetData(), DirSize);
	if (R->IsError()) return false;
	int64 P = 0;
	for (int32 i = 0; i < Count; ++i)
	{
		if (P + 46 > DirSize || Le32(&Dir[P]) != 0x02014b50) return false;
		FZipEntry E;
		E.Method = Le16(&Dir[P + 10]);
		E.Crc = Le32(&Dir[P + 16]);
		E.CompSize = Le32(&Dir[P + 20]);
		E.Size = Le32(&Dir[P + 24]);
		const int32 NameLen = Le16(&Dir[P + 28]), Extra = Le16(&Dir[P + 30]), Note = Le16(&Dir[P + 32]);
		E.LocalOffset = Le32(&Dir[P + 42]);
		if (P + 46 + NameLen > DirSize) return false;
		const FUTF8ToTCHAR Name(reinterpret_cast<const ANSICHAR*>(&Dir[P + 46]), NameLen);
		E.Name = FString(Name.Length(), Name.Get()).Replace(TEXT("\\"), TEXT("/"));
		P += 46 + NameLen + Extra + Note;
		if (E.CompSize == 0xffffffffu || E.Size == 0xffffffffu || E.LocalOffset == 0xffffffffu) return false;
		if (!SafeEntryName(E.Name)) return false;           // (one bad name and the whole zip is not ours)
		Out.Add(MoveTemp(E));
	}
	return true;
}

bool OLUpdate::ExtractEntry(FArchive& Zip, const FZipEntry& E, const FString& ToFile)
{
	if (E.Size > MaxEntryBytes || E.CompSize > MaxEntryBytes || (E.Method != 0 && E.Method != 8)) return false;
	uint8 Local[30];
	if (int64(E.LocalOffset) + 30 > Zip.TotalSize()) return false;
	Zip.Seek(E.LocalOffset);
	Zip.Serialize(Local, 30);
	if (Le32(Local) != 0x04034b50) return false;
	Zip.Seek(int64(E.LocalOffset) + 30 + Le16(Local + 26) + Le16(Local + 28));
	TArray<uint8> In, Data;
	In.SetNumUninitialized(E.CompSize);
	Data.SetNumUninitialized(E.Size);
	if (E.CompSize > 0) Zip.Serialize(In.GetData(), E.CompSize);
	if (Zip.IsError()) return false;
	if (E.Size > 0)
	{
		// (a window of -15 is zlib's raw deflate, what a zip holds)
		if (E.Method == 8)
		{
			if (!FCompression::UncompressMemory(NAME_Zlib, Data.GetData(), int32(E.Size), In.GetData(), int32(E.CompSize), COMPRESS_NoFlags, -15)) return false;
		}
		else
		{
			if (E.CompSize != E.Size) return false;
			Data = MoveTemp(In);
		}
		if (FCrc::MemCrc32(Data.GetData(), Data.Num()) != E.Crc) return false;
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ToFile), true);
	return FFileHelper::SaveArrayToFile(Data, *ToFile);
}

bool OLUpdate::ExtractForPc(const FString& ZipPath, const FString& NewDir, FString& Why)
{
	TArray<FZipEntry> Index;
	if (!ReadZipIndex(ZipPath, Index) || Index.Num() == 0) { Why = TEXT("not a zip"); return false; }
	FString Top;
	Index[0].Name.Split(TEXT("/"), &Top, nullptr);
	if (Top.IsEmpty()) { Why = TEXT("no top folder"); return false; }
	const FString Pre = Top + TEXT("/");
	bool bExe = false, bTheme = false, bMaps = false;
	for (const FZipEntry& E : Index)
	{
		if (!E.Name.StartsWith(Pre)) { Why = TEXT("more than one top folder"); return false; }
		bExe |= E.Name == Pre + TEXT("OutlawsUE.exe");
		bTheme |= E.Name == Pre + TEXT("Tools/MenuOverlay.kpf");
		bMaps |= E.Name.StartsWith(Pre + TEXT("Tools/NativeMaps/")) && !E.Name.EndsWith(TEXT("/"));
	}
	if (!bExe || !bTheme || !bMaps)      // (user 5 Nov: an update without these loses the theme)
	{
		Why = FString::Printf(TEXT("the zip lacks%s%s%s"), bExe ? TEXT("") : TEXT(" the exe"), bTheme ? TEXT("") : TEXT(" the menu theme"), bMaps ? TEXT("") : TEXT(" the native maps"));
		return false;
	}
	IFileManager::Get().DeleteDirectory(*NewDir, false, true);
	TUniquePtr<FArchive> R(IFileManager::Get().CreateFileReader(*ZipPath));
	if (!R) { Why = TEXT("zip not readable"); return false; }
	for (const FZipEntry& E : Index)
	{
		if (E.Name.EndsWith(TEXT("/"))) continue;
		if (!ExtractEntry(*R, E, FPaths::Combine(NewDir, E.Name.Mid(Pre.Len()))))
		{
			Why = FString::Printf(TEXT("could not unpack %s"), *E.Name);
			return false;
		}
	}
	return true;
}

bool OLUpdate::ExtractForQuest(const FString& ZipPath, const FString& DataDir, const FString& ApkTo, const FString& ApkSha256, FString& Why)
{
	TArray<FZipEntry> Index;
	if (!ReadZipIndex(ZipPath, Index) || Index.Num() == 0) { Why = TEXT("not a zip"); return false; }
	const FZipEntry* Apk = nullptr;
	const FZipEntry* Theme = nullptr;
	const FZipEntry* HqPack = nullptr;       // (REQ-HQSOUND) the HQ Sound Pack, when the release carries it
	TArray<TPair<const FZipEntry*, FString>> Maps;
	for (const FZipEntry& E : Index)
	{
		int32 Cut = INDEX_NONE;
		if (!E.Name.FindChar(TEXT('/'), Cut)) continue;
		const FString Rel = E.Name.Mid(Cut + 1);
		if (Rel == TEXT("Tools/OutlawsVR.apk")) Apk = &E;
		else if (Rel == TEXT("Tools/MenuOverlay.kpf")) Theme = &E;
		else if (Rel == TEXT("Tools/HQSoundPack.zip")) HqPack = &E;
		else if (Rel.StartsWith(TEXT("Tools/NativeMaps/")) && !Rel.EndsWith(TEXT("/"))) Maps.Emplace(&E, Rel.Mid(6));
	}
	if (!Apk || !Theme || Maps.Num() == 0)
	{
		Why = FString::Printf(TEXT("the zip lacks%s%s%s"), Apk ? TEXT("") : TEXT(" the apk"), Theme ? TEXT("") : TEXT(" the menu theme"), Maps.Num() ? TEXT("") : TEXT(" the native maps"));
		return false;
	}
	IFileManager::Get().DeleteDirectory(*DataDir, false, true);
	TUniquePtr<FArchive> R(IFileManager::Get().CreateFileReader(*ZipPath));
	if (!R) { Why = TEXT("zip not readable"); return false; }
	if (!ExtractEntry(*R, *Theme, FPaths::Combine(DataDir, TEXT("MenuOverlay.kpf")))) { Why = TEXT("could not unpack the menu theme"); return false; }
	for (const TPair<const FZipEntry*, FString>& M : Maps)
	{
		if (!ExtractEntry(*R, *M.Key, FPaths::Combine(DataDir, M.Value))) { Why = FString::Printf(TEXT("could not unpack %s"), *M.Value); return false; }
	}
	if (HqPack && !ExtractEntry(*R, *HqPack, FPaths::Combine(DataDir, TEXT("HQSoundPack.zip")))) { Why = TEXT("could not unpack the HQ Sound Pack"); return false; }
	if (!ExtractEntry(*R, *Apk, ApkTo)) { Why = TEXT("could not unpack the apk"); return false; }
	if (Sha256OfFile(ApkTo) != ApkSha256)
	{
		IFileManager::Get().Delete(*ApkTo);
		Why = TEXT("the apk's hash is not the one in latest.json");
		return false;
	}
	return true;
}

// The swap: the game cannot replace its own running files, so this runs after it has closed. It holds no paths (it finds the
// install from where it sits), keeps the build that was there in Update\old until the new one has started, and goes back to
// it if a copy fails. A plain copy over the folder, not a rename of it: Explorer or adb holding the folder open must not
// stop the update. Saved stays where it is, and anything of the player's outside the build's own two folders too.
const TCHAR* OLUpdate::SwapScript()
{
	return TEXT(R"OLS(@echo off
rem Outlaws VR: puts an update in place after the game has closed (REQ-AUTOUPDATE). The game writes this file.
setlocal
set "UPD=%~dp0"
set "UPD=%UPD:~0,-1%"
for %%I in ("%UPD%\..") do set "ROOT=%%~fI"
cd /d "%UPD%"
set "LOG=%UPD%\swap.log"
set "RC=/NFL /NDL /NJH /NJS /NP /R:2 /W:1"
echo update: waiting for the game to close (%1)>>"%LOG%"
set /a TRIES=0
:wait
tasklist /FI "PID eq %1" /NH 2>nul | find "%1" >nul
if errorlevel 1 goto gone
set /a TRIES+=1
if %TRIES% geq 60 goto nogame
ping -n 2 localhost >nul
goto wait
:nogame
echo update: the game did not close, nothing changed>>"%LOG%"
exit /b 3
:gone
if not exist "%UPD%\new\OutlawsUE.exe" goto notnew
if not exist "%UPD%\new\Tools\MenuOverlay.kpf" goto notnew
if not exist "%UPD%\new\Tools\NativeMaps" goto notnew
robocopy "%ROOT%" "%UPD%\old" /E /XD "%UPD%" Saved %RC% >>"%LOG%"
if errorlevel 8 goto notnew
robocopy "%UPD%\new" "%ROOT%" /E /XD Saved %RC% >>"%LOG%"
if errorlevel 8 goto back
if not exist "%UPD%\new\OutlawsUE" goto noue
robocopy "%UPD%\new\OutlawsUE" "%ROOT%\OutlawsUE" /MIR /XD Saved %RC% >>"%LOG%"
if errorlevel 8 goto back
:noue
if not exist "%UPD%\new\Engine" goto noengine
robocopy "%UPD%\new\Engine" "%ROOT%\Engine" /MIR /XD Saved %RC% >>"%LOG%"
if errorlevel 8 goto back
:noengine
if not exist "%ROOT%\OutlawsUE.exe" goto back
if not exist "%ROOT%\Tools\MenuOverlay.kpf" goto back
if not exist "%ROOT%\Tools\NativeMaps" goto back
echo update: done>>"%LOG%"
echo done>"%UPD%\done"
goto run
:notnew
echo update: the new build is not whole, nothing changed>>"%LOG%"
echo failed>"%UPD%\failed"
goto run
:back
echo update: a copy failed, going back>>"%LOG%"
robocopy "%UPD%\old" "%ROOT%" /E /XD Saved %RC% >>"%LOG%"
if exist "%UPD%\old\OutlawsUE" robocopy "%UPD%\old\OutlawsUE" "%ROOT%\OutlawsUE" /MIR /XD Saved %RC% >>"%LOG%"
if exist "%UPD%\old\Engine" robocopy "%UPD%\old\Engine" "%ROOT%\Engine" /MIR /XD Saved %RC% >>"%LOG%"
echo failed>"%UPD%\failed"
:run
cd /d "%ROOT%"
if "%2"=="test" goto testrun
if exist "%ROOT%\Play Outlaws VR.bat" goto bat
start "" "%ROOT%\OutlawsUE.exe" -vr -SaveToUserDir
exit /b 0
:bat
start "" "%ROOT%\Play Outlaws VR.bat"
exit /b 0
:testrun
call "%ROOT%\Play Outlaws VR.bat"
exit /b 0
)OLS");
}

FString OLUpdate::UpdatedDataIn(const FString& Base, int32 Mine)
{
	FString Stamp;
	const FString Data = FPaths::Combine(Base, TEXT("data"));
	if (!FFileHelper::LoadFileToString(Stamp, *FPaths::Combine(Base, TEXT("build.txt")))) return FString();
	return FCString::Atoi(*Stamp) >= Mine && Mine > 0 && FPaths::DirectoryExists(Data) ? Data : FString();
}

FString OLUpdate::UpdatedDataDir()
{
#if PLATFORM_ANDROID
	static const FString Dir = UpdatedDataIn(FPaths::Combine(FPaths::ProjectPersistentDownloadDir(), TEXT("OutlawsUpdate")), MyBuild());
	return Dir;
#else
	return FString();
#endif
}

// ---- the subsystem -----------------------------------------------------------------------------------------------------------

UOutlawsUpdate* UOutlawsUpdate::Get(const UObject* Context)
{
	const UWorld* W = GEngine && Context ? GEngine->GetWorldFromContextObject(Context, EGetWorldErrorMode::ReturnNull) : nullptr;
	UGameInstance* GI = W ? W->GetGameInstance() : nullptr;
	return GI ? GI->GetSubsystem<UOutlawsUpdate>() : nullptr;
}

bool UOutlawsUpdate::CheckOn()
{
	const TMap<FString, FString> S = AOutlawsLevelActor::ReadSettings();
	const FString* X = S.Find(SetCheck);
	return !X || X->ToBool();
}

void UOutlawsUpdate::ToggleCheck()
{
	TMap<FString, FString> S = AOutlawsLevelActor::ReadSettings();
	const bool bOn = !CheckOn();
	S.Add(SetCheck, bOn ? TEXT("true") : TEXT("false"));
	AOutlawsLevelActor::WriteSettings(S);
	UE_LOG(LogOutlaws, Log, TEXT("OLUPD the check for updates: %s"), bOn ? TEXT("on") : TEXT("off"));
}

bool UOutlawsUpdate::HandOnBubble(const UObject* Context, int32 Hand)
{
	const UOutlawsUpdate* U = Get(Context);
	return U && IsValid(U->Bubble) && U->Bubble->GetKind() != EOutlawsBubbleKind::None && !U->Bubble->IsHidden() && U->Bubble->HandOn(Hand) >= 0;
}

bool UOutlawsUpdate::BubbleUp(const UObject* Context)
{
	const UOutlawsUpdate* U = Get(Context);
	return U && IsValid(U->Bubble) && U->Bubble->GetKind() != EOutlawsBubbleKind::None;
}

TMap<FString, FString> UOutlawsUpdate::Settings() const
{
#if WITH_OUTLAWS_TESTS
	if (bTest) return Test.Settings;
#endif
	return AOutlawsLevelActor::ReadSettings();
}

void UOutlawsUpdate::WriteSetting(const FString& Key, const FString& Value)
{
#if WITH_OUTLAWS_TESTS
	if (bTest) { Test.Settings.Add(Key, Value); return; }
#endif
	TMap<FString, FString> S = AOutlawsLevelActor::ReadSettings();
	S.Add(Key, Value);
	AOutlawsLevelActor::WriteSettings(S);
}

FString UOutlawsUpdate::RootDir() const
{
#if WITH_OUTLAWS_TESTS
	if (bTest) return Test.Root;
#endif
	return FPaths::ConvertRelativePathToFull(FPaths::RootDir());
}

UWorld* UOutlawsUpdate::GetTickableGameObjectWorld() const
{
	const UGameInstance* GI = GetGameInstance();
	return GI ? GI->GetWorld() : nullptr;
}

TStatId UOutlawsUpdate::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UOutlawsUpdate, STATGROUP_Tickables);
}

// (BUG-UPDATETESTFEED, 7 Nov) only a player's run goes to the feed. A worktree's leftover OutlawsBuildStamp.h gave its test runs a
// real commit, so the old "dev" test let them through, and a published latest.json could have put the prompt into their menu
// tests. So never from a build with the test code in it, whatever its stamp; and in Shipping (the release's, and the real-app
// test's OL_UPDATE_TEST_BUILD build, packaged Shipping) never from a run a script started. Null: go on, check.
static const TCHAR* WhyNoCheck()
{
	if (OLFlags::Param(TEXT("OLOldUpdater"))) return TEXT("-OLOldUpdater");
	if (IsRunningCommandlet()) return TEXT("a commandlet");
#if WITH_OUTLAWS_TESTS
	return TEXT("a build with the test code");
#else
	if (FCString::Strcmp(OutlawsBuildInfo::Commit(), TEXT("dev")) == 0) return TEXT("no build stamp");
	if (OLFlags::Param(TEXT("OLShipSmoke"))) return TEXT("-OLShipSmoke");
	if (FApp::IsUnattended()) return TEXT("-unattended");
	return nullptr;
#endif
}

void UOutlawsUpdate::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
#if PLATFORM_ANDROID
	bQuest = true;
#endif
#if WITH_OUTLAWS_TESTS
	if (OLFlags::Param(TEXT("OLUpdateProbe")) && !GIsEditor && !IsRunningCommandlet()) OLUpdateStartProbe(this);
#endif
	if (const TCHAR* Off = WhyNoCheck())
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLUPD no check: %s"), Off);
		return;       // (an editor run or a test: never on the network; a player's build only)
	}
	CleanOldUpdate();
	if (!CheckOn())
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLUPD the check for updates is off in Options"));
		return;
	}
	Step = EStep::Wait;
	NextAt = FPlatformTime::Seconds() + 3.0;
}

void UOutlawsUpdate::Deinitialize()
{
	if (Work.IsValid()) Work.Wait();
	Super::Deinitialize();
}

// After an update has gone in, what it left behind: the zip, the unpacked build, the old one. (swap.log stays, a small note.)
void UOutlawsUpdate::CleanOldUpdate()
{
	IFileManager& F = IFileManager::Get();
	if (bQuest)
	{
		const FString Base = FPaths::Combine(FPaths::ProjectPersistentDownloadDir(), TEXT("OutlawsUpdate"));
		FString Stamp;
		if (FFileHelper::LoadFileToString(Stamp, *FPaths::Combine(Base, TEXT("build.txt"))) && FCString::Atoi(*Stamp) <= OLUpdate::MyBuild())
		{
			F.DeleteDirectory(*FPaths::Combine(Base, TEXT("download")), false, true);
		}
		return;
	}
	const FString Dir = FPaths::Combine(RootDir(), TEXT("Update"));
	const bool bDone = F.FileExists(*FPaths::Combine(Dir, TEXT("done")));
	if (!bDone && !F.FileExists(*FPaths::Combine(Dir, TEXT("failed")))) return;
	UE_LOG(LogOutlaws, Log, TEXT("OLUPD the last update %s"), bDone ? TEXT("went in, its files cleared") : TEXT("did not go in: this is the build from before it"));
	F.DeleteDirectory(*FPaths::Combine(Dir, TEXT("new")), false, true);
	F.DeleteDirectory(*FPaths::Combine(Dir, TEXT("old")), false, true);
	TArray<FString> Zips;
	F.FindFiles(Zips, *FPaths::Combine(Dir, TEXT("*.zip")), true, false);
	for (const FString& Z : Zips) F.Delete(*FPaths::Combine(Dir, Z));
	F.Delete(*FPaths::Combine(Dir, TEXT("done")));
	F.Delete(*FPaths::Combine(Dir, TEXT("failed")));
	F.Delete(*FPaths::Combine(Dir, TEXT("swap.cmd")));
}

bool UOutlawsUpdate::InMenu() const
{
#if WITH_OUTLAWS_TESTS
	if (bTest) return Test.bFrontEnd;
#endif
	UWorld* W = GetTickableGameObjectWorld();
	if (!W) return false;
	TActorIterator<AOutlawsLevelActor> It(W);
	return It && It->IsFrontEnd();
}

// (the user 4 Nov: the main menu only, never in a game; the bubble is a VR thing, its buttons are the lasers')
bool UOutlawsUpdate::MayShow() const
{
#if WITH_OUTLAWS_TESTS
	if (bTest) return Test.bFrontEnd;
#endif
	if (!InMenu()) return false;
	UWorld* W = GetTickableGameObjectWorld();
	const AOutlawsPlayer* P = W ? Cast<AOutlawsPlayer>(UGameplayStatics::GetPlayerPawn(W, 0)) : nullptr;
	return P && P->IsVR() && !UOutlawsBeacon::BubbleShown(W);
}

void UOutlawsUpdate::Say(const FString& Text, const FString& Yes, const FString& No, const FString& Third)
{
	Words = Text;
	if (!InMenu()) return;
	UWorld* W = GetTickableGameObjectWorld();
	if (!W) return;
	if (!IsValid(Bubble) || Bubble->GetWorld() != W)
	{
		FActorSpawnParameters SP;
		SP.ObjectFlags |= RF_Transient;
		Bubble = W->SpawnActor<AOutlawsJoinBubble>(AOutlawsJoinBubble::StaticClass(), FTransform::Identity, SP);
		if (!Bubble) return;
		Bubble->OnPick = [this](int32 Button) { Pick(Button); };
	}
	Bubble->Show(EOutlawsBubbleKind::Update, Text, Yes, No, Third);
	UE_LOG(LogOutlaws, Log, TEXT("OLUPD bubble: \"%s\" [%s] [%s] [%s]"), *Text, *Yes, *No, *Third);
}

void UOutlawsUpdate::Hide()
{
	Words.Reset();
	bPromptUp = false;
	if (IsValid(Bubble)) Bubble->Show(EOutlawsBubbleKind::None, FString(), FString(), FString());
}

void UOutlawsUpdate::Fetch(const FString& Url, int64 From, int64 To, TFunction<void(int32 Code, const TArray<uint8>& Body)> Done)
{
#if WITH_OUTLAWS_TESTS
	if (bTest && !Test.Feed.IsEmpty())
	{
		// the release as a folder: the file by its last name, a Range as a slice of it, no folder as being offline
		const FString File = FPaths::Combine(Test.Feed, FPaths::GetCleanFilename(Url));
		TArray<uint8> All;
		if (!FPaths::DirectoryExists(Test.Feed)) { Done(0, All); return; }
		if (!FFileHelper::LoadFileToArray(All, *File, FILEREAD_Silent)) { Done(404, TArray<uint8>()); return; }
		if (To < 0) { Done(200, All); return; }
		const int64 End = FMath::Min<int64>(To + 1, All.Num());
		TArray<uint8> Slice(All.GetData() + From, int32(End - From));
		Done(206, Slice);
		return;
	}
#endif
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Rq = FHttpModule::Get().CreateRequest();
	Rq->SetURL(Url);
	Rq->SetVerb(TEXT("GET"));
	Rq->SetHeader(TEXT("User-Agent"), TEXT("OutlawsVR"));
	if (To >= 0) Rq->SetHeader(TEXT("Range"), FString::Printf(TEXT("bytes=%lld-%lld"), From, To));
	Rq->SetTimeout(To >= 0 ? 60.f : 10.f);
	TWeakObjectPtr<UOutlawsUpdate> Weak(this);
	Rq->OnProcessRequestComplete().BindLambda([Weak, Done](FHttpRequestPtr, FHttpResponsePtr Rs, bool bOk)
	{
		if (!Weak.IsValid()) return;
		if (bOk && Rs.IsValid()) Done(Rs->GetResponseCode(), Rs->GetContent());
		else Done(0, TArray<uint8>());
	});
	Rq->ProcessRequest();
}

void UOutlawsUpdate::BeginCheck()
{
	Step = EStep::Checking;
	Fetch(OLUpdate::FeedUrl(), 0, -1, [this](int32 Code, const TArray<uint8>& Body) { OnFeed(Code, Body); });
}

// Silent whatever goes wrong: the log says so, the player sees nothing (the user 1 Nov).
void UOutlawsUpdate::OnFeed(int32 Code, const TArray<uint8>& Body)
{
	if (Step != EStep::Checking) return;
	if (Code != 200 || Body.Num() == 0 || Body.Num() > MaxFeedBytes)
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLUPD no feed (status %d, %d bytes): nothing to say"), Code, Body.Num());
		Step = EStep::Done;
		return;
	}
	FUTF8ToTCHAR Text(reinterpret_cast<const ANSICHAR*>(Body.GetData()), Body.Num());
	OLUpdate::FLatest L;
	if (!OLUpdate::ParseLatest(FString(Text.Length(), Text.Get()), L))
	{
		UE_LOG(LogOutlaws, Log, TEXT("OLUPD the feed is not understood: nothing to say"));
		Step = EStep::Done;
		return;
	}
	int32 Mine = OLUpdate::MyBuild();
#if WITH_OUTLAWS_TESTS
	if (bTest) Mine = Test.Build;
#endif
	const TMap<FString, FString> S = Settings();
	const FString* SkipText = S.Find(SetSkip);
	const OLUpdate::EDecide D = OLUpdate::Decide(L, Mine, SkipText ? FCString::Atoi(**SkipText) : 0);
	UE_LOG(LogOutlaws, Log, TEXT("OLUPD the newest build is %d (%s), this one %d: %s"), L.Build, *L.Version, Mine,
		D == OLUpdate::EDecide::Prompt ? TEXT("offer it") : D == OLUpdate::EDecide::Skipped ? TEXT("skipped by the player") : TEXT("nothing to do"));
	Latest = L;
	Step = D == OLUpdate::EDecide::Prompt ? EStep::Found : EStep::Done;
}

void UOutlawsUpdate::Pick(int32 Button)
{
	switch (Step)
	{
	case EStep::Prompt:
		if (Button == 0) { BeginDownload(); return; }
		if (Button == 2)
		{
			WriteSetting(SetSkip, FString::FromInt(Latest.Build));
			UE_LOG(LogOutlaws, Log, TEXT("OLUPD build %d skipped"), Latest.Build);
		}
		Hide();
		Step = EStep::Done;
		return;
	case EStep::Downloading:
	case EStep::Allow:
	case EStep::Failed:
		if (Button == 1) { Hide(); Step = EStep::Done; }      // (a stopped download goes on from there next time)
		return;
	default:
		return;
	}
}

void UOutlawsUpdate::BeginDownload()
{
	IFileManager& F = IFileManager::Get();
	WorkDir = bQuest ? FPaths::Combine(RootDir(), TEXT("OutlawsUpdate"), TEXT("download")) : FPaths::Combine(RootDir(), TEXT("Update"));
#if PLATFORM_ANDROID
	WorkDir = FPaths::Combine(FPaths::ProjectPersistentDownloadDir(), TEXT("OutlawsUpdate"), TEXT("download"));
#endif
	const FString Try = FPaths::Combine(WorkDir, TEXT("write.test"));
	if (!F.MakeDirectory(*WorkDir, true) || !FFileHelper::SaveStringToFile(TEXT("x"), *Try))
	{
		Fail(TEXT("the folder is not writable"), TEXT("The game's folder cannot be written. Get the new build from the GitHub page."));
		return;
	}
	F.Delete(*Try);
	ZipFile = FPaths::Combine(WorkDir, Latest.ZipSha256 + TEXT(".zip"));
	Part = ZipFile + TEXT(".part");
	bPromptUp = false;
	if (F.FileSize(*ZipFile) == Latest.ZipSize) { BeginWork(); return; }
	const int64 Size = F.FileSize(*Part);
	Have = Size > 0 && Size < Latest.ZipSize ? Size : 0;
	if (Have == 0) F.Delete(*Part);
	Retries = 0;
	Step = EStep::Downloading;
	Say(TEXT("Downloading the update."), FString(), TEXT("X"));
	NextChunk();
}

void UOutlawsUpdate::NextChunk()
{
	NextAt = 0.0;
	const int64 To = FMath::Min(Have + ChunkBytes, Latest.ZipSize) - 1;
	Fetch(OLUpdate::ZipUrl(Latest), Have, To, [this](int32 Code, const TArray<uint8>& Body) { OnChunk(Code, Body); });
}

void UOutlawsUpdate::OnChunk(int32 Code, const TArray<uint8>& Body)
{
	if (Step != EStep::Downloading) return;
	const int64 Want = FMath::Min(ChunkBytes, Latest.ZipSize - Have);
	if ((Code != 206 && Code != 200) || Body.Num() != Want)
	{
		// the whole zip for a Range refused, on a first chunk, would fit nothing here: a miss as any other
		if (++Retries > MaxRetries)
		{
			Fail(FString::Printf(TEXT("download stopped at %lld of %lld (status %d, %d bytes)"), Have, Latest.ZipSize, Code, Body.Num()),
				TEXT("The update could not be downloaded. Your game is as it was."));
			return;
		}
		NextAt = FPlatformTime::Seconds() + 2.0 * Retries;
		return;
	}
	TUniquePtr<FArchive> W(IFileManager::Get().CreateFileWriter(*Part, FILEWRITE_Append | FILEWRITE_Silent));
	if (!W)
	{
		Fail(TEXT("the part file cannot be written"), TEXT("The update could not be saved. Your game is as it was."));
		return;
	}
	W->Serialize(const_cast<uint8*>(Body.GetData()), Body.Num());
	const bool bOk = W->Close();
	if (!bOk)
	{
		Fail(TEXT("the part file cannot be written"), TEXT("The update could not be saved. There may be no room left."));
		return;
	}
	Have += Body.Num();
	Retries = 0;
	if (Have >= Latest.ZipSize)
	{
		IFileManager::Get().Move(*ZipFile, *Part, true, true);
		BeginWork();
		return;
	}
	Say(FString::Printf(TEXT("Downloading the update. %d%%"), int32(Have * 100 / Latest.ZipSize)), FString(), TEXT("X"));
	NextChunk();
}

void UOutlawsUpdate::BeginWork()
{
	Step = EStep::Working;
	Say(TEXT("Checking the update..."));
	const FString Zip = ZipFile, Dir = WorkDir, Hash = Latest.ZipSha256, ApkHash = Latest.ApkSha256;
	const int32 Build = Latest.Build;
	const bool bForQuest = bQuest;
	Work = Async(EAsyncExecution::Thread, [Zip, Dir, Hash, ApkHash, Build, bForQuest]() -> FString
	{
		IFileManager& F = IFileManager::Get();
		if (OLUpdate::Sha256OfFile(Zip) != Hash)
		{
			F.Delete(*Zip);
			return TEXT("the zip's hash is not the one in latest.json");
		}
		FString Why;
		if (!bForQuest)
		{
			return OLUpdate::ExtractForPc(Zip, FPaths::Combine(Dir, TEXT("new")), Why) ? FString() : Why;
		}
		// (a folder the app makes itself: the data Install on Quest pushed stays as it is)
		const FString Base = FPaths::GetPath(Dir), Data = FPaths::Combine(Base, TEXT("data")), Fresh = Data + TEXT(".new");
		if (!OLUpdate::ExtractForQuest(Zip, Fresh, FPaths::Combine(Dir, TEXT("OutlawsVR.apk")), ApkHash, Why)) return Why;
		F.Delete(*FPaths::Combine(Base, TEXT("build.txt")));
		F.DeleteDirectory(*Data, false, true);
		if (!F.Move(*Data, *Fresh, true, true)) return TEXT("could not put the new data in place");
		FFileHelper::SaveStringToFile(FString::FromInt(Build), *FPaths::Combine(Base, TEXT("build.txt")));
		return FString();
	});
}

void UOutlawsUpdate::AfterWork(const FString& Why)
{
	if (!Why.IsEmpty())
	{
		Fail(Why, TEXT("The update did not check out. Your game is as it was."));
		return;
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLUPD build %d is checked and unpacked"), Latest.Build);
	Step = EStep::Ready;
}

void UOutlawsUpdate::Finish()
{
#if WITH_OUTLAWS_TESTS
	const bool bStop = bTest;
#else
	const bool bStop = false;
#endif
	if (bQuest)
	{
		Say(TEXT("Installing. Confirm in the window that opens."));
		if (bStop) { Step = EStep::Exiting; return; }
#if PLATFORM_ANDROID
		if (!JavaBool("AndroidThunkJava_OLUpdate_CanInstall"))
		{
			if (Allowed == 0)
			{
				Allowed = 1;
				JavaVoid("AndroidThunkJava_OLUpdate_OpenAllow");
			}
			Say(TEXT("Allow installs for Outlaws VR. This is asked once."), FString(), TEXT("X"));
			Step = EStep::Allow;
			NextAt = FPlatformTime::Seconds() + 1.0;
			return;
		}
		JavaInstall(FPaths::ConvertRelativePathToFull(FPaths::Combine(WorkDir, TEXT("OutlawsVR.apk"))));
		Step = EStep::Installing;
		NextAt = FPlatformTime::Seconds() + 1.0;
#endif
		return;
	}
	FString Script = OLUpdate::SwapScript();
	Script.ReplaceInline(TEXT("\n"), TEXT("\r\n"));
	const FString Path = FPaths::Combine(WorkDir, TEXT("swap.cmd"));
	if (!FFileHelper::SaveStringToFile(Script, *Path, FFileHelper::EEncodingOptions::ForceAnsi))
	{
		Fail(TEXT("swap.cmd cannot be written"), TEXT("The update could not be saved. Your game is as it was."));
		return;
	}
	Step = EStep::Exiting;
	Say(TEXT("Updating. The game restarts by itself."));
	if (bStop) return;
#if PLATFORM_WINDOWS
	FString Native = Path;
	FPaths::MakePlatformFilename(Native);
	const FString Args = FString::Printf(TEXT("/c \"\"%s\" %u\""), *Native, FPlatformProcess::GetCurrentProcessId());
	FProcHandle Proc = FPlatformProcess::CreateProc(TEXT("cmd.exe"), *Args, true, true, true, nullptr, 0, *FPaths::GetPath(Path), nullptr);
	if (!Proc.IsValid())
	{
		Fail(TEXT("swap.cmd did not start"), TEXT("The update could not start. Your game is as it was."));
		return;
	}
	FPlatformProcess::CloseProc(Proc);
	UE_LOG(LogOutlaws, Log, TEXT("OLUPD handing over to swap.cmd, the game closes"));
	FPlatformMisc::RequestExit(false);
#endif
}

void UOutlawsUpdate::Fail(const FString& Why, const FString& Text)
{
	UE_LOG(LogOutlaws, Warning, TEXT("OLUPD failed: %s"), *Why);
	WhyFailed = Why;
	Step = EStep::Failed;
	bPromptUp = false;
	Say(Text, FString(), TEXT("X"));
}

void UOutlawsUpdate::Tick(float DeltaTime)
{
	const double Now = FPlatformTime::Seconds();
	// (never in a game: what is under way goes on unseen, the bubble is back on the main menu)
	const bool bMenu = InMenu();
	if (!bMenu && IsValid(Bubble) && Bubble->GetKind() != EOutlawsBubbleKind::None)
	{
		const FString Keep = Words;
		Bubble->Show(EOutlawsBubbleKind::None, FString(), FString(), FString());
		Words = Keep;
		if (Step == EStep::Prompt) { bPromptUp = false; Step = EStep::Found; }
	}
	switch (Step)
	{
	case EStep::Wait:
		if (Now >= NextAt) BeginCheck();
		break;
	case EStep::Found:
		if (MayShow())
		{
			Step = EStep::Prompt;
			bPromptUp = true;
			Say(FString::Printf(TEXT("A new version is out: Outlaws VR %s. Your saves are kept."), *Latest.Version), TEXT("Update now"), TEXT("Not now"), TEXT("Skip this build"));
		}
		break;
	case EStep::Downloading:
		if (NextAt > 0.0 && Now >= NextAt) NextChunk();
		break;
	case EStep::Working:
		if (Work.IsValid() && Work.IsReady())
		{
			const FString Why = Work.Get();
			Work = TFuture<FString>();
			AfterWork(Why);
		}
		break;
	case EStep::Ready:
		if (bMenu) Finish();
		break;
#if PLATFORM_ANDROID
	case EStep::Allow:
		if (Now >= NextAt)
		{
			NextAt = Now + 1.0;
			if (JavaBool("AndroidThunkJava_OLUpdate_CanInstall")) Finish();
		}
		break;
	case EStep::Installing:
		if (Now >= NextAt)
		{
			NextAt = Now + 1.0;
			const int32 Status = JavaInt("AndroidThunkJava_OLUpdate_Status");
			// (0 is success, and the app is replaced under us: MY_PACKAGE_REPLACED starts the new one; -1 pending, 1000 being written)
			if (Status > 0 && Status != 1000) Fail(FString::Printf(TEXT("the installer said %d"), Status), TEXT("The update did not install. Your game is as it was."));
		}
		break;
#endif
	default:
		break;
	}
}

#if WITH_OUTLAWS_TESTS
void UOutlawsUpdate::TestStart(const FTestRun& Run)
{
	Hide();
	bTest = true;
	Test = Run;
	bQuest = Run.bQuest;
	WhyFailed.Reset();
	Latest = OLUpdate::FLatest();
	const TMap<FString, FString> S = Settings();
	const FString* X = S.Find(SetCheck);
	if (Run.bOld || (X && !X->ToBool()))
	{
		Step = EStep::Off;
		return;
	}
	BeginCheck();
}

void UOutlawsUpdate::TestStop()
{
	if (Work.IsValid()) Work.Wait();
	Hide();
	if (IsValid(Bubble)) { Bubble->Destroy(); Bubble = nullptr; }
	bTest = false;
	Step = EStep::Off;
}
#endif
