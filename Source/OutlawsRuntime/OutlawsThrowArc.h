#pragma once

// BUG-DYNTHROW (user 2 Nov, the user's snapshot #41): the lob maths of a thrown stick or knife, shared by the match bots'
// throws (UOutlawsCoop::BotThrow, OutlawsCoopBots.cpp, where it was born) and the bad guys' (AOutlawsEnemy::ThrowAtPlayer,
// OutlawsEnemyThrow.cpp).  Moved here as it was: the bots throw as before.

#include "CoreMinimal.h"
#include "Engine/World.h"
#include "CollisionQueryParams.h"

namespace OutlawsThrowArc
{
	/** Gravity times this for a thrown stick or knife (980 cm/s2 times it, as every machine draws the flight). */
	constexpr float Grav = 1.5f;

	/** The launch velocity of a lob from From to To at Speed under G (the lower arc, or bHigh the upper); false: out of its
	 *  reach. */
	inline bool LobVelocity(const FVector& From, const FVector& To, double Speed, double G, FVector& Out, bool bHigh = false)
	{
		const FVector D = To - From;
		const FVector Flat(D.X, D.Y, 0.0);
		const double X = Flat.Size();
		if (X < 1.0) return false;
		const double V2 = Speed * Speed;
		const double Disc = V2 * V2 - G * (G * X * X + 2.0 * D.Z * V2);
		if (Disc < 0.0) return false;
		const double Ang = FMath::Atan((V2 + (bHigh ? 1.0 : -1.0) * FMath::Sqrt(Disc)) / (G * X));
		Out = Flat / X * (Speed * FMath::Cos(Ang)) + FVector(0.0, 0.0, Speed * FMath::Sin(Ang));
		return true;
	}

	/** Whether the arc from From at Vel under G for T seconds comes to within Near of To without meeting the level (a wall,
	 *  a beam, a low ceiling) or another body on the way. */
	inline bool ArcClear(UWorld* W, const AActor* Self, const FVector& From, const FVector& Vel, double G, double T, const FVector& To, double Near)
	{
		if (!W) return false;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsBotThrow), false, Self);
		constexpr int32 Steps = 10;
		FVector P = From;
		for (int32 s = 1; s <= Steps; ++s)
		{
			const double t = T * s / Steps;
			const FVector N = From + Vel * t - FVector(0.0, 0.0, 0.5 * G * t * t);
			FHitResult Hit;
			if (W->LineTraceSingleByChannel(Hit, P, N, ECC_Visibility, Q)) return FVector::Dist(FVector(Hit.ImpactPoint), To) < Near;
			P = N;
		}
		return true;
	}
}
