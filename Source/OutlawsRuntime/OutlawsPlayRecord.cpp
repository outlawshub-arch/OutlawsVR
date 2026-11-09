// REQ-BOTMIMIC: the play recorder (what it writes: OutlawsPlayRecord.h).  It ships: none of it is test-only, and it does
// nothing unless -OLRecordPlay or RecordPlay=1 in the settings file turns it on.

#include "OutlawsPlayRecord.h"
#include "OutlawsRuntime.h"
#include "OutlawsFlags.h"
#include "OutlawsPerfTest.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsCoop.h"
#include "OutlawsRemotePlayer.h"
#include "OutlawsSpriteActor.h"
#include "Engine/World.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/CoreDelegates.h"
#include "HAL/FileManager.h"

namespace
{
	constexpr int32 MaxWho = 16;                  // UOutlawsCoop::MaxPlayers
	constexpr double SampleEvery = 0.1;           // 10 movement samples a second
	constexpr double FlushEvery = 2.0;            // the held lines go to the file every 2 s
	constexpr double SeeGap = 1.0;                // an opponent out of sight this long is "first seen" again
	constexpr double EngageHold = 5.0;            // the opponent last engaged is kept this long after it was last seen
	constexpr double SightRange = 8000.0;         // 80 m
	constexpr double ConeYaw = 50.0, ConePitch = 35.0;    // the view cone (a headset's field of view, about 100 by 70 degrees)
	constexpr double AimCone = 20.0;              // a shot's target: the opponent nearest the aim line within this
	constexpr int64 MaxBytes = 32ll * 1024 * 1024;        // a file stops growing here (a 10 minute match is far under it)
	constexpr float SpeedRef = 0.225f;            // the pace at the match speed of 100 % (the plan, section 2)

	struct FShot
	{
		double T = 0.0;
		int32 W = -1, Pel = 1, Tgt = -1, Hits = 0, Heads = 0, HWho = -1;
		bool bScoped = false, bLos = false;
		float Fw = 0.f, Sd = 0.f;
		double Td = 0.0, Ta = 0.0, Hd = 0.0;
		FString Label;
	};

	struct FRec
	{
		bool bFlag = false, bFlagRead = false, bSetting = false;
		double NextSettingAt = -1.0;
		FString Path, LastPath, Buf;
		TWeakObjectPtr<AOutlawsLevelActor> Level;
		TWeakObjectPtr<AActor> Player;
		FString Map, Char;
		float Dial = -1.f;
		double T0 = 0.0, NextSample = 0.0, NextFlush = 0.0;
		double LastSeen[MaxWho];
		bool bSeenNow[MaxWho];
		int32 Engaged = -1;
		double EngagedSeenAt = -100.0;
		float LastHp = -1.f;
		bool bWasDead = false;
		float Fw = 0.f, Sd = 0.f;     // this frame's speeds (at 100 %)
		bool bPending = false;
		FShot Shot;
		int64 Bytes = 0;
		bool bHooked = false;
		FRec() { Reset(); }
		void Reset()
		{
			for (int32 i = 0; i < MaxWho; ++i) { LastSeen[i] = -100.0; bSeenNow[i] = false; }
			Engaged = -1; EngagedSeenAt = -100.0; LastHp = -1.f; bWasDead = false; bPending = false; Char.Reset(); Dial = -1.f;
		}
	};

	FRec& Rec()
	{
		static FRec R;
		return R;
	}

	/** The map's short name: its file name only, letters, digits, '_' and '-' (a custom map's folder never gets in). */
	FString SafeMap(const FString& LevelName)
	{
		const FString Base = FPaths::GetBaseFilename(LevelName).ToLower();
		FString Out;
		for (TCHAR Ch : Base) if (FChar::IsAlnum(Ch) || Ch == TEXT('_') || Ch == TEXT('-')) Out.AppendChar(Ch);
		return Out.IsEmpty() ? FString(TEXT("map")) : Out.Left(40);
	}

	/** A character's short id as the table has it (anything else: "unk"). */
	FString SafeChar(const FString& Short)
	{
		static const TCHAR* Known[] = { TEXT("San"), TEXT("Jam"), TEXT("Two"), TEXT("Doc"), TEXT("Bob"), TEXT("Mar") };
		for (const TCHAR* K : Known) if (Short.Equals(K, ESearchCase::IgnoreCase)) return K;
		return TEXT("unk");
	}

	/** A weapon's name, letters and digits only. */
	FString SafeLabel(const TCHAR* Label)
	{
		FString Out;
		if (Label) for (const TCHAR* C = Label; *C; ++C) if (FChar::IsAlnum(*C) || *C == TEXT('-')) Out.AppendChar(*C);
		return Out.Left(24);
	}

