#include "OutlawsGlyphs.h"
#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "CanvasItem.h"
#include "CanvasTypes.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "GameFramework/InputSettings.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerInput.h"
#include "InputCoreTypes.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Materials/MaterialInstanceDynamic.h"

// ---- what is bound where -----------------------------------------------------------------------------------------------

namespace OutlawsGlyphs
{
	static const TCHAR* CtlName(ECtl C)
	{
		switch (C)
		{
		case ECtl::Lower: return TEXT("lower");
		case ECtl::Upper: return TEXT("upper");
		case ECtl::Stick: return TEXT("stick");
		case ECtl::StickClick: return TEXT("stick click");
		case ECtl::Menu: return TEXT("menu");
		case ECtl::Trigger: return TEXT("trigger");
		case ECtl::Grip: return TEXT("grip");
		}
		return TEXT("?");
	}

	FString FGlyph::Describe() const
	{
		if (Marks.Num() == 0) return TEXT("none");
		FString S;
		for (const FMark& M : Marks) S += FString::Printf(TEXT("%s%s %s"), S.IsEmpty() ? TEXT("") : TEXT(" + "), M.Hand == 0 ? TEXT("L") : TEXT("R"), CtlName(M.Ctl));
		return S;
	}

	TArray<FMark, TInlineAllocator<2>> FGlyph::Drawn() const
	{
		TArray<FMark, TInlineAllocator<2>> Out;
		for (const FMark& M : Marks) if (Marks.Num() == 1 || M.Hand == Main) Out.Add(M);
		if (Out.Num() == 0 && Marks.Num() > 0) Out.Add(Marks.Last());
		return Out;
	}

	void FGlyph::Add(const FMark& M)
	{
		// one mark per hand (the first bound wins), the left hand's drawn first
		for (const FMark& O : Marks) if (O.Hand == M.Hand) return;
		if (M.Hand == 0) Marks.Insert(M, 0);
		else Marks.Add(M);
	}

	bool KeyToMark(const FKey& Key, FMark& Out)
	{
		const FString N = Key.GetFName().ToString();
		const bool bLeft = N.Contains(TEXT("_Left_")), bRight = N.Contains(TEXT("_Right_"));
		if (!(N.StartsWith(TEXT("OculusTouch_")) || N.StartsWith(TEXT("MotionController_")) || N.StartsWith(TEXT("OculusRemote"))) || bLeft == bRight) return false;
		Out.Hand = bLeft ? 0 : 1;
		if (N.Contains(TEXT("Thumbstick")) && N.EndsWith(TEXT("Click"))) Out.Ctl = ECtl::StickClick;
		else if (N.Contains(TEXT("Thumbstick"))) Out.Ctl = ECtl::Stick;
		else if (N.Contains(TEXT("_X_")) || N.Contains(TEXT("_A_")) || N.Contains(TEXT("FaceButton1"))) Out.Ctl = ECtl::Lower;
		else if (N.Contains(TEXT("_Y_")) || N.Contains(TEXT("_B_")) || N.Contains(TEXT("FaceButton2"))) Out.Ctl = ECtl::Upper;
		else if (N.Contains(TEXT("Menu")) || N.Contains(TEXT("System")) || N.Contains(TEXT("Special"))) Out.Ctl = ECtl::Menu;
		else if (N.Contains(TEXT("Trigger"))) Out.Ctl = ECtl::Trigger;
		else if (N.Contains(TEXT("Grip"))) Out.Ctl = ECtl::Grip;
		else return false;
		return true;
	}

	FKey MarkToKey(const FMark& M)
	{
		const TCHAR* Side = M.Hand == 0 ? TEXT("Left") : TEXT("Right");
		const TCHAR* What = TEXT("");
		switch (M.Ctl)
		{
		case ECtl::Lower: What = M.Hand == 0 ? TEXT("X_Click") : TEXT("A_Click"); break;
		case ECtl::Upper: What = M.Hand == 0 ? TEXT("Y_Click") : TEXT("B_Click"); break;
		case ECtl::StickClick: What = TEXT("Thumbstick_Click"); break;
		case ECtl::Stick: What = TEXT("Thumbstick_Y"); break;
		case ECtl::Menu: What = TEXT("Menu_Click"); break;
		case ECtl::Trigger: What = TEXT("Trigger_Click"); break;
		case ECtl::Grip: What = TEXT("Grip_Click"); break;
		}
		return FKey(FName(*FString::Printf(TEXT("OculusTouch_%s_%s"), Side, What)));
	}

