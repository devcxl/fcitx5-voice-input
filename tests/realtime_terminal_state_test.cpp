#include <iostream>

#include "asr/realtime_asr.h"

namespace {

bool Check(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

bool TestTransportFailureIsNotFinal() {
    using fcitx::DecideRealtimeServerOutcome;
    using fcitx::RealtimeServerOutcome;

    const auto beforeEnd = DecideRealtimeServerOutcome(
        RealtimeServerOutcome::TransportFailure, false);
    const auto afterEnd = DecideRealtimeServerOutcome(
        RealtimeServerOutcome::TransportFailure, true);
    return Check(beforeEnd.reconnect && !beforeEnd.publishFallback,
                 "transport failure before End must reconnect") &&
           Check(afterEnd.stop && afterEnd.publishFallback,
                 "transport failure after End must publish a fallback final");
}

bool TestFinalOutcomeStopsWithoutFallback() {
    const auto decision = fcitx::DecideRealtimeServerOutcome(
        fcitx::RealtimeServerOutcome::FinalReceived, true);
    return Check(decision.stop, "a real final must stop receiving") &&
           Check(!decision.reconnect, "a real final must not reconnect") &&
           Check(!decision.publishFallback,
                 "a real final must not emit a duplicate fallback");
}

bool TestTerminalCanOnlyBePublishedOnce() {
    fcitx::RealtimeTerminalState terminal;
    return Check(terminal.TryMarkPublished(),
                 "the first terminal publication must be accepted") &&
           Check(!terminal.TryMarkPublished(),
                 "a duplicate terminal publication must be rejected") &&
           Check(terminal.IsPublished(), "terminal state must remain published");
}

} // namespace

int main() {
    return TestTransportFailureIsNotFinal() &&
                   TestFinalOutcomeStopsWithoutFallback() &&
                   TestTerminalCanOnlyBePublishedOnce()
               ? 0
               : 1;
}
