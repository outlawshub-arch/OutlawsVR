// -OLShots -OLMovie43Test: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"
#include "OutlawsMovieScreen.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLMovie43Test) { return OLFlags::Param(TEXT("OLMovie43Test")); }

OL_SHOT_TEST(OLMovie43Test, 1320)
{
	// (REQ-MOVIE43, the planner 31 Oct: testers with the original 4:3 cutscenes saw them stretched to 16:9) the movie screen
	// takes the movie's own shape from the media: a 4:3 test movie (1440 x 1080) and a 16:9 one (1920 x 1080), made with
	// ffmpeg's testsrc into -OLMovie43Dir= (m43_4x3.mp4, m43_16x9.mp4; default <project>\out\movie43), each on its own screen
	// as a level plays a cutscene (the level cleared, the camera unfaded), then the 4:3 one again under the end credits'
	// roll (the screen 16:9 again, the picture pillarboxed in the roll's canvas), then one of the player's own converted
	// cutscenes (-OLMovie43Real=, in_shb by default) as before.  Each is shot 2 s in (ol_movie43_<case>.png, in Saved\OLShots),
	// its screen's size checked: 240 x 180 cm for 4:3, 320 x 180 for 16:9 (with -OLOldMovie43 all 320 x 180).
	struct FMovieCase { const TCHAR* Name; const TCHAR* File; float Want; bool bCredits; };
	static const FMovieCase MovieCases[] = {
		{ TEXT("4x3"), TEXT("m43_4x3.mp4"), 4.f / 3.f, false },
		{ TEXT("16x9"), TEXT("m43_16x9.mp4"), 16.f / 9.f, false },
		{ TEXT("4x3_credits"), TEXT("m43_4x3.mp4"), 4.f / 3.f, true },
		{ TEXT("real"), nullptr, 16.f / 9.f, false },
	};
	constexpr int32 NumCases = UE_ARRAY_COUNT(MovieCases);
	static int32 Passes = 0, Fails = 0;
	static float CaseAt = 0.f;
	static TWeakObjectPtr<AOutlawsMovieScreen> Screen;
	static const bool bOld = OLFlags::Param(TEXT("OLOldMovie43"));
	if (!L || ShotTime < 3.f || (ShotStep == 0 && (L->IsMoviePlaying() || L->IsRebuildPending()))) return;
	auto Verdict = [&](bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT movie43 %s: %s"), *What, bOk ? TEXT("PASS") : TEXT("FAIL"));
		if (bOk) ++Passes; else ++Fails;
	};
	if (ShotStep == 0)
	{
		// as PlayMovie stands a cutscene: the level cleared away, the player held (the level is up: the camera not faded)
		L->Clear();
		GetCharacterMovement()->DisableMovement();
		GetCharacterMovement()->Velocity = FVector::ZeroVector;
		ShotStep = 1;
		return;
	}
	const int32 Ci = (ShotStep - 1) / 2;
	if ((ShotStep - 1) % 2 == 0 && ShotStep > 1 && ShotTime - CaseAt < 1.f) return;     // (the last shot taken before its screen goes)
	if (Ci >= NumCases)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT movie43 RESULT %s (%d passed, %d failed)%s"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails,
			bOld ? TEXT(" -OLOldMovie43") : TEXT(""));
		FGenericPlatformMisc::RequestExit(false);
		ShotTime = -1.f;
		return;
	}
	const FMovieCase& K = MovieCases[Ci];
	if ((ShotStep - 1) % 2 == 0)
	{
		// the case's screen, where the level's own would stand
		if (Screen.IsValid()) Screen->Destroy();
		FString Dir = FPaths::ProjectDir() / TEXT("out/movie43"), Real = TEXT("in_shb");
		OLFlags::Value(TEXT("OLMovie43Dir"), Dir);
		OLFlags::Value(TEXT("OLMovie43Real"), Real);
		const FString File = K.File ? FPaths::ConvertRelativePathToFull(Dir / K.File) : L->DataFile(TEXT("Movies"), Real + TEXT(".mp4"));
		FActorSpawnParameters SP;
		SP.ObjectFlags |= RF_Transient;
		Screen = GetWorld()->SpawnActor<AOutlawsMovieScreen>(AOutlawsMovieScreen::StaticClass(), FTransform::Identity, SP);
		if (Screen.IsValid())
		{
			if (K.bCredits)
			{
				TArray<FOutlawsCreditLine> Lines;
				for (const TCHAR* T : { TEXT("~OUTLAWS VR"), TEXT("REQ-MOVIE43"), TEXT(""), TEXT("~THE ORIGINAL CUTSCENES"), TEXT("AT THEIR OWN SHAPE") })
				{
					FOutlawsCreditLine C;
					C.bHeading = T[0] == TEXT('~');
					C.Text = C.bHeading ? FString(T + 1) : FString(T);
					Lines.Add(C);
				}
				Screen->ArmCredits(Lines, nullptr, 0.5, 60.0);
			}
			Screen->SetVolume(0.f);
			Screen->Play(File, Camera->GetComponentLocation(), Camera->GetForwardVector());
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT movie43 case %s: %s%s"), K.Name, *File, FPaths::FileExists(File) ? TEXT("") : TEXT(" (MISSING)"));
		CaseAt = ShotTime;
		++ShotStep;
		return;
	}
	// 2 s of picture (the media open and going), at most 12 s
	AOutlawsMovieScreen* S = Screen.Get();
	const bool bReady = S && S->IsPlaying() && S->GetPictureAspect() > 0.f && S->GetTime() > 2.f && (!K.bCredits || S->IsCreditsRolling());
	if (!bReady && ShotTime - CaseAt < 12.f) return;
	FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString::Printf(TEXT("ol_movie43_%s%s.png"), K.Name, bOld ? TEXT("_old") : TEXT("")), false, false);
	const FVector2D Size = S ? S->GetScreenSize() : FVector2D::ZeroVector;
	// the screen: the picture's shape (16:9 under the roll, whose canvas pillarboxes it), 180 cm high
	const float WantScreen = bOld || K.bCredits ? 16.f / 9.f : K.Want;
	const float WantPicture = bOld ? 16.f / 9.f : K.Want;
	const bool bOk = bReady && FMath::IsNearlyEqual(float(Size.Y), 180.f, 0.5f) && FMath::IsNearlyEqual(float(Size.X), 180.f * WantScreen, 0.5f)
		&& FMath::IsNearlyEqual(S->GetPictureAspect(), WantPicture, 0.01f);
	Verdict(bOk, FString::Printf(TEXT("%s: picture %.3f (want %.3f), screen %.0f x %.0f cm (want %.0f x 180), %.1f s in%s"), K.Name, S ? S->GetPictureAspect() : 0.f, WantPicture,
		Size.X, Size.Y, 180.f * WantScreen, S ? S->GetTime() : 0.f, bReady ? TEXT("") : TEXT(", NOT PLAYING")));
	CaseAt = ShotTime;
	++ShotStep;
}

#endif // WITH_OUTLAWS_TESTS
