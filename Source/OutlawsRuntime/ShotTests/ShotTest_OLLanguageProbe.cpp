// -OLShots -OLLanguageProbe (REQ-LANGUAGE, 9 Nov; the user: six languages, a language's files in front of the stock ones, a line a
// language lacks reads English).  In a story level, one level start per language (en fr de it es pt-BR, each by the test's own
// language, never the settings file), then one with the old rule (-OLOldLanguage's stand-in, the language set to German):
//   order:  the mounted order begins with that language's LABs (OLLanguage::LabsFor, those the install has), then the stock ones
//   where:  local.msg resolves to the language's text LAB (English: a stock LAB)
//   801:    the level's message 801 ("You need the brass key.") is the line read straight from that LAB's own local.msg (and is
//           not the English line)
//   837:    a line German and Spanish lack (the Handful of Missions' cave-in line): not in their file, the English line in the level
//   menu:   a menu source loaded now reads $m_option_language_UC as the language's loc file has it, and $m_vault_restart_video
//           (in English's file only) as English
//   old:    English, no language LAB mounted, local.msg from a stock LAB, message 801 and the menu in English
// plus the first start's reading of a kexengine.cfg line.  Lines "OLSHOT language <lang> <check>: <got> want <want> PASS|FAIL", then
// "OLSHOT language RESULT ALL PASS (n passed, 0 failed)" or "OLSHOT language RESULT FAIL (n passed, m failed)".
// -OLLanguageProbe=flag: no switching, the first level start only, for a run given -OLLanguage=<code> (its order, where, 801 and
// the source "flag").  A run with -OLOldLanguage must FAIL (every language reads English).

#include "ShotTestCommon.h"
#include "OutlawsLanguage.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLLanguageProbe) { return OLFlags::Has(TEXT("OLLanguageProbe")); }

namespace
{
	int32 GLangPass = 0, GLangFail = 0;
	bool GLangBuilt = false;
	double GLangBuiltAt = 0.0;

	void LangCheck(const FString& Lang, const TCHAR* What, bool bOk, const FString& Got, const FString& Want)
	{
		(bOk ? GLangPass : GLangFail)++;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT language %s %s: %s want %s %s"), *Lang, What, *Got.Left(90), *Want.Left(90), bOk ? TEXT("PASS") : TEXT("FAIL"));
	}

	// a LAB's own local.msg, read from the file itself (not through the mounted order)
	bool LabMessages(const FString& Root, const FString& Lab, TMap<int32, FString>& Out)
	{
		for (const FString& P : { FPaths::Combine(Root, Lab), FPaths::Combine(Root, TEXT("assets"), Lab) })
		{
			if (!FPaths::FileExists(P)) continue;
			FOutlawsLabFile File;
			if (!File.Open(P)) return false;
			const int32* E = File.GetIndex().Find(TEXT("local.msg"));
			TArray<uint8> Bytes;
			if (!E || !File.Read(*E, Bytes)) return false;
			FString Text;
			for (uint8 B : Bytes) Text.AppendChar(TCHAR(B));      // latin-1, as GetText
			OLLanguage::ParseLocalMsg(Text, Out);
			return true;
		}
		return false;
	}

	bool InstallHas(const FString& Root, const FString& Lab)
	{
		return FPaths::FileExists(FPaths::Combine(Root, Lab)) || FPaths::FileExists(FPaths::Combine(Root, TEXT("assets"), Lab));
	}

