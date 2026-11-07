// BUG-SKYSCROLL0 (user 6 Nov: "SKYSCROLL 0 means the sky doesn't scroll at all and stays at its default position"; a level with no
// SKYSCROLL stays exactly as it is): -OLShots -OLLevel=<level> -OLNoFrontEnd -OLSkyScrollProbe -OLSkyScrollExpect=still|moves
// [-OLSkyScrollItm=INT|FLOAT|NONE: the level's ITM read with that form of the key] [-OLOldSkyScroll].  The ITM cases first (INT and
// FLOAT, any case and spacing, none, not zero).  Then out under the open sky, a shot low and one looking up, the same two
// 30 s later (skyscroll_<tag>_<t0|t30>_<horizon|up>.png in Saved\OLShots), and every number that moves the sky read at both times:
// the dome's yaw, and Spin, CloudSpin and CloudWind on the sky's instance and each outer wall's (REQ-SKYTHRU).  "still": all of
// them identical to the digit and at the default place.  "moves": today's rates, worked out from the tick's own sums.  A co-op
// host and client each run it (-OLCoopHost with -OLSkyScrollHold, -OLCoopJoin).  Lines "OLSHOT skyscroll check PASS|FAIL: ..." and
// "OLSHOT skyscroll RESULT".

#include "OutlawsPlayer.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsCoop.h"
#include "OutlawsFlags.h"
#include "OutlawsFormats.h"
#include "OutlawsShotTests.h"
#include "Components/CapsuleComponent.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "HAL/PlatformMisc.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ProceduralMeshComponent.h"
#include "UnrealClient.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsSkyScrollProbeLocal
{
	/** A shot's pixels, read back from its PNG (the offscreen viewport reads back black). */
	bool ReadShot(const FString& File, TArray<FColor>& Px, int32& W, int32& H)
	{
		TArray<uint8> Bytes;
		TArray64<uint8> Raw;
		if (!FFileHelper::LoadFileToArray(Bytes, *File, FILEREAD_Silent)) return false;
		IImageWrapperModule& Mod = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> Png = Mod.CreateImageWrapper(EImageFormat::PNG);
		if (!Png.IsValid() || !Png->SetCompressed(Bytes.GetData(), Bytes.Num()) || !Png->GetRaw(ERGBFormat::BGRA, 8, Raw)) return false;
		W = int32(Png->GetWidth());
		H = int32(Png->GetHeight());
		if (W <= 0 || H <= 0 || Raw.Num() != int64(W) * H * 4) return false;
		Px.SetNumUninitialized(W * H);
		FMemory::Memcpy(Px.GetData(), Raw.GetData(), Raw.Num());
		return true;
	}
}

void AOutlawsLevelActor::TestSkyMovers(TArray<float>& Out, double& OutDomeYaw) const
{
	Out.Reset();
	OutDomeYaw = SkyMesh ? double(SkyMesh->GetComponentRotation().Yaw) : 0.0;
	if (!SkySurfaceMid) return;
	auto Add = [&Out](const UMaterialInstanceDynamic* M)
	{
		float Spin = -1.f, Cloud = -1.f;
		FLinearColor Wind(-1.f, -1.f, -1.f, -1.f);
		M->GetScalarParameterValue(FName(TEXT("Spin")), Spin);
		M->GetScalarParameterValue(FName(TEXT("CloudSpin")), Cloud);
		M->GetVectorParameterValue(FName(TEXT("CloudWind")), Wind);
		Out.Add(Spin);
		Out.Add(Cloud);
		Out.Add(Wind.R);
		Out.Add(Wind.G);
	};
	Add(SkySurfaceMid);
	for (const UMaterialInstanceDynamic* M : SkyThruMids) if (M) Add(M);
}

FString AOutlawsLevelActor::TestSkyPictureNow() const
{
	if (!SkySurfaceMid) return TEXT("no sky");
	UTexture* T = nullptr;
	SkySurfaceMid->GetTextureParameterValue(FName(TEXT("Tex")), T);
	bool bAnimated = false;
	for (const TPair<FString, FAtxState>& Kv : AtxLoops) if (Kv.Value.Mids.Contains(TWeakObjectPtr<UMaterialInstanceDynamic>(SkySurfaceMid.Get()))) bAnimated = true;
	return FString::Printf(TEXT("%s%s"), T ? *T->GetName() : TEXT("none"), bAnimated ? TEXT(" (a texture animation owns it)") : TEXT(""));
}

