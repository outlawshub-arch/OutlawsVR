// REQ-LANTERNRANGE (user 2026-11-03: "the lantern's light reaches too far; in narrow spots it feels like a lit room, not
// darkness creeping in a short way ahead"): -OLShots -OLLevel=shsimms -OLNoFrontEnd -OLLanternRangeProbe [-OLOldLantern].
// (1) the curve: the lit lantern (full oil) on a wall facing it in an unlit sector at 0, 0.5, 1, 2, 3, 4, 6, 8 and 12 m,
// ours through the materials' OLLights sum (C0 x (1 - d / r)^2, shown as its 1 / 2.2 power, over a minute of the flicker)
// against the original's: 31 + the steady flicker's 1..4 - the depth map at 4 x z units, less 3 levels every 32 units
// [EXE 0x1403012a0], the map rebuilt as Main_LoadPalettes builds it [EXE 0x14009581a] and the flicker as its rule draws it
// [EXE 0x1402fd740]; within 1.5 levels of 31 at each distance (-OLOldLantern: today's numbers beside the original's, and the
// 22 m and 0.75 in use).  (2) the drawn level: a white wall of M_OLR_World with no sector light at each distance from 0.5 m
// along a clear line, the screen read where it shows, against that frame's L0 / C0 through the same sum (within 0.75 of a
// level); the lantern put out (black) and Brightness 1.6 at 4 m (the 1.0 picture ^ (1 / 1.6)).  (3) shots of a narrow
// corridor, a dark room and the night outside (shsimms sectors 152, 184 and 186, or -OLLanternRangeSpots=a,b,c), the lantern
// lit: lanternrange_<spot>_<new|old>_<pc|q3|q2>.png in Saved\OLShots.  Lines "OLSHOT lanternrange check PASS|FAIL: ..." and
// "OLSHOT lanternrange RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsLevelActor.h"
#include "OutlawsInfRuntime.h"
#include "OutlawsGeometry.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsShotTests.h"
#include "OutlawsDeviceProfile.h"
#include "ProceduralMeshComponent.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialParameterCollectionInstance.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Engine.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "Components/CapsuleComponent.h"
#include "Camera/CameraComponent.h"
#include "Camera/PlayerCameraManager.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformMisc.h"
#include "Math/RandomStream.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "ImageUtils.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

namespace
{
	const float LrDist[] = { 0.f, 0.5f, 1.f, 2.f, 3.f, 4.f, 6.f, 8.f, 12.f };     // metres
	constexpr int32 LrNum = UE_ARRAY_COUNT(LrDist);
	constexpr float LrTol = 1.5f;              // levels of 31: the curve
	constexpr float LrPixTol = 0.75f;          // levels of 31: the screen against the frame's sum

	// the original's depth map [EXE 0x14009581a..0x140095887]: buf[127 - i] = (int)clamp(31 - i * 0.23622, 0, 31)
	void LrDepthMap(uint8 Out[128])
	{
		for (int32 i = 0; i < 128; ++i) Out[127 - i] = uint8(FMath::Clamp(31.f - float(i) * 0.23622f, 0.f, 31.f));
	}

	// the original's light at view depth z (metres) with the steady flicker K (1..4) in a sector of AMBIENT A [EXE 0x1403012a0]
	float LrOriginal(const uint8 Map[128], float DepthM, int32 K, int32 A)
	{
		if (A >= 31) return 31.f;
		const int32 Fx = int32(DepthM / 0.3f * 65536.f);
		const int32 Cue = FMath::Max(0, 31 - (int32(Map[FMath::Min(Fx >> 14, 127)]) - K));
		int32 Light = FMath::Max(Cue, A) - ((Fx >> 20) + (Fx >> 21));
		Light = FMath::Max(Light, (A >> 1) + (A >> 2) + (A >> 3));
		return Light >= 31 ? 31.f : float(FMath::Max(Light, 0));
	}

