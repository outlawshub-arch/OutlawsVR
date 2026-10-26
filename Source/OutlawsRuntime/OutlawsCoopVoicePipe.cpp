// Co-op voice: the microphone reader and the talker's receive path (BUG-MICSTATIC; OutlawsCoopVoicePipe.h says why).

#include "OutlawsCoopVoicePipe.h"
#include "Interfaces/VoiceCodec.h"

namespace OLVoicePipe
{
	int16 GainSample(int16 In, float Gain)
	{
		// straight up to Knee of full scale, then a tanh knee that meets full scale only at infinity
		constexpr float Knee = 0.7f;
		const float X = float(In) / 32767.f * Gain;
		const float A = FMath::Abs(X);
		const float Y = A <= Knee ? A : Knee + (1.f - Knee) * tanhf((A - Knee) / (1.f - Knee));
		return int16(FMath::Clamp(FMath::RoundToInt(FMath::Sign(X) * Y * 32767.f), -32767, 32767));
	}

	bool IsNoiseFrame(const int16* S, int32 N, float* OutRms, float* OutZcr)
	{
		if (N < 2) return false;
		double Sum = 0.0;
		int32 Cross = 0;
		for (int32 i = 0; i < N; ++i)
		{
			Sum += double(S[i]) * double(S[i]);
			if (i > 0 && ((S[i] >= 0) != (S[i - 1] >= 0))) ++Cross;
		}
		const float Rms = float(FMath::Sqrt(Sum / N) / 32768.0);
		const float Zcr = float(Cross) / float(N - 1);
		if (OutRms) *OutRms = Rms;
		if (OutZcr) *OutZcr = Zcr;
		// a slipped byte makes each sample's top byte random: about 0.58 of full scale, crossing zero every other sample;
		// a voice that loud is voiced (few crossings), and a hiss that busy is quiet
		return Rms > 0.25f && Zcr > 0.3f;
	}

	// ---- the resampler ----------------------------------------------------------------------------------------------

	static int32 HalfTaps(int32 From, int32 To) { return 8 * FMath::Max(1, FMath::DivideAndRoundUp(From, To)); }

	void FResampler::Reset(int32 InFrom, int32 InTo)
	{
		From = FMath::Max(1, InFrom);
		To = FMath::Max(1, InTo);
		const int32 W = HalfTaps(From, To);
		History.Reset();
		History.SetNumZeroed(W);
		Pos = double(W);
	}

	void FResampler::Process(const int16* In, int32 N, TArray<uint8>& Out)
	{
		if (From == To)
		{
			Out.Append(reinterpret_cast<const uint8*>(In), N * 2);
			return;
		}
		const int32 W = HalfTaps(From, To);
		if (History.Num() < W) Reset(From, To);
		for (int32 i = 0; i < N; ++i) History.Add(float(In[i]));
		const double Step = double(From) / double(To);
		const double Fc = 0.5 * FMath::Min(1.0, double(To) / double(From)) * 0.9;   // the cut-off, cycles per input sample
		while (Pos + W + 1 < History.Num())
		{
			const int32 I0 = FMath::FloorToInt(Pos);
			double Acc = 0.0, Norm = 0.0;
			for (int32 k = I0 - W + 1; k <= I0 + W; ++k)
			{
				const double X = double(k) - Pos;
				const double Sinc = FMath::Abs(X) < 1e-9 ? 1.0 : FMath::Sin(2.0 * PI * Fc * X) / (2.0 * PI * Fc * X);
				const double Win = 0.5 + 0.5 * FMath::Cos(PI * X / double(W));             // (Hann)
				const double H = Sinc * Win;
				Acc += History[k] * H;
				Norm += H;
			}
			const int16 S = int16(FMath::Clamp(FMath::RoundToInt(Norm != 0.0 ? Acc / Norm : 0.0), -32768, 32767));
			Out.Append(reinterpret_cast<const uint8*>(&S), 2);
			Pos += Step;
		}
		const int32 Drop = FMath::FloorToInt(Pos) - W;
		if (Drop > 0)
		{
			History.RemoveAt(0, Drop, false);
			Pos -= Drop;
		}
	}

	// ---- the microphone reader --------------------------------------------------------------------------------------

	void FMicReader::Open(int32 InRequested, int32 InRingBytes, double Now, bool bInLegacy)
	{
		*this = FMicReader();
		bLegacy = bInLegacy;
		Requested = InRequested > 0 ? InRequested : Rate;
		DeviceRate = Requested;
		RingBytes = FMath::Max(0, InRingBytes);
		WindowAt = Now;
		Resampler.Reset(DeviceRate, Rate);
	}