	FGlyph ForActions(const AOutlawsPlayer* P, std::initializer_list<const TCHAR*> Actions)
	{
		FGlyph G;
		const APlayerController* PC = P ? Cast<APlayerController>(P->GetController()) : nullptr;
		const int32 One = P ? P->OneHand() : -1;
		G.Main = int8(P ? P->MainHand() : 1);
		for (const TCHAR* A : Actions)
		{
			TArray<FInputActionKeyMapping> Keys;
			if (PC && PC->PlayerInput) Keys = PC->PlayerInput->GetKeysForAction(FName(A));
			else if (const UInputSettings* S = UInputSettings::GetInputSettings()) S->GetActionMappingByName(FName(A), Keys);
			for (const FInputActionKeyMapping& K : Keys)
			{
				FMark M;
				if (!KeyToMark(K.Key, M)) continue;
				if (One >= 0 && M.Hand != One) continue;      // (one controller: the other hand holds nothing)
				G.Add(M);
			}
		}
		return G;
	}

	FGlyph ForControl(const AOutlawsPlayer* P, ECtl Ctl, int32 Hand)
	{
		FGlyph G;
		const int32 One = P ? P->OneHand() : -1;
		if (One >= 0) Hand = One;
		G.Main = int8(P ? P->MainHand() : 1);
		for (int32 H = 0; H < 2; ++H) if (Hand < 0 || Hand == H) G.Add(FMark{ int8(H), Ctl });
		return G;
	}

	// ---- drawing ------------------------------------------------------------------------------------------------------

	static constexpr float OneW = 0.95f, Gap = 0.10f;

	float Width(const FGlyph& G, float Height)
	{
		const int32 N = G.Drawn().Num();
		return N == 0 ? 0.f : Height * (OneW * N + Gap * (N - 1));
	}

	struct FPen
	{
		UCanvas* C;
		bool bMask;
		void Poly(const TArray<FVector2D>& Pts, const FLinearColor& Col) const
		{
			if (Pts.Num() < 3) return;
			TArray<FCanvasUVTri> Tris;
			for (int32 i = 1; i + 1 < Pts.Num(); ++i)
			{
				FCanvasUVTri& T = Tris.AddDefaulted_GetRef();
				T.V0_Pos = Pts[0]; T.V1_Pos = Pts[i]; T.V2_Pos = Pts[i + 1];
				T.V0_Color = T.V1_Color = T.V2_Color = bMask ? FLinearColor::White : Col;
			}
			FCanvasTriangleItem Item(Tris, GWhiteTexture);
			Item.BlendMode = SE_BLEND_Translucent;
			C->DrawItem(Item);
		}
		void Ellipse(const FVector2D& At, float Rx, float Ry, const FLinearColor& Col) const
		{
			TArray<FVector2D> Pts;
			const int32 N = 28;
			for (int32 i = 0; i < N; ++i)
			{
				const float A = 2.f * PI * i / N;
				Pts.Add(At + FVector2D(FMath::Cos(A) * Rx, FMath::Sin(A) * Ry));
			}
			Poly(Pts, Col);
		}
		void Ring(const FVector2D& At, float R, float Thick, const FLinearColor& Col) const
		{
			TArray<FCanvasUVTri> Tris;
			const int32 N = 32;
			for (int32 i = 0; i < N; ++i)
			{
				const float A0 = 2.f * PI * i / N, A1 = 2.f * PI * (i + 1) / N;
				const FVector2D D0(FMath::Cos(A0), FMath::Sin(A0)), D1(FMath::Cos(A1), FMath::Sin(A1));
				const FVector2D I0 = At + D0 * (R - Thick * 0.5f), O0 = At + D0 * (R + Thick * 0.5f);
				const FVector2D I1 = At + D1 * (R - Thick * 0.5f), O1 = At + D1 * (R + Thick * 0.5f);
				for (int32 k = 0; k < 2; ++k)
				{
					FCanvasUVTri& T = Tris.AddDefaulted_GetRef();
					T.V0_Pos = k == 0 ? I0 : I1; T.V1_Pos = O0; T.V2_Pos = k == 0 ? I1 : O1;
					T.V0_Color = T.V1_Color = T.V2_Color = bMask ? FLinearColor::White : Col;
				}
			}
			FCanvasTriangleItem Item(Tris, GWhiteTexture);
			Item.BlendMode = SE_BLEND_Translucent;
			C->DrawItem(Item);
		}
	};

