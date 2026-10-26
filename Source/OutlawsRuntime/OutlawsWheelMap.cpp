// REQ-WHEELMAP (user 2026-10-15): the map is no longer a wheel item.  Whenever the wheel is open the map lies flat in its
// middle, inside the horseshoe, drawn from the held map's own canvas (MapCanvas, BuildMapDrawing) with the same markers:
// the red arrow where the player stands, pointing the way they face, and the others' blue diamonds (an arrow at the
// page's edge toward one off the drawing) with their names.  A wheel laser on the map darkens it slightly with a "Trigger
// to zoom" prompt (BUG-MAPZOOMTRIGGER, user 15 Oct: pointing alone zoomed it by accident); that hand's trigger brings it
// toward the eyes over about 0.2 s for a better look, and the trigger again, the laser leaving it or the wheel closing
// sends it back as fast.  The trigger over the map picks, drops and pages nothing.  An icon under the laser always wins
// over the map, so picking a weapon or an item round it works as before.  The markers are flat pictures on the wheel's own overlay
// material (BUG-MAPARROW: the held map's arrow was a mesh lifted off the page and went missing at some headings; these
// are drawn on the page's plane at every heading).
// Test: -OLShots -OLLevel=hideout -OLNoFrontEnd -OLWheelMapTest[=left|one] (with -OLOneController=Right for one).
// The train's map (BUG-TRAINMAP: BuildMapDrawing frames the cars alone): -OLLevel=train ... -OLWheelMapEnds=622+628 adds the
// arrow at the front car and the last; -OLOldTrainMap draws it as before.
#include "OutlawsPlayer.h"
#include "OutlawsFlags.h"
#include "OutlawsPolygon.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsLevel.h"
#include "OutlawsCoop.h"
#include "OutlawsTextures.h"
#include "OutlawsGlyphs.h"
#include "Camera/CameraComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Engine/CanvasRenderTarget2D.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "MotionControllerComponent.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"
#include "OutlawsShotTests.h"

namespace OutlawsWheelMapLocal
{
	// (the wheel's flat pictures: the engine plane turned to face the eyes, its picture upright; the plane's X across to the
	// viewer's right, its Y down; as GPlaneFacing in OutlawsPlayerWeapons.cpp)
	FQuat Facing() { return FRotationMatrix::MakeFromXY(FVector(0.0, -1.0, 0.0), FVector(0.0, 0.0, -1.0)).ToQuat(); }
	constexpr double PageMax = 15.0;          // the page's longer side on the wheel (cm at scale 1; the icons' inner edge is 11.25 out)
	constexpr double RestX = 0.1;             // in front of the horseshoe (-0.5), behind the icons (0.3)
	// pulled close it looks 3.5 times its rest size (BUG-WHEELMAPSIZE, user 15 Oct: about double the old 1.75): half the
	// way to the eyes (about 1.2 m from a 2.4 m wheel, a comfortable reading distance well clear of the hands) and 1.75
	// times as big there
	constexpr double NearDistLook = 2.0;      // the look gained by coming closer
	constexpr double NearGrow = 1.75;         // and by growing
	constexpr double NearLook = NearDistLook * NearGrow;
	constexpr float EaseTime = 0.2f;          // seconds each way
	// the player's arrow and the others' marks, cm across at scale 1 (BUG-MAPARROWSIZE, user 15 Oct: half the old 3.0 and 1.6)
	constexpr double ArrowSize = 1.5, MarkSize = 0.8;
	// (BUG-MAPZOOMTRIGGER) pointed at: the page darkened by a black sheet this see-through (of 255) just over its marks, the
	// prompt over that; its words the size the HUD's card has them (44 cm across at 80 cm), the drawing as every prompt's
	constexpr uint8 ShadeAlpha = 150;         // (72 hardly showed on the bright page: 255 to 254 in the shot)
	constexpr double ShadeLift = 0.1, PromptLift = 0.12;
	constexpr float PromptWidthPerDist = 44.f / 80.f;
	const TCHAR* const PromptText = TEXT("Trigger to zoom");

	/** A flat shape (any outline) on a clear ground (64 px, its top row the picture's top): filled, a dark rim, its edge smoothed. */
	UTexture2D* ShapeTexture(const TArray<FVector2D>& Poly, const FColor& Fill, const FColor& Rim, double RimPx)
	{
		constexpr int32 N = 64;
		TArray<FColor> Px;
		Px.SetNum(N * N);
		for (int32 y = 0; y < N; ++y)
		{
			for (int32 x = 0; x < N; ++x)
			{
				const FVector2D P(x + 0.5, y + 0.5);
				// the distance to the outline, positive inside (an even-odd crossing count says which side)
				double Near = TNumericLimits<double>::Max();
				bool bInside = false;
				for (int32 k = 0; k < Poly.Num(); ++k)
				{
					const FVector2D A = Poly[k], B = Poly[(k + 1) % Poly.Num()];
					const FVector2D AB = B - A;
					const double T = FMath::Clamp(FVector2D::DotProduct(P - A, AB) / FMath::Max(1e-9, AB.SizeSquared()), 0.0, 1.0);
					Near = FMath::Min(Near, FVector2D::Distance(P, A + AB * T));
					if (OutlawsPolygon::Crosses(A, B, P.X, P.Y)) bInside = !bInside;
				}
				const double In = bInside ? Near : -Near;
				const double A = FMath::Clamp(In + 0.5, 0.0, 1.0);
				const double R = FMath::Clamp(RimPx - In + 0.5, 0.0, 1.0);   // 1 on the rim, 0 deeper in
				const FLinearColor C = FMath::Lerp(FLinearColor(Fill), FLinearColor(Rim), float(R));
				FColor O = C.ToFColor(true);
				O.A = uint8(FMath::RoundToInt(255.0 * A));
				Px[y * N + x] = O;
			}
		}
		return OutlawsTextures::MakeTextureBGRA(N, N, Px, false, TF_Bilinear, TA_Clamp);
	}
}

