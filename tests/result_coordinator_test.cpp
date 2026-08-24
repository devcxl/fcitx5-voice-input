#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <string>

#include "pipeline/result_coordinator.h"

namespace {

using namespace std::chrono_literals;
using fcitx::AsrResult;
using fcitx::ResultCoordinator;

bool Check(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

void Register(ResultCoordinator& coordinator, uint64_t id) {
    coordinator.RegisterSession(id, {1, id});
}

bool TestMoreThan128TerminalResultsAreRetained() {
    auto coordinator = std::make_shared<ResultCoordinator>();
    coordinator->Start(1, 1);
    for (uint64_t id = 1; id <= 130; ++id) Register(*coordinator, id);

    for (uint64_t id = 130; id > 0; --id) {
        coordinator->HandleAsrResult(std::to_string(id), true, id);
    }

    if (!Check(coordinator->ResultQueue().Size() == 130,
               "terminal results must not be dropped at the old queue limit")) {
        return false;
    }

    AsrResult result;
    for (uint64_t id = 1; id <= 130; ++id) {
        if (!Check(coordinator->ResultQueue().TryPop(result),
                   "every terminal result must remain available") ||
            !Check(result.utteranceId == id,
                   "terminal results must retain utterance order")) {
            return false;
        }
    }
    return true;
}

bool TestSkippedSessionRejectsLateCallback() {
    auto coordinator = std::make_shared<ResultCoordinator>();
    coordinator->Start(1, 1);
    Register(*coordinator, 1);
    Register(*coordinator, 2);

    coordinator->HandleAsrResult("B partial", false, 2);
    coordinator->SkipSession(2);
    coordinator->HandleAsrResult("B late", true, 2);
    coordinator->HandleAsrResult("A", true, 1);

    AsrResult result;
    return Check(coordinator->ResultQueue().TryPop(result),
                 "A terminal result must be delivered") &&
           Check(result.text == "A", "only A may be delivered") &&
           Check(!coordinator->ResultQueue().TryPop(result),
                 "skipped B callbacks must be discarded");
}

bool TestLlmWithoutCompletionFallsBackToTerminalRawText() {
    auto coordinator = std::make_shared<ResultCoordinator>();
    coordinator->SetLLMClient(
        std::make_shared<fcitx::LLMClient>(fcitx::LLMClient::Config{}));
    coordinator->Start(1, 1);
    Register(*coordinator, 1);
    Register(*coordinator, 2);

    coordinator->HandleAsrResult("A", true, 1);
    coordinator->HandleAsrResult("B", true, 2);

    if (!Check(coordinator->ResultQueue().Size() == 4,
               "missing LLM completion must fall back and unblock B")) {
        return false;
    }
    AsrResult result;
    const uint64_t expectedIds[] = {1, 1, 2, 2};
    for (uint64_t expectedId : expectedIds) {
        if (!Check(coordinator->ResultQueue().TryPop(result),
                   "raw and refined fallback results must be available") ||
            !Check(result.utteranceId == expectedId,
                   "fallback results must preserve utterance order")) {
            return false;
        }
    }
    return true;
}

bool TestCloseWaitsForEnteredNotification() {
    auto coordinator = std::make_shared<ResultCoordinator>();
    coordinator->Start(1, 1);
    Register(*coordinator, 1);

    std::promise<void> notificationEntered;
    std::promise<void> releaseNotification;
    auto release = releaseNotification.get_future().share();
    std::atomic<int> notifications{0};
    coordinator->SetResultCallback([&](const std::string&) {
        ++notifications;
        notificationEntered.set_value();
        release.wait();
    });

    auto publisher = std::async(std::launch::async, [coordinator] {
        coordinator->HandleAsrResult("A", true, 1);
    });
    notificationEntered.get_future().wait();

    auto closer = std::async(std::launch::async, [coordinator] {
        coordinator->Close(2);
    });
    const bool closeBlocked =
        closer.wait_for(20ms) == std::future_status::timeout;
    releaseNotification.set_value();

    const bool publisherFinished =
        publisher.wait_for(1s) == std::future_status::ready;
    const bool closeFinished =
        closer.wait_for(1s) == std::future_status::ready;
    Register(*coordinator, 2);
    coordinator->HandleAsrResult("late", true, 2);

    return Check(closeBlocked,
                 "Close must wait for an entered notification callback") &&
           Check(publisherFinished,
                 "notification callback must finish") &&
           Check(closeFinished,
                 "Close must finish after the callback exits") &&
           Check(notifications.load() == 1,
                 "callbacks after Close returns must be rejected");
}

} // namespace

int main() {
    return TestMoreThan128TerminalResultsAreRetained() &&
                   TestSkippedSessionRejectsLateCallback() &&
                   TestLlmWithoutCompletionFallsBackToTerminalRawText() &&
                   TestCloseWaitsForEnteredNotification()
               ? 0
               : 1;
}
