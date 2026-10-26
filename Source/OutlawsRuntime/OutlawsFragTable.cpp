// FT, the Frag Table (user 2026-10-03): the remaster's scoreboard (fragtable_full.rml with fragtable.rcss) on a canvas, with
// more columns than its kills, and without fragtable_final's town picture.
// - Layout as fragtable.rcss at 1.2 canvas px a dp, the boxes as wide as the canvas: a full_box is the DialogBox ninepatch
//   (corners 116 px at 2x) over the DialogBackground wood (repeated at its native size, inside the box's padding), its
//   full_box_inner padding 32 x 24 dp; the title and the column headers gold #d1af27 bold with a 4 x 3 dp black shadow; the
//   cells in the body font (rml.rcss: Atkinson Hyperlegible Next), team names red / blue, numbers right-aligned; rows 40 dp.
// - The header box (the title, a rule, the header rows) over the players box; one box when there are no header rows.
// - Measure gives the canvas height the rows need; Draw returns where it ended, so the two can be checked against each other.

#include "OutlawsFragTable.h"
#include "OutlawsFlags.h"
#include "OutlawsPerf.h"
#include "OutlawsRuntime.h"
#include "OutlawsCoop.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsStats.h"
#include "Camera/CameraComponent.h"
#include "CanvasItem.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Canvas.h"
#include "Engine/CanvasRenderTarget2D.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "ImageUtils.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/Archive.h"
#include "TextureResource.h"
#include "UnrealClient.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	// fragtable.rcss in canvas px (1.2 a dp)
	constexpr float Dp = 1.2f;
	constexpr float PadL = 24.f * Dp, PadT = 15.f * Dp, PadR = 26.f * Dp, PadB = 16.f * Dp;   // full_box: the frame round the wood
	constexpr float BoxInX = 32.f * Dp, BoxInY = 24.f * Dp;                                     // full_box_inner
	constexpr float Gap = 12.f * Dp;                                                            // full_box + full_box
	constexpr float Corner = 58.f * Dp;                                                         // the ninepatch's 116 px at 2x
	constexpr float RowH = 42.f * Dp;                                                           // a player's row (40 dp, a touch more)
	constexpr float HeadH = 42.f * Dp;                                                          // the column headers
	constexpr float HdrRowH = 36.f * Dp;                                                        // a header box line
	constexpr float HdrTableW = 500.f * Dp;                                                     // full_box.header full_table
	constexpr float TitleH = 50.f * Dp;
	constexpr float RuleW = 400.f * Dp, RuleH = 16.f * Dp, RuleGap = 8.f * Dp;                 // hr.small, 8 dp above and below
	constexpr float CellPad = 6.f * Dp;

	FLinearColor FtCol(uint8 R, uint8 G, uint8 B) { return FLinearColor::FromSRGBColor(FColor(R, G, B)); }
	const FLinearColor& FtGold() { static const FLinearColor C = FtCol(0xd1, 0xaf, 0x27); return C; }
	const FLinearColor& FtHot() { static const FLinearColor C = FtCol(0xff, 0xef, 0x65); return C; }
	const FLinearColor& FtRed() { static const FLinearColor C = FtCol(224, 108, 117); return C; }
	const FLinearColor& FtBlue() { static const FLinearColor C = FtCol(97, 175, 239); return C; }
	FLinearColor FtTeam(int32 Team) { return Team == 1 ? FtRed() : Team == 2 ? FtBlue() : FLinearColor::White; }

	float HeaderBoxH(const FFragTableData& D)
	{
		return PadT + BoxInY + TitleH + RuleGap + RuleH + RuleGap + HdrRowH * D.HeaderRows.Num() + BoxInY + PadB;
	}

	float PlayersBoxH(const FFragTableData& D)
	{
		const float Title = D.HeaderRows.Num() > 0 ? 0.f : TitleH + RuleGap + RuleH + RuleGap;
		return PadT + BoxInY + Title + HeadH + RowH * FMath::Max(1, D.Rows.Num()) + BoxInY + PadB;
	}
}

// ---- the data ----------------------------------------------------------------------------------------------------------

void FFragTableData::SortRows()
{
	Rows.StableSort([](const FFragTableRow& A, const FFragTableRow& B)
	{
		if (A.SortA != B.SortA) return A.SortA > B.SortA;
		if (A.SortB != B.SortB) return A.SortB < B.SortB;
		return A.Name.Compare(B.Name, ESearchCase::IgnoreCase) < 0;
	});
}

