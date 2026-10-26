// -OLShots -OLSaveTest: one of the ShotTest tests (OutlawsShotTests.h).

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS

OL_SHOT_MATCH(OLSaveTest) { return OLFlags::Param(TEXT("OLSaveTest")); }

OL_SHOT_TEST(OLSaveTest, 470)
{
	// save to slot 1, the save / load / gameplay pages, walk off, load, and check it came back
	auto Shot = [](const TCHAR* Name) { FScreenshotRequest::RequestScreenshot(FPaths::ProjectSavedDir() / TEXT("OLShots") / FString(Name) + TEXT(".png"), false, false); };
	static FVector SavedAt = FVector::ZeroVector;
	static int32 SavedBullets = 0;
	const float Due[] = { 3.f, 3.5f, 4.5f, 5.f, 6.f, 6.5f, 7.5f, 8.f, 8.5f, 11.5f, 12.5f };
	// look level at the menu every frame while it is up (a stray mouse on the test window turns the view)
	if (ShotStep >= 2 && ShotStep <= 7 && Controller) Controller->SetControlRotation(FRotator(0.0, MenuYaw, 0.0));
	if (ShotStep >= UE_ARRAY_COUNT(Due) || ShotTime < Due[ShotStep]) return;
	switch (ShotStep)
	{
	case 0:
		SavedAt = GetActorLocation();
		if (L) { SavedBullets = L->GetCount(TEXT("ibullit")); UE_LOG(LogOutlaws, Display, TEXT("OLRT save test: save %d, at %s, bullets %d"), L->SaveGame(0) ? 1 : 0, *SavedAt.ToString(), SavedBullets); }
		break;
	case 1: OpenMenu(); ShowMenuPage(TEXT("save_game.rml"), true); MenuHover[1] = 1; RedrawMenu(); break;
	case 2: Shot(TEXT("ol_save")); break;
	case 3: ShowMenuPage(TEXT("load_game.rml"), false); MenuHover[1] = 0; RedrawMenu(); break;
	case 4: Shot(TEXT("ol_load")); break;
	case 5: ShowMenuPage(TEXT("options_gameplay.rml"), false); MenuHover[1] = -1; RedrawMenu(); break;
	case 6: Shot(TEXT("ol_gameplay")); break;
	case 7:
		CloseMenu();
		TeleportTo(SavedAt + FVector(250.0, 0.0, 0.0), GetActorRotation(), false, true);
		if (L) L->TakeCount(TEXT("ibullit"), 5);
		break;
	case 8: if (L) L->LoadGame(0); break;
	case 9:
		UE_LOG(LogOutlaws, Display, TEXT("OLRT save test: loaded at %s (saved %s, off by %.0f cm), bullets %d (saved %d)"), *GetActorLocation().ToString(), *SavedAt.ToString(),
			FVector::Dist(GetActorLocation(), SavedAt), L ? L->GetCount(TEXT("ibullit")) : -1, SavedBullets);
		Shot(TEXT("ol_loaded"));
		break;
	default: FGenericPlatformMisc::RequestExit(false); ShotTime = -1.f; return;
	}
	++ShotStep;
}

#endif // WITH_OUTLAWS_TESTS
