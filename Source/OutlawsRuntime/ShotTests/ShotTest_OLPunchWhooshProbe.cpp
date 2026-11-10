// -OLShots -OLPunchWhooshProbe (REQ-PUNCHWHOOSH, user 3 Nov: a quick forward thrust of the arm, punching the air, plays the
// game's throw whoosh).  The right hand is moved as a player's would be, frame by frame at 90 a second (its grip placed, its
// velocity set, UpdatePunchWhoosh run), and what plays is counted, by the whooshes and by the level's sound calls
// (AOutlawsLevelActor::WavSpy: "throw" asked for, muted or not): a fast fist thrust whooshes once; a slow one does not; a gun,
// dynamite and the lantern do not (with -OLWhooshAllWeapons they do); the knife and the shovel do; two thrusts inside the
// cooldown whoosh once, one after it again; a thrust that lands a hit (the punch's cooldown set on that frame, as
// UpdatePunches does on a landing) does not.  Lines "OLSHOT punchwhoosh check <what>: PASS|FAIL", "OLSHOT punchwhoosh RESULT
// <n> PASS, <m> FAIL".  -OLOldPunchWhoosh (no whoosh) must FAIL.

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLPunchWhooshProbe) { return OLFlags::Param(TEXT("OLPunchWhooshProbe")); }

