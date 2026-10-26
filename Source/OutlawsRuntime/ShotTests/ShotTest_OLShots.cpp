// -OLShots with no test flag: the see-through mids (glass, then others), one screenshot each (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLShots) { return true; }

OL_SHOT_TEST(OLShots, 10000)
{
	static const struct { int32 Nth; bool bShatter; const TCHAR* Name; } Views[] = {
		{ 0, true, TEXT("ol_glass0") }, { 4, true, TEXT("ol_glass4") }, { 0, false, TEXT("ol_mid0") }, { 6, false, TEXT("ol_mid6") }, { 6, false, TEXT("ol_sky") } };
	const int32 View = ShotStep / 2;
	const float Due = 3.f + ShotStep * 1.2f;
	if (ShotTime < Due) return;
	if (View >= UE_ARRAY_COUNT(Views))
	{
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
		return;
	}
	if (ShotStep % 2 == 0)
	{
		FVector Floor;
		float Yaw = 0.f;
		FString What;
		const bool bSky = FCString::Strcmp(Views[View].Name, TEXT("ol_sky")) == 0;
		if (bSky)
		{
			// outdoors at the level start, looking up and away
			Floor = L->GetPlayerStartLocation() - FVector(0.0, 0.0, 110.0);
			Yaw = L->GetPlayerStartYaw();
			What = TEXT("player start");
		}
		if (bSky || L->DebugMidView(Views[View].Nth, Views[View].bShatter, Floor, Yaw, What))
		{
			TeleportTo(Floor + FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0), FRotator(0.0, Yaw, 0.0), false, true);
			const double Pitch = FCString::Strcmp(Views[View].Name, TEXT("ol_sky")) == 0 ? 40.0 : 0.0;   // the sky view looks up
			if (AController* C = GetController()) C->SetControlRotation(FRotator(Pitch, Yaw + (Pitch > 0.0 ? 180.0 : 0.0), 0.0));
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT %s: %s at %s yaw %.0f"), Views[View].Name, *What, *Floor.ToString(), Yaw);
		}
		else
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT %s: no such mid"), Views[View].Name);
		}
	}
	else
	{
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(Views[View].Name) + TEXT(".png"), false, false);
		// what the view looks at (the sky shot: dome or a ceiling), with the texture it draws
		FHitResult Hit;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsShotLook), true, this);
		const FVector From = Camera->GetComponentLocation();
		if (GetWorld()->LineTraceSingleByChannel(Hit, From, From + Camera->GetForwardVector() * 20000.0, ECC_Visibility, Q) && Hit.GetComponent())
		{
			UPrimitiveComponent* C = Hit.GetComponent();
			UTexture* Tex = nullptr;
			if (UMaterialInterface* M = C->GetMaterial(FMath::Max(0, Hit.FaceIndex >= 0 ? 0 : 0))) M->GetTextureParameterValue(TEXT("Tex"), Tex);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT %s looks at %s (%s) %.0f cm away, first material texture %s; %s"), Views[View].Name, *C->GetName(),
				*C->GetClass()->GetName(), Hit.Distance, Tex ? *Tex->GetName() : TEXT("-"), L ? *L->DescribeSectorsAt(Hit.ImpactPoint) : TEXT(""));
		}
		else
		{
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT %s looks at nothing solid (the sky dome)"), Views[View].Name);
		}
	}
	++ShotStep;
}

#endif // WITH_OUTLAWS_TESTS
