// -OLVoiceSoak[=legacy] [-OLVoiceSoakSeconds=360]: the long-run voice test (BUG-MICSTATIC).
//
// One talker's whole voice path, simulated in time and run faster than real time, with the game's own pieces where they
// exist: a fake microphone (a 440 Hz tone at 44.1 or 48 kHz on its own slightly fast or slow clock), the platform's
// resampling to the 16 kHz asked for (or, as a device that ignores the asked rate, none), the engine's Android capture
// ring copied in behaviour (8 KB, 2048-byte writes, a write cut short to what fits), the sender's game ticks with stalls,
// the microphone reader (FMicReader), the 20 ms framing and the engine's own Opus encoder and decoder, a network with delay,
// jitter, loss and reordering, the receiver's ticks with stalls, the talker (FTalkerRx) and a play device draining the
// queue on its own clock.  What is heard is scored a 20 ms block at a time against the tone (a sine fitted per block).
// "legacy" runs the reader and the talker as the code was before the fix (every byte as read, every packet played, the
// whole queue dropped over 0.4 s).  A beside-ring marks the real low byte of every sample, so a frame encoded off the
// sample grid (a "wrap" of the byte stream) is counted exactly.

#include "OutlawsCoopVoicePipe.h"
#include "OutlawsRuntime.h"
#include "VoiceModule.h"
#include "Interfaces/VoiceCodec.h"
#include "Math/RandomStream.h"
#include "Algo/Sort.h"

#if WITH_OUTLAWS_TESTS

namespace
{
	using namespace OLVoicePipe;

	/** VoiceModuleAndroid.cpp's circular buffer (the same space rule: size - 1 usable), with the truth ring beside it. */
	struct FAndroidRing
	{
		int32 Size = 8192, Wp = 0, Rp = 0;
		TArray<uint8> Buf, Low;
		int32 CutWrites = 0;
		FAndroidRing() { Buf.SetNumZeroed(Size); Low.SetNumZeroed(Size); }
		int32 Free() const { return Wp > Rp ? Rp - Wp + Size - 1 : Wp < Rp ? Rp - Wp - 1 : Size - 1; }
		int32 Used() const { return Wp > Rp ? Wp - Rp : Wp < Rp ? Wp - Rp + Size : 0; }
		void Write(const uint8* In, const uint8* InLow, int32 Bytes)
		{
			const int32 R = Free();
			if (R == 0) { ++CutWrites; return; }
			const int32 W = FMath::Min(Bytes, R);
			if (W < Bytes) ++CutWrites;
			for (int32 i = 0; i < W; ++i)
			{
				Buf[Wp] = In[i];
				Low[Wp] = InLow[i];
				if (++Wp == Size) Wp = 0;
			}
		}
		int32 Read(uint8* Out, uint8* OutLow, int32 Bytes)
		{
			const int32 R = FMath::Min(Bytes, Used());
			for (int32 i = 0; i < R; ++i)
			{
				Out[i] = Buf[Rp];
				OutLow[i] = Low[Rp];
				if (++Rp == Size) Rp = 0;
			}
			return R;
		}
	};

	/** The receiver's play queue, drained by the device on its own clock. */
	struct FSimSink : IVoiceSink
	{
		TArray<int16> Q;
		int32 Head = 0;
		virtual int32 QueuedSamples() const override { return Q.Num() - Head; }
		virtual void Queue(const int16* S, int32 N) override { Q.Append(S, N); }
		virtual void Clear() override { Q.Reset(); Head = 0; }
		int16 Pop(bool& bDry)
		{
			if (Head >= Q.Num()) { bDry = true; return 0; }
			bDry = false;
			const int16 S = Q[Head++];
			if (Head > 65536) { Q.RemoveAt(0, Head, false); Head = 0; }
			return S;
		}
	};

	struct FScenario
	{
		const TCHAR* Name;
		int32 MicRate;          // the fake microphone's own rate
		bool bOsResample;       // the platform hands over the asked 16 kHz (Android's AudioFlinger); false: the raw rate as if 16 kHz
		double MicSkew;         // the microphone's clock against the listener's (+0.005: 0.5% fast)
		double RxSkew;          // the listener's device clock
		bool bSendStalls;       // the sender's game thread stalls now and then (a Quest 2: 0.2 to 0.6 s)
		bool bRecvStalls;
	};

