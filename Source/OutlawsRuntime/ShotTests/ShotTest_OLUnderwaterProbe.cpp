// -OLShots -OLUnderwaterProbe (BUG-MPUNDERWATER, 9 Nov; the user: in multiplayer, under the water, the blue breath bar did not
// show and they could shoot).  Single player has both (OutlawsPlayer.cpp UpdateBreath: the air, the wrist's blue gauge, drowning;
// OutlawsPlayerWeapons.cpp FireGun: no gun fires with its muzzle under the water).  This probe, in whatever level it is run in
// (a story level, a match as host, a match as a client): into the level's deepest water sector, held there, the eyes under;
// 3 s on, then a revolver trigger, then the air run out.  Lines:
//   "OLSHOT underwater <role> <level>: water sector <n> ..., head under <0|1>, air <a> -> <b>, gauge <0|1>, gun fired <0|1>, drowned <hp> -> <hp>"
//   "OLSHOT underwater check PASS|FAIL: <what>"   then "OLSHOT underwater RESULT <n> PASS, <m> FAIL".
// -OLUnderwaterHold=<s>: stay that long after (a host keeps its match up for its client).

#include "ShotTestCommon.h"
#include "OutlawsInfRuntime.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLUnderwaterProbe) { return OLFlags::Param(TEXT("OLUnderwaterProbe")); }

