// -OLShots -OLRecordPlayProbe (REQ-BOTMIMIC, 9 Nov: the play recorder's guard).  A host match with bots:
//   -OLShots -OLMpHost=town_2p -OLMpClass=dm -OLMpBots=3 -OLMpBotChars=Jam,Jam,Jam -OLMpChar=Bob -OLMpMinutes=30 -OLMpFrags=0
//   -OLRecordPlayProbe [-OLRecordPlay] [-OLRecordPlaySeconds=40]
// The match started, the player looks at the nearest living bot, walks at it while it is over 10 m off, steps side to side, and
// pulls the dominant hand's trigger every 1.5 s with the gun's bore on the bot's body.  Then the recorder's file is flushed and
// read: it exists under Saved/PlayRecord with the <yyyymmdd_hhmmss>_<map>.jsonl name, the header parses with its tags (the
// character, the map, the dial, the pace, the speed), there are 10 movement samples a second with their columns, a line per
// shot with its columns, a "see", and none of the forbidden things (the machine's user name and computer name, a "Users"
// folder, a drive path, the loopback address, the player's name given by -OLMpName, a bot's name).
// After the play, in this order (the bots stilled from the retreat's start on: they stand and hold their fire):
//   retreat  a bot found in clear sight, health set to 3, a walk backward away from it for 3.5 s, eyes on it: the plan's
//            retreat (section 5: the distance to the last seen opponent rising over 2 s) is in the mv lines' od, from a
//            sample with hp 3
//   room     4 s standing still with the camera swaying in the play space (a person's steps in the room, faked as the
//            other probes move the head: the camera's place under VROrigin), shooting: the mv lines carry that room walk
//            (rm, rf, rs) with no stick speed, a shot says mov 2; and the stick walk had no room walk (no double count)
//   heal     health from 3 to 8 alive: a heal line, hp0 3 and hp1 8
//   died     the player killed: a died line after the heal, then (the host respawns after 2 s) a spawn line
// Lines "OLSHOT recordplay case PASS|FAIL: ...", the size per minute, then "OLSHOT recordplay RESULT ALL PASS|FAIL".
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
	int32 Stage = 0, Passes = 0, Fails = 0, Shots = 0, Phase = 0;
	double StartAt = 0.0, WaitFrom = 0.0, NextShot = 0.0, PhaseAt = 0.0, RetreatAt = -1.0, SeenSince = -1.0;
	FVector EyeBase = FVector::ZeroVector, RetreatFrom = FVector::ZeroVector;
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
	L->bEnemiesActive = Phase == 0 || (Phase == 2 && RetreatAt < 0.0);     // (ShotTest stills them each tick: the bots' brains are
	                                    // bad guys'; they play until a retreat begins, still after it)
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
	// the nearest living bot over 1.5 m off
	auto Nearest = [&](double& Best) -> FVector
	{
		Best = 1e9;
		FVector Look = FVector::ZeroVector;
		for (int32 Who = 0; Who < UOutlawsCoop::MaxPlayers; ++Who)
		{
			if (Who == C->MyIndex()) continue;
			const AOutlawsRemotePlayer* R = C->PuppetOf(Who);
			if (!R || R->IsHidden() || R->PlateDead()) continue;
			const double D = FVector::Dist(R->ShownBody(), Eye);
			if (D > 150.0 && D < Best) { Best = D; Look = R->ShownBody(); }
		}
		return Look;
	};
	auto Shoot = [&](const FVector& Look)
	{
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
	};
	if (Phase == 0 && Now - StartAt >= Seconds)
	{
		Phase = 2;
		PhaseAt = Now;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT recordplay the play done at %.1f s: the retreat next"), Now - StartAt);
	}
	if (Phase == 0)
	{
		// the eyes on the nearest living bot, a walk at it over 10 m, a step to the side that turns every 1.2 s
		double Best = 1e9;
		const FVector Look = Nearest(Best);
		if (Look.IsZero() || L->IsPlayerDead()) return;
		const FVector Flat = (Look - Eye).GetSafeNormal2D();
		PC->SetControlRotation((Look - Eye).Rotation());
		if (Best > 1000.0) AddMovementInput(Flat, 1.f);
		const double Side = FMath::Fmod(Now - StartAt, 2.4) < 1.2 ? 1.0 : -1.0;
		AddMovementInput(FVector::CrossProduct(FVector::UpVector, Flat) * Side, 0.6f);
		Shoot(Look);
		return;
	}
	if (Phase == 1)
	{
		// the room walk: no stick, the body settles for 1 s, then the camera sways in the play space for 4 s (45 cm side to side
		// every 2 s, 25 cm back and forth every 4 s: about 1 m/s, a person's steps), shooting at the bot; it ends where both
		// swings are back at their start, so putting the camera back is no jump.  The camera is put
		// there at the end of each tick, after the body's own eye height is set, so the recorder (early in the next) reads it.
		const double Tp = Now - PhaseAt;
		double Best = 1e9;
		const FVector Look = Nearest(Best);
		if (!Look.IsZero() && !L->IsPlayerDead()) PC->SetControlRotation((Look - Eye).Rotation());
		if (Tp < 1.0) return;
		if (Tp < 5.0)
		{
			const double U = Tp - 1.0;
			Camera->SetRelativeLocation(FVector(EyeBase.X + 25.0 * FMath::Sin(UE_DOUBLE_PI * 0.5 * U), EyeBase.Y + 45.0 * FMath::Sin(UE_DOUBLE_PI * U), Camera->GetRelativeLocation().Z));
			if (!Look.IsZero() && !L->IsPlayerDead()) Shoot(Look);
			return;
		}
		Camera->SetRelativeLocation(FVector(EyeBase.X, EyeBase.Y, Camera->GetRelativeLocation().Z));
		L->TestSetHealth(3.f);
		Phase = 3;
		PhaseAt = Now;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT recordplay the room walk done; the heal from 3 to 8 next (dead %d)"), L->IsPlayerDead() ? 1 : 0);
		return;
	}
	if (Phase == 2)
	{
		// the retreat: first a bot in clear sight (the bots still play: walk at the nearest until one is, up to 20 s), held
		// 0.5 s so the recorder engages it; then health 3, the bots stilled, and backward away from it for 3.5 s, the eyes on
		// it (the recorder keeps the engaged one 5 s after it was last seen, so its od is written all the way)
		const double Tp = Now - PhaseAt;
		if (L->IsPlayerDead()) return;
		if (RetreatAt < 0.0)
		{
			double Best = 1e9, BestSeen = 1e9;
			const FVector Look = Nearest(Best);
			FVector Seen = FVector::ZeroVector;
			for (int32 Who = 0; Who < UOutlawsCoop::MaxPlayers; ++Who)
			{
				if (Who == C->MyIndex()) continue;
				const AOutlawsRemotePlayer* R = C->PuppetOf(Who);
				if (!R || R->IsHidden() || R->PlateDead()) continue;
				const double D = FVector::Dist(R->ShownBody(), Eye);
				if (D < 150.0 || D > 4000.0 || D >= BestSeen) continue;
				FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsRecordPlayProbeSight), false, this);
				Q.AddIgnoredActor(R);
				if (const AOutlawsSpriteActor* Sp = R->ShownSprite()) Q.AddIgnoredActor(Sp);
				FHitResult Hit;
				if (World->LineTraceSingleByChannel(Hit, Eye, R->ShownBody(), ECC_Visibility, Q)) continue;
				BestSeen = D;
				Seen = R->ShownBody();
			}
			if (!Seen.IsZero())
			{
				PC->SetControlRotation((Seen - Eye).Rotation());
				if (SeenSince < 0.0) SeenSince = Now;
				if (Now - SeenSince >= 0.5 || Tp >= 20.0) { RetreatAt = Now; RetreatFrom = Seen; L->TestSetHealth(3.f); }
				return;
			}
			SeenSince = -1.0;
			if (Look.IsZero()) return;
			PC->SetControlRotation((Look - Eye).Rotation());
			if (Tp < 20.0) { AddMovementInput((Look - Eye).GetSafeNormal2D(), 1.f); return; }
			RetreatAt = Now;
			RetreatFrom = Look;
			L->TestSetHealth(3.f);
			UE_LOG(LogOutlaws, Display, TEXT("OLSHOT recordplay no bot in clear sight in 20 s: the retreat from the nearest"));
		}
		if (Now - RetreatAt < 3.5)
		{
			PC->SetControlRotation((RetreatFrom - Eye).Rotation());
			AddMovementInput(-(RetreatFrom - Eye).GetSafeNormal2D(), 1.f);
			return;
		}
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT recordplay the retreat done, %.0f cm from the bot (%.1f s to find one in sight, health %.1f); the room walk next"),
			FVector::Dist(RetreatFrom, Eye), RetreatAt - PhaseAt, L->GetHealth());
		Phase = 1;
		PhaseAt = Now;
		EyeBase = Camera->GetRelativeLocation();
		return;
	}
	if (Phase == 3)
	{
		// the heal: health 3 (set as the room walk ended), then 8 a moment later, then the kill
		if (Now - PhaseAt < 0.3) return;
		if (Now - PhaseAt < 0.8) { if (L->GetHealth() < 7.f) L->TestSetHealth(8.f); return; }
		const bool bWasInv = L->bInvincible;
		L->bInvincible = false;
		L->DamagePlayer(1e6f, 1.f, true);
		L->bInvincible = bWasInv;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT recordplay the player killed (dead %d); the respawn next"), L->IsPlayerDead() ? 1 : 0);
		Phase = 4;
		PhaseAt = Now;
		return;
	}
	if (Phase == 4)
	{
		// up to 8 s for the respawn, then 1 s alive so the spawn's sample is in
		if (L->IsPlayerDead() && Now - PhaseAt < 8.0) { WaitFrom = Now; return; }
		if (!L->IsPlayerDead() && Now - WaitFrom < 1.0) return;
		UE_LOG(LogOutlaws, Display, TEXT("OLSHOT recordplay %s after %.1f s"), L->IsPlayerDead() ? TEXT("still dead") : TEXT("respawned"), Now - PhaseAt);
		Phase = 5;
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
	int32 RoomOnly = 0, RoomStick = 0, RoomBad = 0, StickWalk = 0, StickRoom = 0, ShotStick = 0, ShotRoom = 0;
	double LastT = 0.0, HealT = -1.0, HealHp0 = -1.0, HealHp1 = -1.0, DiedT = -1.0, SpawnT = -1.0;
	struct FMv { double T, Od, Hp; bool bOd; };
	TArray<FMv> Mvs;
	TSharedPtr<FJsonObject> Hdr;
	FString Kinds;
	for (int32 i = 0; i < Lines.Num(); ++i)
	{
		const TSharedPtr<FJsonObject> O = Parse(Lines[i]);
		if (!O.IsValid() || !O->HasField(TEXT("k"))) { ++Bad; continue; }
		const FString K = O->GetStringField(TEXT("k"));
		if (i == 0 && K == TEXT("hdr")) Hdr = O;
		if (O->HasField(TEXT("t"))) LastT = FMath::Max(LastT, O->GetNumberField(TEXT("t")));
		if (K == TEXT("mv"))
		{
			++Mv;
			if (HasAll(O, { TEXT("t"), TEXT("f"), TEXT("s"), TEXT("rm"), TEXT("rf"), TEXT("rs"), TEXT("g"), TEXT("hp"), TEXT("w") }))
			{
				++MvCols;
				const double Stick = FVector2D(O->GetNumberField(TEXT("f")), O->GetNumberField(TEXT("s"))).Size();
				const double Rm = O->GetNumberField(TEXT("rm")), Rp = FVector2D(O->GetNumberField(TEXT("rf")), O->GetNumberField(TEXT("rs"))).Size();
				if (Rm >= 50.0) { if (Stick < 30.0) ++RoomOnly; else ++RoomStick; if (FMath::Abs(Rp - Rm) > 2.0) ++RoomBad; }
				if (Stick >= 150.0) { ++StickWalk; if (Rm > 5.0) ++StickRoom; }
				Mvs.Add({ O->GetNumberField(TEXT("t")), O->HasField(TEXT("od")) ? O->GetNumberField(TEXT("od")) : 0.0, O->GetNumberField(TEXT("hp")), O->HasField(TEXT("od")) });
			}
		}
		else if (K == TEXT("heal") && HealT < 0.0) { HealT = O->GetNumberField(TEXT("t")); HealHp0 = O->GetNumberField(TEXT("hp0")); HealHp1 = O->GetNumberField(TEXT("hp1")); }
		else if (K == TEXT("died") && HealT >= 0.0 && DiedT < 0.0) DiedT = O->GetNumberField(TEXT("t"));
		else if (K == TEXT("spawn") && DiedT >= 0.0 && SpawnT < 0.0) SpawnT = O->GetNumberField(TEXT("t"));
		else if (K == TEXT("shot"))
		{
			++Shot;
			if (HasAll(O, { TEXT("t"), TEXT("w"), TEXT("wl"), TEXT("pel"), TEXT("sc"), TEXT("f"), TEXT("s"), TEXT("rm"), TEXT("mov"), TEXT("tgt"), TEXT("hit"), TEXT("head") })) ++ShotCols;
			if (O->HasField(TEXT("mov"))) { const int32 Mo = O->GetIntegerField(TEXT("mov")); if (Mo == 1) ++ShotStick; if (Mo == 2) ++ShotRoom; }
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
	Check(RoomOnly >= 25 && RoomBad == 0, FString::Printf(TEXT("recorded: the room walk (rm 50 cm/s or more) with the stick still in %d samples (25 wanted; %d with stick speed too, %d whose rf and rs do not make rm)"), RoomOnly, RoomStick, RoomBad));
	Check(StickWalk >= 20 && StickRoom == 0, FString::Printf(TEXT("recorded: the stick walk is not counted as room walk (%d samples at 150 cm/s or more by the stick, %d of them with rm over 5)"), StickWalk, StickRoom));
	Check(ShotRoom >= 1 && ShotStick >= 1, FString::Printf(TEXT("recorded: the shot says how it moved (%d shots mov 2, the room; %d mov 1, the stick)"), ShotRoom, ShotStick));
	{
		// the plan's retreat (section 5): the distance to the last seen opponent rising over 2 s (20 samples), from a sample with hp 3
		int32 Best = 0;
		double Rise = 0.0, Hp0 = -1.0;
		for (int32 a = 0; a < Mvs.Num(); ++a)
		{
			if (!Mvs[a].bOd || Mvs[a].Hp > 3.5) continue;
			int32 b = a;
			while (b + 1 < Mvs.Num() && Mvs[b + 1].bOd && Mvs[b + 1].Od >= Mvs[b].Od - 30.0) ++b;
			if (b - a > Best && Mvs[b].Od - Mvs[a].Od >= 150.0) { Best = b - a; Rise = Mvs[b].Od - Mvs[a].Od; Hp0 = Mvs[a].Hp; }
			a = b;
		}
		Check(Best >= 20, FString::Printf(TEXT("recorded: a retreat (od rising %d samples, %.0f cm, from hp %.1f; 20 samples and 150 cm from hp 3.5 or less wanted)"), Best, Rise, Hp0));
	}
	Check(HealT >= 0.0 && FMath::Abs(HealHp0 - 3.0) < 0.5 && FMath::Abs(HealHp1 - 8.0) < 0.5, FString::Printf(TEXT("recorded: a heal from 3 to 8 (t %.2f, hp0 %.1f, hp1 %.1f)"), HealT, HealHp0, HealHp1));
	Check(DiedT > HealT && SpawnT > DiedT, FString::Printf(TEXT("recorded: died after the heal and the spawn after it (died %.2f, spawn %.2f)"), DiedT, SpawnT));
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
