#pragma once

#include "CoreMinimal.h"
#include "Misc/Parse.h"

/**
 * The -OL command-line flags, read once (on first use) into one list: each "-OLName" or "-OLName=value" token, matched by its
 * whole name (no substring matches: -OLVoiceSoak no longer matches -OLVoiceSoakSeconds=, and OLLobbyShot= no other flag's
 * tail). The module logs the set once at startup ("OLFlags: -OLA -OLB=3 ..."). Other switches (-unattended, -port=) stay
 * with FParse on the command line.
 */
struct OUTLAWSRUNTIME_API FOutlawsFlags
{
	struct FFlag
	{
		FString Name;        // "OLCoopTest"
		FString Token;       // the first "OLCoopTest=inf" (quotes kept, for FParse::Value), or "OLCoopTest" when only bare
		bool bBare = false;  // "-OLCoopTest" given on its own
		bool bValue = false; // "-OLCoopTest=..." given
	};

	static const FOutlawsFlags& Get();
	const FFlag* Find(const TCHAR* Name) const;
	/** "-OLA -OLB=3 ...", in command-line order. */
	FString Line() const;
	int32 Num() const { return Flags.Num(); }

private:
	FOutlawsFlags();
	TArray<FFlag> Flags;
};

namespace OLFlags
{
	/** "-Name" on its own (FParse::Param's rule: "-Name=x" does not count). */
	inline bool Param(const TCHAR* Name)
	{
		const FOutlawsFlags::FFlag* F = FOutlawsFlags::Get().Find(Name);
		return F && F->bBare;
	}
	/** "-Name" or "-Name=anything". */
	inline bool Has(const TCHAR* Name)
	{
		return FOutlawsFlags::Get().Find(Name) != nullptr;
	}
	/** "-Name=..." (the value may be empty). */
	inline bool HasValue(const TCHAR* Name)
	{
		const FOutlawsFlags::FFlag* F = FOutlawsFlags::Get().Find(Name);
		return F && F->bValue;
	}
	/** "-Name=<value>", parsed as FParse::Value parses it (the same overloads and stop rules). */
	template <typename T, typename... TArgs>
	bool Value(const TCHAR* Name, T& Out, TArgs... Args)
	{
		const FOutlawsFlags::FFlag* F = FOutlawsFlags::Get().Find(Name);
		if (!F || !F->bValue) return false;
		return FParse::Value(*F->Token, *(F->Name + TEXT("=")), Out, Args...);
	}
	/** "-Name=<value>" with exactly this value (case-insensitive). */
	inline bool ValueIs(const TCHAR* Name, const TCHAR* Want)
	{
		FString V;
		return Value(Name, V) && V.Equals(Want, ESearchCase::IgnoreCase);
	}
}
