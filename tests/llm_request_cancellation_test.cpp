#include <cassert>

#include "llm/llm_request_cancellation.h"

int main() {
    fcitx::LLMRequestCancellation cancellation;

    const auto firstRequest = cancellation.BeginRequest();
    assert(!cancellation.IsCancelled(firstRequest));

    cancellation.Cancel();
    assert(cancellation.IsCancelled(firstRequest));

    const auto nextRequest = cancellation.BeginRequest();
    assert(!cancellation.IsCancelled(nextRequest));

    cancellation.Cancel();
    assert(cancellation.IsCancelled(nextRequest));
}
