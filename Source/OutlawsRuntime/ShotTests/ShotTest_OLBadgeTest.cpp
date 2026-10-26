// -OLShots -OLBadgeTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLBadgeTest) { return OLFlags::Param(TEXT("OLBadgeTest")); }

OL_SHOT_TEST(OLBadgeTest, 1160)
{
	// the badge: a 2 point hit costs 1, its sprite exists, and it runs out
	if (ShotStep == 0 && ShotTime > 3.f && L)
	{
		L->BadgeLeft = 1.5f;
		const float Before = L->GetHealth();
		L->bInvincible = false;
		L->DamagePlayer(2.f, 1.f, true);
		L->bInvincible = true;
		AOutlawsSpriteActor* S = L->MakeLooseSprite(TEXT("ibadge.nwx"), Camera->GetComponentLocation() + Camera->GetForwardVector() * 60.0);
		UE_LOG(LogOutlaws, Display, TEXT("OLRT badge test: health %.1f -> %.1f (a 2 point hit), damage scale %.1f, sprite %s %.0f cm"), Before, L->GetHealth(), L->PlayerDamageScale(),
			S ? TEXT("made") : TEXT("MISSING"), S ? float(S->GetActorScale3D().Z) : 0.f);
		++ShotStep;
	}
	else if (ShotStep == 1 && ShotTime > 5.5f && L)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLRT badge test: after 2.5 s power %d, damage scale %.1f"), L->HasBadgePower() ? 1 : 0, L->PlayerDamageScale());
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	}
}

#endif // WITH_OUTLAWS_TESTS
