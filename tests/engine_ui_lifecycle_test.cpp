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

void fcitx::Pipeline::Start() { boundary.pipeline = this; }
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
    fcitx::InputContextManager manager;
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
            fcitx::InputMethodDeactivatedEvent deactivated("voiceinput", context.get());
            engine.deactivate(entry, deactivated);
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
    auto exitTimer = instance.eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 50000, 1,
        [&](fcitx::EventSourceTime*, uint64_t) {
            instance.eventLoop().exit();
            return false;
        });
    instance.eventLoop().exec();
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
