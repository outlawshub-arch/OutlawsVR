// REQ-TUTORIAL2 (PLANS.md, user 14 Oct): the tutorial's card.  The join bubble's look (OutlawsBeacon.cpp: the DialogBox frame
// over the wood, Montagu Slab) and its way of pressing (a hand's laser on a button, the trigger), with Back and Next.  It
// stands where the notice lines stood: its bottom edge 10 degrees over the middle of the view, 80 cm out (nearer in front of
// a wall, as the notices are), so it never covers a shot; the lines go over it while it is up and the card's controller
// glyph under it.  Hidden under the menu and the death view.

#include "OutlawsTutorialCard.h"
#include "OutlawsPerf.h"
#include "OutlawsPlayer.h"
#include "Camera/CameraComponent.h"
#include "CanvasItem.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Canvas.h"
#include "Engine/CanvasRenderTarget2D.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MotionControllerComponent.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	constexpr int32 CardPxW = 1024, CardPxH = 380;
	constexpr float CardW = 46.f;                 // cm across at 80 cm (its height by the canvas's shape)
	constexpr float CardFull = 80.f;              // cm out
	constexpr float CardBottomDeg = 10.f;         // its bottom edge over the middle of the view
	TWeakObjectPtr<AOutlawsTutorialCard> GCard;
}