	// (a grey as it should look through the overlay material: it shows a written value w as about w^0.31, measured from the
	// first pictures, where sRGB-converted greys all came out near white)
	static FLinearColor Grey(uint8 V) { const float W = FMath::Pow(V / 255.f, 3.2f); return FLinearColor(W, W, W, 1.f); }

	/**
	 * One controller seen from the top, H tall, centred on At, drawn for the right hand (Mirror for the left): the tracking ring
	 * arching over the top, the trigger's block peeking out above it, the handle sloping down and out, the grip tab at the inner bottom
	 * edge of the face, and on the face the stick on the outer side, the upper and lower buttons (with their letters) on the
	 * inner side and the round menu or system button under the stick.  Light grey with a darker edge so it reads on the dark
	 * sheets.  The control to use is filled red, a button ringed in red too.
	 */
	static void DrawOne(const FPen& Pen, const FMark& M, const FVector2D& At, float H)
	{
		const float X = M.Hand == 0 ? -1.f : 1.f;
		auto P = [&](float Px, float Py) { return At + FVector2D(Px * X * H, Py * H); };
		const FLinearColor Edge = Grey(62), Shell = Grey(200), Face = Grey(218), FaceIn = Grey(240), Key = Grey(196),
			Ink = Grey(28), Red(1.f, 0.03f, 0.02f, 1.f), White = FLinearColor::White;
		const float E = 0.02f * H;                        // (the dark edge drawn under each shape: it reads on light and dark)
		const float RingT = 0.03f * H;
		auto Disc = [&](const FVector2D& C, float R, const FLinearColor& Fill) { Pen.Ellipse(C, R + E, R + E, Edge); Pen.Ellipse(C, R, R, Fill); };
		auto Letter = [&](const FVector2D& C, float R, const TCHAR* Ch, const FLinearColor& Col)
		{
			UFont* F = GEngine ? GEngine->GetMediumFont() : nullptr;
			if (!F || Pen.bMask) return;
			float XL = 0.f, YL = 0.f;
			Pen.C->TextSize(F, Ch, XL, YL);
			if (YL <= 0.f) return;
			FCanvasTextItem T(C, FText::FromString(Ch), F, Col);
			T.Scale = FVector2D(R * 1.35f / YL, R * 1.35f / YL);
			T.bCentreX = T.bCentreY = true;
			T.BlendMode = SE_BLEND_Translucent;
			Pen.C->DrawItem(T);
		};
		const bool bTrig = M.Ctl == ECtl::Trigger, bGrip = M.Ctl == ECtl::Grip;
		// behind: the trigger, a small block under the ring's arch with only its top showing over it (the body's colour, red
		// when it is the control to use; user 15 Oct: it was a ring round the spot), then the arch
		{
			const float L = -0.15f, R = 0.05f, T = -0.565f, B = -0.33f;      // (the face reaches -0.476: about 0.09 shows)
			Pen.Poly({ P(L, T) + FVector2D(-E * X, -E), P(R, T) + FVector2D(E * X, -E), P(R, B) + FVector2D(E * X, 0.f), P(L, B) + FVector2D(-E * X, 0.f) }, Edge);
			Pen.Poly({ P(L, T), P(R, T), P(R, B), P(L, B) }, bTrig ? Red : Shell);
		}
		Pen.Ellipse(P(0.f, -0.25f), 0.46f * H + E, 0.155f * H + E, Edge);
		Pen.Ellipse(P(0.f, -0.25f), 0.46f * H, 0.155f * H, Shell);
		// the handle, down and out
		{
			const TArray<FVector2D> Hd = { P(-0.164f, 0.08f), P(0.257f, -0.034f), P(0.064f, 0.455f), P(-0.152f, 0.455f) };
			const TArray<FVector2D> HdE = { P(-0.164f - 0.012f, 0.08f), P(0.257f + 0.014f, -0.034f), P(0.064f + 0.012f, 0.455f + 0.006f), P(-0.152f - 0.012f, 0.455f + 0.006f) };
			Pen.Poly(HdE, Edge);
			Pen.Ellipse(P(-0.045f, 0.455f), 0.109f * H + E, 0.05f * H + E, Edge);
			Pen.Poly(Hd, Shell);
			Pen.Ellipse(P(-0.045f, 0.455f), 0.109f * H, 0.05f * H, Shell);
		}
		// the grip tab at the inner bottom edge of the face: the body's colour, red when it is the control to use
		{
			const TArray<FVector2D> Tab = { P(-0.205f, 0.12f), P(-0.145f, 0.12f), P(-0.145f, 0.265f), P(-0.205f, 0.265f) };
			const TArray<FVector2D> TabE = { P(-0.217f, 0.11f), P(-0.133f, 0.11f), P(-0.133f, 0.277f), P(-0.217f, 0.277f) };
			Pen.Poly(TabE, Edge);
			Pen.Poly(Tab, bGrip ? Red : Shell);
		}
		// the face
		const FVector2D FaceAt = P(-0.055f, -0.136f);
		Disc(FaceAt, 0.32f * H, Face);
		Pen.Ellipse(FaceAt, 0.27f * H, 0.27f * H, FaceIn);
		// (the buttons and stick a little larger than the real ones, so they read when the drawing is small)
		const FVector2D StickAt = P(0.06f, -0.2f), UpperAt = P(-0.165f, -0.19f), LowerAt = P(-0.10f, -0.035f), MenuAt = P(0.11f, 0.0f);
		const float ButtonR = 0.064f * H, StickR = 0.11f * H;
		const bool bStick = M.Ctl == ECtl::Stick || M.Ctl == ECtl::StickClick;
		Disc(StickAt, StickR, Grey(150));
		Pen.Ellipse(StickAt, 0.078f * H, 0.078f * H, Grey(185));
		Pen.Ellipse(StickAt, 0.05f * H, 0.05f * H, Grey(225));
		Disc(UpperAt, ButtonR, M.Ctl == ECtl::Upper ? Red : Key);
		Disc(LowerAt, ButtonR, M.Ctl == ECtl::Lower ? Red : Key);
		Disc(MenuAt, ButtonR, M.Ctl == ECtl::Menu ? Red : Key);
		Letter(UpperAt, ButtonR, M.Hand == 0 ? TEXT("Y") : TEXT("B"), M.Ctl == ECtl::Upper ? White : Ink);
		Letter(LowerAt, ButtonR, M.Hand == 0 ? TEXT("X") : TEXT("A"), M.Ctl == ECtl::Lower ? White : Ink);
		// the mark on a button: a ring round it (the filled red above; the trigger and the grip are only filled, a dot on the stick)
		switch (M.Ctl)
		{
		case ECtl::Lower: Pen.Ring(LowerAt, ButtonR + 0.035f * H, RingT, Red); break;
		case ECtl::Upper: Pen.Ring(UpperAt, ButtonR + 0.035f * H, RingT, Red); break;
		case ECtl::Menu: Pen.Ring(MenuAt, ButtonR + 0.035f * H, RingT, Red); break;
		default: break;
		}
		if (bStick)
		{
			Pen.Ring(StickAt, StickR + 0.03f * H, RingT, Red);
			Pen.Ellipse(StickAt, 0.035f * H, 0.035f * H, Red);
			if (M.Ctl == ECtl::Stick)
			{
				// pushed: small arrows above and below the ring
				const float Y0 = StickR + 0.06f * H, Y1 = StickR + 0.12f * H, W = 0.04f * H;
				Pen.Poly({ StickAt + FVector2D(0.f, -Y1), StickAt + FVector2D(W, -Y0), StickAt + FVector2D(-W, -Y0) }, Red);
				Pen.Poly({ StickAt + FVector2D(0.f, Y1), StickAt + FVector2D(-W, Y0), StickAt + FVector2D(W, Y0) }, Red);
			}
		}
	}

