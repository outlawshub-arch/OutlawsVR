// -OLShots -OLLanguageProbe (REQ-LANGUAGE, 9 Nov; the user: six languages, a language's files in front of the stock ones, a line a
// language lacks reads English).  In a story level, one level start per language (en fr de it es pt-BR, each by the test's own
// language, never the settings file), then the Options > Language page, then one level start with the old rule (-OLOldLanguage's
// stand-in, the language set to German):
//   order:  the mounted order begins with that language's LABs (OLLanguage::LabsFor, those the install has), then the stock ones
//   where:  local.msg resolves to the language's text LAB (English: a stock LAB)
//   801:    the level's message 801 ("You need the brass key.") is the line read straight from that LAB's own local.msg (and is
//           not the English line)
//   837:    a line German and Spanish lack (the Handful of Missions' cave-in line): not in their file, the English line in the level
//   menu:   a menu source loaded now reads $m_option_language_UC as the language's loc file has it, and $m_vault_restart_video
//           (in English's file only) as English
//   taunt:  (step 4) the taunt LAB mounted (oltaunt_<l> for de it es pt-BR, none for en fr); c105.wav read from it with the HD
//           sounds on and off, the bytes the LAB's own entry holds; English and French: oltaunthd (HD) and oltaunt; Italian's
//           missing "max.wav": the English oltaunt
//   labels: (step 5) the menu overlay's 16 English labels: each the language's loc value of its key (capitals for the title
//           forms), or our own word for Story, Players, Extras and Graphics (not empty); the Options page's Controls, Graphics
//           and Volume rows read so
//   srt:    (step 7) the subtitles file for a converted cutscene (-OLLanguageMovie's tre_cab): <name>_<l>.srt beside it
//           (English <name>.srt); in_shb (not converted there): the install's movies\IN_SHB_<l>.srt with cues; ram: none
//   movie:  (step 8) the cutscene played on a movie screen: 5 audio tracks (the converter kept them all), the track picked by
//           language (en 0, it 1, de 2, es 3, pt-BR 4; fr the English 0), its own voice (fr not), the subtitles file and shown
//   page:   (step 6) the Options page keeps its Language link; options_language.rml shows the six languages (en fr de it es
//           pt-BR) and the cutscene subtitles row, the language in force marked; pressing German sets it, saves Language=de,
//           reads the menus again (the title in German); the subtitles row Off saves CutsceneSubtitles=Off, On again; a file with
//           no Language= reads the remaster's language or English, an unknown one too, no CutsceneSubtitles= reads On; with the
//           switch Off the subtitles show only where the voice is not the language's own (fr)
//   old:    English, no language LAB mounted, local.msg from a stock LAB, message 801, the menu, the voice, the subtitles and
//           the movie's track in English
// plus the first start's reading of a kexengine.cfg line.  Lines "OLSHOT language <lang> <check>: <got> want <want> PASS|FAIL", then
// "OLSHOT language RESULT ALL PASS (n passed, 0 failed)" or "OLSHOT language RESULT FAIL (n passed, m failed)".  The settings file
// is the run's own (-userdir) and is put back as it was at the end.
//   accents: (the user 9 Nov, "Draw the accent over the menu font's own letter") the menu's title and row fonts, for each accented
//           letter of the sample (Francais, Espanol, Portugues, Active, Uberprufen, Acao, Perche with their accents) and then every letter the composer covers:
//           outside the font's Noto ranges, in the face file it draws with, and (the overlay's fonts, composed) the glyph beginning
//           with the face's own base letter's outline.  -OLOldMenuAccents must FAIL these (the letters from Noto).
// -OLLanguageProbe=accents: the accents checks only (a short run; with -OLOldMenuAccents it must FAIL).
// -OLLanguageProbe=flag: no switching, the first level start only, for a run given -OLLanguage=<code> (its order, where, 801 and
// the source "flag").  A run with -OLOldLanguage must FAIL (every language reads English, the page's rows are gone).
// -OLLanguageHudShot: the level's message 801 on the HUD, Saved\OLShots\ol_language_hud_<lang>.png, then the run ends (sign-off).
// -OLLanguageSubShot[=<movie>]: the cutscene (default in_shb, the intro) played as the level plays it, shot at 12.5 s of its own
// time (the intro's second line), Saved\OLShots\ol_language_sub_<lang>.png, then the run ends (sign-off).

