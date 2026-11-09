#include "OutlawsLanguage.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"         // the settings file (Language=)
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <ShlObj.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

// (REQ-LANGUAGE, user 9 Nov) see OutlawsLanguage.h.

namespace
{
	struct FLangRow
	{
		const TCHAR* Code;
		const TCHAR* LocName;       // loc_<name>.txt in Common.kpf
		const TCHAR* TextLab;       // the LAB with its LOCAL.MSG, intro.txt and item NWX (plan 2.2)
	};
	const FLangRow GRows[] = {
		{ TEXT("en"), TEXT("english"), nullptr },
		{ TEXT("fr"), TEXT("french"), TEXT("oliptch2") },
		{ TEXT("de"), TEXT("german"), TEXT("olintl") },
		{ TEXT("it"), TEXT("italian"), TEXT("oliptch2") },
		{ TEXT("es"), TEXT("spanish"), TEXT("olintl") },
		{ TEXT("pt-BR"), TEXT("portuguese_br"), TEXT("oliptch2") },
	};

	const FLangRow* Row(const FString& Code)
	{
		for (const FLangRow& R : GRows) if (Code.Equals(R.Code, ESearchCase::IgnoreCase)) return &R;
		return nullptr;
	}

	// the run's choice: filled on first use, changed by Set
	bool GChosen = false;
	FString GCode;
	FString GSource;
#if WITH_OUTLAWS_TESTS
	FString GTestCode;
	int32 GTestOld = -1;
#endif

	void Choose()
	{
		if (GChosen) return;
		GChosen = true;
		const TMap<FString, FString> Settings = AOutlawsLevelActor::ReadSettings();
		if (const FString* Set = Settings.Find(TEXT("Language")))
		{
			const FString N = OLLanguage::Normalize(Set->TrimQuotes());
			if (!N.IsEmpty()) { GCode = N; GSource = TEXT("settings"); }
			else UE_LOG(LogOutlaws, Log, TEXT("OLRT language: Language=%s in the settings is none of the six, read as unset"), **Set);
		}
		if (GCode.IsEmpty())
		{
			// the first start (user 9 Nov): the player's remaster language, so the game opens in the language they play it in
			FString File;
			const FString Kex = OLLanguage::RemasterLanguage(&File);
			if (!Kex.IsEmpty()) { GCode = Kex; GSource = TEXT("remaster"); }
		}
		if (GCode.IsEmpty()) { GCode = TEXT("en"); GSource = TEXT("default"); }
	}
}

const TArray<FString>& OLLanguage::Codes()
{
	static const TArray<FString> All = [] { TArray<FString> A; for (const FLangRow& R : GRows) A.Add(R.Code); return A; }();
	return All;
}

FString OLLanguage::Normalize(const FString& Code)
{
	const FLangRow* R = Row(Code.TrimStartAndEnd());
	return R ? FString(R->Code) : FString();
}

bool OLLanguage::Old()
{
#if WITH_OUTLAWS_TESTS
	if (GTestOld >= 0) return GTestOld != 0;
#endif
	static const bool bOld = OLFlags::Param(TEXT("OLOldLanguage"));
	return bOld;
}

FString OLLanguage::Current()
{
	if (Old()) return TEXT("en");
#if WITH_OUTLAWS_TESTS
	if (!GTestCode.IsEmpty()) return GTestCode;
#endif
	// -OLLanguage=<code>: this run only (tests, a tester's quick look); an unknown code is said once and left out
	static const FString Forced = []
	{
		FString V;
		if (!OLFlags::Value(TEXT("OLLanguage"), V)) return FString();
		const FString N = Normalize(V.TrimQuotes());
		if (N.IsEmpty()) UE_LOG(LogOutlaws, Warning, TEXT("OLRT language: -OLLanguage=%s is none of en fr de it es pt-BR, ignored"), *V);
		return N;
	}();
	if (!Forced.IsEmpty()) return Forced;
	Choose();
	return GCode;
}

FString OLLanguage::Source()
{
	if (Old()) return TEXT("old");
#if WITH_OUTLAWS_TESTS
	if (!GTestCode.IsEmpty()) return TEXT("test");
#endif
	FString V;
	if (OLFlags::Value(TEXT("OLLanguage"), V) && !Normalize(V.TrimQuotes()).IsEmpty()) return TEXT("flag");
	Choose();
	return GSource;
}