	// one level start's checks; Lang = the language it should be in, bOld = the old rule
	void CheckLanguage(AOutlawsLevelActor* L, const FString& Lang, bool bOld, bool bFlag)
	{
		const FOutlawsAssets* A = L->GetAssets();
		if (!A) { LangCheck(Lang, TEXT("assets"), false, TEXT("none"), TEXT("open")); return; }
		const FString Root = A->GetRoot();
		const FString Tag = bOld ? TEXT("old(") + Lang + TEXT(")") : Lang;
		const FString Want = bOld ? FString(TEXT("en")) : Lang;
		LangCheck(Tag, TEXT("mounted language"), A->GetLanguage() == Want, A->GetLanguage(), Want);
		if (bFlag) LangCheck(Tag, TEXT("source"), OLLanguage::Source() == TEXT("flag"), OLLanguage::Source(), TEXT("flag"));

		// the order: the language's LABs first (those in the install), then the stock order's
		TArray<FString> Expect;
		for (const FString& Lab : OLLanguage::LabsFor(Want)) if (InstallHas(Root, Lab)) Expect.Add(Lab);
		const TArray<FString> Order = A->MountedOrder();
		bool bOrder = Order.Num() > Expect.Num();
		for (int32 i = 0; bOrder && i < Expect.Num(); ++i) bOrder = Order[i].Equals(Expect[i], ESearchCase::IgnoreCase);
		if (bOrder) bOrder = !Order[Expect.Num()].Contains(TEXT("_"));       // the next is a stock LAB (no language suffix)
		int32 LangMounted = 0;
		for (const FString& O : Order) if (O.Contains(TEXT("_"))) ++LangMounted;
		bOrder = bOrder && LangMounted == Expect.Num() && (Want != TEXT("en") ? Expect.Num() > 0 : true);
		TArray<FString> Head;
		for (int32 i = 0; i < Order.Num() && i < Expect.Num() + 1; ++i) Head.Add(Order[i]);
		Expect.Add(TEXT("<stock>"));
		LangCheck(Tag, TEXT("order"), bOrder, FString::Join(Head, TEXT(",")), FString::Join(Expect, TEXT(",")));
		Expect.Pop();

		// where local.msg comes from, and message 801 against the file's own line
		const FString Where = A->Where(TEXT("local.msg"));
		const TArray<FString> LangLabs = OLLanguage::LabsFor(Want);
		const FString TextLab = LangLabs.Num() ? LangLabs.Last() : FString();
		LangCheck(Tag, TEXT("where local.msg"), Want == TEXT("en") ? !Where.Contains(TEXT("_")) && !Where.IsEmpty() : Where.Equals(TextLab, ESearchCase::IgnoreCase), Where,
			Want == TEXT("en") ? FString(TEXT("<a stock LAB>")) : TextLab);
		TMap<int32, FString> English, File;
		LabMessages(Root, TEXT("olpatch4.lab"), English);
		if (!TextLab.IsEmpty()) LabMessages(Root, TextLab, File);
		const FString Got801 = L->UserMessageOr(801, TEXT("<none>"));
		const FString* Want801 = (Want == TEXT("en") ? English : File).Find(801);
		LangCheck(Tag, TEXT("801"), Want801 && Got801 == *Want801, Got801, Want801 ? *Want801 : FString(TEXT("<not in the file>")));
		if (Want != TEXT("en"))
		{
			const FString* En801 = English.Find(801);
			LangCheck(Tag, TEXT("801 not English"), En801 && Got801 != *En801, Got801, TEXT("not ") + (En801 ? *En801 : FString()));
		}
		// 837: German and Spanish lack it, so it reads English; the others have their own
		const FString Got837 = L->UserMessageOr(837, TEXT("<none>"));
		const FString* En837 = English.Find(837);
		if (Want == TEXT("de") || Want == TEXT("es"))
		{
			LangCheck(Tag, TEXT("837 not in its file"), !File.Contains(837), File.Contains(837) ? File[837] : FString(TEXT("<absent>")), TEXT("<absent>"));
			LangCheck(Tag, TEXT("837 falls back to English"), En837 && Got837 == *En837, Got837, En837 ? *En837 : FString(TEXT("<none>")));
		}
		else
		{
			const FString* W837 = (Want == TEXT("en") ? English : File).Find(837);
			LangCheck(Tag, TEXT("837"), W837 && Got837 == *W837, Got837, W837 ? *W837 : FString(TEXT("<none>")));
		}

		// the menu strings: a source loaded now (as a player's first menu loads it)
		OutlawsMenu::FSource Src;
		if (!Src.Load(Root)) { LangCheck(Tag, TEXT("menu load"), false, TEXT("no Common.kpf"), Root); return; }
		LangCheck(Tag, TEXT("menu language"), Src.GetLanguage() == Want, Src.GetLanguage(), Want);
		TMap<FString, FString> LocEn, LocLang;
		FString Text;
		if (Src.ReadText(OLLanguage::LocFile(TEXT("en")), Text)) OLLanguage::ParseLoc(Text, LocEn);
		if (Src.ReadText(OLLanguage::LocFile(Want), Text)) OLLanguage::ParseLoc(Text, LocLang);
		const FString Title = Src.Loc(TEXT("$m_option_language_UC"));
		const FString* WantTitle = LocLang.Find(TEXT("m_option_language_uc"));
		LangCheck(Tag, TEXT("menu m_option_language_UC"), WantTitle && Title == *WantTitle, Title, WantTitle ? *WantTitle : FString(TEXT("<not in the file>")));
		if (Want != TEXT("en"))
		{
			const FString Vault = Src.Loc(TEXT("$m_vault_restart_video"));
			const FString* EnVault = LocEn.Find(TEXT("m_vault_restart_video"));
			LangCheck(Tag, TEXT("menu missing key reads English"), !LocLang.Contains(TEXT("m_vault_restart_video")) && EnVault && Vault == *EnVault, Vault, EnVault ? *EnVault : FString(TEXT("<none>")));
		}
	}
}

