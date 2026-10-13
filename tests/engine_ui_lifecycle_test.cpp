#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <fcitx-utils/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx/text.h>

#include "engine.h"

namespace {

// One fixture per process. Only the public Pipeline boundary is substituted;
// the engine, result queue, InputContext and dispatcher remain real.
struct PipelineBoundary {
    fcitx::Pipeline* pipeline = nullptr;
    fcitx::Pipeline::ResultCallback resultCallback;
    fcitx::VADWorker::VadStatusCallback vadCallback;
    int starts = 0;
    int stops = 0;
    int pauses = 0;
    bool capturing = false;
    bool captureHealthy = true;
    int captureOpens = 0;

    void EmitResult(uint64_t generation) {
        fcitx::AsrResult result;
        result.text = "fixture transcript";
        result.generation = generation;
        result.utteranceId = 1;
        pipeline->ResultQueue().Push(result);
        resultCallback(result.text);
    }
} boundary;

class FixtureInputContext final : public fcitx::InputContext {
public:
    explicit FixtureInputContext(fcitx::InputContextManager& manager)
        : InputContext(manager, "ui-lifecycle-test") {}
    ~FixtureInputContext() override { destroy(); }
    const char* frontend() const override { return "ui-lifecycle-test"; }
    std::vector<std::string> commits;

protected:
    void commitStringImpl(const std::string& text) override { commits.push_back(text); }
    void deleteSurroundingTextImpl(int, unsigned int) override {}
    void forwardKeyImpl(const fcitx::ForwardKeyEvent&) override {}
    void updatePreeditImpl() override {}
};

} // namespace

void fcitx::Pipeline::Start() {
    boundary.pipeline = this;
    ++boundary.starts;
    if (!boundary.capturing) ++boundary.captureOpens;
    boundary.capturing = true;
    running_ = true;
}
void fcitx::Pipeline::Pause() {
    ++boundary.pauses;
    running_ = false;
}
void fcitx::Pipeline::Stop() {
    ++boundary.stops;
    boundary.capturing = false;
    running_ = false;
}
bool fcitx::Pipeline::IsCapturing() const {
    return boundary.capturing && boundary.captureHealthy;
}
void fcitx::Pipeline::SetResultCallback(ResultCallback callback) {
    boundary.resultCallback = std::move(callback);
}
void fcitx::Pipeline::SetVadStatusCallback(VADWorker::VadStatusCallback callback) {
    boundary.vadCallback = std::move(callback);
}

