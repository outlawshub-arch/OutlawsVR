// -OLShots -OLRecordPlayProbe (REQ-BOTMIMIC, 9 Nov: the play recorder's guard).  A host match with bots:
//   -OLShots -OLMpHost=town_2p -OLMpClass=dm -OLMpBots=3 -OLMpBotChars=Jam,Jam,Jam -OLMpChar=Bob -OLMpMinutes=30 -OLMpFrags=0
//   -OLRecordPlayProbe [-OLRecordPlay] [-OLRecordPlaySeconds=40]
// The match started, the player looks at the nearest living bot, walks at it while it is over 10 m off, steps side to side, and
// pulls the dominant hand's trigger every 1.5 s with the gun's bore on the bot's body.  Then the recorder's file is flushed and
// read: it exists under Saved/PlayRecord with the <yyyymmdd_hhmmss>_<map>.jsonl name, the header parses with its tags (the
// character, the map, the dial, the pace, the speed), there are 10 movement samples a second with their columns, a line per
// shot with its columns, a "see", and none of the forbidden things (the machine's user name and computer name, a "Users"
// folder, a drive path, the loopback address, the player's name given by -OLMpName, a bot's name).  Lines
// "OLSHOT recordplay case PASS|FAIL: ...", the size per minute, then "OLSHOT recordplay RESULT ALL PASS|FAIL".
// Without -OLRecordPlay (and no RecordPlay=1 in the settings) the recorder is off: the "recorded" cases FAIL (the guard's old path).

#include "ShotTestCommon.h"
#include "OutlawsPlayRecord.h"
#include "OutlawsMpHost.h"
#include "OutlawsRemotePlayer.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

#if WITH_OUTLAWS_TESTS

namespace OutlawsRecordPlayProbeLocal
{
	int32 Stage = 0, Passes = 0, Fails = 0, Shots = 0;
	double StartAt = 0.0, WaitFrom = 0.0, NextShot = 0.0;
	TArray<FString> BotNames;

	void Check(bool bOk, const FString& What)
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT recordplay case %s: %s"), bOk ? TEXT("PASS") : TEXT("FAIL"), *What);
		if (bOk) ++Passes; else ++Fails;
	}

	TSharedPtr<FJsonObject> Parse(const FString& Line)
	{
		TSharedPtr<FJsonObject> O;
		const TSharedRef<TJsonReader<>> Rd = TJsonReaderFactory<>::Create(Line);
		if (!FJsonSerializer::Deserialize(Rd, O)) return nullptr;
		return O;
	}

	bool HasAll(const TSharedPtr<FJsonObject>& O, const TArray<const TCHAR*>& Keys)
	{
		for (const TCHAR* K : Keys) if (!O->HasField(K)) return false;
		return true;
	}
}

OL_SHOT_MATCH(OLRecordPlayProbe) { return OLFlags::Param(TEXT("OLRecordPlayProbe")); }

