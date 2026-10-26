// Co-op voice chat (user 2026-10-02: "in a multiplayer section in the options ... they can enable microphone, which is
// normal volume in lobbies, but spatially calculated ingame"; tools/olue/COOP_DESIGN.md, "Voice chat").
//
// The path: the engine's Voice module, used directly.  Its microphone capture (DirectSound on the PC, OpenSL ES on the
// Quest) and its Opus encoder make a few 20 ms frames at a time; they go to the other machine as an unreliable call on
// this player's co-op link (ServerVoice / ClientVoice, so they ride the session's one connection and nothing else), where
// the engine's Opus decoder turns them back into 16 kHz PCM for a procedural sound wave played by an audio component:
// with no attenuation in the lobby (normal volume, 2D), attached to the other player's puppet at head height in a level
// (distance attenuation and panning).  The online subsystem's own VoIP is not used (it needs net ids, a voice handshake
// through player states and its own voice channel; the session's replication sweep and the Null subsystem make that the
// fragile path, and it cannot be fed a test tone).
//
// The setting: MpMicrophone in Saved/OutlawsSettings.ini (the Multiplayer page writes it); read once a second in a session
// (only when the file changed), or at once through NoteMicSetting.  Off: no capture object exists, nothing is sent.
// On: open mic while a session (or its lobby) is on, sent only while the voice gate is open (a level gate over 20 ms
// frames with a short hold; the PC's capture also has the engine's own noise gate).  On the Quest the RECORD_AUDIO
// permission is asked for the first time it is wanted; refused: logged, the microphone stays off, MicState says NotAllowed.

#include "OutlawsCoop.h"
#include "OutlawsRuntime.h"
#include "OutlawsLevelActor.h"
#include "OutlawsPlayer.h"
#include "OutlawsRemotePlayer.h"
#include "OutlawsCoopVoicePipe.h"
#include "VoiceModule.h"
#include "Interfaces/VoiceCapture.h"
#include "Interfaces/VoiceCodec.h"
#include "Components/AudioComponent.h"
#include "Sound/SoundWaveProcedural.h"
#include "Sound/SoundAttenuation.h"
#include "Camera/CameraComponent.h"
#include "Engine/World.h"
#include "Engine/NetDriver.h"
#include "Engine/NetConnection.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Paths.h"
#include "UObject/StrongObjectPtr.h"
#if PLATFORM_ANDROID
#include "AndroidPermissionFunctionLibrary.h"
#include "AndroidPermissionCallbackProxy.h"
#endif

namespace OLVoice
{
	constexpr int32 Rate = 16000;                        // the engine's voice rate (DEFAULT_VOICE_SAMPLE_RATE), mono
	constexpr int32 FrameBytes = Rate / 50 * 2;          // one Opus frame: 20 ms of 16-bit samples (640 bytes)
	constexpr int32 PacketFrames = 3;                    // frames per packet: 60 ms (about 17 packets a second while talking)
	constexpr int32 Bitrate = 20000;                     // Opus bits a second (about 2.5 KB/s while talking, nothing while silent)
	constexpr double GateHold = 0.5;                     // the gate stays open this long after the last loud frame
	constexpr double TalkingFor = 0.3;                   // "talking" while voice came within this long (the nameplate's icon)
	// (the play queue's bounds and the spurt's preroll: FTalkerRx::Receive, OutlawsCoopVoicePipe.cpp)
	// in a level: full volume up to InnerCm, then the engine's "natural sound" fall (linear in dB) to AtMaxDb at
	// InnerCm + FalloffCm, held there beyond (never silent in the same level: a partner far away can still be heard calling)
	constexpr float InnerCm = 300.f;
	constexpr float FalloffCm = 2700.f;
	constexpr float AtMaxDb = -20.f;
	const TCHAR* const Permission = TEXT("android.permission.RECORD_AUDIO");
	static_assert(Rate == OLVoicePipe::Rate, "the capture (after the reader), Opus and the play queue run at one rate");
	// (REQ-VOICEVOL: the partner volume through a soft limiter, OLVoicePipe::GainSample; BUG-MICSTATIC: the reader and the
	// talker's receive path, OutlawsCoopVoicePipe.h)
}

static TAutoConsoleVariable<float> CVarOLVoiceGate(TEXT("ol.VoiceGate"), 0.01f,
	TEXT("Co-op voice: the microphone level (RMS of a 20 ms frame, 0..1) that opens the voice gate (user 2026-10-02)."));

/** Where this player's ears are and how loud the talker is there (the voice test's report). */
static FString OLVoiceHeard(const UAudioComponent* AC, const AOutlawsPlayer* P)
{
	if (!AC || !P) return TEXT("no talker");
	const UCameraComponent* Cam = P->Camera.Get();
	const FVector Ear = Cam ? Cam->GetComponentLocation() : P->GetActorLocation();
	const FVector At = AC->GetComponentLocation();
	if (!AC->bAllowSpatialization) return FString::Printf(TEXT("2D: component x%.2f wherever the ears are (the partner volume is in the samples, never the master volume)"), AC->VolumeMultiplier);
	return FString::Printf(TEXT("this player's ears %.0f cm away: attenuation x%.2f, component x%.2f (the partner volume is in the samples, never the master volume)"), FVector::Dist(Ear, At),
		AC->AttenuationOverrides.Evaluate(FTransform(At), Ear), AC->VolumeMultiplier);
}

/** Everything voice chat keeps (UOutlawsCoop::Voice): the setting, the permission, the capture and its gate, the other
 *  player's decoder and talker, and the counts for the summary. */
