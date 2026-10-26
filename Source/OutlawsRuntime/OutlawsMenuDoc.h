#pragma once

#include "CoreMinimal.h"

/**
 * The remaster's menus, read at runtime from the player's own Common.kpf (a zip archive whose entries
 * are stored uncompressed): the RmlUi documents under menu (the .rml files) and the English strings in
 * localization/loc_english.txt.  Each document becomes a page of items (links to other pages,
 * dialogs, actions, drop-down selects, sliders, checkboxes, radio buttons, key bindings, section
 * headings), enough to show and walk the whole menu tree in VR.  Nothing is written back.
 */
namespace OutlawsMenu
{
	/** A read-only view of a zip whose entries are stored (method 0): Common.kpf. */
	class OUTLAWSRUNTIME_API FKpf
	{
	public:
		bool Open(const FString& InPath);
		bool IsOpen() const { return Entries.Num() > 0; }
		bool Read(const FString& Name, TArray<uint8>& Out) const;
		bool ReadText(const FString& Name, FString& Out) const;
		void List(const FString& Prefix, TArray<FString>& Out) const;

	private:
		struct FEntry { int64 LocalHeader = 0; int64 Size = 0; uint16 Method = 0; };
		FString Path;
		TMap<FString, FEntry> Entries;     // lower-case name -> where it is
	};

	enum class EItemKind : uint8 { Link, Dialog, Action, Button, Select, Range, Check, Radio, Bind, Heading };

	struct FItem
	{
		EItemKind Kind = EItemKind::Action;
		FString Label;
		FString Target;               // Link / Dialog: the .rml it opens
		FString Action;               // onclick (ev_close_menu, ev_restart_level...)
		bool bGoBack = false;         // kex-goback
		TArray<FString> Options;      // Select: the choices shown
		float Min = 0.f, Max = 1.f, Step = 0.1f;   // Range
		FString Name;                 // the cvar a control edits (g_autoAim, snd_volume...)
		FString Classes;              // its class attribute ("pc_only chicken": Exit clucks when pointed at)
	};

	struct FPage
	{
		FString File;
		FString Title;
		FString Message;              // dialogs
		TArray<FItem> Items;
	};

	/**
	 * REQ-TRIGGERSTEP: which way a press steps a value row.  The menu input (AOutlawsPlayer::MenuPress) sets it for the one
	 * press it hands to the row; every value row on every page steps through StepIndex / StepNumber below, so the rule lives
	 * here once.  Two controllers: the right trigger steps up (+1), the left down (-1), number rows stop at their ends.  The
	 * desktop: left click up, right click down, the same stops.  One controller, and every press made from code (tests, the
	 * co-op flow): one step up, round again, as before.
	 */
	struct FStepPress
	{
		int32 Dir = 1;                // +1 up (forward in a list), -1 down
		bool bStopAtEnds = false;     // a number row stops at its first and last value (else it goes round)
		bool bHitEnd = false;         // set when a number row was already at the end it was stepped towards
	};
	OUTLAWSRUNTIME_API FStepPress& StepPress();
	/** A value row's next index among Count values from Cur.  bNumber: an ordered number (stops at its ends while the press
	 *  says so); else a named list (goes round both ways).  Cur outside 0..Count-1 (not found) gives the first value. */
	OUTLAWSRUNTIME_API int32 StepIndex(int32 Cur, int32 Count, bool bNumber);
	/** A number row on the grid of Step between Lo and Hi (the grid counted from 0, as the rows always were: an off-grid
	 *  value goes to the next grid value that way). */
	OUTLAWSRUNTIME_API float StepNumber(float Now, float Lo, float Hi, float Step);
	/** The next of an ordered list of numbers (Steps): its index found (or the first), then StepIndex as a number row. */
	template <typename T>
	T StepAmong(const TArray<T>& Steps, const T& Now)
	{
		if (Steps.Num() == 0) return Now;
		return Steps[StepIndex(Steps.IndexOfByKey(Now), Steps.Num(), true)];
	}

	class OUTLAWSRUNTIME_API FSource
	{
	public:
		/** Open <Root>/Common.kpf and the English strings. */
		bool Load(const FString& InstallRoot);
		bool IsLoaded() const { return Kpf.IsOpen(); }
		/** A menu page by file name (main_game.rml, options.rml, dialog_quit.rml...). */
		bool GetPage(const FString& File, FPage& Out) const;
		/** Any file in Common.kpf (the wheel and menu art, the fonts). */
		bool ReadFile(const FString& Name, TArray<uint8>& Out) const { return Kpf.Read(Name, Out) || (UiKpf.IsOpen() && UiKpf.Read(Name, Out)); }
		/** Menu art: UI.kpf first (Common.kpf holds magenta placeholders for some of it, e.g. Main_Background). */
		bool ReadArt(const FString& Name, TArray<uint8>& Out) const { return (UiKpf.IsOpen() && UiKpf.Read(Name, Out)) || Kpf.Read(Name, Out); }
		/** "$m_local_6050" -> "Resume"; plain text passes through. */
		FString Loc(const FString& Text) const;

	private:
		FKpf Kpf;
		FKpf UiKpf;                   // UI.kpf: the menu art (the save screen's Load_* pieces)
		TMap<FString, FString> Strings;
	};
}