void FFragTableData::SetLevelEndColumns(bool bCoop)
{
	Columns.Reset();
	auto Col = [this](const TCHAR* H, float W) { FFragTableColumn& C = Columns.AddDefaulted_GetRef(); C.Header = H; C.Width = W; };
	Col(TEXT("NAME"), 0.f);
	Col(TEXT("KILLS"), 130.f);
	Col(TEXT("DEATHS"), 150.f);
	Col(TEXT("SHOTS"), 140.f);
	Col(TEXT("ACC"), 120.f);
	Col(TEXT("LONGEST"), 170.f);
	Col(TEXT("TNT"), 110.f);
	Col(TEXT("THROWN"), 160.f);
	if (bCoop) Col(TEXT("REVIVED"), 170.f);
	Col(TEXT("TIME"), 140.f);
}

void FFragTableData::SetMatchColumns(bool bPing)
{
	Columns.Reset();
	auto Col = [this](const TCHAR* H, float W) { FFragTableColumn& C = Columns.AddDefaulted_GetRef(); C.Header = H; C.Width = W; };
	Col(TEXT("NAME"), 0.f);
	Col(TEXT("KILLS"), 170.f);
	Col(TEXT("DEATHS"), 170.f);
	Col(TEXT("SHOTS"), 170.f);
	Col(TEXT("ACC"), 150.f);
	Col(TEXT("LONGEST"), 190.f);
	if (bPing) Col(TEXT("PING"), 150.f);
}

FString FFragTableData::Describe() const
{
	FString S = FString::Printf(TEXT("[%s]"), *Title);
	for (const FFragTableHeaderRow& H : HeaderRows) S += FString::Printf(TEXT(" %s %s;"), *H.Label, *H.Value);
	for (const FFragTableRow& R : Rows) S += FString::Printf(TEXT(" | %s%s%s %s"), *R.Name, R.Team ? *FString::Printf(TEXT(" (team %d)"), R.Team) : TEXT(""), R.bLocal ? TEXT(" (local)") : TEXT(""), *FString::Join(R.Values, TEXT(" ")));
	return S;
}

// ---- the art -----------------------------------------------------------------------------------------------------------

bool FFragTableArt::Load(AOutlawsPlayer* Player)
{
	if (!Player) return false;
	Box = Player->KpfArt(TEXT("DialogBox@2x"));
	Wood = Player->KpfArt(TEXT("DialogBackground@2x"));
	Rule = Player->KpfArt(TEXT("Embellish2@2x"));
	// sizes are points (x 4/3 px): the title 54 px in Montagu Slab bold (the menus' titles), the headers 32 px and the cells
	// 34 px in the body font (fragtable.rcss's full_header is the body font, bold: the display face's H reads as an A in DEATHS)
	Title = Player->KpfFont(TEXT("fonts/MontaguSlab_24pt-Bold.ttf"), 40);
	Header = Player->KpfFont(TEXT("fonts/AtkinsonHyperlegibleNext-VariableFont_wght.ttf"), 24);
	if (!Header) Header = Player->KpfFont(TEXT("fonts/MontaguSlab_24pt-Bold.ttf"), 24);
	Cell = Player->KpfFont(TEXT("fonts/AtkinsonHyperlegibleNext-VariableFont_wght.ttf"), 26);
	if (!Cell) Cell = Player->KpfFont(TEXT("fonts/MontaguSlab_24pt-Bold.ttf"), 25);
	if (GEngine)
	{
		if (!Title) Title = GEngine->GetLargeFont();
		if (!Header) Header = GEngine->GetMediumFont();
		if (!Cell) Cell = GEngine->GetMediumFont();
	}
	UE_LOG(LogOutlaws, Log, TEXT("OLFRAG art: box %s, wood %s, rule %s"), Box ? TEXT("yes") : TEXT("NO"), Wood ? TEXT("yes") : TEXT("NO"), Rule ? TEXT("yes") : TEXT("NO"));
	return Cell != nullptr;
}

// ---- the drawing -------------------------------------------------------------------------------------------------------

int32 FOutlawsFragTable::Measure(const FFragTableData& Data)
{
	const float H = (Data.HeaderRows.Num() > 0 ? HeaderBoxH(Data) + Gap : 0.f) + PlayersBoxH(Data);
	return FMath::CeilToInt(H);
}