void AOutlawsLevelActor::TestSkyScrollItm(FString& ItmText) const
{
	FString Form;
	if (!OLFlags::Value(TEXT("OLSkyScrollItm"), Form, false) || Form.IsEmpty()) return;
	TArray<FString> Lines, Kept;
	ItmText.ParseIntoArrayLines(Lines);
	for (const FString& Line : Lines) if (!Line.ToUpper().Contains(TEXT("SKYSCROLL"))) Kept.Add(Line);
	if (Form.Equals(TEXT("INT"), ESearchCase::IgnoreCase)) Kept.Add(TEXT("\tINT\tSKYSCROLL\t0"));
	else if (Form.Equals(TEXT("FLOAT"), ESearchCase::IgnoreCase)) Kept.Add(TEXT("  FLOAT  SKYSCROLL   0.0"));
	ItmText = FString::Join(Kept, TEXT("\r\n"));
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skyscroll itm form %s on %s"), *Form, *LevelName);
}

OL_SHOT_MATCH(OLSkyScrollProbe) { return OLFlags::Param(TEXT("OLSkyScrollProbe")); }

OL_SHOT_TEST(OLSkyScrollProbe, 2156)
{
	static int32 Passes = 0, Fails = 0;
	static float Wait = 0.f, Quiet = -1.f, Yaw = 0.f;
	static double T0 = 0.0, Yaw0 = 0.0;
	static TArray<float> Sky0;
	static FString Picture0;
	if (!L || ShotStep < 0) return;
	auto Check = [](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skyscroll check %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	};
	UWorld* W = GetWorld();
	const bool bUp = !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay() && !L->IsMoviePlaying() && !bDeadView;
	FString Expect, Form, Join;
	OLFlags::Value(TEXT("OLSkyScrollExpect"), Expect, false);
	OLFlags::Value(TEXT("OLSkyScrollItm"), Form, false);
	const bool bOld = OLFlags::Param(TEXT("OLOldSkyScroll"));
	const TCHAR* const Who = OLFlags::Value(TEXT("OLCoopJoin"), Join) ? TEXT("client") : OLFlags::Param(TEXT("OLCoopHost")) ? TEXT("host") : TEXT("solo");
	const FString Tag = FString::Printf(TEXT("skyscroll_%s_%s%s%s"), *L->LevelName.ToLower(), Who, Form.IsEmpty() ? TEXT("") : *(TEXT("_") + Form.ToLower()), bOld ? TEXT("_old") : TEXT(""));
	auto Shot = [&](const TCHAR* Pose)
	{
		const FString Name = Tag + TEXT("_") + Pose;
		FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Name + TEXT(".png"), false, false);
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skyscroll shot %s.png"), *Name);
	};
	auto Diff = [&](const TCHAR* Pose)
	{
		TArray<FColor> A, B;
		int32 Aw = 0, Ah = 0, Bw = 0, Bh = 0;
		if (!OutlawsSkyScrollProbeLocal::ReadShot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Tag + TEXT("_t0_") + Pose + TEXT(".png"), A, Aw, Ah) ||
			!OutlawsSkyScrollProbeLocal::ReadShot(FPaths::ProjectSavedDir() / TEXT("OLShots") / Tag + TEXT("_t30_") + Pose + TEXT(".png"), B, Bw, Bh) || Aw != Bw || Ah != Bh)
		{
			Check(false, FString::Printf(TEXT("%s shots read"), Pose));
			return 1.f;
		}
		int32 Off = 0;
		for (int32 i = 0; i < A.Num(); ++i)
		{
			if (FMath::Max3(FMath::Abs(A[i].R - B[i].R), FMath::Abs(A[i].G - B[i].G), FMath::Abs(A[i].B - B[i].B)) > 8) ++Off;
		}
		const float Part = float(Off) / float(FMath::Max(1, A.Num()));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skyscroll diff %s %s: %d of %d pixels differ by more than 8 (%.3f%%)"), *Tag, Pose, Off, A.Num(), 100.f * Part);
		return Part;
	};
	const bool bLookUp = ShotStep == 2 || ShotStep == 5;
	if (ShotStep > 0 && Controller) Controller->SetControlRotation(FRotator(bLookUp ? 40.0 : 8.0, Yaw, 0.0));
	switch (ShotStep)
	{
	case 0:
	{
		if (!bUp || !W || ShotTime < 6.f || !TestShadersReady(Quiet, 3.f)) return;
		L->bInvincible = true;
		// the ITM cases, through the parser the level's own file goes through
		struct FCase { const TCHAR* Text; bool bStill; };
		static const FCase ItmCases[] = {
			{ TEXT("ITEM 1.0\r\nDATA 1\r\n\tINT\tSKYSCROLL\t0\r\n"), true },
			{ TEXT("ITEM 1.0\r\nDATA 1\r\n\tFLOAT\tSKYSCROLL\t0.0\r\n"), true },
			{ TEXT("FLOAT SKYSCROLL 0\n"), true },
			{ TEXT("  FLOAT  SKYSCROLL   0.0"), true },
			{ TEXT("int skyscroll 0\n"), true },
			{ TEXT("Float SkyScroll 0.000\n"), true },
			{ TEXT("ITEM 1.0\r\nDATA 1\r\n\tSTR\tTAUNT_TYPE\tTOWN\r\n"), false },
			{ TEXT("FLOAT SKYSCROLL 1.0\n"), false },
			{ TEXT("INT SKYSCROLL 2\n"), false },
		};
		for (const FCase& C : ItmCases)
		{
			FString Func;
			TMap<FString, FString> Data;
			OutlawsFormats::ParseItm(C.Text, Func, Data);
			Check(AOutlawsLevelActor::SkyScrollIsStill(Data) == C.bStill, FString::Printf(TEXT("the ITM text \"%s\" reads as %s"),
				*FString(C.Text).Replace(TEXT("\r\n"), TEXT("|")).Replace(TEXT("\n"), TEXT("|")).Replace(TEXT("\t"), TEXT("~")), C.bStill ? TEXT("still") : TEXT("moving")));
		}
		// out under the open sky: the sky sector whose eye-high view runs farthest, facing its longest (the skydome probe's way)
		int32 Pick = INDEX_NONE, BestPick = INDEX_NONE;
		double BestMean = -1.0;
		FVector BestAt = GetActorLocation();
		float BestYaw = 0.f;
		const double Half = GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsSkyScroll), false, this);
		int32 Forced = INDEX_NONE;      // (-OLSkyScrollSector=<n>: a level whose best guess stands in a box, ranch's)
		OLFlags::Value(TEXT("OLSkyScrollSector"), Forced);
		FVector ForcedPt;
		FHitResult ForcedHit;
		if (Forced != INDEX_NONE && L->DebugPointInSector(Forced, ForcedPt) && W->LineTraceSingleByChannel(ForcedHit, ForcedPt - FVector(0.0, 0.0, 20.0), ForcedPt - FVector(0.0, 0.0, 6000.0), ECC_Visibility, Q))
		{
			Pick = Forced;
			BestAt = ForcedHit.ImpactPoint + FVector(0.0, 0.0, Half + 5.0);
		}
		for (int32 i = 0, Tried = 0, Pass = 0; Pick == INDEX_NONE && Pass < 2 && Tried < 600; i = (i + 1 < L->TestSectorCount()) ? i + 1 : (++Pass, 0))
		{
			FVector Pt;
			if (Pass == 1 && BestPick != INDEX_NONE) break;
			if ((Pass == 0 && !(L->TestSectorFlags(i) & 1u)) || !L->DebugPointInSector(i, Pt)) continue;
			++Tried;
			FHitResult Hit;
			if (!W->LineTraceSingleByChannel(Hit, Pt - FVector(0.0, 0.0, 20.0), Pt - FVector(0.0, 0.0, 6000.0), ECC_Visibility, Q)) continue;
			const FVector Eye = Hit.ImpactPoint + FVector(0.0, 0.0, 170.0);
			double Sum = 0.0, Far = -1.0;
			float FarYaw = 0.f;
			for (int32 k = 0; k < 16; ++k)
			{
				const float Yw = 22.5f * k;
				FHitResult H2;
				const double Dist = W->LineTraceSingleByChannel(H2, Eye, Eye + FRotator(0.0, Yw, 0.0).Vector() * 200000.0, ECC_Visibility, Q) ? H2.Distance : 200000.0;
				Sum += FMath::Min(Dist, 20000.0);
				if (Dist > Far) { Far = Dist; FarYaw = Yw; }
			}
			if (Sum / 16.0 > BestMean) { BestMean = Sum / 16.0; BestPick = i; BestAt = Hit.ImpactPoint + FVector(0.0, 0.0, Half + 5.0); BestYaw = FarYaw; }
		}
		if (Pick == INDEX_NONE) Pick = BestPick;
		TeleportTo(BestAt, FRotator(0.0, BestYaw, 0.0), false, true);
		Yaw = BestYaw;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skyscroll place %s (%s): sky sector %d, mean view %.0f cm, facing %.1f; cloud layer %d, gradient %d, walls' sky instances %d, sky still %d, co-op %d"),
			*L->LevelName, Who, Pick, BestMean, Yaw, L->TestSkyCloudLayer() ? 1 : 0, L->TestSkyGradient() ? 1 : 0, L->TestSkyThruMids(), L->TestSkyStill() ? 1 : 0, UOutlawsCoop::IsActive(this) ? 1 : 0);
		Wait = ShotTime + 2.f;
		ShotStep = 1;
		return;
	}
	case 1:      // (the horizon now, the sky looked up at a turn later)
		if (ShotTime < Wait) return;
		Shot(TEXT("t0_horizon"));
		Wait = ShotTime + 1.5f;
		ShotStep = 2;
		return;
	case 2:
		if (ShotTime < Wait) return;
		Shot(TEXT("t0_up"));
		T0 = W->GetTimeSeconds();
		L->TestSkyMovers(Sky0, Yaw0);
		Picture0 = L->TestSkyPictureNow();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skyscroll sample t0 at %.3f s: dome yaw %.6f, %d numbers, Spin %.6f CloudSpin %.6f wind %.6f %.6f, picture %s"), T0, Yaw0, Sky0.Num(),
			Sky0.IsValidIndex(0) ? Sky0[0] : -1.f, Sky0.IsValidIndex(1) ? Sky0[1] : -1.f, Sky0.IsValidIndex(2) ? Sky0[2] : -1.f, Sky0.IsValidIndex(3) ? Sky0[3] : -1.f, *Picture0);
		Wait = ShotTime + 28.5f;
		ShotStep = 3;
		return;
	case 3:
		if (ShotTime < Wait) return;
		Shot(TEXT("t30_horizon"));
		Wait = ShotTime + 1.5f;
		ShotStep = 5;
		return;
	case 5:
	{
		if (ShotTime < Wait) return;
		Shot(TEXT("t30_up"));
		const double T1 = W->GetTimeSeconds(), Dt30 = T1 - T0;
		TArray<float> Sky1;
		double Yaw1 = 0.0;
		L->TestSkyMovers(Sky1, Yaw1);
		const FString Picture1 = L->TestSkyPictureNow();
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skyscroll sample t30 at %.3f s: dome yaw %.6f, %d numbers, Spin %.6f CloudSpin %.6f wind %.6f %.6f, picture %s"), T1, Yaw1, Sky1.Num(),
			Sky1.IsValidIndex(0) ? Sky1[0] : -1.f, Sky1.IsValidIndex(1) ? Sky1[1] : -1.f, Sky1.IsValidIndex(2) ? Sky1[2] : -1.f, Sky1.IsValidIndex(3) ? Sky1[3] : -1.f, *Picture1);
		Check(Sky0.Num() >= 4 && Sky0.Num() == Sky1.Num(), FString::Printf(TEXT("a sky to read: %d numbers (the sky's and %d walls')"), Sky1.Num(), Sky1.Num() / 4 - 1));
		Check(Dt30 > 29.f && Dt30 < 33.f, FString::Printf(TEXT("the samples %.3f s apart"), Dt30));
		bool bSame = Yaw0 == Yaw1 && Sky0.Num() == Sky1.Num();
		bool bWallsWithSky = true;
		for (int32 i = 0; i < FMath::Min(Sky0.Num(), Sky1.Num()); ++i)
		{
			if (Sky0[i] != Sky1[i]) bSame = false;
			if (Sky1[i] != Sky1[i % 4]) bWallsWithSky = false;
		}
		Check(bWallsWithSky, TEXT("every outer wall's sky stands where the dome's does"));
		if (Expect.Equals(TEXT("still"), ESearchCase::IgnoreCase))
		{
			Check(L->TestSkyStill(), TEXT("the level reads as still"));
			Check(bSame, FString::Printf(TEXT("every number identical over %.1f s (dome yaw %.6f to %.6f)"), Dt30, Yaw0, Yaw1));
			// (no cloud layer, the train: the cloud numbers are the material's unused defaults)
			Check(Yaw1 == 0.0 && Sky1.Num() >= 4 && Sky1[0] == 0.f && (!L->TestSkyCloudLayer() || (Sky1[1] == 1.f && Sky1[2] == 0.25f && Sky1[3] == 0.25f)),
				FString::Printf(TEXT("the sky at its default place: dome yaw %.6f, Spin %.6f, CloudSpin %.6f, wind %.6f %.6f"), Yaw1, Sky1.IsValidIndex(0) ? Sky1[0] : -1.f, Sky1.IsValidIndex(1) ? Sky1[1] : -1.f,
					Sky1.IsValidIndex(2) ? Sky1[2] : -1.f, Sky1.IsValidIndex(3) ? Sky1[3] : -1.f));
			Check(Picture0 == Picture1 && !Picture1.Contains(TEXT("animation")), FString::Printf(TEXT("no animation on the sky's picture: %s"), *Picture1));
		}
		else if (Expect.Equals(TEXT("moves"), ESearchCase::IgnoreCase))
		{
			// the tick's own sums over the same time: the dome's -0.8 degrees a second, the wind's fixed rates, the clouds' turn with its veer
			Check(!L->TestSkyStill(), TEXT("the level reads as moving"));
			const double YawRate = FMath::FindDeltaAngleDegrees(Yaw0, Yaw1) / Dt30;
			Check(FMath::Abs(YawRate + L->SkySpinSpeed) < 2e-3, FString::Printf(TEXT("the dome turns %.6f degrees a second (today's %.6f)"), YawRate, -L->SkySpinSpeed));
			if (Sky0.Num() >= 4 && Sky1.Num() >= 4)
			{
				Check(FMath::Abs(FMath::FindDeltaAngleDegrees(FMath::RadiansToDegrees(Sky1[0]), Yaw1)) < 1e-3, FString::Printf(TEXT("Spin %.6f rad follows the dome"), Sky1[0]));
				if (L->TestSkyCloudLayer())
				{
					const double Turn = FMath::Fmod(double(Sky1[1]) - double(Sky0[1]) + 2.0 * UE_DOUBLE_PI, 2.0 * UE_DOUBLE_PI);
					const double Sum = FMath::DegreesToRadians(1.2 * (0.6 * Dt30 + 8.0 * (FMath::Cos(0.05 * T0) - FMath::Cos(0.05 * T1))));
					Check(FMath::Abs(Turn - Sum) < 1e-3, FString::Printf(TEXT("the clouds turned %.6f rad, the tick's sum %.6f"), Turn, Sum));
					const double WindX = FMath::Frac(double(Sky1[2]) - double(Sky0[2]) + 1.0) / Dt30, WindY = FMath::Frac(double(Sky1[3]) - double(Sky0[3]) + 1.0) / Dt30;
					Check(FMath::Abs(WindX - 0.012) < 2e-5 && FMath::Abs(WindY - 0.004) < 2e-5, FString::Printf(TEXT("the wind %.6f %.6f picture widths a second (today's 0.012000 0.004000)"), WindX, WindY));
				}
			}
		}
		else Check(false, TEXT("-OLSkyScrollExpect=still|moves not given"));
		Wait = ShotTime + 2.f;
		ShotStep = 6;
		return;
	}
	case 6:
	{
		if (ShotTime < Wait) return;
		Diff(TEXT("horizon"));
		const float Up = Diff(TEXT("up"));
		if (Expect.Equals(TEXT("still"), ESearchCase::IgnoreCase) && !L->TestSkyGradient()) Check(Up < 0.02f, FString::Printf(TEXT("the look up unchanged over 30 s: %.3f%% of it differs"), 100.f * Up));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT skyscroll RESULT %s (%d passed, %d failed)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails);
		// (a co-op host stays up for its client's own 30 s: -OLSkyScrollHold=<seconds>)
		float Hold = 0.f;
		OLFlags::Value(TEXT("OLSkyScrollHold"), Hold);
		Wait = ShotTime + Hold;
		ShotStep = 7;
		return;
	}
	case 7:
		if (ShotTime < Wait) return;
		ShotStep = -1;
		FGenericPlatformMisc::RequestExit(false);
		return;
	default:
		return;
	}
}

#endif // WITH_OUTLAWS_TESTS