	FBox2D Draw(UCanvas* C, const FGlyph& G, const FVector2D& Centre, float Height, bool bMask)
	{
		if (!C || G.IsEmpty()) return FBox2D(ForceInit);
		const FPen Pen{ C, bMask };
		const float W = Width(G, Height);
		float X = Centre.X - W * 0.5f + OneW * Height * 0.5f;
		for (const FMark& M : G.Drawn())
		{
			DrawOne(Pen, M, FVector2D(X, Centre.Y), Height);
			X += (OneW + Gap) * Height;
		}
		return FBox2D(Centre - FVector2D(W * 0.5f, Height * 0.5f), Centre + FVector2D(W * 0.5f, Height * 0.5f));
	}

	FBox2D DrawWithLabel(UCanvas* C, const FGlyph& G, const FString& Label, UFont* Font, const FVector2D& Centre, float GlyphH,
		const FLinearColor& Ink, const FLinearColor& Outline, bool bMask, float TextScale)
	{
		if (!C) return FBox2D(ForceInit);
		float XL = 0.f, YL = 0.f;
		if (Font && !Label.IsEmpty()) C->TextSize(Font, Label, XL, YL);
		XL *= TextScale;
		YL *= TextScale;
		const float GW = Width(G, GlyphH), Sp = GW > 0.f && XL > 0.f ? GlyphH * 0.15f : 0.f;
		const float Left = Centre.X - (GW + Sp + XL) * 0.5f;
		FBox2D Box(ForceInit);
		if (GW > 0.f) Box += Draw(C, G, FVector2D(Left + GW * 0.5f, Centre.Y), GlyphH, bMask);
		if (XL > 0.f)
		{
			FCanvasTextItem T(FVector2D(Left + GW + Sp + XL * 0.5f, Centre.Y), FText::FromString(Label), Font, bMask ? FLinearColor::White : Ink);
			T.Scale = FVector2D(TextScale, TextScale);
			T.bCentreX = T.bCentreY = true;
			T.bOutlined = true;
			T.OutlineColor = bMask ? FLinearColor::White : Outline;
			T.BlendMode = SE_BLEND_Translucent;
			C->DrawItem(T);
			const float X0 = Left + GW + Sp;
			Box += FBox2D(FVector2D(X0, Centre.Y - YL * 0.31f), FVector2D(X0 + XL, Centre.Y + YL * 0.31f));
		}
		return Box;
	}