struct FOutlawsCoopVoice
{
	// R8: the Multiplayer page's meter
	float Meter = 0.f;
	bool bMeterSending = false;
	bool bMeterOpen = false;                 // the microphone open for the meter alone (outside a session)
	// the setting
	bool bSetting = false;                   // MpMicrophone as last read (or as the menu said)
	bool bSettingKnown = false;
	FDateTime SettingStamp;
	double NextSettingAt = 0.0;
	bool bWanted = false;                    // the microphone wanted this tick (the setting, in a session)
	// the permission (the Quest)
	enum class EPerm : uint8 { Unknown, Asking, Granted, Denied };
	EPerm Perm = EPerm::Unknown;
	FDelegateHandle PermHandle;
	// the microphone and the encoder
	TSharedPtr<IVoiceCapture> Capture;
	TSharedPtr<IVoiceEncoder> Encoder;
	OLVoicePipe::FMicReader Reader;          // (BUG-MICSTATIC: whole samples only, at 16 kHz, whatever the capture does)
	bool bNoDevice = false;
	bool bOpen = false;                      // capturing (or the test tone running)
	TArray<uint8> Pcm;                       // captured, not yet looked at
	TArray<uint8> Out;                       // frames past the gate, not yet encoded
	TArray<uint8> Prev;                      // the frame before the gate opened (its first syllable)
	TArray<uint8> Enc;
	bool bGateOpen = false;
	double GateUntil = 0.0;
	double LastPumpAt = 0.0;
	int32 Stale = 0;                         // long frames whose sound was dropped
	double ToneAt = 0.0;
	int64 ToneSample = 0;
	float LevelPeak = 0.f;                   // the loudest frame this second (logged: the gate's tuning)
	// the other players' voices here (N1: a talker each, by player index; this machine's own slot stays unused)
	struct FTalker
	{
		TSharedPtr<IVoiceDecoder> Decoder;
		OLVoicePipe::FTalkerRx Rx;           // (BUG-MICSTATIC: in order, gaps reset, the queue bounded, static muted)
		TStrongObjectPtr<USoundWaveProcedural> Wave;
		TWeakObjectPtr<UAudioComponent> Comp;
		bool bHeardAny = false;
		bool bSpatial = false;
		bool bFade = false;                  // (REQ-VOICEVOL: made with the fall with distance on)
		float Gain = 1.f;
		bool bTalking = false;
		double LastHeardAt = 0.0;
		int32 Received = 0;
		FString LastTalker;
	};
	FTalker Talkers[UOutlawsCoop::MaxPlayers];
	int32 Relayed = 0;                       // host: packets passed on from one client to the others (N1)
	// (the partner's talker: the two-player summary and test read these)
	FTalker& Partner(const UOutlawsCoop& C) { return Talkers[FMath::Clamp(C.PartnerIndex(), 0, UOutlawsCoop::MaxPlayers - 1)]; }
	bool AnyTalking() const
	{
		for (const FTalker& T : Talkers) if (T.bTalking) return true;
		return false;
	}
	// counts (this session)
	int32 Sent = 0, Received = 0, BytesSent = 0, BytesReceived = 0, Decoded = 0, Queued = 0, NoAudio = 0;
	int32 GateOpens = 0, TalkOns = 0, TalkOffs = 0, TalkerChanges = 0, Spurts = 0, SentWhileOff = 0;
	int32 SecSent = 0, SecReceived = 0, SecBytesSent = 0, SecBytesReceived = 0, SecPoses = 0, SecSaturated = 0;
	int32 PeakSent = 0, PeakReceived = 0, SecondsWithVoice = 0, SaturatedTicks = 0;
	int32 PosesAtSecond = 0;
	double SecondAt = 0.0;
	FString LastTalker;                      // how another player's voice played last ("2D (lobby)", "spatial ...")
	// the test (-OLCoopTest=voice)
	int32 TestMic = 0;                       // the test's own switch over the setting (0 off, 1 on)
	double TestConnectedAt = 0.0;
	int32 TestReceivedAtMark = 0;
	bool bTestLobbyLogged = false;
};

/** The engine shut the capture down itself: on the Quest an app pause (the headset off, the Oculus menu) unloads the Voice
 *  module, whose FreeAllVoiceCaptureObjects calls Shutdown on every capture (VoiceModuleAndroid.cpp).  Its state is then
 *  UnInitialized; a capture this file started is never in that state otherwise. */
static bool OLVoiceCaptureReleased(const TSharedPtr<IVoiceCapture>& Capture)
{
	uint32 Avail = 0;
	return Capture.IsValid() && Capture->GetCaptureState(Avail) == EVoiceCaptureState::UnInitialized;
}

/** The capture closed (the setting off, the page gone, the session's end): what its reader saw, when it read anything. */
static void OLVoiceCloseCapture(FOutlawsCoopVoice& V, const TCHAR* Why)
{
	if (V.Capture.IsValid())
	{
		if (OLVoiceCaptureReleased(V.Capture))
		{
			// (dotard 2026-10-10: the Quest's capture, once shut down by the engine, has freed its ring but keeps the pointer,
			// and its destructor shuts it down again: a double free, the game gone at the session's end after a headset-off.
			// So a released capture is kept, never destroyed: a few hundred bytes for each pause.)
			static TArray<TSharedPtr<IVoiceCapture>>* const Released = new TArray<TSharedPtr<IVoiceCapture>>();
			Released->Add(V.Capture);
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP voice: the microphone's capture was released by the system (an app pause); kept, not destroyed (%d so far)"), Released->Num());
		}
		else
		{
			V.Capture->Stop();
		}
		if (V.Reader.Ticks > 0) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP voice: the microphone's capture closed (%s): %s"), Why, *V.Reader.Summary());
	}
	V.Capture.Reset();
	V.Reader = OLVoicePipe::FMicReader();
}

bool UOutlawsCoop::MicSettingOn() const
{
	return Voice.IsValid() && Voice->bSettingKnown && Voice->bSetting;     // (REQ-MPLOBBY: the lobby's mute rows, for players with a microphone)
}

FOutlawsCoopVoice& UOutlawsCoop::VoiceState()
{
	if (!Voice.IsValid()) Voice = MakeShared<FOutlawsCoopVoice>();
	return *Voice;
}

int32 UOutlawsCoop::VoiceReceivedFrom(int32 Who) const
{
	return Voice.IsValid() && Who >= 0 && Who < MaxPlayers ? Voice->Talkers[Who].Received : 0;
}

int32 UOutlawsCoop::VoiceRelayed() const
{
	return Voice.IsValid() ? Voice->Relayed : 0;
}

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::SetVoiceTestMic(int32 On)
{
	VoiceState().TestMic = On;
}
#endif // WITH_OUTLAWS_TESTS

// ---- the links ----------------------------------------------------------------------------------------------------------

void AOutlawsCoopLink::ServerVoice_Implementation(const TArray<uint8>& Data)
{
	if (!Admit(EOLGate::Voice, Data.Num() >= 4 && Data.Num() <= 1500)) return;
	UOutlawsCoop* C = UOutlawsCoop::Get(this);
	if (!C) return;
	C->OnVoice(PlayerIndex, Data);
	// (N1: passed on to the other clients, who each play it from this player's puppet)
	const int32 From = PlayerIndex;
	int32 Passed = 0;
	C->ForClients([&](AOutlawsCoopLink& K) { K.ClientVoice(From, Data); ++Passed; }, From);
	if (Passed > 0) C->VoiceState().Relayed += Passed;
}

void AOutlawsCoopLink::ClientVoice_Implementation(int32 From, const TArray<uint8>& Data)
{
	if (UOutlawsCoop* C = UOutlawsCoop::Get(this)) C->OnVoice(From, Data);
}

// ---- what the menu and the nameplate ask ------------------------------------------------------------------------------------

bool UOutlawsCoop::IsRemoteTalking(const UObject* Context, int32 PlayerIndex)
{
	const UOutlawsCoop* C = Get(Context);
	return C && C->Mode != EOutlawsCoopMode::None && C->Voice.IsValid() && PlayerIndex >= 0 && PlayerIndex < MaxPlayers
		&& PlayerIndex != C->MyIndex() && C->Voice->Talkers[PlayerIndex].bTalking;
}

EOutlawsCoopMic UOutlawsCoop::MicState(const UObject* Context)
{
	UOutlawsCoop* C = Get(Context);
	if (!C) return EOutlawsCoopMic::Off;
	FOutlawsCoopVoice& V = C->VoiceState();
	if (!V.bSettingKnown)
	{
		const TMap<FString, FString> S = AOutlawsLevelActor::ReadSettings();
		const FString* M = S.Find(TEXT("MpMicrophone"));
		V.bSetting = M && M->ToBool();
		V.bSettingKnown = true;
	}
	if (!V.bSetting) return EOutlawsCoopMic::Off;
	if (V.Perm == FOutlawsCoopVoice::EPerm::Asking) return EOutlawsCoopMic::Asking;
	if (V.Perm == FOutlawsCoopVoice::EPerm::Denied) return EOutlawsCoopMic::NotAllowed;
	if (V.bNoDevice) return EOutlawsCoopMic::NoDevice;
	return EOutlawsCoopMic::On;
}