	void WriteHeld()
	{
		FRec& R = Rec();
		if (R.Path.IsEmpty() || R.Buf.IsEmpty()) return;
		if (R.Bytes < MaxBytes)
		{
			FFileHelper::SaveStringToFile(R.Buf, *R.Path, FFileHelper::EEncodingOptions::ForceAnsi, &IFileManager::Get(), FILEWRITE_Append);
			R.Bytes += R.Buf.Len();
		}
		R.Buf.Reset();
	}

	void Line(const FString& S)
	{
		FRec& R = Rec();
		R.Buf += S;
		R.Buf += TEXT("\n");
	}

	double Now(const AOutlawsLevelActor* L)
	{
		const UWorld* W = L ? L->GetWorld() : nullptr;
		return W ? W->GetTimeSeconds() : 0.0;
	}

	void WritePendingShot()
	{
		FRec& R = Rec();
		if (!R.bPending) return;
		R.bPending = false;
		const FShot& S = R.Shot;
		FString L = FString::Printf(TEXT("{\"k\":\"shot\",\"t\":%.2f,\"w\":%d,\"wl\":\"%s\",\"pel\":%d,\"sc\":%d,\"f\":%d,\"s\":%d,\"tgt\":%d"),
			S.T, S.W, *S.Label, S.Pel, S.bScoped ? 1 : 0, FMath::RoundToInt(S.Fw), FMath::RoundToInt(S.Sd), S.Tgt);
		if (S.Tgt >= 0) L += FString::Printf(TEXT(",\"td\":%.0f,\"ta\":%.1f,\"los\":%d"), S.Td, S.Ta, S.bLos ? 1 : 0);
		L += FString::Printf(TEXT(",\"hit\":%d,\"head\":%d"), S.Hits, S.Heads);
		if (S.HWho >= 0) L += FString::Printf(TEXT(",\"hwho\":%d,\"hd\":%.0f"), S.HWho, S.Hd);
		L += TEXT("}");
		Line(L);
	}

	void Close(const AOutlawsLevelActor* L, const TCHAR* Why)
	{
		FRec& R = Rec();
		if (R.Path.IsEmpty()) return;
		WritePendingShot();
		Line(FString::Printf(TEXT("{\"k\":\"end\",\"t\":%.2f}"), L ? Now(L) - R.T0 : 0.0));
		WriteHeld();
		UE_LOG(LogOutlaws, Log, TEXT("OLRT play record closed (%s): %s, %lld bytes"), Why, *FPaths::GetCleanFilename(R.Path), R.Bytes);
		R.LastPath = R.Path;
		R.Path.Reset();
		R.Level.Reset();
		R.Reset();
	}

	/** On: -OLRecordPlay, or RecordPlay=1 (or true) in the settings file, read again every 5 s until a file opens. */
	bool Wanted(double RealNow)
	{
		FRec& R = Rec();
		if (!R.bFlagRead)
		{
			R.bFlagRead = true;
			R.bFlag = OLFlags::Param(TEXT("OLRecordPlay"));
		}
		if (R.bFlag) return true;
		if (RealNow >= R.NextSettingAt)
		{
			R.NextSettingAt = RealNow + 5.0;
			const TMap<FString, FString> S = AOutlawsLevelActor::ReadSettings();
			const FString* V = S.Find(TEXT("RecordPlay"));
			R.bSetting = V && (V->Equals(TEXT("1")) || V->ToBool());
		}
		return R.bSetting;
	}

	void Hook()
	{
		FRec& R = Rec();
		if (R.bHooked) return;
		R.bHooked = true;
		// (the game closing, or the headset sending it to the background: what is held is written then)
		FCoreDelegates::OnEnginePreExit.AddLambda([]() { OutlawsPlayRecord::Flush(); });
		FCoreDelegates::ApplicationWillEnterBackgroundDelegate.AddLambda([]() { OutlawsPlayRecord::Flush(); });
	}

	bool ClearLine(UWorld* W, const FVector& From, const FVector& To, const AActor* Me, const AOutlawsRemotePlayer* R)
	{
		FCollisionQueryParams Q(SCENE_QUERY_STAT(OutlawsPlayRecordSight), false, Me);
		Q.AddIgnoredActor(R);
		if (const AOutlawsSpriteActor* S = R->ShownSprite()) Q.AddIgnoredActor(S);
		FHitResult Hit;
		return !W->LineTraceSingleByChannel(Hit, From, To, ECC_Visibility, Q);
	}

	bool Living(const AOutlawsRemotePlayer* R) { return R && !R->IsHidden() && !R->PlateDead(); }
}

FString OutlawsPlayRecord::Folder()
{
	return FPaths::ProjectSavedDir() / TEXT("PlayRecord");
}

bool OutlawsPlayRecord::IsRecording()
{
	return !Rec().Path.IsEmpty();
}

