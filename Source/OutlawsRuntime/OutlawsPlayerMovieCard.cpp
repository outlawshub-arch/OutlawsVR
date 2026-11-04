// REQ-HANDCARDHOLD (the user, 4 Nov, via the planner): the score card on the hand during a cutscene, a thing the player holds:
// "instead of the trigger, maybe we should use the X and A button to hide the scorecard", and "pass it between hands too, kind
// of like a weapon in the game".  A tap of the lower face button of the hand holding it (X left, A right) hides it or shows it;
// the free hand, reaching to it and gripping, takes it, and the button goes with it.  Both last for this run of the game, in
// memory only.  Each player's own, local, nothing sent.  The card itself is AOutlawsMovieScreen's.  -OLOldHandCardHold: none of it.

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsLevelActor.h"
#include "OutlawsMovieScreen.h"

namespace OutlawsMovieCardLocal
{
	constexpr float TapMaxSeconds = 0.5f;     // (held longer it is a hold to skip, no tap: the same half second as the skip's own start)
	constexpr float TakeReachCm = 20.f;       // the gripping hand this near the card
}

int32 AOutlawsPlayer::ScoreCardPassed = -1;

void AOutlawsPlayer::ResetMovieCardButtons()
{
	// what is down now was pressed before this: no tap and no take from it
	for (int32 H = 0; H < 2; ++H)
	{
		MovieCardTap[H] = (FaceHeld & (H == 0 ? 4 : 1)) != 0 ? -2.f : -1.f;
		bMovieGripDown[H] = Axis(H == 0 ? TEXT("OLVR_LeftGrip") : TEXT("OLVR_RightGrip")) > 0.5f;
		bMovieGripTook[H] = false;
	}
}

void AOutlawsPlayer::UpdateMovieCard(float Dt, AOutlawsLevelActor* LM)
{
	using namespace OutlawsMovieCardLocal;
	static const bool bOld = OLFlags::Param(TEXT("OLOldHandCardHold"));
	AOutlawsMovieScreen* Screen = LM ? LM->GetMovieScreen() : nullptr;
	if (bOld || !Screen) return;
	const int32 Hand = ScoreCardHand();
	for (int32 H = 0; H < 2; ++H)
	{
		// on the let go, so a hold to skip on this same button neither hides the card nor shows it
		float& Tap = MovieCardTap[H];
		if ((FaceHeld & (H == 0 ? 4 : 1)) != 0)
		{
			if (Tap > -1.5f) Tap = FMath::Max(Tap, 0.f) + Dt;
			continue;
		}
		if (Tap >= 0.f && Tap < TapMaxSeconds && H == Hand && Screen->HasHandCard()) Screen->ToggleHandCard();
		Tap = -1.f;
	}
	const int32 Free = 1 - Hand;
	for (int32 H = 0; H < 2; ++H)
	{
		const bool bGrip = Axis(H == 0 ? TEXT("OLVR_LeftGrip") : TEXT("OLVR_RightGrip")) > 0.5f;
		if (!bGrip) bMovieGripTook[H] = false;
		if (bGrip && !bMovieGripDown[H] && H == Free && OneHand() < 0 && Screen->HandCardWithin(FrameOf(H)->GetComponentLocation(), TakeReachCm))
		{
			ScoreCardPassed = H;
			bMovieGripTook[H] = true;
			Pulse(H, 0.3f);
			Screen->HandCardMoved();
			UE_LOG(LogOutlaws, Log, TEXT("OLRT the score card passes to the %s hand"), H == 0 ? TEXT("left") : TEXT("right"));
		}
		bMovieGripDown[H] = bGrip;
	}
}