const TCHAR* UOutlawsCoop::MicStateText(EOutlawsCoopMic State)
{
	switch (State)
	{
	case EOutlawsCoopMic::On: return TEXT("on");
	case EOutlawsCoopMic::Asking: return TEXT("asking for the microphone");
	case EOutlawsCoopMic::NotAllowed: return TEXT("microphone not allowed");
	case EOutlawsCoopMic::NoDevice: return TEXT("no microphone found");
	default: return TEXT("off");
	}
}

void UOutlawsCoop::NoteMicSetting(const UObject* Context, bool bOn)
{
	UOutlawsCoop* C = Get(Context);
	if (!C) return;
	FOutlawsCoopVoice& V = C->VoiceState();
	if (bOn && !V.bSetting && V.Perm == FOutlawsCoopVoice::EPerm::Denied) V.Perm = FOutlawsCoopVoice::EPerm::Unknown;   // (turned on again: ask again)
	if (bOn != V.bSetting || !V.bSettingKnown)
	{
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP voice: the microphone setting is %s (the menu)"), bOn ? TEXT("on") : TEXT("off"));
	}
	V.bSetting = bOn;
	V.bSettingKnown = true;
	V.bNoDevice = false;                     // (tried again the next time it is wanted)
	V.SettingStamp = IFileManager::Get().GetTimeStamp(*(FPaths::ProjectSavedDir() / TEXT("OutlawsSettings.ini")));
	if (bOn) C->MicPermitted();              // (the Quest asks now, while the player is at the menu)
}

// ---- R8: sensitivity, push to talk, the meter ------------------------------------------------------------------------

void UOutlawsCoop::ApplyMicSensitivity(int32 Sensitivity)
{
	// low takes a louder voice to open, high a quieter one (0.01 the voice chat's first value)
	static const float Gates[] = { 0.02f, 0.01f, 0.005f };
	CVarOLVoiceGate->Set(Gates[FMath::Clamp(Sensitivity, 0, 2)], ECVF_SetByCode);
}

bool UOutlawsCoop::TalkKeyHeld() const
{
	const AOutlawsLevelActor* L = FindLevel();
	const AOutlawsPlayer* P = LocalPlayer();
	if (!L || L->MpPushToTalk <= 0) return true;
	if (!P) return false;
	const bool bLeft = P->CoopGripAxis(0) >= 0.5f, bRight = P->CoopGripAxis(1) >= 0.5f;
	return L->MpPushToTalk == 1 ? bLeft : L->MpPushToTalk == 2 ? bRight : (bLeft || bRight);
}

float UOutlawsCoop::MicMeterLevel(const UObject* Context)
{
	UOutlawsCoop* C = Get(Context);
	return C ? C->VoiceState().Meter : 0.f;
}

bool UOutlawsCoop::MicMeterSending(const UObject* Context)
{
	UOutlawsCoop* C = Get(Context);
	return C && C->VoiceState().bMeterSending;
}

bool UOutlawsCoop::MicMeterOpen(const UObject* Context)
{
	UOutlawsCoop* C = Get(Context);
	return C && C->VoiceState().bMeterOpen;
}

void UOutlawsCoop::TickMicMeter(double Now)
{
	// (outside a session) the Voice Chat page (REQ-VOICEPAGE) open with the microphone on: opened for the meter, nothing sent
	// (no link); closed again when the page goes
	FOutlawsCoopVoice& V = VoiceState();
	const AOutlawsPlayer* P = LocalPlayer();
	const AOutlawsLevelActor* L = FindLevel();
	const bool bWant = !IsTestRun() && P && L && L->bMpMicrophone && P->IsMenuOpen() && P->CoopMenuPage() == TEXT("#voice") && MicPermitted();
	if (bWant)
	{
		V.bWanted = false;                   // (SendVoice drops: nobody to send to anyway)
		V.bMeterOpen = true;
		PumpMic(Now);
		return;
	}
	if (V.bMeterOpen)
	{
		V.bMeterOpen = false;
		OLVoiceCloseCapture(V, TEXT("the Voice Chat page's meter"));
		V.Pcm.Reset();
		V.Out.Reset();
		V.Prev.Reset();
		V.bGateOpen = false;
		V.bOpen = false;
		V.Meter = 0.f;
		V.bMeterSending = false;
	}
}

// ---- the tick -----------------------------------------------------------------------------------------------------------

bool UOutlawsCoop::MicWanted(double Now)
{
	FOutlawsCoopVoice& V = VoiceState();
	if (bAuto && (TestOnly == TEXT("voice") || TestOnly == TEXT("party"))) return V.TestMic == 1;    // (the tests: their tone, their own switch; N1's party test too)
	if (IsTestRun()) return false;                                     // (other automated runs never open this PC's microphone)
	if (Now >= V.NextSettingAt)
	{
		V.NextSettingAt = Now + 1.0;
		const FString Path = FPaths::ProjectSavedDir() / TEXT("OutlawsSettings.ini");
		const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Path);
		if (!V.bSettingKnown || Stamp != V.SettingStamp)
		{
			V.SettingStamp = Stamp;
			const TMap<FString, FString> S = AOutlawsLevelActor::ReadSettings();
			const FString* M = S.Find(TEXT("MpMicrophone"));
			const bool bOn = M && M->ToBool();
			if (bOn != V.bSetting || !V.bSettingKnown)
			{
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP voice: the microphone setting is %s (MpMicrophone=%s in OutlawsSettings.ini)"),
					bOn ? TEXT("on") : TEXT("off"), M ? **M : TEXT("(none)"));
				if (bOn && !V.bSetting && V.Perm == FOutlawsCoopVoice::EPerm::Denied) V.Perm = FOutlawsCoopVoice::EPerm::Unknown;
				if (bOn && !V.bSetting) V.bNoDevice = false;
			}
			V.bSetting = bOn;
			V.bSettingKnown = true;
		}
	}
	return V.bSetting;
}

bool UOutlawsCoop::MicPermitted()
{
#if PLATFORM_ANDROID
	FOutlawsCoopVoice& V = VoiceState();
	if (V.Perm == FOutlawsCoopVoice::EPerm::Granted) return true;
	if (V.Perm != FOutlawsCoopVoice::EPerm::Unknown) return false;          // (being asked, or refused: off)
	if (UAndroidPermissionFunctionLibrary::CheckPermission(OLVoice::Permission))
	{
		V.Perm = FOutlawsCoopVoice::EPerm::Granted;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP voice: the microphone permission is granted"));
		return true;
	}
	TArray<FString> Ask;
	Ask.Add(OLVoice::Permission);
	UAndroidPermissionCallbackProxy* Proxy = UAndroidPermissionFunctionLibrary::AcquirePermissions(Ask);
	if (!Proxy)
	{
		V.Perm = FOutlawsCoopVoice::EPerm::Denied;
		UE_LOG(LogOutlawsCoop, Warning, TEXT("OLCOOP voice: the microphone permission could not be asked for: the microphone stays off"));
		return false;
	}
	V.Perm = FOutlawsCoopVoice::EPerm::Asking;
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP voice: asking the player for the microphone permission"));
	TWeakObjectPtr<UOutlawsCoop> Self(this);
	V.PermHandle = Proxy->OnPermissionsGrantedDelegate.AddLambda([Self](const TArray<FString>& Names, const TArray<bool>& Granted)
	{
		UOutlawsCoop* C = Self.Get();
		if (!C) return;
		FOutlawsCoopVoice& V = C->VoiceState();
		const int32 i = Names.IndexOfByKey(FString(OLVoice::Permission));
		if (i == INDEX_NONE || V.Perm != FOutlawsCoopVoice::EPerm::Asking) return;
		const bool bYes = Granted.IsValidIndex(i) && Granted[i];
		V.Perm = bYes ? FOutlawsCoopVoice::EPerm::Granted : FOutlawsCoopVoice::EPerm::Denied;
		if (bYes)
		{
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP voice: the player allowed the microphone"));
		}
		else
		{
			UE_LOG(LogOutlawsCoop, Warning, TEXT("OLCOOP voice: the player refused the microphone permission: the microphone stays off (\"microphone not allowed\")"));
		}
		if (UAndroidPermissionCallbackProxy* P = UAndroidPermissionCallbackProxy::GetInstance())
		{
			P->OnPermissionsGrantedDelegate.Remove(V.PermHandle);
		}
		V.PermHandle.Reset();
	});
	return false;
#else
	return true;
#endif
}

