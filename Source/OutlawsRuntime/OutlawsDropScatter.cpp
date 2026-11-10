// REQ-DROPSCATTER (the user, 9 Nov, the Lanes page, "In 1.0"): a dropped thing (a bad guy's death drop, an MP bot's, a player's
// death, the wheel's drop) grows from the body, the pickup's shrink played backward, flies out a random way, lands and bounces
// once.  In co-op and matches the host picks each landing spot and tells everyone; the pickup's reach is as ever, at the
// landing spot (it is held, nobody's to take, while it flies).  -OLOldDropScatter: made where it was before, at once.
//
// The landing spot: 45 to 100 cm out, on floor within a step of the body's (never down a pit or off a ledge, never up onto a
// shelf), nothing between the body and it, clear of the walls round it, in the body's sector or one adjoining it, not in
// water; the arc's way there hits nothing.  None passes (a narrow ledge, a corner): the old spot with a hop in place.
//
// Told to the other machines in the ground item's name (no new message): ",olfly=0/dx/dy/dz", the start's offset from the
// landing spot; a client's own drop asks with ",@" (the host's gate takes names of 32 characters at most, so nothing more) and
// its copy flies to where the host's answer puts it.  An older build passes both parts over (a zero count; no "=").

#include "OutlawsLevelActor.h"
#include "OutlawsRuntime.h"
#include "OutlawsCoop.h"
#include "OutlawsFlags.h"
#include "OutlawsGeometry.h"
#include "OutlawsPerf.h"
#include "OutlawsLevel.h"
#include "OutlawsSpriteActor.h"
#include "Engine/World.h"

namespace OutlawsDropScatterLocal
{
	constexpr double StartUp = 35.0;          // the start over the body's floor (cm)
	constexpr double Gravity = 1500.0;        // cm/s^2
	constexpr float GrowTime = 0.175f;        // the pickup's shrink time (AOutlawsPlayer::UpdatePulls), backward
	constexpr double TouchAt = 0.82;          // the first touch along the way across
	constexpr double StepUp = 32.0;           // the most a landing spot's floor is over or under the body's
	constexpr float WaitMax = 1.5f;           // a client's copy waits this long for the host's spot

	FCollisionResponseParams NoPawns()
	{
		FCollisionResponseParams R;
		R.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
		return R;
	}

	/** Time from a height with an upward speed Vz until it is Dz higher (Dz negative: lower), on the way down. */
	double FallTime(double Vz, double Dz)
	{
		const double Disc = Vz * Vz - 2.0 * Gravity * Dz;
		return (Vz + FMath::Sqrt(FMath::Max(0.0, Disc))) / Gravity;
	}

	/** The legs' times from the points and heights. */
	void Time(AOutlawsLevelActor::FDropFlight& F)
	{
		F.Apex = FMath::Max(F.Apex, F.Touch.Z - F.From.Z + 8.0);
		F.T1 = float(FMath::Max(0.12, FallTime(FMath::Sqrt(2.0 * Gravity * F.Apex), F.Touch.Z - F.From.Z)));
		F.T2 = F.Hop > 0.5 ? float(FMath::Max(0.06, FallTime(FMath::Sqrt(2.0 * Gravity * F.Hop), F.Rest.Z - F.Touch.Z))) : 0.f;
	}

	/** The point in the air at T (in the flight's own frame) and whether it has rested. */
	FVector PointAt(const AOutlawsLevelActor::FDropFlight& F, float T, bool& bOutDone, bool& bOutSecond)
	{
		bOutDone = false;
		bOutSecond = false;
		if (T < F.T1)
		{
			const double A = T / F.T1;
			const double Vz = FMath::Sqrt(2.0 * Gravity * F.Apex);
			FVector P = FMath::Lerp(F.From, F.Touch, A);
			P.Z = F.From.Z + Vz * T - 0.5 * Gravity * T * T;
			return P;
		}
		bOutSecond = true;
		const float T2 = T - F.T1;
		if (F.T2 <= 0.f || T2 >= F.T2)
		{
			bOutDone = true;
			return F.Rest;
		}
		const double A = T2 / F.T2;
		const double Vz = FMath::Sqrt(2.0 * Gravity * F.Hop);
		FVector P = FMath::Lerp(F.Touch, F.Rest, A);
		P.Z = F.Touch.Z + Vz * T2 - 0.5 * Gravity * T2 * T2;
		return P;
	}

