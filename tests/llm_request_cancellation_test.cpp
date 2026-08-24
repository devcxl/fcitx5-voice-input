#include <atomic>
#include <chrono>
#include <future>
#include <iostream>

#include "llm/llm_client.h"
#include "llm/llm_request_cancellation.h"

namespace {

using namespace std::chrono_literals;

bool Check(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

bool TestGenerationIsolation() {
    fcitx::LLMRequestCancellation cancellation;
    cancellation.Activate(1);

    if (!Check(!cancellation.IsCancelled(1), "active generation must run")) {
        return false;
    }

    cancellation.Cancel();
    cancellation.Activate(2);
    bool oldGenerationPublished = false;
    bool newGenerationPublished = false;
    return Check(cancellation.IsCancelled(1),
                 "late request from the old generation must stay cancelled") &&
           Check(!cancellation.IsCancelled(2),
                 "new generation must run after cancellation") &&
           Check(!cancellation.PublishIfCurrent(
                     1, [&] { oldGenerationPublished = true; }),
                 "late old-generation publication must be rejected") &&
           Check(!oldGenerationPublished,
                 "rejected old-generation callback must not run") &&
           Check(cancellation.PublishIfCurrent(
                     2, [&] { newGenerationPublished = true; }),
                 "new-generation publication must be accepted") &&
           Check(newGenerationPublished,
                 "accepted new-generation callback must run");
}

bool TestCancelWaitsForPublication() {
    fcitx::LLMRequestCancellation cancellation;
    cancellation.Activate(1);

    std::promise<void> publicationEntered;
    std::promise<void> releasePublication;
    auto release = releasePublication.get_future().share();
    std::atomic<bool> published{false};
    auto publisher = std::async(std::launch::async, [&] {
        return cancellation.PublishIfCurrent(1, [&] {
            publicationEntered.set_value();
            release.wait();
            published = true;
        });
    });
    publicationEntered.get_future().wait();

    std::promise<void> cancelAttempted;
    auto canceller = std::async(std::launch::async, [&] {
        cancelAttempted.set_value();
        cancellation.Cancel();
    });
    cancelAttempted.get_future().wait();
    const bool cancelBlocked =
        canceller.wait_for(20ms) == std::future_status::timeout;
    releasePublication.set_value();

    return Check(cancelBlocked, "Cancel must wait for an active publication") &&
           Check(publisher.get(), "current publication must be accepted") &&
           Check(published.load(), "accepted publication must complete") &&
           Check(canceller.wait_for(1s) == std::future_status::ready,
                 "Cancel must finish after publication leaves the gate") &&
           Check(!cancellation.PublishIfCurrent(1, [] {}),
                 "publication after Cancel returns must be rejected");
}

bool TestClientRejectsLateGeneration() {
    fcitx::LLMClient client({});
    std::string result;

    client.Activate(1);
    client.Process("first", 1,
                   [&result](const std::string& text) { result = text; });
    if (!Check(result == "first", "active client request must publish")) {
        return false;
    }

    client.Cancel();
    client.Activate(2);
    result.clear();
    client.Process("stale", 1,
                   [&result](const std::string& text) { result = text; });
    client.Process("next", 2,
                   [&result](const std::string& text) { result = text; });
    return Check(result == "next",
                 "client must reject stale requests and publish the new session");
}

} // namespace

int main() {
    return TestGenerationIsolation() && TestCancelWaitsForPublication() &&
                   TestClientRejectsLateGeneration()
               ? 0
               : 1;
}
