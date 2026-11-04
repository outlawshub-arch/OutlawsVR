// BUG-TORCHZ (user 3 Nov, snapshot 2026-11-03_19-42-07, frostbi: a wall torch in mid-air under the glow painted for it, "a z-axis issue
// with how we're placing objects, or for this particular object it just doesn't seem to be sitting high enough"):
// -OLShots -OLLevel=<frostbi|...> -OLNoFrontEnd [-OLNativeMaps=<dir>] -OLTorchZProbe [-OLTorchZOf=<name>] [-OLOldTorchZ] [-OLTorchCensus].  The level's
// hung sprites, each with its point, its insertion point and how far it rises; in frostbi from the snapshot's spot, elsewhere from a
// few metres off the first that rises, a shot with the sprite and one without it (torchz_<level>_<new|old>[_bare].png in
// Saved\OLShots): what the two disagree on is the sprite on the screen.  In frostbi the wall's painted glow is the brightest band of
// rows behind it in the bare shot, and the flame (the sprite's top quarter) has to sit in it.  -OLTorchCensus: every level's sprites
// hung by name alone (the kind that moves), and pickups of the same names (they stay).  "OLSHOT torchz RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsLevel.h"
#include "OutlawsLab.h"
#include "OutlawsFormats.h"
#include "OutlawsGeometry.h"
#include "OutlawsTextures.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/Controller.h"
#include "CollisionQueryParams.h"
#include "Engine/World.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

namespace
{
	/** A shot's pixels, read back from its PNG (the offscreen viewport reads back black). */
	bool TorchShotPixels(const FString& File, TArray<FColor>& Px, int32& W, int32& H)
	{
		TArray<uint8> Bytes;
		TArray64<uint8> Raw;
		if (!FFileHelper::LoadFileToArray(Bytes, *File, FILEREAD_Silent)) return false;
		IImageWrapperModule& Mod = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> Png = Mod.CreateImageWrapper(EImageFormat::PNG);
		if (!Png.IsValid() || !Png->SetCompressed(Bytes.GetData(), Bytes.Num()) || !Png->GetRaw(ERGBFormat::BGRA, 8, Raw)) return false;
		W = int32(Png->GetWidth());
		H = int32(Png->GetHeight());
		if (W <= 0 || H <= 0 || Raw.Num() != int64(W) * H * 4) return false;
		Px.SetNumUninitialized(W * H);
		FMemory::Memcpy(Px.GetData(), Raw.GetData(), Raw.Num());
		return true;
	}