void UOutlawsCoop::ResetVoice()
{
	if (!Voice.IsValid()) return;
	FOutlawsCoopVoice& V = *Voice;
	if (V.Sent > 0 || V.Received > 0) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP voice at the session's end: %s"), *VoiceSummary());
	OLVoiceCloseCapture(V, TEXT("the session's end"));
	V.Encoder.Reset();
	for (FOutlawsCoopVoice::FTalker& T : V.Talkers)
	{
		T.Decoder.Reset();
		if (UAudioComponent* AC = T.Comp.Get())
		{
			AC->Stop();
			AC->DestroyComponent();
		}
		T.Comp.Reset();
		T.Wave.Reset();
	}
	// (the setting, the permission and the test's switch stay: they are this machine's, not the session's)
	const bool bSetting = V.bSetting, bKnown = V.bSettingKnown;
	const FDateTime Stamp = V.SettingStamp;
	const FOutlawsCoopVoice::EPerm Perm = V.Perm;
	const FDelegateHandle Handle = V.PermHandle;
	const int32 TestMic = V.TestMic;
	V = FOutlawsCoopVoice();
	V.bSetting = bSetting;
	V.bSettingKnown = bKnown;
	V.SettingStamp = Stamp;
	V.Perm = Perm;
	V.PermHandle = Handle;
	V.TestMic = TestMic;
}

void UOutlawsCoop::TickVoice(double Now)
{
	FOutlawsCoopVoice& V = VoiceState();
	const bool bVoiceTest = bAuto && TestOnly == TEXT("voice");
	const AOutlawsCoopSession* S = GetSession();
	if (bVoiceTest && S && S->bClientConnected && V.TestConnectedAt == 0.0)
	{
		V.TestConnectedAt = Now;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP voice test: connected, the microphone setting off for 2.5 s (the lobby)"));
	}
	if (bVoiceTest && V.TestMic == 0 && V.TestConnectedAt > 0.0 && Now - V.TestConnectedAt > 2.5 && !V.bTestLobbyLogged)
	{
		V.bTestLobbyLogged = true;
		V.TestMic = 1;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP voice test: the microphone setting off -> on in the lobby (packets sent while off: %d, received while off: %d)"),
			V.Sent, V.Received);
	}
	bool bWant = MicWanted(Now);
	if (bWant && !bVoiceTest && !MicPermitted()) bWant = false;
	V.bWanted = bWant;
	if (bWant) PumpMic(Now);
	else if (V.bOpen)
	{
		OLVoiceCloseCapture(V, TEXT("not wanted"));
		V.Pcm.Reset();
		V.Out.Reset();
		V.Prev.Reset();
		V.bGateOpen = false;
		V.bOpen = false;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP voice: the microphone closed (%d packets sent so far)"), V.Sent);
	}
	TickVoiceTalker(Now);

	// the second's counts (a line a second while voice flows in the tests; every ten seconds of voice otherwise)
	if (V.SecondAt == 0.0)
	{
		V.SecondAt = Now + 1.0;
		V.PosesAtSecond = PosesReceived;
	}
	if (UWorld* W = CurrentWorld())
	{
		if (UNetDriver* D = W->GetNetDriver())
		{
			// (N1: the host counts a tick once if any client's connection is full)
			bool bFull = false;
			if (V.bOpen || V.AnyTalking())
			{
				if (IsHost())
				{
					for (UNetConnection* Conn : D->ClientConnections) bFull = bFull || (Conn && Conn->IsNetReady(false) == 0);
				}
				else if (UNetConnection* Conn = D->ServerConnection.Get())
				{
					bFull = Conn->IsNetReady(false) == 0;
				}
			}
			if (bFull)
			{
				++V.SecSaturated;
				++V.SaturatedTicks;
			}
		}
	}
	if (Now >= V.SecondAt)
	{
		V.SecPoses = PosesReceived - V.PosesAtSecond;
		const bool bAny = V.SecSent > 0 || V.SecReceived > 0;
		if (bAny)
		{
			++V.SecondsWithVoice;
			V.PeakSent = FMath::Max(V.PeakSent, V.SecSent);
			V.PeakReceived = FMath::Max(V.PeakReceived, V.SecReceived);
		}
		if (bAny && (IsTestRun() || V.SecondsWithVoice % 10 == 1))
		{
			int32 OutBps = -1, InBps = -1;
			if (UWorld* W = CurrentWorld())
			{
				if (UNetDriver* D = W->GetNetDriver())
				{
					const UNetConnection* Conn = IsHost() ? (D->ClientConnections.Num() > 0 ? D->ClientConnections[0].Get() : nullptr) : D->ServerConnection.Get();
					if (Conn)
					{
						OutBps = Conn->OutBytesPerSecond;
						InBps = Conn->InBytesPerSecond;
					}
				}
			}
			// (BUG-MICSTATIC: each talker's receive counts, and this machine's microphone reader, so a log shows static and its cause)
			FString Detail;
			for (int32 Who = 0; Who < MaxPlayers; ++Who)
			{
				const OLVoicePipe::FTalkerRx& R = V.Talkers[Who].Rx;
				if (R.Packets == 0) continue;
				Detail += FString::Printf(TEXT("; from the %s: late %d, gaps %d (%d lost), decoder resets %d, queue cuts %d, emptied %d, ran dry %d, deepest %.2f s, noise frames muted %d"),
					WhoWord(Who), R.Late, R.Gaps, R.Lost, R.DecoderResets, R.Trims, R.HardResets, R.Underruns, double(R.MaxQueued) / OLVoice::Rate, R.MutedFrames);
			}
			if (V.Reader.Ticks > 0)
			{
				Detail += FString::Printf(TEXT("; microphone: delivers %.0f a second (played as %d), byte slips %d, full-buffer ticks %d, ticks dropped %d, noise-like frames %d"),
					V.Reader.MeasuredRate, V.Reader.DeviceRate, V.Reader.Slips, V.Reader.FullRings, V.Reader.DroppedTicks, V.Reader.NoiseFrames);
			}
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP voice per second (%s): sent %d packets (%d bytes), received %d packets (%d bytes), poses received %d, connection out %d B/s in %d B/s, saturated ticks %d, loudest frame %.3f%s"),
				IsHost() ? TEXT("host") : TEXT("client"), V.SecSent, V.SecBytesSent, V.SecReceived, V.SecBytesReceived, V.SecPoses, OutBps, InBps, V.SecSaturated, V.LevelPeak, *Detail);
		}
		V.SecSent = V.SecReceived = V.SecBytesSent = V.SecBytesReceived = V.SecSaturated = 0;
		V.LevelPeak = 0.f;
		V.PosesAtSecond = PosesReceived;
		V.SecondAt = Now + 1.0;
	}
}

