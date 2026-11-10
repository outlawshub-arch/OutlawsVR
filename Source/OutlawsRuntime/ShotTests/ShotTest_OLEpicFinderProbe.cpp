// -OLShots -OLEpicFinderProbe -OLTestEpicManifests=<dir> (REQ-EPIC: the install finder reads the Epic Games Launcher's manifests too,
// after Steam and GOG).  Writes fake *.item manifests into <dir> and fake install folders beside it (<dir>\..\EpicFake: empty files
// named olgeo.lab, olobj.lab and oltex.lab, which is all LooksLikeInstall reads; no game files), then asks
// OutlawsInstallWin::EpicFolders.  Cases: an Outlaws manifest whose folder holds the LABs is found, and first; a manifest of another
// name whose folder holds them is found too (the folder decides, not the name); another game's folder, a missing folder, a
// malformed manifest and an incomplete install are not.  Lines "OLSHOT epicfinder check <what>: PASS|FAIL" and
// "OLSHOT epicfinder RESULT <n> PASS, <m> FAIL".  -OLOldInstallFinder (as before: no Epic) must FAIL.

#include "ShotTestCommon.h"

#if WITH_OUTLAWS_TESTS && PLATFORM_WINDOWS
#include "OutlawsInstallWindows.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

namespace
{
	FString EpicProbeNorm(FString D)
	{
		FPaths::NormalizeDirectoryName(D);
		return D;
	}

	bool EpicProbeWriteItem(const FString& Dir, const FString& File, const FString& Name, const FString& Location, bool bIncomplete)
	{
		// the fields a real manifest has (read from the launcher's own *.item files), the path in Windows form as the launcher writes it
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetNumberField(TEXT("FormatVersion"), 0);
		J->SetBoolField(TEXT("bIsIncompleteInstall"), bIncomplete);
		J->SetStringField(TEXT("LaunchExecutable"), TEXT("outlaws.exe"));
		J->SetStringField(TEXT("DisplayName"), Name);
		J->SetStringField(TEXT("InstallLocation"), FPaths::ConvertRelativePathToFull(Location).Replace(TEXT("/"), TEXT("\\")));
		J->SetStringField(TEXT("AppName"), TEXT("probe") + FPaths::GetBaseFilename(File));
		J->SetStringField(TEXT("CatalogItemId"), TEXT("00000000000000000000000000000000"));
		J->SetStringField(TEXT("CatalogNamespace"), TEXT("probe"));
		FString Text;
		TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Text);
		return FJsonSerializer::Serialize(J, W) && FFileHelper::SaveStringToFile(Text, *FPaths::Combine(Dir, File));
	}

	void EpicProbeLabs(const FString& Dir)
	{
		IFileManager::Get().MakeDirectory(*Dir, true);
		for (const TCHAR* Lab : { TEXT("olgeo.lab"), TEXT("olobj.lab"), TEXT("oltex.lab") })
		{
			FFileHelper::SaveStringToFile(FString(), *FPaths::Combine(Dir, Lab));
		}
	}
}

OL_SHOT_MATCH(OLEpicFinderProbe) { return OLFlags::Param(TEXT("OLEpicFinderProbe")); }