OL_SHOT_TEST(OLUnderwaterProbe, 2579)
{
	static int32 Step = 0, Pass = 0, Fail = 0, Sec = -1, MagBefore = 0;
	static float InLevel = 0.f, At = 0.f, Air0 = 1.f, Hp0 = 0.f;
	static FVector Spot = FVector::ZeroVector;
	static double Done = 0.0;
	if (Step < 0 || !L) return;
	if (L->IsFrontEnd() || bMenuOpen) { InLevel = 0.f; return; }
	InLevel += Dt;
	const FString UwRole = OLFlags::Has(TEXT("OLCoopJoin")) ? TEXT("client") : OLFlags::Has(TEXT("OLMpHost")) ? TEXT("host") : TEXT("story");
	auto Check = [&](bool bOk, const FString& What)
	{
		(bOk ? Pass : Fail)++;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT underwater check %s: %s %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *UwRole, *What);
	};
	if (Step == 0)
	{
		if (InLevel < 10.f) return;
		// the deepest water sector, from the INF's live level or (a level with no .inf) the parsed one: picked here, not by the
		// level's own lookups, which are what this probe tests
		const FOutlawsInfRuntime* Ir = L->DebugInf();
		const FOutlawsLevel* Lp = Ir ? &Ir->Live : L->DebugLevelData();
		Sec = -1;
		double Deep = 0.0;
		if (Lp) for (const TPair<int32, FLinearColor>& Kv : L->WaterTints)
			if (const FOutlawsSector* Ws = Lp->Sector(Kv.Key)) if (Ws->Ceil.Y - Ws->Floor.Y > Deep && Ws->Verts.Num() >= 3) { Deep = Ws->Ceil.Y - Ws->Floor.Y; Sec = Kv.Key; }
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT underwater %s level: inf %d, %d water tints, deepest water sector %d (%.1f units)"), *UwRole, Ir ? 1 : 0, L->WaterTints.Num(), Sec, Deep);
		if (Sec < 0)
		{
			Check(false, TEXT("a water sector in this level"));
			Step = 9; Done = FPlatformTime::Seconds();
			return;
		}
		bVR = true;                      // (the wrist and its gauge are the headset's; this run only)
		const FOutlawsSector& Ws = Lp->Sectors[Sec];
		FVector2D C = FVector2D::ZeroVector;
		for (const FVector2D& V : Ws.Verts) C += V;
		C /= Ws.Verts.Num();
		const FVector Feet = FOutlawsGeometry::ToUE(C.X, Lp->SurfaceY(Ws, C, true) + 0.2, C.Y);
		Spot = Feet + FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0);
		TeleportTo(Spot, GetActorRotation(), false, true);
		Step = 1; At = 0.f;
		return;
	}
	if (Step >= 1 && Step <= 3) SetActorLocation(Spot, false, nullptr, ETeleportType::TeleportPhysics);     // (held there)
	At += Dt;
	if (Step == 1 && At > 0.5f)
	{
		Air0 = Air;
		int32 Revolver = -1;
		for (int32 i = 0; i < 32; ++i) if (WeaponLabel(i).Contains(TEXT("revolver"))) { Revolver = i; break; }
		Equip(1, Revolver);
		Step = 2; At = 0.f;
		return;
	}
	if (Step == 2 && At > 3.f)
	{
		FLinearColor Tint;
		float Surf = 0.f;
		const bool bTint = L->UnderwaterTintAt(Camera->GetComponentLocation(), Tint);
		const bool bWater = L->WaterAt(Camera->GetComponentLocation(), Surf);
		WristLast.Reset();
		LayoutWrist();                   // (what the wrist would show raised: its signature says the gauge is the air's)
		// (the signature: health|armour|HD textures|under|gauge steps: the fourth field is the air's gauge)
		TArray<FString> Fields;
		WristLast.ParseIntoArray(Fields, TEXT("|"), false);
		const bool bGauge = Fields.Num() >= 4 && Fields[3] == TEXT("1");
		Mag[1] = 6;
		FireCooldown[1] = 0.f;
		MagBefore = Mag[1];
		const int32 Calls = GunFireCalls;
		FireGun(1, false);
		const bool bFired = Mag[1] < MagBefore;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT underwater %s %s: water sector %d (%d tints), eyes in water %d (%.0f cm under), tint %d, head under %d, air %.3f -> %.3f, gauge %d, swimming %d, gun fired %d (calls %d, mag %d -> %d), wrist '%s'"),
			*UwRole, *L->GetName(), Sec, L->WaterTints.Num(), bWater ? 1 : 0, Surf - Camera->GetComponentLocation().Z, bTint ? 1 : 0, bHeadUnder ? 1 : 0, Air0, Air,
			bGauge ? 1 : 0, IsSwimming() ? 1 : 0, bFired ? 1 : 0, GunFireCalls - Calls, MagBefore, Mag[1], *WristLast);
		Check(bWater && bHeadUnder, TEXT("the eyes under the water read as under"));
		Check(Air < Air0 - 0.03f, FString::Printf(TEXT("the air goes down under the water (%.3f -> %.3f)"), Air0, Air));
		Check(bGauge, TEXT("the wrist shows the blue breath gauge"));
		Check(!bFired, TEXT("no gun fires under the water (the hammer clicks)"));
		// (-OLUnderwaterShot, the sign-off picture: under the water, the wrist up, just after the trigger)
		if (OLFlags::Param(TEXT("OLUnderwaterShot")))
			FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(TEXT("ol_underwater_")) + UwRole + TEXT(".png"), false, false);
		Hp0 = L->GetHealth();
		L->bInvincible = false;          // (-OLShots' own: the drowning measured)
		Air = 0.f;
		DrownWait = 0.f;
		Step = 3; At = 0.f;
		return;
	}
	if (Step == 3 && At > 1.5f)
	{
		const float Hp1 = L->GetHealth();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT underwater %s drowning: health %.2f -> %.2f"), *UwRole, Hp0, Hp1);
		Check(Hp1 < Hp0, FString::Printf(TEXT("out of air, drowning hurts (%.2f -> %.2f)"), Hp0, Hp1));
		L->bInvincible = true;
		Step = 9; Done = FPlatformTime::Seconds();
		return;
	}
	if (Step == 9)
	{
		float Hold = 0.f;
		FString V;
		if (OLFlags::Value(TEXT("OLUnderwaterHold"), V)) Hold = FCString::Atof(*V);
		if (FPlatformTime::Seconds() - Done < 1.0 + Hold) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT underwater RESULT %d PASS, %d FAIL"), Pass, Fail);
		Step = -1;
		FGenericPlatformMisc::RequestExit(false);
	}
}

#endif // WITH_OUTLAWS_TESTS