void UOutlawsCoop::PumpMic(double Now)
{
	using namespace OLVoice;
	FOutlawsCoopVoice& V = VoiceState();
	const bool bTone = bAuto && (TestOnly == TEXT("voice") || TestOnly == TEXT("party"));
	if (!V.bOpen)
	{
		if (V.bNoDevice) return;
		if (!FVoiceModule::IsAvailable() && !FModuleManager::Get().LoadModule(TEXT("Voice")))
		{
			V.bNoDevice = true;
			UE_LOG(LogOutlawsCoop, Warning, TEXT("OLCOOP voice: the engine's Voice module is not there: no microphone"));
			return;
		}
		FVoiceModule& VM = FVoiceModule::Get();
		if (!VM.IsVoiceEnabled())
		{
			V.bNoDevice = true;
			UE_LOG(LogOutlawsCoop, Warning, TEXT("OLCOOP voice: the engine's Voice module is off ([Voice] bEnabled): no microphone"));
			return;
		}
		if (!bTone)
		{
			V.Capture = VM.CreateVoiceCapture(FString(), Rate, 1);
			if (!V.Capture.IsValid() || !V.Capture->Start())
			{
				V.Capture.Reset();
				V.bNoDevice = true;
				UE_LOG(LogOutlawsCoop, Warning, TEXT("OLCOOP voice: no microphone could be opened (\"no microphone found\")"));
				return;
			}
			// (BUG-MICSTATIC) the reader starts with the capture: a new byte count, the sample grid at byte 0, the rate as asked
			V.Reader.Open(Rate, V.Capture->GetBufferSize(), Now);
		}
		if (!V.Encoder.IsValid())
		{
			V.Encoder = VM.CreateVoiceEncoder(Rate, 1, EAudioEncodeHint::VoiceEncode_Voice);
			if (V.Encoder.IsValid()) V.Encoder->SetBitrate(Bitrate);
		}
		if (!V.Encoder.IsValid())
		{
			V.Capture.Reset();
			V.bNoDevice = true;
			UE_LOG(LogOutlawsCoop, Warning, TEXT("OLCOOP voice: no Opus encoder: no microphone"));
			return;
		}
		V.bOpen = true;
		V.ToneAt = Now;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP voice: the microphone open (%s, %d Hz mono, Opus %d bit/s, %d ms packets, gate %.3f; capture asked %d Hz, its buffer %d bytes, Opus and the play queue %d Hz)"),
			bTone ? TEXT("the test tone") : TEXT("capture"), Rate, Bitrate, PacketFrames * 20, CVarOLVoiceGate.GetValueOnGameThread(),
			V.Reader.Requested, V.Reader.RingBytes, OLVoicePipe::Rate);
	}

	// an app pause released the microphone (VoiceModuleAndroid.cpp): closed here and made again on the next tick, so the
	// voice comes back when the headset goes on again instead of staying silent until a rejoin
	if (!bTone && OLVoiceCaptureReleased(V.Capture))
	{
		OLVoiceCloseCapture(V, TEXT("released by the system, opened again"));
		V.Pcm.Reset();
		V.Out.Reset();
		V.Prev.Reset();
		V.bGateOpen = false;
		V.GateUntil = 0.0;
		V.bOpen = false;
		return;
	}

	// a long frame (a level being built, a hitch): what was said meanwhile is stale, and sent at once it would be a burst
	// that fills the connection; dropped, the gate starts afresh
	const bool bStale = V.LastPumpAt > 0.0 && Now - V.LastPumpAt > 0.3;
	V.LastPumpAt = Now;
	if (bStale)
	{
		if (bTone) V.ToneAt = Now - 0.02;
		V.Pcm.Reset();
		V.Out.Reset();
		V.Prev.Reset();
		V.bGateOpen = false;
		V.GateUntil = 0.0;
		++V.Stale;
	}
	// this tick's sound: the microphone's, or the test's tone (1.2 s of a voice-like buzz in syllables, 0.8 s of near silence)
	if (bTone)
	{
		const int32 N = FMath::Clamp(int32((Now - V.ToneAt) * Rate), 0, Rate / 2);
		V.ToneAt += double(N) / Rate;
		const int32 At = V.Pcm.Num();
		V.Pcm.AddUninitialized(N * 2);
		int16* Dst = reinterpret_cast<int16*>(V.Pcm.GetData() + At);
		for (int32 i = 0; i < N; ++i, ++V.ToneSample)
		{
			const double T = double(V.ToneSample) / Rate;
			const bool bOn = FMath::Fmod(T, 2.0) < 1.2;
			const double Syll = 0.6 + 0.4 * FMath::Sin(2.0 * PI * 4.0 * T);
			const double Buzz = (FMath::Sin(2.0 * PI * 180.0 * T) + 0.5 * FMath::Sin(2.0 * PI * 360.0 * T) + 0.25 * FMath::Sin(2.0 * PI * 540.0 * T)) / 1.75;
			const double S = bOn ? 0.25 * Syll * Buzz : 0.001 * FMath::Sin(2.0 * PI * 50.0 * T);
			Dst[i] = int16(FMath::Clamp(S, -1.0, 1.0) * 32000.0);
		}
	}
	else if (V.Capture.IsValid())
	{
		// (the Quest's OpenSL capture copies up to twice the size it is given, and over 2048 it empties its ring instead: asked
		// 1024, up to 2048 bytes a read.  The PC's gives all it holds or nothing (BufferTooSmall): asked what it holds.)
		// BUG-MICSTATIC: every read goes through the reader, which keeps whole samples on the grid and drops a tick that may
		// hold a cut-short ring write (OutlawsCoopVoicePipe.h), and resamples a device that ignores the asked rate
		TArray<uint8> Raw;
		Raw.SetNumUninitialized(4096);
		V.Reader.BeginTick();
		for (int32 Reads = 0; Reads < 32; ++Reads)
		{
			uint32 Ask = 1024;
#if !PLATFORM_ANDROID
			uint32 Avail = 0;
			V.Capture->GetCaptureState(Avail);
			Ask = FMath::Max<uint32>(Avail, 1024);
#endif
			if (Raw.Num() < int32(Ask) * 2) Raw.SetNumUninitialized(int32(Ask) * 2);
			uint32 Got = 0;
			const EVoiceCaptureState::Type St = V.Capture->GetVoiceData(Raw.GetData(), Ask, Got);
			if (St != EVoiceCaptureState::Ok || Got == 0) break;
			V.Reader.Add(Raw.GetData(), FMath::Min<int32>(int32(Got), Raw.Num()));
		}
		FString Note;
		V.Reader.EndTick(Now, bStale, V.Pcm, Note);
		if (!Note.IsEmpty()) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP voice: %s"), *Note);
	}
	if (V.Pcm.Num() > Rate * 2) V.Pcm.RemoveAt(0, (V.Pcm.Num() - Rate * 2 + 1) & ~1, false);     // (a long hitch: only the last second, whole samples)

	// the gate, a 20 ms frame at a time
	auto Flush = [this, &V]()
	{
		if (V.Out.Num() < FrameBytes || !V.Encoder.IsValid()) { V.Out.Reset(); return; }
		const int32 Whole = V.Out.Num() / FrameBytes * FrameBytes;
		V.Enc.SetNumUninitialized(4096);
		uint32 EncSize = uint32(V.Enc.Num());
		V.Encoder->Encode(V.Out.GetData(), uint32(Whole), V.Enc.GetData(), EncSize);
		V.Out.Reset();
		if (EncSize == 0) return;
		TArray<uint8> Packet(V.Enc.GetData(), int32(EncSize));
		SendVoice(Packet);
	};
	const float Gate = CVarOLVoiceGate.GetValueOnGameThread();
	// (R8) push to talk: sent only while the chosen grip is held, all of it (no gate); off: the gate as before
	const AOutlawsLevelActor* PL = FindLevel();
	const bool bPtt = PL && PL->MpPushToTalk > 0 && !bTone;
	const bool bKey = TalkKeyHeld();
	int32 Pos = 0;
	while (V.Pcm.Num() - Pos >= FrameBytes)
	{
		const int16* Smp = reinterpret_cast<const int16*>(V.Pcm.GetData() + Pos);
		double Sum = 0.0;
		for (int32 i = 0; i < FrameBytes / 2; ++i) Sum += double(Smp[i]) * double(Smp[i]);
		const float Level = float(FMath::Sqrt(Sum / (FrameBytes / 2)) / 32768.0);
		V.LevelPeak = FMath::Max(V.LevelPeak, Level);
		V.Meter = FMath::Max(Level, V.Meter * 0.9f);        // (R8: the Multiplayer page's meter, falling back over a few frames)
		if (Level > Gate) V.GateUntil = Now + GateHold;
		if (bPtt) V.GateUntil = bKey ? Now + 0.15 : 0.0;     // (a grip let go: the words in hand go, then nothing)
		const bool bOpenNow = Now < V.GateUntil;
		V.bMeterSending = bOpenNow;
		if (bOpenNow && !V.bGateOpen)
		{
			++V.GateOpens;
			V.Out.Append(V.Prev);                    // (the frame before: the start of the first syllable)
		}
		else if (!bOpenNow && V.bGateOpen)
		{
			Flush();
		}
		V.bGateOpen = bOpenNow;
		if (bOpenNow)
		{
			V.Out.Append(V.Pcm.GetData() + Pos, FrameBytes);
			if (V.Out.Num() >= PacketFrames * FrameBytes) Flush();
		}
		V.Prev.Reset();
		V.Prev.Append(V.Pcm.GetData() + Pos, FrameBytes);
		Pos += FrameBytes;
	}
	if (Pos > 0) V.Pcm.RemoveAt(0, Pos, false);
}

