// REQ-HELDCLASH (user 2026-10-27): two bad guys held one in each hand collide where they are drawn (they passed through each
// other: the sprites have no collision).  After both hands have placed their men (UpdateEnemyGrab), their frames on show are
// tested against each other as drawn (OutlawsHeldClash.h): only the opaque pixels touch, and only within a body's thickness
// along the view.  Touching, each is pushed off the other along the contact's way, by half the overlap: an offset from his
// hand, kept while the hands press on, eased back to the hand (ol.heldclash.EaseSec) once apart.  The smash (its damage,
// cooldown, sound and haptics as before) comes on the first frame of a contact, not when the two chests are within 55 cm;
// a hard one makes them bounce off each other a little.  All on the holder's own machine: in co-op the places go out as
// before (the client's pose carries where its hands' men are, the host's enemy snapshot its own), and the smash's hits
// through TakeHit as before.  A chicken or a tarantula in either hand: the old chest-to-chest smash, no pushing.
// -OLOldSmash: the old smash, and no collision.
#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsHeldClash.h"
#include "OutlawsLevelActor.h"
#include "OutlawsEnemy.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsTextures.h"
#include "OutlawsFeel.h"
#include "OutlawsPerf.h"
#include "Camera/CameraComponent.h"
#include "MotionControllerComponent.h"
#include "Engine/World.h"

bool AOutlawsPlayer::OldSmash()
{
	static const bool bOld = OLFlags::Param(TEXT("OLOldSmash"));
	return bOld;
}

FVector AOutlawsPlayer::HeldClashOffset(int32 Hand, const AOutlawsEnemy* E) const
{
	if (!HeldClash || !E) return FVector::ZeroVector;
	const OutlawsHeldClash::FHand& Hd = HeldClash->Hand[Hand & 1];
	return Hd.Who.Get() == E ? Hd.Off : FVector::ZeroVector;
}

void AOutlawsPlayer::SmashHeld(AOutlawsEnemy* A, AOutlawsEnemy* B, const FVector& Rel, const TCHAR* How)
{
	AOutlawsLevelActor* LG = Level.Get();
	if (!LG || !A || !B) return;
	// two or three smashes fell them (user 2026-09-29): 38..45% of each one's full health a blow, with no place
	// or point-blank multipliers (the point-blank x4 felled them in one)
	const float Dmg = 0.4f;
	const FVector AB = (B->GetActorLocation() - A->GetActorLocation()).GetSafeNormal();
	A->TakeHit(A->GetMaxHealth() * FMath::FRandRange(0.38f, 0.45f), A->GrabChest(), -AB, B->GetActorLocation(), true, 0.f);
	B->TakeHit(B->GetMaxHealth() * FMath::FRandRange(0.38f, 0.45f), B->GrabChest(), AB, A->GetActorLocation(), true, 0.f);
	LG->PlaySoundAt(TEXT("connect"), (A->GrabChest() + B->GrabChest()) * 0.5);
	PulseImpact(0, float(Rel.Size()));
	PulseImpact(1, float(Rel.Size()));
	SmashCool = 0.5f;
	++HeldSmashCount;
	UE_LOG(LogOutlaws, Log, TEXT("OLRT two held bad guys smashed together at %.0f cm/s: %.2f each (%s)"), Rel.Size(), Dmg, How);
}

