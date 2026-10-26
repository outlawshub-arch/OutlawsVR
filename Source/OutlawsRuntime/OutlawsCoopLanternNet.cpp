// BUG-LANTERNNET (user 2026-10-10, RC2): in co-op a partner's lit lantern lit nothing here, neither the room nor the partner's
// own sprite.  The lantern's light is no engine light: it is slot 0 of MPC_OLR_Lights, summed by the world, water, held and
// sprite materials (ue_olue_lights_material.py), and only this machine's own lantern ever wrote it.  The pose now carries the
// lantern (FOutlawsCoopPose::Lamp, LampAt, LampOil) and each machine draws the others' lit lanterns in its lamp slots
// (AOutlawsLevelActor::TickLampLights).  This is its check, -OLCoopAuto -OLCoopTest=lanternnet (run_lanternnet.ps1).
//
// The host holds first, then the client: a lantern taken (lit by the headset's rule: held with oil), put away by the wheel,
// taken again, carried into water (the swim puts it away) and brought out.  The other machine watches the holder's pose and
// checks its own copy of the light at each change: lit, in a slot at the holder's flame, with the holder's reach and colour
// for its oil, the holder's sprite inside it; gone once put away, both ways.

#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsRemotePlayer.h"
#include "OutlawsSpriteActor.h"
#include "OutlawsDeviceProfile.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Engine/World.h"

namespace
{
	// the lantern's light without its flicker, for its oil (AOutlawsLevelActor::LanternLight's); REQ-LANTERNBOOST's 22 m and
	// 0.75 written out here (were 11 m and 0.6), so the check holds the level's numbers to the request
	float LnBaseReach(float Oil) { return 2200.f * FMath::Lerp(0.65f, 1.f, FMath::Clamp(Oil, 0.f, 1.f)); }
	FLinearColor LnBaseColour(float Oil) { return FLinearColor(1.f, 0.86f, 0.68f) * (0.75f * FMath::Lerp(0.3f, 1.f, FMath::Sqrt(FMath::Clamp(Oil, 0.f, 1.f)))); }
	const TCHAR* LnLampWord(uint8 Lamp) { return Lamp == 2 ? TEXT("lit") : Lamp == 1 ? TEXT("in hand, out") : TEXT("put away"); }
}