OL_SHOT_TEST(OLEpicFinderProbe, 2630)
{
	static bool bDone = false;
	if (bDone) return;
	bDone = true;
	int32 Pass = 0, Fail = 0;
	auto Check = [&Pass, &Fail](bool bOk, const TCHAR* What)
	{
		(bOk ? Pass : Fail)++;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT epicfinder check %s: %s"), What, bOk ? TEXT("PASS") : TEXT("FAIL"));
	};
	FString Given;
	if (!OLFlags::Value(TEXT("OLTestEpicManifests"), Given, false) || Given.IsEmpty())
	{
		Check(false, TEXT("-OLTestEpicManifests=<dir> given"));
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT epicfinder RESULT %d PASS, %d FAIL"), Pass, Fail);
		FGenericPlatformMisc::RequestExit(false);
		return;
	}
	const FString Dir = OutlawsInstallWin::EpicManifestsDir();
	const FString Fake = EpicProbeNorm(FPaths::ConvertRelativePathToFull(FPaths::Combine(Dir, TEXT(".."), TEXT("EpicFake"))));
	IFileManager::Get().MakeDirectory(*Dir, true);
	const FString Good = Fake / TEXT("Outlaws Remastered");
	const FString Other = Fake / TEXT("SomeApp");
	const FString Wrong = Fake / TEXT("OtherGame");
	const FString Missing = Fake / TEXT("NotThere");
	const FString Bad = Fake / TEXT("Malformed");
	const FString Partial = Fake / TEXT("Incomplete");
	EpicProbeLabs(Good);
	EpicProbeLabs(Other);
	EpicProbeLabs(Bad);
	EpicProbeLabs(Partial);
	IFileManager::Get().MakeDirectory(*Wrong, true);
	FFileHelper::SaveStringToFile(FString(TEXT("not a game file")), *FPaths::Combine(Wrong, TEXT("game.pak")));
	IFileManager::Get().DeleteDirectory(*Missing, false, true);
	bool bWrote = true;
	// file names sort the unnamed one first, so "first" needs the DisplayName order
	bWrote &= EpicProbeWriteItem(Dir, TEXT("A0SOMEAPP.item"), TEXT("Some App"), Other, false);
	bWrote &= EpicProbeWriteItem(Dir, TEXT("B1OUTLAWS.item"), TEXT("Outlaws + A Handful of Missions: Remaster"), Good, false);
	bWrote &= EpicProbeWriteItem(Dir, TEXT("C2OTHERGAME.item"), TEXT("Other Game"), Wrong, false);
	bWrote &= EpicProbeWriteItem(Dir, TEXT("D3MISSING.item"), TEXT("Outlaws Missing"), Missing, false);
	bWrote &= EpicProbeWriteItem(Dir, TEXT("E4INCOMPLETE.item"), TEXT("Outlaws Incomplete"), Partial, true);
	// a manifest cut short: its folder holds the LABs, so only a parser that took half a file would find it
	const FString Cut = FString::Printf(TEXT("{\n\t\"FormatVersion\": 0,\n\t\"DisplayName\": \"Outlaws Malformed\",\n\t\"InstallLocation\": \"%s\",\n\t\"AppName\": "),
		*Bad.Replace(TEXT("/"), TEXT("\\\\")));
	bWrote &= FFileHelper::SaveStringToFile(Cut, *FPaths::Combine(Dir, TEXT("F5MALFORMED.item")));
	Check(bWrote, TEXT("fake manifests written"));
	TArray<FString> Found;
	OutlawsInstallWin::EpicFolders(Found);
	auto Has = [&Found](const FString& D)
	{
		for (const FString& F : Found) if (FPaths::IsSamePath(EpicProbeNorm(F), D)) return true;
		return false;
	};
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT epicfinder found %d: %s"), Found.Num(), *FString::Join(Found, TEXT(" | ")));
	Check(Has(Good), TEXT("Outlaws manifest found"));
	Check(Found.Num() > 0 && FPaths::IsSamePath(EpicProbeNorm(Found[0]), Good), TEXT("Outlaws manifest first"));
	Check(Has(Other), TEXT("a folder holding the LABs found under another name"));
	Check(!Has(Wrong), TEXT("wrong game ignored"));
	Check(!Has(Missing), TEXT("missing folder ignored"));
	Check(!Has(Bad), TEXT("malformed manifest ignored"));
	Check(!Has(Partial), TEXT("incomplete install ignored"));
	Check(Found.Num() == 2, TEXT("nothing else found"));
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT epicfinder RESULT %d PASS, %d FAIL"), Pass, Fail);
	FGenericPlatformMisc::RequestExit(false);
}

#endif // WITH_OUTLAWS_TESTS && PLATFORM_WINDOWS