	int32 FMicReader::EndTick(double Now, bool bStale, TArray<uint8>& Out, FString& OutLog)
	{
		++Ticks;
		const int32 N = TickBuf.Num();
		BytesIn += uint64(N);
		if (bLegacy)
		{
			// (the old PumpMic: every byte as read, the stale frame's dropped)
			if (bStale)
			{
				++DroppedTicks;
				BytesDropped += uint64(N);
				return -1;
			}
			Out.Append(TickBuf);
			return N;
		}

		// what the device really delivers: a 4 s window; only a clear excess counts (a gated or stalled capture delivers
		// less, a backlog cannot make a whole window look 25% faster), and two windows in a row must agree
		WindowBytes += uint64(N);
		const double Span = Now - WindowAt;
		if (Span >= 4.0)
		{
			MeasuredRate = double(WindowBytes) / 2.0 / Span;
			const double Ratio = MeasuredRate / double(Requested);
			int32 Cand = 0;
			if (Ratio > 0.85 && Ratio < 1.15) Cand = Requested;
			else if (Ratio > 1.25)
			{
				for (const int32 R : { 22050, 24000, 32000, 44100, 48000 })
				{
					if (FMath::Abs(MeasuredRate / double(R) - 1.0) < 0.06) Cand = R;
				}
			}
			if (Cand != 0)
			{
				CandidateHits = Cand == Candidate ? CandidateHits + 1 : 1;
				Candidate = Cand;
				if (CandidateHits >= 2 && Cand != DeviceRate)
				{
					OutLog = FString::Printf(TEXT("the microphone delivers %.0f samples a second, not the %d asked: resampled %d -> %d Hz from here"), MeasuredRate, DeviceRate, Cand, Rate);
					DeviceRate = Cand;
					Resampler.Reset(DeviceRate, Rate);
					++RateSwitches;
				}
			}
			if (!bRateLogged && OutLog.IsEmpty())
			{
				bRateLogged = true;
				OutLog = FString::Printf(TEXT("the microphone delivers %.0f samples a second (asked %d, played as %d, its buffer %d bytes)"), MeasuredRate, Requested, DeviceRate, RingBytes);
			}
			WindowAt = Now;
			WindowBytes = 0;
		}

		bool bDrop = bStale;
		// the sample grid: a cut-short write in the capture's ring leaves the stream's end on an odd byte; the samples after it
		// start there.  (Each accepted tick starts on the grid, so a tick of whole samples ends on it again.)
		const int32 Parity = int32(BytesIn & 1);
		if (Parity != GridParity)
		{
			++Slips;
			GridParity = Parity;
			bDrop = true;
			if (OutLog.IsEmpty()) OutLog = FString::Printf(TEXT("the microphone stream slipped a byte (the capture's buffer overran after a stall: %d bytes waiting); re-aligned, this tick's sound dropped (%d slips so far)"), N, Slips);
		}
		// a tick that found the ring three quarters full or more may hold a write cut short (two of them would leave the
		// parity as it was): dropped, it was at least 0.2 s late anyway
		if (RingBytes > 0 && N >= RingBytes * 3 / 4)
		{
			++FullRings;
			bDrop = true;
		}
		if (bDrop)
		{
			++DroppedTicks;
			BytesDropped += uint64(N);
			return -1;
		}
		const int32 Samples = N / 2;
		const int16* S = reinterpret_cast<const int16*>(TickBuf.GetData());
		for (int32 i = 0; i + FrameSamples <= Samples; i += FrameSamples)
		{
			if (IsNoiseFrame(S + i, FrameSamples)) ++NoiseFrames;
		}
		const int32 Before = Out.Num();
		if (DeviceRate == Rate) Out.Append(TickBuf.GetData(), Samples * 2);
		else Resampler.Process(S, Samples, Out);
		return Out.Num() - Before;
	}

	FString FMicReader::Summary() const
	{
		return FString::Printf(TEXT("reads %d ticks, %llu bytes, delivered %.0f a second (asked %d, played as %d, rate switches %d), byte slips re-aligned %d, full-buffer ticks %d, ticks dropped %d (%llu bytes), noise-like frames %d"),
			Ticks, BytesIn, MeasuredRate, Requested, DeviceRate, RateSwitches, Slips, FullRings, DroppedTicks, BytesDropped, NoiseFrames);
	}

	// ---- one talker's packets ---------------------------------------------------------------------------------------

