// BUG-TWOFEATHERSHD (user 3 Nov: with Remastered graphics on, Two-Feathers the only one of the six characters drawn with his
// old sprite): -OLShots -OLLevel=town -OLNoFrontEnd -OLTwoFeathersHdProbe [-OLOldTwoFeathersHd].  All six characters' sprites
// made as another player's (UOutlawsCoop::MakePuppetSprite, its line "OLCOOP the other player drawn as ... look"), in a row in
// the longest clear way from the start: each in the remaster's HD look, its grid as it was but for his, and the HD atlas's
// pixels where the 1x atlas has its own (the cells where the grid says they are); -OLOldTwoFeathersHd: him in the original
// look, the other five as ever.  Then the row from the front, twofeathershd_new.png (twofeathershd_old.png) in Saved\OLShots.
// Lines "OLSHOT twofeathershd check PASS|FAIL: ..." and "OLSHOT twofeathershd RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsCoop.h"
#include "OutlawsDeathmatch.h"
#include "OutlawsLevelActor.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsTextures.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "TextureResource.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLTwoFeathersHdProbe) { return OLFlags::Param(TEXT("OLTwoFeathersHdProbe")); }

OL_SHOT_TEST(OLTwoFeathersHdProbe, 2300)
{
	static int32 Passes = 0, Fails = 0;
	static float Wait = 0.f, Quiet = -1.f;
	static double ViewYaw = 0.0;
	if (!L || ShotStep < 0) return;
	L->StartSaveDelay = -1.f;           // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	const bool bOld = OLFlags::Param(TEXT("OLOldTwoFeathersHd"));
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT twofeathershd check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 4.f || !bUp || !TestShadersReady(Quiet, 1.f)) return;
		L->bInvincible = true;
		if (!L->bHDTextures) L->SetHDTextures(true);
		UOutlawsCoop* C = UOutlawsCoop::Get(this);
		if (!C) { Check(false, TEXT("no co-op state")); ShotStep = 2; return; }
		// the longest clear way of the eight, the six across it with their feet on the floor there
		const FVector From = GetActorLocation();
		const FCollisionQueryParams Q(SCENE_QUERY_STAT(OLTwoFeathersHd), false, this);
		double BestFree = 0.0;
		for (int32 i = 0; i < 8; ++i)
		{
			FHitResult Hit;
			const bool bHit = GetWorld()->LineTraceSingleByChannel(Hit, From, From + FRotator(0.0, i * 45.0, 0.0).Vector() * 1000.0, ECC_Visibility, Q);
			const double Free = bHit ? Hit.Distance : 1000.0;
			if (Free > BestFree) { BestFree = Free; ViewYaw = i * 45.0; }
		}
		const FVector Ahead = FRotator(0.0, ViewYaw, 0.0).Vector(), Across = FRotator(0.0, ViewYaw + 90.0, 0.0).Vector();
		const double Dist = FMath::Clamp(BestFree - 60.0, 150.0, 420.0);
		int32 N = 0;
		const FOutlawsMpCharacter* All = FOutlawsMpCharacter::All(N);
		for (int32 i = 0; i < N; ++i)
		{
			const FOutlawsMpCharacter& Ch = All[i];
			const bool bHim = FString(Ch.Nwx).Equals(TEXT("twofeath.nwx"), ESearchCase::IgnoreCase);
			const FVector Mid = From + Ahead * Dist + Across * ((i - (N - 1) * 0.5) * 130.0);
			FHitResult Floor;
			const bool bFloor = GetWorld()->LineTraceSingleByChannel(Floor, Mid + FVector(0.0, 0.0, 150.0), Mid - FVector(0.0, 0.0, 400.0), ECC_Visibility, Q);
			const AOutlawsSpriteActor* S = C->MakePuppetSprite(L, bFloor ? FVector(Floor.Location) : Mid - FVector(0.0, 0.0, 60.0), float(ViewYaw + 180.0), Ch.Nwx);
			const bool bHd = S && S->SpriteTextureLo && S->SpriteTexture != S->SpriteTextureLo;
			const TSharedPtr<OutlawsTextures::FSpriteAtlas> A = L->LoadSpriteSet(Ch.Nwx);
			const bool bSquare = A && A->Cols == FMath::CeilToInt(FMath::Sqrt(float(A->Cells)));
			Check(S && bHd == !(bOld && bHim) && bSquare == (bOld || !bHim),
				FString::Printf(TEXT("%s (%s): the %s look, %d cells of %dx%d px laid %d x %d (%s), HD at %dx"), Ch.Name, Ch.Nwx, bHd ? TEXT("remaster's HD") : TEXT("original"),
				A ? A->Cells : 0, A ? A->CellW : 0, A ? A->CellH : 0, A ? A->Cols : 0, A ? A->Rows : 0, bSquare ? TEXT("the square grid") : TEXT("not the square grid"), A ? A->HdScale : 0));
			// the HD atlas's cells where the grid says they are: its pixels over the 1x atlas's drawn ones, against the same
			// read one cell along, a grid off by a cell (the remaster's art is redrawn, so even a right grid is short of 100 %:
			// the five unchanged atlases read 82 to 92 %)
			UTextureRenderTarget2D* Rt = A ? Cast<UTextureRenderTarget2D>(A->TextureHd) : nullptr;
			FTextureRenderTargetResource* Res = Rt ? Rt->GameThread_GetRenderTargetResource() : nullptr;
			if (!Res) continue;
			TArray<FColor> Px;
			Res->ReadPixels(Px);
			const int32 Hs = A->HdScale, Hw = Rt->SizeX;
			int64 Drawn = 0, Over = 0, OverOff = 0;
			for (int32 y = 0; y < A->AtlasH; ++y)
			{
				for (int32 x = 0; x < A->AtlasW; ++x)
				{
					if (!A->Alpha[y * A->AtlasW + x]) continue;
					++Drawn;
					const int32 At = (y * Hs + Hs / 2) * Hw + x * Hs + Hs / 2;
					const int32 Off = (y * Hs + Hs / 2) * Hw + ((x + A->CellW) % A->AtlasW) * Hs + Hs / 2;
					Over += Px.IsValidIndex(At) && Px[At].A > 0;
					OverOff += Px.IsValidIndex(Off) && Px[Off].A > 0;
				}
			}
			const float Share = Drawn ? float(Over) / Drawn : 0.f, ShareOff = Drawn ? float(OverOff) / Drawn : 0.f;
			Check(Hw == A->AtlasW * Hs && Rt->SizeY == A->AtlasH * Hs && Rt->SizeX <= 4096 && Rt->SizeY <= 4096 && Share > ShareOff,
				FString::Printf(TEXT("%s: the HD atlas %dx%d draws over %.1f %% of the 1x atlas's %lld drawn px (one cell off: %.1f %%)"), Ch.Name, Rt->SizeX, Rt->SizeY, Share * 100.f, Drawn, ShareOff * 100.f));
		}
		Check(N == 6, FString::Printf(TEXT("%d characters"), N));
		// (the row in view, the eyes a little down)
		if (Controller) Controller->SetControlRotation(FRotator(-6.0, ViewYaw, 0.0));
		Wait = ShotTime + 2.f;
		ShotStep = 1;
		return;
	}
	case 1:
	{
		if (ShotTime < Wait) return;
		const FString Name = bOld ? TEXT("twofeathershd_old") : TEXT("twofeathershd_new");
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT twofeathershd shot %s.png (looking %.0f)"), *Name, ViewYaw);
		Wait = ShotTime + 1.5f;
		ShotStep = 2;
		return;
	}
	case 2:
		if (ShotTime < Wait) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT twofeathershd RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	}
}

#endif