	/** Its size, a part S of its own: the actor, and a sprite's 3D stand-in, which keeps a world scale of its own. */
	void SizeTo(AActor* A, const AOutlawsLevelActor::FDropFlight& F, float S)
	{
		A->SetActorScale3D(F.Scale * S);
		if (const AOutlawsSpriteActor* Sp = Cast<AOutlawsSpriteActor>(A); Sp && Sp->Model && !F.ModelScale.IsZero()) Sp->Model->SetWorldScale3D(F.ModelScale * S);
	}

	FTransform FrameOf(const AOutlawsLevelActor::FDropFlight& F)
	{
		const USceneComponent* B = F.Base.Get();
		return B ? B->GetComponentTransform() : FTransform::Identity;
	}
}

bool AOutlawsLevelActor::OldDropScatter()
{
	static const bool bOld = OLFlags::Param(TEXT("OLOldDropScatter"));
	return bOld;
}

namespace OutlawsDropScatterLocal
{
	/** The body's floor under Body (cm); Body itself when there is none. */
	FVector BodyFloor(UWorld* W, const FVector& Body)
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsDropBody), true);
		FHitResult H;
		if (W->LineTraceSingleByChannel(H, Body + FVector(0.0, 0.0, 40.0), Body - FVector(0.0, 0.0, 300.0), ECC_Visibility, Q, NoPawns())) return H.ImpactPoint;
		return Body;
	}
}

AActor* AOutlawsLevelActor::SpawnDropItem(const FString& Name, const FVector& OldAt, const FVector& Body, int32 Slot, int32 Slots)
{
	using namespace OutlawsDropScatterLocal;
	UWorld* W = GetWorld();
	if (OldDropScatter() || !W)
	{
		return SpawnGroundItem(Name, OldAt);
	}
	UOutlawsCoop* C = UOutlawsCoop::Get(this);
	const bool bAsk = C && C->IsClient();
	const FVector Floor = BodyFloor(W, Body);
	FDropFlight F;
	F.Body = Floor;
	F.bAsk = bAsk;
	FVector Rest = OldAt;
	if (!bAsk)
	{
		double Apex = 0.0;
		FVector Touch;
		if (!PickDropLanding(Floor, Slot, Slots, Rest, Touch, Apex))
		{
			// nothing passed: today's spot, a hop in place
			Rest = OldAt;
			F.bInPlace = true;
			++DropScatterFallbacks;
			UE_LOG(LogOutlaws, Log, TEXT("OLRT drop scatter: no landing spot round %s for %s: a hop at %s"), *Floor.ToCompactString(), *Name, *OldAt.ToCompactString());
		}
		F.Touch = Touch;
		F.Apex = Apex;
	}
	bDropMaking = true;
	DropMaking = F;
	AActor* A = SpawnGroundItem(Name, Rest);
	bDropMaking = false;
	if (!A)
	{
		return nullptr;
	}
	// where it lies now (the item's own settle onto the floor, the 3D model's lift off it) is the rest spot
	F.Rest = A->GetActorLocation();
	if (F.bInPlace || bAsk)
	{
		F.Touch = F.Rest;
		F.Apex = 30.0;
	}
	F.From = (F.bInPlace ? FVector(F.Rest.X, F.Rest.Y, FMath::Max(F.Rest.Z, Floor.Z)) : Floor) + FVector(0.0, 0.0, StartUp);
	if (!bAsk && !F.bInPlace) F.Touch.Z = F.Rest.Z;
	F.Hop = F.bInPlace ? 8.0 : FMath::Clamp(0.18 * (F.Apex + F.From.Z - F.Touch.Z), 4.0, 14.0);
	F.bWaiting = bAsk;
	StartDropFlight(A, F);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT drop scatter: %s from %s to %s (%.0f cm out%s)"), *Name, *Floor.ToCompactString(), *F.Rest.ToCompactString(),
		FVector2D::Distance(FVector2D(Floor), FVector2D(F.Rest)), bAsk ? TEXT(", the host asked where") : F.bInPlace ? TEXT(", in place") : TEXT(""));
	return A;
}

