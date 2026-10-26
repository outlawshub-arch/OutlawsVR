// REQ-KILLFEED: the kill feed, a match's "Marshal killed Deputy!" lines (OutlawsCoopKillFeed.cpp words them).  Fixed to the
// view above the notices (their block starts 10 degrees up; this one 18.5), the newest at the bottom, four at most, each
// held 6 s and then fading out over its last 1.5 s (its strokes thinned away through M_OLR_TextTop's "Weight", the colour
// dimmed: the overlay's opacity is the glyph's own, it has no alpha to fade).  Nowhere near the middle of the view, where
// the aim is.  Like the notices it comes in front of a wall nearer than its 80 cm (the headset's text is depth tested).
//
// -OLShots -OLKillFeedTest=shot[q3] (one machine, no match): four sample lines and a notice, photographed at the Quest 2's
// (or the Quest 3's) eye buffer size and field of view, then again as they fade, and the feed checked empty after.
#include "OutlawsPlayer.h"
#include "OutlawsFlags.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "Camera/CameraComponent.h"
#include "Components/TextRenderComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
#include "OutlawsShotTests.h"

namespace
{
	constexpr int32 KillFeedMax = 4;
	constexpr float KillFeedSeconds = 6.f;
	constexpr float KillFeedFade = 1.5f;
	constexpr double KillFeedFull = 80.0;            // cm from the eyes (the notices' distance)
	constexpr double KillFeedUpDeg = 18.5;           // the bottom line's angle above the middle of the view
	constexpr double KillFeedStep = 2.8;             // cm between lines at the full distance
	constexpr float KillFeedSize = 2.4f;             // the glyphs' height (the notices' 2.9: these are secondary)
	const FColor KillFeedColour(225, 215, 190);
	const FColor KillFeedMine(255, 196, 120);        // this player's own kill or death

	FCollisionResponseParams KillFeedNoPawns()
	{
		FCollisionResponseParams RP;
		RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);
		return RP;
	}
}

void AOutlawsPlayer::ShowKillLine(const FString& Text, bool bMine)
{
	if (Text.IsEmpty()) return;
	FKillLine& K = KillLines.InsertDefaulted_GetRef(0);
	K.Text = Text;
	K.Left = KillFeedSeconds;
	K.bMine = bMine;
	if (KillLines.Num() > KillFeedMax) KillLines.SetNum(KillFeedMax);
}

