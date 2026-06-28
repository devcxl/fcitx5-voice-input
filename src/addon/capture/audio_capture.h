#pragma once

#include <string>
#include "types.h"
#include "utils/thread_safe_queue.h"

namespace fcitx {

class AudioCapture {
public:
    virtual ~AudioCapture() = default;

    virtual bool Start() = 0;
    virtual void Stop() = 0;
    virtual bool IsRunning() const = 0;
    virtual const char* Name() const = 0;
    virtual void SetFrameQueue(ThreadSafeQueue<AudioFrame>* queue) {
        frameQueue_ = queue;
    }

protected:
    ThreadSafeQueue<AudioFrame>* frameQueue_ = nullptr;
};

} // namespace fcitx
