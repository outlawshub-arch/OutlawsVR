// The -OLShots test registry (OutlawsShotTests.h).

#include "OutlawsShotTests.h"

#if WITH_OUTLAWS_TESTS

namespace
{
	TArray<FOutlawsShotTest>& Tests()
	{
		static TArray<FOutlawsShotTest> All;
		return All;
	}
	bool bSorted = false;
}

void OutlawsShotTests::Register(const FOutlawsShotTest& Test)
{
	Tests().Add(Test);
	bSorted = false;
}

const FOutlawsShotTest* OutlawsShotTests::Find()
{
	TArray<FOutlawsShotTest>& All = Tests();
	if (!bSorted)
	{
		All.StableSort([](const FOutlawsShotTest& A, const FOutlawsShotTest& B) { return A.Order < B.Order; });
		for (int32 i = 1; i < All.Num(); ++i)
		{
			checkf(All[i].Order != All[i - 1].Order, TEXT("OLShots: %s and %s share order %d"), All[i - 1].Name, All[i].Name, All[i].Order);
		}
		bSorted = true;
	}
	for (const FOutlawsShotTest& T : All)
	{
		if (T.Match()) return &T;
	}
	return nullptr;
}

int32 OutlawsShotTests::Num()
{
	return Tests().Num();
}

#endif // WITH_OUTLAWS_TESTS