bool AOutlawsLevelActor::PickDropLanding(const FVector& Floor, int32 Slot, int32 Slots, FVector& OutRest, FVector& OutTouch, double& OutApex) const
{
	using namespace OutlawsDropScatterLocal;
	UWorld* W = GetWorld();
	if (!W) return false;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsDropScatter), true);
	auto Hits = [&](const FVector& A, const FVector& B) { FHitResult H; return W->LineTraceSingleByChannel(H, A, B, ECC_Visibility, Q, NoPawns()); };
	auto FloorAt = [&](const FVector& P, FHitResult& H)
	{
		return W->LineTraceSingleByChannel(H, P + FVector(0.0, 0.0, StepUp + 18.0), P - FVector(0.0, 0.0, StepUp + 30.0), ECC_Visibility, Q, NoPawns());
	};
	// the body's sector and the ones adjoining it (the parsed level's; none known: not asked)
	const FOutlawsSector* BodySec = LevelData ? LevelData->SectorAt(FOutlawsGeometry::ToOutlaws(Floor, 0.5)) : nullptr;
	TSet<int32> Near;
	if (BodySec)
	{
		Near.Add(BodySec->Index);
		for (const FOutlawsWall& Wl : BodySec->Walls)
		{
			if (Wl.Adjoin >= 0) Near.Add(Wl.Adjoin);
			if (Wl.DAdjoin >= 0) Near.Add(Wl.DAdjoin);
		}
		if (BodySec->VAdjoin >= 0) Near.Add(BodySec->VAdjoin);
	}
	// headroom over the start: the arc stays 15 cm under the ceiling
	const FVector Start = Floor + FVector(0.0, 0.0, StartUp);
	double Room = 65.0;
	{
		FHitResult H;
		if (W->LineTraceSingleByChannel(H, Start, Start + FVector(0.0, 0.0, 100.0), ECC_Visibility, Q, NoPawns())) Room = FMath::Max(8.0, H.Distance - 15.0);
	}
	const double SlotYaw = 360.0 * double(Slot) / double(FMath::Max(1, Slots));
	const double Jitter = Slots > 1 ? 0.3 * 360.0 / double(Slots) : 180.0;
	const double Base = FMath::FRandRange(0.0, 360.0);
	int32 No[6] = {};          // (each check's turn-downs, told when none passed)
	FString BetweenHit;
	for (int32 Try = 0; Try < 16; ++Try)
	{
		const bool bNear = Try >= 10;
		const double Yaw = (Slots > 1 && Try < 6 ? SlotYaw + 30.0 + FMath::FRandRange(-Jitter, Jitter) : Base + FMath::FRandRange(0.0, 360.0));
		const double Dist = bNear ? FMath::FRandRange(30.0, 45.0) : FMath::FRandRange(45.0, 100.0);
		const FVector Dir = FRotator(0.0, Yaw, 0.0).Vector();
		FVector Rest = Floor + Dir * Dist;
		// 1. floor under it, facing up, within a step of the body's
		FHitResult H;
		if (!FloorAt(Rest, H) || H.ImpactNormal.Z < 0.7 || FMath::Abs(H.ImpactPoint.Z - Floor.Z) > StepUp) { ++No[0]; continue; }
		Rest = H.ImpactPoint;
		// 2. nothing between the body and it, low and at the hip
		{
			FHitResult Hb;
			if (W->LineTraceSingleByChannel(Hb, Floor + FVector(0.0, 0.0, 12.0), Rest + FVector(0.0, 0.0, 12.0) + Dir * 10.0, ECC_Visibility, Q, NoPawns())
				|| W->LineTraceSingleByChannel(Hb, Floor + FVector(0.0, 0.0, 40.0), Rest + FVector(0.0, 0.0, 40.0) + Dir * 10.0, ECC_Visibility, Q, NoPawns()))
			{
				if (No[1]++ == 0) BetweenHit = FString::Printf(TEXT("%s at %.0f cm, normal %s"), Hb.GetComponent() ? *Hb.GetComponent()->GetName() : TEXT("?"), Hb.Distance, *Hb.ImpactNormal.ToCompactString());
				continue;
			}
		}
		// 3. clear of the walls round it
		bool bWall = false;
		for (int32 k = 0; k < 4 && !bWall; ++k)
		{
			const FVector Side = FRotator(0.0, Yaw + 45.0 + 90.0 * k, 0.0).Vector();
			bWall = Hits(Rest + FVector(0.0, 0.0, 20.0), Rest + FVector(0.0, 0.0, 20.0) + Side * 14.0);
		}
		if (bWall) { ++No[2]; continue; }
		// 4. the body's sector or one adjoining it, no water
		if (BodySec && LevelData)
		{
			const FOutlawsSector* S = LevelData->SectorAt(FOutlawsGeometry::ToOutlaws(Rest, 0.5));
			if (!S || !Near.Contains(S->Index)) { ++No[3]; continue; }
		}
		float Surf = 0.f;
		if (WaterAt(Rest + FVector(0.0, 0.0, 15.0), Surf)) { ++No[3]; continue; }
		// 5. the touch: floor at the rest's height; the arc there hits nothing
		FVector Touch = FMath::Lerp(Floor, Rest, TouchAt);
		FHitResult Ht;
		if (!FloorAt(Touch, Ht) || FMath::Abs(Ht.ImpactPoint.Z - Rest.Z) > 10.0) { ++No[4]; continue; }
		Touch.Z = Rest.Z;
		const double Apex = FMath::Min(Room, FMath::FRandRange(45.0, 65.0));
		const FVector Mid = FVector(FMath::Lerp(Start.X, Touch.X, 0.45), FMath::Lerp(Start.Y, Touch.Y, 0.45), Start.Z + Apex);
		if (Hits(Start, Mid) || Hits(Mid, Touch + FVector(0.0, 0.0, 5.0))) { ++No[5]; continue; }
		OutRest = Rest;
		OutTouch = Touch;
		OutApex = Apex;
		return true;
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLRT drop scatter: 16 spots turned down round %s: floor %d, between %d, walls %d, sector or water %d, touch %d, arc %d%s"), *Floor.ToCompactString(),
		No[0], No[1], No[2], No[3], No[4], No[5], BetweenHit.IsEmpty() ? TEXT("") : *(TEXT(" (between: ") + BetweenHit + TEXT(")")));
	return false;
}