OL_SHOT_TEST(OLRecordPlayProbe, 1950)
{
	using namespace OutlawsRecordPlayProbeLocal;
	if (!L || Stage < 0) return;
	UWorld* World = GetWorld();
	const double Now = World ? World->GetTimeSeconds() : 0.0;
	L->StartSaveDelay = -1.f;           // (no autosaves)
	L->AutosaveWait = 0.f;
	L->bEnemiesActive = true;           // (ShotTest stills them each tick: the bots' brains are bad guys')
	UOutlawsCoop* C = UOutlawsCoop::Get(this);
	int32 Seconds = 40;
	OLFlags::Value(TEXT("OLRecordPlaySeconds"), Seconds);
	const bool bUp = C && C->IsHost() && C->IsMatch() && C->bLevelBuilt && !L->IsFrontEnd() && !L->IsRebuildPending() && L->HasActorBegunPlay();
	const int32 Hand = LeftDominant() ? 0 : 1;
	auto Finish = [&]()
	{
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT recordplay RESULT %s (%d passed, %d failed; %d shots fired)"), Fails == 0 && Passes > 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Passes, Fails, Shots);
		Stage = -1;
		FGenericPlatformMisc::RequestExit(false);
	};
	if (Stage == 0)
	{
		if (WaitFrom == 0.0) WaitFrom = FPlatformTime::Seconds();
		if (!bUp)
		{
			if (FPlatformTime::Seconds() - WaitFrom < 180.0) return;
			Check(false, TEXT("needs a launched host with bots: -OLMpHost=<map> -OLMpClass=dm -OLMpBots=3"));
			Finish();
			return;
		}
		TArray<UOutlawsCoop::FBotProbeView> Views;
		C->DebugBotViews(Views);
		if (Views.Num() == 0) return;
		FOutlawsMatchFlow& Fl = OutlawsMpHost::Flow();
		if (Fl.GetState() == FOutlawsMatchFlow::EState::WarmUp) Fl.HostStart();
		for (const UOutlawsCoop::FBotProbeView& V : Views) if (!V.Name.IsEmpty()) BotNames.AddUnique(V.Name);
		StartAt = Now;
		NextShot = Now + 3.0;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT recordplay %d bots, %d s of play, the recorder %s"), Views.Num(), Seconds, OutlawsPlayRecord::IsRecording() ? TEXT("on") : TEXT("off"));
		Stage = 1;
		return;
	}
	if (!bUp) return;
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC || !PC->PlayerCameraManager) return;
	const FVector Eye = PC->PlayerCameraManager->GetCameraLocation();
	if (Now - StartAt < Seconds)
	{
		// the eyes on the nearest living bot, a walk at it over 10 m, a step to the side that turns every 1.2 s
		double Best = 1e9;
		FVector Look = FVector::ZeroVector;
		for (int32 Who = 0; Who < UOutlawsCoop::MaxPlayers; ++Who)
		{
			if (Who == C->MyIndex()) continue;
			const AOutlawsRemotePlayer* R = C->PuppetOf(Who);
			if (!R || R->IsHidden() || R->PlateDead()) continue;
			const double D = FVector::Dist(R->ShownBody(), Eye);
			if (D > 150.0 && D < Best) { Best = D; Look = R->ShownBody(); }
		}
		if (Look.IsZero() || L->IsPlayerDead()) return;
		const FVector Flat = (Look - Eye).GetSafeNormal2D();
		PC->SetControlRotation((Look - Eye).Rotation());
		if (Best > 1000.0) AddMovementInput(Flat, 1.f);
		const double Side = FMath::Fmod(Now - StartAt, 2.4) < 1.2 ? 1.0 : -1.0;
		AddMovementInput(FVector::CrossProduct(FVector::UpVector, Flat) * Side, 0.6f);
		if (Now >= NextShot)
		{
			NextShot = Now + 1.5;
			if (Equipped[Hand] < 0) Equip(Hand, 0);
			if (Equipped[Hand] >= 0)
			{
				Mag[Hand] = FMath::Max(Mag[Hand], 6);
				FireCooldown[Hand] = 0.f;
				TutorialTestAimGun(Hand, Look);
				TestSetTwoHand(Hand, true);
				const int32 Before = Mag[Hand];
				FireGun(Hand, false);
				TestSetTwoHand(Hand, false);
				if (Mag[Hand] < Before) ++Shots;
			}
		}
		return;
	}
	// the file, as written
	OutlawsPlayRecord::Flush();
	const FString Path = OutlawsPlayRecord::LastFile();
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(OutlawsPlayRecord::Folder() / TEXT("*.jsonl")), true, false);
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT recordplay files in the folder: %d; the recorder %s"), Files.Num(), OutlawsPlayRecord::IsRecording() ? TEXT("on") : TEXT("off"));
	Check(OutlawsPlayRecord::IsRecording(), TEXT("recorded: the recorder is on in the match"));
	FString Text;
	const bool bRead = !Path.IsEmpty() && FFileHelper::LoadFileToString(Text, *Path);
	Check(bRead && Files.Num() == 1, FString::Printf(TEXT("recorded: one file under Saved/PlayRecord (%d), read %s"), Files.Num(), bRead ? TEXT("yes") : TEXT("no")));
	if (!bRead) { Finish(); return; }
	const FString Name = FPaths::GetCleanFilename(Path);
	const FString Map = FPaths::GetBaseFilename(L->LevelName).ToLower();
	{
		bool bOk = Name.Len() > 22 && Name.EndsWith(TEXT(".jsonl")) && Name.Mid(8, 1) == TEXT("_") && Name.Mid(15, 1) == TEXT("_") && Name.Contains(Map);
		for (int32 i = 0; i < 15 && bOk; ++i) if (i != 8 && !FChar::IsDigit(Name[i])) bOk = false;
		Check(bOk, FString::Printf(TEXT("recorded: the name is <yyyymmdd_hhmmss>_<map>.jsonl (%s)"), *Name));
	}
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	int32 Bad = 0, Mv = 0, MvCols = 0, Shot = 0, ShotCols = 0, ShotTgt = 0, See = 0;
	double LastT = 0.0;
	TSharedPtr<FJsonObject> Hdr;
	FString Kinds;
	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		const TSharedPtr<FJsonObject> O = Parse(Lines[i]);
		if (!O.IsValid() || !O->HasField(TEXT("k"))) { ++Bad; continue; }
		const FString K = O->GetStringField(TEXT("k"));
		if (i == 0 && K == TEXT("hdr")) Hdr = O;
		if (O->HasField(TEXT("t"))) LastT = FMath::Max(LastT, O->GetNumberField(TEXT("t")));
		if (K == TEXT("mv")) { ++Mv; if (HasAll(O, { TEXT("t"), TEXT("f"), TEXT("s"), TEXT("g"), TEXT("hp"), TEXT("w") })) ++MvCols; }
		else if (K == TEXT("shot"))
		{
			++Shot;
			if (HasAll(O, { TEXT("t"), TEXT("w"), TEXT("wl"), TEXT("pel"), TEXT("sc"), TEXT("f"), TEXT("s"), TEXT("tgt"), TEXT("hit"), TEXT("head") })) ++ShotCols;
			if (O->GetIntegerField(TEXT("tgt")) >= 0 && O->HasField(TEXT("td")) && O->HasField(TEXT("ta"))) ++ShotTgt;
			if (Shot <= 3) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT recordplay shot line: %s"), *Lines[i].Left(240));
		}
		else if (K == TEXT("see")) ++See;
		if (!Kinds.Contains(K)) Kinds += (Kinds.IsEmpty() ? TEXT("") : TEXT(" ")) + K;
	}
	if (Lines.Num() > 0) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT recordplay header: %s"), *Lines[0].Left(300));
	if (Lines.Num() > 1) UE_LOG(LogOutlaws, Display, TEXT("OLSHOT recordplay a movement line: %s"), *Lines[FMath::Min(20, Lines.Num() - 1)].Left(240));
	Check(Bad == 0 && Lines.Num() > 0, FString::Printf(TEXT("recorded: every line is a JSON object with a kind (%d lines, %d not; kinds: %s)"), Lines.Num(), Bad, *Kinds));
	{
		FString Want;
		const bool bHdr = Hdr.IsValid() && HasAll(Hdr, { TEXT("v"), TEXT("char"), TEXT("map"), TEXT("dial"), TEXT("pace"), TEXT("speed"), TEXT("hpmax"), TEXT("hz") });
		FString CharArg = TEXT("Jam");
		OLFlags::Value(TEXT("OLMpChar"), CharArg);
		const bool bTags = bHdr && Hdr->GetStringField(TEXT("char")).Equals(CharArg, ESearchCase::IgnoreCase) && Hdr->GetStringField(TEXT("map")) == Map
			&& FMath::IsNearlyEqual(float(Hdr->GetNumberField(TEXT("dial"))), L->DialInForce(), 0.002f) && FMath::IsNearlyEqual(float(Hdr->GetNumberField(TEXT("pace"))), L->Pace(), 0.002f);
		Check(bTags, FString::Printf(TEXT("recorded: the header's tags (char %s, map %s, dial %.3f, pace %.4f wanted)"), *CharArg, *Map, L->DialInForce(), L->Pace()));
	}
	Check(Mv >= Seconds * 8 && MvCols == Mv, FString::Printf(TEXT("recorded: movement 10 a second with its columns (%d lines in %.1f s, %d with every column)"), Mv, LastT, MvCols));
	Check(Shot >= FMath::Max(3, Shots - 1) && ShotCols == Shot && ShotTgt >= 1, FString::Printf(TEXT("recorded: a line per shot with its columns (%d lines for %d fired, %d with every column, %d with a target)"), Shot, Shots, ShotCols, ShotTgt));
	Check(See >= 1, FString::Printf(TEXT("recorded: an opponent first seen (%d see lines)"), See));
	// nothing that names the machine, the person or a path
	{
		TArray<FString> Bans = { FPlatformProcess::UserName(false), FPlatformProcess::ComputerName(), TEXT("Users"), TEXT(":\\"), TEXT(":/"), TEXT("127.0.0.1"), TEXT("192.168."), TEXT("/storage/"), TEXT("/data/") };
		FString MpName;
		if (OLFlags::Value(TEXT("OLMpName"), MpName)) Bans.Add(MpName);
		for (const FString& B : BotNames) Bans.Add(B);
		FString Found;
		for (const FString& B : Bans) if (B.Len() >= 3 && Text.Contains(B, ESearchCase::IgnoreCase)) Found += (Found.IsEmpty() ? TEXT("") : TEXT(", ")) + B;
		Check(Found.IsEmpty(), FString::Printf(TEXT("no names, addresses or paths in the file (%d words looked for%s%s)"), Bans.Num(), Found.IsEmpty() ? TEXT("") : TEXT("; found: "), *Found));
	}
	const double Minutes = FMath::Max(LastT, 1.0) / 60.0;
	UE_LOG(LogOutlaws, Display, TEXT("OLSHOT recordplay size: %d bytes for %.1f s of match, %.1f KB a minute (%d shots)"), Text.Len(), LastT, Text.Len() / 1024.0 / Minutes, Shot);
	Check(Text.Len() / 1024.0 / Minutes < 200.0, FString::Printf(TEXT("recorded: small (%.1f KB a minute, under 200)"), Text.Len() / 1024.0 / Minutes));
	Finish();
}

#endif // WITH_OUTLAWS_TESTS
