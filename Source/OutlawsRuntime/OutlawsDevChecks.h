#pragma once

#include "CoreMinimal.h"

namespace OutlawsMenu { struct FPage; }

/**
 * REQ-DEVCHECKS: Pause > Dev checks (under Cheats), a hidden checklist for the headset confirmations.
 *
 * Gate: never in a Shipping build (compiled out). Otherwise on only with -OLDevChecks on the command line or the flag file
 * Saved/DevChecks/devchecks_on (tools/olue/devchecks/devchecks.ps1 push puts it there); -OLNoDevChecks forces it off.
 * Tester drops carry no Saved folder, so the page never shows for them. The row also needs the Cheats turn on the spot.
 *
 * The list: Saved/DevChecks/checks.json (pushed), else the bundled Content/DevChecks/checks.json (staged as UFS).
 * The answers: Saved/DevChecks/answers.json, written at every change.
 */
namespace OutlawsDevChecks
{
	enum EState : int32 { NotChecked = 0, Fixed = 1, StillBroken = 2 };

	struct FCheck
	{
		FString Id;
		FString What;
		FString Detail;
	};

	/** Whether this build and device may show the page (see above). */
	bool Enabled();
	FString Dir();
	FString ListPath();
	FString BundledPath();
	FString AnswersPath();

	/** Reads the list (pushed, else bundled) and the answers again. Returns where the list came from: "pushed", "bundled" or "none". */
	FString Reload();
	const TArray<FCheck>& Checks();
	const FString& Source();
	int32 State(const FString& Id);
	/** Not checked > Fixed > Still broken > Not checked; saved at once. */
	int32 Cycle(const FString& Id);
	const TCHAR* StateText(int32 State);

	/** The page "#devchecks": the current page of checks (8 a page plus a page row once there are more than 9). */
	void BuildPage(OutlawsMenu::FPage& Page);
	void NextPage();
	int32 NumPages();
	int32 CurrentPage();
}