void AOutlawsLevelActor::StartDropFlight(AActor* A, const FDropFlight& InF)
{
	using namespace OutlawsDropScatterLocal;
	if (!A) return;
	FDropFlight F = InF;
	F.Actor = A;
	F.Scale = A->GetActorScale3D();
	if (const AOutlawsSpriteActor* Sp = Cast<AOutlawsSpriteActor>(A); Sp && Sp->Model && Sp->Model->IsUsingAbsoluteScale()) F.ModelScale = Sp->Model->GetComponentScale();
	F.T = 0.f;
	F.Bounces = 0;
	F.MinScale = 1.f;
	// on a lift or a carrying door (SpawnGroundItem rode it on): the points in its frame, so it lands where the lift has gone
	if (USceneComponent* Root = A->GetRootComponent())
	{
		if (USceneComponent* Parent = Root->GetAttachParent())
		{
			F.Base = Parent;
			const FTransform Fr = Parent->GetComponentTransform();
			F.From = Fr.InverseTransformPosition(F.From);
			F.Touch = Fr.InverseTransformPosition(F.Touch);
			F.Rest = Fr.InverseTransformPosition(F.Rest);
		}
	}
	Time(F);
	for (FPickup& P : Pickups)
	{
		if (P.Actor.Get() == A) { P.bHeld = true; P.bWaitClear = false; }      // (nobody's to take while it flies)
	}
	A->SetActorLocation(FrameOf(F).TransformPosition(F.From));
	SizeTo(A, F, 0.01f);
	DropFlights.RemoveAll([A](const FDropFlight& X) { return X.Actor.Get() == A; });
	DropFlights.Add(F);
	++DropFlightsMade;
	SetActorTickEnabled(true);
}

void AOutlawsLevelActor::StartDropFlightFrom(AActor* A, const FVector& Offset)
{
	using namespace OutlawsDropScatterLocal;
	if (!A || OldDropScatter()) return;
	FDropFlight F;
	F.Rest = A->GetActorLocation();
	F.From = F.Rest + Offset;
	F.Body = F.From - FVector(0.0, 0.0, StartUp);
	const FVector Flat(F.Rest.X - F.From.X, F.Rest.Y - F.From.Y, 0.0);
	F.bInPlace = Flat.Size() < 2.0;
	F.Touch = F.bInPlace ? F.Rest : FVector(F.From.X + Flat.X * TouchAt, F.From.Y + Flat.Y * TouchAt, F.Rest.Z);
	F.Apex = F.bInPlace ? 30.0 : 55.0;
	F.Hop = F.bInPlace ? 8.0 : FMath::Clamp(0.18 * (F.Apex + F.From.Z - F.Touch.Z), 4.0, 14.0);
	StartDropFlight(A, F);
}

