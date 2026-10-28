// BUG-SCROLLSRGB (found by another lane, 28 Oct): every scrolling surface drew brighter than its sector's light after its first
// slide, the still faces beside it not.  -OLShots -OLLevel=<map> -OLNoFrontEnd -OLScrollSrgbProbe [-OLOldScrollSRGB]
// [-featureleveles31]: after 4 s of play every scroll section's vertex colours as the GPU has them against the colours the build
// gave them, as the still faces have theirs (no sRGB conversion); then a shot of the largest scrolling surface from 4 m,
// scrollsrgb_<map>[_old][_es31].png in Saved\OLShots.  Lines "OLSHOT scrollsrgb check PASS|FAIL: ..." and "OLSHOT scrollsrgb RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "GameFramework/Controller.h"
#include "Engine/World.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLScrollSrgbProbe) { return OLFlags::Param(TEXT("OLScrollSrgbProbe")); }

OL_SHOT_TEST(OLScrollSrgbProbe, 1244)
{
	static int32 Passes = 0, Fails = 0;
	static float Wait = 0.f;
	static FVector At = FVector::ZeroVector, Facing = FVector::ForwardVector;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT scrollsrgb check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	const bool bOld = OLFlags::Param(TEXT("OLOldScrollSRGB"));
	const bool bEs31 = GetWorld() && GetWorld()->FeatureLevel == ERHIFeatureLevel::ES3_1;
	auto Place = [this]()
	{
		const FVector From = At + Facing * 400.0 + FVector(0.0, 0.0, 60.0);
		const FRotator Look = (At - From).Rotation();
		TeleportTo(From - FVector(0.0, 0.0, 60.0), FRotator(0.0, Look.Yaw, 0.0), false, true);
		if (Controller) Controller->SetControlRotation(Look);
	};
	switch (ShotStep)
	{
	case 0:
		if (ShotTime < 4.f || !bUp) return;
		L->bInvincible = true;
		Wait = ShotTime + 4.f;           // (the scrolls slide a while: each section updated)
		ShotStep = 1;
		return;
	case 1:
	{
		if (ShotTime < Wait) return;
		// the sections as drawn against the build's colours: unconverted (the still faces' rule), and converted (the old)
		int32 Sections = 0, Updated = 0, Verts = 0, AsStill = 0, AsOld = 0, Lit = 0;
		FString Where;
		L->DebugScrollColours(Sections, Updated, Verts, AsStill, AsOld, Lit, At, Facing, Where);
		if (bOld)
			Check(Sections > 0 && Updated > 0 && AsOld == Verts && (Lit == 0 || AsStill < Verts), FString::Printf(TEXT("%s (-OLOldScrollSRGB): %d scroll sections, %d slid; of %d vertices %d brighter than the build's (sRGB-converted), %d as the still faces, %d not black or white"),
				*L->LevelName, Sections, Updated, Verts, AsOld, AsStill, Lit));
		else
			Check(Sections > 0 && Updated > 0 && AsStill == Verts, FString::Printf(TEXT("%s: %d scroll sections, %d slid; of %d vertices %d as the build gave them (as the still faces), %d as the old sRGB conversion, %d not black or white"),
				*L->LevelName, Sections, Updated, Verts, AsStill, AsOld, Lit));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT scrollsrgb the largest scrolling surface: %s"), *Where);
		Place();
		Wait = ShotTime + 1.5f;
		ShotStep = 2;
		return;
	}
	case 2:
	{
		if (ShotTime < Wait) return;
		Place();
		const FString Name = FString::Printf(TEXT("scrollsrgb_%s%s%s"), *L->LevelName, bOld ? TEXT("_old") : TEXT(""), bEs31 ? TEXT("_es31") : TEXT(""));
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT scrollsrgb shot %s.png"), *Name);
		Wait = ShotTime + 1.5f;
		ShotStep = 3;
		return;
	}
	case 3:
		if (ShotTime < Wait) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT scrollsrgb RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