void AOutlawsPlayer::UpdateHeldClash(float Dt)
{
	OL_PERF_SCOPE("Player: held clash");
	using namespace OutlawsHeldClash;
	if (!HeldClash) HeldClash = MakeShared<FState>();
	FState& S = *HeldClash;
	S.bLastValid = false;
	AOutlawsEnemy* Men[2] = { GrabbedEnemy[0].Get(), GrabbedEnemy[1].Get() };
	for (int32 h = 0; h < 2; ++h)
	{
		if (Men[h] && !Men[h]->bHeldByPlayer) Men[h] = nullptr;
		if (S.Hand[h].Who.Get() != Men[h])
		{
			// a new man in this hand, or none: he starts at the hand, his mask made afresh
			S.Hand[h] = FHand();
			S.Hand[h].Who = Men[h];
			S.bTouching = false;
		}
	}
	// each one's offset eased back toward his hand: critically damped, all but 5% of the way in EaseSec (a long frame, a
	// hitch, eases as if a 45th of a second: a whole frame's ease at once would sink them deep into each other while pressed)
	const double W = 4.74 / EaseSec();
	const double Et = FMath::Min(double(Dt), 1.0 / 45.0);
	const double Decay = FMath::Exp(-W * Et);
	FVector Move[2] = { FVector::ZeroVector, FVector::ZeroVector };
	for (int32 h = 0; h < 2; ++h)
	{
		FHand& Hd = S.Hand[h];
		if (!Men[h]) continue;
		const FVector X = Hd.Off, V = Hd.Vel, K = V + X * W;
		Hd.Off = (X + K * Et) * Decay;
		Hd.Vel = (V - K * (W * Et)) * Decay;
		Move[h] = Hd.Off - X;
	}
	AOutlawsEnemy* A = Men[0];
	AOutlawsEnemy* B = Men[1];
	const FVector Rel = HandVelocity[0] - HandVelocity[1];
	const bool bPair = A && B;
	const bool bMen = bPair && !A->IsCritter() && !B->IsCritter();
	bool bMasks = bMen, bNoAlpha = false;
	for (int32 h = 0; h < 2 && bMen; ++h)
	{
		FHand& Hd = S.Hand[h];
		const AOutlawsSpriteActor* Spr = Men[h]->GetSprite();
		const OutlawsTextures::FSpriteAtlas* At = Men[h]->GetAtlasRef().Get();
		if (At && At->Alpha.Num() == 0) bNoAlpha = true;
		// (a frame's cell is -1 till the sprite's next update after a new look, REQ-GRABUPSCALE's held look: the last one kept)
		const int32 Cell = Spr && Spr->CurrentCell >= 0 ? Spr->CurrentCell : (Hd.Atlas == At ? Hd.Cell : -1);
		if (!Spr || !At || Cell < 0)
		{
			bMasks = false;
			continue;
		}
		if (Hd.Atlas != At || Hd.Cell != Cell)
		{
			// (built only for a held man, again only when his frame changes: held, his clock is stopped)
			Hd.Atlas = At;
			Hd.Cell = Cell;
			Hd.bMask = BuildMask(*At, Hd.Cell, Hd.Mask);
		}
		bMasks = bMasks && Hd.bMask;
		if (!Hd.bMask) bNoAlpha = true;          // (no opacity for that cell: the chests' smash)
	}
	FDrawn D[2];
	if (bMasks && DrawnOf(A->GetSprite(), S.Hand[0].Mask, D[0]) && DrawnOf(B->GetSprite(), S.Hand[1].Mask, D[1]))
	{
		// (where this frame's easing puts them, not yet moved there)
		D[0].O += Move[0];
		D[1].O += Move[1];
		const FVector Eye = Camera->GetComponentLocation();
		// held one well in front of the other (half a body's thickness, where the hands put them, the pushes left out): he
		// may slide past behind; side by side they only ever part across
		const FVector HA = D[0].At(S.Hand[0].Mask.Mid.X, S.Hand[0].Mask.Mid.Y) - S.Hand[0].Off;
		const FVector HB = D[1].At(S.Hand[1].Mask.Mid.X, S.Hand[1].Mask.Mid.Y) - S.Hand[1].Off;
		const double HeldDepth = FMath::Abs(FVector::DotProduct(HB - HA, ((HA + HB) * 0.5 - Eye).GetSafeNormal()));
		const FResult R = Test(S.Hand[0].Mask, D[0], S.Hand[1].Mask, D[1], Eye, ThickCm(), S.PrevN, HeldDepth >= 0.5 * ThickCm());
		const bool bFirst = R.bContact && !S.bTouching;
		// touching till clearly apart (two samples, or past where a bounce takes them, across or along the view) and a tenth
		// of a second out of contact (the ease back and the push take turns while pressed): one press, one smash
		const double Clear = FMath::Max(2.0 * R.CellCm, 2.5 * BounceCm());
		S.SinceContact = R.bContact ? 0.f : S.SinceContact + Dt;
		S.bTouching = R.bContact || (S.bTouching && (S.SinceContact < 0.1f || (R.Gap <= Clear && R.DepthGap < ThickCm() + Clear)));
		if (R.bContact) S.PrevN = R.bAlongView ? FVector2D::ZeroVector : R.Normal2;
		else if (!S.bTouching) S.PrevN = FVector2D::ZeroVector;
		FVector Push[2] = { FVector::ZeroVector, FVector::ZeroVector };
		if (R.bContact)
		{
			// pushed off each other, half each; neither keeps a speed into the other
			const FVector& N = R.Normal;
			Push[0] = -N * (R.Push * 0.5);
			Push[1] = N * (R.Push * 0.5);
			for (int32 h = 0; h < 2; ++h)
			{
				S.Hand[h].Off += Push[h];
				Move[h] += Push[h];
			}
			const double Va = FVector::DotProduct(S.Hand[0].Vel, N), Vb = FVector::DotProduct(S.Hand[1].Vel, N);
			if (Va > 0.0) S.Hand[0].Vel -= N * Va;
			if (Vb < 0.0) S.Hand[1].Vel -= N * Vb;
			if (bFirst) ++S.Contacts;
		}
		bool bSmash = false;
		if (bFirst && SmashCool <= 0.f && Rel.Size() > OutlawsFeel::HitSpeedHard)
		{
			SmashHeld(A, B, Rel, TEXT("on contact"));
			bSmash = true;
			++S.Smashes;
			// and they bounce off each other a little: from rest at the contact, critically damped, the farthest a speed v
			// takes him is v / (W e)
			const double Peak = BounceCm() * FMath::Clamp(Rel.Size() / double(OutlawsFeel::HitSpeedHard), 1.0, 2.0);
			const double V0 = Peak * W * 2.718281828459045;
			S.Hand[0].Vel -= R.Normal * V0;
			S.Hand[1].Vel += R.Normal * V0;
		}
		if (bFirst)
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLRT held clash: %s and %s touch (%s, parted %.1f cm %s, depth gap %.0f cm, their boxes %.1f cm apart, %d half-pixel samples of %.1f cm both opaque)"),
				*A->GetName(), *B->GetName(), bSmash ? TEXT("smashed") : SmashCool > 0.f ? TEXT("the smash cooling down") : TEXT("too gently to smash"), R.Push,
				R.bAlongView ? TEXT("along the view") : TEXT("across"), R.DepthGap, R.BoxGap, R.Overlap, 0.5 * R.CellCm);
		}
		S.bLastValid = true;
		S.LastFrame = GFrameCounter;
		S.bLastFirst = bFirst;
		S.bLastSmash = bSmash;
		S.Last = R;
		S.Eye = Eye;
		S.PreA = D[0];
		S.PreB = D[1];
		S.PostA = D[0];
		S.PostB = D[1];
		S.PostA.O += Push[0];
		S.PostB.O += Push[1];
	}
	else
	{
		// (two men with no picture this frame, a sprite between looks: as they were, the touch kept)
		const bool bChests = bPair && (!bMen || bNoAlpha);
		if (!bPair || bChests) S.bTouching = false;
		if (bChests && SmashCool <= 0.f && Rel.Size() > OutlawsFeel::HitSpeedHard && FVector::Dist(A->GrabChest(), B->GrabChest()) < 55.0)
		{
			// (a chicken or a tarantula in a hand, or an atlas with no opacity kept: the chests' smash as before, no pushing)
			SmashHeld(A, B, Rel, TEXT("chest to chest"));
		}
	}
	const double MaxOff = MaxOffsetCm();
	for (int32 h = 0; h < 2; ++h)
	{
		FHand& Hd = S.Hand[h];
		if (!Men[h]) continue;
		if (Hd.Off.SizeSquared() > MaxOff * MaxOff)
		{
			const FVector C = Hd.Off.GetClampedToMaxSize(MaxOff);
			Move[h] += C - Hd.Off;
			Hd.Off = C;
		}
		if (!Move[h].IsNearlyZero(1e-3)) Men[h]->SetActorLocation(Men[h]->GetActorLocation() + Move[h], false);
	}
}