	/** Every level's sprites hung by name alone, by the level's own files: how many, their cell, their point and the rise. */
	void TorchCensus(const FOutlawsAssets& Shown)
	{
		TSharedPtr<FOutlawsAssets> A = FOutlawsAssets::GetShared(Shown.GetRoot());
		if (!A) return;
		TArray<FString> Levels;
		A->ListByExtension(TEXT(".lvt"), Levels);
		for (FString& Lv : Levels) Lv = Lv.LeftChop(4);
		const int32 NumStock = Levels.Num();
		TArray<FString> Natives;
		A->NativeStampList().ParseIntoArray(Natives, TEXT("~"));
		for (const FString& N : Natives) { FString Lv, Stamp; if (N.Split(TEXT("."), &Lv, &Stamp, ESearchCase::IgnoreCase, ESearchDir::FromEnd)) Levels.Add(Lv); }
		static const int32 Owner = 0;
		int32 Total = 0, LevelsHit = 0, Pickups = 0, Pokes = 0;
		TMap<FString, int32> AllNames;
		for (int32 i = 0; i < Levels.Num(); ++i)
		{
			const FString& Lv = Levels[i];
			A->UseLevelOverlay(Lv, &Owner);
			FString Text, Obt;
			FOutlawsLevel L;
			if (!A->GetText(Lv + TEXT(".lvt"), Text) || !L.ParseLvt(Text) || !A->GetText(Lv + TEXT(".obt"), Obt)) continue;
			L.ParseObt(Obt);
			TArray<FColor> Palette;
			OutlawsFormats::FindSpritePalette(*A, Palette);
			TMap<FString, int32> Count;
			TMap<FString, FString> What;
			TMap<FString, double> RiseCm;
			struct FRoom { int32 Poke = 0; double MinGap = 1e9, MaxGap = -1e9, MinDrop = 1e9; };
			TMap<FString, FRoom> Rooms;
			for (const FOutlawsObject& O : L.Objects)
			{
				FString Itm, Func;
				const FString Anim = OutlawsFormats::ResolveAnim(*A, O.Name, &Itm);
				if (!Anim.EndsWith(TEXT(".nwx"), ESearchCase::IgnoreCase) || !OutlawsFormats::IsHanging(O.Name, Itm)) continue;
				TMap<FString, FString> Data;
				OutlawsFormats::ParseItm(Itm, Func, Data);
				if (Data.Contains(TEXT("PICKUP"))) { ++Pickups; continue; }
				if (Data.Contains(TEXT("HEIGHT")) && FCString::Atof(*Data[TEXT("HEIGHT")]) < 0.f) continue;
				const FString Key = O.Name.ToUpper();
				if (!What.Contains(Key))
				{
					TSharedPtr<OutlawsTextures::FSpriteAtlas> At = OutlawsTextures::BuildSpriteAtlas(*A, Anim, Palette, false);
					const int32 Cell0 = At.IsValid() && At->Grid.Num() > 0 ? At->Grid[0] : 0;
					const int32 Ins = At.IsValid() && At->InsBottomPx.IsValidIndex(Cell0) ? At->InsBottomPx[Cell0] : 0;
					const int32 CellH = At.IsValid() ? At->CellH : 0;
					What.Add(Key, FString::Printf(TEXT("cell %d px, %d px under its point, rises %.0f cm"), CellH, Ins, (CellH - Ins) / 16.0 * FOutlawsGeometry::UnitsToCm));
					RiseCm.Add(Key, (CellH - Ins) / 16.0 * FOutlawsGeometry::UnitsToCm);
				}
				// the room round it: its cell's top over its point against the ceiling over it
				const FOutlawsSector* Sec = L.SectorById(O.SectorId);
				if (!Sec) Sec = L.SectorAt(O.Pos.X, O.Pos.Y, O.Pos.Z);
				if (Sec)
				{
					const FVector2D Plan(O.Pos.X, O.Pos.Z);
					const double Gap = (L.SurfaceY(*Sec, Plan, false) - O.Pos.Y) * FOutlawsGeometry::UnitsToCm;
					const double Drop = (O.Pos.Y - L.SurfaceY(*Sec, Plan, true)) * FOutlawsGeometry::UnitsToCm;
					FRoom& R = Rooms.FindOrAdd(Key);
					R.MinGap = FMath::Min(R.MinGap, Gap);
					R.MaxGap = FMath::Max(R.MaxGap, Gap);
					R.MinDrop = FMath::Min(R.MinDrop, Drop);
					R.Poke += RiseCm[Key] > Gap + 15.0 ? 1 : 0;
				}
				Count.FindOrAdd(Key)++;
				AllNames.FindOrAdd(Key)++;
			}
			if (Count.Num() == 0) continue;
			++LevelsHit;
			FString List;
			for (const TPair<FString, int32>& Kv : Count)
			{
				const FRoom R = Rooms.FindRef(Kv.Key);
				List += FString::Printf(TEXT(" %s x%d (%s; its point %.0f to %.0f cm under the ceiling, %.0f over the floor; %d of its tops through the ceiling);"), *Kv.Key, Kv.Value, *What[Kv.Key], R.MinGap, R.MaxGap, R.MinDrop, R.Poke);
				Total += Kv.Value;
				Pokes += R.Poke;
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT torchz census %s (%s):%s"), *Lv, i < NumStock ? TEXT("stock") : TEXT("native"), *List);
		}
		A->CloseOverlay(&Owner);
		FString Names;
		for (const TPair<FString, int32>& Kv : AllNames) Names += FString::Printf(TEXT(" %s x%d"), *Kv.Key, Kv.Value);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT torchz census done: %d levels, %d with sprites hung by name, %d objects (%d tops through a ceiling):%s; %d pickups of such names stay"), Levels.Num(), LevelsHit, Total, Pokes, *Names, Pickups);
	}
}

OL_SHOT_MATCH(OLTorchZProbe) { return OLFlags::Param(TEXT("OLTorchZProbe")); }

OL_SHOT_TEST(OLTorchZProbe, 2170)
{
	static int32 Passes = 0, Fails = 0;
	static float Wait = 0.f, Quiet = -1.f;
	static FRotator Look = FRotator::ZeroRotator;
	static TWeakObjectPtr<AActor> Target;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT torchz check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	UWorld* W = GetWorld();
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	const bool bOld = OLFlags::Param(TEXT("OLOldTorchZ"));
	const FString Lv = L->LevelName.ToLower();
	auto FileOf = [&](const TCHAR* Pose) { return FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("torchz_%s_%s%s.png"), *Lv, bOld ? TEXT("old") : TEXT("new"), Pose); };
	auto Hold = [&]() { if (Controller) Controller->SetControlRotation(Look); };
	switch (ShotStep)
	{
	case 0:
	{
		if (!bUp || !W || ShotTime < 6.f || !TestShadersReady(Quiet, 3.f)) return;
		L->bInvincible = true;
		const TArray<AOutlawsLevelActor::FTestHung>& Hung = L->GetTestHung();
		int32 ByName = 0, Rise = 0, OffPoint = 0;
		for (const AOutlawsLevelActor::FTestHung& H : Hung)
		{
			const double InsAt = H.Bottom + (H.CellH > 0 ? H.Ins * H.Hcm / H.CellH : 0.0);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT torchz hung %s%s at Z %.1f: cell %d px, %d px under its point, the cell's bottom at %.1f, its point at %.1f (%.1f over the top-on-point rule)"),
				*H.Name, H.bByName ? TEXT(" (by name)") : TEXT(" (negative HEIGHT)"), H.Point.Z, H.CellH, H.Ins, H.Bottom, InsAt, H.Bottom - (H.Point.Z - H.Hcm));
			ByName += H.bByName ? 1 : 0;
			Rise += H.bByName && H.Bottom - (H.Point.Z - H.Hcm) > 1.0 ? 1 : 0;
			OffPoint += FMath::Abs(InsAt - H.Point.Z) > 1.0 ? 1 : 0;
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT torchz level %s: %d hung sprites, %d by name alone, %d of them risen%s"), *Lv, Hung.Num(), ByName, Rise, bOld ? TEXT(" (-OLOldTorchZ)") : TEXT(""));
		if (!bOld) Check(OffPoint == 0, FString::Printf(TEXT("every hung sprite draws on its insertion point (%d of %d off it)"), OffPoint, Hung.Num()));
		FVector At = GetActorLocation();
		Look = GetControlRotation();
		const double Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsTorchZ), false, this);
		if (Lv == TEXT("frostbi"))
		{
			// the snapshot's feet and look, and the torch nearest them
			At = FVector(1871.435, 198.562, -369.339);
			Look = FRotator(6.3f, -139.4f, 0.f);
			double Best = 1e12;
			for (const AOutlawsLevelActor::FTestHung& H : Hung)
			{
				const double D = FVector::Dist(H.Point, At);
				if (H.Actor.IsValid() && H.bByName && D < Best) { Best = D; Target = H.Actor; }
			}
		}
		else
		{
			// -OLTorchZOf=<name>: that sprite, not the first: its whole height in the frame, from rings as far off as it takes, aimed
			// at its middle (armad's AHANG stands over 8 m tall, 3.2 m from it is not a place to see it from)
			FString Of;
			OLFlags::Value(TEXT("OLTorchZOf"), Of);
			for (const AOutlawsLevelActor::FTestHung& H : Hung)
			{
				if (!H.Actor.IsValid() || !H.bByName || (!Of.IsEmpty() && !H.Name.Equals(Of, ESearchCase::IgnoreCase))) continue;
				// (the middle of where the rule puts it, on either look: the same view new and -OLOldTorchZ)
				const double OnPoint = H.Point.Z - (H.CellH > 0 ? H.Ins * H.Hcm / H.CellH : 0.0);
				const FVector Aim = Of.IsEmpty() ? H.Point - FVector(0.0, 0.0, 20.0) : FVector(H.Point.X, H.Point.Y, OnPoint + H.Hcm * 0.5);
				const double Ring = Of.IsEmpty() ? 320.0 : FMath::Max(320.0, H.Hcm);
				bool bFound = false;
				for (int32 r = 0; r < (Of.IsEmpty() ? 1 : 4) && !bFound; ++r)
				{
					for (int32 k = 0; k < 16 && !bFound; ++k)
					{
						const FVector Pt = Aim + FVector(0.0, 0.0, 20.0) + FRotator(0.0, 22.5 * k, 0.0).Vector() * Ring * (1.0 + 0.5 * r);
						FHitResult Hit;
						if (!W->LineTraceSingleByChannel(Hit, Pt, Pt - FVector(0.0, 0.0, 4000.0), ECC_Visibility, Q)) continue;
						const FVector Eye = Hit.ImpactPoint + FVector(0.0, 0.0, 170.0);
						if (!L->DebugCanSee(Eye, Aim, nullptr)) continue;
						At = Hit.ImpactPoint + FVector(0.0, 0.0, Half + 5.0);
						Look = (Aim - Eye).Rotation();
						Target = H.Actor;
						bFound = true;
					}
				}
				if (bFound) break;
			}
		}
		TeleportTo(At, FRotator(0.0, Look.Yaw, 0.0), false, true);
		Hold();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT torchz place: at %s, look %s, the sprite %s"), *At.ToString(), *Look.ToString(), Target.IsValid() ? *Target->GetActorLocation().ToString() : TEXT("none"));
		Wait = ShotTime + 2.5f;
		ShotStep = 1;
		return;
	}
	case 1:
		Hold();
		if (ShotTime < Wait) return;
		FScreenshotRequest::RequestScreenshot(FileOf(TEXT("")), false, false);
		Wait = ShotTime + 1.5f;
		ShotStep = 2;
		return;
	case 2:
		Hold();
		if (ShotTime < Wait) return;
		if (Target.IsValid()) Target->SetActorHiddenInGame(true);
		Wait = ShotTime + 1.5f;
		ShotStep = 3;
		return;
	case 3:
		Hold();
		if (ShotTime < Wait) return;
		FScreenshotRequest::RequestScreenshot(FileOf(TEXT("_bare")), false, false);
		Wait = ShotTime + 2.f;
		ShotStep = 4;
		return;
	case 4:
	{
		Hold();
		if (ShotTime < Wait) return;
		TArray<FColor> With, Bare;
		int32 Vw = 0, Vh = 0;
		if (Target.IsValid() && TorchShotPixels(FileOf(TEXT("")), With, Vw, Vh) && TorchShotPixels(FileOf(TEXT("_bare")), Bare, Vw, Vh) && With.Num() == Bare.Num())
		{
			// the sprite: the changed pixels round their middle, row by row and column by column from it while they run on (its
			// light goes with it, so a little of the room changes too)
			TArray<int32> Xs, Ys;
			for (int32 y = 0; y < Vh; ++y)
			{
				for (int32 x = 0; x < Vw; ++x)
				{
					const FColor& A = With[y * Vw + x];
					const FColor& B = Bare[y * Vw + x];
					if (FMath::Abs(int32(A.R) - B.R) + FMath::Abs(int32(A.G) - B.G) + FMath::Abs(int32(A.B) - B.B) <= 60) continue;
					Xs.Add(x); Ys.Add(y);
				}
			}
			int32 X0 = Vw, X1 = -1, Y0 = Vh, Y1 = -1, N = 0;
			if (Xs.Num() > 0)
			{
				TArray<int32> Sx = Xs, Sy = Ys;
				Sx.Sort(); Sy.Sort();
				const int32 Mx = Sx[Sx.Num() / 2], My = Sy[Sy.Num() / 2];
				TArray<int32> PerRow, PerCol;
				PerRow.SetNumZeroed(Vh);
				PerCol.SetNumZeroed(Vw);
				for (int32 i = 0; i < Xs.Num(); ++i)
				{
					if (FMath::Abs(Xs[i] - Mx) > 160 || FMath::Abs(Ys[i] - My) > 300) continue;
					PerRow[Ys[i]]++; PerCol[Xs[i]]++; ++N;
				}
				auto Runs = [](const TArray<int32>& Per, int32 From, int32 Step)
				{
					int32 At = From, Gap = 0;
					for (int32 i = From + Step; Per.IsValidIndex(i) && Gap < 6; i += Step)
					{
						if (Per[i] >= 2) { At = i; Gap = 0; } else ++Gap;
					}
					return At;
				};
				Y0 = Runs(PerRow, My, -1); Y1 = Runs(PerRow, My, 1);
				X0 = Runs(PerCol, Mx, -1); X1 = Runs(PerCol, Mx, 1);
			}
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT torchz sprite on the screen: columns %d to %d, rows %d to %d, %d pixels"), X0, X1, Y0, Y1, N);
			Check(N > 50, FString::Printf(TEXT("the sprite is in view (%d pixels)"), N));
			if (Lv == TEXT("frostbi") && N > 50)
			{
				// the painted glow: the rows of the bare wall behind it (the sprite's columns, a sprite's width each side) much
				// brighter than the wall below it
				const int32 Span = (X1 - X0 + 1), Lo = FMath::Max(0, X0 - Span), Hi = FMath::Min(Vw - 1, X1 + Span);
				TArray<double> Row;
				Row.SetNumZeroed(Vh);
				for (int32 y = 0; y < Vh; ++y)
				{
					for (int32 x = Lo; x <= Hi; ++x) Row[y] += Bare[y * Vw + x].R * 0.3 + Bare[y * Vw + x].G * 0.6 + Bare[y * Vw + x].B * 0.1;
					Row[y] /= Hi - Lo + 1;
				}
				// (the wall's rows only: from the sprite's foot up, as far as the rows run on before the ceiling's dark)
				int32 Peak = Y1;
				for (int32 y = FMath::Min(Vh - 1, Y1); y >= 0; --y) if (Row[y] > Row[Peak]) Peak = y;
				const double Base = Row[FMath::Min(Vh - 1, Y1)], Half2 = Base + (Row[Peak] - Base) * 0.4;
				int32 GlowTop = Peak, GlowBottom = Peak;
				while (GlowTop > 0 && Row[GlowTop - 1] > Half2) --GlowTop;
				while (GlowBottom + 1 < Vh && Row[GlowBottom + 1] > Half2) ++GlowBottom;
				const int32 Height = Y1 - Y0 + 1, Flame = Y0 + Height / 8;
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT torchz glow: rows %d to %d (peak %.0f at row %d, the wall under the sprite %.0f); the flame at row %d, the sprite %d rows tall"), GlowTop, GlowBottom, Row[Peak], Peak, Base, Flame, Height);
				if (!bOld) Check(Flame >= GlowTop && Flame <= GlowBottom + Height / 4, FString::Printf(TEXT("the flame sits in the glow (row %d, the glow's rows %d to %d)"), Flame, GlowTop, GlowBottom));
				else UE_LOG(LogOutlaws, Display, TEXT("OLSHOT torchz old: the flame %d rows under the glow's foot"), Flame - GlowBottom);
			}
		}
		else if (Lv == TEXT("frostbi")) Check(false, TEXT("no shot of the torch"));
		else UE_LOG(LogOutlaws, Display, TEXT("OLSHOT torchz no place to see a hung sprite from here"));
		Wait = ShotTime + 0.5f;
		ShotStep = 5;
		return;
	}
	case 5:
		if (ShotTime < Wait) return;
		if (OLFlags::Param(TEXT("OLTorchCensus")) && L->GetAssets()) TorchCensus(*L->GetAssets());
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT torchz RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