void AOutlawsPlayer::ShowWheelMap()
{
	using namespace OutlawsWheelMapLocal;
	if (!WheelRoot) return;
	AOutlawsLevelActor* L = Level.Get();
	const FOutlawsLevel* Lv = L ? L->LiveLevel() : nullptr;
	const FOutlawsSector* Here = Lv ? Lv->Sector(L->PlayerSectorIndex()) : nullptr;
	// the drawing as the held map has it, made again only for another level or layer (the lock items' blobs refreshed)
	bool bDrawn = Lv && MapCanvas && MapDrawnFor.Get() == L && MapDrawnLevel == Lv && MapDrawnSectorCount == Lv->Sectors.Num() && (!Here || Here->Layer == MapLayer);
	if (bDrawn)
	{
		MapSpotsWait = 0.f;
		RefreshMapMarks(0.f);          // (the blobs' items taken, items seen: REQ-MAPITEMS)
	}
	else
	{
		bDrawn = BuildMapDrawing();
	}
	if (!WheelMapRoot)
	{
		WheelMapRoot = NewObject<USceneComponent>(this, TEXT("WheelMap"));
		WheelMapRoot->SetupAttachment(WheelRoot);
		WheelMapRoot->RegisterComponent();
		auto Plane = [this](UTexture* Tex)
		{
			UMaterialInstanceDynamic* Mid = nullptr;
			UStaticMeshComponent* P = WheelPlane(Mid);
			P->AttachToComponent(WheelMapRoot, FAttachmentTransformRules::KeepRelativeTransform);
			if (Mid && Tex) Mid->SetTextureParameterValue(TEXT("Tex"), Tex);
			return P;
		};
		// the arrow: a red head with a notched tail, its tip up the picture (north at no turn)
		WheelMapArrowTex = ShapeTexture({ FVector2D(32, 3), FVector2D(57, 60), FVector2D(32, 46), FVector2D(7, 60) }, FColor(205, 28, 20), FColor(60, 8, 4), 3.5);
		WheelMapDiamondTex = ShapeTexture({ FVector2D(32, 3), FVector2D(61, 32), FVector2D(32, 61), FVector2D(3, 32) }, FColor(40, 78, 190), FColor(12, 20, 60), 4.0);
		WheelMapEdgeTex = ShapeTexture({ FVector2D(32, 4), FVector2D(58, 58), FVector2D(6, 58) }, FColor(40, 78, 190), FColor(12, 20, 60), 4.0);
		KeepAlive.Add(WheelMapArrowTex);
		KeepAlive.Add(WheelMapDiamondTex);
		KeepAlive.Add(WheelMapEdgeTex);
		WheelMapPage = Plane(MapCanvas);
		WheelMapPage->SetRelativeLocationAndRotation(FVector::ZeroVector, Facing());
		WheelMapArrow = Plane(WheelMapArrowTex);
		WheelMapArrow->SetRelativeScale3D(FVector(ArrowSize / 100.0, ArrowSize / 100.0, 1.0));
		for (int32 k = 0; k < MapMarks; ++k)
		{
			WheelMapMark[k] = Plane(WheelMapDiamondTex);
			UTextRenderComponent* Tx = NewObject<UTextRenderComponent>(this);
			Tx->SetHorizontalAlignment(EHTA_Left);
			Tx->SetVerticalAlignment(EVRTA_TextCenter);
			Tx->SetWorldSize(1.1f);
			Tx->SetTextRenderColor(FColor(20, 14, 8));
			if (UMaterialInterface* TM = WheelOverlayMaterial(true)) Tx->SetTextMaterial(TM);
			Tx->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Tx->SetCastShadow(false);
			Tx->SetupAttachment(WheelMapRoot);
			Tx->RegisterComponent();
			WheelMapName[k] = Tx;
		}
		// (BUG-MAPZOOMTRIGGER) the slight darkening over the whole page, arrow and marks, while it is pointed at
		TArray<FColor> Dark;
		Dark.Init(FColor(0, 0, 0, ShadeAlpha), 16);
		UTexture2D* DarkTex = OutlawsTextures::MakeTextureBGRA(4, 4, Dark, false, TF_Bilinear, TA_Clamp);
		KeepAlive.Add(DarkTex);
		WheelMapShade = Plane(DarkTex);
		WheelMapShade->SetTranslucentSortPriority(8);
	}
	if (UMaterialInstanceDynamic* M = Cast<UMaterialInstanceDynamic>(WheelMapPage->GetMaterial(0)))
	{
		if (MapCanvas) M->SetTextureParameterValue(TEXT("Tex"), MapCanvas);
	}
	// the page's shape: the held page's, its longer side 15 cm on the wheel
	const double K = PageMax / FMath::Max(1.0, FMath::Max(MapPageSize.X, MapPageSize.Y));
	WheelMapHalf = MapPageSize * K * 0.5;
	WheelMapPage->SetRelativeScale3D(FVector(WheelMapHalf.X * 2.0 / 100.0, WheelMapHalf.Y * 2.0 / 100.0, 1.0));
	WheelMapShade->SetRelativeLocationAndRotation(FVector(ShadeLift, 0.0, 0.0), Facing());
	WheelMapShade->SetRelativeScale3D(WheelMapPage->GetRelativeScale3D());
	WheelMapShade->SetVisibility(false);
	if (WheelMapPrompt) WheelMapPrompt->SetVisibility(false);
	WheelMapNear = 0.f;
	bWheelMapHover = false;
	bWheelMapZoom = false;
	WheelMapPointHand = -1;
	bWheelMapTrigArmed[0] = bWheelMapTrigArmed[1] = false;     // (a trigger already held as the wheel opens zooms nothing)
	WheelMapRoot->SetRelativeLocation(FVector(RestX, 0.0, 0.0));
	WheelMapRoot->SetRelativeScale3D(FVector(1.0));
	WheelMapPage->SetVisibility(bDrawn);
	WheelMapArrow->SetVisibility(bDrawn);
	for (int32 k = 0; k < MapMarks; ++k)
	{
		WheelMapMark[k]->SetVisibility(false);
		WheelMapName[k]->SetVisibility(false);
	}
	UpdateWheelMap(0.f);
	UE_LOG(LogOutlaws, Log, TEXT("OLRT wheel map: %s, page %.1f x %.1f cm on the wheel"), bDrawn ? TEXT("shown in the horseshoe") : TEXT("no level to draw"), WheelMapHalf.X * 2.0, WheelMapHalf.Y * 2.0);
}