void UOutlawsCoop::SendVoice(const TArray<uint8>& Packet)
{
	FOutlawsCoopVoice& V = VoiceState();
	if (!V.bWanted)
	{
		++V.SentWhileOff;                        // (never: the gate is only run while wanted; counted to prove it)
		return;
	}
	if (IsHost())
	{
		// (N1: to every client)
		const AOutlawsCoopSession* S = GetSession();
		if (!S || !S->bClientConnected) return;
		int32 To = 0;
		ForClients([&](AOutlawsCoopLink& K) { K.ClientVoice(0, Packet); ++To; });
		if (To == 0) return;
	}
	else
	{
		AOutlawsCoopLink* Mine = GetLink(MyIndex());
		if (!Mine) return;
		Mine->ServerVoice(Packet);
	}
	++V.Sent;
	++V.SecSent;
	V.BytesSent += Packet.Num();
	V.SecBytesSent += Packet.Num();
}

// ---- the other player's voice here ------------------------------------------------------------------------------------

void UOutlawsCoop::OnVoice(int32 Who, const TArray<uint8>& Data)
{
	using namespace OLVoice;
	// (N1: any other player's voice; each speaker plays from its own talker)
	if (Mode == EOutlawsCoopMode::None || Who < 0 || Who >= MaxPlayers || Who == MyIndex() || Data.Num() < 4) return;
	if (IsVoiceMuted(Who)) return;       // (R8: the pause menu's Mute partner: not played, the speaker over the head not shown; N1: everyone; REQ-MPLOBBY: or one player muted in the lobby)
	FOutlawsCoopVoice& V = VoiceState();
	FOutlawsCoopVoice::FTalker& T = V.Talkers[Who];
	++V.Received;
	++V.SecReceived;
	++T.Received;
	V.BytesReceived += Data.Num();
	V.SecBytesReceived += Data.Num();
	if (!T.Decoder.IsValid())
	{
		if (!FVoiceModule::IsAvailable() && !FModuleManager::Get().LoadModule(TEXT("Voice"))) return;
		T.Decoder = FVoiceModule::Get().CreateVoiceDecoder(Rate, 1);
		if (!T.Decoder.IsValid())
		{
			if (T.Received == 1) UE_LOG(LogOutlawsCoop, Warning, TEXT("OLCOOP voice: no Opus decoder here: the %s player's voice cannot be played"), WhoWord(Who));
			return;
		}
	}
	// (BUG-MICSTATIC: the talker's receive path decodes and queues: in order by the packet's generation, the decoder reset on
	// a gap or a new stream, the queue kept between bounds, loud wideband noise muted; OutlawsCoopVoicePipe.h)
	struct FWaveSink : OLVoicePipe::IVoiceSink
	{
		USoundWaveProcedural* Wave = nullptr;
		virtual int32 QueuedSamples() const override { return Wave ? Wave->GetAvailableAudioByteCount() / 2 : 0; }
		virtual void Queue(const int16* S, int32 N) override { if (Wave && N > 0) Wave->QueueAudio(reinterpret_cast<const uint8*>(S), N * 2); }
		virtual void Clear() override { if (Wave) Wave->ResetAudio(); }
		virtual bool Plays() const override { return Wave != nullptr; }
	};
	const double Now = FPlatformTime::Seconds();
	T.bHeardAny = true;
	TickVoiceTalker(Now);                            // (the talker made or moved first, its wave ready)
	const bool bNewSpurt = !T.bTalking;
	FWaveSink Sink;
	const UWorld* W = CurrentWorld();
	const bool bDevice = W && W->GetAudioDeviceRaw();
	if (bDevice) Sink.Wave = T.Wave.Get();
	const int32 DecodedBefore = T.Rx.Decoded;
	FString Note;
	T.Rx.Receive(*T.Decoder, Data.GetData(), Data.Num(), Now, bNewSpurt, T.Gain, Sink, Note);
	if (!Note.IsEmpty()) UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP voice: the %s player's voice: %s"), WhoWord(Who), *Note);
	if (T.Rx.Decoded == DecodedBefore) return;
	++V.Decoded;
	T.LastHeardAt = Now;
	if (!T.bTalking)
	{
		T.bTalking = true;
		++V.TalkOns;
		if (bNewSpurt) ++V.Spurts;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP voice: the other player (%s) is talking (packet %d, %d bytes -> %d bytes of sound)"),
			WhoWord(Who), T.Received, Data.Num(), T.Rx.Block.Num() * 2);
	}
	if (!bDevice || !Sink.Wave)
	{
		++V.NoAudio;                                 // (no audio device here, -nosound: decoded, nothing to play it on)
		return;
	}
	++V.Queued;
	UAudioComponent* AC = T.Comp.Get();
	if (AC && !AC->IsPlaying()) AC->Play();
}

