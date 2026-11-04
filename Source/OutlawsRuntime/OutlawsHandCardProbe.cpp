// REQ-HANDCARDFADE's and REQ-HANDCARDHOLD's probes, the score card on the hand through the hideout's end cutscene, the headset's
// rules: -OLShots -OLNoFrontEnd -OLLevel=hideout.
// -OLHandCardFadeProbe [-OLOldHandCardFade]: nothing at 0.5 s, half up at 1.5 s, all up and solid at 2.5 s (a shot at each,
// handcard_05.png, handcard_15.png, handcard_25.png, and handcard_125, _19 and _21 for the fade's shape and the swap to the solid
// material at 2 s), late counts in the middle of the fade do not start it again.
// -OLHandCardFadeProbe=short: a movie that ends at 1.2 s, in the fade: it goes cleanly.
// -OLOldHandCardFade: all up at 0.5 s, as before.
// -OLHandCardHoldProbe [-OLOldHandCardHold]: the other hand's button does nothing, the card hand's tap hides and shows it, a long
// press does neither and shows the skip bar only past the half second; a grip out of reach takes nothing, one at the card takes
// it, the button with it; one controller: its own button, no pass; a held trigger skips after about 3 s, nothing shown for the
// first half second; the next cutscene: hidden and in the same hand.  -OLOldHandCardHold: no tap, no pass, the bar at once.
// Lines "OLSHOT handcard check PASS|FAIL: ..." and "OLSHOT handcard RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsMovieScreen.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "Camera/CameraComponent.h"
#include "HAL/PlatformMisc.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsHandCardProbeLocal
{
	int32 Stage = 0, Passes = 0, Fails = 0, Sub = 0, CardHand = -1, Free = -1;
	float StageT = 0.f, LastFade = 0.f, HeldFor = 0.f;
	bool bLate = false;
	uint32 ExtraShots = 0;
	const float ExtraAt[3] = { 1.25f, 1.9f, 2.1f };
	const TCHAR* const ExtraName[3] = { TEXT("handcard_125.png"), TEXT("handcard_19.png"), TEXT("handcard_21.png") };

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT handcard check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}

	void Shot(const TCHAR* Name)
	{
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name, false, false);
	}

	void Finished(AOutlawsPlayer* P, const TCHAR* What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT handcard RESULT %s (%d passed, %d failed, %s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails, What);
		Stage = -1;
		FGenericPlatformMisc::RequestExit(false);
	}
}

OL_SHOT_MATCH(OLHandCardFadeProbe) { return OLFlags::Has(TEXT("OLHandCardFadeProbe")); }