void AOutlawsLevelActor::OnDropLandingTold(AActor* A, const FVector& At)
{
	using namespace OutlawsDropScatterLocal;
	for (FDropFlight& F : DropFlights)
	{
		if (F.Actor.Get() != A || !F.bWaiting) continue;
		const FTransform Fr = FrameOf(F);
		const FVector From = Fr.TransformPosition(F.From);
		const FVector Was = Fr.TransformPosition(F.Rest);
		// the host's spot (an older host, or none passed there: the old spot, a hop in place)
		F.Base = nullptr;
		F.bWaiting = false;
		F.bInPlace = FVector2D::Distance(FVector2D(At), FVector2D(Was)) < 3.0;
		F.Rest = F.bInPlace ? Was : At;
		const FVector Flat(F.Rest.X - From.X, F.Rest.Y - From.Y, 0.0);
		F.From = From;
		F.Touch = F.bInPlace ? F.Rest : FVector(From.X + Flat.X * TouchAt, From.Y + Flat.Y * TouchAt, F.Rest.Z);
		F.Apex = F.bInPlace ? 30.0 : 55.0;
		F.Hop = F.bInPlace ? 8.0 : FMath::Clamp(0.18 * (F.Apex + F.From.Z - F.Touch.Z), 4.0, 14.0);
		F.T = 0.f;
		if (A) A->DetachFromActor(FDetachmentTransformRules::KeepWorldTransform);
		Time(F);
		UE_LOG(LogOutlaws, Log, TEXT("OLRT drop scatter: the host's landing spot %s for our drop (%.0f cm from where it was)"), *At.ToCompactString(), FVector::Dist(At, Was));
		return;
	}
}

FVector AOutlawsLevelActor::DropRestAt(const AActor* A) const
{
	for (const FDropFlight& F : DropFlights)
	{
		if (F.Actor.Get() == A) return OutlawsDropScatterLocal::FrameOf(F).TransformPosition(F.Rest);
	}
	return A ? A->GetActorLocation() : FVector::ZeroVector;
}

bool AOutlawsLevelActor::IsDropFlying(const AActor* A) const
{
	return A && DropFlights.ContainsByPredicate([A](const FDropFlight& F) { return F.Actor.Get() == A; });
}

FString AOutlawsLevelActor::DropFlightTag(bool bHost, const FVector& FeetPos) const
{
	if (!bDropMaking) return FString();
	const FDropFlight& F = DropMaking;
	// a client's ask: the host picks round where the client made it (the ground name has 32 characters on the way there)
	if (F.bAsk && !bHost) return TEXT("@");
	// the host's: the start's offset from the landing spot (FeetPos, as told); in place: the old spot's own
	const FVector Start = (F.bInPlace ? FVector(FeetPos.X, FeetPos.Y, FMath::Max(FeetPos.Z, F.Body.Z)) : F.Body) + FVector(0.0, 0.0, OutlawsDropScatterLocal::StartUp);
	const FVector D = Start - FeetPos;
	return FString::Printf(TEXT("olfly=0/%d/%d/%d"), FMath::RoundToInt(D.X), FMath::RoundToInt(D.Y), FMath::RoundToInt(D.Z));
}

bool AOutlawsLevelActor::SplitDropFlight(FString& InOutName, FVector& OutOffset, bool& bOutAsk)
{
	// "gsgun#isgun=1,ishells=17,olfly=0/12/-40/35" (the host's start) or "gsgun#isgun=1,ishells=17,@" (a client's ask): the
	// part out of the name, what is left as it was
	bOutAsk = false;
	OutOffset = FVector::ZeroVector;
	FString Base, Contents;
	if (!InOutName.Split(TEXT("#"), &Base, &Contents)) return false;
	TArray<FString> Parts, Keep;
	Contents.ParseIntoArray(Parts, TEXT(","));
	bool bFound = false;
	for (const FString& Part : Parts)
	{
		if (Part == TEXT("@"))
		{
			bOutAsk = bFound = true;
			continue;
		}
		if (Part.StartsWith(TEXT("olfly=")))
		{
			TArray<FString> Bits;
			Part.Mid(6).ParseIntoArray(Bits, TEXT("/"));
			if (Bits.Num() >= 4)
			{
				OutOffset = FVector(FCString::Atod(*Bits[1]), FCString::Atod(*Bits[2]), FCString::Atod(*Bits[3]));
				bFound = true;
			}
			continue;
		}
		Keep.Add(Part);
	}
	if (!bFound) return false;
	InOutName = Keep.Num() > 0 ? Base + TEXT("#") + FString::Join(Keep, TEXT(",")) : Base;
	return true;
}