OL_SHOT_TEST(OLLanguageProbe, 2588)
{
	static int32 Stage = -1;                  // -1 the first build, 0..5 the six languages, 6 the old rule, 7 done
	static double Wait = 0.0;
	if (Stage >= 7 || !L || ShotTime < 6.f) return;
	const TArray<FString>& Codes = OLLanguage::Codes();
	FString Mode;
	OLFlags::Value(TEXT("OLLanguageProbe"), Mode);
	const bool bFlag = Mode.Equals(TEXT("flag"), ESearchCase::IgnoreCase);
	auto Finish = [&]()
	{
		OLLanguage::TestSet(FString());
		OLLanguage::TestSetOld(-1);
		Stage = 7;
		if (GLangFail == 0 && GLangPass > 0) { UE_LOG(LogOutlaws, Display, TEXT("OLSHOT language RESULT ALL PASS (%d passed, 0 failed)"), GLangPass); }
		else { UE_LOG(LogOutlaws, Display, TEXT("OLSHOT language RESULT FAIL (%d passed, %d failed)"), GLangPass, GLangFail); }
		FGenericPlatformMisc::RequestExit(false);
	};
	auto StartLevel = [&](int32 Next)
	{
		Stage = Next;
		GLangBuilt = false;
		if (Next < 6) { OLLanguage::TestSetOld(-1); OLLanguage::TestSet(Codes[Next]); }
		else { OLLanguage::TestSetOld(1); OLLanguage::TestSet(TEXT("de")); }
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT language start %s"), Next < 6 ? *Codes[Next] : TEXT("old (de asked)"));
		L->RestartLevelThen([]() { GLangBuilt = true; GLangBuiltAt = FPlatformTime::Seconds(); });
	};
	if (Stage == -1)
	{
		// the first start's reading of the remaster's setting (no file written or changed)
		struct FCase { const TCHAR* Cfg; const TCHAR* Want; };
		const FCase KexCases[] = { { TEXT("seta g_language \"de\"\n"), TEXT("de") }, { TEXT("seta snd_voicelanguage \"0\"\nseta g_language \"pt-BR\"\n"), TEXT("pt-BR") },
			{ TEXT("seta g_language \"xx\"\n"), TEXT("") }, { TEXT("seta g_language \"fr\"\nseta g_language \"IT\"\n"), TEXT("it") } };
		for (const FCase& C : KexCases)
		{
			const FString Got = OLLanguage::ParseKexLanguage(C.Cfg);
			LangCheck(TEXT("firststart"), TEXT("kexengine line"), Got == C.Want, Got.IsEmpty() ? FString(TEXT("<none>")) : Got, *C.Want ? C.Want : TEXT("<none>"));
		}
		FString CfgFile;
		const FString Kex = OLLanguage::RemasterLanguage(&CfgFile);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT language remaster setting: %s (%s); this run %s from %s"), Kex.IsEmpty() ? TEXT("<none>") : *Kex,
			CfgFile.IsEmpty() ? TEXT("no kexengine.cfg") : TEXT("the remaster's kexengine.cfg"), *OLLanguage::Current(), *OLLanguage::Source());
		if (bFlag)
		{
			FString Asked;
			OLFlags::Value(TEXT("OLLanguage"), Asked);
			CheckLanguage(L, OLLanguage::Normalize(Asked).IsEmpty() ? FString(TEXT("en")) : OLLanguage::Normalize(Asked), OLLanguage::Old(), true);
			Finish();
			return;
		}
		StartLevel(0);
		return;
	}
	if (!GLangBuilt)
	{
		if (Wait == 0.0) Wait = FPlatformTime::Seconds();
		if (FPlatformTime::Seconds() - Wait > 90.0) { LangCheck(Stage < 6 ? Codes[Stage] : FString(TEXT("old")), TEXT("level start"), false, TEXT("no build in 90 s"), TEXT("built")); Finish(); }
		return;
	}
	Wait = 0.0;
	if (FPlatformTime::Seconds() - GLangBuiltAt < 1.0) return;
	CheckLanguage(L, Stage < 6 ? Codes[Stage] : FString(TEXT("de")), Stage == 6, false);
	if (Stage < 6) StartLevel(Stage + 1);
	else Finish();
}

#endif // WITH_OUTLAWS_TESTS
