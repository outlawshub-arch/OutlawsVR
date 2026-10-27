// -OLShots -OLLevel=hideout -OLNoFrontEnd -OLHeldClashTest (REQ-HELDCLASH): the two nearest bad guys held one in each hand
// before the eyes, the hands brought together:
//  (a) side by side at the same height and depth, slowly: the contact must come when the opaque pixels meet (within a mask
//      cell, the truth below), and pressed on for 0.6 s they must not overlap by more than a cell;
//  (b) the same with the left one 45 cm higher: as (a), and at the contact their opaque boxes overlap by two cells or more
//      (the see-through gaps let them closer than boxes would);
//  (c) the left one 50 cm further along the view, swept right through the other: no contact (their pictures do overlap);
//      20 cm further: a contact;
//  (d) fast claps (200 cm/s a hand): a smash on the first contact, none while pressed on for 0.8 s (past the cooldown),
//      one on the next contact after parting, none on a contact within the cooldown;
//  (e) the cost: the masks built and the tests, microseconds on the game thread.
// The truth: both masks splatted into a 1 cm grid on the same plane (each opaque pixel's own parallelogram); the overlap is
// the shorter of the longest both-covered run in a row and in a column, the gap the least distance between the two.
// Lines "OLHELDCLASH ..."; verdicts "OLHELDCLASH check (x) PASS|FAIL: ..." and "OLHELDCLASH RESULT".  Screenshots
// Saved/OLShots/ol_heldclash_side_press.png and ol_heldclash_clap_0..4.png (the first clap, contact +0 to +0.4 s).
// -OLHeldClashTest=perf: no truth; 5 s held apart, then 5 s pressed together: the frame and game-thread times and the cost of
// each part ("OLHELDCLASH perf ...").  With -OLOldSmash both show the old behaviour (no collision).
#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsHeldClash.h"
#include "OutlawsLevelActor.h"
#include "OutlawsEnemy.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsTextures.h"
#include "OutlawsFeel.h"
#include "Camera/CameraComponent.h"
#include "MotionControllerComponent.h"
#include "Misc/Paths.h"
#include "RenderCore.h"
#include "UnrealClient.h"
#include "OutlawsShotTests.h"

#if WITH_OUTLAWS_TESTS

namespace
{
	using namespace OutlawsHeldClash;

	struct FTruth
	{
		bool bValid = false;
		int32 Both = 0;          // 1 cm cells both cover
		double Overlap = 0.0;    // cm
		double Gap = -1.0;       // cm, when asked and apart (0 touching)
		double Depth = 0.0;      // cm along the view between their middles
	};