#include "ShotTestCommon.h"
#include "OutlawsLanguage.h"
#include "OutlawsMovieScreen.h"
#include "OutlawsFontCompose.h"
#include "Engine/Font.h"

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

	// (REQ-LANGUAGE accents) one menu font against the accent sample: each accented letter outside the font's Noto ranges, in
	// the face file the font draws with, and (a composed face) that glyph beginning with the face's own base letter's outline
	const TCHAR* AccentSample = TEXT("Fran\u00E7ais, Espa\u00F1ol, Portugu\u00EAs, Activ\u00E9, \u00DCberpr\u00FCfen, A\u00E7\u00E3o, Perch\u00E9");
	void CheckAccentFont(const UFont* F, const TCHAR* Which)
	{
		const FString Tag = TEXT("accents");
		if (!F || F->CompositeFont.DefaultTypeface.Fonts.Num() == 0) { LangCheck(Tag, Which, false, TEXT("no font"), TEXT("the menu font")); return; }
		const FString Face = F->CompositeFont.DefaultTypeface.Fonts[0].Font.GetFontFilename();
		TArray<uint8> Bytes;
		FFileHelper::LoadFileToArray(Bytes, *Face, FILEREAD_Silent);
		const bool bComposed = Face.EndsWith(TEXT(".accents.ttf"));
		TSet<uint32> Seen;
		for (const TCHAR* C = AccentSample; *C; ++C)
		{
			const uint32 Cp = uint32(*C);
			if (Cp < 0x80 || Seen.Contains(Cp)) continue;
			Seen.Add(Cp);
			bool bNoto = false;
			for (const FCompositeSubFont& Sub : F->CompositeFont.SubTypefaces) for (const FInt32Range& R : Sub.CharacterRanges) bNoto = bNoto || R.Contains(int32(Cp));
			const bool bHas = OLFontCompose::HasChar(Bytes, Cp);
			const bool bBase = OLFontCompose::StartsWithBase(Bytes, Cp, OLFontCompose::BaseOf(Cp));
			const FString Got = FString::Printf(TEXT("%c: %s, %s, %s"), TCHAR(Cp), bNoto ? TEXT("Noto range") : TEXT("theme range"),
				bHas ? TEXT("in the face") : TEXT("not in the face"), bBase ? TEXT("its base letter's outline + mark") : bComposed ? TEXT("not built on its base") : TEXT("the face's own glyph"));
			LangCheck(Tag, *FString::Printf(TEXT("%s %c"), Which, TCHAR(Cp)), !bNoto && bHas && (bBase || !bComposed), Got, TEXT("theme range, in the face, built on its base"));
		}
		// every letter the composer covers, as one line
		int32 Ok = 0, All = 0;
		for (uint32 Cp : OLFontCompose::Letters())
		{
			bool bNoto = false;
			for (const FCompositeSubFont& Sub : F->CompositeFont.SubTypefaces) for (const FInt32Range& R : Sub.CharacterRanges) bNoto = bNoto || R.Contains(int32(Cp));
			++All;
			if (!bNoto && OLFontCompose::HasChar(Bytes, Cp) && (!bComposed || OLFontCompose::StartsWithBase(Bytes, Cp, OLFontCompose::BaseOf(Cp)))) ++Ok;
		}
		LangCheck(Tag, *FString::Printf(TEXT("%s all letters"), Which), Ok == All, FString::Printf(TEXT("%d of %d (%s)"), Ok, All, *FPaths::GetCleanFilename(Face)), FString::Printf(TEXT("%d of %d"), All, All));
	}

	bool OpenLab(const FString& Root, const FString& Lab, FOutlawsLabFile& File)
	{
		for (const FString& P : { FPaths::Combine(Root, Lab), FPaths::Combine(Root, TEXT("assets"), Lab) })
		{
			if (FPaths::FileExists(P)) return File.Open(P);
		}
		return false;
	}

	// a LAB's own local.msg, read from the file itself (not through the mounted order)
	bool LabMessages(const FString& Root, const FString& Lab, TMap<int32, FString>& Out)
	{
		FOutlawsLabFile File;
		if (!OpenLab(Root, Lab, File)) return false;
		const int32* E = File.GetIndex().Find(TEXT("local.msg"));
		TArray<uint8> Bytes;
		if (!E || !File.Read(*E, Bytes)) return false;
		FString Text;
		for (uint8 B : Bytes) Text.AppendChar(TCHAR(B));      // latin-1, as GetText
		OLLanguage::ParseLocalMsg(Text, Out);
		return true;
	}

	bool InstallHas(const FString& Root, const FString& Lab)
	{
		return FPaths::FileExists(FPaths::Combine(Root, Lab)) || FPaths::FileExists(FPaths::Combine(Root, TEXT("assets"), Lab));
	}

	int32 CountCues(const FString& Srt)
	{
		FString Text;
		if (Srt.IsEmpty() || !FFileHelper::LoadFileToString(Text, *Srt)) return 0;
		int32 N = 0;
		for (int32 At = Text.Find(TEXT("-->")); At != INDEX_NONE; At = Text.Find(TEXT("-->"), ESearchCase::CaseSensitive, ESearchDir::FromStart, At + 3)) ++N;
		return N;
	}

	FString Clean(const FString& Path) { return Path.IsEmpty() ? FString(TEXT("<none>")) : FPaths::GetCleanFilename(Path); }

	// the cutscene the guard converted (convert-cutscenes.ps1 -Only ram,tre_cab), its folder holding both
	FString ProbeMovie()
	{
		FString M;
		OLFlags::Value(TEXT("OLLanguageMovie"), M, /*bShouldStopOnSeparator*/ false);
		return M.TrimQuotes();
	}

	// the remaster's order of the dubbed tracks (MEASURED 9 Nov, ffprobe of every movies\*.webm): en it de es pt-BR
	int32 WantTrack(const FString& Lang)
	{
		return Lang == TEXT("it") ? 1 : Lang == TEXT("de") ? 2 : Lang == TEXT("es") ? 3 : Lang == TEXT("pt-BR") ? 4 : 0;
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

		// (step 4) the voice: the language's taunt LAB, c105.wav from it in both sound sets, byte for byte its own entry
		{
			const FString Taunts = OLLanguage::TauntLab(Want);
			const bool bHere = !Taunts.IsEmpty() && InstallHas(Root, Taunts);
			const FString WantLab = bHere ? Taunts : FString();
			LangCheck(Tag, TEXT("taunt LAB"), A->GetTauntLab().Equals(WantLab, ESearchCase::IgnoreCase), Clean(A->GetTauntLab()), WantLab.IsEmpty() ? FString(TEXT("<none>")) : WantLab);
			const FString Classic = A->WhereSound(TEXT("c105.wav"), false), Hd = A->WhereSound(TEXT("c105.wav"), true);
			LangCheck(Tag, TEXT("c105 classic"), Classic.Equals(bHere ? Taunts : FString(TEXT("oltaunt.lab")), ESearchCase::IgnoreCase), Classic, bHere ? Taunts : FString(TEXT("oltaunt.lab")));
			LangCheck(Tag, TEXT("c105 HD"), Hd.Equals(bHere ? Taunts : FString(TEXT("oltaunthd.lab")), ESearchCase::IgnoreCase), Hd, bHere ? Taunts : FString(TEXT("oltaunthd.lab")));
			if (bHere)
			{
				FOutlawsLabFile Own;
				TArray<uint8> Mine, Got;
				const int32* E = OpenLab(Root, Taunts, Own) ? Own.GetIndex().Find(TEXT("c105.wav")) : nullptr;
				const bool bMine = E && Own.Read(*E, Mine);
				const bool bGot = A->GetSound(TEXT("c105.wav"), true, Got);
				LangCheck(Tag, TEXT("c105 bytes"), bMine && bGot && Mine == Got, FString::Printf(TEXT("%d bytes"), Got.Num()), FString::Printf(TEXT("%d bytes from %s"), Mine.Num(), *Taunts));
			}
			if (Want == TEXT("it"))
			{
				const FString Max = A->WhereSound(TEXT("max.wav"), false);
				LangCheck(Tag, TEXT("max (not in Italian's)"), Max.Equals(TEXT("oltaunt.lab"), ESearchCase::IgnoreCase), Max, TEXT("oltaunt.lab"));
			}
		}

		// (step 7) the subtitles files: beside a converted cutscene, the install's for one not converted, none for RAM
		const FString Movie = ProbeMovie();
		if (!Movie.IsEmpty())
		{
			const FString Dir = FPaths::GetPath(Movie);
			const FString Beside = OLLanguage::SubtitleFile(FPaths::Combine(Dir, TEXT("tre_cab.mp4")), Want, Root);
			const FString WantBeside = Want == TEXT("en") ? FString(TEXT("tre_cab.srt")) : TEXT("tre_cab_") + Want + TEXT(".srt");
			LangCheck(Tag, TEXT("srt beside"), Clean(Beside).Equals(WantBeside, ESearchCase::IgnoreCase) && FPaths::IsSamePath(FPaths::GetPath(Beside), Dir) && CountCues(Beside) > 0,
				FString::Printf(TEXT("%s (%d cues)"), *Clean(Beside), CountCues(Beside)), WantBeside);
			const FString Install = OLLanguage::SubtitleFile(FPaths::Combine(Dir, TEXT("in_shb.mp4")), Want, Root);
			const FString WantInstall = Want == TEXT("en") ? FString(TEXT("<none>")) : TEXT("IN_SHB_") + Want + TEXT(".srt");
			LangCheck(Tag, TEXT("srt from the install"), Clean(Install).Equals(WantInstall, ESearchCase::IgnoreCase) && (Want == TEXT("en") || CountCues(Install) > 0),
				FString::Printf(TEXT("%s (%d cues)"), *Clean(Install), CountCues(Install)), WantInstall);
			const FString Ram = OLLanguage::SubtitleFile(FPaths::Combine(Dir, TEXT("ram.mp4")), Want, Root);
			LangCheck(Tag, TEXT("srt ram (empty or none)"), Ram.IsEmpty(), Clean(Ram), TEXT("<none>"));
		}
		else if (!bFlag) LangCheck(Tag, TEXT("srt"), false, TEXT("no -OLLanguageMovie"), TEXT("the converted cutscenes"));

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

		// (step 5) the overlay's 16 labels, each by itself and three of them as the Options page shows them
		TArray<TPair<FString, FString>> Labels;
		OutlawsMenu::FSource::BakedLabels(Labels);
		int32 LabelsOk = 0;
		TArray<FString> Shown;
		for (const TPair<FString, FString>& Lb : Labels)
		{
			const FString Got = Src.Baked(Lb.Key);
			FString Wanted;
			bool bOk;
			if (Want == TEXT("en")) { Wanted = Lb.Key; bOk = Got == Lb.Key; }
			else if (!Lb.Value.IsEmpty())
			{
				const FString* V = LocLang.Find(Lb.Value.ToLower());
				const bool bUpper = Lb.Key.Equals(OutlawsMenu::FSource::UpperLatin(Lb.Key), ESearchCase::CaseSensitive);
				Wanted = V ? (bUpper ? OutlawsMenu::FSource::UpperLatin(V->TrimStartAndEnd()) : V->TrimStartAndEnd()) : FString(TEXT("<no key>"));
				bOk = V && Got == Wanted;
			}
			else { Wanted = TEXT("<our own word>"); bOk = !Got.TrimStartAndEnd().IsEmpty(); }
			if (bOk) ++LabelsOk;
			else LangCheck(Tag, TEXT("label"), false, Lb.Key + TEXT(" -> ") + Got, Wanted);
			Shown.Add(Got);
		}
		LangCheck(Tag, TEXT("16 labels"), Labels.Num() == 16 && LabelsOk == 16, FString::Printf(TEXT("%d of %d: %s"), LabelsOk, Labels.Num(), *FString::Join(Shown, TEXT("|"))), TEXT("16 of 16"));
		if (Src.HasOverlay())
		{
			OutlawsMenu::FPage Opt;
			TArray<FString> Rows;
			bool bRows = Src.GetPage(TEXT("options.rml"), Opt);
			for (const OutlawsMenu::FItem& It : Opt.Items) Rows.Add(It.Label);
			for (const TCHAR* Lb : { TEXT("Controls"), TEXT("Graphics"), TEXT("Volume") }) bRows = bRows && Rows.Contains(Src.Baked(Lb));
			LangCheck(Tag, TEXT("options.rml rows"), bRows, FString::Join(Rows, TEXT("|")), Src.Baked(TEXT("Controls")) + TEXT("|") + Src.Baked(TEXT("Graphics")) + TEXT("|") + Src.Baked(TEXT("Volume")));
		}
		else LangCheck(Tag, TEXT("options.rml rows"), false, TEXT("no menu overlay open"), TEXT("-OLMenuOverlay"));
	}

	// (step 8) the movie screen's own pick, once its media is open
	void CheckMovie(const AOutlawsMovieScreen* M, const FString& Lang, bool bOld)
	{
		const FString Tag = bOld ? TEXT("old(") + Lang + TEXT(")") : Lang;
		const FString Want = bOld ? FString(TEXT("en")) : Lang;
		if (!M) { LangCheck(Tag, TEXT("movie"), false, TEXT("no screen"), TEXT("played")); return; }
		LangCheck(Tag, TEXT("movie audio tracks"), M->GetAudioTrackCount() == 5, FString::FromInt(M->GetAudioTrackCount()), TEXT("5 (every language the converter kept)"));
		LangCheck(Tag, TEXT("movie track"), M->GetAudioTrack() == WantTrack(Want), FString::Printf(TEXT("%d (%s)"), M->GetAudioTrack(), *M->GetAudioTrackLanguage()), FString::FromInt(WantTrack(Want)));
		LangCheck(Tag, TEXT("movie own voice"), M->IsOwnVoice() == (Want != TEXT("fr")), M->IsOwnVoice() ? TEXT("yes") : TEXT("no"), Want != TEXT("fr") ? TEXT("yes") : TEXT("no (French: the English voice)"));
		const FString WantSrt = Want == TEXT("en") ? FString(TEXT("tre_cab.srt")) : TEXT("tre_cab_") + Want + TEXT(".srt");
		LangCheck(Tag, TEXT("movie subtitles"), Clean(M->GetSubtitlePath()).Equals(WantSrt, ESearchCase::IgnoreCase) && M->GetCueCount() > 0 && M->AreSubtitlesShown(),
			FString::Printf(TEXT("%s, %d cues, %s"), *Clean(M->GetSubtitlePath()), M->GetCueCount(), M->AreSubtitlesShown() ? TEXT("shown") : TEXT("hidden")), WantSrt + TEXT(", shown"));
	}

	FString SettingsValue(const TCHAR* Key)
	{
		const TMap<FString, FString> S = AOutlawsLevelActor::ReadSettings();
		const FString* V = S.Find(Key);
		return V ? *V : FString(TEXT("<none>"));
	}
}