void AOutlawsLevelActor::TickDropFlights(float Dt)
{
	using namespace OutlawsDropScatterLocal;
	if (DropFlights.Num() == 0) return;
	OL_PERF_SCOPE("Level: drop flights");
	for (int32 i = DropFlights.Num() - 1; i >= 0; --i)
	{
		FDropFlight& F = DropFlights[i];
		AActor* A = F.Actor.Get();
		FPickup* P = A ? Pickups.FindByPredicate([A](const FPickup& X) { return X.Actor.Get() == A; }) : nullptr;
		if (!A)
		{
			DropFlights.RemoveAt(i);
			continue;
		}
		if (!P)
		{
			// (no pickup, or taken and gone from the list: where it rests, full size)
			SizeTo(A, F, 1.f);
			A->SetActorLocation(FrameOf(F).TransformPosition(F.Rest));
			DropFlights.RemoveAt(i);
			continue;
		}
		// taken out of its flight meanwhile (the other machine's player has it, or shown again somewhere): full size, as it is
		if (!P->bHeld || A->IsHidden())
		{
			SizeTo(A, F, 1.f);
			DropFlights.RemoveAt(i);
			continue;
		}
		const FTransform Fr = FrameOf(F);
		if (F.bWaiting)
		{
			F.Waited += Dt;
			A->SetActorLocation(Fr.TransformPosition(F.From));
			if (F.Waited < WaitMax) continue;
			// no word from the host: a hop where it is (the host has it there too, or it is not numbered yet)
			F.bWaiting = false;
			F.bInPlace = true;
			UE_LOG(LogOutlaws, Log, TEXT("OLRT drop scatter: no landing spot from the host in %.1f s: a hop in place"), WaitMax);
		}
		const float Was = F.T;
		F.T += Dt;
		bool bDone = false, bSecond = false;
		const FVector Pos = PointAt(F, F.T, bDone, bSecond);
		if (Was < F.T1 && F.T >= F.T1) ++F.Bounces;          // (the touch: the one bounce)
		A->SetActorLocation(Fr.TransformPosition(Pos));
		const float G = FMath::Clamp(F.T / GrowTime, 0.f, 1.f);
		const float E = G * G * (3.f - 2.f * G);
		const float S = FMath::Max(0.01f, 1.f - (1.f - E) * (1.f - E));
		SizeTo(A, F, S);
		F.MinScale = FMath::Min(F.MinScale, S);
		if (AOutlawsSpriteActor* Sp = Cast<AOutlawsSpriteActor>(A); Sp && !bSecond) Sp->OutlawsYaw += Dt * 540.f;
		if (!bDone) continue;
		SizeTo(A, F, 1.f);
		A->SetActorLocation(Fr.TransformPosition(F.Rest));
		P->bHeld = false;
		P->bWaitClear = false;
		++DropFlightsLanded;
#if WITH_OUTLAWS_TESTS
		FDropFlightDone D;
		D.Actor = A;
		D.Body = F.Body;
		D.Rest = A->GetActorLocation();
		D.Bounces = F.Bounces;
		D.MinScale = F.MinScale;
		D.Time = F.T;
		D.bInPlace = F.bInPlace;
		DropFlightsDone.Add(D);
		if (DropFlightsDone.Num() > 256) DropFlightsDone.RemoveAt(0);
#endif
		DropFlights.RemoveAt(i);
	}
}

#if WITH_OUTLAWS_TESTS
bool AOutlawsLevelActor::DebugDropPickup(const AActor* A, bool& bOutHeld, int32& OutCoopId, bool bMakeRoom)
{
	bOutHeld = false;
	OutCoopId = -1;
	for (FPickup& P : Pickups)
	{
		if (!A || P.Actor.Get() != A) continue;
		bOutHeld = P.bHeld;
		OutCoopId = P.CoopId;
		if (bMakeRoom) for (const FPickup::FEntry& E : P.Entries) TakeCount(E.Item, GetCount(E.Item));
		if (bMakeRoom && P.Item.Len() > 0) TakeCount(P.Item, GetCount(P.Item));
		return true;
	}
	return false;
}

AActor* AOutlawsLevelActor::DebugDropWithContents(const FString& Name, const FString& Contents, const FVector& Feet)
{
	UOutlawsCoop* C = UOutlawsCoop::Get(this);
	if (C) C->GroundNameSuffix = TEXT("#") + Contents;
	AActor* A = SpawnDropItem(Name, Feet, Feet);
	if (C) C->GroundNameSuffix.Reset();
	if (A) UOutlawsCoop::ApplyGroundContents(this, A, Contents);
	return A;
}
#endif
