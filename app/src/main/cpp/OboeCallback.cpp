#include "OboeCallback.h"
#include "types.h"
#include "Platform.h"
#include "SPU.h"

using namespace melonDS;

#define INTERNAL_FRAME_RATE 59.8260982880808f
#define FIFO_TARGET_FILL 2048.0
#define MAX_RATE_CORRECTION 0.005

OboeCallback::OboeCallback(int volume, void (*onErrorCallback)(void), std::ostream* recordingStream) : _volume(volume), onErrorCallback(onErrorCallback), _recordingStream(recordingStream) {
    audioSampleFrac = 0;
    fifoFillAverage = FIFO_TARGET_FILL;
}

oboe::DataCallbackResult
OboeCallback::onAudioReady(oboe::AudioStream *stream, void *audioData, int32_t numFrames) {
    auto currentInstance = activeInstance.lock();

    if (!currentInstance)
    {
        memset(audioData, 0, numFrames * sizeof(u16) * 2);
        return oboe::DataCallbackResult::Continue;
    }

    int len = numFrames;

    // The emulator and the audio device run on different clocks. Adjust the resampling ratio to keep the FIFO half full
    int fifoFill = currentInstance->getAudioOutputFill();
    fifoFillAverage += (fifoFill - fifoFillAverage) * 0.05;
    double correction = std::clamp((fifoFillAverage - FIFO_TARGET_FILL) / FIFO_TARGET_FILL * MAX_RATE_CORRECTION, -MAX_RATE_CORRECTION, MAX_RATE_CORRECTION);
    double skew = (60.0 / INTERNAL_FRAME_RATE) * (1.0 + correction);
    currentInstance->setAudioOutputSkew(skew);

    int len_in = getNumSamplesOut(len);
    if (len_in > numFrames) len_in = numFrames;

    int num_in = currentInstance->readAudioOutput((s16*) audioData, len_in);

    if (num_in < 1)
    {
        memset(audioData, 0, len * sizeof(s16) * 2);
        return oboe::DataCallbackResult::Continue;
    }

    if (_volume < 256)
    {
        s16* samples = (s16*) audioData;
        for (int i = 0; i < num_in * 2; i++)
            samples[i] = ((s32) samples[i] * _volume) >> 8;
    }

    if (num_in < len_in)
    {
        int last = num_in - 1;

        for (int i = num_in; i < len_in; i++)
            ((u32*)audioData)[i] = ((u32*)audioData)[last];
    }

    if (_recordingStream) [[unlikely]]
        _recordingStream->write((char*) audioData, numFrames * sizeof(s16) * 2);

    return oboe::DataCallbackResult::Continue;
}

void OboeCallback::onErrorAfterClose(oboe::AudioStream* stream, oboe::Result result)
{
    if (result == oboe::Result::ErrorDisconnected && onErrorCallback != nullptr) {
        onErrorCallback();
    }
}

int OboeCallback::getNumSamplesOut(int len)
{
    // TODO: adjust to game speed
    float f_len_in = len /* * (curFPS/60.0)*/;
    f_len_in += audioSampleFrac;
    int len_in = (int) floor(f_len_in);
    audioSampleFrac = f_len_in - len_in;

    return len_in;
}