	struct FPacket { double At; int32 Seq; TArray<uint8> Data; };

	struct FResult
	{
		int32 Blocks = 0, Silent = 0, Voiced = 0, Clean = 0, Static = 0;
		double MedianSnr = 0.0;
		double MaxDepth = 0.0, P99Depth = 0.0;
		int32 MisalignedFrames = 0, Frames = 0, Packets = 0, Lost = 0, Reordered = 0, Stalls = 0, CutWrites = 0;
		int32 DrySamples = 0;
		FString Reader, Talker;
		bool bMirror = true;
	};

	FResult RunOne(const FScenario& Sc, bool bLegacy, double Seconds, uint32 Seed)
	{
		FResult Res;
		FRandomStream Rng{ int32(Seed) };
		const double Tone = 440.0, Amp = 0.3;
		const float Gain = 2.f;                                            // (the partner volume's default, 200%)
		TSharedPtr<IVoiceEncoder> Enc = FVoiceModule::Get().CreateVoiceEncoder(Rate, 1, EAudioEncodeHint::VoiceEncode_Voice);
		TSharedPtr<IVoiceDecoder> Dec = FVoiceModule::Get().CreateVoiceDecoder(Rate, 1);
		if (!Enc.IsValid() || !Dec.IsValid())
		{
			UE_LOG(LogOutlawsCoop, Error, TEXT("OLVOICESOAK no Opus encoder or decoder (the Voice module off?)"));
			Res.Blocks = -1;
			return Res;
		}
		Enc->SetBitrate(20000);

		// the microphone and the platform
		FResampler Os;
		Os.Reset(Sc.MicRate, Rate);
		int64 MicSamples = 0;
		TArray<uint8> Pending;                                             // delivered, not yet a whole 1024-sample write
		FAndroidRing Ring;
		TArray<uint8> Chunk, ChunkLow, Low;
		ChunkLow.SetNumUninitialized(2048);
		for (int32 i = 0; i < 2048; ++i) ChunkLow[i] = (i & 1) == 0 ? 1 : 0;

		// the sender
		FMicReader Reader;
		Reader.Open(Rate, Ring.Size, 0.0, bLegacy);
		TArray<uint8> Pcm, PcmLow, Out, Raw, RawLow, TickLow, Enc8;
		Raw.SetNumUninitialized(4096);
		RawLow.SetNumUninitialized(4096);
		Enc8.SetNumUninitialized(4096);
		double LastPump = 0.0, NextSendStall = 10.0 + Rng.FRandRange(0.f, 20.f);
		int32 Gen = 0;
		double LastArrival = 0.0;
		TArray<FPacket> Net;

		// the receiver
		FTalkerRx Rx;
		Rx.bLegacy = bLegacy;
		FSimSink Sink;
		bool bTalking = false;
		double LastHeard = -10.0, PlayAcc = 0.0, LastRecv = 0.0, NextRecvStall = 15.0 + Rng.FRandRange(0.f, 25.f);
		TArray<int16> Heard;
		Heard.Reserve(int32(Seconds * Rate * 1.02) + Rate);
		TArray<float> Depths;

		double Ts = 0.0, Tr = 0.0;
		while (FMath::Min(Ts, Tr) < Seconds)
		{
			if (Ts <= Tr)
			{
				const double Now = Ts;
				// the microphone up to now, on its own clock, through the platform, into the ring 2048 bytes at a time
				const int64 Due = int64(Now * Sc.MicRate * (1.0 + Sc.MicSkew));
				TArray<int16> Hw;
				Hw.Reserve(int32(Due - MicSamples));
				for (; MicSamples < Due; ++MicSamples)
				{
					Hw.Add(int16(FMath::RoundToInt(Amp * 32767.0 * FMath::Sin(2.0 * PI * Tone * double(MicSamples) / Sc.MicRate))));
				}
				if (Sc.bOsResample) Os.Process(Hw.GetData(), Hw.Num(), Pending);
				else Pending.Append(reinterpret_cast<const uint8*>(Hw.GetData()), Hw.Num() * 2);
				int32 Taken = 0;
				while (Pending.Num() - Taken >= 2048)
				{
					Ring.Write(Pending.GetData() + Taken, ChunkLow.GetData(), 2048);
					Taken += 2048;
				}
				if (Taken > 0) Pending.RemoveAt(0, Taken, false);

				// PumpMic: the stale rule, the reads (asked 1024: up to 2048 bytes each), the reader
				const bool bStale = LastPump > 0.0 && Now - LastPump > 0.3;
				LastPump = Now;
				if (bStale)
				{
					Pcm.Reset();
					PcmLow.Reset();
					Out.Reset();
				}
				Reader.BeginTick();
				TickLow.Reset();
				for (int32 Reads = 0; Reads < 32; ++Reads)
				{
					const int32 Got = Ring.Read(Raw.GetData(), RawLow.GetData(), 2048);
					if (Got == 0) break;
					Reader.Add(Raw.GetData(), Got);
					TickLow.Append(RawLow.GetData(), Got);
				}
				FString Log;
				const int32 Added = Reader.EndTick(Now, bStale, Pcm, Log);
				if (Added >= 0)
				{
					if (Reader.DeviceRate == Rate && Added == TickLow.Num()) PcmLow.Append(TickLow);
					else Res.bMirror = false;
				}
				if (Pcm.Num() > Rate * 2)
				{
					// (a long hitch: only the last second; the old code cut any count, the new an even one)
					const int32 Cut = bLegacy ? Pcm.Num() - Rate * 2 : (Pcm.Num() - Rate * 2 + 1) & ~1;
					Pcm.RemoveAt(0, Cut, false);
					if (PcmLow.Num() >= Cut) PcmLow.RemoveAt(0, Cut, false);
				}
				// the gate is open throughout (a loud tone): 20 ms frames, three to a packet
				int32 Pos = 0;
				const int32 FrameBytes = FrameSamples * 2;
				while (Pcm.Num() - Pos >= FrameBytes)
				{
					++Res.Frames;
					if (Res.bMirror && PcmLow.IsValidIndex(Pos) && PcmLow[Pos] == 0) ++Res.MisalignedFrames;
					Out.Append(Pcm.GetData() + Pos, FrameBytes);
					Pos += FrameBytes;
					if (Out.Num() >= 3 * FrameBytes)
					{
						uint32 Size = uint32(Enc8.Num());
						Enc->Encode(Out.GetData(), uint32(Out.Num()), Enc8.GetData(), Size);
						Out.Reset();
						if (Size > 0)
						{
							++Res.Packets;
							++Gen;
							if (Rng.FRand() < 0.03f) { ++Res.Lost; continue; }
							FPacket P;
							P.Seq = Gen;
							// (one path keeps its order, as UE's connection does: a packet never passes the one before; 2% are held
							// back 80 ms instead, so the next ones pass them: a reordering the engine would not deliver)
							P.At = Now + 0.04 + Rng.FRandRange(0.f, 0.06f);
							if (Rng.FRand() < 0.02f) { P.At += 0.08; ++Res.Reordered; }
							else { P.At = FMath::Max(P.At, LastArrival + 0.0005); LastArrival = P.At; }
							P.Data = TArray<uint8>(Enc8.GetData(), int32(Size));
							Net.Add(MoveTemp(P));
						}
					}
				}
				if (Pos > 0)
				{
					Pcm.RemoveAt(0, Pos, false);
					if (PcmLow.Num() >= Pos) PcmLow.RemoveAt(0, Pos, false);
					else PcmLow.Reset();
				}
				// the next frame: 72 Hz with a little jitter, or a stall
				double Dt = 1.0 / 72.0 + Rng.FRandRange(-0.0015f, 0.0015f);
				if (Sc.bSendStalls && Now >= NextSendStall)
				{
					Dt = Rng.FRandRange(0.2f, 0.6f);
					++Res.Stalls;
					NextSendStall = Now + Rng.FRandRange(10.f, 40.f);
				}
				Ts += Dt;
			}
			else
			{
				const double Now = Tr;
				// the device plays what is queued up to now, on its own clock (an empty queue plays silence)
				PlayAcc += (Now - LastRecv) * Rate * (1.0 + Sc.RxSkew);
				LastRecv = Now;
				const int32 Play = int32(PlayAcc);
				PlayAcc -= Play;
				for (int32 i = 0; i < Play; ++i)
				{
					bool bDry = false;
					Heard.Add(Sink.Pop(bDry));
					if (bDry) ++Res.DrySamples;
				}
				// the packets that have come, in the order they came
				Net.Sort([](const FPacket& A, const FPacket& B) { return A.At < B.At; });
				int32 Done = 0;
				for (; Done < Net.Num() && Net[Done].At <= Now; ++Done)
				{
					if (bTalking && Now - LastHeard > 0.3) bTalking = false;
					FString Log;
					const int32 Before = Rx.Decoded;
					Rx.Receive(*Dec, Net[Done].Data.GetData(), Net[Done].Data.Num(), Now, !bTalking, Gain, Sink, Log);
					if (Rx.Decoded > Before)
					{
						bTalking = true;
						LastHeard = Now;
					}
				}
				if (Done > 0) Net.RemoveAt(0, Done, false);
				Depths.Add(float(Sink.QueuedSamples()) / Rate);
				double Dt = 1.0 / 90.0 + Rng.FRandRange(-0.001f, 0.001f);
				if (Sc.bRecvStalls && Now >= NextRecvStall)
				{
					Dt = Rng.FRandRange(0.15f, 0.35f);
					NextRecvStall = Now + Rng.FRandRange(20.f, 50.f);
				}
				Tr += Dt;
			}
		}

		// the score: 20 ms blocks, a sine at the tone fitted to each (least squares), what is left over is the error
		const double W = 2.0 * PI * Tone / Rate;
		const double Expected = Amp * Gain / FMath::Sqrt(2.0);
		TArray<double> Snrs;
		const int32 Skip = 2 * Rate;                                       // (the first two seconds: the stream starting)
		for (int32 b = Skip; b + FrameSamples <= Heard.Num(); b += FrameSamples)
		{
			++Res.Blocks;
			double Sxx = 0, Ss = 0, Sc2 = 0, Ssc = 0, Sxs = 0, Sxc = 0;
			int32 Cross = 0;
			for (int32 i = 0; i < FrameSamples; ++i)
			{
				if (i > 0 && ((Heard[b + i] >= 0) != (Heard[b + i - 1] >= 0))) ++Cross;
				const double X = Heard[b + i] / 32768.0;
				const double S = FMath::Sin(W * (b + i)), C = FMath::Cos(W * (b + i));
				Sxx += X * X; Ss += S * S; Sc2 += C * C; Ssc += S * C; Sxs += X * S; Sxc += X * C;
			}
			const double Rms = FMath::Sqrt(Sxx / FrameSamples);
			if (Rms < 0.1 * Expected) { ++Res.Silent; continue; }
			++Res.Voiced;
			const double Det = Ss * Sc2 - Ssc * Ssc;
			const double A = Det != 0 ? (Sxs * Sc2 - Sxc * Ssc) / Det : 0;
			const double B = Det != 0 ? (Sxc * Ss - Sxs * Ssc) / Det : 0;
			double Fit = 0, Err = 0;
			for (int32 i = 0; i < FrameSamples; ++i)
			{
				const double X = Heard[b + i] / 32768.0;
				const double Y = A * FMath::Sin(W * (b + i)) + B * FMath::Cos(W * (b + i));
				Fit += Y * Y;
				Err += (X - Y) * (X - Y);
			}
			const double Snr = 10.0 * FMath::LogX(10.0, FMath::Max(Fit, 1e-12) / FMath::Max(Err, 1e-12));
			Snrs.Add(Snr);
			if (Snr >= 10.0) ++Res.Clean;
			// static: loud and broadband (the tone crosses zero 0.055 times a sample; white noise about 0.5)
			if (Rms > 0.5 * Expected && double(Cross) / (FrameSamples - 1) > 0.25) ++Res.Static;
		}
		if (Snrs.Num() > 0)
		{
			Algo::Sort(Snrs);
			Res.MedianSnr = Snrs[Snrs.Num() / 2];
		}
		if (Depths.Num() > 0)
		{
			Algo::Sort(Depths);
			Res.MaxDepth = Depths.Last();
			Res.P99Depth = Depths[FMath::Min(Depths.Num() - 1, int32(Depths.Num() * 0.99))];
		}
		Res.CutWrites = Ring.CutWrites;
		Res.Reader = Reader.Summary();
		Res.Talker = Rx.Summary();
		return Res;
	}
}