float FOutlawsFragTable::Draw(UCanvas* C, const FFragTableData& Data, const FFragTableArt& Art, int32 Width, int32 Height)
{
	if (!C) return 0.f;
	const float W = float(Width);
	auto Fill = [C](float X, float Y, float Wd, float Ht, const FLinearColor& Colour, bool bBlend)
	{
		FCanvasTileItem T(FVector2D(X, Y), FVector2D(Wd, Ht), Colour);
		T.BlendMode = bBlend ? SE_BLEND_Translucent : SE_BLEND_Opaque;
		C->DrawItem(T);
	};
	auto Tile = [C](UTexture2D* T, float X, float Y, float Wd, float Ht, FVector2D UV0, FVector2D UV1)
	{
		if (!T || !T->GetResource() || Wd <= 0.f || Ht <= 0.f) return;
		FCanvasTileItem I(FVector2D(X, Y), T->GetResource(), FVector2D(Wd, Ht), UV0, UV1, FLinearColor::White);
		I.BlendMode = SE_BLEND_Translucent;
		C->DrawItem(I);
	};
	// Align: 0 from X, 1 centred on X, 2 ending at X; Y the line's middle; shrunk to MaxW (as the cards' long names)
	auto Txt = [C](UFont* F, const FString& Str, float X, float Y, int32 Align, const FLinearColor& Colour, float MaxW, float ShadowPx, bool bBold = false)
	{
		if (!F || Str.IsEmpty()) return;
		float XL = 0.f, YL = 0.f;
		C->TextSize(F, Str, XL, YL);
		const float Sc = MaxW > 0.f && XL > MaxW ? MaxW / XL : 1.f;
		float X0 = X;
		if (Align == 1) X0 -= XL * Sc * 0.5f;
		else if (Align == 2) X0 -= XL * Sc;
		FCanvasTextItem T(FVector2D(X0, Y), FText::FromString(Str), F, Colour);
		T.Scale = FVector2D(Sc, Sc);
		T.bCentreY = true;
		T.BlendMode = SE_BLEND_Translucent;
		if (ShadowPx > 0.f) T.EnableShadow(FLinearColor::Black, FVector2D(ShadowPx, ShadowPx * 0.75f));
		C->DrawItem(T);
		if (bBold)
		{
			// (the body font has one weight here: bold as a second pass a pixel over)
			FCanvasTextItem B(FVector2D(X0 + 1.2f, Y), FText::FromString(Str), F, Colour);
			B.Scale = T.Scale;
			B.bCentreY = true;
			B.BlendMode = SE_BLEND_Translucent;
			C->DrawItem(B);
		}
	};
	// a full_box: the wood inside the padding, the frame's nine pieces over it (the corners whole, the edges stretched)
	auto FullBox = [&](float Y, float Ht)
	{
		const float WX = PadL, WY = Y + PadT, WW = W - PadL - PadR, WH = Ht - PadT - PadB;
		if (Art.Wood && Art.Wood->GetResource())
		{
			const float TW = Art.Wood->GetSizeX() * 0.5f * Dp, TH = Art.Wood->GetSizeY() * 0.5f * Dp;     // (its native size at 2x)
			for (float ty = 0.f; ty < WH; ty += TH)
			{
				for (float tx = 0.f; tx < WW; tx += TW)
				{
					const float CW = FMath::Min(TW, WW - tx), CH = FMath::Min(TH, WH - ty);
					FCanvasTileItem I(FVector2D(WX + tx, WY + ty), Art.Wood->GetResource(), FVector2D(CW, CH), FVector2D(0.f, 0.f), FVector2D(CW / TW, CH / TH), FLinearColor::White);
					I.BlendMode = SE_BLEND_Opaque;
					C->DrawItem(I);
				}
			}
		}
		else
		{
			Fill(WX, WY, WW, WH, FtCol(0x2a, 0x1a, 0x0e), false);
		}
		if (!Art.Box) return;
		const float S = float(Art.Box->GetSizeX());
		const float U0 = 116.f / S, U1 = (S - 116.f) / S;
		const float Xs[4] = { 0.f, Corner, W - Corner, W };
		const float Ys[4] = { Y, Y + Corner, Y + Ht - Corner, Y + Ht };
		const float Us[4] = { 0.f, U0, U1, 1.f };
		for (int32 j = 0; j < 3; ++j)
		{
			for (int32 i = 0; i < 3; ++i)
			{
				if (i == 1 && j == 1) continue;           // (the middle is empty: the wood shows)
				Tile(Art.Box, Xs[i], Ys[j], Xs[i + 1] - Xs[i], Ys[j + 1] - Ys[j], FVector2D(Us[i], Us[j]), FVector2D(Us[i + 1], Us[j + 1]));
			}
		}
	};
	// the title and hr.small under it; returns the y after them
	auto TitleAndRule = [&](float Y)
	{
		Txt(Art.Title, Data.Title, W * 0.5f, Y + TitleH * 0.5f, 1, FtGold(), W - 2.f * (PadL + BoxInX), 5.f);
		Y += TitleH + RuleGap;
		if (Art.Rule) Tile(Art.Rule, (W - RuleW) * 0.5f, Y, RuleW, RuleH, FVector2D(0.f, 0.f), FVector2D(1.f, 1.f));
		else Fill((W - RuleW) * 0.5f, Y + RuleH * 0.5f - 1.f, RuleW, 2.f, FtGold(), false);
		return Y + RuleH + RuleGap;
	};

	float Y = 0.f;
	if (Data.HeaderRows.Num() > 0)
	{
		const float Ht = HeaderBoxH(Data);
		FullBox(Y, Ht);
		float Ry = TitleAndRule(Y + PadT + BoxInY);
		const float L = (W - HdrTableW) * 0.5f, R = L + HdrTableW;
		for (const FFragTableHeaderRow& H : Data.HeaderRows)
		{
			Txt(Art.Cell, H.Label, L, Ry + HdrRowH * 0.5f, 0, FtTeam(H.Team), HdrTableW * 0.66f, 2.f);
			Txt(Art.Cell, H.Value, R, Ry + HdrRowH * 0.5f, 2, FtTeam(H.Team), HdrTableW * 0.32f, 2.f);
			Ry += HdrRowH;
		}
		Y += Ht + Gap;
	}

	// the players box: the column headers, then a row a player
	const float Ht = PlayersBoxH(Data);
	FullBox(Y, Ht);
	float Ry = Y + PadT + BoxInY;
	if (Data.HeaderRows.Num() == 0) Ry = TitleAndRule(Ry);
	const float TL = PadL + BoxInX, TR = W - PadR - BoxInX;
	float Fixed = 0.f;
	for (const FFragTableColumn& Col : Data.Columns) Fixed += Col.Width;
	const float NameW = FMath::Max(120.f, (TR - TL) - Fixed);
	TArray<float> Lefts, Rights;
	float X = TL;
	for (const FFragTableColumn& Col : Data.Columns)
	{
		const float Cw = Col.Width > 0.f ? Col.Width : NameW;
		Lefts.Add(X);
		Rights.Add(X + Cw);
		X += Cw;
	}
	for (int32 c = 0; c < Data.Columns.Num(); ++c)
	{
		const bool bName = Data.Columns[c].Width <= 0.f;
		const float Cw = Rights[c] - Lefts[c] - 2.f * CellPad;
		Txt(Art.Header, Data.Columns[c].Header, bName ? Lefts[c] + CellPad : Rights[c] - CellPad, Ry + HeadH * 0.5f, bName ? 0 : 2, FtGold(), Cw, 4.f, true);
	}
	Ry += HeadH;
	Fill(TL, Ry - 3.f, TR - TL, 2.f, FtGold() * FLinearColor(1.f, 1.f, 1.f, 0.6f), true);
	for (int32 r = 0; r < Data.Rows.Num(); ++r)
	{
		const FFragTableRow& Row = Data.Rows[r];
		// every other row on a faint dark band; the local player's in the hot row's yellow
		if (r % 2 == 1) Fill(TL, Ry, TR - TL, RowH, FLinearColor(0.f, 0.f, 0.f, 0.28f), true);
		const FLinearColor Ink = Row.bLocal ? FtHot() : Row.Team ? FtTeam(Row.Team) : FLinearColor::White;
		int32 v = 0;
		for (int32 c = 0; c < Data.Columns.Num(); ++c)
		{
			const bool bName = Data.Columns[c].Width <= 0.f;
			const float Cw = Rights[c] - Lefts[c] - 2.f * CellPad;
			const FString& Str = bName ? Row.Name : (Row.Values.IsValidIndex(v) ? Row.Values[v] : FString());
			if (!bName) ++v;
			const FLinearColor CellInk = bName ? (Row.Team && !Row.bLocal ? FtTeam(Row.Team) : Ink) : (Row.bLocal ? FtHot() : FLinearColor::White);
			Txt(Art.Cell, Str, bName ? Lefts[c] + CellPad : Rights[c] - CellPad, Ry + RowH * 0.5f, bName ? 0 : 2, CellInk, Cw, 2.f);
		}
		// (a local player on a team: a mark of the team's colour before the name)
		if (Row.bLocal && Row.Team) Fill(TL - 2.f, Ry + 8.f, 4.f, RowH - 16.f, FtTeam(Row.Team), false);
		Ry += RowH;
	}
	(void)Height;
	return Y + Ht;
}

