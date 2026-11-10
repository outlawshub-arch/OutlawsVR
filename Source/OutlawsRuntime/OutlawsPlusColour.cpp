#include "OutlawsPlusColour.h"
#include "OutlawsFlags.h"
#include "Misc/Paths.h"

namespace
{
	bool GPlusRed = true;

	/** The mod's colour for one item's cross: its hue, and value and saturation from the blue texel's (a fit over the
	 *  changed texels of the mod's frames against the stock ones). */
	struct FPlusLook { float Hue; float VMul; float VAdd; float SMax; };
	constexpr FPlusLook RedLook = { 0.f, 0.57f, 0.12f, 0.70f };
	constexpr FPlusLook OrangeLook = { 31.f, 0.80f, 0.155f, 1.f };

	float Ramp(float X, float Lo, float Hi) { return FMath::Clamp((X - Lo) / (Hi - Lo), 0.f, 1.f); }
}

bool OutlawsPlusColour::RedChosen() { return GPlusRed; }
void OutlawsPlusColour::SetRedChosen(bool bRed) { GPlusRed = bRed; }

bool OutlawsPlusColour::OldFlag()
{
	static const bool bOld = OLFlags::Param(TEXT("OLOldPlusColour"));
	return bOld;
}

bool OutlawsPlusColour::Active() { return GPlusRed && !OldFlag(); }

bool OutlawsPlusColour::IsPlusItem(const FString& Name)
{
	const FString Stem = FPaths::GetBaseFilename(Name).ToUpper();
	return Stem == TEXT("GCANTEEN") || Stem == TEXT("GDOCBAG") || Stem == TEXT("IDOCBAG");
}

int32 OutlawsPlusColour::RecolourPixels(const FString& Name, TArray<FColor>& Px)
{
	if (!IsPlusItem(Name)) return 0;
	const FPlusLook& Look = FPaths::GetBaseFilename(Name).ToUpper() == TEXT("IDOCBAG") ? OrangeLook : RedLook;
	int32 Changed = 0;
	for (FColor& C : Px)
	{
		if (C.A == 0) continue;
		const float R = C.R / 255.f, G = C.G / 255.f, B = C.B / 255.f;
		const float Mx = FMath::Max3(R, G, B), Mn = FMath::Min3(R, G, B), D = Mx - Mn;
		if (D <= 1e-4f || Mx != B) continue;          // (blue is the largest channel in every blue the crosses have)
		const float Hue = 60.f * ((R - G) / D + 4.f);   // (hue with blue largest: 180 to 300)
		const float Sat = D / Mx;
		// the cross's blues are hue 200 to 240 at saturation 0.6 to 1; a soft edge either side keeps the outline's blend
		const float W = FMath::Min(Ramp(Hue, 175.f, 190.f), 1.f - Ramp(Hue, 250.f, 265.f)) * Ramp(Sat, 0.18f, 0.32f) * Ramp(Mx, 0.06f, 0.15f);
		if (W <= 0.f) continue;
		const FLinearColor Hsv(Look.Hue, FMath::Min(Sat, Look.SMax), FMath::Clamp(Look.VMul * Mx + Look.VAdd, 0.f, 1.f), 1.f);
		const FLinearColor Rgb = Hsv.HSVToLinearRGB();      // (no gamma: the values stay the stored ones, as the fit's were)
		const FColor N(uint8(FMath::RoundToInt(FMath::Lerp(R, Rgb.R, W) * 255.f)), uint8(FMath::RoundToInt(FMath::Lerp(G, Rgb.G, W) * 255.f)),
			uint8(FMath::RoundToInt(FMath::Lerp(B, Rgb.B, W) * 255.f)), C.A);
		if (N != C) { C = N; ++Changed; }
	}
	return Changed;
}

int32 OutlawsPlusColour::Apply(const FString& Name, TArray<FColor>& Px)
{
	return Active() ? RecolourPixels(Name, Px) : 0;
}