	bool FTalkerRx::Receive(IVoiceDecoder& Decoder, const uint8* Data, int32 Num, double Now, bool bNewSpurt, float Gain, IVoiceSink& Sink, FString& OutLog)
	{
		constexpr float PrerollSeconds = 0.08f;      // silence before a spurt (a small jitter buffer)
		constexpr float HighSeconds = 0.30f;         // more than this waiting: cut back to Target
		constexpr float TargetSeconds = 0.16f;
		constexpr float HardSeconds = 0.5f;          // the queue itself deeper than this: emptied (the last resort)
		++Packets;
		if (!Data || Num < 4) return false;
		const int32 Gen = Data[1];                   // the engine's Opus header: frames, generation (0..254, one a packet)
		const int32 PacketFrames = FMath::Clamp<int32>(Data[0], 1, 6);
		bool bReset = false, bPreroll = bNewSpurt;
		int32 Fill = 0;
		if (!bLegacy)
		{
			const bool bFresh = LastGen < 0 || Now - LastAt > 1.0;
			if (bFresh)
			{
				if (LastGen >= 0) ++NewStreams;
				bReset = LastGen >= 0;
				bPreroll = true;
			}
			else
			{
				const int32 D = (Gen - LastGen + 255) % 255;
				if (D == 0) { ++Repeats; return false; }
				if (D > 64) { ++Late; return false; }               // (behind the last one played: its moment is gone)
				if (D > 1)
				{
					++Gaps;
					Lost += D - 1;
					bReset = true;                                   // (the decoder's state is for the packet that never came)
					if (!bNewSpurt) Fill = FMath::Min(D - 1, 2) * PacketFrames * FrameSamples;   // (the time kept, up to two)
				}
			}
			if (bReset)
			{
				Decoder.Reset();
				++DecoderResets;
			}
		}
		Block.SetNumUninitialized(24 * 1024);
		uint32 OutBytes = uint32(Block.Num() * 2);
		Decoder.Decode(Data, uint32(Num), reinterpret_cast<uint8*>(Block.GetData()), OutBytes);
		if (OutBytes < 2) return false;
		++Decoded;
		LastGen = Gen;
		LastAt = Now;
		int32 N = int32(OutBytes / 2);
		Block.SetNum(N, false);

		// loud wideband noise (a slipped byte on a sender, an older build's static): muted, never played
		if (!bLegacy)
		{
			int32 MutedNow = 0;
			for (int32 i = 0; i + FrameSamples <= N; i += FrameSamples)
			{
				if (IsNoiseFrame(Block.GetData() + i, FrameSamples))
				{
					++NoiseFrames;
					++MutedFrames;
					++MutedNow;
					FMemory::Memzero(Block.GetData() + i, FrameSamples * sizeof(int16));
				}
			}
			if (MutedNow > 0 && !bMuting) OutLog = FString::Printf(TEXT("noise-like sound (static) muted, not played: %d frames so far"), MutedFrames);
			bMuting = MutedNow > 0;
		}
		if (FMath::Abs(Gain - 1.f) > 0.001f)
		{
			for (int32 i = 0; i < N; ++i) Block[i] = GainSample(Block[i], Gain);
		}

		int32 Q = Sink.QueuedSamples();
		if (bLegacy)
		{
			// (the old OnVoice: over 0.4 s waiting, everything waiting dropped)
			if (Q > int32(0.4f * Rate))
			{
				Sink.Clear();
				++HardResets;
			}
		}
		else if (!bPreroll && Q == 0 && Sink.Plays() && Packets > 1)
		{
			++Underruns;                                         // (ran dry mid-spurt: a cushion again)
			bPreroll = true;
		}
		int32 Pre = bPreroll ? int32(PrerollSeconds * Rate) : 0;
		int32 Cut = 0;
		if (!bLegacy)
		{
			const int32 Total = Q + Pre + Fill + N;
			if (Total > int32(HighSeconds * Rate))
			{
				int32 Excess = Total - int32(TargetSeconds * Rate);
				int32 C = FMath::Min(Excess, Fill); Fill -= C; Excess -= C;
				C = FMath::Min(Excess, Pre); Pre -= C; Excess -= C;
				Cut = FMath::Min(Excess, N);
				if (Cut > 0)
				{
					++Trims;
					TrimmedSamples += Cut;
				}
			}
			if (Q > int32(HardSeconds * Rate))
			{
				Sink.Clear();
				++HardResets;
			}
		}
		if (Pre + Fill > 0)
		{
			TArray<int16> Silence;
			Silence.SetNumZeroed(Pre + Fill);
			Sink.Queue(Silence.GetData(), Silence.Num());
			if (Pre > 0) ++Prerolls;
			LastSample = 0;
		}
		if (N - Cut > 0)
		{
			int16* S = Block.GetData() + Cut;
			const int32 M = N - Cut;
			if (Cut > 0)
			{
				// (the join after a cut: eased in from the last sample queued, no click)
				const int32 Ease = FMath::Min(48, M);
				for (int32 i = 0; i < Ease; ++i)
				{
					const float A = float(i + 1) / float(Ease + 1);
					S[i] = int16(FMath::RoundToInt(float(LastSample) * (1.f - A) + float(S[i]) * A));
				}
			}
			Sink.Queue(S, M);
			LastSample = S[M - 1];
		}
		MaxQueued = FMath::Max(MaxQueued, Sink.QueuedSamples());
		return true;
	}

	FString FTalkerRx::Summary() const
	{
		return FString::Printf(TEXT("packets %d, decoded %d, late %d, repeated %d, gaps %d (%d lost), new streams %d, decoder resets %d, queue cuts %d (%.2f s), emptied %d, ran dry %d, deepest %.2f s, noise frames muted %d"),
			Packets, Decoded, Late, Repeats, Gaps, Lost, NewStreams, DecoderResets, Trims, double(TrimmedSamples) / Rate, HardResets, Underruns, double(MaxQueued) / Rate, MutedFrames);
	}
}