// ---- a panel in the world ----------------------------------------------------------------------------------------------

FBoxSphereBounds UOutlawsOnTopPlaneComponent::CalcBounds(const FTransform& LocalToWorld) const
{
	FBoxSphereBounds B = Super::CalcBounds(LocalToWorld);
	B.BoxExtent = FVector(B.SphereRadius);
	return B;
}

AOutlawsFragTablePanel::AOutlawsFragTablePanel()
{
	PrimaryActorTick.bCanEverTick = true;
	Plane = CreateDefaultSubobject<UOutlawsOnTopPlaneComponent>(TEXT("Plane"));
	RootComponent = Plane;
	Plane->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Plane->SetCastShadow(false);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> PlaneMesh(TEXT("/Engine/BasicShapes/Plane.Plane"));
	if (PlaneMesh.Succeeded()) Plane->SetStaticMesh(PlaneMesh.Object);
}

void AOutlawsFragTablePanel::PlaceBefore(const FVector& Eye, const FVector& Forward, float Distance, float InWidthCm, float RaiseCm)
{
	WidthCm = InWidthCm;
	FVector F = FVector(Forward.X, Forward.Y, 0.0).GetSafeNormal();
	if (F.IsNearlyZero()) F = FVector::ForwardVector;
	const FVector Centre = Eye + F * Distance + FVector(0.0, 0.0, RaiseCm);
	// (the plane's face is its +Z, turned to the eye: X to the panel's right, Y down, as the picture's)
	const FVector ToEye = (Eye - Centre).GetSafeNormal();
	const FVector Right(-F.Y, F.X, 0.0);
	const FVector Down = FVector::CrossProduct(ToEye, Right).GetSafeNormal();
	SetActorLocationAndRotation(Centre, FRotationMatrix::MakeFromXY(Right, Down).Rotator());
	Fit();
	KeepOverWorld(Eye);
}