void AOutlawsPlayer::UpdateKillFeed(float Dt)
{
	for (int32 i = KillLines.Num() - 1; i >= 0; --i)
	{
		KillLines[i].Left -= Dt;
		if (KillLines[i].Left <= 0.f) KillLines.RemoveAt(i);
	}
	if (KillLines.Num() == 0 && KillFeedText.Num() == 0) return;
	if (!Camera) return;
	if (KillFeedText.Num() == 0)
	{
		UMaterialInterface* Top = LoadOverlayMaterial(TEXT("M_OLR_TextTop"));
		if (!Top) Top = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_Text.M_OLR_Text"));
		for (int32 i = 0; i < KillFeedMax; ++i)
		{
			UTextRenderComponent* T = NewObject<UTextRenderComponent>(this);
			T->SetHorizontalAlignment(EHTA_Center);
			T->SetVerticalAlignment(EVRTA_TextBottom);
			T->SetWorldSize(KillFeedSize);
			T->SetTextRenderColor(KillFeedColour);
			UMaterialInstanceDynamic* Mid = Top ? UMaterialInstanceDynamic::Create(Top, this) : nullptr;
			if (Mid) T->SetTextMaterial(Mid);
			T->SetTranslucentSortPriority(8);
			T->SetRenderCustomDepth(true);           // stencil 3: no ink line over it (as the notices)
			T->SetCustomDepthStencilValue(3);
			T->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			T->SetCastShadow(false);
			T->SetDepthPriorityGroup(SDPG_Foreground);
			T->SetupAttachment(Camera);
			T->SetText(FText::GetEmpty());
			T->SetVisibility(false);
			T->RegisterComponent();
			KillFeedText.Add(T);
			KillFeedMids.Add(Mid);
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLRT kill feed made: %d lines, material %s"), KillFeedMax, Top ? *Top->GetName() : TEXT("none"));
	}
	// in front of the nearest surface along its line of sight (as the notices: BUG-MSGCLIP), looked for across its width (its
	// ends too: a wall beside the middle of the view cut the lines short), and back out gently
	const double Tan = FMath::Tan(FMath::DegreesToRadians(KillFeedUpDeg));
	if (KillLines.Num() > 0)
	{
		const FTransform& CT = Camera->GetComponentTransform();
		const FVector Eye = CT.GetLocation();
		const FVector Fwd = CT.GetUnitAxis(EAxis::X);
		double Want = KillFeedFull;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsKillFeedClear), false, this);
		for (const auto& H : HeldItem) if (H.IsValid()) Q.AddIgnoredActor(H.Get());
		for (const double Side : { 0.0, -0.16, 0.16 })          // (the middle, and about 9 degrees either way: the lines' ends)
		{
			const FVector Way = CT.TransformVectorNoScale(FVector(1.0, Side, Tan)).GetSafeNormal();
			const double Along = KillFeedFull * FMath::Sqrt(1.0 + Tan * Tan + Side * Side);
			FHitResult Hit;
			if (GetWorld() && GetWorld()->LineTraceSingleByChannel(Hit, Eye, Eye + Way * (Along + 10.0), ECC_Visibility, Q, KillFeedNoPawns()))
			{
				Want = FMath::Min(Want, FMath::Clamp(FVector::DotProduct(FVector(Hit.ImpactPoint) - Eye, Fwd) - 5.0, 20.0, KillFeedFull));
			}
		}
		KillFeedDist = Want < KillFeedDist ? float(Want) : FMath::FInterpTo(KillFeedDist, float(Want), Dt, 4.f);
	}
	const double D = KillFeedDist, S = D / KillFeedFull;
	for (int32 i = 0; i < KillFeedText.Num(); ++i)
	{
		UTextRenderComponent* T = KillFeedText[i];
		if (!T) continue;
		if (!KillLines.IsValidIndex(i))
		{
			if (T->IsVisible()) T->SetVisibility(false);
			continue;
		}
		const FKillLine& K = KillLines[i];
		const FText Want = FText::FromString(K.Text);
		if (!T->Text.EqualTo(Want)) T->SetText(Want);
		// the fade: 1 while held, down to 0 over the last KillFeedFade seconds (quantized: no render update every frame)
		const float A = FMath::RoundToFloat(FMath::Clamp(K.Left / KillFeedFade, 0.f, 1.f) * 20.f) / 20.f;
		const FColor Base = K.bMine ? KillFeedMine : KillFeedColour;
		const float Dim = 0.35f + 0.65f * A;
		const FColor C(uint8(Base.R * Dim), uint8(Base.G * Dim), uint8(Base.B * Dim));
		if (T->TextRenderColor != C) T->SetTextRenderColor(C);
		if (UMaterialInstanceDynamic* Mid = KillFeedMids.IsValidIndex(i) ? KillFeedMids[i].Get() : nullptr)
		{
			Mid->SetScalarParameterValue(TEXT("Weight"), FMath::Lerp(0.545f, 0.5f, A));      // (0.54 wipes the strokes out)
		}
		T->SetRelativeLocationAndRotation(FVector(D, 0.0, D * Tan + i * KillFeedStep * S), FRotator(0.0, 180.0, 0.0));
		T->SetRelativeScale3D(FVector(S));
		if (!T->IsVisible()) T->SetVisibility(true);
	}
	if (KillLines.Num() != KillFeedLogged)
	{
		KillFeedLogged = KillLines.Num();
		UE_LOG(LogOutlaws, Log, TEXT("OLRT kill feed: %d lines at %.0f cm (dt %.3f)"), KillLines.Num(), KillFeedDist, Dt);
	}
}

