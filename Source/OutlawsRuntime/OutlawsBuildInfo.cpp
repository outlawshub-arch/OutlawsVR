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

// the version the main menu shows (VERSION 0.96; the APK's own is VersionDisplayName / StoreVersion in DefaultEngine.ini)
// and the working name of the builds made as things land after it (BUILDLABEL, user 1 Nov: "0.95 hotfix 2"); a release
// changes these two and nothing else, and an empty label is the plain "v0.96  build <hash>" line again
#define OL_VERSION "0.96"
#define OL_BUILD_LABEL ""

const TCHAR* OutlawsBuildInfo::Version() { return TEXT(OL_VERSION); }
const TCHAR* OutlawsBuildInfo::Label() { return TEXT(OL_BUILD_LABEL); }
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

FString OutlawsBuildInfo::MenuLine()
{
	return FString::Printf(TEXT("v%s%s%s  build %s"), Version(), *Label() ? TEXT(" ") : TEXT(""), Label(), Commit());
}