void AOutlawsFragTablePanel::PlaceFacing(const FVector& Centre, const FVector& Eye, const FVector& RightHint, float InWidthCm)
{
	WidthCm = InWidthCm;
	const FVector ToEye = (Eye - Centre).GetSafeNormal();
	FVector Right = RightHint - ToEye * FVector::DotProduct(RightHint, ToEye);
	Right = Right.GetSafeNormal();
	if (Right.IsNearlyZero()) Right = FVector::RightVector;
	const FVector Down = FVector::CrossProduct(ToEye, Right).GetSafeNormal();
	SetActorLocationAndRotation(Centre, FRotationMatrix::MakeFromXY(Right, Down).Rotator());
	Fit();
	KeepOverWorld(Eye);
}

void AOutlawsFragTablePanel::SetData(const FFragTableData& Data)
{
	if (Canvas && Data == Shown) return;
	Shown = Data;
	Redraw();
}

void AOutlawsFragTablePanel::Tick(float DeltaSeconds)
{
	OL_PERF_SCOPE("Other ours");
	Super::Tick(DeltaSeconds);
	if (!Source) return;
	LookIn -= DeltaSeconds;
	if (LookIn > 0.f) return;
	LookIn = 0.5f;
	FFragTableData Now;
	if (Source->FillFragTable(Now)) SetData(Now);
}

void AOutlawsFragTablePanel::Fit()
{
	const float HCm = CanvasH > 0 ? WidthCm * float(CanvasH) / float(FOutlawsFragTable::CanvasW) : WidthCm * 0.25f;
	Plane->SetRelativeScale3D(FVector(WidthCm / 100.f, HCm / 100.f, 1.f));
}

void AOutlawsFragTablePanel::Redraw()
{
	if (!Art.IsLoaded() && Art.Load(Cast<AOutlawsPlayer>(UGameplayStatics::GetPlayerPawn(this, 0))))
	{
		for (UObject* O : { (UObject*)Art.Box, (UObject*)Art.Wood, (UObject*)Art.Rule, (UObject*)Art.Title, (UObject*)Art.Header, (UObject*)Art.Cell }) if (O) ArtKept.Add(O);
	}
	const int32 H = FMath::Max(64, FOutlawsFragTable::Measure(Shown));
	if (!Canvas)
	{
		Canvas = NewObject<UCanvasRenderTarget2D>(this);
		Canvas->RenderTargetFormat = RTF_RGBA8_SRGB;
		Canvas->TargetGamma = 1.f;                 // the sRGB target encodes on write (as the menu's)
		Canvas->ClearColor = FLinearColor::Black;
		Canvas->InitAutoFormat(FOutlawsFragTable::CanvasW, H);
		Canvas->OnCanvasRenderTargetUpdate.AddDynamic(this, &AOutlawsFragTablePanel::DrawCanvas);
		UMaterialInterface* Base = nullptr;
		if (bOnTop)
		{
			// (BUG-SCOREBOARD) the wheel's overlay material, no depth test (the headset's "_Q" copy pulled to arm's length),
			// sorted over the other see-through things, as the join bubble is: no wall in front of it cuts it
			// (AOutlawsPlayer::LoadOverlayMaterial's choice: the headset's mobile renderer takes the "_Q" copy)
			// (BUG-SCOREOCCLUDE, Quest build 47: the 50 cm copy let a wall nearer than that cover the table) the headset takes
			// the wheel's own copy pulled to 18 cm, in front of any wall the head can reach and behind the hands (BUG-WHEELOCCLUDE)
			const UWorld* World = GetWorld();
			if (World && World->FeatureLevel <= ERHIFeatureLevel::ES3_1) Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_WheelOverlay_Q.M_OLR_WheelOverlay_Q"));
			if (!Base && World && World->FeatureLevel <= ERHIFeatureLevel::ES3_1) Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_UIOverlay_Q.M_OLR_UIOverlay_Q"));
			if (!Base) Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_UIOverlay.M_OLR_UIOverlay"));
			if (Base) Plane->SetTranslucentSortPriority(20);
		}
		if (!Base) Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_MovieScreen.M_OLR_MovieScreen"));
		if (!Base) Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_World.M_OLR_World"));
		if (Base)
		{
			Mid = UMaterialInstanceDynamic::Create(Base, this);
			Mid->SetTextureParameterValue(TEXT("Tex"), Canvas);
			Plane->SetMaterial(0, Mid);
		}
	}
	else if (H != CanvasH)
	{
		Canvas->ResizeTarget(FOutlawsFragTable::CanvasW, H);
	}
	CanvasH = H;
	Fit();
	Canvas->UpdateResource();
}