FString OutlawsPlayRecord::LastFile()
{
	const FRec& R = Rec();
	return R.Path.IsEmpty() ? R.LastPath : R.Path;
}

void OutlawsPlayRecord::Flush()
{
	WritePendingShot();
	WriteHeld();
}

void OutlawsPlayRecord::Tick(AOutlawsPlayer* P, AOutlawsLevelActor* L, const FFrame& F)
{
	FRec& R = Rec();
	const bool bMatch = P && L && F.bMatchLive && !L->IsFrontEnd() && !L->IsRebuildPending() && UOutlawsCoop::IsMatchActive(L);
	const bool bOn = bMatch && !OutlawsPerfTest::IsOn() && Wanted(FPlatformTime::Seconds());
	if (!R.Path.IsEmpty() && (!bOn || R.Level.Get() != L || R.Map != SafeMap(L->LevelName)))
	{
		Close(R.Level.Get(), bOn ? TEXT("another map") : TEXT("the match ended"));
	}
	if (!bOn) return;
	UWorld* W = L->GetWorld();
	UOutlawsCoop* C = UOutlawsCoop::Get(L);
	if (!W || !C) return;
	const double T = Now(L);
	const float Pace = FMath::Max(L->Pace(), 0.05f);
	const float Dial = L->DialInForce();
	const FString Char = SafeChar(F.Character);
	// this frame's speeds in the view's frame, at the match speed of 100 %
	{
		const FRotator Yaw(0.0, F.View.Yaw, 0.0);
		const FVector Local = Yaw.UnrotateVector(FVector(F.Velocity.X, F.Velocity.Y, 0.0)) * (SpeedRef / Pace);
		R.Fw = float(Local.X);
		R.Sd = float(Local.Y);
	}
	if (R.Path.IsEmpty())
	{
		Hook();
		R.Reset();
		R.Map = SafeMap(L->LevelName);
		R.Level = L;
		R.Player = P;
		IFileManager::Get().MakeDirectory(*Folder(), true);
		R.Path = Folder() / FString::Printf(TEXT("%s_%s.jsonl"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")), *R.Map);
		R.Bytes = 0;
		R.T0 = T;
		R.NextSample = T;
		R.NextFlush = T + FlushEvery;
		R.Char = Char;
		R.Dial = Dial;
		R.LastHp = F.Health;
		R.bWasDead = F.bDead;
#if PLATFORM_ANDROID
		const TCHAR* Plat = TEXT("android");
#else
		const TCHAR* Plat = TEXT("windows");
#endif
		Line(FString::Printf(TEXT("{\"k\":\"hdr\",\"v\":1,\"char\":\"%s\",\"map\":\"%s\",\"dial\":%.3f,\"pace\":%.4f,\"speed\":%.0f,\"hpmax\":10,\"hz\":10,\"plat\":\"%s\"}"),
			*Char, *R.Map, Dial, Pace, Dial * 200.f, Plat));
		UE_LOG(LogOutlaws, Log, TEXT("OLRT play record on (%s): %s"), R.bFlag ? TEXT("-OLRecordPlay") : TEXT("RecordPlay in the settings"), *FPaths::GetCleanFilename(R.Path));
	}
	const double Rt = T - R.T0;
	if (Char != R.Char || !FMath::IsNearlyEqual(Dial, R.Dial, 0.001f))
	{
		R.Char = Char;
		R.Dial = Dial;
		Line(FString::Printf(TEXT("{\"k\":\"tag\",\"t\":%.2f,\"char\":\"%s\",\"dial\":%.3f,\"pace\":%.4f,\"speed\":%.0f}"), Rt, *Char, Dial, Pace, Dial * 200.f));
	}
	// a shot whose pellets are all in (FireGun ran last frame) goes to the file
	WritePendingShot();
	// deaths, spawns and health picked up
	if (F.bDead && !R.bWasDead) Line(FString::Printf(TEXT("{\"k\":\"died\",\"t\":%.2f}"), Rt));
	else if (!F.bDead && R.bWasDead) Line(FString::Printf(TEXT("{\"k\":\"spawn\",\"t\":%.2f}"), Rt));
	else if (!F.bDead && R.LastHp >= 0.f && F.Health > R.LastHp + 0.05f) Line(FString::Printf(TEXT("{\"k\":\"heal\",\"t\":%.2f,\"hp0\":%.1f,\"hp1\":%.1f}"), Rt, R.LastHp, F.Health));
	R.bWasDead = F.bDead;
	R.LastHp = F.Health;
	if (T >= R.NextSample)
	{
		R.NextSample = FMath::Max(R.NextSample + SampleEvery, T - SampleEvery);
		// sight: each living opponent in the view cone with a clear line from the eyes
		const int32 Me = C->MyIndex();
		double BestSeen = 1e12;
		int32 BestWho = -1;
		for (int32 Who = 0; Who < MaxWho; ++Who)
		{
			R.bSeenNow[Who] = false;
			if (Who == Me || F.bDead) continue;
			const AOutlawsRemotePlayer* Op = C->PuppetOf(Who);
			if (!Living(Op)) continue;
			const FVector To = Op->ShownBody() - F.Eye;
			const double D = To.Size();
			if (D > SightRange || D < 50.0) continue;
			const FVector Lc = F.View.UnrotateVector(To);
			if (Lc.X <= 0.0) continue;
			if (FMath::Abs(FMath::RadiansToDegrees(FMath::Atan2(Lc.Y, Lc.X))) > ConeYaw || FMath::Abs(FMath::RadiansToDegrees(FMath::Atan2(Lc.Z, Lc.X))) > ConePitch) continue;
			if (!ClearLine(W, F.Eye, Op->ShownBody(), P, Op)) continue;
			R.bSeenNow[Who] = true;
			if (T - R.LastSeen[Who] >= SeeGap) Line(FString::Printf(TEXT("{\"k\":\"see\",\"t\":%.2f,\"who\":%d,\"d\":%.0f}"), Rt, Who, D));
			R.LastSeen[Who] = T;
			if (D < BestSeen) { BestSeen = D; BestWho = Who; }
		}
		if (BestWho >= 0 && (R.Engaged < 0 || !R.bSeenNow[R.Engaged])) R.Engaged = BestWho;
		if (R.Engaged >= 0 && R.bSeenNow[R.Engaged]) R.EngagedSeenAt = T;
		if (R.Engaged >= 0 && (T - R.EngagedSeenAt > EngageHold || !Living(C->PuppetOf(R.Engaged)))) R.Engaged = -1;
		FString M = FString::Printf(TEXT("{\"k\":\"mv\",\"t\":%.2f,\"f\":%d,\"s\":%d,\"g\":%d,\"hp\":%.1f,\"w\":%d"), Rt, FMath::RoundToInt(R.Fw), FMath::RoundToInt(R.Sd), F.bOnGround ? 1 : 0, F.Health, F.Weapon);
		if (R.Engaged >= 0)
		{
			const AOutlawsRemotePlayer* Op = C->PuppetOf(R.Engaged);
			M += FString::Printf(TEXT(",\"od\":%.0f,\"ov\":%d"), Op ? FVector::Dist(Op->ShownBody(), F.Eye) : 0.0, R.bSeenNow[R.Engaged] ? 1 : 0);
		}
		M += TEXT("}");
		Line(M);
	}
	if (T >= R.NextFlush)
	{
		R.NextFlush = T + FlushEvery;
		WriteHeld();
	}
}

void OutlawsPlayRecord::NoteShot(AOutlawsLevelActor* L, int32 Weapon, const TCHAR* Label, int32 Pellets, bool bScoped, const FVector& From, const FVector& Dir)
{
	FRec& R = Rec();
	if (R.Path.IsEmpty() || !L || R.Level.Get() != L) return;
	WritePendingShot();
	UOutlawsCoop* C = UOutlawsCoop::Get(L);
	UWorld* W = L->GetWorld();
	if (!C || !W) return;
	FShot S;
	S.T = Now(L) - R.T0;
	S.W = Weapon;
	S.Label = SafeLabel(Label);
	S.Pel = Pellets;
	S.bScoped = bScoped;
	S.Fw = R.Fw;
	S.Sd = R.Sd;
	// the target: the living opponent nearest the aim line within 20 degrees
	const FVector D = Dir.GetSafeNormal();
	double BestA = AimCone;
	for (int32 Who = 0; Who < MaxWho; ++Who)
	{
		if (Who == C->MyIndex()) continue;
		const AOutlawsRemotePlayer* Op = C->PuppetOf(Who);
		if (!Living(Op)) continue;
		const FVector To = Op->ShownBody() - From;
		const double Dist = To.Size();
		if (Dist < 1.0 || Dist > 2.0 * SightRange) continue;
		const double A = FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp(FVector::DotProduct(To / Dist, D), -1.0, 1.0)));
		if (A < BestA) { BestA = A; S.Tgt = Who; S.Td = Dist; S.Ta = A; }
	}
	if (S.Tgt >= 0)
	{
		const AOutlawsRemotePlayer* Op = C->PuppetOf(S.Tgt);
		S.bLos = Op && ClearLine(W, From, Op->ShownBody(), R.Player.Get(), Op);
	}
	R.Shot = S;
	R.bPending = true;
}

void OutlawsPlayRecord::NoteHit(int32 Who, double DistCm, bool bHead)
{
	FRec& R = Rec();
	if (!R.bPending) return;
	FShot& S = R.Shot;
	++S.Hits;
	if (bHead) ++S.Heads;
	if (S.HWho < 0) { S.HWho = Who; S.Hd = DistCm; }
}