	// the stacked prompt: the drawing's top this far under the letters, in drawing heights; the parts this far apart
	static constexpr float StackColGap = 0.4f;

	static float TextHeight(UCanvas* C, UFont* Font, float TextScale)
	{
		float XL = 0.f, YL = 0.f;
		if (C && Font) C->TextSize(Font, TEXT("Back"), XL, YL);
		return YL * TextScale;
	}

	static float PartWidth(UCanvas* C, const FPart& Pt, UFont* Font, float GlyphH, float TextScale)
	{
		float XL = 0.f, YL = 0.f;
		if (C && Font && !Pt.Label.IsEmpty()) C->TextSize(Font, Pt.Label, XL, YL);
		const float GW = Width(Pt.Glyph, GlyphH);
		// (REQ-BACKRIGHT) a bRight part: its label, a gap (DrawWithLabel's), then its drawing, on the line
		if (Pt.bRight) return XL * TextScale + (GW > 0.f && XL > 0.f ? GlyphH * 0.15f : 0.f) + GW;
		return FMath::Max(XL * TextScale, GW);
	}

	float StackedWidth(UCanvas* C, const TArray<FPart>& Parts, UFont* Font, float GlyphH, float TextScale)
	{
		float W = 0.f;
		for (int32 i = 0; i < Parts.Num(); ++i) W += PartWidth(C, Parts[i], Font, GlyphH, TextScale) + (i > 0 ? GlyphH * StackColGap : 0.f);
		return W;
	}

	float StackedBelow(UCanvas* C, UFont* Font, float GlyphH, float TextScale)
	{
		return TextHeight(C, Font, TextScale) * 0.31f + GlyphH * (StackGap + 1.f);
	}

	FBox2D DrawStacked(UCanvas* C, const TArray<FPart>& Parts, UFont* Font, const FVector2D& Centre, float GlyphH,
		const FLinearColor& Ink, const FLinearColor& Outline, bool bMask, float TextScale, TArray<FBox2D>* OutPartBoxes)
	{
		FBox2D Box(ForceInit);
		if (OutPartBoxes) OutPartBoxes->Reset();
		if (!C) return Box;
		const float YL = TextHeight(C, Font, TextScale);
		const float GlyphY = Centre.Y + YL * 0.31f + GlyphH * (StackGap + 0.5f);
		float X = Centre.X - StackedWidth(C, Parts, Font, GlyphH, TextScale) * 0.5f;
		for (const FPart& Pt : Parts)
		{
			const float W = PartWidth(C, Pt, Font, GlyphH, TextScale);
			FBox2D PartBox(ForceInit);
			float XL = 0.f, TY = 0.f;
			if (Font && !Pt.Label.IsEmpty()) C->TextSize(Font, Pt.Label, XL, TY);
			XL *= TextScale;
			// (REQ-BACKRIGHT) a bRight part's label at the left of its width, its drawing at the right; else both centred on it
			const float Mid = Pt.bRight ? X + XL * 0.5f : X + W * 0.5f;
			const float GW = Width(Pt.Glyph, GlyphH);
			const FVector2D GlyphAt = Pt.bRight ? FVector2D(X + W - GW * 0.5f, Centre.Y) : FVector2D(X + W * 0.5f, GlyphY);
			if (Font && !Pt.Label.IsEmpty())
			{
				FCanvasTextItem T(FVector2D(Mid, Centre.Y), FText::FromString(Pt.Label), Font, bMask ? FLinearColor::White : Pt.bHot ? HotInk() : Ink);
				T.Scale = FVector2D(TextScale, TextScale);
				T.bCentreX = T.bCentreY = true;
				T.bOutlined = true;
				T.OutlineColor = bMask ? FLinearColor::White : Pt.bHot ? FLinearColor::Black : Outline;
				T.BlendMode = SE_BLEND_Translucent;
				C->DrawItem(T);
				PartBox += FBox2D(FVector2D(Mid - XL * 0.5f, Centre.Y - YL * 0.31f), FVector2D(Mid + XL * 0.5f, Centre.Y + YL * 0.31f));
			}
			if (!Pt.Glyph.IsEmpty()) PartBox += Draw(C, Pt.Glyph, GlyphAt, GlyphH, bMask);
			if (PartBox.bIsValid) Box += PartBox;
			if (OutPartBoxes) OutPartBoxes->Add(PartBox);
			X += W + GlyphH * StackColGap;
		}
		return Box;
	}