void AOutlawsFragTablePanel::KeepOverWorld(const FVector& Eye)
{
	// (BUG-SCOREOCCLUDE) as the wheel's parts (AOutlawsPlayer::KeepWheelOverWorld): a plane's own small bounds behind a near
	// wall or door let occlusion culling drop it whole, whatever its material; the bounds reach past the eyes instead
	if (!bOnTop || !Plane) return;
	Plane->UpdateBounds();
	const float Radius = Plane->Bounds.SphereRadius / FMath::Max(Plane->BoundsScale, 0.01f);
	if (Radius < 0.01f) return;
	const float Need = (float(FVector::Dist(Eye, Plane->Bounds.Origin)) + 60.f) / Radius;
	if (Plane->BoundsScale < Need) Plane->SetBoundsScale(FMath::Min(Need * 1.25f, 4000.f));
}

float AOutlawsFragTablePanel::GetBoundsScale() const
{
	return Plane ? Plane->BoundsScale : 1.f;
}

bool AOutlawsFragTablePanel::ReadWood(FColor& Out) const
{
	FTextureRenderTargetResource* Res = Canvas ? Canvas->GameThread_GetRenderTargetResource() : nullptr;
	if (!Res || CanvasH <= 0) return false;
	TArray<FColor> Px;
	if (!Res->ReadPixels(Px)) return false;
	const int32 W = Canvas->SizeX, H = Canvas->SizeY;
	if (Px.Num() != W * H) return false;
	// inside the first box's frame (the header box, or the players box alone): wood with a little text over it
	const float BoxH = Shown.HeaderRows.Num() > 0 ? HeaderBoxH(Shown) : PlayersBoxH(Shown);
	const int32 X0 = FMath::CeilToInt(Corner), X1 = W - FMath::CeilToInt(Corner);
	const int32 Y0 = FMath::CeilToInt(Corner), Y1 = FMath::Min(H, FMath::FloorToInt(BoxH - Corner));
	TArray<FColor> In;
	for (int32 y = Y0; y < Y1; y += 2)
	{
		for (int32 x = X0; x < X1; x += 2) In.Add(Px[y * W + x]);
	}
	if (In.Num() == 0) return false;
	In.Sort([](const FColor& A, const FColor& B) { return int32(A.R) + A.G + A.B < int32(B.R) + B.G + B.B; });
	Out = In[In.Num() / 2];
	return true;
}

FString AOutlawsFragTablePanel::DescribeMaterial() const
{
	const UMaterialInterface* M = Plane ? Plane->GetMaterial(0) : nullptr;
	const UMaterialInterface* Base = M ? const_cast<UMaterialInterface*>(M)->GetBaseMaterial() : nullptr;
	return FString::Printf(TEXT("%s (on top %s, sort priority %d)"), Base ? *Base->GetName() : TEXT("none"), bOnTop ? TEXT("yes") : TEXT("no"), Plane ? Plane->TranslucencySortPriority : 0);
}

void AOutlawsFragTablePanel::DrawCanvas(UCanvas* C, int32 Width, int32 Height)
{
	LastDrawnBottom = FOutlawsFragTable::Draw(C, Shown, Art, Width, Height);
}

// ---- -OLFragTableTest ----------------------------------------------------------------------------------------------------