bool AOutlawsPlayer::WheelMapHit(const FVector& From, const FVector& Dir) const
{
	if (!WheelRoot || !WheelMapRoot || !WheelMapPage || !WheelMapPage->IsVisible()) return false;
	// on the map's own plane, where it is now (pulled close and grown, the whole of the bigger page holds it there)
	const FVector C = WheelMapRoot->GetComponentLocation();
	const FVector N = WheelRoot->GetForwardVector();
	const double Den = FVector::DotProduct(Dir, N);
	if (Den > -1e-3) return false;
	const double T = FVector::DotProduct(C - From, N) / Den;
	if (T <= 0.0) return false;
	const FVector Lp = WheelMapRoot->GetComponentTransform().InverseTransformPosition(From + Dir * T);
	const double Margin = bWheelMapHover ? 1.0 : 0.0;          // (once close, a little slack before it goes back)
	return FMath::Abs(Lp.Y) <= WheelMapHalf.X + Margin && FMath::Abs(Lp.Z) <= WheelMapHalf.Y + Margin;
}

void AOutlawsPlayer::UpdateWheelMapPointer()
{
	using namespace OutlawsWheelMapLocal;
	if (!WheelMapRoot || !WheelMapPage || !WheelMapShade || !WheelRoot || !Camera) return;
	// each hand holding the wheel: its laser on the map, and on no icon (greyed ones too) or row arrow, so an icon under the
	// laser still wins; its trigger let go since the wheel opened arms one zoom
	bool bOn[2] = { false, false };
	float Trig[2] = { 0.f, 0.f };
	const bool bShown = WheelMapPage->IsVisible();
	for (int32 H = 0; H < 2; ++H)
	{
		if (!bWheelHeld[H])
		{
			bWheelMapTrigArmed[H] = false;
			continue;
		}
		Trig[H] = Axis(H == 0 ? TEXT("OLVR_LeftTrigger") : TEXT("OLVR_RightTrigger"));
		if (Trig[H] < 0.3f) bWheelMapTrigArmed[H] = true;
		if (!bShown) continue;
		if (WheelMapTestHover >= 0)
		{
			bOn[H] = WheelMapTestHover == 1 && !bOn[0];        // (desktop tests: the first hand holding the wheel points)
			continue;
		}
		if (!bVR || WheelPick[H] >= 0 || WheelDropPick[H] >= 0 || WheelArrowHover[H] != 0) continue;
		UMotionControllerComponent* Aim = AimOf(H);
		bOn[H] = Aim && WheelMapHit(Aim->GetComponentLocation(), Aim->GetForwardVector());
	}
	// the hand pointing: the one already on it, else the first that is
	const int32 Was = WheelMapPointHand;
	const int32 Point = (Was >= 0 && bOn[Was]) ? Was : bOn[0] ? 0 : bOn[1] ? 1 : -1;
	if (Point >= 0 && Was < 0) Pulse(Point, 0.12f);
	// a pull on the map (either hand on it) zooms it in, or out again; it picks, drops and pages nothing (the row arrows'
	// own pull disarmed with it)
	for (int32 H = 0; H < 2; ++H)
	{
		if (!bOn[H] || !bWheelMapTrigArmed[H] || Trig[H] <= 0.7f) continue;
		bWheelMapTrigArmed[H] = false;
		bWheelTriggerArmed[H] = false;
		bWheelMapZoom = !bWheelMapZoom;
		Pulse(H, 0.2f);
		UE_LOG(LogOutlaws, Log, TEXT("OLRT wheel map: the %s trigger %s"), H == 0 ? TEXT("left") : TEXT("right"), bWheelMapZoom ? TEXT("zooms it in") : TEXT("sends it back"));
	}
	// no laser on it: back it goes
	if (Point < 0 && bWheelMapZoom)
	{
		bWheelMapZoom = false;
		UE_LOG(LogOutlaws, Log, TEXT("OLRT wheel map: pointed away, sent back"));
	}
	if (Point != Was) UE_LOG(LogOutlaws, Log, TEXT("OLRT wheel map: %s"), Point < 0 ? TEXT("not pointed at") : Point == 0 ? TEXT("pointed at by the left hand") : TEXT("pointed at by the right hand"));
	WheelMapPointHand = Point;
	bWheelMapHover = Point >= 0;
	// pointed at and not pulled close: darkened, with the prompt and the pointing hand's trigger drawn
	const bool bPrompt = bWheelMapHover && !bWheelMapZoom;
	WheelMapShade->SetVisibility(bPrompt);
	if (bPrompt && !WheelMapPrompt)
	{
		const double Scale = FMath::Max(0.01, WheelRoot->GetComponentScale().X);
		const float Dist = float(FVector::Dist(Camera->GetComponentLocation(), WheelRoot->GetComponentLocation()) / Scale);     // (wheel cm)
		WheelMapPrompt = NewObject<UOutlawsGlyphPlate>(this);
		WheelMapPrompt->SetupAttachment(WheelMapRoot);
		WheelMapPrompt->RegisterComponent();
		WheelMapPrompt->Init(LoadOverlayMaterial(TEXT("M_OLR_MenuOverlay")), GlyphFont(false), GlyphFont(true), PromptWidthPerDist * Dist, 320, OutlawsGlyphs::ViewTan * Dist);
		WheelMapPrompt->SetCastShadow(false);
		WheelMapPrompt->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	}
	if (WheelMapPrompt)
	{
		if (bPrompt)
		{
			FOutlawsGlyphRow Row;
			Row.Glyph = OutlawsGlyphs::ForControl(this, OutlawsGlyphs::ECtl::Trigger, Point);
			Row.Text = PromptText;
			if (WheelMapPrompt->SetRows({ Row })) UE_LOG(LogOutlaws, Log, TEXT("OLGLYPH wheel map prompt: %s, \"%s\""), *Row.Glyph.Describe(), *Row.Text);
			WheelMapPrompt->PlaceFacing(FVector(PromptLift, 0.0, 0.0), FQuat::Identity, true);
		}
		WheelMapPrompt->SetVisibility(bPrompt);
	}
}

