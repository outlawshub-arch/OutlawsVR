// REQ-WHEELSCORE (user 3 Nov: in a match the game stats move out of the pause menu to under the weapon wheel, tilted up toward
// the eyes) probe, flat:
//   a launched match host on its own: -OLShots -OLMpHost=drygulch -OLMpClass=dm|team -OLMpBots=0 -OLMpStartWhen=0
//   -OLLobbyEnterAfter=4 -OLWheelScoreProbe [-OLOldWheelScore] [-ResX=1832 -ResY=1920 -ForceRes (Q2) | -ResX=2064 -ResY=2208 (Q3)]
//   single player: -OLShots -OLNoFrontEnd -OLLevel=hideout -OLWheelScoreProbe
// In the match, for 1, 8 and 16 players (a Death Match 5 too; stand-ins with scores in the table, -OLFakePlayers's): the wheel
// opened with the head level and checked: the card up with every player's row in one column up to 4 players and two from 5
// (the WHEELSCORE follow-up, user 3 Nov: a Death Match's as a team game's), no "Waiting for players" HUD line over it (nor over
// the pause menu below; back once the menu is closed, in the warm-up), under the wheel (its top below the wheel's lowest part, its middle
// straight under the wheel's), its face toward the eyes within 2 degrees, every word's cap height at or above the menu floor
// (1.28 degrees, MenuSmallFont's; the cap measured from the cells' font's ink), no wheel pick circle seen on it, no point of the
// hand rest volume (10 to 40 cm out, 10 to 35 to each side, 50 to 80 down from the eyes) seen on it, every word inside the
// card's frame; a line on the hands pointing at the lowest slots (40 cm out, 20 to the side, 30 down); the head turned to the
// card's middle finds it (the wheel's follow holds). A shot level and one reading (ol_wheelscore_<mode>_<n>_<q2|q3>[_read]). Then
// the pause menu: no table beside it, its panel in the middle (ol_wheelscore_<mode>_pause_<q>). Single player: the wheel has no
// card and the pause menu no table. One "OLSHOT wheelscore check PASS|FAIL: ..." line a check, "OLSHOT wheelscore RESULT".
// -OLOldWheelScore: the old place (the card's checks and the pause check FAIL: the control).

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "OutlawsCoop.h"
#include "OutlawsMpHost.h"
#include "OutlawsFragTable.h"
#include "Camera/CameraComponent.h"
#include "CanvasItem.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "HAL/PlatformMisc.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsWheelScoreProbeLocal
{
	int32 Passes = 0, Fails = 0, Case = 0;
	float At = 0.f, CapPx = 0.f;
	FVector ReadDirWas = FVector::ZeroVector;
	FRotator ReadRot = FRotator::ZeroRotator;
	TArray<int32> Players;                   // (a Death Match's 1, 5, 8 and 16 players, a team game's 1, 8 and 16)
	constexpr double FloorDeg = 1.28;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wheelscore check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}

	void Shoot(const FString& Name)
	{
		const FString Path = FPaths::ProjectSavedDir() / TEXT("OLShots") / (Name + TEXT(".png"));
		FScreenshotRequest::RequestScreenshot(Path, false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wheelscore shot: %s"), *Path);
	}

	/** The cells' font's cap height, px: an H's ink drawn on a canvas as the card's is (white on black, read back). */
	float InkCapPx(UObject* Ctx, const UFont* Font)
	{
		if (!Font) return 0.f;
		UTextureRenderTarget2D* RT = UKismetRenderingLibrary::CreateRenderTarget2D(Ctx, 512, 128, RTF_RGBA8, FLinearColor::Black);
		if (!RT) return 0.f;
		UCanvas* Cv = nullptr;
		FVector2D Size;
		FDrawToRenderTargetContext Draw;
		UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(Ctx, RT, Cv, Size, Draw);
		if (Cv)
		{
			FCanvasTextItem T(FVector2D(16.f, 64.f), FText::FromString(TEXT("HHHH")), const_cast<UFont*>(Font), FLinearColor::White);
			T.bCentreY = true;
			T.BlendMode = SE_BLEND_Translucent;
			Cv->DrawItem(T);
		}
		UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(Ctx, Draw);
		TArray<FColor> Px;
		if (!UKismetRenderingLibrary::ReadRenderTarget(Ctx, RT, Px, false) || Px.Num() != 512 * 128) return 0.f;
		int32 Top = -1, Bottom = -1;
		for (int32 y = 0; y < 128; ++y)
		{
			bool bInk = false;
			for (int32 x = 0; x < 512 && !bInk; ++x) bInk = Px[y * 512 + x].R >= 128;
			if (bInk) { if (Top < 0) Top = y; Bottom = y; }
		}
		return Top >= 0 ? float(Bottom - Top + 1) : 0.f;
	}
}