OL_SHOT_TEST(OLLanguageProbe, 2588)
{
	// -1 the first build, 0..5 the six languages, 6 the Options page, 7 the old rule, 8 done; within a language stage, Phase 0
	// waits for its level, 1 for its movie
	static int32 Stage = -1;
	static int32 Phase = 0;
	static double Wait = 0.0;
	static TWeakObjectPtr<AOutlawsMovieScreen> Screen;
	static bool bHadSettings = false;
	static FString SettingsBefore;
	if (Stage >= 8 || !L || ShotTime < 6.f) return;
	const TArray<FString>& Codes = OLLanguage::Codes();
	FString Mode;
	OLFlags::Value(TEXT("OLLanguageProbe"), Mode);
	const bool bFlag = Mode.Equals(TEXT("flag"), ESearchCase::IgnoreCase);
	const FString SettingsFile = FPaths::ProjectSavedDir() / TEXT("OutlawsSettings.ini");
	auto Finish = [&]()
	{
		OLLanguage::TestSet(FString());
		OLLanguage::TestSetOld(-1);
		// the run's settings file as it was
		if (bHadSettings) FFileHelper::SaveStringToFile(SettingsBefore, *SettingsFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		else IFileManager::Get().Delete(*SettingsFile, false, true, true);
		Stage = 8;
		if (GLangFail == 0 && GLangPass > 0) { UE_LOG(LogOutlaws, Display, TEXT("OLSHOT language RESULT ALL PASS (%d passed, 0 failed)"), GLangPass); }
		else { UE_LOG(LogOutlaws, Display, TEXT("OLSHOT language RESULT FAIL (%d passed, %d failed)"), GLangPass, GLangFail); }
		FGenericPlatformMisc::RequestExit(false);
	};
	auto StartLevel = [&](int32 Next)
	{
		Stage = Next;
		Phase = 0;
		GLangBuilt = false;
		if (Next < 6) { OLLanguage::TestSetOld(-1); OLLanguage::TestSet(Codes[Next]); }
		else { OLLanguage::TestSetOld(1); OLLanguage::TestSet(TEXT("de")); }
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT language start %s"), Next < 6 ? *Codes[Next] : TEXT("old (de asked)"));
		L->RestartLevelThen([]() { GLangBuilt = true; GLangBuiltAt = FPlatformTime::Seconds(); });
	};
	auto StageLang = [&]() { return Stage < 6 ? Codes[Stage] : FString(TEXT("de")); };
	if (Stage == -1)
	{
		bHadSettings = FFileHelper::LoadFileToString(SettingsBefore, *SettingsFile);
		// the first start's reading of the remaster's setting (no file written or changed)
		struct FCase { const TCHAR* Cfg; const TCHAR* Want; };
		const FCase KexCases[] = { { TEXT("seta g_language \"de\"\n"), TEXT("de") }, { TEXT("seta snd_voicelanguage \"0\"\nseta g_language \"pt-BR\"\n"), TEXT("pt-BR") },
			{ TEXT("seta g_language \"xx\"\n"), TEXT("") }, { TEXT("seta g_language \"fr\"\nseta g_language \"IT\"\n"), TEXT("it") } };
		for (const FCase& C : KexCases)
		{
			const FString Got = OLLanguage::ParseKexLanguage(C.Cfg);
			LangCheck(TEXT("firststart"), TEXT("kexengine line"), Got == C.Want, Got.IsEmpty() ? FString(TEXT("<none>")) : Got, *C.Want ? C.Want : TEXT("<none>"));
		}
		// (step 8) the track pick by tags alone: WMF's two letters, an mp4's three, none
		{
			bool bOwn = false;
			const TArray<FString> Two = { TEXT("en"), TEXT("it"), TEXT("de"), TEXT("es"), TEXT("pt") }, Three = { TEXT("eng"), TEXT("ita"), TEXT("ger"), TEXT("spa"), TEXT("por") };
			FString Got;
			bool bAll = true;
			for (const FString& C : Codes)
			{
				const int32 A2 = OLLanguage::PickAudioTrack(Two, C, bOwn);
				const int32 A3 = OLLanguage::PickAudioTrack(Three, C, bOwn);
				Got += FString::Printf(TEXT("%s%s %d/%d"), Got.IsEmpty() ? TEXT("") : TEXT(", "), *C, A2, A3);
				bAll = bAll && A2 == WantTrack(C) && A3 == WantTrack(C);
			}
			const int32 None = OLLanguage::PickAudioTrack(TArray<FString>(), TEXT("de"), bOwn);
			LangCheck(TEXT("firststart"), TEXT("track pick by tag"), bAll && None == 0 && !bOwn, Got + FString::Printf(TEXT(", no tags %d"), None), TEXT("en 0, fr 0, de 2, it 1, es 3, pt-BR 4; no tags 0"));
		}
		FString CfgFile;
		const FString Kex = OLLanguage::RemasterLanguage(&CfgFile);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT language remaster setting: %s (%s); this run %s from %s"), Kex.IsEmpty() ? TEXT("<none>") : *Kex,
			CfgFile.IsEmpty() ? TEXT("no kexengine.cfg") : TEXT("the remaster's kexengine.cfg"), *OLLanguage::Current(), *OLLanguage::Source());
		if (Mode.Equals(TEXT("accents"), ESearchCase::IgnoreCase))
		{
			// (-OLLanguageProbe=accents) the menu fonts only: open the menu (it makes them), check, done
			if (!MenuTitleFont) { OpenMenu(false); if (!MenuTitleFont) return; }
			CheckAccentFont(MenuTitleFont, TEXT("title font"));
			CheckAccentFont(MenuRowFont, TEXT("row font"));
			CloseMenu();
			Finish();
			return;
		}
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
	if (Stage == 6)
	{
		// (step 6) the Options > Language page as the player reaches it, the settings file the run's own
		const FString Tag = TEXT("page");
		OLLanguage::TestSet(FString());
		const FString Remaster = OLLanguage::RemasterLanguage();
		const FString Default = Remaster.IsEmpty() ? FString(TEXT("en")) : Remaster;
		{
			TMap<FString, FString> S = AOutlawsLevelActor::ReadSettings();
			S.Remove(TEXT("Language"));
			S.Remove(TEXT("CutsceneSubtitles"));
			AOutlawsLevelActor::WriteSettings(S);
			OLLanguage::TestForget();
			LangCheck(Tag, TEXT("no Language= reads the remaster's or English"), OLLanguage::Current() == Default, OLLanguage::Current() + TEXT(" from ") + OLLanguage::Source(), Default);
			LangCheck(Tag, TEXT("no CutsceneSubtitles= reads On"), OLLanguage::Subtitles(), OLLanguage::Subtitles() ? TEXT("On") : TEXT("Off"), TEXT("On"));
			S.Add(TEXT("Language"), TEXT("xx"));
			AOutlawsLevelActor::WriteSettings(S);
			OLLanguage::TestForget();
			LangCheck(Tag, TEXT("Language=xx reads the remaster's or English"), OLLanguage::Current() == Default, OLLanguage::Current(), Default);
			S.Remove(TEXT("Language"));
			AOutlawsLevelActor::WriteSettings(S);
			OLLanguage::TestForget();
		}
		if (!bMenuOpen) OpenMenu(false);
		CheckAccentFont(MenuTitleFont, TEXT("title font"));
		CheckAccentFont(MenuRowFont, TEXT("row font"));
		MenuStack.Reset();
		MenuStack.Add(TEXT("#front"));
		ShowMenuPage(TEXT("options.rml"), false);
		const bool bLink = MenuPage && MenuPage->Items.ContainsByPredicate([](const OutlawsMenu::FItem& It) { return It.Target.Equals(TEXT("options_language.rml"), ESearchCase::IgnoreCase); });
		LangCheck(Tag, TEXT("Options keeps the Language link"), bLink, bLink ? TEXT("kept") : TEXT("scrubbed"), TEXT("kept"));
		ShowMenuPage(TEXT("options_language.rml"), true);
		auto Rows = [this]()
		{
			FString R;
			if (MenuPage) for (int32 i = 0; i < MenuPage->Items.Num(); ++i) R += (R.IsEmpty() ? TEXT("") : TEXT(" | ")) + MenuRowText(i);
			return R;
		};
		auto Find = [this](const FString& Name, const FString& Value)
		{
			if (MenuPage) for (int32 i = 0; i < MenuPage->Items.Num(); ++i) if (MenuPage->Items[i].Name == Name && (Value.IsEmpty() || MenuPage->Items[i].Value == Value)) return i;
			return int32(INDEX_NONE);
		};
		TArray<FString> Values;
		if (MenuPage) for (const OutlawsMenu::FItem& It : MenuPage->Items) if (It.Name == TEXT("g_language")) Values.Add(It.Value);
		LangCheck(Tag, TEXT("the six languages"), Values == Codes, FString::Join(Values, TEXT(",")), FString::Join(Codes, TEXT(",")));
		const int32 Cc = Find(TEXT("cc_enable"), FString());
		LangCheck(Tag, TEXT("the subtitles row"), Cc != INDEX_NONE, Rows(), TEXT("a Cutscene Subtitles row"));
		const int32 Cur = Find(TEXT("g_language"), OLLanguage::Current());
		LangCheck(Tag, TEXT("the language in force marked"), Cur != INDEX_NONE && MenuRowText(Cur).StartsWith(TEXT("(o)")), Cur != INDEX_NONE ? MenuRowText(Cur) : FString(TEXT("<no row>")), TEXT("(o) ") + OLLanguage::Current());
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT language page rows (%s): %s"), *OLLanguage::Current(), *Rows());
		// German pressed: kept, saved, the menus in German at once
		const int32 De = Find(TEXT("g_language"), TEXT("de"));
		if (De != INDEX_NONE) MenuActivate(0, De);
		LangCheck(Tag, TEXT("pressed German: in force"), OLLanguage::Current() == TEXT("de"), OLLanguage::Current(), TEXT("de"));
		LangCheck(Tag, TEXT("pressed German: saved"), SettingsValue(TEXT("Language")) == TEXT("de"), SettingsValue(TEXT("Language")), TEXT("de"));
		LangCheck(Tag, TEXT("pressed German: menus read again"), MenuSource && MenuSource->GetLanguage() == TEXT("de"), MenuSource ? MenuSource->GetLanguage() : FString(TEXT("<none>")), TEXT("de"));
		ShowMenuPage(TEXT("options_language.rml"), false);
		bMenuRebuild = false;
		OutlawsMenu::FSource DeSrc;
		FString DeTitle;
		if (L->GetAssets() && DeSrc.Load(L->GetAssets()->GetRoot())) DeTitle = DeSrc.Loc(TEXT("$m_option_language_UC"));
		LangCheck(Tag, TEXT("the page title in German"), MenuPage && !DeTitle.IsEmpty() && MenuPage->Title == DeTitle, MenuPage ? MenuPage->Title : FString(), DeTitle);
		const int32 De2 = Find(TEXT("g_language"), TEXT("de"));
		LangCheck(Tag, TEXT("German marked"), De2 != INDEX_NONE && MenuRowText(De2).StartsWith(TEXT("(o)")), De2 != INDEX_NONE ? MenuRowText(De2) : FString(), TEXT("(o) Deutsch"));
		// the subtitles switch: Off, saved; the rule with it off; On again
		const int32 Cc2 = Find(TEXT("cc_enable"), FString());
		if (Cc2 != INDEX_NONE) MenuActivate(0, Cc2);
		LangCheck(Tag, TEXT("subtitles Off"), !OLLanguage::Subtitles() && SettingsValue(TEXT("CutsceneSubtitles")) == TEXT("Off"), FString::Printf(TEXT("%s, saved %s, row %s"),
			OLLanguage::Subtitles() ? TEXT("On") : TEXT("Off"), *SettingsValue(TEXT("CutsceneSubtitles")), Cc2 != INDEX_NONE ? *MenuRowText(Cc2) : TEXT("-")), TEXT("Off, saved Off"));
		LangCheck(Tag, TEXT("Off: German voice, no subtitles"), !OLLanguage::ShowSubtitles(TEXT("de"), true), OLLanguage::ShowSubtitles(TEXT("de"), true) ? TEXT("shown") : TEXT("hidden"), TEXT("hidden"));
		LangCheck(Tag, TEXT("Off: French (English voice) shows them"), OLLanguage::ShowSubtitles(TEXT("fr"), false), OLLanguage::ShowSubtitles(TEXT("fr"), false) ? TEXT("shown") : TEXT("hidden"), TEXT("shown"));
		LangCheck(Tag, TEXT("Off: English, none"), !OLLanguage::ShowSubtitles(TEXT("en"), true), OLLanguage::ShowSubtitles(TEXT("en"), true) ? TEXT("shown") : TEXT("hidden"), TEXT("hidden"));
		if (Cc2 != INDEX_NONE) MenuActivate(0, Cc2);
		LangCheck(Tag, TEXT("subtitles On again"), OLLanguage::Subtitles() && SettingsValue(TEXT("CutsceneSubtitles")) == TEXT("On"), FString::Printf(TEXT("%s, saved %s"),
			OLLanguage::Subtitles() ? TEXT("On") : TEXT("Off"), *SettingsValue(TEXT("CutsceneSubtitles"))), TEXT("On, saved On"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT language page rows (%s): %s"), *OLLanguage::Current(), *Rows());
		// English pressed again: the page back in English
		const int32 En = Find(TEXT("g_language"), TEXT("en"));
		if (En != INDEX_NONE) MenuActivate(0, En);
		LangCheck(Tag, TEXT("pressed English: in force, saved"), OLLanguage::Current() == TEXT("en") && SettingsValue(TEXT("Language")) == TEXT("en"),
			OLLanguage::Current() + TEXT(", saved ") + SettingsValue(TEXT("Language")), TEXT("en, saved en"));
		CloseMenu();
		StartLevel(7);
		return;
	}
	if (Phase == 0)
	{
		if (!GLangBuilt)
		{
			if (Wait == 0.0) Wait = FPlatformTime::Seconds();
			if (FPlatformTime::Seconds() - Wait > 90.0) { LangCheck(Stage < 6 ? Codes[Stage] : FString(TEXT("old")), TEXT("level start"), false, TEXT("no build in 90 s"), TEXT("built")); Finish(); }
			return;
		}
		Wait = 0.0;
		if (FPlatformTime::Seconds() - GLangBuiltAt < 1.0) return;
		CheckLanguage(L, StageLang(), Stage == 7, false);
		// (step 8) the converted cutscene on a movie screen of its own, in the level's language
		const FString Movie = ProbeMovie();
		const FString TreCab = FPaths::Combine(FPaths::GetPath(Movie), TEXT("tre_cab.mp4"));
		if (!Movie.IsEmpty() && FPaths::FileExists(TreCab) && GetWorld())
		{
			FActorSpawnParameters SP;
			SP.ObjectFlags |= RF_Transient;
			AOutlawsMovieScreen* M = GetWorld()->SpawnActor<AOutlawsMovieScreen>(AOutlawsMovieScreen::StaticClass(), FTransform::Identity, SP);
			if (M)
			{
				M->SetVolume(0.f);
				M->Play(TreCab, GetActorLocation(), GetActorForwardVector(), 300.f);
				Screen = M;
				Phase = 1;
				Wait = FPlatformTime::Seconds();
				return;
			}
		}
		LangCheck(Stage == 7 ? TEXT("old") : StageLang(), TEXT("movie"), false, FString::Printf(TEXT("no converted %s"), *TreCab), TEXT("-OLLanguageMovie"));
	}
	else
	{
		AOutlawsMovieScreen* M = Screen.Get();
		const bool bOpen = M && M->GetAudioTrackCount() >= 0;
		if (!bOpen && FPlatformTime::Seconds() - Wait < 15.0) return;
		CheckMovie(M, StageLang(), Stage == 7);
		if (M) { M->Skip(); M->Destroy(); }
		Screen.Reset();
		Wait = 0.0;
		Phase = 0;
	}
	if (Stage < 5) StartLevel(Stage + 1);
	else if (Stage == 5) { Stage = 6; Phase = 0; }
	else Finish();
}

// -OLLanguageHudShot (sign-off): the level's message 801 ("You need the brass key." in the run's language) on the HUD, shot
OL_SHOT_MATCH(OLLanguageHudShot) { return OLFlags::Has(TEXT("OLLanguageHudShot")); }

OL_SHOT_TEST(OLLanguageHudShot, 2589)
{
	static int32 Stage = 0;
	static float Waited = 0.f;
	if (Stage >= 3 || !L) return;
	Waited += Dt;
	if (Stage == 0)
	{
		if (Waited < 8.f) return;
		ShowMessage(L->UserMessageOr(801, TEXT("<none>")), 30.f);      // (the line as the level puts it on the HUD)
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT language hud %s: \"%s\" (local.msg from %s)"), *OLLanguage::Current(), *L->UserMessageOr(801, TEXT("<none>")),
			L->GetAssets() ? *L->GetAssets()->Where(TEXT("local.msg")) : TEXT("-"));
		Stage = 1; Waited = 0.f;
		return;
	}
	if (Stage == 1)
	{
		if (Waited < 1.5f) return;
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / (TEXT("ol_language_hud_") + OLLanguage::Current() + TEXT(".png")), false, false);
		Stage = 2; Waited = 0.f;
		return;
	}
	if (Waited < 2.f) return;
	Stage = 3;
	FGenericPlatformMisc::RequestExit(false);
}