	FTruth Truth(const FMask& MA, const FDrawn& DA, const FMask& MB, const FDrawn& DB, const FVector& Eye, bool bGap)
	{
		FTruth T;
		if (MA.IsEmpty() || MB.IsEmpty()) return T;
		FVector M, F, R, U;
		PlaneOf(MA, DA, MB, DB, Eye, M, F, R, U);
		T.Depth = FMath::Abs(FVector::DotProduct(DB.At(MB.Mid.X, MB.Mid.Y) - DA.At(MA.Mid.X, MA.Mid.Y), F));
		auto P2 = [&](const FDrawn& D, double X, double Y)
		{
			const FVector P = D.At(X, Y) - M;
			return FVector2D(FVector::DotProduct(P, R), FVector::DotProduct(P, U));
		};
		FBox2D Bx(ForceInit);
		Bx += P2(DA, MA.X0, MA.Y0); Bx += P2(DA, MA.X1 + 1, MA.Y0); Bx += P2(DA, MA.X0, MA.Y1 + 1); Bx += P2(DA, MA.X1 + 1, MA.Y1 + 1);
		Bx += P2(DB, MB.X0, MB.Y0); Bx += P2(DB, MB.X1 + 1, MB.Y0); Bx += P2(DB, MB.X0, MB.Y1 + 1); Bx += P2(DB, MB.X1 + 1, MB.Y1 + 1);
		constexpr double C = 1.0;
		const FVector2D Lo = Bx.Min - FVector2D(2.0, 2.0);
		const int32 NX = FMath::Clamp(FMath::CeilToInt((Bx.Max.X - Lo.X) / C) + 3, 1, 3000);
		const int32 NY = FMath::Clamp(FMath::CeilToInt((Bx.Max.Y - Lo.Y) / C) + 3, 1, 3000);
		TArray<uint8> G;
		G.SetNumZeroed(NX * NY);
		auto Splat = [&](const FMask& K, const FDrawn& D, uint8 Bit)
		{
			const FVector2D O = P2(D, 0.0, 0.0), X = P2(D, 1.0, 0.0) - O, Y = P2(D, 0.0, 1.0) - O;
			const double Det = X.X * Y.Y - Y.X * X.Y;
			if (FMath::Abs(Det) < 1e-6) return;
			for (int32 py = K.Y0; py <= K.Y1; ++py)
			{
				for (int32 px = K.X0; px <= K.X1; ++px)
				{
					if (!K.At(px, py)) continue;
					const FVector2D Q = O + X * px + Y * py;
					FBox2D Pb(ForceInit);
					Pb += Q; Pb += Q + X; Pb += Q + Y; Pb += Q + X + Y;
					const int32 X0 = FMath::Clamp(FMath::FloorToInt((Pb.Min.X - Lo.X) / C), 0, NX - 1), X1 = FMath::Clamp(FMath::FloorToInt((Pb.Max.X - Lo.X) / C), 0, NX - 1);
					const int32 Y0 = FMath::Clamp(FMath::FloorToInt((Pb.Min.Y - Lo.Y) / C), 0, NY - 1), Y1 = FMath::Clamp(FMath::FloorToInt((Pb.Max.Y - Lo.Y) / C), 0, NY - 1);
					for (int32 iy = Y0; iy <= Y1; ++iy)
					{
						for (int32 ix = X0; ix <= X1; ++ix)
						{
							const FVector2D V = Lo + FVector2D((ix + 0.5) * C, (iy + 0.5) * C) - O;
							const double Uu = (Y.Y * V.X - Y.X * V.Y) / Det, Vv = (X.X * V.Y - X.Y * V.X) / Det;
							if (Uu >= px && Uu < px + 1 && Vv >= py && Vv < py + 1) G[iy * NX + ix] |= Bit;
						}
					}
				}
			}
		};
		Splat(MA, DA, 1);
		Splat(MB, DB, 2);
		int32 RowMax = 0, ColMax = 0;
		for (int32 iy = 0; iy < NY; ++iy)
		{
			int32 Run = 0;
			for (int32 ix = 0; ix < NX; ++ix)
			{
				if (G[iy * NX + ix] == 3) { ++T.Both; RowMax = FMath::Max(RowMax, ++Run); }
				else Run = 0;
			}
		}
		for (int32 ix = 0; ix < NX; ++ix)
		{
			int32 Run = 0;
			for (int32 iy = 0; iy < NY; ++iy)
			{
				if (G[iy * NX + ix] == 3) ColMax = FMath::Max(ColMax, ++Run);
				else Run = 0;
			}
		}
		// and along the two diagonals (a sample apart there is 1.41 cm): a sliver at a slant is as thick as it is, not as
		// long as a row or a column crosses it
		int32 DiagMax = 0, AntiMax = 0;
		TArray<int32> PrevD, CurD, PrevN2, CurN2;
		PrevD.SetNumZeroed(NX); CurD.SetNumZeroed(NX); PrevN2.SetNumZeroed(NX); CurN2.SetNumZeroed(NX);
		for (int32 iy = 0; iy < NY; ++iy)
		{
			for (int32 ix = 0; ix < NX; ++ix)
			{
				const bool b3 = G[iy * NX + ix] == 3;
				CurD[ix] = b3 ? (ix > 0 ? PrevD[ix - 1] : 0) + 1 : 0;
				CurN2[ix] = b3 ? (ix + 1 < NX ? PrevN2[ix + 1] : 0) + 1 : 0;
				DiagMax = FMath::Max(DiagMax, CurD[ix]);
				AntiMax = FMath::Max(AntiMax, CurN2[ix]);
			}
			Swap(PrevD, CurD);
			Swap(PrevN2, CurN2);
		}
		T.Overlap = FMath::Min(double(FMath::Min(RowMax, ColMax)), UE_SQRT_2 * FMath::Min(DiagMax, AntiMax)) * C;
		T.bValid = true;
		if (T.Both > 0) { T.Gap = 0.0; return T; }
		if (!bGap) return T;
		// apart: the least distance between their edges
		TArray<FIntPoint> Ea, Eb;
		auto Edge = [&](int32 ix, int32 iy, uint8 Bit)
		{
			if (!(G[iy * NX + ix] & Bit)) return false;
			return ix == 0 || iy == 0 || ix == NX - 1 || iy == NY - 1 || !(G[iy * NX + ix - 1] & Bit) || !(G[iy * NX + ix + 1] & Bit)
				|| !(G[(iy - 1) * NX + ix] & Bit) || !(G[(iy + 1) * NX + ix] & Bit);
		};
		for (int32 iy = 0; iy < NY; ++iy)
		{
			for (int32 ix = 0; ix < NX; ++ix)
			{
				if (Edge(ix, iy, 1)) Ea.Add(FIntPoint(ix, iy));
				if (Edge(ix, iy, 2)) Eb.Add(FIntPoint(ix, iy));
			}
		}
		int64 Best = TNumericLimits<int64>::Max();
		for (const FIntPoint& Pa : Ea)
		{
			for (const FIntPoint& Pb : Eb)
			{
				const int64 Dx = Pa.X - Pb.X, Dy = Pa.Y - Pb.Y;
				Best = FMath::Min(Best, Dx * Dx + Dy * Dy);
			}
		}
		T.Gap = Best == TNumericLimits<int64>::Max() ? -1.0 : FMath::Max(0.0, FMath::Sqrt(double(Best)) * C - C);
		return T;
	}

