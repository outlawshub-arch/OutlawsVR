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
	 *  oliptch1 hold nothing oliptch2 lacks).  Its taunts (oltaunt_<l>) are not here: see TauntLab.  None for en. */
	OUTLAWSRUNTIME_API TArray<FString> LabsFor(const FString& Code);
	/** (REQ-LANGUAGE step 4) the language's taunts and voice lines, oltaunt_<l>.lab (de it es pt-BR); empty for en and fr
	 *  (French has none: its voice is English, user 2 Nov). */
	OUTLAWSRUNTIME_API FString TauntLab(const FString& Code);
	/** (step 6) whether the install under Root holds any of the language's LABs (English always): the Options page greys
	 *  the others "(not installed)". */
	OUTLAWSRUNTIME_API bool Installed(const FString& Code, const FString& Root);
	/** (step 7) the cutscene subtitles switch (the remaster's cc_enable): CutsceneSubtitles=On|Off in the settings, missing
	 *  = On (as before).  -OLOldLanguage: always On. */
	OUTLAWSRUNTIME_API bool Subtitles();
	OUTLAWSRUNTIME_API void SetSubtitles(bool bOn, bool bSave);
	/** (steps 7-8) the language the level's files were last mounted in (FOutlawsAssets::UseLanguage, at a level start) and
	 *  that install's folder: the cutscenes follow it, so a change on the Options page reaches them with the next level. */
	OUTLAWSRUNTIME_API const FString& Applied();
	OUTLAWSRUNTIME_API const FString& AppliedRoot();
	OUTLAWSRUNTIME_API void NoteApplied(const FString& Code, const FString& Root);
	/** (step 7) a cutscene's subtitles: <movie>_<l>.srt beside the movie, else the install's movies\<NAME>_<l>.srt, else
	 *  (and for English) <movie>.srt beside it.  An empty file counts as none.  Empty when there is none at all. */
	OUTLAWSRUNTIME_API FString SubtitleFile(const FString& MovieFile, const FString& Code, const FString& InstallRoot);
	/** (step 8) the movie's audio track for the language, from the tracks' language tags as the player reports them ("en",
	 *  "eng", "pt"...): the language's own, else (French, or a tag missing) the English one, else track 0.  bOwn: the track
	 *  is the language's own voice. */
	OUTLAWSRUNTIME_API int32 PickAudioTrack(const TArray<FString>& TrackLanguages, const FString& Code, bool& bOwn);
	/** (step 7, user 9 Nov: "if the Quest can't switch, English voices with subtitles") the subtitles show when the switch
	 *  is On, or when the voice heard is not the language's own (French, a pick that failed). */
	OUTLAWSRUNTIME_API bool ShowSubtitles(const FString& Code, bool bOwnVoice);
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
	/** (tests) forget the run's chosen language and the subtitles switch, so the next ask reads the settings file again. */
	OUTLAWSRUNTIME_API void TestForget();
#endif
}