int main(int argc, char** argv) {
    const std::string scenario = argc == 2 ? argv[1] : "destroy";
    char program[] = "engine-ui-lifecycle-test";
    char disabled[] = "--disable=all";
    char* arguments[] = {program, disabled, nullptr};
    fcitx::Instance instance(2, arguments);
    instance.initialize();
    auto& manager = instance.inputContextManager();
    auto context = std::make_unique<FixtureInputContext>(manager);
    std::unique_ptr<FixtureInputContext> nextContext;
    fcitx::VoiceInputEngine engine(&instance);
    if (scenario == "pending-switch") {
        fcitx::RawConfig config;
        config.setValueByPath("AutoCommit", "False");
        engine.setSubConfig("asr/openai", config);
    }
    fcitx::InputMethodEntry entry("voiceinput", "Voice Input", "zh_CN", "voiceinput");
    fcitx::InputMethodActivatedEvent activated("voiceinput", context.get());
    engine.activate(entry, activated);
    context->inputPanel().setPreedit(fcitx::Text(std::string(256, 'x')));

    auto actionTimer = instance.eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 5000, 1,
        [&](fcitx::EventSourceTime*, uint64_t) {
            if (scenario == "pending-switch") {
                boundary.EmitResult(1);
                return false;
            }
            if (scenario == "active-destroy" || scenario == "reactivate") {
                // Exercise the real cross-thread callbacks before the context
                // is destroyed or replaced, without reaching into the engine.
                std::thread worker([] {
                    boundary.EmitResult(1);
                    boundary.vadCallback(true);
                });
                worker.join();
            }
            if (scenario == "active-destroy") {
                context.reset();
                return false;
            }
            if (scenario.starts_with("focus-")) {
                fcitx::FocusOutEvent focusOut(context.get());
                engine.deactivate(entry, focusOut);
            } else {
                fcitx::InputMethodDeactivatedEvent deactivated("voiceinput", context.get());
                engine.deactivate(entry, deactivated);
            }
            if (scenario == "destroy") context.reset();
            if (scenario == "reactivate") {
                nextContext = std::make_unique<FixtureInputContext>(manager);
                fcitx::InputMethodActivatedEvent nextActivated("voiceinput", nextContext.get());
                engine.activate(entry, nextActivated);
                nextContext->inputPanel().setPreedit(fcitx::Text("new context"));
            }
            return false;
        });
    bool pendingWasDisplayed = false;
    auto switchTimer = instance.eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 15000, 1,
        [&](fcitx::EventSourceTime*, uint64_t) {
            if (scenario == "focus-out") {
                boundary.EmitResult(1);
                boundary.vadCallback(true);
                return false;
            }
            if (scenario != "pending-switch") return false;
            pendingWasDisplayed = context->inputPanel().preedit().toString() == "fixture transcript";
            fcitx::InputMethodDeactivatedEvent deactivated("voiceinput", context.get());
            engine.deactivate(entry, deactivated);
            nextContext = std::make_unique<FixtureInputContext>(manager);
            fcitx::InputMethodActivatedEvent nextActivated("voiceinput", nextContext.get());
            engine.activate(entry, nextActivated);
            fcitx::KeyEvent key(nextContext.get(), fcitx::Key("a"));
            engine.keyEvent(entry, key);
            return false;
        });
    auto focusTimer = instance.eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 250000, 1,
        [&](fcitx::EventSourceTime*, uint64_t) {
            if (scenario == "focus-resume") {
                fcitx::FocusInEvent focusIn(context.get());
                engine.activate(entry, focusIn);
                boundary.EmitResult(1);
                boundary.EmitResult(3);
            } else if (scenario == "focus-close" || scenario == "focus-dead-close") {
                if (scenario == "focus-dead-close") boundary.captureHealthy = false;
                fcitx::InputContextSwitchInputMethodEvent switched(
                    fcitx::InputMethodSwitchedReason::Enumerate, "voiceinput", context.get());
                // Unfocused switches skip the engine callback in Fcitx core.
                instance.postEvent(switched);
            } else if (scenario == "focus-unrelated-switch" ||
                       scenario == "focus-other-voice-switch" ||
                       scenario == "focus-other-voice-deactivate") {
                nextContext = std::make_unique<FixtureInputContext>(manager);
                fcitx::InputContextSwitchInputMethodEvent switched(
                    fcitx::InputMethodSwitchedReason::Enumerate,
                    scenario == "focus-unrelated-switch" ? "keyboard-us" : "voiceinput",
                    nextContext.get());
                if (scenario == "focus-other-voice-deactivate") {
                    engine.deactivate(entry, switched);
                } else {
                    instance.postEvent(switched);
                }
            }
            return false;
        });
    auto exitTimer = instance.eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) +
            (scenario.starts_with("focus-") ? 350000 : 50000), 1,
        [&](fcitx::EventSourceTime*, uint64_t) {
            instance.eventLoop().exit();
            return false;
        });
    instance.eventLoop().exec();
    if (scenario == "focus-out" &&
        (boundary.starts != 1 || boundary.stops != 0 || boundary.pauses != 1 ||
         !boundary.capturing)) {
        std::cerr << "FAIL: focus loss released the capture stream\n";
        return 1;
    }
    if (scenario == "focus-out" &&
        (!context->commits.empty() || !context->inputPanel().preedit().empty())) {
        std::cerr << "FAIL: a late recognition callback reached the unfocused context\n";
        return 1;
    }
    if (scenario == "focus-resume" &&
        (boundary.captureOpens != 1 || boundary.starts != 2 ||
         context->commits != std::vector<std::string>{"fixture transcript"})) {
        std::cerr << "FAIL: focus resume reopened capture or accepted an old generation\n";
        return 1;
    }
    if ((scenario == "focus-close" || scenario == "focus-dead-close") &&
        (boundary.capturing || boundary.stops != 1)) {
        std::cerr << "FAIL: an unfocused input-method switch did not release capture\n";
        return 1;
    }
    if ((scenario == "focus-unrelated-switch" || scenario == "focus-other-voice-switch" ||
         scenario == "focus-other-voice-deactivate") &&
        (!boundary.capturing || boundary.stops != 0)) {
        std::cerr << "FAIL: another context's switch closed the retained stream\n";
        return 1;
    }
    if (scenario == "active-destroy" &&
        (!boundary.capturing || boundary.pauses != 1 || boundary.pipeline->IsRunning())) {
        std::cerr << "FAIL: destroying the active context did not pause recognition while retaining capture\n";
        return 1;
    }
    if (scenario == "live" && !context->inputPanel().preedit().empty()) {
        std::cerr << "FAIL: live input panel was not cleared\n";
        return 1;
    }
    if (scenario == "reactivate" &&
        (nextContext->inputPanel().preedit().toString() != "new context" ||
         !nextContext->commits.empty())) {
        std::cerr << "FAIL: stale UI callback modified the new input context\n";
        return 1;
    }
    if (scenario == "pending-switch" &&
        (!pendingWasDisplayed || context->commits != std::vector<std::string>{"fixture transcript"} ||
         !nextContext->commits.empty())) {
        std::cerr << "FAIL: pending transcript was not committed only to its original context\n";
        return 1;
    }
    std::cout << "PASS: UI dispatch respects input context lifetime\n";
    return 0;
}