void AOutlawsPlayer::UpdateWheelMap(float RealDt)
{
	using namespace OutlawsWheelMapLocal;
	if (!WheelMapRoot || !WheelMapPage || !WheelRoot || !Camera) return;
	AOutlawsLevelActor* L = Level.Get();
	const FOutlawsLevel* Lv = L ? L->LiveLevel() : nullptr;
	if (!Lv || !WheelMapPage->IsVisible()) return;
	// another layer underfoot (a lift, stairs) while the wheel is up: drawn again
	if (const FOutlawsSector* Here = Lv->Sector(L->PlayerSectorIndex()); Here && Here->Layer != MapLayer) ShowWheelMap();
	RefreshMapMarks(RealDt);           // (an item seen while the wheel is up: REQ-MAPITEMS)
	// the pull toward the eyes: a straight 0.2 s ramp each way, eased at its ends
	const float Was = WheelMapNear;
	WheelMapNear = FMath::FInterpConstantTo(WheelMapNear, bWheelMapZoom ? 1.f : 0.f, RealDt, 1.f / EaseTime);
	const double Ease = FMath::SmoothStep(0.0, 1.0, double(WheelMapNear));
	const double Scale = FMath::Max(0.01, WheelRoot->GetComponentScale().X);
	const double Dist = FVector::Dist(Camera->GetComponentLocation(), WheelRoot->GetComponentLocation());
	const double Pull = Dist * (1.0 - 1.0 / NearDistLook) / Scale;        // (wheel cm toward the eyes, fully close)
	WheelMapRoot->SetRelativeLocation(FVector(RestX + Ease * Pull, 0.0, 0.0));
	const double Grow = 1.0 + Ease * (NearGrow - 1.0);
	WheelMapRoot->SetRelativeScale3D(FVector(Grow));
	// over the icons while it is out in front of them, among them at rest
	if ((Was > 0.f) != (WheelMapNear > 0.f) || RealDt == 0.f)
	{
		const bool bUp = WheelMapNear > 0.f;
		WheelMapPage->SetTranslucentSortPriority(bUp ? 6 : 2);
		WheelMapArrow->SetTranslucentSortPriority(bUp ? 8 : 4);
		for (int32 k = 0; k < MapMarks; ++k)
		{
			WheelMapMark[k]->SetTranslucentSortPriority(bUp ? 7 : 3);
			WheelMapName[k]->SetTranslucentSortPriority(bUp ? 7 : 5);
		}
	}
	// page cm (the held map's: across east, up north) to the wheel's (its Y the viewer's left, Z up)
	const double K = PageMax / FMath::Max(1.0, FMath::Max(MapPageSize.X, MapPageSize.Y));
	const FVector2D HalfPg = MapPageSize * 0.5 * 0.9;
	auto OnWheel = [K](const FVector2D& Q, double Lift) { return FVector(Lift, -Q.X * K, Q.Y * K); };
	// the player's arrow: where the eyes are, turned the way they face (the view flattened, helped by its up when looking
	// down, as the held map's), drawn on the page's own plane so no heading can hide it
	const FVector Eye = Camera->GetComponentLocation();
	const FVector Fw = Camera->GetForwardVector(), Up = Camera->GetUpVector();
	FVector2D H(Fw.X - Up.X * Fw.Z, Fw.Y - Up.Y * Fw.Z);
	H = H.IsNearlyZero() ? FVector2D(1.0, 0.0) : H.GetSafeNormal();
	FVector2D Q((Eye.Y - MapCentre.Y) * MapScale, (Eye.X - MapCentre.X) * MapScale);
	Q = FVector2D(FMath::Clamp(Q.X, -HalfPg.X, HalfPg.X), FMath::Clamp(Q.Y, -HalfPg.Y, HalfPg.Y));
	WheelMapArrowYaw = float(FMath::RadiansToDegrees(FMath::Atan2(H.Y, H.X)));          // (north up the page, east to the right: the yaw)
	const FQuat Turn(FVector::XAxisVector, FMath::DegreesToRadians(double(WheelMapArrowYaw)));     // (about the eyes' axis: clockwise as seen)
	WheelMapArrow->SetRelativeLocationAndRotation(OnWheel(Q, 0.06), Turn * Facing());
	// the others: a diamond where they stand on the drawing, an arrow at the page's edge toward them when off it, the name beside
	for (int32 Slot = 0; Slot < MapMarks; ++Slot)
	{
		UStaticMeshComponent* Mk = WheelMapMark[Slot];
		UTextRenderComponent* Tx = WheelMapName[Slot];
		FVector Feet;
		FString Name;
		if (!UOutlawsCoop::PartnerForMap(this, Feet, Name, Slot))
		{
			Mk->SetVisibility(false);
			Tx->SetVisibility(false);
			continue;
		}
		const FVector2D P((Feet.Y - MapCentre.Y) * MapScale, (Feet.X - MapCentre.X) * MapScale);
		const int32 Si = L->CullSectorOf(Feet + FVector(0.0, 0.0, 10.0));
		const FOutlawsSector* S = Lv->Sector(Si);
		const bool bOn = S && MapSectors.Contains(Si) && S->Layer == MapLayer && FMath::Abs(P.X) <= HalfPg.X && FMath::Abs(P.Y) <= HalfPg.Y;
		FVector2D At = P, Dir(0.0, 1.0);
		if (!bOn)
		{
			const FVector2D Away = P.IsNearlyZero() ? FVector2D(0.0, 1.0) : P.GetSafeNormal();
			const double T = FMath::Min(HalfPg.X / FMath::Max(1e-6, FMath::Abs(Away.X)), HalfPg.Y / FMath::Max(1e-6, FMath::Abs(Away.Y)));
			At = Away * T;
			Dir = Away;
		}
		const double DirYaw = FMath::RadiansToDegrees(FMath::Atan2(Dir.X, Dir.Y));
		Mk->SetRelativeLocationAndRotation(OnWheel(At, 0.04), FQuat(FVector::XAxisVector, FMath::DegreesToRadians(bOn ? 0.0 : DirYaw)) * Facing());
		Mk->SetRelativeScale3D(FVector(MarkSize / 100.0, MarkSize / 100.0, 1.0));
		if (UMaterialInstanceDynamic* M = Cast<UMaterialInstanceDynamic>(Mk->GetMaterial(0))) M->SetTextureParameterValue(TEXT("Tex"), bOn ? WheelMapDiamondTex : WheelMapEdgeTex);
		Mk->SetVisibility(true);
		const FVector2D NameAt = bOn ? At + FVector2D(0.9 / K, 0.0) : At - Dir * (1.8 / K);
		Tx->SetText(FText::FromString(Name));
		Tx->SetHorizontalAlignment(bOn ? EHTA_Left : EHTA_Center);
		Tx->SetRelativeLocation(OnWheel(NameAt, 0.08));
		Tx->SetVisibility(!Name.IsEmpty());
	}
}