// The other player's lantern as this machine draws it, checked at each change of their pose's lantern; true once four
// changes (lit, put away by the wheel, lit again, put away by the swim) have been checked.
bool UOutlawsCoop::LanternNetObserve(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	if (LnSeen >= 4) return true;
	const bool bHost = IsHost();
	const TCHAR* Me = bHost ? TEXT("host") : TEXT("client");
	const TCHAR* Them = bHost ? TEXT("client") : TEXT("host");
	const int32 Who = PartnerIndex();
	const AOutlawsCoopLink* PL = GetLink(Who);
	AOutlawsRemotePlayer* R = PartnerPuppet();
	if (!PL || PL->Pose.Seq <= 0 || !R) return false;
	const FOutlawsCoopPose& Q = PL->Pose;
	if (Q.Lamp != LnLamp)
	{
		LnLamp = Q.Lamp;
		LnSince = Now;
	}
	// facing the holder (the screenshots)
	if (APlayerController* PC = Cast<APlayerController>(P->GetController()))
	{
		const FVector Eye = P->GetPawnViewLocation();
		PC->SetControlRotation((R->ShownBody() - FVector(0.0, 0.0, 20.0) - Eye).Rotation());
	}
	auto Check = [this, Me](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lanternnet check %s (%s): %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), Me, *What);
		if (bOk) ++LnPasses; else ++LnFails;
	};
	UMaterialParameterCollectionInstance* Mpc = L->LightsMpc && L->GetWorld() ? L->GetWorld()->GetParameterCollectionInstance(L->LightsMpc) : nullptr;
	auto Counts = [L, Mpc]()
	{
		FLinearColor C0(0.f, 0.f, 0.f, 0.f);
		if (Mpc) Mpc->GetVectorParameterValue(TEXT("C0"), C0);
		return FString::Printf(TEXT("lights here: own lantern %d, other players' lanterns %d shown (%d lit in reach, cap %d), lamp lights %d"),
			C0.R + C0.G + C0.B > 0.f ? 1 : 0, L->RemoteLanternsShown.Num(), L->RemoteLanternsLit, AOutlawsLevelActor::RemoteLanternCap(), L->LampSlotsLit);
	};
	const bool bWantLit = LnSeen == 0 || LnSeen == 2;
	const double Settle = bWantLit ? 1.5 : 1.0;
	if ((Q.Lamp == 2) != bWantLit || Now - LnSince < Settle) return false;
	const TCHAR* Stage[] = { TEXT("lit"), TEXT("put away by the wheel"), TEXT("lit again"), TEXT("put away by the swim") };
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lanternnet: the %s hears the %s's lantern %s (%s); %s"), Me, Them, LnLampWord(Q.Lamp), Stage[LnSeen], *Counts());
	const AOutlawsLevelActor::FRemoteLanternShown* Sh = L->RemoteLanternsShown.FindByPredicate([Who](const AOutlawsLevelActor::FRemoteLanternShown& X) { return X.Who == Who; });
	if (bWantLit)
	{
		Check(Sh != nullptr, FString::Printf(TEXT("%s: the %s's lantern lights a slot here (%s)"), Stage[LnSeen], Them, Sh ? *FString::Printf(TEXT("L%d"), Sh->Slot) : TEXT("none")));
		if (Sh)
		{
			LnSlot = Sh->Slot;
			FLinearColor Pos(0.f, 0.f, 0.f, 0.f), Col(0.f, 0.f, 0.f, 0.f);
			const bool bRead = Mpc && Mpc->GetVectorParameterValue(*FString::Printf(TEXT("L%d"), Sh->Slot), Pos) && Mpc->GetVectorParameterValue(*FString::Printf(TEXT("C%d"), Sh->Slot), Col);
			const FVector At(Pos.R, Pos.G, Pos.B);
			Check(bRead && At.Equals(Sh->At, 0.01) && FMath::IsNearlyEqual(Pos.A, Sh->Pos.A, 0.01f) && Col.Equals(Sh->Col, 0.0001f),
				FString::Printf(TEXT("the collection's L%d / C%d hold it: %s reach %.1f, colour %s"), Sh->Slot, Sh->Slot, *At.ToString(), Pos.A, *Col.ToString()));
			// the place: the holder's own light's place (the pose's LampAt is what the holder's machine lights), a few cm
			const double DSent = FVector::Dist(At, FVector(Q.LampAt));
			const double DHand = FMath::Min(FVector::Dist(At, R->ShownBody() + (FVector(Q.RightHand) - FVector(Q.Body))), FVector::Dist(At, R->ShownBody() + (FVector(Q.LeftHand) - FVector(Q.Body))));
			Check(DSent <= 3.0, FString::Printf(TEXT("at the %s's own light, %.2f cm off (sent %s)"), Them, DSent, *FVector(Q.LampAt).ToString()));
			Check(DHand <= 60.0, FString::Printf(TEXT("hanging from the %s's hand as drawn here, %.1f cm from the fist"), Them, DHand));
			// the settings: the holder's reach and colour for the oil sent, the same flicker on top
			const float Oil = Q.LampOil / 255.f;
			const float Reach = Pos.A / (1.f + 0.3f * (Sh->Flicker - 1.f));
			const FLinearColor Base = Col * (1.f / FMath::Max(Sh->Flicker, 0.01f));
			Check(FMath::Abs(Reach - LnBaseReach(Oil)) <= 0.5f && Base.Equals(LnBaseColour(Oil), 0.002f) && Sh->Flicker > 0.75f && Sh->Flicker < 1.25f,
				FString::Printf(TEXT("the lantern's settings: base reach %.1f cm (the lantern's %.1f for oil %d/255), base colour %s (%s), flicker %.3f"),
					Reach, LnBaseReach(Oil), Q.LampOil, *Base.ToString(), *LnBaseColour(Oil).ToString(), Sh->Flicker));
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lanternnet remote lantern (%s sees %s): at %s base reach %.1f base colour %.4f %.4f %.4f"), Me, Them, *At.ToString(), Reach, Base.R, Base.G, Base.B);
			// the holder's sprite inside the light, drawn with a material that sums the lights
			const AOutlawsSpriteActor* Spr = R->ShownSprite();
			const UMaterialInterface* M = Spr && Spr->Quad ? Spr->Quad->GetMaterial(0) : nullptr;
			const FString Base0 = M && M->GetMaterial() ? M->GetMaterial()->GetName() : FString(TEXT("none"));
			const bool bLitMat = Base0 == TEXT("M_OL_Sprite") || Base0 == TEXT("M_OLR_SpriteFade") || Base0 == TEXT("M_OLR_SpriteOnTop");
			const double DSpr = Spr && Spr->Quad ? FVector::Dist(Spr->Quad->Bounds.Origin, At) : 1e9;
			Check(bLitMat && DSpr < Pos.A * 0.5, FString::Printf(TEXT("the %s's sprite (%s) %.0f cm from the light, inside its %.0f cm"), Them, *Base0, DSpr, Pos.A));
			Shot(FString::Printf(TEXT("ol_lanternnet_%s_sees_%s_lit%d"), Me, Them, LnSeen / 2 + 1));
		}
	}
	else
	{
		FLinearColor Col(0.f, 0.f, 0.f, 0.f);
		const bool bRead = LnSlot >= 4 && Mpc && Mpc->GetVectorParameterValue(*FString::Printf(TEXT("C%d"), LnSlot), Col);
		const bool bLampThere = LnSlot >= 4 && L->LampLights[LnSlot - 4].Fade > 0.f;
		Check(Sh == nullptr && L->RemoteLanternsLit == 0 && Q.Lamp == 0, FString::Printf(TEXT("%s: the %s's lantern lights nothing here (its pose: %s)"), Stage[LnSeen], Them, LnLampWord(Q.Lamp)));
		Check(bRead && (Col.R + Col.G + Col.B <= 0.f || bLampThere), FString::Printf(TEXT("its slot L%d dark again (colour %s%s)"), LnSlot, *Col.ToString(), bLampThere ? TEXT(", a lamp's again") : TEXT("")));
		if (LnSeen == 1) Shot(FString::Printf(TEXT("ol_lanternnet_%s_sees_%s_out"), Me, Them));
	}
	++LnSeen;
	return LnSeen >= 4;
}

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TickLanternNetTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	const bool bHost = IsHost();
	const TCHAR* Me = bHost ? TEXT("host") : TEXT("client");
	L->bInvincible = true;
	P->bTestLampFollowsHand = true;            // (the headset's rule on this desktop run: held with oil is lit)
	auto Check = [this, Me](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lanternnet check %s (%s): %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), Me, *What);
		if (bOk) ++LnPasses; else ++LnFails;
	};
	auto Own = [this, L, Me](const TCHAR* When)
	{
		FVector At;
		float Oil = 0.f;
		if (!L->LanternShining(At, Oil)) return;
		const FLinearColor C = LnBaseColour(Oil);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lanternnet own lantern (%s, %s): at %s base reach %.1f base colour %.4f %.4f %.4f"), Me, When, *At.ToString(), LnBaseReach(Oil), C.R, C.G, C.B);
	};
	const bool bWatched = LanternNetObserve(Now, L, P);
	// the host holds first (steps 1..7), then watches the client's turn; the client watches first, then holds
	switch (LnStep)
	{
	case 0:
		if (bHost)
		{
			const AOutlawsCoopLink* CL = GetLink(PartnerIndex());
			if (!CL || CL->BuiltSerial != StartedSerial || !PartnerPuppet()) return;
			if (LnAt == 0.0) { LnAt = Now + 4.0; return; }
			if (Now < LnAt) return;
		}
		else
		{
			// the client: once the host's turn is watched, and the host is back on dry ground near (its swim is 30 m off,
			// past the others' lanterns' reach)
			if (!bWatched) return;
			const AOutlawsCoopLink* HL = GetLink(PartnerIndex());
			if (LnAt == 0.0) { LnAt = Now + 4.0; return; }
			if (Now < LnAt || !HL || FVector::Dist(FVector(HL->Pose.Body), P->GetActorLocation()) > 1500.0) return;
		}
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lanternnet: the %s's turn to hold the lantern (the others' lanterns cap %d, the Lamp Lights row %d, %s)"), Me, AOutlawsLevelActor::RemoteLanternCap(),
			OutlawsDeviceProfile::LampLights(), OutlawsDeviceProfile::IsQuest2() ? TEXT("Quest 2 profile") : TEXT("default profile"));
		LnStep = 1;
		return;
	case 1:
		if (L->GetCount(TEXT("ilantern")) <= 0) L->AddCount(TEXT("ilantern"), 1);
		L->AddCount(TEXT("ioil"), 150);
		P->CoopTestTool(1, TEXT("ilantern"));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lanternnet: the %s takes the lantern in the right hand"), Me);
		LnAt = Now + 2.0;
		LnStep = 2;
		return;
	case 2:
		if (Now < LnAt) return;
		Check(L->bLampOn && LampSent == 2, FString::Printf(TEXT("taken with oil: lit here and sent lit (lamp %d, sent %s)"), L->bLampOn ? 1 : 0, LnLampWord(LampSent)));
		Own(TEXT("lit"));
		LnAt = Now + 4.0;
		LnStep = 3;
		return;
	case 3:
		if (Now < LnAt) return;
		Own(TEXT("lit, before the wheel"));
		P->CoopTestToggleGear(1, TEXT("ilantern"));           // (the wheel's second pick of it: put away)
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lanternnet: the %s puts the lantern away by the wheel"), Me);
		LnAt = Now + 3.0;
		LnStep = 4;
		return;
	case 4:
		if (Now < LnAt) return;
		Check(!L->bLampOn && LampSent == 0, FString::Printf(TEXT("put away: out here and sent put away (lamp %d, sent %s)"), L->bLampOn ? 1 : 0, LnLampWord(LampSent)));
		P->CoopTestTool(1, TEXT("ilantern"));
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lanternnet: the %s takes the lantern again"), Me);
		LnAt = Now + 4.0;
		LnStep = 5;
		return;
	case 5:
	{
		if (Now < LnAt) return;
		Check(L->bLampOn && LampSent == 2, FString::Printf(TEXT("taken again: lit (lamp %d, sent %s)"), L->bLampOn ? 1 : 0, LnLampWord(LampSent)));
		Own(TEXT("lit again"));
		const int32 Sec = L->DebugWaterSector();
		FVector Feet;
		float SlopeYaw = 0.f;
		if (Sec < 0 || !L->SlopeFoot(Sec, Feet, SlopeYaw))
		{
			Check(false, TEXT("a water sector in this level"));
			LnAt = Now;
			LnStep = 7;
			return;
		}
		LnDry = P->GetActorLocation();
		P->TeleportTo(Feet + FVector(0.0, 0.0, P->GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0), P->GetActorRotation(), false, true);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lanternnet: the %s goes into water sector %d"), Me, Sec);
		LnAt = Now + 3.0;
		LnStep = 6;
		return;
	}
	case 6:
	{
		if (Now < LnAt) return;
		int32 W = -1;
		FString T0, T1;
		P->CoopHeld(0, W, T0);
		P->CoopHeld(1, W, T1);
		Check(P->IsSwimming() && T0 != TEXT("ilantern") && T1 != TEXT("ilantern") && !L->bLampOn && LampSent == 0,
			FString::Printf(TEXT("in the water: swimming %d, hands '%s' '%s', lamp %d, sent %s"), P->IsSwimming() ? 1 : 0, *T0, *T1, L->bLampOn ? 1 : 0, LnLampWord(LampSent)));
		LnAt = Now + 3.0;
		LnStep = 7;
		return;
	}
	case 7:
		if (Now < LnAt) return;
		if (!LnDry.IsZero()) P->TeleportTo(LnDry, P->GetActorRotation(), false, true);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lanternnet: the %s's turn done (%d passed, %d failed)"), Me, LnPasses, LnFails);
		if (!bHost) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lanternnet client RESULT %s (%d passed, %d failed)"), LnFails == 0 && LnPasses > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), LnPasses, LnFails);
		LnAt = Now + 120.0;
		LnStep = bHost ? 8 : 20;
		return;
	case 8:
		// the host: the client's turn, watched
		if (!bWatched && Now < LnAt) return;
		if (!bWatched) Check(false, FString::Printf(TEXT("the client's turn watched in time (%d of 4 changes)"), LnSeen));
		// (the client's own last checks, in the water and back, before it is told to quit)
		if (bWatched && LnAt > Now + 8.0) { LnAt = Now + 8.0; return; }
		if (bWatched && Now < LnAt) return;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP lanternnet RESULT %s (the host's checks: %d passed, %d failed; the client's in its own log)"),
			LnFails == 0 && LnPasses > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), LnPasses, LnFails);
		TestSay(TEXT("quit"), 0);
		LnAt = Now + 2.0;
		LnStep = 9;
		return;
	case 9:
		if (Now < LnAt) return;
		Summary(TEXT("done"));
		QuitForTest();
		LnStep = 10;
		return;
	default:
		return;
	}
}
#endif // WITH_OUTLAWS_TESTS
