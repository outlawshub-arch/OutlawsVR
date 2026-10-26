#include "OutlawsBuildInfo.h"

// written by the packaging scripts (only when it changes, and they touch this file so it is rebuilt)
#if defined(__has_include)
#if __has_include("OutlawsBuildStamp.h")
#include "OutlawsBuildStamp.h"
#endif
#endif
#ifndef OL_BUILD_COMMIT
#define OL_BUILD_COMMIT "dev"
#endif
#ifndef OL_BUILD_BRANCH
#define OL_BUILD_BRANCH ""
#endif
#ifndef OL_BUILD_DATE
#define OL_BUILD_DATE ""
#endif

const TCHAR* OutlawsBuildInfo::Commit() { return TEXT(OL_BUILD_COMMIT); }
const TCHAR* OutlawsBuildInfo::Branch() { return TEXT(OL_BUILD_BRANCH); }
const TCHAR* OutlawsBuildInfo::Date() { return TEXT(OL_BUILD_DATE); }

FString OutlawsBuildInfo::Line()
{
	FString Extra = Branch();
	if (*Date())
	{
		Extra += (Extra.IsEmpty() ? TEXT("") : TEXT(", ")) + FString(Date());
	}
	return Extra.IsEmpty() ? FString::Printf(TEXT("build %s"), Commit()) : FString::Printf(TEXT("build %s (%s)"), Commit(), *Extra);
}
