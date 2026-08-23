#include <atomic>
#include <chrono>
#include <thread>

#include "asr/asr_session.h"

using namespace std::chrono_literals;

int main() {
    std::atomic<bool> completed{true};
    if (!fcitx::WaitForWorkerCompletion(completed, 1ms)) return 1;

    completed = false;
    if (fcitx::WaitForWorkerCompletion(completed, 1ms)) return 1;

    std::thread worker([&completed]() {
        std::this_thread::sleep_for(2ms);
        completed.store(true, std::memory_order_release);
    });
    const bool observedCompletion =
        fcitx::WaitForWorkerCompletion(completed, 50ms);
    worker.join();
    if (!observedCompletion) return 1;
}
