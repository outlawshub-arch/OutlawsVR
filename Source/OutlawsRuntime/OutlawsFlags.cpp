#include "OutlawsFlags.h"
#include "Misc/CommandLine.h"
#include "Misc/Char.h"

const FOutlawsFlags& FOutlawsFlags::Get()
{
	static const FOutlawsFlags Parsed;       // (once, on first use; the command line is set before any module loads)
	return Parsed;
}

FOutlawsFlags::FOutlawsFlags()
{
	const TCHAR* S = FCommandLine::Get();
	if (!S) return;
	while (*S)
	{
		while (*S && FChar::IsWhitespace(*S)) ++S;
		if (!*S) break;
		// one token: up to whitespace outside quotes (-OLHeadsetModel="Quest 3" is one)
		const TCHAR* Start = S;
		bool bQuoted = false;
		while (*S && (bQuoted || !FChar::IsWhitespace(*S)))
		{
			if (*S == TCHAR('"')) bQuoted = !bQuoted;
			++S;
		}
		if (*Start != TCHAR('-') && *Start != TCHAR('/')) continue;
		const FString Token(int32(S - Start - 1), Start + 1);
		if (!Token.StartsWith(TEXT("OL"), ESearchCase::IgnoreCase)) continue;
		int32 Eq = INDEX_NONE;
		Token.FindChar(TCHAR('='), Eq);
		const FString Name = Eq == INDEX_NONE ? Token : Token.Left(Eq);
		if (Name.Len() <= 2) continue;
		FFlag* F = Flags.FindByPredicate([&Name](const FFlag& X) { return X.Name.Equals(Name, ESearchCase::IgnoreCase); });
		if (!F)
		{
			F = &Flags.AddDefaulted_GetRef();
			F->Name = Name;
			F->Token = Name;
		}
		if (Eq == INDEX_NONE) F->bBare = true;
		else if (!F->bValue)
		{
			// (the first value wins, as FParse::Value finds the first one)
			F->bValue = true;
			F->Token = Token;
		}
	}
}

const FOutlawsFlags::FFlag* FOutlawsFlags::Find(const TCHAR* Name) const
{
	for (const FFlag& F : Flags)
	{
		if (FCString::Stricmp(*F.Name, Name) == 0) return &F;
	}
	return nullptr;
}

FString FOutlawsFlags::Line() const
{
	FString L;
	for (const FFlag& F : Flags)
	{
		if (F.bBare) L += TEXT(" -") + F.Name;
		if (F.bValue) L += TEXT(" -") + F.Token;
	}
	return L.IsEmpty() ? FString(TEXT(" (none)")) : L;
}
