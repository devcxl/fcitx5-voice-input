#pragma once

#include "utils/audio_buffer.h"

namespace fcitx {

class AudioCapture {
public:
    virtual ~AudioCapture() = default;

    virtual bool Start() = 0;
    virtual void Stop() = 0;
    virtual bool IsRunning() const = 0;
    virtual const char* Name() const = 0;

    virtual const AudioRingBuffer* RingBuffer() const = 0;
    virtual AudioRingBuffer* RingBuffer() = 0;
};

} // namespace fcitx
