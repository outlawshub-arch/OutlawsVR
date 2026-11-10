// -OLShots -OLChiefHandsProbe (REQ-CHIEFHANDS, user 8 Nov: in multiplayer the Chief's VR hands are darker, his skin tone read off
// his own sprite as Mary's nails took her polish).  Each multiplayer character's match kit is put on as a spawn puts it on, and
// the skin the hands then wear is checked: the Chief's lit and shade within 2 of the tone read from twofeath.nwx, both darker
// than the hands' own tan; every other character the hands' own tan, as before.  Every character's sampled tone is logged
// ("OLSHOT chiefhands sample <name>: lit #RRGGBB shade #RRGGBB (<n> px)").  The Chief's hands, then Mary's, held up before the
// eyes and shot (ol_chiefhands_two.png, ol_chiefhands_mar.png in Saved\OLShots).  Lines "OLSHOT chiefhands check <what>:
// PASS|FAIL", "OLSHOT chiefhands RESULT <n> PASS, <m> FAIL".  -OLOldChiefHands (as before: every character the hands' own tan)
// must FAIL.

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS
#include "OutlawsBoxHand.h"
#include "OutlawsLevelActor.h"
#include "UnrealClient.h"

OL_SHOT_MATCH(OLChiefHandsProbe) { return OLFlags::Param(TEXT("OLChiefHandsProbe")); }

OL_SHOT_TEST(OLChiefHandsProbe, 2660)
{
	static int32 Pass = 0, Fail = 0;
	static float NextAt = 3.f;
	if (!L || !L->GetLabAssets()) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT chiefhands check %s: %s"), *What, bOk ? TEXT("PASS") : TEXT("FAIL"));
		++(bOk ? Pass : Fail);
	};
	auto Hex = [](const FColor& C) { return FString::Printf(TEXT("#%02X%02X%02X"), C.R, C.G, C.B); };
	auto Near = [](const FColor& A, const FColor& B, int32 Tol)
	{
		return FMath::Abs(int32(A.R) - B.R) <= Tol && FMath::Abs(int32(A.G) - B.G) <= Tol && FMath::Abs(int32(A.B) - B.B) <= Tol;
	};
	auto Lum = [](const FColor& C) { return 0.299f * C.R + 0.587f * C.G + 0.114f * C.B; };
	int32 NumChars = 0;
	const FOutlawsMpCharacter* Chars = FOutlawsMpCharacter::All(NumChars);
	const int32 Two = FOutlawsMpCharacter::IndexOf(TEXT("Two")), Mary = FOutlawsMpCharacter::IndexOf(TEXT("Mar"));
	auto HoldHands = [this]()
	{
		const FVector Eye = Camera->GetComponentLocation();
		const FVector F = Camera->GetForwardVector(), R = Camera->GetRightVector();
		const FVector Fl = FVector(F.X, F.Y, 0.0).GetSafeNormal();
		for (int32 H = 0; H < 2; ++H)
		{
			UOutlawsBoxHand* Box = H == 0 ? LeftHand.Get() : RightHand.Get();
			if (!Box) continue;
			const double Side = H == 0 ? -1.0 : 1.0;
			const FVector Pos = Eye + Fl * 26.0 + R * Side * 8.0 - FVector::UpVector * 16.0;
			const FQuat Rot = FRotationMatrix::MakeFromXZ((Fl - R * Side * 0.25).GetSafeNormal(), FVector::UpVector).ToQuat();
			Box->SetVisibility(true, true);
			FrameOf(H)->SetWorldLocationAndRotation(Pos, Rot);
			Box->SetCurl(0.f, 0.f);
		}
	};
	const int32 Step = ShotStep;
	if (ShotTime < NextAt)
	{
		if (Step > NumChars) HoldHands();
		return;
	}
	if (Step < NumChars)
	{
		// each character's match kit, as a spawn puts it on (ApplyStartKit wears the nails and the skin)
		const FOutlawsMpCharacter& Ch = Chars[Step];
		if (Step == 0) Check(LeftHand && RightHand && LeftHand->HasMesh() && RightHand->HasMesh(), TEXT("the hands are built"));
		L->ApplyStartKit(Ch.Short, 1);
		ResetForLevel();
		FColor SL, SS;
		int32 Px = 0;
		const bool bRead = L->SpriteSkinTone(Ch.Nwx, SL, SS, Px);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT chiefhands sample %s: lit %s shade %s (%d px%s); worn lit %s shade %s"), Ch.Name, *Hex(SL), *Hex(SS), Px,
			bRead ? TEXT("") : TEXT(", none read"), *Hex(SkinLitWorn), *Hex(SkinShadeWorn));
		const FColor NL = UOutlawsBoxHand::NaturalLit(), NS = UOutlawsBoxHand::NaturalShade();
		if (Step == Two)
		{
			Check(bRead && Near(SkinLitWorn, SL, 2) && Near(SkinShadeWorn, SS, 2),
				FString::Printf(TEXT("the Chief's hands wear his sprite's tone (lit %s vs %s, shade %s vs %s)"), *Hex(SkinLitWorn), *Hex(SL), *Hex(SkinShadeWorn), *Hex(SS)));
			Check(Lum(SkinLitWorn) < Lum(NL) - 10.f && Lum(SkinShadeWorn) < Lum(NS) - 10.f,
				FString::Printf(TEXT("the Chief's hands are darker than the hands' own (lit %.0f < %.0f, shade %.0f < %.0f)"), Lum(SkinLitWorn), Lum(NL), Lum(SkinShadeWorn), Lum(NS)));
		}
		else
		{
			Check(SkinLitWorn == NL && SkinShadeWorn == NS, FString::Printf(TEXT("%s's hands keep the hands' own tan (lit %s, shade %s)"), Ch.Name, *Hex(SkinLitWorn), *Hex(SkinShadeWorn)));
		}
		NextAt = ShotTime + 0.5f;
		++ShotStep;
		return;
	}
	// the Chief's hands, then Mary's, up before the eyes, open (a kit put on, 2.5 s to settle, then the shot)
	const int32 Shot = Step - NumChars;              // 0 kit Two, 1 shot, 2 kit Mary, 3 shot, 4 end
	if (Shot == 0 || Shot == 2)
	{
		GEngine->Exec(GetWorld(), TEXT("r.MotionBlurQuality 0"));
		L->ApplyStartKit(Chars[Shot == 0 ? Two : Mary].Short, 1);
		ResetForLevel();
		if (Controller) Controller->SetControlRotation(FRotator(-30.0, GetActorRotation().Yaw, 0.0));
		NextAt = ShotTime + 2.5f;
		++ShotStep;
		return;
	}
	if (Shot == 1 || Shot == 3)
	{
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / (Shot == 1 ? TEXT("ol_chiefhands_two.png") : TEXT("ol_chiefhands_mar.png")), false, false);
		NextAt = ShotTime + 1.f;
		++ShotStep;
		return;
	}
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT chiefhands RESULT %d PASS, %d FAIL"), Pass, Fail);
	++ShotStep;
	FGenericPlatformMisc::RequestExit(false);
	ShotTime = -1.f;
}
#endif // WITH_OUTLAWS_TESTS