OL_SHOT_MATCH(OLWheelScoreProbe) { return OLFlags::Param(TEXT("OLWheelScoreProbe")); }

OL_SHOT_TEST(OLWheelScoreProbe, 1880)
{
	// (the names one by one, not the namespace: in a unity build another test's anonymous namespace has a Check of its own, and
	// a using-directive left the two side by side)
	namespace Ws = OutlawsWheelScoreProbeLocal;
	using Ws::Passes; using Ws::Fails; using Ws::Case; using Ws::At; using Ws::CapPx; using Ws::ReadDirWas; using Ws::ReadRot;
	using Ws::Players; using Ws::FloorDeg; using Ws::Check; using Ws::Shoot; using Ws::InkCapPx;
	if (!L || ShotStep < 0) return;
	L->StartSaveDelay = -1.f;            // (no autosaves: the probe writes nothing)
	L->AutosaveWait = 0.f;
	UOutlawsCoop* C = UOutlawsCoop::Get(this);
	const AOutlawsCoopSession* S = C ? C->GetSession() : nullptr;
	const bool bMatch = C && C->IsMatch() && S && S->MatchClass != 0;
	const bool bOld = UOutlawsCoop::OldWheelScore();
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView && !bFrontMenu;
	int32 ResY = 0;
	FParse::Value(FCommandLine::Get(), TEXT("ResY="), ResY);
	const FString Q = ResY == 1920 ? TEXT("q2") : ResY == 2208 ? TEXT("q3") : TEXT("flat");
	const FString ModeTag = !bMatch ? TEXT("sp") : S->bTeams ? TEXT("team") : TEXT("dm");
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wheelscore RESULT %s (%d passed, %d failed; %s, %s%s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			*ModeTag, *Q, bOld ? TEXT(", -OLOldWheelScore: the old place, FAIL expected") : TEXT(""));
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	auto HeadTo = [this](float Pitch, float Yaw)
	{
		if (AController* Ctl = GetController()) Ctl->SetControlRotation(FRotator(Pitch, Yaw, 0.0));
	};
	const FVector Eye = Camera->GetComponentLocation();
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 6.f || !bUp || bMenuOpen || WarmFrames <= 3) return;
		// (the host in the match's level: the warm-up will do, as the card is up in any match with a game class; a lone host with
		// no bots never gets past it, the host's Start wants a second player)
		if (C && C->IsMatch() && (!S || (S->MatchState != uint8(FOutlawsMatchFlow::EState::WarmUp) && S->MatchState != uint8(FOutlawsMatchFlow::EState::Playing))))
		{
			if (ShotTime < 150.f) return;
			Check(false, TEXT("the match never reached its warm-up or play"));
			Finish();
			return;
		}
		if (GEngine) GEngine->Exec(GetWorld(), TEXT("r.MotionBlurQuality 0"));      // (sharp pictures, as the other shot tests)
		// (an eye's field of view at the Quest sizes, as the kill feed's shots: the window is one eye's buffer)
		if (Q != TEXT("flat")) Camera->SetFieldOfView(Q == TEXT("q3") ? 104.f : 97.f);
		HeadTo(0.f, GetActorRotation().Yaw);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wheelscore start: %s, %s, %d players in the match (%s), fov %.0f%s"), *ModeTag, *Q, S ? S->Scores.Num() : 0,
			S ? FOutlawsMatchFlow::StateName(FOutlawsMatchFlow::EState(S->MatchState)) : TEXT("no session"), Camera->FieldOfView, bOld ? TEXT(", -OLOldWheelScore") : TEXT(""));
		if (bMatch) C->bScoreStandIns = true;
		Players = bMatch && !S->bTeams ? TArray<int32>{ 1, 5, 8, 16 } : TArray<int32>{ 1, 8, 16 };
		Case = 0;
		At = ShotTime;
		ShotStep = bMatch ? 1 : 10;
		return;
	}
	case 1:
	{
		// a case: that many players in the table, the wheel opened with the head level
		if (ShotTime < At + 0.5f) return;
		C->FakePlayers = FMath::Max(0, Players[Case] - S->Scores.Num());
		CloseWheel();
		HeadTo(0.f, GetActorRotation().Yaw);
		At = ShotTime;
		ShotStep = 2;
		return;
	}
	case 2:
	{
		if (ShotTime < At + 0.4f) return;
		OpenWheel(1);
		At = ShotTime;
		ShotStep = 3;
		return;
	}
	case 3:
	{
		if (ShotTime < At + 1.2f) return;
		const int32 Want = Players[Case];
		const FString Tag = FString::Printf(TEXT("%s %d players, %s"), *ModeTag, Want, *Q);
		AOutlawsFragTablePanel* Card = WheelScore.Get();
		const bool bShown = Card && !Card->IsHidden() && Card->GetCanvas();
		Check(bShown && Card->GetData().Rows.Num() == Want, FString::Printf(TEXT("(%s) the card is up under the open wheel with %d rows (wanted %d)"), *Tag, Card ? Card->GetData().Rows.Num() : -1, Want));
		if (!bShown)
		{
			Shoot(FString::Printf(TEXT("ol_wheelscore_%s_%d_%s"), *ModeTag, Want, *Q));
			CloseWheel();
			At = ShotTime;
			ShotStep = ++Case < Players.Num() ? 1 : 6;
			return;
		}
		const FVector Mid = Card->GetActorLocation(), N = Card->GetActorUpVector(), Ax = Card->GetActorForwardVector(), Ay = Card->GetActorRightVector();
		const FVector2D Sz = Card->GetSizeCm();
		const double Wpx = Card->GetCanvasW(), Hpx = Card->GetCanvasH();
		const FVector Fwd = WheelDir.GetSafeNormal(), Up = WheelRoot->GetUpVector(), Rt = -WheelRoot->GetRightVector();
		auto OnCard = [&](const FVector& P) -> bool
		{
			const FVector D = (P - Eye).GetSafeNormal();
			const double Den = FVector::DotProduct(D, N);
			if (FMath::Abs(Den) < 1e-4) return false;
			const double T = FVector::DotProduct(Mid - Eye, N) / Den;
			if (T <= 0.0) return false;
			const FVector Off = Eye + D * T - Mid;
			return FMath::Abs(FVector::DotProduct(Off, Ax)) <= Sz.X * 0.5 && FMath::Abs(FVector::DotProduct(Off, Ay)) <= Sz.Y * 0.5;
		};
		auto DownDeg = [&](const FVector& P) { const FVector V = P - Eye; return FMath::RadiansToDegrees(FMath::Atan2(-FVector::DotProduct(V, Up), FVector::DotProduct(V, Fwd))); };
		auto CanvasToWorld = [&](double Px, double Py) { return Mid + Ax * ((Px / Wpx - 0.5) * Sz.X) + Ay * ((Py / Hpx - 0.5) * Sz.Y); };
		// sizes
		const double WideDeg = 2.0 * FMath::RadiansToDegrees(FMath::Atan(Sz.X * 0.5 / FVector::Dist(Eye, Mid)));
		const double TallDeg = 2.0 * FMath::RadiansToDegrees(FMath::Atan(Sz.Y * 0.5 / FVector::Dist(Eye, Mid)));
		const double TopDeg = DownDeg(Mid - Ay * (Sz.Y * 0.5)), BottomDeg = DownDeg(Mid + Ay * (Sz.Y * 0.5));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wheelscore (%s) the card: %.0f x %.0f px, %.0f x %.0f cm (%.2f cm a px), %.0f cm out, %.1f x %.1f deg; from %.1f to %.1f deg under the wheel's middle, tilt %.1f deg; the wheel's lowest part %.1f deg"),
			*Tag, Wpx, Hpx, Sz.X, Sz.Y, WheelScoreCmPerPx, FVector::Dist(Eye, Mid), WideDeg, TallDeg, TopDeg, BottomDeg, WheelScoreTilt, WheelScoreLowDeg);
		// under the wheel: its top below the wheel's lowest part, its middle straight under the wheel's
		const double Side = FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp(FVector::DotProduct((Mid - Eye).GetSafeNormal(), Rt), -1.0, 1.0)));
		Check(TopDeg >= WheelScoreLowDeg + 1.0 && FMath::Abs(Side) < 1.0, FString::Printf(TEXT("(%s) under the wheel: the card's top %.1f deg under the wheel's middle, its lowest part %.1f; the card's middle %.2f deg to the side"),
			*Tag, TopDeg, WheelScoreLowDeg, Side));
		// facing the eyes
		const double Facing = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(N, (Eye - Mid).GetSafeNormal()), -1.0, 1.0)));
		Check(Facing <= 2.0, FString::Printf(TEXT("(%s) its face toward the eyes: %.2f deg off (tilted up %.1f deg from the wheel's face)"), *Tag, Facing, WheelScoreTilt));
		// every word at or above the floor: its cap seen from the eyes at both its ends
		if (CapPx <= 0.f && Card->LastTexts.Num() > 0) CapPx = InkCapPx(this, Card->LastTexts[0].Font);
		double MinDeg = 1e9, MidDeg = 0.0;
		FString MinWhat;
		for (const FFragTableText& T : Card->LastTexts)
		{
			const double Cy = (T.Min.Y + T.Max.Y) * 0.5, Half = CapPx * T.Scale * 0.5;
			for (const double Px : { double(T.Min.X), double(T.Max.X) })
			{
				const FVector A = CanvasToWorld(Px, Cy - Half), B = CanvasToWorld(Px, Cy + Half);
				const double Deg = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct((A - Eye).GetSafeNormal(), (B - Eye).GetSafeNormal()), -1.0, 1.0)));
				if (Deg < MinDeg) { MinDeg = Deg; MinWhat = T.Str; }
			}
		}
		{
			const FVector A = CanvasToWorld(Wpx * 0.5, Hpx * 0.5 - CapPx * 0.5), B = CanvasToWorld(Wpx * 0.5, Hpx * 0.5 + CapPx * 0.5);
			MidDeg = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct((A - Eye).GetSafeNormal(), (B - Eye).GetSafeNormal()), -1.0, 1.0)));
		}
		Check(CapPx > 0.f && Card->LastTexts.Num() > 0 && MinDeg >= FloorDeg, FString::Printf(TEXT("(%s) every word's cap height at or above the floor %.2f deg: %d words, the smallest %.2f deg (\"%s\", at a corner), %.2f at the middle (cap %.0f px of ink, the card's sizing takes %.0f)"),
			*Tag, FloorDeg, Card->LastTexts.Num(), MinDeg, *MinWhat, MidDeg, CapPx, FOutlawsFragTable::CardCapPx));
		// every row inside the card's frame
		int32 Found = 0, Outside = 0;
		const FString Dots = FString::Chr(TCHAR(0x2026));
		for (const FFragTableRow& R : Card->GetData().Rows)
		{
			Found += Card->LastTexts.ContainsByPredicate([&R, &Dots](const FFragTableText& T)
				{ return T.Str == R.Name || (T.Str.EndsWith(Dots) && R.Name.StartsWith(T.Str.LeftChop(1).TrimEnd())); }) ? 1 : 0;
		}
		for (const FFragTableText& T : Card->LastTexts) Outside += T.Min.X < 10.f || T.Max.X > Wpx - 10.0 || T.Min.Y < 5.f || T.Max.Y > Hpx - 5.0 ? 1 : 0;
		Check(Found == Want && Outside == 0, FString::Printf(TEXT("(%s) all %d rows drawn inside the card (%d names found, %d words past its frame)"), *Tag, Want, Found, Outside));
		// (the WHEELSCORE follow-up, user 3 Nov) one column up to 4 players, two from 5: a Death Match's as a team game's
		TArray<float> ColXs;
		for (const FFragTableRow& R : Card->GetData().Rows)
		{
			const FFragTableText* T = Card->LastTexts.FindByPredicate([&R, &Dots](const FFragTableText& X)
				{ return X.Str == R.Name || (X.Str.EndsWith(Dots) && R.Name.StartsWith(X.Str.LeftChop(1).TrimEnd())); });
			if (T && !ColXs.ContainsByPredicate([T](float X) { return FMath::Abs(X - T->Min.X) < 100.f; })) ColXs.Add(T->Min.X);
		}
		const int32 WantCols = Want > 4 ? 2 : 1;
		Check(ColXs.Num() == WantCols, FString::Printf(TEXT("(%s) the players in %d column%s (wanted %d: one up to 4 players, two from 5)"), *Tag, ColXs.Num(), ColXs.Num() == 1 ? TEXT("") : TEXT("s"), WantCols));
		// (the WHEELSCORE follow-up) the warm-up's "Waiting for players" HUD line off while the card is up (it crossed the title row)
		const bool bWarm = S->MatchState == uint8(FOutlawsMatchFlow::EState::WarmUp);
		const bool bWaitLine = HudLines.ContainsByPredicate([](const FString& H) { return H.StartsWith(TEXT("Waiting for players")); });
		Check(!bWaitLine, FString::Printf(TEXT("(%s) no \"Waiting for players\" HUD line over the card (%s; the last said \"%s\")"), *Tag, bWarm ? TEXT("the warm-up") : TEXT("in play"), *C->WaitingSaid));
		// the wheel's pick circles: none seen on the card
		int32 SlotsOn = 0, Slots = 0;
		const double PickR = 4.5 * WheelRoot->GetComponentScale().X;
		for (int32 i = 0; i < WheelSlots.Num(); ++i)
		{
			if (!WheelSlots[i] || !WheelSlots[i]->IsVisible()) continue;
			++Slots;
			const FVector Sc = WheelSlots[i]->GetComponentLocation();
			bool bOn = false;
			for (int32 a = 0; a < 24 && !bOn; ++a)
			{
				const double An = 2.0 * PI * a / 24.0;
				bOn = OnCard(Sc + (Rt * FMath::Cos(An) + Up * FMath::Sin(An)) * PickR);
			}
			SlotsOn += bOn ? 1 : 0;
		}
		Check(Slots > 0 && SlotsOn == 0, FString::Printf(TEXT("(%s) no wheel pick circle seen on the card (%d slots shown, %d on it)"), *Tag, Slots, SlotsOn));
		// the hand rest volume, both hands: none of it seen on the card (the play space's frame, the wheel's way)
		FVector Fh = FVector(Fwd.X, Fwd.Y, 0.0).GetSafeNormal();
		const FVector Rh(-Fh.Y, Fh.X, 0.0);
		int32 RestOn = 0, RestPts = 0;
		for (int32 s = -1; s <= 1; s += 2)
			for (int32 i = 0; i <= 6; ++i)
				for (int32 j = 0; j <= 6; ++j)
					for (int32 k = 0; k <= 6; ++k)
					{
						const FVector P = Eye + Fh * (10.0 + 30.0 * i / 6.0) + Rh * (s * (10.0 + 25.0 * j / 6.0)) + FVector(0.0, 0.0, -(50.0 + 30.0 * k / 6.0));
						++RestPts;
						RestOn += OnCard(P) ? 1 : 0;
					}
		Check(RestOn == 0, FString::Printf(TEXT("(%s) the hand rest volume clear of the card (%d of %d points seen on it)"), *Tag, RestOn, RestPts));
		// the hands pointing at the lowest slots (40 cm out, 20 to each side, 30 down, a hand 8 cm round): a line, not a check
		int32 PointOn = 0;
		for (int32 s = -1; s <= 1; s += 2)
		{
			const FVector Hc = Eye + Fh * 40.0 + Rh * (s * 20.0) + FVector(0.0, 0.0, -30.0);
			bool bOn = OnCard(Hc);
			for (const FVector& D : { FVector::ForwardVector, FVector::BackwardVector, FVector::RightVector, FVector::LeftVector, FVector::UpVector, FVector::DownVector }) bOn |= OnCard(Hc + D * 8.0);
			PointOn += bOn ? 1 : 0;
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT wheelscore (%s) the hands pointing at the lowest slots: %d of 2 seen on the card%s"), *Tag, PointOn,
			PointOn ? TEXT(" (its outer columns: on the headset the hands draw over it)") : TEXT(""));
		// the head level is not on it; turned to its middle it is (the wheel's follow holds there)
		Check(!HeadOnWheelScore(), FString::Printf(TEXT("(%s) the head level is not on the card (the wheel follows as before)"), *Tag));
		Shoot(FString::Printf(TEXT("ol_wheelscore_%s_%d_%s"), *ModeTag, Want, *Q));
		ReadDirWas = WheelDir;
		ReadRot = (Mid - Eye).Rotation();
		At = ShotTime;
		ShotStep = 13;
		return;
	}
	case 13:
		// (the head turned to the card's middle a moment after the level shot: a shot is taken at the end of the next frame, so a
		// turn in the same tick put it in the level shot, mid-turn)
		if (ShotTime < At + 0.3f) return;
		HeadTo(float(ReadRot.Pitch), float(ReadRot.Yaw));
		At = ShotTime;
		ShotStep = 4;
		return;
	case 4:
	{
		if (ShotTime < At + 0.8f) return;
		const FString Tag = FString::Printf(TEXT("%s %d players, %s"), *ModeTag, Players[Case], *Q);
		Check(HeadOnWheelScore() && WheelDir.Equals(ReadDirWas, 1e-3), FString::Printf(TEXT("(%s) the head turned down to the card's middle is on it (the wheel's follow waits), the wheel where it hung"), *Tag));
		Shoot(FString::Printf(TEXT("ol_wheelscore_%s_%d_%s_read"), *ModeTag, Players[Case], *Q));
		At = ShotTime;
		ShotStep = 5;
		return;
	}
	case 5:
	{
		if (ShotTime < At + 0.5f) return;
		CloseWheel();
		HeadTo(0.f, GetActorRotation().Yaw);
		Check(!WheelScore.IsValid() && (!C->WheelPanel.IsValid() || C->WheelPanel->IsHidden()), TEXT("the card goes with the wheel"));
		At = ShotTime;
		ShotStep = ++Case < Players.Num() ? 1 : 6;
		return;
	}
	case 6:
		// the pause menu: no table beside it, its panel in the middle
		if (ShotTime < At + 0.6f) return;
		C->FakePlayers = 7;
		OpenMenu();
		At = ShotTime;
		ShotStep = 7;
		return;
	case 7:
	{
		if (ShotTime < At + 1.5f) return;
		const double MenuOff = MenuRoot ? FMath::Abs(FRotator::NormalizeAxis((MenuRoot->GetComponentLocation() - Eye).Rotation().Yaw - Camera->GetComponentRotation().Yaw)) : 999.0;
		Check(bMenuOpen && !C->PausePanel.IsValid() && MenuOff < 3.0, FString::Printf(TEXT("(%s, %s) the pause menu has no table beside it (%s) and its panel stands in the middle (%.1f deg off the head)"), *ModeTag, *Q,
			C->PausePanel.IsValid() ? TEXT("a table is up") : TEXT("none"), MenuOff));
		const bool bWaitLine = HudLines.ContainsByPredicate([](const FString& H) { return H.StartsWith(TEXT("Waiting for players")); });
		Check(!bWaitLine, FString::Printf(TEXT("(%s, %s) no \"Waiting for players\" HUD line over the pause menu (it crossed Resume; the last said \"%s\")"), *ModeTag, *Q, *C->WaitingSaid));
		Shoot(FString::Printf(TEXT("ol_wheelscore_%s_pause_%s"), *ModeTag, *Q));
		At = ShotTime;
		ShotStep = 8;
		return;
	}
	case 8:
		if (ShotTime < At + 0.6f) return;
		CloseMenu();
		C->bScoreStandIns = false;
		C->FakePlayers = 0;
		At = ShotTime;
		ShotStep = 9;
		return;
	case 9:
	{
		// the warm-up's line back once the menu is closed (the next one said: 5 s apart)
		const bool bWarm = S && S->MatchState == uint8(FOutlawsMatchFlow::EState::WarmUp);
		const bool bWaitLine = HudLines.ContainsByPredicate([](const FString& H) { return H.StartsWith(TEXT("Waiting for players")); });
		if (bWarm && !bWaitLine && ShotTime < At + 6.5f) return;
		if (bWarm) Check(bWaitLine, FString::Printf(TEXT("(%s, %s) the warm-up's \"Waiting for players\" line back %.1f s after the menu closed"), *ModeTag, *Q, ShotTime - At));
		Finish();
		return;
	}
	// ---- single player: none of it ----
	case 10:
		if (ShotTime < At + 0.5f) return;
		OpenWheel(1);
		At = ShotTime;
		ShotStep = 11;
		return;
	case 11:
	{
		if (ShotTime < At + 1.0f) return;
		Check(bWheelOpen && !WheelScore.IsValid() && (!C || !C->WheelPanel.IsValid()), FString::Printf(TEXT("(single player) the wheel is open with no card under it (wheel %d)"), bWheelOpen ? 1 : 0));
		Shoot(FString::Printf(TEXT("ol_wheelscore_sp_%s"), *Q));
		CloseWheel();
		OpenMenu();
		At = ShotTime;
		ShotStep = 12;
		return;
	}
	case 12:
		if (ShotTime < At + 1.2f) return;
		Check(bMenuOpen && (!C || !C->PausePanel.IsValid()), TEXT("(single player) the pause menu has no table"));
		CloseMenu();
		Finish();
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