	FLinearColor HotInk() { return FLinearColor::FromSRGBColor(FColor(0xe9, 0x93, 0x1c)); }

	bool SplitPrompt(const AOutlawsPlayer* P, const FString& Line, FGlyph& OutGlyph, FString& OutRest)
	{
		struct FPair { const TCHAR* Prefix; const TCHAR* A; const TCHAR* B; };
		static const FPair Pairs[] = {
			{ TEXT("A / X: "), TEXT("OLVR_A"), TEXT("OLVR_X") }, { TEXT("X / A: "), TEXT("OLVR_X"), TEXT("OLVR_A") },
			{ TEXT("B / Y: "), TEXT("OLVR_B"), TEXT("OLVR_Y") }, { TEXT("Y / B: "), TEXT("OLVR_Y"), TEXT("OLVR_B") },
			{ TEXT("A or X: "), TEXT("OLVR_A"), TEXT("OLVR_X") }, { TEXT("B or Y: "), TEXT("OLVR_B"), TEXT("OLVR_Y") },
			{ TEXT("A: "), TEXT("OLVR_A"), nullptr }, { TEXT("B: "), TEXT("OLVR_B"), nullptr },
			{ TEXT("X: "), TEXT("OLVR_X"), nullptr }, { TEXT("Y: "), TEXT("OLVR_Y"), nullptr },
		};
		for (const FPair& Pr : Pairs)
		{
			if (!Line.StartsWith(Pr.Prefix, ESearchCase::CaseSensitive)) continue;
			OutGlyph = Pr.B ? ForActions(P, { Pr.A, Pr.B }) : ForActions(P, { Pr.A });
			OutRest = Line.Mid(FCString::Strlen(Pr.Prefix));
			if (OutRest.Len() > 0) OutRest[0] = FChar::ToUpper(OutRest[0]);
			return true;
		}
		return false;
	}
}

// ---- the plate -------------------------------------------------------------------------------------------------------------

UOutlawsGlyphPlate::UOutlawsGlyphPlate()
{
	SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SetCastShadow(false);
	SetGenerateOverlapEvents(false);
}