bool OLLanguage::Set(const FString& Code, bool bSave)
{
	const FString N = Normalize(Code);
	if (N.IsEmpty()) return false;
	Choose();
	GCode = N;
	GSource = TEXT("settings");
	if (bSave)
	{
		TMap<FString, FString> Settings = AOutlawsLevelActor::ReadSettings();
		Settings.Add(TEXT("Language"), N);
		AOutlawsLevelActor::WriteSettings(Settings);
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT language: set to %s%s (applies at the next level start)"), *N, bSave ? TEXT(", saved") : TEXT(""));
	return true;
}

FString OLLanguage::LocFile(const FString& Code)
{
	const FLangRow* R = Row(Code);
	return FString::Printf(TEXT("localization/loc_%s.txt"), R ? R->LocName : TEXT("english"));
}

TArray<FString> OLLanguage::LabsFor(const FString& Code)
{
	TArray<FString> Out;
	const FLangRow* R = Row(Code);
	if (!R || !R->TextLab) return Out;
	Out.Add(FString::Printf(TEXT("olpatch4_%s.lab"), R->Code));        // interfac.nwx (the 2.0 patch's)
	Out.Add(FString::Printf(TEXT("olremaster1_%s.lab"), R->Code));     // the HD item art (SPX), intro and credits text
	Out.Add(FString::Printf(TEXT("%s_%s.lab"), R->TextLab, R->Code));  // LOCAL.MSG, intro.txt, the item NWX
	return Out;
}

FString OLLanguage::ParseKexLanguage(const FString& CfgText)
{
	// seta g_language "de"   (the last one wins, as the remaster reads the file top to bottom)
	FString Found;
	TArray<FString> Lines;
	CfgText.ParseIntoArrayLines(Lines);
	for (const FString& Line : Lines)
	{
		TArray<FString> W;
		Line.TrimStartAndEnd().ParseIntoArrayWS(W);
		if (W.Num() < 3 || !W[1].Equals(TEXT("g_language"), ESearchCase::IgnoreCase)) continue;
		if (!W[0].Equals(TEXT("seta"), ESearchCase::IgnoreCase) && !W[0].Equals(TEXT("set"), ESearchCase::IgnoreCase)) continue;
		const FString N = Normalize(W[2].TrimQuotes());
		if (!N.IsEmpty()) Found = N;
	}
	return Found;
}

FString OLLanguage::RemasterLanguage(FString* OutFile)
{
#if PLATFORM_WINDOWS
	// READ 9 Nov: the GOG remaster keeps it in the Saved Games known folder, Nightdive Studios\Outlaws EX\kexengine.cfg
	// ("seta g_language "en""), found through the shell's known folder, never a path of its own; the AppData\Roaming one
	// is where Nightdive's Blood EX keeps its file (GUESS for Outlaws)
	FString SavedGames;
	{
		PWSTR Path = nullptr;
		if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_SavedGames, 0, nullptr, &Path)) && Path) SavedGames = Path;
		if (Path) ::CoTaskMemFree(Path);
	}
	const FString AppData = FPlatformMisc::GetEnvironmentVariable(TEXT("APPDATA"));
	TArray<FString> Tries;
	if (!SavedGames.IsEmpty()) Tries.Add(FPaths::Combine(SavedGames, TEXT("Nightdive Studios"), TEXT("Outlaws EX"), TEXT("kexengine.cfg")));
	if (!AppData.IsEmpty()) Tries.Add(FPaths::Combine(AppData, TEXT("Nightdive Studios"), TEXT("Outlaws EX"), TEXT("kexengine.cfg")));
	for (const FString& T : Tries)
	{
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *T)) continue;
		const FString N = ParseKexLanguage(Text);
		if (N.IsEmpty()) continue;
		if (OutFile) *OutFile = T;
		static bool bSaid = false;
		if (!bSaid) { bSaid = true; UE_LOG(LogOutlaws, Log, TEXT("OLRT language: the remaster's g_language is %s (its kexengine.cfg)"), *N); }
		return N;
	}
#endif
	return FString();
}

void OLLanguage::ParseLocalMsg(const FString& Text, TMap<int32, FString>& Out)
{
	// (moved from AOutlawsLevelActor::LoadGameTables) id, priority, the quoted text
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	for (const FString& Line : Lines)
	{
		const FString T = Line.TrimStart();
		int32 Q0, Q1;
		if (T.IsEmpty() || !FChar::IsDigit(T[0]) || !T.FindChar(TEXT('"'), Q0) || !T.FindLastChar(TEXT('"'), Q1) || Q1 <= Q0) continue;
		Out.Add(FCString::Atoi(*T), T.Mid(Q0 + 1, Q1 - Q0 - 1));
	}
}

void OLLanguage::ParseLoc(const FString& Text, TMap<FString, FString>& Out)
{
	// (moved from FSource::Load) key = "value" lines; /// and // lines are comments
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	for (const FString& Line : Lines)
	{
		int32 Eq, Q0, Q1;
		if (!Line.FindChar(TEXT('='), Eq) || !Line.FindChar(TEXT('"'), Q0) || !Line.FindLastChar(TEXT('"'), Q1) || Q1 <= Q0 || Q0 < Eq) continue;
		const FString Key = Line.Left(Eq).TrimStartAndEnd();
		if (Key.IsEmpty() || Key.StartsWith(TEXT("/"))) continue;
		Out.Add(Key.ToLower(), Line.Mid(Q0 + 1, Q1 - Q0 - 1).Replace(TEXT("\\n"), TEXT(" ")));
	}
}

#if WITH_OUTLAWS_TESTS
void OLLanguage::TestSet(const FString& Code)
{
	GTestCode = Normalize(Code);
}

void OLLanguage::TestSetOld(int32 bOld)
{
	GTestOld = bOld;
}
#endif