// -OLLanguageSubShot[=<movie>] (sign-off): one cutscene subtitle on screen in the run's language; the movie as the level plays it
// (PlayMovie: the converted mp4, the subtitles file OLLanguage picks), shot when its own clock is at 12.5 s (in_shb's second line)
OL_SHOT_MATCH(OLLanguageSubShot) { return OLFlags::Has(TEXT("OLLanguageSubShot")); }

OL_SHOT_TEST(OLLanguageSubShot, 2590)
{
	static int32 Stage = 0;
	static float Waited = 0.f;
	if (Stage >= 3 || !L) return;
	Waited += Dt;
	FString Name;
	if (!OLFlags::Value(TEXT("OLLanguageSubShot"), Name) || Name.IsEmpty()) Name = TEXT("in_shb");
	const float At = Name == TEXT("in_shb") ? 12.5f : 6.f;
	if (Stage == 0)
	{
		if (Waited < 8.f) return;
		const bool bOk = L->PlayMovie(Name, true);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT language sub %s: PlayMovie(%s) %d"), *OLLanguage::Current(), *Name, bOk ? 1 : 0);
		if (!bOk) { Stage = 2; Waited = 0.f; return; }
		Stage = 1; Waited = 0.f;
		return;
	}
	if (Stage == 1)
	{
		AOutlawsMovieScreen* M = L->GetMovieScreen();
		const float T = M ? M->GetTime() : -1.f;
		if (T < At && Waited < 60.f) return;
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / (TEXT("ol_language_sub_") + OLLanguage::Current() + TEXT(".png")), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT language sub %s: movie time %.2f, subtitles %s, shown %d, own voice %d, track %d of %d"),
			*OLLanguage::Current(), T, M ? *FPaths::GetCleanFilename(M->GetSubtitlePath()) : TEXT("-"), M && M->AreSubtitlesShown() ? 1 : 0,
			M && M->IsOwnVoice() ? 1 : 0, M ? M->GetAudioTrack() : -1, M ? M->GetAudioTrackCount() : -1);
		Stage = 2; Waited = 0.f;
		return;
	}
	if (Waited < 2.f) return;
	Stage = 3;
	FGenericPlatformMisc::RequestExit(false);
}

#endif // WITH_OUTLAWS_TESTS
