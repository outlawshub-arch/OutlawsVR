#pragma once

#include "CoreMinimal.h"
#include <atomic>
#include "Sound/SoundWaveProcedural.h"
#include "OutlawsPcmWave.generated.h"

/**
 * A sound played straight from the level's decoded samples, shared by every play of it (user 2026-10-01: a procedural
 * wave per play, fed the whole sound with QueueAudio, kept that copy until the next collection: about 9 MB a second in
 * play, 500 MB a minute on the Quest).  The audio thread asks OnGeneratePCMAudio for each callback's worth and gets it
 * from the shared samples, so a wave holds one callback's buffer and no copy of the sound.
 */
UCLASS(Transient)
class UOutlawsPcmWave : public USoundWaveProcedural
{
	GENERATED_BODY()

public:
	/** 16-bit samples, interleaved, whole frames (set before it plays, kept alive by this pointer). */
	TSharedPtr<const TArray<uint8>> Pcm;
	/** Round again at the end (a moving sector's sound) rather than going quiet. */
	bool bLoopPcm = false;

	virtual int32 OnGeneratePCMAudio(TArray<uint8>& OutAudio, int32 NumSamples) override;
	/** (BUG-STUTTER2 (5)) the last hold on a music track's samples (tens of MB) let go on a worker, not in the collection. */
	virtual void FinishDestroy() override;

	/** Bytes of sound handed to the audio thread so far, all waves (ol.Census prints it: the sounds really play). */
	static std::atomic<int64> BytesPlayed;

private:
	int64 Offset = 0;          // (the audio thread's alone)
};