bool OLVoicePipe::RunVoiceSoak(bool bLegacy, double Seconds)
{
	if (!FVoiceModule::IsAvailable() && !FModuleManager::Get().LoadModule(TEXT("Voice")))
	{
		UE_LOG(LogOutlawsCoop, Error, TEXT("OLVOICESOAK the Voice module is not there"));
		return false;
	}
	const FScenario Scenarios[] = {
		{ TEXT("mic44k-os-fast"),   44100, true,  +0.005,  0.0,    false, false },
		{ TEXT("mic48k-os-slow"),   48000, true,  -0.005,  0.0,    false, false },
		{ TEXT("quest2-stalls"),    48000, true,  +0.0002, -0.0001, true,  true  },
		{ TEXT("mic44k-raw"),       44100, false, +0.001,  0.0,    false, false },
		{ TEXT("mic48k-raw-stalls"),48000, false, -0.001,  0.0,    true,  true  },
	};
	const TCHAR* Mode = bLegacy ? TEXT("legacy (before the fix)") : TEXT("current");
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLVOICESOAK start: %s code, %.0f s of voice a scenario, a 440 Hz tone, loss 3%%, reordering 2%%, delay 40-100 ms"), Mode, Seconds);
	int32 Failed = 0;
	uint32 Seed = 1009;
	for (const FScenario& Sc : Scenarios)
	{
		const double T0 = FPlatformTime::Seconds();
		const FResult R = RunOne(Sc, bLegacy, Seconds, Seed++);
		if (R.Blocks < 0) return false;
		const double VoicedFrac = R.Blocks > 0 ? double(R.Voiced) / R.Blocks : 0.0;
		const double CleanFrac = R.Voiced > 0 ? double(R.Clean) / R.Voiced : 0.0;
		const double StaticFrac = R.Blocks > 0 ? double(R.Static) / R.Blocks : 0.0;
		// the bounds: the queue never deeper than half a second; no frame encoded off the sample grid; no static; the tone
		// clean in at least 90% of what is heard; and heard at least 85% of the time (loss is 3%, reordering 2%)
		const bool bDepth = R.MaxDepth <= 0.5;
		const bool bWrap = !R.bMirror || R.MisalignedFrames == 0;
		const bool bStatic = StaticFrac <= 0.005;
		const bool bClean = CleanFrac >= 0.90;
		const bool bHeard = VoicedFrac >= 0.85;
		const bool bPass = bDepth && bWrap && bStatic && bClean && bHeard;
		if (!bPass) ++Failed;
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLVOICESOAK %s %s: %s; heard %.1f%% of %d blocks (%s), clean %.1f%% (%s, median SNR %.1f dB), static %.2f%% (%s), queue deepest %.3f s p99 %.3f s (%s), frames off the sample grid %d of %d (%s), ring writes cut short %d, sender stalls %d, packets %d lost %d reordered %d, device ran dry %.2f s; %.1f s to run"),
			bLegacy ? TEXT("legacy") : TEXT("current"), Sc.Name, bPass ? TEXT("PASS") : TEXT("FAIL"),
			100.0 * VoicedFrac, R.Blocks, bHeard ? TEXT("ok") : TEXT("FAIL"), 100.0 * CleanFrac, bClean ? TEXT("ok") : TEXT("FAIL"), R.MedianSnr,
			100.0 * StaticFrac, bStatic ? TEXT("ok") : TEXT("FAIL"), R.MaxDepth, R.P99Depth, bDepth ? TEXT("ok") : TEXT("FAIL"),
			R.MisalignedFrames, R.Frames, !R.bMirror ? TEXT("n/a, resampled") : bWrap ? TEXT("ok") : TEXT("FAIL"), R.CutWrites, R.Stalls, R.Packets, R.Lost, R.Reordered,
			double(R.DrySamples) / Rate, FPlatformTime::Seconds() - T0);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLVOICESOAK %s %s reader: %s"), bLegacy ? TEXT("legacy") : TEXT("current"), Sc.Name, *R.Reader);
		UE_LOG(LogOutlawsCoop, Log, TEXT("OLVOICESOAK %s %s talker: %s"), bLegacy ? TEXT("legacy") : TEXT("current"), Sc.Name, *R.Talker);
	}
	UE_LOG(LogOutlawsCoop, Log, TEXT("OLVOICESOAK done: %s code: %s (%d of %d scenarios failed)"), Mode, Failed == 0 ? TEXT("ALL PASS") : TEXT("FAIL"), Failed, int32(UE_ARRAY_COUNT(Scenarios)));
	return Failed == 0;
}
#endif // WITH_OUTLAWS_TESTS