// ---- -OLShots -OLLevel=hideout -OLNoFrontEnd -OLWheelMapTest[=left|one] ---------------------------------------------------
// The wheel opened by the main hand (the one hand with -OLOneController), the map in its middle photographed; a pointer at
// the map's middle and at every weapon checked (the map only at its middle, each icon still picked); pointed at for 0.6 s
// with no pull (darkened, the "Trigger to zoom" prompt with the pointing hand's trigger, no zoom; photographed); the main
// hand's trigger pulled, the map's distance from the eyes logged every frame for 0.6 s; photographed close; the trigger
// again (back, the prompt again); the trigger, then pointed away (back); the trigger, then the wheel closed and opened again
// (at rest); what the hands hold, the rounds, the wheel's row and the drops unchanged by every pull; then the player
// turned through the full circle in 15 degree steps, the arrow checked at each (shown, on the page, the same shape and size,
// its heading the player's within 2 degrees) and photographed at about 340 and 205 degrees.

namespace
{
	struct FWheelMapTest
	{
		int32 Step = 0;
		double At = 0.0;
		double RampStart = 0.0;
		double RestMapDist = 0.0;
		int32 Pass = 0, Fail = 0;
		int32 Sweep = 0;
		bool bShotTaken = false;
		FString Tag;
		float BaseYaw = 0.f;
		// (BUG-MAPZOOMTRIGGER) the trigger held by the test until this time; which way back the ramp out is measuring; the
		// hands' state when first pointed (no pull over the map may change it)
		double PullUntil = 0.0;
		int32 Round = 0;
		double MaxNearPointed = 0.0;
		FString Items;
	};
	FWheelMapTest WM;
}