OL_SHOT_TEST(OLHandCardFadeProbe, 2460)
{
	using namespace OutlawsHandCardProbeLocal;
	if (!L || ShotTime < 0.f || Stage < 0) return;
	const bool bOld = OLFlags::Param(TEXT("OLOldHandCardFade")), bShort = OLFlags::ValueIs(TEXT("OLHandCardFadeProbe"), TEXT("short"));
	const TCHAR* What = bShort ? TEXT("short") : bOld ? TEXT("-OLOldHandCardFade") : TEXT("new");
	bVR = true;
	AOutlawsMovieScreen* M = L->GetMovieScreen();
	const bool bPlaying = L->IsMoviePlaying();
	const float Fade = M ? M->HandCardFade() : -1.f, Age = M ? M->GetAge() : -1.f;
	const bool bShown = M && M->HandCardShown();
	auto Say = [&]() { return FString::Printf(TEXT("%.2f s in: fade %.2f, shown %d, solid %d"), Age, Fade, bShown ? 1 : 0, M && M->HandCardSolid() ? 1 : 0); };
	StageT += Dt;
	if (M && Stage >= 3 && Stage <= 5 && ExtraShots < UE_ARRAY_COUNT(ExtraAt) && Age >= ExtraAt[ExtraShots]) Shot(ExtraName[ExtraShots++]);
	switch (Stage)
	{
	case 0:
		if (L->IsRebuildPending() || bMenuOpen || bPlaying || StageT < 3.f) return;
		L->StartSaveDelay = -1.f;
		L->AutosaveWait = 0.f;
		L->CompleteLevel(TEXT("the hand card probe"));
		Stage = 1;
		return;
	case 1:
		if (!bPlaying || !M) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT handcard the movie is up: %s"), *Say());
		// (the card's hand before the eye, so the shots have it, over the picture)
		if (Camera) ScoreCardFrame()->SetWorldLocation(Camera->GetComponentLocation() + Camera->GetForwardVector() * 48.0 - FVector::UpVector * 24.0);
		Stage = 2;
		return;
	case 2:
		if (Age < 0.5f) return;
		if (bOld) Check(bShown && Fade >= 1.f, FString::Printf(TEXT("(old) all up at once: %s"), *Say()));
		else Check(M->HasHandCard() && !bShown && Fade <= 0.f, FString::Printf(TEXT("the first second: nothing on the hand: %s"), *Say()));
		Shot(TEXT("handcard_05.png"));
		if (bShort)
		{
			Stage = 10;
			return;
		}
		Stage = 3;
		return;
	case 3:
		if (Age < 1.5f) return;
		if (!bOld) Check(bShown && Fade > 0.35f && Fade < 0.65f && !M->HandCardSolid() && M->HandCardFadeIsReal(), FString::Printf(TEXT("half way through its second's fade: %s, the material takes it %d"), *Say(), M->HandCardFadeIsReal() ? 1 : 0));
		Shot(TEXT("handcard_15.png"));
		LastFade = Fade;
		M->TestLateCounts();
		Stage = 4;
		return;
	case 4:
		// (the frames just after the late counts: on from where it was)
		if (Age < 1.7f) return;
		if (!bOld) Check(Fade >= LastFade, FString::Printf(TEXT("late counts leave the fade going: %.2f then %.2f, %s"), LastFade, Fade, *M->DescribeHandCard()));
		Stage = 5;
		return;
	case 5:
		if (Age < 2.5f) return;
		Check(bShown && Fade >= 1.f && M->HandCardSolid(), FString::Printf(TEXT("all up and solid: %s"), *Say()));
		Shot(TEXT("handcard_25.png"));
		Stage = 6;
		StageT = 0.f;
		return;
	case 6:
		if (StageT < 0.5f) return;      // (the shot's frame)
		Finished(this, What);
		return;
	case 10:
		if (Age < 1.2f) return;
		Check(bShown && Fade > 0.1f && Fade < 0.3f, FString::Printf(TEXT("(short) the movie ends in the fade: %s"), *Say()));
		L->SkipMovie();
		Stage = 11;
		StageT = 0.f;
		return;
	case 11:
		if (StageT < 1.f) return;
		Check(!L->IsMoviePlaying(), TEXT("(short) it ended with the card half up, no hang"));
		Finished(this, What);
		return;
	default:
		return;
	}
}

OL_SHOT_MATCH(OLHandCardHoldProbe) { return OLFlags::Has(TEXT("OLHandCardHoldProbe")); }