	// ours: the OLLights sum (ue_olue_lights_material.py) over the sector's light, as the screen shows it (^ 1 / 2.2), per channel
	FLinearColor LrOursLinear(const FLinearColor& Pos, const FLinearColor& Col, const FVector& At, const FVector& N, int32 A)
	{
		const FVector D = FVector(Pos.R, Pos.G, Pos.B) - At;
		const double d = D.Size();
		float f = FMath::Clamp(1.f - float(d) / FMath::Max(Pos.A, 1.f), 0.f, 1.f);
		f *= f;
		const float Facing = 0.25f + 0.75f * FMath::Clamp(float(FVector::DotProduct(N.GetSafeNormal(), D / FMath::Max(d, 0.001))), 0.f, 1.f);
		const float S = FMath::Pow(float(A) / 31.f, 2.2f);
		return FLinearColor(S + Col.R * f * Facing, S + Col.G * f * Facing, S + Col.B * f * Facing, 1.f);
	}
	float LrLevel(const FLinearColor& Lin)
	{
		// the colour's luminance in linear light, each channel clipped as the screen clips it, shown as its 1 / 2.2 power
		const float Y = 0.2126f * FMath::Min(Lin.R, 1.f) + 0.7152f * FMath::Min(Lin.G, 1.f) + 0.0722f * FMath::Min(Lin.B, 1.f);
		return 31.f * FMath::Pow(FMath::Clamp(Y, 0.f, 1.f), 1.f / 2.2f);
	}

	// the screen read once (UGameViewportClient's screenshot delegate: no file), with the MPC's lantern of that frame
	TArray<FColor> LrPixels;
	FIntPoint LrSize = FIntPoint::ZeroValue;
	FLinearColor LrPos0, LrCol0;
	float LrOthers = 0.f;                      // the other slots' light that frame (a shot's flash, a lamp): the read is taken again
	bool bLrGot = false;
	FDelegateHandle LrHook;
}

OL_SHOT_MATCH(OLLanternRangeProbe) { return OLFlags::Param(TEXT("OLLanternRangeProbe")); }