OL_SHOT_TEST(OLPunchWhooshProbe, 2670)
{
	if (!L || !L->GetLabAssets() || !Camera || ShotTime < 3.f) return;
	int32 Pass = 0, Fail = 0;
	auto Check = [&](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT punchwhoosh check %s: %s"), *What, bOk ? TEXT("PASS") : TEXT("FAIL"));
		++(bOk ? Pass : Fail);
	};
	TArray<FString> Heard;
	AOutlawsLevelActor::WavSpy = &Heard;
	const bool bVRWas = bVR;
	const int32 EquippedWas = Equipped[1];
	const FString ToolWas = HeldTool[1];
	bVR = true;
	constexpr float Frame = 1.f / 90.f;
	const FVector Eye = Camera->GetComponentLocation();
	const FVector Fl = FVector(Camera->GetForwardVector().X, Camera->GetForwardVector().Y, 0.0).GetSafeNormal();
	const FVector Low = -FVector::UpVector * 20.0;
	// the hand moved from From to To cm in front of the eyes at Speed cm/s (a frame each), UpdatePunchWhoosh each frame; HitAt
	// (cm): the frame it passes there lands a hit (PunchCool set as UpdatePunches sets it)
	auto Stroke = [&](float From, float To, float Speed, float HitAt = -1.f)
	{
		const float Dir = To > From ? 1.f : -1.f;
		const int32 N = FMath::Max(2, FMath::CeilToInt(FMath::Abs(To - From) / (Speed * Frame)));
		for (int32 i = 1; i <= N; ++i)
		{
			const float Out = FMath::Lerp(From, To, float(i) / N);
			GripOf(1)->SetWorldLocation(Eye + Fl * Out + Low);
			HandVelocity[1] = Fl * (Dir * Speed);
			if (HitAt >= 0.f && PunchCool[1] <= 0.f && Out >= HitAt) PunchCool[1] = 0.45f;
			UpdatePunchWhoosh(Frame);
		}
	};
	auto Rest = [&](float Seconds)
	{
		HandVelocity[1] = FVector::ZeroVector;
		GripOf(1)->SetWorldLocation(Eye + Fl * 15.0 + Low);
		for (float T = 0.f; T < Seconds; T += Frame)
		{
			PunchCool[1] = FMath::Max(0.f, PunchCool[1] - Frame);
			UpdatePunchWhoosh(Frame);
		}
	};
	auto Hold = [&](int32 Wi, const TCHAR* Tool)
	{
		Equipped[1] = Wi;
		HeldTool[1] = Tool;
	};
	// one thrust out to arm's length and back: the whooshes it made and the "throw"s asked of the level
	auto Thrust = [&](float Speed, int32& OutWhooshes, int32& OutThrows, float HitAt = -1.f)
	{
		Rest(0.7f);
		const int32 W0 = WhooshCount, H0 = Heard.Num();
		Stroke(15.f, 60.f, Speed, HitAt);
		Stroke(60.f, 15.f, 250.f);
		OutWhooshes = WhooshCount - W0;
		OutThrows = 0;
		for (int32 i = H0; i < Heard.Num(); ++i) if (Heard[i] == TEXT("throw")) ++OutThrows;
	};
	const bool bAll = WhooshAllWeapons();
	int32 W = 0, T = 0;
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT punchwhoosh thresholds: peak %.0f cm/s along the facing, out to %.0f cm, back in %.0f cm, %.2f s between%s%s"), WhooshSpeed, WhooshReach,
		WhooshRearm, WhooshCoolSeconds, bAll ? TEXT(", all weapons (-OLWhooshAllWeapons)") : TEXT(""), OldPunchWhoosh() ? TEXT(", old (-OLOldPunchWhoosh)") : TEXT(""));
	Hold(-1, TEXT(""));
	Thrust(400.f, W, T);
	Check(W == 1 && T == 1, FString::Printf(TEXT("a fist thrust at 400 cm/s whooshes once (%d whooshes, throw asked %d times)"), W, T));
	Thrust(120.f, W, T);
	Check(W == 0 && T == 0, FString::Printf(TEXT("a slow fist move at 120 cm/s does not (%d, %d)"), W, T));
	Hold(0, TEXT(""));
	Thrust(400.f, W, T);
	Check(W == (bAll ? 1 : 0), FString::Printf(TEXT("a revolver thrust %s (%d)"), bAll ? TEXT("whooshes (all weapons)") : TEXT("does not"), W));
	Hold(6, TEXT(""));
	Thrust(400.f, W, T);
	Check(W == (bAll ? 1 : 0), FString::Printf(TEXT("a dynamite thrust %s (%d)"), bAll ? TEXT("whooshes (all weapons)") : TEXT("does not"), W));
	Hold(-1, TEXT("ilantern"));
	Thrust(400.f, W, T);
	Check(W == (bAll ? 1 : 0), FString::Printf(TEXT("a lantern thrust %s (%d)"), bAll ? TEXT("whooshes (all weapons)") : TEXT("does not"), W));
	Hold(7, TEXT(""));
	Thrust(400.f, W, T);
	Check(W == 1, FString::Printf(TEXT("a knife thrust whooshes (%d)"), W));
	Hold(-1, TEXT("ishovel"));
	Thrust(400.f, W, T);
	Check(W == 1, FString::Printf(TEXT("a shovel thrust whooshes (%d)"), W));
	// shaking: out, in, out again inside the cooldown; then once it is over
	Hold(-1, TEXT(""));
	Rest(0.7f);
	{
		const int32 W0 = WhooshCount;
		Stroke(15.f, 60.f, 600.f);
		Stroke(60.f, 15.f, 600.f);
		Stroke(15.f, 60.f, 600.f);
		Stroke(60.f, 15.f, 600.f);
		Check(WhooshCount - W0 == 1, FString::Printf(TEXT("two thrusts %.2f s apart whoosh once (%d)"), 2.f * 45.f / 600.f, WhooshCount - W0));
		Rest(WhooshCoolSeconds + 0.05f);
		const int32 W1 = WhooshCount;
		Stroke(15.f, 60.f, 600.f);
		Check(WhooshCount - W1 == 1, FString::Printf(TEXT("after the %.2f s cooldown it whooshes again (%d)"), WhooshCoolSeconds, WhooshCount - W1));
	}
	// a thrust that lands a hit on its way out: the hit's sound only
	Thrust(400.f, W, T, 35.f);
	Check(W == 0 && T == 0, FString::Printf(TEXT("a thrust that lands a hit at 35 cm does not whoosh (%d, %d)"), W, T));
	PunchCool[1] = 0.f;
	Rest(0.7f);
	AOutlawsLevelActor::WavSpy = nullptr;
	HandVelocity[1] = FVector::ZeroVector;
	Equipped[1] = EquippedWas;
	HeldTool[1] = ToolWas;
	bVR = bVRWas;
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT punchwhoosh RESULT %d PASS, %d FAIL"), Pass, Fail);
	FGenericPlatformMisc::RequestExit(false);
	ShotTime = -1.f;
}
#endif // WITH_OUTLAWS_TESTS
