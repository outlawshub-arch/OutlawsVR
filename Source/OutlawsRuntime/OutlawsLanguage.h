#pragma once

#include "CoreMinimal.h"

/**
 * (REQ-LANGUAGE, user 9 Nov) the game's language: English, French, German, Italian, Spanish or Brazilian Portuguese, as the
 * remaster ships them (its options_language.rml codes en fr de it es pt-BR).  The language picks which of the install's
 * language LABs open in front of the stock order (FOutlawsAssets::UseLanguage, at each level start) and which loc file the
 * menus read over loc_english.txt; the game's messages (LOCAL.MSG) and the menu strings are merged line by line, so a line
 * a language lacks reads English.
 *
 * Which language: -OLOldLanguage = English and no language file at all (today's behaviour); else -OLLanguage=<code> (this
 * run only, never saved); else Language= in OutlawsSettings.ini; else (the first start, on a PC) the remaster's own
 * g_language from its kexengine.cfg; else English.  A change applies at the next level start.
 */
namespace OLLanguage
{
	/** The six codes, in the remaster's page order: en fr de it es pt-BR. */
	OUTLAWSRUNTIME_API const TArray<FString>& Codes();
	/** The code as the table spells it ("PT-br" -> "pt-BR"), empty when it is none of the six. */
	OUTLAWSRUNTIME_API FString Normalize(const FString& Code);
	/** -OLOldLanguage (or the probe's stand-in for it): English, no language LAB, loc_english only. */
	OUTLAWSRUNTIME_API bool Old();
	/** The language in force now (see above); always one of Codes(). */
	OUTLAWSRUNTIME_API FString Current();
	/** Where Current came from: "old", "test", "flag", "settings", "remaster", "default". */
	OUTLAWSRUNTIME_API FString Source();
	/** Pick a language (the Options page's later step): kept for this run and, with bSave, written as Language= into
	 *  OutlawsSettings.ini.  It applies at the next level start.  An unknown code is ignored (false). */
	OUTLAWSRUNTIME_API bool Set(const FString& Code, bool bSave);
	/** The menu strings' file in Common.kpf: localization/loc_<english|french|german|italian|spanish|portuguese_br>.txt. */
	OUTLAWSRUNTIME_API FString LocFile(const FString& Code);
	/** The language's LABs, the one to win first (the install's table, REQ-LANGUAGE plan 2.2): olpatch4_<l>,
	 *  olremaster1_<l>, then its text LAB (olintl_<l> for de and es, oliptch2_<l> for fr, it and pt-BR; pt-BR's olintl and
	 *  oliptch1 hold nothing oliptch2 lacks).  Its taunts (oltaunt_<l>) are not here: the voice is a later step.  None for en. */
	OUTLAWSRUNTIME_API TArray<FString> LabsFor(const FString& Code);
	/** The remaster's language on this PC (kexengine.cfg "seta g_language"), normalised; empty when not found or off Windows. */
	OUTLAWSRUNTIME_API FString RemasterLanguage(FString* OutFile = nullptr);
	/** The g_language value in a kexengine.cfg's text, normalised; empty when none. */
	OUTLAWSRUNTIME_API FString ParseKexLanguage(const FString& CfgText);
	/** LOCAL.MSG lines into Out by id (801  5:  "You need the brass key."), a later line over an earlier one. */
	OUTLAWSRUNTIME_API void ParseLocalMsg(const FString& Text, TMap<int32, FString>& Out);
	/** loc_<l>.txt lines (key = "value") into Out by lower-case key. */
	OUTLAWSRUNTIME_API void ParseLoc(const FString& Text, TMap<FString, FString>& Out);
#if WITH_OUTLAWS_TESTS
	/** (tests) this run's language without the settings file (empty: back to the normal rule), and the old rule on or off
	 *  (-1: as the command line says). */
	OUTLAWSRUNTIME_API void TestSet(const FString& Code);
	OUTLAWSRUNTIME_API void TestSetOld(int32 bOld);
#endif
}