AOutlawsTutorialCard::AOutlawsTutorialCard()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PostUpdateWork;
	Plane = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Plane"));
	RootComponent = Plane;
	Plane->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Plane->SetCastShadow(false);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> PlaneMesh(TEXT("/Engine/BasicShapes/Plane.Plane"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (PlaneMesh.Succeeded()) Plane->SetStaticMesh(PlaneMesh.Object);
	for (int32 H = 0; H < 2; ++H)
	{
		Laser[H] = CreateDefaultSubobject<UStaticMeshComponent>(H == 0 ? TEXT("LaserL") : TEXT("LaserR"));
		Laser[H]->SetupAttachment(RootComponent);
		Laser[H]->SetUsingAbsoluteLocation(true);
		Laser[H]->SetUsingAbsoluteRotation(true);
		Laser[H]->SetUsingAbsoluteScale(true);
		Laser[H]->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Laser[H]->SetCastShadow(false);
		if (CubeMesh.Succeeded()) Laser[H]->SetStaticMesh(CubeMesh.Object);
		Laser[H]->SetVisibility(false);
	}
	SetActorHiddenInGame(true);
}

bool AOutlawsTutorialCard::HandOnCard(int32 Hand)
{
	const AOutlawsTutorialCard* C = GCard.Get();
	return C && C->IsShown() && C->Hover[Hand & 1] >= 0;
}

void AOutlawsTutorialCard::SetCard(int32 InStep, int32 InSteps, const FString& InLine, const FString& InHint, bool bInGood)
{
	GCard = this;
	Step = InStep;
	Steps = InSteps;
	Line = InLine;
	Hint = InHint;
	bGood = bInGood;
	bUp = true;
	Picked = -1;
	Hover[0] = Hover[1] = -1;
	bArmed[0] = bArmed[1] = false;           // (a trigger already held does not press at once)
	Redraw();
}

void AOutlawsTutorialCard::SetGood(bool bInGood)
{
	if (bGood == bInGood) return;
	bGood = bInGood;
	Redraw();
}

void AOutlawsTutorialCard::Hide()
{
	bUp = false;
	SetActorHiddenInGame(true);
	for (int32 H = 0; H < 2; ++H) Laser[H]->SetVisibility(false);
	if (AOutlawsPlayer* P = Cast<AOutlawsPlayer>(UGameplayStatics::GetPlayerPawn(this, 0))) P->bTutCardUp = false;
}

void AOutlawsTutorialCard::Pick(int32 Button)
{
	if (!bUp) return;
	if (Button == 0 && Step <= 1) return;     // (no Back on the first card)
	Picked = Button;
}

void AOutlawsTutorialCard::Redraw()
{
	AOutlawsPlayer* P = Cast<AOutlawsPlayer>(UGameplayStatics::GetPlayerPawn(this, 0));
	if (!Font && P)
	{
		Box = P->KpfArt(TEXT("DialogBox@2x"));
		Wood = P->KpfArt(TEXT("DialogBackground@2x"));
		Font = P->KpfFont(TEXT("fonts/MontaguSlab_24pt-Bold.ttf"), 30);
		SmallFont = P->KpfFont(TEXT("fonts/MontaguSlab_24pt-Bold.ttf"), 24);
		for (UObject* O : { (UObject*)Box, (UObject*)Wood, (UObject*)Font, (UObject*)SmallFont }) if (O) Kept.Add(O);
		if (P->LaserMaterial()) for (int32 H = 0; H < 2; ++H) Laser[H]->SetMaterial(0, P->LaserMaterial());
	}
	if (!Font && GEngine) Font = SmallFont = GEngine->GetMediumFont();
	if (!Canvas)
	{
		Canvas = NewObject<UCanvasRenderTarget2D>(this);
		Canvas->RenderTargetFormat = RTF_RGBA8_SRGB;
		Canvas->TargetGamma = 1.f;
		Canvas->ClearColor = FLinearColor::Black;
		Canvas->InitAutoFormat(CardPxW, CardPxH);
		Canvas->OnCanvasRenderTargetUpdate.AddDynamic(this, &AOutlawsTutorialCard::DrawCanvas);
		// (the bubble's material: the wheel's overlay, over the world)
		UMaterialInterface* Base = P ? P->LoadOverlayMaterial(TEXT("M_OLR_UIOverlay")) : nullptr;
		if (!Base) Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_MovieScreen.M_OLR_MovieScreen"));
		if (Base)
		{
			Plane->SetTranslucentSortPriority(20);
			Mid = UMaterialInstanceDynamic::Create(Base, this);
			Mid->SetTextureParameterValue(TEXT("Tex"), Canvas);
			Plane->SetMaterial(0, Mid);
		}
	}
	Canvas->UpdateResource();
}

void AOutlawsTutorialCard::DrawCanvas(UCanvas* C, int32 Width, int32 Height)
{
	const float W = float(Width), H = float(Height);
	auto Col = [](uint8 R, uint8 G, uint8 B) { return FLinearColor::FromSRGBColor(FColor(R, G, B)); };
	auto Fill = [C](float X, float Y, float Wd, float Ht, const FLinearColor& Colour)
	{
		FCanvasTileItem T(FVector2D(X, Y), FVector2D(Wd, Ht), Colour);
		T.BlendMode = SE_BLEND_Opaque;
		C->DrawItem(T);
	};
	auto Width2 = [C](UFont* F, const FString& Str) { float XL = 0.f, YL = 0.f; if (F) C->TextSize(F, Str, XL, YL); return XL; };
	// (Align: 0 centred on X, -1 from X, 1 ending at X)
	auto Txt = [C, &Width2](UFont* F, const FString& Str, float X, float Y, const FLinearColor& Colour, float MaxW, int32 Align = 0)
	{
		if (!F || Str.IsEmpty()) return;
		const float XL = Width2(F, Str);
		const float Sc = XL > MaxW ? MaxW / XL : 1.f;
		const float X0 = Align == 0 ? X - XL * Sc * 0.5f : Align < 0 ? X : X - XL * Sc;
		FCanvasTextItem T(FVector2D(X0, Y), FText::FromString(Str), F, Colour);
		T.Scale = FVector2D(Sc, Sc);
		T.bCentreY = true;
		T.BlendMode = SE_BLEND_Translucent;
		T.EnableShadow(FLinearColor::Black, FVector2D(3.f, 2.f));
		C->DrawItem(T);
	};
	// the bubble's wood and frame
	constexpr float Pad = 22.f, Corner = 64.f;
	if (Wood && Wood->GetResource())
	{
		const float TW = Wood->GetSizeX() * 0.6f, TH = Wood->GetSizeY() * 0.6f;
		for (float ty = Pad; ty < H - Pad; ty += TH)
		{
			for (float tx = Pad; tx < W - Pad; tx += TW)
			{
				const float CW = FMath::Min(TW, W - Pad - tx), CH = FMath::Min(TH, H - Pad - ty);
				FCanvasTileItem I(FVector2D(tx, ty), Wood->GetResource(), FVector2D(CW, CH), FVector2D(0.f, 0.f), FVector2D(CW / TW, CH / TH), FLinearColor::White);
				I.BlendMode = SE_BLEND_Opaque;
				C->DrawItem(I);
			}
		}
	}
	else
	{
		Fill(Pad, Pad, W - 2.f * Pad, H - 2.f * Pad, Col(0x2a, 0x1a, 0x0e));
	}
	if (Box && Box->GetResource())
	{
		const float S = float(Box->GetSizeX()), U0 = 116.f / S, U1 = (S - 116.f) / S;
		const float Xs[4] = { 0.f, Corner, W - Corner, W }, Ys[4] = { 0.f, Corner, H - Corner, H }, Us[4] = { 0.f, U0, U1, 1.f };
		for (int32 j = 0; j < 3; ++j)
		{
			for (int32 i = 0; i < 3; ++i)
			{
				if (i == 1 && j == 1) continue;
				FCanvasTileItem I(FVector2D(Xs[i], Ys[j]), Box->GetResource(), FVector2D(Xs[i + 1] - Xs[i], Ys[j + 1] - Ys[j]), FVector2D(Us[i], Us[j]), FVector2D(Us[i + 1], Us[j + 1]), FLinearColor::White);
				I.BlendMode = SE_BLEND_Translucent;
				C->DrawItem(I);
			}
		}
	}
	const FLinearColor Gold = Col(0xd1, 0xaf, 0x27), Hot = Col(0xff, 0xef, 0x65), Pale = Col(0xf0, 0xe6, 0xd0), Dim = Col(0x8a, 0x7c, 0x62);
	const FLinearColor Tick = Col(0x9c, 0xd8, 0x5a);
	// "Step N of M" at the top left; the tick and "Good." at the top right once its action is seen
	Txt(SmallFont, FString::Printf(TEXT("Step %d of %d"), Step, Steps), 76.f, 62.f, Gold, 300.f, -1);
	if (bGood)
	{
		const float Gx = W - 76.f;
		const float GoodW = Width2(Font, TEXT("Good."));
		Txt(Font, TEXT("Good."), Gx, 62.f, Tick, 200.f, 1);
		const float Tx = Gx - GoodW - 52.f, Ty = 62.f;
		for (const float Th : { 7.f })
		{
			FCanvasLineItem A(FVector2D(Tx, Ty), FVector2D(Tx + 14.f, Ty + 15.f));
			A.LineThickness = Th;
			A.SetColor(Tick);
			C->DrawItem(A);
			FCanvasLineItem B(FVector2D(Tx + 14.f, Ty + 15.f), FVector2D(Tx + 40.f, Ty - 18.f));
			B.LineThickness = Th;
			B.SetColor(Tick);
			C->DrawItem(B);
		}
	}
	// the line, wrapped at the word nearest its middle when it is too wide; the second line smaller under it
	const float MaxW = W - 150.f;
	FString A = Line, B;
	if (Width2(Font, Line) > MaxW)
	{
		int32 Best = INDEX_NONE;
		for (int32 i = 0; i < Line.Len(); ++i)
		{
			if (Line[i] == TEXT(' ') && (Best == INDEX_NONE || FMath::Abs(i - Line.Len() / 2) < FMath::Abs(Best - Line.Len() / 2))) Best = i;
		}
		if (Best != INDEX_NONE) { A = Line.Left(Best); B = Line.Mid(Best + 1); }
	}
	const bool bHint = !Hint.IsEmpty();
	const float Mid0 = bHint ? 140.f : 158.f;
	if (B.IsEmpty()) Txt(Font, A, W * 0.5f, Mid0 + 18.f, Pale, MaxW);
	else { Txt(Font, A, W * 0.5f, Mid0, Pale, MaxW); Txt(Font, B, W * 0.5f, Mid0 + 40.f, Pale, MaxW); }
	if (bHint) Txt(SmallFont, Hint, W * 0.5f, 226.f, Gold, MaxW);
	// Back on the left (not on the first card), Next on the right: dim till the action is seen, lit then (it always works)
	Buttons[0] = Buttons[1] = FBox2D(ForceInit);
	const float By = 262.f, Bh = 78.f, Bw = 250.f;
	for (int32 b = 0; b < 2; ++b)
	{
		if (b == 0 && Step <= 1) continue;
		const float X = b == 0 ? 70.f : W - 70.f - Bw;
		const bool bHot = Hover[0] == b || Hover[1] == b;
		const bool bLit = b == 0 || bGood;
		const FLinearColor Edge = bHot ? Hot : bLit ? Gold : Dim;
		Fill(X - 3.f, By - 3.f, Bw + 6.f, Bh + 6.f, Edge);
		Fill(X, By, Bw, Bh, b == 1 && bGood ? Col(0x2a, 0x1c, 0x08) : Col(0x14, 0x0c, 0x06));
		const FString Label = b == 0 ? FString(TEXT("Back")) : Step >= Steps ? FString(TEXT("Finish")) : FString(TEXT("Next"));
		Txt(Font, Label, X + Bw * 0.5f, By + Bh * 0.5f, bHot ? Hot : bLit ? Gold : Dim, Bw - 24.f);
		Buttons[b] = FBox2D(FVector2D(X, By), FVector2D(X + Bw, By + Bh));
	}
}

void AOutlawsTutorialCard::Place(float Dt)
{
	AOutlawsPlayer* P = Cast<AOutlawsPlayer>(UGameplayStatics::GetPlayerPawn(this, 0));
	const UCameraComponent* Cam = P ? P->FindComponentByClass<UCameraComponent>() : nullptr;
	if (!Cam) return;
	const FTransform CamX = Cam->GetComponentTransform();
	const float Hcm = CardW * CardPxH / CardPxW;
	const double TanB = FMath::Tan(FMath::DegreesToRadians(double(CardBottomDeg)));
	// (as the notices: in front of a nearer wall along the line to its middle, scaled so it reads the same size)
	const double MidZ = CardFull * TanB + Hcm * 0.5;
	const FVector Eye = CamX.GetLocation();
	const FVector Way = CamX.TransformVectorNoScale(FVector(1.0, 0.0, MidZ / CardFull)).GetSafeNormal();
	const double Along = CardFull * FMath::Sqrt(1.0 + FMath::Square(MidZ / CardFull));
	double Want = CardFull;
	FHitResult Hit;
	FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsTutCardClear), false, P);
	FCollisionResponseParams RP;
	RP.CollisionResponse.SetResponse(ECC_Pawn, ECR_Ignore);       // (the world only, not a bad guy before the eyes)
	if (GetWorld() && GetWorld()->LineTraceSingleByChannel(Hit, Eye, Eye + Way * (Along + 10.0), ECC_Visibility, Q, RP))
	{
		Want = FMath::Clamp((Hit.Distance - 5.0) * CardFull / Along, 25.0, double(CardFull));
	}
	Dist = Want < Dist ? float(Want) : FMath::FInterpTo(Dist, float(Want), Dt, 4.f);
	const float K = Dist / CardFull;
	const FVector Local(Dist, 0.0, (CardFull * TanB + Hcm * 0.5) * K);
	// (the plane's face is its +Z, turned to the eye: X to the card's right, Y down)
	const FVector Centre = CamX.TransformPosition(Local);
	const FVector Right = CamX.TransformVectorNoScale(FVector(0.0, 1.0, 0.0));
	const FVector ToEye = (Eye - Centre).GetSafeNormal();
	const FVector Down = FVector::CrossProduct(ToEye, Right).GetSafeNormal();
	SetActorLocationAndRotation(Centre, FRotationMatrix::MakeFromXY(Right, Down).Rotator());
	Plane->SetWorldScale3D(FVector(CardW * K / 100.f, Hcm * K / 100.f, 1.f));
	P->TutCardDist = Dist;
	P->TutCardBottomZ = float(CardFull * TanB * K);
	P->TutCardTopZ = float((CardFull * TanB + Hcm) * K);
}