#if WITH_OUTLAWS_TESTS
void AOutlawsPlayer::KillFeedShotTick(float Dt)
{
	AOutlawsLevelActor* L = Level.Get();
	if (!L || !Camera) return;
	static float Quiet = -1.f, At = 0.f;
	static int32 Fails = 0;
	const bool bQ3 = OLFlags::ValueIs(TEXT("OLKillFeedTest"), TEXT("shotq3"));
	const TCHAR* Tag = bQ3 ? TEXT("q3") : TEXT("q2");
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT kf shot check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (!bOk) ++Fails;
	};
	auto Shoot = [Tag](const TCHAR* Name)
	{
		const FString Path = FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("ol_kf_%s_%s.png"), Tag, Name);
		FScreenshotRequest::RequestScreenshot(Path, false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT kf shot: %s"), *Path);
	};
	if (ShotStep == 0)
	{
		if (!TestShadersReady(Quiet, 3.f) || ShotTime < 5.f) return;
		// (an eye's field of view: the Quest 2 about 97 degrees across, the Quest 3 about 104; the window is one eye's buffer)
		Camera->SetFieldOfView(bQ3 ? 104.f : 97.f);
		ShowKillLine(TEXT("Bloody Mary self-destructed!"), false);
		ShowKillLine(TEXT("Dynamite Dick blew up Deputy!"), false);
		ShowKillLine(TEXT("You killed Bloodeye Tim!"), true);
		ShowKillLine(TEXT("Jack Sanchez killed you!"), true);
		ShowMessage(TEXT("You found a secret area!"), 6.f);       // (a notice under it: the two blocks apart)
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT kf shot: %s profile, %s, the feed filled"), Tag, GetWorld()->FeatureLevel <= ERHIFeatureLevel::ES3_1 ? TEXT("ES3_1") : TEXT("SM5"));
		At = ShotTime;
		ShotStep = 1;
		return;
	}
	const float In = ShotTime - At;
	if (ShotStep == 1 && In > 1.0f)
	{
		int32 Shown = 0;
		for (const UTextRenderComponent* T : KillFeedText) if (T && T->IsVisible()) ++Shown;
		Check(Shown == 4 && KillLines.Num() == 4 && KillLines[0].Text == TEXT("Jack Sanchez killed you!"), FString::Printf(TEXT("four lines shown, the newest at the bottom (%d shown, bottom \"%s\")"),
			Shown, KillLines.Num() > 0 ? *KillLines[0].Text : TEXT("")));
		const double TopDeg = FMath::RadiansToDegrees(FMath::Atan((KillFeedDist * FMath::Tan(FMath::DegreesToRadians(KillFeedUpDeg)) + 3 * KillFeedStep * KillFeedDist / KillFeedFull + KillFeedSize) / KillFeedDist));
		Check(KillFeedUpDeg > 15.0, FString::Printf(TEXT("the feed clear of the aim: %.1f to %.1f degrees above the middle of the view, at %.0f cm"), KillFeedUpDeg, TopDeg, KillFeedDist));
		Shoot(TEXT("full"));
		ShotStep = 2;
	}
	else if (ShotStep == 2 && In > 5.6f)
	{
		Shoot(TEXT("fading"));
		ShotStep = 3;
	}
	else if (ShotStep == 3 && In > 8.f)
	{
		int32 Shown = 0;
		for (const UTextRenderComponent* T : KillFeedText) if (T && T->IsVisible()) ++Shown;
		Check(Shown == 0 && KillLines.Num() == 0, FString::Printf(TEXT("the lines gone after %.0f s (%d shown)"), KillFeedSeconds, Shown));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT kf shot RESULT %s (%d failed)"), Fails == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Fails);
		ShotStep = 4;
		FGenericPlatformMisc::RequestExit(false);
	}
}

OL_SHOT_MATCH(OLKillFeedTest) { return (OLFlags::ValueIs(TEXT("OLKillFeedTest"), TEXT("shot")) || OLFlags::ValueIs(TEXT("OLKillFeedTest"), TEXT("shotq3"))); }
OL_SHOT_TEST(OLKillFeedTest, 160)
{
	KillFeedShotTick(Dt);            // (REQ-KILLFEED: OutlawsPlayerKillFeed.cpp)
}
#endif // WITH_OUTLAWS_TESTS
