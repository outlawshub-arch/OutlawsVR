// Co-op voice: the parts of the voice path that keep the sound clean over a long session (BUG-MICSTATIC, tester dotard
// 2026-10-10: a Quest 2's voice turned to static about a minute after joining; leaving and rejoining fixed it for a minute).
//
// The cause: the engine's Android capture (VoiceModuleAndroid.cpp, OpenSL ES) keeps the microphone in an 8 KB byte ring
// that holds at most 8191 bytes.  Its recorder callback writes 2048 bytes (64 ms) at a time and, when the ring is nearly
// full (the game thread did not read for about 200 ms: a hitch, a level being built, a menu), writes only what fits: an odd
// number of bytes.  From then on every 16-bit sample read is made of the high byte of one sample and the low byte of the
// next: full-scale white noise, sent and played until the capture is made again (a rejoin) or another stall flips it back.
//
// The pieces here (plain C++, no UObjects, so the long-run soak test drives the same code as the game):
//  - FMicReader: the microphone's bytes into whole 16 kHz samples.  A tick's reads that may hold a cut-short ring write are
//    dropped (they are a stall's stale sound anyway) and the sample grid is set again from the byte count, so a slipped
//    byte never reaches the encoder.  It also measures the rate the device really delivers and resamples a device that
//    ignores the asked rate (44.1 or 48 kHz) to 16 kHz.
//  - FTalkerRx: one other player's packets: in order (the Opus header's generation byte), late or repeated packets dropped,
//    the decoder reset on a gap or a new stream, the play queue kept between bounds (the excess cut, never all of it), and
//    any loud wideband noise that still arrives (an older build's static) muted instead of played.
//  - RunVoiceSoak: -OLVoiceSoak[=legacy]: the long-run test (OutlawsCoopVoiceSoak.cpp).
#pragma once

#include "CoreMinimal.h"

class IVoiceDecoder;

namespace OLVoicePipe
{
	constexpr int32 Rate = 16000;                     // the voice's one rate: capture (after the reader), Opus, the play queue
	constexpr int32 FrameSamples = Rate / 50;         // 20 ms

	/** The partner volume on one sample through a soft limiter (REQ-VOICEVOL: never clips or wraps). */
	int16 GainSample(int16 In, float Gain);

	/** A 20 ms frame of loud, flat, wideband noise (a byte-slipped stream): RMS of full scale and zero-crossing rate. */
	bool IsNoiseFrame(const int16* S, int32 N, float* OutRms = nullptr, float* OutZcr = nullptr);

	/** Resamples 16-bit mono from one rate to another (windowed sinc, low-passed below the lower Nyquist). */
	struct FResampler
	{
		int32 From = Rate, To = Rate;
		double Pos = 0.0;                             // the next output's position in History (input samples)
		TArray<float> History;
		void Reset(int32 InFrom, int32 InTo);
		void Process(const int16* In, int32 N, TArray<uint8>& Out);
	};

	/** The microphone's bytes, one tick at a time, made into clean 16 kHz 16-bit samples. */
	struct FMicReader
	{
		bool bLegacy = false;                         // the soak test's "before": every byte appended as read (the old PumpMic)
		int32 Requested = Rate;                       // the rate the capture was opened at
		int32 DeviceRate = Rate;                      // the rate it really delivers (measured; resampled to Rate when different)
		int32 RingBytes = 0;                          // the capture's own buffer (the Android ring: 8192), 0 unknown
		uint64 BytesIn = 0;                           // every byte read since the capture opened
		int32 GridParity = 0;                         // a sample starts at stream bytes of this parity
		// counts (logged)
		int32 Ticks = 0, Slips = 0, FullRings = 0, DroppedTicks = 0, RateSwitches = 0, NoiseFrames = 0;
		uint64 BytesDropped = 0;
		double MeasuredRate = 0.0;                    // the last window's delivered rate (samples a second, as 16-bit mono)
		// the rate measurement
		double WindowAt = 0.0;
		uint64 WindowBytes = 0;
		int32 Candidate = 0, CandidateHits = 0;
		bool bRateLogged = false;
		FResampler Resampler;
		// this tick
		TArray<uint8> TickBuf;

		void Open(int32 InRequested, int32 InRingBytes, double Now, bool bInLegacy = false);
		void BeginTick() { TickBuf.Reset(); }
		void Add(const uint8* Data, int32 Bytes) { if (Bytes > 0) TickBuf.Append(Data, Bytes); }
		/** Appends this tick's clean samples to Out (bytes, 16-bit, Rate).  bStale: the caller drops this tick's sound (a long
		 *  frame).  Returns the bytes appended, or -1 when the tick was dropped.  OutLog: a line worth logging, or empty. */
		int32 EndTick(double Now, bool bStale, TArray<uint8>& Out, FString& OutLog);
		FString Summary() const;
	};

	/** Where decoded voice goes: the procedural wave in the game, a simulated device in the soak test. */
	struct IVoiceSink
	{
		virtual ~IVoiceSink() {}
		virtual int32 QueuedSamples() const = 0;
		virtual void Queue(const int16* S, int32 N) = 0;
		virtual void Clear() = 0;
		virtual bool Plays() const { return true; }     // (false: nothing to play on, -nosound; its emptiness means nothing)
	};

	/** One other player's voice, packet by packet. */
	struct FTalkerRx
	{
		bool bLegacy = false;                         // the soak test's "before": the old OnVoice (every packet, a full reset over 0.4 s)
		int32 LastGen = -1;
		double LastAt = 0.0;
		int16 LastSample = 0;
		bool bMuting = false;
		// counts (logged)
		int32 Packets = 0, Decoded = 0, Gaps = 0, Lost = 0, Late = 0, Repeats = 0, NewStreams = 0, DecoderResets = 0;
		int32 Trims = 0, HardResets = 0, Underruns = 0, NoiseFrames = 0, MutedFrames = 0, Prerolls = 0;
		int64 TrimmedSamples = 0;
		int32 MaxQueued = 0;
		TArray<int16> Block;

		/** One packet: false when it was not played (late, repeated, undecodable).  bNewSpurt: the talker was silent before. */
		bool Receive(IVoiceDecoder& Decoder, const uint8* Data, int32 Num, double Now, bool bNewSpurt, float Gain, IVoiceSink& Sink, FString& OutLog);
		FString Summary() const;
	};

	/** -OLVoiceSoak[=legacy]: the long-run voice test (OutlawsCoopVoiceSoak.cpp); logs OLVOICESOAK lines, returns true on a pass. */
	bool RunVoiceSoak(bool bLegacy, double Seconds);
}
