#include "OutlawsViewCone.h"
#include "OutlawsRuntime.h"
#include "Engine/Engine.h"
#include "StereoRendering.h"
#include "Components/PrimitiveComponent.h"

namespace
{
	constexpr float ViewConeMargin = 10.f;        // degrees round every side of what the headset reports
	constexpr float ViewConeFallback = 60.f;      // half-angles with no headset: 120 across, 120 up and down

	// the four edges of one eye's projection, as tangents in its view space (x right, y up, z forward)
	bool EyeTangents(int32 Eye, double& L, double& R, double& U, double& D)
	{
		const FMatrix M = GEngine->StereoRenderingDevice->GetStereoProjectionMatrix(Eye);
		if (FMath::Abs(M.M[0][0]) < 1e-6 || FMath::Abs(M.M[1][1]) < 1e-6) return false;
		L = (-1.0 - M.M[2][0]) / M.M[0][0];
		R = (1.0 - M.M[2][0]) / M.M[0][0];
		U = (1.0 - M.M[2][1]) / M.M[1][1];
		D = (-1.0 - M.M[2][1]) / M.M[1][1];
		return R > L && U > D;
	}
}

FOutlawsViewCone FOutlawsViewCone::Get(bool bLog)
{
	FOutlawsViewCone C;
	C.Left = C.Right = C.Up = C.Down = ViewConeFallback;
	if (GEngine && GEngine->StereoRenderingDevice.IsValid() && GEngine->StereoRenderingDevice->IsStereoEnabled())
	{
		double L0, R0, U0, D0, L1, R1, U1, D1;
		if (EyeTangents(eSSE_LEFT_EYE, L0, R0, U0, D0) && EyeTangents(eSSE_RIGHT_EYE, L1, R1, U1, D1))
		{
			// the OpenXR plugin answers a square 45 degrees each way for an eye it has no pose for yet (before the first
			// frame): not the headset's, so the fallback until it has one
			const bool bPlaceholder = FMath::IsNearlyEqual(R0, 1.0, 1e-4) && FMath::IsNearlyEqual(-L0, 1.0, 1e-4)
				&& FMath::IsNearlyEqual(U0, 1.0, 1e-4) && FMath::IsNearlyEqual(-D0, 1.0, 1e-4);
			if (!bPlaceholder)
			{
				C.Left = FMath::RadiansToDegrees(FMath::Atan(-FMath::Min(L0, L1))) + ViewConeMargin;
				C.Right = FMath::RadiansToDegrees(FMath::Atan(FMath::Max(R0, R1))) + ViewConeMargin;
				C.Up = FMath::RadiansToDegrees(FMath::Atan(FMath::Max(U0, U1))) + ViewConeMargin;
				C.Down = FMath::RadiansToDegrees(FMath::Atan(-FMath::Min(D0, D1))) + ViewConeMargin;
				C.bFromHmd = true;
			}
		}
	}
	static int32 LastSource = -1;
	if (bLog || LastSource != int32(C.bFromHmd))
	{
		LastSource = int32(C.bFromHmd);
		UE_LOG(LogOutlaws, Log, TEXT("OLRT view cone: left %.1f right %.1f up %.1f down %.1f degrees (%s)"),
			C.Left, C.Right, C.Up, C.Down, C.bFromHmd ? TEXT("HMD, both eyes + 10 margin") : TEXT("fallback"));
	}
	return C;
}

bool FOutlawsViewCone::Contains(const FVector& Eye, const FQuat& Rot, const FVector& Point) const
{
	const FVector P = Rot.UnrotateVector(Point - Eye);     // x forward, y right, z up
	if (P.X <= 0.0) return false;
	const double H = FMath::RadiansToDegrees(FMath::Atan2(P.Y, P.X));
	const double V = FMath::RadiansToDegrees(FMath::Atan2(P.Z, P.X));
	return H >= -Left && H <= Right && V >= -Down && V <= Up;
}

int32 FOutlawsViewCone::WidenedCount = 0;

void FOutlawsViewCone::WidenHandBounds(UPrimitiveComponent* C, float Scale)
{
	if (!C) return;
	if (C->BoundsScale < Scale) C->SetBoundsScale(Scale);
	++WidenedCount;
}

void FOutlawsViewCone::LogClipDiagnostics()
{
	// The engine culls and projects with the stereo device's own per-eye matrices (the cone below, less its margin), so
	// the frustum is not narrower than the eyes.  What cut parts off early was the near plane: the held gear and the
	// text plates are pulled along each line of sight to a few cm from the eye, and a point at radius r and angle a off
	// axis has view depth r cos a, behind a near plane deeper than that.
	const FOutlawsViewCone C = Get(true);
	const float Edge = FMath::Max(FMath::Max(C.Left, C.Right), FMath::Max(C.Up, C.Down)) - (C.bFromHmd ? ViewConeMargin : 0.f);
	auto ClipAt = [](float PulledCm) { return GNearClippingPlane >= PulledCm ? 0.f : FMath::RadiansToDegrees(FMath::Acos(GNearClippingPlane / PulledCm)); };
	UE_LOG(LogOutlaws, Log, TEXT("OLRT fovclip: near plane %.1f cm; text plates (>= 10 cm) clip beyond %.0f deg, held gear (>= 12 cm) beyond %.0f deg; widest eye edge %.0f deg (%s); %d hand/wrist parts with widened bounds"),
		GNearClippingPlane, ClipAt(10.f), ClipAt(12.f), Edge, C.bFromHmd ? TEXT("HMD") : TEXT("fallback"), WidenedCount);
}
