// -OLShots -OLRedPlusProbe (REQ-REDPLUS, the user 8 Nov: the cross on the canteen and the medical bags red for everyone, as the
// launcher's Red Plus Items mod draws it, by a recolour at runtime; a Display row Plus Colour, Red by default; -OLOldPlusColour:
// the stock blue).  In -OLLevel (hideout), for GCANTEEN, GDOCBAG and IDOCBAG:
//  - cpu: the HD sprite as DecodeSpriteAtlasHdPixels gives it (the 3D models' paint, the icons) with the row on Blue and on Red.
//    The cross is the Blue picture's blue texels (hue 190 to 250, saturation 0.4 up, value 0.2 up); on Red it must hold no more
//    than 2 % of them blue, its mean must be red (GCANTEEN, GDOCBAG) or the 1997 sprite's orange (IDOCBAG), and no texel away
//    from the blues may change.
//  - gpu: the HD atlas the level draws the sprite and the wrist icon with (SpriteIcon, a render target), read back: its strong
//    blue texels on Red no more than 2 % of those on Blue.
//  - paint: the 3D canteen's and bag's texture painted from the sprite (OutlawsItem3d::Paint), the same rule.
//  - the row: Red by default, PlusColour=Blue read from the settings and written back, and the credits line.
// "OLSHOT redplus <stem> <part>: ..." per measure, "OLSHOT redplus check <what>: PASS|FAIL", "OLSHOT redplus RESULT <n> PASS, <m>
// FAIL".  With -OLOldPlusColour the Red picture stays blue: must FAIL.
// -OLRedPlusShots: also the three in front of the eye (the two ground items as the game puts them down, and the three sprites on
// their own), Saved\OLShots\redplus_<new|old>_<sm5|es31>.png, the painted 3D textures, redplus_paint_<stem>_<new|old>.png, and
// the HD sprites as decoded on Red, redplus_sprite_<stem>_<new|old>.png (the player's own art: local, never committed).

#include "ShotTestCommon.h"
#include "OutlawsPlusColour.h"
#include "OutlawsItem3d.h"
#include "Engine/TextureRenderTarget2D.h"
#include "RHI.h"

#if WITH_OUTLAWS_TESTS

namespace
{
	struct FHsv { float H = 0.f, S = 0.f, V = 0.f; };
	FHsv HsvOf(const FColor& C)
	{
		const float R = C.R / 255.f, G = C.G / 255.f, B = C.B / 255.f;
		const float Mx = FMath::Max3(R, G, B), Mn = FMath::Min3(R, G, B), D = Mx - Mn;
		FHsv O;
		O.V = Mx;
		O.S = Mx > 0.f ? D / Mx : 0.f;
		if (D <= 1e-5f) return O;
		if (Mx == R) O.H = 60.f * FMath::Fmod((G - B) / D + 6.f, 6.f);
		else if (Mx == G) O.H = 60.f * ((B - R) / D + 2.f);
		else O.H = 60.f * ((R - G) / D + 4.f);
		return O;
	}
	bool IsCrossBlue(const FColor& C) { const FHsv H = HsvOf(C); return C.A >= 128 && H.H >= 190.f && H.H <= 250.f && H.S >= 0.4f && H.V >= 0.2f; }
	bool IsStrongBlue(const FColor& C) { const FHsv H = HsvOf(C); return C.A >= 128 && H.H >= 190.f && H.H <= 250.f && H.S >= 0.5f && H.V >= 0.3f; }
	bool NearBlue(const FColor& C) { const FHsv H = HsvOf(C); return H.H >= 170.f && H.H <= 270.f && H.S >= 0.15f; }
	int32 CountStrongBlue(const TArray<FColor>& Px) { int32 N = 0; for (const FColor& C : Px) N += IsStrongBlue(C) ? 1 : 0; return N; }

	bool ReadTexture(UTexture* T, TArray<FColor>& Out)
	{
		Out.Reset();
		UTextureRenderTarget2D* Rt = Cast<UTextureRenderTarget2D>(T);
		FTextureRenderTargetResource* Res = Rt ? Rt->GameThread_GetRenderTargetResource() : nullptr;
		return Res && Res->ReadPixels(Out) && Out.Num() > 0;
	}
}

OL_SHOT_MATCH(OLRedPlusProbe) { return OLFlags::Param(TEXT("OLRedPlusProbe")); }