void AOutlawsTutorialCard::Tick(float DeltaSeconds)
{
	OL_PERF_SCOPE("Other ours");
	Super::Tick(DeltaSeconds);
	AOutlawsPlayer* P = Cast<AOutlawsPlayer>(UGameplayStatics::GetPlayerPawn(this, 0));
	if (!P) return;
	const bool bShow = bUp && !P->bMenuOpen && !P->bDeadView;
	if (IsHidden() == bShow) SetActorHiddenInGame(!bShow);
	P->bTutCardUp = bShow;
	if (!bShow)
	{
		for (int32 H = 0; H < 2; ++H) Laser[H]->SetVisibility(false);
		return;
	}
	Place(DeltaSeconds);
	if (!P->bVR) return;
	// each hand's laser onto the card: the button it is on, the trigger presses it (as the bubble's)
	const FTransform T = GetActorTransform();
	const FVector N = T.GetUnitAxis(EAxis::Z);
	bool bChanged = false;
	for (int32 H = 0; H < 2; ++H)
	{
		int32 On = -1;
		float Len = 0.f;
		if (const UMotionControllerComponent* Aim = P->AimOf(H))
		{
			const FVector O = Aim->GetComponentLocation(), D = Aim->GetForwardVector();
			const double Den = FVector::DotProduct(D, N);
			if (FMath::Abs(Den) > 1e-3)
			{
				const double Dd = FVector::DotProduct(T.GetLocation() - O, N) / Den;
				if (Dd > 0.0 && Dd < 200.0)
				{
					// the plane is 100 x 100 before its scale: local X, Y in -50..50
					const FVector L = T.InverseTransformPosition(O + D * Dd);
					const FVector2D Px((L.X / 100.0 + 0.5) * CardPxW, (L.Y / 100.0 + 0.5) * CardPxH);
					for (int32 b = 0; b < 2; ++b) if (Buttons[b].bIsValid && Buttons[b].IsInside(Px)) On = b;
					if (Px.X >= 0.0 && Px.X <= CardPxW && Px.Y >= 0.0 && Px.Y <= CardPxH) Len = float(Dd);
				}
			}
			Laser[H]->SetVisibility(Len > 0.f);
			if (Len > 0.f)
			{
				Laser[H]->SetWorldLocationAndRotation(O + D * (Len * 0.5f), D.Rotation());
				Laser[H]->SetWorldScale3D(FVector(Len / 100.0, 0.003, 0.003));
			}
		}
		if (On != Hover[H]) { Hover[H] = On; bChanged = true; }
		const float Trig = P->Axis(H == 0 ? TEXT("OLVR_LeftTrigger") : TEXT("OLVR_RightTrigger"));
		if (Trig < 0.3f) bArmed[H] = true;
		else if (Trig > 0.7f && bArmed[H])
		{
			bArmed[H] = false;
			if (On >= 0) { Pick(On); P->Pulse(H, 0.1f); }
		}
	}
	if (bChanged) Redraw();
}