bool UOutlawsFragTableTest::ShouldCreateSubsystem(UObject* Outer) const
{
	return OLFlags::Param(TEXT("OLFragTableTest"));
}

UWorld* UOutlawsFragTableTest::GetTickableGameObjectWorld() const
{
	const UGameInstance* GI = GetGameInstance();
	return GI ? GI->GetWorld() : nullptr;
}

TStatId UOutlawsFragTableTest::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UOutlawsFragTableTest, STATGROUP_Tickables);
}

FFragTableData UOutlawsFragTableTest::FakeTable(int32 Players)
{
	static const TCHAR* Names[] = { TEXT("Marshal Anderson"), TEXT("Dingus"), TEXT("Slim Sanchez"), TEXT("Mad Dog"), TEXT("Spittin' Jack"),
		TEXT("Gatling Jim"), TEXT("Bob Graham"), TEXT("Matt Morgan"), TEXT("Kid Calico"), TEXT("Doc Holliday"), TEXT("Big Bad Barbara Le Fou of New Orleans"),
		TEXT("Ugly Jim"), TEXT("Cactus Pete"), TEXT("Two-Gun Tess"), TEXT("Rattler"), TEXT("Ol' Clem") };
	FFragTableData D;
	FRandomStream R(1883 + Players);
	if (Players == 2)
	{
		// the level's end in co-op: the host's row then the client's
		D.Title = TEXT("Hideout");
		D.SetLevelEndColumns(true);
		D.HeaderRows.Add({ TEXT("Difficulty"), TEXT("Bad"), 0 });
		const int32 V[2][9] = { { 23, 2, 141, 38, 41, 3, 4, 1, 754 }, { 17, 4, 203, 22, 27, 0, 7, 2, 754 } };
		for (int32 i = 0; i < 2; ++i)
		{
			FFragTableRow& Row = D.Rows.AddDefaulted_GetRef();
			Row.Name = i == 0 ? TEXT("Marshal Anderson") : TEXT("Dingus");
			Row.bLocal = i == 0;
			Row.Values = { FString::FromInt(V[i][0]), FString::FromInt(V[i][1]), FString::FromInt(V[i][2]), FString::Printf(TEXT("%d%%"), V[i][3]),
				FString::Printf(TEXT("%d m"), V[i][4]), FString::FromInt(V[i][5]), FString::FromInt(V[i][6]), FString::FromInt(V[i][7]),
				FString::Printf(TEXT("%d:%02d"), V[i][8] / 60, V[i][8] % 60) };
		}
		return D;
	}
	const bool bTeams = Players == 4;
	D.Title = bTeams ? TEXT("Team Play") : TEXT("Death Match");
	D.SetMatchColumns(true);
	int32 TeamScore[3] = { 0, 0, 0 };
	for (int32 i = 0; i < Players; ++i)
	{
		FFragTableRow& Row = D.Rows.AddDefaulted_GetRef();
		Row.Name = Names[i % UE_ARRAY_COUNT(Names)];
		Row.Team = bTeams ? 1 + (i % 2) : 0;
		Row.bLocal = i == 2;
		const int32 Kills = R.RandRange(0, 25), Deaths = R.RandRange(0, 15), Shots = Kills * 4 + R.RandRange(5, 60);
		const int32 Acc = FMath::RoundToInt(100.f * float(Kills * 2) / float(Shots));
		Row.Values = { FString::FromInt(Kills), FString::FromInt(Deaths), FString::FromInt(Shots), FString::Printf(TEXT("%d%%"), Acc),
			Kills > 0 ? FString::Printf(TEXT("%d m"), R.RandRange(5, 60)) : FString(TEXT("-")), FString::FromInt(R.RandRange(18, 140)) };
		Row.SortA = Kills;
		Row.SortB = Deaths;
		TeamScore[Row.Team] += Kills;
	}
	if (bTeams)
	{
		D.HeaderRows.Add({ TEXT("Red Team"), FString::FromInt(TeamScore[1]), 1 });
		D.HeaderRows.Add({ TEXT("Blue Team"), FString::FromInt(TeamScore[2]), 2 });
	}
	D.HeaderRows.Add({ TEXT("Time Limit"), TEXT("7:42"), 0 });
	D.HeaderRows.Add({ TEXT("Kill Limit"), TEXT("25"), 0 });
	D.SortRows();
	return D;
}

