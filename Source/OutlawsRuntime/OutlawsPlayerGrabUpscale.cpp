// -OLShots -OLGrabUpscaleTest[=missing] (REQ-GRABUPSCALE): once the level's upscale job is done, the nearest bad guy is held
// before the eyes and the frame on his quad is checked: with the upscales there, his held cell shows its upscaled picture
// (the whole texture, 8x the cell); with -OLGrabUpscaleMissing (the upscales treated as missing) the HD atlas as before.
// Then he is let go and the atlas must be back the next frame.  Screenshots while held and after the let-go
// (Saved/OLShots/ol_grabup_<held|released>_<on|missing>.png).
#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsEnemy.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsGrabUpscale.h"
#include "OutlawsTextures.h"            // (FSpriteAtlas: the full type, not only the forward declaration)
#include "Camera/CameraComponent.h"
#include "MotionControllerComponent.h"
#include "Engine/Texture.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
#include "OutlawsShotTests.h"

#if WITH_OUTLAWS_TESTS
void AOutlawsPlayer::GrabUpscaleTestTick(float Dt)
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L || ShotTime < 0.f) return;
	static TWeakObjectPtr<AOutlawsEnemy> Man;
	static float T0 = 0.f, ShownAt = -1.f, LetGoAt = -1.f;
	static int32 Phase = 0, Fails = 0;
	static FVector Hand;
	const bool bMissing = OutlawsGrabUpscale::PretendMissing();
	const TCHAR* Mode = bMissing ? TEXT("missing") : TEXT("on");
	const FVector Fl = FVector(Camera->GetForwardVector().X, Camera->GetForwardVector().Y, 0.0).GetSafeNormal();
	auto Shot = [&](const TCHAR* What)
	{
		const FString Path = FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("ol_grabup_%s_%s.png"), What, Mode);
		FScreenshotRequest::RequestScreenshot(Path, false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT grabup: screenshot %s"), *Path);
	};
	auto Finish = [&](const TCHAR* Why)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT grabup %s: %s (%d failed checks)"), Mode, Why, Fails);
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	};
	auto TexName = [](UTexture* T) { return T ? FString::Printf(TEXT("%s %dx%d"), *T->GetName(), int32(T->GetSurfaceWidth()), int32(T->GetSurfaceHeight())) : FString(TEXT("none")); };
	// the frame time: the worst of the last 90 frames before the grab, and of the first half second after it
	static TArray<float> Before;
	static float WorstAfter = 0.f;
	if (Phase == 0)
	{
		Before.Add(Dt);
		if (Before.Num() > 90) Before.RemoveAt(0);
		// the level's own upscale job first (a PC that has not made this level's yet makes them now)
		if (ShotTime < 3.f || (OutlawsGrabUpscale::IsBusy() && ShotTime < 240.f)) return;
		AOutlawsEnemy* Pick = nullptr;
		double BestD = TNumericLimits<double>::Max();
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
		{
			AOutlawsEnemy* E = Wk.Get();
			if (!E || E->IsDead() || E->bBoss || E->IsCritter() || E->IsDormant()) continue;
			const double D = FVector::Dist(E->GetActorLocation(), GetActorLocation());
			if (D < BestD) { BestD = D; Pick = E; }
		}
		Man = Pick;
		Hand = Camera->GetComponentLocation() + Fl * 110.0 + FVector(0.0, 0.0, 25.0);
		GripOf(1)->SetWorldLocationAndRotation(Hand, Fl.Rotation());
		bool bOk = false;
		if (Pick)
		{
			Pick->SetActorLocation(Hand + Fl * 30.0, false, nullptr, ETeleportType::TeleportPhysics);
			bOk = TryGrabEnemy(1, Pick);
			TestAxes.Add(TEXT("OLVR_RightGrip"), 1.f);
			bGripArmed[1] = false;
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT grabup %s: man %s (%s), held %d; upscale jobs done %d, HD textures %d"), Mode, Pick ? *Pick->GetName() : TEXT("none"),
			Pick && Pick->GetAtlasRef().IsValid() ? *Pick->GetAtlasRef()->Nwx : TEXT("-"), bOk ? 1 : 0, OutlawsGrabUpscale::JobsDone(), L->bHDTextures ? 1 : 0);
		if (!bOk) { ++Fails; Finish(TEXT("FAIL no man held")); return; }
		T0 = ShotTime;
		Phase = 1;
		return;
	}
	AOutlawsEnemy* E = Man.Get();
	AOutlawsSpriteActor* S = E ? E->GetSprite() : nullptr;
	if (!S) { ++Fails; Finish(TEXT("FAIL the man is gone")); return; }
	const float T = ShotTime - T0;
	HandVelocity[1] = HandWorldVel[1] = FVector::ZeroVector;
	GripOf(1)->SetWorldLocationAndRotation(Hand, Fl.Rotation());
	static FString AfterList;
	if (Phase == 1 && T <= 0.5f)
	{
		WorstAfter = FMath::Max(WorstAfter, Dt);
		AfterList += FString::Printf(TEXT(" %.1f"), Dt * 1000.f);
	}
	if (Phase == 1 && T > 0.5f && T - Dt <= 0.5f)
	{
		float WorstBefore = 0.f, SumBefore = 0.f;
		const int32 From = FMath::Max(0, Before.Num() - 30);
		for (int32 i = From; i < Before.Num(); ++i) { WorstBefore = FMath::Max(WorstBefore, Before[i]); SumBefore += Before[i]; }
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT grabup %s frame time: the 30 frames before the grab worst %.2f ms (mean %.2f); the half second after it worst %.2f ms, each:%s"), Mode,
			WorstBefore * 1000.f, SumBefore * 1000.f / FMath::Max(1, Before.Num() - From), WorstAfter * 1000.f, *AfterList);
	}
	if (Phase == 1)
	{
		UTexture* Held = S->HeldLookShown();
		if (Held && ShownAt < 0.f)
		{
			ShownAt = T;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT grabup %s: the held look on at %.3f s after the grab: %s on cell %d"), Mode, T, *TexName(Held), S->CurrentCell);
		}
		// with the upscales: wait for the held look (2 s at most); missing: the HD atlas all along
		if ((!bMissing && ShownAt >= 0.f && T - ShownAt >= 0.6f) || T >= 2.5f)
		{
			UTexture* Quad = S->DebugQuadTexture();
			UTexture* Atlas = S->SpriteTexture;
			const FIntPoint Want = E->GetAtlasRef().IsValid() ? OutlawsGrabUpscale::UpscaledSize(E->GetAtlasRef()->CellW, E->GetAtlasRef()->CellH) : FIntPoint::ZeroValue;
			bool bOk;
			if (bMissing)
			{
				bOk = !S->HasHeldLook() && Quad && Quad == Atlas;
			}
			else
			{
				bOk = Held && Quad == Held && Held != Atlas && int32(Held->GetSurfaceWidth()) == Want.X && int32(Held->GetSurfaceHeight()) == Want.Y;
			}
			Fails += bOk ? 0 : 1;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT grabup %s held: quad shows %s; the atlas is %s; held look %s (wanted %dx%d) on cell %d: %s"), Mode, *TexName(Quad), *TexName(Atlas),
				*TexName(Held), Want.X, Want.Y, S->CurrentCell, bOk ? TEXT("PASS") : TEXT("FAIL"));
			Shot(TEXT("held"));
			Phase = 2;
			LetGoAt = T + 0.5f;                 // (the screenshot taken before the let-go)
		}
	}
	else if (Phase == 2 && T >= LetGoAt)
	{
		TestAxes.Remove(TEXT("OLVR_RightGrip"));
		LetGoEnemy(1, false, nullptr);
		LetGoAt = T;
		Phase = 3;
	}
	else if (Phase == 3 && T - LetGoAt > 0.05f)
	{
		UTexture* Quad = S->DebugQuadTexture();
		const bool bOk = !S->HasHeldLook() && !S->HeldLookShown() && Quad && Quad == S->SpriteTexture && !E->bHeldByPlayer;
		Fails += bOk ? 0 : 1;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT grabup %s let go: held %d, quad shows %s, the atlas %s: %s"), Mode, E->bHeldByPlayer ? 1 : 0, *TexName(Quad), *TexName(S->SpriteTexture),
			bOk ? TEXT("PASS") : TEXT("FAIL"));
		Phase = 4;
	}
	else if (Phase == 4 && T - LetGoAt > 1.2f)
	{
		Shot(TEXT("released"));
		Phase = 5;
	}
	else if (Phase == 5 && T - LetGoAt > 1.8f)
	{
		Finish(Fails == 0 ? TEXT("PASS") : TEXT("FAIL"));
	}
	if (T > 20.f) { ++Fails; Finish(TEXT("FAIL timed out")); }
}

OL_SHOT_MATCH(OLGrabUpscaleTest) { return OLFlags::Param(TEXT("OLGrabUpscaleTest")); }
OL_SHOT_TEST(OLGrabUpscaleTest, 40)
{
	GrabUpscaleTestTick(Dt);         // (REQ-GRABUPSCALE: OutlawsPlayerGrabUpscale.cpp)
}
#endif // WITH_OUTLAWS_TESTS