OL_SHOT_TEST(OLLanternRangeProbe, 1900)
{
	static int32 Passes = 0, Fails = 0, Case = 0, Spot = 0, Wait = 0;
	static float Quiet = -1.f, NextAt = 0.f, Pix1 = -1.f;
	static uint8 Map[128];
	static double KProb[5] = {};
	static FVector Eye0 = FVector::ZeroVector, Ahead = FVector::ForwardVector;
	static TWeakObjectPtr<UProceduralMeshComponent> Wall;
	static TArray<int32> Spots;
	if (!L || ShotStep < 0) return;
	const bool bOld = OLFlags::Param(TEXT("OLOldLantern"));
	const bool bEs31 = GetWorld()->FeatureLevel == ERHIFeatureLevel::ES3_1;
	const TCHAR* Path = bEs31 ? (OutlawsDeviceProfile::IsQuest2() ? TEXT("q2") : TEXT("q3")) : TEXT("pc");
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lanternrange check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	auto Mpc = [this, L]() { return L->LightsMpc && GetWorld() ? GetWorld()->GetParameterCollectionInstance(L->LightsMpc) : nullptr; };
	auto LampUp = [L]()
	{
		if (L->GetCount(TEXT("ilantern")) <= 0) L->AddCount(TEXT("ilantern"), 1);
		if (L->GetCount(TEXT("ioil")) < 300) L->AddCount(TEXT("ioil"), 300);       // (full: the oil's dimming kept out)
		if (!L->bLampOn) L->ToggleLamp();
	};
	auto OrigMean = [](float d)
	{
		double S = 0.0;
		for (int32 k = 1; k <= 4; ++k) S += KProb[k] * LrOriginal(Map, d, k, 0);
		return float(S);
	};
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	switch (ShotStep)
	{
	case 0:
	{
		if (ShotTime < 4.f || !bUp || !TestShadersReady(Quiet, 2.f)) return;
		L->bInvincible = true;
		// no lamp lights while the test reads (a lit lamp near would add to the wall), the gamma at 1.0 (REQ-GAMMA12: a test's own, SetGammaRaw, the picture as before the row's scale; let go after)
		if (IConsoleVariable* Lamps = IConsoleManager::Get().FindConsoleVariable(TEXT("ol.LampLights"))) Lamps->Set(0, ECVF_SetByCode);
		OutlawsDeviceProfile::SetGammaRaw(1.f);
		LrDepthMap(Map);
		// the steady flicker [EXE 0x1402fd740], every 0.02 s: one time in four the last +-0.125 (at least 1), else 1 + rand(29) / 8
		{
			FRandomStream R(1900);
			float K = 11.f;
			int32 Count[5] = {};
			for (int32 t = 0; t < 100000; ++t)
			{
				float V;
				if (R.FRand() * 4.f < 1.f) V = FMath::Max(FMath::Min((R.FRand() * 2.f - 1.f) * 0.125f + K, 31.f), 1.f);
				else V = R.FRand() * 29.f * 0.125f + 1.f;
				K = float(int32(V));
				++Count[FMath::Clamp(int32(K), 0, 4)];
			}
			double Mean = 0.0;
			for (int32 k = 1; k <= 4; ++k) { KProb[k] = Count[k] / 100000.0; Mean += k * KProb[k]; }
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lanternrange the original: depth map %d %d %d .. %d %d (index 4 x units), steady flicker 1..4 at %.3f %.3f %.3f %.3f, mean %.3f"),
				Map[0], Map[13], Map[53], Map[106], Map[127], KProb[1], KProb[2], KProb[3], KProb[4], Mean);
		}
		// (1) the curve: ours over a minute of the flicker (LanternLight, the formula the level puts in L0 / C0), the original's
		const float Reach = AOutlawsLevelActor::LanternReachNow(), Bright = AOutlawsLevelActor::LanternBrightNow();
		Check(bOld ? (Reach == AOutlawsLevelActor::LanternReach && Bright == AOutlawsLevelActor::LanternBright) : (Reach == AOutlawsLevelActor::LanternRangeReach && Bright == AOutlawsLevelActor::LanternRangeBright),
			FString::Printf(TEXT("(1) %s: reach %.0f cm, brightness %.2f (REQ-LANTERNRANGE 970 and 1.26; -OLOldLantern REQ-LANTERNBOOST's 2200 and 0.75)"), bOld ? TEXT("-OLOldLantern") : TEXT("new"), Reach, Bright));
		int32 Off = 0;
		FString Row;
		for (int32 i = 0; i < LrNum; ++i)
		{
			double S = 0.0;
			int32 n = 0;
			for (float T = 0.f; T < 60.f; T += 0.05f, ++n)
			{
				FLinearColor P, C;
				AOutlawsLevelActor::LanternLight(FVector::ZeroVector, 1.f, T, P, C);
				const FVector At(FMath::Max(LrDist[i], 0.01f) * 100.0, 0.0, 0.0);      // (0 m: a centimetre, the facing kept)
				S += LrLevel(LrOursLinear(P, C, At, FVector(-1.0, 0.0, 0.0), 0));
			}
			const float Ours = float(S / n), Orig = OrigMean(LrDist[i]);
			Off += FMath::Abs(Ours - Orig) > LrTol ? 1 : 0;
			Row += FString::Printf(TEXT(" %g m %.1f/%.1f (%+.1f);"), LrDist[i], Ours, Orig, Ours - Orig);
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lanternrange (1) %s, ours/original (levels of 31, a facing wall, AMBIENT 0, full oil):%s"), bOld ? TEXT("-OLOldLantern") : TEXT("new"), *Row);
		if (bOld) { UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lanternrange (1) -OLOldLantern: %d of %d distances off the original's by more than %.1f levels (today's numbers)"), Off, LrNum, LrTol); }
		else Check(Off == 0, FString::Printf(TEXT("(1) the curve within %.1f levels of the original's at every distance (%d off)"), LrTol, Off));
		// (2) a clear line for the wall: from the start, then the night yard's middle, the first heading clear 13 m out
		LampUp();
		Eye0 = FVector::ZeroVector;
		const FOutlawsInfRuntime* Inf = L->DebugInf();
		TArray<FVector> From;
		From.Add(GetActorLocation());
		if (Inf)
		{
			for (int32 s : { 186, 185, 181 })
			{
				if (!Inf->Live.Sectors.IsValidIndex(s)) continue;
				const FOutlawsSector& Sec = Inf->Live.Sectors[s];
				FVector2D C = FVector2D::ZeroVector;
				for (const FVector2D& V : Sec.Verts) C += V;
				C /= double(FMath::Max(1, Sec.Verts.Num()));
				From.Add(FOutlawsGeometry::ToUE(C.X, Sec.Floor.Y, C.Y) + FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0));
			}
		}
		FCollisionQueryParams Q(TEXT("OLLanternRange"), false, this);
		for (const FVector& F : From)
		{
			for (int32 y = 0; y < 24 && Eye0.IsZero(); ++y)
			{
				const FVector Dir = FRotator(0.0, y * 15.0, 0.0).Vector();
				const FVector Top = F + FVector(0.0, 0.0, 60.0), Low = F + FVector(0.0, 0.0, 10.0);
				FHitResult H;
				if (GetWorld()->LineTraceSingleByChannel(H, Top, Top + Dir * 1400.0, ECC_Visibility, Q)) continue;
				if (GetWorld()->LineTraceSingleByChannel(H, Low, Low + Dir * 1400.0, ECC_Visibility, Q)) continue;
				Eye0 = F;
				Ahead = Dir;
			}
			if (!Eye0.IsZero()) break;
		}
		Check(!Eye0.IsZero(), FString::Printf(TEXT("(2) a clear line 14 m long for the test wall (%d places tried)"), From.Num()));
		if (Eye0.IsZero()) { ShotStep = 3; NextAt = ShotTime + 0.5f; return; }
		TeleportTo(Eye0, Ahead.Rotation(), false, true);
		// the wall: white, AMBIENT 0 (vertex colour black, alpha 1: no swirl), both faces, 1.6 m square, no collision
		UProceduralMeshComponent* M = NewObject<UProceduralMeshComponent>(this, TEXT("OLLanternRangeWall"));
		M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		M->CastShadow = false;
		M->SetUsingAbsoluteLocation(true);
		M->SetUsingAbsoluteRotation(true);
		M->RegisterComponent();
		const TArray<FVector> V = { FVector(0.0, -80.0, -80.0), FVector(0.0, 80.0, -80.0), FVector(0.0, 80.0, 80.0), FVector(0.0, -80.0, 80.0) };
		const TArray<FVector2D> Uv = { FVector2D(0.0, 1.0), FVector2D(1.0, 1.0), FVector2D(1.0, 0.0), FVector2D(0.0, 0.0) };
		const TArray<FColor> Vc = { FColor(0, 0, 0, 255), FColor(0, 0, 0, 255), FColor(0, 0, 0, 255), FColor(0, 0, 0, 255) };
		const TArray<FVector> Nf = { FVector(-1.0, 0.0, 0.0), FVector(-1.0, 0.0, 0.0), FVector(-1.0, 0.0, 0.0), FVector(-1.0, 0.0, 0.0) };
		const TArray<FVector> Nb = { FVector(1.0, 0.0, 0.0), FVector(1.0, 0.0, 0.0), FVector(1.0, 0.0, 0.0), FVector(1.0, 0.0, 0.0) };
		M->CreateMeshSection(0, V, { 0, 1, 2, 0, 2, 3 }, Nf, Uv, Vc, TArray<FProcMeshTangent>(), false);
		M->CreateMeshSection(1, V, { 0, 2, 1, 0, 3, 2 }, Nb, Uv, Vc, TArray<FProcMeshTangent>(), false);
		if (UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Outlaws/Runtime/M_OLR_World.M_OLR_World")))
		{
			UMaterialInstanceDynamic* Mid = UMaterialInstanceDynamic::Create(Base, M);
			if (UTexture* White = LoadObject<UTexture>(nullptr, TEXT("/Engine/EngineResources/WhiteSquareTexture.WhiteSquareTexture"))) Mid->SetTextureParameterValue(TEXT("Tex"), White);
			M->SetMaterial(0, Mid);
			M->SetMaterial(1, Mid);
		}
		Wall = M;
		LrHook = UGameViewportClient::OnScreenshotCaptured().AddLambda([Mpc](int32 W, int32 H, const TArray<FColor>& Bitmap)
		{
			LrPixels = Bitmap;
			LrSize = FIntPoint(W, H);
			if (UMaterialParameterCollectionInstance* I = Mpc())
			{
				I->GetVectorParameterValue(TEXT("L0"), LrPos0);
				I->GetVectorParameterValue(TEXT("C0"), LrCol0);
				LrOthers = 0.f;
				for (int32 s = 1; s < 8; ++s)
				{
					FLinearColor C(0.f, 0.f, 0.f, 0.f);
					I->GetVectorParameterValue(*FString::Printf(TEXT("C%d"), s), C);
					LrOthers += C.R + C.G + C.B;
				}
			}
			bLrGot = true;
		});
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lanternrange (2) the wall's line from %s heading %.0f, %s path"), *Eye0.ToString(), Ahead.Rotation().Yaw, Path);
		Case = 1;      // (1, 2 the path's scale; 3..10 0.5 m to 12 m; 11 the lantern out at 4 m; 12 Brightness 1.6 at 4 m)
		Wait = 0;
		ShotStep = 1;
		return;
	}
	case 1:
	{
		// each case: the wall straight ahead of the eye (the lantern below it, at the chest or in a hand: the frame's L0 says
		// where), 8 frames, the screen read, then the next.  Cases 1 and 2 first: the lantern out, the wall at AMBIENT 16 and
		// 8: how this path shows the level's own light (a level L as L / 31 of full bright, or brighter: the PC's exposure),
		// the scale the lantern's reads are held to
		UProceduralMeshComponent* M = Wall.Get();
		struct FLrCase { int32 Di; bool bLit; int32 Amb; float Gamma; };
		static const FLrCase LrCases[] = { { 5, false, 16, 1.f }, { 5, false, 8, 1.f }, { 1, true, 0, 1.f }, { 2, true, 0, 1.f }, { 3, true, 0, 1.f }, { 4, true, 0, 1.f },
			{ 5, true, 0, 1.f }, { 6, true, 0, 1.f }, { 7, true, 0, 1.f }, { 8, true, 0, 1.f }, { 5, false, 0, 1.f }, { 7, true, 0, 1.6f } };
		static float ExpLin = 1.f;
		const FLrCase& Cs = LrCases[FMath::Clamp(Case - 1, 0, int32(UE_ARRAY_COUNT(LrCases)) - 1)];
		const int32 Di = Cs.Di;
		if (!Cs.bLit) { if (L->bLampOn) L->ToggleLamp(); }
		else LampUp();
		if (Controller) Controller->SetControlRotation(Ahead.Rotation());
		const FVector P = Camera->GetComponentLocation() + Ahead * (LrDist[Di] * 100.0);
		if (M) { M->SetWorldLocation(P); M->SetWorldRotation(Ahead.Rotation()); }
		OutlawsDeviceProfile::SetGammaRaw(Cs.Gamma);
		if (Wait == 0 && M)
		{
			// the wall's own light (the vertex colour's red, as the level's faces carry their sector's)
			const FColor Vc(uint8(FMath::RoundToInt(Cs.Amb / 31.f * 255.f)), 0, 0, 255);
			const TArray<FVector> V = { FVector(0.0, -80.0, -80.0), FVector(0.0, 80.0, -80.0), FVector(0.0, 80.0, 80.0), FVector(0.0, -80.0, 80.0) };
			const TArray<FVector2D> Uv = { FVector2D(0.0, 1.0), FVector2D(1.0, 1.0), FVector2D(1.0, 0.0), FVector2D(0.0, 0.0) };
			const TArray<FColor> Cols = { Vc, Vc, Vc, Vc };
			M->UpdateMeshSection(0, V, { FVector(-1.0, 0.0, 0.0), FVector(-1.0, 0.0, 0.0), FVector(-1.0, 0.0, 0.0), FVector(-1.0, 0.0, 0.0) }, Uv, Cols, TArray<FProcMeshTangent>());
			M->UpdateMeshSection(1, V, { FVector(1.0, 0.0, 0.0), FVector(1.0, 0.0, 0.0), FVector(1.0, 0.0, 0.0), FVector(1.0, 0.0, 0.0) }, Uv, Cols, TArray<FProcMeshTangent>());
		}
		if (++Wait == 8) { bLrGot = false; FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / TEXT("lanternrange_read.png"), false, false); }
		if (Wait < 9 || !bLrGot) { if (Wait > 60) { Check(false, FString::Printf(TEXT("(2) case %d: no screen read"), Case)); Wait = 0; ++Case; } return; }
		if (LrOthers > 0.f)
		{
			// a bad guy's shot flashed (or a lamp shone) on that frame: read again (the night yard's men shoot at us)
			static int32 Again = 0;
			if (++Again <= 40) { Wait = 6; return; }
			Check(false, FString::Printf(TEXT("(2) case %d: every read lit by another slot too (%.3f)"), Case, LrOthers));
		}
		{
			// (the read kept beside the shots, to look at)
			TArray64<uint8> Png;
			FImageUtils::PNGCompressImageArray(LrSize.X, LrSize.Y, TArrayView64<const FColor>(LrPixels.GetData(), LrPixels.Num()), Png);
			FFileHelper::SaveArrayToFile(Png, *(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("lanternrange_read%02d_%s_%s.png"), Case, bOld ? TEXT("old") : TEXT("new"), Path)));
		}
		// the wall's middle where the screen shows it, 5 x 5 pixels
		FVector2D Sp(LrSize.X * 0.5, LrSize.Y * 0.5);
		if (APlayerController* Pc = Cast<APlayerController>(Controller)) Pc->ProjectWorldLocationToScreen(P, Sp, false);
		double R = 0.0, G = 0.0, B = 0.0;
		int32 n = 0;
		for (int32 dy = -2; dy <= 2; ++dy) for (int32 dx = -2; dx <= 2; ++dx)
		{
			const int32 X = FMath::Clamp(FMath::RoundToInt(Sp.X) + dx, 0, LrSize.X - 1), Y = FMath::Clamp(FMath::RoundToInt(Sp.Y) + dy, 0, LrSize.Y - 1);
			if (!LrPixels.IsValidIndex(Y * LrSize.X + X)) continue;
			// (sRGB to linear; the headset's path on the desktop writes its linear colour into the 8-bit buffer as it is, the
			// headset's hardware encoding it to sRGB as it goes out: there the byte is the linear value)
			const FLinearColor Lin = bEs31 ? LrPixels[Y * LrSize.X + X].ReinterpretAsLinear() : FLinearColor(LrPixels[Y * LrSize.X + X]);
			R += Lin.R; G += Lin.G; B += Lin.B; ++n;
		}
		const FLinearColor Got(float(R / FMath::Max(n, 1)), float(G / FMath::Max(n, 1)), float(B / FMath::Max(n, 1)), 1.f);
		const float GotLv = LrLevel(Got);
		FLinearColor Want = LrCol0.R + LrCol0.G + LrCol0.B > 0.f ? LrOursLinear(LrPos0, LrCol0, P, -Ahead, Cs.Amb) : LrOursLinear(LrPos0, FLinearColor(0.f, 0.f, 0.f, 0.f), P, -Ahead, Cs.Amb);
		const float Dist = float(FVector::Dist(FVector(LrPos0.R, LrPos0.G, LrPos0.B), P)) / 100.f;
		if (!Cs.bLit && Cs.Amb > 0)
		{
			// the path's scale: the read against the level's light shown plainly (one number for the whole picture, the
			// level's light and the lantern alike: the lantern adds before it)
			const float Plain = FMath::Pow(Cs.Amb / 31.f, 2.2f);
			const float Y = 0.2126f * Got.R + 0.7152f * Got.G + 0.0722f * Got.B;
			if (Case == 1) ExpLin = Y / Plain;
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lanternrange (2) %s the lantern out, a wall at AMBIENT %d: the screen %.2f levels (plain %d): %.3f times the light in linear (%.3f on the screen)"),
				Path, Cs.Amb, GotLv, Cs.Amb, Y / Plain, FMath::Pow(Y / Plain, 1.f / 2.2f));
			if (Case == 2) Check(FMath::Abs(Y / Plain - ExpLin) <= 0.08f * ExpLin, FString::Printf(TEXT("(2) %s one scale for the whole picture: AMBIENT 16 %.3f, AMBIENT 8 %.3f"), Path, ExpLin, Y / Plain));
		}
		else if (Cs.bLit && Cs.Gamma == 1.f)
		{
			const FLinearColor Seen = Want * ExpLin;
			const float WantLv = LrLevel(Seen), Orig = OrigMean(LrDist[Di]);
			Check(FMath::Abs(GotLv - WantLv) <= LrPixTol, FString::Printf(TEXT("(2) %s %g m: the screen %.2f levels (rgb %.3f %.3f %.3f), the frame's sum %.2f at the path's scale (%.2f plain; reach %.0f cm, colour %.3f %.3f %.3f, %.2f m off); the original %.1f"),
				Path, LrDist[Di], GotLv, Got.R, Got.G, Got.B, WantLv, LrLevel(Want), LrPos0.A, LrCol0.R, LrCol0.G, LrCol0.B, Dist, Orig));
			if (Di == 7) Pix1 = GotLv;
		}
		else if (!Cs.bLit)
		{
			// (the PC's bloom glows the lit yard over everything, a few levels; the headset has none)
			const float Black = bEs31 ? 0.75f : 4.f;
			Check(GotLv <= Black && LrCol0.R + LrCol0.G + LrCol0.B <= 0.f, FString::Printf(TEXT("(2) %s the lantern out, 4 m: the wall black (%.2f levels, at most %.2f; C0 %.3f)"), Path, GotLv, Black, LrCol0.R + LrCol0.G + LrCol0.B));
		}
		else
		{
			// Brightness 1.6 lifts the lantern's dim edge (8 m) as it lifts the level's shadows: the headset's OLQuestLook takes
			// pow(c, 1 / 1.6), the PC's ColorGamma the same about the grade's mid grey (0.18)
			Want *= ExpLin;
			const FLinearColor G16(FMath::Pow(FMath::Min(Want.R, 1.f), 1.f / 1.6f), FMath::Pow(FMath::Min(Want.G, 1.f), 1.f / 1.6f), FMath::Pow(FMath::Min(Want.B, 1.f), 1.f / 1.6f), 1.f);
			Check(GotLv > Pix1 + 1.f, FString::Printf(TEXT("(2) %s Brightness 1.6, 8 m: the screen %.2f levels, brighter than at 1.0 (%.2f; the frame's sum ^ (1 / 1.6) %.2f)"),
				Path, GotLv, Pix1, LrLevel(G16)));
			OutlawsDeviceProfile::SetGammaRaw(1.f);
		}
		Wait = 0;
		if (++Case <= int32(UE_ARRAY_COUNT(LrCases))) return;
		UGameViewportClient::OnScreenshotCaptured().Remove(LrHook);
		if (M) M->DestroyComponent();
		// (3) the shots
		Spots = { 152, 184, 186 };
		FString List;
		if (OLFlags::Value(TEXT("OLLanternRangeSpots"), List))
		{
			TArray<FString> Parts;
			List.ParseIntoArray(Parts, TEXT(","));
			Spots.Reset();
			for (const FString& S : Parts) Spots.Add(FCString::Atoi(*S));
		}
		Spot = 0;
		NextAt = 0.f;
		ShotStep = 2;
		return;
	}
	case 2:
	{
		static const TCHAR* const Names[] = { TEXT("corridor"), TEXT("darkroom"), TEXT("night") };
		const FOutlawsInfRuntime* Inf = L->DebugInf();
		LampUp();
		if (Spot >= Spots.Num() || !Inf) { ShotStep = 3; NextAt = ShotTime + 0.5f; return; }
		if (NextAt == 0.f)
		{
			// stand 1.2 m in from one end of the sector's longer side, looking along it, a little down
			const int32 s = Spots[Spot];
			if (!Inf->Live.Sectors.IsValidIndex(s)) { ++Spot; return; }
			const FOutlawsSector& Sec = Inf->Live.Sectors[s];
			FBox2D Bx(ForceInit);
			for (const FVector2D& V : Sec.Verts) Bx += V;
			const FVector2D C = Bx.GetCenter(), Ext = Bx.GetExtent();
			const bool bX = Ext.X >= Ext.Y;
			const FVector2D Foot = bX ? FVector2D(Bx.Min.X + 4.0, C.Y) : FVector2D(C.X, Bx.Min.Y + 4.0);
			const FVector2D To = bX ? FVector2D(Bx.Max.X, C.Y) : FVector2D(C.X, Bx.Max.Y);
			const FVector Feet = FOutlawsGeometry::ToUE(Foot.X, Sec.Floor.Y, Foot.Y), Far = FOutlawsGeometry::ToUE(To.X, Sec.Floor.Y, To.Y);
			const FRotator Rot(-8.0, (Far - Feet).Rotation().Yaw, 0.0);
			TeleportTo(Feet + FVector(0.0, 0.0, GetCapsuleComponent()->GetScaledCapsuleHalfHeight() + 2.0), FRotator(0.0, Rot.Yaw, 0.0), false, true);
			if (Controller) Controller->SetControlRotation(Rot);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lanternrange (3) %s: sector %d (AMBIENT %d, %.1f x %.1f m), standing at %s heading %.0f"),
				Names[FMath::Min(Spot, 2)], s, Sec.Ambient, Ext.X * 0.6, Ext.Y * 0.6, *Feet.ToString(), Rot.Yaw);
			NextAt = ShotTime + 2.f;
			return;
		}
		if (ShotTime < NextAt) return;
		const FString Name = FString::Printf(TEXT("lanternrange_%s_%s_%s.png"), Names[FMath::Min(Spot, 2)], bOld ? TEXT("old") : TEXT("new"), Path);
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name, false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lanternrange shot %s"), *Name);
		++Spot;
		NextAt = 0.f;
		return;
	}
	case 3:
		if (ShotTime < NextAt + 1.f) return;
		OutlawsDeviceProfile::SetGammaRaw(0.f);       // (the row's own again)
		if (L->bLampOn) L->ToggleLamp();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT lanternrange RESULT %s (%d passed, %d failed; %s, %s)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			bOld ? TEXT("-OLOldLantern") : TEXT("new"), Path);
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