OL_SHOT_TEST(OLRedPlusProbe, 2600)
{
	static int32 Step = 0, Pass = 0, Fail = 0;
	static double Since = 0.0;
	static const bool bShots = OLFlags::Param(TEXT("OLRedPlusShots"));
	if (!L) return;
	const double Now = FPlatformTime::Seconds();
	const bool bOld = OutlawsPlusColour::OldFlag();
	const TCHAR* Mode = bOld ? TEXT("old") : TEXT("new");
	const TCHAR* Fl = GetWorld() && GetWorld()->FeatureLevel == ERHIFeatureLevel::ES3_1 ? TEXT("es31") : TEXT("sm5");
	auto Check = [](bool bOk, const FString& What)
	{
		(bOk ? Pass : Fail)++;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT redplus check %s: %s"), *What, bOk ? TEXT("PASS") : TEXT("FAIL"));
	};
	if (Step == 0)
	{
		if (ShotTime < 4.f) return;
		Since = Now;
		Step = 1;
		if (!L->bHDTextures) L->SetHDTextures(true);
		const FOutlawsAssets* Labs = L->GetLabAssets();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT redplus start: %s, %s, level %s, row %s, recolour %s"), Mode, Fl, *L->LevelName,
			OutlawsPlusColour::RedChosen() ? TEXT("Red") : TEXT("Blue"), OutlawsPlusColour::Active() ? TEXT("on") : TEXT("off"));
		// the row: Red by default (the sandboxed settings have no PlusColour), and the settings key both ways
		Check(OutlawsPlusColour::RedChosen() && OutlawsDeviceProfile::RowValue(TEXT("r_pluscolour")) == TEXT("Red"),
			FString::Printf(TEXT("the row is Red by default ('%s')"), *OutlawsDeviceProfile::RowValue(TEXT("r_pluscolour"))));
		{
			TMap<FString, FString> Was;
			OutlawsDeviceProfile::GatherSettings(Was);
			TMap<FString, FString> Blue = Was;
			Blue.Add(TEXT("PlusColour"), TEXT("Blue"));
			OutlawsDeviceProfile::LoadSettings(Blue, TEXT("redplus probe"));
			TMap<FString, FString> Again;
			OutlawsDeviceProfile::GatherSettings(Again);
			const bool bBlueRead = !OutlawsPlusColour::RedChosen() && Again.FindRef(TEXT("PlusColour")) == TEXT("Blue");
			OutlawsDeviceProfile::LoadSettings(Was, TEXT("redplus probe"));
			Check(bBlueRead && OutlawsPlusColour::RedChosen() && Was.FindRef(TEXT("PlusColour")) == TEXT("Red"),
				FString::Printf(TEXT("PlusColour in the settings: Blue read and written back (%s), Red again (%s)"), *Again.FindRef(TEXT("PlusColour")), *Was.FindRef(TEXT("PlusColour"))));
		}
		{
			FString Credits;
			FFileHelper::LoadFileToString(Credits, *(FPaths::ProjectContentDir() / TEXT("Credits") / TEXT("vr_credits.txt")));
			Check(Credits.Contains(TEXT("\nRed Plus Items by T4H_Dingle")), TEXT("the credits line 'Red Plus Items by T4H_Dingle'"));
		}
		for (const TCHAR* Stem : { TEXT("GCANTEEN"), TEXT("GDOCBAG"), TEXT("IDOCBAG") })
		{
			const bool bOrange = FCString::Strcmp(Stem, TEXT("IDOCBAG")) == 0;
			const TSharedPtr<OutlawsTextures::FSpriteAtlas> At = L->GetPickupAtlas(FString(Stem) + TEXT(".NWX"));
			// cpu: the HD picture with the row on Blue, then on Red
			TArray<FColor> Stock, Mine;
			int32 W = 0, H = 0, W2 = 0, H2 = 0;
			OutlawsPlusColour::SetRedChosen(false);
			const bool bA = At && Labs && OutlawsTextures::DecodeSpriteAtlasHdPixels(*Labs, *At, Stock, W, H, true);
			OutlawsPlusColour::SetRedChosen(true);
			const bool bB = At && Labs && OutlawsTextures::DecodeSpriteAtlasHdPixels(*Labs, *At, Mine, W2, H2, true);
			if (!bA || !bB || W != W2 || H != H2 || Stock.Num() != Mine.Num())
			{
				Check(false, FString::Printf(TEXT("%s: its HD sprite decodes (atlas %s, blue %d, red %d)"), Stem, At ? TEXT("yes") : TEXT("no"), bA, bB));
				continue;
			}
			if (bShots)
			{
				TArray64<uint8> Png;
				FImageUtils::PNGCompressImageArray(W, H, TArrayView64<const FColor>(Mine.GetData(), Mine.Num()), Png);
				FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("redplus_sprite_%s_%s.png"), Stem, Mode)));
			}
			int32 Area = 0, BlueLeft = 0, Outside = 0;
			uint64 Sum[2][3] = {};
			for (int32 i = 0; i < Stock.Num(); ++i)
			{
				if (IsCrossBlue(Stock[i]))
				{
					++Area;
					BlueLeft += IsCrossBlue(Mine[i]) ? 1 : 0;
					Sum[0][0] += Stock[i].R; Sum[0][1] += Stock[i].G; Sum[0][2] += Stock[i].B;
					Sum[1][0] += Mine[i].R; Sum[1][1] += Mine[i].G; Sum[1][2] += Mine[i].B;
				}
				else if (Stock[i] != Mine[i] && !NearBlue(Stock[i])) ++Outside;
			}
			const int32 A = FMath::Max(1, Area);
			const int32 R1 = int32(Sum[1][0] / A), G1 = int32(Sum[1][1] / A), B1 = int32(Sum[1][2] / A);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT redplus %s cpu: %dx%d, cross %d texels, Blue mean (%d,%d,%d), Red mean (%d,%d,%d), blue left %d, changed away from the cross %d"),
				Stem, W, H, Area, int32(Sum[0][0] / A), int32(Sum[0][1] / A), int32(Sum[0][2] / A), R1, G1, B1, BlueLeft, Outside);
			const bool bHue = bOrange ? (R1 >= B1 + 80 && G1 >= B1 + 30 && R1 > G1) : (R1 >= G1 + 50 && R1 >= B1 + 50);
			Check(Area >= 100, FString::Printf(TEXT("%s cpu: the cross found on Blue (%d texels)"), Stem, Area));
			Check(BlueLeft * 50 <= Area, FString::Printf(TEXT("%s cpu: no blue left on Red (%d of %d)"), Stem, BlueLeft, Area));
			Check(bHue, FString::Printf(TEXT("%s cpu: the cross %s on Red (%d,%d,%d)"), Stem, bOrange ? TEXT("orange") : TEXT("red"), R1, G1, B1));
			Check(Outside == 0, FString::Printf(TEXT("%s cpu: nothing else changed (%d texels)"), Stem, Outside));
			// gpu: the HD atlas the level and the wrist icon draw with, made again on Blue and on Red
			int32 Gpu[2] = { -1, -1 };
			for (int32 k = 0; k < 2; ++k)
			{
				OutlawsPlusColour::SetRedChosen(k == 1);
				L->RefreshPlusColour();
				UTexture* T = nullptr;
				FVector2D Sc, Of;
				float Asp = 1.f;
				TArray<FColor> Px;
				if (L->SpriteIcon(FString(Stem) + TEXT(".NWX"), T, Sc, Of, Asp) && T && ReadTexture(T, Px)) Gpu[k] = CountStrongBlue(Px);
			}
			OutlawsPlusColour::SetRedChosen(true);
			L->RefreshPlusColour();
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT redplus %s gpu: strong blue texels in the HD atlas, Blue %d, Red %d"), Stem, Gpu[0], Gpu[1]);
			Check(Gpu[0] >= 50 && Gpu[1] >= 0 && Gpu[1] * 50 <= Gpu[0], FString::Printf(TEXT("%s gpu: the HD atlas's cross not blue on Red (%d of %d)"), Stem, Gpu[1], Gpu[0]));
			// paint: the 3D model's texture painted from the sprite (the canteen and the bag on the floor)
			if (!bOrange)
			{
				int32 Pn[2] = { -1, -1 };
				for (int32 k = 0; k < 2; ++k)
				{
					OutlawsPlusColour::SetRedChosen(k == 1);
					TArray<FColor> Px;
					int32 Pw = 0, Ph = 0;
					FString Err;
					if (OutlawsItem3d::Paint(L, OutlawsItem3d::ShippedDir() / (FString(Stem) + TEXT("_tex.json")), Px, Pw, Ph, Err))
					{
						Pn[k] = CountStrongBlue(Px);
						if (bShots && k == 1)
						{
							TArray64<uint8> Png;
							FImageUtils::PNGCompressImageArray(Pw, Ph, TArrayView64<const FColor>(Px.GetData(), Px.Num()), Png);
							FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("redplus_paint_%s_%s.png"), Stem, Mode)));
						}
					}
					else UE_LOG(LogOutlaws, Display, TEXT("OLSHOT redplus %s paint: not painted (%s)"), Stem, *Err);
				}
				OutlawsPlusColour::SetRedChosen(true);
				UE_LOG(LogOutlaws, Display, TEXT("OLSHOT redplus %s paint: strong blue texels in the 3D texture, Blue %d, Red %d"), Stem, Pn[0], Pn[1]);
				Check(Pn[0] >= 50 && Pn[1] >= 0 && Pn[1] * 50 <= Pn[0], FString::Printf(TEXT("%s paint: the 3D texture's cross not blue on Red (%d of %d)"), Stem, Pn[1], Pn[0]));
			}
		}
		if (!bShots) Step = 9;
		return;
	}
	if (Step == 1)
	{
		// the shot: a clear way ahead, the two ground items and the three sprites on the floor in front, looked down at
		const FVector Eye = Camera ? Camera->GetComponentLocation() : GetActorLocation();
		float BestYaw = GetActorRotation().Yaw;
		double BestFree = -1.0;
		for (int32 a = 0; a < 8; ++a)
		{
			const float Yaw = GetActorRotation().Yaw + 45.f * a;
			FHitResult Hit;
			FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsRedPlusView), true, this);
			const FVector To = Eye + FRotator(0.f, Yaw, 0.f).Vector() * 400.0;
			const double Free = GetWorld()->LineTraceSingleByChannel(Hit, Eye, To, ECC_Visibility, Q) ? Hit.Distance : 400.0;
			if (Free > BestFree) { BestFree = Free; BestYaw = Yaw; }
		}
		TeleportTo(GetActorLocation(), FRotator(0.f, BestYaw, 0.f), false, true);
		if (Controller) Controller->SetControlRotation(FRotator(-28.f, BestYaw, 0.f));
		const FRotator Y(0.f, BestYaw, 0.f);
		const FVector Fw = Y.Vector(), Rt = FRotationMatrix(Y).GetScaledAxis(EAxis::Y);
		const FVector Feet = GetActorLocation() - FVector(0.0, 0.0, GetCapsuleComponent() ? GetCapsuleComponent()->GetScaledCapsuleHalfHeight() : 90.0);
		const double Near = FMath::Min(150.0, BestFree * 0.45), Far = FMath::Min(230.0, BestFree * 0.75);
		L->SpawnGroundItem(TEXT("gcanteen"), Feet + Fw * Near - Rt * 45.0 + FVector(0.0, 0.0, 40.0));
		L->SpawnGroundItem(TEXT("gdocbag"), Feet + Fw * Near + Rt * 45.0 + FVector(0.0, 0.0, 40.0));
		L->MakeLooseSprite(TEXT("gcanteen.nwx"), Feet + Fw * Far - Rt * 80.0);
		L->MakeLooseSprite(TEXT("gdocbag.nwx"), Feet + Fw * Far);
		L->MakeLooseSprite(TEXT("idocbag.nwx"), Feet + Fw * Far + Rt * 80.0);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT redplus view: yaw %.0f, %.0f cm free ahead, items at %.0f and %.0f cm"), BestYaw, BestFree, Near, Far);
		Since = Now;
		Step = 2;
		return;
	}
	if (Step == 2 && Now - Since > 2.5)
	{
		const FString File = FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("redplus_%s_%s.png"), Mode, Fl);
		FScreenshotRequest::RequestScreenshot(File, false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT redplus screenshot %s"), *File);
		Since = Now;
		Step = 9;
		return;
	}
	if (Step == 9 && Now - Since > 1.5)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT redplus RESULT %d PASS, %d FAIL"), Pass, Fail);
		FGenericPlatformMisc::RequestExit(false);
		Step = 10;
	}
}

#endif // WITH_OUTLAWS_TESTS