void UOutlawsCoop::TickVoiceTalker(double Now)
{
	using namespace OLVoice;
	FOutlawsCoopVoice& V = VoiceState();
	AOutlawsLevelActor* L = FindLevel();
	for (int32 Who = 0; Who < MaxPlayers; ++Who)
	{
		FOutlawsCoopVoice::FTalker& T = V.Talkers[Who];
		if (T.bTalking && Now - T.LastHeardAt > TalkingFor)
		{
			T.bTalking = false;
			++V.TalkOffs;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP voice: the other player (%s) stopped talking (%d packets received)"), WhoWord(Who), T.Received);
		}
		if (!T.bHeardAny || !L) continue;
		// in a level with that player drawn: from their puppet's head; otherwise (the lobby, their level not built yet) 2D
		AOutlawsRemotePlayer* R = PuppetOf(Who);
		const bool bSpatial = R && bLevelBuilt && !L->IsFrontEnd() && !R->ShownHead().IsNearlyZero();     // (placed: its first pose eased in)
		AActor* Owner = bSpatial ? static_cast<AActor*>(R) : static_cast<AActor*>(L);
		UAudioComponent* AC = T.Comp.Get();
		// REQ-VOICEVOL (user 2026-10-05: voices quiet on both sides; independent of the master volume): the Multiplayer
		// page's partner volume (a boost, 200% by default) on the voice's own level, never times the master volume. Applied to
		// the decoded samples through a soft limiter (VoiceGainSample) so a boost can never clip or wrap; the component stays
		// at x1. The fall with distance only when "Voice fades with distance" is on (default off: as loud far off as near)
		const bool bFade = L->bMpVoiceFade;
		const float Gain = FMath::Clamp(L->MpPartnerVolume, 50, 300) / 100.f;
		if (T.Comp.IsValid() && FMath::Abs(T.Gain - Gain) > 0.01f)
		{
			T.Gain = Gain;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP voice: the %s player's voice volume now x%.2f (partner volume %d%%, independent of the master volume x%.2f)"), WhoWord(Who), Gain, L->MpPartnerVolume, L->MasterVolume);
		}
		if (!AC || AC->GetOwner() != Owner || T.bSpatial != bSpatial || (bSpatial && T.bFade != bFade))
		{
			if (AC)
			{
				AC->Stop();
				AC->DestroyComponent();
			}
			USoundWaveProcedural* Wave = NewObject<USoundWaveProcedural>(GetTransientPackage());
			Wave->SetSampleRate(Rate);
			Wave->NumChannels = 1;
			Wave->Duration = INDEFINITELY_LOOPING_DURATION;
			Wave->SoundGroup = SOUNDGROUP_Voice;
			Wave->bLooping = false;
			T.Wave.Reset(Wave);
			AC = NewObject<UAudioComponent>(Owner, *FString::Printf(TEXT("OLCoopVoice%d"), Who));
			AC->bAutoActivate = false;
			AC->bAutoDestroy = false;
			AC->bStopWhenOwnerDestroyed = true;
			if (bSpatial)
			{
				AC->SetupAttachment(Owner->GetRootComponent());
				AC->bAllowSpatialization = true;
				AC->bOverrideAttenuation = true;
				FSoundAttenuationSettings& A = AC->AttenuationOverrides;
				A.bAttenuate = bFade;
				A.bSpatialize = true;
				A.DistanceAlgorithm = EAttenuationDistanceModel::NaturalSound;
				A.AttenuationShape = EAttenuationShape::Sphere;
				A.AttenuationShapeExtents = FVector(InnerCm, 0.f, 0.f);
				A.FalloffDistance = FalloffCm;
				A.dBAttenuationAtMax = AtMaxDb;
				A.FalloffMode = ENaturalSoundFalloffMode::Hold;
				A.bAttenuateWithLPF = bFade;             // (a little duller far off: a distance cue)
				A.LPFRadiusMin = InnerCm;
				A.LPFRadiusMax = InnerCm + FalloffCm;
				A.LPFFrequencyAtMin = 20000.f;
				A.LPFFrequencyAtMax = 5000.f;
			}
			else
			{
				AC->bAllowSpatialization = false;
				AC->bOverrideAttenuation = false;
				AC->bIsUISound = true;                   // (plays on while a single player's menu would pause)
			}
			AC->SetSound(Wave);
			AC->RegisterComponent();
			AC->SetVolumeMultiplier(1.f);            // (the voice's own path: the gain is in the samples, master never applied)
			T.Comp = AC;
			T.bSpatial = bSpatial;
			T.bFade = bFade;
			T.Gain = Gain;
			UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP voice: the %s player's voice volume x%.2f (partner volume %d%%, independent of the master volume x%.2f; soft-limited samples, no clipping), %s"), WhoWord(Who), Gain, L->MpPartnerVolume,
				L->MasterVolume, !bSpatial ? TEXT("2D, no attenuation") : bFade ? TEXT("spatial, attenuation on (fades with distance)") : TEXT("spatial and panned, attenuation off (no fade with distance)"));
			++V.TalkerChanges;
			if (bSpatial)
			{
				AC->SetWorldLocation(R->ShownHead());
				const AOutlawsPlayer* P = LocalPlayer();
				const UCameraComponent* Cam = P ? P->Camera.Get() : nullptr;
				const FVector Ear = Cam ? Cam->GetComponentLocation() : P ? P->GetActorLocation() : FVector::ZeroVector;
				const float Dist = float(FVector::Dist(Ear, R->ShownHead()));
				const float Vol = AC->AttenuationOverrides.Evaluate(FTransform(R->ShownHead()), Ear);
				T.LastTalker = FString::Printf(TEXT("spatial at the puppet's head %s"), *R->ShownHead().ToCompactString());
				if (!bFade)
				{
					UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP voice: the %s player's voice is spatial, attached to their puppet at head height %s, panned, no fall with distance (Voice fades with distance off): this player's ears %.0f cm away: volume x%.2f there and everywhere"),
						WhoWord(Who), *R->ShownHead().ToCompactString(), Dist, Gain);
				}
				else
				{
					UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP voice: the %s player's voice is spatial, attached to their puppet at head height %s; attenuation natural sound: full to %.0f cm, %.0f dB at %.0f cm and held beyond, panned, low-pass to %.0f Hz far off; this player's ears %.0f cm away: volume x%.2f (x%.2f at 10 m, x%.2f at 20 m, x%.2f at 30 m, x%.2f at 60 m)"),
						WhoWord(Who), *R->ShownHead().ToCompactString(), InnerCm, AtMaxDb, InnerCm + FalloffCm, AC->AttenuationOverrides.LPFFrequencyAtMax, Dist, Vol,
						AC->AttenuationOverrides.Evaluate(FTransform::Identity, FVector(1000.0, 0.0, 0.0)), AC->AttenuationOverrides.Evaluate(FTransform::Identity, FVector(2000.0, 0.0, 0.0)),
						AC->AttenuationOverrides.Evaluate(FTransform::Identity, FVector(3000.0, 0.0, 0.0)), AC->AttenuationOverrides.Evaluate(FTransform::Identity, FVector(6000.0, 0.0, 0.0)));
				}
			}
			else
			{
				T.LastTalker = TEXT("2D (normal volume)");
				UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP voice: the %s player's voice plays 2D at normal volume (%s)"), WhoWord(Who),
					L->IsFrontEnd() || !bLevelBuilt ? TEXT("the lobby") : TEXT("their puppet not drawn yet"));
			}
			V.LastTalker = T.LastTalker;
			if (T.bTalking) AC->Play();
		}
		if (bSpatial) AC->SetWorldLocation(R->ShownHead());
		if (FMath::Abs(AC->VolumeMultiplier - 1.f) > 0.01f) AC->SetVolumeMultiplier(1.f);
	}
}