	struct FScen
	{
		const TCHAR* Tag;
		const TCHAR* What;
		double Lift;      // the left hand this much higher
		double Behind;    // and this much further along the eye's flat forward
		double From, To;  // the hands' gap across, cm
		double Speed;     // each hand, cm/s
		double Hold;      // pressed on at To, s
	};
	const FScen GScens[] = {
		{ TEXT("a"), TEXT("side by side, the same height and depth"), 0.0, 0.0, 170.0, 0.0, 20.0, 0.6 },
		{ TEXT("b"), TEXT("the left one 45 cm higher"), 45.0, 0.0, 170.0, 0.0, 20.0, 0.4 },
		{ TEXT("c1"), TEXT("the left one 50 cm further along the view, swept right through"), 0.0, 50.0, 170.0, -170.0, 40.0, 0.0 },
		{ TEXT("c2"), TEXT("the left one 20 cm further along the view"), 0.0, 20.0, 170.0, 0.0, 20.0, 0.3 },
	};
	constexpr int32 GNumScens = UE_ARRAY_COUNT(GScens);
}

void AOutlawsPlayer::HeldClashTestTick(float Dt)
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L || ShotTime < 0.f) return;
	const bool bOld = OldSmash();
	const bool bPerf = OLFlags::ValueIs(TEXT("OLHeldClashTest"), TEXT("perf"));
	static TWeakObjectPtr<AOutlawsEnemy> Man[2];
	static FVector Eye0, Fl, Rt;
	static int32 Stage = 0, Fails = 0, Sub = 0, Frame = 0;
	static double T0 = 0.0, Sep = 170.0, SubAt = 0.0;
	static FMask Mk[2];
	static const OutlawsTextures::FSpriteAtlas* MkAtlas[2] = { nullptr, nullptr };
	static int32 MkCell[2] = { -2, -2 };
	// a scenario's tallies
	static bool bSeen = false;
	static int32 Missed = 0, ContactFrames = 0;
	static double SeenAt = 0.0, PrevGap = -1.0, PressMax = 0.0, CrossMax = 0.0, StepCm = 0.0, ContactGap = -1.0, ContactOver = 0.0, ContactBox = 0.0, ContactSep = 0.0, Cell = 0.0;
	// the claps
	static int32 SmashAt[4] = { 0, 0, 0, 0 }, ClapContacts = 0, ShotN = 0;
	static double LastSmashT = -100.0, ContactT = 0.0, ShotAt[5] = { 0.0, 0.05, 0.1, 0.2, 0.4 };
	// perf
	static double PerfFrames = 0.0, PerfMs = 0.0, PerfGameMs = 0.0, PerfMax = 0.0;
	static FCost PerfCost0;
	const double T = ShotTime;
	auto Check = [](const TCHAR* Which, bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLHELDCLASH check (%s) %s: %s"), Which, bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (!bOk) ++Fails;
	};
	auto Finish = [&](const TCHAR* Why)
	{
		const FCost& C = Cost();
		UE_LOG(LogOutlaws, Display, TEXT("OLHELDCLASH cost: masks built %lld, mean %.2f us, max %.2f us; tests %lld, mean %.2f us, max %.2f us; of them sampled (boxes met) %lld, mean %.2f us, max %.2f us"),
			C.Builds, C.Builds ? C.BuildUs / C.Builds : 0.0, C.BuildMaxUs, C.Tests, C.Tests ? C.TestUs / C.Tests : 0.0, C.TestMaxUs, C.FullTests, C.FullTests ? C.FullUs / C.FullTests : 0.0, C.FullMaxUs);
		UE_LOG(LogOutlaws, Display, TEXT("OLHELDCLASH RESULT %s (%s; %d failed checks%s)"), Fails == 0 ? TEXT("PASS") : TEXT("FAIL"), Why, Fails, bOld ? TEXT(", -OLOldSmash") : TEXT(""));
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
	};
	// both men's masks and quads now, the probe's own (its builds kept out of the cost)
	auto Current = [&](FDrawn Out[2]) -> bool
	{
		for (int32 h = 0; h < 2; ++h)
		{
			AOutlawsEnemy* E = Man[h].Get();
			const AOutlawsSpriteActor* S = E ? E->GetSprite() : nullptr;
			const OutlawsTextures::FSpriteAtlas* At = E ? E->GetAtlasRef().Get() : nullptr;
			// (a cell of -1 till the sprite's next update after a new look: the last one, as the feature keeps it)
			const int32 Cel = S && S->CurrentCell >= 0 ? S->CurrentCell : (MkAtlas[h] == At ? MkCell[h] : -1);
			if (!S || !At || Cel < 0) return false;
			if (MkAtlas[h] != At || MkCell[h] != Cel)
			{
				const FCost Keep = Cost();
				MkAtlas[h] = At;
				MkCell[h] = Cel;
				BuildMask(*At, MkCell[h], Mk[h]);
				Cost() = Keep;
			}
			if (!DrawnOf(S, Mk[h], Out[h])) return false;
		}
		return true;
	};
	auto PlaceHands = [&](double Gap, double Lift, double Behind, double Speed)
	{
		const FVector Base = Eye0 + Fl * 180.0 + FVector(0.0, 0.0, 35.0);
		const FVector H0 = Base - Rt * (Gap * 0.5) + Fl * Behind + FVector(0.0, 0.0, Lift);
		const FVector H1 = Base + Rt * (Gap * 0.5);
		GripOf(0)->SetWorldLocationAndRotation(H0, Fl.Rotation());
		GripOf(1)->SetWorldLocationAndRotation(H1, Fl.Rotation());
		HandVelocity[0] = HandWorldVel[0] = Rt * Speed;
		HandVelocity[1] = HandWorldVel[1] = -Rt * Speed;
	};
	if (Stage == 0)
	{
		if (T < 3.0) return;
		Eye0 = Camera->GetComponentLocation();
		Fl = FVector(Camera->GetForwardVector().X, Camera->GetForwardVector().Y, 0.0).GetSafeNormal();
		Rt = FVector(Camera->GetRightVector().X, Camera->GetRightVector().Y, 0.0).GetSafeNormal();
		TArray<AOutlawsEnemy*> Near;
		for (const TWeakObjectPtr<AOutlawsEnemy>& Wk : L->GetEnemies())
		{
			AOutlawsEnemy* E = Wk.Get();
			if (!E || E->IsDead() || E->bBoss || E->IsCritter() || E->IsDormant() || !E->GetSprite()) continue;
			Near.Add(E);
		}
		Near.Sort([this](const AOutlawsEnemy& X, const AOutlawsEnemy& Y) { return FVector::DistSquared(X.GetActorLocation(), GetActorLocation()) < FVector::DistSquared(Y.GetActorLocation(), GetActorLocation()); });
		PlaceHands(170.0, 0.0, 0.0, 0.0);
		HandVelocity[0] = HandVelocity[1] = HandWorldVel[0] = HandWorldVel[1] = FVector::ZeroVector;
		bool bOk = Near.Num() >= 2;
		for (int32 h = 0; h < 2 && bOk; ++h)
		{
			Man[h] = Near[h];
			Near[h]->SetActorLocation(GripOf(h)->GetComponentLocation() + Fl * 40.0, false, nullptr, ETeleportType::TeleportPhysics);
			bOk = TryGrabEnemy(h, Near[h]);
			TestAxes.Add(h == 0 ? TEXT("OLVR_LeftGrip") : TEXT("OLVR_RightGrip"), 1.f);
			bGripArmed[h] = false;
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLHELDCLASH start%s%s: %d men about; left %s, right %s, held %d; thickness %.0f cm, ease %.2f s, bounce %.1f cm"), bOld ? TEXT(" (-OLOldSmash)") : TEXT(""),
			bPerf ? TEXT(" perf") : TEXT(""), Near.Num(), Man[0].IsValid() ? *Man[0]->GetName() : TEXT("none"), Man[1].IsValid() ? *Man[1]->GetName() : TEXT("none"), bOk ? 1 : 0, ThickCm(), EaseSec(), BounceCm());
		if (!bOk) { ++Fails; Finish(TEXT("FAIL two men not held")); return; }
		Cost() = FCost();
		Stage = 1;
		Sub = 0;
		SubAt = T;
		T0 = T;
		return;
	}
	if (!Man[0].IsValid() || !Man[1].IsValid() || GrabbedEnemy[0].Get() != Man[0].Get() || GrabbedEnemy[1].Get() != Man[1].Get())
	{
		++Fails;
		Finish(TEXT("FAIL a man left the hand"));
		return;
	}
	const OutlawsHeldClash::FState* St = HeldClash.Get();
	const bool bFeat = !bOld && St && St->bLastValid && St->LastFrame == GFrameCounter;
	++Frame;

	// ---- (e) perf: apart, then pressed ----------------------------------------------------------------------------------
	if (bPerf)
	{
		auto Report = [&](const TCHAR* Part) -> double
		{
			const FCost& C = Cost();
			const int64 Tn = C.Tests - PerfCost0.Tests, Bn = C.Builds - PerfCost0.Builds;
			const double MeanUs = Tn ? (C.TestUs - PerfCost0.TestUs) / Tn : 0.0;
			UE_LOG(LogOutlaws, Display, TEXT("OLHELDCLASH perf %s%s: %.0f frames, frame %.2f ms mean (%.2f max), game thread %.2f ms mean; tests %lld, mean %.2f us (sampled %lld); masks built %lld, mean %.2f us"), Part,
				bOld ? TEXT(" (-OLOldSmash)") : TEXT(""), PerfFrames, PerfFrames > 0.0 ? PerfMs / PerfFrames : 0.0, PerfMax, PerfFrames > 0.0 ? PerfGameMs / PerfFrames : 0.0, Tn,
				MeanUs, C.FullTests - PerfCost0.FullTests, Bn, Bn ? (C.BuildUs - PerfCost0.BuildUs) / Bn : 0.0);
			PerfFrames = PerfMs = PerfGameMs = PerfMax = 0.0;
			PerfCost0 = C;
			return MeanUs;
		};
		const double Tp = T - SubAt;
		if (Sub == 0) { PlaceHands(170.0, 0.0, 0.0, 0.0); if (Tp > 1.0) { Sub = 1; SubAt = T; PerfFrames = PerfMs = PerfGameMs = PerfMax = 0.0; PerfCost0 = Cost(); } return; }
		PerfFrames += 1.0;
		PerfMs += Dt * 1000.0;
		PerfMax = FMath::Max(PerfMax, double(Dt) * 1000.0);
		PerfGameMs += FPlatformTime::ToMilliseconds(GGameThreadTime);
		if (Sub == 1) { PlaceHands(170.0, 0.0, 0.0, 0.0); if (Tp > 5.0) { Report(TEXT("held apart")); Sub = 2; SubAt = T; } return; }
		if (Sub == 2) { PlaceHands(FMath::Max(10.0, 170.0 - Tp * 160.0), 0.0, 0.0, 0.0); if (Tp > 1.0) { PerfFrames = PerfMs = PerfGameMs = PerfMax = 0.0; PerfCost0 = Cost(); Sub = 3; SubAt = T; } return; }
		if (Sub == 3)
		{
			PlaceHands(10.0, 0.0, 0.0, 0.0);
			if (Tp > 5.0)
			{
				const double MeanUs = Report(TEXT("pressed together"));
				const FCost& C = Cost();
				// (a budget on the PC: a tenth of a millisecond a frame pressed together, a millisecond at the worst)
				if (bOld) Check(TEXT("e"), C.Tests == 0, FString::Printf(TEXT("-OLOldSmash: no clash tests (%lld)"), C.Tests));
				else Check(TEXT("e"), C.Tests > 0 && MeanUs <= 100.0 && C.TestMaxUs <= 1000.0 && C.BuildMaxUs <= 1000.0,
					FString::Printf(TEXT("pressed together a test %.2f us mean, %.2f us at the worst; a mask built in %.2f us at the worst (%lld built)"), MeanUs, C.TestMaxUs, C.BuildMaxUs, C.Builds));
				Finish(TEXT("perf"));
			}
			return;
		}
		return;
	}

	FDrawn Now[2];
	const bool bNow = Current(Now);
	static int32 NoMask = 0;
	if (!bNow)
	{
		// (a man's frame not known yet, just after the grab: the hands kept where they are)
		if (++NoMask > 90) { ++Fails; Finish(TEXT("FAIL no mask for a man")); }
		return;
	}
	NoMask = 0;
	Cell = FMath::Max(Mk[0].W > 0 ? Now[0].Ex.Size() : 0.0, Mk[1].W > 0 ? Now[1].Ex.Size() : 0.0);

	// ---- (a) to (c): slow approaches -------------------------------------------------------------------------------------
	if (Stage <= GNumScens)
	{
		const FScen& Sc = GScens[Stage - 1];
		const double Ts = T - SubAt;
		if (Sub == 0)
		{
			// set up apart, settled
			Sep = Sc.From;
			PlaceHands(Sep, Sc.Lift, Sc.Behind, 0.0);
			if (Ts > 0.8)
			{
				bSeen = false;
				Missed = ContactFrames = 0;
				PrevGap = -1.0;
				PressMax = CrossMax = StepCm = ContactOver = ContactBox = ContactSep = 0.0;
				ContactGap = -1.0;
				UE_LOG(LogOutlaws, Display, TEXT("OLHELDCLASH (%s) %s: hands %.0f to %.0f cm apart at %.0f cm/s each"), Sc.Tag, Sc.What, Sc.From, Sc.To, Sc.Speed);
				Sub = 1;
				SubAt = T;
			}
			return;
		}
		// what is drawn now (after any push) against the truth
		const FTruth Post = Truth(Mk[0], Now[0], Mk[1], Now[1], Eye0, !bSeen);
		const bool bFeatContact = bFeat && St->Last.bContact;
		if (bFeatContact) ++ContactFrames;
		const bool bFirst = bOld ? (!bSeen && Post.Both > 0) : (bFeat && St->bLastFirst);
		if (Sc.Behind >= 35.0) CrossMax = FMath::Max(CrossMax, Post.Overlap);
		if (bFirst && !bSeen)
		{
			bSeen = true;
			SeenAt = T;
			ContactSep = Sep;
			// the pictures tested (before the push)
			const FTruth Pre = bOld ? Post : Truth(Mk[0], St->PreA, Mk[1], St->PreB, St->Eye, true);
			ContactGap = Pre.Gap;
			ContactOver = Pre.Overlap;
			ContactBox = bFeat ? St->Last.BoxGap : 0.0;
			UE_LOG(LogOutlaws, Display, TEXT("OLHELDCLASH (%s) contact at t %.3f, hands %.1f cm apart: as tested the truth's overlap %.1f cm (%d cells), gap %.1f cm; the frame before gap %.1f cm; a frame's step %.2f cm; a mask cell %.2f cm; the opaque boxes %.1f cm apart; parted %.1f cm %s"),
				Sc.Tag, T - T0, Sep, Pre.Overlap, Pre.Both, Pre.Gap, PrevGap, StepCm, Cell, ContactBox, bFeat ? St->Last.Push : 0.0, bFeat && St->Last.bAlongView ? TEXT("along the view") : TEXT("across"));
		}
		else if (!bSeen)
		{
			// (before the contact the drawn ones are the tested ones: an overlap past a cell missed)
			if (Post.Overlap > Cell + 0.5)
			{
				if (Missed == 0) UE_LOG(LogOutlaws, Display, TEXT("OLHELDCLASH (%s) t %.3f: no contact yet but the truth overlaps %.1f cm"), Sc.Tag, T - T0, Post.Overlap);
				++Missed;
			}
			PrevGap = Post.Gap;
		}
		else if (Post.Depth < ThickCm() - 0.5)
		{
			// (pressed on: within a body's thickness along the view they must not sink into each other)
			PressMax = FMath::Max(PressMax, Post.Overlap);
		}
		if (Frame % 6 == 0 || bFirst)
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLHELDCLASH (%s) f %d t %.3f sep %.1f: truth overlap %.1f gap %.1f both %d; feature %s push %.1f gap %.1f depth %.1f boxes %.1f; offsets %.1f %.1f"),
				Sc.Tag, Frame, T - T0, Sep, Post.Overlap, Post.Gap, Post.Both, bFeat ? (St->Last.bContact ? TEXT("contact") : St->Last.bNear ? TEXT("near") : TEXT("apart")) : TEXT("-"),
				bFeat ? St->Last.Push : 0.0, bFeat ? St->Last.Gap : 0.0, bFeat ? St->Last.DepthGap : 0.0, bFeat ? St->Last.BoxGap : 0.0, HeldClashOffset(0, Man[0].Get()).Size(), HeldClashOffset(1, Man[1].Get()).Size());
		}
		if (Stage == 1 && bSeen && ShotN == 0 && T - SeenAt >= 0.4)
		{
			ShotN = -1;
			FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("ol_heldclash_side_press.png"), false, false);
		}
		if (Sub == 1)
		{
			const double Was = Sep;
			const double Dir = Sc.To < Sc.From ? -1.0 : 1.0;
			Sep = Sep + Dir * 2.0 * Sc.Speed * Dt;
			if ((Dir < 0.0 && Sep <= Sc.To) || (Dir > 0.0 && Sep >= Sc.To)) { Sep = Sc.To; Sub = 2; SubAt = T; }
			StepCm = FMath::Abs(Sep - Was);
			PlaceHands(Sep, Sc.Lift, Sc.Behind, Sc.Speed);
			return;
		}
		if (Sub == 2)
		{
			PlaceHands(Sep, Sc.Lift, Sc.Behind, 0.0);
			if (Ts < Sc.Hold) return;
			// the verdicts
			const double Tol = Cell + 0.5;
			const FString Line = FString::Printf(TEXT("contact %d (hands %.1f cm apart), as tested gap %.1f overlap %.1f cm (the frame's step %.2f), missed overlaps %d, pressed overlap at most %.1f cm, a cell %.2f cm, boxes %.1f cm apart at the contact"),
				bSeen ? 1 : 0, ContactSep, ContactGap, ContactOver, StepCm, Missed, PressMax, Cell, ContactBox);
			if (Stage == 1 || Stage == 2)
			{
				const bool bTouch = bSeen && ContactGap >= 0.0 && ContactGap <= Tol && ContactOver <= Tol + StepCm && Missed == 0;
				const bool bPress = bSeen && PressMax <= Tol;
				const bool bBox = Stage != 2 || ContactBox <= -2.0 * Cell;
				Check(Sc.Tag, bTouch && bPress && bBox, Line + (Stage == 2 ? FString::Printf(TEXT("; closer than their boxes by %.1f cm (two cells %.1f)"), -ContactBox, 2.0 * Cell) : FString()));
			}
			else if (Stage == 3)
			{
				Check(Sc.Tag, ContactFrames == 0 && CrossMax > 10.0, FString::Printf(TEXT("50 cm apart along the view: %d contact frames, their pictures overlapping by up to %.1f cm as they crossed"), ContactFrames, CrossMax));
			}
			else
			{
				// a contact, and pressed on they do not sink in (pushed across, or along the view to a body's thickness)
				Check(Sc.Tag, bSeen && PressMax <= Tol, FString::Printf(TEXT("20 cm apart along the view: %s"), *Line));
			}
			Sub = 3;
			SubAt = T;
			return;
		}
		// part, then the next
		Sep = FMath::Min(Sc.From, Sep + 2.0 * 80.0 * Dt);
		PlaceHands(Sep, Sc.Lift, Sc.Behind, -80.0);
		if (Sep >= Sc.From && Ts > 0.5)
		{
			++Stage;
			Sub = 0;
			SubAt = T;
		}
		return;
	}

	// ---- (d) fast claps ----------------------------------------------------------------------------------------------------
	const double Ts = T - SubAt;
	const bool bTouch = bOld ? Truth(Mk[0], Now[0], Mk[1], Now[1], Eye0, false).Both > 0 : (St && St->bTouching);
	const bool bFirst = bOld ? (bTouch && ClapContacts == 0) : (bFeat && St->bLastFirst);
	if (bFeat && St->bLastSmash) LastSmashT = T;
	if (bOld && SmashCool >= 0.49f) LastSmashT = T;
	constexpr double Fast = 200.0;
	auto Clap = [&]() { Sep -= 2.0 * Fast * Dt; PlaceHands(Sep, 0.0, 0.0, Fast); };
	auto Part = [&](double Upto) { Sep = FMath::Min(Upto, Sep + 2.0 * Fast * Dt); PlaceHands(Sep, 0.0, 0.0, -Fast); };
	if (ShotN >= 0 && ShotN < 5 && ContactT > 0.0 && T - ContactT >= ShotAt[ShotN])
	{
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("ol_heldclash_clap_%d.png"), ShotN), false, false);
		++ShotN;
	}
	if (Frame % 4 == 0 || bFirst)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLHELDCLASH (d%d) f %d t %.3f sep %.1f: touching %d first %d smashes %d cool %.2f offsets %.1f %.1f"), Sub, Frame, T - T0, Sep, bTouch ? 1 : 0, bFirst ? 1 : 0, HeldSmashCount,
			SmashCool, HeldClashOffset(0, Man[0].Get()).Size(), HeldClashOffset(1, Man[1].Get()).Size());
	}
	switch (Sub)
	{
	case 0:            // set up apart
		Sep = 170.0;
		PlaceHands(Sep, 0.0, 0.0, 0.0);
		if (ShotN < 0) ShotN = 0;
		if (Ts > 0.8) { Sub = 1; SubAt = T; SmashAt[0] = HeldSmashCount; UE_LOG(LogOutlaws, Display, TEXT("OLHELDCLASH (d) fast claps at %.0f cm/s a hand"), Fast); }
		break;
	case 1:            // the first clap, to the contact
		Clap();
		if (bTouch) { ++ClapContacts; ContactT = T; ContactSep = Sep; SmashAt[0] = HeldSmashCount - SmashAt[0]; SmashAt[1] = HeldSmashCount; Sub = 2; SubAt = T; }
		else if (Sep < -100.0) { Sub = 9; }
		break;
	case 2:            // pressed on, 0.8 s (past the cooldown), the hands 40 cm past the contact
		Sep = FMath::Max(ContactSep - 40.0, Sep - 2.0 * Fast * Dt);
		PlaceHands(Sep, 0.0, 0.0, Fast);
		if (Ts >= 0.8) { SmashAt[1] = HeldSmashCount - SmashAt[1]; Sub = 3; SubAt = T; }
		break;
	case 3:            // parted
		Part(170.0);
		if (Sep >= 170.0 && !bTouch && Ts > 0.5) { Sub = 4; SubAt = T; SmashAt[2] = HeldSmashCount; }
		break;
	case 4:            // the second clap: past the cooldown
		Clap();
		if (bTouch) { ++ClapContacts; SmashAt[2] = HeldSmashCount - SmashAt[2]; Sub = 5; SubAt = T; }
		else if (Sep < -100.0) { Sub = 9; }
		break;
	case 5:            // a quick part
		Part(170.0);
		if (!bTouch && Ts > 0.06) { Sub = 6; SubAt = T; SmashAt[3] = HeldSmashCount; }
		break;
	case 6:            // the third clap, within the cooldown
		Clap();
		if (bTouch)
		{
			++ClapContacts;
			SmashAt[3] = HeldSmashCount - SmashAt[3];
			UE_LOG(LogOutlaws, Display, TEXT("OLHELDCLASH (d) the third contact %.2f s after the last smash"), T - LastSmashT);
			Sub = 7;
			SubAt = T;
		}
		else if (Sep < -100.0) { Sub = 9; }
		break;
	case 7:
		Part(170.0);
		if (Ts > 0.5)
		{
			Check(TEXT("d"), SmashAt[0] == 1 && SmashAt[1] == 0 && SmashAt[2] == 1 && SmashAt[3] == 0,
				FString::Printf(TEXT("smashes: first contact %d (want 1), pressed on 0.8 s %d (want 0), next contact past the cooldown %d (want 1), a contact within the cooldown %d (want 0); %d smashes in all"),
					SmashAt[0], SmashAt[1], SmashAt[2], SmashAt[3], HeldSmashCount));
			Finish(TEXT("done"));
		}
		break;
	default:
		Check(TEXT("d"), false, FString::Printf(TEXT("a clap never touched (step %d)"), Sub));
		Finish(TEXT("done"));
		break;
	}
	if (T - T0 > 90.0) { ++Fails; Finish(TEXT("FAIL timed out")); }
}

OL_SHOT_MATCH(OLHeldClashTest) { return OLFlags::Has(TEXT("OLHeldClashTest")); }
OL_SHOT_TEST(OLHeldClashTest, 31)
{
	HeldClashTestTick(Dt);           // (REQ-HELDCLASH: OutlawsPlayerHeldClashTest.cpp)
}
#endif // WITH_OUTLAWS_TESTS