void UOutlawsFragTableTest::Tick(float DeltaTime)
{
	UWorld* W = GetTickableGameObjectWorld();
	if (!W) return;
	const double Now = FPlatformTime::Seconds();
	AOutlawsPlayer* P = Cast<AOutlawsPlayer>(UGameplayStatics::GetPlayerPawn(W, 0));
	TActorIterator<AOutlawsLevelActor> It(W);
	AOutlawsLevelActor* L = It ? *It : nullptr;
	if (!P || !L) return;
	// the tables in turn: the measurer's checks at 1 and 16 rows, the pictures at 2, 4, 8 (and 16)
	static const int32 Sizes[] = { 1, 2, 4, 8, 16 };
	constexpr int32 NumSizes = UE_ARRAY_COUNT(Sizes);
	if (Step == 0)
	{
		if (L->IsRebuildPending() || !L->HasActorBegunPlay()) return;
		if (At == 0.0) { At = Now + 4.0; return; }
		if (Now < At) return;
		FActorSpawnParameters SP;
		SP.ObjectFlags |= RF_Transient;
		Panel = W->SpawnActor<AOutlawsFragTablePanel>(AOutlawsFragTablePanel::StaticClass(), FTransform::Identity, SP);
		UE_LOG(LogOutlaws, Log, TEXT("OLFRAG test: %s is up, the panel %s"), *L->LevelName, Panel ? TEXT("spawned") : TEXT("NOT SPAWNED"));
		if (!Panel) { Step = 99; FGenericPlatformMisc::RequestExit(false); return; }
		Step = 1;
		At = 0.0;
		return;
	}
	const int32 k = (Step - 1) / 2;
	if (k >= NumSizes)
	{
		if (Step != 99)
		{
			UE_LOG(LogOutlaws, Log, TEXT("OLFRAG test: done, %d bad"), Bad);
			Step = 99;
			FGenericPlatformMisc::RequestExit(false);
		}
		return;
	}
	const int32 N = Sizes[k];
	if ((Step - 1) % 2 == 0)
	{
		// show the table of N in front of the eye: 320 cm wide at 300 cm, nearer (and as much smaller) when a wall is closer
		const UCameraComponent* Cam = P->FindComponentByClass<UCameraComponent>();
		const FVector Eye = Cam ? Cam->GetComponentLocation() : P->GetActorLocation();
		FVector F = Cam ? Cam->GetForwardVector() : P->GetActorForwardVector();
		F.Z = 0.0;
		F = F.GetSafeNormal();
		float Dist = 300.f;
		FHitResult Hit;
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OLFragTest), false, P);
		if (W->LineTraceSingleByChannel(Hit, Eye, Eye + F * 320.0, ECC_Visibility, Q)) Dist = FMath::Max(60.f, float(Hit.Distance) - 25.f);
		const FFragTableData D = FakeTable(N);
		Panel->SetData(D);
		const float H = 320.f * float(Panel->GetCanvasH()) / float(FOutlawsFragTable::CanvasW);
		Panel->PlaceBefore(Eye, F, Dist, 320.f * Dist / 300.f, 0.f);
		UE_LOG(LogOutlaws, Log, TEXT("OLFRAG test: %d players at %.0f cm (canvas 1920 x %d, %.0f cm tall at 300 cm): %s"), N, Dist, Panel->GetCanvasH(), H, *D.Describe());
		At = Now + 2.0;
		++Step;
		return;
	}
	if (Now < At) return;
	// the measurer against the drawing
	const int32 Measured = FOutlawsFragTable::Measure(Panel->GetData());
	const bool bOk = FMath::Abs(float(Measured) - Panel->LastDrawnBottom) <= 1.f;
	if (!bOk) ++Bad;
	UE_LOG(LogOutlaws, Log, TEXT("OLFRAG test: %d rows: measured %d, drawn to %.1f: %s"), N, Measured, Panel->LastDrawnBottom, bOk ? TEXT("ok") : TEXT("BAD"));
	if (N != 1)
	{
		const FString Dir = FPaths::ProjectSavedDir() / TEXT("OLShots");
		IFileManager::Get().MakeDirectory(*Dir, true);
		const FString Shot = Dir / FString::Printf(TEXT("ol_fragtable_%d.png"), N);
		FScreenshotRequest::RequestScreenshot(Shot, false, false);
		// and the canvas itself, as drawn
		if (UCanvasRenderTarget2D* RT = Panel->GetCanvas())
		{
			const FString Flat = Dir / FString::Printf(TEXT("ol_fragtable_%d_canvas.png"), N);
			if (FArchive* Ar = IFileManager::Get().CreateFileWriter(*Flat))
			{
				FImageUtils::ExportRenderTarget2DAsPNG(RT, *Ar);
				delete Ar;
			}
		}
		UE_LOG(LogOutlaws, Log, TEXT("OLFRAG test: screenshot %s"), *FPaths::GetCleanFilename(Shot));
	}
	At = Now + 1.0;
	++Step;
}
