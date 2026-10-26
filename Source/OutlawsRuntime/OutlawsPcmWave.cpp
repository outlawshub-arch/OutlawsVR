#include "OutlawsPcmWave.h"
#include "Async/Async.h"

std::atomic<int64> UOutlawsPcmWave::BytesPlayed{ 0 };

void UOutlawsPcmWave::FinishDestroy()
{
	// (the audio thread is done with it by now: FinishDestroy waits for IsReadyForFinishDestroy).  A music track's samples,
	// when this wave holds the last of them, freed on a worker; a short sound's are only shared with the level's cache
	if (Pcm.IsValid() && Pcm->Num() > (1 << 20) && Pcm.IsUnique())
	{
		Async(EAsyncExecution::ThreadPool, [Old = MoveTemp(Pcm)]() mutable { Old.Reset(); });
	}
	Pcm.Reset();
	Super::FinishDestroy();
}

int32 UOutlawsPcmWave::OnGeneratePCMAudio(TArray<uint8>& OutAudio, int32 NumSamples)
{
	// on the audio render thread: the next NumSamples 16-bit samples from the shared sound, round again for a loop, the
	// tail after the end silence (the level stops the sound once its time is up)
	const TArray<uint8>* P = Pcm.Get();
	OutAudio.Reset();
	if (!P || P->Num() < 2 || NumSamples <= 0 || (Offset >= P->Num() && !bLoopPcm)) return 0;
	const int64 Total = P->Num();
	const int64 Need = int64(NumSamples) * 2;
	OutAudio.AddUninitialized(int32(Need));
	uint8* Dst = OutAudio.GetData();
	int64 Done = 0;
	while (Done < Need)
	{
		if (Offset >= Total)
		{
			if (!bLoopPcm) break;
			Offset = 0;
		}
		const int64 N = FMath::Min(Need - Done, Total - Offset);
		FMemory::Memcpy(Dst + Done, P->GetData() + Offset, N);
		Done += N;
		Offset += N;
	}
	if (Done < Need) FMemory::Memzero(Dst + Done, Need - Done);
	BytesPlayed += Done;
	return NumSamples;
}