#if WITH_OUTLAWS_TESTS
void AOutlawsPlayer::WheelMapTestTick()
{
	AOutlawsLevelActor* L = Level.Get();
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (!L || !PC || L->IsFrontEnd() || L->PendingLoad.IsValid() || WM.Step < 0) return;
	const double Now = FPlatformTime::Seconds();
	// (a co-op client's player is made again when it joins: this one's wheel not built yet, the run starts over with it)
	if (WM.Step >= 1 && (!WheelMapPage || !WheelMapRoot || !WheelRoot)) WM = FWheelMapTest();
	if (WM.At == 0.0) { WM.At = Now + (OLFlags::ValueIs(TEXT("OLWheelMapTest"), TEXT("coop")) ? 16.0 : 5.0); OLFlags::Value(TEXT("OLWheelMapTest"), WM.Tag); if (WM.Tag.IsEmpty()) WM.Tag = TEXT("right");
		if (WM.Tag == TEXT("left")) DominantHand = EOutlawsHand::Left;
		return; }
	const int32 One = OneHand();
	const int32 Hand = One >= 0 ? One : (LeftDominant() ? 0 : 1);
	const TCHAR* TrigName = Hand == 0 ? TEXT("OLVR_LeftTrigger") : TEXT("OLVR_RightTrigger");
	if (WM.PullUntil > 0.0 && Now >= WM.PullUntil)
	{
		TestAxes.Remove(TrigName);          // (the pull let go)
		WM.PullUntil = 0.0;
	}
	if (Now < WM.At && WM.Step != 3 && WM.Step != 4 && WM.Step != 20) return;
	auto Check = [](bool bOk, const FString& What)
	{
		(bOk ? WM.Pass : WM.Fail)++;
		UE_LOG(LogOutlaws, Display, TEXT("OLWHEELMAP check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
	};
	auto Shot = [this](const FString& Name)
	{
		const FString Path = FPaths::ProjectSavedDir() / TEXT("OLShots") / (TEXT("ol_wheelmap_") + WM.Tag + TEXT("_") + Name + TEXT(".png"));
		FScreenshotRequest::RequestScreenshot(Path, false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLWHEELMAP shot %s"), *Path);
	};
	// a trigger pull on the main hand (held a tenth of a second, as Axis reads it)
	auto Pull = [&]()
	{
		TestAxes.Add(TrigName, 1.f);
		WM.PullUntil = Now + 0.1;
		UE_LOG(LogOutlaws, Display, TEXT("OLWHEELMAP the %s trigger pulled"), Hand == 0 ? TEXT("left") : TEXT("right"));
	};
	// what the hands hold, the rounds, the wheel's row and the drops so far: a pull over the map changes none of it
	auto ItemsNow = [this, Hand]()
	{
		const AOutlawsLevelActor* Lv = Level.Get();
		return FString::Printf(TEXT("held %d/%d, rounds %d/%d, row from %d, pick %d, drops %d"), Equipped[0], Equipped[1], int32(Mag[0]), int32(Mag[1]), WheelItemStart, WheelPick[Hand], Lv ? Lv->WheelDrops : -1);
	};
	auto PromptShown = [this]()
	{
		return WheelMapShade && WheelMapShade->IsVisible() && WheelMapPrompt && WheelMapPrompt->IsVisible();
	};
	auto Open = [&](float Yaw)
	{
		CloseWheel();
		PC->SetControlRotation(FRotator(-8.f, Yaw, 0.f));
		OpenWheel(Hand);
		WheelDir = FRotator(-8.f, Yaw, 0.f).Vector();      // (the camera turns on its own tick: the wheel hung where the view now goes)
	};
	// the arrow's heading as drawn: its picture's up (the plane's -Y) seen on the wheel, as a yaw (up north, right east)
	auto ArrowYawSeen = [this]()
	{
		const FVector TipW = WheelMapArrow->GetComponentQuat().RotateVector(FVector(0.0, -1.0, 0.0));
		const FVector Lt = WheelRoot->GetComponentQuat().Inverse().RotateVector(TipW);
		return float(FMath::RadiansToDegrees(FMath::Atan2(-Lt.Y, Lt.Z)));
	};
	auto MapDist = [this]() { return FVector::Dist(Camera->GetComponentLocation(), WheelMapPage->GetComponentLocation()); };
	switch (WM.Step)
	{
	case 0:
	{
		WM.BaseYaw = float(GetControlRotation().Yaw);
		Equip(Hand, 0);
		Open(WM.BaseYaw);
		UE_LOG(LogOutlaws, Display, TEXT("OLWHEELMAP layout %s: dominant %s, one controller %d, the wheel opened by the %s hand, facing yaw %.1f"), *WM.Tag, LeftDominant() ? TEXT("left") : TEXT("right"), One,
			Hand == 0 ? TEXT("left") : TEXT("right"), WM.BaseYaw);
		WM.Step = 1;
		WM.At = Now + 1.0;
		return;
	}
	case 1:
	{
		Check(bWheelOpen && WheelMapPage && WheelMapPage->IsVisible(), TEXT("the map shows in the wheel's middle when it opens"));
		Check(!WheelItem.Contains(FString(TEXT("imap"))), TEXT("the map is no wheel item"));
		const FVector Lm = WheelMapRoot->GetRelativeLocation();
		Check(FMath::Abs(Lm.Y) < 0.01 && FMath::Abs(Lm.Z) < 0.01, FString::Printf(TEXT("the map centred in the horseshoe (%.2f, %.2f)"), Lm.Y, Lm.Z));
		const double Corner = WheelMapHalf.Size();
		Check(Corner < 11.25, FString::Printf(TEXT("the resting page inside the icons' ring (corner %.2f cm from the middle, icons from 11.25)"), Corner));
		Check(WheelMapArrow->IsVisible(), TEXT("the red arrow shows"));
		// a pointer from the eyes: at the map's middle it is on the map; at each weapon not, and the icon is nearest there
		const FVector Eye = Camera->GetComponentLocation();
		Check(WheelMapHit(Eye, (WheelRoot->GetComponentLocation() - Eye).GetSafeNormal()), TEXT("a pointer at the map's middle is on the map"));
		int32 Clear = 0, Weapons = 0;
		for (int32 i = 0; i < WheelWeapon.Num() && i < WheelSlots.Num(); ++i)
		{
			if (WheelWeapon[i] < -1) continue;
			++Weapons;
			if (!WheelMapHit(Eye, (WheelSlots[i]->GetComponentLocation() - Eye).GetSafeNormal())) ++Clear;
		}
		Check(Weapons > 0 && Clear == Weapons, FString::Printf(TEXT("a pointer at each weapon is off the map (%d of %d)"), Clear, Weapons));
		Shot(TEXT("open"));
		WM.Step = 2;
		WM.At = Now + 1.0;
		return;
	}
	case 2:
	{
		// (BUG-MAPZOOMTRIGGER) pointed at for 0.6 s with no pull: darkened with the prompt, and it stays where it is
		WheelMapTestHover = 1;
		WM.RampStart = Now;
		WM.RestMapDist = MapDist();
		WM.MaxNearPointed = 0.0;
		WM.Items = ItemsNow();
		UE_LOG(LogOutlaws, Display, TEXT("OLWHEELMAP pointed at, no pull: %s"), *WM.Items);
		WM.Step = 20;
		return;
	}
	case 20:
	{
		WM.MaxNearPointed = FMath::Max(WM.MaxNearPointed, double(WheelMapNear));
		if (Now - WM.RampStart < 0.6) return;
		Check(bWheelMapHover && WheelMapPointHand == Hand, FString::Printf(TEXT("pointed at by the %s hand (%d)"), Hand == 0 ? TEXT("left") : TEXT("right"), WheelMapPointHand));
		Check(WM.MaxNearPointed == 0.0 && !bWheelMapZoom && FMath::Abs(MapDist() - WM.RestMapDist) < 0.1, FString::Printf(TEXT("pointing alone zooms nothing (near %.3f at most, %.1f cm, %.1f at rest)"), WM.MaxNearPointed, MapDist(), WM.RestMapDist));
		Check(PromptShown(), TEXT("pointed at: the page darkened and the prompt shown"));
		if (WheelMapPrompt && WheelMapPrompt->GetRows().Num() == 1)
		{
			const FOutlawsGlyphRow& R = WheelMapPrompt->GetRows()[0];
			const TArray<OutlawsGlyphs::FMark, TInlineAllocator<2>> Drawn = R.Glyph.Drawn();
			const bool bTrig = Drawn.Num() == 1 && Drawn[0].Ctl == OutlawsGlyphs::ECtl::Trigger && Drawn[0].Hand == Hand;
			Check(R.Text == TEXT("Trigger to zoom") && bTrig, FString::Printf(TEXT("the prompt reads \"%s\" over %s (the pointing hand's trigger)"), *R.Text, *R.Glyph.Describe()));
			// on the page, over its marks, and its words and drawing inside the page as seen
			const FVector Pl = WheelMapPrompt->GetRelativeLocation();
			Check(FMath::Abs(Pl.Y) < 0.01 && FMath::Abs(Pl.Z) < 0.01 && Pl.X > 0.08, FString::Printf(TEXT("the prompt centred on the page, in front of its marks (%.2f, %.2f, %.2f)"), Pl.X, Pl.Y, Pl.Z));
		}
		else Check(false, TEXT("the prompt has one row"));
		const FVector Sc = WheelMapShade->GetRelativeScale3D(), Pg = WheelMapPage->GetRelativeScale3D();
		Check(Sc.Equals(Pg, 1e-4) && WheelMapShade->GetRelativeLocation().X > 0.08, FString::Printf(TEXT("the darkening covers the page and its marks (%.3f x %.3f)"), Sc.X, Sc.Y));
		Shot(TEXT("pointed"));
		WM.Step = 21;
		WM.At = Now + 0.5;
		return;
	}
	case 21:
	{
		Pull();
		WM.RampStart = Now;
		UE_LOG(LogOutlaws, Display, TEXT("OLWHEELMAP ease t=0.000 near=%.3f dist=%.1f (trigger pulled)"), WheelMapNear, MapDist());
		WM.Step = 3;
		return;
	}
	case 22:
	{
		// zoomed in again, then pointed away (Round 1) or the wheel closed (Round 2)
		Pull();
		WM.Step = 23;
		WM.At = Now + 0.6;
		return;
	}
	case 23:
	{
		Check(bWheelMapZoom && WheelMapNear >= 1.f, FString::Printf(TEXT("the trigger zooms it in again (near %.3f)"), WheelMapNear));
		Check(ItemsNow() == WM.Items, FString::Printf(TEXT("the pull over the map changes nothing held (%s)"), *ItemsNow()));
		if (WM.Round == 1)
		{
			WheelMapTestHover = 0;
			WM.RampStart = Now;
			UE_LOG(LogOutlaws, Display, TEXT("OLWHEELMAP ease t=0.000 near=%.3f dist=%.1f (pointed away)"), WheelMapNear, MapDist());
			WM.Step = 4;
			return;
		}
		// closing the wheel sends it back: open again, it rests in the horseshoe
		CloseWheel();
		Check(!bWheelMapZoom && !bWheelMapHover, TEXT("closing the wheel ends the zoom"));
		Open(WM.BaseYaw);
		Check(WheelMapNear == 0.f && !bWheelMapZoom && FMath::Abs(WheelMapRoot->GetRelativeLocation().X - 0.1) < 0.01 && FMath::IsNearlyEqual(WheelMapRoot->GetRelativeScale3D().X, 1.0, 1e-3),
			FString::Printf(TEXT("opened again it rests in the horseshoe (near %.3f, scale %.2f)"), WheelMapNear, WheelMapRoot->GetRelativeScale3D().X));
		WheelMapTestHover = 0;
		WM.Step = 6;
		WM.Sweep = 0;
		WM.At = Now + 0.5;
		return;
	}
	case 24:
	{
		// pointed at again after pointing away, for the wheel close round
		WheelMapTestHover = 1;
		WM.Round = 2;
		WM.Step = 22;
		WM.At = Now + 0.3;
		return;
	}
	case 3:
	case 4:
	{
		// every frame for 0.6 s: the pull's state and the map's distance from the eyes
		const double T = Now - WM.RampStart;
		UE_LOG(LogOutlaws, Display, TEXT("OLWHEELMAP ease t=%.3f near=%.3f dist=%.1f (%s)"), T, WheelMapNear, MapDist(), WM.Step == 3 ? TEXT("trigger pulled") : WM.Round == 0 ? TEXT("trigger again") : TEXT("pointed away"));
		static double ReachedAt = -1.0;
		if (WM.Step == 3 && ReachedAt < 0.0 && WheelMapNear >= 1.f) ReachedAt = T;
		if (WM.Step == 4 && ReachedAt < 0.0 && WheelMapNear <= 0.f) ReachedAt = T;
		if (T < 0.6) return;
		Check(ReachedAt > 0.12 && ReachedAt < 0.3, FString::Printf(TEXT("%s in %.3f s (about 0.2 s)"), WM.Step == 3 ? TEXT("pulled close") : TEXT("back in the horseshoe"), ReachedAt));
		ReachedAt = -1.0;
		if (WM.Step == 3)
		{
			const double Rest = FVector::Dist(Camera->GetComponentLocation(), WheelRoot->GetComponentLocation());
			Check(MapDist() < Rest * 0.7, FString::Printf(TEXT("close it is nearer the eyes (%.1f cm, the wheel %.1f cm)"), MapDist(), Rest));
			// (BUG-WHEELMAPSIZE) as seen, 3.5 times its rest size: half the way in and 1.75 times as big, clear of the hands
			const double Grown = WheelMapRoot->GetRelativeScale3D().X;
			const double Seen = Grown * WM.RestMapDist / FMath::Max(1.0, MapDist());
			Check(FMath::Abs(Seen - 3.5) < 0.25 && MapDist() > 90.0, FString::Printf(TEXT("close it looks %.2f times its rest size (about 3.5; %.2f times as big at %.1f cm, %.1f cm at rest, %.1f cm long)"),
				Seen, Grown, MapDist(), WM.RestMapDist, WheelMapHalf.GetMax() * 2.0 * WheelMapRoot->GetComponentScale().X));
			// the bigger page holds the laser over all of it: a pointer from the eyes at near its corner still on the map
			const FVector Eye = Camera->GetComponentLocation();
			const FVector Corner = WheelMapRoot->GetComponentTransform().TransformPosition(FVector(0.0, WheelMapHalf.X * 0.9, WheelMapHalf.Y * 0.9));
			Check(WheelMapHit(Eye, (Corner - Eye).GetSafeNormal()), TEXT("close, a pointer near the bigger page's corner keeps it close"));
			Check(bWheelMapZoom && !PromptShown(), TEXT("zoomed in: the darkening and the prompt gone"));
			Check(ItemsNow() == WM.Items, FString::Printf(TEXT("the pull over the map changes nothing held (%s)"), *ItemsNow()));
			Shot(TEXT("close"));
			WM.RampStart = Now + 1.0;
			WM.Step = 5;
			WM.At = Now + 1.0;
		}
		else
		{
			Check(FMath::Abs(WheelMapRoot->GetRelativeLocation().X - 0.1) < 0.01 && FMath::IsNearlyEqual(WheelMapRoot->GetRelativeScale3D().X, 1.0, 1e-3), FString::Printf(TEXT("%s: back at rest inside the horseshoe, its rest size"), WM.Round == 0 ? TEXT("the trigger again") : TEXT("pointed away")));
			Check(ItemsNow() == WM.Items, FString::Printf(TEXT("the pulls over the map change nothing held (%s)"), *ItemsNow()));
			if (WM.Round == 0)
			{
				Check(!bWheelMapZoom && PromptShown(), TEXT("sent back by the trigger, still pointed at: the prompt shows again"));
				WM.Round = 1;
				WM.Step = 22;
				WM.At = Now + 0.3;
			}
			else
			{
				Check(!bWheelMapZoom && !bWheelMapHover && !PromptShown(), TEXT("pointed away: no darkening, no prompt"));
				WM.Step = 24;
				WM.At = Now + 0.3;
			}
		}
		return;
	}
	case 5:
	{
		// the trigger again while still pointed at
		Pull();
		WM.Round = 0;
		WM.RampStart = Now;
		UE_LOG(LogOutlaws, Display, TEXT("OLWHEELMAP ease t=0.000 near=%.3f dist=%.1f (trigger again)"), WheelMapNear, MapDist());
		WM.Step = 4;
		return;
	}
	case 6:
	{
		// the sweep: the player turned to each 15 degrees (yaw from north), the wheel opened there
		// 0 to 345 in 15s, then 205 and 340 (the user's two snapshot headings), each photographed
		auto SweepYaw = [](int32 k) { return k < 24 ? float(k * 15) : k == 24 ? 205.f : 340.f; };
		constexpr int32 SweepSteps = 26;
		if (WM.Sweep > 0 && !WM.bShotTaken)
		{
			const float Yaw = SweepYaw(WM.Sweep - 1);
			const float Face = float(FRotator::NormalizeAxis(Camera->GetComponentRotation().Yaw));
			const float Seen = ArrowYawSeen();
			const float Err = FMath::Abs(FRotator::NormalizeAxis(Seen - Face));
			const FVector Al = WheelMapArrow->GetRelativeLocation();
			const bool bOnPage = FMath::Abs(Al.Y) <= WheelMapHalf.X && FMath::Abs(Al.Z) <= WheelMapHalf.Y;
			const FVector Sc = WheelMapArrow->GetRelativeScale3D();
			const bool bShape = FMath::IsNearlyEqual(Sc.X, Sc.Y) && FMath::IsNearlyEqual(Sc.X * 100.0, OutlawsWheelMapLocal::ArrowSize, 0.01);
			// nothing else drawn on the page but the arrow and the others' marks (no line or stray part)
			TArray<USceneComponent*> Parts;
			WheelMapRoot->GetChildrenComponents(true, Parts);
			int32 Shown = 0;
			for (USceneComponent* C : Parts) if (C && C->IsVisible()) ++Shown;
			int32 Others = 0;
			for (int32 k = 0; k < MapMarks; ++k) Others += (WheelMapMark[k]->IsVisible() ? 1 : 0) + (WheelMapName[k]->IsVisible() ? 1 : 0);
			const float FaceErr = FMath::Abs(FRotator::NormalizeAxis(Face - Yaw));
			const bool bOk = WheelMapArrow->IsVisible() && bOnPage && bShape && Err <= 2.f && FaceErr <= 1.f && Shown == 2 + Others;
			Check(bOk, FString::Printf(TEXT("sweep yaw %3.0f: the player faces %6.1f, the arrow points %6.1f (off %.2f), shown %d, on the page %d at (%.2f, %.2f), %.1f x %.1f cm, parts shown %d (page + arrow + %d marks)"),
				Yaw, Face, Seen, Err, WheelMapArrow->IsVisible() ? 1 : 0, bOnPage ? 1 : 0, -Al.Y, Al.Z, Sc.X * 100.0, Sc.Y * 100.0, Shown, Others));
			if (WM.Sweep - 1 >= 24 || Yaw == 90.f || Yaw == 0.f)
			{
				// (the shot is taken at this frame's end: the next turn waits for a later frame)
				Shot(FString::Printf(TEXT("yaw%03d"), FMath::RoundToInt(Yaw)));
				WM.bShotTaken = true;
				WM.At = Now + 0.3;
				return;
			}
		}
		WM.bShotTaken = false;
		if (WM.Sweep >= SweepSteps)
		{
			WM.Step = 7;
			WM.At = Now + 1.0;
			return;
		}
		Open(SweepYaw(WM.Sweep));
		++WM.Sweep;
		WM.At = Now + 0.6;
		return;
	}
	case 7:
	{
		CloseWheel();
		Check(!WheelMapPage->IsVisible() && !WheelMapArrow->IsVisible(), TEXT("closed with the wheel"));
		// (BUG-TRAINMAP) -OLWheelMapEnds=622+628: the player stood in each of those sectors in turn (the train's front car and
		// its last), the wheel opened, the arrow checked on the page near its top or bottom and not held at its edge
		FString Ends;
		if (OLFlags::Value(TEXT("OLWheelMapEnds"), Ends) && WM.Sweep >= 26)
		{
			TArray<FString> Parts;
			Ends.ParseIntoArray(Parts, TEXT("+"));
			if (Parts.IsValidIndex(WM.Sweep - 26))
			{
				FVector Spot;
				if (L->DebugSectorSpot(Parts[WM.Sweep - 26], Spot))
				{
					TeleportTo(Spot + FVector(0.0, 0.0, 92.0), GetActorRotation(), false, true);
					UE_LOG(LogOutlaws, Display, TEXT("OLWHEELMAP to sector %s (floor %s)"), *Parts[WM.Sweep - 26], *Spot.ToString());
				}
				else Check(false, FString::Printf(TEXT("sector %s found"), *Parts[WM.Sweep - 26]));
				WM.Step = 30;
				WM.At = Now + 1.5;
				return;
			}
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLWHEELMAP %s done: %d PASS, %d FAIL%s"), *WM.Tag, WM.Pass, WM.Fail, WM.Fail == 0 ? TEXT(" ALL PASS") : TEXT(""));
		WM.Step = 8;
		WM.At = Now + 1.0;
		return;
	}
	case 30:
	{
		Open(WM.BaseYaw);
		WM.Step = 31;
		WM.At = Now + 0.6;
		return;
	}
	case 31:
	{
		FString Ends;
		OLFlags::Value(TEXT("OLWheelMapEnds"), Ends);
		TArray<FString> Parts;
		Ends.ParseIntoArray(Parts, TEXT("+"));
		const FString Name = Parts.IsValidIndex(WM.Sweep - 26) ? Parts[WM.Sweep - 26] : FString(TEXT("?"));
		const FVector Al = WheelMapArrow->GetRelativeLocation();
		const double Up = Al.Z / FMath::Max(1e-6, WheelMapHalf.Y);        // (-1 the page's bottom edge, 1 its top)
		const double Across = -Al.Y / FMath::Max(1e-6, WheelMapHalf.X);
		const FVector Eye = Camera->GetComponentLocation();
		const FVector2D Q((Eye.Y - MapCentre.Y) * MapScale, (Eye.X - MapCentre.X) * MapScale);
		const bool bHeld = FMath::Abs(Q.X) > MapPageSize.X * 0.45 || FMath::Abs(Q.Y) > MapPageSize.Y * 0.45;
		Check(WheelMapArrow->IsVisible() && !bHeld && FMath::Abs(Up) > 0.6 && FMath::Abs(Across) < 0.5,
			FString::Printf(TEXT("in sector %s the arrow is on the page at %.2f up, %.2f across (of 1 to the edge), not held at the edge (page %.2f, %.2f of %.1f x %.1f cm)"), *Name, Up, Across, Q.X, Q.Y, MapPageSize.X, MapPageSize.Y));
		Shot(TEXT("end_") + Name);
		WM.Step = 32;
		WM.At = Now + 0.3;           // (the shot is taken at this frame's end: the wheel closed on a later one)
		return;
	}
	case 32:
	{
		CloseWheel();
		++WM.Sweep;
		WM.Step = 7;
		WM.At = Now + 0.5;
		return;
	}
	default:
		WM.Step = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	}
}

OL_SHOT_MATCH(OLWheelMapTest) { return OLFlags::Has(TEXT("OLWheelMapTest")); }
OL_SHOT_TEST(OLWheelMapTest, 280)
{
	WheelMapTestTick();              // (REQ-WHEELMAP: OutlawsWheelMap.cpp)
}
#endif // WITH_OUTLAWS_TESTS
