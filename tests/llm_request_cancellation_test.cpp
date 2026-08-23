#include "llm/llm_request_cancellation.h"

int main() {
    fcitx::LLMRequestCancellation cancellation;

    const auto firstRequest = cancellation.BeginRequest();
    if (cancellation.IsCancelled(firstRequest)) return 1;

    cancellation.Cancel();
    if (!cancellation.IsCancelled(firstRequest)) return 1;

    const auto nextRequest = cancellation.BeginRequest();
    if (cancellation.IsCancelled(nextRequest)) return 1;

    cancellation.Cancel();
    if (!cancellation.IsCancelled(nextRequest)) return 1;
}