FString UOutlawsCoop::VoiceSummary() const
{
	if (!Voice.IsValid()) return TEXT("never used (the microphone off, nothing received)");
	const FOutlawsCoopVoice& V = *Voice;
	const double Secs = FMath::Max(1, V.SecondsWithVoice);
	int32 Drops = 0;
	FString Rx;
	for (int32 Who = 0; Who < MaxPlayers; ++Who)
	{
		const OLVoicePipe::FTalkerRx& R = V.Talkers[Who].Rx;
		Drops += R.Trims + R.HardResets;
		if (R.Packets > 0) Rx += FString::Printf(TEXT("; from the %s: %s"), WhoWord(Who), *R.Summary());
	}
	if (V.Reader.Ticks > 0) Rx += FString::Printf(TEXT("; microphone %s"), *V.Reader.Summary());
	return FString::Printf(TEXT("microphone %s at the end (%s), packets sent %d (%d bytes, %.1f a second over %d seconds of voice, peak %d a second), received %d (%d bytes, decoded %d, queued to play %d, no audio device for %d, peak %d a second), sent while off %d, gate opened %d, stale frames dropped %d, talking on %d off %d, talker made %d times (last: %s), queue drops %d, saturated ticks %d, passed on to other clients %d"),
		V.bWanted ? TEXT("on") : TEXT("off"), bAuto && TestOnly == TEXT("voice") ? TEXT("the test's switch, a test tone") : MicStateText(MicState(this)),
		V.Sent, V.BytesSent, V.Sent / Secs, V.SecondsWithVoice, V.PeakSent,
		V.Received, V.BytesReceived, V.Decoded, V.Queued, V.NoAudio, V.PeakReceived, V.SentWhileOff, V.GateOpens, V.Stale, V.TalkOns, V.TalkOffs, V.TalkerChanges,
		V.LastTalker.IsEmpty() ? TEXT("none") : *V.LastTalker, Drops, V.SaturatedTicks, V.Relayed) + Rx;
}

// ---- its test (-OLCoopAuto -OLCoopTest=voice) ----------------------------------------------------------------------------
// Both machines feed a test tone instead of the microphone (the runs have -nosound: the plumbing is what is checked).  The
// lobby: the setting off for 2.5 s after the client connects (nothing may flow), then on (both send, each plays the other
// 2D); the host waits 8 s there before it starts.  The level: both talk on (the talker moves onto the puppet, spatial);
// the host asks the client to report, then to turn its setting off (the host must stop hearing it), then both quit.

#if WITH_OUTLAWS_TESTS
void UOutlawsCoop::TickHostVoiceTest(double Now, AOutlawsLevelActor* L, AOutlawsPlayer* P)
{
	FOutlawsCoopVoice& V = VoiceState();
	AOutlawsCoopLink* CL = GetLink(1);
	switch (TestStep)
	{
	case 300:
		// (both built, each drawing the other: a few seconds of talk in the level)
		if (!PartnerPuppet() || !CL || CL->BuiltSerial != StartedSerial) return;
		TestStep = 301;
		TestAt = Now + 5.0;
		return;
	case 301:
	{
		if (Now < TestAt) return;
		const FOutlawsCoopVoice::FTalker& T = V.Partner(*this);
		const UAudioComponent* AC = T.Comp.Get();
		const AOutlawsRemotePlayer* R = PartnerPuppet();
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP voice test: the host hears the client %s: talker %s, attached to %s, at %s (puppet head %s), received %d, talking now %d; %s"),
			T.bSpatial ? TEXT("SPATIAL") : TEXT("2D"), *T.LastTalker, AC && AC->GetOwner() ? *AC->GetOwner()->GetClass()->GetName() : TEXT("nothing"),
			AC ? *AC->GetComponentLocation().ToCompactString() : TEXT("-"), R ? *R->ShownHead().ToCompactString() : TEXT("-"), V.Received, IsRemoteTalking(this, 1) ? 1 : 0, *OLVoiceHeard(AC, LocalPlayer()));
		TestSay(TEXT("vc:report"), 0);
		TestStep = 302;
		TestAt = Now + 1.5;
		return;
	}
	case 302:
		if (Now < TestAt) return;
		TestSay(TEXT("vc:micoff"), 0);
		TestStep = 303;
		TestAt = Now + 1.0;
		return;
	case 303:
		if (Now < TestAt) return;
		V.TestReceivedAtMark = V.Received;      // (from here nothing more may come from the client)
		TestStep = 304;
		TestAt = Now + 3.0;
		return;
	case 304:
		if (Now < TestAt) return;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP voice test: after the client's setting went off the host received %d more packets (talking now %d): %s"),
			V.Received - V.TestReceivedAtMark, IsRemoteTalking(this, 1) ? 1 : 0, V.Received == V.TestReceivedAtMark && !IsRemoteTalking(this, 1) ? TEXT("gated") : TEXT("NOT GATED"));
		TestSay(TEXT("quit"), 0);
		TestStep = 100;
		TestAt = Now + 3.0;
		return;
	default:
		return;
	}
}

void UOutlawsCoop::OnVoiceTestStep(const FString& Step, int32 Arg)
{
	FOutlawsCoopVoice& V = VoiceState();
	if (Step == TEXT("report"))
	{
		const FOutlawsCoopVoice::FTalker& T = V.Partner(*this);
		const UAudioComponent* AC = T.Comp.Get();
		const AOutlawsRemotePlayer* R = PartnerPuppet();
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP voice test: the client hears the host %s: talker %s, attached to %s, at %s (puppet head %s), received %d, talking now %d; %s"),
			T.bSpatial ? TEXT("SPATIAL") : TEXT("2D"), *T.LastTalker, AC && AC->GetOwner() ? *AC->GetOwner()->GetClass()->GetName() : TEXT("nothing"),
			AC ? *AC->GetComponentLocation().ToCompactString() : TEXT("-"), R ? *R->ShownHead().ToCompactString() : TEXT("-"), V.Received, IsRemoteTalking(this, 0) ? 1 : 0, *OLVoiceHeard(AC, LocalPlayer()));
	}
	else if (Step == TEXT("micoff"))
	{
		V.TestMic = 0;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLCOOP voice test: the client's microphone setting on -> off (%d packets sent so far)"), V.Sent);
	}
}
#endif // WITH_OUTLAWS_TESTS