OL_SHOT_TEST(OLHandCardHoldProbe, 2472)
{
	using namespace OutlawsHandCardProbeLocal;
	if (!L || ShotTime < 0.f || Stage < 0) return;
	const bool bOld = OLFlags::Param(TEXT("OLOldHandCardHold"));
	const TCHAR* What = bOld ? TEXT("-OLOldHandCardHold") : TEXT("new");
	bVR = true;
	AOutlawsMovieScreen* M = L->GetMovieScreen();
	const bool bPlaying = L->IsMoviePlaying();
	const bool bShown = M && M->HandCardShown();
	const float Fade = M ? M->HandCardFade() : -1.f, Age = M ? M->GetAge() : -1.f;
	auto Bit = [](int32 H) { return H == 0 ? 4 : 1; };
	auto Say = [&]() { return FString::Printf(TEXT("%.2f s in: card hand %s, fade %.2f, shown %d, hold %.2f, bar %d"), Age, ScoreCardHand() == 0 ? TEXT("left") : TEXT("right"), Fade, bShown ? 1 : 0, MovieHold, M && M->SkipBarShown() ? 1 : 0); };
	auto Next = [&]() { ++Stage; StageT = 0.f; };
	const TCHAR* const GripOfHand[2] = { TEXT("OLVR_LeftGrip"), TEXT("OLVR_RightGrip") };
	StageT += Dt;
	switch (Stage)
	{
	case 0:
		if (L->IsRebuildPending() || bMenuOpen || bPlaying || StageT < 3.f) return;
		L->StartSaveDelay = -1.f;
		L->AutosaveWait = 0.f;
		L->CompleteLevel(TEXT("the hand card probe"));
		Next();
		return;
	case 1:
		if (!bPlaying || !M || Age < 2.2f) return;
		CardHand = ScoreCardHand();
		Free = 1 - CardHand;
		Check(bShown && Fade >= 1.f, FString::Printf(TEXT("the card up on its hand: %s"), *Say()));
		if (bOld)
		{
			// (old: the card's button is no tap, a held button shows the bar at once)
			FaceHeld |= Bit(CardHand);
			Stage = 40;
			StageT = 0.f;
			return;
		}
		FaceHeld |= Bit(Free);
		Next();
		return;
	case 2:
		// (1) the other hand's button
		if (StageT < 0.1f) return;
		FaceHeld &= ~Bit(Free);
		Next();
		return;
	case 3:
		if (StageT < 0.3f) return;
		Check(bShown && Fade >= 1.f, FString::Printf(TEXT("(1) a tap of the other hand's button does nothing: %s"), *Say()));
		FaceHeld |= Bit(CardHand);
		Next();
		return;
	case 4:
		// (2) the card hand's tap hides, again shows
		if (StageT < 0.1f) return;
		FaceHeld &= ~Bit(CardHand);
		Next();
		return;
	case 5:
		if (StageT < 0.1f) return;
		Check(Fade < 1.f && Fade > 0.f, FString::Printf(TEXT("(2) the tap: a short fade down: %s"), *Say()));
		Next();
		return;
	case 6:
		if (StageT < 0.3f) return;
		Check(!bShown && Fade <= 0.f, FString::Printf(TEXT("(2) hidden: %s"), *Say()));
		FaceHeld |= Bit(CardHand);
		Next();
		return;
	case 7:
		if (StageT < 0.1f) return;
		FaceHeld &= ~Bit(CardHand);
		Next();
		return;
	case 8:
		if (StageT < 0.4f) return;
		Check(bShown && Fade >= 1.f, FString::Printf(TEXT("(2) a second tap shows it: %s"), *Say()));
		FaceHeld |= Bit(CardHand);
		Next();
		return;
	case 9:
		// (3) a long press: no tap; nothing toward the skip until the half second
		if (StageT < 0.3f) return;
		Check(MovieHold == 0.f && !M->SkipBarShown(), FString::Printf(TEXT("(3) a press of 0.3 s: no bar: %s"), *Say()));
		Next();
		return;
	case 10:
		if (StageT < 0.5f) return;
		Check(MovieHold > 0.f && M->SkipBarShown(), FString::Printf(TEXT("(3) past the half second the bar shows: %s"), *Say()));
		FaceHeld &= ~Bit(CardHand);
		Next();
		return;
	case 11:
		if (StageT < 0.3f) return;
		Check(bShown && Fade >= 1.f && MovieHold == 0.f, FString::Printf(TEXT("(3) let go: the card stays, the bar goes: %s"), *Say()));
		// (4) a grip far from the card
		FrameOf(Free)->SetWorldLocation(M->HandCardCentre() + FVector(0.0, 0.0, 200.0));
		TestAxes.Add(GripOfHand[Free], 1.f);
		Next();
		return;
	case 12:
		if (StageT < 0.1f) return;
		TestAxes.Remove(GripOfHand[Free]);
		Next();
		return;
	case 13:
		if (StageT < 0.1f) return;
		Check(ScoreCardHand() == CardHand, FString::Printf(TEXT("(4) a grip 2 m from the card takes nothing: %s"), *Say()));
		FrameOf(Free)->SetWorldLocation(M->HandCardCentre() + FVector(0.0, 0.0, -5.0));
		TestAxes.Add(GripOfHand[Free], 1.f);
		Next();
		return;
	case 14:
		// (5) a grip at it: and held, it counts toward no skip
		if (StageT < 0.8f) return;
		Check(ScoreCardHand() == Free && MovieHold == 0.f, FString::Printf(TEXT("(5) a grip at the card takes it, and the grip held 0.8 s is no hold to skip: %s"), *Say()));
		Check(FVector::Dist(M->HandCardCentre(), FrameOf(Free)->GetComponentLocation()) < 60.f, FString::Printf(TEXT("(5) the card went with it: %.0f cm from the hand"), FVector::Dist(M->HandCardCentre(), FrameOf(Free)->GetComponentLocation())));
		TestAxes.Remove(GripOfHand[Free]);
		FaceHeld |= Bit(CardHand);        // (the hand that gave it up)
		Next();
		return;
	case 15:
		if (StageT < 0.1f) return;
		FaceHeld &= ~Bit(CardHand);
		Next();
		return;
	case 16:
		if (StageT < 0.3f) return;
		Check(bShown && Fade >= 1.f, FString::Printf(TEXT("(6) the hand that gave it up: its button does nothing now: %s"), *Say()));
		FaceHeld |= Bit(Free);
		Next();
		return;
	case 17:
		if (StageT < 0.1f) return;
		FaceHeld &= ~Bit(Free);
		Next();
		return;
	case 18:
		if (StageT < 0.4f) return;
		Check(!bShown, FString::Printf(TEXT("(6) hide after the pass: the button went with it: %s"), *Say()));
		FaceHeld |= Bit(Free);            // (shown again for the one controller's turn)
		Next();
		return;
	case 19:
		if (StageT < 0.1f) return;
		FaceHeld &= ~Bit(Free);
		Next();
		return;
	case 20:
	{
		if (StageT < 0.4f) return;
		// (7) one controller, the left: its own button, no pass
		OneController = 1;
		Check(ScoreCardHand() == 0, FString::Printf(TEXT("(7) one controller, the left: the card there: %s"), *Say()));
		TestAxes.Add(GripOfHand[1], 1.f);
		FrameOf(1)->SetWorldLocation(M->HandCardCentre());
		Next();
		return;
	}
	case 21:
		if (StageT < 0.1f) return;
		TestAxes.Remove(GripOfHand[1]);
		Next();
		return;
	case 22:
		if (StageT < 0.2f) return;
		Check(ScoreCardHand() == 0, FString::Printf(TEXT("(7) no pass: %s"), *Say()));
		OneController = 0;
		// (8) a trigger held to the skip, the card hidden first for the next movie
		FaceHeld |= Bit(ScoreCardHand());
		Next();
		return;
	case 23:
		if (StageT < 0.1f) return;
		FaceHeld &= ~Bit(ScoreCardHand());
		Next();
		return;
	case 24:
		if (StageT < 0.4f) return;
		Check(!bShown, FString::Printf(TEXT("(8) hidden for the next cutscene: %s"), *Say()));
		TestAxes.Add(TEXT("OLVR_RightTrigger"), 1.f);
		HeldFor = 0.f;
		Next();
		return;
	case 25:
		HeldFor += Dt;
		if (bPlaying && HeldFor < 6.f) return;
		TestAxes.Remove(TEXT("OLVR_RightTrigger"));
		Check(!bPlaying && HeldFor > 2.7f && HeldFor < 3.4f, FString::Printf(TEXT("(8) a trigger held skips in about 3 s: %.2f s"), HeldFor));
		Next();
		return;
	case 26:
		// the next level, and its end: the cutscene after
		if (bPlaying || L->IsRebuildPending() || bMenuOpen || bMovieHidden || StageT < 3.f) return;
		L->StartSaveDelay = -1.f;
		L->AutosaveWait = 0.f;
		L->CompleteLevel(TEXT("the hand card probe, the second"));
		Next();
		return;
	case 27:
		if (!bPlaying || !M) return;
		if (Age < 2.5f) return;
		Check(M->HasHandCard() && !bShown && Fade <= 0.f && ScoreCardHand() == Free, FString::Printf(TEXT("(9) the next cutscene: no fade-in, hidden, the same hand (%s wanted): %s"), Free == 0 ? TEXT("left") : TEXT("right"), *Say()));
		FaceHeld |= Bit(Free);
		Next();
		return;
	case 28:
		if (StageT < 0.1f) return;
		FaceHeld &= ~Bit(Free);
		Next();
		return;
	case 29:
		if (StageT < 0.4f) return;
		Check(bShown && Fade >= 1.f, FString::Printf(TEXT("(9) a tap brings it back: %s"), *Say()));
		Finished(this, What);
		return;
	case 40:
		// (old) the button held: the bar in a moment
		if (StageT < 0.2f) return;
		Check(MovieHold > 0.f && M->SkipBarShown(), FString::Printf(TEXT("(old) the bar at once: %s"), *Say()));
		FaceHeld &= ~Bit(CardHand);
		Stage = 41;
		StageT = 0.f;
		return;
	case 41:
		if (StageT < 0.4f) return;
		Check(bShown && Fade >= 1.f && ScoreCardHand() == CardHand, FString::Printf(TEXT("(old) no tap: the card stays: %s"), *Say()));
		TestAxes.Add(TEXT("OLVR_RightTrigger"), 1.f);
		HeldFor = 0.f;
		Stage = 42;
		StageT = 0.f;
		return;
	case 42:
		HeldFor += Dt;
		if (bPlaying && HeldFor < 6.f) return;
		TestAxes.Remove(TEXT("OLVR_RightTrigger"));
		Check(!bPlaying && HeldFor > 2.7f && HeldFor < 3.4f, FString::Printf(TEXT("(old) a trigger held skips in about 3 s: %.2f s"), HeldFor));
		Finished(this, What);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
