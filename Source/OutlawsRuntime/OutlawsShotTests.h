// The -OLShots tests: each one registers itself from its own file (ShotTests\*.cpp, or the file of the tick it calls) with
// the flags that pick it and its place in the order; AOutlawsPlayer::ShotTest runs the first one, by order, whose flags are
// on the command line.  The order is the old ShotTest's if-chain, top to bottom; "OLShots" (the see-through mids) is last
// and always matches.  Test code only: none of it is in a Shipping build.

#pragma once

#include "CoreMinimal.h"
#include "OutlawsFlags.h"

#if WITH_OUTLAWS_TESTS

class AOutlawsPlayer;
class AOutlawsLevelActor;

struct FOutlawsShotTest
{
	const TCHAR* Name = nullptr;     // the -OL flag that picks it
	int32 Order = 0;                 // lower runs first when two match
	bool (*Match)() = nullptr;
	void (*Run)(AOutlawsPlayer& P, float Dt, AOutlawsLevelActor* L) = nullptr;
};

namespace OutlawsShotTests
{
	void Register(const FOutlawsShotTest& Test);
	/** The first test, by order, whose flags are set (nullptr: none registered). */
	const FOutlawsShotTest* Find();
	int32 Num();
}

struct FOutlawsShotTestRegistrar
{
	explicit FOutlawsShotTestRegistrar(const FOutlawsShotTest& Test) { OutlawsShotTests::Register(Test); }
};

/** The test's flag check: OL_SHOT_MATCH(OLFooTest) { return OLFlags::Param(TEXT("OLFooTest")); } */
#define OL_SHOT_MATCH(Key) static bool OLShotMatch_##Key()

/** The test itself, a member of AOutlawsPlayer (Dt and L as ShotTest had them): OL_SHOT_TEST(OLFooTest, 120) { ... } */
#define OL_SHOT_TEST(Key, Order) \
	template<> void AOutlawsPlayer::ShotCase<Order>(float Dt, AOutlawsLevelActor* L); \
	static void OLShotRun_##Key(AOutlawsPlayer& P, float Dt, AOutlawsLevelActor* L) { P.ShotCase<Order>(Dt, L); } \
	static FOutlawsShotTestRegistrar OLShotReg_##Key(FOutlawsShotTest{ TEXT(#Key), Order, &OLShotMatch_##Key, &OLShotRun_##Key }); \
	template<> void AOutlawsPlayer::ShotCase<Order>(float Dt, AOutlawsLevelActor* L)

#endif // WITH_OUTLAWS_TESTS