void UOutlawsGlyphPlate::Init(UMaterialInterface* Overlay, UFont* InFont, UFont* InTitleFont, float InWidthCm, int32 InPixH, float InGlyphCm)
{
	Font = InFont;
	TitleFont = InTitleFont ? InTitleFont : InFont;
	WidthCm = InWidthCm;
	GlyphCm = InGlyphCm;
	if (Tex) return;
	PixH = InPixH;
	SetStaticMesh(LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane")));
	Tex = NewObject<UTextureRenderTarget2D>(this);
	Tex->RenderTargetFormat = RTF_RGBA8_SRGB;
	Tex->TargetGamma = 1.f;
	Tex->ClearColor = FLinearColor::Transparent;
	Tex->InitAutoFormat(PixW, PixH);
	Mask = NewObject<UTextureRenderTarget2D>(this);
	Mask->RenderTargetFormat = RTF_RGBA8;
	Mask->TargetGamma = 1.f;
	Mask->ClearColor = FLinearColor::Black;
	Mask->InitAutoFormat(PixW, PixH);
	if (Overlay)
	{
		Mid = UMaterialInstanceDynamic::Create(Overlay, this);
		Mid->SetTextureParameterValue(TEXT("Tex"), Tex);
		Mid->SetTextureParameterValue(TEXT("Mask"), Mask);
		SetMaterial(0, Mid);
	}
	SetTranslucentSortPriority(9);
	SetRenderCustomDepth(true);          // (stencil 3: no ink line over it, as the notices)
	SetCustomDepthStencilValue(3);
}

static bool RowsSame(const TArray<FOutlawsGlyphRow>& A, const TArray<FOutlawsGlyphRow>& B)
{
	if (A.Num() != B.Num()) return false;
	for (int32 i = 0; i < A.Num(); ++i)
	{
		if (!(A[i].Glyph == B[i].Glyph) || A[i].Text != B[i].Text || A[i].bTitle != B[i].bTitle || A[i].bBeside != B[i].bBeside || A[i].bRight != B[i].bRight || A[i].More.Num() != B[i].More.Num()) return false;
		for (int32 k = 0; k < A[i].More.Num(); ++k) if (!(A[i].More[k].Glyph == B[i].More[k].Glyph) || A[i].More[k].Label != B[i].More[k].Label || A[i].More[k].bHot != B[i].More[k].bHot || A[i].More[k].bRight != B[i].More[k].bRight) return false;
	}
	return true;
}

bool UOutlawsGlyphPlate::SetRows(const TArray<FOutlawsGlyphRow>& InRows)
{
	if (RowsSame(Rows, InRows)) return false;
	Rows = InRows;
	Redraw();
	return true;
}

void UOutlawsGlyphPlate::Redraw()
{
	if (!Tex || !Mask) return;
	// 1024 px across: WidthCm wide in the world; the drawings GlyphCm tall (user 15 Oct: the size the death screen's are seen at,
	// everywhere).  A text row 72 px, a title 84 px; a prompt's row its text with the drawing under it (an instruction line's
	// beside it)
	const float GH = GlyphCm * PixW / WidthCm;
	const float PlateTextRowH = 72.f, PlateTitleH = 84.f;
	// (the sheets keep the size Init gave them: a resize between the two passes left them out of step)
	HeightCmNow = WidthCm * PixH / float(PixW);
	SetRelativeScale3D(FVector(WidthCm / 100.0, HeightCmNow / 100.0, 1.0));
	const FLinearColor Ink = FLinearColor::FromSRGBColor(FColor(245, 230, 190)), Outline = FLinearColor::Black;
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		UTextureRenderTarget2D* RT = Pass == 0 ? Tex.Get() : Mask.Get();
		UKismetRenderingLibrary::ClearRenderTarget2D(this, RT, Pass == 0 ? FLinearColor::Transparent : FLinearColor::Black);
		UCanvas* C = nullptr;
		FVector2D Size;
		FDrawToRenderTargetContext Ctx;
		UKismetRenderingLibrary::BeginDrawCanvasToRenderTarget(this, RT, C, Size, Ctx);
		if (C)
		{
			// each row's parts, text scale (a long line shrinks to the sheet), height and the line's middle in it
			struct FLaid { TArray<OutlawsGlyphs::FPart> Parts; float TS = 1.f; float Rh = 0.f; float Mid = 0.f; float Half = 0.f; };
			TArray<FLaid> Laid;
			float H = 0.f;
			for (const FOutlawsGlyphRow& R : Rows)
			{
				FLaid& L = Laid.AddDefaulted_GetRef();
				L.Parts.Add({ R.Glyph, R.Text });
				L.Parts[0].bRight = R.bRight;
				L.Parts.Append(R.More);
				UFont* F = R.bTitle ? TitleFont.Get() : Font.Get();
				const bool bGlyph = L.Parts.ContainsByPredicate([](const OutlawsGlyphs::FPart& P) { return !P.Glyph.IsEmpty(); });
				float XL = 0.f, YL = 0.f;
				if (F && !R.Text.IsEmpty()) C->TextSize(F, R.Text, XL, YL);
				const float Room = PixW - 40.f - (R.bBeside ? OutlawsGlyphs::Width(R.Glyph, GH) + GH * 0.15f : 0.f);
				if (XL > Room) L.TS = Room / XL;
				L.Half = YL * L.TS * 0.31f;          // (the letters' half height, as the drawn boxes take it)
				if (R.bTitle || !bGlyph) { L.Rh = R.bTitle ? PlateTitleH : PlateTextRowH; L.Mid = L.Rh * 0.5f; }
				else if (R.bBeside) { L.Rh = FMath::Max(GH + 10.f, PlateTextRowH); L.Mid = L.Rh * 0.5f; }
				else
				{
					const float Line = OutlawsGlyphs::StackedBelow(C, F, GH, L.TS) - GH * (1.f + OutlawsGlyphs::StackGap);      // (the letters' half height)
					L.Mid = Line + 8.f;
					L.Half = Line;
					L.Rh = L.Mid + OutlawsGlyphs::StackedBelow(C, F, GH, L.TS) + 10.f;
				}
				H += L.Rh;
			}
			// (REQ-DEATHGAP, user 15 Oct) a title over another row: the title's row as tall as its letters, then the gap asked for
			// down to the next row's letters
			if (TitleGapCm && Rows.Num() > 1 && Rows[0].bTitle)
			{
				const float GapPx = FMath::Max(0.f, TitleGapCm(C)) * PixW / WidthCm;
				H -= Laid[0].Rh;
				Laid[0].Mid = Laid[0].Half;
				Laid[0].Rh = FMath::Max(1.f, Laid[0].Half * 2.f + GapPx - (Laid[1].Mid - Laid[1].Half));
				H += Laid[0].Rh;
			}
			// the rows' block centred on the sheet
			float Y = (PixH - H) * 0.5f;
			if (Pass == 0) PartBoxes.Reset();
			for (int32 i = 0; i < Rows.Num(); ++i)
			{
				const FOutlawsGlyphRow& R = Rows[i];
				const FLaid& L = Laid[i];
				UFont* F = R.bTitle ? TitleFont.Get() : Font.Get();
				const FVector2D At(PixW * 0.5f, Y + L.Mid);
				const bool bGlyph = L.Parts.ContainsByPredicate([](const OutlawsGlyphs::FPart& P) { return !P.Glyph.IsEmpty(); });
				TArray<FBox2D> Boxes;
				if (R.bBeside || !bGlyph) Boxes.Add(OutlawsGlyphs::DrawWithLabel(C, R.Glyph, R.Text, F, At, GH, Ink, Outline, Pass == 1, L.TS));
				else OutlawsGlyphs::DrawStacked(C, L.Parts, F, At, GH, Ink, Outline, Pass == 1, L.TS, &Boxes);
				if (Pass == 0) PartBoxes.Add(MoveTemp(Boxes));
				Y += L.Rh;
			}
			if (Pass == 0)
			{
				LastTitleGapCm = -1.f;
				if (Rows.Num() > 1 && Rows[0].bTitle && PartBoxes.Num() > 1 && PartBoxes[0].Num() > 0 && PartBoxes[1].Num() > 0 && PartBoxes[0][0].bIsValid && PartBoxes[1][0].bIsValid)
					LastTitleGapCm = float(PartBoxes[1][0].Min.Y - PartBoxes[0][0].Max.Y) * WidthCm / PixW;
			}
			if (Pass == 0 && H > PixH) UE_LOG(LogOutlaws, Warning, TEXT("OLGLYPH plate: rows %.0f px tall on a %d px sheet"), H, PixH);
		}
		UKismetRenderingLibrary::EndDrawCanvasToRenderTarget(this, Ctx);
	}
}

bool UOutlawsGlyphPlate::PartAt(const FVector2D& Uv, int32& OutRow, int32& OutPart, float PadPx) const
{
	const FVector2D Px(Uv.X * PixW, Uv.Y * PixH);
	for (int32 r = 0; r < PartBoxes.Num(); ++r)
	{
		for (int32 p = 0; p < PartBoxes[r].Num(); ++p)
		{
			const FBox2D& B = PartBoxes[r][p];
			if (B.bIsValid && B.ExpandBy(PadPx).IsInside(Px)) { OutRow = r; OutPart = p; return true; }
		}
	}
	return false;
}

bool UOutlawsGlyphPlate::PartCentreUv(int32 Row, int32 Part, FVector2D& OutUv) const
{
	if (!PartBoxes.IsValidIndex(Row) || !PartBoxes[Row].IsValidIndex(Part) || !PartBoxes[Row][Part].bIsValid) return false;
	const FVector2D C = PartBoxes[Row][Part].GetCenter();
	OutUv = FVector2D(C.X / PixW, C.Y / PixH);
	return true;
}

void UOutlawsGlyphPlate::PlaceFacing(const FVector& At, const FQuat& Frame, bool bRelative)
{
	// the engine plane faces +Z: turned so it faces the frame's +X with the picture upright (as the menu sheet)
	static const FQuat Turn = FRotationMatrix::MakeFromXY(FVector(0.0, -1.0, 0.0), FVector(0.0, 0.0, -1.0)).ToQuat();
	if (bRelative) SetRelativeLocationAndRotation(At, Frame * Turn);
	else SetWorldLocationAndRotation(At, Frame * Turn);
}
